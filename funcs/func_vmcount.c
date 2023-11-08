/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2006 Tilghman Lesher.  All rights reserved.
 * 
 * Tilghman Lesher <trismedia-vmcount-func@the-tilghman.com>
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
 * \brief VMCOUNT dialplan function
 *
 * \author Tilghman Lesher <trismedia-vmcount-func@the-tilghman.com>
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include <dirent.h>

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="VMCOUNT" language="en_US">
		<synopsis>
			Count the voicemails in a specified mailbox.
		</synopsis>
		<syntax>
			<parameter name="vmbox" required="true" argsep="@">
				<argument name="vmbox" required="true" />
				<argument name="context" required="false">
					<para>If not specified, defaults to <literal>default</literal>.</para>
				</argument>
			</parameter>
			<parameter name="folder" required="false">
				<para>If not specified, defaults to <literal>INBOX</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>Count the number of voicemails in a specified mailbox, you could also specify 
			the <replaceable>context</replaceable> and the mailbox <replaceable>folder</replaceable>.</para>
			<para>Example: <literal>exten => s,1,Set(foo=${VMCOUNT(125)})</literal></para>
		</description>
	</function>
 ***/

static int acf_vmcount_exec(struct tris_channel *chan, const char *cmd, char *argsstr, char *buf, size_t len)
{
	char *context;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(vmbox);
		TRIS_APP_ARG(folder);
	);

	buf[0] = '\0';

	if (tris_strlen_zero(argsstr))
		return -1;

	TRIS_STANDARD_APP_ARGS(args, argsstr);

	if (strchr(args.vmbox, '@')) {
		context = args.vmbox;
		args.vmbox = strsep(&context, "@");
	} else {
		context = "default";
	}

	if (tris_strlen_zero(args.folder)) {
		args.folder = "INBOX";
	}

	snprintf(buf, len, "%d", tris_app_messagecount(context, args.vmbox, args.folder));
	
	return 0;
}

struct tris_custom_function acf_vmcount = {
	.name = "VMCOUNT",
	.read = acf_vmcount_exec,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&acf_vmcount);
}

static int load_module(void)
{
	return tris_custom_function_register(&acf_vmcount);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Indicator for whether a voice mailbox has messages in a given folder.");
