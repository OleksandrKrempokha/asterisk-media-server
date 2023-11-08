/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005 - 2006, Digium, Inc.
 * Copyright (C) 2005, Claude Patry
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
 * \brief Use the base64 as functions
 * 
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 228649 $")

#include "trismedia/module.h"
#include "trismedia/pbx.h"	/* function register/unregister */
#include "trismedia/utils.h"

/*** DOCUMENTATION
	<function name="BASE64_ENCODE" language="en_US">
		<synopsis>
			Encode a string in base64.
		</synopsis>
		<syntax>
			<parameter name="string" required="true">
				<para>Input string</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns the base64 string.</para>
		</description>
	</function>
	<function name="BASE64_DECODE" language="en_US">
		<synopsis>
			Decode a base64 string.
		</synopsis>
		<syntax>
			<parameter name="string" required="true">
				<para>Input string.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns the plain text string.</para>
		</description>
	</function>
 ***/

static int base64_encode(struct tris_channel *chan, const char *cmd, char *data,
			 char *buf, size_t len)
{
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Syntax: BASE64_ENCODE(<data>) - missing argument!\n");
		return -1;
	}

	tris_base64encode(buf, (unsigned char *) data, strlen(data), len);

	return 0;
}

static int base64_decode(struct tris_channel *chan, const char *cmd, char *data,
			 char *buf, size_t len)
{
	int decoded_len;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Syntax: BASE64_DECODE(<base_64 string>) - missing argument!\n");
		return -1;
	}

	decoded_len = tris_base64decode((unsigned char *) buf, data, len);
	if (decoded_len <= (len - 1)) {		/* if not truncated, */
		buf[decoded_len] = '\0';
	} else {
		buf[len - 1] = '\0';
	}

	return 0;
}

static struct tris_custom_function base64_encode_function = {
	.name = "BASE64_ENCODE",
	.read = base64_encode,
};

static struct tris_custom_function base64_decode_function = {
	.name = "BASE64_DECODE",
	.read = base64_decode,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&base64_encode_function) |
		tris_custom_function_unregister(&base64_decode_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&base64_encode_function) |
		tris_custom_function_register(&base64_decode_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "base64 encode/decode dialplan functions");
