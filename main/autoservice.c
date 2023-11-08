/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Russell Bryant <russell@digium.com>
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
 * \brief Automatic channel service routines
 *
 * \author Mark Spencer <markster@digium.com> 
 * \author Russell Bryant <russell@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 223490 $")

#include <sys/time.h>
#include <signal.h>

#include "trismedia/_private.h" /* prototype for tris_autoservice_init() */

#include "trismedia/pbx.h"
#include "trismedia/frame.h"
#include "trismedia/sched.h"
#include "trismedia/channel.h"
#include "trismedia/file.h"
#include "trismedia/translate.h"
#include "trismedia/manager.h"
#include "trismedia/chanvars.h"
#include "trismedia/linkedlists.h"
#include "trismedia/indications.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"

#define MAX_AUTOMONS 1500

struct asent {
	struct tris_channel *chan;
	/*! This gets incremented each time autoservice gets started on the same
	 *  channel.  It will ensure that it doesn't actually get stopped until 
	 *  it gets stopped for the last time. */
	unsigned int use_count;
	unsigned int orig_end_dtmf_flag:1;
	/*! Frames go on at the head of deferred_frames, so we have the frames
	 *  from newest to oldest.  As we put them at the head of the readq, we'll
	 *  end up with them in the right order for the channel's readq. */
	TRIS_LIST_HEAD_NOLOCK(, tris_frame) deferred_frames;
	TRIS_LIST_ENTRY(asent) list;
};

static TRIS_LIST_HEAD_STATIC(aslist, asent);
static tris_cond_t as_cond;

static pthread_t asthread = TRIS_PTHREADT_NULL;

static int as_chan_list_state;

static void *autoservice_run(void *ign)
{
	struct tris_frame hangup_frame = {
		.frametype = TRIS_FRAME_CONTROL,
		.subclass = TRIS_CONTROL_HANGUP,
	};

	for (;;) {
		struct tris_channel *mons[MAX_AUTOMONS];
		struct asent *ents[MAX_AUTOMONS];
		struct tris_channel *chan;
		struct asent *as;
		int i, x = 0, ms = 50;
		struct tris_frame *f = NULL;
		struct tris_frame *defer_frame = NULL;

		TRIS_LIST_LOCK(&aslist);

		/* At this point, we know that no channels that have been removed are going
		 * to get used again. */
		as_chan_list_state++;

		if (TRIS_LIST_EMPTY(&aslist)) {
			tris_cond_wait(&as_cond, &aslist.lock);
		}

		TRIS_LIST_TRAVERSE(&aslist, as, list) {
			if (!tris_check_hangup(as->chan)) {
				if (x < MAX_AUTOMONS) {
					ents[x] = as;
					mons[x++] = as->chan;
				} else {
					tris_log(LOG_WARNING, "Exceeded maximum number of automatic monitoring events.  Fix autoservice.c\n");
				}
			}
		}

		TRIS_LIST_UNLOCK(&aslist);

		if (!x) {
			usleep(10);
			continue;
		}

		chan = tris_waitfor_n(mons, x, &ms);
		if (!chan) {
			continue;
		}

		f = tris_read(chan);

		if (!f) {
			/* No frame means the channel has been hung up.
			 * A hangup frame needs to be queued here as tris_waitfor() may
			 * never return again for the condition to be detected outside
			 * of autoservice.  So, we'll leave a HANGUP queued up so the
			 * thread in charge of this channel will know. */

			defer_frame = &hangup_frame;
		} else {

			/* Do not add a default entry in this switch statement.  Each new
			 * frame type should be addressed directly as to whether it should
			 * be queued up or not. */

			switch (f->frametype) {
			/* Save these frames */
			case TRIS_FRAME_DTMF_END:
			case TRIS_FRAME_CONTROL:
			case TRIS_FRAME_TEXT:
			case TRIS_FRAME_IMAGE:
			case TRIS_FRAME_HTML:
				defer_frame = f;
				break;

			/* Throw these frames away */
			case TRIS_FRAME_DTMF_BEGIN:
			case TRIS_FRAME_VOICE:
			case TRIS_FRAME_VIDEO:
			case TRIS_FRAME_NULL:
			case TRIS_FRAME_IAX:
			case TRIS_FRAME_CNG:
			case TRIS_FRAME_MODEM:
			case TRIS_FRAME_FILE:
			case TRIS_FRAME_DESKTOP:
			case TRIS_FRAME_CHAT:
				break;
			}
		}

		if (defer_frame) {
			for (i = 0; i < x; i++) {
				struct tris_frame *dup_f;
				
				if (mons[i] != chan) {
					continue;
				}
				
				if (defer_frame != f) {
					if ((dup_f = tris_frdup(defer_frame))) {
						TRIS_LIST_INSERT_HEAD(&ents[i]->deferred_frames, dup_f, frame_list);
					}
				} else {
					if ((dup_f = tris_frisolate(defer_frame))) {
						if (dup_f != defer_frame) {
							tris_frfree(defer_frame);
						}
						TRIS_LIST_INSERT_HEAD(&ents[i]->deferred_frames, dup_f, frame_list);
					}
				}
				
				break;
			}
		} else if (f) {
			tris_frfree(f);
		}
	}

	asthread = TRIS_PTHREADT_NULL;

	return NULL;
}

int tris_autoservice_start(struct tris_channel *chan)
{
	int res = 0;
	struct asent *as;

	TRIS_LIST_LOCK(&aslist);
	TRIS_LIST_TRAVERSE(&aslist, as, list) {
		if (as->chan == chan) {
			as->use_count++;
			break;
		}
	}
	TRIS_LIST_UNLOCK(&aslist);

	if (as) {
		/* Entry exists, autoservice is already handling this channel */
		return 0;
	}

	if (!(as = tris_calloc(1, sizeof(*as))))
		return -1;
	
	/* New entry created */
	as->chan = chan;
	as->use_count = 1;

	tris_channel_lock(chan);
	as->orig_end_dtmf_flag = tris_test_flag(chan, TRIS_FLAG_END_DTMF_ONLY) ? 1 : 0;
	if (!as->orig_end_dtmf_flag)
		tris_set_flag(chan, TRIS_FLAG_END_DTMF_ONLY);
	tris_channel_unlock(chan);

	TRIS_LIST_LOCK(&aslist);

	if (TRIS_LIST_EMPTY(&aslist) && asthread != TRIS_PTHREADT_NULL) {
		tris_cond_signal(&as_cond);
	}

	TRIS_LIST_INSERT_HEAD(&aslist, as, list);

	if (asthread == TRIS_PTHREADT_NULL) { /* need start the thread */
		if (tris_pthread_create_background(&asthread, NULL, autoservice_run, NULL)) {
			tris_log(LOG_WARNING, "Unable to create autoservice thread :(\n");
			/* There will only be a single member in the list at this point,
			   the one we just added. */
			TRIS_LIST_REMOVE(&aslist, as, list);
			free(as);
			asthread = TRIS_PTHREADT_NULL;
			res = -1;
		}
	} else {
		pthread_kill(asthread, SIGURG);
	}

	TRIS_LIST_UNLOCK(&aslist);

	return res;
}

int tris_autoservice_stop(struct tris_channel *chan)
{
	int res = -1;
	struct asent *as, *removed = NULL;
	struct tris_frame *f;
	int chan_list_state;

	TRIS_LIST_LOCK(&aslist);

	/* Save the autoservice channel list state.  We _must_ verify that the channel
	 * list has been rebuilt before we return.  Because, after we return, the channel
	 * could get destroyed and we don't want our poor autoservice thread to step on
	 * it after its gone! */
	chan_list_state = as_chan_list_state;

	/* Find the entry, but do not free it because it still can be in the
	   autoservice thread array */
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&aslist, as, list) {	
		if (as->chan == chan) {
			as->use_count--;
			if (as->use_count < 1) {
				TRIS_LIST_REMOVE_CURRENT(list);
				removed = as;
			}
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	if (removed && asthread != TRIS_PTHREADT_NULL) {
		pthread_kill(asthread, SIGURG);
	}

	TRIS_LIST_UNLOCK(&aslist);

	if (!removed) {
		return 0;
	}

	/* Wait while autoservice thread rebuilds its list. */
	while (chan_list_state == as_chan_list_state) {
		usleep(1000);
	}

	/* Now autoservice thread should have no references to our entry
	   and we can safely destroy it */

	if (!chan->_softhangup) {
		res = 0;
	}

	if (!as->orig_end_dtmf_flag) {
		tris_clear_flag(chan, TRIS_FLAG_END_DTMF_ONLY);
	}

	tris_channel_lock(chan);
	while ((f = TRIS_LIST_REMOVE_HEAD(&as->deferred_frames, frame_list))) {
		tris_queue_frame_head(chan, f);
		tris_frfree(f);
	}
	tris_channel_unlock(chan);

	free(as);

	return res;
}

void tris_autoservice_init(void)
{
	tris_cond_init(&as_cond, NULL);
}
