/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Unit tests for the capture orchestrator, with the Location library faked.
 *
 * What is worth testing here is the *sequencing*, because that is what cannot be checked
 * by looking at a stored record: a record assembled from three overlapping requests is
 * indistinguishable from a correct one -- same fields, same shape, wrong data. The only
 * place the difference is visible is in the order and separation of the requests
 * themselves, which is what this fake records.
 *
 * The fake stands in for the modem's one RF front end: it refuses to serve two requests
 * at once, and it flags any attempt, so an overlap fails a test rather than quietly
 * working because native_sim has no radio to conflict over.
 */

#include <unity.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "location.h"
#include "survey.h"
#include "survey_capture.h"
#include "survey_obs.h"
#include "survey_record.h"
#include "survey_store.h"

ZBUS_CHAN_DEFINE(location_chan,
		 struct location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* --- What the fake Location library recorded ------------------------------------------ */

#define MAX_REQUESTS 8

struct request_log {
	enum location_msg_type type;
	int64_t started_ms;
	int64_t finished_ms;
};

static struct request_log requests[MAX_REQUESTS];
static uint8_t request_count;

/* Set if a trigger ever arrived while another request was still being served. The real
 * module discards such a trigger silently, so without this flag an overlapping
 * orchestrator would look like one whose steps merely returned no data.
 */
static bool overlap_detected;

/* True from the moment the fake accepts a trigger until it publishes LOCATION_SEARCH_DONE,
 * which is exactly the window in which the real location module discards a new trigger.
 *
 * This has to outlive the dequeue of the message that started it. An earlier version of
 * this fake set it inside a synchronous serve() call, so a trigger published during a
 * search simply sat in the subscriber queue and was served next -- and the flag, the
 * request order and the started/finished timestamps were all satisfied by construction.
 * The suite passed with the orchestrator's wind-down wait deleted, which is the one thing
 * it exists to prove. Serving asynchronously is what makes the queue stop covering for it.
 */
static bool busy;

/* What the fake should do with each step, so a test can make GNSS fail without making the
 * scan fail. Indexed by request number.
 */
static bool step_succeeds[MAX_REQUESTS];

/* Reproduce the one stray LOCATION_SEARCH_DONE the real system can produce: location.c
 * answers a suppressed application trigger with a synthetic done, and it lands on the same
 * channel the orchestrator sequences on. Published by the fake between accepting a trigger
 * and reporting started, which is the window in which it is indistinguishable from this
 * step's own done unless the orchestrator gates on started.
 */
static bool stray_done_before_started;

/* How long the fake waits before reporting started. Non-zero pushes it past run_step()'s
 * one-second handshake wait, which is what makes the stray done above outlive any clear
 * done at the end of that wait.
 */
static int32_t started_delay_ms;

/* --- What the orchestrator handed to storage ------------------------------------------ */

static struct survey_record_data stored_record;
static bool record_stored;
static int store_result;

/* Sampled on the capture thread at the moment the store begins. On target the store is the
 * long tail of a cycle -- tens of seconds once the partition fills -- and location.c has to
 * answer a suppressed application trigger during it, so the two predicates have to disagree
 * here. See test_the_radio_is_reported_free_before_the_store_begins().
 */
static bool busy_at_store;
static bool radio_busy_at_store;

int survey_store_publish_record(const struct survey_record_data *record)
{
	busy_at_store = survey_capture_busy();
	radio_busy_at_store = survey_capture_radio_busy();

	if (store_result != 0) {
		return store_result;
	}

	stored_record = *record;
	record_stored = true;

	return 0;
}

static uint32_t stub_sequence;

uint32_t survey_store_next_sequence(void)
{
	return stub_sequence++;
}

/* --- Stubs for the rest of the survey module ------------------------------------------ */

/* The cache the orchestrator snapshots after each step. The fake fills it in the same
 * order the real observation path would.
 */
static struct survey_observation cache;

void survey_obs_snapshot(struct survey_observation *out)
{
	*out = cache;
}

void survey_time_now(int64_t *now_ms, enum survey_time_base *time_base)
{
	*now_ms = k_uptime_get();
	*time_base = SURVEY_TIME_BASE_UPTIME;
}

/* --- The fake Location library --------------------------------------------------------- */

ZBUS_MSG_SUBSCRIBER_DEFINE(fake_location);
ZBUS_CHAN_ADD_OBS(location_chan, fake_location, 0);

/* Mirrors survey.c: every message reaches the orchestrator, including the lifecycle
 * events it sequences on.
 */
static void notify_cb(const struct zbus_channel *chan)
{
	survey_capture_notify(zbus_chan_const_msg(chan));
}

ZBUS_LISTENER_DEFINE(notify_listener, notify_cb);
ZBUS_CHAN_ADD_OBS(location_chan, notify_listener, 1);

static void publish(enum location_msg_type type)
{
	struct location_msg msg = { .type = type };

	zbus_chan_pub(&location_chan, &msg, K_SECONDS(1));
}

static bool is_trigger(enum location_msg_type type)
{
	return type == LOCATION_SEARCH_TRIGGER ||
	       type == LOCATION_GNSS_SEARCH_TRIGGER ||
	       type == LOCATION_SCAN_SEARCH_TRIGGER;
}

/* How long the fake pretends to search before producing a result, and how long it then
 * takes to report done. Both matter.
 *
 * The result-to-done gap is the one the real library has and the one the orchestrator's
 * wind-down wait exists for: the library publishes its data first and finishes afterwards,
 * and a trigger sent in that gap is discarded. Without a gap here, an orchestrator that
 * skipped the wait would still be served, and the suite would prove nothing.
 */
#define FAKE_SEARCH_MS 50
#define FAKE_RESULT_TO_DONE_MS 50

/* Scheduling slack for "the next step started promptly". Generous on purpose: native_sim
 * runs in real time for this suite, so the number has to survive a loaded build machine.
 * It still bounds the gap far below any wind-down timeout, which is what it is for.
 */
#define STEP_PICKUP_SLACK_MS 500

enum fake_phase {
	FAKE_PHASE_STARTED,
	FAKE_PHASE_RESULT,
	FAKE_PHASE_DONE,
};

static enum fake_phase phase;
static enum location_msg_type serving_type;
static uint8_t serving_index;

static void fake_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(fake_work, fake_work_fn);

/* Runs on the system workqueue, so the fake's own thread stays free to dequeue -- and
 * therefore to *discard* -- a trigger that arrives mid-search.
 */
static void fake_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (phase == FAKE_PHASE_STARTED) {
		publish(LOCATION_SEARCH_STARTED);

		phase = FAKE_PHASE_RESULT;
		k_work_schedule(&fake_work, K_MSEC(FAKE_SEARCH_MS));

		return;
	}

	if (phase == FAKE_PHASE_RESULT) {
		if (step_succeeds[serving_index]) {
			if (serving_type == LOCATION_SCAN_SEARCH_TRIGGER) {
				cache.scan_valid = true;
				cache.scan_timestamp = k_uptime_get();
				cache.scan.wifi_cnt = 3;
				/* Non-zero so that the orchestrator failing to carry it
				 * into the record is a failure rather than a match against
				 * the zero-initialised field.
				 */
				cache.scan_local_mac_dropped = 2;
				publish(LOCATION_CLOUD_REQUEST);
			} else {
				cache.gnss_valid = true;
				cache.gnss_timestamp = k_uptime_get();
				cache.gnss_time_base = SURVEY_TIME_BASE_UPTIME;
				cache.gnss.latitude = 63.42 + serving_index;
				publish(LOCATION_GNSS_DATA);
			}
		}

		phase = FAKE_PHASE_DONE;
		k_work_schedule(&fake_work, K_MSEC(FAKE_RESULT_TO_DONE_MS));

		return;
	}

	requests[serving_index].finished_ms = k_uptime_get();

	/* Cleared before the publish, not after: done is what releases the orchestrator,
	 * and the real module is genuinely free from that point.
	 */
	busy = false;

	/* Always published, on every terminal path, exactly as the real module does. It is
	 * what the orchestrator sequences on, so a step that produced nothing still
	 * releases the next one.
	 */
	publish(LOCATION_SEARCH_DONE);
}

static void serve(enum location_msg_type trigger)
{
	uint8_t index = request_count;

	if (busy) {
		/* Exactly what the real module does -- discard it -- plus a record that it
		 * happened, which the real module only writes to the log.
		 */
		overlap_detected = true;

		return;
	}

	if (index >= MAX_REQUESTS) {
		return;
	}

	busy = true;
	requests[index].type = trigger;
	requests[index].started_ms = k_uptime_get();
	request_count++;

	serving_type = trigger;
	serving_index = index;
	phase = FAKE_PHASE_RESULT;

	if (stray_done_before_started) {
		publish(LOCATION_SEARCH_DONE);
	}

	if (started_delay_ms > 0) {
		phase = FAKE_PHASE_STARTED;
		k_work_schedule(&fake_work, K_MSEC(started_delay_ms));

		return;
	}

	publish(LOCATION_SEARCH_STARTED);

	k_work_schedule(&fake_work, K_MSEC(FAKE_SEARCH_MS));
}

static void fake_location_thread(void *p1, void *p2, void *p3)
{
	const struct zbus_channel *chan;
	struct location_msg msg;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		if (zbus_sub_wait_msg(&fake_location, &chan, &msg, K_FOREVER) != 0) {
			continue;
		}

		if (chan == &location_chan && is_trigger(msg.type)) {
			serve(msg.type);
		}
	}
}

K_THREAD_DEFINE(fake_location_tid, 4096, fake_location_thread, NULL, NULL, NULL, 3, 0, 0);

/* --- Helpers --------------------------------------------------------------------------- */

static void reset_fake(void)
{
	/* Synchronously, and before anything is zeroed: a work item left over from the
	 * previous test would otherwise publish into the next one and write through
	 * serving_index into a freshly cleared request log.
	 */
	struct k_work_sync sync;

	k_work_cancel_delayable_sync(&fake_work, &sync);

	memset(requests, 0, sizeof(requests));
	memset(&cache, 0, sizeof(cache));
	memset(&stored_record, 0, sizeof(stored_record));
	request_count = 0;
	overlap_detected = false;
	busy = false;
	record_stored = false;
	store_result = 0;
	busy_at_store = false;
	radio_busy_at_store = false;
	stray_done_before_started = false;
	started_delay_ms = 0;

	for (int i = 0; i < MAX_REQUESTS; i++) {
		step_succeeds[i] = true;
	}
}

/* Wait for the cycle to finish rather than sleeping a fixed amount: a fixed sleep either
 * makes the suite slow or makes it flaky, and which one is a property of the host.
 */
static bool wait_for_cycle(k_timeout_t timeout)
{
	int64_t deadline = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);

	while (k_uptime_get() < deadline) {
		if (!survey_capture_busy() && request_count >= 3) {
			/* The last step's store happens after the last request finishes. */
			k_sleep(K_MSEC(20));

			return true;
		}
		k_sleep(K_MSEC(10));
	}

	return false;
}

void setUp(void)
{
	reset_fake();
}

void tearDown(void)
{
}

/* --- Tests ------------------------------------------------------------------------------ */

void test_cycle_issues_gnss_scan_gnss_in_that_order(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_EQUAL_MESSAGE(3, request_count,
				  "a cycle is exactly three location requests");
	TEST_ASSERT_EQUAL(LOCATION_GNSS_SEARCH_TRIGGER, requests[0].type);
	TEST_ASSERT_EQUAL(LOCATION_SCAN_SEARCH_TRIGGER, requests[1].type);
	TEST_ASSERT_EQUAL(LOCATION_GNSS_SEARCH_TRIGGER, requests[2].type);
}

void test_requests_never_overlap(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_FALSE_MESSAGE(overlap_detected,
				  "a trigger was published while a search was still running; "
				  "the real module would have discarded it silently");

	/* Not "started after the previous finished" -- that cannot fail. The fake refuses a
	 * trigger while it is busy, so a request that overlapped would never be recorded at
	 * all, and every request that IS in the array is one the fake accepted when idle.
	 * The assertion would hold with the sequencing deleted.
	 *
	 * What can fail is the size of the gap: the orchestrator should pick the next step
	 * up promptly once the previous search reports done, so an unbounded wait between
	 * them is a real defect this can see.
	 */
	for (uint8_t i = 1; i < request_count; i++) {
		TEST_ASSERT_LESS_THAN_MESSAGE(FAKE_RESULT_TO_DONE_MS + STEP_PICKUP_SLACK_MS,
					      requests[i].started_ms - requests[i - 1].finished_ms,
					      "the orchestrator idled between two steps");
	}
}

void test_every_step_waits_for_the_previous_search_to_report_done(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	/* This is the test that fails if the wind-down wait is removed from run_step().
	 *
	 * The fake publishes each step's result FAKE_RESULT_TO_DONE_MS before it reports
	 * done, and stays busy across that gap -- which is what the real Location library
	 * does. An orchestrator that triggers the next step as soon as the result arrives
	 * lands inside the gap; the fake discards that trigger exactly as the real module
	 * would, so request_count stops short and overlap_detected is set.
	 *
	 * The order and no-overlap assertions elsewhere in this file cannot catch that on
	 * their own: with the trigger discarded there is no out-of-order request to see.
	 * Counting the requests is what makes the omission visible.
	 */
	TEST_ASSERT_EQUAL_MESSAGE(3, request_count,
				  "a trigger was published before the previous search "
				  "reported done, and was discarded");
	TEST_ASSERT_FALSE_MESSAGE(overlap_detected,
				  "a trigger arrived while the library was still busy");

	/* The count above is what proves each trigger landed after the previous done; a
	 * trigger inside the gap is discarded by the fake and never reaches this array. All
	 * that is left to check here is that the pickup is prompt.
	 */
	for (uint8_t i = 1; i < request_count; i++) {
		TEST_ASSERT_LESS_THAN_MESSAGE(FAKE_RESULT_TO_DONE_MS + STEP_PICKUP_SLACK_MS,
					      requests[i].started_ms - requests[i - 1].finished_ms,
					      "the orchestrator idled between two steps");
	}
}

void test_a_second_request_is_refused_while_one_runs(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_EQUAL_MESSAGE(-EALREADY, survey_capture_request(SURVEY_PROFILE_FAST),
				  "overlapping cycles would attribute one cycle's radio work "
				  "to the other's timestamps");
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));
}

/* Late enough to outlive run_step()'s one-second wait for started, which is the point: a
 * stray done that only had to survive that wait could be dealt with by clearing the event
 * at the end of it, and this suite would not tell the difference.
 */
#define FAKE_LATE_STARTED_MS 1300

/* Generous against a fake cycle's ~300 ms: this bounds a hang, it does not measure timing,
 * and a tight bound here would make the suite flaky on a loaded CI host for no gain.
 */
#define POLL_DEADLINE_MS 10000

void test_a_stray_done_before_started_does_not_end_the_step(void)
{
	stray_done_before_started = true;
	started_delay_ms = FAKE_LATE_STARTED_MS;

	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(30)));

	/* This is the test that fails if the orchestrator latches a done that arrived
	 * before its own search started. It would end step 1 immediately, with the fake
	 * still busy, and skip the wind-down wait -- so step 2's trigger is discarded, the
	 * cycle stops short of three requests, and the record it writes describes step 1's
	 * timestamps around step 2's radio work.
	 */
	TEST_ASSERT_FALSE_MESSAGE(overlap_detected,
				  "a trigger was published while the previous search was still "
				  "running; a done from before the search started was taken "
				  "for the search's own");
	TEST_ASSERT_EQUAL_MESSAGE(3, request_count,
				  "the cycle stopped short: a stray done ended a step early and "
				  "the next trigger was discarded");
	TEST_ASSERT_TRUE_MESSAGE(record_stored, "a complete cycle stored nothing");
}

void test_busy_is_asserted_for_the_whole_cycle_including_between_steps(void)
{
	uint8_t steps_seen = 0;
	int64_t deadline_ms;

	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));

	/* location.c publishes a synthetic LOCATION_SEARCH_DONE when it suppresses the
	 * application's own search trigger, and suppresses that publish while a cycle is
	 * busy -- because a DONE arriving mid-cycle is indistinguishable from the cycle's
	 * own, ends the running step early with the radio still working, and can hand the
	 * location module back to INACTIVE mid-search so the next request answers -EBUSY
	 * and resets the device.
	 *
	 * That guard is only as good as this: busy has to stay asserted continuously, not
	 * just while a search is in flight. The gaps between steps are the dangerous
	 * window, and they are exactly where a sampled busy flag would read false.
	 *
	 * Near-tautological as written, and worth saying so: busy is derived from the
	 * cycle slot, which is taken before the first trigger and released at the end, so
	 * today the property holds by construction. It is here for the reimplementation --
	 * a flag set by the capture thread, or one cleared per step -- which is the shape
	 * that would break location.c's guard without breaking anything else.
	 */
	/* Bounded, unlike the property it is polling for. An unbounded wait here would turn
	 * the regression this test exists to catch -- a cycle that stops short of three
	 * steps -- into a hang, and Twister kills the binary on a hang, taking the other
	 * thirteen results in this file with it. A bounded wait fails one test by name.
	 */
	deadline_ms = k_uptime_get() + POLL_DEADLINE_MS;

	while (steps_seen < 3) {
		TEST_ASSERT_TRUE_MESSAGE(k_uptime_get() < deadline_ms,
					 "the cycle did not reach three steps in time; it stopped "
					 "short rather than busy having misreported");

		TEST_ASSERT_TRUE_MESSAGE(survey_capture_busy(),
					 "busy read false part-way through a cycle; a suppressed "
					 "application trigger would publish a synthetic done "
					 "into the middle of it");

		/* The gaps between steps are the point: a per-step flag would read false
		 * here, and the synthetic done it let through would be waited on by the
		 * next step as if it were that step's own.
		 */
		TEST_ASSERT_TRUE_MESSAGE(survey_capture_radio_busy(),
					 "the radio read free part-way through a cycle, between "
					 "two of its own steps");

		if (request_count > steps_seen) {
			steps_seen = request_count;
		}

		k_sleep(K_MSEC(5));
	}

	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));
	TEST_ASSERT_FALSE_MESSAGE(survey_capture_busy(),
				  "busy stayed asserted after the cycle ended, so no suppressed "
				  "trigger would ever be answered again");
}

void test_the_radio_is_reported_free_before_the_store_begins(void)
{
	/* The two predicates have to disagree during the store, and this is the only moment
	 * they can be caught disagreeing from inside the module's own API.
	 *
	 * What rides on it: location.c suppresses main.c's sampling trigger while the survey
	 * owns the search, and publishes a synthetic LOCATION_SEARCH_DONE in its place --
	 * except while the radio is busy, where that DONE would be mistaken for a step's own.
	 * DONE is the only exit from main.c's two SAMPLING states, and the store is the one
	 * stretch of a cycle with no further DONE coming, so if the guard there consulted
	 * survey_capture_busy() a trigger landing during the store would be swallowed and
	 * main would sit in SAMPLING with its sample timer stopped -- no periodic power or
	 * environmental samples, and nothing in the log. This test is what stops
	 * survey_capture_radio_busy() from being quietly reimplemented as an alias for
	 * survey_capture_busy(), which is the change that would restore that bug.
	 */
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_TRUE_MESSAGE(record_stored, "a complete cycle stored nothing");
	TEST_ASSERT_TRUE_MESSAGE(busy_at_store,
				 "the cycle was not busy during its own store, so the shell "
				 "could have interleaved a manual record with it");
	TEST_ASSERT_FALSE_MESSAGE(radio_busy_at_store,
				  "the radio still read busy once the last step was over; a "
				  "suppressed application trigger arriving during the store "
				  "would go unanswered and strand main.c in SAMPLING");
}

void test_a_cycle_can_be_run_again_after_the_previous_one_finished(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	reset_fake();

	TEST_ASSERT_EQUAL_MESSAGE(0, survey_capture_request(SURVEY_PROFILE_FAST),
				  "the slot was not released when the cycle ended");
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));
	TEST_ASSERT_EQUAL(3, request_count);
}

void test_stored_record_brackets_the_scan_with_two_fixes(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_TRUE_MESSAGE(record_stored, "a complete cycle was not stored");
	TEST_ASSERT_TRUE(stored_record.gnss_before_valid);
	TEST_ASSERT_TRUE(stored_record.scan_valid);
	TEST_ASSERT_TRUE(stored_record.gnss_after_valid);

	/* The dropped-AP count has to survive the snapshot into the record. It is one
	 * assignment in survey_capture.c and nothing else asserted it: deleting that line
	 * left every suite green while every record written in the field silently lost the
	 * field, and the loss is invisible in the export -- absent means "nothing was
	 * dropped", which is exactly what a missing assignment looks like.
	 */
	TEST_ASSERT_EQUAL_UINT16_MESSAGE(2, stored_record.scan_local_mac_dropped,
					 "the dropped-AP count did not reach the record");
}

void test_all_offsets_are_populated_and_ordered(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_TRUE(stored_record.cell_start.valid);
	TEST_ASSERT_TRUE(stored_record.cell_end.valid);
	TEST_ASSERT_TRUE(stored_record.wifi_start.valid);
	TEST_ASSERT_TRUE(stored_record.wifi_end.valid);
	TEST_ASSERT_TRUE(stored_record.gnss_after_at.valid);

	/* The time base is the leading fix, so every offset is at or after zero, the scan
	 * window is closed, and the trailing fix is outside it. This is exactly what the
	 * host needs to interpolate to the scan midpoint; if any of it were reversed the
	 * interpolation would still produce a number, just a wrong one.
	 */
	TEST_ASSERT_GREATER_OR_EQUAL(0, stored_record.cell_start.ms);
	TEST_ASSERT_GREATER_OR_EQUAL(stored_record.cell_start.ms, stored_record.cell_end.ms);
	TEST_ASSERT_GREATER_OR_EQUAL(stored_record.cell_end.ms, stored_record.gnss_after_at.ms);

	/* Cell and Wi-Fi are one combined request, so their windows are deliberately the
	 * same. Documented in survey_capture.c: a split would have to be invented.
	 */
	TEST_ASSERT_EQUAL(stored_record.cell_start.ms, stored_record.wifi_start.ms);
	TEST_ASSERT_EQUAL(stored_record.cell_end.ms, stored_record.wifi_end.ms);
}

void test_timing_reports_each_step(void)
{
	struct survey_capture_timing timing;

	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	survey_capture_timing_get(&timing);

	TEST_ASSERT_TRUE(timing.valid);
	TEST_ASSERT_TRUE(timing.gnss_before_ok);
	TEST_ASSERT_TRUE(timing.scan_ok);
	TEST_ASSERT_TRUE(timing.gnss_after_ok);

	/* The fake sleeps 50 ms per step, so each step must show at least that. A zero here
	 * would mean the duration is being measured across the wrong boundary -- which
	 * looks like a fast device rather than a broken measurement.
	 */
	TEST_ASSERT_GREATER_OR_EQUAL(FAKE_SEARCH_MS, timing.gnss_before_ms);
	TEST_ASSERT_GREATER_OR_EQUAL(FAKE_SEARCH_MS, timing.scan_ms);
	TEST_ASSERT_GREATER_OR_EQUAL(FAKE_SEARCH_MS, timing.gnss_after_ms);

	/* A step ends when its result arrives, not when the search reports done. If the
	 * wind-down wait were folded into the step, each would also carry
	 * FAKE_RESULT_TO_DONE_MS -- so an upper bound is what makes this measurable rather
	 * than merely non-zero. Generous, because native_sim scheduling is not exact.
	 */
	TEST_ASSERT_LESS_THAN_MESSAGE(FAKE_SEARCH_MS + FAKE_RESULT_TO_DONE_MS,
				      timing.gnss_before_ms,
				      "the step duration includes the wind-down wait, which is "
				      "not radio time and does not scale like it");
	TEST_ASSERT_LESS_THAN(FAKE_SEARCH_MS + FAKE_RESULT_TO_DONE_MS, timing.scan_ms);
	TEST_ASSERT_LESS_THAN(FAKE_SEARCH_MS + FAKE_RESULT_TO_DONE_MS, timing.gnss_after_ms);

	/* The whole cycle is longer than its steps: the gaps are the three wind-down waits.
	 * A total equal to the sum would mean the waits are not happening at all.
	 */
	TEST_ASSERT_GREATER_THAN_MESSAGE(timing.gnss_before_ms + timing.scan_ms +
					 timing.gnss_after_ms + timing.store_ms,
					 timing.total_ms,
					 "the cycle took no longer than its steps, so nothing "
					 "waited for a search to report done");
}

void test_a_cycle_without_gnss_is_still_stored(void)
{
	/* Indoors this is the common case, and the radio observations are the measurement;
	 * the fixes are the ground truth they are scored against. Discarding the cycle
	 * would throw away the data the survey exists to collect.
	 */
	step_succeeds[0] = false;
	step_succeeds[2] = false;

	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_EQUAL_MESSAGE(3, request_count,
				  "a failed GNSS step must not abort the cycle");
	TEST_ASSERT_TRUE_MESSAGE(record_stored, "a scan-only cycle was discarded");
	TEST_ASSERT_FALSE(stored_record.gnss_before_valid);
	TEST_ASSERT_FALSE(stored_record.gnss_after_valid);
	TEST_ASSERT_TRUE(stored_record.scan_valid);
}

void test_a_cycle_that_produced_nothing_is_not_stored(void)
{
	for (int i = 0; i < MAX_REQUESTS; i++) {
		step_succeeds[i] = false;
	}

	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_EQUAL(3, request_count);
	TEST_ASSERT_FALSE_MESSAGE(record_stored,
				  "an empty record occupies a permanent slot on flash and "
				  "says nothing");
}

void test_sequence_numbers_increase_across_cycles(void)
{
	uint32_t first;

	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));
	first = stored_record.sequence;

	reset_fake();

	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_FAST));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_EQUAL_MESSAGE(first + 1, stored_record.sequence,
				  "records must be orderable after the fact");
}

void test_profile_is_recorded_on_the_record(void)
{
	TEST_ASSERT_EQUAL(0, survey_capture_request(SURVEY_PROFILE_DEEP));
	TEST_ASSERT_TRUE(wait_for_cycle(K_SECONDS(10)));

	TEST_ASSERT_EQUAL(SURVEY_PROFILE_DEEP, stored_record.profile);
}

extern int unity_main(void);

int main(void)
{
	/* The capture thread starts at boot and the fake needs a moment to reach its
	 * wait; without this the first request can be published before anyone observes
	 * the channel.
	 */
	k_sleep(K_MSEC(100));

	return unity_main();
}
