/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Generic File Format Support.
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 238632 $")

#include <dirent.h>
#include <sys/stat.h>
#include <math.h>

#include "trismedia/_private.h"	/* declare tris_file_init() */
#include "trismedia/paths.h"	/* use tris_config_TRIS_DATA_DIR */
#include "trismedia/mod_format.h"
#include "trismedia/cli.h"
#include "trismedia/channel.h"
#include "trismedia/sched.h"
#include "trismedia/translate.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"
#include "trismedia/pbx.h"
#include "trismedia/linkedlists.h"
#include "trismedia/module.h"
#include "trismedia/astobj2.h"

/*
 * The following variable controls the layout of localized sound files.
 * If 0, use the historical layout with prefix just before the filename
 * (i.e. digits/en/1.gsm , digits/it/1.gsm or default to digits/1.gsm),
 * if 1 put the prefix at the beginning of the filename
 * (i.e. en/digits/1.gsm, it/digits/1.gsm or default to digits/1.gsm).
 * The latter permits a language to be entirely in one directory.
 */
int tris_language_is_prefix = 1;

static TRIS_RWLIST_HEAD_STATIC(formats, tris_format);

int __tris_format_register(const struct tris_format *f, struct tris_module *mod)
{
	struct tris_format *tmp;

	TRIS_RWLIST_WRLOCK(&formats);
	TRIS_RWLIST_TRAVERSE(&formats, tmp, list) {
		if (!strcasecmp(f->name, tmp->name)) {
			TRIS_RWLIST_UNLOCK(&formats);
			tris_log(LOG_WARNING, "Tried to register '%s' format, already registered\n", f->name);
			return -1;
		}
	}
	if (!(tmp = tris_calloc(1, sizeof(*tmp)))) {
		TRIS_RWLIST_UNLOCK(&formats);
		return -1;
	}
	*tmp = *f;
	tmp->module = mod;
	if (tmp->buf_size) {
		/*
		 * Align buf_size properly, rounding up to the machine-specific
		 * alignment for pointers.
		 */
		struct _test_align { void *a, *b; } p;
		int align = (char *)&p.b - (char *)&p.a;
		tmp->buf_size = ((f->buf_size + align - 1) / align) * align;
	}
	
	memset(&tmp->list, 0, sizeof(tmp->list));

	TRIS_RWLIST_INSERT_HEAD(&formats, tmp, list);
	TRIS_RWLIST_UNLOCK(&formats);
	tris_verb(2, "Registered file format %s, extension(s) %s\n", f->name, f->exts);

	return 0;
}

int tris_format_unregister(const char *name)
{
	struct tris_format *tmp;
	int res = -1;

	TRIS_RWLIST_WRLOCK(&formats);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&formats, tmp, list) {
		if (!strcasecmp(name, tmp->name)) {
			TRIS_RWLIST_REMOVE_CURRENT(list);
			tris_free(tmp);
			res = 0;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&formats);

	if (!res)
		tris_verb(2, "Unregistered format %s\n", name);
	else
		tris_log(LOG_WARNING, "Tried to unregister format %s, already unregistered\n", name);

	return res;
}

int tris_stopstream(struct tris_channel *tmp)
{
	tris_channel_lock(tmp);

	/* Stop a running stream if there is one */
	if (tmp->stream) {
		tris_closestream(tmp->stream);
		tmp->stream = NULL;
		if (tmp->oldwriteformat && tris_set_write_format(tmp, tmp->oldwriteformat))
			tris_log(LOG_WARNING, "Unable to restore format back to %d\n", tmp->oldwriteformat);
	}
	/* Stop the video stream too */
	if (tmp->vstream != NULL) {
		tris_closestream(tmp->vstream);
		tmp->vstream = NULL;
	}

	tris_channel_unlock(tmp);

	return 0;
}

int tris_writestream(struct tris_filestream *fs, struct tris_frame *f)
{
	int res = -1;
	int alt = 0;
	if (f->frametype == TRIS_FRAME_VIDEO) {
		if (fs->fmt->format & TRIS_FORMAT_AUDIO_MASK) {
			/* This is the audio portion.  Call the video one... */
			if (!fs->vfs && fs->filename) {
				const char *type = tris_getformatname(f->subclass & ~0x1);
				fs->vfs = tris_writefile(fs->filename, type, NULL, fs->flags, 0, fs->mode);
				tris_debug(1, "Opened video output file\n");
			}
			if (fs->vfs)
				return tris_writestream(fs->vfs, f);
			/* else ignore */
			return 0;				
		} else {
			/* Might / might not have mark set */
			alt = 1;
		}
	} else if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Tried to write non-voice frame\n");
		return -1;
	}
	if (((fs->fmt->format | alt) & f->subclass) == f->subclass) {
		res =  fs->fmt->write(fs, f);
		if (res < 0) 
			tris_log(LOG_WARNING, "Natural write failed\n");
		else if (res > 0)
			tris_log(LOG_WARNING, "Huh??\n");
	} else {
		/* XXX If they try to send us a type of frame that isn't the normal frame, and isn't
		       the one we've setup a translator for, we do the "wrong thing" XXX */
		if (fs->trans && f->subclass != fs->lastwriteformat) {
			tris_translator_free_path(fs->trans);
			fs->trans = NULL;
		}
		if (!fs->trans) 
			fs->trans = tris_translator_build_path(fs->fmt->format, f->subclass);
		if (!fs->trans)
			tris_log(LOG_WARNING, "Unable to translate to format %s, source format %s\n",
				fs->fmt->name, tris_getformatname(f->subclass));
		else {
			struct tris_frame *trf;
			fs->lastwriteformat = f->subclass;
			/* Get the translated frame but don't consume the original in case they're using it on another stream */
			if ((trf = tris_translate(fs->trans, f, 0))) {
				struct tris_frame *cur;

				/* the translator may have returned multiple frames, so process them */
				for (cur = trf; cur; cur = TRIS_LIST_NEXT(cur, frame_list)) {
					if ((res = fs->fmt->write(fs, trf))) {
						tris_log(LOG_WARNING, "Translated frame write failed\n");
						break;
					}
				}
				tris_frfree(trf);
			} else {
				res = 0;
			}
		}
	}
	return res;
}

static int copy(const char *infile, const char *outfile)
{
	int ifd, ofd, len;
	char buf[4096];	/* XXX make it lerger. */

	if ((ifd = open(infile, O_RDONLY)) < 0) {
		tris_log(LOG_WARNING, "Unable to open %s in read-only mode\n", infile);
		return -1;
	}
	if ((ofd = open(outfile, O_WRONLY | O_TRUNC | O_CREAT, TRIS_FILE_MODE)) < 0) {
		tris_log(LOG_WARNING, "Unable to open %s in write-only mode\n", outfile);
		close(ifd);
		return -1;
	}
	while ( (len = read(ifd, buf, sizeof(buf)) ) ) {
		int res;
		if (len < 0) {
			tris_log(LOG_WARNING, "Read failed on %s: %s\n", infile, strerror(errno));
			break;
		}
		/* XXX handle partial writes */
		res = write(ofd, buf, len);
		if (res != len) {
			tris_log(LOG_WARNING, "Write failed on %s (%d of %d): %s\n", outfile, res, len, strerror(errno));
			len = -1; /* error marker */
			break;
		}
	}
	close(ifd);
	close(ofd);
	if (len < 0) {
		unlink(outfile);
		return -1; /* error */
	}
	return 0;	/* success */
}

/*!
 * \brief construct a filename. Absolute pathnames are preserved,
 * relative names are prefixed by the sounds/ directory.
 * The wav49 suffix is replaced by 'WAV'.
 * Returns a malloc'ed string to be freed by the caller.
 */
static char *build_filename(const char *filename, const char *ext)
{
	char *fn = NULL;

	if (!strcmp(ext, "wav49"))
		ext = "WAV";

	if (filename[0] == '/') {
		if (asprintf(&fn, "%s.%s", filename, ext) < 0) {
			tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
			fn = NULL;
		}
	} else {
		if (asprintf(&fn, "%s/sounds/%s.%s",
			     tris_config_TRIS_DATA_DIR, filename, ext) < 0) {
			tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
			fn = NULL;
		}
	}
	return fn;
}

/* compare type against the list 'exts' */
/* XXX need a better algorithm */
static int exts_compare(const char *exts, const char *type)
{
	char tmp[256];
	char *stringp = tmp, *ext;

	tris_copy_string(tmp, exts, sizeof(tmp));
	while ((ext = strsep(&stringp, "|"))) {
		if (!strcmp(ext, type))
			return 1;
	}

	return 0;
}

static void filestream_destructor(void *arg)
{
	char *cmd = NULL;
	size_t size = 0;
	struct tris_filestream *f = arg;

	/* Stop a running stream if there is one */
	if (f->owner) {
		if (f->fmt->format < TRIS_FORMAT_AUDIO_MASK) {
			f->owner->stream = NULL;
			TRIS_SCHED_DEL(f->owner->sched, f->owner->streamid);
			tris_settimeout(f->owner, 0, NULL, NULL);
		} else {
			f->owner->vstream = NULL;
			TRIS_SCHED_DEL(f->owner->sched, f->owner->vstreamid);
		}
	}
	/* destroy the translator on exit */
	if (f->trans)
		tris_translator_free_path(f->trans);

	if (f->realfilename && f->filename) {
			size = strlen(f->filename) + strlen(f->realfilename) + 15;
			cmd = alloca(size);
			memset(cmd,0,size);
			snprintf(cmd,size,"/bin/mv -f %s %s",f->filename,f->realfilename);
			tris_safe_system(cmd);
	}

	if (f->filename)
		free(f->filename);
	if (f->realfilename)
		free(f->realfilename);
	if (f->fmt->close) {
		void (*closefn)(struct tris_filestream *) = f->fmt->close;
		closefn(f);
	}
	if (f->f)
		fclose(f->f);
	if (f->vfs)
		tris_closestream(f->vfs);
	if (f->write_buffer) {
		tris_free(f->write_buffer);
	}
	if (f->orig_chan_name)
		free((void *) f->orig_chan_name);
	tris_module_unref(f->fmt->module);
}

static struct tris_filestream *get_filestream(struct tris_format *fmt, FILE *bfile)
{
	struct tris_filestream *s;

	int l = sizeof(*s) + fmt->buf_size + fmt->desc_size;	/* total allocation size */
	if ( (s = ao2_alloc(l, filestream_destructor)) == NULL)
		return NULL;
	s->fmt = fmt;
	s->f = bfile;

	if (fmt->desc_size)
		s->_private = ((char *)(s + 1)) + fmt->buf_size;
	if (fmt->buf_size)
		s->buf = (char *)(s + 1);
	s->fr.src = fmt->name;
	return s;
}

/*
 * Default implementations of open and rewrite.
 * Only use them if you don't have expensive stuff to do.
 */
enum wrap_fn { WRAP_OPEN, WRAP_REWRITE };

static int fn_wrapper(struct tris_filestream *s, const char *comment, enum wrap_fn mode)
{
	struct tris_format *f = s->fmt;
	int ret = -1;
	int (*openfn)(struct tris_filestream *s);

	if (mode == WRAP_OPEN && (openfn = f->open) && openfn(s))
		tris_log(LOG_WARNING, "Unable to open format %s\n", f->name);
	else if (mode == WRAP_REWRITE && f->rewrite && f->rewrite(s, comment))
		tris_log(LOG_WARNING, "Unable to rewrite format %s\n", f->name);
	else {
		/* preliminary checks succeed. update usecount */
		tris_module_ref(f->module);
		ret = 0;
	}
	return ret;
}

static int rewrite_wrapper(struct tris_filestream *s, const char *comment)
{
	return fn_wrapper(s, comment, WRAP_REWRITE);
}

static int open_wrapper(struct tris_filestream *s)
{
	return fn_wrapper(s, NULL, WRAP_OPEN);
}

enum file_action {
	ACTION_EXISTS = 1, /* return matching format if file exists, 0 otherwise */
	ACTION_DELETE,	/* delete file, return 0 on success, -1 on error */
	ACTION_RENAME,	/* rename file. return 0 on success, -1 on error */
	ACTION_OPEN,
	ACTION_COPY	/* copy file. return 0 on success, -1 on error */
};

/*!
 * \brief perform various actions on a file. Second argument
 * arg2 depends on the command:
 *	unused for EXISTS and DELETE
 *	destination file name (const char *) for COPY and RENAME
 * 	struct tris_channel * for OPEN
 * if fmt is NULL, OPEN will return the first matching entry,
 * whereas other functions will run on all matching entries.
 */
static int tris_filehelper(const char *filename, const void *arg2, const char *fmt, const enum file_action action)
{
	struct tris_format *f;
	int res = (action == ACTION_EXISTS) ? 0 : -1;

	TRIS_RWLIST_RDLOCK(&formats);
	/* Check for a specific format */
	TRIS_RWLIST_TRAVERSE(&formats, f, list) {
		char *stringp, *ext = NULL;

		if (fmt && !exts_compare(f->exts, fmt))
			continue;

		/* Look for a file matching the supported extensions.
		 * The file must exist, and for OPEN, must match
		 * one of the formats supported by the channel.
		 */
		stringp = tris_strdupa(f->exts);	/* this is in the stack so does not need to be freed */
		while ( (ext = strsep(&stringp, "|")) ) {
			struct stat st;
			char *fn = build_filename(filename, ext);

			if (fn == NULL)
				continue;

			if ( stat(fn, &st) ) { /* file not existent */
				tris_free(fn);
				continue;
			}
			/* for 'OPEN' we need to be sure that the format matches
			 * what the channel can process
			 */
			if (action == ACTION_OPEN) {
				struct tris_channel *chan = (struct tris_channel *)arg2;
				FILE *bfile;
				struct tris_filestream *s;

				if ( !(chan->writeformat & f->format) &&
				     !((f->format & TRIS_FORMAT_AUDIO_MASK && fmt) ||
					  (f->format & TRIS_FORMAT_VIDEO_MASK && fmt))) {
					tris_free(fn);
					continue;	/* not a supported format */
				}
				if ( (bfile = fopen(fn, "r")) == NULL) {
					tris_free(fn);
					continue;	/* cannot open file */
				}
				s = get_filestream(f, bfile);
				if (!s) {
					fclose(bfile);
					tris_free(fn);	/* cannot allocate descriptor */
					continue;
				}
				if (open_wrapper(s)) {
					tris_free(fn);
					tris_closestream(s);
					continue;	/* cannot run open on file */
				}
				if (st.st_size == 0) {
					tris_log(LOG_WARNING, "File %s detected to have zero size.\n", fn);
				}
				/* ok this is good for OPEN */
				res = 1;	/* found */
				s->lasttimeout = -1;
				s->fmt = f;
				s->trans = NULL;
				s->filename = NULL;
				if (s->fmt->format & TRIS_FORMAT_AUDIO_MASK) {
					if (chan->stream)
						tris_closestream(chan->stream);
					chan->stream = s;
				} else {
					if (chan->vstream)
						tris_closestream(chan->vstream);
					chan->vstream = s;
				}
				tris_free(fn);
				break;
			}
			switch (action) {
			case ACTION_OPEN:
				break;	/* will never get here */

			case ACTION_EXISTS:	/* return the matching format */
				res |= f->format;
				break;

			case ACTION_DELETE:
				if ( (res = unlink(fn)) )
					tris_log(LOG_WARNING, "unlink(%s) failed: %s\n", fn, strerror(errno));
				break;

			case ACTION_RENAME:
			case ACTION_COPY: {
				char *nfn = build_filename((const char *)arg2, ext);
				if (!nfn)
					tris_log(LOG_WARNING, "Out of memory\n");
				else {
					res = action == ACTION_COPY ? copy(fn, nfn) : rename(fn, nfn);
					if (res)
						tris_log(LOG_WARNING, "%s(%s,%s) failed: %s\n",
							action == ACTION_COPY ? "copy" : "rename",
							 fn, nfn, strerror(errno));
					tris_free(nfn);
				}
			    }
				break;

			default:
				tris_log(LOG_WARNING, "Unknown helper %d\n", action);
			}
			tris_free(fn);
		}
	}
	TRIS_RWLIST_UNLOCK(&formats);
	return res;
}

static int is_absolute_path(const char *filename)
{
	return filename[0] == '/';
}

static int fileexists_test(const char *filename, const char *fmt, const char *lang,
			   char *buf, int buflen)
{
	if (buf == NULL) {
		return -1;
	}

	if (tris_language_is_prefix && !is_absolute_path(filename)) { /* new layout */
		if (lang) {
			snprintf(buf, buflen, "%s/%s", lang, filename);
		} else {
			snprintf(buf, buflen, "%s", filename);
		}
	} else { /* old layout */
		strcpy(buf, filename);	/* first copy the full string */
		if (lang) {
			/* insert the language and suffix if needed */
			const char *c = strrchr(filename, '/');
			int offset = c ? c - filename + 1 : 0;	/* points right after the last '/' */
			snprintf(buf + offset, buflen - offset, "%s/%s", lang, filename + offset);
		}
	}

	return tris_filehelper(buf, NULL, fmt, ACTION_EXISTS);
}

/*!
 * \brief helper routine to locate a file with a given format
 * and language preference.
 * Try preflang, preflang with stripped '_' suffix, or NULL.
 * In the standard trismedia, language goes just before the last component.
 * In an alternative configuration, the language should be a prefix
 * to the actual filename.
 *
 * The last parameter(s) point to a buffer of sufficient size,
 * which on success is filled with the matching filename.
 */
static int fileexists_core(const char *filename, const char *fmt, const char *preflang,
			   char *buf, int buflen)
{
	int res = -1;
	char *lang = NULL;

	if (buf == NULL) {
		return -1;
	}

	/* We try languages in the following order:
	 *    preflang (may include dialect)
	 *    lang (preflang without dialect - if any)
	 *    <none>
	 *    default (unless the same as preflang or lang without dialect)
	 */

	/* Try preferred language */
	if (!tris_strlen_zero(preflang)) {
		/* try the preflang exactly as it was requested */
		if ((res = fileexists_test(filename, fmt, preflang, buf, buflen)) > 0) {
			return res;
		} else {
			/* try without a dialect */
			char *postfix = NULL;
			postfix = lang = tris_strdupa(preflang);

			strsep(&postfix, "_");
			if (postfix) {
				if ((res = fileexists_test(filename, fmt, lang, buf, buflen)) > 0) {
					return res;
				}
			}
		}
	}

	/* Try without any language */
	if ((res = fileexists_test(filename, fmt, NULL, buf, buflen)) > 0) {
		return res;
	}

	/* Finally try the default language unless it was already tried before */
	if ((tris_strlen_zero(preflang) || strcmp(preflang, DEFAULT_LANGUAGE)) && (tris_strlen_zero(lang) || strcmp(lang, DEFAULT_LANGUAGE))) {
		if ((res = fileexists_test(filename, fmt, DEFAULT_LANGUAGE, buf, buflen)) > 0) {
			return res;
		}
	}

	return 0;
}

struct tris_filestream *tris_openstream(struct tris_channel *chan, const char *filename, const char *preflang)
{
	return tris_openstream_full(chan, filename, preflang, 0);
}

struct tris_filestream *tris_openstream_full(struct tris_channel *chan, const char *filename, const char *preflang, int asis)
{
	/* 
	 * Use fileexists_core() to find a file in a compatible
	 * language and format, set up a suitable translator,
	 * and open the stream.
	 */
	int fmts, res, buflen;
	char *buf;

	if (!asis) {
		/* do this first, otherwise we detect the wrong writeformat */
		tris_stopstream(chan);
		if (chan->generator)
			tris_deactivate_generator(chan);
	}
	if (preflang == NULL)
		preflang = "";
	buflen = strlen(preflang) + strlen(filename) + 4;
	buf = alloca(buflen);
	if (buf == NULL)
		return NULL;
	fmts = fileexists_core(filename, NULL, preflang, buf, buflen);
	if (fmts > 0)
		fmts &= TRIS_FORMAT_AUDIO_MASK;
	if (fmts < 1) {
		tris_log(LOG_WARNING, "File %s does not exist in any format\n", filename);
		return NULL;
	}
	chan->oldwriteformat = chan->writeformat;
	/* Set the channel to a format we can work with */
	res = tris_set_write_format(chan, fmts);
 	res = tris_filehelper(buf, chan, NULL, ACTION_OPEN);
	if (res >= 0)
		return chan->stream;
	return NULL;
}

struct tris_filestream *tris_openvstream(struct tris_channel *chan, const char *filename, const char *preflang)
{
	/* As above, but for video. But here we don't have translators
	 * so we must enforce a format.
	 */
	unsigned int format;
	char *buf;
	int buflen;

	if (preflang == NULL)
		preflang = "";
	buflen = strlen(preflang) + strlen(filename) + 4;
	buf = alloca(buflen);
	if (buf == NULL)
		return NULL;

	for (format = TRIS_FORMAT_AUDIO_MASK + 1; format <= TRIS_FORMAT_VIDEO_MASK; format = format << 1) {
		int fd;
		const char *fmt;

		if (!(chan->nativeformats & format))
			continue;
		fmt = tris_getformatname(format);
		if ( fileexists_core(filename, fmt, preflang, buf, buflen) < 1)	/* no valid format */
			continue;
	 	fd = tris_filehelper(buf, chan, fmt, ACTION_OPEN);
		if (fd >= 0)
			return chan->vstream;
		tris_log(LOG_WARNING, "File %s has video but couldn't be opened\n", filename);
	}
	return NULL;
}

static struct tris_frame *read_frame(struct tris_filestream *s, int *whennext)
{
	struct tris_frame *fr, *new_fr;

	if (!s || !s->fmt) {
		return NULL;
	}

	if (!(fr = s->fmt->read(s, whennext))) {
		return NULL;
	}

	if (!(new_fr = tris_frisolate(fr))) {
		tris_frfree(fr);
		return NULL;
	}

	if (new_fr != fr) {
		tris_frfree(fr);
		fr = new_fr;
	}

	return fr;
}

struct tris_frame *tris_readframe(struct tris_filestream *s)
{
	int whennext = 0;

	return read_frame(s, &whennext);
}

enum fsread_res {
	FSREAD_FAILURE,
	FSREAD_SUCCESS_SCHED,
	FSREAD_SUCCESS_NOSCHED,
};

static int tris_fsread_audio(const void *data);

static enum fsread_res tris_readaudio_callback(struct tris_filestream *s)
{
	int whennext = 0;

	while (!whennext) {
		struct tris_frame *fr;

		if (s->orig_chan_name && strcasecmp(s->owner->name, s->orig_chan_name)) {
			goto return_failure;
		}

		fr = read_frame(s, &whennext);

		if (!fr /* stream complete */ || tris_write(s->owner, fr) /* error writing */) {
			if (fr) {
				tris_log(LOG_WARNING, "Failed to write frame\n");
				tris_frfree(fr);
			}
			goto return_failure;
		} 

		if (fr) {
			tris_frfree(fr);
		}
	}

	if (whennext != s->lasttimeout) {
		if (s->owner->timingfd > -1) {
			float samp_rate = (float) tris_format_rate(s->fmt->format);
			unsigned int rate;

			rate = (unsigned int) roundf(samp_rate / ((float) whennext));

			tris_settimeout(s->owner, rate, tris_fsread_audio, s);
		} else {
			s->owner->streamid = tris_sched_add(s->owner->sched, 
				whennext / (tris_format_rate(s->fmt->format) / 1000), tris_fsread_audio, s);
		}
		s->lasttimeout = whennext;
		return FSREAD_SUCCESS_NOSCHED;
	}
	return FSREAD_SUCCESS_SCHED;

return_failure:
	s->owner->streamid = -1;
	tris_settimeout(s->owner, 0, NULL, NULL);
	return FSREAD_FAILURE;
}

static int tris_fsread_audio(const void *data)
{
	struct tris_filestream *fs = (struct tris_filestream *)data;
	enum fsread_res res;

	res = tris_readaudio_callback(fs);

	if (res == FSREAD_SUCCESS_SCHED)
		return 1;
	
	return 0;
}

static int tris_fsread_video(const void *data);

static enum fsread_res tris_readvideo_callback(struct tris_filestream *s)
{
	int whennext = 0;

	while (!whennext) {
		struct tris_frame *fr = read_frame(s, &whennext);

		if (!fr /* stream complete */ || tris_write(s->owner, fr) /* error writing */) {
			if (fr) {
				tris_log(LOG_WARNING, "Failed to write frame\n");
				tris_frfree(fr);
			}
			s->owner->vstreamid = -1;
			return FSREAD_FAILURE;
		}

		if (fr) {
			tris_frfree(fr);
		}
	}

	if (whennext != s->lasttimeout) {
		s->owner->vstreamid = tris_sched_add(s->owner->sched, 
			whennext / (tris_format_rate(s->fmt->format) / 1000), 
			tris_fsread_video, s);
		s->lasttimeout = whennext;
		return FSREAD_SUCCESS_NOSCHED;
	}

	return FSREAD_SUCCESS_SCHED;
}

static int tris_fsread_video(const void *data)
{
	struct tris_filestream *fs = (struct tris_filestream *)data;
	enum fsread_res res;

	res = tris_readvideo_callback(fs);

	if (res == FSREAD_SUCCESS_SCHED)
		return 1;
	
	return 0;
}

int tris_applystream(struct tris_channel *chan, struct tris_filestream *s)
{
	s->owner = chan;
	return 0;
}

int tris_playstream(struct tris_filestream *s)
{
	enum fsread_res res;

	if (s->fmt->format & TRIS_FORMAT_AUDIO_MASK)
		res = tris_readaudio_callback(s);
	else
		res = tris_readvideo_callback(s);

	return (res == FSREAD_FAILURE) ? -1 : 0;
}

int tris_seekstream(struct tris_filestream *fs, off_t sample_offset, int whence)
{
	return fs->fmt->seek(fs, sample_offset, whence);
}

int tris_truncstream(struct tris_filestream *fs)
{
	return fs->fmt->trunc(fs);
}

off_t tris_tellstream(struct tris_filestream *fs)
{
	return fs->fmt->tell(fs);
}

int tris_stream_fastforward(struct tris_filestream *fs, off_t ms)
{
	return tris_seekstream(fs, ms * DEFAULT_SAMPLES_PER_MS, SEEK_CUR);
}

int tris_stream_rewind(struct tris_filestream *fs, off_t ms)
{
	return tris_seekstream(fs, -ms * DEFAULT_SAMPLES_PER_MS, SEEK_CUR);
}

int tris_closestream(struct tris_filestream *f)
{
	/* This used to destroy the filestream, but it now just decrements a refcount.
	 * We need to force the stream to quit queuing frames now, because we might
	 * change the writeformat, which could result in a subsequent write error, if
	 * the format is different. */

	/* Stop a running stream if there is one */
	if (f->owner) {
		if (f->fmt->format < TRIS_FORMAT_AUDIO_MASK) {
			f->owner->stream = NULL;
			TRIS_SCHED_DEL(f->owner->sched, f->owner->streamid);
			tris_settimeout(f->owner, 0, NULL, NULL);
		} else {
			f->owner->vstream = NULL;
			TRIS_SCHED_DEL(f->owner->sched, f->owner->vstreamid);
		}
	}

	ao2_ref(f, -1);
	return 0;
}


/*
 * Look the various language-specific places where a file could exist.
 */
int tris_fileexists(const char *filename, const char *fmt, const char *preflang)
{
	char *buf;
	int buflen;

	if (preflang == NULL)
		preflang = "";
	buflen = strlen(preflang) + strlen(filename) + 4;	/* room for everything */
	buf = alloca(buflen);
	if (buf == NULL)
		return 0;
	return fileexists_core(filename, fmt, preflang, buf, buflen);
}

int tris_filedelete(const char *filename, const char *fmt)
{
	return tris_filehelper(filename, NULL, fmt, ACTION_DELETE);
}

int tris_filerename(const char *filename, const char *filename2, const char *fmt)
{
	return tris_filehelper(filename, filename2, fmt, ACTION_RENAME);
}

int tris_filecopy(const char *filename, const char *filename2, const char *fmt)
{
	return tris_filehelper(filename, filename2, fmt, ACTION_COPY);
}

int tris_streamfile(struct tris_channel *chan, const char *filename, const char *preflang)
{
	struct tris_filestream *fs;
	struct tris_filestream *vfs=NULL;
	char fmt[256];
	int seekattempt;
	int res;

	fs = tris_openstream(chan, filename, preflang);
	if (!fs) {
		tris_log(LOG_WARNING, "Unable to open %s (format %s): %s\n", filename, tris_getformatname_multiple(fmt, sizeof(fmt), chan->nativeformats), strerror(errno));
		return -1;
	}

	/* check to see if there is any data present (not a zero length file),
	 * done this way because there is no where for tris_openstream_full to
	 * return the file had no data. */
	seekattempt = fseek(fs->f, -1, SEEK_END);
	if (!seekattempt)
		tris_seekstream(fs, 0, SEEK_SET);
	else
		return 0;

	vfs = tris_openvstream(chan, filename, preflang);
	if (vfs) {
		tris_debug(1, "Ooh, found a video stream, too, format %s\n", tris_getformatname(vfs->fmt->format));
	}

	if (tris_test_flag(chan, TRIS_FLAG_MASQ_NOSTREAM))
		fs->orig_chan_name = tris_strdup(chan->name);
	if (tris_applystream(chan, fs))
		return -1;
	if (vfs && tris_applystream(chan, vfs))
		return -1;
	res = tris_playstream(fs);
	if (!res && vfs)
		res = tris_playstream(vfs);
	tris_verb(3, "<%s> Playing '%s.%s' (language '%s')\n", chan->name, filename, tris_getformatname(chan->writeformat), preflang ? preflang : "default");

	return res;
}

struct tris_filestream *tris_readfile(const char *filename, const char *type, const char *comment, int flags, int check, mode_t mode)
{
	FILE *bfile;
	struct tris_format *f;
	struct tris_filestream *fs = NULL;
	char *fn;
	int format_found = 0;	

	TRIS_RWLIST_RDLOCK(&formats);

	TRIS_RWLIST_TRAVERSE(&formats, f, list) {
		fs = NULL;
		if (!exts_compare(f->exts, type))
			continue;
		else 
			format_found = 1;

		fn = build_filename(filename, type);
		errno = 0;
		bfile = fopen(fn, "r");

		if (!bfile || (fs = get_filestream(f, bfile)) == NULL || open_wrapper(fs) ) {
			tris_log(LOG_WARNING, "Unable to open %s\n", fn);
			if (fs) {
				tris_closestream(fs);
			}
			fs = NULL;
			bfile = NULL;
			tris_free(fn);
			break;				
		}
		/* found it */
		fs->trans = NULL;
		fs->fmt = f;
		fs->flags = flags;
		fs->mode = mode;
		fs->filename = tris_strdup(filename);
		fs->vfs = NULL;
		break;
	}

	TRIS_RWLIST_UNLOCK(&formats);
	if (!format_found)
		tris_log(LOG_WARNING, "No such format '%s'\n", type);

	return fs;
}

struct tris_filestream *tris_writefile(const char *filename, const char *type, const char *comment, int flags, int check, mode_t mode)
{
	int fd, myflags = 0;
	/* compiler claims this variable can be used before initialization... */
	FILE *bfile = NULL;
	struct tris_format *f;
	struct tris_filestream *fs = NULL;
	char *buf = NULL;
	size_t size = 0;
	int format_found = 0;

	TRIS_RWLIST_RDLOCK(&formats);

	/* set the O_TRUNC flag if and only if there is no O_APPEND specified */
	/* We really can't use O_APPEND as it will break WAV header updates */
	if (flags & O_APPEND) { 
		flags &= ~O_APPEND;
	} else {
		myflags = O_TRUNC;
	}
	
	myflags |= O_WRONLY | O_CREAT;

	/* XXX need to fix this - we should just do the fopen,
	 * not open followed by fdopen()
	 */
	TRIS_RWLIST_TRAVERSE(&formats, f, list) {
		char *fn, *orig_fn = NULL;
		if (fs)
			break;

		if (!exts_compare(f->exts, type))
			continue;
		else
			format_found = 1;

		fn = build_filename(filename, type);
		fd = open(fn, flags | myflags, mode);
		if (fd > -1) {
			/* fdopen() the resulting file stream */
			bfile = fdopen(fd, ((flags | myflags) & O_RDWR) ? "w+" : "w");
			if (!bfile) {
				tris_log(LOG_WARNING, "Whoa, fdopen failed: %s!\n", strerror(errno));
				close(fd);
				fd = -1;
			}
		}
		
		if (tris_opt_cache_record_files && (fd > -1)) {
			char *c;

			fclose(bfile);	/* this also closes fd */
			/*
			  We touch orig_fn just as a place-holder so other things (like vmail) see the file is there.
			  What we are really doing is writing to record_cache_dir until we are done then we will mv the file into place.
			*/
			orig_fn = tris_strdupa(fn);
			for (c = fn; *c; c++)
				if (*c == '/')
					*c = '_';

			size = strlen(fn) + strlen(record_cache_dir) + 2;
			buf = alloca(size);
			strcpy(buf, record_cache_dir);
			strcat(buf, "/");
			strcat(buf, fn);
			tris_free(fn);
			fn = buf;
			fd = open(fn, flags | myflags, mode);
			if (fd > -1) {
				/* fdopen() the resulting file stream */
				bfile = fdopen(fd, ((flags | myflags) & O_RDWR) ? "w+" : "w");
				if (!bfile) {
					tris_log(LOG_WARNING, "Whoa, fdopen failed: %s!\n", strerror(errno));
					close(fd);
					fd = -1;
				}
			}
		}
		if (fd > -1) {
			errno = 0;
			fs = get_filestream(f, bfile);
			if (!fs || rewrite_wrapper(fs, comment)) {
				tris_log(LOG_WARNING, "Unable to rewrite %s\n", fn);
				close(fd);
				if (orig_fn) {
					unlink(fn);
					unlink(orig_fn);
				}
				if (fs) {
					tris_closestream(fs);
					fs = NULL;
				}
				continue;
			}
			fs->trans = NULL;
			fs->fmt = f;
			fs->flags = flags;
			fs->mode = mode;
			if (orig_fn) {
				fs->realfilename = tris_strdup(orig_fn);
				fs->filename = tris_strdup(fn);
			} else {
				fs->realfilename = NULL;
				fs->filename = tris_strdup(filename);
			}
			fs->vfs = NULL;
			/* If truncated, we'll be at the beginning; if not truncated, then append */

			if ((fs->write_buffer = tris_malloc(32768))){
				setvbuf(fs->f, fs->write_buffer, _IOFBF, 32768);
			}

			f->seek(fs, 0, SEEK_END);
		} else if (errno != EEXIST) {
			tris_log(LOG_WARNING, "Unable to open file %s: %s\n", fn, strerror(errno));
			if (orig_fn)
				unlink(orig_fn);
		}
		/* if buf != NULL then fn is already free and pointing to it */
		if (!buf)
			tris_free(fn);
	}

	TRIS_RWLIST_UNLOCK(&formats);

	if (!format_found)
		tris_log(LOG_WARNING, "No such format '%s'\n", type);

	return fs;
}

/*!
 * \brief the core of all waitstream() functions
 */
static int waitstream_core(struct tris_channel *c, const char *breakon,
	const char *forward, const char *reverse, int skip_ms,
	int audiofd, int cmdfd,  const char *context)
{
	const char *orig_chan_name = NULL;
	int err = 0;

	if (!breakon)
		breakon = "";
	if (!forward)
		forward = "";
	if (!reverse)
		reverse = "";

	/* Switch the channel to end DTMF frame only. waitstream_core doesn't care about the start of DTMF. */
	tris_set_flag(c, TRIS_FLAG_END_DTMF_ONLY);

	if (tris_test_flag(c, TRIS_FLAG_MASQ_NOSTREAM))
		orig_chan_name = tris_strdupa(c->name);

	while (c->stream) {
		int res;
		int ms;

		if (orig_chan_name && strcasecmp(orig_chan_name, c->name)) {
			tris_stopstream(c);
			err = 1;
			break;
		}

		ms = tris_sched_wait(c->sched);

		if (ms < 0 && !c->timingfunc) {
			tris_stopstream(c);
			break;
		}
		if (ms < 0)
			ms = 1000;
		if (cmdfd < 0) {
			res = tris_waitfor(c, ms);
			if (res < 0) {
				tris_log(LOG_WARNING, "Select failed (%s)\n", strerror(errno));
				tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
				return res;
			}
		} else {
			int outfd;
			struct tris_channel *rchan = tris_waitfor_nandfds(&c, 1, &cmdfd, (cmdfd > -1) ? 1 : 0, NULL, &outfd, &ms);
			if (!rchan && (outfd < 0) && (ms)) {
				/* Continue */
				if (errno == EINTR)
					continue;
				tris_log(LOG_WARNING, "Wait failed (%s)\n", strerror(errno));
				tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
				return -1;
			} else if (outfd > -1) { /* this requires cmdfd set */
				/* The FD we were watching has something waiting */
				tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
				return 1;
			}
			/* if rchan is set, it is 'c' */
			res = rchan ? 1 : 0; /* map into 'res' values */
		}
		if (res > 0) {
			struct tris_frame *fr = tris_read(c);
			if (!fr) {
				tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
				return -1;
			}
			switch (fr->frametype) {
			case TRIS_FRAME_DTMF_END:
				if (context) {
					const char exten[2] = { fr->subclass, '\0' };
					if (tris_exists_extension(c, context, exten, 1, c->cid.cid_num)) {
						res = fr->subclass;
						tris_frfree(fr);
						tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
						return res;
					}
				} else {
					res = fr->subclass;
					if (strchr(forward, res)) {
						int eoftest;
						tris_stream_fastforward(c->stream, skip_ms);
						eoftest = fgetc(c->stream->f);
						if (feof(c->stream->f)) {
							tris_stream_rewind(c->stream, skip_ms);
						} else {
							ungetc(eoftest, c->stream->f);
						}
					} else if (strchr(reverse, res)) {
						tris_stream_rewind(c->stream, skip_ms);
					} else if (strchr(breakon, res)) {
						tris_frfree(fr);
						tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
						return res;
					}					
				}
				break;
			case TRIS_FRAME_CONTROL:
				switch (fr->subclass) {
				case TRIS_CONTROL_HANGUP:
				case TRIS_CONTROL_BUSY:
				case TRIS_CONTROL_CONGESTION:
				case TRIS_CONTROL_TIMEOUT:
				case TRIS_CONTROL_FORBIDDEN:
				case TRIS_CONTROL_ROUTEFAIL:
				case TRIS_CONTROL_REJECTED:
				case TRIS_CONTROL_UNAVAILABLE:
				case TRIS_CONTROL_REFER_INFO:
					tris_frfree(fr);
					tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);
					return -1;
				case TRIS_CONTROL_RINGING:
				case TRIS_CONTROL_ANSWER:
				case TRIS_CONTROL_VIDUPDATE:
				case TRIS_CONTROL_SRCUPDATE:
				case TRIS_CONTROL_SRCCHANGE:
				case TRIS_CONTROL_HOLD:
				case TRIS_CONTROL_UNHOLD:
				case -1:
					/* Unimportant */
					break;
				default:
					tris_log(LOG_WARNING, "Unexpected control subclass '%d'\n", fr->subclass);
				}
				break;
			case TRIS_FRAME_VOICE:
				/* Write audio if appropriate */
				if (audiofd > -1) {
					if (write(audiofd, fr->data.ptr, fr->datalen) < 0) {
						tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
					}
				}
			default:
				/* Ignore all others */
				break;
			}
			tris_frfree(fr);
		}
		tris_sched_runq(c->sched);
	}

	tris_clear_flag(c, TRIS_FLAG_END_DTMF_ONLY);

	return (err || c->_softhangup) ? -1 : 0;
}

int tris_waitstream_fr(struct tris_channel *c, const char *breakon, const char *forward, const char *reverse, int ms)
{
	return waitstream_core(c, breakon, forward, reverse, ms,
		-1 /* no audiofd */, -1 /* no cmdfd */, NULL /* no context */);
}

int tris_waitstream(struct tris_channel *c, const char *breakon)
{
	return waitstream_core(c, breakon, NULL, NULL, 0, -1, -1, NULL);
}

int tris_waitstream_full(struct tris_channel *c, const char *breakon, int audiofd, int cmdfd)
{
	return waitstream_core(c, breakon, NULL, NULL, 0,
		audiofd, cmdfd, NULL /* no context */);
}

int tris_waitstream_exten(struct tris_channel *c, const char *context)
{
	/* Waitstream, with return in the case of a valid 1 digit extension */
	/* in the current or specified context being pressed */

	if (!context)
		context = c->context;
	return waitstream_core(c, NULL, NULL, NULL, 0,
		-1, -1, context);
}

/*
 * if the file name is non-empty, try to play it.
 * Return 0 if success, -1 if error, digit if interrupted by a digit.
 * If digits == "" then we can simply check for non-zero.
 */
int tris_stream_and_wait(struct tris_channel *chan, const char *file, const char *digits)
{
	int res = 0;
	if (!tris_strlen_zero(file)) {
		res = tris_streamfile(chan, file, chan->language);
		if (!res) {
			res = tris_waitstream(chan, digits);
		}
	}
	return res;
} 

char *tris_format_str_reduce(char *fmts)
{
	struct tris_format *f;
	struct tris_format *fmts_ptr[TRIS_MAX_FORMATS];
	char *fmts_str[TRIS_MAX_FORMATS];
	char *stringp, *type;
	char *orig = fmts;
	int i, j, x, first, found = 0;
	int len = strlen(fmts) + 1;
	int res;

	if (TRIS_RWLIST_RDLOCK(&formats)) {
		tris_log(LOG_WARNING, "Unable to lock format list\n");
		return NULL;
	}

	stringp = tris_strdupa(fmts);

	for (x = 0; (type = strsep(&stringp, "|")) && x < TRIS_MAX_FORMATS; x++) {
		TRIS_RWLIST_TRAVERSE(&formats, f, list) {
			if (exts_compare(f->exts, type)) {
				found = 1;
				break;
			}
		}

		fmts_str[x] = type;
		if (found) {
			fmts_ptr[x] = f;
		} else {
			fmts_ptr[x] = NULL;
		}
	}
	TRIS_RWLIST_UNLOCK(&formats);

	first = 1;
	for (i = 0; i < x; i++) {
		/* ignore invalid entries */
		if (!fmts_ptr[i]) {
			tris_log(LOG_WARNING, "ignoring unknown format '%s'\n", fmts_str[i]);
			continue;
		}

		/* special handling for the first entry */
		if (first) {
			res = snprintf(fmts, len, "%s", fmts_str[i]);
			fmts += res;
			len -= res;
			first = 0;
			continue;
		}

		found = 0;
		for (j = 0; j < i; j++) {
			/* this is a duplicate */
			if (fmts_ptr[j] == fmts_ptr[i]) {
				found = 1;
				break;
			}
		}

		if (!found) {
			res = snprintf(fmts, len, "|%s", fmts_str[i]);
			fmts += res;
			len -= res;
		}
	}

	if (first) {
		tris_log(LOG_WARNING, "no known formats found in format list (%s)\n", orig);
		return NULL;
	}

	return orig;
}

static char *handle_cli_core_show_file_formats(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMAT "%-10s %-10s %-20s\n"
#define FORMAT2 "%-10s %-10s %-20s\n"
	struct tris_format *f;
	int count_fmt = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show file formats";
		e->usage =
			"Usage: core show file formats\n"
			"       Displays currently registered file formats (if any).\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, FORMAT, "Format", "Name", "Extensions");
	tris_cli(a->fd, FORMAT, "------", "----", "----------");

	TRIS_RWLIST_RDLOCK(&formats);
	TRIS_RWLIST_TRAVERSE(&formats, f, list) {
		tris_cli(a->fd, FORMAT2, tris_getformatname(f->format), f->name, f->exts);
		count_fmt++;
	}
	TRIS_RWLIST_UNLOCK(&formats);
	tris_cli(a->fd, "%d file formats registered.\n", count_fmt);
	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

struct tris_cli_entry cli_file[] = {
	TRIS_CLI_DEFINE(handle_cli_core_show_file_formats, "Displays file formats")
};

int tris_file_init(void)
{
	tris_cli_register_multiple(cli_file, ARRAY_LEN(cli_file));
	return 0;
}
