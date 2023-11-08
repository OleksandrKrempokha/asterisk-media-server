/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999-2006, Digium, Inc.
 *
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
 * \brief  Call Detail Record related dialplan functions
 *
 * \author Anthony Minessale II 
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 238234 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"
#include "trismedia/cdr.h"

/*** DOCUMENTATION
	<function name="CDR" language="en_US">
		<synopsis>
			Gets or sets a CDR variable.
		</synopsis>	
		<syntax>
			<parameter name="name" required="true">
				<para>CDR field name:</para>
				<enumlist>
					<enum name="clid">
						<para>Caller ID.</para>
					</enum>
					<enum name="lastdata">
						<para>Last application arguments.</para>
					</enum>
					<enum name="disposition">
						<para>ANSWERED, NO ANSWER, BUSY, FAILED.</para>
					</enum>
					<enum name="src">
						<para>Source.</para>
					</enum>
					<enum name="start">
						<para>Time the call started.</para>
					</enum>
					<enum name="amaflags">
						<para>DOCUMENTATION, BILL, IGNORE, etc.</para>
					</enum>
					<enum name="dst">
						<para>Destination.</para>
					</enum>
					<enum name="answer">
						<para>Time the call was answered.</para>
					</enum>
					<enum name="accountcode">
						<para>The channel's account code.</para>
					</enum>
					<enum name="dcontext">
						<para>Destination context.</para>
					</enum>
					<enum name="end">
						<para>Time the call ended.</para>
					</enum>
					<enum name="uniqueid">
						<para>The channel's unique id.</para>
					</enum>
					<enum name="dstchannel">
						<para>Destination channel.</para>
					</enum>
					<enum name="duration">
						<para>Duration of the call.</para>
					</enum>
					<enum name="userfield">
						<para>The channel's user specified field.</para>
					</enum>
					<enum name="lastapp">
						<para>Last application.</para>
					</enum>
					<enum name="billsec">
						<para>Duration of the call once it was answered.</para>
					</enum>
					<enum name="channel">
						<para>Channel name.</para>
					</enum>
				</enumlist>
			</parameter>
			<parameter name="options" required="false">
				<optionlist>
					<option name="l">
						<para>Uses the most recent CDR on a channel with multiple records</para>
					</option>
					<option name="r">
						<para>Searches the entire stack of CDRs on the channel.</para>
					</option>
					<option name="s">
						<para>Skips any CDR's that are marked 'LOCKED' due to forkCDR() calls.
						(on setting/writing CDR vars only)</para>
					</option>
					<option name="u">
						<para>Retrieves the raw, unprocessed value.</para>
						<para>For example, 'start', 'answer', and 'end' will be retrieved as epoch
						values, when the <literal>u</literal> option is passed, but formatted as YYYY-MM-DD HH:MM:SS
						otherwise.  Similarly, disposition and amaflags will return their raw
						integral values.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>All of the CDR field names are read-only, except for <literal>accountcode</literal>,
			<literal>userfield</literal>, and <literal>amaflags</literal>. You may, however, supply
			a name not on the above list, and create your own variable, whose value can be changed
			with this function, and this variable will be stored on the cdr.</para>
			<note><para>For setting CDR values, the <literal>l</literal> flag does not apply to
			setting the <literal>accountcode</literal>, <literal>userfield</literal>, or
			<literal>amaflags</literal>.</para></note>
			<para>Raw values for <literal>disposition</literal>:</para>
			<enumlist>
				<enum name="0">
					<para>NO ANSWER</para>
				</enum>
				<enum name="1">
					<para>NO ANSWER (NULL record)</para>
				</enum>
				<enum name="2">
					<para>FAILED</para>
				</enum>
				<enum name="4">
					<para>BUSY</para>
				</enum>
				<enum name="8">
					<para>ANSWERED</para>
				</enum>
			</enumlist>
			<para>Raw values for <literal>amaflags</literal>:</para>
			<enumlist>
				<enum name="1">
					<para>OMIT</para>
				</enum>
				<enum name="2">
					<para>BILLING</para>
				</enum>
				<enum name="3">
					<para>DOCUMENTATION</para>
				</enum>
			</enumlist>
			<para>Example: exten => 1,1,Set(CDR(userfield)=test)</para>
		</description>
	</function>
 ***/

enum {
	OPT_RECURSIVE = (1 << 0),
	OPT_UNPARSED = (1 << 1),
	OPT_LAST = (1 << 2),
	OPT_SKIPLOCKED = (1 << 3),
} cdr_option_flags;

TRIS_APP_OPTIONS(cdr_func_options, {
	TRIS_APP_OPTION('l', OPT_LAST),
	TRIS_APP_OPTION('r', OPT_RECURSIVE),
	TRIS_APP_OPTION('s', OPT_SKIPLOCKED),
	TRIS_APP_OPTION('u', OPT_UNPARSED),
});

static int cdr_read(struct tris_channel *chan, const char *cmd, char *parse,
		    char *buf, size_t len)
{
	char *ret;
	struct tris_flags flags = { 0 };
	struct tris_cdr *cdr = chan ? chan->cdr : NULL;
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(variable);
			     TRIS_APP_ARG(options);
	);

	if (tris_strlen_zero(parse))
		return -1;

	if (!cdr)
		return -1;

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (!tris_strlen_zero(args.options))
		tris_app_parse_options(cdr_func_options, &flags, NULL, args.options);

	if (tris_test_flag(&flags, OPT_LAST))
		while (cdr->next)
			cdr = cdr->next;

	if (tris_test_flag(&flags, OPT_SKIPLOCKED))
		while (tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED) && cdr->next)
			cdr = cdr->next;

	tris_cdr_getvar(cdr, args.variable, &ret, buf, len,
		       tris_test_flag(&flags, OPT_RECURSIVE),
			   tris_test_flag(&flags, OPT_UNPARSED));

	return ret ? 0 : -1;
}

static int cdr_write(struct tris_channel *chan, const char *cmd, char *parse,
		     const char *value)
{
	struct tris_cdr *cdr = chan ? chan->cdr : NULL;
	struct tris_flags flags = { 0 };
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(variable);
			     TRIS_APP_ARG(options);
	);

	if (tris_strlen_zero(parse) || !value || !chan)
		return -1;

	if (!cdr)
		return -1;

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (!tris_strlen_zero(args.options))
		tris_app_parse_options(cdr_func_options, &flags, NULL, args.options);

	if (tris_test_flag(&flags, OPT_LAST))
		while (cdr->next)
			cdr = cdr->next;

	if (!strcasecmp(args.variable, "accountcode"))  /* the 'l' flag doesn't apply to setting the accountcode, userfield, or amaflags */
		tris_cdr_setaccount(chan, value);
	else if (!strcasecmp(args.variable, "userfield"))
		tris_cdr_setuserfield(chan, value);
	else if (!strcasecmp(args.variable, "amaflags"))
		tris_cdr_setamaflags(chan, value);
	else
		tris_cdr_setvar(cdr, args.variable, value, tris_test_flag(&flags, OPT_RECURSIVE));
		/* No need to worry about the u flag, as all fields for which setting
		 * 'u' would do anything are marked as readonly. */

	return 0;
}

static struct tris_custom_function cdr_function = {
	.name = "CDR",
	.read = cdr_read,
	.write = cdr_write,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&cdr_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&cdr_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Call Detail Record (CDR) dialplan function");
