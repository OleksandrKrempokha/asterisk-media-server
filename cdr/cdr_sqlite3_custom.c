/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2007, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com> and others.
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
 * \brief Custom SQLite3 CDR records.
 *
 * \author Adapted by Alejandro Rios <alejandro.rios@avatar.com.co> and
 *  Russell Bryant <russell@digium.com> from
 *  cdr_mysql_custom by Edward Eastman <ed@dm3.co.uk>,
 *	and cdr_sqlite by Holger Schurig <hs4233@mail.mn-solutions.de>
 *
 *
 * \arg See also \ref AstCDR
 *
 *
 * \ingroup cdr_drivers
 */

/*** MODULEINFO
	<depend>sqlite3</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 223173 $")

#include <time.h>
#include <sqlite3.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_LOG_DIR */
#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/cli.h"

TRIS_MUTEX_DEFINE_STATIC(lock);

static const char config_file[] = "cdr_sqlite3_custom.conf";

static char *desc = "Customizable SQLite3 CDR Backend";
static char *name = "cdr_sqlite3_custom";
static sqlite3 *db = NULL;

static char table[80];
static char *columns;

struct values {
	char *expression;
	TRIS_LIST_ENTRY(values) list;
};

static TRIS_LIST_HEAD_STATIC(sql_values, values);

static void free_config(int reload);

static int load_column_config(const char *tmp)
{
	char *col = NULL;
	char *cols = NULL, *save = NULL;
	char *escaped = NULL;
	struct tris_str *column_string = NULL;

	if (tris_strlen_zero(tmp)) {
		tris_log(LOG_WARNING, "Column names not specified. Module not loaded.\n");
		return -1;
	}
	if (!(column_string = tris_str_create(1024))) {
		tris_log(LOG_ERROR, "Out of memory creating temporary buffer for column list for table '%s.'\n", table);
		return -1;
	}
	if (!(save = cols = tris_strdup(tmp))) {
		tris_log(LOG_ERROR, "Out of memory creating temporary buffer for column list for table '%s.'\n", table);
		tris_free(column_string);
		return -1;
	}
	while ((col = strsep(&cols, ","))) {
		col = tris_strip(col);
		escaped = sqlite3_mprintf("%q", col);
		if (!escaped) {
			tris_log(LOG_ERROR, "Out of memory creating entry for column '%s' in table '%s.'\n", col, table);
			tris_free(column_string);
			tris_free(save);
			return -1;
		}
		tris_str_append(&column_string, 0, "%s%s", tris_str_strlen(column_string) ? "," : "", escaped);
		sqlite3_free(escaped);
	}
	if (!(columns = tris_strdup(tris_str_buffer(column_string)))) {
		tris_log(LOG_ERROR, "Out of memory copying columns string for table '%s.'\n", table);
		tris_free(column_string);
		tris_free(save);
		return -1;
	}
	tris_free(column_string);
	tris_free(save);

	return 0;
}

static int load_values_config(const char *tmp)
{
	char *val = NULL;
	char *vals = NULL, *save = NULL;
	struct values *value = NULL;

	if (tris_strlen_zero(tmp)) {
		tris_log(LOG_WARNING, "Values not specified. Module not loaded.\n");
		return -1;
	}
	if (!(save = vals = tris_strdup(tmp))) {
		tris_log(LOG_ERROR, "Out of memory creating temporary buffer for value '%s'\n", tmp);
		return -1;
	}
	while ((val = strsep(&vals, ","))) {
		/* Strip the single quotes off if they are there */
		val = tris_strip_quoted(val, "'", "'");
		value = tris_calloc(sizeof(char), sizeof(*value) + strlen(val) + 1);
		if (!value) {
			tris_log(LOG_ERROR, "Out of memory creating entry for value '%s'\n", val);
			tris_free(save);
			return -1;
		}
		value->expression = (char *) value + sizeof(*value);
		tris_copy_string(value->expression, val, strlen(val) + 1);
		TRIS_LIST_INSERT_TAIL(&sql_values, value, list);
	}
	tris_free(save);

	return 0;
}

static int load_config(int reload)
{
	struct tris_config *cfg;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	struct tris_variable *mappingvar;
	const char *tmp;

	if ((cfg = tris_config_load(config_file, config_flags)) == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING, "Failed to %sload configuration file. %s\n", reload ? "re" : "", reload ? "" : "Module not activated.");
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	}

	if (reload) {
		free_config(1);
	}

	if (!(mappingvar = tris_variable_browse(cfg, "master"))) {
		/* Nothing configured */
		tris_config_destroy(cfg);
		return -1;
	}

	/* Mapping must have a table name */
	if (!tris_strlen_zero(tmp = tris_variable_retrieve(cfg, "master", "table"))) {
		tris_copy_string(table, tmp, sizeof(table));
	} else {
		tris_log(LOG_WARNING, "Table name not specified.  Assuming cdr.\n");
		strcpy(table, "cdr");
	}

	/* Columns */
	if (load_column_config(tris_variable_retrieve(cfg, "master", "columns"))) {
		tris_config_destroy(cfg);
		free_config(0);
		return -1;
	}

	/* Values */
	if (load_values_config(tris_variable_retrieve(cfg, "master", "values"))) {
		tris_config_destroy(cfg);
		free_config(0);
		return -1;
	}

	tris_verb(3, "cdr_sqlite3_custom: Logging CDR records to table '%s' in 'master.db'\n", table);

	tris_config_destroy(cfg);

	return 0;
}

static void free_config(int reload)
{
	struct values *value;

	if (!reload && db) {
		sqlite3_close(db);
		db = NULL;
	}

	if (columns) {
		tris_free(columns);
		columns = NULL;
	}

	while ((value = TRIS_LIST_REMOVE_HEAD(&sql_values, list))) {
		tris_free(value);
	}
}

static int sqlite3_mylog(struct tris_cdr *cdr)
{
	int res = 0;
	char *error = NULL;
	char *sql = NULL;
	struct tris_channel dummy = { 0, };
	int count = 0;

	if (db == NULL) {
		/* Should not have loaded, but be failsafe. */
		return 0;
	}

	tris_mutex_lock(&lock);

	{ /* Make it obvious that only sql should be used outside of this block */
		char *escaped;
		char subst_buf[2048];
		struct values *value;
		struct tris_str *value_string = tris_str_create(1024);
		dummy.cdr = cdr;
		TRIS_LIST_TRAVERSE(&sql_values, value, list) {
			pbx_substitute_variables_helper(&dummy, value->expression, subst_buf, sizeof(subst_buf) - 1);
			escaped = sqlite3_mprintf("%q", subst_buf);
			tris_str_append(&value_string, 0, "%s'%s'", tris_str_strlen(value_string) ? "," : "", escaped);
			sqlite3_free(escaped);
		}
		sql = sqlite3_mprintf("INSERT INTO %q (%s) VALUES (%s)", table, columns, tris_str_buffer(value_string));
		tris_debug(1, "About to log: %s\n", sql);
		tris_free(value_string);
	}

	/* XXX This seems awful arbitrary... */
	for (count = 0; count < 5; count++) {
		res = sqlite3_exec(db, sql, NULL, NULL, &error);
		if (res != SQLITE_BUSY && res != SQLITE_LOCKED) {
			break;
		}
		usleep(200);
	}

	if (error) {
		tris_log(LOG_ERROR, "%s. SQL: %s.\n", error, sql);
		sqlite3_free(error);
	}

	if (sql) {
		sqlite3_free(sql);
	}

	tris_mutex_unlock(&lock);

	return res;
}

static int unload_module(void)
{
	tris_cdr_unregister(name);

	free_config(0);

	return 0;
}

static int load_module(void)
{
	char *error;
	char filename[PATH_MAX];
	int res;
	char *sql;

	if (load_config(0)) {
		return TRIS_MODULE_LOAD_DECLINE;
	}

	/* is the database there? */
	snprintf(filename, sizeof(filename), "%s/master.db", tris_config_TRIS_LOG_DIR);
	res = sqlite3_open(filename, &db);
	if (res != SQLITE_OK) {
		tris_log(LOG_ERROR, "Could not open database %s.\n", filename);
		free_config(0);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	/* is the table there? */
	sql = sqlite3_mprintf("SELECT COUNT(AcctId) FROM %q;", table);
	res = sqlite3_exec(db, sql, NULL, NULL, NULL);
	sqlite3_free(sql);
	if (res != SQLITE_OK) {
		/* We don't use %q for the column list here since we already escaped when building it */
		sql = sqlite3_mprintf("CREATE TABLE %q (AcctId INTEGER PRIMARY KEY, %s)", table, columns);
		res = sqlite3_exec(db, sql, NULL, NULL, &error);
		sqlite3_free(sql);
		if (res != SQLITE_OK) {
			tris_log(LOG_WARNING, "Unable to create table '%s': %s.\n", table, error);
			sqlite3_free(error);
			free_config(0);
			return TRIS_MODULE_LOAD_DECLINE;
		}
	}

	res = tris_cdr_register(name, desc, sqlite3_mylog);
	if (res) {
		tris_log(LOG_ERROR, "Unable to register custom SQLite3 CDR handling\n");
		free_config(0);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	return TRIS_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	int res = 0;

	tris_mutex_lock(&lock);
	res = load_config(1);
	tris_mutex_unlock(&lock);

	return res;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "SQLite3 Custom CDR Module",
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
);
