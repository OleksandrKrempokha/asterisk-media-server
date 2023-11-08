/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
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
 * \brief Routines implementing call features as call pickup, parking and transfer
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 249933 $")

#include "trismedia/_private.h"

#include <pthread.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <netinet/in.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/causes.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/app.h"
#include "trismedia/say.h"
#include "trismedia/features.h"
#include "trismedia/musiconhold.h"
#include "trismedia/config.h"
#include "trismedia/cli.h"
#include "trismedia/manager.h"
#include "trismedia/utils.h"
#include "trismedia/adsi.h"
#include "trismedia/devicestate.h"
#include "trismedia/monitor.h"
#include "trismedia/audiohook.h"
#include "trismedia/global_datastores.h"
#include "trismedia/astobj2.h"
#include "trismedia/paths.h"

/*** DOCUMENTATION
	<application name="Bridge" language="en_US">
		<synopsis>
			Bridge two channels.
		</synopsis>
		<syntax>
			<parameter name="channel" required="true">
				<para>The current channel is bridged to the specified <replaceable>channel</replaceable>.</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="p">
						<para>Play a courtesy tone to <replaceable>channel</replaceable>.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Allows the ability to bridge two channels via the dialplan.</para>
			<para>This application sets the following channel variable upon completion:</para>
			<variablelist>
				<variable name="BRIDGERESULT">
					<para>The result of the bridge attempt as a text string.</para>
					<value name="SUCCESS" />
					<value name="FAILURE" />
					<value name="LOOP" />
					<value name="NONEXISTENT" />
					<value name="INCOMPATIBLE" />
				</variable>
			</variablelist>
		</description>
	</application>
	<application name="ParkedCall" language="en_US">
		<synopsis>
			Answer a parked call.
		</synopsis>
		<syntax>
			<parameter name="exten" required="true" />
		</syntax>
		<description>
			<para>Used to connect to a parked call. This application is always
			registered internally and does not need to be explicitly added
			into the dialplan, although you should include the <literal>parkedcalls</literal>
			context. If no extension is provided, then the first available
			parked call will be acquired.</para>
		</description>
		<see-also>
			<ref type="application">Park</ref>
			<ref type="application">ParkAndAnnounce</ref>
		</see-also>
	</application>
	<application name="Park" language="en_US">
		<synopsis>
			Park yourself.
		</synopsis>
		<syntax>
			<parameter name="timeout">
				<para>A custom parking timeout for this parked call.</para>
			</parameter>
			<parameter name="return_context">
				<para>The context to return the call to after it times out.</para>
			</parameter>
			<parameter name="return_exten">
				<para>The extension to return the call to after it times out.</para>
			</parameter>
			<parameter name="return_priority">
				<para>The priority to return the call to after it times out.</para>
			</parameter>
			<parameter name="options">
				<para>A list of options for this parked call.</para>
				<optionlist>
					<option name="r">
						<para>Send ringing instead of MOH to the parked call.</para>
					</option>
					<option name="R">
						<para>Randomize the selection of a parking space.</para>
					</option>
					<option name="s">
						<para>Silence announcement of the parking space number.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Used to park yourself (typically in combination with a supervised
			transfer to know the parking space). This application is always
			registered internally and does not need to be explicitly added
			into the dialplan, although you should include the <literal>parkedcalls</literal>
			context (or the context specified in <filename>features.conf</filename>).</para>
			<para>If you set the <variable>PARKINGLOT</variable> variable, the call will be parked
			in the specifed parking context. Note setting this variable overrides the <variable>
			PARKINGLOT</variable> set by the <literal>CHANNEL</literal> function.</para>
			<para>If you set the <variable>PARKINGEXTEN</variable> variable to an extension in your
			parking context, Park() will park the call on that extension, unless
			it already exists. In that case, execution will continue at next priority.</para>
		</description>
		<see-also>
			<ref type="application">ParkAndAnnounce</ref>
			<ref type="application">ParkedCall</ref>
		</see-also>
	</application>
 ***/

#define DEFAULT_PARK_TIME 45000
#define DEFAULT_TRANSFER_DIGIT_TIMEOUT 5000
#define DEFAULT_FEATURE_DIGIT_TIMEOUT 1000
#define DEFAULT_NOANSWER_TIMEOUT_ATTENDED_TRANSFER 120000
#define DEFAULT_PARKINGLOT "default"			/*!< Default parking lot */
#define DEFAULT_ATXFER_DROP_CALL 0
#define DEFAULT_ATXFER_LOOP_DELAY 10000
#define DEFAULT_ATXFER_CALLBACK_RETRIES 2

#define TRIS_MAX_WATCHERS 256

#define LOCK_IF_NEEDED(lock, needed) do { \
	if (needed) \
		tris_channel_lock(lock); \
	} while(0)

#define UNLOCK_IF_NEEDED(lock, needed) do { \
	if (needed) \
		tris_channel_unlock(lock); \
	} while (0)

#define MAX_DIAL_FEATURE_OPTIONS 30

struct feature_group_exten {
	TRIS_LIST_ENTRY(feature_group_exten) entry;
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(exten);
	);
	struct tris_call_feature *feature;
};

struct feature_group {
	TRIS_LIST_ENTRY(feature_group) entry;
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(gname);
	);
	TRIS_LIST_HEAD_NOLOCK(, feature_group_exten) features;
};

static TRIS_RWLIST_HEAD_STATIC(feature_groups, feature_group);

static char *parkedcall = "ParkedCall";

static char pickup_ext[TRIS_MAX_EXTENSION];                 /*!< Call pickup extension */

/*! \brief Description of one parked call, added to a list while active, then removed.
	The list belongs to a parkinglot 
*/
struct parkeduser {
	struct tris_channel *chan;                   /*!< Parking channel */
	struct timeval start;                       /*!< Time the parking started */
	int parkingnum;                             /*!< Parking lot */
	char parkingexten[TRIS_MAX_EXTENSION];       /*!< If set beforehand, parking extension used for this call */
	char context[TRIS_MAX_CONTEXT];              /*!< Where to go if our parking time expires */
	char exten[TRIS_MAX_EXTENSION];
	int priority;
	int parkingtime;                            /*!< Maximum length in parking lot before return */
	unsigned int notquiteyet:1;
	unsigned int options_specified:1;
	char peername[1024];
	unsigned char moh_trys;
	struct tris_parkinglot *parkinglot;
	TRIS_LIST_ENTRY(parkeduser) list;
};

/*! \brief Structure for parking lots which are put in a container. */
struct tris_parkinglot {
	char name[TRIS_MAX_CONTEXT];
	char parking_con[TRIS_MAX_EXTENSION];		/*!< Context for which parking is made accessible */
	char parking_con_dial[TRIS_MAX_EXTENSION];	/*!< Context for dialback for parking (KLUDGE) */
	int parking_start;				/*!< First available extension for parking */
	int parking_stop;				/*!< Last available extension for parking */
	int parking_offset;
	int parkfindnext;
	int parkingtime;				/*!< Default parking time */
	char mohclass[MAX_MUSICCLASS];                  /*!< Music class used for parking */
	int parkaddhints;                               /*!< Add parking hints automatically */
	int parkedcalltransfers;                        /*!< Enable DTMF based transfers on bridge when picking up parked calls */
	int parkedcallreparking;                        /*!< Enable DTMF based parking on bridge when picking up parked calls */
	int parkedcallhangup;                           /*!< Enable DTMF based hangup on a bridge when pickup up parked calls */
	int parkedcallrecording;                        /*!< Enable DTMF based recording on a bridge when picking up parked calls */
	TRIS_LIST_HEAD(parkinglot_parklist, parkeduser) parkings; /*!< List of active parkings in this parkinglot */
};

/*! \brief The list of parking lots configured. Always at least one  - the default parking lot */
static struct ao2_container *parkinglots;
 
struct tris_parkinglot *default_parkinglot;
char parking_ext[TRIS_MAX_EXTENSION];            /*!< Extension you type to park the call */

static char courtesytone[256];                             /*!< Courtesy tone */
static int parkedplay = 0;                                 /*!< Who to play the courtesy tone to */
static char xfersound[256];                                /*!< Call transfer sound */
static char xferfailsound[256];                            /*!< Call transfer failure sound */
static char pickupsound[256];                              /*!< Pickup sound */
static char pickupfailsound[256];                          /*!< Pickup failure sound */

static int adsipark;

static int transferdigittimeout;
static int featuredigittimeout;
static int comebacktoorigin = 1;

static int atxfernoanswertimeout;
static unsigned int atxferdropcall;
static unsigned int atxferloopdelay;
static unsigned int atxfercallbackretries;

static char *registrar = "features";		   /*!< Registrar for operations */

/* module and CLI command definitions */
static char *parkcall = PARK_APP_NAME;

static struct tris_app *monitor_app = NULL;
static int monitor_ok = 1;

static struct tris_app *mixmonitor_app = NULL;
static int mixmonitor_ok = 1;

static struct tris_app *stopmixmonitor_app = NULL;
static int stopmixmonitor_ok = 1;

static pthread_t parking_thread;
struct tris_dial_features {
	struct tris_flags features_caller;
	struct tris_flags features_callee;
	int is_caller;
};

static void *dial_features_duplicate(void *data)
{
	struct tris_dial_features *df = data, *df_copy;
 
 	if (!(df_copy = tris_calloc(1, sizeof(*df)))) {
 		return NULL;
 	}
 
 	memcpy(df_copy, df, sizeof(*df));
 
 	return df_copy;
 }
 
 static void dial_features_destroy(void *data)
 {
 	struct tris_dial_features *df = data;
 	if (df) {
 		tris_free(df);
 	}
 }
 
 const struct tris_datastore_info dial_features_info = {
 	.type = "dial-features",
 	.destroy = dial_features_destroy,
 	.duplicate = dial_features_duplicate,
 };
 
/* Forward declarations */
static struct tris_parkinglot *parkinglot_addref(struct tris_parkinglot *parkinglot);
static void parkinglot_unref(struct tris_parkinglot *parkinglot);
static void parkinglot_destroy(void *obj);
int manage_parkinglot(struct tris_parkinglot *curlot, fd_set *rfds, fd_set *efds, fd_set *nrfds, fd_set *nefds, int *fs, int *max);
struct tris_parkinglot *find_parkinglot(const char *name);
int tris_monitor_stop_for_builtin(struct tris_channel *chan, int need_lock);

tris_sql_select_query_execute_f tris_sql_select_query_execute = NULL;

const char *tris_parking_ext(void)
{
	return parking_ext;
}

const char *tris_pickup_ext(void)
{
	return pickup_ext;
}

struct tris_bridge_thread_obj 
{
	struct tris_bridge_config bconfig;
	struct tris_channel *chan;
	struct tris_channel *peer;
	unsigned int return_to_pbx:1;
};

static int parkinglot_hash_cb(const void *obj, const int flags)
{
	const struct tris_parkinglot *parkinglot = obj;

	return tris_str_case_hash(parkinglot->name);
}

static int parkinglot_cmp_cb(void *obj, void *arg, int flags)
{
	struct tris_parkinglot *parkinglot = obj, *parkinglot2 = arg;

	return !strcasecmp(parkinglot->name, parkinglot2->name) ? CMP_MATCH | CMP_STOP : 0;
}

/*!
 * \brief store context, extension and priority 
 * \param chan, context, ext, pri
*/
static void set_c_e_p(struct tris_channel *chan, const char *context, const char *ext, int pri)
{
	tris_copy_string(chan->context, context, sizeof(chan->context));
	tris_copy_string(chan->exten, ext, sizeof(chan->exten));
	chan->priority = pri;
}

/*!
 * \brief Check goto on transfer
 * \param chan
 *
 * Check if channel has 'GOTO_ON_BLINDXFR' set, if not exit.
 * When found make sure the types are compatible. Check if channel is valid
 * if so start the new channel else hangup the call. 
*/
static void check_goto_on_transfer(struct tris_channel *chan) 
{
	struct tris_channel *xferchan;
	const char *val = pbx_builtin_getvar_helper(chan, "GOTO_ON_BLINDXFR");
	char *x, *goto_on_transfer;

	if (tris_strlen_zero(val))
		return;

	goto_on_transfer = tris_strdupa(val);

	if (!(xferchan = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, "%s", chan->name)))
		return;

	for (x = goto_on_transfer; x && *x; x++) {
		if (*x == '^')
			*x = '|';
	}
	/* Make formats okay */
	xferchan->readformat = chan->readformat;
	xferchan->writeformat = chan->writeformat;
	tris_channel_masquerade(xferchan, chan);
	tris_parseable_goto(xferchan, goto_on_transfer);
	xferchan->_state = TRIS_STATE_UP;
	tris_clear_flag(xferchan, TRIS_FLAGS_ALL);	
	xferchan->_softhangup = 0;
	tris_channel_lock(xferchan);
	tris_do_masquerade(xferchan);
	tris_channel_unlock(xferchan);
	tris_pbx_start(xferchan);
}

static struct tris_channel *feature_request_and_dial(struct tris_channel *caller, struct tris_channel *transferee,
		const char *type, int format, void *data, int timeout, int *outstate, const char *cid_num, const char *cid_name,
		int igncallerstate, const char *language, int ringing, int notifycaller);

static struct tris_channel *feature_dial_byrefer(struct tris_channel *caller, struct tris_channel *transferee, 
		const char *type, int format, void *data, int timeout, int *outstate, const char *cid_num, const char *cid_name, 
		int igncallerstate, const char *language, int ringing, int notifycaller, const char *caller_context,
		struct tris_bridge_config *config, const char *dst, int *holdstate);

/*!
 * \brief bridge the call 
 * \param data thread bridge.
 *
 * Set Last Data for respective channels, reset cdr for channels
 * bridge call, check if we're going back to dialplan
 * if not hangup both legs of the call
*/
static void *bridge_call_thread(void *data)
{
	struct tris_bridge_thread_obj *tobj = data;
	int res;

	tobj->chan->appl = !tobj->return_to_pbx ? "Transferred Call" : "ManagerBridge";
	tobj->chan->data = tobj->peer->name;
	tobj->peer->appl = !tobj->return_to_pbx ? "Transferred Call" : "ManagerBridge";
	tobj->peer->data = tobj->chan->name;

	tris_bridge_call(tobj->peer, tobj->chan, &tobj->bconfig);

	if (tobj->return_to_pbx) {
		if (!tris_check_hangup(tobj->peer)) {
			tris_log(LOG_VERBOSE, "putting peer %s into PBX again\n", tobj->peer->name);
			res = tris_pbx_start(tobj->peer);
			if (res != TRIS_PBX_SUCCESS)
				tris_log(LOG_WARNING, "FAILED continuing PBX on peer %s\n", tobj->peer->name);
		} else
			tris_hangup(tobj->peer);
		if (!tris_check_hangup(tobj->chan)) {
			tris_log(LOG_VERBOSE, "putting chan %s into PBX again\n", tobj->chan->name);
			res = tris_pbx_start(tobj->chan);
			if (res != TRIS_PBX_SUCCESS)
				tris_log(LOG_WARNING, "FAILED continuing PBX on chan %s\n", tobj->chan->name);
		} else
			tris_hangup(tobj->chan);
	} else {
		tris_hangup(tobj->chan);
		tris_hangup(tobj->peer);
	}

	tris_free(tobj);

	return NULL;
}

/*!
 * \brief create thread for the parked call
 * \param data
 *
 * Create thread and attributes, call bridge_call_thread
*/
static void bridge_call_thread_launch(void *data) 
{
	pthread_t thread;
	pthread_attr_t attr;
	struct sched_param sched;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	tris_pthread_create(&thread, &attr, bridge_call_thread, data);
	pthread_attr_destroy(&attr);
	memset(&sched, 0, sizeof(sched));
	pthread_setschedparam(thread, SCHED_RR, &sched);
}

/*!
 * \brief Announce call parking by ADSI
 * \param chan .
 * \param parkingexten .
 * Create message to show for ADSI, display message.
 * \retval 0 on success.
 * \retval -1 on failure.
*/
static int adsi_announce_park(struct tris_channel *chan, char *parkingexten)
{
	int res;
	int justify[5] = {ADSI_JUST_CENT, ADSI_JUST_CENT, ADSI_JUST_CENT, ADSI_JUST_CENT};
	char tmp[256];
	char *message[5] = {NULL, NULL, NULL, NULL, NULL};

	snprintf(tmp, sizeof(tmp), "Parked on %s", parkingexten);
	message[0] = tmp;
	res = tris_adsi_load_session(chan, NULL, 0, 1);
	if (res == -1)
		return res;
	return tris_adsi_print(chan, message, justify, 1);
}

/*! \brief Find parking lot name from channel */
static const char *findparkinglotname(struct tris_channel *chan)
{
	const char *temp, *parkinglot = NULL;

	/* Check if the channel has a parking lot */
	if (!tris_strlen_zero(chan->parkinglot))
		parkinglot = chan->parkinglot;

	/* Channel variables override everything */

	if ((temp  = pbx_builtin_getvar_helper(chan, "PARKINGLOT")))
		return temp;

	return parkinglot;
}

/*! \brief Notify metermaids that we've changed an extension */
static void notify_metermaids(const char *exten, char *context, enum tris_device_state state)
{
	tris_debug(4, "Notification of state change to metermaids %s@%s\n to state '%s'", 
		exten, context, tris_devstate2str(state));

	tris_devstate_changed(state, "park:%s@%s", exten, context);
}

/*! \brief metermaids callback from devicestate.c */
static enum tris_device_state metermaidstate(const char *data)
{
	char *context;
	char *exten;

	context = tris_strdupa(data);

	exten = strsep(&context, "@");
	if (!context)
		return TRIS_DEVICE_INVALID;
	
	tris_debug(4, "Checking state of exten %s in context %s\n", exten, context);

	if (!tris_exists_extension(NULL, context, exten, 1, NULL))
		return TRIS_DEVICE_NOT_INUSE;

	return TRIS_DEVICE_INUSE;
}

/*! Options to pass to park_call_full */
enum tris_park_call_options {
	/*! Provide ringing to the parked caller instead of music on hold */
	TRIS_PARK_OPT_RINGING =   (1 << 0),
	/*! Randomly choose a parking spot for the caller instead of choosing
	 *  the first one that is available. */
	TRIS_PARK_OPT_RANDOMIZE = (1 << 1),
	/*! Do not announce the parking number */
	TRIS_PARK_OPT_SILENCE = (1 << 2),
};

struct tris_park_call_args {
	/*! How long to wait in the parking lot before the call gets sent back
	 *  to the specified return extension (or a best guess at where it came
	 *  from if not explicitly specified). */
	int timeout;
	/*! An output parameter to store the parking space where the parked caller
	 *  was placed. */
	int *extout;
	const char *orig_chan_name;
	const char *return_con;
	const char *return_ext;
	int return_pri;
	uint32_t flags;
	/*! Parked user that has already obtained a parking space */
	struct parkeduser *pu;
};

static struct parkeduser *park_space_reserve(struct tris_channel *chan,
 struct tris_channel *peer, struct tris_park_call_args *args)
{
	struct parkeduser *pu;
	int i, parking_space = -1, parking_range;
	const char *parkinglotname = NULL;
	const char *parkingexten;
	struct tris_parkinglot *parkinglot = NULL;
	
	if (peer)
		parkinglotname = findparkinglotname(peer);

	if (parkinglotname) {
		if (option_debug)
			tris_log(LOG_DEBUG, "Found chanvar Parkinglot: %s\n", parkinglotname);
		parkinglot = find_parkinglot(parkinglotname);	
	}
	if (!parkinglot) {
		parkinglot = parkinglot_addref(default_parkinglot);
	}

	if (option_debug)
		tris_log(LOG_DEBUG, "Parkinglot: %s\n", parkinglot->name);

	/* Allocate memory for parking data */
	if (!(pu = tris_calloc(1, sizeof(*pu)))) {
		parkinglot_unref(parkinglot);
		return NULL;
	}

	/* Lock parking list */
	TRIS_LIST_LOCK(&parkinglot->parkings);
	/* Check for channel variable PARKINGEXTEN */
	tris_channel_lock(chan);
	parkingexten = tris_strdupa(S_OR(pbx_builtin_getvar_helper(chan, "PARKINGEXTEN"), ""));
	tris_channel_unlock(chan);
	if (!tris_strlen_zero(parkingexten)) {
		/*!\note The API forces us to specify a numeric parking slot, even
		 * though the architecture would tend to support non-numeric extensions
		 * (as are possible with SIP, for example).  Hence, we enforce that
		 * limitation here.  If extout was not numeric, we could permit
		 * arbitrary non-numeric extensions.
		 */
        if (sscanf(parkingexten, "%30d", &parking_space) != 1 || parking_space < 0) {
			TRIS_LIST_UNLOCK(&parkinglot->parkings);
			parkinglot_unref(parkinglot);
            free(pu);
            tris_log(LOG_WARNING, "PARKINGEXTEN does not indicate a valid parking slot: '%s'.\n", parkingexten);
            return NULL;
        }
        snprintf(pu->parkingexten, sizeof(pu->parkingexten), "%d", parking_space);

		if (tris_exists_extension(NULL, parkinglot->parking_con, pu->parkingexten, 1, NULL)) {
			tris_log(LOG_WARNING, "Requested parking extension already exists: %s@%s\n", parkingexten, parkinglot->parking_con);
			TRIS_LIST_UNLOCK(&parkinglot->parkings);
			parkinglot_unref(parkinglot);
			tris_free(pu);
			return NULL;
		}
	} else {
		int start;
		struct parkeduser *cur = NULL;

		/* Select parking space within range */
		parking_range = parkinglot->parking_stop - parkinglot->parking_start + 1;

		if (tris_test_flag(args, TRIS_PARK_OPT_RANDOMIZE)) {
			start = tris_random() % (parkinglot->parking_stop - parkinglot->parking_start + 1);
		} else {
			start = parkinglot->parking_start;
		}

		for (i = start; 1; i++) {
			if (i == parkinglot->parking_stop + 1) {
				i = parkinglot->parking_start - 1;
				break;
			}

			TRIS_LIST_TRAVERSE(&parkinglot->parkings, cur, list) {
				if (cur->parkingnum == i) {
					break;
				}
			}
			if (!cur) {
				parking_space = i;
				break;
			}
		}

		if (i == start - 1 && cur) {
			tris_log(LOG_WARNING, "No more parking spaces\n");
			tris_free(pu);
			TRIS_LIST_UNLOCK(&parkinglot->parkings);
			parkinglot_unref(parkinglot);
			return NULL;
		}
		/* Set pointer for next parking */
		if (parkinglot->parkfindnext) 
			parkinglot->parking_offset = parking_space - parkinglot->parking_start + 1;
		snprintf(pu->parkingexten, sizeof(pu->parkingexten), "%d", parking_space);
	}

	pu->notquiteyet = 1;
	pu->parkingnum = parking_space;
	pu->parkinglot = parkinglot_addref(parkinglot);
	TRIS_LIST_INSERT_TAIL(&parkinglot->parkings, pu, list);
	parkinglot_unref(parkinglot);

	return pu;
}

/* Park a call */
static int park_call_full(struct tris_channel *chan, struct tris_channel *peer, struct tris_park_call_args *args)
{
	struct tris_context *con;
	int parkingnum_copy;
	struct parkeduser *pu = args->pu;
	const char *event_from;

	if (pu == NULL)
		pu = park_space_reserve(chan, peer, args);
	if (pu == NULL)
		return 1; /* Continue execution if possible */

	snprintf(pu->parkingexten, sizeof(pu->parkingexten), "%d", pu->parkingnum);
	
	chan->appl = "Parked Call";
	chan->data = NULL; 

	pu->chan = chan;
	
	/* Put the parked channel on hold if we have two different channels */
	if (chan != peer) {
		if (tris_test_flag(args, TRIS_PARK_OPT_RINGING)) {
			tris_indicate(pu->chan, TRIS_CONTROL_RINGING);
		} else {
			tris_indicate_data(pu->chan, TRIS_CONTROL_HOLD, 
				S_OR(pu->parkinglot->mohclass, NULL),
				!tris_strlen_zero(pu->parkinglot->mohclass) ? strlen(pu->parkinglot->mohclass) + 1 : 0);
		}
	}
	
	pu->start = tris_tvnow();
	pu->parkingtime = (args->timeout > 0) ? args->timeout : pu->parkinglot->parkingtime;
	parkingnum_copy = pu->parkingnum;
	if (args->extout)
		*(args->extout) = pu->parkingnum;

	if (peer) { 
		/* This is so ugly that it hurts, but implementing get_base_channel() on local channels
			could have ugly side effects.  We could have transferer<->local,1<->local,2<->parking
			and we need the callback name to be that of transferer.  Since local,1/2 have the same
			name we can be tricky and just grab the bridged channel from the other side of the local
		*/
		if (!strcasecmp(peer->tech->type, "Local")) {
			struct tris_channel *tmpchan, *base_peer;
			char other_side[TRIS_CHANNEL_NAME];
			char *c;
			tris_copy_string(other_side, S_OR(args->orig_chan_name, peer->name), sizeof(other_side));
			if ((c = strrchr(other_side, ';'))) {
				*++c = '1';
			}
			if ((tmpchan = tris_get_channel_by_name_locked(other_side))) {
				if ((base_peer = tris_bridged_channel(tmpchan))) {
					tris_copy_string(pu->peername, base_peer->name, sizeof(pu->peername));
				}
				tris_channel_unlock(tmpchan);
			}
		} else {
			tris_copy_string(pu->peername, S_OR(args->orig_chan_name, peer->name), sizeof(pu->peername));
		}
	}

	/* Remember what had been dialed, so that if the parking
	   expires, we try to come back to the same place */

	pu->options_specified = (!tris_strlen_zero(args->return_con) || !tris_strlen_zero(args->return_ext) || args->return_pri);

	/* If extension has options specified, they override all other possibilities
	such as the returntoorigin flag and transferred context. Information on
	extension options is lost here, so we set a flag */

	tris_copy_string(pu->context, 
		S_OR(args->return_con, S_OR(chan->macrocontext, chan->context)), 
		sizeof(pu->context));
	tris_copy_string(pu->exten, 
		S_OR(args->return_ext, S_OR(chan->macroexten, chan->exten)), 
		sizeof(pu->exten));
	pu->priority = args->return_pri ? args->return_pri : 
		(chan->macropriority ? chan->macropriority : chan->priority);

	/* If parking a channel directly, don't quiet yet get parking running on it.
	 * All parking lot entries are put into the parking lot with notquiteyet on. */
	if (peer != chan) 
		pu->notquiteyet = 0;

	/* Wake up the (presumably select()ing) thread */
	pthread_kill(parking_thread, SIGURG);
	tris_verb(2, "Parked %s on %d (lot %s). Will timeout back to extension [%s] %s, %d in %d seconds\n", pu->chan->name, pu->parkingnum, pu->parkinglot->name, pu->context, pu->exten, pu->priority, (pu->parkingtime/1000));

	if (peer) {
		event_from = peer->name;
	} else {
		event_from = pbx_builtin_getvar_helper(chan, "BLINDTRANSFER");
	}

	manager_event(EVENT_FLAG_CALL, "ParkedCall",
		"Exten: %s\r\n"
		"Channel: %s\r\n"
		"Parkinglot: %s\r\n"
		"From: %s\r\n"
		"Timeout: %ld\r\n"
		"CallerIDNum: %s\r\n"
		"CallerIDName: %s\r\n"
		"Uniqueid: %s\r\n",
		pu->parkingexten, pu->chan->name, pu->parkinglot->name, event_from ? event_from : "",
		(long)pu->start.tv_sec + (long)(pu->parkingtime/1000) - (long)time(NULL),
		S_OR(pu->chan->cid.cid_num, "<unknown>"),
		S_OR(pu->chan->cid.cid_name, "<unknown>"),
		pu->chan->uniqueid
		);

	if (peer && adsipark && tris_adsi_available(peer)) {
		adsi_announce_park(peer, pu->parkingexten);	/* Only supports parking numbers */
		tris_adsi_unload_session(peer);
	}

	con = tris_context_find_or_create(NULL, NULL, pu->parkinglot->parking_con, registrar);
	if (!con)	/* Still no context? Bad */
		tris_log(LOG_ERROR, "Parking context '%s' does not exist and unable to create\n", pu->parkinglot->parking_con);
	if (con) {
		if (!tris_add_extension2(con, 1, pu->parkingexten, 1, NULL, NULL, parkedcall, tris_strdup(pu->parkingexten), tris_free_ptr, registrar))
			notify_metermaids(pu->parkingexten, pu->parkinglot->parking_con, TRIS_DEVICE_INUSE);
	}

	TRIS_LIST_UNLOCK(&pu->parkinglot->parkings);

	/* Only say number if it's a number and the channel hasn't been masqueraded away */
	if (peer && !tris_test_flag(args, TRIS_PARK_OPT_SILENCE) && (tris_strlen_zero(args->orig_chan_name) || !strcasecmp(peer->name, args->orig_chan_name))) {
		/* If a channel is masqueraded into peer while playing back the parking slot number do not continue playing it back. This is the case if an attended transfer occurs. */
		tris_set_flag(peer, TRIS_FLAG_MASQ_NOSTREAM);
		/* Tell the peer channel the number of the parking space */
		tris_say_digits(peer, pu->parkingnum, "", peer->language);
		tris_clear_flag(peer, TRIS_FLAG_MASQ_NOSTREAM);
	}
	if (peer == chan) { /* pu->notquiteyet = 1 */
		/* Wake up parking thread if we're really done */
		tris_indicate_data(pu->chan, TRIS_CONTROL_HOLD, 
			S_OR(pu->parkinglot->mohclass, NULL),
			!tris_strlen_zero(pu->parkinglot->mohclass) ? strlen(pu->parkinglot->mohclass) + 1 : 0);
		pu->notquiteyet = 0;
		pthread_kill(parking_thread, SIGURG);
	}
	return 0;
}

/*! \brief Park a call */
int tris_park_call(struct tris_channel *chan, struct tris_channel *peer, int timeout, int *extout)
{
	struct tris_park_call_args args = {
		.timeout = timeout,
		.extout = extout,
	};

	return park_call_full(chan, peer, &args);
}

static int masq_park_call(struct tris_channel *rchan, struct tris_channel *peer, int timeout, int *extout, int play_announcement, struct tris_park_call_args *args)
{
	struct tris_channel *chan;
	struct tris_frame *f;
	int park_status;
	struct tris_park_call_args park_args = {0,};

	if (!args) {
		args = &park_args;
		args->timeout = timeout;
		args->extout = extout;
	}

	if ((args->pu = park_space_reserve(rchan, peer, args)) == NULL) {
		if (peer)
			tris_stream_and_wait(peer, "beep", "");
		return TRIS_FEATURE_RETURN_PARKFAILED;
	}

	/* Make a new, fake channel that we'll use to masquerade in the real one */
	if (!(chan = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, rchan->accountcode, rchan->exten, rchan->context, rchan->amaflags, "Parked/%s",rchan->name))) {
		tris_log(LOG_WARNING, "Unable to create parked channel\n");
		return -1;
	}

	/* Make formats okay */
	chan->readformat = rchan->readformat;
	chan->writeformat = rchan->writeformat;
	tris_channel_masquerade(chan, rchan);

	/* Setup the extensions and such */
	set_c_e_p(chan, rchan->context, rchan->exten, rchan->priority);

	/* Setup the macro extension and such */
	tris_copy_string(chan->macrocontext,rchan->macrocontext,sizeof(chan->macrocontext));
	tris_copy_string(chan->macroexten,rchan->macroexten,sizeof(chan->macroexten));
	chan->macropriority = rchan->macropriority;

	/* Make the masq execute */
	if ((f = tris_read(chan)))
		tris_frfree(f);

	if (peer == rchan) {
		peer = chan;
	}

	if (peer && (!play_announcement && args == &park_args)) {
		args->orig_chan_name = tris_strdupa(peer->name);
	}

	park_status = park_call_full(chan, peer, args);
	if (park_status == 1) {
	/* would be nice to play "invalid parking extension" */
		tris_hangup(chan);
		return -1;
	}

	return 0;
}

/* Park call via masquraded channel */
int tris_masq_park_call(struct tris_channel *rchan, struct tris_channel *peer, int timeout, int *extout)
{
	return masq_park_call(rchan, peer, timeout, extout, 0, NULL);
}

static int masq_park_call_announce_args(struct tris_channel *rchan, struct tris_channel *peer, struct tris_park_call_args *args)
{
	return masq_park_call(rchan, peer, 0, NULL, 1, args);
}

static int masq_park_call_announce(struct tris_channel *rchan, struct tris_channel *peer, int timeout, int *extout)
{
	return masq_park_call(rchan, peer, timeout, extout, 1, NULL);
}

#define FEATURE_SENSE_CHAN	(1 << 0)
#define FEATURE_SENSE_PEER	(1 << 1)

/*! 
 * \brief set caller and callee according to the direction 
 * \param caller, callee, peer, chan, sense
 *
 * Detect who triggered feature and set callee/caller variables accordingly
*/
void set_peers(struct tris_channel **caller, struct tris_channel **callee,
	struct tris_channel *peer, struct tris_channel *chan, int sense)
{
	if (sense == FEATURE_SENSE_PEER) {
		*caller = peer;
		*callee = chan;
	} else {
		*callee = peer;
		*caller = chan;
	}
}

/*! 
 * \brief support routing for one touch call parking
 * \param chan channel parking call
 * \param peer channel to be parked
 * \param config unsed
 * \param code unused
 * \param sense feature options
 *
 * \param data
 * Setup channel, set return exten,priority to 's,1'
 * answer chan, sleep chan, park call
*/
static int builtin_parkcall(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_channel *parker;
	struct tris_channel *parkee;
	int res = 0;

	set_peers(&parker, &parkee, peer, chan, sense);
	/* we used to set chan's exten and priority to "s" and 1
	   here, but this generates (in some cases) an invalid
	   extension, and if "s" exists, could errantly
	   cause execution of extensions you don't expect. It
	   makes more sense to let nature take its course
	   when chan finishes, and let the pbx do its thing
	   and hang up when the park is over.
	*/
	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);
	if (!res)
		res = tris_safe_sleep(chan, 1000);

	if (!res) { /* one direction used to call park_call.... */
		res = masq_park_call_announce(parkee, parker, 0, NULL);
		/* PBX should hangup zombie channel if a masquerade actually occurred (res=0) */
	}

	return res;
}

/*! \brief Play message to both caller and callee in bridged call, plays synchronously, autoservicing the
	other channel during the message, so please don't use this for very long messages
 */
static int play_message_in_bridged_call(struct tris_channel *caller_chan, struct tris_channel *callee_chan, const char *audiofile)
{
	/* First play for caller, put other channel on auto service */
	if (tris_autoservice_start(callee_chan))
		return -1;
	if (tris_stream_and_wait(caller_chan, audiofile, "")) {
		tris_log(LOG_WARNING, "Failed to play automon message!\n");
		tris_autoservice_stop(callee_chan);
		return -1;
	}
	if (tris_autoservice_stop(callee_chan))
		return -1;
	/* Then play for callee, put other channel on auto service */
	if (tris_autoservice_start(caller_chan))
		return -1;
	if (tris_stream_and_wait(callee_chan, audiofile, "")) {
		tris_log(LOG_WARNING, "Failed to play automon message !\n");
		tris_autoservice_stop(caller_chan);
		return -1;
	}
	if (tris_autoservice_stop(caller_chan))
		return -1;
	return(0);
}

/*!
 * \brief Monitor a channel by DTMF
 * \param chan channel requesting monitor
 * \param peer channel to be monitored
 * \param config
 * \param code
 * \param sense feature options
 *
 * \param data
 * Check monitor app enabled, setup channels, both caller/callee chans not null
 * get TOUCH_MONITOR variable for filename if exists, exec monitor app.
 * \retval TRIS_FEATURE_RETURN_SUCCESS on success.
 * \retval -1 on error.
*/
static int builtin_automonitor(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	char *caller_chan_id = NULL, *callee_chan_id = NULL, *args = NULL, *touch_filename = NULL;
	int x = 0;
	size_t len;
	struct tris_channel *caller_chan, *callee_chan;
	const char *automon_message_start = NULL;
	const char *automon_message_stop = NULL;

	if (!monitor_ok) {
		tris_log(LOG_ERROR,"Cannot record the call. The monitor application is disabled.\n");
		return -1;
	}

	if (!monitor_app && !(monitor_app = pbx_findapp("Monitor"))) {
		monitor_ok = 0;
		tris_log(LOG_ERROR,"Cannot record the call. The monitor application is disabled.\n");
		return -1;
	}

	set_peers(&caller_chan, &callee_chan, peer, chan, sense);
	if (caller_chan) {	/* Find extra messages */
		automon_message_start = pbx_builtin_getvar_helper(caller_chan, "TOUCH_MONITOR_MESSAGE_START");
		automon_message_stop = pbx_builtin_getvar_helper(caller_chan, "TOUCH_MONITOR_MESSAGE_STOP");
	}

	if (!tris_strlen_zero(courtesytone)) {	/* Play courtesy tone if configured */
		if(play_message_in_bridged_call(caller_chan, callee_chan, courtesytone) == -1) {
			return -1;
		}
	}
	
	if (callee_chan->monitor) {
		tris_verb(4, "User hit '%s' to stop recording call.\n", code);
		if (!tris_strlen_zero(automon_message_stop)) {
			play_message_in_bridged_call(caller_chan, callee_chan, automon_message_stop);
		}
		callee_chan->monitor->stop(callee_chan, 1);
		return TRIS_FEATURE_RETURN_SUCCESS;
	}

	if (caller_chan && callee_chan) {
		const char *touch_format = pbx_builtin_getvar_helper(caller_chan, "TOUCH_MONITOR_FORMAT");
		const char *touch_monitor = pbx_builtin_getvar_helper(caller_chan, "TOUCH_MONITOR");
		const char *touch_monitor_prefix = pbx_builtin_getvar_helper(caller_chan, "TOUCH_MONITOR_PREFIX");

		if (!touch_format)
			touch_format = pbx_builtin_getvar_helper(callee_chan, "TOUCH_MONITOR_FORMAT");

		if (!touch_monitor)
			touch_monitor = pbx_builtin_getvar_helper(callee_chan, "TOUCH_MONITOR");
	
		if (!touch_monitor_prefix)
			touch_monitor_prefix = pbx_builtin_getvar_helper(callee_chan, "TOUCH_MONITOR_PREFIX");
	
		if (touch_monitor) {
			len = strlen(touch_monitor) + 50;
			args = alloca(len);
			touch_filename = alloca(len);
			snprintf(touch_filename, len, "%s-%ld-%s", S_OR(touch_monitor_prefix, "auto"), (long)time(NULL), touch_monitor);
			snprintf(args, len, "%s,%s,m", S_OR(touch_format, "wav"), touch_filename);
		} else {
			caller_chan_id = tris_strdupa(S_OR(caller_chan->cid.cid_num, caller_chan->name));
			callee_chan_id = tris_strdupa(S_OR(callee_chan->cid.cid_num, callee_chan->name));
			len = strlen(caller_chan_id) + strlen(callee_chan_id) + 50;
			args = alloca(len);
			touch_filename = alloca(len);
			snprintf(touch_filename, len, "%s-%ld-%s-%s", S_OR(touch_monitor_prefix, "auto"), (long)time(NULL), caller_chan_id, callee_chan_id);
			snprintf(args, len, "%s,%s,m", S_OR(touch_format, "wav"), touch_filename);
		}

		for(x = 0; x < strlen(args); x++) {
			if (args[x] == '/')
				args[x] = '-';
		}
		
		tris_verb(4, "User hit '%s' to record call. filename: %s\n", code, args);

		pbx_exec(callee_chan, monitor_app, args);
		pbx_builtin_setvar_helper(callee_chan, "TOUCH_MONITOR_OUTPUT", touch_filename);
		pbx_builtin_setvar_helper(caller_chan, "TOUCH_MONITOR_OUTPUT", touch_filename);

		if (!tris_strlen_zero(automon_message_start)) {	/* Play start message for both channels */
			play_message_in_bridged_call(caller_chan, callee_chan, automon_message_start);
		}
	
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	
	tris_log(LOG_NOTICE,"Cannot record the call. One or both channels have gone away.\n");	
	return -1;
}

static int builtin_automixmonitor(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	char *caller_chan_id = NULL, *callee_chan_id = NULL, *args = NULL, *touch_filename = NULL;
	int x = 0;
	size_t len;
	struct tris_channel *caller_chan, *callee_chan;
	const char *mixmonitor_spy_type = "MixMonitor";
	int count = 0;

	if (!mixmonitor_ok) {
		tris_log(LOG_ERROR,"Cannot record the call. The mixmonitor application is disabled.\n");
		return -1;
	}

	if (!(mixmonitor_app = pbx_findapp("MixMonitor"))) {
		mixmonitor_ok = 0;
		tris_log(LOG_ERROR,"Cannot record the call. The mixmonitor application is disabled.\n");
		return -1;
	}

	set_peers(&caller_chan, &callee_chan, peer, chan, sense);

	if (!tris_strlen_zero(courtesytone)) {
		if (tris_autoservice_start(callee_chan))
			return -1;
		if (tris_stream_and_wait(caller_chan, courtesytone, "")) {
			tris_log(LOG_WARNING, "Failed to play courtesy tone!\n");
			tris_autoservice_stop(callee_chan);
			return -1;
		}
		if (tris_autoservice_stop(callee_chan))
			return -1;
	}

	tris_channel_lock(callee_chan);
	count = tris_channel_audiohook_count_by_source(callee_chan, mixmonitor_spy_type, TRIS_AUDIOHOOK_TYPE_SPY);
	tris_channel_unlock(callee_chan);

	/* This means a mixmonitor is attached to the channel, running or not is unknown. */
	if (count > 0) {
		
		tris_verb(3, "User hit '%s' to stop recording call.\n", code);

		/* Make sure they are running */
		tris_channel_lock(callee_chan);
		count = tris_channel_audiohook_count_by_source_running(callee_chan, mixmonitor_spy_type, TRIS_AUDIOHOOK_TYPE_SPY);
		tris_channel_unlock(callee_chan);
		if (count > 0) {
			if (!stopmixmonitor_ok) {
				tris_log(LOG_ERROR,"Cannot stop recording the call. The stopmixmonitor application is disabled.\n");
				return -1;
			}
			if (!(stopmixmonitor_app = pbx_findapp("StopMixMonitor"))) {
				stopmixmonitor_ok = 0;
				tris_log(LOG_ERROR,"Cannot stop recording the call. The stopmixmonitor application is disabled.\n");
				return -1;
			} else {
				pbx_exec(callee_chan, stopmixmonitor_app, "");
				return TRIS_FEATURE_RETURN_SUCCESS;
			}
		}
		
		tris_log(LOG_WARNING,"Stopped MixMonitors are attached to the channel.\n");	
	}			

	if (caller_chan && callee_chan) {
		const char *touch_format = pbx_builtin_getvar_helper(caller_chan, "TOUCH_MIXMONITOR_FORMAT");
		const char *touch_monitor = pbx_builtin_getvar_helper(caller_chan, "TOUCH_MIXMONITOR");

		if (!touch_format)
			touch_format = pbx_builtin_getvar_helper(callee_chan, "TOUCH_MIXMONITOR_FORMAT");

		if (!touch_monitor)
			touch_monitor = pbx_builtin_getvar_helper(callee_chan, "TOUCH_MIXMONITOR");

		if (touch_monitor) {
			len = strlen(touch_monitor) + 50;
			args = alloca(len);
			touch_filename = alloca(len);
			snprintf(touch_filename, len, "auto-%ld-%s", (long)time(NULL), touch_monitor);
			snprintf(args, len, "%s.%s,b", touch_filename, (touch_format) ? touch_format : "wav");
		} else {
			caller_chan_id = tris_strdupa(S_OR(caller_chan->cid.cid_num, caller_chan->name));
			callee_chan_id = tris_strdupa(S_OR(callee_chan->cid.cid_num, callee_chan->name));
			len = strlen(caller_chan_id) + strlen(callee_chan_id) + 50;
			args = alloca(len);
			touch_filename = alloca(len);
			snprintf(touch_filename, len, "auto-%ld-%s-%s", (long)time(NULL), caller_chan_id, callee_chan_id);
			snprintf(args, len, "%s.%s,b", touch_filename, S_OR(touch_format, "wav"));
		}

		for( x = 0; x < strlen(args); x++) {
			if (args[x] == '/')
				args[x] = '-';
		}

		tris_verb(3, "User hit '%s' to record call. filename: %s\n", code, touch_filename);

		pbx_exec(callee_chan, mixmonitor_app, args);
		pbx_builtin_setvar_helper(callee_chan, "TOUCH_MIXMONITOR_OUTPUT", touch_filename);
		pbx_builtin_setvar_helper(caller_chan, "TOUCH_MIXMONITOR_OUTPUT", touch_filename);
		return TRIS_FEATURE_RETURN_SUCCESS;
	
	}

	tris_log(LOG_NOTICE,"Cannot record the call. One or both channels have gone away.\n");
	return -1;

}

static int builtin_disconnect(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	tris_verb(4, "User hit '%s' to disconnect call.\n", code);
	return TRIS_FEATURE_RETURN_HANGUP;
}

static int finishup(struct tris_channel *chan)
{
	tris_indicate(chan, TRIS_CONTROL_UNHOLD);

	return tris_autoservice_stop(chan);
	return 0;
}

/*!
 * \brief Find the context for the transfer
 * \param transferer
 * \param transferee
 * 
 * Grab the TRANSFER_CONTEXT, if fails try grabbing macrocontext.
 * \return a context string
*/
static const char *real_ctx(struct tris_channel *transferer, struct tris_channel *transferee)
{
	const char *s = pbx_builtin_getvar_helper(transferer, "TRANSFER_CONTEXT");
	if (tris_strlen_zero(s)) {
		s = pbx_builtin_getvar_helper(transferee, "TRANSFER_CONTEXT");
	}
	if (tris_strlen_zero(s)) { /* Use the non-macro context to transfer the call XXX ? */
		s = transferer->macrocontext;
	}
	if (tris_strlen_zero(s)) {
		s = transferer->context;
	}
	return s;  
}

static int set_channel_not_spy(struct tris_channel *chan)
{
	tris_channel_lock(chan);
	chan->spytransferchan = 1;
	if (chan->audiohooks) {
		tris_audiohook_detach_list(chan->audiohooks);
		chan->audiohooks = NULL;
	}
	tris_channel_unlock(chan);
	return 0;
}

/*!
 * \brief Blind transfer user to another extension
 * \param chan channel to be transfered
 * \param peer channel initiated blind transfer
 * \param config
 * \param code
 * \param data
 * \param sense  feature options
 * 
 * Place chan on hold, check if transferred to parkinglot extension,
 * otherwise check extension exists and transfer caller.
 * \retval TRIS_FEATURE_RETURN_SUCCESS.
 * \retval -1 on failure.
*/
static int builtin_blindtransfer(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_channel *transferer;
	struct tris_channel *transferee;
	const char *transferer_real_context;
	char xferto[256];
	int res, parkstatus = 0;

	set_peers(&transferer, &transferee, peer, chan, sense);
	transferer_real_context = real_ctx(transferer, transferee);
	/* Start autoservice on chan while we talk to the originator */
	//tris_autoservice_start(transferee);
	//tris_indicate(transferee, TRIS_CONTROL_HOLD);
	memset(xferto, 0, sizeof(xferto));
	
	if (!transferer->appl || strcmp(transferer->appl, "AppQueue")) { // by scr, 2014/4/29
		res = tris_stream_and_wait(transferer, "pbx/pbx-transfer", TRIS_DIGIT_ANY);
		if (res < 0) {
			finishup(transferee);
			return -1; /* error ? */
		}
		if (res > 0)	/* If they've typed a digit already, handle it */
			xferto[0] = (char) res;
	
		tris_stopstream(transferer);
	}
	res = tris_app_dtget(transferer, transferer_real_context, xferto, sizeof(xferto), 100, transferdigittimeout);
	if (res < 0) {	/* hangup, would be 0 for invalid and 1 for valid */
		finishup(transferee);
		return res;
	}

	if (transferer->appl && !strcmp(transferer->appl, "AppQueue")) { // by scr, 2014/4/29
		tris_log(LOG_NOTICE, "called %s.\n", transferer->appl);
		if (strlen(xferto) > 2 && xferto[0]=='*' && xferto[1]=='9' && xferto[2]=='9'){
			pbx_builtin_setvar_helper(transferee, "XFERTO", xferto+3);
			res=finishup(transferee);
			tris_set_flag(transferee, TRIS_FLAG_BRIDGE_HANGUP_DONT);
			tris_log(LOG_DEBUG,"ABOUT TO TRIS_ASYNC_GOTO, have a pbx... set HANGUP_DONT on chan=%s\n", transferee->name);
			set_channel_not_spy(transferee);
			set_c_e_p(transferee, "tone", "OPERA", 0);
			check_goto_on_transfer(transferer);
			return res;
		} else if (strlen(xferto) > 2 && xferto[0]=='*' && xferto[1]=='8' && xferto[2]=='1') {
			const char *tmp;
			tmp = pbx_builtin_getvar_helper(transferee, "announce-greeting");
			if (tmp && !tris_strlen_zero(tmp)) {
				tris_play_and_wait(transferee, tmp);
				if (tris_check_hangup(transferer))
					res = finishup(transferee);
				else
					res = TRIS_FEATURE_RETURN_SUCCESS;
				return res;
			}
		} else if (strlen(xferto) > 2 && xferto[0]=='*' && xferto[1]=='7' && xferto[2]=='8') {
			tris_play_and_wait(transferee, "queue/queue-not-found");
			res = finishup(transferee);
			return res;
		} else if (strlen(xferto) > 2 && xferto[0]=='*' && xferto[1]=='7' && xferto[2]=='5') {
			tris_play_and_wait(transferee, "queue/cant_call");
			res = finishup(transferee);
			return res;
		} else if (strlen(xferto) > 2 && xferto[0]=='*' && xferto[1]=='7' && xferto[2]=='6') {
			tris_play_and_wait(transferee, "queue/say-again");
			if (tris_check_hangup(transferer))
				res = finishup(transferee);
			else
				res = TRIS_FEATURE_RETURN_SUCCESS;
			return res;
		} else if (strlen(xferto) < 2 || xferto[0] != '*' || xferto[1] !='7' || xferto[2] !='7') {
			res = TRIS_FEATURE_RETURN_SUCCESS;
			return res;
		}
		char tmp[256];
		snprintf(tmp, sizeof(tmp), "%s", xferto+3);
		snprintf(xferto, sizeof(xferto), "%s", tmp);
	}
	/* Transfer */
	if (!strcmp(xferto, tris_parking_ext())) {
		res = finishup(transferee);
		if (res)
			res = -1;
		else if (!(parkstatus = masq_park_call_announce(transferee, transferer, 0, NULL))) {	/* success */
			/* We return non-zero, but tell the PBX not to hang the channel when
			   the thread dies -- We have to be careful now though.  We are responsible for 
			   hanging up the channel, else it will never be hung up! */
			if (transferer) {
				transferer->transferchan = 1;
				set_channel_not_spy(transferer);
			}
			if (transferee) {
				transferee->transferchan = 2;
				set_channel_not_spy(transferee);
			}

			return 0;
		} else {
			tris_log(LOG_WARNING, "Unable to park call %s, parkstatus = %d\n", transferee->name, parkstatus);
		}
		/*! \todo XXX Maybe we should have another message here instead of invalid extension XXX */
	} else if (tris_exists_extension(transferee, transferer_real_context, xferto, 1, transferer->cid.cid_num)) {
		pbx_builtin_setvar_helper(transferer, "BLINDTRANSFER", transferee->name);
		pbx_builtin_setvar_helper(transferee, "BLINDTRANSFER", transferer->name);
		res=finishup(transferee);
		if (!transferer->cdr) { /* this code should never get called (in a perfect world) */
			transferer->cdr=tris_cdr_alloc();
			if (transferer->cdr) {
				tris_cdr_init(transferer->cdr, transferer); /* initialize our channel's cdr */
				tris_cdr_start(transferer->cdr);
			}
		}
		if (transferer->cdr) {
			struct tris_cdr *swap = transferer->cdr;
			tris_log(LOG_DEBUG,"transferer=%s; transferee=%s; lastapp=%s; lastdata=%s; chan=%s; dstchan=%s\n",
					transferer->name, transferee->name, transferer->cdr->lastapp, transferer->cdr->lastdata, 
					transferer->cdr->channel, transferer->cdr->dstchannel);
			tris_log(LOG_DEBUG,"TRANSFEREE; lastapp=%s; lastdata=%s, chan=%s; dstchan=%s\n",
					transferee->cdr->lastapp, transferee->cdr->lastdata, transferee->cdr->channel, transferee->cdr->dstchannel);
			tris_log(LOG_DEBUG,"transferer_real_context=%s; xferto=%s\n", transferer_real_context, xferto);
			/* swap cdrs-- it will save us some time & work */
			transferer->cdr = transferee->cdr;
			transferee->cdr = swap;
		}
		if (!transferee->pbx) {
			/* Doh!  Use our handy async_goto functions */
			tris_verb(3, "Transferring %s to '%s' (context %s) priority 1\n"
								,transferee->name, xferto, transferer_real_context);
			if (tris_async_goto(transferee, transferer_real_context, xferto, 1))
				tris_log(LOG_WARNING, "Async goto failed :-(\n");
		} else {
			/* Set the channel's new extension, since it exists, using transferer context */
			tris_set_flag(transferee, TRIS_FLAG_BRIDGE_HANGUP_DONT); /* don't let the after-bridge code run the h-exten */
			tris_log(LOG_DEBUG,"ABOUT TO TRIS_ASYNC_GOTO, have a pbx... set HANGUP_DONT on chan=%s\n", transferee->name);
			set_c_e_p(transferee, transferer_real_context, xferto, 0);
		}
		check_goto_on_transfer(transferer);
		if (transferer) {
			transferer->transferchan = 1;
			set_channel_not_spy(transferer);
		}
		if (transferee) {
			transferee->transferchan = 2;
			set_channel_not_spy(transferee);
		}
		if (transferer && transferee)
			tris_set_callerid(transferee, transferer->cid.cid_num, transferer->cid.cid_name, transferer->cid.cid_ani);
		return res;
	} else {
		tris_verb(3, "Unable to find extension '%s' in context '%s'\n", xferto, transferer_real_context);
	}
	if (parkstatus != TRIS_FEATURE_RETURN_PARKFAILED && tris_stream_and_wait(transferer, xferfailsound, TRIS_DIGIT_ANY) < 0) {
		finishup(transferee);
		return -1;
	}
	tris_stopstream(transferer);
	res = finishup(transferee);
	if (res) {
		tris_verb(2, "Hungup during autoservice stop on '%s'\n", transferee->name);
		return res;
	}
	res = TRIS_FEATURE_RETURN_SUCCESS;
	return res;
}

/*!
 * \brief make channels compatible
 * \param c
 * \param newchan
 * \retval 0 on success.
 * \retval -1 on failure.
*/
static int check_compat(struct tris_channel *c, struct tris_channel *newchan)
{
	if (tris_channel_make_compatible(c, newchan) < 0) {
		tris_log(LOG_WARNING, "Had to drop call because I couldn't make %s compatible with %s\n",
			c->name, newchan->name);
		tris_hangup(newchan);
		return -1;
	}
	return 0;
}

/*!
 * \brief Attended transfer
 * \param chan transfered user
 * \param peer person transfering call
 * \param config
 * \param code
 * \param sense feature options
 * 
 * \param data
 * Get extension to transfer to, if you cannot generate channel (or find extension) 
 * return to host channel. After called channel answered wait for hangup of transferer,
 * bridge call between transfer peer (taking them off hold) to attended transfer channel.
 *
 * \return -1 on failure
*/
static int builtin_atxfer(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_channel *transferer;
	struct tris_channel *transferee;
	const char *transferer_real_context;
	char xferto[256] = "";
	int res;
	int outstate=0;
	struct tris_channel *newchan;
	struct tris_channel *xferchan;
	struct tris_bridge_thread_obj *tobj;
	struct tris_bridge_config bconfig;
	int l;
	struct tris_datastore *features_datastore;
	struct tris_dial_features *dialfeatures = NULL;
	char sql[1024], result[1024]="";
	int ringing = 1;
	const char* ringmode;
	int notifycaller = 0;
	const char* notifycaller_str;
	enum tris_channel_state newstate = 0;

	tris_debug(1, "Executing Attended Transfer %s, %s (sense=%d) \n", chan->name, peer->name, sense);
	set_peers(&transferer, &transferee, peer, chan, sense);
	transferer_real_context = real_ctx(transferer, transferee);
	/* Start autoservice on chan while we talk to the originator */

	ringmode = pbx_builtin_getvar_helper(transferee, "ringmode");
	if (!ringmode) {
		snprintf(sql, sizeof(sql), "select queue.ring_mode from queue left join queue_member on queue.id=queue_member.queue_id where queue_member.exten='%s';", transferer->cid.cid_num);
		if (tris_sql_select_query_execute) {
			tris_sql_select_query_execute(result, sql);
			if (!tris_strlen_zero(result) && strcmp(result, "ringback")) {
				ringing = 0;
			}
		} else {
				ringing = 0;
		}
	} else if (strcmp(ringmode, "ringback")) {
		ringing = 0;
	}
	if (ringing) {
		pbx_builtin_setvar_helper(transferee, "ringmode", "ringback");
		pbx_builtin_setvar_helper(transferer, "ringmode", "ringback");
	} else {
		pbx_builtin_setvar_helper(transferee, "ringmode", "moh");
		pbx_builtin_setvar_helper(transferer, "ringmode", "moh");
	}

	notifycaller_str = pbx_builtin_getvar_helper(transferee, "notifycaller");
	if (!notifycaller_str) {
		snprintf(sql, sizeof(sql), "select queue.notifycaller from queue left join queue_member on queue.id=queue_member.queue_id where queue_member.exten='%s';", transferer->cid.cid_num);
		if (tris_sql_select_query_execute) {
			tris_sql_select_query_execute(result, sql);
			if (!tris_strlen_zero(result) && strlen(result) == 1)
				notifycaller = atoi(result);
		} else {
				notifycaller = 1;
		}
	} else if (!tris_strlen_zero(notifycaller_str)) {
		notifycaller = atoi(notifycaller_str);
	}
	snprintf(result, sizeof(result), "%d", notifycaller);
	pbx_builtin_setvar_helper(transferee, "notifycaller", result);
	pbx_builtin_setvar_helper(transferer, "notifycaller", result);

	if (!ringing)
		tris_autoservice_start(transferee);
	tris_indicate(transferee, TRIS_CONTROL_HOLD);
	
	/* Transfer */
	res = tris_stream_and_wait(transferer, "pbx/pbx-transfer", TRIS_DIGIT_ANY);
	if (res < 0) {
		finishup(transferee);
		return res;
	}
	if (res > 0) /* If they've typed a digit already, handle it */
		xferto[0] = (char) res;

	/* this is specific of atxfer */
	res = tris_app_dtget(transferer, transferer_real_context, xferto, sizeof(xferto), 100, transferdigittimeout);
	if (res < 0) {  /* hangup, would be 0 for invalid and 1 for valid */
		finishup(transferee);
		return res;
	}
	if (res == 0) {
		tris_log(LOG_WARNING, "Did not read data.\n");
		finishup(transferee);
		if (tris_stream_and_wait(transferer, "beep", ""))
			return -1;
		return TRIS_FEATURE_RETURN_SUCCESS;
	}

	/* valid extension, res == 1 */
	if (!tris_exists_extension(transferer, transferer_real_context, xferto, 1, transferer->cid.cid_num)) {
		tris_log(LOG_WARNING, "Extension %s does not exist in context %s\n",xferto,transferer_real_context);
		finishup(transferee);
		if (tris_stream_and_wait(transferer, "beep", ""))
			return -1;
		return TRIS_FEATURE_RETURN_SUCCESS;
	}

	snprintf(sql, sizeof(sql), "select key_name from service_set where '%s' like concat(key_number,'%%');", xferto);
	if (tris_sql_select_query_execute) {
		tris_sql_select_query_execute(result, sql);
		if (!tris_strlen_zero(result) && strlen(result) == 12 && !strncmp(result, "bargein3conf", 12)) {
			tris_log(LOG_WARNING, "Can't call barge in 3conf.\n");
			finishup(transferee);
			if (tris_stream_and_wait(transferer, "pbx/pbx-not-found", ""))
				return -1;
			if (tris_stream_and_wait(transferer, "beep", ""))
				return -1;
			return TRIS_FEATURE_RETURN_SUCCESS;
		}
	}

	if (transferer)
		transferer->transferchan = 1;
	if (transferee)
		transferee->transferchan = 2;
	if (transferer && transferee) {
		pbx_builtin_setvar_helper(transferer, "CallerByeNumber", transferee->cid.cid_num);
		pbx_builtin_setvar_helper(transferer, "CalleeByeNumber", xferto);
	}
 	/* If we are attended transfering to parking, just use builtin_parkcall instead of trying to track all of
 	 * the different variables for handling this properly with a builtin_atxfer */
 	if (!strcmp(xferto, tris_parking_ext())) {
 		finishup(transferee);
 		return builtin_parkcall(chan, peer, config, code, sense, data);
 	}

	l = strlen(xferto);
	if (gethostname(result, sizeof(result)) < 0)
		return -1;
	snprintf(xferto + l, sizeof(xferto) - l, "@%s", result);	/* append context */

	/* If we are performing an attended transfer and we have two channels involved then
	   copy sound file information to play upon attended transfer completion */
	if (transferee) {
		const char *chan1_attended_sound = pbx_builtin_getvar_helper(transferer, "ATTENDED_TRANSFER_COMPLETE_SOUND");
		const char *chan2_attended_sound = pbx_builtin_getvar_helper(transferee, "ATTENDED_TRANSFER_COMPLETE_SOUND");

		if (!tris_strlen_zero(chan1_attended_sound)) {
			pbx_builtin_setvar_helper(transferer, "BRIDGE_PLAY_SOUND", chan1_attended_sound);
		}
		if (!tris_strlen_zero(chan2_attended_sound)) {
			pbx_builtin_setvar_helper(transferee, "BRIDGE_PLAY_SOUND", chan2_attended_sound);
		}
	}

	newchan = feature_request_and_dial(transferer, transferee, "Switch", tris_best_codec(transferer->nativeformats),
		xferto, atxfernoanswertimeout, &outstate, transferer->cid.cid_num, transferer->cid.cid_name, 0, 
		transferer->language, ringing, notifycaller);

	if (!tris_check_hangup(transferer)) {
		/* Transferer is up - old behaviour */
		tris_indicate(transferer, -1);
		if (!newchan) {
			finishup(transferee);
			/* any reason besides user requested cancel and busy triggers the failed sound */
			/*if (outstate != TRIS_CONTROL_UNHOLD && outstate != TRIS_CONTROL_BUSY &&
				tris_stream_and_wait(transferer, xferfailsound, ""))
				return -1;*/
			transferer->spytransferchan = 0;
			if (transferee && !tris_check_hangup(transferee))
				transferee->spytransferchan = 0;
			if (tris_stream_and_wait(transferer, xfersound, ""))
				tris_log(LOG_WARNING, "Failed to play transfer sound!\n");
			return TRIS_FEATURE_RETURN_SUCCESS;
		}

		if (check_compat(transferer, newchan)) {
			/* we do mean transferee here, NOT transferer */
			finishup(transferee);
			return -1;
		}
		memset(&bconfig,0,sizeof(struct tris_bridge_config));
		/*tris_set_flag(&(bconfig.features_caller), TRIS_FEATURE_DISCONNECT);
		tris_set_flag(&(bconfig.features_callee), TRIS_FEATURE_DISCONNECT);*/
		tris_set_flag(&(bconfig.features_caller), TRIS_FEATURE_ATXFER);
		tris_set_flag(&(bconfig.features_caller), TRIS_FEATURE_REDIRECT);

		if (tris_check_hangup(transferee) && !tris_check_hangup(transferer) && !tris_check_hangup(newchan)) {
			res = tris_bridge_call(transferer, newchan, &bconfig);
			if (!tris_check_hangup(transferer)) {
				tris_hangup(newchan);
				return TRIS_FEATURE_RETURN_SUCCESS;
			}
		}
		if (tris_check_hangup(newchan)) {
			tris_hangup(newchan);
			if (tris_stream_and_wait(transferer, xfersound, ""))
				tris_log(LOG_WARNING, "Failed to play transfer sound!\n");
			transferer->_softhangup = 0;
			return TRIS_FEATURE_RETURN_SUCCESS;
		}
		if (check_compat(transferee, newchan)) {
			return -1;
		}

		if ((tris_waitfordigit(transferee, 100) < 0)
		 || (tris_waitfordigit(newchan, 100) < 0)
		 || tris_check_hangup(transferee)
		 || tris_check_hangup(newchan)) {
			tris_hangup(newchan);
			return -1;
		}
		xferchan = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, "Transfered/%s", transferee->name);
		if (!xferchan) {
			tris_hangup(newchan);
			return -1;
		}
		/* Make formats okay */
		xferchan->visible_indication = transferer->visible_indication;
		xferchan->readformat = transferee->readformat;
		xferchan->writeformat = transferee->writeformat;
		tris_channel_masquerade(xferchan, transferee);
		tris_explicit_goto(xferchan, transferee->context, transferee->exten, transferee->priority);
		/*xferchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(xferchan, TRIS_FLAGS_ALL);
		tris_channel_lock(xferchan);
		tris_do_masquerade(xferchan);
		tris_channel_unlock(xferchan);
		xferchan->_softhangup = 0;
		newstate = newchan->_state;
		/*newchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(newchan, TRIS_FLAGS_ALL);
		newchan->_softhangup = 0;
		if (!(tobj = tris_calloc(1, sizeof(*tobj)))) {
			tris_hangup(xferchan);
			tris_hangup(newchan);
			return -1;
		}

		tris_channel_lock(newchan);
		if ((features_datastore = tris_channel_datastore_find(newchan, &dial_features_info, NULL))) {
				dialfeatures = features_datastore->data;
		}
		tris_channel_unlock(newchan);

		if (dialfeatures) {
			/* newchan should always be the callee and shows up as callee in dialfeatures, but for some reason
			   I don't currently understand, the abilities of newchan seem to be stored on the caller side */
			tris_copy_flags(&(config->features_callee), &(dialfeatures->features_caller), TRIS_FLAGS_ALL);
			dialfeatures = NULL;
		}

		tris_channel_lock(xferchan);
		if ((features_datastore = tris_channel_datastore_find(xferchan, &dial_features_info, NULL))) {
			dialfeatures = features_datastore->data;
		}
		tris_channel_unlock(xferchan);
	 
		if (dialfeatures) {
			tris_copy_flags(&(config->features_caller), &(dialfeatures->features_caller), TRIS_FLAGS_ALL);
		}
	 
		tobj->chan = newchan;
		tobj->peer = xferchan;
		tobj->bconfig = *config;

		if (tobj->bconfig.end_bridge_callback_data_fixup) {
			tobj->bconfig.end_bridge_callback_data_fixup(&tobj->bconfig, tobj->peer, tobj->chan);
		}

		/*if (newstate == TRIS_STATE_UP && tris_stream_and_wait(newchan, xfersound, ""))
			tris_log(LOG_WARNING, "Failed to play transfer sound!\n");*/

		if (newchan && !tris_check_hangup(newchan))
			set_channel_not_spy(newchan);
		if (xferchan && !tris_check_hangup(xferchan))
			set_channel_not_spy(xferchan);
		bridge_call_thread_launch(tobj);
		return -1;      /* XXX meaning the channel is bridged ? */
	} else if (!tris_check_hangup(transferee)) {
		set_channel_not_spy(transferee);
		if (newchan && !tris_check_hangup(newchan))
			set_channel_not_spy(newchan);
		/* act as blind transfer */
		if (!ringing && tris_autoservice_stop(transferee) < 0) {
			tris_hangup(newchan);
			return -1;
		}

		/*if (!newchan) {
			unsigned int tries = 0;
			char *transferer_tech, *transferer_name = tris_strdupa(transferer->name);

			transferer_tech = strsep(&transferer_name, "/");
			transferer_name = strsep(&transferer_name, "-");

			if (tris_strlen_zero(transferer_name) || tris_strlen_zero(transferer_tech)) {
				tris_log(LOG_WARNING, "Transferer has invalid channel name: '%s'\n", transferer->name);
				if (tris_stream_and_wait(transferee, "beep", ""))
					return -1;
				return TRIS_FEATURE_RETURN_SUCCESS;
			}

			tris_log(LOG_NOTICE, "We're trying to call %s/%s\n", transferer_tech, transferer_name);
			newchan = feature_request_and_dial(transferee, NULL, transferer_tech, tris_best_codec(transferee->nativeformats),
				transferer_name, atxfernoanswertimeout, &outstate, transferee->cid.cid_num, transferee->cid.cid_name, 0, transferer->language);
			while (!newchan && !atxferdropcall && tries < atxfercallbackretries) {
				// Trying to transfer again
				tris_autoservice_start(transferee);
				tris_indicate(transferee, TRIS_CONTROL_HOLD);

				newchan = feature_request_and_dial(transferer, transferee, "Local", tris_best_codec(transferer->nativeformats),
					xferto, atxfernoanswertimeout, &outstate, transferer->cid.cid_num, transferer->cid.cid_name, 1, transferer->language);
				if (tris_autoservice_stop(transferee) < 0) {
					if (newchan)
						tris_hangup(newchan);
					return -1;
				}
				if (!newchan) {
					// Transfer failed, sleeping 
					tris_debug(1, "Sleeping for %d ms before callback.\n", atxferloopdelay);
					tris_safe_sleep(transferee, atxferloopdelay);
					tris_debug(1, "Trying to callback...\n");
					newchan = feature_request_and_dial(transferee, NULL, transferer_tech, tris_best_codec(transferee->nativeformats),
						transferer_name, atxfernoanswertimeout, &outstate, transferee->cid.cid_num, transferee->cid.cid_name, 0, transferer->language);
				}
				tries++;
			}
		}
		*/
		if (!newchan)
			return -1;

		/* newchan is up, we should prepare transferee and bridge them */
		if (check_compat(transferee, newchan)) {
			finishup(transferee);
			return -1;
		}
		tris_indicate(transferee, TRIS_CONTROL_UNHOLD);

		if ((tris_waitfordigit(transferee, 100) < 0)
		   || (tris_waitfordigit(newchan, 100) < 0)
		   || tris_check_hangup(transferee)
		   || tris_check_hangup(newchan)) {
			tris_hangup(newchan);
			return -1;
		}

		xferchan = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, "Transfered/%s", transferee->name);
		if (!xferchan) {
			tris_hangup(newchan);
			return -1;
		}
		/* Make formats okay */
		xferchan->visible_indication = transferer->visible_indication;
		xferchan->readformat = transferee->readformat;
		xferchan->writeformat = transferee->writeformat;
		tris_channel_masquerade(xferchan, transferee);
		tris_explicit_goto(xferchan, transferee->context, transferee->exten, transferee->priority);
		/*xferchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(xferchan, TRIS_FLAGS_ALL);
		tris_channel_lock(xferchan);
		tris_do_masquerade(xferchan);
		tris_channel_unlock(xferchan);
		xferchan->_softhangup = 0;
		newstate = newchan->_state;
		/*newchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(newchan, TRIS_FLAGS_ALL);
		newchan->_softhangup = 0;
		if (!(tobj = tris_calloc(1, sizeof(*tobj)))) {
			tris_hangup(xferchan);
			tris_hangup(newchan);
			return -1;
		}
		tobj->chan = newchan;
		tobj->peer = xferchan;
		tobj->bconfig = *config;

		if (tobj->bconfig.end_bridge_callback_data_fixup) {
			tobj->bconfig.end_bridge_callback_data_fixup(&tobj->bconfig, tobj->peer, tobj->chan);
		}

		/*if (newstate == TRIS_STATE_UP && tris_stream_and_wait(newchan, xfersound, ""))
			tris_log(LOG_WARNING, "Failed to play transfer sound!\n");*/
		bridge_call_thread_launch(tobj);
		return -1;      /* XXX meaning the channel is bridged ? */
	} else {
		/* Transferee hung up */
		finishup(transferee);
		return -1;
	}
}

/*!
 * \brief Attended transfer
 * \param chan transfered user
 * \param peer person transfering call
 * \param config
 * \param code
 * \param sense feature options
 * 
 * \param data
 * Get extension to transfer to, if you cannot generate channel (or find extension) 
 * return to host channel. After called channel answered wait for hangup of transferer,
 * bridge call between transfer peer (taking them off hold) to attended transfer channel.
 *
 * \return -1 on failure
*/
static int builtin_handle_attended_refer(struct tris_channel *chan, struct tris_channel *peer,
		struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_channel *transferer;
	struct tris_channel *transferee;
	const char *transferer_real_context;
	char xferto[256] = "", dst[256] = "";
	const char *exten = NULL;
	int res;
	int outstate=0;
	struct tris_channel *newchan;
	struct tris_channel *xferchan;
	struct tris_bridge_thread_obj *tobj;
	struct tris_bridge_config bconfig;
	int l;
	struct tris_datastore *features_datastore;
	struct tris_dial_features *dialfeatures = NULL;
	char sql[1024], result[1024]="";
	int ringing = 1;
	const char* ringmode;
	int notifycaller = 0;
	const char* notifycaller_str;
	enum tris_channel_state newstate = 0;
	int holdstate = 0;

	if (!chan || tris_check_hangup(chan) || !peer || tris_check_hangup(peer)) {
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	tris_debug(1, "Executing Attended Transfer %s, %s (sense=%d) \n", chan->name, peer->name, sense);
	set_peers(&transferer, &transferee, peer, chan, sense);
	transferer_real_context = real_ctx(transferer, transferee);
	/* Start autoservice on chan while we talk to the originator */

	ringmode = pbx_builtin_getvar_helper(transferee, "ringmode");
	if (!ringmode) {
		snprintf(sql, sizeof(sql), "select queue.ring_mode from queue left join queue_member on queue.id=queue_member.queue_id where queue_member.exten='%s';", transferer->cid.cid_num);
		if (tris_sql_select_query_execute) {
			tris_sql_select_query_execute(result, sql);
			if (!tris_strlen_zero(result) && strcmp(result, "ringback")) {
				ringing = 0;
			}
		} else {
				ringing = 0;
		}
	} else if (strcmp(ringmode, "ringback")) {
		ringing = 0;
	}
	ringing = 1;
	if (ringing) {
		pbx_builtin_setvar_helper(transferee, "ringmode", "ringback");
		pbx_builtin_setvar_helper(transferer, "ringmode", "ringback");
	} else {
		pbx_builtin_setvar_helper(transferee, "ringmode", "moh");
		pbx_builtin_setvar_helper(transferer, "ringmode", "moh");
	}

	notifycaller_str = pbx_builtin_getvar_helper(transferee, "notifycaller");
	if (!notifycaller_str) {
		snprintf(sql, sizeof(sql), "select queue.notifycaller from queue left join queue_member on queue.id=queue_member.queue_id where queue_member.exten='%s';", transferer->cid.cid_num);
		if (tris_sql_select_query_execute) {
			tris_sql_select_query_execute(result, sql);
			if (!tris_strlen_zero(result) && strlen(result) == 1)
				notifycaller = atoi(result);
		} else {
				notifycaller = 1;
		}
	} else if (!tris_strlen_zero(notifycaller_str)) {
		notifycaller = atoi(notifycaller_str);
	}
	snprintf(result, sizeof(result), "%d", notifycaller);
	pbx_builtin_setvar_helper(transferee, "notifycaller", result);
	pbx_builtin_setvar_helper(transferer, "notifycaller", result);

	if (!ringing)
		tris_autoservice_start(transferee);
	tris_indicate(transferee, TRIS_CONTROL_HOLD);

	exten = transferer->referexten;
	if (!exten || tris_strlen_zero(exten)) {
		tris_log(LOG_WARNING, "Did not read data.\n");
		finishup(transferee);
		if (tris_stream_and_wait(transferer, "beep", ""))
			return -1;
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	snprintf(xferto, sizeof(xferto), "%s", exten);
	snprintf(dst, sizeof(dst), "%s", exten);
	transferer->referexten[0] = '\0';
	
	/* valid extension, res == 1 */
	if (!tris_exists_extension(transferer, transferer_real_context, xferto, 1, transferer->cid.cid_num)) {
		tris_log(LOG_WARNING, "Extension %s does not exist in context %s\n",xferto,transferer_real_context);
		finishup(transferee);
		if (tris_stream_and_wait(transferer, "beep", ""))
			return -1;
		return TRIS_FEATURE_RETURN_SUCCESS;
	}

	snprintf(sql, sizeof(sql), "select key_name from service_set where '%s' like concat(key_number,'%%');", xferto);
	if (tris_sql_select_query_execute) {
		tris_sql_select_query_execute(result, sql);
		if (!tris_strlen_zero(result) && strlen(result) == 12 && !strncmp(result, "bargein3conf", 12)) {
			tris_log(LOG_WARNING, "Can't call barge in 3conf.\n");
			finishup(transferee);
			if (tris_stream_and_wait(transferer, "pbx/pbx-not-found", ""))
				return -1;
			if (tris_stream_and_wait(transferer, "beep", ""))
				return -1;
			return TRIS_FEATURE_RETURN_SUCCESS;
		}
	}

	if (transferer)
		transferer->transferchan = 1;
	if (transferee)
		transferee->transferchan = 2;
 	/* If we are attended transfering to parking, just use builtin_parkcall instead of trying to track all of
 	 * the different variables for handling this properly with a builtin_atxfer */
 	if (!strcmp(xferto, tris_parking_ext())) {
 		finishup(transferee);
 		return builtin_parkcall(chan, peer, config, code, sense, data);
 	}

	l = strlen(xferto);
	if (gethostname(result, sizeof(result)) < 0)
		return -1;
	snprintf(xferto + l, sizeof(xferto) - l, "@%s", result);	/* append context */

	/* If we are performing an attended transfer and we have two channels involved then
	   copy sound file information to play upon attended transfer completion */
	if (transferee) {
		const char *chan1_attended_sound = pbx_builtin_getvar_helper(transferer, "ATTENDED_TRANSFER_COMPLETE_SOUND");
		const char *chan2_attended_sound = pbx_builtin_getvar_helper(transferee, "ATTENDED_TRANSFER_COMPLETE_SOUND");

		if (!tris_strlen_zero(chan1_attended_sound)) {
			pbx_builtin_setvar_helper(transferer, "BRIDGE_PLAY_SOUND", chan1_attended_sound);
		}
		if (!tris_strlen_zero(chan2_attended_sound)) {
			pbx_builtin_setvar_helper(transferee, "BRIDGE_PLAY_SOUND", chan2_attended_sound);
		}
	}

	newchan = feature_dial_byrefer(transferer, transferee, "Switch", tris_best_codec(transferer->nativeformats),
		xferto, atxfernoanswertimeout, &outstate, transferer->cid.cid_num, transferer->cid.cid_name, 0, 
		transferer->language, ringing, notifycaller, transferer_real_context, config, dst, &holdstate);

	if (newchan == (struct tris_channel*)-1)
		return -1;
	
	if (!tris_check_hangup(transferer)) {
		/* Transferer is up - old behaviour */
		tris_indicate(transferer, -1);
		if (!newchan) {
			if (transferee && !tris_check_hangup(transferee))
				finishup(transferee);
			/* any reason besides user requested cancel and busy triggers the failed sound */
			/*if (outstate != TRIS_CONTROL_UNHOLD && outstate != TRIS_CONTROL_BUSY &&
				tris_stream_and_wait(transferer, xferfailsound, ""))
				return -1;*/
			transferer->spytransferchan = 0;
			if (transferee && !tris_check_hangup(transferee))
				transferee->spytransferchan = 0;
			if (tris_stream_and_wait(transferer, xfersound, ""))
				tris_log(LOG_WARNING, "Failed to play transfer sound!\n");
			return TRIS_FEATURE_RETURN_SUCCESS;
		}

		if (check_compat(transferer, newchan)) {
			/* we do mean transferee here, NOT transferer */
			finishup(transferee);
			return -1;
		}
		memset(&bconfig,0,sizeof(struct tris_bridge_config));
		/*tris_set_flag(&(bconfig.features_caller), TRIS_FEATURE_DISCONNECT);
		tris_set_flag(&(bconfig.features_callee), TRIS_FEATURE_DISCONNECT);*/
		tris_set_flag(&(bconfig.features_caller), TRIS_FEATURE_ATXFER);
		tris_set_flag(&(bconfig.features_caller), TRIS_FEATURE_REDIRECT);

		if (tris_check_hangup(transferee) && !tris_check_hangup(transferer) && !tris_check_hangup(newchan)) {
			res = tris_bridge_call(transferer, newchan, &bconfig);
			if (!tris_check_hangup(transferer)) {
				tris_hangup(newchan);
				return TRIS_FEATURE_RETURN_SUCCESS;
			}
		}
		if (tris_check_hangup(newchan)) {
			tris_hangup(newchan);
			if (tris_stream_and_wait(transferer, xfersound, ""))
				tris_log(LOG_WARNING, "Failed to play transfer sound!\n");
			transferer->_softhangup = 0;
			return TRIS_FEATURE_RETURN_SUCCESS;
		}
		if (check_compat(transferee, newchan)) {
			return -1;
		}

		if ((tris_waitfordigit(transferee, 100) < 0)
		 || (tris_waitfordigit(newchan, 100) < 0)
		 || tris_check_hangup(transferee)
		 || tris_check_hangup(newchan)) {
			tris_hangup(newchan);
			return -1;
		}
		xferchan = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, "Transfered/%s", transferee->name);
		if (!xferchan) {
			tris_hangup(newchan);
			return -1;
		}
		/* Make formats okay */
		xferchan->visible_indication = TRIS_CONTROL_RINGING;
		xferchan->readformat = transferee->readformat;
		xferchan->writeformat = transferee->writeformat;
		tris_channel_masquerade(xferchan, transferee);
		tris_explicit_goto(xferchan, transferee->context, transferee->exten, transferee->priority);
		/*xferchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(xferchan, TRIS_FLAGS_ALL);
		tris_channel_lock(xferchan);
		tris_do_masquerade(xferchan);
		tris_channel_unlock(xferchan);
		xferchan->_softhangup = 0;
		newstate = newchan->_state;
		/*newchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(newchan, TRIS_FLAGS_ALL);
		newchan->_softhangup = 0;
		if (!(tobj = tris_calloc(1, sizeof(*tobj)))) {
			tris_hangup(xferchan);
			tris_hangup(newchan);
			return -1;
		}

		tris_channel_lock(newchan);
		if ((features_datastore = tris_channel_datastore_find(newchan, &dial_features_info, NULL))) {
				dialfeatures = features_datastore->data;
		}
		tris_channel_unlock(newchan);

		if (dialfeatures) {
			/* newchan should always be the callee and shows up as callee in dialfeatures, but for some reason
			   I don't currently understand, the abilities of newchan seem to be stored on the caller side */
			tris_copy_flags(&(config->features_callee), &(dialfeatures->features_caller), TRIS_FLAGS_ALL);
			dialfeatures = NULL;
		}

		tris_channel_lock(xferchan);
		if ((features_datastore = tris_channel_datastore_find(xferchan, &dial_features_info, NULL))) {
			dialfeatures = features_datastore->data;
		}
		tris_channel_unlock(xferchan);
	 
		if (dialfeatures) {
			tris_copy_flags(&(config->features_caller), &(dialfeatures->features_caller), TRIS_FLAGS_ALL);
		}
	 
		tobj->chan = newchan;
		tobj->peer = xferchan;
		tobj->bconfig = *config;

		if (tobj->bconfig.end_bridge_callback_data_fixup) {
			tobj->bconfig.end_bridge_callback_data_fixup(&tobj->bconfig, tobj->peer, tobj->chan);
		}

		/*if (newstate == TRIS_STATE_UP && tris_stream_and_wait(newchan, xfersound, ""))
			tris_log(LOG_WARNING, "Failed to play transfer sound!\n");*/

		if (newchan && !tris_check_hangup(newchan))
			set_channel_not_spy(newchan);
		if (xferchan && !tris_check_hangup(xferchan))
			set_channel_not_spy(xferchan);
		bridge_call_thread_launch(tobj);
		return -1;      /* XXX meaning the channel is bridged ? */
	} else if (!tris_check_hangup(transferee)) {
		set_channel_not_spy(transferee);
		if (newchan && !tris_check_hangup(newchan))
			set_channel_not_spy(newchan);
		/* act as blind transfer */
		if (!ringing && tris_autoservice_stop(transferee) < 0) {
			tris_hangup(newchan);
			return -1;
		}

		if (!newchan)
			return -1;

		/* newchan is up, we should prepare transferee and bridge them */
		if (check_compat(transferee, newchan)) {
			finishup(transferee);
			return -1;
		}
		if (holdstate)
			tris_indicate(newchan, TRIS_CONTROL_UNHOLD);
		else
			tris_indicate(transferee, TRIS_CONTROL_UNHOLD);

		if ((tris_waitfordigit(transferee, 100) < 0)
		   || (tris_waitfordigit(newchan, 100) < 0)
		   || tris_check_hangup(transferee)
		   || tris_check_hangup(newchan)) {
			if (!tris_check_hangup(newchan))
				tris_hangup(newchan);
			return -1;
		}

		xferchan = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, "Transfered/%s", transferee->name);
		if (!xferchan) {
			tris_hangup(newchan);
			return -1;
		}
		/* Make formats okay */
		xferchan->visible_indication = newchan->_state==TRIS_STATE_UP?0:TRIS_CONTROL_RINGING;
		xferchan->readformat = transferee->readformat;
		xferchan->writeformat = transferee->writeformat;
		tris_channel_masquerade(xferchan, transferee);
		tris_explicit_goto(xferchan, transferee->context, transferee->exten, transferee->priority);
		/*xferchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(xferchan, TRIS_FLAGS_ALL);
		tris_channel_lock(xferchan);
		tris_do_masquerade(xferchan);
		tris_channel_unlock(xferchan);
		xferchan->_softhangup = 0;
		newstate = newchan->_state;
		/*newchan->_state = TRIS_STATE_UP;*/
		tris_clear_flag(newchan, TRIS_FLAGS_ALL);
		newchan->_softhangup = 0;
		if (!(tobj = tris_calloc(1, sizeof(*tobj)))) {
			tris_hangup(xferchan);
			tris_hangup(newchan);
			return -1;
		}
		tobj->chan = newchan;
		tobj->peer = xferchan;
		tobj->bconfig = *config;

		if (tobj->bconfig.end_bridge_callback_data_fixup) {
			tobj->bconfig.end_bridge_callback_data_fixup(&tobj->bconfig, tobj->peer, tobj->chan);
		}

		/*if (newstate == TRIS_STATE_UP && tris_stream_and_wait(newchan, xfersound, ""))
			tris_log(LOG_WARNING, "Failed to play transfer sound!\n");*/
		set_channel_not_spy(xferchan);
		bridge_call_thread_launch(tobj);
		return -1;      /* XXX meaning the channel is bridged ? */
	} else {
		/* Transferee hung up */
		finishup(transferee);
		return -1;
	}
}

/*!
 * \brief Blind transfer user to another extension
 * \param chan channel to be transfered
 * \param peer channel initiated blind transfer
 * \param config
 * \param code
 * \param data
 * \param sense  feature options
 * 
 * Place chan on hold, check if transferred to parkinglot extension,
 * otherwise check extension exists and transfer caller.
 * \retval TRIS_FEATURE_RETURN_SUCCESS.
 * \retval -1 on failure.
*/
static int builtin_handle_blind_refer(struct tris_channel *chan, struct tris_channel *peer,
		struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_channel *transferer;
	struct tris_channel *transferee;
	const char *transferer_real_context;
	char xferto[256];
	int res, parkstatus = 0;
	const char *exten = NULL;

	if (!chan || tris_check_hangup(chan) || !peer || tris_check_hangup(peer)) {
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	set_peers(&transferer, &transferee, peer, chan, sense);
	transferer_real_context = real_ctx(transferer, transferee);
	/* Start autoservice on chan while we talk to the originator */
	//tris_autoservice_start(transferee);
	//tris_indicate(transferee, TRIS_CONTROL_HOLD);
	memset(xferto, 0, sizeof(xferto));
	
	exten = transferer->referexten;
	if (!exten || tris_strlen_zero(exten)) {
		tris_log(LOG_WARNING, "Did not read data.\n");
		finishup(transferee);
		if (tris_stream_and_wait(transferer, "beep", ""))
			return -1;
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	snprintf(xferto, sizeof(xferto), "%s", exten);
	transferer->referexten[0] = '\0';
	
	/* Transfer */
	if (!strcmp(xferto, tris_parking_ext())) {
		res = finishup(transferee);
		if (res)
			res = -1;
		else if (!(parkstatus = masq_park_call_announce(transferee, transferer, 0, NULL))) {	/* success */
			/* We return non-zero, but tell the PBX not to hang the channel when
			   the thread dies -- We have to be careful now though.  We are responsible for 
			   hanging up the channel, else it will never be hung up! */
			if (transferer) {
				transferer->transferchan = 1;
				set_channel_not_spy(transferer);
			}
			if (transferee) {
				transferee->transferchan = 2;
				set_channel_not_spy(transferee);
			}

			return 0;
		} else {
			tris_log(LOG_WARNING, "Unable to park call %s, parkstatus = %d\n", transferee->name, parkstatus);
		}
		/*! \todo XXX Maybe we should have another message here instead of invalid extension XXX */
	} else if (tris_exists_extension(transferee, transferer_real_context, xferto, 1, transferer->cid.cid_num)) {
		pbx_builtin_setvar_helper(transferer, "BLINDTRANSFER", transferee->name);
		pbx_builtin_setvar_helper(transferee, "BLINDTRANSFER", transferer->name);
		res=finishup(transferee);
		if (!transferer->cdr) { /* this code should never get called (in a perfect world) */
			transferer->cdr=tris_cdr_alloc();
			if (transferer->cdr) {
				tris_cdr_init(transferer->cdr, transferer); /* initialize our channel's cdr */
				tris_cdr_start(transferer->cdr);
			}
		}
		if (transferer->cdr) {
			struct tris_cdr *swap = transferer->cdr;
			tris_log(LOG_DEBUG,"transferer=%s; transferee=%s; lastapp=%s; lastdata=%s; chan=%s; dstchan=%s\n",
					transferer->name, transferee->name, transferer->cdr->lastapp, transferer->cdr->lastdata, 
					transferer->cdr->channel, transferer->cdr->dstchannel);
			tris_log(LOG_DEBUG,"TRANSFEREE; lastapp=%s; lastdata=%s, chan=%s; dstchan=%s\n",
					transferee->cdr->lastapp, transferee->cdr->lastdata, transferee->cdr->channel, transferee->cdr->dstchannel);
			tris_log(LOG_DEBUG,"transferer_real_context=%s; xferto=%s\n", transferer_real_context, xferto);
			/* swap cdrs-- it will save us some time & work */
			transferer->cdr = transferee->cdr;
			transferee->cdr = swap;
		}
		if (!transferee->pbx) {
			/* Doh!  Use our handy async_goto functions */
			tris_verb(3, "Transferring %s to '%s' (context %s) priority 1\n"
								,transferee->name, xferto, transferer_real_context);
			if (tris_async_goto(transferee, transferer_real_context, xferto, 1))
				tris_log(LOG_WARNING, "Async goto failed :-(\n");
		} else {
			/* Set the channel's new extension, since it exists, using transferer context */
			tris_set_flag(transferee, TRIS_FLAG_BRIDGE_HANGUP_DONT); /* don't let the after-bridge code run the h-exten */
			tris_log(LOG_DEBUG,"ABOUT TO TRIS_ASYNC_GOTO, have a pbx... set HANGUP_DONT on chan=%s\n", transferee->name);
			set_c_e_p(transferee, transferer_real_context, xferto, 0);
		}
		check_goto_on_transfer(transferer);
		if (transferer) {
			transferer->transferchan = 1;
			set_channel_not_spy(transferer);
		}
		if (transferee) {
			transferee->transferchan = 2;
			set_channel_not_spy(transferee);
		}
		if (transferer && transferee)
			tris_set_callerid(transferee, transferer->cid.cid_num, transferer->cid.cid_name, transferer->cid.cid_ani);
		return res;
	} else {
		tris_verb(3, "Unable to find extension '%s' in context '%s'\n", xferto, transferer_real_context);
	}
	if (parkstatus != TRIS_FEATURE_RETURN_PARKFAILED && tris_stream_and_wait(transferer, xferfailsound, TRIS_DIGIT_ANY) < 0) {
		finishup(transferee);
		return -1;
	}
	tris_stopstream(transferer);
	res = finishup(transferee);
	if (res) {
		tris_verb(2, "Hungup during autoservice stop on '%s'\n", transferee->name);
		return res;
	}
	res = TRIS_FEATURE_RETURN_SUCCESS;
	return res;
}

/*!
 * \brief Blind transfer user to another extension
 * \param chan channel to be transfered
 * \param peer channel initiated blind transfer
 * \param config
 * \param code
 * \param data
 * \param sense  feature options
 * 
 * Place chan on hold, check if transferred to parkinglot extension,
 * otherwise check extension exists and transfer caller.
 * \retval TRIS_FEATURE_RETURN_SUCCESS.
 * \retval -1 on failure.
*/
static int builtin_handle_announce_refer(struct tris_channel *chan, struct tris_channel *peer,
		struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_channel *transferer;
	struct tris_channel *transferee;
	char xferto[256];
	int res = 0;
	const char *exten = NULL;

	if (!chan || tris_check_hangup(chan) || !peer || tris_check_hangup(peer)) {
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	set_peers(&transferer, &transferee, peer, chan, sense);
	memset(xferto, 0, sizeof(xferto));
	
	exten = transferer->referexten;
	if (!exten || tris_strlen_zero(exten)) {
		tris_log(LOG_WARNING, "Did not read data.\n");
		finishup(transferee);
		if (tris_stream_and_wait(transferer, "beep", ""))
			return -1;
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	snprintf(xferto, sizeof(xferto), "%s", exten);
	transferer->referexten[0] = '\0';
	
	pbx_builtin_setvar_helper(transferee, "XFERTO", xferto);
	res=finishup(transferee);
	tris_set_flag(transferee, TRIS_FLAG_BRIDGE_HANGUP_DONT);
	tris_log(LOG_DEBUG,"ABOUT TO TRIS_ASYNC_GOTO, have a pbx... set HANGUP_DONT on chan=%s\n", transferee->name);
	set_channel_not_spy(transferee);
	if (!transferee->pbx) {
		if (tris_async_goto(transferee, "tone", "OPERA", 1))
			tris_log(LOG_WARNING, "Async goto failed :-(\n");
	} else {
		set_c_e_p(transferee, "tone", "OPERA", 0);
	}
	tris_indicate(transferer, TRIS_CONTROL_NOTIFY_ANNOUNCE);
	check_goto_on_transfer(transferer);
	return res;
}

/*!
*/
static int builtin_handle_refer(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_channel *transferer;
	struct tris_channel *transferee;
	int refertype = 0;
	int referaction = 0;

	if (!chan || tris_check_hangup(chan) || !peer || tris_check_hangup(peer)) {
		return TRIS_FEATURE_RETURN_SUCCESS;
	}
	tris_debug(1, "Executing Attended Transfer %s, %s (sense=%d) \n", chan->name, peer->name, sense);
	set_peers(&transferer, &transferee, peer, chan, sense);
	refertype = transferer->refertype;
	referaction = transferer->referaction;
	if (refertype == TRIS_REFER_TYPE_REFER) {
		if (referaction == TRIS_REFER_ACTION_ATTENDED) {
			return builtin_handle_attended_refer(chan, peer, config, code, sense, data);
		} else if (referaction == TRIS_REFER_ACTION_BLIND) {
			return builtin_handle_blind_refer(chan, peer, config, code, sense, data);
		} else if (referaction == TRIS_REFER_ACTION_ANNOUNCE) {
			return builtin_handle_announce_refer(chan, peer, config, code, sense, data);
		}
	}
	return builtin_handle_attended_refer(chan, peer, config, code, sense, data);
}

/* add atxfer and automon as undefined so you can only use em if you configure them */
#define FEATURES_COUNT ARRAY_LEN(builtin_features)

TRIS_RWLOCK_DEFINE_STATIC(features_lock);

static struct tris_call_feature builtin_features[] = 
{
	{ TRIS_FEATURE_REDIRECT, "Blind Transfer", "blindxfer", "#", "#", builtin_blindtransfer, TRIS_FEATURE_FLAG_NEEDSDTMF, "" },
	{ TRIS_FEATURE_REDIRECT, "Attended Transfer", "atxfer", "*", "*", builtin_atxfer, TRIS_FEATURE_FLAG_NEEDSDTMF, "" },
	{ TRIS_FEATURE_AUTOMON, "One Touch Monitor", "automon", "", "", builtin_automonitor, TRIS_FEATURE_FLAG_NEEDSDTMF, "" },
	{ TRIS_FEATURE_DISCONNECT, "Disconnect Call", "disconnect", "*", "*", builtin_disconnect, TRIS_FEATURE_FLAG_NEEDSDTMF, "" },
	{ TRIS_FEATURE_PARKCALL, "Park Call", "parkcall", "", "", builtin_parkcall, TRIS_FEATURE_FLAG_NEEDSDTMF, "" },
	{ TRIS_FEATURE_AUTOMIXMON, "One Touch MixMonitor", "automixmon", "", "", builtin_automixmonitor, TRIS_FEATURE_FLAG_NEEDSDTMF, "" },
};


static TRIS_RWLIST_HEAD_STATIC(feature_list, tris_call_feature);

/*! \brief register new feature into feature_list*/
void tris_register_feature(struct tris_call_feature *feature)
{
	if (!feature) {
		tris_log(LOG_NOTICE,"You didn't pass a feature!\n");
		return;
	}
  
	TRIS_RWLIST_WRLOCK(&feature_list);
	TRIS_RWLIST_INSERT_HEAD(&feature_list,feature,feature_entry);
	TRIS_RWLIST_UNLOCK(&feature_list);

	tris_verb(2, "Registered Feature '%s'\n",feature->sname);
}

/*! 
 * \brief Add new feature group
 * \param fgname feature group name.
 *
 * Add new feature group to the feature group list insert at head of list.
 * \note This function MUST be called while feature_groups is locked.
*/
static struct feature_group* register_group(const char *fgname)
{
	struct feature_group *fg;

	if (!fgname) {
		tris_log(LOG_NOTICE, "You didn't pass a new group name!\n");
		return NULL;
	}

	if (!(fg = tris_calloc(1, sizeof(*fg))))
		return NULL;

	if (tris_string_field_init(fg, 128)) {
		tris_free(fg);
		return NULL;
	}

	tris_string_field_set(fg, gname, fgname);

	TRIS_LIST_INSERT_HEAD(&feature_groups, fg, entry);

	tris_verb(2, "Registered group '%s'\n", fg->gname);

	return fg;
}

/*! 
 * \brief Add feature to group
 * \param fg feature group
 * \param exten
 * \param feature feature to add.
 *
 * Check fg and feature specified, add feature to list
 * \note This function MUST be called while feature_groups is locked. 
*/
static void register_group_feature(struct feature_group *fg, const char *exten, struct tris_call_feature *feature) 
{
	struct feature_group_exten *fge;

	if (!fg) {
		tris_log(LOG_NOTICE, "You didn't pass a group!\n");
		return;
	}

	if (!feature) {
		tris_log(LOG_NOTICE, "You didn't pass a feature!\n");
		return;
	}

	if (!(fge = tris_calloc(1, sizeof(*fge))))
		return;

	if (tris_string_field_init(fge, 128)) {
		tris_free(fge);
		return;
	}

	tris_string_field_set(fge, exten, S_OR(exten, feature->exten));

	fge->feature = feature;

	TRIS_LIST_INSERT_HEAD(&fg->features, fge, entry);		

	tris_verb(2, "Registered feature '%s' for group '%s' at exten '%s'\n",
					feature->sname, fg->gname, exten);
}

void tris_unregister_feature(struct tris_call_feature *feature)
{
	if (!feature) {
		return;
	}

	TRIS_RWLIST_WRLOCK(&feature_list);
	TRIS_RWLIST_REMOVE(&feature_list, feature, feature_entry);
	TRIS_RWLIST_UNLOCK(&feature_list);

	tris_free(feature);
}

/*! \brief Remove all features in the list */
static void tris_unregister_features(void)
{
	struct tris_call_feature *feature;

	TRIS_RWLIST_WRLOCK(&feature_list);
	while ((feature = TRIS_RWLIST_REMOVE_HEAD(&feature_list, feature_entry))) {
		tris_free(feature);
	}
	TRIS_RWLIST_UNLOCK(&feature_list);
}

/*! \brief find a call feature by name */
static struct tris_call_feature *find_dynamic_feature(const char *name)
{
	struct tris_call_feature *tmp;

	TRIS_RWLIST_TRAVERSE(&feature_list, tmp, feature_entry) {
		if (!strcasecmp(tmp->sname, name)) {
			break;
		}
	}

	return tmp;
}

/*! \brief Remove all feature groups in the list */
static void tris_unregister_groups(void)
{
	struct feature_group *fg;
	struct feature_group_exten *fge;

	TRIS_RWLIST_WRLOCK(&feature_groups);
	while ((fg = TRIS_LIST_REMOVE_HEAD(&feature_groups, entry))) {
		while ((fge = TRIS_LIST_REMOVE_HEAD(&fg->features, entry))) {
			tris_string_field_free_memory(fge);
			tris_free(fge);
		}

		tris_string_field_free_memory(fg);
		tris_free(fg);
	}
	TRIS_RWLIST_UNLOCK(&feature_groups);
}

/*! 
 * \brief Find a group by name 
 * \param name feature name
 * \retval feature group on success.
 * \retval NULL on failure.
*/
static struct feature_group *find_group(const char *name) {
	struct feature_group *fg = NULL;

	TRIS_LIST_TRAVERSE(&feature_groups, fg, entry) {
		if (!strcasecmp(fg->gname, name))
			break;
	}

	return fg;
}

void tris_rdlock_call_features(void)
{
	tris_rwlock_rdlock(&features_lock);
}

void tris_unlock_call_features(void)
{
	tris_rwlock_unlock(&features_lock);
}

struct tris_call_feature *tris_find_call_feature(const char *name)
{
	int x;
	for (x = 0; x < FEATURES_COUNT; x++) {
		if (!strcasecmp(name, builtin_features[x].sname))
			return &builtin_features[x];
	}
	return NULL;
}

/*!
 * \brief exec an app by feature 
 * \param chan,peer,config,code,sense,data
 *
 * Find a feature, determine which channel activated
 * \retval TRIS_FEATURE_RETURN_NO_HANGUP_PEER
 * \retval -1 error.
 * \retval -2 when an application cannot be found.
*/
static int feature_exec_app(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data)
{
	struct tris_app *app;
	struct tris_call_feature *feature = data;
	struct tris_channel *work, *idle;
	int res;

	if (!feature) { /* shouldn't ever happen! */
		tris_log(LOG_NOTICE, "Found feature before, but at execing we've lost it??\n");
		return -1; 
	}

	if (sense == FEATURE_SENSE_CHAN) {
		if (!tris_test_flag(feature, TRIS_FEATURE_FLAG_BYCALLER))
			return TRIS_FEATURE_RETURN_KEEPTRYING;
		if (tris_test_flag(feature, TRIS_FEATURE_FLAG_ONSELF)) {
			work = chan;
			idle = peer;
		} else {
			work = peer;
			idle = chan;
		}
	} else {
		if (!tris_test_flag(feature, TRIS_FEATURE_FLAG_BYCALLEE))
			return TRIS_FEATURE_RETURN_KEEPTRYING;
		if (tris_test_flag(feature, TRIS_FEATURE_FLAG_ONSELF)) {
			work = peer;
			idle = chan;
		} else {
			work = chan;
			idle = peer;
		}
	}

	if (!(app = pbx_findapp(feature->app))) {
		tris_log(LOG_WARNING, "Could not find application (%s)\n", feature->app);
		return -2;
	}

	tris_autoservice_start(idle);
	
	if (!tris_strlen_zero(feature->moh_class))
		tris_moh_start(idle, feature->moh_class, NULL);

	res = pbx_exec(work, app, feature->app_args);

	if (!tris_strlen_zero(feature->moh_class))
		tris_moh_stop(idle);

	tris_autoservice_stop(idle);

	if (res) {
		return TRIS_FEATURE_RETURN_SUCCESSBREAK;
	}
	return TRIS_FEATURE_RETURN_SUCCESS;	/*! \todo XXX should probably return res */
}

static void unmap_features(void)
{
	int x;

	tris_rwlock_wrlock(&features_lock);
	for (x = 0; x < FEATURES_COUNT; x++)
		strcpy(builtin_features[x].exten, builtin_features[x].default_exten);
	tris_rwlock_unlock(&features_lock);
}

static int remap_feature(const char *name, const char *value)
{
	int x, res = -1;

	tris_rwlock_wrlock(&features_lock);
	for (x = 0; x < FEATURES_COUNT; x++) {
		if (strcasecmp(builtin_features[x].sname, name))
			continue;

		tris_copy_string(builtin_features[x].exten, value, sizeof(builtin_features[x].exten));
		res = 0;
		break;
	}
	tris_rwlock_unlock(&features_lock);

	return res;
}

/*!
 * \brief Check the dynamic features
 * \param chan,peer,config,code,sense
 *
 * Lock features list, browse for code, unlock list
 * \retval res on success.
 * \retval -1 on failure.
*/
static int feature_interpret(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense)
{
	int x;
	struct tris_flags features;
	struct tris_call_feature *feature;
	struct feature_group *fg = NULL;
	struct feature_group_exten *fge;
	const char *peer_dynamic_features, *chan_dynamic_features;
	char dynamic_features_buf[128];
	char *tmp, *tok;
	int res = TRIS_FEATURE_RETURN_PASSDIGITS;
	int feature_detected = 0;

	if (sense == FEATURE_SENSE_CHAN) {
		tris_copy_flags(&features, &(config->features_caller), TRIS_FLAGS_ALL);
	}
	else {
		tris_copy_flags(&features, &(config->features_callee), TRIS_FLAGS_ALL);
	}

	tris_channel_lock(peer);
	peer_dynamic_features = tris_strdupa(S_OR(pbx_builtin_getvar_helper(peer, "DYNAMIC_FEATURES"),""));
	tris_channel_unlock(peer);

	tris_channel_lock(chan);
	chan_dynamic_features = tris_strdupa(S_OR(pbx_builtin_getvar_helper(chan, "DYNAMIC_FEATURES"),""));
	tris_channel_unlock(chan);

	snprintf(dynamic_features_buf, sizeof(dynamic_features_buf), "%s%s%s", S_OR(chan_dynamic_features, ""), chan_dynamic_features && peer_dynamic_features ? "#" : "", S_OR(peer_dynamic_features,""));

	tris_debug(3, "Feature interpret: chan=%s, peer=%s, code=%s, sense=%d, features=%d, dynamic=%s\n", chan->name, peer->name, code, sense, features.flags, dynamic_features_buf);

	tris_rwlock_rdlock(&features_lock);
	for (x = 0; x < FEATURES_COUNT; x++) {
		if ((tris_test_flag(&features, builtin_features[x].feature_mask)) &&
		    !tris_strlen_zero(builtin_features[x].exten)) {
			/* Feature is up for consideration */
			if (!strcmp(builtin_features[x].exten, code)) {
				tris_debug(3, "Feature detected: fname=%s sname=%s exten=%s\n", builtin_features[x].fname, builtin_features[x].sname, builtin_features[x].exten);
				res = builtin_features[x].operation(chan, peer, config, code, sense, NULL);
				feature_detected = 1;
				break;
			} else if (!strncmp(builtin_features[x].exten, code, strlen(code))) {
				if (res == TRIS_FEATURE_RETURN_PASSDIGITS)
					res = TRIS_FEATURE_RETURN_STOREDIGITS;
			}
		}
	}
	tris_rwlock_unlock(&features_lock);

	if (tris_strlen_zero(dynamic_features_buf) || feature_detected)
		return res;

	tmp = dynamic_features_buf;

	while ((tok = strsep(&tmp, "#"))) {
		TRIS_RWLIST_RDLOCK(&feature_groups);

		fg = find_group(tok);

		if (fg) {
			TRIS_LIST_TRAVERSE(&fg->features, fge, entry) {
				if (strcasecmp(fge->exten, code))
					continue;

				res = fge->feature->operation(chan, peer, config, code, sense, fge->feature);
				if (res != TRIS_FEATURE_RETURN_KEEPTRYING) {
					TRIS_RWLIST_UNLOCK(&feature_groups);
					break;
				}
				res = TRIS_FEATURE_RETURN_PASSDIGITS;
			}
			if (fge)
				break;
		}

		TRIS_RWLIST_UNLOCK(&feature_groups);

		TRIS_RWLIST_RDLOCK(&feature_list);

		if (!(feature = find_dynamic_feature(tok))) {
			TRIS_RWLIST_UNLOCK(&feature_list);
			continue;
		}
			
		/* Feature is up for consideration */
		if (!strcmp(feature->exten, code)) {
			tris_verb(3, " Feature Found: %s exten: %s\n",feature->sname, tok);
			res = feature->operation(chan, peer, config, code, sense, feature);
			if (res != TRIS_FEATURE_RETURN_KEEPTRYING) {
				TRIS_RWLIST_UNLOCK(&feature_list);
				break;
			}
			res = TRIS_FEATURE_RETURN_PASSDIGITS;
		} else if (!strncmp(feature->exten, code, strlen(code)))
			res = TRIS_FEATURE_RETURN_STOREDIGITS;

		TRIS_RWLIST_UNLOCK(&feature_list);
	}
	
	return res;
}

static void set_config_flags(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config)
{
	int x;
	
	tris_clear_flag(config, TRIS_FLAGS_ALL);

	tris_rwlock_rdlock(&features_lock);
	for (x = 0; x < FEATURES_COUNT; x++) {
		if (!tris_test_flag(builtin_features + x, TRIS_FEATURE_FLAG_NEEDSDTMF))
			continue;

		if (tris_test_flag(&(config->features_caller), builtin_features[x].feature_mask))
			tris_set_flag(config, TRIS_BRIDGE_DTMF_CHANNEL_0);

		if (tris_test_flag(&(config->features_callee), builtin_features[x].feature_mask))
			tris_set_flag(config, TRIS_BRIDGE_DTMF_CHANNEL_1);
	}
	tris_rwlock_unlock(&features_lock);
	
	if (chan && peer && !(tris_test_flag(config, TRIS_BRIDGE_DTMF_CHANNEL_0) && tris_test_flag(config, TRIS_BRIDGE_DTMF_CHANNEL_1))) {
		const char *dynamic_features = pbx_builtin_getvar_helper(chan, "DYNAMIC_FEATURES");

		if (dynamic_features) {
			char *tmp = tris_strdupa(dynamic_features);
			char *tok;
			struct tris_call_feature *feature;

			/* while we have a feature */
			while ((tok = strsep(&tmp, "#"))) {
				TRIS_RWLIST_RDLOCK(&feature_list);
				if ((feature = find_dynamic_feature(tok)) && tris_test_flag(feature, TRIS_FEATURE_FLAG_NEEDSDTMF)) {
					if (tris_test_flag(feature, TRIS_FEATURE_FLAG_BYCALLER))
						tris_set_flag(config, TRIS_BRIDGE_DTMF_CHANNEL_0);
					if (tris_test_flag(feature, TRIS_FEATURE_FLAG_BYCALLEE))
						tris_set_flag(config, TRIS_BRIDGE_DTMF_CHANNEL_1);
				}
				TRIS_RWLIST_UNLOCK(&feature_list);
			}
		}
	}
}

int send_control_notify(struct tris_channel* caller, enum tris_control_frame_type ctype, int referid, int notifycaller)
{
	if (notifycaller == 0 || notifycaller == 3 || notifycaller == 4) {
		caller->seqno = referid;
		tris_indicate(caller, ctype);
	}
	return 0;
}

/*! 
 * \brief Get feature and dial
 * \param caller,transferee,type,format,data,timeout,outstate,cid_num,cid_name,igncallerstate
 *
 * Request channel, set channel variables, initiate call,check if they want to disconnect
 * go into loop, check if timeout has elapsed, check if person to be transfered hung up,
 * check for answer break loop, set cdr return channel.
 *
 * \todo XXX Check - this is very similar to the code in channel.c 
 * \return always a channel
*/
static struct tris_channel *feature_request_and_dial(struct tris_channel *caller, struct tris_channel *transferee, 
		const char *type, int format, void *data, int timeout, int *outstate, const char *cid_num, const char *cid_name, 
		int igncallerstate, const char *language, int ringing, int notifycaller)
{
	int state = 0;
	int cause = 0;
	int to;
	struct tris_channel *chan;
	struct tris_channel *monitor_chans[2];
	struct tris_channel *active_channel;
	int res = 0, ready = 1;
	struct timeval started;
	int x, len = 0;
	char *disconnect_code = NULL, *dialed_code = NULL;
	int is_calling = 0;
	const char* busy_peer, *exten = NULL;
	int send_caller_bye = 0;
	char tmp[256];
	struct tris_app *the_app;

	if (!(chan = tris_request(type, format, data, &cause, 0))) {
		tris_log(LOG_NOTICE, "Unable to request channel %s/%s\n", type, (char *)data);
		switch(cause) {
		case TRIS_CAUSE_BUSY:
			state = TRIS_CONTROL_BUSY;
			break;
		case TRIS_CAUSE_CONGESTION:
			state = TRIS_CONTROL_CONGESTION;
			break;
		}
		goto done;
	}

	tris_set_callerid(chan, cid_num, cid_name, cid_num);
	tris_string_field_set(chan, language, language);
	tris_channel_inherit_variables(caller, chan);
	pbx_builtin_setvar_helper(chan, "TRANSFERERNAME", caller->name);
	chan->transferchan = 1;
	snprintf(tmp, sizeof(tmp), "%d", notifycaller);
	pbx_builtin_setvar_helper(chan, "notifycaller", tmp);
		
	snprintf(tmp, sizeof(tmp), "Call-Info: MP,queue,1");
	the_app = pbx_findapp("SWITCHAddHeader");
	if (the_app)
		pbx_exec(chan, the_app, tmp);
		
	if (tris_call(chan, data, timeout)) {
		tris_log(LOG_NOTICE, "Unable to call channel %s/%s\n", type, (char *)data);
		goto done;
	}
	
	/*tris_indicate(caller, TRIS_CONTROL_RINGING);*/
	/* support dialing of the featuremap disconnect code while performing an attended tranfer */
	tris_rwlock_rdlock(&features_lock);
	for (x = 0; x < FEATURES_COUNT; x++) {
		if (strcasecmp(builtin_features[x].sname, "disconnect"))
			continue;

		disconnect_code = builtin_features[x].exten;
		len = strlen(disconnect_code) + 1;
		dialed_code = alloca(len);
		memset(dialed_code, 0, len);
		break;
	}
	tris_rwlock_unlock(&features_lock);
	x = 0;
	started = tris_tvnow();
	to = timeout;

	tris_poll_channel_add(caller, chan);

	monitor_chans[0] = caller;
	monitor_chans[1] = chan;
	while (!((transferee && tris_check_hangup(transferee)) || (!igncallerstate && tris_check_hangup(caller)) || 
			(chan && tris_check_hangup(chan))) && timeout) {
		struct tris_frame *f = NULL;

		active_channel = tris_waitfor_n(monitor_chans, 2, &to);

		/* see if the timeout has been violated */
		if(chan->_state != TRIS_STATE_UP && tris_tvdiff_ms(tris_tvnow(), started) > timeout) {
			state = TRIS_CONTROL_UNHOLD;
			tris_log(LOG_NOTICE, "We exceeded our AT-timeout\n");
			break; /*doh! timeout*/
		}

		if (!active_channel)
			continue;

		if (!send_caller_bye && tris_check_hangup(transferee)) {
			tris_indicate(caller, TRIS_CONTROL_NOTIFY_CALLERBYE);
			send_caller_bye = 1;
		}
		if (monitor_chans[1] && (monitor_chans[1] == active_channel)){
			if (!tris_strlen_zero(monitor_chans[1]->call_forward)) {
				if (!(monitor_chans[1] = tris_call_forward(caller, monitor_chans[1], &to, format, NULL, outstate))) {
					return NULL;
				}
				continue;
			}
			f = tris_read(monitor_chans[1]);
			if (f == NULL) { /*doh! where'd he go?*/
				send_control_notify(caller, TRIS_CONTROL_NOTIFY_CALLERBYE, active_channel->referid, notifycaller);
				state = TRIS_CONTROL_HANGUP;
				res = 0;
				ready = 0;
				break;
			}
			
			if (f->frametype == TRIS_FRAME_CONTROL || f->frametype == TRIS_FRAME_DTMF || f->frametype == TRIS_FRAME_TEXT) {
				if (f->subclass == TRIS_CONTROL_RINGING) {
					state = f->subclass;
					tris_verb(3, "%s is ringing\n", monitor_chans[1]->name);
					tris_indicate(caller, TRIS_CONTROL_RINGING);
					if (notifycaller % 2 == 1) {
						if (!(*outstate))
							tris_indicate(transferee, TRIS_CONTROL_UNHOLD);
						if (!ringing)
							tris_streamfile(transferee, "conference/ringing", transferee->language);
						*outstate = TRIS_CONTROL_RINGING;
					}
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_RINGING, active_channel->referid, notifycaller);
				} else if (f->subclass == TRIS_CONTROL_PROCEEDING) {
					tris_verb(3, "%s is proceeding\n", active_channel->name);
					tris_indicate(caller, TRIS_CONTROL_PROCEEDING);
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_PROCEEDING, active_channel->referid, notifycaller);
				} else if (f->subclass == TRIS_CONTROL_PROGRESS) {
					tris_verb(3, "%s is progressing\n", active_channel->name);
					tris_indicate(caller, TRIS_CONTROL_PROGRESS);
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_PROGRESS, active_channel->referid, notifycaller);
				} else if ((f->subclass == TRIS_CONTROL_BUSY) || (f->subclass == TRIS_CONTROL_CONGESTION) ||
							(f->subclass == TRIS_CONTROL_TIMEOUT)  || (f->subclass == TRIS_CONTROL_FORBIDDEN) ||
							(f->subclass == TRIS_CONTROL_ROUTEFAIL)  || (f->subclass == TRIS_CONTROL_REJECTED) ||
							(f->subclass == TRIS_CONTROL_UNAVAILABLE) || (f->subclass == TRIS_CONTROL_OFFHOOK) ||
							(f->subclass == TRIS_CONTROL_TAKEOFFHOOK)) {
					state = f->subclass;
					if (f->subclass == TRIS_CONTROL_BUSY) {
						tris_verb(3, "%s is busy\n", active_channel->name);
						busy_peer = pbx_builtin_getvar_helper(active_channel, "Error-Info");
						if (!tris_strlen_zero(busy_peer)) {
							exten = strchr(busy_peer, ',');
							if (!tris_strlen_zero(exten))
								pbx_builtin_setvar_helper(caller, "Busy-Peer", exten+1);
						}
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_BUSY, active_channel->referid, notifycaller);
						if (notifycaller) {
							if (!tris_strlen_zero(busy_peer) && !tris_strlen_zero(exten)) {
								tris_play_and_wait(caller, "dial/is_used");
								tris_play_and_wait(caller, "dial/dial-exten-num-is");
								tris_say_digit_str(caller, exten+1, "", caller->language);
								tris_play_and_wait(caller, "dial/dial-is");
							} else {
								tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
							}
						}
					} else if (f->subclass == TRIS_CONTROL_CONGESTION) {
						tris_verb(3, "%s is congestion\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_BUSY, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_ROUTEFAIL) {
						tris_verb(3, "%s is circuit-busy\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_CIRCUITS, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_UNAVAILABLE) {
						tris_verb(3, "%s is unavailable\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_BUSY, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_FORBIDDEN) {
						tris_verb(3, "%s is forbidden\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_FORBIDDEN, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-forbidden", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_OFFHOOK) {
						tris_verb(3, "%s is offhook\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_OFFHOOK, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-not-found", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_TAKEOFFHOOK) {
						tris_verb(3, "%s is takeoffhook\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_TAKEOFFHOOK, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-not-registered", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_TIMEOUT) {
						tris_verb(3, "%s is timeout\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_TIMEOUT, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-no-answer", TRIS_DIGIT_ANY);
						}
					} else {
						tris_verb(3, "%s is busy\n", active_channel->name);
						tris_indicate(caller, f->subclass);
					}
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_CALLERBYE, active_channel->referid, notifycaller);
					tris_frfree(f);
					f = NULL;
					ready = 0;
					if (notifycaller % 2 == 1) {
						if (*outstate == TRIS_CONTROL_RINGING)
							tris_stopstream(transferee);
						*outstate = TRIS_CONTROL_BUSY;
					}
					break;
				} else if (f->subclass == TRIS_CONTROL_ANSWER) {
					/* This is what we are hoping for */
					state = f->subclass;
					tris_frfree(f);
					f = NULL;
					ready=2;
					if (notifycaller % 2 == 1) {
						if (*outstate == TRIS_CONTROL_RINGING)
							tris_stopstream(transferee);
						*outstate = TRIS_CONTROL_ANSWER;
					}
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_ANSWER, monitor_chans[1]->referid, notifycaller);
				} else if (f->subclass >= TRIS_CONTROL_NOTIFY_PROCEEDING && f->subclass <= TRIS_CONTROL_NOTIFY_CIRCUITS) {
					busy_peer = pbx_builtin_getvar_helper(monitor_chans[1], "Busy-Peer");
					if (busy_peer)
						pbx_builtin_setvar_helper(caller, "Busy-Peer", busy_peer);
					send_control_notify(caller, f->subclass, active_channel->referid, notifycaller);
				} else if (f->subclass == TRIS_CONTROL_HOLD || f->subclass == TRIS_CONTROL_UNHOLD ||
						f->subclass == TRIS_CONTROL_VIDUPDATE || f->subclass == TRIS_CONTROL_SRCUPDATE ||
						f->subclass == -1) {
					tris_indicate(caller, f->subclass);
				} else if (f->subclass != -1 && f->subclass != TRIS_CONTROL_PROGRESS) {
					tris_log(LOG_NOTICE, "Don't know what to do about control frame: %d\n", f->subclass);
				}
				/* else who cares */
			} else if (f->frametype == TRIS_FRAME_VOICE || f->frametype == TRIS_FRAME_VIDEO) {
				tris_write(caller, f);
				if (notifycaller % 2 == 1) {
					if (!(*outstate))
						tris_indicate(transferee, TRIS_CONTROL_UNHOLD);
					else if (*outstate == TRIS_CONTROL_RINGING)
						tris_stopstream(transferee);
					if (is_calling != 1)
						tris_write(transferee, f);
					*outstate = TRIS_CONTROL_PROGRESS;
				}
			}

		} else if (caller && (active_channel == caller)) {
			f = tris_read(caller);
			if (f == NULL) { /*doh! where'd he go?*/
				if (!igncallerstate) {
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_CALLERBYE, active_channel->referid, notifycaller);
					if (tris_check_hangup(caller) && !tris_check_hangup(chan)) {
						/* make this a blind transfer */
						ready = 1;
						break;
					}
					state = TRIS_CONTROL_HANGUP;
					res = 0;
					ready = 0;
					break;
				}
			} else {
			
				if (f->frametype == TRIS_FRAME_DTMF) {
					if (f->subclass == '1') {
						if (ready == 2) {
							finishup(transferee);
							tris_indicate(chan, TRIS_CONTROL_HOLD);
							if (!ringing)
								tris_autoservice_start(chan);
							monitor_chans[1] = transferee;
						}
						is_calling = 1;
					} else if (f->subclass == '2') {
						if (ready == 2) {
							finishup(chan);
							tris_indicate(transferee, TRIS_CONTROL_HOLD);
							if (!ringing)
								tris_autoservice_start(transferee);
							monitor_chans[1] = chan;
						}
						if (is_calling == 1)
							is_calling = 2;
					}
					dialed_code[x++] = f->subclass;
					dialed_code[x] = '\0';
					if (strlen(dialed_code) == len) {
						x = 0;
					} else if (x && strncmp(dialed_code, disconnect_code, x)) {
						x = 0;
						dialed_code[x] = '\0';
					}
					if (*dialed_code && !strcmp(dialed_code, disconnect_code)) {
						/* Caller Canceled the call */
						ready = 0;
						state = TRIS_CONTROL_UNHOLD;
						tris_frfree(f);
						f = NULL;
						break;
					}
				} else if (f->frametype == TRIS_FRAME_VOICE || f->frametype == TRIS_FRAME_VIDEO) {
					tris_write(monitor_chans[1], f);
					if (notifycaller % 2 == 1) {
						if (is_calling == 2) {
							if (*outstate == TRIS_CONTROL_RINGING && !ringing) {
								tris_streamfile(transferee, "conference/ringing", transferee->language);
							}
						} else if (is_calling == 1) {
							if (*outstate == TRIS_CONTROL_RINGING) {
								tris_stopstream(transferee);
							}
							tris_write(transferee, f);
						}
					}
				}
			}
		}
		if (f)
			tris_frfree(f);
	} /* end while */

	tris_poll_channel_del(caller, chan);
		
done:
	tris_indicate(caller, -1);
	if (chan) {
		if (!ready) {
			if (monitor_chans[1] == chan) {
				if (caller && !tris_check_hangup(caller))
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_BYE, monitor_chans[1]->referid, notifycaller);
				res = -1;
				tris_hangup(chan);
				chan = NULL;
			}
		} else {
			state = TRIS_CONTROL_ANSWER;
			res = 0;
		}
	} else {
		res = -1;
	}

	if (notifycaller % 2 == 1) {	
		if (!chan && is_calling == 1 && *outstate == TRIS_CONTROL_RINGING && !tris_check_hangup(transferee) && !ringing)
			tris_streamfile(transferee, "conference/ringing", transferee->language);
	}

	if (outstate)
		*outstate = state;

	return chan;
}


static int feature_connect_channels(struct tris_channel *transferee, struct tris_channel *newchan,
		struct tris_channel* transferer, struct tris_bridge_config *config)
{
	struct tris_channel *xferchan;
	struct tris_bridge_thread_obj *tobj;
	struct tris_datastore *features_datastore;
	struct tris_dial_features *dialfeatures = NULL;
	enum tris_channel_state newstate = 0;

	if (check_compat(transferee, newchan)) {
		return -1;
	}
	
	if ((tris_waitfordigit(transferee, 100) < 0)
	 || (tris_waitfordigit(newchan, 100) < 0)
	 || tris_check_hangup(transferee)
	 || tris_check_hangup(newchan)) {
		tris_hangup(newchan);
		return -1;
	}
	xferchan = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, "Transfered/%s", transferee->name);
	if (!xferchan) {
		tris_hangup(newchan);
		return -1;
	}
	/* Make formats okay */
	xferchan->visible_indication = TRIS_CONTROL_RINGING;
	xferchan->readformat = transferee->readformat;
	xferchan->writeformat = transferee->writeformat;
	tris_channel_masquerade(xferchan, transferee);
	tris_explicit_goto(xferchan, transferee->context, transferee->exten, transferee->priority);
	/*xferchan->_state = TRIS_STATE_UP;*/
	tris_clear_flag(xferchan, TRIS_FLAGS_ALL);
	tris_channel_lock(xferchan);
	tris_do_masquerade(xferchan);
	tris_channel_unlock(xferchan);
	xferchan->_softhangup = 0;
	newstate = newchan->_state;
	/*newchan->_state = TRIS_STATE_UP;*/
	tris_clear_flag(newchan, TRIS_FLAGS_ALL);
	newchan->_softhangup = 0;
	if (!(tobj = tris_calloc(1, sizeof(*tobj)))) {
		tris_hangup(xferchan);
		tris_hangup(newchan);
		return -1;
	}
	
	tris_channel_lock(newchan);
	if ((features_datastore = tris_channel_datastore_find(newchan, &dial_features_info, NULL))) {
			dialfeatures = features_datastore->data;
	}
	tris_channel_unlock(newchan);
	
	if (dialfeatures) {
		/* newchan should always be the callee and shows up as callee in dialfeatures, but for some reason
		   I don't currently understand, the abilities of newchan seem to be stored on the caller side */
		tris_copy_flags(&(config->features_callee), &(dialfeatures->features_caller), TRIS_FLAGS_ALL);
		dialfeatures = NULL;
	}
	
	tris_channel_lock(xferchan);
	if ((features_datastore = tris_channel_datastore_find(xferchan, &dial_features_info, NULL))) {
		dialfeatures = features_datastore->data;
	}
	tris_channel_unlock(xferchan);
	
	if (dialfeatures) {
		tris_copy_flags(&(config->features_caller), &(dialfeatures->features_caller), TRIS_FLAGS_ALL);
	}
	
	tobj->chan = newchan;
	tobj->peer = xferchan;
	tobj->bconfig = *config;
	
	if (tobj->bconfig.end_bridge_callback_data_fixup) {
		tobj->bconfig.end_bridge_callback_data_fixup(&tobj->bconfig, tobj->peer, tobj->chan);
	}
	
	/*if (newstate == TRIS_STATE_UP && tris_stream_and_wait(newchan, xfersound, ""))
		tris_log(LOG_WARNING, "Failed to play transfer sound!\n");*/
	
	if (newchan && !tris_check_hangup(newchan))
		set_channel_not_spy(newchan);
	if (xferchan && !tris_check_hangup(xferchan))
		set_channel_not_spy(xferchan);
	bridge_call_thread_launch(tobj);
	tris_log(LOG_WARNING, "In new connect function!\n");
	return 0;
}

/*! 
 * \brief Get feature and dial
 * \param caller,transferee,type,format,data,timeout,outstate,cid_num,cid_name,igncallerstate
 *
 * Request channel, set channel variables, initiate call,check if they want to disconnect
 * go into loop, check if timeout has elapsed, check if person to be transfered hung up,
 * check for answer break loop, set cdr return channel.
 *
 * \todo XXX Check - this is very similar to the code in channel.c 
 * \return always a channel
*/
static struct tris_channel *feature_dial_byrefer(struct tris_channel *caller, struct tris_channel *transferee, 
		const char *type, int format, void *data, int timeout, int *outstate, const char *cid_num, const char *cid_name, 
		int igncallerstate, const char *language, int ringing, int notifycaller, const char *caller_context,
		struct tris_bridge_config *config, const char* dst, int *holdstate)
{
	int state = 0;
	int cause = 0;
	int to;
	struct tris_channel *chan;
	struct tris_channel *monitor_chans[10];
	struct tris_channel *active_channel;
	int res = 0;
	struct timeval started;
	int x, len = 0;
	char *disconnect_code = NULL, *dialed_code = NULL;
	int is_calling = 0;
	const char* busy_peer;
	char tmpstr[256];
	int i, pos = 2, peerpos = 0, curpos = 1, chanpos = 0, lpos = -1;
	int hangupnum = 0, answernum = 0;
	char* exten = NULL;
	char xferto[256] = "";
	char sql[1024], result[1024]="";
	int id = 0, connect = 0;
	struct tris_app *the_app;

	for (i=0; i<10; i++) {
		monitor_chans[i] = NULL;
	}

	caller->referid = -1;
	monitor_chans[0] = caller;
	monitor_chans[1] = transferee;
	if (!(chan = tris_request(type, format, data, &cause, 0))) {
		tris_log(LOG_NOTICE, "Unable to request channel %s/%s\n", type, (char *)data);
		switch(cause) {
		case TRIS_CAUSE_BUSY:
			state = TRIS_CONTROL_BUSY;
			break;
		case TRIS_CAUSE_CONGESTION:
			state = TRIS_CONTROL_CONGESTION;
			break;
		}
		goto done;
	}
	
	tris_set_callerid(chan, cid_num, cid_name, cid_num);
	tris_string_field_set(chan, language, language);
	tris_channel_inherit_variables(caller, chan);	
	pbx_builtin_setvar_helper(chan, "TRANSFERERNAME", caller->name);
	chan->transferchan = 1;
	snprintf(tmpstr, sizeof(tmpstr), "%d", notifycaller);
	pbx_builtin_setvar_helper(chan, "notifycaller", tmpstr);
	chan->referid = caller->referidval;
		
	snprintf(result, sizeof(result), "Call-Info: MP,queue,1");
	the_app = pbx_findapp("SWITCHAddHeader");
	if (the_app) {
		pbx_exec(chan, the_app, result);
		snprintf(result, sizeof(result), "Subject: %s,%s,%s", transferee->cid.cid_num, transferee->exten, dst);
		tris_verbose("Subject: %s\n", result);
		pbx_exec(chan, the_app, result);
	}

	if (tris_call(chan, data, timeout)) {
		tris_log(LOG_NOTICE, "Unable to call channel %s/%s\n", type, (char *)data);
		tris_hangup(chan);
		chan = NULL;
		goto done;
	}
	
	monitor_chans[pos++] = chan;
	chanpos++;

	if (pos == 2)
		goto done;

	chan = NULL;

	/*tris_indicate(caller, TRIS_CONTROL_RINGING);*/
	/* support dialing of the featuremap disconnect code while performing an attended tranfer */
	tris_rwlock_rdlock(&features_lock);
	for (x = 0; x < FEATURES_COUNT; x++) {
		if (strcasecmp(builtin_features[x].sname, "disconnect"))
			continue;

		disconnect_code = builtin_features[x].exten;
		len = strlen(disconnect_code) + 1;
		dialed_code = alloca(len);
		memset(dialed_code, 0, len);
		break;
	}
	tris_rwlock_unlock(&features_lock);
	x = 0;
	started = tris_tvnow();
	to = timeout;

	while (!((!connect && transferee && tris_check_hangup(transferee)) || (!igncallerstate && tris_check_hangup(caller)) || 
			(hangupnum == chanpos)) && !chan && timeout) {
		struct tris_frame *f = NULL;

		active_channel = tris_waitfor_n(monitor_chans, pos, &to);

		/* see if the timeout has been violated */
		if(!answernum && tris_tvdiff_ms(tris_tvnow(), started) > timeout) {
			state = TRIS_CONTROL_UNHOLD;
			tris_log(LOG_NOTICE, "We exceeded our AT-timeout\n");
			break; /*doh! timeout*/
		}

		if (!active_channel)
			continue;

		if (caller && (active_channel != caller)) {
			curpos = 0;
			for (i=1; i<pos; i++) {
				if (monitor_chans[i] == active_channel) {
					curpos = i;
					break;
				}
			}
			
			if (!tris_strlen_zero(active_channel->call_forward)) {
				if (!(active_channel = tris_call_forward(caller, active_channel, &to, format, NULL, outstate))) {
					return NULL;
				}
				continue;
			}
			f = tris_read(active_channel);
			if (f == NULL) { /*doh! where'd he go?*/
				send_control_notify(caller, TRIS_CONTROL_NOTIFY_CALLERBYE, active_channel->referid, notifycaller);
				if (curpos > 1-connect) {
					hangupnum++;
					if (monitor_chans[curpos]) {
						if (monitor_chans[curpos] != transferee)
							tris_hangup(monitor_chans[curpos]);
						else
							tris_softhangup(monitor_chans[curpos], TRIS_SOFTHANGUP_DEV);
					}
					for (i=curpos+1; i<pos; i++)
						monitor_chans[i-1] = monitor_chans[i];
					if (pos) {
						monitor_chans[pos - 1] = NULL;
						pos--;
						if (peerpos >= pos) {
							peerpos--;
						}
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_ACCEPT, monitor_chans[peerpos]->referid, notifycaller);
					}
					if (hangupnum == chanpos) {
						state = TRIS_CONTROL_HANGUP;
						res = 0;
						break;
					}
				}
				continue;
			}
			
			if (f->frametype == TRIS_FRAME_CONTROL || f->frametype == TRIS_FRAME_DTMF || f->frametype == TRIS_FRAME_TEXT) {
				if (f->subclass == TRIS_CONTROL_RINGING) {
					state = f->subclass;
					tris_verb(3, "%s is ringing\n", active_channel->name);
					tris_indicate(caller, TRIS_CONTROL_RINGING);
					if (notifycaller % 2 == 1 && !connect) {
						if (!(*outstate))
							tris_indicate(transferee, TRIS_CONTROL_UNHOLD);
						if (!ringing)
							tris_streamfile(transferee, "conference/ringing", transferee->language);
						*outstate = TRIS_CONTROL_RINGING;
					}
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_RINGING, active_channel->referid, notifycaller);
				} else if (f->subclass == TRIS_CONTROL_PROCEEDING) {
					tris_verb(3, "%s is proceeding\n", active_channel->name);
					tris_indicate(caller, TRIS_CONTROL_PROCEEDING);
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_PROCEEDING, active_channel->referid, notifycaller);
				} else if (f->subclass == TRIS_CONTROL_PROGRESS) {
					tris_verb(3, "%s is progressing\n", active_channel->name);
					tris_indicate(caller, TRIS_CONTROL_PROGRESS);
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_PROGRESS, active_channel->referid, notifycaller);
				} else if ((f->subclass == TRIS_CONTROL_BUSY) || (f->subclass == TRIS_CONTROL_CONGESTION) ||
							(f->subclass == TRIS_CONTROL_TIMEOUT)  || (f->subclass == TRIS_CONTROL_FORBIDDEN) ||
							(f->subclass == TRIS_CONTROL_ROUTEFAIL)  || (f->subclass == TRIS_CONTROL_REJECTED) ||
							(f->subclass == TRIS_CONTROL_UNAVAILABLE) || (f->subclass == TRIS_CONTROL_OFFHOOK) ||
							(f->subclass == TRIS_CONTROL_TAKEOFFHOOK)) {
					state = f->subclass;
					if (f->subclass == TRIS_CONTROL_BUSY) {
						tris_verb(3, "%s is busy\n", active_channel->name);
						busy_peer = pbx_builtin_getvar_helper(active_channel, "Error-Info");
						if (!tris_strlen_zero(busy_peer)) {
							exten = strchr(busy_peer, ',');
							if (!tris_strlen_zero(exten))
								pbx_builtin_setvar_helper(caller, "Busy-Peer", exten+1);
						}
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_BUSY, active_channel->referid, notifycaller);
						if (notifycaller) {
							if (!tris_strlen_zero(busy_peer) && !tris_strlen_zero(exten)) {
								res = tris_play_and_wait(caller, "dial/is_used");
								if (res >= 0)
									res = tris_play_and_wait(caller, "dial/dial-exten-num-is");
								if (res >= 0)
									res = tris_say_digit_str(caller, exten+1, "", caller->language);
								if (res >= 0)
									res = tris_play_and_wait(caller, "dial/dial-is");
								res = 0;
							} else {
								tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
							}
						}
					} else if (f->subclass == TRIS_CONTROL_CONGESTION) {
						tris_verb(3, "%s is congestion\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_BUSY, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_ROUTEFAIL) {
						tris_verb(3, "%s is circuit-busy\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_CIRCUITS, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_UNAVAILABLE) {
						tris_verb(3, "%s is unavailable\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_BUSY, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-busy", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_FORBIDDEN) {
						tris_verb(3, "%s is forbidden\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_FORBIDDEN, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-forbidden", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_OFFHOOK) {
						tris_verb(3, "%s is offhook\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_OFFHOOK, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-not-found", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_TAKEOFFHOOK) {
						tris_verb(3, "%s is takeoffhook\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_TAKEOFFHOOK, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-not-registered", TRIS_DIGIT_ANY);
						}
					} else if (f->subclass == TRIS_CONTROL_TIMEOUT) {
						tris_verb(3, "%s is timeout\n", active_channel->name);
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_TIMEOUT, active_channel->referid, notifycaller);
						if (notifycaller) {
							tris_stream_and_wait(caller, "dial/pbx-no-answer", TRIS_DIGIT_ANY);
						}
					} else {
						tris_verb(3, "%s is busy\n", active_channel->name);
						tris_indicate(caller, f->subclass);
					}
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_CALLERBYE, active_channel->referid, notifycaller);
					tris_frfree(f);
					f = NULL;
					if (notifycaller % 2 == 1 && !connect) {
						if (*outstate == TRIS_CONTROL_RINGING)
							tris_stopstream(transferee);
						*outstate = TRIS_CONTROL_BUSY;
					}
					hangupnum++;
					if (curpos) {
						if (monitor_chans[curpos]) {
							if (monitor_chans[curpos] != transferee)
								tris_hangup(monitor_chans[curpos]);
							else
								tris_softhangup(monitor_chans[curpos], TRIS_SOFTHANGUP_DEV);
						}
						for (i=curpos+1; i<pos; i++)
							monitor_chans[i-1] = monitor_chans[i];
					}
					if (pos) {
						monitor_chans[pos - 1] = NULL;
						pos--;
						if (peerpos >= pos) {
							peerpos--;
						}
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_ACCEPT, monitor_chans[peerpos]->referid, notifycaller);
					}
					if (hangupnum == chanpos)
						break;
				} else if (f->subclass == TRIS_CONTROL_ANSWER) {
					/* This is what we are hoping for */
					if (!peerpos && curpos)
						peerpos = curpos;
					state = f->subclass;
					tris_frfree(f);
					f = NULL;
					if (notifycaller % 2 == 1 && !connect) {
						if (*outstate == TRIS_CONTROL_RINGING)
							tris_stopstream(transferee);
						*outstate = TRIS_CONTROL_ANSWER;
					}
					answernum++;
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_ANSWER, active_channel->referid, notifycaller);
				} else if (f->subclass >= TRIS_CONTROL_NOTIFY_PROCEEDING && f->subclass <= TRIS_CONTROL_NOTIFY_CIRCUITS) {
					busy_peer = pbx_builtin_getvar_helper(active_channel, "Busy-Peer");
					if (busy_peer)
						pbx_builtin_setvar_helper(caller, "Busy-Peer", busy_peer);
					send_control_notify(caller, f->subclass, active_channel->referid, notifycaller);
				} else if (f->subclass == TRIS_CONTROL_HOLD || f->subclass == TRIS_CONTROL_UNHOLD ||
						f->subclass == TRIS_CONTROL_VIDUPDATE || f->subclass == TRIS_CONTROL_SRCUPDATE ||
						f->subclass == -1) {
					tris_indicate(caller, f->subclass);
				} else if (f->subclass != -1 && f->subclass != TRIS_CONTROL_PROGRESS) {
					tris_log(LOG_NOTICE, "Don't know what to do about control frame: %d\n", f->subclass);
				}
				/* else who cares */
			} else if (f->frametype == TRIS_FRAME_VOICE || f->frametype == TRIS_FRAME_VIDEO) {
				if (peerpos != curpos && (peerpos || curpos != 2)) {
					tris_frfree(f);
					f = NULL;
					continue;
				}
				
				tris_write(caller, f);
				if (notifycaller % 2 == 1 && !connect) {
					if (!(*outstate))
						tris_indicate(transferee, TRIS_CONTROL_UNHOLD);
					else if (*outstate == TRIS_CONTROL_RINGING)
						tris_stopstream(transferee);
					if (is_calling != 1 && !peerpos)
						tris_write(transferee, f);
					*outstate = TRIS_CONTROL_PROGRESS;
				}
			}

		} else if (caller && (active_channel == caller)) {
			f = tris_read(caller);
			if (f == NULL) { /*doh! where'd he go?*/
				if (!igncallerstate) {
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_CALLERBYE, active_channel->referid, notifycaller);
					if (peerpos < 2-connect) {
						if (pos > 2-connect) {
							peerpos = 2-connect;
							send_control_notify(caller, TRIS_CONTROL_NOTIFY_ACCEPT, monitor_chans[peerpos]->referid, notifycaller);
						}
					}
					if (peerpos >= 2-connect) {
						if (!tris_check_hangup(monitor_chans[peerpos])) {
							chan = monitor_chans[peerpos];
							monitor_chans[peerpos] = NULL;
							for (i=2-connect; i<pos; i++) {
								if (monitor_chans[i]) {
									if (monitor_chans[i] != transferee)
										tris_hangup(monitor_chans[i]);
									else
										tris_softhangup(monitor_chans[i], TRIS_SOFTHANGUP_DEV);
									monitor_chans[i] = NULL;
								}
							}
							break;
						}
					}
					for (i=2-connect; i<pos; i++) {
						if (monitor_chans[i]) {
							if (monitor_chans[i] != transferee)
								tris_hangup(monitor_chans[i]);
							else
								tris_softhangup(monitor_chans[i], TRIS_SOFTHANGUP_DEV);
							monitor_chans[i] = NULL;
						}
					}
					state = TRIS_CONTROL_HANGUP;
					res = 0;
					break;
				}
			} else if (f->frametype == TRIS_FRAME_VOICE || f->frametype == TRIS_FRAME_VIDEO) {
				if (peerpos)
					tris_write(monitor_chans[peerpos], f);
				if (notifycaller % 2 == 1 && !connect) {
					if (is_calling == 2) {
						if (*outstate == TRIS_CONTROL_RINGING && !ringing) {
							tris_streamfile(transferee, "conference/ringing", transferee->language);
						}
					} else if (is_calling == 1) {
						if (*outstate == TRIS_CONTROL_RINGING) {
							tris_stopstream(transferee);
						}
						tris_write(transferee, f);
					}
				}
			} else if (f->frametype == TRIS_FRAME_CONTROL && f->subclass == TRIS_CONTROL_REFER) {
				if (pos == 10) {
					tris_log(LOG_WARNING, "It's maximum size\n");
					tris_frfree(f);
					f = NULL;
					tris_indicate(caller, TRIS_CONTROL_NOTIFY_OFFHOOK);
					continue;
				}
				exten = caller->referexten;
				if (!exten || tris_strlen_zero(exten)) {
					tris_frfree(f);
					f = NULL;
					tris_indicate(caller, TRIS_CONTROL_NOTIFY_OFFHOOK);
					continue;
				}
				snprintf(xferto, sizeof(xferto), "%s", exten);
				caller->referexten[0] = '\0';
				/* valid extension, res == 1 */
				if (!tris_exists_extension(caller, caller_context, xferto, 1, caller->cid.cid_num)) {
					tris_frfree(f);
					f = NULL;
					tris_indicate(caller, TRIS_CONTROL_NOTIFY_OFFHOOK);
					continue;
				}
				
				snprintf(sql, sizeof(sql), "select key_name from service_set where '%s' like concat(key_number,'%%');", xferto);
				if (tris_sql_select_query_execute) {
					tris_sql_select_query_execute(result, sql);
					if (!tris_strlen_zero(result) && strlen(result) == 12 && !strncmp(result, "bargein3conf", 12)) {
						tris_frfree(f);
						f = NULL;
						tris_indicate(caller, TRIS_CONTROL_NOTIFY_OFFHOOK);
						continue;
					}
				}
				
				/* If we are attended transfering to parking, just use builtin_parkcall instead of trying to track all of
				 * the different variables for handling this properly with a builtin_atxfer */
				if (!strcmp(xferto, tris_parking_ext())) {
					tris_frfree(f);
					f = NULL;
					tris_indicate(caller, TRIS_CONTROL_NOTIFY_OFFHOOK);
					continue;
				}
				
				i = strlen(xferto);
				if (gethostname(result, sizeof(result)) < 0)
					return NULL;
				snprintf(xferto + i, sizeof(xferto) - i, "@%s", result); /* append context */
				
				if (!(chan = tris_request(type, format, xferto, &cause, 0))) {
					tris_frfree(f);
					f = NULL;
					tris_indicate(caller, TRIS_CONTROL_NOTIFY_BUSY);
					continue;
				}
				
				tris_set_callerid(chan, cid_num, cid_name, cid_num);
				tris_string_field_set(chan, language, language);
				tris_channel_inherit_variables(caller, chan);	
				pbx_builtin_setvar_helper(chan, "TRANSFERERNAME", caller->name);
				chan->transferchan = 1;
				snprintf(tmpstr, sizeof(tmpstr), "%d", notifycaller);
				pbx_builtin_setvar_helper(chan, "notifycaller", tmpstr);
				chan->referid = caller->referidval;

				snprintf(result, sizeof(result), "Call-Info: MP,queue,1");
				the_app = pbx_findapp("SWITCHAddHeader");
				if (the_app)
					pbx_exec(chan, the_app, result);
				
				if (tris_call(chan, xferto, timeout)) {
					tris_frfree(f);
					f = NULL;
					tris_indicate(caller, TRIS_CONTROL_NOTIFY_BUSY);
					tris_hangup(chan);
					chan = NULL;
					continue;
				}
				
				monitor_chans[pos++] = chan;
				chanpos++;
				chan = NULL;
			} else if (f->frametype == TRIS_FRAME_CONTROL && f->subclass == TRIS_CONTROL_REFER_INFO) {
				id = caller->referidval;
				lpos = -1;
				if (id >= 0 && caller->referaction > 0) {
					for (i=0; i<pos; i++) {
						if (monitor_chans[i]->referid == id) {
							lpos = i;
							break;
						}
					}
				}
				if (lpos != -1) {
					if (caller->referaction == TRIS_REFER_ACTION_ACCEPT && lpos < pos) {
						if (peerpos) {
							if (monitor_chans[peerpos]->_state == TRIS_STATE_UP) {
								finishup(monitor_chans[lpos]);
								if (peerpos == 1)
									*holdstate = 0;
								else
									*holdstate = 1;
								tris_indicate(monitor_chans[peerpos], TRIS_CONTROL_HOLD);
								if (!ringing)
									tris_autoservice_start(monitor_chans[peerpos]);
							}
							if (is_calling == 1)
								is_calling = 2;
						}
						peerpos = lpos;
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_ACCEPT, id, notifycaller);
					} else if (caller->referaction == TRIS_REFER_ACTION_CONNECT && lpos >= 2 && lpos < pos && !connect) {
						if (monitor_chans[lpos]->_state == TRIS_STATE_UP) {
							if (!tris_check_hangup(transferee) && !tris_check_hangup(monitor_chans[lpos])) {
								if (feature_connect_channels(transferee, monitor_chans[lpos], caller, config) < 0) {
									tris_log(LOG_WARNING, "Can't connect channels.\n");
								}
							}
							hangupnum++;
							for (i=lpos+1; i<pos; i++)
								monitor_chans[i-1] = monitor_chans[i];
							if (pos) {
								monitor_chans[pos - 1] = NULL;
								pos--;
								if (peerpos >= pos)
									peerpos--;
							}
							for (i=2; i<pos; i++)
								monitor_chans[i-1] = monitor_chans[i];
							if (pos) {
								monitor_chans[pos - 1] = NULL;
								pos--;
								if (peerpos >= pos)
									peerpos--;
							}
							connect = 1;
							send_control_notify(caller, TRIS_CONTROL_NOTIFY_CONNECT, id, notifycaller);
							if (hangupnum == chanpos) {
								state = TRIS_CONTROL_UNHOLD;
								res = 0;
								tris_frfree(f);
								f = NULL;
								break;
							}
							send_control_notify(caller, TRIS_CONTROL_NOTIFY_ACCEPT, monitor_chans[peerpos]->referid, notifycaller);
						} else {
							state = TRIS_CONTROL_UNHOLD;
							res = 0;
							tris_frfree(f);
							f = NULL;
							if (caller && !tris_check_hangup(caller))
								tris_softhangup(caller, TRIS_SOFTHANGUP_ASYNCGOTO);
							caller = NULL;
							peerpos = lpos;
							break;
						}
					} else if (caller->referaction == TRIS_REFER_ACTION_CANCEL || caller->referaction == TRIS_REFER_ACTION_BYE) {
						/* Caller Canceled the call */
						if (caller->referaction == TRIS_REFER_ACTION_CANCEL)
							send_control_notify(caller, TRIS_CONTROL_NOTIFY_CANCEL, id, notifycaller);
						else
							send_control_notify(caller, TRIS_CONTROL_NOTIFY_BYE, id, notifycaller);
						if (lpos >= 2-connect) {
							hangupnum++;
							if (monitor_chans[lpos])
								tris_hangup(monitor_chans[lpos]);
							for (i=lpos+1; i<pos; i++)
								monitor_chans[i-1] = monitor_chans[i];
							if (pos) {
								monitor_chans[pos - 1] = NULL;
								pos--;
								if (peerpos >= pos) {
									peerpos--;
								}
								send_control_notify(caller, TRIS_CONTROL_NOTIFY_ACCEPT, monitor_chans[peerpos]->referid, notifycaller);
							}
							if (hangupnum == chanpos) {
								state = TRIS_CONTROL_UNHOLD;
								res = 0;
								tris_frfree(f);
								f = NULL;
								break;
							}
						}
					}
				}
				id = 0;
				caller->referaction = 0;
			} else if (f->frametype == TRIS_FRAME_CONTROL && f->subclass == TRIS_CONTROL_HANGUP) {
				if (!igncallerstate) {
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_CALLERBYE, active_channel->referid, notifycaller);
					if (peerpos < 2-connect) {
						if (pos > 2-connect) {
							peerpos = 2-connect;
							send_control_notify(caller, TRIS_CONTROL_NOTIFY_ACCEPT, monitor_chans[peerpos]->referid, notifycaller);
						}
					}
					if (peerpos >= 2-connect) {
						if (!tris_check_hangup(monitor_chans[peerpos])) {
							chan = monitor_chans[peerpos];
							monitor_chans[peerpos] = NULL;
							for (i=2-connect; i<pos; i++) {
								if (monitor_chans[i]) {
									if (monitor_chans[i] != transferee)
										tris_hangup(monitor_chans[i]);
									else
										tris_softhangup(monitor_chans[i], TRIS_SOFTHANGUP_DEV);
									monitor_chans[i] = NULL;
								}
							}
							break;
						}
					}
					for (i=2-connect; i<pos; i++) {
						if (monitor_chans[i]) {
							if (monitor_chans[i] != transferee)
								tris_hangup(monitor_chans[i]);
							else
								tris_softhangup(monitor_chans[i], TRIS_SOFTHANGUP_DEV);
							monitor_chans[i] = NULL;
						}
					}
					state = TRIS_CONTROL_HANGUP;
					res = 0;
					break;
				}
			}
		}
		if (f)
			tris_frfree(f);
	} /* end while */

done:
	if (caller && !tris_check_hangup(caller))
		tris_indicate(caller, -1);
	if (transferee && !tris_check_hangup(transferee) && !chan && !connect) {
		if (peerpos < 2-connect) {
			peerpos = pos - 1;
		}
		if (peerpos >= 2-connect && monitor_chans[peerpos]) {
			chan = monitor_chans[peerpos];
			monitor_chans[peerpos] = NULL;
		}
	}
	if (chan || !caller || tris_check_hangup(caller)) {
		state = TRIS_CONTROL_ANSWER;
		res = 0;
		for (i=2-connect; i<10; i++) {
			if (monitor_chans[i]) {
				if (caller && !tris_check_hangup(caller)) {
					send_control_notify(caller, TRIS_CONTROL_NOTIFY_BYE, monitor_chans[i]->referid, notifycaller);
				}
				if (monitor_chans[i] != transferee)
					tris_hangup(monitor_chans[i]);
				else
					tris_softhangup(monitor_chans[i], TRIS_SOFTHANGUP_DEV);
			}
		}
	} else {
		for (i=2; i<10; i++) {
			if (monitor_chans[i]) {
				if (chan) {
					if (caller && !tris_check_hangup(caller)) {
						send_control_notify(caller, TRIS_CONTROL_NOTIFY_BYE, monitor_chans[i]->referid, notifycaller);
					}
					if (monitor_chans[i] != transferee)
						tris_hangup(monitor_chans[i]);
					else
						tris_softhangup(monitor_chans[i], TRIS_SOFTHANGUP_DEV);
				} else {
					chan = monitor_chans[i];
				}
			}
		}
		/*if (caller && !tris_check_hangup(caller)) {
			tris_indicate(caller, TRIS_CONTROL_NOTIFY_BUSY);
		}*/
		res = -1;
	}

	if (notifycaller % 2 == 1 && !connect) {	
		if (!chan && is_calling == 1 && *outstate == TRIS_CONTROL_RINGING && !tris_check_hangup(transferee) && !ringing)
			tris_streamfile(transferee, "conference/ringing", transferee->language);
	}

	if (outstate)
		*outstate = state;

	if (connect) {
		if (chan) {
			if (caller && !tris_check_hangup(caller)) {
				send_control_notify(caller, TRIS_CONTROL_NOTIFY_BYE, chan->referid, notifycaller);
			}
			tris_hangup(chan);
		}
		return (struct tris_channel*)-1;
	}
	return chan;
}

/*!
 * \brief return the first unlocked cdr in a possible chain
*/
static struct tris_cdr *pick_unlocked_cdr(struct tris_cdr *cdr)
{
	struct tris_cdr *cdr_orig = cdr;
	while (cdr) {
		if (!tris_test_flag(cdr,TRIS_CDR_FLAG_LOCKED))
			return cdr;
		cdr = cdr->next;
	}
	return cdr_orig; /* everybody LOCKED or some other weirdness, like a NULL */
}

static void set_bridge_features_on_config(struct tris_bridge_config *config, const char *features)
{
	const char *feature;

	if (tris_strlen_zero(features)) {
		return;
	}

	for (feature = features; *feature; feature++) {
		switch (*feature) {
		case 'T' :
		case 't' :
			tris_set_flag(&(config->features_caller), TRIS_FEATURE_REDIRECT);
			break;
		case 'K' :
		case 'k' :
			tris_set_flag(&(config->features_caller), TRIS_FEATURE_PARKCALL);
			break;
		case 'H' :
		case 'h' :
			tris_set_flag(&(config->features_caller), TRIS_FEATURE_DISCONNECT);
			break;
		case 'W' :
		case 'w' :
			tris_set_flag(&(config->features_caller), TRIS_FEATURE_AUTOMON);
			break;
		default :
			tris_log(LOG_WARNING, "Skipping unknown feature code '%c'\n", *feature);
		}
	}
}

static void add_features_datastores(struct tris_channel *caller, struct tris_channel *callee, struct tris_bridge_config *config)
{
	struct tris_datastore *ds_callee_features = NULL, *ds_caller_features = NULL;
	struct tris_dial_features *callee_features = NULL, *caller_features = NULL;

	tris_channel_lock(caller);
	ds_caller_features = tris_channel_datastore_find(caller, &dial_features_info, NULL);
	tris_channel_unlock(caller);
	if (!ds_caller_features) {
		if (!(ds_caller_features = tris_datastore_alloc(&dial_features_info, NULL))) {
			tris_log(LOG_WARNING, "Unable to create channel datastore for caller features. Aborting!\n");
			return;
		}
		if (!(caller_features = tris_calloc(1, sizeof(*caller_features)))) {
			tris_log(LOG_WARNING, "Unable to allocate memory for callee feature flags. Aborting!\n");
			tris_datastore_free(ds_caller_features);
			return;
		}
		ds_caller_features->inheritance = DATASTORE_INHERIT_FOREVER;
		caller_features->is_caller = 1;
		tris_copy_flags(&(caller_features->features_callee), &(config->features_callee), TRIS_FLAGS_ALL);
		tris_copy_flags(&(caller_features->features_caller), &(config->features_caller), TRIS_FLAGS_ALL);
		ds_caller_features->data = caller_features;
		tris_channel_lock(caller);
		tris_channel_datastore_add(caller, ds_caller_features);
		tris_channel_unlock(caller);
	} else {
		/* If we don't return here, then when we do a builtin_atxfer we will copy the disconnect
		 * flags over from the atxfer to the caller */
		return;
	}

	tris_channel_lock(callee);
	ds_callee_features = tris_channel_datastore_find(callee, &dial_features_info, NULL);
	tris_channel_unlock(callee);
	if (!ds_callee_features) {
		if (!(ds_callee_features = tris_datastore_alloc(&dial_features_info, NULL))) {
			tris_log(LOG_WARNING, "Unable to create channel datastore for callee features. Aborting!\n");
			return;
		}
		if (!(callee_features = tris_calloc(1, sizeof(*callee_features)))) {
			tris_log(LOG_WARNING, "Unable to allocate memory for callee feature flags. Aborting!\n");
			tris_datastore_free(ds_callee_features);
			return;
		}
		ds_callee_features->inheritance = DATASTORE_INHERIT_FOREVER;
		callee_features->is_caller = 0;
		tris_copy_flags(&(callee_features->features_callee), &(config->features_caller), TRIS_FLAGS_ALL);
		tris_copy_flags(&(callee_features->features_caller), &(config->features_callee), TRIS_FLAGS_ALL);
		ds_callee_features->data = callee_features;
		tris_channel_lock(callee);
		tris_channel_datastore_add(callee, ds_callee_features);
		tris_channel_unlock(callee);
	}

	return;
}
/*
static int get_monitor_fn(struct tris_channel * chan, struct tris_channel * peer, char *s, int len)
{
	struct tris_tm tm;
	struct timeval t = tris_tvnow();
	
	tris_localtime(&t, &tm, NULL);
	return snprintf(s, len, "%04d%02d%02d-%02d%02d%02d-%s-%s", 
		tm.tm_year+1900, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, 
		tm.tm_sec, S_OR(chan->cid.cid_num, ""),S_OR(peer->cid.cid_num, ""));
}
*/
	
static void clear_dialed_interfaces(struct tris_channel *chan)
{
	struct tris_datastore *di_datastore;

	tris_channel_lock(chan);
	if ((di_datastore = tris_channel_datastore_find(chan, &dialed_interface_info, NULL))) {
		if (option_debug) {
			tris_log(LOG_DEBUG, "Removing dialed interfaces datastore on %s since we're bridging\n", chan->name);
		}
		if (!tris_channel_datastore_remove(chan, di_datastore)) {
			tris_datastore_free(di_datastore);
		}
	}
	tris_channel_unlock(chan);
}

/*!
 * \brief bridge the call and set CDR
 * \param chan,peer,config
 * 
 * Set start time, check for two channels,check if monitor on
 * check for feature activation, create new CDR
 * \retval res on success.
 * \retval -1 on failure to bridge.
*/
int tris_bridge_call(struct tris_channel *chan,struct tris_channel *peer,struct tris_bridge_config *config)
{
	/* Copy voice back and forth between the two channels.  Give the peer
	   the ability to transfer calls with '#<extension' syntax. */
	struct tris_frame *f;
	struct tris_channel *who;
	char chan_featurecode[FEATURE_MAX_LEN + 1]="";
	char peer_featurecode[FEATURE_MAX_LEN + 1]="";
	char orig_channame[TRIS_MAX_EXTENSION];
	char orig_peername[TRIS_MAX_EXTENSION];
	int res;
	int diff;
	int hasfeatures=0;
	int hadfeatures=0;
	int autoloopflag;
	struct tris_option_header *aoh;
	struct tris_bridge_config backup_config;
	struct tris_cdr *bridge_cdr = NULL;
	struct tris_cdr *orig_peer_cdr = NULL;
	struct tris_cdr *chan_cdr = chan->cdr; /* the proper chan cdr, if there are forked cdrs */
	struct tris_cdr *peer_cdr = peer->cdr; /* the proper chan cdr, if there are forked cdrs */
	struct tris_cdr *new_chan_cdr = NULL; /* the proper chan cdr, if there are forked cdrs */
	struct tris_cdr *new_peer_cdr = NULL; /* the proper chan cdr, if there are forked cdrs */
	char *featurecode;
	int sense;
	int referres = TRIS_FEATURE_RETURN_SUCCESS;
	int transfer = 0, sendnotify = 0;
	const char *notifychan = 0;

	notifychan = pbx_builtin_getvar_helper(chan, "notifycaller");
	if (!tris_strlen_zero(notifychan)) {
		transfer = atoi(notifychan);
		switch (transfer) {
			case 0:
				sendnotify = 1;
				break;
			case 1:
				sendnotify = 0;
				break;
			case 2:
				sendnotify = 0;
				break;
			case 3:
				sendnotify = 1;
				break;
			case 4:
				sendnotify = 1;
				break;
		}
	}
	transfer = peer->transferchan;
	
	memset(&backup_config, 0, sizeof(backup_config));

	config->start_time = tris_tvnow();

	if (chan && peer) {
		pbx_builtin_setvar_helper(chan, "BRIDGEPEER", peer->name);
		pbx_builtin_setvar_helper(peer, "BRIDGEPEER", chan->name);
	} else if (chan) {
		pbx_builtin_setvar_helper(chan, "BLINDTRANSFER", NULL);
	}

	set_bridge_features_on_config(config, pbx_builtin_getvar_helper(chan, "BRIDGE_FEATURES"));
	add_features_datastores(chan, peer, config);

	/* This is an interesting case.  One example is if a ringing channel gets redirected to
	 * an extension that picks up a parked call.  This will make sure that the call taken
	 * out of parking gets told that the channel it just got bridged to is still ringing. */
	if (chan->_state == TRIS_STATE_RINGING && peer->visible_indication != TRIS_CONTROL_RINGING) {
		tris_indicate(peer, TRIS_CONTROL_RINGING);
	}

	if (monitor_ok) {
		const char *monitor_exec;
		struct tris_channel *src = NULL;
		if (!monitor_app) { 
			if (!(monitor_app = pbx_findapp("Monitor")))
				monitor_ok=0;
		}
		if ((monitor_exec = pbx_builtin_getvar_helper(chan, "AUTO_MONITOR"))) 
			src = chan;
		else if ((monitor_exec = pbx_builtin_getvar_helper(peer, "AUTO_MONITOR")))
			src = peer;
		if (monitor_app && src) {
			char *tmp = tris_strdupa(monitor_exec);
			pbx_exec(src, monitor_app, tmp);
		}
	}

	set_config_flags(chan, peer, config);
	config->firstpass = 1;

	/* Answer if need be */
	if (chan->_state != TRIS_STATE_UP) {
		if (tris_raw_answer(chan, 1)) {
			return -1;
		}
	}

	tris_copy_string(orig_channame,chan->name,sizeof(orig_channame));
	tris_copy_string(orig_peername,peer->name,sizeof(orig_peername));
	orig_peer_cdr = peer_cdr;
	
	if (!chan_cdr || (chan_cdr && !tris_test_flag(chan_cdr, TRIS_CDR_FLAG_POST_DISABLED))) {
		
		if (chan_cdr) {
			tris_set_flag(chan_cdr, TRIS_CDR_FLAG_MAIN);
			tris_cdr_update(chan);
			bridge_cdr = tris_cdr_dup(chan_cdr);
			/* rip any forked CDR's off of the chan_cdr and attach
			 * them to the bridge_cdr instead */
			bridge_cdr->next = chan_cdr->next;
			chan_cdr->next = NULL;
			tris_copy_string(bridge_cdr->lastapp, S_OR(chan->appl, ""), sizeof(bridge_cdr->lastapp));
			tris_copy_string(bridge_cdr->lastdata, S_OR(chan->data, ""), sizeof(bridge_cdr->lastdata));
			if (peer_cdr && !tris_strlen_zero(peer_cdr->userfield)) {
				tris_copy_string(bridge_cdr->userfield, peer_cdr->userfield, sizeof(bridge_cdr->userfield));
			}
			tris_cdr_setaccount(peer, chan->accountcode);

		} else {
			/* better yet, in a xfer situation, find out why the chan cdr got zapped (pun unintentional) */
			bridge_cdr = tris_cdr_alloc(); /* this should be really, really rare/impossible? */
			tris_copy_string(bridge_cdr->channel, chan->name, sizeof(bridge_cdr->channel));
			tris_copy_string(bridge_cdr->dstchannel, peer->name, sizeof(bridge_cdr->dstchannel));
			tris_copy_string(bridge_cdr->uniqueid, chan->uniqueid, sizeof(bridge_cdr->uniqueid));
			tris_copy_string(bridge_cdr->lastapp, S_OR(chan->appl, ""), sizeof(bridge_cdr->lastapp));
			tris_copy_string(bridge_cdr->lastdata, S_OR(chan->data, ""), sizeof(bridge_cdr->lastdata));
			tris_cdr_setcid(bridge_cdr, chan);
			bridge_cdr->disposition = (chan->_state == TRIS_STATE_UP) ?  TRIS_CDR_ANSWERED : TRIS_CDR_NULL;
			bridge_cdr->amaflags = chan->amaflags ? chan->amaflags :  tris_default_amaflags;
			tris_copy_string(bridge_cdr->accountcode, chan->accountcode, sizeof(bridge_cdr->accountcode));
			/* Destination information */
			tris_copy_string(bridge_cdr->dst, chan->exten, sizeof(bridge_cdr->dst));
			tris_copy_string(bridge_cdr->dcontext, chan->context, sizeof(bridge_cdr->dcontext));
			if (peer_cdr) {
				bridge_cdr->start = peer_cdr->start;
				tris_copy_string(bridge_cdr->userfield, peer_cdr->userfield, sizeof(bridge_cdr->userfield));
			} else {
				tris_cdr_start(bridge_cdr);
			}
		}
		tris_debug(4,"bridge answer set, chan answer set\n");
		/* peer_cdr->answer will be set when a macro runs on the peer;
		   in that case, the bridge answer will be delayed while the
		   macro plays on the peer channel. The peer answered the call
		   before the macro started playing. To the phone system,
		   this is billable time for the call, even tho the caller
		   hears nothing but ringing while the macro does its thing. */

		/* Another case where the peer cdr's time will be set, is when
		   A self-parks by pickup up phone and dialing 700, then B
		   picks up A by dialing its parking slot; there may be more 
		   practical paths that get the same result, tho... in which
		   case you get the previous answer time from the Park... which
		   is before the bridge's start time, so I added in the 
		   tvcmp check to the if below */

		if (peer_cdr && !tris_tvzero(peer_cdr->answer) && tris_tvcmp(peer_cdr->answer, bridge_cdr->start) >= 0) {
			tris_cdr_setanswer(bridge_cdr, peer_cdr->answer);
			tris_cdr_setdisposition(bridge_cdr, peer_cdr->disposition);
			if (chan_cdr) {
				tris_cdr_setanswer(chan_cdr, peer_cdr->answer);
				tris_cdr_setdisposition(chan_cdr, peer_cdr->disposition);
			}
		} else {
			tris_cdr_answer(bridge_cdr);
			if (chan_cdr) {
				tris_cdr_answer(chan_cdr); /* for the sake of cli status checks */
			}
		}
		if (tris_test_flag(chan,TRIS_FLAG_BRIDGE_HANGUP_DONT) && (chan_cdr || peer_cdr)) {
			if (chan_cdr) {
				tris_set_flag(chan_cdr, TRIS_CDR_FLAG_BRIDGED);
			}
			if (peer_cdr) {
				tris_set_flag(peer_cdr, TRIS_CDR_FLAG_BRIDGED);
			}
		}
	}

	/* If we are bridging a call, stop worrying about forwarding loops. We presume that if
	 * a call is being bridged, that the humans in charge know what they're doing. If they
	 * don't, well, what can we do about that? */
	clear_dialed_interfaces(chan);
	clear_dialed_interfaces(peer);

	for (;;) {
		struct tris_channel *other;	/* used later */
	
		res = tris_channel_bridge(chan, peer, config, &f, &who);
		
		/* When frame is not set, we are probably involved in a situation
		   where we've timed out.
		   When frame is set, we'll come this code twice; once for DTMF_BEGIN
		   and also for DTMF_END. If we flow into the following 'if' for both, then 
		   our wait times are cut in half, as both will subtract from the
		   feature_timer. Not good!
		*/
		if (config->feature_timer && (!f || f->frametype == TRIS_FRAME_DTMF_END)) {
			/* Update time limit for next pass */
			diff = tris_tvdiff_ms(tris_tvnow(), config->start_time);
			if (res == TRIS_BRIDGE_RETRY) {
				/* The feature fully timed out but has not been updated. Skip
				 * the potential round error from the diff calculation and
				 * explicitly set to expired. */
				config->feature_timer = -1;
			} else {
				config->feature_timer -= diff;
			}

			if (hasfeatures) {
				/* Running on backup config, meaning a feature might be being
				   activated, but that's no excuse to keep things going 
				   indefinitely! */
				if (backup_config.feature_timer && ((backup_config.feature_timer -= diff) <= 0)) {
					tris_debug(1, "Timed out, realtime this time!\n");
					config->feature_timer = 0;
					who = chan;
					if (f)
						tris_frfree(f);
					f = NULL;
					res = 0;
				} else if (config->feature_timer <= 0) {
					/* Not *really* out of time, just out of time for
					   digits to come in for features. */
					tris_debug(1, "Timed out for feature!\n");
					if (!tris_strlen_zero(peer_featurecode)) {
						tris_dtmf_stream(chan, peer, peer_featurecode, 0, 0);
						memset(peer_featurecode, 0, sizeof(peer_featurecode));
					}
					if (!tris_strlen_zero(chan_featurecode)) {
						tris_dtmf_stream(peer, chan, chan_featurecode, 0, 0);
						memset(chan_featurecode, 0, sizeof(chan_featurecode));
					}
					if (f)
						tris_frfree(f);
					hasfeatures = !tris_strlen_zero(chan_featurecode) || !tris_strlen_zero(peer_featurecode);
					if (!hasfeatures) {
						/* Restore original (possibly time modified) bridge config */
						memcpy(config, &backup_config, sizeof(struct tris_bridge_config));
						memset(&backup_config, 0, sizeof(backup_config));
					}
					hadfeatures = hasfeatures;
					/* Continue as we were */
					continue;
				} else if (!f) {
					/* The bridge returned without a frame and there is a feature in progress.
					 * However, we don't think the feature has quite yet timed out, so just
					 * go back into the bridge. */
					continue;
 				}
			} else {
				if (config->feature_timer <=0) {
					/* We ran out of time */
					config->feature_timer = 0;
					who = chan;
					if (f)
						tris_frfree(f);
					f = NULL;
					res = 0;
				}
			}
		}
		if (res < 0) {
			if (!tris_test_flag(chan, TRIS_FLAG_ZOMBIE) && !tris_test_flag(peer, TRIS_FLAG_ZOMBIE) && !tris_check_hangup(chan) && !tris_check_hangup(peer))
				tris_log(LOG_WARNING, "Bridge failed on channels %s and %s\n", chan->name, peer->name);
			goto before_you_go;
		}
		
		if (transfer && sendnotify && f && f->frametype == TRIS_FRAME_CONTROL && who == peer) {
			switch(f->subclass) {
			case TRIS_CONTROL_ANSWER:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_ANSWER, peer->referid, 0);
				break;
			case TRIS_CONTROL_BUSY:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_BUSY, peer->referid, 0);
				break;
			case TRIS_CONTROL_CONGESTION:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_BUSY, peer->referid, 0);
				break;
			case TRIS_CONTROL_ROUTEFAIL:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_CIRCUITS, peer->referid, 0);
				break;
			case TRIS_CONTROL_UNAVAILABLE:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_BUSY, peer->referid, 0);
				break;
			case TRIS_CONTROL_FORBIDDEN:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_FORBIDDEN, peer->referid, 0);
				break;
			case TRIS_CONTROL_OFFHOOK:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_OFFHOOK, peer->referid, 0);
				break;
			case TRIS_CONTROL_TAKEOFFHOOK:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_TAKEOFFHOOK, peer->referid, 0);
				break;
			case TRIS_CONTROL_TIMEOUT:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_TIMEOUT, peer->referid, 0);
				break;
			case TRIS_CONTROL_RINGING:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_RINGING, peer->referid, 0);
				break;
			case TRIS_CONTROL_PROGRESS:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_PROGRESS, peer->referid, 0);
				break;
			case TRIS_CONTROL_PROCEEDING:
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_PROCEEDING, peer->referid, 0);
				break;
			}
		}
		if (!f || (f->frametype == TRIS_FRAME_CONTROL &&
				(f->subclass == TRIS_CONTROL_HANGUP || f->subclass == TRIS_CONTROL_BUSY || 
					f->subclass == TRIS_CONTROL_CONGESTION || f->subclass == TRIS_CONTROL_TIMEOUT
					|| f->subclass == TRIS_CONTROL_FORBIDDEN || f->subclass == TRIS_CONTROL_ROUTEFAIL
					|| f->subclass == TRIS_CONTROL_REJECTED || f->subclass == TRIS_CONTROL_UNAVAILABLE))) {
			res = -1;
			if (transfer && sendnotify && who == peer) {
				send_control_notify(chan, TRIS_CONTROL_NOTIFY_CALLERBYE, peer->referid, 0);
			}
			break;
		}
		/* many things should be sent to the 'other' channel */
		other = (who == chan) ? peer : chan;
		if (f->frametype == TRIS_FRAME_CONTROL && f->subclass >= TRIS_CONTROL_NOTIFY_PROCEEDING &&
				f->subclass <= TRIS_CONTROL_NOTIFY_CALLEEBYE) {
			send_control_notify(other, f->subclass, who->referid, 0);
		}
		if (f->frametype == TRIS_FRAME_CONTROL) {
			switch (f->subclass) {
			case TRIS_CONTROL_RINGING:
			case TRIS_CONTROL_FLASH:
			case -1:
				tris_indicate(other, f->subclass);
				break;
			case TRIS_CONTROL_HOLD:
			case TRIS_CONTROL_UNHOLD:
				tris_indicate_data(other, f->subclass, f->data.ptr, f->datalen);
				break;
			case TRIS_CONTROL_OPTION:
				aoh = f->data.ptr;
				/* Forward option Requests */
				if (aoh && aoh->flag == TRIS_OPTION_FLAG_REQUEST) {
					tris_channel_setoption(other, ntohs(aoh->option), aoh->data, 
						f->datalen - sizeof(struct tris_option_header), 0);
				}
				break;
			case TRIS_CONTROL_REFER:
				if (who == chan) {
					sense = FEATURE_SENSE_CHAN;
					featurecode = chan_featurecode;
				} else	{
					sense = FEATURE_SENSE_PEER;
					featurecode = peer_featurecode;
				}
				/*! append the event to featurecode. we rely on the string being zero-filled, and
				 * not overflowing it. 
				 * \todo XXX how do we guarantee the latter ?
				 */
				featurecode[strlen(featurecode)] = f->subclass;
				referres = builtin_handle_refer(chan, peer, config, featurecode, sense, NULL);
				break;
			}
			if (referres < TRIS_FEATURE_RETURN_PASSDIGITS)
				break;
		} else if (f->frametype == TRIS_FRAME_DTMF_BEGIN) {
			/* eat it */
		} else if (f->frametype == TRIS_FRAME_DTMF) {
			hadfeatures = hasfeatures;
			/* This cannot overrun because the longest feature is one shorter than our buffer */
			if (who == chan) {
				sense = FEATURE_SENSE_CHAN;
				featurecode = chan_featurecode;
			} else  {
				sense = FEATURE_SENSE_PEER;
				featurecode = peer_featurecode;
			}
			/*! append the event to featurecode. we rely on the string being zero-filled, and
			 * not overflowing it. 
			 * \todo XXX how do we guarantee the latter ?
			 */
			featurecode[strlen(featurecode)] = f->subclass;
			/* Get rid of the frame before we start doing "stuff" with the channels */
			tris_frfree(f);
			f = NULL;
			config->feature_timer = backup_config.feature_timer;
			res = feature_interpret(chan, peer, config, featurecode, sense);
			switch(res) {
			case TRIS_FEATURE_RETURN_PASSDIGITS:
				tris_dtmf_stream(other, who, featurecode, 0, 0);
				/* Fall through */
			case TRIS_FEATURE_RETURN_SUCCESS:
				memset(featurecode, 0, sizeof(chan_featurecode));
				break;
			}
			if (res >= TRIS_FEATURE_RETURN_PASSDIGITS) {
				res = 0;
			} else 
				break;
			hasfeatures = !tris_strlen_zero(chan_featurecode) || !tris_strlen_zero(peer_featurecode);
			if (hadfeatures && !hasfeatures) {
				/* Restore backup */
				memcpy(config, &backup_config, sizeof(struct tris_bridge_config));
				memset(&backup_config, 0, sizeof(struct tris_bridge_config));
			} else if (hasfeatures) {
				if (!hadfeatures) {
					/* Backup configuration */
					memcpy(&backup_config, config, sizeof(struct tris_bridge_config));
					/* Setup temporary config options */
					config->play_warning = 0;
					tris_clear_flag(&(config->features_caller), TRIS_FEATURE_PLAY_WARNING);
					tris_clear_flag(&(config->features_callee), TRIS_FEATURE_PLAY_WARNING);
					config->warning_freq = 0;
					config->warning_sound = NULL;
					config->end_sound = NULL;
					config->start_sound = NULL;
					config->firstpass = 0;
				}
				config->start_time = tris_tvnow();
				config->feature_timer = featuredigittimeout;
				tris_debug(1, "Set time limit to %ld\n", config->feature_timer);
			}
		}
		if (f)
			tris_frfree(f);

	}
   before_you_go:

	if (tris_test_flag(chan,TRIS_FLAG_BRIDGE_HANGUP_DONT)) {
		tris_clear_flag(chan,TRIS_FLAG_BRIDGE_HANGUP_DONT); /* its job is done */
		if (bridge_cdr) {
			tris_cdr_discard(bridge_cdr);
			/* QUESTION: should we copy bridge_cdr fields to the peer before we throw it away? */
		}
		return res; /* if we shouldn't do the h-exten, we shouldn't do the bridge cdr, either! */
	}

	if (config->end_bridge_callback) {
		config->end_bridge_callback(config->end_bridge_callback_data);
	}

	/* run the hangup exten on the chan object IFF it was NOT involved in a parking situation 
	 * if it were, then chan belongs to a different thread now, and might have been hung up long
     * ago.
	 */
	#if 0
	if (!tris_test_flag(&(config->features_caller),TRIS_FEATURE_NO_H_EXTEN) &&
		tris_exists_extension(chan, chan->context, "h", 1, chan->cid.cid_num)) {
		struct tris_cdr *swapper = NULL;
		char savelastapp[TRIS_MAX_EXTENSION];
		char savelastdata[TRIS_MAX_EXTENSION];
		char save_exten[TRIS_MAX_EXTENSION];
		int  save_prio;
		int  found = 0;	/* set if we find at least one match */
		int  spawn_error = 0;
		
		autoloopflag = tris_test_flag(chan, TRIS_FLAG_IN_AUTOLOOP);
		tris_set_flag(chan, TRIS_FLAG_IN_AUTOLOOP);
		if (bridge_cdr && tris_opt_end_cdr_before_h_exten) {
			tris_cdr_end(bridge_cdr);
		}
		/* swap the bridge cdr and the chan cdr for a moment, and let the endbridge
		   dialplan code operate on it */
		tris_channel_lock(chan);
		if (bridge_cdr) {
			swapper = chan->cdr;
			tris_copy_string(savelastapp, bridge_cdr->lastapp, sizeof(bridge_cdr->lastapp));
			tris_copy_string(savelastdata, bridge_cdr->lastdata, sizeof(bridge_cdr->lastdata));
			chan->cdr = bridge_cdr;
		}
		tris_copy_string(save_exten, chan->exten, sizeof(save_exten));
		save_prio = chan->priority;
		tris_copy_string(chan->exten, "h", sizeof(chan->exten));
		chan->priority = 1;
		tris_channel_unlock(chan);
		while ((spawn_error = tris_spawn_extension(chan, chan->context, chan->exten, chan->priority, chan->cid.cid_num, &found, 1)) == 0) {
			chan->priority++;
		}
		if (spawn_error && (!tris_exists_extension(chan, chan->context, chan->exten, chan->priority, chan->cid.cid_num) || tris_check_hangup(chan))) {
			/* if the extension doesn't exist or a hangup occurred, this isn't really a spawn error */
			spawn_error = 0;
		}
		if (found && spawn_error) {
			/* Something bad happened, or a hangup has been requested. */
			tris_debug(1, "Spawn extension (%s,%s,%d) exited non-zero on '%s'\n", chan->context, chan->exten, chan->priority, chan->name);
			tris_verb(2, "Spawn extension (%s, %s, %d) exited non-zero on '%s'\n", chan->context, chan->exten, chan->priority, chan->name);
		}
		/* swap it back */
		tris_channel_lock(chan);
		tris_copy_string(chan->exten, save_exten, sizeof(chan->exten));
		chan->priority = save_prio;
		if (bridge_cdr) {
			if (chan->cdr == bridge_cdr) {
				chan->cdr = swapper;
			} else {
				bridge_cdr = NULL;
			}
		}
		if (!spawn_error) {
			tris_set_flag(chan, TRIS_FLAG_BRIDGE_HANGUP_RUN);
		}
		tris_channel_unlock(chan);
		/* protect the lastapp/lastdata against the effects of the hangup/dialplan code */
		if (bridge_cdr) {
			tris_copy_string(bridge_cdr->lastapp, savelastapp, sizeof(bridge_cdr->lastapp));
			tris_copy_string(bridge_cdr->lastdata, savelastdata, sizeof(bridge_cdr->lastdata));
		}
		tris_set2_flag(chan, autoloopflag, TRIS_FLAG_IN_AUTOLOOP);
	}
	#endif
	
	/* obey the NoCDR() wishes. -- move the DISABLED flag to the bridge CDR if it was set on the channel during the bridge... */
	new_chan_cdr = pick_unlocked_cdr(chan->cdr); /* the proper chan cdr, if there are forked cdrs */
	if (bridge_cdr && new_chan_cdr && tris_test_flag(new_chan_cdr, TRIS_CDR_FLAG_POST_DISABLED))
		tris_set_flag(bridge_cdr, TRIS_CDR_FLAG_POST_DISABLED);

	/* we can post the bridge CDR at this point */
	if (bridge_cdr) {
		tris_cdr_end(bridge_cdr);
		tris_cdr_detach(bridge_cdr);
	}
	
	/* do a specialized reset on the beginning channel
	   CDR's, if they still exist, so as not to mess up
	   issues in future bridges;
	   
	   Here are the rules of the game:
	   1. The chan and peer channel pointers will not change
	      during the life of the bridge.
	   2. But, in transfers, the channel names will change.
	      between the time the bridge is started, and the
	      time the channel ends. 
	      Usually, when a channel changes names, it will
	      also change CDR pointers.
	   3. Usually, only one of the two channels (chan or peer)
	      will change names.
	   4. Usually, if a channel changes names during a bridge,
	      it is because of a transfer. Usually, in these situations,
	      it is normal to see 2 bridges running simultaneously, and
	      it is not unusual to see the two channels that change
	      swapped between bridges.
	   5. After a bridge occurs, we have 2 or 3 channels' CDRs
	      to attend to; if the chan or peer changed names,
	      we have the before and after attached CDR's.
	*/
	
	if (new_chan_cdr) {
		struct tris_channel *chan_ptr = NULL;
 
 		if (strcasecmp(orig_channame, chan->name) != 0) { 
 			/* old channel */
 			chan_ptr = tris_get_channel_by_name_locked(orig_channame);
 			if (chan_ptr) {
 				if (!tris_bridged_channel(chan_ptr)) {
 					struct tris_cdr *cur;
 					for (cur = chan_ptr->cdr; cur; cur = cur->next) {
 						if (cur == chan_cdr) {
 							break;
 						}
 					}
 					if (cur)
 						tris_cdr_specialized_reset(chan_cdr,0);
 				}
 				tris_channel_unlock(chan_ptr);
 			}
 			/* new channel */
 			tris_cdr_specialized_reset(new_chan_cdr,0);
 		} else {
 			tris_cdr_specialized_reset(chan->cdr,0); /* nothing changed, reset the chan_cdr  */
 		}
	}
	
	{
		struct tris_channel *chan_ptr = NULL;
		new_peer_cdr = pick_unlocked_cdr(peer->cdr); /* the proper chan cdr, if there are forked cdrs */
		if (new_chan_cdr && tris_test_flag(new_chan_cdr, TRIS_CDR_FLAG_POST_DISABLED) && new_peer_cdr && !tris_test_flag(new_peer_cdr, TRIS_CDR_FLAG_POST_DISABLED))
			tris_set_flag(new_peer_cdr, TRIS_CDR_FLAG_POST_DISABLED); /* DISABLED is viral-- it will propagate across a bridge */
		if (strcasecmp(orig_peername, peer->name) != 0) { 
			/* old channel */
			chan_ptr = tris_get_channel_by_name_locked(orig_peername);
			if (chan_ptr) {
				if (!tris_bridged_channel(chan_ptr)) {
					struct tris_cdr *cur;
					for (cur = chan_ptr->cdr; cur; cur = cur->next) {
						if (cur == peer_cdr) {
							break;
						}
					}
					if (cur)
						tris_cdr_specialized_reset(peer_cdr,0);
				}
				tris_channel_unlock(chan_ptr);
			}
			/* new channel */
			if (new_peer_cdr) {
				tris_cdr_specialized_reset(new_peer_cdr, 0);
			}
		} else {
			tris_cdr_specialized_reset(peer->cdr,0); /* nothing changed, reset the peer_cdr  */
		}
	}
	
	return res;
}

int tris_monitor_stop_for_builtin(struct tris_channel *chan, int need_lock)
{
	int delfiles = 0;

	LOCK_IF_NEEDED(chan, need_lock);

	if (chan->monitor) {
		char filename[ FILENAME_MAX ];

		if (chan->monitor->read_stream) {
			tris_closestream(chan->monitor->read_stream);
		}
		if (chan->monitor->write_stream) {
			tris_closestream(chan->monitor->write_stream);
		}

		if (chan->monitor->filename_changed && !tris_strlen_zero(chan->monitor->filename_base)) {
			if (tris_fileexists(chan->monitor->read_filename,NULL,NULL) > 0) {
				snprintf(filename, FILENAME_MAX, "%s-in", chan->monitor->filename_base);
				if (tris_fileexists(filename, NULL, NULL) > 0) {
					tris_filedelete(filename, NULL);
				}
				tris_filerename(chan->monitor->read_filename, filename, chan->monitor->format);
			} else {
				tris_log(LOG_WARNING, "File %s not found\n", chan->monitor->read_filename);
			}

			if (tris_fileexists(chan->monitor->write_filename,NULL,NULL) > 0) {
				snprintf(filename, FILENAME_MAX, "%s-out", chan->monitor->filename_base);
				if (tris_fileexists(filename, NULL, NULL) > 0) {
					tris_filedelete(filename, NULL);
				}
				tris_filerename(chan->monitor->write_filename, filename, chan->monitor->format);
			} else {
				tris_log(LOG_WARNING, "File %s not found\n", chan->monitor->write_filename);
			}
		}

		if (chan->monitor->joinfiles && !tris_strlen_zero(chan->monitor->filename_base)) {
			char tmp[1024];
			char tmp2[1024];
			const char *format = !strcasecmp(chan->monitor->format,"wav49") ? "WAV" : chan->monitor->format;
			char *name = chan->monitor->filename_base;
			int directory = strchr(name, '/') ? 1 : 0;
			const char *dir = directory ? "" : tris_config_TRIS_MONITOR_DIR;
			const char *execute, *execute_args;
			const char *absolute = *name == '/' ? "" : "/";

			/* Set the execute application */
			execute = pbx_builtin_getvar_helper(chan, "MONITOR_EXEC");
			if (tris_strlen_zero(execute)) {
#ifdef HAVE_SOXMIX
				execute = "nice -n 19 soxmix";
#else
				execute = "nice -n 19 sox -m";
#endif
				//format = get_soxmix_format(format);
				delfiles = 1;
			} 
			execute_args = pbx_builtin_getvar_helper(chan, "MONITOR_EXEC_ARGS");
			if (tris_strlen_zero(execute_args)) {
				execute_args = "";
			}
			
			snprintf(tmp, sizeof(tmp), "%s \"%s%s%s-in.%s\" \"%s%s%s-out.%s\" \"%s%s%s.%s\" %s &", execute, dir, absolute, name, format, dir, absolute, name, format, dir, absolute, name, format,execute_args);
			if (delfiles) {
				snprintf(tmp2,sizeof(tmp2), "( %s& rm -f \"%s%s%s-\"* ) &",tmp, dir, absolute, name); /* remove legs when done mixing */
				tris_copy_string(tmp, tmp2, sizeof(tmp));
			}
			tris_debug(1,"monitor executing %s\n",tmp);
			tris_verbose("monitor executing %s\n",tmp);
			//if (tris_safe_system(tmp) == -1)
			//	tris_log(LOG_WARNING, "Execute of %s failed.\n",tmp);
		}
		
		tris_free(chan->monitor->format);
		tris_free(chan->monitor);
		chan->monitor = NULL;

		manager_event(EVENT_FLAG_CALL, "MonitorStop",
        	                "Channel: %s\r\n"
	                        "Uniqueid: %s\r\n",
	                        chan->name,
	                        chan->uniqueid
	                        );
	}

	UNLOCK_IF_NEEDED(chan, need_lock);

	return 0;
}

/*! \brief Output parking event to manager */
static void post_manager_event(const char *s, struct parkeduser *pu)
{
	manager_event(EVENT_FLAG_CALL, s,
		"Exten: %s\r\n"
		"Channel: %s\r\n"
		"Parkinglot: %s\r\n"
		"CallerIDNum: %s\r\n"
		"CallerIDName: %s\r\n"
		"UniqueID: %s\r\n\r\n",
		pu->parkingexten, 
		pu->chan->name,
		pu->parkinglot->name,
		S_OR(pu->chan->cid.cid_num, "<unknown>"),
		S_OR(pu->chan->cid.cid_name, "<unknown>"),
		pu->chan->uniqueid
		);
}

static char *callback_dialoptions(struct tris_flags *features_callee, struct tris_flags *features_caller, char *options, size_t len)
{
	int i = 0;
	enum {
		OPT_CALLEE_REDIRECT   = 't',
		OPT_CALLER_REDIRECT   = 'T',
		OPT_CALLEE_AUTOMON    = 'w',
		OPT_CALLER_AUTOMON    = 'W',
		OPT_CALLEE_DISCONNECT = 'h',
		OPT_CALLER_DISCONNECT = 'H',
		OPT_CALLEE_PARKCALL   = 'k',
		OPT_CALLER_PARKCALL   = 'K',
	};

	memset(options, 0, len);
	if (tris_test_flag(features_caller, TRIS_FEATURE_REDIRECT) && i < len) {
		options[i++] = OPT_CALLER_REDIRECT;
	}
	if (tris_test_flag(features_caller, TRIS_FEATURE_AUTOMON) && i < len) {
		options[i++] = OPT_CALLER_AUTOMON;
	}
	if (tris_test_flag(features_caller, TRIS_FEATURE_DISCONNECT) && i < len) {
		options[i++] = OPT_CALLER_DISCONNECT;
	}
	if (tris_test_flag(features_caller, TRIS_FEATURE_PARKCALL) && i < len) {
		options[i++] = OPT_CALLER_PARKCALL;
	}

	if (tris_test_flag(features_callee, TRIS_FEATURE_REDIRECT) && i < len) {
		options[i++] = OPT_CALLEE_REDIRECT;
	}
	if (tris_test_flag(features_callee, TRIS_FEATURE_AUTOMON) && i < len) {
		options[i++] = OPT_CALLEE_AUTOMON;
	}
	if (tris_test_flag(features_callee, TRIS_FEATURE_DISCONNECT) && i < len) {
		options[i++] = OPT_CALLEE_DISCONNECT;
	}
	if (tris_test_flag(features_callee, TRIS_FEATURE_PARKCALL) && i < len) {
		options[i++] = OPT_CALLEE_PARKCALL;
	}

	return options;
}

/*! \brief Run management on parkinglots, called once per parkinglot */
int manage_parkinglot(struct tris_parkinglot *curlot, fd_set *rfds, fd_set *efds, fd_set *nrfds, fd_set *nefds, int *ms, int *max)
{

	struct parkeduser *pu;
	int res = 0;
	char parkingslot[TRIS_MAX_EXTENSION];

	/* Lock parking list */
	TRIS_LIST_LOCK(&curlot->parkings);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&curlot->parkings, pu, list) {
		struct tris_channel *chan = pu->chan;	/* shorthand */
		int tms;        /* timeout for this item */
		int x;          /* fd index in channel */
		struct tris_context *con;

		if (pu->notquiteyet) { /* Pretend this one isn't here yet */
			continue;
		}
		tms = tris_tvdiff_ms(tris_tvnow(), pu->start);
		if (tms > pu->parkingtime) {
			/* Stop music on hold */
			tris_indicate(pu->chan, TRIS_CONTROL_UNHOLD);
			/* Get chan, exten from derived kludge */
			if (pu->peername[0]) {
				char *peername = tris_strdupa(pu->peername);
				char *cp = strrchr(peername, '-');
				char peername_flat[TRIS_MAX_EXTENSION]; /* using something like DAHDI/52 for an extension name is NOT a good idea */
				int i;

				if (cp) 
					*cp = 0;
				tris_copy_string(peername_flat,peername,sizeof(peername_flat));
				for(i=0; peername_flat[i] && i < TRIS_MAX_EXTENSION; i++) {
					if (peername_flat[i] == '/') 
						peername_flat[i]= '0';
				}
				con = tris_context_find_or_create(NULL, NULL, pu->parkinglot->parking_con_dial, registrar);
				if (!con) {
					tris_log(LOG_ERROR, "Parking dial context '%s' does not exist and unable to create\n", pu->parkinglot->parking_con_dial);
				}
				if (con) {
					char returnexten[TRIS_MAX_EXTENSION];
					struct tris_datastore *features_datastore;
					struct tris_dial_features *dialfeatures = NULL;

					tris_channel_lock(chan);

					if ((features_datastore = tris_channel_datastore_find(chan, &dial_features_info, NULL)))
						dialfeatures = features_datastore->data;

					tris_channel_unlock(chan);

					if (!strncmp(peername, "Parked/", 7)) {
						peername += 7;
					}

					if (dialfeatures) {
						char buf[MAX_DIAL_FEATURE_OPTIONS] = {0,};
						snprintf(returnexten, sizeof(returnexten), "%s,30,%s", peername, callback_dialoptions(&(dialfeatures->features_callee), &(dialfeatures->features_caller), buf, sizeof(buf)));
					} else { /* Existing default */
						tris_log(LOG_WARNING, "Dialfeatures not found on %s, using default!\n", chan->name);
						snprintf(returnexten, sizeof(returnexten), "%s,30,t", peername);
					}

					tris_add_extension2(con, 1, peername_flat, 1, NULL, NULL, "Dial", tris_strdup(returnexten), tris_free_ptr, registrar);
				}
				if (pu->options_specified == 1) {
					/* Park() was called with overriding return arguments, respect those arguments */
					set_c_e_p(chan, pu->context, pu->exten, pu->priority);
				} else {
					if (comebacktoorigin) {
						set_c_e_p(chan, pu->parkinglot->parking_con_dial, peername_flat, 1);
					} else {
						tris_log(LOG_WARNING, "now going to parkedcallstimeout,s,1 | ps is %d\n",pu->parkingnum);
						snprintf(parkingslot, sizeof(parkingslot), "%d", pu->parkingnum);
						pbx_builtin_setvar_helper(chan, "PARKINGSLOT", parkingslot);
						set_c_e_p(chan, "parkedcallstimeout", peername_flat, 1);
					}
				}
			} else {
				/* They've been waiting too long, send them back to where they came.  Theoretically they
				   should have their original extensions and such, but we copy to be on the safe side */
				set_c_e_p(chan, pu->context, pu->exten, pu->priority);
			}
			post_manager_event("ParkedCallTimeOut", pu);

			tris_verb(2, "Timeout for %s parked on %d (%s). Returning to %s,%s,%d\n", pu->chan->name, pu->parkingnum, pu->parkinglot->name, pu->chan->context, pu->chan->exten, pu->chan->priority);
			/* Start up the PBX, or hang them up */
			if (tris_pbx_start(chan))  {
				tris_log(LOG_WARNING, "Unable to restart the PBX for user on '%s', hanging them up...\n", pu->chan->name);
				tris_hangup(chan);
			}
			/* And take them out of the parking lot */
			con = tris_context_find(pu->parkinglot->parking_con);
			if (con) {
				if (tris_context_remove_extension2(con, pu->parkingexten, 1, NULL, 0))
					tris_log(LOG_WARNING, "Whoa, failed to remove the parking extension!\n");
				else
					notify_metermaids(pu->parkingexten, curlot->parking_con, TRIS_DEVICE_NOT_INUSE);
			} else
				tris_log(LOG_WARNING, "Whoa, no parking context?\n");
			TRIS_LIST_REMOVE_CURRENT(list);
			free(pu);
		} else {	/* still within parking time, process descriptors */
			for (x = 0; x < TRIS_MAX_FDS; x++) {
				struct tris_frame *f;

				if ((chan->fds[x] == -1) || (!FD_ISSET(chan->fds[x], rfds) && !FD_ISSET(pu->chan->fds[x], efds))) 
					continue;
				
				if (FD_ISSET(chan->fds[x], efds))
					tris_set_flag(chan, TRIS_FLAG_EXCEPTION);
				else
					tris_clear_flag(chan, TRIS_FLAG_EXCEPTION);
				chan->fdno = x;

				/* See if they need servicing */
				f = tris_read(pu->chan);
				/* Hangup? */
				if (!f || ((f->frametype == TRIS_FRAME_CONTROL) && (f->subclass ==  TRIS_CONTROL_HANGUP))) {
					if (f)
						tris_frfree(f);
					post_manager_event("ParkedCallGiveUp", pu);

					/* There's a problem, hang them up*/
					tris_verb(2, "%s got tired of being parked\n", chan->name);
					tris_hangup(chan);
					/* And take them out of the parking lot */
					con = tris_context_find(curlot->parking_con);
					if (con) {
						if (tris_context_remove_extension2(con, pu->parkingexten, 1, NULL, 0))
							tris_log(LOG_WARNING, "Whoa, failed to remove the extension!\n");
						else
							notify_metermaids(pu->parkingexten, curlot->parking_con, TRIS_DEVICE_NOT_INUSE);
					} else
						tris_log(LOG_WARNING, "Whoa, no parking context for parking lot %s?\n", curlot->name);
					TRIS_LIST_REMOVE_CURRENT(list);
					free(pu);
					break;
				} else {
					/* XXX Maybe we could do something with packets, like dial "0" for operator or something XXX */
					tris_frfree(f);
					if (pu->moh_trys < 3 && !chan->generatordata) {
						tris_debug(1, "MOH on parked call stopped by outside source.  Restarting on channel %s.\n", chan->name);
						tris_indicate_data(chan, TRIS_CONTROL_HOLD, 
							S_OR(curlot->mohclass, NULL),
							(!tris_strlen_zero(curlot->mohclass) ? strlen(curlot->mohclass) + 1 : 0));
						pu->moh_trys++;
					}
					goto std;	/* XXX Ick: jumping into an else statement??? XXX */
				}
			} /* End for */
			if (x >= TRIS_MAX_FDS) {
std:				for (x=0; x<TRIS_MAX_FDS; x++) {	/* mark fds for next round */
					if (chan->fds[x] > -1) {
						FD_SET(chan->fds[x], nrfds);
						FD_SET(chan->fds[x], nefds);
						if (chan->fds[x] > *max)
							*max = chan->fds[x];
					}
				}
				/* Keep track of our shortest wait */
				if (tms < *ms || *ms < 0)
					*ms = tms;
			}
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&curlot->parkings);
	return res;
}

/*! 
 * \brief Take care of parked calls and unpark them if needed 
 * \param ignore unused var.
 * 
 * Start inf loop, lock parking lot, check if any parked channels have gone above timeout
 * if so, remove channel from parking lot and return it to the extension that parked it.
 * Check if parked channel decided to hangup, wait until next FD via select().
*/
static void *do_parking_thread(void *ignore)
{
	fd_set rfds, efds;	/* results from previous select, to be preserved across loops. */
	fd_set nrfds, nefds;	/* args for the next select */
	FD_ZERO(&rfds);
	FD_ZERO(&efds);

	for (;;) {
		int res = 0;
		int ms = -1;	/* select timeout, uninitialized */
		int max = -1;	/* max fd, none there yet */
		struct ao2_iterator iter;
		struct tris_parkinglot *curlot;
		FD_ZERO(&nrfds);
		FD_ZERO(&nefds);
		iter = ao2_iterator_init(parkinglots, 0);

		while ((curlot = ao2_iterator_next(&iter))) {
			res = manage_parkinglot(curlot, &rfds, &efds, &nrfds, &nefds, &ms, &max);
			ao2_ref(curlot, -1);
		}

		rfds = nrfds;
		efds = nefds;
		{
			struct timeval wait = tris_samp2tv(ms, 1000);
			/* Wait for something to happen */
			tris_select(max + 1, &rfds, NULL, &efds, (ms > -1) ? &wait : NULL);
		}
		pthread_testcancel();
	}
	return NULL;	/* Never reached */
}

/*! \brief Find parkinglot by name */
struct tris_parkinglot *find_parkinglot(const char *name)
{
	struct tris_parkinglot *parkinglot = NULL;
	struct tris_parkinglot tmp_parkinglot;
	
	if (tris_strlen_zero(name))
		return NULL;

	tris_copy_string(tmp_parkinglot.name, name, sizeof(tmp_parkinglot.name));

	parkinglot = ao2_find(parkinglots, &tmp_parkinglot, OBJ_POINTER);

	if (parkinglot && option_debug)
		tris_log(LOG_DEBUG, "Found Parkinglot: %s\n", parkinglot->name);

	return parkinglot;
}

TRIS_APP_OPTIONS(park_call_options, BEGIN_OPTIONS
	TRIS_APP_OPTION('r', TRIS_PARK_OPT_RINGING),
	TRIS_APP_OPTION('R', TRIS_PARK_OPT_RANDOMIZE),
	TRIS_APP_OPTION('s', TRIS_PARK_OPT_SILENCE),
END_OPTIONS );

/*! \brief Park a call */
static int park_call_exec(struct tris_channel *chan, void *data)
{
	/* Cache the original channel name in case we get masqueraded in the middle
	 * of a park--it is still theoretically possible for a transfer to happen before
	 * we get here, but it is _really_ unlikely */
	char *orig_chan_name = tris_strdupa(chan->name);
	char orig_exten[TRIS_MAX_EXTENSION];
	int orig_priority = chan->priority;

	/* Data is unused at the moment but could contain a parking
	   lot context eventually */
	int res = 0;

	char *parse = NULL;
	TRIS_DECLARE_APP_ARGS(app_args,
		TRIS_APP_ARG(timeout);
		TRIS_APP_ARG(return_con);
		TRIS_APP_ARG(return_ext);
		TRIS_APP_ARG(return_pri);
		TRIS_APP_ARG(options);
	);

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(app_args, parse);

	tris_copy_string(orig_exten, chan->exten, sizeof(orig_exten));

	/* Setup the exten/priority to be s/1 since we don't know
	   where this call should return */
	strcpy(chan->exten, "s");
	chan->priority = 1;

	/* Answer if call is not up */
	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);

	/* Sleep to allow VoIP streams to settle down */
	if (!res)
		res = tris_safe_sleep(chan, 1000);

	/* Park the call */
	if (!res) {
		struct tris_park_call_args args = {
			.orig_chan_name = orig_chan_name,
		};
		struct tris_flags flags = { 0 };

		if (parse) {
			if (!tris_strlen_zero(app_args.timeout)) {
				if (sscanf(app_args.timeout, "%30d", &args.timeout) != 1) {
					tris_log(LOG_WARNING, "Invalid timeout '%s' provided\n", app_args.timeout);
					args.timeout = 0;
				}
			}
			if (!tris_strlen_zero(app_args.return_con)) {
				args.return_con = app_args.return_con;
			}
			if (!tris_strlen_zero(app_args.return_ext)) {
				args.return_ext = app_args.return_ext;
			}
			if (!tris_strlen_zero(app_args.return_pri)) {
				if (sscanf(app_args.return_pri, "%30d", &args.return_pri) != 1) {
					tris_log(LOG_WARNING, "Invalid priority '%s' specified\n", app_args.return_pri);
					args.return_pri = 0;
				}
			}
		}

		tris_app_parse_options(park_call_options, &flags, NULL, app_args.options);
		args.flags = flags.flags;

		res = masq_park_call_announce_args(chan, chan, &args);
		/* Continue on in the dialplan */
		if (res == 1) {
			tris_copy_string(chan->exten, orig_exten, sizeof(chan->exten));
			chan->priority = orig_priority;
			res = 0;
		} else if (!res) {
			res = 1;
		}
	}

	return res;
}

/*! \brief Pickup parked call */
static int park_exec_full(struct tris_channel *chan, void *data, struct tris_parkinglot *parkinglot)
{
	int res = 0;
	struct tris_channel *peer=NULL;
	struct parkeduser *pu;
	struct tris_context *con;
	int park = 0;
	struct tris_bridge_config config;

	if (data)
		park = atoi((char *)data);

	parkinglot = find_parkinglot(findparkinglotname(chan)); 	
	if (!parkinglot)
		parkinglot = default_parkinglot;

	TRIS_LIST_LOCK(&parkinglot->parkings);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&parkinglot->parkings, pu, list) {
		if (!data || pu->parkingnum == park) {
			if (pu->chan->pbx) { /* do not allow call to be picked up until the PBX thread is finished */
				TRIS_LIST_UNLOCK(&parkinglot->parkings);
				return -1;
			}
			TRIS_LIST_REMOVE_CURRENT(list);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&parkinglot->parkings);

	if (pu) {
		peer = pu->chan;
		con = tris_context_find(parkinglot->parking_con);
		if (con) {
			if (tris_context_remove_extension2(con, pu->parkingexten, 1, NULL, 0))
				tris_log(LOG_WARNING, "Whoa, failed to remove the extension!\n");
			else
				notify_metermaids(pu->parkingexten, parkinglot->parking_con, TRIS_DEVICE_NOT_INUSE);
		} else
			tris_log(LOG_WARNING, "Whoa, no parking context?\n");

		manager_event(EVENT_FLAG_CALL, "UnParkedCall",
			"Exten: %s\r\n"
			"Channel: %s\r\n"
			"From: %s\r\n"
			"CallerIDNum: %s\r\n"
			"CallerIDName: %s\r\n",
			pu->parkingexten, pu->chan->name, chan->name,
			S_OR(pu->chan->cid.cid_num, "<unknown>"),
			S_OR(pu->chan->cid.cid_name, "<unknown>")
			);

		tris_free(pu);
	}
	/* JK02: it helps to answer the channel if not already up */
	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	//XXX Why do we unlock here ?
	// uncomment it for now, till my setup with debug_threads and detect_deadlocks starts to complain
	//ASTOBJ_UNLOCK(parkinglot);

	if (peer) {
		struct tris_datastore *features_datastore;
		struct tris_dial_features *dialfeatures = NULL;

		/* Play a courtesy to the source(s) configured to prefix the bridge connecting */

		if (!tris_strlen_zero(courtesytone)) {
			int error = 0;
			tris_indicate(peer, TRIS_CONTROL_UNHOLD);
			if (parkedplay == 0) {
				error = tris_stream_and_wait(chan, courtesytone, "");
			} else if (parkedplay == 1) {
				error = tris_stream_and_wait(peer, courtesytone, "");
			} else if (parkedplay == 2) {
				if (!tris_streamfile(chan, courtesytone, chan->language) &&
						!tris_streamfile(peer, courtesytone, chan->language)) {
					/*! \todo XXX we would like to wait on both! */
					res = tris_waitstream(chan, "");
					if (res >= 0)
						res = tris_waitstream(peer, "");
					if (res < 0)
						error = 1;
				}
			}
			if (error) {
				tris_log(LOG_WARNING, "Failed to play courtesy tone!\n");
				tris_hangup(peer);
				return -1;
			}
		} else
			tris_indicate(peer, TRIS_CONTROL_UNHOLD);

		res = tris_channel_make_compatible(chan, peer);
		if (res < 0) {
			tris_log(LOG_WARNING, "Could not make channels %s and %s compatible for bridge\n", chan->name, peer->name);
			tris_hangup(peer);
			return -1;
		}
		/* This runs sorta backwards, since we give the incoming channel control, as if it
		   were the person called. */
		tris_verb(3, "Channel %s connected to parked call %d\n", chan->name, park);

		pbx_builtin_setvar_helper(chan, "PARKEDCHANNEL", peer->name);
		tris_cdr_setdestchan(chan->cdr, peer->name);
		memset(&config, 0, sizeof(struct tris_bridge_config));

		/* Get datastore for peer and apply it's features to the callee side of the bridge config */
		tris_channel_lock(peer);
		if ((features_datastore = tris_channel_datastore_find(peer, &dial_features_info, NULL))) {
			dialfeatures = features_datastore->data;
		}
		tris_channel_unlock(peer);

		/* When the datastores for both caller and callee are created, both the callee and caller channels
		 * use the features_caller flag variable to represent themselves. With that said, the config.features_callee
		 * flags should be copied from the datastore's caller feature flags regardless if peer was a callee
		 * or caller. */
		if (dialfeatures) {
			tris_copy_flags(&(config.features_callee), &(dialfeatures->features_caller), TRIS_FLAGS_ALL);
		}

		if ((parkinglot->parkedcalltransfers == TRIS_FEATURE_FLAG_BYCALLEE) || (parkinglot->parkedcalltransfers == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_callee), TRIS_FEATURE_REDIRECT);
		}
		if ((parkinglot->parkedcalltransfers == TRIS_FEATURE_FLAG_BYCALLER) || (parkinglot->parkedcalltransfers == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_caller), TRIS_FEATURE_REDIRECT);
		}
		if ((parkinglot->parkedcallreparking == TRIS_FEATURE_FLAG_BYCALLEE) || (parkinglot->parkedcallreparking == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_callee), TRIS_FEATURE_PARKCALL);
		}
		if ((parkinglot->parkedcallreparking == TRIS_FEATURE_FLAG_BYCALLER) || (parkinglot->parkedcallreparking == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_caller), TRIS_FEATURE_PARKCALL);
		}
		if ((parkinglot->parkedcallhangup == TRIS_FEATURE_FLAG_BYCALLEE) || (parkinglot->parkedcallhangup == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_callee), TRIS_FEATURE_DISCONNECT);
		}
		if ((parkinglot->parkedcallhangup == TRIS_FEATURE_FLAG_BYCALLER) || (parkinglot->parkedcallhangup == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_caller), TRIS_FEATURE_DISCONNECT);
		}
		if ((parkinglot->parkedcallrecording == TRIS_FEATURE_FLAG_BYCALLEE) || (parkinglot->parkedcallrecording == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_callee), TRIS_FEATURE_AUTOMON);
		}
		if ((parkinglot->parkedcallrecording == TRIS_FEATURE_FLAG_BYCALLER) || (parkinglot->parkedcallrecording == TRIS_FEATURE_FLAG_BYBOTH)) {
			tris_set_flag(&(config.features_caller), TRIS_FEATURE_AUTOMON);
		}

		res = tris_bridge_call(chan, peer, &config);

		pbx_builtin_setvar_helper(chan, "PARKEDCHANNEL", peer->name);
		tris_cdr_setdestchan(chan->cdr, peer->name);

		/* Simulate the PBX hanging up */
		tris_hangup(peer);
		return -1;
	} else {
		/*! \todo XXX Play a message XXX */
		if (tris_stream_and_wait(chan, "pbx/pbx-invalidpark", ""))
			tris_log(LOG_WARNING, "tris_streamfile of %s failed on %s\n", "pbx/pbx-invalidpark", chan->name);
		tris_verb(3, "Channel %s tried to talk to nonexistent parked call %d\n", chan->name, park);
		res = -1;
	}

	return -1;
}

static int park_exec(struct tris_channel *chan, void *data) 
{
	return park_exec_full(chan, data, default_parkinglot);
}

/*! \brief Unreference parkinglot object. If no more references,
	then go ahead and delete it */
static void parkinglot_unref(struct tris_parkinglot *parkinglot) 
{
	int refcount = ao2_ref(parkinglot, -1);
	if (option_debug > 2)
		tris_log(LOG_DEBUG, "Multiparking: %s refcount now %d\n", parkinglot->name, refcount - 1);
}

static struct tris_parkinglot *parkinglot_addref(struct tris_parkinglot *parkinglot)
{
	int refcount = ao2_ref(parkinglot, +1);
	if (option_debug > 2)
		tris_log(LOG_DEBUG, "Multiparking: %s refcount now %d\n", parkinglot->name, refcount + 1);
	return parkinglot;
}

/*! \brief Allocate parking lot structure */
static struct tris_parkinglot *create_parkinglot(char *name)
{
	struct tris_parkinglot *newlot = (struct tris_parkinglot *) NULL;

	if (!name)
		return NULL;

	newlot = ao2_alloc(sizeof(*newlot), parkinglot_destroy);
	if (!newlot)
		return NULL;
	
	tris_copy_string(newlot->name, name, sizeof(newlot->name));
	TRIS_LIST_HEAD_INIT(&newlot->parkings);

	return newlot;
}

/*! \brief Destroy a parking lot */
static void parkinglot_destroy(void *obj)
{
	struct tris_parkinglot *ruin = obj;
	struct tris_context *con;
	con = tris_context_find(ruin->parking_con);
	if (con)
		tris_context_destroy(con, registrar);
	ao2_unlink(parkinglots, ruin);
}

/*! \brief Build parkinglot from configuration and chain it in */
static struct tris_parkinglot *build_parkinglot(char *name, struct tris_variable *var)
{
	struct tris_parkinglot *parkinglot;
	struct tris_context *con = NULL;

	struct tris_variable *confvar = var;
	int error = 0;
	int start = 0, end = 0;
	int oldparkinglot = 0;

	parkinglot = find_parkinglot(name);
	if (parkinglot)
		oldparkinglot = 1;
	else
		parkinglot = create_parkinglot(name);

	if (!parkinglot)
		return NULL;

	ao2_lock(parkinglot);

	if (option_debug)
		tris_log(LOG_DEBUG, "Building parking lot %s\n", name);
	
	/* Do some config stuff */
	while(confvar) {
		if (!strcasecmp(confvar->name, "context")) {
			tris_copy_string(parkinglot->parking_con, confvar->value, sizeof(parkinglot->parking_con));
		} else if (!strcasecmp(confvar->name, "parkingtime")) {
			if ((sscanf(confvar->value, "%30d", &parkinglot->parkingtime) != 1) || (parkinglot->parkingtime < 1)) {
				tris_log(LOG_WARNING, "%s is not a valid parkingtime\n", confvar->value);
				parkinglot->parkingtime = DEFAULT_PARK_TIME;
			} else
				parkinglot->parkingtime = parkinglot->parkingtime * 1000;
		} else if (!strcasecmp(confvar->name, "parkpos")) {
			if (sscanf(confvar->value, "%30d-%30d", &start, &end) != 2) {
				tris_log(LOG_WARNING, "Format for parking positions is a-b, where a and b are numbers at line %d of parking.conf\n", confvar->lineno);
				error = 1;
			} else {
				parkinglot->parking_start = start;
				parkinglot->parking_stop = end;
			}
		} else if (!strcasecmp(confvar->name, "findslot")) {
			parkinglot->parkfindnext = (!strcasecmp(confvar->value, "next"));
		}
		confvar = confvar->next;
	}
	/* make sure parkingtime is set if not specified */
	if (parkinglot->parkingtime == 0) {
		parkinglot->parkingtime = DEFAULT_PARK_TIME;
	}

	if (!var) {	/* Default parking lot */
		tris_copy_string(parkinglot->parking_con, "parkedcalls", sizeof(parkinglot->parking_con));
		tris_copy_string(parkinglot->parking_con_dial, "park-dial", sizeof(parkinglot->parking_con_dial));
		tris_copy_string(parkinglot->mohclass, "default", sizeof(parkinglot->mohclass));
	}

	/* Check for errors */
	if (tris_strlen_zero(parkinglot->parking_con)) {
		tris_log(LOG_WARNING, "Parking lot %s lacks context\n", name);
		error = 1;
	}

	/* Create context */
	if (!error && !(con = tris_context_find_or_create(NULL, NULL, parkinglot->parking_con, registrar))) {
		tris_log(LOG_ERROR, "Parking context '%s' does not exist and unable to create\n", parkinglot->parking_con);
		error = 1;
	}

	/* Add a parking extension into the context */
	if (!error && !oldparkinglot) {
		if (!tris_strlen_zero(tris_parking_ext())) {
			if (tris_add_extension2(con, 1, tris_parking_ext(), 1, NULL, NULL, parkcall, strdup(""), tris_free_ptr, registrar) == -1)
				error = 1;
		}
	}

	ao2_unlock(parkinglot);

	if (error) {
		tris_log(LOG_WARNING, "Parking %s not open for business. Configuration error.\n", name);
		parkinglot_destroy(parkinglot);
		return NULL;
	}
	if (option_debug)
		tris_log(LOG_DEBUG, "Parking %s now open for business. (start exten %d end %d)\n", name, start, end);


	/* Move it into the list, if it wasn't already there */
	if (!oldparkinglot) {
		ao2_link(parkinglots, parkinglot);
	}
	parkinglot_unref(parkinglot);

	return parkinglot;
}


/*! 
 * \brief Add parking hints for all defined parking lots 
 * \param context
 * \param start starting parkinglot number
 * \param stop ending parkinglot number
*/
static void park_add_hints(char *context, int start, int stop)
{
	int numext;
	char device[TRIS_MAX_EXTENSION];
	char exten[10];

	for (numext = start; numext <= stop; numext++) {
		snprintf(exten, sizeof(exten), "%d", numext);
		snprintf(device, sizeof(device), "park:%s@%s", exten, context);
		tris_add_extension(context, 1, exten, PRIORITY_HINT, NULL, NULL, device, NULL, NULL, registrar);
	}
}

static int load_config(void) 
{
	int start = 0, end = 0;
	int res;
	int i;
	struct tris_context *con = NULL;
	struct tris_config *cfg = NULL;
	struct tris_variable *var = NULL;
	struct feature_group *fg = NULL;
	struct tris_flags config_flags = { 0 };
	char old_parking_ext[TRIS_MAX_EXTENSION];
	char old_parking_con[TRIS_MAX_EXTENSION] = "";
	char *ctg; 
	static const char *categories[] = { 
		/* Categories in features.conf that are not
		 * to be parsed as group categories
		 */
		"general",
		"featuremap",
		"applicationmap"
	};

	if (default_parkinglot) {
		strcpy(old_parking_con, default_parkinglot->parking_con);
		strcpy(old_parking_ext, parking_ext);
	} else {
		default_parkinglot = build_parkinglot(DEFAULT_PARKINGLOT, NULL);
		if (default_parkinglot) {
			ao2_lock(default_parkinglot);
			default_parkinglot->parking_start = 701;
			default_parkinglot->parking_stop = 750;
			default_parkinglot->parking_offset = 0;
			default_parkinglot->parkfindnext = 0;
			default_parkinglot->parkingtime = DEFAULT_PARK_TIME;
			ao2_unlock(default_parkinglot);
		}
	}
	if (default_parkinglot) {
		if (option_debug)
			tris_log(LOG_DEBUG, "Configuration of default parkinglot done.\n");
	} else {
		tris_log(LOG_ERROR, "Configuration of default parkinglot failed.\n");
		return -1;
	}
	

	/* Reset to defaults */
	strcpy(parking_ext, "700");
	strcpy(pickup_ext, "*8");
	courtesytone[0] = '\0';
	strcpy(xfersound, "beep");
	strcpy(xferfailsound, "pbx/pbx-invalid");
	pickupsound[0] = '\0';
	pickupfailsound[0] = '\0';
	adsipark = 0;
	comebacktoorigin = 1;

	default_parkinglot->parkaddhints = 0;
	default_parkinglot->parkedcalltransfers = 0;
	default_parkinglot->parkedcallreparking = 0;
	default_parkinglot->parkedcallrecording = 0;
	default_parkinglot->parkedcallhangup = 0;

	transferdigittimeout = DEFAULT_TRANSFER_DIGIT_TIMEOUT;
	featuredigittimeout = DEFAULT_FEATURE_DIGIT_TIMEOUT;
	atxfernoanswertimeout = DEFAULT_NOANSWER_TIMEOUT_ATTENDED_TRANSFER;
	atxferloopdelay = DEFAULT_ATXFER_LOOP_DELAY;
	atxferdropcall = DEFAULT_ATXFER_DROP_CALL;
	atxfercallbackretries = DEFAULT_ATXFER_CALLBACK_RETRIES;

	cfg = tris_config_load2("features.conf", "features", config_flags);
	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEUNCHANGED || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING,"Could not load features.conf\n");
		return 0;
	}
	for (var = tris_variable_browse(cfg, "general"); var; var = var->next) {
		if (!strcasecmp(var->name, "parkext")) {
			tris_copy_string(parking_ext, var->value, sizeof(parking_ext));
		} else if (!strcasecmp(var->name, "context")) {
			tris_copy_string(default_parkinglot->parking_con, var->value, sizeof(default_parkinglot->parking_con));
		} else if (!strcasecmp(var->name, "parkingtime")) {
			if ((sscanf(var->value, "%30d", &default_parkinglot->parkingtime) != 1) || (default_parkinglot->parkingtime < 1)) {
				tris_log(LOG_WARNING, "%s is not a valid parkingtime\n", var->value);
				default_parkinglot->parkingtime = DEFAULT_PARK_TIME;
			} else
				default_parkinglot->parkingtime = default_parkinglot->parkingtime * 1000;
		} else if (!strcasecmp(var->name, "parkpos")) {
			if (sscanf(var->value, "%30d-%30d", &start, &end) != 2) {
				tris_log(LOG_WARNING, "Format for parking positions is a-b, where a and b are numbers at line %d of features.conf\n", var->lineno);
			} else if (default_parkinglot) {
				default_parkinglot->parking_start = start;
				default_parkinglot->parking_stop = end;
			} else {
				tris_log(LOG_WARNING, "No default parking lot!\n");
			}
		} else if (!strcasecmp(var->name, "findslot")) {
			default_parkinglot->parkfindnext = (!strcasecmp(var->value, "next"));
		} else if (!strcasecmp(var->name, "parkinghints")) {
			default_parkinglot->parkaddhints = tris_true(var->value);
		} else if (!strcasecmp(var->name, "parkedcalltransfers")) {
			if (!strcasecmp(var->value, "both"))
				default_parkinglot->parkedcalltransfers = TRIS_FEATURE_FLAG_BYBOTH;
			else if (!strcasecmp(var->value, "caller"))
				default_parkinglot->parkedcalltransfers = TRIS_FEATURE_FLAG_BYCALLER;
			else if (!strcasecmp(var->value, "callee"))
				default_parkinglot->parkedcalltransfers = TRIS_FEATURE_FLAG_BYCALLEE;
		} else if (!strcasecmp(var->name, "parkedcallreparking")) {
			if (!strcasecmp(var->value, "both"))
				default_parkinglot->parkedcallreparking = TRIS_FEATURE_FLAG_BYBOTH;
			else if (!strcasecmp(var->value, "caller"))
				default_parkinglot->parkedcallreparking = TRIS_FEATURE_FLAG_BYCALLER;
			else if (!strcasecmp(var->value, "callee"))
				default_parkinglot->parkedcallreparking = TRIS_FEATURE_FLAG_BYCALLEE;
		} else if (!strcasecmp(var->name, "parkedcallhangup")) {
			if (!strcasecmp(var->value, "both"))
				default_parkinglot->parkedcallhangup = TRIS_FEATURE_FLAG_BYBOTH;
			else if (!strcasecmp(var->value, "caller"))
				default_parkinglot->parkedcallhangup = TRIS_FEATURE_FLAG_BYCALLER;
			else if (!strcasecmp(var->value, "callee"))
				default_parkinglot->parkedcallhangup = TRIS_FEATURE_FLAG_BYCALLEE;
		} else if (!strcasecmp(var->name, "parkedcallrecording")) {
			if (!strcasecmp(var->value, "both"))
				default_parkinglot->parkedcallrecording = TRIS_FEATURE_FLAG_BYBOTH;
			else if (!strcasecmp(var->value, "caller"))
				default_parkinglot->parkedcallrecording = TRIS_FEATURE_FLAG_BYCALLER;
			else if (!strcasecmp(var->value, "callee"))
				default_parkinglot->parkedcallrecording = TRIS_FEATURE_FLAG_BYCALLEE;
		} else if (!strcasecmp(var->name, "adsipark")) {
			adsipark = tris_true(var->value);
		} else if (!strcasecmp(var->name, "transferdigittimeout")) {
			if ((sscanf(var->value, "%30d", &transferdigittimeout) != 1) || (transferdigittimeout < 1)) {
				tris_log(LOG_WARNING, "%s is not a valid transferdigittimeout\n", var->value);
				transferdigittimeout = DEFAULT_TRANSFER_DIGIT_TIMEOUT;
			} else
				transferdigittimeout = transferdigittimeout * 1000;
		} else if (!strcasecmp(var->name, "featuredigittimeout")) {
			if ((sscanf(var->value, "%30d", &featuredigittimeout) != 1) || (featuredigittimeout < 1)) {
				tris_log(LOG_WARNING, "%s is not a valid featuredigittimeout\n", var->value);
				featuredigittimeout = DEFAULT_FEATURE_DIGIT_TIMEOUT;
			}
		} else if (!strcasecmp(var->name, "atxfernoanswertimeout")) {
			if ((sscanf(var->value, "%30d", &atxfernoanswertimeout) != 1) || (atxfernoanswertimeout < 1)) {
				tris_log(LOG_WARNING, "%s is not a valid atxfernoanswertimeout\n", var->value);
				atxfernoanswertimeout = DEFAULT_NOANSWER_TIMEOUT_ATTENDED_TRANSFER;
			} else
				atxfernoanswertimeout = atxfernoanswertimeout * 1000;
		} else if (!strcasecmp(var->name, "atxferloopdelay")) {
			if ((sscanf(var->value, "%30u", &atxferloopdelay) != 1)) {
				tris_log(LOG_WARNING, "%s is not a valid atxferloopdelay\n", var->value);
				atxferloopdelay = DEFAULT_ATXFER_LOOP_DELAY;
			} else 
				atxferloopdelay *= 1000;
		} else if (!strcasecmp(var->name, "atxferdropcall")) {
			atxferdropcall = tris_true(var->value);
		} else if (!strcasecmp(var->name, "atxfercallbackretries")) {
			if ((sscanf(var->value, "%30u", &atxferloopdelay) != 1)) {
				tris_log(LOG_WARNING, "%s is not a valid atxfercallbackretries\n", var->value);
				atxfercallbackretries = DEFAULT_ATXFER_CALLBACK_RETRIES;
			}
		} else if (!strcasecmp(var->name, "courtesytone")) {
			tris_copy_string(courtesytone, var->value, sizeof(courtesytone));
		}  else if (!strcasecmp(var->name, "parkedplay")) {
			if (!strcasecmp(var->value, "both"))
				parkedplay = 2;
			else if (!strcasecmp(var->value, "parked"))
				parkedplay = 1;
			else
				parkedplay = 0;
		} else if (!strcasecmp(var->name, "xfersound")) {
			tris_copy_string(xfersound, var->value, sizeof(xfersound));
		} else if (!strcasecmp(var->name, "xferfailsound")) {
			tris_copy_string(xferfailsound, var->value, sizeof(xferfailsound));
		} else if (!strcasecmp(var->name, "pickupexten")) {
			tris_copy_string(pickup_ext, var->value, sizeof(pickup_ext));
		} else if (!strcasecmp(var->name, "pickupsound")) {
			tris_copy_string(pickupsound, var->value, sizeof(pickupsound));
		} else if (!strcasecmp(var->name, "pickupfailsound")) {
			tris_copy_string(pickupfailsound, var->value, sizeof(pickupfailsound));
		} else if (!strcasecmp(var->name, "comebacktoorigin")) {
			comebacktoorigin = tris_true(var->value);
		} else if (!strcasecmp(var->name, "parkedmusicclass")) {
			tris_copy_string(default_parkinglot->mohclass, var->value, sizeof(default_parkinglot->mohclass));
		}
	}

	unmap_features();
	for (var = tris_variable_browse(cfg, "featuremap"); var; var = var->next) {
		if (remap_feature(var->name, var->value))
			tris_log(LOG_NOTICE, "Unknown feature '%s'\n", var->name);
	}

	/* Map a key combination to an application*/
	tris_unregister_features();
	for (var = tris_variable_browse(cfg, "applicationmap"); var; var = var->next) {
		char *tmp_val = tris_strdupa(var->value);
		char *exten, *activateon, *activatedby, *app, *app_args, *moh_class; 
		struct tris_call_feature *feature;

		/* strsep() sets the argument to NULL if match not found, and it
		 * is safe to use it with a NULL argument, so we don't check
		 * between calls.
		 */
		exten = strsep(&tmp_val,",");
		activatedby = strsep(&tmp_val,",");
		app = strsep(&tmp_val,",");
		app_args = strsep(&tmp_val,",");
		moh_class = strsep(&tmp_val,",");

		activateon = strsep(&activatedby, "/");	

		/*! \todo XXX var_name or app_args ? */
		if (tris_strlen_zero(app) || tris_strlen_zero(exten) || tris_strlen_zero(activateon) || tris_strlen_zero(var->name)) {
			tris_log(LOG_NOTICE, "Please check the feature Mapping Syntax, either extension, name, or app aren't provided %s %s %s %s\n",
				app, exten, activateon, var->name);
			continue;
		}

		TRIS_RWLIST_RDLOCK(&feature_list);
		if ((feature = find_dynamic_feature(var->name))) {
			TRIS_RWLIST_UNLOCK(&feature_list);
			tris_log(LOG_WARNING, "Dynamic Feature '%s' specified more than once!\n", var->name);
			continue;
		}
		TRIS_RWLIST_UNLOCK(&feature_list);
				
		if (!(feature = tris_calloc(1, sizeof(*feature))))
			continue;					

		tris_copy_string(feature->sname, var->name, FEATURE_SNAME_LEN);
		tris_copy_string(feature->app, app, FEATURE_APP_LEN);
		tris_copy_string(feature->exten, exten, FEATURE_EXTEN_LEN);
		
		if (app_args) 
			tris_copy_string(feature->app_args, app_args, FEATURE_APP_ARGS_LEN);

		if (moh_class)
			tris_copy_string(feature->moh_class, moh_class, FEATURE_MOH_LEN);
			
		tris_copy_string(feature->exten, exten, sizeof(feature->exten));
		feature->operation = feature_exec_app;
		tris_set_flag(feature, TRIS_FEATURE_FLAG_NEEDSDTMF);

		/* Allow caller and calle to be specified for backwards compatability */
		if (!strcasecmp(activateon, "self") || !strcasecmp(activateon, "caller"))
			tris_set_flag(feature, TRIS_FEATURE_FLAG_ONSELF);
		else if (!strcasecmp(activateon, "peer") || !strcasecmp(activateon, "callee"))
			tris_set_flag(feature, TRIS_FEATURE_FLAG_ONPEER);
		else {
			tris_log(LOG_NOTICE, "Invalid 'ActivateOn' specification for feature '%s',"
				" must be 'self', or 'peer'\n", var->name);
			continue;
		}

		if (tris_strlen_zero(activatedby))
			tris_set_flag(feature, TRIS_FEATURE_FLAG_BYBOTH);
		else if (!strcasecmp(activatedby, "caller"))
			tris_set_flag(feature, TRIS_FEATURE_FLAG_BYCALLER);
		else if (!strcasecmp(activatedby, "callee"))
			tris_set_flag(feature, TRIS_FEATURE_FLAG_BYCALLEE);
		else if (!strcasecmp(activatedby, "both"))
			tris_set_flag(feature, TRIS_FEATURE_FLAG_BYBOTH);
		else {
			tris_log(LOG_NOTICE, "Invalid 'ActivatedBy' specification for feature '%s',"
				" must be 'caller', or 'callee', or 'both'\n", var->name);
			continue;
		}

		tris_register_feature(feature);
			
		tris_verb(2, "Mapping Feature '%s' to app '%s(%s)' with code '%s'\n", var->name, app, app_args, exten);
	}

	tris_unregister_groups();
	TRIS_RWLIST_WRLOCK(&feature_groups);

	ctg = NULL;
	while ((ctg = tris_category_browse(cfg, ctg))) {
		/* Is this a parkinglot definition ? */
		if (!strncasecmp(ctg, "parkinglot_", strlen("parkinglot_"))) {
			tris_debug(2, "Found configuration section %s, assume parking context\n", ctg);
			if(!build_parkinglot(ctg, tris_variable_browse(cfg, ctg)))
				tris_log(LOG_ERROR, "Could not build parking lot %s. Configuration error.\n", ctg);
			else
				tris_debug(1, "Configured parking context %s\n", ctg);
			continue;	
		}
		/* No, check if it's a group */
		for (i = 0; i < ARRAY_LEN(categories); i++) {
			if (!strcasecmp(categories[i], ctg))
				break;
		}

		if (i < ARRAY_LEN(categories)) 
			continue;

		if (!(fg = register_group(ctg)))
			continue;

		for (var = tris_variable_browse(cfg, ctg); var; var = var->next) {
			struct tris_call_feature *feature;

			TRIS_RWLIST_RDLOCK(&feature_list);
			if (!(feature = find_dynamic_feature(var->name)) && 
			    !(feature = tris_find_call_feature(var->name))) {
				TRIS_RWLIST_UNLOCK(&feature_list);
				tris_log(LOG_WARNING, "Feature '%s' was not found.\n", var->name);
				continue;
			}
			TRIS_RWLIST_UNLOCK(&feature_list);

			register_group_feature(fg, var->value, feature);
		}
	}

	TRIS_RWLIST_UNLOCK(&feature_groups);

	tris_config_destroy(cfg);

	/* Remove the old parking extension */
	if (!tris_strlen_zero(old_parking_con) && (con = tris_context_find(old_parking_con)))	{
		if(tris_context_remove_extension2(con, old_parking_ext, 1, registrar, 0))
				notify_metermaids(old_parking_ext, old_parking_con, TRIS_DEVICE_NOT_INUSE);
		tris_debug(1, "Removed old parking extension %s@%s\n", old_parking_ext, old_parking_con);
	}
	
	if (!(con = tris_context_find_or_create(NULL, NULL, default_parkinglot->parking_con, registrar))) {
		tris_log(LOG_ERROR, "Parking context '%s' does not exist and unable to create\n", default_parkinglot->parking_con);
		return -1;
	}
	res = tris_add_extension2(con, 1, tris_parking_ext(), 1, NULL, NULL, parkcall, NULL, NULL, registrar);
	if (default_parkinglot->parkaddhints)
		park_add_hints(default_parkinglot->parking_con, default_parkinglot->parking_start, default_parkinglot->parking_stop);
	if (!res)
		notify_metermaids(tris_parking_ext(), default_parkinglot->parking_con, TRIS_DEVICE_INUSE);
	return res;

}

/*!
 * \brief CLI command to list configured features
 * \param e
 * \param cmd
 * \param a
 *
 * \retval CLI_SUCCESS on success.
 * \retval NULL when tab completion is used.
 */
static char *handle_feature_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int i;
	struct tris_call_feature *feature;
	struct ao2_iterator iter;
	struct tris_parkinglot *curlot;
#define HFS_FORMAT "%-25s %-7s %-7s\n"

	switch (cmd) {
	
	case CLI_INIT:
		e->command = "features show";
		e->usage =
			"Usage: features show\n"
			"       Lists configured features\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	tris_cli(a->fd, HFS_FORMAT, "Builtin Feature", "Default", "Current");
	tris_cli(a->fd, HFS_FORMAT, "---------------", "-------", "-------");

	tris_cli(a->fd, HFS_FORMAT, "Pickup", "*8", tris_pickup_ext());          /* default hardcoded above, so we'll hardcode it here */

	tris_rwlock_rdlock(&features_lock);
	for (i = 0; i < FEATURES_COUNT; i++)
		tris_cli(a->fd, HFS_FORMAT, builtin_features[i].fname, builtin_features[i].default_exten, builtin_features[i].exten);
	tris_rwlock_unlock(&features_lock);

	tris_cli(a->fd, "\n");
	tris_cli(a->fd, HFS_FORMAT, "Dynamic Feature", "Default", "Current");
	tris_cli(a->fd, HFS_FORMAT, "---------------", "-------", "-------");
	if (TRIS_RWLIST_EMPTY(&feature_list)) {
		tris_cli(a->fd, "(none)\n");
	} else {
		TRIS_RWLIST_RDLOCK(&feature_list);
		TRIS_RWLIST_TRAVERSE(&feature_list, feature, feature_entry) {
			tris_cli(a->fd, HFS_FORMAT, feature->sname, "no def", feature->exten);
		}
		TRIS_RWLIST_UNLOCK(&feature_list);
	}

	// loop through all the parking lots
	iter = ao2_iterator_init(parkinglots, 0);

	while ((curlot = ao2_iterator_next(&iter))) {
		tris_cli(a->fd, "\nCall parking (Parking lot: %s)\n", curlot->name);
		tris_cli(a->fd, "------------\n");
		tris_cli(a->fd,"%-22s:      %s\n", "Parking extension", parking_ext);
		tris_cli(a->fd,"%-22s:      %s\n", "Parking context", curlot->parking_con);
		tris_cli(a->fd,"%-22s:      %d-%d\n", "Parked call extensions", curlot->parking_start, curlot->parking_stop);
		tris_cli(a->fd,"\n");
		ao2_ref(curlot, -1);
	}


	return CLI_SUCCESS;
}

int tris_features_reload(void)
{
	int res;
	/* Release parking lot list */
	//ASTOBJ_CONTAINER_MARKALL(&parkinglots);
	// TODO: I don't think any marking is necessary

	/* Reload configuration */
	res = load_config();
	
	//ASTOBJ_CONTAINER_PRUNE_MARKED(&parkinglots, parkinglot_destroy);
	return res;
}

static char *handle_features_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {	
	case CLI_INIT:
		e->command = "features reload";
		e->usage =
			"Usage: features reload\n"
			"       Reloads configured call features from features.conf\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	tris_features_reload();

	return CLI_SUCCESS;
}

static char mandescr_bridge[] =
"Description: Bridge together two channels already in the PBX\n"
"Variables: ( Headers marked with * are required )\n"
"   *Channel1: Channel to Bridge to Channel2\n"
"   *Channel2: Channel to Bridge to Channel1\n"
"        Tone: (Yes|No) Play courtesy tone to Channel 2\n"
"\n";

/*!
 * \brief Actual bridge
 * \param chan
 * \param tmpchan
 * 
 * Stop hold music, lock both channels, masq channels,
 * after bridge return channel to next priority.
*/
static void do_bridge_masquerade(struct tris_channel *chan, struct tris_channel *tmpchan)
{
	tris_moh_stop(chan);
	tris_channel_lock(chan);
	tris_setstate(tmpchan, chan->_state);
	tmpchan->readformat = chan->readformat;
	tmpchan->writeformat = chan->writeformat;
	tris_channel_masquerade(tmpchan, chan);
	tris_channel_lock(tmpchan);
	tris_do_masquerade(tmpchan);
	/* when returning from bridge, the channel will continue at the next priority */
	tris_explicit_goto(tmpchan, chan->context, chan->exten, chan->priority + 1);
	tris_channel_unlock(tmpchan);
	tris_channel_unlock(chan);
}

/*!
 * \brief Bridge channels together
 * \param s
 * \param m
 * 
 * Make sure valid channels were specified, 
 * send errors if any of the channels could not be found/locked, answer channels if needed,
 * create the placeholder channels and grab the other channels 
 * make the channels compatible, send error if we fail doing so 
 * setup the bridge thread object and start the bridge.
 * 
 * \retval 0 on success or on incorrect use.
 * \retval 1 on failure to bridge channels.
*/
static int action_bridge(struct mansession *s, const struct message *m)
{
	const char *channela = astman_get_header(m, "Channel1");
	const char *channelb = astman_get_header(m, "Channel2");
	const char *playtone = astman_get_header(m, "Tone");
	struct tris_channel *chana = NULL, *chanb = NULL;
	struct tris_channel *tmpchana = NULL, *tmpchanb = NULL;
	struct tris_bridge_thread_obj *tobj = NULL;

	/* make sure valid channels were specified */
	if (tris_strlen_zero(channela) || tris_strlen_zero(channelb)) {
		astman_send_error(s, m, "Missing channel parameter in request");
		return 0;
	}

	/* The same code must be executed for chana and chanb.  To avoid a
	 * theoretical deadlock, this code is separated so both chana and chanb will
	 * not hold locks at the same time. */

	/* Start with chana */
	chana = tris_get_channel_by_name_prefix_locked(channela, strlen(channela));

	/* send errors if any of the channels could not be found/locked */
	if (!chana) {
		char buf[256];
		snprintf(buf, sizeof(buf), "Channel1 does not exists: %s", channela);
		astman_send_error(s, m, buf);
		return 0;
	}

	/* Answer the channels if needed */
	if (chana->_state != TRIS_STATE_UP)
		tris_answer(chana);

	/* create the placeholder channels and grab the other channels */
	if (!(tmpchana = tris_channel_alloc(0, TRIS_STATE_DOWN, NULL, NULL, NULL, 
		NULL, NULL, 0, "Bridge/%s", chana->name))) {
		astman_send_error(s, m, "Unable to create temporary channel!");
		tris_channel_unlock(chana);
		return 1;
	}

	do_bridge_masquerade(chana, tmpchana);
	tris_channel_unlock(chana);
	chana = NULL;

	/* now do chanb */
	chanb = tris_get_channel_by_name_prefix_locked(channelb, strlen(channelb));
	/* send errors if any of the channels could not be found/locked */
	if (!chanb) {
		char buf[256];
		snprintf(buf, sizeof(buf), "Channel2 does not exists: %s", channelb);
		tris_hangup(tmpchana);
		astman_send_error(s, m, buf);
		return 0;
	}

	/* Answer the channels if needed */
	if (chanb->_state != TRIS_STATE_UP)
		tris_answer(chanb);

	/* create the placeholder channels and grab the other channels */
	if (!(tmpchanb = tris_channel_alloc(0, TRIS_STATE_DOWN, NULL, NULL, NULL, 
		NULL, NULL, 0, "Bridge/%s", chanb->name))) {
		astman_send_error(s, m, "Unable to create temporary channels!");
		tris_hangup(tmpchana);
		tris_channel_unlock(chanb);
		return 1;
	}
	do_bridge_masquerade(chanb, tmpchanb);
	tris_channel_unlock(chanb);
	chanb = NULL;

	/* make the channels compatible, send error if we fail doing so */
	if (tris_channel_make_compatible(tmpchana, tmpchanb)) {
		tris_log(LOG_WARNING, "Could not make channels %s and %s compatible for manager bridge\n", tmpchana->name, tmpchanb->name);
		astman_send_error(s, m, "Could not make channels compatible for manager bridge");
		tris_hangup(tmpchana);
		tris_hangup(tmpchanb);
		return 1;
	}

	/* setup the bridge thread object and start the bridge */
	if (!(tobj = tris_calloc(1, sizeof(*tobj)))) {
		tris_log(LOG_WARNING, "Unable to spawn a new bridge thread on %s and %s: %s\n", tmpchana->name, tmpchanb->name, strerror(errno));
		astman_send_error(s, m, "Unable to spawn a new bridge thread");
		tris_hangup(tmpchana);
		tris_hangup(tmpchanb);
		return 1;
	}

	tobj->chan = tmpchana;
	tobj->peer = tmpchanb;
	tobj->return_to_pbx = 1;

	if (tris_true(playtone)) {
		if (!tris_strlen_zero(xfersound) && !tris_streamfile(tmpchanb, xfersound, tmpchanb->language)) {
			if (tris_waitstream(tmpchanb, "") < 0)
				tris_log(LOG_WARNING, "Failed to play a courtesy tone on chan %s\n", tmpchanb->name);
		}
	}

	bridge_call_thread_launch(tobj);

	astman_send_ack(s, m, "Launched bridge thread with success");

	return 0;
}

/*!
 * \brief CLI command to list parked calls
 * \param e 
 * \param cmd
 * \param a
 *  
 * Check right usage, lock parking lot, display parked calls, unlock parking lot list.
 * \retval CLI_SUCCESS on success.
 * \retval CLI_SHOWUSAGE on incorrect number of arguments.
 * \retval NULL when tab completion is used.
*/
static char *handle_parkedcalls(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct parkeduser *cur;
	int numparked = 0;
	struct ao2_iterator iter;
	struct tris_parkinglot *curlot;

	switch (cmd) {
	case CLI_INIT:
		e->command = "parkedcalls show";
		e->usage =
			"Usage: parkedcalls show\n"
			"       List currently parked calls\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc > e->args)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "%4s %25s (%-15s %-12s %-4s) %-6s \n", "Num", "Channel"
		, "Context", "Extension", "Pri", "Timeout");

	iter = ao2_iterator_init(parkinglots, 0);
	while ((curlot = ao2_iterator_next(&iter))) {
		int lotparked = 0;
		tris_cli(a->fd, "*** Parking lot: %s\n", curlot->name);

		TRIS_LIST_LOCK(&curlot->parkings);
		TRIS_LIST_TRAVERSE(&curlot->parkings, cur, list) {
			tris_cli(a->fd, "%-10.10s %25s (%-15s %-12s %-4d) %6lds\n"
				,cur->parkingexten, cur->chan->name, cur->context, cur->exten
				,cur->priority,
				(long)(cur->start.tv_sec + (cur->parkingtime/1000) - time(NULL)) );
			numparked++;
			numparked += lotparked;
		}
		TRIS_LIST_UNLOCK(&curlot->parkings);
		if (lotparked)
			tris_cli(a->fd, "   %d parked call%s in parking lot %s\n", lotparked, ESS(lotparked), curlot->name);

		ao2_ref(curlot, -1);
	}

	tris_cli(a->fd, "---\n%d parked call%s in total.\n", numparked, ESS(numparked));

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_features[] = {
	TRIS_CLI_DEFINE(handle_feature_show, "Lists configured features"),
	TRIS_CLI_DEFINE(handle_features_reload, "Reloads configured features"),
	TRIS_CLI_DEFINE(handle_parkedcalls, "List currently parked calls"),
};

/*! 
 * \brief Dump parking lot status
 * \param s
 * \param m
 * 
 * Lock parking lot, iterate list and append parked calls status, unlock parking lot.
 * \return Always RESULT_SUCCESS 
*/
static int manager_parking_status(struct mansession *s, const struct message *m)
{
	struct parkeduser *cur;
	const char *id = astman_get_header(m, "ActionID");
	char idText[256] = "";
	struct ao2_iterator iter;
	struct tris_parkinglot *curlot;

	if (!tris_strlen_zero(id))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", id);

	astman_send_ack(s, m, "Parked calls will follow");

	iter = ao2_iterator_init(parkinglots, 0);
	while ((curlot = ao2_iterator_next(&iter))) {

		TRIS_LIST_LOCK(&curlot->parkings);
		TRIS_LIST_TRAVERSE(&curlot->parkings, cur, list) {
			astman_append(s, "Event: ParkedCall\r\n"
				"Exten: %d\r\n"
				"Channel: %s\r\n"
				"From: %s\r\n"
				"Timeout: %ld\r\n"
				"CallerIDNum: %s\r\n"
				"CallerIDName: %s\r\n"
				"%s"
				"\r\n",
				cur->parkingnum, cur->chan->name, cur->peername,
				(long) cur->start.tv_sec + (long) (cur->parkingtime / 1000) - (long) time(NULL),
				S_OR(cur->chan->cid.cid_num, ""),	/* XXX in other places it is <unknown> */
				S_OR(cur->chan->cid.cid_name, ""),
				idText);
		}
		TRIS_LIST_UNLOCK(&curlot->parkings);
		ao2_ref(curlot, -1);
	}

	astman_append(s,
		"Event: ParkedCallsComplete\r\n"
		"%s"
		"\r\n",idText);


	return RESULT_SUCCESS;
}

static char mandescr_park[] =
"Description: Park a channel.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel name to park\n"
"	*Channel2: Channel to announce park info to (and return to if timeout)\n"
"	Timeout: Number of milliseconds to wait before callback.\n";  

/*!
 * \brief Create manager event for parked calls
 * \param s
 * \param m
 *
 * Get channels involved in park, create event.
 * \return Always 0
*/
static int manager_park(struct mansession *s, const struct message *m)
{
	const char *channel = astman_get_header(m, "Channel");
	const char *channel2 = astman_get_header(m, "Channel2");
	const char *timeout = astman_get_header(m, "Timeout");
	char buf[BUFSIZ];
	int to = 0;
	int res = 0;
	int parkExt = 0;
	struct tris_channel *ch1, *ch2;

	if (tris_strlen_zero(channel)) {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}

	if (tris_strlen_zero(channel2)) {
		astman_send_error(s, m, "Channel2 not specified");
		return 0;
	}

	ch1 = tris_get_channel_by_name_locked(channel);
	if (!ch1) {
		snprintf(buf, sizeof(buf), "Channel does not exist: %s", channel);
		astman_send_error(s, m, buf);
		return 0;
	}

	ch2 = tris_get_channel_by_name_locked(channel2);
	if (!ch2) {
		snprintf(buf, sizeof(buf), "Channel does not exist: %s", channel2);
		astman_send_error(s, m, buf);
		tris_channel_unlock(ch1);
		return 0;
	}

	if (!tris_strlen_zero(timeout)) {
		sscanf(timeout, "%30d", &to);
	}

	res = tris_masq_park_call(ch1, ch2, to, &parkExt);
	if (!res) {
		tris_softhangup(ch2, TRIS_SOFTHANGUP_EXPLICIT);
		astman_send_ack(s, m, "Park successful");
	} else {
		astman_send_error(s, m, "Park failure");
	}

	tris_channel_unlock(ch1);
	tris_channel_unlock(ch2);

	return 0;
}

static int find_channel_by_group(struct tris_channel *c, void *data) {
	struct tris_channel *chan = data;

	return !c->pbx &&
		/* Accessing 'chan' here is safe without locking, because there is no way for
		   the channel do disappear from under us at this point.  pickupgroup *could*
		   change while we're here, but that isn't a problem. */
		(c != chan) &&
		(chan->pickupgroup & c->callgroup) &&
		((c->_state == TRIS_STATE_RINGING) || (c->_state == TRIS_STATE_RING));
}

/*!
 * \brief Pickup a call
 * \param chan channel that initiated pickup.
 *
 * Walk list of channels, checking it is not itself, channel is pbx one,
 * check that the callgroup for both channels are the same and the channel is ringing.
 * Answer calling channel, flag channel as answered on queue, masq channels together.
*/
int tris_pickup_call(struct tris_channel *chan)
{
	struct tris_channel *cur = tris_channel_search_locked(find_channel_by_group, chan);

	if (cur) {
		int res = -1;
		tris_debug(1, "Call pickup on chan '%s' by '%s'\n",cur->name, chan->name);
		res = tris_answer(chan);
		if (res)
			tris_log(LOG_WARNING, "Unable to answer '%s'\n", chan->name);
		res = tris_queue_control(chan, TRIS_CONTROL_ANSWER);
		if (res)
			tris_log(LOG_WARNING, "Unable to queue answer on '%s'\n", chan->name);
		res = tris_channel_masquerade(cur, chan);
		if (res)
			tris_log(LOG_WARNING, "Unable to masquerade '%s' into '%s'\n", chan->name, cur->name);		/* Done */
		if (!tris_strlen_zero(pickupsound)) {
			tris_stream_and_wait(cur, pickupsound, "");
		}
		tris_channel_unlock(cur);
		return res;
	} else	{
		tris_debug(1, "No call pickup possible...\n");
		if (!tris_strlen_zero(pickupfailsound)) {
			tris_stream_and_wait(chan, pickupfailsound, "");
		}
	}
	return -1;
}

static char *app_bridge = "Bridge";

enum {
	BRIDGE_OPT_PLAYTONE = (1 << 0),
};

TRIS_APP_OPTIONS(bridge_exec_options, BEGIN_OPTIONS
	TRIS_APP_OPTION('p', BRIDGE_OPT_PLAYTONE)
END_OPTIONS );

/*!
 * \brief Bridge channels
 * \param chan
 * \param data channel to bridge with.
 * 
 * Split data, check we aren't bridging with ourself, check valid channel,
 * answer call if not already, check compatible channels, setup bridge config
 * now bridge call, if transfered party hangs up return to PBX extension.
*/
static int bridge_exec(struct tris_channel *chan, void *data)
{
	struct tris_channel *current_dest_chan, *final_dest_chan;
	char *tmp_data  = NULL;
	struct tris_flags opts = { 0, };
	struct tris_bridge_config bconfig = { { 0, }, };

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(dest_chan);
		TRIS_APP_ARG(options);
	);
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Bridge require at least 1 argument specifying the other end of the bridge\n");
		return -1;
	}

	tmp_data = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, tmp_data);
	if (!tris_strlen_zero(args.options))
		tris_app_parse_options(bridge_exec_options, &opts, NULL, args.options);

	/* avoid bridge with ourselves */
	if (!strcmp(chan->name, args.dest_chan)) {
		tris_log(LOG_WARNING, "Unable to bridge channel %s with itself\n", chan->name);
		manager_event(EVENT_FLAG_CALL, "BridgeExec",
					"Response: Failed\r\n"
					"Reason: Unable to bridge channel to itself\r\n"
					"Channel1: %s\r\n"
					"Channel2: %s\r\n",
					chan->name, args.dest_chan);
		pbx_builtin_setvar_helper(chan, "BRIDGERESULT", "LOOP");
		return 0;
	}

	/* make sure we have a valid end point */
	if (!(current_dest_chan = tris_get_channel_by_name_prefix_locked(args.dest_chan, 
		strlen(args.dest_chan)))) {
		tris_log(LOG_WARNING, "Bridge failed because channel %s does not exists or we "
			"cannot get its lock\n", args.dest_chan);
		manager_event(EVENT_FLAG_CALL, "BridgeExec",
					"Response: Failed\r\n"
					"Reason: Cannot grab end point\r\n"
					"Channel1: %s\r\n"
					"Channel2: %s\r\n", chan->name, args.dest_chan);
		pbx_builtin_setvar_helper(chan, "BRIDGERESULT", "NONEXISTENT");
		return 0;
	}

	/* answer the channel if needed */
	if (current_dest_chan->_state != TRIS_STATE_UP)
		tris_answer(current_dest_chan);

	/* try to allocate a place holder where current_dest_chan will be placed */
	if (!(final_dest_chan = tris_channel_alloc(0, TRIS_STATE_DOWN, NULL, NULL, NULL, 
		NULL, NULL, 0, "Bridge/%s", current_dest_chan->name))) {
		tris_log(LOG_WARNING, "Cannot create placeholder channel for chan %s\n", args.dest_chan);
		manager_event(EVENT_FLAG_CALL, "BridgeExec",
					"Response: Failed\r\n"
					"Reason: cannot create placeholder\r\n"
					"Channel1: %s\r\n"
					"Channel2: %s\r\n", chan->name, args.dest_chan);
	}
	do_bridge_masquerade(current_dest_chan, final_dest_chan);

	tris_channel_unlock(current_dest_chan);

	/* now current_dest_chan is a ZOMBIE and with softhangup set to 1 and final_dest_chan is our end point */
	/* try to make compatible, send error if we fail */
	if (tris_channel_make_compatible(chan, final_dest_chan) < 0) {
		tris_log(LOG_WARNING, "Could not make channels %s and %s compatible for bridge\n", chan->name, final_dest_chan->name);
		manager_event(EVENT_FLAG_CALL, "BridgeExec",
					"Response: Failed\r\n"
					"Reason: Could not make channels compatible for bridge\r\n"
					"Channel1: %s\r\n"
					"Channel2: %s\r\n", chan->name, final_dest_chan->name);
		tris_hangup(final_dest_chan); /* may be we should return this channel to the PBX? */
		pbx_builtin_setvar_helper(chan, "BRIDGERESULT", "INCOMPATIBLE");
		return 0;
	}

	/* Report that the bridge will be successfull */
	manager_event(EVENT_FLAG_CALL, "BridgeExec",
				"Response: Success\r\n"
				"Channel1: %s\r\n"
				"Channel2: %s\r\n", chan->name, final_dest_chan->name);

	/* we have 2 valid channels to bridge, now it is just a matter of setting up the bridge config and starting the bridge */	
	if (tris_test_flag(&opts, BRIDGE_OPT_PLAYTONE) && !tris_strlen_zero(xfersound)) {
		if (!tris_streamfile(final_dest_chan, xfersound, final_dest_chan->language)) {
			if (tris_waitstream(final_dest_chan, "") < 0)
				tris_log(LOG_WARNING, "Failed to play courtesy tone on %s\n", final_dest_chan->name);
		}
	}
	
	/* do the bridge */
	tris_bridge_call(chan, final_dest_chan, &bconfig);

	/* the bridge has ended, set BRIDGERESULT to SUCCESS. If the other channel has not been hung up, return it to the PBX */
	pbx_builtin_setvar_helper(chan, "BRIDGERESULT", "SUCCESS");
	if (!tris_check_hangup(final_dest_chan)) {
		tris_debug(1, "starting new PBX in %s,%s,%d for chan %s\n", 
			final_dest_chan->context, final_dest_chan->exten, 
			final_dest_chan->priority, final_dest_chan->name);

		if (tris_pbx_start(final_dest_chan) != TRIS_PBX_SUCCESS) {
			tris_log(LOG_WARNING, "FAILED continuing PBX on dest chan %s\n", final_dest_chan->name);
			tris_hangup(final_dest_chan);
		} else
			tris_debug(1, "SUCCESS continuing PBX on chan %s\n", final_dest_chan->name);
	} else {
		tris_debug(1, "hangup chan %s since the other endpoint has hung up\n", final_dest_chan->name);
		tris_hangup(final_dest_chan);
	}

	return 0;
}

int tris_features_init(void)
{
	int res;

	tris_register_application2(app_bridge, bridge_exec, NULL, NULL, NULL);

	parkinglots = ao2_container_alloc(7, parkinglot_hash_cb, parkinglot_cmp_cb);

	if ((res = load_config()))
		return res;
	tris_cli_register_multiple(cli_features, ARRAY_LEN(cli_features));
	tris_pthread_create(&parking_thread, NULL, do_parking_thread, NULL);
	res = tris_register_application2(parkedcall, park_exec, NULL, NULL, NULL);
	if (!res)
		res = tris_register_application2(parkcall, park_call_exec, NULL, NULL, NULL);
	if (!res) {
		tris_manager_register("ParkedCalls", 0, manager_parking_status, "List parked calls");
		tris_manager_register2("Park", EVENT_FLAG_CALL, manager_park, "Park a channel", mandescr_park); 
		tris_manager_register2("Bridge", EVENT_FLAG_CALL, action_bridge, "Bridge two channels already in the PBX", mandescr_bridge);
	}

	res |= tris_devstate_prov_add("Park", metermaidstate);

	return res;
}
