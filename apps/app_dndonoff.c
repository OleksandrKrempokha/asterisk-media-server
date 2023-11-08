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
	<application name="DNDon" language="en_US">
		<synopsis>
			DND Activated Application
		</synopsis>
		<syntax>
		</syntax>
		<description>
			<para>DND(Do Not Disturb) is the function that ignore all the calls to me.
			DNDon activates DND(Do Not Disturb).</para>
		</description>
	</application>
	<application name="DNDoff" language="en_US">
		<synopsis>
			DND Deactivated Application
		</synopsis>
		<syntax>
		</syntax>
		<description>
			<para>DND(Do Not Disturb) is the function that ignore all the calls to me.
			DNDoff deactivates DND(Do Not Disturb).</para>
		</description>
	</application>
 ***/

static char *app1 = "DNDon";
static char *app2 = "DNDoff";
const char *fifo_str = ":router.reloadUserinfo:\n\n";

static int dndon_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char sql[256];
	char uid[256];
	char result[256];
	
	if (!chan->cid.cid_num)	
		return -1;
	
	// answer channel
	res = tris_answer(chan);
	if ( res ){
		tris_log(LOG_WARNING, "tris_answer failed: chan_name:%s\n", chan->cid.cid_name);
		return res;
	} 

	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE username = '%s'", chan->cid.cid_num);
	sql_select_query_execute(uid, sql);

	if (tris_strlen_zero(uid)) {
		return -1;
	}

	snprintf(sql, sizeof(sql), "UPDATE user_info SET DND = '1' WHERE uid = '%s'", uid);
	sql_select_query_execute(result, sql);

	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

	// play on_sound
	res = tris_streamfile(chan, "dnd/you-set-do-not-disturb", 0); 
	if (!res) tris_waitstream(chan, "");
	
	return res;
}

static int dndoff_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char sql[256];
	char uid[256];
	char result[256];
	
	if (!chan->cid.cid_num)	
		return -1;
	
	// answer channel
	res = tris_answer(chan);
	if ( res ){
		tris_log(LOG_WARNING, "tris_answer failed: chan_name:%s\n", chan->cid.cid_name);
		return res;
	} 

	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE username = '%s'", chan->cid.cid_num);
	sql_select_query_execute(uid, sql);

	if (tris_strlen_zero(uid)) {
		return -1;
	}

	snprintf(sql, sizeof(sql), "UPDATE user_info SET DND = '0' WHERE uid = '%s'", uid);
	sql_select_query_execute(result, sql);

	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

	// play on_sound
	res = tris_streamfile(chan, "dnd/you-unset-do-not-disturb", 0); 
	if (!res) tris_waitstream(chan, "");
	
	return res;
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

	res = tris_register_application_xml(app1, dndon_exec);
	res |= tris_register_application_xml(app2, dndoff_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Do Not Disturb");
