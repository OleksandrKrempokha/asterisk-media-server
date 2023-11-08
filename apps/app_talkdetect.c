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
 * \brief Playback a file with audio detect
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/utils.h"
#include "trismedia/dsp.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="BackgroundDetect" language="en_US">
		<synopsis>
			Background a file with talk detect.
		</synopsis>
		<syntax>
			<parameter name="filename" required="true" />
			<parameter name="sil">
				<para>If not specified, defaults to <literal>1000</literal>.</para>
			</parameter>
			<parameter name="min">
				<para>If not specified, defaults to <literal>100</literal>.</para>
			</parameter>
			<parameter name="max">
				<para>If not specified, defaults to <literal>infinity</literal>.</para>
			</parameter>
			<parameter name="analysistime">
				<para>If not specified, defaults to <literal>infinity</literal>.</para>
			</parameter>
		</syntax>
		<description>
			<para>Plays back <replaceable>filename</replaceable>, waiting for interruption from a given digit (the digit
			must start the beginning of a valid extension, or it will be ignored). During
			the playback of the file, audio is monitored in the receive direction, and if
			a period of non-silence which is greater than <replaceable>min</replaceable> ms yet less than
			<replaceable>max</replaceable> ms is followed by silence for at least <replaceable>sil</replaceable> ms,
			which occurs during the first <replaceable>analysistime</replaceable> ms, then the audio playback is
			aborted and processing jumps to the <replaceable>talk</replaceable> extension, if available.</para>
		</description>
	</application>
 ***/

static char *app = "BackgroundDetect";

static int background_detect_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char *tmp;
	struct tris_frame *fr;
	int notsilent = 0;
	struct timeval start = { 0, 0 };
	struct timeval detection_start = { 0, 0 };
	int sil = 1000;
	int min = 100;
	int max = -1;
	int analysistime = -1;
	int continue_analysis = 1;
	int x;
	int origrformat = 0;
	struct tris_dsp *dsp = NULL;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(filename);
		TRIS_APP_ARG(silence);
		TRIS_APP_ARG(min);
		TRIS_APP_ARG(max);
		TRIS_APP_ARG(analysistime);
	);
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "BackgroundDetect requires an argument (filename)\n");
		return -1;
	}

	tmp = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, tmp);

	if (!tris_strlen_zero(args.silence) && (sscanf(args.silence, "%30d", &x) == 1) && (x > 0)) {
		sil = x;
	}
	if (!tris_strlen_zero(args.min) && (sscanf(args.min, "%30d", &x) == 1) && (x > 0)) {
		min = x;
	}
	if (!tris_strlen_zero(args.max) && (sscanf(args.max, "%30d", &x) == 1) && (x > 0)) {
		max = x;
	}
	if (!tris_strlen_zero(args.analysistime) && (sscanf(args.analysistime, "%30d", &x) == 1) && (x > 0)) {
		analysistime = x;
	}

	tris_debug(1, "Preparing detect of '%s', sil=%d, min=%d, max=%d, analysistime=%d\n", args.filename, sil, min, max, analysistime);
	do {
		if (chan->_state != TRIS_STATE_UP) {
			if ((res = tris_answer(chan))) {
				break;
			}
		}

		origrformat = chan->readformat;
		if ((tris_set_read_format(chan, TRIS_FORMAT_SLINEAR))) {
			tris_log(LOG_WARNING, "Unable to set read format to linear!\n");
			res = -1;
			break;
		}

		if (!(dsp = tris_dsp_new())) {
			tris_log(LOG_WARNING, "Unable to allocate DSP!\n");
			res = -1;
			break;
		}
		tris_stopstream(chan);
		if (tris_streamfile(chan, tmp, chan->language)) {
			tris_log(LOG_WARNING, "tris_streamfile failed on %s for %s\n", chan->name, (char *)data);
			break;
		}
		detection_start = tris_tvnow();
		while (chan->stream) {
			res = tris_sched_wait(chan->sched);
			if ((res < 0) && !chan->timingfunc) {
				res = 0;
				break;
			}
			if (res < 0) {
				res = 1000;
			}
			res = tris_waitfor(chan, res);
			if (res < 0) {
				tris_log(LOG_WARNING, "Waitfor failed on %s\n", chan->name);
				break;
			} else if (res > 0) {
				fr = tris_read(chan);
				if (continue_analysis && analysistime >= 0) {
					/* If we have a limit for the time to analyze voice
					 * frames and the time has not expired */
					if (tris_tvdiff_ms(tris_tvnow(), detection_start) >= analysistime) {
						continue_analysis = 0;
						tris_verb(3, "BackgroundDetect: Talk analysis time complete on %s.\n", chan->name);
					}
				}
				
				if (!fr) {
					res = -1;
					break;
				} else if (fr->frametype == TRIS_FRAME_DTMF) {
					char t[2];
					t[0] = fr->subclass;
					t[1] = '\0';
					if (tris_canmatch_extension(chan, chan->context, t, 1, chan->cid.cid_num)) {
						/* They entered a valid  extension, or might be anyhow */
						res = fr->subclass;
						tris_frfree(fr);
						break;
					}
				} else if ((fr->frametype == TRIS_FRAME_VOICE) && (fr->subclass == TRIS_FORMAT_SLINEAR) && continue_analysis) {
					int totalsilence;
					int ms;
					res = tris_dsp_silence(dsp, fr, &totalsilence);
					if (res && (totalsilence > sil)) {
						/* We've been quiet a little while */
						if (notsilent) {
							/* We had heard some talking */
							ms = tris_tvdiff_ms(tris_tvnow(), start);
							ms -= sil;
							if (ms < 0)
								ms = 0;
							if ((ms > min) && ((max < 0) || (ms < max))) {
								char ms_str[12];
								tris_debug(1, "Found qualified token of %d ms\n", ms);

								/* Save detected talk time (in milliseconds) */ 
								snprintf(ms_str, sizeof(ms_str), "%d", ms);	
								pbx_builtin_setvar_helper(chan, "TALK_DETECTED", ms_str);

								tris_goto_if_exists(chan, chan->context, "talk", 1);
								res = 0;
								tris_frfree(fr);
								break;
							} else {
								tris_debug(1, "Found unqualified token of %d ms\n", ms);
							}
							notsilent = 0;
						}
					} else {
						if (!notsilent) {
							/* Heard some audio, mark the begining of the token */
							start = tris_tvnow();
							tris_debug(1, "Start of voice token!\n");
							notsilent = 1;
						}
					}
				}
				tris_frfree(fr);
			}
			tris_sched_runq(chan->sched);
		}
		tris_stopstream(chan);
	} while (0);

	if (res > -1) {
		if (origrformat && tris_set_read_format(chan, origrformat)) {
			tris_log(LOG_WARNING, "Failed to restore read format for %s to %s\n", 
				chan->name, tris_getformatname(origrformat));
		}
	}
	if (dsp) {
		tris_dsp_free(dsp);
	}
	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, background_detect_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Playback with Talk Detection");
