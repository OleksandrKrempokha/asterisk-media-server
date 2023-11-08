/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2007, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * SLA Implementation by:
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
 * \brief Meet me conference bridge and Shared Line Appearances
 *
 * \author Mark Spencer <markster@digium.com>
 * \author (SLA) Russell Bryant <russell@digium.com>
 * 
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>dahdi</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 238185 $")

#include <dahdi/user.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/app.h"
#include "trismedia/dsp.h"
#include "trismedia/musiconhold.h"
#include "trismedia/manager.h"
#include "trismedia/cli.h"
#include "trismedia/say.h"
#include "trismedia/utils.h"
#include "trismedia/translate.h"
#include "trismedia/ulaw.h"
#include "trismedia/astobj2.h"
#include "trismedia/devicestate.h"
#include "trismedia/dial.h"
#include "trismedia/causes.h"
#include "trismedia/paths.h"
#include "trismedia/acl.h"
#include "trismedia/res_odbc.h"

#include "enter.h"
#include "leave.h"

/*** DOCUMENTATION
	<application name="MeetMe" language="en_US">
		<synopsis>
			MeetMe conference bridge.
		</synopsis>
		<syntax>
			<parameter name="confno">
				<para>The conference number</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="a">
						<para>Set admin mode.</para>
					</option>
					<option name="A">
						<para>Set marked mode.</para>
					</option>
					<option name="b">
						<para>Run AGI script specified in <variable>MEETME_AGI_BACKGROUND</variable>
						Default: <literal>conf-background.agi</literal>.</para>
						<note><para>This does not work with non-DAHDI channels in the same
						conference).</para></note>
					</option>
					<option name="c">
						<para>Announce user(s) count on joining a conference.</para>
					</option>
					<option name="C">
						<para>Continue in dialplan when kicked out of conference.</para>
					</option>
					<option name="d">
						<para>Dynamically add conference.</para>
					</option>
					<option name="D">
						<para>Dynamically add conference, prompting for a PIN.</para>
					</option>
					<option name="e">
						<para>Select an empty conference.</para>
					</option>
					<option name="E">
						<para>Select an empty pinless conference.</para>
					</option>
					<option name="F">
						<para>Pass DTMF through the conference.</para>
					</option>
					<option name="i">
						<para>Announce user join/leave with review.</para>
					</option>
					<option name="I">
						<para>Announce user join/leave without review.</para>
					</option>
					<option name="l">
						<para>Set listen only mode (Listen only, no talking).</para>
					</option>
					<option name="m">
						<para>Set initially muted.</para>
					</option>
					<option name="M" hasparams="optional">
						<para>Enable music on hold when the conference has a single caller. Optionally,
						specify a musiconhold class to use. If one is not provided, it will use the
						channel's currently set music class, or <literal>default</literal>.</para>
						<argument name="class" required="true" />
					</option>
					<option name="o">
						<para>Set talker optimization - treats talkers who aren't speaking as
						being muted, meaning (a) No encode is done on transmission and (b)
						Received audio that is not registered as talking is omitted causing no
						buildup in background noise.</para>
					</option>
					<option name="p" hasparams="optional">
						<para>Allow user to exit the conference by pressing <literal>#</literal> (default)
						or any of the defined keys. If keys contain <literal>*</literal> this will override
						option <literal>s</literal>. The key used is set to channel variable
						<variable>MEETME_EXIT_KEY</variable>.</para>
						<argument name="keys" required="true" />
					</option>
					<option name="P">
						<para>Always prompt for the pin even if it is specified.</para>
					</option>
					<option name="q">
						<para>Quiet mode (don't play enter/leave sounds).</para>
					</option>
					<option name="r">
						<para>Record conference (records as <variable>MEETME_RECORDINGFILE</variable>
						using format <variable>MEETME_RECORDINGFORMAT</variable>. Default filename is
						<literal>meetme-conf-rec-${CONFNO}-${UNIQUEID}</literal> and the default format is
						wav.</para>
					</option>
					<option name="s">
						<para>Present menu (user or admin) when <literal>*</literal> is received
						(send to menu).</para>
					</option>
					<option name="t">
						<para>Set talk only mode. (Talk only, no listening).</para>
					</option>
					<option name="T">
						<para>Set talker detection (sent to manager interface and meetme list).</para>
					</option>
					<option name="W" hasparams="optional">
						<para>Wait until the marked user enters the conference.</para>
						<argument name="secs" required="true" />
					</option>
					<option name="x">
						<para>Close the conference when last marked user exits</para>
					</option>
					<option name="X">
						<para>Allow user to exit the conference by entering a valid single digit
						extension <variable>MEETME_EXIT_CONTEXT</variable> or the current context
						if that variable is not defined.</para>
					</option>
					<option name="1">
						<para>Do not play message when first person enters</para>
					</option>
					<option name="S">
						<para>Kick the user <replaceable>x</replaceable> seconds <emphasis>after</emphasis> he entered into
						the conference.</para>
						<argument name="x" required="true" />
					</option>
					<option name="L" argsep=":">
						<para>Limit the conference to <replaceable>x</replaceable> ms. Play a warning when
						<replaceable>y</replaceable> ms are left. Repeat the warning every <replaceable>z</replaceable> ms.
						The following special variables can be used with this option:</para>
						<variablelist>
							<variable name="CONF_LIMIT_TIMEOUT_FILE">
								<para>File to play when time is up.</para>
							</variable>
							<variable name="CONF_LIMIT_WARNING_FILE">
								<para>File to play as warning if <replaceable>y</replaceable> is defined. The
								default is to say the time remaining.</para>
							</variable>
						</variablelist>
						<argument name="x" />
						<argument name="y" />
						<argument name="z" />
					</option>
				</optionlist>
			</parameter>
			<parameter name="pin" />
		</syntax>
		<description>
			<para>Enters the user into a specified MeetMe conference.  If the <replaceable>confno</replaceable>
			is omitted, the user will be prompted to enter one.  User can exit the conference by hangup, or
			if the <literal>p</literal> option is specified, by pressing <literal>#</literal>.</para>
			<note><para>The DAHDI kernel modules and at least one hardware driver (or dahdi_dummy)
			must be present for conferencing to operate properly. In addition, the chan_dahdi channel driver
			must be loaded for the <literal>i</literal> and <literal>r</literal> options to operate at
			all.</para></note>
		</description>
		<see-also>
			<ref type="application">MeetMeCount</ref>
			<ref type="application">MeetMeAdmin</ref>
			<ref type="application">MeetMeChannelAdmin</ref>
		</see-also>
	</application>
	<application name="MeetMeCount" language="en_US">
		<synopsis>
			MeetMe participant count.
		</synopsis>
		<syntax>
			<parameter name="confno" required="true">
				<para>Conference number.</para>
			</parameter>
			<parameter name="var" />
		</syntax>
		<description>
			<para>Plays back the number of users in the specified MeetMe conference.
			If <replaceable>var</replaceable> is specified, playback will be skipped and the value
			will be returned in the variable. Upon application completion, MeetMeCount will hangup
			the channel, unless priority <literal>n+1</literal> exists, in which case priority progress will
			continue.</para>
		</description>
		<see-also>
			<ref type="application">MeetMe</ref>
		</see-also>
	</application>
	<application name="MeetMeAdmin" language="en_US">
		<synopsis>
			MeetMe conference administration.
		</synopsis>
		<syntax>
			<parameter name="confno" required="true" />
			<parameter name="command" required="true">
				<optionlist>
					<option name="e">
						<para>Eject last user that joined.</para>
					</option>
					<option name="E">
						<para>Extend conference end time, if scheduled.</para>
					</option>
					<option name="k">
						<para>Kick one user out of conference.</para>
					</option>
					<option name="K">
						<para>Kick all users out of conference.</para>
					</option>
					<option name="l">
						<para>Unlock conference.</para>
					</option>
					<option name="L">
						<para>Lock conference.</para>
					</option>
					<option name="m">
						<para>Unmute one user.</para>
					</option>
					<option name="M">
						<para>Mute one user.</para>
					</option>
					<option name="n">
						<para>Unmute all users in the conference.</para>
					</option>
					<option name="N">
						<para>Mute all non-admin users in the conference.</para>
					</option>
					<option name="r">
						<para>Reset one user's volume settings.</para>
					</option>
					<option name="R">
						<para>Reset all users volume settings.</para>
					</option>
					<option name="s">
						<para>Lower entire conference speaking volume.</para>
					</option>
					<option name="S">
						<para>Raise entire conference speaking volume.</para>
					</option>
					<option name="t">
						<para>Lower one user's talk volume.</para>
					</option>
					<option name="T">
						<para>Raise one user's talk volume.</para>
					</option>
					<option name="u">
						<para>Lower one user's listen volume.</para>
					</option>
					<option name="U">
						<para>Raise one user's listen volume.</para>
					</option>
					<option name="v">
						<para>Lower entire conference listening volume.</para>
					</option>
					<option name="V">
						<para>Raise entire conference listening volume.</para>
					</option>
				</optionlist>
			</parameter>
			<parameter name="user" />
		</syntax>
		<description>
			<para>Run admin <replaceable>command</replaceable> for conference <replaceable>confno</replaceable>.</para>
			<para>Will additionally set the variable <variable>MEETMEADMINSTATUS</variable> with one of
			the following values:</para>
			<variablelist>
				<variable name="MEETMEADMINSTATUS">
					<value name="NOPARSE">
						Invalid arguments.
					</value>
					<value name="NOTFOUND">
						User specified was not found.
					</value>
					<value name="FAILED">
						Another failure occurred.
					</value>
					<value name="OK">
						The operation was completed successfully.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">MeetMe</ref>
		</see-also>
	</application>
	<application name="MeetMeChannelAdmin" language="en_US">
		<synopsis>
			MeetMe conference Administration (channel specific).
		</synopsis>
		<syntax>
			<parameter name="channel" required="true" />
			<parameter name="command" required="true">
				<optionlist>
					<option name="k">
						<para>Kick the specified user out of the conference he is in.</para>
					</option>
					<option name="m">
						<para>Unmute the specified user.</para>
					</option>
					<option name="M">
						<para>Mute the specified user.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Run admin <replaceable>command</replaceable> for a specific
			<replaceable>channel</replaceable> in any coference.</para>
		</description>
	</application>
	<application name="SLAStation" language="en_US">
		<synopsis>
			Shared Line Appearance Station.
		</synopsis>
		<syntax>
			<parameter name="station" required="true">
				<para>Station name</para>
			</parameter>
		</syntax>
		<description>
			<para>This application should be executed by an SLA station. The argument depends
			on how the call was initiated. If the phone was just taken off hook, then the argument
			<replaceable>station</replaceable> should be just the station name. If the call was
			initiated by pressing a line key, then the station name should be preceded by an underscore
			and the trunk name associated with that line button.</para>
			<para>For example: <literal>station1_line1</literal></para>
			<para>On exit, this application will set the variable <variable>SLASTATION_STATUS</variable> to
			one of the following values:</para>
			<variablelist>
				<variable name="SLASTATION_STATUS">
					<value name="FAILURE" />
					<value name="CONGESTION" />
					<value name="SUCCESS" />
				</variable>
			</variablelist>
		</description>
	</application>
	<application name="SLATrunk" language="en_US">
		<synopsis>
			Shared Line Appearance Trunk.
		</synopsis>
		<syntax>
			<parameter name="trunk" required="true">
				<para>Trunk name</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="M" hasparams="optional">
						<para>Play back the specified MOH <replaceable>class</replaceable>
						instead of ringing</para>
						<argument name="class" required="true" />
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>This application should be executed by an SLA trunk on an inbound call. The channel calling
			this application should correspond to the SLA trunk with the name <replaceable>trunk</replaceable>
			that is being passed as an argument.</para>
			<para>On exit, this application will set the variable <variable>SLATRUNK_STATUS</variable> to
			one of the following values:</para>
			<variablelist>
				<variable name="SLATRUNK_STATUS">
					<value name="FAILURE" />
					<value name="SUCCESS" />
					<value name="UNANSWERED" />
					<value name="RINGTIMEOUT" />
				</variable>
			</variablelist>
		</description>
	</application>
 ***/

#define CONFIG_FILE_NAME "meetme.conf"
#define SLA_CONFIG_FILE  "sla.conf"

/*! each buffer is 20ms, so this is 640ms total */
#define DEFAULT_AUDIO_BUFFERS  32
#define DEFAULT_MAX_ROOMS  10

#define MAX_DIALS 256

/*! String format for scheduled conferences */
#define DATE_FORMAT "%Y-%m-%d %H:%M:%S"

enum {
	ADMINFLAG_MUTED =     (1 << 1), /*!< User is muted */
	ADMINFLAG_SELFMUTED = (1 << 2), /*!< User muted self */
	ADMINFLAG_KICKME =    (1 << 3),  /*!< User has been kicked */
	/*! User has requested to speak */
	ADMINFLAG_T_REQUEST = (1 << 4),
	ADMINFLAG_ENDCONF = (1 << 5),
	ADMINFLAG_RECORDCONF = (1 << 6),
};

#define MEETME_DELAYDETECTTALK     300
#define MEETME_DELAYDETECTENDTALK  1000

#define TRIS_FRAME_BITS  32

enum volume_action {
	VOL_UP,
	VOL_DOWN
};

enum entrance_sound {
	ENTER,
	LEAVE
};

enum recording_state {
	MEETME_RECORD_OFF,
	MEETME_RECORD_STARTED,
	MEETME_RECORD_ACTIVE,
	MEETME_RECORD_TERMINATE
};

#define CONF_SIZE  320

enum {
	/*! user has admin access on the conference */
	CONFFLAG_ADMIN = (1 << 0),
	/*! If set the user can only receive audio from the conference */
	CONFFLAG_MONITOR = (1 << 1),
	/*! If set trismedia will exit conference when key defined in p() option is pressed */
	CONFFLAG_KEYEXIT = (1 << 2),
	/*! If set trismedia will provide a menu to the user when '*' is pressed */
	CONFFLAG_STARMENU = (1 << 3),
	/*! If set the use can only send audio to the conference */
	CONFFLAG_TALKER = (1 << 4),
	/*! If set there will be no enter or leave sounds */
	CONFFLAG_QUIET = (1 << 5),
	/*! If set, when user joins the conference, they will be told the number 
	 *  of users that are already in */
	CONFFLAG_ANNOUNCEUSERCOUNT = (1 << 6),
	/*! Set to run AGI Script in Background */
	CONFFLAG_AGI = (1 << 7),
	/*! Set to have music on hold when user is alone in conference */
	CONFFLAG_MOH = (1 << 8),
	/*! If set the MeetMe will return if all marked with this flag left */
	CONFFLAG_MARKEDEXIT = (1 << 9),
	/*! If set, the MeetMe will wait until a marked user enters */
	CONFFLAG_WAITMARKED = (1 << 10),
	/*! If set, the MeetMe will exit to the specified context */
	CONFFLAG_EXIT_CONTEXT = (1 << 11),
	/*! If set, the user will be marked */
	CONFFLAG_MARKEDUSER = (1 << 12),
	/*! If set, user will be ask record name on entry of conference */
	CONFFLAG_INTROUSER = (1 << 13),
	/*! If set, the MeetMe will be recorded */
	CONFFLAG_RECORDCONF = (1<< 14),
	/*! If set, the user will be monitored if the user is talking or not */
	CONFFLAG_MONITORTALKER = (1 << 15),
	CONFFLAG_DYNAMIC = (1 << 16),
	CONFFLAG_DYNAMICPIN = (1 << 17),
	CONFFLAG_EMPTY = (1 << 18),
	CONFFLAG_EMPTYNOPIN = (1 << 19),
	CONFFLAG_ALWAYSPROMPT = (1 << 20),
	/*! If set, treat talking users as muted users */
	CONFFLAG_OPTIMIZETALKER = (1 << 21),
	/*! If set, won't speak the extra prompt when the first person 
	 *  enters the conference */
	CONFFLAG_NOONLYPERSON = (1 << 22),
	/*! If set, user will be asked to record name on entry of conference 
	 *  without review */
	CONFFLAG_INTROUSERNOREVIEW = (1 << 23),
	/*! If set, the user will be initially self-muted */
	CONFFLAG_STARTMUTED = (1 << 24),
	/*! Pass DTMF through the conference */
	CONFFLAG_PASS_DTMF = (1 << 25),
	CONFFLAG_SLA_STATION = (1 << 26),
	/*! If set, the user should continue in the dialplan if kicked out */
	CONFFLAG_KICK_CONTINUE = (1 << 27),
	CONFFLAG_DURATION_STOP = (1 << 28),
	CONFFLAG_DURATION_LIMIT = (1 << 29),
	/*! Do not write any audio to this channel until the state is up. */
	CONFFLAG_DIALOUT = (1 << 30),
	CONFFLAG_NO_AUDIO_UNTIL_UP = (1 << 31),
};

enum {
	OPT_ARG_WAITMARKED = 0,
	OPT_ARG_EXITKEYS   = 1,
	OPT_ARG_DURATION_STOP = 2,
	OPT_ARG_DURATION_LIMIT = 3,
	OPT_ARG_MOH_CLASS = 4,
	OPT_ARG_DIALOUT_MAINCONFID = 5,
	OPT_ARG_ARRAY_SIZE = 6,
};

TRIS_APP_OPTIONS(meetme_opts, BEGIN_OPTIONS
	TRIS_APP_OPTION('A', CONFFLAG_MARKEDUSER ),
	TRIS_APP_OPTION('a', CONFFLAG_ADMIN ),
	TRIS_APP_OPTION('b', CONFFLAG_AGI ),
	TRIS_APP_OPTION('c', CONFFLAG_ANNOUNCEUSERCOUNT ),
	TRIS_APP_OPTION('C', CONFFLAG_KICK_CONTINUE),
	TRIS_APP_OPTION('D', CONFFLAG_DYNAMICPIN ),
	TRIS_APP_OPTION('d', CONFFLAG_DYNAMIC ),
	TRIS_APP_OPTION('E', CONFFLAG_EMPTYNOPIN ),
	TRIS_APP_OPTION('e', CONFFLAG_EMPTY ),
	TRIS_APP_OPTION('F', CONFFLAG_PASS_DTMF ),
	TRIS_APP_OPTION('i', CONFFLAG_INTROUSER ),
	TRIS_APP_OPTION('I', CONFFLAG_INTROUSERNOREVIEW ),
	TRIS_APP_OPTION_ARG('M', CONFFLAG_MOH, OPT_ARG_MOH_CLASS ),
	TRIS_APP_OPTION('m', CONFFLAG_STARTMUTED ),
	TRIS_APP_OPTION('O', CONFFLAG_OPTIMIZETALKER ),
	TRIS_APP_OPTION_ARG('o', CONFFLAG_DIALOUT, OPT_ARG_DIALOUT_MAINCONFID),
	TRIS_APP_OPTION('P', CONFFLAG_ALWAYSPROMPT ),
	TRIS_APP_OPTION_ARG('p', CONFFLAG_KEYEXIT, OPT_ARG_EXITKEYS ),
	TRIS_APP_OPTION('q', CONFFLAG_QUIET ),
	TRIS_APP_OPTION('r', CONFFLAG_RECORDCONF ),
	TRIS_APP_OPTION('s', CONFFLAG_STARMENU ),
	TRIS_APP_OPTION('T', CONFFLAG_MONITORTALKER ),
	TRIS_APP_OPTION('l', CONFFLAG_MONITOR ),
	TRIS_APP_OPTION('t', CONFFLAG_TALKER ),
	TRIS_APP_OPTION_ARG('w', CONFFLAG_WAITMARKED, OPT_ARG_WAITMARKED ),
	TRIS_APP_OPTION('X', CONFFLAG_EXIT_CONTEXT ),
	TRIS_APP_OPTION('x', CONFFLAG_MARKEDEXIT ),
	TRIS_APP_OPTION('1', CONFFLAG_NOONLYPERSON ),
 	TRIS_APP_OPTION_ARG('S', CONFFLAG_DURATION_STOP, OPT_ARG_DURATION_STOP),
	TRIS_APP_OPTION_ARG('L', CONFFLAG_DURATION_LIMIT, OPT_ARG_DURATION_LIMIT),
END_OPTIONS );

static const char *app = "MeetMe";
static const char *app2 = "MeetMeCount";
static const char *app3 = "MeetMeAdmin";
static const char *app4 = "MeetMeChannelAdmin";
static const char *app5 = "ScheduleConf";
static const char *app6 = "UrgencyConf";
static const char *slastation_app = "SLAStation";
static const char *slatrunk_app = "SLATrunk";

/* Lookup RealTime conferences based on confno and current time */
static int rt_schedule;
static int fuzzystart;
static int earlyalert;
static int endalert;
static int extendby;


/* Log participant count to the RealTime backend */
static int rt_log_members;

#define MAX_CONFNUM 80
#define MAX_PIN     80
#define OPTIONS_LEN 100

/* Enough space for "<conference #>,<pin>,<admin pin>" followed by a 0 byte. */
#define MAX_SETTINGS (MAX_CONFNUM + MAX_PIN + MAX_PIN + 3)

enum announcetypes {
	CONF_HASJOIN,
	CONF_HASLEFT
};

struct announce_listitem {
	TRIS_LIST_ENTRY(announce_listitem) entry;
	char namerecloc[PATH_MAX];				/*!< Name Recorded file Location */
	char language[MAX_LANGUAGE];
	struct tris_channel *confchan;
	char exten[80];
	int confusers;
	enum announcetypes announcetype;
};

/*! \brief The MeetMe Conference object */
struct tris_conference {
	tris_mutex_t playlock;                   /*!< Conference specific lock (players) */
	tris_mutex_t listenlock;                 /*!< Conference specific lock (listeners) */
	char confno[MAX_CONFNUM];               /*!< Conference */
	struct tris_channel *admin_chan;
	struct tris_channel *chan;               /*!< Announcements channel */
	struct tris_channel *lchan;              /*!< Listen/Record channel */
	int fd;                                 /*!< Announcements fd */
	int dahdiconf;                            /*!< DAHDI Conf # */
	int users;                              /*!< Number of active users */
	int markedusers;                        /*!< Number of marked users */
	int maxusers;                           /*!< Participant limit if scheduled */
	int endalert;                           /*!< When to play conf ending message */
	time_t start;                           /*!< Start time (s) */
	int refcount;                           /*!< reference count of usage */
	enum recording_state recording:2;       /*!< recording status */
	unsigned int isdynamic:1;               /*!< Created on the fly? */
	unsigned int locked:1;                  /*!< Is the conference locked? */
	pthread_t recordthread;                 /*!< thread for recording */
	tris_mutex_t recordthreadlock;           /*!< control threads trying to start recordthread */
	pthread_attr_t attr;                    /*!< thread attribute */
	char *recordingfilename;                /*!< Filename to record the Conference into */
	char *recordingformat;                  /*!< Format to record the Conference in */
	char pin[MAX_PIN];                      /*!< If protected by a PIN */
	char pinadmin[MAX_PIN];                 /*!< If protected by a admin PIN */
	char uniqueid[32];
	long endtime;                           /*!< When to end the conf if scheduled */
	const char *useropts;                   /*!< RealTime user flags */
	const char *adminopts;                  /*!< RealTime moderator flags */
	const char *bookid;                     /*!< RealTime conference id */
	struct tris_frame *transframe[32];
	struct tris_frame *origframe;
	struct tris_trans_pvt *transpath[32];
	TRIS_LIST_HEAD_NOLOCK(, tris_conf_user) userlist;
	TRIS_LIST_ENTRY(tris_conference) list;
	/* announce_thread related data */
	pthread_t announcethread;
	tris_mutex_t announcethreadlock;
	unsigned int announcethread_stop:1;
	tris_cond_t announcelist_addition;
	TRIS_LIST_HEAD_NOLOCK(, announce_listitem) announcelist;
	tris_mutex_t announcelistlock;
	struct tris_dial *dials[MAX_DIALS];
	int pos;
	int maxreferid;
};

static TRIS_LIST_HEAD_STATIC(confs, tris_conference);

static unsigned int conf_map[1024] = {0, };

struct volume {
	int desired;                            /*!< Desired volume adjustment */
	int actual;                             /*!< Actual volume adjustment (for channels that can't adjust) */
};

/*! \brief The MeetMe User object */
struct tris_conf_user {
	int user_no;                            /*!< User Number */
	int userflags;                          /*!< Flags as set in the conference */
	int adminflags;                         /*!< Flags set by the Admin */
	struct tris_channel *chan;               /*!< Connected channel */
	int talking;                            /*!< Is user talking */
	int dahdichannel;                         /*!< Is a DAHDI channel */
	char usrvalue[50];                      /*!< Custom User Value */
	char namerecloc[PATH_MAX];				/*!< Name Recorded file Location */
	time_t jointime;                        /*!< Time the user joined the conference */
 	time_t kicktime;                        /*!< Time the user will be kicked from the conference */
 	struct timeval start_time;              /*!< Time the user entered into the conference */
 	long timelimit;                         /*!< Time limit for the user to be in the conference L(x:y:z) */
 	long play_warning;                      /*!< Play a warning when 'y' ms are left */
 	long warning_freq;                      /*!< Repeat the warning every 'z' ms */
 	const char *warning_sound;              /*!< File to play as warning if 'y' is defined */
 	const char *end_sound;                  /*!< File to play when time is up. */
	struct volume talk;
	struct volume listen;
	TRIS_LIST_ENTRY(tris_conf_user) list;
};

enum sla_which_trunk_refs {
	ALL_TRUNK_REFS,
	INACTIVE_TRUNK_REFS,
};

enum sla_trunk_state {
	SLA_TRUNK_STATE_IDLE,
	SLA_TRUNK_STATE_RINGING,
	SLA_TRUNK_STATE_UP,
	SLA_TRUNK_STATE_ONHOLD,
	SLA_TRUNK_STATE_ONHOLD_BYME,
};

enum sla_hold_access {
	/*! This means that any station can put it on hold, and any station
	 * can retrieve the call from hold. */
	SLA_HOLD_OPEN,
	/*! This means that only the station that put the call on hold may
	 * retrieve it from hold. */
	SLA_HOLD_PRIVATE,
};

struct sla_trunk_ref;

struct sla_station {
	TRIS_RWLIST_ENTRY(sla_station) entry;
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(name);	
		TRIS_STRING_FIELD(device);	
		TRIS_STRING_FIELD(autocontext);	
	);
	TRIS_LIST_HEAD_NOLOCK(, sla_trunk_ref) trunks;
	struct tris_dial *dial;
	/*! Ring timeout for this station, for any trunk.  If a ring timeout
	 *  is set for a specific trunk on this station, that will take
	 *  priority over this value. */
	unsigned int ring_timeout;
	/*! Ring delay for this station, for any trunk.  If a ring delay
	 *  is set for a specific trunk on this station, that will take
	 *  priority over this value. */
	unsigned int ring_delay;
	/*! This option uses the values in the sla_hold_access enum and sets the
	 * access control type for hold on this station. */
	unsigned int hold_access:1;
	/*! Use count for inside sla_station_exec */
	unsigned int ref_count;
};

struct sla_station_ref {
	TRIS_LIST_ENTRY(sla_station_ref) entry;
	struct sla_station *station;
};

struct sla_trunk {
	TRIS_RWLIST_ENTRY(sla_trunk) entry;
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(name);
		TRIS_STRING_FIELD(device);
		TRIS_STRING_FIELD(autocontext);	
	);
	TRIS_LIST_HEAD_NOLOCK(, sla_station_ref) stations;
	/*! Number of stations that use this trunk */
	unsigned int num_stations;
	/*! Number of stations currently on a call with this trunk */
	unsigned int active_stations;
	/*! Number of stations that have this trunk on hold. */
	unsigned int hold_stations;
	struct tris_channel *chan;
	unsigned int ring_timeout;
	/*! If set to 1, no station will be able to join an active call with
	 *  this trunk. */
	unsigned int barge_disabled:1;
	/*! This option uses the values in the sla_hold_access enum and sets the
	 * access control type for hold on this trunk. */
	unsigned int hold_access:1;
	/*! Whether this trunk is currently on hold, meaning that once a station
	 *  connects to it, the trunk channel needs to have UNHOLD indicated to it. */
	unsigned int on_hold:1;
	/*! Use count for inside sla_trunk_exec */
	unsigned int ref_count;
};

struct sla_trunk_ref {
	TRIS_LIST_ENTRY(sla_trunk_ref) entry;
	struct sla_trunk *trunk;
	enum sla_trunk_state state;
	struct tris_channel *chan;
	/*! Ring timeout to use when this trunk is ringing on this specific
	 *  station.  This takes higher priority than a ring timeout set at
	 *  the station level. */
	unsigned int ring_timeout;
	/*! Ring delay to use when this trunk is ringing on this specific
	 *  station.  This takes higher priority than a ring delay set at
	 *  the station level. */
	unsigned int ring_delay;
};

#define S_REPLACE(s, new_val) \
	do {                      \
		if (s) {              \
			free(s);          \
		}                     \
		s = (new_val);        \
	} while (0)


static TRIS_RWLIST_HEAD_STATIC(sla_stations, sla_station);
static TRIS_RWLIST_HEAD_STATIC(sla_trunks, sla_trunk);

static const char sla_registrar[] = "SLA";

/*! \brief Event types that can be queued up for the SLA thread */
enum sla_event_type {
	/*! A station has put the call on hold */
	SLA_EVENT_HOLD,
	/*! The state of a dial has changed */
	SLA_EVENT_DIAL_STATE,
	/*! The state of a ringing trunk has changed */
	SLA_EVENT_RINGING_TRUNK,
	/*! A reload of configuration has been requested */
	SLA_EVENT_RELOAD,
	/*! Poke the SLA thread so it can check if it can perform a reload */
	SLA_EVENT_CHECK_RELOAD,
};

struct sla_event {
	enum sla_event_type type;
	struct sla_station *station;
	struct sla_trunk_ref *trunk_ref;
	TRIS_LIST_ENTRY(sla_event) entry;
};

/*! \brief A station that failed to be dialed 
 * \note Only used by the SLA thread. */
struct sla_failed_station {
	struct sla_station *station;
	struct timeval last_try;
	TRIS_LIST_ENTRY(sla_failed_station) entry;
};

/*! \brief A trunk that is ringing */
struct sla_ringing_trunk {
	struct sla_trunk *trunk;
	/*! The time that this trunk started ringing */
	struct timeval ring_begin;
	TRIS_LIST_HEAD_NOLOCK(, sla_station_ref) timed_out_stations;
	TRIS_LIST_ENTRY(sla_ringing_trunk) entry;
};

enum sla_station_hangup {
	SLA_STATION_HANGUP_NORMAL,
	SLA_STATION_HANGUP_TIMEOUT,
};

/*! \brief A station that is ringing */
struct sla_ringing_station {
	struct sla_station *station;
	/*! The time that this station started ringing */
	struct timeval ring_begin;
	TRIS_LIST_ENTRY(sla_ringing_station) entry;
};

/*!
 * \brief A structure for data used by the sla thread
 */
static struct {
	/*! The SLA thread ID */
	pthread_t thread;
	tris_cond_t cond;
	tris_mutex_t lock;
	TRIS_LIST_HEAD_NOLOCK(, sla_ringing_trunk) ringing_trunks;
	TRIS_LIST_HEAD_NOLOCK(, sla_ringing_station) ringing_stations;
	TRIS_LIST_HEAD_NOLOCK(, sla_failed_station) failed_stations;
	TRIS_LIST_HEAD_NOLOCK(, sla_event) event_q;
	unsigned int stop:1;
	/*! Attempt to handle CallerID, even though it is known not to work
	 *  properly in some situations. */
	unsigned int attempt_callerid:1;
	/*! A reload has been requested */
	unsigned int reload:1;
} sla = {
	.thread = TRIS_PTHREADT_NULL,
};

/*! The number of audio buffers to be allocated on pseudo channels
 *  when in a conference */
static int audio_buffers;

static int max_rooms;

/*! Map 'volume' levels from -5 through +5 into
 *  decibel (dB) settings for channel drivers
 *  Note: these are not a straight linear-to-dB
 *  conversion... the numbers have been modified
 *  to give the user a better level of adjustability
 */
static char const gain_map[] = {
	-15,
	-13,
	-10,
	-6,
	0,
	0,
	0,
	6,
	10,
	13,
	15,
};


static int admin_exec(struct tris_channel *chan, void *data);
static void *recordthread(void *args);
static void *dial_out(struct tris_channel * chan, struct tris_dial **dials, int *pos, const char *conf_name,const char * data, unsigned int extra_flags);
static int conf_run(struct tris_channel *chan, struct tris_conference *conf, int confflags, char *optargs[]);
static struct tris_conference *find_conf(struct tris_channel *chan, char *confno, int make, int dynamic,
					char *dynamic_pin, size_t pin_buf_len, int refcount, struct tris_flags *confflags);

static char *istalking(int x)
{
	if (x > 0)
		return "(talking)";
	else if (x < 0)
		return "(unmonitored)";
	else 
		return "(not talking)";
}

static int careful_write(int fd, unsigned char *data, int len, int block)
{
	int res;
	int x;

	while (len) {
		if (block) {
			x = DAHDI_IOMUX_WRITE | DAHDI_IOMUX_SIGEVENT;
			res = ioctl(fd, DAHDI_IOMUX, &x);
		} else
			res = 0;
		if (res >= 0)
			res = write(fd, data, len);
		if (res < 1) {
			if (errno != EAGAIN) {
				tris_log(LOG_WARNING, "Failed to write audio data to conference: %s\n", strerror(errno));
				return -1;
			} else
				return 0;
		}
		len -= res;
		data += res;
	}

	return 0;
}

static int set_talk_volume(struct tris_conf_user *user, int volume)
{
	char gain_adjust;

	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	gain_adjust = gain_map[volume + 5];

	return tris_channel_setoption(user->chan, TRIS_OPTION_RXGAIN, &gain_adjust, sizeof(gain_adjust), 0);
}

static int set_listen_volume(struct tris_conf_user *user, int volume)
{
	char gain_adjust;

	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	gain_adjust = gain_map[volume + 5];

	return tris_channel_setoption(user->chan, TRIS_OPTION_TXGAIN, &gain_adjust, sizeof(gain_adjust), 0);
}

static void tweak_volume(struct volume *vol, enum volume_action action)
{
	switch (action) {
	case VOL_UP:
		switch (vol->desired) { 
		case 5:
			break;
		case 0:
			vol->desired = 2;
			break;
		case -2:
			vol->desired = 0;
			break;
		default:
			vol->desired++;
			break;
		}
		break;
	case VOL_DOWN:
		switch (vol->desired) {
		case -5:
			break;
		case 2:
			vol->desired = 0;
			break;
		case 0:
			vol->desired = -2;
			break;
		default:
			vol->desired--;
			break;
		}
	}
}

static void tweak_talk_volume(struct tris_conf_user *user, enum volume_action action)
{
	tweak_volume(&user->talk, action);
	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	if (!set_talk_volume(user, user->talk.desired))
		user->talk.actual = 0;
	else
		user->talk.actual = user->talk.desired;
}

static void tweak_listen_volume(struct tris_conf_user *user, enum volume_action action)
{
	tweak_volume(&user->listen, action);
	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	if (!set_listen_volume(user, user->listen.desired))
		user->listen.actual = 0;
	else
		user->listen.actual = user->listen.desired;
}

static void reset_volumes(struct tris_conf_user *user)
{
	signed char zero_volume = 0;

	tris_channel_setoption(user->chan, TRIS_OPTION_TXGAIN, &zero_volume, sizeof(zero_volume), 0);
	tris_channel_setoption(user->chan, TRIS_OPTION_RXGAIN, &zero_volume, sizeof(zero_volume), 0);
}

static void conf_play(struct tris_channel *chan, struct tris_conference *conf, enum entrance_sound sound)
{
	unsigned char *data;
	int len;
	int res = -1;

	if (!tris_check_hangup(chan))
		res = tris_autoservice_start(chan);

	TRIS_LIST_LOCK(&confs);

	switch(sound) {
	case ENTER:
		data = enter;
		len = sizeof(enter);
		break;
	case LEAVE:
		data = leave;
		len = sizeof(leave);
		break;
	default:
		data = NULL;
		len = 0;
	}
	if (data) {
		careful_write(conf->fd, data, len, 1);
	}

	TRIS_LIST_UNLOCK(&confs);

	if (!res) 
		tris_autoservice_stop(chan);
}

/*!
 * \brief Find or create a conference
 *
 * \param confno The conference name/number
 * \param pin The regular user pin
 * \param pinadmin The admin pin
 * \param make Make the conf if it doesn't exist
 * \param dynamic Mark the newly created conference as dynamic
 * \param refcount How many references to mark on the conference
 * \param chan The trismedia channel
 *
 * \return A pointer to the conference struct, or NULL if it wasn't found and
 *         make or dynamic were not set.
 */
static struct tris_conference *build_conf(char *confno, char *pin, char *pinadmin, int make, int dynamic, int refcount, const struct tris_channel *chan)
{
	struct tris_conference *cnf;
	struct dahdi_confinfo dahdic = { 0, };
	int confno_int = 0;

	TRIS_LIST_LOCK(&confs);

	TRIS_LIST_TRAVERSE(&confs, cnf, list) {
		if (!strcmp(confno, cnf->confno)) 
			break;
	}

	if (cnf || (!make && !dynamic))
		goto cnfout;

	/* Make a new one */
	if (!(cnf = tris_calloc(1, sizeof(*cnf))))
		goto cnfout;

	memset(cnf, 0, sizeof(*cnf));
	tris_mutex_init(&cnf->playlock);
	tris_mutex_init(&cnf->listenlock);
	cnf->recordthread = TRIS_PTHREADT_NULL;
	tris_mutex_init(&cnf->recordthreadlock);
	cnf->announcethread = TRIS_PTHREADT_NULL;
	tris_mutex_init(&cnf->announcethreadlock);
	tris_copy_string(cnf->confno, confno, sizeof(cnf->confno));
	tris_copy_string(cnf->pin, pin, sizeof(cnf->pin));
	tris_copy_string(cnf->pinadmin, pinadmin, sizeof(cnf->pinadmin));
	tris_copy_string(cnf->uniqueid, chan->uniqueid, sizeof(cnf->uniqueid));
	cnf->admin_chan = NULL;

	/* Setup a new dahdi conference */
	dahdic.confno = -1;
	dahdic.confmode = DAHDI_CONF_CONFANN | DAHDI_CONF_CONFANNMON;
	cnf->fd = open("/dev/dahdi/pseudo", O_RDWR);
	if (cnf->fd < 0 || ioctl(cnf->fd, DAHDI_SETCONF, &dahdic)) {
		tris_log(LOG_WARNING, "Unable to open pseudo device\n");
		if (cnf->fd >= 0)
			close(cnf->fd);
		tris_free(cnf);
		cnf = NULL;
		goto cnfout;
	}

	cnf->dahdiconf = dahdic.confno;

	/* Setup a new channel for playback of audio files */
	cnf->chan = tris_request("DAHDI", TRIS_FORMAT_SLINEAR, "pseudo", NULL, 0);
	if (cnf->chan) {
		tris_set_read_format(cnf->chan, TRIS_FORMAT_SLINEAR);
		tris_set_write_format(cnf->chan, TRIS_FORMAT_SLINEAR);
		dahdic.chan = 0;
		dahdic.confno = cnf->dahdiconf;
		dahdic.confmode = DAHDI_CONF_CONFANN | DAHDI_CONF_CONFANNMON;
		if (ioctl(cnf->chan->fds[0], DAHDI_SETCONF, &dahdic)) {
			tris_log(LOG_WARNING, "Error setting conference\n");
			if (cnf->chan)
				tris_hangup(cnf->chan);
			else
				close(cnf->fd);

			tris_free(cnf);
			cnf = NULL;
			goto cnfout;
		}
	}

	/* Fill the conference struct */
	cnf->start = time(NULL);
	cnf->maxusers = 0x7fffffff;
	cnf->isdynamic = dynamic ? 1 : 0;
	tris_verb(3, "Created MeetMe conference %d for conference '%s'\n", cnf->dahdiconf, cnf->confno);
	TRIS_LIST_INSERT_HEAD(&confs, cnf, list);

	/* Reserve conference number in map */
	if ((sscanf(cnf->confno, "%30d", &confno_int) == 1) && (confno_int >= 0 && confno_int < 1024))
		conf_map[confno_int] = 1;

	cnf->maxreferid = 100;
cnfout:
	if (cnf)
		tris_atomic_fetchadd_int(&cnf->refcount, refcount);

	TRIS_LIST_UNLOCK(&confs);

	return cnf;
}

static char *complete_meetmecmd(const char *line, const char *word, int pos, int state)
{
	static char *cmds[] = {"concise", "lock", "unlock", "mute", "unmute", "kick", "list", "record", NULL};

	int len = strlen(word);
	int which = 0;
	struct tris_conference *cnf = NULL;
	struct tris_conf_user *usr = NULL;
	char *confno = NULL;
	char usrno[50] = "";
	char *myline, *ret = NULL;
	
	if (pos == 1) {		/* Command */
		return tris_cli_complete(word, cmds, state);
	} else if (pos == 2) {	/* Conference Number */
		TRIS_LIST_LOCK(&confs);
		TRIS_LIST_TRAVERSE(&confs, cnf, list) {
			if (!strncasecmp(word, cnf->confno, len) && ++which > state) {
				ret = cnf->confno;
				break;
			}
		}
		ret = tris_strdup(ret); /* dup before releasing the lock */
		TRIS_LIST_UNLOCK(&confs);
		return ret;
	} else if (pos == 3) {
		/* User Number || Conf Command option*/
		if (strstr(line, "mute") || strstr(line, "kick")) {
			if (state == 0 && (strstr(line, "kick") || strstr(line, "mute")) && !strncasecmp(word, "all", len))
				return tris_strdup("all");
			which++;
			TRIS_LIST_LOCK(&confs);

			/* TODO: Find the conf number from the cmdline (ignore spaces) <- test this and make it fail-safe! */
			myline = tris_strdupa(line);
			if (strsep(&myline, " ") && strsep(&myline, " ") && !confno) {
				while((confno = strsep(&myline, " ")) && (strcmp(confno, " ") == 0))
					;
			}
			
			TRIS_LIST_TRAVERSE(&confs, cnf, list) {
				if (!strcmp(confno, cnf->confno))
				    break;
			}

			if (cnf) {
				/* Search for the user */
				TRIS_LIST_TRAVERSE(&cnf->userlist, usr, list) {
					snprintf(usrno, sizeof(usrno), "%d", usr->user_no);
					if (!strncasecmp(word, usrno, len) && ++which > state)
						break;
				}
			}
			TRIS_LIST_UNLOCK(&confs);
			return usr ? tris_strdup(usrno) : NULL;
		}
	}

	return NULL;
}

static char *meetme_show_cmd(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	/* Process the command */
	struct tris_conf_user *user;
	struct tris_conference *cnf;
	int hr, min, sec;
	int i = 0, total = 0;
	time_t now;
	struct tris_str *cmdline = NULL;
#define MC_HEADER_FORMAT "%-14s %-14s %-10s %-8s  %-8s  %-6s\n"
#define MC_DATA_FORMAT "%-12.12s   %4.4d	      %4.4s       %02d:%02d:%02d  %-8s  %-6s\n"

	switch (cmd) {
	case CLI_INIT:
		e->command = "meetme list [concise]";
		e->usage =
			"Usage: meetme list <confno> [concise] \n"
			"       List all or a specific conference.\n";
		return NULL;
	case CLI_GENERATE:
		return complete_meetmecmd(a->line, a->word, a->pos, a->n);
	}

	/* Check for length so no buffer will overflow... */
	for (i = 0; i < a->argc; i++) {
		if (strlen(a->argv[i]) > 100)
			tris_cli(a->fd, "Invalid Arguments.\n");
	}

	/* Max confno length */
	if (!(cmdline = tris_str_create(MAX_CONFNUM))) {
		return CLI_FAILURE;
	}

	if (a->argc == 2 || (a->argc == 3 && !strcasecmp(a->argv[2], "concise"))) {
		/* List all the conferences */	
		int concise = (a->argc == 3 && !strcasecmp(a->argv[2], "concise"));
		now = time(NULL);
		TRIS_LIST_LOCK(&confs);
		if (TRIS_LIST_EMPTY(&confs)) {
			if (!concise) {
				tris_cli(a->fd, "No active MeetMe conferences.\n");
			}
			TRIS_LIST_UNLOCK(&confs);
			tris_free(cmdline);
			return CLI_SUCCESS;
		}
		if (!concise) {
			tris_cli(a->fd, MC_HEADER_FORMAT, "Conf Num", "Parties", "Marked", "Activity", "Creation", "Locked");
		}
		TRIS_LIST_TRAVERSE(&confs, cnf, list) {
			if (cnf->markedusers == 0) {
				tris_str_set(&cmdline, 0, "N/A ");
			} else {
				tris_str_set(&cmdline, 0, "%4.4d", cnf->markedusers);
			}
			hr = (now - cnf->start) / 3600;
			min = ((now - cnf->start) % 3600) / 60;
			sec = (now - cnf->start) % 60;
			if (!concise) {
				tris_cli(a->fd, MC_DATA_FORMAT, cnf->confno, cnf->users, tris_str_buffer(cmdline), hr, min, sec, cnf->isdynamic ? "Dynamic" : "Static", cnf->locked ? "Yes" : "No");
			} else {
				tris_cli(a->fd, "%s!%d!%d!%02d:%02d:%02d!%d!%d\n",
					cnf->confno,
					cnf->users,
					cnf->markedusers,
					hr, min, sec,
					cnf->isdynamic,
					cnf->locked);
			}

			total += cnf->users;
		}
		TRIS_LIST_UNLOCK(&confs);
		if (!concise) {
			tris_cli(a->fd, "* Total number of MeetMe users: %d\n", total);
		}
		tris_free(cmdline);
		return CLI_SUCCESS;
	} else if (strcmp(a->argv[1], "list") == 0) {
		int concise = (a->argc == 4 && (!strcasecmp(a->argv[3], "concise")));
		/* List all the users in a conference */
		if (TRIS_LIST_EMPTY(&confs)) {
//			if (!concise) {
//				tris_cli(a->fd, "No active MeetMe conferences.\n");
//			}
			tris_cli(a->fd,"No such conference: %s.\n", a->argv[2]);
			tris_free(cmdline);
			return CLI_SUCCESS;	
		}
		/* Find the right conference */
		TRIS_LIST_LOCK(&confs);
		TRIS_LIST_TRAVERSE(&confs, cnf, list) {
			if (strcmp(cnf->confno, a->argv[2]) == 0) {
				break;
			}
		}
		if (!cnf) {
//			if (!concise)
				tris_cli(a->fd, "No such conference: %s.\n", a->argv[2]);
			TRIS_LIST_UNLOCK(&confs);
			tris_free(cmdline);
			return CLI_SUCCESS;
		}
		/* Show all the users */
		time(&now);
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			hr = (now - user->jointime) / 3600;
			min = ((now - user->jointime) % 3600) / 60;
			sec = (now - user->jointime) % 60;
			if (!concise) {
				tris_cli(a->fd, "User #: %-2.2d %12.12s %-20.20s Channel: %s %s %s %s %s %s %02d:%02d:%02d\n",
					user->user_no,
					S_OR(user->chan->cid.cid_num, "<unknown>"),
					S_OR(user->chan->cid.cid_name, "<no name>"),
					user->chan->name,
					user->userflags & CONFFLAG_ADMIN ? "(Admin)" : "",
					user->userflags & CONFFLAG_MONITOR ? "(Listen only)" : "",
					user->adminflags & ADMINFLAG_MUTED ? "(Admin Muted)" : user->adminflags & ADMINFLAG_SELFMUTED ? "(Muted)" : "",
					user->adminflags & ADMINFLAG_T_REQUEST ? "(Request to Talk)" : "",
					istalking(user->talking), hr, min, sec); 
			} else {
				tris_cli(a->fd, "%d!%s!%s!%s!%s!%s!%s!%s!%d!%02d:%02d:%02d\n",
					user->user_no,
					S_OR(user->chan->cid.cid_num, ""),
					S_OR(user->chan->cid.cid_name, ""),
					user->chan->name,
					user->userflags  & CONFFLAG_ADMIN   ? "1" : "",
					user->userflags  & CONFFLAG_MONITOR ? "1" : "",
					user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED) ? "1" : "",
					user->adminflags & ADMINFLAG_T_REQUEST ? "1" : "",
					user->talking, hr, min, sec);
			}
		}
		if (!concise) {
			tris_cli(a->fd, "%d users in that conference.\n", cnf->users);
		}
		TRIS_LIST_UNLOCK(&confs);
		tris_free(cmdline);
		return CLI_SUCCESS;
	}
	if (a->argc < 2) {
		tris_free(cmdline);
		return CLI_SHOWUSAGE;
	}

	tris_debug(1, "Cmdline: %s\n", tris_str_buffer(cmdline));

	admin_exec(NULL, tris_str_buffer(cmdline));
	tris_free(cmdline);

	return CLI_SUCCESS;
}


static char *meetme_cmd(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	/* Process the command */
	struct tris_str *cmdline = NULL;
	int i = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "meetme {lock|unlock|mute|unmute|kick}";
		e->usage =
			"Usage: meetme (un)lock|(un)mute|kick <confno> <usernumber>\n"
			"       Executes a command for the conference or on a conferee\n";
		return NULL;
	case CLI_GENERATE:
		return complete_meetmecmd(a->line, a->word, a->pos, a->n);
	}

	if (a->argc > 8)
		tris_cli(a->fd, "Invalid Arguments.\n");
	/* Check for length so no buffer will overflow... */
	for (i = 0; i < a->argc; i++) {
		if (strlen(a->argv[i]) > 100)
			tris_cli(a->fd, "Invalid Arguments.\n");
	}

	/* Max confno length */
	if (!(cmdline = tris_str_create(MAX_CONFNUM))) {
		return CLI_FAILURE;
	}

	if (a->argc < 1) {
		tris_free(cmdline);
		return CLI_SHOWUSAGE;
	}

	tris_str_set(&cmdline, 0, "%s", a->argv[2]);	/* Argv 2: conference number */
	if (strstr(a->argv[1], "lock")) {
		if (strcmp(a->argv[1], "lock") == 0) {
			/* Lock */
			tris_str_append(&cmdline, 0, ",L");
		} else {
			/* Unlock */
			tris_str_append(&cmdline, 0, ",l");
		}
	} else if (strstr(a->argv[1], "mute")) { 
		if (a->argc < 4) {
			tris_free(cmdline);
			return CLI_SHOWUSAGE;
		}
		if (strcmp(a->argv[1], "mute") == 0) {
			/* Mute */
			if (strcmp(a->argv[3], "all") == 0) {
				tris_str_append(&cmdline, 0, ",N");
			} else {
				tris_str_append(&cmdline, 0, ",M,%s", a->argv[3]);	
			}
		} else {
			/* Unmute */
			if (strcmp(a->argv[3], "all") == 0) {
				tris_str_append(&cmdline, 0, ",n");
			} else {
				tris_str_append(&cmdline, 0, ",m,%s", a->argv[3]);
			}
		}
	} else if (strcmp(a->argv[1], "kick") == 0) {
		if (a->argc < 4) {
			tris_free(cmdline);
			return CLI_SHOWUSAGE;
		}
		if (strcmp(a->argv[3], "all") == 0) {
			/* Kick all */
			tris_str_append(&cmdline, 0, ",K");
		} else {
			/* Kick a single user */
			tris_str_append(&cmdline, 0, ",k,%s", a->argv[3]);
		}
	}  else if (strcmp(a->argv[1], "record") == 0) {
		if (a->argc < 4)
			return CLI_SHOWUSAGE;
			/* Kick a single user */
		tris_str_append(&cmdline, 0, ",a,%s", a->argv[3]);
	} else {
		tris_free(cmdline);
		return CLI_SHOWUSAGE;
	}

	tris_debug(1, "Cmdline: %s\n", tris_str_buffer(cmdline));

	admin_exec(NULL, tris_str_buffer(cmdline));
	tris_free(cmdline);

	return CLI_SUCCESS;
}

static const char *sla_hold_str(unsigned int hold_access)
{
	const char *hold = "Unknown";

	switch (hold_access) {
	case SLA_HOLD_OPEN:
		hold = "Open";
		break;
	case SLA_HOLD_PRIVATE:
		hold = "Private";
	default:
		break;
	}

	return hold;
}

static char *sla_show_trunks(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	const struct sla_trunk *trunk;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sla show trunks";
		e->usage =
			"Usage: sla show trunks\n"
			"       This will list all trunks defined in sla.conf\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	tris_cli(a->fd, "\n"
	            "=============================================================\n"
	            "=== Configured SLA Trunks ===================================\n"
	            "=============================================================\n"
	            "===\n");
	TRIS_RWLIST_RDLOCK(&sla_trunks);
	TRIS_RWLIST_TRAVERSE(&sla_trunks, trunk, entry) {
		struct sla_station_ref *station_ref;
		char ring_timeout[16] = "(none)";
		if (trunk->ring_timeout)
			snprintf(ring_timeout, sizeof(ring_timeout), "%u Seconds", trunk->ring_timeout);
		tris_cli(a->fd, "=== ---------------------------------------------------------\n"
		            "=== Trunk Name:       %s\n"
		            "=== ==> Device:       %s\n"
		            "=== ==> AutoContext:  %s\n"
		            "=== ==> RingTimeout:  %s\n"
		            "=== ==> BargeAllowed: %s\n"
		            "=== ==> HoldAccess:   %s\n"
		            "=== ==> Stations ...\n",
		            trunk->name, trunk->device, 
		            S_OR(trunk->autocontext, "(none)"), 
		            ring_timeout,
		            trunk->barge_disabled ? "No" : "Yes",
		            sla_hold_str(trunk->hold_access));
		TRIS_RWLIST_RDLOCK(&sla_stations);
		TRIS_LIST_TRAVERSE(&trunk->stations, station_ref, entry)
			tris_cli(a->fd, "===    ==> Station name: %s\n", station_ref->station->name);
		TRIS_RWLIST_UNLOCK(&sla_stations);
		tris_cli(a->fd, "=== ---------------------------------------------------------\n===\n");
	}
	TRIS_RWLIST_UNLOCK(&sla_trunks);
	tris_cli(a->fd, "=============================================================\n\n");

	return CLI_SUCCESS;
}

static const char *trunkstate2str(enum sla_trunk_state state)
{
#define S(e) case e: return # e;
	switch (state) {
	S(SLA_TRUNK_STATE_IDLE)
	S(SLA_TRUNK_STATE_RINGING)
	S(SLA_TRUNK_STATE_UP)
	S(SLA_TRUNK_STATE_ONHOLD)
	S(SLA_TRUNK_STATE_ONHOLD_BYME)
	}
	return "Uknown State";
#undef S
}

static char *sla_show_stations(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	const struct sla_station *station;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sla show stations";
		e->usage =
			"Usage: sla show stations\n"
			"       This will list all stations defined in sla.conf\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	tris_cli(a->fd, "\n" 
	            "=============================================================\n"
	            "=== Configured SLA Stations =================================\n"
	            "=============================================================\n"
	            "===\n");
	TRIS_RWLIST_RDLOCK(&sla_stations);
	TRIS_RWLIST_TRAVERSE(&sla_stations, station, entry) {
		struct sla_trunk_ref *trunk_ref;
		char ring_timeout[16] = "(none)";
		char ring_delay[16] = "(none)";
		if (station->ring_timeout) {
			snprintf(ring_timeout, sizeof(ring_timeout), 
				"%u", station->ring_timeout);
		}
		if (station->ring_delay) {
			snprintf(ring_delay, sizeof(ring_delay), 
				"%u", station->ring_delay);
		}
		tris_cli(a->fd, "=== ---------------------------------------------------------\n"
		            "=== Station Name:    %s\n"
		            "=== ==> Device:      %s\n"
		            "=== ==> AutoContext: %s\n"
		            "=== ==> RingTimeout: %s\n"
		            "=== ==> RingDelay:   %s\n"
		            "=== ==> HoldAccess:  %s\n"
		            "=== ==> Trunks ...\n",
		            station->name, station->device,
		            S_OR(station->autocontext, "(none)"), 
		            ring_timeout, ring_delay,
		            sla_hold_str(station->hold_access));
		TRIS_RWLIST_RDLOCK(&sla_trunks);
		TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
			if (trunk_ref->ring_timeout) {
				snprintf(ring_timeout, sizeof(ring_timeout),
					"%u", trunk_ref->ring_timeout);
			} else
				strcpy(ring_timeout, "(none)");
			if (trunk_ref->ring_delay) {
				snprintf(ring_delay, sizeof(ring_delay),
					"%u", trunk_ref->ring_delay);
			} else
				strcpy(ring_delay, "(none)");
				tris_cli(a->fd, "===    ==> Trunk Name: %s\n"
			            "===       ==> State:       %s\n"
			            "===       ==> RingTimeout: %s\n"
			            "===       ==> RingDelay:   %s\n",
			            trunk_ref->trunk->name,
			            trunkstate2str(trunk_ref->state),
			            ring_timeout, ring_delay);
		}
		TRIS_RWLIST_UNLOCK(&sla_trunks);
		tris_cli(a->fd, "=== ---------------------------------------------------------\n"
		            "===\n");
	}
	TRIS_RWLIST_UNLOCK(&sla_stations);
	tris_cli(a->fd, "============================================================\n"
	            "\n");

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_meetme[] = {
	TRIS_CLI_DEFINE(meetme_cmd, "Execute a command on a conference or conferee"),
	TRIS_CLI_DEFINE(meetme_show_cmd, "List all or one conference"),
	TRIS_CLI_DEFINE(sla_show_trunks, "Show SLA Trunks"),
	TRIS_CLI_DEFINE(sla_show_stations, "Show SLA Stations"),
};

static void conf_flush(int fd, struct tris_channel *chan)
{
	int x;

	/* read any frames that may be waiting on the channel
	   and throw them away
	*/
	if (chan) {
		struct tris_frame *f;

		/* when no frames are available, this will wait
		   for 1 millisecond maximum
		*/
		while (tris_waitfor(chan, 1)) {
			f = tris_read(chan);
			if (f)
				tris_frfree(f);
			else /* channel was hung up or something else happened */
				break;
		}
	}

	/* flush any data sitting in the pseudo channel */
	x = DAHDI_FLUSH_ALL;
	if (ioctl(fd, DAHDI_FLUSH, &x))
		tris_log(LOG_WARNING, "Error flushing channel\n");

}

/* Remove the conference from the list and free it.
   We assume that this was called while holding conflock. */
static int conf_free(struct tris_conference *conf)
{
	int x;
	struct announce_listitem *item;
	
	TRIS_LIST_REMOVE(&confs, conf, list);
	manager_event(EVENT_FLAG_CALL, "MeetmeEnd", "Meetme: %s\r\n", conf->confno);

	if (conf->recording == MEETME_RECORD_ACTIVE) {
		conf->recording = MEETME_RECORD_TERMINATE;
		TRIS_LIST_UNLOCK(&confs);
		while (1) {
			usleep(1);
			TRIS_LIST_LOCK(&confs);
			if (conf->recording == MEETME_RECORD_OFF)
				break;
			TRIS_LIST_UNLOCK(&confs);
		}
	}

	for (x = 0; x < TRIS_FRAME_BITS; x++) {
		if (conf->transframe[x])
			tris_frfree(conf->transframe[x]);
		if (conf->transpath[x])
			tris_translator_free_path(conf->transpath[x]);
	}
	for (x = 0; x < conf->pos; x++) {
		struct tris_dial *dial = conf->dials[x];
		if (!dial)
			continue;
	
		/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
		tris_dial_join(dial);
	
		/* Hangup all channels */
		tris_dial_hangup(dial);
	
		/* Destroy dialing structure */
		tris_dial_destroy(dial);
		conf->dials[x] = NULL;
		tris_verbose(" --------------- destroy dial (%d)\n", x);
	}
	
	if (conf->announcethread != TRIS_PTHREADT_NULL) {
		tris_mutex_lock(&conf->announcelistlock);
		conf->announcethread_stop = 1;
		tris_softhangup(conf->chan, TRIS_SOFTHANGUP_EXPLICIT);
		tris_cond_signal(&conf->announcelist_addition);
		tris_mutex_unlock(&conf->announcelistlock);
		pthread_join(conf->announcethread, NULL);
	
		while ((item = TRIS_LIST_REMOVE_HEAD(&conf->announcelist, entry))) {
			tris_filedelete(item->namerecloc, NULL);
			ao2_ref(item, -1);
		}
		tris_mutex_destroy(&conf->announcelistlock);
	}
	if (conf->origframe)
		tris_frfree(conf->origframe);
	if (conf->lchan)
		tris_hangup(conf->lchan);
	if (conf->chan)
		tris_hangup(conf->chan);
	if (conf->fd >= 0)
		close(conf->fd);
	if (conf->recordingfilename) {
		tris_free(conf->recordingfilename);
	}
	if (conf->recordingformat) {
		tris_free(conf->recordingformat);
	}
	tris_mutex_destroy(&conf->playlock);
	tris_mutex_destroy(&conf->listenlock);
	tris_mutex_destroy(&conf->recordthreadlock);
	tris_mutex_destroy(&conf->announcethreadlock);
	tris_free(conf);

	return 0;
}

static void conf_queue_dtmf(const struct tris_conference *conf,
	const struct tris_conf_user *sender, struct tris_frame *f)
{
	struct tris_conf_user *user;

	TRIS_LIST_TRAVERSE(&conf->userlist, user, list) {
		if (user == sender)
			continue;
		if (tris_write(user->chan, f) < 0)
			tris_log(LOG_WARNING, "Error writing frame to channel %s\n", user->chan->name);
	}
}

static void sla_queue_event_full(enum sla_event_type type, 
	struct sla_trunk_ref *trunk_ref, struct sla_station *station, int lock)
{
	struct sla_event *event;

	if (sla.thread == TRIS_PTHREADT_NULL) {
		return;
	}

	if (!(event = tris_calloc(1, sizeof(*event))))
		return;

	event->type = type;
	event->trunk_ref = trunk_ref;
	event->station = station;

	if (!lock) {
		TRIS_LIST_INSERT_TAIL(&sla.event_q, event, entry);
		return;
	}

	tris_mutex_lock(&sla.lock);
	TRIS_LIST_INSERT_TAIL(&sla.event_q, event, entry);
	tris_cond_signal(&sla.cond);
	tris_mutex_unlock(&sla.lock);
}

static void sla_queue_event_nolock(enum sla_event_type type)
{
	sla_queue_event_full(type, NULL, NULL, 0);
}

static void sla_queue_event(enum sla_event_type type)
{
	sla_queue_event_full(type, NULL, NULL, 1);
}

/*! \brief Queue a SLA event from the conference */
static void sla_queue_event_conf(enum sla_event_type type, struct tris_channel *chan,
	struct tris_conference *conf)
{
	struct sla_station *station;
	struct sla_trunk_ref *trunk_ref = NULL;
	char *trunk_name;

	trunk_name = tris_strdupa(conf->confno);
	strsep(&trunk_name, "_");
	if (tris_strlen_zero(trunk_name)) {
		tris_log(LOG_ERROR, "Invalid conference name for SLA - '%s'!\n", conf->confno);
		return;
	}

	TRIS_RWLIST_RDLOCK(&sla_stations);
	TRIS_RWLIST_TRAVERSE(&sla_stations, station, entry) {
		TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
			if (trunk_ref->chan == chan && !strcmp(trunk_ref->trunk->name, trunk_name))
				break;
		}
		if (trunk_ref)
			break;
	}
	TRIS_RWLIST_UNLOCK(&sla_stations);

	if (!trunk_ref) {
		tris_debug(1, "Trunk not found for event!\n");
		return;
	}

	sla_queue_event_full(type, trunk_ref, station, 1);
}

/* Decrement reference counts, as incremented by find_conf() */
static int dispose_conf(struct tris_conference *conf)
{
	int res = 0;
	int confno_int = 0;

	TRIS_LIST_LOCK(&confs);
	if (tris_atomic_dec_and_test(&conf->refcount)) {
		/* Take the conference room number out of an inuse state */
		if ((sscanf(conf->confno, "%4d", &confno_int) == 1) && (confno_int >= 0 && confno_int < 1024)) {
			conf_map[confno_int] = 0;
		}
		conf_free(conf);
		res = 1;
	}
	TRIS_LIST_UNLOCK(&confs);

	return res;
}

static int rt_extend_conf(char *confno)
{
	char currenttime[32];
	char endtime[32];
	struct timeval now;
	struct tris_tm tm;
	struct tris_variable *var, *orig_var;
	char bookid[51];

	if (!extendby) {
		return 0;
	}

	now = tris_tvnow();

	tris_localtime(&now, &tm, NULL);
	tris_strftime(currenttime, sizeof(currenttime), DATE_FORMAT, &tm);

	var = tris_load_realtime("meetme", "confno",
		confno, "startTime<= ", currenttime,
		"endtime>= ", currenttime, NULL);

	orig_var = var;

	/* Identify the specific RealTime conference */
	while (var) {
		if (!strcasecmp(var->name, "bookid")) {
			tris_copy_string(bookid, var->value, sizeof(bookid));
		}
		if (!strcasecmp(var->name, "endtime")) {
			tris_copy_string(endtime, var->value, sizeof(endtime));
		}

		var = var->next;
	}
	tris_variables_destroy(orig_var);

	tris_strptime(endtime, DATE_FORMAT, &tm);
	now = tris_mktime(&tm, NULL);

	now.tv_sec += extendby;

	tris_localtime(&now, &tm, NULL);
	tris_strftime(currenttime, sizeof(currenttime), DATE_FORMAT, &tm);
	strcat(currenttime, "0"); /* Seconds needs to be 00 */

	var = tris_load_realtime("meetme", "confno",
		confno, "startTime<= ", currenttime,
		"endtime>= ", currenttime, NULL);

	/* If there is no conflict with extending the conference, update the DB */
	if (!var) {
		tris_debug(3, "Trying to update the endtime of Conference %s to %s\n", confno, currenttime);
		tris_update_realtime("meetme", "bookid", bookid, "endtime", currenttime, NULL);
		return 0;

	}

	tris_variables_destroy(var);
	return -1;
}

static void conf_start_moh(struct tris_channel *chan, const char *musicclass)
{
  	char *original_moh;

	tris_channel_lock(chan);
	original_moh = tris_strdupa(chan->musicclass);
	tris_string_field_set(chan, musicclass, musicclass);
	tris_channel_unlock(chan);

	tris_moh_start(chan, original_moh, NULL);

	tris_channel_lock(chan);
	tris_string_field_set(chan, musicclass, original_moh);
	tris_channel_unlock(chan);
}

static const char *get_announce_filename(enum announcetypes type)
{
	switch (type) {
	case CONF_HASLEFT:
		return "conference/conf-hasleft";
		break;
	case CONF_HASJOIN:
		return "conference/conf-hasjoin";
		break;
	default:
		return "";
	}
}

static void *announce_thread(void *data)
{
	struct announce_listitem *current;
	struct tris_conference *conf = data;
	int res;
	char filename[PATH_MAX] = "";
	TRIS_LIST_HEAD_NOLOCK(, announce_listitem) local_list;
	TRIS_LIST_HEAD_INIT_NOLOCK(&local_list);

	while (!conf->announcethread_stop) {
		tris_mutex_lock(&conf->announcelistlock);
		if (conf->announcethread_stop) {
			tris_mutex_unlock(&conf->announcelistlock);
			break;
		}
		if (TRIS_LIST_EMPTY(&conf->announcelist))
			tris_cond_wait(&conf->announcelist_addition, &conf->announcelistlock);

		TRIS_LIST_APPEND_LIST(&local_list, &conf->announcelist, entry);
		TRIS_LIST_HEAD_INIT_NOLOCK(&conf->announcelist);

		tris_mutex_unlock(&conf->announcelistlock);
		if (conf->announcethread_stop) {
			break;
		}

		for (res = 1; !conf->announcethread_stop && (current = TRIS_LIST_REMOVE_HEAD(&local_list, entry)); ao2_ref(current, -1)) {
			tris_log(LOG_DEBUG, "About to play %s\n", current->namerecloc);
//			if (!tris_fileexists(current->namerecloc, NULL, NULL))
//				continue;
			if ((current->confchan) && (current->confusers > 1) && !tris_check_hangup(current->confchan)) {
//				if (!tris_streamfile(current->confchan, current->namerecloc, current->language))
				if(!tris_say_digit_str(current->confchan, current->exten, "", current->language))
					res = tris_waitstream(current->confchan, "");
				if (!res) {
					if(!strncasecmp(conf->confno, "urg", 3) && current->announcetype == CONF_HASLEFT) {
						tris_copy_string(filename, "conference/multi-talking-hasleft", sizeof(filename));
					} else {
						tris_copy_string(filename, get_announce_filename(current->announcetype), sizeof(filename));
					}
					if (!tris_streamfile(current->confchan, filename, current->language))
						tris_waitstream(current->confchan, "");
				}
			}
			if (current->announcetype == CONF_HASLEFT) {
				tris_filedelete(current->namerecloc, NULL);
			}
		}
	}

	/* thread marked to stop, clean up */
	while ((current = TRIS_LIST_REMOVE_HEAD(&local_list, entry))) {
		tris_filedelete(current->namerecloc, NULL);
		ao2_ref(current, -1);
	}
	return NULL;
}

static int can_write(struct tris_channel *chan, int confflags)
{
	if (!(confflags & CONFFLAG_NO_AUDIO_UNTIL_UP)) {
		return 1;
	}

	return (chan->_state == TRIS_STATE_UP);
}

static void send_talking_event(struct tris_channel *chan, struct tris_conference *conf, struct tris_conf_user *user, int talking)
{
	manager_event(EVENT_FLAG_CALL, "MeetmeTalking",
	      "Channel: %s\r\n"
	      "Uniqueid: %s\r\n"
	      "Meetme: %s\r\n"
	      "Usernum: %d\r\n"
	      "Status: %s\r\n",
	      chan->name, chan->uniqueid, conf->confno, user->user_no, talking ? "on" : "off");
}

static void set_user_talking(struct tris_channel *chan, struct tris_conference *conf, struct tris_conf_user *user, int talking, int monitor)
{
	int last_talking = user->talking;
	if (last_talking == talking)
		return;

	user->talking = talking;

	if (monitor) {
		/* Check if talking state changed. Take care of -1 which means unmonitored */
		int was_talking = (last_talking > 0);
		int now_talking = (talking > 0);
		if (was_talking != now_talking) {
			send_talking_event(chan, conf, user, now_talking);
		}
	}
}

static int invite_to_meetme(struct tris_channel *chan, struct tris_dial **dials, int *pos, const char* data, char *confno) 
{
	struct tris_dial *dial;
	char *tech, *tech_data;
	enum tris_dial_result dial_res;
	char meetmeopts[81];
	struct tris_channel *callee_chan = NULL;
	//int res = 0;

	if (!(dial = tris_dial_create())) {
		return 0;
	}

	tech_data = tris_strdupa(data);
	tech = strsep(&tech_data, "/");
	if (tris_dial_append(dial, tech, tech_data) == -1) {
		tris_dial_destroy(dial);
		return 0;
	}

	snprintf(meetmeopts, sizeof(meetmeopts), "MeetMe,%s,dio(%s)", confno, confno);
	tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC,meetmeopts);
	
	dial_res = tris_dial_run(dial, chan, 1, 0);

	if (dial_res != TRIS_DIAL_RESULT_TRYING) {
		tris_dial_destroy(dial);
		return 0;
	}
	//tris_indicate(chan, TRIS_CONTROL_RINGING);
	tris_streamfile(chan, "conference/ringing", chan->language);
	for (;;) {
		unsigned int done = 0;
		switch ((dial_res = tris_dial_state(dial))) {
		case TRIS_DIAL_RESULT_ANSWERED:
			callee_chan = tris_dial_answered(dial);
			done =1;
			break;
		case TRIS_DIAL_RESULT_BUSY:
		case TRIS_DIAL_RESULT_CONGESTION:
		case TRIS_DIAL_RESULT_FORBIDDEN:
		case TRIS_DIAL_RESULT_OFFHOOK:
		case TRIS_DIAL_RESULT_TAKEOFFHOOK:
		case TRIS_DIAL_RESULT_TIMEOUT:
		case TRIS_DIAL_RESULT_HANGUP:
		case TRIS_DIAL_RESULT_INVALID:
		case TRIS_DIAL_RESULT_FAILED:
		case TRIS_DIAL_RESULT_UNANSWERED:
			done = 1;
		case TRIS_DIAL_RESULT_TRYING:
		case TRIS_DIAL_RESULT_RINGING:
		case TRIS_DIAL_RESULT_PROGRESS:
		case TRIS_DIAL_RESULT_PROCEEDING:
			break;
		}
		if(tris_waitfordigit(chan,10) == '*') {
			tris_dial_join(dial);
			tris_dial_destroy(dial);
			tris_play_and_wait(chan, "conference/calling-cancelled");
			return 0;
		}
		if (done)
			break;
	}
	
	switch (dial_res) {
	case TRIS_DIAL_RESULT_ANSWERED:
		tris_stopstream(chan);
		callee_chan = tris_dial_answered(dial);
		break;
	case TRIS_DIAL_RESULT_BUSY:
	case TRIS_DIAL_RESULT_CONGESTION:
		tris_play_and_wait(chan, "conference/pbx-busy");
		break;
	case TRIS_DIAL_RESULT_FORBIDDEN:
		tris_play_and_wait(chan, "conference/pbx-forbidden");
		break;
	case TRIS_DIAL_RESULT_OFFHOOK:
		tris_play_and_wait(chan, "conference/pbx-not-found");
		break;
	case TRIS_DIAL_RESULT_TAKEOFFHOOK:
		tris_play_and_wait(chan, "conference/pbx-not-registered");
		break;
	case TRIS_DIAL_RESULT_TIMEOUT:
		tris_play_and_wait(chan, "conference/pbx-no-answer");
		break;
	default:
		break;
	}
	
	if (!callee_chan) {
		tris_dial_join(dial);
		tris_dial_destroy(dial);
		return 0;
	}
	
	dials[(*pos)++] = dial;
	tris_verbose("--------------------------------\n");
	
	//tris_dial_join(dial);
	//tris_dial_destroy(dial);

	return 1;

}

static int invite_rest_to_meetme(struct tris_conference *conf, struct tris_channel *chan) 
{
	int i;
	struct tris_dial *dial;
	enum tris_dial_result dial_res;

	for (i=0; i<conf->pos; i++) {
		dial = conf->dials[i];
		if (dial && tris_dial_state(dial) != TRIS_DIAL_RESULT_ANSWERED) {
			tris_dial_join(dial);
			tris_dial_hangup(dial);
			dial_res = tris_dial_run(dial, chan, 1, 0);
			
			if (dial_res != TRIS_DIAL_RESULT_TRYING) {
				tris_dial_destroy(dial);
				conf->dials[i] = NULL;
				return 0;
			}
		}
	}
	
	return 0;
}

struct dialplan_obj {
	char *sql;
	char pattern[64];
	SQLLEN err;
};

static SQLHSTMT dialplan_prepare(struct odbc_obj *obj, void *data)
{
	struct dialplan_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_CHAR, q->pattern, sizeof(q->pattern), &q->err);
	//SQLBindCol(sth, 2, SQL_C_CHAR, q->memberuid, sizeof(q->memberuid), &q->err);
	//SQLBindCol(sth, 3, SQL_C_CHAR, q->mempermit, sizeof(q->mempermit), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

static int check_media_service(const char *ext) 
{
	int res = 0;
	struct odbc_obj *obj;
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	struct dialplan_obj q;
	
	char sql[256];

	return 0;
	
	memset(&q, 0, sizeof(q));
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return -1;

	snprintf(sql, sizeof(sql), "SELECT pattern FROM dial_pattern WHERE dp_id = '0' and plan_id = '16'");
	q.sql = sql;
	
	stmt = tris_odbc_prepare_and_execute(obj, dialplan_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
		tris_odbc_release_obj(obj);
		return -1;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return -1;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}
	//while ((tech = strsep(&tmp, "&"))) {
	while ((SQLFetch(stmt)) != SQL_NO_DATA) {

		if(tris_extension_match(q.pattern, ext)) {
			res = 1;
			tris_verbose(" COOL (^_^) Matched!!! OK!!!\n");
			break;
		}

	}
	
	/* remove recording temp file */
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	return res;
}

static void *dial_out(struct tris_channel *chan, struct tris_dial **dials, int *pos, const char* conf_name, const char* data, unsigned int extra_flags) 
{
	struct tris_dial *dial;
	char *tech, *tech_data;
	enum tris_dial_result dial_res;
	char dialout_conf_name[128];
	char meetmeopts[81];
	char *optargs[OPT_ARG_ARRAY_SIZE] = { NULL, };
	struct tris_conference *dialout_conf = NULL;
	struct tris_flags conf_flags = { 0 };
	struct tris_channel *callee_chan = NULL;
	//int res = 0;

	tris_set_flag(&conf_flags, 
		CONFFLAG_ADMIN | CONFFLAG_QUIET | CONFFLAG_MARKEDEXIT |CONFFLAG_MARKEDUSER | 
		CONFFLAG_PASS_DTMF | CONFFLAG_DYNAMIC |
		CONFFLAG_DIALOUT);
	
	if(extra_flags) 
		tris_set_flag(&conf_flags, extra_flags);

	optargs[OPT_ARG_DIALOUT_MAINCONFID] = NULL;
	//conf = build_conf(dialout_conf_name, "", "", 1, 1, 1, chan);

	if (!(dial = tris_dial_create())) {
		return NULL;
	}

	tech_data = tris_strdupa(data);
	tech = strsep(&tech_data, "/");
	if (tris_dial_append(dial, tech, tech_data) == -1) {
		tris_dial_destroy(dial);
		return NULL;
	}

	snprintf(dialout_conf_name, sizeof(dialout_conf_name), "%s", chan->uniqueid);
	snprintf(meetmeopts, sizeof(meetmeopts), "MeetMe,%s,xqo(%s)d", dialout_conf_name,conf_name);
	tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC,meetmeopts);
	
	dial_res = tris_dial_run(dial, chan, 1, 0);

	if (dial_res != TRIS_DIAL_RESULT_TRYING) {
		tris_dial_destroy(dial);
		return NULL;
	}
	//tris_indicate(chan, TRIS_CONTROL_RINGING);
	tris_streamfile(chan, "conference/ringing", chan->language);
	for (;;) {
		unsigned int done = 0;
		switch ((dial_res = tris_dial_state(dial))) {
		case TRIS_DIAL_RESULT_ANSWERED:
			callee_chan = tris_dial_answered(dial);
			done =1;
			break;
		case TRIS_DIAL_RESULT_BUSY:
		case TRIS_DIAL_RESULT_CONGESTION:
		case TRIS_DIAL_RESULT_FORBIDDEN:
		case TRIS_DIAL_RESULT_OFFHOOK:
		case TRIS_DIAL_RESULT_TAKEOFFHOOK:
		case TRIS_DIAL_RESULT_TIMEOUT:
		case TRIS_DIAL_RESULT_HANGUP:
		case TRIS_DIAL_RESULT_INVALID:
		case TRIS_DIAL_RESULT_FAILED:
		case TRIS_DIAL_RESULT_UNANSWERED:
			done = 1;
		case TRIS_DIAL_RESULT_TRYING:
		case TRIS_DIAL_RESULT_RINGING:
		case TRIS_DIAL_RESULT_PROGRESS:
		case TRIS_DIAL_RESULT_PROCEEDING:
			break;
		}
		if(tris_waitfordigit(chan,10) == '*') {
			tris_dial_join(dial);
			tris_dial_destroy(dial);
			tris_play_and_wait(chan, "conference/calling-cancelled");
			return NULL;
		}
		if (done)
			break;
	}
	
	switch (dial_res) {
	case TRIS_DIAL_RESULT_ANSWERED:
		tris_stopstream(chan);
		callee_chan = tris_dial_answered(dial);
		break;
	case TRIS_DIAL_RESULT_BUSY:
	case TRIS_DIAL_RESULT_CONGESTION:
		tris_play_and_wait(chan, "conference/pbx-busy");
		break;
	case TRIS_DIAL_RESULT_FORBIDDEN:
		tris_play_and_wait(chan, "conference/pbx-forbidden");
		break;
	case TRIS_DIAL_RESULT_OFFHOOK:
		tris_play_and_wait(chan, "conference/pbx-not-found");
		break;
	case TRIS_DIAL_RESULT_TAKEOFFHOOK:
		tris_play_and_wait(chan, "conference/pbx-not-registered");
		break;
	case TRIS_DIAL_RESULT_TIMEOUT:
		tris_play_and_wait(chan, "conference/pbx-no-answer");
		break;
	default:
		break;
	}
	
	if (!callee_chan) {
		tris_dial_join(dial);
		tris_dial_destroy(dial);
		return NULL;
	}
	
	dials[(*pos)++] = dial;
	dialout_conf = find_conf(chan, dialout_conf_name, 1, 1, "", 0, 1,&conf_flags);

	if (dialout_conf) {
		conf_run(chan, dialout_conf, conf_flags.flags, optargs);
		dispose_conf(dialout_conf);
		dialout_conf = NULL;

		tris_verbose("--------------------------------\n");
	}
	

	/* If the trunk is going away, it is definitely now IDLE. */

	//tris_dial_join(dial);
	//tris_dial_destroy(dial);

	return 0;

}

static struct tris_conference *find_conf_realtime(struct tris_channel *chan, char *confno, int make, int dynamic,
				char *dynamic_pin, size_t pin_buf_len, int refcount, struct tris_flags *confflags,
				char *optargs[], int *too_early)
{
	struct tris_variable *var;
	struct tris_conference *cnf;

	*too_early = 0;

	/* Check first in the conference list */
	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, cnf, list) {
		if (!strcmp(confno, cnf->confno)) 
			break;
	}
	if (cnf) {
		cnf->refcount += refcount;
	}
	TRIS_LIST_UNLOCK(&confs);

	if (!cnf) {
		char *pin = NULL, *pinadmin = NULL; /* For temp use */
		int maxusers = 0;
		struct timeval now;
		char currenttime[19] = "";
		char eatime[19] = "";
		char useropts[32] = "";
		char adminopts[32] = "";
		struct tris_tm tm, etm;
		struct timeval endtime = { .tv_sec = 0 };

		if (rt_schedule) {
			now = tris_tvnow();

			tris_localtime(&now, &tm, NULL);
			tris_strftime(currenttime, sizeof(currenttime), DATE_FORMAT, &tm);

			tris_debug(1, "Looking for conference %s that starts after %s\n", confno, eatime);

			var = tris_load_realtime("meetme", "roomno",
				confno, "starttime <= ", currenttime, "endtime >= ",
				currenttime, NULL);

			//var = tris_load_realtime("meetme", "confno",
			//	confno, "starttime <= ", currenttime, "endtime >= ",
			//	currenttime, NULL);

			if (!var && fuzzystart) {
				now = tris_tvnow();
				now.tv_sec += fuzzystart;

				tris_localtime(&now, &tm, NULL);
				tris_strftime(currenttime, sizeof(currenttime), DATE_FORMAT, &tm);
				var = tris_load_realtime("meetme", "roomno",
					confno, "starttime <= ", currenttime, "endtime >= ",
					currenttime, NULL);
			}			

			if (!var && earlyalert) {
				now = tris_tvnow();
				now.tv_sec += earlyalert;
				tris_localtime(&now, &etm, NULL);
				tris_strftime(eatime, sizeof(eatime), DATE_FORMAT, &etm);
				var = tris_load_realtime("meetme", "roomno",
					confno, "starttime <= ", eatime, "endtime >= ",
					currenttime, NULL);
				if (var)
					*too_early = 1;
			}

		} else
			 var = tris_load_realtime("meetme", "roomno", confno, NULL);

		if (!var)
			return NULL;

		if (rt_schedule && *too_early) {
			/* Announce that the caller is early and exit */
			if (!tris_streamfile(chan, "conference/conf-has-not-started", chan->language))
				tris_waitstream(chan, "");
			tris_variables_destroy(var);
			return NULL;
		}

		while (var) {
			if (!strcasecmp(var->name, "pin")) {
				pin = tris_strdupa(var->value);
			} else if (!strcasecmp(var->name, "adminpin")) {
				pinadmin = tris_strdupa(var->value);
			} else if (!strcasecmp(var->name, "opts")) {
				tris_copy_string(useropts, var->value, sizeof(useropts));
			} else if (!strcasecmp(var->name, "maxusers")) {
				maxusers = atoi(var->value);
			} else if (!strcasecmp(var->name, "adminopts")) {
				tris_copy_string(adminopts, var->value, sizeof(adminopts));
			} else if (!strcasecmp(var->name, "endtime")) {
				union {
					struct tris_tm atm;
					struct tm tm;
				} t = { { 0, }, };
				strptime(var->value, "%Y-%m-%d %H:%M:%S", &t.tm);
				/* strptime does not determine if a time is
				 * in DST or not.  Set tm_isdst to -1 to 
				 * allow tris_mktime to adjust for DST 
				 * if needed */
				t.tm.tm_isdst = -1; 
				endtime = tris_mktime(&t.atm, NULL);
			}

			var = var->next;
		}

		tris_variables_destroy(var);

		cnf = build_conf(confno, pin ? pin : "", pinadmin ? pinadmin : "", make, dynamic, refcount, chan);

		if (cnf) {
			cnf->maxusers = maxusers;
			cnf->endalert = endalert;
			cnf->endtime = endtime.tv_sec;
		}
	}

	if (cnf) {
		if (confflags && !cnf->chan &&
		    !tris_test_flag(confflags, CONFFLAG_QUIET) &&
		    tris_test_flag(confflags, CONFFLAG_INTROUSER)) {
			tris_log(LOG_WARNING, "No DAHDI channel available for conference, user introduction disabled (is chan_dahdi loaded?)\n");
			tris_clear_flag(confflags, CONFFLAG_INTROUSER);
		}
		
		if (confflags && !cnf->chan &&
		    tris_test_flag(confflags, CONFFLAG_RECORDCONF)) {
			tris_log(LOG_WARNING, "No DAHDI channel available for conference, conference recording disabled (is chan_dahdi loaded?)\n");
			tris_clear_flag(confflags, CONFFLAG_RECORDCONF);
		}
	}

	return cnf;
}


static struct tris_conference *find_conf(struct tris_channel *chan, char *confno, int make, int dynamic,
					char *dynamic_pin, size_t pin_buf_len, int refcount, struct tris_flags *confflags)
{
	struct tris_config *cfg;
	struct tris_variable *var;
	struct tris_flags config_flags = { 0 };
	struct tris_conference *cnf;
	char *parse;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(confno);
		TRIS_APP_ARG(pin);
		TRIS_APP_ARG(pinadmin);
	);

	/* Check first in the conference list */
	tris_debug(1, "The requested confno is '%s'?\n", confno);
	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, cnf, list) {
		tris_debug(3, "Does conf %s match %s?\n", confno, cnf->confno);
		if (!strcmp(confno, cnf->confno)) 
			break;
	}
	if (cnf) {
		cnf->refcount += refcount;
	}
	TRIS_LIST_UNLOCK(&confs);

	if (!cnf) {
		if (dynamic) {
			/* No need to parse meetme.conf */
			tris_debug(1, "Building dynamic conference '%s'\n", confno);
			if (dynamic_pin) {
				if (dynamic_pin[0] == 'q') {
					/* Query the user to enter a PIN */
					if (tris_app_getdata(chan, "conference/conf-getpin", dynamic_pin, pin_buf_len - 1, 0) < 0)
						return NULL;
				}
				cnf = build_conf(confno, dynamic_pin, "", make, dynamic, refcount, chan);
			} else {
				cnf = build_conf(confno, "", "", make, dynamic, refcount, chan);
			}
		} else {
			/* Check the config */
			cfg = tris_config_load(CONFIG_FILE_NAME, config_flags);
			if (!cfg) {
				tris_log(LOG_WARNING, "No %s file :(\n", CONFIG_FILE_NAME);
				return NULL;
			}
			for (var = tris_variable_browse(cfg, "rooms"); var; var = var->next) {
				if (strcasecmp(var->name, "conf"))
					continue;
				
				if (!(parse = tris_strdupa(var->value)))
					return NULL;
				
				TRIS_STANDARD_APP_ARGS(args, parse);
				tris_debug(3, "Will conf %s match %s?\n", confno, args.confno);
				if (!strcasecmp(args.confno, confno)) {
					/* Bingo it's a valid conference */
					cnf = build_conf(args.confno,
							S_OR(args.pin, ""),
							S_OR(args.pinadmin, ""),
							make, dynamic, refcount, chan);
					break;
				}
			}
			if (!var) {
				tris_debug(1, "%s isn't a valid conference\n", confno);
			}
			tris_config_destroy(cfg);
		}
	} else if (dynamic_pin) {
		/* Correct for the user selecting 'D' instead of 'd' to have
		   someone join into a conference that has already been created
		   with a pin. */
		if (dynamic_pin[0] == 'q')
			dynamic_pin[0] = '\0';
	}

	if (cnf) {
		if (confflags && !cnf->chan &&
		    !tris_test_flag(confflags, CONFFLAG_QUIET) &&
		    tris_test_flag(confflags, CONFFLAG_INTROUSER)) {
			tris_log(LOG_WARNING, "No DAHDI channel available for conference, user introduction disabled (is chan_dahdi loaded?)\n");
			tris_clear_flag(confflags, CONFFLAG_INTROUSER);
		}
		
		if (confflags && !cnf->chan &&
		    tris_test_flag(confflags, CONFFLAG_RECORDCONF)) {
			tris_log(LOG_WARNING, "No DAHDI channel available for conference, conference recording disabled (is chan_dahdi loaded?)\n");
			tris_clear_flag(confflags, CONFFLAG_RECORDCONF);
		}
	}

	return cnf;
}

static int get_monitor_fn(struct tris_channel * chan, char *s, int len)
{
	struct tris_tm tm;
	struct timeval t = tris_tvnow();
	
	tris_localtime(&t, &tm, NULL);
	return snprintf(s, len, "%04d%02d%02d-%02d%02d%02d-%s-%s", 
		tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, 
		tm.tm_sec, S_OR(chan->cid.cid_num, ""), S_OR(chan->appl,""));
}

static int check_mark(struct tris_channel *chan)
{
	char result[80], sql[256];

	snprintf(sql, sizeof(sql), "SELECT extension FROM user_info where (uid='%s' or extension='%s') AND tapstart = 1",
		chan->cid.cid_num, chan->cid.cid_num);
	sql_select_query_execute(result, sql);
	if (!tris_strlen_zero(result))
		return 1;

	snprintf(sql, sizeof(sql), "SELECT pattern FROM mark_pattern WHERE pattern = '%s'", chan->cid.cid_num);
	sql_select_query_execute(result, sql);
	if (!tris_strlen_zero(result))
		return 1;

	return 0;
}

static void exec_monitor(struct tris_channel *chan) 
{
	char mfn[256], args[256];
	struct tris_app *tris_app = pbx_findapp("Monitor");

	/* If the application was not found, return immediately */
	if (!tris_app)
		return;

	if (!check_mark(chan))
		return;
	
	get_monitor_fn(chan, mfn, sizeof(mfn));
	snprintf(args, sizeof(args), ",%s,m", mfn);
	/* All is well... execute the application */
	pbx_exec(chan, tris_app, args);
}

static struct tris_conf_user *get_user(struct tris_conference *conf, char *exten) 
{
	struct tris_conf_user *user = NULL;
	
	if (conf && !tris_strlen_zero(exten)) {
		TRIS_LIST_TRAVERSE(&conf->userlist, user, list) {
			if (!strcasecmp(exten, user->chan->cid.cid_num))
				return user;
		}
	}
	return NULL;
}

static int kick_user(struct tris_conference *conf, char *exten) 
{
	struct tris_conf_user *user;
	if(tris_strlen_zero(exten)) 
		return -1;
	
	user = get_user(conf, exten);
	if (user)
		user->adminflags |= ADMINFLAG_KICKME;
	else
		return -1;
	
	return 0;
}

struct trisconf_obj {
	char *sql;
	char roomno[12];
	char memberuid[64];
	char mempermit[32];
	SQLLEN err;
};

static SQLHSTMT trisconf_prepare(struct odbc_obj *obj, void *data)
{
	struct trisconf_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_CHAR, q->roomno, sizeof(q->roomno), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->memberuid, sizeof(q->memberuid), &q->err);
	SQLBindCol(sth, 3, SQL_C_CHAR, q->mempermit, sizeof(q->mempermit), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

static int check_callconf_sponser(char *roomno, const char * ext)
{
	char sql[256];
	char result[1024];
	char *tmp = 0, *cur;

	snprintf(sql, sizeof(sql), "SELECT sponseruid FROM callconf_room WHERE sponseruid REGEXP '.*%s.*' AND roomno = '%s'",
			ext, roomno);
	sql_select_query_execute(result, sql);
	
	if(tris_strlen_zero(result)){
		return 0;
	}

	cur = result;
	while (cur) {
		tmp = strsep(&cur, ",");
		if (!tmp)
			return 0;
		if (strlen(tmp) == strlen(ext) && !strncmp(tmp, ext, strlen(ext))) {
			return 1;
		}
	}

	return 0;

}

static int invite_callconf_member(struct tris_channel *chan, struct tris_conference *conf, int confflags)
{
	char meetmeopts[88];
	char adminopts[88];
	char onlylistenopts[88];
	char *p_opts = meetmeopts;
	int res = 0;
	char sql[256];
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	char calling_uri[100];
	struct odbc_obj *obj;
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	struct trisconf_obj q;
	enum tris_dial_result dial_res;
	char numbuf[256];
	char *roomid;
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);

	snprintf(numbuf, sizeof(numbuf), "%s", conf->confno);
	roomid = strchr(numbuf, '-');
	if (roomid)
		*roomid++ = '\0';

	snprintf(meetmeopts, sizeof(meetmeopts), "MeetMe,%s,dxq", conf->confno);
	snprintf(adminopts, sizeof(adminopts), "MeetMe,%s,dqA", conf->confno);

	snprintf(onlylistenopts, sizeof(onlylistenopts), "MeetMe,%s,dmxq", conf->confno);

	/* Go through parsing/calling each device */
	memset(&q, 0, sizeof(q));
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;

	snprintf(sql, sizeof(sql), "SELECT roomno, memberuid, mempermit FROM callconf_member WHERE roomid = '%s'", roomid);
	q.sql = sql;
	
	stmt = tris_odbc_prepare_and_execute(obj, trisconf_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}
	while ((SQLFetch(stmt)) != SQL_NO_DATA) {

		struct tris_dial *dial = NULL;

		if (!check_callconf_sponser(numbuf, q.memberuid)) {
			if(!strcmp(q.mempermit, "1"))
				p_opts = meetmeopts;
			else
				p_opts = onlylistenopts;
		} else {
			p_opts = adminopts;
		}

		if (conf->pos >= MAX_DIALS)
			continue;
		sprintf(calling_uri,"%s@%s:5060", q.memberuid, tris_inet_ntoa(ourip));
		
		/* Create a dialing structure */
		if (!(dial = tris_dial_create())) {
			tris_log(LOG_WARNING, "Failed to create dialing structure.\n");
			continue;
		}

		/* Append technology and resource */
		tris_dial_append(dial, "SIP", calling_uri);

		/* Set ANSWER_EXEC as global option */
		tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC, p_opts);

		/* Run this dial in async mode */
		dial_res = tris_dial_run(dial, chan, 1, conf->maxreferid++);

		if (dial_res != TRIS_DIAL_RESULT_TRYING) {
			tris_dial_destroy(dial);
			return 0;
		}
		
		/* Put in our dialing array */
		conf->dials[conf->pos++] = dial;
	}

	tris_verbose("hsh commented....\n");

	/* remove recording temp file */
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);

	return -1;
}

static int handle_conf_refer(struct tris_channel *chan, struct tris_conference *conf, int confflags)
{
	const char *exten;
	char dest[256], data[256];
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	struct tris_dial *dial = NULL;
	enum tris_dial_result dial_res;
	char confbuf[256];
	char *confid;
	int id = 0, i;

	exten = chan->referexten;
	id = chan->referidval;
	if (chan->refertype != TRIS_REFER_TYPE_CONF) {
		return 0;
	}

	for (i=0; i<conf->pos; i++) {
		if (conf->dials[i] && tris_dial_check(conf->dials[i], id)) {
			dial = conf->dials[i];
			break;
		}
	}
	
	if (dial) {
		tris_dial_join(dial);
		tris_dial_hangup(dial);
		dial_res = tris_dial_run(dial, chan, 1, id);
		
		if (dial_res != TRIS_DIAL_RESULT_TRYING) {
			tris_dial_destroy(dial);
			conf->dials[i] = NULL;
			return 0;
		}
		return 0;
	}
	if (conf->pos >= MAX_DIALS) {
		tris_log(LOG_WARNING, "Conf size exceed max dial size\n");
		return 0;
	}
	if (!(dial = tris_dial_create())) {
		return 0;
	}
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);
	
	snprintf(dest, sizeof(dest), "%s@%s:5060", exten, tris_inet_ntoa(ourip));
	snprintf(confbuf, sizeof(confbuf), "%s", conf->confno);
	confid = strchr(confbuf, '-');
	if (confid)
		*confid = '\0';
	
	if (!check_callconf_sponser(confbuf, exten))
		snprintf(data, sizeof(data), "MeetMe,%s,dxq", conf->confno);
	else
		snprintf(data, sizeof(data), "MeetMe,%s,dqA", conf->confno);

	if (tris_dial_append(dial, "SIP", dest) == -1) {
		tris_dial_destroy(dial);
		return 0;
	}
	
	tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC, data);
	
	dial_res = tris_dial_run(dial, chan, 1, id);
	
	if (dial_res != TRIS_DIAL_RESULT_TRYING) {
		tris_dial_destroy(dial);
		return 0;
	}
	
	conf->dials[conf->pos++] = dial;
	tris_verbose("--------------------------------\n");
		
	return 0;
}

static int bye_member_byreferid(struct tris_channel *chan, struct tris_conference *conf, int referid)
{
	struct tris_dial *dial = NULL;
	int i, j;
	for (i=0; i<conf->pos; i++) {
		if (conf->dials[i] && tris_dial_check(conf->dials[i], referid)) {
			dial = conf->dials[i];
			for (j=i; j<conf->pos-1; j++) {
				conf->dials[j] = conf->dials[j+1];
			}
			conf->pos--;
			tris_dial_send_notify(dial, "", TRIS_CONTROL_NOTIFY_BYE);
			tris_dial_join(dial);
			tris_dial_hangup(dial);
			tris_dial_destroy(dial);
			tris_log(LOG_DEBUG, "Found proper dial: %d\n", referid);
			return 1;
		}
	}
	tris_log(LOG_WARNING, "Not found proper dial: %d\n", referid);
	return 0;
}

static int bye_member_byuser(struct tris_channel *chan, struct tris_conference *conf, int referid)
{
	struct tris_dial *dial = NULL;
	int i;
	for (i=0; i<conf->pos; i++) {
		if (conf->dials[i] && tris_dial_check(conf->dials[i], referid)) {
			dial = conf->dials[i];
			tris_dial_send_notify(dial, "", TRIS_CONTROL_NOTIFY_BYE);
			tris_dial_join(dial);
			tris_dial_hangup(dial);
			//tris_dial_destroy(dial);
			tris_log(LOG_DEBUG, "Found proper dial: %d\n", referid);
			return 1;
		}
	}
	tris_log(LOG_WARNING, "Not found proper dial: %d\n", referid);
	return 0;
}

static int handle_conf_refer_info(struct tris_channel *chan, struct tris_conference *conf, int confflags)
{
	int id = 100;
	struct tris_conf_user *user;
	
	if (chan->refertype != TRIS_REFER_TYPE_CONF)
		return 0;
	
	id = chan->referidval;

	if (chan->referaction == TRIS_REFER_ACTION_CANCEL || chan->referaction == TRIS_REFER_ACTION_BYE) {
		TRIS_LIST_TRAVERSE(&conf->userlist, user, list) {
			if (user->chan && user->chan->referid == id) {
				user->adminflags |= ADMINFLAG_KICKME;
				return 0;
			}
		}
		if (bye_member_byreferid(chan, conf, id))
			return 0;
	} else if (chan->referaction == TRIS_REFER_ACTION_MUTE || chan->referaction == TRIS_REFER_ACTION_UNMUTE) {
		TRIS_LIST_TRAVERSE(&conf->userlist, user, list) {
			if (user->chan && user->chan->referid == id) {
				if (chan->referaction == TRIS_REFER_ACTION_MUTE)
					user->adminflags |= ADMINFLAG_MUTED;	/* request user muting */
				else
					user->adminflags &= ~(ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED | ADMINFLAG_T_REQUEST); /* request user unmuting */
				return 0;
			}
		}
	}

	return 0;
}

static int unset_admin_channel(struct tris_channel *chan, struct tris_conference *conf, int confflags)
{
	int i;
	for (i=0; i<conf->pos; i++) {
		if (conf->dials[i])
			tris_dial_unset_chan(conf->dials[i]);
	}
	return 0;
}

static int conf_run(struct tris_channel *chan, struct tris_conference *conf, int confflags, char *optargs[])
{
	struct tris_conf_user *user = NULL;
	struct tris_conf_user *usr = NULL;
	struct tris_dial *dials[MAX_DIALS];
	int i, pos = 0;
	int fd;
	struct dahdi_confinfo dahdic, dahdic_empty;
	struct tris_frame *f;
	struct tris_channel *c;
	struct tris_frame fr;
	int outfd;
	int ms;
	int nfds;
	int res;
	int retrydahdi;
	int origfd;
	int musiconhold = 0, mohtempstopped = 0;
	int firstpass = 0;
	int lastmarked = 0;
	int currentmarked = 0;
	int ret = -1;
	int x;
	int menu_active = 0;
	int talkreq_manager = 0;
	int using_pseudo = 0;
	int hr, min, sec;
	int sent_event = 0;
	int checked = 0;
	int announcement_played = 0;
	struct timeval now;
	struct tris_dsp *dsp = NULL;
	struct tris_app *agi_app;
	char *agifile;
	const char *agifiledefault = "conference/conf-background.agi", *tmpvar;
	char meetmesecs[30] = "";
	char exitcontext[TRIS_MAX_CONTEXT] = "";
	char recordingtmp[256] = "";
	char members[10] = "";
	int dtmf, opt_waitmarked_timeout = 0;
	time_t timeout = 0;
	struct dahdi_bufferinfo bi;
	char __buf[CONF_SIZE + TRIS_FRIENDLY_OFFSET];
	char *buf = __buf + TRIS_FRIENDLY_OFFSET;
	char *exitkeys = NULL;
 	unsigned int calldurationlimit = 0;
 	long timelimit = 0;
 	long play_warning = 0;
 	long warning_freq = 0;
 	const char *warning_sound = NULL;
 	const char *end_sound = NULL;
 	char *parse;	
 	long time_left_ms = 0;
 	struct timeval nexteventts = { 0, };
 	int to;
	int setusercount = 0;
	int confsilence = 0, totalsilence = 0;

	if (!(user = tris_calloc(1, sizeof(*user))))
		return ret;

	/* Possible timeout waiting for marked user */
	if ((confflags & CONFFLAG_WAITMARKED) &&
		!tris_strlen_zero(optargs[OPT_ARG_WAITMARKED]) &&
		(sscanf(optargs[OPT_ARG_WAITMARKED], "%30d", &opt_waitmarked_timeout) == 1) &&
		(opt_waitmarked_timeout > 0)) {
		timeout = time(NULL) + opt_waitmarked_timeout;
	}
	 	
 	if ((confflags & CONFFLAG_DURATION_STOP) && !tris_strlen_zero(optargs[OPT_ARG_DURATION_STOP])) {
 		calldurationlimit = atoi(optargs[OPT_ARG_DURATION_STOP]);
 		tris_verb(3, "Setting call duration limit to %d seconds.\n", calldurationlimit);
 	}
 	
 	if ((confflags & CONFFLAG_DURATION_LIMIT) && !tris_strlen_zero(optargs[OPT_ARG_DURATION_LIMIT])) {
 		char *limit_str, *warning_str, *warnfreq_str;
		const char *var;
 
 		parse = optargs[OPT_ARG_DURATION_LIMIT];
 		limit_str = strsep(&parse, ":");
 		warning_str = strsep(&parse, ":");
 		warnfreq_str = parse;
 
 		timelimit = atol(limit_str);
 		if (warning_str)
 			play_warning = atol(warning_str);
 		if (warnfreq_str)
 			warning_freq = atol(warnfreq_str);
 
 		if (!timelimit) {
 			timelimit = play_warning = warning_freq = 0;
 			warning_sound = NULL;
 		} else if (play_warning > timelimit) {			
 			if (!warning_freq) {
 				play_warning = 0;
 			} else {
 				while (play_warning > timelimit)
 					play_warning -= warning_freq;
 				if (play_warning < 1)
 					play_warning = warning_freq = 0;
 			}
 		}
 		
		tris_channel_lock(chan);
		if ((var = pbx_builtin_getvar_helper(chan, "CONF_LIMIT_WARNING_FILE"))) {
			var = tris_strdupa(var);
		}
		tris_channel_unlock(chan);

 		warning_sound = var ? var : "timeleft";
 		
		tris_channel_lock(chan);
		if ((var = pbx_builtin_getvar_helper(chan, "CONF_LIMIT_TIMEOUT_FILE"))) {
			var = tris_strdupa(var);
		}
		tris_channel_unlock(chan);
 		
		end_sound = var ? var : NULL;
 			
 		/* undo effect of S(x) in case they are both used */
 		calldurationlimit = 0;
 		/* more efficient do it like S(x) does since no advanced opts */
 		if (!play_warning && !end_sound && timelimit) { 
 			calldurationlimit = timelimit / 1000;
 			timelimit = play_warning = warning_freq = 0;
 		} else {
 			tris_debug(2, "Limit Data for this call:\n");
			tris_debug(2, "- timelimit     = %ld\n", timelimit);
 			tris_debug(2, "- play_warning  = %ld\n", play_warning);
 			tris_debug(2, "- warning_freq  = %ld\n", warning_freq);
 			tris_debug(2, "- warning_sound = %s\n", warning_sound ? warning_sound : "UNDEF");
 			tris_debug(2, "- end_sound     = %s\n", end_sound ? end_sound : "UNDEF");
 		}
 	}

	/* Get exit keys */
	if ((confflags & CONFFLAG_KEYEXIT)) {
		if (!tris_strlen_zero(optargs[OPT_ARG_EXITKEYS]))
			exitkeys = tris_strdupa(optargs[OPT_ARG_EXITKEYS]);
		else
			exitkeys = tris_strdupa("#"); /* Default */
	}
	
	if (confflags & CONFFLAG_RECORDCONF) {
		if (!conf->recordingfilename) {
			const char *var;
			tris_channel_lock(chan);
			if ((var = pbx_builtin_getvar_helper(chan, "MEETME_RECORDINGFILE"))) {
				conf->recordingfilename = tris_strdup(var);
			}
			if ((var = pbx_builtin_getvar_helper(chan, "MEETME_RECORDINGFORMAT"))) {
				conf->recordingformat = tris_strdup(var);
			}
			tris_channel_unlock(chan);
			if (!conf->recordingfilename) {
				struct tris_tm tm;
				struct timeval t = tris_tvnow();
				
				tris_localtime(&t, &tm, NULL);
				snprintf(recordingtmp, sizeof(recordingtmp), "%s/satellite/conf-rec-%s-%s-%04d%02d%02d-%02d%02d%02d", 
					tris_config_TRIS_MONITOR_DIR, conf->confno, S_OR(chan->cid.cid_num, "<unknown>"),tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, 
					tm.tm_hour, tm.tm_min, tm.tm_sec);
				conf->recordingfilename = tris_strdup(recordingtmp);
			}
			if (!conf->recordingformat) {
				conf->recordingformat = tris_strdup("wav");
			}
			tris_verb(4, "Starting recording of MeetMe Conference %s into file %s.%s.\n",
				    conf->confno, conf->recordingfilename, conf->recordingformat);
		}
	}

	tris_mutex_lock(&conf->recordthreadlock);
	if ((conf->recordthread == TRIS_PTHREADT_NULL) && (confflags & CONFFLAG_RECORDCONF) && ((conf->lchan = tris_request("DAHDI", TRIS_FORMAT_SLINEAR, "pseudo", NULL, 0)))) {
		tris_set_read_format(conf->lchan, TRIS_FORMAT_SLINEAR);
		tris_set_write_format(conf->lchan, TRIS_FORMAT_SLINEAR);
		dahdic.chan = 0;
		dahdic.confno = conf->dahdiconf;
		dahdic.confmode = DAHDI_CONF_CONFANN | DAHDI_CONF_CONFANNMON;
		if (ioctl(conf->lchan->fds[0], DAHDI_SETCONF, &dahdic)) {
			tris_log(LOG_WARNING, "Error starting listen channel\n");
			tris_hangup(conf->lchan);
			conf->lchan = NULL;
		} else {
			tris_pthread_create_detached_background(&conf->recordthread, NULL, recordthread, conf);
		}
	}
	tris_mutex_unlock(&conf->recordthreadlock);

	tris_mutex_lock(&conf->announcethreadlock);
	if ((conf->announcethread == TRIS_PTHREADT_NULL) && !(confflags & CONFFLAG_QUIET) && ((confflags & CONFFLAG_INTROUSER) || (confflags & CONFFLAG_INTROUSERNOREVIEW))) {
		tris_mutex_init(&conf->announcelistlock);
		TRIS_LIST_HEAD_INIT_NOLOCK(&conf->announcelist);
		tris_pthread_create_background(&conf->announcethread, NULL, announce_thread, conf);
	}
	tris_mutex_unlock(&conf->announcethreadlock);

	time(&user->jointime);
	
	user->timelimit = timelimit;
	user->play_warning = play_warning;
	user->warning_freq = warning_freq;
	user->warning_sound = warning_sound;
	user->end_sound = end_sound;	
	
	if (calldurationlimit > 0) {
		time(&user->kicktime);
		user->kicktime = user->kicktime + calldurationlimit;
	}
	
	if (tris_tvzero(user->start_time))
		user->start_time = tris_tvnow();
	time_left_ms = user->timelimit;
	
	if (user->timelimit) {
		nexteventts = tris_tvadd(user->start_time, tris_samp2tv(user->timelimit, 1000));
		nexteventts = tris_tvsub(nexteventts, tris_samp2tv(user->play_warning, 1000));
	}

	if (conf->locked && (!(confflags & CONFFLAG_ADMIN))) {
		/* Sorry, but this conference is locked! */	
		if (!tris_streamfile(chan, "conference/conf-locked", chan->language))
			tris_waitstream(chan, "");
		goto outrun;
	}

   	tris_mutex_lock(&conf->playlock);

	if (TRIS_LIST_EMPTY(&conf->userlist))
		user->user_no = 1;
	else
		user->user_no = TRIS_LIST_LAST(&conf->userlist)->user_no + 1;

	if (rt_schedule && conf->maxusers)
		if (conf->users >= conf->maxusers) {
			/* Sorry, but this confernce has reached the participant limit! */	
			if (!tris_streamfile(chan, "conference/conf-full", chan->language))
				tris_waitstream(chan, "");
			tris_mutex_unlock(&conf->playlock);
			user->user_no = 0;
			goto outrun;
		}

	TRIS_LIST_INSERT_TAIL(&conf->userlist, user, list);

	user->chan = chan;
	user->userflags = confflags;
	user->adminflags = (confflags & CONFFLAG_STARTMUTED) ? ADMINFLAG_SELFMUTED : 0;
	user->talking = -1;

	tris_mutex_unlock(&conf->playlock);

/*	if (!(confflags & CONFFLAG_QUIET) && ((confflags & CONFFLAG_INTROUSER) || (confflags & CONFFLAG_INTROUSERNOREVIEW))) {
		char destdir[PATH_MAX];

		snprintf(destdir, sizeof(destdir), "%s/meetme", tris_config_TRIS_SPOOL_DIR);

		if (tris_mkdir(destdir, 0777) != 0) {
			tris_log(LOG_WARNING, "mkdir '%s' failed: %s\n", destdir, strerror(errno));
			goto outrun;
		}

		snprintf(user->namerecloc, sizeof(user->namerecloc),
			 "%s/meetme-username-%s-%d", destdir,
			 conf->confno, user->user_no);
		if (confflags & CONFFLAG_INTROUSERNOREVIEW)
			res = tris_play_and_record(chan, "voicemail/vm-rec-name", user->namerecloc, 10, "sln", &duration, tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE), 0, NULL);
		else
			res = tris_record_review(chan, "voicemail/vm-rec-name", user->namerecloc, 10, "sln", &duration, NULL);
		if (res == -1)
			goto outrun;
	}*/

	tris_mutex_lock(&conf->playlock);

	if (confflags & CONFFLAG_MARKEDUSER)
		conf->markedusers++;
	conf->users++;
	if (rt_log_members) {
		/* Update table */
		snprintf(members, sizeof(members), "%d", conf->users);
		tris_realtime_require_field("meetme",
			"confno", strlen(conf->confno) > 7 ? RQ_UINTEGER4 : strlen(conf->confno) > 4 ? RQ_UINTEGER3 : RQ_UINTEGER2, strlen(conf->confno),
			"members", RQ_UINTEGER1, strlen(members),
			NULL);
		tris_update_realtime("meetme", "roomno", conf->confno, "members", members, NULL);
	}
	setusercount = 1;

	/* This device changed state now - if this is the first user */
	if (conf->users == 1)
		tris_devstate_changed(TRIS_DEVICE_INUSE, "meetme:%s", conf->confno);

	tris_mutex_unlock(&conf->playlock);

	/* return the unique ID of the conference */
	pbx_builtin_setvar_helper(chan, "MEETMEUNIQUEID", conf->uniqueid);

	if (confflags & CONFFLAG_EXIT_CONTEXT) {
		tris_channel_lock(chan);
		if ((tmpvar = pbx_builtin_getvar_helper(chan, "MEETME_EXIT_CONTEXT"))) {
			tris_copy_string(exitcontext, tmpvar, sizeof(exitcontext));
		} else if (!tris_strlen_zero(chan->macrocontext)) {
			tris_copy_string(exitcontext, chan->macrocontext, sizeof(exitcontext));
		} else {
			tris_copy_string(exitcontext, chan->context, sizeof(exitcontext));
		}
		tris_channel_unlock(chan);
	}

/*	if (!(confflags & (CONFFLAG_QUIET | CONFFLAG_NOONLYPERSON))) {
		if (conf->users == 1 && !(confflags & CONFFLAG_WAITMARKED))
			if (!tris_streamfile(chan, "conference/conf-onlyperson", chan->language))
				tris_waitstream(chan, "");
		if ((confflags & CONFFLAG_WAITMARKED) && conf->markedusers == 0)
			if (!tris_streamfile(chan, "conference/conf-waitforleader", chan->language))
				tris_waitstream(chan, "");
	}

	if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_ANNOUNCEUSERCOUNT) && conf->users > 1) {
		int keepplaying = 1;

		if (conf->users == 2) { 
			if (!tris_streamfile(chan, "conference/conf-onlyone", chan->language)) {
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
				tris_stopstream(chan);
				if (res > 0)
					keepplaying = 0;
				else if (res == -1)
					goto outrun;
			}
		} else { 
			if (!tris_streamfile(chan, "conference/conf-thereare", chan->language)) {
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
				tris_stopstream(chan);
				if (res > 0)
					keepplaying = 0;
				else if (res == -1)
					goto outrun;
			}
			if (keepplaying) {
				res = tris_say_number(chan, conf->users - 1, TRIS_DIGIT_ANY, chan->language, (char *) NULL);
				if (res > 0)
					keepplaying = 0;
				else if (res == -1)
					goto outrun;
			}
			if (keepplaying && !tris_streamfile(chan, "conference/conf-otherinparty", chan->language)) {
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
				tris_stopstream(chan);
				if (res > 0)
					keepplaying = 0;
				else if (res == -1) 
					goto outrun;
			}
		}
	}*/

	if (!(confflags & CONFFLAG_NO_AUDIO_UNTIL_UP)) {
		/* We're leaving this alone until the state gets changed to up */
		tris_indicate(chan, -1);
	}

	if (tris_set_write_format(chan, TRIS_FORMAT_SLINEAR) < 0) {
		tris_log(LOG_WARNING, "Unable to set '%s' to write linear mode\n", chan->name);
		goto outrun;
	}

	if (tris_set_read_format(chan, TRIS_FORMAT_SLINEAR) < 0) {
		tris_log(LOG_WARNING, "Unable to set '%s' to read linear mode\n", chan->name);
		goto outrun;
	}

	retrydahdi = (strcasecmp(chan->tech->type, "DAHDI") || (chan->audiohooks || chan->monitor) ? 1 : 0);
	user->dahdichannel = !retrydahdi;

 dahdiretry:
	origfd = chan->fds[0];
	if (retrydahdi) {
		/* open pseudo in non-blocking mode */
		fd = open("/dev/dahdi/pseudo", O_RDWR | O_NONBLOCK);
		if (fd < 0) {
			tris_log(LOG_WARNING, "Unable to open pseudo channel: %s\n", strerror(errno));
			goto outrun;
		}
		using_pseudo = 1;
		/* Setup buffering information */
		memset(&bi, 0, sizeof(bi));
		bi.bufsize = CONF_SIZE / 2;
		bi.txbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.rxbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.numbufs = audio_buffers;
		if (ioctl(fd, DAHDI_SET_BUFINFO, &bi)) {
			tris_log(LOG_WARNING, "Unable to set buffering information: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		x = 1;
		if (ioctl(fd, DAHDI_SETLINEAR, &x)) {
			tris_log(LOG_WARNING, "Unable to set linear mode: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		nfds = 1;
	} else {
		/* XXX Make sure we're not running on a pseudo channel XXX */
		fd = chan->fds[0];
		nfds = 0;
	}
	memset(&dahdic, 0, sizeof(dahdic));
	memset(&dahdic_empty, 0, sizeof(dahdic_empty));
	/* Check to see if we're in a conference... */
	dahdic.chan = 0;	
	if (ioctl(fd, DAHDI_GETCONF, &dahdic)) {
		tris_log(LOG_WARNING, "Error getting conference\n");
		close(fd);
		goto outrun;
	}
	if (dahdic.confmode) {
		/* Whoa, already in a conference...  Retry... */
		if (!retrydahdi) {
			tris_debug(1, "DAHDI channel is in a conference already, retrying with pseudo\n");
			retrydahdi = 1;
			goto dahdiretry;
		}
	}
	memset(&dahdic, 0, sizeof(dahdic));
	/* Add us to the conference */
	dahdic.chan = 0;	
	dahdic.confno = conf->dahdiconf;

	if (tris_strlen_zero(optargs[OPT_ARG_DIALOUT_MAINCONFID]) && !(confflags & CONFFLAG_QUIET) && ((confflags & CONFFLAG_INTROUSER) || (confflags & CONFFLAG_INTROUSERNOREVIEW)) && conf->users > 1) {
		struct announce_listitem *item;
		if (!(item = ao2_alloc(sizeof(*item), NULL)))
			return -1;
		tris_copy_string(item->namerecloc, user->namerecloc, sizeof(item->namerecloc));
		tris_copy_string(item->language, chan->language, sizeof(item->language));
		item->confchan = conf->chan;
		tris_copy_string(item->exten, chan->cid.cid_num, sizeof(item->exten));
		item->confusers = conf->users;
		item->announcetype = CONF_HASJOIN;
		tris_mutex_lock(&conf->announcelistlock);
		ao2_ref(item, +1); /* add one more so we can determine when announce_thread is done playing it */
		TRIS_LIST_INSERT_TAIL(&conf->announcelist, item, entry);
		tris_cond_signal(&conf->announcelist_addition);
		tris_mutex_unlock(&conf->announcelistlock);

		while (!tris_check_hangup(conf->chan) && ao2_ref(item, 0) == 2 && !tris_safe_sleep(chan, 1000)) {
			;
		}
		ao2_ref(item, -1);
	}

	if (confflags & CONFFLAG_WAITMARKED && !conf->markedusers)
		dahdic.confmode = DAHDI_CONF_CONF;
	else if (confflags & CONFFLAG_MONITOR)
		dahdic.confmode = DAHDI_CONF_CONFMON | DAHDI_CONF_LISTENER;
	else if (confflags & CONFFLAG_TALKER)
		dahdic.confmode = DAHDI_CONF_CONF | DAHDI_CONF_TALKER;
	else 
		dahdic.confmode = DAHDI_CONF_CONF | DAHDI_CONF_TALKER | DAHDI_CONF_LISTENER;

	if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
		tris_log(LOG_WARNING, "Error setting conference\n");
		close(fd);
		goto outrun;
	}
	tris_debug(1, "Placed channel %s in DAHDI conf %d\n", chan->name, conf->dahdiconf);

	if (!sent_event) {
		manager_event(EVENT_FLAG_CALL, "MeetmeJoin", 
			        "Channel: %s\r\n"
			        "Uniqueid: %s\r\n"
				"Meetme: %s\r\n"
				"Usernum: %d\r\n"
				"CallerIDnum: %s\r\n"
			      	"CallerIDname: %s\r\n",
			      	chan->name, chan->uniqueid, conf->confno, 
				user->user_no,
				S_OR(user->chan->cid.cid_num, "<unknown>"),
				S_OR(user->chan->cid.cid_name, "<unknown>")
				);
		sent_event = 1;
	}

	if (!firstpass && !(confflags & CONFFLAG_MONITOR) && !(confflags & CONFFLAG_ADMIN)) {
		firstpass = 1;
		if (!(confflags & CONFFLAG_QUIET))
			if (!(confflags & CONFFLAG_WAITMARKED) || ((confflags & CONFFLAG_MARKEDUSER) && (conf->markedusers >= 1)))
				conf_play(chan, conf, ENTER);
	}

	conf_flush(fd, chan);

	if (!(dsp = tris_dsp_new())) {
		tris_log(LOG_WARNING, "Unable to allocate DSP!\n");
		res = -1;
	}

	if (confflags & CONFFLAG_ADMIN && !strncmp(conf->confno, "spg", 3)) {
		invite_callconf_member(chan, conf, confflags);
	}
	if (!(confflags & CONFFLAG_ADMIN) && !chan->referid) {
		chan->referid = conf->maxreferid++;
		if (conf->admin_chan && conf->admin_chan->seqtype) {
			snprintf(conf->admin_chan->refer_phonenum, TRIS_MAX_EXTENSION, "%s", chan->cid.cid_num);
			send_control_notify(conf->admin_chan, TRIS_CONTROL_NOTIFY_ANSWER, chan->referid, 0);
		}
	}
	if (confflags & CONFFLAG_AGI) {
		/* Get name of AGI file to run from $(MEETME_AGI_BACKGROUND)
		   or use default filename of conf-background.agi */

		tris_channel_lock(chan);
		if ((tmpvar = pbx_builtin_getvar_helper(chan, "MEETME_AGI_BACKGROUND"))) {
			agifile = tris_strdupa(tmpvar);
		} else {
			agifile = tris_strdupa(agifiledefault);
		}
		tris_channel_unlock(chan);
		
		if (user->dahdichannel) {
			/*  Set CONFMUTE mode on DAHDI channel to mute DTMF tones */
			x = 1;
			tris_channel_setoption(chan, TRIS_OPTION_TONE_VERIFY, &x, sizeof(char), 0);
		}
		/* Find a pointer to the agi app and execute the script */
		agi_app = pbx_findapp("agi");
		if (agi_app) {
			ret = pbx_exec(chan, agi_app, agifile);
		} else {
			tris_log(LOG_WARNING, "Could not find application (agi)\n");
			ret = -2;
		}
		if (user->dahdichannel) {
			/*  Remove CONFMUTE mode on DAHDI channel */
			x = 0;
			tris_channel_setoption(chan, TRIS_OPTION_TONE_VERIFY, &x, sizeof(char), 0);
		}
	} else {
		if (user->dahdichannel && (confflags & CONFFLAG_STARMENU)) {
			/*  Set CONFMUTE mode on DAHDI channel to mute DTMF tones when the menu is enabled */
			x = 1;
			tris_channel_setoption(chan, TRIS_OPTION_TONE_VERIFY, &x, sizeof(char), 0);
		}	
		for (;;) {
			int menu_was_active = 0;

			outfd = -1;
			ms = -1;
			now = tris_tvnow();

			if (rt_schedule && conf->endtime) {
				char currenttime[32];
				long localendtime = 0;
				int extended = 0;
				struct tris_tm tm;
				struct tris_variable *var, *origvar;
				struct timeval tmp;

				if (now.tv_sec % 60 == 0) {
					if (!checked) {
						tris_localtime(&now, &tm, NULL);
						tris_strftime(currenttime, sizeof(currenttime), DATE_FORMAT, &tm);
						var = origvar = tris_load_realtime("meetme", "confno",
							conf->confno, "starttime <=", currenttime,
							 "endtime >=", currenttime, NULL);

						for ( ; var; var = var->next) {
							if (!strcasecmp(var->name, "endtime")) {
								struct tris_tm endtime_tm;
								tris_strptime(var->value, "%Y-%m-%d %H:%M:%S", &endtime_tm);
								tmp = tris_mktime(&endtime_tm, NULL);
								localendtime = tmp.tv_sec;
							}
						}
						tris_variables_destroy(origvar);

						/* A conference can be extended from the
						   Admin/User menu or by an external source */
						if (localendtime > conf->endtime){
							conf->endtime = localendtime;
							extended = 1;
						}

						if (conf->endtime && (now.tv_sec >= conf->endtime)) {
							tris_verbose("Quitting time...\n");
							goto outrun;
						}

						if (!announcement_played && conf->endalert) {
							if (now.tv_sec + conf->endalert >= conf->endtime) {
								if (!tris_streamfile(chan, "conference/conf-will-end-in", chan->language))
									tris_waitstream(chan, "");
								tris_say_digits(chan, (conf->endtime - now.tv_sec) / 60, "", chan->language);
								if (!tris_streamfile(chan, "minutes", chan->language))
									tris_waitstream(chan, "");
								announcement_played = 1;
							}
						}

						if (extended) {
							announcement_played = 0;
						}

						checked = 1;
					}
				} else {
					checked = 0;
				}
			}

 			if (user->kicktime && (user->kicktime <= now.tv_sec)) {
				break;
			}
  
 			to = -1;
 			if (user->timelimit) {
				int minutes = 0, seconds = 0, remain = 0;
 
 				to = tris_tvdiff_ms(nexteventts, now);
 				if (to < 0) {
 					to = 0;
				}
 				time_left_ms = user->timelimit - tris_tvdiff_ms(now, user->start_time);
 				if (time_left_ms < to) {
 					to = time_left_ms;
				}
 	
 				if (time_left_ms <= 0) {
 					if (user->end_sound) {						
 						res = tris_streamfile(chan, user->end_sound, chan->language);
 						res = tris_waitstream(chan, "");
 					}
 					break;
 				}
 				
 				if (!to) {
 					if (time_left_ms >= 5000) {						
 						
 						remain = (time_left_ms + 500) / 1000;
 						if (remain / 60 >= 1) {
 							minutes = remain / 60;
 							seconds = remain % 60;
 						} else {
 							seconds = remain;
 						}
 						
 						/* force the time left to round up if appropriate */
 						if (user->warning_sound && user->play_warning) {
 							if (!strcmp(user->warning_sound, "timeleft")) {
 								
 								res = tris_streamfile(chan, "voicemail/vm-youhave", chan->language);
 								res = tris_waitstream(chan, "");
 								if (minutes) {
 									res = tris_say_number(chan, minutes, TRIS_DIGIT_ANY, chan->language, (char *) NULL);
 									res = tris_streamfile(chan, "queue-minutes", chan->language);
 									res = tris_waitstream(chan, "");
 								}
 								if (seconds) {
 									res = tris_say_number(chan, seconds, TRIS_DIGIT_ANY, chan->language, (char *) NULL);
 									res = tris_streamfile(chan, "queue-seconds", chan->language);
 									res = tris_waitstream(chan, "");
 								}
 							} else {
 								res = tris_streamfile(chan, user->warning_sound, chan->language);
 								res = tris_waitstream(chan, "");
 							}
 						}
 					}
 					if (user->warning_freq) {
 						nexteventts = tris_tvadd(nexteventts, tris_samp2tv(user->warning_freq, 1000));
 					} else {
 						nexteventts = tris_tvadd(user->start_time, tris_samp2tv(user->timelimit, 1000));
					}
 				}
 			}

			now = tris_tvnow();
			if (timeout && now.tv_sec >= timeout) {
				break;
			}

			/* if we have just exited from the menu, and the user had a channel-driver
			   volume adjustment, restore it
			*/
			if (!menu_active && menu_was_active && user->listen.desired && !user->listen.actual) {
				set_talk_volume(user, user->listen.desired);
			}

			menu_was_active = menu_active;

			currentmarked = conf->markedusers;
			if (!(confflags & CONFFLAG_QUIET) &&
			    (confflags & CONFFLAG_MARKEDUSER) &&
			    (confflags & CONFFLAG_WAITMARKED) &&
			    lastmarked == 0) {
				if (currentmarked == 1 && conf->users > 1) {
					tris_say_number(chan, conf->users - 1, TRIS_DIGIT_ANY, chan->language, (char *) NULL);
					if (conf->users - 1 == 1) {
						if (!tris_streamfile(chan, "conference/conf-userwilljoin", chan->language)) {
							tris_waitstream(chan, "");
						}
					} else {
						if (!tris_streamfile(chan, "conference/conf-userswilljoin", chan->language)) {
							tris_waitstream(chan, "");
						}
					}
				}
				if (conf->users == 1 && ! (confflags & CONFFLAG_MARKEDUSER)) {
					if (!tris_streamfile(chan, "conference/conf-onlyperson", chan->language)) {
						tris_waitstream(chan, "");
					}
				}
			}

			/* Update the struct with the actual confflags */
			user->userflags = confflags;

			if (confflags & CONFFLAG_WAITMARKED) {
				if (currentmarked == 0) {
					if (lastmarked != 0) {
						if (!(confflags & CONFFLAG_QUIET)) {
							if (!tris_streamfile(chan, "conference/conf-leaderhasleft", chan->language)) {
								tris_waitstream(chan, "");
							}
						}
						if (confflags & CONFFLAG_MARKEDEXIT) {
							if (confflags & CONFFLAG_KICK_CONTINUE) {
								ret = 0;
							}
							break;
						} else {
							dahdic.confmode = DAHDI_CONF_CONF;
							if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
								tris_log(LOG_WARNING, "Error setting conference\n");
								close(fd);
								goto outrun;
							}
						}
					}
					if (!musiconhold && (confflags & CONFFLAG_MOH)) {
						conf_start_moh(chan, optargs[OPT_ARG_MOH_CLASS]);
						musiconhold = 1;
					}
				} else if (currentmarked >= 1 && lastmarked == 0) {
					/* Marked user entered, so cancel timeout */
					timeout = 0;
					if (confflags & CONFFLAG_MONITOR) {
						dahdic.confmode = DAHDI_CONF_CONFMON | DAHDI_CONF_LISTENER;
					} else if (confflags & CONFFLAG_TALKER) {
						dahdic.confmode = DAHDI_CONF_CONF | DAHDI_CONF_TALKER;
					} else {
						dahdic.confmode = DAHDI_CONF_CONF | DAHDI_CONF_TALKER | DAHDI_CONF_LISTENER;
					}
					if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
						tris_log(LOG_WARNING, "Error setting conference\n");
						close(fd);
						goto outrun;
					}
					if (musiconhold && (confflags & CONFFLAG_MOH)) {
						tris_moh_stop(chan);
						musiconhold = 0;
					}
					if (!(confflags & CONFFLAG_QUIET) && !(confflags & CONFFLAG_MARKEDUSER)) {
						if (!tris_streamfile(chan, "conference/conf-placeintoconf", chan->language)) {
							tris_waitstream(chan, "");
						}
						conf_play(chan, conf, ENTER);
					}
				}
			}

			/* trying to add moh for single person conf */
			if ((confflags & CONFFLAG_MOH) && !(confflags & CONFFLAG_WAITMARKED)) {
				if (conf->users == 1) {
					if (!musiconhold) {
						conf_start_moh(chan, optargs[OPT_ARG_MOH_CLASS]);
						musiconhold = 1;
					} 
				} else {
					if (musiconhold) {
						tris_moh_stop(chan);
						musiconhold = 0;
					}
				}
			}
			
			/* Leave if the last marked user left */
			if (currentmarked == 0 && lastmarked != 0 && (confflags & CONFFLAG_MARKEDEXIT)) {
				if (confflags & CONFFLAG_KICK_CONTINUE) {
					ret = 0;
				} else {
					ret = -1;
				}
				if (!(confflags & CONFFLAG_QUIET)) {
					if(!strncasecmp(conf->confno, "cmd", 3) &&
					  !tris_streamfile(chan, "conference/end_cmd", chan->language)) {
						tris_waitstream(chan, "");
					} else if(!strncasecmp(conf->confno, "urg", 3) &&
					  !tris_streamfile(chan, "conference/end_multi_talking", chan->language)) {
						tris_waitstream(chan, "");
					} else if(!tris_streamfile(chan, "conference/end_conf", chan->language)) {
						tris_waitstream(chan, "");
					} 
				}
				break;
			}
	
			/* Check if my modes have changed */
			if(user->adminflags & ADMINFLAG_RECORDCONF && !(confflags & CONFFLAG_RECORDCONF)) {
				
				if (!conf->recordingfilename) {
					//conf->recordingfilename = strdup(pbx_builtin_getvar_helper(chan, "MEETME_RECORDINGFILE"));
					//if (!conf->recordingfilename) {
						struct tris_tm tm;
						struct timeval t = tris_tvnow();
						
						tris_localtime(&t, &tm, NULL);
						snprintf(recordingtmp, sizeof(recordingtmp), "%s/satellite/conf-rec-%s-%s-%04d%02d%02d-%02d%02d%02d", 
							tris_config_TRIS_MONITOR_DIR, conf->confno, S_OR(chan->cid.cid_num, "<unknown>"),tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, 
							tm.tm_hour, tm.tm_min, tm.tm_sec);
						conf->recordingfilename = tris_strdup(recordingtmp);
					//}
					//conf->recordingformat = strdup(pbx_builtin_getvar_helper(chan, "MEETME_RECORDINGFORMAT"));
					//if (!conf->recordingformat) {
						tris_copy_string(recordingtmp, "wav", sizeof(recordingtmp));
						conf->recordingformat = tris_strdup(recordingtmp);
					//}
					tris_verb(4, "Starting recording of MeetMe Conference %s into file %s.%s.\n",
						    conf->confno, conf->recordingfilename, conf->recordingformat);
				}

				tris_mutex_lock(&conf->recordthreadlock);
				if ((conf->recordthread == TRIS_PTHREADT_NULL) && ((conf->lchan = tris_request("DAHDI", TRIS_FORMAT_SLINEAR, "pseudo", NULL, 0)))) {
					tris_set_read_format(conf->lchan, TRIS_FORMAT_SLINEAR);
					tris_set_write_format(conf->lchan, TRIS_FORMAT_SLINEAR);
					dahdic.chan = 0;
					dahdic.confno = conf->dahdiconf;
					dahdic.confmode = DAHDI_CONF_CONFANN | DAHDI_CONF_CONFANNMON;
					if (ioctl(conf->lchan->fds[0], DAHDI_SETCONF, &dahdic)) {
						tris_log(LOG_WARNING, "Error starting listen channel\n");
						tris_hangup(conf->lchan);
						conf->lchan = NULL;
					} else {
						tris_pthread_create_detached_background(&conf->recordthread, NULL, recordthread, conf);
					}
				}
				tris_mutex_unlock(&conf->recordthreadlock);
				user->adminflags &= ~(ADMINFLAG_RECORDCONF);
			}

			/* If I should be muted but am still talker, mute me */
			if ((user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED)) && (dahdic.confmode & DAHDI_CONF_TALKER)) {
				dahdic.confmode ^= DAHDI_CONF_TALKER;
				if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
					tris_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
					ret = -1;
					break;
				}

				/* Indicate user is not talking anymore - change him to unmonitored state */
				if ((confflags & (CONFFLAG_MONITORTALKER | CONFFLAG_OPTIMIZETALKER))) {
					set_user_talking(chan, conf, user, -1, confflags & CONFFLAG_MONITORTALKER);
				}

				manager_event(EVENT_FLAG_CALL, "MeetmeMute", 
						"Channel: %s\r\n"
						"Uniqueid: %s\r\n"
						"Meetme: %s\r\n"
						"Usernum: %i\r\n"
						"Status: on\r\n",
						chan->name, chan->uniqueid, conf->confno, user->user_no);
			}

			/* If I should be un-muted but am not talker, un-mute me */
			if (!(user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED)) && !(confflags & CONFFLAG_MONITOR) && !(dahdic.confmode & DAHDI_CONF_TALKER)) {
				dahdic.confmode |= DAHDI_CONF_TALKER;
				if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
					tris_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
					ret = -1;
					break;
				}

				manager_event(EVENT_FLAG_CALL, "MeetmeMute", 
						"Channel: %s\r\n"
						"Uniqueid: %s\r\n"
						"Meetme: %s\r\n"
						"Usernum: %i\r\n"
						"Status: off\r\n",
						chan->name, chan->uniqueid, conf->confno, user->user_no);
			}
			
			if ((user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED)) && 
				(user->adminflags & ADMINFLAG_T_REQUEST) && !(talkreq_manager)) {
				talkreq_manager = 1;

				manager_event(EVENT_FLAG_CALL, "MeetmeTalkRequest", 
					      "Channel: %s\r\n"
							      "Uniqueid: %s\r\n"
							      "Meetme: %s\r\n"
							      "Usernum: %i\r\n"
							      "Status: on\r\n",
							      chan->name, chan->uniqueid, conf->confno, user->user_no);
			}

			
			if (!(user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED)) && 
				!(user->adminflags & ADMINFLAG_T_REQUEST) && (talkreq_manager)) {
				talkreq_manager = 0;
				manager_event(EVENT_FLAG_CALL, "MeetmeTalkRequest", 
					      "Channel: %s\r\n"
							      "Uniqueid: %s\r\n"
							      "Meetme: %s\r\n"
							      "Usernum: %i\r\n"
							      "Status: off\r\n",
							     chan->name, chan->uniqueid, conf->confno, user->user_no);
			}
			
			/* If I have been kicked, exit the conference */
			if (user->adminflags & ADMINFLAG_KICKME) {
				/* You have been kicked. */
				if (!strncasecmp(conf->confno, "urg", 3) && !(confflags & CONFFLAG_QUIET) && 
					!tris_streamfile(chan, "conference/nway-kick", chan->language)) {
					tris_waitstream(chan, "");
				} else if (!(confflags & CONFFLAG_QUIET) && 
					!tris_streamfile(chan, "conference/you-are-kicked", chan->language)) {
					tris_waitstream(chan, "");
				}
				ret = 0;
				break;
			}

			/* the conference end */
			if (user->adminflags & ADMINFLAG_ENDCONF) {
				if(confflags & CONFFLAG_DIALOUT) {
					optargs[OPT_ARG_DIALOUT_MAINCONFID] = NULL;
				}
				else if (!(confflags & CONFFLAG_QUIET)){
					if(!strncasecmp(conf->confno, "cmd", 3) &&
					  !tris_streamfile(chan, "conference/end_cmd", chan->language)) {
						tris_waitstream(chan, "");
					} else if(!strncasecmp(conf->confno, "urg", 3) &&
					  !tris_streamfile(chan, "conference/end_multi_talking", chan->language)) {
						tris_waitstream(chan, "");
					} else if(!tris_streamfile(chan, "conference/end_conf", chan->language)) {
						tris_waitstream(chan, "");
					} 
				}
				ret = 0;
				break;
			}

			/* Perform an extra hangup check just in case */
			if (tris_check_hangup(chan)) {
				struct tris_conf_user *_u = NULL;
				if((confflags & CONFFLAG_DIALOUT) && !tris_strlen_zero(optargs[OPT_ARG_DIALOUT_MAINCONFID])) {
					optargs[OPT_ARG_DIALOUT_MAINCONFID] = NULL;
					conf->markedusers = 0;
				}
				if(!(confflags & CONFFLAG_DIALOUT) && (confflags& CONFFLAG_ADMIN)) {
					TRIS_LIST_TRAVERSE(&conf->userlist, _u, list)
						_u->adminflags |= ADMINFLAG_ENDCONF;
				}
				ret = 0;
				break;
			}

			c = tris_waitfor_nandfds(&chan, 1, &fd, nfds, NULL, &outfd, &ms);

			if (c) {
				char dtmfstr[2] = "";

				if (c->fds[0] != origfd || (user->dahdichannel && (c->audiohooks || c->monitor))) {
					if (using_pseudo) {
						/* Kill old pseudo */
						close(fd);
						using_pseudo = 0;
					}
					tris_debug(1, "Ooh, something swapped out under us, starting over\n");
					retrydahdi = (strcasecmp(c->tech->type, "DAHDI") || (c->audiohooks || c->monitor) ? 1 : 0);
					user->dahdichannel = !retrydahdi;
					goto dahdiretry;
				}
				if ((confflags & CONFFLAG_MONITOR) || (user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED))) {
					f = tris_read_noaudio(c);
				} else {
					f = tris_read(c);
				}
				if (!f) {
					if (confflags & CONFFLAG_ADMIN) {
						unset_admin_channel(chan, conf, confflags);
					}
					break;
				}
				if (f->frametype == TRIS_FRAME_DTMF) {
					dtmfstr[0] = f->subclass;
					dtmfstr[1] = '\0';
				}

				if ((f->frametype == TRIS_FRAME_VOICE) && (f->subclass == TRIS_FORMAT_SLINEAR)) {
					if (user->talk.actual) {
						tris_frame_adjust_volume(f, user->talk.actual);
					}

					if (confflags & (CONFFLAG_OPTIMIZETALKER | CONFFLAG_MONITORTALKER)) {
						if (user->talking == -1) {
							user->talking = 0;
						}

						res = tris_dsp_silence(dsp, f, &totalsilence);
						if (totalsilence < MEETME_DELAYDETECTTALK) {
							set_user_talking(chan, conf, user, 1, confflags & CONFFLAG_MONITORTALKER);
						}
						if (totalsilence > MEETME_DELAYDETECTENDTALK) {
							set_user_talking(chan, conf, user, 0, confflags & CONFFLAG_MONITORTALKER);
						}
					}
					if (using_pseudo) {
						/* Absolutely do _not_ use careful_write here...
						   it is important that we read data from the channel
						   as fast as it arrives, and feed it into the conference.
						   The buffering in the pseudo channel will take care of any
						   timing differences, unless they are so drastic as to lose
						   audio frames (in which case carefully writing would only
						   have delayed the audio even further).
						*/
						/* As it turns out, we do want to use careful write.  We just
						   don't want to block, but we do want to at least *try*
						   to write out all the samples.
						 */
						if (user->talking || !(confflags & CONFFLAG_OPTIMIZETALKER)) {
							careful_write(fd, f->data.ptr, f->datalen, 0);
						}
					}
				} else if ((!(confflags & CONFFLAG_DIALOUT)) && (((f->frametype == TRIS_FRAME_DTMF) && (f->subclass == '*') && (confflags & CONFFLAG_STARMENU)) || ((f->frametype == TRIS_FRAME_DTMF) && menu_active))) {
					if (confflags & CONFFLAG_PASS_DTMF) {
						conf_queue_dtmf(conf, user, f);
					}
					if (ioctl(fd, DAHDI_SETCONF, &dahdic_empty)) {
						tris_log(LOG_WARNING, "Error setting conference\n");
						close(fd);
						tris_frfree(f);
						goto outrun;
					}

					/* if we are entering the menu, and the user has a channel-driver
					   volume adjustment, clear it
					*/
					if (!menu_active && user->talk.desired && !user->talk.actual) {
						set_talk_volume(user, 0);
					}

					if (musiconhold) {
			   			tris_moh_stop(chan);
					}
					if ((confflags & CONFFLAG_ADMIN)) {
						/* Admin menu */
						if (!menu_active) {
							menu_active = 1;
							/* Record this sound! */
							if (!tris_streamfile(chan, "conference/conf-adminmenu-162", chan->language)) {
								dtmf = tris_waitstream(chan, TRIS_DIGIT_ANY);
								tris_stopstream(chan);
							} else {
								dtmf = 0;
							}
						} else {
							dtmf = f->subclass;
						}
						if (dtmf) {
							switch(dtmf) {
							case '1': /* Un/Mute */
								menu_active = 0;

								/* for admin, change both admin and use flags */
								if (user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED)) {
									user->adminflags &= ~(ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED);
								} else {
									user->adminflags |= (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED);
								}

								if ((confflags & CONFFLAG_MONITOR) || (user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED))) {
									if (!tris_streamfile(chan, "conference/conf-muted", chan->language)) {
										tris_waitstream(chan, "");
									}
								} else {
									if (!tris_streamfile(chan, "conference/conf-unmuted", chan->language)) {
										tris_waitstream(chan, "");
									}
								}
								break;
							case '2': /* Un/Lock the Conference */
								menu_active = 0;
								if (conf->locked) {
									conf->locked = 0;
									if (!tris_streamfile(chan, "conference/conf-unlockednow", chan->language)) {
										tris_waitstream(chan, "");
									}
								} else {
									conf->locked = 1;
									if (!tris_streamfile(chan, "conference/conf-lockednow", chan->language)) {
										tris_waitstream(chan, "");
									}
								}
								break;
							case '3': /* Eject last user */
								menu_active = 0;
								usr = TRIS_LIST_LAST(&conf->userlist);
								if ((usr->chan->name == chan->name) || (usr->userflags & CONFFLAG_ADMIN)) {
									if (!tris_streamfile(chan, "conference/conf-errormenu", chan->language)) {
										tris_waitstream(chan, "");
									}
								} else {
									usr->adminflags |= ADMINFLAG_KICKME;
								}
								tris_stopstream(chan);
								break;	
							case '4':
								tweak_listen_volume(user, VOL_DOWN);
								break;
							case '5':
								/* Extend RT conference */
								if (rt_schedule) {
									if (!rt_extend_conf(conf->confno)) {
										if (!tris_streamfile(chan, "conference/conf-extended", chan->language)) {
											tris_waitstream(chan, "");
										}
									} else {
										if (!tris_streamfile(chan, "conference/conf-nonextended", chan->language)) {
											tris_waitstream(chan, "");
										}
									}
									tris_stopstream(chan);
								}
								menu_active = 0;
								break;
							case '6':
								tweak_listen_volume(user, VOL_UP);
								break;
							case '7':
								tweak_talk_volume(user, VOL_DOWN);
								break;
							case '8':
								menu_active = 0;
								break;
							case '9':
								tweak_talk_volume(user, VOL_UP);
								break;
							default:
								menu_active = 0;
								/* Play an error message! */
								if (!tris_streamfile(chan, "conference/conf-errormenu", chan->language)) {
									tris_waitstream(chan, "");
								}
								break;
							}
						}
					} else {
						/* User menu */
						if (!menu_active) {
							menu_active = 1;
							if (!tris_streamfile(chan, "conference/conf-usermenu-162", chan->language)) {
								dtmf = tris_waitstream(chan, TRIS_DIGIT_ANY);
								tris_stopstream(chan);
							} else {
								dtmf = 0;
							}
						} else {
							dtmf = f->subclass;
						}
						if (dtmf) {
							switch (dtmf) {
							case '1': /* Un/Mute */
								menu_active = 0;

								/* user can only toggle the self-muted state */
								user->adminflags ^= ADMINFLAG_SELFMUTED;

								/* they can't override the admin mute state */
								if ((confflags & CONFFLAG_MONITOR) || (user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED))) {
									if (!tris_streamfile(chan, "conference/conf-muted", chan->language)) {
										tris_waitstream(chan, "");
									}
								} else {
									if (!tris_streamfile(chan, "conference/conf-unmuted", chan->language)) {
										tris_waitstream(chan, "");
									}
								}
								break;
							case '2':
								menu_active = 0;
								if (user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED)) {
									user->adminflags |= ADMINFLAG_T_REQUEST;
								}
									
								if (user->adminflags & ADMINFLAG_T_REQUEST) {
									if (!tris_streamfile(chan, "beep", chan->language)) {
										tris_waitstream(chan, "");
									}
								}
								break;
							case '4':
								tweak_listen_volume(user, VOL_DOWN);
								break;
							case '5':
								/* Extend RT conference */
								if (rt_schedule) {
									rt_extend_conf(conf->confno);
								}
								menu_active = 0;
								break;
							case '6':
								tweak_listen_volume(user, VOL_UP);
								break;
							case '7':
								tweak_talk_volume(user, VOL_DOWN);
								break;
							case '8':
								menu_active = 0;
								break;
							case '9':
								tweak_talk_volume(user, VOL_UP);
								break;
							default:
								menu_active = 0;
								if (!tris_streamfile(chan, "conference/conf-errormenu", chan->language)) {
									tris_waitstream(chan, "");
								}
								break;
							}
						}
					}
					if (musiconhold) {
						conf_start_moh(chan, optargs[OPT_ARG_MOH_CLASS]);
					}

					if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
						tris_log(LOG_WARNING, "Error setting conference\n");
						close(fd);
						tris_frfree(f);
						goto outrun;
					}

					conf_flush(fd, chan);
				/* Since this option could absorb DTMF meant for the previous (menu), we have to check this one last */
				} else if ((f->frametype == TRIS_FRAME_DTMF) && (confflags & CONFFLAG_EXIT_CONTEXT) && tris_exists_extension(chan, exitcontext, dtmfstr, 1, "")) {
					if (confflags & CONFFLAG_PASS_DTMF) {
						conf_queue_dtmf(conf, user, f);
					}

					if (!tris_goto_if_exists(chan, exitcontext, dtmfstr, 1)) {
						tris_debug(1, "Got DTMF %c, goto context %s\n", dtmfstr[0], exitcontext);
						ret = 0;
						tris_frfree(f);
						break;
					} else {
						tris_debug(2, "Exit by single digit did not work in meetme. Extension %s does not exist in context %s\n", dtmfstr, exitcontext);
					}
				} else if ((f->frametype == TRIS_FRAME_DTMF) && (confflags & CONFFLAG_KEYEXIT) && (strchr(exitkeys, f->subclass))) {
					pbx_builtin_setvar_helper(chan, "MEETME_EXIT_KEY", dtmfstr);
						
					if (confflags & CONFFLAG_PASS_DTMF) {
						conf_queue_dtmf(conf, user, f);
					}
					if(!(confflags & CONFFLAG_DIALOUT) && (confflags& CONFFLAG_ADMIN)) {
						struct tris_conf_user *_u = NULL;
						TRIS_LIST_TRAVERSE(&conf->userlist, _u, list)
							_u->adminflags |= ADMINFLAG_ENDCONF;
					}
					ret = 0;
					tris_frfree(f);
					break;
				} else if ((confflags & CONFFLAG_DIALOUT) &&(f->frametype == TRIS_FRAME_DTMF) //&& (confflags & CONFFLAG_ADMIN) 
					&& (f->subclass == '*')) {
					struct tris_conf_user *_u = NULL;
					TRIS_LIST_TRAVERSE(&conf->userlist, _u, list)
						_u->adminflags |= ADMINFLAG_ENDCONF;
					ret = 0;
					break;
					
				} else if (!(confflags & CONFFLAG_DIALOUT) &&(f->frametype == TRIS_FRAME_DTMF) && (confflags & CONFFLAG_ADMIN) 
					&& (f->subclass == '*')) {
					int tel_res = -1;
					char tel_num[80];
					tel_res = tris_meetme_dialout_getdata(chan, "conference/dial_extn_star", tel_num, sizeof(tel_num) - 1, 0, "*");
					if(kick_user(conf, tel_num))
						tris_play_and_wait(chan, "conference/not_found_user");
					
				}
				else if ((!(confflags & CONFFLAG_DIALOUT)) &&(f->frametype == TRIS_FRAME_DTMF) ) {
					if(f->subclass == '1') {
						manager_event(EVENT_FLAG_CALL, "MeetmeRequestRight", 
						"Channel: %s\r\n"
						"Uniqueid: %s\r\n"
						"Meetme: %s\r\n"
						"Usernum: %i\r\n",
						chan->name, chan->uniqueid, conf->confno, user->user_no);
					}
					else if (f->subclass == '#') {
						//else if ((f->frametype == TRIS_FRAME_DTMF_BEGIN || f->frametype == TRIS_FRAME_DTMF_END)
						//&& confflags & CONFFLAG_PASS_DTMF) {
						tris_verbose("BEGIN AND END HSH...\n"); //hsh start
						
						if(!strncasecmp(conf->confno, "spg", 3) || !strncasecmp(conf->confno, "cmd", 3)) {
							
							struct in_addr ourip;
							struct sockaddr_in bindaddr;
							int calling_tel_res = -1;
							char calling_telnum[100] ="" ;
							char calling_uri[100];
							
							memset(&bindaddr, 0, sizeof(struct sockaddr_in));
							tris_find_ourip(&ourip, bindaddr);
								
							calling_tel_res = tris_meetme_dialout_getdata(chan, "conference/dial_extn_pound", calling_telnum + strlen(calling_telnum), sizeof(calling_telnum) - 1 - strlen(calling_telnum), 0, "*#");
							
							if (calling_tel_res == '*' || tris_strlen_zero(calling_telnum)) {
								tris_verbose("cancelled calling phone! \n");
							} else if(!check_media_service(calling_telnum)) {
								if (!strcmp(calling_telnum, "0")) {
									invite_rest_to_meetme(conf, chan);
								} else {
									if(get_user(conf, calling_telnum)) {
										tris_play_and_wait(chan, "conference/already_existing");
									} else {
						
										tris_verb(1,"entered dtmf is %s\n", calling_telnum);
										snprintf(calling_uri, sizeof(calling_uri), "SIP/%s@%s:5060", calling_telnum, tris_inet_ntoa(ourip));
										printf("calling phone is %s\n",calling_uri );
										invite_to_meetme(chan, dials, &pos, calling_uri, conf->confno);
									}
								}
							}
					
					
						} else if(!strncasecmp(conf->confno, "urg", 3)) {
							if ((confflags & CONFFLAG_ADMIN) && (conf->users < 5)) {
							
								struct in_addr ourip;
								struct sockaddr_in bindaddr;
								int calling_tel_res = -1;
								char calling_telnum[100] ="" ;
								char calling_uri[100];
								
								memset(&bindaddr, 0, sizeof(struct sockaddr_in));
								tris_find_ourip(&ourip, bindaddr);
									
								//calling_tel_res = tris_app_getdata(chan, "conference/conf-getchannel", calling_telnum + strlen(calling_telnum), sizeof(calling_telnum) - 1 - strlen(calling_telnum), 0);
								calling_tel_res = tris_meetme_dialout_getdata(chan, "conference/dial_extn_pound", calling_telnum + strlen(calling_telnum), sizeof(calling_telnum) - 1 - strlen(calling_telnum), 0, "*#");
								
								if (calling_tel_res == '*' || tris_strlen_zero(calling_telnum)) {
									tris_verbose("cancelled calling phone! \n");
								} else if(!check_media_service(calling_telnum)) {
									if(get_user(conf, calling_telnum)) {
										tris_play_and_wait(chan, "conference/already_existing");
									} else {
					
										//printf("ORACLE....\n");
										tris_verb(1,"entered dtmf is %s\n", calling_telnum);
					
										//snprintf(meetmeopts, sizeof(meetmeopts), "MeetMe,%s,d", conf->confno);
										snprintf(calling_uri, sizeof(calling_uri), "SIP/%s@%s:5060", calling_telnum, tris_inet_ntoa(ourip));
										//snprintf(calling_uri, sizeof(calling_uri), "%s", calling_telnum);
										S_REPLACE(chan->cid.cid_name, tris_strdup("Conference") );
										printf("calling phone is %s\n",calling_uri );
										invite_to_meetme(chan, dials, &pos, calling_uri, conf->confno);
									/*
										struct tris_dial *calling_dial = NULL;
										if(!(calling_dial = tris_dial_create())) {
											tris_verbose("Failed to create new dialing structure!\n");
										}
					
										tris_dial_append(calling_dial, "SIP", calling_uri);
										tris_dial_option_global_enable(calling_dial, TRIS_DIAL_OPTION_ANSWER_EXEC,meetmeopts);
										//tris_dial_option_global_enable(calling_dial, TRIS_DIAL_OPTION_ANSWER_EXEC,"Dial");
					
										char *dup_confno ;
										dup_confno = conf->confno;
										S_REPLACE(chan->cid.cid_name, tris_strdup("Urgency Conference") ); 
										
										tris_dial_run(calling_dial,chan,1, 0);
										dials[pos++] = calling_dial;
									*/
									}
								}
							}
						}else {
							ret = -4;
							break;
						}
						//conf_queue_dtmf(conf, user, f);
					}
						
				} else if ((f->frametype == TRIS_FRAME_DTMF_BEGIN || f->frametype == TRIS_FRAME_DTMF_END)
					&& confflags & CONFFLAG_PASS_DTMF) {
					conf_queue_dtmf(conf, user, f);
				} else if ((confflags & CONFFLAG_SLA_STATION) && f->frametype == TRIS_FRAME_CONTROL) {
					switch (f->subclass) {
					case TRIS_CONTROL_HOLD:
						sla_queue_event_conf(SLA_EVENT_HOLD, chan, conf);
						break;
					default:
						break;
					}
				} else if (f->frametype == TRIS_FRAME_CONTROL) {
					switch (f->subclass) {
					case TRIS_CONTROL_REFER:
						handle_conf_refer(chan, conf, confflags);
						break;
					case TRIS_CONTROL_REFER_INFO:
						handle_conf_refer_info(chan, conf, confflags);
						break;
					default:
						break;
					}
				} else if (f->frametype == TRIS_FRAME_NULL) {
					/* Ignore NULL frames. It is perfectly normal to get these if the person is muted. */
				} else {
					tris_debug(1, 
						"Got unrecognized frame on channel %s, f->frametype=%d,f->subclass=%d\n",
						chan->name, f->frametype, f->subclass);
				}
				tris_frfree(f);
			} else if (outfd > -1) {
				res = read(outfd, buf, CONF_SIZE);
				if (res > 0) {
					memset(&fr, 0, sizeof(fr));
					fr.frametype = TRIS_FRAME_VOICE;
					fr.subclass = TRIS_FORMAT_SLINEAR;
					fr.datalen = res;
					fr.samples = res / 2;
					fr.data.ptr = buf;
					fr.offset = TRIS_FRIENDLY_OFFSET;
					if (!user->listen.actual &&
						((confflags & CONFFLAG_MONITOR) ||
						 (user->adminflags & (ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED)) ||
						 (!user->talking && (confflags & CONFFLAG_OPTIMIZETALKER))
						 )) {
						int idx;
						for (idx = 0; idx < TRIS_FRAME_BITS; idx++) {
							if (chan->rawwriteformat & (1 << idx)) {
								break;
							}
						}
						if (idx >= TRIS_FRAME_BITS) {
							goto bailoutandtrynormal;
						}
						tris_mutex_lock(&conf->listenlock);
						if (!conf->transframe[idx]) {
							if (conf->origframe) {
								if (!conf->transpath[idx]) {
									conf->transpath[idx] = tris_translator_build_path((1 << idx), TRIS_FORMAT_SLINEAR);
								}
								if (conf->transpath[idx]) {
									conf->transframe[idx] = tris_translate(conf->transpath[idx], conf->origframe, 0);
									if (!conf->transframe[idx]) {
										conf->transframe[idx] = &tris_null_frame;
									}
								}
							}
						}
						if (conf->transframe[idx]) {
 							if ((conf->transframe[idx]->frametype != TRIS_FRAME_NULL) &&
							    can_write(chan, confflags)) {
								struct tris_frame *cur;
								if (musiconhold && !tris_dsp_silence(dsp, conf->transframe[idx], &confsilence) && confsilence < MEETME_DELAYDETECTTALK) {
									tris_moh_stop(chan);
									mohtempstopped = 1;
								}

								/* the translator may have returned a list of frames, so
								   write each one onto the channel
								*/
								for (cur = conf->transframe[idx]; cur; cur = TRIS_LIST_NEXT(cur, frame_list)) {
									if (tris_write(chan, cur)) {
										tris_log(LOG_WARNING, "Unable to write frame to channel %s\n", chan->name);
										break;
									}
								}
								if (musiconhold && mohtempstopped && confsilence > MEETME_DELAYDETECTENDTALK) {
									mohtempstopped = 0;
									tris_moh_start(chan, NULL, NULL);
								}
							}
						} else {
							tris_mutex_unlock(&conf->listenlock);
							goto bailoutandtrynormal;
						}
						tris_mutex_unlock(&conf->listenlock);
					} else {
bailoutandtrynormal:
						if (musiconhold && !tris_dsp_silence(dsp, &fr, &confsilence) && confsilence < MEETME_DELAYDETECTTALK) {
							tris_moh_stop(chan);
							mohtempstopped = 1;
						}
						if (user->listen.actual) {
							tris_frame_adjust_volume(&fr, user->listen.actual);
						}
						if (can_write(chan, confflags) && tris_write(chan, &fr) < 0) {
							tris_log(LOG_WARNING, "Unable to write frame to channel %s\n", chan->name);
						}
						if (musiconhold && mohtempstopped && confsilence > MEETME_DELAYDETECTENDTALK) {
							mohtempstopped = 0;
							tris_moh_start(chan, NULL, NULL);
						}
					}
				} else {
					tris_log(LOG_WARNING, "Failed to read frame: %s\n", strerror(errno));
				}
			}
			lastmarked = currentmarked;
		}
	}

	if (musiconhold) {
		tris_moh_stop(chan);
	}
	
	if (using_pseudo) {
		close(fd);
	} else {
		/* Take out of conference */
		dahdic.chan = 0;	
		dahdic.confno = 0;
		dahdic.confmode = 0;
		if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
			tris_log(LOG_WARNING, "Error setting conference\n");
		}
	}

	reset_volumes(user);

	if (!(confflags & CONFFLAG_QUIET) && !(confflags & CONFFLAG_MONITOR) && !(confflags & CONFFLAG_ADMIN)) {
		conf_play(chan, conf, LEAVE);
	}

	if (!(user->adminflags & ADMINFLAG_ENDCONF) && ret != -4 && !(confflags & CONFFLAG_QUIET) && ((confflags & CONFFLAG_INTROUSER) || (confflags & CONFFLAG_INTROUSERNOREVIEW)) && conf->users > 1) {
		struct announce_listitem *item;
		if (!(item = ao2_alloc(sizeof(*item), NULL)))
			return -1;
		tris_copy_string(item->namerecloc, user->namerecloc, sizeof(item->namerecloc));
		tris_copy_string(item->language, chan->language, sizeof(item->language));
		item->confchan = conf->chan;
		tris_copy_string(item->exten, chan->cid.cid_num, sizeof(item->exten));
		item->confusers = conf->users;
		item->announcetype = CONF_HASLEFT;
		tris_mutex_lock(&conf->announcelistlock);
		TRIS_LIST_INSERT_TAIL(&conf->announcelist, item, entry);
		tris_cond_signal(&conf->announcelist_addition);
		tris_mutex_unlock(&conf->announcelistlock);
	} else if (ret != -4 && !(confflags & CONFFLAG_QUIET) && ((confflags & CONFFLAG_INTROUSER) || (confflags & CONFFLAG_INTROUSERNOREVIEW)) && conf->users == 1) {
		/* Last person is leaving, so no reason to try and announce, but should delete the name recording */
		tris_filedelete(user->namerecloc, NULL);
	}

 outrun:
	if (!(confflags & CONFFLAG_ADMIN)) {
		if (!bye_member_byuser(chan, conf, chan->referid)) {
			if (conf->admin_chan && conf->admin_chan->seqtype) {
				snprintf(conf->admin_chan->refer_phonenum, TRIS_MAX_EXTENSION, "%s", chan->cid.cid_num);
				send_control_notify(conf->admin_chan, TRIS_CONTROL_NOTIFY_BYE, chan->referid, 0);
			}
		}
	}
	if(pos)
		sleep(2);
	for (i = 0; i < pos; i++) {
		struct tris_dial *dial = dials[i];
	 
		/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
		tris_dial_join(dial);
	 
		/* Hangup all channels */
		tris_dial_hangup(dial);
	 
		/* Destroy dialing structure */
		tris_dial_destroy(dial);
		tris_verbose(" --------------- destroy dial (%d)\n", i);
	}

	TRIS_LIST_LOCK(&confs);

	if (dsp) {
		tris_dsp_free(dsp);
	}
	
	if (user->user_no) { /* Only cleanup users who really joined! */
		now = tris_tvnow();
		hr = (now.tv_sec - user->jointime) / 3600;
		min = ((now.tv_sec - user->jointime) % 3600) / 60;
		sec = (now.tv_sec - user->jointime) % 60;

		if (sent_event) {
			manager_event(EVENT_FLAG_CALL, "MeetmeLeave",
				      "Channel: %s\r\n"
				      "Uniqueid: %s\r\n"
				      "Meetme: %s\r\n"
				      "Usernum: %d\r\n"
				      "CallerIDNum: %s\r\n"
				      "CallerIDName: %s\r\n"
				      "Duration: %ld\r\n",
				      chan->name, chan->uniqueid, conf->confno, 
				      user->user_no,
				      S_OR(user->chan->cid.cid_num, "<unknown>"),
				      S_OR(user->chan->cid.cid_name, "<unknown>"),
				      (long)(now.tv_sec - user->jointime));
		}

		if (setusercount) {
			conf->users--;
			if (rt_log_members) {
				/* Update table */
				snprintf(members, sizeof(members), "%d", conf->users);
				tris_realtime_require_field("meetme",
					"confno", strlen(conf->confno) > 7 ? RQ_UINTEGER4 : strlen(conf->confno) > 4 ? RQ_UINTEGER3 : RQ_UINTEGER2, strlen(conf->confno),
					"members", RQ_UINTEGER1, strlen(members),
					NULL);
				tris_update_realtime("meetme", "roomno", conf->confno, "members", members, NULL);
			}
			if (confflags & CONFFLAG_MARKEDUSER) {
				conf->markedusers--;
			}
		}
		/* Remove ourselves from the list */
		TRIS_LIST_REMOVE(&conf->userlist, user, list);

		/* Change any states */
		if (!conf->users) {
			tris_devstate_changed(TRIS_DEVICE_NOT_INUSE, "meetme:%s", conf->confno);
		}

		/* Return the number of seconds the user was in the conf */
		snprintf(meetmesecs, sizeof(meetmesecs), "%d", (int) (time(NULL) - user->jointime));
		pbx_builtin_setvar_helper(chan, "MEETMESECS", meetmesecs);
	}
	tris_free(user);
	TRIS_LIST_UNLOCK(&confs);

	return ret;
}

/*! \brief The MeetmeCount application */
static int count_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_conference *conf;
	int count;
	char *localdata;
	char val[80] = "0"; 
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(confno);
		TRIS_APP_ARG(varname);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "MeetMeCount requires an argument (conference number)\n");
		return -1;
	}
	
	if (!(localdata = tris_strdupa(data)))
		return -1;

	TRIS_STANDARD_APP_ARGS(args, localdata);
	
	conf = find_conf(chan, args.confno, 0, 0, NULL, 0, 1, NULL);

	if (conf) {
		count = conf->users;
		dispose_conf(conf);
		conf = NULL;
	} else
		count = 0;

	if (!tris_strlen_zero(args.varname)) {
		/* have var so load it and exit */
		snprintf(val, sizeof(val), "%d", count);
		pbx_builtin_setvar_helper(chan, args.varname, val);
	} else {
		if (chan->_state != TRIS_STATE_UP) {
			tris_answer(chan);
		}
		res = tris_say_number(chan, count, "", chan->language, (char *) NULL); /* Needs gender */
	}

	return res;
}

static int play_conf_info(struct tris_channel *chan, struct tris_conference *conf)
{
	if(conf->admin_chan == NULL) 
		return 0;
	
	tris_stream_and_wait(chan, "conference/you-now", "");
	tris_say_digit_str(chan, conf->admin_chan->cid.cid_num, "", chan->language);
	if(!strncasecmp(conf->confno, "cmd", 3))
		tris_stream_and_wait(chan, "conference/entering-urg-cmd", "");
	else
		tris_stream_and_wait(chan, "conference/entering-conf", "");
	tris_stream_and_wait(chan, "conference/waiting", "");
	return 1;
}

/*! \brief The meetme() application */
static int conf_exec(struct tris_channel *chan, void *data)
{
	struct tris_dial *dials[MAX_DIALS];
	int i,pos = 0;
	int res = -1;
	char confno[MAX_CONFNUM] = "";
	int allowretry = 0;
	int retrycnt = 0;
	struct tris_conference *cnf = NULL;
	struct tris_flags confflags = {0}, config_flags = { 0 };
	int dynamic = 0;
	int empty = 0, empty_no_pin = 0;
	int always_prompt = 0;
	char *notdata, *info, the_pin[MAX_PIN] = "";
	char sql[256], exten[80];//, callinfo[80];
	int rooms = 0;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(confno);
		TRIS_APP_ARG(options);
		TRIS_APP_ARG(pin);
	);
	char *optargs[OPT_ARG_ARRAY_SIZE] = { NULL, };

	if (tris_strlen_zero(data)) {
		allowretry = 1;
		notdata = "";
	} else {
		notdata = data;
	}
	
	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	pbx_builtin_setvar_helper(chan, "is3broadcast", "meetme");

	snprintf(sql, sizeof(sql), "SELECT extension FROM uri WHERE username='%s'", chan->cid.cid_num);
	sql_select_query_execute(exten, sql);
	if(!tris_strlen_zero(exten) && strcmp(exten,chan->cid.cid_num)) {
		strcpy(chan->cid.cid_num, exten);
	}

	info = tris_strdupa(notdata);

	TRIS_STANDARD_APP_ARGS(args, info);	

	if (args.confno) {
		tris_copy_string(confno, args.confno, sizeof(confno));
		if (tris_strlen_zero(confno)) {
			allowretry = 1;
		}
	}
	
	if (!strncasecmp(confno, "urg", 3)) {
		//count urgency conf
		TRIS_LIST_LOCK(&confs);
		TRIS_LIST_TRAVERSE(&confs, cnf, list) {
			if (!strncasecmp(cnf->confno, "urg", 3)) {
				rooms++;
			}
		}
		TRIS_LIST_UNLOCK(&confs);
		if(rooms >= max_rooms) {
			tris_play_and_wait(chan, "conference/conf-roomfull");
			return res;
		}
	}

	if (args.pin)
		tris_copy_string(the_pin, args.pin, sizeof(the_pin));

	if (args.options) {
		tris_app_parse_options(meetme_opts, &confflags, optargs, args.options);
		dynamic = tris_test_flag(&confflags, CONFFLAG_DYNAMIC | CONFFLAG_DYNAMICPIN);
		if (tris_test_flag(&confflags, CONFFLAG_DYNAMICPIN) && tris_strlen_zero(args.pin))
			strcpy(the_pin, "q");

		empty = tris_test_flag(&confflags, CONFFLAG_EMPTY | CONFFLAG_EMPTYNOPIN);
		empty_no_pin = tris_test_flag(&confflags, CONFFLAG_EMPTYNOPIN);
		always_prompt = tris_test_flag(&confflags, CONFFLAG_ALWAYSPROMPT | CONFFLAG_DYNAMICPIN);
	}

	if(!chan->monitor)
		exec_monitor(chan);

	do {
		if (retrycnt > 3)
			allowretry = 0;
		if (empty) {
			int i;
			struct tris_config *cfg;
			struct tris_variable *var;
			int confno_int;

			/* We only need to load the config file for static and empty_no_pin (otherwise we don't care) */
			if ((empty_no_pin) || (!dynamic)) {
				cfg = tris_config_load(CONFIG_FILE_NAME, config_flags);
				if (cfg && cfg != CONFIG_STATUS_FILEINVALID) {
					var = tris_variable_browse(cfg, "rooms");
					while (var) {
						char parse[MAX_SETTINGS], *stringp = parse, *confno_tmp;
						if (!strcasecmp(var->name, "conf")) {
							int found = 0;
							tris_copy_string(parse, var->value, sizeof(parse));
							confno_tmp = strsep(&stringp, "|,");
							if (!dynamic) {
								/* For static:  run through the list and see if this conference is empty */
								TRIS_LIST_LOCK(&confs);
								TRIS_LIST_TRAVERSE(&confs, cnf, list) {
									if (!strcmp(confno_tmp, cnf->confno)) {
										/* The conference exists, therefore it's not empty */
										found = 1;
										break;
									}
								}
								TRIS_LIST_UNLOCK(&confs);
								if (!found) {
									/* At this point, we have a confno_tmp (static conference) that is empty */
									if ((empty_no_pin && tris_strlen_zero(stringp)) || (!empty_no_pin)) {
										/* Case 1:  empty_no_pin and pin is nonexistent (NULL)
										 * Case 2:  empty_no_pin and pin is blank (but not NULL)
										 * Case 3:  not empty_no_pin
										 */
										tris_copy_string(confno, confno_tmp, sizeof(confno));
										break;
										/* XXX the map is not complete (but we do have a confno) */
									}
								}
							}
						}
						var = var->next;
					}
					tris_config_destroy(cfg);
				}
			}

			/* Select first conference number not in use */
			if (tris_strlen_zero(confno) && dynamic) {
				TRIS_LIST_LOCK(&confs);
				for (i = 0; i < ARRAY_LEN(conf_map); i++) {
					if (!conf_map[i]) {
						snprintf(confno, sizeof(confno), "%d", i);
						conf_map[i] = 1;
						break;
					}
				}
				TRIS_LIST_UNLOCK(&confs);
			}

			/* Not found? */
			if (tris_strlen_zero(confno)) {
				res = tris_streamfile(chan, "conference/conf-noempty", chan->language);
				if (!res)
					tris_waitstream(chan, "");
			} else {
				if (sscanf(confno, "%30d", &confno_int) == 1) {
					if (!tris_test_flag(&confflags, CONFFLAG_QUIET)) {
						res = tris_streamfile(chan, "conference/conf-enteringno", chan->language);
						if (!res) {
							tris_waitstream(chan, "");
							res = tris_say_digits(chan, confno_int, "", chan->language);
						}
					}
				} else {
					tris_log(LOG_ERROR, "Could not scan confno '%s'\n", confno);
				}
			}
		}

		while (allowretry && (tris_strlen_zero(confno)) && (++retrycnt < 4)) {
			/* Prompt user for conference number */
			res = tris_app_getdata(chan, "conference/conf-getconfno", confno, sizeof(confno) - 1, 0);
			if (res < 0) {
				/* Don't try to validate when we catch an error */
				confno[0] = '\0';
				allowretry = 0;
				break;
			}
		}
		if (!tris_strlen_zero(confno)) {
			/* Check the validity of the conference */
			cnf = find_conf(chan, confno, 1, dynamic, the_pin, 
				sizeof(the_pin), 1, &confflags);
			if (!cnf) {
				int too_early = 0;

				cnf = find_conf_realtime(chan, confno, 1, dynamic, 
					the_pin, sizeof(the_pin), 1, &confflags, optargs, &too_early);
				if (rt_schedule && too_early)
					allowretry = 0;
			}

			if (!cnf) {
				if (allowretry) {
					confno[0] = '\0';
					res = tris_streamfile(chan, "conference/conf-invalid", chan->language);
					if (!res)
						tris_waitstream(chan, "");
					res = -1;
				}
			} else {
				if ((!tris_strlen_zero(cnf->pin) &&
				     !tris_test_flag(&confflags, CONFFLAG_ADMIN)) ||
				    (!tris_strlen_zero(cnf->pinadmin) &&
				     tris_test_flag(&confflags, CONFFLAG_ADMIN))) {
					char pin[MAX_PIN] = "";
					int j;

					/* Allow the pin to be retried up to 3 times */
					for (j = 0; j < 3; j++) {
						if (*the_pin && (always_prompt == 0)) {
							tris_copy_string(pin, the_pin, sizeof(pin));
							res = 0;
						} else {
							/* Prompt user for pin if pin is required */
							res = tris_app_getdata(chan, "conference/conf-getpin", pin + strlen(pin), sizeof(pin) - 1 - strlen(pin), 0);
						}
						if (res >= 0) {
							if (!strcasecmp(pin, cnf->pin) ||
							    (!tris_strlen_zero(cnf->pinadmin) &&
							     !strcasecmp(pin, cnf->pinadmin))) {
								/* Pin correct */
								allowretry = 0;
								if (!tris_strlen_zero(cnf->pinadmin) && !strcasecmp(pin, cnf->pinadmin)) {
									if (!tris_strlen_zero(cnf->adminopts)) {
										char *opts = tris_strdupa(cnf->adminopts);
										tris_app_parse_options(meetme_opts, &confflags, optargs, opts);
									}
								} else {
									if (!tris_strlen_zero(cnf->useropts)) {
										char *opts = tris_strdupa(cnf->useropts);
										tris_app_parse_options(meetme_opts, &confflags, optargs, opts);
									}
								}
								/* Run the conference */
								tris_verb(4, "Starting recording of MeetMe Conference %s into file %s.%s.\n", cnf->confno, cnf->recordingfilename, cnf->recordingformat);
								res = conf_run(chan, cnf, confflags.flags, optargs);
								break;
							} else {
								/* Pin invalid */
								if (!tris_streamfile(chan, "conference/conf-invalidpin", chan->language)) {
									res = tris_waitstream(chan, TRIS_DIGIT_ANY);
									tris_stopstream(chan);
								} else {
									tris_log(LOG_WARNING, "Couldn't play invalid pin msg!\n");
									break;
								}
								if (res < 0)
									break;
								pin[0] = res;
								pin[1] = '\0';
								res = -1;
								if (allowretry)
									confno[0] = '\0';
							}
						} else {
							/* failed when getting the pin */
							res = -1;
							allowretry = 0;
							/* see if we need to get rid of the conference */
							break;
						}

						/* Don't retry pin with a static pin */
						if (*the_pin && (always_prompt == 0)) {
							break;
						}
					}
				} else {
					/* No pin required */
					allowretry = 0;

					if(!strncasecmp(cnf->confno, "urg", 3) || !strncasecmp(cnf->confno, "spg", 3) ||
					   !strncasecmp(cnf->confno, "cmd", 3) )
							tris_clear_flag(&confflags, CONFFLAG_DIALOUT);
					
					if(tris_test_flag(&confflags,CONFFLAG_ADMIN) && !tris_test_flag(&confflags,CONFFLAG_DIALOUT)) { 
						//strcpy(cnf->admin, chan->cid.cid_num);
						cnf->admin_chan = chan;
					}/* else if(strncasecmp(cnf->confno, "urg", 3) && tris_strlen_zero(optargs[OPT_ARG_DIALOUT_MAINCONFID])){
						play_conf_info(chan, cnf);
					}*/

					/* Run the conference */
					res = conf_run(chan, cnf, confflags.flags, optargs);
				}
				if(!tris_test_flag(&confflags,CONFFLAG_DIALOUT)) { 
					//if(tris_test_flag(&confflags,CONFFLAG_ADMIN)) 
					char roomname[80];
					if(!strncasecmp(confno,"sch",3)) {
						snprintf(sql, sizeof(sql), "SELECT roomname FROM schedule_room WHERE roomno='%s'", confno);
						sql_select_query_execute(roomname, sql);
						//S_REPLACE(chan->cid.cid_name, tris_strdup("Scheduled Conference")); 
						S_REPLACE(chan->cid.cid_name, tris_strdup(roomname));
					}
					/*else if(!strncasecmp(confno,"spg",3)){
						snprintf(sql, sizeof(sql), "SELECT roomname FROM callconf_room WHERE roomno='%s'", confno);
						sql_select_query_execute(roonname, sql);
						//S_REPLACE(chan->cid.cid_name, tris_strdup("Call Conference"));
						S_REPLACE(chan->cid.cid_name, tris_strdup(roomname));
					}
					else if(!strncasecmp(confno,"cmd",3)) {
						snprintf(sql, sizeof(sql), "SELECT roomname FROM urgentcmd_room WHERE roomno='%s'", confno);
						sql_select_query_execute(roonname, sql);
						//S_REPLACE(chan->cid.cid_name, tris_strdup("Urgency Conference"));
						S_REPLACE(chan->cid.cid_name, tris_strdup(roomname));
					}*/

					while(res == -4) {
						struct in_addr ourip;
						struct sockaddr_in bindaddr;
						int calling_tel_res = -1;
						char calling_telnum[100] ;
						char calling_uri[100];

						memset(&bindaddr, 0, sizeof(struct sockaddr_in));
						tris_find_ourip(&ourip, bindaddr);
						//calling_tel_res = tris_app_getdata(chan, "conference/conf-getchannel", calling_telnum + strlen(calling_telnum), sizeof(calling_telnum) - 1 - strlen(calling_telnum), 0);
						calling_tel_res = tris_meetme_dialout_getdata(chan, "conference/dial_extn_pound", calling_telnum, sizeof(calling_telnum) - 1, 0, "*#");

						if (calling_tel_res == '*' || tris_strlen_zero(calling_telnum)) {
							tris_verbose("cancelled calling phone! %s\n", calling_telnum);
						} else if(!check_media_service(calling_telnum)){
							snprintf(calling_uri, sizeof(calling_uri), "SIP/%s@%s:5060", calling_telnum, tris_inet_ntoa(ourip));
							//snprintf(calling_uri, sizeof(calling_uri), "SIP/%s", calling_telnum);
							if(!get_user(cnf, calling_telnum)) {
								if(tris_test_flag(&confflags,CONFFLAG_ADMIN)) 
									dial_out(chan, dials, &pos, confno, calling_uri,CONFFLAG_KEYEXIT);
								else 
									dial_out(chan, dials, &pos, confno, calling_uri,0);
							} else {
								tris_play_and_wait(chan, "conference/already_existing");
							}
						} /* else {
							tris_play_and_wait(chan, "conference/pbx-not-found");
						} */
						
						dispose_conf(cnf);
						cnf = NULL;

						cnf = find_conf(chan, confno, 1, dynamic, the_pin, 
							sizeof(the_pin), 1, &confflags);
						if (!cnf) {
							int too_early = 0;
							cnf = find_conf_realtime(chan, confno, 1, dynamic, 
								the_pin, sizeof(the_pin), 1, &confflags, optargs, &too_early);
						}

						if(tris_test_flag(&confflags,CONFFLAG_ADMIN) && !tris_test_flag(&confflags,CONFFLAG_DIALOUT)) { 
							//strcpy(cnf->admin, chan->cid.cid_num);
							cnf->admin_chan = chan;
						}
						optargs[OPT_ARG_DIALOUT_MAINCONFID] = confno;
						if(cnf) 
							res = conf_run(chan, cnf, confflags.flags, optargs);
					}
					if(pos)
						sleep(2);
					for (i = 0; i < pos; i++) {
						struct tris_dial *dial = dials[i];

						/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
						tris_dial_join(dial);

						/* Hangup all channels */
						tris_dial_hangup(dial);

						/* Destroy dialing structure */
						tris_dial_destroy(dial);
						tris_verbose(" --------------- destroy dial (%d)\n", i);
					}
				} else {//if (tris_test_flag(&confflags,CONFFLAG_DIALOUT)) {

					if(!tris_strlen_zero(optargs[OPT_ARG_DIALOUT_MAINCONFID])) {
						dispose_conf(cnf);
						cnf = NULL;
						
						tris_copy_string(confno, optargs[OPT_ARG_DIALOUT_MAINCONFID], sizeof(confno));
						tris_verbose(" ---- main confno = %s\n",confno);
						tris_app_parse_options(meetme_opts, &confflags, optargs, "di");
						cnf = find_conf(chan, confno, 1, 1, "", 
							0, 1, &confflags);
						
						//if(strncasecmp(cnf->confno, "urg", 3)){
						//	play_conf_info(chan, cnf);
						//}
						res = conf_run(chan, cnf, confflags.flags, optargs);
					}
					
				}

				dispose_conf(cnf);
				cnf = NULL;
			}
		}
	} while (allowretry);

	if (cnf)
		dispose_conf(cnf);
	
	return res;
}

static int check_schedule_sponser(char *roomno, char *ext, int *intro) 
{
	char sql[256];
	char exx[256];
	char result[32];
	int ret = 0;

	snprintf(sql, sizeof(sql), "SELECT extension FROM uri WHERE uid='%s' or extension='%s'", ext, ext);
	sql_select_query_execute(exx, sql);
	if (tris_strlen_zero(exx))
		ret = 0;
	else
		ret = 1;
	if (!ret)
		return 0;

	snprintf(sql, sizeof(sql), "SELECT sponseruid FROM schedule_room WHERE roomno='%s' and sponseruid='%s'",
			roomno, exx);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		ret = 0;
	else
		ret = 1;
	if (!ret)
		return 0;

	snprintf(sql, sizeof(sql), "SELECT notify_status FROM schedule_room WHERE roomno='%s'", roomno);
	sql_select_query_execute(result, sql);
	*intro = atoi(result);
	return ret;
}

static int check_schedule_member(char *roomno, char *ext) 
{
	char sql[256];
	char result[32];
	char exx[256];

	snprintf(sql, sizeof(sql), "SELECT extension FROM uri WHERE uid='%s' or extension='%s'", ext, ext);
	sql_select_query_execute(exx, sql);
	if (tris_strlen_zero(exx))
		return 0;
	snprintf(sql, sizeof(sql), "SELECT memberuid FROM schedule_member WHERE roomno='%s' and memberuid='%s'",
			roomno, exx);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;
	return 1;
}

static int check_schedule_room(char *roomno) 
{
	char sql[256];
	char result[32];

	snprintf(sql, sizeof(sql), "SELECT sponseruid FROM schedule_room WHERE roomno='%s'", roomno);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;
	return 1;
}

static int scheduleconf_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	int tries = 3;
	char roomno[256] = "";
	char options[256] = "";
	char header[256] = "";
	char dtmfs[256] = "";
	struct tris_app* app;
	int intro = 1;
	
	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}
	
	if (tris_strlen_zero(chan->cid.cid_num)) {
		return -1;
	}

	while (tries && !res) {
		if (tris_strlen_zero(dtmfs))
			res = tris_app_getdata(chan, "conference/select_room_num", dtmfs, sizeof(dtmfs)-1, 5000);
		if (!tris_strlen_zero(dtmfs))
			snprintf(roomno, sizeof(roomno), "sch%s", dtmfs);
		if (check_schedule_sponser(roomno, chan->cid.cid_num, &intro)) {
			tris_play_and_wait(chan, "conference/first_participant");
			if (intro)
				snprintf(options, sizeof(options), "%s,adi", roomno);
			else
				snprintf(options, sizeof(options), "%s,adq", roomno);
			res = 2;
			break;
		}
		if (check_schedule_member(roomno, chan->cid.cid_num)) {
			res = 1;
			if (intro)
				snprintf(options, sizeof(options), "%s,di", roomno);
			else
				snprintf(options, sizeof(options), "%s,dq", roomno);
			break;
		}
		if (!check_schedule_room(roomno)) {
			tris_verbose("There is no report room\n");
			if(!tris_strlen_zero(roomno)) {
				tris_app_getdata(chan, "conference/retry_room_num", dtmfs, sizeof(dtmfs)-1, 5000);
			}
		} else {
			tris_play_and_wait(chan, "conference/is_not_participant");
			dtmfs[0] = '\0';
		}
		res = 0;
		tries--;
		continue;
	}

	if (!res) {
		tris_play_and_wait(chan, "goodbye");
		return 0;
	}

	snprintf(header, sizeof(header), "Call-Info: MS,Scheduleconf,%s", roomno);
	app = pbx_findapp("SIPAddHeader");
	if (app) {
		pbx_exec(chan, app, header);
	}

	conf_exec(chan, options);
	return 0;
}

static int check_urgencyconf_permission(char *ext) 
{
	char sql[256];
	char result[32];
	
	snprintf(sql, sizeof(sql), "SELECT spermit FROM user_info where uid = '%s' or extension = '%s'", ext, ext);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;
	if (strlen(result) < 14)
		return 0;
	if (result[14] == '0')
		return 0;
	return 1;
}

static int urgencyconf_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char roomno[256] = "";
	char options[256] = "";
	char header[256] = "";
	struct tris_app* app;
	
	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}
	
	if (tris_strlen_zero(chan->cid.cid_num)) {
		return -1;
	}

	if (!check_urgencyconf_permission(chan->cid.cid_num)) {
		tris_play_and_wait(chan, "conference/not-nway");
		return 0;
	}
	tris_play_and_wait(chan, "conference/nway");
	snprintf(roomno, sizeof(roomno), "urg%s", chan->uniqueid);
	snprintf(options, sizeof(options), "%s,ad", roomno);
	
	snprintf(header, sizeof(header), "Call-Info: MS,Urgencyconf,%s", roomno);
	app = pbx_findapp("SIPAddHeader");
	if (app) {
		pbx_exec(chan, app, header);
	}

	conf_exec(chan, options);
	return 0;
}

static struct tris_conf_user *find_user(struct tris_conference *conf, char *callerident) 
{
	struct tris_conf_user *user = NULL;
	int cid;
	
	sscanf(callerident, "%30i", &cid);
	if (conf && callerident) {
		TRIS_LIST_TRAVERSE(&conf->userlist, user, list) {
			if (cid == user->user_no)
				return user;
		}
	}
	return NULL;
}

/*! \brief The MeetMeadmin application */
/* MeetMeAdmin(confno, command, caller) */
static int admin_exec(struct tris_channel *chan, void *data) {
	char *params;
	struct tris_conference *cnf;
	struct tris_conf_user *user = NULL;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(confno);
		TRIS_APP_ARG(command);
		TRIS_APP_ARG(user);
	);
	int res = 0;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "MeetMeAdmin requires an argument!\n");
		pbx_builtin_setvar_helper(chan, "MEETMEADMINSTATUS", "NOPARSE");
		return -1;
	}

	params = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, params);

	if (!args.command) {
		tris_log(LOG_WARNING, "MeetmeAdmin requires a command!\n");
		pbx_builtin_setvar_helper(chan, "MEETMEADMINSTATUS", "NOPARSE");
		return -1;
	}

	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, cnf, list) {
		if (!strcmp(cnf->confno, args.confno))
			break;
	}

	if (!cnf) {
		tris_log(LOG_WARNING, "Conference number '%s' not found!\n", args.confno);
		TRIS_LIST_UNLOCK(&confs);
		pbx_builtin_setvar_helper(chan, "MEETMEADMINSTATUS", "NOTFOUND");
		return 0;
	}

	tris_atomic_fetchadd_int(&cnf->refcount, 1);

	if (args.user)
		user = find_user(cnf, args.user);

	switch (*args.command) {
	case 97: /* a: record*/
		user->adminflags |= ADMINFLAG_RECORDCONF;
		break;
	case 76: /* L: Lock */ 
		cnf->locked = 1;
		break;
	case 108: /* l: Unlock */ 
		cnf->locked = 0;
		break;
	case 75: /* K: kick all users */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list)
			user->adminflags |= ADMINFLAG_ENDCONF;
		break;
	case 101: /* e: Eject last user*/
		user = TRIS_LIST_LAST(&cnf->userlist);
		if (!(user->userflags & CONFFLAG_ADMIN))
			user->adminflags |= ADMINFLAG_KICKME;
		else {
			res = -1;
			tris_log(LOG_NOTICE, "Not kicking last user, is an Admin!\n");
		}
		break;
	case 77: /* M: Mute */ 
		if (user) {
			user->adminflags |= ADMINFLAG_MUTED;
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 78: /* N: Mute all (non-admin) users */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			if (!(user->userflags & CONFFLAG_ADMIN)) {
				user->adminflags |= ADMINFLAG_MUTED;
			}
		}
		break;					
	case 109: /* m: Unmute */ 
		if (user) {
			user->adminflags &= ~(ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED | ADMINFLAG_T_REQUEST);
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 110: /* n: Unmute all users */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			user->adminflags &= ~(ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED | ADMINFLAG_T_REQUEST);
		}
		break;
	case 107: /* k: Kick user */ 
		if (user) {
			user->adminflags |= ADMINFLAG_KICKME;
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 118: /* v: Lower all users listen volume */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			tweak_listen_volume(user, VOL_DOWN);
		}
		break;
	case 86: /* V: Raise all users listen volume */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			tweak_listen_volume(user, VOL_UP);
		}
		break;
	case 115: /* s: Lower all users speaking volume */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			tweak_talk_volume(user, VOL_DOWN);
		}
		break;
	case 83: /* S: Raise all users speaking volume */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			tweak_talk_volume(user, VOL_UP);
		}
		break;
	case 82: /* R: Reset all volume levels */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			reset_volumes(user);
		}
		break;
	case 114: /* r: Reset user's volume level */
		if (user) {
			reset_volumes(user);
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 85: /* U: Raise user's listen volume */
		if (user) {
			tweak_listen_volume(user, VOL_UP);
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 117: /* u: Lower user's listen volume */
		if (user) {
			tweak_listen_volume(user, VOL_DOWN);
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 84: /* T: Raise user's talk volume */
		if (user) {
			tweak_talk_volume(user, VOL_UP);
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 116: /* t: Lower user's talk volume */
		if (user) {
			tweak_talk_volume(user, VOL_DOWN);
		} else {
			res = -2;
			tris_log(LOG_NOTICE, "Specified User not found!\n");
		}
		break;
	case 'E': /* E: Extend conference */
		if (rt_extend_conf(args.confno)) {
			res = -1;
		}
		break;
	}

	TRIS_LIST_UNLOCK(&confs);

	dispose_conf(cnf);
	pbx_builtin_setvar_helper(chan, "MEETMEADMINSTATUS", res == -2 ? "NOTFOUND" : res ? "FAILED" : "OK");

	return 0;
}

/*--- channel_admin_exec: The MeetMeChannelAdmin application */
/* MeetMeChannelAdmin(channel, command) */
static int channel_admin_exec(struct tris_channel *chan, void *data) {
	char *params;
	struct tris_conference *conf = NULL;
	struct tris_conf_user *user = NULL;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(channel);
		TRIS_APP_ARG(command);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "MeetMeChannelAdmin requires two arguments!\n");
		return -1;
	}
	
	params = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, params);

	if (!args.channel) {
		tris_log(LOG_WARNING, "MeetMeChannelAdmin requires a channel name!\n");
		return -1;
	}

	if (!args.command) {
		tris_log(LOG_WARNING, "MeetMeChannelAdmin requires a command!\n");
		return -1;
	}

	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, conf, list) {
		TRIS_LIST_TRAVERSE(&conf->userlist, user, list) {
			if (!strcmp(user->chan->name, args.channel))
				break;
		}
	}
	
	if (!user) {
		tris_log(LOG_NOTICE, "Specified user (%s) not found\n", args.channel);
		TRIS_LIST_UNLOCK(&confs);
		return 0;
	}
	
	/* perform the specified action */
	switch (*args.command) {
		case 77: /* M: Mute */ 
			user->adminflags |= ADMINFLAG_MUTED;
			break;
		case 109: /* m: Unmute */ 
			user->adminflags &= ~ADMINFLAG_MUTED;
			break;
		case 107: /* k: Kick user */ 
			user->adminflags |= ADMINFLAG_KICKME;
			break;
		default: /* unknown command */
			tris_log(LOG_WARNING, "Unknown MeetMeChannelAdmin command '%s'\n", args.command);
			break;
	}

	TRIS_LIST_UNLOCK(&confs);
	
	return 0;
}

static int action_meetmerecord(struct mansession *s, const struct message *m)
{
	
	struct tris_conference *conf;
	struct tris_conf_user *user;
	const char *confid = astman_get_header(m, "Confno");
	char *userid = tris_strdupa(astman_get_header(m, "Usernum"));
	int userno;

	if (tris_strlen_zero(confid)) {
		astman_send_error(s, m, "Meetme conference not specified");
		return 0;
	}

	if (tris_strlen_zero(userid)) {
		astman_send_error(s, m, "Meetme user number not specified");
		return 0;
	}

	userno = strtoul(userid, &userid, 10);

	if (*userid) {
		astman_send_error(s, m, "Invalid user number");
		return 0;
	}

	/* Look in the conference list */
	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, conf, list) {
		if (!strcmp(confid, conf->confno))
			break;
	}

	if (!conf) {
		TRIS_LIST_UNLOCK(&confs);
		astman_send_error(s, m, "Meetme conference does not exist");
		return 0;
	}

	TRIS_LIST_TRAVERSE(&conf->userlist, user, list)
		if (user->user_no == userno)
			break;

	if (!user) {
		TRIS_LIST_UNLOCK(&confs);
		astman_send_error(s, m, "User number not found");
		return 0;
	}

	//user->adminflags |= ADMINFLAG_RECORDCONF;	/* request user muting */
	// 2012-07-26 begin
	char mfn[256], args[256];
	struct tris_tm tm;
	struct timeval t = tris_tvnow();
	struct tris_channel *chan = user->chan;

	struct tris_app *the_app = pbx_findapp("Monitor");

	/* If the application was not found, return immediately */
	if (!the_app)
		return -1;

	tris_localtime(&t, &tm, NULL);
	snprintf(mfn, sizeof(mfn), "satellite/conf-rec-%s-%s-%04d%02d%02d-%02d%02d%02d", 
		conf->confno, S_OR(chan->cid.cid_num, "<unknown>"), tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, 
		tm.tm_hour, tm.tm_min, tm.tm_sec);
	
	snprintf(args, sizeof(args), ",%s,m", mfn);
	/* All is well... execute the application */
	pbx_exec(chan, the_app, args);
	// 2012-07-26 end
	
	TRIS_LIST_UNLOCK(&confs);

	astman_send_ack(s, m,  "Success");
	return 0;
}

static int meetmemute(struct mansession *s, const struct message *m, int mute)
{
	struct tris_conference *conf;
	struct tris_conf_user *user;
	const char *confid = astman_get_header(m, "Meetme");
	char *userid = tris_strdupa(astman_get_header(m, "Usernum"));
	int userno;

	if (tris_strlen_zero(confid)) {
		astman_send_error(s, m, "Meetme conference not specified");
		return 0;
	}

	if (tris_strlen_zero(userid)) {
		astman_send_error(s, m, "Meetme user number not specified");
		return 0;
	}

	userno = strtoul(userid, &userid, 10);

	if (*userid) {
		astman_send_error(s, m, "Invalid user number");
		return 0;
	}

	/* Look in the conference list */
	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, conf, list) {
		if (!strcmp(confid, conf->confno))
			break;
	}

	if (!conf) {
		TRIS_LIST_UNLOCK(&confs);
		astman_send_error(s, m, "Meetme conference does not exist");
		return 0;
	}

	TRIS_LIST_TRAVERSE(&conf->userlist, user, list)
		if (user->user_no == userno)
			break;

	if (!user) {
		TRIS_LIST_UNLOCK(&confs);
		astman_send_error(s, m, "User number not found");
		return 0;
	}

	if (mute)
		user->adminflags |= ADMINFLAG_MUTED;	/* request user muting */
	else
		user->adminflags &= ~(ADMINFLAG_MUTED | ADMINFLAG_SELFMUTED | ADMINFLAG_T_REQUEST);	/* request user unmuting */

	TRIS_LIST_UNLOCK(&confs);

	tris_log(LOG_NOTICE, "Requested to %smute conf %s user %d userchan %s uniqueid %s\n", mute ? "" : "un", conf->confno, user->user_no, user->chan->name, user->chan->uniqueid);

	//astman_send_ack(s, m, mute ? "User muted\r\n!!!END!!!" : "User unmuted\r\n!!!END!!!");
	astman_send_ack(s, m, mute ? "User muted" : "User unmuted");
	return 0;
}

static int action_meetmemute(struct mansession *s, const struct message *m)
{
	return meetmemute(s, m, 1);
}

static int action_meetmeunmute(struct mansession *s, const struct message *m)
{
	return meetmemute(s, m, 0);
}

/* 2012-07-16 */
struct user_obj {
	char *sql;
	char name[64];
	char job[256];
	char groupname[256];
	SQLLEN err;
};

static SQLHSTMT user_prepare(struct odbc_obj *obj, void *data)
{
	struct user_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_CHAR, q->name, sizeof(q->name), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->job, sizeof(q->job), &q->err);
	SQLBindCol(sth, 3, SQL_C_CHAR, q->groupname, sizeof(q->groupname), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

/* 2012-07-16 */

static int user_info(char *result, const char *extension, struct odbc_obj *obj)
{
	char sqlbuf[1024];
	SQLHSTMT stmt;
	struct user_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;

	result[0] = '\0';

	if (tris_strlen_zero(extension)) {
		return 0;
	}

	memset(&q, 0, sizeof(q));

	if (!obj)
		return 0;
	
	//snprintf(sqlbuf, sizeof(sqlbuf), 
	//	"SELECT name, job FROM user_info WHERE extension = '%s' ", extension);
	snprintf(sqlbuf, sizeof(sqlbuf), 
		"SELECT u.name, u.job, c.grp_name FROM user_info AS u LEFT JOIN groups AS c ON u.gid = c.gid WHERE u.extension = '%s' ", extension);

	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, user_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		return -1;
	}

	char tmp[1024];
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		if(tris_strlen_zero(result))
			snprintf(tmp, sizeof(tmp), "%s,%s %s", tris_strlen_zero(q.name)?"<unknown>":q.name, tris_strlen_zero(q.groupname)?"<unknown>":q.groupname, tris_strlen_zero(q.job)?"<unknown>":q.job);
		else
			snprintf(tmp, sizeof(tmp), ",%s,%s %s", tris_strlen_zero(q.name)?"<unknown>":q.name, tris_strlen_zero(q.groupname)?"<unknown>":q.groupname, tris_strlen_zero(q.job)?"<unknown>":q.job);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	
	if(tris_strlen_zero(result))
		strcpy(result, "<unknown>,<unknown> <unknown>");
	
	return 0;
}

static int action_satelliteuserdetail(struct mansession *s, const struct message *m)
{
	const char *userid = astman_get_header(m, "UserID");
	char result[5120]="";
	struct odbc_obj *obj;
	int res = 0;

	if (tris_strlen_zero(userid)) {
		astman_send_error(s, m, "UserID not specified");
		return 0;
	}
	
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;

	res = user_info(result, userid, obj);
	
	astman_send_ack(s, m, "User info will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

struct room_obj {
	char *sql;
	char roomno[16];
	char roomname[40];
	char sponsoruid[64];
	SQLLEN err;
};

static SQLHSTMT room_prepare(struct odbc_obj *obj, void *data)
{
	struct room_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_CHAR, q->roomno, sizeof(q->roomno), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->roomname, sizeof(q->roomname), &q->err);
	SQLBindCol(sth, 3, SQL_C_CHAR, q->sponsoruid, sizeof(q->sponsoruid), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

/* 2012-07-16 */


static char mandescr_satellitelist[] =
"Description: Satellite List.\n"
"Variables: (Names marked with * are required)\n"
"	*Sponosr: Sponsor ID\n"
"Returns satellite list that <Sponsor ID> could open.\n"
"\n";

static int action_satellitelist(struct mansession *s, const struct message *m)
{
	const char *sponsor = astman_get_header(m, "Sponsor");
	char result[5120]="", sqlbuf[1024];
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	struct room_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;

	if (tris_strlen_zero(sponsor)) {
		astman_send_error(s, m, "Sponosr not specified");
		return 0;
	}
	
	memset(&q, 0, sizeof(q));

	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;
	
	snprintf(sqlbuf, sizeof(sqlbuf), "SELECT roomno, roomname, sponseruid FROM callconf_room WHERE sponseruid REGEXP '.*%s.*'", sponsor);
	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, room_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	char tmp[1024];
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		if(tris_strlen_zero(result)) 
			snprintf(tmp, sizeof(tmp), "%s,%s", q.roomno, q.roomname);
		else
			snprintf(tmp, sizeof(tmp), ",%s,%s", q.roomno, q.roomname);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	astman_send_ack(s, m, "Satellite list will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

static int action_satellitecanparticipate(struct mansession *s, const struct message *m)
{
	const char *participant = astman_get_header(m, "Participant");
	char result[5120]="", sqlbuf[1024], totalcount[30];
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	struct room_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;
	struct tris_conference *cnf;

	if (tris_strlen_zero(participant)) {
		astman_send_error(s, m, "Participant not specified");
		return 0;
	}
	
	memset(&q, 0, sizeof(q));

	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;
	
	snprintf(sqlbuf, sizeof(sqlbuf), " SELECT c.roomno, c.roomname, c.sponseruid FROM callconf_member AS u LEFT JOIN callconf_room AS c ON u.roomno = c.roomno WHERE memberuid='%s'", participant);
	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, room_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	char tmp[1024];
	int usercount;
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		usercount = 0;
		snprintf(sqlbuf, sizeof(sqlbuf), "SELECT COUNT(*) FROM callconf_member WHERE roomno='%s'", q.roomno);
		sql_select_query_execute(totalcount, sqlbuf);
		
		/* Find the right conference */
		TRIS_LIST_LOCK(&confs);
		TRIS_LIST_TRAVERSE(&confs, cnf, list) {
			/* If we ask for one particular, and this isn't it, skip it */
			if (!tris_strlen_zero(q.roomno) && !strcmp(cnf->confno, q.roomno)) {
				//user_info(userinfo, S_OR(user->chan->cid.cid_num, "<unknown>"), obj);
				usercount = cnf->users;
				break;
			}
		}
		TRIS_LIST_UNLOCK(&confs);
		
		char all_info[2048] = "", u_info[1024], *exten, *tmp2 = q.sponsoruid;
		while ((exten = strsep(&tmp2, ","))) {
			user_info(u_info, exten, obj);
			snprintf(tmp, sizeof(tmp), ",%s,%s", exten, u_info);
			strcat(all_info, tmp);
		}
		
		if(tris_strlen_zero(result)) 
			snprintf(tmp, sizeof(tmp), "%s,%s%s,%d/%s", q.roomno, q.roomname, all_info, usercount,totalcount);
		else
			snprintf(tmp, sizeof(tmp), "!%s,%s%s,%d/%s", q.roomno, q.roomname, all_info, usercount,totalcount);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	astman_send_ack(s, m, "List will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

static int action_satelliteaddmember(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "Roomno");
	const char *memberid = astman_get_header(m, "MemberID");
	char sqlbuf[1024], result[256];

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}
	if (tris_strlen_zero(memberid)) {
		astman_send_error(s, m, "MemberID not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), "INSERT INTO callconf_member(roomno, memberuid, mempermit) VALUES('%s', '%s', '1')", 
		roomno, memberid);
	sql_select_query_execute(result, sqlbuf);
	astman_send_listack(s, m, "Successfully completed", result);
	return 0;

}

static int action_satelliteremovemember(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "Roomno");
	const char *memberid = astman_get_header(m, "MemberID");
	char sqlbuf[1024], result[256];

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}
	if (tris_strlen_zero(memberid)) {
		astman_send_error(s, m, "MemberID not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), "DELETE FROM callconf_member WHERE roomno='%s' AND memberuid='%s'", 
		roomno, memberid);
	sql_select_query_execute(result, sqlbuf);
	astman_send_listack(s, m, "Successfully completed", result);
	return 0;

}

static int action_satellitesettalking(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "Roomno");
	const char *memberid = astman_get_header(m, "MemberID");
	const char *talking = astman_get_header(m, "Talking");
	char sqlbuf[1024], result[256];

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}
	if (tris_strlen_zero(memberid)) {
		astman_send_error(s, m, "MemberID not specified");
		return 0;
	}
	if (tris_strlen_zero(talking)) {
		astman_send_error(s, m, "Talking not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), 
		"UPDATE callconf_member SET mempermit='%s' WHERE roomno='%s' AND memberuid='%s'", 
		strcasecmp(talking,"true")?"0":"1", roomno, memberid);
	sql_select_query_execute(result, sqlbuf);
	astman_send_listack(s, m, "Successfully completed", result);
	return 0;

}


struct member_obj {
	char *sql;
	char memberuid[64];
	char mempermit[10];
	SQLLEN err;
};

static SQLHSTMT member_prepare(struct odbc_obj *obj, void *data)
{
	struct member_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_CHAR, q->memberuid, sizeof(q->memberuid), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->mempermit, sizeof(q->mempermit), &q->err);
//	SQLBindCol(sth, 3, SQL_C_CHAR, q->sponsoruid, sizeof(q->sponsoruid), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}
static char mandescr_satelliteroomdetail[] =
"Description: Satellite Room Detail.\n"
"Variables: (Names marked with * are required)\n"
"	*Roomno: Room number\n"
"	Sponosr: Sponsor ID\n"
"Returns participant list for Roomno.\n"
"\n";

static int action_satelliteroomdetail(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "roomno");
	char result[5120]="", sqlbuf[1024], roomname[40];
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	struct member_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), "SELECT roomname FROM callconf_room where roomno='%s' ", roomno);
	sql_select_query_execute(roomname, sqlbuf);
	
	memset(&q, 0, sizeof(q));

	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;
	
	snprintf(sqlbuf, sizeof(sqlbuf), "SELECT memberuid,mempermit FROM callconf_member WHERE roomno='%s' ", roomno);
	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, member_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	snprintf(result, sizeof(result), "%s,%s", roomno, roomname);
	
	char tmp[1024], u_info[1024];
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		user_info(u_info, q.memberuid, obj);
		snprintf(tmp, sizeof(tmp), ",%s,%s,%s", q.memberuid, u_info, q.mempermit);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	astman_send_ack(s, m, "Satellite list will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

static char mandescr_meetmelist[] =
"Description: Lists all users in a particular MeetMe conference.\n"
"MeetmeList will follow as separate events, followed by a final event called\n"
"MeetmeListComplete.\n"
"Variables:\n"
"    *ActionId: <id>\n"
"    *Conference: <confno>\n";

static int action_meetmelist(struct mansession *s, const struct message *m)
{
	const char *actionid = astman_get_header(m, "ActionID");
	const char *conference = astman_get_header(m, "Conference");
	char idText[80] = "";
	struct tris_conference *cnf;
	struct tris_conf_user *user;
	int total = 0;

	if (!tris_strlen_zero(actionid))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", actionid);

	if (TRIS_LIST_EMPTY(&confs)) {
		astman_send_error(s, m, "No active conferences.");
		return 0;
	}

	astman_send_listack(s, m, "Meetme user list will follow", "start");

	/* Find the right conference */
	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, cnf, list) {
		/* If we ask for one particular, and this isn't it, skip it */
		if (!tris_strlen_zero(conference) && strcmp(cnf->confno, conference))
			continue;

		/* Show all the users */
		TRIS_LIST_TRAVERSE(&cnf->userlist, user, list) {
			total++;
			astman_append(s,
			"Event: MeetmeList\r\n"
			"%s"
			"Conference: %s\r\n"
			"UserNumber: %d\r\n"
			"CallerIDNum: %s\r\n"
			"CallerIDName: %s\r\n"
			"Channel: %s\r\n"
			"Admin: %s\r\n"
			"Role: %s\r\n"
			"MarkedUser: %s\r\n"
			"Muted: %s\r\n"
			"Talking: %s\r\n"
			"\r\n",
			idText,
			cnf->confno,
			user->user_no,
			S_OR(user->chan->cid.cid_num, "<unknown>"),
			S_OR(user->chan->cid.cid_name, "<no name>"),
			user->chan->name,
			user->userflags & CONFFLAG_ADMIN ? "Yes" : "No",
			user->userflags & CONFFLAG_MONITOR ? "Listen only" : user->userflags & CONFFLAG_TALKER ? "Talk only" : "Talk and listen",
			user->userflags & CONFFLAG_MARKEDUSER ? "Yes" : "No",
			user->adminflags & ADMINFLAG_MUTED ? "By admin" : user->adminflags & ADMINFLAG_SELFMUTED ? "By self" : "No",
			user->talking > 0 ? "Yes" : user->talking == 0 ? "No" : "Not monitored"); 
		}
	}
	TRIS_LIST_UNLOCK(&confs);
	/* Send final confirmation */
	astman_append(s,
	"Event: MeetmeListComplete\r\n"
	"EventList: Complete\r\n"
	"ListItems: %d\r\n"
	"%s"
	"\r\n", total, idText);
	return 0;
}

static void *recordthread(void *args)
{
	struct tris_conference *cnf = args;
	struct tris_frame *f = NULL;
	int flags;
	struct tris_filestream *s = NULL;
	int res = 0;
	int x;
	const char *oldrecordingfilename = NULL;

	if (!cnf || !cnf->lchan) {
		pthread_exit(0);
	}

	tris_stopstream(cnf->lchan);
	flags = O_CREAT | O_TRUNC | O_WRONLY;


	cnf->recording = MEETME_RECORD_ACTIVE;
	while (tris_waitfor(cnf->lchan, -1) > -1) {
		if (cnf->recording == MEETME_RECORD_TERMINATE) {
			TRIS_LIST_LOCK(&confs);
			TRIS_LIST_UNLOCK(&confs);
			break;
		}
		if (!s && cnf->recordingfilename && (cnf->recordingfilename != oldrecordingfilename)) {
			s = tris_writefile(cnf->recordingfilename, cnf->recordingformat, NULL, flags, 0, TRIS_FILE_MODE);
			oldrecordingfilename = cnf->recordingfilename;
		}
		
		f = tris_read(cnf->lchan);
		if (!f) {
			res = -1;
			break;
		}
		if (f->frametype == TRIS_FRAME_VOICE) {
			tris_mutex_lock(&cnf->listenlock);
			for (x = 0; x < TRIS_FRAME_BITS; x++) {
				/* Free any translations that have occured */
				if (cnf->transframe[x]) {
					tris_frfree(cnf->transframe[x]);
					cnf->transframe[x] = NULL;
				}
			}
			if (cnf->origframe)
				tris_frfree(cnf->origframe);
			cnf->origframe = tris_frdup(f);
			tris_mutex_unlock(&cnf->listenlock);
			if (s)
				res = tris_writestream(s, f);
			if (res) {
				tris_frfree(f);
				break;
			}
		}
		tris_frfree(f);
	}
	cnf->recording = MEETME_RECORD_OFF;
	if (s)
		tris_closestream(s);
	
	pthread_exit(0);
}

/*! \brief Callback for devicestate providers */
static enum tris_device_state meetmestate(const char *data)
{
	struct tris_conference *conf;

	/* Find conference */
	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, conf, list) {
		if (!strcmp(data, conf->confno))
			break;
	}
	TRIS_LIST_UNLOCK(&confs);
	if (!conf)
		return TRIS_DEVICE_INVALID;


	/* SKREP to fill */
	if (!conf->users)
		return TRIS_DEVICE_NOT_INUSE;

	return TRIS_DEVICE_INUSE;
}

static void load_config_meetme(void)
{
	struct tris_config *cfg;
	struct tris_flags config_flags = { 0 };
	const char *val;

	if (!(cfg = tris_config_load(CONFIG_FILE_NAME, config_flags))) {
		return;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file " CONFIG_FILE_NAME " is in an invalid format.  Aborting.\n");
		return;
	}

	audio_buffers = DEFAULT_AUDIO_BUFFERS;
	max_rooms = DEFAULT_MAX_ROOMS;

	/*  Scheduling support is off by default */
	rt_schedule = 0;
	fuzzystart = 0;
	earlyalert = 0;
	endalert = 0;
	extendby = 0;

	/*  Logging of participants defaults to ON for compatibility reasons */
	rt_log_members = 0;  

	if ((val = tris_variable_retrieve(cfg, "general", "audiobuffers"))) {
		if ((sscanf(val, "%30d", &audio_buffers) != 1)) {
			tris_log(LOG_WARNING, "audiobuffers setting must be a number, not '%s'\n", val);
			audio_buffers = DEFAULT_AUDIO_BUFFERS;
		} else if ((audio_buffers < DAHDI_DEFAULT_NUM_BUFS) || (audio_buffers > DAHDI_MAX_NUM_BUFS)) {
			tris_log(LOG_WARNING, "audiobuffers setting must be between %d and %d\n",
				DAHDI_DEFAULT_NUM_BUFS, DAHDI_MAX_NUM_BUFS);
			audio_buffers = DEFAULT_AUDIO_BUFFERS;
		}
		if (audio_buffers != DEFAULT_AUDIO_BUFFERS)
			tris_log(LOG_NOTICE, "Audio buffers per channel set to %d\n", audio_buffers);
	}

	if ((val = tris_variable_retrieve(cfg, "general", "maxrooms"))) {
		if ((sscanf(val, "%d", &max_rooms) != 1)) {
			tris_log(LOG_WARNING, "maxrooms setting must be a number, not '%s'\n", val);
			max_rooms = DEFAULT_MAX_ROOMS;
		}
		tris_verbose("Meetme rooms set to %d\n", max_rooms);
	}

	if ((val = tris_variable_retrieve(cfg, "general", "schedule")))
		rt_schedule = tris_true(val);
	if ((val = tris_variable_retrieve(cfg, "general", "logmembercount")))
		rt_log_members = tris_true(val);
	if ((val = tris_variable_retrieve(cfg, "general", "fuzzystart"))) {
		if ((sscanf(val, "%30d", &fuzzystart) != 1)) {
			tris_log(LOG_WARNING, "fuzzystart must be a number, not '%s'\n", val);
			fuzzystart = 0;
		} 
	}
	if ((val = tris_variable_retrieve(cfg, "general", "earlyalert"))) {
		if ((sscanf(val, "%30d", &earlyalert) != 1)) {
			tris_log(LOG_WARNING, "earlyalert must be a number, not '%s'\n", val);
			earlyalert = 0;
		} 
	}
	if ((val = tris_variable_retrieve(cfg, "general", "endalert"))) {
		if ((sscanf(val, "%30d", &endalert) != 1)) {
			tris_log(LOG_WARNING, "endalert must be a number, not '%s'\n", val);
			endalert = 0;
		} 
	}
	if ((val = tris_variable_retrieve(cfg, "general", "extendby"))) {
		if ((sscanf(val, "%30d", &extendby) != 1)) {
			tris_log(LOG_WARNING, "extendby must be a number, not '%s'\n", val);
			extendby = 0;
		} 
	}

	tris_config_destroy(cfg);
}

/*! \brief Find an SLA trunk by name
 * \note This must be called with the sla_trunks container locked
 */
static struct sla_trunk *sla_find_trunk(const char *name)
{
	struct sla_trunk *trunk = NULL;

	TRIS_RWLIST_TRAVERSE(&sla_trunks, trunk, entry) {
		if (!strcasecmp(trunk->name, name))
			break;
	}

	return trunk;
}

/*! \brief Find an SLA station by name
 * \note This must be called with the sla_stations container locked
 */
static struct sla_station *sla_find_station(const char *name)
{
	struct sla_station *station = NULL;

	TRIS_RWLIST_TRAVERSE(&sla_stations, station, entry) {
		if (!strcasecmp(station->name, name))
			break;
	}

	return station;
}

static int sla_check_station_hold_access(const struct sla_trunk *trunk,
	const struct sla_station *station)
{
	struct sla_station_ref *station_ref;
	struct sla_trunk_ref *trunk_ref;

	/* For each station that has this call on hold, check for private hold. */
	TRIS_LIST_TRAVERSE(&trunk->stations, station_ref, entry) {
		TRIS_LIST_TRAVERSE(&station_ref->station->trunks, trunk_ref, entry) {
			if (trunk_ref->trunk != trunk || station_ref->station == station)
				continue;
			if (trunk_ref->state == SLA_TRUNK_STATE_ONHOLD_BYME &&
				station_ref->station->hold_access == SLA_HOLD_PRIVATE)
				return 1;
			return 0;
		}
	}

	return 0;
}

/*! \brief Find a trunk reference on a station by name
 * \param station the station
 * \param name the trunk's name
 * \return a pointer to the station's trunk reference.  If the trunk
 *         is not found, it is not idle and barge is disabled, or if
 *         it is on hold and private hold is set, then NULL will be returned.
 */
static struct sla_trunk_ref *sla_find_trunk_ref_byname(const struct sla_station *station,
	const char *name)
{
	struct sla_trunk_ref *trunk_ref = NULL;

	TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
		if (strcasecmp(trunk_ref->trunk->name, name))
			continue;

		if ( (trunk_ref->trunk->barge_disabled 
			&& trunk_ref->state == SLA_TRUNK_STATE_UP) ||
			(trunk_ref->trunk->hold_stations 
			&& trunk_ref->trunk->hold_access == SLA_HOLD_PRIVATE
			&& trunk_ref->state != SLA_TRUNK_STATE_ONHOLD_BYME) ||
			sla_check_station_hold_access(trunk_ref->trunk, station) ) 
		{
			trunk_ref = NULL;
		}

		break;
	}

	return trunk_ref;
}

static struct sla_station_ref *sla_create_station_ref(struct sla_station *station)
{
	struct sla_station_ref *station_ref;

	if (!(station_ref = tris_calloc(1, sizeof(*station_ref))))
		return NULL;

	station_ref->station = station;

	return station_ref;
}

static struct sla_ringing_station *sla_create_ringing_station(struct sla_station *station)
{
	struct sla_ringing_station *ringing_station;

	if (!(ringing_station = tris_calloc(1, sizeof(*ringing_station))))
		return NULL;

	ringing_station->station = station;
	ringing_station->ring_begin = tris_tvnow();

	return ringing_station;
}

static enum tris_device_state sla_state_to_devstate(enum sla_trunk_state state)
{
	switch (state) {
	case SLA_TRUNK_STATE_IDLE:
		return TRIS_DEVICE_NOT_INUSE;
	case SLA_TRUNK_STATE_RINGING:
		return TRIS_DEVICE_RINGING;
	case SLA_TRUNK_STATE_UP:
		return TRIS_DEVICE_INUSE;
	case SLA_TRUNK_STATE_ONHOLD:
	case SLA_TRUNK_STATE_ONHOLD_BYME:
		return TRIS_DEVICE_ONHOLD;
	}

	return TRIS_DEVICE_UNKNOWN;
}

static void sla_change_trunk_state(const struct sla_trunk *trunk, enum sla_trunk_state state, 
	enum sla_which_trunk_refs inactive_only, const struct sla_trunk_ref *exclude)
{
	struct sla_station *station;
	struct sla_trunk_ref *trunk_ref;

	TRIS_LIST_TRAVERSE(&sla_stations, station, entry) {
		TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
			if (trunk_ref->trunk != trunk || (inactive_only ? trunk_ref->chan : 0)
				|| trunk_ref == exclude)
				continue;
			trunk_ref->state = state;
			tris_devstate_changed(sla_state_to_devstate(state), 
				"SLA:%s_%s", station->name, trunk->name);
			break;
		}
	}
}

struct run_station_args {
	struct sla_station *station;
	struct sla_trunk_ref *trunk_ref;
	tris_mutex_t *cond_lock;
	tris_cond_t *cond;
};

static void answer_trunk_chan(struct tris_channel *chan)
{
	tris_answer(chan);
	tris_indicate(chan, -1);
}

static void *run_station(void *data)
{
	struct sla_station *station;
	struct sla_trunk_ref *trunk_ref;
	struct tris_str *conf_name = tris_str_create(16);
	struct tris_flags conf_flags = { 0 };
	struct tris_conference *conf;

	{
		struct run_station_args *args = data;
		station = args->station;
		trunk_ref = args->trunk_ref;
		tris_mutex_lock(args->cond_lock);
		tris_cond_signal(args->cond);
		tris_mutex_unlock(args->cond_lock);
		/* args is no longer valid here. */
	}

	tris_atomic_fetchadd_int((int *) &trunk_ref->trunk->active_stations, 1);
	tris_str_set(&conf_name, 0, "SLA_%s", trunk_ref->trunk->name);
	tris_set_flag(&conf_flags, 
		CONFFLAG_QUIET | CONFFLAG_MARKEDEXIT | CONFFLAG_PASS_DTMF | CONFFLAG_SLA_STATION);
	answer_trunk_chan(trunk_ref->chan);
	conf = build_conf(tris_str_buffer(conf_name), "", "", 0, 0, 1, trunk_ref->chan);
	if (conf) {
		conf_run(trunk_ref->chan, conf, conf_flags.flags, NULL);
		dispose_conf(conf);
		conf = NULL;
	}
	trunk_ref->chan = NULL;
	if (tris_atomic_dec_and_test((int *) &trunk_ref->trunk->active_stations) &&
		trunk_ref->state != SLA_TRUNK_STATE_ONHOLD_BYME) {
		tris_str_append(&conf_name, 0, ",K");
		admin_exec(NULL, tris_str_buffer(conf_name));
		trunk_ref->trunk->hold_stations = 0;
		sla_change_trunk_state(trunk_ref->trunk, SLA_TRUNK_STATE_IDLE, ALL_TRUNK_REFS, NULL);
	}

	tris_dial_join(station->dial);
	tris_dial_destroy(station->dial);
	station->dial = NULL;
	tris_free(conf_name);

	return NULL;
}

static void sla_stop_ringing_trunk(struct sla_ringing_trunk *ringing_trunk)
{
	char buf[80];
	struct sla_station_ref *station_ref;

	snprintf(buf, sizeof(buf), "SLA_%s,K", ringing_trunk->trunk->name);
	admin_exec(NULL, buf);
	sla_change_trunk_state(ringing_trunk->trunk, SLA_TRUNK_STATE_IDLE, ALL_TRUNK_REFS, NULL);

	while ((station_ref = TRIS_LIST_REMOVE_HEAD(&ringing_trunk->timed_out_stations, entry)))
		tris_free(station_ref);

	tris_free(ringing_trunk);
}

static void sla_stop_ringing_station(struct sla_ringing_station *ringing_station,
	enum sla_station_hangup hangup)
{
	struct sla_ringing_trunk *ringing_trunk;
	struct sla_trunk_ref *trunk_ref;
	struct sla_station_ref *station_ref;

	tris_dial_join(ringing_station->station->dial);
	tris_dial_destroy(ringing_station->station->dial);
	ringing_station->station->dial = NULL;

	if (hangup == SLA_STATION_HANGUP_NORMAL)
		goto done;

	/* If the station is being hung up because of a timeout, then add it to the
	 * list of timed out stations on each of the ringing trunks.  This is so
	 * that when doing further processing to figure out which stations should be
	 * ringing, which trunk to answer, determining timeouts, etc., we know which
	 * ringing trunks we should ignore. */
	TRIS_LIST_TRAVERSE(&sla.ringing_trunks, ringing_trunk, entry) {
		TRIS_LIST_TRAVERSE(&ringing_station->station->trunks, trunk_ref, entry) {
			if (ringing_trunk->trunk == trunk_ref->trunk)
				break;
		}
		if (!trunk_ref)
			continue;
		if (!(station_ref = sla_create_station_ref(ringing_station->station)))
			continue;
		TRIS_LIST_INSERT_TAIL(&ringing_trunk->timed_out_stations, station_ref, entry);
	}

done:
	tris_free(ringing_station);
}

static void sla_dial_state_callback(struct tris_dial *dial)
{
	sla_queue_event(SLA_EVENT_DIAL_STATE);
}

/*! \brief Check to see if dialing this station already timed out for this ringing trunk
 * \note Assumes sla.lock is locked
 */
static int sla_check_timed_out_station(const struct sla_ringing_trunk *ringing_trunk,
	const struct sla_station *station)
{
	struct sla_station_ref *timed_out_station;

	TRIS_LIST_TRAVERSE(&ringing_trunk->timed_out_stations, timed_out_station, entry) {
		if (station == timed_out_station->station)
			return 1;
	}

	return 0;
}

/*! \brief Choose the highest priority ringing trunk for a station
 * \param station the station
 * \param remove remove the ringing trunk once selected
 * \param trunk_ref a place to store the pointer to this stations reference to
 *        the selected trunk
 * \return a pointer to the selected ringing trunk, or NULL if none found
 * \note Assumes that sla.lock is locked
 */
static struct sla_ringing_trunk *sla_choose_ringing_trunk(struct sla_station *station, 
	struct sla_trunk_ref **trunk_ref, int rm)
{
	struct sla_trunk_ref *s_trunk_ref;
	struct sla_ringing_trunk *ringing_trunk = NULL;

	TRIS_LIST_TRAVERSE(&station->trunks, s_trunk_ref, entry) {
		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.ringing_trunks, ringing_trunk, entry) {
			/* Make sure this is the trunk we're looking for */
			if (s_trunk_ref->trunk != ringing_trunk->trunk)
				continue;

			/* This trunk on the station is ringing.  But, make sure this station
			 * didn't already time out while this trunk was ringing. */
			if (sla_check_timed_out_station(ringing_trunk, station))
				continue;

			if (rm)
				TRIS_LIST_REMOVE_CURRENT(entry);

			if (trunk_ref)
				*trunk_ref = s_trunk_ref;

			break;
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
	
		if (ringing_trunk)
			break;
	}

	return ringing_trunk;
}

static void sla_handle_dial_state_event(void)
{
	struct sla_ringing_station *ringing_station;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.ringing_stations, ringing_station, entry) {
		struct sla_trunk_ref *s_trunk_ref = NULL;
		struct sla_ringing_trunk *ringing_trunk = NULL;
		struct run_station_args args;
		enum tris_dial_result dial_res;
		pthread_t dont_care;
		tris_mutex_t cond_lock;
		tris_cond_t cond;

		switch ((dial_res = tris_dial_state(ringing_station->station->dial))) {
		case TRIS_DIAL_RESULT_HANGUP:
		case TRIS_DIAL_RESULT_INVALID:
		case TRIS_DIAL_RESULT_FAILED:
		case TRIS_DIAL_RESULT_TIMEOUT:
		case TRIS_DIAL_RESULT_UNANSWERED:
			TRIS_LIST_REMOVE_CURRENT(entry);
			sla_stop_ringing_station(ringing_station, SLA_STATION_HANGUP_NORMAL);
			break;
		case TRIS_DIAL_RESULT_ANSWERED:
			TRIS_LIST_REMOVE_CURRENT(entry);
			/* Find the appropriate trunk to answer. */
			tris_mutex_lock(&sla.lock);
			ringing_trunk = sla_choose_ringing_trunk(ringing_station->station, &s_trunk_ref, 1);
			tris_mutex_unlock(&sla.lock);
			if (!ringing_trunk) {
				tris_debug(1, "Found no ringing trunk for station '%s' to answer!\n", ringing_station->station->name);
				break;
			}
			/* Track the channel that answered this trunk */
			s_trunk_ref->chan = tris_dial_answered(ringing_station->station->dial);
			/* Actually answer the trunk */
			answer_trunk_chan(ringing_trunk->trunk->chan);
			sla_change_trunk_state(ringing_trunk->trunk, SLA_TRUNK_STATE_UP, ALL_TRUNK_REFS, NULL);
			/* Now, start a thread that will connect this station to the trunk.  The rest of
			 * the code here sets up the thread and ensures that it is able to save the arguments
			 * before they are no longer valid since they are allocated on the stack. */
			args.trunk_ref = s_trunk_ref;
			args.station = ringing_station->station;
			args.cond = &cond;
			args.cond_lock = &cond_lock;
			tris_free(ringing_trunk);
			tris_free(ringing_station);
			tris_mutex_init(&cond_lock);
			tris_cond_init(&cond, NULL);
			tris_mutex_lock(&cond_lock);
			tris_pthread_create_detached_background(&dont_care, NULL, run_station, &args);
			tris_cond_wait(&cond, &cond_lock);
			tris_mutex_unlock(&cond_lock);
			tris_mutex_destroy(&cond_lock);
			tris_cond_destroy(&cond);
			break;
		case TRIS_DIAL_RESULT_TRYING:
		case TRIS_DIAL_RESULT_RINGING:
		case TRIS_DIAL_RESULT_PROGRESS:
		case TRIS_DIAL_RESULT_PROCEEDING:
			break;
		default:
			break;
		}
		if (dial_res == TRIS_DIAL_RESULT_ANSWERED) {
			/* Queue up reprocessing ringing trunks, and then ringing stations again */
			sla_queue_event(SLA_EVENT_RINGING_TRUNK);
			sla_queue_event(SLA_EVENT_DIAL_STATE);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
}

/*! \brief Check to see if this station is already ringing 
 * \note Assumes sla.lock is locked 
 */
static int sla_check_ringing_station(const struct sla_station *station)
{
	struct sla_ringing_station *ringing_station;

	TRIS_LIST_TRAVERSE(&sla.ringing_stations, ringing_station, entry) {
		if (station == ringing_station->station)
			return 1;
	}

	return 0;
}

/*! \brief Check to see if this station has failed to be dialed in the past minute
 * \note assumes sla.lock is locked
 */
static int sla_check_failed_station(const struct sla_station *station)
{
	struct sla_failed_station *failed_station;
	int res = 0;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.failed_stations, failed_station, entry) {
		if (station != failed_station->station)
			continue;
		if (tris_tvdiff_ms(tris_tvnow(), failed_station->last_try) > 1000) {
			TRIS_LIST_REMOVE_CURRENT(entry);
			tris_free(failed_station);
			break;
		}
		res = 1;
	}
	TRIS_LIST_TRAVERSE_SAFE_END

	return res;
}

/*! \brief Ring a station
 * \note Assumes sla.lock is locked
 */
static int sla_ring_station(struct sla_ringing_trunk *ringing_trunk, struct sla_station *station)
{
	char *tech, *tech_data;
	struct tris_dial *dial;
	struct sla_ringing_station *ringing_station;
	const char *cid_name = NULL, *cid_num = NULL;
	enum tris_dial_result res;

	if (!(dial = tris_dial_create()))
		return -1;

	tris_dial_set_state_callback(dial, sla_dial_state_callback);
	tech_data = tris_strdupa(station->device);
	tech = strsep(&tech_data, "/");

	if (tris_dial_append(dial, tech, tech_data) == -1) {
		tris_dial_destroy(dial);
		return -1;
	}

	if (!sla.attempt_callerid && !tris_strlen_zero(ringing_trunk->trunk->chan->cid.cid_name)) {
		cid_name = tris_strdupa(ringing_trunk->trunk->chan->cid.cid_name);
		tris_free(ringing_trunk->trunk->chan->cid.cid_name);
		ringing_trunk->trunk->chan->cid.cid_name = NULL;
	}
	if (!sla.attempt_callerid && !tris_strlen_zero(ringing_trunk->trunk->chan->cid.cid_num)) {
		cid_num = tris_strdupa(ringing_trunk->trunk->chan->cid.cid_num);
		tris_free(ringing_trunk->trunk->chan->cid.cid_num);
		ringing_trunk->trunk->chan->cid.cid_num = NULL;
	}

	res = tris_dial_run(dial, ringing_trunk->trunk->chan, 1, 0);
	
	if (cid_name)
		ringing_trunk->trunk->chan->cid.cid_name = tris_strdup(cid_name);
	if (cid_num)
		ringing_trunk->trunk->chan->cid.cid_num = tris_strdup(cid_num);
	
	if (res != TRIS_DIAL_RESULT_TRYING) {
		struct sla_failed_station *failed_station;
		tris_dial_destroy(dial);
		if (!(failed_station = tris_calloc(1, sizeof(*failed_station))))
			return -1;
		failed_station->station = station;
		failed_station->last_try = tris_tvnow();
		TRIS_LIST_INSERT_HEAD(&sla.failed_stations, failed_station, entry);
		return -1;
	}
	if (!(ringing_station = sla_create_ringing_station(station))) {
		tris_dial_join(dial);
		tris_dial_destroy(dial);
		return -1;
	}

	station->dial = dial;

	TRIS_LIST_INSERT_HEAD(&sla.ringing_stations, ringing_station, entry);

	return 0;
}

/*! \brief Check to see if a station is in use
 */
static int sla_check_inuse_station(const struct sla_station *station)
{
	struct sla_trunk_ref *trunk_ref;

	TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
		if (trunk_ref->chan)
			return 1;
	}

	return 0;
}

static struct sla_trunk_ref *sla_find_trunk_ref(const struct sla_station *station,
	const struct sla_trunk *trunk)
{
	struct sla_trunk_ref *trunk_ref = NULL;

	TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
		if (trunk_ref->trunk == trunk)
			break;
	}

	return trunk_ref;
}

/*! \brief Calculate the ring delay for a given ringing trunk on a station
 * \param station the station
 * \param ringing_trunk the trunk.  If NULL, the highest priority ringing trunk will be used
 * \return the number of ms left before the delay is complete, or INT_MAX if there is no delay
 */
static int sla_check_station_delay(struct sla_station *station, 
	struct sla_ringing_trunk *ringing_trunk)
{
	struct sla_trunk_ref *trunk_ref;
	unsigned int delay = UINT_MAX;
	int time_left, time_elapsed;

	if (!ringing_trunk)
		ringing_trunk = sla_choose_ringing_trunk(station, &trunk_ref, 0);
	else
		trunk_ref = sla_find_trunk_ref(station, ringing_trunk->trunk);

	if (!ringing_trunk || !trunk_ref)
		return delay;

	/* If this station has a ring delay specific to the highest priority
	 * ringing trunk, use that.  Otherwise, use the ring delay specified
	 * globally for the station. */
	delay = trunk_ref->ring_delay;
	if (!delay)
		delay = station->ring_delay;
	if (!delay)
		return INT_MAX;

	time_elapsed = tris_tvdiff_ms(tris_tvnow(), ringing_trunk->ring_begin);
	time_left = (delay * 1000) - time_elapsed;

	return time_left;
}

/*! \brief Ring stations based on current set of ringing trunks
 * \note Assumes that sla.lock is locked
 */
static void sla_ring_stations(void)
{
	struct sla_station_ref *station_ref;
	struct sla_ringing_trunk *ringing_trunk;

	/* Make sure that every station that uses at least one of the ringing
	 * trunks, is ringing. */
	TRIS_LIST_TRAVERSE(&sla.ringing_trunks, ringing_trunk, entry) {
		TRIS_LIST_TRAVERSE(&ringing_trunk->trunk->stations, station_ref, entry) {
			int time_left;

			/* Is this station already ringing? */
			if (sla_check_ringing_station(station_ref->station))
				continue;

			/* Is this station already in a call? */
			if (sla_check_inuse_station(station_ref->station))
				continue;

			/* Did we fail to dial this station earlier?  If so, has it been
 			 * a minute since we tried? */
			if (sla_check_failed_station(station_ref->station))
				continue;

			/* If this station already timed out while this trunk was ringing,
			 * do not dial it again for this ringing trunk. */
			if (sla_check_timed_out_station(ringing_trunk, station_ref->station))
				continue;

			/* Check for a ring delay in progress */
			time_left = sla_check_station_delay(station_ref->station, ringing_trunk);
			if (time_left != INT_MAX && time_left > 0)
				continue;

			/* It is time to make this station begin to ring.  Do it! */
			sla_ring_station(ringing_trunk, station_ref->station);
		}
	}
	/* Now, all of the stations that should be ringing, are ringing. */
}

static void sla_hangup_stations(void)
{
	struct sla_trunk_ref *trunk_ref;
	struct sla_ringing_station *ringing_station;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.ringing_stations, ringing_station, entry) {
		TRIS_LIST_TRAVERSE(&ringing_station->station->trunks, trunk_ref, entry) {
			struct sla_ringing_trunk *ringing_trunk;
			tris_mutex_lock(&sla.lock);
			TRIS_LIST_TRAVERSE(&sla.ringing_trunks, ringing_trunk, entry) {
				if (trunk_ref->trunk == ringing_trunk->trunk)
					break;
			}
			tris_mutex_unlock(&sla.lock);
			if (ringing_trunk)
				break;
		}
		if (!trunk_ref) {
			TRIS_LIST_REMOVE_CURRENT(entry);
			tris_dial_join(ringing_station->station->dial);
			tris_dial_destroy(ringing_station->station->dial);
			ringing_station->station->dial = NULL;
			tris_free(ringing_station);
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END
}

static void sla_handle_ringing_trunk_event(void)
{
	tris_mutex_lock(&sla.lock);
	sla_ring_stations();
	tris_mutex_unlock(&sla.lock);

	/* Find stations that shouldn't be ringing anymore. */
	sla_hangup_stations();
}

static void sla_handle_hold_event(struct sla_event *event)
{
	tris_atomic_fetchadd_int((int *) &event->trunk_ref->trunk->hold_stations, 1);
	event->trunk_ref->state = SLA_TRUNK_STATE_ONHOLD_BYME;
	tris_devstate_changed(TRIS_DEVICE_ONHOLD, "SLA:%s_%s", 
		event->station->name, event->trunk_ref->trunk->name);
	sla_change_trunk_state(event->trunk_ref->trunk, SLA_TRUNK_STATE_ONHOLD, 
		INACTIVE_TRUNK_REFS, event->trunk_ref);

	if (event->trunk_ref->trunk->active_stations == 1) {
		/* The station putting it on hold is the only one on the call, so start
		 * Music on hold to the trunk. */
		event->trunk_ref->trunk->on_hold = 1;
		tris_indicate(event->trunk_ref->trunk->chan, TRIS_CONTROL_HOLD);
	}

	tris_softhangup(event->trunk_ref->chan, TRIS_SOFTHANGUP_DEV);
	event->trunk_ref->chan = NULL;
}

/*! \brief Process trunk ring timeouts
 * \note Called with sla.lock locked
 * \return non-zero if a change to the ringing trunks was made
 */
static int sla_calc_trunk_timeouts(unsigned int *timeout)
{
	struct sla_ringing_trunk *ringing_trunk;
	int res = 0;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.ringing_trunks, ringing_trunk, entry) {
		int time_left, time_elapsed;
		if (!ringing_trunk->trunk->ring_timeout)
			continue;
		time_elapsed = tris_tvdiff_ms(tris_tvnow(), ringing_trunk->ring_begin);
		time_left = (ringing_trunk->trunk->ring_timeout * 1000) - time_elapsed;
		if (time_left <= 0) {
			pbx_builtin_setvar_helper(ringing_trunk->trunk->chan, "SLATRUNK_STATUS", "RINGTIMEOUT");
			TRIS_LIST_REMOVE_CURRENT(entry);
			sla_stop_ringing_trunk(ringing_trunk);
			res = 1;
			continue;
		}
		if (time_left < *timeout)
			*timeout = time_left;
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	return res;
}

/*! \brief Process station ring timeouts
 * \note Called with sla.lock locked
 * \return non-zero if a change to the ringing stations was made
 */
static int sla_calc_station_timeouts(unsigned int *timeout)
{
	struct sla_ringing_trunk *ringing_trunk;
	struct sla_ringing_station *ringing_station;
	int res = 0;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.ringing_stations, ringing_station, entry) {
		unsigned int ring_timeout = 0;
		int time_elapsed, time_left = INT_MAX, final_trunk_time_left = INT_MIN;
		struct sla_trunk_ref *trunk_ref;

		/* If there are any ring timeouts specified for a specific trunk
		 * on the station, then use the highest per-trunk ring timeout.
		 * Otherwise, use the ring timeout set for the entire station. */
		TRIS_LIST_TRAVERSE(&ringing_station->station->trunks, trunk_ref, entry) {
			struct sla_station_ref *station_ref;
			int trunk_time_elapsed, trunk_time_left;

			TRIS_LIST_TRAVERSE(&sla.ringing_trunks, ringing_trunk, entry) {
				if (ringing_trunk->trunk == trunk_ref->trunk)
					break;
			}
			if (!ringing_trunk)
				continue;

			/* If there is a trunk that is ringing without a timeout, then the
			 * only timeout that could matter is a global station ring timeout. */
			if (!trunk_ref->ring_timeout)
				break;

			/* This trunk on this station is ringing and has a timeout.
			 * However, make sure this trunk isn't still ringing from a
			 * previous timeout.  If so, don't consider it. */
			TRIS_LIST_TRAVERSE(&ringing_trunk->timed_out_stations, station_ref, entry) {
				if (station_ref->station == ringing_station->station)
					break;
			}
			if (station_ref)
				continue;

			trunk_time_elapsed = tris_tvdiff_ms(tris_tvnow(), ringing_trunk->ring_begin);
			trunk_time_left = (trunk_ref->ring_timeout * 1000) - trunk_time_elapsed;
			if (trunk_time_left > final_trunk_time_left)
				final_trunk_time_left = trunk_time_left;
		}

		/* No timeout was found for ringing trunks, and no timeout for the entire station */
		if (final_trunk_time_left == INT_MIN && !ringing_station->station->ring_timeout)
			continue;

		/* Compute how much time is left for a global station timeout */
		if (ringing_station->station->ring_timeout) {
			ring_timeout = ringing_station->station->ring_timeout;
			time_elapsed = tris_tvdiff_ms(tris_tvnow(), ringing_station->ring_begin);
			time_left = (ring_timeout * 1000) - time_elapsed;
		}

		/* If the time left based on the per-trunk timeouts is smaller than the
		 * global station ring timeout, use that. */
		if (final_trunk_time_left > INT_MIN && final_trunk_time_left < time_left)
			time_left = final_trunk_time_left;

		/* If there is no time left, the station needs to stop ringing */
		if (time_left <= 0) {
			TRIS_LIST_REMOVE_CURRENT(entry);
			sla_stop_ringing_station(ringing_station, SLA_STATION_HANGUP_TIMEOUT);
			res = 1;
			continue;
		}

		/* There is still some time left for this station to ring, so save that
		 * timeout if it is the first event scheduled to occur */
		if (time_left < *timeout)
			*timeout = time_left;
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	return res;
}

/*! \brief Calculate the ring delay for a station
 * \note Assumes sla.lock is locked
 */
static int sla_calc_station_delays(unsigned int *timeout)
{
	struct sla_station *station;
	int res = 0;

	TRIS_LIST_TRAVERSE(&sla_stations, station, entry) {
		struct sla_ringing_trunk *ringing_trunk;
		int time_left;

		/* Ignore stations already ringing */
		if (sla_check_ringing_station(station))
			continue;

		/* Ignore stations already on a call */
		if (sla_check_inuse_station(station))
			continue;

		/* Ignore stations that don't have one of their trunks ringing */
		if (!(ringing_trunk = sla_choose_ringing_trunk(station, NULL, 0)))
			continue;

		if ((time_left = sla_check_station_delay(station, ringing_trunk)) == INT_MAX)
			continue;

		/* If there is no time left, then the station needs to start ringing.
		 * Return non-zero so that an event will be queued up an event to 
		 * make that happen. */
		if (time_left <= 0) {
			res = 1;
			continue;
		}

		if (time_left < *timeout)
			*timeout = time_left;
	}

	return res;
}

/*! \brief Calculate the time until the next known event
 *  \note Called with sla.lock locked */
static int sla_process_timers(struct timespec *ts)
{
	unsigned int timeout = UINT_MAX;
	struct timeval wait;
	unsigned int change_made = 0;

	/* Check for ring timeouts on ringing trunks */
	if (sla_calc_trunk_timeouts(&timeout))
		change_made = 1;

	/* Check for ring timeouts on ringing stations */
	if (sla_calc_station_timeouts(&timeout))
		change_made = 1;

	/* Check for station ring delays */
	if (sla_calc_station_delays(&timeout))
		change_made = 1;

	/* queue reprocessing of ringing trunks */
	if (change_made)
		sla_queue_event_nolock(SLA_EVENT_RINGING_TRUNK);

	/* No timeout */
	if (timeout == UINT_MAX)
		return 0;

	if (ts) {
		wait = tris_tvadd(tris_tvnow(), tris_samp2tv(timeout, 1000));
		ts->tv_sec = wait.tv_sec;
		ts->tv_nsec = wait.tv_usec * 1000;
	}

	return 1;
}

static int sla_load_config(int reload);

/*! \brief Check if we can do a reload of SLA, and do it if we can */
static void sla_check_reload(void)
{
	struct sla_station *station;
	struct sla_trunk *trunk;

	tris_mutex_lock(&sla.lock);

	if (!TRIS_LIST_EMPTY(&sla.event_q) || !TRIS_LIST_EMPTY(&sla.ringing_trunks) 
		|| !TRIS_LIST_EMPTY(&sla.ringing_stations)) {
		tris_mutex_unlock(&sla.lock);
		return;
	}

	TRIS_RWLIST_RDLOCK(&sla_stations);
	TRIS_RWLIST_TRAVERSE(&sla_stations, station, entry) {
		if (station->ref_count)
			break;
	}
	TRIS_RWLIST_UNLOCK(&sla_stations);
	if (station) {
		tris_mutex_unlock(&sla.lock);
		return;
	}

	TRIS_RWLIST_RDLOCK(&sla_trunks);
	TRIS_RWLIST_TRAVERSE(&sla_trunks, trunk, entry) {
		if (trunk->ref_count)
			break;
	}
	TRIS_RWLIST_UNLOCK(&sla_trunks);
	if (trunk) {
		tris_mutex_unlock(&sla.lock);
		return;
	}

	/* yay */
	sla_load_config(1);
	sla.reload = 0;

	tris_mutex_unlock(&sla.lock);
}

static void *sla_thread(void *data)
{
	struct sla_failed_station *failed_station;
	struct sla_ringing_station *ringing_station;

	tris_mutex_lock(&sla.lock);

	while (!sla.stop) {
		struct sla_event *event;
		struct timespec ts = { 0, };
		unsigned int have_timeout = 0;

		if (TRIS_LIST_EMPTY(&sla.event_q)) {
			if ((have_timeout = sla_process_timers(&ts)))
				tris_cond_timedwait(&sla.cond, &sla.lock, &ts);
			else
				tris_cond_wait(&sla.cond, &sla.lock);
			if (sla.stop)
				break;
		}

		if (have_timeout)
			sla_process_timers(NULL);

		while ((event = TRIS_LIST_REMOVE_HEAD(&sla.event_q, entry))) {
			tris_mutex_unlock(&sla.lock);
			switch (event->type) {
			case SLA_EVENT_HOLD:
				sla_handle_hold_event(event);
				break;
			case SLA_EVENT_DIAL_STATE:
				sla_handle_dial_state_event();
				break;
			case SLA_EVENT_RINGING_TRUNK:
				sla_handle_ringing_trunk_event();
				break;
			case SLA_EVENT_RELOAD:
				sla.reload = 1;
			case SLA_EVENT_CHECK_RELOAD:
				break;
			}
			tris_free(event);
			tris_mutex_lock(&sla.lock);
		}

		if (sla.reload)
			sla_check_reload();
	}

	tris_mutex_unlock(&sla.lock);

	while ((ringing_station = TRIS_LIST_REMOVE_HEAD(&sla.ringing_stations, entry)))
		tris_free(ringing_station);

	while ((failed_station = TRIS_LIST_REMOVE_HEAD(&sla.failed_stations, entry)))
		tris_free(failed_station);

	return NULL;
}

struct dial_trunk_args {
	struct sla_trunk_ref *trunk_ref;
	struct sla_station *station;
	tris_mutex_t *cond_lock;
	tris_cond_t *cond;
};

static void *dial_trunk(void *data)
{
	struct dial_trunk_args *args = data;
	struct tris_dial *dial;
	char *tech, *tech_data;
	enum tris_dial_result dial_res;
	char conf_name[MAX_CONFNUM];
	struct tris_conference *conf;
	struct tris_flags conf_flags = { 0 };
	struct sla_trunk_ref *trunk_ref = args->trunk_ref;
	const char *cid_name = NULL, *cid_num = NULL;

	if (!(dial = tris_dial_create())) {
		tris_mutex_lock(args->cond_lock);
		tris_cond_signal(args->cond);
		tris_mutex_unlock(args->cond_lock);
		return NULL;
	}

	tech_data = tris_strdupa(trunk_ref->trunk->device);
	tech = strsep(&tech_data, "/");
	if (tris_dial_append(dial, tech, tech_data) == -1) {
		tris_mutex_lock(args->cond_lock);
		tris_cond_signal(args->cond);
		tris_mutex_unlock(args->cond_lock);
		tris_dial_destroy(dial);
		return NULL;
	}

	if (!sla.attempt_callerid && !tris_strlen_zero(trunk_ref->chan->cid.cid_name)) {
		cid_name = tris_strdupa(trunk_ref->chan->cid.cid_name);
		tris_free(trunk_ref->chan->cid.cid_name);
		trunk_ref->chan->cid.cid_name = NULL;
	}
	if (!sla.attempt_callerid && !tris_strlen_zero(trunk_ref->chan->cid.cid_num)) {
		cid_num = tris_strdupa(trunk_ref->chan->cid.cid_num);
		tris_free(trunk_ref->chan->cid.cid_num);
		trunk_ref->chan->cid.cid_num = NULL;
	}

	dial_res = tris_dial_run(dial, trunk_ref->chan, 1, 0);

	if (cid_name)
		trunk_ref->chan->cid.cid_name = tris_strdup(cid_name);
	if (cid_num)
		trunk_ref->chan->cid.cid_num = tris_strdup(cid_num);

	if (dial_res != TRIS_DIAL_RESULT_TRYING) {
		tris_mutex_lock(args->cond_lock);
		tris_cond_signal(args->cond);
		tris_mutex_unlock(args->cond_lock);
		tris_dial_destroy(dial);
		return NULL;
	}

	for (;;) {
		unsigned int done = 0;
		switch ((dial_res = tris_dial_state(dial))) {
		case TRIS_DIAL_RESULT_ANSWERED:
			trunk_ref->trunk->chan = tris_dial_answered(dial);
		case TRIS_DIAL_RESULT_HANGUP:
		case TRIS_DIAL_RESULT_INVALID:
		case TRIS_DIAL_RESULT_FAILED:
		case TRIS_DIAL_RESULT_TIMEOUT:
		case TRIS_DIAL_RESULT_UNANSWERED:
			done = 1;
		case TRIS_DIAL_RESULT_TRYING:
		case TRIS_DIAL_RESULT_RINGING:
		case TRIS_DIAL_RESULT_PROGRESS:
		case TRIS_DIAL_RESULT_PROCEEDING:
			break;
		default:
			break;
		}
		if (done)
			break;
	}

	if (!trunk_ref->trunk->chan) {
		tris_mutex_lock(args->cond_lock);
		tris_cond_signal(args->cond);
		tris_mutex_unlock(args->cond_lock);
		tris_dial_join(dial);
		tris_dial_destroy(dial);
		return NULL;
	}

	snprintf(conf_name, sizeof(conf_name), "SLA_%s", trunk_ref->trunk->name);
	tris_set_flag(&conf_flags, 
		CONFFLAG_QUIET | CONFFLAG_MARKEDEXIT | CONFFLAG_MARKEDUSER | 
		CONFFLAG_PASS_DTMF );
	conf = build_conf(conf_name, "", "", 1, 1, 1, trunk_ref->trunk->chan);

	tris_mutex_lock(args->cond_lock);
	tris_cond_signal(args->cond);
	tris_mutex_unlock(args->cond_lock);

	if (conf) {
		conf_run(trunk_ref->trunk->chan, conf, conf_flags.flags, NULL);
		dispose_conf(conf);
		conf = NULL;
	}

	/* If the trunk is going away, it is definitely now IDLE. */
	sla_change_trunk_state(trunk_ref->trunk, SLA_TRUNK_STATE_IDLE, ALL_TRUNK_REFS, NULL);

	trunk_ref->trunk->chan = NULL;
	trunk_ref->trunk->on_hold = 0;

	tris_dial_join(dial);
	tris_dial_destroy(dial);

	return NULL;
}

/*! \brief For a given station, choose the highest priority idle trunk
 */
static struct sla_trunk_ref *sla_choose_idle_trunk(const struct sla_station *station)
{
	struct sla_trunk_ref *trunk_ref = NULL;

	TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
		if (trunk_ref->state == SLA_TRUNK_STATE_IDLE)
			break;
	}

	return trunk_ref;
}

static int sla_station_exec(struct tris_channel *chan, void *data)
{
	char *station_name, *trunk_name;
	struct sla_station *station;
	struct sla_trunk_ref *trunk_ref = NULL;
	char conf_name[MAX_CONFNUM];
	struct tris_flags conf_flags = { 0 };
	struct tris_conference *conf;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Invalid Arguments to SLAStation!\n");
		pbx_builtin_setvar_helper(chan, "SLASTATION_STATUS", "FAILURE");
		return 0;
	}

	trunk_name = tris_strdupa(data);
	station_name = strsep(&trunk_name, "_");

	if (tris_strlen_zero(station_name)) {
		tris_log(LOG_WARNING, "Invalid Arguments to SLAStation!\n");
		pbx_builtin_setvar_helper(chan, "SLASTATION_STATUS", "FAILURE");
		return 0;
	}

	TRIS_RWLIST_RDLOCK(&sla_stations);
	station = sla_find_station(station_name);
	if (station)
		tris_atomic_fetchadd_int((int *) &station->ref_count, 1);
	TRIS_RWLIST_UNLOCK(&sla_stations);

	if (!station) {
		tris_log(LOG_WARNING, "Station '%s' not found!\n", station_name);
		pbx_builtin_setvar_helper(chan, "SLASTATION_STATUS", "FAILURE");
		sla_queue_event(SLA_EVENT_CHECK_RELOAD);
		return 0;
	}

	TRIS_RWLIST_RDLOCK(&sla_trunks);
	if (!tris_strlen_zero(trunk_name)) {
		trunk_ref = sla_find_trunk_ref_byname(station, trunk_name);
	} else
		trunk_ref = sla_choose_idle_trunk(station);
	TRIS_RWLIST_UNLOCK(&sla_trunks);

	if (!trunk_ref) {
		if (tris_strlen_zero(trunk_name))
			tris_log(LOG_NOTICE, "No trunks available for call.\n");
		else {
			tris_log(LOG_NOTICE, "Can't join existing call on trunk "
				"'%s' due to access controls.\n", trunk_name);
		}
		pbx_builtin_setvar_helper(chan, "SLASTATION_STATUS", "CONGESTION");
		tris_atomic_fetchadd_int((int *) &station->ref_count, -1);
		sla_queue_event(SLA_EVENT_CHECK_RELOAD);
		return 0;
	}

	if (trunk_ref->state == SLA_TRUNK_STATE_ONHOLD_BYME) {
		if (tris_atomic_dec_and_test((int *) &trunk_ref->trunk->hold_stations) == 1)
			sla_change_trunk_state(trunk_ref->trunk, SLA_TRUNK_STATE_UP, ALL_TRUNK_REFS, NULL);
		else {
			trunk_ref->state = SLA_TRUNK_STATE_UP;
			tris_devstate_changed(TRIS_DEVICE_INUSE, 
				"SLA:%s_%s", station->name, trunk_ref->trunk->name);
		}
	} else if (trunk_ref->state == SLA_TRUNK_STATE_RINGING) {
		struct sla_ringing_trunk *ringing_trunk;

		tris_mutex_lock(&sla.lock);
		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.ringing_trunks, ringing_trunk, entry) {
			if (ringing_trunk->trunk == trunk_ref->trunk) {
				TRIS_LIST_REMOVE_CURRENT(entry);
				break;
			}
		}
		TRIS_LIST_TRAVERSE_SAFE_END
		tris_mutex_unlock(&sla.lock);

		if (ringing_trunk) {
			answer_trunk_chan(ringing_trunk->trunk->chan);
			sla_change_trunk_state(ringing_trunk->trunk, SLA_TRUNK_STATE_UP, ALL_TRUNK_REFS, NULL);

			free(ringing_trunk);

			/* Queue up reprocessing ringing trunks, and then ringing stations again */
			sla_queue_event(SLA_EVENT_RINGING_TRUNK);
			sla_queue_event(SLA_EVENT_DIAL_STATE);
		}
	}

	trunk_ref->chan = chan;

	if (!trunk_ref->trunk->chan) {
		tris_mutex_t cond_lock;
		tris_cond_t cond;
		pthread_t dont_care;
		struct dial_trunk_args args = {
			.trunk_ref = trunk_ref,
			.station = station,
			.cond_lock = &cond_lock,
			.cond = &cond,
		};
		sla_change_trunk_state(trunk_ref->trunk, SLA_TRUNK_STATE_UP, ALL_TRUNK_REFS, NULL);
		/* Create a thread to dial the trunk and dump it into the conference.
		 * However, we want to wait until the trunk has been dialed and the
		 * conference is created before continuing on here. */
		tris_autoservice_start(chan);
		tris_mutex_init(&cond_lock);
		tris_cond_init(&cond, NULL);
		tris_mutex_lock(&cond_lock);
		tris_pthread_create_detached_background(&dont_care, NULL, dial_trunk, &args);
		tris_cond_wait(&cond, &cond_lock);
		tris_mutex_unlock(&cond_lock);
		tris_mutex_destroy(&cond_lock);
		tris_cond_destroy(&cond);
		tris_autoservice_stop(chan);
		if (!trunk_ref->trunk->chan) {
			tris_debug(1, "Trunk didn't get created. chan: %lx\n", (long) trunk_ref->trunk->chan);
			pbx_builtin_setvar_helper(chan, "SLASTATION_STATUS", "CONGESTION");
			sla_change_trunk_state(trunk_ref->trunk, SLA_TRUNK_STATE_IDLE, ALL_TRUNK_REFS, NULL);
			trunk_ref->chan = NULL;
			tris_atomic_fetchadd_int((int *) &station->ref_count, -1);
			sla_queue_event(SLA_EVENT_CHECK_RELOAD);
			return 0;
		}
	}

	if (tris_atomic_fetchadd_int((int *) &trunk_ref->trunk->active_stations, 1) == 0 &&
		trunk_ref->trunk->on_hold) {
		trunk_ref->trunk->on_hold = 0;
		tris_indicate(trunk_ref->trunk->chan, TRIS_CONTROL_UNHOLD);
		sla_change_trunk_state(trunk_ref->trunk, SLA_TRUNK_STATE_UP, ALL_TRUNK_REFS, NULL);
	}

	snprintf(conf_name, sizeof(conf_name), "SLA_%s", trunk_ref->trunk->name);
	tris_set_flag(&conf_flags, 
		CONFFLAG_QUIET | CONFFLAG_MARKEDEXIT | CONFFLAG_PASS_DTMF | CONFFLAG_SLA_STATION);
	tris_answer(chan);
	conf = build_conf(conf_name, "", "", 0, 0, 1, chan);
	if (conf) {
		conf_run(chan, conf, conf_flags.flags, NULL);
		dispose_conf(conf);
		conf = NULL;
	}
	trunk_ref->chan = NULL;
	if (tris_atomic_dec_and_test((int *) &trunk_ref->trunk->active_stations) &&
		trunk_ref->state != SLA_TRUNK_STATE_ONHOLD_BYME) {
		strncat(conf_name, ",K", sizeof(conf_name) - strlen(conf_name) - 1);
		admin_exec(NULL, conf_name);
		trunk_ref->trunk->hold_stations = 0;
		sla_change_trunk_state(trunk_ref->trunk, SLA_TRUNK_STATE_IDLE, ALL_TRUNK_REFS, NULL);
	}
	
	pbx_builtin_setvar_helper(chan, "SLASTATION_STATUS", "SUCCESS");

	tris_atomic_fetchadd_int((int *) &station->ref_count, -1);
	sla_queue_event(SLA_EVENT_CHECK_RELOAD);

	return 0;
}

static struct sla_trunk_ref *create_trunk_ref(struct sla_trunk *trunk)
{
	struct sla_trunk_ref *trunk_ref;

	if (!(trunk_ref = tris_calloc(1, sizeof(*trunk_ref))))
		return NULL;

	trunk_ref->trunk = trunk;

	return trunk_ref;
}

static struct sla_ringing_trunk *queue_ringing_trunk(struct sla_trunk *trunk)
{
	struct sla_ringing_trunk *ringing_trunk;

	if (!(ringing_trunk = tris_calloc(1, sizeof(*ringing_trunk))))
		return NULL;
	
	ringing_trunk->trunk = trunk;
	ringing_trunk->ring_begin = tris_tvnow();

	sla_change_trunk_state(trunk, SLA_TRUNK_STATE_RINGING, ALL_TRUNK_REFS, NULL);

	tris_mutex_lock(&sla.lock);
	TRIS_LIST_INSERT_HEAD(&sla.ringing_trunks, ringing_trunk, entry);
	tris_mutex_unlock(&sla.lock);

	sla_queue_event(SLA_EVENT_RINGING_TRUNK);

	return ringing_trunk;
}

enum {
	SLA_TRUNK_OPT_MOH = (1 << 0),
};

enum {
	SLA_TRUNK_OPT_ARG_MOH_CLASS = 0,
	SLA_TRUNK_OPT_ARG_ARRAY_SIZE = 1,
};

TRIS_APP_OPTIONS(sla_trunk_opts, BEGIN_OPTIONS
	TRIS_APP_OPTION_ARG('M', SLA_TRUNK_OPT_MOH, SLA_TRUNK_OPT_ARG_MOH_CLASS),
END_OPTIONS );

static int sla_trunk_exec(struct tris_channel *chan, void *data)
{
	char conf_name[MAX_CONFNUM];
	struct tris_conference *conf;
	struct tris_flags conf_flags = { 0 };
	struct sla_trunk *trunk;
	struct sla_ringing_trunk *ringing_trunk;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(trunk_name);
		TRIS_APP_ARG(options);
	);
	char *opts[SLA_TRUNK_OPT_ARG_ARRAY_SIZE] = { NULL, };
	char *conf_opt_args[OPT_ARG_ARRAY_SIZE] = { NULL, };
	struct tris_flags opt_flags = { 0 };
	char *parse;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_ERROR, "The SLATrunk application requires an argument, the trunk name\n");
		return -1;
	}

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);
	if (args.argc == 2) {
		if (tris_app_parse_options(sla_trunk_opts, &opt_flags, opts, args.options)) {
			tris_log(LOG_ERROR, "Error parsing options for SLATrunk\n");
			return -1;
		}
	}

	TRIS_RWLIST_RDLOCK(&sla_trunks);
	trunk = sla_find_trunk(args.trunk_name);
	if (trunk)
		tris_atomic_fetchadd_int((int *) &trunk->ref_count, 1);
	TRIS_RWLIST_UNLOCK(&sla_trunks);

	if (!trunk) {
		tris_log(LOG_ERROR, "SLA Trunk '%s' not found!\n", args.trunk_name);
		pbx_builtin_setvar_helper(chan, "SLATRUNK_STATUS", "FAILURE");
		tris_atomic_fetchadd_int((int *) &trunk->ref_count, -1);
		sla_queue_event(SLA_EVENT_CHECK_RELOAD);	
		return 0;
	}

	if (trunk->chan) {
		tris_log(LOG_ERROR, "Call came in on %s, but the trunk is already in use!\n",
			args.trunk_name);
		pbx_builtin_setvar_helper(chan, "SLATRUNK_STATUS", "FAILURE");
		tris_atomic_fetchadd_int((int *) &trunk->ref_count, -1);
		sla_queue_event(SLA_EVENT_CHECK_RELOAD);	
		return 0;
	}

	trunk->chan = chan;

	if (!(ringing_trunk = queue_ringing_trunk(trunk))) {
		pbx_builtin_setvar_helper(chan, "SLATRUNK_STATUS", "FAILURE");
		tris_atomic_fetchadd_int((int *) &trunk->ref_count, -1);
		sla_queue_event(SLA_EVENT_CHECK_RELOAD);	
		return 0;
	}

	snprintf(conf_name, sizeof(conf_name), "SLA_%s", args.trunk_name);
	conf = build_conf(conf_name, "", "", 1, 1, 1, chan);
	if (!conf) {
		pbx_builtin_setvar_helper(chan, "SLATRUNK_STATUS", "FAILURE");
		tris_atomic_fetchadd_int((int *) &trunk->ref_count, -1);
		sla_queue_event(SLA_EVENT_CHECK_RELOAD);	
		return 0;
	}
	tris_set_flag(&conf_flags, 
		CONFFLAG_QUIET | CONFFLAG_MARKEDEXIT | CONFFLAG_MARKEDUSER | CONFFLAG_PASS_DTMF | CONFFLAG_NO_AUDIO_UNTIL_UP);

	if (tris_test_flag(&opt_flags, SLA_TRUNK_OPT_MOH)) {
		tris_indicate(chan, -1);
		tris_set_flag(&conf_flags, CONFFLAG_MOH);
		conf_opt_args[OPT_ARG_MOH_CLASS] = opts[SLA_TRUNK_OPT_ARG_MOH_CLASS];
	} else
		tris_indicate(chan, TRIS_CONTROL_RINGING);

	conf_run(chan, conf, conf_flags.flags, opts);
	dispose_conf(conf);
	conf = NULL;
	trunk->chan = NULL;
	trunk->on_hold = 0;

	sla_change_trunk_state(trunk, SLA_TRUNK_STATE_IDLE, ALL_TRUNK_REFS, NULL);

	if (!pbx_builtin_getvar_helper(chan, "SLATRUNK_STATUS"))
		pbx_builtin_setvar_helper(chan, "SLATRUNK_STATUS", "SUCCESS");

	/* Remove the entry from the list of ringing trunks if it is still there. */
	tris_mutex_lock(&sla.lock);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sla.ringing_trunks, ringing_trunk, entry) {
		if (ringing_trunk->trunk == trunk) {
			TRIS_LIST_REMOVE_CURRENT(entry);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	tris_mutex_unlock(&sla.lock);
	if (ringing_trunk) {
		tris_free(ringing_trunk);
		pbx_builtin_setvar_helper(chan, "SLATRUNK_STATUS", "UNANSWERED");
		/* Queue reprocessing of ringing trunks to make stations stop ringing
		 * that shouldn't be ringing after this trunk stopped. */
		sla_queue_event(SLA_EVENT_RINGING_TRUNK);
	}

	tris_atomic_fetchadd_int((int *) &trunk->ref_count, -1);
	sla_queue_event(SLA_EVENT_CHECK_RELOAD);	

	return 0;
}

static enum tris_device_state sla_state(const char *data)
{
	char *buf, *station_name, *trunk_name;
	struct sla_station *station;
	struct sla_trunk_ref *trunk_ref;
	enum tris_device_state res = TRIS_DEVICE_INVALID;

	trunk_name = buf = tris_strdupa(data);
	station_name = strsep(&trunk_name, "_");

	TRIS_RWLIST_RDLOCK(&sla_stations);
	TRIS_LIST_TRAVERSE(&sla_stations, station, entry) {
		if (strcasecmp(station_name, station->name))
			continue;
		TRIS_RWLIST_RDLOCK(&sla_trunks);
		TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
			if (!strcasecmp(trunk_name, trunk_ref->trunk->name))
				break;
		}
		if (!trunk_ref) {
			TRIS_RWLIST_UNLOCK(&sla_trunks);
			break;
		}
		res = sla_state_to_devstate(trunk_ref->state);
		TRIS_RWLIST_UNLOCK(&sla_trunks);
	}
	TRIS_RWLIST_UNLOCK(&sla_stations);

	if (res == TRIS_DEVICE_INVALID) {
		tris_log(LOG_ERROR, "Could not determine state for trunk %s on station %s!\n",
			trunk_name, station_name);
	}

	return res;
}

static void destroy_trunk(struct sla_trunk *trunk)
{
	struct sla_station_ref *station_ref;

	if (!tris_strlen_zero(trunk->autocontext))
		tris_context_remove_extension(trunk->autocontext, "s", 1, sla_registrar);

	while ((station_ref = TRIS_LIST_REMOVE_HEAD(&trunk->stations, entry)))
		tris_free(station_ref);

	tris_string_field_free_memory(trunk);
	tris_free(trunk);
}

static void destroy_station(struct sla_station *station)
{
	struct sla_trunk_ref *trunk_ref;

	if (!tris_strlen_zero(station->autocontext)) {
		TRIS_RWLIST_RDLOCK(&sla_trunks);
		TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
			char exten[TRIS_MAX_EXTENSION];
			char hint[TRIS_MAX_APP];
			snprintf(exten, sizeof(exten), "%s_%s", station->name, trunk_ref->trunk->name);
			snprintf(hint, sizeof(hint), "SLA:%s", exten);
			tris_context_remove_extension(station->autocontext, exten, 
				1, sla_registrar);
			tris_context_remove_extension(station->autocontext, hint, 
				PRIORITY_HINT, sla_registrar);
		}
		TRIS_RWLIST_UNLOCK(&sla_trunks);
	}

	while ((trunk_ref = TRIS_LIST_REMOVE_HEAD(&station->trunks, entry)))
		tris_free(trunk_ref);

	tris_string_field_free_memory(station);
	tris_free(station);
}

static void sla_destroy(void)
{
	struct sla_trunk *trunk;
	struct sla_station *station;

	TRIS_RWLIST_WRLOCK(&sla_trunks);
	while ((trunk = TRIS_RWLIST_REMOVE_HEAD(&sla_trunks, entry)))
		destroy_trunk(trunk);
	TRIS_RWLIST_UNLOCK(&sla_trunks);

	TRIS_RWLIST_WRLOCK(&sla_stations);
	while ((station = TRIS_RWLIST_REMOVE_HEAD(&sla_stations, entry)))
		destroy_station(station);
	TRIS_RWLIST_UNLOCK(&sla_stations);

	if (sla.thread != TRIS_PTHREADT_NULL) {
		tris_mutex_lock(&sla.lock);
		sla.stop = 1;
		tris_cond_signal(&sla.cond);
		tris_mutex_unlock(&sla.lock);
		pthread_join(sla.thread, NULL);
	}

	/* Drop any created contexts from the dialplan */
	tris_context_destroy(NULL, sla_registrar);

	tris_mutex_destroy(&sla.lock);
	tris_cond_destroy(&sla.cond);
}

static int sla_check_device(const char *device)
{
	char *tech, *tech_data;

	tech_data = tris_strdupa(device);
	tech = strsep(&tech_data, "/");

	if (tris_strlen_zero(tech) || tris_strlen_zero(tech_data))
		return -1;

	return 0;
}

static int sla_build_trunk(struct tris_config *cfg, const char *cat)
{
	struct sla_trunk *trunk;
	struct tris_variable *var;
	const char *dev;

	if (!(dev = tris_variable_retrieve(cfg, cat, "device"))) {
		tris_log(LOG_ERROR, "SLA Trunk '%s' defined with no device!\n", cat);
		return -1;
	}

	if (sla_check_device(dev)) {
		tris_log(LOG_ERROR, "SLA Trunk '%s' define with invalid device '%s'!\n",
			cat, dev);
		return -1;
	}

	if (!(trunk = tris_calloc(1, sizeof(*trunk))))
		return -1;
	if (tris_string_field_init(trunk, 32)) {
		tris_free(trunk);
		return -1;
	}

	tris_string_field_set(trunk, name, cat);
	tris_string_field_set(trunk, device, dev);

	for (var = tris_variable_browse(cfg, cat); var; var = var->next) {
		if (!strcasecmp(var->name, "autocontext"))
			tris_string_field_set(trunk, autocontext, var->value);
		else if (!strcasecmp(var->name, "ringtimeout")) {
			if (sscanf(var->value, "%30u", &trunk->ring_timeout) != 1) {
				tris_log(LOG_WARNING, "Invalid ringtimeout '%s' specified for trunk '%s'\n",
					var->value, trunk->name);
				trunk->ring_timeout = 0;
			}
		} else if (!strcasecmp(var->name, "barge"))
			trunk->barge_disabled = tris_false(var->value);
		else if (!strcasecmp(var->name, "hold")) {
			if (!strcasecmp(var->value, "private"))
				trunk->hold_access = SLA_HOLD_PRIVATE;
			else if (!strcasecmp(var->value, "open"))
				trunk->hold_access = SLA_HOLD_OPEN;
			else {
				tris_log(LOG_WARNING, "Invalid value '%s' for hold on trunk %s\n",
					var->value, trunk->name);
			}
		} else if (strcasecmp(var->name, "type") && strcasecmp(var->name, "device")) {
			tris_log(LOG_ERROR, "Invalid option '%s' specified at line %d of %s!\n",
				var->name, var->lineno, SLA_CONFIG_FILE);
		}
	}

	if (!tris_strlen_zero(trunk->autocontext)) {
		struct tris_context *context;
		context = tris_context_find_or_create(NULL, NULL, trunk->autocontext, sla_registrar);
		if (!context) {
			tris_log(LOG_ERROR, "Failed to automatically find or create "
				"context '%s' for SLA!\n", trunk->autocontext);
			destroy_trunk(trunk);
			return -1;
		}
		if (tris_add_extension2(context, 0 /* don't replace */, "s", 1,
			NULL, NULL, slatrunk_app, tris_strdup(trunk->name), tris_free_ptr, sla_registrar)) {
			tris_log(LOG_ERROR, "Failed to automatically create extension "
				"for trunk '%s'!\n", trunk->name);
			destroy_trunk(trunk);
			return -1;
		}
	}

	TRIS_RWLIST_WRLOCK(&sla_trunks);
	TRIS_RWLIST_INSERT_TAIL(&sla_trunks, trunk, entry);
	TRIS_RWLIST_UNLOCK(&sla_trunks);

	return 0;
}

static void sla_add_trunk_to_station(struct sla_station *station, struct tris_variable *var)
{
	struct sla_trunk *trunk;
	struct sla_trunk_ref *trunk_ref;
	struct sla_station_ref *station_ref;
	char *trunk_name, *options, *cur;

	options = tris_strdupa(var->value);
	trunk_name = strsep(&options, ",");
	
	TRIS_RWLIST_RDLOCK(&sla_trunks);
	TRIS_RWLIST_TRAVERSE(&sla_trunks, trunk, entry) {
		if (!strcasecmp(trunk->name, trunk_name))
			break;
	}

	TRIS_RWLIST_UNLOCK(&sla_trunks);
	if (!trunk) {
		tris_log(LOG_ERROR, "Trunk '%s' not found!\n", var->value);
		return;
	}
	if (!(trunk_ref = create_trunk_ref(trunk)))
		return;
	trunk_ref->state = SLA_TRUNK_STATE_IDLE;

	while ((cur = strsep(&options, ","))) {
		char *name, *value = cur;
		name = strsep(&value, "=");
		if (!strcasecmp(name, "ringtimeout")) {
			if (sscanf(value, "%30u", &trunk_ref->ring_timeout) != 1) {
				tris_log(LOG_WARNING, "Invalid ringtimeout value '%s' for "
					"trunk '%s' on station '%s'\n", value, trunk->name, station->name);
				trunk_ref->ring_timeout = 0;
			}
		} else if (!strcasecmp(name, "ringdelay")) {
			if (sscanf(value, "%30u", &trunk_ref->ring_delay) != 1) {
				tris_log(LOG_WARNING, "Invalid ringdelay value '%s' for "
					"trunk '%s' on station '%s'\n", value, trunk->name, station->name);
				trunk_ref->ring_delay = 0;
			}
		} else {
			tris_log(LOG_WARNING, "Invalid option '%s' for "
				"trunk '%s' on station '%s'\n", name, trunk->name, station->name);
		}
	}

	if (!(station_ref = sla_create_station_ref(station))) {
		tris_free(trunk_ref);
		return;
	}
	tris_atomic_fetchadd_int((int *) &trunk->num_stations, 1);
	TRIS_RWLIST_WRLOCK(&sla_trunks);
	TRIS_LIST_INSERT_TAIL(&trunk->stations, station_ref, entry);
	TRIS_RWLIST_UNLOCK(&sla_trunks);
	TRIS_LIST_INSERT_TAIL(&station->trunks, trunk_ref, entry);
}

static int sla_build_station(struct tris_config *cfg, const char *cat)
{
	struct sla_station *station;
	struct tris_variable *var;
	const char *dev;

	if (!(dev = tris_variable_retrieve(cfg, cat, "device"))) {
		tris_log(LOG_ERROR, "SLA Station '%s' defined with no device!\n", cat);
		return -1;
	}

	if (!(station = tris_calloc(1, sizeof(*station))))
		return -1;
	if (tris_string_field_init(station, 32)) {
		tris_free(station);
		return -1;
	}

	tris_string_field_set(station, name, cat);
	tris_string_field_set(station, device, dev);

	for (var = tris_variable_browse(cfg, cat); var; var = var->next) {
		if (!strcasecmp(var->name, "trunk"))
			sla_add_trunk_to_station(station, var);
		else if (!strcasecmp(var->name, "autocontext"))
			tris_string_field_set(station, autocontext, var->value);
		else if (!strcasecmp(var->name, "ringtimeout")) {
			if (sscanf(var->value, "%30u", &station->ring_timeout) != 1) {
				tris_log(LOG_WARNING, "Invalid ringtimeout '%s' specified for station '%s'\n",
					var->value, station->name);
				station->ring_timeout = 0;
			}
		} else if (!strcasecmp(var->name, "ringdelay")) {
			if (sscanf(var->value, "%30u", &station->ring_delay) != 1) {
				tris_log(LOG_WARNING, "Invalid ringdelay '%s' specified for station '%s'\n",
					var->value, station->name);
				station->ring_delay = 0;
			}
		} else if (!strcasecmp(var->name, "hold")) {
			if (!strcasecmp(var->value, "private"))
				station->hold_access = SLA_HOLD_PRIVATE;
			else if (!strcasecmp(var->value, "open"))
				station->hold_access = SLA_HOLD_OPEN;
			else {
				tris_log(LOG_WARNING, "Invalid value '%s' for hold on station %s\n",
					var->value, station->name);
			}

		} else if (strcasecmp(var->name, "type") && strcasecmp(var->name, "device")) {
			tris_log(LOG_ERROR, "Invalid option '%s' specified at line %d of %s!\n",
				var->name, var->lineno, SLA_CONFIG_FILE);
		}
	}

	if (!tris_strlen_zero(station->autocontext)) {
		struct tris_context *context;
		struct sla_trunk_ref *trunk_ref;
		context = tris_context_find_or_create(NULL, NULL, station->autocontext, sla_registrar);
		if (!context) {
			tris_log(LOG_ERROR, "Failed to automatically find or create "
				"context '%s' for SLA!\n", station->autocontext);
			destroy_station(station);
			return -1;
		}
		/* The extension for when the handset goes off-hook.
		 * exten => station1,1,SLAStation(station1) */
		if (tris_add_extension2(context, 0 /* don't replace */, station->name, 1,
			NULL, NULL, slastation_app, tris_strdup(station->name), tris_free_ptr, sla_registrar)) {
			tris_log(LOG_ERROR, "Failed to automatically create extension "
				"for trunk '%s'!\n", station->name);
			destroy_station(station);
			return -1;
		}
		TRIS_RWLIST_RDLOCK(&sla_trunks);
		TRIS_LIST_TRAVERSE(&station->trunks, trunk_ref, entry) {
			char exten[TRIS_MAX_EXTENSION];
			char hint[TRIS_MAX_APP];
			snprintf(exten, sizeof(exten), "%s_%s", station->name, trunk_ref->trunk->name);
			snprintf(hint, sizeof(hint), "SLA:%s", exten);
			/* Extension for this line button 
			 * exten => station1_line1,1,SLAStation(station1_line1) */
			if (tris_add_extension2(context, 0 /* don't replace */, exten, 1,
				NULL, NULL, slastation_app, tris_strdup(exten), tris_free_ptr, sla_registrar)) {
				tris_log(LOG_ERROR, "Failed to automatically create extension "
					"for trunk '%s'!\n", station->name);
				destroy_station(station);
				return -1;
			}
			/* Hint for this line button 
			 * exten => station1_line1,hint,SLA:station1_line1 */
			if (tris_add_extension2(context, 0 /* don't replace */, exten, PRIORITY_HINT,
				NULL, NULL, hint, NULL, NULL, sla_registrar)) {
				tris_log(LOG_ERROR, "Failed to automatically create hint "
					"for trunk '%s'!\n", station->name);
				destroy_station(station);
				return -1;
			}
		}
		TRIS_RWLIST_UNLOCK(&sla_trunks);
	}

	TRIS_RWLIST_WRLOCK(&sla_stations);
	TRIS_RWLIST_INSERT_TAIL(&sla_stations, station, entry);
	TRIS_RWLIST_UNLOCK(&sla_stations);

	return 0;
}

static int sla_load_config(int reload)
{
	struct tris_config *cfg;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	const char *cat = NULL;
	int res = 0;
	const char *val;

	if (!reload) {
		tris_mutex_init(&sla.lock);
		tris_cond_init(&sla.cond, NULL);
	}

	if (!(cfg = tris_config_load(SLA_CONFIG_FILE, config_flags))) {
		return 0; /* Treat no config as normal */
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file " SLA_CONFIG_FILE " is in an invalid format.  Aborting.\n");
		return 0;
	}

	if ((val = tris_variable_retrieve(cfg, "general", "attemptcallerid")))
		sla.attempt_callerid = tris_true(val);

	while ((cat = tris_category_browse(cfg, cat)) && !res) {
		const char *type;
		if (!strcasecmp(cat, "general"))
			continue;
		if (!(type = tris_variable_retrieve(cfg, cat, "type"))) {
			tris_log(LOG_WARNING, "Invalid entry in %s defined with no type!\n",
				SLA_CONFIG_FILE);
			continue;
		}
		if (!strcasecmp(type, "trunk"))
			res = sla_build_trunk(cfg, cat);
		else if (!strcasecmp(type, "station"))
			res = sla_build_station(cfg, cat);
		else {
			tris_log(LOG_WARNING, "Entry in %s defined with invalid type '%s'!\n",
				SLA_CONFIG_FILE, type);
		}
	}

	tris_config_destroy(cfg);

	if (!reload && (!TRIS_LIST_EMPTY(&sla_stations) || !TRIS_LIST_EMPTY(&sla_stations)))
		tris_pthread_create(&sla.thread, NULL, sla_thread, NULL);

	return res;
}

static int acf_meetme_info_eval(char *keyword, struct tris_conference *conf)
{
	if (!strcasecmp("lock", keyword)) {
		return conf->locked;
	} else if (!strcasecmp("parties", keyword)) {
		return conf->users;
	} else if (!strcasecmp("activity", keyword)) {
		time_t now;
		now = time(NULL);
		return (now - conf->start);
	} else if (!strcasecmp("dynamic", keyword)) {
		return conf->isdynamic;
	} else {
		return -1;
	}

}

static int acf_meetme_info(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_conference *conf;
	char *parse;
	int result = -2; /* only non-negative numbers valid, -1 is used elsewhere */
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(keyword);
		TRIS_APP_ARG(confno);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_ERROR, "Syntax: MEETME_INFO() requires two arguments\n");
		return -1;
	}

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (tris_strlen_zero(args.keyword)) {
		tris_log(LOG_ERROR, "Syntax: MEETME_INFO() requires a keyword\n");
		return -1;
	}

	if (tris_strlen_zero(args.confno)) {
		tris_log(LOG_ERROR, "Syntax: MEETME_INFO() requires a conference number\n");
		return -1;
	}

	TRIS_LIST_LOCK(&confs);
	TRIS_LIST_TRAVERSE(&confs, conf, list) {
		if (!strcmp(args.confno, conf->confno)) {
			result = acf_meetme_info_eval(args.keyword, conf);
			break;
		}
	}
	TRIS_LIST_UNLOCK(&confs);

	if (result > -1) {
		snprintf(buf, len, "%d", result);
	} else if (result == -1) {
		tris_log(LOG_NOTICE, "Error: invalid keyword: '%s'\n", args.keyword);
		snprintf(buf, len, "0");
	} else if (result == -2) {
		tris_log(LOG_NOTICE, "Error: conference (%s) not found\n", args.confno); 
		snprintf(buf, len, "0");
	}

	return 0;
}


static struct tris_custom_function meetme_info_acf = {
	.name = "MEETME_INFO",
	.synopsis = "Query a given conference of various properties.",
	.syntax = "MEETME_INFO(<keyword>,<confno>)",
	.read = acf_meetme_info,
	.desc =
"Returns information from a given keyword. (For booleans 1-true, 0-false)\n"
"  Options:\n"
"    lock     - boolean of whether the corresponding conference is locked\n" 
"    parties  - number of parties in a given conference\n"
"    activity - duration of conference in seconds\n"
"    dynamic  - boolean of whether the corresponding coference is dynamic\n",
};


static int load_config(int reload)
{
	load_config_meetme();

	if (reload) {
		sla_queue_event(SLA_EVENT_RELOAD);
		tris_log(LOG_NOTICE, "A reload of the SLA configuration has been requested "
			"and will be completed when the system is idle.\n");
		return 0;
	}
	
	return sla_load_config(0);
}

static int unload_module(void)
{
	int res = 0;
	
	tris_cli_unregister_multiple(cli_meetme, ARRAY_LEN(cli_meetme));
	res = tris_manager_unregister("MeetmeMute");
	res |= tris_manager_unregister("MeetmeUnmute");
	res |= tris_manager_unregister("MeetmeList");
	res |= tris_unregister_application(app4);
	res |= tris_unregister_application(app3);
	res |= tris_unregister_application(app2);
	res |= tris_unregister_application(app);
	res |= tris_unregister_application(slastation_app);
	res |= tris_unregister_application(slatrunk_app);

	tris_devstate_prov_del("Meetme");
	tris_devstate_prov_del("SLA");
	
	sla_destroy();
	
	res |= tris_custom_function_unregister(&meetme_info_acf);
	tris_unload_realtime("meetme");

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= load_config(0);

	tris_cli_register_multiple(cli_meetme, ARRAY_LEN(cli_meetme));
	res |= tris_manager_register("MeetmeRecord", 0, 
				    action_meetmerecord, "Record a Meetme");
	res |= tris_manager_register("MeetmeMute", EVENT_FLAG_CALL, 
				    action_meetmemute, "Mute a Meetme user");
	res |= tris_manager_register("MeetmeUnmute", EVENT_FLAG_CALL, 
				    action_meetmeunmute, "Unmute a Meetme user");
	res |= tris_manager_register2("MeetmeList", EVENT_FLAG_REPORTING, 
				    action_meetmelist, "List participants in a conference", mandescr_meetmelist);
	res |= tris_manager_register2("SatelliteList", EVENT_FLAG_CALL, 
				    action_satellitelist, "Satellite List", mandescr_satellitelist);
	res |= tris_manager_register2("SatelliteRoomDetail", EVENT_FLAG_CALL, 
				    action_satelliteroomdetail, "Satellite Room Detail", mandescr_satelliteroomdetail);
	res |= tris_manager_register("SatelliteCanParticipate", EVENT_FLAG_CALL, 
				    action_satellitecanparticipate, "List that one can participant");
	res |= tris_manager_register("SatelliteAddMember", EVENT_FLAG_CALL, 
				    action_satelliteaddmember, "Add Member");
	res |= tris_manager_register("SatelliteRemoveMember", EVENT_FLAG_CALL, 
				    action_satelliteremovemember, "Remove Member");
	res |= tris_manager_register("SatelliteSetTalking", EVENT_FLAG_CALL, 
				    action_satellitesettalking, "Set Talking");
	res |= tris_manager_register("SatelliteUserDetail", EVENT_FLAG_CALL, 
				    action_satelliteuserdetail, "User Detail");
	res |= tris_register_application_xml(app6, urgencyconf_exec);
	res |= tris_register_application_xml(app5, scheduleconf_exec);
	res |= tris_register_application_xml(app4, channel_admin_exec);
	res |= tris_register_application_xml(app3, admin_exec);
	res |= tris_register_application_xml(app2, count_exec);
	res |= tris_register_application_xml(app, conf_exec);
	res |= tris_register_application_xml(slastation_app, sla_station_exec);
	res |= tris_register_application_xml(slatrunk_app, sla_trunk_exec);

	res |= tris_devstate_prov_add("Meetme", meetmestate);
	res |= tris_devstate_prov_add("SLA", sla_state);

	res |= tris_custom_function_register(&meetme_info_acf);
	tris_realtime_require_field("meetme", "confno", RQ_UINTEGER2, 3, "members", RQ_UINTEGER1, 3, NULL);

	return res;
}

static int reload(void)
{
	tris_unload_realtime("meetme");
	return load_config(1);
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "MeetMe conference bridge",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );

