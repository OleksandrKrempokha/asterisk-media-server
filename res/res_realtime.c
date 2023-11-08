/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Anthony Minessale <anthmct@yahoo.com>
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
 * \brief RealTime CLI
 *
 * \author Anthony Minessale <anthmct@yahoo.com>
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 208053 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/cli.h"


static char *cli_realtime_load(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
#define CRL_HEADER_FORMAT "%30s  %-30s\n"
	struct tris_variable *var = NULL, *orig_var = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime load";
		e->usage =
			"Usage: realtime load <family> <colmatch> <value>\n"
			"       Prints out a list of variables using the RealTime driver.\n"
			"       You must supply a family name, a column to match on, and a value to match to.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}


	if (a->argc < 5) 
		return CLI_SHOWUSAGE;

	var = tris_load_realtime_all(a->argv[2], a->argv[3], a->argv[4], SENTINEL);

	if (var) {
		tris_cli(a->fd, CRL_HEADER_FORMAT, "Column Name", "Column Value");
		tris_cli(a->fd, CRL_HEADER_FORMAT, "--------------------", "--------------------");
		orig_var = var;
		while (var) {
			tris_cli(a->fd, CRL_HEADER_FORMAT, var->name, var->value);
			var = var->next;
		}
	} else {
		tris_cli(a->fd, "No rows found matching search criteria.\n");
	}
	tris_variables_destroy(orig_var);
	return CLI_SUCCESS;
}

static char *cli_realtime_update(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime update";
		e->usage =
			"Usage: realtime update <family> <colmatch> <valuematch> <colupdate> <newvalue>\n"
			"       Update a single variable using the RealTime driver.\n"
			"       You must supply a family name, a column to update on, a new value, column to match, and value to match.\n"
			"       Ex: realtime update sipfriends name bobsphone port 4343\n"
			"       will execute SQL as UPDATE sipfriends SET port = 4343 WHERE name = bobsphone\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 7) 
		return CLI_SHOWUSAGE;

	res = tris_update_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], SENTINEL);

	if (res < 0) {
		tris_cli(a->fd, "Failed to update. Check the debug log for possible SQL related entries.\n");
		return CLI_FAILURE;
	}

	tris_cli(a->fd, "Updated %d RealTime record%s.\n", res, ESS(res));

	return CLI_SUCCESS;
}

static char *cli_realtime_update2(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res = -1;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime update2";
		e->usage =
			"Usage: realtime update2 <family> <colmatch> <valuematch> [... <colmatch5> <valuematch5>] NULL <colupdate> <newvalue>\n"
			"   Update a single variable, requiring one or more fields to match using the\n"
			"   RealTime driver.  You must supply a family name, a column to update, a new\n"
			"   value, and at least one column and value to match.\n"
			"   Ex: realtime update sipfriends name bobsphone ipaddr 127.0.0.1 NULL port 4343\n"
			"   will execute SQL as\n"
			"   UPDATE sipfriends SET port='4343' WHERE name='bobsphone' and ipaddr='127.0.0.1'\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 7) 
		return CLI_SHOWUSAGE;

	if (a->argc == 7) {
		res = tris_update2_realtime(a->argv[2], a->argv[3], a->argv[4], SENTINEL, a->argv[5], a->argv[6], SENTINEL);
	} else if (a->argc == 9) {
		res = tris_update2_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], SENTINEL, a->argv[7], a->argv[8], SENTINEL);
	} else if (a->argc == 11) {
		res = tris_update2_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], SENTINEL, a->argv[9], a->argv[10], SENTINEL);
	} else if (a->argc == 13) {
		res = tris_update2_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], a->argv[9], a->argv[10], SENTINEL, a->argv[11], a->argv[12], SENTINEL);
	} else if (a->argc == 15) {
		res = tris_update2_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], a->argv[9], a->argv[10], a->argv[11], a->argv[12], SENTINEL, a->argv[13], a->argv[14], SENTINEL);
	} else {
		return CLI_SHOWUSAGE;
	}

	if (res < 0) {
		tris_cli(a->fd, "Failed to update. Check the debug log for possible SQL related entries.\n");
		return CLI_FAILURE;
	}

	tris_cli(a->fd, "Updated %d RealTime record%s.\n", res, ESS(res));

	return CLI_SUCCESS;
}

static char *cli_realtime_store(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res = -1;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime store";
		e->usage =
			"Usage: realtime store <family> <colname1> <value1> [<colname2> <value2> [... <colname5> <value5>]]\n"
			"       Create a stored row using the RealTime driver.\n"
			"       You must supply a family name and name/value pairs (up to 5).  If\n"
			"       you need to store more than 5 key/value pairs, start with the first\n"
			"       five, then use 'realtime update' or 'realtime update2' to add\n"
			"       additional columns.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 5) {
		return CLI_SHOWUSAGE;
	} else if (a->argc == 5) {
		res = tris_store_realtime(a->argv[2], a->argv[3], a->argv[4], SENTINEL);
	} else if (a->argc == 7) {
		res = tris_store_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], SENTINEL);
	} else if (a->argc == 9) {
		res = tris_store_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], SENTINEL);
	} else if (a->argc == 11) {
		res = tris_store_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], a->argv[9], a->argv[10], SENTINEL);
	} else if (a->argc == 13) {
		res = tris_store_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], a->argv[9], a->argv[10], a->argv[11], a->argv[12], SENTINEL);
	} else {
		return CLI_SHOWUSAGE;
	}

	if (res < 0) {
		tris_cli(a->fd, "Failed to store record. Check the debug log for possible SQL related entries.\n");
		return CLI_FAILURE;
	}

	tris_cli(a->fd, "Stored RealTime record.\n");

	return CLI_SUCCESS;
}

static char *cli_realtime_destroy(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res = -1;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime destroy";
		e->usage =
			"Usage: realtime destroy <family> <colmatch1> <valuematch1> [<colmatch2> <valuematch2> [... <colmatch5> <valuematch5>]]\n"
			"       Remove a stored row using the RealTime driver.\n"
			"       You must supply a family name and name/value pairs (up to 5).\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 5) {
		return CLI_SHOWUSAGE;
	} else if (a->argc == 5) {
		res = tris_destroy_realtime(a->argv[2], a->argv[3], a->argv[4], SENTINEL);
	} else if (a->argc == 7) {
		res = tris_destroy_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], SENTINEL);
	} else if (a->argc == 9) {
		res = tris_destroy_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], SENTINEL);
	} else if (a->argc == 11) {
		res = tris_destroy_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], a->argv[9], a->argv[10], SENTINEL);
	} else if (a->argc == 13) {
		res = tris_destroy_realtime(a->argv[2], a->argv[3], a->argv[4], a->argv[5], a->argv[6], a->argv[7], a->argv[8], a->argv[9], a->argv[10], a->argv[11], a->argv[12], SENTINEL);
	} else {
		return CLI_SHOWUSAGE;
	}

	if (res < 0) {
		tris_cli(a->fd, "Failed to remove record. Check the debug log for possible SQL related entries.\n");
		return CLI_FAILURE;
	}

	tris_cli(a->fd, "Removed %d RealTime record%s.\n", res, ESS(res));

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_realtime[] = {
	TRIS_CLI_DEFINE(cli_realtime_load, "Used to print out RealTime variables."),
	TRIS_CLI_DEFINE(cli_realtime_update, "Used to update RealTime variables."),
	TRIS_CLI_DEFINE(cli_realtime_update2, "Used to test the RealTime update2 method"),
	TRIS_CLI_DEFINE(cli_realtime_store, "Store a new row into a RealTime database"),
	TRIS_CLI_DEFINE(cli_realtime_destroy, "Delete a row from a RealTime database"),
};

static int unload_module(void)
{
	tris_cli_unregister_multiple(cli_realtime, ARRAY_LEN(cli_realtime));
	return 0;
}

static int load_module(void)
{
	tris_cli_register_multiple(cli_realtime, ARRAY_LEN(cli_realtime));
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Realtime Data Lookup/Rewrite");
