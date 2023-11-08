/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005-2006, Digium, Inc.
 * Portions Copyright (C) 2005, Tilghman Lesher.  All rights reserved.
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
 * \brief String manipulation dialplan functions
 *
 * \author Tilghman Lesher
 * \author Anothony Minessale II 
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 246207 $")

#include <regex.h>
#include <ctype.h>

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"
#include "trismedia/localtime.h"

TRIS_THREADSTORAGE(result_buf);

/*** DOCUMENTATION
	<function name="FIELDQTY" language="en_US">
		<synopsis>
			Count the fields with an arbitrary delimiter
		</synopsis>
		<syntax>
			<parameter name="varname" required="true" />
			<parameter name="delim" required="true" />
		</syntax>
		<description>
			<para>The delimiter may be specified as a special or extended ASCII character, by encoding it.  The characters
			<literal>\n</literal>, <literal>\r</literal>, and <literal>\t</literal> are all recognized as the newline,
			carriage return, and tab characters, respectively.  Also, octal and hexadecimal specifications are recognized
			by the patterns <literal>\0nnn</literal> and <literal>\xHH</literal>, respectively.  For example, if you wanted
			to encode a comma as the delimiter, you could use either <literal>\054</literal> or <literal>\x2C</literal>.</para>
			<para>Example: If ${example} contains <literal>ex-amp-le</literal>, then ${FIELDQTY(example,-)} returns 3.</para>
		</description>
	</function>
	<function name="LISTFILTER" language="en_US">
		<synopsis>Remove an item from a list, by name.</synopsis>
		<syntax>
			<parameter name="varname" required="true" />
			<parameter name="delim" required="true" default="," />
			<parameter name="value" required="true" />
		</syntax>
		<description>
			<para>Remove <replaceable>value</replaceable> from the list contained in the <replaceable>varname</replaceable>
			variable, where the list delimiter is specified by the <replaceable>delim</replaceable> parameter.  This is
			very useful for removing a single channel name from a list of channels, for example.</para>
		</description>
	</function>
	<function name="FILTER" language="en_US">
		<synopsis>
			Filter the string to include only the allowed characters
		</synopsis>
		<syntax>
			<parameter name="allowed-chars" required="true" />
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Permits all characters listed in <replaceable>allowed-chars</replaceable>, 
			filtering all others outs. In addition to literally listing the characters, 
			you may also use ranges of characters (delimited by a <literal>-</literal></para>
			<para>Hexadecimal characters started with a <literal>\x</literal>(i.e. \x20)</para>
			<para>Octal characters started with a <literal>\0</literal> (i.e. \040)</para>
			<para>Also <literal>\t</literal>,<literal>\n</literal> and <literal>\r</literal> are recognized.</para> 
			<note><para>If you want the <literal>-</literal> character it needs to be prefixed with a 
			<literal>\</literal></para></note>
		</description>
	</function>
	<function name="REGEX" language="en_US">
		<synopsis>
			Check string against a regular expression.
		</synopsis>
		<syntax argsep=" ">
			<parameter name="&quot;regular expression&quot;" required="true" />
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Return <literal>1</literal> on regular expression match or <literal>0</literal> otherwise</para>
			<para>Please note that the space following the double quotes separating the 
			regex from the data is optional and if present, is skipped. If a space is 
			desired at the beginning of the data, then put two spaces there; the second 
			will not be skipped.</para>
		</description>
	</function>
	<application name="ClearHash" language="en_US">
		<synopsis>
			Clear the keys from a specified hashname.
		</synopsis>
		<syntax>
			<parameter name="hashname" required="true" />
		</syntax>
		<description>
			<para>Clears all keys out of the specified <replaceable>hashname</replaceable>.</para>
		</description>
	</application>
	<function name="HASH" language="en_US">
		<synopsis>
			Implementation of a dialplan associative array
		</synopsis>
		<syntax>
			<parameter name="hashname" required="true" />
			<parameter name="hashkey" />
		</syntax>
		<description>
			<para>In two arguments mode, gets and sets values to corresponding keys within
			a named associative array. The single-argument mode will only work when assigned
			to from a function defined by func_odbc</para>
		</description>
	</function>
	<function name="HASHKEYS" language="en_US">
		<synopsis>
			Retrieve the keys of the HASH() function.
		</synopsis>
		<syntax>
			<parameter name="hashname" required="true" />
		</syntax>
		<description>
			<para>Returns a comma-delimited list of the current keys of the associative array 
			defined by the HASH() function. Note that if you iterate over the keys of 
			the result, adding keys during iteration will cause the result of the HASHKEYS()
			function to change.</para>
		</description>
	</function>
	<function name="KEYPADHASH" language="en_US">
		<synopsis>
			Hash the letters in string into equivalent keypad numbers.
		</synopsis>
		<syntax>
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Example: ${KEYPADHASH(Les)} returns "537"</para>
		</description>
	</function>
	<function name="ARRAY" language="en_US">
		<synopsis>
			Allows setting multiple variables at once.
		</synopsis>
		<syntax>
			<parameter name="var1" required="true" />
			<parameter name="var2" required="false" multiple="true" />
			<parameter name="varN" required="false" />
		</syntax>
		<description>
			<para>The comma-delimited list passed as a value to which the function is set will 
			be interpreted as a set of values to which the comma-delimited list of 
			variable names in the argument should be set.</para>
			<para>Example: Set(ARRAY(var1,var2)=1,2) will set var1 to 1 and var2 to 2</para>
		</description>
	</function>
	<function name="STRPTIME" language="en_US">
		<synopsis>
			Returns the epoch of the arbitrary date/time string structured as described by the format.
		</synopsis>
		<syntax>
			<parameter name="datetime" required="true" />
			<parameter name="timezone" required="true" />
			<parameter name="format" required="true" />
		</syntax>
		<description>
			<para>This is useful for converting a date into <literal>EPOCH</literal> time, 
			possibly to pass to an application like SayUnixTime or to calculate the difference
			between the two date strings</para>
			<para>Example: ${STRPTIME(2006-03-01 07:30:35,America/Chicago,%Y-%m-%d %H:%M:%S)} returns 1141219835</para>
		</description>
	</function>
	<function name="STRFTIME" language="en_US">
		<synopsis>
			Returns the current date/time in the specified format.
		</synopsis>
		<syntax>
			<parameter name="epoch" />
			<parameter name="timezone" />
			<parameter name="format" />
		</syntax>
		<description>
			<para>STRFTIME supports all of the same formats as the underlying C function
			<emphasis>strftime(3)</emphasis>.
			It also supports the following format: <literal>%[n]q</literal> - fractions of a second,
			with leading zeros.</para>
			<para>Example: <literal>%3q</literal> will give milliseconds and <literal>%1q</literal>
			will give tenths of a second. The default is set at milliseconds (n=3).
			The common case is to use it in combination with %S, as in <literal>%S.%3q</literal>.</para>
		</description>
		<see-also>
			<ref type="manpage">strftime(3)</ref>
		</see-also>
	</function>
	<function name="EVAL" language="en_US">
		<synopsis>
			Evaluate stored variables
		</synopsis>
		<syntax>
			<parameter name="variable" required="true" />
		</syntax>
		<description>
			<para>Using EVAL basically causes a string to be evaluated twice.
			When a variable or expression is in the dialplan, it will be
			evaluated at runtime. However, if the results of the evaluation
			is in fact another variable or expression, using EVAL will have it
			evaluated a second time.</para>
			<para>Example: If the <variable>MYVAR</variable> contains
			<variable>OTHERVAR</variable>, then the result of ${EVAL(
			<variable>MYVAR</variable>)} in the dialplan will be the
			contents of <variable>OTHERVAR</variable>. Normally just
			putting <variable>MYVAR</variable> in the dialplan the result
			would be <variable>OTHERVAR</variable>.</para>
		</description>
	</function>
	<function name="TOUPPER" language="en_US">
		<synopsis>
			Convert string to all uppercase letters.
		</synopsis>
		<syntax>
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Example: ${TOUPPER(Example)} returns "EXAMPLE"</para>
		</description>
	</function>
	<function name="TOLOWER" language="en_US">
		<synopsis>
			Convert string to all lowercase letters.
		</synopsis>
		<syntax>
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Example: ${TOLOWER(Example)} returns "example"</para>
		</description>
	</function>
	<function name="LEN" language="en_US">
		<synopsis>
			Return the length of the string given.
		</synopsis>
		<syntax>
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Example: ${LEN(example)} returns 7</para>
		</description>
	</function>
	<function name="QUOTE" language="en_US">
		<synopsis>
			Quotes a given string, escaping embedded quotes as necessary
		</synopsis>
		<syntax>
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Example: ${QUOTE(ab"c"de)} will return "abcde"</para>
		</description>
	</function>
	<function name="CSV_QUOTE" language="en_US">
		<synopsis>
			Quotes a given string for use in a CSV file, escaping embedded quotes as necessary
		</synopsis>
		<syntax>
			<parameter name="string" required="true" />
		</syntax>
		<description>
			<para>Example: ${CSV_QUOTE("a,b" 123)} will return """a,b"" 123"</para>
		</description>
	</function>
 ***/

static int function_fieldqty(struct tris_channel *chan, const char *cmd,
			     char *parse, char *buf, size_t len)
{
	char *varsubst, varval[8192], *varval2 = varval;
	int fieldcount = 0;
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(varname);
			     TRIS_APP_ARG(delim);
		);
	char delim[2] = "";
	size_t delim_used;

	TRIS_STANDARD_APP_ARGS(args, parse);
	if (args.delim) {
		tris_get_encoded_char(args.delim, delim, &delim_used);

		varsubst = alloca(strlen(args.varname) + 4);

		sprintf(varsubst, "${%s}", args.varname);
		pbx_substitute_variables_helper(chan, varsubst, varval, sizeof(varval) - 1);
		if (tris_strlen_zero(varval2))
			fieldcount = 0;
		else {
			while (strsep(&varval2, delim))
				fieldcount++;
		}
	} else {
		fieldcount = 1;
	}
	snprintf(buf, len, "%d", fieldcount);

	return 0;
}

static struct tris_custom_function fieldqty_function = {
	.name = "FIELDQTY",
	.read = function_fieldqty,
};

static int listfilter(struct tris_channel *chan, const char *cmd, char *parse, char *buf, size_t len)
{
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(listname);
		TRIS_APP_ARG(delimiter);
		TRIS_APP_ARG(fieldvalue);
	);
	const char *orig_list, *ptr;
	const char *begin, *cur, *next;
	int dlen, flen, first = 1;
	struct tris_str *result = tris_str_thread_get(&result_buf, 16);
	char *delim;

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (args.argc < 3) {
		tris_log(LOG_ERROR, "Usage: LISTFILTER(<listname>,<delimiter>,<fieldvalue>)\n");
		return -1;
	}

	/* If we don't lock the channel, the variable could disappear out from underneath us. */
	if (chan) {
		tris_channel_lock(chan);
	}
	if (!(orig_list = pbx_builtin_getvar_helper(chan, args.listname))) {
		tris_log(LOG_ERROR, "List variable '%s' not found\n", args.listname);
		if (chan) {
			tris_channel_unlock(chan);
		}
		return -1;
	}

	/* If the string isn't there, just copy out the string and be done with it. */
	if (!(ptr = strstr(orig_list, args.fieldvalue))) {
		tris_copy_string(buf, orig_list, len);
		if (chan) {
			tris_channel_unlock(chan);
		}
		return 0;
	}

	dlen = strlen(args.delimiter);
	delim = alloca(dlen + 1);
	tris_get_encoded_str(args.delimiter, delim, dlen + 1);

	if ((dlen = strlen(delim)) == 0) {
		delim = ",";
		dlen = 1;
	}

	flen = strlen(args.fieldvalue);

	tris_str_reset(result);
	/* Enough space for any result */
	tris_str_make_space(&result, strlen(orig_list) + 1);

	begin = orig_list;
	next = strstr(begin, delim);

	do {
		/* Find next boundary */
		if (next) {
			cur = next;
			next = strstr(cur + dlen, delim);
		} else {
			cur = strchr(begin + dlen, '\0');
		}

		if (flen == cur - begin && !strncmp(begin, args.fieldvalue, flen)) {
			/* Skip field */
			begin += flen + dlen;
		} else {
			/* Copy field to output */
			if (!first) {
				tris_str_append(&result, 0, "%s", delim);
			}

			tris_str_append_substr(&result, 0, begin, cur - begin + 1);
			first = 0;
			begin = cur + dlen;
		}
	} while (*cur != '\0');
	if (chan) {
		tris_channel_unlock(chan);
	}

	tris_copy_string(buf, tris_str_buffer(result), len);

	return 0;
}

static struct tris_custom_function listfilter_function = {
	.name = "LISTFILTER",
	.read = listfilter,
};

static int filter(struct tris_channel *chan, const char *cmd, char *parse, char *buf,
		  size_t len)
{
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(allowed);
			     TRIS_APP_ARG(string);
	);
	char *outbuf = buf;
	unsigned char ac;
	char allowed[256] = "";
	size_t allowedlen = 0;
	int32_t bitfield[8] = { 0, }; /* 256 bits */

	TRIS_STANDARD_RAW_ARGS(args, parse);

	if (!args.string) {
		tris_log(LOG_ERROR, "Usage: FILTER(<allowed-chars>,<string>)\n");
		return -1;
	}

	if (args.allowed[0] == '"' && !tris_opt_dont_warn) {
		tris_log(LOG_WARNING, "FILTER allowed characters includes the quote (\") character.  This may not be what you want.\n");
	}

	/* Expand ranges */
	for (; *(args.allowed);) {
		char c1 = 0, c2 = 0;
		size_t consumed = 0;

		if (tris_get_encoded_char(args.allowed, &c1, &consumed))
			return -1;
		args.allowed += consumed;

		if (*(args.allowed) == '-') {
			if (tris_get_encoded_char(args.allowed + 1, &c2, &consumed))
				c2 = -1;
			args.allowed += consumed + 1;

			if ((c2 < c1 || c2 == -1) && !tris_opt_dont_warn) {
				tris_log(LOG_WARNING, "Range wrapping in FILTER(%s,%s).  This may not be what you want.\n", parse, args.string);
			}

			/*!\note
			 * Looks a little strange, until you realize that we can overflow
			 * the size of a char.
			 */
			for (ac = c1; ac != c2; ac++) {
				bitfield[ac / 32] |= 1 << (ac % 32);
			}
			bitfield[ac / 32] |= 1 << (ac % 32);

			tris_debug(4, "c1=%d, c2=%d\n", c1, c2);
		} else {
			ac = (unsigned char) c1;
			tris_debug(4, "c1=%d, consumed=%d, args.allowed=%s\n", c1, (int) consumed, args.allowed - consumed);
			bitfield[ac / 32] |= 1 << (ac % 32);
		}
	}

	for (ac = 1; ac != 0; ac++) {
		if (bitfield[ac / 32] & (1 << (ac % 32))) {
			allowed[allowedlen++] = ac;
		}
	}

	tris_debug(1, "Allowed: %s\n", allowed);

	for (; *(args.string) && (buf + len - 1 > outbuf); (args.string)++) {
		if (strchr(allowed, *(args.string)))
			*outbuf++ = *(args.string);
	}
	*outbuf = '\0';

	return 0;
}

static struct tris_custom_function filter_function = {
	.name = "FILTER",
	.read = filter,
};

static int regex(struct tris_channel *chan, const char *cmd, char *parse, char *buf,
		 size_t len)
{
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(null);
			     TRIS_APP_ARG(reg);
			     TRIS_APP_ARG(str);
	);
	int errcode;
	regex_t regexbuf;

	buf[0] = '\0';

	TRIS_NONSTANDARD_APP_ARGS(args, parse, '"');

	if (args.argc != 3) {
		tris_log(LOG_ERROR, "Unexpected arguments: should have been in the form '\"<regex>\" <string>'\n");
		return -1;
	}
	if ((*args.str == ' ') || (*args.str == '\t'))
		args.str++;

	tris_debug(1, "FUNCTION REGEX (%s)(%s)\n", args.reg, args.str);

	if ((errcode = regcomp(&regexbuf, args.reg, REG_EXTENDED | REG_NOSUB))) {
		regerror(errcode, &regexbuf, buf, len);
		tris_log(LOG_WARNING, "Malformed input %s(%s): %s\n", cmd, parse, buf);
		return -1;
	}
	
	strcpy(buf, regexec(&regexbuf, args.str, 0, NULL, 0) ? "0" : "1");

	regfree(&regexbuf);

	return 0;
}

static struct tris_custom_function regex_function = {
	.name = "REGEX",
	.read = regex,
};

#define HASH_PREFIX	"~HASH~%s~"
#define HASH_FORMAT	HASH_PREFIX "%s~"

static char *app_clearhash = "ClearHash";

/* This function probably should migrate to main/pbx.c, as pbx_builtin_clearvar_prefix() */
static void clearvar_prefix(struct tris_channel *chan, const char *prefix)
{
	struct tris_var_t *var;
	int len = strlen(prefix);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&chan->varshead, var, entries) {
		if (strncasecmp(prefix, tris_var_name(var), len) == 0) {
			TRIS_LIST_REMOVE_CURRENT(entries);
			tris_free(var);
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END
}

static int exec_clearhash(struct tris_channel *chan, void *data)
{
	char prefix[80];
	snprintf(prefix, sizeof(prefix), HASH_PREFIX, data ? (char *)data : "null");
	clearvar_prefix(chan, prefix);
	return 0;
}

static int array(struct tris_channel *chan, const char *cmd, char *var,
		 const char *value)
{
	TRIS_DECLARE_APP_ARGS(arg1,
			     TRIS_APP_ARG(var)[100];
	);
	TRIS_DECLARE_APP_ARGS(arg2,
			     TRIS_APP_ARG(val)[100];
	);
	char *origvar = "", *value2, varname[256];
	int i, ishash = 0;

	value2 = tris_strdupa(value);
	if (!var || !value2)
		return -1;

	if (!strcmp(cmd, "HASH")) {
		const char *var2 = pbx_builtin_getvar_helper(chan, "~ODBCFIELDS~");
		origvar = var;
		if (var2)
			var = tris_strdupa(var2);
		else {
			if (chan)
				tris_autoservice_stop(chan);
			return -1;
		}
		ishash = 1;
	}

	/* The functions this will generally be used with are SORT and ODBC_*, which
	 * both return comma-delimited lists.  However, if somebody uses literal lists,
	 * their commas will be translated to vertical bars by the load, and I don't
	 * want them to be surprised by the result.  Hence, we prefer commas as the
	 * delimiter, but we'll fall back to vertical bars if commas aren't found.
	 */
	tris_debug(1, "array (%s=%s)\n", var, value2);
	TRIS_STANDARD_APP_ARGS(arg1, var);

	TRIS_STANDARD_APP_ARGS(arg2, value2);

	for (i = 0; i < arg1.argc; i++) {
		tris_debug(1, "array set value (%s=%s)\n", arg1.var[i],
				arg2.val[i]);
		if (i < arg2.argc) {
			if (ishash) {
				snprintf(varname, sizeof(varname), HASH_FORMAT, origvar, arg1.var[i]);
				pbx_builtin_setvar_helper(chan, varname, arg2.val[i]);
			} else {
				pbx_builtin_setvar_helper(chan, arg1.var[i], arg2.val[i]);
			}
		} else {
			/* We could unset the variable, by passing a NULL, but due to
			 * pushvar semantics, that could create some undesired behavior. */
			if (ishash) {
				snprintf(varname, sizeof(varname), HASH_FORMAT, origvar, arg1.var[i]);
				pbx_builtin_setvar_helper(chan, varname, "");
			} else {
				pbx_builtin_setvar_helper(chan, arg1.var[i], "");
			}
		}
	}

	return 0;
}

static int hashkeys_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_var_t *newvar;
	int plen;
	char prefix[80];
	snprintf(prefix, sizeof(prefix), HASH_PREFIX, data);
	plen = strlen(prefix);

	memset(buf, 0, len);
	TRIS_LIST_TRAVERSE(&chan->varshead, newvar, entries) {
		if (strncasecmp(prefix, tris_var_name(newvar), plen) == 0) {
			/* Copy everything after the prefix */
			strncat(buf, tris_var_name(newvar) + plen, len - strlen(buf) - 1);
			/* Trim the trailing ~ */
			buf[strlen(buf) - 1] = ',';
		}
	}
	/* Trim the trailing comma */
	buf[strlen(buf) - 1] = '\0';
	return 0;
}

static int hash_write(struct tris_channel *chan, const char *cmd, char *var, const char *value)
{
	char varname[256];
	TRIS_DECLARE_APP_ARGS(arg,
		TRIS_APP_ARG(hashname);
		TRIS_APP_ARG(hashkey);
	);

	if (!strchr(var, ',')) {
		/* Single argument version */
		return array(chan, "HASH", var, value);
	}

	TRIS_STANDARD_APP_ARGS(arg, var);
	snprintf(varname, sizeof(varname), HASH_FORMAT, arg.hashname, arg.hashkey);
	pbx_builtin_setvar_helper(chan, varname, value);

	return 0;
}

static int hash_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char varname[256];
	const char *varvalue;
	TRIS_DECLARE_APP_ARGS(arg,
		TRIS_APP_ARG(hashname);
		TRIS_APP_ARG(hashkey);
	);

	TRIS_STANDARD_APP_ARGS(arg, data);
	if (arg.argc == 2) {
		snprintf(varname, sizeof(varname), HASH_FORMAT, arg.hashname, arg.hashkey);
		varvalue = pbx_builtin_getvar_helper(chan, varname);
		if (varvalue)
			tris_copy_string(buf, varvalue, len);
		else
			*buf = '\0';
	} else if (arg.argc == 1) {
		char colnames[4096];
		int i;
		TRIS_DECLARE_APP_ARGS(arg2,
			TRIS_APP_ARG(col)[100];
		);

		/* Get column names, in no particular order */
		hashkeys_read(chan, "HASHKEYS", arg.hashname, colnames, sizeof(colnames));
		pbx_builtin_setvar_helper(chan, "~ODBCFIELDS~", colnames);

		TRIS_STANDARD_APP_ARGS(arg2, colnames);
		*buf = '\0';

		/* Now get the corresponding column values, in exactly the same order */
		for (i = 0; i < arg2.argc; i++) {
			snprintf(varname, sizeof(varname), HASH_FORMAT, arg.hashname, arg2.col[i]);
			varvalue = pbx_builtin_getvar_helper(chan, varname);
			strncat(buf, varvalue, len - strlen(buf) - 1);
			strncat(buf, ",", len - strlen(buf) - 1);
		}

		/* Strip trailing comma */
		buf[strlen(buf) - 1] = '\0';
	}

	return 0;
}

static struct tris_custom_function hash_function = {
	.name = "HASH",
	.write = hash_write,
	.read = hash_read,
};

static struct tris_custom_function hashkeys_function = {
	.name = "HASHKEYS",
	.read = hashkeys_read,
};

static struct tris_custom_function array_function = {
	.name = "ARRAY",
	.write = array,
};

static int quote(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *bufptr = buf, *dataptr = data;

	if (len < 3){ /* at least two for quotes and one for binary zero */
		tris_log(LOG_ERROR, "Not enough buffer");
		return -1;
	}

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "No argument specified!\n");
		tris_copy_string(buf, "\"\"", len);
		return 0;
	}

	*bufptr++ = '"';
	for (; bufptr < buf + len - 3; dataptr++) {
		if (*dataptr == '\\') {
			*bufptr++ = '\\';
			*bufptr++ = '\\';
		} else if (*dataptr == '"') {
			*bufptr++ = '\\';
			*bufptr++ = '"';
		} else if (*dataptr == '\0') {
			break;
		} else {
			*bufptr++ = *dataptr;
		}
	}
	*bufptr++ = '"';
	*bufptr = '\0';
	return 0;
}

static struct tris_custom_function quote_function = {
	.name = "QUOTE",
	.read = quote,
};

static int csv_quote(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *bufptr = buf, *dataptr = data;

	if (len < 3){ /* at least two for quotes and one for binary zero */
		tris_log(LOG_ERROR, "Not enough buffer");
		return -1;
	}

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "No argument specified!\n");
		tris_copy_string(buf,"\"\"",len);
		return 0;
	}

	*bufptr++ = '"';
	for (; bufptr < buf + len - 3; dataptr++){
		if (*dataptr == '"') {
			*bufptr++ = '"';
			*bufptr++ = '"';
		} else if (*dataptr == '\0') {
			break;
		} else {
			*bufptr++ = *dataptr;
		}
	}
	*bufptr++ = '"';
	*bufptr='\0';
	return 0;
}

static struct tris_custom_function csv_quote_function = {
	.name = "CSV_QUOTE",
	.read = csv_quote,
};

static int len(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t buflen)
{
	int length = 0;

	if (data)
		length = strlen(data);

	snprintf(buf, buflen, "%d", length);

	return 0;
}

static struct tris_custom_function len_function = {
	.name = "LEN",
	.read = len,
};

static int acf_strftime(struct tris_channel *chan, const char *cmd, char *parse,
			char *buf, size_t buflen)
{
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(epoch);
			     TRIS_APP_ARG(timezone);
			     TRIS_APP_ARG(format);
	);
	struct timeval when;
	struct tris_tm tm;

	buf[0] = '\0';

	TRIS_STANDARD_APP_ARGS(args, parse);

	tris_get_timeval(args.epoch, &when, tris_tvnow(), NULL);
	tris_localtime(&when, &tm, args.timezone);

	if (!args.format)
		args.format = "%c";

	if (tris_strftime(buf, buflen, args.format, &tm) <= 0)
		tris_log(LOG_WARNING, "C function strftime() output nothing?!!\n");

	buf[buflen - 1] = '\0';

	return 0;
}

static struct tris_custom_function strftime_function = {
	.name = "STRFTIME",
	.read = acf_strftime,
};

static int acf_strptime(struct tris_channel *chan, const char *cmd, char *data,
			char *buf, size_t buflen)
{
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(timestring);
			     TRIS_APP_ARG(timezone);
			     TRIS_APP_ARG(format);
	);
	struct tris_tm tm;

	buf[0] = '\0';

	if (!data) {
		tris_log(LOG_ERROR,
				"Trismedia function STRPTIME() requires an argument.\n");
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, data);

	if (tris_strlen_zero(args.format)) {
		tris_log(LOG_ERROR,
				"No format supplied to STRPTIME(<timestring>,<timezone>,<format>)");
		return -1;
	}

	if (!tris_strptime(args.timestring, args.format, &tm)) {
		tris_log(LOG_WARNING, "STRPTIME() found no time specified within the string\n");
	} else {
		struct timeval when;
		when = tris_mktime(&tm, args.timezone);
		snprintf(buf, buflen, "%d", (int) when.tv_sec);
	}

	return 0;
}

static struct tris_custom_function strptime_function = {
	.name = "STRPTIME",
	.read = acf_strptime,
};

static int function_eval(struct tris_channel *chan, const char *cmd, char *data,
			 char *buf, size_t buflen)
{
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "EVAL requires an argument: EVAL(<string>)\n");
		return -1;
	}

	pbx_substitute_variables_helper(chan, data, buf, buflen - 1);

	return 0;
}

static struct tris_custom_function eval_function = {
	.name = "EVAL",
	.read = function_eval,
};

static int keypadhash(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t buflen)
{
	char *bufptr, *dataptr;

	for (bufptr = buf, dataptr = data; bufptr < buf + buflen - 1; dataptr++) {
		if (*dataptr == '\0') {
			*bufptr++ = '\0';
			break;
		} else if (*dataptr == '1') {
			*bufptr++ = '1';
		} else if (strchr("AaBbCc2", *dataptr)) {
			*bufptr++ = '2';
		} else if (strchr("DdEeFf3", *dataptr)) {
			*bufptr++ = '3';
		} else if (strchr("GgHhIi4", *dataptr)) {
			*bufptr++ = '4';
		} else if (strchr("JjKkLl5", *dataptr)) {
			*bufptr++ = '5';
		} else if (strchr("MmNnOo6", *dataptr)) {
			*bufptr++ = '6';
		} else if (strchr("PpQqRrSs7", *dataptr)) {
			*bufptr++ = '7';
		} else if (strchr("TtUuVv8", *dataptr)) {
			*bufptr++ = '8';
		} else if (strchr("WwXxYyZz9", *dataptr)) {
			*bufptr++ = '9';
		} else if (*dataptr == '0') {
			*bufptr++ = '0';
		}
	}
	buf[buflen - 1] = '\0';

	return 0;
}

static struct tris_custom_function keypadhash_function = {
	.name = "KEYPADHASH",
	.read = keypadhash,
};

static int string_toupper(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t buflen)
{
	char *bufptr = buf, *dataptr = data;

	while ((bufptr < buf + buflen - 1) && (*bufptr++ = toupper(*dataptr++)));

	return 0;
}

static struct tris_custom_function toupper_function = {
	.name = "TOUPPER",
	.read = string_toupper,
};

static int string_tolower(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t buflen)
{
	char *bufptr = buf, *dataptr = data;

	while ((bufptr < buf + buflen - 1) && (*bufptr++ = tolower(*dataptr++)));

	return 0;
}

static struct tris_custom_function tolower_function = {
	.name = "TOLOWER",
	.read = string_tolower,
};

static int unload_module(void)
{
	int res = 0;

	res |= tris_custom_function_unregister(&fieldqty_function);
	res |= tris_custom_function_unregister(&filter_function);
	res |= tris_custom_function_unregister(&listfilter_function);
	res |= tris_custom_function_unregister(&regex_function);
	res |= tris_custom_function_unregister(&array_function);
	res |= tris_custom_function_unregister(&quote_function);
	res |= tris_custom_function_unregister(&csv_quote_function);
	res |= tris_custom_function_unregister(&len_function);
	res |= tris_custom_function_unregister(&strftime_function);
	res |= tris_custom_function_unregister(&strptime_function);
	res |= tris_custom_function_unregister(&eval_function);
	res |= tris_custom_function_unregister(&keypadhash_function);
	res |= tris_custom_function_unregister(&hashkeys_function);
	res |= tris_custom_function_unregister(&hash_function);
	res |= tris_unregister_application(app_clearhash);
	res |= tris_custom_function_unregister(&toupper_function);
	res |= tris_custom_function_unregister(&tolower_function);

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= tris_custom_function_register(&fieldqty_function);
	res |= tris_custom_function_register(&filter_function);
	res |= tris_custom_function_register(&listfilter_function);
	res |= tris_custom_function_register(&regex_function);
	res |= tris_custom_function_register(&array_function);
	res |= tris_custom_function_register(&quote_function);
	res |= tris_custom_function_register(&csv_quote_function);
	res |= tris_custom_function_register(&len_function);
	res |= tris_custom_function_register(&strftime_function);
	res |= tris_custom_function_register(&strptime_function);
	res |= tris_custom_function_register(&eval_function);
	res |= tris_custom_function_register(&keypadhash_function);
	res |= tris_custom_function_register(&hashkeys_function);
	res |= tris_custom_function_register(&hash_function);
	res |= tris_register_application_xml(app_clearhash, exec_clearhash);
	res |= tris_custom_function_register(&toupper_function);
	res |= tris_custom_function_register(&tolower_function);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "String handling dialplan functions");
