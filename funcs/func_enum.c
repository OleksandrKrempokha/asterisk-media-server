/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006
 *
 * Mark Spencer <markster@digium.com>
 * Oleksiy Krivoshey <oleksiyk@gmail.com>
 * Russell Bryant <russelb@clemson.edu>
 * Brett Bryant <bbryant@digium.com>
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
 * \brief ENUM Functions
 *
 * \author Mark Spencer <markster@digium.com>
 * \author Oleksiy Krivoshey <oleksiyk@gmail.com>
 * \author Russell Bryant <russelb@clemson.edu>
 * \author Brett Bryant <bbryant@digium.com>
 *
 * \arg See also AstENUM
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/enum.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="ENUMQUERY" language="en_US">
		<synopsis>
			Initiate an ENUM query.
		</synopsis>
		<syntax>
			<parameter name="number" required="true" />
			<parameter name="method-type">
				<para>If no <replaceable>method-type</replaceable> is given, the default will be
				<literal>sip</literal>.</para>
			</parameter>
			<parameter name="zone-suffix">
				<para>If no <replaceable>zone-suffix</replaceable> is given, the default will be
				<literal>e164.arpa</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>This will do a ENUM lookup of the given phone number.</para>
		</description>
	</function>
	<function name="ENUMRESULT" language="en_US">
		<synopsis>
			Retrieve results from a ENUMQUERY.
		</synopsis>
		<syntax>
			<parameter name="id" required="true">
				<para>The identifier returned by the ENUMQUERY function.</para>
			</parameter>
			<parameter name="resultnum" required="true">
				<para>The number of the result that you want to retrieve.</para>
				<para>Results start at <literal>1</literal>. If this argument is specified
				as <literal>getnum</literal>, then it will return the total number of results 
				that are available.</para>
			</parameter>
		</syntax>
		<description>
			<para>This function will retrieve results from a previous use
			of the ENUMQUERY function.</para>
		</description>
	</function>	
	<function name="ENUMLOOKUP" language="en_US">
		<synopsis>
			General or specific querying of NAPTR records for ENUM or ENUM-like DNS pointers.
		</synopsis>
		<syntax>
			<parameter name="number" required="true" />
			<parameter name="method-type">
				<para>If no <replaceable>method-type</replaceable> is given, the default will be
                                <literal>sip</literal>.</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="c">
						<para>Returns an integer count of the number of NAPTRs of a certain RR type.</para>
						<para>Combination of <literal>c</literal> and Method-type of <literal>ALL</literal> will
						return a count of all NAPTRs for the record.</para>
					</option>
					<option name="u">
						<para>Returns the full URI and does not strip off the URI-scheme.</para>
					</option>
					<option name="s">
						<para>Triggers ISN specific rewriting.</para>
					</option>
					<option name="i">
						<para>Looks for branches into an Infrastructure ENUM tree.</para>
					</option>
					<option name="d">
						<para>for a direct DNS lookup without any flipping of digits.</para>
					</option>
				</optionlist>	
			</parameter>
			<parameter name="record#">
				<para>If no <replaceable>record#</replaceable> is given, 
				defaults to <literal>1</literal>.</para>
			</parameter>
			<parameter name="zone-suffix">
				<para>If no <replaceable>zone-suffix</replaceable> is given, the default will be
				<literal>e164.arpa</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>For more information see <filename>doc/trismedia.pdf</filename>.</para>
		</description>
	</function>
	<function name="TXTCIDNAME" language="en_US">
		<synopsis>
			TXTCIDNAME looks up a caller name via DNS.
		</synopsis>
		<syntax>
			<parameter name="number" required="true" />
			<parameter name="zone-suffix">
				<para>If no <replaceable>zone-suffix</replaceable> is given, the default will be
				<literal>e164.arpa</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>This function looks up the given phone number in DNS to retrieve
			the caller id name.  The result will either be blank or be the value
			found in the TXT record in DNS.</para>
		</description>
	</function>
 ***/

static char *synopsis = "Syntax: ENUMLOOKUP(number[,Method-type[,options[,record#[,zone-suffix]]]])\n";

static int function_enum(struct tris_channel *chan, const char *cmd, char *data,
			 char *buf, size_t len)
{
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(number);
		TRIS_APP_ARG(tech);
		TRIS_APP_ARG(options);
		TRIS_APP_ARG(record);
		TRIS_APP_ARG(zone);
	);
	int res = 0;
	char tech[80];
	char dest[256] = "", tmp[2] = "", num[TRIS_MAX_EXTENSION] = "";
	char *s, *p;
	unsigned int record = 1;

	buf[0] = '\0';

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "%s", synopsis);
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, data);

	if (args.argc < 1) {
		tris_log(LOG_WARNING, "%s", synopsis);
		return -1;
	}

	if (args.tech && !tris_strlen_zero(args.tech)) {
		tris_copy_string(tech,args.tech, sizeof(tech));
	} else {
		tris_copy_string(tech,"sip",sizeof(tech));
	}

	if (!args.zone) {
		args.zone = "e164.arpa";
	}
	if (!args.options) {
		args.options = "";
	}
	if (args.record) {
		record = atoi(args.record) ? atoi(args.record) : record;
	}

	/* strip any '-' signs from number */
	for (s = p = args.number; *s; s++) {
		if (*s != '-') {
			snprintf(tmp, sizeof(tmp), "%c", *s);
			strncat(num, tmp, sizeof(num) - strlen(num) - 1);
		}

	}
	res = tris_get_enum(chan, num, dest, sizeof(dest), tech, sizeof(tech), args.zone, args.options, record, NULL);

	p = strchr(dest, ':');
	if (p && strcasecmp(tech, "ALL") && !strchr(args.options, 'u')) {
		tris_copy_string(buf, p + 1, len);
	} else {
		tris_copy_string(buf, dest, len);
	}
	return 0;
}

unsigned int enum_datastore_id;

struct enum_result_datastore {
	struct enum_context *context;
	unsigned int id;
};

static void erds_destroy(struct enum_result_datastore *data) 
{
	int k;

	for (k = 0; k < data->context->naptr_rrs_count; k++) {
		tris_free(data->context->naptr_rrs[k].result);
		tris_free(data->context->naptr_rrs[k].tech);
	}

	tris_free(data->context->naptr_rrs);
	tris_free(data->context);
	tris_free(data);
}

static void erds_destroy_cb(void *data) 
{
	struct enum_result_datastore *erds = data;
	erds_destroy(erds);
}

const struct tris_datastore_info enum_result_datastore_info = {
	.type = "ENUMQUERY",
	.destroy = erds_destroy_cb,
}; 

static int enum_query_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct enum_result_datastore *erds;
	struct tris_datastore *datastore;
	char *parse, tech[128], dest[128];
	int res = -1;

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(number);
		TRIS_APP_ARG(tech);
		TRIS_APP_ARG(zone);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "ENUMQUERY requires at least a number as an argument...\n");
		goto finish;
	}

	parse = tris_strdupa(data);
    
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (!chan) {
		tris_log(LOG_ERROR, "ENUMQUERY cannot be used without a channel!\n");
		goto finish;
	}

	if (!args.zone)
		args.zone = "e164.zone";

	tris_copy_string(tech, args.tech ? args.tech : "sip", sizeof(tech));

	if (!(erds = tris_calloc(1, sizeof(*erds))))
		goto finish;

	if (!(erds->context = tris_calloc(1, sizeof(*erds->context)))) {
		tris_free(erds);
		goto finish;
	}

	erds->id = tris_atomic_fetchadd_int((int *) &enum_datastore_id, 1);

	snprintf(buf, len, "%u", erds->id);

	if (!(datastore = tris_datastore_alloc(&enum_result_datastore_info, buf))) {
		tris_free(erds->context);
		tris_free(erds);
		goto finish;
	}

	tris_get_enum(chan, args.number, dest, sizeof(dest), tech, sizeof(tech), args.zone, "", 1, &erds->context);

	datastore->data = erds;

	tris_channel_lock(chan);
	tris_channel_datastore_add(chan, datastore);
	tris_channel_unlock(chan);
   
	res = 0;
    
finish:

	return res;
}

static int enum_result_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct enum_result_datastore *erds;
	struct tris_datastore *datastore;
	char *parse, *p;
	unsigned int num;
	int res = -1, k;
	TRIS_DECLARE_APP_ARGS(args, 
		TRIS_APP_ARG(id);
		TRIS_APP_ARG(resultnum);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "ENUMRESULT requires two arguments (id and resultnum)\n");
		goto finish;
	}

	if (!chan) {
		tris_log(LOG_ERROR, "ENUMRESULT can not be used without a channel!\n");
		goto finish;
	}
   
	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (tris_strlen_zero(args.id)) {
		tris_log(LOG_ERROR, "A result ID must be provided to ENUMRESULT\n");
		goto finish;
	}

	if (tris_strlen_zero(args.resultnum)) {
		tris_log(LOG_ERROR, "A result number must be given to ENUMRESULT!\n");
		goto finish;
	}

	tris_channel_lock(chan);
	datastore = tris_channel_datastore_find(chan, &enum_result_datastore_info, args.id);
	tris_channel_unlock(chan);
	if (!datastore) {
		tris_log(LOG_WARNING, "No ENUM results found for query id!\n");
		goto finish;
	}

	erds = datastore->data;

	if (!strcasecmp(args.resultnum, "getnum")) {
		snprintf(buf, len, "%u", erds->context->naptr_rrs_count);
		res = 0;
		goto finish;
	}

	if (sscanf(args.resultnum, "%30u", &num) != 1) {
		tris_log(LOG_ERROR, "Invalid value '%s' for resultnum to ENUMRESULT!\n", args.resultnum);
		goto finish;
	}

	if (!num || num > erds->context->naptr_rrs_count) {
		tris_log(LOG_WARNING, "Result number %u is not valid for ENUM query results for ID %s!\n", num, args.id);
		goto finish;
	}

	for (k = 0; k < erds->context->naptr_rrs_count; k++) {
		if (num - 1 != erds->context->naptr_rrs[k].sort_pos)
			continue;

		p = strchr(erds->context->naptr_rrs[k].result, ':');
              
		if (p && strcasecmp(erds->context->naptr_rrs[k].tech, "ALL"))
			tris_copy_string(buf, p + 1, len);
		else
			tris_copy_string(buf, erds->context->naptr_rrs[k].result, len);

		break;
	}

	res = 0;

finish:

	return res;
}

static struct tris_custom_function enum_query_function = {
	.name = "ENUMQUERY",
	.read = enum_query_read,
};

static struct tris_custom_function enum_result_function = {
	.name = "ENUMRESULT",
	.read = enum_result_read,
};

static struct tris_custom_function enum_function = {
	.name = "ENUMLOOKUP",
	.read = function_enum,
};

static int function_txtcidname(struct tris_channel *chan, const char *cmd,
			       char *data, char *buf, size_t len)
{
	int res;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(number);
		TRIS_APP_ARG(zone);
	);

	buf[0] = '\0';

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Syntax: TXTCIDNAME(number[,zone-suffix])\n");
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, data);

	if (args.argc < 1) {
		tris_log(LOG_WARNING, "Syntax: TXTCIDNAME(number[,zone-suffix])\n");
		return -1;
	}

	if (!args.zone) {
		args.zone = "e164.arpa";
	}

	res = tris_get_txt(chan, args.number, buf, len, args.zone);

	return 0;
}

static struct tris_custom_function txtcidname_function = {
	.name = "TXTCIDNAME",
	.read = function_txtcidname,
};

static int unload_module(void)
{
	int res = 0;

	res |= tris_custom_function_unregister(&enum_result_function);
	res |= tris_custom_function_unregister(&enum_query_function);
	res |= tris_custom_function_unregister(&enum_function);
	res |= tris_custom_function_unregister(&txtcidname_function);

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= tris_custom_function_register(&enum_result_function);
	res |= tris_custom_function_register(&enum_query_function);
	res |= tris_custom_function_register(&enum_function);
	res |= tris_custom_function_register(&txtcidname_function);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "ENUM related dialplan functions");
