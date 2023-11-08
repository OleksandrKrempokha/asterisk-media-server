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
 * \brief Wait for Ring Application
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 239713 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"

/*** DOCUMENTATION
	<application name="WaitForRing" language="en_US">
		<synopsis>
			Wait for Ring Application.
		</synopsis>
		<syntax>
			<parameter name="timeout" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>0</literal> after waiting at least <replaceable>timeout</replaceable> seconds,
			and only after the next ring has completed. Returns <literal>0</literal> on success or
			<literal>-1</literal> on hangup.</para>
		</description>
	</application>
 ***/

static char *app = "WaitForRing";

static int waitforring_exec(struct tris_channel *chan, void *data)
{
	struct tris_frame *f;
	struct tris_silence_generator *silgen = NULL;
	int res = 0;
	double s;
	int ms;

	if (!data || (sscanf(data, "%30lg", &s) != 1)) {
		tris_log(LOG_WARNING, "WaitForRing requires an argument (minimum seconds)\n");
		return 0;
	}

	if (tris_opt_transmit_silence) {
		silgen = tris_channel_start_silence_generator(chan);
	}

	ms = s * 1000.0;
	while (ms > 0) {
		ms = tris_waitfor(chan, ms);
		if (ms < 0) {
			res = ms;
			break;
		}
		if (ms > 0) {
			f = tris_read(chan);
			if (!f) {
				res = -1;
				break;
			}
			if ((f->frametype == TRIS_FRAME_CONTROL) && (f->subclass == TRIS_CONTROL_RING)) {
				tris_verb(3, "Got a ring but still waiting for timeout\n");
			}
			tris_frfree(f);
		}
	}
	/* Now we're really ready for the ring */
	if (!res) {
		ms = 99999999;
		while(ms > 0) {
			ms = tris_waitfor(chan, ms);
			if (ms < 0) {
				res = ms;
				break;
			}
			if (ms > 0) {
				f = tris_read(chan);
				if (!f) {
					res = -1;
					break;
				}
				if ((f->frametype == TRIS_FRAME_CONTROL) && (f->subclass == TRIS_CONTROL_RING)) {
					tris_verb(3, "Got a ring after the timeout\n");
					tris_frfree(f);
					break;
				}
				tris_frfree(f);
			}
		}
	}

	if (silgen) {
		tris_channel_stop_silence_generator(chan, silgen);
	}

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, waitforring_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Waits until first ring after time");
