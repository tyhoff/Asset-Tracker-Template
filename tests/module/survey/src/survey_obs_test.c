/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Unit tests for the survey observation cache and its renderer.
 *
 * The renderer's output is what an operator on a bench will read to decide whether a
 * capture worked, so the assertions here are about the things that would make that
 * reading wrong: a missing measurement shown as 0, an index shown as if it were dBm, a
 * field silently dropped, or a stale cache presented as fresh.
 */

#include <unity.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/wifi.h>
#include <modem/lte_lc.h>

#include "survey_obs.h"

/* Captured renderer output, one line per print() call, newline separated. */
/* A full-capacity render (10 APs + 10 neighbors + 10 GCI cells) is around 5 KB. */
#define OUT_SIZE 16384
static char out[OUT_SIZE];
static size_t out_len;
static unsigned int out_lines;

static void capture(void *ctx, const char *fmt, ...)
{
	va_list args;
	int written;

	ARG_UNUSED(ctx);

	va_start(args, fmt);
	written = vsnprintf(out + out_len, OUT_SIZE - out_len, fmt, args);
	va_end(args);

	TEST_ASSERT_GREATER_THAN_INT(0, written);
	TEST_ASSERT_LESS_THAN_size_t(OUT_SIZE - out_len, (size_t)written);

	out_len += (size_t)written;
	out[out_len++] = '\n';
	out[out_len] = '\0';
	out_lines++;
}

static void capture_reset(void)
{
	memset(out, 0, sizeof(out));
	out_len = 0;
	out_lines = 0;
}

/* Assert that the rendered output does (or does not) contain a substring. */
#define ASSERT_OUT_HAS(substr)                                                             \
	do {                                                                               \
		if (strstr(out, (substr)) == NULL) {                                       \
			TEST_FAIL_MESSAGE("expected to find \"" substr "\" in output");     \
		}                                                                          \
	} while (0)

#define ASSERT_OUT_LACKS(substr)                                                           \
	do {                                                                               \
		if (strstr(out, (substr)) != NULL) {                                       \
			TEST_FAIL_MESSAGE("did not expect \"" substr "\" in output");       \
		}                                                                          \
	} while (0)

static struct location_msg gnss_msg(void)
{
	struct location_msg msg = {
		.type = LOCATION_GNSS_DATA,
		.timestamp = 1767225600000,
		.gnss_data = {
			.latitude = 63.4212340,
			.longitude = 10.4056780,
			.accuracy = 4.2f,
			.datetime = { .valid = true, .year = 2026, .month = 1, .day = 1 },
		},
	};

	msg.gnss_data.details.gnss.satellites_tracked = 9;
	msg.gnss_data.details.gnss.satellites_used = 7;
	msg.gnss_data.details.gnss.pvt_data.altitude = 128.5f;
	msg.gnss_data.details.gnss.pvt_data.speed = 31.25f;
	msg.gnss_data.details.gnss.pvt_data.heading = 275.5f;

	return msg;
}

/* A scan with everything populated with valid values. */
static struct location_msg scan_msg(void)
{
	struct location_msg msg = {
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = {
			.current_cell = {
				/* 0x00BC614E == 12345678 */
				.id = 0x00BC614E, .mcc = 242, .mnc = 1, .tac = 4660,
				.earfcn = 6300, .rsrp = 55, .rsrq = 30,
				.timing_advance = 64,
			},
			.ncells_count = 2,
			.neighbor_cells = {
				{ .phys_cell_id = 101, .earfcn = 6300, .rsrp = 45,
				  .rsrq = 22, .time_diff = 24 },
				{ .phys_cell_id = 202, .earfcn = 1650, .rsrp = 30,
				  .rsrq = 10, .time_diff = -8 },
			},
			.gci_cells_count = 1,
			.gci_cells = {
				{ .id = 0x00BC614F, .mcc = 242, .mnc = 1, .tac = 4661,
				  .earfcn = 1650, .rsrp = 40, .rsrq = 20 },
			},
			.wifi_cnt = 2,
			.wifi_aps = {
				{ .rssi = -62, .mac_length = 6,
				  .mac = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 },
				  .channel = 6, .band = WIFI_FREQ_BAND_2_4_GHZ },
				{ .rssi = -80, .mac_length = 6,
				  .mac = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF },
				  .channel = 36, .band = WIFI_FREQ_BAND_5_GHZ },
			},
		},
	};

	return msg;
}

void setUp(void)
{
	survey_obs_reset();
	capture_reset();
}

void tearDown(void)
{
}

/* --- cache behaviour --------------------------------------------------------------- */

void test_cache_starts_empty(void)
{
	struct survey_observation o;

	survey_obs_snapshot(&o);

	TEST_ASSERT_FALSE(o.gnss_valid);
	TEST_ASSERT_FALSE(o.scan_valid);
	TEST_ASSERT_EQUAL_UINT32(0, o.gnss_count);
	TEST_ASSERT_EQUAL_UINT32(0, o.scan_count);
	TEST_ASSERT_EQUAL_INT(SURVEY_TIME_BASE_NONE, o.gnss_time_base);
}

void test_gnss_message_is_cached(void)
{
	struct location_msg msg = gnss_msg();
	struct survey_observation o;

	survey_obs_update(&msg, 999, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);

	TEST_ASSERT_TRUE(o.gnss_valid);
	TEST_ASSERT_EQUAL_UINT32(1, o.gnss_count);
	TEST_ASSERT_EQUAL_DOUBLE(63.4212340, o.gnss.latitude);
	TEST_ASSERT_EQUAL_DOUBLE(10.4056780, o.gnss.longitude);
	TEST_ASSERT_EQUAL_FLOAT(4.2f, o.gnss.accuracy);
	TEST_ASSERT_EQUAL_INT(SURVEY_TIME_BASE_UNIX, o.gnss_time_base);

	/* GNSS carries its own timestamp; now_ms must not override it. */
	TEST_ASSERT_EQUAL_INT64(1767225600000, o.gnss_timestamp);

	/* A GNSS fix must not mark a scan as available. */
	TEST_ASSERT_FALSE(o.scan_valid);
}

void test_scan_message_is_cached_and_stamped_on_receipt(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* Cloud requests carry no timestamp of their own; msg.timestamp is left at 0 and
	 * must not be mistaken for a real time.
	 */
	TEST_ASSERT_EQUAL_INT64(0, msg.timestamp);

	survey_obs_update(&msg, 1767225604000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);

	TEST_ASSERT_TRUE(o.scan_valid);
	TEST_ASSERT_EQUAL_UINT32(1, o.scan_count);
	TEST_ASSERT_EQUAL_INT64(1767225604000, o.scan_timestamp);
	TEST_ASSERT_EQUAL_UINT8(2, o.scan.ncells_count);
	TEST_ASSERT_EQUAL_UINT8(1, o.scan.gci_cells_count);
	TEST_ASSERT_EQUAL_UINT16(2, o.scan.wifi_cnt);
	TEST_ASSERT_EQUAL_UINT32(0x00BC614E, o.scan.current_cell.id);
	TEST_ASSERT_EQUAL_UINT8_ARRAY(msg.cloud_request.wifi_aps[1].mac, o.scan.wifi_aps[1].mac,
				      MAC_ADDR_LEN);
	TEST_ASSERT_FALSE(o.gnss_valid);
}

void test_uptime_time_base_is_preserved(void)
{
	struct location_msg msg = gnss_msg();
	struct survey_observation o;

	/* A fix stamped against uptime cannot be correlated with anything off-device. If
	 * this were silently recorded as Unix time the whole record would be unusable
	 * without being detectably so.
	 */
	msg.timestamp = 45000;
	survey_obs_update(&msg, 45000, SURVEY_TIME_BASE_UPTIME);
	survey_obs_snapshot(&o);

	TEST_ASSERT_EQUAL_INT(SURVEY_TIME_BASE_UPTIME, o.gnss_time_base);

	survey_obs_format(&o, capture, NULL);
	ASSERT_OUT_HAS("(uptime)");
	ASSERT_OUT_LACKS("(unix)");
}

void test_non_observation_messages_are_ignored(void)
{
	const enum location_msg_type ignored[] = {
		LOCATION_SEARCH_STARTED, LOCATION_SEARCH_DONE, LOCATION_AGNSS_REQUEST,
		LOCATION_MODULE_READY, LOCATION_SEARCH_TRIGGER, LOCATION_GNSS_SEARCH_TRIGGER,
		LOCATION_SEARCH_CANCEL, LOCATION_SCAN_SEARCH_TRIGGER,
	};
	struct survey_observation o;

	for (size_t i = 0; i < ARRAY_SIZE(ignored); i++) {
		struct location_msg msg = { .type = ignored[i] };

		survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	}

	survey_obs_snapshot(&o);

	TEST_ASSERT_EQUAL_UINT32(0, o.gnss_count);
	TEST_ASSERT_EQUAL_UINT32(0, o.scan_count);
	TEST_ASSERT_FALSE(o.gnss_valid);
	TEST_ASSERT_FALSE(o.scan_valid);
}

void test_counters_count_all_observations_not_just_cached_ones(void)
{
	struct location_msg gnss = gnss_msg();
	struct location_msg scan = scan_msg();
	struct survey_observation o;

	for (int i = 0; i < 3; i++) {
		survey_obs_update(&gnss, 1000, SURVEY_TIME_BASE_UNIX);
	}
	survey_obs_update(&scan, 2000, SURVEY_TIME_BASE_UNIX);

	survey_obs_snapshot(&o);

	TEST_ASSERT_EQUAL_UINT32(3, o.gnss_count);
	TEST_ASSERT_EQUAL_UINT32(1, o.scan_count);
}

void test_reset_clears_cache_and_counters(void)
{
	struct location_msg gnss = gnss_msg();
	struct location_msg scan = scan_msg();
	struct survey_observation o;

	survey_obs_update(&gnss, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_update(&scan, 2000, SURVEY_TIME_BASE_UNIX);

	survey_obs_reset();
	survey_obs_snapshot(&o);

	TEST_ASSERT_FALSE(o.gnss_valid);
	TEST_ASSERT_FALSE(o.scan_valid);
	TEST_ASSERT_EQUAL_UINT32(0, o.gnss_count);
	TEST_ASSERT_EQUAL_UINT32(0, o.scan_count);
}

void test_snapshot_is_a_copy_not_a_view(void)
{
	struct location_msg gnss = gnss_msg();
	struct survey_observation first;
	struct survey_observation second;

	survey_obs_update(&gnss, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&first);

	/* A later observation must not mutate an already-taken snapshot -- otherwise a
	 * long render could interleave two different captures into one output.
	 */
	gnss.gnss_data.latitude = 0.0;
	survey_obs_update(&gnss, 2000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&second);

	TEST_ASSERT_EQUAL_DOUBLE(63.4212340, first.gnss.latitude);
	TEST_ASSERT_EQUAL_DOUBLE(0.0, second.gnss.latitude);
	TEST_ASSERT_EQUAL_UINT32(1, first.gnss_count);
	TEST_ASSERT_EQUAL_UINT32(2, second.gnss_count);
}

void test_null_arguments_are_tolerated(void)
{
	struct survey_observation o;

	/* Called from a zbus callback and a shell handler; neither should be able to
	 * crash the device with a bad pointer.
	 */
	survey_obs_update(NULL, 0, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(NULL);
	survey_obs_format(NULL, capture, NULL);
	survey_obs_format_stats(NULL, 0, capture, NULL);

	survey_obs_snapshot(&o);
	survey_obs_format(&o, NULL, NULL);
	survey_obs_format_stats(&o, 0, NULL, NULL);

	TEST_ASSERT_EQUAL_UINT32(0, out_lines);
}

/* --- rendering -------------------------------------------------------------------- */

void test_empty_cache_renders_as_explicitly_empty(void)
{
	struct survey_observation o;

	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	/* An empty cache must say so rather than printing a plausible-looking record of
	 * zeros.
	 */
	ASSERT_OUT_HAS("GNSS: no fix cached");
	ASSERT_OUT_HAS("Scan: none cached");
	ASSERT_OUT_LACKS("eci");
	ASSERT_OUT_LACKS("macAddress");
}

void test_gnss_renders_position_and_pvt_derived_fields(void)
{
	struct location_msg msg = gnss_msg();
	struct survey_observation o;

	survey_obs_update(&msg, 1767225600000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("lat 63.4212340");
	ASSERT_OUT_HAS("lon 10.4056780");
	ASSERT_OUT_HAS("acc 4.2 m");
	ASSERT_OUT_HAS("ts 1767225600000 (unix)");

	/* alt, spd and hdg are required for interpolation and for profile gating, and are
	 * absent from struct location_data. They are reachable only through the PVT frame
	 * that LOCATION_DATA_DETAILS carries, so assert they actually come out.
	 */
	ASSERT_OUT_HAS("alt 128.5 m");
	ASSERT_OUT_HAS("spd 31.25 m/s");
	ASSERT_OUT_HAS("hdg 275.50 deg");
	ASSERT_OUT_HAS("sats tracked 9 used 7");
}

void test_scan_renders_ground_fix_field_names(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	survey_obs_update(&msg, 1767225604000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	/* Names come from the nRF Cloud ground-fix API, so what is read on a console is
	 * the same vocabulary as the exported dataset and the API being validated.
	 */
	ASSERT_OUT_HAS("lte[] serving cell");
	ASSERT_OUT_HAS("eci 12345678");
	ASSERT_OUT_HAS("mcc 242 mnc 1 tac 4660");
	ASSERT_OUT_HAS("earfcn 6300");
	ASSERT_OUT_HAS("adv 64");

	ASSERT_OUT_HAS("nmr[] 2 neighbor(s)");
	ASSERT_OUT_HAS("pci 101");
	ASSERT_OUT_HAS("pci 202");
	ASSERT_OUT_HAS("timeDiff 24");
	ASSERT_OUT_HAS("timeDiff -8");

	ASSERT_OUT_HAS("gci 1 full-identity cell(s)");
	ASSERT_OUT_HAS("eci 12345679");

	ASSERT_OUT_HAS("wifi.accessPoints[] 2");
	ASSERT_OUT_HAS("macAddress 00:11:22:33:44:55");
	ASSERT_OUT_HAS("macAddress AA:BB:CC:DD:EE:FF");
	ASSERT_OUT_HAS("signalStrength -62 dBm");
	ASSERT_OUT_HAS("signalStrength -80 dBm");
}

void test_rsrp_and_rsrq_render_index_and_converted_value(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	/* The modem reports 3GPP indices, the ground-fix API takes dBm/dB. Rendering an
	 * index as though it were dBm is the kind of mistake that would silently skew an
	 * entire accuracy study, so both forms are shown and both are asserted.
	 *
	 * serving:     rsrp idx 55 -> 55 - 141 = -86 dBm; rsrq idx 30 -> (30-40)/2 = -5.0 dB
	 * neighbor[0]: rsrp idx 45 -> -96 dBm;            rsrq idx 22 -> -9.0 dB
	 * neighbor[1]: rsrp idx 30 -> -111 dBm;           rsrq idx 10 -> -15.0 dB
	 */
	ASSERT_OUT_HAS("rsrp 55 (idx) = -86 dBm");
	ASSERT_OUT_HAS("rsrq 30 (idx) = -5.0 dB");
	ASSERT_OUT_HAS("rsrp 45 (idx) = -96 dBm");
	ASSERT_OUT_HAS("rsrq 22 (idx) = -9.0 dB");
	ASSERT_OUT_HAS("rsrp 30 (idx) = -111 dBm");
	ASSERT_OUT_HAS("rsrq 10 (idx) = -15.0 dB");
}

void test_rsrq_conversion_uses_the_correct_branch_above_index_35(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* The RSRQ formula changes divisor offset at index 35. Index 40 must render as
	 * (40-41)/2 = -0.5 dB, not (40-40)/2 = 0.0 dB.
	 */
	msg.cloud_request.current_cell.rsrq = 40;
	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("rsrq 40 (idx) = -0.5 dB");
}

void test_unavailable_measurements_render_as_absent_not_as_zero(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* This is the single most important assertion in this suite. A measurement the
	 * modem could not make must never be rendered (or later stored) as 0, because 0
	 * is a plausible value for these fields and would be indistinguishable from a
	 * real reading.
	 */
	msg.cloud_request.current_cell.id = LTE_LC_CELL_EUTRAN_ID_INVALID;
	/* earfcn left valid so the cell is not wholly empty and each field is rendered
	 * individually; the wholly-empty case is covered by
	 * test_wifi_only_scan_reports_no_cellular_data.
	 */
	msg.cloud_request.current_cell.rsrp = LTE_LC_CELL_RSRP_INVALID;
	msg.cloud_request.current_cell.rsrq = LTE_LC_CELL_RSRQ_INVALID;
	msg.cloud_request.current_cell.timing_advance = LTE_LC_CELL_TIMING_ADVANCE_INVALID;
	msg.cloud_request.ncells_count = 0;
	msg.cloud_request.gci_cells_count = 0;
	msg.cloud_request.wifi_cnt = 0;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("eci absent");
	ASSERT_OUT_HAS("rsrp absent");
	ASSERT_OUT_HAS("rsrq absent");
	ASSERT_OUT_HAS("adv absent");

	/* And specifically not rendered as the sentinel or as zero. */
	ASSERT_OUT_LACKS("rsrp 255");
	ASSERT_OUT_LACKS("rsrq 255");
	ASSERT_OUT_LACKS("adv 65535");
	ASSERT_OUT_LACKS("adv 0");

	/* mcc/mnc/tac are meaningless without a cell identity and must not be printed. */
	ASSERT_OUT_LACKS("mcc 242");
}

void test_timing_advance_of_zero_is_treated_as_absent(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* Timing advance is only measured in RRC-connected state, which this application
	 * does not enter for capture. A 0 here is an uninitialised struct, not a reading.
	 */
	msg.cloud_request.current_cell.timing_advance = 0;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("adv absent");
}

void test_omitted_pci_is_reported_as_deliberate_rather_than_as_a_gap(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	/* mcc/mnc/eci/tac identify these cells globally, so PCI is not captured for them.
	 * The renderer has to say that it was a choice: an operator reading a bench capture
	 * must not conclude the radio failed to report it, nor file it as a defect.
	 */
	ASSERT_OUT_HAS("pci not captured (not needed for an identified cell)");
}

void test_wifi_frequency_is_derived_from_channel_and_band(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	/* The Wi-Fi driver reports channel and band but never a frequency, so the rendered
	 * frequency is computed. Channel numbers repeat across bands, which is why both
	 * inputs matter -- these two cases would collide if band were ignored.
	 */
	ASSERT_OUT_HAS("channel 6 frequency 2437 MHz band 2.4GHz");
	ASSERT_OUT_HAS("channel 36 frequency 5180 MHz band 5GHz");
}

void test_absent_wifi_channel_is_not_rendered_as_a_frequency(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* Channel 0 does not exist, so it is the driver's "not reported" value. Feeding it
	 * to the frequency formula would invent 2407 MHz out of a missing measurement.
	 */
	msg.cloud_request.wifi_cnt = 1;
	msg.cloud_request.wifi_aps[0].channel = 0;
	msg.cloud_request.wifi_aps[0].band = WIFI_FREQ_BAND_2_4_GHZ;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("channel absent");
	ASSERT_OUT_LACKS("2407 MHz");
}

void test_underivable_frequency_is_reported_rather_than_computed(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* An unrecognised band reaches the formatter; "0 MHz" would read as a measurement. */
	msg.cloud_request.wifi_cnt = 1;
	msg.cloud_request.wifi_aps[0].channel = 44;
	msg.cloud_request.wifi_aps[0].band = WIFI_FREQ_BAND_UNKNOWN;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("channel 44 frequency undetermined");
	ASSERT_OUT_LACKS("0 MHz");
}

void test_channel_outside_its_band_does_not_produce_a_frequency(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* Channel 165 is 5 GHz; the 2.4 GHz formula would yield a bogus 3232 MHz. */
	msg.cloud_request.wifi_cnt = 1;
	msg.cloud_request.wifi_aps[0].channel = 165;
	msg.cloud_request.wifi_aps[0].band = WIFI_FREQ_BAND_2_4_GHZ;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("channel 165 frequency undetermined");
	ASSERT_OUT_LACKS("3232");
}

void test_ap_and_neighbor_counts_bound_the_rendered_entries(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* Entries beyond the reported counts hold stale or zeroed data and must not be
	 * rendered as though they had been observed.
	 */
	msg.cloud_request.wifi_cnt = 1;
	msg.cloud_request.ncells_count = 1;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("macAddress 00:11:22:33:44:55");
	ASSERT_OUT_LACKS("macAddress AA:BB:CC:DD:EE:FF");
	ASSERT_OUT_HAS("pci 101");
	ASSERT_OUT_LACKS("pci 202");
}

void test_stats_report_counters_and_configured_caps(void)
{
	struct location_msg gnss = gnss_msg();
	struct survey_observation o;

	survey_obs_update(&gnss, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format_stats(&o, 0, capture, NULL);

	ASSERT_OUT_HAS("gnss fixes observed : 1");
	ASSERT_OUT_HAS("scans observed      : 0");
	ASSERT_OUT_HAS("none cached");

	/* The caps truncate dense-urban scans, so the values in force have to be visible
	 * on the device that produced a dataset rather than looked up in a build config.
	 */
	ASSERT_OUT_HAS("wifi_aps 10");
	ASSERT_OUT_HAS("neighbor_cells 10");
}


void test_negative_signal_indices_use_the_other_conversion_branch(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* Both conversions branch on idx < 0 and every other fixture uses positive
	 * indices, so without this half of each formula is unverified.
	 *
	 * rsrp -5  -> -5 - 140   = -145 dBm
	 * rsrq -10 -> (-10-39)/2 = -24.5 dB
	 */
	msg.cloud_request.current_cell.rsrp = -5;
	msg.cloud_request.current_cell.rsrq = -10;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_HAS("rsrp -5 (idx) = -145 dBm");
	ASSERT_OUT_HAS("rsrq -10 (idx) = -24.5 dB");
}

void test_rsrq_conversion_at_the_branch_point(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* Indices 34 and 35 straddle the divisor change and collide at -3.0 dB by design.
	 * Pinning both anchors the branch point itself.
	 */
	msg.cloud_request.current_cell.rsrq = 34;
	msg.cloud_request.ncells_count = 0;
	msg.cloud_request.gci_cells_count = 0;
	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);
	ASSERT_OUT_HAS("rsrq 34 (idx) = -3.0 dB");

	capture_reset();
	msg.cloud_request.current_cell.rsrq = 35;
	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);
	ASSERT_OUT_HAS("rsrq 35 (idx) = -3.0 dB");
}

void test_signal_index_zero_is_absent_not_minus_141_dbm(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* lte_lc.h documents index 0 as "not used" for both RSRP and RSRQ, and a
	 * Wi-Fi-only scan leaves the whole cell block zeroed. Rendering those zeroes
	 * would fabricate a serving cell at -141 dBm / -20.0 dB out of nothing -- the
	 * exact failure this module exists to prevent.
	 */
	msg.cloud_request.current_cell.rsrp = 0;
	msg.cloud_request.current_cell.rsrq = 0;
	msg.cloud_request.neighbor_cells[0].rsrp = 0;
	msg.cloud_request.neighbor_cells[0].rsrq = 0;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_LACKS("-141 dBm");
	ASSERT_OUT_LACKS("-20.0 dB");
	ASSERT_OUT_HAS("rsrp absent");
	ASSERT_OUT_HAS("rsrq absent");
}

void test_wifi_only_scan_reports_no_cellular_data(void)
{
	struct location_msg msg = {
		.type = LOCATION_CLOUD_REQUEST,
		.cloud_request = {
			/* Exactly what location_helper.c produces for a Wi-Fi-only result:
			 * an invalid cell identity and an otherwise zeroed cell block.
			 */
			.current_cell = { .id = LTE_LC_CELL_EUTRAN_ID_INVALID },
			.wifi_cnt = 1,
			.wifi_aps = {
				{ .rssi = -62, .mac_length = 6,
				  .mac = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 } },
			},
		},
	};
	struct survey_observation o;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	/* One clear statement beats six "absent" lines an operator has to interpret. */
	ASSERT_OUT_HAS("no cellular data in this scan");
	ASSERT_OUT_LACKS("mcc 0");
	ASSERT_OUT_LACKS("rsrp");
	ASSERT_OUT_LACKS("rsrq");

	/* The Wi-Fi side must still render. */
	ASSERT_OUT_HAS("macAddress 00:11:22:33:44:55");
}

void test_absence_is_reported_on_neighbors_and_gci_cells_too(void)
{
	struct location_msg msg = scan_msg();
	struct survey_observation o;

	/* format_signal is shared by serving, neighbor and GCI cells; only the serving
	 * cell was covered before.
	 */
	msg.cloud_request.ncells_count = 1;
	msg.cloud_request.neighbor_cells[0].rsrp = LTE_LC_CELL_RSRP_INVALID;
	msg.cloud_request.neighbor_cells[0].rsrq = LTE_LC_CELL_RSRQ_INVALID;
	msg.cloud_request.neighbor_cells[0].earfcn = 0;
	msg.cloud_request.gci_cells_count = 1;
	msg.cloud_request.gci_cells[0].earfcn = 0;

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	ASSERT_OUT_LACKS("rsrp 255");
	ASSERT_OUT_LACKS("rsrq 255");
	ASSERT_OUT_LACKS("earfcn 0");
	ASSERT_OUT_HAS("earfcn absent");
}

void test_full_capacity_render_is_complete_and_bounded(void)
{
	struct location_msg msg = { .type = LOCATION_CLOUD_REQUEST };
	struct survey_observation o;

	/* The configured maxima. Renders at this size are what an operator actually sees
	 * in a dense environment, and the line count matters over a serial console.
	 */
	msg.cloud_request.current_cell = scan_msg().cloud_request.current_cell;
	msg.cloud_request.ncells_count = CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX;
	msg.cloud_request.gci_cells_count = CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX;
	msg.cloud_request.wifi_cnt = CONFIG_APP_LOCATION_WIFI_APS_MAX;

	for (int i = 0; i < CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX; i++) {
		msg.cloud_request.neighbor_cells[i].phys_cell_id = 100 + i;
		msg.cloud_request.neighbor_cells[i].earfcn = 6300;
		msg.cloud_request.neighbor_cells[i].rsrp = 40 + i;
		msg.cloud_request.neighbor_cells[i].rsrq = 20;
		msg.cloud_request.gci_cells[i].id = 0x1000 + i;
		msg.cloud_request.gci_cells[i].earfcn = 6300;
		msg.cloud_request.gci_cells[i].rsrp = 40;
		msg.cloud_request.gci_cells[i].rsrq = 20;
	}
	for (int i = 0; i < CONFIG_APP_LOCATION_WIFI_APS_MAX; i++) {
		msg.cloud_request.wifi_aps[i].rssi = -50 - i;
		msg.cloud_request.wifi_aps[i].mac_length = 6;
		msg.cloud_request.wifi_aps[i].mac[0] = 0xF0;
		msg.cloud_request.wifi_aps[i].mac[5] = (uint8_t)i;
	}

	survey_obs_update(&msg, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);
	survey_obs_format(&o, capture, NULL);

	/* Every entry must appear -- nothing silently truncated by the renderer. */
	ASSERT_OUT_HAS("nmr[] 10 neighbor(s)");
	ASSERT_OUT_HAS("gci 10 full-identity cell(s)");
	ASSERT_OUT_HAS("wifi.accessPoints[] 10");
	ASSERT_OUT_HAS("pci 109");
	ASSERT_OUT_HAS("macAddress F0:00:00:00:00:09");

	/* Bounds the console cost of one "survey show" at full capacity. */
	TEST_ASSERT_LESS_THAN_UINT(250, out_lines);
}

void test_synthetic_data_is_banner_marked(void)
{
	struct location_msg gnss = gnss_msg();
	struct survey_observation o;

	/* survey selftest injects fabricated values into the live cache. If they could be
	 * read back as a measurement, a selftest run would contaminate a dataset.
	 */
	survey_obs_update(&gnss, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_mark_synthetic();
	survey_obs_snapshot(&o);

	TEST_ASSERT_TRUE(o.synthetic);

	survey_obs_format(&o, capture, NULL);
	ASSERT_OUT_HAS("SYNTHETIC SELFTEST DATA");

	capture_reset();
	survey_obs_format_stats(&o, 0, capture, NULL);
	ASSERT_OUT_HAS("SYNTHETIC SELFTEST DATA");
}

void test_real_observation_clears_the_synthetic_mark(void)
{
	struct location_msg gnss = gnss_msg();
	struct survey_observation o;

	survey_obs_mark_synthetic();
	survey_obs_update(&gnss, 1000, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);

	TEST_ASSERT_FALSE(o.synthetic);

	survey_obs_format(&o, capture, NULL);
	ASSERT_OUT_LACKS("SYNTHETIC");
}

void test_stats_report_cache_age(void)
{
	struct location_msg gnss = gnss_msg();
	struct survey_observation o;

	/* An absolute epoch-ms timestamp does not tell an operator in a moving vehicle
	 * whether the cached fix is 4 seconds or 40 minutes old.
	 */
	gnss.timestamp = 1767225600000;
	survey_obs_update(&gnss, gnss.timestamp, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);

	survey_obs_format_stats(&o, 1767225630000, capture, NULL);

	ASSERT_OUT_HAS("cached 30 s ago");
	ASSERT_OUT_HAS("scan                : none cached");
}

void test_stats_age_unknown_when_no_reference_time(void)
{
	struct location_msg gnss = gnss_msg();
	struct survey_observation o;

	survey_obs_update(&gnss, gnss.timestamp, SURVEY_TIME_BASE_UNIX);
	survey_obs_snapshot(&o);

	/* now_ms of 0, or a clock behind the cached stamp (a uptime/unix mix-up), must
	 * say so rather than print a nonsense age.
	 */
	survey_obs_format_stats(&o, 0, capture, NULL);
	ASSERT_OUT_HAS("age unknown");

	capture_reset();
	survey_obs_format_stats(&o, 1000, capture, NULL);
	ASSERT_OUT_HAS("age unknown");
}

/* Provided by the test runner generator. */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
