/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "survey_obs.h"
#include "survey_record.h"
#include "survey_store.h"

#if defined(CONFIG_APP_SURVEY_LOG_LEVEL)
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(survey_store, CONFIG_APP_SURVEY_LOG_LEVEL);
#define STORE_WRN(...) LOG_WRN(__VA_ARGS__)
#else
/* Built into a native_sim unit test that does not link the survey log module. */
#define STORE_WRN(...)
#endif

/* A slot must hold a record that encodes to the design budget, with room for the length
 * prefix. Checked here rather than trusted, because SURVEY_STORE_SLOT_SIZE was chosen to
 * pack the flash block and the budget was chosen from the schema -- the two numbers have
 * no reason to stay compatible unless something says so.
 */
BUILD_ASSERT(SURVEY_STORE_PAYLOAD_MAX >= SURVEY_RECORD_BUDGET_SIZE,
	     "Survey storage slot cannot hold a budget-sized record. Either the schema grew "
	     "past SURVEY_RECORD_BUDGET_SIZE or SURVEY_STORE_SLOT_SIZE shrank.");

ZBUS_CHAN_DEFINE(survey_store_chan,
		 struct survey_store_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Publishing copies the message into the channel, so the staging buffer is only live for
 * the duration of survey_store_publish(). It is static because a struct survey_store_msg
 * is 816 bytes and the callers -- a shell thread today, the capture orchestrator later --
 * do not have that to spare on the stack. The mutex is what makes that safe.
 */
static struct survey_store_msg staging;
static K_MUTEX_DEFINE(staging_lock);

/* Also static, and for the same reason: struct survey_record_data carries two
 * location_data (each with an nrf_modem_gnss_pvt_data_frame) plus a cloud request with ten
 * APs and ten neighbours, which is over a kilobyte. survey_shell.c keeps its own copies off
 * the shell thread's stack for exactly this reason -- CONFIG_SHELL_STACK_SIZE is 2560 --
 * and this function runs on that thread. staging_lock covers it.
 */
static struct survey_record_data staged_record;

/* Called by the storage module for every message on survey_store_chan.
 *
 * Every message published here is a record that has already been encoded, so nothing
 * legitimate is filtered out. A zero-length message is the channel's ZBUS_MSG_INIT(0)
 * value and would mean survey_store_publish() published without encoding -- refuse it
 * rather than commit an empty slot to flash, and say so, because it is a bug and not a
 * status message to skip.
 */
bool survey_store_check(const struct survey_store_msg *msg)
{
	if (msg->len == 0) {
		STORE_WRN("Zero-length survey message on survey_store_chan; not stored");

		return false;
	}

	return true;
}

void survey_store_extract(const struct survey_store_msg *msg, struct survey_store_msg *data)
{
	*data = *msg;
}

int survey_store_publish(const struct survey_observation *obs, uint32_t sequence)
{
	size_t encoded_len;
	int err;

	if (obs == NULL) {
		return -EINVAL;
	}

	if (!obs->gnss_valid && !obs->scan_valid) {
		return -EINVAL;
	}

	k_mutex_lock(&staging_lock, K_FOREVER);

	survey_record_from_obs(obs, sequence, &staged_record);

	err = survey_record_encode(&staged_record, staging.cbor, sizeof(staging.cbor),
				   &encoded_len);
	if (err) {
		k_mutex_unlock(&staging_lock);

		STORE_WRN("Encode failed (%d); record %u not stored", err, sequence);

		return err;
	}

	staging.len = (uint32_t)encoded_len;

	/* The tail of the slot is whatever the previous record left there. Clearing it costs
	 * a memset per record and buys a stored image that is a function of the record alone,
	 * which is what makes a byte-for-byte fixture comparison meaningful when reading the
	 * partition back.
	 */
	memset(staging.cbor + encoded_len, 0, sizeof(staging.cbor) - encoded_len);

	err = zbus_chan_pub(&survey_store_chan, &staging, K_SECONDS(1));

	k_mutex_unlock(&staging_lock);

	if (err) {
		STORE_WRN("Publish failed (%d); record %u not stored", err, sequence);

		return err;
	}

	return 0;
}
