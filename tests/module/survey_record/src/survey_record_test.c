/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Round-trip tests for the survey record encoder.
 *
 * The assertions that matter are about what a stored record means, because the record is
 * the dataset: a field the modem never measured must come back absent rather than zero,
 * a real zero must survive as a measurement, and the whole thing has to fit the budget.
 */

#include <unity.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/wifi.h>
#include <modem/lte_lc.h>

#include "survey_record.h"
#include "survey_record_decode.h"
#include "survey_record_types.h"

static uint8_t buf[SURVEY_RECORD_MAX_SIZE];
static size_t encoded_len;
static struct survey_record decoded;

void setUp(void)
{
	memset(buf, 0, sizeof(buf));
	memset(&decoded, 0, sizeof(decoded));
	encoded_len = 0;
}

void tearDown(void)
{
}

/* Encode then decode, failing the test on either step. */
static void round_trip(const struct survey_record_data *rec)
{
	size_t decoded_len;
	int err;

	err = survey_record_encode(rec, buf, sizeof(buf), &encoded_len);
	TEST_ASSERT_EQUAL_INT(0, err);
	TEST_ASSERT_GREATER_THAN_size_t(0, encoded_len);

	printf("ENCODE OK: %zu bytes\n", encoded_len);
	err = cbor_decode_survey_record(buf, encoded_len, &decoded, &decoded_len);
	printf("DECODE returned %d\n", err);
	TEST_ASSERT_EQUAL_INT(ZCBOR_SUCCESS, err);
	TEST_ASSERT_EQUAL_size_t(encoded_len, decoded_len);
}

/* A fully populated cycle: everything measured, all caps filled. */
static struct survey_record_data full_record(void)
{
	struct survey_record_data rec = {
		.sequence = 42,
		.profile = SURVEY_PROFILE_DEEP,
		.time_base = SURVEY_TIME_BASE_UNIX,
		.t_base_ms = 1786140992000,
		.gnss_before_valid = true,
		.gnss_after_valid = true,
		.cell_start = { .valid = true, .ms = 120 },
		.cell_end = { .valid = true, .ms = 4700 },
		.wifi_start = { .valid = true, .ms = 130 },
		.wifi_end = { .valid = true, .ms = 4650 },
		.gnss_after_at = { .valid = true, .ms = 6800 },
		.scan_valid = true,
		.network_mode = SURVEY_NETWORK_MODE_LTEM,
	};

	rec.gnss_before.latitude = 37.7712317;
	rec.gnss_before.longitude = -122.4310404;
	rec.gnss_before.accuracy = 18.3f;
	rec.gnss_before.details.gnss.satellites_used = 6;
	rec.gnss_before.details.gnss.pvt_data.altitude = 25.0f;
	rec.gnss_before.details.gnss.pvt_data.speed = 31.25f;
	rec.gnss_before.details.gnss.pvt_data.heading = 275.5f;

	rec.gnss_after = rec.gnss_before;
	rec.gnss_after.latitude = 37.7713000;

	rec.scan.current_cell = (struct location_cell_info){
		.id = 21414146, .mcc = 310, .mnc = 260, .tac = 14453,
		.earfcn = 2300, .rsrp = 54, .rsrq = 19,
		.timing_advance = LTE_LC_CELL_TIMING_ADVANCE_INVALID,
	};

	rec.scan.ncells_count = CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX;
	for (int i = 0; i < rec.scan.ncells_count; i++) {
		rec.scan.neighbor_cells[i] = (struct location_neighbor_cell_info){
			.phys_cell_id = 100 + i, .earfcn = 650, .rsrp = 60, .rsrq = 21,
			.time_diff = 32,
		};
	}

	rec.scan.gci_cells_count = 3;
	for (int i = 0; i < 3; i++) {
		rec.scan.gci_cells[i] = (struct location_cell_info){
			.id = 170307089 + i, .mcc = 310, .mnc = 410, .tac = 35634,
			.earfcn = 5110, .rsrp = 69, .rsrq = 18,
			.timing_advance = LTE_LC_CELL_TIMING_ADVANCE_INVALID,
		};
	}

	rec.scan.wifi_cnt = CONFIG_APP_LOCATION_WIFI_APS_MAX;
	for (int i = 0; i < rec.scan.wifi_cnt; i++) {
		rec.scan.wifi_aps[i] = (struct location_wifi_ap_info){
			.rssi = -40 - i, .mac_length = 6,
			.mac = { 0x94, 0x2A, 0x6F, 0xC4, 0x48, (uint8_t)i },
			.channel = 116, .band = WIFI_FREQ_BAND_5_GHZ,
		};
	}

	return rec;
}

/* The canonical session header, shared with the host fixture. */
static struct survey_record_session full_session(void)
{
	return (struct survey_record_session){
		.device_id = "thingy91x-4694acc7099",
		.app_version = "1.5.4",
		.modem_version = "mfw_nrf91x1_2.0.4",
		.exported_at_valid = true,
		.exported_at_ms = 1786140992000,
	};
}

void test_scalar_header_fields_survive_the_round_trip(void)
{
	struct survey_record_data rec = full_record();

	round_trip(&rec);

	TEST_ASSERT_EQUAL_UINT32(SURVEY_RECORD_VERSION, decoded.version_m);
	TEST_ASSERT_EQUAL_UINT32(42, decoded.sequence_m);
	TEST_ASSERT_EQUAL_UINT32(SURVEY_PROFILE_DEEP, decoded.profile_m);
	TEST_ASSERT_EQUAL_UINT32(SURVEY_TIME_BASE_UNIX, decoded.time_base_m);
	TEST_ASSERT_EQUAL_UINT64(1786140992000, decoded.t_base_m);
	TEST_ASSERT_TRUE(decoded.network_mode_m_present);
	TEST_ASSERT_EQUAL_UINT32(SURVEY_NETWORK_MODE_LTEM, decoded.network_mode_m.network_mode_m);
}

void test_position_survives_at_the_precision_the_schema_promises(void)
{
	struct survey_record_data rec = full_record();

	round_trip(&rec);

	/* Scaled integers at 1e-7 degrees. The exact values matter: this is ground truth,
	 * and a sign or scale error here would silently corrupt every accuracy figure
	 * computed from the dataset.
	 */
	TEST_ASSERT_TRUE(decoded.gnss_fix_before_m_present);
	TEST_ASSERT_EQUAL_INT32(377712317,
				decoded.gnss_fix_before_m.gnss_fix_before_m.lat_e7_m);
	TEST_ASSERT_EQUAL_INT32(-1224310404,
				decoded.gnss_fix_before_m.gnss_fix_before_m.lon_e7_m);
	TEST_ASSERT_EQUAL_UINT32(18300, decoded.gnss_fix_before_m.gnss_fix_before_m.acc_mm_m);

	TEST_ASSERT_TRUE(decoded.gnss_fix_after_m_present);
	TEST_ASSERT_EQUAL_INT32(377713000, decoded.gnss_fix_after_m.gnss_fix_after_m.lat_e7_m);
}

void test_negative_longitude_is_not_mangled(void)
{
	struct survey_record_data rec = full_record();

	/* A signed CBOR integer is encoded differently from an unsigned one, and the
	 * western hemisphere is the only place this shows up.
	 */
	rec.gnss_before.longitude = -0.0000001;
	round_trip(&rec);

	TEST_ASSERT_EQUAL_INT32(-1, decoded.gnss_fix_before_m.gnss_fix_before_m.lon_e7_m);
}

void test_pvt_derived_fields_survive(void)
{
	struct survey_record_data rec = full_record();

	round_trip(&rec);

	const struct gnss_fix *fix = &decoded.gnss_fix_before_m.gnss_fix_before_m;

	TEST_ASSERT_TRUE(fix->alt_mm_m_present);
	TEST_ASSERT_EQUAL_INT32(25000, fix->alt_mm_m.alt_mm_m);
	TEST_ASSERT_TRUE(fix->spd_mmps_m_present);
	TEST_ASSERT_EQUAL_UINT32(31250, fix->spd_mmps_m.spd_mmps_m);
	TEST_ASSERT_TRUE(fix->hdg_cdeg_m_present);
	TEST_ASSERT_EQUAL_UINT32(27550, fix->hdg_cdeg_m.hdg_cdeg_m);
	TEST_ASSERT_TRUE(fix->sats_used_m_present);
	TEST_ASSERT_EQUAL_UINT32(6, fix->sats_used_m.sats_used_m);
}

void test_cells_and_aps_survive_with_their_counts(void)
{
	struct survey_record_data rec = full_record();

	round_trip(&rec);

	TEST_ASSERT_TRUE(decoded.serving_cell_m_present);
	TEST_ASSERT_EQUAL_UINT32(21414146, decoded.serving_cell_m.serving_cell_m.eci_m);
	TEST_ASSERT_EQUAL_UINT32(310, decoded.serving_cell_m.serving_cell_m.mcc_m);
	TEST_ASSERT_EQUAL_UINT32(260, decoded.serving_cell_m.serving_cell_m.mnc_m);
	TEST_ASSERT_EQUAL_UINT32(14453, decoded.serving_cell_m.serving_cell_m.tac_m);

	TEST_ASSERT_TRUE(decoded.neighbour_m_l_present);
	TEST_ASSERT_EQUAL_size_t(CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX,
				 decoded.neighbour_m_l.neighbour_m_count);
	TEST_ASSERT_EQUAL_UINT32(100, decoded.neighbour_m_l.neighbour_m[0].pci_m);

	TEST_ASSERT_TRUE(decoded.gci_cell_m_l_present);
	TEST_ASSERT_EQUAL_size_t(3, decoded.gci_cell_m_l.gci_cell_m_count);

	TEST_ASSERT_TRUE(decoded.access_point_m_l_present);
	TEST_ASSERT_EQUAL_size_t(CONFIG_APP_LOCATION_WIFI_APS_MAX,
				 decoded.access_point_m_l.access_point_m_count);
}

void test_mac_address_is_stored_as_six_raw_bytes(void)
{
	struct survey_record_data rec = full_record();
	const uint8_t expected[] = { 0x94, 0x2A, 0x6F, 0xC4, 0x48, 0x00 };

	round_trip(&rec);

	/* Binary, not text: six bytes rather than the seventeen a string would cost, times
	 * ten APs, times every record in a drive.
	 */
	TEST_ASSERT_EQUAL_size_t(6, decoded.access_point_m_l.access_point_m[0].mac_m.len);
	TEST_ASSERT_EQUAL_HEX8_ARRAY(expected,
				     decoded.access_point_m_l.access_point_m[0].mac_m.value, 6);
}

void test_unmeasured_fields_are_absent_not_zero(void)
{
	struct survey_record_data rec = full_record();

	/* The whole point of the format. The modem reports these as unavailable, and an
	 * encoded 0 would be indistinguishable from a real reading -- rsrq index 0 would
	 * decode as a signal quality, not as a gap.
	 */
	rec.scan.current_cell.rsrp = LTE_LC_CELL_RSRP_INVALID;
	rec.scan.current_cell.rsrq = LTE_LC_CELL_RSRQ_INVALID;
	rec.scan.current_cell.earfcn = 0;

	round_trip(&rec);

	TEST_ASSERT_TRUE(decoded.serving_cell_m_present);
	TEST_ASSERT_FALSE(decoded.serving_cell_m.serving_cell_m.rsrp_idx_m_present);
	TEST_ASSERT_FALSE(decoded.serving_cell_m.serving_cell_m.rsrq_idx_m_present);
	TEST_ASSERT_FALSE(decoded.serving_cell_m.serving_cell_m.earfcn_m_present);
}

void test_timing_advance_is_absent_when_the_modem_did_not_measure_it(void)
{
	struct survey_record_data rec = full_record();

	round_trip(&rec);

	/* adv is only measured in RRC-connected state, which this application does not
	 * hold, so it should be missing from essentially every real record.
	 */
	TEST_ASSERT_FALSE(decoded.serving_cell_m.serving_cell_m.adv_m_present);
}

void test_a_measured_zero_is_kept_as_a_measurement(void)
{
	struct survey_record_data rec = full_record();

	/* The mirror of the test above, and the reason absence is encoded by omission
	 * rather than by a zero: a device sitting still at sea level facing due north
	 * reports 0 for all three, and those are measurements.
	 *
	 * Note what is NOT used as the example here. timeDiff looks like the obvious
	 * candidate, but lte_lc.h defines LTE_LC_CELL_TIME_DIFF_INVALID as 0, so a zero
	 * there is the modem saying it could not align the neighbour at all. The PVT
	 * fields have no such marker, which is what makes them genuine.
	 */
	rec.gnss_before.details.gnss.pvt_data.altitude = 0.0f;
	rec.gnss_before.details.gnss.pvt_data.speed = 0.0f;
	rec.gnss_before.details.gnss.pvt_data.heading = 0.0f;

	round_trip(&rec);

	TEST_ASSERT_TRUE(decoded.gnss_fix_before_m.gnss_fix_before_m.alt_mm_m_present);
	TEST_ASSERT_EQUAL_INT32(0, decoded.gnss_fix_before_m.gnss_fix_before_m.alt_mm_m.alt_mm_m);
	TEST_ASSERT_TRUE(decoded.gnss_fix_before_m.gnss_fix_before_m.spd_mmps_m_present);
	TEST_ASSERT_EQUAL_UINT32(
		0, decoded.gnss_fix_before_m.gnss_fix_before_m.spd_mmps_m.spd_mmps_m);
	TEST_ASSERT_TRUE(decoded.gnss_fix_before_m.gnss_fix_before_m.hdg_cdeg_m_present);
	TEST_ASSERT_EQUAL_UINT32(
		0, decoded.gnss_fix_before_m.gnss_fix_before_m.hdg_cdeg_m.hdg_cdeg_m);
}

void test_absent_wifi_channel_omits_band_too(void)
{
	struct survey_record_data rec = full_record();

	/* Band has no "unknown" encoding -- its zero value is 2.4 GHz -- so storing it
	 * without a channel would assert a measurement that was never made.
	 */
	rec.scan.wifi_cnt = 1;
	rec.scan.wifi_aps[0].channel = 0;
	rec.scan.wifi_aps[0].band = WIFI_FREQ_BAND_2_4_GHZ;

	round_trip(&rec);

	TEST_ASSERT_FALSE(decoded.access_point_m_l.access_point_m[0].ap_channel_m_present);
	TEST_ASSERT_FALSE(decoded.access_point_m_l.access_point_m[0].ap_band_m_present);
}

void test_unidentified_serving_cell_is_omitted_entirely(void)
{
	struct survey_record_data rec = full_record();

	/* A Wi-Fi-only scan leaves the cell block zeroed. Without an identity there is
	 * nothing a lookup can use, and mcc/mnc/tac are mandatory in the schema, so the
	 * entry must not be written at all rather than written with placeholder identity.
	 */
	memset(&rec.scan.current_cell, 0, sizeof(rec.scan.current_cell));

	round_trip(&rec);

	TEST_ASSERT_FALSE(decoded.serving_cell_m_present);
}

void test_a_cycle_with_no_gnss_still_encodes(void)
{
	struct survey_record_data rec = full_record();

	/* The radio observations are still valid; they just cannot be scored. Dropping
	 * the record would throw away data that a later analysis may still want.
	 */
	rec.gnss_before_valid = false;
	rec.gnss_after_valid = false;

	round_trip(&rec);

	TEST_ASSERT_FALSE(decoded.gnss_fix_before_m_present);
	TEST_ASSERT_FALSE(decoded.gnss_fix_after_m_present);
	TEST_ASSERT_TRUE(decoded.access_point_m_l_present);
}

void test_offsets_are_signed_so_a_stepped_clock_does_not_wrap(void)
{
	struct survey_record_data rec = full_record();

	/* apply_gnss_time() can step the system clock from a GNSS fix mid-cycle, which
	 * can place a later event before the record's own time base.
	 */
	rec.cell_start.ms = -250;

	round_trip(&rec);

	TEST_ASSERT_TRUE(decoded.cell_start_ms_m_present);
	TEST_ASSERT_EQUAL_INT32(-250, decoded.cell_start_ms_m.cell_start_ms_m);
}

void test_full_record_fits_the_size_budget(void)
{
	struct survey_record_data rec = full_record();

	round_trip(&rec);

	/* FW-5 budgets 700 B for a FAST record. This is deliberately larger than one: a
	 * DEEP record with every cap filled -- 10 APs, 10 neighbours, 3 GCI cells and both
	 * GNSS fixes. Storage capacity is planned from this number, so it is asserted
	 * rather than merely printed; if a schema change pushes it past the budget, that
	 * is a capacity decision and not something to discover during a drive.
	 */
	printf("MEASURED full DEEP record: %zu bytes\n", encoded_len);

	TEST_ASSERT_LESS_OR_EQUAL_size_t(SURVEY_RECORD_BUDGET_SIZE, encoded_len);
}

/* The weak end of both 3GPP index ranges is negative -- RSRP -17..97, RSRQ -30..46 per
 * lte_lc.h -- and a cell-edge measurement is exactly what this survey exists to capture.
 * An unsigned schema type does not clip these: zcbor range-guards them, so one negative
 * neighbour reading fails the encode and discards the entire record, GNSS bracket
 * included. That failure would only ever appear at the edge of coverage, in the field.
 */
void test_negative_signal_indices_encode(void)
{
	struct survey_record_data rec = full_record();

	rec.scan.current_cell.rsrp = -17;
	rec.scan.current_cell.rsrq = -30;
	rec.scan.neighbor_cells[0].rsrp = -1;
	rec.scan.neighbor_cells[0].rsrq = -4;

	round_trip(&rec);

	TEST_ASSERT_TRUE(decoded.serving_cell_m_present);
	TEST_ASSERT_EQUAL_INT32(-17, decoded.serving_cell_m.serving_cell_m.rsrp_idx_m.rsrp_idx_m);
	TEST_ASSERT_EQUAL_INT32(-30, decoded.serving_cell_m.serving_cell_m.rsrq_idx_m.rsrq_idx_m);

	TEST_ASSERT_EQUAL_INT32(
		-1, decoded.neighbour_m_l.neighbour_m[0].nbr_rsrp_idx_m.nbr_rsrp_idx_m);
	TEST_ASSERT_EQUAL_INT32(
		-4, decoded.neighbour_m_l.neighbour_m[0].nbr_rsrq_idx_m.nbr_rsrq_idx_m);
}

/* LTE_LC_CELL_TIME_DIFF_INVALID is 0, so a neighbour the modem could not time-align is
 * indistinguishable from one aligned exactly. Storing it would fabricate the strongest
 * possible alignment out of a failed measurement.
 */
void test_invalid_time_diff_is_absent_not_zero(void)
{
	struct survey_record_data rec = full_record();

	rec.scan.neighbor_cells[0].time_diff = LTE_LC_CELL_TIME_DIFF_INVALID;

	round_trip(&rec);

	TEST_ASSERT_FALSE(decoded.neighbour_m_l.neighbour_m[0].time_diff_m_present);
	TEST_ASSERT_TRUE(decoded.neighbour_m_l.neighbour_m[1].time_diff_m_present);
}

/* survey_record_from_obs builds a record from the cache, which holds the latest fix and
 * the latest scan independently. There is no second bracketing fix and no measurement
 * boundaries, so everything that would imply one has to come out absent -- a zero here
 * would claim the closing fix happened at the same instant as the opening one.
 */
void test_from_obs_leaves_the_unmeasured_bracket_absent(void)
{
	struct survey_record_data rec;
	struct survey_observation obs = {
		.gnss_valid = true,
		.gnss_time_base = SURVEY_TIME_BASE_UNIX,
		.gnss_timestamp = 1786140992000,
		.scan_valid = true,
		.scan_time_base = SURVEY_TIME_BASE_UNIX,
		.scan_timestamp = 1786140997000,
	};

	obs.gnss.latitude = 37.7712317;
	obs.gnss.longitude = -122.4310404;
	obs.gnss.accuracy = 18.3f;
	obs.scan.current_cell = (struct location_cell_info){
		.id = 21414146, .mcc = 310, .mnc = 260, .tac = 14453,
		.earfcn = 2300, .rsrp = 54, .rsrq = 19,
		.timing_advance = LTE_LC_CELL_TIMING_ADVANCE_INVALID,
	};

	survey_record_from_obs(&obs, 7, &rec);
	round_trip(&rec);

	TEST_ASSERT_EQUAL_UINT32(7, decoded.sequence_m);
	TEST_ASSERT_EQUAL_UINT32(SURVEY_PROFILE_FAST, decoded.profile_m);
	TEST_ASSERT_EQUAL_UINT32(SURVEY_TIME_BASE_UNIX, decoded.time_base_m);
	TEST_ASSERT_EQUAL_UINT64(1786140992000, decoded.t_base_m);

	TEST_ASSERT_TRUE(decoded.gnss_fix_before_m_present);
	TEST_ASSERT_TRUE(decoded.serving_cell_m_present);

	TEST_ASSERT_FALSE(decoded.gnss_fix_after_m_present);
	TEST_ASSERT_FALSE(decoded.cell_start_ms_m_present);
	TEST_ASSERT_FALSE(decoded.cell_end_ms_m_present);
	TEST_ASSERT_FALSE(decoded.wifi_start_ms_m_present);
	TEST_ASSERT_FALSE(decoded.wifi_end_ms_m_present);
	TEST_ASSERT_FALSE(decoded.gnss_after_ms_m_present);
}

/* The time base has to follow whichever leg actually carried a timestamp. A scan-only
 * cycle is normal -- GNSS fails indoors and the radio observations are still worth
 * keeping -- and mislabelling its base would make the record unscoreable.
 */
void test_from_obs_falls_back_to_the_scan_time_base(void)
{
	struct survey_record_data rec;
	struct survey_observation obs = {
		.gnss_valid = false,
		.scan_valid = true,
		.scan_time_base = SURVEY_TIME_BASE_UPTIME,
		.scan_timestamp = 45000,
	};

	obs.scan.wifi_cnt = 1;
	obs.scan.wifi_aps[0] = (struct location_wifi_ap_info){
		.rssi = -62, .mac_length = 6,
		.mac = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 },
		.channel = 6, .band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	survey_record_from_obs(&obs, 1, &rec);
	round_trip(&rec);

	TEST_ASSERT_EQUAL_UINT32(SURVEY_TIME_BASE_UPTIME, decoded.time_base_m);
	TEST_ASSERT_EQUAL_UINT64(45000, decoded.t_base_m);
	TEST_ASSERT_FALSE(decoded.gnss_fix_before_m_present);
	TEST_ASSERT_EQUAL_size_t(1, decoded.access_point_m_l.access_point_m_count);
}

void test_session_header_round_trips(void)
{
	struct survey_record_session session = full_session();
	struct survey_session out;
	size_t len, decoded_len;
	int err;

	err = survey_record_session_encode(&session, buf, sizeof(buf), &len);
	TEST_ASSERT_EQUAL_INT(0, err);

	memset(&out, 0, sizeof(out));
	err = cbor_decode_survey_session(buf, len, &out, &decoded_len);
	TEST_ASSERT_EQUAL_INT(ZCBOR_SUCCESS, err);

	/* The version ties a dataset to the schema that produced it; the decoder refuses
	 * versions it does not know rather than guessing.
	 */
	TEST_ASSERT_EQUAL_UINT32(SURVEY_RECORD_VERSION, out.version_m);
	TEST_ASSERT_EQUAL_size_t(strlen(session.modem_version), out.modem_version_m.len);
	TEST_ASSERT_EQUAL_MEMORY(session.modem_version, out.modem_version_m.value,
				 out.modem_version_m.len);
	TEST_ASSERT_TRUE(out.exported_at_m_present);
}

void test_session_header_omits_an_unset_export_time(void)
{
	struct survey_record_session session = {
		.device_id = "d", .app_version = "a", .modem_version = "m",
		.exported_at_valid = false,
	};
	struct survey_session out;
	size_t len, decoded_len;
	int err;

	err = survey_record_session_encode(&session, buf, sizeof(buf), &len);
	TEST_ASSERT_EQUAL_INT(0, err);

	memset(&out, 0, sizeof(out));
	err = cbor_decode_survey_session(buf, len, &out, &decoded_len);
	TEST_ASSERT_EQUAL_INT(ZCBOR_SUCCESS, err);

	/* Epoch 0 would decode as 1970 and look like a real export time. */
	TEST_ASSERT_FALSE(out.exported_at_m_present);
}

void test_null_arguments_are_rejected(void)
{
	struct survey_record_data rec = full_record();
	size_t len;

	TEST_ASSERT_EQUAL_INT(-EINVAL, survey_record_encode(NULL, buf, sizeof(buf), &len));
	TEST_ASSERT_EQUAL_INT(-EINVAL, survey_record_encode(&rec, NULL, sizeof(buf), &len));
	TEST_ASSERT_EQUAL_INT(-EINVAL, survey_record_encode(&rec, buf, sizeof(buf), NULL));
}

void test_a_buffer_that_is_too_small_fails_rather_than_truncating(void)
{
	struct survey_record_data rec = full_record();
	uint8_t small[16];
	size_t len;

	/* A truncated record would decode as corrupt at best and as a different record at
	 * worst, so this has to be an error rather than a short write.
	 */
	TEST_ASSERT_EQUAL_INT(-ENOMEM, survey_record_encode(&rec, small, sizeof(small), &len));
}

static void emit_fixture(const char *name, const uint8_t *data, size_t len)
{
	printf("-----BEGIN FIXTURE %s-----\n", name);
	for (size_t i = 0; i < len; i++) {
		printf("%02x", data[i]);
	}
	printf("\n-----END FIXTURE %s-----\n", name);
}

/* Emits the exact bytes the host decoder is tested against.
 *
 * This is what makes CP4 a real cross-check rather than two independent readings of the
 * CDDL: the host golden-file test decodes these bytes, so a schema change that the
 * decoder has not caught up with fails there instead of during analysis.
 *
 * Regenerate with scripts/survey_fixture.py -- see tests/host/README.md.
 */
void test_emit_host_fixture(void)
{
	struct survey_record_data rec = full_record();
	struct survey_record_session session = full_session();
	size_t len;

	TEST_ASSERT_EQUAL_INT(0, survey_record_encode(&rec, buf, sizeof(buf), &len));
	emit_fixture("record", buf, len);

	TEST_ASSERT_EQUAL_INT(0, survey_record_session_encode(&session, buf, sizeof(buf), &len));
	emit_fixture("session", buf, len);
}

/* Provided by the test runner generator. */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
