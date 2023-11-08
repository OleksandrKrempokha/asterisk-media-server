/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief codec_alaw.c - translate between signed linear and alaw
 * 
 * \ingroup codecs
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 150729 $")

#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/translate.h"
#include "trismedia/alaw.h"
#include "trismedia/utils.h"

#define BUFFER_SAMPLES   8096	/* size for the translation buffers */

/* Sample frame data */
#include "trismedia/slin.h"
#include "ex_alaw.h"

/*! \brief decode frame into lin and fill output buffer. */
static int alawtolin_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	int i = f->samples;
	unsigned char *src = f->data.ptr;
	int16_t *dst = pvt->outbuf.i16 + pvt->samples;

	pvt->samples += i;
	pvt->datalen += i * 2;	/* 2 bytes/sample */
	
	while (i--)
		*dst++ = TRIS_ALAW(*src++);

	return 0;
}

/*! \brief convert and store input samples in output buffer */
static int lintoalaw_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	int i = f->samples;
	char *dst = pvt->outbuf.c + pvt->samples;
	int16_t *src = f->data.ptr;

	pvt->samples += i;
	pvt->datalen += i;	/* 1 byte/sample */

	while (i--) 
		*dst++ = TRIS_LIN2A(*src++);

	return 0;
}

static struct tris_translator alawtolin = {
	.name = "alawtolin",
	.srcfmt = TRIS_FORMAT_ALAW,
	.dstfmt = TRIS_FORMAT_SLINEAR,
	.framein = alawtolin_framein,
	.sample = alaw_sample,
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
	.plc_samples = 160,
};

static struct tris_translator lintoalaw = {
	"lintoalaw",
	.srcfmt = TRIS_FORMAT_SLINEAR,
	.dstfmt = TRIS_FORMAT_ALAW,
	.framein = lintoalaw_framein,
	.sample = slin8_sample,
	.buffer_samples = BUFFER_SAMPLES,
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
			alawtolin.useplc = tris_true(var->value) ? 1 : 0;
			tris_verb(3, "codec_alaw: %susing generic PLC\n", alawtolin.useplc ? "" : "not ");
		}
	}
	tris_config_destroy(cfg);
	return 0;
}

/*! \brief standard module stuff */

static int reload(void)
{
	if (parse_config(1))
		return TRIS_MODULE_LOAD_DECLINE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_translator(&lintoalaw);
	res |= tris_unregister_translator(&alawtolin);

	return res;
}

static int load_module(void)
{
	int res;

	if (parse_config(0))
		return TRIS_MODULE_LOAD_DECLINE;
	res = tris_register_translator(&alawtolin);
	if (!res)
		res = tris_register_translator(&lintoalaw);
	else
		tris_unregister_translator(&alawtolin);
	if (res)
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "A-law Coder/Decoder",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
