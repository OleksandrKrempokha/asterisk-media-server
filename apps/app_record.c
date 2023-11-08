/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Matthew Fredrickson <creslin@digium.com>
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
 * \brief Trivial application to record a sound file
 *
 * \author Matthew Fredrickson <creslin@digium.com>
 *
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/file.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/channel.h"
#include "trismedia/dsp.h"	/* use dsp routines for silence detection */

/*** DOCUMENTATION
	<application name="Record" language="en_US">
		<synopsis>
			Record to a file.
		</synopsis>
		<syntax>
			<parameter name="filename" required="true" argsep=".">
				<argument name="filename" required="true" />
				<argument name="format" required="true">
					<para>Is the format of the file type to be recorded (wav, gsm, etc).</para>
				</argument>
			</parameter>
			<parameter name="silence">
				<para>Is the number of seconds of silence to allow before returning.</para>
			</parameter>
			<parameter name="maxduration">
				<para>Is the maximum recording duration in seconds. If missing
				or 0 there is no maximum.</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="a">
						<para>Append to existing recording rather than replacing.</para>
					</option>
					<option name="n">
						<para>Do not answer, but record anyway if line not yet answered.</para>
					</option>
					<option name="q">
						<para>quiet (do not play a beep tone).</para>
					</option>
					<option name="s">
						<para>skip recording if the line is not yet answered.</para>
					</option>
					<option name="t">
						<para>use alternate '*' terminator key (DTMF) instead of default '#'</para>
					</option>
					<option name="x">
						<para>Ignore all terminator keys (DTMF) and keep recording until hangup.</para>
					</option>
					<option name="k">
					        <para>Keep recording if channel hangs up.</para>
					</option>	
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>If filename contains <literal>%d</literal>, these characters will be replaced with a number
			incremented by one each time the file is recorded.
			Use <astcli>core show file formats</astcli> to see the available formats on your system
			User can press <literal>#</literal> to terminate the recording and continue to the next priority.
			If the user hangs up during a recording, all data will be lost and the application will terminate.</para>
			<variablelist>
				<variable name="RECORDED_FILE">
					<para>Will be set to the final filename of the recording.</para>
				</variable>
				<variable name="RECORD_STATUS">
					<para>This is the final status of the command</para>
					<value name="DTMF">A terminating DTMF was received ('#' or '*', depending upon option 't')</value>
					<value name="SILENCE">The maximum silence occurred in the recording.</value>
					<value name="SKIP">The line was not yet answered and the 's' option was specified.</value>
					<value name="TIMEOUT">The maximum length was reached.</value>
					<value name="HANGUP">The channel was hung up.</value>
					<value name="ERROR">An unrecoverable error occurred, which resulted in a WARNING to the logs.</value>
				</variable>
			</variablelist>
		</description>
	</application>

 ***/

static char *app = "Record";

enum {
	OPTION_APPEND = (1 << 0),
	OPTION_NOANSWER = (1 << 1),
	OPTION_QUIET = (1 << 2),
	OPTION_SKIP = (1 << 3),
	OPTION_STAR_TERMINATE = (1 << 4),
	OPTION_IGNORE_TERMINATE = (1 << 5),
	OPTION_KEEP = (1 << 6),
	FLAG_HAS_PERCENT = (1 << 7),
};

TRIS_APP_OPTIONS(app_opts,{
	TRIS_APP_OPTION('a', OPTION_APPEND),
	TRIS_APP_OPTION('k', OPTION_KEEP),	
	TRIS_APP_OPTION('n', OPTION_NOANSWER),
	TRIS_APP_OPTION('q', OPTION_QUIET),
	TRIS_APP_OPTION('s', OPTION_SKIP),
	TRIS_APP_OPTION('t', OPTION_STAR_TERMINATE),
	TRIS_APP_OPTION('x', OPTION_IGNORE_TERMINATE),
});

static int record_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	int count = 0;
	char *ext = NULL, *opts[0];
	char *parse, *dir, *file;
	int i = 0;
	char tmp[256];

	struct tris_filestream *s = NULL;
	struct tris_frame *f = NULL;
	
	struct tris_dsp *sildet = NULL;   	/* silence detector dsp */
	int totalsilence = 0;
	int dspsilence = 0;
	int silence = 0;		/* amount of silence to allow */
	int gotsilence = 0;		/* did we timeout for silence? */
	int maxduration = 0;		/* max duration of recording in milliseconds */
	int gottimeout = 0;		/* did we timeout for maxduration exceeded? */
	int terminator = '#';
	int rfmt = 0;
	int ioflags;
	int waitres;
	struct tris_silence_generator *silgen = NULL;
	struct tris_flags flags = { 0, };
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(filename);
		TRIS_APP_ARG(silence);
		TRIS_APP_ARG(maxduration);
		TRIS_APP_ARG(options);
	);
	
	/* The next few lines of code parse out the filename and header from the input string */
	if (tris_strlen_zero(data)) { /* no data implies no filename or anything is present */
		tris_log(LOG_WARNING, "Record requires an argument (filename)\n");
		pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
		return -1;
	}

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);
	if (args.argc == 4)
		tris_app_parse_options(app_opts, &flags, opts, args.options);

	if (!tris_strlen_zero(args.filename)) {
		if (strstr(args.filename, "%d"))
			tris_set_flag(&flags, FLAG_HAS_PERCENT);
		ext = strrchr(args.filename, '.'); /* to support filename with a . in the filename, not format */
		if (!ext)
			ext = strchr(args.filename, ':');
		if (ext) {
			*ext = '\0';
			ext++;
		}
	}
	if (!ext) {
		tris_log(LOG_WARNING, "No extension specified to filename!\n");
		pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
		return -1;
	}
	if (args.silence) {
		if ((sscanf(args.silence, "%30d", &i) == 1) && (i > -1)) {
			silence = i * 1000;
		} else if (!tris_strlen_zero(args.silence)) {
			tris_log(LOG_WARNING, "'%s' is not a valid silence duration\n", args.silence);
		}
	}
	
	if (args.maxduration) {
		if ((sscanf(args.maxduration, "%30d", &i) == 1) && (i > -1))
			/* Convert duration to milliseconds */
			maxduration = i * 1000;
		else if (!tris_strlen_zero(args.maxduration))
			tris_log(LOG_WARNING, "'%s' is not a valid maximum duration\n", args.maxduration);
	}

	if (tris_test_flag(&flags, OPTION_STAR_TERMINATE))
		terminator = '*';
	if (tris_test_flag(&flags, OPTION_IGNORE_TERMINATE))
		terminator = '\0';

	/* done parsing */

	/* these are to allow the use of the %d in the config file for a wild card of sort to
	  create a new file with the inputed name scheme */
	if (tris_test_flag(&flags, FLAG_HAS_PERCENT)) {
		TRIS_DECLARE_APP_ARGS(fname,
			TRIS_APP_ARG(piece)[100];
		);
		char *tmp2 = tris_strdupa(args.filename);
		char countstring[15];
		int idx;

		/* Separate each piece out by the format specifier */
		TRIS_NONSTANDARD_APP_ARGS(fname, tmp2, '%');
		do {
			int tmplen;
			/* First piece has no leading percent, so it's copied verbatim */
			tris_copy_string(tmp, fname.piece[0], sizeof(tmp));
			tmplen = strlen(tmp);
			for (idx = 1; idx < fname.argc; idx++) {
				if (fname.piece[idx][0] == 'd') {
					/* Substitute the count */
					snprintf(countstring, sizeof(countstring), "%d", count);
					tris_copy_string(tmp + tmplen, countstring, sizeof(tmp) - tmplen);
					tmplen += strlen(countstring);
				} else if (tmplen + 2 < sizeof(tmp)) {
					/* Unknown format specifier - just copy it verbatim */
					tmp[tmplen++] = '%';
					tmp[tmplen++] = fname.piece[idx][0];
				}
				/* Copy the remaining portion of the piece */
				tris_copy_string(tmp + tmplen, &(fname.piece[idx][1]), sizeof(tmp) - tmplen);
			}
			count++;
		} while (tris_fileexists(tmp, ext, chan->language) > 0);
		pbx_builtin_setvar_helper(chan, "RECORDED_FILE", tmp);
	} else
		tris_copy_string(tmp, args.filename, sizeof(tmp));
	/* end of routine mentioned */

	if (chan->_state != TRIS_STATE_UP) {
		if (tris_test_flag(&flags, OPTION_SKIP)) {
			/* At the user's option, skip if the line is not up */
			pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "SKIP");
			return 0;
		} else if (!tris_test_flag(&flags, OPTION_NOANSWER)) {
			/* Otherwise answer unless we're supposed to record while on-hook */
			res = tris_answer(chan);
		}
	}

	if (res) {
		tris_log(LOG_WARNING, "Could not answer channel '%s'\n", chan->name);
		pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
		goto out;
	}

	if (!tris_test_flag(&flags, OPTION_QUIET)) {
		/* Some code to play a nice little beep to signify the start of the record operation */
		res = tris_streamfile(chan, "beep", chan->language);
		if (!res) {
			res = tris_waitstream(chan, "");
		} else {
			tris_log(LOG_WARNING, "tris_streamfile failed on %s\n", chan->name);
		}
		tris_stopstream(chan);
	}

	/* The end of beep code.  Now the recording starts */

	if (silence > 0) {
		rfmt = chan->readformat;
		res = tris_set_read_format(chan, TRIS_FORMAT_SLINEAR);
		if (res < 0) {
			tris_log(LOG_WARNING, "Unable to set to linear mode, giving up\n");
			pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
			return -1;
		}
		sildet = tris_dsp_new();
		if (!sildet) {
			tris_log(LOG_WARNING, "Unable to create silence detector :(\n");
			pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
			return -1;
		}
		tris_dsp_set_threshold(sildet, tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE));
	} 

	/* Create the directory if it does not exist. */
	dir = tris_strdupa(tmp);
	if ((file = strrchr(dir, '/')))
		*file++ = '\0';
	tris_mkdir (dir, 0777);

	ioflags = tris_test_flag(&flags, OPTION_APPEND) ? O_CREAT|O_APPEND|O_WRONLY : O_CREAT|O_TRUNC|O_WRONLY;
	s = tris_writefile(tmp, ext, NULL, ioflags, 0, TRIS_FILE_MODE);

	if (!s) {
		tris_log(LOG_WARNING, "Could not create file %s\n", args.filename);
		pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
		goto out;
	}

	if (tris_opt_transmit_silence)
		silgen = tris_channel_start_silence_generator(chan);

	/* Request a video update */
	tris_indicate(chan, TRIS_CONTROL_VIDUPDATE);

	if (maxduration <= 0)
		maxduration = -1;

	while ((waitres = tris_waitfor(chan, maxduration)) > -1) {
		if (maxduration > 0) {
			if (waitres == 0) {
				gottimeout = 1;
				pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "TIMEOUT");
				break;
			}
			maxduration = waitres;
		}

		f = tris_read(chan);
		if (!f) {
			res = -1;
			break;
		}
		if (f->frametype == TRIS_FRAME_VOICE) {
			res = tris_writestream(s, f);

			if (res) {
				tris_log(LOG_WARNING, "Problem writing frame\n");
				tris_frfree(f);
				pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
				break;
			}

			if (silence > 0) {
				dspsilence = 0;
				tris_dsp_silence(sildet, f, &dspsilence);
				if (dspsilence) {
					totalsilence = dspsilence;
				} else {
					totalsilence = 0;
				}
				if (totalsilence > silence) {
					/* Ended happily with silence */
					tris_frfree(f);
					gotsilence = 1;
					pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "SILENCE");
					break;
				}
			}
		} else if (f->frametype == TRIS_FRAME_VIDEO) {
			res = tris_writestream(s, f);

			if (res) {
				tris_log(LOG_WARNING, "Problem writing frame\n");
				pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "ERROR");
				tris_frfree(f);
				break;
			}
		} else if ((f->frametype == TRIS_FRAME_DTMF) &&
		    (f->subclass == terminator)) {
			tris_frfree(f);
			pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "DTMF");
			break;
		}
		tris_frfree(f);
	}
	if (!f) {
		tris_debug(1, "Got hangup\n");
		res = -1;
		pbx_builtin_setvar_helper(chan, "RECORD_STATUS", "HANGUP");
		if (!tris_test_flag(&flags, OPTION_KEEP)) {
			tris_filedelete(args.filename, NULL);
		}
	}

	if (gotsilence) {
		tris_stream_rewind(s, silence - 1000);
		tris_truncstream(s);
	} else if (!gottimeout) {
		/* Strip off the last 1/4 second of it */
		tris_stream_rewind(s, 250);
		tris_truncstream(s);
	}
	tris_closestream(s);

	if (silgen)
		tris_channel_stop_silence_generator(chan, silgen);

out:
	if ((silence > 0) && rfmt) {
		res = tris_set_read_format(chan, rfmt);
		if (res)
			tris_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
		if (sildet)
			tris_dsp_free(sildet);
	}

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, record_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Trivial Record Application");
