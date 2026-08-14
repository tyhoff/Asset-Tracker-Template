/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/wifi.h>
#include <modem/lte_lc.h>

#include "survey_absent.h"
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

/* IEEE 802.11 channel to frequency. struct wifi_scan_result has no frequency field, so it
 * is derived for display rather than stored. Channel numbers repeat across bands, hence the
 * band parameter. Out-of-range pairs return 0 rather than a fabricated frequency; the driver
 * does not validate the band.
 */
static uint16_t wifi_frequency_mhz(uint8_t band, uint8_t channel)
{
	switch (band) {
	case WIFI_FREQ_BAND_2_4_GHZ:
		if (channel == 14) {
			return 2484;
		}
		return (channel >= 1 && channel <= 13) ? 2407 + (5 * channel) : 0;
	case WIFI_FREQ_BAND_5_GHZ:
		/* 184-196 is the Japanese 4.9 GHz band, which uses a 4000 MHz base. */
		return (channel >= 32 && channel <= 177) ? 5000 + (5 * channel) : 0;
	case WIFI_FREQ_BAND_6_GHZ:
		if (channel == 2) {
			return 5935;
		}
		return (channel >= 1 && channel <= 233) ? 5950 + (5 * channel) : 0;
	default:
		return 0;
	}
}

/* Not wifi_band_txt(): that lives in the Wi-Fi L2, which native_sim does not link. */
static const char *wifi_band_str(uint8_t band)
{
	switch (band) {
	case WIFI_FREQ_BAND_2_4_GHZ:
		return "2.4GHz";
	case WIFI_FREQ_BAND_5_GHZ:
		return "5GHz";
	case WIFI_FREQ_BAND_6_GHZ:
		return "6GHz";
	default:
		return "unknown";
	}
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

/* Bit 1 of the first octet of a MAC address: set means locally administered rather than
 * assigned from an OUI. IEEE 802 calls it the U/L bit.
 */
#define MAC_LOCALLY_ADMINISTERED BIT(1)

/* Drop access points whose BSSID is locally administered, compacting the array in place.
 * Returns the surviving and removed counts.
 *
 * These are randomised BSSIDs -- phone and laptop hotspots, and Wi-Fi Direct and CarPlay
 * links, which re-randomise per session. They are not stable, so they cannot be resolved
 * to a position by any positioning service, and the survey exists to be checked against
 * one. Keeping them costs record space and puts measurements into the dataset that no
 * consumer can do anything with. A survey taken in a train carriage or a car park is
 * mostly them.
 *
 * What this does NOT do is recover access points that were crowded out. The Location
 * library caps its result set at CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT (10
 * on this board) before it publishes, and location_helper.c rejects anything longer with
 * -ENOMEM rather than truncating, so by the time a scan reaches this cache the real access
 * points that the randomised ones displaced are already gone. The measured 4-in-10 spent 4
 * of those 10 upstream slots. Recovering them means raising that cap and
 * CONFIG_APP_LOCATION_WIFI_APS_MAX with it, which is a separate change with its own
 * heap and record-size consequences -- filtering here cannot substitute for it.
 *
 * Filtered here, where every scan lands, rather than in the encoder: the console commands
 * read this same cache, so filtering at encode time would leave "survey show" listing
 * access points that are not in the stored record, which is the kind of disagreement that
 * costs a bench session to work out.
 *
 * Only the U/L bit. The multicast bit (bit 0) would make the address malformed as a BSSID
 * rather than merely unstable, and nothing has been seen to report one; leaving it alone
 * keeps this function about one well-defined thing.
 */
static struct survey_obs_scan_counts drop_local_mac_aps(struct location_cloud_request_data *scan)
{
	/* Clamped rather than trusted. Every producer in the tree already bounds this
	 * (location_helper.c rejects an over-long count with -ENOMEM, cloud_location.c
	 * validates), so an out-of-range wifi_cnt is unreachable today -- but this is the
	 * only place in the module that *writes* based on the count, and the two writes
	 * below would run off the end of a fixed array inside a static struct. A read that
	 * overruns prints nonsense; a write that overruns corrupts the neighbouring fields.
	 *
	 * No cast on ARRAY_SIZE: narrowing it to uint16_t to match the operand types would
	 * evaluate to 0 for an array longer than 65535 and silently discard every access
	 * point. Widening wifi_cnt instead makes the comparison safe at any array size, and
	 * the result is provably <= wifi_cnt, so the narrowing on assignment cannot lose.
	 */
	const uint16_t count = (uint16_t)MIN((size_t)scan->wifi_cnt, ARRAY_SIZE(scan->wifi_aps));
	uint16_t kept = 0;
	uint16_t dropped = 0;

	/* Applied before the filter can decline to run, so the bound holds in both
	 * configurations. A clamp that exists only when DROP_LOCAL_MAC=y would be worse than
	 * no clamp at all: format_scan() reads up to wifi_cnt in either build, and a safety
	 * property that silently depends on an unrelated Kconfig is one nobody will check.
	 */
	scan->wifi_cnt = count;

	if (!IS_ENABLED(CONFIG_APP_SURVEY_DROP_LOCAL_MAC)) {
		return (struct survey_obs_scan_counts){ .kept = count, .dropped = 0 };
	}

	for (uint16_t i = 0; i < count; i++) {
		if (scan->wifi_aps[i].mac[0] & MAC_LOCALLY_ADMINISTERED) {
			dropped++;
			continue;
		}

		/* Self-assignment when nothing has been dropped yet, which is the common
		 * case and is cheaper than branching around it.
		 */
		scan->wifi_aps[kept] = scan->wifi_aps[i];
		kept++;
	}

	/* Zeroed rather than left behind the new count. Nothing should read past wifi_cnt,
	 * but a stale BSSID sitting in the tail of a cached structure is exactly the sort
	 * of thing that reappears in a hex dump and takes an afternoon to explain.
	 *
	 * The length comes from the array, not from the drop count, and the whole remainder
	 * goes rather than just the gap the compaction opened. That is deliberate: kept is
	 * bounded by count which is bounded by ARRAY_SIZE, so this expression cannot address
	 * past the end whatever the counters do, whereas a dropped-derived length can -- and
	 * an overrun here is currently unobservable from outside. Measured from the DWARF for
	 * this build: wifi_aps is the last member of the scan and ends at offset 958 of
	 * struct survey_observation, then 2 bytes of padding, then scan_local_mac_dropped at
	 * 960, then 6 bytes of tail padding to the 968-byte total. A one-entry overrun is 10
	 * bytes and lands in exactly those 2 + 2 + 6 -- it never leaves the struct, and the
	 * only live bytes it touches are a counter survey_obs_update() overwrites on the
	 * following line. So no test sees it and ASAN cannot redzone it either.
	 *
	 * That is a coincidence with zero bytes of margin, not a safety property. Append one
	 * member after scan_local_mac_dropped, or change CONFIG_APP_LOCATION_WIFI_APS_MAX,
	 * and the same slip becomes a real write past the end of a .bss object. Either way
	 * the answer is the same: the arithmetic has to not exist.
	 */
	memset(&scan->wifi_aps[kept], 0,
	       (ARRAY_SIZE(scan->wifi_aps) - kept) * sizeof(scan->wifi_aps[0]));

	scan->wifi_cnt = kept;

	return (struct survey_obs_scan_counts){ .kept = kept, .dropped = dropped };
}

struct survey_obs_scan_counts survey_obs_update(const struct location_msg *msg, int64_t now_ms,
						enum survey_time_base time_base)
{
	struct survey_obs_scan_counts counts = { 0 };

	if (msg == NULL) {
		return counts;
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
		counts = drop_local_mac_aps(&obs.scan);
		obs.scan_local_mac_dropped = counts.dropped;
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

	return counts;
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
	if (survey_rsrp_absent(rsrp)) {
		print(ctx, "%srsrp absent", indent);
	} else {
		print(ctx, "%srsrp %d (idx) = %d dBm", indent, rsrp,
		      SURVEY_RSRP_IDX_TO_DBM((int)rsrp));
	}

	if (survey_rsrq_absent(rsrq)) {
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
	return survey_eci_absent(cell->id) && survey_earfcn_absent(cell->earfcn) &&
	       survey_rsrp_absent(cell->rsrp) && survey_rsrq_absent(cell->rsrq);
}

static void format_identified_cell(survey_print_fn print, void *ctx, const char *indent,
				   const struct location_cell_info *cell)
{
	if (cell_empty(cell)) {
		print(ctx, "%sno cellular data in this scan", indent);
		return;
	}

	if (survey_eci_absent(cell->id)) {
		/* Without a cell identity, mcc/mnc/tac are not meaningful either. */
		print(ctx, "%seci absent (cell not identified)", indent);
	} else {
		print(ctx, "%seci %u", indent, cell->id);
		print(ctx, "%smcc %d mnc %d tac %u", indent, cell->mcc, cell->mnc, cell->tac);
	}

	if (survey_earfcn_absent(cell->earfcn)) {
		print(ctx, "%searfcn absent", indent);
	} else {
		print(ctx, "%searfcn %u", indent, cell->earfcn);
	}

	format_signal(print, ctx, indent, cell->rsrp, cell->rsrq);

	if (survey_adv_absent(cell->timing_advance)) {
		print(ctx, "%sadv absent", indent);
	} else {
		print(ctx, "%sadv %u", indent, cell->timing_advance);
	}

	/* PCI is deliberately not captured for identified cells: mcc/mnc/eci/tac already
	 * identify the cell globally, so the physical cell ID adds nothing a lookup can
	 * use. Neighbors, which have no identity, do carry it. Stated explicitly so the
	 * absence does not read as a scan failure.
	 */
	print(ctx, "%spci not captured (not needed for an identified cell)", indent);
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
		if (survey_earfcn_absent(n->earfcn)) {
			print(ctx, "        earfcn absent");
		} else {
			print(ctx, "        earfcn %u", n->earfcn);
		}
		format_signal(print, ctx, "        ", n->rsrp, n->rsrq);
		if (survey_time_diff_absent(n->time_diff)) {
			print(ctx, "        timeDiff absent");
		} else {
			print(ctx, "        timeDiff %d", n->time_diff);
		}
	}

	print(ctx, "  gci %u full-identity cell(s):", o->scan.gci_cells_count);
	for (uint8_t i = 0; i < o->scan.gci_cells_count; i++) {
		print(ctx, "    [%u]", i);
		format_identified_cell(print, ctx, "        ", &o->scan.gci_cells[i]);
	}

	/* Printed only when something was dropped, so that the common line stays the one
	 * an operator already knows how to read. Printed at all because "3 access points"
	 * in a busy place otherwise looks like a broken scan rather than a working filter.
	 */
	if (o->scan_local_mac_dropped > 0) {
		print(ctx, "  wifi: %u locally-administered BSSID(s) dropped (randomised, "
			   "not resolvable to a position)", o->scan_local_mac_dropped);
	}

	print(ctx, "  wifi.accessPoints[] %u:", o->scan.wifi_cnt);
	for (uint16_t i = 0; i < o->scan.wifi_cnt; i++) {
		const struct location_wifi_ap_info *ap = &o->scan.wifi_aps[i];

		print(ctx, "    [%u] macAddress %02X:%02X:%02X:%02X:%02X:%02X (len %u)", i,
		      ap->mac[0], ap->mac[1], ap->mac[2], ap->mac[3], ap->mac[4], ap->mac[5],
		      ap->mac_length);
		print(ctx, "        signalStrength %d dBm", ap->rssi);

		/* Channel 0 is not a valid Wi-Fi channel, so it is the driver's "not
		 * reported" value; band alone would be meaningless without it.
		 */
		if (ap->channel == 0) {
			print(ctx, "        channel absent");
		} else {
			uint16_t freq = wifi_frequency_mhz(ap->band, ap->channel);

			if (freq == 0) {
				print(ctx, "        channel %u frequency undetermined "
					   "(band %u unrecognised for this channel)",
				      ap->channel, ap->band);
			} else {
				print(ctx, "        channel %u frequency %u MHz band %s",
				      ap->channel, freq, wifi_band_str(ap->band));
			}
		}
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
	/* The BSSID filter is a compile-time choice, and until it is reported here there is
	 * no way to ask a flashed device which way it was built. The integration suite needs
	 * that to decide whether "no randomised BSSID survived" is a requirement or a
	 * misconfiguration, and an operator comparing two boards needs it more.
	 */
	print(ctx, "caps                : wifi_aps %d, neighbor_cells %d, drop_local_mac %s",
	      CONFIG_APP_LOCATION_WIFI_APS_MAX, CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX,
	      IS_ENABLED(CONFIG_APP_SURVEY_DROP_LOCAL_MAC) ? "y" : "n");
}
