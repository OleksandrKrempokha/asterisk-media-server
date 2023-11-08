/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * WaitForSilence Application by David C. Troy <dave@popvox.com>
 * Version 1.11 2006-06-29
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
 * \brief Wait for Silence
 *   - Waits for up to 'x' milliseconds of silence, 'y' times \n
 *   - WaitForSilence(500,2) will wait for 1/2 second of silence, twice \n
 *   - WaitForSilence(1000,1) will wait for 1 second of silence, once \n
 *   - WaitForSilence(300,3,10) will wait for 300ms of silence, 3 times, and return after 10sec \n
 *
 * \author David C. Troy <dave@popvox.com>
 *
 * \brief Wait For Noise
 * The same as Wait For Silence but listenes noise on the chennel that is above \n
 * the pre-configured silence threshold from dsp.conf
 *
 * \author Philipp Skadorov <skadorov@yahoo.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 239713 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/dsp.h"
#include "trismedia/module.h"

/*** DOCUMENTATION
	<application name="WaitForSilence" language="en_US">
		<synopsis>
			Waits for a specified amount of silence.
		</synopsis>
		<syntax>
			<parameter name="silencerequired" required="true" />
			<parameter name="iterations">
				<para>If not specified, defaults to <literal>1</literal>.</para>
			</parameter>
			<parameter name="timeout">
				<para>Is specified only to avoid an infinite loop in cases where silence is never achieved.</para>
			</parameter>
		</syntax>
		<description>
			<para>Waits for up to <replaceable>silencerequired</replaceable> milliseconds of silence,
			<replaceable>iterations</replaceable> times. An optional <replaceable>timeout</replaceable>
			specified the number of seconds to return after, even if we do not receive the specified amount of silence.
			Use <replaceable>timeout</replaceable> with caution, as it may defeat the purpose of this application, which
			is to wait indefinitely until silence is detected on the line. This is particularly useful for reverse-911-type
			call broadcast applications where you need to wait for an answering machine to complete its spiel before
			playing a message.</para>
			<para>Typically you will want to include two or more calls to WaitForSilence when dealing with an answering
			machine; first waiting for the spiel to finish, then waiting for the beep, etc.</para>
			<para>Examples:</para>
			<para>WaitForSilence(500,2) will wait for 1/2 second of silence, twice</para>
			<para>WaitForSilence(1000) will wait for 1 second of silence, once</para>
			<para>WaitForSilence(300,3,10) will wait for 300ms silence, 3 times, and returns after 10 sec, even if silence
			is not detected</para>
			<para>Sets the channel variable <variable>WAITSTATUS</variable> to one of these values:</para>
			<variablelist>
				<variable name="WAITSTATUS">
					<value name="SILENCE">
						if exited with silence detected.
					</value>
					<value name="TIMEOUT">
						if exited without silence detected after timeout.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">WaitForNoise</ref>
		</see-also>
	</application>
	<application name="WaitForNoise" language="en_US">
		<synopsis>
			Waits for a specified amount of noise.
		</synopsis>
		<syntax>
			<parameter name="noiserequired" required="true" />
			<parameter name="iterations">
				<para>If not specified, defaults to <literal>1</literal>.</para>
			</parameter>
			<parameter name="timeout">
				<para>Is specified only to avoid an infinite loop in cases where silence is never achieved.</para>
			</parameter>
		</syntax>
		<description>
			<para>Waits for up to <replaceable>noiserequired</replaceable> milliseconds of noise,
			<replaceable>iterations</replaceable> times. An optional <replaceable>timeout</replaceable>
			specified the number of seconds to return after, even if we do not receive the specified amount of noise.
			Use <replaceable>timeout</replaceable> with caution, as it may defeat the purpose of this application, which
			is to wait indefinitely until noise is detected on the line.</para>
		</description>
		<see-also>
			<ref type="application">WaitForSilence</ref>
		</see-also>
	</application>
 ***/

static char *app_silence = "WaitForSilence";
static char *app_noise = "WaitForNoise";

static int do_waiting(struct tris_channel *chan, int timereqd, time_t waitstart, int timeout, int wait_for_silence) {
	struct tris_frame *f = NULL;
	int dsptime = 0;
	int rfmt = 0;
	int res = 0;
	struct tris_dsp *sildet;	 /* silence detector dsp */
 	time_t now;

	/*Either silence or noise calc depending on wait_for_silence flag*/
	int (*tris_dsp_func)(struct tris_dsp*, struct tris_frame*, int*) =
				wait_for_silence ? tris_dsp_silence : tris_dsp_noise;

	rfmt = chan->readformat; /* Set to linear mode */
	if ((res = tris_set_read_format(chan, TRIS_FORMAT_SLINEAR)) < 0) {
		tris_log(LOG_WARNING, "Unable to set channel to linear mode, giving up\n");
		return -1;
	}

	/* Create the silence detector */
	if (!(sildet = tris_dsp_new())) {
		tris_log(LOG_WARNING, "Unable to create silence detector :(\n");
		return -1;
	}
	tris_dsp_set_threshold(sildet, tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE));

	/* Await silence... */
	for (;;) {
		/* Start with no silence received */
		dsptime = 0;

		res = tris_waitfor(chan, timereqd);

		/* Must have gotten a hangup; let's exit */
		if (res < 0) {
			pbx_builtin_setvar_helper(chan, "WAITSTATUS", "HANGUP");
			break;
		}
		
		/* We waited and got no frame; sounds like digital silence or a muted digital channel */
		if (res == 0) {
			dsptime = timereqd;
		} else {
			/* Looks like we did get a frame, so let's check it out */
			if (!(f = tris_read(chan))) {
				pbx_builtin_setvar_helper(chan, "WAITSTATUS", "HANGUP");
				break;
			}
			if (f->frametype == TRIS_FRAME_VOICE) {
				tris_dsp_func(sildet, f, &dsptime);
			}
			tris_frfree(f);
		}

		tris_verb(6, "Got %dms %s < %dms required\n", dsptime, wait_for_silence ? "silence" : "noise", timereqd);

		if (dsptime >= timereqd) {
			tris_verb(3, "Exiting with %dms %s >= %dms required\n", dsptime, wait_for_silence ? "silence" : "noise", timereqd);
			/* Ended happily with silence */
			res = 1;
			pbx_builtin_setvar_helper(chan, "WAITSTATUS", wait_for_silence ? "SILENCE" : "NOISE");
			tris_debug(1, "WAITSTATUS was set to %s\n", wait_for_silence ? "SILENCE" : "NOISE");
			break;
		}

		if (timeout && (difftime(time(&now), waitstart) >= timeout)) {
			pbx_builtin_setvar_helper(chan, "WAITSTATUS", "TIMEOUT");
			tris_debug(1, "WAITSTATUS was set to TIMEOUT\n");
			res = 0;
			break;
		}
	}


	if (rfmt && tris_set_read_format(chan, rfmt)) {
		tris_log(LOG_WARNING, "Unable to restore format %s to channel '%s'\n", tris_getformatname(rfmt), chan->name);
	}
	tris_dsp_free(sildet);
	return res;
}

static int waitfor_exec(struct tris_channel *chan, void *data, int wait_for_silence)
{
	int res = 1;
	int timereqd = 1000;
	int timeout = 0;
	int iterations = 1, i;
	time_t waitstart;
	struct tris_silence_generator *silgen = NULL;

	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan); /* Answer the channel */
	}

	if (!data || ( (sscanf(data, "%30d,%30d,%30d", &timereqd, &iterations, &timeout) != 3) &&
		(sscanf(data, "%30d,%30d", &timereqd, &iterations) != 2) &&
		(sscanf(data, "%30d", &timereqd) != 1) ) ) {
		tris_log(LOG_WARNING, "Using default value of 1000ms, 1 iteration, no timeout\n");
	}

	tris_verb(3, "Waiting %d time(s) for %d ms silence with %d timeout\n", iterations, timereqd, timeout);

	if (tris_opt_transmit_silence) {
		silgen = tris_channel_start_silence_generator(chan);
	}
	time(&waitstart);
	res = 1;
	for (i=0; (i<iterations) && (res == 1); i++) {
		res = do_waiting(chan, timereqd, waitstart, timeout, wait_for_silence);
	}
	if (silgen) {
		tris_channel_stop_silence_generator(chan, silgen);
	}


	if (res > 0)
		res = 0;
	return res;
}

static int waitforsilence_exec(struct tris_channel *chan, void *data)
{
	return waitfor_exec(chan, data, 1);
}

static int waitfornoise_exec(struct tris_channel *chan, void *data)
{
	return waitfor_exec(chan, data, 0);
}

static int unload_module(void)
{
	int res;
	res = tris_unregister_application(app_silence);
	res |= tris_unregister_application(app_noise);

	return res;
}

static int load_module(void)
{
	int res;

	res = tris_register_application_xml(app_silence, waitforsilence_exec);
	res |= tris_register_application_xml(app_noise, waitfornoise_exec);
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Wait For Silence");

