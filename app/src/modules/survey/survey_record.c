/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Maps the application's capture data onto the generated CBOR types.
 *
 * All the schema lives in survey_record.cddl; this file only decides what counts as
 * measured. Those decisions come from survey_absent.h, shared with the console, so a
 * field can never print as "absent" and still be stored as a number.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "survey_absent.h"
#include "survey_record.h"
#include "survey_record_encode.h"
#include "survey_record_types.h"

/* Scaled integers rather than floats, per the schema. Rounded away from zero so a value
 * is never biased toward the origin, and clamped because CBOR sizes are fixed: a wild
 * value from a bad fix must not wrap into a plausible-looking coordinate.
 */
static int32_t scale_clamped(double value, double scale, double lo, double hi)
{
	double scaled = round(value * scale);

	if (scaled < lo) {
		return (int32_t)lo;
	}
	if (scaled > hi) {
		return (int32_t)hi;
	}
	return (int32_t)scaled;
}

static void fill_gnss(struct gnss_fix *dst, const struct location_data *src)
{
	memset(dst, 0, sizeof(*dst));

	dst->lat_e7_m = scale_clamped(src->latitude, 1e7, -900000000.0, 900000000.0);
	dst->lon_e7_m = scale_clamped(src->longitude, 1e7, -1800000000.0, 1800000000.0);
	dst->acc_mm_m = (uint32_t)scale_clamped((double)src->accuracy, 1000.0, 0.0, 4000000000.0);

#if defined(CONFIG_LOCATION_DATA_DETAILS)
	/* PVT always carries these alongside a fix, so they are written whenever there is
	 * one. Each is a legitimate zero -- altitude at sea level, a stationary device --
	 * which is why they are optional in the schema rather than sentinel-encoded.
	 */
	const struct nrf_modem_gnss_pvt_data_frame *pvt = &src->details.gnss.pvt_data;

	dst->alt_mm_m.alt_mm_m =
		scale_clamped((double)pvt->altitude, 1000.0, -2000000000.0, 2000000000.0);
	dst->alt_mm_m_present = true;

	dst->spd_mmps_m.spd_mmps_m =
		(uint32_t)scale_clamped((double)pvt->speed, 1000.0, 0.0, 4000000000.0);
	dst->spd_mmps_m_present = true;

	dst->hdg_cdeg_m.hdg_cdeg_m = (uint32_t)scale_clamped((double)pvt->heading, 100.0, 0.0,
							     35999.0);
	dst->hdg_cdeg_m_present = true;

	dst->sats_used_m.sats_used_m = src->details.gnss.satellites_used;
	dst->sats_used_m_present = true;
#endif /* CONFIG_LOCATION_DATA_DETAILS */
}

static void fill_cell(struct cell *dst, const struct location_cell_info *src)
{
	memset(dst, 0, sizeof(*dst));

	dst->eci_m = src->id;
	dst->mcc_m = src->mcc;
	dst->mnc_m = src->mnc;
	dst->tac_m = src->tac;

	if (!survey_earfcn_absent(src->earfcn)) {
		dst->earfcn_m.earfcn_m = src->earfcn;
		dst->earfcn_m_present = true;
	}
	if (!survey_rsrp_absent(src->rsrp)) {
		dst->rsrp_idx_m.rsrp_idx_m = (uint32_t)src->rsrp;
		dst->rsrp_idx_m_present = true;
	}
	if (!survey_rsrq_absent(src->rsrq)) {
		dst->rsrq_idx_m.rsrq_idx_m = (uint32_t)src->rsrq;
		dst->rsrq_idx_m_present = true;
	}
	if (!survey_adv_absent(src->timing_advance)) {
		dst->adv_m.adv_m = src->timing_advance;
		dst->adv_m_present = true;
	}
}

static void fill_neighbour(struct neighbour *dst, const struct location_neighbor_cell_info *src)
{
	memset(dst, 0, sizeof(*dst));

	/* PCI is the only thing distinguishing one neighbour from another, so it is
	 * mandatory here where it is omitted for identified cells.
	 */
	dst->pci_m = src->phys_cell_id;

	if (!survey_earfcn_absent(src->earfcn)) {
		dst->nbr_earfcn_m.nbr_earfcn_m = src->earfcn;
		dst->nbr_earfcn_m_present = true;
	}
	if (!survey_rsrp_absent(src->rsrp)) {
		dst->nbr_rsrp_idx_m.nbr_rsrp_idx_m = (uint32_t)src->rsrp;
		dst->nbr_rsrp_idx_m_present = true;
	}
	if (!survey_rsrq_absent(src->rsrq)) {
		dst->nbr_rsrq_idx_m.nbr_rsrq_idx_m = (uint32_t)src->rsrq;
		dst->nbr_rsrq_idx_m_present = true;
	}

	/* time_diff has no invalid marker and 0 is a real reading, so it is always
	 * written when the neighbour itself was reported.
	 */
	dst->time_diff_m.time_diff_m = src->time_diff;
	dst->time_diff_m_present = true;
}

static void fill_ap(struct access_point *dst, const struct location_wifi_ap_info *src)
{
	memset(dst, 0, sizeof(*dst));

	dst->mac_m.value = src->mac;
	dst->mac_m.len = sizeof(src->mac);
	dst->rssi_dbm_m = src->rssi;

	/* Channel 0 is the driver's "not reported" value, and band is meaningless without
	 * it -- the zero band value is 2.4 GHz, not "unknown", so storing band alone would
	 * assert a measurement that was never made.
	 */
	if (src->channel != 0) {
		dst->ap_channel_m.ap_channel_m = src->channel;
		dst->ap_channel_m_present = true;
		dst->ap_band_m.ap_band_m = src->band;
		dst->ap_band_m_present = true;
	}
}

static void fill_offset(struct survey_record *dst, const struct survey_record_data *rec)
{
	if (rec->cell_start.valid) {
		dst->cell_start_ms_m.cell_start_ms_m = rec->cell_start.ms;
		dst->cell_start_ms_m_present = true;
	}
	if (rec->cell_end.valid) {
		dst->cell_end_ms_m.cell_end_ms_m = rec->cell_end.ms;
		dst->cell_end_ms_m_present = true;
	}
	if (rec->wifi_start.valid) {
		dst->wifi_start_ms_m.wifi_start_ms_m = rec->wifi_start.ms;
		dst->wifi_start_ms_m_present = true;
	}
	if (rec->wifi_end.valid) {
		dst->wifi_end_ms_m.wifi_end_ms_m = rec->wifi_end.ms;
		dst->wifi_end_ms_m_present = true;
	}
	if (rec->gnss_after_at.valid) {
		dst->gnss_after_ms_m.gnss_after_ms_m = rec->gnss_after_at.ms;
		dst->gnss_after_ms_m_present = true;
	}
}

static void fill_scan(struct survey_record *dst, const struct location_cloud_request_data *scan)
{
	size_t n;

	/* Without an identity the cell block carries nothing a lookup can use, and
	 * mcc/mnc/tac are mandatory in the schema, so the whole entry is omitted rather
	 * than written with placeholder identity fields.
	 */
	if (!survey_eci_absent(scan->current_cell.id)) {
		fill_cell(&dst->serving_cell_m.serving_cell_m, &scan->current_cell);
		dst->serving_cell_m_present = true;
	}

	n = MIN(scan->ncells_count, ARRAY_SIZE(dst->neighbour_m_l.neighbour_m));
	for (size_t i = 0; i < n; i++) {
		fill_neighbour(&dst->neighbour_m_l.neighbour_m[i], &scan->neighbor_cells[i]);
	}
	dst->neighbour_m_l.neighbour_m_count = n;
	dst->neighbour_m_l_present = (n > 0);

	n = 0;
	for (size_t i = 0; i < MIN(scan->gci_cells_count,
				   ARRAY_SIZE(dst->gci_cell_m_l.gci_cell_m)); i++) {
		if (survey_eci_absent(scan->gci_cells[i].id)) {
			continue;
		}
		fill_cell(&dst->gci_cell_m_l.gci_cell_m[n], &scan->gci_cells[i]);
		n++;
	}
	dst->gci_cell_m_l.gci_cell_m_count = n;
	dst->gci_cell_m_l_present = (n > 0);

	n = MIN(scan->wifi_cnt, ARRAY_SIZE(dst->access_point_m_l.access_point_m));
	for (size_t i = 0; i < n; i++) {
		fill_ap(&dst->access_point_m_l.access_point_m[i], &scan->wifi_aps[i]);
	}
	dst->access_point_m_l.access_point_m_count = n;
	dst->access_point_m_l_present = (n > 0);
}

int survey_record_encode(const struct survey_record_data *rec, uint8_t *buf, size_t buf_len,
			 size_t *out_len)
{
	/* File scope would need a lock; this is only reached from the storage path, on a
	 * thread whose stack is sized for it.
	 */
	static struct survey_record out;
	int err;

	if (rec == NULL || buf == NULL || out_len == NULL) {
		return -EINVAL;
	}

	memset(&out, 0, sizeof(out));

	out.version_m = SURVEY_RECORD_VERSION;
	out.sequence_m = rec->sequence;
	out.profile_m = (uint32_t)rec->profile;
	out.time_base_m = (uint32_t)rec->time_base;
	out.t_base_m = (uint64_t)rec->t_base_ms;

	if (rec->gnss_before_valid) {
		fill_gnss(&out.gnss_fix_before_m.gnss_fix_before_m, &rec->gnss_before);
		out.gnss_fix_before_m_present = true;
	}
	if (rec->gnss_after_valid) {
		fill_gnss(&out.gnss_fix_after_m.gnss_fix_after_m, &rec->gnss_after);
		out.gnss_fix_after_m_present = true;
	}

	fill_offset(&out, rec);

	if (rec->scan_valid) {
		fill_scan(&out, &rec->scan);
	}

	if (rec->network_mode != SURVEY_NETWORK_MODE_UNKNOWN) {
		out.network_mode_m.network_mode_m = (uint32_t)rec->network_mode;
		out.network_mode_m_present = true;
	}

	err = cbor_encode_survey_record(buf, buf_len, &out, out_len);
	if (err != ZCBOR_SUCCESS) {
		return -ENOMEM;
	}

	return 0;
}

int survey_record_session_encode(const struct survey_record_session *session, uint8_t *buf,
				 size_t buf_len, size_t *out_len)
{
	struct survey_session out = { 0 };
	int err;

	if (session == NULL || buf == NULL || out_len == NULL ||
	    session->device_id == NULL || session->app_version == NULL ||
	    session->modem_version == NULL) {
		return -EINVAL;
	}

	out.version_m = SURVEY_RECORD_VERSION;
	out.device_id_m.value = (const uint8_t *)session->device_id;
	out.device_id_m.len = strlen(session->device_id);
	out.app_version_m.value = (const uint8_t *)session->app_version;
	out.app_version_m.len = strlen(session->app_version);
	out.modem_version_m.value = (const uint8_t *)session->modem_version;
	out.modem_version_m.len = strlen(session->modem_version);

	if (session->exported_at_valid) {
		out.exported_at_m.exported_at_m = (uint64_t)session->exported_at_ms;
		out.exported_at_m_present = true;
	}

	err = cbor_encode_survey_session(buf, buf_len, &out, out_len);
	if (err != ZCBOR_SUCCESS) {
		return -ENOMEM;
	}

	return 0;
}
