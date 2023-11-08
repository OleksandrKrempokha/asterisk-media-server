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
 * \brief Execute arbitrary system commands
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 177664 $")

#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/channel.h"	/* autoservice */
#include "trismedia/strings.h"
#include "trismedia/threadstorage.h"

/*** DOCUMENTATION
	<application name="System" language="en_US">
		<synopsis>
			Execute a system command.
		</synopsis>
		<syntax>
			<parameter name="command" required="true">
				<para>Command to execute</para>
			</parameter>
		</syntax>
		<description>
			<para>Executes a command  by  using  system(). If the command
			fails, the console should report a fallthrough.</para>
			<para>Result of execution is returned in the <variable>SYSTEMSTATUS</variable> channel variable:</para>
			<variablelist>
				<variable name="SYSTEMSTATUS">
					<value name="FAILURE">
						Could not execute the specified command.
					</value>
					<value name="SUCCESS">
						Specified command successfully executed.
					</value>
				</variable>
			</variablelist>
		</description>
	</application>
	<application name="TrySystem" language="en_US">
		<synopsis>
			Try executing a system command.
		</synopsis>
		<syntax>
			<parameter name="command" required="true">
				<para>Command to execute</para>
			</parameter>
		</syntax>
		<description>
			<para>Executes a command  by  using  system().</para>
			<para>Result of execution is returned in the <variable>SYSTEMSTATUS</variable> channel variable:</para>
			<variablelist>
				<variable name="SYSTEMSTATUS">
					<value name="FAILURE">
						Could not execute the specified command.
					</value>
					<value name="SUCCESS">
						Specified command successfully executed.
					</value>
					<value name="APPERROR">
						Specified command successfully executed, but returned error code.
					</value>
				</variable>
			</variablelist>
		</description>
	</application>

 ***/

TRIS_THREADSTORAGE(buf_buf);

static char *app = "System";

static char *app2 = "TrySystem";

static char *chanvar = "SYSTEMSTATUS";

static int system_exec_helper(struct tris_channel *chan, void *data, int failmode)
{
	int res = 0;
	struct tris_str *buf = tris_str_thread_get(&buf_buf, 16);
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "System requires an argument(command)\n");
		pbx_builtin_setvar_helper(chan, chanvar, "FAILURE");
		return failmode;
	}

	tris_autoservice_start(chan);

	/* Do our thing here */
	tris_str_get_encoded_str(&buf, 0, (char *) data);
	res = tris_safe_system(tris_str_buffer(buf));

	if ((res < 0) && (errno != ECHILD)) {
		tris_log(LOG_WARNING, "Unable to execute '%s'\n", (char *)data);
		pbx_builtin_setvar_helper(chan, chanvar, "FAILURE");
		res = failmode;
	} else if (res == 127) {
		tris_log(LOG_WARNING, "Unable to execute '%s'\n", (char *)data);
		pbx_builtin_setvar_helper(chan, chanvar, "FAILURE");
		res = failmode;
	} else {
		if (res < 0) 
			res = 0;
		if (res != 0)
			pbx_builtin_setvar_helper(chan, chanvar, "APPERROR");
		else
			pbx_builtin_setvar_helper(chan, chanvar, "SUCCESS");
		res = 0;
	} 

	tris_autoservice_stop(chan);

	return res;
}

static int system_exec(struct tris_channel *chan, void *data)
{
	return system_exec_helper(chan, data, -1);
}

static int trysystem_exec(struct tris_channel *chan, void *data)
{
	return system_exec_helper(chan, data, 0);
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app);
	res |= tris_unregister_application(app2);

	return res;
}

static int load_module(void)
{
	int res;

	res = tris_register_application_xml(app2, trysystem_exec);
	res |= tris_register_application_xml(app, system_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Generic System() application");
