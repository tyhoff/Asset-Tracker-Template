/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SURVEY_STORE_H_
#define _SURVEY_STORE_H_

/**
 * @file survey_store.h
 * @brief Durable storage of encoded survey records (FW-6).
 *
 * One encoded CBOR record per slot, published on its own zbus channel and picked up by
 * the storage module's LittleFS backend. Records are committed as they are produced,
 * never buffered in RAM until the end of a drive: a drive that ends with a flat battery
 * or a yanked cable is a normal outcome, and everything captured before that moment is
 * still valid data.
 *
 * Slots are fixed-size because the backend is a fixed-stride ring -- see the note on
 * SURVEY_STORE_SLOT_SIZE for why that size is what it is.
 *
 * @section locking Lock ordering
 *
 * Both the staging slot and the encoder's scratch record are file-scope statics, each with
 * its own mutex. The only nesting is `staging_lock` (survey_store.c) then `encode_lock`
 * (survey_record.c), taken in that order by survey_store_publish(). Nothing takes them the
 * other way round -- the `survey hex` shell command takes encode_lock alone -- so there is
 * no cycle. Keep it that way.
 *
 * survey_store_publish() calls zbus_chan_pub() while holding staging_lock. That is safe
 * only because survey_store_chan has no listeners: the storage module is a
 * ZBUS_MSG_SUBSCRIBER, so publishing is a queue push rather than a callback run under the
 * lock. It is not free of consequence, though: if enough messages are in flight that the
 * system heap cannot back another net_buf, zbus asserts and the device reboots. The
 * timeout does not prevent that -- see the K_FOREVER rationale in survey_store.c -- so the
 * worst case of a burst is a panic, not a dropped record. At a 10-30 s cadence there is no
 * burst: one record is in flight at a time, and the publish never blocks at all.
 *
 * Adding a ZBUS_OBSERVERS listener to this channel would run that callback in publisher
 * context with staging_lock held, and that is a different and much worse proposition.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/zbus/zbus.h>

#include "survey_obs.h"
#include "survey_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes one record occupies on flash, including its length prefix.
 *
 * Chosen to pack the LittleFS block, not rounded to something tidy. The backend writes
 * entries at a fixed stride and never lets one straddle a block boundary, so the waste
 * per block is `block_size % slot_size` -- and at the 4096-byte erase block of the
 * Thingy:91 X external flash, 816 gives 5 slots using 4080 of 4096 bytes. Rounding up to
 * 1024 instead would fit 4 slots and throw away a fifth of a 24 MiB partition, which is
 * most of a day of driving. See FW-6 in REQUIREMENTS.md.
 *
 * The CTZ-skip-list argument that originally justified 816 over a tighter-packed value
 * like 819 (single-block-file storage needs at most block_size - 8 bytes, the skip-list's
 * two 4-byte pointers) only applies when each file is exactly one block --
 * CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE at its default of 0. The survey overlay
 * sets that to 65536 (16 blocks/file, see app/overlay-survey.conf and "A near-full
 * partition..." in docs/common/dev_workflow.md), specifically to cut the file count at
 * full capacity and avoid an fs_mgmt/LittleFS directory-scan timeout; at that file size the
 * single-block CTZ ceiling no longer constrains slot packing, though 816 is still a fine
 * choice on its own byte-waste terms.
 *
 * If the block size turns out not to be 4096, nothing breaks: the backend computes
 * entries-per-file at runtime from fs_statvfs() and CONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE.
 * Only the packing efficiency changes.
 */
#define SURVEY_STORE_SLOT_SIZE 816

/** Bytes of encoded CBOR one slot can hold, after the length prefix. */
#define SURVEY_STORE_PAYLOAD_MAX (SURVEY_STORE_SLOT_SIZE - sizeof(uint32_t))

/** One stored record.
 *
 * This is both the zbus message type and the on-flash format, so a record is written to
 * flash exactly as it was published, with no reserialisation step in between that could
 * disagree with the encoder.
 */
struct survey_store_msg {
	/** Encoded length in bytes. Records are variable length; the slot is not. */
	uint32_t len;

	/** Encoded CBOR. Bytes beyond @ref len are unspecified and must not be read. */
	uint8_t cbor[SURVEY_STORE_PAYLOAD_MAX];
};

/* The backend strides by sizeof(), not by SURVEY_STORE_SLOT_SIZE, so the block-packing
 * argument above is only true while the two agree. A slot size that is not a multiple of
 * the struct's alignment would silently pad and invalidate it.
 */
BUILD_ASSERT(sizeof(struct survey_store_msg) == SURVEY_STORE_SLOT_SIZE,
	     "struct survey_store_msg is padded; SURVEY_STORE_SLOT_SIZE no longer describes "
	     "the on-flash stride.");

/** Channel carrying encoded records to the storage module. */
extern const struct zbus_channel survey_store_chan;

/**
 * @brief Encode an observation and publish it for storage.
 *
 * Encodes @p obs into a record and publishes it on @ref survey_store_chan. Publishing
 * hands the record to the storage module's thread; the write to flash completes some time
 * after this returns. To ask what actually landed, use the upstream `att_storage stats`
 * shell command -- it runs on the storage thread, which is the only thread that may touch
 * the backend's open file handles.
 *
 * @param obs      Observation to encode. Must hold at least a fix or a scan.
 * @param sequence Monotonic sequence number within the session.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p obs is NULL or holds nothing.
 * @retval -ENOMEM if the encoded record does not fit a slot. This means the schema grew
 *                 past its budget, not that storage is full.
 * @retval negative errno from the encoder or from zbus otherwise.
 */
int survey_store_publish(const struct survey_observation *obs, uint32_t sequence);

/**
 * @brief Encode an already-assembled record and publish it for storage.
 *
 * The capture orchestrator's entry point. It differs from @ref survey_store_publish in what
 * it can express, not in how it stores: an observation cache holds the latest fix and the
 * latest scan with no relationship between them, whereas a capture cycle knows which fixes
 * bracket which scan and when each measurement happened relative to the others. Those are
 * the fields the host needs to interpolate ground truth, and there is nowhere to put them
 * on the observation path.
 *
 * @p record is read for the duration of the call and not retained.
 *
 * @param record Record to encode. @c sequence is taken from the record itself, so that the
 *               number in the log, in the timing report and on flash cannot disagree.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p record is NULL, or if the cycle produced neither a fix nor a scan.
 * @retval negative errno from the encoder or from zbus otherwise.
 */
int survey_store_publish_record(const struct survey_record_data *record);

/** @brief Take the next record sequence number for this boot.
 *
 * The single source for both store paths. Sequence numbers restart at zero every boot;
 * they order records within one run and nothing more.
 *
 * Every caller that is going to write a record must draw from here rather than keep its
 * own counter. Two counters means two records claiming the same sequence in one boot, and
 * a host decoder that orders or de-duplicates on the field would then mis-order or drop a
 * real measurement -- with nothing on the device to indicate it happened.
 *
 * Safe from any thread.
 */
uint32_t survey_store_next_sequence(void);

/** @brief Report whether the survey storage type is at capacity.
 *
 * Synchronous and proactive: it asks the backend's current record count rather than
 * waiting to see a store fail. That matters because @ref survey_store_publish_record only
 * reports whether the record reached the storage module's queue, not whether the
 * asynchronous write to flash that follows actually succeeded -- see storage.c's
 * handle_data_message(), which logs a failed backend store but does not propagate it.
 * Checking capacity here is the only way a caller finds out before that write is attempted.
 *
 * Always false under APP_STORAGE_FULL_OVERWRITE, which never stops accepting records.
 * Meaningful only under APP_STORAGE_FULL_STOP, where hitting capacity means further
 * records are silently dropped -- the condition FW-9's storage-full LED state exists to
 * surface.
 *
 * Safe from any thread: it does not take staging_lock, and unlike every other caller of
 * storage_backend_get(), it runs on the survey capture thread rather than storage's own
 * thread. The header-file access this ends up doing inside the LittleFS backend is guarded
 * by a mutex in littlefs_backend.c specifically so this cross-thread call cannot interleave
 * with a concurrent header write from storage.c's own thread.
 */
bool survey_store_is_full(void);

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_STORE_H_ */
