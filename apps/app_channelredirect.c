/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Sergey Basmanov
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
 * \brief ChannelRedirect application
 *
 * \author Sergey Basmanov <sergey_basmanov@mail.ru>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 172063 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"
#include "trismedia/features.h"

/*** DOCUMENTATION
	<application name="ChannelRedirect" language="en_US">
		<synopsis>
			Redirects given channel to a dialplan target
		</synopsis>
		<syntax>
			<parameter name="channel" required="true" />
			<parameter name="context" required="false" />
			<parameter name="extension" required="false" />
			<parameter name="priority" required="true" />
		</syntax>
		<description>
			<para>Sends the specified channel to the specified extension priority</para>

			<para>This application sets the following channel variables upon completion</para>
			<variablelist>
				<variable name="CHANNELREDIRECT_STATUS">
					<value name="NOCHANNEL" />
					<value name="SUCCESS" />
					<para>Are set to the result of the redirection</para>
				</variable>
			</variablelist>
		</description>
	</application>
 ***/
static char *app = "ChannelRedirect";

static int asyncgoto_exec(struct tris_channel *chan, void *data)
{
	int res = -1;
	char *info;
	struct tris_channel *chan2 = NULL;

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(channel);
		TRIS_APP_ARG(label);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "%s requires an argument (channel,[[context,]exten,]priority)\n", app);
		return -1;
	}

	info = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, info);

	if (tris_strlen_zero(args.channel) || tris_strlen_zero(args.label)) {
		tris_log(LOG_WARNING, "%s requires an argument (channel,[[context,]exten,]priority)\n", app);
		return -1;
	}

	chan2 = tris_get_channel_by_name_locked(args.channel);
	if (!chan2) {
		tris_log(LOG_WARNING, "No such channel: %s\n", args.channel);
		pbx_builtin_setvar_helper(chan, "CHANNELREDIRECT_STATUS", "NOCHANNEL");
		return 0;
	}

	if (chan2->pbx) {
		tris_set_flag(chan2, TRIS_FLAG_BRIDGE_HANGUP_DONT); /* don't let the after-bridge code run the h-exten */
	}
	res = tris_async_parseable_goto(chan2, args.label);
	pbx_builtin_setvar_helper(chan, "CHANNELREDIRECT_STATUS", "SUCCESS");
	tris_channel_unlock(chan2);

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, asyncgoto_exec) ?
		TRIS_MODULE_LOAD_DECLINE : TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Redirects a given channel to a dialplan target");
