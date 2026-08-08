/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SURVEY_ABSENT_H_
#define _SURVEY_ABSENT_H_

/**
 * @file survey_absent.h
 * @brief Which modem measurements count as "not measured".
 *
 * Shared by the console renderer and the CBOR encoder so that the two cannot disagree.
 * If they did, a field could print as "absent" and still be stored as a number, or the
 * reverse -- and the stored dataset is the thing being validated.
 *
 * The struct these read from is zero-initialised before every scan and is only partly
 * filled in: a Wi-Fi-only result leaves the whole cell block at zero. Treating those
 * zeroes as measurements would fabricate a serving cell at -141 dBm out of nothing, so
 * every field that cannot legitimately be zero is treated as unavailable instead.
 */

#include <stdbool.h>
#include <stdint.h>

#include <modem/lte_lc.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool survey_eci_absent(uint32_t eci)
{
	return eci == LTE_LC_CELL_EUTRAN_ID_INVALID || eci == 0;
}

static inline bool survey_earfcn_absent(uint32_t earfcn)
{
	/* EARFCN 0 is valid in 3GPP numbering but is what an uninitialised struct holds,
	 * and no deployed E-UTRA band uses it, so treat it as unavailable.
	 */
	return earfcn == 0 || earfcn > LTE_LC_CELL_EARFCN_MAX;
}

static inline bool survey_rsrp_absent(int16_t rsrp)
{
	/* lte_lc.h documents index 0 as "not used", so it is never a measurement. */
	return rsrp == LTE_LC_CELL_RSRP_INVALID || rsrp == 0;
}

static inline bool survey_rsrq_absent(int16_t rsrq)
{
	/* As for RSRP, index 0 is documented as "not used". */
	return rsrq == LTE_LC_CELL_RSRQ_INVALID || rsrq == 0;
}

static inline bool survey_adv_absent(uint16_t adv)
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

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_ABSENT_H_ */
