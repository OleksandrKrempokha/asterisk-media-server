/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Author: Ben Miller <bgmiller@dccinc.com>
 *    With TONS of help from Mark!
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
 * \brief ParkAndAnnounce application for Trismedia
 *
 * \author Ben Miller <bgmiller@dccinc.com>
 * \arg With TONS of help from Mark!
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 170047 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/features.h"
#include "trismedia/say.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="ParkAndAnnounce" language="en_US">
		<synopsis>
			Park and Announce.
		</synopsis>
		<syntax>
			<parameter name="announce_template" required="true" argsep=":">
				<argument name="announce" required="true">
					<para>Colon-separated list of files to announce. The word
					<literal>PARKED</literal> will be replaced by a say_digits of the extension in which
					the call is parked.</para>
				</argument>
				<argument name="announce1" multiple="true" />
			</parameter>
			<parameter name="timeout" required="true">
				<para>Time in seconds before the call returns into the return
				context.</para>
			</parameter>
			<parameter name="dial" required="true">
				<para>The app_dial style resource to call to make the
				announcement. Console/dsp calls the console.</para>
			</parameter>
			<parameter name="return_context">
				<para>The goto-style label to jump the call back into after
				timeout. Default <literal>priority+1</literal>.</para>
			</parameter>
		</syntax>
		<description>
			<para>Park a call into the parkinglot and announce the call to another channel.</para>
			<para>The variable <variable>PARKEDAT</variable> will contain the parking extension
			into which the call was placed.  Use with the Local channel to allow the dialplan to make
			use of this information.</para>
		</description>
		<see-also>
			<ref type="application">Park</ref>
			<ref type="application">ParkedCall</ref>
		</see-also>
	</application>
 ***/

static char *app = "ParkAndAnnounce";

static int parkandannounce_exec(struct tris_channel *chan, void *data)
{
	int res = -1;
	int lot, timeout = 0, dres;
	char *dialtech, *tmp[100], buf[13];
	int looptemp, i;
	char *s;

	struct tris_channel *dchan;
	struct outgoing_helper oh = { 0, };
	int outstate;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(template);
		TRIS_APP_ARG(timeout);
		TRIS_APP_ARG(dial);
		TRIS_APP_ARG(return_context);
	);
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "ParkAndAnnounce requires arguments: (announce:template|timeout|dial|[return_context])\n");
		return -1;
	}
  
	s = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, s);

	if (args.timeout)
		timeout = atoi(args.timeout) * 1000;

	if (tris_strlen_zero(args.dial)) {
		tris_log(LOG_WARNING, "PARK: A dial resource must be specified i.e: Console/dsp or DAHDI/g1/5551212\n");
		return -1;
	}

	dialtech = strsep(&args.dial, "/");
	tris_verb(3, "Dial Tech,String: (%s,%s)\n", dialtech, args.dial);

	if (!tris_strlen_zero(args.return_context)) {
		tris_clear_flag(chan, TRIS_FLAG_IN_AUTOLOOP);
		tris_parseable_goto(chan, args.return_context);
	}

	tris_verb(3, "Return Context: (%s,%s,%d) ID: %s\n", chan->context, chan->exten, chan->priority, chan->cid.cid_num);
		if (!tris_exists_extension(chan, chan->context, chan->exten, chan->priority, chan->cid.cid_num)) {
		tris_verb(3, "Warning: Return Context Invalid, call will return to default|s\n");
		}

	/* we are using masq_park here to protect * from touching the channel once we park it.  If the channel comes out of timeout
	before we are done announcing and the channel is messed with, Kablooeee.  So we use Masq to prevent this.  */

	res = tris_masq_park_call(chan, NULL, timeout, &lot);
	if (res == -1)
		return res;

	tris_verb(3, "Call Parking Called, lot: %d, timeout: %d, context: %s\n", lot, timeout, args.return_context);

	/* Now place the call to the extension */

	snprintf(buf, sizeof(buf), "%d", lot);
	oh.parent_channel = chan;
	oh.vars = tris_variable_new("_PARKEDAT", buf, "");
	dchan = __tris_request_and_dial(dialtech, TRIS_FORMAT_SLINEAR, args.dial, 30000, &outstate, chan->cid.cid_num,
			chan->cid.cid_name, &oh);

	if (dchan) {
		if (dchan->_state == TRIS_STATE_UP) {
			tris_verb(4, "Channel %s was answered.\n", dchan->name);
		} else {
			tris_verb(4, "Channel %s was never answered.\n", dchan->name);
			tris_log(LOG_WARNING, "PARK: Channel %s was never answered for the announce.\n", dchan->name);
			tris_hangup(dchan);
			return -1;
		}
	} else {
		tris_log(LOG_WARNING, "PARK: Unable to allocate announce channel.\n");
		return -1; 
	}

	tris_stopstream(dchan);

	/* now we have the call placed and are ready to play stuff to it */

	tris_verb(4, "Announce Template:%s\n", args.template);

	for (looptemp = 0; looptemp < ARRAY_LEN(tmp); looptemp++) {
		if ((tmp[looptemp] = strsep(&args.template, ":")) != NULL)
			continue;
		else
			break;
	}

	for (i = 0; i < looptemp; i++) {
		tris_verb(4, "Announce:%s\n", tmp[i]);
		if (!strcmp(tmp[i], "PARKED")) {
			tris_say_digits(dchan, lot, "", dchan->language);
		} else {
			dres = tris_streamfile(dchan, tmp[i], dchan->language);
			if (!dres) {
				dres = tris_waitstream(dchan, "");
			} else {
				tris_log(LOG_WARNING, "tris_streamfile of %s failed on %s\n", tmp[i], dchan->name);
				dres = 0;
			}
		}
	}

	tris_stopstream(dchan);  
	tris_hangup(dchan);
	
	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	/* return tris_register_application(app, park_exec); */
	return tris_register_application_xml(app, parkandannounce_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Call Parking and Announce Application");
