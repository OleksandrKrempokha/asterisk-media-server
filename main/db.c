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
 * \brief ASTdb Management
 *
 * \author Mark Spencer <markster@digium.com> 
 *
 * \note DB3 is licensed under Sleepycat Public License and is thus incompatible
 * with GPL.  To avoid having to make another exception (and complicate 
 * licensing even further) we elect to use DB1 which is BSD licensed 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 182453 $")

#include "trismedia/_private.h"
#include "trismedia/paths.h"	/* use tris_config_TRIS_DB */
#include <sys/time.h>
#include <signal.h>
#include <dirent.h>

#include "trismedia/channel.h"
#include "trismedia/file.h"
#include "trismedia/app.h"
#include "trismedia/dsp.h"
#include "trismedia/astdb.h"
#include "trismedia/cli.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/manager.h"
#include "db1-ast/include/db.h"

static DB *astdb;
TRIS_MUTEX_DEFINE_STATIC(dblock);

static int dbinit(void) 
{
	if (!astdb && !(astdb = dbopen(tris_config_TRIS_DB, O_CREAT | O_RDWR, TRIS_FILE_MODE, DB_BTREE, NULL))) {
		tris_log(LOG_WARNING, "Unable to open Trismedia database '%s': %s\n", tris_config_TRIS_DB, strerror(errno));
		return -1;
	}
	return 0;
}


static inline int keymatch(const char *key, const char *prefix)
{
	int preflen = strlen(prefix);
	if (!preflen)
		return 1;
	if (!strcasecmp(key, prefix))
		return 1;
	if ((strlen(key) > preflen) && !strncasecmp(key, prefix, preflen)) {
		if (key[preflen] == '/')
			return 1;
	}
	return 0;
}

static inline int subkeymatch(const char *key, const char *suffix)
{
	int suffixlen = strlen(suffix);
	if (suffixlen) {
		const char *subkey = key + strlen(key) - suffixlen;
		if (subkey < key)
			return 0;
		if (!strcasecmp(subkey, suffix))
			return 1;
	}
	return 0;
}

int tris_db_deltree(const char *family, const char *keytree)
{
	char prefix[256];
	DBT key, data;
	char *keys;
	int res;
	int pass;
	int counter = 0;
	
	if (family) {
		if (keytree) {
			snprintf(prefix, sizeof(prefix), "/%s/%s", family, keytree);
		} else {
			snprintf(prefix, sizeof(prefix), "/%s", family);
		}
	} else if (keytree) {
		return -1;
	} else {
		prefix[0] = '\0';
	}
	
	tris_mutex_lock(&dblock);
	if (dbinit()) {
		tris_mutex_unlock(&dblock);
		return -1;
	}
	
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	pass = 0;
	while (!(res = astdb->seq(astdb, &key, &data, pass++ ? R_NEXT : R_FIRST))) {
		if (key.size) {
			keys = key.data;
			keys[key.size - 1] = '\0';
		} else {
			keys = "<bad key>";
		}
		if (keymatch(keys, prefix)) {
			astdb->del(astdb, &key, 0);
			counter++;
		}
	}
	astdb->sync(astdb, 0);
	tris_mutex_unlock(&dblock);
	return counter;
}

int tris_db_put(const char *family, const char *keys, const char *value)
{
	char fullkey[256];
	DBT key, data;
	int res, fullkeylen;

	tris_mutex_lock(&dblock);
	if (dbinit()) {
		tris_mutex_unlock(&dblock);
		return -1;
	}

	fullkeylen = snprintf(fullkey, sizeof(fullkey), "/%s/%s", family, keys);
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	key.data = fullkey;
	key.size = fullkeylen + 1;
	data.data = (char *) value;
	data.size = strlen(value) + 1;
	res = astdb->put(astdb, &key, &data, 0);
	astdb->sync(astdb, 0);
	tris_mutex_unlock(&dblock);
	if (res)
		tris_log(LOG_WARNING, "Unable to put value '%s' for key '%s' in family '%s'\n", value, keys, family);
	return res;
}

int tris_db_get(const char *family, const char *keys, char *value, int valuelen)
{
	char fullkey[256] = "";
	DBT key, data;
	int res, fullkeylen;

	tris_mutex_lock(&dblock);
	if (dbinit()) {
		tris_mutex_unlock(&dblock);
		return -1;
	}

	fullkeylen = snprintf(fullkey, sizeof(fullkey), "/%s/%s", family, keys);
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	memset(value, 0, valuelen);
	key.data = fullkey;
	key.size = fullkeylen + 1;

	res = astdb->get(astdb, &key, &data, 0);

	/* Be sure to NULL terminate our data either way */
	if (res) {
		tris_debug(1, "Unable to find key '%s' in family '%s'\n", keys, family);
	} else {
#if 0
		printf("Got value of size %d\n", data.size);
#endif
		if (data.size) {
			((char *)data.data)[data.size - 1] = '\0';
			/* Make sure that we don't write too much to the dst pointer or we don't read too much from the source pointer */
			tris_copy_string(value, data.data, (valuelen > data.size) ? data.size : valuelen);
		} else {
			tris_log(LOG_NOTICE, "Strange, empty value for /%s/%s\n", family, keys);
		}
	}

	/* Data is not fully isolated for concurrency, so the lock must be extended
	 * to after the copy to the output buffer. */
	tris_mutex_unlock(&dblock);

	return res;
}

int tris_db_del(const char *family, const char *keys)
{
	char fullkey[256];
	DBT key;
	int res, fullkeylen;

	tris_mutex_lock(&dblock);
	if (dbinit()) {
		tris_mutex_unlock(&dblock);
		return -1;
	}
	
	fullkeylen = snprintf(fullkey, sizeof(fullkey), "/%s/%s", family, keys);
	memset(&key, 0, sizeof(key));
	key.data = fullkey;
	key.size = fullkeylen + 1;
	
	res = astdb->del(astdb, &key, 0);
	astdb->sync(astdb, 0);
	
	tris_mutex_unlock(&dblock);

	if (res) {
		tris_debug(1, "Unable to find key '%s' in family '%s'\n", keys, family);
	}
	return res;
}

static char *handle_cli_database_put(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "database put";
		e->usage =
			"Usage: database put <family> <key> <value>\n"
			"       Adds or updates an entry in the Trismedia database for\n"
			"       a given family, key, and value.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 5)
		return CLI_SHOWUSAGE;
	res = tris_db_put(a->argv[2], a->argv[3], a->argv[4]);
	if (res)  {
		tris_cli(a->fd, "Failed to update entry\n");
	} else {
		tris_cli(a->fd, "Updated database successfully\n");
	}
	return CLI_SUCCESS;
}

static char *handle_cli_database_get(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;
	char tmp[256];

	switch (cmd) {
	case CLI_INIT:
		e->command = "database get";
		e->usage =
			"Usage: database get <family> <key>\n"
			"       Retrieves an entry in the Trismedia database for a given\n"
			"       family and key.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	res = tris_db_get(a->argv[2], a->argv[3], tmp, sizeof(tmp));
	if (res) {
		tris_cli(a->fd, "Database entry not found.\n");
	} else {
		tris_cli(a->fd, "Value: %s\n", tmp);
	}
	return CLI_SUCCESS;
}

static char *handle_cli_database_del(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "database del";
		e->usage =
			"Usage: database del <family> <key>\n"
			"       Deletes an entry in the Trismedia database for a given\n"
			"       family and key.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	res = tris_db_del(a->argv[2], a->argv[3]);
	if (res) {
		tris_cli(a->fd, "Database entry does not exist.\n");
	} else {
		tris_cli(a->fd, "Database entry removed.\n");
	}
	return CLI_SUCCESS;
}

static char *handle_cli_database_deltree(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "database deltree";
		e->usage =
			"Usage: database deltree <family> [keytree]\n"
			"       Deletes a family or specific keytree within a family\n"
			"       in the Trismedia database.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if ((a->argc < 3) || (a->argc > 4))
		return CLI_SHOWUSAGE;
	if (a->argc == 4) {
		res = tris_db_deltree(a->argv[2], a->argv[3]);
	} else {
		res = tris_db_deltree(a->argv[2], NULL);
	}
	if (res < 0) {
		tris_cli(a->fd, "Database entries do not exist.\n");
	} else {
		tris_cli(a->fd, "%d database entries removed.\n",res);
	}
	return CLI_SUCCESS;
}

static char *handle_cli_database_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	char prefix[256];
	DBT key, data;
	char *keys, *values;
	int res;
	int pass;
	int counter = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "database show";
		e->usage =
			"Usage: database show [family [keytree]]\n"
			"       Shows Trismedia database contents, optionally restricted\n"
			"       to a given family, or family and keytree.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == 4) {
		/* Family and key tree */
		snprintf(prefix, sizeof(prefix), "/%s/%s", a->argv[2], a->argv[3]);
	} else if (a->argc == 3) {
		/* Family only */
		snprintf(prefix, sizeof(prefix), "/%s", a->argv[2]);
	} else if (a->argc == 2) {
		/* Neither */
		prefix[0] = '\0';
	} else {
		return CLI_SHOWUSAGE;
	}
	tris_mutex_lock(&dblock);
	if (dbinit()) {
		tris_mutex_unlock(&dblock);
		tris_cli(a->fd, "Database unavailable\n");
		return CLI_SUCCESS;	
	}
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	pass = 0;
	while (!(res = astdb->seq(astdb, &key, &data, pass++ ? R_NEXT : R_FIRST))) {
		if (key.size) {
			keys = key.data;
			keys[key.size - 1] = '\0';
		} else {
			keys = "<bad key>";
		}
		if (data.size) {
			values = data.data;
			values[data.size - 1]='\0';
		} else {
			values = "<bad value>";
		}
		if (keymatch(keys, prefix)) {
			tris_cli(a->fd, "%-50s: %-25s\n", keys, values);
			counter++;
		}
	}
	tris_mutex_unlock(&dblock);
	tris_cli(a->fd, "%d results found.\n", counter);
	return CLI_SUCCESS;	
}

static char *handle_cli_database_showkey(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	char suffix[256];
	DBT key, data;
	char *keys, *values;
	int res;
	int pass;
	int counter = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "database showkey";
		e->usage =
			"Usage: database showkey <keytree>\n"
			"       Shows Trismedia database contents, restricted to a given key.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == 3) {
		/* Key only */
		snprintf(suffix, sizeof(suffix), "/%s", a->argv[2]);
	} else {
		return CLI_SHOWUSAGE;
	}
	tris_mutex_lock(&dblock);
	if (dbinit()) {
		tris_mutex_unlock(&dblock);
		tris_cli(a->fd, "Database unavailable\n");
		return CLI_SUCCESS;	
	}
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	pass = 0;
	while (!(res = astdb->seq(astdb, &key, &data, pass++ ? R_NEXT : R_FIRST))) {
		if (key.size) {
			keys = key.data;
			keys[key.size - 1] = '\0';
		} else {
			keys = "<bad key>";
		}
		if (data.size) {
			values = data.data;
			values[data.size - 1]='\0';
		} else {
			values = "<bad value>";
		}
		if (subkeymatch(keys, suffix)) {
			tris_cli(a->fd, "%-50s: %-25s\n", keys, values);
			counter++;
		}
	}
	tris_mutex_unlock(&dblock);
	tris_cli(a->fd, "%d results found.\n", counter);
	return CLI_SUCCESS;	
}

struct tris_db_entry *tris_db_gettree(const char *family, const char *keytree)
{
	char prefix[256];
	DBT key, data;
	char *keys, *values;
	int values_len;
	int res;
	int pass;
	struct tris_db_entry *last = NULL;
	struct tris_db_entry *cur, *ret=NULL;

	if (!tris_strlen_zero(family)) {
		if (!tris_strlen_zero(keytree)) {
			/* Family and key tree */
			snprintf(prefix, sizeof(prefix), "/%s/%s", family, keytree);
		} else {
			/* Family only */
			snprintf(prefix, sizeof(prefix), "/%s", family);
		}
	} else {
		prefix[0] = '\0';
	}
	tris_mutex_lock(&dblock);
	if (dbinit()) {
		tris_mutex_unlock(&dblock);
		tris_log(LOG_WARNING, "Database unavailable\n");
		return NULL;	
	}
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	pass = 0;
	while (!(res = astdb->seq(astdb, &key, &data, pass++ ? R_NEXT : R_FIRST))) {
		if (key.size) {
			keys = key.data;
			keys[key.size - 1] = '\0';
		} else {
			keys = "<bad key>";
		}
		if (data.size) {
			values = data.data;
			values[data.size - 1] = '\0';
		} else {
			values = "<bad value>";
		}
		values_len = strlen(values) + 1;
		if (keymatch(keys, prefix) && (cur = tris_malloc(sizeof(*cur) + strlen(keys) + 1 + values_len))) {
			cur->next = NULL;
			cur->key = cur->data + values_len;
			strcpy(cur->data, values);
			strcpy(cur->key, keys);
			if (last) {
				last->next = cur;
			} else {
				ret = cur;
			}
			last = cur;
		}
	}
	tris_mutex_unlock(&dblock);
	return ret;	
}

void tris_db_freetree(struct tris_db_entry *dbe)
{
	struct tris_db_entry *last;
	while (dbe) {
		last = dbe;
		dbe = dbe->next;
		tris_free(last);
	}
}

struct tris_cli_entry cli_database[] = {
	TRIS_CLI_DEFINE(handle_cli_database_show,    "Shows database contents"),
	TRIS_CLI_DEFINE(handle_cli_database_showkey, "Shows database contents"),
	TRIS_CLI_DEFINE(handle_cli_database_get,     "Gets database value"),
	TRIS_CLI_DEFINE(handle_cli_database_put,     "Adds/updates database value"),
	TRIS_CLI_DEFINE(handle_cli_database_del,     "Removes database key/value"),
	TRIS_CLI_DEFINE(handle_cli_database_deltree, "Removes database keytree/values")
};

static int manager_dbput(struct mansession *s, const struct message *m)
{
	const char *family = astman_get_header(m, "Family");
	const char *key = astman_get_header(m, "Key");
	const char *val = astman_get_header(m, "Val");
	int res;

	if (tris_strlen_zero(family)) {
		astman_send_error(s, m, "No family specified");
		return 0;
	}
	if (tris_strlen_zero(key)) {
		astman_send_error(s, m, "No key specified");
		return 0;
	}

	res = tris_db_put(family, key, S_OR(val, ""));
	if (res) {
		astman_send_error(s, m, "Failed to update entry");
	} else {
		astman_send_ack(s, m, "Updated database successfully");
	}
	return 0;
}

static int manager_dbget(struct mansession *s, const struct message *m)
{
	const char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	const char *family = astman_get_header(m, "Family");
	const char *key = astman_get_header(m, "Key");
	char tmp[256];
	int res;

	if (tris_strlen_zero(family)) {
		astman_send_error(s, m, "No family specified.");
		return 0;
	}
	if (tris_strlen_zero(key)) {
		astman_send_error(s, m, "No key specified.");
		return 0;
	}

	if (!tris_strlen_zero(id))
		snprintf(idText, sizeof(idText) ,"ActionID: %s\r\n", id);

	res = tris_db_get(family, key, tmp, sizeof(tmp));
	if (res) {
		astman_send_error(s, m, "Database entry not found");
	} else {
		astman_send_ack(s, m, "Result will follow");
		astman_append(s, "Event: DBGetResponse\r\n"
				"Family: %s\r\n"
				"Key: %s\r\n"
				"Val: %s\r\n"
				"%s"
				"\r\n",
				family, key, tmp, idText);
	}
	return 0;
}

static int manager_dbdel(struct mansession *s, const struct message *m)
{
	const char *family = astman_get_header(m, "Family");
	const char *key = astman_get_header(m, "Key");
	int res;

	if (tris_strlen_zero(family)) {
		astman_send_error(s, m, "No family specified.");
		return 0;
	}

	if (tris_strlen_zero(key)) {
		astman_send_error(s, m, "No key specified.");
		return 0;
	}

	res = tris_db_del(family, key);
	if (res)
		astman_send_error(s, m, "Database entry not found");
	else
		astman_send_ack(s, m, "Key deleted successfully");

	return 0;
}

static int manager_dbdeltree(struct mansession *s, const struct message *m)
{
	const char *family = astman_get_header(m, "Family");
	const char *key = astman_get_header(m, "Key");
	int res;

	if (tris_strlen_zero(family)) {
		astman_send_error(s, m, "No family specified.");
		return 0;
	}

	if (!tris_strlen_zero(key))
		res = tris_db_deltree(family, key);
	else
		res = tris_db_deltree(family, NULL);

	if (res < 0)
		astman_send_error(s, m, "Database entry not found");
	else
		astman_send_ack(s, m, "Key tree deleted successfully");
	
	return 0;
}

int astdb_init(void)
{
	dbinit();
	tris_cli_register_multiple(cli_database, ARRAY_LEN(cli_database));
	tris_manager_register("DBGet", EVENT_FLAG_SYSTEM | EVENT_FLAG_REPORTING, manager_dbget, "Get DB Entry");
	tris_manager_register("DBPut", EVENT_FLAG_SYSTEM, manager_dbput, "Put DB Entry");
	tris_manager_register("DBDel", EVENT_FLAG_SYSTEM, manager_dbdel, "Delete DB Entry");
	tris_manager_register("DBDelTree", EVENT_FLAG_SYSTEM, manager_dbdeltree, "Delete DB Tree");
	return 0;
}
