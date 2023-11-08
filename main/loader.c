/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Kevin P. Fleming <kpfleming@digium.com>
 * Luigi Rizzo <rizzo@icir.org>
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
 * \brief Module Loader
 * \author Mark Spencer <markster@digium.com>
 * \author Kevin P. Fleming <kpfleming@digium.com>
 * \author Luigi Rizzo <rizzo@icir.org>
 * - See ModMngMnt
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 248011 $")

#include "trismedia/_private.h"
#include "trismedia/paths.h"	/* use tris_config_TRIS_MODULE_DIR */
#include <dirent.h>

#include "trismedia/linkedlists.h"
#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/channel.h"
#include "trismedia/term.h"
#include "trismedia/manager.h"
#include "trismedia/cdr.h"
#include "trismedia/enum.h"
#include "trismedia/rtp.h"
#include "trismedia/http.h"
#include "trismedia/lock.h"
#include "trismedia/features.h"
#include "trismedia/dsp.h"
#include "trismedia/udptl.h"
#include "trismedia/heap.h"

#include <dlfcn.h>

#include "trismedia/md5.h"
#include "trismedia/utils.h"

#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif

#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif

struct tris_module_user {
	struct tris_channel *chan;
	TRIS_LIST_ENTRY(tris_module_user) entry;
};

TRIS_LIST_HEAD(module_user_list, tris_module_user);

static unsigned char expected_key[] =
{ 0x87, 0x81, 0x84, 0x45, 0x0d, 0x35, 0xed, 0x0d,
  0x4a, 0xf1, 0xe8, 0x0f, 0xb6, 0x31, 0x64, 0xc4 };

static char buildopt_sum[33] = TRIS_BUILDOPT_SUM;

static unsigned int embedding = 1; /* we always start out by registering embedded modules,
				      since they are here before we dlopen() any
				   */

struct tris_module {
	const struct tris_module_info *info;
	void *lib;					/* the shared lib, or NULL if embedded */
	int usecount;					/* the number of 'users' currently in this module */
	struct module_user_list users;			/* the list of users in the module */
	struct {
		unsigned int running:1;
		unsigned int declined:1;
	} flags;
	TRIS_LIST_ENTRY(tris_module) entry;
	char resource[0];
};

static TRIS_LIST_HEAD_STATIC(module_list, tris_module);

/*
 * module_list is cleared by its constructor possibly after
 * we start accumulating embedded modules, so we need to
 * use another list (without the lock) to accumulate them.
 * Then we update the main list when embedding is done.
 */
static struct module_list embedded_module_list;

struct loadupdate {
	int (*updater)(void);
	TRIS_LIST_ENTRY(loadupdate) entry;
};

static TRIS_LIST_HEAD_STATIC(updaters, loadupdate);

TRIS_MUTEX_DEFINE_STATIC(reloadlock);

struct reload_queue_item {
	TRIS_LIST_ENTRY(reload_queue_item) entry;
	char module[0];
};

static int do_full_reload = 0;

static TRIS_LIST_HEAD_STATIC(reload_queue, reload_queue_item);

/* when dynamic modules are being loaded, tris_module_register() will
   need to know what filename the module was loaded from while it
   is being registered
*/
struct tris_module *resource_being_loaded;

/* XXX: should we check for duplicate resource names here? */

void tris_module_register(const struct tris_module_info *info)
{
	struct tris_module *mod;

	if (embedding) {
		if (!(mod = tris_calloc(1, sizeof(*mod) + strlen(info->name) + 1)))
			return;
		strcpy(mod->resource, info->name);
	} else {
		mod = resource_being_loaded;
	}

	mod->info = info;
	TRIS_LIST_HEAD_INIT(&mod->users);

	/* during startup, before the loader has been initialized,
	   there are no threads, so there is no need to take the lock
	   on this list to manipulate it. it is also possible that it
	   might be unsafe to use the list lock at that point... so
	   let's avoid it altogether
	*/
	if (embedding) {
		TRIS_LIST_INSERT_TAIL(&embedded_module_list, mod, entry);
	} else {
		TRIS_LIST_LOCK(&module_list);
		/* it is paramount that the new entry be placed at the tail of
		   the list, otherwise the code that uses dlopen() to load
		   dynamic modules won't be able to find out if the module it
		   just opened was registered or failed to load
		*/
		TRIS_LIST_INSERT_TAIL(&module_list, mod, entry);
		TRIS_LIST_UNLOCK(&module_list);
	}

	/* give the module a copy of its own handle, for later use in registrations and the like */
	*((struct tris_module **) &(info->self)) = mod;
}

void tris_module_unregister(const struct tris_module_info *info)
{
	struct tris_module *mod = NULL;

	/* it is assumed that the users list in the module structure
	   will already be empty, or we cannot have gotten to this
	   point
	*/
	TRIS_LIST_LOCK(&module_list);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&module_list, mod, entry) {
		if (mod->info == info) {
			TRIS_LIST_REMOVE_CURRENT(entry);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&module_list);

	if (mod) {
		TRIS_LIST_HEAD_DESTROY(&mod->users);
		tris_free(mod);
	}
}

struct tris_module_user *__tris_module_user_add(struct tris_module *mod,
					      struct tris_channel *chan)
{
	struct tris_module_user *u = tris_calloc(1, sizeof(*u));

	if (!u)
		return NULL;

	u->chan = chan;

	TRIS_LIST_LOCK(&mod->users);
	TRIS_LIST_INSERT_HEAD(&mod->users, u, entry);
	TRIS_LIST_UNLOCK(&mod->users);

	tris_atomic_fetchadd_int(&mod->usecount, +1);

	tris_update_use_count();

	return u;
}

void __tris_module_user_remove(struct tris_module *mod, struct tris_module_user *u)
{
	TRIS_LIST_LOCK(&mod->users);
	TRIS_LIST_REMOVE(&mod->users, u, entry);
	TRIS_LIST_UNLOCK(&mod->users);
	tris_atomic_fetchadd_int(&mod->usecount, -1);
	tris_free(u);

	tris_update_use_count();
}

void __tris_module_user_hangup_all(struct tris_module *mod)
{
	struct tris_module_user *u;

	TRIS_LIST_LOCK(&mod->users);
	while ((u = TRIS_LIST_REMOVE_HEAD(&mod->users, entry))) {
		tris_softhangup(u->chan, TRIS_SOFTHANGUP_APPUNLOAD);
		tris_atomic_fetchadd_int(&mod->usecount, -1);
		tris_free(u);
	}
	TRIS_LIST_UNLOCK(&mod->users);

	tris_update_use_count();
}

/*! \note
 * In addition to modules, the reload command handles some extra keywords
 * which are listed here together with the corresponding handlers.
 * This table is also used by the command completion code.
 */
static struct reload_classes {
	const char *name;
	int (*reload_fn)(void);
} reload_classes[] = {	/* list in alpha order, longest match first for cli completion */
	{ "cdr",	tris_cdr_engine_reload },
	{ "dnsmgr",	dnsmgr_reload },
	{ "extconfig",	read_config_maps },
	{ "enum",	tris_enum_reload },
	{ "manager",	reload_manager },
	{ "rtp",	tris_rtp_reload },
	{ "http",	tris_http_reload },
	{ "logger",	logger_reload },
	{ "features",	tris_features_reload },
	{ "dsp",	tris_dsp_reload},
	{ "udptl",	tris_udptl_reload },
	{ "indications", tris_indications_reload },
	{ NULL, 	NULL }
};

static int printdigest(const unsigned char *d)
{
	int x, pos;
	char buf[256]; /* large enough so we don't have to worry */

	for (pos = 0, x = 0; x < 16; x++)
		pos += sprintf(buf + pos, " %02x", *d++);

	tris_debug(1, "Unexpected signature:%s\n", buf);

	return 0;
}

static int key_matches(const unsigned char *key1, const unsigned char *key2)
{
	int x;

	for (x = 0; x < 16; x++) {
		if (key1[x] != key2[x])
			return 0;
	}

	return 1;
}

static int verify_key(const unsigned char *key)
{
	struct MD5Context c;
	unsigned char digest[16];

	MD5Init(&c);
	MD5Update(&c, key, strlen((char *)key));
	MD5Final(digest, &c);

	if (key_matches(expected_key, digest))
		return 0;

	printdigest(digest);

	return -1;
}

static int resource_name_match(const char *name1_in, const char *name2_in)
{
	char *name1 = (char *) name1_in;
	char *name2 = (char *) name2_in;

	/* trim off any .so extensions */
	if (!strcasecmp(name1 + strlen(name1) - 3, ".so")) {
		name1 = tris_strdupa(name1);
		name1[strlen(name1) - 3] = '\0';
	}
	if (!strcasecmp(name2 + strlen(name2) - 3, ".so")) {
		name2 = tris_strdupa(name2);
		name2[strlen(name2) - 3] = '\0';
	}

	return strcasecmp(name1, name2);
}

static struct tris_module *find_resource(const char *resource, int do_lock)
{
	struct tris_module *cur;

	if (do_lock)
		TRIS_LIST_LOCK(&module_list);

	TRIS_LIST_TRAVERSE(&module_list, cur, entry) {
		if (!resource_name_match(resource, cur->resource))
			break;
	}

	if (do_lock)
		TRIS_LIST_UNLOCK(&module_list);

	return cur;
}

#ifdef LOADABLE_MODULES
static void unload_dynamic_module(struct tris_module *mod)
{
	void *lib = mod->lib;

	/* WARNING: the structure pointed to by mod is going to
	   disappear when this operation succeeds, so we can't
	   dereference it */

	if (lib)
		while (!dlclose(lib));
}

static struct tris_module *load_dynamic_module(const char *resource_in, unsigned int global_symbols_only)
{
	char fn[PATH_MAX] = "";
	void *lib = NULL;
	struct tris_module *mod;
	unsigned int wants_global;
	int space;	/* room needed for the descriptor */
	int missing_so = 0;

	space = sizeof(*resource_being_loaded) + strlen(resource_in) + 1;
	if (strcasecmp(resource_in + strlen(resource_in) - 3, ".so")) {
		missing_so = 1;
		space += 3;	/* room for the extra ".so" */
	}

	snprintf(fn, sizeof(fn), "%s/%s%s", tris_config_TRIS_MODULE_DIR, resource_in, missing_so ? ".so" : "");

	/* make a first load of the module in 'quiet' mode... don't try to resolve
	   any symbols, and don't export any symbols. this will allow us to peek into
	   the module's info block (if available) to see what flags it has set */

	resource_being_loaded = tris_calloc(1, space);
	if (!resource_being_loaded)
		return NULL;
	strcpy(resource_being_loaded->resource, resource_in);
	if (missing_so)
		strcat(resource_being_loaded->resource, ".so");

	if (!(lib = dlopen(fn, RTLD_LAZY | RTLD_LOCAL))) {
		tris_log(LOG_WARNING, "Error loading module '%s': %s\n", resource_in, dlerror());
		tris_free(resource_being_loaded);
		return NULL;
	}

	/* the dlopen() succeeded, let's find out if the module
	   registered itself */
	/* note that this will only work properly as long as
	   tris_module_register() (which is called by the module's
	   constructor) places the new module at the tail of the
	   module_list
	*/
	if (resource_being_loaded != (mod = TRIS_LIST_LAST(&module_list))) {
		tris_log(LOG_WARNING, "Module '%s' did not register itself during load\n", resource_in);
		/* no, it did not, so close it and return */
		while (!dlclose(lib));
		/* note that the module's destructor will call tris_module_unregister(),
		   which will free the structure we allocated in resource_being_loaded */
		return NULL;
	}

	wants_global = tris_test_flag(mod->info, TRIS_MODFLAG_GLOBAL_SYMBOLS);

	/* if we are being asked only to load modules that provide global symbols,
	   and this one does not, then close it and return */
	if (global_symbols_only && !wants_global) {
		while (!dlclose(lib));
		return NULL;
	}

	while (!dlclose(lib));
	resource_being_loaded = NULL;

	/* start the load process again */
	resource_being_loaded = tris_calloc(1, space);
	if (!resource_being_loaded)
		return NULL;
	strcpy(resource_being_loaded->resource, resource_in);
	if (missing_so)
		strcat(resource_being_loaded->resource, ".so");

	if (!(lib = dlopen(fn, wants_global ? RTLD_LAZY | RTLD_GLOBAL : RTLD_NOW | RTLD_LOCAL))) {
		tris_log(LOG_WARNING, "Error loading module '%s': %s\n", resource_in, dlerror());
		tris_free(resource_being_loaded);
		return NULL;
	}

	/* since the module was successfully opened, and it registered itself
	   the previous time we did that, we're going to assume it worked this
	   time too :) */

	TRIS_LIST_LAST(&module_list)->lib = lib;
	resource_being_loaded = NULL;

	return TRIS_LIST_LAST(&module_list);
}
#endif

void tris_module_shutdown(void)
{
	struct tris_module *mod;
	int somethingchanged = 1, final = 0;

	TRIS_LIST_LOCK(&module_list);

	/*!\note Some resources, like timers, are started up dynamically, and thus
	 * may be still in use, even if all channels are dead.  We must therefore
	 * check the usecount before asking modules to unload. */
	do {
		if (!somethingchanged) {
			/*!\note If we go through the entire list without changing
			 * anything, ignore the usecounts and unload, then exit. */
			final = 1;
		}

		/* Reset flag before traversing the list */
		somethingchanged = 0;

		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&module_list, mod, entry) {
			if (!final && mod->usecount) {
				continue;
			}
			TRIS_LIST_REMOVE_CURRENT(entry);
			if (mod->info->unload) {
				mod->info->unload();
			}
			TRIS_LIST_HEAD_DESTROY(&mod->users);
			free(mod);
			somethingchanged = 1;
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
	} while (somethingchanged && !final);

	TRIS_LIST_UNLOCK(&module_list);
}

int tris_unload_resource(const char *resource_name, enum tris_module_unload_mode force)
{
	struct tris_module *mod;
	int res = -1;
	int error = 0;

	TRIS_LIST_LOCK(&module_list);

	if (!(mod = find_resource(resource_name, 0))) {
		TRIS_LIST_UNLOCK(&module_list);
		tris_log(LOG_WARNING, "Unload failed, '%s' could not be found\n", resource_name);
		return 0;
	}

	if (!(mod->flags.running || mod->flags.declined))
		error = 1;

	if (!error && (mod->usecount > 0)) {
		if (force)
			tris_log(LOG_WARNING, "Warning:  Forcing removal of module '%s' with use count %d\n",
				resource_name, mod->usecount);
		else {
			tris_log(LOG_WARNING, "Soft unload failed, '%s' has use count %d\n", resource_name,
				mod->usecount);
			error = 1;
		}
	}

	if (!error) {
		__tris_module_user_hangup_all(mod);
		res = mod->info->unload();

		if (res) {
			tris_log(LOG_WARNING, "Firm unload failed for %s\n", resource_name);
			if (force <= TRIS_FORCE_FIRM)
				error = 1;
			else
				tris_log(LOG_WARNING, "** Dangerous **: Unloading resource anyway, at user request\n");
		}
	}

	if (!error)
		mod->flags.running = mod->flags.declined = 0;

	TRIS_LIST_UNLOCK(&module_list);

	if (!error && !mod->lib && mod->info && mod->info->restore_globals)
		mod->info->restore_globals();

#ifdef LOADABLE_MODULES
	if (!error)
		unload_dynamic_module(mod);
#endif

	if (!error)
		tris_update_use_count();

	return res;
}

char *tris_module_helper(const char *line, const char *word, int pos, int state, int rpos, int needsreload)
{
	struct tris_module *cur;
	int i, which=0, l = strlen(word);
	char *ret = NULL;

	if (pos != rpos)
		return NULL;

	TRIS_LIST_LOCK(&module_list);
	TRIS_LIST_TRAVERSE(&module_list, cur, entry) {
		if (!strncasecmp(word, cur->resource, l) &&
		    (cur->info->reload || !needsreload) &&
		    ++which > state) {
			ret = tris_strdup(cur->resource);
			break;
		}
	}
	TRIS_LIST_UNLOCK(&module_list);

	if (!ret) {
		for (i=0; !ret && reload_classes[i].name; i++) {
			if (!strncasecmp(word, reload_classes[i].name, l) && ++which > state)
				ret = tris_strdup(reload_classes[i].name);
		}
	}

	return ret;
}

void tris_process_pending_reloads(void)
{
	struct reload_queue_item *item;

	if (!tris_fully_booted) {
		return;
	}

	TRIS_LIST_LOCK(&reload_queue);

	if (do_full_reload) {
		do_full_reload = 0;
		TRIS_LIST_UNLOCK(&reload_queue);
		tris_log(LOG_NOTICE, "Executing deferred reload request.\n");
		tris_module_reload(NULL);
		return;
	}

	while ((item = TRIS_LIST_REMOVE_HEAD(&reload_queue, entry))) {
		tris_log(LOG_NOTICE, "Executing deferred reload request for module '%s'.\n", item->module);
		tris_module_reload(item->module);
		tris_free(item);
	}

	TRIS_LIST_UNLOCK(&reload_queue);
}

static void queue_reload_request(const char *module)
{
	struct reload_queue_item *item;

	TRIS_LIST_LOCK(&reload_queue);

	if (do_full_reload) {
		TRIS_LIST_UNLOCK(&reload_queue);
		return;
	}

	if (tris_strlen_zero(module)) {
		/* A full reload request (when module is NULL) wipes out any previous
		   reload requests and causes the queue to ignore future ones */
		while ((item = TRIS_LIST_REMOVE_HEAD(&reload_queue, entry))) {
			tris_free(item);
		}
		do_full_reload = 1;
	} else {
		/* No reason to add the same module twice */
		TRIS_LIST_TRAVERSE(&reload_queue, item, entry) {
			if (!strcasecmp(item->module, module)) {
				TRIS_LIST_UNLOCK(&reload_queue);
				return;
			}
		}
		item = tris_calloc(1, sizeof(*item) + strlen(module) + 1);
		if (!item) {
			tris_log(LOG_ERROR, "Failed to allocate reload queue item.\n");
			TRIS_LIST_UNLOCK(&reload_queue);
			return;
		}
		strcpy(item->module, module);
		TRIS_LIST_INSERT_TAIL(&reload_queue, item, entry);
	}
	TRIS_LIST_UNLOCK(&reload_queue);
}

int tris_module_reload(const char *name)
{
	struct tris_module *cur;
	int res = 0; /* return value. 0 = not found, others, see below */
	int i;

	/* If we aren't fully booted, we just pretend we reloaded but we queue this
	   up to run once we are booted up. */
	if (!tris_fully_booted) {
		queue_reload_request(name);
		return 0;
	}

	if (tris_mutex_trylock(&reloadlock)) {
		tris_verbose("The previous reload command didn't finish yet\n");
		return -1;	/* reload already in progress */
	}
	tris_lastreloadtime = tris_tvnow();

	/* Call "predefined" reload here first */
	for (i = 0; reload_classes[i].name; i++) {
		if (!name || !strcasecmp(name, reload_classes[i].name)) {
			reload_classes[i].reload_fn();	/* XXX should check error ? */
			res = 2;	/* found and reloaded */
		}
	}

	if (name && res) {
		tris_mutex_unlock(&reloadlock);
		return res;
	}

	TRIS_LIST_LOCK(&module_list);
	TRIS_LIST_TRAVERSE(&module_list, cur, entry) {
		const struct tris_module_info *info = cur->info;

		if (name && resource_name_match(name, cur->resource))
			continue;

		if (!cur->flags.running || cur->flags.declined) {
			if (!name)
				continue;
			tris_log(LOG_NOTICE, "The module '%s' was not properly initialized.  "
				"Before reloading the module, you must run \"module load %s\" "
				"and fix whatever is preventing the module from being initialized.\n",
				name, name);
			res = 2; /* Don't report that the module was not found */
			break;
		}

		if (!info->reload) {	/* cannot be reloaded */
			if (res < 1)	/* store result if possible */
				res = 1;	/* 1 = no reload() method */
			continue;
		}

		res = 2;
		tris_verb(3, "Reloading module '%s' (%s)\n", cur->resource, info->description);
		info->reload();
	}
	TRIS_LIST_UNLOCK(&module_list);

	tris_mutex_unlock(&reloadlock);

	return res;
}

static unsigned int inspect_module(const struct tris_module *mod)
{
	if (!mod->info->description) {
		tris_log(LOG_WARNING, "Module '%s' does not provide a description.\n", mod->resource);
		return 1;
	}

	if (!mod->info->key) {
		tris_log(LOG_WARNING, "Module '%s' does not provide a license key.\n", mod->resource);
		return 1;
	}

	if (verify_key((unsigned char *) mod->info->key)) {
		tris_log(LOG_WARNING, "Module '%s' did not provide a valid license key.\n", mod->resource);
		return 1;
	}

	if (!tris_strlen_zero(mod->info->buildopt_sum) &&
	    strcmp(buildopt_sum, mod->info->buildopt_sum)) {
		tris_log(LOG_WARNING, "Module '%s' was not compiled with the same compile-time options as this version of Trismedia.\n", mod->resource);
		tris_log(LOG_WARNING, "Module '%s' will not be initialized as it may cause instability.\n", mod->resource);
		return 1;
	}

	return 0;
}

static enum tris_module_load_result start_resource(struct tris_module *mod)
{
	char tmp[256];
	enum tris_module_load_result res;

	if (!mod->info->load) {
		return TRIS_MODULE_LOAD_FAILURE;
	}

	res = mod->info->load();

	switch (res) {
	case TRIS_MODULE_LOAD_SUCCESS:
		if (!tris_fully_booted) {
			tris_verb(1, "%s => (%s)\n", mod->resource, term_color(tmp, mod->info->description, COLOR_BROWN, COLOR_BLACK, sizeof(tmp)));
			if (tris_opt_console && !option_verbose)
				tris_verbose( ".");
		} else {
			tris_verb(1, "Loaded %s => (%s)\n", mod->resource, mod->info->description);
		}

		mod->flags.running = 1;

		tris_update_use_count();
		break;
	case TRIS_MODULE_LOAD_DECLINE:
		mod->flags.declined = 1;
		break;
	case TRIS_MODULE_LOAD_FAILURE:
	case TRIS_MODULE_LOAD_SKIP: /* modules should never return this value */
	case TRIS_MODULE_LOAD_PRIORITY:
		break;
	}

	return res;
}

/*! loads a resource based upon resource_name. If global_symbols_only is set
 *  only modules with global symbols will be loaded.
 *
 *  If the tris_heap is provided (not NULL) the module is found and added to the
 *  heap without running the module's load() function.  By doing this, modules
 *  added to the resource_heap can be initialized later in order by priority. 
 *
 *  If the tris_heap is not provided, the module's load function will be executed
 *  immediately */
static enum tris_module_load_result load_resource(const char *resource_name, unsigned int global_symbols_only, struct tris_heap *resource_heap)
{
	struct tris_module *mod;
	enum tris_module_load_result res = TRIS_MODULE_LOAD_SUCCESS;

	if ((mod = find_resource(resource_name, 0))) {
		if (mod->flags.running) {
			tris_log(LOG_WARNING, "Module '%s' already exists.\n", resource_name);
			return TRIS_MODULE_LOAD_DECLINE;
		}
		if (global_symbols_only && !tris_test_flag(mod->info, TRIS_MODFLAG_GLOBAL_SYMBOLS))
			return TRIS_MODULE_LOAD_SKIP;
	} else {
#ifdef LOADABLE_MODULES
		if (!(mod = load_dynamic_module(resource_name, global_symbols_only))) {
			/* don't generate a warning message during load_modules() */
			if (!global_symbols_only) {
				tris_log(LOG_WARNING, "Module '%s' could not be loaded.\n", resource_name);
				return TRIS_MODULE_LOAD_DECLINE;
			} else {
				return TRIS_MODULE_LOAD_SKIP;
			}
		}
#else
		tris_log(LOG_WARNING, "Module '%s' could not be loaded.\n", resource_name);
		return TRIS_MODULE_LOAD_DECLINE;
#endif
	}

	if (inspect_module(mod)) {
		tris_log(LOG_WARNING, "Module '%s' could not be loaded.\n", resource_name);
#ifdef LOADABLE_MODULES
		unload_dynamic_module(mod);
#endif
		return TRIS_MODULE_LOAD_DECLINE;
	}

	if (!mod->lib && mod->info->backup_globals()) {
		tris_log(LOG_WARNING, "Module '%s' was unable to backup its global data.\n", resource_name);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	mod->flags.declined = 0;

	if (resource_heap) {
		tris_heap_push(resource_heap, mod);
		res = TRIS_MODULE_LOAD_PRIORITY;
	} else {
		res = start_resource(mod);
	}

	return res;
}

int tris_load_resource(const char *resource_name)
{
	int res;
	TRIS_LIST_LOCK(&module_list);
	res = load_resource(resource_name, 0, NULL);
	TRIS_LIST_UNLOCK(&module_list);

	return res;
}

struct load_order_entry {
	char *resource;
	TRIS_LIST_ENTRY(load_order_entry) entry;
};

TRIS_LIST_HEAD_NOLOCK(load_order, load_order_entry);

static struct load_order_entry *add_to_load_order(const char *resource, struct load_order *load_order)
{
	struct load_order_entry *order;

	TRIS_LIST_TRAVERSE(load_order, order, entry) {
		if (!resource_name_match(order->resource, resource))
			return NULL;
	}

	if (!(order = tris_calloc(1, sizeof(*order))))
		return NULL;

	order->resource = tris_strdup(resource);
	TRIS_LIST_INSERT_TAIL(load_order, order, entry);

	return order;
}

static int mod_load_cmp(void *a, void *b)
{
	struct tris_module *a_mod = (struct tris_module *) a;
	struct tris_module *b_mod = (struct tris_module *) b;
	int res = -1;
	/* if load_pri is not set, default is 255.  Lower is better*/
	unsigned char a_pri = tris_test_flag(a_mod->info, TRIS_MODFLAG_LOAD_ORDER) ? a_mod->info->load_pri : 255;
	unsigned char b_pri = tris_test_flag(b_mod->info, TRIS_MODFLAG_LOAD_ORDER) ? b_mod->info->load_pri : 255;
	if (a_pri == b_pri) {
		res = 0;
	} else if (a_pri < b_pri) {
		res = 1;
	}
	return res;
}

/*! loads modules in order by load_pri, updates mod_count */
static int load_resource_list(struct load_order *load_order, unsigned int global_symbols, int *mod_count)
{
	struct tris_heap *resource_heap;
	struct load_order_entry *order;
	struct tris_module *mod;
	int count = 0;
	int res = 0;

	if(!(resource_heap = tris_heap_create(8, mod_load_cmp, -1))) {
		return -1;
	}

	/* first, add find and add modules to heap */
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(load_order, order, entry) {
		switch (load_resource(order->resource, global_symbols, resource_heap)) {
		case TRIS_MODULE_LOAD_SUCCESS:
		case TRIS_MODULE_LOAD_DECLINE:
			TRIS_LIST_REMOVE_CURRENT(entry);
			tris_free(order->resource);
			tris_free(order);
			break;
		case TRIS_MODULE_LOAD_FAILURE:
			res = -1;
			goto done;
		case TRIS_MODULE_LOAD_SKIP:
			break;
		case TRIS_MODULE_LOAD_PRIORITY:
			TRIS_LIST_REMOVE_CURRENT(entry);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	/* second remove modules from heap sorted by priority */
	while ((mod = tris_heap_pop(resource_heap))) {
		switch (start_resource(mod)) {
		case TRIS_MODULE_LOAD_SUCCESS:
			count++;
		case TRIS_MODULE_LOAD_DECLINE:
			break;
		case TRIS_MODULE_LOAD_FAILURE:
			res = -1;
			goto done;
		case TRIS_MODULE_LOAD_SKIP:
		case TRIS_MODULE_LOAD_PRIORITY:
			break;
		}
	}

done:
	if (mod_count) {
		*mod_count += count;
	}
	tris_heap_destroy(resource_heap);

	return res;
}

int load_modules(unsigned int preload_only)
{
	struct tris_config *cfg;
	struct tris_module *mod;
	struct load_order_entry *order;
	struct tris_variable *v;
	unsigned int load_count;
	struct load_order load_order;
	int res = 0;
	struct tris_flags config_flags = { 0 };
	int modulecount = 0;

#ifdef LOADABLE_MODULES
	struct dirent *dirent;
	DIR *dir;
#endif

	/* all embedded modules have registered themselves by now */
	embedding = 0;

	tris_verb(1, "Trismedia Dynamic Loader Starting:\n");

	TRIS_LIST_HEAD_INIT_NOLOCK(&load_order);

	TRIS_LIST_LOCK(&module_list);

	if (embedded_module_list.first) {
		module_list.first = embedded_module_list.first;
		module_list.last = embedded_module_list.last;
		embedded_module_list.first = NULL;
	}

	cfg = tris_config_load2(TRIS_MODULE_CONFIG, "" /* core, can't reload */, config_flags);
	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING, "No '%s' found, no modules will be loaded.\n", TRIS_MODULE_CONFIG);
		goto done;
	}

	/* first, find all the modules we have been explicitly requested to load */
	for (v = tris_variable_browse(cfg, "modules"); v; v = v->next) {
		if (!strcasecmp(v->name, preload_only ? "preload" : "load")) {
			add_to_load_order(v->value, &load_order);
		}
	}

	/* check if 'autoload' is on */
	if (!preload_only && tris_true(tris_variable_retrieve(cfg, "modules", "autoload"))) {
		/* if so, first add all the embedded modules that are not already running to the load order */
		TRIS_LIST_TRAVERSE(&module_list, mod, entry) {
			/* if it's not embedded, skip it */
			if (mod->lib)
				continue;

			if (mod->flags.running)
				continue;

			order = add_to_load_order(mod->resource, &load_order);
		}

#ifdef LOADABLE_MODULES
		/* if we are allowed to load dynamic modules, scan the directory for
		   for all available modules and add them as well */
		if ((dir  = opendir(tris_config_TRIS_MODULE_DIR))) {
			while ((dirent = readdir(dir))) {
				int ld = strlen(dirent->d_name);

				/* Must end in .so to load it.  */

				if (ld < 4)
					continue;

				if (strcasecmp(dirent->d_name + ld - 3, ".so"))
					continue;

				/* if there is already a module by this name in the module_list,
				   skip this file */
				if (find_resource(dirent->d_name, 0))
					continue;

				add_to_load_order(dirent->d_name, &load_order);
			}

			closedir(dir);
		} else {
			if (!tris_opt_quiet)
				tris_log(LOG_WARNING, "Unable to open modules directory '%s'.\n",
					tris_config_TRIS_MODULE_DIR);
		}
#endif
	}

	/* now scan the config for any modules we are prohibited from loading and
	   remove them from the load order */
	for (v = tris_variable_browse(cfg, "modules"); v; v = v->next) {
		if (strcasecmp(v->name, "noload"))
			continue;

		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&load_order, order, entry) {
			if (!resource_name_match(order->resource, v->value)) {
				TRIS_LIST_REMOVE_CURRENT(entry);
				tris_free(order->resource);
				tris_free(order);
			}
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
	}

	/* we are done with the config now, all the information we need is in the
	   load_order list */
	tris_config_destroy(cfg);

	load_count = 0;
	TRIS_LIST_TRAVERSE(&load_order, order, entry)
		load_count++;

	if (load_count)
		tris_log(LOG_NOTICE, "%d modules will be loaded.\n", load_count);

	/* first, load only modules that provide global symbols */
	if ((res = load_resource_list(&load_order, 1, &modulecount)) < 0) {
		goto done;
	}

	/* now load everything else */
	if ((res = load_resource_list(&load_order, 0, &modulecount)) < 0) {
		goto done;
	}

done:
	while ((order = TRIS_LIST_REMOVE_HEAD(&load_order, entry))) {
		tris_free(order->resource);
		tris_free(order);
	}

	TRIS_LIST_UNLOCK(&module_list);
	
	/* Tell manager clients that are aggressive at logging in that we're done
	   loading modules. If there's a DNS problem in chan_sip, we might not
	   even reach this */
	manager_event(EVENT_FLAG_SYSTEM, "ModuleLoadReport", "ModuleLoadStatus: Done\r\nModuleSelection: %s\r\nModuleCount: %d\r\n", preload_only ? "Preload" : "All", modulecount);
	
	return res;
}

void tris_update_use_count(void)
{
	/* Notify any module monitors that the use count for a
	   resource has changed */
	struct loadupdate *m;

	TRIS_LIST_LOCK(&updaters);
	TRIS_LIST_TRAVERSE(&updaters, m, entry)
		m->updater();
	TRIS_LIST_UNLOCK(&updaters);
}

int tris_update_module_list(int (*modentry)(const char *module, const char *description, int usecnt, const char *like),
			   const char *like)
{
	struct tris_module *cur;
	int unlock = -1;
	int total_mod_loaded = 0;

	if (TRIS_LIST_TRYLOCK(&module_list))
		unlock = 0;
 
	TRIS_LIST_TRAVERSE(&module_list, cur, entry) {
		total_mod_loaded += modentry(cur->resource, cur->info->description, cur->usecount, like);
	}

	if (unlock)
		TRIS_LIST_UNLOCK(&module_list);

	return total_mod_loaded;
}

/*! \brief Check if module exists */
int tris_module_check(const char *name)
{
	struct tris_module *cur;

	if (tris_strlen_zero(name))
		return 0;       /* FALSE */

	cur = find_resource(name, 1);

	return (cur != NULL);
}


int tris_loader_register(int (*v)(void))
{
	struct loadupdate *tmp;

	if (!(tmp = tris_malloc(sizeof(*tmp))))
		return -1;

	tmp->updater = v;
	TRIS_LIST_LOCK(&updaters);
	TRIS_LIST_INSERT_HEAD(&updaters, tmp, entry);
	TRIS_LIST_UNLOCK(&updaters);

	return 0;
}

int tris_loader_unregister(int (*v)(void))
{
	struct loadupdate *cur;

	TRIS_LIST_LOCK(&updaters);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&updaters, cur, entry) {
		if (cur->updater == v)	{
			TRIS_LIST_REMOVE_CURRENT(entry);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&updaters);

	return cur ? 0 : -1;
}

struct tris_module *tris_module_ref(struct tris_module *mod)
{
	tris_atomic_fetchadd_int(&mod->usecount, +1);
	tris_update_use_count();

	return mod;
}

void tris_module_unref(struct tris_module *mod)
{
	tris_atomic_fetchadd_int(&mod->usecount, -1);
	tris_update_use_count();
}
