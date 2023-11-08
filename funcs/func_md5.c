/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005-2006, Digium, Inc.
 * Copyright (C) 2005, Olle E. Johansson, Edvina.net
 * Copyright (C) 2005, Russell Bryant <russelb@clemson.edu> 
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
 * \brief MD5 digest related dialplan functions
 * 
 * \author Olle E. Johansson <oej@edvina.net>
 * \author Russell Bryant <russelb@clemson.edu>
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/module.h"
#include "trismedia/pbx.h"

/*** DOCUMENTATION
	<function name="MD5" language="en_US">
		<synopsis>
			Computes an MD5 digest.
		</synopsis>
		<syntax>
			<parameter name="data" required="true" />
		</syntax>
		<description>
			<para>Computes an MD5 digest.</para>
		</description>
	</function>
 ***/

static int md5(struct tris_channel *chan, const char *cmd, char *data,
	       char *buf, size_t len)
{
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Syntax: MD5(<data>) - missing argument!\n");
		return -1;
	}

	tris_md5_hash(buf, data);
	buf[32] = '\0';

	return 0;
}

static struct tris_custom_function md5_function = {
	.name = "MD5",
	.read = md5,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&md5_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&md5_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "MD5 digest dialplan functions");
