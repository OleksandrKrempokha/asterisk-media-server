/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2004-2006 Tilghman Lesher <app_stack_v003@the-tilghman.com>.
 *
 * This code is released by the author with no restrictions on usage.
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
 * \brief Stack applications Gosub, Return, etc.
 *
 * \author Tilghman Lesher <app_stack_v003@the-tilghman.com>
 * 
 * \ingroup applications
 */

/*** MODULEINFO
	<use>res_agi</use>
 ***/

#include "trismedia.h"
 
TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 236303 $")

#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/manager.h"
#include "trismedia/channel.h"
#include "trismedia/agi.h"

/*** DOCUMENTATION
	<application name="Gosub" language="en_US">
		<synopsis>
			Jump to label, saving return address.
		</synopsis>
		<syntax>
			<parameter name="context" />
			<parameter name="exten" />
			<parameter name="priority" required="true" hasparams="optional">
				<argument name="arg1" multiple="true" required="true" />
				<argument name="argN" />
			</parameter>
		</syntax>
		<description>
			<para>Jumps to the label specified, saving the return address.</para>
		</description>
		<see-also>
			<ref type="application">GosubIf</ref>
			<ref type="application">Macro</ref>
			<ref type="application">Goto</ref>
			<ref type="application">Return</ref>
			<ref type="application">StackPop</ref>
		</see-also>
	</application>
	<application name="GosubIf" language="en_US">
		<synopsis>
			Conditionally jump to label, saving return address.
		</synopsis>
		<syntax argsep="?">
			<parameter name="condition" required="true" />
			<parameter name="destination" required="true" argsep=":">
				<argument name="labeliftrue" hasparams="optional">
					<argument name="arg1" required="true" multiple="true" />
					<argument name="argN" />
				</argument>
				<argument name="labeliffalse" hasparams="optional">
					<argument name="arg1" required="true" multiple="true" />
					<argument name="argN" />
				</argument>
			</parameter>
		</syntax>
		<description>
			<para>If the condition is true, then jump to labeliftrue.  If false, jumps to
			labeliffalse, if specified.  In either case, a jump saves the return point
			in the dialplan, to be returned to with a Return.</para>
		</description>
		<see-also>
			<ref type="application">Gosub</ref>
			<ref type="application">Return</ref>
			<ref type="application">MacroIf</ref>
			<ref type="function">IF</ref>
			<ref type="application">GotoIf</ref>
		</see-also>
	</application>
	<application name="Return" language="en_US">
		<synopsis>
			Return from gosub routine.
		</synopsis>
		<syntax>
			<parameter name="value">
				<para>Return value.</para>
			</parameter>
		</syntax>
		<description>
			<para>Jumps to the last label on the stack, removing it. The return <replaceable>value</replaceable>, if
			any, is saved in the channel variable <variable>GOSUB_RETVAL</variable>.</para>
		</description>
		<see-also>
			<ref type="application">Gosub</ref>
			<ref type="application">StackPop</ref>
		</see-also>
	</application>
	<application name="StackPop" language="en_US">
		<synopsis>
			Remove one address from gosub stack.
		</synopsis>
		<syntax />
		<description>
			<para>Removes last label on the stack, discarding it.</para>
		</description>
		<see-also>
			<ref type="application">Return</ref>
			<ref type="application">Gosub</ref>
		</see-also>
	</application>
	<function name="LOCAL" language="en_US">
		<synopsis>
			Manage variables local to the gosub stack frame.
		</synopsis>
		<syntax>
			<parameter name="varname" required="true" />
		</syntax>
		<description>
			<para>Read and write a variable local to the gosub stack frame, once we Return() it will be lost
			(or it will go back to whatever value it had before the Gosub()).</para>
		</description>
		<see-also>
			<ref type="application">Gosub</ref>
			<ref type="application">GosubIf</ref>
			<ref type="application">Return</ref>
		</see-also>
	</function>
	<function name="LOCAL_PEEK" language="en_US">
		<synopsis>
			Retrieve variables hidden by the local gosub stack frame.
		</synopsis>
		<syntax>
			<parameter name="n" required="true" />
			<parameter name="varname" required="true" />
		</syntax>
		<description>
			<para>Read a variable <replaceable>varname</replaceable> hidden by
			<replaceable>n</replaceable> levels of gosub stack frames.  Note that ${LOCAL_PEEK(0,foo)}
			is the same as <variable>foo</variable>, since the value of <replaceable>n</replaceable>
			peeks under 0 levels of stack frames; in other words, 0 is the current level.  If
			<replaceable>n</replaceable> exceeds the available number of stack frames, then an empty
			string is returned.</para>
		</description>
		<see-also>
			<ref type="application">Gosub</ref>
			<ref type="application">GosubIf</ref>
			<ref type="application">Return</ref>
		</see-also>
	</function>
 ***/

static const char *app_gosub = "Gosub";
static const char *app_gosubif = "GosubIf";
static const char *app_return = "Return";
static const char *app_pop = "StackPop";

static void gosub_free(void *data);

static struct tris_datastore_info stack_info = {
	.type = "GOSUB",
	.destroy = gosub_free,
};

struct gosub_stack_frame {
	TRIS_LIST_ENTRY(gosub_stack_frame) entries;
	/* 100 arguments is all that we support anyway, but this will handle up to 255 */
	unsigned char arguments;
	struct varshead varshead;
	int priority;
	unsigned int is_agi:1;
	char *context;
	char extension[0];
};

static int frame_set_var(struct tris_channel *chan, struct gosub_stack_frame *frame, const char *var, const char *value)
{
	struct tris_var_t *variables;
	int found = 0;

	/* Does this variable already exist? */
	TRIS_LIST_TRAVERSE(&frame->varshead, variables, entries) {
		if (!strcmp(var, tris_var_name(variables))) {
			found = 1;
			break;
		}
	}

	if (!found) {
		variables = tris_var_assign(var, "");
		TRIS_LIST_INSERT_HEAD(&frame->varshead, variables, entries);
		pbx_builtin_pushvar_helper(chan, var, value);
	} else {
		pbx_builtin_setvar_helper(chan, var, value);
	}

	manager_event(EVENT_FLAG_DIALPLAN, "VarSet",
		"Channel: %s\r\n"
		"Variable: LOCAL(%s)\r\n"
		"Value: %s\r\n"
		"Uniqueid: %s\r\n",
		chan->name, var, value, chan->uniqueid);
	return 0;
}

static void gosub_release_frame(struct tris_channel *chan, struct gosub_stack_frame *frame)
{
	struct tris_var_t *vardata;

	/* If chan is not defined, then we're calling it as part of gosub_free,
	 * and the channel variables will be deallocated anyway.  Otherwise, we're
	 * just releasing a single frame, so we need to clean up the arguments for
	 * that frame, so that we re-expose the variables from the previous frame
	 * that were hidden by this one.
	 */
	while ((vardata = TRIS_LIST_REMOVE_HEAD(&frame->varshead, entries))) {
		if (chan)
			pbx_builtin_setvar_helper(chan, tris_var_name(vardata), NULL);	
		tris_var_delete(vardata);
	}

	tris_free(frame);
}

static struct gosub_stack_frame *gosub_allocate_frame(const char *context, const char *extension, int priority, unsigned char arguments)
{
	struct gosub_stack_frame *new = NULL;
	int len_extension = strlen(extension), len_context = strlen(context);

	if ((new = tris_calloc(1, sizeof(*new) + 2 + len_extension + len_context))) {
		TRIS_LIST_HEAD_INIT_NOLOCK(&new->varshead);
		strcpy(new->extension, extension);
		new->context = new->extension + len_extension + 1;
		strcpy(new->context, context);
		new->priority = priority;
		new->arguments = arguments;
	}
	return new;
}

static void gosub_free(void *data)
{
	TRIS_LIST_HEAD(, gosub_stack_frame) *oldlist = data;
	struct gosub_stack_frame *oldframe;
	TRIS_LIST_LOCK(oldlist);
	while ((oldframe = TRIS_LIST_REMOVE_HEAD(oldlist, entries))) {
		gosub_release_frame(NULL, oldframe);
	}
	TRIS_LIST_UNLOCK(oldlist);
	TRIS_LIST_HEAD_DESTROY(oldlist);
	tris_free(oldlist);
}

static int pop_exec(struct tris_channel *chan, void *data)
{
	struct tris_datastore *stack_store = tris_channel_datastore_find(chan, &stack_info, NULL);
	struct gosub_stack_frame *oldframe;
	TRIS_LIST_HEAD(, gosub_stack_frame) *oldlist;

	if (!stack_store) {
		tris_log(LOG_WARNING, "%s called with no gosub stack allocated.\n", app_pop);
		return 0;
	}

	oldlist = stack_store->data;
	TRIS_LIST_LOCK(oldlist);
	oldframe = TRIS_LIST_REMOVE_HEAD(oldlist, entries);
	TRIS_LIST_UNLOCK(oldlist);

	if (oldframe) {
		gosub_release_frame(chan, oldframe);
	} else {
		tris_debug(1, "%s called with an empty gosub stack\n", app_pop);
	}
	return 0;
}

static int return_exec(struct tris_channel *chan, void *data)
{
	struct tris_datastore *stack_store = tris_channel_datastore_find(chan, &stack_info, NULL);
	struct gosub_stack_frame *oldframe;
	TRIS_LIST_HEAD(, gosub_stack_frame) *oldlist;
	char *retval = data;
	int res = 0;

	if (!stack_store) {
		tris_log(LOG_ERROR, "Return without Gosub: stack is unallocated\n");
		return -1;
	}

	oldlist = stack_store->data;
	TRIS_LIST_LOCK(oldlist);
	oldframe = TRIS_LIST_REMOVE_HEAD(oldlist, entries);
	TRIS_LIST_UNLOCK(oldlist);

	if (!oldframe) {
		tris_log(LOG_ERROR, "Return without Gosub: stack is empty\n");
		return -1;
	} else if (oldframe->is_agi) {
		/* Exit from AGI */
		res = -1;
	}

	tris_explicit_goto(chan, oldframe->context, oldframe->extension, oldframe->priority);
	gosub_release_frame(chan, oldframe);

	/* Set a return value, if any */
	pbx_builtin_setvar_helper(chan, "GOSUB_RETVAL", S_OR(retval, ""));
	return res;
}

static int gosub_exec(struct tris_channel *chan, void *data)
{
	struct tris_datastore *stack_store = tris_channel_datastore_find(chan, &stack_info, NULL);
	TRIS_LIST_HEAD(, gosub_stack_frame) *oldlist;
	struct gosub_stack_frame *newframe;
	char argname[15], *tmp = tris_strdupa(data), *label, *endparen;
	int i;
	TRIS_DECLARE_APP_ARGS(args2,
		TRIS_APP_ARG(argval)[100];
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_ERROR, "%s requires an argument: %s([[context,]exten,]priority[(arg1[,...][,argN])])\n", app_gosub, app_gosub);
		return -1;
	}

	if (!stack_store) {
		tris_debug(1, "Channel %s has no datastore, so we're allocating one.\n", chan->name);
		stack_store = tris_datastore_alloc(&stack_info, NULL);
		if (!stack_store) {
			tris_log(LOG_ERROR, "Unable to allocate new datastore.  Gosub will fail.\n");
			return -1;
		}

		oldlist = tris_calloc(1, sizeof(*oldlist));
		if (!oldlist) {
			tris_log(LOG_ERROR, "Unable to allocate datastore list head.  Gosub will fail.\n");
			tris_datastore_free(stack_store);
			return -1;
		}

		stack_store->data = oldlist;
		TRIS_LIST_HEAD_INIT(oldlist);
		tris_channel_datastore_add(chan, stack_store);
	}

	/* Separate the arguments from the label */
	/* NOTE:  you cannot use tris_app_separate_args for this, because '(' cannot be used as a delimiter. */
	label = strsep(&tmp, "(");
	if (tmp) {
		endparen = strrchr(tmp, ')');
		if (endparen)
			*endparen = '\0';
		else
			tris_log(LOG_WARNING, "Ouch.  No closing paren: '%s'?\n", (char *)data);
		TRIS_STANDARD_RAW_ARGS(args2, tmp);
	} else
		args2.argc = 0;

	/* Create the return address, but don't save it until we know that the Gosub destination exists */
	newframe = gosub_allocate_frame(chan->context, chan->exten, chan->priority + 1, args2.argc);

	if (!newframe) {
		return -1;
	}

	if (tris_parseable_goto(chan, label)) {
		tris_log(LOG_ERROR, "Gosub address is invalid: '%s'\n", (char *)data);
		tris_free(newframe);
		return -1;
	}

	if (!tris_exists_extension(chan, chan->context, chan->exten, tris_test_flag(chan, TRIS_FLAG_IN_AUTOLOOP) ? chan->priority + 1 : chan->priority, chan->cid.cid_num)) {
		tris_log(LOG_ERROR, "Attempt to reach a non-existent destination for gosub: (Context:%s, Extension:%s, Priority:%d)\n",
				chan->context, chan->exten, chan->priority);
		tris_copy_string(chan->context, newframe->context, sizeof(chan->context));
		tris_copy_string(chan->exten, newframe->extension, sizeof(chan->exten));
		chan->priority = newframe->priority;
		tris_free(newframe);
		return -1;
	}

	/* Now that we know for certain that we're going to a new location, set our arguments */
	for (i = 0; i < args2.argc; i++) {
		snprintf(argname, sizeof(argname), "ARG%d", i + 1);
		frame_set_var(chan, newframe, argname, args2.argval[i]);
		tris_debug(1, "Setting '%s' to '%s'\n", argname, args2.argval[i]);
	}
	snprintf(argname, sizeof(argname), "%d", args2.argc);
	frame_set_var(chan, newframe, "ARGC", argname);

	/* And finally, save our return address */
	oldlist = stack_store->data;
	TRIS_LIST_LOCK(oldlist);
	TRIS_LIST_INSERT_HEAD(oldlist, newframe, entries);
	TRIS_LIST_UNLOCK(oldlist);

	return 0;
}

static int gosubif_exec(struct tris_channel *chan, void *data)
{
	char *args;
	int res=0;
	TRIS_DECLARE_APP_ARGS(cond,
		TRIS_APP_ARG(ition);
		TRIS_APP_ARG(labels);
	);
	TRIS_DECLARE_APP_ARGS(label,
		TRIS_APP_ARG(iftrue);
		TRIS_APP_ARG(iffalse);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "GosubIf requires an argument: GosubIf(cond?label1(args):label2(args)\n");
		return 0;
	}

	args = tris_strdupa(data);
	TRIS_NONSTANDARD_RAW_ARGS(cond, args, '?');
	if (cond.argc != 2) {
		tris_log(LOG_WARNING, "GosubIf requires an argument: GosubIf(cond?label1(args):label2(args)\n");
		return 0;
	}

	TRIS_NONSTANDARD_RAW_ARGS(label, cond.labels, ':');

	if (pbx_checkcondition(cond.ition)) {
		if (!tris_strlen_zero(label.iftrue))
			res = gosub_exec(chan, label.iftrue);
	} else if (!tris_strlen_zero(label.iffalse)) {
		res = gosub_exec(chan, label.iffalse);
	}

	return res;
}

static int local_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_datastore *stack_store = tris_channel_datastore_find(chan, &stack_info, NULL);
	TRIS_LIST_HEAD(, gosub_stack_frame) *oldlist;
	struct gosub_stack_frame *frame;
	struct tris_var_t *variables;

	if (!stack_store)
		return -1;

	oldlist = stack_store->data;
	TRIS_LIST_LOCK(oldlist);
	if (!(frame = TRIS_LIST_FIRST(oldlist))) {
		/* Not within a Gosub routine */
		TRIS_LIST_UNLOCK(oldlist);
		return -1;
	}

	TRIS_LIST_TRAVERSE(&frame->varshead, variables, entries) {
		if (!strcmp(data, tris_var_name(variables))) {
			const char *tmp;
			tris_channel_lock(chan);
			tmp = pbx_builtin_getvar_helper(chan, data);
			tris_copy_string(buf, S_OR(tmp, ""), len);
			tris_channel_unlock(chan);
			break;
		}
	}
	TRIS_LIST_UNLOCK(oldlist);
	return 0;
}

static int local_write(struct tris_channel *chan, const char *cmd, char *var, const char *value)
{
	struct tris_datastore *stack_store = tris_channel_datastore_find(chan, &stack_info, NULL);
	TRIS_LIST_HEAD(, gosub_stack_frame) *oldlist;
	struct gosub_stack_frame *frame;

	if (!stack_store) {
		tris_log(LOG_ERROR, "Tried to set LOCAL(%s), but we aren't within a Gosub routine\n", var);
		return -1;
	}

	oldlist = stack_store->data;
	TRIS_LIST_LOCK(oldlist);
	frame = TRIS_LIST_FIRST(oldlist);

	if (frame)
		frame_set_var(chan, frame, var, value);

	TRIS_LIST_UNLOCK(oldlist);

	return 0;
}

static struct tris_custom_function local_function = {
	.name = "LOCAL",
	.write = local_write,
	.read = local_read,
};

static int peek_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	int found = 0, n;
	struct tris_var_t *variables;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(n);
		TRIS_APP_ARG(name);
	);

	if (!chan) {
		tris_log(LOG_ERROR, "LOCAL_PEEK must be called on an active channel\n");
		return -1;
	}

	TRIS_STANDARD_RAW_ARGS(args, data);
	n = atoi(args.n);
	*buf = '\0';

	tris_channel_lock(chan);
	TRIS_LIST_TRAVERSE(&chan->varshead, variables, entries) {
		if (!strcmp(args.name, tris_var_name(variables)) && ++found > n) {
			tris_copy_string(buf, tris_var_value(variables), len);
			break;
		}
	}
	tris_channel_unlock(chan);
	return 0;
}

static struct tris_custom_function peek_function = {
	.name = "LOCAL_PEEK",
	.read = peek_read,
};

static int handle_gosub(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int old_priority, priority;
	char old_context[TRIS_MAX_CONTEXT], old_extension[TRIS_MAX_EXTENSION];
	struct tris_app *theapp;
	char *gosub_args;

	if (argc < 4 || argc > 5) {
		return RESULT_SHOWUSAGE;
	}

	tris_debug(1, "Gosub called with %d arguments: 0:%s 1:%s 2:%s 3:%s 4:%s\n", argc, argv[0], argv[1], argv[2], argv[3], argc == 5 ? argv[4] : "");

	if (sscanf(argv[3], "%30d", &priority) != 1 || priority < 1) {
		/* Lookup the priority label */
		if ((priority = tris_findlabel_extension(chan, argv[1], argv[2], argv[3], chan->cid.cid_num)) < 0) {
			tris_log(LOG_ERROR, "Priority '%s' not found in '%s@%s'\n", argv[3], argv[2], argv[1]);
			tris_agi_send(agi->fd, chan, "200 result=-1 Gosub label not found\n");
			return RESULT_FAILURE;
		}
	} else if (!tris_exists_extension(chan, argv[1], argv[2], priority, chan->cid.cid_num)) {
		tris_agi_send(agi->fd, chan, "200 result=-1 Gosub label not found\n");
		return RESULT_FAILURE;
	}

	/* Save previous location, since we're going to change it */
	tris_copy_string(old_context, chan->context, sizeof(old_context));
	tris_copy_string(old_extension, chan->exten, sizeof(old_extension));
	old_priority = chan->priority;

	if (!(theapp = pbx_findapp("Gosub"))) {
		tris_log(LOG_ERROR, "Gosub() cannot be found in the list of loaded applications\n");
		tris_agi_send(agi->fd, chan, "503 result=-2 Gosub is not loaded\n");
		return RESULT_FAILURE;
	}

	/* Apparently, if you run tris_pbx_run on a channel that already has a pbx
	 * structure, you need to add 1 to the priority to get it to go to the
	 * right place.  But if it doesn't have a pbx structure, then leaving off
	 * the 1 is the right thing to do.  See how this code differs when we
	 * call a Gosub for the CALLEE channel in Dial or Queue.
	 */
	if (argc == 5) {
		if (asprintf(&gosub_args, "%s,%s,%d(%s)", argv[1], argv[2], priority + (chan->pbx ? 1 : 0), argv[4]) < 0) {
			tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
			gosub_args = NULL;
		}
	} else {
		if (asprintf(&gosub_args, "%s,%s,%d", argv[1], argv[2], priority + (chan->pbx ? 1 : 0)) < 0) {
			tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
			gosub_args = NULL;
		}
	}

	if (gosub_args) {
		int res;

		tris_debug(1, "Trying gosub with arguments '%s'\n", gosub_args);

		if ((res = pbx_exec(chan, theapp, gosub_args)) == 0) {
			struct tris_pbx *pbx = chan->pbx;
			struct tris_pbx_args args;
			struct tris_datastore *stack_store = tris_channel_datastore_find(chan, &stack_info, NULL);
			TRIS_LIST_HEAD(, gosub_stack_frame) *oldlist = stack_store->data;
			struct gosub_stack_frame *cur = TRIS_LIST_FIRST(oldlist);
			cur->is_agi = 1;

			memset(&args, 0, sizeof(args));
			args.no_hangup_chan = 1;
			/* Suppress warning about PBX already existing */
			chan->pbx = NULL;
			tris_agi_send(agi->fd, chan, "100 result=0 Trying...\n");
			tris_pbx_run_args(chan, &args);
			tris_agi_send(agi->fd, chan, "200 result=0 Gosub complete\n");
			if (chan->pbx) {
				tris_free(chan->pbx);
			}
			chan->pbx = pbx;
		} else {
			tris_agi_send(agi->fd, chan, "200 result=%d Gosub failed\n", res);
		}
		tris_free(gosub_args);
	} else {
		tris_agi_send(agi->fd, chan, "503 result=-2 Memory allocation failure\n");
		return RESULT_FAILURE;
	}

	/* Restore previous location */
	tris_copy_string(chan->context, old_context, sizeof(chan->context));
	tris_copy_string(chan->exten, old_extension, sizeof(chan->exten));
	chan->priority = old_priority;

	return RESULT_SUCCESS;
}

static char usage_gosub[] =
" Usage: GOSUB <context> <extension> <priority> [<optional-argument>]\n"
"   Cause the channel to execute the specified dialplan subroutine, returning\n"
" to the dialplan with execution of a Return()\n";

struct agi_command gosub_agi_command =
	{ { "gosub", NULL }, handle_gosub, "Execute a dialplan subroutine", usage_gosub , 0 };

static int unload_module(void)
{
	if (tris_agi_unregister) {
		 tris_agi_unregister(tris_module_info->self, &gosub_agi_command);
	}

	tris_unregister_application(app_return);
	tris_unregister_application(app_pop);
	tris_unregister_application(app_gosubif);
	tris_unregister_application(app_gosub);
	tris_custom_function_unregister(&local_function);
	tris_custom_function_unregister(&peek_function);

	return 0;
}

static int load_module(void)
{
	if (tris_agi_register) {
		 tris_agi_register(tris_module_info->self, &gosub_agi_command);
	}

	tris_register_application_xml(app_pop, pop_exec);
	tris_register_application_xml(app_return, return_exec);
	tris_register_application_xml(app_gosubif, gosubif_exec);
	tris_register_application_xml(app_gosub, gosub_exec);
	tris_custom_function_register(&local_function);
	tris_custom_function_register(&peek_function);

	return 0;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Dialplan subroutines (Gosub, Return, etc)");
