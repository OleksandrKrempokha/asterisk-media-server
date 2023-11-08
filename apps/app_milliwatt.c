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
 * \brief Digital Milliwatt Test
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 209842 $")

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/indications.h"

/*** DOCUMENTATION
	<application name="Milliwatt" language="en_US">
		<synopsis>
			Generate a Constant 1004Hz tone at 0dbm (mu-law).
		</synopsis>
		<syntax>
			<parameter name="options">
				<optionlist>
					<option name="o">
						<para>Generate the tone at 1000Hz like previous version.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Previous versions of this application generated the tone at 1000Hz.  If for
			some reason you would prefer that behavior, supply the <literal>o</literal> option to get the
			old behavior.</para>
		</description>
	</application>
 ***/

static char *app = "Milliwatt";

static char digital_milliwatt[] = {0x1e,0x0b,0x0b,0x1e,0x9e,0x8b,0x8b,0x9e} ;

static void *milliwatt_alloc(struct tris_channel *chan, void *params)
{
	return tris_calloc(1, sizeof(int));
}

static void milliwatt_release(struct tris_channel *chan, void *data)
{
	tris_free(data);
	return;
}

static int milliwatt_generate(struct tris_channel *chan, void *data, int len, int samples)
{
	unsigned char buf[TRIS_FRIENDLY_OFFSET + 640];
	const int maxsamples = ARRAY_LEN(buf);
	int i, *indexp = (int *) data;
	struct tris_frame wf = {
		.frametype = TRIS_FRAME_VOICE,
		.subclass = TRIS_FORMAT_ULAW,
		.offset = TRIS_FRIENDLY_OFFSET,
		.src = __FUNCTION__,
	};
	wf.data.ptr = buf + TRIS_FRIENDLY_OFFSET;

	/* Instead of len, use samples, because channel.c generator_force
	* generate(chan, tmp, 0, 160) ignores len. In any case, len is
	* a multiple of samples, given by number of samples times bytes per
	* sample. In the case of ulaw, len = samples. for signed linear
	* len = 2 * samples */
	if (samples > maxsamples) {
		tris_log(LOG_WARNING, "Only doing %d samples (%d requested)\n", maxsamples, samples);
		samples = maxsamples;
	}

	len = samples * sizeof (buf[0]);
	wf.datalen = len;
	wf.samples = samples;

	/* create a buffer containing the digital milliwatt pattern */
	for (i = 0; i < len; i++) {
		buf[TRIS_FRIENDLY_OFFSET + i] = digital_milliwatt[(*indexp)++];
		*indexp &= 7;
	}

	if (tris_write(chan,&wf) < 0) {
		tris_log(LOG_WARNING,"Failed to write frame to '%s': %s\n",chan->name,strerror(errno));
		return -1;
	}

	return 0;
}

static struct tris_generator milliwattgen = {
	alloc: milliwatt_alloc,
	release: milliwatt_release,
	generate: milliwatt_generate,
};

static int old_milliwatt_exec(struct tris_channel *chan)
{
	tris_set_write_format(chan, TRIS_FORMAT_ULAW);
	tris_set_read_format(chan, TRIS_FORMAT_ULAW);

	if (chan->_state != TRIS_STATE_UP) {
		tris_answer(chan);
	}

	if (tris_activate_generator(chan,&milliwattgen,"milliwatt") < 0) {
		tris_log(LOG_WARNING,"Failed to activate generator on '%s'\n",chan->name);
		return -1;
	}

	while (!tris_safe_sleep(chan, 10000))
		;

	tris_deactivate_generator(chan);

	return -1;
}

static int milliwatt_exec(struct tris_channel *chan, void *data)
{
	const char *options = data;
	int res = -1;

	if (!tris_strlen_zero(options) && strchr(options, 'o')) {
		return old_milliwatt_exec(chan);
	}

	res = tris_playtones_start(chan, 23255, "1004/1000", 0);

	while (!res) {
		res = tris_safe_sleep(chan, 10000);
	}

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, milliwatt_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Digital Milliwatt (mu-law) Test Application");
