/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Brian K. West <brian@bkw.org>
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
 * \brief Save to raw, headerless iLBC data.
 * \arg File name extension: ilbc
 * \ingroup formats
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 233694 $")

#include "trismedia/mod_format.h"
#include "trismedia/module.h"
#include "trismedia/endian.h"

/* Some Ideas for this code came from makeg729e.c by Jeffrey Chilton */

/* Portions of the conversion code are by guido@sienanet.it */

#define	ILBC_BUF_SIZE	50	/* One Real iLBC Frame */
#define	ILBC_SAMPLES	240

static struct tris_frame *ilbc_read(struct tris_filestream *s, int *whennext)
{
	int res;
	/* Send a frame from the file to the appropriate channel */
	s->fr.frametype = TRIS_FRAME_VOICE;
	s->fr.subclass = TRIS_FORMAT_ILBC;
	s->fr.mallocd = 0;
	TRIS_FRAME_SET_BUFFER(&s->fr, s->buf, TRIS_FRIENDLY_OFFSET, ILBC_BUF_SIZE);
	if ((res = fread(s->fr.data.ptr, 1, s->fr.datalen, s->f)) != s->fr.datalen) {
		if (res)
			tris_log(LOG_WARNING, "Short read (%d) (%s)!\n", res, strerror(errno));
		return NULL;
	}
	*whennext = s->fr.samples = ILBC_SAMPLES;
	return &s->fr;
}

static int ilbc_write(struct tris_filestream *fs, struct tris_frame *f)
{
	int res;
	if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Asked to write non-voice frame!\n");
		return -1;
	}
	if (f->subclass != TRIS_FORMAT_ILBC) {
		tris_log(LOG_WARNING, "Asked to write non-iLBC frame (%d)!\n", f->subclass);
		return -1;
	}
	if (f->datalen % 50) {
		tris_log(LOG_WARNING, "Invalid data length, %d, should be multiple of 50\n", f->datalen);
		return -1;
	}
	if ((res = fwrite(f->data.ptr, 1, f->datalen, fs->f)) != f->datalen) {
			tris_log(LOG_WARNING, "Bad write (%d/50): %s\n", res, strerror(errno));
			return -1;
	}
	return 0;
}

static int ilbc_seek(struct tris_filestream *fs, off_t sample_offset, int whence)
{
	long bytes;
	off_t min,cur,max,offset=0;
	min = 0;
	cur = ftello(fs->f);
	fseeko(fs->f, 0, SEEK_END);
	max = ftello(fs->f);
	
	bytes = ILBC_BUF_SIZE * (sample_offset / ILBC_SAMPLES);
	if (whence == SEEK_SET)
		offset = bytes;
	else if (whence == SEEK_CUR || whence == SEEK_FORCECUR)
		offset = cur + bytes;
	else if (whence == SEEK_END)
		offset = max - bytes;
	if (whence != SEEK_FORCECUR) {
		offset = (offset > max)?max:offset;
	}
	/* protect against seeking beyond begining. */
	offset = (offset < min)?min:offset;
	if (fseeko(fs->f, offset, SEEK_SET) < 0)
		return -1;
	return 0;
}

static int ilbc_trunc(struct tris_filestream *fs)
{
	/* Truncate file to current length */
	if (ftruncate(fileno(fs->f), ftello(fs->f)) < 0)
		return -1;
	return 0;
}

static off_t ilbc_tell(struct tris_filestream *fs)
{
	off_t offset = ftello(fs->f);
	return (offset/ILBC_BUF_SIZE)*ILBC_SAMPLES;
}

static const struct tris_format ilbc_f = {
	.name = "iLBC",
	.exts = "ilbc",
	.format = TRIS_FORMAT_ILBC,
	.write = ilbc_write,
	.seek = ilbc_seek,
	.trunc = ilbc_trunc,
	.tell = ilbc_tell,
	.read = ilbc_read,
	.buf_size = ILBC_BUF_SIZE + TRIS_FRIENDLY_OFFSET,
};

static int load_module(void)
{
	if (tris_format_register(&ilbc_f))
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	return tris_format_unregister(ilbc_f.name);
}	

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_LOAD_ORDER, "Raw iLBC data",
	.load = load_module,
	.unload = unload_module,
	.load_pri = 10,
);
