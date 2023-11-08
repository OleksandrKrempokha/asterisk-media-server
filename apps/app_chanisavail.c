/*
* Trismedia -- An open source telephony toolkit.
*
* Copyright (C) 1999 - 2005, Digium, Inc.
*
* Mark Spencer <markster@digium.com>
* James Golovich <james@gnuinter.net>
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
 * \brief Check if Channel is Available
 *
 * \author Mark Spencer <markster@digium.com>
 * \author James Golovich <james@gnuinter.net>

 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 229969 $")

#include <sys/ioctl.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/devicestate.h"

static char *app = "ChanIsAvail";

/*** DOCUMENTATION
	<application name="ChanIsAvail" language="en_US">
		<synopsis>
			Check channel availability
		</synopsis>
		<syntax>
			<parameter name="Technology/Resource" required="true" argsep="&amp;">
				<argument name="Technology2/Resource2" multiple="true">
					<para>Optional extra devices to check</para>
					<para>If you need more then one enter them as
					Technology2/Resource2&amp;Technology3/Resourse3&amp;.....</para>
				</argument>
				<para>Specification of the device(s) to check.  These must be in the format of 
				<literal>Technology/Resource</literal>, where <replaceable>Technology</replaceable>
				represents a particular channel driver, and <replaceable>Resource</replaceable>
				represents a resource available to that particular channel driver.</para>
			</parameter>
			<parameter name="options" required="false">
				<optionlist>
					<option name="a">
						<para>Check for all available channels, not only the first one</para>
					</option>
					<option name="s">
						<para>Consider the channel unavailable if the channel is in use at all</para>
					</option>
					<option name="t" implies="s">
						<para>Simply checks if specified channels exist in the channel list</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>This application will check to see if any of the specified channels are available.</para>
			<para>This application sets the following channel variables:</para>
			<variablelist>
				<variable name="AVAILCHAN">
					<para>The name of the available channel, if one exists</para>
				</variable>
				<variable name="AVAILORIGCHAN">
					<para>The canonical channel name that was used to create the channel</para>
				</variable>
				<variable name="AVAILSTATUS">
					<para>The status code for the available channel. This is used for both
					device state and cause code. It is recommended that you use AVAILORIGCHAN
					instead to see if a device is available or not.</para>
				</variable>
			</variablelist>
		</description>
	</application>
 ***/

static int chanavail_exec(struct tris_channel *chan, void *data)
{
	int inuse=-1, option_state=0, string_compare=0, option_all_avail=0;
	int status;
	char *info, tmp[512], trychan[512], *peers, *tech, *number, *rest, *cur;
	struct tris_str *tmp_availchan = tris_str_alloca(2048);
	struct tris_str *tmp_availorig = tris_str_alloca(2048);
	struct tris_str *tmp_availstat = tris_str_alloca(2048);
	struct tris_channel *tempchan;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(reqchans);
		TRIS_APP_ARG(options);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "ChanIsAvail requires an argument (DAHDI/1&DAHDI/2)\n");
		return -1;
	}

	info = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, info);

	if (args.options) {
		if (strchr(args.options, 'a')) {
			option_all_avail = 1;
		}
		if (strchr(args.options, 's')) {
			option_state = 1;
		}
		if (strchr(args.options, 't')) {
			string_compare = 1;
		}
	}
	peers = args.reqchans;
	if (peers) {
		cur = peers;
		do {
			/* remember where to start next time */
			rest = strchr(cur, '&');
			if (rest) {
				*rest = 0;
				rest++;
			}
			tech = cur;
			number = strchr(tech, '/');
			if (!number) {
				tris_log(LOG_WARNING, "ChanIsAvail argument takes format ([technology]/[device])\n");
				return -1;
			}
			*number = '\0';
			number++;
			
			if (string_compare) {
				/* tris_parse_device_state checks for "SIP/1234" as a channel name.
				   tris_device_state will ask the SIP driver for the channel state. */

				snprintf(trychan, sizeof(trychan), "%s/%s",cur,number);
				status = inuse = tris_parse_device_state(trychan);
			} else if (option_state) {
				/* If the pbx says in use then don't bother trying further.
				   This is to permit testing if someone's on a call, even if the
				   channel can permit more calls (ie callwaiting, sip calls, etc).  */

				snprintf(trychan, sizeof(trychan), "%s/%s",cur,number);
				status = inuse = tris_device_state(trychan);
			}
			if ((inuse <= 1) && (tempchan = tris_request(tech, chan->nativeformats, number, &status, 0))) {
					tris_str_append(&tmp_availchan, 0, "%s%s", tris_str_strlen(tmp_availchan) ? "&" : "", tempchan->name);
					
					snprintf(tmp, sizeof(tmp), "%s/%s", tech, number);
					tris_str_append(&tmp_availorig, 0, "%s%s", tris_str_strlen(tmp_availorig) ? "&" : "", tmp);

					snprintf(tmp, sizeof(tmp), "%d", status);
					tris_str_append(&tmp_availstat, 0, "%s%s", tris_str_strlen(tmp_availstat) ? "&" : "", tmp);

					tris_hangup(tempchan);
					tempchan = NULL;

					if (!option_all_avail) {
						break;
					}
			} else {
				snprintf(tmp, sizeof(tmp), "%d", status);
				tris_str_append(&tmp_availstat, 0, "%s%s", tris_str_strlen(tmp_availstat) ? "&" : "", tmp);
			}
			cur = rest;
		} while (cur);
	}

	pbx_builtin_setvar_helper(chan, "AVAILCHAN", tris_str_buffer(tmp_availchan));
	/* Store the originally used channel too */
	pbx_builtin_setvar_helper(chan, "AVAILORIGCHAN", tris_str_buffer(tmp_availorig));
	pbx_builtin_setvar_helper(chan, "AVAILSTATUS", tris_str_buffer(tmp_availstat));

	return 0;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, chanavail_exec) ?
		TRIS_MODULE_LOAD_DECLINE : TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Check channel availability");
