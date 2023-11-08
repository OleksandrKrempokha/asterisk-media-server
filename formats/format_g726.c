/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2004 - 2005, inAccess Networks
 *
 * Michael Manousos <manousos@inaccessnetworks.com>
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

/*!\file
 *
 * \brief Headerless G.726 (16/24/32/40kbps) data format for Trismedia.
 * 
 * File name extensions:
 * \arg 40 kbps: g726-40
 * \arg 32 kbps: g726-32
 * \arg 24 kbps: g726-24
 * \arg 16 kbps: g726-16
 * \ingroup formats
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 233694 $")

#include "trismedia/mod_format.h"
#include "trismedia/module.h"
#include "trismedia/endian.h"

#define	RATE_40		0
#define	RATE_32		1
#define	RATE_24		2
#define	RATE_16		3

/* We can only read/write chunks of FRAME_TIME ms G.726 data */
#define	FRAME_TIME	10	/* 10 ms size */

#define	BUF_SIZE	(5*FRAME_TIME)	/* max frame size in bytes ? */
/* Frame sizes in bytes */
static int frame_size[4] = { 
		FRAME_TIME * 5,
		FRAME_TIME * 4,
		FRAME_TIME * 3,
		FRAME_TIME * 2
};

struct g726_desc  {
	int rate;	/* RATE_* defines */
};

/*
 * Rate dependant format functions (open, rewrite)
 */
static int g726_open(struct tris_filestream *tmp, int rate)
{
	struct g726_desc *s = (struct g726_desc *)tmp->_private;
	s->rate = rate;
	tris_debug(1, "Created filestream G.726-%dk.\n", 40 - s->rate * 8);
	return 0;
}

static int g726_40_open(struct tris_filestream *s)
{
	return g726_open(s, RATE_40);
}

static int g726_32_open(struct tris_filestream *s)
{
	return g726_open(s, RATE_32);
}

static int g726_24_open(struct tris_filestream *s)
{
	return g726_open(s, RATE_24);
}

static int g726_16_open(struct tris_filestream *s)
{
	return g726_open(s, RATE_16);
}

static int g726_40_rewrite(struct tris_filestream *s, const char *comment)
{
	return g726_open(s, RATE_40);
}

static int g726_32_rewrite(struct tris_filestream *s, const char *comment)
{
	return g726_open(s, RATE_32);
}

static int g726_24_rewrite(struct tris_filestream *s, const char *comment)
{
	return g726_open(s, RATE_24);
}

static int g726_16_rewrite(struct tris_filestream *s, const char *comment)
{
	return g726_open(s, RATE_16);
}

/*
 * Rate independent format functions (read, write)
 */

static struct tris_frame *g726_read(struct tris_filestream *s, int *whennext)
{
	int res;
	struct g726_desc *fs = (struct g726_desc *)s->_private;

	/* Send a frame from the file to the appropriate channel */
	s->fr.frametype = TRIS_FRAME_VOICE;
	s->fr.subclass = TRIS_FORMAT_G726;
	s->fr.mallocd = 0;
	TRIS_FRAME_SET_BUFFER(&s->fr, s->buf, TRIS_FRIENDLY_OFFSET, frame_size[fs->rate]);
	s->fr.samples = 8 * FRAME_TIME;
	if ((res = fread(s->fr.data.ptr, 1, s->fr.datalen, s->f)) != s->fr.datalen) {
		if (res)
			tris_log(LOG_WARNING, "Short read (%d) (%s)!\n", res, strerror(errno));
		return NULL;
	}
	*whennext = s->fr.samples;
	return &s->fr;
}

static int g726_write(struct tris_filestream *s, struct tris_frame *f)
{
	int res;
	struct g726_desc *fs = (struct g726_desc *)s->_private;

	if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Asked to write non-voice frame!\n");
		return -1;
	}
	if (f->subclass != TRIS_FORMAT_G726) {
		tris_log(LOG_WARNING, "Asked to write non-G726 frame (%d)!\n", 
						f->subclass);
		return -1;
	}
	if (f->datalen % frame_size[fs->rate]) {
		tris_log(LOG_WARNING, "Invalid data length %d, should be multiple of %d\n", 
						f->datalen, frame_size[fs->rate]);
		return -1;
	}
	if ((res = fwrite(f->data.ptr, 1, f->datalen, s->f)) != f->datalen) {
		tris_log(LOG_WARNING, "Bad write (%d/%d): %s\n", 
				res, frame_size[fs->rate], strerror(errno));
			return -1;
	}
	return 0;
}

static int g726_seek(struct tris_filestream *fs, off_t sample_offset, int whence)
{
	return -1;
}

static int g726_trunc(struct tris_filestream *fs)
{
	return -1;
}

static off_t g726_tell(struct tris_filestream *fs)
{
	return -1;
}

static const struct tris_format f[] = {
	{
		.name = "g726-40",
		.exts = "g726-40",
		.format = TRIS_FORMAT_G726,
		.open = g726_40_open,
		.rewrite = g726_40_rewrite,
		.write = g726_write,
		.seek = g726_seek,
		.trunc = g726_trunc,
		.tell = g726_tell,
		.read = g726_read,
		.buf_size = BUF_SIZE + TRIS_FRIENDLY_OFFSET,
		.desc_size = sizeof(struct g726_desc),
	},
	{
		.name = "g726-32",
		.exts = "g726-32",
		.format = TRIS_FORMAT_G726,
		.open = g726_32_open,
		.rewrite = g726_32_rewrite,
		.write = g726_write,
		.seek = g726_seek,
		.trunc = g726_trunc,
		.tell = g726_tell,
		.read = g726_read,
		.buf_size = BUF_SIZE + TRIS_FRIENDLY_OFFSET,
		.desc_size = sizeof(struct g726_desc),
	},
	{
		.name = "g726-24",
		.exts = "g726-24",
		.format = TRIS_FORMAT_G726,
		.open = g726_24_open,
		.rewrite = g726_24_rewrite,
		.write = g726_write,
		.seek = g726_seek,
		.trunc = g726_trunc,
		.tell = g726_tell,
		.read = g726_read,
		.buf_size = BUF_SIZE + TRIS_FRIENDLY_OFFSET,
		.desc_size = sizeof(struct g726_desc),
	},
	{
		.name = "g726-16",
		.exts = "g726-16",
		.format = TRIS_FORMAT_G726,
		.open = g726_16_open,
		.rewrite = g726_16_rewrite,
		.write = g726_write,
		.seek = g726_seek,
		.trunc = g726_trunc,
		.tell = g726_tell,
		.read = g726_read,
		.buf_size = BUF_SIZE + TRIS_FRIENDLY_OFFSET,
		.desc_size = sizeof(struct g726_desc),
	},
	{	.format = 0 }	/* terminator */
};

static int load_module(void)
{
	int i;

	for (i = 0; f[i].format ; i++) {
		if (tris_format_register(&f[i])) {	/* errors are fatal */
			tris_log(LOG_WARNING, "Failed to register format %s.\n", f[i].name);
			return TRIS_MODULE_LOAD_FAILURE;
		}
	}
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	int i;

	for (i = 0; f[i].format ; i++) {
		if (tris_format_unregister(f[i].name))
			tris_log(LOG_WARNING, "Failed to unregister format %s.\n", f[i].name);
	}
	return(0);
}	

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_LOAD_ORDER, "Raw G.726 (16/24/32/40kbps) data",
	.load = load_module,
	.unload = unload_module,
	.load_pri = 10,
);
