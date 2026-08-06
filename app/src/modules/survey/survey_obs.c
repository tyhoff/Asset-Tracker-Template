/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <modem/lte_lc.h>

#include "survey_obs.h"

/* The modem reports RSRP and RSRQ as 3GPP index values, not as dBm/dB -- lte_lc passes
 * the indices through unconverted. The ground-fix API expects dBm/dB, so a conversion
 * has to happen somewhere. Doing it here, in the rendering path only, keeps the raw
 * index available for the record format; both are printed so the two can be checked
 * against each other on target before the record schema is frozen.
 *
 * Formulas are those of RSRP_IDX_TO_DBM() and RSRQ_IDX_TO_DB() in modem/modem_info.h,
 * duplicated rather than included because that header pulls in cJSON, which is not
 * available on native_sim and would put this unit out of reach of the unit tests.
 */
#define SURVEY_RSRP_IDX_TO_DBM(idx) ((idx) < 0 ? (idx) - 140 : (idx) - 141)

static double survey_rsrq_idx_to_db(int idx)
{
	if (idx < 0) {
		return ((double)idx - 39) * 0.5;
	}
	if (idx < 35) {
		return ((double)idx - 40) * 0.5;
	}
	return ((double)idx - 41) * 0.5;
}

/* Absence predicates.
 *
 * The struct these read from is zero-initialised before every scan and is only partly
 * filled in: a Wi-Fi-only result leaves the whole cell block at zero. Rendering those
 * zeroes as measurements would fabricate a serving cell at -141 dBm out of nothing, so
 * every field that cannot legitimately be zero is reported as "absent" instead.
 */
static bool eci_absent(uint32_t eci)
{
	return eci == LTE_LC_CELL_EUTRAN_ID_INVALID || eci == 0;
}

static bool earfcn_absent(uint32_t earfcn)
{
	/* EARFCN 0 is valid in 3GPP numbering but is what an uninitialised struct holds,
	 * and no deployed E-UTRA band uses it, so treat it as unavailable.
	 */
	return earfcn == 0 || earfcn > LTE_LC_CELL_EARFCN_MAX;
}

static bool rsrp_absent(int16_t rsrp)
{
	/* lte_lc.h documents index 0 as "not used", so it is never a measurement. */
	return rsrp == LTE_LC_CELL_RSRP_INVALID || rsrp == 0;
}

static bool rsrq_absent(int16_t rsrq)
{
	/* As for RSRP, index 0 is documented as "not used". */
	return rsrq == LTE_LC_CELL_RSRQ_INVALID || rsrq == 0;
}

static bool adv_absent(uint16_t adv)
{
	/* Timing advance is only measured in RRC-connected state; in idle/PSM the modem
	 * reports LTE_LC_CELL_TIMING_ADVANCE_INVALID. 0 is likewise treated as
	 * unavailable: it is the uninitialised value and this application does not hold
	 * the modem connected to obtain a real one.
	 *
	 * A timing advance of 0 is nominally valid, for a device right at the tower, so
	 * this discards a real reading in that one case. That is deliberate: adv is not a
	 * goal of this application, and reporting an uninitialised 0 as a measurement
	 * would be the worse error.
	 */
	return adv == LTE_LC_CELL_TIMING_ADVANCE_INVALID || adv == 0;
}

static const char *time_base_str(enum survey_time_base base)
{
	switch (base) {
	case SURVEY_TIME_BASE_UPTIME:
		return "uptime";
	case SURVEY_TIME_BASE_UNIX:
		return "unix";
	default:
		return "none";
	}
}

/* Cached observation. Written from the publishing context of location_chan, read from
 * whichever thread renders it, hence the mutex.
 */
static struct survey_observation obs;
static K_MUTEX_DEFINE(obs_lock);

void survey_obs_reset(void)
{
	k_mutex_lock(&obs_lock, K_FOREVER);
	memset(&obs, 0, sizeof(obs));
	k_mutex_unlock(&obs_lock);
}

void survey_obs_update(const struct location_msg *msg, int64_t now_ms,
		       enum survey_time_base time_base)
{
	if (msg == NULL) {
		return;
	}

	k_mutex_lock(&obs_lock, K_FOREVER);

	switch (msg->type) {
	case LOCATION_GNSS_DATA:
		obs.synthetic = false;
		obs.gnss_count++;
		obs.gnss = msg->gnss_data;
		obs.gnss_timestamp = msg->timestamp;
		obs.gnss_time_base = time_base;
		obs.gnss_valid = true;
		break;

	case LOCATION_CLOUD_REQUEST:
		obs.synthetic = false;
		obs.scan_count++;
		obs.scan = msg->cloud_request;
		/* Stamped on receipt: the location module does not timestamp cloud
		 * requests. This is an upper bound on the true measurement time.
		 */
		obs.scan_timestamp = now_ms;
		obs.scan_time_base = time_base;
		obs.scan_valid = true;
		break;

	default:
		/* Not an observation. */
		break;
	}

	k_mutex_unlock(&obs_lock);
}

void survey_obs_mark_synthetic(void)
{
	k_mutex_lock(&obs_lock, K_FOREVER);
	obs.synthetic = true;
	k_mutex_unlock(&obs_lock);
}

void survey_obs_snapshot(struct survey_observation *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&obs_lock, K_FOREVER);
	*out = obs;
	k_mutex_unlock(&obs_lock);
}

/* Render one line of "<name> <value>" or "<name> absent", for the signal-quality pair
 * shared by serving, GCI and neighbor cells.
 */
static void format_signal(survey_print_fn print, void *ctx, const char *indent, int16_t rsrp,
			  int16_t rsrq)
{
	if (rsrp_absent(rsrp)) {
		print(ctx, "%srsrp absent", indent);
	} else {
		print(ctx, "%srsrp %d (idx) = %d dBm", indent, rsrp,
		      SURVEY_RSRP_IDX_TO_DBM((int)rsrp));
	}

	if (rsrq_absent(rsrq)) {
		print(ctx, "%srsrq absent", indent);
	} else {
		print(ctx, "%srsrq %d (idx) = %.1f dB", indent, rsrq,
		      survey_rsrq_idx_to_db((int)rsrq));
	}
}

/* True when the cell block holds no measurement at all, which is the normal case for a
 * Wi-Fi-only scan result.
 */
static bool cell_empty(const struct location_cell_info *cell)
{
	return eci_absent(cell->id) && earfcn_absent(cell->earfcn) &&
	       rsrp_absent(cell->rsrp) && rsrq_absent(cell->rsrq);
}

static void format_identified_cell(survey_print_fn print, void *ctx, const char *indent,
				   const struct location_cell_info *cell)
{
	if (cell_empty(cell)) {
		print(ctx, "%sno cellular data in this scan", indent);
		return;
	}

	if (eci_absent(cell->id)) {
		/* Without a cell identity, mcc/mnc/tac are not meaningful either. */
		print(ctx, "%seci absent (cell not identified)", indent);
	} else {
		print(ctx, "%seci %u", indent, cell->id);
		print(ctx, "%smcc %d mnc %d tac %u", indent, cell->mcc, cell->mnc, cell->tac);
	}

	if (earfcn_absent(cell->earfcn)) {
		print(ctx, "%searfcn absent", indent);
	} else {
		print(ctx, "%searfcn %u", indent, cell->earfcn);
	}

	format_signal(print, ctx, indent, cell->rsrp, cell->rsrq);

	if (adv_absent(cell->timing_advance)) {
		print(ctx, "%sadv absent", indent);
	} else {
		print(ctx, "%sadv %u", indent, cell->timing_advance);
	}

	/* struct location_cell_info carries no PCI, so the serving cell and every GCI
	 * cell lose their physical cell ID on the way through the location module even
	 * though the modem reports it. Adding the field is the next checkpoint; until
	 * then this line is the reminder that the gap is real and not an empty scan.
	 */
	print(ctx, "%spci not captured yet (firmware limitation)", indent);
}

static void format_gnss(const struct survey_observation *o, survey_print_fn print, void *ctx)
{
	if (!o->gnss_valid) {
		print(ctx, "GNSS: no fix cached");
		return;
	}

	print(ctx, "GNSS: ts %lld (%s)", (long long)o->gnss_timestamp,
	      time_base_str(o->gnss_time_base));
	print(ctx, "  lat %.7f lon %.7f acc %.1f m", o->gnss.latitude, o->gnss.longitude,
	      (double)o->gnss.accuracy);
	if (o->gnss.datetime.valid) {
		print(ctx, "  datetime %04u-%02u-%02u %02u:%02u:%02u.%03u UTC",
		      o->gnss.datetime.year, o->gnss.datetime.month, o->gnss.datetime.day,
		      o->gnss.datetime.hour, o->gnss.datetime.minute, o->gnss.datetime.second,
		      o->gnss.datetime.ms);
	} else {
		/* The location module warns on this but still delivers the fix. Printing the
		 * zeroed struct would look like a real date of 0000-00-00.
		 */
		print(ctx, "  datetime invalid");
	}

#if defined(CONFIG_LOCATION_DATA_DETAILS) && defined(CONFIG_LOCATION_METHOD_GNSS)
	/* alt, spd and hdg are absent from struct location_data but present in the PVT
	 * frame that LOCATION_DATA_DETAILS carries along, so no extra plumbing is needed
	 * to obtain them. Confirming that on target is a goal of this checkpoint.
	 */
	const struct nrf_modem_gnss_pvt_data_frame *pvt = &o->gnss.details.gnss.pvt_data;

	print(ctx, "  alt %.1f m spd %.2f m/s hdg %.2f deg", (double)pvt->altitude,
	      (double)pvt->speed, (double)pvt->heading);
	print(ctx, "  sats tracked %u used %u, gnss time %u ms",
	      o->gnss.details.gnss.satellites_tracked, o->gnss.details.gnss.satellites_used,
	      o->gnss.details.gnss.elapsed_time_gnss);
#else
	print(ctx, "  alt/spd/hdg unavailable (LOCATION_DATA_DETAILS disabled)");
#endif
}

static void format_scan(const struct survey_observation *o, survey_print_fn print, void *ctx)
{
	if (!o->scan_valid) {
		print(ctx, "Scan: none cached");
		return;
	}

	print(ctx, "Scan: ts %lld (%s, stamped on receipt)", (long long)o->scan_timestamp,
	      time_base_str(o->scan_time_base));

	print(ctx, "  lte[] serving cell:");
	format_identified_cell(print, ctx, "    ", &o->scan.current_cell);

	print(ctx, "  nmr[] %u neighbor(s):", o->scan.ncells_count);
	for (uint8_t i = 0; i < o->scan.ncells_count; i++) {
		const struct location_neighbor_cell_info *n = &o->scan.neighbor_cells[i];

		print(ctx, "    [%u] pci %u", i, n->phys_cell_id);
		if (earfcn_absent(n->earfcn)) {
			print(ctx, "        earfcn absent");
		} else {
			print(ctx, "        earfcn %u", n->earfcn);
		}
		format_signal(print, ctx, "        ", n->rsrp, n->rsrq);
		print(ctx, "        timeDiff %d", n->time_diff);
	}

	print(ctx, "  gci %u full-identity cell(s):", o->scan.gci_cells_count);
	for (uint8_t i = 0; i < o->scan.gci_cells_count; i++) {
		print(ctx, "    [%u]", i);
		format_identified_cell(print, ctx, "        ", &o->scan.gci_cells[i]);
	}

	print(ctx, "  wifi.accessPoints[] %u:", o->scan.wifi_cnt);
	for (uint16_t i = 0; i < o->scan.wifi_cnt; i++) {
		const struct location_wifi_ap_info *ap = &o->scan.wifi_aps[i];

		print(ctx, "    [%u] macAddress %02X:%02X:%02X:%02X:%02X:%02X (len %u)", i,
		      ap->mac[0], ap->mac[1], ap->mac[2], ap->mac[3], ap->mac[4], ap->mac[5],
		      ap->mac_length);
		print(ctx, "        signalStrength %d dBm", ap->rssi);
		/* channel and frequency are reported by the Wi-Fi driver but dropped by
		 * struct location_wifi_ap_info. Added in the next checkpoint.
		 */
		print(ctx, "        channel/frequency/band not captured yet "
			   "(firmware limitation)");
	}
}

void survey_obs_format(const struct survey_observation *o, survey_print_fn print, void *ctx)
{
	if (o == NULL || print == NULL) {
		return;
	}

	if (o->synthetic) {
		print(ctx, "*** SYNTHETIC SELFTEST DATA -- NOT A REAL OBSERVATION ***");
		print(ctx, "*** run \"survey clear\" to discard it ***");
	}

	print(ctx, "Survey observation (gnss #%u, scan #%u)", o->gnss_count, o->scan_count);
	format_gnss(o, print, ctx);
	format_scan(o, print, ctx);
}

/* Report how old a cached timestamp is, which is what an operator in a moving vehicle
 * actually needs to know -- an absolute epoch-ms value does not answer it.
 */
static void format_age(survey_print_fn print, void *ctx, const char *label, bool valid,
		       int64_t timestamp, int64_t now_ms)
{
	if (!valid) {
		print(ctx, "%s: none cached", label);
		return;
	}

	if (now_ms == 0 || now_ms < timestamp) {
		print(ctx, "%s: cached, age unknown", label);
		return;
	}

	print(ctx, "%s: cached %lld s ago", label, (long long)((now_ms - timestamp) / 1000));
}

void survey_obs_format_stats(const struct survey_observation *o, int64_t now_ms,
			     survey_print_fn print, void *ctx)
{
	if (o == NULL || print == NULL) {
		return;
	}

	if (o->synthetic) {
		print(ctx, "*** SYNTHETIC SELFTEST DATA -- NOT A REAL OBSERVATION ***");
	}

	print(ctx, "gnss fixes observed : %u", o->gnss_count);
	print(ctx, "scans observed      : %u", o->scan_count);
	format_age(print, ctx, "gnss                ", o->gnss_valid, o->gnss_timestamp, now_ms);
	format_age(print, ctx, "scan                ", o->scan_valid, o->scan_timestamp, now_ms);
	print(ctx, "time base           : gnss %s, scan %s", time_base_str(o->gnss_time_base),
	      time_base_str(o->scan_time_base));
	print(ctx, "caps                : wifi_aps %d, neighbor_cells %d",
	      CONFIG_APP_LOCATION_WIFI_APS_MAX, CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX);
}
