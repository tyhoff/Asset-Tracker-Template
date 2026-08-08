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
 * ZBUS_MSG_SUBSCRIBER, so publishing is a bounded queue push and the worst case is a
 * concurrent publisher blocking for the 1 s timeout, with priority inheritance in play.
 * Adding a ZBUS_OBSERVERS listener to this channel would run that callback in publisher
 * context with staging_lock held, and that is a different and much worse proposition.
 */

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
 * most of a day of driving.
 *
 * The 16 bytes left over are not slack, they are the reason this works. The backend keeps
 * one *file* per block, so each file is entries_per_block * slot_size = 4080 bytes, and
 * LittleFS stores a file in a single block only while it is at most block_size - 8 bytes
 * (the CTZ skip-list reserves two 4-byte pointers). 4080 clears 4088 by eight bytes. A
 * slot size that packed the block more tightly -- 819, say, for 4095 of 4096 -- would push
 * every file onto a second block and double the partition's block cost overnight, which
 * the byte-based verify_partition_size() check would not notice. See FW-6 in
 * REQUIREMENTS.md.
 *
 * If the block size turns out not to be 4096, nothing breaks: the backend computes
 * entries-per-block at runtime from fs_statvfs(). Only the packing efficiency changes.
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
 * after this returns. To ask what actually landed, use the upstream `att storage stats`
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

#ifdef __cplusplus
}
#endif

#endif /* _SURVEY_STORE_H_ */
