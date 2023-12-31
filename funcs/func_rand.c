/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
 * Copyright (C) 2006, Claude Patry
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
 * \brief Generate Random Number
 * 
 * \author Claude Patry <cpatry@gmail.com>
 * \author Tilghman Lesher ( http://trismedia.drunkcoder.com/ )
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="RAND" language="en_US">
		<synopsis>
			Choose a random number in a range.			
		</synopsis>
		<syntax>
			<parameter name="min" />
			<parameter name="max" />
		</syntax>
		<description>
			<para>Choose a random number between <replaceable>min</replaceable> and <replaceable>max</replaceable>. 
			<replaceable>min</replaceable> defaults to <literal>0</literal>, if not specified, while <replaceable>max</replaceable> defaults 
			to <literal>RAND_MAX</literal> (2147483647 on many systems).</para>
			<para>Example:  Set(junky=${RAND(1,8)});
			Sets junky to a random number between 1 and 8, inclusive.</para>
		</description>
	</function>
 ***/
static int acf_rand_exec(struct tris_channel *chan, const char *cmd,
			 char *parse, char *buffer, size_t buflen)
{
	int min_int, response_int, max_int;
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(min);
			     TRIS_APP_ARG(max);
	);

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (tris_strlen_zero(args.min) || sscanf(args.min, "%30d", &min_int) != 1)
		min_int = 0;

	if (tris_strlen_zero(args.max) || sscanf(args.max, "%30d", &max_int) != 1)
		max_int = RAND_MAX;

	if (max_int < min_int) {
		int tmp = max_int;

		max_int = min_int;
		min_int = tmp;
		tris_debug(1, "max<min\n");
	}

	response_int = min_int + (tris_random() % (max_int - min_int + 1));
	tris_debug(1, "%d was the lucky number in range [%d,%d]\n", response_int, min_int, max_int);
	snprintf(buffer, buflen, "%d", response_int);

	return 0;
}

static struct tris_custom_function acf_rand = {
	.name = "RAND",
	.read = acf_rand_exec,
};

static int unload_module(void)
{
	tris_custom_function_unregister(&acf_rand);

	return 0;
}

static int load_module(void)
{
	return tris_custom_function_register(&acf_rand);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Random number dialplan function");
