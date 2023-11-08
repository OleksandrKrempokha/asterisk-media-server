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
 * \brief Frame and codec manipulation routines
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 243247 $")

#include "trismedia/_private.h"
#include "trismedia/lock.h"
#include "trismedia/frame.h"
#include "trismedia/channel.h"
#include "trismedia/cli.h"
#include "trismedia/term.h"
#include "trismedia/utils.h"
#include "trismedia/threadstorage.h"
#include "trismedia/linkedlists.h"
#include "trismedia/translate.h"
#include "trismedia/dsp.h"
#include "trismedia/file.h"

#if !defined(LOW_MEMORY)
static void frame_cache_cleanup(void *data);

/*! \brief A per-thread cache of frame headers */
TRIS_THREADSTORAGE_CUSTOM(frame_cache, NULL, frame_cache_cleanup);

/*! 
 * \brief Maximum tris_frame cache size
 *
 * In most cases where the frame header cache will be useful, the size
 * of the cache will stay very small.  However, it is not always the case that
 * the same thread that allocates the frame will be the one freeing them, so
 * sometimes a thread will never have any frames in its cache, or the cache
 * will never be pulled from.  For the latter case, we limit the maximum size. 
 */ 
#define FRAME_CACHE_MAX_SIZE	10

/*! \brief This is just so tris_frames, a list head struct for holding a list of
 *  tris_frame structures, is defined. */
TRIS_LIST_HEAD_NOLOCK(tris_frames, tris_frame);

struct tris_frame_cache {
	struct tris_frames list;
	size_t size;
};
#endif

#define SMOOTHER_SIZE 8000

enum frame_type {
	TYPE_HIGH,     /* 0x0 */
	TYPE_LOW,      /* 0x1 */
	TYPE_SILENCE,  /* 0x2 */
	TYPE_DONTSEND  /* 0x3 */
};

#define TYPE_MASK 0x3

struct tris_smoother {
	int size;
	int format;
	int flags;
	float samplesperbyte;
	unsigned int opt_needs_swap:1;
	struct tris_frame f;
	struct timeval delivery;
	char data[SMOOTHER_SIZE];
	char framedata[SMOOTHER_SIZE + TRIS_FRIENDLY_OFFSET];
	struct tris_frame *opt;
	int len;
};

/*! \brief Definition of supported media formats (codecs) */
static const struct tris_format_list TRIS_FORMAT_LIST[] = {
	{ TRIS_FORMAT_G723_1 , "g723", 8000, "G.723.1", 20, 30, 300, 30, 30 },                                  /*!< G723.1 */
	{ TRIS_FORMAT_GSM, "gsm", 8000, "GSM", 33, 20, 300, 20, 20 },                                           /*!< codec_gsm.c */
	{ TRIS_FORMAT_ULAW, "ulaw", 8000, "G.711 u-law", 80, 10, 150, 10, 20 },                                 /*!< codec_ulaw.c */
	{ TRIS_FORMAT_ALAW, "alaw", 8000, "G.711 A-law", 80, 10, 150, 10, 20 },                                 /*!< codec_alaw.c */
	{ TRIS_FORMAT_G726, "g726", 8000, "G.726 RFC3551", 40, 10, 300, 10, 20 },                               /*!< codec_g726.c */
	{ TRIS_FORMAT_ADPCM, "adpcm" , 8000, "ADPCM", 40, 10, 300, 10, 20 },                                    /*!< codec_adpcm.c */
	{ TRIS_FORMAT_SLINEAR, "slin", 8000, "16 bit Signed Linear PCM", 160, 10, 70, 10, 20, TRIS_SMOOTHER_FLAG_BE }, /*!< Signed linear */
	{ TRIS_FORMAT_LPC10, "lpc10", 8000, "LPC10", 7, 20, 20, 20, 20 },                                       /*!< codec_lpc10.c */ 
	{ TRIS_FORMAT_G729A, "g729", 8000, "G.729A", 10, 10, 230, 10, 20, TRIS_SMOOTHER_FLAG_G729 },             /*!< Binary commercial distribution */
	{ TRIS_FORMAT_SPEEX, "speex", 8000, "SpeeX", 10, 10, 60, 10, 20 },                                      /*!< codec_speex.c */
	{ TRIS_FORMAT_SPEEX16, "speex16", 16000, "SpeeX 16khz", 10, 10, 60, 10, 20 },                          /*!< codec_speex.c */
	{ TRIS_FORMAT_ILBC, "ilbc", 8000, "iLBC", 50, 30, 30, 30, 30 },                                         /*!< codec_ilbc.c */ /* inc=30ms - workaround */
	{ TRIS_FORMAT_G726_AAL2, "g726aal2", 8000, "G.726 AAL2", 40, 10, 300, 10, 20 },                         /*!< codec_g726.c */
	{ TRIS_FORMAT_G722, "g722", 16000, "G722", 80, 10, 150, 10, 20 },                                       /*!< codec_g722.c */
	{ TRIS_FORMAT_SLINEAR16, "slin16", 16000, "16 bit Signed Linear PCM (16kHz)", 320, 10, 70, 10, 20, TRIS_SMOOTHER_FLAG_BE },    /*!< Signed linear (16kHz) */
	{ TRIS_FORMAT_JPEG, "jpeg", 0, "JPEG image"},                                                           /*!< See format_jpeg.c */
	{ TRIS_FORMAT_PNG, "png", 0, "PNG image"},                                                              /*!< PNG Image format */
	{ TRIS_FORMAT_H261, "h261", 0, "H.261 Video" },                                                         /*!< H.261 Video Passthrough */
	{ TRIS_FORMAT_H263, "h263", 0, "H.263 Video" },                                                         /*!< H.263 Passthrough support, see format_h263.c */
	{ TRIS_FORMAT_H263_PLUS, "h263p", 0, "H.263+ Video" },                                                  /*!< H.263plus passthrough support See format_h263.c */
	{ TRIS_FORMAT_H264, "h264", 0, "H.264 Video" },                                                         /*!< Passthrough support, see format_h263.c */
	{ TRIS_FORMAT_MP4_VIDEO, "mpeg4", 0, "MPEG4 Video" },                                                   /*!< Passthrough support for MPEG4 */
	{ TRIS_FORMAT_T140RED, "red", 1, "T.140 Realtime Text with redundancy"},                                /*!< Redundant T.140 Realtime Text */
	{ TRIS_FORMAT_T140, "t140", 0, "Passthrough T.140 Realtime Text" },                                     /*!< Passthrough support for T.140 Realtime Text */
	{ TRIS_FORMAT_SIREN7, "siren7", 16000, "ITU G.722.1 (Siren7, licensed from Polycom)", 80, 20, 80, 20, 20 },			/*!< Binary commercial distribution */
	{ TRIS_FORMAT_SIREN14, "siren14", 32000, "ITU G.722.1 Annex C, (Siren14, licensed from Polycom)", 120, 20, 80, 20, 20 },	/*!< Binary commercial distribution */
};

struct tris_frame tris_null_frame = { TRIS_FRAME_NULL, };

static int smoother_frame_feed(struct tris_smoother *s, struct tris_frame *f, int swap)
{
	if (s->flags & TRIS_SMOOTHER_FLAG_G729) {
		if (s->len % 10) {
			tris_log(LOG_NOTICE, "Dropping extra frame of G.729 since we already have a VAD frame at the end\n");
			return 0;
		}
	}
	if (swap) {
		tris_swapcopy_samples(s->data + s->len, f->data.ptr, f->samples);
	} else {
		memcpy(s->data + s->len, f->data.ptr, f->datalen);
	}
	/* If either side is empty, reset the delivery time */
	if (!s->len || tris_tvzero(f->delivery) || tris_tvzero(s->delivery)) {	/* XXX really ? */
		s->delivery = f->delivery;
	}
	s->len += f->datalen;

	return 0;
}

void tris_smoother_reset(struct tris_smoother *s, int bytes)
{
	memset(s, 0, sizeof(*s));
	s->size = bytes;
}

void tris_smoother_reconfigure(struct tris_smoother *s, int bytes)
{
	/* if there is no change, then nothing to do */
	if (s->size == bytes) {
		return;
	}
	/* set the new desired output size */
	s->size = bytes;
	/* if there is no 'optimized' frame in the smoother,
	 *   then there is nothing left to do
	 */
	if (!s->opt) {
		return;
	}
	/* there is an 'optimized' frame here at the old size,
	 * but it must now be put into the buffer so the data
	 * can be extracted at the new size
	 */
	smoother_frame_feed(s, s->opt, s->opt_needs_swap);
	s->opt = NULL;
}

struct tris_smoother *tris_smoother_new(int size)
{
	struct tris_smoother *s;
	if (size < 1)
		return NULL;
	if ((s = tris_malloc(sizeof(*s))))
		tris_smoother_reset(s, size);
	return s;
}

int tris_smoother_get_flags(struct tris_smoother *s)
{
	return s->flags;
}

void tris_smoother_set_flags(struct tris_smoother *s, int flags)
{
	s->flags = flags;
}

int tris_smoother_test_flag(struct tris_smoother *s, int flag)
{
	return (s->flags & flag);
}

int __tris_smoother_feed(struct tris_smoother *s, struct tris_frame *f, int swap)
{
	if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Huh?  Can't smooth a non-voice frame!\n");
		return -1;
	}
	if (!s->format) {
		s->format = f->subclass;
		s->samplesperbyte = (float)f->samples / (float)f->datalen;
	} else if (s->format != f->subclass) {
		tris_log(LOG_WARNING, "Smoother was working on %d format frames, now trying to feed %d?\n", s->format, f->subclass);
		return -1;
	}
	if (s->len + f->datalen > SMOOTHER_SIZE) {
		tris_log(LOG_WARNING, "Out of smoother space\n");
		return -1;
	}
	if (((f->datalen == s->size) ||
	     ((f->datalen < 10) && (s->flags & TRIS_SMOOTHER_FLAG_G729))) &&
	    !s->opt &&
	    !s->len &&
	    (f->offset >= TRIS_MIN_OFFSET)) {
		/* Optimize by sending the frame we just got
		   on the next read, thus eliminating the douple
		   copy */
		if (swap)
			tris_swapcopy_samples(f->data.ptr, f->data.ptr, f->samples);
		s->opt = f;
		s->opt_needs_swap = swap ? 1 : 0;
		return 0;
	}

	return smoother_frame_feed(s, f, swap);
}

struct tris_frame *tris_smoother_read(struct tris_smoother *s)
{
	struct tris_frame *opt;
	int len;

	/* IF we have an optimization frame, send it */
	if (s->opt) {
		if (s->opt->offset < TRIS_FRIENDLY_OFFSET)
			tris_log(LOG_WARNING, "Returning a frame of inappropriate offset (%d).\n",
							s->opt->offset);
		opt = s->opt;
		s->opt = NULL;
		return opt;
	}

	/* Make sure we have enough data */
	if (s->len < s->size) {
		/* Or, if this is a G.729 frame with VAD on it, send it immediately anyway */
		if (!((s->flags & TRIS_SMOOTHER_FLAG_G729) && (s->len % 10)))
			return NULL;
	}
	len = s->size;
	if (len > s->len)
		len = s->len;
	/* Make frame */
	s->f.frametype = TRIS_FRAME_VOICE;
	s->f.subclass = s->format;
	s->f.data.ptr = s->framedata + TRIS_FRIENDLY_OFFSET;
	s->f.offset = TRIS_FRIENDLY_OFFSET;
	s->f.datalen = len;
	/* Samples will be improper given VAD, but with VAD the concept really doesn't even exist */
	s->f.samples = len * s->samplesperbyte;	/* XXX rounding */
	s->f.delivery = s->delivery;
	/* Fill Data */
	memcpy(s->f.data.ptr, s->data, len);
	s->len -= len;
	/* Move remaining data to the front if applicable */
	if (s->len) {
		/* In principle this should all be fine because if we are sending
		   G.729 VAD, the next timestamp will take over anyawy */
		memmove(s->data, s->data + len, s->len);
		if (!tris_tvzero(s->delivery)) {
			/* If we have delivery time, increment it, otherwise, leave it at 0 */
			s->delivery = tris_tvadd(s->delivery, tris_samp2tv(s->f.samples, tris_format_rate(s->format)));
		}
	}
	/* Return frame */
	return &s->f;
}

void tris_smoother_free(struct tris_smoother *s)
{
	tris_free(s);
}

static struct tris_frame *tris_frame_header_new(void)
{
	struct tris_frame *f;

#if !defined(LOW_MEMORY)
	struct tris_frame_cache *frames;

	if ((frames = tris_threadstorage_get(&frame_cache, sizeof(*frames)))) {
		if ((f = TRIS_LIST_REMOVE_HEAD(&frames->list, frame_list))) {
			size_t mallocd_len = f->mallocd_hdr_len;
			memset(f, 0, sizeof(*f));
			f->mallocd_hdr_len = mallocd_len;
			f->mallocd = TRIS_MALLOCD_HDR;
			frames->size--;
			return f;
		}
	}
	if (!(f = tris_calloc_cache(1, sizeof(*f))))
		return NULL;
#else
	if (!(f = tris_calloc(1, sizeof(*f))))
		return NULL;
#endif

	f->mallocd_hdr_len = sizeof(*f);
	
	return f;
}

#if !defined(LOW_MEMORY)
static void frame_cache_cleanup(void *data)
{
	struct tris_frame_cache *frames = data;
	struct tris_frame *f;

	while ((f = TRIS_LIST_REMOVE_HEAD(&frames->list, frame_list)))
		tris_free(f);
	
	tris_free(frames);
}
#endif

static void __frame_free(struct tris_frame *fr, int cache)
{
	if (tris_test_flag(fr, TRIS_FRFLAG_FROM_TRANSLATOR)) {
		tris_translate_frame_freed(fr);
	} else if (tris_test_flag(fr, TRIS_FRFLAG_FROM_DSP)) {
		tris_dsp_frame_freed(fr);
	}

	if (!fr->mallocd)
		return;

#if !defined(LOW_MEMORY)
	if (cache && fr->mallocd == TRIS_MALLOCD_HDR) {
		/* Cool, only the header is malloc'd, let's just cache those for now 
		 * to keep things simple... */
		struct tris_frame_cache *frames;

		if ((frames = tris_threadstorage_get(&frame_cache, sizeof(*frames))) &&
		    (frames->size < FRAME_CACHE_MAX_SIZE)) {
			TRIS_LIST_INSERT_HEAD(&frames->list, fr, frame_list);
			frames->size++;
			return;
		}
	}
#endif
	
	if (fr->mallocd & TRIS_MALLOCD_DATA) {
		if (fr->data.ptr) 
			tris_free(fr->data.ptr - fr->offset);
	}
	if (fr->mallocd & TRIS_MALLOCD_SRC) {
		if (fr->src)
			tris_free((void *) fr->src);
	}
	if (fr->mallocd & TRIS_MALLOCD_HDR) {
		tris_free(fr);
	}
}


void tris_frame_free(struct tris_frame *frame, int cache)
{
	struct tris_frame *next;

	for (next = TRIS_LIST_NEXT(frame, frame_list);
	     frame;
	     frame = next, next = frame ? TRIS_LIST_NEXT(frame, frame_list) : NULL) {
		__frame_free(frame, cache);
	}
}

/*!
 * \brief 'isolates' a frame by duplicating non-malloc'ed components
 * (header, src, data).
 * On return all components are malloc'ed
 */
struct tris_frame *tris_frisolate(struct tris_frame *fr)
{
	struct tris_frame *out;
	void *newdata;

	/* if none of the existing frame is malloc'd, let tris_frdup() do it
	   since it is more efficient
	*/
	if (fr->mallocd == 0) {
		return tris_frdup(fr);
	}

	/* if everything is already malloc'd, we are done */
	if ((fr->mallocd & (TRIS_MALLOCD_HDR | TRIS_MALLOCD_SRC | TRIS_MALLOCD_DATA)) ==
	    (TRIS_MALLOCD_HDR | TRIS_MALLOCD_SRC | TRIS_MALLOCD_DATA)) {
		return fr;
	}

	if (!(fr->mallocd & TRIS_MALLOCD_HDR)) {
		/* Allocate a new header if needed */
		if (!(out = tris_frame_header_new())) {
			return NULL;
		}
		out->frametype = fr->frametype;
		out->subclass = fr->subclass;
		out->datalen = fr->datalen;
		out->samples = fr->samples;
		out->offset = fr->offset;
		/* Copy the timing data */
		tris_copy_flags(out, fr, TRIS_FRFLAG_HAS_TIMING_INFO);
		if (tris_test_flag(fr, TRIS_FRFLAG_HAS_TIMING_INFO)) {
			out->ts = fr->ts;
			out->len = fr->len;
			out->seqno = fr->seqno;
		}
	} else {
		tris_clear_flag(fr, TRIS_FRFLAG_FROM_TRANSLATOR);
		tris_clear_flag(fr, TRIS_FRFLAG_FROM_DSP);
		out = fr;
	}
	
	if (!(fr->mallocd & TRIS_MALLOCD_SRC) && fr->src) {
		if (!(out->src = tris_strdup(fr->src))) {
			if (out != fr) {
				tris_free(out);
			}
			return NULL;
		}
	} else {
		out->src = fr->src;
		fr->src = NULL;
		fr->mallocd &= ~TRIS_MALLOCD_SRC;
	}
	
	if (!(fr->mallocd & TRIS_MALLOCD_DATA))  {
		if (!fr->datalen) {
			out->data.uint32 = fr->data.uint32;
			out->mallocd = TRIS_MALLOCD_HDR | TRIS_MALLOCD_SRC;
			return out;
		}
		if (!(newdata = tris_malloc(fr->datalen + TRIS_FRIENDLY_OFFSET))) {
			if (out->src != fr->src) {
				tris_free((void *) out->src);
			}
			if (out != fr) {
				tris_free(out);
			}
			return NULL;
		}
		newdata += TRIS_FRIENDLY_OFFSET;
		out->offset = TRIS_FRIENDLY_OFFSET;
		out->datalen = fr->datalen;
		memcpy(newdata, fr->data.ptr, fr->datalen);
		out->data.ptr = newdata;
	} else {
		out->data = fr->data;
		memset(&fr->data, 0, sizeof(fr->data));
		fr->mallocd &= ~TRIS_MALLOCD_DATA;
	}

	out->mallocd = TRIS_MALLOCD_HDR | TRIS_MALLOCD_SRC | TRIS_MALLOCD_DATA;
	
	return out;
}

struct tris_frame *tris_frdup(const struct tris_frame *f)
{
	struct tris_frame *out = NULL;
	int len, srclen = 0;
	void *buf = NULL;

#if !defined(LOW_MEMORY)
	struct tris_frame_cache *frames;
#endif

	/* Start with standard stuff */
	len = sizeof(*out) + TRIS_FRIENDLY_OFFSET + f->datalen;
	/* If we have a source, add space for it */
	/*
	 * XXX Watch out here - if we receive a src which is not terminated
	 * properly, we can be easily attacked. Should limit the size we deal with.
	 */
	if (f->src)
		srclen = strlen(f->src);
	if (srclen > 0)
		len += srclen + 1;
	
#if !defined(LOW_MEMORY)
	if ((frames = tris_threadstorage_get(&frame_cache, sizeof(*frames)))) {
		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&frames->list, out, frame_list) {
			if (out->mallocd_hdr_len >= len) {
				size_t mallocd_len = out->mallocd_hdr_len;

				TRIS_LIST_REMOVE_CURRENT(frame_list);
				memset(out, 0, sizeof(*out));
				out->mallocd_hdr_len = mallocd_len;
				buf = out;
				frames->size--;
				break;
			}
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
	}
#endif

	if (!buf) {
		if (!(buf = tris_calloc_cache(1, len)))
			return NULL;
		out = buf;
		out->mallocd_hdr_len = len;
	}

	out->frametype = f->frametype;
	out->subclass = f->subclass;
	out->datalen = f->datalen;
	out->samples = f->samples;
	out->delivery = f->delivery;
	/* Set us as having malloc'd header only, so it will eventually
	   get freed. */
	out->mallocd = TRIS_MALLOCD_HDR;
	out->offset = TRIS_FRIENDLY_OFFSET;
	if (out->datalen) {
		out->data.ptr = buf + sizeof(*out) + TRIS_FRIENDLY_OFFSET;
		memcpy(out->data.ptr, f->data.ptr, out->datalen);	
	} else {
		out->data.uint32 = f->data.uint32;
	}
	if (srclen > 0) {
		/* This may seem a little strange, but it's to avoid a gcc (4.2.4) compiler warning */
		char *src;
		out->src = buf + sizeof(*out) + TRIS_FRIENDLY_OFFSET + f->datalen;
		src = (char *) out->src;
		/* Must have space since we allocated for it */
		strcpy(src, f->src);
	}
	tris_copy_flags(out, f, TRIS_FRFLAG_HAS_TIMING_INFO);
	out->ts = f->ts;
	out->len = f->len;
	out->seqno = f->seqno;
	out->promoter = f->promoter;
	return out;
}

void tris_swapcopy_samples(void *dst, const void *src, int samples)
{
	int i;
	unsigned short *dst_s = dst;
	const unsigned short *src_s = src;

	for (i = 0; i < samples; i++)
		dst_s[i] = (src_s[i]<<8) | (src_s[i]>>8);
}


const struct tris_format_list *tris_get_format_list_index(int idx) 
{
	return &TRIS_FORMAT_LIST[idx];
}

const struct tris_format_list *tris_get_format_list(size_t *size) 
{
	*size = ARRAY_LEN(TRIS_FORMAT_LIST);
	return TRIS_FORMAT_LIST;
}

char* tris_getformatname(int format)
{
	int x;
	char *ret = "unknown";
	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (TRIS_FORMAT_LIST[x].bits == format) {
			ret = TRIS_FORMAT_LIST[x].name;
			break;
		}
	}
	return ret;
}

char *tris_getformatname_multiple(char *buf, size_t size, int format)
{
	int x;
	unsigned len;
	char *start, *end = buf;

	if (!size)
		return buf;
	snprintf(end, size, "0x%x (", format);
	len = strlen(end);
	end += len;
	size -= len;
	start = end;
	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (TRIS_FORMAT_LIST[x].bits & format) {
			snprintf(end, size,"%s|",TRIS_FORMAT_LIST[x].name);
			len = strlen(end);
			end += len;
			size -= len;
		}
	}
	if (start == end)
		tris_copy_string(start, "nothing)", size);
	else if (size > 1)
		*(end -1) = ')';
	return buf;
}

static struct tris_codec_alias_table {
	char *alias;
	char *realname;
} tris_codec_alias_table[] = {
	{ "slinear", "slin"},
	{ "slinear16", "slin16"},
	{ "g723.1", "g723"},
	{ "g722.1", "siren7"},
	{ "g722.1c", "siren14"},
};

static const char *tris_expand_codec_alias(const char *in)
{
	int x;

	for (x = 0; x < ARRAY_LEN(tris_codec_alias_table); x++) {
		if (!strcmp(in,tris_codec_alias_table[x].alias))
			return tris_codec_alias_table[x].realname;
	}
	return in;
}

int tris_getformatbyname(const char *name)
{
	int x, all, format = 0;

	all = strcasecmp(name, "all") ? 0 : 1;
	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (all || 
			  !strcasecmp(TRIS_FORMAT_LIST[x].name,name) ||
			  !strcasecmp(TRIS_FORMAT_LIST[x].name, tris_expand_codec_alias(name))) {
			format |= TRIS_FORMAT_LIST[x].bits;
			if (!all)
				break;
		}
	}

	return format;
}

char *tris_codec2str(int codec)
{
	int x;
	char *ret = "unknown";
	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (TRIS_FORMAT_LIST[x].bits == codec) {
			ret = TRIS_FORMAT_LIST[x].desc;
			break;
		}
	}
	return ret;
}

static char *show_codecs(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int i, found=0;
	char hex[25];

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show codecs [audio|video|image]";
		e->usage = 
			"Usage: core show codecs [audio|video|image]\n"
			"       Displays codec mapping\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	
	if ((a->argc < 3) || (a->argc > 4))
		return CLI_SHOWUSAGE;

	if (!tris_opt_dont_warn)
		tris_cli(a->fd, "Disclaimer: this command is for informational purposes only.\n"
				"\tIt does not indicate anything about your configuration.\n");

	tris_cli(a->fd, "%11s %9s %10s   TYPE   %8s   %s\n","INT","BINARY","HEX","NAME","DESC");
	tris_cli(a->fd, "--------------------------------------------------------------------------------\n");
	if ((a->argc == 3) || (!strcasecmp(a->argv[3],"audio"))) {
		found = 1;
		for (i=0;i<13;i++) {
			snprintf(hex,25,"(0x%x)",1<<i);
			tris_cli(a->fd, "%11u (1 << %2d) %10s  audio   %8s   (%s)\n",1 << i,i,hex,tris_getformatname(1<<i),tris_codec2str(1<<i));
		}
	}

	if ((a->argc == 3) || (!strcasecmp(a->argv[3],"image"))) {
		found = 1;
		for (i=16;i<18;i++) {
			snprintf(hex,25,"(0x%x)",1<<i);
			tris_cli(a->fd, "%11u (1 << %2d) %10s  image   %8s   (%s)\n",1 << i,i,hex,tris_getformatname(1<<i),tris_codec2str(1<<i));
		}
	}

	if ((a->argc == 3) || (!strcasecmp(a->argv[3],"video"))) {
		found = 1;
		for (i=18;i<22;i++) {
			snprintf(hex,25,"(0x%x)",1<<i);
			tris_cli(a->fd, "%11u (1 << %2d) %10s  video   %8s   (%s)\n",1 << i,i,hex,tris_getformatname(1<<i),tris_codec2str(1<<i));
		}
	}

	if (!found)
		return CLI_SHOWUSAGE;
	else
		return CLI_SUCCESS;
}

static char *show_codec_n(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int codec, i, found=0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show codec";
		e->usage = 
			"Usage: core show codec <number>\n"
			"       Displays codec mapping\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	if (sscanf(a->argv[3], "%30d", &codec) != 1)
		return CLI_SHOWUSAGE;

	for (i = 0; i < 32; i++)
		if (codec & (1 << i)) {
			found = 1;
			tris_cli(a->fd, "%11u (1 << %2d)  %s\n",1 << i,i,tris_codec2str(1<<i));
		}

	if (!found)
		tris_cli(a->fd, "Codec %d not found\n", codec);

	return CLI_SUCCESS;
}

/*! Dump a frame for debugging purposes */
void tris_frame_dump(const char *name, struct tris_frame *f, char *prefix)
{
	const char noname[] = "unknown";
	char ftype[40] = "Unknown Frametype";
	char cft[80];
	char subclass[40] = "Unknown Subclass";
	char csub[80];
	char moreinfo[40] = "";
	char cn[60];
	char cp[40];
	char cmn[40];
	const char *message = "Unknown";

	if (!name)
		name = noname;


	if (!f) {
		tris_verbose("%s [ %s (NULL) ] [%s]\n", 
			term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
			term_color(cft, "HANGUP", COLOR_BRRED, COLOR_BLACK, sizeof(cft)), 
			term_color(cn, name, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
		return;
	}
	/* XXX We should probably print one each of voice and video when the format changes XXX */
	if (f->frametype == TRIS_FRAME_VOICE)
		return;
	if (f->frametype == TRIS_FRAME_VIDEO)
		return;
	switch(f->frametype) {
	case TRIS_FRAME_DTMF_BEGIN:
		strcpy(ftype, "DTMF Begin");
		subclass[0] = f->subclass;
		subclass[1] = '\0';
		break;
	case TRIS_FRAME_DTMF_END:
		strcpy(ftype, "DTMF End");
		subclass[0] = f->subclass;
		subclass[1] = '\0';
		break;
	case TRIS_FRAME_CONTROL:
		strcpy(ftype, "Control");
		switch(f->subclass) {
		case TRIS_CONTROL_HANGUP:
			strcpy(subclass, "Hangup");
			break;
		case TRIS_CONTROL_RING:
			strcpy(subclass, "Ring");
			break;
		case TRIS_CONTROL_RINGING:
			strcpy(subclass, "Ringing");
			break;
		case TRIS_CONTROL_ANSWER:
			strcpy(subclass, "Answer");
			break;
		case TRIS_CONTROL_BUSY:
			strcpy(subclass, "Busy");
			break;
		case TRIS_CONTROL_TAKEOFFHOOK:
			strcpy(subclass, "Take Off Hook");
			break;
		case TRIS_CONTROL_OFFHOOK:
			strcpy(subclass, "Line Off Hook");
			break;
		case TRIS_CONTROL_CONGESTION:
			strcpy(subclass, "Congestion");
			break;
		case TRIS_CONTROL_TIMEOUT:
			strcpy(subclass, "Timeout");
			break;
		case TRIS_CONTROL_FORBIDDEN:
			strcpy(subclass, "Forbidden");
			break;
		case TRIS_CONTROL_ROUTEFAIL:
			strcpy(subclass, "Route Fail");
			break;
		case TRIS_CONTROL_REJECTED:
			strcpy(subclass, "Declined");
			break;
		case TRIS_CONTROL_UNAVAILABLE:
			strcpy(subclass, "Unavailable");
			break;
		case TRIS_CONTROL_FLASH:
			strcpy(subclass, "Flash");
			break;
		case TRIS_CONTROL_WINK:
			strcpy(subclass, "Wink");
			break;
		case TRIS_CONTROL_OPTION:
			strcpy(subclass, "Option");
			break;
		case TRIS_CONTROL_RADIO_KEY:
			strcpy(subclass, "Key Radio");
			break;
		case TRIS_CONTROL_RADIO_UNKEY:
			strcpy(subclass, "Unkey Radio");
			break;
		case TRIS_CONTROL_HOLD:
			strcpy(subclass, "Hold");
			break;
		case TRIS_CONTROL_UNHOLD:
			strcpy(subclass, "Unhold");
			break;
		case TRIS_CONTROL_T38_PARAMETERS:
			if (f->datalen != sizeof(struct tris_control_t38_parameters)) {
				message = "Invalid";
			} else {
				struct tris_control_t38_parameters *parameters = f->data.ptr;
				enum tris_control_t38 state = parameters->request_response;
				if (state == TRIS_T38_REQUEST_NEGOTIATE)
					message = "Negotiation Requested";
				else if (state == TRIS_T38_REQUEST_TERMINATE)
					message = "Negotiation Request Terminated";
				else if (state == TRIS_T38_NEGOTIATED)
					message = "Negotiated";
				else if (state == TRIS_T38_TERMINATED)
					message = "Terminated";
				else if (state == TRIS_T38_REFUSED)
					message = "Refused";
			}
			snprintf(subclass, sizeof(subclass), "T38_Parameters/%s", message);
			break;
		case -1:
			strcpy(subclass, "Stop generators");
			break;
		default:
			snprintf(subclass, sizeof(subclass), "Unknown control '%d'", f->subclass);
		}
		break;
	case TRIS_FRAME_NULL:
		strcpy(ftype, "Null Frame");
		strcpy(subclass, "N/A");
		break;
	case TRIS_FRAME_IAX:
		/* Should never happen */
		strcpy(ftype, "IAX Specific");
		snprintf(subclass, sizeof(subclass), "IAX Frametype %d", f->subclass);
		break;
	case TRIS_FRAME_TEXT:
		strcpy(ftype, "Text");
		strcpy(subclass, "N/A");
		tris_copy_string(moreinfo, f->data.ptr, sizeof(moreinfo));
		break;
	case TRIS_FRAME_IMAGE:
		strcpy(ftype, "Image");
		snprintf(subclass, sizeof(subclass), "Image format %s\n", tris_getformatname(f->subclass));
		break;
	case TRIS_FRAME_HTML:
		strcpy(ftype, "HTML");
		switch(f->subclass) {
		case TRIS_HTML_URL:
			strcpy(subclass, "URL");
			tris_copy_string(moreinfo, f->data.ptr, sizeof(moreinfo));
			break;
		case TRIS_HTML_DATA:
			strcpy(subclass, "Data");
			break;
		case TRIS_HTML_BEGIN:
			strcpy(subclass, "Begin");
			break;
		case TRIS_HTML_END:
			strcpy(subclass, "End");
			break;
		case TRIS_HTML_LDCOMPLETE:
			strcpy(subclass, "Load Complete");
			break;
		case TRIS_HTML_NOSUPPORT:
			strcpy(subclass, "No Support");
			break;
		case TRIS_HTML_LINKURL:
			strcpy(subclass, "Link URL");
			tris_copy_string(moreinfo, f->data.ptr, sizeof(moreinfo));
			break;
		case TRIS_HTML_UNLINK:
			strcpy(subclass, "Unlink");
			break;
		case TRIS_HTML_LINKREJECT:
			strcpy(subclass, "Link Reject");
			break;
		default:
			snprintf(subclass, sizeof(subclass), "Unknown HTML frame '%d'\n", f->subclass);
			break;
		}
		break;
	case TRIS_FRAME_MODEM:
		strcpy(ftype, "Modem");
		switch (f->subclass) {
		case TRIS_MODEM_T38:
			strcpy(subclass, "T.38");
			break;
		case TRIS_MODEM_V150:
			strcpy(subclass, "V.150");
			break;
		default:
			snprintf(subclass, sizeof(subclass), "Unknown MODEM frame '%d'\n", f->subclass);
			break;
		}
		break;
	default:
		snprintf(ftype, sizeof(ftype), "Unknown Frametype '%d'", f->frametype);
	}
	if (!tris_strlen_zero(moreinfo))
		tris_verbose("%s [ TYPE: %s (%d) SUBCLASS: %s (%d) '%s' ] [%s]\n",  
			    term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
			    term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
			    f->frametype, 
			    term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
			    f->subclass, 
			    term_color(cmn, moreinfo, COLOR_BRGREEN, COLOR_BLACK, sizeof(cmn)),
			    term_color(cn, name, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
	else
		tris_verbose("%s [ TYPE: %s (%d) SUBCLASS: %s (%d) ] [%s]\n",  
			    term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
			    term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
			    f->frametype, 
			    term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
			    f->subclass, 
			    term_color(cn, name, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
}


/* Builtin Trismedia CLI-commands for debugging */
static struct tris_cli_entry my_clis[] = {
	TRIS_CLI_DEFINE(show_codecs, "Displays a list of codecs"),
	TRIS_CLI_DEFINE(show_codec_n, "Shows a specific codec"),
};

int init_framer(void)
{
	tris_cli_register_multiple(my_clis, ARRAY_LEN(my_clis));
	return 0;	
}

void tris_codec_pref_convert(struct tris_codec_pref *pref, char *buf, size_t size, int right) 
{
	int x, differential = (int) 'A', mem;
	char *from, *to;

	if (right) {
		from = pref->order;
		to = buf;
		mem = size;
	} else {
		to = pref->order;
		from = buf;
		mem = 32;
	}

	memset(to, 0, mem);
	for (x = 0; x < 32 ; x++) {
		if (!from[x])
			break;
		to[x] = right ? (from[x] + differential) : (from[x] - differential);
	}
}

int tris_codec_pref_string(struct tris_codec_pref *pref, char *buf, size_t size) 
{
	int x, codec; 
	size_t total_len, slen;
	char *formatname;
	
	memset(buf,0,size);
	total_len = size;
	buf[0] = '(';
	total_len--;
	for(x = 0; x < 32 ; x++) {
		if (total_len <= 0)
			break;
		if (!(codec = tris_codec_pref_index(pref,x)))
			break;
		if ((formatname = tris_getformatname(codec))) {
			slen = strlen(formatname);
			if (slen > total_len)
				break;
			strncat(buf, formatname, total_len - 1); /* safe */
			total_len -= slen;
		}
		if (total_len && x < 31 && tris_codec_pref_index(pref , x + 1)) {
			strncat(buf, "|", total_len - 1); /* safe */
			total_len--;
		}
	}
	if (total_len) {
		strncat(buf, ")", total_len - 1); /* safe */
		total_len--;
	}

	return size - total_len;
}

int tris_codec_pref_index(struct tris_codec_pref *pref, int idx)
{
	int slot = 0;

	if ((idx >= 0) && (idx < sizeof(pref->order))) {
		slot = pref->order[idx];
	}

	return slot ? TRIS_FORMAT_LIST[slot - 1].bits : 0;
}

/*! \brief Remove codec from pref list */
void tris_codec_pref_remove(struct tris_codec_pref *pref, int format)
{
	struct tris_codec_pref oldorder;
	int x, y = 0;
	int slot;
	int size;

	if (!pref->order[0])
		return;

	memcpy(&oldorder, pref, sizeof(oldorder));
	memset(pref, 0, sizeof(*pref));

	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		slot = oldorder.order[x];
		size = oldorder.framing[x];
		if (! slot)
			break;
		if (TRIS_FORMAT_LIST[slot-1].bits != format) {
			pref->order[y] = slot;
			pref->framing[y++] = size;
		}
	}
	
}

/*! \brief Append codec to list */
int tris_codec_pref_append(struct tris_codec_pref *pref, int format)
{
	int x, newindex = 0;

	tris_codec_pref_remove(pref, format);

	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (TRIS_FORMAT_LIST[x].bits == format) {
			newindex = x + 1;
			break;
		}
	}

	if (newindex) {
		for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
			if (!pref->order[x]) {
				pref->order[x] = newindex;
				break;
			}
		}
	}

	return x;
}

/*! \brief Prepend codec to list */
void tris_codec_pref_prepend(struct tris_codec_pref *pref, int format, int only_if_existing)
{
	int x, newindex = 0;

	/* First step is to get the codecs "index number" */
	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (TRIS_FORMAT_LIST[x].bits == format) {
			newindex = x + 1;
			break;
		}
	}
	/* Done if its unknown */
	if (!newindex)
		return;

	/* Now find any existing occurrence, or the end */
	for (x = 0; x < 32; x++) {
		if (!pref->order[x] || pref->order[x] == newindex)
			break;
	}

	if (only_if_existing && !pref->order[x])
		return;

	/* Move down to make space to insert - either all the way to the end,
	   or as far as the existing location (which will be overwritten) */
	for (; x > 0; x--) {
		pref->order[x] = pref->order[x - 1];
		pref->framing[x] = pref->framing[x - 1];
	}

	/* And insert the new entry */
	pref->order[0] = newindex;
	pref->framing[0] = 0; /* ? */
}

/*! \brief Set packet size for codec */
int tris_codec_pref_setsize(struct tris_codec_pref *pref, int format, int framems)
{
	int x, idx = -1;

	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (TRIS_FORMAT_LIST[x].bits == format) {
			idx = x;
			break;
		}
	}

	if (idx < 0)
		return -1;

	/* size validation */
	if (!framems)
		framems = TRIS_FORMAT_LIST[idx].def_ms;

	if (TRIS_FORMAT_LIST[idx].inc_ms && framems % TRIS_FORMAT_LIST[idx].inc_ms) /* avoid division by zero */
		framems -= framems % TRIS_FORMAT_LIST[idx].inc_ms;

	if (framems < TRIS_FORMAT_LIST[idx].min_ms)
		framems = TRIS_FORMAT_LIST[idx].min_ms;

	if (framems > TRIS_FORMAT_LIST[idx].max_ms)
		framems = TRIS_FORMAT_LIST[idx].max_ms;

	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (pref->order[x] == (idx + 1)) {
			pref->framing[x] = framems;
			break;
		}
	}

	return x;
}

/*! \brief Get packet size for codec */
struct tris_format_list tris_codec_pref_getsize(struct tris_codec_pref *pref, int format)
{
	int x, idx = -1, framems = 0;
	struct tris_format_list fmt = { 0, };

	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (TRIS_FORMAT_LIST[x].bits == format) {
			fmt = TRIS_FORMAT_LIST[x];
			idx = x;
			break;
		}
	}

	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		if (pref->order[x] == (idx + 1)) {
			framems = pref->framing[x];
			break;
		}
	}

	/* size validation */
	if (!framems)
		framems = TRIS_FORMAT_LIST[idx].def_ms;

	if (TRIS_FORMAT_LIST[idx].inc_ms && framems % TRIS_FORMAT_LIST[idx].inc_ms) /* avoid division by zero */
		framems -= framems % TRIS_FORMAT_LIST[idx].inc_ms;

	if (framems < TRIS_FORMAT_LIST[idx].min_ms)
		framems = TRIS_FORMAT_LIST[idx].min_ms;

	if (framems > TRIS_FORMAT_LIST[idx].max_ms)
		framems = TRIS_FORMAT_LIST[idx].max_ms;

	fmt.cur_ms = framems;

	return fmt;
}

/*! \brief Pick a codec */
int tris_codec_choose(struct tris_codec_pref *pref, int formats, int find_best)
{
	int x, ret = 0, slot;

	for (x = 0; x < ARRAY_LEN(TRIS_FORMAT_LIST); x++) {
		slot = pref->order[x];

		if (!slot)
			break;
		if (formats & TRIS_FORMAT_LIST[slot-1].bits) {
			ret = TRIS_FORMAT_LIST[slot-1].bits;
			break;
		}
	}
	if (ret & TRIS_FORMAT_AUDIO_MASK)
		return ret;

	tris_debug(4, "Could not find preferred codec - %s\n", find_best ? "Going for the best codec" : "Returning zero codec");

   	return find_best ? tris_best_codec(formats) : 0;
}

int tris_parse_allow_disallow(struct tris_codec_pref *pref, int *mask, const char *list, int allowing) 
{
	int errors = 0;
	char *parse = NULL, *this = NULL, *psize = NULL;
	int format = 0, framems = 0;

	parse = tris_strdupa(list);
	while ((this = strsep(&parse, ","))) {
		framems = 0;
		if ((psize = strrchr(this, ':'))) {
			*psize++ = '\0';
			tris_debug(1, "Packetization for codec: %s is %s\n", this, psize);
			framems = atoi(psize);
			if (framems < 0) {
				framems = 0;
				errors++;
				tris_log(LOG_WARNING, "Bad packetization value for codec %s\n", this);
			}
		}
		if (!(format = tris_getformatbyname(this))) {
			tris_log(LOG_WARNING, "Cannot %s unknown format '%s'\n", allowing ? "allow" : "disallow", this);
			errors++;
			continue;
		}

		if (mask) {
			if (allowing)
				*mask |= format;
			else
				*mask &= ~format;
		}

		/* Set up a preference list for audio. Do not include video in preferences 
		   since we can not transcode video and have to use whatever is offered
		 */
		if (pref && (format & TRIS_FORMAT_AUDIO_MASK)) {
			if (strcasecmp(this, "all")) {
				if (allowing) {
					tris_codec_pref_append(pref, format);
					tris_codec_pref_setsize(pref, format, framems);
				}
				else
					tris_codec_pref_remove(pref, format);
			} else if (!allowing) {
				memset(pref, 0, sizeof(*pref));
			}
		}
	}
	return errors;
}

static int g723_len(unsigned char buf)
{
	enum frame_type type = buf & TYPE_MASK;

	switch(type) {
	case TYPE_DONTSEND:
		return 0;
		break;
	case TYPE_SILENCE:
		return 4;
		break;
	case TYPE_HIGH:
		return 24;
		break;
	case TYPE_LOW:
		return 20;
		break;
	default:
		tris_log(LOG_WARNING, "Badly encoded frame (%d)\n", type);
	}
	return -1;
}

static int g723_samples(unsigned char *buf, int maxlen)
{
	int pos = 0;
	int samples = 0;
	int res;
	while(pos < maxlen) {
		res = g723_len(buf[pos]);
		if (res <= 0)
			break;
		samples += 240;
		pos += res;
	}
	return samples;
}

static unsigned char get_n_bits_at(unsigned char *data, int n, int bit)
{
	int byte = bit / 8;       /* byte containing first bit */
	int rem = 8 - (bit % 8);  /* remaining bits in first byte */
	unsigned char ret = 0;
	
	if (n <= 0 || n > 8)
		return 0;

	if (rem < n) {
		ret = (data[byte] << (n - rem));
		ret |= (data[byte + 1] >> (8 - n + rem));
	} else {
		ret = (data[byte] >> (rem - n));
	}

	return (ret & (0xff >> (8 - n)));
}

static int speex_get_wb_sz_at(unsigned char *data, int len, int bit)
{
	static int SpeexWBSubModeSz[] = {
		4, 36, 112, 192,
		352, 0, 0, 0 };
	int off = bit;
	unsigned char c;

	/* skip up to two wideband frames */
	if (((len * 8 - off) >= 5) && 
		get_n_bits_at(data, 1, off)) {
		c = get_n_bits_at(data, 3, off + 1);
		off += SpeexWBSubModeSz[c];

		if (((len * 8 - off) >= 5) && 
			get_n_bits_at(data, 1, off)) {
			c = get_n_bits_at(data, 3, off + 1);
			off += SpeexWBSubModeSz[c];

			if (((len * 8 - off) >= 5) && 
				get_n_bits_at(data, 1, off)) {
				tris_log(LOG_WARNING, "Encountered corrupt speex frame; too many wideband frames in a row.\n");
				return -1;
			}
		}

	}
	return off - bit;
}

static int speex_samples(unsigned char *data, int len)
{
	static int SpeexSubModeSz[] = {
		5, 43, 119, 160,
		220, 300, 364, 492, 
		79, 0, 0, 0,
		0, 0, 0, 0 };
	static int SpeexInBandSz[] = { 
		1, 1, 4, 4,
		4, 4, 4, 4,
		8, 8, 16, 16,
		32, 32, 64, 64 };
	int bit = 0;
	int cnt = 0;
	int off;
	unsigned char c;

	while ((len * 8 - bit) >= 5) {
		/* skip wideband frames */
		off = speex_get_wb_sz_at(data, len, bit);
		if (off < 0)  {
			tris_log(LOG_WARNING, "Had error while reading wideband frames for speex samples\n");
			break;
		}
		bit += off;

		if ((len * 8 - bit) < 5) {
			break;
		}

		/* get control bits */
		c = get_n_bits_at(data, 5, bit);
		bit += 5;

		if (c == 15) { 
			/* terminator */
			break; 
		} else if (c == 14) {
			/* in-band signal; next 4 bits contain signal id */
			c = get_n_bits_at(data, 4, bit);
			bit += 4;
			bit += SpeexInBandSz[c];
		} else if (c == 13) {
			/* user in-band; next 4 bits contain msg len */
			c = get_n_bits_at(data, 4, bit);
			bit += 4;
			/* after which it's 5-bit signal id + c bytes of data */
			bit += 5 + c * 8;
		} else if (c > 8) {
			/* unknown */
			break;
		} else {
			/* skip number bits for submode (less the 5 control bits) */
			bit += SpeexSubModeSz[c] - 5;
			cnt += 160; /* new frame */
		}
	}
	return cnt;
}

int tris_codec_get_samples(struct tris_frame *f)
{
	int samples = 0;

	switch(f->subclass) {
	case TRIS_FORMAT_SPEEX:
		samples = speex_samples(f->data.ptr, f->datalen);
		break;
	case TRIS_FORMAT_SPEEX16:
		samples = 2 * speex_samples(f->data.ptr, f->datalen);
		break;
	case TRIS_FORMAT_G723_1:
		samples = g723_samples(f->data.ptr, f->datalen);
		break;
	case TRIS_FORMAT_ILBC:
		samples = 240 * (f->datalen / 50);
		break;
	case TRIS_FORMAT_GSM:
		samples = 160 * (f->datalen / 33);
		break;
	case TRIS_FORMAT_G729A:
		samples = f->datalen * 8;
		break;
	case TRIS_FORMAT_SLINEAR:
	case TRIS_FORMAT_SLINEAR16:
		samples = f->datalen / 2;
		break;
	case TRIS_FORMAT_LPC10:
		/* assumes that the RTP packet contains one LPC10 frame */
		samples = 22 * 8;
		samples += (((char *)(f->data.ptr))[7] & 0x1) * 8;
		break;
	case TRIS_FORMAT_ULAW:
	case TRIS_FORMAT_ALAW:
		samples = f->datalen;
		break;
	case TRIS_FORMAT_G722:
	case TRIS_FORMAT_ADPCM:
	case TRIS_FORMAT_G726:
	case TRIS_FORMAT_G726_AAL2:
		samples = f->datalen * 2;
		break;
	case TRIS_FORMAT_SIREN7:
		/* 16,000 samples per second at 32kbps is 4,000 bytes per second */
		samples = f->datalen * (16000 / 4000);
		break;
	case TRIS_FORMAT_SIREN14:
		/* 32,000 samples per second at 48kbps is 6,000 bytes per second */
		samples = (int) f->datalen * ((float) 32000 / 6000);
		break;
	default:
		tris_log(LOG_WARNING, "Unable to calculate samples for format %s\n", tris_getformatname(f->subclass));
	}
	return samples;
}

int tris_codec_get_len(int format, int samples)
{
	int len = 0;

	/* XXX Still need speex, and lpc10 XXX */	
	switch(format) {
	case TRIS_FORMAT_G723_1:
		len = (samples / 240) * 20;
		break;
	case TRIS_FORMAT_ILBC:
		len = (samples / 240) * 50;
		break;
	case TRIS_FORMAT_GSM:
		len = (samples / 160) * 33;
		break;
	case TRIS_FORMAT_G729A:
		len = samples / 8;
		break;
	case TRIS_FORMAT_SLINEAR:
	case TRIS_FORMAT_SLINEAR16:
		len = samples * 2;
		break;
	case TRIS_FORMAT_ULAW:
	case TRIS_FORMAT_ALAW:
		len = samples;
		break;
	case TRIS_FORMAT_G722:
	case TRIS_FORMAT_ADPCM:
	case TRIS_FORMAT_G726:
	case TRIS_FORMAT_G726_AAL2:
		len = samples / 2;
		break;
	case TRIS_FORMAT_SIREN7:
		/* 16,000 samples per second at 32kbps is 4,000 bytes per second */
		len = samples / (16000 / 4000);
		break;
	case TRIS_FORMAT_SIREN14:
		/* 32,000 samples per second at 48kbps is 6,000 bytes per second */
		len = (int) samples / ((float) 32000 / 6000);
		break;
	default:
		tris_log(LOG_WARNING, "Unable to calculate sample length for format %s\n", tris_getformatname(format));
	}

	return len;
}

int tris_frame_adjust_volume(struct tris_frame *f, int adjustment)
{
	int count;
	short *fdata = f->data.ptr;
	short adjust_value = abs(adjustment);

	if ((f->frametype != TRIS_FRAME_VOICE) || (f->subclass != TRIS_FORMAT_SLINEAR))
		return -1;

	if (!adjustment)
		return 0;

	for (count = 0; count < f->samples; count++) {
		if (adjustment > 0) {
			tris_slinear_saturated_multiply(&fdata[count], &adjust_value);
		} else if (adjustment < 0) {
			tris_slinear_saturated_divide(&fdata[count], &adjust_value);
		}
	}

	return 0;
}

int tris_frame_slinear_sum(struct tris_frame *f1, struct tris_frame *f2)
{
	int count;
	short *data1, *data2;

	if ((f1->frametype != TRIS_FRAME_VOICE) || (f1->subclass != TRIS_FORMAT_SLINEAR))
		return -1;

	if ((f2->frametype != TRIS_FRAME_VOICE) || (f2->subclass != TRIS_FORMAT_SLINEAR))
		return -1;

	if (f1->samples != f2->samples)
		return -1;

	for (count = 0, data1 = f1->data.ptr, data2 = f2->data.ptr;
	     count < f1->samples;
	     count++, data1++, data2++)
		tris_slinear_saturated_add(data1, data2);

	return 0;
}
