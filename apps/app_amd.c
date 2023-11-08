/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2003 - 2006, Aheeva Technology.
 *
 * Claude Klimos (claude.klimos@aheeva.com)
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
 *
 * A license has been granted to Digium (via disclaimer) for the use of
 * this code.
 */

/*! \file
 *
 * \brief Answering machine detection
 *
 * \author Claude Klimos (claude.klimos@aheeva.com)
 */


#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 232359 $")

#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/dsp.h"
#include "trismedia/pbx.h"
#include "trismedia/config.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="AMD" language="en_US">
		<synopsis>
			Attempt to detect answering machines.
		</synopsis>
		<syntax>
			<parameter name="initialSilence" required="false">
				<para>Is maximum initial silence duration before greeting.</para>
				<para>If this is exceeded set as MACHINE</para>
			</parameter>
			<parameter name="greeting" required="false">
				<para>is the maximum length of a greeting.</para>
				<para>If this is exceeded set as MACHINE</para>
			</parameter>
			<parameter name="afterGreetingSilence" required="false">
				<para>Is the silence after detecting a greeting.</para>
				<para>If this is exceeded set as HUMAN</para>
			</parameter>
			<parameter name="totalAnalysis Time" required="false">
				<para>Is the maximum time allowed for the algorithm</para>
				<para>to decide HUMAN or MACHINE</para>
			</parameter>
			<parameter name="miniumWordLength" required="false">
				<para>Is the minimum duration of Voice considered to be a word</para>
			</parameter>
			<parameter name="betweenWordSilence" required="false">
				<para>Is the minimum duration of silence after a word to
				consider the audio that follows to be a new word</para>
			</parameter>
			<parameter name="maximumNumberOfWords" required="false">
				<para>Is the maximum number of words in a greeting</para>
				<para>If this is exceeded set as MACHINE</para>
			</parameter>
			<parameter name="silenceThreshold" required="false">
				<para>How long do we consider silence</para>
			</parameter>
			<parameter name="maximumWordLength" required="false">
				<para>Is the maximum duration of a word to accept.</para>
				<para>If exceeded set as MACHINE</para>
			</parameter>
		</syntax>
		<description>
			<para>This application attempts to detect answering machines at the beginning
			of outbound calls. Simply call this application after the call
			has been answered (outbound only, of course).</para>
			<para>When loaded, AMD reads amd.conf and uses the parameters specified as
			default values. Those default values get overwritten when the calling AMD
			with parameters.</para>
			<para>This application sets the following channel variables:</para>
			<variablelist>
				<variable name="AMDSTATUS">
					<para>This is the status of the answering machine detection</para>
					<value name="MACHINE" />
					<value name="HUMAN" />
					<value name="NOTSURE" />
					<value name="HANGUP" />
				</variable>
				<variable name="AMDCAUSE">
					<para>Indicates the cause that led to the conclusion</para>
					<value name="TOOLONG">
						Total Time.
					</value>
					<value name="INITIALSILENCE">
						Silence Duration - Initial Silence.
					</value>
					<value name="HUMAN">
						Silence Duration - afterGreetingSilence.
					</value>
					<value name="LONGGREETING">
						Voice Duration - Greeting.
					</value>
					<value name="MAXWORDLENGTH">
						Word Count - maximum number of words.
					</value>	
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">WaitForSilence</ref>
			<ref type="application">WaitForNoise</ref>
		</see-also>
	</application>

 ***/

static char *app = "AMD";

#define STATE_IN_WORD       1
#define STATE_IN_SILENCE    2

/* Some default values for the algorithm parameters. These defaults will be overwritten from amd.conf */
static int dfltInitialSilence       = 2500;
static int dfltGreeting             = 1500;
static int dfltAfterGreetingSilence = 800;
static int dfltTotalAnalysisTime    = 5000;
static int dfltMinimumWordLength    = 100;
static int dfltBetweenWordsSilence  = 50;
static int dfltMaximumNumberOfWords = 3;
static int dfltSilenceThreshold     = 256;
static int dfltMaximumWordLength    = 5000; /* Setting this to a large default so it is not used unless specify it in the configs or command line */

/* Set to the lowest ms value provided in amd.conf or application parameters */
static int dfltMaxWaitTimeForFrame  = 50;

static void isAnsweringMachine(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct tris_frame *f = NULL;
	struct tris_dsp *silenceDetector = NULL;
	int dspsilence = 0, readFormat, framelength = 0;
	int inInitialSilence = 1;
	int inGreeting = 0;
	int voiceDuration = 0;
	int silenceDuration = 0;
	int iTotalTime = 0;
	int iWordsCount = 0;
	int currentState = STATE_IN_WORD;
	int previousState = STATE_IN_SILENCE;
	int consecutiveVoiceDuration = 0;
	char amdCause[256] = "", amdStatus[256] = "";
	char *parse = tris_strdupa(data);

	/* Lets set the initial values of the variables that will control the algorithm.
	   The initial values are the default ones. If they are passed as arguments
	   when invoking the application, then the default values will be overwritten
	   by the ones passed as parameters. */
	int initialSilence       = dfltInitialSilence;
	int greeting             = dfltGreeting;
	int afterGreetingSilence = dfltAfterGreetingSilence;
	int totalAnalysisTime    = dfltTotalAnalysisTime;
	int minimumWordLength    = dfltMinimumWordLength;
	int betweenWordsSilence  = dfltBetweenWordsSilence;
	int maximumNumberOfWords = dfltMaximumNumberOfWords;
	int silenceThreshold     = dfltSilenceThreshold;
	int maximumWordLength    = dfltMaximumWordLength;
	int maxWaitTimeForFrame  = dfltMaxWaitTimeForFrame;

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(argInitialSilence);
		TRIS_APP_ARG(argGreeting);
		TRIS_APP_ARG(argAfterGreetingSilence);
		TRIS_APP_ARG(argTotalAnalysisTime);
		TRIS_APP_ARG(argMinimumWordLength);
		TRIS_APP_ARG(argBetweenWordsSilence);
		TRIS_APP_ARG(argMaximumNumberOfWords);
		TRIS_APP_ARG(argSilenceThreshold);
		TRIS_APP_ARG(argMaximumWordLength);
	);

	tris_verb(3, "AMD: %s %s %s (Fmt: %d)\n", chan->name ,chan->cid.cid_ani, chan->cid.cid_rdnis, chan->readformat);

	/* Lets parse the arguments. */
	if (!tris_strlen_zero(parse)) {
		/* Some arguments have been passed. Lets parse them and overwrite the defaults. */
		TRIS_STANDARD_APP_ARGS(args, parse);
		if (!tris_strlen_zero(args.argInitialSilence))
			initialSilence = atoi(args.argInitialSilence);
		if (!tris_strlen_zero(args.argGreeting))
			greeting = atoi(args.argGreeting);
		if (!tris_strlen_zero(args.argAfterGreetingSilence))
			afterGreetingSilence = atoi(args.argAfterGreetingSilence);
		if (!tris_strlen_zero(args.argTotalAnalysisTime))
			totalAnalysisTime = atoi(args.argTotalAnalysisTime);
		if (!tris_strlen_zero(args.argMinimumWordLength))
			minimumWordLength = atoi(args.argMinimumWordLength);
		if (!tris_strlen_zero(args.argBetweenWordsSilence))
			betweenWordsSilence = atoi(args.argBetweenWordsSilence);
		if (!tris_strlen_zero(args.argMaximumNumberOfWords))
			maximumNumberOfWords = atoi(args.argMaximumNumberOfWords);
		if (!tris_strlen_zero(args.argSilenceThreshold))
			silenceThreshold = atoi(args.argSilenceThreshold);
		if (!tris_strlen_zero(args.argMaximumWordLength))
			maximumWordLength = atoi(args.argMaximumWordLength);
	} else {
		tris_debug(1, "AMD using the default parameters.\n");
	}

	/* Find lowest ms value, that will be max wait time for a frame */
	if (maxWaitTimeForFrame > initialSilence)
		maxWaitTimeForFrame = initialSilence;
	if (maxWaitTimeForFrame > greeting)
		maxWaitTimeForFrame = greeting;
	if (maxWaitTimeForFrame > afterGreetingSilence)
		maxWaitTimeForFrame = afterGreetingSilence;
	if (maxWaitTimeForFrame > totalAnalysisTime)
		maxWaitTimeForFrame = totalAnalysisTime;
	if (maxWaitTimeForFrame > minimumWordLength)
		maxWaitTimeForFrame = minimumWordLength;
	if (maxWaitTimeForFrame > betweenWordsSilence)
		maxWaitTimeForFrame = betweenWordsSilence;

	/* Now we're ready to roll! */
	tris_verb(3, "AMD: initialSilence [%d] greeting [%d] afterGreetingSilence [%d] "
		"totalAnalysisTime [%d] minimumWordLength [%d] betweenWordsSilence [%d] maximumNumberOfWords [%d] silenceThreshold [%d] maximumWordLength [%d] \n",
				initialSilence, greeting, afterGreetingSilence, totalAnalysisTime,
				minimumWordLength, betweenWordsSilence, maximumNumberOfWords, silenceThreshold, maximumWordLength);

	/* Set read format to signed linear so we get signed linear frames in */
	readFormat = chan->readformat;
	if (tris_set_read_format(chan, TRIS_FORMAT_SLINEAR) < 0 ) {
		tris_log(LOG_WARNING, "AMD: Channel [%s]. Unable to set to linear mode, giving up\n", chan->name );
		pbx_builtin_setvar_helper(chan , "AMDSTATUS", "");
		pbx_builtin_setvar_helper(chan , "AMDCAUSE", "");
		return;
	}

	/* Create a new DSP that will detect the silence */
	if (!(silenceDetector = tris_dsp_new())) {
		tris_log(LOG_WARNING, "AMD: Channel [%s]. Unable to create silence detector :(\n", chan->name );
		pbx_builtin_setvar_helper(chan , "AMDSTATUS", "");
		pbx_builtin_setvar_helper(chan , "AMDCAUSE", "");
		return;
	}

	/* Set silence threshold to specified value */
	tris_dsp_set_threshold(silenceDetector, silenceThreshold);

	/* Now we go into a loop waiting for frames from the channel */
	while ((res = tris_waitfor(chan, 2 * maxWaitTimeForFrame)) > -1) {

		/* If we fail to read in a frame, that means they hung up */
		if (!(f = tris_read(chan))) {
			tris_verb(3, "AMD: Channel [%s]. HANGUP\n", chan->name);
			tris_debug(1, "Got hangup\n");
			strcpy(amdStatus, "HANGUP");
			res = 1;
			break;
		}

		if (f->frametype == TRIS_FRAME_VOICE || f->frametype == TRIS_FRAME_NULL || f->frametype == TRIS_FRAME_CNG) {
			/* If the total time exceeds the analysis time then give up as we are not too sure */
			if (f->frametype == TRIS_FRAME_VOICE)
				framelength = (tris_codec_get_samples(f) / DEFAULT_SAMPLES_PER_MS);
			else
				framelength += 2 * maxWaitTimeForFrame;

			iTotalTime += framelength;
			if (iTotalTime >= totalAnalysisTime) {
				tris_verb(3, "AMD: Channel [%s]. Too long...\n", chan->name );
				tris_frfree(f);
				strcpy(amdStatus , "NOTSURE");
				sprintf(amdCause , "TOOLONG-%d", iTotalTime);
				break;
			}

			/* Feed the frame of audio into the silence detector and see if we get a result */
			if (f->frametype != TRIS_FRAME_VOICE)
				dspsilence += 2 * maxWaitTimeForFrame;
			else {
				dspsilence = 0;
				tris_dsp_silence(silenceDetector, f, &dspsilence);
			}

			if (dspsilence > 0) {
				silenceDuration = dspsilence;
				
				if (silenceDuration >= betweenWordsSilence) {
					if (currentState != STATE_IN_SILENCE ) {
						previousState = currentState;
						tris_verb(3, "AMD: Channel [%s]. Changed state to STATE_IN_SILENCE\n", chan->name);
					}
					/* Find words less than word duration */
					if (consecutiveVoiceDuration < minimumWordLength && consecutiveVoiceDuration > 0){
						tris_verb(3, "AMD: Channel [%s]. Short Word Duration: %d\n", chan->name, consecutiveVoiceDuration);
					}
					currentState  = STATE_IN_SILENCE;
					consecutiveVoiceDuration = 0;
				}

				if (inInitialSilence == 1  && silenceDuration >= initialSilence) {
					tris_verb(3, "AMD: Channel [%s]. ANSWERING MACHINE: silenceDuration:%d initialSilence:%d\n",
						chan->name, silenceDuration, initialSilence);
					tris_frfree(f);
					strcpy(amdStatus , "MACHINE");
					sprintf(amdCause , "INITIALSILENCE-%d-%d", silenceDuration, initialSilence);
					res = 1;
					break;
				}
				
				if (silenceDuration >= afterGreetingSilence  &&  inGreeting == 1) {
					tris_verb(3, "AMD: Channel [%s]. HUMAN: silenceDuration:%d afterGreetingSilence:%d\n",
						chan->name, silenceDuration, afterGreetingSilence);
					tris_frfree(f);
					strcpy(amdStatus , "HUMAN");
					sprintf(amdCause , "HUMAN-%d-%d", silenceDuration, afterGreetingSilence);
					res = 1;
					break;
				}
				
			} else {
				consecutiveVoiceDuration += framelength;
				voiceDuration += framelength;

				/* If I have enough consecutive voice to say that I am in a Word, I can only increment the
				   number of words if my previous state was Silence, which means that I moved into a word. */
				if (consecutiveVoiceDuration >= minimumWordLength && currentState == STATE_IN_SILENCE) {
					iWordsCount++;
					tris_verb(3, "AMD: Channel [%s]. Word detected. iWordsCount:%d\n", chan->name, iWordsCount);
					previousState = currentState;
					currentState = STATE_IN_WORD;
				}
				if (consecutiveVoiceDuration >= maximumWordLength){
					tris_verb(3, "AMD: Channel [%s]. Maximum Word Length detected. [%d]\n", chan->name, consecutiveVoiceDuration);
					tris_frfree(f);
					strcpy(amdStatus , "MACHINE");
					sprintf(amdCause , "MAXWORDLENGTH-%d", consecutiveVoiceDuration);
					break;
				}
				if (iWordsCount >= maximumNumberOfWords) {
					tris_verb(3, "AMD: Channel [%s]. ANSWERING MACHINE: iWordsCount:%d\n", chan->name, iWordsCount);
					tris_frfree(f);
					strcpy(amdStatus , "MACHINE");
					sprintf(amdCause , "MAXWORDS-%d-%d", iWordsCount, maximumNumberOfWords);
					res = 1;
					break;
				}

				if (inGreeting == 1 && voiceDuration >= greeting) {
					tris_verb(3, "AMD: Channel [%s]. ANSWERING MACHINE: voiceDuration:%d greeting:%d\n", chan->name, voiceDuration, greeting);
					tris_frfree(f);
					strcpy(amdStatus , "MACHINE");
					sprintf(amdCause , "LONGGREETING-%d-%d", voiceDuration, greeting);
					res = 1;
					break;
				}

				if (voiceDuration >= minimumWordLength ) {
					if (silenceDuration > 0)
						tris_verb(3, "AMD: Channel [%s]. Detected Talk, previous silence duration: %d\n", chan->name, silenceDuration);
					silenceDuration = 0;
				}
				if (consecutiveVoiceDuration >= minimumWordLength && inGreeting == 0) {
					/* Only go in here once to change the greeting flag when we detect the 1st word */
					if (silenceDuration > 0)
						tris_verb(3, "AMD: Channel [%s]. Before Greeting Time:  silenceDuration: %d voiceDuration: %d\n", chan->name, silenceDuration, voiceDuration);
					inInitialSilence = 0;
					inGreeting = 1;
				}
				
			}
		}
		tris_frfree(f);
	}
	
	if (!res) {
		/* It took too long to get a frame back. Giving up. */
		tris_verb(3, "AMD: Channel [%s]. Too long...\n", chan->name);
		strcpy(amdStatus , "NOTSURE");
		sprintf(amdCause , "TOOLONG-%d", iTotalTime);
	}

	/* Set the status and cause on the channel */
	pbx_builtin_setvar_helper(chan , "AMDSTATUS" , amdStatus);
	pbx_builtin_setvar_helper(chan , "AMDCAUSE" , amdCause);

	/* Restore channel read format */
	if (readFormat && tris_set_read_format(chan, readFormat))
		tris_log(LOG_WARNING, "AMD: Unable to restore read format on '%s'\n", chan->name);

	/* Free the DSP used to detect silence */
	tris_dsp_free(silenceDetector);

	return;
}


static int amd_exec(struct tris_channel *chan, void *data)
{
	isAnsweringMachine(chan, data);

	return 0;
}

static int load_config(int reload)
{
	struct tris_config *cfg = NULL;
	char *cat = NULL;
	struct tris_variable *var = NULL;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	dfltSilenceThreshold = tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE);

	if (!(cfg = tris_config_load("amd.conf", config_flags))) {
		tris_log(LOG_ERROR, "Configuration file amd.conf missing.\n");
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file amd.conf is in an invalid format.  Aborting.\n");
		return -1;
	}

	cat = tris_category_browse(cfg, NULL);

	while (cat) {
		if (!strcasecmp(cat, "general") ) {
			var = tris_variable_browse(cfg, cat);
			while (var) {
				if (!strcasecmp(var->name, "initial_silence")) {
					dfltInitialSilence = atoi(var->value);
				} else if (!strcasecmp(var->name, "greeting")) {
					dfltGreeting = atoi(var->value);
				} else if (!strcasecmp(var->name, "after_greeting_silence")) {
					dfltAfterGreetingSilence = atoi(var->value);
				} else if (!strcasecmp(var->name, "silence_threshold")) {
					dfltSilenceThreshold = atoi(var->value);
				} else if (!strcasecmp(var->name, "total_analysis_time")) {
					dfltTotalAnalysisTime = atoi(var->value);
				} else if (!strcasecmp(var->name, "min_word_length")) {
					dfltMinimumWordLength = atoi(var->value);
				} else if (!strcasecmp(var->name, "between_words_silence")) {
					dfltBetweenWordsSilence = atoi(var->value);
				} else if (!strcasecmp(var->name, "maximum_number_of_words")) {
					dfltMaximumNumberOfWords = atoi(var->value);
				} else if (!strcasecmp(var->name, "maximum_word_length")) {
					dfltMaximumWordLength = atoi(var->value);

				} else {
					tris_log(LOG_WARNING, "%s: Cat:%s. Unknown keyword %s at line %d of amd.conf\n",
						app, cat, var->name, var->lineno);
				}
				var = var->next;
			}
		}
		cat = tris_category_browse(cfg, cat);
	}

	tris_config_destroy(cfg);

	tris_verb(3, "AMD defaults: initialSilence [%d] greeting [%d] afterGreetingSilence [%d] "
		"totalAnalysisTime [%d] minimumWordLength [%d] betweenWordsSilence [%d] maximumNumberOfWords [%d] silenceThreshold [%d] maximumWordLength [%d]\n",
		dfltInitialSilence, dfltGreeting, dfltAfterGreetingSilence, dfltTotalAnalysisTime,
		dfltMinimumWordLength, dfltBetweenWordsSilence, dfltMaximumNumberOfWords, dfltSilenceThreshold, dfltMaximumWordLength);

	return 0;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	if (load_config(0))
		return TRIS_MODULE_LOAD_DECLINE;
	if (tris_register_application_xml(app, amd_exec))
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	if (load_config(1))
		return TRIS_MODULE_LOAD_DECLINE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Answering Machine Detection Application",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
);
