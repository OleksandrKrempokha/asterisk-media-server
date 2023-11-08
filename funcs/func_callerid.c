/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999-2006, Digium, Inc.
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
 * \brief Caller ID related dialplan functions
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 220632 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"
#include "trismedia/callerid.h"

/*** DOCUMENTATION
	<function name="CALLERID" language="en_US">
		<synopsis>
			Gets or sets Caller*ID data on the channel.
		</synopsis>
		<syntax>
			<parameter name="datatype" required="true">
				<para>The allowable datatypes are:</para>
				<enumlist>
					<enum name="all" />
					<enum name="num" />
					<enum name="name" />
					<enum name="ANI" />
					<enum name="DNID" />
					<enum name="RDNIS" />
					<enum name="pres" />
					<enum name="ton" />
				</enumlist>
			</parameter>
			<parameter name="CID">
				<para>Optional Caller*ID</para>
			</parameter>
		</syntax>
		<description>
			<para>Gets or sets Caller*ID data on the channel. Uses channel callerid by default or optional
			callerid, if specified.</para>
		</description>
	</function>
	<function name="CALLERPRES" language="en_US">
		<synopsis>
			Gets or sets Caller*ID presentation on the channel.
		</synopsis>
		<syntax />
		<description>
			<para>Gets or sets Caller*ID presentation on the channel. The following values
			are valid:</para>
			<enumlist>
				<enum name="allowed_not_screened">
					<para>Presentation Allowed, Not Screened.</para>
				</enum>
				<enum name="allowed_passed_screen">
					<para>Presentation Allowed, Passed Screen.</para>
				</enum>
				<enum name="allowed_failed_screen">
					<para>Presentation Allowed, Failed Screen.</para>
				</enum>
				<enum name="allowed">
					<para>Presentation Allowed, Network Number.</para>
				</enum>
				<enum name="prohib_not_screened">
					<para>Presentation Prohibited, Not Screened.</para>
				</enum>
				<enum name="prohib_passed_screen">
					<para>Presentation Prohibited, Passed Screen.</para>
				</enum>
				<enum name="prohib_failed_screen">
					<para>Presentation Prohibited, Failed Screen.</para>
				</enum>
				<enum name="prohib">
					<para>Presentation Prohibited, Network Number.</para>
				</enum>
				<enum name="unavailable">
					<para>Number Unavailable.</para>
				</enum>
			</enumlist>
		</description>
	</function>
 ***/

static int callerpres_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	tris_copy_string(buf, tris_named_caller_presentation(chan->cid.cid_pres), len);
	return 0;
}

static int callerpres_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	int pres = tris_parse_caller_presentation(value);
	if (pres < 0)
		tris_log(LOG_WARNING, "'%s' is not a valid presentation (see 'show function CALLERPRES')\n", value);
	else
		chan->cid.cid_pres = pres;
	return 0;
}

static int callerid_read(struct tris_channel *chan, const char *cmd, char *data,
			 char *buf, size_t len)
{
	char *opt = data;

	/* Ensure that the buffer is empty */
	*buf = 0;

	if (!chan)
		return -1;

	if (strchr(opt, ',')) {
		char name[80], num[80];

		data = strsep(&opt, ",");
		tris_callerid_split(opt, name, sizeof(name), num, sizeof(num));

		if (!strncasecmp("all", data, 3)) {
			snprintf(buf, len, "\"%s\" <%s>", name, num);
		} else if (!strncasecmp("name", data, 4)) {
			tris_copy_string(buf, name, len);
		} else if (!strncasecmp("num", data, 3)) {
			/* also matches "number" */
			tris_copy_string(buf, num, len);
		} else {
			tris_log(LOG_ERROR, "Unknown callerid data type '%s'.\n", data);
		}
	} else {
		tris_channel_lock(chan);

		if (!strncasecmp("all", data, 3)) {
			snprintf(buf, len, "\"%s\" <%s>",
				S_OR(chan->cid.cid_name, ""),
				S_OR(chan->cid.cid_num, ""));
		} else if (!strncasecmp("name", data, 4)) {
			if (chan->cid.cid_name) {
				tris_copy_string(buf, chan->cid.cid_name, len);
			}
		} else if (!strncasecmp("num", data, 3)) {
			/* also matches "number" */
			if (chan->cid.cid_num) {
				tris_copy_string(buf, chan->cid.cid_num, len);
			}
		} else if (!strncasecmp("ani", data, 3)) {
			if (!strncasecmp(data + 3, "2", 1)) {
				snprintf(buf, len, "%d", chan->cid.cid_ani2);
			} else if (chan->cid.cid_ani) {
				tris_copy_string(buf, chan->cid.cid_ani, len);
			}
		} else if (!strncasecmp("dnid", data, 4)) {
			if (chan->cid.cid_dnid) {
				tris_copy_string(buf, chan->cid.cid_dnid, len);
			}
		} else if (!strncasecmp("rdnis", data, 5)) {
			if (chan->cid.cid_rdnis) {
				tris_copy_string(buf, chan->cid.cid_rdnis, len);
			}
		} else if (!strncasecmp("pres", data, 4)) {
			tris_copy_string(buf, tris_named_caller_presentation(chan->cid.cid_pres), len);
		} else if (!strncasecmp("ton", data, 3)) {
			snprintf(buf, len, "%d", chan->cid.cid_ton);
		} else {
			tris_log(LOG_ERROR, "Unknown callerid data type '%s'.\n", data);
		}

		tris_channel_unlock(chan);
	}

	return 0;
}

static int callerid_write(struct tris_channel *chan, const char *cmd, char *data,
			  const char *value)
{
	if (!value || !chan)
		return -1;

	value = tris_skip_blanks(value);

	if (!strncasecmp("all", data, 3)) {
		char name[256];
		char num[256];

		tris_callerid_split(value, name, sizeof(name), num, sizeof(num));
		tris_set_callerid(chan, num, name, num);
		if (chan->cdr) {
			tris_cdr_setcid(chan->cdr, chan);
		}
	} else if (!strncasecmp("name", data, 4)) {
		tris_set_callerid(chan, NULL, value, NULL);
		if (chan->cdr) {
			tris_cdr_setcid(chan->cdr, chan);
		}
	} else if (!strncasecmp("num", data, 3)) {
		/* also matches "number" */
		tris_set_callerid(chan, value, NULL, NULL);
		if (chan->cdr) {
			tris_cdr_setcid(chan->cdr, chan);
		}
	} else if (!strncasecmp("ani", data, 3)) {
		if (!strncasecmp(data + 3, "2", 1)) {
			chan->cid.cid_ani2 = atoi(value);
		} else {
			tris_set_callerid(chan, NULL, NULL, value);
		}
		if (chan->cdr) {
			tris_cdr_setcid(chan->cdr, chan);
		}
	} else if (!strncasecmp("dnid", data, 4)) {
		tris_channel_lock(chan);
		if (chan->cid.cid_dnid) {
			tris_free(chan->cid.cid_dnid);
		}
		chan->cid.cid_dnid = tris_strdup(value);
		if (chan->cdr) {
			tris_cdr_setcid(chan->cdr, chan);
		}
		tris_channel_unlock(chan);
	} else if (!strncasecmp("rdnis", data, 5)) {
		tris_channel_lock(chan);
		if (chan->cid.cid_rdnis) {
			tris_free(chan->cid.cid_rdnis);
		}
		chan->cid.cid_rdnis = tris_strdup(value);
		if (chan->cdr) {
			tris_cdr_setcid(chan->cdr, chan);
		}
		tris_channel_unlock(chan);
	} else if (!strncasecmp("pres", data, 4)) {
		int i;
		char *val;

		val = tris_strdupa(value);
		tris_trim_blanks(val);

		if ((val[0] >= '0') && (val[0] <= '9')) {
			i = atoi(val);
		} else {
			i = tris_parse_caller_presentation(val);
		}

		if (i < 0) {
			tris_log(LOG_ERROR, "Unknown calling number presentation '%s', value unchanged\n", val);
		} else {
			chan->cid.cid_pres = i;
		}
	} else if (!strncasecmp("ton", data, 3)) {
		chan->cid.cid_ton = atoi(value);
	} else {
		tris_log(LOG_ERROR, "Unknown callerid data type '%s'.\n", data);
	}

	return 0;
}

static struct tris_custom_function callerid_function = {
	.name = "CALLERID",
	.read = callerid_read,
	.write = callerid_write,
};

static struct tris_custom_function callerpres_function = {
	.name = "CALLERPRES",
	.read = callerpres_read,
	.write = callerpres_write,
};

static int unload_module(void)
{
	int res = tris_custom_function_unregister(&callerpres_function);
	res |= tris_custom_function_unregister(&callerid_function);
	return res;
}

static int load_module(void)
{
	int res = tris_custom_function_register(&callerpres_function);
	res |= tris_custom_function_register(&callerid_function);
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Caller ID related dialplan functions");
