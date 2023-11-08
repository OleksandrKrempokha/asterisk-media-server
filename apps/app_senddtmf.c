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
 * \brief App to send DTMF digits
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 184082 $")

#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/manager.h"
#include "trismedia/channel.h"

/*** DOCUMENTATION
	<application name="SendDTMF" language="en_US">
		<synopsis>
			Sends arbitrary DTMF digits
		</synopsis>
		<syntax>
			<parameter name="digits" required="true">
				<para>List of digits 0-9,*#,abcd</para>
			</parameter>
			<parameter name="timeout_ms" required="false">
				<para>Amount of time to wait in ms between tones. (defaults to .25s)</para>
			</parameter>
			<parameter name="duration_ms" required="false">
				<para>Duration of each digit</para>
			</parameter>
		</syntax>
		<description>
			<para>DTMF digits sent to a channel with half second pause</para>
			<para>It will pass all digits or terminate if it encounters an error.</para>
		</description>
		<see-also>
			<ref type="application">Read</ref>
		</see-also>
	</application>
 ***/
static char *app = "SendDTMF";

static int senddtmf_exec(struct tris_channel *chan, void *vdata)
{
	int res = 0;
	char *data;
	int timeout = 0, duration = 0;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(digits);
		TRIS_APP_ARG(timeout);
		TRIS_APP_ARG(duration);
	);

	if (tris_strlen_zero(vdata)) {
		tris_log(LOG_WARNING, "SendDTMF requires an argument (digits or *#aAbBcCdD)\n");
		return 0;
	}

	data = tris_strdupa(vdata);
	TRIS_STANDARD_APP_ARGS(args, data);

	if (!tris_strlen_zero(args.timeout))
		timeout = atoi(args.timeout);
	if (!tris_strlen_zero(args.duration))
		duration = atoi(args.duration);
	res = tris_dtmf_stream(chan, NULL, args.digits, timeout <= 0 ? 250 : timeout, duration);

	return res;
}

static char mandescr_playdtmf[] =
"Description: Plays a dtmf digit on the specified channel.\n"
"Variables: (all are required)\n"
"	Channel: Channel name to send digit to\n"
"	Digit: The dtmf digit to play\n";

static int manager_play_dtmf(struct mansession *s, const struct message *m)
{
	const char *channel = astman_get_header(m, "Channel");
	const char *digit = astman_get_header(m, "Digit");
	struct tris_channel *chan = tris_get_channel_by_name_locked(channel);
	
	if (!chan) {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}
	if (tris_strlen_zero(digit)) {
		astman_send_error(s, m, "No digit specified");
		tris_channel_unlock(chan);
		return 0;
	}

	tris_senddigit(chan, *digit, 0);

	tris_channel_unlock(chan);
	astman_send_ack(s, m, "DTMF successfully queued");
	
	return 0;
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app);
	res |= tris_manager_unregister("PlayDTMF");

	return res;	
}

static int load_module(void)
{
	int res;

	res = tris_manager_register2( "PlayDTMF", EVENT_FLAG_CALL, manager_play_dtmf, "Play DTMF signal on a specific channel.", mandescr_playdtmf );
	res |= tris_register_application_xml(app, senddtmf_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Send DTMF digits Application");
