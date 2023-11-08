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
 * \brief Silly application to play an MP3 file -- uses mpg123
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 238013 $")

#include <sys/time.h>
#include <signal.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/frame.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/app.h"

#define LOCAL_MPG_123 "/usr/local/bin/mpg123"
#define MPG_123 "/usr/bin/mpg123"

/*** DOCUMENTATION
	<application name="MP3Player" language="en_US">
		<synopsis>
			Play an MP3 file or stream.
		</synopsis>
		<syntax>
			<parameter name="Location" required="true">
				<para>Location of the file to be played.
				(argument passed to mpg123)</para>
			</parameter>
		</syntax>
		<description>
			<para>Executes mpg123 to play the given location, which typically would be a filename or a URL.
			User can exit by pressing any key on the dialpad, or by hanging up.</para>
		</description>
	</application>

 ***/
static char *app = "MP3Player";

static int mp3play(char *filename, int fd)
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

	dup2(fd, STDOUT_FILENO);
	tris_close_fds_above_n(STDERR_FILENO);

	/* Execute mpg123, but buffer if it's a net connection */
	if (!strncasecmp(filename, "http://", 7)) {
		/* Most commonly installed in /usr/local/bin */
	    execl(LOCAL_MPG_123, "mpg123", "-q", "-s", "-b", "1024", "-f", "8192", "--mono", "-r", "8000", filename, (char *)NULL);
		/* But many places has it in /usr/bin */
	    execl(MPG_123, "mpg123", "-q", "-s", "-b", "1024","-f", "8192", "--mono", "-r", "8000", filename, (char *)NULL);
		/* As a last-ditch effort, try to use PATH */
	    execlp("mpg123", "mpg123", "-q", "-s", "-b", "1024",  "-f", "8192", "--mono", "-r", "8000", filename, (char *)NULL);
	}
	else {
		/* Most commonly installed in /usr/local/bin */
	    execl(MPG_123, "mpg123", "-q", "-s", "-f", "8192", "--mono", "-r", "8000", filename, (char *)NULL);
		/* But many places has it in /usr/bin */
	    execl(LOCAL_MPG_123, "mpg123", "-q", "-s", "-f", "8192", "--mono", "-r", "8000", filename, (char *)NULL);
		/* As a last-ditch effort, try to use PATH */
	    execlp("mpg123", "mpg123", "-q", "-s", "-f", "8192", "--mono", "-r", "8000", filename, (char *)NULL);
	}
	/* Can't use tris_log since FD's are closed */
	fprintf(stderr, "Execute of mpg123 failed\n");
	_exit(0);
}

static int timed_read(int fd, void *data, int datalen, int timeout)
{
	int res;
	struct pollfd fds[1];
	fds[0].fd = fd;
	fds[0].events = POLLIN;
	res = tris_poll(fds, 1, timeout);
	if (res < 1) {
		tris_log(LOG_NOTICE, "Poll timed out/errored out with %d\n", res);
		return -1;
	}
	return read(fd, data, datalen);
	
}

static int mp3_exec(struct tris_channel *chan, void *data)
{
	int res=0;
	int fds[2];
	int ms = -1;
	int pid = -1;
	int owriteformat;
	int timeout = 2000;
	struct timeval next;
	struct tris_frame *f;
	struct myframe {
		struct tris_frame f;
		char offset[TRIS_FRIENDLY_OFFSET];
		short frdata[160];
	} myf = {
		.f = { 0, },
	};

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "MP3 Playback requires an argument (filename)\n");
		return -1;
	}

	if (pipe(fds)) {
		tris_log(LOG_WARNING, "Unable to create pipe\n");
		return -1;
	}
	
	tris_stopstream(chan);

	owriteformat = chan->writeformat;
	res = tris_set_write_format(chan, TRIS_FORMAT_SLINEAR);
	if (res < 0) {
		tris_log(LOG_WARNING, "Unable to set write format to signed linear\n");
		return -1;
	}
	
	res = mp3play((char *)data, fds[1]);
	if (!strncasecmp((char *)data, "http://", 7)) {
		timeout = 10000;
	}
	/* Wait 1000 ms first */
	next = tris_tvnow();
	next.tv_sec += 1;
	if (res >= 0) {
		pid = res;
		/* Order is important -- there's almost always going to be mp3...  we want to prioritize the
		   user */
		for (;;) {
			ms = tris_tvdiff_ms(next, tris_tvnow());
			if (ms <= 0) {
				res = timed_read(fds[0], myf.frdata, sizeof(myf.frdata), timeout);
				if (res > 0) {
					myf.f.frametype = TRIS_FRAME_VOICE;
					myf.f.subclass = TRIS_FORMAT_SLINEAR;
					myf.f.datalen = res;
					myf.f.samples = res / 2;
					myf.f.mallocd = 0;
					myf.f.offset = TRIS_FRIENDLY_OFFSET;
					myf.f.src = __PRETTY_FUNCTION__;
					myf.f.delivery.tv_sec = 0;
					myf.f.delivery.tv_usec = 0;
					myf.f.data.ptr = myf.frdata;
					if (tris_write(chan, &myf.f) < 0) {
						res = -1;
						break;
					}
				} else {
					tris_debug(1, "No more mp3\n");
					res = 0;
					break;
				}
				next = tris_tvadd(next, tris_samp2tv(myf.f.samples, 8000));
			} else {
				ms = tris_waitfor(chan, ms);
				if (ms < 0) {
					tris_debug(1, "Hangup detected\n");
					res = -1;
					break;
				}
				if (ms) {
					f = tris_read(chan);
					if (!f) {
						tris_debug(1, "Null frame == hangup() detected\n");
						res = -1;
						break;
					}
					if (f->frametype == TRIS_FRAME_DTMF) {
						tris_debug(1, "User pressed a key\n");
						tris_frfree(f);
						res = 0;
						break;
					}
					tris_frfree(f);
				} 
			}
		}
	}
	close(fds[0]);
	close(fds[1]);
	
	if (pid > -1)
		kill(pid, SIGKILL);
	if (!res && owriteformat)
		tris_set_write_format(chan, owriteformat);
	
	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, mp3_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Silly MP3 Application");
