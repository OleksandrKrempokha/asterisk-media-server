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
 * \brief Wait for Ring Application
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 239713 $")

#include <sys/types.h>
#include <sys/stat.h>

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/astdb.h"		// added by computopia to use TrisDB module 10:33 2010-11-8
#include "trismedia/app.h"		// added by computopia to use PlayGSM module 10:33 2010-11-8
#include "trismedia/causes.h"
#include "trismedia/res_odbc.h"

/*** DOCUMENTATION
	<application name="CallForwardOn" language="en_US">
		<synopsis>
			Set Call Forward with on
		</synopsis>
		<syntax>
			<parameter name="type" required="true">
				<optionlist>
					<option name="0">
						<para>Call Forward Unconditional</para>
					</option>
					<option name="1">
						<para>Call Forward Offline</para>
					</option>
					<option name="2">
						<para>Call Forward Busy</para>
					</option>
					<option name="3">
						<para>Call Forward No Reply</para>
					</option>
				</optionlist>
			</parameter>
			<parameter name="extension" required="true" />
		</syntax>
		<description>
			<para>Set call forward of extension according to <replaceable>type</replaceable>.</para>
		</description>
	</application>
	<application name="CallForwardOff" language="en_US">
		<synopsis>
			Set Call Forward with off
		</synopsis>
		<syntax>
			<parameter name="type" required="true">
				<optionlist>
					<option name="0">
						<para>Call Forward Unconditional</para>
					</option>
					<option name="1">
						<para>Call Forward Offline</para>
					</option>
					<option name="2">
						<para>Call Forward Busy</para>
					</option>
					<option name="3">
						<para>Call Forward No Reply</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Set call forward of extension according to <replaceable>type</replaceable>.</para>
		</description>
	</application>
 ***/

static char *app1 = "RegisterCbOnbusy";

static int register_alarm_exec(struct tris_channel *chan, void *data)
{
	int res = 0, len = 0;
	char fifoname[256], sql[256];
	char *argcopy = NULL;
	FILE *f;
	
	TRIS_DECLARE_APP_ARGS(arglist,
		TRIS_APP_ARG(ext);
	);

	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}

	argcopy = tris_strdupa(data);
	
	TRIS_STANDARD_APP_ARGS(arglist, argcopy);
	
	if (tris_strlen_zero(arglist.ext))
		return -1;
	
	snprintf(fifoname, sizeof(fifoname), "/tmp/trismedia_replyfifo-%s-%s", chan->cid.cid_num, arglist.ext);
	if (mkfifo(fifoname, 0) < 0) {
		tris_log(LOG_ERROR, "Can't make fifo file\n");
		tris_play_and_wait(chan, "voicemail/failed_to_callback");
		return 0;
	}
	
	len = snprintf(sql, sizeof(sql), ":b2blogic.register_callback_onbusy:trismedia_replyfifo-%s-%s\n%s\n%s\n%s\n\n",
			chan->cid.cid_num, arglist.ext, chan->cid.cid_num, arglist.ext, chan->exten);
	res = write2fifo(sql, len);
	f = fopen(fifoname, "r");
	if (!f) {
		tris_log(LOG_ERROR, "Can't open fifo file descriptor\n");
		goto error;
	}
	fgets(sql, sizeof(sql), f);
	if (strstr(sql, "300")) {
		tris_play_and_wait(chan, "voicemail/already_callback");
	} else if (strstr(sql, "400")) {
		tris_play_and_wait(chan, "voicemail/destination_isnot_busy");
	} else if (strstr(sql, "500")) {
		tris_play_and_wait(chan, "voicemail/failed_to_callback");
	} else {
		tris_play_and_wait(chan, "voicemail/success_to_callback");
	}
	fclose(f);
	unlink(fifoname);
	tris_play_and_wait(chan, "goodbye");

	return 0;
error:
	tris_play_and_wait(chan, "voicemail/failed_to_callback");
	unlink(fifoname);
	return 0;
}

static int unload_module(void)
{
	int res = -1;
	
	res = tris_unregister_application(app1);
	
	return res;
}

static int load_module(void)
{
	int res = -1;

	res = tris_register_application_xml(app1, register_alarm_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Set Callforward");
