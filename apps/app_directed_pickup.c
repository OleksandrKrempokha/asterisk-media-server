/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Joshua Colp
 *
 * Joshua Colp <jcolp@digium.com>
 *
 * Portions merged from app_pickupchan, which was
 * Copyright (C) 2008, Gary Cook
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
 * \brief Directed Call Pickup Support
 *
 * \author Joshua Colp <jcolp@digium.com>
 * \author Gary Cook
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 218238 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"
#include "trismedia/features.h"

#define PICKUPMARK "PICKUPMARK"

/*** DOCUMENTATION
	<application name="Pickup" language="en_US">
		<synopsis>
			Directed extension call pickup.
		</synopsis>
		<syntax argsep="&amp;">
			<parameter name="ext" argsep="@" required="true">
				<argument name="extension" required="true"/>
				<argument name="context" />
			</parameter>
			<parameter name="ext2" argsep="@" multiple="true">
				<argument name="extension2" required="true"/>
				<argument name="context2"/>
			</parameter>
		</syntax>
		<description>
			<para>This application can pickup any ringing channel that is calling
			the specified <replaceable>extension</replaceable>. If no <replaceable>context</replaceable>
			is specified, the current context will be used. If you use the special string <literal>PICKUPMARK</literal>
			for the context parameter, for example 10@PICKUPMARK, this application
			tries to find a channel which has defined a <variable>PICKUPMARK</variable>
			channel variable with the same value as <replaceable>extension</replaceable>
			(in this example, <literal>10</literal>). When no parameter is specified, the application
			will pickup a channel matching the pickup group of the active channel.</para>
		</description>
	</application>
	<application name="PickupChan" language="en_US">
		<synopsis>
			Pickup a ringing channel.
		</synopsis>
		<syntax>
			<parameter name="channel" required="true" />
			<parameter name="channel2" multiple="true" />
		</syntax>
		<description>
			<para>This will pickup a specified <replaceable>channel</replaceable> if ringing.</para>
		</description>
	</application>
 ***/

static const char *app = "Pickup";
static const char *app2 = "PickupChan";
/*! \todo This application should return a result code, like PICKUPRESULT */

/* Perform actual pickup between two channels */
static int pickup_do(struct tris_channel *chan, struct tris_channel *target)
{
	int res = 0;

	tris_debug(1, "Call pickup on '%s' by '%s'\n", target->name, chan->name);

	if ((res = tris_answer(chan))) {
		tris_log(LOG_WARNING, "Unable to answer '%s'\n", chan->name);
		return -1;
	}

	if ((res = tris_queue_control(chan, TRIS_CONTROL_ANSWER))) {
		tris_log(LOG_WARNING, "Unable to queue answer on '%s'\n", chan->name);
		return -1;
	}

	if ((res = tris_channel_masquerade(target, chan))) {
		tris_log(LOG_WARNING, "Unable to masquerade '%s' into '%s'\n", chan->name, target->name);
		return -1;
	}

	return res;
}

/* Helper function that determines whether a channel is capable of being picked up */
static int can_pickup(struct tris_channel *chan)
{
	if (!chan->pbx && (chan->_state == TRIS_STATE_RINGING || chan->_state == TRIS_STATE_RING || chan->_state == TRIS_STATE_DOWN))
		return 1;
	else
		return 0;
}

/*! \brief Helper Function to walk through ALL channels checking NAME and STATE */
static struct tris_channel *my_tris_get_channel_by_name_locked(const char *channame)
{
	struct tris_channel *chan;
	char *chkchan;
	size_t channame_len, chkchan_len;

	channame_len = strlen(channame);
	chkchan_len = channame_len + 2;

 	chkchan = alloca(chkchan_len);

	/* need to append a '-' for the comparison so we check full channel name,
	 * i.e SIP/hgc- , use a temporary variable so original stays the same for
	 * debugging.
	 */
	strcpy(chkchan, channame);
	strcat(chkchan, "-");

	for (chan = tris_walk_channel_by_name_prefix_locked(NULL, channame, channame_len);
		 chan;
		 chan = tris_walk_channel_by_name_prefix_locked(chan, channame, channame_len)) {
		if (!strncasecmp(chan->name, chkchan, chkchan_len) && can_pickup(chan)) {
			return chan;
		}
		tris_channel_unlock(chan);
	}
	return NULL;
}

/*! \brief Attempt to pick up specified channel named , does not use context */
static int pickup_by_channel(struct tris_channel *chan, char *pickup)
{
	int res = 0;
	struct tris_channel *target;

	if (!(target = my_tris_get_channel_by_name_locked(pickup)))
		return -1;

	/* Just check that we are not picking up the SAME as target */
	if (chan->name != target->name && chan != target) {
		res = pickup_do(chan, target);
	}
	tris_channel_unlock(target);

	return res;
}

struct pickup_criteria {
	const char *exten;
	const char *context;
	struct tris_channel *chan;
};

static int find_by_exten(struct tris_channel *c, void *data)
{
	struct pickup_criteria *info = data;

	return (!strcasecmp(c->macroexten, info->exten) || !strcasecmp(c->exten, info->exten)) &&
		!strcasecmp(c->dialcontext, info->context) &&
		(info->chan != c) && can_pickup(c);
}

/* Attempt to pick up specified extension with context */
static int pickup_by_exten(struct tris_channel *chan, const char *exten, const char *context)
{
	struct tris_channel *target = NULL;
	struct pickup_criteria search = {
		.exten = exten,
		.context = context,
		.chan = chan,
	};

	target = tris_channel_search_locked(find_by_exten, &search);

	if (target) {
		int res = pickup_do(chan, target);
		tris_channel_unlock(target);
		target = NULL;
		return res;
	}

	return -1;
}

static int find_by_mark(struct tris_channel *c, void *data)
{
	const char *mark = data;
	const char *tmp;

	return (tmp = pbx_builtin_getvar_helper(c, PICKUPMARK)) &&
		!strcasecmp(tmp, mark) &&
		can_pickup(c);
}

/* Attempt to pick up specified mark */
static int pickup_by_mark(struct tris_channel *chan, const char *mark)
{
	struct tris_channel *target = tris_channel_search_locked(find_by_mark, (char *) mark);

	if (target) {
		int res = pickup_do(chan, target);
		tris_channel_unlock(target);
		target = NULL;
		return res;
	}

	return -1;
}

/* application entry point for Pickup() */
static int pickup_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char *tmp = tris_strdupa(data);
	char *exten = NULL, *context = NULL;

	if (tris_strlen_zero(data)) {
		res = tris_pickup_call(chan);
		return res;
	}
	
	/* Parse extension (and context if there) */
	while (!tris_strlen_zero(tmp) && (exten = strsep(&tmp, "&"))) {
		if ((context = strchr(exten, '@')))
			*context++ = '\0';
		if (!tris_strlen_zero(context) && !strcasecmp(context, PICKUPMARK)) {
			if (!pickup_by_mark(chan, exten))
				break;
		} else {
			if (!pickup_by_exten(chan, exten, !tris_strlen_zero(context) ? context : chan->context))
				break;
		}
		tris_log(LOG_NOTICE, "No target channel found for %s.\n", exten);
	}

	return res;
}

/* application entry point for PickupChan() */
static int pickupchan_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char *tmp = tris_strdupa(data);
	char *pickup = NULL;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "PickupChan requires an argument (channel)!\n");
		return -1;	
	}

	/* Parse channel */
	while (!tris_strlen_zero(tmp) && (pickup = strsep(&tmp, "&"))) {
		if (!strncasecmp(chan->name, pickup, strlen(pickup))) {
			tris_log(LOG_NOTICE, "Cannot pickup your own channel %s.\n", pickup);
		} else {
			if (!pickup_by_channel(chan, pickup)) {
				break;
			}
			tris_log(LOG_NOTICE, "No target channel found for %s.\n", pickup);
		}
	}

	return res;
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app);
	res |= tris_unregister_application(app2);

	return res;
}

static int load_module(void)
{
	int res;

	res = tris_register_application_xml(app, pickup_exec);
	res |= tris_register_application_xml(app2, pickupchan_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Directed Call Pickup Application");
