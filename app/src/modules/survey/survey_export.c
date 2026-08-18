/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file survey_export.c
 * @brief CP8 / FW-8 -- restrict MCUmgr fs_mgmt to read-only access under /att_storage.
 *
 * Without this, fs_mgmt is an unauthenticated write primitive on a device an untrained
 * operator plugs into their laptop: "fs upload" can write any path the filesystem
 * accepts, including the header files whose offsets the storage backend and the sequence
 * recovery in survey_store.c both trust without validating. This file is the only thing
 * standing between that and a corrupted partition.
 *
 * The policy is deliberately simple and denies by default: only a read or a checksum/hash
 * request is allowed, and only under /att_storage -- everything else (upload, a status
 * query) is rejected outright. There is no case in the collection workflow -- the
 * technical fallback script or the non-technical browser page -- that needs anything else.
 * "survey clear" already exists as a shell command for the one legitimate write this
 * partition needs, gated to a console session rather than exposed over the export
 * transport.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>

#if defined(CONFIG_APP_SURVEY_LOG_LEVEL)
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(survey_export, CONFIG_APP_SURVEY_LOG_LEVEL);
#define EXPORT_WRN(...) LOG_WRN(__VA_ARGS__)
#else
#define EXPORT_WRN(...)
#endif

/* uart-mcumgr and modem tracing both want uart1 on this board (see
 * overlay-survey-export.overlay), and nothing enforces at build time that only one of them
 * claims it. A build that turned both on would not fail to compile or to link; it would
 * flash, and then either survey export or modem trace would silently get nothing -- the
 * same failure class as the CONFIG_UART_MCUMGR lesson in docs/common/dev_workflow.md, and
 * exactly the kind of thing this file exists to convert into a compile error instead of a
 * discovery three transports deep into a debugging session.
 */
BUILD_ASSERT(!(IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_UART) &&
		IS_ENABLED(CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART)),
	     "CONFIG_MCUMGR_TRANSPORT_UART and modem tracing over UART both claim uart1; "
	     "a build cannot enable both.");

#define SURVEY_EXPORT_MOUNT_POINT "/att_storage"
#define SURVEY_EXPORT_MOUNT_POINT_LEN (sizeof(SURVEY_EXPORT_MOUNT_POINT) - 1)

static bool path_under_mount(const char *path)
{
	if (path == NULL) {
		return false;
	}

	/* Exact prefix match plus a following '/' -- not strncmp alone, which would also
	 * accept "/att_storage_evil" as being "under" "/att_storage". A ".." component
	 * is rejected outright rather than resolved: this build only ever mounts one
	 * filesystem at this mount point, so "/att_storage/../x" cannot currently walk
	 * anywhere else, but that is a property of today's mount table, not of this
	 * check, and the check should not depend on it staying that way.
	 */
	return strncmp(path, SURVEY_EXPORT_MOUNT_POINT, SURVEY_EXPORT_MOUNT_POINT_LEN) == 0 &&
	       path[SURVEY_EXPORT_MOUNT_POINT_LEN] == '/' &&
	       strstr(path, "..") == NULL;
}

static enum mgmt_cb_return survey_export_file_access(uint32_t event,
						      enum mgmt_cb_return prev_status,
						      int32_t *rc, uint16_t *group,
						      bool *abort_more, void *data,
						      size_t data_size)
{
	const struct fs_mgmt_file_access *access = data;

	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);

	if (event != MGMT_EVT_OP_FS_MGMT_FILE_ACCESS || data_size < sizeof(*access)) {
		return MGMT_CB_OK;
	}

	if ((access->access == FS_MGMT_FILE_ACCESS_READ ||
	     access->access == FS_MGMT_FILE_ACCESS_HASH_CHECKSUM) &&
	    path_under_mount(access->filename)) {
		return MGMT_CB_OK;
	}

	EXPORT_WRN("Denied fs_mgmt access type %d to \"%s\"", access->access,
		   access->filename ? access->filename : "(null)");

	*abort_more = true;
	*rc = MGMT_ERR_EACCESSDENIED;

	return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback survey_export_callback = {
	.callback = survey_export_file_access,
	.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS,
};

static int survey_export_init(void)
{
	mgmt_callback_register(&survey_export_callback);

	return 0;
}

/* POST_KERNEL, not APPLICATION: Zephyr's own mcumgr internals -- smp_uart_init() and
 * mcumgr's group registration -- run at APPLICATION/CONFIG_APPLICATION_INIT_PRIORITY too,
 * and same-level-and-priority SYS_INIT entries run in link order, which nothing here
 * controls. Sharing that level would make "is the deny-by-default hook registered before
 * the transport can serve a request" a coin flip decided by the linker rather than a
 * guarantee -- exactly the failure this file exists to prevent. POST_KERNEL entirely
 * precedes APPLICATION regardless of link order, so this is unconditionally registered
 * first no matter what else runs at the default application priority.
 */
SYS_INIT(survey_export_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
