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
 * \brief Stream to an icecast server via ICES (see contrib/trismedia-ices.xml)
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \extref ICES - http://www.icecast.org/ices.php
 *
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 153365 $")

#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_CONFIG_DIR */
#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/frame.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="ICES" language="en_US">
		<synopsis>
			Encode and stream using 'ices'.
		</synopsis>
		<syntax>
			<parameter name="config" required="true">
				<para>ICES configuration file.</para>
			</parameter>
		</syntax>
		<description>
			<para>Streams to an icecast server using ices (available separately).
			A configuration file must be supplied for ices (see contrib/trismedia-ices.xml).</para>
			<note><para>ICES version 2 cient and server required.</para></note>
		</description>
	</application>

 ***/

#define path_BIN "/usr/bin/"
#define path_LOCAL "/usr/local/bin/"

static char *app = "ICES";

static int icesencode(char *filename, int fd)
{
	int res;

	res = tris_safe_fork(0);
	if (res < 0) 
		tris_log(LOG_WARNING, "Fork failed\n");
	if (res) {
		return res;
	}

	if (tris_opt_high_priority)
		tris_set_priority(0);
	dup2(fd, STDIN_FILENO);
	tris_close_fds_above_n(STDERR_FILENO);

	/* Most commonly installed in /usr/local/bin 
	 * But many places has it in /usr/bin 
	 * As a last-ditch effort, try to use PATH
	 */
	execl(path_LOCAL "ices2", "ices", filename, SENTINEL);
	execl(path_BIN "ices2", "ices", filename, SENTINEL);
	execlp("ices2", "ices", filename, SENTINEL);

	tris_debug(1, "Couldn't find ices version 2, attempting to use ices version 1.");

	execl(path_LOCAL "ices", "ices", filename, SENTINEL);
	execl(path_BIN "ices", "ices", filename, SENTINEL);
	execlp("ices", "ices", filename, SENTINEL);

	tris_log(LOG_WARNING, "Execute of ices failed, could not find command.\n");
	close(fd);
	_exit(0);
}

static int ices_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	int fds[2];
	int ms = -1;
	int pid = -1;
	int flags;
	int oreadformat;
	struct timeval last;
	struct tris_frame *f;
	char filename[256]="";
	char *c;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "ICES requires an argument (configfile.xml)\n");
		return -1;
	}
	
	last = tris_tv(0, 0);
	
	if (pipe(fds)) {
		tris_log(LOG_WARNING, "Unable to create pipe\n");
		return -1;
	}
	flags = fcntl(fds[1], F_GETFL);
	fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
	
	tris_stopstream(chan);

	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);
		
	if (res) {
		close(fds[0]);
		close(fds[1]);
		tris_log(LOG_WARNING, "Answer failed!\n");
		return -1;
	}

	oreadformat = chan->readformat;
	res = tris_set_read_format(chan, TRIS_FORMAT_SLINEAR);
	if (res < 0) {
		close(fds[0]);
		close(fds[1]);
		tris_log(LOG_WARNING, "Unable to set write format to signed linear\n");
		return -1;
	}
	if (((char *)data)[0] == '/')
		tris_copy_string(filename, (char *) data, sizeof(filename));
	else
		snprintf(filename, sizeof(filename), "%s/%s", tris_config_TRIS_CONFIG_DIR, (char *)data);
	/* Placeholder for options */		
	c = strchr(filename, '|');
	if (c)
		*c = '\0';	
	res = icesencode(filename, fds[0]);
	if (res >= 0) {
		pid = res;
		for (;;) {
			/* Wait for audio, and stream */
			ms = tris_waitfor(chan, -1);
			if (ms < 0) {
				tris_debug(1, "Hangup detected\n");
				res = -1;
				break;
			}
			f = tris_read(chan);
			if (!f) {
				tris_debug(1, "Null frame == hangup() detected\n");
				res = -1;
				break;
			}
			if (f->frametype == TRIS_FRAME_VOICE) {
				res = write(fds[1], f->data.ptr, f->datalen);
				if (res < 0) {
					if (errno != EAGAIN) {
						tris_log(LOG_WARNING, "Write failed to pipe: %s\n", strerror(errno));
						res = -1;
						tris_frfree(f);
						break;
					}
				}
			}
			tris_frfree(f);
		}
	}
	close(fds[0]);
	close(fds[1]);

	if (pid > -1)
		kill(pid, SIGKILL);
	if (!res && oreadformat)
		tris_set_read_format(chan, oreadformat);

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, ices_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Encode and Stream via icecast and ices");
