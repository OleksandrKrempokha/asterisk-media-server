/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright 2004 - 2005, Anthony Minessale <anthmct@yahoo.com>
 *
 * Anthony Minessale <anthmct@yahoo.com>
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
 * \brief While Loop Implementation
 *
 * \author Anthony Minessale <anthmct@yahoo.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 156756 $")

#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/channel.h"

/*** DOCUMENTATION
	<application name="While" language="en_US">
		<synopsis>
			Start a while loop.
		</synopsis>
		<syntax>
			<parameter name="expr" required="true" />
		</syntax>
		<description>
			<para>Start a While Loop.  Execution will return to this point when
			<literal>EndWhile()</literal> is called until expr is no longer true.</para>
		</description>
		<see-also>
			<ref type="application">EndWhile</ref>
			<ref type="application">ExitWhile</ref>
			<ref type="application">ContinueWhile</ref>
		</see-also>
	</application>
	<application name="EndWhile" language="en_US">
		<synopsis>
			End a while loop.
		</synopsis>
		<syntax />
		<description>
			<para>Return to the previous called <literal>While()</literal>.</para>
		</description>
		<see-also>
			<ref type="application">While</ref>
			<ref type="application">ExitWhile</ref>
			<ref type="application">ContinueWhile</ref>
		</see-also>
	</application>
	<application name="ExitWhile" language="en_US">
		<synopsis>
			End a While loop.
		</synopsis>
		<syntax />
		<description>
			<para>Exits a <literal>While()</literal> loop, whether or not the conditional has been satisfied.</para>
		</description>
		<see-also>
			<ref type="application">While</ref>
			<ref type="application">EndWhile</ref>
			<ref type="application">ContinueWhile</ref>
		</see-also>
	</application>
	<application name="ContinueWhile" language="en_US">
		<synopsis>
			Restart a While loop.
		</synopsis>
		<syntax />
		<description>
			<para>Returns to the top of the while loop and re-evaluates the conditional.</para>
		</description>
		<see-also>
			<ref type="application">While</ref>
			<ref type="application">EndWhile</ref>
			<ref type="application">ExitWhile</ref>
		</see-also>
	</application>
 ***/

static char *start_app = "While";
static char *stop_app = "EndWhile";
static char *exit_app = "ExitWhile";
static char *continue_app = "ContinueWhile";

#define VAR_SIZE 64


static const char *get_index(struct tris_channel *chan, const char *prefix, int idx) {
	char varname[VAR_SIZE];

	snprintf(varname, VAR_SIZE, "%s_%d", prefix, idx);
	return pbx_builtin_getvar_helper(chan, varname);
}

static struct tris_exten *find_matching_priority(struct tris_context *c, const char *exten, int priority, const char *callerid)
{
	struct tris_exten *e;
	struct tris_include *i;
	struct tris_context *c2;

	for (e=tris_walk_context_extensions(c, NULL); e; e=tris_walk_context_extensions(c, e)) {
		if (tris_extension_match(tris_get_extension_name(e), exten)) {
			int needmatch = tris_get_extension_matchcid(e);
			if ((needmatch && tris_extension_match(tris_get_extension_cidmatch(e), callerid)) ||
				(!needmatch)) {
				/* This is the matching extension we want */
				struct tris_exten *p;
				for (p=tris_walk_extension_priorities(e, NULL); p; p=tris_walk_extension_priorities(e, p)) {
					if (priority != tris_get_extension_priority(p))
						continue;
					return p;
				}
			}
		}
	}

	/* No match; run through includes */
	for (i=tris_walk_context_includes(c, NULL); i; i=tris_walk_context_includes(c, i)) {
		for (c2=tris_walk_contexts(NULL); c2; c2=tris_walk_contexts(c2)) {
			if (!strcmp(tris_get_context_name(c2), tris_get_include_name(i))) {
				e = find_matching_priority(c2, exten, priority, callerid);
				if (e)
					return e;
			}
		}
	}
	return NULL;
}

static int find_matching_endwhile(struct tris_channel *chan)
{
	struct tris_context *c;
	int res=-1;

	if (tris_rdlock_contexts()) {
		tris_log(LOG_ERROR, "Failed to lock contexts list\n");
		return -1;
	}

	for (c=tris_walk_contexts(NULL); c; c=tris_walk_contexts(c)) {
		struct tris_exten *e;

		if (!tris_rdlock_context(c)) {
			if (!strcmp(tris_get_context_name(c), chan->context)) {
				/* This is the matching context we want */
				int cur_priority = chan->priority + 1, level=1;

				for (e = find_matching_priority(c, chan->exten, cur_priority, chan->cid.cid_num); e; e = find_matching_priority(c, chan->exten, ++cur_priority, chan->cid.cid_num)) {
					if (!strcasecmp(tris_get_extension_app(e), "WHILE")) {
						level++;
					} else if (!strcasecmp(tris_get_extension_app(e), "ENDWHILE")) {
						level--;
					}

					if (level == 0) {
						res = cur_priority;
						break;
					}
				}
			}
			tris_unlock_context(c);
			if (res > 0) {
				break;
			}
		}
	}
	tris_unlock_contexts();
	return res;
}

static int _while_exec(struct tris_channel *chan, void *data, int end)
{
	int res=0;
	const char *while_pri = NULL;
	char *my_name = NULL;
	const char *condition = NULL, *label = NULL;
	char varname[VAR_SIZE], end_varname[VAR_SIZE];
	const char *prefix = "WHILE";
	size_t size=0;
	int used_index_i = -1, x=0;
	char used_index[VAR_SIZE] = "0", new_index[VAR_SIZE] = "0";

	if (!chan) {
		/* huh ? */
		return -1;
	}

#if 0
	/* dont want run away loops if the chan isn't even up
	   this is up for debate since it slows things down a tad ......

	   Debate is over... this prevents While/EndWhile from working
	   within the "h" extension.  Not good.
	*/
	if (tris_waitfordigit(chan,1) < 0)
		return -1;
#endif

	for (x=0;;x++) {
		if (get_index(chan, prefix, x)) {
			used_index_i = x;
		} else 
			break;
	}
	
	snprintf(used_index, VAR_SIZE, "%d", used_index_i);
	snprintf(new_index, VAR_SIZE, "%d", used_index_i + 1);
	
	if (!end)
		condition = tris_strdupa(data);

	size = strlen(chan->context) + strlen(chan->exten) + 32;
	my_name = alloca(size);
	memset(my_name, 0, size);
	snprintf(my_name, size, "%s_%s_%d", chan->context, chan->exten, chan->priority);
	
	tris_channel_lock(chan);
	if (end) {
		label = used_index;
	} else if (!(label = pbx_builtin_getvar_helper(chan, my_name))) {
		label = new_index;
		pbx_builtin_setvar_helper(chan, my_name, label);
	}
	snprintf(varname, VAR_SIZE, "%s_%s", prefix, label);
	if ((while_pri = pbx_builtin_getvar_helper(chan, varname)) && !end) {
		while_pri = tris_strdupa(while_pri);
		snprintf(end_varname,VAR_SIZE,"END_%s",varname);
	}
	tris_channel_unlock(chan);
	

	if ((!end && !pbx_checkcondition(condition)) || (end == 2)) {
		/* Condition Met (clean up helper vars) */
		const char *goto_str;
		pbx_builtin_setvar_helper(chan, varname, NULL);
		pbx_builtin_setvar_helper(chan, my_name, NULL);
		snprintf(end_varname,VAR_SIZE,"END_%s",varname);
		tris_channel_lock(chan);
		if ((goto_str = pbx_builtin_getvar_helper(chan, end_varname))) {
			tris_parseable_goto(chan, goto_str);
			pbx_builtin_setvar_helper(chan, end_varname, NULL);
		} else {
			int pri = find_matching_endwhile(chan);
			if (pri > 0) {
				tris_verb(3, "Jumping to priority %d\n", pri);
				chan->priority = pri;
			} else {
				tris_log(LOG_WARNING, "Couldn't find matching EndWhile? (While at %s@%s priority %d)\n", chan->context, chan->exten, chan->priority);
			}
		}
		tris_channel_unlock(chan);
		return res;
	}

	if (!end && !while_pri) {
		char *goto_str;
		size = strlen(chan->context) + strlen(chan->exten) + 32;
		goto_str = alloca(size);
		memset(goto_str, 0, size);
		snprintf(goto_str, size, "%s,%s,%d", chan->context, chan->exten, chan->priority);
		pbx_builtin_setvar_helper(chan, varname, goto_str);
	}

	else if (end && while_pri) {
		/* END of loop */
		snprintf(end_varname, VAR_SIZE, "END_%s", varname);
		if (! pbx_builtin_getvar_helper(chan, end_varname)) {
			char *goto_str;
			size = strlen(chan->context) + strlen(chan->exten) + 32;
			goto_str = alloca(size);
			memset(goto_str, 0, size);
			snprintf(goto_str, size, "%s,%s,%d", chan->context, chan->exten, chan->priority+1);
			pbx_builtin_setvar_helper(chan, end_varname, goto_str);
		}
		tris_parseable_goto(chan, while_pri);
	}

	return res;
}

static int while_start_exec(struct tris_channel *chan, void *data) {
	return _while_exec(chan, data, 0);
}

static int while_end_exec(struct tris_channel *chan, void *data) {
	return _while_exec(chan, data, 1);
}

static int while_exit_exec(struct tris_channel *chan, void *data) {
	return _while_exec(chan, data, 2);
}

static int while_continue_exec(struct tris_channel *chan, void *data)
{
	int x;
	const char *prefix = "WHILE", *while_pri=NULL;

	for (x = 0; ; x++) {
		const char *tmp = get_index(chan, prefix, x);
		if (tmp)
			while_pri = tmp;
		else
			break;
	}

	if (while_pri)
		tris_parseable_goto(chan, while_pri);

	return 0;
}

static int unload_module(void)
{
	int res;
	
	res = tris_unregister_application(start_app);
	res |= tris_unregister_application(stop_app);
	res |= tris_unregister_application(exit_app);
	res |= tris_unregister_application(continue_app);

	return res;
}

static int load_module(void)
{
	int res;

	res = tris_register_application_xml(start_app, while_start_exec);
	res |= tris_register_application_xml(stop_app, while_end_exec);
	res |= tris_register_application_xml(exit_app, while_exit_exec);
	res |= tris_register_application_xml(continue_app, while_continue_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "While Loops and Conditional Execution");
