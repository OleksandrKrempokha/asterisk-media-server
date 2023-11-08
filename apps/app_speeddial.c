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
 * \brief SpeedDial Application
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
#include "trismedia/say.h"		// added by computopia to use tris_say_digits() function 16:36 2010/11/11
#include "trismedia/causes.h"
#include "trismedia/cli.h"
#include "trismedia/res_odbc.h"

/*** DOCUMENTATION
	<application name="SetSpeeddial" language="en_US">
		<synopsis>
			Set,Change,Delete Speed Dial Number Application
		</synopsis>
		<syntax>
			<parameter name="key" required="true" />
			<parameter name="extension" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>0</literal> after waiting at least <replaceable>timeout</replaceable> seconds,
			and only after the next ring has completed. Returns <literal>0</literal> on success or
			<literal>-1</literal> on hangup.</para>
		</description>
	</application>
	<application name="UnsetSpeeddial" language="en_US">
		<synopsis>
			Unset Dial Number Application
		</synopsis>
		<syntax>
			<parameter name="key" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>0</literal> after waiting at least <replaceable>timeout</replaceable> seconds,
			and only after the next ring has completed. Returns <literal>0</literal> on success or
			<literal>-1</literal> on hangup.</para>
		</description>
	</application>
	<application name="Speeddial" language="en_US">
		<synopsis>
			Call incorrect Speeddial Context Application
		</synopsis>
		<syntax>
			<parameter name="key" required="true" />
			<parameter name="context" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>0</literal> after waiting at least <replaceable>timeout</replaceable> seconds,
			and only after the next ring has completed. Returns <literal>0</literal> on success or
			<literal>-1</literal> on hangup.</para>
		</description>
	</application>
 ***/

static char *app1 = "SetSpeeddial";
static char *app2 = "UnsetSpeeddial";

static char *fifo_str = ":sd.reload:\n\n";

/**
* Read Speed Number.
* @return success: num, fail: 0
*/
static int ReadSpeedNumber(struct tris_channel *chan, char *num, char* filename) {
	int res = -1;
	char tmp[256] = "";
	int maxdigits = 255;
	int to = 0;
	
	tris_stopstream(chan);
	res = tris_app_getdata(chan, filename, tmp, maxdigits, to);
	
	if (res > -1) {
		if (!tris_strlen_zero(tmp)) {
			tris_verb(3, "User entered '%s'\n", tmp);
		} else {
			tris_verb(3, "User entered nothing.\n");
		}
	} else {
		tris_verb(3, "User disconnected\n");
	}
	
	sprintf(num, "%s", tmp);
	
	return -1;
}

static int setspeeddial_exec(struct tris_channel *chan, void *data)
{
	char key[256] = "";
	char extension[256] = "";
	char *parse;
	char *args_key=NULL, *args_extension=NULL;
	int max_attempts = 3;
	int attempts = 0;
	int cmd = 0;
	int res = 0;
	char sql[256];
	char uid[256];
	char result[256];

	if (!chan->cid.cid_num)	
		return -1;
	
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(key_extension);
	);
	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}

	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE username = '%s'", chan->cid.cid_num);
	sql_select_query_execute(uid, sql);

	if (tris_strlen_zero(uid)) {
		return -1;
	}

	if (!tris_strlen_zero(args.key_extension)) {
		args_extension = tris_strdupa(args.key_extension);
		args_key = strsep(&args_extension, "*");
	}

	
	if (tris_strlen_zero(args_key) || tris_strlen_zero(args_extension)) {
		cmd = 'p';
		while ((cmd >= 0) && (cmd != 't')) {
			switch (cmd) {
			case '1':
				/* set speeddial */
				// read key
				res = ReadSpeedNumber(chan, key, "speeddial/sp-enter");
				if (!res) {
					tris_log(LOG_WARNING, "read key on %s\n", chan->name);
					return -1;
				}
				// read extension
				res = ReadSpeedNumber(chan, extension, "speeddial/sp-enter-exten");
				if (!res) {
					tris_log(LOG_WARNING, "read exten on %s\n", chan->name);
					return -1;
				}
				// check key and extension
				if (tris_strlen_zero(key) || tris_strlen_zero(extension)) {
					cmd = 'p';
				} else {
					snprintf(sql, sizeof(sql), "SELECT d_username FROM speed_dial WHERE uid = '%s' AND s_username = '%s'", uid, key);
					sql_select_query_execute(result, sql);

					if (!tris_strlen_zero(extension)) {
						if (tris_strlen_zero(result)) {
							snprintf(sql, sizeof(sql), "INSERT INTO speed_dial (sid, uid, s_username, d_username, d_domain, scheme) VALUES (NULL, '%s', '%s', '%s', '', 'sip')", uid, key, extension);
							sql_select_query_execute(result, sql);
						} else {
							snprintf(sql, sizeof(sql), "UPDATE speed_dial SET d_username = '%s' where uid = '%s' AND s_username = '%s'", extension, uid, key);
							sql_select_query_execute(result, sql);
						}
					}
					// play sp-set-ok
					res = tris_streamfile(chan, "speeddial/sp-set-ok", NULL);
					if (!res) { 
						tris_waitstream(chan, "");
						tris_stopstream(chan);
					} else {
						tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
						return -1;
					}
				}
				cmd = 'p';
				break;
			case '2':
				/* unset speeddial */
				// read key
				res = ReadSpeedNumber(chan, key, "speeddial/sp-enter");
				if (!res) {
					tris_log(LOG_WARNING, "read key on %s\n", chan->name);
					return -1;
				}
				snprintf(sql, sizeof(sql), "SELECT d_username FROM speed_dial WHERE uid = '%s' AND s_username = '%s'", uid, key);
				sql_select_query_execute(result, sql);
				
				if (!tris_strlen_zero(key) && tris_strlen_zero(result)) {
					res = tris_play_and_wait(chan, "speeddial/sp-no-exten");
					if (res) {
						tris_log(LOG_WARNING, "playing sp-no-exten failed\n");
						return -1;
					}
					cmd = 'p';
					break;
				}

				res = 0;
				if (!tris_strlen_zero(key)) {
					snprintf(sql, sizeof(sql), "DELETE FROM speed_dial WHERE uid = '%s' AND s_username = '%s'", uid, key);
					sql_select_query_execute(result, sql);
				} else {
					snprintf(sql, sizeof(sql), "DELETE FROM speed_dial WHERE uid = '%s'", uid);
					sql_select_query_execute(result, sql);
				}
				// play sp-unset-ok
				res = tris_streamfile(chan, "speeddial/sp-unset-ok", NULL);
				if (!res) { 
					tris_waitstream(chan, "");
					tris_stopstream(chan);
				} else {
					tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
					return -1;
				}
				cmd = 'p';
				break;
			case '3':
				/* read */
				// read key
				res = ReadSpeedNumber(chan, key, "speeddial/sp-enter");
				if (!res) {
					tris_log(LOG_WARNING, "read key on %s\n", chan->name);
					return -1;
				}
				res = 0;
				snprintf(sql, sizeof(sql), "SELECT d_username FROM speed_dial WHERE uid = '%s' AND s_username = '%s'", uid, key);
				sql_select_query_execute(result, sql);
				
				if (!tris_strlen_zero(result)) {
					tris_play_and_wait(chan, "speeddial/sp-exten-num-is");
					tris_say_digit_str(chan, result, TRIS_DIGIT_ANY, chan->language);
					tris_play_and_wait(chan, "speeddial/sp-is");
				} else {
					// play sp-no-exten
					res = tris_streamfile(chan, "speeddial/sp-no-exten", NULL);
					if (!res) { 
						tris_waitstream(chan, "");
						tris_stopstream(chan);
					} else {
						tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
						return -1;
					}
				}
				cmd = 'p';
				break;
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			case '0':
			case '#':
				cmd = tris_play_and_wait(chan, "speeddial/sp-sorry");
				cmd = 'p';
				break;
			case '*':
				// play goodbye
				res = tris_streamfile(chan, "goodbye", NULL);
				if (!res) { 
					tris_waitstream(chan, "");
					tris_stopstream(chan);
				} else {
					tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
					return -1;
				}
				cmd = 't';
				break;
			default:
				cmd = tris_play_and_wait(chan, "speeddial/sp-menu");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 6000);
				if (!cmd) {
					attempts++;
				}
				if (attempts > max_attempts) {
					cmd = 't';
				}
				break;
			}
		}
	} else {
		snprintf(sql, sizeof(sql), "SELECT d_username FROM speed_dial WHERE uid = '%s' AND s_username = '%s'", uid, args_key);
		sql_select_query_execute(result, sql);

		if (!tris_strlen_zero(args_extension)) {
			if (tris_strlen_zero(result)) {
				snprintf(sql, sizeof(sql), "INSERT INTO speed_dial (sid, uid, s_username, d_username, d_domain, scheme) VALUES (NULL, '%s', '%s', '%s', '', 'sip')", uid, args_key, args_extension);
				sql_select_query_execute(result, sql);
			} else {
				snprintf(sql, sizeof(sql), "UPDATE speed_dial SET d_username = '%s' where uid = '%s' AND s_username = '%s'", args_extension, uid, args_key);
				sql_select_query_execute(result, sql);
			}
		}

		if (!res) {
			// play sp-set-ok
			res = tris_streamfile(chan, "speeddial/sp-set-ok", NULL);
			if (!res) { 
				tris_waitstream(chan, "");
				tris_stopstream(chan);
			} else {
				tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
				return -1;
			}
		} else {
			// play sp-set-failed
			res = tris_streamfile(chan, "speeddial/sp-set-failed", NULL);
			if (!res) { 
				tris_waitstream(chan, "");
				tris_stopstream(chan);
			} else {
				tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
				return -1;
			}
		}
	}

	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

	return 0;
}

static int unsetspeeddial_exec(struct tris_channel *chan, void *data)
{
	char *parse;
	int res = 0;
	char sql[256];
	char uid[256];
	char result[256];

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(key);
	);
	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

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

	tris_verbose("key : %s\n", args.key);
	
	snprintf(sql, sizeof(sql), "SELECT d_username FROM speed_dial WHERE uid = '%s' AND s_username = '%s'", uid, args.key);
	sql_select_query_execute(result, sql);
	
	if (!tris_strlen_zero(args.key) && tris_strlen_zero(result)) {
		res = tris_play_and_wait(chan, "speeddial/sp-no-exten");
		if (res) {
			tris_log(LOG_WARNING, "playing sp-no-exten failed\n");
			return -1;
		}
		return 0;
	}
	
	/* unset speeddial */
	// play sp-enter
	res = 0;
	if (!tris_strlen_zero(args.key)) {
		snprintf(sql, sizeof(sql), "DELETE FROM speed_dial WHERE uid = '%s' AND s_username = '%s'", uid, args.key);
		sql_select_query_execute(result, sql);
	} else {
		snprintf(sql, sizeof(sql), "DELETE FROM speed_dial WHERE uid = '%s'", uid);
		sql_select_query_execute(result, sql);
	}
	// play sp-unset-ok
	if (!res) {
		// play sp-unset-ok
		res = tris_streamfile(chan, "speeddial/sp-unset-ok", NULL);
		if (!res) { 
			tris_waitstream(chan, "");
			tris_stopstream(chan);
		} else {
			tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
			return -1;
		}
	} else {
		// play sp-unset-failed
		res = tris_streamfile(chan, "speeddial/sp-unset-failed", NULL);
		if (!res) { 
			tris_waitstream(chan, "");
			tris_stopstream(chan);
		} else {
			tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
			return -1;
		}
	}

	if (write2fifo(fifo_str, strlen(fifo_str)) < 0) {
		tris_verbose("Error: Can't reload Uri\n");
		return -1;
	}

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

	res = tris_register_application_xml(app1, setspeeddial_exec);
	res |= tris_register_application_xml(app2, unsetspeeddial_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Speed Dial");
