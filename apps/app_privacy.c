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
 * \brief Block all calls without Caller*ID, require phone # to be entered
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/utils.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/image.h"
#include "trismedia/callerid.h"
#include "trismedia/app.h"
#include "trismedia/config.h"

/*** DOCUMENTATION
	<application name="PrivacyManager" language="en_US">
		<synopsis>
			Require phone number to be entered, if no CallerID sent
		</synopsis>
		<syntax>
			<parameter name="maxretries">
				<para>Total tries caller is allowed to input a callerid. Defaults to <literal>3</literal>.</para>
			</parameter>
			<parameter name="minlength">
				<para>Minimum allowable digits in the input callerid number. Defaults to <literal>10</literal>.</para>
			</parameter>
			<parameter name="context">
				<para>Context to check the given callerid against patterns.</para>
			</parameter>
		</syntax>
		<description>
			<para>If no Caller*ID is sent, PrivacyManager answers the channel and asks
			the caller to enter their phone number. The caller is given
			<replaceable>maxretries</replaceable> attempts to do so. The application does
			<emphasis>nothing</emphasis> if Caller*ID was received on the channel.</para>
			<para>The application sets the following channel variable upon completion:</para>
			<variablelist>
				<variable name="PRIVACYMGRSTATUS">
					<para>The status of the privacy manager's attempt to collect a phone number from the user.</para>
					<value name="SUCCESS"/>
					<value name="FAILED"/>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">Zapateller</ref>
		</see-also>
	</application>
 ***/


static char *app = "PrivacyManager";

static int privacy_exec (struct tris_channel *chan, void *data)
{
	int res=0;
	int retries;
	int maxretries = 3;
	int minlength = 10;
	int x = 0;
	char phone[30];
	char *parse = NULL;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(maxretries);
		TRIS_APP_ARG(minlength);
		TRIS_APP_ARG(options);
		TRIS_APP_ARG(checkcontext);
	);

	if (!tris_strlen_zero(chan->cid.cid_num)) {
		tris_verb(3, "CallerID Present: Skipping\n");
	} else {
		/*Answer the channel if it is not already*/
		if (chan->_state != TRIS_STATE_UP) {
			if ((res = tris_answer(chan)))
				return -1;
		}

		if (!tris_strlen_zero(data)) {
			parse = tris_strdupa(data);
			
			TRIS_STANDARD_APP_ARGS(args, parse);

			if (args.maxretries) {
				if (sscanf(args.maxretries, "%30d", &x) == 1)
					maxretries = x;
				else
					tris_log(LOG_WARNING, "Invalid max retries argument\n");
			}
			if (args.minlength) {
				if (sscanf(args.minlength, "%30d", &x) == 1)
					minlength = x;
				else
					tris_log(LOG_WARNING, "Invalid min length argument\n");
			}
		}		

		/* Play unidentified call */
		res = tris_safe_sleep(chan, 1000);
		if (!res)
			res = tris_streamfile(chan, "privacy-unident", chan->language);
		if (!res)
			res = tris_waitstream(chan, "");

		/* Ask for 10 digit number, give 3 attempts */
		for (retries = 0; retries < maxretries; retries++) {
			if (!res)
				res = tris_streamfile(chan, "privacy-prompt", chan->language);
			if (!res)
				res = tris_waitstream(chan, "");

			if (!res ) 
				res = tris_readstring(chan, phone, sizeof(phone) - 1, /* digit timeout ms */ 3200, /* first digit timeout */ 5000, "#");

			if (res < 0)
				break;

			/* Make sure we get at least digits */
			if (strlen(phone) >= minlength ) {
				/* if we have a checkcontext argument, do pattern matching */
				if (!tris_strlen_zero(args.checkcontext)) {
					if (!tris_exists_extension(NULL, args.checkcontext, phone, 1, NULL)) {
						res = tris_streamfile(chan, "privacy-incorrect", chan->language);
						if (!res) {
							res = tris_waitstream(chan, "");
						}
					} else {
						break;
					}
				} else {
					break;
				}
			} else {
				res = tris_streamfile(chan, "privacy-incorrect", chan->language);
				if (!res)
					res = tris_waitstream(chan, "");
			}
		}
		
		/* Got a number, play sounds and send them on their way */
		if ((retries < maxretries) && res >= 0 ) {
			res = tris_streamfile(chan, "privacy-thankyou", chan->language);
			if (!res)
				res = tris_waitstream(chan, "");

			tris_set_callerid (chan, phone, "Privacy Manager", NULL); 

			/* Clear the unavailable presence bit so if it came in on PRI
			 * the caller id will now be passed out to other channels
			 */
			chan->cid.cid_pres &= (TRIS_PRES_UNAVAILABLE ^ 0xFF);

			tris_verb(3, "Changed Caller*ID to %s, callerpres to %d\n",phone,chan->cid.cid_pres);

			pbx_builtin_setvar_helper(chan, "PRIVACYMGRSTATUS", "SUCCESS");
		} else {
			pbx_builtin_setvar_helper(chan, "PRIVACYMGRSTATUS", "FAILED");
		}
	}

	return 0;
}

static int unload_module(void)
{
	return tris_unregister_application (app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, privacy_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Require phone number to be entered, if no CallerID sent");
