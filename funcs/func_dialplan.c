/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Digium, Inc.
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
 * \brief Dialplan group functions check if a dialplan entry exists
 *
 * \author Gregory Nietsky AKA irroot <gregory@networksentry.co.za>
 * \author Russell Bryant <russell@digium.com>
 * 
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="DIALPLAN_EXISTS" language="en_US">
		<synopsis>
			Checks the existence of a dialplan target.
		</synopsis>
		<syntax>
			<parameter name="context" required="true" />
			<parameter name="extension" />
			<parameter name="priority" />
		</syntax>
		<description>
			<para>This function returns <literal>1</literal> if the target exits. Otherwise, it returns <literal>0</literal>.</para>
		</description>
	</function>

 ***/

static int isexten_function_read(struct tris_channel *chan, const char *cmd, char *data, 
	char *buf, size_t len) 
{
	char *parse;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(context);
		TRIS_APP_ARG(exten);
		TRIS_APP_ARG(priority);
	);

	strcpy(buf, "0");

	if (tris_strlen_zero(data)) {
		tris_log(LOG_ERROR, "DIALPLAN_EXISTS() requires an argument\n");
		return -1;
	}

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (!tris_strlen_zero(args.priority)) {
		int priority_num;
		if (sscanf(args.priority, "%30d", &priority_num) == 1 && priority_num > 0) {
			int res;
			res = tris_exists_extension(chan, args.context, args.exten, priority_num, 
				chan->cid.cid_num);
			if (res)
				strcpy(buf, "1");
		} else {
			int res;
			res = tris_findlabel_extension(chan, args.context, args.exten, 
				args.priority, chan->cid.cid_num);
			if (res > 0)
				strcpy(buf, "1");
		}
	} else if (!tris_strlen_zero(args.exten)) {
		int res;
		res = tris_exists_extension(chan, args.context, args.exten, 1, 
			chan->cid.cid_num);
		if (res)
			strcpy(buf, "1");
	} else if (!tris_strlen_zero(args.context)) {
		if (tris_context_find(args.context))
			strcpy(buf, "1");
	} else {
		tris_log(LOG_ERROR, "Invalid arguments provided to DIALPLAN_EXISTS\n");
		return -1;
	}
	
	return 0;
}

static struct tris_custom_function isexten_function = {
	.name = "DIALPLAN_EXISTS",
	.read = isexten_function_read,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&isexten_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&isexten_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Dialplan Context/Extension/Priority Checking Functions");
