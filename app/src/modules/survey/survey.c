/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* zbus glue for the radio survey: observes location_chan and folds every GNSS fix and
 * cell/Wi-Fi scan into the observation cache in survey_obs.c.
 *
 * A listener rather than a message subscriber, following the same choice the led module
 * makes (ZBUS_LISTENER_DEFINE in led.c): the work here is one struct copy, so it does not
 * warrant a thread, a stack and a task watchdog channel of its own.
 *
 * The capture orchestrator (survey_capture.c) does add a thread, and it is registered with
 * the task watchdog like every other thread in this application. That was not true when it
 * was first written, and the consequence was specific: the record store publishes with
 * K_FOREVER, so a wedged storage thread would block the capture thread forever, and the
 * admission semaphore it holds would never be returned -- surveying would stop permanently
 * with nothing but "a capture is already running" to show for it.
 *
 * The callback runs in the *publisher's* context with the channel mutex held, so it must
 * never block. Do not add anything here that can sleep, take a long lock, or do
 * synchronous I/O.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <date_time.h>

#include "location.h"
#include "survey.h"
#include "survey_obs.h"
#if defined(CONFIG_APP_SURVEY_CAPTURE)
#include "survey_capture.h"
#endif

LOG_MODULE_REGISTER(survey, CONFIG_APP_SURVEY_LOG_LEVEL);

void survey_time_now(int64_t *now_ms, enum survey_time_base *time_base)
{
	/* The location module stamps GNSS messages with Unix time when the clock has been
	 * synchronised and silently falls back to uptime when it has not. Which of the two
	 * it used is not recorded in the message, so establish it here and store it
	 * alongside: a record stamped with uptime cannot be correlated with anything
	 * off-device, and must not be mistaken for one that can.
	 */
	if (date_time_now(now_ms) == 0) {
		*time_base = SURVEY_TIME_BASE_UNIX;
		return;
	}

	*now_ms = k_uptime_get();
	*time_base = SURVEY_TIME_BASE_UPTIME;
}

static void survey_location_cb(const struct zbus_channel *chan)
{
	const struct location_msg *msg = zbus_chan_const_msg(chan);
	enum survey_time_base time_base;
	int64_t now_ms;
	struct survey_obs_scan_counts counts;

	if (msg->type != LOCATION_GNSS_DATA && msg->type != LOCATION_CLOUD_REQUEST) {
		/* Lifecycle events (started, done) carry no data, so the orchestrator can be
		 * woken immediately. It sequences the cycle on these -- a step that misses its
		 * wake-up stalls until its timeout. Only a k_event post, safe in this context.
		 */
		IF_ENABLED(CONFIG_APP_SURVEY_CAPTURE, (survey_capture_notify(msg);));

		return;
	}

	survey_time_now(&now_ms, &time_base);
	counts = survey_obs_update(msg, now_ms, time_base);

	/* Result events: notify *after* the cache is written, never before.
	 *
	 * The orchestrator's response to this wake-up is survey_obs_snapshot(), so waking it
	 * first is a race on the data it is about to read. It cannot bite today -- the capture
	 * thread is K_LOWEST_APPLICATION_THREAD_PRIO and cannot preempt the publisher on a
	 * uniprocessor -- but the consequence if it ever did is not a crash: step 3 would
	 * snapshot step 1's fix, still flagged valid, and store a record whose trailing
	 * bracket is a duplicate of the leading one. Well-formed and silently wrong, which is
	 * the exact failure the bracketing exists to prevent. Ordering it correctly costs
	 * nothing, so do not rely on the priority.
	 */
	IF_ENABLED(CONFIG_APP_SURVEY_CAPTURE, (survey_capture_notify(msg);));

	/* LOG_INF, not LOG_DBG: this is the confirmation an operator is told to look for
	 * after running "survey scan", and the module log level defaults to INF.
	 */
	if (msg->type == LOCATION_GNSS_DATA) {
		LOG_INF("GNSS fix cached (%s time base). Run \"survey show\".",
			time_base == SURVEY_TIME_BASE_UNIX ? "unix" : "uptime");
	} else {
		/* The AP count is counts.kept, which is what was *cached*, not the count the
		 * modem reported: locally-administered BSSIDs are dropped on the way in, and a
		 * log line that disagreed with "survey show" would send someone looking for a
		 * bug in the storage path. Taking it from the return value rather than
		 * subtracting from msg->cloud_request.wifi_cnt also means no arithmetic here can
		 * disagree with what was stored -- survey_obs_update() bounds the count to the
		 * destination array, so the message's own wifi_cnt is not always the right
		 * starting point.
		 *
		 * IS_ENABLED rather than #if: both branches then have to type-check in both
		 * configurations, which is the only thing that would have caught a build break
		 * in the branch no build in the verification set compiles. The dead one is
		 * optimised out.
		 *
		 * The parenthetical is dropped rather than printed as a zero when the filter is
		 * off. Someone who set DROP_LOCAL_MAC=n did it to see the unfiltered air, and
		 * telling them "0 randomised BSSID(s) dropped" on every scan reads as a filter
		 * that is running and finding nothing. The formatter suppresses its line for the
		 * same reason.
		 */
		if (IS_ENABLED(CONFIG_APP_SURVEY_DROP_LOCAL_MAC)) {
			LOG_INF("Scan cached: %u neighbor(s), %u gci, %u AP(s) "
				"(%u randomised BSSID(s) dropped). Run \"survey show\".",
				msg->cloud_request.ncells_count,
				msg->cloud_request.gci_cells_count, counts.kept, counts.dropped);
		} else {
			LOG_INF("Scan cached: %u neighbor(s), %u gci, %u AP(s). "
				"Run \"survey show\".",
				msg->cloud_request.ncells_count,
				msg->cloud_request.gci_cells_count, counts.kept);
		}
	}
}

ZBUS_LISTENER_DEFINE(survey_listener, survey_location_cb);
ZBUS_CHAN_ADD_OBS(location_chan, survey_listener, 0);
