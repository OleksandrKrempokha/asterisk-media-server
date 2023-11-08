/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007-2008, Dwayne M. Hubbard 
 *
 * Dwayne M. Hubbard <dhubbard@digium.com>
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
 * \brief Maintain a container of uniquely-named taskprocessor threads that can be shared across modules.
 *
 * \author Dwayne Hubbard <dhubbard@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 185947 $")

#include <signal.h>
#include <sys/time.h>

#include "trismedia/_private.h"
#include "trismedia/module.h"
#include "trismedia/time.h"
#include "trismedia/astobj2.h"
#include "trismedia/cli.h"
#include "trismedia/taskprocessor.h"


/*! \brief tps_task structure is queued to a taskprocessor
 *
 * tps_tasks are processed in FIFO order and freed by the taskprocessing
 * thread after the task handler returns.  The callback function that is assigned
 * to the execute() function pointer is responsible for releasing datap resources if necessary. */
struct tps_task {
	/*! \brief The execute() task callback function pointer */
	int (*execute)(void *datap);
	/*! \brief The data pointer for the task execute() function */
	void *datap;
	/*! \brief TRIS_LIST_ENTRY overhead */
	TRIS_LIST_ENTRY(tps_task) list;
};

/*! \brief tps_taskprocessor_stats maintain statistics for a taskprocessor. */
struct tps_taskprocessor_stats {
	/*! \brief This is the maximum number of tasks queued at any one time */
	unsigned long max_qsize;
	/*! \brief This is the current number of tasks processed */
	unsigned long _tasks_processed_count;
};

/*! \brief A tris_taskprocessor structure is a singleton by name */
struct tris_taskprocessor {
	/*! \brief Friendly name of the taskprocessor */
	char *name;
	/*! \brief Thread poll condition */
	tris_cond_t poll_cond;
	/*! \brief Taskprocessor thread */
	pthread_t poll_thread;
	/*! \brief Taskprocessor lock */
	tris_mutex_t taskprocessor_lock;
	/*! \brief Taskprocesor thread run flag */
	unsigned char poll_thread_run;
	/*! \brief Taskprocessor statistics */
	struct tps_taskprocessor_stats *stats;
	/*! \brief Taskprocessor current queue size */
	long tps_queue_size;
	/*! \brief Taskprocessor queue */
	TRIS_LIST_HEAD_NOLOCK(tps_queue, tps_task) tps_queue;
	/*! \brief Taskprocessor singleton list entry */
	TRIS_LIST_ENTRY(tris_taskprocessor) list;
};
#define TPS_MAX_BUCKETS 7
/*! \brief tps_singletons is the astobj2 container for taskprocessor singletons */
static struct ao2_container *tps_singletons;

/*! \brief CLI <example>taskprocessor ping &lt;blah&gt;</example> operation requires a ping condition */
static tris_cond_t cli_ping_cond;

/*! \brief CLI <example>taskprocessor ping &lt;blah&gt;</example> operation requires a ping condition lock */
TRIS_MUTEX_DEFINE_STATIC(cli_ping_cond_lock);

/*! \brief The astobj2 hash callback for taskprocessors */
static int tps_hash_cb(const void *obj, const int flags);
/*! \brief The astobj2 compare callback for taskprocessors */
static int tps_cmp_cb(void *obj, void *arg, int flags);

/*! \brief The task processing function executed by a taskprocessor */
static void *tps_processing_function(void *data);

/*! \brief Destroy the taskprocessor when its refcount reaches zero */
static void tps_taskprocessor_destroy(void *tps);

/*! \brief CLI <example>taskprocessor ping &lt;blah&gt;</example> handler function */
static int tps_ping_handler(void *datap);

/*! \brief Remove the front task off the taskprocessor queue */
static struct tps_task *tps_taskprocessor_pop(struct tris_taskprocessor *tps);

/*! \brief Return the size of the taskprocessor queue */
static int tps_taskprocessor_depth(struct tris_taskprocessor *tps);

static char *cli_tps_ping(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
static char *cli_tps_report(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);

static struct tris_cli_entry taskprocessor_clis[] = {
	TRIS_CLI_DEFINE(cli_tps_ping, "Ping a named task processor"),
	TRIS_CLI_DEFINE(cli_tps_report, "List instantiated task processors and statistics"),
};

/* initialize the taskprocessor container and register CLI operations */
int tris_tps_init(void)
{
	if (!(tps_singletons = ao2_container_alloc(TPS_MAX_BUCKETS, tps_hash_cb, tps_cmp_cb))) {
		tris_log(LOG_ERROR, "taskprocessor container failed to initialize!\n");
		return -1;
	}

	tris_cond_init(&cli_ping_cond, NULL);

	tris_cli_register_multiple(taskprocessor_clis, ARRAY_LEN(taskprocessor_clis));
	return 0;
}

/* allocate resources for the task */
static struct tps_task *tps_task_alloc(int (*task_exe)(void *datap), void *datap)
{
	struct tps_task *t;
	if ((t = tris_calloc(1, sizeof(*t)))) {
		t->execute = task_exe;
		t->datap = datap;
	}
	return t;
}

/* release task resources */	
static void *tps_task_free(struct tps_task *task)
{
	if (task) {
		tris_free(task);
	}
	return NULL;
}

/* taskprocessor tab completion */
static char *tps_taskprocessor_tab_complete(struct tris_taskprocessor *p, struct tris_cli_args *a) 
{
	int tklen;
	int wordnum = 0;
	char *name = NULL;
	struct ao2_iterator i;

	if (a->pos != 3)
		return NULL;

	tklen = strlen(a->word);
	i = ao2_iterator_init(tps_singletons, 0);
	while ((p = ao2_iterator_next(&i))) {
		if (!strncasecmp(a->word, p->name, tklen) && ++wordnum > a->n) {
			name = tris_strdup(p->name);
			ao2_ref(p, -1);
			break;
		}
		ao2_ref(p, -1);
	}
	return name;
}

/* ping task handling function */
static int tps_ping_handler(void *datap)
{
	tris_mutex_lock(&cli_ping_cond_lock);
	tris_cond_signal(&cli_ping_cond);
	tris_mutex_unlock(&cli_ping_cond_lock);
	return 0;
}

/* ping the specified taskprocessor and display the ping time on the CLI */
static char *cli_tps_ping(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct timeval begin, end, delta;
	char *name;
	struct timeval when;
	struct timespec ts;
	struct tris_taskprocessor *tps = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core ping taskprocessor";
		e->usage = 
			"Usage: core ping taskprocessor <taskprocessor>\n"
			"	Displays the time required for a task to be processed\n";
		return NULL;
	case CLI_GENERATE:
		return tps_taskprocessor_tab_complete(tps, a);
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	name = a->argv[3];
	if (!(tps = tris_taskprocessor_get(name, TPS_REF_IF_EXISTS))) {
		tris_cli(a->fd, "\nping failed: %s not found\n\n", name);
		return CLI_SUCCESS;
	}
	tris_cli(a->fd, "\npinging %s ...", name);
	when = tris_tvadd((begin = tris_tvnow()), tris_samp2tv(1000, 1000));
	ts.tv_sec = when.tv_sec;
	ts.tv_nsec = when.tv_usec * 1000;
	tris_mutex_lock(&cli_ping_cond_lock);
	if (tris_taskprocessor_push(tps, tps_ping_handler, 0) < 0) {
		tris_cli(a->fd, "\nping failed: could not push task to %s\n\n", name);
		ao2_ref(tps, -1);
		return CLI_FAILURE;
	}
	tris_cond_timedwait(&cli_ping_cond, &cli_ping_cond_lock, &ts);
	tris_mutex_unlock(&cli_ping_cond_lock);
	end = tris_tvnow();
	delta = tris_tvsub(end, begin);
	tris_cli(a->fd, "\n\t%24s ping time: %.1ld.%.6ld sec\n\n", name, (long)delta.tv_sec, (long int)delta.tv_usec);
	ao2_ref(tps, -1);
	return CLI_SUCCESS;	
}

static char *cli_tps_report(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	char name[256];
	int tcount;
	unsigned long qsize;
	unsigned long maxqsize;
	unsigned long processed;
	struct tris_taskprocessor *p;
	struct ao2_iterator i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show taskprocessors";
		e->usage = 
			"Usage: core show taskprocessors\n"
			"	Shows a list of instantiated task processors and their statistics\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "\n\t+----- Processor -----+--- Processed ---+- In Queue -+- Max Depth -+");
	i = ao2_iterator_init(tps_singletons, 0);
	while ((p = ao2_iterator_next(&i))) {
		tris_copy_string(name, p->name, sizeof(name));
		qsize = p->tps_queue_size;
		maxqsize = p->stats->max_qsize;
		processed = p->stats->_tasks_processed_count;
		tris_cli(a->fd, "\n%24s   %17ld %12ld %12ld", name, processed, qsize, maxqsize);
		ao2_ref(p, -1);
	}
	tcount = ao2_container_count(tps_singletons); 
	tris_cli(a->fd, "\n\t+---------------------+-----------------+------------+-------------+\n\t%d taskprocessors\n\n", tcount);
	return CLI_SUCCESS;	
}

/* this is the task processing worker function */
static void *tps_processing_function(void *data)
{
	struct tris_taskprocessor *i = data;
	struct tps_task *t;
	int size;

	if (!i) {
		tris_log(LOG_ERROR, "cannot start thread_function loop without a tris_taskprocessor structure.\n");
		return NULL;
	}

	while (i->poll_thread_run) {
 		tris_mutex_lock(&i->taskprocessor_lock);
 		if (!i->poll_thread_run) {
  			tris_mutex_unlock(&i->taskprocessor_lock);
 			break;
  		}
 		if (!(size = tps_taskprocessor_depth(i))) {
 			tris_cond_wait(&i->poll_cond, &i->taskprocessor_lock);
  			if (!i->poll_thread_run) {
  				tris_mutex_unlock(&i->taskprocessor_lock);
	  			break;
			}
  		}
  		tris_mutex_unlock(&i->taskprocessor_lock);
 		/* stuff is in the queue */
 		if (!(t = tps_taskprocessor_pop(i))) {
 			tris_log(LOG_ERROR, "Wtf?? %d tasks in the queue, but we're popping blanks!\n", size);
 			continue;
 		}
 		if (!t->execute) {
 			tris_log(LOG_WARNING, "Task is missing a function to execute!\n");
 			tps_task_free(t);
 			continue;
 		}
 		t->execute(t->datap);
 
 		tris_mutex_lock(&i->taskprocessor_lock);
 		if (i->stats) {
 			i->stats->_tasks_processed_count++;
 			if (size > i->stats->max_qsize) {
 				i->stats->max_qsize = size;
 			}
 		}
 		tris_mutex_unlock(&i->taskprocessor_lock);
 
 		tps_task_free(t);
  	}
	while ((t = tps_taskprocessor_pop(i))) {
		tps_task_free(t);
	}
	return NULL;
}

/* hash callback for astobj2 */
static int tps_hash_cb(const void *obj, const int flags)
{
	const struct tris_taskprocessor *tps = obj;

	return tris_str_case_hash(tps->name);
}

/* compare callback for astobj2 */
static int tps_cmp_cb(void *obj, void *arg, int flags)
{
	struct tris_taskprocessor *lhs = obj, *rhs = arg;

	return !strcasecmp(lhs->name, rhs->name) ? CMP_MATCH | CMP_STOP : 0;
}

/* destroy the taskprocessor */
static void tps_taskprocessor_destroy(void *tps)
{
	struct tris_taskprocessor *t = tps;
	
	if (!tps) {
		tris_log(LOG_ERROR, "missing taskprocessor\n");
		return;
	}
	tris_log(LOG_DEBUG, "destroying taskprocessor '%s'\n", t->name);
	/* kill it */	
	tris_mutex_lock(&t->taskprocessor_lock);
	t->poll_thread_run = 0;
	tris_cond_signal(&t->poll_cond);
	tris_mutex_unlock(&t->taskprocessor_lock);
	pthread_join(t->poll_thread, NULL);
	t->poll_thread = TRIS_PTHREADT_NULL;
	tris_mutex_destroy(&t->taskprocessor_lock);
	tris_cond_destroy(&t->poll_cond);
	/* free it */
	if (t->stats) {
		tris_free(t->stats);
		t->stats = NULL;
	}
	tris_free(t->name);
}

/* pop the front task and return it */
static struct tps_task *tps_taskprocessor_pop(struct tris_taskprocessor *tps)
{
	struct tps_task *task;

	if (!tps) {
		tris_log(LOG_ERROR, "missing taskprocessor\n");
		return NULL;
	}
	tris_mutex_lock(&tps->taskprocessor_lock);
	if ((task = TRIS_LIST_REMOVE_HEAD(&tps->tps_queue, list))) {
		tps->tps_queue_size--;
	}
	tris_mutex_unlock(&tps->taskprocessor_lock);
	return task;
}

static int tps_taskprocessor_depth(struct tris_taskprocessor *tps)
{
	return (tps) ? tps->tps_queue_size : -1;
}

/* taskprocessor name accessor */
const char *tris_taskprocessor_name(struct tris_taskprocessor *tps)
{
	if (!tps) {
		tris_log(LOG_ERROR, "no taskprocessor specified!\n");
		return NULL;
	}
	return tps->name;
}

/* Provide a reference to a taskprocessor.  Create the taskprocessor if necessary, but don't
 * create the taskprocessor if we were told via tris_tps_options to return a reference only 
 * if it already exists */
struct tris_taskprocessor *tris_taskprocessor_get(char *name, enum tris_tps_options create)
{
	struct tris_taskprocessor *p, tmp_tps = {
		.name = name,
	};
		
	if (tris_strlen_zero(name)) {
		tris_log(LOG_ERROR, "requesting a nameless taskprocessor!!!\n");
		return NULL;
	}
	ao2_lock(tps_singletons);
	p = ao2_find(tps_singletons, &tmp_tps, OBJ_POINTER);
	if (p) {
		ao2_unlock(tps_singletons);
		return p;
	}
	if (create & TPS_REF_IF_EXISTS) {
		/* calling function does not want a new taskprocessor to be created if it doesn't already exist */
		ao2_unlock(tps_singletons);
		return NULL;
	}
	/* create a new taskprocessor */
	if (!(p = ao2_alloc(sizeof(*p), tps_taskprocessor_destroy))) {
		ao2_unlock(tps_singletons);
		tris_log(LOG_WARNING, "failed to create taskprocessor '%s'\n", name);
		return NULL;
	}

	tris_cond_init(&p->poll_cond, NULL);
	tris_mutex_init(&p->taskprocessor_lock);

	if (!(p->stats = tris_calloc(1, sizeof(*p->stats)))) {
		ao2_unlock(tps_singletons);
		tris_log(LOG_WARNING, "failed to create taskprocessor stats for '%s'\n", name);
		ao2_ref(p, -1);
		return NULL;
	}
	if (!(p->name = tris_strdup(name))) {
		ao2_unlock(tps_singletons);
		ao2_ref(p, -1);
		return NULL;
	}
	p->poll_thread_run = 1;
	p->poll_thread = TRIS_PTHREADT_NULL;
	if (tris_pthread_create(&p->poll_thread, NULL, tps_processing_function, p) < 0) {
		ao2_unlock(tps_singletons);
		tris_log(LOG_ERROR, "Taskprocessor '%s' failed to create the processing thread.\n", p->name);
		ao2_ref(p, -1);
		return NULL;
	}
	if (!(ao2_link(tps_singletons, p))) {
		ao2_unlock(tps_singletons);
		tris_log(LOG_ERROR, "Failed to add taskprocessor '%s' to container\n", p->name);
		ao2_ref(p, -1);
		return NULL;
	}
	ao2_unlock(tps_singletons);
	return p;
}

/* decrement the taskprocessor reference count and unlink from the container if necessary */
void *tris_taskprocessor_unreference(struct tris_taskprocessor *tps)
{
	if (tps) {
		ao2_lock(tps_singletons);
		ao2_unlink(tps_singletons, tps);
		if (ao2_ref(tps, -1) > 1) {
			ao2_link(tps_singletons, tps);
		}
		ao2_unlock(tps_singletons);
	}
	return NULL;
}

/* push the task into the taskprocessor queue */	
int tris_taskprocessor_push(struct tris_taskprocessor *tps, int (*task_exe)(void *datap), void *datap)
{
	struct tps_task *t;

	if (!tps || !task_exe) {
		tris_log(LOG_ERROR, "%s is missing!!\n", (tps) ? "task callback" : "taskprocessor");
		return -1;
	}
	if (!(t = tps_task_alloc(task_exe, datap))) {
		tris_log(LOG_ERROR, "failed to allocate task!  Can't push to '%s'\n", tps->name);
		return -1;
	}
	tris_mutex_lock(&tps->taskprocessor_lock);
	TRIS_LIST_INSERT_TAIL(&tps->tps_queue, t, list);
	tps->tps_queue_size++;
	tris_cond_signal(&tps->poll_cond);
	tris_mutex_unlock(&tps->taskprocessor_lock);
	return 0;
}

