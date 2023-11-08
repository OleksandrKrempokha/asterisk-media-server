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

static char *app1 = "CallForwardOn";
static char *app2 = "CallForwardOff";

static char *fifo_str = ":callfwd.reload:\n\n";

static int cfon_exec(struct tris_channel *chan, void *data)
{
	char extension[64];
	int type;
	int res = -1;
	char *argcopy = NULL;
	int maxtries = 3;
	int cmd = 0;
	int maxdigits = 255;
	int to = 0;
	char sql[256];
	char uid[256];
	char result[256];
	
	TRIS_DECLARE_APP_ARGS(arglist,
		TRIS_APP_ARG(type);
		TRIS_APP_ARG(extension);
	);
	
	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}

	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE username = '%s'", chan->cid.cid_num);
	sql_select_query_execute(uid, sql);

	if (tris_strlen_zero(uid)) {
		return -1;
	}

	argcopy = tris_strdupa(data);
	
	TRIS_STANDARD_APP_ARGS(arglist, argcopy);
	
	if (tris_strlen_zero(arglist.type))
		return -1;

	if (!strcmp(arglist.type, "0")) {
		type = 0;
	} else if (!strcmp(arglist.type, "1")){
		type = 3;
	} else if (!strcmp(arglist.type, "2")){
		type = 2;
	} else if (!strcmp(arglist.type, "3")){
		type = 1;
	} else if (!strcmp(arglist.type, "4")){
		type = 5;
	} else {
		tris_verbose("Invalid type pramameter.\n");
		return -1;
	}

	while (cmd>=0 && maxtries > 0) {	
		if (tris_strlen_zero(arglist.extension)) {
			cmd = tris_app_getdata(chan, "callforward/callforward-enter-exten", extension, maxdigits, to);
		} else
			sprintf(extension, "%s", arglist.extension);

		snprintf(sql, sizeof(sql), "SELECT fwd_num FROM callfwd WHERE uid = '%s' AND conditions = '%d'", uid, type);
		sql_select_query_execute(result, sql);

		if (!tris_strlen_zero(extension)) {
			if (tris_strlen_zero(result)) {
				snprintf(sql, sizeof(sql), "INSERT INTO callfwd (uid, fwd_num, inv_time, conditions, scheme) VALUES ('%s', '%s', '120', '%d', 'sip')", uid, extension, type);
				sql_select_query_execute(result, sql);
			} else {
				snprintf(sql, sizeof(sql), "UPDATE callfwd SET fwd_num = '%s' where uid = '%s' AND conditions = '%d'", extension, uid, type);
				sql_select_query_execute(result, sql);
			}
		}

		if (res && cmd >= 0) {
			cmd = tris_play_and_wait(chan, "callforward/extension-not-exist");
			if (cmd >= 0 && (!tris_strlen_zero(arglist.extension) || maxtries == 1)) {
				cmd = tris_play_and_wait(chan, "goodbye");
				tris_verbose("Unable set callforward of %s on.\n", chan->cid.cid_num); 
				return -1;
			} else {
				maxtries--;
			}
		} else {
			break;
		}
	}

	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

	// play callforward/callforward-set-ok
	if (cmd >= 0)
		res = tris_play_and_wait(chan, "callforward/callforward-set-ok");

	return 0;
}

static int cfoff_exec(struct tris_channel *chan, void *data)
{
	int type;
	int res = 0;
	char *argcopy = NULL;
	char sql[256];
	char uid[256];
	char result[256];
	
	if (!chan->cid.cid_num)	
		return -1;
	
	TRIS_DECLARE_APP_ARGS(arglist,
		TRIS_APP_ARG(type);
	);
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}

	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE username = '%s'", chan->cid.cid_num);
	sql_select_query_execute(uid, sql);

	if (tris_strlen_zero(uid)) {
		return -1;
	}

	argcopy = tris_strdupa(data);
	
	TRIS_STANDARD_APP_ARGS(arglist, argcopy);
	
	if (tris_strlen_zero(arglist.type))
		return -1;
	
	if (!strcmp(arglist.type, "0")) {
		type = 0;
	} else if (!strcmp(arglist.type, "1")){
		type = 3;
	} else if (!strcmp(arglist.type, "2")){
		type = 2;
	} else if (!strcmp(arglist.type, "3")){
		type = 1;
	} else if (!strcmp(arglist.type, "4")){
		type = 5;
	} else {
		tris_verbose("Invalid type pramameter.\n");
		return -1;
	}
	
	snprintf(sql, sizeof(sql), "DELETE FROM callfwd WHERE uid = '%s' AND conditions = '%d'", uid, type);
	sql_select_query_execute(result, sql);
	if (res){
		tris_verbose("Unable set callforward of %s %s.\n", chan->cid.cid_num, "off" ); 
		return -1;
	}
	
	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

	// play callforward/callforward_unset_ok
	res = tris_play_and_wait( chan, "callforward/callforward-unset-ok" ); 
	
	return 0;
}

static int unload_module(void)
{
	int res = -1;
	
	res = tris_unregister_application(app1);
	res |= tris_unregister_application(app2);
	
	return res;
}

static int load_module(void)
{
	int res = -1;

	res = tris_register_application_xml(app1, cfon_exec);
	res |= tris_register_application_xml(app2, cfoff_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Set Callforward");
