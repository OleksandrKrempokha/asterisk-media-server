/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
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
 * \brief Scheduler Routines (from cheops-NG)
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 244555 $")

#ifdef DEBUG_SCHEDULER
#define DEBUG(a) do { \
	if (option_debug) \
		DEBUG_M(a) \
	} while (0)
#else
#define DEBUG(a) 
#endif

#include <sys/time.h>

#include "trismedia/sched.h"
#include "trismedia/channel.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"
#include "trismedia/linkedlists.h"
#include "trismedia/dlinkedlists.h"
#include "trismedia/hashtab.h"
#include "trismedia/heap.h"

struct sched {
	TRIS_LIST_ENTRY(sched) list;
	int id;                       /*!< ID number of event */
	struct timeval when;          /*!< Absolute time event should take place */
	int resched;                  /*!< When to reschedule */
	int variable;                 /*!< Use return value from callback to reschedule */
	const void *data;             /*!< Data */
	tris_sched_cb callback;        /*!< Callback */
	ssize_t __heap_index;
};

struct sched_context {
	tris_mutex_t lock;
	unsigned int eventcnt;                  /*!< Number of events processed */
	unsigned int schedcnt;                  /*!< Number of outstanding schedule events */
	unsigned int highwater;					/*!< highest count so far */
	struct tris_hashtab *schedq_ht;             /*!< hash table for fast searching */
	struct tris_heap *sched_heap;

#ifdef SCHED_MAX_CACHE
	TRIS_LIST_HEAD_NOLOCK(, sched) schedc;   /*!< Cache of unused schedule structures and how many */
	unsigned int schedccnt;
#endif
};

struct tris_sched_thread {
	pthread_t thread;
	tris_mutex_t lock;
	tris_cond_t cond;
	struct sched_context *context;
	unsigned int stop:1;
};

static void *sched_run(void *data)
{
	struct tris_sched_thread *st = data;

	while (!st->stop) {
		int ms;
		struct timespec ts = {
			.tv_sec = 0,	
		};

		tris_mutex_lock(&st->lock);

		if (st->stop) {
			tris_mutex_unlock(&st->lock);
			return NULL;
		}

		ms = tris_sched_wait(st->context);

		if (ms == -1) {
			tris_cond_wait(&st->cond, &st->lock);
		} else {	
			struct timeval tv;
			tv = tris_tvadd(tris_tvnow(), tris_samp2tv(ms, 1000));
			ts.tv_sec = tv.tv_sec;
			ts.tv_nsec = tv.tv_usec * 1000;
			tris_cond_timedwait(&st->cond, &st->lock, &ts);
		}

		tris_mutex_unlock(&st->lock);

		if (st->stop) {
			return NULL;
		}

		tris_sched_runq(st->context);
	}

	return NULL;
}

void tris_sched_thread_poke(struct tris_sched_thread *st)
{
	tris_mutex_lock(&st->lock);
	tris_cond_signal(&st->cond);
	tris_mutex_unlock(&st->lock);
}

struct sched_context *tris_sched_thread_get_context(struct tris_sched_thread *st)
{
	return st->context;
}

struct tris_sched_thread *tris_sched_thread_destroy(struct tris_sched_thread *st)
{
	if (st->thread != TRIS_PTHREADT_NULL) {
		tris_mutex_lock(&st->lock);
		st->stop = 1;
		tris_cond_signal(&st->cond);
		tris_mutex_unlock(&st->lock);
		pthread_join(st->thread, NULL);
		st->thread = TRIS_PTHREADT_NULL;
	}

	tris_mutex_destroy(&st->lock);
	tris_cond_destroy(&st->cond);

	if (st->context) {
		sched_context_destroy(st->context);
		st->context = NULL;
	}

	tris_free(st);

	return NULL;
}

struct tris_sched_thread *tris_sched_thread_create(void)
{
	struct tris_sched_thread *st;

	if (!(st = tris_calloc(1, sizeof(*st)))) {
		return NULL;
	}

	tris_mutex_init(&st->lock);
	tris_cond_init(&st->cond, NULL);

	st->thread = TRIS_PTHREADT_NULL;

	if (!(st->context = sched_context_create())) {
		tris_log(LOG_ERROR, "Failed to create scheduler\n");
		tris_sched_thread_destroy(st);
		return NULL;
	}
	
	if (tris_pthread_create_background(&st->thread, NULL, sched_run, st)) {
		tris_log(LOG_ERROR, "Failed to create scheduler thread\n");
		tris_sched_thread_destroy(st);
		return NULL;
	}

	return st;
}

int tris_sched_thread_add_variable(struct tris_sched_thread *st, int when, tris_sched_cb cb,
		const void *data, int variable)
{
	int res;

	tris_mutex_lock(&st->lock);
	res = tris_sched_add_variable(st->context, when, cb, data, variable);
	if (res != -1) {
		tris_cond_signal(&st->cond);
	}
	tris_mutex_unlock(&st->lock);

	return res;
}

int tris_sched_thread_add(struct tris_sched_thread *st, int when, tris_sched_cb cb,
		const void *data)
{
	int res;

	tris_mutex_lock(&st->lock);
	res = tris_sched_add(st->context, when, cb, data);
	if (res != -1) {
		tris_cond_signal(&st->cond);
	}
	tris_mutex_unlock(&st->lock);

	return res;
}

/* hash routines for sched */

static int sched_cmp(const void *a, const void *b)
{
	const struct sched *as = a;
	const struct sched *bs = b;
	return as->id != bs->id; /* return 0 on a match like strcmp would */
}

static unsigned int sched_hash(const void *obj)
{
	const struct sched *s = obj;
	unsigned int h = s->id;
	return h;
}

static int sched_time_cmp(void *a, void *b)
{
	return tris_tvcmp(((struct sched *) b)->when, ((struct sched *) a)->when);
}

struct sched_context *sched_context_create(void)
{
	struct sched_context *tmp;

	if (!(tmp = tris_calloc(1, sizeof(*tmp))))
		return NULL;

	tris_mutex_init(&tmp->lock);
	tmp->eventcnt = 1;

	tmp->schedq_ht = tris_hashtab_create(23, sched_cmp, tris_hashtab_resize_java, tris_hashtab_newsize_java, sched_hash, 1);

	if (!(tmp->sched_heap = tris_heap_create(8, sched_time_cmp,
			offsetof(struct sched, __heap_index)))) {
		sched_context_destroy(tmp);
		return NULL;
	}

	return tmp;
}

void sched_context_destroy(struct sched_context *con)
{
	struct sched *s;

	tris_mutex_lock(&con->lock);

#ifdef SCHED_MAX_CACHE
	/* Eliminate the cache */
	while ((s = TRIS_LIST_REMOVE_HEAD(&con->schedc, list)))
		tris_free(s);
#endif

	if (con->sched_heap) {
		while ((s = tris_heap_pop(con->sched_heap))) {
			tris_free(s);
		}
		tris_heap_destroy(con->sched_heap);
		con->sched_heap = NULL;
	}

	tris_hashtab_destroy(con->schedq_ht, NULL);
	con->schedq_ht = NULL;
	
	/* And the context */
	tris_mutex_unlock(&con->lock);
	tris_mutex_destroy(&con->lock);
	tris_free(con);
}

static struct sched *sched_alloc(struct sched_context *con)
{
	struct sched *tmp;

	/*
	 * We keep a small cache of schedule entries
	 * to minimize the number of necessary malloc()'s
	 */
#ifdef SCHED_MAX_CACHE
	if ((tmp = TRIS_LIST_REMOVE_HEAD(&con->schedc, list)))
		con->schedccnt--;
	else
#endif
		tmp = tris_calloc(1, sizeof(*tmp));

	return tmp;
}

static void sched_release(struct sched_context *con, struct sched *tmp)
{
	/*
	 * Add to the cache, or just free() if we
	 * already have too many cache entries
	 */

#ifdef SCHED_MAX_CACHE	 
	if (con->schedccnt < SCHED_MAX_CACHE) {
		TRIS_LIST_INSERT_HEAD(&con->schedc, tmp, list);
		con->schedccnt++;
	} else
#endif
		tris_free(tmp);
}

/*! \brief
 * Return the number of milliseconds 
 * until the next scheduled event
 */
int tris_sched_wait(struct sched_context *con)
{
	int ms;
	struct sched *s;

	DEBUG(tris_debug(1, "tris_sched_wait()\n"));

	tris_mutex_lock(&con->lock);
	if ((s = tris_heap_peek(con->sched_heap, 1))) {
		ms = tris_tvdiff_ms(s->when, tris_tvnow());
		if (ms < 0) {
			ms = 0;
		}
	} else {
		ms = -1;
	}
	tris_mutex_unlock(&con->lock);

	return ms;
}


/*! \brief
 * Take a sched structure and put it in the
 * queue, such that the soonest event is
 * first in the list. 
 */
static void schedule(struct sched_context *con, struct sched *s)
{
	tris_heap_push(con->sched_heap, s);

	if (!tris_hashtab_insert_safe(con->schedq_ht, s)) {
		tris_log(LOG_WARNING,"Schedule Queue entry %d is already in table!\n", s->id);
	}

	con->schedcnt++;

	if (con->schedcnt > con->highwater) {
		con->highwater = con->schedcnt;
	}
}

/*! \brief
 * given the last event *tv and the offset in milliseconds 'when',
 * computes the next value,
 */
static int sched_settime(struct timeval *t, int when)
{
	struct timeval now = tris_tvnow();

	/*tris_debug(1, "TV -> %lu,%lu\n", tv->tv_sec, tv->tv_usec);*/
	if (tris_tvzero(*t))	/* not supplied, default to now */
		*t = now;
	*t = tris_tvadd(*t, tris_samp2tv(when, 1000));
	if (tris_tvcmp(*t, now) < 0) {
		*t = now;
	}
	return 0;
}

int tris_sched_replace_variable(int old_id, struct sched_context *con, int when, tris_sched_cb callback, const void *data, int variable)
{
	/* 0 means the schedule item is new; do not delete */
	if (old_id > 0) {
		TRIS_SCHED_DEL(con, old_id);
	}
	return tris_sched_add_variable(con, when, callback, data, variable);
}

/*! \brief
 * Schedule callback(data) to happen when ms into the future
 */
int tris_sched_add_variable(struct sched_context *con, int when, tris_sched_cb callback, const void *data, int variable)
{
	struct sched *tmp;
	int res = -1;

	DEBUG(tris_debug(1, "tris_sched_add()\n"));

	tris_mutex_lock(&con->lock);
	if ((tmp = sched_alloc(con))) {
		tmp->id = con->eventcnt++;
		tmp->callback = callback;
		tmp->data = data;
		tmp->resched = when;
		tmp->variable = variable;
		tmp->when = tris_tv(0, 0);
		if (sched_settime(&tmp->when, when)) {
			sched_release(con, tmp);
		} else {
			schedule(con, tmp);
			res = tmp->id;
		}
	}
#ifdef DUMP_SCHEDULER
	/* Dump contents of the context while we have the lock so nothing gets screwed up by accident. */
	if (option_debug)
		tris_sched_dump(con);
#endif
	tris_mutex_unlock(&con->lock);

	return res;
}

int tris_sched_replace(int old_id, struct sched_context *con, int when, tris_sched_cb callback, const void *data)
{
	if (old_id > -1) {
		TRIS_SCHED_DEL(con, old_id);
	}
	return tris_sched_add(con, when, callback, data);
}

int tris_sched_add(struct sched_context *con, int when, tris_sched_cb callback, const void *data)
{
	return tris_sched_add_variable(con, when, callback, data, 0);
}

const void *tris_sched_find_data(struct sched_context *con, int id)
{
	struct sched tmp,*res;
	tmp.id = id;
	res = tris_hashtab_lookup(con->schedq_ht, &tmp);
	if (res)
		return res->data;
	return NULL;
}
	
/*! \brief
 * Delete the schedule entry with number
 * "id".  It's nearly impossible that there
 * would be two or more in the list with that
 * id.
 */
#ifndef TRIS_DEVMODE
int tris_sched_del(struct sched_context *con, int id)
#else
int _tris_sched_del(struct sched_context *con, int id, const char *file, int line, const char *function)
#endif
{
	struct sched *s, tmp = {
		.id = id,
	};

	DEBUG(tris_debug(1, "tris_sched_del(%d)\n", id));
	
	tris_mutex_lock(&con->lock);
	s = tris_hashtab_lookup(con->schedq_ht, &tmp);
	if (s) {
		if (!tris_heap_remove(con->sched_heap, s)) {
			tris_log(LOG_WARNING,"sched entry %d not in the sched heap?\n", s->id);
		}

		if (!tris_hashtab_remove_this_object(con->schedq_ht, s)) {
			tris_log(LOG_WARNING,"Found sched entry %d, then couldn't remove it?\n", s->id);
		}

		con->schedcnt--;

		sched_release(con, s);
	}
	
#ifdef DUMP_SCHEDULER
	/* Dump contents of the context while we have the lock so nothing gets screwed up by accident. */
	if (option_debug)
		tris_sched_dump(con);
#endif
	tris_mutex_unlock(&con->lock);

	if (!s) {
		tris_debug(1, "Attempted to delete nonexistent schedule entry %d!\n", id);
#ifndef TRIS_DEVMODE
		tris_assert(s != NULL);
#else
		_tris_assert(0, "s != NULL", file, line, function);
#endif
		return -1;
	}
	
	return 0;
}

void tris_sched_report(struct sched_context *con, struct tris_str **buf, struct tris_cb_names *cbnames)
{
	int i, x;
	struct sched *cur;
	int countlist[cbnames->numassocs + 1];
	size_t heap_size;
	
	memset(countlist, 0, sizeof(countlist));
	tris_str_set(buf, 0, " Highwater = %d\n schedcnt = %d\n", con->highwater, con->schedcnt);

	tris_mutex_lock(&con->lock);

	heap_size = tris_heap_size(con->sched_heap);
	for (x = 1; x <= heap_size; x++) {
		cur = tris_heap_peek(con->sched_heap, x);
		/* match the callback to the cblist */
		for (i = 0; i < cbnames->numassocs; i++) {
			if (cur->callback == cbnames->cblist[i]) {
				break;
			}
		}
		if (i < cbnames->numassocs) {
			countlist[i]++;
		} else {
			countlist[cbnames->numassocs]++;
		}
	}

	tris_mutex_unlock(&con->lock);

	for (i = 0; i < cbnames->numassocs; i++) {
		tris_str_append(buf, 0, "    %s : %d\n", cbnames->list[i], countlist[i]);
	}

	tris_str_append(buf, 0, "   <unknown> : %d\n", countlist[cbnames->numassocs]);
}
	
/*! \brief Dump the contents of the scheduler to LOG_DEBUG */
void tris_sched_dump(struct sched_context *con)
{
	struct sched *q;
	struct timeval when = tris_tvnow();
	int x;
	size_t heap_size;
#ifdef SCHED_MAX_CACHE
	tris_debug(1, "Trismedia Schedule Dump (%d in Q, %d Total, %d Cache, %d high-water)\n", con->schedcnt, con->eventcnt - 1, con->schedccnt, con->highwater);
#else
	tris_debug(1, "Trismedia Schedule Dump (%d in Q, %d Total, %d high-water)\n", con->schedcnt, con->eventcnt - 1, con->highwater);
#endif

	tris_debug(1, "=============================================================\n");
	tris_debug(1, "|ID    Callback          Data              Time  (sec:ms)   |\n");
	tris_debug(1, "+-----+-----------------+-----------------+-----------------+\n");
	tris_mutex_lock(&con->lock);
	heap_size = tris_heap_size(con->sched_heap);
	for (x = 1; x <= heap_size; x++) {
		struct timeval delta;
		q = tris_heap_peek(con->sched_heap, x);
		delta = tris_tvsub(q->when, when);
		tris_debug(1, "|%.4d | %-15p | %-15p | %.6ld : %.6ld |\n", 
			q->id,
			q->callback,
			q->data,
			(long)delta.tv_sec,
			(long int)delta.tv_usec);
	}
	tris_mutex_unlock(&con->lock);
	tris_debug(1, "=============================================================\n");
}

/*! \brief
 * Launch all events which need to be run at this time.
 */
int tris_sched_runq(struct sched_context *con)
{
	struct sched *current;
	struct timeval when;
	int numevents;
	int res;

	DEBUG(tris_debug(1, "tris_sched_runq()\n"));
		
	tris_mutex_lock(&con->lock);

	when = tris_tvadd(tris_tvnow(), tris_tv(0, 1000));
	for (numevents = 0; (current = tris_heap_peek(con->sched_heap, 1)); numevents++) {
		/* schedule all events which are going to expire within 1ms.
		 * We only care about millisecond accuracy anyway, so this will
		 * help us get more than one event at one time if they are very
		 * close together.
		 */
		if (tris_tvcmp(current->when, when) != -1) {
			break;
		}
		
		current = tris_heap_pop(con->sched_heap);

		if (!tris_hashtab_remove_this_object(con->schedq_ht, current)) {
			tris_log(LOG_ERROR,"Sched entry %d was in the schedq list but not in the hashtab???\n", current->id);
		}

		con->schedcnt--;

		/*
		 * At this point, the schedule queue is still intact.  We
		 * have removed the first event and the rest is still there,
		 * so it's permissible for the callback to add new events, but
		 * trying to delete itself won't work because it isn't in
		 * the schedule queue.  If that's what it wants to do, it 
		 * should return 0.
		 */
			
		tris_mutex_unlock(&con->lock);
		res = current->callback(current->data);
		tris_mutex_lock(&con->lock);
			
		if (res) {
		 	/*
			 * If they return non-zero, we should schedule them to be
			 * run again.
			 */
			if (sched_settime(&current->when, current->variable? res : current->resched)) {
				sched_release(con, current);
			} else {
				schedule(con, current);
			}
		} else {
			/* No longer needed, so release it */
			sched_release(con, current);
		}
	}

	tris_mutex_unlock(&con->lock);
	
	return numevents;
}

long tris_sched_when(struct sched_context *con,int id)
{
	struct sched *s, tmp;
	long secs = -1;
	DEBUG(tris_debug(1, "tris_sched_when()\n"));

	tris_mutex_lock(&con->lock);
	
	/* these next 2 lines replace a lookup loop */
	tmp.id = id;
	s = tris_hashtab_lookup(con->schedq_ht, &tmp);
	
	if (s) {
		struct timeval now = tris_tvnow();
		secs = s->when.tv_sec - now.tv_sec;
	}
	tris_mutex_unlock(&con->lock);
	
	return secs;
}
