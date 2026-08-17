/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/iterable_sections.h>

#include "survey_obs.h"
#include "survey_record.h"
#include "survey_record_decode.h"
#include "survey_record_types.h"
#include "survey_session.h"
#include "survey_store.h"
#include "storage_data_types.h"
#include "storage_backend.h"

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

/* Encode @p record into the staging buffer and publish it. The caller holds staging_lock,
 * which is what protects both the buffer and @p record's lifetime for the duration.
 *
 * Split out so that the two entry points -- an observation from the shell, a completed
 * capture cycle from the orchestrator -- share one encode-and-publish path. Duplicating it
 * would mean the K_FOREVER reasoning below has two copies to stay true of.
 */
static int stage_and_publish(const struct survey_record_data *record, uint32_t sequence)
{
	size_t encoded_len;
	int err;

	err = survey_record_encode(record, staging.cbor, sizeof(staging.cbor),
				   &encoded_len);
	if (err) {
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

	/* K_FOREVER rather than a timeout, because a timeout here cannot do what it looks like
	 * it does and the longest one is the least dangerous.
	 *
	 * zbus copies the message into a net_buf for each ZBUS_MSG_SUBSCRIBER -- the storage
	 * module is one -- and hands this timeout to the allocator. If the allocation fails,
	 * zbus does not return the -ENOMEM the caller asked for by passing a timeout at all.
	 * It asserts, and the device reboots:
	 *
	 *   ASSERTION FAIL @ zephyr/subsys/zbus/zbus.c:252
	 *   net_buf zbus_msg_subscribers_pool is unavailable or heap is full
	 *   <err> os: ***** USAGE FAULT ***** Attempt to execute undefined instruction
	 *
	 * So the previous K_SECONDS(1) did not mean "drop this record if storage is
	 * congested". It meant "panic if a single store takes longer than a second" -- and
	 * stores that slow are ordinary, because LittleFS compacts the record directory on
	 * append and that cost grows with the entry count. Single writes of 14 s and 43.9 s
	 * were measured on target at only a few hundred entries.
	 *
	 * K_FOREVER is not backpressure, and it is worth being precise about why, because the
	 * shape of the pool invites the opposite conclusion. Allocation has two halves. The
	 * net_buf *descriptor* comes from a 64-entry array (prj.conf sets
	 * CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE) and that wait does honour the
	 * timeout. The buffer's
	 * *data* comes from heap_data_alloc(), which accepts a k_timeout_t and then ignores it
	 * in favour of a non-blocking k_malloc() on the 12288-byte system heap. At 816 bytes
	 * plus overhead per message the heap runs out around a dozen in flight -- long before
	 * the 64 descriptors do -- so the blocking half is unreachable and every real failure
	 * is the non-blocking half asserting. Verified on target.
	 *
	 * So K_FOREVER changes nothing observable here. It is chosen because a finite timeout
	 * can only make things worse: it cannot turn the assert into an -ENOMEM, and it can
	 * turn an ordinary slow store into a panic. overlay-survey.conf records the three
	 * configurations tried against the heap limit and why each is worse than the problem.
	 *
	 * None of it is reachable from normal capture: one record in flight, 10-30 s apart,
	 * so the heap holds one 816-byte message and the allocation never comes close. It
	 * takes a burst -- which today only the bench command "survey fill" produces. See
	 * docs/modules/storage.md.
	 */
	err = zbus_chan_pub(&survey_store_chan, &staging, K_FOREVER);
	if (err) {
		STORE_WRN("Publish failed (%d); record %u not stored", err, sequence);

		return err;
	}

	return 0;
}

/* The one source of record sequence numbers for this boot.
 *
 * It lives here rather than in either caller because there are two callers -- the capture
 * orchestrator and "survey store" -- and they each used to keep their own counter starting
 * at zero. Both write the result into survey_record_data.sequence on flash, so a bench
 * session that used both produced two different records claiming the same sequence, and a
 * host decoder ordering or de-duplicating on that field would mis-order or silently drop a
 * real measurement.
 */
static atomic_t sequence_next;

/* Guards the one-time recovery below, not the counter itself -- atomic_inc() needs no lock.
 * A separate mutex from staging_lock on purpose: next_sequence() is called before
 * stage_and_publish() takes staging_lock in the shell path, and conflating the two would
 * make the lock ordering a thing to reason about instead of a thing that is obviously fine.
 */
static K_MUTEX_DEFINE(sequence_recovery_lock);
static bool sequence_recovered;

/* Duplicates the LittleFS backend's record-locating arithmetic (littlefs_backend.c:
 * get_entries_per_file(), get_file_index(), get_entry_offset_index(), and the header
 * layout) rather than extending that backend with an indexed-read primitive it does not
 * have. That backend is upstream-owned; this file is not.
 *
 * The duplication is safe specifically because it is fail-closed, not because it is
 * guaranteed to match: any drift between this arithmetic and the real backend -- a path
 * that does not exist, a seek past what was written, a slot that does not decode -- just
 * falls through to "start at 0", which is the behaviour a corrupted or unreadable last
 * record is supposed to get anyway. A mismatch here is never a silently wrong sequence
 * number, only an unnecessarily conservative one -- but that guarantee only holds if this
 * arithmetic actually mirrors get_entries_per_file(), including the TARGET_FILE_SIZE
 * block-grouping it applies; a mismatch that still lands on a real, in-bounds file would
 * read a real (wrong) record instead of failing to open it. Keep this in sync with
 * CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE the same way scripts/survey_export.py and
 * web/export.html do.
 */
static uint32_t recover_last_sequence(void)
{
	struct fs_file_t file;
	struct {
		uint32_t read_offset;
		uint32_t write_offset;
	} header;
	struct fs_statvfs stat;
	char path[sizeof("/att_storage/SURVEY_4294967295.bin")];
	size_t blocks_per_file;
	size_t entries_per_file;
	uint32_t wrapped_index;
	uint32_t file_index;
	uint32_t entry_offset_index;
	static struct survey_store_msg slot;
	static struct survey_record decoded;
	size_t decoded_len;
	int ret;

	fs_file_t_init(&file);

	ret = fs_open(&file, "/att_storage/SURVEY.header", FS_O_READ);
	if (ret < 0) {
		STORE_WRN("Sequence recovery: no header (%d); starting a new sequence at 0", ret);

		return 0;
	}

	ret = (int)fs_read(&file, &header, sizeof(header));
	fs_close(&file);

	if (ret != (int)sizeof(header)) {
		STORE_WRN("Sequence recovery: short header read (%d); starting a new sequence "
			  "at 0", ret);

		return 0;
	}

	if (header.write_offset == header.read_offset) {
		/* No records survived, whether because none were ever stored or because
		 * everything was retrieved and cleared. Either way there is nothing to
		 * recover from and 0 is the right answer, not a fallback.
		 */
		return 0;
	}

	ret = fs_statvfs("/att_storage", &stat);
	if (ret < 0 || stat.f_frsize == 0) {
		STORE_WRN("Sequence recovery: fs_statvfs failed (%d); starting a new sequence "
			  "at 0", ret);

		return 0;
	}

	/* Mirrors get_entries_per_file()'s blocks_per_file floor of 1: a TARGET_FILE_SIZE of 0
	 * (the default) or below one block reproduces the original one-block-per-file layout.
	 */
	blocks_per_file = CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE / stat.f_frsize;
	if (blocks_per_file < 1) {
		blocks_per_file = 1;
	}

	entries_per_file = (blocks_per_file * stat.f_frsize) / SURVEY_STORE_SLOT_SIZE;
	if (entries_per_file == 0) {
		STORE_WRN("Sequence recovery: slot size exceeds file size; starting a new "
			  "sequence at 0");

		return 0;
	}

	wrapped_index = (header.write_offset - 1) % CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE;
	file_index = wrapped_index / entries_per_file;
	entry_offset_index = wrapped_index % entries_per_file;

	ret = snprintk(path, sizeof(path), "/att_storage/SURVEY_%u.bin", file_index);
	if (ret < 0 || ret >= (int)sizeof(path)) {
		STORE_WRN("Sequence recovery: path build failed; starting a new sequence at 0");

		return 0;
	}

	fs_file_t_init(&file);

	ret = fs_open(&file, path, FS_O_READ);
	if (ret < 0) {
		STORE_WRN("Sequence recovery: cannot open %s (%d); starting a new sequence "
			  "at 0", path, ret);

		return 0;
	}

	ret = fs_seek(&file, (off_t)(entry_offset_index * SURVEY_STORE_SLOT_SIZE), FS_SEEK_SET);
	if (ret < 0) {
		fs_close(&file);
		STORE_WRN("Sequence recovery: seek into %s failed (%d); starting a new "
			  "sequence at 0", path, ret);

		return 0;
	}

	ret = (int)fs_read(&file, &slot, sizeof(slot));
	fs_close(&file);

	if (ret != (int)sizeof(slot) || slot.len == 0 || slot.len > sizeof(slot.cbor)) {
		STORE_WRN("Sequence recovery: bad slot read from %s (%d); starting a new "
			  "sequence at 0", path, ret);

		return 0;
	}

	ret = cbor_decode_survey_record(slot.cbor, slot.len, &decoded, &decoded_len);
	if (ret != 0) {
		STORE_WRN("Sequence recovery: last record does not decode (%d); starting a "
			  "new sequence at 0", ret);

		return 0;
	}

	return decoded.sequence_m + 1;
}

uint32_t survey_store_next_sequence(void)
{
	/* Not folded into the sequence_recovered guard below: survey_session_ensure_written()
	 * no-ops once it has actually succeeded, but retries on every call until then, so a
	 * transient failure (e.g. the modem not yet attached, or nRF Cloud client ID not yet
	 * provisioned, at cold boot) does not permanently lose the session header for the
	 * boot the way gating it behind a "ran once" flag would. See FW-8 in REQUIREMENTS.md
	 * for why a session header needs a once-per-boot write and did not have one until now.
	 */
	survey_session_ensure_written();

	k_mutex_lock(&sequence_recovery_lock, K_FOREVER);
	if (!sequence_recovered) {
		atomic_set(&sequence_next, (atomic_val_t)recover_last_sequence());
		sequence_recovered = true;
	}
	k_mutex_unlock(&sequence_recovery_lock);

	return (uint32_t)atomic_inc(&sequence_next);
}

int survey_store_publish(const struct survey_observation *obs, uint32_t sequence)
{
	int err;

	if (obs == NULL) {
		return -EINVAL;
	}

	if (!obs->gnss_valid && !obs->scan_valid) {
		return -EINVAL;
	}

	k_mutex_lock(&staging_lock, K_FOREVER);

	survey_record_from_obs(obs, sequence, &staged_record);

	err = stage_and_publish(&staged_record, sequence);

	k_mutex_unlock(&staging_lock);

	return err;
}

int survey_store_publish_record(const struct survey_record_data *record)
{
	int err;

	if (record == NULL) {
		return -EINVAL;
	}

	/* A cycle in which every step failed has nothing to say and is refused. One where
	 * only GNSS failed is not: the radio observations are the measurement, and a record
	 * without ground truth still contributes to coverage. The decoder decides what it
	 * can score, not this function.
	 */
	if (!record->scan_valid && !record->gnss_before_valid && !record->gnss_after_valid) {
		return -EINVAL;
	}

	k_mutex_lock(&staging_lock, K_FOREVER);

	err = stage_and_publish(record, record->sequence);

	k_mutex_unlock(&staging_lock);

	return err;
}

bool survey_store_is_full(void)
{
#if defined(CONFIG_APP_STORAGE_FULL_STOP)
	const struct storage_backend *backend = storage_backend_get();

	/* SURVEY is only registered when CONFIG_APP_SURVEY_STORAGE is on -- see
	 * DATA_SOURCE_LIST in storage_data_types.h -- which is always true wherever this
	 * function is reachable, so the loop always finds it.
	 */
	STRUCT_SECTION_FOREACH(storage_data, type) {
		if (type->data_type == STORAGE_TYPE_SURVEY) {
			int count = backend->count(type);

			/* A backend error (count < 0) is treated as full, not as "not full": this
			 * function exists specifically so the LED never claims success when it
			 * can't confirm capacity, and failing open here would defeat that.
			 */
			return count < 0 || count >= CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE;
		}
	}
#endif /* CONFIG_APP_STORAGE_FULL_STOP */

	/* APP_STORAGE_FULL_OVERWRITE never stops accepting records -- the oldest one is
	 * dropped instead -- so there is no "full" condition for this function to report.
	 */
	return false;
}
