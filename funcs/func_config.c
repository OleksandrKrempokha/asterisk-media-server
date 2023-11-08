/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Digium, Inc.
 *
 * Russell Bryant <russell@digium.com>
 * Tilghman Lesher <func_config__200803@the-tilghman.com>
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
 * \brief A function to retrieve variables from an Trismedia configuration file
 *
 * \author Russell Bryant <russell@digium.com>
 * \author Tilghman Lesher <func_config__200803@the-tilghman.com>
 * 
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="TRIS_CONFIG" language="en_US">
		<synopsis>
			Retrieve a variable from a configuration file.
		</synopsis>
		<syntax>
			<parameter name="config_file" required="true" />
			<parameter name="category" required="true" />
			<parameter name="variable_name" required="true" />
		</syntax>
		<description>
			<para>This function reads a variable from an Trismedia configuration file.</para>
		</description>
	</function>

***/

struct config_item {
	TRIS_RWLIST_ENTRY(config_item) entry;
	struct tris_config *cfg;
	char filename[0];
};

static TRIS_RWLIST_HEAD_STATIC(configs, config_item);

static int config_function_read(struct tris_channel *chan, const char *cmd, char *data, 
	char *buf, size_t len) 
{
	struct tris_config *cfg;
	struct tris_flags cfg_flags = { CONFIG_FLAG_FILEUNCHANGED };
	const char *val;
	char *parse;
	struct config_item *cur;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(filename);
		TRIS_APP_ARG(category);
		TRIS_APP_ARG(variable);
		TRIS_APP_ARG(index);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_ERROR, "TRIS_CONFIG() requires an argument\n");
		return -1;
	}

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (tris_strlen_zero(args.filename)) {
		tris_log(LOG_ERROR, "TRIS_CONFIG() requires a filename\n");
		return -1;
	}

	if (tris_strlen_zero(args.category)) {
		tris_log(LOG_ERROR, "TRIS_CONFIG() requires a category\n");
		return -1;
	}
	
	if (tris_strlen_zero(args.variable)) {
		tris_log(LOG_ERROR, "TRIS_CONFIG() requires a variable\n");
		return -1;
	}

	if (!(cfg = tris_config_load(args.filename, cfg_flags)) || cfg == CONFIG_STATUS_FILEINVALID) {
		return -1;
	}

	if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		/* Retrieve cfg from list */
		TRIS_RWLIST_RDLOCK(&configs);
		TRIS_RWLIST_TRAVERSE(&configs, cur, entry) {
			if (!strcmp(cur->filename, args.filename)) {
				break;
			}
		}

		if (!cur) {
			/* At worst, we might leak an entry while upgrading locks */
			TRIS_RWLIST_UNLOCK(&configs);
			TRIS_RWLIST_WRLOCK(&configs);
			if (!(cur = tris_malloc(sizeof(*cur) + strlen(args.filename) + 1))) {
				TRIS_RWLIST_UNLOCK(&configs);
				return -1;
			}

			strcpy(cur->filename, args.filename);

			tris_clear_flag(&cfg_flags, CONFIG_FLAG_FILEUNCHANGED);
			if (!(cfg = tris_config_load(args.filename, cfg_flags)) || cfg == CONFIG_STATUS_FILEINVALID) {
				tris_free(cur);
				TRIS_RWLIST_UNLOCK(&configs);
				return -1;
			}

			cur->cfg = cfg;
			TRIS_RWLIST_INSERT_TAIL(&configs, cur, entry);
		}

		cfg = cur->cfg;
	} else {
		/* Replace cfg in list */
		TRIS_RWLIST_WRLOCK(&configs);
		TRIS_RWLIST_TRAVERSE(&configs, cur, entry) {
			if (!strcmp(cur->filename, args.filename)) {
				break;
			}
		}

		if (!cur) {
			if (!(cur = tris_malloc(sizeof(*cur) + strlen(args.filename) + 1))) {
				TRIS_RWLIST_UNLOCK(&configs);
				return -1;
			}

			strcpy(cur->filename, args.filename);
			cur->cfg = cfg;

			TRIS_RWLIST_INSERT_TAIL(&configs, cur, entry);
		} else {
			tris_config_destroy(cur->cfg);
			cur->cfg = cfg;
		}
	}

	if (!(val = tris_variable_retrieve(cfg, args.category, args.variable))) {
		tris_log(LOG_ERROR, "'%s' not found in [%s] of '%s'\n", args.variable, 
			args.category, args.filename);
		TRIS_RWLIST_UNLOCK(&configs);
		return -1;
	}

	tris_copy_string(buf, val, len);

	/* Unlock down here, so there's no chance the struct goes away while we're using it. */
	TRIS_RWLIST_UNLOCK(&configs);

	return 0;
}

static struct tris_custom_function config_function = {
	.name = "TRIS_CONFIG",
	.read = config_function_read,
};

static int unload_module(void)
{
	struct config_item *current;
	int res = tris_custom_function_unregister(&config_function);

	TRIS_RWLIST_WRLOCK(&configs);
	while ((current = TRIS_RWLIST_REMOVE_HEAD(&configs, entry))) {
		tris_config_destroy(current->cfg);
		tris_free(current);
	}
	TRIS_RWLIST_UNLOCK(&configs);

	return res;
}

static int load_module(void)
{
	return tris_custom_function_register(&config_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Trismedia configuration file variable access");
