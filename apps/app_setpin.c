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
 * \brief Set Pin of Extension
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
	<application name="SetPin" language="en_US">
		<synopsis>
			DND Deactivated Application
		</synopsis>
		<syntax>
			<parameter name="timeout" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>0</literal> after waiting at least <replaceable>timeout</replaceable> seconds,
			and only after the next ring has completed. Returns <literal>0</literal> on success or
			<literal>-1</literal> on hangup.</para>
		</description>
	</application>
 ***/

static char *app = "SetPin";

static int verify_oldpin(struct tris_channel *chan, char *filename, char *read_data, char *uid)
{
	int res = 0;
	int maxdigits = 255;
	int tries = 3, to = 0;
	char sql[256];
	char pin[256];

	snprintf(sql, sizeof(sql), "SELECT pin FROM credentials WHERE uid = '%s'", uid);
	sql_select_query_execute(pin, sql);

	if (tris_strlen_zero(pin)) {
		return 1;
	}
	while (tries && !res) {
		res = tris_app_getdata(chan, filename, read_data, maxdigits, to);
		
		if (res > -1) {
			if (!tris_strlen_zero(read_data) && !strcmp(read_data, pin)) {
				tris_verbose("User entered '%s'\n", read_data);
				res = 1;
				break;
			} else {
				res = tris_streamfile(chan, "pin/pin-entered-wrong-pin", chan->language);
				if (!res) tris_waitstream(chan, "");
				tries--;
				if (tries)
					tris_verbose("User entered nothing or invalid pin, %d chance%s left\n", tries, (tries != 1) ? "s" : "");
				else
					tris_verbose("User entered nothing or invalid pin.\n");
			}
			res = 0;
		}
	}
	
	return res;
}

static int set_newpin(struct tris_channel *chan, char *filename, char *read_data, char *uid)
{
	int res = 0;
	int maxdigits = 255;
	int to = 0;
	char sql[256];
	char result[256];

	res = tris_app_getdata(chan, filename, read_data, maxdigits, to);
		
	if (!tris_strlen_zero(read_data)) {
		tris_verbose("User entered '%s'\n", read_data);
	} else {
		tris_verbose("User entered nothing.\n");
	}

	if (!tris_strlen_zero(read_data)) {
		snprintf(sql, sizeof(sql), "UPDATE credentials SET pin = '%s' WHERE uid = '%s'", read_data, uid);
		sql_select_query_execute(result, sql);
	}

	return res;
}

static int setpin_exec(struct tris_channel *chan, void *data)
{
	char oldpin[64];
	char newpin[64];
	int res = -1;
	char sql[256];
	char uid[256];
	
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

	if (!verify_oldpin(chan, "pin/pin-enter-old-pin", oldpin, uid)) {
		tris_verbose("Verification failed\n");
		return -1;
	}

	if (set_newpin(chan, "pin/pin-enter-new-pin", newpin, uid) < 0) {
		tris_verbose("Failed to set new pin\n");
		return -1;
	}
	res = tris_streamfile(chan, "pin/pin-new-pin-set-success", chan->language);
	if (!res) tris_waitstream(chan, "");
	tris_verbose("Success to set new pin\n");

	return 0;
	
}

static int unload_module(void)
{
	int res = -1;
	res = tris_unregister_application(app);
	
	return res;
}

static int load_module(void)
{
	int res = -1;
	res = tris_register_application_xml(app, setpin_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Set Pin");

