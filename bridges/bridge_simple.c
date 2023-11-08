/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Digium, Inc.
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
 * \brief Simple two channel bridging module
 *
 * \author Joshua Colp <jcolp@digium.com>
 *
 * \ingroup bridges
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 180369 $")

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

static int simple_bridge_join(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel)
{
	struct tris_channel *c0 = TRIS_LIST_FIRST(&bridge->channels)->chan, *c1 = TRIS_LIST_LAST(&bridge->channels)->chan;

	/* If this is the first channel we can't make it compatible... unless we make it compatible with itself O.o */
	if (TRIS_LIST_FIRST(&bridge->channels) == TRIS_LIST_LAST(&bridge->channels)) {
		return 0;
	}

	/* See if we need to make these compatible */
	if (((c0->writeformat == c1->readformat) && (c0->readformat == c1->writeformat) && (c0->nativeformats == c1->nativeformats))) {
		return 0;
	}

	/* BOOM! We do. */
	return tris_channel_make_compatible(c0, c1);
}

static enum tris_bridge_write_result simple_bridge_write(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, struct tris_frame *frame)
{
	struct tris_bridge_channel *other = NULL;

	/* If this is the only channel in this bridge then immediately exit */
	if (TRIS_LIST_FIRST(&bridge->channels) == TRIS_LIST_LAST(&bridge->channels)) {
		return TRIS_BRIDGE_WRITE_FAILED;
	}

	/* Find the channel we actually want to write to */
	if (!(other = (TRIS_LIST_FIRST(&bridge->channels) == bridge_channel ? TRIS_LIST_LAST(&bridge->channels) : TRIS_LIST_FIRST(&bridge->channels)))) {
		return TRIS_BRIDGE_WRITE_FAILED;
	}

	/* Write the frame out if they are in the waiting state... don't worry about freeing it, the bridging core will take care of it */
	if (other->state == TRIS_BRIDGE_CHANNEL_STATE_WAIT) {
		tris_write(other->chan, frame);
	}

	return TRIS_BRIDGE_WRITE_SUCCESS;
}

static struct tris_bridge_technology simple_bridge = {
	.name = "simple_bridge",
	.capabilities = TRIS_BRIDGE_CAPABILITY_1TO1MIX | TRIS_BRIDGE_CAPABILITY_THREAD,
	.preference = TRIS_BRIDGE_PREFERENCE_MEDIUM,
	.formats = TRIS_FORMAT_AUDIO_MASK | TRIS_FORMAT_VIDEO_MASK | TRIS_FORMAT_TEXT_MASK,
	.join = simple_bridge_join,
	.write = simple_bridge_write,
};

static int unload_module(void)
{
	return tris_bridge_technology_unregister(&simple_bridge);
}

static int load_module(void)
{
	return tris_bridge_technology_register(&simple_bridge);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Simple two channel bridging module");
