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
 * \brief codec_a_mu.c - translate between alaw and ulaw directly
 *
 * \ingroup codecs
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 150729 $")

#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/alaw.h"
#include "trismedia/ulaw.h"
#include "trismedia/utils.h"

#define BUFFER_SAMPLES   8000	/* size for the translation buffers */

static unsigned char mu2a[256];
static unsigned char a2mu[256];

/* Sample frame data */
#include "ex_ulaw.h"
#include "ex_alaw.h"

/*! \brief convert frame data and store into the buffer */
static int alawtoulaw_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	int x = f->samples;
	unsigned char *src = f->data.ptr;
	unsigned char *dst = pvt->outbuf.uc + pvt->samples;

	pvt->samples += x;
	pvt->datalen += x;

	while (x--)
		*dst++ = a2mu[*src++];

	return 0;
}

/*! \brief convert frame data and store into the buffer */
static int ulawtoalaw_framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	int x = f->samples;
	unsigned char *src = f->data.ptr;
	unsigned char *dst = pvt->outbuf.uc + pvt->samples;

	pvt->samples += x;
	pvt->datalen += x;

	while (x--)
		*dst++ = mu2a[*src++];

	return 0;
}

static struct tris_translator alawtoulaw = {
	.name = "alawtoulaw",
	.srcfmt = TRIS_FORMAT_ALAW,
	.dstfmt = TRIS_FORMAT_ULAW,
	.framein = alawtoulaw_framein,
	.sample = alaw_sample,
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES,
};

static struct tris_translator ulawtoalaw = {
	.name = "ulawtoalaw",
	.srcfmt = TRIS_FORMAT_ULAW,
	.dstfmt = TRIS_FORMAT_ALAW,
	.framein = ulawtoalaw_framein,
	.sample = ulaw_sample,
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES,
};

/*! \brief standard module glue */

static int unload_module(void)
{
	int res;

	res = tris_unregister_translator(&ulawtoalaw);
	res |= tris_unregister_translator(&alawtoulaw);

	return res;
}

static int load_module(void)
{
	int res;
	int x;

	for (x=0;x<256;x++) {
		mu2a[x] = TRIS_LIN2A(TRIS_MULAW(x));
		a2mu[x] = TRIS_LIN2MU(TRIS_ALAW(x));
	}
	res = tris_register_translator(&alawtoulaw);
	if (!res)
		res = tris_register_translator(&ulawtoalaw);
	else
		tris_unregister_translator(&alawtoulaw);
	if (res)
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "A-law and Mulaw direct Coder/Decoder");
