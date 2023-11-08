/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2007, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
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
 * \brief Dialing API
 *
 * \author Joshua Colp <jcolp@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153223 $")

#include <sys/time.h>
#include <signal.h>

#include "trismedia/channel.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/linkedlists.h"
#include "trismedia/dial.h"
#include "trismedia/pbx.h"
#include "trismedia/musiconhold.h"

/*! \brief Main dialing structure. Contains global options, channels being dialed, and more! */
struct tris_dial {
	int num;                                           /*!< Current number to give to next dialed channel */
	int timeout;                                       /*!< Maximum time allowed for dial attempts */
	int actual_timeout;                                /*!< Actual timeout based on all factors (ie: channels) */
	enum tris_dial_result state;                        /*!< Status of dial */
	void *options[TRIS_DIAL_OPTION_MAX];                /*!< Global options */
	tris_dial_state_callback state_callback;            /*!< Status callback */
	TRIS_LIST_HEAD(, tris_dial_channel) channels; /*!< Channels being dialed */
	pthread_t thread;                                  /*!< Thread (if running in async) */
	tris_mutex_t lock;                                  /*! Lock to protect the thread information above */
	struct tris_channel *chan;
	int referid;
};

/*! \brief Dialing channel structure. Contains per-channel dialing options, trismedia channel, and more! */
struct tris_dial_channel {
	int num;				/*!< Unique number for dialed channel */
	int timeout;				/*!< Maximum time allowed for attempt */
	char *tech;				/*!< Technology being dialed */
	char *device;				/*!< Device being dialed */
	void *options[TRIS_DIAL_OPTION_MAX];	/*!< Channel specific options */
	int cause;				/*!< Cause code in case of failure */
	unsigned int is_running_app:1;		/*!< Is this running an application? */
	struct tris_channel *owner;		/*!< Trismedia channel */
	TRIS_LIST_ENTRY(tris_dial_channel) list;	/*!< Linked list information */
};

/*! \brief Typedef for dial option enable */
typedef void *(*tris_dial_option_cb_enable)(void *data);

/*! \brief Typedef for dial option disable */
typedef int (*tris_dial_option_cb_disable)(void *data);

/*! \brief Structure for 'ANSWER_EXEC' option */
struct answer_exec_struct {
	char app[TRIS_MAX_APP]; /*!< Application name */
	char *args;            /*!< Application arguments */
};

/*! \brief Enable function for 'ANSWER_EXEC' option */
static void *answer_exec_enable(void *data)
{
	struct answer_exec_struct *answer_exec = NULL;
	char *app = tris_strdupa((char*)data), *args = NULL;

	/* Not giving any data to this option is bad, mmmk? */
	if (tris_strlen_zero(app))
		return NULL;

	/* Create new data structure */
	if (!(answer_exec = tris_calloc(1, sizeof(*answer_exec))))
		return NULL;
	
	/* Parse out application and arguments */
	if ((args = strchr(app, ','))) {
		*args++ = '\0';
		answer_exec->args = tris_strdup(args);
	}

	/* Copy application name */
	tris_copy_string(answer_exec->app, app, sizeof(answer_exec->app));

	return answer_exec;
}

/*! \brief Disable function for 'ANSWER_EXEC' option */
static int answer_exec_disable(void *data)
{
	struct answer_exec_struct *answer_exec = data;

	/* Make sure we have a value */
	if (!answer_exec)
		return -1;

	/* If arguments are present, free them too */
	if (answer_exec->args)
		tris_free(answer_exec->args);

	/* This is simple - just free the structure */
	tris_free(answer_exec);

	return 0;
}

static void *music_enable(void *data)
{
	return tris_strdup(data);
}

static int music_disable(void *data)
{
	if (!data)
		return -1;

	tris_free(data);

	return 0;
}

/*! \brief Application execution function for 'ANSWER_EXEC' option */
static void answer_exec_run(struct tris_dial *dial, struct tris_dial_channel *dial_channel, char *app, char *args)
{
	struct tris_channel *chan = dial_channel->owner;
	struct tris_app *tris_app = pbx_findapp(app);

	/* If the application was not found, return immediately */
	if (!tris_app)
		return;

	/* All is well... execute the application */
	pbx_exec(chan, tris_app, args);

	/* If another thread is not taking over hang up the channel */
	tris_mutex_lock(&dial->lock);
	if (dial->thread != TRIS_PTHREADT_STOP) {
		if (!dial->referid) {
			if (dial_channel->owner) {
				if (!tris_check_hangup(dial_channel->owner))
					tris_hangup(dial_channel->owner);
			}
		}
		dial_channel->owner = NULL;
	}
	tris_mutex_unlock(&dial->lock);

	return;
}

/*! \brief Options structure - maps options to respective handlers (enable/disable). This list MUST be perfectly kept in order, or else madness will happen. */
static const struct tris_option_types {
	enum tris_dial_option option;
	tris_dial_option_cb_enable enable;
	tris_dial_option_cb_disable disable;
} option_types[] = {
	{ TRIS_DIAL_OPTION_RINGING, NULL, NULL },                                  /*!< Always indicate ringing to caller */
	{ TRIS_DIAL_OPTION_ANSWER_EXEC, answer_exec_enable, answer_exec_disable }, /*!< Execute application upon answer in async mode */
	{ TRIS_DIAL_OPTION_MUSIC, music_enable, music_disable },                   /*!< Play music to the caller instead of ringing */
	{ TRIS_DIAL_OPTION_DISABLE_CALL_FORWARDING, NULL, NULL },                  /*!< Disable call forwarding on channels */
	{ TRIS_DIAL_OPTION_MAX, NULL, NULL },                                      /*!< Terminator of list */
};

/*! \brief free the buffer if allocated, and set the pointer to the second arg */
#define S_REPLACE(s, new_val) \
	do {                      \
		if (s) {              \
			free(s);          \
		}                     \
		s = (new_val);        \
	} while (0)

/*! \brief Maximum number of channels we can watch at a time */
#define TRIS_MAX_WATCHERS 256

/*! \brief Macro for finding the option structure to use on a dialed channel */
#define FIND_RELATIVE_OPTION(dial, dial_channel, tris_dial_option) (dial_channel->options[tris_dial_option] ? dial_channel->options[tris_dial_option] : dial->options[tris_dial_option])

/*! \brief Macro that determines whether a channel is the caller or not */
#define IS_CALLER(chan, owner) (chan == owner ? 1 : 0)

/*! \brief New dialing structure
 * \note Create a dialing structure
 * \return Returns a calloc'd tris_dial structure, NULL on failure
 */
struct tris_dial *tris_dial_create(void)
{
	struct tris_dial *dial = NULL;

	/* Allocate new memory for structure */
	if (!(dial = tris_calloc(1, sizeof(*dial))))
		return NULL;

	memset(dial, 0, sizeof(*dial));
	/* Initialize list of channels */
	TRIS_LIST_HEAD_INIT(&dial->channels);

	/* Initialize thread to NULL */
	dial->thread = TRIS_PTHREADT_NULL;

	/* No timeout exists... yet */
	dial->timeout = -1;
	dial->actual_timeout = -1;

	/* Can't forget about the lock */
	tris_mutex_init(&dial->lock);

	return dial;
}

/*! \brief Append a channel
 * \note Appends a channel to a dialing structure
 * \return Returns channel reference number on success, -1 on failure
 */
int tris_dial_append(struct tris_dial *dial, const char *tech, const char *device)
{
	struct tris_dial_channel *channel = NULL;

	/* Make sure we have required arguments */
	if (!dial || !tech || !device)
		return -1;

	/* Allocate new memory for dialed channel structure */
	if (!(channel = tris_calloc(1, sizeof(*channel))))
		return -1;

	/* Record technology and device for when we actually dial */
	channel->tech = tris_strdup(tech);
	channel->device = tris_strdup(device);

	/* Grab reference number from dial structure */
	channel->num = tris_atomic_fetchadd_int(&dial->num, +1);

	/* No timeout exists... yet */
	channel->timeout = -1;

	/* Insert into channels list */
	TRIS_LIST_INSERT_TAIL(&dial->channels, channel, list);

	return channel->num;
}

/*! \brief Helper function that does the beginning dialing per-appended channel */
static int begin_dial_channel(struct tris_dial_channel *channel, struct tris_channel *chan)
{
	char numsubst[TRIS_MAX_EXTENSION];
	char dialnumber[TRIS_MAX_EXTENSION];
	char *tmp;
	int res = 1;

	/* Copy device string over */
	tris_copy_string(numsubst, channel->device, sizeof(numsubst));

	tris_copy_string(dialnumber, channel->device, sizeof(dialnumber));
	if ((tmp =strchr(dialnumber,'@'))) 
		*tmp='\0';

	/* If we fail to create our owner channel bail out */
	if (!(channel->owner = tris_request(channel->tech, chan ? chan->nativeformats : TRIS_FORMAT_AUDIO_MASK, numsubst, &channel->cause, chan?chan:NULL)))
		return -1;

	channel->owner->appl = "AppDial2";
	channel->owner->data = "(Outgoing Line)";
	memset(&channel->owner->whentohangup, 0, sizeof(channel->owner->whentohangup));

	/* Inherit everything from he who spawned this dial */
	if (chan) {
		tris_channel_inherit_variables(chan, channel->owner);
		tris_channel_datastore_inherit(chan, channel->owner);

		/* Copy over callerid information */
		tris_verbose("dialnumbering is %s\n",dialnumber);
		S_REPLACE(channel->owner->cid.cid_num, tris_strdup(dialnumber));
		S_REPLACE(channel->owner->cid.cid_name, tris_strdup(chan->cid.cid_name));
		S_REPLACE(channel->owner->cid.cid_from_num, tris_strdup(chan->cid.cid_num));
		S_REPLACE(channel->owner->cid.cid_ani, tris_strdup(chan->cid.cid_ani));
		S_REPLACE(channel->owner->cid.cid_rdnis, tris_strdup(chan->cid.cid_rdnis));

		tris_string_field_set(channel->owner, language, chan->language);
		tris_string_field_set(channel->owner, accountcode, chan->accountcode);
		channel->owner->cdrflags = chan->cdrflags;
		if (tris_strlen_zero(channel->owner->musicclass))
			tris_string_field_set(channel->owner, musicclass, chan->musicclass);

		channel->owner->cid.cid_pres = chan->cid.cid_pres;
		channel->owner->cid.cid_ton = chan->cid.cid_ton;
		channel->owner->cid.cid_tns = chan->cid.cid_tns;
		channel->owner->adsicpe = chan->adsicpe;
		channel->owner->transfercapability = chan->transfercapability;
	}

	/* Attempt to actually call this device */
	if ((res = tris_call(channel->owner, numsubst, 0))) {
		res = 0;
		tris_hangup(channel->owner);
		channel->owner = NULL;
	} else {
		if (chan)
			tris_poll_channel_add(chan, channel->owner);
		res = 1;
		tris_verb(3, "Called %s\n", numsubst);
	}

	return res;
}

/*! \brief Helper function that does the beginning dialing per dial structure */
static int begin_dial(struct tris_dial *dial, struct tris_channel *chan)
{
	struct tris_dial_channel *channel = NULL;
	int success = 0;

	/* Iterate through channel list, requesting and calling each one */
	TRIS_LIST_LOCK(&dial->channels);
	TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
		success += begin_dial_channel(channel, chan);
		if (channel && channel->owner)
			channel->owner->referid = dial->referid;
	}
	TRIS_LIST_UNLOCK(&dial->channels);

	/* If number of failures matches the number of channels, then this truly failed */
	return success;
}

/*! \brief Helper function to handle channels that have been call forwarded */
static int handle_call_forward(struct tris_dial *dial, struct tris_dial_channel *channel, struct tris_channel *chan)
{
	struct tris_channel *original = channel->owner;
	char *tmp = tris_strdupa(channel->owner->call_forward);
	char *tech = "Local", *device = tmp, *stuff;

	/* If call forwarding is disabled just drop the original channel and don't attempt to dial the new one */
	if (FIND_RELATIVE_OPTION(dial, channel, TRIS_DIAL_OPTION_DISABLE_CALL_FORWARDING)) {
		tris_hangup(original);
		channel->owner = NULL;
		return 0;
	}

	/* Figure out the new destination */
	if ((stuff = strchr(tmp, '/'))) {
		*stuff++ = '\0';
		tech = tmp;
		device = stuff;
	}

	/* Drop old destination information */
	tris_free(channel->tech);
	tris_free(channel->device);

	/* Update the dial channel with the new destination information */
	channel->tech = tris_strdup(tech);
	channel->device = tris_strdup(device);
	TRIS_LIST_UNLOCK(&dial->channels);

	/* Finally give it a go... send it out into the world */
	begin_dial_channel(channel, chan);

	/* Drop the original channel */
	tris_hangup(original);

	return 0;
}

/*! \brief Helper function that finds the dialed channel based on owner */
static struct tris_dial_channel *find_relative_dial_channel(struct tris_dial *dial, struct tris_channel *owner)
{
	struct tris_dial_channel *channel = NULL;

	TRIS_LIST_LOCK(&dial->channels);
	TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
		if (channel->owner == owner)
			break;
	}
	TRIS_LIST_UNLOCK(&dial->channels);

	return channel;
}

static void set_state(struct tris_dial *dial, enum tris_dial_result state)
{
	dial->state = state;

	if (dial->state_callback)
		dial->state_callback(dial);
}

/*! \brief Helper function that handles control frames WITH owner */
static void handle_frame(struct tris_dial *dial, struct tris_dial_channel *channel, struct tris_frame *fr, struct tris_channel *chan)
{
	if (fr->frametype == TRIS_FRAME_CONTROL) {
		switch (fr->subclass) {
		case TRIS_CONTROL_ANSWER:
			tris_verb(3, "%s answered %s\n", channel->owner->name, chan->name);
			TRIS_LIST_LOCK(&dial->channels);
			TRIS_LIST_REMOVE(&dial->channels, channel, list);
			TRIS_LIST_INSERT_HEAD(&dial->channels, channel, list);
			TRIS_LIST_UNLOCK(&dial->channels);
			set_state(dial, TRIS_DIAL_RESULT_ANSWERED);
			break;
		case TRIS_CONTROL_BUSY:
			tris_verb(3, "%s is busy\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_BUSY);
			break;
		case TRIS_CONTROL_CONGESTION:
			tris_verb(3, "%s is circuit-busy\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
			break;
		case TRIS_CONTROL_ROUTEFAIL:
			tris_verb(3, "%s is circuit-busy\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
			break;
		case TRIS_CONTROL_REJECTED:
			tris_verb(3, "%s is circuit-busy\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
			break;
		case TRIS_CONTROL_UNAVAILABLE:
			tris_verb(3, "%s is circuit-busy\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
			break;
		case TRIS_CONTROL_RINGING:
			tris_verb(3, "%s is ringing\n", channel->owner->name);
			if (!dial->options[TRIS_DIAL_OPTION_MUSIC])
				tris_indicate(chan, TRIS_CONTROL_RINGING);
			set_state(dial, TRIS_DIAL_RESULT_RINGING);
			break;
		case TRIS_CONTROL_PROGRESS:
			tris_verb(3, "%s is making progress, passing it to %s\n", channel->owner->name, chan->name);
			tris_indicate(chan, TRIS_CONTROL_PROGRESS);
			set_state(dial, TRIS_DIAL_RESULT_PROGRESS);
			break;
		case TRIS_CONTROL_VIDUPDATE:
			tris_verb(3, "%s requested a video update, passing it to %s\n", channel->owner->name, chan->name);
			tris_indicate(chan, TRIS_CONTROL_VIDUPDATE);
			break;
		case TRIS_CONTROL_SRCUPDATE:
			if (option_verbose > 2)
				tris_verbose (VERBOSE_PREFIX_3 "%s requested a source update, passing it to %s\n", channel->owner->name, chan->name);
			tris_indicate(chan, TRIS_CONTROL_SRCUPDATE);
			break;
		case TRIS_CONTROL_PROCEEDING:
			tris_verb(3, "%s is proceeding, passing it to %s\n", channel->owner->name, chan->name);
			tris_indicate(chan, TRIS_CONTROL_PROCEEDING);
			set_state(dial, TRIS_DIAL_RESULT_PROCEEDING);
			break;
		case TRIS_CONTROL_HOLD:
			tris_verb(3, "Call on %s placed on hold\n", chan->name);
			tris_indicate(chan, TRIS_CONTROL_HOLD);
			break;
		case TRIS_CONTROL_UNHOLD:
			tris_verb(3, "Call on %s left from hold\n", chan->name);
			tris_indicate(chan, TRIS_CONTROL_UNHOLD);
			break;
		case TRIS_CONTROL_OFFHOOK:
			tris_verb(3, "%s is off hook\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_OFFHOOK);
		case TRIS_CONTROL_FLASH:
			break;
		case TRIS_CONTROL_FORBIDDEN:
			tris_verb(3, "%s is forbidden\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_FORBIDDEN);
			break;
		case TRIS_CONTROL_TAKEOFFHOOK:
			tris_verb(3, "%s is take off hook\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_TAKEOFFHOOK);
			break;
		case TRIS_CONTROL_TIMEOUT:
			tris_verb(3, "%s is time out\n", channel->owner->name);
			tris_hangup(channel->owner);
			channel->owner = NULL;
			set_state(dial, TRIS_DIAL_RESULT_TIMEOUT);
			break;
		case TRIS_CONTROL_HANGUP:
			set_state(dial, TRIS_DIAL_RESULT_HANGUP);
			break;
		case -1:
			/* Prod the channel */
			tris_indicate(chan, -1);
			break;
		default:
			break;
		}
	}

	return;
}

/*! \brief Helper function that handles control frames WITHOUT owner */
static void handle_frame_ownerless(struct tris_dial *dial, struct tris_dial_channel *channel, struct tris_frame *fr)
{
	/* If we have no owner we can only update the state of the dial structure, so only look at control frames */
	if (fr->frametype != TRIS_FRAME_CONTROL)
		return;

	switch (fr->subclass) {
	case TRIS_CONTROL_ANSWER:
		tris_verb(3, "%s answered\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_ANSWER);
		TRIS_LIST_LOCK(&dial->channels);
		TRIS_LIST_REMOVE(&dial->channels, channel, list);
		TRIS_LIST_INSERT_HEAD(&dial->channels, channel, list);
		TRIS_LIST_UNLOCK(&dial->channels);
		set_state(dial, TRIS_DIAL_RESULT_ANSWERED);
		break;
	case TRIS_CONTROL_BUSY:
		tris_verb(3, "## %s is busy\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_BUSY);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_BUSY);
		break;
	case TRIS_CONTROL_CONGESTION:
		tris_verb(3, "## %s is circuit-busy\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_BUSY);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
		break;
	case TRIS_CONTROL_ROUTEFAIL:
		tris_verb(3, "## %s is circuit-busy\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_CIRCUITS);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
		break;
	case TRIS_CONTROL_REJECTED:
		tris_verb(3, "## %s is circuit-busy\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_BUSY);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
		break;
	case TRIS_CONTROL_UNAVAILABLE:
		tris_verb(3, "## %s is circuit-busy\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_BUSY);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_CONGESTION);
		break;
	case TRIS_CONTROL_OFFHOOK:
		tris_verb(3, " ## %s is off hook\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_OFFHOOK);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_OFFHOOK);
		break;
	case TRIS_CONTROL_FORBIDDEN:
		tris_verb(3, "## %s is forbidden\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_FORBIDDEN);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_FORBIDDEN);
		break;
	case TRIS_CONTROL_TAKEOFFHOOK:
		tris_verb(3, "## %s is take off hood\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_TAKEOFFHOOK);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_TAKEOFFHOOK);
		break;
	case TRIS_CONTROL_TIMEOUT:
		tris_verb(3, "## %s is time out\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_TIMEOUT);
		tris_hangup(channel->owner);
		channel->owner = NULL;
		set_state(dial, TRIS_DIAL_RESULT_TIMEOUT);
		break;
	case TRIS_CONTROL_RINGING:
		tris_verb(3, "%s is ringing\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_RINGING);
		set_state(dial, TRIS_DIAL_RESULT_RINGING);
		break;
	case TRIS_CONTROL_PROGRESS:
		tris_verb(3, "%s is making progress\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_PROGRESS);
		set_state(dial, TRIS_DIAL_RESULT_PROGRESS);
		break;
	case TRIS_CONTROL_PROCEEDING:
		tris_verb(3, "%s is proceeding\n", channel->owner->name);
		tris_dial_send_notify(dial, channel->owner->cid.cid_num, TRIS_CONTROL_NOTIFY_PROCEEDING);
		set_state(dial, TRIS_DIAL_RESULT_PROCEEDING);
		break;
	case TRIS_CONTROL_HANGUP:
		set_state(dial, TRIS_DIAL_RESULT_HANGUP);
		break;
	default:
		break;
	}

	return;
}

/*! \brief Helper function to handle when a timeout occurs on dialing attempt */
static int handle_timeout_trip(struct tris_dial *dial, struct timeval start)
{
	struct tris_dial_channel *channel = NULL;
	int diff = tris_tvdiff_ms(tris_tvnow(), start), lowest_timeout = -1, new_timeout = -1;

	/* If the global dial timeout tripped switch the state to timeout so our channel loop will drop every channel */
	if (diff >= dial->timeout) {
		set_state(dial, TRIS_DIAL_RESULT_TIMEOUT);
		new_timeout = 0;
	}

	/* Go through dropping out channels that have met their timeout */
	TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
		if (dial->state == TRIS_DIAL_RESULT_TIMEOUT || diff >= channel->timeout) {
			tris_hangup(channel->owner);
			channel->owner = NULL;
		} else if ((lowest_timeout == -1) || (lowest_timeout > channel->timeout)) {
			lowest_timeout = channel->timeout;
		}
	}

	/* Calculate the new timeout using the lowest timeout found */
	if (lowest_timeout >= 0)
		new_timeout = lowest_timeout - diff;

	return new_timeout;
}

/*! \brief Helper function that basically keeps tabs on dialing attempts */
static enum tris_dial_result monitor_dial(struct tris_dial *dial, struct tris_channel *chan)
{
	int timeout = -1;
	struct tris_channel *cs[TRIS_MAX_WATCHERS], *who = NULL;
	struct tris_dial_channel *channel = NULL;
	struct answer_exec_struct *answer_exec = NULL;
	struct timeval start;

	set_state(dial, TRIS_DIAL_RESULT_TRYING);

	/* If the "always indicate ringing" option is set, change state to ringing and indicate to the owner if present */
	if (dial->options[TRIS_DIAL_OPTION_RINGING]) {
		set_state(dial, TRIS_DIAL_RESULT_RINGING);
		if (chan)
			tris_indicate(chan, TRIS_CONTROL_RINGING);
	} else if (chan && dial->options[TRIS_DIAL_OPTION_MUSIC] && 
		!tris_strlen_zero(dial->options[TRIS_DIAL_OPTION_MUSIC])) {
		char *original_moh = tris_strdupa(chan->musicclass);
		tris_indicate(chan, -1);
		tris_string_field_set(chan, musicclass, dial->options[TRIS_DIAL_OPTION_MUSIC]);
		tris_moh_start(chan, dial->options[TRIS_DIAL_OPTION_MUSIC], NULL);
		tris_string_field_set(chan, musicclass, original_moh);
	}

	/* Record start time for timeout purposes */
	start = tris_tvnow();

	/* We actually figured out the maximum timeout we can do as they were added, so we can directly access the info */
	timeout = dial->actual_timeout;

	/* Go into an infinite loop while we are trying */
	while ((dial->state != TRIS_DIAL_RESULT_UNANSWERED) && (dial->state != TRIS_DIAL_RESULT_ANSWERED) && (dial->state != TRIS_DIAL_RESULT_HANGUP) && (dial->state != TRIS_DIAL_RESULT_TIMEOUT)) {
		int pos = 0, count = 0;
		struct tris_frame *fr = NULL;

		/* Set up channel structure array */
		pos = count = 0;
		if (chan)
			cs[pos++] = chan;

		/* Add channels we are attempting to dial */
		TRIS_LIST_LOCK(&dial->channels);
		TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
			if (channel->owner) {
				cs[pos++] = channel->owner;
				count++;
			}
		}
		TRIS_LIST_UNLOCK(&dial->channels);

		/* If we have no outbound channels in progress, switch state to unanswered and stop */
		if (!count) {
			//set_state(dial, TRIS_DIAL_RESULT_UNANSWERED);
			break;
		}

		/* Just to be safe... */
		if (dial->thread == TRIS_PTHREADT_STOP)
			break;

		/* Wait for frames from channels */
		who = tris_waitfor_n(cs, pos, &timeout);

		/* Check to see if our thread is being cancelled */
		if (dial->thread == TRIS_PTHREADT_STOP)
			break;

		/* If the timeout no longer exists OR if we got no channel it basically means the timeout was tripped, so handle it */
		if (!timeout || !who) {
			timeout = handle_timeout_trip(dial, start);
			continue;
		}

		/* Find relative dial channel */
		if (!chan || !IS_CALLER(chan, who))
			channel = find_relative_dial_channel(dial, who);

		/* See if this channel has been forwarded elsewhere */
		if (!tris_strlen_zero(who->call_forward)) {
			handle_call_forward(dial, channel, chan);
			continue;
		}

		/* Attempt to read in a frame */
		if (!(fr = tris_read(who))) {
			/* If this is the caller then we switch state to hangup and stop */
			if (chan && IS_CALLER(chan, who)) {
				set_state(dial, TRIS_DIAL_RESULT_HANGUP);
				break;
			}
			if (chan)
				tris_poll_channel_del(chan, channel->owner);
			tris_hangup(who);
			channel->owner = NULL;
			continue;
		}

		/* Process the frame */
		if (chan)
			handle_frame(dial, channel, fr, chan);
		else
			handle_frame_ownerless(dial, channel, fr);

		/* Free the received frame and start all over */
		tris_frfree(fr);
	}

	/* Do post-processing from loop */
	if (dial->state == TRIS_DIAL_RESULT_ANSWERED) {
		/* Hangup everything except that which answered */
		TRIS_LIST_LOCK(&dial->channels);
		TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
			if (!channel->owner || channel->owner == who)
				continue;
			if (chan)
				tris_poll_channel_del(chan, channel->owner);
			tris_hangup(channel->owner);
			channel->owner = NULL;
		}
		TRIS_LIST_UNLOCK(&dial->channels);
		/* If ANSWER_EXEC is enabled as an option, execute application on answered channel */
		if ((channel = find_relative_dial_channel(dial, who)) && (answer_exec = FIND_RELATIVE_OPTION(dial, channel, TRIS_DIAL_OPTION_ANSWER_EXEC))) {
			channel->is_running_app = 1;
			answer_exec_run(dial, channel, answer_exec->app, answer_exec->args);
			channel->is_running_app = 0;
		}

		if (chan && dial->options[TRIS_DIAL_OPTION_MUSIC] && 
			!tris_strlen_zero(dial->options[TRIS_DIAL_OPTION_MUSIC])) {
			tris_moh_stop(chan);
		}
	} else if (dial->state == TRIS_DIAL_RESULT_HANGUP) {
		/* Hangup everything */
		TRIS_LIST_LOCK(&dial->channels);
		TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
			if (!channel->owner)
				continue;
			if (chan)
				tris_poll_channel_del(chan, channel->owner);
			tris_hangup(channel->owner);
			channel->owner = NULL;
		}
		TRIS_LIST_UNLOCK(&dial->channels);
	}

	return dial->state;
}

/*! \brief Dial async thread function */
static void *async_dial(void *data)
{
	struct tris_dial *dial = data;

	/* This is really really simple... we basically pass monitor_dial a NULL owner and it changes it's behavior */
	monitor_dial(dial, NULL);

	return NULL;
}

/*! \brief Execute dialing synchronously or asynchronously
 * \note Dials channels in a dial structure.
 * \return Returns dial result code. (TRYING/INVALID/FAILED/ANSWERED/TIMEOUT/UNANSWERED).
 */
enum tris_dial_result tris_dial_run(struct tris_dial *dial, struct tris_channel *chan, int async, int referid)
{
	enum tris_dial_result res = TRIS_DIAL_RESULT_TRYING;

	/* Ensure required arguments are passed */
	if (!dial || (!chan && !async)) {
		tris_debug(1, "invalid #1\n");
		return TRIS_DIAL_RESULT_INVALID;
	}

	/* If there are no channels to dial we can't very well try to dial them */
	if (TRIS_LIST_EMPTY(&dial->channels)) {
		tris_debug(1, "invalid #2\n");
		return TRIS_DIAL_RESULT_INVALID;
	}

	if (referid) {
		dial->referid = referid;
		dial->chan = chan;
	}
	/* Dial each requested channel */
	if (!begin_dial(dial, chan))
		return TRIS_DIAL_RESULT_FAILED;

	/* If we are running async spawn a thread and send it away... otherwise block here */
	if (async) {
		dial->state = TRIS_DIAL_RESULT_TRYING;
		/* Try to create a thread */
		if (tris_pthread_create(&dial->thread, NULL, async_dial, dial)) {
			/* Failed to create the thread - hangup all dialed channels and return failed */
			tris_dial_hangup(dial);
			res = TRIS_DIAL_RESULT_FAILED;
		}
	} else {
		res = monitor_dial(dial, chan);
	}

	return res;
}

int tris_dial_unset_chan(struct tris_dial *dial)
{
	dial->chan = NULL;
	return 0;
}

int tris_dial_check(struct tris_dial *dial, int referid)
{
	return (dial->referid == referid);
}

int tris_dial_send_notify(struct tris_dial *dial, const char *phonenum, enum tris_control_frame_type type)
{
	if (dial->chan && !tris_check_hangup(dial->chan) && dial->chan->seqtype) {
		snprintf(dial->chan->refer_phonenum, TRIS_MAX_EXTENSION, "%s", phonenum);
		send_control_notify(dial->chan, type, dial->referid, 0);
	}
	return 0;
}

/*! \brief Return channel that answered
 * \note Returns the Trismedia channel that answered
 * \param dial Dialing structure
 */
struct tris_channel *tris_dial_answered(struct tris_dial *dial)
{
	if (!dial)
		return NULL;

	return ((dial->state == TRIS_DIAL_RESULT_ANSWERED) ? TRIS_LIST_FIRST(&dial->channels)->owner : NULL);
}

/*! \brief Steal the channel that answered
 * \note Returns the Trismedia channel that answered and removes it from the dialing structure
 * \param dial Dialing structure
 */
struct tris_channel *tris_dial_answered_steal(struct tris_dial *dial)
{
	struct tris_channel *chan = NULL;

	if (!dial)
		return NULL;

	if (dial->state == TRIS_DIAL_RESULT_ANSWERED) {
		chan = TRIS_LIST_FIRST(&dial->channels)->owner;
		TRIS_LIST_FIRST(&dial->channels)->owner = NULL;
	}

	return chan;
}

/*! \brief Return state of dial
 * \note Returns the state of the dial attempt
 * \param dial Dialing structure
 */
enum tris_dial_result tris_dial_state(struct tris_dial *dial)
{
	return dial->state;
}

/*! \brief Cancel async thread
 * \note Cancel a running async thread
 * \param dial Dialing structure
 */
enum tris_dial_result tris_dial_join(struct tris_dial *dial)
{
	pthread_t thread;

	/* If the dial structure is not running in async, return failed */
	if (dial->thread == TRIS_PTHREADT_NULL)
		return TRIS_DIAL_RESULT_FAILED;

	/* Record thread */
	thread = dial->thread;

	/* Boom, commence locking */
	tris_mutex_lock(&dial->lock);

	/* Stop the thread */
	dial->thread = TRIS_PTHREADT_STOP;

	/* If the answered channel is running an application we have to soft hangup it, can't just poke the thread */
	TRIS_LIST_LOCK(&dial->channels);
	if (TRIS_LIST_FIRST(&dial->channels)->is_running_app) {
		struct tris_channel *chan = TRIS_LIST_FIRST(&dial->channels)->owner;
		if (chan) {
			tris_channel_lock(chan);
			tris_softhangup(chan, TRIS_SOFTHANGUP_EXPLICIT);
			tris_channel_unlock(chan);
		}
	} else {
		/* Now we signal it with SIGURG so it will break out of it's waitfor */
		pthread_kill(thread, SIGURG);
	}
	TRIS_LIST_UNLOCK(&dial->channels);

	/* Yay done with it */
	tris_mutex_unlock(&dial->lock);

	/* Finally wait for the thread to exit */
	pthread_join(thread, NULL);

	/* Yay thread is all gone */
	dial->thread = TRIS_PTHREADT_NULL;

	return dial->state;
}

/*! \brief Hangup channels
 * \note Hangup all active channels
 * \param dial Dialing structure
 */
void tris_dial_hangup(struct tris_dial *dial)
{
	struct tris_dial_channel *channel = NULL;

	if (!dial)
		return;
	
	tris_mutex_lock(&dial->lock);
	TRIS_LIST_LOCK(&dial->channels);
	TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
		if (channel->owner) {
			tris_hangup(channel->owner);
			channel->owner = NULL;
		}
	}
	TRIS_LIST_UNLOCK(&dial->channels);
	set_state(dial, TRIS_DIAL_RESULT_HANGUP);
	tris_mutex_unlock(&dial->lock);

	return;
}

/*! \brief Destroys a dialing structure
 * \note Destroys (free's) the given tris_dial structure
 * \param dial Dialing structure to free
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_destroy(struct tris_dial *dial)
{
	int i = 0;
	struct tris_dial_channel *channel = NULL;

	if (!dial)
		return -1;
	
	/* Hangup and deallocate all the dialed channels */
	TRIS_LIST_LOCK(&dial->channels);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&dial->channels, channel, list) {
		/* Disable any enabled options */
		for (i = 0; i < TRIS_DIAL_OPTION_MAX; i++) {
			if (!channel->options[i])
				continue;
			if (option_types[i].disable)
				option_types[i].disable(channel->options[i]);
			channel->options[i] = NULL;
		}
		/* Hang up channel if need be */
		if (channel->owner) {
			tris_hangup(channel->owner);
			channel->owner = NULL;
		}
		/* Free structure */
		tris_free(channel->tech);
		tris_free(channel->device);
		TRIS_LIST_REMOVE_CURRENT(list);
		tris_free(channel);
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&dial->channels);
 
	/* Disable any enabled options globally */
	for (i = 0; i < TRIS_DIAL_OPTION_MAX; i++) {
		if (!dial->options[i])
			continue;
		if (option_types[i].disable)
			option_types[i].disable(dial->options[i]);
		dial->options[i] = NULL;
	}

	/* Lock be gone! */
	tris_mutex_destroy(&dial->lock);

	/* Free structure */
	tris_free(dial);

	return 0;
}

/*! \brief Enables an option globally
 * \param dial Dial structure to enable option on
 * \param option Option to enable
 * \param data Data to pass to this option (not always needed)
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_global_enable(struct tris_dial *dial, enum tris_dial_option option, void *data)
{
	/* If the option is already enabled, return failure */
	if (dial->options[option])
		return -1;

	/* Execute enable callback if it exists, if not simply make sure the value is set */
	if (option_types[option].enable)
		dial->options[option] = option_types[option].enable(data);
	else
		dial->options[option] = (void*)1;

	return 0;
}

/*! \brief Helper function for finding a channel in a dial structure based on number
 */
static struct tris_dial_channel *find_dial_channel(struct tris_dial *dial, int num)
{
	struct tris_dial_channel *channel = TRIS_LIST_LAST(&dial->channels);

	/* We can try to predict programmer behavior, the last channel they added is probably the one they wanted to modify */
	if (channel->num == num)
		return channel;

	/* Hrm not at the end... looking through the list it is! */
	TRIS_LIST_LOCK(&dial->channels);
	TRIS_LIST_TRAVERSE(&dial->channels, channel, list) {
		if (channel->num == num)
			break;
	}
	TRIS_LIST_UNLOCK(&dial->channels);
	
	return channel;
}

/*! \brief Enables an option per channel
 * \param dial Dial structure
 * \param num Channel number to enable option on
 * \param option Option to enable
 * \param data Data to pass to this option (not always needed)
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_enable(struct tris_dial *dial, int num, enum tris_dial_option option, void *data)
{
	struct tris_dial_channel *channel = NULL;

	/* Ensure we have required arguments */
	if (!dial || TRIS_LIST_EMPTY(&dial->channels))
		return -1;

	if (!(channel = find_dial_channel(dial, num)))
		return -1;

	/* If the option is already enabled, return failure */
	if (channel->options[option])
		return -1;

	/* Execute enable callback if it exists, if not simply make sure the value is set */
	if (option_types[option].enable)
		channel->options[option] = option_types[option].enable(data);
	else
		channel->options[option] = (void*)1;

	return 0;
}

/*! \brief Disables an option globally
 * \param dial Dial structure to disable option on
 * \param option Option to disable
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_global_disable(struct tris_dial *dial, enum tris_dial_option option)
{
	/* If the option is not enabled, return failure */
	if (!dial->options[option]) {
		return -1;
	}

	/* Execute callback of option to disable if it exists */
	if (option_types[option].disable)
		option_types[option].disable(dial->options[option]);

	/* Finally disable option on the structure */
	dial->options[option] = NULL;

	return 0;
}

/*! \brief Disables an option per channel
 * \param dial Dial structure
 * \param num Channel number to disable option on
 * \param option Option to disable
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_disable(struct tris_dial *dial, int num, enum tris_dial_option option)
{
	struct tris_dial_channel *channel = NULL;

	/* Ensure we have required arguments */
	if (!dial || TRIS_LIST_EMPTY(&dial->channels))
		return -1;

	if (!(channel = find_dial_channel(dial, num)))
		return -1;

	/* If the option is not enabled, return failure */
	if (!channel->options[option])
		return -1;

	/* Execute callback of option to disable it if it exists */
	if (option_types[option].disable)
		option_types[option].disable(channel->options[option]);

	/* Finally disable the option on the structure */
	channel->options[option] = NULL;

	return 0;
}

void tris_dial_set_state_callback(struct tris_dial *dial, tris_dial_state_callback callback)
{
	dial->state_callback = callback;
}

/*! \brief Set the maximum time (globally) allowed for trying to ring phones
 * \param dial The dial structure to apply the time limit to
 * \param timeout Maximum time allowed
 * \return nothing
 */
void tris_dial_set_global_timeout(struct tris_dial *dial, int timeout)
{
	dial->timeout = timeout;

	if (dial->timeout > 0 && (dial->actual_timeout > dial->timeout || dial->actual_timeout == -1))
		dial->actual_timeout = dial->timeout;

	return;
}

/*! \brief Set the maximum time (per channel) allowed for trying to ring the phone
 * \param dial The dial structure the channel belongs to
 * \param num Channel number to set timeout on
 * \param timeout Maximum time allowed
 * \return nothing
 */
void tris_dial_set_timeout(struct tris_dial *dial, int num, int timeout)
{
	struct tris_dial_channel *channel = NULL;

	if (!(channel = find_dial_channel(dial, num)))
		return;

	channel->timeout = timeout;

	if (channel->timeout > 0 && (dial->actual_timeout > channel->timeout || dial->actual_timeout == -1))
		dial->actual_timeout = channel->timeout;

	return;
}
