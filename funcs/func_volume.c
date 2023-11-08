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
 * \brief Technology independent volume control
 *
 * \author Joshua Colp <jcolp@digium.com> 
 *
 * \ingroup functions
 *
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/audiohook.h"

/*** DOCUMENTATION
	<function name="VOLUME" language="en_US">
		<synopsis>
			Set the TX or RX volume of a channel.
		</synopsis>
		<syntax>
			<parameter name="direction" required="true">
				<para>Must be <literal>TX</literal> or <literal>RX</literal>.</para>
			</parameter>
		</syntax>
		<description>
			<para>The VOLUME function can be used to increase or decrease the <literal>tx</literal> or
			<literal>rx</literal> gain of any channel.</para>
			<para>For example:</para>
			<para>Set(VOLUME(TX)=3)</para>
			<para>Set(VOLUME(RX)=2)</para>
		</description>
	</function>
 ***/

struct volume_information {
	struct tris_audiohook audiohook;
	int tx_gain;
	int rx_gain;
};

static void destroy_callback(void *data)
{
	struct volume_information *vi = data;

	/* Destroy the audiohook, and destroy ourselves */
	tris_audiohook_destroy(&vi->audiohook);
	free(vi);

	return;
}

/*! \brief Static structure for datastore information */
static const struct tris_datastore_info volume_datastore = {
	.type = "volume",
	.destroy = destroy_callback
};

static int volume_callback(struct tris_audiohook *audiohook, struct tris_channel *chan, struct tris_frame *frame, enum tris_audiohook_direction direction)
{
	struct tris_datastore *datastore = NULL;
	struct volume_information *vi = NULL;
	int *gain = NULL;

	/* If the audiohook is stopping it means the channel is shutting down.... but we let the datastore destroy take care of it */
	if (audiohook->status == TRIS_AUDIOHOOK_STATUS_DONE)
		return 0;

	/* Grab datastore which contains our gain information */
	if (!(datastore = tris_channel_datastore_find(chan, &volume_datastore, NULL)))
		return 0;

	vi = datastore->data;

	/* If this is DTMF then allow them to increase/decrease the gains */
	if (frame->frametype == TRIS_FRAME_DTMF) {
		/* Only use DTMF coming from the source... not going to it */
		if (direction != TRIS_AUDIOHOOK_DIRECTION_READ)
			return 0;
		if (frame->subclass == '*') {
			vi->tx_gain += 1;
			vi->rx_gain += 1;
		} else if (frame->subclass == '#') {
			vi->tx_gain -= 1;
			vi->rx_gain -= 1;
		}
	} else if (frame->frametype == TRIS_FRAME_VOICE) {
		/* Based on direction of frame grab the gain, and confirm it is applicable */
		if (!(gain = (direction == TRIS_AUDIOHOOK_DIRECTION_READ) ? &vi->rx_gain : &vi->tx_gain) || !*gain)
			return 0;
		/* Apply gain to frame... easy as pi */
		tris_frame_adjust_volume(frame, *gain);
	}

	return 0;
}

static int volume_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	struct tris_datastore *datastore = NULL;
	struct volume_information *vi = NULL;
	int is_new = 0;

	if (!(datastore = tris_channel_datastore_find(chan, &volume_datastore, NULL))) {
		/* Allocate a new datastore to hold the reference to this volume and audiohook information */
		if (!(datastore = tris_datastore_alloc(&volume_datastore, NULL)))
			return 0;
		if (!(vi = tris_calloc(1, sizeof(*vi)))) {
			tris_datastore_free(datastore);
			return 0;
		}
		tris_audiohook_init(&vi->audiohook, TRIS_AUDIOHOOK_TYPE_MANIPULATE, "Volume");
		vi->audiohook.manipulate_callback = volume_callback;
		tris_set_flag(&vi->audiohook, TRIS_AUDIOHOOK_WANTS_DTMF);
		is_new = 1;
	} else {
		vi = datastore->data;
	}

	/* Adjust gain on volume information structure */
	if (!strcasecmp(data, "tx"))
		vi->tx_gain = atoi(value);
	else if (!strcasecmp(data, "rx"))
		vi->rx_gain = atoi(value);

	if (is_new) {
		datastore->data = vi;
		tris_channel_datastore_add(chan, datastore);
		tris_audiohook_attach(chan, &vi->audiohook);
	}

	return 0;
}

static struct tris_custom_function volume_function = {
	.name = "VOLUME",
	.write = volume_write,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&volume_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&volume_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Technology independent volume control");
