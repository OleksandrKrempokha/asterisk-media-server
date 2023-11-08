/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief Channel Management
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 246901 $")

#include "trismedia/_private.h"

#include <sys/time.h>
#include <signal.h>
#include <math.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_SYSTEM_NAME */

#include "trismedia/pbx.h"
#include "trismedia/frame.h"
#include "trismedia/mod_format.h"
#include "trismedia/sched.h"
#include "trismedia/channel.h"
#include "trismedia/musiconhold.h"
#include "trismedia/say.h"
#include "trismedia/file.h"
#include "trismedia/cli.h"
#include "trismedia/translate.h"
#include "trismedia/manager.h"
#include "trismedia/chanvars.h"
#include "trismedia/linkedlists.h"
#include "trismedia/indications.h"
#include "trismedia/monitor.h"
#include "trismedia/causes.h"
#include "trismedia/callerid.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"
#include "trismedia/transcap.h"
#include "trismedia/devicestate.h"
#include "trismedia/sha1.h"
#include "trismedia/threadstorage.h"
#include "trismedia/slinfactory.h"
#include "trismedia/audiohook.h"
#include "trismedia/timing.h"

#ifdef HAVE_EPOLL
#include <sys/epoll.h>
#endif

struct tris_epoll_data {
	struct tris_channel *chan;
	int which;
};

/* uncomment if you have problems with 'monitoring' synchronized files */
#if 0
#define MONITOR_CONSTANT_DELAY
#define MONITOR_DELAY	150 * 8		/*!< 150 ms of MONITORING DELAY */
#endif

/*! \brief Prevent new channel allocation if shutting down. */
static int shutting_down;

static int uniqueint;

unsigned long global_fin, global_fout;

TRIS_THREADSTORAGE(state2str_threadbuf);
#define STATE2STR_BUFSIZE   32

/*! Default amount of time to use when emulating a digit as a begin and end 
 *  100ms */
#define TRIS_DEFAULT_EMULATE_DTMF_DURATION 100

/*! Minimum allowed digit length - 80ms */
#define TRIS_MIN_DTMF_DURATION 80

/*! Minimum amount of time between the end of the last digit and the beginning 
 *  of a new one - 45ms */
#define TRIS_MIN_DTMF_GAP 45

/*! \brief List of channel drivers */
struct chanlist {
	const struct tris_channel_tech *tech;
	TRIS_LIST_ENTRY(chanlist) list;
};

#ifdef CHANNEL_TRACE
/*! \brief Structure to hold channel context backtrace data */
struct tris_chan_trace_data {
	int enabled;
	TRIS_LIST_HEAD_NOLOCK(, tris_chan_trace) trace;
};

/*! \brief Structure to save contexts where an tris_chan has been into */
struct tris_chan_trace {
	char context[TRIS_MAX_CONTEXT];
	char exten[TRIS_MAX_EXTENSION];
	int priority;
	TRIS_LIST_ENTRY(tris_chan_trace) entry;
};
#endif

/*! \brief the list of registered channel types */
static TRIS_LIST_HEAD_NOLOCK_STATIC(backends, chanlist);

/*! \brief the list of channels we have. Note that the lock for this list is used for
	both the channels list and the backends list.  */
static TRIS_RWLIST_HEAD_STATIC(channels, tris_channel);

/*! \brief map TRIS_CAUSE's to readable string representations 
 *
 * \ref causes.h
*/
static const struct {
	int cause;
	const char *name;
	const char *desc;
} causes[] = {
	{ TRIS_CAUSE_UNALLOCATED, "UNALLOCATED", "Unallocated (unassigned) number" },
	{ TRIS_CAUSE_NO_ROUTE_TRANSIT_NET, "NO_ROUTE_TRANSIT_NET", "No route to specified transmit network" },
	{ TRIS_CAUSE_NO_ROUTE_DESTINATION, "NO_ROUTE_DESTINATION", "No route to destination" },
	{ TRIS_CAUSE_CHANNEL_UNACCEPTABLE, "CHANNEL_UNACCEPTABLE", "Channel unacceptable" },
	{ TRIS_CAUSE_CALL_AWARDED_DELIVERED, "CALL_AWARDED_DELIVERED", "Call awarded and being delivered in an established channel" },
	{ TRIS_CAUSE_NORMAL_CLEARING, "NORMAL_CLEARING", "Normal Clearing" },
	{ TRIS_CAUSE_USER_BUSY, "USER_BUSY", "User busy" },
	{ TRIS_CAUSE_NO_USER_RESPONSE, "NO_USER_RESPONSE", "No user responding" },
	{ TRIS_CAUSE_NO_ANSWER, "NO_ANSWER", "User alerting, no answer" },
	{ TRIS_CAUSE_CALL_REJECTED, "CALL_REJECTED", "Call Rejected" },
	{ TRIS_CAUSE_NUMBER_CHANGED, "NUMBER_CHANGED", "Number changed" },
	{ TRIS_CAUSE_DESTINATION_OUT_OF_ORDER, "DESTINATION_OUT_OF_ORDER", "Destination out of order" },
	{ TRIS_CAUSE_INVALID_NUMBER_FORMAT, "INVALID_NUMBER_FORMAT", "Invalid number format" },
	{ TRIS_CAUSE_FACILITY_REJECTED, "FACILITY_REJECTED", "Facility rejected" },
	{ TRIS_CAUSE_RESPONSE_TO_STATUS_ENQUIRY, "RESPONSE_TO_STATUS_ENQUIRY", "Response to STATus ENQuiry" },
	{ TRIS_CAUSE_NORMAL_UNSPECIFIED, "NORMAL_UNSPECIFIED", "Normal, unspecified" },
	{ TRIS_CAUSE_NORMAL_CIRCUIT_CONGESTION, "NORMAL_CIRCUIT_CONGESTION", "Circuit/channel congestion" },
	{ TRIS_CAUSE_NETWORK_OUT_OF_ORDER, "NETWORK_OUT_OF_ORDER", "Network out of order" },
	{ TRIS_CAUSE_NORMAL_TEMPORARY_FAILURE, "NORMAL_TEMPORARY_FAILURE", "Temporary failure" },
	{ TRIS_CAUSE_SWITCH_CONGESTION, "SWITCH_CONGESTION", "Switching equipment congestion" },
	{ TRIS_CAUSE_ACCESS_INFO_DISCARDED, "ACCESS_INFO_DISCARDED", "Access information discarded" },
	{ TRIS_CAUSE_REQUESTED_CHAN_UNAVAIL, "REQUESTED_CHAN_UNAVAIL", "Requested channel not available" },
	{ TRIS_CAUSE_PRE_EMPTED, "PRE_EMPTED", "Pre-empted" },
	{ TRIS_CAUSE_FACILITY_NOT_SUBSCRIBED, "FACILITY_NOT_SUBSCRIBED", "Facility not subscribed" },
	{ TRIS_CAUSE_OUTGOING_CALL_BARRED, "OUTGOING_CALL_BARRED", "Outgoing call barred" },
	{ TRIS_CAUSE_INCOMING_CALL_BARRED, "INCOMING_CALL_BARRED", "Incoming call barred" },
	{ TRIS_CAUSE_BEARERCAPABILITY_NOTAUTH, "BEARERCAPABILITY_NOTAUTH", "Bearer capability not authorized" },
	{ TRIS_CAUSE_BEARERCAPABILITY_NOTAVAIL, "BEARERCAPABILITY_NOTAVAIL", "Bearer capability not available" },
	{ TRIS_CAUSE_BEARERCAPABILITY_NOTIMPL, "BEARERCAPABILITY_NOTIMPL", "Bearer capability not implemented" },
	{ TRIS_CAUSE_CHAN_NOT_IMPLEMENTED, "CHAN_NOT_IMPLEMENTED", "Channel not implemented" },
	{ TRIS_CAUSE_FACILITY_NOT_IMPLEMENTED, "FACILITY_NOT_IMPLEMENTED", "Facility not implemented" },
	{ TRIS_CAUSE_INVALID_CALL_REFERENCE, "INVALID_CALL_REFERENCE", "Invalid call reference value" },
	{ TRIS_CAUSE_INCOMPATIBLE_DESTINATION, "INCOMPATIBLE_DESTINATION", "Incompatible destination" },
	{ TRIS_CAUSE_INVALID_MSG_UNSPECIFIED, "INVALID_MSG_UNSPECIFIED", "Invalid message unspecified" },
	{ TRIS_CAUSE_MANDATORY_IE_MISSING, "MANDATORY_IE_MISSING", "Mandatory information element is missing" },
	{ TRIS_CAUSE_MESSAGE_TYPE_NONEXIST, "MESSAGE_TYPE_NONEXIST", "Message type nonexist." },
	{ TRIS_CAUSE_WRONG_MESSAGE, "WRONG_MESSAGE", "Wrong message" },
	{ TRIS_CAUSE_IE_NONEXIST, "IE_NONEXIST", "Info. element nonexist or not implemented" },
	{ TRIS_CAUSE_INVALID_IE_CONTENTS, "INVALID_IE_CONTENTS", "Invalid information element contents" },
	{ TRIS_CAUSE_WRONG_CALL_STATE, "WRONG_CALL_STATE", "Message not compatible with call state" },
	{ TRIS_CAUSE_RECOVERY_ON_TIMER_EXPIRE, "RECOVERY_ON_TIMER_EXPIRE", "Recover on timer expiry" },
	{ TRIS_CAUSE_MANDATORY_IE_LENGTH_ERROR, "MANDATORY_IE_LENGTH_ERROR", "Mandatory IE length error" },
	{ TRIS_CAUSE_PROTOCOL_ERROR, "PROTOCOL_ERROR", "Protocol error, unspecified" },
	{ TRIS_CAUSE_INTERWORKING, "INTERWORKING", "Interworking, unspecified" },
};

struct tris_variable *tris_channeltype_list(void)
{
	struct chanlist *cl;
	struct tris_variable *var=NULL, *prev = NULL;
	TRIS_LIST_TRAVERSE(&backends, cl, list) {
		if (prev)  {
			if ((prev->next = tris_variable_new(cl->tech->type, cl->tech->description, "")))
				prev = prev->next;
		} else {
			var = tris_variable_new(cl->tech->type, cl->tech->description, "");
			prev = var;
		}
	}
	return var;
}

/*! \brief Show channel types - CLI command */
static char *handle_cli_core_show_channeltypes(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMAT  "%-10.10s  %-40.40s %-12.12s %-12.12s %-12.12s\n"
	struct chanlist *cl;
	int count_chan = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show channeltypes";
		e->usage =
			"Usage: core show channeltypes\n"
			"       Lists available channel types registered in your\n"
			"       Trismedia server.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, FORMAT, "Type", "Description",       "Devicestate", "Indications", "Transfer");
	tris_cli(a->fd, FORMAT, "----------", "-----------", "-----------", "-----------", "--------");

	TRIS_RWLIST_RDLOCK(&channels);

	TRIS_LIST_TRAVERSE(&backends, cl, list) {
		tris_cli(a->fd, FORMAT, cl->tech->type, cl->tech->description,
			(cl->tech->devicestate) ? "yes" : "no",
			(cl->tech->indicate) ? "yes" : "no",
			(cl->tech->transfer) ? "yes" : "no");
		count_chan++;
	}

	TRIS_RWLIST_UNLOCK(&channels);

	tris_cli(a->fd, "----------\n%d channel drivers registered.\n", count_chan);

	return CLI_SUCCESS;

#undef FORMAT
}

static char *complete_channeltypes(struct tris_cli_args *a)
{
	struct chanlist *cl;
	int which = 0;
	int wordlen;
	char *ret = NULL;

	if (a->pos != 3)
		return NULL;

	wordlen = strlen(a->word);

	TRIS_LIST_TRAVERSE(&backends, cl, list) {
		if (!strncasecmp(a->word, cl->tech->type, wordlen) && ++which > a->n) {
			ret = tris_strdup(cl->tech->type);
			break;
		}
	}
	
	return ret;
}

/*! \brief Show details about a channel driver - CLI command */
static char *handle_cli_core_show_channeltype(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct chanlist *cl = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show channeltype";
		e->usage =
			"Usage: core show channeltype <name>\n"
			"	Show details about the specified channel type, <name>.\n";
		return NULL;
	case CLI_GENERATE:
		return complete_channeltypes(a);
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	
	TRIS_RWLIST_RDLOCK(&channels);

	TRIS_LIST_TRAVERSE(&backends, cl, list) {
		if (!strncasecmp(cl->tech->type, a->argv[3], strlen(cl->tech->type)))
			break;
	}


	if (!cl) {
		tris_cli(a->fd, "\n%s is not a registered channel driver.\n", a->argv[3]);
		TRIS_RWLIST_UNLOCK(&channels);
		return CLI_FAILURE;
	}

	tris_cli(a->fd,
		"-- Info about channel driver: %s --\n"
		"  Device State: %s\n"
		"    Indication: %s\n"
		"     Transfer : %s\n"
		"  Capabilities: %d\n"
		"   Digit Begin: %s\n"
		"     Digit End: %s\n"
		"    Send HTML : %s\n"
		" Image Support: %s\n"
		"  Text Support: %s\n",
		cl->tech->type,
		(cl->tech->devicestate) ? "yes" : "no",
		(cl->tech->indicate) ? "yes" : "no",
		(cl->tech->transfer) ? "yes" : "no",
		(cl->tech->capabilities) ? cl->tech->capabilities : -1,
		(cl->tech->send_digit_begin) ? "yes" : "no",
		(cl->tech->send_digit_end) ? "yes" : "no",
		(cl->tech->send_html) ? "yes" : "no",
		(cl->tech->send_image) ? "yes" : "no",
		(cl->tech->send_text) ? "yes" : "no"
		
	);

	TRIS_RWLIST_UNLOCK(&channels);
	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_channel[] = {
	TRIS_CLI_DEFINE(handle_cli_core_show_channeltypes, "List available channel types"),
	TRIS_CLI_DEFINE(handle_cli_core_show_channeltype,  "Give more details on that channel type")
};

#ifdef CHANNEL_TRACE
/*! \brief Destructor for the channel trace datastore */
static void tris_chan_trace_destroy_cb(void *data)
{
	struct tris_chan_trace *trace;
	struct tris_chan_trace_data *traced = data;
	while ((trace = TRIS_LIST_REMOVE_HEAD(&traced->trace, entry))) {
		tris_free(trace);
	}
	tris_free(traced);
}

/*! \brief Datastore to put the linked list of tris_chan_trace and trace status */
const struct tris_datastore_info tris_chan_trace_datastore_info = {
	.type = "ChanTrace",
	.destroy = tris_chan_trace_destroy_cb
};

/*! \brief Put the channel backtrace in a string */
int tris_channel_trace_serialize(struct tris_channel *chan, struct tris_str **buf)
{
	int total = 0;
	struct tris_chan_trace *trace;
	struct tris_chan_trace_data *traced;
	struct tris_datastore *store;

	tris_channel_lock(chan);
	store = tris_channel_datastore_find(chan, &tris_chan_trace_datastore_info, NULL);
	if (!store) {
		tris_channel_unlock(chan);
		return total;
	}
	traced = store->data;
	tris_str_reset(*buf);
	TRIS_LIST_TRAVERSE(&traced->trace, trace, entry) {
		if (tris_str_append(buf, 0, "[%d] => %s, %s, %d\n", total, trace->context, trace->exten, trace->priority) < 0) {
			tris_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
			total = -1;
			break;
		}
		total++;
	}
	tris_channel_unlock(chan);
	return total;
}

/* !\brief Whether or not context tracing is enabled */
int tris_channel_trace_is_enabled(struct tris_channel *chan)
{
	struct tris_datastore *store = tris_channel_datastore_find(chan, &tris_chan_trace_datastore_info, NULL);
	if (!store)
		return 0;
	return ((struct tris_chan_trace_data *)store->data)->enabled;
}

/*! \brief Update the context backtrace data if tracing is enabled */
static int tris_channel_trace_data_update(struct tris_channel *chan, struct tris_chan_trace_data *traced)
{
	struct tris_chan_trace *trace;
	if (!traced->enabled)
		return 0;
	/* If the last saved context does not match the current one
	   OR we have not saved any context so far, then save the current context */
	if ((!TRIS_LIST_EMPTY(&traced->trace) && strcasecmp(TRIS_LIST_FIRST(&traced->trace)->context, chan->context)) || 
	    (TRIS_LIST_EMPTY(&traced->trace))) {
		/* Just do some debug logging */
		if (TRIS_LIST_EMPTY(&traced->trace))
			tris_log(LOG_DEBUG, "Setting initial trace context to %s\n", chan->context);
		else
			tris_log(LOG_DEBUG, "Changing trace context from %s to %s\n", TRIS_LIST_FIRST(&traced->trace)->context, chan->context);
		/* alloc or bail out */
		trace = tris_malloc(sizeof(*trace));
		if (!trace) 
			return -1;
		/* save the current location and store it in the trace list */
		tris_copy_string(trace->context, chan->context, sizeof(trace->context));
		tris_copy_string(trace->exten, chan->exten, sizeof(trace->exten));
		trace->priority = chan->priority;
		TRIS_LIST_INSERT_HEAD(&traced->trace, trace, entry);
	}
	return 0;
}

/*! \brief Update the context backtrace if tracing is enabled */
int tris_channel_trace_update(struct tris_channel *chan)
{
	struct tris_datastore *store = tris_channel_datastore_find(chan, &tris_chan_trace_datastore_info, NULL);
	if (!store)
		return 0;
	return tris_channel_trace_data_update(chan, store->data);
}

/*! \brief Enable context tracing in the channel */
int tris_channel_trace_enable(struct tris_channel *chan)
{
	struct tris_datastore *store = tris_channel_datastore_find(chan, &tris_chan_trace_datastore_info, NULL);
	struct tris_chan_trace_data *traced;
	if (!store) {
		store = tris_datastore_alloc(&tris_chan_trace_datastore_info, "ChanTrace");
		if (!store) 
			return -1;
		traced = tris_calloc(1, sizeof(*traced));
		if (!traced) {
			tris_datastore_free(store);
			return -1;
		}	
		store->data = traced;
		TRIS_LIST_HEAD_INIT_NOLOCK(&traced->trace);
		tris_channel_datastore_add(chan, store);
	}	
	((struct tris_chan_trace_data *)store->data)->enabled = 1;
	tris_channel_trace_data_update(chan, store->data);
	return 0;
}

/*! \brief Disable context tracing in the channel */
int tris_channel_trace_disable(struct tris_channel *chan)
{
	struct tris_datastore *store = tris_channel_datastore_find(chan, &tris_chan_trace_datastore_info, NULL);
	if (!store)
		return 0;
	((struct tris_chan_trace_data *)store->data)->enabled = 0;
	return 0;
}
#endif /* CHANNEL_TRACE */

/*! \brief Checks to see if a channel is needing hang up */
int tris_check_hangup(struct tris_channel *chan)
{
	if (chan->_softhangup)		/* yes if soft hangup flag set */
		return 1;
	if (tris_tvzero(chan->whentohangup))	/* no if no hangup scheduled */
		return 0;
	if (tris_tvdiff_ms(chan->whentohangup, tris_tvnow()) > 0) 	/* no if hangup time has not come yet. */
		return 0;
	chan->_softhangup |= TRIS_SOFTHANGUP_TIMEOUT;	/* record event */
	return 1;
}

static int tris_check_hangup_locked(struct tris_channel *chan)
{
	int res;
	tris_channel_lock(chan);
	res = tris_check_hangup(chan);
	tris_channel_unlock(chan);
	return res;
}

/*! \brief Initiate system shutdown */
void tris_begin_shutdown(int hangup)
{
	struct tris_channel *c;
	shutting_down = 1;
	if (hangup) {
		TRIS_RWLIST_RDLOCK(&channels);
		TRIS_RWLIST_TRAVERSE(&channels, c, chan_list) {
			tris_softhangup(c, TRIS_SOFTHANGUP_SHUTDOWN);
		}
		TRIS_RWLIST_UNLOCK(&channels);
	}
}

/*! \brief returns number of active/allocated channels */
int tris_active_channels(void)
{
	struct tris_channel *c;
	int cnt = 0;
	TRIS_RWLIST_RDLOCK(&channels);
	TRIS_RWLIST_TRAVERSE(&channels, c, chan_list)
		cnt++;
	TRIS_RWLIST_UNLOCK(&channels);
	return cnt;
}

/*! \brief Cancel a shutdown in progress */
void tris_cancel_shutdown(void)
{
	shutting_down = 0;
}

/*! \brief Returns non-zero if Trismedia is being shut down */
int tris_shutting_down(void)
{
	return shutting_down;
}

/*! \brief Set when to hangup channel */
void tris_channel_setwhentohangup_tv(struct tris_channel *chan, struct timeval offset)
{
	chan->whentohangup = tris_tvzero(offset) ? offset : tris_tvadd(offset, tris_tvnow());
	tris_queue_frame(chan, &tris_null_frame);
	return;
}

void tris_channel_setwhentohangup(struct tris_channel *chan, time_t offset)
{
	struct timeval when = { offset, };
	tris_channel_setwhentohangup_tv(chan, when);
}

/*! \brief Compare a offset with when to hangup channel */
int tris_channel_cmpwhentohangup_tv(struct tris_channel *chan, struct timeval offset)
{
	struct timeval whentohangup;

	if (tris_tvzero(chan->whentohangup))
		return tris_tvzero(offset) ? 0 : -1;

	if (tris_tvzero(offset))
		return 1;

	whentohangup = tris_tvadd(offset, tris_tvnow());

	return tris_tvdiff_ms(whentohangup, chan->whentohangup);
}

int tris_channel_cmpwhentohangup(struct tris_channel *chan, time_t offset)
{
	struct timeval when = { offset, };
	return tris_channel_cmpwhentohangup_tv(chan, when);
}

/*! \brief Register a new telephony channel in Trismedia */
int tris_channel_register(const struct tris_channel_tech *tech)
{
	struct chanlist *chan;

	TRIS_RWLIST_WRLOCK(&channels);

	TRIS_LIST_TRAVERSE(&backends, chan, list) {
		if (!strcasecmp(tech->type, chan->tech->type)) {
			tris_log(LOG_WARNING, "Already have a handler for type '%s'\n", tech->type);
			TRIS_RWLIST_UNLOCK(&channels);
			return -1;
		}
	}
	
	if (!(chan = tris_calloc(1, sizeof(*chan)))) {
		TRIS_RWLIST_UNLOCK(&channels);
		return -1;
	}
	chan->tech = tech;
	TRIS_LIST_INSERT_HEAD(&backends, chan, list);

	tris_debug(1, "Registered handler for '%s' (%s)\n", chan->tech->type, chan->tech->description);

	tris_verb(2, "Registered channel type '%s' (%s)\n", chan->tech->type, chan->tech->description);

	TRIS_RWLIST_UNLOCK(&channels);
	return 0;
}

/*! \brief Unregister channel driver */
void tris_channel_unregister(const struct tris_channel_tech *tech)
{
	struct chanlist *chan;

	tris_debug(1, "Unregistering channel type '%s'\n", tech->type);

	TRIS_RWLIST_WRLOCK(&channels);

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&backends, chan, list) {
		if (chan->tech == tech) {
			TRIS_LIST_REMOVE_CURRENT(list);
			tris_free(chan);
			tris_verb(2, "Unregistered channel type '%s'\n", tech->type);
			break;	
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	TRIS_RWLIST_UNLOCK(&channels);
}

/*! \brief Get handle to channel driver based on name */
const struct tris_channel_tech *tris_get_channel_tech(const char *name)
{
	struct chanlist *chanls;
	const struct tris_channel_tech *ret = NULL;

	TRIS_RWLIST_RDLOCK(&channels);

	TRIS_LIST_TRAVERSE(&backends, chanls, list) {
		if (!strcasecmp(name, chanls->tech->type)) {
			ret = chanls->tech;
			break;
		}
	}

	TRIS_RWLIST_UNLOCK(&channels);
	
	return ret;
}

/*! \brief Gives the string form of a given hangup cause */
const char *tris_cause2str(int cause)
{
	int x;

	for (x = 0; x < ARRAY_LEN(causes); x++) {
		if (causes[x].cause == cause)
			return causes[x].desc;
	}

	return "Unknown";
}

/*! \brief Convert a symbolic hangup cause to number */
int tris_str2cause(const char *name)
{
	int x;

	for (x = 0; x < ARRAY_LEN(causes); x++)
		if (!strncasecmp(causes[x].name, name, strlen(causes[x].name)))
			return causes[x].cause;

	return -1;
}

/*! \brief Gives the string form of a given channel state.
	\note This function is not reentrant.
 */
const char *tris_state2str(enum tris_channel_state state)
{
	char *buf;

	switch (state) {
	case TRIS_STATE_DOWN:
		return "Down";
	case TRIS_STATE_RESERVED:
		return "Rsrvd";
	case TRIS_STATE_OFFHOOK:
		return "OffHook";
	case TRIS_STATE_DIALING:
		return "Dialing";
	case TRIS_STATE_RING:
		return "Ring";
	case TRIS_STATE_RINGING:
		return "Ringing";
	case TRIS_STATE_UP:
		return "Up";
	case TRIS_STATE_BUSY:
		return "Busy";
	case TRIS_STATE_DIALING_OFFHOOK:
		return "Dialing Offhook";
	case TRIS_STATE_PRERING:
		return "Pre-ring";
	default:
		if (!(buf = tris_threadstorage_get(&state2str_threadbuf, STATE2STR_BUFSIZE)))
			return "Unknown";
		snprintf(buf, STATE2STR_BUFSIZE, "Unknown (%d)", state);
		return buf;
	}
}

/*! \brief Gives the string form of a given transfer capability */
char *tris_transfercapability2str(int transfercapability)
{
	switch (transfercapability) {
	case TRIS_TRANS_CAP_SPEECH:
		return "SPEECH";
	case TRIS_TRANS_CAP_DIGITAL:
		return "DIGITAL";
	case TRIS_TRANS_CAP_RESTRICTED_DIGITAL:
		return "RESTRICTED_DIGITAL";
	case TRIS_TRANS_CAP_3_1K_AUDIO:
		return "3K1AUDIO";
	case TRIS_TRANS_CAP_DIGITAL_W_TONES:
		return "DIGITAL_W_TONES";
	case TRIS_TRANS_CAP_VIDEO:
		return "VIDEO";
	default:
		return "UNKNOWN";
	}
}

/*! \brief Pick the best audio codec */
int tris_best_codec(int fmts)
{
	/* This just our opinion, expressed in code.  We are asked to choose
	   the best codec to use, given no information */
	int x;
	static const int prefs[] =
	{
		/*! Okay, ulaw is used by all telephony equipment, so start with it */
		TRIS_FORMAT_ULAW,
		/*! Unless of course, you're a silly European, so then prefer ALAW */
		TRIS_FORMAT_ALAW,
		TRIS_FORMAT_SIREN14,
		TRIS_FORMAT_SIREN7,
		/*! G.722 is better then all below, but not as common as the above... so give ulaw and alaw priority */
		TRIS_FORMAT_G722,
		/*! Okay, well, signed linear is easy to translate into other stuff */
		TRIS_FORMAT_SLINEAR16,
		TRIS_FORMAT_SLINEAR,
		/*! G.726 is standard ADPCM, in RFC3551 packing order */
		TRIS_FORMAT_G726,
		/*! G.726 is standard ADPCM, in AAL2 packing order */
		TRIS_FORMAT_G726_AAL2,
		/*! ADPCM has great sound quality and is still pretty easy to translate */
		TRIS_FORMAT_ADPCM,
		/*! Okay, we're down to vocoders now, so pick GSM because it's small and easier to
		    translate and sounds pretty good */
		TRIS_FORMAT_GSM,
		/*! iLBC is not too bad */
		TRIS_FORMAT_ILBC,
		/*! Speex is free, but computationally more expensive than GSM */
		TRIS_FORMAT_SPEEX16,
		TRIS_FORMAT_SPEEX,
		/*! Ick, LPC10 sounds terrible, but at least we have code for it, if you're tacky enough
		    to use it */
		TRIS_FORMAT_LPC10,
		/*! G.729a is faster than 723 and slightly less expensive */
		TRIS_FORMAT_G729A,
		/*! Down to G.723.1 which is proprietary but at least designed for voice */
		TRIS_FORMAT_G723_1,
	};

	/* Strip out video */
	fmts &= TRIS_FORMAT_AUDIO_MASK;
	
	/* Find the first preferred codec in the format given */
	for (x = 0; x < ARRAY_LEN(prefs); x++) {
		if (fmts & prefs[x])
			return prefs[x];
	}

	tris_log(LOG_WARNING, "Don't know any of 0x%x formats\n", fmts);

	return 0;
}

static const struct tris_channel_tech null_tech = {
	.type = "NULL",
	.description = "Null channel (should not see this)",
};

/*! \brief Create a new channel structure */
static struct tris_channel * attribute_malloc __attribute__((format(printf, 12, 0)))
__tris_channel_alloc_ap(int needqueue, int state, const char *cid_num, const char *cid_name,
		       const char *acctcode, const char *exten, const char *context,
		       const int amaflag, const char *file, int line, const char *function,
		       const char *name_fmt, va_list ap1, va_list ap2)
{
	struct tris_channel *tmp;
	int x;
	int flags;
	struct varshead *headp;

	/* If shutting down, don't allocate any new channels */
	if (shutting_down) {
		tris_log(LOG_WARNING, "Channel allocation failed: Refusing due to active shutdown\n");
		return NULL;
	}

#if defined(__TRIS_DEBUG_MALLOC)
	if (!(tmp = __tris_calloc(1, sizeof(*tmp), file, line, function))) {
		return NULL;
	}
#else
	if (!(tmp = tris_calloc(1, sizeof(*tmp)))) {
		return NULL;
	}
#endif

	if (!(tmp->sched = sched_context_create())) {
		tris_log(LOG_WARNING, "Channel allocation failed: Unable to create schedule context\n");
		tris_free(tmp);
		return NULL;
	}
	
	if ((tris_string_field_init(tmp, 128))) {
		sched_context_destroy(tmp->sched);
		tris_free(tmp);
		return NULL;
	}

#ifdef HAVE_EPOLL
	tmp->epfd = epoll_create(25);
#endif

	for (x = 0; x < TRIS_MAX_FDS; x++) {
		tmp->fds[x] = -1;
#ifdef HAVE_EPOLL
		tmp->epfd_data[x] = NULL;
#endif
	}

	if ((tmp->timer = tris_timer_open())) {
		needqueue = 0;
		tmp->timingfd = tris_timer_fd(tmp->timer);
	} else {
		tmp->timingfd = -1;
	}

	if (needqueue) {
		if (pipe(tmp->alertpipe)) {
			tris_log(LOG_WARNING, "Channel allocation failed: Can't create alert pipe! Try increasing max file descriptors with ulimit -n\n");
alertpipe_failed:
			if (tmp->timer) {
				tris_timer_close(tmp->timer);
			}

			sched_context_destroy(tmp->sched);
			tris_string_field_free_memory(tmp);
			tris_free(tmp);
			return NULL;
		} else {
			flags = fcntl(tmp->alertpipe[0], F_GETFL);
			if (fcntl(tmp->alertpipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
				tris_log(LOG_WARNING, "Channel allocation failed: Unable to set alertpipe nonblocking! (%d: %s)\n", errno, strerror(errno));
				close(tmp->alertpipe[0]);
				close(tmp->alertpipe[1]);
				goto alertpipe_failed;
			}
			flags = fcntl(tmp->alertpipe[1], F_GETFL);
			if (fcntl(tmp->alertpipe[1], F_SETFL, flags | O_NONBLOCK) < 0) {
				tris_log(LOG_WARNING, "Channel allocation failed: Unable to set alertpipe nonblocking! (%d: %s)\n", errno, strerror(errno));
				close(tmp->alertpipe[0]);
				close(tmp->alertpipe[1]);
				goto alertpipe_failed;
			}
		}
	} else	/* Make sure we've got it done right if they don't */
		tmp->alertpipe[0] = tmp->alertpipe[1] = -1;

	/* Always watch the alertpipe */
	tris_channel_set_fd(tmp, TRIS_ALERT_FD, tmp->alertpipe[0]);
	/* And timing pipe */
	tris_channel_set_fd(tmp, TRIS_TIMING_FD, tmp->timingfd);
	tris_string_field_set(tmp, name, "**Unknown**");

	/* Initial state */
	tmp->_state = state;

	tmp->streamid = -1;
	
	tmp->fin = global_fin;
	tmp->fout = global_fout;

	if (tris_strlen_zero(tris_config_TRIS_SYSTEM_NAME)) {
		tris_string_field_build(tmp, uniqueid, "%li.%d", (long) time(NULL), 
				       tris_atomic_fetchadd_int(&uniqueint, 1));
	} else {
		tris_string_field_build(tmp, uniqueid, "%s-%li.%d", tris_config_TRIS_SYSTEM_NAME, 
				       (long) time(NULL), tris_atomic_fetchadd_int(&uniqueint, 1));
	}

	tmp->cid.cid_name = tris_strdup(cid_name);
	tmp->cid.cid_num = tris_strdup(cid_num);
	
	if (!tris_strlen_zero(name_fmt)) {
		/* Almost every channel is calling this function, and setting the name via the tris_string_field_build() call.
		 * And they all use slightly different formats for their name string.
		 * This means, to set the name here, we have to accept variable args, and call the string_field_build from here.
		 * This means, that the stringfields must have a routine that takes the va_lists directly, and 
		 * uses them to build the string, instead of forming the va_lists internally from the vararg ... list.
		 * This new function was written so this can be accomplished.
		 */
		tris_string_field_build_va(tmp, name, name_fmt, ap1, ap2);
	}

	/* Reminder for the future: under what conditions do we NOT want to track cdrs on channels? */

	/* These 4 variables need to be set up for the cdr_init() to work right */
	if (amaflag)
		tmp->amaflags = amaflag;
	else
		tmp->amaflags = tris_default_amaflags;
	
	if (!tris_strlen_zero(acctcode))
		tris_string_field_set(tmp, accountcode, acctcode);
	else
		tris_string_field_set(tmp, accountcode, tris_default_accountcode);
		
	if (!tris_strlen_zero(context))
		tris_copy_string(tmp->context, context, sizeof(tmp->context));
	else
		strcpy(tmp->context, "default");

	if (!tris_strlen_zero(exten))
		tris_copy_string(tmp->exten, exten, sizeof(tmp->exten));
	else
		strcpy(tmp->exten, "s");

	tmp->priority = 1;
		
	tmp->cdr = tris_cdr_alloc();
	tris_cdr_init(tmp->cdr, tmp);
	tris_cdr_start(tmp->cdr);
	
	headp = &tmp->varshead;
	TRIS_LIST_HEAD_INIT_NOLOCK(headp);
	
	tris_mutex_init(&tmp->lock_dont_use);
	
	TRIS_LIST_HEAD_INIT_NOLOCK(&tmp->datastores);
	
	tris_string_field_set(tmp, language, defaultlanguage);

	tmp->tech = &null_tech;

	tris_set_flag(tmp, TRIS_FLAG_IN_CHANNEL_LIST);

	TRIS_RWLIST_WRLOCK(&channels);
	TRIS_RWLIST_INSERT_HEAD(&channels, tmp, chan_list);
	TRIS_RWLIST_UNLOCK(&channels);

	/*\!note
	 * and now, since the channel structure is built, and has its name, let's
	 * call the manager event generator with this Newchannel event. This is the
	 * proper and correct place to make this call, but you sure do have to pass
	 * a lot of data into this func to do it here!
	 */
	if (!tris_strlen_zero(name_fmt)) {
		manager_event(EVENT_FLAG_CALL, "Newchannel",
			"Channel: %s\r\n"
			"ChannelState: %d\r\n"
			"ChannelStateDesc: %s\r\n"
			"CallerIDNum: %s\r\n"
			"CallerIDName: %s\r\n"
			"AccountCode: %s\r\n"
			"Exten: %s\r\n"
			"Context: %s\r\n"
			"Uniqueid: %s\r\n",
			tmp->name, 
			state, 
			tris_state2str(state),
			S_OR(cid_num, ""),
			S_OR(cid_name, ""),
			tmp->accountcode,
			S_OR(exten, ""),
			S_OR(context, ""),
			tmp->uniqueid);
	}

	return tmp;
}

struct tris_channel *__tris_channel_alloc(int needqueue, int state, const char *cid_num,
					const char *cid_name, const char *acctcode,
					const char *exten, const char *context,
					const int amaflag, const char *file, int line,
					const char *function, const char *name_fmt, ...)
{
	va_list ap1, ap2;
	struct tris_channel *result;

	va_start(ap1, name_fmt);
	va_start(ap2, name_fmt);
	result = __tris_channel_alloc_ap(needqueue, state, cid_num, cid_name, acctcode, exten, context,
					amaflag, file, line, function, name_fmt, ap1, ap2);
	va_end(ap1);
	va_end(ap2);

	return result;
}

static int __tris_queue_frame(struct tris_channel *chan, struct tris_frame *fin, int head, struct tris_frame *after)
{
	struct tris_frame *f;
	struct tris_frame *cur;
	int blah = 1;
	unsigned int new_frames = 0;
	unsigned int new_voice_frames = 0;
	unsigned int queued_frames = 0;
	unsigned int queued_voice_frames = 0;
	TRIS_LIST_HEAD_NOLOCK(, tris_frame) frames;

	tris_channel_lock(chan);

	/* See if the last frame on the queue is a hangup, if so don't queue anything */
	if ((cur = TRIS_LIST_LAST(&chan->readq)) &&
	    (cur->frametype == TRIS_FRAME_CONTROL) &&
	    (cur->subclass == TRIS_CONTROL_HANGUP)) {
		tris_channel_unlock(chan);
		return 0;
	}

	/* Build copies of all the frames and count them */
	TRIS_LIST_HEAD_INIT_NOLOCK(&frames);
	for (cur = fin; cur; cur = TRIS_LIST_NEXT(cur, frame_list)) {
		if (!(f = tris_frdup(cur))) {
			tris_frfree(TRIS_LIST_FIRST(&frames));
			tris_channel_unlock(chan);
			return -1;
		}

		TRIS_LIST_INSERT_TAIL(&frames, f, frame_list);
		new_frames++;
		if (f->frametype == TRIS_FRAME_VOICE) {
			new_voice_frames++;
		}
	}

	/* Count how many frames exist on the queue */
	TRIS_LIST_TRAVERSE(&chan->readq, cur, frame_list) {
		queued_frames++;
		if (cur->frametype == TRIS_FRAME_VOICE) {
			queued_voice_frames++;
		}
	}

	if ((queued_frames + new_frames > 128 || queued_voice_frames + new_voice_frames > 96)) {
		int count = 0;
		tris_log(LOG_WARNING, "Exceptionally long %squeue length queuing to %s\n", queued_frames + new_frames > 128 ? "" : "voice ", chan->name);
		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&chan->readq, cur, frame_list) {
			/* Save the most recent frame */
			if (!TRIS_LIST_NEXT(cur, frame_list)) {
				break;
			} else if (cur->frametype == TRIS_FRAME_VOICE || cur->frametype == TRIS_FRAME_VIDEO || cur->frametype == TRIS_FRAME_NULL) {
				if (++count > 64) {
					break;
				}
				TRIS_LIST_REMOVE_CURRENT(frame_list);
				tris_frfree(cur);
			}
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
	}

	if (after) {
		TRIS_LIST_INSERT_LIST_AFTER(&chan->readq, &frames, after, frame_list);
	} else {
		if (head) {
			TRIS_LIST_APPEND_LIST(&frames, &chan->readq, frame_list);
			TRIS_LIST_HEAD_INIT_NOLOCK(&chan->readq);
		}
		TRIS_LIST_APPEND_LIST(&chan->readq, &frames, frame_list);
	}

	if (chan->alertpipe[1] > -1) {
		if (write(chan->alertpipe[1], &blah, new_frames * sizeof(blah)) != (new_frames * sizeof(blah))) {
			tris_log(LOG_WARNING, "Unable to write to alert pipe on %s (qlen = %d): %s!\n",
				chan->name, queued_frames, strerror(errno));
		}
	} else if (chan->timingfd > -1) {
		tris_timer_enable_continuous(chan->timer);
	} else if (tris_test_flag(chan, TRIS_FLAG_BLOCKING)) {
		pthread_kill(chan->blocker, SIGURG);
	}

	tris_channel_unlock(chan);

	return 0;
}

int tris_queue_frame(struct tris_channel *chan, struct tris_frame *fin)
{
	if (fin)
		return __tris_queue_frame(chan, fin, 0, NULL);
	else
		return 0;
}

int tris_queue_frame_head(struct tris_channel *chan, struct tris_frame *fin)
{
	return __tris_queue_frame(chan, fin, 1, NULL);
}

/*! \brief Queue a hangup frame for channel */
int tris_queue_hangup(struct tris_channel *chan)
{
	struct tris_frame f = { TRIS_FRAME_CONTROL, TRIS_CONTROL_HANGUP };
	/* Yeah, let's not change a lock-critical value without locking */
	if (!tris_channel_trylock(chan)) {
		chan->_softhangup |= TRIS_SOFTHANGUP_DEV;
		tris_channel_unlock(chan);
	}
	return tris_queue_frame(chan, &f);
}

/*! \brief Queue a hangup frame for channel */
int tris_queue_hangup_with_cause(struct tris_channel *chan, int cause)
{
	struct tris_frame f = { TRIS_FRAME_CONTROL, TRIS_CONTROL_HANGUP };

	if (cause >= 0)
		f.data.uint32 = cause;

	/* Yeah, let's not change a lock-critical value without locking */
	if (!tris_channel_trylock(chan)) {
		chan->_softhangup |= TRIS_SOFTHANGUP_DEV;
		if (cause < 0)
			f.data.uint32 = chan->hangupcause;

		tris_channel_unlock(chan);
	}

	return tris_queue_frame(chan, &f);
}

/*! \brief Queue a control frame */
int tris_queue_control(struct tris_channel *chan, enum tris_control_frame_type control)
{
	struct tris_frame f = { TRIS_FRAME_CONTROL, };

	f.subclass = control;

	return tris_queue_frame(chan, &f);
}

/*! \brief Queue a control frame with payload */
int tris_queue_control_data(struct tris_channel *chan, enum tris_control_frame_type control,
			   const void *data, size_t datalen)
{
	struct tris_frame f = { TRIS_FRAME_CONTROL, };

	f.subclass = control;
	f.data.ptr = (void *) data;
	f.datalen = datalen;

	return tris_queue_frame(chan, &f);
}

/*! \brief Set defer DTMF flag on channel */
int tris_channel_defer_dtmf(struct tris_channel *chan)
{
	int pre = 0;

	if (chan) {
		pre = tris_test_flag(chan, TRIS_FLAG_DEFER_DTMF);
		tris_set_flag(chan, TRIS_FLAG_DEFER_DTMF);
	}
	return pre;
}

/*! \brief Unset defer DTMF flag on channel */
void tris_channel_undefer_dtmf(struct tris_channel *chan)
{
	if (chan)
		tris_clear_flag(chan, TRIS_FLAG_DEFER_DTMF);
}

/*!
 * \brief Helper function to find channels.
 *
 * It supports these modes:
 *
 * prev != NULL : get channel next in list after prev
 * name != NULL : get channel with matching name
 * name != NULL && namelen != 0 : get channel whose name starts with prefix
 * exten != NULL : get channel whose exten or macroexten matches
 * context != NULL && exten != NULL : get channel whose context or macrocontext
 *
 * It returns with the channel's lock held. If getting the individual lock fails,
 * unlock and retry quickly up to 10 times, then give up.
 *
 * \note XXX Note that this code has cost O(N) because of the need to verify
 * that the object is still on the global list.
 *
 * \note XXX also note that accessing fields (e.g. c->name in tris_log())
 * can only be done with the lock held or someone could delete the
 * object while we work on it. This causes some ugliness in the code.
 * Note that removing the first tris_log() may be harmful, as it would
 * shorten the retry period and possibly cause failures.
 * We should definitely go for a better scheme that is deadlock-free.
 */
static struct tris_channel *channel_find_locked(const struct tris_channel *prev,
					       const char *name, const int namelen,
					       const char *context, const char *exten)
{
	const char *msg = prev ? "deadlock" : "initial deadlock";
	int retries;
	struct tris_channel *c;
	const struct tris_channel *_prev = prev;

	for (retries = 0; retries < 200; retries++) {
		int done;
		/* Reset prev on each retry.  See note below for the reason. */
		prev = _prev;
		TRIS_RWLIST_RDLOCK(&channels);
		TRIS_RWLIST_TRAVERSE(&channels, c, chan_list) {
			if (prev) {	/* look for last item, first, before any evaluation */
				if (c != prev)	/* not this one */
					continue;
				/* found, prepare to return c->next */
				if ((c = TRIS_RWLIST_NEXT(c, chan_list)) == NULL) break;
				/*!\note
				 * We're done searching through the list for the previous item.
				 * Any item after this point, we want to evaluate for a match.
				 * If we didn't set prev to NULL here, then we would only
				 * return matches for the first matching item (since the above
				 * "if (c != prev)" would not permit any other potential
				 * matches to reach the additional matching logic, below).
				 * Instead, it would just iterate until it once again found the
				 * original match, then iterate down to the end of the list and
				 * quit.
				 */
				prev = NULL;
			}
			if (name) { /* want match by name */
				if ((!namelen && strcasecmp(c->name, name) && strcmp(c->uniqueid, name)) ||
				    (namelen && strncasecmp(c->name, name, namelen)))
					continue;	/* name match failed */
			} else if (exten) {
				if (context && strcasecmp(c->context, context) &&
				    strcasecmp(c->macrocontext, context))
					continue;	/* context match failed */
				if (strcasecmp(c->exten, exten) &&
				    strcasecmp(c->macroexten, exten))
					continue;	/* exten match failed */
			}
			/* if we get here, c points to the desired record */
			break;
		}
		/* exit if chan not found or mutex acquired successfully */
		/* this is slightly unsafe, as we _should_ hold the lock to access c->name */
		done = c == NULL || tris_channel_trylock(c) == 0;
		if (!done) {
			tris_debug(1, "Avoiding %s for channel '%p'\n", msg, c);
			if (retries == 199) {
				/* We are about to fail due to a deadlock, so report this
				 * while we still have the list lock.
				 */
				tris_debug(1, "Failure, could not lock '%p' after %d retries!\n", c, retries);
				/* As we have deadlocked, we will skip this channel and
				 * see if there is another match.
				 * NOTE: No point doing this for a full-name match,
				 * as there can be no more matches.
				 */
				if (!(name && !namelen)) {
					_prev = c;
					retries = -1;
				}
			}
		}
		TRIS_RWLIST_UNLOCK(&channels);
		if (done)
			return c;
		/* If we reach this point we basically tried to lock a channel and failed. Instead of
		 * starting from the beginning of the list we can restore our saved pointer to the previous
		 * channel and start from there.
		 */
		prev = _prev;
		usleep(1);	/* give other threads a chance before retrying */
	}

	return NULL;
}

/*! \brief Browse channels in use */
struct tris_channel *tris_channel_walk_locked(const struct tris_channel *prev)
{
	return channel_find_locked(prev, NULL, 0, NULL, NULL);
}

/*! \brief Get channel by name and lock it */
struct tris_channel *tris_get_channel_by_name_locked(const char *name)
{
	return channel_find_locked(NULL, name, 0, NULL, NULL);
}

/*! \brief Get channel by name prefix and lock it */
struct tris_channel *tris_get_channel_by_name_prefix_locked(const char *name, const int namelen)
{
	return channel_find_locked(NULL, name, namelen, NULL, NULL);
}

/*! \brief Get next channel by name prefix and lock it */
struct tris_channel *tris_walk_channel_by_name_prefix_locked(const struct tris_channel *chan, const char *name,
							   const int namelen)
{
	return channel_find_locked(chan, name, namelen, NULL, NULL);
}

/*! \brief Get channel by exten (and optionally context) and lock it */
struct tris_channel *tris_get_channel_by_exten_locked(const char *exten, const char *context)
{
	return channel_find_locked(NULL, NULL, 0, context, exten);
}

/*! \brief Get next channel by exten (and optionally context) and lock it */
struct tris_channel *tris_walk_channel_by_exten_locked(const struct tris_channel *chan, const char *exten,
						     const char *context)
{
	return channel_find_locked(chan, NULL, 0, context, exten);
}

/*! \brief Search for a channel based on the passed channel matching callback (first match) and return it, locked */
void *tris_broad3channel_hangup_locked(const struct tris_channel *chan, const char *cid_num,
							 const char *exten)
{
	struct tris_channel *c = NULL;

	TRIS_RWLIST_RDLOCK(&channels);
	TRIS_RWLIST_TRAVERSE(&channels, c, chan_list) {
		tris_channel_lock(c);
		if(chan) {
			if ((c != chan) && !strcasecmp(c->exten, chan->exten)
				&& !strcasecmp(c->cid.cid_num, chan->cid.cid_num)) {
					tris_softhangup(c, TRIS_SOFTHANGUP_EXPLICIT);

			}
		} else if(exten && cid_num){
			if (!strcasecmp(c->exten, exten) && !strcasecmp(c->cid.cid_num, cid_num)) {
					tris_softhangup(c, TRIS_SOFTHANGUP_EXPLICIT);

			}
			if (!strcasecmp(c->exten, exten) && !strcasecmp(c->cid.cid_num, exten)) {
					tris_softhangup(c, TRIS_SOFTHANGUP_EXPLICIT);

			}
		}
		tris_channel_unlock(c);
	}
	TRIS_RWLIST_UNLOCK(&channels);
	return NULL;
}

int tris_broad3channel_search_locked(const char *exten, const char *cid)
{
	struct tris_channel *c = NULL;

	TRIS_RWLIST_RDLOCK(&channels);
	TRIS_RWLIST_TRAVERSE(&channels, c, chan_list) {
		tris_channel_lock(c);
		if (!strcasecmp(c->exten, exten)&& !strcasecmp(c->cid.cid_num, cid)) {
			tris_channel_unlock(c);
			TRIS_RWLIST_UNLOCK(&channels);
			return 1;

		}
		if (!strcasecmp(c->exten, exten)&& !strcasecmp(c->cid.cid_num, exten)) {
			tris_channel_unlock(c);
			TRIS_RWLIST_UNLOCK(&channels);
			return 1;

		}
		tris_channel_unlock(c);
	}
	TRIS_RWLIST_UNLOCK(&channels);
	
	return 0;

}

void tris_rakwonchannel_hangup(const struct tris_channel *chan)
{
	struct tris_channel *c = NULL;

	TRIS_RWLIST_RDLOCK(&channels);
	TRIS_RWLIST_TRAVERSE(&channels, c, chan_list) {
		tris_channel_lock(c);
		if ((c != chan) && !strcasecmp(c->tech->type, chan->tech->type) && !strcasecmp(c->cid.cid_num, chan->cid.cid_num)) {
				tris_softhangup(c, TRIS_SOFTHANGUP_EXPLICIT);
				tris_log(LOG_WARNING, "tris_rakwonchannel_hangup() --- hangup channel duplicated: '%p' \n", (void*)c);
		}
		tris_channel_unlock(c);
	}
	TRIS_RWLIST_UNLOCK(&channels);
}

/*! \brief Search for a channel based on the passed channel matching callback (first match) and return it, locked */
struct tris_channel *tris_channel_search_locked(int (*is_match)(struct tris_channel *, void *), void *data)
{
	struct tris_channel *c = NULL;

	TRIS_RWLIST_RDLOCK(&channels);
	TRIS_RWLIST_TRAVERSE(&channels, c, chan_list) {
		tris_channel_lock(c);
		if (is_match(c, data)) {
			break;
		}
		tris_channel_unlock(c);
	}
	TRIS_RWLIST_UNLOCK(&channels);

	return c;
}

/*! \brief Wait, look for hangups and condition arg */
int tris_safe_sleep_conditional(struct tris_channel *chan, int ms, int (*cond)(void*), void *data)
{
	struct tris_frame *f;
	struct tris_silence_generator *silgen = NULL;
	int res = 0;

	/* If no other generator is present, start silencegen while waiting */
	if (tris_opt_transmit_silence && !chan->generatordata) {
		silgen = tris_channel_start_silence_generator(chan);
	}

	while (ms > 0) {
		if (cond && ((*cond)(data) == 0)) {
			break;
		}
		ms = tris_waitfor(chan, ms);
		if (ms < 0) {
			res = -1;
			break;
		}
		if (ms > 0) {
			f = tris_read(chan);
			if (!f) {
				res = -1;
				break;
			}
			tris_frfree(f);
		}
	}

	/* stop silgen if present */
	if (silgen) {
		tris_channel_stop_silence_generator(chan, silgen);
	}

	return res;
}

/*! \brief Wait, look for hangups */
int tris_safe_sleep(struct tris_channel *chan, int ms)
{
	return tris_safe_sleep_conditional(chan, ms, NULL, NULL);
}

static void free_cid(struct tris_callerid *cid)
{
	if (cid->cid_dnid)
		tris_free(cid->cid_dnid);
	if (cid->cid_num)
		tris_free(cid->cid_num);	
	if (cid->cid_from_num)
		tris_free(cid->cid_from_num);
	if (cid->cid_name)
		tris_free(cid->cid_name);	
	if (cid->cid_ani)
		tris_free(cid->cid_ani);
	if (cid->cid_rdnis)
		tris_free(cid->cid_rdnis);
	cid->cid_dnid = cid->cid_num = cid->cid_from_num = cid->cid_name = cid->cid_ani = cid->cid_rdnis = NULL;
}

/*! \brief Free a channel structure */
void tris_channel_free(struct tris_channel *chan)
{
	int fd;
#ifdef HAVE_EPOLL
	int i;
#endif
	struct tris_var_t *vardata;
	struct tris_frame *f;
	struct varshead *headp;
	struct tris_datastore *datastore = NULL;
	char name[TRIS_CHANNEL_NAME], *dashptr;
	int inlist;
	
	headp=&chan->varshead;
	
	inlist = tris_test_flag(chan, TRIS_FLAG_IN_CHANNEL_LIST);
	if (inlist) {
		TRIS_RWLIST_WRLOCK(&channels);
		if (!TRIS_RWLIST_REMOVE(&channels, chan, chan_list)) {
			tris_debug(1, "Unable to find channel in list to free. Assuming it has already been done.\n");
		}
		/* Lock and unlock the channel just to be sure nobody has it locked still
		   due to a reference retrieved from the channel list. */
		tris_channel_lock(chan);
		tris_channel_unlock(chan);
	}

	/* Get rid of each of the data stores on the channel */
	tris_channel_lock(chan);
	while ((datastore = TRIS_LIST_REMOVE_HEAD(&chan->datastores, entry)))
		/* Free the data store */
		tris_datastore_free(datastore);
	tris_channel_unlock(chan);

	/* Lock and unlock the channel just to be sure nobody has it locked still
	   due to a reference that was stored in a datastore. (i.e. app_chanspy) */
	tris_channel_lock(chan);
	tris_channel_unlock(chan);

	if (chan->tech_pvt) {
		tris_log(LOG_WARNING, "Channel '%s' may not have been hung up properly\n", chan->name);
		tris_free(chan->tech_pvt);
	}

	if (chan->sched)
		sched_context_destroy(chan->sched);

	tris_copy_string(name, chan->name, sizeof(name));
	if ((dashptr = strrchr(name, '-'))) {
		*dashptr = '\0';
	}

	/* Stop monitoring */
	if (chan->monitor)
		chan->monitor->stop( chan, 0 );

	/* If there is native format music-on-hold state, free it */
	if (chan->music_state)
		tris_moh_cleanup(chan);

	/* Free translators */
	if (chan->readtrans)
		tris_translator_free_path(chan->readtrans);
	if (chan->writetrans)
		tris_translator_free_path(chan->writetrans);
	if (chan->pbx)
		tris_log(LOG_WARNING, "PBX may not have been terminated properly on '%s'\n", chan->name);
	free_cid(&chan->cid);
	/* Close pipes if appropriate */
	if ((fd = chan->alertpipe[0]) > -1)
		close(fd);
	if ((fd = chan->alertpipe[1]) > -1)
		close(fd);
	if (chan->timer) {
		tris_timer_close(chan->timer);
	}
#ifdef HAVE_EPOLL
	for (i = 0; i < TRIS_MAX_FDS; i++) {
		if (chan->epfd_data[i])
			free(chan->epfd_data[i]);
	}
	close(chan->epfd);
#endif
	while ((f = TRIS_LIST_REMOVE_HEAD(&chan->readq, frame_list)))
		tris_frfree(f);
	
	/* loop over the variables list, freeing all data and deleting list items */
	/* no need to lock the list, as the channel is already locked */
	
	while ((vardata = TRIS_LIST_REMOVE_HEAD(headp, entries)))
		tris_var_delete(vardata);

	tris_app_group_discard(chan);

	/* Destroy the jitterbuffer */
	tris_jb_destroy(chan);

	if (chan->cdr) {
		tris_cdr_discard(chan->cdr);
		chan->cdr = NULL;
	}

	if (chan->zone) {
		chan->zone = tris_tone_zone_unref(chan->zone);
	}

	tris_mutex_destroy(&chan->lock_dont_use);

	tris_string_field_free_memory(chan);
	tris_free(chan);
	if (inlist)
		TRIS_RWLIST_UNLOCK(&channels);

	/* Queue an unknown state, because, while we know that this particular
	 * instance is dead, we don't know the state of all other possible
	 * instances. */
	tris_devstate_changed_literal(TRIS_DEVICE_UNKNOWN, name);
}

struct tris_datastore *tris_channel_datastore_alloc(const struct tris_datastore_info *info, const char *uid)
{
	return tris_datastore_alloc(info, uid);
}

int tris_channel_datastore_free(struct tris_datastore *datastore)
{
	return tris_datastore_free(datastore);
}

int tris_channel_datastore_inherit(struct tris_channel *from, struct tris_channel *to)
{
	struct tris_datastore *datastore = NULL, *datastore2;

	TRIS_LIST_TRAVERSE(&from->datastores, datastore, entry) {
		if (datastore->inheritance > 0) {
			datastore2 = tris_datastore_alloc(datastore->info, datastore->uid);
			if (datastore2) {
				datastore2->data = datastore->info->duplicate ? datastore->info->duplicate(datastore->data) : NULL;
				datastore2->inheritance = datastore->inheritance == DATASTORE_INHERIT_FOREVER ? DATASTORE_INHERIT_FOREVER : datastore->inheritance - 1;
				TRIS_LIST_INSERT_TAIL(&to->datastores, datastore2, entry);
			}
		}
	}
	return 0;
}

int tris_channel_datastore_add(struct tris_channel *chan, struct tris_datastore *datastore)
{
	int res = 0;

	TRIS_LIST_INSERT_HEAD(&chan->datastores, datastore, entry);

	return res;
}

int tris_channel_datastore_remove(struct tris_channel *chan, struct tris_datastore *datastore)
{
	return TRIS_LIST_REMOVE(&chan->datastores, datastore, entry) ? 0 : -1;
}

struct tris_datastore *tris_channel_datastore_find(struct tris_channel *chan, const struct tris_datastore_info *info, const char *uid)
{
	struct tris_datastore *datastore = NULL;
	
	if (info == NULL)
		return NULL;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&chan->datastores, datastore, entry) {
		if (datastore->info != info) {
			continue;
		}

		if (uid == NULL) {
			/* matched by type only */
			break;
		}

		if ((datastore->uid != NULL) && !strcasecmp(uid, datastore->uid)) {
			/* Matched by type AND uid */
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	return datastore;
}

/*! Set the file descriptor on the channel */
void tris_channel_set_fd(struct tris_channel *chan, int which, int fd)
{
#ifdef HAVE_EPOLL
	struct epoll_event ev;
	struct tris_epoll_data *aed = NULL;

	if (chan->fds[which] > -1) {
		epoll_ctl(chan->epfd, EPOLL_CTL_DEL, chan->fds[which], &ev);
		aed = chan->epfd_data[which];
	}

	/* If this new fd is valid, add it to the epoll */
	if (fd > -1) {
		if (!aed && (!(aed = tris_calloc(1, sizeof(*aed)))))
			return;
		
		chan->epfd_data[which] = aed;
		aed->chan = chan;
		aed->which = which;
		
		ev.events = EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP;
		ev.data.ptr = aed;
		epoll_ctl(chan->epfd, EPOLL_CTL_ADD, fd, &ev);
	} else if (aed) {
		/* We don't have to keep around this epoll data structure now */
		free(aed);
		chan->epfd_data[which] = NULL;
	}
#endif
	chan->fds[which] = fd;
	return;
}

/*! Add a channel to an optimized waitfor */
void tris_poll_channel_add(struct tris_channel *chan0, struct tris_channel *chan1)
{
#ifdef HAVE_EPOLL
	struct epoll_event ev;
	int i = 0;

	if (chan0->epfd == -1)
		return;

	/* Iterate through the file descriptors on chan1, adding them to chan0 */
	for (i = 0; i < TRIS_MAX_FDS; i++) {
		if (chan1->fds[i] == -1)
			continue;
		ev.events = EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP;
		ev.data.ptr = chan1->epfd_data[i];
		epoll_ctl(chan0->epfd, EPOLL_CTL_ADD, chan1->fds[i], &ev);
	}

#endif
	return;
}

/*! Delete a channel from an optimized waitfor */
void tris_poll_channel_del(struct tris_channel *chan0, struct tris_channel *chan1)
{
#ifdef HAVE_EPOLL
	struct epoll_event ev;
	int i = 0;

	if (chan0->epfd == -1)
		return;

	for (i = 0; i < TRIS_MAX_FDS; i++) {
		if (chan1->fds[i] == -1)
			continue;
		epoll_ctl(chan0->epfd, EPOLL_CTL_DEL, chan1->fds[i], &ev);
	}

#endif
	return;
}

/*! \brief Softly hangup a channel, don't lock */
int tris_softhangup_nolock(struct tris_channel *chan, int cause)
{
	tris_debug(1, "Soft-Hanging up channel '%s'\n", chan->name);
	/* Inform channel driver that we need to be hung up, if it cares */
	chan->_softhangup |= cause;
	tris_queue_frame(chan, &tris_null_frame);
	/* Interrupt any poll call or such */
	if (tris_test_flag(chan, TRIS_FLAG_BLOCKING))
		pthread_kill(chan->blocker, SIGURG);
	return 0;
}

/*! \brief Softly hangup a channel, lock */
int tris_softhangup(struct tris_channel *chan, int cause)
{
	int res;

	tris_channel_lock(chan);
	res = tris_softhangup_nolock(chan, cause);
	tris_channel_unlock(chan);

	return res;
}

static void free_translation(struct tris_channel *clonechan)
{
	if (clonechan->writetrans)
		tris_translator_free_path(clonechan->writetrans);
	if (clonechan->readtrans)
		tris_translator_free_path(clonechan->readtrans);
	clonechan->writetrans = NULL;
	clonechan->readtrans = NULL;
	clonechan->rawwriteformat = clonechan->nativeformats;
	clonechan->rawreadformat = clonechan->nativeformats;
}

/*! \brief Hangup a channel */
int tris_hangup(struct tris_channel *chan)
{
	int res = 0;

	/* Don't actually hang up a channel that will masquerade as someone else, or
	   if someone is going to masquerade as us */
	tris_channel_lock(chan);

	if (chan->audiohooks) {
		tris_audiohook_detach_list(chan->audiohooks);
		chan->audiohooks = NULL;
	}

	tris_autoservice_stop(chan);

	if (chan->masq) {
		if (tris_do_masquerade(chan))
			tris_log(LOG_WARNING, "Failed to perform masquerade\n");
	}

	if (chan->masq) {
		tris_log(LOG_WARNING, "%s getting hung up, but someone is trying to masq into us?!?\n", chan->name);
		tris_channel_unlock(chan);
		return 0;
	}
	/* If this channel is one which will be masqueraded into something,
	   mark it as a zombie already, so we know to free it later */
	if (chan->masqr) {
		tris_set_flag(chan, TRIS_FLAG_ZOMBIE);
		tris_channel_unlock(chan);
		return 0;
	}
	tris_channel_unlock(chan);

	TRIS_RWLIST_WRLOCK(&channels);
	if (!TRIS_RWLIST_REMOVE(&channels, chan, chan_list)) {
		tris_log(LOG_ERROR, "Unable to find channel in list to free. Assuming it has already been done.\n");
	}
	tris_clear_flag(chan, TRIS_FLAG_IN_CHANNEL_LIST);
	TRIS_RWLIST_UNLOCK(&channels);

	tris_channel_lock(chan);
	free_translation(chan);
	/* Close audio stream */
	if (chan->stream) {
		tris_closestream(chan->stream);
		chan->stream = NULL;
	}
	/* Close video stream */
	if (chan->vstream) {
		tris_closestream(chan->vstream);
		chan->vstream = NULL;
	}
	if (chan->sched) {
		sched_context_destroy(chan->sched);
		chan->sched = NULL;
	}
	
	if (chan->generatordata)	/* Clear any tone stuff remaining */
		if (chan->generator && chan->generator->release)
			chan->generator->release(chan, chan->generatordata);
	chan->generatordata = NULL;
	chan->generator = NULL;
	if (tris_test_flag(chan, TRIS_FLAG_BLOCKING)) {
		tris_log(LOG_WARNING, "Hard hangup called by thread %ld on %s, while fd "
					"is blocked by thread %ld in procedure %s!  Expect a failure\n",
					(long)pthread_self(), chan->name, (long)chan->blocker, chan->blockproc);
		tris_assert(tris_test_flag(chan, TRIS_FLAG_BLOCKING) == 0);
	}
	if (!tris_test_flag(chan, TRIS_FLAG_ZOMBIE)) {
		tris_debug(1, "Hanging up channel '%s'\n", chan->name);
		if (chan->tech->hangup)
			res = chan->tech->hangup(chan);
	} else {
		tris_debug(1, "Hanging up zombie '%s'\n", chan->name);
	}
			
	tris_channel_unlock(chan);
	manager_event(EVENT_FLAG_CALL, "Hangup",
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"CallerIDNum: %s\r\n"
			"CallerIDName: %s\r\n"
			"Cause: %d\r\n"
			"Cause-txt: %s\r\n",
			chan->name,
			chan->uniqueid,
			S_OR(chan->cid.cid_num, "<unknown>"),
			S_OR(chan->cid.cid_name, "<unknown>"),
			chan->hangupcause,
			tris_cause2str(chan->hangupcause)
			);

	if (chan->cdr && !tris_test_flag(chan->cdr, TRIS_CDR_FLAG_BRIDGED) && 
		!tris_test_flag(chan->cdr, TRIS_CDR_FLAG_POST_DISABLED) && 
	    (chan->cdr->disposition != TRIS_CDR_NULL || tris_test_flag(chan->cdr, TRIS_CDR_FLAG_DIALED))) {
		tris_channel_lock(chan);
			
		tris_cdr_end(chan->cdr);
		tris_cdr_detach(chan->cdr);
		chan->cdr = NULL;
		tris_channel_unlock(chan);
	}
	
	tris_channel_free(chan);

	return res;
}

int tris_raw_answer(struct tris_channel *chan, int cdr_answer)
{
	int res = 0;

	tris_channel_lock(chan);

	/* You can't answer an outbound call */
	if (tris_test_flag(chan, TRIS_FLAG_OUTGOING)) {
		tris_channel_unlock(chan);
		return 0;
	}

	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(chan, TRIS_FLAG_ZOMBIE) || tris_check_hangup(chan)) {
		tris_channel_unlock(chan);
		return -1;
	}

	tris_channel_unlock(chan);

	switch (chan->_state) {
	case TRIS_STATE_RINGING:
	case TRIS_STATE_RING:
		tris_channel_lock(chan);
		if (chan->tech->answer) {
			res = chan->tech->answer(chan);
		}
		tris_setstate(chan, TRIS_STATE_UP);
		if (cdr_answer) {
			tris_cdr_answer(chan->cdr);
		}
		tris_channel_unlock(chan);
		break;
	case TRIS_STATE_UP:
		/* Calling tris_cdr_answer when it it has previously been called
		 * is essentially a no-op, so it is safe.
		 */
		if (cdr_answer) {
			tris_cdr_answer(chan->cdr);
		}
		break;
	default:
		break;
	}

	tris_indicate(chan, -1);
	chan->visible_indication = 0;

	return res;
}

int __tris_answer(struct tris_channel *chan, unsigned int delay, int cdr_answer)
{
	int res = 0;
	enum tris_channel_state old_state;

	old_state = chan->_state;
	if ((res = tris_raw_answer(chan, cdr_answer))) {
		return res;
	}

	switch (old_state) {
	case TRIS_STATE_RINGING:
	case TRIS_STATE_RING:
		/* wait for media to start flowing, but don't wait any longer
		 * than 'delay' or 500 milliseconds, whichever is longer
		 */
		do {
			TRIS_LIST_HEAD_NOLOCK(, tris_frame) frames;
			struct tris_frame *cur, *new;
			int ms = MAX(delay, 500);
			unsigned int done = 0;

			TRIS_LIST_HEAD_INIT_NOLOCK(&frames);

			for (;;) {
				ms = tris_waitfor(chan, ms);
				if (ms < 0) {
					tris_log(LOG_WARNING, "Error condition occurred when polling channel %s for a voice frame: %s\n", chan->name, strerror(errno));
					res = -1;
					break;
				}
				if (ms == 0) {
					tris_debug(2, "Didn't receive a media frame from %s within %d ms of answering. Continuing anyway\n", chan->name, MAX(delay, 500));
					break;
				}
				cur = tris_read(chan);
				if (!cur || ((cur->frametype == TRIS_FRAME_CONTROL) &&
					     (cur->subclass == TRIS_CONTROL_HANGUP))) {
					if (cur) {
						tris_frfree(cur);
					}
					res = -1;
					tris_debug(2, "Hangup of channel %s detected in answer routine\n", chan->name);
					break;
				}

				if ((new = tris_frisolate(cur)) != cur) {
					tris_frfree(cur);
				}

				TRIS_LIST_INSERT_HEAD(&frames, new, frame_list);

				/* if a specific delay period was requested, continue
				 * until that delay has passed. don't stop just because
				 * incoming media has arrived.
				 */
				if (delay) {
					continue;
				}

				switch (new->frametype) {
					/* all of these frametypes qualify as 'media' */
				case TRIS_FRAME_VOICE:
				case TRIS_FRAME_VIDEO:
				case TRIS_FRAME_TEXT:
				case TRIS_FRAME_DTMF_BEGIN:
				case TRIS_FRAME_DTMF_END:
				case TRIS_FRAME_IMAGE:
				case TRIS_FRAME_HTML:
				case TRIS_FRAME_MODEM:
				case TRIS_FRAME_FILE:
				case TRIS_FRAME_DESKTOP:
				case TRIS_FRAME_CHAT:
					done = 1;
					break;
				case TRIS_FRAME_CONTROL:
				case TRIS_FRAME_IAX:
				case TRIS_FRAME_NULL:
				case TRIS_FRAME_CNG:
					break;
				}

				if (done) {
					break;
				}
			}

			if (res == 0) {
				tris_channel_lock(chan);
				while ((cur = TRIS_LIST_REMOVE_HEAD(&frames, frame_list))) {
					tris_queue_frame_head(chan, cur);
					tris_frfree(cur);
				}
				tris_channel_unlock(chan);
			}
		} while (0);
		break;
	default:
		break;
	}

	return res;
}

int tris_answer(struct tris_channel *chan)
{
	return __tris_answer(chan, 0, 1);
}

void tris_deactivate_generator(struct tris_channel *chan)
{
	tris_channel_lock(chan);
	if (chan->generatordata) {
		if (chan->generator && chan->generator->release)
			chan->generator->release(chan, chan->generatordata);
		chan->generatordata = NULL;
		chan->generator = NULL;
		tris_channel_set_fd(chan, TRIS_GENERATOR_FD, -1);
		tris_clear_flag(chan, TRIS_FLAG_WRITE_INT);
		tris_settimeout(chan, 0, NULL, NULL);
	}
	tris_channel_unlock(chan);
}

static int generator_force(const void *data)
{
	/* Called if generator doesn't have data */
	void *tmp;
	int res;
	int (*generate)(struct tris_channel *chan, void *tmp, int datalen, int samples) = NULL;
	struct tris_channel *chan = (struct tris_channel *)data;

	tris_channel_lock(chan);
	tmp = chan->generatordata;
	chan->generatordata = NULL;
	if (chan->generator)
		generate = chan->generator->generate;
	tris_channel_unlock(chan);

	if (!tmp || !generate)
		return 0;

	res = generate(chan, tmp, 0, tris_format_rate(chan->writeformat & TRIS_FORMAT_AUDIO_MASK) / 50);

	chan->generatordata = tmp;

	if (res) {
		tris_debug(1, "Auto-deactivating generator\n");
		tris_deactivate_generator(chan);
	}

	return 0;
}

int tris_activate_generator(struct tris_channel *chan, struct tris_generator *gen, void *params)
{
	int res = 0;

	tris_channel_lock(chan);

	if (chan->generatordata) {
		if (chan->generator && chan->generator->release)
			chan->generator->release(chan, chan->generatordata);
		chan->generatordata = NULL;
	}

	if (gen->alloc && !(chan->generatordata = gen->alloc(chan, params))) {
		res = -1;
	}
	
	if (!res) {
		tris_settimeout(chan, 50, generator_force, chan);
		chan->generator = gen;
	}

	tris_channel_unlock(chan);
	tris_prod(chan);

	return res;
}

/*! \brief Wait for x amount of time on a file descriptor to have input.  */
int tris_waitfor_n_fd(int *fds, int n, int *ms, int *exception)
{
	int winner = -1;
	tris_waitfor_nandfds(NULL, 0, fds, n, exception, &winner, ms);
	return winner;
}

/*! \brief Wait for x amount of time on a file descriptor to have input.  */
#ifdef HAVE_EPOLL
static struct tris_channel *tris_waitfor_nandfds_classic(struct tris_channel **c, int n, int *fds, int nfds,
					int *exception, int *outfd, int *ms)
#else
struct tris_channel *tris_waitfor_nandfds(struct tris_channel **c, int n, int *fds, int nfds,
					int *exception, int *outfd, int *ms)
#endif
{
	struct timeval start = { 0 , 0 };
	struct pollfd *pfds = NULL;
	int res;
	long rms;
	int x, y, max;
	int sz;
	struct timeval now = { 0, 0 };
	struct timeval whentohangup = { 0, 0 }, diff;
	struct tris_channel *winner = NULL;
	struct fdmap {
		int chan;
		int fdno;
	} *fdmap = NULL;

	if ((sz = n * TRIS_MAX_FDS + nfds)) {
		pfds = alloca(sizeof(*pfds) * sz);
		fdmap = alloca(sizeof(*fdmap) * sz);
	}

	if (outfd)
		*outfd = -99999;
	if (exception)
		*exception = 0;

	if (!ms)
		return NULL;
	
	/* Perform any pending masquerades */
	for (x = 0; x < n; x++) {
		tris_channel_lock(c[x]);
		if (c[x]->masq && tris_do_masquerade(c[x])) {
			tris_log(LOG_WARNING, "Masquerade failed\n");
			*ms = -1;
			tris_channel_unlock(c[x]);
			return NULL;
		}
		if (!tris_tvzero(c[x]->whentohangup)) {
			if (tris_tvzero(whentohangup))
				now = tris_tvnow();
			diff = tris_tvsub(c[x]->whentohangup, now);
			if (diff.tv_sec < 0 || tris_tvzero(diff)) {
				/* Should already be hungup */
				c[x]->_softhangup |= TRIS_SOFTHANGUP_TIMEOUT;
				tris_channel_unlock(c[x]);
				return c[x];
			}
			if (tris_tvzero(whentohangup) || tris_tvcmp(diff, whentohangup) < 0)
				whentohangup = diff;
		}
		tris_channel_unlock(c[x]);
	}
	/* Wait full interval */
	rms = *ms;
	if (!tris_tvzero(whentohangup)) {
		rms = whentohangup.tv_sec * 1000 + whentohangup.tv_usec / 1000;              /* timeout in milliseconds */
		if (*ms >= 0 && *ms < rms)		/* original *ms still smaller */
			rms =  *ms;
	}
	/*
	 * Build the pollfd array, putting the channels' fds first,
	 * followed by individual fds. Order is important because
	 * individual fd's must have priority over channel fds.
	 */
	max = 0;
	for (x = 0; x < n; x++) {
		for (y = 0; y < TRIS_MAX_FDS; y++) {
			fdmap[max].fdno = y;  /* fd y is linked to this pfds */
			fdmap[max].chan = x;  /* channel x is linked to this pfds */
			max += tris_add_fd(&pfds[max], c[x]->fds[y]);
		}
		CHECK_BLOCKING(c[x]);
	}
	/* Add the individual fds */
	for (x = 0; x < nfds; x++) {
		fdmap[max].chan = -1;
		max += tris_add_fd(&pfds[max], fds[x]);
	}

	if (*ms > 0)
		start = tris_tvnow();
	
	if (sizeof(int) == 4) {	/* XXX fix timeout > 600000 on linux x86-32 */
		do {
			int kbrms = rms;
			if (kbrms > 600000)
				kbrms = 600000;
			res = tris_poll(pfds, max, kbrms);
			if (!res)
				rms -= kbrms;
		} while (!res && (rms > 0));
	} else {
		res = tris_poll(pfds, max, rms);
	}
	for (x = 0; x < n; x++)
		tris_clear_flag(c[x], TRIS_FLAG_BLOCKING);
	if (res < 0) { /* Simulate a timeout if we were interrupted */
		if (errno != EINTR)
			*ms = -1;
		return NULL;
	}
	if (!tris_tvzero(whentohangup)) {   /* if we have a timeout, check who expired */
		now = tris_tvnow();
		for (x = 0; x < n; x++) {
			if (!tris_tvzero(c[x]->whentohangup) && tris_tvcmp(c[x]->whentohangup, now) <= 0) {
				c[x]->_softhangup |= TRIS_SOFTHANGUP_TIMEOUT;
				if (winner == NULL)
					winner = c[x];
			}
		}
	}
	if (res == 0) { /* no fd ready, reset timeout and done */
		*ms = 0;	/* XXX use 0 since we may not have an exact timeout. */
		return winner;
	}
	/*
	 * Then check if any channel or fd has a pending event.
	 * Remember to check channels first and fds last, as they
	 * must have priority on setting 'winner'
	 */
	for (x = 0; x < max; x++) {
		res = pfds[x].revents;
		if (res == 0)
			continue;
		if (fdmap[x].chan >= 0) {	/* this is a channel */
			winner = c[fdmap[x].chan];	/* override previous winners */
			if (res & POLLPRI)
				tris_set_flag(winner, TRIS_FLAG_EXCEPTION);
			else
				tris_clear_flag(winner, TRIS_FLAG_EXCEPTION);
			winner->fdno = fdmap[x].fdno;
		} else {			/* this is an fd */
			if (outfd)
				*outfd = pfds[x].fd;
			if (exception)
				*exception = (res & POLLPRI) ? -1 : 0;
			winner = NULL;
		}
	}
	if (*ms > 0) {
		*ms -= tris_tvdiff_ms(tris_tvnow(), start);
		if (*ms < 0)
			*ms = 0;
	}
	return winner;
}

#ifdef HAVE_EPOLL
static struct tris_channel *tris_waitfor_nandfds_simple(struct tris_channel *chan, int *ms)
{
	struct timeval start = { 0 , 0 };
	int res = 0;
	struct epoll_event ev[1];
	long diff, rms = *ms;
	struct tris_channel *winner = NULL;
	struct tris_epoll_data *aed = NULL;

	tris_channel_lock(chan);

	/* See if this channel needs to be masqueraded */
	if (chan->masq && tris_do_masquerade(chan)) {
		tris_log(LOG_WARNING, "Failed to perform masquerade on %s\n", chan->name);
		*ms = -1;
		tris_channel_unlock(chan);
		return NULL;
	}

	/* Figure out their timeout */
	if (!tris_tvzero(chan->whentohangup)) {
		if ((diff = tris_tvdiff_ms(chan->whentohangup, tris_tvnow())) < 0) {
			/* They should already be hungup! */
			chan->_softhangup |= TRIS_SOFTHANGUP_TIMEOUT;
			tris_channel_unlock(chan);
			return NULL;
		}
		/* If this value is smaller then the current one... make it priority */
		if (rms > diff)
			rms = diff;
	}

	tris_channel_unlock(chan);

	/* Time to make this channel block... */
	CHECK_BLOCKING(chan);

	if (*ms > 0)
		start = tris_tvnow();

	/* We don't have to add any file descriptors... they are already added, we just have to wait! */
	res = epoll_wait(chan->epfd, ev, 1, rms);

	/* Stop blocking */
	tris_clear_flag(chan, TRIS_FLAG_BLOCKING);

	/* Simulate a timeout if we were interrupted */
	if (res < 0) {
		if (errno != EINTR)
			*ms = -1;
		return NULL;
	}

	/* If this channel has a timeout see if it expired */
	if (!tris_tvzero(chan->whentohangup)) {
		if (tris_tvdiff_ms(tris_tvnow(), chan->whentohangup) >= 0) {
			chan->_softhangup |= TRIS_SOFTHANGUP_TIMEOUT;
			winner = chan;
		}
	}

	/* No fd ready, reset timeout and be done for now */
	if (!res) {
		*ms = 0;
		return winner;
	}

	/* See what events are pending */
	aed = ev[0].data.ptr;
	chan->fdno = aed->which;
	if (ev[0].events & EPOLLPRI)
		tris_set_flag(chan, TRIS_FLAG_EXCEPTION);
	else
		tris_clear_flag(chan, TRIS_FLAG_EXCEPTION);

	if (*ms > 0) {
		*ms -= tris_tvdiff_ms(tris_tvnow(), start);
		if (*ms < 0)
			*ms = 0;
	}

	return chan;
}

static struct tris_channel *tris_waitfor_nandfds_complex(struct tris_channel **c, int n, int *ms)
{
	struct timeval start = { 0 , 0 };
	int res = 0, i;
	struct epoll_event ev[25] = { { 0, } };
	struct timeval now = { 0, 0 };
	long whentohangup = 0, diff = 0, rms = *ms;
	struct tris_channel *winner = NULL;

	for (i = 0; i < n; i++) {
		tris_channel_lock(c[i]);
		if (c[i]->masq && tris_do_masquerade(c[i])) {
			tris_log(LOG_WARNING, "Masquerade failed\n");
			*ms = -1;
			tris_channel_unlock(c[i]);
			return NULL;
		}
		if (!tris_tvzero(c[i]->whentohangup)) {
			if (whentohangup == 0)
				now = tris_tvnow();
			if ((diff = tris_tvdiff_ms(c[i]->whentohangup, now)) < 0) {
				c[i]->_softhangup |= TRIS_SOFTHANGUP_TIMEOUT;
				tris_channel_unlock(c[i]);
				return c[i];
			}
			if (!whentohangup || whentohangup > diff)
				whentohangup = diff;
		}
		tris_channel_unlock(c[i]);
		CHECK_BLOCKING(c[i]);
	}

	rms = *ms;
	if (whentohangup) {
		rms = whentohangup;
		if (*ms >= 0 && *ms < rms)
			rms = *ms;
	}

	if (*ms > 0)
		start = tris_tvnow();

	res = epoll_wait(c[0]->epfd, ev, 25, rms);

	for (i = 0; i < n; i++)
		tris_clear_flag(c[i], TRIS_FLAG_BLOCKING);

	if (res < 0) {
		if (errno != EINTR)
			*ms = -1;
		return NULL;
	}

	if (whentohangup) {
		now = tris_tvnow();
		for (i = 0; i < n; i++) {
			if (!tris_tvzero(c[i]->whentohangup) && tris_tvdiff_ms(now, c[i]->whentohangup) >= 0) {
				c[i]->_softhangup |= TRIS_SOFTHANGUP_TIMEOUT;
				if (!winner)
					winner = c[i];
			}
		}
	}

	if (!res) {
		*ms = 0;
		return winner;
	}

	for (i = 0; i < res; i++) {
		struct tris_epoll_data *aed = ev[i].data.ptr;

		if (!ev[i].events || !aed)
			continue;

		winner = aed->chan;
		if (ev[i].events & EPOLLPRI)
			tris_set_flag(winner, TRIS_FLAG_EXCEPTION);
		else
			tris_clear_flag(winner, TRIS_FLAG_EXCEPTION);
		winner->fdno = aed->which;
	}

	if (*ms > 0) {
		*ms -= tris_tvdiff_ms(tris_tvnow(), start);
		if (*ms < 0)
			*ms = 0;
	}

	return winner;
}

struct tris_channel *tris_waitfor_nandfds(struct tris_channel **c, int n, int *fds, int nfds,
					int *exception, int *outfd, int *ms)
{
	/* Clear all provided values in one place. */
	if (outfd)
		*outfd = -99999;
	if (exception)
		*exception = 0;

	if (!ms)
		return NULL;
	/* If no epoll file descriptor is available resort to classic nandfds */
	if (!n || nfds || c[0]->epfd == -1)
		return tris_waitfor_nandfds_classic(c, n, fds, nfds, exception, outfd, ms);
	else if (!nfds && n == 1)
		return tris_waitfor_nandfds_simple(c[0], ms);
	else
		return tris_waitfor_nandfds_complex(c, n, ms);
}
#endif

struct tris_channel *tris_waitfor_n(struct tris_channel **c, int n, int *ms)
{
	return tris_waitfor_nandfds(c, n, NULL, 0, NULL, NULL, ms);
}

int tris_waitfor(struct tris_channel *c, int ms)
{
	int oldms = ms;	/* -1 if no timeout */

	tris_waitfor_nandfds(&c, 1, NULL, 0, NULL, NULL, &ms);
	if ((ms < 0) && (oldms < 0))
		ms = 0;
	return ms;
}

/* XXX never to be called with ms = -1 */
int tris_waitfordigit(struct tris_channel *c, int ms)
{
	return tris_waitfordigit_full(c, ms, -1, -1);
}

int tris_settimeout(struct tris_channel *c, unsigned int rate, int (*func)(const void *data), void *data)
{
	int res;
	unsigned int real_rate = rate, max_rate;

	tris_channel_lock(c);

	if (c->timingfd == -1) {
		tris_channel_unlock(c);
		return -1;
	}

	if (!func) {
		rate = 0;
		data = NULL;
	}

	if (rate && rate > (max_rate = tris_timer_get_max_rate(c->timer))) {
		real_rate = max_rate;
	}

	tris_debug(1, "Scheduling timer at (%u requested / %u actual) timer ticks per second\n", rate, real_rate);

	res = tris_timer_set_rate(c->timer, real_rate);

	c->timingfunc = func;
	c->timingdata = data;

	tris_channel_unlock(c);

	return res;
}

int tris_waitfordigit_full(struct tris_channel *c, int ms, int audiofd, int cmdfd)
{
	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(c, TRIS_FLAG_ZOMBIE) || tris_check_hangup(c))
		return -1;

	/* Only look for the end of DTMF, don't bother with the beginning and don't emulate things */
	tris_set_flag(c, TRIS_FLAG_END_DTMF_ONLY);

	/* Wait for a digit, no more than ms milliseconds total. */
	
	while (ms) {
		struct tris_channel *rchan;
		int outfd=-1;

		errno = 0;
		rchan = tris_waitfor_nandfds(&c, 1, &cmdfd, (cmdfd > -1) ? 1 : 0, NULL, &outfd, &ms);
		
		if (!rchan && outfd < 0 && ms) {
			if (errno == 0 || errno == EINTR)
				continue;
			tris_log(LOG_WARNING, "Wait failed (%s)\n", strerror(errno));
			tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
			return -1;
		} else if (outfd > -1) {
			/* The FD we were watching has something waiting */
			tris_log(LOG_WARNING, "The FD we were waiting for has something waiting. Waitfordigit returning numeric 1\n");
			tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
			return 1;
		} else if (rchan) {
			int res;
			struct tris_frame *f = tris_read(c);
			if (!f)
				return -1;

			switch (f->frametype) {
			case TRIS_FRAME_DTMF_BEGIN:
				break;
			case TRIS_FRAME_DTMF_END:
				res = f->subclass;
				tris_frfree(f);
				tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
				return res;
			case TRIS_FRAME_CONTROL:
				switch (f->subclass) {
				case TRIS_CONTROL_HANGUP:
					tris_frfree(f);
					tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
					return -1;
				case TRIS_CONTROL_RINGING:
				case TRIS_CONTROL_ANSWER:
				case TRIS_CONTROL_SRCUPDATE:
				case TRIS_CONTROL_SRCCHANGE:
					/* Unimportant */
					break;
				default:
					tris_log(LOG_WARNING, "Unexpected control subclass '%d'\n", f->subclass);
					break;
				}
				break;
			case TRIS_FRAME_VOICE:
				/* Write audio if appropriate */
				if (audiofd > -1) {
					if (write(audiofd, f->data.ptr, f->datalen) < 0) {
						tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
					}
				}
			default:
				/* Ignore */
				break;
			}
			tris_frfree(f);
		}
	}

	tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);

	return 0; /* Time is up */
}

static void send_dtmf_event(const struct tris_channel *chan, const char *direction, const char digit, const char *begin, const char *end)
{
	manager_event(EVENT_FLAG_DTMF,
			"DTMF",
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"Digit: %c\r\n"
			"Direction: %s\r\n"
			"Begin: %s\r\n"
			"End: %s\r\n",
			chan->name, chan->uniqueid, digit, direction, begin, end);
}

static void tris_read_generator_actions(struct tris_channel *chan, struct tris_frame *f)
{
	if (chan->generator && chan->generator->generate && chan->generatordata &&  !tris_internal_timing_enabled(chan)) {
		void *tmp = chan->generatordata;
		int (*generate)(struct tris_channel *chan, void *tmp, int datalen, int samples) = chan->generator->generate;
		int res;
		int samples;

		if (chan->timingfunc) {
			tris_debug(1, "Generator got voice, switching to phase locked mode\n");
			tris_settimeout(chan, 0, NULL, NULL);
		}

		chan->generatordata = NULL;     /* reset, to let writes go through */

		if (f->subclass != chan->writeformat) {
			float factor;
			factor = ((float) tris_format_rate(chan->writeformat)) / ((float) tris_format_rate(f->subclass));
			samples = (int) ( ((float) f->samples) * factor );
		} else {
			samples = f->samples;
		}
		
		/* This unlock is here based on two assumptions that hold true at this point in the
		 * code. 1) this function is only called from within __tris_read() and 2) all generators
		 * call tris_write() in their generate callback.
		 *
		 * The reason this is added is so that when tris_write is called, the lock that occurs 
		 * there will not recursively lock the channel. Doing this will cause intended deadlock 
		 * avoidance not to work in deeper functions
		 */
		tris_channel_unlock(chan);
		res = generate(chan, tmp, f->datalen, samples);
		tris_channel_lock(chan);
		chan->generatordata = tmp;
		if (res) {
			tris_debug(1, "Auto-deactivating generator\n");
			tris_deactivate_generator(chan);
		}

	} else if (f->frametype == TRIS_FRAME_CNG) {
		if (chan->generator && !chan->timingfunc && (chan->timingfd > -1)) {
			tris_debug(1, "Generator got CNG, switching to timed mode\n");
			tris_settimeout(chan, 50, generator_force, chan);
		}
	}
}

static inline void queue_dtmf_readq(struct tris_channel *chan, struct tris_frame *f)
{
	struct tris_frame *fr = &chan->dtmff;

	fr->frametype = TRIS_FRAME_DTMF_END;
	fr->subclass = f->subclass;
	fr->len = f->len;

	/* The only time this function will be called is for a frame that just came
	 * out of the channel driver.  So, we want to stick it on the tail of the
	 * readq. */

	tris_queue_frame(chan, fr);
}

/*!
 * \brief Determine whether or not we should ignore DTMF in the readq
 */
static inline int should_skip_dtmf(struct tris_channel *chan)
{
	if (tris_test_flag(chan, TRIS_FLAG_DEFER_DTMF | TRIS_FLAG_EMULATE_DTMF)) {
		/* We're in the middle of emulating a digit, or DTMF has been
		 * explicitly deferred.  Skip this digit, then. */
		return 1;
	}
			
	if (!tris_tvzero(chan->dtmf_tv) && 
			tris_tvdiff_ms(tris_tvnow(), chan->dtmf_tv) < TRIS_MIN_DTMF_GAP) {
		/* We're not in the middle of a digit, but it hasn't been long enough
		 * since the last digit, so we'll have to skip DTMF for now. */
		return 1;
	}

	return 0;
}

/*!
 * \brief calculates the number of samples to jump forward with in a monitor stream.
 
 * \note When using tris_seekstream() with the read and write streams of a monitor,
 * the number of samples to seek forward must be of the same sample rate as the stream
 * or else the jump will not be calculated correctly.
 *
 * \retval number of samples to seek forward after rate conversion.
 */
static inline int calc_monitor_jump(int samples, int sample_rate, int seek_rate)
{
	int diff = sample_rate - seek_rate;

	if (diff > 0) {
		samples = samples / (float) (sample_rate / seek_rate);
	} else if (diff < 0) {
		samples = samples * (float) (seek_rate / sample_rate);
	}

	return samples;
}

static struct tris_frame *__tris_read(struct tris_channel *chan, int dropaudio)
{
	struct tris_frame *f = NULL;	/* the return value */
	int blah;
	int prestate;
	int count = 0, cause = 0;

	/* this function is very long so make sure there is only one return
	 * point at the end (there are only two exceptions to this).
	 */
	while(tris_channel_trylock(chan)) {
		if(count++ > 10) 
			/*cannot goto done since the channel is not locked*/
			return &tris_null_frame;
		usleep(1);
	}

	if (chan->masq) {
		if (tris_do_masquerade(chan))
			tris_log(LOG_WARNING, "Failed to perform masquerade\n");
		else
			f =  &tris_null_frame;
		goto done;
	}

	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(chan, TRIS_FLAG_ZOMBIE) || tris_check_hangup(chan)) {
		if (chan->generator)
			tris_deactivate_generator(chan);
		goto done;
	}

#ifdef TRIS_DEVMODE
	/* 
	 * The tris_waitfor() code records which of the channel's file descriptors reported that
	 * data is available.  In theory, tris_read() should only be called after tris_waitfor()
	 * reports that a channel has data available for reading.  However, there still may be
	 * some edge cases throughout the code where tris_read() is called improperly.  This can
	 * potentially cause problems, so if this is a developer build, make a lot of noise if
	 * this happens so that it can be addressed. 
	 */
	if (chan->fdno == -1) {
		tris_log(LOG_ERROR, "tris_read() called with no recorded file descriptor.\n");
	}
#endif

	prestate = chan->_state;

	/* Read and ignore anything on the alertpipe, but read only
	   one sizeof(blah) per frame that we send from it */
	if (chan->alertpipe[0] > -1) {
		int flags = fcntl(chan->alertpipe[0], F_GETFL);
		/* For some odd reason, the alertpipe occasionally loses nonblocking status,
		 * which immediately causes a deadlock scenario.  Detect and prevent this. */
		if ((flags & O_NONBLOCK) == 0) {
			tris_log(LOG_ERROR, "Alertpipe on channel %s lost O_NONBLOCK?!!\n", chan->name);
			if (fcntl(chan->alertpipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
				tris_log(LOG_WARNING, "Unable to set alertpipe nonblocking! (%d: %s)\n", errno, strerror(errno));
				f = &tris_null_frame;
				goto done;
			}
		}
		if (read(chan->alertpipe[0], &blah, sizeof(blah)) < 0) {
			if (errno != EINTR && errno != EAGAIN)
				tris_log(LOG_WARNING, "read() failed: %s\n", strerror(errno));
		}
	}

	if (chan->timingfd > -1 && chan->fdno == TRIS_TIMING_FD) {
		enum tris_timer_event res;

		tris_clear_flag(chan, TRIS_FLAG_EXCEPTION);

		res = tris_timer_get_event(chan->timer);

		switch (res) {
		case TRIS_TIMING_EVENT_EXPIRED:
			tris_timer_ack(chan->timer, 1);

			if (chan->timingfunc) {
				/* save a copy of func/data before unlocking the channel */
				int (*func)(const void *) = chan->timingfunc;
				void *data = chan->timingdata;
				chan->fdno = -1;
				tris_channel_unlock(chan);
				func(data);
			} else {
				tris_timer_set_rate(chan->timer, 0);
				chan->fdno = -1;
				tris_channel_unlock(chan);
			}

			/* cannot 'goto done' because the channel is already unlocked */
			return &tris_null_frame;

		case TRIS_TIMING_EVENT_CONTINUOUS:
			if (TRIS_LIST_EMPTY(&chan->readq) || 
				!TRIS_LIST_NEXT(TRIS_LIST_FIRST(&chan->readq), frame_list)) {
				tris_timer_disable_continuous(chan->timer);
			}
			break;
		}

	} else if (chan->fds[TRIS_GENERATOR_FD] > -1 && chan->fdno == TRIS_GENERATOR_FD) {
		/* if the TRIS_GENERATOR_FD is set, call the generator with args
		 * set to -1 so it can do whatever it needs to.
		 */
		void *tmp = chan->generatordata;
		chan->generatordata = NULL;     /* reset to let tris_write get through */
		chan->generator->generate(chan, tmp, -1, -1);
		chan->generatordata = tmp;
		f = &tris_null_frame;
		chan->fdno = -1;
		goto done;
	}

	/* Check for pending read queue */
	if (!TRIS_LIST_EMPTY(&chan->readq)) {
		int skip_dtmf = should_skip_dtmf(chan);

		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&chan->readq, f, frame_list) {
			/* We have to be picky about which frame we pull off of the readq because
			 * there are cases where we want to leave DTMF frames on the queue until
			 * some later time. */

			if ( (f->frametype == TRIS_FRAME_DTMF_BEGIN || f->frametype == TRIS_FRAME_DTMF_END) && skip_dtmf) {
				continue;
			}

			TRIS_LIST_REMOVE_CURRENT(frame_list);
			break;
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
		
		if (!f) {
			/* There were no acceptable frames on the readq. */
			f = &tris_null_frame;
			if (chan->alertpipe[0] > -1) {
				int poke = 0;
				/* Restore the state of the alertpipe since we aren't ready for any
				 * of the frames in the readq. */
				if (write(chan->alertpipe[1], &poke, sizeof(poke)) != sizeof(poke)) {
					tris_log(LOG_ERROR, "Failed to write to alertpipe: %s\n", strerror(errno));
				}
			}
		}

		/* Interpret hangup and return NULL */
		/* XXX why not the same for frames from the channel ? */
		if (f->frametype == TRIS_FRAME_CONTROL && f->subclass == TRIS_CONTROL_HANGUP) {
			cause = f->data.uint32;
			tris_frfree(f);
			f = NULL;
		}
	} else {
		chan->blocker = pthread_self();
		if (tris_test_flag(chan, TRIS_FLAG_EXCEPTION)) {
			if (chan->tech->exception)
				f = chan->tech->exception(chan);
			else {
				tris_log(LOG_WARNING, "Exception flag set on '%s', but no exception handler\n", chan->name);
				f = &tris_null_frame;
			}
			/* Clear the exception flag */
			tris_clear_flag(chan, TRIS_FLAG_EXCEPTION);
		} else if (chan->tech->read)
			f = chan->tech->read(chan);
		else
			tris_log(LOG_WARNING, "No read routine on channel %s\n", chan->name);
	}

	/*
	 * Reset the recorded file descriptor that triggered this read so that we can
	 * easily detect when tris_read() is called without properly using tris_waitfor().
	 */
	chan->fdno = -1;

	if (f) {
		struct tris_frame *readq_tail = TRIS_LIST_LAST(&chan->readq);

		/* if the channel driver returned more than one frame, stuff the excess
		   into the readq for the next tris_read call
		*/
		if (TRIS_LIST_NEXT(f, frame_list)) {
			tris_queue_frame(chan, TRIS_LIST_NEXT(f, frame_list));
			tris_frfree(TRIS_LIST_NEXT(f, frame_list));
			TRIS_LIST_NEXT(f, frame_list) = NULL;
		}

		switch (f->frametype) {
		case TRIS_FRAME_CONTROL:
			if (f->subclass == TRIS_CONTROL_ANSWER) {
/*				if (!tris_test_flag(chan, TRIS_FLAG_OUTGOING)) {
					tris_debug(1, "Ignoring answer on an inbound call!\n");
					tris_frfree(f);
					f = &tris_null_frame;
				} else*/ if (prestate == TRIS_STATE_UP) {
					tris_debug(1, "Dropping duplicate answer!\n");
					tris_frfree(f);
					f = &tris_null_frame;
				} else {
					/* Answer the CDR */
					tris_setstate(chan, TRIS_STATE_UP);
					/* removed a call to tris_cdr_answer(chan->cdr) from here. */
				}
			}
			break;
		case TRIS_FRAME_DTMF_END:
			send_dtmf_event(chan, "Received", f->subclass, "No", "Yes");
			tris_log(LOG_DTMF, "DTMF end '%c' received on %s, duration %ld ms\n", f->subclass, chan->name, f->len);
			/* Queue it up if DTMF is deferred, or if DTMF emulation is forced. */
			if (tris_test_flag(chan, TRIS_FLAG_DEFER_DTMF) || tris_test_flag(chan, TRIS_FLAG_EMULATE_DTMF)) {
				queue_dtmf_readq(chan, f);
				tris_frfree(f);
				f = &tris_null_frame;
			} else if (!tris_test_flag(chan, TRIS_FLAG_IN_DTMF | TRIS_FLAG_END_DTMF_ONLY)) {
				if (!tris_tvzero(chan->dtmf_tv) && 
				    tris_tvdiff_ms(tris_tvnow(), chan->dtmf_tv) < TRIS_MIN_DTMF_GAP) {
					/* If it hasn't been long enough, defer this digit */
					queue_dtmf_readq(chan, f);
					tris_frfree(f);
					f = &tris_null_frame;
				} else {
					/* There was no begin, turn this into a begin and send the end later */
					f->frametype = TRIS_FRAME_DTMF_BEGIN;
					tris_set_flag(chan, TRIS_FLAG_EMULATE_DTMF);
					chan->emulate_dtmf_digit = f->subclass;
					chan->dtmf_tv = tris_tvnow();
					if (f->len) {
						if (f->len > TRIS_MIN_DTMF_DURATION)
							chan->emulate_dtmf_duration = f->len;
						else 
							chan->emulate_dtmf_duration = TRIS_MIN_DTMF_DURATION;
					} else
						chan->emulate_dtmf_duration = TRIS_DEFAULT_EMULATE_DTMF_DURATION;
					tris_log(LOG_DTMF, "DTMF begin emulation of '%c' with duration %u queued on %s\n", f->subclass, chan->emulate_dtmf_duration, chan->name);
				}
				if (chan->audiohooks) {
					struct tris_frame *old_frame = f;
					/*!
					 * \todo XXX It is possible to write a digit to the audiohook twice
					 * if the digit was originally read while the channel was in autoservice. */
					f = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_READ, f);
					if (old_frame != f)
						tris_frfree(old_frame);
				}
			} else {
				struct timeval now = tris_tvnow();
				if (tris_test_flag(chan, TRIS_FLAG_IN_DTMF)) {
					tris_log(LOG_DTMF, "DTMF end accepted with begin '%c' on %s\n", f->subclass, chan->name);
					tris_clear_flag(chan, TRIS_FLAG_IN_DTMF);
					if (!f->len)
						f->len = tris_tvdiff_ms(now, chan->dtmf_tv);
				} else if (!f->len) {
					tris_log(LOG_DTMF, "DTMF end accepted without begin '%c' on %s\n", f->subclass, chan->name);
					f->len = TRIS_MIN_DTMF_DURATION;
				}
				if (f->len < TRIS_MIN_DTMF_DURATION && !tris_test_flag(chan, TRIS_FLAG_END_DTMF_ONLY)) {
					tris_log(LOG_DTMF, "DTMF end '%c' has duration %ld but want minimum %d, emulating on %s\n", f->subclass, f->len, TRIS_MIN_DTMF_DURATION, chan->name);
					tris_set_flag(chan, TRIS_FLAG_EMULATE_DTMF);
					chan->emulate_dtmf_digit = f->subclass;
					chan->emulate_dtmf_duration = TRIS_MIN_DTMF_DURATION - f->len;
					tris_frfree(f);
					f = &tris_null_frame;
				} else {
					tris_log(LOG_DTMF, "DTMF end passthrough '%c' on %s\n", f->subclass, chan->name);
					if (f->len < TRIS_MIN_DTMF_DURATION) {
						f->len = TRIS_MIN_DTMF_DURATION;
					}
					chan->dtmf_tv = now;
				}
				if (chan->audiohooks) {
					struct tris_frame *old_frame = f;
					f = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_READ, f);
					if (old_frame != f)
						tris_frfree(old_frame);
				}
			}
			break;
		case TRIS_FRAME_DTMF_BEGIN:
			send_dtmf_event(chan, "Received", f->subclass, "Yes", "No");
			tris_log(LOG_DTMF, "DTMF begin '%c' received on %s\n", f->subclass, chan->name);
			if ( tris_test_flag(chan, TRIS_FLAG_DEFER_DTMF | TRIS_FLAG_END_DTMF_ONLY | TRIS_FLAG_EMULATE_DTMF) || 
			    (!tris_tvzero(chan->dtmf_tv) && 
			      tris_tvdiff_ms(tris_tvnow(), chan->dtmf_tv) < TRIS_MIN_DTMF_GAP) ) {
				tris_log(LOG_DTMF, "DTMF begin ignored '%c' on %s\n", f->subclass, chan->name);
				tris_frfree(f);
				f = &tris_null_frame;
			} else {
				tris_set_flag(chan, TRIS_FLAG_IN_DTMF);
				chan->dtmf_tv = tris_tvnow();
				tris_log(LOG_DTMF, "DTMF begin passthrough '%c' on %s\n", f->subclass, chan->name);
			}
			break;
		case TRIS_FRAME_NULL:
			/* The EMULATE_DTMF flag must be cleared here as opposed to when the duration
			 * is reached , because we want to make sure we pass at least one
			 * voice frame through before starting the next digit, to ensure a gap
			 * between DTMF digits. */
			if (tris_test_flag(chan, TRIS_FLAG_EMULATE_DTMF)) {
				struct timeval now = tris_tvnow();
				if (!chan->emulate_dtmf_duration) {
					tris_clear_flag(chan, TRIS_FLAG_EMULATE_DTMF);
					chan->emulate_dtmf_digit = 0;
				} else if (tris_tvdiff_ms(now, chan->dtmf_tv) >= chan->emulate_dtmf_duration) {
					chan->emulate_dtmf_duration = 0;
					tris_frfree(f);
					f = &chan->dtmff;
					f->frametype = TRIS_FRAME_DTMF_END;
					f->subclass = chan->emulate_dtmf_digit;
					f->len = tris_tvdiff_ms(now, chan->dtmf_tv);
					chan->dtmf_tv = now;
					tris_clear_flag(chan, TRIS_FLAG_EMULATE_DTMF);
					chan->emulate_dtmf_digit = 0;
					tris_log(LOG_DTMF, "DTMF end emulation of '%c' queued on %s\n", f->subclass, chan->name);
					if (chan->audiohooks) {
						struct tris_frame *old_frame = f;
						f = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_READ, f);
						if (old_frame != f) {
							tris_frfree(old_frame);
						}
					}
				}
			}
			break;
		case TRIS_FRAME_VOICE:
			/* The EMULATE_DTMF flag must be cleared here as opposed to when the duration
			 * is reached , because we want to make sure we pass at least one
			 * voice frame through before starting the next digit, to ensure a gap
			 * between DTMF digits. */
			if (tris_test_flag(chan, TRIS_FLAG_EMULATE_DTMF) && !chan->emulate_dtmf_duration) {
				tris_clear_flag(chan, TRIS_FLAG_EMULATE_DTMF);
				chan->emulate_dtmf_digit = 0;
			}

			if (dropaudio || tris_test_flag(chan, TRIS_FLAG_IN_DTMF)) {
				if (dropaudio)
					tris_read_generator_actions(chan, f);
				tris_frfree(f);
				f = &tris_null_frame;
			}

			if (tris_test_flag(chan, TRIS_FLAG_EMULATE_DTMF) && !tris_test_flag(chan, TRIS_FLAG_IN_DTMF)) {
				struct timeval now = tris_tvnow();
				if (tris_tvdiff_ms(now, chan->dtmf_tv) >= chan->emulate_dtmf_duration) {
					chan->emulate_dtmf_duration = 0;
					tris_frfree(f);
					f = &chan->dtmff;
					f->frametype = TRIS_FRAME_DTMF_END;
					f->subclass = chan->emulate_dtmf_digit;
					f->len = tris_tvdiff_ms(now, chan->dtmf_tv);
					chan->dtmf_tv = now;
					if (chan->audiohooks) {
						struct tris_frame *old_frame = f;
						f = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_READ, f);
						if (old_frame != f)
							tris_frfree(old_frame);
					}
					tris_log(LOG_DTMF, "DTMF end emulation of '%c' queued on %s\n", f->subclass, chan->name);
				} else {
					/* Drop voice frames while we're still in the middle of the digit */
					tris_frfree(f);
					f = &tris_null_frame;
				}
			} else if ((f->frametype == TRIS_FRAME_VOICE) && !(f->subclass & chan->nativeformats)) {
				/* This frame is not one of the current native formats -- drop it on the floor */
				char to[200];
				tris_log(LOG_NOTICE, "Dropping incompatible voice frame on %s of format %s since our native format has changed to %s\n",
					chan->name, tris_getformatname(f->subclass), tris_getformatname_multiple(to, sizeof(to), chan->nativeformats));
				tris_frfree(f);
				f = &tris_null_frame;
			} else if ((f->frametype == TRIS_FRAME_VOICE)) {
				/* Send frame to audiohooks if present */
				if (chan->audiohooks) {
					struct tris_frame *old_frame = f;
					f = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_READ, f);
					if (old_frame != f)
						tris_frfree(old_frame);
				}
				if (chan->monitor && chan->monitor->read_stream ) {
					/* XXX what does this do ? */
#ifndef MONITOR_CONSTANT_DELAY
					int jump = chan->outsmpl - chan->insmpl - 4 * f->samples;
					if (jump >= 0) {
						jump = calc_monitor_jump((chan->outsmpl - chan->insmpl), tris_format_rate(f->subclass), tris_format_rate(chan->monitor->read_stream->fmt->format));
						if (tris_seekstream(chan->monitor->read_stream, jump, SEEK_FORCECUR) == -1)
							tris_log(LOG_WARNING, "Failed to perform seek in monitoring read stream, synchronization between the files may be broken\n");
						chan->insmpl += (chan->outsmpl - chan->insmpl) + f->samples;
					} else
						chan->insmpl+= f->samples;
#else
					int jump = calc_monitor_jump((chan->outsmpl - chan->insmpl), tris_format_rate(f->subclass), tris_format_rate(chan->monitor->read_stream->fmt->format));
					if (jump - MONITOR_DELAY >= 0) {
						if (tris_seekstream(chan->monitor->read_stream, jump - f->samples, SEEK_FORCECUR) == -1)
							tris_log(LOG_WARNING, "Failed to perform seek in monitoring read stream, synchronization between the files may be broken\n");
						chan->insmpl += chan->outsmpl - chan->insmpl;
					} else
						chan->insmpl += f->samples;
#endif
					if (chan->monitor->state == TRIS_MONITOR_RUNNING) {
						if (tris_writestream(chan->monitor->read_stream, f) < 0)
							tris_log(LOG_WARNING, "Failed to write data to channel monitor read stream\n");
					}
				}

				if (chan->readtrans && (f = tris_translate(chan->readtrans, f, 1)) == NULL) {
					f = &tris_null_frame;
				}

				/* it is possible for the translation process on chan->readtrans to have
				   produced multiple frames from the single input frame we passed it; if
				   this happens, queue the additional frames *before* the frames we may
				   have queued earlier. if the readq was empty, put them at the head of
				   the queue, and if it was not, put them just after the frame that was
				   at the end of the queue.
				*/
				if (TRIS_LIST_NEXT(f, frame_list)) {
					if (!readq_tail) {
						tris_queue_frame_head(chan, TRIS_LIST_NEXT(f, frame_list));
					} else {
						__tris_queue_frame(chan, TRIS_LIST_NEXT(f, frame_list), 0, readq_tail);
					}
					tris_frfree(TRIS_LIST_NEXT(f, frame_list));
					TRIS_LIST_NEXT(f, frame_list) = NULL;
				}

				/* Run generator sitting on the line if timing device not available
				* and synchronous generation of outgoing frames is necessary       */
				tris_read_generator_actions(chan, f);
			}
		default:
			/* Just pass it on! */
			break;
		}
	} else {
		/* Make sure we always return NULL in the future */
		chan->_softhangup |= TRIS_SOFTHANGUP_DEV;
		if (cause)
			chan->hangupcause = cause;
		if (chan->generator)
			tris_deactivate_generator(chan);
		/* We no longer End the CDR here */
	}

	/* High bit prints debugging */
	if (chan->fin & DEBUGCHAN_FLAG)
		tris_frame_dump(chan->name, f, "<<");
	chan->fin = FRAMECOUNT_INC(chan->fin);

done:
	if (chan->music_state && chan->generator && chan->generator->digit && f && f->frametype == TRIS_FRAME_DTMF_END)
		chan->generator->digit(chan, f->subclass);

	tris_channel_unlock(chan);
	return f;
}

int tris_internal_timing_enabled(struct tris_channel *chan)
{
	int ret = tris_opt_internal_timing && chan->timingfd > -1;
	/*tris_debug(5, "Internal timing is %s (option_internal_timing=%d chan->timingfd=%d)\n", ret? "enabled": "disabled", tris_opt_internal_timing, chan->timingfd);*/
	return ret;
}

struct tris_frame *tris_read(struct tris_channel *chan)
{
	return __tris_read(chan, 0);
}

struct tris_frame *tris_read_noaudio(struct tris_channel *chan)
{
	return __tris_read(chan, 1);
}

int tris_indicate(struct tris_channel *chan, int condition)
{
	return tris_indicate_data(chan, condition, NULL, 0);
}

static int attribute_const is_visible_indication(enum tris_control_frame_type condition)
{
	/* Don't include a default case here so that we get compiler warnings
	 * when a new type is added. */

	switch (condition) {
	case TRIS_CONTROL_PROGRESS:
	case TRIS_CONTROL_PROCEEDING:
	case TRIS_CONTROL_VIDUPDATE:
	case TRIS_CONTROL_SRCUPDATE:
	case TRIS_CONTROL_SRCCHANGE:
	case TRIS_CONTROL_RADIO_KEY:
	case TRIS_CONTROL_RADIO_UNKEY:
	case TRIS_CONTROL_OPTION:
	case TRIS_CONTROL_WINK:
	case TRIS_CONTROL_FLASH:
	case TRIS_CONTROL_OFFHOOK:
	case TRIS_CONTROL_TAKEOFFHOOK:
	case TRIS_CONTROL_ANSWER:
	case TRIS_CONTROL_HANGUP:
	case TRIS_CONTROL_T38_PARAMETERS:
	case _XXX_TRIS_CONTROL_T38:
		break;

	case TRIS_CONTROL_CONGESTION:
	case TRIS_CONTROL_TIMEOUT:
	case TRIS_CONTROL_FORBIDDEN:
	case TRIS_CONTROL_BUSY:
	case TRIS_CONTROL_RINGING:
	case TRIS_CONTROL_RING:
	case TRIS_CONTROL_HOLD:
	case TRIS_CONTROL_UNHOLD:
	case TRIS_CONTROL_ROUTEFAIL:
	case TRIS_CONTROL_REJECTED:
	case TRIS_CONTROL_UNAVAILABLE:
		return 1;
	}

	return 0;
}

int tris_indicate_data(struct tris_channel *chan, int _condition,
		const void *data, size_t datalen)
{
	/* By using an enum, we'll get compiler warnings for values not handled 
	 * in switch statements. */
	enum tris_control_frame_type condition = _condition;
	struct tris_tone_zone_sound *ts = NULL;
	int res = -1;

	/* Don't bother if the channel is about to go away, anyway. */
	if (tris_test_flag(chan, TRIS_FLAG_ZOMBIE) || tris_check_hangup(chan)) {
		return -1;
	}

	tris_channel_lock(chan);

	if (chan->tech->indicate) {
		/* See if the channel driver can handle this condition. */
		res = chan->tech->indicate(chan, condition, data, datalen);
	}

	tris_channel_unlock(chan);

	if (!res) {
		/* The channel driver successfully handled this indication */
		if (is_visible_indication(condition)) {
			chan->visible_indication = condition;
		}
		return 0;
	}

	/* The channel driver does not support this indication, let's fake
	 * it by doing our own tone generation if applicable. */

	/*!\note If we compare the enumeration type, which does not have any
	 * negative constants, the compiler may optimize this code away.
	 * Therefore, we must perform an integer comparison here. */
	if (_condition < 0) {
		/* Stop any tones that are playing */
		tris_playtones_stop(chan);
		return 0;
	}

	/* Handle conditions that we have tones for. */
	switch (condition) {
	case _XXX_TRIS_CONTROL_T38:
		/* deprecated T.38 control frame */
		return -1;
	case TRIS_CONTROL_T38_PARAMETERS:
		/* there is no way to provide 'default' behavior for these
		 * control frames, so we need to return failure, but there
		 * is also no value in the log message below being emitted
		 * since failure to handle these frames is not an 'error'
		 * so just return right now.
		 */
		return -1;
	case TRIS_CONTROL_RINGING:
		ts = tris_get_indication_tone(chan->zone, "ring");
		/* It is common practice for channel drivers to return -1 if trying
		 * to indicate ringing on a channel which is up. The idea is to let the
		 * core generate the ringing inband. However, we don't want the
		 * warning message about not being able to handle the specific indication
		 * to print nor do we want tris_indicate_data to return an "error" for this
		 * condition
		 */
		if (chan->_state == TRIS_STATE_UP) {
			res = 0;
		}
		break;
	case TRIS_CONTROL_BUSY:
		ts = tris_get_indication_tone(chan->zone, "busy");
		break;
	case TRIS_CONTROL_CONGESTION:
		ts = tris_get_indication_tone(chan->zone, "congestion");
		break;
	case TRIS_CONTROL_PROGRESS:
	case TRIS_CONTROL_PROCEEDING:
	case TRIS_CONTROL_VIDUPDATE:
	case TRIS_CONTROL_SRCUPDATE:
	case TRIS_CONTROL_SRCCHANGE:
	case TRIS_CONTROL_RADIO_KEY:
	case TRIS_CONTROL_RADIO_UNKEY:
	case TRIS_CONTROL_OPTION:
	case TRIS_CONTROL_WINK:
	case TRIS_CONTROL_FLASH:
	case TRIS_CONTROL_OFFHOOK:
	case TRIS_CONTROL_TAKEOFFHOOK:
	case TRIS_CONTROL_TIMEOUT:
	case TRIS_CONTROL_FORBIDDEN:
	case TRIS_CONTROL_ANSWER:
	case TRIS_CONTROL_HANGUP:
	case TRIS_CONTROL_RING:
	case TRIS_CONTROL_HOLD:
	case TRIS_CONTROL_UNHOLD:
	case TRIS_CONTROL_ROUTEFAIL:
	case TRIS_CONTROL_REJECTED:
	case TRIS_CONTROL_UNAVAILABLE:
		/* Nothing left to do for these. */
		res = 0;
		break;
	}

	if (ts) {
		/* We have a tone to play, yay. */
		tris_debug(1, "Driver for channel '%s' does not support indication %d, emulating it\n", chan->name, condition);
		tris_playtones_start(chan, 0, ts->data, 1);
		ts = tris_tone_zone_sound_unref(ts);
		res = 0;
		chan->visible_indication = condition;
	}

	if (res) {
		/* not handled */
		tris_log(LOG_WARNING, "Unable to handle indication %d for '%s'\n", condition, chan->name);
	}

	return res;
}

int tris_recvchar(struct tris_channel *chan, int timeout)
{
	int c;
	char *buf = tris_recvtext(chan, timeout);
	if (buf == NULL)
		return -1;	/* error or timeout */
	c = *(unsigned char *)buf;
	tris_free(buf);
	return c;
}

char *tris_recvtext(struct tris_channel *chan, int timeout)
{
	int res, done = 0;
	char *buf = NULL;
	
	while (!done) {
		struct tris_frame *f;
		if (tris_check_hangup(chan))
			break;
		res = tris_waitfor(chan, timeout);
		if (res <= 0) /* timeout or error */
			break;
		timeout = res;	/* update timeout */
		f = tris_read(chan);
		if (f == NULL)
			break; /* no frame */
		if (f->frametype == TRIS_FRAME_CONTROL && f->subclass == TRIS_CONTROL_HANGUP)
			done = 1;	/* force a break */
		else if (f->frametype == TRIS_FRAME_TEXT) {		/* what we want */
			buf = tris_strndup((char *) f->data.ptr, f->datalen);	/* dup and break */
			done = 1;
		}
		tris_frfree(f);
	}
	return buf;
}

int tris_sendtext(struct tris_channel *chan, const char *text)
{
	int res = 0;
	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(chan, TRIS_FLAG_ZOMBIE) || tris_check_hangup(chan))
		return -1;
	CHECK_BLOCKING(chan);
	if (chan->tech->send_text)
		res = chan->tech->send_text(chan, text);
	tris_clear_flag(chan, TRIS_FLAG_BLOCKING);
	return res;
}

int tris_senddigit_begin(struct tris_channel *chan, char digit)
{
	/* Device does not support DTMF tones, lets fake
	 * it by doing our own generation. */
	static const char* dtmf_tones[] = {
		"941+1336", /* 0 */
		"697+1209", /* 1 */
		"697+1336", /* 2 */
		"697+1477", /* 3 */
		"770+1209", /* 4 */
		"770+1336", /* 5 */
		"770+1477", /* 6 */
		"852+1209", /* 7 */
		"852+1336", /* 8 */
		"852+1477", /* 9 */
		"697+1633", /* A */
		"770+1633", /* B */
		"852+1633", /* C */
		"941+1633", /* D */
		"941+1209", /* * */
		"941+1477"  /* # */
	};

	if (!chan->tech->send_digit_begin)
		return 0;

	if (!chan->tech->send_digit_begin(chan, digit))
		return 0;

	if (digit >= '0' && digit <='9')
		tris_playtones_start(chan, 0, dtmf_tones[digit-'0'], 0);
	else if (digit >= 'A' && digit <= 'D')
		tris_playtones_start(chan, 0, dtmf_tones[digit-'A'+10], 0);
	else if (digit == '*')
		tris_playtones_start(chan, 0, dtmf_tones[14], 0);
	else if (digit == '#')
		tris_playtones_start(chan, 0, dtmf_tones[15], 0);
	else {
		/* not handled */
		tris_debug(1, "Unable to generate DTMF tone '%c' for '%s'\n", digit, chan->name);
	}

	return 0;
}

int tris_senddigit_end(struct tris_channel *chan, char digit, unsigned int duration)
{
	int res = -1;

	if (chan->tech->send_digit_end)
		res = chan->tech->send_digit_end(chan, digit, duration);

	if (res && chan->generator)
		tris_playtones_stop(chan);
	
	return 0;
}

int tris_senddigit(struct tris_channel *chan, char digit, unsigned int duration)
{
	if (chan->tech->send_digit_begin) {
		tris_senddigit_begin(chan, digit);
		tris_safe_sleep(chan, (duration >= TRIS_DEFAULT_EMULATE_DTMF_DURATION ? duration : TRIS_DEFAULT_EMULATE_DTMF_DURATION));
	}
	
	return tris_senddigit_end(chan, digit, (duration >= TRIS_DEFAULT_EMULATE_DTMF_DURATION ? duration : TRIS_DEFAULT_EMULATE_DTMF_DURATION));
}

int tris_prod(struct tris_channel *chan)
{
	struct tris_frame a = { TRIS_FRAME_VOICE };
	char nothing[128];

	/* Send an empty audio frame to get things moving */
	if (chan->_state != TRIS_STATE_UP) {
		tris_debug(1, "Prodding channel '%s'\n", chan->name);
		a.subclass = chan->rawwriteformat;
		a.data.ptr = nothing + TRIS_FRIENDLY_OFFSET;
		a.src = "tris_prod";
		if (tris_write(chan, &a))
			tris_log(LOG_WARNING, "Prodding channel '%s' failed\n", chan->name);
	}
	return 0;
}

int tris_write_video(struct tris_channel *chan, struct tris_frame *fr)
{
	int res;
	if (!chan->tech->write_video)
		return 0;
	res = tris_write(chan, fr);
	if (!res)
		res = 1;
	return res;
}

int tris_write(struct tris_channel *chan, struct tris_frame *fr)
{
	int res = -1;
	struct tris_frame *f = NULL;
	int count = 0;

	/*Deadlock avoidance*/
	while(tris_channel_trylock(chan)) {
		/*cannot goto done since the channel is not locked*/
		if(count++ > 10) {
			tris_debug(1, "Deadlock avoided for write to channel '%s'\n", chan->name);
			return 0;
		}
		usleep(1);
	}
	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(chan, TRIS_FLAG_ZOMBIE) || tris_check_hangup(chan))
		goto done;

	/* Handle any pending masquerades */
	if (chan->masq && tris_do_masquerade(chan)) {
		tris_log(LOG_WARNING, "Failed to perform masquerade\n");
		goto done;
	}
	if (chan->masqr) {
		res = 0;	/* XXX explain, why 0 ? */
		goto done;
	}
	if (chan->generatordata) {
		if (tris_test_flag(chan, TRIS_FLAG_WRITE_INT))
			tris_deactivate_generator(chan);
		else {
			if (fr->frametype == TRIS_FRAME_DTMF_END) {
				/* There is a generator running while we're in the middle of a digit.
				 * It's probably inband DTMF, so go ahead and pass it so it can
				 * stop the generator */
				tris_clear_flag(chan, TRIS_FLAG_BLOCKING);
				tris_channel_unlock(chan);
				res = tris_senddigit_end(chan, fr->subclass, fr->len);
				tris_channel_lock(chan);
				CHECK_BLOCKING(chan);
			} else if (fr->frametype == TRIS_FRAME_CONTROL && fr->subclass == TRIS_CONTROL_UNHOLD) {
				/* This is a side case where Echo is basically being called and the person put themselves on hold and took themselves off hold */
				res = (chan->tech->indicate == NULL) ? 0 :
					chan->tech->indicate(chan, fr->subclass, fr->data.ptr, fr->datalen);
			}
			res = 0;	/* XXX explain, why 0 ? */
			goto done;
		}
	}
	/* High bit prints debugging */
	if (chan->fout & DEBUGCHAN_FLAG)
		tris_frame_dump(chan->name, fr, ">>");
	CHECK_BLOCKING(chan);
	switch (fr->frametype) {
	case TRIS_FRAME_CONTROL:
		res = (chan->tech->indicate == NULL) ? 0 :
			chan->tech->indicate(chan, fr->subclass, fr->data.ptr, fr->datalen);
		break;
	case TRIS_FRAME_DTMF_BEGIN:
		if (chan->audiohooks) {
			struct tris_frame *old_frame = fr;
			fr = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_WRITE, fr);
			if (old_frame != fr)
				f = fr;
		}
		send_dtmf_event(chan, "Sent", fr->subclass, "Yes", "No");
		tris_clear_flag(chan, TRIS_FLAG_BLOCKING);
		tris_channel_unlock(chan);
		res = tris_senddigit_begin(chan, fr->subclass);
		tris_channel_lock(chan);
		CHECK_BLOCKING(chan);
		break;
	case TRIS_FRAME_DTMF_END:
		if (chan->audiohooks) {
			struct tris_frame *new_frame = fr;

			new_frame = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_WRITE, fr);
			if (new_frame != fr) {
				tris_frfree(new_frame);
			}
		}
		send_dtmf_event(chan, "Sent", fr->subclass, "No", "Yes");
		tris_clear_flag(chan, TRIS_FLAG_BLOCKING);
		tris_channel_unlock(chan);
		res = tris_senddigit_end(chan, fr->subclass, fr->len);
		tris_channel_lock(chan);
		CHECK_BLOCKING(chan);
		break;
	case TRIS_FRAME_TEXT:
		if (fr->subclass == TRIS_FORMAT_T140) {
			res = (chan->tech->write_text == NULL) ? 0 :
				chan->tech->write_text(chan, fr);
		} else {
			res = (chan->tech->send_text == NULL) ? 0 :
				chan->tech->send_text(chan, (char *) fr->data.ptr);
		}
		break;
	case TRIS_FRAME_HTML:
		res = (chan->tech->send_html == NULL) ? 0 :
			chan->tech->send_html(chan, fr->subclass, (char *) fr->data.ptr, fr->datalen);
		break;
	case TRIS_FRAME_VIDEO:
		/* XXX Handle translation of video codecs one day XXX */
		res = (chan->tech->write_video == NULL) ? 0 :
			chan->tech->write_video(chan, fr);
		break;
	case TRIS_FRAME_MODEM:
		res = (chan->tech->write == NULL) ? 0 :
			chan->tech->write(chan, fr);
		break;
	case TRIS_FRAME_VOICE:
		if (chan->tech->write == NULL)
			break;	/*! \todo XXX should return 0 maybe ? */

		/* If the frame is in the raw write format, then it's easy... just use the frame - otherwise we will have to translate */
		if (fr->subclass == chan->rawwriteformat)
			f = fr;
		else
			f = (chan->writetrans) ? tris_translate(chan->writetrans, fr, 0) : fr;

		if (!f) {
			res = 0;
			break;
		}

		if (chan->audiohooks) {
			struct tris_frame *prev = NULL, *new_frame, *cur, *dup;
			int freeoldlist = 0;

			if (f != fr) {
				freeoldlist = 1;
			}

			/* Since tris_audiohook_write may return a new frame, and the cur frame is
			 * an item in a list of frames, create a new list adding each cur frame back to it
			 * regardless if the cur frame changes or not. */
			for (cur = f; cur; cur = TRIS_LIST_NEXT(cur, frame_list)) {
				new_frame = tris_audiohook_write_list(chan, chan->audiohooks, TRIS_AUDIOHOOK_DIRECTION_WRITE, cur);

				/* if this frame is different than cur, preserve the end of the list,
				 * free the old frames, and set cur to be the new frame */
				if (new_frame != cur) {

					/* doing an tris_frisolate here seems silly, but we are not guaranteed the new_frame
					 * isn't part of local storage, meaning if tris_audiohook_write is called multiple
					 * times it may override the previous frame we got from it unless we dup it */
					if ((dup = tris_frisolate(new_frame))) {
						TRIS_LIST_NEXT(dup, frame_list) = TRIS_LIST_NEXT(cur, frame_list);
						if (freeoldlist) {
							TRIS_LIST_NEXT(cur, frame_list) = NULL;
							tris_frfree(cur);
						}
						cur = dup;
					}
				}

				/* now, regardless if cur is new or not, add it to the new list,
				 * if the new list has not started, cur will become the first item. */
				if (prev) {
					TRIS_LIST_NEXT(prev, frame_list) = cur;
				} else {
					f = cur; /* set f to be the beginning of our new list */
				}
				prev = cur;
			}
		}
		
		/* If Monitor is running on this channel, then we have to write frames out there too */
		/* the translator on chan->writetrans may have returned multiple frames
		   from the single frame we passed in; if so, feed each one of them to the
		   monitor */
		if (chan->monitor && chan->monitor->write_stream) {
			struct tris_frame *cur;

			for (cur = f; cur; cur = TRIS_LIST_NEXT(cur, frame_list)) {
			/* XXX must explain this code */
#ifndef MONITOR_CONSTANT_DELAY
				int jump = chan->insmpl - chan->outsmpl - 4 * cur->samples;
				if (jump >= 0) {
					jump = calc_monitor_jump((chan->insmpl - chan->outsmpl), tris_format_rate(f->subclass), tris_format_rate(chan->monitor->read_stream->fmt->format));
					if (tris_seekstream(chan->monitor->write_stream, jump, SEEK_FORCECUR) == -1)
						tris_log(LOG_WARNING, "Failed to perform seek in monitoring write stream, synchronization between the files may be broken\n");
					chan->outsmpl += (chan->insmpl - chan->outsmpl) + cur->samples;
				} else {
					chan->outsmpl += cur->samples;
				}
#else
				int jump = calc_monitor_jump((chan->insmpl - chan->outsmpl), tris_format_rate(f->subclass), tris_format_rate(chan->monitor->read_stream->fmt->format));
				if (jump - MONITOR_DELAY >= 0) {
					if (tris_seekstream(chan->monitor->write_stream, jump - cur->samples, SEEK_FORCECUR) == -1)
						tris_log(LOG_WARNING, "Failed to perform seek in monitoring write stream, synchronization between the files may be broken\n");
					chan->outsmpl += chan->insmpl - chan->outsmpl;
				} else {
					chan->outsmpl += cur->samples;
				}
#endif
				if (chan->monitor->state == TRIS_MONITOR_RUNNING) {
					if (tris_writestream(chan->monitor->write_stream, cur) < 0)
						tris_log(LOG_WARNING, "Failed to write data to channel monitor write stream\n");
				}
			}
		}

		/* the translator on chan->writetrans may have returned multiple frames
		   from the single frame we passed in; if so, feed each one of them to the
		   channel, freeing each one after it has been written */
		if ((f != fr) && TRIS_LIST_NEXT(f, frame_list)) {
			struct tris_frame *cur, *next;
			unsigned int skip = 0;

			for (cur = f, next = TRIS_LIST_NEXT(cur, frame_list);
			     cur;
			     cur = next, next = cur ? TRIS_LIST_NEXT(cur, frame_list) : NULL) {
				if (!skip) {
					if ((res = chan->tech->write(chan, cur)) < 0) {
						chan->_softhangup |= TRIS_SOFTHANGUP_DEV;
						skip = 1;
					} else if (next) {
						/* don't do this for the last frame in the list,
						   as the code outside the loop will do it once
						*/
						chan->fout = FRAMECOUNT_INC(chan->fout);
					}
				}
				tris_frfree(cur);
			}

			/* reset f so the code below doesn't attempt to free it */
			f = NULL;
		} else {
			res = chan->tech->write(chan, f);
		}
		break;
	case TRIS_FRAME_NULL:
	case TRIS_FRAME_IAX:
		/* Ignore these */
		res = 0;
		break;
	default:
		/* At this point, fr is the incoming frame and f is NULL.  Channels do
		 * not expect to get NULL as a frame pointer and will segfault.  Hence,
		 * we output the original frame passed in. */
		res = chan->tech->write(chan, fr);
		break;
	}

	if (f && f != fr)
		tris_frfree(f);
	tris_clear_flag(chan, TRIS_FLAG_BLOCKING);

	/* Consider a write failure to force a soft hangup */
	if (res < 0) {
		chan->_softhangup |= TRIS_SOFTHANGUP_DEV;
	} else {
		chan->fout = FRAMECOUNT_INC(chan->fout);
	}
done:
	tris_channel_unlock(chan);
	return res;
}

static int set_format(struct tris_channel *chan, int fmt, int *rawformat, int *format,
		      struct tris_trans_pvt **trans, const int direction)
{
	int native;
	int res;
	char from[200], to[200];
	
	/* Make sure we only consider audio */
	fmt &= TRIS_FORMAT_AUDIO_MASK;
	
	native = chan->nativeformats;

	if (!fmt || !native)	/* No audio requested */
		return 0;	/* Let's try a call without any sounds (video, text) */
	
	/* Find a translation path from the native format to one of the desired formats */
	if (!direction)
		/* reading */
		res = tris_translator_best_choice(&fmt, &native);
	else
		/* writing */
		res = tris_translator_best_choice(&native, &fmt);

	if (res < 0) {
		tris_log(LOG_WARNING, "Unable to find a codec translation path from %s to %s\n",
			tris_getformatname_multiple(from, sizeof(from), native),
			tris_getformatname_multiple(to, sizeof(to), fmt));
		return -1;
	}
	
	/* Now we have a good choice for both. */
	tris_channel_lock(chan);

	if ((*rawformat == native) && (*format == fmt) && ((*rawformat == *format) || (*trans))) {
		/* the channel is already in these formats, so nothing to do */
		tris_channel_unlock(chan);
		return 0;
	}

	*rawformat = native;
	/* User perspective is fmt */
	*format = fmt;
	/* Free any read translation we have right now */
	if (*trans)
		tris_translator_free_path(*trans);
	/* Build a translation path from the raw format to the desired format */
	if (!direction)
		/* reading */
		*trans = tris_translator_build_path(*format, *rawformat);
	else
		/* writing */
		*trans = tris_translator_build_path(*rawformat, *format);
	tris_channel_unlock(chan);
	tris_debug(1, "Set channel %s to %s format %s\n", chan->name,
		direction ? "write" : "read", tris_getformatname(fmt));
	return 0;
}

int tris_set_read_format(struct tris_channel *chan, int fmt)
{
	return set_format(chan, fmt, &chan->rawreadformat, &chan->readformat,
			  &chan->readtrans, 0);
}

int tris_set_write_format(struct tris_channel *chan, int fmt)
{
	return set_format(chan, fmt, &chan->rawwriteformat, &chan->writeformat,
			  &chan->writetrans, 1);
}

const char *tris_channel_reason2str(int reason)
{
	switch (reason) /* the following appear to be the only ones actually returned by request_and_dial */
	{
	case 0:
		return "Call Failure (not BUSY, and not NO_ANSWER, maybe Circuit busy or down?)";
	case TRIS_CONTROL_HANGUP:
		return "Hangup";
	case TRIS_CONTROL_RING:
		return "Local Ring";
	case TRIS_CONTROL_RINGING:
		return "Remote end Ringing";
	case TRIS_CONTROL_ANSWER:
		return "Remote end has Answered";
	case TRIS_CONTROL_BUSY:
		return "Remote end is Busy";
	case TRIS_CONTROL_CONGESTION:
		return "Congestion (circuits busy)";
	case TRIS_CONTROL_TIMEOUT:
		return "Timeout (circuits busy)";
	case TRIS_CONTROL_FORBIDDEN:
		return "Forbidden (circuits busy)";
	case TRIS_CONTROL_ROUTEFAIL:
		return "Routefail (circuits busy)";
	case TRIS_CONTROL_REJECTED:
		return "Rejected (circuits busy)";
	case TRIS_CONTROL_UNAVAILABLE:
		return "Unavailable (circuits busy)";
	default:
		return "Unknown Reason!!";
	}
}

static void handle_cause(int cause, int *outstate)
{
	if (outstate) {
		/* compute error and return */
		if (cause == TRIS_CAUSE_BUSY)
			*outstate = TRIS_CONTROL_BUSY;
		else if (cause == TRIS_CAUSE_CONGESTION)
			*outstate = TRIS_CONTROL_CONGESTION;
		else
			*outstate = 0;
	}
}

struct tris_channel *tris_call_forward(struct tris_channel *caller, struct tris_channel *orig, int *timeout, int format, struct outgoing_helper *oh, int *outstate)
{
	char tmpchan[256];
	struct tris_channel *new = NULL;
	char *data, *type;
	int cause = 0;

	/* gather data and request the new forward channel */
	tris_copy_string(tmpchan, orig->call_forward, sizeof(tmpchan));
	if ((data = strchr(tmpchan, '/'))) {
		*data++ = '\0';
		type = tmpchan;
	} else {
		const char *forward_context;
		tris_channel_lock(orig);
		forward_context = pbx_builtin_getvar_helper(orig, "FORWARD_CONTEXT");
		snprintf(tmpchan, sizeof(tmpchan), "%s@%s", orig->call_forward, S_OR(forward_context, orig->context));
		tris_channel_unlock(orig);
		data = tmpchan;
		type = "Local";
	}
	if (!(new = tris_request(type, format, data, &cause, 0))) {
		tris_log(LOG_NOTICE, "Unable to create channel for call forward to '%s/%s' (cause = %d)\n", type, data, cause);
		handle_cause(cause, outstate);
		tris_hangup(orig);
		return NULL;
	}

	/* Copy/inherit important information into new channel */
	if (oh) {
		if (oh->vars) {
			tris_set_variables(new, oh->vars);
		}
		if (!tris_strlen_zero(oh->cid_num) && !tris_strlen_zero(oh->cid_name)) {
			tris_set_callerid(new, oh->cid_num, oh->cid_name, oh->cid_num);
		}
		if (oh->parent_channel) {
			tris_channel_inherit_variables(oh->parent_channel, new);
			tris_channel_datastore_inherit(oh->parent_channel, new);
		}
		if (oh->account) {
			tris_cdr_setaccount(new, oh->account);
		}
	} else if (caller) { /* no outgoing helper so use caller if avaliable */
		tris_channel_inherit_variables(caller, new);
		tris_channel_datastore_inherit(caller, new);
	}

	tris_channel_lock(orig);
	while (tris_channel_trylock(new)) {
		CHANNEL_DEADLOCK_AVOIDANCE(orig);
	}
	tris_copy_flags(new->cdr, orig->cdr, TRIS_CDR_FLAG_ORIGINATED);
	tris_string_field_set(new, accountcode, orig->accountcode);
	if (!tris_strlen_zero(orig->cid.cid_num) && !tris_strlen_zero(new->cid.cid_name)) {
		tris_set_callerid(new, orig->cid.cid_num, orig->cid.cid_name, orig->cid.cid_num);
	}
	tris_channel_unlock(new);
	tris_channel_unlock(orig);

	/* call new channel */
	if ((*timeout = tris_call(new, data, 0))) {
		tris_log(LOG_NOTICE, "Unable to call forward to channel %s/%s\n", type, (char *)data);
		tris_hangup(orig);
		tris_hangup(new);
		return NULL;
	}
	tris_hangup(orig);

	return new;
}

struct tris_channel *__tris_request_and_dial(const char *type, int format, void *data, int timeout, int *outstate,
		const char *cid_num, const char *cid_name, struct outgoing_helper *oh)
{
	int dummy_outstate;
	int cause = 0;
	struct tris_channel *chan;
	int res = 0;
	int last_subclass = 0;
	struct tris_app *the_app;
	char callinfo[80];
	
	if (outstate)
		*outstate = 0;
	else
		outstate = &dummy_outstate;	/* make outstate always a valid pointer */

	chan = tris_request(type, format, data, &cause, 0);
	if (!chan) {
		tris_log(LOG_NOTICE, "Unable to request channel %s/%s\n", type, (char *)data);
		handle_cause(cause, outstate);
		return NULL;
	}

	if (oh) {
		if (oh->vars)	
			tris_set_variables(chan, oh->vars);
		/* XXX why is this necessary, for the parent_channel perhaps ? */
		if (!tris_strlen_zero(oh->cid_num) && !tris_strlen_zero(oh->cid_name))
			tris_set_callerid(chan, oh->cid_num, oh->cid_name, oh->cid_num);
		if (oh->parent_channel) {
			tris_channel_inherit_variables(oh->parent_channel, chan);
			tris_channel_datastore_inherit(oh->parent_channel, chan);
		}
		if (oh->account)
			tris_cdr_setaccount(chan, oh->account);	
	}
	tris_set_callerid(chan, cid_num, cid_name, cid_num);
	tris_set_flag(chan->cdr, TRIS_CDR_FLAG_ORIGINATED);

	if (type && !strcmp(type, "SIP")) {
		if (cid_name && !strcmp(cid_name, "Broadcast"))
			snprintf(callinfo, sizeof(callinfo), "Call-Info: MS,broadcast3,%s", cid_num);
		else
			snprintf(callinfo, sizeof(callinfo), "Call-Info: MS,outgoing,%s", cid_num);
		the_app = pbx_findapp("SIPAddHeader");
		if (the_app)
			pbx_exec(chan, the_app, callinfo);
	}

	if (tris_call(chan, data, 0)) {	/* tris_call failed... */
		tris_log(LOG_NOTICE, "Unable to call channel %s/%s\n", type, (char *)data);
	} else {
		res = 1;	/* mark success in case chan->_state is already TRIS_STATE_UP */
		while (timeout && chan->_state != TRIS_STATE_UP) {
			struct tris_frame *f;
			res = tris_waitfor(chan, timeout);
			if (res == 0) { /* timeout, treat it like ringing */
				*outstate = TRIS_CONTROL_RINGING;
				break;
			}
			if (res < 0) /* error or done */
				break;
			if (timeout > -1)
				timeout = res;
			if (!tris_strlen_zero(chan->call_forward)) {
				if (!(chan = tris_call_forward(NULL, chan, &timeout, format, oh, outstate))) {
					return NULL;
				}
				continue;
			}

			f = tris_read(chan);
			if (!f) {
				*outstate = TRIS_CONTROL_HANGUP;
				res = 0;
				break;
			}
			if (f->frametype == TRIS_FRAME_CONTROL) {
				switch (f->subclass) {
				case TRIS_CONTROL_RINGING:	/* record but keep going */
					*outstate = f->subclass;
					break;

				case TRIS_CONTROL_BUSY:
				case TRIS_CONTROL_CONGESTION:
				case TRIS_CONTROL_ANSWER:
				case TRIS_CONTROL_TAKEOFFHOOK:
				case TRIS_CONTROL_OFFHOOK:
				case TRIS_CONTROL_TIMEOUT:
				case TRIS_CONTROL_FORBIDDEN:
				case TRIS_CONTROL_ROUTEFAIL:
				case TRIS_CONTROL_REJECTED:
				case TRIS_CONTROL_UNAVAILABLE:
					*outstate = f->subclass;
					timeout = 0;		/* trick to force exit from the while() */
					break;

				/* Ignore these */
				case TRIS_CONTROL_PROGRESS:
				case TRIS_CONTROL_PROCEEDING:
				case TRIS_CONTROL_HOLD:
				case TRIS_CONTROL_UNHOLD:
				case TRIS_CONTROL_VIDUPDATE:
				case TRIS_CONTROL_SRCUPDATE:
				case TRIS_CONTROL_SRCCHANGE:
				case -1:			/* Ignore -- just stopping indications */
					break;

				default:
					tris_log(LOG_NOTICE, "Don't know what to do with control frame %d\n", f->subclass);
				}
				last_subclass = f->subclass;
			}
			tris_frfree(f);
		}
	}

	/* Final fixups */
	if (oh) {
		if (!tris_strlen_zero(oh->context))
			tris_copy_string(chan->context, oh->context, sizeof(chan->context));
		if (!tris_strlen_zero(oh->exten))
			tris_copy_string(chan->exten, oh->exten, sizeof(chan->exten));
		if (oh->priority)	
			chan->priority = oh->priority;
	}
	if (chan->_state == TRIS_STATE_UP)
		*outstate = TRIS_CONTROL_ANSWER;

	if (res <= 0) {
		if ( TRIS_CONTROL_RINGING == last_subclass ) 
			chan->hangupcause = TRIS_CAUSE_NO_ANSWER;
		if (!chan->cdr && (chan->cdr = tris_cdr_alloc()))
			tris_cdr_init(chan->cdr, chan);
		if (chan->cdr) {
			char tmp[256];
			snprintf(tmp, sizeof(tmp), "%s/%s", type, (char *)data);
			tris_cdr_setapp(chan->cdr,"Dial",tmp);
			tris_cdr_update(chan);
			tris_cdr_start(chan->cdr);
			tris_cdr_end(chan->cdr);
			/* If the cause wasn't handled properly */
			if (tris_cdr_disposition(chan->cdr,chan->hangupcause))
				tris_cdr_failed(chan->cdr);
		}
		tris_hangup(chan);
		chan = NULL;
	}
	return chan;
}

struct tris_channel *tris_request_and_dial(const char *type, int format, void *data, int timeout, int *outstate,
		const char *cidnum, const char *cidname)
{
	return __tris_request_and_dial(type, format, data, timeout, outstate, cidnum, cidname, NULL);
}

struct tris_channel *tris_request(const char *type, int format, void *data, int *cause, struct tris_channel* src)
{
	struct chanlist *chan;
	struct tris_channel *c;
	int capabilities;
	int fmt;
	int res;
	int foo;
	int videoformat = format & TRIS_FORMAT_VIDEO_MASK;
	int textformat = format & TRIS_FORMAT_TEXT_MASK;

	if (!cause)
		cause = &foo;
	*cause = TRIS_CAUSE_NOTDEFINED;

	if (TRIS_RWLIST_RDLOCK(&channels)) {
		tris_log(LOG_WARNING, "Unable to lock channel list\n");
		return NULL;
	}

	TRIS_LIST_TRAVERSE(&backends, chan, list) {
		if (strcasecmp(type, chan->tech->type))
			continue;

		capabilities = chan->tech->capabilities;
		fmt = format & TRIS_FORMAT_AUDIO_MASK;
		if (fmt) {
			/* We have audio - is it possible to connect the various calls to each other? 
				(Avoid this check for calls without audio, like text+video calls)
			*/
			res = tris_translator_best_choice(&fmt, &capabilities);
			if (res < 0) {
				tris_log(LOG_WARNING, "No translator path exists for channel type %s (native 0x%x) to 0x%x\n", type, chan->tech->capabilities, format);
				*cause = TRIS_CAUSE_BEARERCAPABILITY_NOTAVAIL;
				TRIS_RWLIST_UNLOCK(&channels);
				return NULL;
			}
		}
		TRIS_RWLIST_UNLOCK(&channels);
		if (!chan->tech->requester)
			return NULL;
		
		if (!(c = chan->tech->requester(type, capabilities | videoformat | textformat, data, cause, src)))
			return NULL;
		
		/* no need to generate a Newchannel event here; it is done in the channel_alloc call */
		return c;
	}

	tris_log(LOG_WARNING, "No channel type registered for '%s'\n", type);
	*cause = TRIS_CAUSE_NOSUCHDRIVER;
	TRIS_RWLIST_UNLOCK(&channels);

	return NULL;
}

int tris_call(struct tris_channel *chan, char *addr, int timeout)
{
	/* Place an outgoing call, but don't wait any longer than timeout ms before returning.
	   If the remote end does not answer within the timeout, then do NOT hang up, but
	   return anyway.  */
	int res = -1;
	/* Stop if we're a zombie or need a soft hangup */
	tris_channel_lock(chan);
	if (!tris_test_flag(chan, TRIS_FLAG_ZOMBIE) && !tris_check_hangup(chan)) {
		if (chan->cdr) {
			tris_set_flag(chan->cdr, TRIS_CDR_FLAG_DIALED);
			tris_set_flag(chan->cdr, TRIS_CDR_FLAG_ORIGINATED);
		}
		if (chan->tech->call)
			res = chan->tech->call(chan, addr, timeout);
		tris_set_flag(chan, TRIS_FLAG_OUTGOING);
	}
	tris_channel_unlock(chan);
	return res;
}

/*!
  \brief Transfer a call to dest, if the channel supports transfer

  Called by:
	\arg app_transfer
	\arg the manager interface
*/
int tris_transfer(struct tris_channel *chan, char *dest)
{
	int res = -1;

	/* Stop if we're a zombie or need a soft hangup */
	tris_channel_lock(chan);
	if (!tris_test_flag(chan, TRIS_FLAG_ZOMBIE) && !tris_check_hangup(chan)) {
		if (chan->tech->transfer) {
			res = chan->tech->transfer(chan, dest);
			if (!res)
				res = 1;
		} else
			res = 0;
	}
	tris_channel_unlock(chan);
	return res;
}

int tris_readstring(struct tris_channel *c, char *s, int len, int timeout, int ftimeout, char *enders)
{
	return tris_readstring_full(c, s, len, timeout, ftimeout, enders, -1, -1);
}

int tris_readstring_full(struct tris_channel *c, char *s, int len, int timeout, int ftimeout, char *enders, int audiofd, int ctrlfd)
{
	int pos = 0;	/* index in the buffer where we accumulate digits */
	int to = ftimeout;

	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(c, TRIS_FLAG_ZOMBIE) || tris_check_hangup(c))
		return -1;
	if (!len)
		return -1;
	for (;;) {
		int d;
		if (c->stream) {
			d = tris_waitstream_full(c, TRIS_DIGIT_ANY, audiofd, ctrlfd);
			tris_stopstream(c);
			usleep(1000);
			if (!d)
				d = tris_waitfordigit_full(c, to, audiofd, ctrlfd);
		} else {
			d = tris_waitfordigit_full(c, to, audiofd, ctrlfd);
		}
		if (d < 0)
			return TRIS_GETDATA_FAILED;
		if (d == 0) {
			s[pos] = '\0';
			return TRIS_GETDATA_TIMEOUT;
		}
		if (d == 1) {
			s[pos] = '\0';
			return TRIS_GETDATA_INTERRUPTED;
		}
		if (strchr(enders, d) && (pos == 0)) {
			s[pos] = '\0';
			return TRIS_GETDATA_EMPTY_END_TERMINATED;
		}
		if (!strchr(enders, d)) {
			s[pos++] = d;
		}
		if (strchr(enders, d) || (pos >= len)) {
			s[pos] = '\0';
			return TRIS_GETDATA_COMPLETE;
		}
		to = timeout;
	}
	/* Never reached */
	return 0;
}

int tris_channel_supports_html(struct tris_channel *chan)
{
	return (chan->tech->send_html) ? 1 : 0;
}

int tris_channel_sendhtml(struct tris_channel *chan, int subclass, const char *data, int datalen)
{
	if (chan->tech->send_html)
		return chan->tech->send_html(chan, subclass, data, datalen);
	return -1;
}

int tris_channel_sendurl(struct tris_channel *chan, const char *url)
{
	return tris_channel_sendhtml(chan, TRIS_HTML_URL, url, strlen(url) + 1);
}

/*! \brief Set up translation from one channel to another */
static int tris_channel_make_compatible_helper(struct tris_channel *from, struct tris_channel *to)
{
	int src;
	int dst;

	if (from->readformat == to->writeformat && from->writeformat == to->readformat) {
		/* Already compatible!  Moving on ... */
		return 0;
	}

	/* Set up translation from the 'from' channel to the 'to' channel */
	src = from->nativeformats;
	dst = to->nativeformats;

	/* If there's no audio in this call, don't bother with trying to find a translation path */
	if ((src & TRIS_FORMAT_AUDIO_MASK) == 0 || (dst & TRIS_FORMAT_AUDIO_MASK) == 0)
		return 0;

	if (tris_translator_best_choice(&dst, &src) < 0) {
		tris_log(LOG_WARNING, "No path to translate from %s(%d) to %s(%d)\n", from->name, src, to->name, dst);
		return -1;
	}

	/* if the best path is not 'pass through', then
	   transcoding is needed; if desired, force transcode path
	   to use SLINEAR between channels, but only if there is
	   no direct conversion available */
	if ((src != dst) && tris_opt_transcode_via_slin &&
	    (tris_translate_path_steps(dst, src) != 1))
		dst = TRIS_FORMAT_SLINEAR;
	if (tris_set_read_format(from, dst) < 0) {
		tris_log(LOG_WARNING, "Unable to set read format on channel %s to %d\n", from->name, dst);
		return -1;
	}
	if (tris_set_write_format(to, dst) < 0) {
		tris_log(LOG_WARNING, "Unable to set write format on channel %s to %d\n", to->name, dst);
		return -1;
	}
	return 0;
}

int tris_channel_make_compatible(struct tris_channel *chan, struct tris_channel *peer)
{
	/* Some callers do not check return code, and we must try to set all call legs correctly */
	int rc = 0;

	/* Set up translation from the chan to the peer */
	rc = tris_channel_make_compatible_helper(chan, peer);

	if (rc < 0)
		return rc;

	/* Set up translation from the peer to the chan */
	rc = tris_channel_make_compatible_helper(peer, chan);

	return rc;
}

int tris_channel_masquerade(struct tris_channel *original, struct tris_channel *clonechan)
{
	int res = -1;
	struct tris_channel *final_orig, *final_clone, *base;

retrymasq:
	final_orig = original;
	final_clone = clonechan;

	tris_channel_lock(original);
	while (tris_channel_trylock(clonechan)) {
		tris_channel_unlock(original);
		usleep(1);
		tris_channel_lock(original);
	}

	/* each of these channels may be sitting behind a channel proxy (i.e. chan_agent)
	   and if so, we don't really want to masquerade it, but its proxy */
	if (original->_bridge && (original->_bridge != tris_bridged_channel(original)) && (original->_bridge->_bridge != original))
		final_orig = original->_bridge;

	if (clonechan->_bridge && (clonechan->_bridge != tris_bridged_channel(clonechan)) && (clonechan->_bridge->_bridge != clonechan))
		final_clone = clonechan->_bridge;
	
	if (final_clone->tech->get_base_channel && (base = final_clone->tech->get_base_channel(final_clone))) {
		final_clone = base;
	}

	if ((final_orig != original) || (final_clone != clonechan)) {
		/* Lots and lots of deadlock avoidance.  The main one we're competing with
		 * is tris_write(), which locks channels recursively, when working with a
		 * proxy channel. */
		if (tris_channel_trylock(final_orig)) {
			tris_channel_unlock(clonechan);
			tris_channel_unlock(original);
			goto retrymasq;
		}
		if (tris_channel_trylock(final_clone)) {
			tris_channel_unlock(final_orig);
			tris_channel_unlock(clonechan);
			tris_channel_unlock(original);
			goto retrymasq;
		}
		tris_channel_unlock(clonechan);
		tris_channel_unlock(original);
		original = final_orig;
		clonechan = final_clone;
	}

	if (original == clonechan) {
		tris_log(LOG_WARNING, "Can't masquerade channel '%s' into itself!\n", original->name);
		tris_channel_unlock(clonechan);
		tris_channel_unlock(original);
		return -1;
	}

	tris_debug(1, "Planning to masquerade channel %s into the structure of %s\n",
		clonechan->name, original->name);
	if (original->masq) {
		tris_log(LOG_WARNING, "%s is already going to masquerade as %s\n",
			original->masq->name, original->name);
	} else if (clonechan->masqr) {
		tris_log(LOG_WARNING, "%s is already going to masquerade as %s\n",
			clonechan->name, clonechan->masqr->name);
	} else {
		original->masq = clonechan;
		clonechan->masqr = original;
		tris_queue_frame(original, &tris_null_frame);
		tris_queue_frame(clonechan, &tris_null_frame);
		tris_debug(1, "Done planning to masquerade channel %s into the structure of %s\n", clonechan->name, original->name);
		res = 0;
	}

	tris_channel_unlock(clonechan);
	tris_channel_unlock(original);

	return res;
}

void tris_change_name(struct tris_channel *chan, char *newname)
{
	manager_event(EVENT_FLAG_CALL, "Rename", "Channel: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", chan->name, newname, chan->uniqueid);
	tris_string_field_set(chan, name, newname);
}

void tris_channel_inherit_variables(const struct tris_channel *parent, struct tris_channel *child)
{
	struct tris_var_t *current, *newvar;
	const char *varname;

	TRIS_LIST_TRAVERSE(&parent->varshead, current, entries) {
		int vartype = 0;

		varname = tris_var_full_name(current);
		if (!varname)
			continue;

		if (varname[0] == '_') {
			vartype = 1;
			if (varname[1] == '_')
				vartype = 2;
		}

		switch (vartype) {
		case 1:
			newvar = tris_var_assign(&varname[1], tris_var_value(current));
			if (newvar) {
				TRIS_LIST_INSERT_TAIL(&child->varshead, newvar, entries);
				tris_debug(1, "Copying soft-transferable variable %s.\n", tris_var_name(newvar));
			}
			break;
		case 2:
			newvar = tris_var_assign(varname, tris_var_value(current));
			if (newvar) {
				TRIS_LIST_INSERT_TAIL(&child->varshead, newvar, entries);
				tris_debug(1, "Copying hard-transferable variable %s.\n", tris_var_name(newvar));
			}
			break;
		default:
			tris_debug(1, "Not copying variable %s.\n", tris_var_name(current));
			break;
		}
	}
}

/*!
  \brief Clone channel variables from 'clone' channel into 'original' channel

  All variables except those related to app_groupcount are cloned.
  Variables are actually _removed_ from 'clone' channel, presumably
  because it will subsequently be destroyed.

  \note Assumes locks will be in place on both channels when called.
*/
static void clone_variables(struct tris_channel *original, struct tris_channel *clonechan)
{
	struct tris_var_t *current, *newvar;
	/* Append variables from clone channel into original channel */
	/* XXX Is this always correct?  We have to in order to keep MACROS working XXX */
	if (TRIS_LIST_FIRST(&clonechan->varshead))
		TRIS_LIST_APPEND_LIST(&original->varshead, &clonechan->varshead, entries);

	/* then, dup the varshead list into the clone */
	
	TRIS_LIST_TRAVERSE(&original->varshead, current, entries) {
		newvar = tris_var_assign(current->name, current->value);
		if (newvar)
			TRIS_LIST_INSERT_TAIL(&clonechan->varshead, newvar, entries);
	}
}

/*!
 * \pre chan is locked
 */
static void report_new_callerid(const struct tris_channel *chan)
{
	manager_event(EVENT_FLAG_CALL, "NewCallerid",
				"Channel: %s\r\n"
				"CallerIDNum: %s\r\n"
				"CallerIDName: %s\r\n"
				"Uniqueid: %s\r\n"
				"CID-CallingPres: %d (%s)\r\n",
				chan->name,
				S_OR(chan->cid.cid_num, ""),
				S_OR(chan->cid.cid_name, ""),
				chan->uniqueid,
				chan->cid.cid_pres,
				tris_describe_caller_presentation(chan->cid.cid_pres)
				);
}

/*!
  \brief Masquerade a channel

  \note Assumes channel will be locked when called
*/
int tris_do_masquerade(struct tris_channel *original)
{
	int x,i;
	int res=0;
	int origstate;
	struct tris_frame *current;
	const struct tris_channel_tech *t;
	void *t_pvt;
	struct tris_callerid tmpcid;
	struct tris_channel *clonechan = original->masq;
	struct tris_channel *bridged;
	struct tris_cdr *cdr;
	int rformat = original->readformat;
	int wformat = original->writeformat;
	char newn[TRIS_CHANNEL_NAME];
	char orig[TRIS_CHANNEL_NAME];
	char masqn[TRIS_CHANNEL_NAME];
	char zombn[TRIS_CHANNEL_NAME];

	tris_debug(4, "Actually Masquerading %s(%d) into the structure of %s(%d)\n",
		clonechan->name, clonechan->_state, original->name, original->_state);

	manager_event(EVENT_FLAG_CALL, "Masquerade", "Clone: %s\r\nCloneState: %s\r\nOriginal: %s\r\nOriginalState: %s\r\n",
		      clonechan->name, tris_state2str(clonechan->_state), original->name, tris_state2str(original->_state));

	/* XXX This operation is a bit odd.  We're essentially putting the guts of
	 * the clone channel into the original channel.  Start by killing off the
	 * original channel's backend.  While the features are nice, which is the
	 * reason we're keeping it, it's still awesomely weird. XXX */

	/* We need the clone's lock, too */
	tris_channel_lock(clonechan);

	tris_debug(2, "Got clone lock for masquerade on '%s' at %p\n", clonechan->name, &clonechan->lock_dont_use);

	/* Having remembered the original read/write formats, we turn off any translation on either
	   one */
	free_translation(clonechan);
	free_translation(original);


	/* Unlink the masquerade */
	original->masq = NULL;
	clonechan->masqr = NULL;
	
	/* Save the original name */
	tris_copy_string(orig, original->name, sizeof(orig));
	/* Save the new name */
	tris_copy_string(newn, clonechan->name, sizeof(newn));
	/* Create the masq name */
	snprintf(masqn, sizeof(masqn), "%s<MASQ>", newn);
		
	/* Copy the name from the clone channel */
	tris_string_field_set(original, name, newn);

	/* Mangle the name of the clone channel */
	tris_string_field_set(clonechan, name, masqn);
	
	/* Notify any managers of the change, first the masq then the other */
	manager_event(EVENT_FLAG_CALL, "Rename", "Channel: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", newn, masqn, clonechan->uniqueid);
	manager_event(EVENT_FLAG_CALL, "Rename", "Channel: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", orig, newn, original->uniqueid);

	/* Swap the technologies */	
	t = original->tech;
	original->tech = clonechan->tech;
	clonechan->tech = t;

	/* Swap the cdrs */
	cdr = original->cdr;
	original->cdr = clonechan->cdr;
	clonechan->cdr = cdr;

	t_pvt = original->tech_pvt;
	original->tech_pvt = clonechan->tech_pvt;
	clonechan->tech_pvt = t_pvt;

	/* Swap the alertpipes */
	for (i = 0; i < 2; i++) {
		x = original->alertpipe[i];
		original->alertpipe[i] = clonechan->alertpipe[i];
		clonechan->alertpipe[i] = x;
	}

	/* 
	 * Swap the readq's.  The end result should be this:
	 *
	 *  1) All frames should be on the new (original) channel.
	 *  2) Any frames that were already on the new channel before this
	 *     masquerade need to be at the end of the readq, after all of the
	 *     frames on the old (clone) channel.
	 *  3) The alertpipe needs to get poked for every frame that was already
	 *     on the new channel, since we are now using the alert pipe from the
	 *     old (clone) channel.
	 */
	{
		TRIS_LIST_HEAD_NOLOCK(, tris_frame) tmp_readq;
		TRIS_LIST_HEAD_SET_NOLOCK(&tmp_readq, NULL);

		TRIS_LIST_APPEND_LIST(&tmp_readq, &original->readq, frame_list);
		TRIS_LIST_APPEND_LIST(&original->readq, &clonechan->readq, frame_list);

		while ((current = TRIS_LIST_REMOVE_HEAD(&tmp_readq, frame_list))) {
			TRIS_LIST_INSERT_TAIL(&original->readq, current, frame_list);
			if (original->alertpipe[1] > -1) {
				int poke = 0;

				if (write(original->alertpipe[1], &poke, sizeof(poke)) < 0) {
					tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
				}
			}
		}
	}

	/* Swap the raw formats */
	x = original->rawreadformat;
	original->rawreadformat = clonechan->rawreadformat;
	clonechan->rawreadformat = x;
	x = original->rawwriteformat;
	original->rawwriteformat = clonechan->rawwriteformat;
	clonechan->rawwriteformat = x;

	clonechan->_softhangup = TRIS_SOFTHANGUP_DEV;

	/* And of course, so does our current state.  Note we need not
	   call tris_setstate since the event manager doesn't really consider
	   these separate.  We do this early so that the clone has the proper
	   state of the original channel. */
	origstate = original->_state;
	original->_state = clonechan->_state;
	clonechan->_state = origstate;

	if (clonechan->tech->fixup){
		res = clonechan->tech->fixup(original, clonechan);
		if (res)
			tris_log(LOG_WARNING, "Fixup failed on channel %s, strange things may happen.\n", clonechan->name);
	}

	/* Start by disconnecting the original's physical side */
	if (clonechan->tech->hangup)
		res = clonechan->tech->hangup(clonechan);
	if (res) {
		tris_log(LOG_WARNING, "Hangup failed!  Strange things may happen!\n");
		tris_channel_unlock(clonechan);
		return -1;
	}

	snprintf(zombn, sizeof(zombn), "%s<ZOMBIE>", orig);
	/* Mangle the name of the clone channel */
	tris_string_field_set(clonechan, name, zombn);
	manager_event(EVENT_FLAG_CALL, "Rename", "Channel: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", masqn, zombn, clonechan->uniqueid);

	/* Update the type. */
	t_pvt = original->monitor;
	original->monitor = clonechan->monitor;
	clonechan->monitor = t_pvt;

	/* Keep the same language.  */
	tris_string_field_set(original, language, clonechan->language);
	/* Copy the FD's other than the generator fd */
	for (x = 0; x < TRIS_MAX_FDS; x++) {
		if (x != TRIS_GENERATOR_FD)
			tris_channel_set_fd(original, x, clonechan->fds[x]);
	}

	tris_app_group_update(clonechan, original);

	/* Move data stores over */
	if (TRIS_LIST_FIRST(&clonechan->datastores)) {
		struct tris_datastore *ds;
		/* We use a safe traversal here because some fixup routines actually
		 * remove the datastore from the list and free them.
		 */
		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&clonechan->datastores, ds, entry) {
			if (ds->info->chan_fixup)
				ds->info->chan_fixup(ds->data, clonechan, original);
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
		TRIS_LIST_APPEND_LIST(&original->datastores, &clonechan->datastores, entry);
	}

	clone_variables(original, clonechan);
	/* Presense of ADSI capable CPE follows clone */
	original->adsicpe = clonechan->adsicpe;
	/* Bridge remains the same */
	/* CDR fields remain the same */
	/* XXX What about blocking, softhangup, blocker, and lock and blockproc? XXX */
	/* Application and data remain the same */
	/* Clone exception  becomes real one, as with fdno */
	tris_set_flag(original, tris_test_flag(clonechan, TRIS_FLAG_OUTGOING | TRIS_FLAG_EXCEPTION));
	original->fdno = clonechan->fdno;
	/* Schedule context remains the same */
	/* Stream stuff stays the same */
	/* Keep the original state.  The fixup code will need to work with it most likely */

	/* Just swap the whole structures, nevermind the allocations, they'll work themselves
	   out. */
	tmpcid = original->cid;
	original->cid = clonechan->cid;
	clonechan->cid = tmpcid;
	report_new_callerid(original);

	/* Restore original timing file descriptor */
	tris_channel_set_fd(original, TRIS_TIMING_FD, original->timingfd);

	/* Our native formats are different now */
	original->nativeformats = clonechan->nativeformats;

	/* Context, extension, priority, app data, jump table,  remain the same */
	/* pvt switches.  pbx stays the same, as does next */

	/* Set the write format */
	tris_set_write_format(original, wformat);

	/* Set the read format */
	tris_set_read_format(original, rformat);

	/* Copy the music class */
	tris_string_field_set(original, musicclass, clonechan->musicclass);

	tris_debug(1, "Putting channel %s in %d/%d formats\n", original->name, wformat, rformat);

	/* Okay.  Last thing is to let the channel driver know about all this mess, so he
	   can fix up everything as best as possible */
	if (original->tech->fixup) {
		res = original->tech->fixup(clonechan, original);
		if (res) {
			tris_log(LOG_WARNING, "Channel for type '%s' could not fixup channel %s\n",
				original->tech->type, original->name);
			tris_channel_unlock(clonechan);
			return -1;
		}
	} else
		tris_log(LOG_WARNING, "Channel type '%s' does not have a fixup routine (for %s)!  Bad things may happen.\n",
			original->tech->type, original->name);

	/* 
	 * If an indication is currently playing, maintain it on the channel 
	 * that is taking the place of original 
	 *
	 * This is needed because the masquerade is swapping out in the internals
	 * of this channel, and the new channel private data needs to be made
	 * aware of the current visible indication (RINGING, CONGESTION, etc.)
	 */
	if (original->visible_indication) {
		tris_indicate(original, original->visible_indication);
	}
	
	/* Now, at this point, the "clone" channel is totally F'd up.  We mark it as
	   a zombie so nothing tries to touch it.  If it's already been marked as a
	   zombie, then free it now (since it already is considered invalid). */
	if (tris_test_flag(clonechan, TRIS_FLAG_ZOMBIE)) {
		tris_debug(1, "Destroying channel clone '%s'\n", clonechan->name);
		tris_channel_unlock(clonechan);
		manager_event(EVENT_FLAG_CALL, "Hangup",
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"Cause: %d\r\n"
			"Cause-txt: %s\r\n",
			clonechan->name,
			clonechan->uniqueid,
			clonechan->hangupcause,
			tris_cause2str(clonechan->hangupcause)
			);
		tris_channel_free(clonechan);
	} else {
		tris_debug(1, "Released clone lock on '%s'\n", clonechan->name);
		tris_set_flag(clonechan, TRIS_FLAG_ZOMBIE);
		tris_queue_frame(clonechan, &tris_null_frame);
		tris_channel_unlock(clonechan);
	}

	/* Signal any blocker */
	if (tris_test_flag(original, TRIS_FLAG_BLOCKING))
		pthread_kill(original->blocker, SIGURG);
	tris_debug(1, "Done Masquerading %s (%d)\n", original->name, original->_state);
	if ((bridged = tris_bridged_channel(original))) {
		tris_channel_lock(bridged);
		tris_indicate(bridged, TRIS_CONTROL_SRCCHANGE);
		tris_channel_unlock(bridged);
	}

	tris_indicate(original, TRIS_CONTROL_SRCCHANGE);

	return 0;
}

void tris_set_callerid(struct tris_channel *chan, const char *cid_num, const char *cid_name, const char *cid_ani)
{
	tris_channel_lock(chan);

	if (cid_num) {
		if (chan->cid.cid_num)
			tris_free(chan->cid.cid_num);
		chan->cid.cid_num = tris_strdup(cid_num);
	}
	if (cid_name) {
		if (chan->cid.cid_name)
			tris_free(chan->cid.cid_name);
		chan->cid.cid_name = tris_strdup(cid_name);
	}
	if (cid_ani) {
		if (chan->cid.cid_ani)
			tris_free(chan->cid.cid_ani);
		chan->cid.cid_ani = tris_strdup(cid_ani);
	}

	report_new_callerid(chan);

	tris_channel_unlock(chan);
}

int tris_setstate(struct tris_channel *chan, enum tris_channel_state state)
{
	int oldstate = chan->_state;
	char name[TRIS_CHANNEL_NAME], *dashptr;

	if (oldstate == state)
		return 0;

	tris_copy_string(name, chan->name, sizeof(name));
	if ((dashptr = strrchr(name, '-'))) {
		*dashptr = '\0';
	}

	chan->_state = state;

	/* We have to pass TRIS_DEVICE_UNKNOWN here because it is entirely possible that the channel driver
	 * for this channel is using the callback method for device state. If we pass in an actual state here
	 * we override what they are saying the state is and things go amuck. */
	tris_devstate_changed_literal(TRIS_DEVICE_UNKNOWN, name);

	/* setstate used to conditionally report Newchannel; this is no more */
	manager_event(EVENT_FLAG_CALL,
		      "Newstate",
		      "Channel: %s\r\n"
		      "ChannelState: %d\r\n"
		      "ChannelStateDesc: %s\r\n"
		      "CallerIDNum: %s\r\n"
		      "CallerIDName: %s\r\n"
		      "Uniqueid: %s\r\n",
		      chan->name, chan->_state, tris_state2str(chan->_state),
		      S_OR(chan->cid.cid_num, ""),
		      S_OR(chan->cid.cid_name, ""),
		      chan->uniqueid);

	return 0;
}

/*! \brief Find bridged channel */
struct tris_channel *tris_bridged_channel(struct tris_channel *chan)
{
	struct tris_channel *bridged;
	bridged = chan->_bridge;
	if (bridged && bridged->tech->bridged_channel)
		bridged = bridged->tech->bridged_channel(chan, bridged);
	return bridged;
}

static void bridge_playfile(struct tris_channel *chan, struct tris_channel *peer, const char *sound, int remain)
{
	int min = 0, sec = 0, check;

	check = tris_autoservice_start(peer);
	if (check)
		return;

	if (remain > 0) {
		if (remain / 60 > 1) {
			min = remain / 60;
			sec = remain % 60;
		} else {
			sec = remain;
		}
	}
	
	if (!strcmp(sound,"timeleft")) {	/* Queue support */
		tris_stream_and_wait(chan, "voicemail/vm-youhave", "");
		if (min) {
			tris_say_number(chan, min, TRIS_DIGIT_ANY, chan->language, NULL);
			tris_stream_and_wait(chan, "queue-minutes", "");
		}
		if (sec) {
			tris_say_number(chan, sec, TRIS_DIGIT_ANY, chan->language, NULL);
			tris_stream_and_wait(chan, "queue-seconds", "");
		}
	} else {
		tris_stream_and_wait(chan, sound, "");
	}

	tris_autoservice_stop(peer);
}

static enum tris_bridge_result tris_generic_bridge(struct tris_channel *c0, struct tris_channel *c1,
						 struct tris_bridge_config *config, struct tris_frame **fo,
						 struct tris_channel **rc, struct timeval bridge_end)
{
	/* Copy voice back and forth between the two channels. */
	struct tris_channel *cs[3];
	struct tris_frame *f;
	enum tris_bridge_result res = TRIS_BRIDGE_COMPLETE;
	int o0nativeformats;
	int o1nativeformats;
	int watch_c0_dtmf;
	int watch_c1_dtmf;
	void *pvt0, *pvt1;
	/* Indicates whether a frame was queued into a jitterbuffer */
	int frame_put_in_jb = 0;
	int jb_in_use;
	int to;
	
	cs[0] = c0;
	cs[1] = c1;
	pvt0 = c0->tech_pvt;
	pvt1 = c1->tech_pvt;
	o0nativeformats = c0->nativeformats;
	o1nativeformats = c1->nativeformats;
	watch_c0_dtmf = config->flags & TRIS_BRIDGE_DTMF_CHANNEL_0;
	watch_c1_dtmf = config->flags & TRIS_BRIDGE_DTMF_CHANNEL_1;

	/* Check the need of a jitterbuffer for each channel */
	jb_in_use = tris_jb_do_usecheck(c0, c1);
	if (jb_in_use)
		tris_jb_empty_and_reset(c0, c1);

	tris_poll_channel_add(c0, c1);

	if (config->feature_timer > 0 && tris_tvzero(config->nexteventts)) {
		/* calculate when the bridge should possibly break
 		 * if a partial feature match timed out */
		config->partialfeature_timer = tris_tvadd(tris_tvnow(), tris_samp2tv(config->feature_timer, 1000));
	} else {
		memset(&config->partialfeature_timer, 0, sizeof(config->partialfeature_timer));
	}

	for (;;) {
		struct tris_channel *who, *other;

		if ((c0->tech_pvt != pvt0) || (c1->tech_pvt != pvt1) ||
		    (o0nativeformats != c0->nativeformats) ||
		    (o1nativeformats != c1->nativeformats)) {
			/* Check for Masquerade, codec changes, etc */
			res = TRIS_BRIDGE_RETRY;
			break;
		}
		if (bridge_end.tv_sec) {
			to = tris_tvdiff_ms(bridge_end, tris_tvnow());
			if (to <= 0) {
				if (config->timelimit) {
					res = TRIS_BRIDGE_RETRY;
					/* generic bridge ending to play warning */
					tris_set_flag(config, TRIS_FEATURE_WARNING_ACTIVE);
				} else {
					res = TRIS_BRIDGE_COMPLETE;
				}
				break;
			}
		} else {
			/* If a feature has been started and the bridge is configured to 
 			 * to not break, leave the channel bridge when the feature timer
			 * time has elapsed so the DTMF will be sent to the other side. 
 			 */
			if (!tris_tvzero(config->partialfeature_timer)) {
				int diff = tris_tvdiff_ms(config->partialfeature_timer, tris_tvnow());
				if (diff <= 0) {
					res = TRIS_BRIDGE_RETRY;
					break;
				}
			}
			to = -1;
		}
		/* Calculate the appropriate max sleep interval - in general, this is the time,
		   left to the closest jb delivery moment */
		if (jb_in_use)
			to = tris_jb_get_when_to_wakeup(c0, c1, to);
		who = tris_waitfor_n(cs, 2, &to);
		if (!who) {
			/* No frame received within the specified timeout - check if we have to deliver now */
			if (jb_in_use)
				tris_jb_get_and_deliver(c0, c1);
			if (c0->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE || c1->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE) {
				if (c0->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE)
					c0->_softhangup = 0;
				if (c1->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE)
					c1->_softhangup = 0;
				c0->_bridge = c1;
				c1->_bridge = c0;
			}
			continue;
		}
		f = tris_read(who);
		if (!f) {
			*fo = NULL;
			*rc = who;
			tris_debug(1, "Didn't get a frame from channel: %s\n",who->name);
			break;
		}

		other = (who == c0) ? c1 : c0; /* the 'other' channel */
		/* Try add the frame info the who's bridged channel jitterbuff */
		if (jb_in_use)
			frame_put_in_jb = !tris_jb_put(other, f);

		if ((f->frametype == TRIS_FRAME_CONTROL) && !(config->flags & TRIS_BRIDGE_IGNORE_SIGS)) {
			int bridge_exit = 0;

			switch (f->subclass) {
			case TRIS_CONTROL_HOLD:
			case TRIS_CONTROL_UNHOLD:
			case TRIS_CONTROL_VIDUPDATE:
			case TRIS_CONTROL_SRCUPDATE:
			case TRIS_CONTROL_SRCCHANGE:
			case TRIS_CONTROL_T38_PARAMETERS:
				tris_indicate_data(other, f->subclass, f->data.ptr, f->datalen);
				if (jb_in_use) {
					tris_jb_empty_and_reset(c0, c1);
				}
				break;
			default:
				*fo = f;
				*rc = who;
				bridge_exit = 1;
				tris_debug(1, "Got a FRAME_CONTROL (%d) frame on channel %s\n", f->subclass, who->name);
				break;
			}
			if (bridge_exit)
				break;
		}
		if ((f->frametype == TRIS_FRAME_VOICE) ||
		    (f->frametype == TRIS_FRAME_DTMF_BEGIN) ||
		    (f->frametype == TRIS_FRAME_DTMF) ||
		    (f->frametype == TRIS_FRAME_VIDEO) ||
		    (f->frametype == TRIS_FRAME_IMAGE) ||
		    (f->frametype == TRIS_FRAME_HTML) ||
		    (f->frametype == TRIS_FRAME_MODEM) ||
		    (f->frametype == TRIS_FRAME_TEXT) ||
		    (f->frametype == TRIS_FRAME_FILE) ||
		    (f->frametype == TRIS_FRAME_DESKTOP) ||
		    (f->frametype == TRIS_FRAME_CHAT)) {
			/* monitored dtmf causes exit from bridge */
			int monitored_source = (who == c0) ? watch_c0_dtmf : watch_c1_dtmf;

			if (monitored_source && 
				(f->frametype == TRIS_FRAME_DTMF_END || 
				f->frametype == TRIS_FRAME_DTMF_BEGIN)) {
				*fo = f;
				*rc = who;
				tris_debug(1, "Got DTMF %s on channel (%s)\n", 
					f->frametype == TRIS_FRAME_DTMF_END ? "end" : "begin",
					who->name);

				break;
			}
			/* Write immediately frames, not passed through jb */
			if (!frame_put_in_jb)
				tris_write(other, f);
				
			/* Check if we have to deliver now */
			if (jb_in_use)
				tris_jb_get_and_deliver(c0, c1);
		}
		/* XXX do we want to pass on also frames not matched above ? */
		tris_frfree(f);

#ifndef HAVE_EPOLL
		/* Swap who gets priority */
		cs[2] = cs[0];
		cs[0] = cs[1];
		cs[1] = cs[2];
#endif
	}

	tris_poll_channel_del(c0, c1);

	return res;
}

/*! \brief Bridge two channels together (early) */
int tris_channel_early_bridge(struct tris_channel *c0, struct tris_channel *c1)
{
	/* Make sure we can early bridge, if not error out */
	if (!c0->tech->early_bridge || (c1 && (!c1->tech->early_bridge || c0->tech->early_bridge != c1->tech->early_bridge)))
		return -1;

	return c0->tech->early_bridge(c0, c1);
}

/*! \brief Send manager event for bridge link and unlink events.
 * \param onoff Link/Unlinked 
 * \param type 1 for core, 2 for native
 * \param c0 first channel in bridge
 * \param c1 second channel in bridge
*/
static void manager_bridge_event(int onoff, int type, struct tris_channel *c0, struct tris_channel *c1)
{
	manager_event(EVENT_FLAG_CALL, "Bridge",
			"Bridgestate: %s\r\n"
		     "Bridgetype: %s\r\n"
		      "Channel1: %s\r\n"
		      "Channel2: %s\r\n"
		      "Uniqueid1: %s\r\n"
		      "Uniqueid2: %s\r\n"
		      "CallerID1: %s\r\n"
		      "CallerID2: %s\r\n",
			onoff ? "Link" : "Unlink",
			type == 1 ? "core" : "native",
			c0->name, c1->name, c0->uniqueid, c1->uniqueid, 
			S_OR(c0->cid.cid_num, ""), 
			S_OR(c1->cid.cid_num, ""));
}

static void update_bridge_vars(struct tris_channel *c0, struct tris_channel *c1)
{
	const char *c0_name;
	const char *c1_name;
	const char *c0_pvtid = NULL;
	const char *c1_pvtid = NULL;

	tris_channel_lock(c1);
	c1_name = tris_strdupa(c1->name);
	if (c1->tech->get_pvt_uniqueid) {
		c1_pvtid = tris_strdupa(c1->tech->get_pvt_uniqueid(c1));
	}
	tris_channel_unlock(c1);

	tris_channel_lock(c0);
	if (!tris_strlen_zero(pbx_builtin_getvar_helper(c0, "BRIDGEPEER"))) {
		pbx_builtin_setvar_helper(c0, "BRIDGEPEER", c1_name);
	}
	if (c1_pvtid) {
		pbx_builtin_setvar_helper(c0, "BRIDGEPVTCALLID", c1_pvtid);
	}
	c0_name = tris_strdupa(c0->name);
	if (c0->tech->get_pvt_uniqueid) {
		c0_pvtid = tris_strdupa(c0->tech->get_pvt_uniqueid(c0));
	}
	tris_channel_unlock(c0);

	tris_channel_lock(c1);
	if (!tris_strlen_zero(pbx_builtin_getvar_helper(c1, "BRIDGEPEER"))) {
		pbx_builtin_setvar_helper(c1, "BRIDGEPEER", c0_name);
	}
	if (c0_pvtid) {
		pbx_builtin_setvar_helper(c1, "BRIDGEPVTCALLID", c0_pvtid);
	}
	tris_channel_unlock(c1);
}

static void bridge_play_sounds(struct tris_channel *c0, struct tris_channel *c1)
{
	const char *s, *sound;

	/* See if we need to play an audio file to any side of the bridge */

	tris_channel_lock(c0);
	if ((s = pbx_builtin_getvar_helper(c0, "BRIDGE_PLAY_SOUND"))) {
		sound = tris_strdupa(s);
		tris_channel_unlock(c0);
		bridge_playfile(c0, c1, sound, 0);
		pbx_builtin_setvar_helper(c0, "BRIDGE_PLAY_SOUND", NULL);
	} else {
		tris_channel_unlock(c0);
	}

	tris_channel_lock(c1);
	if ((s = pbx_builtin_getvar_helper(c1, "BRIDGE_PLAY_SOUND"))) {
		sound = tris_strdupa(s);
		tris_channel_unlock(c1);
		bridge_playfile(c1, c0, sound, 0);
		pbx_builtin_setvar_helper(c1, "BRIDGE_PLAY_SOUND", NULL);
	} else {
		tris_channel_unlock(c1);
	}
}

/*! \brief Bridge two channels together */
enum tris_bridge_result tris_channel_bridge(struct tris_channel *c0, struct tris_channel *c1,
					  struct tris_bridge_config *config, struct tris_frame **fo, struct tris_channel **rc)
{
	struct tris_channel *who = NULL;
	enum tris_bridge_result res = TRIS_BRIDGE_COMPLETE;
	int nativefailed=0;
	int firstpass;
	int o0nativeformats;
	int o1nativeformats;
	long time_left_ms=0;
	char caller_warning = 0;
	char callee_warning = 0;

	if (c0->_bridge) {
		tris_log(LOG_WARNING, "%s is already in a bridge with %s\n",
			c0->name, c0->_bridge->name);
		return -1;
	}
	if (c1->_bridge) {
		tris_log(LOG_WARNING, "%s is already in a bridge with %s\n",
			c1->name, c1->_bridge->name);
		return -1;
	}
	
	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(c0, TRIS_FLAG_ZOMBIE) || tris_check_hangup_locked(c0) ||
	    tris_test_flag(c1, TRIS_FLAG_ZOMBIE) || tris_check_hangup_locked(c1))
		return -1;

	*fo = NULL;
	firstpass = config->firstpass;
	config->firstpass = 0;

	if (tris_tvzero(config->start_time))
		config->start_time = tris_tvnow();
	time_left_ms = config->timelimit;

	caller_warning = tris_test_flag(&config->features_caller, TRIS_FEATURE_PLAY_WARNING);
	callee_warning = tris_test_flag(&config->features_callee, TRIS_FEATURE_PLAY_WARNING);

	if (config->start_sound && firstpass) {
		if (caller_warning)
			bridge_playfile(c0, c1, config->start_sound, time_left_ms / 1000);
		if (callee_warning)
			bridge_playfile(c1, c0, config->start_sound, time_left_ms / 1000);
	}

	/* Keep track of bridge */
	c0->_bridge = c1;
	c1->_bridge = c0;


	o0nativeformats = c0->nativeformats;
	o1nativeformats = c1->nativeformats;

	if (config->feature_timer && !tris_tvzero(config->nexteventts)) {
		config->nexteventts = tris_tvadd(config->start_time, tris_samp2tv(config->feature_timer, 1000));
	} else if (config->timelimit && firstpass) {
		config->nexteventts = tris_tvadd(config->start_time, tris_samp2tv(config->timelimit, 1000));
		if (caller_warning || callee_warning)
			config->nexteventts = tris_tvsub(config->nexteventts, tris_samp2tv(config->play_warning, 1000));
	}

	if (!c0->tech->send_digit_begin)
		tris_set_flag(c1, TRIS_FLAG_END_DTMF_ONLY);
	if (!c1->tech->send_digit_begin)
		tris_set_flag(c0, TRIS_FLAG_END_DTMF_ONLY);
	manager_bridge_event(1, 1, c0, c1);

	/* Before we enter in and bridge these two together tell them both the source of audio has changed */
	tris_indicate(c0, TRIS_CONTROL_SRCUPDATE);
	tris_indicate(c1, TRIS_CONTROL_SRCUPDATE);

	for (/* ever */;;) {
		struct timeval now = { 0, };
		int to;

		to = -1;

		if (!tris_tvzero(config->nexteventts)) {
			now = tris_tvnow();
			to = tris_tvdiff_ms(config->nexteventts, now);
			if (to <= 0) {
				if (!config->timelimit) {
					res = TRIS_BRIDGE_COMPLETE;
					break;
				}
				to = 0;
			}
		}

		if (config->timelimit) {
			time_left_ms = config->timelimit - tris_tvdiff_ms(now, config->start_time);
			if (time_left_ms < to)
				to = time_left_ms;

			if (time_left_ms <= 0) {
				if (caller_warning && config->end_sound)
					bridge_playfile(c0, c1, config->end_sound, 0);
				if (callee_warning && config->end_sound)
					bridge_playfile(c1, c0, config->end_sound, 0);
				*fo = NULL;
				if (who)
					*rc = who;
				res = 0;
				break;
			}

			if (!to) {
				if (time_left_ms >= 5000 && config->warning_sound && config->play_warning && tris_test_flag(config, TRIS_FEATURE_WARNING_ACTIVE)) {
					int t = (time_left_ms + 500) / 1000; /* round to nearest second */
					if (caller_warning)
						bridge_playfile(c0, c1, config->warning_sound, t);
					if (callee_warning)
						bridge_playfile(c1, c0, config->warning_sound, t);
				}
				if (config->warning_freq && (time_left_ms > (config->warning_freq + 5000)))
					config->nexteventts = tris_tvadd(config->nexteventts, tris_samp2tv(config->warning_freq, 1000));
				else
					config->nexteventts = tris_tvadd(config->start_time, tris_samp2tv(config->timelimit, 1000));
			}
			tris_clear_flag(config, TRIS_FEATURE_WARNING_ACTIVE);
		}

		if (c0->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE || c1->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE) {
			if (c0->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE)
				c0->_softhangup = 0;
			if (c1->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE)
				c1->_softhangup = 0;
			c0->_bridge = c1;
			c1->_bridge = c0;
			tris_debug(1, "Unbridge signal received. Ending native bridge.\n");
			continue;
		}

		/* Stop if we're a zombie or need a soft hangup */
		if (tris_test_flag(c0, TRIS_FLAG_ZOMBIE) || tris_check_hangup_locked(c0) ||
		    tris_test_flag(c1, TRIS_FLAG_ZOMBIE) || tris_check_hangup_locked(c1)) {
			*fo = NULL;
			if (who)
				*rc = who;
			res = 0;
			tris_debug(1, "Bridge stops because we're zombie or need a soft hangup: c0=%s, c1=%s, flags: %s,%s,%s,%s\n",
				c0->name, c1->name,
				tris_test_flag(c0, TRIS_FLAG_ZOMBIE) ? "Yes" : "No",
				tris_check_hangup(c0) ? "Yes" : "No",
				tris_test_flag(c1, TRIS_FLAG_ZOMBIE) ? "Yes" : "No",
				tris_check_hangup(c1) ? "Yes" : "No");
			break;
		}

		update_bridge_vars(c0, c1);

		bridge_play_sounds(c0, c1);

		if (c0->tech->bridge &&
		    (c0->tech->bridge == c1->tech->bridge) &&
		    !nativefailed && !c0->monitor && !c1->monitor &&
		    !c0->audiohooks && !c1->audiohooks && 
		    !c0->masq && !c0->masqr && !c1->masq && !c1->masqr) {
			/* Looks like they share a bridge method and nothing else is in the way */
			tris_set_flag(c0, TRIS_FLAG_NBRIDGE);
			tris_set_flag(c1, TRIS_FLAG_NBRIDGE);
			if ((res = c0->tech->bridge(c0, c1, config->flags, fo, rc, to)) == TRIS_BRIDGE_COMPLETE) {
				/* \todo  XXX here should check that cid_num is not NULL */
				manager_event(EVENT_FLAG_CALL, "Unlink",
					      "Channel1: %s\r\n"
					      "Channel2: %s\r\n"
					      "Uniqueid1: %s\r\n"
					      "Uniqueid2: %s\r\n"
					      "CallerID1: %s\r\n"
					      "CallerID2: %s\r\n",
					      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
				tris_debug(1, "Returning from native bridge, channels: %s, %s\n", c0->name, c1->name);

				tris_clear_flag(c0, TRIS_FLAG_NBRIDGE);
				tris_clear_flag(c1, TRIS_FLAG_NBRIDGE);

				if (c0->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE || c1->_softhangup == TRIS_SOFTHANGUP_UNBRIDGE)
					continue;

				c0->_bridge = NULL;
				c1->_bridge = NULL;

				return res;
			} else {
				tris_clear_flag(c0, TRIS_FLAG_NBRIDGE);
				tris_clear_flag(c1, TRIS_FLAG_NBRIDGE);
			}
			switch (res) {
			case TRIS_BRIDGE_RETRY:
				if (config->play_warning) {
					tris_set_flag(config, TRIS_FEATURE_WARNING_ACTIVE);
				}
				continue;
			default:
				tris_verb(3, "Native bridging %s and %s ended\n", c0->name, c1->name);
				/* fallthrough */
			case TRIS_BRIDGE_FAILED_NOWARN:
				nativefailed++;
				break;
			}
		}

		if (((c0->writeformat != c1->readformat) || (c0->readformat != c1->writeformat) ||
		    (c0->nativeformats != o0nativeformats) || (c1->nativeformats != o1nativeformats)) &&
		    !(c0->generator || c1->generator)) {
			if (tris_channel_make_compatible(c0, c1)) {
				tris_log(LOG_WARNING, "Can't make %s and %s compatible\n", c0->name, c1->name);
				manager_bridge_event(0, 1, c0, c1);
				return TRIS_BRIDGE_FAILED;
			}
			o0nativeformats = c0->nativeformats;
			o1nativeformats = c1->nativeformats;
		}

		update_bridge_vars(c0, c1);

		res = tris_generic_bridge(c0, c1, config, fo, rc, config->nexteventts);
		if (res != TRIS_BRIDGE_RETRY) {
			break;
		} else if (config->feature_timer) {
			/* feature timer expired but has not been updated, sending to tris_bridge_call to do so */
			break;
		}
	}

	tris_clear_flag(c0, TRIS_FLAG_END_DTMF_ONLY);
	tris_clear_flag(c1, TRIS_FLAG_END_DTMF_ONLY);

	/* Now that we have broken the bridge the source will change yet again */
	tris_indicate(c0, TRIS_CONTROL_SRCUPDATE);
	tris_indicate(c1, TRIS_CONTROL_SRCUPDATE);

	c0->_bridge = NULL;
	c1->_bridge = NULL;

	/* \todo  XXX here should check that cid_num is not NULL */
	manager_event(EVENT_FLAG_CALL, "Unlink",
		      "Channel1: %s\r\n"
		      "Channel2: %s\r\n"
		      "Uniqueid1: %s\r\n"
		      "Uniqueid2: %s\r\n"
		      "CallerID1: %s\r\n"
		      "CallerID2: %s\r\n",
		      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
	tris_debug(1, "Bridge stops bridging channels %s and %s\n", c0->name, c1->name);

	return res;
}

/*! \brief Sets an option on a channel */
int tris_channel_setoption(struct tris_channel *chan, int option, void *data, int datalen, int block)
{
	if (!chan->tech->setoption) {
		errno = ENOSYS;
		return -1;
	}

	if (block)
		tris_log(LOG_ERROR, "XXX Blocking not implemented yet XXX\n");

	return chan->tech->setoption(chan, option, data, datalen);
}

int tris_channel_queryoption(struct tris_channel *chan, int option, void *data, int *datalen, int block)
{
	if (!chan->tech->queryoption) {
		errno = ENOSYS;
		return -1;
	}

	if (block)
		tris_log(LOG_ERROR, "XXX Blocking not implemented yet XXX\n");

	return chan->tech->queryoption(chan, option, data, datalen);
}

struct tonepair_def {
	int freq1;
	int freq2;
	int duration;
	int vol;
};

struct tonepair_state {
	int fac1;
	int fac2;
	int v1_1;
	int v2_1;
	int v3_1;
	int v1_2;
	int v2_2;
	int v3_2;
	int origwfmt;
	int pos;
	int duration;
	int modulate;
	struct tris_frame f;
	unsigned char offset[TRIS_FRIENDLY_OFFSET];
	short data[4000];
};

static void tonepair_release(struct tris_channel *chan, void *params)
{
	struct tonepair_state *ts = params;

	if (chan)
		tris_set_write_format(chan, ts->origwfmt);
	tris_free(ts);
}

static void *tonepair_alloc(struct tris_channel *chan, void *params)
{
	struct tonepair_state *ts;
	struct tonepair_def *td = params;

	if (!(ts = tris_calloc(1, sizeof(*ts))))
		return NULL;
	ts->origwfmt = chan->writeformat;
	if (tris_set_write_format(chan, TRIS_FORMAT_SLINEAR)) {
		tris_log(LOG_WARNING, "Unable to set '%s' to signed linear format (write)\n", chan->name);
		tonepair_release(NULL, ts);
		ts = NULL;
	} else {
		ts->fac1 = 2.0 * cos(2.0 * M_PI * (td->freq1 / 8000.0)) * 32768.0;
		ts->v1_1 = 0;
		ts->v2_1 = sin(-4.0 * M_PI * (td->freq1 / 8000.0)) * td->vol;
		ts->v3_1 = sin(-2.0 * M_PI * (td->freq1 / 8000.0)) * td->vol;
		ts->v2_1 = 0;
		ts->fac2 = 2.0 * cos(2.0 * M_PI * (td->freq2 / 8000.0)) * 32768.0;
		ts->v2_2 = sin(-4.0 * M_PI * (td->freq2 / 8000.0)) * td->vol;
		ts->v3_2 = sin(-2.0 * M_PI * (td->freq2 / 8000.0)) * td->vol;
		ts->duration = td->duration;
		ts->modulate = 0;
	}
	/* Let interrupts interrupt :) */
	tris_set_flag(chan, TRIS_FLAG_WRITE_INT);
	return ts;
}

static int tonepair_generator(struct tris_channel *chan, void *data, int len, int samples)
{
	struct tonepair_state *ts = data;
	int x;

	/* we need to prepare a frame with 16 * timelen samples as we're
	 * generating SLIN audio
	 */
	len = samples * 2;

	if (len > sizeof(ts->data) / 2 - 1) {
		tris_log(LOG_WARNING, "Can't generate that much data!\n");
		return -1;
	}
	memset(&ts->f, 0, sizeof(ts->f));
 	for (x=0;x<len/2;x++) {
 		ts->v1_1 = ts->v2_1;
 		ts->v2_1 = ts->v3_1;
 		ts->v3_1 = (ts->fac1 * ts->v2_1 >> 15) - ts->v1_1;
 		
 		ts->v1_2 = ts->v2_2;
 		ts->v2_2 = ts->v3_2;
 		ts->v3_2 = (ts->fac2 * ts->v2_2 >> 15) - ts->v1_2;
 		if (ts->modulate) {
 			int p;
 			p = ts->v3_2 - 32768;
 			if (p < 0) p = -p;
 			p = ((p * 9) / 10) + 1;
 			ts->data[x] = (ts->v3_1 * p) >> 15;
 		} else
 			ts->data[x] = ts->v3_1 + ts->v3_2; 
 	}
	ts->f.frametype = TRIS_FRAME_VOICE;
	ts->f.subclass = TRIS_FORMAT_SLINEAR;
	ts->f.datalen = len;
	ts->f.samples = samples;
	ts->f.offset = TRIS_FRIENDLY_OFFSET;
	ts->f.data.ptr = ts->data;
	tris_write(chan, &ts->f);
	ts->pos += x;
	if (ts->duration > 0) {
		if (ts->pos >= ts->duration * 8)
			return -1;
	}
	return 0;
}

static struct tris_generator tonepair = {
	alloc: tonepair_alloc,
	release: tonepair_release,
	generate: tonepair_generator,
};

int tris_tonepair_start(struct tris_channel *chan, int freq1, int freq2, int duration, int vol)
{
	struct tonepair_def d = { 0, };

	d.freq1 = freq1;
	d.freq2 = freq2;
	d.duration = duration;
	d.vol = (vol < 1) ? 8192 : vol; /* force invalid to 8192 */
	if (tris_activate_generator(chan, &tonepair, &d))
		return -1;
	return 0;
}

void tris_tonepair_stop(struct tris_channel *chan)
{
	tris_deactivate_generator(chan);
}

int tris_tonepair(struct tris_channel *chan, int freq1, int freq2, int duration, int vol)
{
	int res;

	if ((res = tris_tonepair_start(chan, freq1, freq2, duration, vol)))
		return res;

	/* Give us some wiggle room */
	while (chan->generatordata && tris_waitfor(chan, 100) >= 0) {
		struct tris_frame *f = tris_read(chan);
		if (f)
			tris_frfree(f);
		else
			return -1;
	}
	return 0;
}

tris_group_t tris_get_group(const char *s)
{
	char *piece;
	char *c;
	int start=0, finish=0, x;
	tris_group_t group = 0;

	if (tris_strlen_zero(s))
		return 0;

	c = tris_strdupa(s);
	
	while ((piece = strsep(&c, ","))) {
		if (sscanf(piece, "%30d-%30d", &start, &finish) == 2) {
			/* Range */
		} else if (sscanf(piece, "%30d", &start)) {
			/* Just one */
			finish = start;
		} else {
			tris_log(LOG_ERROR, "Syntax error parsing group configuration '%s' at '%s'. Ignoring.\n", s, piece);
			continue;
		}
		for (x = start; x <= finish; x++) {
			if ((x > 63) || (x < 0)) {
				tris_log(LOG_WARNING, "Ignoring invalid group %d (maximum group is 63)\n", x);
			} else
				group |= ((tris_group_t) 1 << x);
		}
	}
	return group;
}

static int (*tris_moh_start_ptr)(struct tris_channel *, const char *, const char *) = NULL;
static void (*tris_moh_stop_ptr)(struct tris_channel *) = NULL;
static void (*tris_moh_cleanup_ptr)(struct tris_channel *) = NULL;

void tris_install_music_functions(int (*start_ptr)(struct tris_channel *, const char *, const char *),
				 void (*stop_ptr)(struct tris_channel *),
				 void (*cleanup_ptr)(struct tris_channel *))
{
	tris_moh_start_ptr = start_ptr;
	tris_moh_stop_ptr = stop_ptr;
	tris_moh_cleanup_ptr = cleanup_ptr;
}

void tris_uninstall_music_functions(void)
{
	tris_moh_start_ptr = NULL;
	tris_moh_stop_ptr = NULL;
	tris_moh_cleanup_ptr = NULL;
}

/*! \brief Turn on music on hold on a given channel */
int tris_moh_start(struct tris_channel *chan, const char *mclass, const char *interpclass)
{
	if (tris_moh_start_ptr)
		return tris_moh_start_ptr(chan, mclass, interpclass);

	tris_verb(3, "Music class %s requested but no musiconhold loaded.\n", mclass ? mclass : (interpclass ? interpclass : "default"));

	return 0;
}

/*! \brief Turn off music on hold on a given channel */
void tris_moh_stop(struct tris_channel *chan)
{
	if (tris_moh_stop_ptr)
		tris_moh_stop_ptr(chan);
}

void tris_moh_cleanup(struct tris_channel *chan)
{
	if (tris_moh_cleanup_ptr)
		tris_moh_cleanup_ptr(chan);
}

void tris_channels_init(void)
{
	tris_cli_register_multiple(cli_channel, ARRAY_LEN(cli_channel));
}

/*! \brief Print call group and pickup group ---*/
char *tris_print_group(char *buf, int buflen, tris_group_t group)
{
	unsigned int i;
	int first = 1;
	char num[3];

	buf[0] = '\0';
	
	if (!group)	/* Return empty string if no group */
		return buf;

	for (i = 0; i <= 63; i++) {	/* Max group is 63 */
		if (group & ((tris_group_t) 1 << i)) {
	   		if (!first) {
				strncat(buf, ", ", buflen - strlen(buf) - 1);
			} else {
				first = 0;
	  		}
			snprintf(num, sizeof(num), "%u", i);
			strncat(buf, num, buflen - strlen(buf) - 1);
		}
	}
	return buf;
}

void tris_set_variables(struct tris_channel *chan, struct tris_variable *vars)
{
	struct tris_variable *cur;

	for (cur = vars; cur; cur = cur->next)
		pbx_builtin_setvar_helper(chan, cur->name, cur->value);	
}

static void *silence_generator_alloc(struct tris_channel *chan, void *data)
{
	/* just store the data pointer in the channel structure */
	return data;
}

static void silence_generator_release(struct tris_channel *chan, void *data)
{
	/* nothing to do */
}

static int silence_generator_generate(struct tris_channel *chan, void *data, int len, int samples)
{
	short buf[samples];
	struct tris_frame frame = {
		.frametype = TRIS_FRAME_VOICE,
		.subclass = TRIS_FORMAT_SLINEAR,
		.data.ptr = buf,
		.samples = samples,
		.datalen = sizeof(buf),
	};

	memset(buf, 0, sizeof(buf));

	if (tris_write(chan, &frame))
		return -1;

	return 0;
}

static struct tris_generator silence_generator = {
	.alloc = silence_generator_alloc,
	.release = silence_generator_release,
	.generate = silence_generator_generate,
};

struct tris_silence_generator {
	int old_write_format;
};

struct tris_silence_generator *tris_channel_start_silence_generator(struct tris_channel *chan)
{
	struct tris_silence_generator *state;

	if (!(state = tris_calloc(1, sizeof(*state)))) {
		return NULL;
	}

	state->old_write_format = chan->writeformat;

	if (tris_set_write_format(chan, TRIS_FORMAT_SLINEAR) < 0) {
		tris_log(LOG_ERROR, "Could not set write format to SLINEAR\n");
		tris_free(state);
		return NULL;
	}

	tris_activate_generator(chan, &silence_generator, state);

	tris_debug(1, "Started silence generator on '%s'\n", chan->name);

	return state;
}

void tris_channel_stop_silence_generator(struct tris_channel *chan, struct tris_silence_generator *state)
{
	if (!state)
		return;

	tris_deactivate_generator(chan);

	tris_debug(1, "Stopped silence generator on '%s'\n", chan->name);

	if (tris_set_write_format(chan, state->old_write_format) < 0)
		tris_log(LOG_ERROR, "Could not return write format to its original state\n");

	tris_free(state);
}


/*! \ brief Convert channel reloadreason (ENUM) to text string for manager event */
const char *channelreloadreason2txt(enum channelreloadreason reason)
{
	switch (reason) {
	case CHANNEL_MODULE_LOAD:
		return "LOAD (Channel module load)";

	case CHANNEL_MODULE_RELOAD:
		return "RELOAD (Channel module reload)";

	case CHANNEL_CLI_RELOAD:
		return "CLIRELOAD (Channel module reload by CLI command)";

	default:
		return "MANAGERRELOAD (Channel module reload by manager)";
	}
};

#ifdef DEBUG_CHANNEL_LOCKS

/*! \brief Unlock AST channel (and print debugging output) 
\note You need to enable DEBUG_CHANNEL_LOCKS for this function
*/
int __tris_channel_unlock(struct tris_channel *chan, const char *filename, int lineno, const char *func)
{
	int res = 0;
	tris_debug(3, "::::==== Unlocking AST channel %s\n", chan->name);
	
	if (!chan) {
		tris_debug(1, "::::==== Unlocking non-existing channel \n");
		return 0;
	}
#ifdef DEBUG_THREADS
	res = __tris_pthread_mutex_unlock(filename, lineno, func, "(channel lock)", &chan->lock_dont_use);
#else
	res = tris_mutex_unlock(&chan->lock_dont_use);
#endif

	if (option_debug > 2) {
#ifdef DEBUG_THREADS
		int count = 0;
		if ((count = chan->lock_dont_use.track.reentrancy))
			tris_debug(3, ":::=== Still have %d locks (recursive)\n", count);
#endif
		if (!res)
			tris_debug(3, "::::==== Channel %s was unlocked\n", chan->name);
		if (res == EINVAL) {
			tris_debug(3, "::::==== Channel %s had no lock by this thread. Failed unlocking\n", chan->name);
		}
	}
	if (res == EPERM) {
		/* We had no lock, so okay any way*/
		tris_debug(4, "::::==== Channel %s was not locked at all \n", chan->name);
		res = 0;
	}
	return res;
}

/*! \brief Lock AST channel (and print debugging output)
\note You need to enable DEBUG_CHANNEL_LOCKS for this function */
int __tris_channel_lock(struct tris_channel *chan, const char *filename, int lineno, const char *func)
{
	int res;

	tris_debug(4, "====:::: Locking AST channel %s\n", chan->name);

#ifdef DEBUG_THREADS
	res = __tris_pthread_mutex_lock(filename, lineno, func, "(channel lock)", &chan->lock_dont_use);
#else
	res = tris_mutex_lock(&chan->lock_dont_use);
#endif

	if (option_debug > 3) {
#ifdef DEBUG_THREADS
		int count = 0;
		if ((count = chan->lock_dont_use.track.reentrancy))
			tris_debug(4, ":::=== Now have %d locks (recursive)\n", count);
#endif
		if (!res)
			tris_debug(4, "::::==== Channel %s was locked\n", chan->name);
		if (res == EDEADLK) {
			/* We had no lock, so okey any way */
			tris_debug(4, "::::==== Channel %s was not locked by us. Lock would cause deadlock.\n", chan->name);
		}
		if (res == EINVAL) {
			tris_debug(4, "::::==== Channel %s lock failed. No mutex.\n", chan->name);
		}
	}
	return res;
}

/*! \brief Lock AST channel (and print debugging output)
\note	You need to enable DEBUG_CHANNEL_LOCKS for this function */
int __tris_channel_trylock(struct tris_channel *chan, const char *filename, int lineno, const char *func)
{
	int res;

	tris_debug(3, "====:::: Trying to lock AST channel %s\n", chan->name);
#ifdef DEBUG_THREADS
	res = __tris_pthread_mutex_trylock(filename, lineno, func, "(channel lock)", &chan->lock_dont_use);
#else
	res = tris_mutex_trylock(&chan->lock_dont_use);
#endif

	if (option_debug > 2) {
#ifdef DEBUG_THREADS
		int count = 0;
		if ((count = chan->lock_dont_use.track.reentrancy))
			tris_debug(3, ":::=== Now have %d locks (recursive)\n", count);
#endif
		if (!res)
			tris_debug(3, "::::==== Channel %s was locked\n", chan->name);
		if (res == EBUSY) {
			/* We failed to lock */
			tris_debug(3, "::::==== Channel %s failed to lock. Not waiting around...\n", chan->name);
		}
		if (res == EDEADLK) {
			/* We had no lock, so okey any way*/
			tris_debug(3, "::::==== Channel %s was not locked. Lock would cause deadlock.\n", chan->name);
		}
		if (res == EINVAL)
			tris_debug(3, "::::==== Channel %s lock failed. No mutex.\n", chan->name);
	}
	return res;
}

#endif

/*
 * Wrappers for various tris_say_*() functions that call the full version
 * of the same functions.
 * The proper place would be say.c, but that file is optional and one
 * must be able to build trismedia even without it (using a loadable 'say'
 * implementation that only supplies the 'full' version of the functions.
 */

int tris_say_number(struct tris_channel *chan, int num,
	const char *ints, const char *language, const char *options)
{
	return tris_say_number_full(chan, num, ints, language, options, -1, -1);
}

int tris_say_enumeration(struct tris_channel *chan, int num,
	const char *ints, const char *language, const char *options)
{
	return tris_say_enumeration_full(chan, num, ints, language, options, -1, -1);
}

int tris_say_digits(struct tris_channel *chan, int num,
	const char *ints, const char *lang)
{
	return tris_say_digits_full(chan, num, ints, lang, -1, -1);
}

int tris_say_digit_str(struct tris_channel *chan, const char *str,
	const char *ints, const char *lang)
{
	return tris_say_digit_str_full(chan, str, ints, lang, -1, -1);
}

int tris_say_character_str(struct tris_channel *chan, const char *str,
	const char *ints, const char *lang)
{
	return tris_say_character_str_full(chan, str, ints, lang, -1, -1);
}

int tris_say_phonetic_str(struct tris_channel *chan, const char *str,
	const char *ints, const char *lang)
{
	return tris_say_phonetic_str_full(chan, str, ints, lang, -1, -1);
}

int tris_say_digits_full(struct tris_channel *chan, int num,
	const char *ints, const char *lang, int audiofd, int ctrlfd)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "%d", num);

	return tris_say_digit_str_full(chan, buf, ints, lang, audiofd, ctrlfd);
}

/* DO NOT PUT ADDITIONAL FUNCTIONS BELOW THIS BOUNDARY
 *
 * ONLY FUNCTIONS FOR PROVIDING BACKWARDS ABI COMPATIBILITY BELONG HERE
 *
 */

/* Provide binary compatibility for modules that call tris_channel_alloc() directly;
 * newly compiled modules will call __tris_channel_alloc() via the macros in channel.h
 */
#undef tris_channel_alloc
struct tris_channel __attribute__((format(printf, 9, 10)))
	*tris_channel_alloc(int needqueue, int state, const char *cid_num,
			   const char *cid_name, const char *acctcode,
			   const char *exten, const char *context,
			   const int amaflag, const char *name_fmt, ...);
struct tris_channel *tris_channel_alloc(int needqueue, int state, const char *cid_num,
				      const char *cid_name, const char *acctcode,
				      const char *exten, const char *context,
				      const int amaflag, const char *name_fmt, ...)
{
	va_list ap1, ap2;
	struct tris_channel *result;


	va_start(ap1, name_fmt);
	va_start(ap2, name_fmt);
	result = __tris_channel_alloc_ap(needqueue, state, cid_num, cid_name, acctcode, exten, context,
					amaflag, __FILE__, __LINE__, __FUNCTION__, name_fmt, ap1, ap2);
	va_end(ap1);
	va_end(ap2);

	return result;
}
