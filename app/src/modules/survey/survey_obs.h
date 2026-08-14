/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SURVEY_OBS_H_
#define _SURVEY_OBS_H_

/**
 * @file survey_obs.h
 * @brief Cache and human-readable rendering of the most recent survey observation.
 *
 * This unit is deliberately free of zbus, the shell, and the modem so that it can be
 * unit tested on native_sim with no fakes. survey.c provides the zbus glue and
 * survey_shell.c the console commands; both are thin wrappers over this API.
 *
 * At this checkpoint the cache holds the *latest* GNSS fix and the *latest* cell/Wi-Fi
 * scan independently. They are not yet a matched pair -- pairing, bracketing GNSS fixes
 * and the six-timestamp record come later. The purpose here is observability: making
 * visible exactly what the existing location pipeline already hands the application.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#include "location.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which clock a cached timestamp was taken against.
 *
 * The location module falls back to uptime when the system clock has never been
 * synchronised (see @c location_msg.timestamp). Records taken against uptime cannot be
 * correlated with anything off-device, so the time base must travel with the data
 * rather than being assumed.
 */
enum survey_time_base {
	/** No timestamp has been recorded yet. */
	SURVEY_TIME_BASE_NONE = 0,
	/** Milliseconds since boot. Not correlatable off-device. */
	SURVEY_TIME_BASE_UPTIME,
	/** Unix time in milliseconds. */
	SURVEY_TIME_BASE_UNIX,
};

/** @brief Latest observed GNSS fix and radio scan. */
struct survey_observation {
	/** Total GNSS fixes seen since boot (not the number cached). */
	uint32_t gnss_count;
	/** Total cell/Wi-Fi scans seen since boot. */
	uint32_t scan_count;

	/** True when the cache holds injected test data rather than a real observation.
	 *
	 * Set by @c survey_obs_mark_synthetic and cleared by the next real observation.
	 * Rendered as a banner so that selftest output can never be mistaken for a
	 * measurement.
	 */
	bool synthetic;

	/** True when @c gnss holds a fix. */
	bool gnss_valid;
	/** Timestamp of the cached fix, in the base given by @c gnss_time_base. */
	int64_t gnss_timestamp;
	enum survey_time_base gnss_time_base;
	struct location_data gnss;

	/** True when @c scan holds cell and/or Wi-Fi observations. */
	bool scan_valid;
	/** Timestamp of receipt of the cached scan, in @c scan_time_base.
	 *
	 * The location module does not stamp LOCATION_CLOUD_REQUEST messages, so this is
	 * when the application observed the scan, not when the modem measured it. Real
	 * measurement start/end timestamps arrive with the capture orchestrator.
	 */
	int64_t scan_timestamp;
	enum survey_time_base scan_time_base;
	struct location_cloud_request_data scan;

	/** Access points dropped from @c scan for having a locally-administered BSSID.
	 *
	 * Reported so that a scan with few access points can be told apart from a filter
	 * that is discarding too much. Zero when CONFIG_APP_SURVEY_DROP_LOCAL_MAC is n, and
	 * refers only to the most recent scan.
	 */
	uint16_t scan_local_mac_dropped;
};

/** @brief printf-style line sink.
 *
 * Each invocation emits exactly one line; the sink appends the newline. This keeps the
 * formatter independent of the shell so tests can capture its output into a buffer.
 */
typedef void (*survey_print_fn)(void *ctx, const char *fmt, ...) __printf_like(2, 3);

/** @brief Discard the cached observation and zero the counters. */
void survey_obs_reset(void);

/** @brief What one scan left in the cache, and what the BSSID filter took out of it. */
struct survey_obs_scan_counts {
	/** Access points cached, after filtering and after the wifi_cnt bound. */
	uint16_t kept;
	/** Access points removed for having a locally-administered BSSID.
	 *
	 * Always zero when CONFIG_APP_SURVEY_DROP_LOCAL_MAC is n.
	 */
	uint16_t dropped;
};

/** @brief Fold a location module message into the cache.
 *
 * Messages other than LOCATION_GNSS_DATA and LOCATION_CLOUD_REQUEST are ignored.
 *
 * @param msg        Message from @c location_chan.
 * @param now_ms     Current time, in the base described by @p time_base. Used to stamp
 *                   scans, which the location module leaves unstamped.
 * @param time_base  Which clock @p now_ms and @c msg->timestamp are against.
 *
 * @return The cached and dropped access point counts for this scan. Both zero for any
 *         message that is not a scan.
 *
 * Returned rather than read back through an accessor, and both counts rather than just
 * the dropped one. The cache is not private to @c location_chan -- @c survey clear and
 * @c survey selftest reach it from the shell thread, outside the channel mutex that
 * serialises publishers -- so a second look could see a different scan, or none, and
 * subtracting one snapshot's count from another's can go negative. Returning @c kept as
 * well means the caller needs no arithmetic at all: it cannot derive a surviving count
 * from @c msg->cloud_request.wifi_cnt that disagrees with what was actually stored, which
 * it otherwise could whenever a message arrives with more access points than the cache's
 * array holds and the count is clamped to fit.
 */
struct survey_obs_scan_counts survey_obs_update(const struct location_msg *msg, int64_t now_ms,
						enum survey_time_base time_base);

/** @brief Mark the cached observation as injected test data.
 *
 * Used by the selftest console command. Cleared by the next real observation.
 */
void survey_obs_mark_synthetic(void);

/** @brief Copy the cached observation into @p out.
 *
 * The cache is written from the publishing context of @c location_chan and read from
 * whichever thread is rendering it, so readers take a consistent copy rather than a
 * pointer into live state.
 */
void survey_obs_snapshot(struct survey_observation *out);

/** @brief Render an observation as human-readable lines.
 *
 * Field names follow the nRF Cloud ground-fix API. Measurements the modem did not make
 * are rendered as "absent" rather than as the zero the struct was initialised with, so
 * that a partly-filled scan cannot be misread as a complete one.
 */
void survey_obs_format(const struct survey_observation *obs, survey_print_fn print, void *ctx);

/** @brief Render observation counters and cache state as human-readable lines.
 *
 * @param now_ms Current time in the same base as the cached timestamps, used to report
 *               how old the cached observation is. Pass 0 to omit the age.
 */
void survey_obs_format_stats(const struct survey_observation *obs, int64_t now_ms,
			     survey_print_fn print, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_OBS_H_ */
