/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
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
 * \brief Return the current Version strings
 * 
 * \author Steve Murphy (murf@digium.com)
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"
#include "trismedia/tris_version.h"
#include "trismedia/build.h"

/*** DOCUMENTATION
	<function name="VERSION" language="en_US">
		<synopsis>
			Return the Version info for this Trismedia.
		</synopsis>
		<syntax>
			<parameter name="info">
				<para>The possible values are:</para>
				<enumlist>
					<enum name="TRISMEDIA_VERSION_NUM">
						<para>A string of digits is returned (right now fixed at 999999).</para>
					</enum>
					<enum name="BUILD_USER">
						<para>The string representing the user's name whose account
						was used to configure Trismedia, is returned.</para>
					</enum>
					<enum name="BUILD_HOSTNAME">
						<para>The string representing the name of the host on which Trismedia was configured, is returned.</para>
					</enum>
					<enum name="BUILD_MACHINE">
						<para>The string representing the type of machine on which Trismedia was configured, is returned.</para>
					</enum>
					<enum name="BUILD_OS">
						<para>The string representing the OS of the machine on which Trismedia was configured, is returned.</para>
					</enum>
					<enum name="BUILD_DATE">
						<para>The string representing the date on which Trismedia was configured, is returned.</para>
					</enum>
					<enum name="BUILD_KERNEL">
						<para>The string representing the kernel version of the machine on which Trismedia
						was configured, is returned.</para>
					</enum>
				</enumlist>
			</parameter>
		</syntax>
		<description>
			<para>If there are no arguments, return the version of Trismedia in this format: SVN-branch-1.4-r44830M</para>
			<para>Example:  Set(junky=${VERSION()};</para>
			<para>Sets junky to the string <literal>SVN-branch-1.6-r74830M</literal>, or possibly, <literal>SVN-trunk-r45126M</literal>.</para>
		</description>
	</function>
 ***/

static int acf_version_exec(struct tris_channel *chan, const char *cmd,
			 char *parse, char *buffer, size_t buflen)
{
	const char *response_char = tris_get_version();
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(info);
	);

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (!tris_strlen_zero(args.info) ) {
		if (!strcasecmp(args.info,"TRISMEDIA_VERSION_NUM"))
			response_char = tris_get_version_num();
		else if (!strcasecmp(args.info,"BUILD_USER"))
			response_char = BUILD_USER;
		else if (!strcasecmp(args.info,"BUILD_HOSTNAME"))
			response_char = BUILD_HOSTNAME;
		else if (!strcasecmp(args.info,"BUILD_MACHINE"))
			response_char = BUILD_MACHINE;
		else if (!strcasecmp(args.info,"BUILD_KERNEL"))
			response_char = BUILD_KERNEL;
		else if (!strcasecmp(args.info,"BUILD_OS"))
			response_char = BUILD_OS;
		else if (!strcasecmp(args.info,"BUILD_DATE"))
			response_char = BUILD_DATE;
	}

	tris_debug(1, "VERSION returns %s result, given %s argument\n", response_char, args.info);

	tris_copy_string(buffer, response_char, buflen);

	return 0;
}

static struct tris_custom_function acf_version = {
	.name = "VERSION",
	.read = acf_version_exec,
};

static int unload_module(void)
{
	tris_custom_function_unregister(&acf_version);

	return 0;
}

static int load_module(void)
{
	return tris_custom_function_register(&acf_version);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Get Trismedia Version/Build Info");
