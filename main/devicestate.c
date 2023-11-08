/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Russell Bryant <russell@digium.com>
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Device state management
 *
 * \author Mark Spencer <markster@digium.com> 
 * \author Russell Bryant <russell@digium.com>
 *
 *	\arg \ref AstExtState
 */

/*! \page AstExtState Extension and device states in Trismedia
 *
 * (Note that these descriptions of device states and extension
 * states have not been updated to the way things work
 * in Trismedia 1.6.)
 *
 *	Trismedia has an internal system that reports states
 *	for an extension. By using the dialplan priority -1,
 *	also called a \b hint, a connection can be made from an
 *	extension to one or many devices. The state of the extension
 *	now depends on the combined state of the devices.
 *
 *	The device state is basically based on the current calls.
 *	If the devicestate engine can find a call from or to the
 *	device, it's in use.
 *	
 *	Some channel drivers implement a callback function for 
 *	a better level of reporting device states. The SIP channel
 *	has a complicated system for this, which is improved 
 *	by adding call limits to the configuration.
 * 
 *	Functions that want to check the status of an extension
 *	register themself as a \b watcher.
 *	Watchers in this system can subscribe either to all extensions
 *	or just a specific extensions.
 *
 *	For non-device related states, there's an API called
 *	devicestate providers. This is an extendible system for
 *	delivering state information from outside sources or
 *	functions within Trismedia. Currently we have providers
 *	for app_meetme.c - the conference bridge - and call
 *	parking (metermaids).
 *
 *	There are manly three subscribers to extension states 
 *	within Trismedia:
 *	- AMI, the manager interface
 *	- app_queue.c - the Queue dialplan application
 *	- SIP subscriptions, a.k.a. "blinking lamps" or 
 *	  "buddy lists"
 *
 *	The CLI command "show hints" show last known state
 *
 *	\note None of these handle user states, like an IM presence
 *	system. res_jabber.c can subscribe and watch such states
 *	in jabber/xmpp based systems.
 *
 *	\section AstDevStateArch Architecture for devicestates
 *
 *	When a channel driver or trismedia app changes state for 
 *	a watched object, it alerts the core. The core queues
 *	a change. When the change is processed, there's a query
 *	sent to the channel driver/provider if there's a function
 *	to handle that, otherwise a channel walk is issued to find
 *	a channel that involves the object.
 *	
 *	The changes are queued and processed by a separate thread.
 *	This thread calls the watchers subscribing to status 
 *	changes for the object. For manager, this results 
 *	in events. For SIP, NOTIFY requests.
 *
 *	- Device states
 *		\arg \ref devicestate.c 
 *		\arg \ref devicestate.h 
 *
 *	\section AstExtStateArch Architecture for extension states
 *	
 *	Hints are connected to extension. If an extension changes state
 *	it checks the hint devices. If there is a hint, the callbacks into
 *	device states are checked. The aggregated state is set for the hint
 *	and reported back.
 *
 *	- Extension states
 *		\arg \ref AstENUM tris_extension_states
 *		\arg \ref pbx.c 
 *		\arg \ref pbx.h 
 *	- Structures
 *		- \ref tris_state_cb struct.  Callbacks for watchers
 *		- Callback tris_state_cb_type
 *		- \ref tris_hint struct.
 * 	- Functions
 *		- tris_extension_state_add()
 *		- tris_extension_state_del()
 *		- tris_get_hint()
 *	
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 205413 $")

#include "trismedia/_private.h"
#include "trismedia/channel.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/linkedlists.h"
#include "trismedia/devicestate.h"
#include "trismedia/pbx.h"
#include "trismedia/app.h"
#include "trismedia/event.h"

/*! \brief Device state strings for printing */
static const char *devstatestring[][2] = {
	{ /* 0 TRIS_DEVICE_UNKNOWN */     "Unknown",     "UNKNOWN"     }, /*!< Valid, but unknown state */
	{ /* 1 TRIS_DEVICE_NOT_INUSE */   "Not in use",  "NOT_INUSE"   }, /*!< Not used */
	{ /* 2 TRIS_DEVICE IN USE */      "In use",      "INUSE"       }, /*!< In use */
	{ /* 3 TRIS_DEVICE_BUSY */        "Busy",        "BUSY"        }, /*!< Busy */
	{ /* 4 TRIS_DEVICE_INVALID */     "Invalid",     "INVALID"     }, /*!< Invalid - not known to Trismedia */
	{ /* 5 TRIS_DEVICE_UNAVAILABLE */ "Unavailable", "UNAVAILABLE" }, /*!< Unavailable (not registered) */
	{ /* 6 TRIS_DEVICE_RINGING */     "Ringing",     "RINGING"     }, /*!< Ring, ring, ring */
	{ /* 7 TRIS_DEVICE_RINGINUSE */   "Ring+Inuse",  "RINGINUSE"   }, /*!< Ring and in use */
	{ /* 8 TRIS_DEVICE_ONHOLD */      "On Hold",      "ONHOLD"      }, /*!< On Hold */
};

/*!\brief Mapping for channel states to device states */
static const struct chan2dev {
	enum tris_channel_state chan;
	enum tris_device_state dev;
} chan2dev[] = {
	{ TRIS_STATE_DOWN,            TRIS_DEVICE_NOT_INUSE },
	{ TRIS_STATE_RESERVED,        TRIS_DEVICE_INUSE },
	{ TRIS_STATE_OFFHOOK,         TRIS_DEVICE_INUSE },
	{ TRIS_STATE_DIALING,         TRIS_DEVICE_INUSE },
	{ TRIS_STATE_RING,            TRIS_DEVICE_INUSE },
	{ TRIS_STATE_RINGING,         TRIS_DEVICE_RINGING },
	{ TRIS_STATE_UP,              TRIS_DEVICE_INUSE },
	{ TRIS_STATE_BUSY,            TRIS_DEVICE_BUSY },
	{ TRIS_STATE_DIALING_OFFHOOK, TRIS_DEVICE_INUSE },
	{ TRIS_STATE_PRERING,         TRIS_DEVICE_RINGING },
	{ -100,                      -100 },
};

/*! \brief  A device state provider (not a channel) */
struct devstate_prov {
	char label[40];
	tris_devstate_prov_cb_type callback;
	TRIS_RWLIST_ENTRY(devstate_prov) list;
};

/*! \brief A list of providers */
static TRIS_RWLIST_HEAD_STATIC(devstate_provs, devstate_prov);

struct state_change {
	TRIS_LIST_ENTRY(state_change) list;
	char device[1];
};

/*! \brief The state change queue. State changes are queued
	for processing by a separate thread */
static TRIS_LIST_HEAD_STATIC(state_changes, state_change);

/*! \brief The device state change notification thread */
static pthread_t change_thread = TRIS_PTHREADT_NULL;

/*! \brief Flag for the queue */
static tris_cond_t change_pending;

struct devstate_change {
	TRIS_LIST_ENTRY(devstate_change) entry;
	uint32_t state;
	struct tris_eid eid;
	char device[1];
};

struct {
	pthread_t thread;
	struct tris_event_sub *event_sub;
	tris_cond_t cond;
	tris_mutex_t lock;
	TRIS_LIST_HEAD_NOLOCK(, devstate_change) devstate_change_q;
	unsigned int enabled:1;
} devstate_collector = {
	.thread = TRIS_PTHREADT_NULL,
	.enabled = 0,
};

/* Forward declarations */
static int getproviderstate(const char *provider, const char *address);

/*! \brief Find devicestate as text message for output */
const char *tris_devstate2str(enum tris_device_state devstate) 
{
	return devstatestring[devstate][0];
}

/* Deprecated interface (not prefixed with tris_) */
const char *devstate2str(enum tris_device_state devstate) 
{
	return devstatestring[devstate][0];
}

enum tris_device_state tris_state_chan2dev(enum tris_channel_state chanstate)
{
	int i;
	chanstate &= 0xFFFF;
	for (i = 0; chan2dev[i].chan != -100; i++) {
		if (chan2dev[i].chan == chanstate) {
			return chan2dev[i].dev;
		}
	}
	return TRIS_DEVICE_UNKNOWN;
}

/* Parseable */
const char *tris_devstate_str(enum tris_device_state state)
{
	return devstatestring[state][1];
}

enum tris_device_state tris_devstate_val(const char *val)
{
	if (!strcasecmp(val, "NOT_INUSE"))
		return TRIS_DEVICE_NOT_INUSE;
	else if (!strcasecmp(val, "INUSE"))
		return TRIS_DEVICE_INUSE;
	else if (!strcasecmp(val, "BUSY"))
		return TRIS_DEVICE_BUSY;
	else if (!strcasecmp(val, "INVALID"))
		return TRIS_DEVICE_INVALID;
	else if (!strcasecmp(val, "UNAVAILABLE"))
		return TRIS_DEVICE_UNAVAILABLE;
	else if (!strcasecmp(val, "RINGING"))
		return TRIS_DEVICE_RINGING;
	else if (!strcasecmp(val, "RINGINUSE"))
		return TRIS_DEVICE_RINGINUSE;
	else if (!strcasecmp(val, "ONHOLD"))
		return TRIS_DEVICE_ONHOLD;

	return TRIS_DEVICE_UNKNOWN;
}

/*! \brief Find out if device is active in a call or not 
	\note find channels with the device's name in it
	This function is only used for channels that does not implement 
	devicestate natively
*/
enum tris_device_state tris_parse_device_state(const char *device)
{
	struct tris_channel *chan;
	char match[TRIS_CHANNEL_NAME];
	enum tris_device_state res;

	tris_copy_string(match, device, sizeof(match)-1);
	strcat(match, "-");
	chan = tris_get_channel_by_name_prefix_locked(match, strlen(match));

	if (!chan)
		return TRIS_DEVICE_UNKNOWN;

	if (chan->_state == TRIS_STATE_RINGING)
		res = TRIS_DEVICE_RINGING;
	else
		res = TRIS_DEVICE_INUSE;
	
	tris_channel_unlock(chan);

	return res;
}

static enum tris_device_state devstate_cached(const char *device)
{
	enum tris_device_state res = TRIS_DEVICE_UNKNOWN;
	struct tris_event *event;

	event = tris_event_get_cached(TRIS_EVENT_DEVICE_STATE,
		TRIS_EVENT_IE_DEVICE, TRIS_EVENT_IE_PLTYPE_STR, device,
		TRIS_EVENT_IE_END);

	if (!event)
		return res;

	res = tris_event_get_ie_uint(event, TRIS_EVENT_IE_STATE);

	tris_event_destroy(event);

	return res;
}

/*! \brief Check device state through channel specific function or generic function */
static enum tris_device_state _tris_device_state(const char *device, int check_cache)
{
	char *buf;
	char *number;
	const struct tris_channel_tech *chan_tech;
	enum tris_device_state res;
	/*! \brief Channel driver that provides device state */
	char *tech;
	/*! \brief Another provider of device state */
	char *provider = NULL;

	/* If the last known state is cached, just return that */
	if (check_cache) {
		res = devstate_cached(device);
		if (res != TRIS_DEVICE_UNKNOWN) {
			return res;
		}
	}

	buf = tris_strdupa(device);
	tech = strsep(&buf, "/");
	if (!(number = buf)) {
		if (!(provider = strsep(&tech, ":")))
			return TRIS_DEVICE_INVALID;
		/* We have a provider */
		number = tech;
		tech = NULL;
	}

	if (provider)  {
		tris_debug(3, "Checking if I can find provider for \"%s\" - number: %s\n", provider, number);
		return getproviderstate(provider, number);
	}

	//tris_debug(4, "No provider found, checking channel drivers for %s - %s\n", tech, number);

	if (!(chan_tech = tris_get_channel_tech(tech)))
		return TRIS_DEVICE_INVALID;

	if (!(chan_tech->devicestate)) /* Does the channel driver support device state notification? */
		return tris_parse_device_state(device); /* No, try the generic function */

	res = chan_tech->devicestate(number);

	if (res != TRIS_DEVICE_UNKNOWN)
		return res;

	res = tris_parse_device_state(device);

	return res;
}

enum tris_device_state tris_device_state(const char *device)
{
	/* This function is called from elsewhere in the code to find out the
	 * current state of a device.  Check the cache, first. */

	return _tris_device_state(device, 1);
}

/*! \brief Add device state provider */
int tris_devstate_prov_add(const char *label, tris_devstate_prov_cb_type callback)
{
	struct devstate_prov *devprov;

	if (!callback || !(devprov = tris_calloc(1, sizeof(*devprov))))
		return -1;

	devprov->callback = callback;
	tris_copy_string(devprov->label, label, sizeof(devprov->label));

	TRIS_RWLIST_WRLOCK(&devstate_provs);
	TRIS_RWLIST_INSERT_HEAD(&devstate_provs, devprov, list);
	TRIS_RWLIST_UNLOCK(&devstate_provs);

	return 0;
}

/*! \brief Remove device state provider */
int tris_devstate_prov_del(const char *label)
{
	struct devstate_prov *devcb;
	int res = -1;

	TRIS_RWLIST_WRLOCK(&devstate_provs);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&devstate_provs, devcb, list) {
		if (!strcasecmp(devcb->label, label)) {
			TRIS_RWLIST_REMOVE_CURRENT(list);
			tris_free(devcb);
			res = 0;
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&devstate_provs);

	return res;
}

/*! \brief Get provider device state */
static int getproviderstate(const char *provider, const char *address)
{
	struct devstate_prov *devprov;
	int res = TRIS_DEVICE_INVALID;

	TRIS_RWLIST_RDLOCK(&devstate_provs);
	TRIS_RWLIST_TRAVERSE(&devstate_provs, devprov, list) {
		tris_debug(5, "Checking provider %s with %s\n", devprov->label, provider);

		if (!strcasecmp(devprov->label, provider)) {
			res = devprov->callback(address);
			break;
		}
	}
	TRIS_RWLIST_UNLOCK(&devstate_provs);

	return res;
}

static void devstate_event(const char *device, enum tris_device_state state)
{
	struct tris_event *event;
	enum tris_event_type event_type;

	if (devstate_collector.enabled) {
		/* Distributed device state is enabled, so this state change is a change
		 * for a single server, not the real state. */
		event_type = TRIS_EVENT_DEVICE_STATE_CHANGE;
	} else {
		event_type = TRIS_EVENT_DEVICE_STATE;
	}

	tris_debug(3, "device '%s' state '%d'\n", device, state);

	if (!(event = tris_event_new(event_type,
			TRIS_EVENT_IE_DEVICE, TRIS_EVENT_IE_PLTYPE_STR, device,
			TRIS_EVENT_IE_STATE, TRIS_EVENT_IE_PLTYPE_UINT, state,
			TRIS_EVENT_IE_END))) {
		return;
	}

	tris_event_queue_and_cache(event);
}

/*! Called by the state change thread to find out what the state is, and then
 *  to queue up the state change event */
static void do_state_change(const char *device)
{
	enum tris_device_state state;

	state = _tris_device_state(device, 0);

	tris_debug(3, "Changing state for %s - state %d (%s)\n", device, state, tris_devstate2str(state));

	devstate_event(device, state);
}

int tris_devstate_changed_literal(enum tris_device_state state, const char *device)
{
	struct state_change *change;

	/* 
	 * If we know the state change (how nice of the caller of this function!)
	 * then we can just generate a device state event. 
	 *
	 * Otherwise, we do the following:
	 *   - Queue an event up to another thread that the state has changed
	 *   - In the processing thread, it calls the callback provided by the
	 *     device state provider (which may or may not be a channel driver)
	 *     to determine the state.
	 *   - If the device state provider does not know the state, or this is
	 *     for a channel and the channel driver does not implement a device
	 *     state callback, then we will look through the channel list to
	 *     see if we can determine a state based on active calls.
	 *   - Once a state has been determined, a device state event is generated.
	 */

	if (state != TRIS_DEVICE_UNKNOWN) {
		devstate_event(device, state);
	} else if (change_thread == TRIS_PTHREADT_NULL || !(change = tris_calloc(1, sizeof(*change) + strlen(device)))) {
		/* we could not allocate a change struct, or */
		/* there is no background thread, so process the change now */
		do_state_change(device);
	} else {
		/* queue the change */
		strcpy(change->device, device);
		TRIS_LIST_LOCK(&state_changes);
		TRIS_LIST_INSERT_TAIL(&state_changes, change, list);
		tris_cond_signal(&change_pending);
		TRIS_LIST_UNLOCK(&state_changes);
	}

	return 1;
}

int tris_device_state_changed_literal(const char *dev)
{
	return tris_devstate_changed_literal(TRIS_DEVICE_UNKNOWN, dev);
}

int tris_devstate_changed(enum tris_device_state state, const char *fmt, ...) 
{
	char buf[TRIS_MAX_EXTENSION];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return tris_devstate_changed_literal(state, buf);
}

int tris_device_state_changed(const char *fmt, ...) 
{
	char buf[TRIS_MAX_EXTENSION];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return tris_devstate_changed_literal(TRIS_DEVICE_UNKNOWN, buf);
}

/*! \brief Go through the dev state change queue and update changes in the dev state thread */
static void *do_devstate_changes(void *data)
{
	struct state_change *next, *current;

	for (;;) {
		/* This basically pops off any state change entries, resets the list back to NULL, unlocks, and processes each state change */
		TRIS_LIST_LOCK(&state_changes);
		if (TRIS_LIST_EMPTY(&state_changes))
			tris_cond_wait(&change_pending, &state_changes.lock);
		next = TRIS_LIST_FIRST(&state_changes);
		TRIS_LIST_HEAD_INIT_NOLOCK(&state_changes);
		TRIS_LIST_UNLOCK(&state_changes);

		/* Process each state change */
		while ((current = next)) {
			next = TRIS_LIST_NEXT(current, list);
			do_state_change(current->device);
			tris_free(current);
		}
	}

	return NULL;
}

static void destroy_devstate_change(struct devstate_change *sc)
{
	tris_free(sc);
}

#define MAX_SERVERS 64
struct change_collection {
	struct devstate_change states[MAX_SERVERS];
	size_t num_states;
};

static void devstate_cache_cb(const struct tris_event *event, void *data)
{
	struct change_collection *collection = data;
	int i;
	const struct tris_eid *eid;

	if (collection->num_states == ARRAY_LEN(collection->states)) {
		tris_log(LOG_ERROR, "More per-server state values than we have room for (MAX_SERVERS is %d)\n",
			MAX_SERVERS);
		return;
	}

	if (!(eid = tris_event_get_ie_raw(event, TRIS_EVENT_IE_EID))) {
		tris_log(LOG_ERROR, "Device state change event with no EID\n");
		return;
	}

	i = collection->num_states;

	collection->states[i].state = tris_event_get_ie_uint(event, TRIS_EVENT_IE_STATE);
	collection->states[i].eid = *eid;

	collection->num_states++;
}

static void process_collection(const char *device, struct change_collection *collection)
{
	int i;
	struct tris_devstate_aggregate agg;
	enum tris_device_state state;
	struct tris_event *event;

	tris_devstate_aggregate_init(&agg);

	for (i = 0; i < collection->num_states; i++) {
		tris_debug(1, "Adding per-server state of '%s' for '%s'\n", 
			tris_devstate2str(collection->states[i].state), device);
		tris_devstate_aggregate_add(&agg, collection->states[i].state);
	}

	state = tris_devstate_aggregate_result(&agg);

	tris_debug(1, "Aggregate devstate result is %d\n", state);

	event = tris_event_get_cached(TRIS_EVENT_DEVICE_STATE,
		TRIS_EVENT_IE_DEVICE, TRIS_EVENT_IE_PLTYPE_STR, device,
		TRIS_EVENT_IE_END);
	
	if (event) {
		enum tris_device_state old_state;

		old_state = tris_event_get_ie_uint(event, TRIS_EVENT_IE_STATE);
		
		tris_event_destroy(event);

		if (state == old_state) {
			/* No change since last reported device state */
			tris_debug(1, "Aggregate state for device '%s' has not changed from '%s'\n",
				device, tris_devstate2str(state));
			return;
		}
	}

	tris_debug(1, "Aggregate state for device '%s' has changed to '%s'\n",
		device, tris_devstate2str(state));

	event = tris_event_new(TRIS_EVENT_DEVICE_STATE,
		TRIS_EVENT_IE_DEVICE, TRIS_EVENT_IE_PLTYPE_STR, device,
		TRIS_EVENT_IE_STATE, TRIS_EVENT_IE_PLTYPE_UINT, state,
		TRIS_EVENT_IE_END);

	if (!event) {
		return;
	}

	tris_event_queue_and_cache(event);
}

static void handle_devstate_change(struct devstate_change *sc)
{
	struct tris_event_sub *tmp_sub;
	struct change_collection collection = {
		.num_states = 0,
	};

	tris_debug(1, "Processing device state change for '%s'\n", sc->device);

	if (!(tmp_sub = tris_event_subscribe_new(TRIS_EVENT_DEVICE_STATE_CHANGE, devstate_cache_cb, &collection))) {
		tris_log(LOG_ERROR, "Failed to create subscription\n");
		return;
	}

	if (tris_event_sub_append_ie_str(tmp_sub, TRIS_EVENT_IE_DEVICE, sc->device)) {
		tris_log(LOG_ERROR, "Failed to append device IE\n");
		tris_event_sub_destroy(tmp_sub);
		return;
	}

	/* Populate the collection of device states from the cache */
	tris_event_dump_cache(tmp_sub);

	process_collection(sc->device, &collection);

	tris_event_sub_destroy(tmp_sub);
}

static void *run_devstate_collector(void *data)
{
	for (;;) {
		struct devstate_change *sc;

		tris_mutex_lock(&devstate_collector.lock);
		while (!(sc = TRIS_LIST_REMOVE_HEAD(&devstate_collector.devstate_change_q, entry)))
			tris_cond_wait(&devstate_collector.cond, &devstate_collector.lock);
		tris_mutex_unlock(&devstate_collector.lock);

		handle_devstate_change(sc);

		destroy_devstate_change(sc);
	}

	return NULL;
}

static void devstate_change_collector_cb(const struct tris_event *event, void *data)
{
	struct devstate_change *sc;
	const char *device;
	const struct tris_eid *eid;
	uint32_t state;

	device = tris_event_get_ie_str(event, TRIS_EVENT_IE_DEVICE);
	eid = tris_event_get_ie_raw(event, TRIS_EVENT_IE_EID);
	state = tris_event_get_ie_uint(event, TRIS_EVENT_IE_STATE);

	if (tris_strlen_zero(device) || !eid) {
		tris_log(LOG_ERROR, "Invalid device state change event received\n");
		return;
	}

	if (!(sc = tris_calloc(1, sizeof(*sc) + strlen(device))))
		return;

	strcpy(sc->device, device);
	sc->eid = *eid;
	sc->state = state;

	tris_mutex_lock(&devstate_collector.lock);
	TRIS_LIST_INSERT_TAIL(&devstate_collector.devstate_change_q, sc, entry);
	tris_cond_signal(&devstate_collector.cond);
	tris_mutex_unlock(&devstate_collector.lock);
}

/*! \brief Initialize the device state engine in separate thread */
int tris_device_state_engine_init(void)
{
	tris_cond_init(&change_pending, NULL);
	if (tris_pthread_create_background(&change_thread, NULL, do_devstate_changes, NULL) < 0) {
		tris_log(LOG_ERROR, "Unable to start device state change thread.\n");
		return -1;
	}

	return 0;
}

void tris_devstate_aggregate_init(struct tris_devstate_aggregate *agg)
{
	memset(agg, 0, sizeof(*agg));

	agg->all_unknown = 1;
	agg->all_unavail = 1;
	agg->all_busy = 1;
	agg->all_free = 1;
}

void tris_devstate_aggregate_add(struct tris_devstate_aggregate *agg, enum tris_device_state state)
{
	switch (state) {
	case TRIS_DEVICE_NOT_INUSE:
		agg->all_unknown = 0;
		agg->all_unavail = 0;
		agg->all_busy = 0;
		break;
	case TRIS_DEVICE_INUSE:
		agg->in_use = 1;
		agg->all_unavail = 0;
		agg->all_free = 0;
		agg->all_unknown = 0;
		break;
	case TRIS_DEVICE_RINGING:
		agg->ring = 1;
		agg->all_unavail = 0;
		agg->all_free = 0;
		agg->all_unknown = 0;
		break;
	case TRIS_DEVICE_RINGINUSE:
		agg->in_use = 1;
		agg->ring = 1;
		agg->all_unavail = 0;
		agg->all_free = 0;
		agg->all_unknown = 0;
		break;
	case TRIS_DEVICE_ONHOLD:
		agg->all_unknown = 0;
		agg->all_unavail = 0;
		agg->all_free = 0;
		agg->on_hold = 1;
		break;
	case TRIS_DEVICE_BUSY:
		agg->all_unknown = 0;
		agg->all_unavail = 0;
		agg->all_free = 0;
		agg->busy = 1;
		agg->in_use = 1;
		break;
	case TRIS_DEVICE_UNAVAILABLE:
		agg->all_unknown = 0;
	case TRIS_DEVICE_INVALID:
		agg->all_busy = 0;
		agg->all_free = 0;
		break;
	case TRIS_DEVICE_UNKNOWN:
		agg->all_busy = 0;
		agg->all_free = 0;
		break;
	case TRIS_DEVICE_TOTAL: /* not a device state, included for completeness. */
		break;
	}
}


enum tris_device_state tris_devstate_aggregate_result(struct tris_devstate_aggregate *agg)
{
	if (agg->all_free)
		return TRIS_DEVICE_NOT_INUSE;
	if ((agg->in_use || agg->on_hold) && agg->ring)
		return TRIS_DEVICE_RINGINUSE;
	if (agg->ring)
		return TRIS_DEVICE_RINGING;
	if (agg->busy)
		return TRIS_DEVICE_BUSY;
	if (agg->in_use)
		return TRIS_DEVICE_INUSE;
	if (agg->on_hold)
		return TRIS_DEVICE_ONHOLD;
	if (agg->all_busy)
		return TRIS_DEVICE_BUSY;
	if (agg->all_unknown)
		return TRIS_DEVICE_UNKNOWN;
	if (agg->all_unavail)
		return TRIS_DEVICE_UNAVAILABLE;

	return TRIS_DEVICE_NOT_INUSE;
}

int tris_enable_distributed_devstate(void)
{
	if (devstate_collector.enabled) {
		return 0;
	}

	devstate_collector.event_sub = tris_event_subscribe(TRIS_EVENT_DEVICE_STATE_CHANGE,
		devstate_change_collector_cb, NULL, TRIS_EVENT_IE_END);

	if (!devstate_collector.event_sub) {
		tris_log(LOG_ERROR, "Failed to create subscription for the device state change collector\n");
		return -1;
	}

	tris_mutex_init(&devstate_collector.lock);
	tris_cond_init(&devstate_collector.cond, NULL);
	if (tris_pthread_create_background(&devstate_collector.thread, NULL, run_devstate_collector, NULL) < 0) {
		tris_log(LOG_ERROR, "Unable to start device state collector thread.\n");
		return -1;
	}

	devstate_collector.enabled = 1;

	return 0;
}
