/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2008, Digium, Inc.
 *
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
 * \brief Internal generic event system
 *
 * \author Russell Bryant <russell@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 245626 $")

#include "trismedia/_private.h"

#include "trismedia/event.h"
#include "trismedia/linkedlists.h"
#include "trismedia/dlinkedlists.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"
#include "trismedia/unaligned.h"
#include "trismedia/utils.h"
#include "trismedia/taskprocessor.h"
#include "trismedia/astobj2.h"

struct tris_taskprocessor *event_dispatcher;

/*!
 * \brief An event information element
 *
 * \note The format of this structure is important.  Since these events may
 *       be sent directly over a network, changing this structure will break
 *       compatibility with older versions.  However, at this point, this code
 *       has not made it into a release, so it is still fair game for change.
 */
struct tris_event_ie {
	enum tris_event_ie_type ie_type:16;
	/*! Total length of the IE payload */
	uint16_t ie_payload_len;
	unsigned char ie_payload[0];
} __attribute__((packed));

/*!
 * \brief The payload for a string information element
 */
struct tris_event_ie_str_payload {
	/*! \brief A hash calculated with tris_str_hash(), to speed up comparisons */
	uint32_t hash;
	/*! \brief The actual string, null terminated */
	char str[1];
} __attribute__((packed));

/*!
 * \brief An event
 *
 * An tris_event consists of an event header (this structure), and zero or
 * more information elements defined by tris_event_ie.
 *
 * \note The format of this structure is important.  Since these events may
 *       be sent directly over a network, changing this structure will break
 *       compatibility with older versions.  However, at this point, this code
 *       has not made it into a release, so it is still fair game for change.
 */
struct tris_event {
	/*! Event type */
	enum tris_event_type type:16;
	/*! Total length of the event */
	uint16_t event_len:16;
	/*! The data payload of the event, made up of information elements */
	unsigned char payload[0];
} __attribute__((packed));


/*!
 * \brief A holder for an event
 *
 * \details This struct used to have more of a purpose than it does now.
 * It is used to hold events in the event cache.  It can be completely removed
 * if one of these two things is done:
 *  - tris_event gets changed such that it never has to be realloc()d
 *  - astobj2 is updated so that you can realloc() an astobj2 object
 */
struct tris_event_ref {
	struct tris_event *event;
};

struct tris_event_ie_val {
	TRIS_LIST_ENTRY(tris_event_ie_val) entry;
	enum tris_event_ie_type ie_type;
	enum tris_event_ie_pltype ie_pltype;
	union {
		uint32_t uint;
		struct {
			uint32_t hash;
			const char *str;
		};
		void *raw;
	} payload;
	size_t raw_datalen;
};

/*! \brief Event subscription */
struct tris_event_sub {
	enum tris_event_type type;
	tris_event_cb_t cb;
	void *userdata;
	uint32_t uniqueid;
	TRIS_LIST_HEAD_NOLOCK(, tris_event_ie_val) ie_vals;
	TRIS_RWDLLIST_ENTRY(tris_event_sub) entry;
};

static uint32_t sub_uniqueid;

/*! \brief Event subscriptions
 * The event subscribers are indexed by which event they are subscribed to */
static TRIS_RWDLLIST_HEAD(tris_event_sub_list, tris_event_sub) tris_event_subs[TRIS_EVENT_TOTAL];

static int tris_event_cmp(void *obj, void *arg, int flags);
static int tris_event_hash_mwi(const void *obj, const int flags);
static int tris_event_hash_devstate(const void *obj, const int flags);
static int tris_event_hash_devstate_change(const void *obj, const int flags);

#ifdef LOW_MEMORY
#define NUM_CACHE_BUCKETS 17
#else
#define NUM_CACHE_BUCKETS 563
#endif

#define MAX_CACHE_ARGS 8

/*!
 * \brief Event types that are kept in the cache.
 */
static struct {
	/*! 
	 * \brief Container of cached events
	 *
	 * \details This gets allocated in tris_event_init() when Trismedia starts
	 * for the event types declared as using the cache.
	 */
	struct ao2_container *container;
	/*! \brief Event type specific hash function */
	ao2_hash_fn *hash_fn;
	/*!
	 * \brief Information Elements used for caching
	 *
	 * \details This array is the set of information elements that will be unique
	 * among all events in the cache for this event type.  When a new event gets
	 * cached, a previous event with the same values for these information elements
	 * will be replaced.
	 */
	enum tris_event_ie_type cache_args[MAX_CACHE_ARGS];
} tris_event_cache[TRIS_EVENT_TOTAL] = {
	[TRIS_EVENT_MWI] = {
		.hash_fn = tris_event_hash_mwi,
		.cache_args = { TRIS_EVENT_IE_MAILBOX, TRIS_EVENT_IE_CONTEXT },
	},
	[TRIS_EVENT_DEVICE_STATE] = {
		.hash_fn = tris_event_hash_devstate,
		.cache_args = { TRIS_EVENT_IE_DEVICE, },
	},
	[TRIS_EVENT_DEVICE_STATE_CHANGE] = {
		.hash_fn = tris_event_hash_devstate_change,
		.cache_args = { TRIS_EVENT_IE_DEVICE, TRIS_EVENT_IE_EID, },
	},
};

/*!
 * The index of each entry _must_ match the event type number!
 */
static struct event_name {
	enum tris_event_type type;
	const char *name;
} event_names[] = {
	{ 0, "" },
	{ TRIS_EVENT_CUSTOM,              "Custom" },
	{ TRIS_EVENT_MWI,                 "MWI" },
	{ TRIS_EVENT_SUB,                 "Subscription" },
	{ TRIS_EVENT_UNSUB,               "Unsubscription" },
	{ TRIS_EVENT_DEVICE_STATE,        "DeviceState" },
	{ TRIS_EVENT_DEVICE_STATE_CHANGE, "DeviceStateChange" },
};

/*!
 * The index of each entry _must_ match the event ie number!
 */
static struct ie_map {
	enum tris_event_ie_type ie_type;
	enum tris_event_ie_pltype ie_pltype;
	const char *name;
} ie_maps[] = {
	{ 0, 0, "" },
	{ TRIS_EVENT_IE_NEWMSGS,   TRIS_EVENT_IE_PLTYPE_UINT, "NewMessages" },
	{ TRIS_EVENT_IE_OLDMSGS,   TRIS_EVENT_IE_PLTYPE_UINT, "OldMessages" },
	{ TRIS_EVENT_IE_MAILBOX,   TRIS_EVENT_IE_PLTYPE_STR,  "Mailbox" },
	{ TRIS_EVENT_IE_UNIQUEID,  TRIS_EVENT_IE_PLTYPE_UINT, "UniqueID" },
	{ TRIS_EVENT_IE_EVENTTYPE, TRIS_EVENT_IE_PLTYPE_UINT, "EventType" },
	{ TRIS_EVENT_IE_EXISTS,    TRIS_EVENT_IE_PLTYPE_UINT, "Exists" },
	{ TRIS_EVENT_IE_DEVICE,    TRIS_EVENT_IE_PLTYPE_STR,  "Device" },
	{ TRIS_EVENT_IE_STATE,     TRIS_EVENT_IE_PLTYPE_UINT, "State" },
	{ TRIS_EVENT_IE_CONTEXT,   TRIS_EVENT_IE_PLTYPE_STR,  "Context" },
	{ TRIS_EVENT_IE_EID,       TRIS_EVENT_IE_PLTYPE_RAW,  "EntityID" },
};

const char *tris_event_get_type_name(const struct tris_event *event)
{
	enum tris_event_type type;

	type = tris_event_get_type(event);

	if (type >= TRIS_EVENT_TOTAL || type < 0) {
		tris_log(LOG_ERROR, "Invalid event type - '%d'\n", type);
		return "";
	}

	return event_names[type].name;
}

int tris_event_str_to_event_type(const char *str, enum tris_event_type *event_type)
{
	int i;

	for (i = 0; i < ARRAY_LEN(event_names); i++) {
		if (strcasecmp(event_names[i].name, str))
			continue;

		*event_type = event_names[i].type;
		return 0;
	}

	return -1;
}

const char *tris_event_get_ie_type_name(enum tris_event_ie_type ie_type)
{
	if (ie_type <= 0 || ie_type > TRIS_EVENT_IE_MAX) {
		tris_log(LOG_ERROR, "Invalid IE type - '%d'\n", ie_type);
		return "";
	}

	if (ie_maps[ie_type].ie_type != ie_type) {
		tris_log(LOG_ERROR, "The ie type passed in does not match the ie type defined in the ie table.\n");
		return "";
	}

	return ie_maps[ie_type].name;
}

enum tris_event_ie_pltype tris_event_get_ie_pltype(enum tris_event_ie_type ie_type)
{
	if (ie_type <= 0 || ie_type > TRIS_EVENT_IE_MAX) {
		tris_log(LOG_ERROR, "Invalid IE type - '%d'\n", ie_type);
		return TRIS_EVENT_IE_PLTYPE_UNKNOWN;
	}

	if (ie_maps[ie_type].ie_type != ie_type) {
		tris_log(LOG_ERROR, "The ie type passed in does not match the ie type defined in the ie table.\n");
		return TRIS_EVENT_IE_PLTYPE_UNKNOWN;
	}

	return ie_maps[ie_type].ie_pltype;
}

int tris_event_str_to_ie_type(const char *str, enum tris_event_ie_type *ie_type)
{
	int i;

	for (i = 0; i < ARRAY_LEN(ie_maps); i++) {
		if (strcasecmp(ie_maps[i].name, str))
			continue;

		*ie_type = ie_maps[i].ie_type;
		return 0;
	}

	return -1;
}

size_t tris_event_get_size(const struct tris_event *event)
{
	size_t res;

	res = ntohs(event->event_len);

	return res;
}

static void tris_event_ie_val_destroy(struct tris_event_ie_val *ie_val)
{
	switch (ie_val->ie_pltype) {
	case TRIS_EVENT_IE_PLTYPE_STR:
		tris_free((char *) ie_val->payload.str);
		break;
	case TRIS_EVENT_IE_PLTYPE_RAW:
		tris_free(ie_val->payload.raw);
		break;
	case TRIS_EVENT_IE_PLTYPE_UINT:
	case TRIS_EVENT_IE_PLTYPE_EXISTS:
	case TRIS_EVENT_IE_PLTYPE_UNKNOWN:
		break;
	}

	tris_free(ie_val);
}

enum tris_event_subscriber_res tris_event_check_subscriber(enum tris_event_type type, ...)
{
	va_list ap;
	enum tris_event_ie_type ie_type;
	enum tris_event_subscriber_res res = TRIS_EVENT_SUB_NONE;
	struct tris_event_ie_val *ie_val, *sub_ie_val;
	struct tris_event_sub *sub;
	TRIS_LIST_HEAD_NOLOCK_STATIC(ie_vals, tris_event_ie_val);

	if (type >= TRIS_EVENT_TOTAL) {
		tris_log(LOG_ERROR, "%u is an invalid type!\n", type);
		return res;
	}

	va_start(ap, type);
	for (ie_type = va_arg(ap, enum tris_event_ie_type);
		ie_type != TRIS_EVENT_IE_END;
		ie_type = va_arg(ap, enum tris_event_ie_type))
	{
		struct tris_event_ie_val *ie_value = alloca(sizeof(*ie_value));
		memset(ie_value, 0, sizeof(*ie_value));
		ie_value->ie_type = ie_type;
		ie_value->ie_pltype = va_arg(ap, enum tris_event_ie_pltype);
		if (ie_value->ie_pltype == TRIS_EVENT_IE_PLTYPE_UINT)
			ie_value->payload.uint = va_arg(ap, uint32_t);
		else if (ie_value->ie_pltype == TRIS_EVENT_IE_PLTYPE_STR)
			ie_value->payload.str = tris_strdupa(va_arg(ap, const char *));
		else if (ie_value->ie_pltype == TRIS_EVENT_IE_PLTYPE_RAW) {
			void *data = va_arg(ap, void *);
			size_t datalen = va_arg(ap, size_t);
			ie_value->payload.raw = alloca(datalen);
			memcpy(ie_value->payload.raw, data, datalen);
			ie_value->raw_datalen = datalen;
		}
		TRIS_LIST_INSERT_TAIL(&ie_vals, ie_value, entry);
	}
	va_end(ap);

	TRIS_RWDLLIST_RDLOCK(&tris_event_subs[type]);
	TRIS_RWDLLIST_TRAVERSE(&tris_event_subs[type], sub, entry) {
		TRIS_LIST_TRAVERSE(&ie_vals, ie_val, entry) {
			TRIS_LIST_TRAVERSE(&sub->ie_vals, sub_ie_val, entry) {
				if (sub_ie_val->ie_type == ie_val->ie_type)
					break;
			}
			if (!sub_ie_val) {
				if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_EXISTS)
					break;
				continue;
			}
			/* The subscriber doesn't actually care what the value is */
			if (sub_ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_EXISTS)
				continue;
			if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_UINT &&
				ie_val->payload.uint != sub_ie_val->payload.uint)
				break;
			if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_STR &&
				strcmp(ie_val->payload.str, sub_ie_val->payload.str))
				break;
			if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_RAW &&
				memcmp(ie_val->payload.raw, sub_ie_val->payload.raw, ie_val->raw_datalen))
				break;
		}
		if (!ie_val)
			break;
	}
	TRIS_RWDLLIST_UNLOCK(&tris_event_subs[type]);

	if (sub) /* All parameters were matched */
		return TRIS_EVENT_SUB_EXISTS;

	TRIS_RWDLLIST_RDLOCK(&tris_event_subs[TRIS_EVENT_ALL]);
	if (!TRIS_DLLIST_EMPTY(&tris_event_subs[TRIS_EVENT_ALL]))
		res = TRIS_EVENT_SUB_EXISTS;
	TRIS_RWDLLIST_UNLOCK(&tris_event_subs[TRIS_EVENT_ALL]);

	return res;
}

static int match_ie_val(const struct tris_event *event,
		const struct tris_event_ie_val *ie_val, const struct tris_event *event2)
{
	if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_UINT) {
		uint32_t val = event2 ? tris_event_get_ie_uint(event2, ie_val->ie_type) : ie_val->payload.uint;
		if (val == tris_event_get_ie_uint(event, ie_val->ie_type))
			return 1;
		return 0;
	}

	if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_STR) {
		const char *str;
		uint32_t hash;

		hash = event2 ? tris_event_get_ie_str_hash(event2, ie_val->ie_type) : ie_val->payload.hash;
		if (hash != tris_event_get_ie_str_hash(event, ie_val->ie_type)) {
			return 0;
		}

		str = event2 ? tris_event_get_ie_str(event2, ie_val->ie_type) : ie_val->payload.str;
		if (str && !strcmp(str, tris_event_get_ie_str(event, ie_val->ie_type))) {
			return 1;
		}

		return 0;
	}

	if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_RAW) {
		const void *buf = event2 ? tris_event_get_ie_raw(event2, ie_val->ie_type) : ie_val->payload.raw;
		if (buf && !memcmp(buf, tris_event_get_ie_raw(event, ie_val->ie_type), ie_val->raw_datalen))
			return 1;
		return 0;
	}

	if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_EXISTS) {
		if (tris_event_get_ie_raw(event, ie_val->ie_type))
			return 1;
		return 0;
	}

	return 0;
}

static int dump_cache_cb(void *obj, void *arg, int flags)
{
	const struct tris_event_ref *event_ref = obj;
	const struct tris_event *event = event_ref->event;
	const struct tris_event_sub *event_sub = arg;
	struct tris_event_ie_val *ie_val = NULL;

	TRIS_LIST_TRAVERSE(&event_sub->ie_vals, ie_val, entry) {
		if (!match_ie_val(event, ie_val, NULL)) {
			break;
		}
	}

	if (!ie_val) {
		/* All parameters were matched on this cache entry, so dump it */
		event_sub->cb(event, event_sub->userdata);
	}

	return 0;
}

/*! \brief Dump the event cache for the subscribed event type */
void tris_event_dump_cache(const struct tris_event_sub *event_sub)
{
	ao2_callback(tris_event_cache[event_sub->type].container, OBJ_NODATA,
			dump_cache_cb, (void *) event_sub);
}

static struct tris_event *gen_sub_event(struct tris_event_sub *sub)
{
	struct tris_event_ie_val *ie_val;
	struct tris_event *event;

	event = tris_event_new(TRIS_EVENT_SUB,
		TRIS_EVENT_IE_UNIQUEID,  TRIS_EVENT_IE_PLTYPE_UINT, sub->uniqueid,
		TRIS_EVENT_IE_EVENTTYPE, TRIS_EVENT_IE_PLTYPE_UINT, sub->type,
		TRIS_EVENT_IE_END);

	if (!event)
		return NULL;

	TRIS_LIST_TRAVERSE(&sub->ie_vals, ie_val, entry) {
		switch (ie_val->ie_pltype) {
		case TRIS_EVENT_IE_PLTYPE_UNKNOWN:
			break;
		case TRIS_EVENT_IE_PLTYPE_EXISTS:
			tris_event_append_ie_uint(&event, TRIS_EVENT_IE_EXISTS, ie_val->ie_type);
			break;
		case TRIS_EVENT_IE_PLTYPE_UINT:
			tris_event_append_ie_uint(&event, ie_val->ie_type, ie_val->payload.uint);
			break;
		case TRIS_EVENT_IE_PLTYPE_STR:
			tris_event_append_ie_str(&event, ie_val->ie_type, ie_val->payload.str);
			break;
		case TRIS_EVENT_IE_PLTYPE_RAW:
			tris_event_append_ie_raw(&event, ie_val->ie_type, ie_val->payload.raw, ie_val->raw_datalen);
			break;
		}
		if (!event)
			break;
	}

	return event;
}

/*! \brief Send TRIS_EVENT_SUB events to this subscriber of ... subscriber events */
void tris_event_report_subs(const struct tris_event_sub *event_sub)
{
	struct tris_event *event;
	struct tris_event_sub *sub;
	enum tris_event_type event_type = -1;
	struct tris_event_ie_val *ie_val;

	if (event_sub->type != TRIS_EVENT_SUB)
		return;

	TRIS_LIST_TRAVERSE(&event_sub->ie_vals, ie_val, entry) {
		if (ie_val->ie_type == TRIS_EVENT_IE_EVENTTYPE) {
			event_type = ie_val->payload.uint;
			break;
		}
	}

	if (event_type == -1)
		return;

	TRIS_RWDLLIST_RDLOCK(&tris_event_subs[event_type]);
	TRIS_RWDLLIST_TRAVERSE(&tris_event_subs[event_type], sub, entry) {
		if (event_sub == sub)
			continue;

		event = gen_sub_event(sub);

		if (!event)
			continue;

		event_sub->cb(event, event_sub->userdata);

		tris_event_destroy(event);
	}
	TRIS_RWDLLIST_UNLOCK(&tris_event_subs[event_type]);
}

struct tris_event_sub *tris_event_subscribe_new(enum tris_event_type type, 
	tris_event_cb_t cb, void *userdata)
{
	struct tris_event_sub *sub;

	if (type < 0 || type >= TRIS_EVENT_TOTAL) {
		tris_log(LOG_ERROR, "%u is an invalid type!\n", type);
		return NULL;
	}

	if (!(sub = tris_calloc(1, sizeof(*sub))))
		return NULL;

	sub->type = type;
	sub->cb = cb;
	sub->userdata = userdata;
	sub->uniqueid = tris_atomic_fetchadd_int((int *) &sub_uniqueid, 1);

	return sub;
}

int tris_event_sub_append_ie_uint(struct tris_event_sub *sub,
	enum tris_event_ie_type ie_type, uint32_t unsigned_int)
{
	struct tris_event_ie_val *ie_val;

	if (ie_type < 0 || ie_type > TRIS_EVENT_IE_MAX)
		return -1;

	if (!(ie_val = tris_calloc(1, sizeof(*ie_val))))
		return -1;

	ie_val->ie_type = ie_type;
	ie_val->payload.uint = unsigned_int;
	ie_val->ie_pltype = TRIS_EVENT_IE_PLTYPE_UINT;

	TRIS_LIST_INSERT_TAIL(&sub->ie_vals, ie_val, entry);

	return 0;
}

int tris_event_sub_append_ie_exists(struct tris_event_sub *sub,
	enum tris_event_ie_type ie_type)
{
	struct tris_event_ie_val *ie_val;

	if (ie_type < 0 || ie_type > TRIS_EVENT_IE_MAX)
		return -1;

	if (!(ie_val = tris_calloc(1, sizeof(*ie_val))))
		return -1;

	ie_val->ie_type = ie_type;
	ie_val->ie_pltype = TRIS_EVENT_IE_PLTYPE_EXISTS;

	TRIS_LIST_INSERT_TAIL(&sub->ie_vals, ie_val, entry);

	return 0;
}

int tris_event_sub_append_ie_str(struct tris_event_sub *sub, 	
	enum tris_event_ie_type ie_type, const char *str)
{
	struct tris_event_ie_val *ie_val;

	if (ie_type < 0 || ie_type > TRIS_EVENT_IE_MAX)
		return -1;

	if (!(ie_val = tris_calloc(1, sizeof(*ie_val))))
		return -1;

	ie_val->ie_type = ie_type;
	ie_val->ie_pltype = TRIS_EVENT_IE_PLTYPE_STR;

	if (!(ie_val->payload.str = tris_strdup(str))) {
		tris_free(ie_val);
		return -1;
	}

	ie_val->payload.hash = tris_str_hash(str);

	TRIS_LIST_INSERT_TAIL(&sub->ie_vals, ie_val, entry);

	return 0;
}

int tris_event_sub_append_ie_raw(struct tris_event_sub *sub, 	
	enum tris_event_ie_type ie_type, void *data, size_t raw_datalen)
{
	struct tris_event_ie_val *ie_val;

	if (ie_type < 0 || ie_type > TRIS_EVENT_IE_MAX)
		return -1;

	if (!(ie_val = tris_calloc(1, sizeof(*ie_val))))
		return -1;

	ie_val->ie_type = ie_type;
	ie_val->ie_pltype = TRIS_EVENT_IE_PLTYPE_RAW;
	ie_val->raw_datalen = raw_datalen;

	if (!(ie_val->payload.raw = tris_malloc(raw_datalen))) {
		tris_free(ie_val);
		return -1;
	}

	memcpy(ie_val->payload.raw, data, raw_datalen);

	TRIS_LIST_INSERT_TAIL(&sub->ie_vals, ie_val, entry);

	return 0;
}

int tris_event_sub_activate(struct tris_event_sub *sub)
{
	if (tris_event_check_subscriber(TRIS_EVENT_SUB,
		TRIS_EVENT_IE_EVENTTYPE, TRIS_EVENT_IE_PLTYPE_UINT, sub->type,
		TRIS_EVENT_IE_END) != TRIS_EVENT_SUB_NONE) {
		struct tris_event *event;

		event = gen_sub_event(sub);

		if (event)
			tris_event_queue(event);
	}

	TRIS_RWDLLIST_WRLOCK(&tris_event_subs[sub->type]);
	TRIS_RWDLLIST_INSERT_TAIL(&tris_event_subs[sub->type], sub, entry);
	TRIS_RWDLLIST_UNLOCK(&tris_event_subs[sub->type]);

	return 0;
}

struct tris_event_sub *tris_event_subscribe(enum tris_event_type type, tris_event_cb_t cb, 
	void *userdata, ...)
{
	va_list ap;
	enum tris_event_ie_type ie_type;
	struct tris_event_sub *sub;

	if (!(sub = tris_event_subscribe_new(type, cb, userdata)))
		return NULL;

	va_start(ap, userdata);
	for (ie_type = va_arg(ap, enum tris_event_ie_type);
		ie_type != TRIS_EVENT_IE_END;
		ie_type = va_arg(ap, enum tris_event_ie_type))
	{
		enum tris_event_ie_pltype ie_pltype;

		ie_pltype = va_arg(ap, enum tris_event_ie_pltype);

		switch (ie_pltype) {
		case TRIS_EVENT_IE_PLTYPE_UNKNOWN:
			break;
		case TRIS_EVENT_IE_PLTYPE_UINT:
		{
			uint32_t unsigned_int = va_arg(ap, uint32_t);
			tris_event_sub_append_ie_uint(sub, ie_type, unsigned_int);
			break;
		}
		case TRIS_EVENT_IE_PLTYPE_STR:
		{
			const char *str = va_arg(ap, const char *);
			tris_event_sub_append_ie_str(sub, ie_type, str);
			break;
		}
		case TRIS_EVENT_IE_PLTYPE_RAW:
		{
			void *data = va_arg(ap, void *);
			size_t data_len = va_arg(ap, size_t);
			tris_event_sub_append_ie_raw(sub, ie_type, data, data_len);
			break;
		}
		case TRIS_EVENT_IE_PLTYPE_EXISTS:
			tris_event_sub_append_ie_exists(sub, ie_type);
			break;
		}
	}
	va_end(ap);

	tris_event_sub_activate(sub);

	return sub;
}

void tris_event_sub_destroy(struct tris_event_sub *sub)
{
	struct tris_event_ie_val *ie_val;

	while ((ie_val = TRIS_LIST_REMOVE_HEAD(&sub->ie_vals, entry)))
		tris_event_ie_val_destroy(ie_val);

	tris_free(sub);
}

struct tris_event_sub *tris_event_unsubscribe(struct tris_event_sub *sub)
{
	struct tris_event *event;

	TRIS_RWDLLIST_WRLOCK(&tris_event_subs[sub->type]);
	TRIS_DLLIST_REMOVE(&tris_event_subs[sub->type], sub, entry);
	TRIS_RWDLLIST_UNLOCK(&tris_event_subs[sub->type]);

	if (tris_event_check_subscriber(TRIS_EVENT_UNSUB,
		TRIS_EVENT_IE_EVENTTYPE, TRIS_EVENT_IE_PLTYPE_UINT, sub->type,
		TRIS_EVENT_IE_END) != TRIS_EVENT_SUB_NONE) {
		
		event = tris_event_new(TRIS_EVENT_UNSUB,
			TRIS_EVENT_IE_UNIQUEID,  TRIS_EVENT_IE_PLTYPE_UINT, sub->uniqueid,
			TRIS_EVENT_IE_EVENTTYPE, TRIS_EVENT_IE_PLTYPE_UINT, sub->type,
			TRIS_EVENT_IE_END);

		if (event)
			tris_event_queue(event);
	}

	tris_event_sub_destroy(sub);

	return NULL;
}

void tris_event_iterator_init(struct tris_event_iterator *iterator, const struct tris_event *event)
{
	iterator->event_len = ntohs(event->event_len);
	iterator->event = event;
	iterator->ie = (struct tris_event_ie *) ( ((char *) event) + sizeof(*event) );
	return;
}

int tris_event_iterator_next(struct tris_event_iterator *iterator)
{
	iterator->ie = (struct tris_event_ie *) ( ((char *) iterator->ie) + sizeof(*iterator->ie) + ntohs(iterator->ie->ie_payload_len));
	return ((iterator->event_len <= (((char *) iterator->ie) - ((char *) iterator->event))) ? -1 : 0);
}

enum tris_event_ie_type tris_event_iterator_get_ie_type(struct tris_event_iterator *iterator)
{
	return ntohs(iterator->ie->ie_type);
}

uint32_t tris_event_iterator_get_ie_uint(struct tris_event_iterator *iterator)
{
	return ntohl(get_unaligned_uint32(iterator->ie->ie_payload));
}

const char *tris_event_iterator_get_ie_str(struct tris_event_iterator *iterator)
{
	const struct tris_event_ie_str_payload *str_payload;

	str_payload = (struct tris_event_ie_str_payload *) iterator->ie->ie_payload;

	return str_payload ? str_payload->str : NULL;
}

void *tris_event_iterator_get_ie_raw(struct tris_event_iterator *iterator)
{
	return iterator->ie->ie_payload;
}

enum tris_event_type tris_event_get_type(const struct tris_event *event)
{
	return ntohs(event->type);
}

uint32_t tris_event_get_ie_uint(const struct tris_event *event, enum tris_event_ie_type ie_type)
{
	const uint32_t *ie_val;

	ie_val = tris_event_get_ie_raw(event, ie_type);

	return ie_val ? ntohl(get_unaligned_uint32(ie_val)) : 0;
}

uint32_t tris_event_get_ie_str_hash(const struct tris_event *event, enum tris_event_ie_type ie_type)
{
	const struct tris_event_ie_str_payload *str_payload;

	str_payload = tris_event_get_ie_raw(event, ie_type);

	return str_payload ? str_payload->hash : 0;
}

const char *tris_event_get_ie_str(const struct tris_event *event, enum tris_event_ie_type ie_type)
{
	const struct tris_event_ie_str_payload *str_payload;

	str_payload = tris_event_get_ie_raw(event, ie_type);

	return str_payload ? str_payload->str : NULL;
}

const void *tris_event_get_ie_raw(const struct tris_event *event, enum tris_event_ie_type ie_type)
{
	struct tris_event_iterator iterator;
	int res = 0;

	for (tris_event_iterator_init(&iterator, event); !res; res = tris_event_iterator_next(&iterator)) {
		if (tris_event_iterator_get_ie_type(&iterator) == ie_type)
			return tris_event_iterator_get_ie_raw(&iterator);
	}

	return NULL;
}

int tris_event_append_ie_str(struct tris_event **event, enum tris_event_ie_type ie_type,
	const char *str)
{
	struct tris_event_ie_str_payload *str_payload;
	size_t payload_len;

	payload_len = sizeof(*str_payload) + strlen(str);
	str_payload = alloca(payload_len);

	strcpy(str_payload->str, str);
	str_payload->hash = tris_str_hash(str);

	return tris_event_append_ie_raw(event, ie_type, str_payload, payload_len);
}

int tris_event_append_ie_uint(struct tris_event **event, enum tris_event_ie_type ie_type,
	uint32_t data)
{
	data = htonl(data);
	return tris_event_append_ie_raw(event, ie_type, &data, sizeof(data));
}

int tris_event_append_ie_raw(struct tris_event **event, enum tris_event_ie_type ie_type,
	const void *data, size_t data_len)
{
	struct tris_event_ie *ie;
	unsigned int extra_len;
	uint16_t event_len;

	event_len = ntohs((*event)->event_len);
	extra_len = sizeof(*ie) + data_len;

	if (!(*event = tris_realloc(*event, event_len + extra_len)))
		return -1;

	ie = (struct tris_event_ie *) ( ((char *) *event) + event_len );
	ie->ie_type = htons(ie_type);
	ie->ie_payload_len = htons(data_len);
	memcpy(ie->ie_payload, data, data_len);

	(*event)->event_len = htons(event_len + extra_len);

	return 0;
}

struct tris_event *tris_event_new(enum tris_event_type type, ...)
{
	va_list ap;
	struct tris_event *event;
	enum tris_event_ie_type ie_type;
	struct tris_event_ie_val *ie_val;
	TRIS_LIST_HEAD_NOLOCK_STATIC(ie_vals, tris_event_ie_val);

	/* Invalid type */
	if (type >= TRIS_EVENT_TOTAL) {
		tris_log(LOG_WARNING, "Someone tried to create an event of invalid "
			"type '%d'!\n", type);
		return NULL;
	}

	va_start(ap, type);
	for (ie_type = va_arg(ap, enum tris_event_ie_type);
		ie_type != TRIS_EVENT_IE_END;
		ie_type = va_arg(ap, enum tris_event_ie_type))
	{
		struct tris_event_ie_val *ie_value = alloca(sizeof(*ie_value));
		memset(ie_value, 0, sizeof(*ie_value));
		ie_value->ie_type = ie_type;
		ie_value->ie_pltype = va_arg(ap, enum tris_event_ie_pltype);
		if (ie_value->ie_pltype == TRIS_EVENT_IE_PLTYPE_UINT)
			ie_value->payload.uint = va_arg(ap, uint32_t);
		else if (ie_value->ie_pltype == TRIS_EVENT_IE_PLTYPE_STR)
			ie_value->payload.str = tris_strdupa(va_arg(ap, const char *));
		else if (ie_value->ie_pltype == TRIS_EVENT_IE_PLTYPE_RAW) {
			void *data = va_arg(ap, void *);
			size_t datalen = va_arg(ap, size_t);
			ie_value->payload.raw = alloca(datalen);
			memcpy(ie_value->payload.raw, data, datalen);
			ie_value->raw_datalen = datalen;
		}
		TRIS_LIST_INSERT_TAIL(&ie_vals, ie_value, entry);
	}
	va_end(ap);

	if (!(event = tris_calloc(1, sizeof(*event))))
		return NULL;

	event->type = htons(type);
	event->event_len = htons(sizeof(*event));

	TRIS_LIST_TRAVERSE(&ie_vals, ie_val, entry) {
		if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_STR)
			tris_event_append_ie_str(&event, ie_val->ie_type, ie_val->payload.str);
		else if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_UINT)
			tris_event_append_ie_uint(&event, ie_val->ie_type, ie_val->payload.uint);
		else if (ie_val->ie_pltype == TRIS_EVENT_IE_PLTYPE_RAW)
			tris_event_append_ie_raw(&event, ie_val->ie_type, ie_val->payload.raw, ie_val->raw_datalen);

		if (!event)
			break;
	}

	if (!tris_event_get_ie_raw(event, TRIS_EVENT_IE_EID)) {
		/* If the event is originating on this server, add the server's
		 * entity ID to the event. */
		tris_event_append_ie_raw(&event, TRIS_EVENT_IE_EID, &tris_eid_default, sizeof(tris_eid_default));
	}

	return event;
}

void tris_event_destroy(struct tris_event *event)
{
	tris_free(event);
}

static void tris_event_ref_destroy(void *obj)
{
	struct tris_event_ref *event_ref = obj;

	tris_event_destroy(event_ref->event);
}

static struct tris_event *tris_event_dup(const struct tris_event *event)
{
	struct tris_event *dup_event;
	uint16_t event_len;

	event_len = tris_event_get_size(event);

	if (!(dup_event = tris_calloc(1, event_len))) {
		return NULL;
	}

	memcpy(dup_event, event, event_len);

	return dup_event;
}

struct tris_event *tris_event_get_cached(enum tris_event_type type, ...)
{
	va_list ap;
	enum tris_event_ie_type ie_type;
	struct tris_event *dup_event = NULL;
	struct tris_event_ref *cached_event_ref;
	struct tris_event *cache_arg_event;
	struct tris_event_ref tmp_event_ref = {
		.event = NULL,
	};
	struct ao2_container *container = NULL;

	if (type >= TRIS_EVENT_TOTAL) {
		tris_log(LOG_ERROR, "%u is an invalid type!\n", type);
		return NULL;
	}

	if (!(container = tris_event_cache[type].container)) {
		tris_log(LOG_ERROR, "%u is not a cached event type\n", type);
		return NULL;
	}

	if (!(cache_arg_event = tris_event_new(type, TRIS_EVENT_IE_END))) {
		return NULL;
	}

	va_start(ap, type);
	for (ie_type = va_arg(ap, enum tris_event_ie_type);
		ie_type != TRIS_EVENT_IE_END;
		ie_type = va_arg(ap, enum tris_event_ie_type))
	{
		enum tris_event_ie_pltype ie_pltype;

		ie_pltype = va_arg(ap, enum tris_event_ie_pltype);

		switch (ie_pltype) {
		case TRIS_EVENT_IE_PLTYPE_UINT:
			tris_event_append_ie_uint(&cache_arg_event, ie_type, va_arg(ap, uint32_t));
			break;
		case TRIS_EVENT_IE_PLTYPE_STR:
			tris_event_append_ie_str(&cache_arg_event, ie_type, va_arg(ap, const char *));
			break;
		case TRIS_EVENT_IE_PLTYPE_RAW:
		{
			void *data = va_arg(ap, void *);
			size_t datalen = va_arg(ap, size_t);
			tris_event_append_ie_raw(&cache_arg_event, ie_type, data, datalen);
		}
		case TRIS_EVENT_IE_PLTYPE_EXISTS:
			tris_log(LOG_WARNING, "PLTYPE_EXISTS not supported by this function\n");
			break;
		case TRIS_EVENT_IE_PLTYPE_UNKNOWN:
			break;
		}
	}
	va_end(ap);

	tmp_event_ref.event = cache_arg_event;

	cached_event_ref = ao2_find(container, &tmp_event_ref, OBJ_POINTER);

	tris_event_destroy(cache_arg_event);
	cache_arg_event = NULL;

	if (cached_event_ref) {
		dup_event = tris_event_dup(cached_event_ref->event);
		ao2_ref(cached_event_ref, -1);
		cached_event_ref = NULL;
	}

	return dup_event;
}

static struct tris_event_ref *alloc_event_ref(void)
{
	return ao2_alloc(sizeof(struct tris_event_ref), tris_event_ref_destroy);
}

/*! \brief Duplicate an event and add it to the cache
 * \note This assumes this index in to the cache is locked */
static int tris_event_dup_and_cache(const struct tris_event *event)
{
	struct tris_event *dup_event;
	struct tris_event_ref *event_ref;

	if (!(dup_event = tris_event_dup(event))) {
		return -1;
	}

	if (!(event_ref = alloc_event_ref())) {
		tris_event_destroy(dup_event);
		return -1;
	}

	event_ref->event = dup_event;

	ao2_link(tris_event_cache[tris_event_get_type(event)].container, event_ref);

	ao2_ref(event_ref, -1);

	return 0;
}

int tris_event_queue_and_cache(struct tris_event *event)
{
	struct ao2_container *container;
	struct tris_event_ref tmp_event_ref = {
		.event = event,
	};
	int res = -1;

	if (!(container = tris_event_cache[tris_event_get_type(event)].container)) {
		tris_log(LOG_WARNING, "cache requested for non-cached event type\n");
		goto queue_event;
	}

	/* Remove matches from the cache */
	ao2_callback(container, OBJ_POINTER | OBJ_UNLINK | OBJ_MULTIPLE | OBJ_NODATA,
			tris_event_cmp, &tmp_event_ref);

	res = tris_event_dup_and_cache(event);

queue_event:
	return tris_event_queue(event) ? -1 : res;
}

static int handle_event(void *data)
{
	struct tris_event_ref *event_ref = data;
	struct tris_event_sub *sub;
	uint16_t host_event_type;

	host_event_type = ntohs(event_ref->event->type);

	/* Subscribers to this specific event first */
	TRIS_RWDLLIST_RDLOCK(&tris_event_subs[host_event_type]);
	TRIS_RWDLLIST_TRAVERSE(&tris_event_subs[host_event_type], sub, entry) {
		struct tris_event_ie_val *ie_val;
		TRIS_LIST_TRAVERSE(&sub->ie_vals, ie_val, entry) {
			if (!match_ie_val(event_ref->event, ie_val, NULL)) {
				break;
			}
		}
		if (ie_val) {
			continue;
		}
		sub->cb(event_ref->event, sub->userdata);
	}
	TRIS_RWDLLIST_UNLOCK(&tris_event_subs[host_event_type]);

	/* Now to subscribers to all event types */
	TRIS_RWDLLIST_RDLOCK(&tris_event_subs[TRIS_EVENT_ALL]);
	TRIS_RWDLLIST_TRAVERSE(&tris_event_subs[TRIS_EVENT_ALL], sub, entry) {
		sub->cb(event_ref->event, sub->userdata);
	}
	TRIS_RWDLLIST_UNLOCK(&tris_event_subs[TRIS_EVENT_ALL]);

	ao2_ref(event_ref, -1);

	return 0;
}

int tris_event_queue(struct tris_event *event)
{
	struct tris_event_ref *event_ref;
	uint16_t host_event_type;

	host_event_type = ntohs(event->type);

	/* Invalid type */
	if (host_event_type >= TRIS_EVENT_TOTAL) {
		tris_log(LOG_WARNING, "Someone tried to queue an event of invalid "
			"type '%d'!\n", host_event_type);
		return -1;
	}

	/* If nobody has subscribed to this event type, throw it away now */
	if (tris_event_check_subscriber(host_event_type, TRIS_EVENT_IE_END)
			== TRIS_EVENT_SUB_NONE) {
		tris_event_destroy(event);
		return 0;
	}

	if (!(event_ref = alloc_event_ref())) {
		return -1;
	}

	event_ref->event = event;

	return tris_taskprocessor_push(event_dispatcher, handle_event, event_ref);
}

static int tris_event_hash_mwi(const void *obj, const int flags)
{
	const struct tris_event *event = obj;
	const char *mailbox = tris_event_get_ie_str(event, TRIS_EVENT_IE_MAILBOX);
	const char *context = tris_event_get_ie_str(event, TRIS_EVENT_IE_CONTEXT);

	return tris_str_hash_add(context, tris_str_hash(mailbox));
}

/*!
 * \internal
 * \brief Hash function for TRIS_EVENT_DEVICE_STATE
 *
 * \param[in] obj an tris_event
 * \param[in] flags unused
 *
 * \return hash value
 */
static int tris_event_hash_devstate(const void *obj, const int flags)
{
	const struct tris_event *event = obj;

	return tris_str_hash(tris_event_get_ie_str(event, TRIS_EVENT_IE_DEVICE));
}

/*!
 * \internal
 * \brief Hash function for TRIS_EVENT_DEVICE_STATE_CHANGE
 *
 * \param[in] obj an tris_event
 * \param[in] flags unused
 *
 * \return hash value
 */
static int tris_event_hash_devstate_change(const void *obj, const int flags)
{
	const struct tris_event *event = obj;

	return tris_str_hash(tris_event_get_ie_str(event, TRIS_EVENT_IE_DEVICE));
}

static int tris_event_hash(const void *obj, const int flags)
{
	const struct tris_event_ref *event_ref;
	const struct tris_event *event;
	ao2_hash_fn *hash_fn;

	event_ref = obj;
	event = event_ref->event;

	if (!(hash_fn = tris_event_cache[tris_event_get_type(event)].hash_fn)) {
		return 0;
	}

	return hash_fn(event, flags);
}

/*!
 * \internal
 * \brief Compare two events
 *
 * \param[in] obj the first event, as an tris_event_ref
 * \param[in] arg the second event, as an tris_event_ref
 * \param[in] flags unused
 *
 * \pre Both events must be the same type.
 * \pre The event type must be declared as a cached event type in tris_event_cache
 *
 * \details This function takes two events, and determines if they are considered
 * equivalent.  The values of information elements specified in the cache arguments
 * for the event type are used to determine if the events are equivalent.
 *
 * \retval 0 No match
 * \retval CMP_MATCH The events are considered equivalent based on the cache arguments
 */
static int tris_event_cmp(void *obj, void *arg, int flags)
{
	struct tris_event_ref *event_ref, *event_ref2;
	struct tris_event *event, *event2;
	int res = CMP_MATCH;
	int i;
	enum tris_event_ie_type *cache_args;

	event_ref = obj;
	event = event_ref->event;

	event_ref2 = arg;
	event2 = event_ref2->event;

	cache_args = tris_event_cache[tris_event_get_type(event)].cache_args;

	for (i = 0; i < ARRAY_LEN(tris_event_cache[0].cache_args) && cache_args[i]; i++) {
		struct tris_event_ie_val ie_val = {
			.ie_pltype = tris_event_get_ie_pltype(cache_args[i]),
			.ie_type = cache_args[i],
		};

		if (!match_ie_val(event, &ie_val, event2)) {
			res = 0;
			break;
		}
	}

	return res;
}

int tris_event_init(void)
{
	int i;

	for (i = 0; i < TRIS_EVENT_TOTAL; i++) {
		TRIS_RWDLLIST_HEAD_INIT(&tris_event_subs[i]);
	}

	for (i = 0; i < TRIS_EVENT_TOTAL; i++) {
		if (!tris_event_cache[i].hash_fn) {
			/* This event type is not cached. */
			continue;
		}

		if (!(tris_event_cache[i].container = ao2_container_alloc(NUM_CACHE_BUCKETS,
				tris_event_hash, tris_event_cmp))) {
			return -1;
		}
	}

	if (!(event_dispatcher = tris_taskprocessor_get("core_event_dispatcher", 0))) {
		return -1;
	}

	return 0;
}
