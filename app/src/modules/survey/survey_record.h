/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SURVEY_RECORD_H_
#define _SURVEY_RECORD_H_

/**
 * @file survey_record.h
 * @brief CBOR encoding of one capture cycle.
 *
 * The wire format is defined by survey_record.cddl, which is the authority; this header
 * only describes what the application hands the encoder. Absent fields are omitted from
 * the encoding rather than written as zero, because zero is a legitimate reading for
 * several of them.
 *
 * The capture payload reuses the location module's own types rather than a parallel set
 * of structs, so the data is encoded from the same representation the rest of the
 * application already carries.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "location.h"
#include "survey_obs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Schema version written into every record and session header.
 *
 * Bump on any incompatible change. The host decoder dispatches on this and refuses
 * versions it does not know rather than guessing.
 */
#define SURVEY_RECORD_VERSION 1

/** FW-5's design budget for one record, in bytes.
 *
 * This is the number storage capacity is planned from, and the encoder's unit test
 * asserts a fully populated DEEP record against it -- 10 APs, 10 neighbours, 3 GCI cells
 * and both bracketing fixes measured at 583 B. A schema change that pushes past this is a
 * capacity decision about how many days of data fit on the device, so it should fail a
 * build rather than turn up during a drive.
 */
#define SURVEY_RECORD_BUDGET_SIZE 700

/** Buffer size for one encoded record.
 *
 * Headroom over the budget, so that a record which overshoots is reported as an
 * out-of-budget failure by the test rather than as a truncated encode at runtime.
 */
#define SURVEY_RECORD_MAX_SIZE 1024

/** Capture profile, stored so analysis can separate the two populations. */
enum survey_profile {
	SURVEY_PROFILE_FAST = 0,
	SURVEY_PROFILE_DEEP = 1,
};

/** Radio access technology the cell observations were taken on. */
enum survey_network_mode {
	SURVEY_NETWORK_MODE_UNKNOWN = 0,
	SURVEY_NETWORK_MODE_LTEM = 1,
	SURVEY_NETWORK_MODE_NBIOT = 2,
};

/** An optional millisecond offset from the record's time base.
 *
 * A validity flag rather than a sentinel: 0 is a legitimate offset, so there is no value
 * that could stand in for "not recorded".
 */
struct survey_record_offset {
	bool valid;
	int32_t ms;
};

/** One capture cycle, as the orchestrator assembles it.
 *
 * The bracketing fixes are both optional. A cycle whose GNSS failed is still worth
 * storing -- the radio observations remain valid, they just cannot be scored against
 * ground truth, and the decoder refuses to interpolate when a bracket is missing.
 */
struct survey_record_data {
	/** Monotonic within a session. */
	uint32_t sequence;
	enum survey_profile profile;
	/** Which clock @c t_base_ms is against. Uptime records cannot be correlated
	 *  off-device, so the base travels with the data rather than being assumed.
	 */
	enum survey_time_base time_base;
	/** Absolute time of the first GNSS fix; every offset below is relative to it. */
	int64_t t_base_ms;

	bool gnss_before_valid;
	struct location_data gnss_before;
	bool gnss_after_valid;
	struct location_data gnss_after;

	struct survey_record_offset cell_start;
	struct survey_record_offset cell_end;
	struct survey_record_offset wifi_start;
	struct survey_record_offset wifi_end;
	struct survey_record_offset gnss_after_at;

	bool scan_valid;
	struct location_cloud_request_data scan;

	enum survey_network_mode network_mode;
};

/** Identifies a dataset. Written once, before any records. */
struct survey_record_session {
	const char *device_id;
	const char *app_version;
	/** As reported by AT+CGMR, e.g. "mfw_nrf91x1_2.0.4". */
	const char *modem_version;
	/** Epoch ms. Omitted from the encoding when @c exported_at_valid is false. */
	bool exported_at_valid;
	int64_t exported_at_ms;
};

/** @brief Build a record from a cached observation.
 *
 * Exists so the encoder can be exercised on target before the capture orchestrator
 * lands. The cache holds the latest fix and the latest scan independently rather than as
 * a matched pair, so the result carries only a leading fix: there is no second bracketing
 * fix, and no measurement offsets, because the cache records when the application
 * observed a scan and not when the modem measured it.
 *
 * Everything the cache cannot supply is left absent rather than filled with a plausible
 * value, so a record built this way is honestly distinguishable from one the orchestrator
 * produced.
 *
 * @param obs       Snapshot from @c survey_obs_snapshot.
 * @param sequence  Sequence number to stamp into the record.
 * @param out       Record to populate. Fully overwritten.
 */
void survey_record_from_obs(const struct survey_observation *obs, uint32_t sequence,
			    struct survey_record_data *out);

/** @brief Encode one capture cycle.
 *
 * @param rec      Cycle to encode.
 * @param buf      Destination buffer.
 * @param buf_len  Size of @p buf. SURVEY_RECORD_MAX_SIZE is always sufficient.
 * @param out_len  Set to the number of bytes written on success.
 *
 * @retval 0 on success.
 * @retval -EINVAL if any pointer argument is NULL.
 * @retval -ENOMEM if @p buf was too small.
 */
int survey_record_encode(const struct survey_record_data *rec, uint8_t *buf, size_t buf_len,
			 size_t *out_len);

/** @brief Encode the session header.
 *
 * Arguments and return values as for @c survey_record_encode.
 */
int survey_record_session_encode(const struct survey_record_session *session, uint8_t *buf,
				 size_t buf_len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_RECORD_H_ */
