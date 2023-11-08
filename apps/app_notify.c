/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Trivial application to playback a sound file
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 220292 $")

#include "trismedia/file.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
/* This file provides config-file based 'say' functions, and implenents
 * some CLI commands.
 */
#include "trismedia/say.h"	/* provides config-file based 'say' functions */
#include "trismedia/cli.h"

/*** DOCUMENTATION
	<application name="Playback" language="en_US">
		<synopsis>
			Play a file.
		</synopsis>
		<syntax>
			<parameter name="filenames" required="true" argsep="&amp;">
				<argument name="filename" required="true" />
				<argument name="filename2" multiple="true" />
			</parameter>
			<parameter name="options">
				<para>Comma separated list of options</para>
				<optionlist>
					<option name="skip">
						<para>Do not play if not answered</para>
					</option>
					<option name="noanswer">
						<para>Playback without answering, otherwise the channel will
						be answered before the sound is played.</para>
						<note><para>Not all channel types support playing messages while still on hook.</para></note>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Plays back given filenames (do not put extension of wav/alaw etc).
			The playback command answer the channel if no options are specified.
			If the file is non-existant it will fail</para>
			<para>This application sets the following channel variable upon completion:</para>
			<variablelist>
				<variable name="PLAYBACKSTATUS">
					<para>The status of the playback attempt as a text string.</para>
					<value name="SUCCESS"/>
					<value name="FAILED"/>
				</variable>
			</variablelist>
			<para>See Also: Background (application) -- for playing sound files that are interruptible</para>
			<para>WaitExten (application) -- wait for digits from caller, optionally play music on hold</para>
		</description>
	</application>
 ***/

static char *app = "ListenTime";
static char *app2 = "ListenCallerID";


static int listen_time_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	time_t t;

	t = time(0);
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}

	tris_play_and_wait(chan, "notify/current-time-is");
	tris_say_date_with_format(chan, t, NULL, chan->language, "HM", NULL);
	tris_play_and_wait(chan, "notify/time-is");
	
	return res;
}


static int listen_callerid_exec(struct tris_channel *chan, void *data)
{
	int res = 0;

	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}

	tris_play_and_wait(chan, "notify/your-callerid-is");
	tris_say_digit_str(chan, chan->cid.cid_num, NULL, chan->language);
	tris_play_and_wait(chan, "notify/callerid-is");
	
	return res;
}

static int reload(void)
{
	return 0;
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app);
	res |= tris_unregister_application(app);

	return res;	
}

static int load_module(void)
{
	int res = 0;
	res = tris_register_application_xml(app, listen_time_exec);
	res |= tris_register_application_xml(app2, listen_callerid_exec);
	return res;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Notify Time and CallerID Application",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
