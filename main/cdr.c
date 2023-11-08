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
 * \brief Call Detail Record API 
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \note Includes code and algorithms from the Zapata library.
 *
 * \note We do a lot of checking here in the CDR code to try to be sure we don't ever let a CDR slip
 * through our fingers somehow.  If someone allocates a CDR, it must be completely handled normally
 * or a WARNING shall be logged, so that we can best keep track of any escape condition where the CDR
 * isn't properly generated and posted.
 */


#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 242139 $")

#include <signal.h>

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/callerid.h"
#include "trismedia/manager.h"
#include "trismedia/causes.h"
#include "trismedia/linkedlists.h"
#include "trismedia/utils.h"
#include "trismedia/sched.h"
#include "trismedia/config.h"
#include "trismedia/cli.h"
#include "trismedia/stringfields.h"

/*! Default AMA flag for billing records (CDR's) */
int tris_default_amaflags = TRIS_CDR_DOCUMENTATION;
char tris_default_accountcode[TRIS_MAX_ACCOUNT_CODE];

struct tris_cdr_beitem {
	char name[20];
	char desc[80];
	tris_cdrbe be;
	TRIS_RWLIST_ENTRY(tris_cdr_beitem) list;
};

static TRIS_RWLIST_HEAD_STATIC(be_list, tris_cdr_beitem);

struct tris_cdr_batch_item {
	struct tris_cdr *cdr;
	struct tris_cdr_batch_item *next;
};

static struct tris_cdr_batch {
	int size;
	struct tris_cdr_batch_item *head;
	struct tris_cdr_batch_item *tail;
} *batch = NULL;

static struct sched_context *sched;
static int cdr_sched = -1;
static pthread_t cdr_thread = TRIS_PTHREADT_NULL;

#define BATCH_SIZE_DEFAULT 100
#define BATCH_TIME_DEFAULT 300
#define BATCH_SCHEDULER_ONLY_DEFAULT 0
#define BATCH_SAFE_SHUTDOWN_DEFAULT 1

static int enabled;		/*! Is the CDR subsystem enabled ? */
static int unanswered;
static int batchmode;
static int batchsize;
static int batchtime;
static int batchscheduleronly;
static int batchsafeshutdown;

TRIS_MUTEX_DEFINE_STATIC(cdr_batch_lock);

/* these are used to wake up the CDR thread when there's work to do */
TRIS_MUTEX_DEFINE_STATIC(cdr_pending_lock);
static tris_cond_t cdr_pending_cond;

int check_cdr_enabled()
{
	return enabled;
}

/*! Register a CDR driver. Each registered CDR driver generates a CDR 
	\return 0 on success, -1 on failure 
*/
int tris_cdr_register(const char *name, const char *desc, tris_cdrbe be)
{
	struct tris_cdr_beitem *i = NULL;

	if (!name)
		return -1;

	if (!be) {
		tris_log(LOG_WARNING, "CDR engine '%s' lacks backend\n", name);
		return -1;
	}

	TRIS_RWLIST_WRLOCK(&be_list);
	TRIS_RWLIST_TRAVERSE(&be_list, i, list) {
		if (!strcasecmp(name, i->name)) {
			tris_log(LOG_WARNING, "Already have a CDR backend called '%s'\n", name);
			TRIS_RWLIST_UNLOCK(&be_list);
			return -1;
		}
	}

	if (!(i = tris_calloc(1, sizeof(*i)))) 	
		return -1;

	i->be = be;
	tris_copy_string(i->name, name, sizeof(i->name));
	tris_copy_string(i->desc, desc, sizeof(i->desc));

	TRIS_RWLIST_INSERT_HEAD(&be_list, i, list);
	TRIS_RWLIST_UNLOCK(&be_list);

	return 0;
}

/*! unregister a CDR driver */
void tris_cdr_unregister(const char *name)
{
	struct tris_cdr_beitem *i = NULL;

	TRIS_RWLIST_WRLOCK(&be_list);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&be_list, i, list) {
		if (!strcasecmp(name, i->name)) {
			TRIS_RWLIST_REMOVE_CURRENT(list);
			tris_verb(2, "Unregistered '%s' CDR backend\n", name);
			tris_free(i);
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&be_list);
}

int tris_cdr_isset_unanswered(void)
{
	return unanswered;
}

/*! Duplicate a CDR record 
	\returns Pointer to new CDR record
*/
struct tris_cdr *tris_cdr_dup(struct tris_cdr *cdr) 
{
	struct tris_cdr *newcdr;
	
	if (!cdr) /* don't die if we get a null cdr pointer */
		return NULL;
	newcdr = tris_cdr_alloc();
	if (!newcdr)
		return NULL;

	memcpy(newcdr, cdr, sizeof(*newcdr));
	/* The varshead is unusable, volatile even, after the memcpy so we take care of that here */
	memset(&newcdr->varshead, 0, sizeof(newcdr->varshead));
	tris_cdr_copy_vars(newcdr, cdr);
	newcdr->next = NULL;

	return newcdr;
}

static const char *tris_cdr_getvar_internal(struct tris_cdr *cdr, const char *name, int recur) 
{
	if (tris_strlen_zero(name))
		return NULL;

	for (; cdr; cdr = recur ? cdr->next : NULL) {
		struct tris_var_t *variables;
		struct varshead *headp = &cdr->varshead;
		TRIS_LIST_TRAVERSE(headp, variables, entries) {
			if (!strcasecmp(name, tris_var_name(variables)))
				return tris_var_value(variables);
		}
	}

	return NULL;
}

static void cdr_get_tv(struct timeval when, const char *fmt, char *buf, int bufsize)
{
	if (fmt == NULL) {	/* raw mode */
		snprintf(buf, bufsize, "%ld.%06ld", (long)when.tv_sec, (long)when.tv_usec);
	} else {
		if (when.tv_sec) {
			struct tris_tm tm;
			
			tris_localtime(&when, &tm, NULL);
			tris_strftime(buf, bufsize, fmt, &tm);
		}
	}
}

/*! CDR channel variable retrieval */
void tris_cdr_getvar(struct tris_cdr *cdr, const char *name, char **ret, char *workspace, int workspacelen, int recur, int raw) 
{
	const char *fmt = "%Y-%m-%d %T";
	const char *varbuf;

	if (!cdr)  /* don't die if the cdr is null */
		return;

	*ret = NULL;
	/* special vars (the ones from the struct tris_cdr when requested by name) 
	   I'd almost say we should convert all the stringed vals to vars */

	if (!strcasecmp(name, "clid"))
		tris_copy_string(workspace, cdr->clid, workspacelen);
	else if (!strcasecmp(name, "src"))
		tris_copy_string(workspace, cdr->src, workspacelen);
	else if (!strcasecmp(name, "dst"))
		tris_copy_string(workspace, cdr->dst, workspacelen);
	else if (!strcasecmp(name, "dcontext"))
		tris_copy_string(workspace, cdr->dcontext, workspacelen);
	else if (!strcasecmp(name, "channel"))
		tris_copy_string(workspace, cdr->channel, workspacelen);
	else if (!strcasecmp(name, "dstchannel"))
		tris_copy_string(workspace, cdr->dstchannel, workspacelen);
	else if (!strcasecmp(name, "lastapp"))
		tris_copy_string(workspace, cdr->lastapp, workspacelen);
	else if (!strcasecmp(name, "lastdata"))
		tris_copy_string(workspace, cdr->lastdata, workspacelen);
	else if (!strcasecmp(name, "start"))
		cdr_get_tv(cdr->start, raw ? NULL : fmt, workspace, workspacelen);
	else if (!strcasecmp(name, "answer"))
		cdr_get_tv(cdr->answer, raw ? NULL : fmt, workspace, workspacelen);
	else if (!strcasecmp(name, "end"))
		cdr_get_tv(cdr->end, raw ? NULL : fmt, workspace, workspacelen);
	else if (!strcasecmp(name, "duration"))
		snprintf(workspace, workspacelen, "%ld", cdr->duration ? cdr->duration : (long)tris_tvdiff_ms(tris_tvnow(), cdr->start) / 1000);
	else if (!strcasecmp(name, "billsec"))
		snprintf(workspace, workspacelen, "%ld", cdr->billsec || cdr->answer.tv_sec == 0 ? cdr->billsec : (long)tris_tvdiff_ms(tris_tvnow(), cdr->answer) / 1000);
	else if (!strcasecmp(name, "disposition")) {
		if (raw) {
			snprintf(workspace, workspacelen, "%ld", cdr->disposition);
		} else {
			tris_copy_string(workspace, tris_cdr_disp2str(cdr->disposition), workspacelen);
		}
	} else if (!strcasecmp(name, "amaflags")) {
		if (raw) {
			snprintf(workspace, workspacelen, "%ld", cdr->amaflags);
		} else {
			tris_copy_string(workspace, tris_cdr_flags2str(cdr->amaflags), workspacelen);
		}
	} else if (!strcasecmp(name, "accountcode"))
		tris_copy_string(workspace, cdr->accountcode, workspacelen);
	else if (!strcasecmp(name, "uniqueid"))
		tris_copy_string(workspace, cdr->uniqueid, workspacelen);
	else if (!strcasecmp(name, "userfield"))
		tris_copy_string(workspace, cdr->userfield, workspacelen);
	else if ((varbuf = tris_cdr_getvar_internal(cdr, name, recur)))
		tris_copy_string(workspace, varbuf, workspacelen);
	else
		workspace[0] = '\0';

	if (!tris_strlen_zero(workspace))
		*ret = workspace;
}

/* readonly cdr variables */
static	const char *cdr_readonly_vars[] = { "clid", "src", "dst", "dcontext", "channel", "dstchannel",
				    "lastapp", "lastdata", "start", "answer", "end", "duration",
				    "billsec", "disposition", "amaflags", "accountcode", "uniqueid",
				    "userfield", NULL };
/*! Set a CDR channel variable 
	\note You can't set the CDR variables that belong to the actual CDR record, like "billsec".
*/
int tris_cdr_setvar(struct tris_cdr *cdr, const char *name, const char *value, int recur) 
{
	struct tris_var_t *newvariable;
	struct varshead *headp;
	int x;
	
	if (!cdr)  /* don't die if the cdr is null */
		return -1;
	
	for (x = 0; cdr_readonly_vars[x]; x++) {
		if (!strcasecmp(name, cdr_readonly_vars[x])) {
			tris_log(LOG_ERROR, "Attempt to set the '%s' read-only variable!.\n", name);
			return -1;
		}
	}

	if (!cdr) {
		tris_log(LOG_ERROR, "Attempt to set a variable on a nonexistent CDR record.\n");
		return -1;
	}

	for (; cdr; cdr = recur ? cdr->next : NULL) {
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_DONT_TOUCH) && tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED))
			continue;
		headp = &cdr->varshead;
		TRIS_LIST_TRAVERSE_SAFE_BEGIN(headp, newvariable, entries) {
			if (!strcasecmp(tris_var_name(newvariable), name)) {
				/* there is already such a variable, delete it */
				TRIS_LIST_REMOVE_CURRENT(entries);
				tris_var_delete(newvariable);
				break;
			}
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
		
		if (value) {
			newvariable = tris_var_assign(name, value);
			TRIS_LIST_INSERT_HEAD(headp, newvariable, entries);
		}
	}

	return 0;
}

int tris_cdr_copy_vars(struct tris_cdr *to_cdr, struct tris_cdr *from_cdr)
{
	struct tris_var_t *variables, *newvariable = NULL;
	struct varshead *headpa, *headpb;
	const char *var, *val;
	int x = 0;

	if (!to_cdr || !from_cdr) /* don't die if one of the pointers is null */
		return 0;

	headpa = &from_cdr->varshead;
	headpb = &to_cdr->varshead;

	TRIS_LIST_TRAVERSE(headpa,variables,entries) {
		if (variables &&
		    (var = tris_var_name(variables)) && (val = tris_var_value(variables)) &&
		    !tris_strlen_zero(var) && !tris_strlen_zero(val)) {
			newvariable = tris_var_assign(var, val);
			TRIS_LIST_INSERT_HEAD(headpb, newvariable, entries);
			x++;
		}
	}

	return x;
}

int tris_cdr_serialize_variables(struct tris_cdr *cdr, struct tris_str **buf, char delim, char sep, int recur) 
{
	struct tris_var_t *variables;
	const char *var, *val;
	char *tmp;
	char workspace[256];
	int total = 0, x = 0, i;

	tris_str_reset(*buf);

	for (; cdr; cdr = recur ? cdr->next : NULL) {
		if (++x > 1)
			tris_str_append(buf, 0, "\n");

		TRIS_LIST_TRAVERSE(&cdr->varshead, variables, entries) {
			if (variables &&
			    (var = tris_var_name(variables)) && (val = tris_var_value(variables)) &&
			    !tris_strlen_zero(var) && !tris_strlen_zero(val)) {
				if (tris_str_append(buf, 0, "level %d: %s%c%s%c", x, var, delim, val, sep) < 0) {
 					tris_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
 					break;
				} else
					total++;
			} else 
				break;
		}

		for (i = 0; cdr_readonly_vars[i]; i++) {
			workspace[0] = 0; /* null out the workspace, because the cdr_get_tv() won't write anything if time is NULL, so you get old vals */
			tris_cdr_getvar(cdr, cdr_readonly_vars[i], &tmp, workspace, sizeof(workspace), 0, 0);
			if (!tmp)
				continue;
			
			if (tris_str_append(buf, 0, "level %d: %s%c%s%c", x, cdr_readonly_vars[i], delim, tmp, sep) < 0) {
				tris_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
				break;
			} else
				total++;
		}
	}

	return total;
}


void tris_cdr_free_vars(struct tris_cdr *cdr, int recur)
{

	/* clear variables */
	for (; cdr; cdr = recur ? cdr->next : NULL) {
		struct tris_var_t *vardata;
		struct varshead *headp = &cdr->varshead;
		while ((vardata = TRIS_LIST_REMOVE_HEAD(headp, entries)))
			tris_var_delete(vardata);
	}
}

/*! \brief  print a warning if cdr already posted */
static void check_post(struct tris_cdr *cdr)
{
	if (!cdr)
		return;
	if (tris_test_flag(cdr, TRIS_CDR_FLAG_POSTED))
		tris_log(LOG_NOTICE, "CDR on channel '%s' already posted\n", S_OR(cdr->channel, "<unknown>"));
}

void tris_cdr_free(struct tris_cdr *cdr)
{

	while (cdr) {
		struct tris_cdr *next = cdr->next;

		tris_cdr_free_vars(cdr, 0);
		tris_free(cdr);
		cdr = next;
	}
}

/*! \brief the same as a cdr_free call, only with no checks; just get rid of it */
void tris_cdr_discard(struct tris_cdr *cdr)
{
	while (cdr) {
		struct tris_cdr *next = cdr->next;

		tris_cdr_free_vars(cdr, 0);
		tris_free(cdr);
		cdr = next;
	}
}

struct tris_cdr *tris_cdr_alloc(void)
{
	struct tris_cdr *x;
	x = tris_calloc(1, sizeof(*x));
	if (!x)
		tris_log(LOG_ERROR,"Allocation Failure for a CDR!\n");
	return x;
}

static void cdr_merge_vars(struct tris_cdr *to, struct tris_cdr *from)
{
	struct tris_var_t *variablesfrom,*variablesto;
	struct varshead *headpfrom = &to->varshead;
	struct varshead *headpto = &from->varshead;
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(headpfrom, variablesfrom, entries) {
		/* for every var in from, stick it in to */
		const char *fromvarname, *fromvarval;
		const char *tovarname = NULL, *tovarval = NULL;
		fromvarname = tris_var_name(variablesfrom);
		fromvarval = tris_var_value(variablesfrom);
		tovarname = 0;

		/* now, quick see if that var is in the 'to' cdr already */
		TRIS_LIST_TRAVERSE(headpto, variablesto, entries) {

			/* now, quick see if that var is in the 'to' cdr already */
			if ( strcasecmp(fromvarname, tris_var_name(variablesto)) == 0 ) {
				tovarname = tris_var_name(variablesto);
				tovarval = tris_var_value(variablesto);
				break;
			}
		}
		if (tovarname && strcasecmp(fromvarval,tovarval) != 0) {  /* this message here to see how irritating the userbase finds it */
			tris_log(LOG_NOTICE, "Merging CDR's: variable %s value %s dropped in favor of value %s\n", tovarname, fromvarval, tovarval);
			continue;
		} else if (tovarname && strcasecmp(fromvarval,tovarval) == 0) /* if they are the same, the job is done */
			continue;

		/* rip this var out of the from cdr, and stick it in the to cdr */
		TRIS_LIST_MOVE_CURRENT(headpto, entries);
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
}

void tris_cdr_merge(struct tris_cdr *to, struct tris_cdr *from)
{
	struct tris_cdr *zcdr;
	struct tris_cdr *lto = NULL;
	struct tris_cdr *lfrom = NULL;
	int discard_from = 0;
	
	if (!to || !from)
		return;

	/* don't merge into locked CDR's -- it's bad business */
	if (tris_test_flag(to, TRIS_CDR_FLAG_LOCKED)) {
		zcdr = to; /* safety valve? */
		while (to->next) {
			lto = to;
			to = to->next;
		}
		
		if (tris_test_flag(to, TRIS_CDR_FLAG_LOCKED)) {
			tris_log(LOG_WARNING, "Merging into locked CDR... no choice.");
			to = zcdr; /* safety-- if all there are is locked CDR's, then.... ?? */
			lto = NULL;
		}
	}

	if (tris_test_flag(from, TRIS_CDR_FLAG_LOCKED)) {
		struct tris_cdr *llfrom = NULL;
		discard_from = 1;
		if (lto) {
			/* insert the from stuff after lto */
			lto->next = from;
			lfrom = from;
			while (lfrom && lfrom->next) {
				if (!lfrom->next->next)
					llfrom = lfrom;
				lfrom = lfrom->next; 
			}
			/* rip off the last entry and put a copy of the to at the end */
			llfrom->next = to;
			from = lfrom;
		} else {
			/* save copy of the current *to cdr */
			struct tris_cdr tcdr;
			memcpy(&tcdr, to, sizeof(tcdr));
			/* copy in the locked from cdr */
			memcpy(to, from, sizeof(*to));
			lfrom = from;
			while (lfrom && lfrom->next) {
				if (!lfrom->next->next)
					llfrom = lfrom;
				lfrom = lfrom->next; 
			}
			from->next = NULL;
			/* rip off the last entry and put a copy of the to at the end */
			if (llfrom == from)
				to = to->next = tris_cdr_dup(&tcdr);
			else
				to = llfrom->next = tris_cdr_dup(&tcdr);
			from = lfrom;
		}
	}
	
	if (!tris_tvzero(from->start)) {
		if (!tris_tvzero(to->start)) {
			if (tris_tvcmp(to->start, from->start) > 0 ) {
				to->start = from->start; /* use the earliest time */
				from->start = tris_tv(0,0); /* we actively "steal" these values */
			}
			/* else nothing to do */
		} else {
			to->start = from->start;
			from->start = tris_tv(0,0); /* we actively "steal" these values */
		}
	}
	if (!tris_tvzero(from->answer)) {
		if (!tris_tvzero(to->answer)) {
			if (tris_tvcmp(to->answer, from->answer) > 0 ) {
				to->answer = from->answer; /* use the earliest time */
				from->answer = tris_tv(0,0); /* we actively "steal" these values */
			}
			/* we got the earliest answer time, so we'll settle for that? */
		} else {
			to->answer = from->answer;
			from->answer = tris_tv(0,0); /* we actively "steal" these values */
		}
	}
	if (!tris_tvzero(from->end)) {
		if (!tris_tvzero(to->end)) {
			if (tris_tvcmp(to->end, from->end) < 0 ) {
				to->end = from->end; /* use the latest time */
				from->end = tris_tv(0,0); /* we actively "steal" these values */
				to->duration = to->end.tv_sec - to->start.tv_sec;  /* don't forget to update the duration, billsec, when we set end */
				to->billsec = tris_tvzero(to->answer) ? 0 : to->end.tv_sec - to->answer.tv_sec;
			}
			/* else, nothing to do */
		} else {
			to->end = from->end;
			from->end = tris_tv(0,0); /* we actively "steal" these values */
			to->duration = to->end.tv_sec - to->start.tv_sec;
			to->billsec = tris_tvzero(to->answer) ? 0 : to->end.tv_sec - to->answer.tv_sec;
		}
	}
	if (to->disposition < from->disposition) {
		to->disposition = from->disposition;
		from->disposition = TRIS_CDR_NOANSWER;
	}
	if (tris_strlen_zero(to->lastapp) && !tris_strlen_zero(from->lastapp)) {
		tris_copy_string(to->lastapp, from->lastapp, sizeof(to->lastapp));
		from->lastapp[0] = 0; /* theft */
	}
	if (tris_strlen_zero(to->lastdata) && !tris_strlen_zero(from->lastdata)) {
		tris_copy_string(to->lastdata, from->lastdata, sizeof(to->lastdata));
		from->lastdata[0] = 0; /* theft */
	}
	if (tris_strlen_zero(to->dcontext) && !tris_strlen_zero(from->dcontext)) {
		tris_copy_string(to->dcontext, from->dcontext, sizeof(to->dcontext));
		from->dcontext[0] = 0; /* theft */
	}
	if (tris_strlen_zero(to->dstchannel) && !tris_strlen_zero(from->dstchannel)) {
		tris_copy_string(to->dstchannel, from->dstchannel, sizeof(to->dstchannel));
		from->dstchannel[0] = 0; /* theft */
	}
	if (!tris_strlen_zero(from->channel) && (tris_strlen_zero(to->channel) || !strncasecmp(from->channel, "Agent/", 6))) {
		tris_copy_string(to->channel, from->channel, sizeof(to->channel));
		from->channel[0] = 0; /* theft */
	}
	if (tris_strlen_zero(to->src) && !tris_strlen_zero(from->src)) {
		tris_copy_string(to->src, from->src, sizeof(to->src));
		from->src[0] = 0; /* theft */
	}
	if (tris_strlen_zero(to->clid) && !tris_strlen_zero(from->clid)) {
		tris_copy_string(to->clid, from->clid, sizeof(to->clid));
		from->clid[0] = 0; /* theft */
	}
	if (tris_strlen_zero(to->dst) && !tris_strlen_zero(from->dst)) {
		tris_copy_string(to->dst, from->dst, sizeof(to->dst));
		from->dst[0] = 0; /* theft */
	}
	if (!to->amaflags)
		to->amaflags = TRIS_CDR_DOCUMENTATION;
	if (!from->amaflags)
		from->amaflags = TRIS_CDR_DOCUMENTATION; /* make sure both amaflags are set to something (DOC is default) */
	if (tris_test_flag(from, TRIS_CDR_FLAG_LOCKED) || (to->amaflags == TRIS_CDR_DOCUMENTATION && from->amaflags != TRIS_CDR_DOCUMENTATION)) {
		to->amaflags = from->amaflags;
	}
	if (tris_test_flag(from, TRIS_CDR_FLAG_LOCKED) || (tris_strlen_zero(to->accountcode) && !tris_strlen_zero(from->accountcode))) {
		tris_copy_string(to->accountcode, from->accountcode, sizeof(to->accountcode));
	}
	if (tris_test_flag(from, TRIS_CDR_FLAG_LOCKED) || (tris_strlen_zero(to->userfield) && !tris_strlen_zero(from->userfield))) {
		tris_copy_string(to->userfield, from->userfield, sizeof(to->userfield));
	}
	/* flags, varsead, ? */
	cdr_merge_vars(from, to);

	if (tris_test_flag(from, TRIS_CDR_FLAG_KEEP_VARS))
		tris_set_flag(to, TRIS_CDR_FLAG_KEEP_VARS);
	if (tris_test_flag(from, TRIS_CDR_FLAG_POSTED))
		tris_set_flag(to, TRIS_CDR_FLAG_POSTED);
	if (tris_test_flag(from, TRIS_CDR_FLAG_LOCKED))
		tris_set_flag(to, TRIS_CDR_FLAG_LOCKED);
	if (tris_test_flag(from, TRIS_CDR_FLAG_CHILD))
		tris_set_flag(to, TRIS_CDR_FLAG_CHILD);
	if (tris_test_flag(from, TRIS_CDR_FLAG_POST_DISABLED))
		tris_set_flag(to, TRIS_CDR_FLAG_POST_DISABLED);

	/* last, but not least, we need to merge any forked CDRs to the 'to' cdr */
	while (from->next) {
		/* just rip 'em off the 'from' and insert them on the 'to' */
		zcdr = from->next;
		from->next = zcdr->next;
		zcdr->next = NULL;
		/* zcdr is now ripped from the current list; */
		tris_cdr_append(to, zcdr);
	}
	if (discard_from)
		tris_cdr_discard(from);
}

void tris_cdr_start(struct tris_cdr *cdr)
{
	char *chan; 

	for (; cdr; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			chan = S_OR(cdr->channel, "<unknown>");
			check_post(cdr);
			cdr->start = tris_tvnow();
		}
	}
}

void tris_cdr_answer(struct tris_cdr *cdr)
{

	for (; cdr; cdr = cdr->next) {
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_ANSLOCKED)) 
			continue;
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_DONT_TOUCH) && tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED))
			continue;
		check_post(cdr);
		if (cdr->disposition < TRIS_CDR_ANSWERED)
			cdr->disposition = TRIS_CDR_ANSWERED;
		if (tris_tvzero(cdr->answer))
			cdr->answer = tris_tvnow();
	}
}

void tris_cdr_busy(struct tris_cdr *cdr)
{

	for (; cdr; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			check_post(cdr);
			if (cdr->disposition < TRIS_CDR_BUSY)
				cdr->disposition = TRIS_CDR_BUSY;
		}
	}
}

void tris_cdr_failed(struct tris_cdr *cdr)
{
	for (; cdr; cdr = cdr->next) {
		check_post(cdr);
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			check_post(cdr);
			if (cdr->disposition < TRIS_CDR_FAILED)
				cdr->disposition = TRIS_CDR_FAILED;
		}
	}
}

void tris_cdr_noanswer(struct tris_cdr *cdr)
{
	char *chan; 

	while (cdr) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			chan = !tris_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
			if (tris_test_flag(cdr, TRIS_CDR_FLAG_POSTED))
				tris_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
			if (cdr->disposition < TRIS_CDR_NOANSWER)
				cdr->disposition = TRIS_CDR_NOANSWER;
		}
		cdr = cdr->next;
	}
}

/* everywhere tris_cdr_disposition is called, it will call tris_cdr_failed() 
   if tris_cdr_disposition returns a non-zero value */

int tris_cdr_disposition(struct tris_cdr *cdr, int cause)
{
	int res = 0;

	for (; cdr; cdr = cdr->next) {
		switch (cause) {  /* handle all the non failure, busy cases, return 0 not to set disposition,
							return -1 to set disposition to FAILED */
		case TRIS_CAUSE_BUSY:
			tris_cdr_busy(cdr);
			break;
		case TRIS_CAUSE_NO_ANSWER:
			tris_cdr_noanswer(cdr);
			break;
		case TRIS_CAUSE_NORMAL:
			break;
		default:
			res = -1;
		}
	}
	return res;
}

void tris_cdr_setdestchan(struct tris_cdr *cdr, const char *chann)
{
	for (; cdr; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			check_post(cdr);
			tris_copy_string(cdr->dstchannel, chann, sizeof(cdr->dstchannel));
		}
	}
}

void tris_cdr_setapp(struct tris_cdr *cdr, const char *app, const char *data)
{

	for (; cdr; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			check_post(cdr);
			tris_copy_string(cdr->lastapp, S_OR(app, ""), sizeof(cdr->lastapp));
			tris_copy_string(cdr->lastdata, S_OR(data, ""), sizeof(cdr->lastdata));
		}
	}
}

void tris_cdr_setanswer(struct tris_cdr *cdr, struct timeval t)
{

	for (; cdr; cdr = cdr->next) {
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_ANSLOCKED))
			continue;
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_DONT_TOUCH) && tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED))
			continue;
		check_post(cdr);
		cdr->answer = t;
	}
}

void tris_cdr_setdisposition(struct tris_cdr *cdr, long int disposition)
{

	for (; cdr; cdr = cdr->next) {
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED))
			continue;
		check_post(cdr);
		cdr->disposition = disposition;
	}
}

/* set cid info for one record */
static void set_one_cid(struct tris_cdr *cdr, struct tris_channel *c)
{
	/* Grab source from ANI or normal Caller*ID */
	const char *num = S_OR(c->cid.cid_ani, c->cid.cid_num);
	if (!cdr)
		return;
	if (!tris_strlen_zero(c->cid.cid_name)) {
		if (!tris_strlen_zero(num))	/* both name and number */
			snprintf(cdr->clid, sizeof(cdr->clid), "\"%s\" <%s>", c->cid.cid_name, num);
		else				/* only name */
			tris_copy_string(cdr->clid, c->cid.cid_name, sizeof(cdr->clid));
	} else if (!tris_strlen_zero(num)) {	/* only number */
		tris_copy_string(cdr->clid, num, sizeof(cdr->clid));
	} else {				/* nothing known */
		cdr->clid[0] = '\0';
	}
	tris_copy_string(cdr->src, S_OR(num, ""), sizeof(cdr->src));
	tris_cdr_setvar(cdr, "dnid", S_OR(c->cid.cid_dnid, ""), 0);

}
int tris_cdr_setcid(struct tris_cdr *cdr, struct tris_channel *c)
{
	for (; cdr; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED))
			set_one_cid(cdr, c);
	}
	return 0;
}

int tris_cdr_init(struct tris_cdr *cdr, struct tris_channel *c)
{
	char *chan;

	for ( ; cdr ; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			chan = S_OR(cdr->channel, "<unknown>");
			tris_copy_string(cdr->channel, c->name, sizeof(cdr->channel));
			set_one_cid(cdr, c);

			cdr->disposition = (c->_state == TRIS_STATE_UP) ?  TRIS_CDR_ANSWERED : TRIS_CDR_NOANSWER;
			cdr->amaflags = c->amaflags ? c->amaflags :  tris_default_amaflags;
			tris_copy_string(cdr->accountcode, c->accountcode, sizeof(cdr->accountcode));
			/* Destination information */
			tris_copy_string(cdr->dst, S_OR(c->macroexten,c->exten), sizeof(cdr->dst));
			tris_copy_string(cdr->dcontext, S_OR(c->macrocontext,c->context), sizeof(cdr->dcontext));
			/* Unique call identifier */
			tris_copy_string(cdr->uniqueid, c->uniqueid, sizeof(cdr->uniqueid));
		}
	}
	return 0;
}

/* Three routines were "fixed" via 10668, and later shown that 
   users were depending on this behavior. tris_cdr_end,
   tris_cdr_setvar and tris_cdr_answer are the three routines.
   While most of the other routines would not touch 
   LOCKED cdr's, these three routines were designed to
   operate on locked CDR's as a matter of course.
   I now appreciate how this plays with the ForkCDR app,
   which forms these cdr chains in the first place. 
   cdr_end is pretty key: all cdrs created are closed
   together. They only vary by start time. Arithmetically,
   users can calculate the subintervals they wish to track. */

void tris_cdr_end(struct tris_cdr *cdr)
{
	for ( ; cdr ; cdr = cdr->next) {
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_DONT_TOUCH) && tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED))
			continue;
		check_post(cdr);
		if (tris_tvzero(cdr->end))
			cdr->end = tris_tvnow();
		if (tris_tvzero(cdr->start)) {
			tris_log(LOG_WARNING, "CDR on channel '%s' has not started\n", S_OR(cdr->channel, "<unknown>"));
			cdr->disposition = TRIS_CDR_FAILED;
		} else
			cdr->duration = cdr->end.tv_sec - cdr->start.tv_sec;
		if (tris_tvzero(cdr->answer)) {
			if (cdr->disposition == TRIS_CDR_ANSWERED) {
				tris_log(LOG_WARNING, "CDR on channel '%s' has no answer time but is 'ANSWERED'\n", S_OR(cdr->channel, "<unknown>"));
				cdr->disposition = TRIS_CDR_FAILED;
			}
		} else {
			cdr->billsec = cdr->end.tv_sec - cdr->answer.tv_sec;
			if (tris_test_flag(&tris_options, TRIS_OPT_FLAG_INITIATED_SECONDS))
				cdr->billsec += cdr->end.tv_usec > cdr->answer.tv_usec ? 1 : 0;
		}
	}
}

char *tris_cdr_disp2str(int disposition)
{
	switch (disposition) {
	case TRIS_CDR_NULL:
		return "NO ANSWER"; /* by default, for backward compatibility */
	case TRIS_CDR_NOANSWER:
		return "NO ANSWER";
	case TRIS_CDR_FAILED:
		return "FAILED";		
	case TRIS_CDR_BUSY:
		return "BUSY";		
	case TRIS_CDR_ANSWERED:
		return "ANSWERED";
	}
	return "UNKNOWN";
}

/*! Converts AMA flag to printable string */
char *tris_cdr_flags2str(int flag)
{
	switch (flag) {
	case TRIS_CDR_OMIT:
		return "OMIT";
	case TRIS_CDR_BILLING:
		return "BILLING";
	case TRIS_CDR_DOCUMENTATION:
		return "DOCUMENTATION";
	}
	return "Unknown";
}

int tris_cdr_setaccount(struct tris_channel *chan, const char *account)
{
	struct tris_cdr *cdr = chan->cdr;
	char buf[BUFSIZ/2] = "";
	if (!tris_strlen_zero(chan->accountcode))
		tris_copy_string(buf, chan->accountcode, sizeof(buf));

	tris_string_field_set(chan, accountcode, account);
	for ( ; cdr ; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			tris_copy_string(cdr->accountcode, chan->accountcode, sizeof(cdr->accountcode));
		}
	}

	/* Signal change of account code to manager */
	manager_event(EVENT_FLAG_CALL, "NewAccountCode", "Channel: %s\r\nUniqueid: %s\r\nAccountCode: %s\r\nOldAccountCode: %s\r\n", chan->name, chan->uniqueid, chan->accountcode, buf);
	return 0;
}

int tris_cdr_setamaflags(struct tris_channel *chan, const char *flag)
{
	struct tris_cdr *cdr;
	int newflag = tris_cdr_amaflags2int(flag);
	if (newflag) {
		for (cdr = chan->cdr; cdr; cdr = cdr->next) {
			if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
				cdr->amaflags = newflag;
			}
		}
	}

	return 0;
}

int tris_cdr_setuserfield(struct tris_channel *chan, const char *userfield)
{
	struct tris_cdr *cdr = chan->cdr;

	for ( ; cdr ; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) 
			tris_copy_string(cdr->userfield, userfield, sizeof(cdr->userfield));
	}

	return 0;
}

int tris_cdr_appenduserfield(struct tris_channel *chan, const char *userfield)
{
	struct tris_cdr *cdr = chan->cdr;

	for ( ; cdr ; cdr = cdr->next) {
		int len = strlen(cdr->userfield);

		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED))
			tris_copy_string(cdr->userfield + len, userfield, sizeof(cdr->userfield) - len);
	}

	return 0;
}

int tris_cdr_update(struct tris_channel *c)
{
	struct tris_cdr *cdr = c->cdr;

	for ( ; cdr ; cdr = cdr->next) {
		if (!tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			set_one_cid(cdr, c);

			/* Copy account code et-al */	
			tris_copy_string(cdr->accountcode, c->accountcode, sizeof(cdr->accountcode));
			
			/* Destination information */ /* XXX privilege macro* ? */
			tris_copy_string(cdr->dst, S_OR(c->macroexten, c->exten), sizeof(cdr->dst));
			tris_copy_string(cdr->dcontext, S_OR(c->macrocontext, c->context), sizeof(cdr->dcontext));
		}
	}

	return 0;
}

int tris_cdr_amaflags2int(const char *flag)
{
	if (!strcasecmp(flag, "default"))
		return 0;
	if (!strcasecmp(flag, "omit"))
		return TRIS_CDR_OMIT;
	if (!strcasecmp(flag, "billing"))
		return TRIS_CDR_BILLING;
	if (!strcasecmp(flag, "documentation"))
		return TRIS_CDR_DOCUMENTATION;
	return -1;
}

static void post_cdr(struct tris_cdr *cdr)
{
	char *chan;
	struct tris_cdr_beitem *i;

	for ( ; cdr ; cdr = cdr->next) {
		if (!unanswered && cdr->disposition < TRIS_CDR_ANSWERED && (tris_strlen_zero(cdr->channel) || tris_strlen_zero(cdr->dstchannel))) {
			/* For people, who don't want to see unanswered single-channel events */
			tris_set_flag(cdr, TRIS_CDR_FLAG_POST_DISABLED);
			continue;
		}

		/* don't post CDRs that are for dialed channels unless those
		 * channels were originated from trismedia (pbx_spool, manager,
		 * cli) */
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_DIALED) && !tris_test_flag(cdr, TRIS_CDR_FLAG_ORIGINATED)) {
			tris_set_flag(cdr, TRIS_CDR_FLAG_POST_DISABLED);
			continue;
		}

		chan = S_OR(cdr->channel, "<unknown>");
		check_post(cdr);
		tris_set_flag(cdr, TRIS_CDR_FLAG_POSTED);
		if (tris_test_flag(cdr, TRIS_CDR_FLAG_POST_DISABLED))
			continue;
		TRIS_RWLIST_RDLOCK(&be_list);
		TRIS_RWLIST_TRAVERSE(&be_list, i, list) {
			i->be(cdr);
		}
		TRIS_RWLIST_UNLOCK(&be_list);
	}
}

void tris_cdr_reset(struct tris_cdr *cdr, struct tris_flags *_flags)
{
	struct tris_cdr *duplicate;
	struct tris_flags flags = { 0 };

	if (_flags)
		tris_copy_flags(&flags, _flags, TRIS_FLAGS_ALL);

	for ( ; cdr ; cdr = cdr->next) {
		/* Detach if post is requested */
		if (tris_test_flag(&flags, TRIS_CDR_FLAG_LOCKED) || !tris_test_flag(cdr, TRIS_CDR_FLAG_LOCKED)) {
			if (tris_test_flag(&flags, TRIS_CDR_FLAG_POSTED)) {
				tris_cdr_end(cdr);
				if ((duplicate = tris_cdr_dup(cdr))) {
					tris_cdr_detach(duplicate);
				}
				tris_set_flag(cdr, TRIS_CDR_FLAG_POSTED);
			}

			/* enable CDR only */
			if (tris_test_flag(&flags, TRIS_CDR_FLAG_POST_ENABLE)) {
				tris_clear_flag(cdr, TRIS_CDR_FLAG_POST_DISABLED);
				continue;
			}

			/* clear variables */
			if (!tris_test_flag(&flags, TRIS_CDR_FLAG_KEEP_VARS)) {
				tris_cdr_free_vars(cdr, 0);
			}

			/* Reset to initial state */
			tris_clear_flag(cdr, TRIS_FLAGS_ALL);	
			memset(&cdr->start, 0, sizeof(cdr->start));
			memset(&cdr->end, 0, sizeof(cdr->end));
			memset(&cdr->answer, 0, sizeof(cdr->answer));
			cdr->billsec = 0;
			cdr->duration = 0;
			tris_cdr_start(cdr);
			cdr->disposition = TRIS_CDR_NOANSWER;
		}
	}
}

void tris_cdr_specialized_reset(struct tris_cdr *cdr, struct tris_flags *_flags)
{
	struct tris_flags flags = { 0 };

	if (_flags)
		tris_copy_flags(&flags, _flags, TRIS_FLAGS_ALL);
	
	/* Reset to initial state */
	if (tris_test_flag(cdr, TRIS_CDR_FLAG_POST_DISABLED)) { /* But do NOT lose the NoCDR() setting */
		tris_clear_flag(cdr, TRIS_FLAGS_ALL);	
		tris_set_flag(cdr, TRIS_CDR_FLAG_POST_DISABLED);
	} else {
		tris_clear_flag(cdr, TRIS_FLAGS_ALL);	
	}
	
	memset(&cdr->start, 0, sizeof(cdr->start));
	memset(&cdr->end, 0, sizeof(cdr->end));
	memset(&cdr->answer, 0, sizeof(cdr->answer));
	cdr->billsec = 0;
	cdr->duration = 0;
	tris_cdr_start(cdr);
	cdr->disposition = TRIS_CDR_NULL;
}

struct tris_cdr *tris_cdr_append(struct tris_cdr *cdr, struct tris_cdr *newcdr) 
{
	struct tris_cdr *ret;

	if (cdr) {
		ret = cdr;

		while (cdr->next)
			cdr = cdr->next;
		cdr->next = newcdr;
	} else {
		ret = newcdr;
	}

	return ret;
}

/*! \note Don't call without cdr_batch_lock */
static void reset_batch(void)
{
	batch->size = 0;
	batch->head = NULL;
	batch->tail = NULL;
}

/*! \note Don't call without cdr_batch_lock */
static int init_batch(void)
{
	/* This is the single meta-batch used to keep track of all CDRs during the entire life of the program */
	if (!(batch = tris_malloc(sizeof(*batch))))
		return -1;

	reset_batch();

	return 0;
}

static void *do_batch_backend_process(void *data)
{
	struct tris_cdr_batch_item *processeditem;
	struct tris_cdr_batch_item *batchitem = data;

	/* Push each CDR into storage mechanism(s) and free all the memory */
	while (batchitem) {
		post_cdr(batchitem->cdr);
		tris_cdr_free(batchitem->cdr);
		processeditem = batchitem;
		batchitem = batchitem->next;
		tris_free(processeditem);
	}

	return NULL;
}

void tris_cdr_submit_batch(int do_shutdown)
{
	struct tris_cdr_batch_item *oldbatchitems = NULL;
	pthread_t batch_post_thread = TRIS_PTHREADT_NULL;

	/* if there's no batch, or no CDRs in the batch, then there's nothing to do */
	if (!batch || !batch->head)
		return;

	/* move the old CDRs aside, and prepare a new CDR batch */
	tris_mutex_lock(&cdr_batch_lock);
	oldbatchitems = batch->head;
	reset_batch();
	tris_mutex_unlock(&cdr_batch_lock);

	/* if configured, spawn a new thread to post these CDRs,
	   also try to save as much as possible if we are shutting down safely */
	if (batchscheduleronly || do_shutdown) {
		tris_debug(1, "CDR single-threaded batch processing begins now\n");
		do_batch_backend_process(oldbatchitems);
	} else {
		if (tris_pthread_create_detached_background(&batch_post_thread, NULL, do_batch_backend_process, oldbatchitems)) {
			tris_log(LOG_WARNING, "CDR processing thread could not detach, now trying in this thread\n");
			do_batch_backend_process(oldbatchitems);
		} else {
			tris_debug(1, "CDR multi-threaded batch processing begins now\n");
		}
	}
}

static int submit_scheduled_batch(const void *data)
{
	tris_cdr_submit_batch(0);
	/* manually reschedule from this point in time */
	cdr_sched = tris_sched_add(sched, batchtime * 1000, submit_scheduled_batch, NULL);
	/* returning zero so the scheduler does not automatically reschedule */
	return 0;
}

static void submit_unscheduled_batch(void)
{
	/* this is okay since we are not being called from within the scheduler */
	TRIS_SCHED_DEL(sched, cdr_sched);
	/* schedule the submission to occur ASAP (1 ms) */
	cdr_sched = tris_sched_add(sched, 1, submit_scheduled_batch, NULL);
	/* signal the do_cdr thread to wakeup early and do some work (that lazy thread ;) */
	tris_mutex_lock(&cdr_pending_lock);
	tris_cond_signal(&cdr_pending_cond);
	tris_mutex_unlock(&cdr_pending_lock);
}

void tris_cdr_detach(struct tris_cdr *cdr)
{
	struct tris_cdr_batch_item *newtail;
	int curr;

	if (!cdr)
		return;

	/* maybe they disabled CDR stuff completely, so just drop it */
	if (!enabled) {
		tris_debug(1, "Dropping CDR !\n");
		tris_set_flag(cdr, TRIS_CDR_FLAG_POST_DISABLED);
		tris_cdr_free(cdr);
		return;
	}

	/* post stuff immediately if we are not in batch mode, this is legacy behaviour */
	if (!batchmode) {
		post_cdr(cdr);
		tris_cdr_free(cdr);
		return;
	}

	/* otherwise, each CDR gets put into a batch list (at the end) */
	tris_debug(1, "CDR detaching from this thread\n");

	/* we'll need a new tail for every CDR */
	if (!(newtail = tris_calloc(1, sizeof(*newtail)))) {
		post_cdr(cdr);
		tris_cdr_free(cdr);
		return;
	}

	/* don't traverse a whole list (just keep track of the tail) */
	tris_mutex_lock(&cdr_batch_lock);
	if (!batch)
		init_batch();
	if (!batch->head) {
		/* new batch is empty, so point the head at the new tail */
		batch->head = newtail;
	} else {
		/* already got a batch with something in it, so just append a new tail */
		batch->tail->next = newtail;
	}
	newtail->cdr = cdr;
	batch->tail = newtail;
	curr = batch->size++;
	tris_mutex_unlock(&cdr_batch_lock);

	/* if we have enough stuff to post, then do it */
	if (curr >= (batchsize - 1))
		submit_unscheduled_batch();
}

static void *do_cdr(void *data)
{
	struct timespec timeout;
	int schedms;
	int numevents = 0;

	for (;;) {
		struct timeval now;
		schedms = tris_sched_wait(sched);
		/* this shouldn't happen, but provide a 1 second default just in case */
		if (schedms <= 0)
			schedms = 1000;
		now = tris_tvadd(tris_tvnow(), tris_samp2tv(schedms, 1000));
		timeout.tv_sec = now.tv_sec;
		timeout.tv_nsec = now.tv_usec * 1000;
		/* prevent stuff from clobbering cdr_pending_cond, then wait on signals sent to it until the timeout expires */
		tris_mutex_lock(&cdr_pending_lock);
		tris_cond_timedwait(&cdr_pending_cond, &cdr_pending_lock, &timeout);
		numevents = tris_sched_runq(sched);
		tris_mutex_unlock(&cdr_pending_lock);
		tris_debug(2, "Processed %d scheduled CDR batches from the run queue\n", numevents);
	}

	return NULL;
}

static char *handle_cli_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_cdr_beitem *beitem=NULL;
	int cnt=0;
	long nextbatchtime=0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "cdr show status";
		e->usage = 
			"Usage: cdr show status\n"
			"	Displays the Call Detail Record engine system status.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc > 3)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "\n");
	tris_cli(a->fd, "Call Detail Record (CDR) settings\n");
	tris_cli(a->fd, "----------------------------------\n");
	tris_cli(a->fd, "  Logging:                    %s\n", enabled ? "Enabled" : "Disabled");
	tris_cli(a->fd, "  Mode:                       %s\n", batchmode ? "Batch" : "Simple");
	if (enabled) {
		tris_cli(a->fd, "  Log unanswered calls:       %s\n\n", unanswered ? "Yes" : "No");
		if (batchmode) {
			tris_cli(a->fd, "* Batch Mode Settings\n");
			tris_cli(a->fd, "  -------------------\n");
			if (batch)
				cnt = batch->size;
			if (cdr_sched > -1)
				nextbatchtime = tris_sched_when(sched, cdr_sched);
			tris_cli(a->fd, "  Safe shutdown:              %s\n", batchsafeshutdown ? "Enabled" : "Disabled");
			tris_cli(a->fd, "  Threading model:            %s\n", batchscheduleronly ? "Scheduler only" : "Scheduler plus separate threads");
			tris_cli(a->fd, "  Current batch size:         %d record%s\n", cnt, ESS(cnt));
			tris_cli(a->fd, "  Maximum batch size:         %d record%s\n", batchsize, ESS(batchsize));
			tris_cli(a->fd, "  Maximum batch time:         %d second%s\n", batchtime, ESS(batchtime));
			tris_cli(a->fd, "  Next batch processing time: %ld second%s\n\n", nextbatchtime, ESS(nextbatchtime));
		}
		tris_cli(a->fd, "* Registered Backends\n");
		tris_cli(a->fd, "  -------------------\n");
		TRIS_RWLIST_RDLOCK(&be_list);
		if (TRIS_RWLIST_EMPTY(&be_list)) {
			tris_cli(a->fd, "    (none)\n");
		} else {
			TRIS_RWLIST_TRAVERSE(&be_list, beitem, list) {
				tris_cli(a->fd, "    %s\n", beitem->name);
			}
		}
		TRIS_RWLIST_UNLOCK(&be_list);
		tris_cli(a->fd, "\n");
	}

	return CLI_SUCCESS;
}

static char *handle_cli_submit(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "cdr submit";
		e->usage = 
			"Usage: cdr submit\n"
			"       Posts all pending batched CDR data to the configured CDR backend engine modules.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc > 2)
		return CLI_SHOWUSAGE;

	submit_unscheduled_batch();
	tris_cli(a->fd, "Submitted CDRs to backend engines for processing.  This may take a while.\n");

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_submit = TRIS_CLI_DEFINE(handle_cli_submit, "Posts all pending batched CDR data");
static struct tris_cli_entry cli_status = TRIS_CLI_DEFINE(handle_cli_status, "Display the CDR status");

static int do_reload(int reload)
{
	struct tris_config *config;
	const char *enabled_value;
	const char *unanswered_value;
	const char *batched_value;
	const char *scheduleronly_value;
	const char *batchsafeshutdown_value;
	const char *size_value;
	const char *time_value;
	const char *end_before_h_value;
	const char *initiatedseconds_value;
	int cfg_size;
	int cfg_time;
	int was_enabled;
	int was_batchmode;
	int res=0;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	if ((config = tris_config_load2("cdr.conf", "cdr", config_flags)) == CONFIG_STATUS_FILEUNCHANGED)
		return 0;
	if (config == CONFIG_STATUS_FILEMISSING || config == CONFIG_STATUS_FILEUNCHANGED || config == CONFIG_STATUS_FILEINVALID) {
		return 0;
	}

	tris_mutex_lock(&cdr_batch_lock);

	batchsize = BATCH_SIZE_DEFAULT;
	batchtime = BATCH_TIME_DEFAULT;
	batchscheduleronly = BATCH_SCHEDULER_ONLY_DEFAULT;
	batchsafeshutdown = BATCH_SAFE_SHUTDOWN_DEFAULT;
	was_enabled = enabled;
	was_batchmode = batchmode;
	enabled = 1;
	batchmode = 0;

	/* don't run the next scheduled CDR posting while reloading */
	TRIS_SCHED_DEL(sched, cdr_sched);

	if (config) {
		if ((enabled_value = tris_variable_retrieve(config, "general", "enable"))) {
			enabled = tris_true(enabled_value);
		}
		if ((unanswered_value = tris_variable_retrieve(config, "general", "unanswered"))) {
			unanswered = tris_true(unanswered_value);
		}
		if ((batched_value = tris_variable_retrieve(config, "general", "batch"))) {
			batchmode = tris_true(batched_value);
		}
		if ((scheduleronly_value = tris_variable_retrieve(config, "general", "scheduleronly"))) {
			batchscheduleronly = tris_true(scheduleronly_value);
		}
		if ((batchsafeshutdown_value = tris_variable_retrieve(config, "general", "safeshutdown"))) {
			batchsafeshutdown = tris_true(batchsafeshutdown_value);
		}
		if ((size_value = tris_variable_retrieve(config, "general", "size"))) {
			if (sscanf(size_value, "%30d", &cfg_size) < 1)
				tris_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", size_value);
			else if (cfg_size < 0)
				tris_log(LOG_WARNING, "Invalid maximum batch size '%d' specified, using default\n", cfg_size);
			else
				batchsize = cfg_size;
		}
		if ((time_value = tris_variable_retrieve(config, "general", "time"))) {
			if (sscanf(time_value, "%30d", &cfg_time) < 1)
				tris_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", time_value);
			else if (cfg_time < 0)
				tris_log(LOG_WARNING, "Invalid maximum batch time '%d' specified, using default\n", cfg_time);
			else
				batchtime = cfg_time;
		}
		if ((end_before_h_value = tris_variable_retrieve(config, "general", "endbeforehexten")))
			tris_set2_flag(&tris_options, tris_true(end_before_h_value), TRIS_OPT_FLAG_END_CDR_BEFORE_H_EXTEN);
		if ((initiatedseconds_value = tris_variable_retrieve(config, "general", "initiatedseconds")))
			tris_set2_flag(&tris_options, tris_true(initiatedseconds_value), TRIS_OPT_FLAG_INITIATED_SECONDS);
	}

	if (enabled && !batchmode) {
		tris_log(LOG_NOTICE, "CDR simple logging enabled.\n");
	} else if (enabled && batchmode) {
		cdr_sched = tris_sched_add(sched, batchtime * 1000, submit_scheduled_batch, NULL);
		tris_log(LOG_NOTICE, "CDR batch mode logging enabled, first of either size %d or time %d seconds.\n", batchsize, batchtime);
	} else {
		tris_log(LOG_NOTICE, "CDR logging disabled, data will be lost.\n");
	}

	/* if this reload enabled the CDR batch mode, create the background thread
	   if it does not exist */
	if (enabled && batchmode && (!was_enabled || !was_batchmode) && (cdr_thread == TRIS_PTHREADT_NULL)) {
		tris_cond_init(&cdr_pending_cond, NULL);
		if (tris_pthread_create_background(&cdr_thread, NULL, do_cdr, NULL) < 0) {
			tris_log(LOG_ERROR, "Unable to start CDR thread.\n");
			TRIS_SCHED_DEL(sched, cdr_sched);
		} else {
			tris_cli_register(&cli_submit);
			tris_register_atexit(tris_cdr_engine_term);
			res = 0;
		}
	/* if this reload disabled the CDR and/or batch mode and there is a background thread,
	   kill it */
	} else if (((!enabled && was_enabled) || (!batchmode && was_batchmode)) && (cdr_thread != TRIS_PTHREADT_NULL)) {
		/* wake up the thread so it will exit */
		pthread_cancel(cdr_thread);
		pthread_kill(cdr_thread, SIGURG);
		pthread_join(cdr_thread, NULL);
		cdr_thread = TRIS_PTHREADT_NULL;
		tris_cond_destroy(&cdr_pending_cond);
		tris_cli_unregister(&cli_submit);
		tris_unregister_atexit(tris_cdr_engine_term);
		res = 0;
		/* if leaving batch mode, then post the CDRs in the batch,
		   and don't reschedule, since we are stopping CDR logging */
		if (!batchmode && was_batchmode) {
			tris_cdr_engine_term();
		}
	} else {
		res = 0;
	}

	tris_mutex_unlock(&cdr_batch_lock);
	tris_config_destroy(config);
	manager_event(EVENT_FLAG_SYSTEM, "Reload", "Module: CDR\r\nMessage: CDR subsystem reload requested\r\n");

	return res;
}

int tris_cdr_engine_init(void)
{
	int res;

	sched = sched_context_create();
	if (!sched) {
		tris_log(LOG_ERROR, "Unable to create schedule context.\n");
		return -1;
	}

	tris_cli_register(&cli_status);

	res = do_reload(0);
	if (res) {
		tris_mutex_lock(&cdr_batch_lock);
		res = init_batch();
		tris_mutex_unlock(&cdr_batch_lock);
	}

	return res;
}

/* \note This actually gets called a couple of times at shutdown.  Once, before we start
   hanging up channels, and then again, after the channel hangup timeout expires */
void tris_cdr_engine_term(void)
{
	tris_cdr_submit_batch(batchsafeshutdown);
}

int tris_cdr_engine_reload(void)
{
	return do_reload(1);
}

