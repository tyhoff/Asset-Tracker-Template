/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <app_version.h>
#include <net/nrf_cloud.h>
#include <modem/modem_info.h>

#include "survey_record.h"
#include "survey_session.h"

#if defined(CONFIG_APP_SURVEY_LOG_LEVEL)
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(survey_session, CONFIG_APP_SURVEY_LOG_LEVEL);
#define SESSION_WRN(...) LOG_WRN(__VA_ARGS__)
#define SESSION_INF(...) LOG_INF(__VA_ARGS__)
#else
/* Built into a native_sim unit test that does not link the survey log module. */
#define SESSION_WRN(...)
#define SESSION_INF(...)
#endif

/* Under the same mount fs_mgmt already restricts export to read-only (survey_export.c),
 * so this needs no ACL change -- it is just one more file under /att_storage.
 */
#define SURVEY_SESSION_PATH "/att_storage/SURVEY.session"

static K_MUTEX_DEFINE(session_lock);
static bool session_written;

static int write_session_file(const uint8_t *buf, size_t len)
{
	struct fs_file_t file;
	ssize_t written;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, SURVEY_SESSION_PATH, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		return ret;
	}

	written = fs_write(&file, buf, len);
	fs_close(&file);

	if (written < 0) {
		return (int)written;
	}

	return (written == (ssize_t)len) ? 0 : -EIO;
}

void survey_session_ensure_written(void)
{
	char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];
	char modem_version[MODEM_INFO_FWVER_SIZE];
	struct survey_record_session session = { 0 };
	uint8_t buf[SURVEY_RECORD_MAX_SIZE];
	size_t encoded_len;
	int err;

	k_mutex_lock(&session_lock, K_FOREVER);

	if (session_written) {
		k_mutex_unlock(&session_lock);
		return;
	}

	/* A no-op today (see modem_info.c), but part of the documented contract for
	 * modem_info_string_get() -- calling it costs nothing and keeps this file
	 * correct if that ever changes.
	 */
	(void)modem_info_init();

	err = nrf_cloud_client_id_get(device_id, sizeof(device_id));
	if (err) {
		SESSION_WRN("Cannot obtain device ID (%d); session header not written", err);
		goto out;
	}

	err = modem_info_string_get(MODEM_INFO_FW_VERSION, modem_version,
				    sizeof(modem_version));
	if (err < 0) {
		SESSION_WRN("Cannot obtain modem firmware version (%d); session header not "
			    "written", err);
		goto out;
	}

	session.device_id = device_id;
	session.app_version = APP_VERSION_STRING;
	session.modem_version = modem_version;
	session.exported_at_valid = false;

	err = survey_record_session_encode(&session, buf, sizeof(buf), &encoded_len);
	if (err) {
		SESSION_WRN("Session header encode failed (%d); not written", err);
		goto out;
	}

	err = write_session_file(buf, encoded_len);
	if (err) {
		SESSION_WRN("Session header write failed (%d)", err);
		goto out;
	}

	session_written = true;
	SESSION_INF("Session header written: device=%s app=%s modem=%s", device_id,
		    APP_VERSION_STRING, modem_version);

out:
	k_mutex_unlock(&session_lock);
}
