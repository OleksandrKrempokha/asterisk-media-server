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
#include "trismedia/paths.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/*** DOCUMENTATION
	<application name="AlarmSet" language="en_US">
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
	<application name="AlarmUnset" language="en_US">
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

static char *app1 = "AlarmSet";
static char *app2 = "AlarmUnset";

int max_alarm_num = 10;

static int alarmset_exec(struct tris_channel *chan, void *data)
{
	int res = -1;
	char *argcopy = NULL;
	int cmd = 0;
	int maxdigits = 255;
	int to = 0;
	char sql[256];
	char uid[256];
	char result[256];
	char time_str[256];
	char song_str[256];
	char hour_str[256];
	char min_str[256];
	char cronfpath[256];
	char content[256];
	int hour_num, min_num, song_num, roomno;
	FILE* cronf;
	int r;
	
	TRIS_DECLARE_APP_ARGS(arglist,
		TRIS_APP_ARG(time_num);
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

	snprintf(sql, sizeof(sql), "SELECT count(*) FROM outgoing_listeners WHERE listener_uid = '%s'", uid);
	sql_select_query_execute(result, sql);

	if (!tris_strlen_zero(result) && atoi(result) >= max_alarm_num) {
		cmd = tris_play_and_wait(chan, "alarm/alarm-set-failed");
		return 0;
	}

	argcopy = tris_strdupa(data);
	
	TRIS_STANDARD_APP_ARGS(arglist, argcopy);

	if (!tris_strlen_zero(arglist.time_num)) {
		if (strlen(arglist.time_num) != 5) {
			cmd = tris_play_and_wait(chan, "alarm/alarm-set-timefail");
			cmd = tris_play_and_wait(chan, "goodbye");
			return 0;
		}
		snprintf(hour_str, 3, "%s", arglist.time_num);
		snprintf(min_str, 3, "%s", arglist.time_num+2);
		snprintf(song_str, 2, "%s", arglist.time_num+4);
	} else {
		cmd = tris_app_getdata(chan, "alarm/alarm-enter-time", time_str, maxdigits, to);
		if (strlen(time_str) != 4) {
			cmd = tris_play_and_wait(chan, "alarm/alarm-set-timefail");
			cmd = tris_play_and_wait(chan, "goodbye");
			return 0;
		}
		snprintf(hour_str, 3, "%s", time_str);
		snprintf(min_str, 3, "%s", time_str+2);
		cmd = tris_app_getdata(chan, "alarm/alarm-enter-songnum", song_str, maxdigits, to);
		if (strlen(song_str) != 1) {
			cmd = tris_play_and_wait(chan, "alarm/alarm-set-songnumfail");
			cmd = tris_play_and_wait(chan, "goodbye");
			return 0;
		}
	}
	hour_num = atoi(hour_str);
	min_num = atoi(min_str);
	song_num = atoi(song_str);
	if (hour_num > 23 || min_num > 59) {
		cmd = tris_play_and_wait(chan, "alarm/alarm-set-timefail");
		cmd = tris_play_and_wait(chan, "goodbye");
		return 0;
	}
	if (song_num > 3) {
		cmd = tris_play_and_wait(chan, "alarm/alarm-set-songnumfail");
		cmd = tris_play_and_wait(chan, "goodbye");
		return 0;
	}

	r = rand();
	snprintf(sql, sizeof(sql), "INSERT INTO outgoing_room (roomname, time, sound_type) VALUES ('%s-%s-%d', '%s:%s', '%d')",
			"SettedbyUser", chan->cid.cid_num, r, hour_str, min_str, song_num);
	sql_select_query_execute(result, sql);
	snprintf(sql, sizeof(sql), "SELECT roomno FROM outgoing_room where roomname='%s-%s-%d' and time='%s:%s'",
			"SettedbyUser", chan->cid.cid_num, r, hour_str, min_str);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result)) {
		tris_log(LOG_ERROR, "Can't insert outgoing room data\n");
		return -1;
	}
	roomno = atoi(result);
	snprintf(sql, sizeof(sql), "INSERT INTO outgoing_listeners (roomno, listener_uid) VALUES ('%d', '%s')", 
			roomno, chan->cid.cid_num);
	sql_select_query_execute(result, sql);

	if (snprintf(cronfpath, sizeof(cronfpath), "%s/%s", tris_config_TRIS_SPOOL_DIR, "outgoing_tmp/outgoing.cron") < 0) {
		tris_log(LOG_ERROR, "Can't create cron file path\n");
		return -1;
	}
	cronf = fopen(cronfpath, "a");
	if (!cronf) {
		tris_log(LOG_ERROR, "Can't open file\n");
		return -1;
	}
	if (snprintf(content, sizeof(content), "%s %s * * * /usr/local/share/trisweb/conf/movefile.sh %s:%s-%d", min_str, 
			hour_str, hour_str, min_str, roomno) < 0) {
		tris_log(LOG_ERROR, "Can't create cron file path\n");
		fclose(cronf);
		return -1;
	}
	if (fprintf(cronf, "%s\n", content) < 0) {
		tris_log(LOG_ERROR, "Can't write to file\n");
		fclose(cronf);
		return -1;
	}
	if (fclose(cronf) < 0) {
		tris_log(LOG_ERROR, "Can't close file\n");
		return -1;
	}

	snprintf(content, sizeof(content), "/usr/bin/crontab %s", cronfpath);
	cronf = popen(content, "r");
	if (!cronf) {
		tris_log(LOG_ERROR, "Can't excute crontab\n");
		return -1;
	}
	pclose(cronf);

	if (snprintf(cronfpath, sizeof(cronfpath), "%s/%s/%s:%s-%d.call", tris_config_TRIS_SPOOL_DIR, 
			"outgoing_tmp", hour_str, min_str, roomno) < 0) {
		tris_log(LOG_ERROR, "Can't create cron file path\n");
		return -1;
	}
	cronf = fopen(cronfpath, "w");
	if (!cronf) {
		tris_log(LOG_ERROR, "Can't open file\n");
		return -1;
	}

	if (snprintf(content, sizeof(content), "roomno:%d\napplication:Playback\ndata:alarm/song_%d\ncallerid:\"Alarm\"<1124>",
			roomno, song_num) < 0) {
		tris_log(LOG_ERROR, "Can't create cron file path\n");
		fclose(cronf);
		return -1;
	}
	if (fprintf(cronf, "%s\n", content) < 0) {
		tris_log(LOG_ERROR, "Can't write to file\n");
		fclose(cronf);
		return -1;
	}
	if (fclose(cronf) < 0) {
		tris_log(LOG_ERROR, "Can't close file\n");
		return -1;
	}

	// play alarm/alarm-set-ok
	if (cmd >= 0) {
		res = tris_play_and_wait(chan, "alarm/alarm-set-ok");
		res = tris_play_and_wait(chan, "goodbye");
	}

	return 0;
}

static int alarmunset_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char *argcopy = NULL;
	char sql[256];
	char uid[256];
	char result[256];
	char time_str[256];
	char hour_str[256];
	char min_str[256];
	char cronfpath[256];
	char tmpcronfpath[256];
	int hour_num, min_num, roomno;
	FILE* cronf, *tmpcronf;
	int cmd = 0;
	int all = 0;
	/*int maxdigits = 255;
	int to = 0;*/
	struct stat st;
	char buf[1024]="";
	
	if (!chan->cid.cid_num)	
		return -1;
	
	TRIS_DECLARE_APP_ARGS(arglist,
		TRIS_APP_ARG(time_str);
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

	if (!tris_strlen_zero(arglist.time_str)) {
		snprintf(time_str, sizeof(time_str), "%s", arglist.time_str);
	} else {
		/*cmd = tris_app_getdata(chan, "alarm/alarm-enter-time", time_str, maxdigits, to);*/
		snprintf(time_str, sizeof(time_str), "*");
	}
	if (strlen(time_str) == 4) {
		snprintf(hour_str, 3, "%s", time_str);
		snprintf(min_str, 3, "%s", time_str+2);
	} else if (strlen(time_str) == 1) {
		if (!strcmp(time_str, "*")) {
			all = 1;
		} else {
			cmd = tris_play_and_wait(chan, "alarm/alarm-set-timefail");
			cmd = tris_play_and_wait(chan, "goodbye");
			return 0;
		}
	} else {
		cmd = tris_play_and_wait(chan, "alarm/alarm-set-timefail");
		cmd = tris_play_and_wait(chan, "goodbye");
		return 0;
	}

	hour_num = atoi(hour_str);
	min_num = atoi(min_str);
	if (hour_num > 23 || min_num > 59) {
		cmd = tris_play_and_wait(chan, "alarm/alarm-set-timefail");
		cmd = tris_play_and_wait(chan, "goodbye");
		return 0;
	}

	if (snprintf(cronfpath, sizeof(cronfpath), "%s/%s", tris_config_TRIS_SPOOL_DIR, "outgoing_tmp/outgoing.cron") < 0) {
		tris_log(LOG_ERROR, "Can't create cron file path\n");
		return -1;
	}

	if (snprintf(tmpcronfpath, sizeof(tmpcronfpath), "%s/%s", tris_config_TRIS_SPOOL_DIR, "outgoing_tmp/outgoing.cron.tmp") < 0) {
		tris_log(LOG_ERROR, "Can't create cron file path\n");
		return -1;
	}
	while (1) {
		if (all) {
			snprintf(sql, sizeof(sql), "SELECT roomno FROM outgoing_listeners WHERE listener_uid='%s'", chan->cid.cid_num);
			sql_select_query_execute(result, sql);
			if (tris_strlen_zero(result))
				break;
			roomno = atoi(result);
		} else {
			snprintf(time_str, sizeof(time_str), "%s:%s", hour_str, min_str);
			snprintf(sql, sizeof(sql), "SELECT outgoing_room.roomno FROM outgoing_room LEFT JOIN outgoing_listeners on outgoing_room.roomno=outgoing_listeners.roomno WHERE outgoing_listeners.listener_uid='%s' and outgoing_room.time='%s'", chan->cid.cid_num, time_str);
			sql_select_query_execute(result, sql);
			if (tris_strlen_zero(result))
				break;
			roomno = atoi(result);
		}

		snprintf(sql, sizeof(sql), "DELETE FROM outgoing_listeners WHERE listener_uid='%s' and roomno='%d'", chan->cid.cid_num, roomno);
		sql_select_query_execute(result, sql);

		if (all) {
			snprintf(sql, sizeof(sql), "SELECT time FROM outgoing_room WHERE roomno='%d'", roomno);
			sql_select_query_execute(result, sql);
			if (tris_strlen_zero(result))
				break;
			snprintf(time_str, sizeof(time_str), "%s", result);
			char* min_temp;
			if (strlen(time_str) < 3 || strlen(time_str) > 5 || !(min_temp = strchr(time_str, ':')))
				break;
			snprintf(hour_str, min_temp-time_str+1, "%s", time_str);
			snprintf(min_str, time_str+strlen(time_str)-min_temp, "%s", min_temp+1);
		}
		
		snprintf(sql, sizeof(sql), "SELECT roomno FROM outgoing_listeners WHERE roomno='%d'", roomno);
		sql_select_query_execute(result, sql);
		if (tris_strlen_zero(result)) {
			snprintf(sql, sizeof(sql), "DELETE FROM outgoing_room WHERE roomno='%d'", roomno);
			sql_select_query_execute(result, sql);
		}
		snprintf(time_str, sizeof(time_str), "%s:%s-%d", hour_str, min_str, roomno);
		cronf = fopen(cronfpath, "r");
		if (!cronf) {
			tris_log(LOG_ERROR, "Can't open file\n");
			break;
		}
		
		tmpcronf = fopen(tmpcronfpath, "w");
		if (!tmpcronf) {
			tris_log(LOG_ERROR, "Can't open file\n");
			fclose(cronf);
			return -1;
		}
		while(fgets(buf, sizeof(buf), cronf)) {
			if (strstr(buf, time_str)) {
				continue;
			}
			if (fprintf(tmpcronf, "%s", buf) < 0) {
				tris_log(LOG_ERROR, "Can't write file\n");
				fclose(cronf);
				fclose(tmpcronf);
				return -1;
			}
		}
		fclose(cronf);
		fclose(tmpcronf);

		if (unlink(cronfpath) < 0) {
			tris_log(LOG_ERROR, "Can't stat file\n");
			return -1;
		}
		
		if (rename(tmpcronfpath, cronfpath) < 0) {
			tris_log(LOG_ERROR, "Can't rename file\n");
			return -1;
		}

		if (chmod(cronfpath, 0777) < 0) {
			tris_log(LOG_ERROR, "Can't change file mode\n");
			return -1;
		}

		if (snprintf(buf, sizeof(buf), "%s/outgoing_tmp/%s:%s-%d.call", tris_config_TRIS_SPOOL_DIR, 
				hour_str, min_str, roomno) < 0) {
			tris_log(LOG_ERROR, "Can't create cron file path\n");
			return -1;
		}
		res = stat(buf, &st);
		if (res == 0) {
			if (unlink(buf) < 0) {
				tris_log(LOG_ERROR, "Can't stat file\n");
				return -1;
			}
		} else if (res < 0) {
			if (unlink(buf) < 0) {
				tris_log(LOG_ERROR, "Can't stat file\n");
			}
		}
		
	}
	
	snprintf(tmpcronfpath, sizeof(tmpcronfpath), "/usr/bin/crontab %s", cronfpath);
	cronf = popen(tmpcronfpath, "r");
	if (!cronf) {
		tris_log(LOG_ERROR, "Can't excute crontab\n");
		return -1;
	}
	pclose(cronf);

	// play alarm/alarm_unset_ok
	res = tris_play_and_wait( chan, "alarm/alarm-unset-ok" );
	res = tris_play_and_wait( chan, "goodbye" );
	
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

	res = tris_register_application_xml(app1, alarmset_exec);
	res |= tris_register_application_xml(app2, alarmunset_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Set Callforward");
