/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Console commands for inspecting the radio survey.
 *
 * These exist so that every later checkpoint can be verified on target rather than
 * inferred. The handlers are deliberately thin: all rendering lives in survey_obs.c,
 * which is unit tested.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/net/wifi.h>
#include <zephyr/sys/util.h>
#include <modem/lte_lc.h>
#include <errno.h>
#include <stdarg.h>

#include "app_common.h"
#include "location.h"
#include "survey.h"
#include "survey_obs.h"
#include "survey_record.h"

/* Guards against a future Kconfig edit that separates the two symbols: without the scan
 * trigger handler compiled into the location module, "survey scan" would publish a
 * message nobody acts on and appear to do nothing at all.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_APP_LOCATION_SCAN_TRIGGER),
	     "CONFIG_APP_SURVEY requires CONFIG_APP_LOCATION_SCAN_TRIGGER");

/* Scratch space for rendering and for the selftest's injected messages.
 *
 * File scope rather than on the stack: both members are around a kilobyte and
 * CONFIG_SHELL_STACK_SIZE is 2560. A union is enough because shell commands run one at a
 * time on the shell thread, and the selftest is finished with the message before it takes
 * the snapshot.
 */
static union {
	struct survey_observation obs;
	struct location_msg msg;
} scratch;

/* Not part of the union above: building a record reads the snapshot while writing the
 * record, so the two cannot share storage. Both are around a kilobyte, which is why
 * neither lives on the shell thread's stack.
 */
static struct survey_record_data hex_record;
static uint8_t hex_buf[SURVEY_RECORD_MAX_SIZE];

/* One line stays inside the shell's own output buffer and remains greppable by eye. */
#define HEX_BYTES_PER_LINE 32

/* Adapter from the formatter's line sink to the shell. */
static void shell_line_print(void *ctx, const char *fmt, ...)
{
	const struct shell *sh = ctx;
	va_list args;

	va_start(args, fmt);
	shell_vfprintf(sh, SHELL_NORMAL, fmt, args);
	va_end(args);
	shell_fprintf(sh, SHELL_NORMAL, "\n");
}

static int trigger_publish(const struct shell *sh, enum location_msg_type type, const char *what,
			   const char *how_long)
{
	struct location_msg msg = { .type = type };
	int err;

	err = zbus_chan_pub(&location_chan, &msg, PUB_TIMEOUT);
	if (err) {
		shell_error(sh, "Failed to request %s: %d", what, err);
		return err;
	}

	/* The location module drops a trigger that arrives while a search is already
	 * running, and publishing to zbus succeeds either way, so this command cannot
	 * report that outcome directly. Point at the log, where it is now a warning.
	 */
	shell_print(sh, "%s requested. Wait %s, then run \"survey show\". If a search is "
			"already running this trigger is dropped and a warning is logged.",
		    what, how_long);
	return 0;
}

static int cmd_survey_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return trigger_publish(sh, LOCATION_SCAN_SEARCH_TRIGGER,
			       "Radio scan (Wi-Fi + cellular)", "about 5-10 s");
}

static int cmd_survey_gnss(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return trigger_publish(sh, LOCATION_GNSS_SEARCH_TRIGGER, "GNSS-only fix",
			       "seconds once warm, up to 10 minutes from cold");
}

static int cmd_survey_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	survey_obs_snapshot(&scratch.obs);
	survey_obs_format(&scratch.obs, shell_line_print, (void *)sh);

	return 0;
}

static int cmd_survey_stats(const struct shell *sh, size_t argc, char **argv)
{
	enum survey_time_base time_base;
	int64_t now_ms;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	survey_time_now(&now_ms, &time_base);

	survey_obs_snapshot(&scratch.obs);
	survey_obs_format_stats(&scratch.obs, now_ms, shell_line_print, (void *)sh);

	return 0;
}

static int cmd_survey_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	survey_obs_reset();
	shell_print(sh, "Observation cache cleared.");

	return 0;
}

/* Feeds the cache a synthetic observation and renders it.
 *
 * This makes the cache and the formatter provable on a bench with no SIM, no antenna and
 * no sky view: if "survey selftest" prints sensible values then everything between
 * location_chan and the console is working, and any subsequent empty "survey show" is a
 * radio or plumbing problem rather than a rendering one.
 *
 * The cache is marked synthetic so the injected values cannot later be mistaken for a
 * measurement, whether or not the operator remembers to run "survey clear".
 */
static int cmd_survey_selftest(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Injecting synthetic GNSS fix and scan...");

	scratch.msg = (struct location_msg){
		.type = LOCATION_GNSS_DATA,
		.timestamp = 1767225600000,
		.gnss_data = {
			.latitude = 63.4212340,
			.longitude = 10.4056780,
			.accuracy = 4.2f,
			.datetime = {
				.valid = true,
				.year = 2026, .month = 1, .day = 1,
				.hour = 0, .minute = 0, .second = 0, .ms = 0,
			},
#if defined(CONFIG_LOCATION_DATA_DETAILS)
			/* Non-zero on purpose. The encoder writes altitude, speed,
			 * heading and satsUsed whenever a fix carries details, and each
			 * of those has a legitimate zero -- so leaving this block at its
			 * default would make "survey selftest" followed by "survey hex"
			 * emit four fabricated zeroes as measurements, which is exactly
			 * the confusion the schema's absent-not-zero rule exists to
			 * prevent.
			 */
			.details = {
				.gnss = {
					.satellites_used = 7,
					.pvt_data = {
						.altitude = 42.5f,
						.speed = 1.25f,
						.heading = 137.0f,
					},
				},
			},
#endif /* CONFIG_LOCATION_DATA_DETAILS */
		},
	};
	survey_obs_update(&scratch.msg, 1767225600000, SURVEY_TIME_BASE_UNIX);

	scratch.msg = (struct location_msg){
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = {
			.current_cell = {
				.id = 0x00BC614E, .mcc = 242, .mnc = 1, .tac = 4660,
				.earfcn = 6300, .rsrp = 55, .rsrq = 30,
				.timing_advance = LTE_LC_CELL_TIMING_ADVANCE_INVALID,
			},
			.ncells_count = 1,
			.neighbor_cells = {
				{ .phys_cell_id = 101, .earfcn = 6300, .rsrp = 45,
				  .rsrq = 22, .time_diff = 24 },
			},
			.wifi_cnt = 1,
			.wifi_aps = {
				{ .rssi = -62,
				  .mac = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 },
				  .mac_length = 6,
				  .channel = 6,
				  .band = WIFI_FREQ_BAND_2_4_GHZ },
			},
		},
	};
	survey_obs_update(&scratch.msg, 1767225604000, SURVEY_TIME_BASE_UNIX);
	survey_obs_mark_synthetic();

	survey_obs_snapshot(&scratch.obs);
	survey_obs_format(&scratch.obs, shell_line_print, (void *)sh);

	shell_print(sh, "Expected: lat 63.4212340, eci 12345678, rsrp -86 dBm, "
			"1 neighbor, 1 AP 00:11:22:33:44:55 on channel 6 = 2437 MHz.");

	return 0;
}

/* Encodes the cached observation and prints the CBOR as hex.
 *
 * This is CP3's on-target proof: it exercises the real encoder against real modem data
 * and needs no host tooling to show that encoding happened and how large the result is.
 * The delimiters exist so the host decoder can lift the payload straight out of a
 * terminal capture without the operator editing anything by hand.
 *
 * The record is built from the observation cache, which holds the latest fix and the
 * latest scan independently. Until the capture orchestrator lands, that means no second
 * bracketing fix and no measurement offsets -- see survey_record_from_obs.
 */
static int cmd_survey_hex(const struct shell *sh, size_t argc, char **argv)
{
	size_t len;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	survey_obs_snapshot(&scratch.obs);

	if (!scratch.obs.gnss_valid && !scratch.obs.scan_valid) {
		shell_warn(sh, "Nothing cached. Run \"survey scan\", \"survey gnss\" or "
			       "\"survey selftest\" first.");
		return -ENODATA;
	}

	/* Loud, because these bytes are indistinguishable from a real record once they are
	 * pasted into the host decoder.
	 */
	if (scratch.obs.synthetic) {
		shell_warn(sh, "Cache holds synthetic data: this record is NOT a measurement.");
	}

	survey_record_from_obs(&scratch.obs, 0, &hex_record);

	err = survey_record_encode(&hex_record, hex_buf, sizeof(hex_buf), &len);
	if (err == -ENOMEM) {
		shell_error(sh, "Encode failed: record did not fit in %u bytes.",
			    (unsigned int)sizeof(hex_buf));
		return err;
	} else if (err) {
		shell_error(sh, "Encode failed (%d): a value is outside its schema type. "
				"Enlarging the buffer will not help.", err);
		return err;
	}

	shell_print(sh, "survey record: %u bytes, schema version %d", (unsigned int)len,
		    SURVEY_RECORD_VERSION);
	shell_print(sh, "-----BEGIN SURVEY RECORD-----");

	for (size_t i = 0; i < len; i += HEX_BYTES_PER_LINE) {
		char line[HEX_BYTES_PER_LINE * 2 + 1];
		size_t n = MIN(HEX_BYTES_PER_LINE, len - i);

		bin2hex(&hex_buf[i], n, line, sizeof(line));
		shell_print(sh, "%s", line);
	}

	shell_print(sh, "-----END SURVEY RECORD-----");

	return 0;
}

/* SHELL_CMD_ARG with zero optional arguments, so that a mistyped "survey show 3" reports
 * an error instead of silently ignoring the argument.
 */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_survey,
	SHELL_CMD_ARG(scan, NULL,
		      "Request a Wi-Fi + cellular scan (no GNSS)", cmd_survey_scan, 1, 0),
	SHELL_CMD_ARG(gnss, NULL,
		      "Request a GNSS-only fix", cmd_survey_gnss, 1, 0),
	SHELL_CMD_ARG(show, NULL,
		      "Print the cached GNSS fix and radio scan", cmd_survey_show, 1, 0),
	SHELL_CMD_ARG(stats, NULL,
		      "Print observation counters and cache age", cmd_survey_stats, 1, 0),
	SHELL_CMD_ARG(clear, NULL,
		      "Discard the cached observation", cmd_survey_clear, 1, 0),
	SHELL_CMD_ARG(selftest, NULL,
		      "Render a synthetic observation; needs no radio", cmd_survey_selftest, 1, 0),
	SHELL_CMD_ARG(hex, NULL,
		      "Encode the cached observation and hexdump the CBOR", cmd_survey_hex, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(survey, &sub_survey, "Radio survey commands", NULL);
