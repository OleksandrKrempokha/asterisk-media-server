/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
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
 * \brief Built in bridging features
 *
 * \author Joshua Colp <jcolp@digium.com>
 *
 * \ingroup bridges
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision$")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/bridging.h"
#include "trismedia/bridging_technology.h"
#include "trismedia/frame.h"
#include "trismedia/file.h"
#include "trismedia/app.h"
#include "trismedia/astobj2.h"

/*! \brief Helper function that presents dialtone and grabs extension */
static int grab_transfer(struct tris_channel *chan, char *exten, size_t exten_len, const char *context)
{
	int res;

	/* Play the simple "transfer" prompt out and wait */
	res = tris_stream_and_wait(chan, "pbx-transfer", TRIS_DIGIT_ANY);
	tris_stopstream(chan);

	/* If the person hit a DTMF digit while the above played back stick it into the buffer */
	if (res) {
		exten[0] = (char)res;
	}

	/* Drop to dialtone so they can enter the extension they want to transfer to */
	res = tris_app_dtget(chan, context, exten, exten_len, 100, 1000);

	return res;
}

/*! \brief Helper function that creates an outgoing channel and returns it immediately */
static struct tris_channel *dial_transfer(const struct tris_channel *caller, const char *exten, const char *context)
{
	char destination[TRIS_MAX_EXTENSION+TRIS_MAX_CONTEXT+1] = "";
	struct tris_channel *chan = NULL;
	int cause;

	/* Fill the variable with the extension and context we want to call */
	snprintf(destination, sizeof(destination), "%s@%s", exten, context);

	/* Now we request that chan_local prepare to call the destination */
	if (!(chan = tris_request("Local", caller->nativeformats, destination, &cause, 0))) {
		return NULL;
	}

	/* Before we actually dial out let's inherit the appropriate dialplan variables */
	tris_channel_inherit_variables(caller, chan);

	/* Since the above worked fine now we actually call it and return the channel */
	if (tris_call(chan, destination, 0)) {
		tris_hangup(chan);
		return NULL;
	}

	return chan;
}

/*! \brief Internal built in feature for blind transfers */
static int feature_blind_transfer(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, void *hook_pvt)
{
	char exten[TRIS_MAX_EXTENSION] = "";
	struct tris_channel *chan = NULL;
	struct tris_bridge_features_blind_transfer *blind_transfer = hook_pvt;
	const char *context = (blind_transfer && !tris_strlen_zero(blind_transfer->context) ? blind_transfer->context : bridge_channel->chan->context);

	/* Grab the extension to transfer to */
	if (!grab_transfer(bridge_channel->chan, exten, sizeof(exten), context)) {
		tris_stream_and_wait(bridge_channel->chan, "pbx-invalid", TRIS_DIGIT_ANY);
		tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_WAIT);
		return 0;
	}

	/* Get a channel that is the destination we wish to call */
	if (!(chan = dial_transfer(bridge_channel->chan, exten, context))) {
		tris_stream_and_wait(bridge_channel->chan, "beeperr", TRIS_DIGIT_ANY);
		tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_WAIT);
		return 0;
	}

	/* This is sort of the fun part. We impart the above channel onto the bridge, and have it take our place. */
	tris_bridge_impart(bridge, chan, bridge_channel->chan, NULL);

	return 0;
}

/*! \brief Attended transfer feature to turn it into a threeway call */
static int attended_threeway_transfer(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, void *hook_pvt)
{
	/* This is sort of abusing the depart state but in this instance it is only going to be handled in the below function so it is okay */
	tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_DEPART);
	return 0;
}

/*! \brief Attended transfer abort feature */
static int attended_abort_transfer(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, void *hook_pvt)
{
	struct tris_bridge_channel *called_bridge_channel = NULL;

	/* It is possible (albeit unlikely) that the bridge channels list may change, so we have to ensure we do all of our magic while locked */
	ao2_lock(bridge);

	if (TRIS_LIST_FIRST(&bridge->channels) != bridge_channel) {
		called_bridge_channel = TRIS_LIST_FIRST(&bridge->channels);
	} else {
		called_bridge_channel = TRIS_LIST_LAST(&bridge->channels);
	}

	/* Now we basically eject the other channel from the bridge. This will cause their thread to hang them up, and our own code to consider the transfer failed. */
	if (called_bridge_channel) {
		tris_bridge_change_state(called_bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_HANGUP);
	}

	tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_END);

	ao2_unlock(bridge);

	return 0;
}

/*! \brief Internal built in feature for attended transfers */
static int feature_attended_transfer(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, void *hook_pvt)
{
	char exten[TRIS_MAX_EXTENSION] = "";
	struct tris_channel *chan = NULL;
	struct tris_bridge *attended_bridge = NULL;
	struct tris_bridge_features caller_features, called_features;
	enum tris_bridge_channel_state attended_bridge_result;
	struct tris_bridge_features_attended_transfer *attended_transfer = hook_pvt;
	const char *context = (attended_transfer && !tris_strlen_zero(attended_transfer->context) ? attended_transfer->context : bridge_channel->chan->context);

	/* Grab the extension to transfer to */
	if (!grab_transfer(bridge_channel->chan, exten, sizeof(exten), context)) {
		tris_stream_and_wait(bridge_channel->chan, "pbx-invalid", TRIS_DIGIT_ANY);
		tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_WAIT);
		return 0;
	}

	/* Get a channel that is the destination we wish to call */
	if (!(chan = dial_transfer(bridge_channel->chan, exten, context))) {
		tris_stream_and_wait(bridge_channel->chan, "beeperr", TRIS_DIGIT_ANY);
		tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_WAIT);
		return 0;
	}

	/* Create a bridge to use to talk to the person we are calling */
	if (!(attended_bridge = tris_bridge_new(TRIS_BRIDGE_CAPABILITY_1TO1MIX, 0))) {
		tris_hangup(chan);
		tris_stream_and_wait(bridge_channel->chan, "beeperr", TRIS_DIGIT_ANY);
		tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_WAIT);
		return 0;
	}

	/* Setup our called features structure so that if they hang up we immediately get thrown out of the bridge */
	tris_bridge_features_init(&called_features);
	tris_bridge_features_set_flag(&called_features, TRIS_BRIDGE_FLAG_DISSOLVE);

	/* This is how this is going down, we are imparting the channel we called above into this bridge first */
	tris_bridge_impart(attended_bridge, chan, NULL, &called_features);

	/* Before we join setup a features structure with the hangup option, just in case they want to use DTMF */
	tris_bridge_features_init(&caller_features);
	tris_bridge_features_enable(&caller_features, TRIS_BRIDGE_BUILTIN_HANGUP,
				   (attended_transfer && !tris_strlen_zero(attended_transfer->complete) ? attended_transfer->complete : "*1"), NULL);
	tris_bridge_features_hook(&caller_features, (attended_transfer && !tris_strlen_zero(attended_transfer->threeway) ? attended_transfer->threeway : "*2"),
				 attended_threeway_transfer, NULL);
	tris_bridge_features_hook(&caller_features, (attended_transfer && !tris_strlen_zero(attended_transfer->abort) ? attended_transfer->abort : "*3"),
				 attended_abort_transfer, NULL);

	/* But for the caller we want to join the bridge in a blocking fashion so we don't spin around in this function doing nothing while waiting */
	attended_bridge_result = tris_bridge_join(attended_bridge, bridge_channel->chan, NULL, &caller_features);

	/* Since the above returned the caller features structure is of no more use */
	tris_bridge_features_cleanup(&caller_features);

	/* Drop the channel we are transferring to out of the above bridge since it has ended */
	if ((attended_bridge_result != TRIS_BRIDGE_CHANNEL_STATE_HANGUP) && !tris_bridge_depart(attended_bridge, chan)) {
		/* If the user wants to turn this into a threeway transfer then do so, otherwise they take our place */
		if (attended_bridge_result == TRIS_BRIDGE_CHANNEL_STATE_DEPART) {
			/* We want to impart them upon the bridge and just have us return to it as normal */
			tris_bridge_impart(bridge, chan, NULL, NULL);
		} else {
			tris_bridge_impart(bridge, chan, bridge_channel->chan, NULL);
		}
	} else {
		tris_stream_and_wait(bridge_channel->chan, "beeperr", TRIS_DIGIT_ANY);
		tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_WAIT);
	}

	/* Now that all channels are out of it we can destroy the bridge and the called features structure */
	tris_bridge_features_cleanup(&called_features);
	tris_bridge_destroy(attended_bridge);

	return 0;
}

/*! \brief Internal built in feature for hangup */
static int feature_hangup(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, void *hook_pvt)
{
	/* This is very simple, we basically change the state on the bridge channel to end and the core takes care of the rest */
	tris_bridge_change_state(bridge_channel, TRIS_BRIDGE_CHANNEL_STATE_END);
	return 0;
}

static int unload_module(void)
{
	return 0;
}

static int load_module(void)
{
	tris_bridge_features_register(TRIS_BRIDGE_BUILTIN_BLINDTRANSFER, feature_blind_transfer, NULL);
	tris_bridge_features_register(TRIS_BRIDGE_BUILTIN_ATTENDEDTRANSFER, feature_attended_transfer, NULL);
	tris_bridge_features_register(TRIS_BRIDGE_BUILTIN_HANGUP, feature_hangup, NULL);

	/* Bump up our reference count so we can't be unloaded */
	tris_module_ref(tris_module_info->self);

	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Built in bridging features");
