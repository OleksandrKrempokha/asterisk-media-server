/*
 * Trismedia -- A telephony toolkit for Linux.
 *
 * Copyright (C) 1999-2005, Digium, Inc.
 * 
 * Manuel Guesdon <mguesdon@oxymium.net> - PostgreSQL RealTime Driver Author/Adaptor
 * Mark Spencer <markster@digium.com>  - Trismedia Author
 * Matthew Boehm <mboehm@cytelcom.com> - MySQL RealTime Driver Author
 *
 * res_config_pgsql.c <PostgreSQL plugin for RealTime configuration engine>
 *
 * v1.0   - (07-11-05) - Initial version based on res_config_mysql v2.0
 */

/*! \file
 *
 * \brief PostgreSQL plugin for Trismedia RealTime Architecture
 *
 * \author Mark Spencer <markster@digium.com>
 * \author Manuel Guesdon <mguesdon@oxymium.net> - PostgreSQL RealTime Driver Author/Adaptor
 *
 * \arg http://www.postgresql.org
 */

/*** MODULEINFO
	<depend>pgsql</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 229094 $")

#include <libpq-fe.h>			/* PostgreSQL */

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"
#include "trismedia/cli.h"

TRIS_MUTEX_DEFINE_STATIC(pgsql_lock);
TRIS_THREADSTORAGE(sql_buf);
TRIS_THREADSTORAGE(findtable_buf);
TRIS_THREADSTORAGE(where_buf);
TRIS_THREADSTORAGE(escapebuf_buf);

#define RES_CONFIG_PGSQL_CONF "res_pgsql.conf"

PGconn *pgsqlConn = NULL;
static int version;
#define has_schema_support	(version > 70300 ? 1 : 0)

#define MAX_DB_OPTION_SIZE 64

struct columns {
	char *name;
	char *type;
	int len;
	unsigned int notnull:1;
	unsigned int hasdefault:1;
	TRIS_LIST_ENTRY(columns) list;
};

struct tables {
	tris_rwlock_t lock;
	TRIS_LIST_HEAD_NOLOCK(psql_columns, columns) columns;
	TRIS_LIST_ENTRY(tables) list;
	char name[0];
};

static TRIS_LIST_HEAD_STATIC(psql_tables, tables);

static char dbhost[MAX_DB_OPTION_SIZE] = "";
static char dbuser[MAX_DB_OPTION_SIZE] = "";
static char dbpass[MAX_DB_OPTION_SIZE] = "";
static char dbname[MAX_DB_OPTION_SIZE] = "";
static char dbsock[MAX_DB_OPTION_SIZE] = "";
static int dbport = 5432;
static time_t connect_time = 0;

static int parse_config(int reload);
static int pgsql_reconnect(const char *database);
static char *handle_cli_realtime_pgsql_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
static char *handle_cli_realtime_pgsql_cache(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);

enum { RQ_WARN, RQ_CREATECLOSE, RQ_CREATECHAR } requirements;

static struct tris_cli_entry cli_realtime[] = {
	TRIS_CLI_DEFINE(handle_cli_realtime_pgsql_status, "Shows connection information for the PostgreSQL RealTime driver"),
	TRIS_CLI_DEFINE(handle_cli_realtime_pgsql_cache, "Shows cached tables within the PostgreSQL realtime driver"),
};

#define ESCAPE_STRING(buffer, stringname) \
	do { \
		int len; \
		if ((len = strlen(stringname)) > (tris_str_size(buffer) - 1) / 2) { \
			tris_str_make_space(&buffer, len * 2 + 1); \
		} \
		PQescapeStringConn(pgsqlConn, tris_str_buffer(buffer), stringname, len, &pgresult); \
	} while (0)

static void destroy_table(struct tables *table)
{
	struct columns *column;
	tris_rwlock_wrlock(&table->lock);
	while ((column = TRIS_LIST_REMOVE_HEAD(&table->columns, list))) {
		tris_free(column);
	}
	tris_rwlock_unlock(&table->lock);
	tris_rwlock_destroy(&table->lock);
	tris_free(table);
}

static struct tables *find_table(const char *orig_tablename)
{
	struct columns *column;
	struct tables *table;
	struct tris_str *sql = tris_str_thread_get(&findtable_buf, 330);
	char *pgerror;
	PGresult *result;
	char *fname, *ftype, *flen, *fnotnull, *fdef;
	int i, rows;

	TRIS_LIST_LOCK(&psql_tables);
	TRIS_LIST_TRAVERSE(&psql_tables, table, list) {
		if (!strcasecmp(table->name, orig_tablename)) {
			tris_debug(1, "Found table in cache; now locking\n");
			tris_rwlock_rdlock(&table->lock);
			tris_debug(1, "Lock cached table; now returning\n");
			TRIS_LIST_UNLOCK(&psql_tables);
			return table;
		}
	}

	tris_debug(1, "Table '%s' not found in cache, querying now\n", orig_tablename);

	/* Not found, scan the table */
	if (has_schema_support) {
		char *schemaname, *tablename;
		if (strchr(orig_tablename, '.')) {
			schemaname = tris_strdupa(orig_tablename);
			tablename = strchr(schemaname, '.');
			*tablename++ = '\0';
		} else {
			schemaname = "";
			tablename = tris_strdupa(orig_tablename);
		}

		/* Escape special characters in schemaname */
		if (strchr(schemaname, '\\') || strchr(schemaname, '\'')) {
			char *tmp = schemaname, *ptr;

			ptr = schemaname = alloca(strlen(tmp) * 2 + 1);
			for (; *tmp; tmp++) {
				if (strchr("\\'", *tmp)) {
					*ptr++ = *tmp;
				}
				*ptr++ = *tmp;
			}
			*ptr = '\0';
		}
		/* Escape special characters in tablename */
		if (strchr(tablename, '\\') || strchr(tablename, '\'')) {
			char *tmp = tablename, *ptr;

			ptr = tablename = alloca(strlen(tmp) * 2 + 1);
			for (; *tmp; tmp++) {
				if (strchr("\\'", *tmp)) {
					*ptr++ = *tmp;
				}
				*ptr++ = *tmp;
			}
			*ptr = '\0';
		}

		tris_str_set(&sql, 0, "SELECT a.attname, t.typname, a.attlen, a.attnotnull, d.adsrc, a.atttypmod FROM (((pg_catalog.pg_class c INNER JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace AND c.relname = '%s' AND n.nspname = %s%s%s) INNER JOIN pg_catalog.pg_attribute a ON (NOT a.attisdropped) AND a.attnum > 0 AND a.attrelid = c.oid) INNER JOIN pg_catalog.pg_type t ON t.oid = a.atttypid) LEFT OUTER JOIN pg_attrdef d ON a.atthasdef AND d.adrelid = a.attrelid AND d.adnum = a.attnum ORDER BY n.nspname, c.relname, attnum",
			tablename,
			tris_strlen_zero(schemaname) ? "" : "'", tris_strlen_zero(schemaname) ? "current_schema()" : schemaname, tris_strlen_zero(schemaname) ? "" : "'");
	} else {
		/* Escape special characters in tablename */
		if (strchr(orig_tablename, '\\') || strchr(orig_tablename, '\'')) {
			const char *tmp = orig_tablename;
			char *ptr;

			orig_tablename = ptr = alloca(strlen(tmp) * 2 + 1);
			for (; *tmp; tmp++) {
				if (strchr("\\'", *tmp)) {
					*ptr++ = *tmp;
				}
				*ptr++ = *tmp;
			}
			*ptr = '\0';
		}

		tris_str_set(&sql, 0, "SELECT a.attname, t.typname, a.attlen, a.attnotnull, d.adsrc, a.atttypmod FROM pg_class c, pg_type t, pg_attribute a LEFT OUTER JOIN pg_attrdef d ON a.atthasdef AND d.adrelid = a.attrelid AND d.adnum = a.attnum WHERE c.oid = a.attrelid AND a.atttypid = t.oid AND (a.attnum > 0) AND c.relname = '%s' ORDER BY c.relname, attnum", orig_tablename);
	}

	result = PQexec(pgsqlConn, tris_str_buffer(sql));
	tris_debug(1, "Query of table structure complete.  Now retrieving results.\n");
	if (PQresultStatus(result) != PGRES_TUPLES_OK) {
		pgerror = PQresultErrorMessage(result);
		tris_log(LOG_ERROR, "Failed to query database columns: %s\n", pgerror);
		PQclear(result);
		TRIS_LIST_UNLOCK(&psql_tables);
		return NULL;
	}

	if (!(table = tris_calloc(1, sizeof(*table) + strlen(orig_tablename) + 1))) {
		tris_log(LOG_ERROR, "Unable to allocate memory for new table structure\n");
		TRIS_LIST_UNLOCK(&psql_tables);
		return NULL;
	}
	strcpy(table->name, orig_tablename); /* SAFE */
	tris_rwlock_init(&table->lock);
	TRIS_LIST_HEAD_INIT_NOLOCK(&table->columns);

	rows = PQntuples(result);
	for (i = 0; i < rows; i++) {
		fname = PQgetvalue(result, i, 0);
		ftype = PQgetvalue(result, i, 1);
		flen = PQgetvalue(result, i, 2);
		fnotnull = PQgetvalue(result, i, 3);
		fdef = PQgetvalue(result, i, 4);
		tris_verb(4, "Found column '%s' of type '%s'\n", fname, ftype);

		if (!(column = tris_calloc(1, sizeof(*column) + strlen(fname) + strlen(ftype) + 2))) {
			tris_log(LOG_ERROR, "Unable to allocate column element for %s, %s\n", orig_tablename, fname);
			destroy_table(table);
			TRIS_LIST_UNLOCK(&psql_tables);
			return NULL;
		}

		if (strcmp(flen, "-1") == 0) {
			/* Some types, like chars, have the length stored in a different field */
			flen = PQgetvalue(result, i, 5);
			sscanf(flen, "%30d", &column->len);
			column->len -= 4;
		} else {
			sscanf(flen, "%30d", &column->len);
		}
		column->name = (char *)column + sizeof(*column);
		column->type = (char *)column + sizeof(*column) + strlen(fname) + 1;
		strcpy(column->name, fname);
		strcpy(column->type, ftype);
		if (*fnotnull == 't') {
			column->notnull = 1;
		} else {
			column->notnull = 0;
		}
		if (!tris_strlen_zero(fdef)) {
			column->hasdefault = 1;
		} else {
			column->hasdefault = 0;
		}
		TRIS_LIST_INSERT_TAIL(&table->columns, column, list);
	}
	PQclear(result);

	TRIS_LIST_INSERT_TAIL(&psql_tables, table, list);
	tris_rwlock_rdlock(&table->lock);
	TRIS_LIST_UNLOCK(&psql_tables);
	return table;
}

#define release_table(table) tris_rwlock_unlock(&(table)->lock);

static struct columns *find_column(struct tables *t, const char *colname)
{
	struct columns *column;

	/* Check that the column exists in the table */
	TRIS_LIST_TRAVERSE(&t->columns, column, list) {
		if (strcmp(column->name, colname) == 0) {
			return column;
		}
	}
	return NULL;
}

static struct tris_variable *realtime_pgsql(const char *database, const char *tablename, va_list ap)
{
	PGresult *result = NULL;
	int num_rows = 0, pgresult;
	struct tris_str *sql = tris_str_thread_get(&sql_buf, 100);
	struct tris_str *escapebuf = tris_str_thread_get(&escapebuf_buf, 100);
	char *stringp;
	char *chunk;
	char *op;
	const char *newparam, *newval;
	struct tris_variable *var = NULL, *prev = NULL;

	if (!tablename) {
		tris_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return NULL;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		}
		return NULL;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */
	op = strchr(newparam, ' ') ? "" : " =";

	ESCAPE_STRING(escapebuf, newval);
	if (pgresult) {
		tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
		va_end(ap);
		return NULL;
	}

	tris_str_set(&sql, 0, "SELECT * FROM %s WHERE %s%s '%s'", tablename, newparam, op, tris_str_buffer(escapebuf));
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if (!strchr(newparam, ' '))
			op = " =";
		else
			op = "";

		ESCAPE_STRING(escapebuf, newval);
		if (pgresult) {
			tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			va_end(ap);
			return NULL;
		}

		tris_str_append(&sql, 0, " AND %s%s '%s'", newparam, op, tris_str_buffer(escapebuf));
	}
	va_end(ap);

	/* We now have our complete statement; Lets connect to the server and execute it. */
	tris_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		tris_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	if (!(result = PQexec(pgsqlConn, tris_str_buffer(sql)))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query '%s@%s'. Check debug for more info.\n", tablename, database);
		tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
		tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		tris_mutex_unlock(&pgsql_lock);
		return NULL;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			tris_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query '%s@%s'. Check debug for more info.\n", tablename, database);
			tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
			tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			tris_mutex_unlock(&pgsql_lock);
			return NULL;
		}
	}

	tris_debug(1, "PostgreSQL RealTime: Result=%p Query: %s\n", result, tris_str_buffer(sql));

	if ((num_rows = PQntuples(result)) > 0) {
		int i = 0;
		int rowIndex = 0;
		int numFields = PQnfields(result);
		char **fieldnames = NULL;

		tris_debug(1, "PostgreSQL RealTime: Found %d rows.\n", num_rows);

		if (!(fieldnames = tris_calloc(1, numFields * sizeof(char *)))) {
			tris_mutex_unlock(&pgsql_lock);
			PQclear(result);
			return NULL;
		}
		for (i = 0; i < numFields; i++)
			fieldnames[i] = PQfname(result, i);
		for (rowIndex = 0; rowIndex < num_rows; rowIndex++) {
			for (i = 0; i < numFields; i++) {
				stringp = PQgetvalue(result, rowIndex, i);
				while (stringp) {
					chunk = strsep(&stringp, ";");
					if (!tris_strlen_zero(tris_strip(chunk))) {
						if (prev) {
							prev->next = tris_variable_new(fieldnames[i], chunk, "");
							if (prev->next) {
								prev = prev->next;
							}
						} else {
							prev = var = tris_variable_new(fieldnames[i], chunk, "");
						}
					}
				}
			}
		}
		tris_free(fieldnames);
	} else {
		tris_debug(1, "Postgresql RealTime: Could not find any rows in table %s@%s.\n", tablename, database);
	}

	tris_mutex_unlock(&pgsql_lock);
	PQclear(result);

	return var;
}

static struct tris_config *realtime_multi_pgsql(const char *database, const char *table, va_list ap)
{
	PGresult *result = NULL;
	int num_rows = 0, pgresult;
	struct tris_str *sql = tris_str_thread_get(&sql_buf, 100);
	struct tris_str *escapebuf = tris_str_thread_get(&escapebuf_buf, 100);
	const char *initfield = NULL;
	char *stringp;
	char *chunk;
	char *op;
	const char *newparam, *newval;
	struct tris_variable *var = NULL;
	struct tris_config *cfg = NULL;
	struct tris_category *cat = NULL;

	if (!table) {
		tris_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return NULL;
	}

	if (!(cfg = tris_config_new()))
		return NULL;

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		}
		return NULL;
	}

	initfield = tris_strdupa(newparam);
	if ((op = strchr(initfield, ' '))) {
		*op = '\0';
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	if (!strchr(newparam, ' '))
		op = " =";
	else
		op = "";

	ESCAPE_STRING(escapebuf, newval);
	if (pgresult) {
		tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
		va_end(ap);
		return NULL;
	}

	tris_str_set(&sql, 0, "SELECT * FROM %s WHERE %s%s '%s'", table, newparam, op, tris_str_buffer(escapebuf));
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if (!strchr(newparam, ' '))
			op = " =";
		else
			op = "";

		ESCAPE_STRING(escapebuf, newval);
		if (pgresult) {
			tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			va_end(ap);
			return NULL;
		}

		tris_str_append(&sql, 0, " AND %s%s '%s'", newparam, op, tris_str_buffer(escapebuf));
	}

	if (initfield) {
		tris_str_append(&sql, 0, " ORDER BY %s", initfield);
	}

	va_end(ap);

	/* We now have our complete statement; Lets connect to the server and execute it. */
	tris_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		tris_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	if (!(result = PQexec(pgsqlConn, tris_str_buffer(sql)))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query %s@%s. Check debug for more info.\n", table, database);
		tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
		tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		tris_mutex_unlock(&pgsql_lock);
		return NULL;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			tris_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query %s@%s. Check debug for more info.\n", table, database);
			tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
			tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			tris_mutex_unlock(&pgsql_lock);
			return NULL;
		}
	}

	tris_debug(1, "PostgreSQL RealTime: Result=%p Query: %s\n", result, tris_str_buffer(sql));

	if ((num_rows = PQntuples(result)) > 0) {
		int numFields = PQnfields(result);
		int i = 0;
		int rowIndex = 0;
		char **fieldnames = NULL;

		tris_debug(1, "PostgreSQL RealTime: Found %d rows.\n", num_rows);

		if (!(fieldnames = tris_calloc(1, numFields * sizeof(char *)))) {
			tris_mutex_unlock(&pgsql_lock);
			PQclear(result);
			return NULL;
		}
		for (i = 0; i < numFields; i++)
			fieldnames[i] = PQfname(result, i);

		for (rowIndex = 0; rowIndex < num_rows; rowIndex++) {
			var = NULL;
			if (!(cat = tris_category_new("","",99999)))
				continue;
			for (i = 0; i < numFields; i++) {
				stringp = PQgetvalue(result, rowIndex, i);
				while (stringp) {
					chunk = strsep(&stringp, ";");
					if (!tris_strlen_zero(tris_strip(chunk))) {
						if (initfield && !strcmp(initfield, fieldnames[i])) {
							tris_category_rename(cat, chunk);
						}
						var = tris_variable_new(fieldnames[i], chunk, "");
						tris_variable_append(cat, var);
					}
				}
			}
			tris_category_append(cfg, cat);
		}
		tris_free(fieldnames);
	} else {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Could not find any rows in table %s.\n", table);
	}

	tris_mutex_unlock(&pgsql_lock);
	PQclear(result);

	return cfg;
}

static int update_pgsql(const char *database, const char *tablename, const char *keyfield,
						const char *lookup, va_list ap)
{
	PGresult *result = NULL;
	int numrows = 0, pgresult;
	const char *newparam, *newval;
	struct tris_str *sql = tris_str_thread_get(&sql_buf, 100);
	struct tris_str *escapebuf = tris_str_thread_get(&escapebuf_buf, 100);
	struct tables *table;
	struct columns *column = NULL;

	if (!tablename) {
		tris_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return -1;
	}

	if (!(table = find_table(tablename))) {
		tris_log(LOG_ERROR, "Table '%s' does not exist!!\n", tablename);
		return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		}
		release_table(table);
		return -1;
	}

	/* Check that the column exists in the table */
	TRIS_LIST_TRAVERSE(&table->columns, column, list) {
		if (strcmp(column->name, newparam) == 0) {
			break;
		}
	}

	if (!column) {
		tris_log(LOG_ERROR, "PostgreSQL RealTime: Updating on column '%s', but that column does not exist within the table '%s'!\n", newparam, tablename);
		release_table(table);
		return -1;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	ESCAPE_STRING(escapebuf, newval);
	if (pgresult) {
		tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
		va_end(ap);
		release_table(table);
		return -1;
	}
	tris_str_set(&sql, 0, "UPDATE %s SET %s = '%s'", tablename, newparam, tris_str_buffer(escapebuf));

	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);

		if (!find_column(table, newparam)) {
			tris_log(LOG_WARNING, "Attempted to update column '%s' in table '%s', but column does not exist!\n", newparam, tablename);
			continue;
		}

		ESCAPE_STRING(escapebuf, newval);
		if (pgresult) {
			tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			va_end(ap);
			release_table(table);
			return -1;
		}

		tris_str_append(&sql, 0, ", %s = '%s'", newparam, tris_str_buffer(escapebuf));
	}
	va_end(ap);
	release_table(table);

	ESCAPE_STRING(escapebuf, lookup);
	if (pgresult) {
		tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", lookup);
		va_end(ap);
		return -1;
	}

	tris_str_append(&sql, 0, " WHERE %s = '%s'", keyfield, tris_str_buffer(escapebuf));

	tris_debug(1, "PostgreSQL RealTime: Update SQL: %s\n", tris_str_buffer(sql));

	/* We now have our complete statement; Lets connect to the server and execute it. */
	tris_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	}

	if (!(result = PQexec(pgsqlConn, tris_str_buffer(sql)))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
		tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			tris_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
			tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			tris_mutex_unlock(&pgsql_lock);
			return -1;
		}
	}

	numrows = atoi(PQcmdTuples(result));
	tris_mutex_unlock(&pgsql_lock);

	tris_debug(1, "PostgreSQL RealTime: Updated %d rows on table: %s\n", numrows, tablename);

	/* From http://dev.pgsql.com/doc/pgsql/en/pgsql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	 */

	if (numrows >= 0)
		return (int) numrows;

	return -1;
}

static int update2_pgsql(const char *database, const char *tablename, va_list ap)
{
	PGresult *result = NULL;
	int numrows = 0, pgresult, first = 1;
	struct tris_str *escapebuf = tris_str_thread_get(&escapebuf_buf, 16);
	const char *newparam, *newval;
	struct tris_str *sql = tris_str_thread_get(&sql_buf, 100);
	struct tris_str *where = tris_str_thread_get(&where_buf, 100);
	struct tables *table;

	if (!tablename) {
		tris_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return -1;
	}

	if (!escapebuf || !sql || !where) {
		/* Memory error, already handled */
		return -1;
	}

	if (!(table = find_table(tablename))) {
		tris_log(LOG_ERROR, "Table '%s' does not exist!!\n", tablename);
		return -1;
	}

	tris_str_set(&sql, 0, "UPDATE %s SET ", tablename);
	tris_str_set(&where, 0, "WHERE");

	while ((newparam = va_arg(ap, const char *))) {
		if (!find_column(table, newparam)) {
			tris_log(LOG_ERROR, "Attempted to update based on criteria column '%s' (%s@%s), but that column does not exist!\n", newparam, tablename, database);
			release_table(table);
			return -1;
		}
			
		newval = va_arg(ap, const char *);
		ESCAPE_STRING(escapebuf, newval);
		if (pgresult) {
			tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			release_table(table);
			tris_free(sql);
			return -1;
		}
		tris_str_append(&where, 0, "%s %s='%s'", first ? "" : " AND", newparam, tris_str_buffer(escapebuf));
		first = 0;
	}

	if (first) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime update requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		}
		release_table(table);
		return -1;
	}

	/* Now retrieve the columns to update */
	first = 1;
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);

		/* If the column is not within the table, then skip it */
		if (!find_column(table, newparam)) {
			tris_log(LOG_NOTICE, "Attempted to update column '%s' in table '%s@%s', but column does not exist!\n", newparam, tablename, database);
			continue;
		}

		ESCAPE_STRING(escapebuf, newval);
		if (pgresult) {
			tris_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			release_table(table);
			tris_free(sql);
			return -1;
		}

		tris_str_append(&sql, 0, "%s %s='%s'", first ? "" : ",", newparam, tris_str_buffer(escapebuf));
	}
	release_table(table);

	tris_str_append(&sql, 0, " %s", tris_str_buffer(where));

	tris_debug(1, "PostgreSQL RealTime: Update SQL: %s\n", tris_str_buffer(sql));

	/* We now have our complete statement; connect to the server and execute it. */
	tris_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	}

	if (!(result = PQexec(pgsqlConn, tris_str_buffer(sql)))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
		tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			tris_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
			tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			tris_mutex_unlock(&pgsql_lock);
			return -1;
		}
	}

	numrows = atoi(PQcmdTuples(result));
	tris_mutex_unlock(&pgsql_lock);

	tris_debug(1, "PostgreSQL RealTime: Updated %d rows on table: %s\n", numrows, tablename);

	/* From http://dev.pgsql.com/doc/pgsql/en/pgsql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	 */

	if (numrows >= 0) {
		return (int) numrows;
	}

	return -1;
}

static int store_pgsql(const char *database, const char *table, va_list ap)
{
	PGresult *result = NULL;
	Oid insertid;
	struct tris_str *buf = tris_str_thread_get(&escapebuf_buf, 256);
	struct tris_str *sql1 = tris_str_thread_get(&sql_buf, 256);
	struct tris_str *sql2 = tris_str_thread_get(&where_buf, 256);
	int pgresult;
	const char *newparam, *newval;

	if (!table) {
		tris_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime storage requires at least 1 parameter and 1 value to store.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		}
		return -1;
	}

	/* Must connect to the server before anything else, as the escape function requires the connection handle.. */
	tris_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */
	ESCAPE_STRING(buf, newparam);
	tris_str_set(&sql1, 0, "INSERT INTO %s (%s", table, tris_str_buffer(buf));
	ESCAPE_STRING(buf, newval);
	tris_str_set(&sql2, 0, ") VALUES ('%s'", tris_str_buffer(buf));
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		ESCAPE_STRING(buf, newparam);
		tris_str_append(&sql1, 0, ", %s", tris_str_buffer(buf));
		ESCAPE_STRING(buf, newval);
		tris_str_append(&sql2, 0, ", '%s'", tris_str_buffer(buf));
	}
	va_end(ap);
	tris_str_append(&sql1, 0, "%s)", tris_str_buffer(sql2));

	tris_debug(1, "PostgreSQL RealTime: Insert SQL: %s\n", tris_str_buffer(sql1));

	if (!(result = PQexec(pgsqlConn, tris_str_buffer(sql1)))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql1));
		tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			tris_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql1));
			tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			tris_mutex_unlock(&pgsql_lock);
			return -1;
		}
	}

	insertid = PQoidValue(result);
	tris_mutex_unlock(&pgsql_lock);

	tris_debug(1, "PostgreSQL RealTime: row inserted on table: %s, id: %u\n", table, insertid);

	/* From http://dev.pgsql.com/doc/pgsql/en/pgsql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	 */

	if (insertid >= 0)
		return (int) insertid;

	return -1;
}

static int destroy_pgsql(const char *database, const char *table, const char *keyfield, const char *lookup, va_list ap)
{
	PGresult *result = NULL;
	int numrows = 0;
	int pgresult;
	struct tris_str *sql = tris_str_thread_get(&sql_buf, 256);
	struct tris_str *buf1 = tris_str_thread_get(&where_buf, 60), *buf2 = tris_str_thread_get(&escapebuf_buf, 60);
	const char *newparam, *newval;

	if (!table) {
		tris_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	/*newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {*/
	if (tris_strlen_zero(keyfield) || tris_strlen_zero(lookup))  {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime destroy requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		};
		return -1;
	}

	/* Must connect to the server before anything else, as the escape function requires the connection handle.. */
	tris_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	}


	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	ESCAPE_STRING(buf1, keyfield);
	ESCAPE_STRING(buf2, lookup);
	tris_str_set(&sql, 0, "DELETE FROM %s WHERE %s = '%s'", table, tris_str_buffer(buf1), tris_str_buffer(buf2));
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		ESCAPE_STRING(buf1, newparam);
		ESCAPE_STRING(buf2, newval);
		tris_str_append(&sql, 0, " AND %s = '%s'", tris_str_buffer(buf1), tris_str_buffer(buf2));
	}
	va_end(ap);

	tris_debug(1, "PostgreSQL RealTime: Delete SQL: %s\n", tris_str_buffer(sql));

	if (!(result = PQexec(pgsqlConn, tris_str_buffer(sql)))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
		tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		tris_mutex_unlock(&pgsql_lock);
		return -1;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			tris_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
			tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			tris_mutex_unlock(&pgsql_lock);
			return -1;
		}
	}

	numrows = atoi(PQcmdTuples(result));
	tris_mutex_unlock(&pgsql_lock);

	tris_debug(1, "PostgreSQL RealTime: Deleted %d rows on table: %s\n", numrows, table);

	/* From http://dev.pgsql.com/doc/pgsql/en/pgsql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	 */

	if (numrows >= 0)
		return (int) numrows;

	return -1;
}


static struct tris_config *config_pgsql(const char *database, const char *table,
									   const char *file, struct tris_config *cfg,
									   struct tris_flags flags, const char *suggested_incl, const char *who_asked)
{
	PGresult *result = NULL;
	long num_rows;
	struct tris_variable *new_v;
	struct tris_category *cur_cat = NULL;
	struct tris_str *sql = tris_str_thread_get(&sql_buf, 100);
	char last[80] = "";
	int last_cat_metric = 0;

	last[0] = '\0';

	if (!file || !strcmp(file, RES_CONFIG_PGSQL_CONF)) {
		tris_log(LOG_WARNING, "PostgreSQL RealTime: Cannot configure myself.\n");
		return NULL;
	}

	tris_str_set(&sql, 0, "SELECT category, var_name, var_val, cat_metric FROM %s "
			"WHERE filename='%s' and commented=0"
			"ORDER BY cat_metric DESC, var_metric ASC, category, var_name ", table, file);

	tris_debug(1, "PostgreSQL RealTime: Static SQL: %s\n", tris_str_buffer(sql));

	/* We now have our complete statement; Lets connect to the server and execute it. */
	tris_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		tris_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	if (!(result = PQexec(pgsqlConn, tris_str_buffer(sql)))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query '%s@%s'. Check debug for more info.\n", table, database);
		tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
		tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		tris_mutex_unlock(&pgsql_lock);
		return NULL;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			tris_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			tris_debug(1, "PostgreSQL RealTime: Query: %s\n", tris_str_buffer(sql));
			tris_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			tris_mutex_unlock(&pgsql_lock);
			return NULL;
		}
	}

	if ((num_rows = PQntuples(result)) > 0) {
		int rowIndex = 0;

		tris_debug(1, "PostgreSQL RealTime: Found %ld rows.\n", num_rows);

		for (rowIndex = 0; rowIndex < num_rows; rowIndex++) {
			char *field_category = PQgetvalue(result, rowIndex, 0);
			char *field_var_name = PQgetvalue(result, rowIndex, 1);
			char *field_var_val = PQgetvalue(result, rowIndex, 2);
			char *field_cat_metric = PQgetvalue(result, rowIndex, 3);
			if (!strcmp(field_var_name, "#include")) {
				if (!tris_config_internal_load(field_var_val, cfg, flags, "", who_asked)) {
					PQclear(result);
					tris_mutex_unlock(&pgsql_lock);
					return NULL;
				}
				continue;
			}

			if (strcmp(last, field_category) || last_cat_metric != atoi(field_cat_metric)) {
				cur_cat = tris_category_new(field_category, "", 99999);
				if (!cur_cat)
					break;
				strcpy(last, field_category);
				last_cat_metric = atoi(field_cat_metric);
				tris_category_append(cfg, cur_cat);
			}
			new_v = tris_variable_new(field_var_name, field_var_val, "");
			tris_variable_append(cur_cat, new_v);
		}
	} else {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Could not find config '%s' in database.\n", file);
	}

	PQclear(result);
	tris_mutex_unlock(&pgsql_lock);

	return cfg;
}

static int require_pgsql(const char *database, const char *tablename, va_list ap)
{
	struct columns *column;
	struct tables *table = find_table(tablename);
	char *elm;
	int type, size, res = 0;

	if (!table) {
		tris_log(LOG_WARNING, "Table %s not found in database.  This table should exist if you're using realtime.\n", tablename);
		return -1;
	}

	while ((elm = va_arg(ap, char *))) {
		type = va_arg(ap, require_type);
		size = va_arg(ap, int);
		TRIS_LIST_TRAVERSE(&table->columns, column, list) {
			if (strcmp(column->name, elm) == 0) {
				/* Char can hold anything, as long as it is large enough */
				if ((strncmp(column->type, "char", 4) == 0 || strncmp(column->type, "varchar", 7) == 0 || strcmp(column->type, "bpchar") == 0)) {
					if ((size > column->len) && column->len != -1) {
						tris_log(LOG_WARNING, "Column '%s' should be at least %d long, but is only %d long.\n", column->name, size, column->len);
						res = -1;
					}
				} else if (strncmp(column->type, "int", 3) == 0) {
					int typesize = atoi(column->type + 3);
					/* Integers can hold only other integers */
					if ((type == RQ_INTEGER8 || type == RQ_UINTEGER8 ||
						type == RQ_INTEGER4 || type == RQ_UINTEGER4 ||
						type == RQ_INTEGER3 || type == RQ_UINTEGER3 ||
						type == RQ_UINTEGER2) && typesize == 2) {
						tris_log(LOG_WARNING, "Column '%s' may not be large enough for the required data length: %d\n", column->name, size);
						res = -1;
					} else if ((type == RQ_INTEGER8 || type == RQ_UINTEGER8 ||
						type == RQ_UINTEGER4) && typesize == 4) {
						tris_log(LOG_WARNING, "Column '%s' may not be large enough for the required data length: %d\n", column->name, size);
						res = -1;
					} else if (type == RQ_CHAR || type == RQ_DATETIME || type == RQ_FLOAT || type == RQ_DATE) {
						tris_log(LOG_WARNING, "Column '%s' is of the incorrect type: (need %s(%d) but saw %s)\n",
							column->name,
								type == RQ_CHAR ? "char" :
								type == RQ_DATETIME ? "datetime" :
								type == RQ_DATE ? "date" :
								type == RQ_FLOAT ? "float" :
								"a rather stiff drink ",
							size, column->type);
						res = -1;
					}
				} else if (strncmp(column->type, "float", 5) == 0 && !tris_rq_is_int(type) && type != RQ_FLOAT) {
					tris_log(LOG_WARNING, "Column %s cannot be a %s\n", column->name, column->type);
					res = -1;
				} else { /* There are other types that no module implements yet */
					tris_log(LOG_WARNING, "Possibly unsupported column type '%s' on column '%s'\n", column->type, column->name);
					res = -1;
				}
				break;
			}
		}

		if (!column) {
			if (requirements == RQ_WARN) {
				tris_log(LOG_WARNING, "Table %s requires a column '%s' of size '%d', but no such column exists.\n", tablename, elm, size);
			} else {
				struct tris_str *sql = tris_str_create(100);
				char fieldtype[15];
				PGresult *result;

				if (requirements == RQ_CREATECHAR || type == RQ_CHAR) {
					/* Size is minimum length; make it at least 50% greater,
					 * just to be sure, because PostgreSQL doesn't support
					 * resizing columns. */
					snprintf(fieldtype, sizeof(fieldtype), "CHAR(%d)",
						size < 15 ? size * 2 :
						(size * 3 / 2 > 255) ? 255 : size * 3 / 2);
				} else if (type == RQ_INTEGER1 || type == RQ_UINTEGER1 || type == RQ_INTEGER2) {
					snprintf(fieldtype, sizeof(fieldtype), "INT2");
				} else if (type == RQ_UINTEGER2 || type == RQ_INTEGER3 || type == RQ_UINTEGER3 || type == RQ_INTEGER4) {
					snprintf(fieldtype, sizeof(fieldtype), "INT4");
				} else if (type == RQ_UINTEGER4 || type == RQ_INTEGER8) {
					snprintf(fieldtype, sizeof(fieldtype), "INT8");
				} else if (type == RQ_UINTEGER8) {
					/* No such type on PostgreSQL */
					snprintf(fieldtype, sizeof(fieldtype), "CHAR(20)");
				} else if (type == RQ_FLOAT) {
					snprintf(fieldtype, sizeof(fieldtype), "FLOAT8");
				} else if (type == RQ_DATE) {
					snprintf(fieldtype, sizeof(fieldtype), "DATE");
				} else if (type == RQ_DATETIME) {
					snprintf(fieldtype, sizeof(fieldtype), "TIMESTAMP");
				} else {
					tris_log(LOG_ERROR, "Unrecognized request type %d\n", type);
					tris_free(sql);
					continue;
				}
				tris_str_set(&sql, 0, "ALTER TABLE %s ADD COLUMN %s %s", tablename, elm, fieldtype);
				tris_debug(1, "About to lock pgsql_lock (running alter on table '%s' to add column '%s')\n", tablename, elm);

				tris_mutex_lock(&pgsql_lock);
				if (!pgsql_reconnect(database)) {
					tris_mutex_unlock(&pgsql_lock);
					tris_log(LOG_ERROR, "Unable to add column: %s\n", tris_str_buffer(sql));
					tris_free(sql);
					continue;
				}

				tris_debug(1, "About to run ALTER query on table '%s' to add column '%s'\n", tablename, elm);
				result = PQexec(pgsqlConn, tris_str_buffer(sql));
				tris_debug(1, "Finished running ALTER query on table '%s'\n", tablename);
				if (PQresultStatus(result) != PGRES_COMMAND_OK) {
					tris_log(LOG_ERROR, "Unable to add column: %s\n", tris_str_buffer(sql));
				}
				PQclear(result);
				tris_mutex_unlock(&pgsql_lock);

				tris_free(sql);
			}
		}
	}
	release_table(table);
	return res;
}

static int unload_pgsql(const char *database, const char *tablename)
{
	struct tables *cur;
	tris_debug(2, "About to lock table cache list\n");
	TRIS_LIST_LOCK(&psql_tables);
	tris_debug(2, "About to traverse table cache list\n");
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&psql_tables, cur, list) {
		if (strcmp(cur->name, tablename) == 0) {
			tris_debug(2, "About to remove matching cache entry\n");
			TRIS_LIST_REMOVE_CURRENT(list);
			tris_debug(2, "About to destroy matching cache entry\n");
			destroy_table(cur);
			tris_debug(1, "Cache entry '%s@%s' destroyed\n", tablename, database);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END
	TRIS_LIST_UNLOCK(&psql_tables);
	tris_debug(2, "About to return\n");
	return cur ? 0 : -1;
}

static struct tris_config_engine pgsql_engine = {
	.name = "pgsql",
	.load_func = config_pgsql,
	.realtime_func = realtime_pgsql,
	.realtime_multi_func = realtime_multi_pgsql,
	.store_func = store_pgsql,
	.destroy_func = destroy_pgsql,
	.update_func = update_pgsql,
	.update2_func = update2_pgsql,
	.require_func = require_pgsql,
	.unload_func = unload_pgsql,
};

static int load_module(void)
{
	if(!parse_config(0))
		return TRIS_MODULE_LOAD_DECLINE;

	tris_config_engine_register(&pgsql_engine);
	tris_verb(1, "PostgreSQL RealTime driver loaded.\n");
	tris_cli_register_multiple(cli_realtime, ARRAY_LEN(cli_realtime));

	return 0;
}

static int unload_module(void)
{
	struct tables *table;
	/* Acquire control before doing anything to the module itself. */
	tris_mutex_lock(&pgsql_lock);

	if (pgsqlConn) {
		PQfinish(pgsqlConn);
		pgsqlConn = NULL;
	}
	tris_cli_unregister_multiple(cli_realtime, ARRAY_LEN(cli_realtime));
	tris_config_engine_deregister(&pgsql_engine);
	tris_verb(1, "PostgreSQL RealTime unloaded.\n");

	/* Destroy cached table info */
	TRIS_LIST_LOCK(&psql_tables);
	while ((table = TRIS_LIST_REMOVE_HEAD(&psql_tables, list))) {
		destroy_table(table);
	}
	TRIS_LIST_UNLOCK(&psql_tables);

	/* Unlock so something else can destroy the lock. */
	tris_mutex_unlock(&pgsql_lock);

	return 0;
}

static int reload(void)
{
	parse_config(1);

	return 0;
}

static int parse_config(int is_reload)
{
	struct tris_config *config;
	const char *s;
	struct tris_flags config_flags = { is_reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	config = tris_config_load(RES_CONFIG_PGSQL_CONF, config_flags);
	if (config == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	}

	if (config == CONFIG_STATUS_FILEMISSING || config == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING, "Unable to load config %s\n", RES_CONFIG_PGSQL_CONF);
		return 0;
	}

	tris_mutex_lock(&pgsql_lock);

	if (pgsqlConn) {
		PQfinish(pgsqlConn);
		pgsqlConn = NULL;
	}

	if (!(s = tris_variable_retrieve(config, "general", "dbuser"))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: No database user found, using 'trismedia' as default.\n");
		strcpy(dbuser, "trismedia");
	} else {
		tris_copy_string(dbuser, s, sizeof(dbuser));
	}

	if (!(s = tris_variable_retrieve(config, "general", "dbpass"))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: No database password found, using 'trismedia' as default.\n");
		strcpy(dbpass, "trismedia");
	} else {
		tris_copy_string(dbpass, s, sizeof(dbpass));
	}

	if (!(s = tris_variable_retrieve(config, "general", "dbhost"))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: No database host found, using localhost via socket.\n");
		dbhost[0] = '\0';
	} else {
		tris_copy_string(dbhost, s, sizeof(dbhost));
	}

	if (!(s = tris_variable_retrieve(config, "general", "dbname"))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: No database name found, using 'trismedia' as default.\n");
		strcpy(dbname, "trismedia");
	} else {
		tris_copy_string(dbname, s, sizeof(dbname));
	}

	if (!(s = tris_variable_retrieve(config, "general", "dbport"))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: No database port found, using 5432 as default.\n");
		dbport = 5432;
	} else {
		dbport = atoi(s);
	}

	if (!tris_strlen_zero(dbhost)) {
		/* No socket needed */
	} else if (!(s = tris_variable_retrieve(config, "general", "dbsock"))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: No database socket found, using '/tmp/pgsql.sock' as default.\n");
		strcpy(dbsock, "/tmp/pgsql.sock");
	} else {
		tris_copy_string(dbsock, s, sizeof(dbsock));
	}

	if (!(s = tris_variable_retrieve(config, "general", "requirements"))) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: no requirements setting found, using 'warn' as default.\n");
		requirements = RQ_WARN;
	} else if (!strcasecmp(s, "createclose")) {
		requirements = RQ_CREATECLOSE;
	} else if (!strcasecmp(s, "createchar")) {
		requirements = RQ_CREATECHAR;
	}

	tris_config_destroy(config);

	if (option_debug) {
		if (!tris_strlen_zero(dbhost)) {
			tris_debug(1, "PostgreSQL RealTime Host: %s\n", dbhost);
			tris_debug(1, "PostgreSQL RealTime Port: %i\n", dbport);
		} else {
			tris_debug(1, "PostgreSQL RealTime Socket: %s\n", dbsock);
		}
		tris_debug(1, "PostgreSQL RealTime User: %s\n", dbuser);
		tris_debug(1, "PostgreSQL RealTime Password: %s\n", dbpass);
		tris_debug(1, "PostgreSQL RealTime DBName: %s\n", dbname);
	}

	if (!pgsql_reconnect(NULL)) {
		tris_log(LOG_WARNING,
				"PostgreSQL RealTime: Couldn't establish connection. Check debug.\n");
		tris_debug(1, "PostgreSQL RealTime: Cannot Connect: %s\n", PQerrorMessage(pgsqlConn));
	}

	tris_verb(2, "PostgreSQL RealTime reloaded.\n");

	/* Done reloading. Release lock so others can now use driver. */
	tris_mutex_unlock(&pgsql_lock);

	return 1;
}

static int pgsql_reconnect(const char *database)
{
	char my_database[50];

	tris_copy_string(my_database, S_OR(database, dbname), sizeof(my_database));

	/* mutex lock should have been locked before calling this function. */

	if (pgsqlConn && PQstatus(pgsqlConn) != CONNECTION_OK) {
		PQfinish(pgsqlConn);
		pgsqlConn = NULL;
	}

	/* DB password can legitimately be 0-length */
	if ((!pgsqlConn) && (!tris_strlen_zero(dbhost) || !tris_strlen_zero(dbsock)) && !tris_strlen_zero(dbuser) && !tris_strlen_zero(my_database)) {
		struct tris_str *connInfo = tris_str_create(32);

		tris_str_set(&connInfo, 0, "host=%s port=%d dbname=%s user=%s",
			dbhost, dbport, my_database, dbuser);
		if (!tris_strlen_zero(dbpass))
			tris_str_append(&connInfo, 0, " password=%s", dbpass);

		tris_debug(1, "%u connInfo=%s\n", (unsigned int)tris_str_size(connInfo), tris_str_buffer(connInfo));
		pgsqlConn = PQconnectdb(tris_str_buffer(connInfo));
		tris_debug(1, "%u connInfo=%s\n", (unsigned int)tris_str_size(connInfo), tris_str_buffer(connInfo));
		tris_free(connInfo);
		connInfo = NULL;

		tris_debug(1, "pgsqlConn=%p\n", pgsqlConn);
		if (pgsqlConn && PQstatus(pgsqlConn) == CONNECTION_OK) {
			tris_debug(1, "PostgreSQL RealTime: Successfully connected to database.\n");
			connect_time = time(NULL);
			version = PQserverVersion(pgsqlConn);
			return 1;
		} else {
			tris_log(LOG_ERROR,
					"PostgreSQL RealTime: Failed to connect database %s on %s: %s\n",
					dbname, dbhost, PQresultErrorMessage(NULL));
			return 0;
		}
	} else {
		tris_debug(1, "PostgreSQL RealTime: One or more of the parameters in the config does not pass our validity checks.\n");
		return 1;
	}
}

static char *handle_cli_realtime_pgsql_cache(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tables *cur;
	int l, which;
	char *ret = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime show pgsql cache";
		e->usage =
			"Usage: realtime show pgsql cache [<table>]\n"
			"       Shows table cache for the PostgreSQL RealTime driver\n";
		return NULL;
	case CLI_GENERATE:
		if (a->argc != 4) {
			return NULL;
		}
		l = strlen(a->word);
		which = 0;
		TRIS_LIST_LOCK(&psql_tables);
		TRIS_LIST_TRAVERSE(&psql_tables, cur, list) {
			if (!strncasecmp(a->word, cur->name, l) && ++which > a->n) {
				ret = tris_strdup(cur->name);
				break;
			}
		}
		TRIS_LIST_UNLOCK(&psql_tables);
		return ret;
	}

	if (a->argc == 4) {
		/* List of tables */
		TRIS_LIST_LOCK(&psql_tables);
		TRIS_LIST_TRAVERSE(&psql_tables, cur, list) {
			tris_cli(a->fd, "%s\n", cur->name);
		}
		TRIS_LIST_UNLOCK(&psql_tables);
	} else if (a->argc == 5) {
		/* List of columns */
		if ((cur = find_table(a->argv[4]))) {
			struct columns *col;
			tris_cli(a->fd, "Columns for Table Cache '%s':\n", a->argv[4]);
			tris_cli(a->fd, "%-20.20s %-20.20s %-3.3s %-8.8s\n", "Name", "Type", "Len", "Nullable");
			TRIS_LIST_TRAVERSE(&cur->columns, col, list) {
				tris_cli(a->fd, "%-20.20s %-20.20s %3d %-8.8s\n", col->name, col->type, col->len, col->notnull ? "NOT NULL" : "");
			}
			release_table(cur);
		} else {
			tris_cli(a->fd, "No such table '%s'\n", a->argv[4]);
		}
	}
	return 0;
}

static char *handle_cli_realtime_pgsql_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	char status[256], credentials[100] = "";
	int ctimesec = time(NULL) - connect_time;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime show pgsql status";
		e->usage =
			"Usage: realtime show pgsql status\n"
			"       Shows connection information for the PostgreSQL RealTime driver\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	if (pgsqlConn && PQstatus(pgsqlConn) == CONNECTION_OK) {
		if (!tris_strlen_zero(dbhost))
			snprintf(status, sizeof(status), "Connected to %s@%s, port %d", dbname, dbhost, dbport);
		else if (!tris_strlen_zero(dbsock))
			snprintf(status, sizeof(status), "Connected to %s on socket file %s", dbname, dbsock);
		else
			snprintf(status, sizeof(status), "Connected to %s@%s", dbname, dbhost);

		if (!tris_strlen_zero(dbuser))
			snprintf(credentials, sizeof(credentials), " with username %s", dbuser);

		if (ctimesec > 31536000)
			tris_cli(a->fd, "%s%s for %d years, %d days, %d hours, %d minutes, %d seconds.\n",
					status, credentials, ctimesec / 31536000, (ctimesec % 31536000) / 86400,
					(ctimesec % 86400) / 3600, (ctimesec % 3600) / 60, ctimesec % 60);
		else if (ctimesec > 86400)
			tris_cli(a->fd, "%s%s for %d days, %d hours, %d minutes, %d seconds.\n", status,
					credentials, ctimesec / 86400, (ctimesec % 86400) / 3600, (ctimesec % 3600) / 60,
					ctimesec % 60);
		else if (ctimesec > 3600)
			tris_cli(a->fd, "%s%s for %d hours, %d minutes, %d seconds.\n", status, credentials,
					ctimesec / 3600, (ctimesec % 3600) / 60, ctimesec % 60);
		else if (ctimesec > 60)
			tris_cli(a->fd, "%s%s for %d minutes, %d seconds.\n", status, credentials, ctimesec / 60,
					ctimesec % 60);
		else
			tris_cli(a->fd, "%s%s for %d seconds.\n", status, credentials, ctimesec);

		return CLI_SUCCESS;
	} else {
		return CLI_FAILURE;
	}
}

/* needs usecount semantics defined */
TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_GLOBAL_SYMBOLS, "PostgreSQL RealTime Configuration Driver",
		.load = load_module,
		.unload = unload_module,
		.reload = reload
	       );
