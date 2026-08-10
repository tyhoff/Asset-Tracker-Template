/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _LOCATION_TRIGGER_STATS_H_
#define _LOCATION_TRIGGER_STATS_H_

/**
 * @file location_trigger_stats.h
 * @brief How many location triggers the module accepted, and how many it discarded.
 *
 * This exists to make a trigger testable from outside the device.
 *
 * Every observable consequence of a location request -- the cached fix, the cached scan,
 * the "GNSS fix cached" log line, the observation counters -- is produced by the *result*
 * arriving, and results are indistinguishable by origin. The application takes a sample at
 * startup and on a timer, so a test that sends "survey scan" and then sees a scan appear
 * has not shown its own trigger did anything: a background search finishing inside the same
 * window looks exactly the same. Deleting the trigger from the firmware would not fail such
 * a test.
 *
 * The absence of the "trigger received while a search is active, ignoring" warning does not
 * close the gap either -- that warning is also absent when no trigger ever arrived. What is
 * needed is a positive token emitted where the module *accepts* a request, and this counter
 * is it. A test that requires @c accepted to advance fails if its trigger never reaches the
 * module, no matter what the radios happen to be doing.
 *
 * Declared in its own header rather than in location.h so that the upstream header stays
 * untouched; location.c has already diverged.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Trigger dispositions since boot. */
struct location_trigger_stats {
	/** Triggers that started a location request. */
	uint32_t accepted;
	/** Triggers discarded because a search was already running.
	 *
	 * Not an error: a request can hold the module for minutes, and callers are expected
	 * to retry. Counting them makes a starved caller visible instead of silent.
	 */
	uint32_t dropped;
	/** Default LOCATION_SEARCH_TRIGGERs ignored because the survey owns the search.
	 *
	 * Separate from @c dropped because the two dispositions are different facts and a
	 * test needs to tell them apart: @c dropped means a caller lost a race it may win on
	 * retry, @c suppressed means APP_SURVEY_CAPTURE_OWNS_SEARCH deliberately silenced the
	 * application's own sampling timer. Folding them together would make a starved survey
	 * step indistinguishable from the suppression working as designed.
	 */
	uint32_t suppressed;
};

/** @brief Copy the current counts.
 *
 * Safe from any thread. The fields are read separately, so a concurrent trigger can make
 * them momentarily inconsistent with each other; each is individually monotonic, which is
 * all a before/after comparison needs.
 */
void location_trigger_stats_get(struct location_trigger_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* _LOCATION_TRIGGER_STATS_H_ */
