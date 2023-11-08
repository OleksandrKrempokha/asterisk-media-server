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
 * \brief IVR Demo application
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153747 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="IVRDemo" language="en_US">
		<synopsis>
			IVR Demo Application.
		</synopsis>
		<syntax>
			<parameter name="filename" required="true" />
		</syntax>
		<description>
			<para>This is a skeleton application that shows you the basic structure to create your
			own trismedia applications and demonstrates the IVR demo.</para>
		</description>
	</application>
 ***/

static char *app = "IVRDemo";

static int ivr_demo_func(struct tris_channel *chan, void *data)
{
	tris_verbose("IVR Demo, data is %s!\n", (char *)data);
	return 0;
}

TRIS_IVR_DECLARE_MENU(ivr_submenu, "IVR Demo Sub Menu", 0, 
{
	{ "s", TRIS_ACTION_BACKGROUND, "demo-abouttotry" },
	{ "s", TRIS_ACTION_WAITOPTION },
	{ "1", TRIS_ACTION_PLAYBACK, "digits/1" },
	{ "1", TRIS_ACTION_PLAYBACK, "digits/1" },
	{ "1", TRIS_ACTION_RESTART },
	{ "2", TRIS_ACTION_PLAYLIST, "digits/2;digits/3" },
	{ "3", TRIS_ACTION_CALLBACK, ivr_demo_func },
	{ "4", TRIS_ACTION_TRANSFER, "demo|s|1" },
	{ "*", TRIS_ACTION_REPEAT },
	{ "#", TRIS_ACTION_UPONE  },
	{ NULL }
});

TRIS_IVR_DECLARE_MENU(ivr_demo, "IVR Demo Main Menu", 0, 
{
	{ "s", TRIS_ACTION_BACKGROUND, "demo-congrats" },
	{ "g", TRIS_ACTION_BACKGROUND, "demo-instruct" },
	{ "g", TRIS_ACTION_WAITOPTION },
	{ "1", TRIS_ACTION_PLAYBACK, "digits/1" },
	{ "1", TRIS_ACTION_RESTART },
	{ "2", TRIS_ACTION_MENU, &ivr_submenu },
	{ "2", TRIS_ACTION_RESTART },
	{ "i", TRIS_ACTION_PLAYBACK, "invalid" },
	{ "i", TRIS_ACTION_REPEAT, (void *)(unsigned long)2 },
	{ "#", TRIS_ACTION_EXIT },
	{ NULL },
});


static int skel_exec(struct tris_channel *chan, void *data)
{
	int res=0;
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "skel requires an argument (filename)\n");
		return -1;
	}
	
	/* Do our thing here */

	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);
	if (!res)
		res = tris_ivr_menu_run(chan, &ivr_demo, data);
	
	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, skel_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "IVR Demo Application");
