/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2005, Holger Schurig
 *
 *
 * Ideas taken from other cdr_*.c files
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
 * \brief Store CDR records in a SQLite database.
 *
 * \author Holger Schurig <hs4233@mail.mn-solutions.de>
 * \extref SQLite http://www.sqlite.org/
 *
 * See also
 * \arg \ref Config_cdr
 * \arg http://www.sqlite.org/
 *
 * Creates the database and table on-the-fly
 * \ingroup cdr_drivers
 *
 * \note This module has been marked deprecated in favor for cdr_sqlite3_custom
 */

/*** MODULEINFO
	<depend>sqlite</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 158072 $")

#include <sqlite.h>

#include "trismedia/channel.h"
#include "trismedia/module.h"
#include "trismedia/utils.h"
#include "trismedia/paths.h"

#define LOG_UNIQUEID	0
#define LOG_USERFIELD	0

/* When you change the DATE_FORMAT, be sure to change the CHAR(19) below to something else */
#define DATE_FORMAT "%Y-%m-%d %T"

static char *name = "sqlite";
static sqlite* db = NULL;

TRIS_MUTEX_DEFINE_STATIC(sqlite_lock);

/*! \brief SQL table format */
static char sql_create_table[] = "CREATE TABLE cdr ("
"	AcctId		INTEGER PRIMARY KEY,"
"	clid		VARCHAR(80),"
"	src		VARCHAR(80),"
"	dst		VARCHAR(80),"
"	dcontext	VARCHAR(80),"
"	channel		VARCHAR(80),"
"	dstchannel	VARCHAR(80),"
"	lastapp		VARCHAR(80),"
"	lastdata	VARCHAR(80),"
"	start		CHAR(19),"
"	answer		CHAR(19),"
"	end		CHAR(19),"
"	duration	INTEGER,"
"	billsec		INTEGER,"
"	disposition	INTEGER,"
"	amaflags	INTEGER,"
"	accountcode	VARCHAR(20)"
#if LOG_UNIQUEID
"	,uniqueid	VARCHAR(32)"
#endif
#if LOG_USERFIELD
"	,userfield	VARCHAR(255)"
#endif
");";

static void format_date(char *buffer, size_t length, struct timeval *when)
{
	struct tris_tm tm;

	tris_localtime(when, &tm, NULL);
	tris_strftime(buffer, length, DATE_FORMAT, &tm);
}

static int sqlite_log(struct tris_cdr *cdr)
{
	int res = 0;
	char *zErr = 0;
	char startstr[80], answerstr[80], endstr[80];
	int count;

	tris_mutex_lock(&sqlite_lock);

	format_date(startstr, sizeof(startstr), &cdr->start);
	format_date(answerstr, sizeof(answerstr), &cdr->answer);
	format_date(endstr, sizeof(endstr), &cdr->end);

	for(count=0; count<5; count++) {
		res = sqlite_exec_printf(db,
			"INSERT INTO cdr ("
				"clid,src,dst,dcontext,"
				"channel,dstchannel,lastapp,lastdata, "
				"start,answer,end,"
				"duration,billsec,disposition,amaflags, "
				"accountcode"
#				if LOG_UNIQUEID
				",uniqueid"
#				endif
#				if LOG_USERFIELD
				",userfield"
#				endif
			") VALUES ("
				"'%q', '%q', '%q', '%q', "
				"'%q', '%q', '%q', '%q', "
				"'%q', '%q', '%q', "
				"%d, %d, %d, %d, "
				"'%q'"
#				if LOG_UNIQUEID
				",'%q'"
#				endif
#				if LOG_USERFIELD
				",'%q'"
#				endif
			")", NULL, NULL, &zErr,
				cdr->clid, cdr->src, cdr->dst, cdr->dcontext,
				cdr->channel, cdr->dstchannel, cdr->lastapp, cdr->lastdata,
				startstr, answerstr, endstr,
				cdr->duration, cdr->billsec, cdr->disposition, cdr->amaflags,
				cdr->accountcode
#				if LOG_UNIQUEID
				,cdr->uniqueid
#				endif
#				if LOG_USERFIELD
				,cdr->userfield
#				endif
			);
		if (res != SQLITE_BUSY && res != SQLITE_LOCKED)
			break;
		usleep(200);
	}

	if (zErr) {
		tris_log(LOG_ERROR, "cdr_sqlite: %s\n", zErr);
		tris_free(zErr);
	}

	tris_mutex_unlock(&sqlite_lock);
	return res;
}

static int unload_module(void)
{
	if (db)
		sqlite_close(db);
	tris_cdr_unregister(name);
	return 0;
}

static int load_module(void)
{
	char *zErr;
	char fn[PATH_MAX];
	int res;

	tris_log(LOG_WARNING, "This module has been marked deprecated in favor of "
		"using cdr_sqlite3_custom. (May be removed after Trismedia 1.6)\n");

	/* is the database there? */
	snprintf(fn, sizeof(fn), "%s/cdr.db", tris_config_TRIS_LOG_DIR);
	db = sqlite_open(fn, TRIS_FILE_MODE, &zErr);
	if (!db) {
		tris_log(LOG_ERROR, "cdr_sqlite: %s\n", zErr);
		tris_free(zErr);
		return -1;
	}

	/* is the table there? */
	res = sqlite_exec(db, "SELECT COUNT(AcctId) FROM cdr;", NULL, NULL, NULL);
	if (res) {
		res = sqlite_exec(db, sql_create_table, NULL, NULL, &zErr);
		if (res) {
			tris_log(LOG_ERROR, "cdr_sqlite: Unable to create table 'cdr': %s\n", zErr);
			tris_free(zErr);
			goto err;
		}

		/* TODO: here we should probably create an index */
	}

	res = tris_cdr_register(name, tris_module_info->description, sqlite_log);
	if (res) {
		tris_log(LOG_ERROR, "Unable to register SQLite CDR handling\n");
		return -1;
	}
	return 0;

err:
	if (db)
		sqlite_close(db);
	return -1;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "SQLite CDR Backend");
