/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2004 - 2006 Digium, Inc.  All rights reserved.
 *
 * Mark Spencer <markster@digium.com>
 *
 * This code is released under the GNU General Public License
 * version 2.0.  See LICENSE for more information.
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 *
 * \brief page() - Paging application
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>dahdi</depend>
	<depend>app_meetme</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 123332 $")

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <dirent.h>

#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/file.h"
#include "trismedia/app.h"
#include "trismedia/chanvars.h"
#include "trismedia/utils.h"
#include "trismedia/devicestate.h"
#include "trismedia/dial.h"
#include "trismedia/logger.h"
#include "trismedia/lock.h"
#include "trismedia/manager.h"
#include "trismedia/acl.h"
#include "trismedia/paths.h"	/* use tris_config_TRIS_SPOOL_DIR */
#include "trismedia/callerid.h"

//#ifdef ODBC_STORAGE
#include "trismedia/res_odbc.h"
//#endif

static const char *app_page= "Page";
static const char *app_confpage= "ConfPage";
static const char *app_urgentcmd= "UrgentCmd";
static const char *app_videoconf= "VideoConference";
static const char *app_cmdbroadcast= "CmdBroadcast";
static const char *app_callconf= "CallConf";

static const char *page_synopsis = "Pages phones";
static const char *confpage_synopsis = "Pages phones for conference";
static const char *cmd_synopsis = "Make a conference for command";
static const char *videoconf_synopsis = "Make a video conference";
static const char *cmdbroadcast_synopsis = "Make a command broadcast";
static const char *callconf_synopsis = "Make a call conference";

static const char *page_descrip =
"Page(roomno,Technology/Resource&Technology2/Resource2[,options])\n"
"  Places outbound calls to the given technology / resource and dumps\n"
"them into a conference bridge as muted participants.  The original\n"
"caller is dumped into the conference as a speaker and the room is\n"
"destroyed when the original caller leaves.  Valid options are:\n"
"        d - full duplex audio\n"
"        q - quiet, do not play beep to caller\n"
"        r - record the page into a file (see 'r' for app_meetme)\n"
"        s - only dial channel if devicestate says it is not in use\n";

enum {
	PAGE_DUPLEX = (1 << 0),
	PAGE_QUIET = (1 << 1),
	PAGE_RECORD = (1 << 2),
	PAGE_SKIP = (1 << 3),
} page_opt_flags;

TRIS_APP_OPTIONS(page_opts, {
	TRIS_APP_OPTION('d', PAGE_DUPLEX),
	TRIS_APP_OPTION('q', PAGE_QUIET),
	TRIS_APP_OPTION('r', PAGE_RECORD),
	TRIS_APP_OPTION('s', PAGE_SKIP),
});

/*! \brief free the buffer if allocated, and set the pointer to the second arg */
#define S_REPLACE(s, new_val) \
	do {                      \
		if (s) {              \
			free(s);          \
		}                     \
		s = (new_val);        \
	} while (0)


#define MAX_DIALS 256

struct tris_page {
	char roomno[80];
	struct tris_dial *dials[MAX_DIALS];
	char *extens[MAX_DIALS];
	int pos;
	TRIS_LIST_ENTRY(tris_page) list;
};

static TRIS_LIST_HEAD_STATIC(pages, tris_page);

struct tris_vm_user {
	char context[TRIS_MAX_CONTEXT];   /*!< Voicemail context */
	char mailbox[TRIS_MAX_EXTENSION]; /*!< Mailbox id, unique within vm context */
	char password[80];               /*!< Secret pin code, numbers only */
	char fullname[80];               /*!< Full name, for directory app */
	char email[80];                  /*!< E-mail address */
	char pager[80];                  /*!< E-mail address to pager (no attachment) */
	char serveremail[80];            /*!< From: Mail address */
	char mailcmd[160];               /*!< Configurable mail command */
	char language[MAX_LANGUAGE];     /*!< Config: Language setting */
	char zonetag[80];                /*!< Time zone */
	char callback[80];
	char dialout[80];
	char uniqueid[80];               /*!< Unique integer identifier */
	char exit[80];
	char attachfmt[20];              /*!< Attachment format */
	unsigned int flags;              /*!< VM_ flags */	
	int saydurationm;
	int maxmsg;                      /*!< Maximum number of msgs per folder for this mailbox */
	int maxdeletedmsg;               /*!< Maximum number of deleted msgs saved for this mailbox */
	int maxsecs;                     /*!< Maximum number of seconds per message for this mailbox */
#ifdef IMAP_STORAGE
	char imapuser[80];               /*!< IMAP server login */
	char imappassword[80];           /*!< IMAP server password if authpassword not defined */
#endif
	double volgain;                  /*!< Volume gain for voicemails sent via email */
	TRIS_LIST_ENTRY(tris_vm_user) list;
};
#define VM_ALLOCED       (1 << 13)

static struct tris_vm_user* create_user(struct tris_vm_user *ivm, const char *context, char* usernm)
{
	struct tris_vm_user* vmu = NULL;

	if (!context)
		context = "default";


	/* Make a copy, so that on a reload, we have no race */
	if ((vmu = (ivm ? ivm : tris_malloc(sizeof(*vmu))))) {
		tris_set2_flag(vmu, !ivm, VM_ALLOCED);
		tris_copy_string(vmu->context, context, sizeof(vmu->context));
		tris_copy_string(vmu->mailbox, usernm, sizeof(vmu->mailbox));
		
	//	populate_defaults(vmu);

		vmu->password[0] = '\0';
	}
	
	return vmu;
}

static int make_file(char *dest, const int len, const char *dir, const int num)
{
	return snprintf(dest, len, "%s/msg%04d", dir, num);
}

static void free_user(struct tris_vm_user *vmu)
{
	if (tris_test_flag(vmu, VM_ALLOCED))
		tris_free(vmu);
}


static int make_dir(char *dest, int len, const char *domain, const char *username, const char *folder)
{
	static char VM_SPOOL_DIR[PATH_MAX];
	snprintf(VM_SPOOL_DIR, sizeof(VM_SPOOL_DIR), "%s/voicemail/", tris_config_TRIS_SPOOL_DIR);
	return snprintf(dest, len, "%s%s/%s%s%s", VM_SPOOL_DIR, domain, username, tris_strlen_zero(folder) ? "" : "/", folder ? folder : "");
}

static int create_dirpath(char *dest, int len, const char *context, const char *ext, const char *folder)
{
	mode_t	mode = 0777;
	int res;

	make_dir(dest, len, context, ext, folder);
	if ((res = tris_mkdir(dest, mode))) {
		tris_log(LOG_WARNING, "tris_mkdir '%s' failed: %s\n", dest, strerror(res));
		return -1;
	}
	return 0;
}

static int get_date(char *s, int len)
{
	struct tris_tm tm;
	struct timeval t = tris_tvnow();
	
	tris_localtime(&t, &tm, NULL);
	return tris_strftime(s, len, "%F %T", &tm);
}

#define MAXMSGLIMIT 9999

static int last_message_index(struct tris_vm_user *vmu, char *dir)
{
	int x;
	unsigned char map[MAXMSGLIMIT] = "";
	DIR *msgdir;
	struct dirent *msgdirent;
	int msgdirint;

	/* Reading the entire directory into a file map scales better than
	* doing a stat repeatedly on a predicted sequence.  I suspect this
	* is partially due to stat(2) internally doing a readdir(2) itself to
	* find each file. */
	msgdir = opendir(dir);
	while ((msgdirent = readdir(msgdir))) {
		if (sscanf(msgdirent->d_name, "msg%d", &msgdirint) == 1 && msgdirint < MAXMSGLIMIT)
			map[msgdirint] = 1;
	}
	closedir(msgdir);

	for (x = 0; x < MAXMSGLIMIT; x++) {
		if (map[x] == 0)
			break;
	}

	return x - 1;
}


static int store_vmfile(struct tris_channel *chan, char *tempfile, char *context, char *ext, char *callerid, int duration, char *fmt)
{
	char priority[16];
	char origtime[16];
	char date[256];
	int rtmsgid = 0;
	const char *category = NULL;
	struct tris_vm_user *vmu, vmus;
	FILE *txt;
	char dir[PATH_MAX], tmpdir[PATH_MAX];
	char fn[PATH_MAX];
	char txtfile[PATH_MAX], tmptxtfile[PATH_MAX];
	int msgnum = 0;
	int txtdes;
	char sql[256], uid[32], calleruid[256];
	int my_umask;

	tris_verbose("[acmy] send voicemail to %s\n", ext);

	category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");
	
	tris_debug(3, "Before find_user\n");
	if (!(vmu = create_user(&vmus, context, ext))) {
		return 0;
	}
	
	create_dirpath(dir, sizeof(dir), vmu->context, ext, "INBOX");

	/* Real filename */
	msgnum = last_message_index(vmu, dir) + 1;
	make_file(fn, sizeof(fn), dir, msgnum);
	
	/* Get uid by extension */
	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE username='%s'", callerid);
	sql_select_query_execute(uid, sql);

	/* Store information in real-time storage */
	if (tris_check_realtime("voicemail_data")) {
		snprintf(priority, sizeof(priority), "%d", chan->priority);
		snprintf(origtime, sizeof(origtime), "%ld", (long)time(NULL));
		get_date(date, sizeof(date));
		rtmsgid = tris_store_realtime("voicemail_data", "origmailbox", ext, "context", chan->context, "macrocontext", chan->macrocontext, "exten", chan->exten, "priority", priority, "callerchan", chan->name, "callerid", tris_callerid_merge(calleruid, sizeof(calleruid), uid, callerid, "Unknown"), "origdate", date, "origtime", origtime, "category", category ? category : "", NULL);
	}

	/* Store information */
	create_dirpath(tmpdir, sizeof(tmpdir), vmu->context, ext, "tmp");
	snprintf(tmptxtfile, sizeof(tmptxtfile), "%s/XXXXXX", tmpdir);
	txtdes = mkstemp(tmptxtfile);
	my_umask = umask(0);
	chmod(tmptxtfile, 0666 & ~my_umask);

	txt = fdopen(txtdes, "w+");
	if (txt) {
		get_date(date, sizeof(date));
		fprintf(txt, 
			";\n"
			"; Message Information file by broadcast voicemail\n"
			";\n"
			"[message]\n"
			"origmailbox=%s\n"
			"context=%s\n"
			"macrocontext=%s\n"
			"exten=%s\n"
			"priority=%d\n"
			"callerchan=%s\n"
			"callerid=%s\n"
			"origdate=%s\n"
			"origtime=%ld\n"
			"category=%s\n",
			ext,
			vmu->context,
			chan->macrocontext, 
			chan->exten,
			chan->priority,
			chan->name,
			tris_callerid_merge(calleruid, sizeof(calleruid), S_OR(uid, NULL), S_OR(callerid, NULL), "Unknown"),
			date, (long)time(NULL),
			category ? category : ""); 
	}
	fprintf(txt, "duration=%d\n", duration);
	fclose(txt);

	snprintf(txtfile, sizeof(txtfile), "%s.txt", fn);
	rename(tmptxtfile, txtfile);

	/* Otherwise 1 is to save the existing message */
	tris_verb(3, "Saving message as is\n");

	/* Store message 
		copy instead rename for multi user 
	tris_filerename(tempfile, fn, NULL); */
	tris_filecopy(tempfile, fn, NULL);
	
	free_user(vmu);

	return 1;
}

struct trisconf_obj {
	char *sql;
	char roomno[12];
	char memberuid[64];
	char mempermit[32];
	SQLLEN err;
};

static SQLHSTMT trisconf_prepare(struct odbc_obj *obj, void *data)
{
	struct trisconf_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_CHAR, q->roomno, sizeof(q->roomno), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->memberuid, sizeof(q->memberuid), &q->err);
	SQLBindCol(sth, 3, SQL_C_CHAR, q->mempermit, sizeof(q->mempermit), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

static int page_exec(struct tris_channel *chan, void *data)
{
	char *options, *tech, *resource, *tmp;
	char meetmeopts[88], originator[TRIS_CHANNEL_NAME], *opts[0];
	struct tris_flags flags = { 0 };
	char *confid;
	struct tris_app *app, *the_app;
	int res = 0, pos = 0, i = 0;
	struct tris_dial *dials[MAX_DIALS];
	int record_count = 0;
	char *dial_extns[MAX_DIALS];
	char recordingtmp[TRIS_MAX_EXTENSION] = "";
	char *tmpext, *ext, *callerid;
	char callinfo[80];
	int duration = 0;
	struct tris_filestream *fs;
	enum tris_dial_result state = 0;
 
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "This application requires at least one argument (destination(s) to page)\n");
		return -1;
	}

	if (!(app = pbx_findapp("MeetMe"))) {
		tris_log(LOG_WARNING, "There is no MeetMe application available!\n");
		return -1;
	};

	options = tris_strdupa(data);

//	S_REPLACE(chan->cid.cid_name, tris_strdup("Conference")); 
	S_REPLACE(chan->cid.cid_name, tris_strdup("Conference")); 

	tris_copy_string(originator, chan->name, sizeof(originator));
	if ((tmp = strchr(originator, '-')))
		*tmp = '\0';

	confid = strsep(&options, ",");
	
	snprintf(callinfo, sizeof(callinfo), "Call-Info: MS,broadcast,%s", confid);
	the_app = pbx_findapp("SIPAddHeader");
	if (the_app)
		pbx_exec(chan, the_app, callinfo);

	tmp = strsep(&options, ",");
	if (options)
		tris_app_parse_options(page_opts, &flags, opts, options);

	snprintf(meetmeopts, sizeof(meetmeopts), "MeetMe,%s,%s%sqd", confid, (tris_test_flag(&flags, PAGE_DUPLEX) ? "" : "m"),
		(tris_test_flag(&flags, PAGE_RECORD) ? "r" : "") );

	/* Go through parsing/calling each device */
	while ((tech = strsep(&tmp, "&"))) {

		struct tris_dial *dial = NULL;

		/* don't call the originating device */
		if (!strcasecmp(tech, originator))
			continue;

		/* If no resource is available, continue on */
		if (!(resource = strchr(tech, '/'))) {
			tris_log(LOG_WARNING, "Incomplete destination '%s' supplied.\n", tech);
			continue;
		}
		dial_extns[record_count++] = tris_strdupa(tech);
		
		/* Ensure device is not in use if skip option is enabled */
		if (tris_test_flag(&flags, PAGE_SKIP)) {
/*
			state = tris_device_state(tech);
			if (state == TRIS_DEVICE_NOT_INUSE) {
				tris_log(LOG_WARNING, "Destination '%s' has device state '%s'. start recording file\n", tech, devstate2str(state));
				tris_set_flag(&flags, PAGE_RECORD);

				tmpext = tris_strdupa(resource);
				ext = strchr(tmpext, '@');
				*ext = '\0';
				ext = tmpext;
				dial_extns[record_count++] = ext+1;
			} else {
				if (state == TRIS_DEVICE_UNKNOWN) {
					tris_log(LOG_WARNING, "Destination '%s' has device state '%s'. Paging anyway.\n", tech, devstate2str(state));
				}
			}
*/
		}

		*resource++ = '\0';

		/* Create a dialing structure */
		if (!(dial = tris_dial_create())) {
			tris_log(LOG_WARNING, "Failed to create dialing structure.\n");
			continue;
		}

		/* Append technology and resource */
		tris_dial_append(dial, tech, resource);

		/* Set ANSWER_EXEC as global option */
		tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC, meetmeopts);

		/* Run this dial in async mode */
		tris_dial_run(dial, chan, 1, 0);

		/* Put in our dialing array */
		dials[pos++] = dial;
	}

	tris_set_flag(&flags, PAGE_RECORD);
	if (tris_test_flag(&flags, PAGE_RECORD)) {
 		snprintf(recordingtmp, sizeof(recordingtmp), "%s/broadcast-rec-%s", tris_config_TRIS_SPOOL_DIR, chan->uniqueid);
		pbx_builtin_setvar_helper(chan, "MEETME_RECORDINGFILE", recordingtmp);
	}

	if (!tris_test_flag(&flags, PAGE_QUIET)) {
		res = tris_streamfile(chan, "beep", chan->language);
		if (!res)
			res = tris_waitstream(chan, "");
	}

	while(state != TRIS_DIAL_RESULT_ANSWERED 
			&& state != TRIS_DIAL_RESULT_TIMEOUT) {
		for (i=0; i<pos; i++) {
			state = tris_dial_state(dials[i]);
			if(state == TRIS_DIAL_RESULT_ANSWERED)
				break;
			else if(state == TRIS_DIAL_RESULT_TIMEOUT)
				break;
		}
		if ((res = tris_waitstream(chan, "")))	/* Channel is hung up */
			goto end_page;
	}
	
	if (!res) {
		tris_play_and_wait(chan, "beep");
		snprintf(meetmeopts, sizeof(meetmeopts), "%s,A%s%sqxd", confid, (tris_test_flag(&flags, PAGE_DUPLEX) ? "" : "t"), 
			(tris_test_flag(&flags, PAGE_RECORD) ? "r" : "") );
		pbx_exec(chan, app,meetmeopts );
	}
	
	tris_verbose("hsh commented....\n");
	if(tris_fileexists(recordingtmp, NULL, NULL) <= 0)
		goto end_page;
	
	fs = tris_openstream(chan, recordingtmp, chan->language);
	if(!fs)
		goto end_page;
	
	tris_seekstream(fs, 0, SEEK_END);
	duration = fs? tris_tellstream(fs) / 8000 : 0;
	//tris_closestream(fs);
	tris_verbose("broadcast mail\nrecording temp file : %s\ncount of users to leave message : duration = %d\n", recordingtmp, duration);
	/* copy temp file to mailbox */
	for (i=0; i<pos; i++) {
		state = tris_dial_state(dials[i]);
		tris_verbose("  == %d\n", state);
		if (!(resource = strchr(dial_extns[i], '/'))) {
			tris_log(LOG_WARNING, "Incomplete destination '%s' supplied.\n", dial_extns[i]);
			continue;
		}
		if (state != TRIS_DIAL_RESULT_ANSWERED) {
			tris_log(LOG_WARNING, "Destination '%s' has device state '%d'. start recording file\n", dial_extns[i], state);
//			tris_set_flag(&flags, PAGE_RECORD);
			resource++;
			tmpext = tris_strdupa(resource);
			ext = strchr(tmpext, '@');
			*ext = '\0';
			ext = tmpext;
			callerid = tris_strdupa(chan->cid.cid_num);

			if (!store_vmfile(chan, recordingtmp, NULL, ext, callerid, duration, "wav")) {
				tris_log(LOG_WARNING, "fail sending mail to '%s'\n", ext);
			}
		} else {
			if (state == TRIS_DEVICE_UNKNOWN) {
				tris_log(LOG_WARNING, "Destination '%s' has device state ''. Paging anyway.\n", dial_extns[i]);
			}
		}
	}
	
end_page:
	/* Go through each dial attempt cancelling, joining, and destroying */
	for (i = 0; i < pos; i++) {
		struct tris_dial *dial = dials[i];

		/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
		tris_dial_join(dial);

		/* Hangup all channels */
		tris_dial_hangup(dial);

		/* Destroy dialing structure */
		tris_dial_destroy(dial);
		
		tris_verbose("  --  destroy dial(%d)\n", i);
	}

	/* remove recording temp file */
	tris_filedelete(recordingtmp, NULL);

	return -1;
}

static int cmdbroadcast_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char roomno[256] = "";
	char options[2048] = "";
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	struct odbc_obj *obj;
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	struct trisconf_obj q;
	char sql[256];
	char resource_list[2048] = "";
	char tmp[2048] = "";
	int count = 0;
	char groups[1024] = "";
	char *cur = 0, *sep = 0;
	
	
	
	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}
	
	if (tris_strlen_zero(chan->cid.cid_num)) {
		return -1;
	}

	snprintf(sql, sizeof(sql), "SELECT roomno FROM broadcast WHERE announcer = '%s'", chan->cid.cid_num);
	sql_select_query_execute(roomno, sql);
	if (tris_strlen_zero(roomno)) {
		tris_play_and_wait(chan, "broadcast/no_manager");
		return 0;
	}
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);

	/* Go through parsing/calling each device */

	memset(&q, 0, sizeof(q));
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;

	snprintf(sql, sizeof(sql), "SELECT roomno, listeneruid, listenergid FROM broadcast_listener WHERE  roomno = '%s'", roomno);
	q.sql = sql;
	
	stmt = tris_odbc_prepare_and_execute(obj, trisconf_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	while ((SQLFetch(stmt)) != SQL_NO_DATA && count < 20) {
		if (!tris_strlen_zero(q.memberuid)) {
			if (!tris_strlen_zero(resource_list))
				snprintf(tmp, sizeof(tmp), "%s&SIP/%s@%s:5060", resource_list, q.memberuid, tris_inet_ntoa(ourip));
			else
				snprintf(tmp, sizeof(tmp), "SIP/%s@%s:5060", q.memberuid, tris_inet_ntoa(ourip));

			snprintf(resource_list, sizeof(resource_list), "%s", tmp);
			count++;
		} else if (!tris_strlen_zero(q.mempermit)) {
			if (!tris_strlen_zero(groups))
				snprintf(tmp, sizeof(tmp), "%s,%s", groups, q.mempermit);
			else
				snprintf(tmp, sizeof(tmp), "%s", q.mempermit);
			snprintf(groups, sizeof(groups), "%s", tmp);
		}
	}
	/* remove recording temp file */
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);

	sep = groups;
	cur = groups;
	while (sep) {
		cur = strsep(&sep, ",");
		if (!tris_strlen_zero(cur)) {
			/* Go through parsing/calling each device */
			
			memset(&q, 0, sizeof(q));
			obj = tris_odbc_request_obj("trisdb", 0);
			if (!obj)
				return 0;
			
			snprintf(sql, sizeof(sql), "SELECT gid, uid, extension FROM user_info WHERE gid = '%s' AND extension != '%s'",
					roomno, chan->cid.cid_num);
			q.sql = sql;
			
			stmt = tris_odbc_prepare_and_execute(obj, trisconf_prepare, &q);
			
			if (!stmt) {
				tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
				tris_odbc_release_obj(obj);
				return 0;
			}
			
			res = SQLNumResultCols(stmt, &rowcount);
			
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				return 0;
			}
			
			if (!rowcount) {
				tris_log(LOG_NOTICE, "found nothing\n");
				tris_odbc_release_obj(obj);
				return -1;
			}
			
			while ((SQLFetch(stmt)) != SQL_NO_DATA && count < 20) {
				if (!tris_strlen_zero(q.memberuid)) {
					snprintf(tmp, sizeof(tmp), "SIP/%s@%s:5060", q.memberuid, tris_inet_ntoa(ourip));
					if (!tris_strlen_zero(resource_list)) {
						if (!strstr(resource_list, tmp)) {
							snprintf(tmp, sizeof(tmp), "%s&SIP/%s@%s:5060", resource_list, q.memberuid, tris_inet_ntoa(ourip));
							snprintf(resource_list, sizeof(resource_list), "%s", tmp);
							count++;
						}
					} else {
						snprintf(tmp, sizeof(tmp), "SIP/%s@%s:5060", q.memberuid, tris_inet_ntoa(ourip));
						snprintf(resource_list, sizeof(resource_list), "%s", tmp);
						count++;
					}
				}
			}
			/* remove recording temp file */
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			
		}
	}
	
	snprintf(options, sizeof(options), "b%s,%s,sq", roomno, resource_list);
	page_exec(chan, options);
	return 0;

}

static int confpage_exec(struct tris_channel *chan, void *data)
{
	char *options, *tmp;
	char meetmeopts[88], originator[TRIS_CHANNEL_NAME], *opts[0];
	char onlylistenopts[88];
	char *p_opts = meetmeopts;
	struct tris_flags flags = { 0 };
	char *confid, *roomid;
	struct tris_app *app,*the_app;
	int res = 0, pos = 0, i = 0;
	struct tris_dial *dials[MAX_DIALS];
	//int record_count = 0;
	//char *dial_extns[MAX_DIALS];
	char recordingtmp[TRIS_MAX_EXTENSION] = "";
//	char *tmpext, *ext, *callerid;
	char callinfo[80];
	char roomname[80], sql[256], recording[80] = "";

	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	char calling_uri[100];
		

	struct odbc_obj *obj;
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	struct trisconf_obj q;
	
//	int duration = 0;
//	struct tris_filestream *fs;
//	enum tris_dial_result state = 0;

	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);

 
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "This application requires at least one argument (destination(s) to page)\n");
		return -1;
	}

	if (!(app = pbx_findapp("MeetMe"))) {
		tris_log(LOG_WARNING, "There is no MeetMe application available!\n");
		return -1;
	};

	options = tris_strdupa(data);
	
	tris_copy_string(originator, chan->name, sizeof(originator));
	if ((tmp = strchr(originator, '-')))
		*tmp = '\0';

	confid = strsep(&options, ",");
	roomid = strsep(&options, ",");
	
	snprintf(callinfo, sizeof(callinfo), "Call-Info: MS,callconf,%s", confid);
	the_app = pbx_findapp("SIPAddHeader");
	if (the_app)
		pbx_exec(chan, the_app, callinfo);
	
	tmp = strsep(&options, ",");
	
	if (options)
		tris_app_parse_options(page_opts, &flags, opts, options);

	snprintf(meetmeopts, sizeof(meetmeopts), "MeetMe,%s,%s%sdxq", confid, (tris_test_flag(&flags, PAGE_DUPLEX) ? "" : "m"),
		(tris_test_flag(&flags, PAGE_RECORD) ? "r" : "") );
	
	snprintf(onlylistenopts, sizeof(onlylistenopts), "MeetMe,%s,%sdmxq", confid,
		(tris_test_flag(&flags, PAGE_RECORD) ? "r" : "") );

	snprintf(sql, sizeof(sql), "SELECT roomname FROM callconf_room WHERE roomid='%s'", roomid);
	sql_select_query_execute(roomname, sql);

	snprintf(sql, sizeof(sql), "SELECT recording FROM callconf_room WHERE roomid='%s'", roomid);
	sql_select_query_execute(recording, sql);

	//S_REPLACE(chan->cid.cid_name, tris_strdup("Conference")); 
	S_REPLACE(chan->cid.cid_name, tris_strdup(roomname));

	/* Go through parsing/calling each device */

	memset(&q, 0, sizeof(q));
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;

	snprintf(sql, sizeof(sql), "SELECT roomno, memberuid, mempermit FROM callconf_member WHERE roomid = '%s'", roomid);
	q.sql = sql;
	
	stmt = tris_odbc_prepare_and_execute(obj, trisconf_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}
	//while ((tech = strsep(&tmp, "&"))) {
	while ((SQLFetch(stmt)) != SQL_NO_DATA) {

		struct tris_dial *dial = NULL;

		if(!strcmp(q.mempermit, "1"))
			p_opts = meetmeopts;
		else
			p_opts = onlylistenopts;
		
		sprintf(calling_uri,"%s@%s:5060", q.memberuid, tris_inet_ntoa(ourip));
		
		/* Create a dialing structure */
		if (!(dial = tris_dial_create())) {
			tris_log(LOG_WARNING, "Failed to create dialing structure.\n");
			continue;
		}

		/* Append technology and resource */
		//tris_dial_append(dial, tech, calling_uri);
		tris_dial_append(dial, "SIP", calling_uri);

		/* Set ANSWER_EXEC as global option */
		tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC, p_opts);

		/* Run this dial in async mode */
		tris_dial_run(dial, chan, 1, 0);

		/* Put in our dialing array */
		dials[pos++] = dial;
	}

/*	tris_set_flag(&flags, PAGE_RECORD);
	if (tris_test_flag(&flags, PAGE_RECORD)) {
 		snprintf(recordingtmp, sizeof(recordingtmp), "%s/broadcast-rec-%s", tris_config_TRIS_SPOOL_DIR, chan->uniqueid);
		pbx_builtin_setvar_helper(chan, "MEETME_RECORDINGFILE", recordingtmp);
	}
*/
	if (!tris_test_flag(&flags, PAGE_QUIET)) {
		res = tris_streamfile(chan, "beep", chan->language);
		if (!res)
			res = tris_waitstream(chan, "");
	}

/*	while(state != TRIS_DIAL_RESULT_ANSWERED 
			&& state != TRIS_DIAL_RESULT_TIMEOUT) {
		for (i=0; i<pos; i++) {
			state = tris_dial_state(dials[i]);
			if(state == TRIS_DIAL_RESULT_ANSWERED)
				break;
			else if(state == TRIS_DIAL_RESULT_TIMEOUT)
				break;
		}
		if (res = tris_waitstream(chan, ""))	// Channel is hung up 
			goto end_confpage;
	}
*/	
	if (!res) {
		//tris_play_and_wait(chan, "beep");
		snprintf(meetmeopts, sizeof(meetmeopts), "%s,a%s%sdA", confid, (tris_test_flag(&flags, PAGE_DUPLEX) ? "" : "t"),
			(!strcmp(recording, "1")) ? "r" : "");
		pbx_exec(chan, app,meetmeopts );
	}
	
	tris_verbose("hsh commented....\n");
/*	if(tris_fileexists(recordingtmp, NULL, NULL) <= 0)
		goto end_confpage;
	
	fs = tris_openstream(chan, recordingtmp, chan->language);
	if(!fs)
		goto end_confpage;
	
	tris_seekstream(fs, 0, SEEK_END);
	duration = fs? tris_tellstream(fs) / 8000 : 0;
	//tris_closestream(fs);
	tris_verbose("broadcast mail\nrecording temp file : %s\ncount of users to leave message : duration = %d\n", recordingtmp, duration);

	for (i=0; i<pos; i++) {
		state = tris_dial_state(dials[i]);
		tris_verbose("  == %d\n", state);
		if (!(resource = strchr(dial_extns[i], '/'))) {
			tris_log(LOG_WARNING, "Incomplete destination '%s' supplied.\n", dial_extns[i]);
			continue;
		}
		if (state != TRIS_DIAL_RESULT_ANSWERED) {
			tris_log(LOG_WARNING, "Destination '%s' has device state '%d'. start recording file\n", dial_extns[i], state);
//			tris_set_flag(&flags, PAGE_RECORD);
			resource++;
			tmpext = tris_strdupa(resource);
			ext = strchr(tmpext, '@');
			*ext = '\0';
			ext = tmpext;
			callerid = tris_strdupa(chan->cid.cid_num);

			if (!store_vmfile(chan, recordingtmp, NULL, ext, callerid, duration, "wav")) {
				tris_log(LOG_WARNING, "fail sending mail to '%s'\n", ext);
			}
		} else {
			if (state == TRIS_DEVICE_UNKNOWN) {
				tris_log(LOG_WARNING, "Destination '%s' has device state '%s'. Paging anyway.\n", dial_extns[i], devstate2str(state));
			}
		}
	}
*/

//end_confpage:
	/* Go through each dial attempt cancelling, joining, and destroying */
	if(pos)
		sleep(2);
	for (i = 0; i < pos; i++) {
		struct tris_dial *dial = dials[i];

		/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
		tris_dial_join(dial);

		/* Hangup all channels */
		tris_dial_hangup(dial);

		/* Destroy dialing structure */
		tris_dial_destroy(dial);
		
		tris_verbose("  --  destroy dial(%d)\n", i);
	}

	/* remove recording temp file */
	tris_filedelete(recordingtmp, NULL);

	/* remove recording temp file */
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);

	return -1;
}

static int check_callconf_sponser(char *roomno, char * ext, char *roomid)
{
	char sql[256];
	char result[1024];
	char *tmp = 0, *cur;

	snprintf(sql, sizeof(sql), "SELECT sponseruid FROM callconf_room WHERE sponseruid REGEXP '.*%s.*' AND roomno = '%s'",
			ext, roomno);
	sql_select_query_execute(result, sql);
	
	if(tris_strlen_zero(result)){
		return 0;
	}

	cur = result;
	while (cur) {
		tmp = strsep(&cur, ",");
		if (!tmp)
			return 0;
		if (strlen(tmp) == strlen(ext) && !strncmp(tmp, ext, strlen(ext))) {
			snprintf(sql, sizeof(sql), "SELECT roomid FROM callconf_room WHERE sponseruid REGEXP '.*%s.*' AND roomno = '%s'",
					ext, roomno);
			sql_select_query_execute(roomid, sql);
			return 1;
		}
	}

	return 0;

}

static int check_callconf_member(char *roomno, char *ext, char *roomid) 
{
	char sql[256];

	snprintf(sql, sizeof(sql), "SELECT roomid FROM callconf_member WHERE roomno='%s' and memberuid='%s'",
			roomno, ext);
	sql_select_query_execute(roomid, sql);
	if (tris_strlen_zero(roomid))
		return 0;
	return 1;
}

static int callconf_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char roomno[256] = "";
	char options[2048] = "";
	char roomid[1024] = "";
	char realid[1024] = "";
	char sql[256] = "";
	char recording[256] = "";
	char *tmp = NULL;
	struct tris_app *tris_app = NULL;
	int sponser = 0;
	
	if (tris_strlen_zero(chan->cid.cid_num)) {
		return -1;
	}
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}
	
	snprintf(roomno, sizeof(roomno), "spg%s", chan->exten);
	if (!check_callconf_sponser(roomno, chan->cid.cid_num, options)) {
		if (!check_callconf_member(roomno, chan->cid.cid_num, options)) {
			tris_play_and_wait(chan, "conference/is_not_participant");
			return 0;
		}
	} else {
		sponser = 1;
	}
	if (tris_strlen_zero(options)) {
		tris_play_and_wait(chan, "conference/is_not_participant");
		return 0;
	}
	snprintf(roomid, sizeof(roomid), "spg%s-%s", chan->exten, options);
	snprintf(realid, sizeof(realid), "%s", options);

	tris_app = pbx_findapp("MeetmeCount");
	if (!tris_app) {
		tris_log(LOG_ERROR, "Can't find MeetmeCount\n");
		return -1;
	}
	snprintf(options, sizeof(options), "%s,numofmembers",roomid);
	/* All is well... execute the application */
	pbx_exec(chan, tris_app, options);
	tmp = (char*)pbx_builtin_getvar_helper(chan, "numofmembers");

	if (sponser) {
		if (!tris_strlen_zero(tmp) && atoi(tmp) >= 1) {
			tris_play_and_wait(chan, "conference/select_other_room");
			return 0;
		}
		/*snprintf(options, sizeof(options), "%s,%s,0000,sqd",roomid, realid);
		confpage_exec(chan, options);*/
		tris_app = pbx_findapp("SIPAddHeader");
		if (!tris_app) {
			tris_log(LOG_ERROR, "Can't find SIPAddHeader\n");
			return -1;
		}
		snprintf(options, sizeof(options), "Call-Info: MS,callconf,%s", roomid);
		pbx_exec(chan, tris_app, options);
		tris_app = pbx_findapp("Meetme");
		if (!tris_app) {
			tris_log(LOG_ERROR, "Can't find Meetme\n");
			return -1;
		}
		snprintf(sql, sizeof(sql), "SELECT recording FROM callconf_room WHERE roomid='%s'", realid);
		sql_select_query_execute(recording, sql);
		snprintf(options, sizeof(options), "%s,adA%s",roomid, (!strcmp(recording, "1")) ? "r" : "");

		snprintf(sql, sizeof(sql), "SELECT send_notify FROM callconf_room WHERE roomid='%s'", realid);
		sql_select_query_execute(recording, sql);
		if (!strcmp(recording, "1"))
			chan->seqtype = 1;
		
		pbx_exec(chan, tris_app, options);
	} else {
		if (tris_strlen_zero(tmp) || atoi(tmp) < 1) {
			tris_play_and_wait(chan, "conference/you_cant_open_the_conf");
			return 0;
		}
		tris_app = pbx_findapp("SIPAddHeader");
		if (!tris_app) {
			tris_log(LOG_ERROR, "Can't find SIPAddHeader\n");
			return -1;
		}
		snprintf(options, sizeof(options), "Call-Info: MS,callconf,%s",roomid);
		pbx_exec(chan, tris_app, options);
		tris_app = pbx_findapp("Meetme");
		if (!tris_app) {
			tris_log(LOG_ERROR, "Can't find Meetme\n");
			return -1;
		}
		snprintf(options, sizeof(options), "%s,dxq",roomid);
		pbx_exec(chan, tris_app, options);
	}
	
	return 0;

}

static char mandescr_pagestatus[] =
"Description: Lists all users in a particular Page conference.\n"
"Variables:\n"
"    *Roomno: <roomno>\n";

static int action_pagestatus(struct mansession *s, const struct message *m)
{
	const char *roomno= astman_get_header(m, "Roomno");
	struct tris_page *cnf;
	int total = 0;
	enum tris_dial_result state = 0;
	int nstate;
	int i;
	
	if (TRIS_LIST_EMPTY(&pages)) {
		astman_send_error(s, m, "No active conferences.");
		return 0;
	}

	astman_send_listack(s, m, "Meetme user list will follow", "start");

	/* Find the right conference */
	TRIS_LIST_LOCK(&pages);
	TRIS_LIST_TRAVERSE(&pages, cnf, list) {
		/* If we ask for one particular, and this isn't it, skip it */
		tris_verbose("  --  --  %s : %s\n", roomno, cnf->roomno);
		if (!tris_strlen_zero(roomno) && strcmp(cnf->roomno, roomno))
			continue;

		/* Show all the users */
		for(i=0; i<cnf->pos; i++){
			total++;
			state = tris_dial_state(cnf->dials[i]);
			switch (state) {
			case TRIS_DIAL_RESULT_ANSWERED:
				nstate = 0;
				break;
			case TRIS_DIAL_RESULT_BUSY:
			case TRIS_DIAL_RESULT_CONGESTION:
			case TRIS_DIAL_RESULT_TIMEOUT:
			case TRIS_DIAL_RESULT_FORBIDDEN:
				nstate = 1;
				break;
			case TRIS_DIAL_RESULT_TAKEOFFHOOK:
				nstate = 2;
				break;
			case TRIS_DIAL_RESULT_OFFHOOK:
				nstate = 3;
				break;
			default:
				nstate = 4;
				break;
			}
			tris_verbose("%s:%d\n", cnf->extens[i], nstate); 

			astman_append(s,
			"%s:%d\r\n",
			cnf->extens[i],
			nstate); 
		}
	}
	TRIS_LIST_UNLOCK(&pages);
	/* Send final confirmation */
/*	astman_append(s,
	"Event: MeetmeListComplete\r\n"
	"EventList: Complete\r\n"
	"ListItems: %d\r\n"
	"\r\n", total);
*/
	return 0;
}


static int urgentcmd_exec(struct tris_channel *chan, void *data)
{
	char *options, *tech, *resource, *tmp;
	char meetmeopts[88], originator[TRIS_CHANNEL_NAME], *opts[0];
	char onlylistenopts[88];
	char *p_opts = meetmeopts;
	struct tris_flags flags = { 0 };
	char *confno;
	struct tris_app *app,*the_app;
	int res = 0, i = 0;
	int record_count = 0;
	char *dial_extns[MAX_DIALS];
	char recordingtmp[TRIS_MAX_EXTENSION] = "";
	char *ext, *callerid;
	char callinfo[80];
	int duration = 0;
	struct tris_filestream *fs;
	enum tris_dial_result state = 0;
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	char calling_uri[100];
	struct tris_page *cnf;
	char roomname[80], sql[256];
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);

 
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "This application requires at least one argument (destination(s) to page)\n");
		return -1;
	}

	if (!(app = pbx_findapp("MeetMe"))) {
		tris_log(LOG_WARNING, "There is no MeetMe application available!\n");
		return -1;
	};
	
	options = tris_strdupa(data);


	tris_copy_string(originator, chan->name, sizeof(originator));
	if ((tmp = strchr(originator, '-')))
		*tmp = '\0';

	confno = strsep(&options, ",");

	TRIS_LIST_LOCK(&pages);
	TRIS_LIST_TRAVERSE(&pages, cnf, list) {
		if (!strcmp(confno, cnf->roomno)) 
			return -1;
	}
	TRIS_LIST_UNLOCK(&pages);
	
	if (!(cnf = tris_calloc(1, sizeof(*cnf))))
		return -1;
	strcpy(cnf->roomno, confno);

	snprintf(callinfo, sizeof(callinfo), "Call-Info: MS,urgentcmd,%s", confno);
	the_app = pbx_findapp("SIPAddHeader");
	if (the_app)
		pbx_exec(chan, the_app, callinfo);
	
	tmp = strsep(&options, ",");
	
	if (options)
		tris_app_parse_options(page_opts, &flags, opts, options);

	snprintf(meetmeopts, sizeof(meetmeopts), "MeetMe,%s,%s%sd", confno, (tris_test_flag(&flags, PAGE_DUPLEX) ? "" : "m"),
		(tris_test_flag(&flags, PAGE_RECORD) ? "r" : "") );
	
	snprintf(onlylistenopts, sizeof(onlylistenopts), "MeetMe,%s,%sdm", confno,
		(tris_test_flag(&flags, PAGE_RECORD) ? "r" : "") );

	snprintf(sql, sizeof(sql), "SELECT roomname FROM urgentcmd_room WHERE roomno='%s'", confno);
	sql_select_query_execute(roomname, sql);
	//S_REPLACE(chan->cid.cid_name, tris_strdup("Conference")); 
	S_REPLACE(chan->cid.cid_name, tris_strdup(roomname));

	/* Go through parsing/calling each device */
	while ((tech = strsep(&tmp, "&"))) {

		struct tris_dial *dial = NULL;

		/* don't call the originating device */
		if (!strcasecmp(tech, originator))
			continue;
		
		if(*(tech+1) == '1')
			p_opts = meetmeopts;
		else
			p_opts = onlylistenopts;

		tech += 3;
		resource = tech;
		/* If no resource is available, continue on */
/*		if (!(resource = strchr(tech, '/'))) {
			tris_log(LOG_WARNING, "Incomplete destination '%s' supplied.\n", tech);
			continue;
		}
*/		
		/* Ensure device is not in use if skip option is enabled */
		if (tris_test_flag(&flags, PAGE_SKIP)) {
/*
			state = tris_device_state(tech);
			if (state == TRIS_DEVICE_NOT_INUSE) {
				tris_log(LOG_WARNING, "Destination '%s' has device state '%s'. start recording file\n", tech, devstate2str(state));
				tris_set_flag(&flags, PAGE_RECORD);

				tmpext = tris_strdupa(resource);
				ext = strchr(tmpext, '@');
				*ext = '\0';
				ext = tmpext;
				dial_extns[record_count++] = ext+1;
			} else {
				if (state == TRIS_DEVICE_UNKNOWN) {
					tris_log(LOG_WARNING, "Destination '%s' has device state '%s'. Paging anyway.\n", tech, devstate2str(state));
				}
			}
*/
		}
		
		//*resource++ = '\0';
		
		dial_extns[record_count++] = tris_strdupa(resource);
		sprintf(calling_uri,"%s@%s:5060", resource, tris_inet_ntoa(ourip));
		
		/* Create a dialing structure */
		if (!(dial = tris_dial_create())) {
			tris_log(LOG_WARNING, "Failed to create dialing structure.\n");
			continue;
		}

		/* Append technology and resource */
		//tris_dial_append(dial, tech, calling_uri);
		tris_dial_append(dial, "SIP", calling_uri);

		/* Set ANSWER_EXEC as global option */
		tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC, p_opts);

		/* Run this dial in async mode */
		tris_dial_run(dial, chan, 1, 0);

		/* Put in our dialing array */
		cnf->extens[cnf->pos] = resource;
		cnf->dials[cnf->pos++] = dial;
	}

	//tris_set_flag(&flags, PAGE_RECORD);
//	if (tris_test_flag(&flags, PAGE_RECORD)) {
 		snprintf(recordingtmp, sizeof(recordingtmp), "%s/urg-cmd-rec-%s", tris_config_TRIS_SPOOL_DIR, chan->uniqueid);
		pbx_builtin_setvar_helper(chan, "MEETME_RECORDINGFILE", recordingtmp);
//	}

	if (!tris_test_flag(&flags, PAGE_QUIET)) {
		res = tris_streamfile(chan, "beep", chan->language);
		if (!res)
			res = tris_waitstream(chan, "");
	}

/*	while(state != TRIS_DIAL_RESULT_ANSWERED 
			&& state != TRIS_DIAL_RESULT_TIMEOUT) {
		for (i=0; i<pos; i++) {
			state = tris_dial_state(dials[i]);
			if(state == TRIS_DIAL_RESULT_ANSWERED)
				break;
			else if(state == TRIS_DIAL_RESULT_TIMEOUT)
				break;
		}
		if (res = tris_waitstream(chan, ""))	// Channel is hung up 
			goto end_cmd;
	}
*/	
	if (!res) {
		TRIS_LIST_LOCK(&pages);
		TRIS_LIST_INSERT_HEAD(&pages, cnf, list);
		TRIS_LIST_UNLOCK(&pages);

		//tris_play_and_wait(chan, "beep");
		snprintf(meetmeopts, sizeof(meetmeopts), "%s,a%s%sdp(9)", confno, (tris_test_flag(&flags, PAGE_DUPLEX) ? "" : "t"), 
			(tris_test_flag(&flags, PAGE_RECORD) ? "r" : "") );
		pbx_exec(chan, app,meetmeopts );
	}
	
	tris_verbose("hsh commented....\n");
	if(tris_fileexists(recordingtmp, NULL, NULL) <= 0)
		goto end_cmd;
	
	fs = tris_openstream(chan, recordingtmp, chan->language);
	if(!fs)
		goto end_cmd;
	
	tris_seekstream(fs, 0, SEEK_END);
	duration = fs? tris_tellstream(fs) / 8000 : 0;
	//tris_closestream(fs);
	tris_verbose("broadcast mail\nrecording temp file : %s\ncount of users to leave message : duration = %d\n", recordingtmp, duration);

	for (i=0; i<cnf->pos; i++) {
		state = tris_dial_state(cnf->dials[i]);
		tris_verbose("  == %d\n", state);
		
		if (state != TRIS_DIAL_RESULT_ANSWERED) {
			tris_log(LOG_WARNING, "Destination '%s' has device state '%d'. start recording file\n", dial_extns[i], state);
//			tris_set_flag(&flags, PAGE_RECORD);
			ext = dial_extns[i];
			callerid = tris_strdupa(chan->cid.cid_num);

			if (!store_vmfile(chan, recordingtmp, NULL, ext, callerid, duration, "wav")) {
				tris_log(LOG_WARNING, "fail sending mail to '%s'\n", ext);
			}
		} else {
			if (state == TRIS_DEVICE_UNKNOWN) {
				tris_log(LOG_WARNING, "Destination '%s' has device state ''. Paging anyway.\n", dial_extns[i]);
			}
		}
	}


end_cmd:
	/* Go through each dial attempt cancelling, joining, and destroying */
	if(cnf->pos)
		sleep(2);
	for (i = 0; i < cnf->pos; i++) {
		struct tris_dial *dial = cnf->dials[i];

		/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
		tris_dial_join(dial);

		/* Hangup all channels */
		tris_dial_hangup(dial);

		/* Destroy dialing structure */
		tris_dial_destroy(dial);
		
		tris_verbose("  --  destroy dial(%d)\n", i);
	}

	/* remove recording temp file */
	tris_filedelete(recordingtmp, NULL);
	
	TRIS_LIST_REMOVE(&pages, cnf, list);
	tris_free(cnf);
	return -1;
}

static int videoconference_exec(struct tris_channel *chan, void *data)
{
	char vconfopts[88];
	char onlylistenopts[88];
	char *p_opts = vconfopts;
	struct tris_flags flags = { 0 };
	char confno[64], tmp[64], *info;
	struct tris_app *app,*the_app;
	int res = 0, i = 0;
	char callinfo[80];
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	char calling_uri[100];
	char roomname[80], sql[256];
	int pos = 0;
	struct tris_dial *dials[MAX_DIALS];
	
	struct odbc_obj *obj;
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	struct trisconf_obj q;
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);

	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	if(tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Not specified ConfNo\n");
		return -1;
	}

	info = tris_strdupa(data);

	snprintf(confno, sizeof(confno), "video%s", info);

	snprintf(sql, sizeof(sql), "SELECT sponseruid FROM videoconf_room WHERE roomno='%s'", confno);
	sql_select_query_execute(tmp, sql);

	if(tris_strlen_zero(tmp)) {
		tris_stream_and_wait(chan, "conference/retry_room_num", "");
		return -1;
	} else if(strcmp(tmp, chan->cid.cid_num)){
		tris_stream_and_wait(chan, "conference/you_cant_open_the_conf", "");
		return -1;
	}
	
 	if (!(app = pbx_findapp("Conference"))) {
		tris_log(LOG_WARNING, "There is no Conference application available!\n");
		return -1;
	};
	
	snprintf(callinfo, sizeof(callinfo), "Call-Info: MS,videoconf,%s", confno);
	the_app = pbx_findapp("SIPAddHeader");
	if (the_app)
		pbx_exec(chan, the_app, callinfo);
	
	snprintf(vconfopts, sizeof(vconfopts), "Conference,%s/0", confno); // can speak
	snprintf(onlylistenopts, sizeof(onlylistenopts), "Conference,%s/0L", confno); // can't speak

	snprintf(sql, sizeof(sql), "SELECT roomname FROM videoconf_room WHERE roomno='%s'", confno);
	sql_select_query_execute(roomname, sql);
	S_REPLACE(chan->cid.cid_name, tris_strdup(roomname));

	/* Go through parsing/calling each device */

	memset(&q, 0, sizeof(q));
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;

	snprintf(sql, sizeof(sql), "SELECT roomno, memberuid, mempermit FROM videoconf_member WHERE roomno = '%s'", confno);
	q.sql = sql;
	
	stmt = tris_odbc_prepare_and_execute(obj, trisconf_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	while ((SQLFetch(stmt)) != SQL_NO_DATA) {

		struct tris_dial *dial = NULL;

		/* don't call the originating device */
		if(!strcmp(q.mempermit, "1"))
			p_opts = vconfopts;
		else
			p_opts = onlylistenopts;

		sprintf(calling_uri,"%s@%s:5060", q.memberuid, tris_inet_ntoa(ourip));
		
		/* Create a dialing structure */
		if (!(dial = tris_dial_create())) {
			tris_log(LOG_WARNING, "Failed to create dialing structure.\n");
			continue;
		}

		/* Append technology and resource */
		//tris_dial_append(dial, tech, calling_uri);
		tris_dial_append(dial, "SIP", calling_uri);

		/* Set ANSWER_EXEC as global option */
		tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC, p_opts);

		/* Run this dial in async mode */
		tris_dial_run(dial, chan, 1, 0);

		/* Put in our dialing array */
		dials[pos++] = dial;
	}

	if (!tris_test_flag(&flags, PAGE_QUIET)) {
		res = tris_streamfile(chan, "beep", chan->language);
		if (!res)
			res = tris_waitstream(chan, "");
	}
	if (!res) {

		snprintf(vconfopts, sizeof(vconfopts), "%s/Mac", confno);
		pbx_exec(chan, app,vconfopts );
	}
	

	/* Go through each dial attempt cancelling, joining, and destroying */
	if(pos)
		sleep(2);
	for (i = 0; i < pos; i++) {
		struct tris_dial *dial = dials[i];

		/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
		tris_dial_join(dial);

		/* Hangup all channels */
		tris_dial_hangup(dial);

		/* Destroy dialing structure */
		tris_dial_destroy(dial);
		
		tris_verbose("  --  destroy dial(%d)\n", i);
	}

	/* remove recording temp file */
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	return -1;
}

static int unload_module(void)
{
	int res;
	res = tris_unregister_application(app_page);
	res |= tris_unregister_application(app_confpage);
	res |= tris_unregister_application(app_urgentcmd);
	res |= tris_unregister_application(app_videoconf);
	return res;
}

static int load_module(void)
{
	int res;
	res = tris_register_application(app_page, page_exec, page_synopsis, page_descrip);
	res |= tris_register_application(app_confpage, confpage_exec, confpage_synopsis, page_descrip);
	res |= tris_register_application(app_urgentcmd, urgentcmd_exec, cmd_synopsis, page_descrip);
	res |= tris_register_application(app_videoconf, videoconference_exec, videoconf_synopsis, "VideoConference(ConfNo)");
	res |= tris_register_application(app_cmdbroadcast, cmdbroadcast_exec, cmdbroadcast_synopsis, "CmdBroadcast");
	res |= tris_register_application(app_callconf, callconf_exec, callconf_synopsis, "CallConf");

	res |= tris_manager_register2("PageStatus", 0, 
				    action_pagestatus, "Status of participants in a page", mandescr_pagestatus);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Page Multiple Phones");

