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
 * \brief App to transmit an image
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 154578 $")

#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/image.h"

static char *app = "SendImage";

/*** DOCUMENTATION
	<application name="SendImage" language="en_US">
		<synopsis>
			Sends an image file.
		</synopsis>
		<syntax>
			<parameter name="filename" required="true">
				<para>Path of the filename (image) to send.</para>
			</parameter>
		</syntax>
		<description>
			<para>Send an image file on a channel supporting it.</para>
			<para>Result of transmission will be stored in <variable>SENDIMAGESTATUS</variable></para>
			<variablelist>
				<variable name="SENDIMAGESTATUS">
					<value name="SUCCESS">
						Transmission succeeded.
					</value>
					<value name="FAILURE">
						Transmission failed.
					</value>
					<value name="UNSUPPORTED">
						Image transmission not supported by channel.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">SendText</ref>
			<ref type="application">SendURL</ref>
		</see-also>
	</application>
 ***/

static int sendimage_exec(struct tris_channel *chan, void *data)
{

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "SendImage requires an argument (filename)\n");
		return -1;
	}

	if (!tris_supports_images(chan)) {
		/* Does not support transport */
		pbx_builtin_setvar_helper(chan, "SENDIMAGESTATUS", "UNSUPPORTED");
		return 0;
	}

	if (!tris_send_image(chan, data)) {
		pbx_builtin_setvar_helper(chan, "SENDIMAGESTATUS", "SUCCESS");
	} else {
		pbx_builtin_setvar_helper(chan, "SENDIMAGESTATUS", "FAILURE");
	}
	
	return 0;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, sendimage_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Image Transmission Application");
