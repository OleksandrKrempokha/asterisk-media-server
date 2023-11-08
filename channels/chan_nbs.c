/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Network broadcast sound support channel driver
 * 
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup channel_drivers
 */

/*** MODULEINFO
	<depend>nbs</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 117870 $")

#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nbs.h>

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"

static const char tdesc[] = "Network Broadcast Sound Driver";

/* Only linear is allowed */
static int prefformat = TRIS_FORMAT_SLINEAR;

static char context[TRIS_MAX_EXTENSION] = "default";
static char type[] = "NBS";

/* NBS creates private structures on demand */
   
struct nbs_pvt {
	NBS *nbs;
	struct tris_channel *owner;		/* Channel we belong to, possibly NULL */
	char app[16];					/* Our app */
	char stream[80];				/* Our stream */
	struct tris_frame fr;			/* "null" frame */
	struct tris_module_user *u;		/*! for holding a reference to this module */
};

static struct tris_channel *nbs_request(const char *type, int format, void *data, int *cause, struct tris_channel* src);
static int nbs_call(struct tris_channel *ast, char *dest, int timeout);
static int nbs_hangup(struct tris_channel *ast);
static struct tris_frame *nbs_xread(struct tris_channel *ast);
static int nbs_xwrite(struct tris_channel *ast, struct tris_frame *frame);

static const struct tris_channel_tech nbs_tech = {
	.type = type,
	.description = tdesc,
	.capabilities = TRIS_FORMAT_SLINEAR,
	.requester = nbs_request,
	.call = nbs_call,
	.hangup = nbs_hangup,
	.read = nbs_xread,
	.write = nbs_xwrite,
};

static int nbs_call(struct tris_channel *ast, char *dest, int timeout)
{
	struct nbs_pvt *p;

	p = ast->tech_pvt;

	if ((ast->_state != TRIS_STATE_DOWN) && (ast->_state != TRIS_STATE_RESERVED)) {
		tris_log(LOG_WARNING, "nbs_call called on %s, neither down nor reserved\n", ast->name);
		return -1;
	}
	/* When we call, it just works, really, there's no destination...  Just
	   ring the phone and wait for someone to answer */
	tris_debug(1, "Calling %s on %s\n", dest, ast->name);

	/* If we can't connect, return congestion */
	if (nbs_connect(p->nbs)) {
		tris_log(LOG_WARNING, "NBS Connection failed on %s\n", ast->name);
		tris_queue_control(ast, TRIS_CONTROL_CONGESTION);
	} else {
		tris_setstate(ast, TRIS_STATE_RINGING);
		tris_queue_control(ast, TRIS_CONTROL_ANSWER);
	}

	return 0;
}

static void nbs_destroy(struct nbs_pvt *p)
{
	if (p->nbs)
		nbs_delstream(p->nbs);
	tris_module_user_remove(p->u);
	tris_free(p);
}

static struct nbs_pvt *nbs_alloc(void *data)
{
	struct nbs_pvt *p;
	int flags = 0;
	char stream[256];
	char *opts;

	tris_copy_string(stream, data, sizeof(stream));
	if ((opts = strchr(stream, ':'))) {
		*opts = '\0';
		opts++;
	} else
		opts = "";
	p = tris_calloc(1, sizeof(*p));
	if (p) {
		if (!tris_strlen_zero(opts)) {
			if (strchr(opts, 'm'))
				flags |= NBS_FLAG_MUTE;
			if (strchr(opts, 'o'))
				flags |= NBS_FLAG_OVERSPEAK;
			if (strchr(opts, 'e'))
				flags |= NBS_FLAG_EMERGENCY;
			if (strchr(opts, 'O'))
				flags |= NBS_FLAG_OVERRIDE;
		} else
			flags = NBS_FLAG_OVERSPEAK;
		
		tris_copy_string(p->stream, stream, sizeof(p->stream));
		p->nbs = nbs_newstream("trismedia", stream, flags);
		if (!p->nbs) {
			tris_log(LOG_WARNING, "Unable to allocate new NBS stream '%s' with flags %d\n", stream, flags);
			tris_free(p);
			p = NULL;
		} else {
			/* Set for 8000 hz mono, 640 samples */
			nbs_setbitrate(p->nbs, 8000);
			nbs_setchannels(p->nbs, 1);
			nbs_setblocksize(p->nbs, 640);
			nbs_setblocking(p->nbs, 0);
		}
	}
	return p;
}

static int nbs_hangup(struct tris_channel *ast)
{
	struct nbs_pvt *p;
	p = ast->tech_pvt;
	tris_debug(1, "nbs_hangup(%s)\n", ast->name);
	if (!ast->tech_pvt) {
		tris_log(LOG_WARNING, "Asked to hangup channel not connected\n");
		return 0;
	}
	nbs_destroy(p);
	ast->tech_pvt = NULL;
	tris_setstate(ast, TRIS_STATE_DOWN);
	return 0;
}

static struct tris_frame  *nbs_xread(struct tris_channel *ast)
{
	struct nbs_pvt *p = ast->tech_pvt;
	

	/* Some nice norms */
	p->fr.datalen = 0;
	p->fr.samples = 0;
	p->fr.data.ptr =  NULL;
	p->fr.src = type;
	p->fr.offset = 0;
	p->fr.mallocd=0;
	p->fr.delivery.tv_sec = 0;
	p->fr.delivery.tv_usec = 0;

	tris_debug(1, "Returning null frame on %s\n", ast->name);

	return &p->fr;
}

static int nbs_xwrite(struct tris_channel *ast, struct tris_frame *frame)
{
	struct nbs_pvt *p = ast->tech_pvt;
	/* Write a frame of (presumably voice) data */
	if (frame->frametype != TRIS_FRAME_VOICE) {
		if (frame->frametype != TRIS_FRAME_IMAGE)
			tris_log(LOG_WARNING, "Don't know what to do with  frame type '%d'\n", frame->frametype);
		return 0;
	}
	if (!(frame->subclass &
		(TRIS_FORMAT_SLINEAR))) {
		tris_log(LOG_WARNING, "Cannot handle frames in %d format\n", frame->subclass);
		return 0;
	}
	if (ast->_state != TRIS_STATE_UP) {
		/* Don't try tos end audio on-hook */
		return 0;
	}
	if (nbs_write(p->nbs, frame->data.ptr, frame->datalen / 2) < 0) 
		return -1;
	return 0;
}

static struct tris_channel *nbs_new(struct nbs_pvt *i, int state)
{
	struct tris_channel *tmp;
	tmp = tris_channel_alloc(1, state, 0, 0, "", "s", context, 0, "NBS/%s", i->stream);
	if (tmp) {
		tmp->tech = &nbs_tech;
		tris_channel_set_fd(tmp, 0, nbs_fd(i->nbs));
		tmp->nativeformats = prefformat;
		tmp->rawreadformat = prefformat;
		tmp->rawwriteformat = prefformat;
		tmp->writeformat = prefformat;
		tmp->readformat = prefformat;
		if (state == TRIS_STATE_RING)
			tmp->rings = 1;
		tmp->tech_pvt = i;
		tris_copy_string(tmp->context, context, sizeof(tmp->context));
		tris_copy_string(tmp->exten, "s",  sizeof(tmp->exten));
		tris_string_field_set(tmp, language, "");
		i->owner = tmp;
		i->u = tris_module_user_add(tmp);
		if (state != TRIS_STATE_DOWN) {
			if (tris_pbx_start(tmp)) {
				tris_log(LOG_WARNING, "Unable to start PBX on %s\n", tmp->name);
				tris_hangup(tmp);
			}
		}
	} else
		tris_log(LOG_WARNING, "Unable to allocate channel structure\n");
	return tmp;
}


static struct tris_channel *nbs_request(const char *type, int format, void *data, int *cause, struct tris_channel* src)
{
	int oldformat;
	struct nbs_pvt *p;
	struct tris_channel *tmp = NULL;
	
	oldformat = format;
	format &= (TRIS_FORMAT_SLINEAR);
	if (!format) {
		tris_log(LOG_NOTICE, "Asked to get a channel of unsupported format '%d'\n", oldformat);
		return NULL;
	}
	p = nbs_alloc(data);
	if (p) {
		tmp = nbs_new(p, TRIS_STATE_DOWN);
		if (!tmp)
			nbs_destroy(p);
	}
	return tmp;
}

static int unload_module(void)
{
	/* First, take us out of the channel loop */
	tris_channel_unregister(&nbs_tech);
	return 0;
}

static int load_module(void)
{
	/* Make sure we can register our channel type */
	if (tris_channel_register(&nbs_tech)) {
		tris_log(LOG_ERROR, "Unable to register channel class %s\n", type);
		return -1;
	}
	return 0;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Network Broadcast Sound Support");
