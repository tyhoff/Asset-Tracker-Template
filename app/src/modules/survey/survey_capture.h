/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SURVEY_CAPTURE_H_
#define _SURVEY_CAPTURE_H_

/**
 * @file survey_capture.h
 * @brief One capture cycle: GNSS fix, radio scan, GNSS fix.
 *
 * FW-2 requires the three steps to be separate, sequenced location requests. The Location
 * library stops at the first successful method, so a single request that lists GNSS and
 * the radios returns a fix and never scans. FW-4 requires the two GNSS fixes to bracket
 * the scan, so that host-side analysis can interpolate ground truth to the scan midpoint
 * instead of extrapolating from one fix taken who-knows-how-long before.
 *
 * The modem cannot run GNSS and LTE at once -- one RF front end -- so the sequencing is a
 * hardware requirement, not a stylistic one. It is enforced structurally: one thread runs
 * the cycle top to bottom, and each step waits for the Location library to report the
 * previous request finished before triggering the next. A trigger published while a search
 * is active is silently discarded by the location module, so "wait for done" is what makes
 * the sequence real rather than hopeful.
 *
 * Nothing here cancels a request. LOCATION_SEARCH_CANCEL cannot truly cancel a Wi-Fi scan
 * and leaves the next request returning -EBUSY; scan-only requests wind down through the
 * library's own state machine instead (see location.c). A cycle that has to give up waits
 * for the request to finish on its own rather than tearing it down.
 */

#include <stdbool.h>
#include <stdint.h>

/* For MSEC_PER_SEC, used by SURVEY_CAPTURE_WIND_DOWN_BUDGET_MS below. Included rather than
 * relied upon: every current includer happens to pull in zephyr/kernel.h first.
 */
#include <zephyr/sys/util.h>

#include "location.h"
#include "survey_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Longest a cycle can take before every step has given up.
 *
 * Each of the three steps can spend its own timeout waiting for a result and then the idle
 * timeout waiting for the search to wind down; the wind-down is part of the bound, not a
 * rounding error. Anything that has to decide "this device has stopped responding" -- the
 * shell's estimate, the hardware harness's patience -- must use this rather than summing
 * the step timeouts, because understating it turns a device that is merely indoors into a
 * reported failure.
 */
#define SURVEY_CAPTURE_WORST_CASE_SECONDS					\
	(2 * (CONFIG_APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS +			\
	      CONFIG_APP_SURVEY_CAPTURE_IDLE_TIMEOUT_SECONDS) +			\
	 CONFIG_APP_SURVEY_CAPTURE_SCAN_TIMEOUT_SECONDS +			\
	 CONFIG_APP_SURVEY_CAPTURE_IDLE_TIMEOUT_SECONDS)

/** @brief Time a cycle can spend inside itself without any step measuring it.
 *
 * The four reported durations (three steps plus the store) are disjoint intervals, so their
 * sum is always at most the total -- but the total also contains, per step, the wait for
 * the search to wind down and the two short waits around publishing the trigger and seeing
 * LOCATION_SEARCH_STARTED. That unmeasured remainder is what a cross-check has to allow,
 * and it has to come from the same Kconfig symbols the firmware uses. Exported for the
 * hardware harness, which previously reproduced this arithmetic in Python and would have
 * gone quietly wrong the first time a timeout changed.
 */
#define SURVEY_CAPTURE_WIND_DOWN_BUDGET_MS					\
	(3 * (CONFIG_APP_SURVEY_CAPTURE_IDLE_TIMEOUT_SECONDS + 2) * MSEC_PER_SEC)

/** @brief What one cycle measured, in milliseconds.
 *
 * Reported by "survey timing" so that the cadence in FW-7 can be set from measurement
 * rather than from the estimates in FW-2. Durations are wall-clock from the orchestrator's
 * point of view: they include the Location library's own scheduling, which is what a
 * cadence has to accommodate.
 *
 * Measured on the uptime clock, not on survey_time_now(). The scan step is when date_time
 * is most likely to sync, and a duration differenced across the resulting uptime-to-Unix
 * switch would read as about 1.7e12 ms. Offsets in the *record* still use survey_time_now(),
 * because they have to share a base with the GNSS timestamps they are relative to.
 */
struct survey_capture_timing {
	/** False until a cycle has completed. */
	bool valid;
	/** Sequence number of the cycle these numbers came from. */
	uint32_t sequence;

	/** Trigger of the first GNSS request to its fix being published. */
	int32_t gnss_before_ms;
	/** Trigger of the scan to the cloud request arriving. */
	int32_t scan_ms;
	/** Trigger of the second GNSS request to its fix being published. */
	int32_t gnss_after_ms;
	/** Handing the record to storage.
	 *
	 * Separate from the three steps because it is not radio work and does not scale
	 * like radio work: on target a single store has been measured at tens of seconds
	 * once the partition holds thousands of records. Charged to a step, it would make
	 * the radio timings appear to degrade as the flash fills.
	 */
	int32_t store_ms;
	/** Whole cycle, trigger of step 1 to record handed to storage.
	 *
	 * Larger than the four numbers above add up to: the gaps are the waits for each
	 * search to report done, which are real elapsed time but belong to no step.
	 */
	int32_t total_ms;

	/** Which steps produced data. A cycle can be worth storing with any of these
	 *  false except @c scan_ok -- the radio observations are the measurement; the
	 *  fixes are the ground truth they are scored against.
	 */
	bool gnss_before_ok;
	bool scan_ok;
	bool gnss_after_ok;
};

/** @brief Fold a location module message into the capture state machine.
 *
 * Called from the zbus listener in survey.c, which runs in the publisher's context with
 * the channel mutex held. This function only posts to a @c k_event and never blocks.
 *
 * Not declared at all when CONFIG_APP_SURVEY_CAPTURE is disabled: the call sites in
 * survey.c use IF_ENABLED, which elides the call rather than calling an empty function.
 * Reach for IS_ENABLED here and it will not link.
 */
void survey_capture_notify(const struct location_msg *msg);

/** @brief Run one capture cycle.
 *
 * Asynchronous: this queues the cycle and returns. The cycle itself takes as long as GNSS
 * does, which can be minutes, so it cannot run on a shell or zbus thread.
 *
 * @param profile Profile to record with the cycle. Gating between them is CP7; today the
 *                caller chooses.
 *
 * @retval 0        Cycle queued.
 * @retval -EALREADY A cycle is already running. Cycles are never overlapped: the second
 *                   one's triggers would be discarded and its timestamps would describe
 *                   the first one's radio work.
 */
int survey_capture_request(enum survey_profile profile);

/** @brief True while a cycle is running, from admission until the record is stored.
 *
 * The question a caller that wants to start something asks. Use this to refuse work that
 * would collide with the cycle as a whole -- another cycle, or a manual store whose
 * record would interleave with the cycle's.
 */
bool survey_capture_busy(void);

/** @brief True while a cycle still has radio work to do.
 *
 * A strict subset of survey_capture_busy(): it goes false as soon as the third step ends,
 * whereas the cycle stays busy through survey_store_publish_record(), which has been
 * measured at tens of seconds once the partition fills.
 *
 * The question a caller that wants to know whether the *radio* is spoken for asks -- which
 * is a different question from whether a cycle is running, and answering it with
 * survey_capture_busy() strands main.c: see the trigger-suppression branch in location.c.
 */
bool survey_capture_radio_busy(void);

/** @brief Copy the timing of the most recent completed cycle. */
void survey_capture_timing_get(struct survey_capture_timing *out);

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_CAPTURE_H_ */
