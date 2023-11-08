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

/*! 
 * \file
 *
 * \brief Old-style G.723.1 frame/timestamp format.
 * 
 * \arg Extensions: g723, g723sf
 * \ingroup formats
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 233694 $")

#include "trismedia/mod_format.h"
#include "trismedia/module.h"

#define G723_MAX_SIZE 1024

static struct tris_frame *g723_read(struct tris_filestream *s, int *whennext)
{
	unsigned short size;
	int res;
	int delay;
	/* Read the delay for the next packet, and schedule again if necessary */
	/* XXX is this ignored ? */
	if (fread(&delay, 1, 4, s->f) == 4) 
		delay = ntohl(delay);
	else
		delay = -1;
	if (fread(&size, 1, 2, s->f) != 2) {
		/* Out of data, or the file is no longer valid.  In any case
		   go ahead and stop the stream */
		return NULL;
	}
	/* Looks like we have a frame to read from here */
	size = ntohs(size);
	if (size > G723_MAX_SIZE) {
		tris_log(LOG_WARNING, "Size %d is invalid\n", size);
		/* The file is apparently no longer any good, as we
		   shouldn't ever get frames even close to this 
		   size.  */
		return NULL;
	}
	/* Read the data into the buffer */
	s->fr.frametype = TRIS_FRAME_VOICE;
	s->fr.subclass = TRIS_FORMAT_G723_1;
	s->fr.mallocd = 0;
	TRIS_FRAME_SET_BUFFER(&s->fr, s->buf, TRIS_FRIENDLY_OFFSET, size);
	if ((res = fread(s->fr.data.ptr, 1, s->fr.datalen, s->f)) != size) {
		tris_log(LOG_WARNING, "Short read (%d of %d bytes) (%s)!\n", res, size, strerror(errno));
		return NULL;
	}
	*whennext = s->fr.samples = 240;
	return &s->fr;
}

static int g723_write(struct tris_filestream *s, struct tris_frame *f)
{
	uint32_t delay;
	uint16_t size;
	int res;
	/* XXX there used to be a check s->fr means a read stream */
	if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Asked to write non-voice frame!\n");
		return -1;
	}
	if (f->subclass != TRIS_FORMAT_G723_1) {
		tris_log(LOG_WARNING, "Asked to write non-g723 frame!\n");
		return -1;
	}
	delay = 0;
	if (f->datalen <= 0) {
		tris_log(LOG_WARNING, "Short frame ignored (%d bytes long?)\n", f->datalen);
		return 0;
	}
	if ((res = fwrite(&delay, 1, 4, s->f)) != 4) {
		tris_log(LOG_WARNING, "Unable to write delay: res=%d (%s)\n", res, strerror(errno));
		return -1;
	}
	size = htons(f->datalen);
	if ((res = fwrite(&size, 1, 2, s->f)) != 2) {
		tris_log(LOG_WARNING, "Unable to write size: res=%d (%s)\n", res, strerror(errno));
		return -1;
	}
	if ((res = fwrite(f->data.ptr, 1, f->datalen, s->f)) != f->datalen) {
		tris_log(LOG_WARNING, "Unable to write frame: res=%d (%s)\n", res, strerror(errno));
		return -1;
	}	
	return 0;
}

static int g723_seek(struct tris_filestream *fs, off_t sample_offset, int whence)
{
	return -1;
}

static int g723_trunc(struct tris_filestream *fs)
{
	/* Truncate file to current length */
	if (ftruncate(fileno(fs->f), ftello(fs->f)) < 0)
		return -1;
	return 0;
}

static off_t g723_tell(struct tris_filestream *fs)
{
	return -1;
}

static const struct tris_format g723_1_f = {
	.name = "g723sf",
	.exts = "g723|g723sf",
	.format = TRIS_FORMAT_G723_1,
	.write = g723_write,
	.seek =	g723_seek,
	.trunc = g723_trunc,
	.tell =	g723_tell,
	.read =	g723_read,
	.buf_size = G723_MAX_SIZE + TRIS_FRIENDLY_OFFSET,
};

static int load_module(void)
{
	if (tris_format_register(&g723_1_f))
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	return tris_format_unregister(g723_1_f.name);
}	

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_LOAD_ORDER, "G.723.1 Simple Timestamp File Format",
	.load = load_module,
	.unload = unload_module,
	.load_pri = 10,
);
