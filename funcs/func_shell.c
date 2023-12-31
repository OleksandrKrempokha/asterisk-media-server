/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
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
 * SHELL function to return the value of a system call.
 * 
 * \note Inspiration and Guidance from Russell! Thank You! 
 *
 * \author Brandon Kruse <bkruse@digium.com>
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

static int shell_helper(struct tris_channel *chan, const char *cmd, char *data,
		                         char *buf, size_t len)
{
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Missing Argument!  Example:  Set(foo=${SHELL(echo \"bar\")})\n");
		return -1;
	}

	if (chan)
		tris_autoservice_start(chan);

	if (len >= 1) {
		FILE *ptr;
		char plbuff[4096];

		ptr = popen(data, "r");
		while (fgets(plbuff, sizeof(plbuff), ptr)) {
			strncat(buf, plbuff, len - strlen(buf) - 1);
		}
		pclose(ptr);
	}

	if (chan)
		tris_autoservice_stop(chan);

	return 0;
}

/*** DOCUMENTATION
	<function name="SHELL" language="en_US">
		<synopsis>
			Executes a command as if you were at a shell.
		</synopsis>
		<syntax>
			<parameter name="command" required="true">
				<para>This is the argument to the function, the command you want to pass to the shell.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns the value from a system command</para>
			<para>Example:  <literal>Set(foo=${SHELL(echo \bar\)})</literal></para>
			<note><para>When using the SHELL() dialplan function, your \SHELL\ is /bin/sh,
			which may differ as to the underlying shell, depending upon your production
			platform.  Also keep in mind that if you are using a common path, you should
			be mindful of race conditions that could result from two calls running
			SHELL() simultaneously.</para></note>
		</description>
 
	</function>
 ***/
static struct tris_custom_function shell_function = {
	.name = "SHELL",
	.read = shell_helper,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&shell_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&shell_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Returns the output of a shell command");

