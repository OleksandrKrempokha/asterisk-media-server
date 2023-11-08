/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Martin Pycko <martinp@digium.com>
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
 * \brief Applications connected with CDR engine
 *
 * \author Martin Pycko <martinp@digium.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/channel.h"
#include "trismedia/module.h"

/*** DOCUMENTATION
	<application name="NoCDR" language="en_US">
		<synopsis>
			Tell Trismedia to not maintain a CDR for the current call
		</synopsis>
		<syntax />
		<description>
			<para>This application will tell Trismedia not to maintain a CDR for the current call.</para>
		</description>
	</application>
 ***/

static char *nocdr_app = "NoCDR";

static int nocdr_exec(struct tris_channel *chan, void *data)
{
	if (chan->cdr)
		tris_set_flag(chan->cdr, TRIS_CDR_FLAG_POST_DISABLED);

	return 0;
}

static int unload_module(void)
{
	return tris_unregister_application(nocdr_app);
}

static int load_module(void)
{
	if (tris_register_application_xml(nocdr_app, nocdr_exec))
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Tell Trismedia to not maintain a CDR for the current call");
