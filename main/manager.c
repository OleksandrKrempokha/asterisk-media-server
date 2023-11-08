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
 * \brief The Trismedia Management Interface - AMI
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \extref OpenSSL http://www.openssl.org - for AMI/SSL 
 *
 * At the moment this file contains a number of functions, namely:
 *
 * - data structures storing AMI state
 * - AMI-related API functions, used by internal trismedia components
 * - handlers for AMI-related CLI functions
 * - handlers for AMI functions (available through the AMI socket)
 * - the code for the main AMI listener thread and individual session threads
 * - the http handlers invoked for AMI-over-HTTP by the threads in main/http.c
 *
 * \ref amiconf
 */

/*! \addtogroup Group_AMI AMI functions
*/
/*! @{
 Doxygen group */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 243989 $")

#include "trismedia/_private.h"
#include "trismedia/paths.h"	/* use various tris_config_TRIS_* */
#include <ctype.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/mman.h>

#include "trismedia/channel.h"
#include "trismedia/file.h"
#include "trismedia/manager.h"
#include "trismedia/module.h"
#include "trismedia/config.h"
#include "trismedia/callerid.h"
#include "trismedia/lock.h"
#include "trismedia/cli.h"
#include "trismedia/app.h"
#include "trismedia/pbx.h"
#include "trismedia/md5.h"
#include "trismedia/acl.h"
#include "trismedia/utils.h"
#include "trismedia/tcptls.h"
#include "trismedia/http.h"
#include "trismedia/tris_version.h"
#include "trismedia/threadstorage.h"
#include "trismedia/linkedlists.h"
#include "trismedia/version.h"
#include "trismedia/term.h"
#include "trismedia/astobj2.h"
#include "trismedia/features.h"


enum error_type {
	UNKNOWN_ACTION = 1,
	UNKNOWN_CATEGORY,
	UNSPECIFIED_CATEGORY,
	UNSPECIFIED_ARGUMENT,
	FAILURE_ALLOCATION,
	FAILURE_NEWCAT,
	FAILURE_DELCAT,
	FAILURE_EMPTYCAT,
	FAILURE_UPDATE,
	FAILURE_DELETE,
	FAILURE_APPEND
};


/*!
 * Linked list of events.
 * Global events are appended to the list by append_event().
 * The usecount is the number of stored pointers to the element,
 * excluding the list pointers. So an element that is only in
 * the list has a usecount of 0, not 1.
 *
 * Clients have a pointer to the last event processed, and for each
 * of these clients we track the usecount of the elements.
 * If we have a pointer to an entry in the list, it is safe to navigate
 * it forward because elements will not be deleted, but only appended.
 * The worst that can happen is seeing the pointer still NULL.
 *
 * When the usecount of an element drops to 0, and the element is the
 * first in the list, we can remove it. Removal is done within the
 * main thread, which is woken up for the purpose.
 *
 * For simplicity of implementation, we make sure the list is never empty.
 */
struct eventqent {
	int usecount;		/*!< # of clients who still need the event */
	int category;
	unsigned int seq;	/*!< sequence number */
	TRIS_LIST_ENTRY(eventqent) eq_next;
	char eventdata[1];	/*!< really variable size, allocated by append_event() */
};

static TRIS_LIST_HEAD_STATIC(all_events, eventqent);

static int displayconnects = 1;
static int allowmultiplelogin = 1;
static int timestampevents;
static int httptimeout = 60;
static int manager_enabled = 0;
static int webmanager_enabled = 0;

static int block_sockets;
static int num_sessions;

static int manager_debug;	/*!< enable some debugging code in the manager */

/*! \brief
 * Descriptor for a manager session, either on the AMI socket or over HTTP.
 *
 * \note
 * AMI session have managerid == 0; the entry is created upon a connect,
 * and destroyed with the socket.
 * HTTP sessions have managerid != 0, the value is used as a search key
 * to lookup sessions (using the mansession_id cookie).
 */
#define MAX_BLACKLIST_CMD_LEN 2
static struct {
	char *words[TRIS_MAX_CMD_LEN];
} command_blacklist[] = {
	{{ "module", "load", NULL }},
	{{ "module", "unload", NULL }},
	{{ "restart", "gracefully", NULL }},
};

/* In order to understand what the heck is going on with the
 * mansession_session and mansession structs, we need to have a bit of a history
 * lesson.
 *
 * In the beginning, there was the mansession. The mansession contained data that was
 * intrinsic to a manager session, such as the time that it started, the name of the logged-in
 * user, etc. In addition to these parameters were the f and fd parameters. For typical manager
 * sessions, these were used to represent the TCP socket over which the AMI session was taking
 * place. It makes perfect sense for these fields to be a part of the session-specific data since
 * the session actually defines this information.
 *
 * Then came the HTTP AMI sessions. With these, the f and fd fields need to be opened and closed
 * for every single action that occurs. Thus the f and fd fields aren't really specific to the session
 * but rather to the action that is being executed. Because a single session may execute many commands
 * at once, some sort of safety needed to be added in order to be sure that we did not end up with fd
 * leaks from one action overwriting the f and fd fields used by a previous action before the previous action
 * has had a chance to properly close its handles.
 *
 * The initial idea to solve this was to use thread synchronization, but this prevented multiple actions
 * from being run at the same time in a single session. Some manager actions may block for a long time, thus
 * creating a large queue of actions to execute. In addition, this fix did not address the basic architectural
 * issue that for HTTP manager sessions, the f and fd variables are not really a part of the session, but are
 * part of the action instead.
 *
 * The new idea was to create a structure on the stack for each HTTP Manager action. This structure would
 * contain the action-specific information, such as which file to write to. In order to maintain expectations
 * of action handlers and not have to change the public API of the manager code, we would need to name this
 * new stacked structure 'mansession' and contain within it the old mansession struct that we used to use.
 * We renamed the old mansession struct 'mansession_session' to hopefully convey that what is in this structure
 * is session-specific data. The structure that it is wrapped in, called a 'mansession' really contains action-specific
 * data.
 */
struct mansession_session {
	pthread_t ms_t;		/*!< Execution thread, basically useless */
	tris_mutex_t __lock;	/*!< Thread lock -- don't use in action callbacks, it's already taken care of  */
				/* XXX need to document which fields it is protecting */
	struct sockaddr_in sin;	/*!< address we are connecting from */
	FILE *f;		/*!< fdopen() on the underlying fd */
	int fd;			/*!< descriptor used for output. Either the socket (AMI) or a temporary file (HTTP) */
	int inuse;		/*!< number of HTTP sessions using this entry */
	int needdestroy;	/*!< Whether an HTTP session should be destroyed */
	pthread_t waiting_thread;	/*!< Sleeping thread using this descriptor */
	uint32_t managerid;	/*!< Unique manager identifier, 0 for AMI sessions */
	time_t sessionstart;    /*!< Session start time */
	time_t sessiontimeout;	/*!< Session timeout if HTTP */
	char username[80];	/*!< Logged in username */
	char challenge[10];	/*!< Authentication challenge */
	int authenticated;	/*!< Authentication status */
	int readperm;		/*!< Authorization for reading */
	int writeperm;		/*!< Authorization for writing */
	char inbuf[1025];	/*!< Buffer */
				/* we use the extra byte to add a '\0' and simplify parsing */
	int inlen;		/*!< number of buffered bytes */
	int send_events;	/*!<  XXX what ? */
	struct eventqent *last_ev;	/*!< last event processed. */
	int writetimeout;	/*!< Timeout for tris_carefulwrite() */
	int pending_event;         /*!< Pending events indicator in case when waiting_thread is NULL */
	TRIS_LIST_HEAD_NOLOCK(mansession_datastores, tris_datastore) datastores; /*!< Data stores on the session */
	TRIS_LIST_ENTRY(mansession_session) list;
};

/* In case you didn't read that giant block of text above the mansession_session struct, the
 * 'mansession' struct is named this solely to keep the API the same in Trismedia. This structure really
 * represents data that is different from Manager action to Manager action. The mansession_session pointer
 * contained within points to session-specific data.
 */
struct mansession {
	struct mansession_session *session;
	FILE *f;
	int fd;
};

#define NEW_EVENT(m)	(TRIS_LIST_NEXT(m->session->last_ev, eq_next))

static TRIS_LIST_HEAD_STATIC(sessions, mansession_session);

/*! \brief user descriptor, as read from the config file.
 *
 * \note It is still missing some fields -- e.g. we can have multiple permit and deny
 * lines which are not supported here, and readperm/writeperm/writetimeout
 * are not stored.
 */
struct tris_manager_user {
	char username[80];
	char *secret;
	struct tris_ha *ha;		/*!< ACL setting */
	int readperm;			/*! Authorization for reading */
	int writeperm;			/*! Authorization for writing */
	int writetimeout;		/*! Per user Timeout for tris_carefulwrite() */
	int displayconnects;	/*!< XXX unused */
	int keep;	/*!< mark entries created on a reload */
	TRIS_RWLIST_ENTRY(tris_manager_user) list;
};

/*! \brief list of users found in the config file */
static TRIS_RWLIST_HEAD_STATIC(users, tris_manager_user);

/*! \brief list of actions registered */
static TRIS_RWLIST_HEAD_STATIC(actions, manager_action);

/*! \brief list of hooks registered */
static TRIS_RWLIST_HEAD_STATIC(manager_hooks, manager_custom_hook);

/*! \brief Add a custom hook to be called when an event is fired */
void tris_manager_register_hook(struct manager_custom_hook *hook)
{
	TRIS_RWLIST_WRLOCK(&manager_hooks);
	TRIS_RWLIST_INSERT_TAIL(&manager_hooks, hook, list);
	TRIS_RWLIST_UNLOCK(&manager_hooks);
	return;
}

/*! \brief Delete a custom hook to be called when an event is fired */
void tris_manager_unregister_hook(struct manager_custom_hook *hook)
{
	TRIS_RWLIST_WRLOCK(&manager_hooks);
	TRIS_RWLIST_REMOVE(&manager_hooks, hook, list);
	TRIS_RWLIST_UNLOCK(&manager_hooks);
	return;
}

/*! \brief
 * Event list management functions.
 * We assume that the event list always has at least one element,
 * and the delete code will not remove the last entry even if the
 * 
 */
#if 0
static time_t __deb(time_t start, const char *msg)
{
	time_t now = time(NULL);
	tris_verbose("%4d th %p %s\n", (int)(now % 3600), pthread_self(), msg);
	if (start != 0 && now - start > 5)
		tris_verbose("+++ WOW, %s took %d seconds\n", msg, (int)(now - start));
	return now;
}

static void LOCK_EVENTS(void)
{
	time_t start = __deb(0, "about to lock events");
	TRIS_LIST_LOCK(&all_events);
	__deb(start, "done lock events");
}

static void UNLOCK_EVENTS(void)
{
	__deb(0, "about to unlock events");
	TRIS_LIST_UNLOCK(&all_events);
}

static void LOCK_SESS(void)
{
	time_t start = __deb(0, "about to lock sessions");
	TRIS_LIST_LOCK(&sessions);
	__deb(start, "done lock sessions");
}

static void UNLOCK_SESS(void)
{
	__deb(0, "about to unlock sessions");
	TRIS_LIST_UNLOCK(&sessions);
}
#endif

int check_manager_enabled()
{
	return manager_enabled;
}

int check_webmanager_enabled()
{
	return (webmanager_enabled && manager_enabled);
}

/*!
 * Grab a reference to the last event, update usecount as needed.
 * Can handle a NULL pointer.
 */
static struct eventqent *grab_last(void)
{
	struct eventqent *ret;

	TRIS_LIST_LOCK(&all_events);
	ret = TRIS_LIST_LAST(&all_events);
	/* the list is never empty now, but may become so when
	 * we optimize it in the future, so be prepared.
	 */
	if (ret)
		tris_atomic_fetchadd_int(&ret->usecount, 1);
	TRIS_LIST_UNLOCK(&all_events);
	return ret;
}

/*!
 * Purge unused events. Remove elements from the head
 * as long as their usecount is 0 and there is a next element.
 */
static void purge_events(void)
{
	struct eventqent *ev;

	TRIS_LIST_LOCK(&all_events);
	while ( (ev = TRIS_LIST_FIRST(&all_events)) &&
	    ev->usecount == 0 && TRIS_LIST_NEXT(ev, eq_next)) {
		TRIS_LIST_REMOVE_HEAD(&all_events, eq_next);
		tris_free(ev);
	}
	TRIS_LIST_UNLOCK(&all_events);
}

/*!
 * helper functions to convert back and forth between
 * string and numeric representation of set of flags
 */
static struct permalias {
	int num;
	char *label;
} perms[] = {
	{ EVENT_FLAG_SYSTEM, "system" },
	{ EVENT_FLAG_CALL, "call" },
	{ EVENT_FLAG_LOG, "log" },
	{ EVENT_FLAG_VERBOSE, "verbose" },
	{ EVENT_FLAG_COMMAND, "command" },
	{ EVENT_FLAG_AGENT, "agent" },
	{ EVENT_FLAG_USER, "user" },
	{ EVENT_FLAG_CONFIG, "config" },
	{ EVENT_FLAG_DTMF, "dtmf" },
	{ EVENT_FLAG_REPORTING, "reporting" },
	{ EVENT_FLAG_CDR, "cdr" },
	{ EVENT_FLAG_DIALPLAN, "dialplan" },
	{ EVENT_FLAG_ORIGINATE, "originate" },
	{ EVENT_FLAG_AGI, "agi" },
	{ INT_MAX, "all" },
	{ 0, "none" },
};

/*! \brief Convert authority code to a list of options */
static char *authority_to_str(int authority, struct tris_str **res)
{
	int i;
	char *sep = "";

	tris_str_reset(*res);
	for (i = 0; i < ARRAY_LEN(perms) - 1; i++) {
		if (authority & perms[i].num) {
			tris_str_append(res, 0, "%s%s", sep, perms[i].label);
			sep = ",";
		}
	}

	if (tris_str_strlen(*res) == 0)	/* replace empty string with something sensible */
		tris_str_append(res, 0, "<none>");

	return tris_str_buffer(*res);
}

/*! Tells you if smallstr exists inside bigstr
   which is delim by delim and uses no buf or stringsep
   tris_instring("this|that|more","this",'|') == 1;

   feel free to move this to app.c -anthm */
static int tris_instring(const char *bigstr, const char *smallstr, const char delim)
{
	const char *val = bigstr, *next;

	do {
		if ((next = strchr(val, delim))) {
			if (!strncmp(val, smallstr, (next - val)))
				return 1;
			else
				continue;
		} else
			return !strcmp(smallstr, val);
	} while (*(val = (next + 1)));

	return 0;
}

static int get_perm(const char *instr)
{
	int x = 0, ret = 0;

	if (!instr)
		return 0;

	for (x = 0; x < ARRAY_LEN(perms); x++) {
		if (tris_instring(instr, perms[x].label, ','))
			ret |= perms[x].num;
	}

	return ret;
}

/*!
 * A number returns itself, false returns 0, true returns all flags,
 * other strings return the flags that are set.
 */
static int strings_to_mask(const char *string)
{
	const char *p;

	if (tris_strlen_zero(string))
		return -1;

	for (p = string; *p; p++)
		if (*p < '0' || *p > '9')
			break;
	if (!*p) /* all digits */
		return atoi(string);
	if (tris_false(string))
		return 0;
	if (tris_true(string)) {	/* all permissions */
		int x, ret = 0;
		for (x = 0; x < ARRAY_LEN(perms); x++)
			ret |= perms[x].num;
		return ret;
	}
	return get_perm(string);
}

static int check_manager_session_inuse(const char *name)
{
	struct mansession_session *session = NULL;

	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_TRAVERSE(&sessions, session, list) {
		if (!strcasecmp(session->username, name)) 
			break;
	}
	TRIS_LIST_UNLOCK(&sessions);

	return session ? 1 : 0;
}


/*!
 * lookup an entry in the list of registered users.
 * must be called with the list lock held.
 */
static struct tris_manager_user *get_manager_by_name_locked(const char *name)
{
	struct tris_manager_user *user = NULL;

	TRIS_RWLIST_TRAVERSE(&users, user, list)
		if (!strcasecmp(user->username, name))
			break;
	return user;
}

/*! \brief Get displayconnects config option.
 *  \param session manager session to get parameter from.
 *  \return displayconnects config option value.
 */
static int manager_displayconnects (struct mansession_session *session)
{
	struct tris_manager_user *user = NULL;
	int ret = 0;

	TRIS_RWLIST_RDLOCK(&users);
	if ((user = get_manager_by_name_locked (session->username)))
		ret = user->displayconnects;
	TRIS_RWLIST_UNLOCK(&users);
	
	return ret;
}

static char *handle_showmancmd(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct manager_action *cur;
	struct tris_str *authority;
	int num, l, which;
	char *ret = NULL;
	switch (cmd) {
	case CLI_INIT:
		e->command = "manager show command";
		e->usage = 
			"Usage: manager show command <actionname> [<actionname> [<actionname> [...]]]\n"
			"	Shows the detailed description for a specific Trismedia manager interface command.\n";
		return NULL;
	case CLI_GENERATE:
		l = strlen(a->word);
		which = 0;
		TRIS_RWLIST_RDLOCK(&actions);
		TRIS_RWLIST_TRAVERSE(&actions, cur, list) {
			if (!strncasecmp(a->word, cur->action, l) && ++which > a->n) {
				ret = tris_strdup(cur->action);
				break;	/* make sure we exit even if tris_strdup() returns NULL */
			}
		}
		TRIS_RWLIST_UNLOCK(&actions);
		return ret;
	}
	authority = tris_str_alloca(80);
	if (a->argc < 4) {
		return CLI_SHOWUSAGE;
	}

	TRIS_RWLIST_RDLOCK(&actions);
	TRIS_RWLIST_TRAVERSE(&actions, cur, list) {
		for (num = 3; num < a->argc; num++) {
			if (!strcasecmp(cur->action, a->argv[num])) {
				tris_cli(a->fd, "Action: %s\nSynopsis: %s\nPrivilege: %s\n%s\n",
					cur->action, cur->synopsis,
					authority_to_str(cur->authority, &authority),
					S_OR(cur->description, ""));
			}
		}
	}
	TRIS_RWLIST_UNLOCK(&actions);

	return CLI_SUCCESS;
}

static char *handle_mandebug(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "manager set debug [on|off]";
		e->usage = "Usage: manager set debug [on|off]\n	Show, enable, disable debugging of the manager code.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	if (a->argc == 3)
		tris_cli(a->fd, "manager debug is %s\n", manager_debug? "on" : "off");
	else if (a->argc == 4) {
		if (!strcasecmp(a->argv[3], "on"))
			manager_debug = 1;
		else if (!strcasecmp(a->argv[3], "off"))
			manager_debug = 0;
		else
			return CLI_SHOWUSAGE;
	}
	return CLI_SUCCESS;
}

static char *handle_showmanager(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_manager_user *user = NULL;
	int l, which;
	char *ret = NULL;
	struct tris_str *rauthority = tris_str_alloca(128);
	struct tris_str *wauthority = tris_str_alloca(128);

	switch (cmd) {
	case CLI_INIT:
		e->command = "manager show user";
		e->usage = 
			" Usage: manager show user <user>\n"
			"        Display all information related to the manager user specified.\n";
		return NULL;
	case CLI_GENERATE:
		l = strlen(a->word);
		which = 0;
		if (a->pos != 3)
			return NULL;
		TRIS_RWLIST_RDLOCK(&users);
		TRIS_RWLIST_TRAVERSE(&users, user, list) {
			if ( !strncasecmp(a->word, user->username, l) && ++which > a->n ) {
				ret = tris_strdup(user->username);
				break;
			}
		}
		TRIS_RWLIST_UNLOCK(&users);
		return ret;
	}

	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	TRIS_RWLIST_RDLOCK(&users);

	if (!(user = get_manager_by_name_locked(a->argv[3]))) {
		tris_cli(a->fd, "There is no manager called %s\n", a->argv[3]);
		TRIS_RWLIST_UNLOCK(&users);
		return CLI_SUCCESS;
	}

	tris_cli(a->fd, "\n");
	tris_cli(a->fd,
		"       username: %s\n"
		"         secret: %s\n"
		"            acl: %s\n"
		"      read perm: %s\n"
		"     write perm: %s\n"
		"displayconnects: %s\n",
		(user->username ? user->username : "(N/A)"),
		(user->secret ? "<Set>" : "(N/A)"),
		(user->ha ? "yes" : "no"),
		authority_to_str(user->readperm, &rauthority),
		authority_to_str(user->writeperm, &wauthority),
		(user->displayconnects ? "yes" : "no"));

	TRIS_RWLIST_UNLOCK(&users);

	return CLI_SUCCESS;
}


static char *handle_showmanagers(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_manager_user *user = NULL;
	int count_amu = 0;
	switch (cmd) {
	case CLI_INIT:
		e->command = "manager show users";
		e->usage = 
			"Usage: manager show users\n"
			"       Prints a listing of all managers that are currently configured on that\n"
			" system.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	TRIS_RWLIST_RDLOCK(&users);

	/* If there are no users, print out something along those lines */
	if (TRIS_RWLIST_EMPTY(&users)) {
		tris_cli(a->fd, "There are no manager users.\n");
		TRIS_RWLIST_UNLOCK(&users);
		return CLI_SUCCESS;
	}

	tris_cli(a->fd, "\nusername\n--------\n");

	TRIS_RWLIST_TRAVERSE(&users, user, list) {
		tris_cli(a->fd, "%s\n", user->username);
		count_amu++;
	}

	TRIS_RWLIST_UNLOCK(&users);

	tris_cli(a->fd, "-------------------\n");
	tris_cli(a->fd, "%d manager users configured.\n", count_amu);

	return CLI_SUCCESS;
}


/*! \brief  CLI command  manager list commands */
static char *handle_showmancmds(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct manager_action *cur;
	struct tris_str *authority;
#define HSMC_FORMAT "  %-15.15s  %-15.15s  %-55.55s\n"
	switch (cmd) {
	case CLI_INIT:
		e->command = "manager show commands";
		e->usage = 
			"Usage: manager show commands\n"
			"	Prints a listing of all the available Trismedia manager interface commands.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}	
	authority = tris_str_alloca(80);
	tris_cli(a->fd, HSMC_FORMAT, "Action", "Privilege", "Synopsis");
	tris_cli(a->fd, HSMC_FORMAT, "------", "---------", "--------");

	TRIS_RWLIST_RDLOCK(&actions);
	TRIS_RWLIST_TRAVERSE(&actions, cur, list)
		tris_cli(a->fd, HSMC_FORMAT, cur->action, authority_to_str(cur->authority, &authority), cur->synopsis);
	TRIS_RWLIST_UNLOCK(&actions);

	return CLI_SUCCESS;
}

/*! \brief CLI command manager list connected */
static char *handle_showmanconn(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct mansession_session *session;
	time_t now = time(NULL);
#define HSMCONN_FORMAT1 "  %-15.15s  %-15.15s  %-10.10s  %-10.10s  %-8.8s  %-8.8s  %-5.5s  %-5.5s\n"
#define HSMCONN_FORMAT2 "  %-15.15s  %-15.15s  %-10d  %-10d  %-8d  %-8d  %-5.5d  %-5.5d\n"
	int count = 0;
	switch (cmd) {
	case CLI_INIT:
		e->command = "manager show connected";
		e->usage = 
			"Usage: manager show connected\n"
			"	Prints a listing of the users that are currently connected to the\n"
			"Trismedia manager interface.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	tris_cli(a->fd, HSMCONN_FORMAT1, "Username", "IP Address", "Start", "Elapsed", "FileDes", "HttpCnt", "Read", "Write");

	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_TRAVERSE(&sessions, session, list) {
		tris_cli(a->fd, HSMCONN_FORMAT2, session->username, tris_inet_ntoa(session->sin.sin_addr), (int)(session->sessionstart), (int)(now - session->sessionstart), session->fd, session->inuse, session->readperm, session->writeperm);
		count++;
	}
	TRIS_LIST_UNLOCK(&sessions);

	tris_cli(a->fd, "%d users connected.\n", count);

	return CLI_SUCCESS;
}

/*! \brief CLI command manager list eventq */
/* Should change to "manager show connected" */
static char *handle_showmaneventq(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct eventqent *s;
	switch (cmd) {
	case CLI_INIT:
		e->command = "manager show eventq";
		e->usage = 
			"Usage: manager show eventq\n"
			"	Prints a listing of all events pending in the Trismedia manger\n"
			"event queue.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	TRIS_LIST_LOCK(&all_events);
	TRIS_LIST_TRAVERSE(&all_events, s, eq_next) {
		tris_cli(a->fd, "Usecount: %d\n", s->usecount);
		tris_cli(a->fd, "Category: %d\n", s->category);
		tris_cli(a->fd, "Event:\n%s", s->eventdata);
	}
	TRIS_LIST_UNLOCK(&all_events);

	return CLI_SUCCESS;
}

/*! \brief CLI command manager reload */
static char *handle_manager_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "manager reload";
		e->usage =
			"Usage: manager reload\n"
			"       Reloads the manager configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc > 2)
		return CLI_SHOWUSAGE;
	reload_manager();
	return CLI_SUCCESS;
}


static struct tris_cli_entry cli_manager[] = {
	TRIS_CLI_DEFINE(handle_showmancmd, "Show a manager interface command"),
	TRIS_CLI_DEFINE(handle_showmancmds, "List manager interface commands"),
	TRIS_CLI_DEFINE(handle_showmanconn, "List connected manager interface users"),
	TRIS_CLI_DEFINE(handle_showmaneventq, "List manager interface queued events"),
	TRIS_CLI_DEFINE(handle_showmanagers, "List configured manager users"),
	TRIS_CLI_DEFINE(handle_showmanager, "Display information on a specific manager user"),
	TRIS_CLI_DEFINE(handle_mandebug, "Show, enable, disable debugging of the manager code"),
	TRIS_CLI_DEFINE(handle_manager_reload, "Reload manager configurations"),
};

/*
 * Decrement the usecount for the event; if it goes to zero,
 * (why check for e->next ?) wakeup the
 * main thread, which is in charge of freeing the record.
 * Returns the next record.
 */
static struct eventqent *unref_event(struct eventqent *e)
{
	tris_atomic_fetchadd_int(&e->usecount, -1);
	return TRIS_LIST_NEXT(e, eq_next);
}

static void ref_event(struct eventqent *e)
{
	tris_atomic_fetchadd_int(&e->usecount, 1);
}

/*
 * destroy a session, leaving the usecount
 */
static void free_session(struct mansession_session *session)
{
	struct eventqent *eqe = session->last_ev;
	struct tris_datastore *datastore;

	/* Get rid of each of the data stores on the session */
	while ((datastore = TRIS_LIST_REMOVE_HEAD(&session->datastores, entry))) {
		/* Free the data store */
		tris_datastore_free(datastore);
	}

	if (session->f != NULL)
		fclose(session->f);
	tris_mutex_destroy(&session->__lock);
	tris_free(session);
	unref_event(eqe);
}

static void destroy_session(struct mansession_session *session)
{
	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_REMOVE(&sessions, session, list);
	tris_atomic_fetchadd_int(&num_sessions, -1);
	free_session(session);
	TRIS_LIST_UNLOCK(&sessions);
}

/*
 * Generic function to return either the first or the last matching header
 * from a list of variables, possibly skipping empty strings.
 * At the moment there is only one use of this function in this file,
 * so we make it static.
 */
#define	GET_HEADER_FIRST_MATCH	0
#define	GET_HEADER_LAST_MATCH	1
#define	GET_HEADER_SKIP_EMPTY	2
static const char *__astman_get_header(const struct message *m, char *var, int mode)
{
	int x, l = strlen(var);
	const char *result = "";

	for (x = 0; x < m->hdrcount; x++) {
		const char *h = m->headers[x];
		if (!strncasecmp(var, h, l) && h[l] == ':' && h[l+1] == ' ') {
			const char *value = h + l + 2;
			/* found a potential candidate */
			if (mode & GET_HEADER_SKIP_EMPTY && tris_strlen_zero(value))
				continue;	/* not interesting */
			if (mode & GET_HEADER_LAST_MATCH)
				result = value;	/* record the last match so far */
			else
				return value;
		}
	}

	return "";
}

/*
 * Return the first matching variable from an array.
 * This is the legacy function and is implemented in therms of
 * __astman_get_header().
 */
const char *astman_get_header(const struct message *m, char *var)
{
	return __astman_get_header(m, var, GET_HEADER_FIRST_MATCH);
}


struct tris_variable *astman_get_variables(const struct message *m)
{
	int varlen, x, y;
	struct tris_variable *head = NULL, *cur;

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(vars)[32];
	);

	varlen = strlen("Variable: ");

	for (x = 0; x < m->hdrcount; x++) {
		char *parse, *var, *val;

		if (strncasecmp("Variable: ", m->headers[x], varlen))
			continue;
		parse = tris_strdupa(m->headers[x] + varlen);

		TRIS_STANDARD_APP_ARGS(args, parse);
		if (!args.argc)
			continue;
		for (y = 0; y < args.argc; y++) {
			if (!args.vars[y])
				continue;
			var = val = tris_strdupa(args.vars[y]);
			strsep(&val, "=");
			if (!val || tris_strlen_zero(var))
				continue;
			cur = tris_variable_new(var, val, "");
			cur->next = head;
			head = cur;
		}
	}

	return head;
}

/*!
 * helper function to send a string to the socket.
 * Return -1 on error (e.g. buffer full).
 */
static int send_string(struct mansession *s, char *string)
{
	if (s->f) {
		return tris_careful_fwrite(s->f, s->fd, string, strlen(string), s->session->writetimeout);
	} else {
		return tris_careful_fwrite(s->session->f, s->session->fd, string, strlen(string), s->session->writetimeout);
	}
}

/*!
 * \brief thread local buffer for astman_append
 *
 * \note This can not be defined within the astman_append() function
 *       because it declares a couple of functions that get used to
 *       initialize the thread local storage key.
 */
TRIS_THREADSTORAGE(astman_append_buf);
TRIS_THREADSTORAGE(userevent_buf);

/*! \brief initial allocated size for the astman_append_buf */
#define ASTMAN_APPEND_BUF_INITSIZE   256

/*!
 * utility functions for creating AMI replies
 */
void astman_append(struct mansession *s, const char *fmt, ...)
{
	va_list ap;
	struct tris_str *buf;

	if (!(buf = tris_str_thread_get(&astman_append_buf, ASTMAN_APPEND_BUF_INITSIZE)))
		return;

	va_start(ap, fmt);
	tris_str_set_va(&buf, 0, fmt, ap);
	va_end(ap);

	if (s->f != NULL || s->session->f != NULL) {
		send_string(s, tris_str_buffer(buf));
	} else {
		tris_verbose("fd == -1 in astman_append, should not happen\n");
	}
}

/*! \note NOTE: XXX this comment is unclear and possibly wrong.
   Callers of astman_send_error(), astman_send_response() or astman_send_ack() must EITHER
   hold the session lock _or_ be running in an action callback (in which case s->session->busy will
   be non-zero). In either of these cases, there is no need to lock-protect the session's
   fd, since no other output will be sent (events will be queued), and no input will
   be read until either the current action finishes or get_input() obtains the session
   lock.
 */

/*! \brief send a response with an optional message,
 * and terminate it with an empty line.
 * m is used only to grab the 'ActionID' field.
 *
 * Use the explicit constant MSG_MOREDATA to remove the empty line.
 * XXX MSG_MOREDATA should go to a header file.
 */
#define MSG_MOREDATA	((char *)astman_send_response)
static void astman_send_response_full(struct mansession *s, const struct message *m, char *resp, char *msg, char *listflag)
{
	const char *id = astman_get_header(m, "ActionID");

	astman_append(s, "Response: %s\r\n", resp);
	if (!tris_strlen_zero(id))
		astman_append(s, "ActionID: %s\r\n", id);
	if (listflag)
		astman_append(s, "Eventlist: %s\r\n", listflag);	/* Start, complete, cancelled */
	if (msg == MSG_MOREDATA)
		return;
	else if (msg)
		astman_append(s, "Message: %s\r\n\r\n", msg);
	else
		astman_append(s, "\r\n");
}

void astman_send_response(struct mansession *s, const struct message *m, char *resp, char *msg)
{
	astman_send_response_full(s, m, resp, msg, NULL);
}

void astman_send_error(struct mansession *s, const struct message *m, char *error)
{
	astman_send_response_full(s, m, "Error", error, NULL);
}

void astman_send_ack(struct mansession *s, const struct message *m, char *msg)
{
	astman_send_response_full(s, m, "Success", msg, NULL);
}

static void astman_start_ack(struct mansession *s, const struct message *m)
{
	astman_send_response_full(s, m, "Success", MSG_MOREDATA, NULL);
}

void astman_send_listack(struct mansession *s, const struct message *m, char *msg, char *listflag)
{
	astman_send_response_full(s, m, "Success", msg, listflag);
}


/*! \brief
   Rather than braindead on,off this now can also accept a specific int mask value
   or a ',' delim list of mask strings (the same as manager.conf) -anthm
*/
static int set_eventmask(struct mansession *s, const char *eventmask)
{
	int maskint = strings_to_mask(eventmask);

	tris_mutex_lock(&s->session->__lock);
	if (maskint >= 0)
		s->session->send_events = maskint;
	tris_mutex_unlock(&s->session->__lock);

	return maskint;
}

/*
 * Here we start with action_ handlers for AMI actions,
 * and the internal functions used by them.
 * Generally, the handlers are called action_foo()
 */

/* helper function for action_login() */
static int authenticate(struct mansession *s, const struct message *m)
{
	const char *username = astman_get_header(m, "Username");
	const char *password = astman_get_header(m, "Secret");
	int error = -1;
	struct tris_manager_user *user = NULL;

	if (tris_strlen_zero(username))	/* missing username */
		return -1;

	/* locate user in locked state */
	TRIS_RWLIST_WRLOCK(&users);

	if (!(user = get_manager_by_name_locked(username))) {
		tris_log(LOG_NOTICE, "%s tried to authenticate with nonexistent user '%s'\n", tris_inet_ntoa(s->session->sin.sin_addr), username);
	} else if (user->ha && !tris_apply_ha(user->ha, &(s->session->sin))) {
		tris_log(LOG_NOTICE, "%s failed to pass IP ACL as '%s'\n", tris_inet_ntoa(s->session->sin.sin_addr), username);
	} else if (!strcasecmp(astman_get_header(m, "AuthType"), "MD5")) {
		const char *key = astman_get_header(m, "Key");
		if (!tris_strlen_zero(key) && !tris_strlen_zero(s->session->challenge) && user->secret) {
			int x;
			int len = 0;
			char md5key[256] = "";
			struct MD5Context md5;
			unsigned char digest[16];

			MD5Init(&md5);
			MD5Update(&md5, (unsigned char *) s->session->challenge, strlen(s->session->challenge));
			MD5Update(&md5, (unsigned char *) user->secret, strlen(user->secret));
			MD5Final(digest, &md5);
			for (x = 0; x < 16; x++)
				len += sprintf(md5key + len, "%2.2x", digest[x]);
			if (!strcmp(md5key, key))
				error = 0;
		} else {
			tris_debug(1, "MD5 authentication is not possible.  challenge: '%s'\n", 
				S_OR(s->session->challenge, ""));
		}
	} else if (password && user->secret && !strcmp(password, user->secret))
		error = 0;

	if (error) {
		tris_log(LOG_NOTICE, "%s failed to authenticate as '%s'\n", tris_inet_ntoa(s->session->sin.sin_addr), username);
		TRIS_RWLIST_UNLOCK(&users);
		return -1;
	}

	/* auth complete */
	
	tris_copy_string(s->session->username, username, sizeof(s->session->username));
	s->session->readperm = user->readperm;
	s->session->writeperm = user->writeperm;
	s->session->writetimeout = user->writetimeout;
	s->session->sessionstart = time(NULL);
	set_eventmask(s, astman_get_header(m, "Events"));
	
	TRIS_RWLIST_UNLOCK(&users);
	return 0;
}


/*! \brief Manager PING */
static char mandescr_ping[] =
"Description: A 'Ping' action will ellicit a 'Pong' response.  Used to keep the\n"
"  manager connection open.\n"
"Variables: NONE\n";

static int action_ping(struct mansession *s, const struct message *m)
{
	const char *actionid = astman_get_header(m, "ActionID");

	astman_append(s, "Response: Success\r\n");
	if (!tris_strlen_zero(actionid)){
		astman_append(s, "ActionID: %s\r\n", actionid);
	}
	astman_append(s, "Ping: Pong\r\n\r\n");
	return 0;
}

static char mandescr_getconfig[] =
"Description: A 'GetConfig' action will dump the contents of a configuration\n"
"file by category and contents or optionally by specified category only.\n"
"Variables: (Names marked with * are required)\n"
"   *Filename: Configuration filename (e.g. foo.conf)\n"
"   Category: Category in configuration file\n";

static int action_getconfig(struct mansession *s, const struct message *m)
{
	struct tris_config *cfg;
	const char *fn = astman_get_header(m, "Filename");
	const char *category = astman_get_header(m, "Category");
	int catcount = 0;
	int lineno = 0;
	char *cur_category = NULL;
	struct tris_variable *v;
	struct tris_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS | CONFIG_FLAG_NOCACHE };

	if (tris_strlen_zero(fn)) {
		astman_send_error(s, m, "Filename not specified");
		return 0;
	}
	cfg = tris_config_load2(fn, "manager", config_flags);
	if (cfg == CONFIG_STATUS_FILEMISSING) {
		astman_send_error(s, m, "Config file not found");
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		astman_send_error(s, m, "Config file has invalid format");
		return 0;
	}

	astman_start_ack(s, m);
	while ((cur_category = tris_category_browse(cfg, cur_category))) {
		if (tris_strlen_zero(category) || (!tris_strlen_zero(category) && !strcmp(category, cur_category))) {
			lineno = 0;
			astman_append(s, "Category-%06d: %s\r\n", catcount, cur_category);
			for (v = tris_variable_browse(cfg, cur_category); v; v = v->next)
				astman_append(s, "Line-%06d-%06d: %s=%s\r\n", catcount, lineno++, v->name, v->value);
			catcount++;
		}
	}
	if (!tris_strlen_zero(category) && catcount == 0) /* TODO: actually, a config with no categories doesn't even get loaded */
		astman_append(s, "No categories found\r\n");
	tris_config_destroy(cfg);
	astman_append(s, "\r\n");

	return 0;
}

static char mandescr_listcategories[] =
"Description: A 'ListCategories' action will dump the categories in\n"
"a given file.\n"
"Variables:\n"
"   Filename: Configuration filename (e.g. foo.conf)\n";

static int action_listcategories(struct mansession *s, const struct message *m)
{
	struct tris_config *cfg;
	const char *fn = astman_get_header(m, "Filename");
	char *category = NULL;
	struct tris_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS | CONFIG_FLAG_NOCACHE };
	int catcount = 0;

	if (tris_strlen_zero(fn)) {
		astman_send_error(s, m, "Filename not specified");
		return 0;
	}
	if (!(cfg = tris_config_load2(fn, "manager", config_flags))) {
		astman_send_error(s, m, "Config file not found");
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		astman_send_error(s, m, "Config file has invalid format");
		return 0;
	}
	astman_start_ack(s, m);
	while ((category = tris_category_browse(cfg, category))) {
		astman_append(s, "Category-%06d: %s\r\n", catcount, category);
		catcount++;
	}
	if (catcount == 0) /* TODO: actually, a config with no categories doesn't even get loaded */
		astman_append(s, "Error: no categories found\r\n");
	tris_config_destroy(cfg);
	astman_append(s, "\r\n");

	return 0;
}


	

/*! The amount of space in out must be at least ( 2 * strlen(in) + 1 ) */
static void json_escape(char *out, const char *in)
{
	for (; *in; in++) {
		if (*in == '\\' || *in == '\"')
			*out++ = '\\';
		*out++ = *in;
	}
	*out = '\0';
}

static char mandescr_getconfigjson[] =
"Description: A 'GetConfigJSON' action will dump the contents of a configuration\n"
"file by category and contents in JSON format.  This only makes sense to be used\n"
"using rawman over the HTTP interface.\n"
"Variables:\n"
"   Filename: Configuration filename (e.g. foo.conf)\n";

static int action_getconfigjson(struct mansession *s, const struct message *m)
{
	struct tris_config *cfg;
	const char *fn = astman_get_header(m, "Filename");
	char *category = NULL;
	struct tris_variable *v;
	int comma1 = 0;
	char *buf = NULL;
	unsigned int buf_len = 0;
	struct tris_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS | CONFIG_FLAG_NOCACHE };

	if (tris_strlen_zero(fn)) {
		astman_send_error(s, m, "Filename not specified");
		return 0;
	}

	if (!(cfg = tris_config_load2(fn, "manager", config_flags))) {
		astman_send_error(s, m, "Config file not found");
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		astman_send_error(s, m, "Config file has invalid format");
		return 0;
	}

	buf_len = 512;
	buf = alloca(buf_len);

	astman_start_ack(s, m);
	astman_append(s, "JSON: {");
	while ((category = tris_category_browse(cfg, category))) {
		int comma2 = 0;
		if (buf_len < 2 * strlen(category) + 1) {
			buf_len *= 2;
			buf = alloca(buf_len);
		}
		json_escape(buf, category);
		astman_append(s, "%s\"%s\":[", comma1 ? "," : "", buf);
		if (!comma1)
			comma1 = 1;
		for (v = tris_variable_browse(cfg, category); v; v = v->next) {
			if (comma2)
				astman_append(s, ",");
			if (buf_len < 2 * strlen(v->name) + 1) {
				buf_len *= 2;
				buf = alloca(buf_len);
			}
			json_escape(buf, v->name);
			astman_append(s, "\"%s", buf);
			if (buf_len < 2 * strlen(v->value) + 1) {
				buf_len *= 2;
				buf = alloca(buf_len);
			}
			json_escape(buf, v->value);
			astman_append(s, "%s\"", buf);
			if (!comma2)
				comma2 = 1;
		}
		astman_append(s, "]");
	}
	astman_append(s, "}\r\n\r\n");

	tris_config_destroy(cfg);

	return 0;
}

/* helper function for action_updateconfig */
static enum error_type handle_updates(struct mansession *s, const struct message *m, struct tris_config *cfg, const char *dfn)
{
	int x;
	char hdr[40];
	const char *action, *cat, *var, *value, *match, *line;
	struct tris_category *category;
	struct tris_variable *v;
	struct tris_str *str1 = tris_str_create(16), *str2 = tris_str_create(16);
	enum error_type result = 0;

	for (x = 0; x < 100000; x++) {	/* 100000 = the max number of allowed updates + 1 */
		unsigned int object = 0;

		snprintf(hdr, sizeof(hdr), "Action-%06d", x);
		action = astman_get_header(m, hdr);
		if (tris_strlen_zero(action))		/* breaks the for loop if no action header */
			break;                      	/* this could cause problems if actions come in misnumbered */

		snprintf(hdr, sizeof(hdr), "Cat-%06d", x);
		cat = astman_get_header(m, hdr);
		if (tris_strlen_zero(cat)) {		/* every action needs a category */
			result =  UNSPECIFIED_CATEGORY;
			break;
		}

		snprintf(hdr, sizeof(hdr), "Var-%06d", x);
		var = astman_get_header(m, hdr);

		snprintf(hdr, sizeof(hdr), "Value-%06d", x);
		value = astman_get_header(m, hdr);

		if (!tris_strlen_zero(value) && *value == '>') {
			object = 1;
			value++;
		}
	
		snprintf(hdr, sizeof(hdr), "Match-%06d", x);
		match = astman_get_header(m, hdr);

		snprintf(hdr, sizeof(hdr), "Line-%06d", x);
		line = astman_get_header(m, hdr);

		if (!strcasecmp(action, "newcat")) {
			if (tris_category_get(cfg,cat)) {	/* check to make sure the cat doesn't */
				result = FAILURE_NEWCAT;	/* already exist */
				break;
			}
			if (!(category = tris_category_new(cat, dfn, -1))) {
				result = FAILURE_ALLOCATION;
				break;
			}
			if (tris_strlen_zero(match)) {
				tris_category_append(cfg, category);
			} else
				tris_category_insert(cfg, category, match);
		} else if (!strcasecmp(action, "renamecat")) {
			if (tris_strlen_zero(value)) {
				result = UNSPECIFIED_ARGUMENT;
				break;
			}
			if (!(category = tris_category_get(cfg, cat))) {
				result = UNKNOWN_CATEGORY;
				break;
			}
			tris_category_rename(category, value);
		} else if (!strcasecmp(action, "delcat")) {
			if (tris_category_delete(cfg, cat)) {
				result = FAILURE_DELCAT;
				break;
			}
		} else if (!strcasecmp(action, "emptycat")) {
			if (tris_category_empty(cfg, cat)) {
				result = FAILURE_EMPTYCAT;
				break;
			}
		} else if (!strcasecmp(action, "update")) {
			if (tris_strlen_zero(var)) {
				result = UNSPECIFIED_ARGUMENT;
				break;
			}
			if (!(category = tris_category_get(cfg,cat))) {
				result = UNKNOWN_CATEGORY;
				break;
			}
			if (tris_variable_update(category, var, value, match, object)) {
				result = FAILURE_UPDATE;
				break;
			}
		} else if (!strcasecmp(action, "delete")) {
			if ((tris_strlen_zero(var) && tris_strlen_zero(line))) {
				result = UNSPECIFIED_ARGUMENT;
				break;
			}
			if (!(category = tris_category_get(cfg, cat))) {
				result = UNKNOWN_CATEGORY;
				break;
			}
			if (tris_variable_delete(category, var, match, line)) {
				result = FAILURE_DELETE;
				break;
			}
		} else if (!strcasecmp(action, "append")) {
			if (tris_strlen_zero(var)) {
				result = UNSPECIFIED_ARGUMENT;
				break;
			}
			if (!(category = tris_category_get(cfg, cat))) {
				result = UNKNOWN_CATEGORY;	
				break;
			}
			if (!(v = tris_variable_new(var, value, dfn))) {
				result = FAILURE_ALLOCATION;
				break;
			}
			if (object || (match && !strcasecmp(match, "object")))
				v->object = 1;
			tris_variable_append(category, v);
		} else if (!strcasecmp(action, "insert")) {
			if (tris_strlen_zero(var) || tris_strlen_zero(line)) {
				result = UNSPECIFIED_ARGUMENT;
				break;
			}
			if (!(category = tris_category_get(cfg, cat))) {
				result = UNKNOWN_CATEGORY;
				break;
			}
			if (!(v = tris_variable_new(var, value, dfn))) {
				result = FAILURE_ALLOCATION;
				break;
			}
			tris_variable_insert(category, v, line);
		}
		else {
			tris_log(LOG_WARNING, "Action-%06d: %s not handled\n", x, action);
			result = UNKNOWN_ACTION;
			break;
		}
	}
	tris_free(str1);
	tris_free(str2);
	return result;
}

static char mandescr_updateconfig[] =
"Description: A 'UpdateConfig' action will modify, create, or delete\n"
"configuration elements in Trismedia configuration files.\n"
"Variables (X's represent 6 digit number beginning with 000000):\n"
"   SrcFilename:   Configuration filename to read(e.g. foo.conf)\n"
"   DstFilename:   Configuration filename to write(e.g. foo.conf)\n"
"   Reload:        Whether or not a reload should take place (or name of specific module)\n"
"   Action-XXXXXX: Action to Take (NewCat,RenameCat,DelCat,EmptyCat,Update,Delete,Append,Insert)\n"
"   Cat-XXXXXX:    Category to operate on\n"
"   Var-XXXXXX:    Variable to work on\n"
"   Value-XXXXXX:  Value to work on\n"
"   Match-XXXXXX:  Extra match required to match line\n"
"   Line-XXXXXX:   Line in category to operate on (used with delete and insert actions)\n";

static int action_updateconfig(struct mansession *s, const struct message *m)
{
	struct tris_config *cfg;
	const char *sfn = astman_get_header(m, "SrcFilename");
	const char *dfn = astman_get_header(m, "DstFilename");
	int res;
	const char *rld = astman_get_header(m, "Reload");
	struct tris_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS | CONFIG_FLAG_NOCACHE };
	enum error_type result;

	if (tris_strlen_zero(sfn) || tris_strlen_zero(dfn)) {
		astman_send_error(s, m, "Filename not specified");
		return 0;
	}
	if (!(cfg = tris_config_load2(sfn, "manager", config_flags))) {
		astman_send_error(s, m, "Config file not found");
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		astman_send_error(s, m, "Config file has invalid format");
		return 0;
	}
	result = handle_updates(s, m, cfg, dfn);
	if (!result) {
		tris_include_rename(cfg, sfn, dfn); /* change the include references from dfn to sfn, so things match up */
		res = tris_config_text_file_save(dfn, cfg, "Manager");
		tris_config_destroy(cfg);
		if (res) {
			astman_send_error(s, m, "Save of config failed");
			return 0;
		}
		astman_send_ack(s, m, NULL);
		if (!tris_strlen_zero(rld)) {
			if (tris_true(rld))
				rld = NULL;
			tris_module_reload(rld);
		}
	} else {
		tris_config_destroy(cfg);
		switch(result) {
		case UNKNOWN_ACTION:
			astman_send_error(s, m, "Unknown action command");
			break;
		case UNKNOWN_CATEGORY:
			astman_send_error(s, m, "Given category does not exist");
			break;
		case UNSPECIFIED_CATEGORY:
			astman_send_error(s, m, "Category not specified");
			break;
		case UNSPECIFIED_ARGUMENT:
			astman_send_error(s, m, "Problem with category, value, or line (if required)");
			break;
		case FAILURE_ALLOCATION:
			astman_send_error(s, m, "Memory allocation failure, this should not happen");
			break;
		case FAILURE_NEWCAT:
			astman_send_error(s, m, "Create category did not complete successfully");
			break;
		case FAILURE_DELCAT:
			astman_send_error(s, m, "Delete category did not complete successfully");
			break;
		case FAILURE_EMPTYCAT:
			astman_send_error(s, m, "Empty category did not complete successfully");
			break;
		case FAILURE_UPDATE:
			astman_send_error(s, m, "Update did not complete successfully");
			break;
		case FAILURE_DELETE:
			astman_send_error(s, m, "Delete did not complete successfully");
			break;
		case FAILURE_APPEND:
			astman_send_error(s, m, "Append did not complete successfully");
			break;
		}
	}
	return 0;
}

static char mandescr_createconfig[] =
"Description: A 'CreateConfig' action will create an empty file in the\n"
"configuration directory. This action is intended to be used before an\n"
"UpdateConfig action.\n"
"Variables\n"
"   Filename:   The configuration filename to create (e.g. foo.conf)\n";

static int action_createconfig(struct mansession *s, const struct message *m)
{
	int fd;
	const char *fn = astman_get_header(m, "Filename");
	struct tris_str *filepath = tris_str_alloca(PATH_MAX);
	tris_str_set(&filepath, 0, "%s/", tris_config_TRIS_CONFIG_DIR);
	tris_str_append(&filepath, 0, "%s", fn);

	if ((fd = open(tris_str_buffer(filepath), O_CREAT | O_EXCL, TRIS_FILE_MODE)) != -1) {
		close(fd);
		astman_send_ack(s, m, "New configuration file created successfully");
	} else {
		astman_send_error(s, m, strerror(errno));
	}

	return 0;
}

/*! \brief Manager WAITEVENT */
static char mandescr_waitevent[] =
"Description: A 'WaitEvent' action will ellicit a 'Success' response.  Whenever\n"
"a manager event is queued.  Once WaitEvent has been called on an HTTP manager\n"
"session, events will be generated and queued.\n"
"Variables: \n"
"   Timeout: Maximum time (in seconds) to wait for events, -1 means forever.\n";

static int action_waitevent(struct mansession *s, const struct message *m)
{
	const char *timeouts = astman_get_header(m, "Timeout");
	int timeout = -1;
	int x;
	int needexit = 0;
	const char *id = astman_get_header(m, "ActionID");
	char idText[256];

	if (!tris_strlen_zero(id))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", id);
	else
		idText[0] = '\0';

	if (!tris_strlen_zero(timeouts)) {
		sscanf(timeouts, "%30i", &timeout);
		if (timeout < -1)
			timeout = -1;
		/* XXX maybe put an upper bound, or prevent the use of 0 ? */
	}

	tris_mutex_lock(&s->session->__lock);
	if (s->session->waiting_thread != TRIS_PTHREADT_NULL)
		pthread_kill(s->session->waiting_thread, SIGURG);

	if (s->session->managerid) { /* AMI-over-HTTP session */
		/*
		 * Make sure the timeout is within the expire time of the session,
		 * as the client will likely abort the request if it does not see
		 * data coming after some amount of time.
		 */
		time_t now = time(NULL);
		int max = s->session->sessiontimeout - now - 10;

		if (max < 0)	/* We are already late. Strange but possible. */
			max = 0;
		if (timeout < 0 || timeout > max)
			timeout = max;
		if (!s->session->send_events)	/* make sure we record events */
			s->session->send_events = -1;
	}
	tris_mutex_unlock(&s->session->__lock);

	/* XXX should this go inside the lock ? */
	s->session->waiting_thread = pthread_self();	/* let new events wake up this thread */
	tris_debug(1, "Starting waiting for an event!\n");

	for (x = 0; x < timeout || timeout < 0; x++) {
		tris_mutex_lock(&s->session->__lock);
		if (NEW_EVENT(s))
			needexit = 1;
		/* We can have multiple HTTP session point to the same mansession entry.
		 * The way we deal with it is not very nice: newcomers kick out the previous
		 * HTTP session. XXX this needs to be improved.
		 */
		if (s->session->waiting_thread != pthread_self())
			needexit = 1;
		if (s->session->needdestroy)
			needexit = 1;
		tris_mutex_unlock(&s->session->__lock);
		if (needexit)
			break;
		if (s->session->managerid == 0) {	/* AMI session */
			if (tris_wait_for_input(s->session->fd, 1000))
				break;
		} else {	/* HTTP session */
			sleep(1);
		}
	}
	tris_debug(1, "Finished waiting for an event!\n");
	tris_mutex_lock(&s->session->__lock);
	if (s->session->waiting_thread == pthread_self()) {
		struct eventqent *eqe;
		astman_send_response(s, m, "Success", "Waiting for Event completed.");
		while ( (eqe = NEW_EVENT(s)) ) {
			ref_event(eqe);
			if (((s->session->readperm & eqe->category) == eqe->category) &&
			    ((s->session->send_events & eqe->category) == eqe->category)) {
				astman_append(s, "%s", eqe->eventdata);
			}
			s->session->last_ev = unref_event(s->session->last_ev);
		}
		astman_append(s,
			"Event: WaitEventComplete\r\n"
			"%s"
			"\r\n", idText);
		s->session->waiting_thread = TRIS_PTHREADT_NULL;
	} else {
		tris_debug(1, "Abandoning event request!\n");
	}
	tris_mutex_unlock(&s->session->__lock);
	return 0;
}

static char mandescr_listcommands[] =
"Description: Returns the action name and synopsis for every\n"
"  action that is available to the user\n"
"Variables: NONE\n";

/*! \note The actionlock is read-locked by the caller of this function */
static int action_listcommands(struct mansession *s, const struct message *m)
{
	struct manager_action *cur;
	struct tris_str *temp = tris_str_alloca(BUFSIZ); /* XXX very large ? */

	astman_start_ack(s, m);
	TRIS_RWLIST_TRAVERSE(&actions, cur, list) {
		if (s->session->writeperm & cur->authority || cur->authority == 0)
			astman_append(s, "%s: %s (Priv: %s)\r\n",
				cur->action, cur->synopsis, authority_to_str(cur->authority, &temp));
	}
	astman_append(s, "\r\n");

	return 0;
}

static char mandescr_events[] =
"Description: Enable/Disable sending of events to this manager\n"
"  client.\n"
"Variables:\n"
"	EventMask: 'on' if all events should be sent,\n"
"		'off' if no events should be sent,\n"
"		'system,call,log' to select which flags events should have to be sent.\n";

static int action_events(struct mansession *s, const struct message *m)
{
	const char *mask = astman_get_header(m, "EventMask");
	int res;

	res = set_eventmask(s, mask);
	if (res > 0)
		astman_append(s, "Response: Success\r\n"
				 "Events: On\r\n\r\n");
	else if (res == 0)
		astman_append(s, "Response: Success\r\n"
				 "Events: Off\r\n\r\n");
	return 0;
}

static char mandescr_logoff[] =
"Description: Logoff this manager session\n"
"Variables: NONE\n";

static int action_logoff(struct mansession *s, const struct message *m)
{
	astman_send_response(s, m, "Goodbye", "Thanks for all the fish.");
	return -1;
}

static int action_login(struct mansession *s, const struct message *m)
{
	if (authenticate(s, m)) {
		sleep(1);
		astman_send_error(s, m, "Authentication failed");
		return -1;
	}
	s->session->authenticated = 1;
	if (manager_displayconnects(s->session))
		tris_verb(2, "%sManager '%s' logged on from %s\n", (s->session->managerid ? "HTTP " : ""), s->session->username, tris_inet_ntoa(s->session->sin.sin_addr));
	tris_log(LOG_EVENT, "%sManager '%s' logged on from %s\n", (s->session->managerid ? "HTTP " : ""), s->session->username, tris_inet_ntoa(s->session->sin.sin_addr));
	astman_send_ack(s, m, "Authentication accepted");
	return 0;
}

static int action_challenge(struct mansession *s, const struct message *m)
{
	const char *authtype = astman_get_header(m, "AuthType");

	if (!strcasecmp(authtype, "MD5")) {
		if (tris_strlen_zero(s->session->challenge))
			snprintf(s->session->challenge, sizeof(s->session->challenge), "%ld", tris_random());
		tris_mutex_lock(&s->session->__lock);
		astman_start_ack(s, m);
		astman_append(s, "Challenge: %s\r\n\r\n", s->session->challenge);
		tris_mutex_unlock(&s->session->__lock);
	} else {
		astman_send_error(s, m, "Must specify AuthType");
	}
	return 0;
}

static char mandescr_hangup[] =
"Description: Hangup a channel\n"
"Variables: \n"
"	Channel: The channel name to be hungup\n";

static int action_hangup(struct mansession *s, const struct message *m)
{
	struct tris_channel *c = NULL;
	const char *name = astman_get_header(m, "Channel");
	if (tris_strlen_zero(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	c = tris_get_channel_by_name_locked(name);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	tris_softhangup(c, TRIS_SOFTHANGUP_EXPLICIT);
	tris_channel_unlock(c);
	astman_send_ack(s, m, "Channel Hungup");
	return 0;
}

static char mandescr_setvar[] =
"Description: Set a global or local channel variable.\n"
"Variables: (Names marked with * are required)\n"
"	Channel: Channel to set variable for\n"
"	*Variable: Variable name\n"
"	*Value: Value\n";

static int action_setvar(struct mansession *s, const struct message *m)
{
	struct tris_channel *c = NULL;
	const char *name = astman_get_header(m, "Channel");
	const char *varname = astman_get_header(m, "Variable");
	const char *varval = astman_get_header(m, "Value");
	int res = 0;
	
	if (tris_strlen_zero(varname)) {
		astman_send_error(s, m, "No variable specified");
		return 0;
	}

	if (!tris_strlen_zero(name)) {
		c = tris_get_channel_by_name_locked(name);
		if (!c) {
			astman_send_error(s, m, "No such channel");
			return 0;
		}
	}
	if (varname[strlen(varname)-1] == ')') {
		char *function = tris_strdupa(varname);
		res = tris_func_write(c, function, varval);
	} else {
		pbx_builtin_setvar_helper(c, varname, S_OR(varval, ""));
	}

	if (c)
		tris_channel_unlock(c);
	if (res == 0) {
		astman_send_ack(s, m, "Variable Set");	
	} else {
		astman_send_error(s, m, "Variable not set");
	}
	return 0;
}

static char mandescr_getvar[] =
"Description: Get the value of a global or local channel variable.\n"
"Variables: (Names marked with * are required)\n"
"	Channel: Channel to read variable from\n"
"	*Variable: Variable name\n"
"	ActionID: Optional Action id for message matching.\n";

static int action_getvar(struct mansession *s, const struct message *m)
{
	struct tris_channel *c = NULL;
	const char *name = astman_get_header(m, "Channel");
	const char *varname = astman_get_header(m, "Variable");
	char *varval;
	char workspace[1024] = "";

	if (tris_strlen_zero(varname)) {
		astman_send_error(s, m, "No variable specified");
		return 0;
	}

	if (!tris_strlen_zero(name)) {
		c = tris_get_channel_by_name_locked(name);
		if (!c) {
			astman_send_error(s, m, "No such channel");
			return 0;
		}
	}

	if (varname[strlen(varname) - 1] == ')') {
		if (!c) {
			c = tris_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/manager");
			if (c) {
				tris_func_read(c, (char *) varname, workspace, sizeof(workspace));
				tris_channel_free(c);
				c = NULL;
			} else
				tris_log(LOG_ERROR, "Unable to allocate bogus channel for variable substitution.  Function results may be blank.\n");
		} else
			tris_func_read(c, (char *) varname, workspace, sizeof(workspace));
		varval = workspace;
	} else {
		pbx_retrieve_variable(c, varname, &varval, workspace, sizeof(workspace), NULL);
	}

	if (c)
		tris_channel_unlock(c);
	astman_start_ack(s, m);
	astman_append(s, "Variable: %s\r\nValue: %s\r\n\r\n", varname, varval);

	return 0;
}

static char mandescr_status[] = 
"Description: Lists channel status along with requested channel vars.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Name of the channel to query for status\n"
"	Variables: Comma ',' separated list of variables to include\n"
"	ActionID: Optional ID for this transaction\n"
"Will return the status information of each channel along with the\n"
"value for the specified channel variables.\n";
 

/*! \brief Manager "status" command to show channels */
/* Needs documentation... */
static int action_status(struct mansession *s, const struct message *m)
{
	const char *name = astman_get_header(m, "Channel");
	const char *cvariables = astman_get_header(m, "Variables");
	char *variables = tris_strdupa(S_OR(cvariables, ""));
	struct tris_channel *c;
	char bridge[256];
	struct timeval now = tris_tvnow();
	long elapsed_seconds = 0;
	int channels = 0;
	int all = tris_strlen_zero(name); /* set if we want all channels */
	const char *id = astman_get_header(m, "ActionID");
	char idText[256];
	TRIS_DECLARE_APP_ARGS(vars,
		TRIS_APP_ARG(name)[100];
	);
	struct tris_str *str = tris_str_create(1000);

	if (!tris_strlen_zero(id))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", id);
	else
		idText[0] = '\0';

	if (all)
		c = tris_channel_walk_locked(NULL);
	else {
		c = tris_get_channel_by_name_locked(name);
		if (!c) {
			astman_send_error(s, m, "No such channel");
			tris_free(str);
			return 0;
		}
	}
	astman_send_ack(s, m, "Channel status will follow");

	if (!tris_strlen_zero(cvariables)) {
		TRIS_STANDARD_APP_ARGS(vars, variables);
	}

	/* if we look by name, we break after the first iteration */
	while (c) {
		if (!tris_strlen_zero(cvariables)) {
			int i;
			tris_str_reset(str);
			for (i = 0; i < vars.argc; i++) {
				char valbuf[512], *ret = NULL;

				if (vars.name[i][strlen(vars.name[i]) - 1] == ')') {
					if (tris_func_read(c, vars.name[i], valbuf, sizeof(valbuf)) < 0) {
						valbuf[0] = '\0';
					}
					ret = valbuf;
				} else {
					pbx_retrieve_variable(c, vars.name[i], &ret, valbuf, sizeof(valbuf), NULL);
				}

				tris_str_append(&str, 0, "Variable: %s=%s\r\n", vars.name[i], ret);
			}
		}

		channels++;
		if (c->_bridge)
			snprintf(bridge, sizeof(bridge), "BridgedChannel: %s\r\nBridgedUniqueid: %s\r\n", c->_bridge->name, c->_bridge->uniqueid);
		else
			bridge[0] = '\0';
		if (c->pbx) {
			if (c->cdr) {
				elapsed_seconds = now.tv_sec - c->cdr->start.tv_sec;
			}
			astman_append(s,
			"Event: Status\r\n"
			"Privilege: Call\r\n"
			"Channel: %s\r\n"
			"CallerIDNum: %s\r\n"
			"CallerIDName: %s\r\n"
			"Accountcode: %s\r\n"
			"ChannelState: %d\r\n"
			"ChannelStateDesc: %s\r\n"
			"Context: %s\r\n"
			"Extension: %s\r\n"
			"Priority: %d\r\n"
			"Seconds: %ld\r\n"
			"%s"
			"Uniqueid: %s\r\n"
			"%s"
			"%s"
			"\r\n",
			c->name,
			S_OR(c->cid.cid_num, ""),
			S_OR(c->cid.cid_name, ""),
			c->accountcode,
			c->_state,
			tris_state2str(c->_state), c->context,
			c->exten, c->priority, (long)elapsed_seconds, bridge, c->uniqueid, tris_str_buffer(str), idText);
		} else {
			astman_append(s,
			"Event: Status\r\n"
			"Privilege: Call\r\n"
			"Channel: %s\r\n"
			"CallerIDNum: %s\r\n"
			"CallerIDName: %s\r\n"
			"Account: %s\r\n"
			"State: %s\r\n"
			"%s"
			"Uniqueid: %s\r\n"
			"%s"
			"%s"
			"\r\n",
			c->name,
			S_OR(c->cid.cid_num, "<unknown>"),
			S_OR(c->cid.cid_name, "<unknown>"),
			c->accountcode,
			tris_state2str(c->_state), bridge, c->uniqueid, tris_str_buffer(str), idText);
		}
		tris_channel_unlock(c);
		if (!all)
			break;
		c = tris_channel_walk_locked(c);
	}
	astman_append(s,
	"Event: StatusComplete\r\n"
	"%s"
	"Items: %d\r\n"
	"\r\n", idText, channels);
	tris_free(str);
	return 0;
}

static char mandescr_sendtext[] =
"Description: Sends A Text Message while in a call.\n"
"Variables: (Names marked with * are required)\n"
"       *Channel: Channel to send message to\n"
"       *Message: Message to send\n"
"       ActionID: Optional Action id for message matching.\n";

static int action_sendtext(struct mansession *s, const struct message *m)
{
	struct tris_channel *c = NULL;
	const char *name = astman_get_header(m, "Channel");
	const char *textmsg = astman_get_header(m, "Message");
	int res = 0;

	if (tris_strlen_zero(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}

	if (tris_strlen_zero(textmsg)) {
		astman_send_error(s, m, "No Message specified");
		return 0;
	}

	c = tris_get_channel_by_name_locked(name);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}

	res = tris_sendtext(c, textmsg);
	tris_channel_unlock(c);
	
	if (res > 0)
		astman_send_ack(s, m, "Success");
	else
		astman_send_error(s, m, "Failure");
	
	return res;
}

static char mandescr_redirect[] =
"Description: Redirect (transfer) a call.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel to redirect\n"
"	ExtraChannel: Second call leg to transfer (optional)\n"
"	*Exten: Extension to transfer to\n"
"	*Context: Context to transfer to\n"
"	*Priority: Priority to transfer to\n"
"	ActionID: Optional Action id for message matching.\n";

/*! \brief  action_redirect: The redirect manager command */
static int action_redirect(struct mansession *s, const struct message *m)
{
	const char *name = astman_get_header(m, "Channel");
	const char *name2 = astman_get_header(m, "ExtraChannel");
	const char *exten = astman_get_header(m, "Exten");
	const char *context = astman_get_header(m, "Context");
	const char *priority = astman_get_header(m, "Priority");
	struct tris_channel *chan, *chan2 = NULL;
	int pi = 0;
	int res;

	if (tris_strlen_zero(name)) {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}
	if (!tris_strlen_zero(priority) && (sscanf(priority, "%30d", &pi) != 1)) {
		if ((pi = tris_findlabel_extension(NULL, context, exten, priority, NULL)) < 1) {
			astman_send_error(s, m, "Invalid priority");
			return 0;
		}
	}
	/* XXX watch out, possible deadlock - we are trying to get two channels!!! */
	chan = tris_get_channel_by_name_locked(name);
	if (!chan) {
		char buf[256];
		snprintf(buf, sizeof(buf), "Channel does not exist: %s", name);
		astman_send_error(s, m, buf);
		return 0;
	}
	if (tris_check_hangup(chan)) {
		astman_send_error(s, m, "Redirect failed, channel not up.");
		tris_channel_unlock(chan);
		return 0;
	}
	if (!tris_strlen_zero(name2))
		chan2 = tris_get_channel_by_name_locked(name2);
	if (chan2 && tris_check_hangup(chan2)) {
		astman_send_error(s, m, "Redirect failed, extra channel not up.");
		tris_channel_unlock(chan);
		tris_channel_unlock(chan2);
		return 0;
	}
	if (chan->pbx) {
		tris_channel_lock(chan);
		tris_set_flag(chan, TRIS_FLAG_BRIDGE_HANGUP_DONT); /* don't let the after-bridge code run the h-exten */
		tris_channel_unlock(chan);
	}
	res = tris_async_goto(chan, context, exten, pi);
	if (!res) {
		if (!tris_strlen_zero(name2)) {
			if (chan2) {
				if (chan2->pbx) {
					tris_channel_lock(chan2);
					tris_set_flag(chan2, TRIS_FLAG_BRIDGE_HANGUP_DONT); /* don't let the after-bridge code run the h-exten */
					tris_channel_unlock(chan2);
				}
				res = tris_async_goto(chan2, context, exten, pi);
			} else {
				res = -1;
			}
			if (!res)
				astman_send_ack(s, m, "Dual Redirect successful");
			else
				astman_send_error(s, m, "Secondary redirect failed");
		} else
			astman_send_ack(s, m, "Redirect successful");
	} else
		astman_send_error(s, m, "Redirect failed");
	if (chan)
		tris_channel_unlock(chan);
	if (chan2)
		tris_channel_unlock(chan2);
	return 0;
}

static char mandescr_atxfer[] =
"Description: Attended transfer.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Transferer's channel\n"
"	*Exten: Extension to transfer to\n"
"	*Context: Context to transfer to\n"
"	*Priority: Priority to transfer to\n"
"	ActionID: Optional Action id for message matching.\n";

static int action_atxfer(struct mansession *s, const struct message *m)
{
	const char *name = astman_get_header(m, "Channel");
	const char *exten = astman_get_header(m, "Exten");
	const char *context = astman_get_header(m, "Context");
	struct tris_channel *chan = NULL;
	struct tris_call_feature *atxfer_feature = NULL;
	char *feature_code = NULL;

	if (tris_strlen_zero(name)) { 
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if (tris_strlen_zero(exten)) {
		astman_send_error(s, m, "No extension specified");
		return 0;
	}

	if (!(atxfer_feature = tris_find_call_feature("atxfer"))) {
		astman_send_error(s, m, "No attended transfer feature found");
		return 0;
	}

	if (!(chan = tris_get_channel_by_name_locked(name))) {
		astman_send_error(s, m, "Channel specified does not exist");
		return 0;
	}

	if (!tris_strlen_zero(context)) {
		pbx_builtin_setvar_helper(chan, "TRANSFER_CONTEXT", context);
	}

	for (feature_code = atxfer_feature->exten; feature_code && *feature_code; ++feature_code) {
		struct tris_frame f = {TRIS_FRAME_DTMF, *feature_code};
		tris_queue_frame(chan, &f);
	}

	for (feature_code = (char *)exten; feature_code && *feature_code; ++feature_code) {
		struct tris_frame f = {TRIS_FRAME_DTMF, *feature_code};
		tris_queue_frame(chan, &f);
	}

	astman_send_ack(s, m, "Atxfer successfully queued");
	tris_channel_unlock(chan);

	return 0;
}

static int check_blacklist(const char *cmd)
{
	char *cmd_copy, *cur_cmd;
	char *cmd_words[MAX_BLACKLIST_CMD_LEN] = { NULL, };
	int i;

	cmd_copy = tris_strdupa(cmd);
	for (i = 0; i < MAX_BLACKLIST_CMD_LEN && (cur_cmd = strsep(&cmd_copy, " ")); i++) {
		cur_cmd = tris_strip(cur_cmd);
		if (tris_strlen_zero(cur_cmd)) {
			i--;
			continue;
		}

		cmd_words[i] = cur_cmd;
	}

	for (i = 0; i < ARRAY_LEN(command_blacklist); i++) {
		int j, match = 1;

		for (j = 0; command_blacklist[i].words[j]; j++) {
			if (tris_strlen_zero(cmd_words[j]) || strcasecmp(cmd_words[j], command_blacklist[i].words[j])) {
				match = 0;
				break;
			}
		}

		if (match) {
			return 1;
		}
	}

	return 0;
}

static char mandescr_command[] =
"Description: Run a CLI command.\n"
"Variables: (Names marked with * are required)\n"
"	*Command: Trismedia CLI command to run\n"
"	ActionID: Optional Action id for message matching.\n";

/*! \brief  Manager command "command" - execute CLI command */
static int action_command(struct mansession *s, const struct message *m)
{
	const char *cmd = astman_get_header(m, "Command");
	const char *id = astman_get_header(m, "ActionID");
	char *buf, *final_buf;
	char template[] = "/tmp/ast-ami-XXXXXX";	/* template for temporary file */
	int fd;
	off_t l;

	if (tris_strlen_zero(cmd)) {
		astman_send_error(s, m, "No command provided");
		return 0;
	}

	if (check_blacklist(cmd)) {
		astman_send_error(s, m, "Command blacklisted");
		return 0;
	}

	fd = mkstemp(template);

	astman_append(s, "Response: Follows\r\nPrivilege: Command\r\n");
	if (!tris_strlen_zero(id))
		astman_append(s, "ActionID: %s\r\n", id);
	/* FIXME: Wedge a ActionID response in here, waiting for later changes */
	tris_cli_command(fd, cmd);	/* XXX need to change this to use a FILE * */
	l = lseek(fd, 0, SEEK_END);	/* how many chars available */

	/* This has a potential to overflow the stack.  Hence, use the heap. */
	buf = tris_calloc(1, l + 1);
	final_buf = tris_calloc(1, l + 1);
	if (buf) {
		lseek(fd, 0, SEEK_SET);
		if (read(fd, buf, l) < 0) {
			tris_log(LOG_WARNING, "read() failed: %s\n", strerror(errno));
		}
		buf[l] = '\0';
		if (final_buf) {
			term_strip(final_buf, buf, l);
			final_buf[l] = '\0';
		}
		astman_append(s, "%s", S_OR(final_buf, buf));
		tris_free(buf);
	}
	close(fd);
	unlink(template);
	astman_append(s, "--END COMMAND--\r\n\r\n");
	if (final_buf)
		tris_free(final_buf);
	return 0;
}

/*! \brief helper function for originate */
struct ftris_originate_helper {
	char tech[TRIS_MAX_EXTENSION];
	/*! data can contain a channel name, extension number, username, password, etc. */
	char data[512];
	int timeout;
	int format;				/*!< Codecs used for a call */
	char app[TRIS_MAX_APP];
	char appdata[TRIS_MAX_EXTENSION];
	char cid_name[TRIS_MAX_EXTENSION];
	char cid_num[TRIS_MAX_EXTENSION];
	char context[TRIS_MAX_CONTEXT];
	char exten[TRIS_MAX_EXTENSION];
	char idtext[TRIS_MAX_EXTENSION];
	char account[TRIS_MAX_ACCOUNT_CODE];
	int priority;
	struct tris_variable *vars;
};

static void *ftris_originate(void *data)
{
	struct ftris_originate_helper *in = data;
	int res;
	int reason = 0;
	struct tris_channel *chan = NULL;
	char requested_channel[TRIS_CHANNEL_NAME];

	if (!tris_strlen_zero(in->app)) {
		res = tris_pbx_outgoing_app(in->tech, in->format, in->data, in->timeout, in->app, in->appdata, &reason, 1,
			S_OR(in->cid_num, NULL),
			S_OR(in->cid_name, NULL),
			in->vars, in->account, &chan);
	} else {
		res = tris_pbx_outgoing_exten(in->tech, in->format, in->data, in->timeout, in->context, in->exten, in->priority, &reason, 1,
			S_OR(in->cid_num, NULL),
			S_OR(in->cid_name, NULL),
			in->vars, in->account, &chan);
	}

	if (!chan)
		snprintf(requested_channel, TRIS_CHANNEL_NAME, "%s/%s", in->tech, in->data);	
	/* Tell the manager what happened with the channel */
	manager_event(EVENT_FLAG_CALL, "OriginateResponse",
		"%s%s"
		"Response: %s\r\n"
		"Channel: %s\r\n"
		"Context: %s\r\n"
		"Exten: %s\r\n"
		"Reason: %d\r\n"
		"Uniqueid: %s\r\n"
		"CallerIDNum: %s\r\n"
		"CallerIDName: %s\r\n",
		in->idtext, tris_strlen_zero(in->idtext) ? "" : "\r\n", res ? "Failure" : "Success", 
		chan ? chan->name : requested_channel, in->context, in->exten, reason, 
		chan ? chan->uniqueid : "<null>",
		S_OR(in->cid_num, "<unknown>"),
		S_OR(in->cid_name, "<unknown>")
		);

	/* Locked by tris_pbx_outgoing_exten or tris_pbx_outgoing_app */
	if (chan)
		tris_channel_unlock(chan);
	tris_free(in);
	return NULL;
}

static char mandescr_originate[] =
"Description: Generates an outgoing call to a Extension/Context/Priority or\n"
"  Application/Data\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel name to call\n"
"	Exten: Extension to use (requires 'Context' and 'Priority')\n"
"	Context: Context to use (requires 'Exten' and 'Priority')\n"
"	Priority: Priority to use (requires 'Exten' and 'Context')\n"
"	Application: Application to use\n"
"	Data: Data to use (requires 'Application')\n"
"	Timeout: How long to wait for call to be answered (in ms. Default: 30000)\n"
"	CallerID: Caller ID to be set on the outgoing channel\n"
"	Variable: Channel variable to set, multiple Variable: headers are allowed\n"
"	Account: Account code\n"
"	Async: Set to 'true' for fast origination\n";

static int action_originate(struct mansession *s, const struct message *m)
{
	const char *name = astman_get_header(m, "Channel");
	const char *exten = astman_get_header(m, "Exten");
	const char *context = astman_get_header(m, "Context");
	const char *priority = astman_get_header(m, "Priority");
	const char *timeout = astman_get_header(m, "Timeout");
	const char *callerid = astman_get_header(m, "CallerID");
	const char *account = astman_get_header(m, "Account");
	const char *app = astman_get_header(m, "Application");
	const char *appdata = astman_get_header(m, "Data");
	const char *async = astman_get_header(m, "Async");
	const char *id = astman_get_header(m, "ActionID");
	const char *codecs = astman_get_header(m, "Codecs");
	struct tris_variable *vars = astman_get_variables(m);
	char *tech, *data;
	char *l = NULL, *n = NULL;
	int pi = 0;
	int res;
	int to = 30000;
	int reason = 0;
	char tmp[256];
	char tmp2[256];
	int format = TRIS_FORMAT_SLINEAR;
	pthread_t th;
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);

	tris_verbose("name  is %s\n", name);

	if (tris_strlen_zero(name)) {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}
	if (!tris_strlen_zero(priority) && (sscanf(priority, "%30d", &pi) != 1)) {
		if ((pi = tris_findlabel_extension(NULL, context, exten, priority, NULL)) < 1) {
			astman_send_error(s, m, "Invalid priority");
			return 0;
		}
	}
	if (!tris_strlen_zero(timeout) && (sscanf(timeout, "%30d", &to) != 1)) {
		astman_send_error(s, m, "Invalid timeout");
		return 0;
	}

	snprintf(tmp, sizeof(tmp), "%s@%s:5060", name, tris_inet_ntoa(ourip));
//	tris_copy_string(tmp, name, sizeof(tmp));
	tech = tmp;
	data = strchr(tmp, '/');
	if (!data) {
		astman_send_error(s, m, "Invalid channel");
		return 0;
	}
	*data++ = '\0';
	tris_copy_string(tmp2, callerid, sizeof(tmp2));
	tris_callerid_parse(tmp2, &n, &l);
	tris_verbose("cid name is %s and cid location is %s\n",n, l);

	if (n) {
		if (tris_strlen_zero(n))
			n = NULL;
	}
	if (l) {
		tris_shrink_phone_number(l);
		if (tris_strlen_zero(l))
			l = NULL;
	}
	if (!tris_strlen_zero(codecs)) {
		format = 0;
		tris_parse_allow_disallow(NULL, &format, codecs, 1);
	}
	if (tris_true(async)) {
		struct ftris_originate_helper *fast = tris_calloc(1, sizeof(*fast));
		if (!fast) {
			res = -1;
		} else {
			if (!tris_strlen_zero(id))
				snprintf(fast->idtext, sizeof(fast->idtext), "ActionID: %s", id);
			tris_copy_string(fast->tech, tech, sizeof(fast->tech));
   			tris_copy_string(fast->data, data, sizeof(fast->data));
			tris_copy_string(fast->app, app, sizeof(fast->app));
			tris_copy_string(fast->appdata, appdata, sizeof(fast->appdata));
			if (l)
				tris_copy_string(fast->cid_num, l, sizeof(fast->cid_num));
			if (n)
				tris_copy_string(fast->cid_name, n, sizeof(fast->cid_name));
			fast->vars = vars;
			tris_copy_string(fast->context, context, sizeof(fast->context));
			tris_copy_string(fast->exten, exten, sizeof(fast->exten));
			tris_copy_string(fast->account, account, sizeof(fast->account));
			fast->format = format;
			fast->timeout = to;
			fast->priority = pi;
			if (tris_pthread_create_detached(&th, NULL, ftris_originate, fast)) {
				tris_free(fast);
				res = -1;
			} else {
				res = 0;
			}
		}
	} else if (!tris_strlen_zero(app)) {
		/* To run the System application (or anything else that goes to shell), you must have the additional System privilege */
		if (!(s->session->writeperm & EVENT_FLAG_SYSTEM)
			&& (
				strcasestr(app, "system") == 0 || /* System(rm -rf /)
				                                     TrySystem(rm -rf /)       */
				strcasestr(app, "exec") ||        /* Exec(System(rm -rf /))
				                                     TryExec(System(rm -rf /)) */
				strcasestr(app, "agi") ||         /* AGI(/bin/rm,-rf /)
				                                     EAGI(/bin/rm,-rf /)       */
				strstr(appdata, "SHELL") ||       /* NoOp(${SHELL(rm -rf /)})  */
				strstr(appdata, "EVAL")           /* NoOp(${EVAL(${some_var_containing_SHELL})}) */
				)) {
			astman_send_error(s, m, "Originate with certain 'Application' arguments requires the additional System privilege, which you do not have.");
			return 0;
		}
		res = tris_pbx_outgoing_app(tech, format, data, to, app, appdata, &reason, 1, l, n, vars, account, NULL);
	} else {
		if (exten && context && pi)
			res = tris_pbx_outgoing_exten(tech, format, data, to, context, exten, pi, &reason, 1, l, n, vars, account, NULL);
		else {
			astman_send_error(s, m, "Originate with 'Exten' requires 'Context' and 'Priority'");
			return 0;
		}
	}
	if (!res)
		astman_send_ack(s, m, "Originate successfully queued");
	else
		astman_send_error(s, m, "Originate failed");
	return 0;
}

/*! \brief Help text for manager command mailboxstatus
 */
static char mandescr_mailboxstatus[] =
"Description: Checks a voicemail account for status.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of messages.\n"
"	Message: Mailbox Status\n"
"	Mailbox: <mailboxid>\n"
"	Waiting: <count>\n"
"\n";

static int action_mailboxstatus(struct mansession *s, const struct message *m)
{
	const char *mailbox = astman_get_header(m, "Mailbox");
	int ret;

	if (tris_strlen_zero(mailbox)) {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
	ret = tris_app_has_voicemail(mailbox, NULL);
	astman_start_ack(s, m);
	astman_append(s, "Message: Mailbox Status\r\n"
			 "Mailbox: %s\r\n"
			 "Waiting: %d\r\n\r\n", mailbox, ret);
	return 0;
}

static char mandescr_mailboxcount[] =
"Description: Checks a voicemail account for new messages.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of urgent, new and old messages.\n"
"	Message: Mailbox Message Count\n"
"	Mailbox: <mailboxid>\n"
"	UrgentMessages: <count>\n"
"	NewMessages: <count>\n"
"	OldMessages: <count>\n"
"\n";
static int action_mailboxcount(struct mansession *s, const struct message *m)
{
	const char *mailbox = astman_get_header(m, "Mailbox");
	int newmsgs = 0, oldmsgs = 0, urgentmsgs = 0;;

	if (tris_strlen_zero(mailbox)) {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
	tris_app_inboxcount2(mailbox, &urgentmsgs, &newmsgs, &oldmsgs);
	astman_start_ack(s, m);
	astman_append(s,   "Message: Mailbox Message Count\r\n"
			   "Mailbox: %s\r\n"
			   "UrgMessages: %d\r\n"
			   "NewMessages: %d\r\n"
			   "OldMessages: %d\r\n"
			   "\r\n",
			   mailbox, urgentmsgs, newmsgs, oldmsgs);
	return 0;
}

static char mandescr_getvmlist[] =
"Description: Get voicemail list.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	*Folder: Folder ID <ex>INBOX, Old\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of new and old messages.\n"
"	<vmlist string>\n"
"\n";

static int action_getvmlist(struct mansession *s, const struct message *m)
{
	const char *mailbox = astman_get_header(m, "Mailbox");
	const char *folder = astman_get_header(m, "Folder");
	char vmlist[5120];
	//int newmsgs = 0, oldmsgs = 0;

	if (tris_strlen_zero(mailbox)) {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
	tris_app_get_vmlist(mailbox, folder, vmlist);
	astman_start_ack(s, m);
	astman_append(s,   "%s\r\n",
			   vmlist);
	return 0;
}

static char mandescr_managemailbox[] =
"Description: Manage Mailbox.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	*Folder: Folder ID\n"
"	*Msglist: 0,2,3,11,...\n"
"	*Command: HEARD, SAVED, DELETED\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of new and old messages.\n"
"	<vmlist string>\n"
"\n";

static int action_managemailbox(struct mansession *s, const struct message *m)
{
	const char *mailbox = astman_get_header(m, "Mailbox");
	const char *folder = astman_get_header(m, "Folder");
	const char *msgs = astman_get_header(m, "Msglist"); 
	const char *command = astman_get_header(m, "Command");
	char result[256]="";
	int folder_int=0, msglist[100], msgcount = 0;

	if (tris_strlen_zero(mailbox)) {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
	if (tris_strlen_zero(msgs)) {
		astman_send_error(s, m, "Msglist not specified");
		return 0;
	}

	if (!strcasecmp(folder, "INBOX"))
		folder_int = 0;
	else if (!strcasecmp(folder, "OLD"))
		folder_int = 1;
	else if (!strcasecmp(folder, "SAVED"))
		folder_int = 2;
	else if (!strcasecmp(folder, "DELETED"))
		folder_int = 3;
	
	char *c = (char*)msgs;
	int msgno = 0;
	while(*c != '\0') {
		if(*c == ',') {
			msglist[msgcount] = msgno;
			msgcount++;
			msgno = 0;
		}
		else {
			if(*c < '0' || *c >'9') {
				astman_start_ack(s, m);
				astman_append(s,   "%s\r\n",
						   "Faild: Message Nunber is not digits.");
				return -1;
			}
			msgno = msgno*10 + (*c-'0');
		}
		c++;
	}

	msglist[msgcount] = msgno;
	msgcount++;
	
	tris_app_manage_mailbox(mailbox, folder_int, msglist, msgcount, command, result);
	astman_start_ack(s, m);
	astman_append(s,   "%s\r\n", result);
	return 0;
}

static char mandescr_extensionstate[] =
"Description: Report the extension state for given extension.\n"
"  If the extension has a hint, will use devicestate to check\n"
"  the status of the device connected to the extension.\n"
"Variables: (Names marked with * are required)\n"
"	*Exten: Extension to check state on\n"
"	*Context: Context for extension\n"
"	ActionId: Optional ID for this transaction\n"
"Will return an \"Extension Status\" message.\n"
"The response will include the hint for the extension and the status.\n";

static int action_extensionstate(struct mansession *s, const struct message *m)
{
	const char *exten = astman_get_header(m, "Exten");
	const char *context = astman_get_header(m, "Context");
	char hint[256] = "";
	int status;
	if (tris_strlen_zero(exten)) {
		astman_send_error(s, m, "Extension not specified");
		return 0;
	}
	if (tris_strlen_zero(context))
		context = "default";
	status = tris_extension_state(NULL, context, exten);
	tris_get_hint(hint, sizeof(hint) - 1, NULL, 0, NULL, context, exten);
	astman_start_ack(s, m);
	astman_append(s,   "Message: Extension Status\r\n"
			   "Exten: %s\r\n"
			   "Context: %s\r\n"
			   "Hint: %s\r\n"
			   "Status: %d\r\n\r\n",
			   exten, context, hint, status);
	return 0;
}

static char mandescr_timeout[] =
"Description: Hangup a channel after a certain time.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel name to hangup\n"
"	*Timeout: Maximum duration of the call (sec)\n"
"Acknowledges set time with 'Timeout Set' message\n";

static int action_timeout(struct mansession *s, const struct message *m)
{
	struct tris_channel *c;
	const char *name = astman_get_header(m, "Channel");
	double timeout = atof(astman_get_header(m, "Timeout"));
	struct timeval when = { timeout, 0 };

	if (tris_strlen_zero(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if (!timeout || timeout < 0) {
		astman_send_error(s, m, "No timeout specified");
		return 0;
	}
	c = tris_get_channel_by_name_locked(name);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}

	when.tv_usec = (timeout - when.tv_sec) * 1000000.0;
	tris_channel_setwhentohangup_tv(c, when);
	tris_channel_unlock(c);
	astman_send_ack(s, m, "Timeout Set");
	return 0;
}

/*!
 * Send any applicable events to the client listening on this socket.
 * Wait only for a finite time on each event, and drop all events whether
 * they are successfully sent or not.
 */
static int process_events(struct mansession *s)
{
	int ret = 0;

	tris_mutex_lock(&s->session->__lock);
	if (s->session->f != NULL) {
		struct eventqent *eqe;

		while ( (eqe = NEW_EVENT(s)) ) {
			ref_event(eqe);
			if (!ret && s->session->authenticated &&
			    (s->session->readperm & eqe->category) == eqe->category &&
			    (s->session->send_events & eqe->category) == eqe->category) {
				if (send_string(s, eqe->eventdata) < 0)
					ret = -1;	// don't send more 
			}
			s->session->last_ev = unref_event(s->session->last_ev);
		}
	}
	tris_mutex_unlock(&s->session->__lock);
	return ret;
}

static char mandescr_userevent[] =
"Description: Send an event to manager sessions.\n"
"Variables: (Names marked with * are required)\n"
"       *UserEvent: EventStringToSend\n"
"       Header1: Content1\n"
"       HeaderN: ContentN\n";

static int action_userevent(struct mansession *s, const struct message *m)
{
	const char *event = astman_get_header(m, "UserEvent");
	struct tris_str *body = tris_str_thread_get(&userevent_buf, 16);
	int x;

	tris_str_reset(body);

	for (x = 0; x < m->hdrcount; x++) {
		if (strncasecmp("UserEvent:", m->headers[x], strlen("UserEvent:"))) {
			tris_str_append(&body, 0, "%s\r\n", m->headers[x]);
		}
	}

	astman_send_ack(s, m, "Event Sent");	
	manager_event(EVENT_FLAG_USER, "UserEvent", "UserEvent: %s\r\n%s", event, tris_str_buffer(body));
	return 0;
}

static char mandescr_coresettings[] =
"Description: Query for Core PBX settings.\n"
"Variables: (Names marked with * are optional)\n"
"       *ActionID: ActionID of this transaction\n";

/*! \brief Show PBX core settings information */
static int action_coresettings(struct mansession *s, const struct message *m)
{
	const char *actionid = astman_get_header(m, "ActionID");
	char idText[150];

	if (!tris_strlen_zero(actionid))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", actionid);
	else
		idText[0] = '\0';

	astman_append(s, "Response: Success\r\n"
			"%s"
			"AMIversion: %s\r\n"
			"TrismediaVersion: %s\r\n"
			"SystemName: %s\r\n"
			"CoreMaxCalls: %d\r\n"
			"CoreMaxLoadAvg: %f\r\n"
			"CoreRunUser: %s\r\n"
			"CoreRunGroup: %s\r\n"
			"CoreMaxFilehandles: %d\r\n" 
			"CoreRealTimeEnabled: %s\r\n"
			"CoreCDRenabled: %s\r\n"
			"CoreHTTPenabled: %s\r\n"
			"\r\n",
			idText,
			AMI_VERSION,
			tris_get_version(), 
			tris_config_TRIS_SYSTEM_NAME,
			option_maxcalls,
			option_maxload,
			tris_config_TRIS_RUN_USER,
			tris_config_TRIS_RUN_GROUP,
			option_maxfiles,
			tris_realtime_enabled() ? "Yes" : "No",
			check_cdr_enabled() ? "Yes" : "No",
			check_webmanager_enabled() ? "Yes" : "No"
			);
	return 0;
}

static char mandescr_corestatus[] =
"Description: Query for Core PBX status.\n"
"Variables: (Names marked with * are optional)\n"
"       *ActionID: ActionID of this transaction\n";

/*! \brief Show PBX core status information */
static int action_corestatus(struct mansession *s, const struct message *m)
{
	const char *actionid = astman_get_header(m, "ActionID");
	char idText[150];
	char startuptime[150];
	char reloadtime[150];
	struct tris_tm tm;

	if (!tris_strlen_zero(actionid))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", actionid);
	else
		idText[0] = '\0';

	tris_localtime(&tris_startuptime, &tm, NULL);
	tris_strftime(startuptime, sizeof(startuptime), "%H:%M:%S", &tm);
	tris_localtime(&tris_lastreloadtime, &tm, NULL);
	tris_strftime(reloadtime, sizeof(reloadtime), "%H:%M:%S", &tm);

	astman_append(s, "Response: Success\r\n"
			"%s"
			"CoreStartupTime: %s\r\n"
			"CoreReloadTime: %s\r\n"
			"CoreCurrentCalls: %d\r\n"
			"\r\n",
			idText,
			startuptime,
			reloadtime,
			tris_active_channels()
			);
	return 0;
}

static char mandescr_reload[] =
"Description: Send a reload event.\n"
"Variables: (Names marked with * are optional)\n"
"       *ActionID: ActionID of this transaction\n"
"       *Module: Name of the module to reload\n";

/*! \brief Send a reload event */
static int action_reload(struct mansession *s, const struct message *m)
{
	const char *module = astman_get_header(m, "Module");
	int res = tris_module_reload(S_OR(module, NULL));

	if (res == 2)
		astman_send_ack(s, m, "Module Reloaded");
	else
		astman_send_error(s, m, s == 0 ? "No such module" : "Module does not support reload");
	return 0;
}

static char mandescr_coreshowchannels[] =
"Description: List currently defined channels and some information\n"
"             about them.\n"
"Variables:\n"
"          ActionID: Optional Action id for message matching.\n";

/*! \brief  Manager command "CoreShowChannels" - List currently defined channels 
 *          and some information about them. */
static int action_coreshowchannels(struct mansession *s, const struct message *m)
{
	const char *actionid = astman_get_header(m, "ActionID");
	char idText[256];
	struct tris_channel *c = NULL;
	int numchans = 0;
	int duration, durh, durm, durs;

	if (!tris_strlen_zero(actionid))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", actionid);
	else
		idText[0] = '\0';

	astman_send_listack(s, m, "Channels will follow", "start");	

	while ((c = tris_channel_walk_locked(c)) != NULL) {
		struct tris_channel *bc = tris_bridged_channel(c);
		char durbuf[10] = "";

		if (c->cdr && !tris_tvzero(c->cdr->start)) {
			duration = (int)(tris_tvdiff_ms(tris_tvnow(), c->cdr->start) / 1000);
			durh = duration / 3600;
			durm = (duration % 3600) / 60;
			durs = duration % 60;
			snprintf(durbuf, sizeof(durbuf), "%02d:%02d:%02d", durh, durm, durs);
		}

		astman_append(s,
			"Event: CoreShowChannel\r\n"
			"%s"
			"Channel: %s\r\n"
			"UniqueID: %s\r\n"
			"Context: %s\r\n"
			"Extension: %s\r\n"
			"Priority: %d\r\n"
			"ChannelState: %d\r\n"
			"ChannelStateDesc: %s\r\n"
			"Application: %s\r\n"
			"ApplicationData: %s\r\n"
			"CallerIDnum: %s\r\n"
			"Duration: %s\r\n"
			"AccountCode: %s\r\n"
			"BridgedChannel: %s\r\n"
			"BridgedUniqueID: %s\r\n"
			"\r\n", idText, c->name, c->uniqueid, c->context, c->exten, c->priority, c->_state,
			tris_state2str(c->_state), c->appl ? c->appl : "", c->data ? S_OR(c->data, "") : "",
			S_OR(c->cid.cid_num, ""), durbuf, S_OR(c->accountcode, ""), bc ? bc->name : "", bc ? bc->uniqueid : "");
		tris_channel_unlock(c);
		numchans++;
	}

	astman_append(s,
		"Event: CoreShowChannelsComplete\r\n"
		"EventList: Complete\r\n"
		"ListItems: %d\r\n"
		"%s"
		"\r\n", numchans, idText);

	return 0;
}

static char mandescr_modulecheck[] = 
"Description: Checks if Trismedia module is loaded\n"
"Variables: \n"
"  ActionID: <id>          Action ID for this transaction. Will be returned.\n"
"  Module: <name>          Trismedia module name (not including extension)\n"
"\n"
"Will return Success/Failure\n"
"For success returns, the module revision number is included.\n";

/* Manager function to check if module is loaded */
static int manager_modulecheck(struct mansession *s, const struct message *m)
{
	int res;
	const char *module = astman_get_header(m, "Module");
	const char *id = astman_get_header(m, "ActionID");
	char idText[256];
#if !defined(LOW_MEMORY)
	const char *version;
#endif
	char filename[PATH_MAX];
	char *cut;

	tris_copy_string(filename, module, sizeof(filename));
	if ((cut = strchr(filename, '.'))) {
		*cut = '\0';
	} else {
		cut = filename + strlen(filename);
	}
	snprintf(cut, (sizeof(filename) - strlen(filename)) - 1, ".so");
	tris_log(LOG_DEBUG, "**** ModuleCheck .so file %s\n", filename);
	res = tris_module_check(filename);
	if (!res) {
		astman_send_error(s, m, "Module not loaded");
		return 0;
	}
	snprintf(cut, (sizeof(filename) - strlen(filename)) - 1, ".c");
	tris_log(LOG_DEBUG, "**** ModuleCheck .c file %s\n", filename);
#if !defined(LOW_MEMORY)
	version = tris_file_version_find(filename);
#endif

	if (!tris_strlen_zero(id))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", id);
	else
		idText[0] = '\0';
	astman_append(s, "Response: Success\r\n%s", idText);
#if !defined(LOW_MEMORY)
	astman_append(s, "Version: %s\r\n\r\n", version ? version : "");
#endif
	return 0;
}

static char mandescr_moduleload[] = 
"Description: Loads, unloads or reloads an Trismedia module in a running system.\n"
"Variables: \n"
"  ActionID: <id>          Action ID for this transaction. Will be returned.\n"
"  Module: <name>          Trismedia module name (including .so extension)\n"
"                          or subsystem identifier:\n"
"				cdr, enum, dnsmgr, extconfig, manager, rtp, http\n"
"  LoadType: load | unload | reload\n"
"                          The operation to be done on module\n"
" If no module is specified for a reload loadtype, all modules are reloaded";

static int manager_moduleload(struct mansession *s, const struct message *m)
{
	int res;
	const char *module = astman_get_header(m, "Module");
	const char *loadtype = astman_get_header(m, "LoadType");

	if (!loadtype || strlen(loadtype) == 0)
		astman_send_error(s, m, "Incomplete ModuleLoad action.");
	if ((!module || strlen(module) == 0) && strcasecmp(loadtype, "reload") != 0)
		astman_send_error(s, m, "Need module name");

	if (!strcasecmp(loadtype, "load")) {
		res = tris_load_resource(module);
		if (res)
			astman_send_error(s, m, "Could not load module.");
		else
			astman_send_ack(s, m, "Module loaded.");
	} else if (!strcasecmp(loadtype, "unload")) {
		res = tris_unload_resource(module, TRIS_FORCE_SOFT);
		if (res)
			astman_send_error(s, m, "Could not unload module.");
		else
			astman_send_ack(s, m, "Module unloaded.");
	} else if (!strcasecmp(loadtype, "reload")) {
		if (module != NULL) {
			res = tris_module_reload(module);
			if (res == 0)
				astman_send_error(s, m, "No such module.");
			else if (res == 1)
				astman_send_error(s, m, "Module does not support reload action.");
			else
				astman_send_ack(s, m, "Module reloaded.");
		} else {
			tris_module_reload(NULL);	/* Reload all modules */
			astman_send_ack(s, m, "All modules reloaded");
		}
	} else 
		astman_send_error(s, m, "Incomplete ModuleLoad action.");
	return 0;
}

/*
 * Done with the action handlers here, we start with the code in charge
 * of accepting connections and serving them.
 * accept_thread() forks a new thread for each connection, session_do(),
 * which in turn calls get_input() repeatedly until a full message has
 * been accumulated, and then invokes process_message() to pass it to
 * the appropriate handler.
 */

/*
 * Process an AMI message, performing desired action.
 * Return 0 on success, -1 on error that require the session to be destroyed.
 */
static int process_message(struct mansession *s, const struct message *m)
{
	char action[80] = "";
	int ret = 0;
	struct manager_action *tmp;
	const char *user = astman_get_header(m, "Username");

	tris_copy_string(action, __astman_get_header(m, "Action", GET_HEADER_SKIP_EMPTY), sizeof(action));
	tris_debug(1, "Manager received command '%s'\n", action);

	if (tris_strlen_zero(action)) {
		tris_mutex_lock(&s->session->__lock);
		astman_send_error(s, m, "Missing action in request");
		tris_mutex_unlock(&s->session->__lock);
		return 0;
	}

	if (!s->session->authenticated && strcasecmp(action, "Login") && strcasecmp(action, "Logoff") && strcasecmp(action, "Challenge")) {
		tris_mutex_lock(&s->session->__lock);
		astman_send_error(s, m, "Permission denied");
		tris_mutex_unlock(&s->session->__lock);
		return 0;
	}

	if (!allowmultiplelogin && !s->session->authenticated && user &&
		(!strcasecmp(action, "Login") || !strcasecmp(action, "Challenge"))) {
		if (check_manager_session_inuse(user)) {
			sleep(1);
			tris_mutex_lock(&s->session->__lock);
			astman_send_error(s, m, "Login Already In Use");
			tris_mutex_unlock(&s->session->__lock);
			return -1;
		}
	}

	TRIS_RWLIST_RDLOCK(&actions);
	TRIS_RWLIST_TRAVERSE(&actions, tmp, list) {
		if (strcasecmp(action, tmp->action))
			continue;
		if (s->session->writeperm & tmp->authority || tmp->authority == 0)
			ret = tmp->func(s, m);
		else
			astman_send_error(s, m, "Permission denied");
		break;
	}
	TRIS_RWLIST_UNLOCK(&actions);

	if (!tmp) {
		char buf[512];
		snprintf(buf, sizeof(buf), "Invalid/unknown command: %s. Use Action: ListCommands to show available commands.", action);
		tris_mutex_lock(&s->session->__lock);
		astman_send_error(s, m, buf);
		tris_mutex_unlock(&s->session->__lock);
	}
	if (ret)
		return ret;
	/* Once done with our message, deliver any pending events unless the
	   requester doesn't want them as part of this response.
	*/
	//if (tris_strlen_zero(astman_get_header(m, "SuppressEvents"))) {
	//	return process_events(s);
	//} else {
		return ret;
	//}
}

/*!
 * Read one full line (including crlf) from the manager socket.
 * \note \verbatim
 * \r\n is the only valid terminator for the line.
 * (Note that, later, '\0' will be considered as the end-of-line marker,
 * so everything between the '\0' and the '\r\n' will not be used).
 * Also note that we assume output to have at least "maxlen" space.
 * \endverbatim
 */
static int get_input(struct mansession *s, char *output)
{
	int res, x;
	int maxlen = sizeof(s->session->inbuf) - 1;
	char *src = s->session->inbuf;

	/*
	 * Look for \r\n within the buffer. If found, copy to the output
	 * buffer and return, trimming the \r\n (not used afterwards).
	 */
	for (x = 0; x < s->session->inlen; x++) {
		int cr;	/* set if we have \r */
		if (src[x] == '\r' && x+1 < s->session->inlen && src[x+1] == '\n')
			cr = 2;	/* Found. Update length to include \r\n */
		else if (src[x] == '\n')
			cr = 1;	/* also accept \n only */
		else
			continue;
		memmove(output, src, x);	/*... but trim \r\n */
		output[x] = '\0';		/* terminate the string */
		x += cr;			/* number of bytes used */
		s->session->inlen -= x;			/* remaining size */
		memmove(src, src + x, s->session->inlen); /* remove used bytes */
		return 1;
	}
	if (s->session->inlen >= maxlen) {
		/* no crlf found, and buffer full - sorry, too long for us */
		tris_log(LOG_WARNING, "Dumping long line with no return from %s: %s\n", tris_inet_ntoa(s->session->sin.sin_addr), src);
		s->session->inlen = 0;
	}
	res = 0;
	while (res == 0) {
		/* XXX do we really need this locking ? */
		tris_mutex_lock(&s->session->__lock);
		if (s->session->pending_event) {
			s->session->pending_event = 0;
			tris_mutex_unlock(&s->session->__lock);
			return 0;
		}
		s->session->waiting_thread = pthread_self();
		tris_mutex_unlock(&s->session->__lock);

		res = tris_wait_for_input(s->session->fd, -1);	/* return 0 on timeout ? */

		tris_mutex_lock(&s->session->__lock);
		s->session->waiting_thread = TRIS_PTHREADT_NULL;
		tris_mutex_unlock(&s->session->__lock);
	}
	if (res < 0) {
		/* If we get a signal from some other thread (typically because
		 * there are new events queued), return 0 to notify the caller.
		 */
		if (errno == EINTR || errno == EAGAIN)
			return 0;
		tris_log(LOG_WARNING, "poll() returned error: %s\n", strerror(errno));
		return -1;
	}
	tris_mutex_lock(&s->session->__lock);
	res = fread(src + s->session->inlen, 1, maxlen - s->session->inlen, s->session->f);
	if (res < 1)
		res = -1;	/* error return */
	else {
		s->session->inlen += res;
		src[s->session->inlen] = '\0';
		res = 0;
	}
	tris_mutex_unlock(&s->session->__lock);
	return res;
}

static int do_message(struct mansession *s)
{
	struct message m = { 0 };
	char header_buf[sizeof(s->session->inbuf)] = { '\0' };
	int res;

	for (;;) {
		/* Check if any events are pending and do them if needed */
		if (process_events(s))
			return -1;
		res = get_input(s, header_buf);
		if (res == 0) {
			continue;
		} else if (res > 0) {
			if (tris_strlen_zero(header_buf)) {
				res = process_message(s, &m) ? -1 : 0;
				astman_append(s,"!!!END!!!\r\n\r\n");
				if (!res && tris_strlen_zero(astman_get_header(&m, "SuppressEvents"))) {
					return process_events(s);
				} else {
					return res;
				}
			}
			else if (m.hdrcount < (TRIS_MAX_MANHEADERS - 1))
				m.headers[m.hdrcount++] = tris_strdupa(header_buf);
		} else {
			return res;
		}
	}
}

/*! \brief The body of the individual manager session.
 * Call get_input() to read one line at a time
 * (or be woken up on new events), collect the lines in a
 * message until found an empty line, and execute the request.
 * In any case, deliver events asynchronously through process_events()
 * (called from here if no line is available, or at the end of
 * process_message(). )
 */
static void *session_do(void *data)
{
	struct tris_tcptls_session_instance *ser = data;
	struct mansession_session *session = tris_calloc(1, sizeof(*session));
	struct mansession s = {.session = NULL, };
	int flags;
	int res;

	if (session == NULL)
		goto done;

	session->writetimeout = 100;
	session->waiting_thread = TRIS_PTHREADT_NULL;

	flags = fcntl(ser->fd, F_GETFL);
	if (!block_sockets) /* make sure socket is non-blocking */
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	fcntl(ser->fd, F_SETFL, flags);

	tris_mutex_init(&session->__lock);
	session->send_events = -1;
	/* Hook to the tail of the event queue */
	session->last_ev = grab_last();

	/* these fields duplicate those in the 'ser' structure */
	session->fd = ser->fd;
	session->f = ser->f;
	session->sin = ser->remote_address;
	s.session = session;

	TRIS_LIST_HEAD_INIT_NOLOCK(&session->datastores);

	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_INSERT_HEAD(&sessions, session, list);
	tris_atomic_fetchadd_int(&num_sessions, 1);
	TRIS_LIST_UNLOCK(&sessions);

	astman_append(&s, "Trismedia Call Manager/%s\r\n", AMI_VERSION);	/* welcome prompt */
	for (;;) {
		if ((res = do_message(&s)) < 0)
			break;
	}
	/* session is over, explain why and terminate */
	if (session->authenticated) {
			if (manager_displayconnects(session))
			tris_verb(2, "Manager '%s' logged off from %s\n", session->username, tris_inet_ntoa(session->sin.sin_addr));
		tris_log(LOG_EVENT, "Manager '%s' logged off from %s\n", session->username, tris_inet_ntoa(session->sin.sin_addr));
	} else {
			if (displayconnects)
			tris_verb(2, "Connect attempt from '%s' unable to authenticate\n", tris_inet_ntoa(session->sin.sin_addr));
		tris_log(LOG_EVENT, "Failed attempt from %s\n", tris_inet_ntoa(session->sin.sin_addr));
	}

	/* It is possible under certain circumstances for this session thread
	   to complete its work and exit *before* the thread that created it
	   has finished executing the tris_pthread_create_background() function.
	   If this occurs, some versions of glibc appear to act in a buggy
	   fashion and attempt to write data into memory that it thinks belongs
	   to the thread but is in fact not owned by the thread (or may have
	   been freed completely).

	   Causing this thread to yield to other threads at least one time
	   appears to work around this bug.
	*/
	usleep(1);

	destroy_session(session);

done:
	ao2_ref(ser, -1);
	ser = NULL;
	return NULL;
}

/*! \brief remove at most n_max stale session from the list. */
static void purge_sessions(int n_max)
{
	struct mansession_session *session;
	time_t now = time(NULL);

	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&sessions, session, list) {
		if (session->sessiontimeout && (now > session->sessiontimeout) && !session->inuse) {
			TRIS_LIST_REMOVE_CURRENT(list);
			tris_atomic_fetchadd_int(&num_sessions, -1);
			if (session->authenticated && (VERBOSITY_ATLEAST(2)) && manager_displayconnects(session)) {
				tris_verb(2, "HTTP Manager '%s' timed out from %s\n",
					session->username, tris_inet_ntoa(session->sin.sin_addr));
			}
			free_session(session);	/* XXX outside ? */
			if (--n_max <= 0)
				break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&sessions);
}

/*
 * events are appended to a queue from where they
 * can be dispatched to clients.
 */
static int append_event(const char *str, int category)
{
	struct eventqent *tmp = tris_malloc(sizeof(*tmp) + strlen(str));
	static int seq;	/* sequence number */

	if (!tmp)
		return -1;

	/* need to init all fields, because tris_malloc() does not */
	tmp->usecount = 0;
	tmp->category = category;
	tmp->seq = tris_atomic_fetchadd_int(&seq, 1);
	TRIS_LIST_NEXT(tmp, eq_next) = NULL;
	strcpy(tmp->eventdata, str);

	TRIS_LIST_LOCK(&all_events);
	TRIS_LIST_INSERT_TAIL(&all_events, tmp, eq_next);
	TRIS_LIST_UNLOCK(&all_events);

	return 0;
}

/* XXX see if can be moved inside the function */
TRIS_THREADSTORAGE(manager_event_buf);
#define MANAGER_EVENT_BUF_INITSIZE   256

/*! \brief  manager_event: Send AMI event to client */
int __manager_event(int category, const char *event,
	const char *file, int line, const char *func, const char *fmt, ...)
{
	struct mansession_session *session;
	struct manager_custom_hook *hook;
	struct tris_str *auth = tris_str_alloca(80);
	const char *cat_str;
	va_list ap;
	struct timeval now;
	struct tris_str *buf;

	/* Abort if there are neither any manager sessions nor hooks */
	if (!num_sessions && TRIS_RWLIST_EMPTY(&manager_hooks))
		return 0;

	if (!(buf = tris_str_thread_get(&manager_event_buf, MANAGER_EVENT_BUF_INITSIZE)))
		return -1;

	cat_str = authority_to_str(category, &auth);
	tris_str_set(&buf, 0,
			"Event: %s\r\nPrivilege: %s\r\n",
			 event, cat_str);

	if (timestampevents) {
		now = tris_tvnow();
		tris_str_append(&buf, 0,
				"Timestamp: %ld.%06lu\r\n",
				 (long)now.tv_sec, (unsigned long) now.tv_usec);
	}
	if (manager_debug) {
		static int seq;
		tris_str_append(&buf, 0,
				"SequenceNumber: %d\r\n",
				 tris_atomic_fetchadd_int(&seq, 1));
		tris_str_append(&buf, 0,
				"File: %s\r\nLine: %d\r\nFunc: %s\r\n", file, line, func);
	}

	va_start(ap, fmt);
	tris_str_append_va(&buf, 0, fmt, ap);
	va_end(ap);

	tris_str_append(&buf, 0, "\r\n");

	append_event(tris_str_buffer(buf), category);

	if (num_sessions) {
		/* Wake up any sleeping sessions */
		TRIS_LIST_LOCK(&sessions);
		TRIS_LIST_TRAVERSE(&sessions, session, list) {
			tris_mutex_lock(&session->__lock);
			if (session->waiting_thread != TRIS_PTHREADT_NULL)
				pthread_kill(session->waiting_thread, SIGURG);
			else
				/* We have an event to process, but the mansession is
				 * not waiting for it. We still need to indicate that there
				 * is an event waiting so that get_input processes the pending
				 * event instead of polling.
				 */
				session->pending_event = 1;
			tris_mutex_unlock(&session->__lock);
		}
		TRIS_LIST_UNLOCK(&sessions);
	}

	if (!TRIS_RWLIST_EMPTY(&manager_hooks)) {
		TRIS_RWLIST_RDLOCK(&manager_hooks);
		TRIS_RWLIST_TRAVERSE(&manager_hooks, hook, list) {
			hook->helper(category, event, tris_str_buffer(buf));
		}
		TRIS_RWLIST_UNLOCK(&manager_hooks);
	}

	return 0;
}

/*
 * support functions to register/unregister AMI action handlers,
 */
int tris_manager_unregister(char *action)
{
	struct manager_action *cur;
	struct timespec tv = { 5, };

	if (TRIS_RWLIST_TIMEDWRLOCK(&actions, &tv)) {
		tris_log(LOG_ERROR, "Could not obtain lock on manager list\n");
		return -1;
	}
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&actions, cur, list) {
		if (!strcasecmp(action, cur->action)) {
			TRIS_RWLIST_REMOVE_CURRENT(list);
			tris_free(cur);
			tris_verb(2, "Manager unregistered action %s\n", action);
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&actions);

	return 0;
}

static int manager_state_cb(char *context, char *exten, int state, void *data)
{
	/* Notify managers of change */
	char hint[512];
	tris_get_hint(hint, sizeof(hint), NULL, 0, NULL, context, exten);

	manager_event(EVENT_FLAG_CALL, "ExtensionStatus", "Exten: %s\r\nContext: %s\r\nHint: %s\r\nStatus: %d\r\n", exten, context, hint, state);
	return 0;
}

static int tris_manager_register_struct(struct manager_action *act)
{
	struct manager_action *cur, *prev = NULL;
	struct timespec tv = { 5, };

	if (TRIS_RWLIST_TIMEDWRLOCK(&actions, &tv)) {
		tris_log(LOG_ERROR, "Could not obtain lock on manager list\n");
		return -1;
	}
	TRIS_RWLIST_TRAVERSE(&actions, cur, list) {
		int ret = strcasecmp(cur->action, act->action);
		if (ret == 0) {
			tris_log(LOG_WARNING, "Manager: Action '%s' already registered\n", act->action);
			TRIS_RWLIST_UNLOCK(&actions);
			return -1;
		}
		if (ret > 0) { /* Insert these alphabetically */
			prev = cur;
			break;
		}
	}

	if (prev)
		TRIS_RWLIST_INSERT_AFTER(&actions, prev, act, list);
	else
		TRIS_RWLIST_INSERT_HEAD(&actions, act, list);

	tris_verb(2, "Manager registered action %s\n", act->action);

	TRIS_RWLIST_UNLOCK(&actions);

	return 0;
}

/*! \brief register a new command with manager, including online help. This is
	the preferred way to register a manager command */
int tris_manager_register2(const char *action, int auth, int (*func)(struct mansession *s, const struct message *m), const char *synopsis, const char *description)
{
	struct manager_action *cur = NULL;

	if (!(cur = tris_calloc(1, sizeof(*cur))))
		return -1;

	cur->action = action;
	cur->authority = auth;
	cur->func = func;
	cur->synopsis = synopsis;
	cur->description = description;

	if (tris_manager_register_struct(cur)) {
		tris_free(cur);
		return -1;
	}

	return 0;
}
/*! @}
 END Doxygen group */

/*
 * The following are support functions for AMI-over-http.
 * The common entry point is generic_http_callback(),
 * which extracts HTTP header and URI fields and reformats
 * them into AMI messages, locates a proper session
 * (using the mansession_id Cookie or GET variable),
 * and calls process_message() as for regular AMI clients.
 * When done, the output (which goes to a temporary file)
 * is read back into a buffer and reformatted as desired,
 * then fed back to the client over the original socket.
 */

enum output_format {
	FORMAT_RAW,
	FORMAT_HTML,
	FORMAT_XML,
};

static char *contenttype[] = {
	[FORMAT_RAW] = "plain",
	[FORMAT_HTML] = "html",
	[FORMAT_XML] =  "xml",
};

/*!
 * locate an http session in the list. The search key (ident) is
 * the value of the mansession_id cookie (0 is not valid and means
 * a session on the AMI socket).
 */
static struct mansession_session *find_session(uint32_t ident, int incinuse)
{
	struct mansession_session *session;

	if (ident == 0)
		return NULL;

	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_TRAVERSE(&sessions, session, list) {
		tris_mutex_lock(&session->__lock);
		if (session->managerid == ident && !session->needdestroy) {
			tris_atomic_fetchadd_int(&session->inuse, incinuse ? 1 : 0);
			break;
		}
		tris_mutex_unlock(&session->__lock);
	}
	TRIS_LIST_UNLOCK(&sessions);

	return session;
}

int astman_is_authed(uint32_t ident) 
{
	int authed;
	struct mansession_session *session;

	if (!(session = find_session(ident, 0)))
		return 0;

	authed = (session->authenticated != 0);

	tris_mutex_unlock(&session->__lock);

	return authed;
}

int astman_verify_session_readpermissions(uint32_t ident, int perm)
{
	int result = 0;
	struct mansession_session *session;

	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_TRAVERSE(&sessions, session, list) {
		tris_mutex_lock(&session->__lock);
		if ((session->managerid == ident) && (session->readperm & perm)) {
			result = 1;
			tris_mutex_unlock(&session->__lock);
			break;
		}
		tris_mutex_unlock(&session->__lock);
	}
	TRIS_LIST_UNLOCK(&sessions);
	return result;
}

int astman_verify_session_writepermissions(uint32_t ident, int perm)
{
	int result = 0;
	struct mansession_session *session;

	TRIS_LIST_LOCK(&sessions);
	TRIS_LIST_TRAVERSE(&sessions, session, list) {
		tris_mutex_lock(&session->__lock);
		if ((session->managerid == ident) && (session->writeperm & perm)) {
			result = 1;
			tris_mutex_unlock(&session->__lock);
			break;
		}
		tris_mutex_unlock(&session->__lock);
	}
	TRIS_LIST_UNLOCK(&sessions);
	return result;
}

/*
 * convert to xml with various conversion:
 * mode & 1	-> lowercase;
 * mode & 2	-> replace non-alphanumeric chars with underscore
 */
static void xml_copy_escape(struct tris_str **out, const char *src, int mode)
{
	/* store in a local buffer to avoid calling tris_str_append too often */
	char buf[256];
	char *dst = buf;
	int space = sizeof(buf);
	/* repeat until done and nothing to flush */
	for ( ; *src || dst != buf ; src++) {
		if (*src == '\0' || space < 10) {	/* flush */
			*dst++ = '\0';
			tris_str_append(out, 0, "%s", buf);
			dst = buf;
			space = sizeof(buf);
			if (*src == '\0')
				break;
		}
			
		if ( (mode & 2) && !isalnum(*src)) {
			*dst++ = '_';
			space--;
			continue;
		}
		switch (*src) {
		case '<':
			strcpy(dst, "&lt;");
			dst += 4;
			space -= 4;
			break;
		case '>':
			strcpy(dst, "&gt;");
			dst += 4;
			space -= 4;
			break;
		case '\"':
			strcpy(dst, "&quot;");
			dst += 6;
			space -= 6;
			break;
		case '\'':
			strcpy(dst, "&apos;");
			dst += 6;
			space -= 6;
			break;
		case '&':
			strcpy(dst, "&amp;");
			dst += 5;
			space -= 5;
			break;

		default:
			*dst++ = mode ? tolower(*src) : *src;
			space--;
		}
	}
}

struct variable_count {
	char *varname;
	int count;
};

static int compress_char(char c)
{
	c &= 0x7f;
	if (c < 32)
		return 0;
	else if (c >= 'a' && c <= 'z')
		return c - 64;
	else if (c > 'z')
		return '_';
	else
		return c - 32;
}

static int variable_count_hash_fn(const void *vvc, const int flags)
{
	const struct variable_count *vc = vvc;
	int res = 0, i;
	for (i = 0; i < 5; i++) {
		if (vc->varname[i] == '\0')
			break;
		res += compress_char(vc->varname[i]) << (i * 6);
	}
	return res;
}

static int variable_count_cmp_fn(void *obj, void *vstr, int flags)
{
	/* Due to the simplicity of struct variable_count, it makes no difference
	 * if you pass in objects or strings, the same operation applies. This is
	 * due to the fact that the hash occurs on the first element, which means
	 * the address of both the struct and the string are exactly the same. */
	struct variable_count *vc = obj;
	char *str = vstr;
	return !strcmp(vc->varname, str) ? CMP_MATCH | CMP_STOP : 0;
}

/*! \brief Convert the input into XML or HTML.
 * The input is supposed to be a sequence of lines of the form
 *	Name: value
 * optionally followed by a blob of unformatted text.
 * A blank line is a section separator. Basically, this is a
 * mixture of the format of Manager Interface and CLI commands.
 * The unformatted text is considered as a single value of a field
 * named 'Opaque-data'.
 *
 * At the moment the output format is the following (but it may
 * change depending on future requirements so don't count too
 * much on it when writing applications):
 *
 * General: the unformatted text is used as a value of
 * XML output:  to be completed
 * 
 * \verbatim
 *   Each section is within <response type="object" id="xxx">
 *   where xxx is taken from ajaxdest variable or defaults to unknown
 *   Each row is reported as an attribute Name="value" of an XML
 *   entity named from the variable ajaxobjtype, default to "generic"
 * \endverbatim
 *
 * HTML output:
 *   each Name-value pair is output as a single row of a two-column table.
 *   Sections (blank lines in the input) are separated by a <HR>
 *
 */
static void xml_translate(struct tris_str **out, char *in, struct tris_variable *vars, enum output_format format)
{
	struct tris_variable *v;
	const char *dest = NULL;
	char *var, *val;
	const char *objtype = NULL;
	int in_data = 0;	/* parsing data */
	int inobj = 0;
	int xml = (format == FORMAT_XML);
	struct variable_count *vc = NULL;
	struct ao2_container *vco = NULL;

	for (v = vars; v; v = v->next) {
		if (!dest && !strcasecmp(v->name, "ajaxdest"))
			dest = v->value;
		else if (!objtype && !strcasecmp(v->name, "ajaxobjtype"))
			objtype = v->value;
	}
	if (!dest)
		dest = "unknown";
	if (!objtype)
		objtype = "generic";

	/* we want to stop when we find an empty line */
	while (in && *in) {
		val = strsep(&in, "\r\n");	/* mark start and end of line */
		if (in && *in == '\n')		/* remove trailing \n if any */
			in++;
		tris_trim_blanks(val);
		tris_debug(5, "inobj %d in_data %d line <%s>\n", inobj, in_data, val);
		if (tris_strlen_zero(val)) {
			if (in_data) { /* close data */
				tris_str_append(out, 0, xml ? "'" : "</td></tr>\n");
				in_data = 0;
			}
			if (inobj) {
				tris_str_append(out, 0, xml ? " /></response>\n" :
					"<tr><td colspan=\"2\"><hr></td></tr>\r\n");
				inobj = 0;
				ao2_ref(vco, -1);
				vco = NULL;
			}
			continue;
		}

		/* we expect Name: value lines */
		if (in_data) {
			var = NULL;
		} else {
			var = strsep(&val, ":");
			if (val) {	/* found the field name */
				val = tris_skip_blanks(val);
				tris_trim_blanks(var);
			} else {		/* field name not found, move to opaque mode */
				val = var;
				var = "Opaque-data";
			}
		}

		if (!inobj) {
			if (xml)
				tris_str_append(out, 0, "<response type='object' id='%s'><%s", dest, objtype);
			else
				tris_str_append(out, 0, "<body>\n");
			vco = ao2_container_alloc(37, variable_count_hash_fn, variable_count_cmp_fn);
			inobj = 1;
		}

		if (!in_data) {	/* build appropriate line start */
			tris_str_append(out, 0, xml ? " " : "<tr><td>");
			if ((vc = ao2_find(vco, var, 0)))
				vc->count++;
			else {
				/* Create a new entry for this one */
				vc = ao2_alloc(sizeof(*vc), NULL);
				vc->varname = var;
				vc->count = 1;
				ao2_link(vco, vc);
			}
			xml_copy_escape(out, var, xml ? 1 | 2 : 0);
			if (vc->count > 1)
				tris_str_append(out, 0, "-%d", vc->count);
			ao2_ref(vc, -1);
			tris_str_append(out, 0, xml ? "='" : "</td><td>");
			if (!strcmp(var, "Opaque-data"))
				in_data = 1;
		}
		xml_copy_escape(out, val, 0);	/* data field */
		if (!in_data)
			tris_str_append(out, 0, xml ? "'" : "</td></tr>\n");
		else
			tris_str_append(out, 0, xml ? "\n" : "<br>\n");
	}
	if (inobj) {
		tris_str_append(out, 0, xml ? " /></response>\n" :
			"<tr><td colspan=\"2\"><hr></td></tr>\r\n");
		ao2_ref(vco, -1);
	}
}

static struct tris_str *generic_http_callback(enum output_format format,
					     struct sockaddr_in *remote_address, const char *uri, enum tris_http_method method,
					     struct tris_variable *params, int *status,
					     char **title, int *contentlength)
{
	struct mansession s = {.session = NULL, };
	struct mansession_session *session = NULL;
	uint32_t ident = 0;
	int blastaway = 0;
	struct tris_variable *v;
	char template[] = "/tmp/ast-http-XXXXXX";	/* template for temporary file */
	struct tris_str *out = NULL;
	struct message m = { 0 };
	unsigned int x;
	size_t hdrlen;

	for (v = params; v; v = v->next) {
		if (!strcasecmp(v->name, "mansession_id")) {
			sscanf(v->value, "%30x", &ident);
			break;
		}
	}

	if (!(session = find_session(ident, 1))) {
		/* Create new session.
		 * While it is not in the list we don't need any locking
		 */
		if (!(session = tris_calloc(1, sizeof(*session)))) {
			*status = 500;
			goto generic_callback_out;
		}
		session->sin = *remote_address;
		session->fd = -1;
		session->waiting_thread = TRIS_PTHREADT_NULL;
		session->send_events = 0;
		tris_mutex_init(&session->__lock);
		tris_mutex_lock(&session->__lock);
		session->inuse = 1;
		/*!\note There is approximately a 1 in 1.8E19 chance that the following
		 * calculation will produce 0, which is an invalid ID, but due to the
		 * properties of the rand() function (and the constantcy of s), that
		 * won't happen twice in a row.
		 */
		while ((session->managerid = tris_random() ^ (unsigned long) session) == 0);
		session->last_ev = grab_last();
		TRIS_LIST_HEAD_INIT_NOLOCK(&session->datastores);
		TRIS_LIST_LOCK(&sessions);
		TRIS_LIST_INSERT_HEAD(&sessions, session, list);
		tris_atomic_fetchadd_int(&num_sessions, 1);
		TRIS_LIST_UNLOCK(&sessions);
	}

	s.session = session;

	tris_mutex_unlock(&session->__lock);

	if (!(out = tris_str_create(1024))) {
		*status = 500;
		goto generic_callback_out;
	}

	s.fd = mkstemp(template);	/* create a temporary file for command output */
	unlink(template);
	s.f = fdopen(s.fd, "w+");

	for (x = 0, v = params; v && (x < TRIS_MAX_MANHEADERS); x++, v = v->next) {
		hdrlen = strlen(v->name) + strlen(v->value) + 3;
		m.headers[m.hdrcount] = alloca(hdrlen);
		snprintf((char *) m.headers[m.hdrcount], hdrlen, "%s: %s", v->name, v->value);
		tris_verb(4, "HTTP Manager add header %s\n", m.headers[m.hdrcount]);
		m.hdrcount = x + 1;
	}

	if (process_message(&s, &m)) {
		if (session->authenticated) {
			if (manager_displayconnects(session)) {
				tris_verb(2, "HTTP Manager '%s' logged off from %s\n", session->username, tris_inet_ntoa(session->sin.sin_addr));
			}
			tris_log(LOG_EVENT, "HTTP Manager '%s' logged off from %s\n", session->username, tris_inet_ntoa(session->sin.sin_addr));
		} else {
			if (displayconnects) {
				tris_verb(2, "HTTP Connect attempt from '%s' unable to authenticate\n", tris_inet_ntoa(session->sin.sin_addr));
			}
			tris_log(LOG_EVENT, "HTTP Failed attempt from %s\n", tris_inet_ntoa(session->sin.sin_addr));
		}
		session->needdestroy = 1;
	}

	tris_str_append(&out, 0,
		       "Content-type: text/%s\r\n"
		       "Cache-Control: no-cache;\r\n"
		       "Set-Cookie: mansession_id=\"%08x\"; Version=\"1\"; Max-Age=%d\r\n"
		       "Pragma: SuppressEvents\r\n"
		       "\r\n",
			contenttype[format],
			session->managerid, httptimeout);

	if (format == FORMAT_XML) {
		tris_str_append(&out, 0, "<ajax-response>\n");
	} else if (format == FORMAT_HTML) {
		/*
		 * When handling AMI-over-HTTP in HTML format, we provide a simple form for
		 * debugging purposes. This HTML code should not be here, we
		 * should read from some config file...
		 */

#define ROW_FMT	"<tr><td colspan=\"2\" bgcolor=\"#f1f1ff\">%s</td></tr>\r\n"
#define TEST_STRING \
	"<form action=\"manager\">\n\
	Action: <select name=\"action\">\n\
		<option value=\"\">-----&gt;</option>\n\
		<option value=\"login\">login</option>\n\
		<option value=\"command\">Command</option>\n\
		<option value=\"waitevent\">waitevent</option>\n\
		<option value=\"listcommands\">listcommands</option>\n\
	</select>\n\
	or <input name=\"action\"><br/>\n\
	CLI Command <input name=\"command\"><br>\n\
	user <input name=\"username\"> pass <input type=\"password\" name=\"secret\"><br>\n\
	<input type=\"submit\">\n</form>\n"

		tris_str_append(&out, 0, "<title>Trismedia&trade; Manager Interface</title>");
		tris_str_append(&out, 0, "<body bgcolor=\"#ffffff\"><table align=center bgcolor=\"#f1f1f1\" width=\"500\">\r\n");
		tris_str_append(&out, 0, ROW_FMT, "<h1>Manager Tester</h1>");
		tris_str_append(&out, 0, ROW_FMT, TEST_STRING);
	}

	if (s.f != NULL) {	/* have temporary output */
		char *buf;
		size_t l;
		
		/* Ensure buffer is NULL-terminated */
		fprintf(s.f, "%c", 0);

		if ((l = ftell(s.f))) {
			if ((buf = mmap(NULL, l, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0))) {
				if (format == FORMAT_XML || format == FORMAT_HTML)
					xml_translate(&out, buf, params, format);
				else
					tris_str_append(&out, 0, "%s", buf);
				munmap(buf, l);
			}
		} else if (format == FORMAT_XML || format == FORMAT_HTML) {
			xml_translate(&out, "", params, format);
		}
		fclose(s.f);
		s.f = NULL;
		s.fd = -1;
	}

	if (format == FORMAT_XML) {
		tris_str_append(&out, 0, "</ajax-response>\n");
	} else if (format == FORMAT_HTML)
		tris_str_append(&out, 0, "</table></body>\r\n");

	tris_mutex_lock(&session->__lock);
	/* Reset HTTP timeout.  If we're not authenticated, keep it extremely short */
	session->sessiontimeout = time(NULL) + ((session->authenticated || httptimeout < 5) ? httptimeout : 5);

	if (session->needdestroy) {
		if (session->inuse == 1) {
			tris_debug(1, "Need destroy, doing it now!\n");
			blastaway = 1;
		} else {
			tris_debug(1, "Need destroy, but can't do it yet!\n");
			if (session->waiting_thread != TRIS_PTHREADT_NULL)
				pthread_kill(session->waiting_thread, SIGURG);
			session->inuse--;
		}
	} else
		session->inuse--;
	tris_mutex_unlock(&session->__lock);

	if (blastaway)
		destroy_session(session);
generic_callback_out:
	if (*status != 200)
		return tris_http_error(500, "Server Error", NULL, "Internal Server Error (out of memory)\n");
	return out;
}

static struct tris_str *manager_http_callback(struct tris_tcptls_session_instance *ser, const struct tris_http_uri *urih, const char *uri, enum tris_http_method method, struct tris_variable *params, struct tris_variable *headers, int *status, char **title, int *contentlength)
{
	return generic_http_callback(FORMAT_HTML, &ser->remote_address, uri, method, params, status, title, contentlength);
}

static struct tris_str *mxml_http_callback(struct tris_tcptls_session_instance *ser, const struct tris_http_uri *urih, const char *uri, enum tris_http_method method, struct tris_variable *params, struct tris_variable *headers, int *status, char **title, int *contentlength)
{
	return generic_http_callback(FORMAT_XML, &ser->remote_address, uri, method, params, status, title, contentlength);
}

static struct tris_str *rawman_http_callback(struct tris_tcptls_session_instance *ser, const struct tris_http_uri *urih, const char *uri, enum tris_http_method method, struct tris_variable *params, struct tris_variable *headers, int *status, char **title, int *contentlength)
{
	return generic_http_callback(FORMAT_RAW, &ser->remote_address, uri, method, params, status, title, contentlength);
}

struct tris_http_uri rawmanuri = {
	.description = "Raw HTTP Manager Event Interface",
	.uri = "rawman",
	.callback = rawman_http_callback,
	.supports_get = 1,
	.data = NULL,
	.key = __FILE__,
};

struct tris_http_uri manageruri = {
	.description = "HTML Manager Event Interface",
	.uri = "manager",
	.callback = manager_http_callback,
	.supports_get = 1,
	.data = NULL,
	.key = __FILE__,
};

struct tris_http_uri managerxmluri = {
	.description = "XML Manager Event Interface",
	.uri = "mxml",
	.callback = mxml_http_callback,
	.supports_get = 1,
	.data = NULL,
	.key = __FILE__,
};

static int registered = 0;
static int webregged = 0;

/*! \brief cleanup code called at each iteration of server_root,
 * guaranteed to happen every 5 seconds at most
 */
static void purge_old_stuff(void *data)
{
	purge_sessions(1);
	purge_events();
}

struct tris_tls_config ami_tls_cfg;
static struct tris_tcptls_session_args ami_desc = {
	.accept_fd = -1,
	.master = TRIS_PTHREADT_NULL,
	.tls_cfg = NULL, 
	.poll_timeout = 5000,	/* wake up every 5 seconds */
	.periodic_fn = purge_old_stuff,
	.name = "AMI server",
	.accept_fn = tris_tcptls_server_root,	/* thread doing the accept() */
	.worker_fn = session_do,	/* thread handling the session */
};

static struct tris_tcptls_session_args amis_desc = {
	.accept_fd = -1,
	.master = TRIS_PTHREADT_NULL,
	.tls_cfg = &ami_tls_cfg, 
	.poll_timeout = -1,	/* the other does the periodic cleanup */
	.name = "AMI TLS server",
	.accept_fn = tris_tcptls_server_root,	/* thread doing the accept() */
	.worker_fn = session_do,	/* thread handling the session */
};

static int __init_manager(int reload)
{
	struct tris_config *ucfg = NULL, *cfg = NULL;
	const char *val;
	char *cat = NULL;
	int newhttptimeout = 60;
	int have_sslbindaddr = 0;
	struct hostent *hp;
	struct tris_hostent ahp;
	struct tris_manager_user *user = NULL;
	struct tris_variable *var;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	manager_enabled = 0;

	if (!registered) {
		/* Register default actions */
		tris_manager_register2("Ping", 0, action_ping, "Keepalive command", mandescr_ping);
		tris_manager_register2("Events", 0, action_events, "Control Event Flow", mandescr_events);
		tris_manager_register2("Logoff", 0, action_logoff, "Logoff Manager", mandescr_logoff);
		tris_manager_register2("Login", 0, action_login, "Login Manager", NULL);
		tris_manager_register2("Challenge", 0, action_challenge, "Generate Challenge for MD5 Auth", NULL);
		tris_manager_register2("Hangup", EVENT_FLAG_SYSTEM | EVENT_FLAG_CALL, action_hangup, "Hangup Channel", mandescr_hangup);
		tris_manager_register2("Status", EVENT_FLAG_SYSTEM | EVENT_FLAG_CALL | EVENT_FLAG_REPORTING, action_status, "Lists channel status", mandescr_status);
		tris_manager_register2("Setvar", EVENT_FLAG_CALL, action_setvar, "Set Channel Variable", mandescr_setvar);
		tris_manager_register2("Getvar", EVENT_FLAG_CALL | EVENT_FLAG_REPORTING, action_getvar, "Gets a Channel Variable", mandescr_getvar);
		tris_manager_register2("GetConfig", EVENT_FLAG_SYSTEM | EVENT_FLAG_CONFIG, action_getconfig, "Retrieve configuration", mandescr_getconfig);
		tris_manager_register2("GetConfigJSON", EVENT_FLAG_SYSTEM | EVENT_FLAG_CONFIG, action_getconfigjson, "Retrieve configuration (JSON format)", mandescr_getconfigjson);
		tris_manager_register2("UpdateConfig", EVENT_FLAG_CONFIG, action_updateconfig, "Update basic configuration", mandescr_updateconfig);
		tris_manager_register2("CreateConfig", EVENT_FLAG_CONFIG, action_createconfig, "Creates an empty file in the configuration directory", mandescr_createconfig);
		tris_manager_register2("ListCategories", EVENT_FLAG_CONFIG, action_listcategories, "List categories in configuration file", mandescr_listcategories);
		tris_manager_register2("Redirect", EVENT_FLAG_CALL, action_redirect, "Redirect (transfer) a call", mandescr_redirect );
		tris_manager_register2("Atxfer", EVENT_FLAG_CALL, action_atxfer, "Attended transfer", mandescr_atxfer);
		tris_manager_register2("Originate", EVENT_FLAG_ORIGINATE, action_originate, "Originate Call", mandescr_originate);
		tris_manager_register2("Command", EVENT_FLAG_COMMAND, action_command, "Execute Trismedia CLI Command", mandescr_command );
		tris_manager_register2("ExtensionState", EVENT_FLAG_CALL | EVENT_FLAG_REPORTING, action_extensionstate, "Check Extension Status", mandescr_extensionstate );
		tris_manager_register2("AbsoluteTimeout", EVENT_FLAG_SYSTEM | EVENT_FLAG_CALL, action_timeout, "Set Absolute Timeout", mandescr_timeout );
		tris_manager_register2("MailboxStatus", 0, action_mailboxstatus, "Check Mailbox", mandescr_mailboxstatus );
		tris_manager_register2("MailboxCount", 0, action_mailboxcount, "Check Mailbox Message Count", mandescr_mailboxcount );
		tris_manager_register2("GetVMList", 0, action_getvmlist, "Get VM List", mandescr_getvmlist);
		tris_manager_register2("ManageMailbox", 0, action_managemailbox, "Manage Mailbox", mandescr_managemailbox);
		tris_manager_register2("ListCommands", 0, action_listcommands, "List available manager commands", mandescr_listcommands);
		tris_manager_register2("SendText", EVENT_FLAG_CALL, action_sendtext, "Send text message to channel", mandescr_sendtext);
		tris_manager_register2("UserEvent", EVENT_FLAG_USER, action_userevent, "Send an arbitrary event", mandescr_userevent);
		tris_manager_register2("WaitEvent", 0, action_waitevent, "Wait for an event to occur", mandescr_waitevent);
		tris_manager_register2("CoreSettings", EVENT_FLAG_SYSTEM | EVENT_FLAG_REPORTING, action_coresettings, "Show PBX core settings (version etc)", mandescr_coresettings);
		tris_manager_register2("CoreStatus", EVENT_FLAG_SYSTEM | EVENT_FLAG_REPORTING, action_corestatus, "Show PBX core status variables", mandescr_corestatus);
		tris_manager_register2("Reload", EVENT_FLAG_CONFIG | EVENT_FLAG_SYSTEM, action_reload, "Send a reload event", mandescr_reload);
		tris_manager_register2("CoreShowChannels", EVENT_FLAG_SYSTEM | EVENT_FLAG_REPORTING, action_coreshowchannels, "List currently active channels", mandescr_coreshowchannels);
		tris_manager_register2("ModuleLoad", EVENT_FLAG_SYSTEM, manager_moduleload, "Module management", mandescr_moduleload);
		tris_manager_register2("ModuleCheck", EVENT_FLAG_SYSTEM, manager_modulecheck, "Check if module is loaded", mandescr_modulecheck);

		tris_cli_register_multiple(cli_manager, ARRAY_LEN(cli_manager));
		tris_extension_state_add(NULL, NULL, manager_state_cb, NULL);
		registered = 1;
		/* Append placeholder event so master_eventq never runs dry */
		append_event("Event: Placeholder\r\n\r\n", 0);
	}
	if ((cfg = tris_config_load2("manager.conf", "manager", config_flags)) == CONFIG_STATUS_FILEUNCHANGED)
		return 0;

	displayconnects = 1;
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_NOTICE, "Unable to open AMI configuration manager.conf, or configuration is invalid. Trismedia management interface (AMI) disabled.\n");
		return 0;
	}

	/* default values */
	memset(&ami_desc.local_address, 0, sizeof(struct sockaddr_in));
	memset(&amis_desc.local_address, 0, sizeof(amis_desc.local_address));
	amis_desc.local_address.sin_port = htons(5039);
	ami_desc.local_address.sin_port = htons(DEFAULT_MANAGER_PORT);

	ami_tls_cfg.enabled = 0;
	if (ami_tls_cfg.certfile)
		tris_free(ami_tls_cfg.certfile);
	ami_tls_cfg.certfile = tris_strdup(TRIS_CERTFILE);
	if (ami_tls_cfg.cipher)
		tris_free(ami_tls_cfg.cipher);
	ami_tls_cfg.cipher = tris_strdup("");

	for (var = tris_variable_browse(cfg, "general"); var; var = var->next) {
		val = var->value;
		if (!strcasecmp(var->name, "sslenable"))
			ami_tls_cfg.enabled = tris_true(val);
		else if (!strcasecmp(var->name, "sslbindport"))
			amis_desc.local_address.sin_port = htons(atoi(val));
		else if (!strcasecmp(var->name, "sslbindaddr")) {
			if ((hp = tris_gethostbyname(val, &ahp))) {
				memcpy(&amis_desc.local_address.sin_addr, hp->h_addr, sizeof(amis_desc.local_address.sin_addr));
				have_sslbindaddr = 1;
			} else {
				tris_log(LOG_WARNING, "Invalid bind address '%s'\n", val);
			}
		} else if (!strcasecmp(var->name, "sslcert")) {
			tris_free(ami_tls_cfg.certfile);
			ami_tls_cfg.certfile = tris_strdup(val);
		} else if (!strcasecmp(var->name, "sslcipher")) {
			tris_free(ami_tls_cfg.cipher);
			ami_tls_cfg.cipher = tris_strdup(val);
		} else if (!strcasecmp(var->name, "enabled")) {
			manager_enabled = tris_true(val);
		} else if (!strcasecmp(var->name, "block-sockets")) {
			block_sockets = tris_true(val);
		} else if (!strcasecmp(var->name, "webenabled")) {
			webmanager_enabled = tris_true(val);
		} else if (!strcasecmp(var->name, "port")) {
			ami_desc.local_address.sin_port = htons(atoi(val));
		} else if (!strcasecmp(var->name, "bindaddr")) {
			if (!inet_aton(val, &ami_desc.local_address.sin_addr)) {
				tris_log(LOG_WARNING, "Invalid address '%s' specified, using 0.0.0.0\n", val);
				memset(&ami_desc.local_address.sin_addr, 0, sizeof(ami_desc.local_address.sin_addr));
			}
		} else if (!strcasecmp(var->name, "allowmultiplelogin")) { 
			allowmultiplelogin = tris_true(val);
		} else if (!strcasecmp(var->name, "displayconnects")) {
			displayconnects = tris_true(val);
		} else if (!strcasecmp(var->name, "timestampevents")) {
			timestampevents = tris_true(val);
		} else if (!strcasecmp(var->name, "debug")) {
			manager_debug = tris_true(val);
		} else if (!strcasecmp(var->name, "httptimeout")) {
			newhttptimeout = atoi(val);
		} else {
			tris_log(LOG_NOTICE, "Invalid keyword <%s> = <%s> in manager.conf [general]\n",
				var->name, val);
		}	
	}

	if (manager_enabled)
		ami_desc.local_address.sin_family = AF_INET;
	if (!have_sslbindaddr)
		amis_desc.local_address.sin_addr = ami_desc.local_address.sin_addr;
	if (ami_tls_cfg.enabled)
		amis_desc.local_address.sin_family = AF_INET;

	
	TRIS_RWLIST_WRLOCK(&users);

	/* First, get users from users.conf */
	ucfg = tris_config_load2("users.conf", "manager", config_flags);
	if (ucfg && (ucfg != CONFIG_STATUS_FILEUNCHANGED) && ucfg != CONFIG_STATUS_FILEINVALID) {
		const char *hasmanager;
		int genhasmanager = tris_true(tris_variable_retrieve(ucfg, "general", "hasmanager"));

		while ((cat = tris_category_browse(ucfg, cat))) {
			if (!strcasecmp(cat, "general"))
				continue;
			
			hasmanager = tris_variable_retrieve(ucfg, cat, "hasmanager");
			if ((!hasmanager && genhasmanager) || tris_true(hasmanager)) {
				const char *user_secret = tris_variable_retrieve(ucfg, cat, "secret");
				const char *user_read = tris_variable_retrieve(ucfg, cat, "read");
				const char *user_write = tris_variable_retrieve(ucfg, cat, "write");
				const char *user_displayconnects = tris_variable_retrieve(ucfg, cat, "displayconnects");
				const char *user_writetimeout = tris_variable_retrieve(ucfg, cat, "writetimeout");
				
				/* Look for an existing entry,
				 * if none found - create one and add it to the list
				 */
				if (!(user = get_manager_by_name_locked(cat))) {
					if (!(user = tris_calloc(1, sizeof(*user))))
						break;

					/* Copy name over */
					tris_copy_string(user->username, cat, sizeof(user->username));
					/* Insert into list */
					TRIS_LIST_INSERT_TAIL(&users, user, list);
					user->ha = NULL;
					user->keep = 1;
					user->readperm = -1;
					user->writeperm = -1;
					/* Default displayconnect from [general] */
					user->displayconnects = displayconnects;
					user->writetimeout = 100;
				}

				if (!user_secret)
					user_secret = tris_variable_retrieve(ucfg, "general", "secret");
				if (!user_read)
					user_read = tris_variable_retrieve(ucfg, "general", "read");
				if (!user_write)
					user_write = tris_variable_retrieve(ucfg, "general", "write");
				if (!user_displayconnects)
					user_displayconnects = tris_variable_retrieve(ucfg, "general", "displayconnects");
				if (!user_writetimeout)
					user_writetimeout = tris_variable_retrieve(ucfg, "general", "writetimeout");

				if (!tris_strlen_zero(user_secret)) {
					if (user->secret)
						tris_free(user->secret);
					user->secret = tris_strdup(user_secret);
				}

				if (user_read)
					user->readperm = get_perm(user_read);
				if (user_write)
					user->writeperm = get_perm(user_write);
				if (user_displayconnects)
					user->displayconnects = tris_true(user_displayconnects);

				if (user_writetimeout) {
					int value = atoi(user_writetimeout);
					if (value < 100)
						tris_log(LOG_WARNING, "Invalid writetimeout value '%s' at users.conf line %d\n", var->value, var->lineno);
					else
						user->writetimeout = value;
				}
			}
		}
		tris_config_destroy(ucfg);
	}

	/* cat is NULL here in any case */

	while ((cat = tris_category_browse(cfg, cat))) {
		struct tris_ha *oldha;

		if (!strcasecmp(cat, "general"))
			continue;

		/* Look for an existing entry, if none found - create one and add it to the list */
		if (!(user = get_manager_by_name_locked(cat))) {
			if (!(user = tris_calloc(1, sizeof(*user))))
				break;
			/* Copy name over */
			tris_copy_string(user->username, cat, sizeof(user->username));

			user->ha = NULL;
			user->readperm = 0;
			user->writeperm = 0;
			/* Default displayconnect from [general] */
			user->displayconnects = displayconnects;
			user->writetimeout = 100;

			/* Insert into list */
			TRIS_RWLIST_INSERT_TAIL(&users, user, list);
		}

		/* Make sure we keep this user and don't destroy it during cleanup */
		user->keep = 1;
		oldha = user->ha;
		user->ha = NULL;

		var = tris_variable_browse(cfg, cat);
		for (; var; var = var->next) {
			if (!strcasecmp(var->name, "secret")) {
				if (user->secret)
					tris_free(user->secret);
				user->secret = tris_strdup(var->value);
			} else if (!strcasecmp(var->name, "deny") ||
				       !strcasecmp(var->name, "permit")) {
				user->ha = tris_append_ha(var->name, var->value, user->ha, NULL);
			}  else if (!strcasecmp(var->name, "read") ) {
				user->readperm = get_perm(var->value);
			}  else if (!strcasecmp(var->name, "write") ) {
				user->writeperm = get_perm(var->value);
			}  else if (!strcasecmp(var->name, "displayconnects") ) {
				user->displayconnects = tris_true(var->value);
			} else if (!strcasecmp(var->name, "writetimeout")) {
				int value = atoi(var->value);
				if (value < 100)
					tris_log(LOG_WARNING, "Invalid writetimeout value '%s' at line %d\n", var->value, var->lineno);
				else
					user->writetimeout = value;
			} else
				tris_debug(1, "%s is an unknown option.\n", var->name);
		}
		tris_free_ha(oldha);
	}
	tris_config_destroy(cfg);

	/* Perform cleanup - essentially prune out old users that no longer exist */
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&users, user, list) {
		if (user->keep) {	/* valid record. clear flag for the next round */
			user->keep = 0;
			continue;
		}
		/* We do not need to keep this user so take them out of the list */
		TRIS_RWLIST_REMOVE_CURRENT(list);
		/* Free their memory now */
		if (user->secret)
			tris_free(user->secret);
		tris_free_ha(user->ha);
		tris_free(user);
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;

	TRIS_RWLIST_UNLOCK(&users);

	if (webmanager_enabled && manager_enabled) {
		if (!webregged) {
			tris_http_uri_link(&rawmanuri);
			tris_http_uri_link(&manageruri);
			tris_http_uri_link(&managerxmluri);
			webregged = 1;
		}
	} else {
		if (webregged) {
			tris_http_uri_unlink(&rawmanuri);
			tris_http_uri_unlink(&manageruri);
			tris_http_uri_unlink(&managerxmluri);
			webregged = 0;
		}
	}

	if (newhttptimeout > 0)
		httptimeout = newhttptimeout;

	manager_event(EVENT_FLAG_SYSTEM, "Reload", "Module: Manager\r\nStatus: %s\r\nMessage: Manager reload Requested\r\n", manager_enabled ? "Enabled" : "Disabled");

	tris_tcptls_server_start(&ami_desc);
	if (tris_ssl_setup(amis_desc.tls_cfg))
		tris_tcptls_server_start(&amis_desc);
	return 0;
}

int init_manager(void)
{
	return __init_manager(0);
}

int reload_manager(void)
{
	return __init_manager(1);
}

int astman_datastore_add(struct mansession *s, struct tris_datastore *datastore)
{
	TRIS_LIST_INSERT_HEAD(&s->session->datastores, datastore, entry);

	return 0;
}

int astman_datastore_remove(struct mansession *s, struct tris_datastore *datastore)
{
	return TRIS_LIST_REMOVE(&s->session->datastores, datastore, entry) ? 0 : -1;
}

struct tris_datastore *astman_datastore_find(struct mansession *s, const struct tris_datastore_info *info, const char *uid)
{
	struct tris_datastore *datastore = NULL;
	
	if (info == NULL)
		return NULL;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&s->session->datastores, datastore, entry) {
		if (datastore->info != info) {
			continue;
		}

		if (uid == NULL) {
			/* matched by type only */
			break;
		}

		if ((datastore->uid != NULL) && !strcasecmp(uid, datastore->uid)) {
			/* Matched by type AND uid */
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	return datastore;
}
