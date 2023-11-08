/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2005, 2006 Tilghman Lesher
 * Copyright (c) 2008 Digium, Inc.
 *
 * Tilghman Lesher <func_odbc__200508@the-tilghman.com>
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

/*!
 * \file
 *
 * \brief ODBC lookups
 *
 * \author Tilghman Lesher <func_odbc__200508@the-tilghman.com>
 *
 * \ingroup functions
 */

/*** MODULEINFO
	<depend>res_odbc</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/module.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/config.h"
#include "trismedia/res_odbc.h"
#include "trismedia/app.h"
#include "trismedia/cli.h"
#include "trismedia/strings.h"

/*** DOCUMENTATION
	<function name="ODBC_FETCH" language="en_US">
		<synopsis>
			Fetch a row from a multirow query.
		</synopsis>
		<syntax>
			<parameter name="result-id" required="true" />
		</syntax>
		<description>
			<para>For queries which are marked as mode=multirow, the original 
			query returns a <replaceable>result-id</replaceable> from which results 
			may be fetched.  This function implements the actual fetch of the results.</para>
			<para>This also sets <variable>ODBC_FETCH_STATUS</variable>.</para>
			<variablelist>
				<variable name="ODBC_FETCH_STATUS">
					<value name="SUCESS">
						If rows are available.
					</value>
					<value name="FAILURE">
						If no rows are available.
					</value>
				</variable>
			</variablelist>
		</description>
	</function>
	<application name="ODBCFinish" language="en_US">
		<synopsis>
			Clear the resultset of a sucessful multirow query.
		</synopsis>
		<syntax>
			<parameter name="result-id" required="true" />
		</syntax>
		<description>
			<para>For queries which are marked as mode=multirow, this will clear 
			any remaining rows of the specified resultset.</para>
		</description>
	</application>
	<function name="SQL_ESC" language="en_US">
		<synopsis>
			Escapes single ticks for use in SQL statements.
		</synopsis>
		<syntax>
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Used in SQL templates to escape data which may contain single ticks 
			<literal>'</literal> which are otherwise used to delimit data.</para>
		  	<para>Example: SELECT foo FROM bar WHERE baz='${SQL_ESC(${ARG1})}'</para>
		</description>
	</function>
 ***/

static char *config = "func_odbc.conf";

enum {
	OPT_ESCAPECOMMAS =	(1 << 0),
	OPT_MULTIROW     =	(1 << 1),
} odbc_option_flags;

struct acf_odbc_query {
	TRIS_RWLIST_ENTRY(acf_odbc_query) list;
	char readhandle[5][30];
	char writehandle[5][30];
	char sql_read[2048];
	char sql_write[2048];
	char sql_insert[2048];
	unsigned int flags;
	int rowlimit;
	struct tris_custom_function *acf;
};

static void odbc_datastore_free(void *data);

struct tris_datastore_info odbc_info = {
	.type = "FUNC_ODBC",
	.destroy = odbc_datastore_free,
};

/* For storing each result row */
struct odbc_datastore_row {
	TRIS_LIST_ENTRY(odbc_datastore_row) list;
	char data[0];
};

/* For storing each result set */
struct odbc_datastore {
	TRIS_LIST_HEAD(, odbc_datastore_row);
	char names[0];
};

TRIS_RWLIST_HEAD_STATIC(queries, acf_odbc_query);

static int resultcount = 0;

TRIS_THREADSTORAGE(sql_buf);
TRIS_THREADSTORAGE(sql2_buf);
TRIS_THREADSTORAGE(coldata_buf);
TRIS_THREADSTORAGE(colnames_buf);

static void odbc_datastore_free(void *data)
{
	struct odbc_datastore *result = data;
	struct odbc_datastore_row *row;
	TRIS_LIST_LOCK(result);
	while ((row = TRIS_LIST_REMOVE_HEAD(result, list))) {
		tris_free(row);
	}
	TRIS_LIST_UNLOCK(result);
	TRIS_LIST_HEAD_DESTROY(result);
	tris_free(result);
}

static SQLHSTMT generic_execute(struct odbc_obj *obj, void *data)
{
	int res;
	char *sql = data;
	SQLHSTMT stmt;

	res = SQLAllocHandle (SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL Alloc Handle failed (%d)!\n", res);
		return NULL;
	}

	res = SQLExecDirect(stmt, (unsigned char *)sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		if (res == SQL_ERROR) {
			int i;
			SQLINTEGER nativeerror=0, numfields=0;
			SQLSMALLINT diagbytes=0;
			unsigned char state[10], diagnostic[256];

			SQLGetDiagField(SQL_HANDLE_STMT, stmt, 1, SQL_DIAG_NUMBER, &numfields, SQL_IS_INTEGER, &diagbytes);
			for (i = 0; i < numfields; i++) {
				SQLGetDiagRec(SQL_HANDLE_STMT, stmt, i + 1, state, &nativeerror, diagnostic, sizeof(diagnostic), &diagbytes);
				tris_log(LOG_WARNING, "SQL Execute returned an error %d: %s: %s (%d)\n", res, state, diagnostic, diagbytes);
				if (i > 10) {
					tris_log(LOG_WARNING, "Oh, that was good.  There are really %d diagnostics?\n", (int)numfields);
					break;
				}
			}
		}

		tris_log(LOG_WARNING, "SQL Exec Direct failed (%d)![%s]\n", res, sql);
		SQLCloseCursor(stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	return stmt;
}

/*
 * Master control routine
 */
static int acf_odbc_write(struct tris_channel *chan, const char *cmd, char *s, const char *value)
{
	struct odbc_obj *obj = NULL;
	struct acf_odbc_query *query;
	char *t, varname[15];
	int i, dsn, bogus_chan = 0;
	int transactional = 0;
	TRIS_DECLARE_APP_ARGS(values,
		TRIS_APP_ARG(field)[100];
	);
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(field)[100];
	);
	SQLHSTMT stmt = NULL;
	SQLLEN rows=0;
	struct tris_str *buf = tris_str_thread_get(&sql_buf, 16);
	struct tris_str *insertbuf = tris_str_thread_get(&sql2_buf, 16);
	const char *status = "FAILURE";

	if (!buf) {
		return -1;
	}

	TRIS_RWLIST_RDLOCK(&queries);
	TRIS_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, cmd)) {
			break;
		}
	}

	if (!query) {
		tris_log(LOG_ERROR, "No such function '%s'\n", cmd);
		TRIS_RWLIST_UNLOCK(&queries);
		pbx_builtin_setvar_helper(chan, "ODBCSTATUS", status);
		return -1;
	}

	if (!chan) {
		if ((chan = tris_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc")))
			bogus_chan = 1;
	}

	if (chan)
		tris_autoservice_start(chan);

	tris_str_make_space(&buf, strlen(query->sql_write) * 2 + 300);
	tris_str_make_space(&insertbuf, strlen(query->sql_insert) * 2 + 300);

	/* Parse our arguments */
	t = value ? tris_strdupa(value) : "";

	if (!s || !t) {
		tris_log(LOG_ERROR, "Out of memory\n");
		TRIS_RWLIST_UNLOCK(&queries);
		if (chan)
			tris_autoservice_stop(chan);
		if (bogus_chan) {
			tris_channel_free(chan);
		} else {
			pbx_builtin_setvar_helper(chan, "ODBCSTATUS", status);
		}
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, s);
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[i]);
	}

	/* Parse values, just like arguments */
	TRIS_STANDARD_APP_ARGS(values, t);
	for (i = 0; i < values.argc; i++) {
		snprintf(varname, sizeof(varname), "VAL%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, values.field[i]);
	}

	/* Additionally set the value as a whole (but push an empty string if value is NULL) */
	pbx_builtin_pushvar_helper(chan, "VALUE", value ? value : "");

	tris_str_substitute_variables(&buf, 0, chan, query->sql_write);
	tris_str_substitute_variables(&insertbuf, 0, chan, query->sql_insert);

	/* Restore prior values */
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_setvar_helper(chan, varname, NULL);
	}

	for (i = 0; i < values.argc; i++) {
		snprintf(varname, sizeof(varname), "VAL%d", i + 1);
		pbx_builtin_setvar_helper(chan, varname, NULL);
	}
	pbx_builtin_setvar_helper(chan, "VALUE", NULL);

	/*!\note
	 * Okay, this part is confusing.  Transactions belong to a single database
	 * handle.  Therefore, when working with transactions, we CANNOT failover
	 * to multiple DSNs.  We MUST have a single handle all the way through the
	 * transaction, or else we CANNOT enforce atomicity.
	 */
	for (dsn = 0; dsn < 5; dsn++) {
		if (transactional) {
			/* This can only happen second time through or greater. */
			tris_log(LOG_WARNING, "Transactions do not work well with multiple DSNs for 'writehandle'\n");
		}

		if (!tris_strlen_zero(query->writehandle[dsn])) {
			if ((obj = tris_odbc_retrieve_transaction_obj(chan, query->writehandle[dsn]))) {
				transactional = 1;
			} else {
				obj = tris_odbc_request_obj(query->writehandle[dsn], 0);
				transactional = 0;
			}
			if (obj && (stmt = tris_odbc_direct_execute(obj, generic_execute, tris_str_buffer(buf)))) {
				break;
			}
		}

		if (obj && !transactional) {
			tris_odbc_release_obj(obj);
		}
	}

	if (stmt && rows == 0 && tris_str_strlen(insertbuf) != 0) {
		SQLCloseCursor(stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		for (dsn = 0; dsn < 5; dsn++) {
			if (!tris_strlen_zero(query->writehandle[dsn])) {
				obj = tris_odbc_request_obj(query->writehandle[dsn], 0);
				if (obj) {
					stmt = tris_odbc_direct_execute(obj, generic_execute, tris_str_buffer(insertbuf));
				}
			}
			if (stmt) {
				status = "FAILOVER";
				SQLRowCount(stmt, &rows);
				break;
			}
		}
	} else if (stmt) {
		status = "SUCCESS";
		SQLRowCount(stmt, &rows);
	}

	TRIS_RWLIST_UNLOCK(&queries);

	/* Output the affected rows, for all cases.  In the event of failure, we
	 * flag this as -1 rows.  Note that this is different from 0 affected rows
	 * which would be the case if we succeeded in our query, but the values did
	 * not change. */
	snprintf(varname, sizeof(varname), "%d", (int)rows);
	pbx_builtin_setvar_helper(chan, "ODBCROWS", varname);
	pbx_builtin_setvar_helper(chan, "ODBCSTATUS", status);

	if (stmt) {
		SQLCloseCursor(stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	}
	if (obj && !transactional) {
		tris_odbc_release_obj(obj);
		obj = NULL;
	}

	if (chan)
		tris_autoservice_stop(chan);
	if (bogus_chan)
		tris_channel_free(chan);

	return 0;
}

static int acf_odbc_read(struct tris_channel *chan, const char *cmd, char *s, char *buf, size_t len)
{
	struct odbc_obj *obj = NULL;
	struct acf_odbc_query *query;
	char varname[15], rowcount[12] = "-1";
	struct tris_str *colnames = tris_str_thread_get(&colnames_buf, 16);
	int res, x, y, buflen = 0, escapecommas, rowlimit = 1, dsn, bogus_chan = 0;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(field)[100];
	);
	SQLHSTMT stmt = NULL;
	SQLSMALLINT colcount=0;
	SQLLEN indicator;
	SQLSMALLINT collength;
	struct odbc_datastore *resultset = NULL;
	struct odbc_datastore_row *row = NULL;
	struct tris_str *sql = tris_str_thread_get(&sql_buf, 16);
	const char *status = "FAILURE";

	if (!sql) {
		pbx_builtin_setvar_helper(chan, "ODBCSTATUS", status);
		return -1;
	}

	tris_str_reset(colnames);

	TRIS_RWLIST_RDLOCK(&queries);
	TRIS_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, cmd)) {
			break;
		}
	}

	if (!query) {
		tris_log(LOG_ERROR, "No such function '%s'\n", cmd);
		TRIS_RWLIST_UNLOCK(&queries);
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		pbx_builtin_setvar_helper(chan, "ODBCSTATUS", status);
		return -1;
	}

	if (!chan) {
		if ((chan = tris_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc"))) {
			bogus_chan = 1;
		}
	}

	if (chan) {
		tris_autoservice_start(chan);
	}

	TRIS_STANDARD_APP_ARGS(args, s);
	for (x = 0; x < args.argc; x++) {
		snprintf(varname, sizeof(varname), "ARG%d", x + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[x]);
	}

	tris_str_substitute_variables(&sql, 0, chan, query->sql_read);

	/* Restore prior values */
	for (x = 0; x < args.argc; x++) {
		snprintf(varname, sizeof(varname), "ARG%d", x + 1);
		pbx_builtin_setvar_helper(chan, varname, NULL);
	}

	/* Save these flags, so we can release the lock */
	escapecommas = tris_test_flag(query, OPT_ESCAPECOMMAS);
	if (tris_test_flag(query, OPT_MULTIROW)) {
		resultset = tris_calloc(1, sizeof(*resultset));
		TRIS_LIST_HEAD_INIT(resultset);
		if (query->rowlimit) {
			rowlimit = query->rowlimit;
		} else {
			rowlimit = INT_MAX;
		}
	}
	TRIS_RWLIST_UNLOCK(&queries);

	for (dsn = 0; dsn < 5; dsn++) {
		if (!tris_strlen_zero(query->readhandle[dsn])) {
			obj = tris_odbc_request_obj(query->readhandle[dsn], 0);
			if (obj) {
				stmt = tris_odbc_direct_execute(obj, generic_execute, tris_str_buffer(sql));
			}
		}
		if (stmt) {
			break;
		}
	}

	if (!stmt) {
		tris_log(LOG_ERROR, "Unable to execute query [%s]\n", tris_str_buffer(sql));
		if (obj) {
			tris_odbc_release_obj(obj);
			obj = NULL;
		}
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		if (chan) {
			tris_autoservice_stop(chan);
		}
		if (bogus_chan) {
			tris_channel_free(chan);
		}
		return -1;
	}

	res = SQLNumResultCols(stmt, &colcount);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", tris_str_buffer(sql));
		SQLCloseCursor(stmt);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		obj = NULL;
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		if (chan) {
			tris_autoservice_stop(chan);
		}
		if (bogus_chan) {
			tris_channel_free(chan);
		}
		return -1;
	}

	res = SQLFetch(stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		int res1 = -1;
		if (res == SQL_NO_DATA) {
			tris_verb(4, "Found no rows [%s]\n", tris_str_buffer(sql));
			res1 = 0;
			buf[0] = '\0';
			tris_copy_string(rowcount, "0", sizeof(rowcount));
			status = "NODATA";
		} else {
			tris_log(LOG_WARNING, "Error %d in FETCH [%s]\n", res, tris_str_buffer(sql));
			status = "FETCHERROR";
		}
		SQLCloseCursor(stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		obj = NULL;
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		pbx_builtin_setvar_helper(chan, "ODBCSTATUS", status);
		if (chan)
			tris_autoservice_stop(chan);
		if (bogus_chan)
			tris_channel_free(chan);
		return res1;
	}

	status = "SUCCESS";

	for (y = 0; y < rowlimit; y++) {
		buf[0] = '\0';
		for (x = 0; x < colcount; x++) {
			int i;
			struct tris_str *coldata = tris_str_thread_get(&coldata_buf, 16);
			char *ptrcoldata;

			if (y == 0) {
				char colname[256];
				SQLULEN maxcol;

				res = SQLDescribeCol(stmt, x + 1, (unsigned char *)colname, sizeof(colname), &collength, NULL, &maxcol, NULL, NULL);
				tris_debug(3, "Got collength of %d and maxcol of %d for column '%s' (offset %d)\n", (int)collength, (int)maxcol, colname, x);
				if (((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) || collength == 0) {
					snprintf(colname, sizeof(colname), "field%d", x);
				}

				tris_str_make_space(&coldata, maxcol + 1);

				if (tris_str_strlen(colnames)) {
					tris_str_append(&colnames, 0, ",");
				}
				tris_str_append_escapecommas(&colnames, 0, colname, sizeof(colname));

				if (resultset) {
					void *tmp = tris_realloc(resultset, sizeof(*resultset) + tris_str_strlen(colnames) + 1);
					if (!tmp) {
						tris_log(LOG_ERROR, "No space for a new resultset?\n");
						tris_free(resultset);
						SQLCloseCursor(stmt);
						SQLFreeHandle(SQL_HANDLE_STMT, stmt);
						tris_odbc_release_obj(obj);
						obj = NULL;
						pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
						pbx_builtin_setvar_helper(chan, "ODBCSTATUS", "MEMERROR");
						if (chan)
							tris_autoservice_stop(chan);
						if (bogus_chan)
							tris_channel_free(chan);
						return -1;
					}
					resultset = tmp;
					strcpy((char *)resultset + sizeof(*resultset), tris_str_buffer(colnames));
				}
			}

			buflen = strlen(buf);
			res = tris_odbc_tris_str_SQLGetData(&coldata, -1, stmt, x + 1, SQL_CHAR, &indicator);
			if (indicator == SQL_NULL_DATA) {
				tris_debug(3, "Got NULL data\n");
				tris_str_reset(coldata);
				res = SQL_SUCCESS;
			}

			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", tris_str_buffer(sql));
				y = -1;
				buf[0] = '\0';
				goto end_acf_read;
			}

			tris_debug(2, "Got coldata of '%s'\n", tris_str_buffer(coldata));

			if (x) {
				buf[buflen++] = ',';
			}

			/* Copy data, encoding '\' and ',' for the argument parser */
			ptrcoldata = tris_str_buffer(coldata);
			for (i = 0; i < tris_str_strlen(coldata); i++) {
				if (escapecommas && (ptrcoldata[i] == '\\' || ptrcoldata[i] == ',')) {
					buf[buflen++] = '\\';
				}
				buf[buflen++] = ptrcoldata[i];

				if (buflen >= len - 2) {
					break;
				}

				if (ptrcoldata[i] == '\0') {
					break;
				}
			}

			buf[buflen] = '\0';
			tris_debug(2, "buf is now set to '%s'\n", buf);
		}
		tris_debug(2, "buf is now set to '%s'\n", buf);

		if (resultset) {
			row = tris_calloc(1, sizeof(*row) + buflen + 1);
			if (!row) {
				tris_log(LOG_ERROR, "Unable to allocate space for more rows in this resultset.\n");
				status = "MEMERROR";
				goto end_acf_read;
			}
			strcpy((char *)row + sizeof(*row), buf);
			TRIS_LIST_INSERT_TAIL(resultset, row, list);

			/* Get next row */
			res = SQLFetch(stmt);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				if (res != SQL_NO_DATA) {
					tris_log(LOG_WARNING, "Error %d in FETCH [%s]\n", res, tris_str_buffer(sql));
				}
				/* Number of rows in the resultset */
				y++;
				break;
			}
		}
	}

end_acf_read:
	snprintf(rowcount, sizeof(rowcount), "%d", y);
	pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
	pbx_builtin_setvar_helper(chan, "ODBCSTATUS", status);
	pbx_builtin_setvar_helper(chan, "~ODBCFIELDS~", tris_str_buffer(colnames));
	if (resultset) {
		int uid;
		struct tris_datastore *odbc_store;
		uid = tris_atomic_fetchadd_int(&resultcount, +1) + 1;
		snprintf(buf, len, "%d", uid);
		odbc_store = tris_datastore_alloc(&odbc_info, buf);
		if (!odbc_store) {
			tris_log(LOG_ERROR, "Rows retrieved, but unable to store it in the channel.  Results fail.\n");
			pbx_builtin_setvar_helper(chan, "ODBCSTATUS", "MEMERROR");
			odbc_datastore_free(resultset);
			SQLCloseCursor(stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			obj = NULL;
			if (chan)
				tris_autoservice_stop(chan);
			if (bogus_chan)
				tris_channel_free(chan);
			return -1;
		}
		odbc_store->data = resultset;
		tris_channel_datastore_add(chan, odbc_store);
	}
	SQLCloseCursor(stmt);
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	obj = NULL;
	if (chan)
		tris_autoservice_stop(chan);
	if (bogus_chan)
		tris_channel_free(chan);
	return 0;
}

static int acf_escape(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *out = buf;

	for (; *data && out - buf < len; data++) {
		if (*data == '\'') {
			*out = '\'';
			out++;
		}
		*out++ = *data;
	}
	*out = '\0';

	return 0;
}

static struct tris_custom_function escape_function = {
	.name = "SQL_ESC",
	.read = acf_escape,
	.write = NULL,
};

static int acf_fetch(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_datastore *store;
	struct odbc_datastore *resultset;
	struct odbc_datastore_row *row;
	store = tris_channel_datastore_find(chan, &odbc_info, data);
	if (!store) {
		pbx_builtin_setvar_helper(chan, "ODBC_FETCH_STATUS", "FAILURE");
		return -1;
	}
	resultset = store->data;
	TRIS_LIST_LOCK(resultset);
	row = TRIS_LIST_REMOVE_HEAD(resultset, list);
	TRIS_LIST_UNLOCK(resultset);
	if (!row) {
		/* Cleanup datastore */
		tris_channel_datastore_remove(chan, store);
		tris_datastore_free(store);
		pbx_builtin_setvar_helper(chan, "ODBC_FETCH_STATUS", "FAILURE");
		return -1;
	}
	pbx_builtin_setvar_helper(chan, "~ODBCFIELDS~", resultset->names);
	tris_copy_string(buf, row->data, len);
	tris_free(row);
	pbx_builtin_setvar_helper(chan, "ODBC_FETCH_STATUS", "SUCCESS");
	return 0;
}

static struct tris_custom_function fetch_function = {
	.name = "ODBC_FETCH",
	.read = acf_fetch,
	.write = NULL,
};

static char *app_odbcfinish = "ODBCFinish";

static int exec_odbcfinish(struct tris_channel *chan, void *data)
{
	struct tris_datastore *store = tris_channel_datastore_find(chan, &odbc_info, data);
	if (!store) /* Already freed; no big deal. */
		return 0;
	tris_channel_datastore_remove(chan, store);
	tris_datastore_free(store);
	return 0;
}

static int init_acf_query(struct tris_config *cfg, char *catg, struct acf_odbc_query **query)
{
	const char *tmp;
	int i;

	if (!cfg || !catg) {
		return EINVAL;
	}

	*query = tris_calloc(1, sizeof(struct acf_odbc_query));
	if (! (*query))
		return ENOMEM;

	if (((tmp = tris_variable_retrieve(cfg, catg, "writehandle"))) || ((tmp = tris_variable_retrieve(cfg, catg, "dsn")))) {
		char *tmp2 = tris_strdupa(tmp);
		TRIS_DECLARE_APP_ARGS(writeconf,
			TRIS_APP_ARG(dsn)[5];
		);
		TRIS_STANDARD_APP_ARGS(writeconf, tmp2);
		for (i = 0; i < 5; i++) {
			if (!tris_strlen_zero(writeconf.dsn[i]))
				tris_copy_string((*query)->writehandle[i], writeconf.dsn[i], sizeof((*query)->writehandle[i]));
		}
	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "readhandle"))) {
		char *tmp2 = tris_strdupa(tmp);
		TRIS_DECLARE_APP_ARGS(readconf,
			TRIS_APP_ARG(dsn)[5];
		);
		TRIS_STANDARD_APP_ARGS(readconf, tmp2);
		for (i = 0; i < 5; i++) {
			if (!tris_strlen_zero(readconf.dsn[i]))
				tris_copy_string((*query)->readhandle[i], readconf.dsn[i], sizeof((*query)->readhandle[i]));
		}
	} else {
		/* If no separate readhandle, then use the writehandle for reading */
		for (i = 0; i < 5; i++) {
			if (!tris_strlen_zero((*query)->writehandle[i]))
				tris_copy_string((*query)->readhandle[i], (*query)->writehandle[i], sizeof((*query)->readhandle[i]));
		}
 	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "readsql")))
		tris_copy_string((*query)->sql_read, tmp, sizeof((*query)->sql_read));
	else if ((tmp = tris_variable_retrieve(cfg, catg, "read"))) {
		tris_log(LOG_WARNING, "Parameter 'read' is deprecated for category %s.  Please use 'readsql' instead.\n", catg);
		tris_copy_string((*query)->sql_read, tmp, sizeof((*query)->sql_read));
	}

	if (!tris_strlen_zero((*query)->sql_read) && tris_strlen_zero((*query)->readhandle[0])) {
		tris_free(*query);
		*query = NULL;
		tris_log(LOG_ERROR, "There is SQL, but no ODBC class to be used for reading: %s\n", catg);
		return EINVAL;
	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "writesql")))
		tris_copy_string((*query)->sql_write, tmp, sizeof((*query)->sql_write));
	else if ((tmp = tris_variable_retrieve(cfg, catg, "write"))) {
		tris_log(LOG_WARNING, "Parameter 'write' is deprecated for category %s.  Please use 'writesql' instead.\n", catg);
		tris_copy_string((*query)->sql_write, tmp, sizeof((*query)->sql_write));
	}

	if (!tris_strlen_zero((*query)->sql_write) && tris_strlen_zero((*query)->writehandle[0])) {
		tris_free(*query);
		*query = NULL;
		tris_log(LOG_ERROR, "There is SQL, but no ODBC class to be used for writing: %s\n", catg);
		return EINVAL;
	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "insertsql"))) {
		tris_copy_string((*query)->sql_insert, tmp, sizeof((*query)->sql_insert));
	}

	/* Allow escaping of embedded commas in fields to be turned off */
	tris_set_flag((*query), OPT_ESCAPECOMMAS);
	if ((tmp = tris_variable_retrieve(cfg, catg, "escapecommas"))) {
		if (tris_false(tmp))
			tris_clear_flag((*query), OPT_ESCAPECOMMAS);
	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "mode"))) {
		if (strcasecmp(tmp, "multirow") == 0)
			tris_set_flag((*query), OPT_MULTIROW);
		if ((tmp = tris_variable_retrieve(cfg, catg, "rowlimit")))
			sscanf(tmp, "%30d", &((*query)->rowlimit));
	}

	(*query)->acf = tris_calloc(1, sizeof(struct tris_custom_function));
	if (! (*query)->acf) {
		tris_free(*query);
		*query = NULL;
		return ENOMEM;
	}
	if (tris_string_field_init((*query)->acf, 128)) {
		tris_free((*query)->acf);
		tris_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "prefix")) && !tris_strlen_zero(tmp)) {
		if (asprintf((char **)&((*query)->acf->name), "%s_%s", tmp, catg) < 0) {
			tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
		}
	} else {
		if (asprintf((char **)&((*query)->acf->name), "ODBC_%s", catg) < 0) {
			tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
		}
	}

	if (!((*query)->acf->name)) {
		tris_string_field_free_memory((*query)->acf);
		tris_free((*query)->acf);
		tris_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "syntax")) && !tris_strlen_zero(tmp)) {
		tris_string_field_build((*query)->acf, syntax, "%s(%s)", (*query)->acf->name, tmp);
	} else {
		tris_string_field_build((*query)->acf, syntax, "%s(<arg1>[...[,<argN>]])", (*query)->acf->name);
	}

	if (tris_strlen_zero((*query)->acf->syntax)) {
		tris_free((char *)(*query)->acf->name);
		tris_string_field_free_memory((*query)->acf);
		tris_free((*query)->acf);
		tris_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if ((tmp = tris_variable_retrieve(cfg, catg, "synopsis")) && !tris_strlen_zero(tmp)) {
		tris_string_field_set((*query)->acf, synopsis, tmp);
	} else {
		tris_string_field_set((*query)->acf, synopsis, "Runs the referenced query with the specified arguments");
	}

	if (tris_strlen_zero((*query)->acf->synopsis)) {
		tris_free((char *)(*query)->acf->name);
		tris_string_field_free_memory((*query)->acf);
		tris_free((*query)->acf);
		tris_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if (!tris_strlen_zero((*query)->sql_read) && !tris_strlen_zero((*query)->sql_write)) {
		tris_string_field_build((*query)->acf, desc,
					"Runs the following query, as defined in func_odbc.conf, performing\n"
				   	"substitution of the arguments into the query as specified by ${ARG1},\n"
					"${ARG2}, ... ${ARGn}.  When setting the function, the values are provided\n"
					"either in whole as ${VALUE} or parsed as ${VAL1}, ${VAL2}, ... ${VALn}.\n"
					"%s"
					"\nRead:\n%s\n\nWrite:\n%s\n%s%s%s",
					tris_strlen_zero((*query)->sql_insert) ? "" :
						"If the write query affects no rows, the insert query will be\n"
						"performed.\n",
					(*query)->sql_read,
					(*query)->sql_write,
					tris_strlen_zero((*query)->sql_insert) ? "" : "Insert:\n",
					tris_strlen_zero((*query)->sql_insert) ? "" : (*query)->sql_insert,
					tris_strlen_zero((*query)->sql_insert) ? "" : "\n");
	} else if (!tris_strlen_zero((*query)->sql_read)) {
		tris_string_field_build((*query)->acf, desc,
						"Runs the following query, as defined in func_odbc.conf, performing\n"
					   	"substitution of the arguments into the query as specified by ${ARG1},\n"
						"${ARG2}, ... ${ARGn}.  This function may only be read, not set.\n\nSQL:\n%s\n",
						(*query)->sql_read);
	} else if (!tris_strlen_zero((*query)->sql_write)) {
		tris_string_field_build((*query)->acf, desc,	
					"Runs the following query, as defined in func_odbc.conf, performing\n"
				   	"substitution of the arguments into the query as specified by ${ARG1},\n"
					"${ARG2}, ... ${ARGn}.  The values are provided either in whole as\n"
					"${VALUE} or parsed as ${VAL1}, ${VAL2}, ... ${VALn}.\n"
					"This function may only be set.\n%sSQL:\n%s\n%s%s%s",
					tris_strlen_zero((*query)->sql_insert) ? "" :
						"If the write query affects no rows, the insert query will be\n"
						"performed.\n",
					(*query)->sql_write,
					tris_strlen_zero((*query)->sql_insert) ? "" : "Insert:\n",
					tris_strlen_zero((*query)->sql_insert) ? "" : (*query)->sql_insert,
					tris_strlen_zero((*query)->sql_insert) ? "" : "\n");
	} else {
		tris_string_field_free_memory((*query)->acf);
		tris_free((char *)(*query)->acf->name);
		tris_free((*query)->acf);
		tris_free(*query);
		tris_log(LOG_WARNING, "Section '%s' was found, but there was no SQL to execute.  Ignoring.\n", catg);
		return EINVAL;
	}

	if (tris_strlen_zero((*query)->acf->desc)) {
		tris_string_field_free_memory((*query)->acf);
		tris_free((char *)(*query)->acf->name);
		tris_free((*query)->acf);
		tris_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if (tris_strlen_zero((*query)->sql_read)) {
		(*query)->acf->read = NULL;
	} else {
		(*query)->acf->read = acf_odbc_read;
	}

	if (tris_strlen_zero((*query)->sql_write)) {
		(*query)->acf->write = NULL;
	} else {
		(*query)->acf->write = acf_odbc_write;
	}

	return 0;
}

static int free_acf_query(struct acf_odbc_query *query)
{
	if (query) {
		if (query->acf) {
			if (query->acf->name)
				tris_free((char *)query->acf->name);
			tris_string_field_free_memory(query->acf);
			tris_free(query->acf);
		}
		tris_free(query);
	}
	return 0;
}

static char *cli_odbc_read(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(field)[100];
	);
	struct tris_str *sql;
	char *char_args, varname[10];
	struct acf_odbc_query *query;
	struct tris_channel *chan;
	int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "odbc read";
		e->usage =
			"Usage: odbc read <name> <args> [exec]\n"
			"       Evaluates the SQL provided in the ODBC function <name>, and\n"
			"       optionally executes the function.  This function is intended for\n"
			"       testing purposes.  Remember to quote arguments containing spaces.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 2) {
			int wordlen = strlen(a->word), which = 0;
			/* Complete function name */
			TRIS_RWLIST_RDLOCK(&queries);
			TRIS_RWLIST_TRAVERSE(&queries, query, list) {
				if (!strncasecmp(query->acf->name, a->word, wordlen)) {
					if (++which > a->n) {
						char *res = tris_strdup(query->acf->name);
						TRIS_RWLIST_UNLOCK(&queries);
						return res;
					}
				}
			}
			TRIS_RWLIST_UNLOCK(&queries);
			return NULL;
		} else if (a->pos == 4) {
			return a->n == 0 ? tris_strdup("exec") : NULL;
		} else {
			return NULL;
		}
	}

	if (a->argc < 4 || a->argc > 5) {
		return CLI_SHOWUSAGE;
	}

	sql = tris_str_thread_get(&sql_buf, 16);
	if (!sql) {
		return CLI_FAILURE;
	}

	TRIS_RWLIST_RDLOCK(&queries);
	TRIS_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, a->argv[2])) {
			break;
		}
	}

	if (!query) {
		tris_cli(a->fd, "No such query '%s'\n", a->argv[2]);
		TRIS_RWLIST_UNLOCK(&queries);
		return CLI_SHOWUSAGE;
	}

	if (tris_strlen_zero(query->sql_read)) {
		tris_cli(a->fd, "The function %s has no writesql parameter.\n", a->argv[2]);
		TRIS_RWLIST_UNLOCK(&queries);
		return CLI_SUCCESS;
	}

	tris_str_make_space(&sql, strlen(query->sql_read) * 2 + 300);

	/* Evaluate function */
	char_args = tris_strdupa(a->argv[3]);

	chan = tris_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc");

	TRIS_STANDARD_APP_ARGS(args, char_args);
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[i]);
	}

	tris_str_substitute_variables(&sql, 0, chan, query->sql_read);
	tris_channel_free(chan);

	if (a->argc == 5 && !strcmp(a->argv[4], "exec")) {
		/* Execute the query */
		struct odbc_obj *obj = NULL;
		int dsn, executed = 0;
		SQLHSTMT stmt;
		int rows = 0, res, x;
		SQLSMALLINT colcount = 0, collength;
		SQLLEN indicator;
		struct tris_str *coldata = tris_str_thread_get(&coldata_buf, 16);
		char colname[256];
		SQLULEN maxcol;

		for (dsn = 0; dsn < 5; dsn++) {
			if (tris_strlen_zero(query->readhandle[dsn])) {
				continue;
			}
			tris_debug(1, "Found handle %s\n", query->readhandle[dsn]);
			if (!(obj = tris_odbc_request_obj(query->readhandle[dsn], 0))) {
				continue;
			}

			tris_debug(1, "Got obj\n");
			if (!(stmt = tris_odbc_direct_execute(obj, generic_execute, tris_str_buffer(sql)))) {
				tris_odbc_release_obj(obj);
				obj = NULL;
				continue;
			}

			executed = 1;

			res = SQLNumResultCols(stmt, &colcount);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_cli(a->fd, "SQL Column Count error!\n[%s]\n\n", tris_str_buffer(sql));
				SQLCloseCursor(stmt);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				obj = NULL;
				TRIS_RWLIST_UNLOCK(&queries);
				return CLI_SUCCESS;
			}

			res = SQLFetch(stmt);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				SQLCloseCursor(stmt);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				obj = NULL;
				if (res == SQL_NO_DATA) {
					tris_cli(a->fd, "Returned %d rows.  Query executed on handle %d:%s [%s]\n", rows, dsn, query->readhandle[dsn], tris_str_buffer(sql));
					break;
				} else {
					tris_cli(a->fd, "Error %d in FETCH [%s]\n", res, tris_str_buffer(sql));
				}
				TRIS_RWLIST_UNLOCK(&queries);
				return CLI_SUCCESS;
			}
			for (;;) {
				for (x = 0; x < colcount; x++) {
					res = SQLDescribeCol(stmt, x + 1, (unsigned char *)colname, sizeof(colname), &collength, NULL, &maxcol, NULL, NULL);
					if (((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) || collength == 0) {
						snprintf(colname, sizeof(colname), "field%d", x);
					}

					res = tris_odbc_tris_str_SQLGetData(&coldata, maxcol, stmt, x + 1, SQL_CHAR, &indicator);
					if (indicator == SQL_NULL_DATA) {
						tris_str_set(&coldata, 0, "(nil)");
						res = SQL_SUCCESS;
					}

					if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
						tris_cli(a->fd, "SQL Get Data error %d!\n[%s]\n\n", res, tris_str_buffer(sql));
						SQLCloseCursor(stmt);
						SQLFreeHandle(SQL_HANDLE_STMT, stmt);
						tris_odbc_release_obj(obj);
						obj = NULL;
						TRIS_RWLIST_UNLOCK(&queries);
						return CLI_SUCCESS;
					}

					tris_cli(a->fd, "%-20.20s  %s\n", colname, tris_str_buffer(coldata));
				}
				rows++;

				/* Get next row */
				res = SQLFetch(stmt);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					break;
				}
				tris_cli(a->fd, "%-20.20s  %s\n", "----------", "----------");
			}
			SQLCloseCursor(stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			obj = NULL;
			tris_cli(a->fd, "Returned %d row%s.  Query executed on handle %d [%s]\n", rows, rows == 1 ? "" : "s", dsn, query->readhandle[dsn]);
			break;
		}
		if (obj) {
			tris_odbc_release_obj(obj);
			obj = NULL;
		}

		if (!executed) {
			tris_cli(a->fd, "Failed to execute query. [%s]\n", tris_str_buffer(sql));
		}
	} else { /* No execution, just print out the resulting SQL */
		tris_cli(a->fd, "%s\n", tris_str_buffer(sql));
	}
	TRIS_RWLIST_UNLOCK(&queries);
	return CLI_SUCCESS;
}

static char *cli_odbc_write(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	TRIS_DECLARE_APP_ARGS(values,
		TRIS_APP_ARG(field)[100];
	);
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(field)[100];
	);
	struct tris_str *sql;
	char *char_args, *char_values, varname[10];
	struct acf_odbc_query *query;
	struct tris_channel *chan;
	int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "odbc write";
		e->usage =
			"Usage: odbc write <name> <args> <value> [exec]\n"
			"       Evaluates the SQL provided in the ODBC function <name>, and\n"
			"       optionally executes the function.  This function is intended for\n"
			"       testing purposes.  Remember to quote arguments containing spaces.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 2) {
			int wordlen = strlen(a->word), which = 0;
			/* Complete function name */
			TRIS_RWLIST_RDLOCK(&queries);
			TRIS_RWLIST_TRAVERSE(&queries, query, list) {
				if (!strncasecmp(query->acf->name, a->word, wordlen)) {
					if (++which > a->n) {
						char *res = tris_strdup(query->acf->name);
						TRIS_RWLIST_UNLOCK(&queries);
						return res;
					}
				}
			}
			TRIS_RWLIST_UNLOCK(&queries);
			return NULL;
		} else if (a->pos == 5) {
			return a->n == 0 ? tris_strdup("exec") : NULL;
		} else {
			return NULL;
		}
	}

	if (a->argc < 5 || a->argc > 6) {
		return CLI_SHOWUSAGE;
	}

	sql = tris_str_thread_get(&sql_buf, 16);
	if (!sql) {
		return CLI_FAILURE;
	}

	TRIS_RWLIST_RDLOCK(&queries);
	TRIS_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, a->argv[2])) {
			break;
		}
	}

	if (!query) {
		tris_cli(a->fd, "No such query '%s'\n", a->argv[2]);
		TRIS_RWLIST_UNLOCK(&queries);
		return CLI_SHOWUSAGE;
	}

	if (tris_strlen_zero(query->sql_write)) {
		tris_cli(a->fd, "The function %s has no writesql parameter.\n", a->argv[2]);
		TRIS_RWLIST_UNLOCK(&queries);
		return CLI_SUCCESS;
	}

	tris_str_make_space(&sql, strlen(query->sql_write) * 2 + 300);

	/* Evaluate function */
	char_args = tris_strdupa(a->argv[3]);
	char_values = tris_strdupa(a->argv[4]);

	chan = tris_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc");

	TRIS_STANDARD_APP_ARGS(args, char_args);
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[i]);
	}

	/* Parse values, just like arguments */
	TRIS_STANDARD_APP_ARGS(values, char_values);
	for (i = 0; i < values.argc; i++) {
		snprintf(varname, sizeof(varname), "VAL%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, values.field[i]);
	}

	/* Additionally set the value as a whole (but push an empty string if value is NULL) */
	pbx_builtin_pushvar_helper(chan, "VALUE", S_OR(a->argv[4], ""));
	tris_str_substitute_variables(&sql, 0, chan, query->sql_write);
	tris_debug(1, "SQL is %s\n", tris_str_buffer(sql));
	tris_channel_free(chan);

	if (a->argc == 6 && !strcmp(a->argv[5], "exec")) {
		/* Execute the query */
		struct odbc_obj *obj = NULL;
		int dsn, executed = 0;
		SQLHSTMT stmt;
		SQLLEN rows = -1;

		for (dsn = 0; dsn < 5; dsn++) {
			if (tris_strlen_zero(query->writehandle[dsn])) {
				continue;
			}
			if (!(obj = tris_odbc_request_obj(query->writehandle[dsn], 0))) {
				continue;
			}
			if (!(stmt = tris_odbc_direct_execute(obj, generic_execute, tris_str_buffer(sql)))) {
				tris_odbc_release_obj(obj);
				obj = NULL;
				continue;
			}

			SQLRowCount(stmt, &rows);
			SQLCloseCursor(stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			obj = NULL;
			tris_cli(a->fd, "Affected %d rows.  Query executed on handle %d [%s]\n", (int)rows, dsn, query->writehandle[dsn]);
			executed = 1;
			break;
		}

		if (!executed) {
			tris_cli(a->fd, "Failed to execute query.\n");
		}
	} else { /* No execution, just print out the resulting SQL */
		tris_cli(a->fd, "%s\n", tris_str_buffer(sql));
	}
	TRIS_RWLIST_UNLOCK(&queries);
	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_func_odbc[] = {
	TRIS_CLI_DEFINE(cli_odbc_write, "Test setting a func_odbc function"),
	TRIS_CLI_DEFINE(cli_odbc_read, "Test reading a func_odbc function"),
};

static int load_module(void)
{
	int res = 0;
	struct tris_config *cfg;
	char *catg;
	struct tris_flags config_flags = { 0 };

	res |= tris_custom_function_register(&fetch_function);
	res |= tris_register_application_xml(app_odbcfinish, exec_odbcfinish);
	TRIS_RWLIST_WRLOCK(&queries);

	cfg = tris_config_load(config, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_NOTICE, "Unable to load config for func_odbc: %s\n", config);
		TRIS_RWLIST_UNLOCK(&queries);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	for (catg = tris_category_browse(cfg, NULL);
	     catg;
	     catg = tris_category_browse(cfg, catg)) {
		struct acf_odbc_query *query = NULL;
		int err;

		if ((err = init_acf_query(cfg, catg, &query))) {
			if (err == ENOMEM)
				tris_log(LOG_ERROR, "Out of memory\n");
			else if (err == EINVAL)
				tris_log(LOG_ERROR, "Invalid parameters for category %s\n", catg);
			else
				tris_log(LOG_ERROR, "%s (%d)\n", strerror(err), err);
		} else {
			TRIS_RWLIST_INSERT_HEAD(&queries, query, list);
			tris_custom_function_register(query->acf);
		}
	}

	tris_config_destroy(cfg);
	res |= tris_custom_function_register(&escape_function);
	tris_cli_register_multiple(cli_func_odbc, ARRAY_LEN(cli_func_odbc));

	TRIS_RWLIST_UNLOCK(&queries);
	return res;
}

static int unload_module(void)
{
	struct acf_odbc_query *query;
	int res = 0;

	TRIS_RWLIST_WRLOCK(&queries);
	while (!TRIS_RWLIST_EMPTY(&queries)) {
		query = TRIS_RWLIST_REMOVE_HEAD(&queries, list);
		tris_custom_function_unregister(query->acf);
		free_acf_query(query);
	}

	res |= tris_custom_function_unregister(&escape_function);
	res |= tris_custom_function_unregister(&fetch_function);
	res |= tris_unregister_application(app_odbcfinish);
	tris_cli_unregister_multiple(cli_func_odbc, ARRAY_LEN(cli_func_odbc));

	/* Allow any threads waiting for this lock to pass (avoids a race) */
	TRIS_RWLIST_UNLOCK(&queries);
	usleep(1);
	TRIS_RWLIST_WRLOCK(&queries);

	TRIS_RWLIST_UNLOCK(&queries);
	return 0;
}

static int reload(void)
{
	int res = 0;
	struct tris_config *cfg;
	struct acf_odbc_query *oldquery;
	char *catg;
	struct tris_flags config_flags = { CONFIG_FLAG_FILEUNCHANGED };

	cfg = tris_config_load(config, config_flags);
	if (cfg == CONFIG_STATUS_FILEUNCHANGED || cfg == CONFIG_STATUS_FILEINVALID)
		return 0;

	TRIS_RWLIST_WRLOCK(&queries);

	while (!TRIS_RWLIST_EMPTY(&queries)) {
		oldquery = TRIS_RWLIST_REMOVE_HEAD(&queries, list);
		tris_custom_function_unregister(oldquery->acf);
		free_acf_query(oldquery);
	}

	if (!cfg) {
		tris_log(LOG_WARNING, "Unable to load config for func_odbc: %s\n", config);
		goto reload_out;
	}

	for (catg = tris_category_browse(cfg, NULL);
	     catg;
	     catg = tris_category_browse(cfg, catg)) {
		struct acf_odbc_query *query = NULL;

		if (init_acf_query(cfg, catg, &query)) {
			tris_log(LOG_ERROR, "Cannot initialize query %s\n", catg);
		} else {
			TRIS_RWLIST_INSERT_HEAD(&queries, query, list);
			tris_custom_function_register(query->acf);
		}
	}

	tris_config_destroy(cfg);
reload_out:
	TRIS_RWLIST_UNLOCK(&queries);
	return res;
}

/* XXX need to revise usecount - set if query_lock is set */

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "ODBC lookups",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );

