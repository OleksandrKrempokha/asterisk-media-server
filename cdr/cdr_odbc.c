/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2003-2005, Digium, Inc.
 *
 * Brian K. West <brian@bkw.org>
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
 * \brief ODBC CDR Backend
 *
 * \author Brian K. West <brian@bkw.org>
 *
 * See also:
 * \arg http://www.unixodbc.org
 * \arg \ref Config_cdr
 * \ingroup cdr_drivers
 */

/*** MODULEINFO
	<depend>generic_odbc</depend>
	<depend>ltdl</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 168734 $")

#include <time.h>

#include "trismedia/config.h"
#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/module.h"
#include "trismedia/res_odbc.h"

#define DATE_FORMAT "%Y-%m-%d %T"

static char *name = "ODBC";
static char *config_file = "cdr_odbc.conf";
static char *dsn = NULL, *table = NULL;

enum {
	CONFIG_LOGUNIQUEID =       1 << 0,
	CONFIG_USEGMTIME =         1 << 1,
	CONFIG_DISPOSITIONSTRING = 1 << 2,
};

static struct tris_flags config = { 0 };

static SQLHSTMT execute_cb(struct odbc_obj *obj, void *data)
{
	struct tris_cdr *cdr = data;
	SQLRETURN ODBC_res;
	char sqlcmd[2048] = "", timestr[128];
	struct tris_tm tm;
	SQLHSTMT stmt;

	tris_localtime(&cdr->start, &tm, tris_test_flag(&config, CONFIG_USEGMTIME) ? "GMT" : NULL);
	tris_strftime(timestr, sizeof(timestr), DATE_FORMAT, &tm);

	if (tris_test_flag(&config, CONFIG_LOGUNIQUEID)) {
		snprintf(sqlcmd,sizeof(sqlcmd),"INSERT INTO %s "
		"(calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,"
		"lastdata,duration,billsec,disposition,amaflags,accountcode,uniqueid,userfield) "
		"VALUES ({ts '%s'},?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", table, timestr);
	} else {
		snprintf(sqlcmd,sizeof(sqlcmd),"INSERT INTO %s "
		"(calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,"
		"duration,billsec,disposition,amaflags,accountcode) "
		"VALUES ({ts '%s'},?,?,?,?,?,?,?,?,?,?,?,?,?)", table, timestr);
	}

	ODBC_res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);

	if ((ODBC_res != SQL_SUCCESS) && (ODBC_res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(11, "cdr_odbc: Failure in AllocStatement %d\n", ODBC_res);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->clid), 0, cdr->clid, 0, NULL);
	SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->src), 0, cdr->src, 0, NULL);
	SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->dst), 0, cdr->dst, 0, NULL);
	SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->dcontext), 0, cdr->dcontext, 0, NULL);
	SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->channel), 0, cdr->channel, 0, NULL);
	SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->dstchannel), 0, cdr->dstchannel, 0, NULL);
	SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->lastapp), 0, cdr->lastapp, 0, NULL);
	SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->lastdata), 0, cdr->lastdata, 0, NULL);
	SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &cdr->duration, 0, NULL);
	SQLBindParameter(stmt, 10, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &cdr->billsec, 0, NULL);
	if (tris_test_flag(&config, CONFIG_DISPOSITIONSTRING))
		SQLBindParameter(stmt, 11, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(tris_cdr_disp2str(cdr->disposition)) + 1, 0, tris_cdr_disp2str(cdr->disposition), 0, NULL);
	else
		SQLBindParameter(stmt, 11, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &cdr->disposition, 0, NULL);
	SQLBindParameter(stmt, 12, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &cdr->amaflags, 0, NULL);
	SQLBindParameter(stmt, 13, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->accountcode), 0, cdr->accountcode, 0, NULL);

	if (tris_test_flag(&config, CONFIG_LOGUNIQUEID)) {
		SQLBindParameter(stmt, 14, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->uniqueid), 0, cdr->uniqueid, 0, NULL);
		SQLBindParameter(stmt, 15, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, sizeof(cdr->userfield), 0, cdr->userfield, 0, NULL);
	}

	ODBC_res = SQLExecDirect(stmt, (unsigned char *)sqlcmd, SQL_NTS);

	if ((ODBC_res != SQL_SUCCESS) && (ODBC_res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(11, "cdr_odbc: Error in ExecDirect: %d\n", ODBC_res);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	return stmt;
}


static int odbc_log(struct tris_cdr *cdr)
{
	struct odbc_obj *obj = tris_odbc_request_obj(dsn, 0);
	SQLHSTMT stmt;

	if (!obj) {
		tris_log(LOG_ERROR, "Unable to retrieve database handle.  CDR failed.\n");
		return -1;
	}

	stmt = tris_odbc_direct_execute(obj, execute_cb, cdr);
	if (stmt) {
		SQLLEN rows = 0;

		SQLRowCount(stmt, &rows);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);

		if (rows == 0)
			tris_log(LOG_WARNING, "CDR successfully ran, but inserted 0 rows?\n");
	} else
		tris_log(LOG_ERROR, "CDR direct execute failed\n");
	tris_odbc_release_obj(obj);
	return 0;
}

static int odbc_load_module(int reload)
{
	int res = 0;
	struct tris_config *cfg;
	struct tris_variable *var;
	const char *tmp;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	do {
		cfg = tris_config_load(config_file, config_flags);
		if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
			tris_log(LOG_WARNING, "cdr_odbc: Unable to load config for ODBC CDR's: %s\n", config_file);
			res = TRIS_MODULE_LOAD_DECLINE;
			break;
		} else if (cfg == CONFIG_STATUS_FILEUNCHANGED)
			break;

		var = tris_variable_browse(cfg, "global");
		if (!var) {
			/* nothing configured */
			break;
		}

		if ((tmp = tris_variable_retrieve(cfg, "global", "dsn")) == NULL) {
			tris_log(LOG_WARNING, "cdr_odbc: dsn not specified.  Assuming trismediadb\n");
			tmp = "trismediadb";
		}
		if (dsn)
			tris_free(dsn);
		dsn = tris_strdup(tmp);
		if (dsn == NULL) {
			res = -1;
			break;
		}

		if (((tmp = tris_variable_retrieve(cfg, "global", "dispositionstring"))) && tris_true(tmp))
			tris_set_flag(&config, CONFIG_DISPOSITIONSTRING);
		else
			tris_clear_flag(&config, CONFIG_DISPOSITIONSTRING);

		if (((tmp = tris_variable_retrieve(cfg, "global", "loguniqueid"))) && tris_true(tmp)) {
			tris_set_flag(&config, CONFIG_LOGUNIQUEID);
			tris_debug(1, "cdr_odbc: Logging uniqueid\n");
		} else {
			tris_clear_flag(&config, CONFIG_LOGUNIQUEID);
			tris_debug(1, "cdr_odbc: Not logging uniqueid\n");
		}

		if (((tmp = tris_variable_retrieve(cfg, "global", "usegmtime"))) && tris_true(tmp)) {
			tris_set_flag(&config, CONFIG_USEGMTIME);
			tris_debug(1, "cdr_odbc: Logging in GMT\n");
		} else {
			tris_clear_flag(&config, CONFIG_USEGMTIME);
			tris_debug(1, "cdr_odbc: Logging in local time\n");
		}

		if ((tmp = tris_variable_retrieve(cfg, "global", "table")) == NULL) {
			tris_log(LOG_WARNING, "cdr_odbc: table not specified.  Assuming cdr\n");
			tmp = "cdr";
		}
		if (table)
			tris_free(table);
		table = tris_strdup(tmp);
		if (table == NULL) {
			res = -1;
			break;
		}

		tris_verb(3, "cdr_odbc: dsn is %s\n", dsn);
		tris_verb(3, "cdr_odbc: table is %s\n", table);

		res = tris_cdr_register(name, tris_module_info->description, odbc_log);
		if (res) {
			tris_log(LOG_ERROR, "cdr_odbc: Unable to register ODBC CDR handling\n");
		}
	} while (0);

	if (cfg && cfg != CONFIG_STATUS_FILEUNCHANGED && cfg != CONFIG_STATUS_FILEINVALID)
		tris_config_destroy(cfg);
	return res;
}

static int load_module(void)
{
	return odbc_load_module(0);
}

static int unload_module(void)
{
	tris_cdr_unregister(name);

	if (dsn) {
		tris_verb(11, "cdr_odbc: free dsn\n");
		tris_free(dsn);
	}
	if (table) {
		tris_verb(11, "cdr_odbc: free table\n");
		tris_free(table);
	}

	return 0;
}

static int reload(void)
{
	return odbc_load_module(1);
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "ODBC CDR Backend",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
