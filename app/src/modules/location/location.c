/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/init.h>
#include <zephyr/smf.h>
#include <modem/location.h>
#include <nrf_modem_gnss.h>
#include <date_time.h>
#include <modem/nrf_modem_lib.h>

#include "app_common.h"
#include "modem/lte_lc.h"
#include "location.h"
#include "location_helper.h"
#if defined(CONFIG_APP_SURVEY)
/* Guarded so that a build without the survey leaves this upstream-owned file's include
 * block byte-identical, and every rebase conflict here is one the survey actually caused.
 */
#include "location_trigger_stats.h"
#endif
#if defined(CONFIG_APP_SURVEY_CAPTURE)
#include "survey_capture.h"
#else
/* The suppression branch below is guarded with IS_ENABLED(), which is dead code rather
 * than absent code, so the symbol still has to resolve in a build without the survey.
 */
static inline bool survey_capture_radio_busy(void)
{
	return false;
}
#endif

LOG_MODULE_REGISTER(location_module, CONFIG_APP_LOCATION_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
BUILD_ASSERT(CONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX >= CONFIG_LTE_NEIGHBOR_CELLS_MAX);
#endif /* CONFIG_LOCATION_METHOD_CELLULAR */

#if defined(CONFIG_LOCATION_METHOD_WIFI)
BUILD_ASSERT(CONFIG_APP_LOCATION_WIFI_APS_MAX >=
	     CONFIG_LOCATION_METHOD_WIFI_SCANNING_RESULTS_MAX_CNT);
#endif /* CONFIG_LOCATION_METHOD_WIFI */

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(location);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(location_chan,
		 struct location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Private channel message types for internal state management. */
enum priv_location_msg_type {
	/* Modem functional mode has been set. */
	LOCATION_PRIV_CFUN_REQUIRED_SET,
};

struct priv_location_msg {
	enum priv_location_msg_type type;
};

/* Create private location channel for internal messaging that is not intended for external use. */
ZBUS_CHAN_DEFINE(priv_location_chan,
		 struct priv_location_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(location),
		 ZBUS_MSG_INIT(0)
);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)								\
	X(location_chan,	struct location_msg)			\
	X(priv_location_chan,	struct priv_location_msg)			\

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE			MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add the location subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, location, 0);

/*
 * Expand to a call to ZBUS_CHAN_ADD_OBS for each channel in the list.
 * Example: ZBUS_CHAN_ADD_OBS(location_chan, location, 0);
 */
CHANNEL_LIST(ADD_OBSERVERS)

/* Forward declarations */
static void location_event_handler(const struct location_event_data *event_data);
static void on_cfun(int mode, void *ctx);

NRF_MODEM_LIB_ON_CFUN(location_cfun_hook, on_cfun, NULL);

static void on_cfun(int mode, void *ctx)
{
	int err;
	struct priv_location_msg msg = { .type = LOCATION_PRIV_CFUN_REQUIRED_SET };

	ARG_UNUSED(ctx);

	if ((mode == LTE_LC_FUNC_MODE_NORMAL) || (mode == LTE_LC_FUNC_MODE_ACTIVATE_LTE)) {
		err = zbus_chan_pub(&priv_location_chan, &msg, PUB_TIMEOUT);
		if (err) {
			LOG_ERR("zbus_chan_pub, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

/* State machine */

/* Location module states */
enum location_module_state {
	/* Waiting for modem functional mode to be set */
	STATE_WAITING_FOR_CFUN,
	/* The module is running */
	STATE_RUNNING,
		/* Location search is inactive. */
		STATE_LOCATION_SEARCH_INACTIVE,
		/* Location search is active. */
		STATE_LOCATION_SEARCH_ACTIVE,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct location_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];
};

/* Forward declarations of state handlers */
static enum smf_state_result state_waiting_for_cfun_run(void *obj);
static void state_running_entry(void *obj);
static void state_location_search_inactive_entry(void *obj);
static enum smf_state_result state_location_search_inactive_run(void *obj);
static void state_location_search_active_entry(void *obj);
static enum smf_state_result state_location_search_active_run(void *obj);

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_WAITING_FOR_CFUN] =
		SMF_CREATE_STATE(NULL,
				 state_waiting_for_cfun_run,
				 NULL,
				 NULL,
				 NULL),
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 NULL,
				 NULL,
				 NULL,
				 &states[STATE_LOCATION_SEARCH_INACTIVE]),
	[STATE_LOCATION_SEARCH_INACTIVE] =
		SMF_CREATE_STATE(state_location_search_inactive_entry,
				 state_location_search_inactive_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_LOCATION_SEARCH_ACTIVE] =
		SMF_CREATE_STATE(state_location_search_active_entry,
				 state_location_search_active_run,
				 NULL,
				 &states[STATE_RUNNING],
				 NULL),
};

static void location_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* True while the ongoing request came from LOCATION_SCAN_SEARCH_TRIGGER, which wants the
 * raw scan and nothing else. Only ever written from the module thread, before the request
 * that reads it is started.
 */
static bool scan_only_request;

#if defined(CONFIG_APP_SURVEY)
/* Trigger dispositions. Incremented only from the module thread; read from anywhere, so
 * atomics rather than plain counters. See location_trigger_stats.h for why a positive
 * acceptance token has to exist at all.
 *
 * Behind CONFIG_APP_SURVEY so that a build without the survey is byte-identical to
 * upstream here. The three increments below are IF_ENABLED for the same reason: the
 * maintainer rebases this file on upstream, and every unconditional line is rebase surface.
 */
static atomic_t triggers_accepted;
static atomic_t triggers_dropped;
static atomic_t triggers_suppressed;

void location_trigger_stats_get(struct location_trigger_stats *out)
{
	if (out == NULL) {
		return;
	}

	out->accepted = (uint32_t)atomic_get(&triggers_accepted);
	out->dropped = (uint32_t)atomic_get(&triggers_dropped);
	out->suppressed = (uint32_t)atomic_get(&triggers_suppressed);
}
#endif /* CONFIG_APP_SURVEY */

/* Bound the Location library to the same window the survey's capture step waits for.
 *
 * Without this the two disagree and the disagreement cascades: the step gives up at
 * APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS while the library keeps searching until
 * LOCATION_REQUEST_DEFAULT_GNSS_TIMEOUT (600 s in the survey overlay), so the next two
 * triggers of the cycle land while a search is still active and are discarded. The cycle
 * then reports three steps, stores nothing, and leaves the modem busy into the next cycle.
 *
 * The library's own doc asks for the overall request timeout to be "one minute or more
 * larger than the sum of method-specific timeouts", which is what the margin is.
 */
#if defined(CONFIG_APP_SURVEY_CAPTURE)
#define SURVEY_REQUEST_MARGIN_MS (60 * MSEC_PER_SEC)

static void survey_timeouts_apply(struct location_config *config, int32_t step_timeout_ms)
{
	/* Divided across the methods, not handed to each of them. The caller's budget is
	 * the one the capture orchestrator will wait out for the whole step; if each method
	 * got the full amount, a two-method scan could run for twice as long as the step
	 * that owns it. The orchestrator would then time out, give up waiting for idle, and
	 * publish step 3's trigger into a radio the library is still using -- that trigger
	 * is discarded, and the cycle stores a record missing its trailing fix. Dividing
	 * keeps the two from disagreeing about when the step is over.
	 */
	int32_t method_timeout_ms;

	/* location_config_defaults_set() always fills in at least one method, so this is a
	 * guard against a future caller rather than against today's -- but the failure it
	 * guards against is a divide-by-zero fault in the location thread, and the cost of
	 * not dividing is one step running long.
	 */
	if (config->methods_count == 0) {
		LOG_WRN("Location config has no methods; leaving the timeouts alone");

		config->timeout = step_timeout_ms + SURVEY_REQUEST_MARGIN_MS;

		return;
	}

	method_timeout_ms = step_timeout_ms / config->methods_count;

	for (uint8_t i = 0; i < config->methods_count; i++) {
		switch (config->methods[i].method) {
		case LOCATION_METHOD_GNSS:
			config->methods[i].gnss.timeout = method_timeout_ms;
			break;
		case LOCATION_METHOD_CELLULAR:
			config->methods[i].cellular.timeout = method_timeout_ms;
			break;
		case LOCATION_METHOD_WIFI:
			config->methods[i].wifi.timeout = method_timeout_ms;
			break;
		default:
			break;
		}
	}

	/* The library requires the overall timeout to exceed the sum of the per-method ones,
	 * which is now step_timeout_ms (modulo the division's remainder), so the margin is
	 * what satisfies that requirement rather than what sets the budget.
	 */
	config->timeout = step_timeout_ms + SURVEY_REQUEST_MARGIN_MS;
}
#endif /* CONFIG_APP_SURVEY_CAPTURE */

#if defined(CONFIG_LOCATION_METHOD_WIFI) || defined(CONFIG_LOCATION_METHOD_CELLULAR)
static void cloud_request_send(const struct location_data_cloud *cloud_request)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_CLOUD_REQUEST,
	};

	err = location_cloud_request_data_copy(&location_msg.cloud_request, cloud_request);
	if (err) {
		LOG_ERR("location_cloud_request_data_copy, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = zbus_chan_pub(&location_chan, &location_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}
#endif /* defined(CONFIG_LOCATION_METHOD_WIFI) || defined(CONFIG_LOCATION_METHOD_CELLULAR) */

#if defined(CONFIG_NRF_CLOUD_AGNSS)
static void agnss_request_send(const struct nrf_modem_gnss_agnss_data_frame *agnss_request)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_AGNSS_REQUEST,
		.agnss_request = *agnss_request
	};

	err = zbus_chan_pub(&location_chan, &location_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}
#endif /* defined(CONFIG_NRF_CLOUD_AGNSS) */

static void gnss_location_send(const struct location_data *location_data)
{
	int err;
	struct location_msg location_msg = {
		.type = LOCATION_GNSS_DATA,
		.gnss_data = *location_data,
		.timestamp = k_uptime_get()
	};

	err = date_time_now(&location_msg.timestamp);
	if (err != 0 && err != -ENODATA) {
		LOG_ERR("date_time_now, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = zbus_chan_pub(&location_chan, &location_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static void message_send(enum location_msg_type msg_type)
{
	int err;
	struct location_msg location_msg = {
		.type = msg_type
	};

	err = zbus_chan_pub(&location_chan, &location_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* State handlers */

static enum smf_state_result state_waiting_for_cfun_run(void *obj)
{
	struct location_state_object *state_object = obj;

	if (state_object->chan == &priv_location_chan) {
		const struct priv_location_msg *msg =
			(const struct priv_location_msg *)state_object->msg_buf;

		if (msg->type == LOCATION_PRIV_CFUN_REQUIRED_SET) {
			LOG_DBG("CFUN set, transitioning to running state");
			smf_set_state(SMF_CTX(state_object), &states[STATE_RUNNING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	int err;

	LOG_DBG("%s", __func__);

	err = location_init(location_event_handler);
	if (err) {
		LOG_ERR("Unable to init location library: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	const struct location_msg ready_msg = {
		.type = LOCATION_MODULE_READY
	};

	err = zbus_chan_pub(&location_chan, &ready_msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	LOG_DBG("Location library initialized");
}

static void state_location_search_inactive_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_location_search_inactive_run(void *obj)
{
	int err;
	struct location_state_object *state_object = obj;

	if (state_object->chan == &location_chan) {
		const struct location_msg *location_msg =
			(const struct location_msg *)state_object->msg_buf;

		if (location_msg->type == LOCATION_SEARCH_CANCEL) {
			LOG_DBG("Location search cancel received in inactive state, ignoring");
		} else if (IS_ENABLED(CONFIG_APP_SURVEY_CAPTURE_OWNS_SEARCH) &&
			   location_msg->type == LOCATION_SEARCH_TRIGGER) {
			/* The survey's capture orchestrator is the only thing that may start a
			 * search. main.c still publishes this on its sampling timer and once at
			 * startup, and two drivers cannot share the Location library: whichever
			 * trigger arrives second is discarded, so a capture step that loses the
			 * race silently attributes the other driver's radio work to its own
			 * timestamps.
			 *
			 * Dropped here rather than by not publishing it, so that the suppression
			 * is one decision in one place and main.c stays upstream.
			 */
			LOG_DBG("Default search trigger ignored; the survey owns the search");

			IF_ENABLED(CONFIG_APP_SURVEY, (atomic_inc(&triggers_suppressed);));

			/* Swallowing the trigger is not enough: LOCATION_SEARCH_DONE is the
			 * only exit from main.c's two SAMPLING states, so a trigger that
			 * produces no DONE leaves main in SAMPLING with its sample timer
			 * already stopped -- no periodic power or environmental samples, and
			 * nothing in the log, because main feeds its watchdog before waiting
			 * on zbus rather than after. It recovers only by accident, when a
			 * capture cycle's own DONE happens along.
			 *
			 * Answering immediately is also honest: no search was performed, so a
			 * search that takes zero time is exactly what happened. Safe to
			 * publish from here -- this module observes location_chan as a message
			 * subscriber, so the message is queued rather than delivered under the
			 * channel lock, and a DONE arriving in this state falls through.
			 *
			 * Only while the survey's radio work is in flight, though. A DONE
			 * published then is indistinguishable from that cycle's own,
			 * and it is not private to main.c: survey_capture waits on DONE
			 * alongside each step's data event, so it would end the step early with
			 * the radio still busy and stamp this step's record with the next one's
			 * work; and this module's own subscriber FIFO could deliver it after an
			 * already-queued capture trigger, returning us to INACTIVE mid-search so
			 * the following trigger is accepted, location_request() answers -EBUSY,
			 * and SEND_FATAL_ERROR() resets the device. Nothing is lost by staying
			 * quiet: the cycle's next step publishes a real DONE within seconds,
			 * and that one releases main just as well.
			 *
			 * survey_capture_radio_busy() and not survey_capture_busy(): a cycle
			 * stays busy through its store, which is tens of seconds once the
			 * partition fills and which has no more DONEs coming, so suppressing
			 * across the whole cycle would leave main in SAMPLING with nothing to
			 * release it -- until some later cycle's DONE happened along, which at
			 * CP6, where cycles are started by hand, may be never. Answering during
			 * the store is safe: the last step's real DONE has already put us back
			 * in this state, and the orchestrator is no longer waiting on one.
			 *
			 * Not a lock: a cycle could still begin between this check and the
			 * publish. That window is microseconds wide against a trigger that
			 * arrives on a timer measured in minutes, and the flag is raised by the
			 * requester rather than by the low-priority capture thread, so it does
			 * not stretch to cover the queueing delay. What makes the survivor
			 * harmless is that nothing else issues an accepted trigger during a
			 * cycle: the shell's are refused while busy, main's are suppressed
			 * here, and the orchestrator waits on DONE before each of its own. That
			 * is the property to preserve -- a future caller that starts a search
			 * without checking would break it -- rather than the width of the
			 * window. Locking across a zbus publish to close it would be the more
			 * dangerous of the two.
			 */
			if (!survey_capture_radio_busy()) {
				message_send(LOCATION_SEARCH_DONE);
			}
		} else if (location_msg->type == LOCATION_SEARCH_TRIGGER) {
			LOG_DBG("Location search trigger received");

			scan_only_request = false;

			err = location_request(NULL);
			if (err) {
				LOG_WRN("location_request, error: %d", err);
				SEND_FATAL_ERROR();

				return SMF_EVENT_HANDLED;
			}

			IF_ENABLED(CONFIG_APP_SURVEY, (atomic_inc(&triggers_accepted);));

			smf_set_state(SMF_CTX(state_object), &states[STATE_LOCATION_SEARCH_ACTIVE]);

			return SMF_EVENT_HANDLED;
		} else if (location_msg->type == LOCATION_GNSS_SEARCH_TRIGGER) {
			struct location_config config;
			enum location_method methods[] = {
				LOCATION_METHOD_GNSS
			};

			LOG_DBG("GNSS fix trigger received");

			scan_only_request = false;

			location_config_defaults_set(&config, 1, methods);

			IF_ENABLED(CONFIG_APP_SURVEY_CAPTURE, (survey_timeouts_apply(&config,
				CONFIG_APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS * MSEC_PER_SEC);));

			err = location_request(&config);
			if (err) {
				LOG_WRN("location_request, error: %d", err);
				SEND_FATAL_ERROR();

				return SMF_EVENT_HANDLED;
			}

			IF_ENABLED(CONFIG_APP_SURVEY, (atomic_inc(&triggers_accepted);));

			smf_set_state(SMF_CTX(state_object), &states[STATE_LOCATION_SEARCH_ACTIVE]);

			return SMF_EVENT_HANDLED;
		} else if (IS_ENABLED(CONFIG_APP_LOCATION_SCAN_TRIGGER) &&
			   location_msg->type == LOCATION_SCAN_SEARCH_TRIGGER) {
			struct location_config config;
			/* Adjacent in the method list, which makes the Location library
			 * combine them into a single cloud request carrying both.
			 */
			enum location_method methods[] = {
				LOCATION_METHOD_WIFI,
				LOCATION_METHOD_CELLULAR
			};

			LOG_DBG("Radio scan trigger received");

			scan_only_request = true;

			location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);

			IF_ENABLED(CONFIG_APP_SURVEY_CAPTURE, (survey_timeouts_apply(&config,
				CONFIG_APP_SURVEY_CAPTURE_SCAN_TIMEOUT_SECONDS * MSEC_PER_SEC);));

			err = location_request(&config);
			if (err) {
				LOG_WRN("location_request, error: %d", err);
				SEND_FATAL_ERROR();

				return SMF_EVENT_HANDLED;
			}

			IF_ENABLED(CONFIG_APP_SURVEY, (atomic_inc(&triggers_accepted);));

			smf_set_state(SMF_CTX(state_object), &states[STATE_LOCATION_SEARCH_ACTIVE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_location_search_active_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);
}

static enum smf_state_result state_location_search_active_run(void *obj)
{
	struct location_state_object *state_object = obj;

	if (state_object->chan == &location_chan) {
		const struct location_msg *location_msg =
			(const struct location_msg *)state_object->msg_buf;
		int err;

		if (location_msg->type == LOCATION_SEARCH_TRIGGER ||
		    location_msg->type == LOCATION_GNSS_SEARCH_TRIGGER ||
		    (IS_ENABLED(CONFIG_APP_LOCATION_SCAN_TRIGGER) &&
		     location_msg->type == LOCATION_SCAN_SEARCH_TRIGGER)) {
			/* Warning, not debug: a GNSS request can hold the module for
			 * minutes, and a caller whose trigger was dropped would otherwise
			 * have no indication and would read a stale result as fresh.
			 */
			LOG_WRN("Location trigger received while a search is active, ignoring");

			IF_ENABLED(CONFIG_APP_SURVEY, (atomic_inc(&triggers_dropped);));
		} else if (location_msg->type == LOCATION_SEARCH_CANCEL) {
			LOG_DBG("Location search cancel received, cancelling location request");

			err = location_request_cancel();
			if (err) {
				LOG_ERR("Unable to cancel location request: %d", err);
			} else {
				LOG_DBG("Location request cancelled successfully");
			}

		} else if (location_msg->type == LOCATION_SEARCH_DONE) {
			LOG_DBG("Location search done message received, going to inactive state");

			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_LOCATION_SEARCH_INACTIVE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void location_print_data_details(enum location_method method,
					const struct location_data_details *details)
{
	LOG_DBG("Elapsed method time: %d ms", details->elapsed_time_method);

#if defined(CONFIG_LOCATION_METHOD_GNSS)
	if (method == LOCATION_METHOD_GNSS) {
		LOG_DBG("Satellites tracked: %d", details->gnss.satellites_tracked);
		LOG_DBG("Satellites used: %d", details->gnss.satellites_used);
		LOG_DBG("Elapsed GNSS time: %d ms", details->gnss.elapsed_time_gnss);
		LOG_DBG("GNSS execution time: %d ms", details->gnss.pvt_data.execution_time);
	}
#endif

#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
	if (method == LOCATION_METHOD_CELLULAR || method == LOCATION_METHOD_WIFI_CELLULAR) {
		LOG_DBG("Neighbor cells: %d", details->cellular.ncells_count);
		LOG_DBG("GCI cells: %d", details->cellular.gci_cells_count);
	}
#endif

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	if (method == LOCATION_METHOD_WIFI || method == LOCATION_METHOD_WIFI_CELLULAR) {
		LOG_DBG("Wi-Fi APs: %d", details->wifi.ap_count);
	}
#endif
}

/* Take time from PVT data and apply it to system time. */
#if defined(CONFIG_LOCATION_METHOD_GNSS)
static void apply_gnss_time(const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	struct tm gnss_time = {
		.tm_year = pvt_data->datetime.year - 1900,
		.tm_mon = pvt_data->datetime.month - 1,
		.tm_mday = pvt_data->datetime.day,
		.tm_hour = pvt_data->datetime.hour,
		.tm_min = pvt_data->datetime.minute,
		.tm_sec = pvt_data->datetime.seconds,
	};

	date_time_set(&gnss_time);
}
#endif /* CONFIG_LOCATION_METHOD_GNSS */

static void location_event_handler(const struct location_event_data *event_data)
{
	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		LOG_DBG("Got location: lat: %f, lon: %f, acc: %f, method: %s",
			(double) event_data->location.latitude,
			(double) event_data->location.longitude,
			(double) event_data->location.accuracy,
			location_method_str(event_data->method));

#if defined(CONFIG_LOCATION_METHOD_GNSS)
		if (event_data->method == LOCATION_METHOD_GNSS) {
			struct nrf_modem_gnss_pvt_data_frame pvt_data =
				event_data->location.details.gnss.pvt_data;
			if (event_data->location.datetime.valid) {
				/* GNSS is the most accurate time source -  use it. */
				apply_gnss_time(&pvt_data);
			} else {
				/* this should not happen */
				LOG_WRN("Got GNSS location without valid time data");
			}

			/* Send GNSS location data to cloud for reporting */
			gnss_location_send(&event_data->location);
		}
#endif /* CONFIG_LOCATION_METHOD_GNSS */

		message_send(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_STARTED:
		message_send(LOCATION_SEARCH_STARTED);
		break;
	case LOCATION_EVT_TIMEOUT:
		LOG_DBG("Getting location timed out");
		message_send(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_ERROR:
		LOG_WRN("Location request failed:");
		LOG_WRN("Used method: %s (%d)", location_method_str(event_data->method),
								    event_data->method);

		location_print_data_details(event_data->method, &event_data->error.details);

		message_send(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_FALLBACK:
		LOG_DBG("Location request fallback has occurred:");
		LOG_DBG("Failed method: %s (%d)", location_method_str(event_data->method),
								      event_data->method);
		LOG_DBG("New method: %s (%d)", location_method_str(
							event_data->fallback.next_method),
							event_data->fallback.next_method);
		LOG_DBG("Cause: %s",
			(event_data->fallback.cause == LOCATION_EVT_TIMEOUT) ? "timeout" :
			(event_data->fallback.cause == LOCATION_EVT_ERROR) ? "error" :
			"unknown");

		location_print_data_details(event_data->method, &event_data->fallback.details);
		break;
#if defined(CONFIG_LOCATION_METHOD_WIFI) || defined(CONFIG_LOCATION_METHOD_CELLULAR)
	case LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST:
		LOG_DBG("Cloud location request received from location library");

		cloud_request_send(&event_data->cloud_location_request);

		if (IS_ENABLED(CONFIG_APP_LOCATION_SCAN_TRIGGER) && scan_only_request) {
			/* A scan-only request has already produced everything it was asked
			 * for. Hand the library an 'unknown' result so it winds the request
			 * down through its own state machine instead of being cancelled:
			 * location_request_cancel() cannot truly cancel a Wi-Fi scan and can
			 * leave the next request returning -EBUSY (see location.h), which
			 * matters here because scan requests are issued repeatedly.
			 *
			 * Wi-Fi and cellular are combined into a single method for these
			 * requests, so there is no method left to fall back to and the
			 * library finishes with LOCATION_EVT_RESULT_UNKNOWN.
			 */
			location_cloud_location_ext_result_set(LOCATION_EXT_RESULT_UNKNOWN, NULL);
			break;
		}

		/* Cancel the current location request to avoid falling back to the next
		 * location source. Treat the fact that we have found Wi-Fi APs and/or cellular data
		 * as a successful location request, even if we don't know whether the
		 * cloud is able to resolve data to a location or not.
		 */
		message_send(LOCATION_SEARCH_CANCEL);
		break;
#endif /* CONFIG_LOCATION_METHOD_WIFI || CONFIG_LOCATION_METHOD_CELLULAR */
#if defined(CONFIG_NRF_CLOUD_AGNSS)
	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
		LOG_DBG("A-GNSS assistance request received from location library");
		agnss_request_send(&event_data->agnss_request);
		break;
#endif /* CONFIG_NRF_CLOUD_AGNSS */
	case LOCATION_EVT_RESULT_UNKNOWN:
		LOG_DBG("Location result unknown");
		message_send(LOCATION_SEARCH_DONE);
		break;
	case LOCATION_EVT_CANCELLED:
		LOG_DBG("Location request cancelled");
		message_send(LOCATION_SEARCH_DONE);
		break;
	default:
		LOG_DBG("Getting location: Unknown event %d", event_data->id);
		break;
	}
}

static void location_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_LOCATION_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_LOCATION_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	/* Place state object in .data section to ensure it is captured in coredumps
	 * and can be inspected by external tools during state analysis.
	 */
	__attribute__((section(".data"))) static struct location_state_object location_state;

	LOG_DBG("Location module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, location_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	/* Initialize the state machine */
	smf_set_initial(SMF_CTX(&location_state), &states[STATE_WAITING_FOR_CFUN]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("Failed to feed the watchdog: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&location, &location_state.chan,
					 location_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&location_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(location_module_thread_id, CONFIG_APP_LOCATION_THREAD_STACK_SIZE,
		location_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
