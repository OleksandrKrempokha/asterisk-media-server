/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
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
 * \brief Speech Recognition Utility Applications
 *
 * \author Joshua Colp <jcolp@digium.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 179254 $");

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"
#include "trismedia/speech.h"

/*** DOCUMENTATION
	<application name="SpeechCreate" language="en_US">
		<synopsis>
			Create a Speech Structure.
		</synopsis>
		<syntax>
			<parameter name="engine_name" required="true" />
		</syntax>
		<description>
			<para>This application creates information to be used by all the other applications.
			It must be called before doing any speech recognition activities such as activating a grammar.
			It takes the engine name to use as the argument, if not specified the default engine will be used.</para>
		</description>
	</application>
	<application name="SpeechActivateGrammar" language="en_US">
		<synopsis>
			Activate a grammar.
		</synopsis>
		<syntax>
			<parameter name="grammar_name" required="true" />
		</syntax>
		<description>
			<para>This activates the specified grammar to be recognized by the engine.
			A grammar tells the speech recognition engine what to recognize, and how to portray it back to you
			in the dialplan. The grammar name is the only argument to this application.</para>
		</description>
	</application>
	<application name="SpeechStart" language="en_US">
		<synopsis>
			Start recognizing voice in the audio stream.
		</synopsis>
		<syntax />
		<description>
			<para>Tell the speech recognition engine that it should start trying to get results from audio being
			fed to it.</para>
		</description>
	</application>
	<application name="SpeechBackground" language="en_US">
		<synopsis>
			Play a sound file and wait for speech to be recognized.
		</synopsis>
		<syntax>
			<parameter name="sound_file" required="true" />
			<parameter name="timeout">
				<para>Timeout integer in seconds. Note the timeout will only start
				once the sound file has stopped playing.</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="n">
						<para>Don't answer the channel if it has not already been answered.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>This application plays a sound file and waits for the person to speak. Once they start speaking playback
			of the file stops, and silence is heard. Once they stop talking the processing sound is played to indicate
			the speech recognition engine is working. Once results are available the application returns and results
			(score and text) are available using dialplan functions.</para>
			<para>The first text and score are ${SPEECH_TEXT(0)} AND ${SPEECH_SCORE(0)} while the second are ${SPEECH_TEXT(1)}
			and ${SPEECH_SCORE(1)}.</para>
			<para>The first argument is the sound file and the second is the timeout integer in seconds.</para>
			
		</description>
	</application>
	<application name="SpeechDeactivateGrammar" language="en_US">
		<synopsis>
			Deactivate a grammar.
		</synopsis>
		<syntax>
			<parameter name="grammar_name" required="true">
				<para>The grammar name to deactivate</para>
			</parameter>
		</syntax>
		<description>
			<para>This deactivates the specified grammar so that it is no longer recognized.</para>
		</description>
	</application>
	<application name="SpeechProcessingSound" language="en_US">
		<synopsis>
			Change background processing sound.
		</synopsis>
		<syntax>
			<parameter name="sound_file" required="true" />
		</syntax>
		<description>
			<para>This changes the processing sound that SpeechBackground plays back when the speech recognition engine is
			processing and working to get results.</para>
		</description>
	</application>
	<application name="SpeechDestroy" language="en_US">
		<synopsis>
			End speech recognition.
		</synopsis>
		<syntax />
		<description>
			<para>This destroys the information used by all the other speech recognition applications.
			If you call this application but end up wanting to recognize more speech, you must call SpeechCreate()
			again before calling any other application.</para>
		</description>
	</application>
	<application name="SpeechLoadGrammar" language="en_US">
		<synopsis>
			Load a grammar.
		</synopsis>
		<syntax>
			<parameter name="grammar_name" required="true" />
			<parameter name="path" required="true" />
		</syntax>
		<description>
			<para>Load a grammar only on the channel, not globally.</para>
		</description>
	</application>
	<application name="SpeechUnloadGrammar" language="en_US">
		<synopsis>
			Unload a grammar.
		</synopsis>
		<syntax>
			<parameter name="grammar_name" required="true" />
		</syntax>
		<description>
			<para>Unload a grammar.</para>
		</description>
	</application>
	<function name="SPEECH_SCORE" language="en_US">
		<synopsis>
			Gets the confidence score of a result.
		</synopsis>
		<syntax argsep="/">
			<parameter name="nbest_number" />
			<parameter name="result_number" required="true" />
		</syntax>
		<description>
			<para>Gets the confidence score of a result.</para>
		</description>
	</function>
	<function name="SPEECH_TEXT" language="en_US">
		<synopsis>
			Gets the recognized text of a result.
		</synopsis>
		<syntax argsep="/">
			<parameter name="nbest_number" />
			<parameter name="result_number" required="true" />
		</syntax>
		<description>
			<para>Gets the recognized text of a result.</para>
		</description>
	</function>
	<function name="SPEECH_GRAMMAR" language="en_US">
		<synopsis>
			Gets the matched grammar of a result if available.
		</synopsis>
		<syntax argsep="/">
			<parameter name="nbest_number" />
			<parameter name="result_number" required="true" />
		</syntax>
		<description>
			<para>Gets the matched grammar of a result if available.</para>
		</description>
	</function>
	<function name="SPEECH_ENGINE" language="en_US">
		<synopsis>
			Change a speech engine specific attribute.
		</synopsis>
		<syntax>
			<parameter name="name" required="true" />
		</syntax>
		<description>
			<para>Changes a speech engine specific attribute.</para>
		</description>
	</function>
	<function name="SPEECH_RESULTS_TYPE" language="en_US">
		<synopsis>
			Sets the type of results that will be returned.
		</synopsis>
		<syntax />
		<description>
			<para>Sets the type of results that will be returned. Valid options are normal or nbest.</para>
		</description>
	</function>
	<function name="SPEECH" language="en_US">
		<synopsis>
			Gets information about speech recognition results.
		</synopsis>
		<syntax>
			<parameter name="argument" required="true">
				<enumlist>
					<enum name="status">
						<para>Returns <literal>1</literal> upon speech object existing,
						or <literal>0</literal> if not</para>
					</enum>
					<enum name="spoke">
						<para>Returns <literal>1</literal> if spoker spoke,
						or <literal>0</literal> if not</para>
					</enum>
					<enum name="results">
						<para>Returns number of results that were recognized.</para>
					</enum>
				</enumlist>
			</parameter>
		</syntax>
		<description>
			<para>Gets information about speech recognition results.</para>
		</description>
	</function>
 ***/

/*! \brief Helper function used by datastores to destroy the speech structure upon hangup */
static void destroy_callback(void *data)
{
	struct tris_speech *speech = (struct tris_speech*)data;

	if (speech == NULL) {
		return;
	}

	/* Deallocate now */
	tris_speech_destroy(speech);

	return;
}

/*! \brief Static structure for datastore information */
static const struct tris_datastore_info speech_datastore = {
	.type = "speech",
	.destroy = destroy_callback
};

/*! \brief Helper function used to find the speech structure attached to a channel */
static struct tris_speech *find_speech(struct tris_channel *chan)
{
	struct tris_speech *speech = NULL;
	struct tris_datastore *datastore = NULL;
	
	datastore = tris_channel_datastore_find(chan, &speech_datastore, NULL);
	if (datastore == NULL) {
		return NULL;
	}
	speech = datastore->data;

	return speech;
}

/* Helper function to find a specific speech recognition result by number and nbest alternative */
static struct tris_speech_result *find_result(struct tris_speech_result *results, char *result_num)
{
	struct tris_speech_result *result = results;
	char *tmp = NULL;
	int nbest_num = 0, wanted_num = 0, i = 0;

	if (!result) {
		return NULL;
	}

	if ((tmp = strchr(result_num, '/'))) {
		*tmp++ = '\0';
		nbest_num = atoi(result_num);
		wanted_num = atoi(tmp);
	} else {
		wanted_num = atoi(result_num);
	}

	do {
		if (result->nbest_num != nbest_num)
			continue;
		if (i == wanted_num)
			break;
		i++;
	} while ((result = TRIS_LIST_NEXT(result, list)));

	return result;
}

/*! \brief SPEECH_SCORE() Dialplan Function */
static int speech_score(struct tris_channel *chan, const char *cmd, char *data,
		       char *buf, size_t len)
{
	struct tris_speech_result *result = NULL;
	struct tris_speech *speech = find_speech(chan);
	char tmp[128] = "";

	if (data == NULL || speech == NULL || !(result = find_result(speech->results, data))) {
		return -1;
	}
	
	snprintf(tmp, sizeof(tmp), "%d", result->score);
	
	tris_copy_string(buf, tmp, len);

	return 0;
}

static struct tris_custom_function speech_score_function = {
	.name = "SPEECH_SCORE",
	.read = speech_score,
	.write = NULL,
};

/*! \brief SPEECH_TEXT() Dialplan Function */
static int speech_text(struct tris_channel *chan, const char *cmd, char *data,
			char *buf, size_t len)
{
	struct tris_speech_result *result = NULL;
	struct tris_speech *speech = find_speech(chan);

	if (data == NULL || speech == NULL || !(result = find_result(speech->results, data))) {
		return -1;
	}

	if (result->text != NULL) {
		tris_copy_string(buf, result->text, len);
	} else {
		buf[0] = '\0';
	}

	return 0;
}

static struct tris_custom_function speech_text_function = {
	.name = "SPEECH_TEXT",
	.read = speech_text,
	.write = NULL,
};

/*! \brief SPEECH_GRAMMAR() Dialplan Function */
static int speech_grammar(struct tris_channel *chan, const char *cmd, char *data,
			char *buf, size_t len)
{
	struct tris_speech_result *result = NULL;
	struct tris_speech *speech = find_speech(chan);

	if (data == NULL || speech == NULL || !(result = find_result(speech->results, data))) {
		return -1;
	}

	if (result->grammar != NULL) {
		tris_copy_string(buf, result->grammar, len);
	} else {
		buf[0] = '\0';
	}

	return 0;
}

static struct tris_custom_function speech_grammar_function = {
	.name = "SPEECH_GRAMMAR",
	.read = speech_grammar,
	.write = NULL,
};

/*! \brief SPEECH_ENGINE() Dialplan Function */
static int speech_engine_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	struct tris_speech *speech = find_speech(chan);

	if (data == NULL || speech == NULL) {
		return -1;
	}

	tris_speech_change(speech, data, value);

	return 0;
}

static struct tris_custom_function speech_engine_function = {
	.name = "SPEECH_ENGINE",
	.read = NULL,
	.write = speech_engine_write,
};

/*! \brief SPEECH_RESULTS_TYPE() Dialplan Function */
static int speech_results_type_write(struct tris_channel *chan, const char *cmd, char *data, const char *value)
{
	struct tris_speech *speech = find_speech(chan);

	if (data == NULL || speech == NULL)
		return -1;

	if (!strcasecmp(value, "normal"))
		tris_speech_change_results_type(speech, TRIS_SPEECH_RESULTS_TYPE_NORMAL);
	else if (!strcasecmp(value, "nbest"))
		tris_speech_change_results_type(speech, TRIS_SPEECH_RESULTS_TYPE_NBEST);

	return 0;
}

static struct tris_custom_function speech_results_type_function = {
	.name = "SPEECH_RESULTS_TYPE",
	.read = NULL,
	.write = speech_results_type_write,
};

/*! \brief SPEECH() Dialplan Function */
static int speech_read(struct tris_channel *chan, const char *cmd, char *data,
			char *buf, size_t len)
{
	int results = 0;
	struct tris_speech_result *result = NULL;
	struct tris_speech *speech = find_speech(chan);
	char tmp[128] = "";

	/* Now go for the various options */
	if (!strcasecmp(data, "status")) {
		if (speech != NULL)
			tris_copy_string(buf, "1", len);
		else
			tris_copy_string(buf, "0", len);
		return 0;
	}

	/* Make sure we have a speech structure for everything else */
	if (speech == NULL) {
		return -1;
	}

	/* Check to see if they are checking for silence */
	if (!strcasecmp(data, "spoke")) {
		if (tris_test_flag(speech, TRIS_SPEECH_SPOKE))
			tris_copy_string(buf, "1", len);
		else
			tris_copy_string(buf, "0", len);
	} else if (!strcasecmp(data, "results")) {
		/* Count number of results */
		for (result = speech->results; result; result = TRIS_LIST_NEXT(result, list))
			results++;
		snprintf(tmp, sizeof(tmp), "%d", results);
		tris_copy_string(buf, tmp, len);
	} else {
		buf[0] = '\0';
	}

	return 0;
}

static struct tris_custom_function speech_function = {
	.name = "SPEECH",
	.read = speech_read,
	.write = NULL,
};



/*! \brief SpeechCreate() Dialplan Application */
static int speech_create(struct tris_channel *chan, void *data)
{
	struct tris_speech *speech = NULL;
	struct tris_datastore *datastore = NULL;

	/* Request a speech object */
	speech = tris_speech_new(data, chan->nativeformats);
	if (speech == NULL) {
		/* Not available */
		pbx_builtin_setvar_helper(chan, "ERROR", "1");
		return 0;
	}

	datastore = tris_datastore_alloc(&speech_datastore, NULL);
	if (datastore == NULL) {
		tris_speech_destroy(speech);
		pbx_builtin_setvar_helper(chan, "ERROR", "1");
		return 0;
	}
	pbx_builtin_setvar_helper(chan, "ERROR", NULL);
	datastore->data = speech;
	tris_channel_datastore_add(chan, datastore);

	return 0;
}

/*! \brief SpeechLoadGrammar(Grammar Name,Path) Dialplan Application */
static int speech_load(struct tris_channel *chan, void *vdata)
{
	int res = 0;
	struct tris_speech *speech = find_speech(chan);
	char *data;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(grammar);
		TRIS_APP_ARG(path);
	);

	data = tris_strdupa(vdata);
	TRIS_STANDARD_APP_ARGS(args, data);

	if (speech == NULL)
		return -1;

	if (args.argc != 2)
		return -1;

	/* Load the grammar locally on the object */
	res = tris_speech_grammar_load(speech, args.grammar, args.path);

	return res;
}

/*! \brief SpeechUnloadGrammar(Grammar Name) Dialplan Application */
static int speech_unload(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_speech *speech = find_speech(chan);

	if (speech == NULL)
		return -1;

	/* Unload the grammar */
	res = tris_speech_grammar_unload(speech, data);

	return res;
}

/*! \brief SpeechDeactivateGrammar(Grammar Name) Dialplan Application */
static int speech_deactivate(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_speech *speech = find_speech(chan);

	if (speech == NULL)
		return -1;

	/* Deactivate the grammar on the speech object */
	res = tris_speech_grammar_deactivate(speech, data);

	return res;
}

/*! \brief SpeechActivateGrammar(Grammar Name) Dialplan Application */
static int speech_activate(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_speech *speech = find_speech(chan);

	if (speech == NULL)
		return -1;

	/* Activate the grammar on the speech object */
	res = tris_speech_grammar_activate(speech, data);

	return res;
}

/*! \brief SpeechStart() Dialplan Application */
static int speech_start(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_speech *speech = find_speech(chan);

	if (speech == NULL)
		return -1;

	tris_speech_start(speech);

	return res;
}

/*! \brief SpeechProcessingSound(Sound File) Dialplan Application */
static int speech_processing_sound(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_speech *speech = find_speech(chan);

	if (speech == NULL)
		return -1;

	if (speech->processing_sound != NULL) {
		tris_free(speech->processing_sound);
		speech->processing_sound = NULL;
	}

	speech->processing_sound = tris_strdup(data);

	return res;
}

/*! \brief Helper function used by speech_background to playback a soundfile */
static int speech_streamfile(struct tris_channel *chan, const char *filename, const char *preflang)
{
	struct tris_filestream *fs = NULL;

	if (!(fs = tris_openstream(chan, filename, preflang)))
		return -1;
	
	if (tris_applystream(chan, fs))
		return -1;
	
	tris_playstream(fs);

	return 0;
}

enum {
	SB_OPT_NOANSWER = (1 << 0),
};

TRIS_APP_OPTIONS(speech_background_options, BEGIN_OPTIONS
	TRIS_APP_OPTION('n', SB_OPT_NOANSWER),
END_OPTIONS );

/*! \brief SpeechBackground(Sound File,Timeout) Dialplan Application */
static int speech_background(struct tris_channel *chan, void *data)
{
	unsigned int timeout = 0;
	int res = 0, done = 0, started = 0, quieted = 0, max_dtmf_len = 0;
	struct tris_speech *speech = find_speech(chan);
	struct tris_frame *f = NULL;
	int oldreadformat = TRIS_FORMAT_SLINEAR;
	char dtmf[TRIS_MAX_EXTENSION] = "";
	struct timeval start = { 0, 0 }, current;
	struct tris_datastore *datastore = NULL;
	char *parse, *filename_tmp = NULL, *filename = NULL, tmp[2] = "", dtmf_terminator = '#';
	const char *tmp2 = NULL;
	struct tris_flags options = { 0 };
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(soundfile);
		TRIS_APP_ARG(timeout);
		TRIS_APP_ARG(options);
	);

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (speech == NULL)
		return -1;

	if (!tris_strlen_zero(args.options)) {
		char *options_buf = tris_strdupa(args.options);
		tris_app_parse_options(speech_background_options, &options, NULL, options_buf);
	}

	/* If channel is not already answered, then answer it */
	if (chan->_state != TRIS_STATE_UP && !tris_test_flag(&options, SB_OPT_NOANSWER)
		&& tris_answer(chan)) {
			return -1;
	}

	/* Record old read format */
	oldreadformat = chan->readformat;

	/* Change read format to be signed linear */
	if (tris_set_read_format(chan, speech->format))
		return -1;

	if (!tris_strlen_zero(args.soundfile)) {
		/* Yay sound file */
		filename_tmp = tris_strdupa(args.soundfile);
		if (!tris_strlen_zero(args.timeout)) {
			if ((timeout = atof(args.timeout) * 1000.0) == 0)
				timeout = -1;
		} else
			timeout = 0;
	}

	/* See if the maximum DTMF length variable is set... we use a variable in case they want to carry it through their entire dialplan */
	tris_channel_lock(chan);
	if ((tmp2 = pbx_builtin_getvar_helper(chan, "SPEECH_DTMF_MAXLEN")) && !tris_strlen_zero(tmp2)) {
		max_dtmf_len = atoi(tmp2);
	}
	
	/* See if a terminator is specified */
	if ((tmp2 = pbx_builtin_getvar_helper(chan, "SPEECH_DTMF_TERMINATOR"))) {
		if (tris_strlen_zero(tmp2))
			dtmf_terminator = '\0';
		else
			dtmf_terminator = tmp2[0];
	}
	tris_channel_unlock(chan);

	/* Before we go into waiting for stuff... make sure the structure is ready, if not - start it again */
	if (speech->state == TRIS_SPEECH_STATE_NOT_READY || speech->state == TRIS_SPEECH_STATE_DONE) {
		tris_speech_change_state(speech, TRIS_SPEECH_STATE_NOT_READY);
		tris_speech_start(speech);
	}

	/* Ensure no streams are currently running */
	tris_stopstream(chan);

	/* Okay it's streaming so go into a loop grabbing frames! */
	while (done == 0) {
		/* If the filename is null and stream is not running, start up a new sound file */
		if (!quieted && (chan->streamid == -1 && chan->timingfunc == NULL) && (filename = strsep(&filename_tmp, "&"))) {
			/* Discard old stream information */
			tris_stopstream(chan);
			/* Start new stream */
			speech_streamfile(chan, filename, chan->language);
		}

		/* Run scheduled stuff */
		tris_sched_runq(chan->sched);

		/* Yay scheduling */
		res = tris_sched_wait(chan->sched);
		if (res < 0)
			res = 1000;

		/* If there is a frame waiting, get it - if not - oh well */
		if (tris_waitfor(chan, res) > 0) {
			f = tris_read(chan);
			if (f == NULL) {
				/* The channel has hung up most likely */
				done = 3;
				break;
			}
		}

		/* Do timeout check (shared between audio/dtmf) */
		if ((!quieted || strlen(dtmf)) && started == 1) {
			current = tris_tvnow();
			if ((tris_tvdiff_ms(current, start)) >= timeout) {
				done = 1;
				if (f)
					tris_frfree(f);
				break;
			}
		}

		/* Do checks on speech structure to see if it's changed */
		tris_mutex_lock(&speech->lock);
		if (tris_test_flag(speech, TRIS_SPEECH_QUIET)) {
			if (chan->stream)
				tris_stopstream(chan);
			tris_clear_flag(speech, TRIS_SPEECH_QUIET);
			quieted = 1;
		}
		/* Check state so we can see what to do */
		switch (speech->state) {
		case TRIS_SPEECH_STATE_READY:
			/* If audio playback has stopped do a check for timeout purposes */
			if (chan->streamid == -1 && chan->timingfunc == NULL)
				tris_stopstream(chan);
			if (!quieted && chan->stream == NULL && timeout && started == 0 && !filename_tmp) {
				if (timeout == -1) {
					done = 1;
					if (f)
						tris_frfree(f);
					break;
				}
				start = tris_tvnow();
				started = 1;
			}
			/* Write audio frame out to speech engine if no DTMF has been received */
			if (!strlen(dtmf) && f != NULL && f->frametype == TRIS_FRAME_VOICE) {
				tris_speech_write(speech, f->data.ptr, f->datalen);
			}
			break;
		case TRIS_SPEECH_STATE_WAIT:
			/* Cue up waiting sound if not already playing */
			if (!strlen(dtmf)) {
				if (chan->stream == NULL) {
					if (speech->processing_sound != NULL) {
						if (strlen(speech->processing_sound) > 0 && strcasecmp(speech->processing_sound, "none")) {
							speech_streamfile(chan, speech->processing_sound, chan->language);
						}
					}
				} else if (chan->streamid == -1 && chan->timingfunc == NULL) {
					tris_stopstream(chan);
					if (speech->processing_sound != NULL) {
						if (strlen(speech->processing_sound) > 0 && strcasecmp(speech->processing_sound, "none")) {
							speech_streamfile(chan, speech->processing_sound, chan->language);
						}
					}
				}
			}
			break;
		case TRIS_SPEECH_STATE_DONE:
			/* Now that we are done... let's switch back to not ready state */
			tris_speech_change_state(speech, TRIS_SPEECH_STATE_NOT_READY);
			if (!strlen(dtmf)) {
				/* Copy to speech structure the results, if available */
				speech->results = tris_speech_results_get(speech);
				/* Break out of our background too */
				done = 1;
				/* Stop audio playback */
				if (chan->stream != NULL) {
					tris_stopstream(chan);
				}
			}
			break;
		default:
			break;
		}
		tris_mutex_unlock(&speech->lock);

		/* Deal with other frame types */
		if (f != NULL) {
			/* Free the frame we received */
			switch (f->frametype) {
			case TRIS_FRAME_DTMF:
				if (dtmf_terminator != '\0' && f->subclass == dtmf_terminator) {
					done = 1;
				} else {
					if (chan->stream != NULL) {
						tris_stopstream(chan);
					}
					if (!started) {
						/* Change timeout to be 5 seconds for DTMF input */
						timeout = (chan->pbx && chan->pbx->dtimeoutms) ? chan->pbx->dtimeoutms : 5000;
						started = 1;
					}
					start = tris_tvnow();
					snprintf(tmp, sizeof(tmp), "%c", f->subclass);
					strncat(dtmf, tmp, sizeof(dtmf) - strlen(dtmf) - 1);
					/* If the maximum length of the DTMF has been reached, stop now */
					if (max_dtmf_len && strlen(dtmf) == max_dtmf_len)
						done = 1;
				}
				break;
			case TRIS_FRAME_CONTROL:
				switch (f->subclass) {
				case TRIS_CONTROL_HANGUP:
					/* Since they hung up we should destroy the speech structure */
					done = 3;
				default:
					break;
				}
			default:
				break;
			}
			tris_frfree(f);
			f = NULL;
		}
	}

	if (!tris_strlen_zero(dtmf)) {
		/* We sort of make a results entry */
		speech->results = tris_calloc(1, sizeof(*speech->results));
		if (speech->results != NULL) {
			tris_speech_dtmf(speech, dtmf);
			speech->results->score = 1000;
			speech->results->text = tris_strdup(dtmf);
			speech->results->grammar = tris_strdup("dtmf");
		}
		tris_speech_change_state(speech, TRIS_SPEECH_STATE_NOT_READY);
	}

	/* See if it was because they hung up */
	if (done == 3) {
		/* Destroy speech structure */
		tris_speech_destroy(speech);
		datastore = tris_channel_datastore_find(chan, &speech_datastore, NULL);
		if (datastore != NULL)
			tris_channel_datastore_remove(chan, datastore);
	} else {
		/* Channel is okay so restore read format */
		tris_set_read_format(chan, oldreadformat);
	}

	return 0;
}


/*! \brief SpeechDestroy() Dialplan Application */
static int speech_destroy(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_speech *speech = find_speech(chan);
	struct tris_datastore *datastore = NULL;

	if (speech == NULL)
		return -1;

	/* Destroy speech structure */
	tris_speech_destroy(speech);

	datastore = tris_channel_datastore_find(chan, &speech_datastore, NULL);
	if (datastore != NULL) {
		tris_channel_datastore_remove(chan, datastore);
	}

	return res;
}

static int unload_module(void)
{
	int res = 0;

	res = tris_unregister_application("SpeechCreate");
	res |= tris_unregister_application("SpeechLoadGrammar");
	res |= tris_unregister_application("SpeechUnloadGrammar");
	res |= tris_unregister_application("SpeechActivateGrammar");
	res |= tris_unregister_application("SpeechDeactivateGrammar");
	res |= tris_unregister_application("SpeechStart");
	res |= tris_unregister_application("SpeechBackground");
	res |= tris_unregister_application("SpeechDestroy");
	res |= tris_unregister_application("SpeechProcessingSound");
	res |= tris_custom_function_unregister(&speech_function);
	res |= tris_custom_function_unregister(&speech_score_function);
	res |= tris_custom_function_unregister(&speech_text_function);
	res |= tris_custom_function_unregister(&speech_grammar_function);
	res |= tris_custom_function_unregister(&speech_engine_function);
	res |= tris_custom_function_unregister(&speech_results_type_function);

	return res;	
}

static int load_module(void)
{
	int res = 0;

	res = tris_register_application_xml("SpeechCreate", speech_create);
	res |= tris_register_application_xml("SpeechLoadGrammar", speech_load);
	res |= tris_register_application_xml("SpeechUnloadGrammar", speech_unload);
	res |= tris_register_application_xml("SpeechActivateGrammar", speech_activate);
	res |= tris_register_application_xml("SpeechDeactivateGrammar", speech_deactivate);
	res |= tris_register_application_xml("SpeechStart", speech_start);
	res |= tris_register_application_xml("SpeechBackground", speech_background);
	res |= tris_register_application_xml("SpeechDestroy", speech_destroy);
	res |= tris_register_application_xml("SpeechProcessingSound", speech_processing_sound);
	res |= tris_custom_function_register(&speech_function);
	res |= tris_custom_function_register(&speech_score_function);
	res |= tris_custom_function_register(&speech_text_function);
	res |= tris_custom_function_register(&speech_grammar_function);
	res |= tris_custom_function_register(&speech_engine_function);
	res |= tris_custom_function_register(&speech_results_type_function);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Dialplan Speech Applications");
