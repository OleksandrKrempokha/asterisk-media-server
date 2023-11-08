/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Ivr application -- dial and call
 *
 * \author cool@voipteam.com
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 249952 $")

#include "trismedia/file.h"
#include "trismedia/module.h"
#include "trismedia/channel.h"

/*** DOCUMENTATION
	<application name="Ivr" language="en_US">
		<synopsis>
			dial and call
		</synopsis>
		<syntax />
		<description>
			<para>dial number and press '#'</para>
		</description>
	</application>
 ***/

static char *app = "Ivr";

static int ivr_exec(struct tris_channel *chan, void *data)
{
	int res = -1;
	char *status;
	char dest[81], args[128];
	
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	int calling_tel_res = -1;
	char calling_telnum[100] ="" ;
	char calling_uri[100];
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);
	
	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	tris_app_getdata(chan,"ivr/dial_extn_pound",dest, sizeof(dest) - 1,5000);
	if(!strlen(dest)) {
		return 0;
	}
	
	struct tris_app *the_app = pbx_findapp("Dial");

	/* All is well... execute the application */
	if(the_app) {
		snprintf(args, sizeof(args), "SIP/%s@%s:5060,45", dest, tris_inet_ntoa(ourip));
		chan->transferchan = 1;
		res = pbx_exec(chan, the_app, args);
	}

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, ivr_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Simple Ivr Application");
