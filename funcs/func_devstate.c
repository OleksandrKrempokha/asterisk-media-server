/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Digium, Inc.
 *
 * Russell Bryant <russell@digium.com> 
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
 * \brief Manually controlled blinky lights
 *
 * \author Russell Bryant <russell@digium.com> 
 *
 * \ingroup functions
 *
 * \todo Delete the entry from AstDB when set to nothing like Set(DEVICE_STATE(Custom:lamp1)=)
 *
 * \note Props go out to Ahrimanes in \#trismedia for requesting this at 4:30 AM
 *       when I couldn't sleep.  :)
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 193336 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/linkedlists.h"
#include "trismedia/devicestate.h"
#include "trismedia/cli.h"
#include "trismedia/astdb.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="DEVICE_STATE" language="en_US">
		<synopsis>
			Get or Set a device state.
		</synopsis>
		<syntax>
			<parameter name="device" required="true" />
		</syntax>
		<description>
			<para>The DEVICE_STATE function can be used to retrieve the device state from any
			device state provider. For example:</para>
			<para>NoOp(SIP/mypeer has state ${DEVICE_STATE(SIP/mypeer)})</para>
			<para>NoOp(Conference number 1234 has state ${DEVICE_STATE(MeetMe:1234)})</para>
			<para>The DEVICE_STATE function can also be used to set custom device state from
			the dialplan.  The <literal>Custom:</literal> prefix must be used. For example:</para>
			<para>Set(DEVICE_STATE(Custom:lamp1)=BUSY)</para>
			<para>Set(DEVICE_STATE(Custom:lamp2)=NOT_INUSE)</para>
			<para>You can subscribe to the status of a custom device state using a hint in
			the dialplan:</para>
			<para>exten => 1234,hint,Custom:lamp1</para>
			<para>The possible values for both uses of this function are:</para>
			<para>UNKNOWN | NOT_INUSE | INUSE | BUSY | INVALID | UNAVAILABLE | RINGING |
			RINGINUSE | ONHOLD</para>
		</description>
	</function>
	<function name="HINT" language="en_US">
		<synopsis>
			Get the devices set for a dialplan hint.
		</synopsis>
		<syntax>
			<parameter name="extension" required="true" argsep="@">
				<argument name="extension" required="true" />
				<argument name="context" />
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="n">
						<para>Retrieve name on the hint instead of list of devices.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>The HINT function can be used to retrieve the list of devices that are
			mapped to a dialplan hint. For example:</para>
			<para>NoOp(Hint for Extension 1234 is ${HINT(1234)})</para>
		</description>
	</function>
 ***/


static const char astdb_family[] = "CustomDevstate";

static int devstate_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	tris_copy_string(buf, tris_devstate_str(tris_device_state(data)), len);

	return 0;
}

static int devstate_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	size_t len = strlen("Custom:");
	enum tris_device_state state_val;

	if (strncasecmp(data, "Custom:", len)) {
		tris_log(LOG_WARNING, "The DEVICE_STATE function can only be used to set 'Custom:' device state!\n");
		return -1;
	}
	data += len;
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "DEVICE_STATE function called with no custom device name!\n");
		return -1;
	}

	state_val = tris_devstate_val(value);

	if (state_val == TRIS_DEVICE_UNKNOWN) {
		tris_log(LOG_ERROR, "DEVICE_STATE function given invalid state value '%s'\n", value);
		return -1;
	}

	tris_db_put(astdb_family, data, value);

	tris_devstate_changed(state_val, "Custom:%s", data);

	return 0;
}

enum {
	HINT_OPT_NAME = (1 << 0),
};

TRIS_APP_OPTIONS(hint_options, BEGIN_OPTIONS
	TRIS_APP_OPTION('n', HINT_OPT_NAME),
END_OPTIONS );

static int hint_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *exten, *context;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(exten);
		TRIS_APP_ARG(options);
	);
	struct tris_flags opts = { 0, };
	int res;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "The HINT function requires an extension\n");
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, data);

	if (tris_strlen_zero(args.exten)) {
		tris_log(LOG_WARNING, "The HINT function requires an extension\n");
		return -1;
	}

	context = exten = args.exten;
	strsep(&context, "@");
	if (tris_strlen_zero(context))
		context = "default";

	if (!tris_strlen_zero(args.options))
		tris_app_parse_options(hint_options, &opts, NULL, args.options);

	if (tris_test_flag(&opts, HINT_OPT_NAME))
		res = tris_get_hint(NULL, 0, buf, len, chan, context, exten);
	else
		res = tris_get_hint(buf, len, NULL, 0, chan, context, exten);

	return !res; /* tris_get_hint returns non-zero on success */
}

static enum tris_device_state custom_devstate_callback(const char *data)
{
	char buf[256] = "";

	tris_db_get(astdb_family, data, buf, sizeof(buf));

	return tris_devstate_val(buf);
}

static char *handle_cli_devstate_list(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_db_entry *db_entry, *db_tree;

	switch (cmd) {
	case CLI_INIT:
		e->command = "devstate list";
		e->usage =
			"Usage: devstate list\n"
			"       List all custom device states that have been set by using\n"
			"       the DEVICE_STATE dialplan function.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "\n"
	        "---------------------------------------------------------------------\n"
	        "--- Custom Device States --------------------------------------------\n"
	        "---------------------------------------------------------------------\n"
	        "---\n");

	db_entry = db_tree = tris_db_gettree(astdb_family, NULL);
	for (; db_entry; db_entry = db_entry->next) {
		const char *dev_name = strrchr(db_entry->key, '/') + 1;
		if (dev_name <= (const char *) 1)
			continue;
		tris_cli(a->fd, "--- Name: 'Custom:%s'  State: '%s'\n"
		               "---\n", dev_name, db_entry->data);
	}
	tris_db_freetree(db_tree);
	db_tree = NULL;

	tris_cli(a->fd,
	        "---------------------------------------------------------------------\n"
	        "---------------------------------------------------------------------\n"
	        "\n");

	return CLI_SUCCESS;
}

static char *handle_cli_devstate_change(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
    size_t len;
	const char *dev, *state;
	enum tris_device_state state_val;

	switch (cmd) {
	case CLI_INIT:
		e->command = "devstate change";
		e->usage =
			"Usage: devstate change <device> <state>\n"
			"       Change a custom device to a new state.\n"
			"       The possible values for the state are:\n"
			"UNKNOWN | NOT_INUSE | INUSE | BUSY | INVALID | UNAVAILABLE | RINGING\n"
			"RINGINUSE | ONHOLD\n",
			"\n"
			"Examples:\n"
			"       devstate change Custom:mystate1 INUSE\n"
			"       devstate change Custom:mystate1 NOT_INUSE\n"
			"       \n";
		return NULL;
	case CLI_GENERATE:
	{
		static char * const cmds[] = { "UNKNOWN", "NOT_INUSE", "INUSE", "BUSY",
			"UNAVAILABLE", "RINGING", "RINGINUSE", "ONHOLD", NULL };

		if (a->pos == e->args + 1)
			return tris_cli_complete(a->word, cmds, a->n);

		return NULL;
	}
	}

	if (a->argc != e->args + 2)
		return CLI_SHOWUSAGE;

	len = strlen("Custom:");
	dev = a->argv[e->args];
	state = a->argv[e->args + 1];

	if (strncasecmp(dev, "Custom:", len)) {
		tris_cli(a->fd, "The devstate command can only be used to set 'Custom:' device state!\n");
		return CLI_FAILURE;
	}

	dev += len;
	if (tris_strlen_zero(dev))
		return CLI_SHOWUSAGE;

	state_val = tris_devstate_val(state);

	if (state_val == TRIS_DEVICE_UNKNOWN)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "Changing %s to %s\n", dev, state);

	tris_db_put(astdb_family, dev, state);

	tris_devstate_changed(state_val, "Custom:%s", dev);

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_funcdevstate[] = {
	TRIS_CLI_DEFINE(handle_cli_devstate_list, "List currently known custom device states"),
	TRIS_CLI_DEFINE(handle_cli_devstate_change, "Change a custom device state"),
};

static struct tris_custom_function devstate_function = {
	.name = "DEVICE_STATE",
	.read = devstate_read,
	.write = devstate_write,
};

static struct tris_custom_function hint_function = {
	.name = "HINT",
	.read = hint_read,
};

static int unload_module(void)
{
	int res = 0;

	res |= tris_custom_function_unregister(&devstate_function);
	res |= tris_custom_function_unregister(&hint_function);
	res |= tris_devstate_prov_del("Custom");
	res |= tris_cli_unregister_multiple(cli_funcdevstate, ARRAY_LEN(cli_funcdevstate));

	return res;
}

static int load_module(void)
{
	int res = 0;
	struct tris_db_entry *db_entry, *db_tree;

	/* Populate the device state cache on the system with all of the currently
	 * known custom device states. */
	db_entry = db_tree = tris_db_gettree(astdb_family, NULL);
	for (; db_entry; db_entry = db_entry->next) {
		const char *dev_name = strrchr(db_entry->key, '/') + 1;
		if (dev_name <= (const char *) 1)
			continue;
		tris_devstate_changed(tris_devstate_val(db_entry->data),
			"Custom:%s\n", dev_name);
	}
	tris_db_freetree(db_tree);
	db_tree = NULL;

	res |= tris_custom_function_register(&devstate_function);
	res |= tris_custom_function_register(&hint_function);
	res |= tris_devstate_prov_add("Custom", custom_devstate_callback);
	res |= tris_cli_register_multiple(cli_funcdevstate, ARRAY_LEN(cli_funcdevstate));

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Gets or sets a device state in the dialplan");
