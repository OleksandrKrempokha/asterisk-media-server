/* trisstubs.c - trismedia stubs
 * Copyright (C) 2007, Anders Baekgaard
 *
 * Author: Anders Baekgaard <ab@dicea.dk>
 * This work is included with chan_ss7, see copyright below.
 */

/*
 * This file is part of chan_ss7.
 *
 * chan_ss7 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * chan_ss7 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with chan_ss7; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>

#define TRIS_API_MODULE
#include <trismedia/autoconfig.h>
#include <trismedia/compiler.h>
#include <trismedia/linkedlists.h>
#include <trismedia/time.h>
#include <trismedia/options.h>
#include <trismedia/utils.h>
#include "trismedia/abstract_jb.h"
//#include <trismedia/strings.h>

#include "trisstubs.h"

int option_debug;
struct tris_cli_entry;

int option_verbose;
int option_debug;
struct tris_flags tris_options;


#undef pthread_mutex_init
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_mutex_t
#undef tris_calloc
#define tris_mutex_init(m) pthread_mutex_init(m,0)
#define tris_mutex_lock pthread_mutex_lock
#define tris_mutex_unlock pthread_mutex_unlock
#define tris_mutex_t pthread_mutex_t
#define tris_calloc calloc

//define DEBUG(x) {if (option_debug) x;}
#define DEBUG(x)


int tris_safe_system(const char *s);
void tris_register_file_version(const char *file, const char *version);
void tris_unregister_file_version(const char *file);
void tris_cli_register_multiple(struct tris_cli_entry *e, int len);
void tris_cli(int fd, char *fmt, ...);

#if (defined(MALLOC_DEBUG) && !defined(STANDALONE))
int __tris_debug_str_helper(struct tris_str **buf, ssize_t max_len,
	int append, const char *fmt, va_list ap, const char *file, int lineno, const char *function)
#else
int __tris_str_helper(struct tris_str **buf, ssize_t max_len,
	int append, const char *fmt, va_list ap)
#endif
{
	int res, need;
	int offset = (append && (*buf)->__TRIS_STR_LEN) ? (*buf)->__TRIS_STR_USED : 0;
	va_list aq;

	do {
		if (max_len < 0) {
			max_len = (*buf)->__TRIS_STR_LEN;	/* don't exceed the allocated space */
		}
		/*
		 * Ask vsnprintf how much space we need. Remember that vsnprintf
		 * does not count the final <code>'\0'</code> so we must add 1.
		 */
		va_copy(aq, ap);
		res = vsnprintf((*buf)->__TRIS_STR_STR + offset, (*buf)->__TRIS_STR_LEN - offset, fmt, aq);

		need = res + offset + 1;
		/*
		 * If there is not enough space and we are below the max length,
		 * reallocate the buffer and return a message telling to retry.
		 */
		if (need > (*buf)->__TRIS_STR_LEN && (max_len == 0 || (*buf)->__TRIS_STR_LEN < max_len) ) {
			if (max_len && max_len < need) {	/* truncate as needed */
				need = max_len;
			} else if (max_len == 0) {	/* if unbounded, give more room for next time */
				need += 16 + need / 4;
			}
			if (0) {	/* debugging */
				tris_verbose_ss7("extend from %d to %d\n", (int)(*buf)->__TRIS_STR_LEN, need);
			}
			if (
#if (defined(MALLOC_DEBUG) && !defined(STANDALONE))
					_tris_str_make_space(buf, need, file, lineno, function)
#else
					tris_str_make_space(buf, need)
#endif
				) {
				tris_verbose_ss7("failed to extend from %d to %d\n", (int)(*buf)->__TRIS_STR_LEN, need);
				va_end(aq);
				return TRIS_DYNSTR_BUILD_FAILED;
			}
			(*buf)->__TRIS_STR_STR[offset] = '\0';	/* Truncate the partial write. */

			/* Restart va_copy before calling vsnprintf() again. */
			va_end(aq);
			continue;
		}
		va_end(aq);
		break;
	} while (1);
	/* update space used, keep in mind the truncation */
	(*buf)->__TRIS_STR_USED = (res + offset > (*buf)->__TRIS_STR_LEN) ? (*buf)->__TRIS_STR_LEN - 1 : res + offset;

	return res;
}


char *__tris_str_helper2(struct tris_str **buf, ssize_t maxlen, const char *src, size_t maxsrc, int append, int escapecommas)
{
	int dynamic = 0;
	char *ptr = append ? &((*buf)->__TRIS_STR_STR[(*buf)->__TRIS_STR_USED]) : (*buf)->__TRIS_STR_STR;

	if (maxlen < 1) {
		if (maxlen == 0) {
			dynamic = 1;
		}
		maxlen = (*buf)->__TRIS_STR_LEN;
	}

	while (*src && maxsrc && maxlen && (!escapecommas || (maxlen - 1))) {
		if (escapecommas && (*src == '\\' || *src == ',')) {
			*ptr++ = '\\';
			maxlen--;
			(*buf)->__TRIS_STR_USED++;
		}
		*ptr++ = *src++;
		maxsrc--;
		maxlen--;
		(*buf)->__TRIS_STR_USED++;

		if ((ptr >= (*buf)->__TRIS_STR_STR + (*buf)->__TRIS_STR_LEN - 3) ||
			(dynamic && (!maxlen || (escapecommas && !(maxlen - 1))))) {
			char *oldbase = (*buf)->__TRIS_STR_STR;
			size_t old = (*buf)->__TRIS_STR_LEN;
			if (tris_str_make_space(buf, (*buf)->__TRIS_STR_LEN * 2)) {
				/* If the buffer can't be extended, end it. */
				break;
			}
			/* What we extended the buffer by */
			maxlen = old;

			ptr += (*buf)->__TRIS_STR_STR - oldbase;
		}
	}
	if (__builtin_expect(!maxlen, 0)) {
		ptr--;
	}
	*ptr = '\0';
	return (*buf)->__TRIS_STR_STR;
}


int tris_safe_system(const char *s)
{
  return -1;
}

void tris_register_file_version(const char *file, const char *version)
{
}

void tris_unregister_file_version(const char *file)
{
}

void tris_cli_register_multiple(struct tris_cli_entry *e, int len)
{
}

void tris_cli(int fd, char *fmt, ...)
{
}

void tris_log(int level, const char *file, int line, const char *function, const char *fmt, ...)
{
  va_list ap;
  char *l;
  char buff[1024];

  if ((level == __LOG_DEBUG) && !option_debug)
    return;
  switch (level) {
  case __LOG_DEBUG: l= "DEBUG"; break;
  case __LOG_EVENT: l= "EVENT"; break;
  case __LOG_NOTICE: l= "NOTICE"; break;
  case __LOG_WARNING: l= "WARNING"; break;
  case __LOG_ERROR: l= "ERROR"; break;
  default: l = "unknown";
  }
  sprintf(buff, "[%s] %s:%d %s %s", l, file, line, function, fmt);
  va_start(ap, fmt);
  vprintf(buff, ap);
  fflush(stdout);
}

void tris_verbose_ss7(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  fflush(stdout);
}

#undef  TRIS_LIST_INSERT_BEFORE_CURRENT
#ifndef TRIS_LIST_INSERT_BEFORE_CURRENT
/* Trismedia 1.2.x */
#define TRIS_LIST_INSERT_BEFORE_CURRENT(head, elm, field) do {		\
	if (__list_prev) {						\
		(elm)->field.next = __list_prev->field.next;		\
		__list_prev->field.next = elm;				\
	} else {							\
		(elm)->field.next = (head)->first;			\
		(head)->first = (elm);					\
	}								\
	__new_prev = (elm);						\
} while (0)
#endif

struct sched {
	TRIS_LIST_ENTRY(sched) list;
	int id;                       /*!< ID number of event */
	struct timeval when;          /*!< Absolute time event should take place */
	int resched;                  /*!< When to reschedule */
	int variable;                 /*!< Use return value from callback to reschedule */
	void *data;                   /*!< Data */
	tris_sched_cb callback;        /*!< Callback */
};

struct sched_context {
	tris_mutex_t lock;
	unsigned int eventcnt;                  /*!< Number of events processed */
	unsigned int schedcnt;                  /*!< Number of outstanding schedule events */
	TRIS_LIST_HEAD_NOLOCK(, sched) schedq;   /*!< Schedule entry and main queue */

#ifdef SCHED_MAX_CACHE
	TRIS_LIST_HEAD_NOLOCK(, sched) schedc;   /*!< Cache of unused schedule structures and how many */
	unsigned int schedccnt;
#endif
};

#define ONE_MILLION	1000000
static struct timeval tvfix(struct timeval a)
{
	if (a.tv_usec >= ONE_MILLION) {
		tris_log(LOG_WARNING, "warning too large timestamp %ld.%ld\n",
			a.tv_sec, (long int) a.tv_usec);
		a.tv_sec += a.tv_usec / ONE_MILLION;
		a.tv_usec %= ONE_MILLION;
	} else if (a.tv_usec < 0) {
		tris_log(LOG_WARNING, "warning negative timestamp %ld.%ld\n",
			a.tv_sec, (long int) a.tv_usec);
		a.tv_usec = 0;
	}
	return a;
}

struct timeval tris_tvadd(struct timeval a, struct timeval b)
{
	/* consistency checks to guarantee usec in 0..999999 */
	a = tvfix(a);
	b = tvfix(b);
	a.tv_sec += b.tv_sec;
	a.tv_usec += b.tv_usec;
	if (a.tv_usec >= ONE_MILLION) {
		a.tv_sec++;
		a.tv_usec -= ONE_MILLION;
	}
	return a;
}

struct sched_context *mtp_sched_context_create(void)
{
	struct sched_context *tmp;

	if (!(tmp = tris_calloc(1, sizeof(*tmp))))
		return NULL;

	tris_mutex_init(&tmp->lock);
	tmp->eventcnt = 1;
	
	return tmp;
}

void mtp_sched_context_destroy(struct sched_context *con)
{
	struct sched *s;

	tris_mutex_lock(&con->lock);

#ifdef SCHED_MAX_CACHE
	/* Eliminate the cache */
	while ((s = TRIS_LIST_REMOVE_HEAD(&con->schedc, list)))
		free(s);
#endif

	/* And the queue */
	while ((s = TRIS_LIST_REMOVE_HEAD(&con->schedq, list)))
		free(s);
	
	/* And the context */
	tris_mutex_unlock(&con->lock);
	tris_mutex_destroy(&con->lock);
	free(con);
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
		free(tmp);
}

int mtp_sched_wait(struct sched_context *con)
{
	int ms;

	DEBUG(tris_log(LOG_DEBUG, "tris_sched_wait()\n"));

	tris_mutex_lock(&con->lock);
	if (TRIS_LIST_EMPTY(&con->schedq)) {
		ms = -1;
	} else {
		ms = tris_tvdiff_ms(TRIS_LIST_FIRST(&con->schedq)->when, tris_tvnow());
		if (ms < 0)
			ms = 0;
	}
	tris_mutex_unlock(&con->lock);

	return ms;
}


static void schedule(struct sched_context *con, struct sched *s)
{
	 
	struct sched *cur = NULL;
	
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&con->schedq, cur, list) {
		if (tris_tvcmp(s->when, cur->when) == -1) {
			TRIS_LIST_INSERT_BEFORE_CURRENT(&con->schedq, s, list);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END
	if (!cur)
		TRIS_LIST_INSERT_TAIL(&con->schedq, s, list);
	
	con->schedcnt++;
}

static int sched_settime(struct timeval *tv, int when)
{
	struct timeval now = tris_tvnow();

	/*tris_log(LOG_DEBUG, "TV -> %lu,%lu\n", tv->tv_sec, tv->tv_usec);*/
	if (tris_tvzero(*tv))	/* not supplied, default to now */
		*tv = now;
	*tv = tris_tvadd(*tv, tris_samp2tv(when, 1000));
	if (tris_tvcmp(*tv, now) < 0) {
		tris_log(LOG_DEBUG, "Request to schedule in the ptris?!?!\n");
		*tv = now;
	}
	return 0;
}

static int tris_sched_add_variable_ss7(struct sched_context *con, int when, tris_sched_cb callback, void *data, int variable)
{
	struct sched *tmp;
	int res = -1;
	DEBUG(tris_log(LOG_DEBUG, "tris_sched_add()\n"));
	if (!when) {
		tris_log(LOG_NOTICE, "Scheduled event in 0 ms?\n");
		return -1;
	}
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


int mtp_sched_add(struct sched_context *con, int when, tris_sched_cb callback, void *data)
{
	return tris_sched_add_variable_ss7(con, when, callback, data, 0);
}

//kyz added
#define TRIS_LIST_REMOVE_CURRENT_ss7(head, field)						\
	__new_prev = __list_prev;							\
	if (__list_prev)								\
		__list_prev->field.next = __list_next;					\
	else										\
		(head)->first = __list_next;						\
	if (!__list_next)								\
		(head)->last = __list_prev;
		

int mtp_sched_del(struct sched_context *con, int id)
{
	struct sched *s;

	DEBUG(tris_log(LOG_DEBUG, "tris_sched_del()\n"));
	
	tris_mutex_lock(&con->lock);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&con->schedq, s, list) {
		if (s->id == id) {
			TRIS_LIST_REMOVE_CURRENT_ss7(&con->schedq, list);
			con->schedcnt--;
			sched_release(con, s);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END

#ifdef DUMP_SCHEDULER
	/* Dump contents of the context while we have the lock so nothing gets screwed up by accident. */
	if (option_debug)
		tris_sched_dump(con);
#endif
	tris_mutex_unlock(&con->lock);

	if (!s) {
		if (option_debug)
			tris_log(LOG_DEBUG, "Attempted to delete nonexistent schedule entry %d!\n", id);
#ifdef DO_CRASH
		CRASH;
#endif
		return -1;
	}
	
	return 0;
}

int mtp_sched_runq(struct sched_context *con)
{
	struct sched *current;
	struct timeval tv;
	int numevents;
	int res;

	DEBUG(tris_log(LOG_DEBUG, "tris_sched_runq()\n"));
		
	tris_mutex_lock(&con->lock);

	for (numevents = 0; !TRIS_LIST_EMPTY(&con->schedq); numevents++) {
		/* schedule all events which are going to expire within 1ms.
		 * We only care about millisecond accuracy anyway, so this will
		 * help us get more than one event at one time if they are very
		 * close together.
		 */
		tv = tris_tvadd(tris_tvnow(), tris_tv(0, 1000));
		if (tris_tvcmp(TRIS_LIST_FIRST(&con->schedq)->when, tv) != -1)
			break;
		
		current = TRIS_LIST_REMOVE_HEAD(&con->schedq, list);
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
			} else
				schedule(con, current);
		} else {
			/* No longer needed, so release it */
		 	sched_release(con, current);
		}
	}

	tris_mutex_unlock(&con->lock);
	
	return numevents;
}


int tris_jb_read_conf_ss7(struct tris_jb_conf *conf, char *varname, char *value)
{
  return 0;
}
