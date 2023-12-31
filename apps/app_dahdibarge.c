/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Special thanks to comphealth.com for sponsoring this
 * GPL application.
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
 * \brief DAHDI Barge support
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \note Special thanks to comphealth.com for sponsoring this
 * GPL application.
 * 
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>dahdi</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include <dahdi/user.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/app.h"
#include "trismedia/cli.h"
#include "trismedia/say.h"
#include "trismedia/utils.h"

/*** DOCUMENTATION
	<application name="DAHDIBarge" language="en_US">
		<synopsis>
			Barge in (monitor) DAHDI channel.
		</synopsis>
		<syntax>
			<parameter name="channel">
				<para>Channel to barge.</para>
			</parameter>
		</syntax>
		<description>
			<para>Barges in on a specified DAHDI <replaceable>channel</replaceable> or prompts
			if one is not specified. Returns <literal>-1</literal> when caller user hangs
			up and is independent of the state of the channel being monitored.
			</para>
		</description>
	</application>
 ***/
static char *app = "DAHDIBarge";

#define CONF_SIZE 160

static int careful_write(int fd, unsigned char *data, int len)
{
	int res;
	while(len) {
		res = write(fd, data, len);
		if (res < 1) {
			if (errno != EAGAIN) {
				tris_log(LOG_WARNING, "Failed to write audio data to conference: %s\n", strerror(errno));
				return -1;
			} else
				return 0;
		}
		len -= res;
		data += res;
	}
	return 0;
}

static int conf_run(struct tris_channel *chan, int confno, int confflags)
{
	int fd;
	struct dahdi_confinfo dahdic;
	struct tris_frame *f;
	struct tris_channel *c;
	struct tris_frame fr;
	int outfd;
	int ms;
	int nfds;
	int res;
	int flags;
	int retrydahdi;
	int origfd;
	int ret = -1;

	struct dahdi_bufferinfo bi;
	char __buf[CONF_SIZE + TRIS_FRIENDLY_OFFSET];
	char *buf = __buf + TRIS_FRIENDLY_OFFSET;

	/* Set it into U-law mode (write) */
	if (tris_set_write_format(chan, TRIS_FORMAT_ULAW) < 0) {
		tris_log(LOG_WARNING, "Unable to set '%s' to write ulaw mode\n", chan->name);
		goto outrun;
	}

	/* Set it into U-law mode (read) */
	if (tris_set_read_format(chan, TRIS_FORMAT_ULAW) < 0) {
		tris_log(LOG_WARNING, "Unable to set '%s' to read ulaw mode\n", chan->name);
		goto outrun;
	}
	tris_indicate(chan, -1);
	retrydahdi = strcasecmp(chan->tech->type, "DAHDI");
dahdiretry:
	origfd = chan->fds[0];
	if (retrydahdi) {
		fd = open("/dev/dahdi/pseudo", O_RDWR);
		if (fd < 0) {
			tris_log(LOG_WARNING, "Unable to open pseudo channel: %s\n", strerror(errno));
			goto outrun;
		}
		/* Make non-blocking */
		flags = fcntl(fd, F_GETFL);
		if (flags < 0) {
			tris_log(LOG_WARNING, "Unable to get flags: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
			tris_log(LOG_WARNING, "Unable to set flags: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		/* Setup buffering information */
		memset(&bi, 0, sizeof(bi));
		bi.bufsize = CONF_SIZE;
		bi.txbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.rxbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.numbufs = 4;
		if (ioctl(fd, DAHDI_SET_BUFINFO, &bi)) {
			tris_log(LOG_WARNING, "Unable to set buffering information: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		nfds = 1;
	} else {
		/* XXX Make sure we're not running on a pseudo channel XXX */
		fd = chan->fds[0];
		nfds = 0;
	}
	memset(&dahdic, 0, sizeof(dahdic));
	/* Check to see if we're in a conference... */
	dahdic.chan = 0;	
	if (ioctl(fd, DAHDI_GETCONF, &dahdic)) {
		tris_log(LOG_WARNING, "Error getting conference\n");
		close(fd);
		goto outrun;
	}
	if (dahdic.confmode) {
		/* Whoa, already in a conference...  Retry... */
		if (!retrydahdi) {
			tris_debug(1, "DAHDI channel is in a conference already, retrying with pseudo\n");
			retrydahdi = 1;
			goto dahdiretry;
		}
	}
	memset(&dahdic, 0, sizeof(dahdic));
	/* Add us to the conference */
	dahdic.chan = 0;	
	dahdic.confno = confno;
	dahdic.confmode = DAHDI_CONF_MONITORBOTH;

	if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
		tris_log(LOG_WARNING, "Error setting conference\n");
		close(fd);
		goto outrun;
	}
	tris_debug(1, "Placed channel %s in DAHDI channel %d monitor\n", chan->name, confno);

	for(;;) {
		outfd = -1;
		ms = -1;
		c = tris_waitfor_nandfds(&chan, 1, &fd, nfds, NULL, &outfd, &ms);
		if (c) {
			if (c->fds[0] != origfd) {
				if (retrydahdi) {
					/* Kill old pseudo */
					close(fd);
				}
				tris_debug(1, "Ooh, something swapped out under us, starting over\n");
				retrydahdi = 0;
				goto dahdiretry;
			}
			f = tris_read(c);
			if (!f) 
				break;
			if ((f->frametype == TRIS_FRAME_DTMF) && (f->subclass == '#')) {
				ret = 0;
				tris_frfree(f);
				break;
			} else if (fd != chan->fds[0]) {
				if (f->frametype == TRIS_FRAME_VOICE) {
					if (f->subclass == TRIS_FORMAT_ULAW) {
						/* Carefully write */
						careful_write(fd, f->data.ptr, f->datalen);
					} else
						tris_log(LOG_WARNING, "Huh?  Got a non-ulaw (%d) frame in the conference\n", f->subclass);
				}
			}
			tris_frfree(f);
		} else if (outfd > -1) {
			res = read(outfd, buf, CONF_SIZE);
			if (res > 0) {
				memset(&fr, 0, sizeof(fr));
				fr.frametype = TRIS_FRAME_VOICE;
				fr.subclass = TRIS_FORMAT_ULAW;
				fr.datalen = res;
				fr.samples = res;
				fr.data.ptr = buf;
				fr.offset = TRIS_FRIENDLY_OFFSET;
				if (tris_write(chan, &fr) < 0) {
					tris_log(LOG_WARNING, "Unable to write frame to channel: %s\n", strerror(errno));
					/* break; */
				}
			} else 
				tris_log(LOG_WARNING, "Failed to read frame: %s\n", strerror(errno));
		}
	}
	if (fd != chan->fds[0])
		close(fd);
	else {
		/* Take out of conference */
		/* Add us to the conference */
		dahdic.chan = 0;	
		dahdic.confno = 0;
		dahdic.confmode = 0;
		if (ioctl(fd, DAHDI_SETCONF, &dahdic)) {
			tris_log(LOG_WARNING, "Error setting conference\n");
		}
	}

outrun:

	return ret;
}

static int conf_exec(struct tris_channel *chan, void *data)
{
	int res = -1;
	int retrycnt = 0;
	int confflags = 0;
	int confno = 0;
	char confnostr[80] = "";
	
	if (!tris_strlen_zero(data)) {
		if ((sscanf(data, "DAHDI/%30d", &confno) != 1) &&
		    (sscanf(data, "%30d", &confno) != 1)) {
			tris_log(LOG_WARNING, "DAHDIBarge Argument (if specified) must be a channel number, not '%s'\n", (char *)data);
			return 0;
		}
	}
	
	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	while(!confno && (++retrycnt < 4)) {
		/* Prompt user for conference number */
		confnostr[0] = '\0';
		res = tris_app_getdata(chan, "conference/conf-getchannel",confnostr, sizeof(confnostr) - 1, 0);
		if (res <0) goto out;
		if (sscanf(confnostr, "%30d", &confno) != 1)
			confno = 0;
	}
	if (confno) {
		/* XXX Should prompt user for pin if pin is required XXX */
		/* Run the conference */
		res = conf_run(chan, confno, confflags);
	}
out:
	/* Do the conference */
	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return ((tris_register_application_xml(app, conf_exec)) ? TRIS_MODULE_LOAD_FAILURE : TRIS_MODULE_LOAD_SUCCESS);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Barge in on DAHDI channel application");
