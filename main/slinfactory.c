/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Anthony Minessale II.
 *
 * Anthony Minessale <anthmct@yahoo.com>
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
 * \brief A machine to gather up arbitrary frames and convert them
 * to raw slinear on demand.
 *
 * \author Anthony Minessale <anthmct@yahoo.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 201097 $")

#include "trismedia/frame.h"
#include "trismedia/slinfactory.h"
#include "trismedia/translate.h"

void tris_slinfactory_init(struct tris_slinfactory *sf) 
{
	memset(sf, 0, sizeof(*sf));
	sf->offset = sf->hold;
	sf->output_format = TRIS_FORMAT_SLINEAR;
}

int tris_slinfactory_init_rate(struct tris_slinfactory *sf, unsigned int sample_rate) 
{
	memset(sf, 0, sizeof(*sf));
	sf->offset = sf->hold;
	switch (sample_rate) {
	case 8000:
		sf->output_format = TRIS_FORMAT_SLINEAR;
		break;
	case 16000:
		sf->output_format = TRIS_FORMAT_SLINEAR16;
		break;
	default:
		return -1;
	}

	return 0;
}

void tris_slinfactory_destroy(struct tris_slinfactory *sf) 
{
	struct tris_frame *f;

	if (sf->trans) {
		tris_translator_free_path(sf->trans);
		sf->trans = NULL;
	}

	while ((f = TRIS_LIST_REMOVE_HEAD(&sf->queue, frame_list)))
		tris_frfree(f);
}

int tris_slinfactory_feed(struct tris_slinfactory *sf, struct tris_frame *f)
{
	struct tris_frame *begin_frame = f, *duped_frame = NULL, *frame_ptr;
	unsigned int x = 0;

	/* In some cases, we can be passed a frame which has no data in it, but
	 * which has a positive number of samples defined. Once such situation is
	 * when a jitter buffer is in use and the jitter buffer interpolates a frame.
	 * The frame it produces has data set to NULL, datalen set to 0, and samples
	 * set to either 160 or 240.
	 */
	if (!f->data.ptr) {
		return 0;
	}

	if (f->subclass != sf->output_format) {
		if (sf->trans && f->subclass != sf->format) {
			tris_translator_free_path(sf->trans);
			sf->trans = NULL;
		}

		if (!sf->trans) {
			if (!(sf->trans = tris_translator_build_path(sf->output_format, f->subclass))) {
				tris_log(LOG_WARNING, "Cannot build a path from %s to %s\n", tris_getformatname(f->subclass),
					tris_getformatname(sf->output_format));
				return -1;
			}
			sf->format = f->subclass;
		}

		if (!(begin_frame = tris_translate(sf->trans, f, 0))) {
			return 0;
		}
		
		if (!(duped_frame = tris_frisolate(begin_frame))) {
			return 0;
		}

		if (duped_frame != begin_frame) {
			tris_frfree(begin_frame);
		}
	} else {
		if (sf->trans) {
			tris_translator_free_path(sf->trans);
			sf->trans = NULL;
		}
		if (!(duped_frame = tris_frdup(f)))
			return 0;
	}

	TRIS_LIST_TRAVERSE(&sf->queue, frame_ptr, frame_list) {
		x++;
	}

	/* if the frame was translated, the translator may have returned multiple
	   frames, so process each of them
	*/
	for (begin_frame = duped_frame; begin_frame; begin_frame = TRIS_LIST_NEXT(begin_frame, frame_list)) {
		TRIS_LIST_INSERT_TAIL(&sf->queue, begin_frame, frame_list);
		sf->size += begin_frame->samples;
	}

	return x;
}

int tris_slinfactory_read(struct tris_slinfactory *sf, short *buf, size_t samples) 
{
	struct tris_frame *frame_ptr;
	unsigned int sofar = 0, ineed, remain;
	short *frame_data, *offset = buf;

	while (sofar < samples) {
		ineed = samples - sofar;

		if (sf->holdlen) {
			if (sf->holdlen <= ineed) {
				memcpy(offset, sf->hold, sf->holdlen * sizeof(*offset));
				sofar += sf->holdlen;
				offset += sf->holdlen;
				sf->holdlen = 0;
				sf->offset = sf->hold;
			} else {
				remain = sf->holdlen - ineed;
				memcpy(offset, sf->offset, ineed * sizeof(*offset));
				sofar += ineed;
				sf->offset += ineed;
				sf->holdlen = remain;
			}
			continue;
		}
		
		if ((frame_ptr = TRIS_LIST_REMOVE_HEAD(&sf->queue, frame_list))) {
			frame_data = frame_ptr->data.ptr;
			
			if (frame_ptr->samples <= ineed) {
				memcpy(offset, frame_data, frame_ptr->samples * sizeof(*offset));
				sofar += frame_ptr->samples;
				offset += frame_ptr->samples;
			} else {
				remain = frame_ptr->samples - ineed;
				memcpy(offset, frame_data, ineed * sizeof(*offset));
				sofar += ineed;
				frame_data += ineed;
				if (remain > (TRIS_SLINFACTORY_MAX_HOLD - sf->holdlen)) {
					remain = TRIS_SLINFACTORY_MAX_HOLD - sf->holdlen;
				}
				memcpy(sf->hold, frame_data, remain * sizeof(*offset));
				sf->holdlen = remain;
			}
			tris_frfree(frame_ptr);
		} else {
			break;
		}
	}

	sf->size -= sofar;
	return sofar;
}

unsigned int tris_slinfactory_available(const struct tris_slinfactory *sf)
{
	return sf->size;
}

void tris_slinfactory_flush(struct tris_slinfactory *sf)
{
	struct tris_frame *fr = NULL;

	if (sf->trans) {
		tris_translator_free_path(sf->trans);
		sf->trans = NULL;
	}

	while ((fr = TRIS_LIST_REMOVE_HEAD(&sf->queue, frame_list)))
		tris_frfree(fr);

	sf->size = sf->holdlen = 0;
	sf->offset = sf->hold;

	return;
}
