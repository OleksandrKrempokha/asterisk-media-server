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
 * \brief Convenient Application Routines
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 231689 $")

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#include <regex.h>
#include <sys/file.h> /* added this to allow to compile, sorry! */
#include <signal.h>
#include <sys/time.h>       /* for getrlimit(2) */
#include <sys/resource.h>   /* for getrlimit(2) */
#include <stdlib.h>         /* for closefrom(3) */
#ifdef HAVE_CAP
#include <sys/capability.h>
#endif /* HAVE_CAP */

#include "trismedia/paths.h"	/* use tris_config_TRIS_DATA_DIR */
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/file.h"
#include "trismedia/app.h"
#include "trismedia/dsp.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/indications.h"
#include "trismedia/linkedlists.h"
#include "trismedia/threadstorage.h"

TRIS_THREADSTORAGE_PUBLIC(tris_str_thread_global_buf);


#define TRIS_MAX_FORMATS 10

static TRIS_RWLIST_HEAD_STATIC(groups, tris_group_info);

/*!
 * \brief This function presents a dialtone and reads an extension into 'collect'
 * which must be a pointer to a **pre-initialized** array of char having a
 * size of 'size' suitable for writing to.  It will collect no more than the smaller
 * of 'maxlen' or 'size' minus the original strlen() of collect digits.
 * \param chan struct.
 * \param context
 * \param collect
 * \param size
 * \param maxlen
 * \param timeout timeout in seconds
 *
 * \return 0 if extension does not exist, 1 if extension exists
*/
int tris_app_dtget(struct tris_channel *chan, const char *context, char *collect, size_t size, int maxlen, int timeout)
{
	struct tris_tone_zone_sound *ts;
	int res = 0, x = 0;

	if (maxlen > size) {
		maxlen = size;
	}

	if (!timeout && chan->pbx) {
		timeout = chan->pbx->dtimeoutms / 1000.0;
	} else if (!timeout) {
		timeout = 5;
	}

	if ((ts = tris_get_indication_tone(chan->zone, "dial"))) {
		res = tris_playtones_start(chan, 0, ts->data, 0);
		ts = tris_tone_zone_sound_unref(ts);
	} else {
		tris_log(LOG_NOTICE, "Huh....? no dial for indications?\n");
	}

	for (x = strlen(collect); x < maxlen; ) {
		res = tris_waitfordigit(chan, timeout);
		if (!tris_ignore_pattern(context, collect)) {
			tris_playtones_stop(chan);
		}
		if (res < 1) {
			break;
		}
		if (res == '#') {
			break;
		}
		collect[x++] = res;
		if (!tris_matchmore_extension(chan, context, collect, 1, chan->cid.cid_num)) {
			break;
		}
	}

	if (res >= 0) {
		res = tris_exists_extension(chan, context, collect, 1, chan->cid.cid_num) ? 1 : 0;
	}

	return res;
}

/*!
 * \brief tris_app_getdata
 * \param c The channel to read from
 * \param prompt The file to stream to the channel
 * \param s The string to read in to.  Must be at least the size of your length
 * \param maxlen How many digits to read (maximum)
 * \param timeout set timeout to 0 for "standard" timeouts. Set timeout to -1 for 
 *      "ludicrous time" (essentially never times out) */
enum tris_getdata_result tris_app_getdata(struct tris_channel *c, const char *prompt, char *s, int maxlen, int timeout)
{
	int res = 0, to, fto;
	char *front, *filename;

	/* XXX Merge with full version? XXX */

	if (maxlen)
		s[0] = '\0';

	if (!prompt)
		prompt = "";

	filename = tris_strdupa(prompt);
	while ((front = strsep(&filename, "&"))) {
		if (!tris_strlen_zero(front)) {
			res = tris_streamfile(c, front, c->language);
			if (res)
				continue;
		}
		if (tris_strlen_zero(filename)) {
			/* set timeouts for the last prompt */
			fto = c->pbx ? c->pbx->rtimeoutms : 6000;
			to = c->pbx ? c->pbx->dtimeoutms : 2000;

			if (timeout > 0) {
				fto = to = timeout;
			}
			if (timeout < 0) {
				fto = to = 1000000000;
			}
		} else {
			/* there is more than one prompt, so
			 * get rid of the long timeout between
			 * prompts, and make it 50ms */
			fto = 50;
			to = c->pbx ? c->pbx->dtimeoutms : 2000;
		}
		res = tris_readstring(c, s, maxlen, to, fto, "#");
		if (res == TRIS_GETDATA_EMPTY_END_TERMINATED) {
			return res;
		}
		if (!tris_strlen_zero(s)) {
			return res;
		}
	}

	return res;
}

int meetme_readstring(struct tris_channel *c, char *s, int len, int timeout, int ftimeout, char *enders)
{
	return meetme_readstring_full(c, s, len, timeout, ftimeout, enders, -1, -1);
}

int meetme_readstring_full(struct tris_channel *c, char *s, int len, int timeout, int ftimeout, char *enders, int audiofd, int ctrlfd)
{
	int pos = 0;	/* index in the buffer where we accumulate digits */
	int to = ftimeout;

	/* Stop if we're a zombie or need a soft hangup */
	if (tris_test_flag(c, TRIS_FLAG_ZOMBIE) || tris_check_hangup(c))
		return -1;
	if (!len)
		return -1;
	for (;;) {
		int d;
		if (c->stream) {
			d = tris_waitstream_full(c, TRIS_DIGIT_ANY, audiofd, ctrlfd);
			tris_stopstream(c);
			usleep(1000);
			if (!d)
				d = tris_waitfordigit_full(c, to, audiofd, ctrlfd);
		} else {
			d = tris_waitfordigit_full(c, to, audiofd, ctrlfd);
		}
		if (d < 0)
			return -1;
		if (d == 0) {
			s[pos]='\0';
			return 1;
		}
		if (d == 1) {
			s[pos]='\0';
			return 2;
		}
		if (!strchr(enders, d))
			s[pos++] = d;
		if (strchr(enders, d) || (pos >= len)) {
			s[pos]='\0';
			return d;
		}
		to = timeout;
	}
	/* Never reached */
	return 0;
}

int tris_meetme_dialout_getdata(struct tris_channel *c, const char *prompt, char *s, int maxlen, int timeout, char *endcodes)
{
	int res = 0, to, fto;
	char *front, *filename;

	/* XXX Merge with full version? XXX */
	
	if (maxlen)
		s[0] = '\0';

	if (!prompt)
		prompt = "";

	filename = tris_strdupa(prompt);
	while ((front = strsep(&filename, "&"))) {
		if (!tris_strlen_zero(front)) {
			res = tris_streamfile(c, front, c->language);
			if (res)
				continue;
		}
		if (tris_strlen_zero(filename)) {
			/* set timeouts for the last prompt */
			fto = c->pbx ? c->pbx->rtimeoutms * 1000 : 6000;
			to = c->pbx ? c->pbx->dtimeoutms * 1000 : 2000;

			if (timeout > 0) 
				fto = to = timeout;
			if (timeout < 0) 
				fto = to = 1000000000;
		} else {
			/* there is more than one prompt, so
			   get rid of the long timeout between 
			   prompts, and make it 50ms */
			fto = 50;
			to = c->pbx ? c->pbx->dtimeoutms * 1000 : 2000;
		}
		if(tris_strlen_zero(endcodes))
			res = meetme_readstring(c, s, maxlen, to, fto, "#");
		else
			res = meetme_readstring(c, s, maxlen, to, fto, endcodes);
		if (!tris_strlen_zero(s))
			return res;
	}
	
	return res;
}


/* The lock type used by tris_lock_path() / tris_unlock_path() */
static enum TRIS_LOCK_TYPE tris_lock_type = TRIS_LOCK_TYPE_LOCKFILE;

int tris_app_getdata_full(struct tris_channel *c, char *prompt, char *s, int maxlen, int timeout, int audiofd, int ctrlfd)
{
	int res, to = 2000, fto = 6000;

	if (!tris_strlen_zero(prompt)) {
		res = tris_streamfile(c, prompt, c->language);
		if (res < 0) {
			return res;
		}
	}

	if (timeout > 0) {
		fto = to = timeout;
	}
	if (timeout < 0) {
		fto = to = 1000000000;
	}

	res = tris_readstring_full(c, s, maxlen, to, fto, "#", audiofd, ctrlfd);

	return res;
}

static int (*tris_has_voicemail_func)(const char *mailbox, const char *folder) = NULL;
static int (*tris_inboxcount_func)(const char *mailbox, int *newmsgs, int *oldmsgs) = NULL;
static int (*tris_inboxcount2_func)(const char *mailbox, int *urgentmsgs, int *newmsgs, int *oldmsgs) = NULL;
static int (*tris_sayname_func)(struct tris_channel *chan, const char *mailbox, const char *context) = NULL;
static int (*tris_messagecount_func)(const char *context, const char *mailbox, const char *folder) = NULL;
static int (*tris_getvmlist_func)(const char *mailbox, const char*folder, char *vmlist)=NULL;
static int (*tris_managemailbox_func)(const char * mailbox, int folder, int *msglist, int msgcount, const char * command, char *result)=NULL;

void tris_install_vm_functions(int (*has_voicemail_func)(const char *mailbox, const char *folder),
			      int (*inboxcount_func)(const char *mailbox, int *newmsgs, int *oldmsgs),
			      int (*inboxcount2_func)(const char *mailbox, int *urgentmsgs, int *newmsgs, int *oldmsgs),
			      int (*messagecount_func)(const char *context, const char *mailbox, const char *folder),
			      int (*sayname_func)(struct tris_channel *chan, const char *mailbox, const char *context),
			      int (*getvmlist_func)(const char *mailbox, const char*folder, char *vmlist),
				  int (*managemailbox_func)(const char * mailbox, int folder, int * msglist, int msgcount, const char * command, char *result))
{
	tris_has_voicemail_func = has_voicemail_func;
	tris_inboxcount_func = inboxcount_func;
	tris_inboxcount2_func = inboxcount2_func;
	tris_messagecount_func = messagecount_func;
	tris_sayname_func = sayname_func;
	tris_getvmlist_func = getvmlist_func;
	tris_managemailbox_func = managemailbox_func;
}

void tris_uninstall_vm_functions(void)
{
	tris_has_voicemail_func = NULL;
	tris_inboxcount_func = NULL;
	tris_inboxcount2_func = NULL;
	tris_messagecount_func = NULL;
	tris_sayname_func = NULL;
}

int tris_app_has_voicemail(const char *mailbox, const char *folder)
{
	static int warned = 0;
	if (tris_has_voicemail_func) {
		return tris_has_voicemail_func(mailbox, folder);
	}

	if (warned++ % 10 == 0) {
		tris_verb(3, "Message check requested for mailbox %s/folder %s but voicemail not loaded.\n", mailbox, folder ? folder : "INBOX");
	}
	return 0;
}


int tris_app_inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	static int warned = 0;
	if (newmsgs) {
		*newmsgs = 0;
	}
	if (oldmsgs) {
		*oldmsgs = 0;
	}
	if (tris_inboxcount_func) {
		return tris_inboxcount_func(mailbox, newmsgs, oldmsgs);
	}

	if (warned++ % 10 == 0) {
		tris_verb(3, "Message count requested for mailbox %s but voicemail not loaded.\n", mailbox);
	}

	return 0;
}

int tris_app_inboxcount2(const char *mailbox, int *urgentmsgs, int *newmsgs, int *oldmsgs)
{
	static int warned = 0;
	if (newmsgs) {
		*newmsgs = 0;
	}
	if (oldmsgs) {
		*oldmsgs = 0;
	}
	if (urgentmsgs) {
		*urgentmsgs = 0;
	}
	if (tris_inboxcount_func) {
		return tris_inboxcount2_func(mailbox, urgentmsgs, newmsgs, oldmsgs);
	}

	if (warned++ % 10 == 0) {
		tris_verb(3, "Message count requested for mailbox %s but voicemail not loaded.\n", mailbox);
	}

	return 0;
}

int tris_app_sayname(struct tris_channel *chan, const char *mailbox, const char *context)
{
	if (tris_sayname_func) {
		return tris_sayname_func(chan, mailbox, context);
	}
	return -1;
}

int tris_app_messagecount(const char *context, const char *mailbox, const char *folder)
{
	static int warned = 0;
	if (tris_messagecount_func) {
		return tris_messagecount_func(context, mailbox, folder);
	}

	if (!warned) {
		warned++;
		tris_verb(3, "Message count requested for mailbox %s@%s/%s but voicemail not loaded.\n", mailbox, context, folder);
	}

	return 0;
}

int tris_app_get_vmlist(const char *mailbox, const char *folder, char *vmlist)
{
	static int warned = 0;
	if (tris_getvmlist_func)
		return tris_getvmlist_func(mailbox, folder, vmlist);

	if (!warned) {
		tris_verb(3, "Message check requested for mailbox %s/folder %s but voicemail not loaded.\n", mailbox, folder ? folder : "INBOX");
		warned++;
	}
	return 0;
}

int tris_app_manage_mailbox(const char *mailbox, int folder, int *msglist, int msgcount, const char *command, char *result)
{
	static int warned = 0;
	if (tris_managemailbox_func)
		return tris_managemailbox_func(mailbox, folder, msglist, msgcount, command, result);

	if (!warned) {
		tris_verb(3, "Message check requested for mailbox %s/folder %d but voicemail not loaded.\n", mailbox, folder ? folder : 0);
		warned++;
	}
	return 0;
}

int tris_dtmf_stream(struct tris_channel *chan, struct tris_channel *peer, const char *digits, int between, unsigned int duration) 
{
	const char *ptr;
	int res = 0;
	struct tris_silence_generator *silgen = NULL;

	if (!between) {
		between = 100;
	}

	if (peer) {
		res = tris_autoservice_start(peer);
	}

	if (!res) {
		res = tris_waitfor(chan, 100);
	}

	/* tris_waitfor will return the number of remaining ms on success */
	if (res < 0) {
		if (peer) {
			tris_autoservice_stop(peer);
		}
		return res;
	}

	if (tris_opt_transmit_silence) {
		silgen = tris_channel_start_silence_generator(chan);
	}

	for (ptr = digits; *ptr; ptr++) {
		if (*ptr == 'w') {
			/* 'w' -- wait half a second */
			if ((res = tris_safe_sleep(chan, 500))) {
				break;
			}
		} else if (strchr("0123456789*#abcdfABCDF", *ptr)) {
			/* Character represents valid DTMF */
			if (*ptr == 'f' || *ptr == 'F') {
				/* ignore return values if not supported by channel */
				tris_indicate(chan, TRIS_CONTROL_FLASH);
			} else {
				tris_senddigit(chan, *ptr, duration);
			}
			/* pause between digits */
			if ((res = tris_safe_sleep(chan, between))) {
				break;
			}
		} else {
			tris_log(LOG_WARNING, "Illegal DTMF character '%c' in string. (0-9*#aAbBcCdD allowed)\n", *ptr);
		}
	}

	if (peer) {
		/* Stop autoservice on the peer channel, but don't overwrite any error condition
		   that has occurred previously while acting on the primary channel */
		if (tris_autoservice_stop(peer) && !res) {
			res = -1;
		}
	}

	if (silgen) {
		tris_channel_stop_silence_generator(chan, silgen);
	}

	return res;
}

struct linear_state {
	int fd;
	int autoclose;
	int allowoverride;
	int origwfmt;
};

static void linear_release(struct tris_channel *chan, void *params)
{
	struct linear_state *ls = params;

	if (ls->origwfmt && tris_set_write_format(chan, ls->origwfmt)) {
		tris_log(LOG_WARNING, "Unable to restore channel '%s' to format '%d'\n", chan->name, ls->origwfmt);
	}

	if (ls->autoclose) {
		close(ls->fd);
	}

	tris_free(params);
}

static int linear_generator(struct tris_channel *chan, void *data, int len, int samples)
{
	short buf[2048 + TRIS_FRIENDLY_OFFSET / 2];
	struct linear_state *ls = data;
	struct tris_frame f = {
		.frametype = TRIS_FRAME_VOICE,
		.subclass = TRIS_FORMAT_SLINEAR,
		.data.ptr = buf + TRIS_FRIENDLY_OFFSET / 2,
		.offset = TRIS_FRIENDLY_OFFSET,
	};
	int res;

	len = samples * 2;
	if (len > sizeof(buf) - TRIS_FRIENDLY_OFFSET) {
		tris_log(LOG_WARNING, "Can't generate %d bytes of data!\n" , len);
		len = sizeof(buf) - TRIS_FRIENDLY_OFFSET;
	}
	res = read(ls->fd, buf + TRIS_FRIENDLY_OFFSET/2, len);
	if (res > 0) {
		f.datalen = res;
		f.samples = res / 2;
		tris_write(chan, &f);
		if (res == len) {
			return 0;
		}
	}
	return -1;
}

static void *linear_alloc(struct tris_channel *chan, void *params)
{
	struct linear_state *ls = params;

	if (!params) {
		return NULL;
	}

	/* In this case, params is already malloc'd */
	if (ls->allowoverride) {
		tris_set_flag(chan, TRIS_FLAG_WRITE_INT);
	} else {
		tris_clear_flag(chan, TRIS_FLAG_WRITE_INT);
	}

	ls->origwfmt = chan->writeformat;

	if (tris_set_write_format(chan, TRIS_FORMAT_SLINEAR)) {
		tris_log(LOG_WARNING, "Unable to set '%s' to linear format (write)\n", chan->name);
		tris_free(ls);
		ls = params = NULL;
	}

	return params;
}

static struct tris_generator linearstream =
{
	alloc: linear_alloc,
	release: linear_release,
	generate: linear_generator,
};

int tris_linear_stream(struct tris_channel *chan, const char *filename, int fd, int allowoverride)
{
	struct linear_state *lin;
	char tmpf[256];
	int res = -1;
	int autoclose = 0;
	if (fd < 0) {
		if (tris_strlen_zero(filename)) {
			return -1;
		}
		autoclose = 1;
		if (filename[0] == '/') {
			tris_copy_string(tmpf, filename, sizeof(tmpf));
		} else {
			snprintf(tmpf, sizeof(tmpf), "%s/%s/%s", tris_config_TRIS_DATA_DIR, "sounds", filename);
		}
		if ((fd = open(tmpf, O_RDONLY)) < 0) {
			tris_log(LOG_WARNING, "Unable to open file '%s': %s\n", tmpf, strerror(errno));
			return -1;
		}
	}
	if ((lin = tris_calloc(1, sizeof(*lin)))) {
		lin->fd = fd;
		lin->allowoverride = allowoverride;
		lin->autoclose = autoclose;
		res = tris_activate_generator(chan, &linearstream, lin);
	}
	return res;
}

int tris_control_streamfile(struct tris_channel *chan, const char *file,
			   const char *fwd, const char *rev,
			   const char *stop, const char *suspend,
			   const char *restart, int skipms, long *offsetms)
{
	char *breaks = NULL;
	char *end = NULL;
	int blen = 2;
	int res;
	long pause_restart_point = 0;
	long offset = 0;

	if (offsetms) {
		offset = *offsetms * 8; /* XXX Assumes 8kHz */
	}

	if (stop) {
		blen += strlen(stop);
	}
	if (suspend) {
		blen += strlen(suspend);
	}
	if (restart) {
		blen += strlen(restart);
	}

	if (blen > 2) {
		breaks = alloca(blen + 1);
		breaks[0] = '\0';
		if (stop) {
			strcat(breaks, stop);
		}
		if (suspend) {
			strcat(breaks, suspend);
		}
		if (restart) {
			strcat(breaks, restart);
		}
	}
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}

	if (file) {
		if ((end = strchr(file, ':'))) {
			if (!strcasecmp(end, ":end")) {
				*end = '\0';
				end++;
			}
		}
	}

	for (;;) {
		tris_stopstream(chan);
		res = tris_streamfile(chan, file, chan->language);
		if (!res) {
			if (pause_restart_point) {
				tris_seekstream(chan->stream, pause_restart_point, SEEK_SET);
				pause_restart_point = 0;
			}
			else if (end || offset < 0) {
				if (offset == -8) {
					offset = 0;
				}
				tris_verb(3, "ControlPlayback seek to offset %ld from end\n", offset);

				tris_seekstream(chan->stream, offset, SEEK_END);
				end = NULL;
				offset = 0;
			} else if (offset) {
				tris_verb(3, "ControlPlayback seek to offset %ld\n", offset);
				tris_seekstream(chan->stream, offset, SEEK_SET);
				offset = 0;
			}
			res = tris_waitstream_fr(chan, breaks, fwd, rev, skipms);
		}

		if (res < 1) {
			break;
		}

		/* We go at next loop if we got the restart char */
		if (restart && strchr(restart, res)) {
			tris_debug(1, "we'll restart the stream here at next loop\n");
			pause_restart_point = 0;
			continue;
		}

		if (suspend && strchr(suspend, res)) {
			pause_restart_point = tris_tellstream(chan->stream);
			for (;;) {
				tris_stopstream(chan);
				if (!(res = tris_waitfordigit(chan, 1000))) {
					continue;
				} else if (res == -1 || strchr(suspend, res) || (stop && strchr(stop, res))) {
					break;
				}
			}
			if (res == *suspend) {
				res = 0;
				continue;
			}
		}

		if (res == -1) {
			break;
		}

		/* if we get one of our stop chars, return it to the calling function */
		if (stop && strchr(stop, res)) {
			break;
		}
	}

	if (pause_restart_point) {
		offset = pause_restart_point;
	} else {
		if (chan->stream) {
			offset = tris_tellstream(chan->stream);
		} else {
			offset = -8;  /* indicate end of file */
		}
	}

	if (offsetms) {
		*offsetms = offset / 8; /* samples --> ms ... XXX Assumes 8 kHz */
	}

	/* If we are returning a digit cast it as char */
	if (res > 0 || chan->stream) {
		res = (char)res;
	}

	tris_stopstream(chan);

	return res;
}

int tris_play_and_wait(struct tris_channel *chan, const char *fn)
{
	int d = 0;

	if ((d = tris_streamfile(chan, fn, chan->language))) {
		return d;
	}

	d = tris_waitstream(chan, TRIS_DIGIT_ANY);

	tris_stopstream(chan);

	return d;
}

static int global_silence_threshold = 128;
static int global_maxsilence = 0;

/*! Optionally play a sound file or a beep, then record audio and video from the channel.
 * \param chan Channel to playback to/record from.
 * \param playfile Filename of sound to play before recording begins.
 * \param recordfile Filename to record to.
 * \param maxtime Maximum length of recording (in milliseconds).
 * \param fmt Format(s) to record message in. Multiple formats may be specified by separating them with a '|'.
 * \param duration Where to store actual length of the recorded message (in milliseconds).
 * \param beep Whether to play a beep before starting to record.
 * \param silencethreshold
 * \param maxsilence Length of silence that will end a recording (in milliseconds).
 * \param path Optional filesystem path to unlock.
 * \param prepend If true, prepend the recorded audio to an existing file.
 * \param acceptdtmf DTMF digits that will end the recording.
 * \param canceldtmf DTMF digits that will cancel the recording.
 */

static int __tris_play_and_record(struct tris_channel *chan, const char *playfile, const char *recordfile, int maxtime, const char *fmt, int *duration, int beep, int silencethreshold, int maxsilence, const char *path, int prepend, const char *acceptdtmf, const char *canceldtmf)
{
	int d = 0;
	char *fmts;
	char comment[256];
	int x, fmtcnt = 1, res = -1, outmsg = 0;
	struct tris_filestream *others[TRIS_MAX_FORMATS];
	char *sfmt[TRIS_MAX_FORMATS];
	char *stringp = NULL;
	time_t start, end;
	struct tris_dsp *sildet = NULL;   /* silence detector dsp */
	int totalsilence = 0;
	int rfmt = 0;
	struct tris_silence_generator *silgen = NULL;
	char prependfile[80];

	if (silencethreshold < 0) {
		silencethreshold = global_silence_threshold;
	}

	if (maxsilence < 0) {
		maxsilence = global_maxsilence;
	}

	/* barf if no pointer passed to store duration in */
	if (!duration) {
		tris_log(LOG_WARNING, "Error play_and_record called without duration pointer\n");
		return -1;
	}

	tris_debug(1, "play_and_record: %s, %s, '%s'\n", playfile ? playfile : "<None>", recordfile, fmt);
	snprintf(comment, sizeof(comment), "Playing %s, Recording to: %s on %s\n", playfile ? playfile : "<None>", recordfile, chan->name);

	if (playfile || beep) {
		if (!beep) {
			d = tris_play_and_wait(chan, playfile);
		}
		if (d > -1) {
			d = tris_stream_and_wait(chan, "beep", "");
		}
		if (d < 0) {
			return -1;
		}
	}

	if (prepend) {
		tris_copy_string(prependfile, recordfile, sizeof(prependfile));
		strncat(prependfile, "-prepend", sizeof(prependfile) - strlen(prependfile) - 1);
	}

	fmts = tris_strdupa(fmt);

	stringp = fmts;
	strsep(&stringp, "|");
	tris_debug(1, "Recording Formats: sfmts=%s\n", fmts);
	sfmt[0] = tris_strdupa(fmts);

	while ((fmt = strsep(&stringp, "|"))) {
		if (fmtcnt > TRIS_MAX_FORMATS - 1) {
			tris_log(LOG_WARNING, "Please increase TRIS_MAX_FORMATS in file.h\n");
			break;
		}
		sfmt[fmtcnt++] = tris_strdupa(fmt);
	}

	end = start = time(NULL);  /* pre-initialize end to be same as start in case we never get into loop */
	for (x = 0; x < fmtcnt; x++) {
		others[x] = tris_writefile(prepend ? prependfile : recordfile, sfmt[x], comment, O_TRUNC, 0, TRIS_FILE_MODE);
		tris_verb(3, "x=%d, open writing:  %s format: %s, %p\n", x, prepend ? prependfile : recordfile, sfmt[x], others[x]);

		if (!others[x]) {
			break;
		}
	}

	if (path) {
		tris_unlock_path(path);
	}

	if (maxsilence > 0) {
		sildet = tris_dsp_new(); /* Create the silence detector */
		if (!sildet) {
			tris_log(LOG_WARNING, "Unable to create silence detector :(\n");
			return -1;
		}
		tris_dsp_set_threshold(sildet, silencethreshold);
		rfmt = chan->readformat;
		res = tris_set_read_format(chan, TRIS_FORMAT_SLINEAR);
		if (res < 0) {
			tris_log(LOG_WARNING, "Unable to set to linear mode, giving up\n");
			tris_dsp_free(sildet);
			return -1;
		}
	}

	if (!prepend) {
		/* Request a video update */
		tris_indicate(chan, TRIS_CONTROL_VIDUPDATE);

		if (tris_opt_transmit_silence) {
			silgen = tris_channel_start_silence_generator(chan);
		}
	}

	if (x == fmtcnt) {
		/* Loop forever, writing the packets we read to the writer(s), until
		   we read a digit or get a hangup */
		struct tris_frame *f;
		for (;;) {
			if (!(res = tris_waitfor(chan, 2000))) {
				tris_debug(1, "One waitfor failed, trying another\n");
				/* Try one more time in case of masq */
				if (!(res = tris_waitfor(chan, 2000))) {
					tris_log(LOG_WARNING, "No audio available on %s??\n", chan->name);
					res = -1;
				}
			}

			if (res < 0) {
				f = NULL;
				break;
			}
			if (!(f = tris_read(chan))) {
				break;
			}
			if (f->frametype == TRIS_FRAME_VOICE) {
				/* write each format */
				for (x = 0; x < fmtcnt; x++) {
					if (prepend && !others[x]) {
						break;
					}
					res = tris_writestream(others[x], f);
				}

				/* Silence Detection */
				if (maxsilence > 0) {
					int dspsilence = 0;
					tris_dsp_silence(sildet, f, &dspsilence);
					if (dspsilence) {
						totalsilence = dspsilence;
					} else {
						totalsilence = 0;
					}

					if (totalsilence > maxsilence) {
						/* Ended happily with silence */
						tris_verb(3, "Recording automatically stopped after a silence of %d seconds\n", totalsilence/1000);
						res = 'S';
						outmsg = 2;
						break;
					}
				}
				/* Exit on any error */
				if (res) {
					tris_log(LOG_WARNING, "Error writing frame\n");
					break;
				}
			} else if (f->frametype == TRIS_FRAME_VIDEO) {
				/* Write only once */
				tris_writestream(others[0], f);
			} else if (f->frametype == TRIS_FRAME_DTMF) {
				if (prepend) {
				/* stop recording with any digit */
					tris_verb(3, "User ended message by pressing %c\n", f->subclass);
					res = 't';
					outmsg = 2;
					break;
				}
				if (strchr(acceptdtmf, f->subclass)) {
					tris_verb(3, "User ended message by pressing %c\n", f->subclass);
					res = f->subclass;
					outmsg = 2;
					break;
				}
				if (strchr(canceldtmf, f->subclass)) {
					tris_verb(3, "User cancelled message by pressing %c\n", f->subclass);
					res = f->subclass;
					outmsg = 0;
					break;
				}
			}
			if (maxtime) {
				end = time(NULL);
				if (maxtime < (end - start)) {
					tris_verb(3, "Took too long, cutting it short...\n");
					res = 't';
					outmsg = 2;
					break;
				}
			}
			tris_frfree(f);
		}
		if (!f) {
			tris_verb(3, "User hung up\n");
			res = -1;
			outmsg = 1;
		} else {
			tris_frfree(f);
		}
	} else {
		tris_log(LOG_WARNING, "Error creating writestream '%s', format '%s'\n", recordfile, sfmt[x]);
	}

	if (!prepend) {
		if (silgen) {
			tris_channel_stop_silence_generator(chan, silgen);
		}
	}

	/*!\note
	 * Instead of asking how much time passed (end - start), calculate the number
	 * of seconds of audio which actually went into the file.  This fixes a
	 * problem where audio is stopped up on the network and never gets to us.
	 *
	 * Note that we still want to use the number of seconds passed for the max
	 * message, otherwise we could get a situation where this stream is never
	 * closed (which would create a resource leak).
	 */
	*duration = others[0] ? tris_tellstream(others[0]) / 8000 : 0;

	if (!prepend) {
		for (x = 0; x < fmtcnt; x++) {
			if (!others[x]) {
				break;
			}
			/*!\note
			 * If we ended with silence, trim all but the first 200ms of silence
			 * off the recording.  However, if we ended with '#', we don't want
			 * to trim ANY part of the recording.
			 */
			if (res > 0 && totalsilence) {
				tris_stream_rewind(others[x], totalsilence - 200);
				/* Reduce duration by a corresponding amount */
				if (x == 0 && *duration) {
					*duration -= (totalsilence - 200) / 1000;
					if (*duration < 0) {
						*duration = 0;
					}
				}
			}
			tris_truncstream(others[x]);
			tris_closestream(others[x]);
		}
	}

	if (prepend && outmsg) {
		struct tris_filestream *realfiles[TRIS_MAX_FORMATS];
		struct tris_frame *fr;

		for (x = 0; x < fmtcnt; x++) {
			snprintf(comment, sizeof(comment), "Opening the real file %s.%s\n", recordfile, sfmt[x]);
			realfiles[x] = tris_readfile(recordfile, sfmt[x], comment, O_RDONLY, 0, 0);
			if (!others[x] || !realfiles[x]) {
				break;
			}
			/*!\note Same logic as above. */
			if (totalsilence) {
				tris_stream_rewind(others[x], totalsilence - 200);
			}
			tris_truncstream(others[x]);
			/* add the original file too */
			while ((fr = tris_readframe(realfiles[x]))) {
				tris_writestream(others[x], fr);
				tris_frfree(fr);
			}
			tris_closestream(others[x]);
			tris_closestream(realfiles[x]);
			tris_filerename(prependfile, recordfile, sfmt[x]);
			tris_verb(4, "Recording Format: sfmts=%s, prependfile %s, recordfile %s\n", sfmt[x], prependfile, recordfile);
			tris_filedelete(prependfile, sfmt[x]);
		}
	}
	if (rfmt && tris_set_read_format(chan, rfmt)) {
		tris_log(LOG_WARNING, "Unable to restore format %s to channel '%s'\n", tris_getformatname(rfmt), chan->name);
	}
	if (sildet) {
		tris_dsp_free(sildet);
	}
	return res;
}

static char default_acceptdtmf[] = "#";
static char default_canceldtmf[] = "";

int tris_play_and_record_full(struct tris_channel *chan, const char *playfile, const char *recordfile, int maxtime, const char *fmt, int *duration, int silencethreshold, int maxsilence, const char *path, const char *acceptdtmf, const char *canceldtmf)
{
	return __tris_play_and_record(chan, playfile, recordfile, maxtime, fmt, duration, 0, silencethreshold, maxsilence, path, 0, S_OR(acceptdtmf, default_acceptdtmf), S_OR(canceldtmf, default_canceldtmf));
}

int tris_play_and_record(struct tris_channel *chan, const char *playfile, const char *recordfile, int maxtime, const char *fmt, int *duration, int silencethreshold, int maxsilence, const char *path)
{
	return __tris_play_and_record(chan, playfile, recordfile, maxtime, fmt, duration, 1, silencethreshold, maxsilence, path, 0, default_acceptdtmf, default_canceldtmf);
}

int tris_play_and_prepend(struct tris_channel *chan, char *playfile, char *recordfile, int maxtime, char *fmt, int *duration, int beep, int silencethreshold, int maxsilence)
{
	return __tris_play_and_record(chan, playfile, recordfile, maxtime, fmt, duration, beep, silencethreshold, maxsilence, NULL, 1, default_acceptdtmf, default_canceldtmf);
}

/* Channel group core functions */

int tris_app_group_split_group(const char *data, char *group, int group_max, char *category, int category_max)
{
	int res = 0;
	char tmp[256];
	char *grp = NULL, *cat = NULL;

	if (!tris_strlen_zero(data)) {
		tris_copy_string(tmp, data, sizeof(tmp));
		grp = tmp;
		if ((cat = strchr(tmp, '@'))) {
			*cat++ = '\0';
		}
	}

	if (!tris_strlen_zero(grp)) {
		tris_copy_string(group, grp, group_max);
	} else {
		*group = '\0';
	}

	if (!tris_strlen_zero(cat)) {
		tris_copy_string(category, cat, category_max);
	}

	return res;
}

int tris_app_group_set_channel(struct tris_channel *chan, const char *data)
{
	int res = 0;
	char group[80] = "", category[80] = "";
	struct tris_group_info *gi = NULL;
	size_t len = 0;

	if (tris_app_group_split_group(data, group, sizeof(group), category, sizeof(category))) {
		return -1;
	}

	/* Calculate memory we will need if this is new */
	len = sizeof(*gi) + strlen(group) + 1;
	if (!tris_strlen_zero(category)) {
		len += strlen(category) + 1;
	}

	TRIS_RWLIST_WRLOCK(&groups);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&groups, gi, group_list) {
		if ((gi->chan == chan) && ((tris_strlen_zero(category) && tris_strlen_zero(gi->category)) || (!tris_strlen_zero(gi->category) && !strcasecmp(gi->category, category)))) {
			TRIS_RWLIST_REMOVE_CURRENT(group_list);
			free(gi);
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;

	if (tris_strlen_zero(group)) {
		/* Enable unsetting the group */
	} else if ((gi = calloc(1, len))) {
		gi->chan = chan;
		gi->group = (char *) gi + sizeof(*gi);
		strcpy(gi->group, group);
		if (!tris_strlen_zero(category)) {
			gi->category = (char *) gi + sizeof(*gi) + strlen(group) + 1;
			strcpy(gi->category, category);
		}
		TRIS_RWLIST_INSERT_TAIL(&groups, gi, group_list);
	} else {
		res = -1;
	}

	TRIS_RWLIST_UNLOCK(&groups);

	return res;
}

int tris_app_group_get_count(const char *group, const char *category)
{
	struct tris_group_info *gi = NULL;
	int count = 0;

	if (tris_strlen_zero(group)) {
		return 0;
	}

	TRIS_RWLIST_RDLOCK(&groups);
	TRIS_RWLIST_TRAVERSE(&groups, gi, group_list) {
		if (!strcasecmp(gi->group, group) && (tris_strlen_zero(category) || (!tris_strlen_zero(gi->category) && !strcasecmp(gi->category, category)))) {
			count++;
		}
	}
	TRIS_RWLIST_UNLOCK(&groups);

	return count;
}

int tris_app_group_match_get_count(const char *groupmatch, const char *category)
{
	struct tris_group_info *gi = NULL;
	regex_t regexbuf;
	int count = 0;

	if (tris_strlen_zero(groupmatch)) {
		return 0;
	}

	/* if regex compilation fails, return zero matches */
	if (regcomp(&regexbuf, groupmatch, REG_EXTENDED | REG_NOSUB)) {
		return 0;
	}

	TRIS_RWLIST_RDLOCK(&groups);
	TRIS_RWLIST_TRAVERSE(&groups, gi, group_list) {
		if (!regexec(&regexbuf, gi->group, 0, NULL, 0) && (tris_strlen_zero(category) || (!tris_strlen_zero(gi->category) && !strcasecmp(gi->category, category)))) {
			count++;
		}
	}
	TRIS_RWLIST_UNLOCK(&groups);

	regfree(&regexbuf);

	return count;
}

int tris_app_group_update(struct tris_channel *old, struct tris_channel *new)
{
	struct tris_group_info *gi = NULL;

	TRIS_RWLIST_WRLOCK(&groups);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&groups, gi, group_list) {
		if (gi->chan == old) {
			gi->chan = new;
		} else if (gi->chan == new) {
			TRIS_RWLIST_REMOVE_CURRENT(group_list);
			tris_free(gi);
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&groups);

	return 0;
}

int tris_app_group_discard(struct tris_channel *chan)
{
	struct tris_group_info *gi = NULL;

	TRIS_RWLIST_WRLOCK(&groups);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&groups, gi, group_list) {
		if (gi->chan == chan) {
			TRIS_RWLIST_REMOVE_CURRENT(group_list);
			tris_free(gi);
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&groups);

	return 0;
}

int tris_app_group_list_wrlock(void)
{
	return TRIS_RWLIST_WRLOCK(&groups);
}

int tris_app_group_list_rdlock(void)
{
	return TRIS_RWLIST_RDLOCK(&groups);
}

struct tris_group_info *tris_app_group_list_head(void)
{
	return TRIS_RWLIST_FIRST(&groups);
}

int tris_app_group_list_unlock(void)
{
	return TRIS_RWLIST_UNLOCK(&groups);
}

#undef tris_app_separate_args
unsigned int tris_app_separate_args(char *buf, char delim, char **array, int arraylen);

unsigned int __tris_app_separate_args(char *buf, char delim, int remove_chars, char **array, int arraylen)
{
	int argc;
	char *scan, *wasdelim = NULL;
	int paren = 0, quote = 0;

	if (!buf || !array || !arraylen) {
		return 0;
	}

	memset(array, 0, arraylen * sizeof(*array));

	scan = buf;

	for (argc = 0; *scan && (argc < arraylen - 1); argc++) {
		array[argc] = scan;
		for (; *scan; scan++) {
			if (*scan == '(') {
				paren++;
			} else if (*scan == ')') {
				if (paren) {
					paren--;
				}
			} else if (*scan == '"' && delim != '"') {
				quote = quote ? 0 : 1;
				if (remove_chars) {
					/* Remove quote character from argument */
					memmove(scan, scan + 1, strlen(scan));
					scan--;
				}
			} else if (*scan == '\\') {
				if (remove_chars) {
					/* Literal character, don't parse */
					memmove(scan, scan + 1, strlen(scan));
				} else {
					scan++;
				}
			} else if ((*scan == delim) && !paren && !quote) {
				wasdelim = scan;
				*scan++ = '\0';
				break;
			}
		}
	}

	/* If the last character in the original string was the delimiter, then
	 * there is one additional argument. */
	if (*scan || (scan > buf && (scan - 1) == wasdelim)) {
		array[argc++] = scan;
	}

	return argc;
}

/* ABI compatible function */
unsigned int tris_app_separate_args(char *buf, char delim, char **array, int arraylen)
{
	return __tris_app_separate_args(buf, delim, 1, array, arraylen);
}

static enum TRIS_LOCK_RESULT tris_lock_path_lockfile(const char *path)
{
	char *s;
	char *fs;
	int res;
	int fd;
	int lp = strlen(path);
	time_t start;

	s = alloca(lp + 10);
	fs = alloca(lp + 20);

	snprintf(fs, strlen(path) + 19, "%s/.lock-%08lx", path, tris_random());
	fd = open(fs, O_WRONLY | O_CREAT | O_EXCL, TRIS_FILE_MODE);
	if (fd < 0) {
		tris_log(LOG_ERROR, "Unable to create lock file '%s': %s\n", path, strerror(errno));
		return TRIS_LOCK_PATH_NOT_FOUND;
	}
	close(fd);

	snprintf(s, strlen(path) + 9, "%s/.lock", path);
	start = time(NULL);
	while (((res = link(fs, s)) < 0) && (errno == EEXIST) && (time(NULL) - start < 5)) {
		sched_yield();
	}

	unlink(fs);

	if (res) {
		tris_log(LOG_WARNING, "Failed to lock path '%s': %s\n", path, strerror(errno));
		return TRIS_LOCK_TIMEOUT;
	} else {
		tris_debug(1, "Locked path '%s'\n", path);
		return TRIS_LOCK_SUCCESS;
	}
}

static int tris_unlock_path_lockfile(const char *path)
{
	char *s;
	int res;

	s = alloca(strlen(path) + 10);

	snprintf(s, strlen(path) + 9, "%s/%s", path, ".lock");

	if ((res = unlink(s))) {
		tris_log(LOG_ERROR, "Could not unlock path '%s': %s\n", path, strerror(errno));
	} else {
		tris_debug(1, "Unlocked path '%s'\n", path);
	}

	return res;
}

struct path_lock {
	TRIS_LIST_ENTRY(path_lock) le;
	int fd;
	char *path;
};

static TRIS_LIST_HEAD_STATIC(path_lock_list, path_lock);

static void path_lock_destroy(struct path_lock *obj)
{
	if (obj->fd >= 0) {
		close(obj->fd);
	}
	if (obj->path) {
		free(obj->path);
	}
	free(obj);
}

static enum TRIS_LOCK_RESULT tris_lock_path_flock(const char *path)
{
	char *fs;
	int res;
	int fd;
	time_t start;
	struct path_lock *pl;
	struct stat st, ost;

	fs = alloca(strlen(path) + 20);

	snprintf(fs, strlen(path) + 19, "%s/lock", path);
	if (lstat(fs, &st) == 0) {
		if ((st.st_mode & S_IFMT) == S_IFLNK) {
			tris_log(LOG_WARNING, "Unable to create lock file "
					"'%s': it's already a symbolic link\n",
					fs);
			return TRIS_LOCK_FAILURE;
		}
		if (st.st_nlink > 1) {
			tris_log(LOG_WARNING, "Unable to create lock file "
					"'%s': %u hard links exist\n",
					fs, (unsigned int) st.st_nlink);
			return TRIS_LOCK_FAILURE;
		}
	}
	if ((fd = open(fs, O_WRONLY | O_CREAT, 0600)) < 0) {
		tris_log(LOG_WARNING, "Unable to create lock file '%s': %s\n",
				fs, strerror(errno));
		return TRIS_LOCK_PATH_NOT_FOUND;
	}
	if (!(pl = tris_calloc(1, sizeof(*pl)))) {
		/* We don't unlink the lock file here, on the possibility that
		 * someone else created it - better to leave a little mess
		 * than create a big one by destroying someone else's lock
		 * and causing something to be corrupted.
		 */
		close(fd);
		return TRIS_LOCK_FAILURE;
	}
	pl->fd = fd;
	pl->path = strdup(path);

	time(&start);
	while (
		#ifdef SOLARIS
		((res = fcntl(pl->fd, F_SETLK, fcntl(pl->fd, F_GETFL) | O_NONBLOCK)) < 0) &&
		#else
		((res = flock(pl->fd, LOCK_EX | LOCK_NB)) < 0) &&
		#endif
			(errno == EWOULDBLOCK) &&
			(time(NULL) - start < 5))
		usleep(1000);
	if (res) {
		tris_log(LOG_WARNING, "Failed to lock path '%s': %s\n",
				path, strerror(errno));
		/* No unlinking of lock done, since we tried and failed to
		 * flock() it.
		 */
		path_lock_destroy(pl);
		return TRIS_LOCK_TIMEOUT;
	}

	/* Check for the race where the file is recreated or deleted out from
	 * underneath us.
	 */
	if (lstat(fs, &st) != 0 && fstat(pl->fd, &ost) != 0 &&
			st.st_dev != ost.st_dev &&
			st.st_ino != ost.st_ino) {
		tris_log(LOG_WARNING, "Unable to create lock file '%s': "
				"file changed underneath us\n", fs);
		path_lock_destroy(pl);
		return TRIS_LOCK_FAILURE;
	}

	/* Success: file created, flocked, and is the one we started with */
	TRIS_LIST_LOCK(&path_lock_list);
	TRIS_LIST_INSERT_TAIL(&path_lock_list, pl, le);
	TRIS_LIST_UNLOCK(&path_lock_list);

	tris_debug(1, "Locked path '%s'\n", path);

	return TRIS_LOCK_SUCCESS;
}

static int tris_unlock_path_flock(const char *path)
{
	char *s;
	struct path_lock *p;

	s = alloca(strlen(path) + 20);

	TRIS_LIST_LOCK(&path_lock_list);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&path_lock_list, p, le) {
		if (!strcmp(p->path, path)) {
			TRIS_LIST_REMOVE_CURRENT(le);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&path_lock_list);

	if (p) {
		snprintf(s, strlen(path) + 19, "%s/lock", path);
		unlink(s);
		path_lock_destroy(p);
		tris_log(LOG_DEBUG, "Unlocked path '%s'\n", path);
	} else {
		tris_log(LOG_DEBUG, "Failed to unlock path '%s': "
				"lock not found\n", path);
	}

	return 0;
}

void tris_set_lock_type(enum TRIS_LOCK_TYPE type)
{
	tris_lock_type = type;
}

enum TRIS_LOCK_RESULT tris_lock_path(const char *path)
{
	enum TRIS_LOCK_RESULT r = TRIS_LOCK_FAILURE;

	switch (tris_lock_type) {
	case TRIS_LOCK_TYPE_LOCKFILE:
		r = tris_lock_path_lockfile(path);
		break;
	case TRIS_LOCK_TYPE_FLOCK:
		r = tris_lock_path_flock(path);
		break;
	}

	return r;
}

int tris_unlock_path(const char *path)
{
	int r = 0;

	switch (tris_lock_type) {
	case TRIS_LOCK_TYPE_LOCKFILE:
		r = tris_unlock_path_lockfile(path);
		break;
	case TRIS_LOCK_TYPE_FLOCK:
		r = tris_unlock_path_flock(path);
		break;
	}

	return r;
}

int tris_record_review(struct tris_channel *chan, const char *playfile, const char *recordfile, int maxtime, const char *fmt, int *duration, const char *path) 
{
	int silencethreshold;
	int maxsilence = 0;
	int res = 0;
	int cmd = 0;
	int max_attempts = 3;
	int attempts = 0;
	int recorded = 0;
	int message_exists = 0;
	/* Note that urgent and private are for flagging messages as such in the future */

	/* barf if no pointer passed to store duration in */
	if (!duration) {
		tris_log(LOG_WARNING, "Error tris_record_review called without duration pointer\n");
		return -1;
	}

	cmd = '3';	 /* Want to start by recording */

	silencethreshold = tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE);

	while ((cmd >= 0) && (cmd != 't')) {
		switch (cmd) {
		case '1':
			if (!message_exists) {
				/* In this case, 1 is to record a message */
				cmd = '3';
				break;
			} else {
				tris_stream_and_wait(chan, "voicemail/vm-msgsaved", "");
				cmd = 't';
				return res;
			}
		case '2':
			/* Review */
			tris_verb(3, "Reviewing the recording\n");
			cmd = tris_stream_and_wait(chan, recordfile, TRIS_DIGIT_ANY);
			break;
		case '3':
			message_exists = 0;
			/* Record */
			tris_verb(3, "R%secording\n", recorded == 1 ? "e-r" : "");
			recorded = 1;
			if ((cmd = tris_play_and_record(chan, playfile, recordfile, maxtime, fmt, duration, silencethreshold, maxsilence, path)) == -1) {
				/* User has hung up, no options to give */
				return cmd;
			}
			if (cmd == '0') {
				break;
			} else if (cmd == '*') {
				break;
			} else {
				/* If all is well, a message exists */
				message_exists = 1;
				cmd = 0;
			}
			break;
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '*':
		case '#':
			cmd = tris_play_and_wait(chan, "voicemail/vm-sorry");
			break;
		default:
			if (message_exists) {
				cmd = tris_play_and_wait(chan, "voicemail/vm-review");
			} else {
				if (!(cmd = tris_play_and_wait(chan, "voicemail/vm-torerecord"))) {
					cmd = tris_waitfordigit(chan, 600);
				}
			}

			if (!cmd) {
				cmd = tris_waitfordigit(chan, 6000);
			}
			if (!cmd) {
				attempts++;
			}
			if (attempts > max_attempts) {
				cmd = 't';
			}
		}
	}
	if (cmd == 't') {
		cmd = 0;
	}
	return cmd;
}

#define RES_UPONE (1 << 16)
#define RES_EXIT  (1 << 17)
#define RES_REPEAT (1 << 18)
#define RES_RESTART ((1 << 19) | RES_REPEAT)

static int tris_ivr_menu_run_internal(struct tris_channel *chan, struct tris_ivr_menu *menu, void *cbdata);

static int ivr_dispatch(struct tris_channel *chan, struct tris_ivr_option *option, char *exten, void *cbdata)
{
	int res;
	int (*ivr_func)(struct tris_channel *, void *);
	char *c;
	char *n;

	switch (option->action) {
	case TRIS_ACTION_UPONE:
		return RES_UPONE;
	case TRIS_ACTION_EXIT:
		return RES_EXIT | (((unsigned long)(option->adata)) & 0xffff);
	case TRIS_ACTION_REPEAT:
		return RES_REPEAT | (((unsigned long)(option->adata)) & 0xffff);
	case TRIS_ACTION_RESTART:
		return RES_RESTART ;
	case TRIS_ACTION_NOOP:
		return 0;
	case TRIS_ACTION_BACKGROUND:
		res = tris_stream_and_wait(chan, (char *)option->adata, TRIS_DIGIT_ANY);
		if (res < 0) {
			tris_log(LOG_NOTICE, "Unable to find file '%s'!\n", (char *)option->adata);
			res = 0;
		}
		return res;
	case TRIS_ACTION_PLAYBACK:
		res = tris_stream_and_wait(chan, (char *)option->adata, "");
		if (res < 0) {
			tris_log(LOG_NOTICE, "Unable to find file '%s'!\n", (char *)option->adata);
			res = 0;
		}
		return res;
	case TRIS_ACTION_MENU:
		if ((res = tris_ivr_menu_run_internal(chan, (struct tris_ivr_menu *)option->adata, cbdata)) == -2) {
			/* Do not pass entry errors back up, treat as though it was an "UPONE" */
			res = 0;
		}
		return res;
	case TRIS_ACTION_WAITOPTION:
		if (!(res = tris_waitfordigit(chan, chan->pbx ? chan->pbx->rtimeoutms : 10000))) {
			return 't';
		}
		return res;
	case TRIS_ACTION_CALLBACK:
		ivr_func = option->adata;
		res = ivr_func(chan, cbdata);
		return res;
	case TRIS_ACTION_TRANSFER:
		res = tris_parseable_goto(chan, option->adata);
		return 0;
	case TRIS_ACTION_PLAYLIST:
	case TRIS_ACTION_BACKLIST:
		res = 0;
		c = tris_strdupa(option->adata);
		while ((n = strsep(&c, ";"))) {
			if ((res = tris_stream_and_wait(chan, n,
					(option->action == TRIS_ACTION_BACKLIST) ? TRIS_DIGIT_ANY : ""))) {
				break;
			}
		}
		tris_stopstream(chan);
		return res;
	default:
		tris_log(LOG_NOTICE, "Unknown dispatch function %d, ignoring!\n", option->action);
		return 0;
	}
	return -1;
}

static int option_exists(struct tris_ivr_menu *menu, char *option)
{
	int x;
	for (x = 0; menu->options[x].option; x++) {
		if (!strcasecmp(menu->options[x].option, option)) {
			return x;
		}
	}
	return -1;
}

static int option_matchmore(struct tris_ivr_menu *menu, char *option)
{
	int x;
	for (x = 0; menu->options[x].option; x++) {
		if ((!strncasecmp(menu->options[x].option, option, strlen(option))) &&
				(menu->options[x].option[strlen(option)])) {
			return x;
		}
	}
	return -1;
}

static int read_newoption(struct tris_channel *chan, struct tris_ivr_menu *menu, char *exten, int maxexten)
{
	int res = 0;
	int ms;
	while (option_matchmore(menu, exten)) {
		ms = chan->pbx ? chan->pbx->dtimeoutms : 5000;
		if (strlen(exten) >= maxexten - 1) {
			break;
		}
		if ((res = tris_waitfordigit(chan, ms)) < 1) {
			break;
		}
		exten[strlen(exten) + 1] = '\0';
		exten[strlen(exten)] = res;
	}
	return res > 0 ? 0 : res;
}

static int tris_ivr_menu_run_internal(struct tris_channel *chan, struct tris_ivr_menu *menu, void *cbdata)
{
	/* Execute an IVR menu structure */
	int res = 0;
	int pos = 0;
	int retries = 0;
	char exten[TRIS_MAX_EXTENSION] = "s";
	if (option_exists(menu, "s") < 0) {
		strcpy(exten, "g");
		if (option_exists(menu, "g") < 0) {
			tris_log(LOG_WARNING, "No 's' nor 'g' extension in menu '%s'!\n", menu->title);
			return -1;
		}
	}
	while (!res) {
		while (menu->options[pos].option) {
			if (!strcasecmp(menu->options[pos].option, exten)) {
				res = ivr_dispatch(chan, menu->options + pos, exten, cbdata);
				tris_debug(1, "IVR Dispatch of '%s' (pos %d) yields %d\n", exten, pos, res);
				if (res < 0) {
					break;
				} else if (res & RES_UPONE) {
					return 0;
				} else if (res & RES_EXIT) {
					return res;
				} else if (res & RES_REPEAT) {
					int maxretries = res & 0xffff;
					if ((res & RES_RESTART) == RES_RESTART) {
						retries = 0;
					} else {
						retries++;
					}
					if (!maxretries) {
						maxretries = 3;
					}
					if ((maxretries > 0) && (retries >= maxretries)) {
						tris_debug(1, "Max retries %d exceeded\n", maxretries);
						return -2;
					} else {
						if (option_exists(menu, "g") > -1) {
							strcpy(exten, "g");
						} else if (option_exists(menu, "s") > -1) {
							strcpy(exten, "s");
						}
					}
					pos = 0;
					continue;
				} else if (res && strchr(TRIS_DIGIT_ANY, res)) {
					tris_debug(1, "Got start of extension, %c\n", res);
					exten[1] = '\0';
					exten[0] = res;
					if ((res = read_newoption(chan, menu, exten, sizeof(exten)))) {
						break;
					}
					if (option_exists(menu, exten) < 0) {
						if (option_exists(menu, "i")) {
							tris_debug(1, "Invalid extension entered, going to 'i'!\n");
							strcpy(exten, "i");
							pos = 0;
							continue;
						} else {
							tris_debug(1, "Aborting on invalid entry, with no 'i' option!\n");
							res = -2;
							break;
						}
					} else {
						tris_debug(1, "New existing extension: %s\n", exten);
						pos = 0;
						continue;
					}
				}
			}
			pos++;
		}
		tris_debug(1, "Stopping option '%s', res is %d\n", exten, res);
		pos = 0;
		if (!strcasecmp(exten, "s")) {
			strcpy(exten, "g");
		} else {
			break;
		}
	}
	return res;
}

int tris_ivr_menu_run(struct tris_channel *chan, struct tris_ivr_menu *menu, void *cbdata)
{
	int res = tris_ivr_menu_run_internal(chan, menu, cbdata);
	/* Hide internal coding */
	return res > 0 ? 0 : res;
}

char *tris_read_textfile(const char *filename)
{
	int fd, count = 0, res;
	char *output = NULL;
	struct stat filesize;

	if (stat(filename, &filesize) == -1) {
		tris_log(LOG_WARNING, "Error can't stat %s\n", filename);
		return NULL;
	}

	count = filesize.st_size + 1;

	if ((fd = open(filename, O_RDONLY)) < 0) {
		tris_log(LOG_WARNING, "Cannot open file '%s' for reading: %s\n", filename, strerror(errno));
		return NULL;
	}

	if ((output = tris_malloc(count))) {
		res = read(fd, output, count - 1);
		if (res == count - 1) {
			output[res] = '\0';
		} else {
			tris_log(LOG_WARNING, "Short read of %s (%d of %d): %s\n", filename, res, count - 1, strerror(errno));
			tris_free(output);
			output = NULL;
		}
	}

	close(fd);

	return output;
}

int tris_app_parse_options(const struct tris_app_option *options, struct tris_flags *flags, char **args, char *optstr)
{
	char *s, *arg;
	int curarg, res = 0;
	unsigned int argloc;

	tris_clear_flag(flags, TRIS_FLAGS_ALL);

	if (!optstr) {
		return 0;
	}

	s = optstr;
	while (*s) {
		curarg = *s++ & 0x7f;	/* the array (in app.h) has 128 entries */
		argloc = options[curarg].arg_index;
		if (*s == '(') {
			/* Has argument */
			arg = ++s;
			if ((s = strchr(s, ')'))) {
				if (argloc) {
					args[argloc - 1] = arg;
				}
				*s++ = '\0';
			} else {
				tris_log(LOG_WARNING, "Missing closing parenthesis for argument '%c' in string '%s'\n", curarg, arg);
				res = -1;
				break;
			}
		} else if (argloc) {
			args[argloc - 1] = "";
		}
		tris_set_flag(flags, options[curarg].flag);
	}

	return res;
}

/* the following function will probably only be used in app_dial, until app_dial is reorganized to
   better handle the large number of options it provides. After it is, you need to get rid of this variant 
   -- unless, of course, someone else digs up some use for large flag fields. */

int tris_app_parse_options64(const struct tris_app_option *options, struct tris_flags64 *flags, char **args, char *optstr)
{
	char *s, *arg;
	int curarg, res = 0;
	unsigned int argloc;

	flags->flags = 0;

	if (!optstr) {
		return 0;
	}

	s = optstr;
	while (*s) {
		curarg = *s++ & 0x7f;	/* the array (in app.h) has 128 entries */
		tris_set_flag64(flags, options[curarg].flag);
		argloc = options[curarg].arg_index;
		if (*s == '(') {
			/* Has argument */
			arg = ++s;
			if ((s = strchr(s, ')'))) {
				if (argloc) {
					args[argloc - 1] = arg;
				}
				*s++ = '\0';
			} else {
				tris_log(LOG_WARNING, "Missing closing parenthesis for argument '%c' in string '%s'\n", curarg, arg);
				res = -1;
				break;
			}
		} else if (argloc) {
			args[argloc - 1] = NULL;
		}
	}

	return res;
}

void tris_app_options2str64(const struct tris_app_option *options, struct tris_flags64 *flags, char *buf, size_t len)
{
	unsigned int i, found = 0;
	for (i = 32; i < 128 && found < len; i++) {
		if (tris_test_flag64(flags, options[i].flag)) {
			buf[found++] = i;
		}
	}
	buf[found] = '\0';
}

int tris_get_encoded_char(const char *stream, char *result, size_t *consumed)
{
	int i;
	*consumed = 1;
	*result = 0;
	if (tris_strlen_zero(stream)) {
		*consumed = 0;
		return -1;
	}

	if (*stream == '\\') {
		*consumed = 2;
		switch (*(stream + 1)) {
		case 'n':
			*result = '\n';
			break;
		case 'r':
			*result = '\r';
			break;
		case 't':
			*result = '\t';
			break;
		case 'x':
			/* Hexadecimal */
			if (strchr("0123456789ABCDEFabcdef", *(stream + 2)) && *(stream + 2) != '\0') {
				*consumed = 3;
				if (*(stream + 2) <= '9') {
					*result = *(stream + 2) - '0';
				} else if (*(stream + 2) <= 'F') {
					*result = *(stream + 2) - 'A' + 10;
				} else {
					*result = *(stream + 2) - 'a' + 10;
				}
			} else {
				tris_log(LOG_ERROR, "Illegal character '%c' in hexadecimal string\n", *(stream + 2));
				return -1;
			}

			if (strchr("0123456789ABCDEFabcdef", *(stream + 3)) && *(stream + 3) != '\0') {
				*consumed = 4;
				*result <<= 4;
				if (*(stream + 3) <= '9') {
					*result += *(stream + 3) - '0';
				} else if (*(stream + 3) <= 'F') {
					*result += *(stream + 3) - 'A' + 10;
				} else {
					*result += *(stream + 3) - 'a' + 10;
				}
			}
			break;
		case '0':
			/* Octal */
			*consumed = 2;
			for (i = 2; ; i++) {
				if (strchr("01234567", *(stream + i)) && *(stream + i) != '\0') {
					(*consumed)++;
					tris_debug(5, "result was %d, ", *result);
					*result <<= 3;
					*result += *(stream + i) - '0';
					tris_debug(5, "is now %d\n", *result);
				} else {
					break;
				}
			}
			break;
		default:
			*result = *(stream + 1);
		}
	} else {
		*result = *stream;
		*consumed = 1;
	}
	return 0;
}

char *tris_get_encoded_str(const char *stream, char *result, size_t result_size)
{
	char *cur = result;
	size_t consumed;

	while (cur < result + result_size - 1 && !tris_get_encoded_char(stream, cur, &consumed)) {
		cur++;
		stream += consumed;
	}
	*cur = '\0';
	return result;
}

int tris_str_get_encoded_str(struct tris_str **str, int maxlen, const char *stream)
{
	char next, *buf;
	size_t offset = 0;
	size_t consumed;

	if (strchr(stream, '\\')) {
		while (!tris_get_encoded_char(stream, &next, &consumed)) {
			if (offset + 2 > tris_str_size(*str) && maxlen > -1) {
				tris_str_make_space(str, maxlen > 0 ? maxlen : (tris_str_size(*str) + 48) * 2 - 48);
			}
			if (offset + 2 > tris_str_size(*str)) {
				break;
			}
			buf = tris_str_buffer(*str);
			buf[offset++] = next;
			stream += consumed;
		}
		buf = tris_str_buffer(*str);
		buf[offset++] = '\0';
		tris_str_update(*str);
	} else {
		tris_str_set(str, maxlen, "%s", stream);
	}
	return 0;
}

void tris_close_fds_above_n(int n)
{
#ifdef HAVE_CLOSEFROM
	closefrom(n + 1);
#else
	int x, null;
	struct rlimit rl;
	getrlimit(RLIMIT_NOFILE, &rl);
	null = open("/dev/null", O_RDONLY);
	for (x = n + 1; x < rl.rlim_cur; x++) {
		if (x != null) {
			/* Side effect of dup2 is that it closes any existing fd without error.
			 * This prevents valgrind and other debugging tools from sending up
			 * false error reports. */
			while (dup2(null, x) < 0 && errno == EINTR);
			close(x);
		}
	}
	close(null);
#endif
}

int tris_safe_fork(int stop_reaper)
{
	sigset_t signal_set, old_set;
	int pid;

	/* Don't let the default signal handler for children reap our status */
	if (stop_reaper) {
		tris_replace_sigchld();
	}

	sigfillset(&signal_set);
	pthread_sigmask(SIG_BLOCK, &signal_set, &old_set);

	pid = fork();

	if (pid != 0) {
		/* Fork failed or parent */
		pthread_sigmask(SIG_SETMASK, &old_set, NULL);
		return pid;
	} else {
		/* Child */
#ifdef HAVE_CAP
		cap_t cap = cap_from_text("cap_net_admin-eip");

		if (cap_set_proc(cap)) {
			tris_log(LOG_WARNING, "Unable to remove capabilities.\n");
		}
		cap_free(cap);
#endif

		/* Before we unblock our signals, return our trapped signals back to the defaults */
		signal(SIGHUP, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGURG, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGXFSZ, SIG_DFL);

		/* unblock important signal handlers */
		if (pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL)) {
			tris_log(LOG_WARNING, "unable to unblock signals: %s\n", strerror(errno));
			_exit(1);
		}

		return pid;
	}
}

void tris_safe_fork_cleanup(void)
{
	tris_unreplace_sigchld();
}

