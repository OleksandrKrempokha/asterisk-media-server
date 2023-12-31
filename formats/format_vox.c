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
 * \brief Flat, binary, ADPCM vox file format.
 * \arg File name extensions: vox
 * 
 * \ingroup formats
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 233694 $")

#include "trismedia/mod_format.h"
#include "trismedia/module.h"
#include "trismedia/endian.h"

#define BUF_SIZE	80		/* 80 bytes, 160 samples */
#define VOX_SAMPLES	160

static struct tris_frame *vox_read(struct tris_filestream *s, int *whennext)
{
	int res;

	/* Send a frame from the file to the appropriate channel */
	s->fr.frametype = TRIS_FRAME_VOICE;
	s->fr.subclass = TRIS_FORMAT_ADPCM;
	s->fr.mallocd = 0;
	TRIS_FRAME_SET_BUFFER(&s->fr, s->buf, TRIS_FRIENDLY_OFFSET, BUF_SIZE);
	if ((res = fread(s->fr.data.ptr, 1, s->fr.datalen, s->f)) < 1) {
		if (res)
			tris_log(LOG_WARNING, "Short read (%d) (%s)!\n", res, strerror(errno));
		return NULL;
	}
	*whennext = s->fr.samples = res * 2;
	s->fr.datalen = res;
	return &s->fr;
}

static int vox_write(struct tris_filestream *s, struct tris_frame *f)
{
	int res;
	if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Asked to write non-voice frame!\n");
		return -1;
	}
	if (f->subclass != TRIS_FORMAT_ADPCM) {
		tris_log(LOG_WARNING, "Asked to write non-ADPCM frame (%d)!\n", f->subclass);
		return -1;
	}
	if ((res = fwrite(f->data.ptr, 1, f->datalen, s->f)) != f->datalen) {
			tris_log(LOG_WARNING, "Bad write (%d/%d): %s\n", res, f->datalen, strerror(errno));
			return -1;
	}
	return 0;
}

static int vox_seek(struct tris_filestream *fs, off_t sample_offset, int whence)
{
     off_t offset=0,min,cur,max,distance;
	
     min = 0;
     cur = ftello(fs->f);
     fseeko(fs->f, 0, SEEK_END);
	 max = ftello(fs->f);
	 
     /* have to fudge to frame here, so not fully to sample */
     distance = sample_offset/2;
     if(whence == SEEK_SET)
	  offset = distance;
     else if(whence == SEEK_CUR || whence == SEEK_FORCECUR)
	  offset = distance + cur;
     else if(whence == SEEK_END)
	  offset = max - distance;
     if (whence != SEEK_FORCECUR) {
	  offset = (offset > max)?max:offset;
	  offset = (offset < min)?min:offset;
     }
     return fseeko(fs->f, offset, SEEK_SET);
}

static int vox_trunc(struct tris_filestream *fs)
{
     return ftruncate(fileno(fs->f), ftello(fs->f));
}

static off_t vox_tell(struct tris_filestream *fs)
{
     off_t offset;
     offset = ftello(fs->f) << 1;
     return offset; 
}

static const struct tris_format vox_f = {
	.name = "vox",
	.exts = "vox",
	.format = TRIS_FORMAT_ADPCM,
	.write = vox_write,
	.seek = vox_seek,
	.trunc = vox_trunc,
	.tell = vox_tell,
	.read = vox_read,
	.buf_size = BUF_SIZE + TRIS_FRIENDLY_OFFSET,
};

static int load_module(void)
{
	if (tris_format_register(&vox_f))
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	return tris_format_unregister(vox_f.name);
}	

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_LOAD_ORDER, "Dialogic VOX (ADPCM) File Format",
	.load = load_module,
	.unload = unload_module,
	.load_pri = 10,
);
