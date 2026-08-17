/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SURVEY_SESSION_H_
#define _SURVEY_SESSION_H_

/**
 * @file survey_session.h
 * @brief Once-per-boot session header (FW-8).
 *
 * A dataset exported from a device with no session header cannot be attributed to a
 * device, an app build, or a modem firmware version -- three drives collected on
 * different hardware or after an OTA look identical on the wire. survey_record_session
 * and its encoder already exist (survey_record.h); this file is the missing "session
 * started" boundary that calls them.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write the session header to flash, once per boot.
 *
 * Idempotent: every call after the first in a boot returns immediately having done
 * nothing. Safe from any thread.
 *
 * Failure is logged and non-fatal. A session header that could not be written costs the
 * exported dataset its attribution, not a capture cycle -- gating capture on it would
 * turn a missing device ID into a missing drive.
 */
void survey_session_ensure_written(void);

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_SESSION_H_ */
