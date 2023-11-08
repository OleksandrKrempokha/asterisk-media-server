/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Kevin P. Fleming <kpfleming@digium.com>
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
 * \brief Debugging support for thread-local-storage objects
 *
 * \author Kevin P. Fleming <kpfleming@digium.com>
 */

#include "trismedia.h"
#include "trismedia/_private.h"

#if !defined(DEBUG_THREADLOCALS)

void threadstorage_init(void)
{
}

#else /* !defined(DEBUG_THREADLOCALS) */

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 164737 $")

#include "trismedia/strings.h"
#include "trismedia/utils.h"
#include "trismedia/threadstorage.h"
#include "trismedia/linkedlists.h"
#include "trismedia/cli.h"

struct tls_object {
	void *key;
	size_t size;
	const char *file;
	const char *function;
	unsigned int line;
	pthread_t thread;
	TRIS_LIST_ENTRY(tls_object) entry;
};

static TRIS_LIST_HEAD_NOLOCK_STATIC(tls_objects, tls_object);

/* Allow direct use of pthread_mutex_t and friends */
#undef pthread_mutex_t
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_mutex_init
#undef pthread_mutex_destroy

/*!
 * \brief lock for the tls_objects list
 *
 * \note We can not use an tris_mutex_t for this.  The reason is that this
 *       lock is used within the context of thread-local data destructors,
 *       and the tris_mutex_* API uses thread-local data.  Allocating more
 *       thread-local data at that point just causes a memory leak.
 */
static pthread_mutex_t threadstoragelock;

void __tris_threadstorage_object_add(void *key, size_t len, const char *file, const char *function, unsigned int line)
{
	struct tls_object *to;

	if (!(to = tris_calloc(1, sizeof(*to))))
		return;

	to->key = key;
	to->size = len;
	to->file = file;
	to->function = function;
	to->line = line;
	to->thread = pthread_self();

	pthread_mutex_lock(&threadstoragelock);
	TRIS_LIST_INSERT_TAIL(&tls_objects, to, entry);
	pthread_mutex_unlock(&threadstoragelock);
}

void __tris_threadstorage_object_remove(void *key)
{
	struct tls_object *to;

	pthread_mutex_lock(&threadstoragelock);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&tls_objects, to, entry) {
		if (to->key == key) {
			TRIS_LIST_REMOVE_CURRENT(entry);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	pthread_mutex_unlock(&threadstoragelock);
	if (to)
		tris_free(to);
}

void __tris_threadstorage_object_replace(void *key_old, void *key_new, size_t len)
{
	struct tls_object *to;

	pthread_mutex_lock(&threadstoragelock);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&tls_objects, to, entry) {
		if (to->key == key_old) {
			to->key = key_new;
			to->size = len;
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	pthread_mutex_unlock(&threadstoragelock);
}

static char *handle_cli_threadstorage_show_allocations(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	char *fn = NULL;
	size_t len = 0;
	unsigned int count = 0;
	struct tls_object *to;

	switch (cmd) {
	case CLI_INIT:
		e->command = "threadstorage show allocations";
		e->usage =
			"Usage: threadstorage show allocations [<file>]\n"
			"       Dumps a list of all thread-specific memory allocations,\n"
			"       optionally limited to those from a specific file\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc > 4)
		return CLI_SHOWUSAGE;

	if (a->argc > 3)
		fn = a->argv[3];

	pthread_mutex_lock(&threadstoragelock);

	TRIS_LIST_TRAVERSE(&tls_objects, to, entry) {
		if (fn && strcasecmp(to->file, fn))
			continue;

		tris_cli(a->fd, "%10d bytes allocated in %20s at line %5d of %25s (thread %p)\n",
			(int) to->size, to->function, to->line, to->file, (void *) to->thread);
		len += to->size;
		count++;
	}

	pthread_mutex_unlock(&threadstoragelock);

	tris_cli(a->fd, "%10d bytes allocated in %d allocation%s\n", (int) len, count, count > 1 ? "s" : "");
	
	return CLI_SUCCESS;
}

static char *handle_cli_threadstorage_show_summary(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	char *fn = NULL;
	size_t len = 0;
	unsigned int count = 0;
	struct tls_object *to;
	struct file {
		const char *name;
		size_t len;
		unsigned int count;
		TRIS_LIST_ENTRY(file) entry;
	} *file;
	TRIS_LIST_HEAD_NOLOCK_STATIC(file_summary, file);

	switch (cmd) {
	case CLI_INIT:
		e->command = "threadstorage show summary";
		e->usage =
			"Usage: threadstorage show summary [<file>]\n"
			"       Summarizes thread-specific memory allocations by file, or optionally\n"
			"       by function, if a file is specified\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc > 4)
		return CLI_SHOWUSAGE;

	if (a->argc > 3)
		fn = a->argv[3];

	pthread_mutex_lock(&threadstoragelock);

	TRIS_LIST_TRAVERSE(&tls_objects, to, entry) {
		if (fn && strcasecmp(to->file, fn))
			continue;

		TRIS_LIST_TRAVERSE(&file_summary, file, entry) {
			if ((!fn && (file->name == to->file)) || (fn && (file->name == to->function)))
				break;
		}

		if (!file) {
			file = alloca(sizeof(*file));
			memset(file, 0, sizeof(*file));
			file->name = fn ? to->function : to->file;
			TRIS_LIST_INSERT_TAIL(&file_summary, file, entry);
		}

		file->len += to->size;
		file->count++;
	}

	pthread_mutex_unlock(&threadstoragelock);
	
	TRIS_LIST_TRAVERSE(&file_summary, file, entry) {
		len += file->len;
		count += file->count;
		if (fn) {
			tris_cli(a->fd, "%10d bytes in %d allocation%ss in function %s\n",
				(int) file->len, file->count, file->count > 1 ? "s" : "", file->name);
		} else {
			tris_cli(a->fd, "%10d bytes in %d allocation%s in file %s\n",
				(int) file->len, file->count, file->count > 1 ? "s" : "", file->name);
		}
	}

	tris_cli(a->fd, "%10d bytes allocated in %d allocation%s\n", (int) len, count, count > 1 ? "s" : "");

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli[] = {
	TRIS_CLI_DEFINE(handle_cli_threadstorage_show_allocations, "Display outstanding thread local storage allocations"),
	TRIS_CLI_DEFINE(handle_cli_threadstorage_show_summary,     "Summarize outstanding memory allocations")
};

void threadstorage_init(void)
{
	pthread_mutex_init(&threadstoragelock, NULL);
	tris_cli_register_multiple(cli, ARRAY_LEN(cli));
}

#endif /* !defined(DEBUG_THREADLOCALS) */

