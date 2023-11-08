/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Redfish Solutions
 *
 * Philip Prindeville <philipp@redfish-solutions.com>
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
 * \brief Sleep until the given epoch
 *
 * \author Philip Prindeville <philipp@redfish-solutions.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/logger.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"

/*** DOCUMENTATION
	<application name="WaitUntil" language="en_US">
		<synopsis>
			Wait (sleep) until the current time is the given epoch.
		</synopsis>
		<syntax>
			<parameter name="epoch" required="true" />
		</syntax>
		<description>
			<para>Waits until the given <replaceable>epoch</replaceable>.</para>
			<para>Sets <variable>WAITUNTILSTATUS</variable> to one of the following values:</para>
			<variablelist>
				<variable name="WAITUNTILSTATUS">
					<value name="OK">
						Wait succeeded.
					</value>
					<value name="FAILURE">
						Invalid argument.
					</value>
					<value name="HANGUP">
						Channel hungup before time elapsed.
					</value>
					<value name="PAST">
						Time specified had already past.
					</value>
				</variable>
			</variablelist>
		</description>
	</application>
 ***/

static char *app = "WaitUntil";

static int waituntil_exec(struct tris_channel *chan, void *data)
{
	int res;
	double fraction;
	long seconds;
	struct timeval future = { 0, };
	struct timeval now = tris_tvnow();
	int msec;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "WaitUntil requires an argument(epoch)\n");
		pbx_builtin_setvar_helper(chan, "WAITUNTILSTATUS", "FAILURE");
		return 0;
	}

	if (sscanf(data, "%30ld%30lf", &seconds, &fraction) == 0) {
		tris_log(LOG_WARNING, "WaitUntil called with non-numeric argument\n");
		pbx_builtin_setvar_helper(chan, "WAITUNTILSTATUS", "FAILURE");
		return 0;
	}

	future.tv_sec = seconds;
	future.tv_usec = fraction * 1000000;

	if ((msec = tris_tvdiff_ms(future, now)) < 0) {
		tris_log(LOG_NOTICE, "WaitUntil called in the past (now %ld, arg %ld)\n", (long)now.tv_sec, (long)future.tv_sec);
		pbx_builtin_setvar_helper(chan, "WAITUNTILSTATUS", "PAST");
		return 0;
	}

	if ((res = tris_safe_sleep(chan, msec)))
		pbx_builtin_setvar_helper(chan, "WAITUNTILSTATUS", "HANGUP");
	else
		pbx_builtin_setvar_helper(chan, "WAITUNTILSTATUS", "OK");

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, waituntil_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Wait until specified time");
