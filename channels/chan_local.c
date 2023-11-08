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
 * \author Mark Spencer <markster@digium.com>
 *
 * \brief Local Proxy Channel
 * 
 * \ingroup channel_drivers
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 249895 $")

#include <fcntl.h>
#include <sys/signal.h>

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/sched.h"
#include "trismedia/io.h"
#include "trismedia/rtp.h"
#include "trismedia/acl.h"
#include "trismedia/callerid.h"
#include "trismedia/file.h"
#include "trismedia/cli.h"
#include "trismedia/app.h"
#include "trismedia/musiconhold.h"
#include "trismedia/manager.h"
#include "trismedia/stringfields.h"
#include "trismedia/devicestate.h"

static const char tdesc[] = "Local Proxy Channel Driver";

#define IS_OUTBOUND(a,b) (a == b->chan ? 1 : 0)

static struct tris_jb_conf g_jb_conf = {
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = "",
	.target_extra = -1,
};

static struct tris_channel *local_request(const char *type, int format, void *data, int *cause, struct tris_channel* src);
static int local_digit_begin(struct tris_channel *ast, char digit);
static int local_digit_end(struct tris_channel *ast, char digit, unsigned int duration);
static int local_call(struct tris_channel *ast, char *dest, int timeout);
static int local_hangup(struct tris_channel *ast);
static int local_answer(struct tris_channel *ast);
static struct tris_frame *local_read(struct tris_channel *ast);
static int local_write(struct tris_channel *ast, struct tris_frame *f);
static int local_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen);
static int local_fixup(struct tris_channel *oldchan, struct tris_channel *newchan);
static int local_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen);
static int local_sendtext(struct tris_channel *ast, const char *text);
static int local_devicestate(void *data);
static struct tris_channel *local_bridgedchannel(struct tris_channel *chan, struct tris_channel *bridge);

/* PBX interface structure for channel registration */
static const struct tris_channel_tech local_tech = {
	.type = "Local",
	.description = tdesc,
	.capabilities = -1,
	.requester = local_request,
	.send_digit_begin = local_digit_begin,
	.send_digit_end = local_digit_end,
	.call = local_call,
	.hangup = local_hangup,
	.answer = local_answer,
	.read = local_read,
	.write = local_write,
	.write_video = local_write,
	.exception = local_read,
	.indicate = local_indicate,
	.fixup = local_fixup,
	.send_html = local_sendhtml,
	.send_text = local_sendtext,
	.devicestate = local_devicestate,
	.bridged_channel = local_bridgedchannel,
};

struct local_pvt {
	tris_mutex_t lock;			/* Channel private lock */
	unsigned int flags;                     /* Private flags */
	char context[TRIS_MAX_CONTEXT];		/* Context to call */
	char exten[TRIS_MAX_EXTENSION];		/* Extension to call */
	int reqformat;				/* Requested format */
	struct tris_jb_conf jb_conf;		/*!< jitterbuffer configuration for this local channel */
	struct tris_channel *owner;		/* Master Channel - Bridging happens here */
	struct tris_channel *chan;		/* Outbound channel - PBX is run here */
	struct tris_module_user *u_owner;	/*! reference to keep the module loaded while in use */
	struct tris_module_user *u_chan;		/*! reference to keep the module loaded while in use */
	TRIS_LIST_ENTRY(local_pvt) list;		/* Next entity */
};

#define LOCAL_GLARE_DETECT    (1 << 0) /*!< Detect glare on hangup */
#define LOCAL_CANCEL_QUEUE    (1 << 1) /*!< Cancel queue */
#define LOCAL_ALREADY_MASQED  (1 << 2) /*!< Already masqueraded */
#define LOCAL_LAUNCHED_PBX    (1 << 3) /*!< PBX was launched */
#define LOCAL_NO_OPTIMIZATION (1 << 4) /*!< Do not optimize using masquerading */
#define LOCAL_BRIDGE          (1 << 5) /*!< Report back the "true" channel as being bridged to */
#define LOCAL_MOH_PASSTHRU    (1 << 6) /*!< Pass through music on hold start/stop frames */

static TRIS_LIST_HEAD_STATIC(locals, local_pvt);

/*! \brief Adds devicestate to local channels */
static int local_devicestate(void *data)
{
	char *exten = tris_strdupa(data);
	char *context = NULL, *opts = NULL;
	int res;
	struct local_pvt *lp;

	if (!(context = strchr(exten, '@'))) {
		tris_log(LOG_WARNING, "Someone used Local/%s somewhere without a @context. This is bad.\n", exten);
		return TRIS_DEVICE_INVALID;	
	}

	*context++ = '\0';

	/* Strip options if they exist */
	if ((opts = strchr(context, '/')))
		*opts = '\0';

	tris_debug(3, "Checking if extension %s@%s exists (devicestate)\n", exten, context);

	res = tris_exists_extension(NULL, context, exten, 1, NULL);
	if (!res)		
		return TRIS_DEVICE_INVALID;
	
	res = TRIS_DEVICE_NOT_INUSE;
	TRIS_LIST_LOCK(&locals);
	TRIS_LIST_TRAVERSE(&locals, lp, list) {
		if (!strcmp(exten, lp->exten) && !strcmp(context, lp->context) && lp->owner) {
			res = TRIS_DEVICE_INUSE;
			break;
		}
	}
	TRIS_LIST_UNLOCK(&locals);

	return res;
}

/*!
 * \note Assumes the pvt is no longer in the pvts list
 */
static struct local_pvt *local_pvt_destroy(struct local_pvt *pvt)
{
	tris_mutex_destroy(&pvt->lock);
	tris_free(pvt);
	return NULL;
}

/*! \brief Return the bridged channel of a Local channel */
static struct tris_channel *local_bridgedchannel(struct tris_channel *chan, struct tris_channel *bridge)
{
	struct local_pvt *p = bridge->tech_pvt;
	struct tris_channel *bridged = bridge;

	if (!p) {
		tris_debug(1, "Asked for bridged channel on '%s'/'%s', returning <none>\n",
			chan->name, bridge->name);
		return NULL;
	}

	tris_mutex_lock(&p->lock);

	if (tris_test_flag(p, LOCAL_BRIDGE)) {
		/* Find the opposite channel */
		bridged = (bridge == p->owner ? p->chan : p->owner);
		
		/* Now see if the opposite channel is bridged to anything */
		if (!bridged) {
			bridged = bridge;
		} else if (bridged->_bridge) {
			bridged = bridged->_bridge;
		}
	}

	tris_mutex_unlock(&p->lock);

	return bridged;
}

static int local_queue_frame(struct local_pvt *p, int isoutbound, struct tris_frame *f, 
	struct tris_channel *us, int us_locked)
{
	struct tris_channel *other = NULL;
	const char* busy_peer;

	/* Recalculate outbound channel */
	other = isoutbound ? p->owner : p->chan;

	if (!other) {
		return 0;
	}

	/* do not queue frame if generator is on both local channels */
	if (us && us->generator && other->generator) {
		return 0;
	}

	/* Set glare detection */
	tris_set_flag(p, LOCAL_GLARE_DETECT);

	/* Ensure that we have both channels locked */
	while (other && tris_channel_trylock(other)) {
		tris_mutex_unlock(&p->lock);
		if (us && us_locked) {
			do {
				CHANNEL_DEADLOCK_AVOIDANCE(us);
			} while (tris_mutex_trylock(&p->lock));
		} else {
			usleep(1);
			tris_mutex_lock(&p->lock);
		}
		other = isoutbound ? p->owner : p->chan;
	}

	/* Since glare detection only occurs within this function, and because
	 * a pvt flag cannot be set without having the pvt lock, this is the only
	 * location where we could detect a cancelling of the queue. */
	if (tris_test_flag(p, LOCAL_CANCEL_QUEUE)) {
		/* We had a glare on the hangup.  Forget all this business,
		return and destroy p.  */
		tris_mutex_unlock(&p->lock);
		p = local_pvt_destroy(p);
		if (other) {
			tris_channel_unlock(other);
		}
		return -1;
	}

	busy_peer = pbx_builtin_getvar_helper(us, "Busy-Peer");
	if (other) {
		if (busy_peer && f->subclass == TRIS_CONTROL_NOTIFY_BUSY)
			pbx_builtin_setvar_helper(other, "Busy-Peer", busy_peer);
		if (f->frametype == TRIS_FRAME_CONTROL && f->subclass == TRIS_CONTROL_RINGING) {
			tris_setstate(other, TRIS_STATE_RINGING);
		}
		tris_queue_frame(other, f);
		tris_channel_unlock(other);
	}

	tris_clear_flag(p, LOCAL_GLARE_DETECT);

	return 0;
}

static int local_answer(struct tris_channel *ast)
{
	struct local_pvt *p = ast->tech_pvt;
	int isoutbound;
	int res = -1;

	if (!p)
		return -1;

	tris_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	if (isoutbound) {
		/* Pass along answer since somebody answered us */
		struct tris_frame answer = { TRIS_FRAME_CONTROL, TRIS_CONTROL_ANSWER };
		res = local_queue_frame(p, isoutbound, &answer, ast, 1);
	} else
		tris_log(LOG_WARNING, "Huh?  Local is being asked to answer?\n");
	if (!res)
		tris_mutex_unlock(&p->lock);
	return res;
}

static void check_bridge(struct local_pvt *p, int isoutbound)
{
	struct tris_channel_monitor *tmp;
	struct tris_channel *masq = NULL;
	
	if (tris_test_flag(p, LOCAL_ALREADY_MASQED) || tris_test_flag(p, LOCAL_NO_OPTIMIZATION) || !p->chan || !p->owner || (p->chan->_bridge != tris_bridged_channel(p->chan)))
		return;

	/* only do the masquerade if we are being called on the outbound channel,
	   if it has been bridged to another channel and if there are no pending
	   frames on the owner channel (because they would be transferred to the
	   outbound channel during the masquerade)
	*/
	if (isoutbound && p->chan->_bridge /* Not tris_bridged_channel!  Only go one step! */ && TRIS_LIST_EMPTY(&p->owner->readq)) {
		/* Masquerade bridged channel into owner */
		/* Lock everything we need, one by one, and give up if
		   we can't get everything.  Remember, we'll get another
		   chance in just a little bit */
		if (!tris_channel_trylock(p->chan->_bridge)) {
			if (!tris_check_hangup(p->chan->_bridge)) {
				if (!tris_channel_trylock(p->owner)) {
					if (!tris_check_hangup(p->owner)) {
						if (p->owner->monitor && !p->chan->_bridge->monitor) {
							/* If a local channel is being monitored, we don't want a masquerade
							 * to cause the monitor to go away. Since the masquerade swaps the monitors,
							 * pre-swapping the monitors before the masquerade will ensure that the monitor
							 * ends up where it is expected.
							 */
							tmp = p->owner->monitor;
							p->owner->monitor = p->chan->_bridge->monitor;
							p->chan->_bridge->monitor = tmp;
						}
						if (p->chan->audiohooks) {
							struct tris_audiohook_list *audiohooks_swapper;
							audiohooks_swapper = p->chan->audiohooks;
							p->chan->audiohooks = p->owner->audiohooks;
							p->owner->audiohooks = audiohooks_swapper;
						}
						tris_app_group_update(p->chan, p->owner);
						tris_channel_masquerade(p->owner, p->chan->_bridge);
						masq = p->owner;
						tris_set_flag(p, LOCAL_ALREADY_MASQED);
					}
					tris_channel_unlock(p->owner);
				}
			}
			tris_channel_unlock(p->chan->_bridge);
		}
		if (masq) {
			if (!tris_channel_trylock(masq)) {
				tris_do_masquerade(masq);
				tris_channel_unlock(masq);
			}
		}
	/* We only allow masquerading in one 'direction'... it's important to preserve the state
	   (group variables, etc.) that live on p->chan->_bridge (and were put there by the dialplan)
	   when the local channels go away.
	*/
#if 0
	} else if (!isoutbound && p->owner && p->owner->_bridge && p->chan && TRIS_LIST_EMPTY(&p->chan->readq)) {
		/* Masquerade bridged channel into chan */
		if (!tris_mutex_trylock(&(p->owner->_bridge)->lock)) {
			if (!tris_check_hangup(p->owner->_bridge)) {
				if (!tris_mutex_trylock(&p->chan->lock)) {
					if (!tris_check_hangup(p->chan)) {
						tris_channel_masquerade(p->chan, p->owner->_bridge);
						tris_set_flag(p, LOCAL_ALREADY_MASQED);
					}
					tris_mutex_unlock(&p->chan->lock);
				}
			}
			tris_mutex_unlock(&(p->owner->_bridge)->lock);
		}
#endif
	}
}

static struct tris_frame  *local_read(struct tris_channel *ast)
{
	return &tris_null_frame;
}

static int local_write(struct tris_channel *ast, struct tris_frame *f)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	int isoutbound;

	if (!p)
		return -1;

	/* Just queue for delivery to the other side */
	tris_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	if (f && (f->frametype == TRIS_FRAME_VOICE || f->frametype == TRIS_FRAME_VIDEO))
		check_bridge(p, isoutbound);
	if (!tris_test_flag(p, LOCAL_ALREADY_MASQED))
		res = local_queue_frame(p, isoutbound, f, ast, 1);
	else {
		tris_debug(1, "Not posting to queue since already masked on '%s'\n", ast->name);
		res = 0;
	}
	if (!res)
		tris_mutex_unlock(&p->lock);
	return res;
}

static int local_fixup(struct tris_channel *oldchan, struct tris_channel *newchan)
{
	struct local_pvt *p = newchan->tech_pvt;

	if (!p)
		return -1;

	tris_mutex_lock(&p->lock);

	if ((p->owner != oldchan) && (p->chan != oldchan)) {
		tris_log(LOG_WARNING, "Old channel wasn't %p but was %p/%p\n", oldchan, p->owner, p->chan);
		tris_mutex_unlock(&p->lock);
		return -1;
	}
	if (p->owner == oldchan)
		p->owner = newchan;
	else
		p->chan = newchan;
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int local_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = 0;
	struct tris_frame f = { TRIS_FRAME_CONTROL, };
	int isoutbound;

	if (!p)
		return -1;

	/* If this is an MOH hold or unhold, do it on the Local channel versus real channel */
	if (!tris_test_flag(p, LOCAL_MOH_PASSTHRU) && condition == TRIS_CONTROL_HOLD) {
		/*tris_moh_start(ast, data, NULL);*/
	} else if (!tris_test_flag(p, LOCAL_MOH_PASSTHRU) && condition == TRIS_CONTROL_UNHOLD) {
		/*tris_moh_stop(ast);*/
	} else {
		/* Queue up a frame representing the indication as a control frame */
		tris_mutex_lock(&p->lock);
		isoutbound = IS_OUTBOUND(ast, p);
		f.subclass = condition;
		f.data.ptr = (void*)data;
		f.datalen = datalen;
		if (!(res = local_queue_frame(p, isoutbound, &f, ast, 1)))
			tris_mutex_unlock(&p->lock);
	}

	return res;
}

static int local_digit_begin(struct tris_channel *ast, char digit)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct tris_frame f = { TRIS_FRAME_DTMF_BEGIN, };
	int isoutbound;

	if (!p)
		return -1;

	tris_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass = digit;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		tris_mutex_unlock(&p->lock);

	return res;
}

static int local_digit_end(struct tris_channel *ast, char digit, unsigned int duration)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct tris_frame f = { TRIS_FRAME_DTMF_END, };
	int isoutbound;

	if (!p)
		return -1;

	tris_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass = digit;
	f.len = duration;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		tris_mutex_unlock(&p->lock);

	return res;
}

static int local_sendtext(struct tris_channel *ast, const char *text)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct tris_frame f = { TRIS_FRAME_TEXT, };
	int isoutbound;

	if (!p)
		return -1;

	tris_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.data.ptr = (char *) text;
	f.datalen = strlen(text) + 1;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		tris_mutex_unlock(&p->lock);
	return res;
}

static int local_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct tris_frame f = { TRIS_FRAME_HTML, };
	int isoutbound;

	if (!p)
		return -1;
	
	tris_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass = subclass;
	f.data.ptr = (char *)data;
	f.datalen = datalen;
	if (!(res = local_queue_frame(p, isoutbound, &f, ast, 0)))
		tris_mutex_unlock(&p->lock);
	return res;
}

/*! \brief Initiate new call, part of PBX interface 
 * 	dest is the dial string */
static int local_call(struct tris_channel *ast, char *dest, int timeout)
{
	struct local_pvt *p = ast->tech_pvt;
	int res;
	struct tris_var_t *varptr = NULL, *new;
	size_t len, namelen;

	if (!p)
		return -1;
	
	tris_mutex_lock(&p->lock);

	/*
	 * Note that cid_num and cid_name aren't passed in the tris_channel_alloc
	 * call, so it's done here instead.
	 */
	p->chan->cid.cid_dnid = tris_strdup(p->owner->cid.cid_dnid);
	p->chan->cid.cid_num = tris_strdup(p->owner->cid.cid_num);
	p->chan->cid.cid_name = tris_strdup(p->owner->cid.cid_name);
	p->chan->cid.cid_rdnis = tris_strdup(p->owner->cid.cid_rdnis);
	p->chan->cid.cid_ani = tris_strdup(p->owner->cid.cid_ani);
	p->chan->cid.cid_pres = p->owner->cid.cid_pres;
	p->chan->cid.cid_ani2 = p->owner->cid.cid_ani2;
	p->chan->cid.cid_ton = p->owner->cid.cid_ton;
	p->chan->cid.cid_tns = p->owner->cid.cid_tns;
	tris_string_field_set(p->chan, language, p->owner->language);
	tris_string_field_set(p->chan, accountcode, p->owner->accountcode);
	tris_string_field_set(p->chan, musicclass, p->owner->musicclass);
	tris_cdr_update(p->chan);
	p->chan->cdrflags = p->owner->cdrflags;

	if (!tris_exists_extension(NULL, p->chan->context, p->chan->exten, 1, p->owner->cid.cid_num)) {
		tris_log(LOG_NOTICE, "No such extension/context %s@%s while calling Local channel\n", p->chan->exten, p->chan->context);
		tris_mutex_unlock(&p->lock);
		return -1;
	}

	/* Make sure we inherit the ANSWERED_ELSEWHERE flag if it's set on the queue/dial call request in the dialplan */
	if (tris_test_flag(ast, TRIS_FLAG_ANSWERED_ELSEWHERE)) {
		tris_set_flag(p->chan, TRIS_FLAG_ANSWERED_ELSEWHERE);
	}

	/* copy the channel variables from the incoming channel to the outgoing channel */
	/* Note that due to certain assumptions, they MUST be in the same order */
	TRIS_LIST_TRAVERSE(&p->owner->varshead, varptr, entries) {
		namelen = strlen(varptr->name);
		len = sizeof(struct tris_var_t) + namelen + strlen(varptr->value) + 2;
		if ((new = tris_calloc(1, len))) {
			memcpy(new, varptr, len);
			new->value = &(new->name[0]) + namelen + 1;
			TRIS_LIST_INSERT_TAIL(&p->chan->varshead, new, entries);
		}
	}

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&p->chan->varshead, varptr, entries) {
		if (strstr(varptr->name, "SWITCHADDHEADER") && strstr(varptr->value, "Call-Info")) {
			TRIS_LIST_REMOVE_CURRENT(entries);
			tris_var_delete(varptr);
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	
	tris_channel_datastore_inherit(p->owner, p->chan);

	if (ast->appl && !strcmp(ast->appl, "AppQueue"))
		pbx_builtin_setvar_helper(p->chan, "RealApplication", "AppQueue");
	p->chan->referid = ast->referid;

	/* Start switch on sub channel */
	if (!(res = tris_pbx_start(p->chan)))
		tris_set_flag(p, LOCAL_LAUNCHED_PBX);

	tris_mutex_unlock(&p->lock);
	return res;
}

/*! \brief Hangup a call through the local proxy channel */
static int local_hangup(struct tris_channel *ast)
{
	struct local_pvt *p = ast->tech_pvt;
	int isoutbound;
	struct tris_frame f = { TRIS_FRAME_CONTROL, TRIS_CONTROL_HANGUP, .data.uint32 = ast->hangupcause };
	struct tris_channel *ochan = NULL;
	int glaredetect = 0, res = 0;

	if (!p)
		return -1;

	tris_mutex_lock(&p->lock);

	isoutbound = IS_OUTBOUND(ast, p);

	if (p->chan && tris_test_flag(ast, TRIS_FLAG_ANSWERED_ELSEWHERE)) {
		tris_set_flag(p->chan, TRIS_FLAG_ANSWERED_ELSEWHERE);
		tris_debug(2, "This local call has the ANSWERED_ELSEWHERE flag set.\n");
	}

	if (isoutbound) {
		const char *status = pbx_builtin_getvar_helper(p->chan, "DIALSTATUS");
		if ((status) && (p->owner)) {
			/* Deadlock avoidance */
			while (p->owner && tris_channel_trylock(p->owner)) {
				tris_mutex_unlock(&p->lock);
				if (ast) {
					tris_channel_unlock(ast);
				}
				usleep(1);
				if (ast) {
					tris_channel_lock(ast);
				}
				tris_mutex_lock(&p->lock);
			}
			if (p->owner) {
				pbx_builtin_setvar_helper(p->owner, "CHANLOCALSTATUS", status);
				tris_channel_unlock(p->owner);
			}
		}
		p->chan = NULL;
		tris_clear_flag(p, LOCAL_LAUNCHED_PBX);
		tris_module_user_remove(p->u_chan);
	} else {
		tris_module_user_remove(p->u_owner);
		while (p->chan && tris_channel_trylock(p->chan)) {
			DEADLOCK_AVOIDANCE(&p->lock);
		}
		p->owner = NULL;
		if (p->chan) {
			tris_queue_hangup(p->chan);
			tris_channel_unlock(p->chan);
		}
	}
	
	ast->tech_pvt = NULL;
	
	if (!p->owner && !p->chan) {
		/* Okay, done with the private part now, too. */
		glaredetect = tris_test_flag(p, LOCAL_GLARE_DETECT);
		/* If we have a queue holding, don't actually destroy p yet, but
		   let local_queue do it. */
		if (glaredetect)
			tris_set_flag(p, LOCAL_CANCEL_QUEUE);
		/* Remove from list */
		TRIS_LIST_LOCK(&locals);
		TRIS_LIST_REMOVE(&locals, p, list);
		TRIS_LIST_UNLOCK(&locals);
		tris_mutex_unlock(&p->lock);
		/* And destroy */
		if (!glaredetect) {
			p = local_pvt_destroy(p);
		}
		return 0;
	}
	if (p->chan && !tris_test_flag(p, LOCAL_LAUNCHED_PBX))
		/* Need to actually hangup since there is no PBX */
		ochan = p->chan;
	else
		res = local_queue_frame(p, isoutbound, &f, NULL, 1);
	if (!res)
		tris_mutex_unlock(&p->lock);
	if (ochan)
		tris_hangup(ochan);
	return 0;
}

/*! \brief Create a call structure */
static struct local_pvt *local_alloc(const char *data, int format)
{
	struct local_pvt *tmp = NULL;
	char *c = NULL, *opts = NULL;

	if (!(tmp = tris_calloc(1, sizeof(*tmp))))
		return NULL;

	/* Initialize private structure information */
	tris_mutex_init(&tmp->lock);
	tris_copy_string(tmp->exten, data, sizeof(tmp->exten));

	memcpy(&tmp->jb_conf, &g_jb_conf, sizeof(tmp->jb_conf));

	/* Look for options */
	if ((opts = strchr(tmp->exten, '/'))) {
		*opts++ = '\0';
		if (strchr(opts, 'n'))
			tris_set_flag(tmp, LOCAL_NO_OPTIMIZATION);
		if (strchr(opts, 'j')) {
			if (tris_test_flag(tmp, LOCAL_NO_OPTIMIZATION))
				tris_set_flag(&tmp->jb_conf, TRIS_JB_ENABLED);
			else {
				tris_log(LOG_ERROR, "You must use the 'n' option for chan_local "
					"to use the 'j' option to enable the jitterbuffer\n");
			}
		}
		if (strchr(opts, 'b')) {
			tris_set_flag(tmp, LOCAL_BRIDGE);
		}
		if (strchr(opts, 'm')) {
			tris_set_flag(tmp, LOCAL_MOH_PASSTHRU);
		}
	}

	/* Look for a context */
	if ((c = strchr(tmp->exten, '@')))
		*c++ = '\0';

	tris_copy_string(tmp->context, c ? c : "default", sizeof(tmp->context));

	tmp->reqformat = format;

#if 0
	/* We can't do this check here, because we don't know the CallerID yet, and
	 * the CallerID could potentially affect what step is actually taken (or
	 * even if that step exists). */
	if (!tris_exists_extension(NULL, tmp->context, tmp->exten, 1, NULL)) {
		tris_log(LOG_NOTICE, "No such extension/context %s@%s creating local channel\n", tmp->exten, tmp->context);
		tmp = local_pvt_destroy(tmp);
	} else {
#endif
		/* Add to list */
		TRIS_LIST_LOCK(&locals);
		TRIS_LIST_INSERT_HEAD(&locals, tmp, list);
		TRIS_LIST_UNLOCK(&locals);
#if 0
	}
#endif
	
	return tmp;
}

/*! \brief Start new local channel */
static struct tris_channel *local_new(struct local_pvt *p, int state)
{
	struct tris_channel *tmp = NULL, *tmp2 = NULL;
	int randnum = tris_random() & 0xffff, fmt = 0;
	const char *t;
	int ama;

	/* Allocate two new Trismedia channels */
	/* safe accountcode */
	if (p->owner && p->owner->accountcode)
		t = p->owner->accountcode;
	else
		t = "";

	if (p->owner)
		ama = p->owner->amaflags;
	else
		ama = 0;
	if (!(tmp = tris_channel_alloc(1, state, 0, 0, t, p->exten, p->context, ama, "Local/%s@%s-%04x;1", p->exten, p->context, randnum)) 
			|| !(tmp2 = tris_channel_alloc(1, TRIS_STATE_RING, 0, 0, t, p->exten, p->context, ama, "Local/%s@%s-%04x;2", p->exten, p->context, randnum))) {
		if (tmp)
			tris_channel_free(tmp);
		if (tmp2)
			tris_channel_free(tmp2);
		tris_log(LOG_WARNING, "Unable to allocate channel structure(s)\n");
		return NULL;
	} 

	tmp2->tech = tmp->tech = &local_tech;

	tmp->nativeformats = p->reqformat;
	tmp2->nativeformats = p->reqformat;

	/* Determine our read/write format and set it on each channel */
	fmt = tris_best_codec(p->reqformat);
	tmp->writeformat = fmt;
	tmp2->writeformat = fmt;
	tmp->rawwriteformat = fmt;
	tmp2->rawwriteformat = fmt;
	tmp->readformat = fmt;
	tmp2->readformat = fmt;
	tmp->rawreadformat = fmt;
	tmp2->rawreadformat = fmt;

	tmp->tech_pvt = p;
	tmp2->tech_pvt = p;

	p->owner = tmp;
	p->chan = tmp2;
	p->u_owner = tris_module_user_add(p->owner);
	p->u_chan = tris_module_user_add(p->chan);

	tris_copy_string(tmp->context, p->context, sizeof(tmp->context));
	tris_copy_string(tmp2->context, p->context, sizeof(tmp2->context));
	tris_copy_string(tmp2->exten, p->exten, sizeof(tmp->exten));
	tmp->priority = 1;
	tmp2->priority = 1;

	tris_jb_configure(tmp, &p->jb_conf);

	return tmp;
}

/*! \brief Part of PBX interface */
static struct tris_channel *local_request(const char *type, int format, void *data, int *cause, struct tris_channel* src)
{
	struct local_pvt *p = NULL;
	struct tris_channel *chan = NULL;

	/* Allocate a new private structure and then Trismedia channel */
	if ((p = local_alloc(data, format))) {
		if (!(chan = local_new(p, TRIS_STATE_DOWN))) {
			TRIS_LIST_LOCK(&locals);
			TRIS_LIST_REMOVE(&locals, p, list);
			TRIS_LIST_UNLOCK(&locals);
			p = local_pvt_destroy(p);
		}
	}

	return chan;
}

/*! \brief CLI command "local show channels" */
static char *locals_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct local_pvt *p = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "local show channels";
		e->usage =
			"Usage: local show channels\n"
			"       Provides summary information on active local proxy channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	TRIS_LIST_LOCK(&locals);
	if (!TRIS_LIST_EMPTY(&locals)) {
		TRIS_LIST_TRAVERSE(&locals, p, list) {
			tris_mutex_lock(&p->lock);
			tris_cli(a->fd, "%s -- %s@%s\n", p->owner ? p->owner->name : "<unowned>", p->exten, p->context);
			tris_mutex_unlock(&p->lock);
		}
	} else
		tris_cli(a->fd, "No local channels in use\n");
	TRIS_LIST_UNLOCK(&locals);

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_local[] = {
	TRIS_CLI_DEFINE(locals_show, "List status of local channels"),
};

/*! \brief Load module into PBX, register channel */
static int load_module(void)
{
	/* Make sure we can register our channel type */
	if (tris_channel_register(&local_tech)) {
		tris_log(LOG_ERROR, "Unable to register channel class 'Local'\n");
		return TRIS_MODULE_LOAD_FAILURE;
	}
	tris_cli_register_multiple(cli_local, sizeof(cli_local) / sizeof(struct tris_cli_entry));
	return TRIS_MODULE_LOAD_SUCCESS;
}

/*! \brief Unload the local proxy channel from Trismedia */
static int unload_module(void)
{
	struct local_pvt *p = NULL;

	/* First, take us out of the channel loop */
	tris_cli_unregister_multiple(cli_local, sizeof(cli_local) / sizeof(struct tris_cli_entry));
	tris_channel_unregister(&local_tech);
	if (!TRIS_LIST_LOCK(&locals)) {
		/* Hangup all interfaces if they have an owner */
		TRIS_LIST_TRAVERSE(&locals, p, list) {
			if (p->owner)
				tris_softhangup(p->owner, TRIS_SOFTHANGUP_APPUNLOAD);
		}
		TRIS_LIST_UNLOCK(&locals);
	} else {
		tris_log(LOG_WARNING, "Unable to lock the monitor\n");
		return -1;
	}		
	return 0;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Local Proxy Channel (Note: used internally by other modules)");
