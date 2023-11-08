/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Tilghman Lesher
 *
 * Tilghman Lesher <func_global__200605@the-tilghman.com>
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
 * \brief Global variable dialplan functions
 *
 * \author Tilghman Lesher <func_global__200605@the-tilghman.com>
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include <sys/stat.h>

#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/channel.h"
#include "trismedia/app.h"
#include "trismedia/manager.h"

/*** DOCUMENTATION
	<function name="GLOBAL" language="en_US">
		<synopsis>
			Gets or sets the global variable specified.
		</synopsis>
		<syntax>
			<parameter name="varname" required="true">
				<para>Global variable name</para>
			</parameter>
		</syntax>
		<description>
			<para>Set or get the value of a global variable specified in <replaceable>varname</replaceable></para>
		</description>
	</function>
	<function name="SHARED" language="en_US">
		<synopsis>
			Gets or sets the shared variable specified.
		</synopsis>
		<syntax>
			<parameter name="varname" required="true">
				<para>Variable name</para>
			</parameter>
			<parameter name="channel">
				<para>If not specified will default to current channel. It is the complete
				channel name: <literal>SIP/12-abcd1234</literal> or the prefix only <literal>SIP/12</literal>.</para>
			</parameter>
		</syntax>
		<description>
			<para>Implements a shared variable area, in which you may share variables between
			channels.</para>
			<para>The variables used in this space are separate from the general namespace of
			the channel and thus <variable>SHARED(foo)</variable> and <variable>foo</variable> 
			represent two completely different variables, despite sharing the same name.</para>
			<para>Finally, realize that there is an inherent race between channels operating
			at the same time, fiddling with each others' internal variables, which is why
			this special variable namespace exists; it is to remind you that variables in
			the SHARED namespace may change at any time, without warning.  You should
			therefore take special care to ensure that when using the SHARED namespace,
			you retrieve the variable and store it in a regular channel variable before
			using it in a set of calculations (or you might be surprised by the result).</para>
		</description>
	</function>

 ***/

static void shared_variable_free(void *data);

static struct tris_datastore_info shared_variable_info = {
	.type = "SHARED_VARIABLES",
	.destroy = shared_variable_free,
};

static void shared_variable_free(void *data)
{
	struct varshead *varshead = data;
	struct tris_var_t *var;

	while ((var = TRIS_LIST_REMOVE_HEAD(varshead, entries))) {
		tris_var_delete(var);
	}
	tris_free(varshead);
}

static int global_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	const char *var = pbx_builtin_getvar_helper(NULL, data);

	*buf = '\0';

	if (var)
		tris_copy_string(buf, var, len);

	return 0;
}

static int global_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	pbx_builtin_setvar_helper(NULL, data, value);

	return 0;
}

static struct tris_custom_function global_function = {
	.name = "GLOBAL",
	.read = global_read,
	.write = global_write,
};

static int shared_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_datastore *varstore;
	struct varshead *varshead;
	struct tris_var_t *var;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(var);
		TRIS_APP_ARG(chan);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "SHARED() requires an argument: SHARED(<var>[,<chan>])\n");
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, data);

	if (!tris_strlen_zero(args.chan)) {
		char *prefix = alloca(strlen(args.chan) + 2);
		sprintf(prefix, "%s-", args.chan);
		if (!(chan = tris_get_channel_by_name_locked(args.chan)) && !(chan = tris_get_channel_by_name_prefix_locked(prefix, strlen(prefix)))) {
			tris_log(LOG_ERROR, "Channel '%s' not found!  Variable '%s' will be blank.\n", args.chan, args.var);
			return -1;
		}
	} else
		tris_channel_lock(chan);

	if (!(varstore = tris_channel_datastore_find(chan, &shared_variable_info, NULL))) {
		tris_channel_unlock(chan);
		return -1;
	}

	varshead = varstore->data;
	*buf = '\0';

	/* Protected by the channel lock */
	TRIS_LIST_TRAVERSE(varshead, var, entries) {
		if (!strcmp(args.var, tris_var_name(var))) {
			tris_copy_string(buf, tris_var_value(var), len);
			break;
		}
	}

	tris_channel_unlock(chan);

	return 0;
}

static int shared_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	struct tris_datastore *varstore;
	struct varshead *varshead;
	struct tris_var_t *var;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(var);
		TRIS_APP_ARG(chan);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "SHARED() requires an argument: SHARED(<var>[,<chan>])\n");
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, data);

	if (!tris_strlen_zero(args.chan)) {
		char *prefix = alloca(strlen(args.chan) + 2);
		sprintf(prefix, "%s-", args.chan);
		if (!(chan = tris_get_channel_by_name_locked(args.chan)) && !(chan = tris_get_channel_by_name_prefix_locked(prefix, strlen(prefix)))) {
			tris_log(LOG_ERROR, "Channel '%s' not found!  Variable '%s' not set to '%s'.\n", args.chan, args.var, value);
			return -1;
		}
	} else
		tris_channel_lock(chan);

	if (!(varstore = tris_channel_datastore_find(chan, &shared_variable_info, NULL))) {
		if (!(varstore = tris_datastore_alloc(&shared_variable_info, NULL))) {
			tris_log(LOG_ERROR, "Unable to allocate new datastore.  Shared variable not set.\n");
			tris_channel_unlock(chan);
			return -1;
		}

		if (!(varshead = tris_calloc(1, sizeof(*varshead)))) {
			tris_log(LOG_ERROR, "Unable to allocate variable structure.  Shared variable not set.\n");
			tris_datastore_free(varstore);
			tris_channel_unlock(chan);
			return -1;
		}

		varstore->data = varshead;
		tris_channel_datastore_add(chan, varstore);
	}
	varshead = varstore->data;

	/* Protected by the channel lock */
	TRIS_LIST_TRAVERSE(varshead, var, entries) {
		/* If there's a previous value, remove it */
		if (!strcmp(args.var, tris_var_name(var))) {
			TRIS_LIST_REMOVE(varshead, var, entries);
			tris_var_delete(var);
			break;
		}
	}

	var = tris_var_assign(args.var, S_OR(value, ""));
	TRIS_LIST_INSERT_HEAD(varshead, var, entries);
	manager_event(EVENT_FLAG_DIALPLAN, "VarSet", 
		"Channel: %s\r\n"
		"Variable: SHARED(%s)\r\n"
		"Value: %s\r\n"
		"Uniqueid: %s\r\n", 
		chan ? chan->name : "none", args.var, value, 
		chan ? chan->uniqueid : "none");

	tris_channel_unlock(chan);

	return 0;
}

static struct tris_custom_function shared_function = {
	.name = "SHARED",
	.read = shared_read,
	.write = shared_write,
};

static int unload_module(void)
{
	int res = 0;

	res |= tris_custom_function_unregister(&global_function);
	res |= tris_custom_function_unregister(&shared_function);

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= tris_custom_function_register(&global_function);
	res |= tris_custom_function_register(&shared_function);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Variable dialplan functions");
