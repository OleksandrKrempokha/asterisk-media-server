/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) <Year>, <Your Name Here>
 *
 * <Your Name Here> <<Your Email Here>>
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
 *
 * Please follow coding guidelines 
 * http://svn.digium.com/view/trismedia/trunk/doc/CODING-GUIDELINES
 */

/*! \file
 *
 * \brief Skeleton application
 *
 * \author\verbatim <Your Name Here> <<Your Email Here>> \endverbatim
 * 
 * This is a skeleton for development of an Trismedia application 
 * \ingroup applications
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="Skel" language="en_US">
		<synopsis>
			Simple one line explaination.
		</synopsis>
		<syntax>
			<parameter name="dummy" required="true"/>
			<parameter name="options">
				<optionlist>
					<option name="a">
						<para>Option A.</para>
					</option>
					<option name="b">
						<para>Option B.</para>
					</option>
					<option name="c">
						<para>Option C.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
		<para>This application is a template to build other applications from. 
		It shows you the basic structure to create your own Trismedia applications.</para>
		</description>
	</application>
 ***/

static char *app = "Skel";

enum {
	OPTION_A = (1 << 0),
	OPTION_B = (1 << 1),
	OPTION_C = (1 << 2),
} option_flags;

enum {
	OPTION_ARG_B = 0,
	OPTION_ARG_C = 1,
	/* This *must* be the last value in this enum! */
	OPTION_ARG_ARRAY_SIZE = 2,
} option_args;

TRIS_APP_OPTIONS(app_opts,{
	TRIS_APP_OPTION('a', OPTION_A),
	TRIS_APP_OPTION_ARG('b', OPTION_B, OPTION_ARG_B),
	TRIS_APP_OPTION_ARG('c', OPTION_C, OPTION_ARG_C),
});


static int app_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_flags flags;
	char *parse, *opts[OPTION_ARG_ARRAY_SIZE];
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(dummy);
		TRIS_APP_ARG(options);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "%s requires an argument (dummy[,options])\n", app);
		return -1;
	}

	/* Do our thing here */

	/* We need to make a copy of the input string if we are going to modify it! */
	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (args.argc == 2) {
		tris_app_parse_options(app_opts, &flags, opts, args.options);
	}

	if (!tris_strlen_zero(args.dummy)) {
		tris_log(LOG_NOTICE, "Dummy value is : %s\n", args.dummy);
	}

	if (tris_test_flag(&flags, OPTION_A)) {
		tris_log(LOG_NOTICE, "Option A is set\n");
	}

	if (tris_test_flag(&flags, OPTION_B)) {
		tris_log(LOG_NOTICE, "Option B is set with : %s\n", opts[OPTION_ARG_B] ? opts[OPTION_ARG_B] : "<unspecified>");
	}

	if (tris_test_flag(&flags, OPTION_C)) {
		tris_log(LOG_NOTICE, "Option C is set with : %s\n", opts[OPTION_ARG_C] ? opts[OPTION_ARG_C] : "<unspecified>");
	}

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, app_exec) ? 
		TRIS_MODULE_LOAD_DECLINE : TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Skeleton (sample) Application");
