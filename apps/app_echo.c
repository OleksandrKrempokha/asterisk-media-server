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
 * \brief Echo application -- play back what you hear to evaluate latency
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 249952 $")

#include "trismedia/file.h"
#include "trismedia/module.h"
#include "trismedia/channel.h"

/*** DOCUMENTATION
	<application name="Echo" language="en_US">
		<synopsis>
			Echo audio, video, DTMF back to the calling party
		</synopsis>
		<syntax />
		<description>
			<para>Echos back any audio, video or DTMF frames read from the calling 
			channel back to itself. Note: If '#' detected application exits</para>
		</description>
	</application>
 ***/

static char *app = "Echo";

static int echo_exec(struct tris_channel *chan, void *data)
{
	int res = -1;
	int format;

	format = tris_best_codec(chan->nativeformats);
	tris_set_write_format(chan, format);
	tris_set_read_format(chan, format);

	while (tris_waitfor(chan, -1) > -1) {
		struct tris_frame *f = tris_read(chan);
		if (!f) {
			break;
		}
		f->delivery.tv_sec = 0;
		f->delivery.tv_usec = 0;
		if (tris_write(chan, f)) {
			tris_frfree(f);
			goto end;
		}
		if ((f->frametype == TRIS_FRAME_DTMF) && (f->subclass == '#')) {
			res = 0;
			tris_frfree(f);
			goto end;
		}
		tris_frfree(f);
	}
end:
	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, echo_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Simple Echo Application");
