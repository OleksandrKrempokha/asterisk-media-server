/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005 - 2006, Digium, Inc.
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

/*! 
 * \file
 * \author Russell Bryant <russell@digium.com>
 *
 * \brief Originate calls via the CLI
 * 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 163828 $");

#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/cli.h"
#include "trismedia/utils.h"
#include "trismedia/frame.h"

/*! The timeout for originated calls, in seconds */
#define TIMEOUT 30

/*!
 * \brief orginate a call from the CLI
 * \param fd file descriptor for cli
 * \param chan channel to create type/data
 * \param app application you want to run
 * \param appdata data for application
 * \retval CLI_SUCCESS on success.
 * \retval CLI_SHOWUSAGE on failure.
*/
static char *orig_app(int fd, const char *chan, const char *app, const char *appdata)
{
	char *chantech;
	char *chandata;
	int reason = 0;
	
	if (tris_strlen_zero(app))
		return CLI_SHOWUSAGE;

	chandata = tris_strdupa(chan);
	
	chantech = strsep(&chandata, "/");
	if (!chandata) {
		tris_cli(fd, "*** No data provided after channel type! ***\n");
		return CLI_SHOWUSAGE;
	}

	tris_pbx_outgoing_app(chantech, TRIS_FORMAT_SLINEAR, chandata, TIMEOUT * 1000, app, appdata, &reason, 0, NULL, NULL, NULL, NULL, NULL);

	return CLI_SUCCESS;
}

/*!
 * \brief orginate from extension
 * \param fd file descriptor for cli
 * \param chan channel to create type/data
 * \param data contains exten\@context
 * \retval CLI_SUCCESS on success.
 * \retval CLI_SHOWUSAGE on failure.
*/
static char *orig_exten(int fd, const char *chan, const char *data)
{
	char *chantech;
	char *chandata;
	char *exten = NULL;
	char *context = NULL;
	int reason = 0;

	chandata = tris_strdupa(chan);
	
	chantech = strsep(&chandata, "/");
	if (!chandata) {
		tris_cli(fd, "*** No data provided after channel type! ***\n");
		return CLI_SHOWUSAGE;
	}

	if (!tris_strlen_zero(data)) {
		context = tris_strdupa(data);
		exten = strsep(&context, "@");
	}

	if (tris_strlen_zero(exten))
		exten = "s";
	if (tris_strlen_zero(context))
		context = "default";
	
	tris_pbx_outgoing_exten(chantech, TRIS_FORMAT_SLINEAR, chandata, TIMEOUT * 1000, context, exten, 1, &reason, 0, NULL, NULL, NULL, NULL, NULL);

	return CLI_SUCCESS;
}

/*!
 * \brief handle for orgination app or exten.
 * \param e pointer to the CLI structure to initialize
 * \param cmd operation to execute
 * \param a structure that contains either application or extension arguments
 * \retval CLI_SUCCESS on success.
 * \retval CLI_SHOWUSAGE on failure.
*/
static char *handle_orig(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	static char *choices[] = { "application", "extension", NULL };
	char *res;
	switch (cmd) {
	case CLI_INIT:
		e->command = "channel originate";
		e->usage = 
			"  There are two ways to use this command. A call can be originated between a\n"
			"channel and a specific application, or between a channel and an extension in\n"
			"the dialplan. This is similar to call files or the manager originate action.\n"
			"Calls originated with this command are given a timeout of 30 seconds.\n\n"

			"Usage1: channel originate <tech/data> application <appname> [appdata]\n"
			"  This will originate a call between the specified channel tech/data and the\n"
			"given application. Arguments to the application are optional. If the given\n"
			"arguments to the application include spaces, all of the arguments to the\n"
			"application need to be placed in quotation marks.\n\n"

			"Usage2: channel originate <tech/data> extension [exten@][context]\n"
			"  This will originate a call between the specified channel tech/data and the\n"
			"given extension. If no context is specified, the 'default' context will be\n"
			"used. If no extension is given, the 's' extension will be used.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos != 3)
			return NULL;

		/* ugly, can be removed when CLI entries have tris_module pointers */
		tris_module_ref(tris_module_info->self);
		res = tris_cli_complete(a->word, choices, a->n);
		tris_module_unref(tris_module_info->self);

		return res;
	}

	if (tris_strlen_zero(a->argv[2]) || tris_strlen_zero(a->argv[3]))
		return CLI_SHOWUSAGE;

	/* ugly, can be removed when CLI entries have tris_module pointers */
	tris_module_ref(tris_module_info->self);

	if (!strcasecmp("application", a->argv[3])) {
		res = orig_app(a->fd, a->argv[2], a->argv[4], a->argv[5]);	
	} else if (!strcasecmp("extension", a->argv[3])) {
		res = orig_exten(a->fd, a->argv[2], a->argv[4]);
	} else {
		tris_log(LOG_WARNING, "else");
		res = CLI_SHOWUSAGE;
	}

	tris_module_unref(tris_module_info->self);

	return res;
}

static char *handle_redirect(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	const char *name, *dest;
	struct tris_channel *chan;
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "channel redirect";
		e->usage = ""
		"Usage: channel redirect <channel> <[[context,]exten,]priority>\n"
		"    Redirect an active channel to a specified extension.\n";
		/*! \todo It would be nice to be able to redirect 2 channels at the same
		 *  time like you can with AMI redirect.  However, it is not possible to acquire
		 *  two channels without the potential for a deadlock with how tris_channel structs
		 *  are managed today.  Once tris_channel is a refcounted object, this command
		 *  will be able to support that. */
		return NULL;
	case CLI_GENERATE:
		return tris_complete_channels(a->line, a->word, a->pos, a->n, 2);
	}

	if (a->argc != e->args + 2) {
		return CLI_SHOWUSAGE;
	}

	name = a->argv[2];
	dest = a->argv[3];

	chan = tris_get_channel_by_name_locked(name);
	if (!chan) {
		tris_cli(a->fd, "Channel '%s' not found\n", name);
		return CLI_FAILURE;
	}

	res = tris_async_parseable_goto(chan, dest);

	tris_channel_unlock(chan);

	if (!res) {
		tris_cli(a->fd, "Channel '%s' successfully redirected to %s\n", name, dest);
	} else {
		tris_cli(a->fd, "Channel '%s' failed to be redirected to %s\n", name, dest);
	}

	return res ? CLI_FAILURE : CLI_SUCCESS;
}

static struct tris_cli_entry cli_cliorig[] = {
	TRIS_CLI_DEFINE(handle_orig, "Originate a call"),
	TRIS_CLI_DEFINE(handle_redirect, "Redirect a call"),
};

static int unload_module(void)
{
	return tris_cli_unregister_multiple(cli_cliorig, ARRAY_LEN(cli_cliorig));
}

static int load_module(void)
{
	int res;
	res = tris_cli_register_multiple(cli_cliorig, ARRAY_LEN(cli_cliorig));
	return res ? TRIS_MODULE_LOAD_DECLINE : TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Call origination and redirection from the CLI");
