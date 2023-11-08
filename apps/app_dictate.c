/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Anthony Minessale II
 *
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 * Donated by Sangoma Technologies <http://www.samgoma.com>
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
 * \brief Virtual Dictation Machine Application For Trismedia
 *
 * \author Anthony Minessale II <anthmct@yahoo.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 174945 $")

#include <sys/stat.h>

#include "trismedia/paths.h" /* use tris_config_TRIS_SPOOL_DIR */
#include "trismedia/file.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/say.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="Dictate" language="en_US">
		<synopsis>
			Virtual Dictation Machine.
		</synopsis>
		<syntax>
			<parameter name="base_dir" />
			<parameter name="filename" />
		</syntax>
		<description>
			<para>Start dictation machine using optional <replaceable>base_dir</replaceable> for files.</para>
		</description>
	</application>
 ***/

static char *app = "Dictate";

typedef enum {
	DFLAG_RECORD = (1 << 0),
	DFLAG_PLAY = (1 << 1),
	DFLAG_TRUNC = (1 << 2),
	DFLAG_PAUSE = (1 << 3),
} dflags;

typedef enum {
	DMODE_INIT,
	DMODE_RECORD,
	DMODE_PLAY
} dmodes;

#define tris_toggle_flag(it,flag) if(tris_test_flag(it, flag)) tris_clear_flag(it, flag); else tris_set_flag(it, flag)

static int play_and_wait(struct tris_channel *chan, char *file, char *digits)
{
	int res = -1;
	if (!tris_streamfile(chan, file, chan->language)) {
		res = tris_waitstream(chan, digits);
	}
	return res;
}

static int dictate_exec(struct tris_channel *chan, void *data)
{
	char *path = NULL, filein[256], *filename = "";
	char *parse;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(base);
		TRIS_APP_ARG(filename);
	);
	char dftbase[256];
	char *base;
	struct tris_flags flags = {0};
	struct tris_filestream *fs;
	struct tris_frame *f = NULL;
	int ffactor = 320 * 80,
		res = 0,
		done = 0,
		oldr = 0,
		lastop = 0,
		samples = 0,
		speed = 1,
		digit = 0,
		len = 0,
		maxlen = 0,
		mode = 0;

	snprintf(dftbase, sizeof(dftbase), "%s/dictate", tris_config_TRIS_SPOOL_DIR);
	if (!tris_strlen_zero(data)) {
		parse = tris_strdupa(data);
		TRIS_STANDARD_APP_ARGS(args, parse);
	} else
		args.argc = 0;

	if (args.argc && !tris_strlen_zero(args.base)) {
		base = args.base;
	} else {
		base = dftbase;
	}
	if (args.argc > 1 && args.filename) {
		filename = args.filename;
	}
	oldr = chan->readformat;
	if ((res = tris_set_read_format(chan, TRIS_FORMAT_SLINEAR)) < 0) {
		tris_log(LOG_WARNING, "Unable to set to linear mode.\n");
		return -1;
	}

	if (chan->_state != TRIS_STATE_UP) {
		tris_answer(chan);
	}
	tris_safe_sleep(chan, 200);
	for (res = 0; !res;) {
		if (tris_strlen_zero(filename)) {
			if (tris_app_getdata(chan, "dictate/enter_filename", filein, sizeof(filein), 0) ||
				tris_strlen_zero(filein)) {
				res = -1;
				break;
			}
		} else {
			tris_copy_string(filein, filename, sizeof(filein));
			filename = "";
		}
		tris_mkdir(base, 0755);
		len = strlen(base) + strlen(filein) + 2;
		if (!path || len > maxlen) {
			path = alloca(len);
			memset(path, 0, len);
			maxlen = len;
		} else {
			memset(path, 0, maxlen);
		}

		snprintf(path, len, "%s/%s", base, filein);
		fs = tris_writefile(path, "raw", NULL, O_CREAT|O_APPEND, 0, TRIS_FILE_MODE);
		mode = DMODE_PLAY;
		memset(&flags, 0, sizeof(flags));
		tris_set_flag(&flags, DFLAG_PAUSE);
		digit = play_and_wait(chan, "dictate/forhelp", TRIS_DIGIT_ANY);
		done = 0;
		speed = 1;
		res = 0;
		lastop = 0;
		samples = 0;
		while (!done && ((res = tris_waitfor(chan, -1)) > -1) && fs && (f = tris_read(chan))) {
			if (digit) {
				struct tris_frame fr = {TRIS_FRAME_DTMF, digit};
				tris_queue_frame(chan, &fr);
				digit = 0;
			}
			if ((f->frametype == TRIS_FRAME_DTMF)) {
				int got = 1;
				switch(mode) {
				case DMODE_PLAY:
					switch(f->subclass) {
					case '1':
						tris_set_flag(&flags, DFLAG_PAUSE);
						mode = DMODE_RECORD;
						break;
					case '2':
						speed++;
						if (speed > 4) {
							speed = 1;
						}
						res = tris_say_number(chan, speed, TRIS_DIGIT_ANY, chan->language, NULL);
						break;
					case '7':
						samples -= ffactor;
						if(samples < 0) {
							samples = 0;
						}
						tris_seekstream(fs, samples, SEEK_SET);
						break;
					case '8':
						samples += ffactor;
						tris_seekstream(fs, samples, SEEK_SET);
						break;
						
					default:
						got = 0;
					}
					break;
				case DMODE_RECORD:
					switch(f->subclass) {
					case '1':
						tris_set_flag(&flags, DFLAG_PAUSE);
						mode = DMODE_PLAY;
						break;
					case '8':
						tris_toggle_flag(&flags, DFLAG_TRUNC);
						lastop = 0;
						break;
					default:
						got = 0;
					}
					break;
				default:
					got = 0;
				}
				if (!got) {
					switch(f->subclass) {
					case '#':
						done = 1;
						continue;
						break;
					case '*':
						tris_toggle_flag(&flags, DFLAG_PAUSE);
						if (tris_test_flag(&flags, DFLAG_PAUSE)) {
							digit = play_and_wait(chan, "dictate/pause", TRIS_DIGIT_ANY);
						} else {
							digit = play_and_wait(chan, mode == DMODE_PLAY ? "dictate/playback" : "dictate/record", TRIS_DIGIT_ANY);
						}
						break;
					case '0':
						tris_set_flag(&flags, DFLAG_PAUSE);
						digit = play_and_wait(chan, "dictate/paused", TRIS_DIGIT_ANY);
						switch(mode) {
						case DMODE_PLAY:
							digit = play_and_wait(chan, "dictate/play_help", TRIS_DIGIT_ANY);
							break;
						case DMODE_RECORD:
							digit = play_and_wait(chan, "dictate/record_help", TRIS_DIGIT_ANY);
							break;
						}
						if (digit == 0) {
							digit = play_and_wait(chan, "dictate/both_help", TRIS_DIGIT_ANY);
						} else if (digit < 0) {
							done = 1;
							break;
						}
						break;
					}
				}
				
			} else if (f->frametype == TRIS_FRAME_VOICE) {
				switch(mode) {
					struct tris_frame *fr;
					int x;
				case DMODE_PLAY:
					if (lastop != DMODE_PLAY) {
						if (tris_test_flag(&flags, DFLAG_PAUSE)) {
							digit = play_and_wait(chan, "dictate/playback_mode", TRIS_DIGIT_ANY);
							if (digit == 0) {
								digit = play_and_wait(chan, "dictate/paused", TRIS_DIGIT_ANY);
							} else if (digit < 0) {
								break;
							}
						}
						if (lastop != DFLAG_PLAY) {
							lastop = DFLAG_PLAY;
							tris_closestream(fs);
							if (!(fs = tris_openstream(chan, path, chan->language)))
								break;
							tris_seekstream(fs, samples, SEEK_SET);
							chan->stream = NULL;
						}
						lastop = DMODE_PLAY;
					}

					if (!tris_test_flag(&flags, DFLAG_PAUSE)) {
						for (x = 0; x < speed; x++) {
							if ((fr = tris_readframe(fs))) {
								tris_write(chan, fr);
								samples += fr->samples;
								tris_frfree(fr);
								fr = NULL;
							} else {
								samples = 0;
								tris_seekstream(fs, 0, SEEK_SET);
							}
						}
					}
					break;
				case DMODE_RECORD:
					if (lastop != DMODE_RECORD) {
						int oflags = O_CREAT | O_WRONLY;
						if (tris_test_flag(&flags, DFLAG_PAUSE)) {
							digit = play_and_wait(chan, "dictate/record_mode", TRIS_DIGIT_ANY);
							if (digit == 0) {
								digit = play_and_wait(chan, "dictate/paused", TRIS_DIGIT_ANY);
							} else if (digit < 0) {
								break;
							}
						}
						lastop = DMODE_RECORD;
						tris_closestream(fs);
						if ( tris_test_flag(&flags, DFLAG_TRUNC)) {
							oflags |= O_TRUNC;
							digit = play_and_wait(chan, "dictate/truncating_audio", TRIS_DIGIT_ANY);
						} else {
							oflags |= O_APPEND;
						}
						fs = tris_writefile(path, "raw", NULL, oflags, 0, TRIS_FILE_MODE);
						if (tris_test_flag(&flags, DFLAG_TRUNC)) {
							tris_seekstream(fs, 0, SEEK_SET);
							tris_clear_flag(&flags, DFLAG_TRUNC);
						} else {
							tris_seekstream(fs, 0, SEEK_END);
						}
					}
					if (!tris_test_flag(&flags, DFLAG_PAUSE)) {
						res = tris_writestream(fs, f);
					}
					break;
				}
				
			}

			tris_frfree(f);
		}
	}
	if (oldr) {
		tris_set_read_format(chan, oldr);
	}
	return 0;
}

static int unload_module(void)
{
	int res;
	res = tris_unregister_application(app);
	return res;
}

static int load_module(void)
{
	return tris_register_application_xml(app, dictate_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Virtual Dictation Machine");
