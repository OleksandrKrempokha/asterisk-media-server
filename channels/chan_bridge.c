/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
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
 * \author Joshua Colp <jcolp@digium.com>
 *
 * \brief Bridge Interaction Channel
 *
 * \ingroup channel_drivers
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 180369 $")

#include <fcntl.h>
#include <sys/signal.h>

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/sched.h"
#include "trismedia/io.h"
#include "trismedia/rtp.h"
#include "trismedia/acl.h"
#include "trismedia/callerid.h"
#include "trismedia/file.h"
#include "trismedia/cli.h"
#include "trismedia/app.h"
#include "trismedia/bridging.h"

static struct tris_channel *bridge_request(const char *type, int format, void *data, int *cause, struct tris_channel* src);
static int bridge_call(struct tris_channel *ast, char *dest, int timeout);
static int bridge_hangup(struct tris_channel *ast);
static struct tris_frame *bridge_read(struct tris_channel *ast);
static int bridge_write(struct tris_channel *ast, struct tris_frame *f);
static struct tris_channel *bridge_bridgedchannel(struct tris_channel *chan, struct tris_channel *bridge);

static const struct tris_channel_tech bridge_tech = {
	.type = "Bridge",
	.description = "Bridge Interaction Channel",
	.capabilities = -1,
	.requester = bridge_request,
	.call = bridge_call,
	.hangup = bridge_hangup,
	.read = bridge_read,
	.write = bridge_write,
	.write_video = bridge_write,
	.exception = bridge_read,
	.bridged_channel = bridge_bridgedchannel,
};

struct bridge_pvt {
	tris_mutex_t lock;           /*!< Lock that protects this structure */
	struct tris_channel *input;  /*!< Input channel - talking to source */
	struct tris_channel *output; /*!< Output channel - talking to bridge */
};

/*! \brief Called when the user of this channel wants to get the actual channel in the bridge */
static struct tris_channel *bridge_bridgedchannel(struct tris_channel *chan, struct tris_channel *bridge)
{
	struct bridge_pvt *p = chan->tech_pvt;
	return (chan == p->input) ? p->output : bridge;
}

/*! \brief Called when a frame should be read from the channel */
static struct tris_frame  *bridge_read(struct tris_channel *ast)
{
	return &tris_null_frame;
}

/*! \brief Called when a frame should be written out to a channel */
static int bridge_write(struct tris_channel *ast, struct tris_frame *f)
{
	struct bridge_pvt *p = ast->tech_pvt;
	struct tris_channel *other;

	tris_mutex_lock(&p->lock);

	other = (p->input == ast ? p->output : p->input);

	while (other && tris_channel_trylock(other)) {
		tris_mutex_unlock(&p->lock);
		do {
			CHANNEL_DEADLOCK_AVOIDANCE(ast);
		} while (tris_mutex_trylock(&p->lock));
		other = (p->input == ast ? p->output : p->input);
	}

	/* We basically queue the frame up on the other channel if present */
	if (other) {
		tris_queue_frame(other, f);
		tris_channel_unlock(other);
	}

	tris_mutex_unlock(&p->lock);

	return 0;
}

/*! \brief Called when the channel should actually be dialed */
static int bridge_call(struct tris_channel *ast, char *dest, int timeout)
{
	struct bridge_pvt *p = ast->tech_pvt;

	/* If no bridge has been provided on the input channel, bail out */
	if (!ast->bridge) {
		return -1;
	}

	/* Impart the output channel upon the given bridge of the input channel */
	tris_bridge_impart(p->input->bridge, p->output, NULL, NULL);

	return 0;
}

/*! \brief Helper function to not deadlock when queueing the hangup frame */
static void bridge_queue_hangup(struct bridge_pvt *p, struct tris_channel *us)
{
	struct tris_channel *other = (p->input == us ? p->output : p->input);

	while (other && tris_channel_trylock(other)) {
		tris_mutex_unlock(&p->lock);
		do {
			CHANNEL_DEADLOCK_AVOIDANCE(us);
		} while (tris_mutex_trylock(&p->lock));
		other = (p->input == us ? p->output : p->input);
	}

	/* We basically queue the frame up on the other channel if present */
	if (other) {
		tris_queue_hangup(other);
		tris_channel_unlock(other);
	}

	return;
}

/*! \brief Called when a channel should be hung up */
static int bridge_hangup(struct tris_channel *ast)
{
	struct bridge_pvt *p = ast->tech_pvt;

	tris_mutex_lock(&p->lock);

	/* Figure out which channel this is... and set it to NULL as it has gone, but also queue up a hangup frame. */
	if (p->input == ast) {
		if (p->output) {
			bridge_queue_hangup(p, ast);
		}
		p->input = NULL;
	} else if (p->output == ast) {
		if (p->input) {
			bridge_queue_hangup(p, ast);
		}
		p->output = NULL;
	}

	/* Deal with the Trismedia portion of it */
	ast->tech_pvt = NULL;

	/* If both sides have been terminated free the structure and be done with things */
	if (!p->input && !p->output) {
		tris_mutex_unlock(&p->lock);
		tris_mutex_destroy(&p->lock);
		tris_free(p);
	} else {
		tris_mutex_unlock(&p->lock);
	}

	return 0;
}

/*! \brief Called when we want to place a call somewhere, but not actually call it... yet */
static struct tris_channel *bridge_request(const char *type, int format, void *data, int *cause, struct tris_channel* src)
{
	struct bridge_pvt *p = NULL;

	/* Try to allocate memory for our very minimal pvt structure */
	if (!(p = tris_calloc(1, sizeof(*p)))) {
		return NULL;
	}

	/* Try to grab two Trismedia channels to use as input and output channels */
	if (!(p->input = tris_channel_alloc(1, TRIS_STATE_UP, 0, 0, "", "", "", 0, "Bridge/%p-input", p))) {
		tris_free(p);
		return NULL;
	}
	if (!(p->output = tris_channel_alloc(1, TRIS_STATE_UP, 0, 0, "", "", "", 0, "Bridge/%p-output", p))) {
		tris_channel_free(p->input);
		tris_free(p);
		return NULL;
	}

	/* Setup the lock on the pvt structure, we will need that */
	tris_mutex_init(&p->lock);

	/* Setup parameters on both new channels */
	p->input->tech = p->output->tech = &bridge_tech;
	p->input->tech_pvt = p->output->tech_pvt = p;
	p->input->nativeformats = p->output->nativeformats = TRIS_FORMAT_SLINEAR;
	p->input->readformat = p->output->readformat = TRIS_FORMAT_SLINEAR;
	p->input->rawreadformat = p->output->rawreadformat = TRIS_FORMAT_SLINEAR;
	p->input->writeformat = p->output->writeformat = TRIS_FORMAT_SLINEAR;
	p->input->rawwriteformat = p->output->rawwriteformat = TRIS_FORMAT_SLINEAR;

	return p->input;
}

/*! \brief Load module into PBX, register channel */
static int load_module(void)
{
	/* Make sure we can register our channel type */
	if (tris_channel_register(&bridge_tech)) {
		tris_log(LOG_ERROR, "Unable to register channel class 'Bridge'\n");
		return TRIS_MODULE_LOAD_FAILURE;
	}
	return TRIS_MODULE_LOAD_SUCCESS;
}

/*! \brief Unload the bridge interaction channel from Trismedia */
static int unload_module(void)
{
	tris_channel_unregister(&bridge_tech);
	return 0;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Bridge Interaction Channel");
