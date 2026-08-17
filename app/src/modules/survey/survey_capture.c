/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#if defined(CONFIG_TASK_WDT)
#include <zephyr/task_wdt/task_wdt.h>
#include "app_common.h"
#endif

#include "location.h"
#include "survey_capture.h"
#include "survey_obs.h"
#include "survey_record.h"
#include "survey_store.h"
#include "survey.h"
#if defined(CONFIG_APP_LED)
#include "led.h"
#endif

#if defined(CONFIG_APP_SURVEY_LOG_LEVEL)
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(survey_capture, CONFIG_APP_SURVEY_LOG_LEVEL);
#else
#define LOG_INF(...)
#define LOG_WRN(...)
#define LOG_DBG(...)
#define LOG_ERR(...)
#endif

/* Events posted by the zbus listener in survey.c and consumed by the capture thread.
 *
 * A k_event rather than a message queue because nothing but the fact of the event is
 * needed: the data itself is already in the observation cache, which the capture thread
 * snapshots. Copying a location_msg here would mean a second copy of a structure that is
 * over a kilobyte, on a channel that is already the largest in the application.
 */
#define EVT_GNSS_DATA  BIT(0)
#define EVT_CLOUD_REQ  BIT(1)
#define EVT_DONE       BIT(2)
#define EVT_STARTED    BIT(3)

static K_EVENT_DEFINE(capture_events);

/* Admission: taking it is what makes a request the one that runs. A binary semaphore
 * rather than a flag because "is a cycle running" and "may I start one" have to be one
 * atomic question, and survey_capture_request() can be called from any thread.
 */
static K_SEM_DEFINE(cycle_slot, 1, 1);

/* True from admission until the cycle's third step ends -- a strict subset of cycle_slot,
 * which stays held through the store as well.
 *
 * Set by the requester rather than by the capture thread, for the same reason
 * survey_capture_busy() reads the slot: the capture thread is lowest priority and may not
 * run for a while after the request is accepted, and a false answer in that gap is exactly
 * the reordering hazard this flag exists to prevent.
 *
 * Atomic because location.c reads it from the location module thread.
 */
static atomic_t radio_busy;

/* Handed from the requester to the capture thread under cycle_slot. */
static enum survey_profile requested_profile;
static K_SEM_DEFINE(cycle_go, 0, 1);

static struct survey_capture_timing last_timing;
static K_MUTEX_DEFINE(timing_lock);

/* Both large enough that they belong in bss rather than on the capture thread's stack:
 * a survey_observation carries a location_cloud_request_data, and a survey_record_data
 * carries that plus two location_data. Only the capture thread touches them.
 */
static struct survey_observation cycle_obs;
static struct survey_record_data cycle_record;

/* Task watchdog.
 *
 * This thread can block indefinitely in exactly one place: survey_store_publish_record()
 * publishes with K_FOREVER, so a storage thread that stops consuming blocks it forever --
 * and because cycle_slot is released only after the cycle, surveying would then stop
 * permanently, with nothing to show for it but "a capture is already running" on every
 * subsequent command. On a device that is supposed to collect unattended for a week, a
 * silent permanent stop is the worst available failure. The watchdog turns it into a
 * reset, which is the behaviour asked for: the device comes back and keeps recording.
 *
 * Fed before every step rather than once per cycle, so the timeout only has to cover the
 * longest single wait rather than the whole cycle.
 */
#if defined(CONFIG_TASK_WDT)
#define WDT_FEED_INTERVAL_SECONDS 10

/* The longest interval between two feeds is a step that fails: 1 s to publish the trigger,
 * 1 s waiting for LOCATION_SEARCH_STARTED, the step timeout itself, and then the whole idle
 * timeout in the failure path. The two 1 s waits are what the earlier form of this assert
 * left out -- it compared against the step timeout alone, which passes at a GNSS timeout
 * that is in fact two seconds too long and reboots the device on every slow fix.
 */
#define WDT_STEP_OVERHEAD_SECONDS 4

BUILD_ASSERT(CONFIG_APP_SURVEY_CAPTURE_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS +
	     CONFIG_APP_SURVEY_CAPTURE_IDLE_TIMEOUT_SECONDS +
	     WDT_STEP_OVERHEAD_SECONDS,
	     "The watchdog must outlast the longest step, or a slow fix reboots the device");

/* This thread is the ninth task-watchdog client: upstream uses all eight that prj.conf
 * allocates (main, power, fota, location, network, environmental, storage, cloud).
 * Without the ninth, task_wdt_add() returns -ENOMEM and wdt_register() calls
 * SEND_FATAL_ERROR() -- a boot loop caused purely by a missing overlay. Caught here
 * instead, because a build error naming the file to edit costs a minute and the boot
 * loop cost an afternoon and twelve red hardware tests that all looked unrelated.
 */
BUILD_ASSERT(CONFIG_TASK_WDT_CHANNELS >= 9,
	     "The capture thread needs a ninth task-watchdog channel; see overlay-survey.conf");

static void capture_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static int wdt_register(void)
{
	int id = task_wdt_add(CONFIG_APP_SURVEY_CAPTURE_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC,
			      capture_wdt_callback, (void *)k_current_get());

	if (id < 0) {
		/* -ENOMEM here means CONFIG_TASK_WDT_CHANNELS is one short: upstream uses
		 * every one of the eight prj.conf allocates, and this thread is the ninth.
		 * overlay-survey.conf raises it. Fatal rather than tolerated, because the
		 * whole point of the channel is that a permanent stop must not be silent.
		 */
		LOG_ERR("Failed to add capture thread to watchdog: %d", id);
		SEND_FATAL_ERROR();

		return id;
	}

	/* Announced positively, not just on failure. The registration failing left every
	 * radio test on the bench red at once and the cause was one line in a boot log
	 * nothing asserted on; a line that is present when the thread *is* covered is
	 * something a test can require. Absence of an error is not evidence -- it is also
	 * what a build with the whole watchdog block deleted looks like.
	 */
	LOG_INF("Capture thread watchdog channel %d, timeout %d s", id,
		CONFIG_APP_SURVEY_CAPTURE_WATCHDOG_TIMEOUT_SECONDS);

	return id;
}

static void wdt_feed(int id)
{
	if (id >= 0) {
		(void)task_wdt_feed(id);
	}
}
#else
/* Unit builds: native_sim has no watchdog subsystem and the suite tests sequencing, not
 * supervision. The register/feed calls stay in the code path so the sequencing under test
 * is the code that ships.
 */
#define WDT_FEED_INTERVAL_SECONDS 10
static int wdt_register(void) { return -1; }
static void wdt_feed(int id) { ARG_UNUSED(id); }
#endif /* CONFIG_TASK_WDT */

void survey_capture_notify(const struct location_msg *msg)
{
	switch (msg->type) {
	case LOCATION_GNSS_DATA:
		k_event_post(&capture_events, EVT_GNSS_DATA);
		break;
	case LOCATION_CLOUD_REQUEST:
		k_event_post(&capture_events, EVT_CLOUD_REQ);
		break;
	case LOCATION_SEARCH_STARTED:
		k_event_post(&capture_events, EVT_STARTED);
		break;
	case LOCATION_SEARCH_DONE:
		/* Dropped until our own search has started. The library reports started
		 * before done for every request it accepts, so a done arriving while
		 * EVT_STARTED is clear cannot be ours -- today it is location.c's synthetic
		 * done for a suppressed application trigger. Latched, it would satisfy the
		 * step's wait immediately and end the step with the radio still working,
		 * and the stored record would carry this step's timestamps around the next
		 * step's measurements.
		 *
		 * Gated here rather than cleared in run_step() after its started handshake:
		 * that handshake gives up after a second, and a stray done latched before a
		 * slower started would survive the clear and be waited on straight after.
		 * Here the ordering is the channel's, not the thread's.
		 *
		 * If started were never reported the step would wait out its timeout and
		 * then the wind-down, log both, and carry on with no record corrupted --
		 * which is why this is gated on started rather than the other way round.
		 * APP_SURVEY_CAPTURE selects LOCATION_DATA_DETAILS so that it is reported.
		 */
		if (k_event_wait(&capture_events, EVT_STARTED, false, K_NO_WAIT) & EVT_STARTED) {
			k_event_post(&capture_events, EVT_DONE);
		}
		break;
	default:
		break;
	}
}

/* Static rather than on the stack: struct location_msg is over a kilobyte, and this
 * function is the deepest frame on the capture thread -- zbus_chan_pub() runs the whole
 * listener chain synchronously on this stack on top of it. Only the capture thread calls
 * trigger(), so a single shared buffer is safe.
 */
static struct location_msg trigger_msg;

static int trigger(enum location_msg_type type)
{
	memset(&trigger_msg, 0, sizeof(trigger_msg));
	trigger_msg.type = type;

	/* A finite timeout, unlike the store path: this is a trigger, and the location
	 * module is a message subscriber whose queue is short. A publish that cannot
	 * complete in a second means the module is not consuming, which is a failed cycle
	 * rather than something to wait out.
	 */
	return zbus_chan_pub(&location_chan, &trigger_msg, K_SECONDS(1));
}

/* Wait for one of `wanted`, returning the events that fired.
 *
 * EVT_DONE is always waited for alongside the wanted event, because it is the only
 * indication that a request that produced nothing has finished. Triggering the next step
 * before then would have it silently discarded by the location module, and the cycle would
 * then attribute this step's radio work to the next step's timestamps.
 */
static uint32_t wait_for(uint32_t wanted, k_timeout_t timeout)
{
	return k_event_wait(&capture_events, wanted | EVT_DONE, false, timeout);
}

/* Wait for the request to wind down after its result has been seen.
 *
 * The library publishes the data first and finishes afterwards. Not waiting is the
 * difference between a sequenced cycle and one whose second trigger is dropped.
 */
static void wait_for_idle(k_timeout_t timeout)
{
	if (!(k_event_wait(&capture_events, EVT_DONE, false, timeout) & EVT_DONE)) {
		LOG_WRN("Location search did not report done; the next step may be dropped");
	}
}

static void offset_set(struct survey_record_offset *off, int64_t t_base_ms, int64_t now_ms)
{
	int64_t delta = now_ms - t_base_ms;

	/* The field is int32_t: a cycle would have to run for 24 days to overflow it. If
	 * one somehow did, record the offset as absent rather than as a wrapped number that
	 * analysis would silently interpolate against.
	 */
	if (delta < INT32_MIN || delta > INT32_MAX) {
		off->valid = false;
		return;
	}

	off->valid = true;
	off->ms = (int32_t)delta;
}

/* When a step began and ended, on both clocks.
 *
 * Two clocks because they answer different questions and only one of them is safe for
 * each. `*_at_ms` is survey_time_now(), which is what the record's offsets have to be in
 * -- they are relative to a GNSS timestamp on the same base. `*_uptime_ms` is
 * k_uptime_get(), which is what the *durations* have to be in: survey_time_now() switches
 * from uptime to Unix the moment date_time syncs, and the LTE scan step is exactly when
 * that happens, so a duration differenced across it would be about 1.7e12 ms.
 */
struct step_time {
	int64_t started_at_ms;
	int64_t ended_at_ms;
	int64_t started_uptime_ms;
	int64_t ended_uptime_ms;
};

static int32_t step_duration_ms(const struct step_time *t)
{
	return (int32_t)(t->ended_uptime_ms - t->started_uptime_ms);
}

/* One step: trigger, wait for its result, wait for the library to go idle.
 *
 * Returns true when the wanted event arrived. `t` is always filled in, including on every
 * failure path, so no caller can read an uninitialised timestamp -- an earlier version
 * returned before setting it when the trigger publish failed, and the garbage propagated
 * into the reported scan and trailing-fix durations.
 *
 * The step's window closes when its result arrives, not when the library reports done: the
 * wind-down wait afterwards is bookkeeping, and folding up to IDLE_TIMEOUT of it into the
 * step would inflate the number the capture cadence is chosen from.
 */
static bool run_step(enum location_msg_type type, uint32_t wanted, k_timeout_t timeout,
		     struct step_time *t)
{
	enum survey_time_base base;
	uint32_t events;
	int err;

	k_event_clear(&capture_events, EVT_GNSS_DATA | EVT_CLOUD_REQ | EVT_DONE | EVT_STARTED);

	survey_time_now(&t->started_at_ms, &base);
	t->started_uptime_ms = k_uptime_get();
	t->ended_at_ms = t->started_at_ms;
	t->ended_uptime_ms = t->started_uptime_ms;

	err = trigger(type);
	if (err) {
		LOG_WRN("Failed to publish trigger %d: %d", (int)type, err);
		return false;
	}

	/* Best effort: if the library reports a start, use it -- the module may sit on a
	 * trigger while the modem finishes something else, and the publish time would then
	 * put the scan window earlier than the scan. If no start arrives within a second,
	 * keep the publish time. Either way the field is a real timestamp, and the
	 * difference between the two is bounded by this wait.
	 */
	if (k_event_wait(&capture_events, EVT_STARTED, false, K_SECONDS(1)) & EVT_STARTED) {
		survey_time_now(&t->started_at_ms, &base);
		t->started_uptime_ms = k_uptime_get();
	}

	events = wait_for(wanted, timeout);

	survey_time_now(&t->ended_at_ms, &base);
	t->ended_uptime_ms = k_uptime_get();

	if (!(events & wanted)) {
		/* Either the search finished without producing what was asked for, or it
		 * has not finished at all. In the second case the cycle must not proceed --
		 * the modem is still busy and the next trigger would be discarded -- so wait
		 * for it to wind down rather than cancelling. LOCATION_SEARCH_CANCEL cannot
		 * truly cancel a Wi-Fi scan and poisons the next request with -EBUSY.
		 *
		 * A bounded wait is enough because location.c caps the library's own timeout
		 * at this same step timeout for the survey's triggers; done is due within
		 * seconds of here, not minutes. If it still does not arrive, proceeding is
		 * better than blocking: the next trigger is dropped, and location.c counts
		 * that where "survey show" can report it.
		 */
		if (!(events & EVT_DONE)) {
			LOG_WRN("Step timed out with the search still active; waiting it out");
			wait_for_idle(K_SECONDS(CONFIG_APP_SURVEY_CAPTURE_IDLE_TIMEOUT_SECONDS));
		}

		return false;
	}

	wait_for_idle(K_SECONDS(CONFIG_APP_SURVEY_CAPTURE_IDLE_TIMEOUT_SECONDS));

	return true;
}

/* FW-9: an operator with no console has exactly the LED to tell whether the device is
 * working. Five states, latched until the next publish overwrites them (repetitions = -1),
 * so the pattern never goes dark and reads as "device is off" between cycles:
 *
 *  - SEARCHING latches for the duration of a cycle, so a hang mid-cycle is visible as
 *    "still searching" rather than looking identical to the previous cycle's result.
 *  - CAPTURE_OK latches when a cycle stored a record with at least one real GNSS fix --
 *    the record can be interpolated against ground truth, which is the point of the survey.
 *  - GNSS_POOR latches when a cycle stored a record, but neither bracket produced a fix.
 *    The radio observation is still saved (see survey_store_publish_record's -EINVAL
 *    rationale), but the operator should know positioning is degraded, e.g. move to open
 *    sky.
 *  - STORAGE_FULL latches when the survey storage type is at capacity under
 *    APP_STORAGE_FULL_STOP, so further records are being silently dropped -- see
 *    survey_store_is_full()'s doc comment for why this can only be caught proactively.
 *    Checked ahead of GNSS_POOR/CAPTURE_OK: an operator who can only see one color needs to
 *    know recording has stopped before they need to know fix quality.
 *  - ERROR latches when a cycle produced nothing to store (every step failed) or the store
 *    itself failed for a reason other than being full.
 *
 * Colors are the existing dim (~55/255) palette, chosen so no two states share a hue:
 * blue/green/amber/magenta/red.
 */
enum survey_led_state {
	SURVEY_LED_SEARCHING,
	SURVEY_LED_CAPTURE_OK,
	SURVEY_LED_GNSS_POOR,
	SURVEY_LED_STORAGE_FULL,
	SURVEY_LED_ERROR,
};

#if defined(CONFIG_APP_LED)
static void survey_led_signal(enum survey_led_state state)
{
	struct led_msg led_msg = {
		.type = LED_RGB_SET,
		.repetitions = -1,
	};

	switch (state) {
	case SURVEY_LED_SEARCHING:
		led_msg.blue = 55;
		led_msg.duration_on_msec = 250;
		led_msg.duration_off_msec = 250;
		break;
	case SURVEY_LED_CAPTURE_OK:
		led_msg.green = 55;
		led_msg.duration_on_msec = 250;
		led_msg.duration_off_msec = 2000;
		break;
	case SURVEY_LED_GNSS_POOR:
		/* Green raised close to red (rather than a duller ~25) so this reads as amber,
		 * not "dim red" -- easy to mistake for ERROR at a glance through a windshield,
		 * even though the two also differ in blink rate (slow here vs. fast for ERROR).
		 */
		led_msg.red = 55;
		led_msg.green = 45;
		led_msg.duration_on_msec = 250;
		led_msg.duration_off_msec = 2000;
		break;
	case SURVEY_LED_STORAGE_FULL:
		led_msg.red = 55;
		led_msg.blue = 55;
		led_msg.duration_on_msec = 250;
		led_msg.duration_off_msec = 2000;
		break;
	case SURVEY_LED_ERROR:
	default:
		led_msg.red = 55;
		led_msg.duration_on_msec = 250;
		led_msg.duration_off_msec = 250;
		break;
	}

	int err = zbus_chan_pub(&led_chan, &led_msg, K_MSEC(100));

	if (err) {
		LOG_WRN("Failed to publish survey LED pattern: %d", err);
	}
}
#else
static inline void survey_led_signal(enum survey_led_state state)
{
	ARG_UNUSED(state);
}
#endif /* CONFIG_APP_LED */

static void capture_cycle(enum survey_profile profile, int wdt_id)
{
	struct survey_capture_timing timing = {
		.valid = true,
		.sequence = survey_store_next_sequence(),
	};
	struct step_time gnss_before, scan, gnss_after;
	enum survey_time_base base;
	int64_t t_base_ms, cycle_started_uptime_ms, store_started_uptime_ms;
	int err;

	cycle_started_uptime_ms = k_uptime_get();
	survey_led_signal(SURVEY_LED_SEARCHING);

	memset(&cycle_record, 0, sizeof(cycle_record));
	cycle_record.sequence = timing.sequence;
	cycle_record.profile = profile;

	/* Step 1: the leading bracket. Its timestamp is the record's time base, so that
	 * every offset in the record is relative to a real measurement rather than to
	 * whenever the orchestrator happened to start.
	 */
	wdt_feed(wdt_id);
	timing.gnss_before_ok = run_step(LOCATION_GNSS_SEARCH_TRIGGER, EVT_GNSS_DATA,
					 K_SECONDS(CONFIG_APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS),
					 &gnss_before);
	timing.gnss_before_ms = step_duration_ms(&gnss_before);
	survey_obs_snapshot(&cycle_obs);

	if (timing.gnss_before_ok && cycle_obs.gnss_valid) {
		cycle_record.gnss_before_valid = true;
		cycle_record.gnss_before = cycle_obs.gnss;
		t_base_ms = cycle_obs.gnss_timestamp;
		cycle_record.time_base = cycle_obs.gnss_time_base;
	} else {
		/* No leading fix. The radio observations are still the measurement and are
		 * still worth storing; they simply cannot be interpolated against ground
		 * truth, and the decoder refuses to try when a bracket is missing. Base the
		 * offsets on the clock so the scan window is still internally consistent.
		 */
		survey_time_now(&t_base_ms, &base);
		cycle_record.time_base = base;
		timing.gnss_before_ok = false;
	}

	/* Step 2: the radio scan. Wi-Fi and cellular are one combined request -- see
	 * location.c -- so the library reports one elapsed time for the pair and there is
	 * no per-leg timing to record. Both windows are therefore set to the observed
	 * combined window rather than split on a guess. That is conservative in the right
	 * direction: the host's uncertainty model widens with the scan duration, so an
	 * honestly-wide window overstates uncertainty where a fabricated split would
	 * understate it. Separate cell-only and Wi-Fi-only requests would give true
	 * per-leg windows and cost another cycle; see the open question in REQUIREMENTS.md.
	 */
	wdt_feed(wdt_id);
	timing.scan_ok = run_step(LOCATION_SCAN_SEARCH_TRIGGER, EVT_CLOUD_REQ,
				  K_SECONDS(CONFIG_APP_SURVEY_CAPTURE_SCAN_TIMEOUT_SECONDS),
				  &scan);
	timing.scan_ms = step_duration_ms(&scan);

	if (timing.scan_ok) {
		survey_obs_snapshot(&cycle_obs);
		if (cycle_obs.scan_valid) {
			cycle_record.scan_valid = true;
			cycle_record.scan = cycle_obs.scan;
			cycle_record.scan_local_mac_dropped =
				cycle_obs.scan_local_mac_dropped;
			offset_set(&cycle_record.cell_start, t_base_ms, scan.started_at_ms);
			offset_set(&cycle_record.wifi_start, t_base_ms, scan.started_at_ms);
			offset_set(&cycle_record.cell_end, t_base_ms, scan.ended_at_ms);
			offset_set(&cycle_record.wifi_end, t_base_ms, scan.ended_at_ms);
		}
	}

	/* Step 3: the trailing bracket. */
	wdt_feed(wdt_id);
	timing.gnss_after_ok = run_step(LOCATION_GNSS_SEARCH_TRIGGER, EVT_GNSS_DATA,
					K_SECONDS(CONFIG_APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS),
					&gnss_after);
	timing.gnss_after_ms = step_duration_ms(&gnss_after);

	if (timing.gnss_after_ok) {
		survey_obs_snapshot(&cycle_obs);
		if (cycle_obs.gnss_valid) {
			cycle_record.gnss_after_valid = true;
			cycle_record.gnss_after = cycle_obs.gnss;
			offset_set(&cycle_record.gnss_after_at, t_base_ms,
				   cycle_obs.gnss_timestamp);
		}
	}

	cycle_record.t_base_ms = t_base_ms;

	/* The radio is free from here on; everything below is bookkeeping and flash. Dropped
	 * before the store rather than with the slot, because the store is the long part: a
	 * suppressed application trigger arriving during it would otherwise get no
	 * LOCATION_SEARCH_DONE, and DONE is the only exit from main.c's two SAMPLING states.
	 * Main would sit there with its sample timer stopped -- no periodic power or
	 * environmental samples -- until some later cycle's DONE happened along, which at CP6,
	 * where cycles are started by hand, may be never.
	 *
	 * Safe to answer a trigger from here: the third step's real DONE has already been
	 * processed, so this module is back in its inactive state and a synthetic DONE falls
	 * through it, and the orchestrator is no longer waiting on one -- the next cycle's
	 * first step clears both events before it triggers.
	 */
	atomic_set(&radio_busy, 0);

	/* Timed separately from the three steps rather than folded into them. On target a
	 * single store has been measured at tens of seconds once the partition holds
	 * thousands of records (see REQUIREMENTS.md, CP5), which is larger than any of the
	 * steps -- charging it to the trailing fix would make the radio timings look like
	 * they degrade as the flash fills, and they do not.
	 */
	wdt_feed(wdt_id);
	store_started_uptime_ms = k_uptime_get();

	if (!cycle_record.scan_valid && !cycle_record.gnss_before_valid &&
	    !cycle_record.gnss_after_valid) {
		LOG_WRN("Cycle %u produced nothing; not stored", cycle_record.sequence);
		survey_led_signal(SURVEY_LED_ERROR);
	} else {
		err = survey_store_publish_record(&cycle_record);
		if (err) {
			LOG_WRN("Cycle %u not stored: %d", cycle_record.sequence, err);
			survey_led_signal(survey_store_is_full() ? SURVEY_LED_STORAGE_FULL :
								    SURVEY_LED_ERROR);
		} else {
			LOG_INF("Cycle %u stored: gnss %s/%s, scan %s",
				cycle_record.sequence,
				cycle_record.gnss_before_valid ? "ok" : "--",
				cycle_record.gnss_after_valid ? "ok" : "--",
				cycle_record.scan_valid ? "ok" : "--");
			if (survey_store_is_full()) {
				survey_led_signal(SURVEY_LED_STORAGE_FULL);
			} else if (cycle_record.gnss_before_valid ||
				   cycle_record.gnss_after_valid) {
				survey_led_signal(SURVEY_LED_CAPTURE_OK);
			} else {
				survey_led_signal(SURVEY_LED_GNSS_POOR);
			}
		}
	}

	timing.store_ms = (int32_t)(k_uptime_get() - store_started_uptime_ms);
	timing.total_ms = (int32_t)(k_uptime_get() - cycle_started_uptime_ms);

	k_mutex_lock(&timing_lock, K_FOREVER);
	last_timing = timing;
	k_mutex_unlock(&timing_lock);
}

static void capture_thread_fn(void *p1, void *p2, void *p3)
{
	int wdt_id;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	wdt_id = wdt_register();

	while (true) {
		enum survey_profile profile;

		/* Bounded rather than K_FOREVER so the idle thread still feeds the
		 * watchdog. Nothing is polled here; the wake-up exists only to feed.
		 */
		wdt_feed(wdt_id);

		if (k_sem_take(&cycle_go, K_SECONDS(WDT_FEED_INTERVAL_SECONDS)) != 0) {
			continue;
		}

		profile = requested_profile;

		capture_cycle(profile, wdt_id);

		/* Released only here: the slot is what serialises cycles, and it must
		 * outlive the whole cycle rather than just the request that started it.
		 */
		k_sem_give(&cycle_slot);
	}
}

K_THREAD_DEFINE(survey_capture_thread, CONFIG_APP_SURVEY_CAPTURE_THREAD_STACK_SIZE,
		capture_thread_fn, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

int survey_capture_request(enum survey_profile profile)
{
	if (k_sem_take(&cycle_slot, K_NO_WAIT) != 0) {
		return -EALREADY;
	}

	requested_profile = profile;
	atomic_set(&radio_busy, 1);
	k_sem_give(&cycle_go);

	return 0;
}

bool survey_capture_busy(void)
{
	/* The slot, not cycle_running: the slot is taken synchronously by the requester,
	 * whereas cycle_running is set by the capture thread, which is lowest priority and
	 * may not run for a while. Reading the flag left a window where a request had been
	 * accepted and "survey timing" still reported that nothing was running -- which is
	 * the line a bench operator reads to decide whether the command worked.
	 */
	return k_sem_count_get(&cycle_slot) == 0;
}

bool survey_capture_radio_busy(void)
{
	return atomic_get(&radio_busy) != 0;
}

void survey_capture_timing_get(struct survey_capture_timing *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&timing_lock, K_FOREVER);
	*out = last_timing;
	k_mutex_unlock(&timing_lock);
}

/* FW-7: the device has to record with nobody there to type "survey capture". Before this,
 * survey_capture_request() had exactly one caller -- the shell command -- so a headless
 * build did nothing forever. This timer is the second caller.
 *
 * Reschedules itself from the handler rather than using k_timer's own periodic mode, so
 * that a cycle running long (GNSS can take the full SURVEY_CAPTURE_WORST_CASE_SECONDS) is a
 * dropped tick, not a queued one: k_timer would fire again mid-cycle and every one of those
 * extra fires would see -EALREADY, but the interval between *starts* would drift shorter
 * than the interval that was set. Rescheduling only after the request is made, whatever the
 * result, keeps the interval a floor on the gap between cycles instead.
 */
static atomic_t cadence_interval_s = ATOMIC_INIT(CONFIG_APP_SURVEY_CAPTURE_INTERVAL_SECONDS);

int survey_capture_set_interval(uint32_t seconds)
{
	if (seconds == 0) {
		return -EINVAL;
	}

	atomic_set(&cadence_interval_s, (atomic_val_t)seconds);

	return 0;
}

uint32_t survey_capture_get_interval(void)
{
	return (uint32_t)atomic_get(&cadence_interval_s);
}

#if defined(CONFIG_APP_SURVEY_CAPTURE_CADENCE_AUTOSTART)
static void cadence_work_handler(struct k_work *work)
{
	int err = survey_capture_request(SURVEY_PROFILE_FAST);

	if (err && err != -EALREADY) {
		LOG_WRN("Automatic capture trigger failed: %d", err);
	}

	k_work_reschedule(k_work_delayable_from_work(work),
			   K_SECONDS(atomic_get(&cadence_interval_s)));
}

static K_WORK_DELAYABLE_DEFINE(cadence_work, cadence_work_handler);

static int cadence_start(void)
{
	/* APPLICATION, not an earlier level: the system workqueue this schedules onto must
	 * already be running, which it is by POST_KERNEL. survey_capture_request() itself
	 * only needs cycle_slot and cycle_go, both statically initialised, so it is safe to
	 * call from here even though the capture thread (K_THREAD_DEFINE below) is scheduled
	 * by the kernel rather than started by a SYS_INIT of its own.
	 */
	k_work_schedule(&cadence_work, K_SECONDS(atomic_get(&cadence_interval_s)));

	return 0;
}
SYS_INIT(cadence_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif /* CONFIG_APP_SURVEY_CAPTURE_CADENCE_AUTOSTART */
