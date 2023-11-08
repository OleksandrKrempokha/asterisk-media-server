/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Digium, Inc.
 *
 * Brian Degenhardt <bmd@digium.com>
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
 * \brief Noise reduction and automatic gain control (AGC)
 *
 * \author Brian Degenhardt <bmd@digium.com> 
 * \author Brett Bryant <bbryant@digium.com> 
 *
 * \ingroup functions
 *
 * \extref The Speex library - http://www.speex.org
 */

/*** MODULEINFO
	<depend>speex</depend>
	<depend>speex_preprocess</depend>
	<use>speexdsp</use>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 224859 $")

#include <speex/speex_preprocess.h>
#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/audiohook.h"

#define DEFAULT_AGC_LEVEL 8000.0

/*** DOCUMENTATION
	<function name="AGC" language="en_US">
		<synopsis>
			Apply automatic gain control to audio on a channel.
		</synopsis>
		<syntax>
			<parameter name="channeldirection" required="true">
				<para>This can be either <literal>rx</literal> or <literal>tx</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>The AGC function will apply automatic gain control to the audio on the
			channel that it is executed on. Using <literal>rx</literal> for audio received
			and <literal>tx</literal> for audio transmitted to the channel. When using this
			function you set a target audio level. It is primarily intended for use with
			analog lines, but could be useful for other channels as well. The target volume 
			is set with a number between <literal>1-32768</literal>. The larger the number
			the louder (more gain) the channel will receive.</para>
			<para>Examples:</para>
			<para>exten => 1,1,Set(AGC(rx)=8000)</para>
			<para>exten => 1,2,Set(AGC(tx)=off)</para>
		</description>
	</function>
	<function name="DENOISE" language="en_US">
		<synopsis>
			Apply noise reduction to audio on a channel.
		</synopsis>
		<syntax>
			<parameter name="channeldirection" required="true">
				<para>This can be either <literal>rx</literal> or <literal>tx</literal> 
				the values that can be set to this are either <literal>on</literal> and
				<literal>off</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>The DENOISE function will apply noise reduction to audio on the channel
			that it is executed on. It is very useful for noisy analog lines, especially
			when adjusting gains or using AGC. Use <literal>rx</literal> for audio received from the channel
			and <literal>tx</literal> to apply the filter to the audio being sent to the channel.</para>
			<para>Examples:</para>
			<para>exten => 1,1,Set(DENOISE(rx)=on)</para>
			<para>exten => 1,2,Set(DENOISE(tx)=off)</para>
		</description>
	</function>
 ***/

struct speex_direction_info {
	SpeexPreprocessState *state;	/*!< speex preprocess state object */
	int agc;						/*!< audio gain control is enabled or not */
	int denoise;					/*!< denoise is enabled or not */
	int samples;					/*!< n of 8Khz samples in last frame */
	float agclevel;					/*!< audio gain control level [1.0 - 32768.0] */
};

struct speex_info {
	struct tris_audiohook audiohook;
	struct speex_direction_info *tx, *rx;
};

static void destroy_callback(void *data) 
{
	struct speex_info *si = data;

	tris_audiohook_destroy(&si->audiohook);

	if (si->rx && si->rx->state) {
		speex_preprocess_state_destroy(si->rx->state);
	}

	if (si->tx && si->tx->state) {
		speex_preprocess_state_destroy(si->tx->state);
	}

	if (si->rx) {
		tris_free(si->rx);
	}

	if (si->tx) {
		tris_free(si->tx);
	}

	tris_free(data);
};

static const struct tris_datastore_info speex_datastore = {
	.type = "speex",
	.destroy = destroy_callback
};

static int speex_callback(struct tris_audiohook *audiohook, struct tris_channel *chan, struct tris_frame *frame, enum tris_audiohook_direction direction)
{
	struct tris_datastore *datastore = NULL;
	struct speex_direction_info *sdi = NULL;
	struct speex_info *si = NULL;
	char source[80];

	/* If the audiohook is stopping it means the channel is shutting down.... but we let the datastore destroy take care of it */
	if (audiohook->status == TRIS_AUDIOHOOK_STATUS_DONE || frame->frametype != TRIS_FRAME_VOICE) {
		return -1;
	}

	/* We are called with chan already locked */
	if (!(datastore = tris_channel_datastore_find(chan, &speex_datastore, NULL))) {
		return -1;
	}

	si = datastore->data;

	sdi = (direction == TRIS_AUDIOHOOK_DIRECTION_READ) ? si->rx : si->tx;

	if (!sdi) {
		return -1;
	}

	if (sdi->samples != frame->samples) {
		if (sdi->state) {
			speex_preprocess_state_destroy(sdi->state);
		}

		if (!(sdi->state = speex_preprocess_state_init((sdi->samples = frame->samples), 8000))) {
			return -1;
		}

		speex_preprocess_ctl(sdi->state, SPEEX_PREPROCESS_SET_AGC, &sdi->agc);

		if (sdi->agc) {
			speex_preprocess_ctl(sdi->state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &sdi->agclevel);
		}

		speex_preprocess_ctl(sdi->state, SPEEX_PREPROCESS_SET_DENOISE, &sdi->denoise);
	}

	speex_preprocess(sdi->state, frame->data.ptr, NULL);
	snprintf(source, sizeof(source), "%s/speex", frame->src);
	if (frame->mallocd & TRIS_MALLOCD_SRC) {
		tris_free((char *) frame->src);
	}
	frame->src = tris_strdup(source);
	frame->mallocd |= TRIS_MALLOCD_SRC;

	return 0;
}

static int speex_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	struct tris_datastore *datastore = NULL;
	struct speex_info *si = NULL;
	struct speex_direction_info **sdi = NULL;
	int is_new = 0;

	tris_channel_lock(chan);
	if (!(datastore = tris_channel_datastore_find(chan, &speex_datastore, NULL))) {
		tris_channel_unlock(chan);

		if (!(datastore = tris_datastore_alloc(&speex_datastore, NULL))) {
			return 0;
		}

		if (!(si = tris_calloc(1, sizeof(*si)))) {
			tris_datastore_free(datastore);
			return 0;
		}

		tris_audiohook_init(&si->audiohook, TRIS_AUDIOHOOK_TYPE_MANIPULATE, "speex");
		si->audiohook.manipulate_callback = speex_callback;

		is_new = 1;
	} else {
		tris_channel_unlock(chan);
		si = datastore->data;
	}

	if (!strcasecmp(data, "rx")) {
		sdi = &si->rx;
	} else if (!strcasecmp(data, "tx")) {
		sdi = &si->tx;
	} else {
		tris_log(LOG_ERROR, "Invalid argument provided to the %s function\n", cmd);

		if (is_new) {
			tris_datastore_free(datastore);
			return -1;
		}
	}

	if (!*sdi) {
		if (!(*sdi = tris_calloc(1, sizeof(**sdi)))) {
			return 0;
		}
		/* Right now, the audiohooks API will _only_ provide us 8 kHz slinear
		 * audio.  When it supports 16 kHz (or any other sample rates, we will
		 * have to take that into account here. */
		(*sdi)->samples = -1;
	}

	if (!strcasecmp(cmd, "agc")) {
		if (!sscanf(value, "%30f", &(*sdi)->agclevel))
			(*sdi)->agclevel = tris_true(value) ? DEFAULT_AGC_LEVEL : 0.0;
	
		if ((*sdi)->agclevel > 32768.0) {
			tris_log(LOG_WARNING, "AGC(%s)=%.01f is greater than 32768... setting to 32768 instead\n", 
					((*sdi == si->rx) ? "rx" : "tx"), (*sdi)->agclevel);
			(*sdi)->agclevel = 32768.0;
		}
	
		(*sdi)->agc = !!((*sdi)->agclevel);

		if ((*sdi)->state) {
			speex_preprocess_ctl((*sdi)->state, SPEEX_PREPROCESS_SET_AGC, &(*sdi)->agc);
			if ((*sdi)->agc) {
				speex_preprocess_ctl((*sdi)->state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &(*sdi)->agclevel);
			}
		}
	} else if (!strcasecmp(cmd, "denoise")) {
		(*sdi)->denoise = (tris_true(value) != 0);

		if ((*sdi)->state) {
			speex_preprocess_ctl((*sdi)->state, SPEEX_PREPROCESS_SET_DENOISE, &(*sdi)->denoise);
		}
	}

	if (!(*sdi)->agc && !(*sdi)->denoise) {
		if ((*sdi)->state)
			speex_preprocess_state_destroy((*sdi)->state);

		tris_free(*sdi);
		*sdi = NULL;
	}

	if (!si->rx && !si->tx) {
		if (is_new) {
			is_new = 0;
		} else {
			tris_channel_lock(chan);
			tris_channel_datastore_remove(chan, datastore);
			tris_channel_unlock(chan);
			tris_audiohook_remove(chan, &si->audiohook);
			tris_audiohook_detach(&si->audiohook);
		}
		
		tris_datastore_free(datastore);
	}

	if (is_new) { 
		datastore->data = si;
		tris_channel_lock(chan);
		tris_channel_datastore_add(chan, datastore);
		tris_channel_unlock(chan);
		tris_audiohook_attach(chan, &si->audiohook);
	}

	return 0;
}

static int speex_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_datastore *datastore = NULL;
	struct speex_info *si = NULL;
	struct speex_direction_info *sdi = NULL;

	if (!chan) {
		tris_log(LOG_ERROR, "%s cannot be used without a channel!\n", cmd);
		return -1;
	}

	tris_channel_lock(chan);
	if (!(datastore = tris_channel_datastore_find(chan, &speex_datastore, NULL))) {
		tris_channel_unlock(chan);
		return -1;
	}
	tris_channel_unlock(chan);

	si = datastore->data;

	if (!strcasecmp(data, "tx"))
		sdi = si->tx;
	else if (!strcasecmp(data, "rx"))
		sdi = si->rx;
	else {
		tris_log(LOG_ERROR, "%s(%s) must either \"tx\" or \"rx\"\n", cmd, data);
		return -1;
	}

	if (!strcasecmp(cmd, "agc"))
		snprintf(buf, len, "%.01f", sdi ? sdi->agclevel : 0.0);
	else
		snprintf(buf, len, "%d", sdi ? sdi->denoise : 0);

	return 0;
}

static struct tris_custom_function agc_function = {
	.name = "AGC",
	.write = speex_write,
	.read = speex_read
};

static struct tris_custom_function denoise_function = {
	.name = "DENOISE",
	.write = speex_write,
	.read = speex_read
};

static int unload_module(void)
{
	tris_custom_function_unregister(&agc_function);
	tris_custom_function_unregister(&denoise_function);
	return 0;
}

static int load_module(void)
{
	if (tris_custom_function_register(&agc_function)) {
		return TRIS_MODULE_LOAD_DECLINE;
	}

	if (tris_custom_function_register(&denoise_function)) {
		tris_custom_function_unregister(&agc_function);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Noise reduction and Automatic Gain Control (AGC)");
