/*! \file
 * \brief
 *
 * Copyright (C) 2008, Digium, Inc.
 *
 * Distributed under the terms of the GNU General Public License
 *
 */

static uint8_t ex_lpc10[] = {
	0x01, 0x08, 0x31, 0x08, 0x31, 0x80, 0x30,
};

static struct tris_frame *lpc10_sample(void)
{
	static struct tris_frame f = {
		.frametype = TRIS_FRAME_VOICE,
		.subclass = TRIS_FORMAT_LPC10,
		.datalen = sizeof(ex_lpc10),
		/* All frames are 22 ms long (maybe a little more -- why did he choose
		   LPC10_SAMPLES_PER_FRAME sample frames anyway?? */
		.samples = LPC10_SAMPLES_PER_FRAME,
		.mallocd = 0,
		.offset = 0,
		.src = __PRETTY_FUNCTION__,
		.data.ptr = ex_lpc10,
	};

	return &f;
}
