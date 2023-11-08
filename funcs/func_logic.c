/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 * Portions Copyright (C) 2005, Anthony Minessale II
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
 * \brief Conditional logic dialplan functions
 * 
 * \author Anthony Minessale II
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 168547 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="ISNULL" language="en_US">
		<synopsis>
			Check if a value is NULL.
		</synopsis>
		<syntax>
			<parameter name="data" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>1</literal> if NULL or <literal>0</literal> otherwise.</para>
		</description>
	</function>
	<function name="SET" language="en_US">
		<synopsis>
			SET assigns a value to a channel variable.
		</synopsis>
		<syntax argsep="=">
			<parameter name="varname" required="true" />
			<parameter name="value" />
		</syntax>
		<description>
		</description>
	</function>
	<function name="EXISTS" language="en_US">
		<synopsis>
			Test the existence of a value.
		</synopsis>
		<syntax>
			<parameter name="data" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>1</literal> if exists, <literal>0</literal> otherwise.</para>
		</description>
	</function>
	<function name="IF" language="en_US">
		<synopsis>
			Check for an expresion.
		</synopsis>
		<syntax argsep="?">
			<parameter name="expresion" required="true" />
			<parameter name="retvalue" argsep=":" required="true">
				<argument name="true" />
				<argument name="false" />
			</parameter>
		</syntax>
		<description>
			<para>Returns the data following <literal>?</literal> if true, else the data following <literal>:</literal></para>
		</description>	
	</function>
	<function name="IFTIME" language="en_US">
		<synopsis>
			Temporal Conditional.
		</synopsis>
		<syntax argsep="?">
			<parameter name="timespec" required="true" />
			<parameter name="retvalue" required="true" argsep=":">
				<argument name="true" />
				<argument name="false" />
			</parameter>
		</syntax>
		<description>
			<para>Returns the data following <literal>?</literal> if true, else the data following <literal>:</literal></para>
		</description>
	</function>
	<function name="IMPORT" language="en_US">
		<synopsis>
			Retrieve the value of a variable from another channel.
		</synopsis>
		<syntax>
			<parameter name="channel" required="true" />
			<parameter name="variable" required="true" />
		</syntax>
		<description>
		</description>
	</function>
 ***/

static int isnull(struct tris_channel *chan, const char *cmd, char *data,
		  char *buf, size_t len)
{
	strcpy(buf, data && *data ? "0" : "1");

	return 0;
}

static int exists(struct tris_channel *chan, const char *cmd, char *data, char *buf,
		  size_t len)
{
	strcpy(buf, data && *data ? "1" : "0");

	return 0;
}

static int iftime(struct tris_channel *chan, const char *cmd, char *data, char *buf,
		  size_t len)
{
	struct tris_timing timing;
	char *expr;
	char *iftrue;
	char *iffalse;

	data = tris_strip_quoted(data, "\"", "\"");
	expr = strsep(&data, "?");
	iftrue = strsep(&data, ":");
	iffalse = data;

	if (tris_strlen_zero(expr) || !(iftrue || iffalse)) {
		tris_log(LOG_WARNING,
				"Syntax IFTIME(<timespec>?[<true>][:<false>])\n");
		return -1;
	}

	if (!tris_build_timing(&timing, expr)) {
		tris_log(LOG_WARNING, "Invalid Time Spec.\n");
		tris_destroy_timing(&timing);
		return -1;
	}

	if (iftrue)
		iftrue = tris_strip_quoted(iftrue, "\"", "\"");
	if (iffalse)
		iffalse = tris_strip_quoted(iffalse, "\"", "\"");

	tris_copy_string(buf, tris_check_timing(&timing) ? S_OR(iftrue, "") : S_OR(iffalse, ""), len);
	tris_destroy_timing(&timing);

	return 0;
}

static int acf_if(struct tris_channel *chan, const char *cmd, char *data, char *buf,
		  size_t len)
{
	TRIS_DECLARE_APP_ARGS(args1,
		TRIS_APP_ARG(expr);
		TRIS_APP_ARG(remainder);
	);
	TRIS_DECLARE_APP_ARGS(args2,
		TRIS_APP_ARG(iftrue);
		TRIS_APP_ARG(iffalse);
	);
	args2.iftrue = args2.iffalse = NULL; /* you have to set these, because if there is nothing after the '?',
											then args1.remainder will be NULL, not a pointer to a null string, and
											then any garbage in args2.iffalse will not be cleared, and you'll crash.
										    -- and if you mod the tris_app_separate_args func instead, you'll really
											mess things up badly, because the rest of everything depends on null args
											for non-specified stuff. */
	
	TRIS_NONSTANDARD_APP_ARGS(args1, data, '?');
	TRIS_NONSTANDARD_APP_ARGS(args2, args1.remainder, ':');

	if (tris_strlen_zero(args1.expr) || !(args2.iftrue || args2.iffalse)) {
		tris_log(LOG_WARNING, "Syntax IF(<expr>?[<true>][:<false>])  (expr must be non-null, and either <true> or <false> must be non-null)\n");
		tris_log(LOG_WARNING, "      In this case, <expr>='%s', <true>='%s', and <false>='%s'\n", args1.expr, args2.iftrue, args2.iffalse);
		return -1;
	}

	args1.expr = tris_strip(args1.expr);
	if (args2.iftrue)
		args2.iftrue = tris_strip(args2.iftrue);
	if (args2.iffalse)
		args2.iffalse = tris_strip(args2.iffalse);

	tris_copy_string(buf, pbx_checkcondition(args1.expr) ? (S_OR(args2.iftrue, "")) : (S_OR(args2.iffalse, "")), len);

	return 0;
}

static int set(struct tris_channel *chan, const char *cmd, char *data, char *buf,
	       size_t len)
{
	char *varname;
	char *val;

	varname = strsep(&data, "=");
	val = data;

	if (tris_strlen_zero(varname) || !val) {
		tris_log(LOG_WARNING, "Syntax SET(<varname>=[<value>])\n");
		return -1;
	}

	varname = tris_strip(varname);
	val = tris_strip(val);
	pbx_builtin_setvar_helper(chan, varname, val);
	tris_copy_string(buf, val, len);

	return 0;
}

static int acf_import(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(channel);
		TRIS_APP_ARG(varname);
	);
	TRIS_STANDARD_APP_ARGS(args, data);
	buf[0] = 0;
	if (!tris_strlen_zero(args.varname)) {
		struct tris_channel *chan2 = tris_get_channel_by_name_locked(args.channel);
		if (chan2) {
			char *s = alloca(strlen(args.varname) + 4);
			if (s) {
				sprintf(s, "${%s}", args.varname);
				pbx_substitute_variables_helper(chan2, s, buf, len);
			}
			tris_channel_unlock(chan2);
		}
	}
	return 0;
}

static struct tris_custom_function isnull_function = {
	.name = "ISNULL",
	.read = isnull,
};

static struct tris_custom_function set_function = {
	.name = "SET",
	.read = set,
};

static struct tris_custom_function exists_function = {
	.name = "EXISTS",
	.read = exists,
};

static struct tris_custom_function if_function = {
	.name = "IF",
	.read = acf_if,
};

static struct tris_custom_function if_time_function = {
	.name = "IFTIME",
	.read = iftime,
};

static struct tris_custom_function import_function = {
	.name = "IMPORT",
	.read = acf_import,
};

static int unload_module(void)
{
	int res = 0;

	res |= tris_custom_function_unregister(&isnull_function);
	res |= tris_custom_function_unregister(&set_function);
	res |= tris_custom_function_unregister(&exists_function);
	res |= tris_custom_function_unregister(&if_function);
	res |= tris_custom_function_unregister(&if_time_function);
	res |= tris_custom_function_unregister(&import_function);

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= tris_custom_function_register(&isnull_function);
	res |= tris_custom_function_register(&set_function);
	res |= tris_custom_function_register(&exists_function);
	res |= tris_custom_function_register(&if_function);
	res |= tris_custom_function_register(&if_time_function);
	res |= tris_custom_function_register(&import_function);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Logical dialplan functions");
