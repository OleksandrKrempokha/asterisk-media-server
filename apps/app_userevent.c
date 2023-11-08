/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief UserEvent application -- send manager event
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 169365 $")

#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/manager.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="UserEvent" language="en_US">
		<synopsis>
			Send an arbitrary event to the manager interface.
		</synopsis>
		<syntax>
			<parameter name="eventname" required="true" />
			<parameter name="body" />
		</syntax>
		<description>
			<para>Sends an arbitrary event to the manager interface, with an optional
			<replaceable>body</replaceable> representing additional arguments. The
			<replaceable>body</replaceable> may be specified as
			a <literal>|</literal> delimited list of headers. Each additional
			argument will be placed on a new line in the event. The format of the
			event will be:</para>
			<para>    Event: UserEvent</para>
			<para>    UserEvent: &lt;specified event name&gt;</para>
			<para>    [body]</para>
			<para>If no <replaceable>body</replaceable> is specified, only Event and UserEvent headers will be present.</para>
		</description>
	</application>
 ***/

static char *app = "UserEvent";

static int userevent_exec(struct tris_channel *chan, void *data)
{
	char *parse;
	int x;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(eventname);
		TRIS_APP_ARG(extra)[100];
	);
	struct tris_str *body = tris_str_create(16);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "UserEvent requires an argument (eventname,optional event body)\n");
		tris_free(body);
		return -1;
	}

	if (!body) {
		tris_log(LOG_WARNING, "Unable to allocate buffer\n");
		return -1;
	}

	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);

	for (x = 0; x < args.argc - 1; x++) {
		tris_str_append(&body, 0, "%s\r\n", args.extra[x]);
	}

	manager_event(EVENT_FLAG_USER, "UserEvent", "UserEvent: %s\r\n%s", args.eventname, tris_str_buffer(body));
	tris_free(body);

	return 0;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, userevent_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Custom User Event Application");
