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
 * \brief Full-featured outgoing call spool support
 * 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <dirent.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_SPOOL_DIR */
#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/logger.h"
#include "trismedia/channel.h"
#include "trismedia/callerid.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/utils.h"
#include "trismedia/options.h"
#include "trismedia/res_odbc.h"
#include "trismedia/acl.h"

/*
 * pbx_spool is similar in spirit to qcall, but with substantially enhanced functionality...
 * The spool file contains a header 
 */

enum {
	/*! Always delete the call file after a call succeeds or the
	 * maximum number of retries is exceeded, even if the
	 * modification time of the call file is in the future.
	 */
	SPOOL_FLAG_ALWAYS_DELETE = (1 << 0),
	/* Don't unlink the call file after processing, move in qdonedir */
	SPOOL_FLAG_ARCHIVE = (1 << 1)
};

static char qdir[255];
static char qdonedir[255];

struct outgoing {
	int retries;                              /*!< Current number of retries */
	int maxretries;                           /*!< Maximum number of retries permitted */
	int retrytime;                            /*!< How long to wait between retries (in seconds) */
	int waittime;                             /*!< How long to wait for an answer */
	long callingpid;                          /*!< PID which is currently calling */
	int format;                               /*!< Formats (codecs) for this call */
	TRIS_DECLARE_STRING_FIELDS (
		TRIS_STRING_FIELD(fn);                 /*!< File name of call file */
		TRIS_STRING_FIELD(tech);               /*!< Which channel technology to use for outgoing call */
		TRIS_STRING_FIELD(dest);               /*!< Which device/line to use for outgoing call */
		TRIS_STRING_FIELD(app);                /*!< If application: Application name */
		TRIS_STRING_FIELD(data);               /*!< If application: Application data */
		TRIS_STRING_FIELD(exten);              /*!< If extension/context/priority: Extension in dialplan */
		TRIS_STRING_FIELD(context);            /*!< If extension/context/priority: Dialplan context */
		TRIS_STRING_FIELD(cid_num);            /*!< CallerID Information: Number/extension */
		TRIS_STRING_FIELD(cid_name);           /*!< CallerID Information: Name */
		TRIS_STRING_FIELD(account);            /*!< account code */
		TRIS_STRING_FIELD(roomno);
	);
	int priority;                             /*!< If extension/context/priority: Dialplan priority */
	struct tris_variable *vars;                /*!< Variables and Functions */
	int maxlen;                               /*!< Maximum length of call */
	struct tris_flags options;                 /*!< options */
};

struct spool_obj {
	char *sql;
	char roomno[2];
	char listener_uid[32];
	SQLLEN err;
};

static SQLHSTMT spool_prepare(struct odbc_obj *obj, void *data)
{
	struct spool_obj *q = data;
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

	SQLBindCol(sth, 1, SQL_C_ULONG, q->roomno, sizeof(q->roomno), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->listener_uid, sizeof(q->listener_uid), &q->err);
//	SQLBindCol(sth, 3, SQL_C_CHAR, q->var_name, sizeof(q->var_name), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

static int init_outgoing(struct outgoing *o)
{
	o->priority = 1;
	o->retrytime = 300;
	o->waittime = 45;
	o->format = TRIS_FORMAT_SLINEAR;
	tris_set_flag(&o->options, SPOOL_FLAG_ALWAYS_DELETE);
	if (tris_string_field_init(o, 128)) {
		return -1;
	}
	return 0;
}

static void free_outgoing(struct outgoing *o)
{
	if (o->vars) {
		tris_variables_destroy(o->vars);
	}
	tris_string_field_free_memory(o);
	tris_free(o);
}

static struct outgoing* duplicate_outgoing(struct outgoing *src)
{
	struct outgoing* tmp;
	if (!(tmp = tris_calloc(1, sizeof(*tmp)))) {
		tris_log(LOG_WARNING, "Out of memory ;(\n");
		return 0;
	}
	
	init_outgoing(tmp);

	tmp->retries = src->retries;
	tmp->maxretries = src->maxretries;
	tmp->retrytime = src->retrytime;
	tmp->waittime = src->waittime;
	tmp->callingpid = src->callingpid;
	tmp->format = src->format;
	tmp->priority = src->priority;
	tmp->maxlen= src->maxlen;

	tris_string_field_set(tmp, fn, src->fn);
	tris_string_field_set(tmp, tech, src->tech);
	tris_string_field_set(tmp, dest, src->dest);
	tris_string_field_set(tmp, app, src->app);
	tris_string_field_set(tmp, data, src->data);
	tris_string_field_set(tmp, exten, src->exten);
	tris_string_field_set(tmp, context, src->context);
	tris_string_field_set(tmp, cid_num, src->cid_num);
	tris_string_field_set(tmp, cid_name, src->cid_name);
	tris_string_field_set(tmp, account, src->account);
	tris_string_field_set(tmp, roomno, src->roomno);

	return tmp;

}
static int apply_outgoing(struct outgoing *o, char *fn, FILE *f)
{
	char buf[256];
	char *c, *c2;
	int lineno = 0;
	struct tris_variable *var, *last = o->vars;

	while (last && last->next) {
		last = last->next;
	}

	while(fgets(buf, sizeof(buf), f)) {
		lineno++;
		/* Trim comments */
		c = buf;
		while ((c = strchr(c, '#'))) {
			if ((c == buf) || (*(c-1) == ' ') || (*(c-1) == '\t'))
				*c = '\0';
			else
				c++;
		}

		c = buf;
		while ((c = strchr(c, ';'))) {
			if ((c > buf) && (c[-1] == '\\')) {
				memmove(c - 1, c, strlen(c) + 1);
				c++;
			} else {
				*c = '\0';
				break;
			}
		}

		/* Trim trailing white space */
		while(!tris_strlen_zero(buf) && buf[strlen(buf) - 1] < 33)
			buf[strlen(buf) - 1] = '\0';
		if (!tris_strlen_zero(buf)) {
			c = strchr(buf, ':');
			if (c) {
				*c = '\0';
				c++;
				while ((*c) && (*c < 33))
					c++;
#if 0
				printf("'%s' is '%s' at line %d\n", buf, c, lineno);
#endif
				if (!strcasecmp(buf, "channel")) {
					if ((c2 = strchr(c, '/'))) {
						*c2 = '\0';
						c2++;
						tris_string_field_set(o, tech, c);
						tris_string_field_set(o, dest, c2);
					} else {
						tris_log(LOG_NOTICE, "Channel should be in form Tech/Dest at line %d of %s\n", lineno, fn);
					}
				} else if (!strcasecmp(buf, "callerid")) {
					char cid_name[80] = {0}, cid_num[80] = {0};
					tris_callerid_split(c, cid_name, sizeof(cid_name), cid_num, sizeof(cid_num));
					tris_string_field_set(o, cid_num, cid_num);
					tris_string_field_set(o, cid_name, cid_name);
				} else if (!strcasecmp(buf, "application")) {
					tris_string_field_set(o, app, c);
				} else if (!strcasecmp(buf, "data")) {
					tris_string_field_set(o, data, c);
				} else if (!strcasecmp(buf, "maxretries")) {
					if (sscanf(c, "%30d", &o->maxretries) != 1) {
						tris_log(LOG_WARNING, "Invalid max retries at line %d of %s\n", lineno, fn);
						o->maxretries = 0;
					}
				} else if (!strcasecmp(buf, "codecs")) {
					tris_parse_allow_disallow(NULL, &o->format, c, 1);
				} else if (!strcasecmp(buf, "roomno")) {
					tris_string_field_set(o, roomno, c);
				} else if (!strcasecmp(buf, "context")) {
					tris_string_field_set(o, context, c);
				} else if (!strcasecmp(buf, "extension")) {
					tris_string_field_set(o, exten, c);
				} else if (!strcasecmp(buf, "priority")) {
					if ((sscanf(c, "%30d", &o->priority) != 1) || (o->priority < 1)) {
						tris_log(LOG_WARNING, "Invalid priority at line %d of %s\n", lineno, fn);
						o->priority = 1;
					}
				} else if (!strcasecmp(buf, "retrytime")) {
					if ((sscanf(c, "%30d", &o->retrytime) != 1) || (o->retrytime < 1)) {
						tris_log(LOG_WARNING, "Invalid retrytime at line %d of %s\n", lineno, fn);
						o->retrytime = 300;
					}
				} else if (!strcasecmp(buf, "waittime")) {
					if ((sscanf(c, "%30d", &o->waittime) != 1) || (o->waittime < 1)) {
						tris_log(LOG_WARNING, "Invalid waittime at line %d of %s\n", lineno, fn);
						o->waittime = 45;
					}
				} else if (!strcasecmp(buf, "retry")) {
					o->retries++;
				} else if (!strcasecmp(buf, "startretry")) {
					if (sscanf(c, "%30ld", &o->callingpid) != 1) {
						tris_log(LOG_WARNING, "Unable to retrieve calling PID!\n");
						o->callingpid = 0;
					}
				} else if (!strcasecmp(buf, "endretry") || !strcasecmp(buf, "abortretry")) {
					o->callingpid = 0;
					o->retries++;
				} else if (!strcasecmp(buf, "delayedretry")) {
				} else if (!strcasecmp(buf, "setvar") || !strcasecmp(buf, "set")) {
					c2 = c;
					strsep(&c2, "=");
					if (c2) {
						var = tris_variable_new(c, c2, fn);
						if (var) {
							/* Always insert at the end, because some people want to treat the spool file as a script */
							if (last) {
								last->next = var;
							} else {
								o->vars = var;
							}
							last = var;
						}
					} else
						tris_log(LOG_WARNING, "Malformed \"%s\" argument.  Should be \"%s: variable=value\"\n", buf, buf);
				} else if (!strcasecmp(buf, "account")) {
					tris_string_field_set(o, account, c);
				} else if (!strcasecmp(buf, "alwaysdelete")) {
					tris_set2_flag(&o->options, tris_true(c), SPOOL_FLAG_ALWAYS_DELETE);
				} else if (!strcasecmp(buf, "archive")) {
					tris_set2_flag(&o->options, tris_true(c), SPOOL_FLAG_ARCHIVE);
				} else {
					tris_log(LOG_WARNING, "Unknown keyword '%s' at line %d of %s\n", buf, lineno, fn);
				}
			} else
				tris_log(LOG_NOTICE, "Syntax error at line %d of %s\n", lineno, fn);
		}
	}
	tris_string_field_set(o, fn, fn);
//	if (tris_strlen_zero(o->tech) || tris_strlen_zero(o->dest) || (tris_strlen_zero(o->app) && tris_strlen_zero(o->exten))) {
	if (tris_strlen_zero(o->app) && tris_strlen_zero(o->exten)) {
		tris_log(LOG_WARNING, "At least one of app or extension must be specified, along with tech and dest in file %s\n", fn);
		return -1;
	}
	return 0;
}

static void safe_append(struct outgoing *o, time_t now, char *s)
{
	int fd;
	FILE *f;
	struct utimbuf tbuf;

	if ((fd = open(o->fn, O_WRONLY | O_APPEND)) < 0)
		return;

	if ((f = fdopen(fd, "a"))) {
		fprintf(f, "\n%s: %ld %d (%ld)\n", s, (long)tris_mainpid, o->retries, (long) now);
		fclose(f);
	} else
		close(fd);

	/* Update the file time */
	tbuf.actime = now;
	tbuf.modtime = now + o->retrytime;
	if (utime(o->fn, &tbuf))
		tris_log(LOG_WARNING, "Unable to set utime on %s: %s\n", o->fn, strerror(errno));
}

/*!
 * \brief Remove a call file from the outgoing queue optionally moving it in the archive dir
 *
 * \param o the pointer to outgoing struct
 * \param status the exit status of the call. Can be "Completed", "Failed" or "Expired"
 */
static int remove_from_queue(struct outgoing *o, const char *status)
{
	int fd;
	FILE *f;
	char newfn[256];
	const char *bname;

	if (!tris_test_flag(&o->options, SPOOL_FLAG_ALWAYS_DELETE)) {
		struct stat current_file_status;

		if (!stat(o->fn, &current_file_status)) {
			if (time(NULL) < current_file_status.st_mtime)
				return 0;
		}
	}

	if (!tris_test_flag(&o->options, SPOOL_FLAG_ARCHIVE)) {
		unlink(o->fn);
		return 0;
	}

	if (tris_mkdir(qdonedir, 0777)) {
		tris_log(LOG_WARNING, "Unable to create queue directory %s -- outgoing spool archiving disabled\n", qdonedir);
		unlink(o->fn);
		return -1;
	}

	if ((fd = open(o->fn, O_WRONLY | O_APPEND))) {
		if ((f = fdopen(fd, "a"))) {
			fprintf(f, "Status: %s\n", status);
			fclose(f);
		} else
			close(fd);
	}

	if (!(bname = strrchr(o->fn, '/')))
		bname = o->fn;
	else
		bname++;	
	snprintf(newfn, sizeof(newfn), "%s/%s", qdonedir, bname);
	/* a existing call file the archive dir is overwritten */
	unlink(newfn);
	if (rename(o->fn, newfn) != 0) {
		unlink(o->fn);
		return -1;
	} else
		return 0;
}

static void *attempt_thread(void *data)
{
	struct outgoing *o = data;
	int res, reason;
	tris_verbose("  **  %s/%s\n", o->tech, o->dest);

	if (!tris_strlen_zero(o->app)) {
		tris_verb(3, "Attempting call on %s/%s for application %s(%s) (Retry %d)\n", o->tech, o->dest, o->app, o->data, o->retries);
		res = tris_pbx_outgoing_app(o->tech, o->format, (void *) o->dest, o->waittime * 1000, o->app, o->data, &reason, 2 /* wait to finish */, o->cid_num, o->cid_name, o->vars, o->account, NULL);
		o->vars = NULL;
	} else {
		tris_verb(3, "Attempting call on %s/%s for %s@%s:%d (Retry %d)\n", o->tech, o->dest, o->exten, o->context,o->priority, o->retries);
		res = tris_pbx_outgoing_exten(o->tech, o->format, (void *) o->dest, o->waittime * 1000, o->context, o->exten, o->priority, &reason, 2 /* wait to finish */, o->cid_num, o->cid_name, o->vars, o->account, NULL);
		o->vars = NULL;
	}
	if (res) {
		tris_log(LOG_NOTICE, "Call failed to go through, reason (%d) %s\n", reason, tris_channel_reason2str(reason));
		if (o->retries >= o->maxretries + 1) {
			/* Max retries exceeded */
			tris_log(LOG_EVENT, "Queued call to %s/%s expired without completion after %d attempt%s\n", o->tech, o->dest, o->retries - 1, ((o->retries - 1) != 1) ? "s" : "");
//			remove_from_queue(o, "Expired");
		} else {
			/* Notate that the call is still active */
			safe_append(o, time(NULL), "EndRetry");
		}
	} else {
		tris_log(LOG_NOTICE, "Call completed to %s/%s\n", o->tech, o->dest);
		tris_log(LOG_EVENT, "Queued call to %s/%s completed\n", o->tech, o->dest);
//		remove_from_queue(o, "Completed");
	}
	free_outgoing(o);
	return NULL;
}

static void launch_service(struct outgoing *o)
{
	pthread_t t;
	int ret;

	if ((ret = tris_pthread_create_detached(&t, NULL, attempt_thread, o))) {
		tris_log(LOG_WARNING, "Unable to create thread :( (returned error: %d)\n", ret);
		free_outgoing(o);
	}
}

static int run_outgoing_info(const char *database, const char *table, struct outgoing *o)
{
	int res = 0;
	int x = 0;
	struct odbc_obj *obj;
	char sqlbuf[1024] = "";
	char *sql = sqlbuf;
	size_t sqlleft = sizeof(sqlbuf);
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	struct spool_obj q;
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	struct outgoing *tmp = NULL;
	char tmpdst[1024];

	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);
	
	memset(&q, 0, sizeof(q));

	obj = tris_odbc_request_obj(database, 0);
	if (!obj)
		return 0;
	
	if (sscanf(o->roomno, "%d", &x) != 1)
		tris_log(LOG_WARNING, "Failed to read roomno!\n");
	
	tris_build_string(&sql, &sqlleft, "SELECT roomno, listener_uid FROM %s ", table);
	tris_build_string(&sql, &sqlleft, "WHERE roomno=%d", x);
	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, spool_prepare, &q);

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

	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		
		tris_string_field_set(o, tech, "SIP");
		snprintf(tmpdst, sizeof(tmpdst), "%s@%s:5060", q.listener_uid, tris_inet_ntoa(ourip));
		tris_string_field_set(o, dest, tmpdst);

		tmp = duplicate_outgoing(o);

		launch_service(tmp);
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	return 1;
}

static int scan_service(char *fn, time_t now, time_t atime)
{
	struct outgoing *o = NULL;
	FILE *f;
	int res = 0;

	if (!(o = tris_calloc(1, sizeof(*o)))) {
		tris_log(LOG_WARNING, "Out of memory ;(\n");
		return -1;
	}
	
	if (init_outgoing(o)) {
		/* No need to call free_outgoing here since we know the failure
		 * was to allocate string fields and no variables have been allocated
		 * yet.
		 */
		tris_free(o);
		return -1;
	}

	/* Attempt to open the file */
	if (!(f = fopen(fn, "r+"))) {
		remove_from_queue(o, "Failed");
		free_outgoing(o);
		tris_log(LOG_WARNING, "Unable to open %s: %s, deleting\n", fn, strerror(errno));
		return -1;
	}

	/* Read in and verify the contents */
	if (apply_outgoing(o, fn, f)) {
		remove_from_queue(o, "Failed");
		free_outgoing(o);
		tris_log(LOG_WARNING, "Invalid file contents in %s, deleting\n", fn);
		fclose(f);
		return -1;
	}
	
#if 0
	printf("Filename: %s, Retries: %d, max: %d\n", fn, o->retries, o->maxretries);
#endif
	fclose(f);
	if (o->retries <= o->maxretries) {
		now += o->retrytime;
		if (o->callingpid && (o->callingpid == tris_mainpid)) {
			safe_append(o, time(NULL), "DelayedRetry");
			tris_log(LOG_DEBUG, "Delaying retry since we're currently running '%s'\n", o->fn);
			free_outgoing(o);
		} else {
			/* Increment retries */
			o->retries++;
			/* If someone else was calling, they're presumably gone now
			   so abort their retry and continue as we were... */
			if (o->callingpid)
				safe_append(o, time(NULL), "AbortRetry");
			
			safe_append(o, now, "StartRetry");
			//tris_copy_string(o->context, "defualt", sizeof(o->context));
			//o->priority = 1;
			if (o->roomno && !tris_strlen_zero(o->roomno)) {
				run_outgoing_info("trisdb", "outgoing_listeners", o );
				remove_from_queue(o, "Completed");
				free_outgoing(o);
			} else {
				launch_service(o);
				remove_from_queue(o, "Completed");
			}
		}
		res = now;
	} else {
		tris_log(LOG_EVENT, "Queued call to %s/%s expired without completion after %d attempt%s\n", o->tech, o->dest, o->retries - 1, ((o->retries - 1) != 1) ? "s" : "");
		remove_from_queue(o, "Expired");
		free_outgoing(o);
	}

	return res;
}

static void *scan_thread(void *unused)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	char fn[256];
	int res;
	time_t last = 0, next = 0, now;
	struct timespec ts = { .tv_sec = 1 };
  
	while (!tris_fully_booted) {
		nanosleep(&ts, NULL);
	}

	for(;;) {
		/* Wait a sec */
		nanosleep(&ts, NULL);
		time(&now);

		if (stat(qdir, &st)) {
			tris_log(LOG_WARNING, "Unable to stat %s\n", qdir);
			continue;
		}

		/* Make sure it is time for us to execute our check */
		if ((st.st_mtime == last) && (next && (next > now)))
			continue;
		
#if 0
		printf("atime: %ld, mtime: %ld, ctime: %ld\n", st.st_atime, st.st_mtime, st.st_ctime);
		printf("Ooh, something changed / timeout\n");
#endif
		next = 0;
		last = st.st_mtime;

		if (!(dir = opendir(qdir))) {
			tris_log(LOG_WARNING, "Unable to open directory %s: %s\n", qdir, strerror(errno));
			continue;
		}

		while ((de = readdir(dir))) {
			snprintf(fn, sizeof(fn), "%s/%s", qdir, de->d_name);
			if (stat(fn, &st)) {
				tris_log(LOG_WARNING, "Unable to stat %s: %s\n", fn, strerror(errno));
				continue;
			}
			if (!S_ISREG(st.st_mode))
				continue;
			if (st.st_mtime <= now) {
				res = scan_service(fn, now, st.st_atime);
				if (res > 0) {
					/* Update next service time */
					if (!next || (res < next)) {
						next = res;
					}
				} else if (res) {
					tris_log(LOG_WARNING, "Failed to scan service '%s'\n", fn);
				} else if (!next) {
					/* Expired entry: must recheck on the next go-around */
					next = st.st_mtime;
				}
			} else {
				/* Update "next" update if necessary */
				if (!next || (st.st_mtime < next))
					next = st.st_mtime;
			}
		}
		closedir(dir);
	}
	return NULL;
}

static void *scan_monitor(void *unused)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	char fn[256];
	time_t now, interval = 1*24*3600;
	struct timespec ts = { .tv_sec = 1 };
	const char *mdir = "/usr/local/spool/trismedia/monitor/";
  
	while (!tris_fully_booted) {
		nanosleep(&ts, NULL);
	}

	for(;;) {
		/* Wait a sec */
		nanosleep(&ts, NULL);
		time(&now);

		if (stat(mdir, &st)) {
			tris_log(LOG_WARNING, "Unable to stat %s\n", mdir);
			continue;
		}

		if (!(dir = opendir(mdir))) {
			tris_log(LOG_WARNING, "Unable to open directory %s: %s\n", mdir, strerror(errno));
			continue;
		}

		while ((de = readdir(dir))) {
			snprintf(fn, sizeof(fn), "%s/%s", mdir, de->d_name);
			if (stat(fn, &st)) {
				tris_log(LOG_WARNING, "Unable to stat %s: %s\n", fn, strerror(errno));
				continue;
			}
			if (!S_ISREG(st.st_mode))
				continue;
			if (st.st_mtime < now - interval) {
				unlink(fn);
				continue;
			}
		}
		closedir(dir);
	}
	return NULL;
}

static int unload_module(void)
{
	return -1;
}

static int load_module(void)
{
	pthread_t thread, mthread;
	int ret;
	snprintf(qdir, sizeof(qdir), "%s/%s", tris_config_TRIS_SPOOL_DIR, "outgoing");
	if (tris_mkdir(qdir, 0777)) {
		tris_log(LOG_WARNING, "Unable to create queue directory %s -- outgoing spool disabled\n", qdir);
		return TRIS_MODULE_LOAD_DECLINE;
	}
	snprintf(qdonedir, sizeof(qdir), "%s/%s", tris_config_TRIS_SPOOL_DIR, "outgoing_done");

	if ((ret = tris_pthread_create_detached_background(&thread, NULL, scan_thread, NULL))) {
		tris_log(LOG_WARNING, "Unable to create thread :( (returned error: %d)\n", ret);
		return TRIS_MODULE_LOAD_FAILURE;
	}

	if ((ret = tris_pthread_create_detached_background(&mthread, NULL, scan_monitor, NULL))) {
		tris_log(LOG_WARNING, "Unable to create thread :( (returned error: %d)\n", ret);
		return TRIS_MODULE_LOAD_FAILURE;
	}

	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Outgoing Spool Support");
