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
 * \brief codec_ulaw.c - translate between signed linear and ulaw
 * 
 * \ingroup codecs
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 150729 $")

#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/translate.h"
#include "trismedia/ulaw.h"
#include "trismedia/utils.h"

#define BUFFER_SAMPLES   8096	/* size for the translation buffers */

/* Sample frame data */
#include "trismedia/slin.h"
#include "ex_ulaw.h"

/*! \brief convert and store samples in outbuf */
static int ulawtolin_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	int i = f->samples;
	unsigned char *src = f->data.ptr;
	int16_t *dst = pvt->outbuf.i16 + pvt->samples;

	pvt->samples += i;
	pvt->datalen += i * 2;	/* 2 bytes/sample */

	/* convert and copy in outbuf */
	while (i--)
		*dst++ = TRIS_MULAW(*src++);

	return 0;
}

/*! \brief convert and store samples in outbuf */
static int lintoulaw_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	int i = f->samples;
	char *dst = pvt->outbuf.c + pvt->samples;
	int16_t *src = f->data.ptr;

	pvt->samples += i;
	pvt->datalen += i;	/* 1 byte/sample */

	while (i--)
		*dst++ = TRIS_LIN2MU(*src++);

	return 0;
}

/*!
 * \brief The complete translator for ulawToLin.
 */

static struct tris_translator ulawtolin = {
	.name = "ulawtolin",
	.srcfmt = TRIS_FORMAT_ULAW,
	.dstfmt = TRIS_FORMAT_SLINEAR,
	.framein = ulawtolin_framein,
	.sample = ulaw_sample,
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
	.plc_samples = 160,
};

/*!
 * \brief The complete translator for LinToulaw.
 */

static struct tris_translator lintoulaw = {
	.name = "lintoulaw",
	.srcfmt = TRIS_FORMAT_SLINEAR,
	.dstfmt = TRIS_FORMAT_ULAW,
	.framein = lintoulaw_framein,
	.sample = slin8_sample,
	.buf_size = BUFFER_SAMPLES,
	.buffer_samples = BUFFER_SAMPLES,
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
			ulawtolin.useplc = tris_true(var->value) ? 1 : 0;
			tris_verb(3, "codec_ulaw: %susing generic PLC\n", ulawtolin.useplc ? "" : "not ");
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
	int res;

	res = tris_unregister_translator(&lintoulaw);
	res |= tris_unregister_translator(&ulawtolin);

	return res;
}

static int load_module(void)
{
	int res;

	if (parse_config(0))
		return TRIS_MODULE_LOAD_DECLINE;
	res = tris_register_translator(&ulawtolin);
	if (!res)
		res = tris_register_translator(&lintoulaw);
	else
		tris_unregister_translator(&ulawtolin);
	if (res)
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "mu-Law Coder/Decoder",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
