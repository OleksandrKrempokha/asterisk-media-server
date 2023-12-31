/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
 *
 * Matthew Fredrickson <creslin@digium.com>
 * Russell Bryant <russell@digium.com>
 *
 * Special thanks to Steve Underwood for the implementation
 * and for doing the 8khz<->g.722 direct translation code.
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
 * \brief codec_g722.c - translate between signed linear and ITU G.722-64kbps
 *
 * \author Matthew Fredrickson <creslin@digium.com>
 * \author Russell Bryant <russell@digium.com>
 *
 * \arg http://soft-switch.org/downloads/non-gpl-bits.tgz
 * \arg http://lists.digium.com/pipermail/trismedia-dev/2006-September/022866.html
 *
 * \ingroup codecs
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 150729 $")

#include "trismedia/linkedlists.h"
#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/translate.h"
#include "trismedia/utils.h"

#define BUFFER_SAMPLES   8096	/* size for the translation buffers */
#define BUF_SHIFT	5

#include "g722/g722.h"

/* Sample frame data */
#include "trismedia/slin.h"
#include "ex_g722.h"

struct g722_encoder_pvt {
	g722_encode_state_t g722;
};

struct g722_decoder_pvt {
	g722_decode_state_t g722;
};

/*! \brief init a new instance of g722_encoder_pvt. */
static int lintog722_new(struct tris_trans_pvt *pvt)
{
	struct g722_encoder_pvt *tmp = pvt->pvt;

	g722_encode_init(&tmp->g722, 64000, G722_SAMPLE_RATE_8000);

	return 0;
}

static int lin16tog722_new(struct tris_trans_pvt *pvt)
{
	struct g722_encoder_pvt *tmp = pvt->pvt;

	g722_encode_init(&tmp->g722, 64000, 0);

	return 0;
}

/*! \brief init a new instance of g722_encoder_pvt. */
static int g722tolin_new(struct tris_trans_pvt *pvt)
{
	struct g722_decoder_pvt *tmp = pvt->pvt;

	g722_decode_init(&tmp->g722, 64000, G722_SAMPLE_RATE_8000);

	return 0;
}

static int g722tolin16_new(struct tris_trans_pvt *pvt)
{
	struct g722_decoder_pvt *tmp = pvt->pvt;

	g722_decode_init(&tmp->g722, 64000, 0);

	return 0;
}

static int g722tolin_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	struct g722_decoder_pvt *tmp = pvt->pvt;
	int out_samples;
	int in_samples;

	/* g722_decode expects the samples to be in the invalid samples / 2 format */
	in_samples = f->samples / 2;

	out_samples = g722_decode(&tmp->g722, &pvt->outbuf.i16[pvt->samples * sizeof(int16_t)], 
		(uint8_t *) f->data.ptr, in_samples);

	pvt->samples += out_samples;

	pvt->datalen += (out_samples * sizeof(int16_t));

	return 0;
}

static int lintog722_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	struct g722_encoder_pvt *tmp = pvt->pvt;
	int outlen;

	outlen = g722_encode(&tmp->g722, (&pvt->outbuf.ui8[pvt->datalen]), 
		(int16_t *) f->data.ptr, f->samples);

	pvt->samples += outlen * 2;

	pvt->datalen += outlen;

	return 0;
}

static struct tris_translator g722tolin = {
	.name = "g722tolin",
	.srcfmt = TRIS_FORMAT_G722,
	.dstfmt = TRIS_FORMAT_SLINEAR,
	.newpvt = g722tolin_new,	/* same for both directions */
	.framein = g722tolin_framein,
	.sample = g722_sample,
	.desc_size = sizeof(struct g722_decoder_pvt),
	.buffer_samples = BUFFER_SAMPLES / sizeof(int16_t),
	.buf_size = BUFFER_SAMPLES,
	.plc_samples = 160,
};

static struct tris_translator lintog722 = {
	.name = "lintog722",
	.srcfmt = TRIS_FORMAT_SLINEAR,
	.dstfmt = TRIS_FORMAT_G722,
	.newpvt = lintog722_new,	/* same for both directions */
	.framein = lintog722_framein,
	.sample = slin8_sample,
	.desc_size = sizeof(struct g722_encoder_pvt),
	.buffer_samples = BUFFER_SAMPLES * 2,
	.buf_size = BUFFER_SAMPLES,
};

static struct tris_translator g722tolin16 = {
	.name = "g722tolin16",
	.srcfmt = TRIS_FORMAT_G722,
	.dstfmt = TRIS_FORMAT_SLINEAR16,
	.newpvt = g722tolin16_new,	/* same for both directions */
	.framein = g722tolin_framein,
	.sample = g722_sample,
	.desc_size = sizeof(struct g722_decoder_pvt),
	.buffer_samples = BUFFER_SAMPLES / sizeof(int16_t),
	.buf_size = BUFFER_SAMPLES,
	.plc_samples = 160,
};

static struct tris_translator lin16tog722 = {
	.name = "lin16tog722",
	.srcfmt = TRIS_FORMAT_SLINEAR16,
	.dstfmt = TRIS_FORMAT_G722,
	.newpvt = lin16tog722_new,	/* same for both directions */
	.framein = lintog722_framein,
	.sample = slin16_sample,
	.desc_size = sizeof(struct g722_encoder_pvt),
	.buffer_samples = BUFFER_SAMPLES * 2,
	.buf_size = BUFFER_SAMPLES,
};

static int parse_config(int reload)
{
	struct tris_variable *var;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	struct tris_config *cfg = tris_config_load("codecs.conf", config_flags);

	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEUNCHANGED || cfg == CONFIG_STATUS_FILEINVALID)
		return 0;
	for (var = tris_variable_browse(cfg, "plc"); var; var = var->next) {
		if (!strcasecmp(var->name, "genericplc")) {
			g722tolin.useplc = tris_true(var->value) ? 1 : 0;
			tris_verb(3, "codec_g722: %susing generic PLC\n",
					g722tolin.useplc ? "" : "not ");
		}
	}
	tris_config_destroy(cfg);
	return 0;
}

static int reload(void)
{
	if (parse_config(1))
		return TRIS_MODULE_LOAD_DECLINE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	int res = 0;

	res |= tris_unregister_translator(&g722tolin);
	res |= tris_unregister_translator(&lintog722);
	res |= tris_unregister_translator(&g722tolin16);
	res |= tris_unregister_translator(&lin16tog722);

	return res;
}

static int load_module(void)
{
	int res = 0;

	if (parse_config(0))
		return TRIS_MODULE_LOAD_DECLINE;

	res |= tris_register_translator(&g722tolin);
	res |= tris_register_translator(&lintog722);
	res |= tris_register_translator(&g722tolin16);
	res |= tris_register_translator(&lin16tog722);

	if (res) {
		unload_module();
		return TRIS_MODULE_LOAD_FAILURE;
	}	

	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "ITU G.722-64kbps G722 Transcoder",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
