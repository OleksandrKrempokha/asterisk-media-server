/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Tilghman Lesher
 *
 * Tilghman Lesher <func_lock_2007@the-tilghman.com>
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
 * \brief Dialplan mutexes
 *
 * \author Tilghman Lesher <func_lock_2007@the-tilghman.com>
 *
 * \ingroup functions
 * 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 232015 $")

#include <signal.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/linkedlists.h"
#include "trismedia/astobj2.h"
#include "trismedia/utils.h"

/*** DOCUMENTATION
	<function name="LOCK" language="en_US">
		<synopsis>
			Attempt to obtain a named mutex.
		</synopsis>
		<syntax>
			<parameter name="lockname" required="true" />
		</syntax>
		<description>
			<para>Attempts to grab a named lock exclusively, and prevents other channels from
			obtaining the same lock.  LOCK will wait for the lock to become available.
			Returns <literal>1</literal> if the lock was obtained or <literal>0</literal> on error.</para>
			<note><para>To avoid the possibility of a deadlock, LOCK will only attempt to
			obtain the lock for 3 seconds if the channel already has another lock.</para></note>
		</description>
	</function>
	<function name="TRYLOCK" language="en_US">
		<synopsis>
			Attempt to obtain a named mutex.
		</synopsis>
		<syntax>
			<parameter name="lockname" required="true" />
		</syntax>
		<description>
			<para>Attempts to grab a named lock exclusively, and prevents other channels
			from obtaining the same lock.  Returns <literal>1</literal> if the lock was 
			available or <literal>0</literal> otherwise.</para>
		</description>
	</function>
	<function name="UNLOCK" language="en_US">
		<synopsis>
			Unlocks a named mutex.
		</synopsis>
		<syntax>
			<parameter name="lockname" required="true" />
		</syntax>
		<description>
			<para>Unlocks a previously locked mutex. Returns <literal>1</literal> if the channel 
			had a lock or <literal>0</literal> otherwise.</para>
			<note><para>It is generally unnecessary to unlock in a hangup routine, as any locks 
			held are automatically freed when the channel is destroyed.</para></note>
		</description>
	</function>
 ***/



TRIS_LIST_HEAD_STATIC(locklist, lock_frame);

static void lock_free(void *data);
static void lock_fixup(void *data, struct tris_channel *oldchan, struct tris_channel *newchan);
static int unloading = 0;
static pthread_t broker_tid = TRIS_PTHREADT_NULL;

static struct tris_datastore_info lock_info = {
	.type = "MUTEX",
	.destroy = lock_free,
	.chan_fixup = lock_fixup,
};

struct lock_frame {
	TRIS_LIST_ENTRY(lock_frame) entries;
	tris_mutex_t mutex;
	tris_cond_t cond;
	/*! count is needed so if a recursive mutex exits early, we know how many times to unlock it. */
	unsigned int count;
	/*! Container of requesters for the named lock */
	struct ao2_container *requesters;
	/*! who owns us */
	struct tris_channel *owner;
	/*! name of the lock */
	char name[0];
};

struct channel_lock_frame {
	TRIS_LIST_ENTRY(channel_lock_frame) list;
	/*! Need to save channel pointer here, because during destruction, we won't have it. */
	struct tris_channel *channel;
	struct lock_frame *lock_frame;
};

static void lock_free(void *data)
{
	TRIS_LIST_HEAD(, channel_lock_frame) *oldlist = data;
	struct channel_lock_frame *clframe;
	TRIS_LIST_LOCK(oldlist);
	while ((clframe = TRIS_LIST_REMOVE_HEAD(oldlist, list))) {
		/* Only unlock if we own the lock */
		if (clframe->channel == clframe->lock_frame->owner) {
			clframe->lock_frame->count = 0;
			clframe->lock_frame->owner = NULL;
		}
		tris_free(clframe);
	}
	TRIS_LIST_UNLOCK(oldlist);
	TRIS_LIST_HEAD_DESTROY(oldlist);
	tris_free(oldlist);
}

static void lock_fixup(void *data, struct tris_channel *oldchan, struct tris_channel *newchan)
{
	struct tris_datastore *lock_store = tris_channel_datastore_find(oldchan, &lock_info, NULL);
	TRIS_LIST_HEAD(, channel_lock_frame) *list;
	struct channel_lock_frame *clframe = NULL;

	if (!lock_store) {
		return;
	}
	list = lock_store->data;

	TRIS_LIST_LOCK(list);
	TRIS_LIST_TRAVERSE(list, clframe, list) {
		if (clframe->lock_frame->owner == oldchan) {
			clframe->lock_frame->owner = newchan;
		}
		/* We don't move requesters, because the thread stack is different */
		clframe->channel = newchan;
	}
	TRIS_LIST_UNLOCK(list);
}

static void *lock_broker(void *unused)
{
	struct lock_frame *frame;
	struct timespec forever = { 1000000, 0 };
	for (;;) {
		int found_requester = 0;

		/* Test for cancel outside of the lock */
		pthread_testcancel();
		TRIS_LIST_LOCK(&locklist);

		TRIS_LIST_TRAVERSE(&locklist, frame, entries) {
			if (ao2_container_count(frame->requesters)) {
				found_requester++;
				tris_mutex_lock(&frame->mutex);
				if (!frame->owner) {
					tris_cond_signal(&frame->cond);
				}
				tris_mutex_unlock(&frame->mutex);
			}
		}

		TRIS_LIST_UNLOCK(&locklist);
		pthread_testcancel();

		/* If there are no requesters, then wait for a signal */
		if (!found_requester) {
			nanosleep(&forever, NULL);
		} else {
			sched_yield();
		}
	}
	/* Not reached */
	return NULL;
}

static int null_hash_cb(const void *obj, const int flags)
{
	return (int)(long) obj;
}

static int null_cmp_cb(void *obj, void *arg, int flags)
{
	return obj == arg ? CMP_MATCH : 0;
}

static int get_lock(struct tris_channel *chan, char *lockname, int try)
{
	struct tris_datastore *lock_store = tris_channel_datastore_find(chan, &lock_info, NULL);
	struct lock_frame *current;
	struct channel_lock_frame *clframe = NULL;
	TRIS_LIST_HEAD(, channel_lock_frame) *list;
	int res = 0, *link;
	struct timespec three_seconds = { .tv_sec = 3 };

	if (!lock_store) {
		tris_debug(1, "Channel %s has no lock datastore, so we're allocating one.\n", chan->name);
		lock_store = tris_datastore_alloc(&lock_info, NULL);
		if (!lock_store) {
			tris_log(LOG_ERROR, "Unable to allocate new datastore.  No locks will be obtained.\n");
			return -1;
		}

		list = tris_calloc(1, sizeof(*list));
		if (!list) {
			tris_log(LOG_ERROR, "Unable to allocate datastore list head.  %sLOCK will fail.\n", try ? "TRY" : "");
			tris_datastore_free(lock_store);
			return -1;
		}

		lock_store->data = list;
		TRIS_LIST_HEAD_INIT(list);
		tris_channel_datastore_add(chan, lock_store);
	} else
		list = lock_store->data;

	/* Lock already exists? */
	TRIS_LIST_LOCK(&locklist);
	TRIS_LIST_TRAVERSE(&locklist, current, entries) {
		if (strcmp(current->name, lockname) == 0) {
			break;
		}
	}

	if (!current) {
		if (unloading) {
			/* Don't bother */
			TRIS_LIST_UNLOCK(&locklist);
			return -1;
		}

		/* Create new lock entry */
		current = tris_calloc(1, sizeof(*current) + strlen(lockname) + 1);
		if (!current) {
			TRIS_LIST_UNLOCK(&locklist);
			return -1;
		}

		strcpy(current->name, lockname); /* SAFE */
		if ((res = tris_mutex_init(&current->mutex))) {
			tris_log(LOG_ERROR, "Unable to initialize mutex: %s\n", strerror(res));
			tris_free(current);
			TRIS_LIST_UNLOCK(&locklist);
			return -1;
		}
		if ((res = tris_cond_init(&current->cond, NULL))) {
			tris_log(LOG_ERROR, "Unable to initialize condition variable: %s\n", strerror(res));
			tris_mutex_destroy(&current->mutex);
			tris_free(current);
			TRIS_LIST_UNLOCK(&locklist);
			return -1;
		}
		if (!(current->requesters = ao2_container_alloc(7, null_hash_cb, null_cmp_cb))) {
			tris_mutex_destroy(&current->mutex);
			tris_cond_destroy(&current->cond);
			tris_free(current);
			TRIS_LIST_UNLOCK(&locklist);
			return -1;
		}
		TRIS_LIST_INSERT_TAIL(&locklist, current, entries);
	}
	TRIS_LIST_UNLOCK(&locklist);

	/* Found lock or created one - now find or create the corresponding link in the channel */
	TRIS_LIST_LOCK(list);
	TRIS_LIST_TRAVERSE(list, clframe, list) {
		if (clframe->lock_frame == current) {
			break;
		}
	}

	if (!clframe) {
		if (unloading) {
			/* Don't bother */
			TRIS_LIST_UNLOCK(list);
			return -1;
		}

		if (!(clframe = tris_calloc(1, sizeof(*clframe)))) {
			tris_log(LOG_ERROR, "Unable to allocate channel lock frame.  %sLOCK will fail.\n", try ? "TRY" : "");
			TRIS_LIST_UNLOCK(list);
			return -1;
		}

		clframe->lock_frame = current;
		clframe->channel = chan;
		TRIS_LIST_INSERT_TAIL(list, clframe, list);
	}
	TRIS_LIST_UNLOCK(list);

	/* If we already own the lock, then we're being called recursively.
	 * Keep track of how many times that is, because we need to unlock
	 * the same amount, before we'll release this one.
	 */
	if (current->owner == chan) {
		current->count++;
		return 0;
	}

	/* Link is just an empty flag, used to check whether more than one channel
	 * is contending for the lock. */
	if (!(link = ao2_alloc(sizeof(*link), NULL))) {
		return -1;
	}

	/* Okay, we have both frames, so now we need to try to lock.
	 *
	 * Locking order: always lock locklist first.  We need the
	 * locklist lock because the broker thread counts whether
	 * there are requesters with the locklist lock held, and we
	 * need to hold it, so that when we send our signal, below,
	 * to wake up the broker thread, it definitely will see that
	 * a requester exists at that point in time.  Otherwise, we
	 * could add to the requesters after it has already seen that
	 * that lock is unoccupied and wait forever for another signal.
	 */
	TRIS_LIST_LOCK(&locklist);
	tris_mutex_lock(&current->mutex);
	/* Add to requester list */
	ao2_link(current->requesters, link);
	pthread_kill(broker_tid, SIGURG);
	TRIS_LIST_UNLOCK(&locklist);

	if ((!current->owner) ||
		(!try && !(res = tris_cond_timedwait(&current->cond, &current->mutex, &three_seconds)))) {
		res = 0;
		current->owner = chan;
		current->count++;
	} else {
		res = -1;
	}
	/* Remove from requester list */
	ao2_unlink(current->requesters, link);
	ao2_ref(link, -1);
	tris_mutex_unlock(&current->mutex);

	return res;
}

static int unlock_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_datastore *lock_store = tris_channel_datastore_find(chan, &lock_info, NULL);
	struct channel_lock_frame *clframe;
	TRIS_LIST_HEAD(, channel_lock_frame) *list;

	if (!lock_store) {
		tris_log(LOG_WARNING, "No datastore for dialplan locks.  Nothing was ever locked!\n");
		tris_copy_string(buf, "0", len);
		return 0;
	}

	if (!(list = lock_store->data)) {
		tris_debug(1, "This should NEVER happen\n");
		tris_copy_string(buf, "0", len);
		return 0;
	}

	/* Find item in the channel list */
	TRIS_LIST_LOCK(list);
	TRIS_LIST_TRAVERSE(list, clframe, list) {
		if (clframe->lock_frame && clframe->lock_frame->owner == chan && strcmp(clframe->lock_frame->name, data) == 0) {
			break;
		}
	}
	/* We never destroy anything until channel destruction, which will never
	 * happen while this routine is executing, so we don't need to hold the
	 * lock beyond this point. */
	TRIS_LIST_UNLOCK(list);

	if (!clframe) {
		/* We didn't have this lock in the first place */
		tris_copy_string(buf, "0", len);
		return 0;
	}

	if (--clframe->lock_frame->count == 0) {
		clframe->lock_frame->owner = NULL;
	}

	tris_copy_string(buf, "1", len);
	return 0;
}

static int lock_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	if (chan)
		tris_autoservice_start(chan);

	tris_copy_string(buf, get_lock(chan, data, 0) ? "0" : "1", len);

	if (chan)
		tris_autoservice_stop(chan);

	return 0;
}

static int trylock_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	if (chan)
		tris_autoservice_start(chan);

	tris_copy_string(buf, get_lock(chan, data, 1) ? "0" : "1", len);

	if (chan)
		tris_autoservice_stop(chan);

	return 0;
}

static struct tris_custom_function lock_function = {
	.name = "LOCK",
	.read = lock_read,
};

static struct tris_custom_function trylock_function = {
	.name = "TRYLOCK",
	.read = trylock_read,
};

static struct tris_custom_function unlock_function = {
	.name = "UNLOCK",
	.read = unlock_read,
};

static int unload_module(void)
{
	struct lock_frame *current;

	/* Module flag */
	unloading = 1;

	TRIS_LIST_LOCK(&locklist);
	while ((current = TRIS_LIST_REMOVE_HEAD(&locklist, entries))) {
		/* If any locks are currently in use, then we cannot unload this module */
		if (current->owner || ao2_container_count(current->requesters)) {
			/* Put it back */
			TRIS_LIST_INSERT_HEAD(&locklist, current, entries);
			TRIS_LIST_UNLOCK(&locklist);
			unloading = 0;
			return -1;
		}
		tris_mutex_destroy(&current->mutex);
		ao2_ref(current->requesters, -1);
		tris_free(current);
	}

	/* No locks left, unregister functions */
	tris_custom_function_unregister(&lock_function);
	tris_custom_function_unregister(&trylock_function);
	tris_custom_function_unregister(&unlock_function);

	pthread_cancel(broker_tid);
	pthread_kill(broker_tid, SIGURG);
	pthread_join(broker_tid, NULL);

	TRIS_LIST_UNLOCK(&locklist);

	return 0;
}

static int load_module(void)
{
	int res = tris_custom_function_register(&lock_function);
	res |= tris_custom_function_register(&trylock_function);
	res |= tris_custom_function_register(&unlock_function);
	tris_pthread_create_background(&broker_tid, NULL, lock_broker, NULL);
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Dialplan mutexes");
