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
 * \brief Channel group related dialplan functions
 * 
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 232270 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="GROUP_COUNT" language="en_US">
		<synopsis>
			Counts the number of channels in the specified group.
		</synopsis>
		<syntax argsep="@">
			<parameter name="groupname">
				<para>Group name.</para>
			</parameter>
			<parameter name="category">
				<para>Category name</para>
			</parameter>
		</syntax>
		<description>
			<para>Calculates the group count for the specified group, or uses the
			channel's current group if not specifed (and non-empty).</para>
		</description>
	</function>
	<function name="GROUP_MATCH_COUNT" language="en_US">
		<synopsis>
			Counts the number of channels in the groups matching the specified pattern.
		</synopsis>
		<syntax argsep="@">
			<parameter name="groupmatch" required="true">
				<para>A standard regular expression used to match a group name.</para>
			</parameter>
			<parameter name="category">
				<para>Category name.</para>
			</parameter>
		</syntax>
		<description>
			<para>Calculates the group count for all groups that match the specified pattern.
			Uses standard regular expression matching (see regex(7)).</para>
		</description>
	</function>
	<function name="GROUP" language="en_US">
		<synopsis>
			Gets or sets the channel group.
		</synopsis>
		<syntax>
			<parameter name="category">
				<para>Category name.</para>
			</parameter>
		</syntax>
		<description>
			<para><replaceable>category</replaceable> can be employed for more fine grained group management. Each channel 
			can only be member of exactly one group per <replaceable>category</replaceable>.</para>
		</description>
	</function>
	<function name="GROUP_LIST" language="en_US">
		<synopsis>
			Gets a list of the groups set on a channel.
		</synopsis>
		<syntax />
		<description>
			<para>Gets a list of the groups set on a channel.</para>
		</description>
	</function>

 ***/

static int group_count_function_read(struct tris_channel *chan, const char *cmd,
				     char *data, char *buf, size_t len)
{
	int ret = -1;
	int count = -1;
	char group[80] = "", category[80] = "";

	tris_app_group_split_group(data, group, sizeof(group), category,
				  sizeof(category));

	/* If no group has been provided let's find one */
	if (tris_strlen_zero(group)) {
		struct tris_group_info *gi = NULL;

		tris_app_group_list_rdlock();
		for (gi = tris_app_group_list_head(); gi; gi = TRIS_LIST_NEXT(gi, group_list)) {
			if (gi->chan != chan)
				continue;
			if (tris_strlen_zero(category) || (!tris_strlen_zero(gi->category) && !strcasecmp(gi->category, category)))
				break;
		}
		if (gi) {
			tris_copy_string(group, gi->group, sizeof(group));
			if (!tris_strlen_zero(gi->category))
				tris_copy_string(category, gi->category, sizeof(category));
		}
		tris_app_group_list_unlock();
	}

	if ((count = tris_app_group_get_count(group, category)) == -1) {
		tris_log(LOG_NOTICE, "No group could be found for channel '%s'\n", chan->name);
	} else {
		snprintf(buf, len, "%d", count);
		ret = 0;
	}

	return ret;
}

static struct tris_custom_function group_count_function = {
	.name = "GROUP_COUNT",
	.read = group_count_function_read,
};

static int group_match_count_function_read(struct tris_channel *chan,
					   const char *cmd, char *data, char *buf,
					   size_t len)
{
	int count;
	char group[80] = "";
	char category[80] = "";

	tris_app_group_split_group(data, group, sizeof(group), category,
				  sizeof(category));

	if (!tris_strlen_zero(group)) {
		count = tris_app_group_match_get_count(group, category);
		snprintf(buf, len, "%d", count);
		return 0;
	}

	return -1;
}

static struct tris_custom_function group_match_count_function = {
	.name = "GROUP_MATCH_COUNT",
	.read = group_match_count_function_read,
	.write = NULL,
};

static int group_function_read(struct tris_channel *chan, const char *cmd,
			       char *data, char *buf, size_t len)
{
	int ret = -1;
	struct tris_group_info *gi = NULL;
	
	tris_app_group_list_rdlock();
	
	for (gi = tris_app_group_list_head(); gi; gi = TRIS_LIST_NEXT(gi, group_list)) {
		if (gi->chan != chan)
			continue;
		if (tris_strlen_zero(data))
			break;
		if (!tris_strlen_zero(gi->category) && !strcasecmp(gi->category, data))
			break;
	}
	
	if (gi) {
		tris_copy_string(buf, gi->group, len);
		ret = 0;
	}
	
	tris_app_group_list_unlock();
	
	return ret;
}

static int group_function_write(struct tris_channel *chan, const char *cmd,
				char *data, const char *value)
{
	char grpcat[256];

	if (!value) {
		return -1;
	}

	if (!tris_strlen_zero(data)) {
		snprintf(grpcat, sizeof(grpcat), "%s@%s", value, data);
	} else {
		tris_copy_string(grpcat, value, sizeof(grpcat));
	}

	if (tris_app_group_set_channel(chan, grpcat))
		tris_log(LOG_WARNING,
				"Setting a group requires an argument (group name)\n");

	return 0;
}

static struct tris_custom_function group_function = {
	.name = "GROUP",
	.read = group_function_read,
	.write = group_function_write,
};

static int group_list_function_read(struct tris_channel *chan, const char *cmd,
				    char *data, char *buf, size_t len)
{
	struct tris_group_info *gi = NULL;
	char tmp1[1024] = "";
	char tmp2[1024] = "";

	if (!chan)
		return -1;

	tris_app_group_list_rdlock();

	for (gi = tris_app_group_list_head(); gi; gi = TRIS_LIST_NEXT(gi, group_list)) {
		if (gi->chan != chan)
			continue;
		if (!tris_strlen_zero(tmp1)) {
			tris_copy_string(tmp2, tmp1, sizeof(tmp2));
			if (!tris_strlen_zero(gi->category))
				snprintf(tmp1, sizeof(tmp1), "%s %s@%s", tmp2, gi->group, gi->category);
			else
				snprintf(tmp1, sizeof(tmp1), "%s %s", tmp2, gi->group);
		} else {
			if (!tris_strlen_zero(gi->category))
				snprintf(tmp1, sizeof(tmp1), "%s@%s", gi->group, gi->category);
			else
				snprintf(tmp1, sizeof(tmp1), "%s", gi->group);
		}
	}
	
	tris_app_group_list_unlock();

	tris_copy_string(buf, tmp1, len);

	return 0;
}

static struct tris_custom_function group_list_function = {
	.name = "GROUP_LIST",
	.read = group_list_function_read,
	.write = NULL,
};

static int unload_module(void)
{
	int res = 0;

	res |= tris_custom_function_unregister(&group_count_function);
	res |= tris_custom_function_unregister(&group_match_count_function);
	res |= tris_custom_function_unregister(&group_list_function);
	res |= tris_custom_function_unregister(&group_function);

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= tris_custom_function_register(&group_count_function);
	res |= tris_custom_function_register(&group_match_count_function);
	res |= tris_custom_function_register(&group_list_function);
	res |= tris_custom_function_register(&group_function);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Channel group dialplan functions");
