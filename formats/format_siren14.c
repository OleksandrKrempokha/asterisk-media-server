/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Anthony Minessale and Digium, Inc.
 * Anthony Minessale (anthmct@yahoo.com)
 * Kevin P. Fleming <kpfleming@digium.com>
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
 * \brief ITU G.722.1 Annex C (Siren14, licensed from Polycom) format, 48kbps bitrate only
 * \arg File name extensions: siren14
 * \ingroup formats
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 233694 $")

#include "trismedia/mod_format.h"
#include "trismedia/module.h"
#include "trismedia/endian.h"

#define BUF_SIZE	120		/* 20 milliseconds == 120 bytes, 640 samples */
#define SAMPLES_TO_BYTES(x)	((typeof(x)) x / ((float) 640 / 120))
#define BYTES_TO_SAMPLES(x)	((typeof(x)) x * ((float) 640 / 120))

static struct tris_frame *siren14read(struct tris_filestream *s, int *whennext)
{
	int res;
	/* Send a frame from the file to the appropriate channel */

	s->fr.frametype = TRIS_FRAME_VOICE;
	s->fr.subclass = TRIS_FORMAT_SIREN14;
	s->fr.mallocd = 0;
	TRIS_FRAME_SET_BUFFER(&s->fr, s->buf, TRIS_FRIENDLY_OFFSET, BUF_SIZE);
	if ((res = fread(s->fr.data.ptr, 1, s->fr.datalen, s->f)) != s->fr.datalen) {
		if (res)
			tris_log(LOG_WARNING, "Short read (%d) (%s)!\n", res, strerror(errno));
		return NULL;
	}
	*whennext = s->fr.samples = BYTES_TO_SAMPLES(res);
	return &s->fr;
}

static int siren14write(struct tris_filestream *fs, struct tris_frame *f)
{
	int res;

	if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Asked to write non-voice frame!\n");
		return -1;
	}
	if (f->subclass != TRIS_FORMAT_SIREN14) {
		tris_log(LOG_WARNING, "Asked to write non-Siren14 frame (%d)!\n", f->subclass);
		return -1;
	}
	if ((res = fwrite(f->data.ptr, 1, f->datalen, fs->f)) != f->datalen) {
		tris_log(LOG_WARNING, "Bad write (%d/%d): %s\n", res, f->datalen, strerror(errno));
		return -1;
	}
	return 0;
}

static int siren14seek(struct tris_filestream *fs, off_t sample_offset, int whence)
{
	off_t offset = 0, min = 0, cur, max;

	sample_offset = SAMPLES_TO_BYTES(sample_offset);

	cur = ftello(fs->f);

	fseeko(fs->f, 0, SEEK_END);

	max = ftello(fs->f);

	if (whence == SEEK_SET)
		offset = sample_offset;
	else if (whence == SEEK_CUR || whence == SEEK_FORCECUR)
		offset = sample_offset + cur;
	else if (whence == SEEK_END)
		offset = max - sample_offset;

	if (whence != SEEK_FORCECUR)
		offset = (offset > max) ? max : offset;

	/* always protect against seeking past begining. */
	offset = (offset < min) ? min : offset;

	return fseeko(fs->f, offset, SEEK_SET);
}

static int siren14trunc(struct tris_filestream *fs)
{
	return ftruncate(fileno(fs->f), ftello(fs->f));
}

static off_t siren14tell(struct tris_filestream *fs)
{
	return BYTES_TO_SAMPLES(ftello(fs->f));
}

static const struct tris_format siren14_f = {
	.name = "siren14",
	.exts = "siren14",
	.format = TRIS_FORMAT_SIREN14,
	.write = siren14write,
	.seek = siren14seek,
	.trunc = siren14trunc,
	.tell = siren14tell,
	.read = siren14read,
	.buf_size = BUF_SIZE + TRIS_FRIENDLY_OFFSET,
};

static int load_module(void)
{
	if (tris_format_register(&siren14_f))
		return TRIS_MODULE_LOAD_DECLINE;

	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	return tris_format_unregister(siren14_f.name);
}	

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_LOAD_ORDER, "ITU G.722.1 Annex C (Siren14, licensed from Polycom)",
	.load = load_module,
	.unload = unload_module,
	.load_pri = 10,
);
