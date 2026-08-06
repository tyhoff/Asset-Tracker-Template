/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SURVEY_H_
#define _SURVEY_H_

#include <stdint.h>

#include "survey_obs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Read the clock that survey timestamps are taken against.
 *
 * Returns Unix time when the system clock has been synchronised and uptime when it has
 * not, reporting which one it gave. Kept in one place so that the observation cache and
 * the console always agree on the time base.
 *
 * @param[out] now_ms     Current time in milliseconds.
 * @param[out] time_base  Which clock @p now_ms is against.
 */
void survey_time_now(int64_t *now_ms, enum survey_time_base *time_base);

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_H_ */
