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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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
#include "trismedia/paths.h"

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

static char *app = "Greeting";

static int record_greeting(struct tris_channel *chan, char *recordfile, char *oldfile, char *newfile)
{
	int cmd = '3';
	int res = 0;
	int duration = 0;
	int tries = 0;
	
	while (cmd != '*' && cmd >= 0 && !res && tries < 5) {
		switch (cmd) {
		case '1':
			cmd = tris_play_and_wait(chan, recordfile);
			cmd = 'm';
			break;
		case '2':
			res = rename(oldfile, newfile);
			if (res < 0) {
				tris_log(LOG_ERROR, "Can't rename file: %s, %s\n", oldfile, newfile);
				return -1;
			}
			cmd = tris_play_and_wait(chan, "voicemail/selected_recorded_greeting");
			return 1;
		case '3':
			cmd = tris_play_and_wait(chan, "voicemail/record_greeting");
			cmd = tris_play_and_record(chan, NULL, recordfile, 0, "wav", &duration, 256, 0, NULL);
			cmd = 'm';
			break;
		case 'm':
			cmd = tris_play_and_wait(chan, "voicemail/greeting_record_options");
			if (!cmd)
				cmd = tris_waitfordigit(chan, 5000);
			break;
			tries++;
		case '*':
			return 0;
		default:
			if (cmd)
				cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			if (!cmd)
				cmd = 'm';
			tries++;
			break;
		}
	}

	if (cmd < 0)
		return cmd;
	
	return 0;
}

static int use_user_greeting(struct tris_channel *chan, char *oldfile, char *newfile, char *recordfile, char *recordfile2)
{
	int tries = 5;
	int cmd = 0;
	int res = 0;
	char *playfile = recordfile2;
	struct stat st;
	
	res = stat(oldfile, &st);
	if (res < 0) {
		res = stat(newfile, &st);
		if (res < 0) {
			res = tris_play_and_wait(chan, "voicemail/no_recorded_greeting");
			return 0;
		}
		playfile = recordfile;
	}
	while (tries >= 0 && cmd != '*' && cmd >= 0 && !res) {
		if (!cmd)
			cmd = tris_play_and_wait(chan, "voicemail/listen_old_greeting");
		if (!cmd)
			cmd = tris_waitfordigit(chan, 5000);
		switch (cmd) {
		case '1':
			cmd = tris_play_and_wait(chan, playfile);
			break;
		case '2':
			if (playfile != recordfile2) {
				res = rename(newfile, oldfile);
				if (res < 0) {
					tris_log(LOG_ERROR, "Can't rename file: %s, %s\n", newfile, oldfile);
					return -1;
				}
			}
			cmd = tris_play_and_wait(chan, "voicemail/selected_recorded_greeting");
			return 1;
		case '*':
			return 0;
		default:
			if (cmd)
				cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		}
		tries--;
	}

	if (cmd < 0)
		return cmd;
	return 0;
}

static int greeting_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char sql[256];
	char uid[256];
	int tries = 3;
	int cmd = 0;
	char oldfile[256], newfile[256];
	char recordfile[256], recordfile2[256];
	struct stat st;
	
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

	snprintf(oldfile, sizeof(oldfile), "%s/voicemail/default/%s", tris_config_TRIS_SPOOL_DIR, uid);
	if (tris_mkdir(oldfile, 0755) < 0) {
		tris_log(LOG_ERROR, "Can't create directory\n");
		return -1;
	}
	snprintf(oldfile, sizeof(oldfile), "%s/voicemail/default/%s/greeting_y.wav", tris_config_TRIS_SPOOL_DIR, uid);
	snprintf(newfile, sizeof(newfile), "%s/voicemail/default/%s/greeting_n.wav", tris_config_TRIS_SPOOL_DIR, uid);
	snprintf(recordfile, sizeof(recordfile), "%s/voicemail/default/%s/greeting_n", tris_config_TRIS_SPOOL_DIR, uid);
	snprintf(recordfile2, sizeof(recordfile2), "%s/voicemail/default/%s/greeting_y", tris_config_TRIS_SPOOL_DIR, uid);

	while (tries && cmd != '*' && cmd >= 0 && !res) {
		if (!cmd)
			cmd = tris_play_and_wait(chan, "voicemail/greeting_menu");
		if (!cmd)
			cmd = tris_waitfordigit(chan, 5000);
		switch (cmd) {
		case '1':
			res = stat(oldfile, &st);
			if (!res) {
				res = rename(oldfile, newfile);
				if (res < 0) {
					tris_log(LOG_ERROR, "Can't rename file: %s, %s\n", oldfile, newfile);
					goto end;
				}
			} else {
				res = 0;
			}
			cmd = tris_play_and_wait(chan, "voicemail/selected_default_greeting");
			goto end;
		case '2':
			res = use_user_greeting(chan, oldfile, newfile, recordfile, recordfile2);
			cmd = 0;
			break;
		case '3':
			res = record_greeting(chan, recordfile, newfile, oldfile);
			cmd = 0;
			break;
		case '*':
			goto end;
		default:
			if (cmd)
				cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		}
		tries--;
	}

end:
	if (cmd >= 0 && res >= 0)
		cmd = tris_play_and_wait(chan, "goodbye");

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
	res = tris_register_application_xml(app, greeting_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Set Pin");

