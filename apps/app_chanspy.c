/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005 Anthony Minessale II (anthmct@yahoo.com)
 * Copyright (C) 2005 - 2008, Digium, Inc.
 *
 * A license has been granted to Digium (via disclaimer) for the use of
 * this code.
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
 * \brief ChanSpy: Listen in on any channel.
 *
 * \author Anthony Minessale II <anthmct@yahoo.com>
 * \author Joshua Colp <jcolp@digium.com>
 * \author Russell Bryant <russell@digium.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 228195 $")

#include <ctype.h>
#include <errno.h>

#include "trismedia/paths.h" /* use tris_config_TRIS_MONITOR_DIR */
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/audiohook.h"
#include "trismedia/features.h"
#include "trismedia/app.h"
#include "trismedia/utils.h"
#include "trismedia/say.h"
#include "trismedia/pbx.h"
#include "trismedia/translate.h"
#include "trismedia/manager.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/options.h"
#include "trismedia/res_odbc.h"

#define TRIS_NAME_STRLEN 256
#define NUM_SPYGROUPS 128

/*** DOCUMENTATION
	<application name="ChanSpy" language="en_US">
		<synopsis>
			Listen to a channel, and optionally whisper into it.
		</synopsis>
		<syntax>
			<parameter name="chanprefix" />
			<parameter name="options">
				<optionlist>
					<option name="b">
						<para>Only spy on channels involved in a bridged call.</para>
					</option>
					<option name="B">
						<para>Instead of whispering on a single channel barge in on both
						channels involved in the call.</para>
					</option>
					<option name="d">
						<para>Override the typical numeric DTMF functionality and instead
						use DTMF to switch between spy modes.</para>
						<enumlist>
							<enum name="4">
								<para>spy mode</para>
							</enum>
							<enum name="5">
								<para>whisper mode</para>
							</enum>
							<enum name="6">
								<para>barge mode</para>
							</enum>
						</enumlist>
					</option>
					<option name="g">
						<argument name="grp" required="true">
							<para>Only spy on channels in which one or more of the groups
							listed in <replaceable>grp</replaceable> matches one or more groups from the
							<variable>SPYGROUP</variable> variable set on the channel to be spied upon.</para>
						</argument>
						<note><para>both <replaceable>grp</replaceable> and <variable>SPYGROUP</variable> can contain 
						either a single group or a colon-delimited list of groups, such
						as <literal>sales:support:accounting</literal>.</para></note>
					</option>
					<option name="n" argsep="@">
						<para>Say the name of the person being spied on if that person has recorded
						his/her name. If a context is specified, then that voicemail context will
						be searched when retrieving the name, otherwise the <literal>default</literal> context
						be used when searching for the name (i.e. if SIP/1000 is the channel being
						spied on and no mailbox is specified, then <literal>1000</literal> will be used when searching
						for the name).</para>
						<argument name="mailbox" />
						<argument name="context" />
					</option>
					<option name="q">
						<para>Don't play a beep when beginning to spy on a channel, or speak the
						selected channel name.</para>
					</option>
					<option name="r">
						<para>Record the session to the monitor spool directory. An optional base for the filename 
						may be specified. The default is <literal>chanspy</literal>.</para>
						<argument name="basename" />
					</option>
					<option name="s">
						<para>Skip the playback of the channel type (i.e. SIP, IAX, etc) when
						speaking the selected channel name.</para>
					</option>
					<option name="v">
						<argument name="value" />
						<para>Adjust the initial volume in the range from <literal>-4</literal> 
						to <literal>4</literal>. A negative value refers to a quieter setting.</para>
					</option>
					<option name="w">
						<para>Enable <literal>whisper</literal> mode, so the spying channel can talk to
						the spied-on channel.</para>
					</option>
					<option name="W">
						<para>Enable <literal>private whisper</literal> mode, so the spying channel can
						talk to the spied-on channel but cannot listen to that channel.</para>
					</option>
					<option name="o">
						<para>Only listen to audio coming from this channel.</para>
					</option>
					<option name="X">
						<para>Allow the user to exit ChanSpy to a valid single digit
						numeric extension in the current context or the context
						specified by the <variable>SPY_EXIT_CONTEXT</variable> channel variable. The
						name of the last channel that was spied on will be stored
						in the <variable>SPY_CHANNEL</variable> variable.</para>
					</option>
					<option name="e">
						<argument name="ext" required="true" />
						<para>Enable <emphasis>enforced</emphasis> mode, so the spying channel can
						only monitor extensions whose name is in the <replaceable>ext</replaceable> : delimited 
						list.</para>
					</option>
				</optionlist>		
			</parameter>
		</syntax>
		<description>
			<para>This application is used to listen to the audio from an Trismedia channel. This includes the audio 
			coming in and "out of the channel being spied on. If the <literal>chanprefix</literal> parameter is specified,
			only channels beginning with this string will be spied upon.</para>
			<para>While spying, the following actions may be performed:</para>
			<para> - Dialing <literal>#</literal> cycles the volume level.</para>
			<para> - Dialing <literal>*</literal> will stop spying and look for another channel to spy on.</para>
			<para> - Dialing a series of digits followed by <literal>#</literal> builds a channel name to append
			to 'chanprefix'. For example, executing ChanSpy(Agent) and then dialing the digits '1234#' 
			while spying will begin spying on the channel 'Agent/1234'. Note that this feature will be overridden if the 'd' option
			is used</para>
			<note><para>The <replaceable>X</replaceable> option supersedes the three features above in that if a valid
			single digit extension exists in the correct context ChanSpy will exit to it.
			This also disables choosing a channel based on <literal>chanprefix</literal> and a digit sequence.</para></note>
		</description>
		<see-also>
			<ref type="application">ExtenSpy</ref>
		</see-also>
	</application>
	<application name="ExtenSpy" language="en_US">
		<synopsis>
			Listen to a channel, and optionally whisper into it.
		</synopsis>
		<syntax>
			<parameter name="exten" required="true" argsep="@">
				<argument name="exten" required="true">
					<para>Specify extension.</para>
				</argument>
				<argument name="context">
					<para>Optionally specify a context, defaults to <literal>default</literal>.</para>
				</argument>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="b">
						<para>Only spy on channels involved in a bridged call.</para>
					</option>
					<option name="B">
						<para>Instead of whispering on a single channel barge in on both
						channels involved in the call.</para>
					</option>
					<option name="d">
						<para>Override the typical numeric DTMF functionality and instead
						use DTMF to switch between spy modes.</para>
						<enumlist>
							<enum name="4">
								<para>spy mode</para>
							</enum>
							<enum name="5">
								<para>whisper mode</para>
							</enum>
							<enum name="6">
								<para>barge mode</para>
							</enum>
						</enumlist>
					</option>
					<option name="g">
						<argument name="grp" required="true">
							<para>Only spy on channels in which one or more of the groups
							listed in <replaceable>grp</replaceable> matches one or more groups from the
							<variable>SPYGROUP</variable> variable set on the channel to be spied upon.</para>
						</argument>
						<note><para>both <replaceable>grp</replaceable> and <variable>SPYGROUP</variable> can contain 
						either a single group or a colon-delimited list of groups, such
						as <literal>sales:support:accounting</literal>.</para></note>
					</option>
					<option name="n" argsep="@">
						<para>Say the name of the person being spied on if that person has recorded
						his/her name. If a context is specified, then that voicemail context will
						be searched when retrieving the name, otherwise the <literal>default</literal> context
						be used when searching for the name (i.e. if SIP/1000 is the channel being
						spied on and no mailbox is specified, then <literal>1000</literal> will be used when searching
						for the name).</para>
						<argument name="mailbox" />
						<argument name="context" />
					</option>
					<option name="q">
						<para>Don't play a beep when beginning to spy on a channel, or speak the
						selected channel name.</para>
					</option>
					<option name="r">
						<para>Record the session to the monitor spool directory. An optional base for the filename 
						may be specified. The default is <literal>chanspy</literal>.</para>
						<argument name="basename" />
					</option>
					<option name="s">
						<para>Skip the playback of the channel type (i.e. SIP, IAX, etc) when
						speaking the selected channel name.</para>
					</option>
					<option name="v">
						<argument name="value" />
						<para>Adjust the initial volume in the range from <literal>-4</literal> 
						to <literal>4</literal>. A negative value refers to a quieter setting.</para>
					</option>
					<option name="w">
						<para>Enable <literal>whisper</literal> mode, so the spying channel can talk to
						the spied-on channel.</para>
					</option>
					<option name="W">
						<para>Enable <literal>private whisper</literal> mode, so the spying channel can
						talk to the spied-on channel but cannot listen to that channel.</para>
					</option>
					<option name="o">
						<para>Only listen to audio coming from this channel.</para>
					</option>
					<option name="X">
						<para>Allow the user to exit ChanSpy to a valid single digit
						numeric extension in the current context or the context
						specified by the <variable>SPY_EXIT_CONTEXT</variable> channel variable. The
						name of the last channel that was spied on will be stored
						in the <variable>SPY_CHANNEL</variable> variable.</para>
					</option>
					<option name="e">
						<argument name="ext" required="true" />
						<para>Enable <emphasis>enforced</emphasis> mode, so the spying channel can
						only monitor extensions whose name is in the <replaceable>ext</replaceable> : delimited 
						list.</para>
					</option>
				</optionlist>	
			</parameter>
		</syntax>
		<description>
			<para>This application is used to listen to the audio from an Trismedia channel. This includes 
			the audio coming in and out of the channel being spied on. Only channels created by outgoing calls for the
			specified extension will be selected for spying. If the optional context is not supplied, 
			the current channel's context will be used.</para>
			<para>While spying, the following actions may be performed:</para>
			<para> - Dialing <literal>#</literal> cycles the volume level.</para>
                        <para> - Dialing <literal>*</literal> will stop spying and look for another channel to spy on.</para>
			<note><para>The <replaceable>X</replaceable> option supersedes the three features above in that if a valid
			single digit extension exists in the correct context ChanSpy will exit to it.
			This also disables choosing a channel based on <literal>chanprefix</literal> and a digit sequence.</para></note>
		</description>
		<see-also>
			<ref type="application">ChanSpy</ref>
		</see-also>
	</application>

 ***/
static const char *app_chan = "ChanSpy";

static const char *app_ext = "ExtenSpy";

enum {
	OPTION_QUIET             = (1 << 0),    /* Quiet, no announcement */
	OPTION_BRIDGED           = (1 << 1),    /* Only look at bridged calls */
	OPTION_VOLUME            = (1 << 2),    /* Specify initial volume */
	OPTION_GROUP             = (1 << 3),    /* Only look at channels in group */
	OPTION_RECORD            = (1 << 4),
	OPTION_WHISPER           = (1 << 5),
	OPTION_PRIVATE           = (1 << 6),    /* Private Whisper mode */
	OPTION_READONLY          = (1 << 7),    /* Don't mix the two channels */
	OPTION_EXIT              = (1 << 8),    /* Exit to a valid single digit extension */
	OPTION_ENFORCED          = (1 << 9),    /* Enforced mode */
	OPTION_NOTECH            = (1 << 10),   /* Skip technology name playback */
	OPTION_BARGE             = (1 << 11),   /* Barge mode (whisper to both channels) */
	OPTION_NAME              = (1 << 12),   /* Say the name of the person on whom we will spy */
	OPTION_DTMF_SWITCH_MODES = (1 << 13),   /*Allow numeric DTMF to switch between chanspy modes */
	OPTION_OPERATOR			 = (1 << 14),	/* Spying when operator. Don't check listener */
} chanspy_opt_flags;

enum {
	OPT_ARG_VOLUME = 0,
	OPT_ARG_GROUP,
	OPT_ARG_RECORD,
	OPT_ARG_ENFORCED,
	OPT_ARG_NAME,
	OPT_ARG_ARRAY_SIZE,
} chanspy_opt_args;

TRIS_APP_OPTIONS(spy_opts, {
	TRIS_APP_OPTION('q', OPTION_QUIET),
	TRIS_APP_OPTION('b', OPTION_BRIDGED),
	TRIS_APP_OPTION('B', OPTION_BARGE),
	TRIS_APP_OPTION('w', OPTION_WHISPER),
	TRIS_APP_OPTION('W', OPTION_PRIVATE),
	TRIS_APP_OPTION_ARG('v', OPTION_VOLUME, OPT_ARG_VOLUME),
	TRIS_APP_OPTION_ARG('g', OPTION_GROUP, OPT_ARG_GROUP),
	TRIS_APP_OPTION_ARG('r', OPTION_RECORD, OPT_ARG_RECORD),
	TRIS_APP_OPTION_ARG('e', OPTION_ENFORCED, OPT_ARG_ENFORCED),
	TRIS_APP_OPTION('o', OPTION_READONLY),
	TRIS_APP_OPTION('X', OPTION_EXIT),
	TRIS_APP_OPTION('s', OPTION_NOTECH),
	TRIS_APP_OPTION_ARG('n', OPTION_NAME, OPT_ARG_NAME),
	TRIS_APP_OPTION('d', OPTION_DTMF_SWITCH_MODES),
	TRIS_APP_OPTION('O', OPTION_OPERATOR),
});

static int next_unique_id_to_use = 0;

struct chanspy_translation_helper {
	/* spy data */
	struct tris_audiohook spy_audiohook;
	struct tris_audiohook whisper_audiohook;
	struct tris_audiohook bridge_whisper_audiohook;
	int fd;
	int volfactor;
};

static int extenspy_exec(struct tris_channel *chan, void *data);

static void *spy_alloc(struct tris_channel *chan, void *data)
{
	/* just store the data pointer in the channel structure */
	return data;
}

static void spy_release(struct tris_channel *chan, void *data)
{
	/* nothing to do */
}

static int spy_generate(struct tris_channel *chan, void *data, int len, int samples)
{
	struct chanspy_translation_helper *csth = data;
	struct tris_frame *f, *cur;

	tris_audiohook_lock(&csth->spy_audiohook);
	if (csth->spy_audiohook.status != TRIS_AUDIOHOOK_STATUS_RUNNING) {
		/* Channel is already gone more than likely */
		tris_audiohook_unlock(&csth->spy_audiohook);
		return -1;
	}

	if (tris_test_flag(&csth->spy_audiohook, OPTION_READONLY)) {
		/* Option 'o' was set, so don't mix channel audio */
		f = tris_audiohook_read_frame(&csth->spy_audiohook, samples, TRIS_AUDIOHOOK_DIRECTION_READ, TRIS_FORMAT_SLINEAR);
	} else {
		f = tris_audiohook_read_frame(&csth->spy_audiohook, samples, TRIS_AUDIOHOOK_DIRECTION_BOTH, TRIS_FORMAT_SLINEAR);
	}

	tris_audiohook_unlock(&csth->spy_audiohook);

	if (!f)
		return 0;

	for (cur = f; cur; cur = TRIS_LIST_NEXT(cur, frame_list)) {
		if (tris_write(chan, cur)) {
			tris_frfree(f);
			return -1;
		}

		if (csth->fd) {
			if (write(csth->fd, cur->data.ptr, cur->datalen) < 0) {
				tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
			}
		}
	}

	tris_frfree(f);

	return 0;
}

static struct tris_generator spygen = {
	.alloc = spy_alloc,
	.release = spy_release,
	.generate = spy_generate,
};

static int start_spying(struct tris_channel *chan, const char *spychan_name, struct tris_audiohook *audiohook)
{
	int res = 0;
	struct tris_channel *peer = NULL;

	tris_log(LOG_NOTICE, "Attaching %s to %s\n", spychan_name, chan->name);

	tris_set_flag(audiohook, TRIS_AUDIOHOOK_TRIGGER_SYNC | TRIS_AUDIOHOOK_SMALL_QUEUE);
	res = tris_audiohook_attach(chan, audiohook);

	if (!res && tris_test_flag(chan, TRIS_FLAG_NBRIDGE) && (peer = tris_bridged_channel(chan))) { 
		tris_softhangup(peer, TRIS_SOFTHANGUP_UNBRIDGE);
	}
	return res;
}

struct chanspy_ds {
	struct tris_channel *chan;
	char unique_id[20];
	tris_mutex_t lock;
};

static void change_spy_mode(const char digit, struct tris_flags *flags)
{
	if (digit == '4') {
		tris_clear_flag(flags, OPTION_WHISPER);
		tris_clear_flag(flags, OPTION_BARGE);
	} else if (digit == '5') {
		tris_clear_flag(flags, OPTION_BARGE);
		tris_set_flag(flags, OPTION_WHISPER);
	} else if (digit == '6') {
		tris_clear_flag(flags, OPTION_WHISPER);
		tris_set_flag(flags, OPTION_BARGE);
	}
}

static int channel_spy(struct tris_channel *chan, struct chanspy_ds *spyee_chanspy_ds, 
	int *volfactor, int fd, struct tris_flags *flags, char *exitcontext) 
{
	struct chanspy_translation_helper csth;
	int running = 0, res, x = 0;
	char inp[24] = {0};
	char *name;
	struct tris_frame *f;
	struct tris_silence_generator *silgen = NULL;
	struct tris_channel *spyee = NULL, *spyee_bridge = NULL;
	const char *spyer_name;

	tris_channel_lock(chan);
	spyer_name = tris_strdupa(chan->name);
	tris_channel_unlock(chan);

	tris_mutex_lock(&spyee_chanspy_ds->lock);
	while ((spyee = spyee_chanspy_ds->chan) && tris_channel_trylock(spyee)) {
		/* avoid a deadlock here, just in case spyee is masqueraded and
		 * chanspy_ds_chan_fixup() is called with the channel locked */
		DEADLOCK_AVOIDANCE(&spyee_chanspy_ds->lock);
	}
	tris_mutex_unlock(&spyee_chanspy_ds->lock);

	if (!spyee) {
		return 0;
	}

	/* We now hold the channel lock on spyee */

	if (tris_check_hangup(chan) || tris_check_hangup(spyee)) {
		tris_channel_unlock(spyee);
		return 0;
	}

	name = tris_strdupa(spyee->name);

	tris_verb(2, "Spying on channel %s\n", name);
	manager_event(EVENT_FLAG_CALL, "ChanSpyStart",
			"SpyerChannel: %s\r\n"
			"SpyeeChannel: %s\r\n",
			spyer_name, name);

	memset(&csth, 0, sizeof(csth));
	tris_copy_flags(&csth.spy_audiohook, flags, TRIS_FLAGS_ALL);

	tris_audiohook_init(&csth.spy_audiohook, TRIS_AUDIOHOOK_TYPE_SPY, "ChanSpy");

	if (start_spying(spyee, spyer_name, &csth.spy_audiohook)) {
		tris_audiohook_destroy(&csth.spy_audiohook);
		tris_channel_unlock(spyee);
		return 0;
	}
	if (tris_test_flag(flags, OPTION_WHISPER)) {
	 	tris_audiohook_init(&csth.whisper_audiohook, TRIS_AUDIOHOOK_TYPE_WHISPER, "ChanSpy");
		tris_audiohook_init(&csth.bridge_whisper_audiohook, TRIS_AUDIOHOOK_TYPE_WHISPER, "Chanspy");
	  	if (start_spying(spyee, spyer_name, &csth.whisper_audiohook)) {
			tris_log(LOG_WARNING, "Unable to attach whisper audiohook to spyee %s. Whisper mode disabled!\n", spyee->name);
		}
	}
	if ((spyee_bridge = tris_bridged_channel(spyee))) {
		tris_channel_lock(spyee_bridge);
		if (start_spying(spyee_bridge, spyer_name, &csth.bridge_whisper_audiohook)) {
			tris_log(LOG_WARNING, "Unable to attach barge audiohook on spyee %s. Barge mode disabled!\n", spyee->name);
		}
		tris_channel_unlock(spyee_bridge);
	}
	tris_channel_unlock(spyee);
	spyee = NULL;

	tris_channel_lock(chan);
	tris_set_flag(chan, TRIS_FLAG_END_DTMF_ONLY);
	tris_channel_unlock(chan);

	csth.volfactor = *volfactor;

	if (csth.volfactor) {
		csth.spy_audiohook.options.read_volume = csth.volfactor;
		csth.spy_audiohook.options.write_volume = csth.volfactor;
	}

	csth.fd = fd;

	if (tris_test_flag(flags, OPTION_PRIVATE))
		silgen = tris_channel_start_silence_generator(chan);
	else
		tris_activate_generator(chan, &spygen, &csth);

	/* We can no longer rely on 'spyee' being an actual channel;
	   it can be hung up and freed out from under us. However, the
	   channel destructor will put NULL into our csth.spy.chan
	   field when that happens, so that is our signal that the spyee
	   channel has gone away.
	*/

	/* Note: it is very important that the tris_waitfor() be the first
	   condition in this expression, so that if we wait for some period
	   of time before receiving a frame from our spying channel, we check
	   for hangup on the spied-on channel _after_ knowing that a frame
	   has arrived, since the spied-on channel could have gone away while
	   we were waiting
	*/
	while ((res = tris_waitfor(chan, -1) > -1) && csth.spy_audiohook.status == TRIS_AUDIOHOOK_STATUS_RUNNING) {
		if (!(f = tris_read(chan)) || tris_check_hangup(chan)) {
			running = -1;
			break;
		}

		if (tris_test_flag(flags, OPTION_BARGE) && f->frametype == TRIS_FRAME_VOICE) {
			tris_audiohook_lock(&csth.whisper_audiohook);
			tris_audiohook_lock(&csth.bridge_whisper_audiohook);
			tris_audiohook_write_frame(&csth.whisper_audiohook, TRIS_AUDIOHOOK_DIRECTION_WRITE, f);
			tris_audiohook_write_frame(&csth.bridge_whisper_audiohook, TRIS_AUDIOHOOK_DIRECTION_WRITE, f);
			tris_audiohook_unlock(&csth.whisper_audiohook);
			tris_audiohook_unlock(&csth.bridge_whisper_audiohook);
			tris_frfree(f);
			continue;
		} else if (tris_test_flag(flags, OPTION_WHISPER) && f->frametype == TRIS_FRAME_VOICE) {
			tris_audiohook_lock(&csth.whisper_audiohook);
			tris_audiohook_write_frame(&csth.whisper_audiohook, TRIS_AUDIOHOOK_DIRECTION_WRITE, f);
			tris_audiohook_unlock(&csth.whisper_audiohook);
			tris_frfree(f);
			continue;
		}
		
		res = (f->frametype == TRIS_FRAME_DTMF) ? f->subclass : 0;
		tris_frfree(f);
		if (!res)
			continue;

		if (x == sizeof(inp))
			x = 0;

		if (res < 0) {
			running = -1;
			break;
		}

		if (tris_test_flag(flags, OPTION_EXIT)) {
			char tmp[2];
			tmp[0] = res;
			tmp[1] = '\0';
			if (!tris_goto_if_exists(chan, exitcontext, tmp, 1)) {
				tris_debug(1, "Got DTMF %c, goto context %s\n", tmp[0], exitcontext);
				pbx_builtin_setvar_helper(chan, "SPY_CHANNEL", name);
				running = -2;
				break;
			} else {
				tris_debug(2, "Exit by single digit did not work in chanspy. Extension %s does not exist in context %s\n", tmp, exitcontext);
			}
		} else if (res >= '0' && res <= '9') {
			if (tris_test_flag(flags, OPTION_DTMF_SWITCH_MODES)) {
				change_spy_mode(res, flags);
			} else {
				inp[x++] = res;
			}
		}

		if (res == '*') {
			running = 0;
			break;
		} else if (res == '#') {
			/* cool's code begin
			char rno[20];
			tris_verbose("You pressed # key!\n");
			tris_readstring(chan, rno, sizeof(rno)-1, 3000, 7000, "#");
			tris_verbose("You typed <%s>!\n", rno);
			if(!tris_strlen_zero(rno)) {
				char sql[128], announcer[80];
				sprintf(sql, "SELECT announcer FROM broadcast3 WHERE roomno='%s'", rno);
				sql_select_query_execute(announcer, sql);
				if(!tris_strlen_zero(announcer))
					strcpy(mailbox , announcer);
				break;
			}
			cool's code end*/

			
			if (!tris_strlen_zero(inp)) {
				running = atoi(inp);
				break;
			}

			(*volfactor)++;
			if (*volfactor > 4)
				*volfactor = -4;
			tris_verb(3, "Setting spy volume on %s to %d\n", chan->name, *volfactor);

			csth.volfactor = *volfactor;
			csth.spy_audiohook.options.read_volume = csth.volfactor;
			csth.spy_audiohook.options.write_volume = csth.volfactor;
			

		}
	}

	if (tris_test_flag(flags, OPTION_PRIVATE))
		tris_channel_stop_silence_generator(chan, silgen);
	else
		tris_deactivate_generator(chan);

	tris_channel_lock(chan);
	tris_clear_flag(chan, TRIS_FLAG_END_DTMF_ONLY);
	tris_channel_unlock(chan);
	
	if (tris_test_flag(flags, OPTION_WHISPER)) {
		tris_audiohook_lock(&csth.whisper_audiohook);
		tris_audiohook_detach(&csth.whisper_audiohook);
		tris_audiohook_unlock(&csth.whisper_audiohook);
		tris_audiohook_destroy(&csth.whisper_audiohook);
		
		tris_audiohook_lock(&csth.bridge_whisper_audiohook);
		tris_audiohook_detach(&csth.bridge_whisper_audiohook);
		tris_audiohook_unlock(&csth.bridge_whisper_audiohook);
		tris_audiohook_destroy(&csth.bridge_whisper_audiohook);
	}
	
	tris_audiohook_lock(&csth.spy_audiohook);
	tris_audiohook_detach(&csth.spy_audiohook);
	tris_audiohook_unlock(&csth.spy_audiohook);
	tris_audiohook_destroy(&csth.spy_audiohook);
	
	tris_verb(2, "Done Spying on channel %s\n", name);
	manager_event(EVENT_FLAG_CALL, "ChanSpyStop", "SpyeeChannel: %s\r\n", name);

	return running;
}

/*!
 * \note This relies on the embedded lock to be recursive, as it may be called
 * due to a call to chanspy_ds_free with the lock held there.
 */
static void chanspy_ds_destroy(void *data)
{
	struct chanspy_ds *chanspy_ds = data;

	/* Setting chan to be NULL is an atomic operation, but we don't want this
	 * value to change while this lock is held.  The lock is held elsewhere
	 * while it performs non-atomic operations with this channel pointer */

	tris_mutex_lock(&chanspy_ds->lock);
	chanspy_ds->chan = NULL;
	tris_mutex_unlock(&chanspy_ds->lock);
}

static void chanspy_ds_chan_fixup(void *data, struct tris_channel *old_chan, struct tris_channel *new_chan)
{
	struct chanspy_ds *chanspy_ds = data;
	
	tris_mutex_lock(&chanspy_ds->lock);
	chanspy_ds->chan = new_chan;
	tris_mutex_unlock(&chanspy_ds->lock);
}

static const struct tris_datastore_info chanspy_ds_info = {
	.type = "chanspy",
	.destroy = chanspy_ds_destroy,
	.chan_fixup = chanspy_ds_chan_fixup,
};

static struct chanspy_ds *chanspy_ds_free(struct chanspy_ds *chanspy_ds)
{
	struct tris_channel *chan;

	if (!chanspy_ds) {
		return NULL;
	}

	tris_mutex_lock(&chanspy_ds->lock);
	while ((chan = chanspy_ds->chan)) {
		struct tris_datastore *datastore;

		if (tris_channel_trylock(chan)) {
			DEADLOCK_AVOIDANCE(&chanspy_ds->lock);
			continue;
		}
		if ((datastore = tris_channel_datastore_find(chan, &chanspy_ds_info, chanspy_ds->unique_id))) {
			tris_channel_datastore_remove(chan, datastore);
			/* chanspy_ds->chan is NULL after this call */
			chanspy_ds_destroy(datastore->data);
			datastore->data = NULL;
			tris_datastore_free(datastore);
		}
		tris_channel_unlock(chan);
		break;
	}
	tris_mutex_unlock(&chanspy_ds->lock);

	return NULL;
}

/*! \note Returns the channel in the chanspy_ds locked as well as the chanspy_ds locked */
static struct chanspy_ds *setup_chanspy_ds(struct tris_channel *chan, struct chanspy_ds *chanspy_ds)
{
	struct tris_datastore *datastore = NULL;

	tris_mutex_lock(&chanspy_ds->lock);

	if (!(datastore = tris_datastore_alloc(&chanspy_ds_info, chanspy_ds->unique_id))) {
		tris_mutex_unlock(&chanspy_ds->lock);
		chanspy_ds = chanspy_ds_free(chanspy_ds);
		tris_channel_unlock(chan);
		return NULL;
	}
	
	chanspy_ds->chan = chan;
	datastore->data = chanspy_ds;
	tris_channel_datastore_add(chan, datastore);

	return chanspy_ds;
}

static struct chanspy_ds *next_channel(struct tris_channel *chan,
	const struct tris_channel *last, const char *spec,
	const char *exten, const char *context, struct chanspy_ds *chanspy_ds)
{
	struct tris_channel *next;
	const size_t pseudo_len = strlen("DAHDI/pseudo");

redo:
	if (!tris_strlen_zero(spec))
		next = tris_walk_channel_by_name_prefix_locked(last, spec, strlen(spec));
	else if (!tris_strlen_zero(exten))
		next = tris_walk_channel_by_exten_locked(last, exten, context);
	else
		next = tris_channel_walk_locked(last);

	if (!next)
		return NULL;

	if (!strncmp(next->name, "DAHDI/pseudo", pseudo_len)) {
		last = next;
		tris_channel_unlock(next);
		goto redo;
	} else if (next == chan) {
		last = next;
		tris_channel_unlock(next);
		goto redo;
	}
	if (next->spytransferchan) {
		last = next;
		tris_channel_unlock(next);
		goto redo;
	}
	if (tris_device_state(next->name) == TRIS_DEVICE_ONHOLD) {
		last = next;
		tris_channel_unlock(next);
		goto redo;
	}

	return setup_chanspy_ds(next, chanspy_ds);
}

static int check_listener(char *ext, char *roomno)
{
	char sql[256];
	int res;
	SQLHSTMT stmt;
	char rowdata[20], pattern[30];
	SQLLEN indicator;

	if (tris_strlen_zero(roomno)) {
		return 0;
	}
	
	snprintf(sql, sizeof(sql), "SELECT listeneruid FROM broadcast3_listener WHERE roomno = '%s'", roomno);
	struct generic_prepare_struct gps = { .sql = sql, .argc =0, .argv = NULL };

	struct odbc_obj *obj;
	obj = tris_odbc_request_obj(tris_database, 0);

	if (obj) {
		//snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		//snprintf(sql, sizeof(sql), "SELECT %s FROM view%s WHERE tel='%s'", field, pre, tel);//rate_table);
		tris_verbose("%s\n", sql);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			goto yuck;
		}

		while ((res=SQLFetch(stmt)) != SQL_NO_DATA) {
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				goto yuck;
			}
			res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), &indicator);
			if(indicator == SQL_NULL_DATA) {
				rowdata[0] = '\0';
				res = SQL_SUCCESS;
			}
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				goto yuck;
			}
			//tris_verbose(" COOL (^_^) listeneruid = %s, CallerID = %s\n",rowdata, ext);
			sprintf(pattern, "_%s", rowdata);
			if(!tris_strlen_zero(rowdata) && tris_extension_match(pattern, ext)) {
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				tris_verbose(" COOL (^_^) Matched!!! OK!!!\n");
				return 1;
			}
		}
//		if (sscanf(rowdata, "%d", &x) != 1)
//			tris_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);

	} else{
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", tris_database);
	}
yuck:
	
	return 0;

}

static int check_listener_group(char *ext)
{
	char sql[256];
	char result[32];

	snprintf(sql, sizeof(sql), " SELECT u.listenergid FROM broadcast3_listener AS u LEFT JOIN uri AS c ON u.listenergid = c.gid WHERE c.username = '%s'", ext);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;

	return 1;
}

static int check_operator_listener(char *ext, char *roomno)
{
	char sql[256];
	int res;
	SQLHSTMT stmt;
	char rowdata[20], pattern[30];
	SQLLEN indicator;

	if (tris_strlen_zero(roomno)) {
		return 0;
	}
	
	snprintf(sql, sizeof(sql), "SELECT listeneruid FROM queue_listener WHERE roomno = '%s'", roomno);
	struct generic_prepare_struct gps = { .sql = sql, .argc =0, .argv = NULL };

	struct odbc_obj *obj;
	obj = tris_odbc_request_obj(tris_database, 0);

	if (obj) {
		//snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		//snprintf(sql, sizeof(sql), "SELECT %s FROM view%s WHERE tel='%s'", field, pre, tel);//rate_table);
		tris_verbose("%s\n", sql);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			goto yuck;
		}

		while ((res=SQLFetch(stmt)) != SQL_NO_DATA) {
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				goto yuck;
			}
			res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), &indicator);
			if(indicator == SQL_NULL_DATA) {
				rowdata[0] = '\0';
				res = SQL_SUCCESS;
			}
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				goto yuck;
			}
			//tris_verbose(" COOL (^_^) listeneruid = %s, CallerID = %s\n",rowdata, ext);
			sprintf(pattern, "_%s", rowdata);
			if(!tris_strlen_zero(rowdata) && tris_extension_match(pattern, ext)) {
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				tris_verbose(" COOL (^_^) Matched!!! OK!!!\n");
				return 1;
			}
		}
//		if (sscanf(rowdata, "%d", &x) != 1)
//			tris_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);

	} else{
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", tris_database);
	}
yuck:
	
	return 0;

}

static int check_operator_listener_group(char *ext)
{
	char sql[256];
	char result[32];

	snprintf(sql, sizeof(sql), " SELECT u.listenergid FROM queue_listener AS u LEFT JOIN uri AS c ON u.listenergid = c.gid WHERE c.username = '%s'", ext);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;

	return 1;
}

static int common_exec(struct tris_channel *chan, struct tris_flags *flags,
	int volfactor, const int fd, const char *mygroup, const char *myenforced,
	const char *spec, const char *exten, const char *context, const char *mailbox,
	const char *name_context)
{
	char nameprefix[TRIS_NAME_STRLEN];
	char peer_name[TRIS_NAME_STRLEN + 5];
	char exitcontext[TRIS_MAX_CONTEXT] = "";
	signed char zero_volume = 0;
	int waitms;
	int res;
	char *ptr;
	int num;
	int num_spyed_upon = 1;
	struct chanspy_ds chanspy_ds = { 0, };

	if (tris_test_flag(flags, OPTION_EXIT)) {
		const char *c;
		tris_channel_lock(chan);
		if ((c = pbx_builtin_getvar_helper(chan, "SPY_EXIT_CONTEXT"))) {
			tris_copy_string(exitcontext, c, sizeof(exitcontext));
		} else if (!tris_strlen_zero(chan->macrocontext)) {
			tris_copy_string(exitcontext, chan->macrocontext, sizeof(exitcontext));
		} else {
			tris_copy_string(exitcontext, chan->context, sizeof(exitcontext));
		}
		tris_channel_unlock(chan);
	}

	tris_mutex_init(&chanspy_ds.lock);

	snprintf(chanspy_ds.unique_id, sizeof(chanspy_ds.unique_id), "%d", tris_atomic_fetchadd_int(&next_unique_id_to_use, +1));

	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	if (!tris_test_flag(flags, OPTION_OPERATOR) && !check_listener(chan->cid.cid_num, chan->exten) &&
			!check_listener_group(chan->cid.cid_num)) {
		tris_play_and_wait(chan, "spy/pbx-not-found");
		return 0;
	}
	
	tris_set_flag(chan, TRIS_FLAG_SPYING); /* so nobody can spy on us while we are spying */

	waitms = 100;

	for (;;) {
		struct chanspy_ds *peer_chanspy_ds = NULL, *next_chanspy_ds = NULL;
		struct tris_channel *prev = NULL, *peer = NULL;

		if (!tris_test_flag(flags, OPTION_QUIET) && num_spyed_upon) {
			res = tris_streamfile(chan, "beep", chan->language);
			if (!res)
				res = tris_waitstream(chan, "");
			else if (res < 0) {
				tris_clear_flag(chan, TRIS_FLAG_SPYING);
				break;
			}
			if (!tris_strlen_zero(exitcontext)) {
				char tmp[2];
				tmp[0] = res;
				tmp[1] = '\0';
				if (!tris_goto_if_exists(chan, exitcontext, tmp, 1))
					goto exit;
				else
					tris_debug(2, "Exit by single digit did not work in chanspy. Extension %s does not exist in context %s\n", tmp, exitcontext);
			}
		}

		res = tris_waitfordigit(chan, waitms);
		if (res < 0) {
			tris_clear_flag(chan, TRIS_FLAG_SPYING);
			break;
		}
		if (!tris_strlen_zero(exitcontext)) {
			char tmp[2];
			tmp[0] = res;
			tmp[1] = '\0';
			if (!tris_goto_if_exists(chan, exitcontext, tmp, 1))
				goto exit;
			else
				tris_debug(2, "Exit by single digit did not work in chanspy. Extension %s does not exist in context %s\n", tmp, exitcontext);
		}

		
		/* reset for the next loop around, unless overridden later */
		waitms = 100;
		num_spyed_upon = 0;

		for (peer_chanspy_ds = next_channel(chan, prev, spec, exten, context, &chanspy_ds);
		     peer_chanspy_ds;
			 chanspy_ds_free(peer_chanspy_ds), prev = peer,
		     peer_chanspy_ds = next_chanspy_ds ? next_chanspy_ds : 
			 	next_channel(chan, prev, spec, exten, context, &chanspy_ds), next_chanspy_ds = NULL) {
			int igrp = !mygroup;
			int ienf = !myenforced;
			char *s;

			peer = peer_chanspy_ds->chan;

			tris_mutex_unlock(&peer_chanspy_ds->lock);

			if (peer == prev) {
				tris_channel_unlock(peer);
				chanspy_ds_free(peer_chanspy_ds);
				break;
			}

			if (tris_check_hangup(chan)) {
				tris_channel_unlock(peer);
				chanspy_ds_free(peer_chanspy_ds);
				break;
			}

			if (tris_test_flag(flags, OPTION_BRIDGED) && !tris_bridged_channel(peer)) {
				tris_channel_unlock(peer);
				continue;
			}

			if (tris_check_hangup(peer) || tris_test_flag(peer, TRIS_FLAG_SPYING)) {
				tris_channel_unlock(peer);
				continue;
			}

			if (mygroup) {
				int num_groups = 0;
				int num_mygroups = 0;
				char dup_group[512];
				char dup_mygroup[512];
				char *groups[NUM_SPYGROUPS];
				char *mygroups[NUM_SPYGROUPS];
				const char *group;
				int x;
				int y;
				tris_copy_string(dup_mygroup, mygroup, sizeof(dup_mygroup));
				num_mygroups = tris_app_separate_args(dup_mygroup, ':', mygroups,
					ARRAY_LEN(mygroups));

				if ((group = pbx_builtin_getvar_helper(peer, "SPYGROUP"))) {
					tris_copy_string(dup_group, group, sizeof(dup_group));
					num_groups = tris_app_separate_args(dup_group, ':', groups,
						ARRAY_LEN(groups));
				}

				for (y = 0; y < num_mygroups; y++) {
					for (x = 0; x < num_groups; x++) {
						if (!strcmp(mygroups[y], groups[x])) {
							igrp = 1;
							break;
						}
					}
				}
			}

			if (!igrp) {
				tris_channel_unlock(peer);
				continue;
			}

			if (myenforced) {
				char ext[TRIS_CHANNEL_NAME + 3];
				char buffer[512];
				char *end;

				snprintf(buffer, sizeof(buffer) - 1, ":%s:", myenforced);

				tris_copy_string(ext + 1, peer->name, sizeof(ext) - 1);
				if ((end = strchr(ext, '-'))) {
					*end++ = ':';
					*end = '\0';
				}

				ext[0] = ':';

				if (strcasestr(buffer, ext)) {
					ienf = 1;
				}
			}

			if (!ienf) {
				tris_channel_unlock(peer);
				continue;
			}

			strcpy(peer_name, "spy-");
			strncat(peer_name, peer->name, TRIS_NAME_STRLEN - 4 - 1);
			ptr = strchr(peer_name, '/');
			*ptr++ = '\0';
			ptr = strsep(&ptr, "-");

			for (s = peer_name; s < ptr; s++)
				*s = tolower(*s);
			/* We have to unlock the peer channel here to avoid a deadlock.
			 * So, when we need to dereference it again, we have to lock the 
			 * datastore and get the pointer from there to see if the channel 
			 * is still valid. */
			tris_channel_unlock(peer);

			if (!tris_test_flag(flags, OPTION_QUIET)) {
				if (tris_test_flag(flags, OPTION_NAME)) {
					const char *local_context = S_OR(name_context, "default");
					const char *local_mailbox = S_OR(mailbox, ptr);
					res = tris_app_sayname(chan, local_mailbox, local_context);
				}
				if (!tris_test_flag(flags, OPTION_NAME) || res < 0) {
					if (!tris_test_flag(flags, OPTION_NOTECH)) {
						if (tris_fileexists(peer_name, NULL, NULL) != -1) {
							res = tris_streamfile(chan, peer_name, chan->language);
							if (!res) {
								res = tris_waitstream(chan, "");
							}
							if (res) {
								chanspy_ds_free(peer_chanspy_ds);
								break;
							}
						} else {
							res = tris_say_character_str(chan, peer_name, "", chan->language);
						}
					}
					if ((num = atoi(ptr)))
						tris_say_digits(chan, atoi(ptr), "", chan->language);
				}
			}

			res = channel_spy(chan, peer_chanspy_ds, &volfactor, fd, flags, exitcontext);
			num_spyed_upon++;	

			if (res == -1) {
				chanspy_ds_free(peer_chanspy_ds);
				goto exit;
			} else if (res == -2) {
				res = 0;
				chanspy_ds_free(peer_chanspy_ds);
				goto exit;
			} else if (res > 1 && spec) {
				struct tris_channel *next;

				snprintf(nameprefix, TRIS_NAME_STRLEN, "%s/%d", spec, res);

				if ((next = tris_get_channel_by_name_prefix_locked(nameprefix, strlen(nameprefix)))) {
					peer_chanspy_ds = chanspy_ds_free(peer_chanspy_ds);
					next_chanspy_ds = setup_chanspy_ds(next, &chanspy_ds);
				} else {
					/* stay on this channel, if it is still valid */

					tris_mutex_lock(&peer_chanspy_ds->lock);
					if (peer_chanspy_ds->chan) {
						tris_channel_lock(peer_chanspy_ds->chan);
						next_chanspy_ds = peer_chanspy_ds;
						peer_chanspy_ds = NULL;
					} else {
						/* the channel is gone */
						tris_mutex_unlock(&peer_chanspy_ds->lock);
						next_chanspy_ds = NULL;
					}
				}

				peer = NULL;
			}
		}
		if (res == -1 || tris_check_hangup(chan))
			break;
	}
exit:

	tris_clear_flag(chan, TRIS_FLAG_SPYING);

	tris_channel_setoption(chan, TRIS_OPTION_TXGAIN, &zero_volume, sizeof(zero_volume), 0);

	tris_mutex_lock(&chanspy_ds.lock);
	tris_mutex_unlock(&chanspy_ds.lock);
	tris_mutex_destroy(&chanspy_ds.lock);

	return res;
}

static int chanspy_exec(struct tris_channel *chan, void *data)
{
	char *myenforced = NULL;
	char *mygroup = NULL;
	char *recbase = NULL;
	int fd = 0;
	struct tris_flags flags;
	int oldwf = 0;
	int volfactor = 0;
	int res;
	char *mailbox = NULL;
	char *name_context = NULL;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(spec);
		TRIS_APP_ARG(options);
	);
	char *opts[OPT_ARG_ARRAY_SIZE];

	data = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, data);

	if (args.spec && !strcmp(args.spec, "all"))
		args.spec = NULL;

	if (args.options) {
		tris_app_parse_options(spy_opts, &flags, opts, args.options);
		if (tris_test_flag(&flags, OPTION_GROUP))
			mygroup = opts[OPT_ARG_GROUP];

		if (tris_test_flag(&flags, OPTION_RECORD) &&
			!(recbase = opts[OPT_ARG_RECORD]))
			recbase = "chanspy";

		if (tris_test_flag(&flags, OPTION_VOLUME) && opts[OPT_ARG_VOLUME]) {
			int vol;

			if ((sscanf(opts[OPT_ARG_VOLUME], "%30d", &vol) != 1) || (vol > 4) || (vol < -4))
				tris_log(LOG_NOTICE, "Volume factor must be a number between -4 and 4\n");
			else
				volfactor = vol;
		}

		if (tris_test_flag(&flags, OPTION_PRIVATE))
			tris_set_flag(&flags, OPTION_WHISPER);

		if (tris_test_flag(&flags, OPTION_ENFORCED))
			myenforced = opts[OPT_ARG_ENFORCED];
		
		if (tris_test_flag(&flags, OPTION_NAME)) {
			if (!tris_strlen_zero(opts[OPT_ARG_NAME])) {
				char *delimiter;
				if ((delimiter = strchr(opts[OPT_ARG_NAME], '@'))) {
					mailbox = opts[OPT_ARG_NAME];
					*delimiter++ = '\0';
					name_context = delimiter;
				} else {
					mailbox = opts[OPT_ARG_NAME];
				}
			}
		}


	} else
		tris_clear_flag(&flags, TRIS_FLAGS_ALL);

	oldwf = chan->writeformat;
	if (tris_set_write_format(chan, TRIS_FORMAT_SLINEAR) < 0) {
		tris_log(LOG_ERROR, "Could Not Set Write Format.\n");
		return -1;
	}

	if (recbase) {
		char filename[PATH_MAX];

		snprintf(filename, sizeof(filename), "%s/%s.%d.raw", tris_config_TRIS_MONITOR_DIR, recbase, (int) time(NULL));
		if ((fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, TRIS_FILE_MODE)) <= 0) {
			tris_log(LOG_WARNING, "Cannot open '%s' for recording\n", filename);
			fd = 0;
		}
	}

	res = common_exec(chan, &flags, volfactor, fd, mygroup, myenforced, args.spec, NULL, NULL, mailbox, name_context);

	if (fd)
		close(fd);

	if (oldwf && tris_set_write_format(chan, oldwf) < 0)
		tris_log(LOG_ERROR, "Could Not Set Write Format.\n");

	return res;
}

static int extenspy_exec(struct tris_channel *chan, void *data)
{
	char *ptr, *exten = NULL;
	char *mygroup = NULL;
	char *recbase = NULL;
	int fd = 0;
	struct tris_flags flags;
	int oldwf = 0;
	int volfactor = 0;
	int res;
	char *mailbox = NULL;
	char *name_context = NULL;

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(context);
		TRIS_APP_ARG(options);
		TRIS_APP_ARG(roomno);
	);

	data = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, data);
	if (!tris_strlen_zero(args.context) && (ptr = strchr(args.context, '@'))) {
		exten = args.context;
		*ptr++ = '\0';
		args.context = ptr;
	}

	if (tris_strlen_zero(args.context))
		args.context = tris_strdupa(chan->context);

	if (args.options) {
		char *opts[OPT_ARG_ARRAY_SIZE];

		tris_app_parse_options(spy_opts, &flags, opts, args.options);
		if (tris_test_flag(&flags, OPTION_GROUP))
			mygroup = opts[OPT_ARG_GROUP];

		if (tris_test_flag(&flags, OPTION_RECORD) &&
			!(recbase = opts[OPT_ARG_RECORD]))
			recbase = "chanspy";

		if (tris_test_flag(&flags, OPTION_VOLUME) && opts[OPT_ARG_VOLUME]) {
			int vol;

			if ((sscanf(opts[OPT_ARG_VOLUME], "%30d", &vol) != 1) || (vol > 4) || (vol < -4))
				tris_log(LOG_NOTICE, "Volume factor must be a number between -4 and 4\n");
			else
				volfactor = vol;
		}

		if (tris_test_flag(&flags, OPTION_PRIVATE))
			tris_set_flag(&flags, OPTION_WHISPER);

		
		if (tris_test_flag(&flags, OPTION_NAME)) {
			if (!tris_strlen_zero(opts[OPT_ARG_NAME])) {
				char *delimiter;
				if ((delimiter = strchr(opts[OPT_ARG_NAME], '@'))) {
					mailbox = opts[OPT_ARG_NAME];
					*delimiter++ = '\0';
					name_context = delimiter;
				} else {
					mailbox = opts[OPT_ARG_NAME];
				}
			}
		}

	} else
		tris_clear_flag(&flags, TRIS_FLAGS_ALL);

	oldwf = chan->writeformat;
	if (tris_set_write_format(chan, TRIS_FORMAT_SLINEAR) < 0) {
		tris_log(LOG_ERROR, "Could Not Set Write Format.\n");
		return -1;
	}

	if (recbase) {
		char filename[PATH_MAX];

		snprintf(filename, sizeof(filename), "%s/%s.%d.raw", tris_config_TRIS_MONITOR_DIR, recbase, (int) time(NULL));
		if ((fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, TRIS_FILE_MODE)) <= 0) {
			tris_log(LOG_WARNING, "Cannot open '%s' for recording\n", filename);
			fd = 0;
		}
	}
	
	if (tris_test_flag(&flags, OPTION_OPERATOR) && !check_operator_listener(chan->cid.cid_num, args.roomno) &&
			!check_operator_listener_group(chan->cid.cid_num)) {
		tris_play_and_wait(chan, "spy/pbx-not-found");
		return 0;
	}
	
	res = common_exec(chan, &flags, volfactor, fd, mygroup, NULL, NULL, exten, args.context, mailbox, name_context);

	if (fd)
		close(fd);

	if (oldwf && tris_set_write_format(chan, oldwf) < 0)
		tris_log(LOG_ERROR, "Could Not Set Write Format.\n");

	return res;
}

static int unload_module(void)
{
	int res = 0;

	res |= tris_unregister_application(app_chan);
	res |= tris_unregister_application(app_ext);

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= tris_register_application_xml(app_chan, chanspy_exec);
	res |= tris_register_application_xml(app_ext, extenspy_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Listen to the audio of an active channel");
