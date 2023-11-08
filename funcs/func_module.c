/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Simple module check function
 * \author Olle E. Johansson, Edvina.net
 *
 * \ingroup functions
 */
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/module.h"
#include "trismedia/pbx.h"

/*** DOCUMENTATION
	<function name="IFMODULE" language="en_US">
		<synopsis>
			Checks if an Trismedia module is loaded in memory.
		</synopsis>
		<syntax>
			<parameter name="modulename.so" required="true">
				<para>Module name complete with <literal>.so</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>Checks if a module is loaded. Use the full module name
			as shown by the list in <literal>module list</literal>.
			Returns <literal>1</literal> if module exists in memory, otherwise <literal>0</literal></para>
		</description>
	</function>
 ***/

static int ifmodule_read(struct tris_channel *chan, const char *cmd, char *data,
		    char *buf, size_t len)
{
	char *ret = "0";

	*buf = '\0';

	if (data)
		if (tris_module_check(data))
			ret = "1";

	tris_copy_string(buf, ret, len);

	return 0;
}

static struct tris_custom_function ifmodule_function = {
	.name = "IFMODULE",
	.read = ifmodule_read,
};


static int unload_module(void)
{
	return tris_custom_function_unregister(&ifmodule_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&ifmodule_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Checks if Trismedia module is loaded in memory");
