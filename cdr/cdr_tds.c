/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2006, Digium, Inc.
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
 * \brief FreeTDS CDR logger
 *
 * See also
 * \arg \ref Config_cdr
 * \arg http://www.freetds.org/
 * \ingroup cdr_drivers
 */

/*! \verbatim
 *
 * Table Structure for `cdr`
 *
 * Created on: 05/20/2004 16:16
 * Last changed on: 07/27/2004 20:01

CREATE TABLE [dbo].[cdr] (
	[accountcode] [varchar] (20) NULL ,
	[src] [varchar] (80) NULL ,
	[dst] [varchar] (80) NULL ,
	[dcontext] [varchar] (80) NULL ,
	[clid] [varchar] (80) NULL ,
	[channel] [varchar] (80) NULL ,
	[dstchannel] [varchar] (80) NULL ,
	[lastapp] [varchar] (80) NULL ,
	[lastdata] [varchar] (80) NULL ,
	[start] [datetime] NULL ,
	[answer] [datetime] NULL ,
	[end] [datetime] NULL ,
	[duration] [int] NULL ,
	[billsec] [int] NULL ,
	[disposition] [varchar] (20) NULL ,
	[amaflags] [varchar] (16) NULL ,
	[uniqueid] [varchar] (32) NULL ,
	[userfield] [varchar] (256) NULL
) ON [PRIMARY]

\endverbatim

*/

/*** MODULEINFO
	<depend>freetds</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 159818 $")

#include <time.h>
#include <math.h>

#include "trismedia/config.h"
#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/module.h"

#include <sqlfront.h>
#include <sybdb.h>

#define DATE_FORMAT "%Y/%m/%d %T"

static char *name = "FreeTDS (MSSQL)";
static char *config = "cdr_tds.conf";

struct cdr_tds_config {
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(hostname);
		TRIS_STRING_FIELD(database);
		TRIS_STRING_FIELD(username);
		TRIS_STRING_FIELD(password);
		TRIS_STRING_FIELD(table);
		TRIS_STRING_FIELD(charset);
		TRIS_STRING_FIELD(language);
	);
	DBPROCESS *dbproc;
	unsigned int connected:1;
	unsigned int has_userfield:1;
};

TRIS_MUTEX_DEFINE_STATIC(tds_lock);

static struct cdr_tds_config *settings;

static char *anti_injection(const char *, int);
static void get_date(char *, size_t len, struct timeval);

static int execute_and_consume(DBPROCESS *dbproc, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static int mssql_connect(void);
static int mssql_disconnect(void);

static int tds_log(struct tris_cdr *cdr)
{
	char start[80], answer[80], end[80];
	char *accountcode, *src, *dst, *dcontext, *clid, *channel, *dstchannel, *lastapp, *lastdata, *uniqueid, *userfield = NULL;
	RETCODE erc;
	int res = -1;
	int attempt = 1;

	accountcode = anti_injection(cdr->accountcode, 20);
	src         = anti_injection(cdr->src, 80);
	dst         = anti_injection(cdr->dst, 80);
	dcontext    = anti_injection(cdr->dcontext, 80);
	clid        = anti_injection(cdr->clid, 80);
	channel     = anti_injection(cdr->channel, 80);
	dstchannel  = anti_injection(cdr->dstchannel, 80);
	lastapp     = anti_injection(cdr->lastapp, 80);
	lastdata    = anti_injection(cdr->lastdata, 80);
	uniqueid    = anti_injection(cdr->uniqueid, 32);

	get_date(start, sizeof(start), cdr->start);
	get_date(answer, sizeof(answer), cdr->answer);
	get_date(end, sizeof(end), cdr->end);

	tris_mutex_lock(&tds_lock);

	if (settings->has_userfield) {
		userfield = anti_injection(cdr->userfield, TRIS_MAX_USER_FIELD);
	}

retry:
	/* Ensure that we are connected */
	if (!settings->connected) {
		tris_log(LOG_NOTICE, "Attempting to reconnect to %s (Attempt %d)\n", settings->hostname, attempt);
		if (mssql_connect()) {
			/* Connect failed */
			if (attempt++ < 3) {
				goto retry;
			}
			goto done;
		}
	}

	if (settings->has_userfield) {
		erc = dbfcmd(settings->dbproc,
					 "INSERT INTO %s "
					 "("
					 "accountcode, src, dst, dcontext, clid, channel, "
					 "dstchannel, lastapp, lastdata, start, answer, [end], duration, "
					 "billsec, disposition, amaflags, uniqueid, userfield"
					 ") "
					 "VALUES "
					 "("
					 "'%s', '%s', '%s', '%s', '%s', '%s', "
					 "'%s', '%s', '%s', %s, %s, %s, %ld, "
					 "%ld, '%s', '%s', '%s', '%s'"
					 ")",
					 settings->table,
					 accountcode, src, dst, dcontext, clid, channel,
					 dstchannel, lastapp, lastdata, start, answer, end, cdr->duration,
					 cdr->billsec, tris_cdr_disp2str(cdr->disposition), tris_cdr_flags2str(cdr->amaflags), uniqueid,
					 userfield
			);
	} else {
		erc = dbfcmd(settings->dbproc,
					 "INSERT INTO %s "
					 "("
					 "accountcode, src, dst, dcontext, clid, channel, "
					 "dstchannel, lastapp, lastdata, start, answer, [end], duration, "
					 "billsec, disposition, amaflags, uniqueid"
					 ") "
					 "VALUES "
					 "("
					 "'%s', '%s', '%s', '%s', '%s', '%s', "
					 "'%s', '%s', '%s', %s, %s, %s, %ld, "
					 "%ld, '%s', '%s', '%s'"
					 ")",
					 settings->table,
					 accountcode, src, dst, dcontext, clid, channel,
					 dstchannel, lastapp, lastdata, start, answer, end, cdr->duration,
					 cdr->billsec, tris_cdr_disp2str(cdr->disposition), tris_cdr_flags2str(cdr->amaflags), uniqueid
			);
	}

	if (erc == FAIL) {
		if (attempt++ < 3) {
			tris_log(LOG_NOTICE, "Failed to build INSERT statement, retrying...\n");
			mssql_disconnect();
			goto retry;
		} else {
			tris_log(LOG_ERROR, "Failed to build INSERT statement, no CDR was logged.\n");
			goto done;
		}
	}

	if (dbsqlexec(settings->dbproc) == FAIL) {
		if (attempt++ < 3) {
			tris_log(LOG_NOTICE, "Failed to execute INSERT statement, retrying...\n");
			mssql_disconnect();
			goto retry;
		} else {
			tris_log(LOG_ERROR, "Failed to execute INSERT statement, no CDR was logged.\n");
			goto done;
		}
	}

	/* Consume any results we might get back (this is more of a sanity check than
	 * anything else, since an INSERT shouldn't return results). */
	while (dbresults(settings->dbproc) != NO_MORE_RESULTS) {
		while (dbnextrow(settings->dbproc) != NO_MORE_ROWS);
	}

	res = 0;

done:
	tris_mutex_unlock(&tds_lock);

	tris_free(accountcode);
	tris_free(src);
	tris_free(dst);
	tris_free(dcontext);
	tris_free(clid);
	tris_free(channel);
	tris_free(dstchannel);
	tris_free(lastapp);
	tris_free(lastdata);
	tris_free(uniqueid);

	if (userfield) {
		tris_free(userfield);
	}

	return res;
}

static char *anti_injection(const char *str, int len)
{
	/* Reference to http://www.nextgenss.com/papers/advanced_sql_injection.pdf */
	char *buf;
	char *buf_ptr, *srh_ptr;
	char *known_bad[] = {"select", "insert", "update", "delete", "drop", ";", "--", "\0"};
	int idx;

	if (!(buf = tris_calloc(1, len + 1))) {
		tris_log(LOG_ERROR, "Out of memory\n");
		return NULL;
	}

	buf_ptr = buf;

	/* Escape single quotes */
	for (; *str && strlen(buf) < len; str++) {
		if (*str == '\'') {
			*buf_ptr++ = '\'';
		}
		*buf_ptr++ = *str;
	}
	*buf_ptr = '\0';

	/* Erase known bad input */
	for (idx = 0; *known_bad[idx]; idx++) {
		while ((srh_ptr = strcasestr(buf, known_bad[idx]))) {
			memmove(srh_ptr, srh_ptr + strlen(known_bad[idx]), strlen(srh_ptr + strlen(known_bad[idx])) + 1);
		}
	}

	return buf;
}

static void get_date(char *dateField, size_t len, struct timeval when)
{
	/* To make sure we have date variable if not insert null to SQL */
	if (!tris_tvzero(when)) {
		struct tris_tm tm;
		tris_localtime(&when, &tm, NULL);
		tris_strftime(dateField, len, "'" DATE_FORMAT "'", &tm);
	} else {
		tris_copy_string(dateField, "null", len);
	}
}

static int execute_and_consume(DBPROCESS *dbproc, const char *fmt, ...)
{
	va_list ap;
	char *buffer;

	va_start(ap, fmt);
	if (tris_vasprintf(&buffer, fmt, ap) < 0) {
		va_end(ap);
		return 1;
	}
	va_end(ap);

	if (dbfcmd(dbproc, buffer) == FAIL) {
		free(buffer);
		return 1;
	}

	free(buffer);

	if (dbsqlexec(dbproc) == FAIL) {
		return 1;
	}

	/* Consume the result set (we don't really care about the result, though) */
	while (dbresults(dbproc) != NO_MORE_RESULTS) {
		while (dbnextrow(dbproc) != NO_MORE_ROWS);
	}

	return 0;
}

static int mssql_disconnect(void)
{
	if (settings->dbproc) {
		dbclose(settings->dbproc);
		settings->dbproc = NULL;
	}

	settings->connected = 0;

	return 0;
}

static int mssql_connect(void)
{
	LOGINREC *login;

	if ((login = dblogin()) == NULL) {
		tris_log(LOG_ERROR, "Unable to allocate login structure for db-lib\n");
		return -1;
	}

	DBSETLAPP(login,     "TSQL");
	DBSETLUSER(login,    (char *) settings->username);
	DBSETLPWD(login,     (char *) settings->password);
	DBSETLCHARSET(login, (char *) settings->charset);
	DBSETLNATLANG(login, (char *) settings->language);

	if ((settings->dbproc = dbopen(login, (char *) settings->hostname)) == NULL) {
		tris_log(LOG_ERROR, "Unable to connect to %s\n", settings->hostname);
		dbloginfree(login);
		return -1;
	}

	dbloginfree(login);

	if (dbuse(settings->dbproc, (char *) settings->database) == FAIL) {
		tris_log(LOG_ERROR, "Unable to select database %s\n", settings->database);
		goto failed;
	}

	if (execute_and_consume(settings->dbproc, "SELECT 1 FROM [%s]", settings->table)) {
		tris_log(LOG_ERROR, "Unable to find table '%s'\n", settings->table);
		goto failed;
	}

	/* Check to see if we have a userfield column in the table */
	if (execute_and_consume(settings->dbproc, "SELECT userfield FROM [%s] WHERE 1 = 0", settings->table)) {
		tris_log(LOG_NOTICE, "Unable to find 'userfield' column in table '%s'\n", settings->table);
		settings->has_userfield = 0;
	} else {
		settings->has_userfield = 1;
	}

	settings->connected = 1;

	return 0;

failed:
	dbclose(settings->dbproc);
	settings->dbproc = NULL;
	return -1;
}

static int tds_unload_module(void)
{
	if (settings) {
		tris_mutex_lock(&tds_lock);
		mssql_disconnect();
		tris_mutex_unlock(&tds_lock);

		tris_string_field_free_memory(settings);
		tris_free(settings);
	}

	tris_cdr_unregister(name);

	dbexit();

	return 0;
}

static int tds_error_handler(DBPROCESS *dbproc, int severity, int dberr, int oserr, char *dberrstr, char *oserrstr)
{
	tris_log(LOG_ERROR, "%s (%d)\n", dberrstr, dberr);

	if (oserr != DBNOERR) {
		tris_log(LOG_ERROR, "%s (%d)\n", oserrstr, oserr);
	}

	return INT_CANCEL;
}

static int tds_message_handler(DBPROCESS *dbproc, DBINT msgno, int msgstate, int severity, char *msgtext, char *srvname, char *procname, int line)
{
	tris_debug(1, "Msg %d, Level %d, State %d, Line %d\n", msgno, severity, msgstate, line);
	tris_log(LOG_NOTICE, "%s\n", msgtext);

	return 0;
}

static int tds_load_module(int reload)
{
	struct tris_config *cfg;
	const char *ptr = NULL;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	cfg = tris_config_load(config, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_NOTICE, "Unable to load TDS config for CDRs: %s\n", config);
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED)
		return 0;

	if (!tris_variable_browse(cfg, "global")) {
		/* nothing configured */
		tris_config_destroy(cfg);
		return 0;
	}

	tris_mutex_lock(&tds_lock);

	/* Clear out any existing settings */
	tris_string_field_init(settings, 0);

	ptr = tris_variable_retrieve(cfg, "global", "hostname");
	if (ptr) {
		tris_string_field_set(settings, hostname, ptr);
	} else {
		tris_log(LOG_ERROR, "Failed to connect: Database server hostname not specified.\n");
		goto failed;
	}

	ptr = tris_variable_retrieve(cfg, "global", "dbname");
	if (ptr) {
		tris_string_field_set(settings, database, ptr);
	} else {
		tris_log(LOG_ERROR, "Failed to connect: Database dbname not specified.\n");
		goto failed;
	}

	ptr = tris_variable_retrieve(cfg, "global", "user");
	if (ptr) {
		tris_string_field_set(settings, username, ptr);
	} else {
		tris_log(LOG_ERROR, "Failed to connect: Database dbuser not specified.\n");
		goto failed;
	}

	ptr = tris_variable_retrieve(cfg, "global", "password");
	if (ptr) {
		tris_string_field_set(settings, password, ptr);
	} else {
		tris_log(LOG_ERROR, "Failed to connect: Database password not specified.\n");
		goto failed;
	}

	ptr = tris_variable_retrieve(cfg, "global", "charset");
	if (ptr) {
		tris_string_field_set(settings, charset, ptr);
	} else {
		tris_string_field_set(settings, charset, "iso_1");
	}

	ptr = tris_variable_retrieve(cfg, "global", "language");
	if (ptr) {
		tris_string_field_set(settings, language, ptr);
	} else {
		tris_string_field_set(settings, language, "us_english");
	}

	ptr = tris_variable_retrieve(cfg, "global", "table");
	if (ptr) {
		tris_string_field_set(settings, table, ptr);
	} else {
		tris_log(LOG_NOTICE, "Table name not specified, using 'cdr' by default.\n");
		tris_string_field_set(settings, table, "cdr");
	}

	mssql_disconnect();

	if (mssql_connect()) {
		/* We failed to connect (mssql_connect takes care of logging it) */
		goto failed;
	}

	tris_mutex_unlock(&tds_lock);
	tris_config_destroy(cfg);

	return 1;

failed:
	tris_mutex_unlock(&tds_lock);
	tris_config_destroy(cfg);

	return 0;
}

static int reload(void)
{
	return tds_load_module(1);
}

static int load_module(void)
{
	if (dbinit() == FAIL) {
		tris_log(LOG_ERROR, "Failed to initialize FreeTDS db-lib\n");
		return TRIS_MODULE_LOAD_DECLINE;
	}

	dberrhandle(tds_error_handler);
	dbmsghandle(tds_message_handler);

	settings = tris_calloc(1, sizeof(*settings));

	if (!settings || tris_string_field_init(settings, 256)) {
		if (settings) {
			tris_free(settings);
			settings = NULL;
		}
		dbexit();
		return TRIS_MODULE_LOAD_DECLINE;
	}

	if (!tds_load_module(0)) {
		tris_string_field_free_memory(settings);
		tris_free(settings);
		settings = NULL;
		dbexit();
		return TRIS_MODULE_LOAD_DECLINE;
	}

	tris_cdr_register(name, tris_module_info->description, tds_log);

	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	return tds_unload_module();
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "FreeTDS CDR Backend",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
