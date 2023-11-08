/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2003 - 2006
 *
 * Matthew D. Hardeman <mhardemn@papersoft.com>
 * Adapted from the MySQL CDR logger originally by James Sharp
 *
 * Modified September 2003
 * Matthew D. Hardeman <mhardemn@papersoft.com>
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
 * \brief PostgreSQL CDR logger
 *
 * \author Matthew D. Hardeman <mhardemn@papersoft.com>
 * \extref PostgreSQL http://www.postgresql.org/
 *
 * See also
 * \arg \ref Config_cdr
 * \arg http://www.postgresql.org/
 * \ingroup cdr_drivers
 */

/*** MODULEINFO
	<depend>pgsql</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 222311 $")

#include <time.h>

#include <libpq-fe.h>

#include "trismedia/config.h"
#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/module.h"

#define DATE_FORMAT "'%Y-%m-%d %T'"

static char *name = "pgsql";
static char *config = "cdr_pgsql.conf";
static char *pghostname = NULL, *pgdbname = NULL, *pgdbuser = NULL, *pgpassword = NULL, *pgdbport = NULL, *table = NULL;
static int connected = 0;
static int maxsize = 512, maxsize2 = 512;

TRIS_MUTEX_DEFINE_STATIC(pgsql_lock);

static PGconn	*conn = NULL;

struct columns {
	char *name;
	char *type;
	int len;
	unsigned int notnull:1;
	unsigned int hasdefault:1;
	TRIS_RWLIST_ENTRY(columns) list;
};

static TRIS_RWLIST_HEAD_STATIC(psql_columns, columns);

#define LENGTHEN_BUF1(size)                                               \
			do {                                                          \
				/* Lengthen buffer, if necessary */                       \
				if (tris_str_strlen(sql) + size + 1 > tris_str_size(sql)) { \
					if (tris_str_make_space(&sql, ((tris_str_size(sql) + size + 3) / 512 + 1) * 512) != 0) {	\
						tris_log(LOG_ERROR, "Unable to allocate sufficient memory.  Insert CDR failed.\n"); \
						tris_free(sql);                                    \
						tris_free(sql2);                                   \
						TRIS_RWLIST_UNLOCK(&psql_columns);                 \
						return -1;                                        \
					}                                                     \
				}                                                         \
			} while (0)

#define LENGTHEN_BUF2(size)                               \
			do {                                          \
				if (tris_str_strlen(sql2) + size + 1 > tris_str_size(sql2)) {  \
					if (tris_str_make_space(&sql2, ((tris_str_size(sql2) + size + 3) / 512 + 1) * 512) != 0) {	\
						tris_log(LOG_ERROR, "Unable to allocate sufficient memory.  Insert CDR failed.\n");	\
						tris_free(sql);                    \
						tris_free(sql2);                   \
						TRIS_RWLIST_UNLOCK(&psql_columns); \
						return -1;                        \
					}                                     \
				}                                         \
			} while (0)

static int pgsql_log(struct tris_cdr *cdr)
{
	struct tris_tm tm;
	char *pgerror;
	PGresult *result;

	tris_mutex_lock(&pgsql_lock);

	if ((!connected) && pghostname && pgdbuser && pgpassword && pgdbname) {
		conn = PQsetdbLogin(pghostname, pgdbport, NULL, NULL, pgdbname, pgdbuser, pgpassword);
		if (PQstatus(conn) != CONNECTION_BAD) {
			connected = 1;
		} else {
			pgerror = PQerrorMessage(conn);
			tris_log(LOG_ERROR, "Unable to connect to database server %s.  Calls will not be logged!\n", pghostname);
			tris_log(LOG_ERROR, "Reason: %s\n", pgerror);
			PQfinish(conn);
			conn = NULL;
		}
	}

	if (connected) {
		struct columns *cur;
		struct tris_str *sql = tris_str_create(maxsize), *sql2 = tris_str_create(maxsize2);
		char buf[257], escapebuf[513], *value;
		int first = 1;
  
		if (!sql || !sql2) {
			if (sql) {
				tris_free(sql);
			}
			if (sql2) {
				tris_free(sql2);
			}
			return -1;
		}

		tris_str_set(&sql, 0, "INSERT INTO %s (", table);
		tris_str_set(&sql2, 0, " VALUES (");

		TRIS_RWLIST_RDLOCK(&psql_columns);
		TRIS_RWLIST_TRAVERSE(&psql_columns, cur, list) {
			/* For fields not set, simply skip them */
			tris_cdr_getvar(cdr, cur->name, &value, buf, sizeof(buf), 0, 0);
			if (strcmp(cur->name, "calldate") == 0 && !value) {
				tris_cdr_getvar(cdr, "start", &value, buf, sizeof(buf), 0, 0);
			}
			if (!value) {
				if (cur->notnull && !cur->hasdefault) {
					/* Field is NOT NULL (but no default), must include it anyway */
					LENGTHEN_BUF1(strlen(cur->name) + 2);
					tris_str_append(&sql, 0, "%s\"%s\"", first ? "" : ",", cur->name);
					LENGTHEN_BUF2(3);
					tris_str_append(&sql2, 0, "%s''", first ? "" : ",");
					first = 0;
				}
				continue;
			}

			LENGTHEN_BUF1(strlen(cur->name) + 2);
			tris_str_append(&sql, 0, "%s\"%s\"", first ? "" : ",", cur->name);

			if (strcmp(cur->name, "start") == 0 || strcmp(cur->name, "calldate") == 0) {
				if (strncmp(cur->type, "int", 3) == 0) {
					LENGTHEN_BUF2(13);
					tris_str_append(&sql2, 0, "%s%ld", first ? "" : ",", cdr->start.tv_sec);
				} else if (strncmp(cur->type, "float", 5) == 0) {
					LENGTHEN_BUF2(31);
					tris_str_append(&sql2, 0, "%s%f", first ? "" : ",", (double)cdr->start.tv_sec + (double)cdr->start.tv_usec / 1000000.0);
				} else {
					/* char, hopefully */
					LENGTHEN_BUF2(31);
					tris_localtime(&cdr->start, &tm, NULL);
					tris_strftime(buf, sizeof(buf), DATE_FORMAT, &tm);
					tris_str_append(&sql2, 0, "%s%s", first ? "" : ",", buf);
				}
			} else if (strcmp(cur->name, "answer") == 0) {
				if (strncmp(cur->type, "int", 3) == 0) {
					LENGTHEN_BUF2(13);
					tris_str_append(&sql2, 0, "%s%ld", first ? "" : ",", cdr->answer.tv_sec);
				} else if (strncmp(cur->type, "float", 5) == 0) {
					LENGTHEN_BUF2(31);
					tris_str_append(&sql2, 0, "%s%f", first ? "" : ",", (double)cdr->answer.tv_sec + (double)cdr->answer.tv_usec / 1000000.0);
				} else {
					/* char, hopefully */
					LENGTHEN_BUF2(31);
					tris_localtime(&cdr->start, &tm, NULL);
					tris_strftime(buf, sizeof(buf), DATE_FORMAT, &tm);
					tris_str_append(&sql2, 0, "%s%s", first ? "" : ",", buf);
				}
			} else if (strcmp(cur->name, "end") == 0) {
				if (strncmp(cur->type, "int", 3) == 0) {
					LENGTHEN_BUF2(13);
					tris_str_append(&sql2, 0, "%s%ld", first ? "" : ",", cdr->end.tv_sec);
				} else if (strncmp(cur->type, "float", 5) == 0) {
					LENGTHEN_BUF2(31);
					tris_str_append(&sql2, 0, "%s%f", first ? "" : ",", (double)cdr->end.tv_sec + (double)cdr->end.tv_usec / 1000000.0);
				} else {
					/* char, hopefully */
					LENGTHEN_BUF2(31);
					tris_localtime(&cdr->end, &tm, NULL);
					tris_strftime(buf, sizeof(buf), DATE_FORMAT, &tm);
					tris_str_append(&sql2, 0, "%s%s", first ? "" : ",", buf);
				}
			} else if (strcmp(cur->name, "duration") == 0 || strcmp(cur->name, "billsec") == 0) {
				if (cur->type[0] == 'i') {
					/* Get integer, no need to escape anything */
					tris_cdr_getvar(cdr, cur->name, &value, buf, sizeof(buf), 0, 0);
					LENGTHEN_BUF2(13);
					tris_str_append(&sql2, 0, "%s%s", first ? "" : ",", value);
				} else if (strncmp(cur->type, "float", 5) == 0) {
					struct timeval *when = cur->name[0] == 'd' ? &cdr->start : &cdr->answer;
					LENGTHEN_BUF2(31);
					tris_str_append(&sql2, 0, "%s%f", first ? "" : ",", (double)cdr->end.tv_sec - when->tv_sec + cdr->end.tv_usec / 1000000.0 - when->tv_usec / 1000000.0);
				} else {
					/* Char field, probably */
					struct timeval *when = cur->name[0] == 'd' ? &cdr->start : &cdr->answer;
					LENGTHEN_BUF2(31);
					tris_str_append(&sql2, 0, "%s'%f'", first ? "" : ",", (double)cdr->end.tv_sec - when->tv_sec + cdr->end.tv_usec / 1000000.0 - when->tv_usec / 1000000.0);
				}
			} else if (strcmp(cur->name, "disposition") == 0 || strcmp(cur->name, "amaflags") == 0) {
				if (strncmp(cur->type, "int", 3) == 0) {
					/* Integer, no need to escape anything */
					tris_cdr_getvar(cdr, cur->name, &value, buf, sizeof(buf), 0, 1);
					LENGTHEN_BUF2(13);
					tris_str_append(&sql2, 0, "%s%s", first ? "" : ",", value);
				} else {
					/* Although this is a char field, there are no special characters in the values for these fields */
					tris_cdr_getvar(cdr, cur->name, &value, buf, sizeof(buf), 0, 0);
					LENGTHEN_BUF2(31);
					tris_str_append(&sql2, 0, "%s'%s'", first ? "" : ",", value);
				}
			} else {
				/* Arbitrary field, could be anything */
				tris_cdr_getvar(cdr, cur->name, &value, buf, sizeof(buf), 0, 0);
				if (strncmp(cur->type, "int", 3) == 0) {
					long long whatever;
					if (value && sscanf(value, "%30lld", &whatever) == 1) {
						LENGTHEN_BUF2(26);
						tris_str_append(&sql2, 0, "%s%lld", first ? "" : ",", whatever);
					} else {
						LENGTHEN_BUF2(2);
						tris_str_append(&sql2, 0, "%s0", first ? "" : ",");
					}
				} else if (strncmp(cur->type, "float", 5) == 0) {
					long double whatever;
					if (value && sscanf(value, "%30Lf", &whatever) == 1) {
						LENGTHEN_BUF2(51);
						tris_str_append(&sql2, 0, "%s%30Lf", first ? "" : ",", whatever);
					} else {
						LENGTHEN_BUF2(2);
						tris_str_append(&sql2, 0, "%s0", first ? "" : ",");
					}
				/* XXX Might want to handle dates, times, and other misc fields here XXX */
				} else {
					if (value)
						PQescapeStringConn(conn, escapebuf, value, strlen(value), NULL);
					else
						escapebuf[0] = '\0';
					LENGTHEN_BUF2(strlen(escapebuf) + 3);
					tris_str_append(&sql2, 0, "%s'%s'", first ? "" : ",", escapebuf);
				}
			}
			first = 0;
  		}
		TRIS_RWLIST_UNLOCK(&psql_columns);
		LENGTHEN_BUF1(tris_str_strlen(sql2) + 2);
		tris_str_append(&sql, 0, ")%s)", tris_str_buffer(sql2));
		tris_verb(11, "[%s]\n", tris_str_buffer(sql));

		tris_debug(2, "inserting a CDR record.\n");

		/* Test to be sure we're still connected... */
		/* If we're connected, and connection is working, good. */
		/* Otherwise, attempt reconnect.  If it fails... sorry... */
		if (PQstatus(conn) == CONNECTION_OK) {
			connected = 1;
		} else {
			tris_log(LOG_ERROR, "Connection was lost... attempting to reconnect.\n");
			PQreset(conn);
			if (PQstatus(conn) == CONNECTION_OK) {
				tris_log(LOG_ERROR, "Connection reestablished.\n");
				connected = 1;
			} else {
				pgerror = PQerrorMessage(conn);
				tris_log(LOG_ERROR, "Unable to reconnect to database server %s. Calls will not be logged!\n", pghostname);
				tris_log(LOG_ERROR, "Reason: %s\n", pgerror);
				PQfinish(conn);
				conn = NULL;
				connected = 0;
				tris_mutex_unlock(&pgsql_lock);
				tris_free(sql);
				tris_free(sql2);
				return -1;
			}
		}
		result = PQexec(conn, tris_str_buffer(sql));
		if (PQresultStatus(result) != PGRES_COMMAND_OK) {
			pgerror = PQresultErrorMessage(result);
			tris_log(LOG_ERROR, "Failed to insert call detail record into database!\n");
			tris_log(LOG_ERROR, "Reason: %s\n", pgerror);
			tris_log(LOG_ERROR, "Connection may have been lost... attempting to reconnect.\n");
			PQreset(conn);
			if (PQstatus(conn) == CONNECTION_OK) {
				tris_log(LOG_ERROR, "Connection reestablished.\n");
				connected = 1;
				PQclear(result);
				result = PQexec(conn, tris_str_buffer(sql));
				if (PQresultStatus(result) != PGRES_COMMAND_OK) {
					pgerror = PQresultErrorMessage(result);
					tris_log(LOG_ERROR, "HARD ERROR!  Attempted reconnection failed.  DROPPING CALL RECORD!\n");
					tris_log(LOG_ERROR, "Reason: %s\n", pgerror);
				}
			}
			tris_mutex_unlock(&pgsql_lock);
			PQclear(result);
			tris_free(sql);
			tris_free(sql2);
			return -1;
		}
		PQclear(result);
		tris_free(sql);
		tris_free(sql2);
	}
	tris_mutex_unlock(&pgsql_lock);
	return 0;
}

static int unload_module(void)
{
	struct columns *current;
	tris_cdr_unregister(name);

	/* Give all threads time to finish */
	usleep(1);
	PQfinish(conn);

	if (pghostname)
		tris_free(pghostname);
	if (pgdbname)
		tris_free(pgdbname);
	if (pgdbuser)
		tris_free(pgdbuser);
	if (pgpassword)
		tris_free(pgpassword);
	if (pgdbport)
		tris_free(pgdbport);
	if (table)
		tris_free(table);

	TRIS_RWLIST_WRLOCK(&psql_columns);
	while ((current = TRIS_RWLIST_REMOVE_HEAD(&psql_columns, list))) {
		tris_free(current);
	}
	TRIS_RWLIST_UNLOCK(&psql_columns);

	return 0;
}

static int config_module(int reload)
{
	struct tris_variable *var;
	char *pgerror;
	struct columns *cur;
	PGresult *result;
	const char *tmp;
	struct tris_config *cfg;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	if ((cfg = tris_config_load(config, config_flags)) == NULL || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING, "Unable to load config for PostgreSQL CDR's: %s\n", config);
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED)
		return 0;

	if (!(var = tris_variable_browse(cfg, "global"))) {
		tris_config_destroy(cfg);
		return 0;
	}

	if (!(tmp = tris_variable_retrieve(cfg, "global", "hostname"))) {
		tris_log(LOG_WARNING, "PostgreSQL server hostname not specified.  Assuming unix socket connection\n");
		tmp = "";	/* connect via UNIX-socket by default */
	}

	if (pghostname)
		tris_free(pghostname);
	if (!(pghostname = tris_strdup(tmp))) {
		tris_config_destroy(cfg);
		return -1;
	}

	if (!(tmp = tris_variable_retrieve(cfg, "global", "dbname"))) {
		tris_log(LOG_WARNING, "PostgreSQL database not specified.  Assuming trismedia\n");
		tmp = "trismediacdrdb";
	}

	if (pgdbname)
		tris_free(pgdbname);
	if (!(pgdbname = tris_strdup(tmp))) {
		tris_config_destroy(cfg);
		return -1;
	}

	if (!(tmp = tris_variable_retrieve(cfg, "global", "user"))) {
		tris_log(LOG_WARNING, "PostgreSQL database user not specified.  Assuming trismedia\n");
		tmp = "trismedia";
	}

	if (pgdbuser)
		tris_free(pgdbuser);
	if (!(pgdbuser = tris_strdup(tmp))) {
		tris_config_destroy(cfg);
		return -1;
	}

	if (!(tmp = tris_variable_retrieve(cfg, "global", "password"))) {
		tris_log(LOG_WARNING, "PostgreSQL database password not specified.  Assuming blank\n");
		tmp = "";
	}

	if (pgpassword)
		tris_free(pgpassword);
	if (!(pgpassword = tris_strdup(tmp))) {
		tris_config_destroy(cfg);
		return -1;
	}

	if (!(tmp = tris_variable_retrieve(cfg, "global", "port"))) {
		tris_log(LOG_WARNING, "PostgreSQL database port not specified.  Using default 5432.\n");
		tmp = "5432";
	}

	if (pgdbport)
		tris_free(pgdbport);
	if (!(pgdbport = tris_strdup(tmp))) {
		tris_config_destroy(cfg);
		return -1;
	}

	if (!(tmp = tris_variable_retrieve(cfg, "global", "table"))) {
		tris_log(LOG_WARNING, "CDR table not specified.  Assuming cdr\n");
		tmp = "cdr";
	}

	if (table)
		tris_free(table);
	if (!(table = tris_strdup(tmp))) {
		tris_config_destroy(cfg);
		return -1;
	}

	if (option_debug) {
		if (tris_strlen_zero(pghostname)) {
			tris_debug(1, "using default unix socket\n");
		} else {
			tris_debug(1, "got hostname of %s\n", pghostname);
		}
		tris_debug(1, "got port of %s\n", pgdbport);
		tris_debug(1, "got user of %s\n", pgdbuser);
		tris_debug(1, "got dbname of %s\n", pgdbname);
		tris_debug(1, "got password of %s\n", pgpassword);
		tris_debug(1, "got sql table name of %s\n", table);
	}

	conn = PQsetdbLogin(pghostname, pgdbport, NULL, NULL, pgdbname, pgdbuser, pgpassword);
	if (PQstatus(conn) != CONNECTION_BAD) {
		char sqlcmd[768];
		char *fname, *ftype, *flen, *fnotnull, *fdef;
		int i, rows, version;
		tris_debug(1, "Successfully connected to PostgreSQL database.\n");
		connected = 1;
		version = PQserverVersion(conn);

		if (version >= 70300) {
			char *schemaname, *tablename;
			if (strchr(table, '.')) {
				schemaname = tris_strdupa(table);
				tablename = strchr(schemaname, '.');
				*tablename++ = '\0';
			} else {
				schemaname = "";
				tablename = table;
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

			snprintf(sqlcmd, sizeof(sqlcmd), "SELECT a.attname, t.typname, a.attlen, a.attnotnull, d.adsrc, a.atttypmod FROM (((pg_catalog.pg_class c INNER JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace AND c.relname = '%s' AND n.nspname = %s%s%s) INNER JOIN pg_catalog.pg_attribute a ON (NOT a.attisdropped) AND a.attnum > 0 AND a.attrelid = c.oid) INNER JOIN pg_catalog.pg_type t ON t.oid = a.atttypid) LEFT OUTER JOIN pg_attrdef d ON a.atthasdef AND d.adrelid = a.attrelid AND d.adnum = a.attnum ORDER BY n.nspname, c.relname, attnum",
				tablename,
				tris_strlen_zero(schemaname) ? "" : "'", tris_strlen_zero(schemaname) ? "current_schema()" : schemaname, tris_strlen_zero(schemaname) ? "" : "'");
		} else {
			snprintf(sqlcmd, sizeof(sqlcmd), "SELECT a.attname, t.typname, a.attlen, a.attnotnull, d.adsrc, a.atttypmod FROM pg_class c, pg_type t, pg_attribute a LEFT OUTER JOIN pg_attrdef d ON a.atthasdef AND d.adrelid = a.attrelid AND d.adnum = a.attnum WHERE c.oid = a.attrelid AND a.atttypid = t.oid AND (a.attnum > 0) AND c.relname = '%s' ORDER BY c.relname, attnum", table);
		}
		/* Query the columns */
		result = PQexec(conn, sqlcmd);
		if (PQresultStatus(result) != PGRES_TUPLES_OK) {
			pgerror = PQresultErrorMessage(result);
			tris_log(LOG_ERROR, "Failed to query database columns: %s\n", pgerror);
			PQclear(result);
			unload_module();
			return TRIS_MODULE_LOAD_DECLINE;
		}

		rows = PQntuples(result);
		for (i = 0; i < rows; i++) {
			fname = PQgetvalue(result, i, 0);
			ftype = PQgetvalue(result, i, 1);
			flen = PQgetvalue(result, i, 2);
			fnotnull = PQgetvalue(result, i, 3);
			fdef = PQgetvalue(result, i, 4);
			if (atoi(flen) == -1) {
				/* For varchar columns, the maximum length is encoded in a different field */
				flen = PQgetvalue(result, i, 5);
			}
			tris_verb(4, "Found column '%s' of type '%s'\n", fname, ftype);
			cur = tris_calloc(1, sizeof(*cur) + strlen(fname) + strlen(ftype) + 2);
			if (cur) {
				sscanf(flen, "%30d", &cur->len);
				cur->name = (char *)cur + sizeof(*cur);
				cur->type = (char *)cur + sizeof(*cur) + strlen(fname) + 1;
				strcpy(cur->name, fname);
				strcpy(cur->type, ftype);
				if (*fnotnull == 't') {
					cur->notnull = 1;
				} else {
					cur->notnull = 0;
				}
				if (!tris_strlen_zero(fdef)) {
					cur->hasdefault = 1;
				} else {
					cur->hasdefault = 0;
				}
				TRIS_RWLIST_INSERT_TAIL(&psql_columns, cur, list);
			}
		}
		PQclear(result);
	} else {
		pgerror = PQerrorMessage(conn);
		tris_log(LOG_ERROR, "Unable to connect to database server %s.  CALLS WILL NOT BE LOGGED!!\n", pghostname);
		tris_log(LOG_ERROR, "Reason: %s\n", pgerror);
		connected = 0;
	}

	tris_config_destroy(cfg);

	return tris_cdr_register(name, tris_module_info->description, pgsql_log);
}

static int load_module(void)
{
	return config_module(0) ? TRIS_MODULE_LOAD_DECLINE : 0;
}

static int reload(void)
{
	return config_module(1);
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "PostgreSQL CDR Backend",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
