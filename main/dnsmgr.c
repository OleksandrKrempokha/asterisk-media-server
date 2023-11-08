/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005-2006, Kevin P. Fleming
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
 * \brief Background DNS update manager
 *
 * \author Kevin P. Fleming <kpfleming@digium.com> 
 *
 * \bug There is a minor race condition.  In the event that an IP address
 * of a dnsmgr managed host changes, there is the potential for the consumer
 * of that address to access the in_addr data at the same time that the dnsmgr
 * thread is in the middle of updating it to the new address.
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/_private.h"
#include <regex.h>
#include <signal.h>

#include "trismedia/dnsmgr.h"
#include "trismedia/linkedlists.h"
#include "trismedia/utils.h"
#include "trismedia/config.h"
#include "trismedia/sched.h"
#include "trismedia/cli.h"
#include "trismedia/manager.h"
#include "trismedia/acl.h"

static struct sched_context *sched;
static int refresh_sched = -1;
static pthread_t refresh_thread = TRIS_PTHREADT_NULL;

struct tris_dnsmgr_entry {
	/*! where we will store the resulting IP address and port number */
	struct sockaddr_in *result;
	/*! the last result, used to check if address/port has changed */
	struct sockaddr_in last;
	/*! SRV record to lookup, if provided. Composed of service, protocol, and domain name: _Service._Proto.Name */
	char *service;
	/*! Set to 1 if the entry changes */
	unsigned int changed:1;
	tris_mutex_t lock;
	TRIS_RWLIST_ENTRY(tris_dnsmgr_entry) list;
	/*! just 1 here, but we use calloc to allocate the correct size */
	char name[1];
};

static TRIS_RWLIST_HEAD_STATIC(entry_list, tris_dnsmgr_entry);

TRIS_MUTEX_DEFINE_STATIC(refresh_lock);

#define REFRESH_DEFAULT 300

static int enabled;
static int refresh_interval;

struct refresh_info {
	struct entry_list *entries;
	int verbose;
	unsigned int regex_present:1;
	regex_t filter;
};

static struct refresh_info master_refresh_info = {
	.entries = &entry_list,
	.verbose = 0,
};

struct tris_dnsmgr_entry *tris_dnsmgr_get(const char *name, struct sockaddr_in *result, const char *service)
{
	struct tris_dnsmgr_entry *entry;
	int total_size = sizeof(*entry) + strlen(name) + (service ? strlen(service) + 1 : 0);

	if (!result || tris_strlen_zero(name) || !(entry = tris_calloc(1, total_size)))
		return NULL;

	entry->result = result;
	tris_mutex_init(&entry->lock);
	strcpy(entry->name, name);
	memcpy(&entry->last, result, sizeof(entry->last));
	if (service) {
		entry->service = ((char *) entry) + sizeof(*entry) + strlen(name);
		strcpy(entry->service, service);
	}

	TRIS_RWLIST_WRLOCK(&entry_list);
	TRIS_RWLIST_INSERT_HEAD(&entry_list, entry, list);
	TRIS_RWLIST_UNLOCK(&entry_list);

	return entry;
}

void tris_dnsmgr_release(struct tris_dnsmgr_entry *entry)
{
	if (!entry)
		return;

	TRIS_RWLIST_WRLOCK(&entry_list);
	TRIS_RWLIST_REMOVE(&entry_list, entry, list);
	TRIS_RWLIST_UNLOCK(&entry_list);
	tris_verb(4, "removing dns manager for '%s'\n", entry->name);

	tris_mutex_destroy(&entry->lock);
	tris_free(entry);
}

int tris_dnsmgr_lookup(const char *name, struct sockaddr_in *result, struct tris_dnsmgr_entry **dnsmgr, const char *service)
{
	if (tris_strlen_zero(name) || !result || !dnsmgr)
		return -1;

	if (*dnsmgr && !strcasecmp((*dnsmgr)->name, name))
		return 0;

	/* if it's actually an IP address and not a name,
	   there's no need for a managed lookup */
	if (inet_aton(name, &result->sin_addr))
		return 0;

	tris_verb(4, "doing dnsmgr_lookup for '%s'\n", name);

	/* do a lookup now but add a manager so it will automagically get updated in the background */
	tris_get_ip_or_srv(result, name, service);
	
	/* if dnsmgr is not enable don't bother adding an entry */
	if (!enabled)
		return 0;
	
	tris_verb(3, "adding dns manager for '%s'\n", name);
	*dnsmgr = tris_dnsmgr_get(name, result, service);
	return !*dnsmgr;
}

/*
 * Refresh a dnsmgr entry
 */
static int dnsmgr_refresh(struct tris_dnsmgr_entry *entry, int verbose)
{
	char iabuf[INET_ADDRSTRLEN];
	char iabuf2[INET_ADDRSTRLEN];
	struct sockaddr_in tmp;
	int changed = 0;
        
	tris_mutex_lock(&entry->lock);
	if (verbose)
		tris_verb(3, "refreshing '%s'\n", entry->name);

	tmp.sin_port = entry->last.sin_port;
	
	if (!tris_get_ip_or_srv(&tmp, entry->name, entry->service) && inaddrcmp(&tmp, &entry->last)) {
		tris_copy_string(iabuf, tris_inet_ntoa(entry->last.sin_addr), sizeof(iabuf));
		tris_copy_string(iabuf2, tris_inet_ntoa(tmp.sin_addr), sizeof(iabuf2));
		tris_log(LOG_NOTICE, "dnssrv: host '%s' changed from %s:%d to %s:%d\n", 
			entry->name, iabuf, ntohs(entry->last.sin_port), iabuf2, ntohs(tmp.sin_port));
		*entry->result = tmp;
		entry->last = tmp;
		changed = entry->changed = 1;
	}

	tris_mutex_unlock(&entry->lock);
	return changed;
}

int tris_dnsmgr_refresh(struct tris_dnsmgr_entry *entry)
{
	return dnsmgr_refresh(entry, 0);
}

/*
 * Check if dnsmgr entry has changed from since last call to this function
 */
int tris_dnsmgr_changed(struct tris_dnsmgr_entry *entry) 
{
	int changed;

	tris_mutex_lock(&entry->lock);

	changed = entry->changed;
	entry->changed = 0;

	tris_mutex_unlock(&entry->lock);
	
	return changed;
}

static void *do_refresh(void *data)
{
	for (;;) {
		pthread_testcancel();
		usleep((tris_sched_wait(sched)*1000));
		pthread_testcancel();
		tris_sched_runq(sched);
	}
	return NULL;
}

static int refresh_list(const void *data)
{
	struct refresh_info *info = (struct refresh_info *)data;
	struct tris_dnsmgr_entry *entry;

	/* if a refresh or reload is already in progress, exit now */
	if (tris_mutex_trylock(&refresh_lock)) {
		if (info->verbose)
			tris_log(LOG_WARNING, "DNS Manager refresh already in progress.\n");
		return -1;
	}

	tris_verb(3, "Refreshing DNS lookups.\n");
	TRIS_RWLIST_RDLOCK(info->entries);
	TRIS_RWLIST_TRAVERSE(info->entries, entry, list) {
		if (info->regex_present && regexec(&info->filter, entry->name, 0, NULL, 0))
		    continue;

		dnsmgr_refresh(entry, info->verbose);
	}
	TRIS_RWLIST_UNLOCK(info->entries);

	tris_mutex_unlock(&refresh_lock);

	/* automatically reschedule based on the interval */
	return refresh_interval * 1000;
}

void dnsmgr_start_refresh(void)
{
	if (refresh_sched > -1) {
		TRIS_SCHED_DEL(sched, refresh_sched);
		refresh_sched = tris_sched_add_variable(sched, 100, refresh_list, &master_refresh_info, 1);
	}
}

static int do_reload(int loading);

static char *handle_cli_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "dnsmgr reload";
		e->usage = 
			"Usage: dnsmgr reload\n"
			"       Reloads the DNS manager configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	if (a->argc > 2)
		return CLI_SHOWUSAGE;

	do_reload(0);
	return CLI_SUCCESS;
}

static char *handle_cli_refresh(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct refresh_info info = {
		.entries = &entry_list,
		.verbose = 1,
	};
	switch (cmd) {
	case CLI_INIT:
		e->command = "dnsmgr refresh";
		e->usage = 
			"Usage: dnsmgr refresh [pattern]\n"
			"       Peforms an immediate refresh of the managed DNS entries.\n"
			"       Optional regular expression pattern is used to filter the entries to refresh.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if (!enabled) {
		tris_cli(a->fd, "DNS Manager is disabled.\n");
		return 0;
	}

	if (a->argc > 3) {
		return CLI_SHOWUSAGE;
	}

	if (a->argc == 3) {
		if (regcomp(&info.filter, a->argv[2], REG_EXTENDED | REG_NOSUB)) {
			return CLI_SHOWUSAGE;
		} else {
			info.regex_present = 1;
		}
	}

	refresh_list(&info);

	if (info.regex_present) {
		regfree(&info.filter);
	}

	return CLI_SUCCESS;
}

static char *handle_cli_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int count = 0;
	struct tris_dnsmgr_entry *entry;
	switch (cmd) {
	case CLI_INIT:
		e->command = "dnsmgr status";
		e->usage = 
			"Usage: dnsmgr status\n"
			"       Displays the DNS manager status.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if (a->argc > 2)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "DNS Manager: %s\n", enabled ? "enabled" : "disabled");
	tris_cli(a->fd, "Refresh Interval: %d seconds\n", refresh_interval);
	TRIS_RWLIST_RDLOCK(&entry_list);
	TRIS_RWLIST_TRAVERSE(&entry_list, entry, list)
		count++;
	TRIS_RWLIST_UNLOCK(&entry_list);
	tris_cli(a->fd, "Number of entries: %d\n", count);

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_reload = TRIS_CLI_DEFINE(handle_cli_reload, "Reloads the DNS manager configuration");
static struct tris_cli_entry cli_refresh = TRIS_CLI_DEFINE(handle_cli_refresh, "Performs an immediate refresh");
static struct tris_cli_entry cli_status = TRIS_CLI_DEFINE(handle_cli_status, "Display the DNS manager status");

int dnsmgr_init(void)
{
	if (!(sched = sched_context_create())) {
		tris_log(LOG_ERROR, "Unable to create schedule context.\n");
		return -1;
	}
	tris_cli_register(&cli_reload);
	tris_cli_register(&cli_status);
	tris_cli_register(&cli_refresh);
	return do_reload(1);
}

int dnsmgr_reload(void)
{
	return do_reload(0);
}

static int do_reload(int loading)
{
	struct tris_config *config;
	struct tris_flags config_flags = { loading ? 0 : CONFIG_FLAG_FILEUNCHANGED };
	const char *interval_value;
	const char *enabled_value;
	int interval;
	int was_enabled;
	int res = -1;

	config = tris_config_load2("dnsmgr.conf", "dnsmgr", config_flags);
	if (config == CONFIG_STATUS_FILEMISSING || config == CONFIG_STATUS_FILEUNCHANGED || config == CONFIG_STATUS_FILEINVALID) {
		return 0;
	}

	/* ensure that no refresh cycles run while the reload is in progress */
	tris_mutex_lock(&refresh_lock);

	/* reset defaults in preparation for reading config file */
	refresh_interval = REFRESH_DEFAULT;
	was_enabled = enabled;
	enabled = 0;

	TRIS_SCHED_DEL(sched, refresh_sched);

	if (config) {
		if ((enabled_value = tris_variable_retrieve(config, "general", "enable"))) {
			enabled = tris_true(enabled_value);
		}
		if ((interval_value = tris_variable_retrieve(config, "general", "refreshinterval"))) {
			if (sscanf(interval_value, "%30d", &interval) < 1)
				tris_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", interval_value);
			else if (interval < 0)
				tris_log(LOG_WARNING, "Invalid refresh interval '%d' specified, using default\n", interval);
			else
				refresh_interval = interval;
		}
		tris_config_destroy(config);
	}

	if (enabled && refresh_interval)
		tris_log(LOG_NOTICE, "Managed DNS entries will be refreshed every %d seconds.\n", refresh_interval);

	/* if this reload enabled the manager, create the background thread
	   if it does not exist */
	if (enabled) {
		if (!was_enabled && (refresh_thread == TRIS_PTHREADT_NULL)) {
			if (tris_pthread_create_background(&refresh_thread, NULL, do_refresh, NULL) < 0) {
				tris_log(LOG_ERROR, "Unable to start refresh thread.\n");
			}
		}
		/* make a background refresh happen right away */
		refresh_sched = tris_sched_add_variable(sched, 100, refresh_list, &master_refresh_info, 1);
		res = 0;
	}
	/* if this reload disabled the manager and there is a background thread,
	   kill it */
	else if (!enabled && was_enabled && (refresh_thread != TRIS_PTHREADT_NULL)) {
		/* wake up the thread so it will exit */
		pthread_cancel(refresh_thread);
		pthread_kill(refresh_thread, SIGURG);
		pthread_join(refresh_thread, NULL);
		refresh_thread = TRIS_PTHREADT_NULL;
		res = 0;
	}
	else
		res = 0;

	tris_mutex_unlock(&refresh_lock);
	manager_event(EVENT_FLAG_SYSTEM, "Reload", "Module: DNSmgr\r\nStatus: %s\r/nMessage: DNSmgr reload Requested\r\n", enabled ? "Enabled" : "Disabled");

	return res;
}
