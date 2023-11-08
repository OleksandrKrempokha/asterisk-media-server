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
#include "trismedia/say.h"
#include "trismedia/res_odbc.h"

/*** DOCUMENTATION
	<application name="AddBlacklist" language="en_US">
		<synopsis>
			Add the number into the blacklist
		</synopsis>
		<syntax>
		</syntax>
		<description>
			<para>Returns <literal>0</literal> after waiting at least <replaceable>timeout</replaceable> seconds,
			and only after the next ring has completed. Returns <literal>0</literal> on success or
			<literal>-1</literal> on hangup.</para>
		</description>
	</application>
	<application name="RemoveBlacklist" language="en_US">
		<synopsis>
			Remove the number into the blacklist
		</synopsis>
		<syntax>
		</syntax>
		<description>
			<para>Returns <literal>0</literal> after waiting at least <replaceable>timeout</replaceable> seconds,
			and only after the next ring has completed. Returns <literal>0</literal> on success or
			<literal>-1</literal> on hangup.</para>
		</description>
	</application>
 ***/

static char *app1 = "AddBlacklist";
static char *app2 = "RemoveBlacklist";

static char *fifo_str = ":permit.reloadPermission:\n\n";

static int read_blacklist(struct tris_channel *chan, char *filename, char *read_data){
	int res = 0;
	int maxdigits = 255;
	int tries = 3, to = 0;
	
	if (!res) {
		while (tries && !res) {
			res = tris_app_getdata(chan, filename, read_data, maxdigits, to);
			
			if (res > -1) {
				if (!tris_strlen_zero(read_data)) {
					tris_verbose("User entered '%s'\n", read_data);
					tries = 0;
					res = 1;
					break;
				} else {
					tris_play_and_wait(chan, "blacklist/extension-not-exist");
					tries--;
					if (tries)
						tris_verbose("User entered nothing, %d chance%s left\n", tries, (tries != 1) ? "s" : "");
					else
						tris_verbose("User entered nothing.\n");
				}
				res = 0;
			}
			
		}
	}
	
	return res;
}

static int addblacklist_exec(struct tris_channel *chan, void *data)
{
	char read_data[256] = "";
	int res = 0;
	char sql[256];
	char uid[256];
	char result[256];

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

	if (tris_strlen_zero(data)) {
		res = read_blacklist(chan, "blacklist/enter-phone-number-to-block", read_data);
		if (!res) {
			tris_verbose("There's no such an extension\n");
			tris_play_and_wait(chan, "goodbye");
			return res;
		}
	} else {
		strcpy(read_data, data);
	}

	if (!tris_strlen_zero(read_data)) {
		snprintf(sql, sizeof(sql), "SELECT caller_pattern FROM call_permit WHERE callee_uid = '%s' and permit = '0'", uid);
		sql_select_query_execute(result, sql);
		if (tris_strlen_zero(result) || strcmp(result, read_data)) {
			snprintf(sql, sizeof(sql), "INSERT INTO call_permit (caller_pattern, callee_uid, permit) VALUES ('%s', '%s', '0')", read_data, uid);
			sql_select_query_execute(result, sql);
		} else {
			snprintf(sql, sizeof(sql), "UPDATE call_permit SET caller_pattern = '%s' WHERE callee_uid = '%s' and permit = '0'", read_data, uid);
			sql_select_query_execute(result, sql);
		}
	}

	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

	tris_say_digit_str(chan, read_data, TRIS_DIGIT_ANY, chan->language);
	tris_play_and_wait(chan,"blacklist/is-set-in-blacklist");

	tris_play_and_wait(chan, "goodbye");
	return 0;
}

static int removeblacklist_exec(struct tris_channel *chan, void *data)
{
	char read_data[256] = "";
	int res = 0;
	char sql[256];
	char uid[256];
	char result[256];

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

	if (tris_strlen_zero(data)) {
		res = read_blacklist(chan, "blacklist/enter-phone-number-to-delete-from-blacklist", read_data);
		if (!res) {
			tris_verbose("There's no such an extension\n");
			tris_play_and_wait(chan, "goodbye");
			return res;
		}
	} else {
		strcpy(read_data, data);
	}

	snprintf(sql, sizeof(sql), "DELETE FROM call_permit WHERE caller_pattern = '%s' AND callee_uid = '%s' AND permit = '0'", read_data, uid);
	sql_select_query_execute(result, sql);

	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

	tris_say_digit_str(chan, read_data, TRIS_DIGIT_ANY, chan->language);
	tris_play_and_wait(chan,"blacklist/is-free-from-blacklist");

	tris_play_and_wait(chan, "goodbye");
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
	
	
	res = tris_register_application_xml(app1, addblacklist_exec);
	res |= tris_register_application_xml(app2, removeblacklist_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Set Blacklist");
