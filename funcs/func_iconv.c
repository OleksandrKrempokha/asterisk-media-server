/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2005,2006,2007 Sven Slezak <sunny@mezzo.net>
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

/*!
 * \file
 *
 * \brief Charset conversions
 *
 * \author Sven Slezak <sunny@mezzo.net>
 *
 * \ingroup functions
 */

/*** MODULEINFO
    <depend>iconv</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include <ctype.h>
#include <iconv.h>

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="ICONV" language="en_US">
		<synopsis>
			Converts charsets of strings.	
		</synopsis>
		<syntax>
			<parameter name="in-charset" required="true">
				<para>Input charset</para>
			</parameter>
			<parameter name="out-charset" required="true">
				<para>Output charset</para>
			</parameter>
			<parameter name="string" required="true">
				<para>String to convert, from <replaceable>in-charset</replaceable> to <replaceable>out-charset</replaceable></para>
			</parameter>
		</syntax>
		<description>
			<para>Converts string from <replaceable>in-charset</replaceable> into <replaceable>out-charset</replaceable>.
			For available charsets, use <literal>iconv -l</literal> on your shell command line.</para>
			<note><para>Due to limitations within the API, ICONV will not currently work with
			charsets with embedded NULLs. If found, the string will terminate.</para></note>
		</description>
	</function>
 ***/


/*! 
 * Some systems define the second arg to iconv() as (const char *),
 * while others define it as (char *).  Cast it to a (void *) to 
 * suppress compiler warnings about it. 
 */
#define TRIS_ICONV_CAST void *

static int iconv_read(struct tris_channel *chan, const char *cmd, char *arguments, char *buf, size_t len)
{
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(in_charset);
		TRIS_APP_ARG(out_charset);
		TRIS_APP_ARG(text);
	);
	iconv_t cd;
	size_t incount, outcount = len;
	char *parse;

	if (tris_strlen_zero(arguments)) {
		tris_log(LOG_WARNING, "Syntax: ICONV(<in-charset>,<out-charset>,<text>) - missing arguments!\n");
		return -1;
	}

	parse = tris_strdupa(arguments);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (args.argc < 3) {
		tris_log(LOG_WARNING, "Syntax: ICONV(<in-charset>,<out-charset>,<text>) %d\n", args.argc);
		return -1;
	}

	incount = strlen(args.text);

	tris_debug(1, "Iconv: \"%s\" %s -> %s\n", args.text, args.in_charset, args.out_charset);

	cd = iconv_open(args.out_charset, args.in_charset);

	if (cd == (iconv_t) -1) {
		tris_log(LOG_ERROR, "conversion from '%s' to '%s' not available. type 'iconv -l' in a shell to list the supported charsets.\n", args.in_charset, args.out_charset);
		return -1;
	}

	if (iconv(cd, (TRIS_ICONV_CAST) &args.text, &incount, &buf, &outcount) == (size_t) -1) {
		if (errno == E2BIG)
			tris_log(LOG_WARNING, "Iconv: output buffer too small.\n");
		else if (errno == EILSEQ)
			tris_log(LOG_WARNING,  "Iconv: illegal character.\n");
		else if (errno == EINVAL)
			tris_log(LOG_WARNING,  "Iconv: incomplete character sequence.\n");
		else
			tris_log(LOG_WARNING,  "Iconv: error %d: %s.\n", errno, strerror(errno));
	}
	iconv_close(cd);

	return 0;
}


static struct tris_custom_function iconv_function = {
	.name = "ICONV",
	.read = iconv_read
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&iconv_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&iconv_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Charset conversions");

