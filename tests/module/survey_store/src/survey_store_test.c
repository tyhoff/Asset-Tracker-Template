/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file survey_store_test.c
 * @brief FW-6: survey records are committed to flash, and the device says so when it stops.
 *
 * This drives the real path -- survey_store_publish() -> zbus -> the storage module's
 * thread -> the LittleFS backend -> a simulated flash device -- rather than calling the
 * backend directly. The interesting failures of a storage layer live in the seams
 * between those pieces (a slot too small for the message, a record count that disagrees
 * with what is on flash, a full partition that quietly eats data), and a test that pokes
 * the backend alone cannot see any of them.
 *
 * The same source is built twice, once per full-behaviour choice. Which one is compiled
 * in decides what the capacity tests assert; see the STOP/OVERWRITE split at the bottom.
 */

#include <string.h>

#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>

#include "storage.h"
#include "storage_backend.h"
#include "storage_data_types.h"
#include "survey_obs.h"
#include "survey_record.h"
#include "survey_record_decode.h"
#include "survey_record_types.h"
#include "survey_store.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);

/* Long enough for the storage thread to drain everything published so far. The backend
 * writes to a RAM-backed flash simulator, so this is scheduling latency, not flash time.
 */
#define SETTLE K_MSEC(500)

#define MAX_RECORDS CONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE

/* Too large for a test-function stack frame in this configuration. */
static struct survey_observation obs;
static struct survey_store_msg slot;
static uint8_t reference[SURVEY_RECORD_MAX_SIZE];

/* Build a distinguishable observation.
 *
 * Latitude carries the index so that two stored records are never byte-identical: a
 * read-back test that stores the same bytes N times cannot tell a working ring buffer
 * from one that rewrites slot zero N times.
 */
static void make_observation(unsigned int index, struct survey_observation *out)
{
	memset(out, 0, sizeof(*out));

	out->gnss_valid = true;
	out->gnss_time_base = SURVEY_TIME_BASE_UNIX;
	out->gnss_timestamp = 1700000000000LL + index;
	out->gnss.latitude = 63.4 + (double)index / 10000.0;
	out->gnss.longitude = 10.4;
	out->gnss.accuracy = 12.5f;

	out->scan_valid = true;
	out->scan_time_base = SURVEY_TIME_BASE_UNIX;
	out->scan_timestamp = out->gnss_timestamp;
	out->scan.current_cell.id = 0x1234500 + index;
	out->scan.current_cell.tac = 0x4321;
	out->scan.current_cell.mcc = 242;
	out->scan.current_cell.mnc = 1;
	out->scan.current_cell.rsrp = -60;
	out->scan.current_cell.rsrq = -10;
	out->scan.current_cell.earfcn = 6300;
	out->scan.ncells_count = 0;
	out->scan.gci_cells_count = 0;
	out->scan.wifi_cnt = 0;
}

/* Build the largest observation the application can produce: a fix, a serving cell, and
 * every neighbour, GCI cell and access point the location module's arrays can hold. This
 * is what the slot size has to survive -- make_observation() encodes to well under a
 * hundred bytes and would pass any slot-size assertion ever written.
 */
static void make_maximal_observation(struct survey_observation *out)
{
	make_observation(0, out);

	out->scan.ncells_count = ARRAY_SIZE(out->scan.neighbor_cells);
	for (unsigned int i = 0; i < out->scan.ncells_count; i++) {
		out->scan.neighbor_cells[i].earfcn = 6300 + i;
		out->scan.neighbor_cells[i].time_diff = -1000 - (int)i;
		out->scan.neighbor_cells[i].phys_cell_id = 100 + i;
		out->scan.neighbor_cells[i].rsrp = -90 - (int16_t)i;
		out->scan.neighbor_cells[i].rsrq = -12 - (int16_t)i;
	}

	out->scan.gci_cells_count = ARRAY_SIZE(out->scan.gci_cells);
	for (unsigned int i = 0; i < out->scan.gci_cells_count; i++) {
		out->scan.gci_cells[i].id = 0x2000000 + i;
		out->scan.gci_cells[i].mcc = 242;
		out->scan.gci_cells[i].mnc = 1;
		out->scan.gci_cells[i].tac = 0x5000 + i;
		out->scan.gci_cells[i].earfcn = 1500 + i;
		out->scan.gci_cells[i].rsrp = -95 - (int16_t)i;
		out->scan.gci_cells[i].rsrq = -14 - (int16_t)i;
	}

	out->scan.wifi_cnt = ARRAY_SIZE(out->scan.wifi_aps);
	for (unsigned int i = 0; i < out->scan.wifi_cnt; i++) {
		out->scan.wifi_aps[i].rssi = -50 - (int8_t)i;
		out->scan.wifi_aps[i].mac_length = 6;
		for (unsigned int b = 0; b < 6; b++) {
			out->scan.wifi_aps[i].mac[b] = (uint8_t)(0xA0 + i * 8 + b);
		}
		out->scan.wifi_aps[i].channel = (uint8_t)(1 + i);
		out->scan.wifi_aps[i].band = 0;
	}
}

/* Encode what the store is expected to have written, using the same encoder. */
static size_t encode_reference(unsigned int index, uint32_t sequence)
{
	struct survey_record_data rec;
	struct survey_observation local;
	size_t len;
	int err;

	make_observation(index, &local);
	survey_record_from_obs(&local, sequence, &rec);

	err = survey_record_encode(&rec, reference, sizeof(reference), &len);
	TEST_ASSERT_EQUAL(0, err);

	return len;
}

static const struct storage_data *survey_type(void)
{
	STRUCT_SECTION_FOREACH(storage_data, t) {
		if (t->data_type == STORAGE_TYPE_SURVEY) {
			return t;
		}
	}

	return NULL;
}

/* Publish `count` records, sequence numbers and payloads both derived from the index. */
static void store_records(unsigned int first, unsigned int count)
{
	for (unsigned int i = first; i < first + count; i++) {
		make_observation(i, &obs);
		TEST_ASSERT_EQUAL(0, survey_store_publish(&obs, i));
	}

	k_sleep(SETTLE);
}

/* Retrieve the head record and assert it is the one built from `index`. */
static void assert_head_is(const struct storage_data *type, unsigned int index)
{
	size_t expected_len = encode_reference(index, index);
	int ret;

	ret = storage_backend_get()->retrieve(type, &slot, sizeof(slot));
	TEST_ASSERT_EQUAL((int)sizeof(slot), ret);
	TEST_ASSERT_EQUAL_UINT32((uint32_t)expected_len, slot.len);
	TEST_ASSERT_EQUAL_MEMORY(reference, slot.cbor, expected_len);
}

void setUp(void)
{
	struct storage_msg clear_msg = { .type = STORAGE_CLEAR };

	TEST_ASSERT_EQUAL(0, zbus_chan_pub(&storage_chan, &clear_msg, K_SECONDS(1)));
	k_sleep(SETTLE);

	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
}

void tearDown(void)
{
}

/* The slot size is a flash-packing decision and the record budget is a schema decision.
 * survey_store.c BUILD_ASSERTs that they are compatible; this is the same claim measured
 * against a record that was actually encoded at its worst case, so a schema change that
 * stays under the budget on paper but not in bytes is still caught.
 */
void test_the_largest_possible_record_fits_a_slot(void)
{
	struct survey_record_data rec;
	size_t len;

	make_maximal_observation(&obs);
	survey_record_from_obs(&obs, 0, &rec);

	TEST_ASSERT_EQUAL(0, survey_record_encode(&rec, reference, sizeof(reference), &len));
	TEST_ASSERT_LESS_OR_EQUAL_size_t(SURVEY_STORE_PAYLOAD_MAX, len);

	/* And it is genuinely large, so the assertion above is not passing on a stub. */
	TEST_ASSERT_GREATER_THAN_size_t(400, len);
}

/* A maximal record must survive the whole path, not just the encoder: it is the case that
 * fills the slot, so any off-by-one in the message size shows up here first.
 */
void test_the_largest_possible_record_round_trips(void)
{
	const struct storage_data *type = survey_type();
	struct survey_record decoded;
	size_t decoded_len;

	make_maximal_observation(&obs);
	TEST_ASSERT_EQUAL(0, survey_store_publish(&obs, 99));
	k_sleep(SETTLE);

	TEST_ASSERT_EQUAL((int)sizeof(slot),
			  storage_backend_get()->retrieve(type, &slot, sizeof(slot)));
	TEST_ASSERT_EQUAL(0, cbor_decode_survey_record(slot.cbor, slot.len, &decoded,
						       &decoded_len));
	TEST_ASSERT_EQUAL_UINT32(99, decoded.sequence_m);
}

/* The headline requirement: store N, read back N, in order, byte for byte. */
void test_store_n_records_and_read_them_all_back(void)
{
	const struct storage_data *type = survey_type();
	const unsigned int n = 12;

	TEST_ASSERT_NOT_NULL(type);

	store_records(0, n);

	TEST_ASSERT_EQUAL(n, storage_backend_get()->count(type));

	for (unsigned int i = 0; i < n; i++) {
		assert_head_is(type, i);
	}

	TEST_ASSERT_EQUAL(0, storage_backend_get()->count(type));
}

/* A record read back off flash must decode, not merely compare equal to a buffer this
 * test produced. Comparing against our own encoder would pass just as happily if both
 * sides were writing garbage.
 */
void test_a_read_back_record_still_decodes(void)
{
	const struct storage_data *type = survey_type();
	struct survey_record decoded;
	size_t decoded_len;
	int err;

	store_records(7, 1);

	TEST_ASSERT_EQUAL((int)sizeof(slot),
			  storage_backend_get()->retrieve(type, &slot, sizeof(slot)));

	err = cbor_decode_survey_record(slot.cbor, slot.len, &decoded, &decoded_len);
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL_size_t(slot.len, decoded_len);
	TEST_ASSERT_EQUAL_UINT32(7, decoded.sequence_m);
}

/* Records are variable length in a fixed-length slot, so every slot has a tail. If the
 * tail were left as whatever the previous occupant wrote, a short record would carry
 * fragments of an older one off the device -- readable, decodable-looking bytes past
 * `len` that no reader is obliged to ignore.
 */
void test_the_unused_tail_of_a_slot_is_zeroed(void)
{
	const struct storage_data *type = survey_type();
	uint32_t big_len;

	TEST_ASSERT_NOT_NULL(type);

	/* Dirty every slot with a maximal record first, then drain, so that the small
	 * record stored below lands in a slot that already holds several hundred bytes of
	 * an older one. Storing into a freshly cleared partition would assert nothing: the
	 * tail would read back as zero whether or not survey_store.c cleared it.
	 */
	for (unsigned int i = 0; i < MAX_RECORDS; i++) {
		make_maximal_observation(&obs);
		TEST_ASSERT_EQUAL(0, survey_store_publish(&obs, i));
	}
	k_sleep(SETTLE);

	TEST_ASSERT_EQUAL((int)sizeof(slot),
			  storage_backend_get()->retrieve(type, &slot, sizeof(slot)));
	big_len = slot.len;

	for (unsigned int i = 1; i < MAX_RECORDS; i++) {
		TEST_ASSERT_EQUAL((int)sizeof(slot),
				  storage_backend_get()->retrieve(type, &slot, sizeof(slot)));
	}
	TEST_ASSERT_EQUAL(0, storage_backend_get()->count(type));

	/* Wraps back onto slot zero, which is still holding a maximal record's bytes. */
	store_records(0, 1);

	TEST_ASSERT_EQUAL((int)sizeof(slot),
			  storage_backend_get()->retrieve(type, &slot, sizeof(slot)));
	TEST_ASSERT_LESS_THAN_UINT32(big_len, slot.len);

	for (uint32_t i = slot.len; i < SURVEY_STORE_PAYLOAD_MAX; i++) {
		TEST_ASSERT_EQUAL_UINT8(0, slot.cbor[i]);
	}
}

/* Surviving a power cycle means the ring's read and write offsets are on flash, not in
 * RAM. Everything else in this file reads back through the same backend instance that did
 * the writing, which cannot tell a synced header from a cached one. This opens the header
 * file independently, through the filesystem, and reads what a freshly booted device would
 * find there.
 *
 * A remount would be the more direct test, but the backend keeps its header files open for
 * its own lifetime and offers no way to close them, so unmounting underneath it is not
 * something this test can do safely.
 */
void test_the_ring_offsets_are_on_flash_not_in_ram(void)
{
	/* Mirrors struct storage_file_header, which is private to the backend. */
	struct {
		uint32_t read_offset;
		uint32_t write_offset;
	} header;
	const struct storage_data *type = survey_type();
	const char *mnt_point = NULL;
	struct fs_file_t file;
	char path[64];
	int idx = 0;

	TEST_ASSERT_NOT_NULL(type);
	TEST_ASSERT_EQUAL(0, fs_readmount(&idx, &mnt_point));
	TEST_ASSERT_NOT_NULL(mnt_point);

	TEST_ASSERT_LESS_THAN_INT((int)sizeof(path),
				  snprintk(path, sizeof(path), "%s/%s.header",
					   mnt_point, type->name));

	store_records(0, 5);

	fs_file_t_init(&file);
	TEST_ASSERT_EQUAL(0, fs_open(&file, path, FS_O_READ));
	TEST_ASSERT_EQUAL((int)sizeof(header), fs_read(&file, &header, sizeof(header)));
	TEST_ASSERT_EQUAL(0, fs_close(&file));

	TEST_ASSERT_EQUAL_UINT32(5, header.write_offset);
	TEST_ASSERT_EQUAL_UINT32(0, header.read_offset);

	/* Reading is destructive, and that must be durable too -- otherwise a reset in the
	 * middle of an export would hand the same records out twice.
	 */
	TEST_ASSERT_EQUAL((int)sizeof(slot),
			  storage_backend_get()->retrieve(type, &slot, sizeof(slot)));
	TEST_ASSERT_EQUAL((int)sizeof(slot),
			  storage_backend_get()->retrieve(type, &slot, sizeof(slot)));

	fs_file_t_init(&file);
	TEST_ASSERT_EQUAL(0, fs_open(&file, path, FS_O_READ));
	TEST_ASSERT_EQUAL((int)sizeof(header), fs_read(&file, &header, sizeof(header)));
	TEST_ASSERT_EQUAL(0, fs_close(&file));

	TEST_ASSERT_EQUAL_UINT32(5, header.write_offset);
	TEST_ASSERT_EQUAL_UINT32(2, header.read_offset);
}

/* An observation with neither a fix nor a scan is a bug at the call site, not an empty
 * record to file away. Storing it would put a record on flash that decodes to nothing.
 */
void test_an_empty_observation_is_refused(void)
{
	const struct storage_data *type = survey_type();
	struct survey_observation empty = {0};

	TEST_ASSERT_EQUAL(-EINVAL, survey_store_publish(&empty, 0));
	TEST_ASSERT_EQUAL(-EINVAL, survey_store_publish(NULL, 0));

	k_sleep(SETTLE);

	TEST_ASSERT_EQUAL(0, storage_backend_get()->count(type));
}

/* Filling exactly to capacity must not itself trigger the full-behaviour. Off-by-one here
 * would cost one record per run under STOP, and would be invisible.
 */
void test_filling_to_capacity_stores_every_record(void)
{
	const struct storage_data *type = survey_type();

	store_records(0, MAX_RECORDS);

	TEST_ASSERT_EQUAL(MAX_RECORDS, storage_backend_get()->count(type));
	assert_head_is(type, 0);
}

/* The three tests below each apply to exactly one full-behaviour. Unity's runner
 * generator scans the source text and emits a call for every `void test_*(void)` it
 * finds, so they cannot be #if'd out of the build -- the runner would still call them.
 * They report themselves as ignored in the configuration they do not describe, which also
 * makes it visible in the log that the other configuration is where they ran.
 */
#define REQUIRE_FULL_STOP()								\
	if (!IS_ENABLED(CONFIG_APP_STORAGE_FULL_STOP)) {				\
		TEST_IGNORE_MESSAGE("FULL_OVERWRITE build; asserted in .stop");		\
	}

#define REQUIRE_FULL_OVERWRITE()							\
	if (IS_ENABLED(CONFIG_APP_STORAGE_FULL_STOP)) {					\
		TEST_IGNORE_MESSAGE("FULL_STOP build; asserted in .overwrite");		\
	}

/* The survey configuration. Once full, the device keeps what it has: the beginning of a
 * drive is the part that was hardest to collect, and losing it silently is the failure
 * this choice exists to prevent.
 */
void test_storage_full_keeps_the_oldest_records(void)
{
	const struct storage_data *type = survey_type();

	REQUIRE_FULL_STOP();

	store_records(0, MAX_RECORDS);
	store_records(MAX_RECORDS, 5);

	TEST_ASSERT_EQUAL(MAX_RECORDS, storage_backend_get()->count(type));

	/* Record 0 is still the head, and the five newest were never written. */
	for (unsigned int i = 0; i < MAX_RECORDS; i++) {
		assert_head_is(type, i);
	}

	TEST_ASSERT_EQUAL(0, storage_backend_get()->count(type));
}

/* Rejecting a record must not wedge the ring. After draining, the device records again --
 * otherwise "storage full" would be a one-way trip requiring a reflash.
 */
void test_storage_accepts_records_again_after_being_drained(void)
{
	const struct storage_data *type = survey_type();

	REQUIRE_FULL_STOP();

	store_records(0, MAX_RECORDS);
	store_records(MAX_RECORDS, 1);

	for (unsigned int i = 0; i < MAX_RECORDS; i++) {
		assert_head_is(type, i);
	}

	store_records(100, 3);

	TEST_ASSERT_EQUAL(3, storage_backend_get()->count(type));
	assert_head_is(type, 100);
	assert_head_is(type, 101);
	assert_head_is(type, 102);
}

/* Upstream's behaviour, kept working. A build that wants a live view of the recent past
 * still gets one; this test is what stops the STOP path from becoming the only path.
 */
void test_storage_full_drops_the_oldest_records(void)
{
	const struct storage_data *type = survey_type();
	const unsigned int extra = 5;

	REQUIRE_FULL_OVERWRITE();

	store_records(0, MAX_RECORDS);
	store_records(MAX_RECORDS, extra);

	TEST_ASSERT_EQUAL(MAX_RECORDS, storage_backend_get()->count(type));

	/* The first `extra` records are gone; the head is now record `extra`. */
	for (unsigned int i = extra; i < MAX_RECORDS + extra; i++) {
		assert_head_is(type, i);
	}

	TEST_ASSERT_EQUAL(0, storage_backend_get()->count(type));
}

/* Not tested here: the two mutexes added to survey_record_encode() and
 * survey_store_publish(). They cannot be exercised on native_sim. Its simulated clock only
 * advances when no thread is ready to run, so a contender thread that spins on k_yield()
 * never lets the main thread's k_sleep() expire and the suite hangs until twister kills it,
 * while a contender that sleeps is never runnable during the window the main thread spends
 * inside the encoder. Both were tried; the sleeping version passes with the lock deliberately
 * removed, which is worse than no test at all.
 *
 * A version of this test is checked in nowhere on purpose. The serialisation is verified by
 * inspection instead -- see the lock-ordering section in survey_store.h -- and on target,
 * where preemption is real, once the capture orchestrator gives a second thread that actually
 * calls these functions.
 */

/* Provided by the test runner generator. */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
