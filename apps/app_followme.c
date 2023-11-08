/*
 * Trismedia -- A telephony toolkit for Linux.
 *
 * A full-featured Find-Me/Follow-Me Application
 * 
 * Copyright (C) 2005-2006, BJ Weschke All Rights Reserved.
 *
 * BJ Weschke <bweschke@btwtech.com>
 *
 * This code is released by the author with no restrictions on usage.
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 *
 * \brief Find-Me Follow-Me application
 *
 * \author BJ Weschke <bweschke@btwtech.com>
 *
 * \arg See \ref Config_followme
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>chan_local</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 218582 $")

#include <signal.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_SPOOL_DIR */
#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/say.h"
#include "trismedia/features.h"
#include "trismedia/musiconhold.h"
#include "trismedia/cli.h"
#include "trismedia/manager.h"
#include "trismedia/config.h"
#include "trismedia/monitor.h"
#include "trismedia/utils.h"
#include "trismedia/causes.h"
#include "trismedia/astdb.h"
#include "trismedia/dsp.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="FollowMe" language="en_US">
		<synopsis>
			Find-Me/Follow-Me application.
		</synopsis>
		<syntax>
			<parameter name="followmeid" required="true" />
			<parameter name="options">
				<optionlist>
					<option name="s">
						<para>Playback the incoming status message prior to starting
						the follow-me step(s)</para>
					</option>
					<option name="a">
						<para>Record the caller's name so it can be announced to the
						callee on each step.</para>
					</option>
					<option name="n">
						<para>Playback the unreachable status message if we've run out
						of steps to reach the or the callee has elected not to be reachable.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>This application performs Find-Me/Follow-Me functionality for the caller
			as defined in the profile matching the <replaceable>followmeid</replaceable> parameter in
			<filename>followme.conf</filename>. If the specified <replaceable>followmeid</replaceable>
			profile doesn't exist in <filename>followme.conf</filename>, execution will be returned
			to the dialplan and call execution will continue at the next priority.</para>
			<para>Returns -1 on hangup.</para>
		</description>
	</application>
 ***/

static char *app = "FollowMe";

/*! \brief Number structure */
struct number {
	char number[512];	/*!< Phone Number(s) and/or Extension(s) */
	long timeout;		/*!< Dial Timeout, if used. */
	int order;		/*!< The order to dial in */
	TRIS_LIST_ENTRY(number) entry; /*!< Next Number record */
};

/*! \brief Data structure for followme scripts */
struct call_followme {
	tris_mutex_t lock;
	char name[TRIS_MAX_EXTENSION];	/*!< Name - FollowMeID */
	char moh[TRIS_MAX_CONTEXT];	/*!< Music On Hold Class to be used */
	char context[TRIS_MAX_CONTEXT];  /*!< Context to dial from */
	unsigned int active;		/*!< Profile is active (1), or disabled (0). */
	int realtime;           /*!< Cached from realtime */
	char takecall[20];		/*!< Digit mapping to take a call */
	char nextindp[20];		/*!< Digit mapping to decline a call */
	char callfromprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char norecordingprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char optionsprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char plsholdprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char statusprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char sorryprompt[PATH_MAX];	/*!< Sound prompt name and path */

	TRIS_LIST_HEAD_NOLOCK(numbers, number) numbers;	   /*!< Head of the list of follow-me numbers */
	TRIS_LIST_HEAD_NOLOCK(blnumbers, number) blnumbers; /*!< Head of the list of black-listed numbers */
	TRIS_LIST_HEAD_NOLOCK(wlnumbers, number) wlnumbers; /*!< Head of the list of white-listed numbers */
	TRIS_LIST_ENTRY(call_followme) entry;           /*!< Next Follow-Me record */
};

struct fm_args {
	struct tris_channel *chan;
	char *mohclass;
	TRIS_LIST_HEAD_NOLOCK(cnumbers, number) cnumbers;
	int status;
	char context[TRIS_MAX_CONTEXT];
	char namerecloc[TRIS_MAX_CONTEXT];
	struct tris_channel *outbound;
	char takecall[20];		/*!< Digit mapping to take a call */
	char nextindp[20];		/*!< Digit mapping to decline a call */
	char callfromprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char norecordingprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char optionsprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char plsholdprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char statusprompt[PATH_MAX];	/*!< Sound prompt name and path */
	char sorryprompt[PATH_MAX];	/*!< Sound prompt name and path */
	struct tris_flags followmeflags;
};

struct findme_user {
	struct tris_channel *ochan;
	int state;
	char dialarg[256];
	char yn[10];
	int ynidx; 
	long digts;
	int cleared;
	TRIS_LIST_ENTRY(findme_user) entry;
};

enum {
	FOLLOWMEFLAG_STATUSMSG = (1 << 0),
	FOLLOWMEFLAG_RECORDNAME = (1 << 1),
	FOLLOWMEFLAG_UNREACHABLEMSG = (1 << 2)
};

TRIS_APP_OPTIONS(followme_opts, {
	TRIS_APP_OPTION('s', FOLLOWMEFLAG_STATUSMSG ),
	TRIS_APP_OPTION('a', FOLLOWMEFLAG_RECORDNAME ),
	TRIS_APP_OPTION('n', FOLLOWMEFLAG_UNREACHABLEMSG ),
});

static int ynlongest = 0;

static const char *featuredigittostr;
static int featuredigittimeout = 5000;		/*!< Feature Digit Timeout */
static const char *defaultmoh = "default";    	/*!< Default Music-On-Hold Class */

static char takecall[20] = "1", nextindp[20] = "2";
static char callfromprompt[PATH_MAX] = "followme/call-from";
static char norecordingprompt[PATH_MAX] = "followme/no-recording";
static char optionsprompt[PATH_MAX] = "followme/options";
static char plsholdprompt[PATH_MAX] = "followme/pls-hold-while-try";
static char statusprompt[PATH_MAX] = "followme/status";
static char sorryprompt[PATH_MAX] = "followme/sorry";


static TRIS_RWLIST_HEAD_STATIC(followmes, call_followme);
TRIS_LIST_HEAD_NOLOCK(findme_user_listptr, findme_user);

static void free_numbers(struct call_followme *f)
{
	/* Free numbers attached to the profile */
	struct number *prev;

	while ((prev = TRIS_LIST_REMOVE_HEAD(&f->numbers, entry)))
		/* Free the number */
		tris_free(prev);
	TRIS_LIST_HEAD_INIT_NOLOCK(&f->numbers);

	while ((prev = TRIS_LIST_REMOVE_HEAD(&f->blnumbers, entry)))
		/* Free the blacklisted number */
		tris_free(prev);
	TRIS_LIST_HEAD_INIT_NOLOCK(&f->blnumbers);

	while ((prev = TRIS_LIST_REMOVE_HEAD(&f->wlnumbers, entry)))
		/* Free the whitelisted number */
		tris_free(prev);
	TRIS_LIST_HEAD_INIT_NOLOCK(&f->wlnumbers);
}


/*! \brief Allocate and initialize followme profile */
static struct call_followme *alloc_profile(const char *fmname)
{
	struct call_followme *f;

	if (!(f = tris_calloc(1, sizeof(*f))))
		return NULL;

	tris_mutex_init(&f->lock);
	tris_copy_string(f->name, fmname, sizeof(f->name));
	f->moh[0] = '\0';
	f->context[0] = '\0';
	tris_copy_string(f->takecall, takecall, sizeof(f->takecall));
	tris_copy_string(f->nextindp, nextindp, sizeof(f->nextindp));
	tris_copy_string(f->callfromprompt, callfromprompt, sizeof(f->callfromprompt));
	tris_copy_string(f->norecordingprompt, norecordingprompt, sizeof(f->norecordingprompt));
	tris_copy_string(f->optionsprompt, optionsprompt, sizeof(f->optionsprompt));
	tris_copy_string(f->plsholdprompt, plsholdprompt, sizeof(f->plsholdprompt));
	tris_copy_string(f->statusprompt, statusprompt, sizeof(f->statusprompt));
	tris_copy_string(f->sorryprompt, sorryprompt, sizeof(f->sorryprompt));
	TRIS_LIST_HEAD_INIT_NOLOCK(&f->numbers);
	TRIS_LIST_HEAD_INIT_NOLOCK(&f->blnumbers);
	TRIS_LIST_HEAD_INIT_NOLOCK(&f->wlnumbers);
	return f;
}

static void init_profile(struct call_followme *f)
{
	f->active = 1;
	tris_copy_string(f->moh, defaultmoh, sizeof(f->moh));
}

   
   
/*! \brief Set parameter in profile from configuration file */
static void profile_set_param(struct call_followme *f, const char *param, const char *val, int linenum, int failunknown)
{

	if (!strcasecmp(param, "musicclass") || !strcasecmp(param, "musiconhold") || !strcasecmp(param, "music")) 
		tris_copy_string(f->moh, val, sizeof(f->moh));
	else if (!strcasecmp(param, "context")) 
		tris_copy_string(f->context, val, sizeof(f->context));
	else if (!strcasecmp(param, "takecall"))
		tris_copy_string(f->takecall, val, sizeof(f->takecall));
	else if (!strcasecmp(param, "declinecall"))
		tris_copy_string(f->nextindp, val, sizeof(f->nextindp));
	else if (!strcasecmp(param, "call-from-prompt") || !strcasecmp(param, "call_from_prompt"))
		tris_copy_string(f->callfromprompt, val, sizeof(f->callfromprompt));
	else if (!strcasecmp(param, "followme-norecording-prompt") || !strcasecmp(param, "norecording_prompt")) 
		tris_copy_string(f->norecordingprompt, val, sizeof(f->norecordingprompt));
	else if (!strcasecmp(param, "followme-options-prompt") || !strcasecmp(param, "options_prompt")) 
		tris_copy_string(f->optionsprompt, val, sizeof(f->optionsprompt));
	else if (!strcasecmp(param, "followme-pls-hold-prompt") || !strcasecmp(param, "pls_hold_prompt"))
		tris_copy_string(f->plsholdprompt, val, sizeof(f->plsholdprompt));
	else if (!strcasecmp(param, "followme-status-prompt") || !strcasecmp(param, "status_prompt")) 
		tris_copy_string(f->statusprompt, val, sizeof(f->statusprompt));
	else if (!strcasecmp(param, "followme-sorry-prompt") || !strcasecmp(param, "sorry_prompt")) 
		tris_copy_string(f->sorryprompt, val, sizeof(f->sorryprompt));
	else if (failunknown) {
		if (linenum >= 0)
			tris_log(LOG_WARNING, "Unknown keyword in profile '%s': %s at line %d of followme.conf\n", f->name, param, linenum);
		else
			tris_log(LOG_WARNING, "Unknown keyword in profile '%s': %s\n", f->name, param);
	}
}

/*! \brief Add a new number */
static struct number *create_followme_number(char *number, int timeout, int numorder)
{
	struct number *cur;
	char *tmp;

	if (!(cur = tris_calloc(1, sizeof(*cur))))
		return NULL;

	cur->timeout = timeout;
	if ((tmp = strchr(number, ','))) 
		*tmp = '\0';
	tris_copy_string(cur->number, number, sizeof(cur->number));
	cur->order = numorder;
	tris_debug(1, "Created a number, %s, order of , %d, with a timeout of %ld.\n", cur->number, cur->order, cur->timeout);

	return cur;
}

/*! \brief Reload followme application module */
static int reload_followme(int reload)
{
	struct call_followme *f;
	struct tris_config *cfg;
	char *cat = NULL, *tmp;
	struct tris_variable *var;
	struct number *cur, *nm;
	char numberstr[90];
	int timeout;
	char *timeoutstr;
	int numorder;
	const char *takecallstr;
	const char *declinecallstr;
	const char *tmpstr;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	if (!(cfg = tris_config_load("followme.conf", config_flags))) {
		tris_log(LOG_WARNING, "No follow me config file (followme.conf), so no follow me\n");
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file followme.conf is in an invalid format.  Aborting.\n");
		return 0;
	}

	TRIS_RWLIST_WRLOCK(&followmes);

	/* Reset Global Var Values */
	featuredigittimeout = 5000;

	/* Mark all profiles as inactive for the moment */
	TRIS_RWLIST_TRAVERSE(&followmes, f, entry) {
		f->active = 0;
	}

	featuredigittostr = tris_variable_retrieve(cfg, "general", "featuredigittimeout");

	if (!tris_strlen_zero(featuredigittostr)) {
		if (!sscanf(featuredigittostr, "%30d", &featuredigittimeout))
			featuredigittimeout = 5000;
	}

	if ((takecallstr = tris_variable_retrieve(cfg, "general", "takecall")) && !tris_strlen_zero(takecallstr)) {
		tris_copy_string(takecall, takecallstr, sizeof(takecall));
	}

	if ((declinecallstr = tris_variable_retrieve(cfg, "general", "declinecall")) && !tris_strlen_zero(declinecallstr)) {
		tris_copy_string(nextindp, declinecallstr, sizeof(nextindp));
	}

	if ((tmpstr = tris_variable_retrieve(cfg, "general", "call-from-prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(callfromprompt, tmpstr, sizeof(callfromprompt));
	} else if ((tmpstr = tris_variable_retrieve(cfg, "general", "call_from_prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(callfromprompt, tmpstr, sizeof(callfromprompt));
	}

	if ((tmpstr = tris_variable_retrieve(cfg, "general", "norecording-prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(norecordingprompt, tmpstr, sizeof(norecordingprompt));
	} else if ((tmpstr = tris_variable_retrieve(cfg, "general", "norecording_prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(callfromprompt, tmpstr, sizeof(callfromprompt));
	}


	if ((tmpstr = tris_variable_retrieve(cfg, "general", "options-prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	} else if ((tmpstr = tris_variable_retrieve(cfg, "general", "options_prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	}

	if ((tmpstr = tris_variable_retrieve(cfg, "general", "pls-hold-prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	} else if ((tmpstr = tris_variable_retrieve(cfg, "general", "pls_hold_prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	}

	if ((tmpstr = tris_variable_retrieve(cfg, "general", "status-prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	} else if ((tmpstr = tris_variable_retrieve(cfg, "general", "status_prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	}

	if ((tmpstr = tris_variable_retrieve(cfg, "general", "sorry-prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	} else if ((tmpstr = tris_variable_retrieve(cfg, "general", "sorry_prompt")) && !tris_strlen_zero(tmpstr)) {
		tris_copy_string(optionsprompt, tmpstr, sizeof(optionsprompt));
	}

	/* Chug through config file */
	while ((cat = tris_category_browse(cfg, cat))) {
		int new = 0;

		if (!strcasecmp(cat, "general"))
			continue;

		/* Look for an existing one */
		TRIS_LIST_TRAVERSE(&followmes, f, entry) {
			if (!strcasecmp(f->name, cat))
				break;
		}

		tris_debug(1, "New profile %s.\n", cat);

		if (!f) {
			/* Make one then */
			f = alloc_profile(cat);
			new = 1;
		}

		/* Totally fail if we fail to find/create an entry */
		if (!f)
			continue;

		if (!new)
			tris_mutex_lock(&f->lock);
		/* Re-initialize the profile */
		init_profile(f);
		free_numbers(f);
		var = tris_variable_browse(cfg, cat);
		while (var) {
			if (!strcasecmp(var->name, "number")) {
				int idx = 0;

				/* Add a new number */
				tris_copy_string(numberstr, var->value, sizeof(numberstr));
				if ((tmp = strchr(numberstr, ','))) {
					*tmp++ = '\0';
					timeoutstr = tris_strdupa(tmp);
					if ((tmp = strchr(timeoutstr, ','))) {
						*tmp++ = '\0';
						numorder = atoi(tmp);
						if (numorder < 0)
							numorder = 0;
					} else 
						numorder = 0;
					timeout = atoi(timeoutstr);
					if (timeout < 0) 
						timeout = 25;
				} else {
					timeout = 25;
					numorder = 0;
				}

				if (!numorder) {
					idx = 1;
					TRIS_LIST_TRAVERSE(&f->numbers, nm, entry) 
						idx++;
					numorder = idx;
				}
				cur = create_followme_number(numberstr, timeout, numorder);
				TRIS_LIST_INSERT_TAIL(&f->numbers, cur, entry);
			} else {
				profile_set_param(f, var->name, var->value, var->lineno, 1);
				tris_debug(2, "Logging parameter %s with value %s from lineno %d\n", var->name, var->value, var->lineno);
			}
			var = var->next;
		} /* End while(var) loop */

		if (!new) 
			tris_mutex_unlock(&f->lock);
		else
			TRIS_RWLIST_INSERT_HEAD(&followmes, f, entry);
	}

	tris_config_destroy(cfg);

	TRIS_RWLIST_UNLOCK(&followmes);

	return 1;
}

static void clear_caller(struct findme_user *tmpuser)
{
	struct tris_channel *outbound;

	if (tmpuser && tmpuser->ochan && tmpuser->state >= 0) {
		outbound = tmpuser->ochan;
		if (!outbound->cdr) {
			outbound->cdr = tris_cdr_alloc();
			if (outbound->cdr)
				tris_cdr_init(outbound->cdr, outbound);
		}
		if (outbound->cdr) {
			char tmp[256];

			snprintf(tmp, sizeof(tmp), "%s/%s", "Local", tmpuser->dialarg);
			tris_cdr_setapp(outbound->cdr, "FollowMe", tmp);
			tris_cdr_update(outbound);
			tris_cdr_start(outbound->cdr);
			tris_cdr_end(outbound->cdr);
			/* If the cause wasn't handled properly */
			if (tris_cdr_disposition(outbound->cdr, outbound->hangupcause))
				tris_cdr_failed(outbound->cdr);
		} else
			tris_log(LOG_WARNING, "Unable to create Call Detail Record\n");
		tris_hangup(tmpuser->ochan);
	}

}

static void clear_calling_tree(struct findme_user_listptr *findme_user_list) 
{
	struct findme_user *tmpuser;

	TRIS_LIST_TRAVERSE(findme_user_list, tmpuser, entry) {
		clear_caller(tmpuser);
		tmpuser->cleared = 1;
	}
}



static struct tris_channel *wait_for_winner(struct findme_user_listptr *findme_user_list, struct number *nm, struct tris_channel *caller, char *namerecloc, int *status, struct fm_args *tpargs) 
{
	struct tris_channel *watchers[256];
	int pos;
	struct tris_channel *winner;
	struct tris_frame *f;
	int ctstatus = 0;
	int dg;
	struct findme_user *tmpuser;
	int to = 0;
	int livechannels = 0;
	int tmpto;
	long totalwait = 0, wtd = 0, towas = 0;
	char *callfromname;
	char *pressbuttonname;

	/* ------------ wait_for_winner_channel start --------------- */ 

	callfromname = tris_strdupa(tpargs->callfromprompt);
	pressbuttonname = tris_strdupa(tpargs->optionsprompt);

	if (TRIS_LIST_EMPTY(findme_user_list)) {
		tris_verb(3, "couldn't reach at this number.\n");
		return NULL;
	}

	if (!caller) {
		tris_verb(3, "Original caller hungup. Cleanup.\n");
		clear_calling_tree(findme_user_list);
		return NULL;
	}

	totalwait = nm->timeout * 1000;

	while (!ctstatus) {
		to = 1000;
		pos = 1; 
		livechannels = 0;
		watchers[0] = caller;

		dg = 0;
		winner = NULL;
		TRIS_LIST_TRAVERSE(findme_user_list, tmpuser, entry) {
			if (tmpuser->state >= 0 && tmpuser->ochan) {
				if (tmpuser->state == 3) 
					tmpuser->digts += (towas - wtd);
				if (tmpuser->digts && (tmpuser->digts > featuredigittimeout)) {
					tris_verb(3, "We've been waiting for digits longer than we should have.\n");
					if (!tris_strlen_zero(namerecloc)) {
						tmpuser->state = 1;
						tmpuser->digts = 0;
						if (!tris_streamfile(tmpuser->ochan, callfromname, tmpuser->ochan->language)) {
							tris_sched_runq(tmpuser->ochan->sched);
						} else {
							tris_log(LOG_WARNING, "Unable to playback %s.\n", callfromname);
							return NULL;
						}
					} else {
						tmpuser->state = 2;
						tmpuser->digts = 0;
						if (!tris_streamfile(tmpuser->ochan, tpargs->norecordingprompt, tmpuser->ochan->language))
							tris_sched_runq(tmpuser->ochan->sched);
						else {
							tris_log(LOG_WARNING, "Unable to playback %s.\n", tpargs->norecordingprompt);
							return NULL;
						}
					}
				}
				if (tmpuser->ochan->stream) {
					tris_sched_runq(tmpuser->ochan->sched);
					tmpto = tris_sched_wait(tmpuser->ochan->sched);
					if (tmpto > 0 && tmpto < to)
						to = tmpto;
					else if (tmpto < 0 && !tmpuser->ochan->timingfunc) {
						tris_stopstream(tmpuser->ochan);
						if (tmpuser->state == 1) {
							tris_verb(3, "Playback of the call-from file appears to be done.\n");
							if (!tris_streamfile(tmpuser->ochan, namerecloc, tmpuser->ochan->language)) {
								tmpuser->state = 2;
							} else {
								tris_log(LOG_NOTICE, "Unable to playback %s. Maybe the caller didn't record their name?\n", namerecloc);
								memset(tmpuser->yn, 0, sizeof(tmpuser->yn));
								tmpuser->ynidx = 0;
								if (!tris_streamfile(tmpuser->ochan, pressbuttonname, tmpuser->ochan->language))
									tmpuser->state = 3;
								else {
									tris_log(LOG_WARNING, "Unable to playback %s.\n", pressbuttonname);
									return NULL;
								} 
							}
						} else if (tmpuser->state == 2) {
							tris_verb(3, "Playback of name file appears to be done.\n");
							memset(tmpuser->yn, 0, sizeof(tmpuser->yn));
							tmpuser->ynidx = 0;
							if (!tris_streamfile(tmpuser->ochan, pressbuttonname, tmpuser->ochan->language)) {
								tmpuser->state = 3;
							} else {
								return NULL;
							} 
						} else if (tmpuser->state == 3) {
							tris_verb(3, "Playback of the next step file appears to be done.\n");
							tmpuser->digts = 0;
						}
					}
				}
				watchers[pos++] = tmpuser->ochan;
				livechannels++;
			}
		}

		tmpto = to;
		if (to < 0) {
			to = 1000;
			tmpto = 1000;
		}
		towas = to;
		winner = tris_waitfor_n(watchers, pos, &to);
		tmpto -= to;
		totalwait -= tmpto;
		wtd = to;
		if (totalwait <= 0) {
			tris_verb(3, "We've hit our timeout for this step. Drop everyone and move on to the next one. %ld\n", totalwait);
			clear_calling_tree(findme_user_list);
			return NULL;
		}
		if (winner) {
			/* Need to find out which channel this is */
			dg = 0;
			while ((winner != watchers[dg]) && (dg < 256))
				dg++;
			TRIS_LIST_TRAVERSE(findme_user_list, tmpuser, entry)
				if (tmpuser->ochan == winner)
					break;
			f = tris_read(winner);
			if (f) {
				if (f->frametype == TRIS_FRAME_CONTROL) {
					switch(f->subclass) {
					case TRIS_CONTROL_HANGUP:
						tris_verb(3, "%s received a hangup frame.\n", winner->name);
						if (f->data.uint32) {
							winner->hangupcause = f->data.uint32;
						}
						if (dg == 0) {
							tris_verb(3, "The calling channel hungup. Need to drop everyone else.\n");
							clear_calling_tree(findme_user_list);
							ctstatus = -1;
						}
						break;
					case TRIS_CONTROL_ANSWER:
						tris_verb(3, "%s answered %s\n", winner->name, caller->name);
						/* If call has been answered, then the eventual hangup is likely to be normal hangup */ 
						winner->hangupcause = TRIS_CAUSE_NORMAL_CLEARING;
						caller->hangupcause = TRIS_CAUSE_NORMAL_CLEARING;
						tris_verb(3, "Starting playback of %s\n", callfromname);
						if (dg > 0) {
							if (!tris_strlen_zero(namerecloc)) {
								if (!tris_streamfile(winner, callfromname, winner->language)) {
									tris_sched_runq(winner->sched);
									tmpuser->state = 1;
								} else {
									tris_log(LOG_WARNING, "Unable to playback %s.\n", callfromname);
									tris_frfree(f);
									return NULL;
								}
							} else {
								tmpuser->state = 2;
								if (!tris_streamfile(tmpuser->ochan, tpargs->norecordingprompt, tmpuser->ochan->language))
									tris_sched_runq(tmpuser->ochan->sched);
								else {
									tris_log(LOG_WARNING, "Unable to playback %s.\n", tpargs->norecordingprompt);
									tris_frfree(f);
									return NULL;
								}
							}
						}
						break;
					case TRIS_CONTROL_BUSY:
						tris_verb(3, "%s is busy\n", winner->name);
						break;
					case TRIS_CONTROL_CONGESTION:
						tris_verb(3, "%s is circuit-busy\n", winner->name);
						break;
					case TRIS_CONTROL_RINGING:
						tris_verb(3, "%s is ringing\n", winner->name);
						break;
					case TRIS_CONTROL_PROGRESS:
						tris_verb(3, "%s is making progress passing it to %s\n", winner->name, caller->name);
						break;
					case TRIS_CONTROL_VIDUPDATE:
						tris_verb(3, "%s requested a video update, passing it to %s\n", winner->name, caller->name);
						break;
					case TRIS_CONTROL_SRCUPDATE:
						tris_verb(3, "%s requested a source update, passing it to %s\n", winner->name, caller->name);
						break;
					case TRIS_CONTROL_PROCEEDING:
						tris_verb(3, "%s is proceeding passing it to %s\n", winner->name,caller->name);
						break;
					case TRIS_CONTROL_HOLD:
						tris_verb(3, "Call on %s placed on hold\n", winner->name);
						break;
					case TRIS_CONTROL_UNHOLD:
						tris_verb(3, "Call on %s left from hold\n", winner->name);
						break;
					case TRIS_CONTROL_OFFHOOK:
					case TRIS_CONTROL_FLASH:
						/* Ignore going off hook and flash */
						break;
					case -1:
						tris_verb(3, "%s stopped sounds\n", winner->name);
						break;
					default:
						tris_debug(1, "Dunno what to do with control type %d\n", f->subclass);
						break;
					}
				} 
				if (tmpuser && tmpuser->state == 3 && f->frametype == TRIS_FRAME_DTMF) {
					if (winner->stream)
						tris_stopstream(winner);
					tmpuser->digts = 0;
					tris_debug(1, "DTMF received: %c\n",(char) f->subclass);
					tmpuser->yn[tmpuser->ynidx] = (char) f->subclass;
					tmpuser->ynidx++;
					tris_debug(1, "DTMF string: %s\n", tmpuser->yn);
					if (tmpuser->ynidx >= ynlongest) {
						tris_debug(1, "reached longest possible match - doing evals\n");
						if (!strcmp(tmpuser->yn, tpargs->takecall)) {
							tris_debug(1, "Match to take the call!\n");
							tris_frfree(f);
							return tmpuser->ochan;
						}
						if (!strcmp(tmpuser->yn, tpargs->nextindp)) {
							tris_debug(1, "Next in dial plan step requested.\n");
							*status = 1;
							tris_frfree(f);
							return NULL;
						}

					}
				}

				tris_frfree(f);
			} else {
				if (winner) {
					tris_debug(1, "we didn't get a frame. hanging up. dg is %d\n",dg);					      
					if (!dg) {
						clear_calling_tree(findme_user_list);
						return NULL;
					} else {
						tmpuser->state = -1;
						tris_hangup(winner);  
						livechannels--;
						tris_debug(1, "live channels left %d\n", livechannels);
						if (!livechannels) {
							tris_verb(3, "no live channels left. exiting.\n");
							return NULL;
						}
					}
				}
			}

		} else
			tris_debug(1, "timed out waiting for action\n");
	}

	/* --- WAIT FOR WINNER NUMBER END! -----------*/
	return NULL;
}

static void findmeexec(struct fm_args *tpargs)
{
	struct number *nm;
	struct tris_channel *outbound;
	struct tris_channel *caller;
	struct tris_channel *winner = NULL;
	char dialarg[512];
	int dg, idx;
	char *rest, *number;
	struct findme_user *tmpuser;
	struct findme_user *fmuser;
	struct findme_user *headuser;
	struct findme_user_listptr *findme_user_list;
	int status;

	findme_user_list = tris_calloc(1, sizeof(*findme_user_list));
	TRIS_LIST_HEAD_INIT_NOLOCK(findme_user_list);

	/* We're going to figure out what the longest possible string of digits to collect is */
	ynlongest = 0;
	if (strlen(tpargs->takecall) > ynlongest)
		ynlongest = strlen(tpargs->takecall);
	if (strlen(tpargs->nextindp) > ynlongest)
		ynlongest = strlen(tpargs->nextindp);

	idx = 1;
	caller = tpargs->chan;
	TRIS_LIST_TRAVERSE(&tpargs->cnumbers, nm, entry)
		if (nm->order == idx)
			break;

	while (nm) {

		tris_debug(2, "Number %s timeout %ld\n", nm->number,nm->timeout);

		number = tris_strdupa(nm->number);
		tris_debug(3, "examining %s\n", number);
		do {
			rest = strchr(number, '&');
			if (rest) {
				*rest = 0;
				rest++;
			}

			if (!strcmp(tpargs->context, ""))
				snprintf(dialarg, sizeof(dialarg), "%s", number);
			else
				snprintf(dialarg, sizeof(dialarg), "%s@%s", number, tpargs->context);

			tmpuser = tris_calloc(1, sizeof(*tmpuser));
			if (!tmpuser) {
				tris_log(LOG_WARNING, "Out of memory!\n");
				tris_free(findme_user_list);
				return;
			}

			outbound = tris_request("Local", tris_best_codec(caller->nativeformats), dialarg, &dg, 0);
			if (outbound) {
				tris_set_callerid(outbound, caller->cid.cid_num, caller->cid.cid_name, caller->cid.cid_num);
				tris_channel_inherit_variables(tpargs->chan, outbound);
				tris_channel_datastore_inherit(tpargs->chan, outbound);
				tris_string_field_set(outbound, language, tpargs->chan->language);
				tris_string_field_set(outbound, accountcode, tpargs->chan->accountcode);
				tris_string_field_set(outbound, musicclass, tpargs->chan->musicclass);
				tris_verb(3, "calling %s\n", dialarg);
				if (!tris_call(outbound,dialarg,0)) {
					tmpuser->ochan = outbound;
					tmpuser->state = 0;
					tmpuser->cleared = 0;
					tris_copy_string(tmpuser->dialarg, dialarg, sizeof(dialarg));
					TRIS_LIST_INSERT_TAIL(findme_user_list, tmpuser, entry);
				} else {
					tris_verb(3, "couldn't reach at this number.\n"); 
					if (outbound) {
						if (!outbound->cdr) 
							outbound->cdr = tris_cdr_alloc();
						if (outbound->cdr) {
							char tmp[256];

							tris_cdr_init(outbound->cdr, outbound);
							snprintf(tmp, sizeof(tmp), "%s/%s", "Local", dialarg);
							tris_cdr_setapp(outbound->cdr, "FollowMe", tmp);
							tris_cdr_update(outbound);
							tris_cdr_start(outbound->cdr);
							tris_cdr_end(outbound->cdr);
							/* If the cause wasn't handled properly */
							if (tris_cdr_disposition(outbound->cdr,outbound->hangupcause))
								tris_cdr_failed(outbound->cdr);
						} else {
							tris_log(LOG_ERROR, "Unable to create Call Detail Record\n");
							tris_hangup(outbound);
							outbound = NULL;
						}
					}
				}
			} else 
				tris_log(LOG_WARNING, "Unable to allocate a channel for Local/%s cause: %s\n", dialarg, tris_cause2str(dg));

			number = rest;
		} while (number);

		status = 0;
		if (!TRIS_LIST_EMPTY(findme_user_list))
			winner = wait_for_winner(findme_user_list, nm, caller, tpargs->namerecloc, &status, tpargs);

		while ((fmuser = TRIS_LIST_REMOVE_HEAD(findme_user_list, entry))) {
			if (!fmuser->cleared && fmuser->ochan != winner)
				clear_caller(fmuser);
			tris_free(fmuser);
		}

		fmuser = NULL;
		tmpuser = NULL;
		headuser = NULL;
		if (winner)
			break;

		if (!caller || tris_check_hangup(caller)) {
			tpargs->status = 1;
			tris_free(findme_user_list);
			return;
		}

		idx++;
		TRIS_LIST_TRAVERSE(&tpargs->cnumbers, nm, entry) {
			if (nm->order == idx)
				break;
		}
	}
	tris_free(findme_user_list);
	if (!winner) 
		tpargs->status = 1;
	else {
		tpargs->status = 100;
		tpargs->outbound = winner;
	}

	return;
}

static struct call_followme *find_realtime(const char *name)
{
	struct tris_variable *var = tris_load_realtime("followme", "name", name, SENTINEL), *v;
	struct tris_config *cfg;
	const char *catg;
	struct call_followme *new;
	struct tris_str *str = tris_str_create(16);

	if (!var) {
		return NULL;
	}

	if (!(new = alloc_profile(name))) {
		return NULL;
	}

	for (v = var; v; v = v->next) {
		if (!strcasecmp(v->name, "active")) {
			if (tris_false(v->value)) {
				tris_mutex_destroy(&new->lock);
				tris_free(new);
				return NULL;
			}
		} else {
			profile_set_param(new, v->name, v->value, 0, 0);
		}
	}

	tris_variables_destroy(var);
	new->realtime = 1;

	/* Load numbers */
	if (!(cfg = tris_load_realtime_multientry("followme_numbers", "ordinal LIKE", "%", "name", name, SENTINEL))) {
		tris_mutex_destroy(&new->lock);
		tris_free(new);
		return NULL;
	}

	for (catg = tris_category_browse(cfg, NULL); catg; catg = tris_category_browse(cfg, catg)) {
		const char *numstr, *timeoutstr, *ordstr;
		int timeout;
		struct number *cur;
		if (!(numstr = tris_variable_retrieve(cfg, catg, "phonenumber"))) {
			continue;
		}
		if (!(timeoutstr = tris_variable_retrieve(cfg, catg, "timeout")) || sscanf(timeoutstr, "%30d", &timeout) != 1 || timeout < 1) {
			timeout = 25;
		}
		/* This one has to exist; it was part of the query */
		ordstr = tris_variable_retrieve(cfg, catg, "ordinal");
		tris_str_set(&str, 0, "%s", numstr);
		if ((cur = create_followme_number(tris_str_buffer(str), timeout, atoi(ordstr)))) {
			TRIS_LIST_INSERT_TAIL(&new->numbers, cur, entry);
		}
	}
	tris_config_destroy(cfg);

	return new;
}

static void end_bridge_callback(void *data)
{
	char buf[80];
	time_t end;
	struct tris_channel *chan = data;

	time(&end);

	tris_channel_lock(chan);
	if (chan->cdr->answer.tv_sec) {
		snprintf(buf, sizeof(buf), "%ld", end - chan->cdr->answer.tv_sec);
		pbx_builtin_setvar_helper(chan, "ANSWEREDTIME", buf);
	}

	if (chan->cdr->start.tv_sec) {
		snprintf(buf, sizeof(buf), "%ld", end - chan->cdr->start.tv_sec);
		pbx_builtin_setvar_helper(chan, "DIALEDTIME", buf);
	}
	tris_channel_unlock(chan);
}

static void end_bridge_callback_data_fixup(struct tris_bridge_config *bconfig, struct tris_channel *originator, struct tris_channel *terminator)
{
	bconfig->end_bridge_callback_data = originator;
}

static int app_exec(struct tris_channel *chan, void *data)
{
	struct fm_args targs = { 0, };
	struct tris_bridge_config config;
	struct call_followme *f;
	struct number *nm, *newnm;
	int res = 0;
	char *argstr;
	char namerecloc[255];
	int duration = 0;
	struct tris_channel *caller;
	struct tris_channel *outbound;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(followmeid);
		TRIS_APP_ARG(options);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "%s requires an argument (followmeid)\n", app);
		return -1;
	}

	if (!(argstr = tris_strdupa((char *)data))) {
		tris_log(LOG_ERROR, "Out of memory!\n");
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, argstr);

	if (tris_strlen_zero(args.followmeid)) {
		tris_log(LOG_WARNING, "%s requires an argument (followmeid)\n", app);
		return -1;
	}

	TRIS_RWLIST_RDLOCK(&followmes);
	TRIS_RWLIST_TRAVERSE(&followmes, f, entry) {
		if (!strcasecmp(f->name, args.followmeid) && (f->active))
			break;
	}
	TRIS_RWLIST_UNLOCK(&followmes);

	tris_debug(1, "New profile %s.\n", args.followmeid);

	if (!f) {
		f = find_realtime(args.followmeid);
	}

	if (!f) {
		tris_log(LOG_WARNING, "Profile requested, %s, not found in the configuration.\n", args.followmeid);
		return 0;
	}

	/* XXX TODO: Reinsert the db check value to see whether or not follow-me is on or off */
	if (args.options) 
		tris_app_parse_options(followme_opts, &targs.followmeflags, NULL, args.options);

	/* Lock the profile lock and copy out everything we need to run with before unlocking it again */
	tris_mutex_lock(&f->lock);
	targs.mohclass = tris_strdupa(f->moh);
	tris_copy_string(targs.context, f->context, sizeof(targs.context));
	tris_copy_string(targs.takecall, f->takecall, sizeof(targs.takecall));
	tris_copy_string(targs.nextindp, f->nextindp, sizeof(targs.nextindp));
	tris_copy_string(targs.callfromprompt, f->callfromprompt, sizeof(targs.callfromprompt));
	tris_copy_string(targs.norecordingprompt, f->norecordingprompt, sizeof(targs.norecordingprompt));
	tris_copy_string(targs.optionsprompt, f->optionsprompt, sizeof(targs.optionsprompt));
	tris_copy_string(targs.plsholdprompt, f->plsholdprompt, sizeof(targs.plsholdprompt));
	tris_copy_string(targs.statusprompt, f->statusprompt, sizeof(targs.statusprompt));
	tris_copy_string(targs.sorryprompt, f->sorryprompt, sizeof(targs.sorryprompt));
	/* Copy the numbers we're going to use into another list in case the master list should get modified 
	   (and locked) while we're trying to do a follow-me */
	TRIS_LIST_HEAD_INIT_NOLOCK(&targs.cnumbers);
	TRIS_LIST_TRAVERSE(&f->numbers, nm, entry) {
		newnm = create_followme_number(nm->number, nm->timeout, nm->order);
		TRIS_LIST_INSERT_TAIL(&targs.cnumbers, newnm, entry);
	}
	tris_mutex_unlock(&f->lock);

	/* Answer the call */
	if (chan->_state != TRIS_STATE_UP) {
		tris_answer(chan);
	}

	if (tris_test_flag(&targs.followmeflags, FOLLOWMEFLAG_STATUSMSG)) 
		tris_stream_and_wait(chan, targs.statusprompt, "");

	snprintf(namerecloc,sizeof(namerecloc),"%s/followme.%s",tris_config_TRIS_SPOOL_DIR,chan->uniqueid);
	duration = 5;

	if (tris_test_flag(&targs.followmeflags, FOLLOWMEFLAG_RECORDNAME)) 
		if (tris_play_and_record(chan, "voicemail/vm-rec-name", namerecloc, 5, "sln", &duration, tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE), 0, NULL) < 0)
			goto outrun;

	if (!tris_fileexists(namerecloc, NULL, chan->language))
		tris_copy_string(namerecloc, "", sizeof(namerecloc));

	if (tris_streamfile(chan, targs.plsholdprompt, chan->language))
		goto outrun;
	if (tris_waitstream(chan, "") < 0)
		goto outrun;
	tris_moh_start(chan, S_OR(targs.mohclass, NULL), NULL);

	targs.status = 0;
	targs.chan = chan;
	tris_copy_string(targs.namerecloc, namerecloc, sizeof(targs.namerecloc));

	findmeexec(&targs);

	while ((nm = TRIS_LIST_REMOVE_HEAD(&targs.cnumbers, entry)))
		tris_free(nm);

	if (!tris_strlen_zero(namerecloc))
		unlink(namerecloc);

	if (targs.status != 100) {
		tris_moh_stop(chan);
		if (tris_test_flag(&targs.followmeflags, FOLLOWMEFLAG_UNREACHABLEMSG)) 
			tris_stream_and_wait(chan, targs.sorryprompt, "");
		res = 0;
	} else {
		caller = chan;
		outbound = targs.outbound;
		/* Bridge the two channels. */

		memset(&config, 0, sizeof(config));
		tris_set_flag(&(config.features_callee), TRIS_FEATURE_REDIRECT);
		tris_set_flag(&(config.features_callee), TRIS_FEATURE_AUTOMON);
		tris_set_flag(&(config.features_caller), TRIS_FEATURE_AUTOMON);
		config.end_bridge_callback = end_bridge_callback;
		config.end_bridge_callback_data = chan;
		config.end_bridge_callback_data_fixup = end_bridge_callback_data_fixup;

		tris_moh_stop(caller);
		/* Be sure no generators are left on it */
		tris_deactivate_generator(caller);
		/* Make sure channels are compatible */
		res = tris_channel_make_compatible(caller, outbound);
		if (res < 0) {
			tris_log(LOG_WARNING, "Had to drop call because I couldn't make %s compatible with %s\n", caller->name, outbound->name);
			tris_hangup(outbound);
			goto outrun;
		}
		res = tris_bridge_call(caller, outbound, &config);
		if (outbound)
			tris_hangup(outbound);
	}

	outrun:

	if (f->realtime) {
		/* Not in list */
		free_numbers(f);
		tris_free(f);
	}

	return res;
}

static int unload_module(void)
{
	struct call_followme *f;

	tris_unregister_application(app);

	/* Free Memory. Yeah! I'm free! */
	TRIS_RWLIST_WRLOCK(&followmes);
	while ((f = TRIS_RWLIST_REMOVE_HEAD(&followmes, entry))) {
		free_numbers(f);
		tris_free(f);
	}

	TRIS_RWLIST_UNLOCK(&followmes);

	return 0;
}

static int load_module(void)
{
	if(!reload_followme(0))
		return TRIS_MODULE_LOAD_DECLINE;

	return tris_register_application_xml(app, app_exec);
}

static int reload(void)
{
	reload_followme(1);

	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Find-Me/Follow-Me Application",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
