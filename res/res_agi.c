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
 * \brief AGI - the Trismedia Gateway Interface
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \todo Convert the rest of the AGI commands over to XML documentation
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 246199 $")

#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>

#include "trismedia/paths.h"	/* use many tris_config_TRIS_*_DIR */
#include "trismedia/network.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/astdb.h"
#include "trismedia/callerid.h"
#include "trismedia/cli.h"
#include "trismedia/image.h"
#include "trismedia/say.h"
#include "trismedia/app.h"
#include "trismedia/dsp.h"
#include "trismedia/musiconhold.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/strings.h"
#include "trismedia/manager.h"
#include "trismedia/tris_version.h"
#include "trismedia/speech.h"
#include "trismedia/manager.h"
#include "trismedia/features.h"
#include "trismedia/term.h"
#include "trismedia/xmldoc.h"

#define TRIS_API_MODULE
#include "trismedia/agi.h"

/*** DOCUMENTATION
	<agi name="answer" language="en_US">
		<synopsis>
			Answer channel
		</synopsis>
		<syntax />
		<description>
			<para>Answers channel if not already in answer state. Returns <literal>-1</literal> on
			channel failure, or <literal>0</literal> if successful.</para>
		</description>
		<see-also>
			<ref type="agi">hangup</ref>
		</see-also>
	</agi>
	<agi name="asyncagi break" language="en_US">
		<synopsis>
			Interrupts Async AGI
		</synopsis>
		<syntax />
		<description>
			<para>Interrupts expected flow of Async AGI commands and returns control to previous source
			(typically, the PBX dialplan).</para>
		</description>
		<see-also>
			<ref type="agi">hangup</ref>
		</see-also>
	</agi>
	<agi name="channel status" language="en_US">
		<synopsis>
			Returns status of the connected channel.
		</synopsis>
		<syntax>
			<parameter name="channelname" />
		</syntax>
		<description>
			<para>Returns the status of the specified <replaceable>channelname</replaceable>.
			If no channel name is given then returns the status of the current channel.</para>
			<para>Return values:</para>
			<enumlist>
				<enum name="0">
					<para>Channel is down and available.</para>
				</enum>
				<enum name="1">
					<para>Channel is down, but reserved.</para>
				</enum>
				<enum name="2">
					<para>Channel is off hook.</para>
				</enum>
				<enum name="3">
					<para>Digits (or equivalent) have been dialed.</para>
				</enum>
				<enum name="4">
					<para>Line is ringing.</para>
				</enum>
				<enum name="5">
					<para>Remote end is ringing.</para>
				</enum>
				<enum name="6">
					<para>Line is up.</para>
				</enum>
				<enum name="7">
					<para>Line is busy.</para>
				</enum>
			</enumlist>
		</description>
	</agi>
	<agi name="database del" language="en_US">
		<synopsis>
			Removes database key/value
		</synopsis>
		<syntax>
			<parameter name="family" required="true" />
			<parameter name="key" required="true" />
		</syntax>
		<description>
			<para>Deletes an entry in the Trismedia database for a given
			<replaceable>family</replaceable> and <replaceable>key</replaceable>.</para>
			<para>Returns <literal>1</literal> if successful, <literal>0</literal>
			otherwise.</para>
		</description>
	</agi>
	<agi name="database deltree" language="en_US">
		<synopsis>
			Removes database keytree/value
		</synopsis>
		<syntax>
			<parameter name="family" required="true" />
			<parameter name="keytree" />
		</syntax>
		<description>
			<para>Deletes a <replaceable>family</replaceable> or specific <replaceable>keytree</replaceable>
			within a <replaceable>family</replaceable> in the Trismedia database.</para>
			<para>Returns <literal>1</literal> if successful, <literal>0</literal> otherwise.</para>
		</description>
	</agi>
	<agi name="database get" language="en_US">
		<synopsis>
			Gets database value
		</synopsis>
		<syntax>
			<parameter name="family" required="true" />
			<parameter name="key" required="true" />
		</syntax>
		<description>
			<para>Retrieves an entry in the Trismedia database for a given <replaceable>family</replaceable>
			and <replaceable>key</replaceable>.</para>
			<para>Returns <literal>0</literal> if <replaceable>key</replaceable> is not set.
			Returns <literal>1</literal> if <replaceable>key</replaceable> is set and returns the variable
			in parenthesis.</para>
			<para>Example return code: 200 result=1 (testvariable)</para>
		</description>
	</agi>
	<agi name="database put" language="en_US">
		<synopsis>
			Adds/updates database value
		</synopsis>
		<syntax>
			<parameter name="family" required="true" />
			<parameter name="key" required="true" />
			<parameter name="value" required="true" />
		</syntax>
		<description>
			<para>Adds or updates an entry in the Trismedia database for a given
			<replaceable>family</replaceable>, <replaceable>key</replaceable>, and
			<replaceable>value</replaceable>.</para>
			<para>Returns <literal>1</literal> if successful, <literal>0</literal> otherwise.</para>
		</description>
	</agi>
	<agi name="exec" language="en_US">
		<synopsis>
			Executes a given Application
		</synopsis>
		<syntax>
			<parameter name="application" required="true" />
			<parameter name="options" required="true" />
		</syntax>
		<description>
			<para>Executes <replaceable>application</replaceable> with given
			<replaceable>options</replaceable>.</para>
			<para>Returns whatever the <replaceable>application</replaceable> returns, or
			<literal>-2</literal> on failure to find <replaceable>application</replaceable>.</para>
		</description>
	</agi>
	<agi name="get data" language="en_US">
		<synopsis>
			Prompts for DTMF on a channel
		</synopsis>
		<syntax>
			<parameter name="file" required="true" />
			<parameter name="timeout" />
			<parameter name="maxdigits" />
		</syntax>
		<description>
			<para>Stream the given <replaceable>file</replaceable>, and receive DTMF data.</para>
			<para>Returns the digits received from the channel at the other end.</para>
		</description>
	</agi>
	<agi name="get full variable" language="en_US">
		<synopsis>
			Evaluates a channel expression
		</synopsis>
		<syntax>
			<parameter name="variablename" required="true" />
			<parameter name="channel name" />
		</syntax>
		<description>
			<para>Returns <literal>0</literal> if <replaceable>variablename</replaceable> is not set
			or channel does not exist. Returns <literal>1</literal> if <replaceable>variablename</replaceable>
			is set and returns the variable in parenthesis. Understands complex variable names and builtin
			variables, unlike GET VARIABLE.</para>
			<para>Example return code: 200 result=1 (testvariable)</para>
		</description>
	</agi>
	<agi name="get option" language="en_US">
		<synopsis>
			Stream file, prompt for DTMF, with timeout.
		</synopsis>
		<syntax>
			<parameter name="filename" required="true" />
			<parameter name="escape_digits" required="true" />
			<parameter name="timeout" />
		</syntax>
		<description>
			<para>Behaves similar to STREAM FILE but used with a timeout option.</para>
		</description>
		<see-also>
			<ref type="agi">stream file</ref>
		</see-also>
	</agi>
	<agi name="get variable" language="en_US">
		<synopsis>
			Gets a channel variable.
		</synopsis>
		<syntax>
			<parameter name="variablename" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>0</literal> if <replaceable>variablename</replaceable> is not set.
			Returns <literal>1</literal> if <replaceable>variablename</replaceable> is set and returns
			the variable in parentheses.</para>
			<para>Example return code: 200 result=1 (testvariable)</para>
		</description>
	</agi>
	<agi name="hangup" language="en_US">
		<synopsis>
			Hangup the current channel.
		</synopsis>
		<syntax>
			<parameter name="channelname" />
		</syntax>
		<description>
			<para>Hangs up the specified channel. If no channel name is given, hangs
			up the current channel</para>
		</description>
	</agi>
	<agi name="noop" language="en_US">
		<synopsis>
			Does nothing.
		</synopsis>
		<syntax />
		<description>
			<para>Does nothing.</para>
		</description>
	</agi>
	<agi name="set music" language="en_US">
		<synopsis>
			Enable/Disable Music on hold generator
		</synopsis>
		<syntax>
			<parameter required="true">
				<enumlist>
					<enum>
						<parameter name="on" literal="true" required="true" />
					</enum>
					<enum>
						<parameter name="off" literal="true" required="true" />
					</enum>
				</enumlist>
			</parameter>
			<parameter name="class" required="true" />
		</syntax>
		<description>
			<para>Enables/Disables the music on hold generator. If <replaceable>class</replaceable>
			is not specified, then the <literal>default</literal> music on hold class will be
			used.</para>
			<para>Always returns <literal>0</literal>.</para>
		</description>
	</agi>
 ***/

#define MAX_ARGS 128
#define MAX_CMD_LEN 80
#define AGI_NANDFS_RETRY 3
#define AGI_BUF_LEN 2048

static char *app = "AGI";

static char *eapp = "EAGI";

static char *deadapp = "DeadAGI";

static char *synopsis = "Executes an AGI compliant application";
static char *esynopsis = "Executes an EAGI compliant application";
static char *deadsynopsis = "Executes AGI on a hungup channel";

static char *descrip =
"  [E|Dead]AGI(command,args): Executes an Trismedia Gateway Interface compliant\n"
"program on a channel. AGI allows Trismedia to launch external programs written\n"
"in any language to control a telephony channel, play audio, read DTMF digits,\n"
"etc. by communicating with the AGI protocol on stdin and stdout.\n"
"  As of 1.6.0, this channel will not stop dialplan execution on hangup inside\n"
"of this application. Dialplan execution will continue normally, even upon\n"
"hangup until the AGI application signals a desire to stop (either by exiting\n"
"or, in the case of a net script, by closing the connection).\n"
"  A locally executed AGI script will receive SIGHUP on hangup from the channel\n"
"except when using DeadAGI. A fast AGI server will correspondingly receive a\n"
"HANGUP in OOB data. Both of these signals may be disabled by setting the\n"
"AGISIGHUP channel variable to \"no\" before executing the AGI application.\n"
"  Using 'EAGI' provides enhanced AGI, with incoming audio available out of band\n"
"on file descriptor 3.\n\n"
"  Use the CLI command 'agi show commnands' to list available agi commands.\n"
"  This application sets the following channel variable upon completion:\n"
"     AGISTATUS      The status of the attempt to the run the AGI script\n"
"                    text string, one of SUCCESS | FAILURE | NOTFOUND | HANGUP\n";

static int agidebug = 0;

#define TONE_BLOCK_SIZE 200

/* Max time to connect to an AGI remote host */
#define MAX_AGI_CONNECT 2000

#define AGI_PORT 4573

enum agi_result {
	AGI_RESULT_FAILURE = -1,
	AGI_RESULT_SUCCESS,
	AGI_RESULT_SUCCESS_FAST,
	AGI_RESULT_SUCCESS_ASYNC,
	AGI_RESULT_NOTFOUND,
	AGI_RESULT_HANGUP,
};

static agi_command *find_command(char *cmds[], int exact);

TRIS_THREADSTORAGE(agi_buf);
#define AGI_BUF_INITSIZE 256

int tris_agi_send(int fd, struct tris_channel *chan, char *fmt, ...)
{
	int res = 0;
	va_list ap;
	struct tris_str *buf;

	if (!(buf = tris_str_thread_get(&agi_buf, AGI_BUF_INITSIZE)))
		return -1;

	va_start(ap, fmt);
	res = tris_str_set_va(&buf, 0, fmt, ap);
	va_end(ap);

	if (res == -1) {
		tris_log(LOG_ERROR, "Out of memory\n");
		return -1;
	}

	if (agidebug) {
		if (chan) {
			tris_verbose("<%s>AGI Tx >> %s", chan->name, tris_str_buffer(buf));
		} else {
			tris_verbose("AGI Tx >> %s", tris_str_buffer(buf));
		}
	}

	return tris_carefulwrite(fd, tris_str_buffer(buf), tris_str_strlen(buf), 100);
}

/* linked list of AGI commands ready to be executed by Async AGI */
struct agi_cmd {
	char *cmd_buffer;
	char *cmd_id;
	TRIS_LIST_ENTRY(agi_cmd) entry;
};

static void free_agi_cmd(struct agi_cmd *cmd)
{
	tris_free(cmd->cmd_buffer);
	tris_free(cmd->cmd_id);
	tris_free(cmd);
}

/* AGI datastore destructor */
static void agi_destroy_commands_cb(void *data)
{
	struct agi_cmd *cmd;
	TRIS_LIST_HEAD(, agi_cmd) *chan_cmds = data;
	TRIS_LIST_LOCK(chan_cmds);
	while ( (cmd = TRIS_LIST_REMOVE_HEAD(chan_cmds, entry)) ) {
		free_agi_cmd(cmd);
	}
	TRIS_LIST_UNLOCK(chan_cmds);
	TRIS_LIST_HEAD_DESTROY(chan_cmds);
	tris_free(chan_cmds);
}

/* channel datastore to keep the queue of AGI commands in the channel */
static const struct tris_datastore_info agi_commands_datastore_info = {
	.type = "AsyncAGI",
	.destroy = agi_destroy_commands_cb
};

static const char mandescr_asyncagi[] =
"Description: Add an AGI command to the execute queue of the channel in Async AGI\n"
"Variables:\n"
"  *Channel: Channel that is currently in Async AGI\n"
"  *Command: Application to execute\n"
"   CommandID: comand id. This will be sent back in CommandID header of AsyncAGI exec event notification\n"
"\n";

static struct agi_cmd *get_agi_cmd(struct tris_channel *chan)
{
	struct tris_datastore *store;
	struct agi_cmd *cmd;
	TRIS_LIST_HEAD(, agi_cmd) *agi_commands;

	tris_channel_lock(chan);
	store = tris_channel_datastore_find(chan, &agi_commands_datastore_info, NULL);
	tris_channel_unlock(chan);
	if (!store) {
		tris_log(LOG_ERROR, "Hu? datastore disappeared at Async AGI on Channel %s!\n", chan->name);
		return NULL;
	}
	agi_commands = store->data;
	TRIS_LIST_LOCK(agi_commands);
	cmd = TRIS_LIST_REMOVE_HEAD(agi_commands, entry);
	TRIS_LIST_UNLOCK(agi_commands);
	return cmd;
}

/* channel is locked when calling this one either from the CLI or manager thread */
static int add_agi_cmd(struct tris_channel *chan, const char *cmd_buff, const char *cmd_id)
{
	struct tris_datastore *store;
	struct agi_cmd *cmd;
	TRIS_LIST_HEAD(, agi_cmd) *agi_commands;

	store = tris_channel_datastore_find(chan, &agi_commands_datastore_info, NULL);
	if (!store) {
		tris_log(LOG_WARNING, "Channel %s is not at Async AGI.\n", chan->name);
		return -1;
	}
	agi_commands = store->data;
	cmd = tris_calloc(1, sizeof(*cmd));
	if (!cmd) {
		return -1;
	}
	cmd->cmd_buffer = tris_strdup(cmd_buff);
	if (!cmd->cmd_buffer) {
		tris_free(cmd);
		return -1;
	}
	cmd->cmd_id = tris_strdup(cmd_id);
	if (!cmd->cmd_id) {
		tris_free(cmd->cmd_buffer);
		tris_free(cmd);
		return -1;
	}
	TRIS_LIST_LOCK(agi_commands);
	TRIS_LIST_INSERT_TAIL(agi_commands, cmd, entry);
	TRIS_LIST_UNLOCK(agi_commands);
	return 0;
}

static int add_to_agi(struct tris_channel *chan)
{
	struct tris_datastore *datastore;
	TRIS_LIST_HEAD(, agi_cmd) *agi_cmds_list;

	/* check if already on AGI */
	tris_channel_lock(chan);
	datastore = tris_channel_datastore_find(chan, &agi_commands_datastore_info, NULL);
	tris_channel_unlock(chan);
	if (datastore) {
		/* we already have an AGI datastore, let's just
		   return success */
		return 0;
	}

	/* the channel has never been on Async AGI,
	   let's allocate it's datastore */
	datastore = tris_datastore_alloc(&agi_commands_datastore_info, "AGI");
	if (!datastore) {
		return -1;
	}
	agi_cmds_list = tris_calloc(1, sizeof(*agi_cmds_list));
	if (!agi_cmds_list) {
		tris_log(LOG_ERROR, "Unable to allocate Async AGI commands list.\n");
		tris_datastore_free(datastore);
		return -1;
	}
	datastore->data = agi_cmds_list;
	TRIS_LIST_HEAD_INIT(agi_cmds_list);
	tris_channel_lock(chan);
	tris_channel_datastore_add(chan, datastore);
	tris_channel_unlock(chan);
	return 0;
}

/*!
 * \brief CLI command to add applications to execute in Async AGI
 * \param e
 * \param cmd
 * \param a
 *
 * \retval CLI_SUCCESS on success
 * \retval NULL when init or tab completion is used
*/
static char *handle_cli_agi_add_cmd(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_channel *chan;
	switch (cmd) {
	case CLI_INIT:
		e->command = "agi exec";
		e->usage = "Usage: agi exec <channel name> <app and arguments> [id]\n"
			   "       Add AGI command to the execute queue of the specified channel in Async AGI\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 2)
			return tris_complete_channels(a->line, a->word, a->pos, a->n, 2);
		return NULL;
	}

	if (a->argc < 4)
		return CLI_SHOWUSAGE;
	chan = tris_get_channel_by_name_locked(a->argv[2]);
	if (!chan) {
		tris_log(LOG_WARNING, "Channel %s does not exists or cannot lock it\n", a->argv[2]);
		return CLI_FAILURE;
	}
	if (add_agi_cmd(chan, a->argv[3], (a->argc > 4 ? a->argv[4] : ""))) {
		tris_log(LOG_WARNING, "failed to add AGI command to queue of channel %s\n", chan->name);
		tris_channel_unlock(chan);
		return CLI_FAILURE;
	}
	tris_log(LOG_DEBUG, "Added AGI command to channel %s queue\n", chan->name);
	tris_channel_unlock(chan);
	return CLI_SUCCESS;
}

/*!
 * \brief Add a new command to execute by the Async AGI application
 * \param s
 * \param m
 *
 * It will append the application to the specified channel's queue
 * if the channel is not inside Async AGI application it will return an error
 * \retval 0 on success or incorrect use
 * \retval 1 on failure to add the command ( most likely because the channel
 * is not in Async AGI loop )
*/
static int action_add_agi_cmd(struct mansession *s, const struct message *m)
{
	const char *channel = astman_get_header(m, "Channel");
	const char *cmdbuff = astman_get_header(m, "Command");
	const char *cmdid   = astman_get_header(m, "CommandID");
	struct tris_channel *chan;
	char buf[256];
	if (tris_strlen_zero(channel) || tris_strlen_zero(cmdbuff)) {
		astman_send_error(s, m, "Both, Channel and Command are *required*");
		return 0;
	}
	chan = tris_get_channel_by_name_locked(channel);
	if (!chan) {
		snprintf(buf, sizeof(buf), "Channel %s does not exists or cannot get its lock", channel);
		astman_send_error(s, m, buf);
		return 0;
	}
	if (add_agi_cmd(chan, cmdbuff, cmdid)) {
		snprintf(buf, sizeof(buf), "Failed to add AGI command to channel %s queue", chan->name);
		astman_send_error(s, m, buf);
		tris_channel_unlock(chan);
		return 0;
	}
	astman_send_ack(s, m, "Added AGI command to queue");
	tris_channel_unlock(chan);
	return 0;
}

static int agi_handle_command(struct tris_channel *chan, AGI *agi, char *buf, int dead);
static void setup_env(struct tris_channel *chan, char *request, int fd, int enhanced, int argc, char *argv[]);
static enum agi_result launch_asyncagi(struct tris_channel *chan, char *argv[], int *efd)
{
/* This buffer sizes might cause truncation if the AGI command writes more data
   than AGI_BUF_SIZE as result. But let's be serious, is there an AGI command
   that writes a response larger than 1024 bytes?, I don't think so, most of
   them are just result=blah stuff. However probably if GET VARIABLE is called
   and the variable has large amount of data, that could be a problem. We could
   make this buffers dynamic, but let's leave that as a second step.

   AMI_BUF_SIZE is twice AGI_BUF_SIZE just for the sake of choosing a safe
   number. Some characters of AGI buf will be url encoded to be sent to manager
   clients.  An URL encoded character will take 3 bytes, but again, to cause
   truncation more than about 70% of the AGI buffer should be URL encoded for
   that to happen.  Not likely at all.

   On the other hand. I wonder if read() could eventually return less data than
   the amount already available in the pipe? If so, how to deal with that?
   So far, my tests on Linux have not had any problems.
 */
#define AGI_BUF_SIZE 1024
#define AMI_BUF_SIZE 2048
	struct tris_frame *f;
	struct agi_cmd *cmd;
	int res, fds[2];
	int timeout = 100;
	char agi_buffer[AGI_BUF_SIZE + 1];
	char ami_buffer[AMI_BUF_SIZE];
	enum agi_result returnstatus = AGI_RESULT_SUCCESS_ASYNC;
	AGI async_agi;

	if (efd) {
		tris_log(LOG_WARNING, "Async AGI does not support Enhanced AGI yet\n");
		return AGI_RESULT_FAILURE;
	}

	/* add AsyncAGI datastore to the channel */
	if (add_to_agi(chan)) {
		tris_log(LOG_ERROR, "failed to start Async AGI on channel %s\n", chan->name);
		return AGI_RESULT_FAILURE;
	}

	/* this pipe allows us to create a "fake" AGI struct to use
	   the AGI commands */
	res = pipe(fds);
	if (res) {
		tris_log(LOG_ERROR, "failed to create Async AGI pipe\n");
		/* intentionally do not remove datastore, added with
		   add_to_agi(), from channel. It will be removed when
		   the channel is hung up anyways */
		return AGI_RESULT_FAILURE;
	}

	/* handlers will get the pipe write fd and we read the AGI responses
	   from the pipe read fd */
	async_agi.fd = fds[1];
	async_agi.ctrl = fds[1];
	async_agi.audio = -1; /* no audio support */
	async_agi.fast = 0;
	async_agi.speech = NULL;

	/* notify possible manager users of a new channel ready to
	   receive commands */
	setup_env(chan, "async", fds[1], 0, 0, NULL);
	/* read the environment */
	res = read(fds[0], agi_buffer, AGI_BUF_SIZE);
	if (!res) {
		tris_log(LOG_ERROR, "failed to read from Async AGI pipe on channel %s\n", chan->name);
		returnstatus = AGI_RESULT_FAILURE;
		goto quit;
	}
	agi_buffer[res] = '\0';
	/* encode it and send it thru the manager so whoever is going to take
	   care of AGI commands on this channel can decide which AGI commands
	   to execute based on the setup info */
	tris_uri_encode(agi_buffer, ami_buffer, AMI_BUF_SIZE, 1);
	manager_event(EVENT_FLAG_AGI, "AsyncAGI", "SubEvent: Start\r\nChannel: %s\r\nEnv: %s\r\n", chan->name, ami_buffer);
	while (1) {
		/* bail out if we need to hangup */
		if (tris_check_hangup(chan)) {
			tris_log(LOG_DEBUG, "tris_check_hangup returned true on chan %s\n", chan->name);
			break;
		}
		/* retrieve a command
		   (commands are added via the manager or the cli threads) */
		cmd = get_agi_cmd(chan);
		if (cmd) {
			/* OK, we have a command, let's call the
			   command handler. */
			res = agi_handle_command(chan, &async_agi, cmd->cmd_buffer, 0);
			if (res < 0) {
				free_agi_cmd(cmd);
				break;
			}
			/* the command handler must have written to our fake
			   AGI struct fd (the pipe), let's read the response */
			res = read(fds[0], agi_buffer, AGI_BUF_SIZE);
			if (!res) {
				returnstatus = AGI_RESULT_FAILURE;
				tris_log(LOG_ERROR, "failed to read from AsyncAGI pipe on channel %s\n", chan->name);
				free_agi_cmd(cmd);
				break;
			}
			/* we have a response, let's send the response thru the
			   manager. Include the CommandID if it was specified
			   when the command was added */
			agi_buffer[res] = '\0';
			tris_uri_encode(agi_buffer, ami_buffer, AMI_BUF_SIZE, 1);
			if (tris_strlen_zero(cmd->cmd_id))
				manager_event(EVENT_FLAG_AGI, "AsyncAGI", "SubEvent: Exec\r\nChannel: %s\r\nResult: %s\r\n", chan->name, ami_buffer);
			else
				manager_event(EVENT_FLAG_AGI, "AsyncAGI", "SubEvent: Exec\r\nChannel: %s\r\nCommandID: %s\r\nResult: %s\r\n", chan->name, cmd->cmd_id, ami_buffer);
			free_agi_cmd(cmd);
		} else {
			/* no command so far, wait a bit for a frame to read */
			res = tris_waitfor(chan, timeout);
			if (res < 0) {
				tris_log(LOG_DEBUG, "tris_waitfor returned <= 0 on chan %s\n", chan->name);
				break;
			}
			if (res == 0)
				continue;
			f = tris_read(chan);
			if (!f) {
				tris_log(LOG_DEBUG, "No frame read on channel %s, going out ...\n", chan->name);
				returnstatus = AGI_RESULT_HANGUP;
				break;
			}
			/* is there any other frame we should care about
			   besides TRIS_CONTROL_HANGUP? */
			if (f->frametype == TRIS_FRAME_CONTROL && f->subclass == TRIS_CONTROL_HANGUP) {
				tris_log(LOG_DEBUG, "Got HANGUP frame on channel %s, going out ...\n", chan->name);
				tris_frfree(f);
				break;
			}
			tris_frfree(f);
		}
	}

	if (async_agi.speech) {
		tris_speech_destroy(async_agi.speech);
	}
quit:
	/* notify manager users this channel cannot be
	   controlled anymore by Async AGI */
	manager_event(EVENT_FLAG_AGI, "AsyncAGI", "SubEvent: End\r\nChannel: %s\r\n", chan->name);

	/* close the pipe */
	close(fds[0]);
	close(fds[1]);

	/* intentionally don't get rid of the datastore. So commands can be
	   still in the queue in case AsyncAGI gets called again.
	   Datastore destructor will be called on channel destroy anyway  */

	return returnstatus;

#undef AGI_BUF_SIZE
#undef AMI_BUF_SIZE
}

/* launch_netscript: The fastagi handler.
	FastAGI defaults to port 4573 */
static enum agi_result launch_netscript(char *agiurl, char *argv[], int *fds, int *efd, int *opid)
{
	int s, flags, res, port = AGI_PORT;
	struct pollfd pfds[1];
	char *host, *c, *script = "";
	struct sockaddr_in addr_in;
	struct hostent *hp;
	struct tris_hostent ahp;

	/* agiusl is "agi://host.domain[:port][/script/name]" */
	host = tris_strdupa(agiurl + 6);	/* Remove agi:// */
	/* Strip off any script name */
	if ((c = strchr(host, '/'))) {
		*c = '\0';
		c++;
		script = c;
	}
	if ((c = strchr(host, ':'))) {
		*c = '\0';
		c++;
		port = atoi(c);
	}
	if (efd) {
		tris_log(LOG_WARNING, "AGI URI's don't support Enhanced AGI yet\n");
		return -1;
	}
	if (!(hp = tris_gethostbyname(host, &ahp))) {
		tris_log(LOG_WARNING, "Unable to locate host '%s'\n", host);
		return -1;
	}
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		tris_log(LOG_WARNING, "Unable to create socket: %s\n", strerror(errno));
		return -1;
	}
	if ((flags = fcntl(s, F_GETFL)) < 0) {
		tris_log(LOG_WARNING, "Fcntl(F_GETFL) failed: %s\n", strerror(errno));
		close(s);
		return -1;
	}
	if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
		tris_log(LOG_WARNING, "Fnctl(F_SETFL) failed: %s\n", strerror(errno));
		close(s);
		return -1;
	}
	memset(&addr_in, 0, sizeof(addr_in));
	addr_in.sin_family = AF_INET;
	addr_in.sin_port = htons(port);
	memcpy(&addr_in.sin_addr, hp->h_addr, sizeof(addr_in.sin_addr));
	if (connect(s, (struct sockaddr *)&addr_in, sizeof(addr_in)) && (errno != EINPROGRESS)) {
		tris_log(LOG_WARNING, "Connect failed with unexpected error: %s\n", strerror(errno));
		close(s);
		return AGI_RESULT_FAILURE;
	}

	pfds[0].fd = s;
	pfds[0].events = POLLOUT;
	while ((res = tris_poll(pfds, 1, MAX_AGI_CONNECT)) != 1) {
		if (errno != EINTR) {
			if (!res) {
				tris_log(LOG_WARNING, "FastAGI connection to '%s' timed out after MAX_AGI_CONNECT (%d) milliseconds.\n",
					agiurl, MAX_AGI_CONNECT);
			} else
				tris_log(LOG_WARNING, "Connect to '%s' failed: %s\n", agiurl, strerror(errno));
			close(s);
			return AGI_RESULT_FAILURE;
		}
	}

	if (tris_agi_send(s, NULL, "agi_network: yes\n") < 0) {
		if (errno != EINTR) {
			tris_log(LOG_WARNING, "Connect to '%s' failed: %s\n", agiurl, strerror(errno));
			close(s);
			return AGI_RESULT_FAILURE;
		}
	}

	/* If we have a script parameter, relay it to the fastagi server */
	/* Script parameters take the form of: AGI(agi://my.example.com/?extension=${EXTEN}) */
	if (!tris_strlen_zero(script))
		tris_agi_send(s, NULL, "agi_network_script: %s\n", script);

	tris_debug(4, "Wow, connected!\n");
	fds[0] = s;
	fds[1] = s;
	*opid = -1;
	return AGI_RESULT_SUCCESS_FAST;
}

static enum agi_result launch_script(struct tris_channel *chan, char *script, char *argv[], int *fds, int *efd, int *opid)
{
	char tmp[256];
	int pid, toast[2], fromast[2], audio[2], res;
	struct stat st;

	if (!strncasecmp(script, "agi://", 6))
		return launch_netscript(script, argv, fds, efd, opid);
	if (!strncasecmp(script, "agi:async", sizeof("agi:async")-1))
		return launch_asyncagi(chan, argv, efd);

	if (script[0] != '/') {
		snprintf(tmp, sizeof(tmp), "%s/%s", tris_config_TRIS_AGI_DIR, script);
		script = tmp;
	}

	/* Before even trying let's see if the file actually exists */
	if (stat(script, &st)) {
		tris_log(LOG_WARNING, "Failed to execute '%s': File does not exist.\n", script);
		return AGI_RESULT_NOTFOUND;
	}

	if (pipe(toast)) {
		tris_log(LOG_WARNING, "Unable to create toast pipe: %s\n",strerror(errno));
		return AGI_RESULT_FAILURE;
	}
	if (pipe(fromast)) {
		tris_log(LOG_WARNING, "unable to create fromast pipe: %s\n", strerror(errno));
		close(toast[0]);
		close(toast[1]);
		return AGI_RESULT_FAILURE;
	}
	if (efd) {
		if (pipe(audio)) {
			tris_log(LOG_WARNING, "unable to create audio pipe: %s\n", strerror(errno));
			close(fromast[0]);
			close(fromast[1]);
			close(toast[0]);
			close(toast[1]);
			return AGI_RESULT_FAILURE;
		}
		res = fcntl(audio[1], F_GETFL);
		if (res > -1)
			res = fcntl(audio[1], F_SETFL, res | O_NONBLOCK);
		if (res < 0) {
			tris_log(LOG_WARNING, "unable to set audio pipe parameters: %s\n", strerror(errno));
			close(fromast[0]);
			close(fromast[1]);
			close(toast[0]);
			close(toast[1]);
			close(audio[0]);
			close(audio[1]);
			return AGI_RESULT_FAILURE;
		}
	}

	if ((pid = tris_safe_fork(1)) < 0) {
		tris_log(LOG_WARNING, "Failed to fork(): %s\n", strerror(errno));
		return AGI_RESULT_FAILURE;
	}
	if (!pid) {
		/* Pass paths to AGI via environmental variables */
		setenv("TRIS_CONFIG_DIR", tris_config_TRIS_CONFIG_DIR, 1);
		setenv("TRIS_CONFIG_FILE", tris_config_TRIS_CONFIG_FILE, 1);
		setenv("TRIS_MODULE_DIR", tris_config_TRIS_MODULE_DIR, 1);
		setenv("TRIS_SPOOL_DIR", tris_config_TRIS_SPOOL_DIR, 1);
		setenv("TRIS_MONITOR_DIR", tris_config_TRIS_MONITOR_DIR, 1);
		setenv("TRIS_VAR_DIR", tris_config_TRIS_VAR_DIR, 1);
		setenv("TRIS_DATA_DIR", tris_config_TRIS_DATA_DIR, 1);
		setenv("TRIS_LOG_DIR", tris_config_TRIS_LOG_DIR, 1);
		setenv("TRIS_AGI_DIR", tris_config_TRIS_AGI_DIR, 1);
		setenv("TRIS_KEY_DIR", tris_config_TRIS_KEY_DIR, 1);
		setenv("TRIS_RUN_DIR", tris_config_TRIS_RUN_DIR, 1);

		/* Don't run AGI scripts with realtime priority -- it causes audio stutter */
		tris_set_priority(0);

		/* Redirect stdin and out, provide enhanced audio channel if desired */
		dup2(fromast[0], STDIN_FILENO);
		dup2(toast[1], STDOUT_FILENO);
		if (efd)
			dup2(audio[0], STDERR_FILENO + 1);
		else
			close(STDERR_FILENO + 1);

		/* Close everything but stdin/out/error */
		tris_close_fds_above_n(STDERR_FILENO + 1);

		/* Execute script */
		/* XXX argv should be deprecated in favor of passing agi_argX paramaters */
		execv(script, argv);
		/* Can't use tris_log since FD's are closed */
		tris_child_verbose(1, "Failed to execute '%s': %s", script, strerror(errno));
		/* Special case to set status of AGI to failure */
		fprintf(stdout, "failure\n");
		fflush(stdout);
		_exit(1);
	}
	tris_verb(3, "Launched AGI Script %s\n", script);
	fds[0] = toast[0];
	fds[1] = fromast[1];
	if (efd)
		*efd = audio[1];
	/* close what we're not using in the parent */
	close(toast[1]);
	close(fromast[0]);

	if (efd)
		close(audio[0]);

	*opid = pid;
	return AGI_RESULT_SUCCESS;
}

static void setup_env(struct tris_channel *chan, char *request, int fd, int enhanced, int argc, char *argv[])
{
	int count;

	/* Print initial environment, with agi_request always being the first
	   thing */
	tris_agi_send(fd, chan, "agi_request: %s\n", request);
	tris_agi_send(fd, chan, "agi_channel: %s\n", chan->name);
	tris_agi_send(fd, chan, "agi_language: %s\n", chan->language);
	tris_agi_send(fd, chan, "agi_type: %s\n", chan->tech->type);
	tris_agi_send(fd, chan, "agi_uniqueid: %s\n", chan->uniqueid);
	tris_agi_send(fd, chan, "agi_version: %s\n", tris_get_version());

	/* ANI/DNIS */
	tris_agi_send(fd, chan, "agi_callerid: %s\n", S_OR(chan->cid.cid_num, "unknown"));
	tris_agi_send(fd, chan, "agi_calleridname: %s\n", S_OR(chan->cid.cid_name, "unknown"));
	tris_agi_send(fd, chan, "agi_callingpres: %d\n", chan->cid.cid_pres);
	tris_agi_send(fd, chan, "agi_callingani2: %d\n", chan->cid.cid_ani2);
	tris_agi_send(fd, chan, "agi_callington: %d\n", chan->cid.cid_ton);
	tris_agi_send(fd, chan, "agi_callingtns: %d\n", chan->cid.cid_tns);
	tris_agi_send(fd, chan, "agi_dnid: %s\n", S_OR(chan->cid.cid_dnid, "unknown"));
	tris_agi_send(fd, chan, "agi_rdnis: %s\n", S_OR(chan->cid.cid_rdnis, "unknown"));

	/* Context information */
	tris_agi_send(fd, chan, "agi_context: %s\n", chan->context);
	tris_agi_send(fd, chan, "agi_extension: %s\n", chan->exten);
	tris_agi_send(fd, chan, "agi_priority: %d\n", chan->priority);
	tris_agi_send(fd, chan, "agi_enhanced: %s\n", enhanced ? "1.0" : "0.0");

	/* User information */
	tris_agi_send(fd, chan, "agi_accountcode: %s\n", chan->accountcode ? chan->accountcode : "");
	tris_agi_send(fd, chan, "agi_threadid: %ld\n", (long)pthread_self());

	/* Send any parameters to the fastagi server that have been passed via the agi application */
	/* Agi application paramaters take the form of: AGI(/path/to/example/script|${EXTEN}) */
	for(count = 1; count < argc; count++)
		tris_agi_send(fd, chan, "agi_arg_%d: %s\n", count, argv[count]);

	/* End with empty return */
	tris_agi_send(fd, chan, "\n");
}

static int handle_answer(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res = 0;

	/* Answer the channel */
	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);

	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_asyncagi_break(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	tris_agi_send(agi->fd, chan, "200 result=0\n");
	return RESULT_FAILURE;
}

static int handle_waitfordigit(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, to;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[3], "%30d", &to) != 1)
		return RESULT_SHOWUSAGE;
	res = tris_waitfordigit_full(chan, to, agi->audio, agi->ctrl);
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_sendtext(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res;

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	/* At the moment, the parser (perhaps broken) returns with
	   the last argument PLUS the newline at the end of the input
	   buffer. This probably needs to be fixed, but I wont do that
	   because other stuff may break as a result. The right way
	   would probably be to strip off the trailing newline before
	   parsing, then here, add a newline at the end of the string
	   before sending it to tris_sendtext --DUDE */
	res = tris_sendtext(chan, argv[2]);
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_recvchar(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res;

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	res = tris_recvchar(chan,atoi(argv[2]));
	if (res == 0) {
		tris_agi_send(agi->fd, chan, "200 result=%d (timeout)\n", res);
		return RESULT_SUCCESS;
	}
	if (res > 0) {
		tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
		return RESULT_SUCCESS;
	}
	tris_agi_send(agi->fd, chan, "200 result=%d (hangup)\n", res);
	return RESULT_FAILURE;
}

static int handle_recvtext(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	char *buf;

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	buf = tris_recvtext(chan, atoi(argv[2]));
	if (buf) {
		tris_agi_send(agi->fd, chan, "200 result=1 (%s)\n", buf);
		tris_free(buf);
	} else {
		tris_agi_send(agi->fd, chan, "200 result=-1\n");
	}
	return RESULT_SUCCESS;
}

static int handle_tddmode(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, x;

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	if (!strncasecmp(argv[2],"on",2)) {
		x = 1;
	} else  {
		x = 0;
	}
	if (!strncasecmp(argv[2],"mate",4))  {
		x = 2;
	}
	if (!strncasecmp(argv[2],"tdd",3)) {
		x = 1;
	}
	res = tris_channel_setoption(chan, TRIS_OPTION_TDD, &x, sizeof(char), 0);
	if (res != RESULT_SUCCESS) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	} else {
		tris_agi_send(agi->fd, chan, "200 result=1\n");
	}
	return RESULT_SUCCESS;
}

static int handle_sendimage(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res;

	if (argc != 3) {
		return RESULT_SHOWUSAGE;
	}

	res = tris_send_image(chan, argv[2]);
	if (!tris_check_hangup(chan)) {
		res = 0;
	}
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_controlstreamfile(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res = 0, skipms = 3000;
	char *fwd = "#", *rev = "*", *suspend = NULL, *stop = NULL;	/* Default values */

	if (argc < 5 || argc > 9) {
		return RESULT_SHOWUSAGE;
	}

	if (!tris_strlen_zero(argv[4])) {
		stop = argv[4];
	}

	if ((argc > 5) && (sscanf(argv[5], "%30d", &skipms) != 1)) {
		return RESULT_SHOWUSAGE;
	}

	if (argc > 6 && !tris_strlen_zero(argv[6])) {
		fwd = argv[6];
	}

	if (argc > 7 && !tris_strlen_zero(argv[7])) {
		rev = argv[7];
	}

	if (argc > 8 && !tris_strlen_zero(argv[8])) {
		suspend = argv[8];
	}

	res = tris_control_streamfile(chan, argv[3], fwd, rev, stop, suspend, NULL, skipms, NULL);

	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);

	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_streamfile(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, vres;
	struct tris_filestream *fs, *vfs;
	long sample_offset = 0, max_length;
	char *edigits = "";

	if (argc < 4 || argc > 5)
		return RESULT_SHOWUSAGE;

	if (argv[3])
		edigits = argv[3];

	if ((argc > 4) && (sscanf(argv[4], "%30ld", &sample_offset) != 1))
		return RESULT_SHOWUSAGE;

	if (!(fs = tris_openstream(chan, argv[2], chan->language))) {
		tris_agi_send(agi->fd, chan, "200 result=%d endpos=%ld\n", 0, sample_offset);
		return RESULT_SUCCESS;
	}

	if ((vfs = tris_openvstream(chan, argv[2], chan->language)))
		tris_debug(1, "Ooh, found a video stream, too\n");

	tris_verb(3, "Playing '%s' (escape_digits=%s) (sample_offset %ld)\n", argv[2], edigits, sample_offset);

	tris_seekstream(fs, 0, SEEK_END);
	max_length = tris_tellstream(fs);
	tris_seekstream(fs, sample_offset, SEEK_SET);
	res = tris_applystream(chan, fs);
	if (vfs)
		vres = tris_applystream(chan, vfs);
	tris_playstream(fs);
	if (vfs)
		tris_playstream(vfs);

	res = tris_waitstream_full(chan, argv[3], agi->audio, agi->ctrl);
	/* this is to check for if tris_waitstream closed the stream, we probably are at
	 * the end of the stream, return that amount, else check for the amount */
	sample_offset = (chan->stream) ? tris_tellstream(fs) : max_length;
	tris_stopstream(chan);
	if (res == 1) {
		/* Stop this command, don't print a result line, as there is a new command */
		return RESULT_SUCCESS;
	}
	tris_agi_send(agi->fd, chan, "200 result=%d endpos=%ld\n", res, sample_offset);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

/*! \brief get option - really similar to the handle_streamfile, but with a timeout */
static int handle_getoption(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, vres;
	struct tris_filestream *fs, *vfs;
	long sample_offset = 0, max_length;
	int timeout = 0;
	char *edigits = "";

	if ( argc < 4 || argc > 5 )
		return RESULT_SHOWUSAGE;

	if ( argv[3] )
		edigits = argv[3];

	if ( argc == 5 )
		timeout = atoi(argv[4]);
	else if (chan->pbx->dtimeoutms) {
		/* by default dtimeout is set to 5sec */
		timeout = chan->pbx->dtimeoutms; /* in msec */
	}

	if (!(fs = tris_openstream(chan, argv[2], chan->language))) {
		tris_agi_send(agi->fd, chan, "200 result=%d endpos=%ld\n", 0, sample_offset);
		tris_log(LOG_WARNING, "Unable to open %s\n", argv[2]);
		return RESULT_SUCCESS;
	}

	if ((vfs = tris_openvstream(chan, argv[2], chan->language)))
		tris_debug(1, "Ooh, found a video stream, too\n");

	tris_verb(3, "Playing '%s' (escape_digits=%s) (timeout %d)\n", argv[2], edigits, timeout);

	tris_seekstream(fs, 0, SEEK_END);
	max_length = tris_tellstream(fs);
	tris_seekstream(fs, sample_offset, SEEK_SET);
	res = tris_applystream(chan, fs);
	if (vfs)
		vres = tris_applystream(chan, vfs);
	tris_playstream(fs);
	if (vfs)
		tris_playstream(vfs);

	res = tris_waitstream_full(chan, argv[3], agi->audio, agi->ctrl);
	/* this is to check for if tris_waitstream closed the stream, we probably are at
	 * the end of the stream, return that amount, else check for the amount */
	sample_offset = (chan->stream)?tris_tellstream(fs):max_length;
	tris_stopstream(chan);
	if (res == 1) {
		/* Stop this command, don't print a result line, as there is a new command */
		return RESULT_SUCCESS;
	}

	/* If the user didnt press a key, wait for digitTimeout*/
	if (res == 0 ) {
		res = tris_waitfordigit_full(chan, timeout, agi->audio, agi->ctrl);
		/* Make sure the new result is in the escape digits of the GET OPTION */
		if ( !strchr(edigits,res) )
			res=0;
	}

	tris_agi_send(agi->fd, chan, "200 result=%d endpos=%ld\n", res, sample_offset);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}




/*! \brief Say number in various language syntaxes */
/* While waiting, we're sending a NULL.  */
static int handle_saynumber(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, num;

	if (argc < 4 || argc > 5)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%30d", &num) != 1)
		return RESULT_SHOWUSAGE;
	res = tris_say_number_full(chan, num, argv[3], chan->language, argc > 4 ? argv[4] : NULL, agi->audio, agi->ctrl);
	if (res == 1)
		return RESULT_SUCCESS;
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_saydigits(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, num;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%30d", &num) != 1)
		return RESULT_SHOWUSAGE;

	res = tris_say_digit_str_full(chan, argv[2], argv[3], chan->language, agi->audio, agi->ctrl);
	if (res == 1) /* New command */
		return RESULT_SUCCESS;
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_sayalpha(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res;

	if (argc != 4)
		return RESULT_SHOWUSAGE;

	res = tris_say_character_str_full(chan, argv[2], argv[3], chan->language, agi->audio, agi->ctrl);
	if (res == 1) /* New command */
		return RESULT_SUCCESS;
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_saydate(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, num;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%30d", &num) != 1)
		return RESULT_SHOWUSAGE;
	res = tris_say_date(chan, num, argv[3], chan->language);
	if (res == 1)
		return RESULT_SUCCESS;
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_saytime(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, num;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%30d", &num) != 1)
		return RESULT_SHOWUSAGE;
	res = tris_say_time(chan, num, argv[3], chan->language);
	if (res == 1)
		return RESULT_SUCCESS;
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_saydatetime(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res = 0;
	time_t unixtime;
	char *format, *zone = NULL;

	if (argc < 4)
		return RESULT_SHOWUSAGE;

	if (argc > 4) {
		format = argv[4];
	} else {
		/* XXX this doesn't belong here, but in the 'say' module */
		if (!strcasecmp(chan->language, "de")) {
			format = "A dBY HMS";
		} else {
			format = "ABdY 'digits/at' IMp";
		}
	}

	if (argc > 5 && !tris_strlen_zero(argv[5]))
		zone = argv[5];

	if (tris_get_time_t(argv[2], &unixtime, 0, NULL))
		return RESULT_SHOWUSAGE;

	res = tris_say_date_with_format(chan, unixtime, argv[3], chan->language, format, zone);
	if (res == 1)
		return RESULT_SUCCESS;

	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_sayphonetic(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res;

	if (argc != 4)
		return RESULT_SHOWUSAGE;

	res = tris_say_phonetic_str_full(chan, argv[2], argv[3], chan->language, agi->audio, agi->ctrl);
	if (res == 1) /* New command */
		return RESULT_SUCCESS;
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);
	return (res >= 0) ? RESULT_SUCCESS : RESULT_FAILURE;
}

static int handle_getdata(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	int res, max, timeout;
	char data[1024];

	if (argc < 3)
		return RESULT_SHOWUSAGE;
	if (argc >= 4)
		timeout = atoi(argv[3]);
	else
		timeout = 0;
	if (argc >= 5)
		max = atoi(argv[4]);
	else
		max = 1024;
	res = tris_app_getdata_full(chan, argv[2], data, max, timeout, agi->audio, agi->ctrl);
	if (res == 2)			/* New command */
		return RESULT_SUCCESS;
	else if (res == 1)
		tris_agi_send(agi->fd, chan, "200 result=%s (timeout)\n", data);
	else if (res < 0 )
		tris_agi_send(agi->fd, chan, "200 result=-1\n");
	else
		tris_agi_send(agi->fd, chan, "200 result=%s\n", data);
	return RESULT_SUCCESS;
}

static int handle_setcontext(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	tris_copy_string(chan->context, argv[2], sizeof(chan->context));
	tris_agi_send(agi->fd, chan, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_setextension(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	tris_copy_string(chan->exten, argv[2], sizeof(chan->exten));
	tris_agi_send(agi->fd, chan, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_setpriority(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int pri;

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	if (sscanf(argv[2], "%30d", &pri) != 1) {
		if ((pri = tris_findlabel_extension(chan, chan->context, chan->exten, argv[2], chan->cid.cid_num)) < 1)
			return RESULT_SHOWUSAGE;
	}

	tris_explicit_goto(chan, NULL, NULL, pri);
	tris_agi_send(agi->fd, chan, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_recordfile(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	struct tris_filestream *fs;
	struct tris_frame *f;
	struct timeval start;
	long sample_offset = 0;
	int res = 0;
	int ms;

	struct tris_dsp *sildet=NULL;         /* silence detector dsp */
	int totalsilence = 0;
	int dspsilence = 0;
	int silence = 0;                /* amount of silence to allow */
	int gotsilence = 0;             /* did we timeout for silence? */
	char *silencestr = NULL;
	int rfmt = 0;

	/* XXX EAGI FIXME XXX */

	if (argc < 6)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[5], "%30d", &ms) != 1)
		return RESULT_SHOWUSAGE;

	if (argc > 6)
		silencestr = strchr(argv[6],'s');
	if ((argc > 7) && (!silencestr))
		silencestr = strchr(argv[7],'s');
	if ((argc > 8) && (!silencestr))
		silencestr = strchr(argv[8],'s');

	if (silencestr) {
		if (strlen(silencestr) > 2) {
			if ((silencestr[0] == 's') && (silencestr[1] == '=')) {
				silencestr++;
				silencestr++;
				if (silencestr)
					silence = atoi(silencestr);
				if (silence > 0)
					silence *= 1000;
			}
		}
	}

	if (silence > 0) {
		rfmt = chan->readformat;
		res = tris_set_read_format(chan, TRIS_FORMAT_SLINEAR);
		if (res < 0) {
			tris_log(LOG_WARNING, "Unable to set to linear mode, giving up\n");
			return -1;
		}
		sildet = tris_dsp_new();
		if (!sildet) {
			tris_log(LOG_WARNING, "Unable to create silence detector :(\n");
			return -1;
		}
		tris_dsp_set_threshold(sildet, tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE));
	}
	
	/* backward compatibility, if no offset given, arg[6] would have been
	 * caught below and taken to be a beep, else if it is a digit then it is a
	 * offset */
	if ((argc >6) && (sscanf(argv[6], "%30ld", &sample_offset) != 1) && (!strchr(argv[6], '=')))
		res = tris_streamfile(chan, "beep", chan->language);

	if ((argc > 7) && (!strchr(argv[7], '=')))
		res = tris_streamfile(chan, "beep", chan->language);

	if (!res)
		res = tris_waitstream(chan, argv[4]);
	if (res) {
		tris_agi_send(agi->fd, chan, "200 result=%d (randomerror) endpos=%ld\n", res, sample_offset);
	} else {
		fs = tris_writefile(argv[2], argv[3], NULL, O_CREAT | O_WRONLY | (sample_offset ? O_APPEND : 0), 0, TRIS_FILE_MODE);
		if (!fs) {
			res = -1;
			tris_agi_send(agi->fd, chan, "200 result=%d (writefile)\n", res);
			if (sildet)
				tris_dsp_free(sildet);
			return RESULT_FAILURE;
		}

		/* Request a video update */
		tris_indicate(chan, TRIS_CONTROL_VIDUPDATE);

		chan->stream = fs;
		tris_applystream(chan,fs);
		/* really should have checks */
		tris_seekstream(fs, sample_offset, SEEK_SET);
		tris_truncstream(fs);

		start = tris_tvnow();
		while ((ms < 0) || tris_tvdiff_ms(tris_tvnow(), start) < ms) {
			res = tris_waitfor(chan, ms - tris_tvdiff_ms(tris_tvnow(), start));
			if (res < 0) {
				tris_closestream(fs);
				tris_agi_send(agi->fd, chan, "200 result=%d (waitfor) endpos=%ld\n", res,sample_offset);
				if (sildet)
					tris_dsp_free(sildet);
				return RESULT_FAILURE;
			}
			f = tris_read(chan);
			if (!f) {
				tris_agi_send(agi->fd, chan, "200 result=%d (hangup) endpos=%ld\n", -1, sample_offset);
				tris_closestream(fs);
				if (sildet)
					tris_dsp_free(sildet);
				return RESULT_FAILURE;
			}
			switch(f->frametype) {
			case TRIS_FRAME_DTMF:
				if (strchr(argv[4], f->subclass)) {
					/* This is an interrupting chracter, so rewind to chop off any small
					   amount of DTMF that may have been recorded
					*/
					tris_stream_rewind(fs, 200);
					tris_truncstream(fs);
					sample_offset = tris_tellstream(fs);
					tris_agi_send(agi->fd, chan, "200 result=%d (dtmf) endpos=%ld\n", f->subclass, sample_offset);
					tris_closestream(fs);
					tris_frfree(f);
					if (sildet)
						tris_dsp_free(sildet);
					return RESULT_SUCCESS;
				}
				break;
			case TRIS_FRAME_VOICE:
				tris_writestream(fs, f);
				/* this is a safe place to check progress since we know that fs
				 * is valid after a write, and it will then have our current
				 * location */
				sample_offset = tris_tellstream(fs);
				if (silence > 0) {
					dspsilence = 0;
					tris_dsp_silence(sildet, f, &dspsilence);
					if (dspsilence) {
						totalsilence = dspsilence;
					} else {
						totalsilence = 0;
					}
					if (totalsilence > silence) {
						/* Ended happily with silence */
						gotsilence = 1;
						break;
					}
				}
				break;
			case TRIS_FRAME_VIDEO:
				tris_writestream(fs, f);
			default:
				/* Ignore all other frames */
				break;
			}
			tris_frfree(f);
			if (gotsilence)
				break;
		}

		if (gotsilence) {
			tris_stream_rewind(fs, silence-1000);
			tris_truncstream(fs);
			sample_offset = tris_tellstream(fs);
		}
		tris_agi_send(agi->fd, chan, "200 result=%d (timeout) endpos=%ld\n", res, sample_offset);
		tris_closestream(fs);
	}

	if (silence > 0) {
		res = tris_set_read_format(chan, rfmt);
		if (res)
			tris_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
		tris_dsp_free(sildet);
	}

	return RESULT_SUCCESS;
}

static int handle_autohangup(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	double timeout;
	struct timeval whentohangup = { 0, 0 };

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%30lf", &timeout) != 1)
		return RESULT_SHOWUSAGE;
	if (timeout < 0)
		timeout = 0;
	if (timeout) {
		whentohangup.tv_sec = timeout;
		whentohangup.tv_usec = (timeout - whentohangup.tv_sec) * 1000000.0;
	}
	tris_channel_setwhentohangup_tv(chan, whentohangup);
	tris_agi_send(agi->fd, chan, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_hangup(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	struct tris_channel *c;

	if (argc == 1) {
		/* no argument: hangup the current channel */
		tris_softhangup(chan,TRIS_SOFTHANGUP_EXPLICIT);
		tris_agi_send(agi->fd, chan, "200 result=1\n");
		return RESULT_SUCCESS;
	} else if (argc == 2) {
		/* one argument: look for info on the specified channel */
		c = tris_get_channel_by_name_locked(argv[1]);
		if (c) {
			/* we have a matching channel */
			tris_softhangup(c,TRIS_SOFTHANGUP_EXPLICIT);
			tris_agi_send(agi->fd, chan, "200 result=1\n");
			tris_channel_unlock(c);
			return RESULT_SUCCESS;
		}
		/* if we get this far no channel name matched the argument given */
		tris_agi_send(agi->fd, chan, "200 result=-1\n");
		return RESULT_SUCCESS;
	} else {
		return RESULT_SHOWUSAGE;
	}
}

static int handle_exec(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int res, workaround;
	struct tris_app *app_to_exec;

	if (argc < 2)
		return RESULT_SHOWUSAGE;

	tris_verb(3, "AGI Script Executing Application: (%s) Options: (%s)\n", argv[1], argc >= 3 ? argv[2] : "");

	if ((app_to_exec = pbx_findapp(argv[1]))) {
		if(!strcasecmp(argv[1], PARK_APP_NAME)) {
			tris_masq_park_call(chan, NULL, 0, NULL);
		}
		if (!(workaround = tris_test_flag(chan, TRIS_FLAG_DISABLE_WORKAROUNDS))) {
			tris_set_flag(chan, TRIS_FLAG_DISABLE_WORKAROUNDS);
		}
		if (tris_compat_res_agi && argc >= 3 && !tris_strlen_zero(argv[2])) {
			char *compat = alloca(strlen(argv[2]) * 2 + 1), *cptr, *vptr;
			for (cptr = compat, vptr = argv[2]; *vptr; vptr++) {
				if (*vptr == ',') {
					*cptr++ = '\\';
					*cptr++ = ',';
				} else if (*vptr == '|') {
					*cptr++ = ',';
				} else {
					*cptr++ = *vptr;
				}
			}
			*cptr = '\0';
			res = pbx_exec(chan, app_to_exec, compat);
		} else {
			res = pbx_exec(chan, app_to_exec, argc == 2 ? "" : argv[2]);
		}
		if (!workaround) {
			tris_clear_flag(chan, TRIS_FLAG_DISABLE_WORKAROUNDS);
		}
	} else {
		tris_log(LOG_WARNING, "Could not find application (%s)\n", argv[1]);
		res = -2;
	}
	tris_agi_send(agi->fd, chan, "200 result=%d\n", res);

	/* Even though this is wrong, users are depending upon this result. */
	return res;
}

static int handle_setcallerid(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	char tmp[256]="";
	char *l = NULL, *n = NULL;

	if (argv[2]) {
		tris_copy_string(tmp, argv[2], sizeof(tmp));
		tris_callerid_parse(tmp, &n, &l);
		if (l)
			tris_shrink_phone_number(l);
		else
			l = "";
		if (!n)
			n = "";
		tris_set_callerid(chan, l, n, NULL);
	}

	tris_agi_send(agi->fd, chan, "200 result=1\n");
	return RESULT_SUCCESS;
}

static int handle_channelstatus(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	struct tris_channel *c;
	if (argc == 2) {
		/* no argument: supply info on the current channel */
		tris_agi_send(agi->fd, chan, "200 result=%d\n", chan->_state);
		return RESULT_SUCCESS;
	} else if (argc == 3) {
		/* one argument: look for info on the specified channel */
		c = tris_get_channel_by_name_locked(argv[2]);
		if (c) {
			tris_agi_send(agi->fd, chan, "200 result=%d\n", c->_state);
			tris_channel_unlock(c);
			return RESULT_SUCCESS;
		}
		/* if we get this far no channel name matched the argument given */
		tris_agi_send(agi->fd, chan, "200 result=-1\n");
		return RESULT_SUCCESS;
	} else {
		return RESULT_SHOWUSAGE;
	}
}

static int handle_setvariable(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	if (argv[3])
		pbx_builtin_setvar_helper(chan, argv[2], argv[3]);

	tris_agi_send(agi->fd, chan, "200 result=1\n");
	return RESULT_SUCCESS;
}

static int handle_getvariable(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	char *ret;
	char tempstr[1024];

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	/* check if we want to execute an tris_custom_function */
	if (!tris_strlen_zero(argv[2]) && (argv[2][strlen(argv[2]) - 1] == ')')) {
		ret = tris_func_read(chan, argv[2], tempstr, sizeof(tempstr)) ? NULL : tempstr;
	} else {
		pbx_retrieve_variable(chan, argv[2], &ret, tempstr, sizeof(tempstr), NULL);
	}

	if (ret)
		tris_agi_send(agi->fd, chan, "200 result=1 (%s)\n", ret);
	else
		tris_agi_send(agi->fd, chan, "200 result=0\n");

	return RESULT_SUCCESS;
}

static int handle_getvariablefull(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	char tmp[4096];
	struct tris_channel *chan2=NULL;

	if ((argc != 4) && (argc != 5))
		return RESULT_SHOWUSAGE;
	if (argc == 5) {
		chan2 = tris_get_channel_by_name_locked(argv[4]);
	} else {
		chan2 = chan;
	}
	if (chan2) {
		pbx_substitute_variables_helper(chan2, argv[3], tmp, sizeof(tmp) - 1);
		tris_agi_send(agi->fd, chan, "200 result=1 (%s)\n", tmp);
	} else {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	}
	if (chan2 && (chan2 != chan))
		tris_channel_unlock(chan2);
	return RESULT_SUCCESS;
}

static int handle_verbose(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int level = 0;

	if (argc < 2)
		return RESULT_SHOWUSAGE;

	if (argv[2])
		sscanf(argv[2], "%30d", &level);

	tris_verb(level, "%s: %s\n", chan->data, argv[1]);

	tris_agi_send(agi->fd, chan, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int handle_dbget(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int res;
	struct tris_str *buf;

	if (argc != 4)
		return RESULT_SHOWUSAGE;

	if (!(buf = tris_str_create(16))) {
		tris_agi_send(agi->fd, chan, "200 result=-1\n");
		return RESULT_SUCCESS;
	}

	do {
		res = tris_db_get(argv[2], argv[3], tris_str_buffer(buf), tris_str_size(buf));
		tris_str_update(buf);
		if (tris_str_strlen(buf) < tris_str_size(buf) - 1) {
			break;
		}
		if (tris_str_make_space(&buf, tris_str_size(buf) * 2)) {
			break;
		}
	} while (1);
	
	if (res)
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	else
		tris_agi_send(agi->fd, chan, "200 result=1 (%s)\n", tris_str_buffer(buf));

	tris_free(buf);
	return RESULT_SUCCESS;
}

static int handle_dbput(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int res;

	if (argc != 5)
		return RESULT_SHOWUSAGE;
	res = tris_db_put(argv[2], argv[3], argv[4]);
	tris_agi_send(agi->fd, chan, "200 result=%c\n", res ? '0' : '1');
	return RESULT_SUCCESS;
}

static int handle_dbdel(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int res;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	res = tris_db_del(argv[2], argv[3]);
	tris_agi_send(agi->fd, chan, "200 result=%c\n", res ? '0' : '1');
	return RESULT_SUCCESS;
}

static int handle_dbdeltree(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	int res;

	if ((argc < 3) || (argc > 4))
		return RESULT_SHOWUSAGE;
	if (argc == 4)
		res = tris_db_deltree(argv[2], argv[3]);
	else
		res = tris_db_deltree(argv[2], NULL);

	tris_agi_send(agi->fd, chan, "200 result=%c\n", res ? '0' : '1');
	return RESULT_SUCCESS;
}

static char *handle_cli_agi_debug(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "agi set debug [on|off]";
		e->usage =
			"Usage: agi set debug [on|off]\n"
			"       Enables/disables dumping of AGI transactions for\n"
			"       debugging purposes.\n";
		return NULL;

	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	if (strncasecmp(a->argv[3], "off", 3) == 0) {
		agidebug = 0;
	} else if (strncasecmp(a->argv[3], "on", 2) == 0) {
		agidebug = 1;
	} else {
		return CLI_SHOWUSAGE;
	}
	tris_cli(a->fd, "AGI Debugging %sabled\n", agidebug ? "En" : "Dis");
	return CLI_SUCCESS;
}

static int handle_noop(struct tris_channel *chan, AGI *agi, int arg, char *argv[])
{
	tris_agi_send(agi->fd, chan, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_setmusic(struct tris_channel *chan, AGI *agi, int argc, char *argv[])
{
	if (argc < 3) {
		return RESULT_SHOWUSAGE;
	}
	if (!strncasecmp(argv[2], "on", 2))
		tris_moh_start(chan, argc > 3 ? argv[3] : NULL, NULL);
	else if (!strncasecmp(argv[2], "off", 3))
		tris_moh_stop(chan);
	tris_agi_send(agi->fd, chan, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_speechcreate(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	/* If a structure already exists, return an error */
        if (agi->speech) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	if ((agi->speech = tris_speech_new(argv[2], TRIS_FORMAT_SLINEAR)))
		tris_agi_send(agi->fd, chan, "200 result=1\n");
	else
		tris_agi_send(agi->fd, chan, "200 result=0\n");

	return RESULT_SUCCESS;
}

static int handle_speechset(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	/* Check for minimum arguments */
        if (argc != 3)
		return RESULT_SHOWUSAGE;

	/* Check to make sure speech structure exists */
	if (!agi->speech) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	tris_speech_change(agi->speech, argv[2], argv[3]);
	tris_agi_send(agi->fd, chan, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int handle_speechdestroy(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	if (agi->speech) {
		tris_speech_destroy(agi->speech);
		agi->speech = NULL;
		tris_agi_send(agi->fd, chan, "200 result=1\n");
	} else {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	}

	return RESULT_SUCCESS;
}

static int handle_speechloadgrammar(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	if (argc != 5)
		return RESULT_SHOWUSAGE;

	if (!agi->speech) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	if (tris_speech_grammar_load(agi->speech, argv[3], argv[4]))
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	else
		tris_agi_send(agi->fd, chan, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int handle_speechunloadgrammar(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	if (argc != 4)
		return RESULT_SHOWUSAGE;

	if (!agi->speech) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	if (tris_speech_grammar_unload(agi->speech, argv[3]))
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	else
		tris_agi_send(agi->fd, chan, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int handle_speechactivategrammar(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	if (argc != 4)
		return RESULT_SHOWUSAGE;

	if (!agi->speech) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	if (tris_speech_grammar_activate(agi->speech, argv[3]))
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	else
		tris_agi_send(agi->fd, chan, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int handle_speechdeactivategrammar(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	if (argc != 4)
		return RESULT_SHOWUSAGE;

	if (!agi->speech) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	if (tris_speech_grammar_deactivate(agi->speech, argv[3]))
		tris_agi_send(agi->fd, chan, "200 result=0\n");
	else
		tris_agi_send(agi->fd, chan, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int speech_streamfile(struct tris_channel *chan, const char *filename, const char *preflang, int offset)
{
	struct tris_filestream *fs = NULL;

	if (!(fs = tris_openstream(chan, filename, preflang)))
		return -1;

	if (offset)
		tris_seekstream(fs, offset, SEEK_SET);

	if (tris_applystream(chan, fs))
		return -1;

	if (tris_playstream(fs))
		return -1;

	return 0;
}

static int handle_speechrecognize(struct tris_channel *chan, AGI *agi, int argc, char **argv)
{
	struct tris_speech *speech = agi->speech;
	char *prompt, dtmf = 0, tmp[4096] = "", *buf = tmp;
	int timeout = 0, offset = 0, old_read_format = 0, res = 0, i = 0;
	long current_offset = 0;
	const char *reason = NULL;
	struct tris_frame *fr = NULL;
	struct tris_speech_result *result = NULL;
	size_t left = sizeof(tmp);
	time_t start = 0, current;

	if (argc < 4)
		return RESULT_SHOWUSAGE;

	if (!speech) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	prompt = argv[2];
	timeout = atoi(argv[3]);

	/* If offset is specified then convert from text to integer */
	if (argc == 5)
		offset = atoi(argv[4]);

	/* We want frames coming in signed linear */
	old_read_format = chan->readformat;
	if (tris_set_read_format(chan, TRIS_FORMAT_SLINEAR)) {
		tris_agi_send(agi->fd, chan, "200 result=0\n");
		return RESULT_SUCCESS;
	}

	/* Setup speech structure */
	if (speech->state == TRIS_SPEECH_STATE_NOT_READY || speech->state == TRIS_SPEECH_STATE_DONE) {
		tris_speech_change_state(speech, TRIS_SPEECH_STATE_NOT_READY);
		tris_speech_start(speech);
	}

	/* Start playing prompt */
	speech_streamfile(chan, prompt, chan->language, offset);

	/* Go into loop reading in frames, passing to speech thingy, checking for hangup, all that jazz */
	while (tris_strlen_zero(reason)) {
		/* Run scheduled items */
                tris_sched_runq(chan->sched);

		/* See maximum time of waiting */
		if ((res = tris_sched_wait(chan->sched)) < 0)
			res = 1000;

		/* Wait for frame */
		if (tris_waitfor(chan, res) > 0) {
			if (!(fr = tris_read(chan))) {
				reason = "hangup";
				break;
			}
		}

		/* Perform timeout check */
		if ((timeout > 0) && (start > 0)) {
			time(&current);
			if ((current - start) >= timeout) {
				reason = "timeout";
				if (fr)
					tris_frfree(fr);
				break;
			}
		}

		/* Check the speech structure for any changes */
		tris_mutex_lock(&speech->lock);

		/* See if we need to quiet the audio stream playback */
		if (tris_test_flag(speech, TRIS_SPEECH_QUIET) && chan->stream) {
			current_offset = tris_tellstream(chan->stream);
			tris_stopstream(chan);
			tris_clear_flag(speech, TRIS_SPEECH_QUIET);
		}

		/* Check each state */
		switch (speech->state) {
		case TRIS_SPEECH_STATE_READY:
			/* If the stream is done, start timeout calculation */
			if ((timeout > 0) && start == 0 && ((!chan->stream) || (chan->streamid == -1 && chan->timingfunc == NULL))) {
				tris_stopstream(chan);
				time(&start);
			}
			/* Write audio frame data into speech engine if possible */
			if (fr && fr->frametype == TRIS_FRAME_VOICE)
				tris_speech_write(speech, fr->data.ptr, fr->datalen);
			break;
		case TRIS_SPEECH_STATE_WAIT:
			/* Cue waiting sound if not already playing */
			if ((!chan->stream) || (chan->streamid == -1 && chan->timingfunc == NULL)) {
				tris_stopstream(chan);
				/* If a processing sound exists, or is not none - play it */
				if (!tris_strlen_zero(speech->processing_sound) && strcasecmp(speech->processing_sound, "none"))
					speech_streamfile(chan, speech->processing_sound, chan->language, 0);
			}
			break;
		case TRIS_SPEECH_STATE_DONE:
			/* Get the results */
			speech->results = tris_speech_results_get(speech);
			/* Change state to not ready */
			tris_speech_change_state(speech, TRIS_SPEECH_STATE_NOT_READY);
			reason = "speech";
			break;
		default:
			break;
		}
		tris_mutex_unlock(&speech->lock);

		/* Check frame for DTMF or hangup */
		if (fr) {
			if (fr->frametype == TRIS_FRAME_DTMF) {
				reason = "dtmf";
				dtmf = fr->subclass;
			} else if (fr->frametype == TRIS_FRAME_CONTROL && fr->subclass == TRIS_CONTROL_HANGUP) {
				reason = "hangup";
			}
			tris_frfree(fr);
		}
	}

	if (!strcasecmp(reason, "speech")) {
		/* Build string containing speech results */
                for (result = speech->results; result; result = TRIS_LIST_NEXT(result, list)) {
			/* Build result string */
			tris_build_string(&buf, &left, "%sscore%d=%d text%d=\"%s\" grammar%d=%s", (i > 0 ? " " : ""), i, result->score, i, result->text, i, result->grammar);
                        /* Increment result count */
			i++;
		}
                /* Print out */
		tris_agi_send(agi->fd, chan, "200 result=1 (speech) endpos=%ld results=%d %s\n", current_offset, i, tmp);
	} else if (!strcasecmp(reason, "dtmf")) {
		tris_agi_send(agi->fd, chan, "200 result=1 (digit) digit=%c endpos=%ld\n", dtmf, current_offset);
	} else if (!strcasecmp(reason, "hangup") || !strcasecmp(reason, "timeout")) {
		tris_agi_send(agi->fd, chan, "200 result=1 (%s) endpos=%ld\n", reason, current_offset);
	} else {
		tris_agi_send(agi->fd, chan, "200 result=0 endpos=%ld\n", current_offset);
	}

	return RESULT_SUCCESS;
}

static char usage_verbose[] =
" Usage: VERBOSE <message> <level>\n"
"	Sends <message> to the console via verbose message system.\n"
" <level> is the the verbose level (1-4)\n"
" Always returns 1.\n";

static char usage_setvariable[] =
" Usage: SET VARIABLE <variablename> <value>\n";

static char usage_setcallerid[] =
" Usage: SET CALLERID <number>\n"
"	Changes the callerid of the current channel.\n";

static char usage_waitfordigit[] =
" Usage: WAIT FOR DIGIT <timeout>\n"
"	Waits up to 'timeout' milliseconds for channel to receive a DTMF digit.\n"
" Returns -1 on channel failure, 0 if no digit is received in the timeout, or\n"
" the numerical value of the ascii of the digit if one is received.  Use -1\n"
" for the timeout value if you desire the call to block indefinitely.\n";

static char usage_sendtext[] =
" Usage: SEND TEXT \"<text to send>\"\n"
"	Sends the given text on a channel. Most channels do not support the\n"
" transmission of text.  Returns 0 if text is sent, or if the channel does not\n"
" support text transmission.  Returns -1 only on error/hangup.  Text\n"
" consisting of greater than one word should be placed in quotes since the\n"
" command only accepts a single argument.\n";

static char usage_recvchar[] =
" Usage: RECEIVE CHAR <timeout>\n"
"	Receives a character of text on a channel. Specify timeout to be the\n"
" maximum time to wait for input in milliseconds, or 0 for infinite. Most channels\n"
" do not support the reception of text. Returns the decimal value of the character\n"
" if one is received, or 0 if the channel does not support text reception.  Returns\n"
" -1 only on error/hangup.\n";

static char usage_recvtext[] =
" Usage: RECEIVE TEXT <timeout>\n"
"	Receives a string of text on a channel. Specify timeout to be the\n"
" maximum time to wait for input in milliseconds, or 0 for infinite. Most channels\n"
" do not support the reception of text. Returns -1 for failure or 1 for success, and the string in parentheses.\n";

static char usage_tddmode[] =
" Usage: TDD MODE <on|off>\n"
"	Enable/Disable TDD transmission/reception on a channel. Returns 1 if\n"
" successful, or 0 if channel is not TDD-capable.\n";

static char usage_sendimage[] =
" Usage: SEND IMAGE <image>\n"
"	Sends the given image on a channel. Most channels do not support the\n"
" transmission of images. Returns 0 if image is sent, or if the channel does not\n"
" support image transmission.  Returns -1 only on error/hangup. Image names\n"
" should not include extensions.\n";

static char usage_streamfile[] =
" Usage: STREAM FILE <filename> <escape digits> [sample offset]\n"
"	Send the given file, allowing playback to be interrupted by the given\n"
" digits, if any. Use double quotes for the digits if you wish none to be\n"
" permitted. If sample offset is provided then the audio will seek to sample\n"
" offset before play starts.  Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed,\n"
" or -1 on error or if the channel was disconnected. Remember, the file\n"
" extension must not be included in the filename.\n";

static char usage_controlstreamfile[] =
" Usage: CONTROL STREAM FILE <filename> <escape digits> [skipms] [ffchar] [rewchr] [pausechr]\n"
"	Send the given file, allowing playback to be controled by the given\n"
" digits, if any. Use double quotes for the digits if you wish none to be\n"
" permitted.  Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed,\n"
" or -1 on error or if the channel was disconnected. Remember, the file\n"
" extension must not be included in the filename.\n\n"
" Note: ffchar and rewchar default to * and # respectively.\n";

static char usage_saynumber[] =
" Usage: SAY NUMBER <number> <escape digits> [gender]\n"
"	Say a given number, returning early if any of the given DTMF digits\n"
" are received on the channel.  Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed or\n"
" -1 on error/hangup.\n";

static char usage_saydigits[] =
" Usage: SAY DIGITS <number> <escape digits>\n"
"	Say a given digit string, returning early if any of the given DTMF digits\n"
" are received on the channel. Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed or\n"
" -1 on error/hangup.\n";

static char usage_sayalpha[] =
" Usage: SAY ALPHA <number> <escape digits>\n"
"	Say a given character string, returning early if any of the given DTMF digits\n"
" are received on the channel. Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed or\n"
" -1 on error/hangup.\n";

static char usage_saydate[] =
" Usage: SAY DATE <date> <escape digits>\n"
"	Say a given date, returning early if any of the given DTMF digits are\n"
" received on the channel.  <date> is number of seconds elapsed since 00:00:00\n"
" on January 1, 1970, Coordinated Universal Time (UTC). Returns 0 if playback\n"
" completes without a digit being pressed, or the ASCII numerical value of the\n"
" digit if one was pressed or -1 on error/hangup.\n";

static char usage_saytime[] =
" Usage: SAY TIME <time> <escape digits>\n"
"	Say a given time, returning early if any of the given DTMF digits are\n"
" received on the channel.  <time> is number of seconds elapsed since 00:00:00\n"
" on January 1, 1970, Coordinated Universal Time (UTC). Returns 0 if playback\n"
" completes without a digit being pressed, or the ASCII numerical value of the\n"
" digit if one was pressed or -1 on error/hangup.\n";

static char usage_saydatetime[] =
" Usage: SAY DATETIME <time> <escape digits> [format] [timezone]\n"
"	Say a given time, returning early if any of the given DTMF digits are\n"
" received on the channel.  <time> is number of seconds elapsed since 00:00:00\n"
" on January 1, 1970, Coordinated Universal Time (UTC). [format] is the format\n"
" the time should be said in.  See voicemail.conf (defaults to \"ABdY\n"
" 'digits/at' IMp\").  Acceptable values for [timezone] can be found in\n"
" /usr/share/zoneinfo.  Defaults to machine default. Returns 0 if playback\n"
" completes without a digit being pressed, or the ASCII numerical value of the\n"
" digit if one was pressed or -1 on error/hangup.\n";

static char usage_sayphonetic[] =
" Usage: SAY PHONETIC <string> <escape digits>\n"
"	Say a given character string with phonetics, returning early if any of the\n"
" given DTMF digits are received on the channel. Returns 0 if playback\n"
" completes without a digit pressed, the ASCII numerical value of the digit\n"
" if one was pressed, or -1 on error/hangup.\n";

static char usage_setcontext[] =
" Usage: SET CONTEXT <desired context>\n"
"	Sets the context for continuation upon exiting the application.\n";

static char usage_setextension[] =
" Usage: SET EXTENSION <new extension>\n"
"	Changes the extension for continuation upon exiting the application.\n";

static char usage_setpriority[] =
" Usage: SET PRIORITY <priority>\n"
"	Changes the priority for continuation upon exiting the application.\n"
" The priority must be a valid priority or label.\n";

static char usage_recordfile[] =
" Usage: RECORD FILE <filename> <format> <escape digits> <timeout> \\\n"
"                                          [offset samples] [BEEP] [s=silence]\n"
"	Record to a file until a given dtmf digit in the sequence is received\n"
" Returns -1 on hangup or error.  The format will specify what kind of file\n"
" will be recorded.  The timeout is the maximum record time in milliseconds, or\n"
" -1 for no timeout. \"Offset samples\" is optional, and, if provided, will seek\n"
" to the offset without exceeding the end of the file.  \"silence\" is the number\n"
" of seconds of silence allowed before the function returns despite the\n"
" lack of dtmf digits or reaching timeout.  Silence value must be\n"
" preceeded by \"s=\" and is also optional.\n";

static char usage_autohangup[] =
" Usage: SET AUTOHANGUP <time>\n"
"	Cause the channel to automatically hangup at <time> seconds in the\n"
" future.  Of course it can be hungup before then as well. Setting to 0 will\n"
" cause the autohangup feature to be disabled on this channel.\n";

static char usage_speechcreate[] =
" Usage: SPEECH CREATE <engine>\n"
"       Create a speech object to be used by the other Speech AGI commands.\n";

static char usage_speechset[] =
" Usage: SPEECH SET <name> <value>\n"
"       Set an engine-specific setting.\n";

static char usage_speechdestroy[] =
" Usage: SPEECH DESTROY\n"
"       Destroy the speech object created by SPEECH CREATE.\n";

static char usage_speechloadgrammar[] =
" Usage: SPEECH LOAD GRAMMAR <grammar name> <path to grammar>\n"
"       Loads the specified grammar as the specified name.\n";

static char usage_speechunloadgrammar[] =
" Usage: SPEECH UNLOAD GRAMMAR <grammar name>\n"
"       Unloads the specified grammar.\n";

static char usage_speechactivategrammar[] =
" Usage: SPEECH ACTIVATE GRAMMAR <grammar name>\n"
"       Activates the specified grammar on the speech object.\n";

static char usage_speechdeactivategrammar[] =
" Usage: SPEECH DEACTIVATE GRAMMAR <grammar name>\n"
"       Deactivates the specified grammar on the speech object.\n";

static char usage_speechrecognize[] =
" Usage: SPEECH RECOGNIZE <prompt> <timeout> [<offset>]\n"
"       Plays back given prompt while listening for speech and dtmf.\n";

/*!
 * \brief AGI commands list
 */
static struct agi_command commands[] = {
	{ { "answer", NULL }, handle_answer, NULL, NULL, 0 },
	{ { "asyncagi", "break", NULL }, handle_asyncagi_break, NULL, NULL, 1 },
	{ { "channel", "status", NULL }, handle_channelstatus, NULL, NULL, 0 },
	{ { "database", "del", NULL }, handle_dbdel, NULL, NULL, 1 },
	{ { "database", "deltree", NULL }, handle_dbdeltree, NULL, NULL, 1 },
	{ { "database", "get", NULL }, handle_dbget, NULL, NULL, 1 },
	{ { "database", "put", NULL }, handle_dbput, NULL, NULL, 1 },
	{ { "exec", NULL }, handle_exec, NULL, NULL, 1 },
	{ { "get", "data", NULL }, handle_getdata, NULL, NULL, 0 },
	{ { "get", "full", "variable", NULL }, handle_getvariablefull, NULL, NULL, 1 },
	{ { "get", "option", NULL }, handle_getoption, NULL, NULL, 0 },
	{ { "get", "variable", NULL }, handle_getvariable, NULL, NULL, 1 },
	{ { "hangup", NULL }, handle_hangup, NULL, NULL, 0 },
	{ { "noop", NULL }, handle_noop, NULL, NULL, 1 },
	{ { "receive", "char", NULL }, handle_recvchar, "Receives one character from channels supporting it", usage_recvchar , 0 },
	{ { "receive", "text", NULL }, handle_recvtext, "Receives text from channels supporting it", usage_recvtext , 0 },
	{ { "record", "file", NULL }, handle_recordfile, "Records to a given file", usage_recordfile , 0 },
	{ { "say", "alpha", NULL }, handle_sayalpha, "Says a given character string", usage_sayalpha , 0 },
	{ { "say", "digits", NULL }, handle_saydigits, "Says a given digit string", usage_saydigits , 0 },
	{ { "say", "number", NULL }, handle_saynumber, "Says a given number", usage_saynumber , 0 },
	{ { "say", "phonetic", NULL }, handle_sayphonetic, "Says a given character string with phonetics", usage_sayphonetic , 0 },
	{ { "say", "date", NULL }, handle_saydate, "Says a given date", usage_saydate , 0 },
	{ { "say", "time", NULL }, handle_saytime, "Says a given time", usage_saytime , 0 },
	{ { "say", "datetime", NULL }, handle_saydatetime, "Says a given time as specfied by the format given", usage_saydatetime , 0 },
	{ { "send", "image", NULL }, handle_sendimage, "Sends images to channels supporting it", usage_sendimage , 0 },
	{ { "send", "text", NULL }, handle_sendtext, "Sends text to channels supporting it", usage_sendtext , 0 },
	{ { "set", "autohangup", NULL }, handle_autohangup, "Autohangup channel in some time", usage_autohangup , 0 },
	{ { "set", "callerid", NULL }, handle_setcallerid, "Sets callerid for the current channel", usage_setcallerid , 0 },
	{ { "set", "context", NULL }, handle_setcontext, "Sets channel context", usage_setcontext , 0 },
	{ { "set", "extension", NULL }, handle_setextension, "Changes channel extension", usage_setextension , 0 },
	{ { "set", "music", NULL }, handle_setmusic, NULL, NULL, 0 },
	{ { "set", "priority", NULL }, handle_setpriority, "Set channel dialplan priority", usage_setpriority , 0 },
	{ { "set", "variable", NULL }, handle_setvariable, "Sets a channel variable", usage_setvariable , 1 },
	{ { "stream", "file", NULL }, handle_streamfile, "Sends audio file on channel", usage_streamfile , 0 },
	{ { "control", "stream", "file", NULL }, handle_controlstreamfile, "Sends audio file on channel and allows the listner to control the stream", usage_controlstreamfile , 0 },
	{ { "tdd", "mode", NULL }, handle_tddmode, "Toggles TDD mode (for the deaf)", usage_tddmode , 0 },
	{ { "verbose", NULL }, handle_verbose, "Logs a message to the trismedia verbose log", usage_verbose , 1 },
	{ { "wait", "for", "digit", NULL }, handle_waitfordigit, "Waits for a digit to be pressed", usage_waitfordigit , 0 },
	{ { "speech", "create", NULL }, handle_speechcreate, "Creates a speech object", usage_speechcreate, 0 },
	{ { "speech", "set", NULL }, handle_speechset, "Sets a speech engine setting", usage_speechset, 0 },
	{ { "speech", "destroy", NULL }, handle_speechdestroy, "Destroys a speech object", usage_speechdestroy, 1 },
	{ { "speech", "load", "grammar", NULL }, handle_speechloadgrammar, "Loads a grammar", usage_speechloadgrammar, 0 },
	{ { "speech", "unload", "grammar", NULL }, handle_speechunloadgrammar, "Unloads a grammar", usage_speechunloadgrammar, 1 },
	{ { "speech", "activate", "grammar", NULL }, handle_speechactivategrammar, "Activates a grammar", usage_speechactivategrammar, 0 },
	{ { "speech", "deactivate", "grammar", NULL }, handle_speechdeactivategrammar, "Deactivates a grammar", usage_speechdeactivategrammar, 0 },
	{ { "speech", "recognize", NULL }, handle_speechrecognize, "Recognizes speech", usage_speechrecognize, 0 },
};

static TRIS_RWLIST_HEAD_STATIC(agi_commands, agi_command);

static char *help_workhorse(int fd, char *match[])
{
	char fullcmd[MAX_CMD_LEN], matchstr[MAX_CMD_LEN];
	struct agi_command *e;

	if (match)
		tris_join(matchstr, sizeof(matchstr), match);

	tris_cli(fd, "%5.5s %30.30s   %s\n","Dead","Command","Description");
	TRIS_RWLIST_RDLOCK(&agi_commands);
	TRIS_RWLIST_TRAVERSE(&agi_commands, e, list) {
		if (!e->cmda[0])
			break;
		/* Hide commands that start with '_' */
		if ((e->cmda[0])[0] == '_')
			continue;
		tris_join(fullcmd, sizeof(fullcmd), e->cmda);
		if (match && strncasecmp(matchstr, fullcmd, strlen(matchstr)))
			continue;
		tris_cli(fd, "%5.5s %30.30s   %s\n", e->dead ? "Yes" : "No" , fullcmd, S_OR(e->summary, "Not available"));
	}
	TRIS_RWLIST_UNLOCK(&agi_commands);

	return CLI_SUCCESS;
}

int tris_agi_register(struct tris_module *mod, agi_command *cmd)
{
	char fullcmd[MAX_CMD_LEN];

	tris_join(fullcmd, sizeof(fullcmd), cmd->cmda);

	if (!find_command(cmd->cmda,1)) {
		cmd->docsrc = TRIS_STATIC_DOC;
		if (tris_strlen_zero(cmd->summary) && tris_strlen_zero(cmd->usage)) {
#ifdef TRIS_XML_DOCS
			*((char **) &cmd->summary) = tris_xmldoc_build_synopsis("agi", fullcmd);
			*((char **) &cmd->usage) = tris_xmldoc_build_description("agi", fullcmd);
			*((char **) &cmd->syntax) = tris_xmldoc_build_syntax("agi", fullcmd);
			*((char **) &cmd->seealso) = tris_xmldoc_build_seealso("agi", fullcmd);
			*((enum tris_doc_src *) &cmd->docsrc) = TRIS_XML_DOC;
#elif (!defined(HAVE_NULLSAFE_PRINTF))
			*((char **) &cmd->summary) = tris_strdup("");
			*((char **) &cmd->usage) = tris_strdup("");
			*((char **) &cmd->syntax) = tris_strdup("");
			*((char **) &cmd->seealso) = tris_strdup("");
#endif
		}

		cmd->mod = mod;
		TRIS_RWLIST_WRLOCK(&agi_commands);
		TRIS_LIST_INSERT_TAIL(&agi_commands, cmd, list);
		TRIS_RWLIST_UNLOCK(&agi_commands);
		if (mod != tris_module_info->self)
			tris_module_ref(tris_module_info->self);
		tris_verb(2, "AGI Command '%s' registered\n",fullcmd);
		return 1;
	} else {
		tris_log(LOG_WARNING, "Command already registered!\n");
		return 0;
	}
}

int tris_agi_unregister(struct tris_module *mod, agi_command *cmd)
{
	struct agi_command *e;
	int unregistered = 0;
	char fullcmd[MAX_CMD_LEN];

	tris_join(fullcmd, sizeof(fullcmd), cmd->cmda);

	TRIS_RWLIST_WRLOCK(&agi_commands);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&agi_commands, e, list) {
		if (cmd == e) {
			TRIS_RWLIST_REMOVE_CURRENT(list);
			if (mod != tris_module_info->self)
				tris_module_unref(tris_module_info->self);
#ifdef TRIS_XML_DOCS
			if (e->docsrc == TRIS_XML_DOC) {
				tris_free(e->summary);
				tris_free(e->usage);
				tris_free(e->syntax);
				tris_free(e->seealso);
				e->summary = NULL, e->usage = NULL;
				e->syntax = NULL, e->seealso = NULL;
			}
#endif
			unregistered=1;
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&agi_commands);
	if (unregistered)
		tris_verb(2, "AGI Command '%s' unregistered\n",fullcmd);
	else
		tris_log(LOG_WARNING, "Unable to unregister command: '%s'!\n",fullcmd);
	return unregistered;
}

int tris_agi_register_multiple(struct tris_module *mod, struct agi_command *cmd, unsigned int len)
{
	unsigned int i, x = 0;

	for (i = 0; i < len; i++) {
		if (tris_agi_register(mod, cmd + i) == 1) {
			x++;
			continue;
		}

		/* registration failed, unregister everything
		   that had been registered up to that point
		*/
		for (; x > 0; x--) {
			/* we are intentionally ignoring the
			   result of tris_agi_unregister() here,
			   but it should be safe to do so since
			   we just registered these commands and
			   the only possible way for unregistration
			   to fail is if the command is not
			   registered
			*/
			(void) tris_agi_unregister(mod, cmd + x - 1);
		}
		return -1;
	}

	return 0;
}

int tris_agi_unregister_multiple(struct tris_module *mod, struct agi_command *cmd, unsigned int len)
{
	unsigned int i;
	int res = 0;

	for (i = 0; i < len; i++) {
		/* remember whether any of the unregistration
		   attempts failed... there is no recourse if
		   any of them do
		*/
		res |= tris_agi_unregister(mod, cmd + i);
	}

	return res;
}

static agi_command *find_command(char *cmds[], int exact)
{
	int y, match;
	struct agi_command *e;

	TRIS_RWLIST_RDLOCK(&agi_commands);
	TRIS_RWLIST_TRAVERSE(&agi_commands, e, list) {
		if (!e->cmda[0])
			break;
		/* start optimistic */
		match = 1;
		for (y = 0; match && cmds[y]; y++) {
			/* If there are no more words in the command (and we're looking for
			   an exact match) or there is a difference between the two words,
			   then this is not a match */
			if (!e->cmda[y] && !exact)
				break;
			/* don't segfault if the next part of a command doesn't exist */
			if (!e->cmda[y]) {
				TRIS_RWLIST_UNLOCK(&agi_commands);
				return NULL;
			}
			if (strcasecmp(e->cmda[y], cmds[y]))
				match = 0;
		}
		/* If more words are needed to complete the command then this is not
		   a candidate (unless we're looking for a really inexact answer  */
		if ((exact > -1) && e->cmda[y])
			match = 0;
		if (match) {
			TRIS_RWLIST_UNLOCK(&agi_commands);
			return e;
		}
	}
	TRIS_RWLIST_UNLOCK(&agi_commands);
	return NULL;
}

static int parse_args(char *s, int *max, char *argv[])
{
	int x = 0, quoted = 0, escaped = 0, whitespace = 1;
	char *cur;

	cur = s;
	while(*s) {
		switch(*s) {
		case '"':
			/* If it's escaped, put a literal quote */
			if (escaped)
				goto normal;
			else
				quoted = !quoted;
			if (quoted && whitespace) {
				/* If we're starting a quote, coming off white space start a new word, too */
				argv[x++] = cur;
				whitespace=0;
			}
			escaped = 0;
		break;
		case ' ':
		case '\t':
			if (!quoted && !escaped) {
				/* If we're not quoted, mark this as whitespace, and
				   end the previous argument */
				whitespace = 1;
				*(cur++) = '\0';
			} else
				/* Otherwise, just treat it as anything else */
				goto normal;
			break;
		case '\\':
			/* If we're escaped, print a literal, otherwise enable escaping */
			if (escaped) {
				goto normal;
			} else {
				escaped=1;
			}
			break;
		default:
normal:
			if (whitespace) {
				if (x >= MAX_ARGS -1) {
					tris_log(LOG_WARNING, "Too many arguments, truncating\n");
					break;
				}
				/* Coming off of whitespace, start the next argument */
				argv[x++] = cur;
				whitespace=0;
			}
			*(cur++) = *s;
			escaped=0;
		}
		s++;
	}
	/* Null terminate */
	*(cur++) = '\0';
	argv[x] = NULL;
	*max = x;
	return 0;
}

static int agi_handle_command(struct tris_channel *chan, AGI *agi, char *buf, int dead)
{
	char *argv[MAX_ARGS];
	int argc = MAX_ARGS, res;
	agi_command *c;
	const char *ami_res = "Unknown Result";
	char *ami_cmd = tris_strdupa(buf);
	int command_id = tris_random(), resultcode = 200;

	manager_event(EVENT_FLAG_AGI, "AGIExec",
			"SubEvent: Start\r\n"
			"Channel: %s\r\n"
			"CommandId: %d\r\n"
			"Command: %s\r\n", chan->name, command_id, ami_cmd);
	parse_args(buf, &argc, argv);
	if ((c = find_command(argv, 0)) && (!dead || (dead && c->dead))) {
		/* if this command wasnt registered by res_agi, be sure to usecount
		the module we are using */
		if (c->mod != tris_module_info->self)
			tris_module_ref(c->mod);
		/* If the AGI command being executed is an actual application (using agi exec)
		the app field will be updated in pbx_exec via handle_exec */
		if (chan->cdr && !tris_check_hangup(chan) && strcasecmp(argv[0], "EXEC"))
			tris_cdr_setapp(chan->cdr, "AGI", buf);

		res = c->handler(chan, agi, argc, argv);
		if (c->mod != tris_module_info->self)
			tris_module_unref(c->mod);
		switch (res) {
		case RESULT_SHOWUSAGE: ami_res = "Usage"; resultcode = 520; break;
		case RESULT_FAILURE: ami_res = "Failure"; resultcode = -1; break;
		case RESULT_SUCCESS: ami_res = "Success"; resultcode = 200; break;
		}
		manager_event(EVENT_FLAG_AGI, "AGIExec",
				"SubEvent: End\r\n"
				"Channel: %s\r\n"
				"CommandId: %d\r\n"
				"Command: %s\r\n"
				"ResultCode: %d\r\n"
				"Result: %s\r\n", chan->name, command_id, ami_cmd, resultcode, ami_res);
		switch(res) {
		case RESULT_SHOWUSAGE:
			if (tris_strlen_zero(c->usage)) {
				tris_agi_send(agi->fd, chan, "520 Invalid command syntax.  Proper usage not available.\n");
			} else {
				tris_agi_send(agi->fd, chan, "520-Invalid command syntax.  Proper usage follows:\n");
				tris_agi_send(agi->fd, chan, "%s", c->usage);
				tris_agi_send(agi->fd, chan, "520 End of proper usage.\n");
			}
			break;
		case RESULT_FAILURE:
			/* They've already given the failure.  We've been hung up on so handle this
			   appropriately */
			return -1;
		}
	} else if ((c = find_command(argv, 0))) {
		tris_agi_send(agi->fd, chan, "511 Command Not Permitted on a dead channel\n");
		manager_event(EVENT_FLAG_AGI, "AGIExec",
				"SubEvent: End\r\n"
				"Channel: %s\r\n"
				"CommandId: %d\r\n"
				"Command: %s\r\n"
				"ResultCode: 511\r\n"
				"Result: Command not permitted on a dead channel\r\n", chan->name, command_id, ami_cmd);
	} else {
		tris_agi_send(agi->fd, chan, "510 Invalid or unknown command\n");
		manager_event(EVENT_FLAG_AGI, "AGIExec",
				"SubEvent: End\r\n"
				"Channel: %s\r\n"
				"CommandId: %d\r\n"
				"Command: %s\r\n"
				"ResultCode: 510\r\n"
				"Result: Invalid or unknown command\r\n", chan->name, command_id, ami_cmd);
	}
	return 0;
}
static enum agi_result run_agi(struct tris_channel *chan, char *request, AGI *agi, int pid, int *status, int dead, int argc, char *argv[])
{
	struct tris_channel *c;
	int outfd, ms, needhup = 0;
	enum agi_result returnstatus = AGI_RESULT_SUCCESS;
	struct tris_frame *f;
	char buf[AGI_BUF_LEN];
	char *res = NULL;
	FILE *readf;
	/* how many times we'll retry if tris_waitfor_nandfs will return without either
	  channel or file descriptor in case select is interrupted by a system call (EINTR) */
	int retry = AGI_NANDFS_RETRY;
	int send_sighup;
	const char *sighup_str;
	
	tris_channel_lock(chan);
	sighup_str = pbx_builtin_getvar_helper(chan, "AGISIGHUP");
	send_sighup = tris_strlen_zero(sighup_str) || !tris_false(sighup_str);
	tris_channel_unlock(chan);

	if (!(readf = fdopen(agi->ctrl, "r"))) {
		tris_log(LOG_WARNING, "Unable to fdopen file descriptor\n");
		if (send_sighup && pid > -1)
			kill(pid, SIGHUP);
		close(agi->ctrl);
		return AGI_RESULT_FAILURE;
	}
	
	setlinebuf(readf);
	setup_env(chan, request, agi->fd, (agi->audio > -1), argc, argv);
	for (;;) {
		if (needhup) {
			needhup = 0;
			dead = 1;
			if (send_sighup) {
				if (pid > -1) {
					kill(pid, SIGHUP);
				} else if (agi->fast) {
					send(agi->ctrl, "HANGUP\n", 7, MSG_OOB);
				}
			}
		}
		ms = -1;
		c = tris_waitfor_nandfds(&chan, dead ? 0 : 1, &agi->ctrl, 1, NULL, &outfd, &ms);
		if (c) {
			retry = AGI_NANDFS_RETRY;
			/* Idle the channel until we get a command */
			f = tris_read(c);
			if (!f) {
				tris_debug(1, "%s hungup\n", chan->name);
				returnstatus = AGI_RESULT_HANGUP;
				needhup = 1;
				continue;
			} else {
				/* If it's voice, write it to the audio pipe */
				if ((agi->audio > -1) && (f->frametype == TRIS_FRAME_VOICE)) {
					/* Write, ignoring errors */
					if (write(agi->audio, f->data.ptr, f->datalen) < 0) {
					}
				}
				tris_frfree(f);
			}
		} else if (outfd > -1) {
			size_t len = sizeof(buf);
			size_t buflen = 0;

			retry = AGI_NANDFS_RETRY;
			buf[0] = '\0';

			while (buflen < (len - 1)) {
				res = fgets(buf + buflen, len, readf);
				if (feof(readf))
					break;
				if (ferror(readf) && ((errno != EINTR) && (errno != EAGAIN)))
					break;
				if (res != NULL && !agi->fast)
					break;
				buflen = strlen(buf);
				if (buflen && buf[buflen - 1] == '\n')
					break;
				len -= buflen;
				if (agidebug)
					tris_verbose( "AGI Rx << temp buffer %s - errno %s\n", buf, strerror(errno));
			}

			if (!buf[0]) {
				/* Program terminated */
				if (returnstatus) {
					returnstatus = -1;
				}
				tris_verb(3, "<%s>AGI Script %s completed, returning %d\n", chan->name, request, returnstatus);
				if (pid > 0)
					waitpid(pid, status, 0);
				/* No need to kill the pid anymore, since they closed us */
				pid = -1;
				break;
			}

			/* Special case for inability to execute child process */
			if (*buf && strncasecmp(buf, "failure", 7) == 0) {
				returnstatus = AGI_RESULT_FAILURE;
				break;
			}

			/* get rid of trailing newline, if any */
			if (*buf && buf[strlen(buf) - 1] == '\n')
				buf[strlen(buf) - 1] = 0;
			if (agidebug)
				tris_verbose("<%s>AGI Rx << %s\n", chan->name, buf);
			returnstatus |= agi_handle_command(chan, agi, buf, dead);
			/* If the handle_command returns -1, we need to stop */
			if (returnstatus < 0) {
				needhup = 1;
				continue;
			}
		} else {
			if (--retry <= 0) {
				tris_log(LOG_WARNING, "No channel, no fd?\n");
				returnstatus = AGI_RESULT_FAILURE;
				break;
			}
		}
	}
	if (agi->speech) {
		tris_speech_destroy(agi->speech);
	}
	/* Notify process */
	if (send_sighup) {
		if (pid > -1) {
			if (kill(pid, SIGHUP)) {
				tris_log(LOG_WARNING, "unable to send SIGHUP to AGI process %d: %s\n", pid, strerror(errno));
			} else { /* Give the process a chance to die */
				usleep(1);
			}
			waitpid(pid, status, WNOHANG);
		} else if (agi->fast) {
			send(agi->ctrl, "HANGUP\n", 7, MSG_OOB);
		}
	}
	fclose(readf);
	return returnstatus;
}

static char *handle_cli_agi_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct agi_command *command;
	char fullcmd[MAX_CMD_LEN];
	int error = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "agi show commands [topic]";
		e->usage =
			"Usage: agi show commands [topic] <topic>\n"
			"       When called with a topic as an argument, displays usage\n"
			"       information on the given command.  If called without a\n"
			"       topic, it provides a list of AGI commands.\n";
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc < e->args - 1 || (a->argc >= e->args && strcasecmp(a->argv[e->args - 1], "topic")))
		return CLI_SHOWUSAGE;
	if (a->argc > e->args - 1) {
		command = find_command(a->argv + e->args, 1);
		if (command) {
			char *synopsis = NULL, *description = NULL, *syntax = NULL, *seealso = NULL;
			char info[30 + MAX_CMD_LEN];					/* '-= Info about...' */
			char infotitle[30 + MAX_CMD_LEN + TRIS_TERM_MAX_ESCAPE_CHARS];	/* '-= Info about...' with colors */
			char syntitle[11 + TRIS_TERM_MAX_ESCAPE_CHARS];			/* [Syntax]\n with colors */
			char desctitle[15 + TRIS_TERM_MAX_ESCAPE_CHARS];			/* [Description]\n with colors */
			char deadtitle[13 + TRIS_TERM_MAX_ESCAPE_CHARS];			/* [Runs Dead]\n with colors */
			char deadcontent[3 + TRIS_TERM_MAX_ESCAPE_CHARS];		/* 'Yes' or 'No' with colors */
			char seealsotitle[12 + TRIS_TERM_MAX_ESCAPE_CHARS];		/* [See Also]\n with colors */
			char stxtitle[10 + TRIS_TERM_MAX_ESCAPE_CHARS];			/* [Syntax]\n with colors */
			size_t synlen, desclen, seealsolen, stxlen;

			term_color(syntitle, "[Synopsis]\n", COLOR_MAGENTA, 0, sizeof(syntitle));
			term_color(desctitle, "[Description]\n", COLOR_MAGENTA, 0, sizeof(desctitle));
			term_color(deadtitle, "[Runs Dead]\n", COLOR_MAGENTA, 0, sizeof(deadtitle));
			term_color(seealsotitle, "[See Also]\n", COLOR_MAGENTA, 0, sizeof(seealsotitle));
			term_color(stxtitle, "[Syntax]\n", COLOR_MAGENTA, 0, sizeof(stxtitle));
			term_color(deadcontent, command->dead ? "Yes" : "No", COLOR_CYAN, 0, sizeof(deadcontent));

			tris_join(fullcmd, sizeof(fullcmd), a->argv + e->args);
			snprintf(info, sizeof(info), "\n  -= Info about agi '%s' =- ", fullcmd);
			term_color(infotitle, info, COLOR_CYAN, 0, sizeof(infotitle));
#ifdef TRIS_XML_DOCS
			if (command->docsrc == TRIS_XML_DOC) {
				synopsis = tris_xmldoc_printable(S_OR(command->summary, "Not available"), 1);
				description = tris_xmldoc_printable(S_OR(command->usage, "Not available"), 1);
				seealso = tris_xmldoc_printable(S_OR(command->seealso, "Not available"), 1);
				if (!seealso || !description || !synopsis) {
					error = 1;
					goto return_cleanup;
				}
			} else
#endif
			{
				synlen = strlen(S_OR(command->summary, "Not available")) + TRIS_TERM_MAX_ESCAPE_CHARS;
				synopsis = tris_malloc(synlen);

				desclen = strlen(S_OR(command->usage, "Not available")) + TRIS_TERM_MAX_ESCAPE_CHARS;
				description = tris_malloc(desclen);

				seealsolen = strlen(S_OR(command->seealso, "Not available")) + TRIS_TERM_MAX_ESCAPE_CHARS;
				seealso = tris_malloc(seealsolen);

				if (!synopsis || !description || !seealso) {
					error = 1;
					goto return_cleanup;
				}
				term_color(synopsis, S_OR(command->summary, "Not available"), COLOR_CYAN, 0, synlen);
				term_color(description, S_OR(command->usage, "Not available"), COLOR_CYAN, 0, desclen);
				term_color(seealso, S_OR(command->seealso, "Not available"), COLOR_CYAN, 0, seealsolen);
			}

			stxlen = strlen(S_OR(command->syntax, "Not available")) + TRIS_TERM_MAX_ESCAPE_CHARS;
			syntax = tris_malloc(stxlen);
			if (!syntax) {
				error = 1;
				goto return_cleanup;
			}
			term_color(syntax, S_OR(command->syntax, "Not available"), COLOR_CYAN, 0, stxlen);

			tris_cli(a->fd, "%s\n\n%s%s\n\n%s%s\n\n%s%s\n\n%s%s\n\n%s%s\n\n", infotitle, stxtitle, syntax,
					desctitle, description, syntitle, synopsis, deadtitle, deadcontent,
					seealsotitle, seealso);
return_cleanup:
			tris_free(synopsis);
			tris_free(description);
			tris_free(syntax);
			tris_free(seealso);
		} else {
			if (find_command(a->argv + e->args, -1)) {
				return help_workhorse(a->fd, a->argv + e->args);
			} else {
				tris_join(fullcmd, sizeof(fullcmd), a->argv + e->args);
				tris_cli(a->fd, "No such command '%s'.\n", fullcmd);
			}
		}
	} else {
		return help_workhorse(a->fd, NULL);
	}
	return (error ? CLI_FAILURE : CLI_SUCCESS);
}

/*! \brief Convert string to use HTML escaped characters
	\note Maybe this should be a generic function?
*/
static void write_html_escaped(FILE *htmlfile, char *str)
{
	char *cur = str;

	while(*cur) {
		switch (*cur) {
		case '<':
			fprintf(htmlfile, "%s", "&lt;");
			break;
		case '>':
			fprintf(htmlfile, "%s", "&gt;");
			break;
		case '&':
			fprintf(htmlfile, "%s", "&amp;");
			break;
		case '"':
			fprintf(htmlfile, "%s", "&quot;");
			break;
		default:
			fprintf(htmlfile, "%c", *cur);
			break;
		}
		cur++;
	}

	return;
}

static int write_htmldump(char *filename)
{
	struct agi_command *command;
	char fullcmd[MAX_CMD_LEN];
	FILE *htmlfile;

	if (!(htmlfile = fopen(filename, "wt")))
		return -1;

	fprintf(htmlfile, "<HTML>\n<HEAD>\n<TITLE>AGI Commands</TITLE>\n</HEAD>\n");
	fprintf(htmlfile, "<BODY>\n<CENTER><B><H1>AGI Commands</H1></B></CENTER>\n\n");
	fprintf(htmlfile, "<TABLE BORDER=\"0\" CELLSPACING=\"10\">\n");

	TRIS_RWLIST_RDLOCK(&agi_commands);
	TRIS_RWLIST_TRAVERSE(&agi_commands, command, list) {
#ifdef TRIS_XML_DOCS
		char *stringptmp;
#endif
		char *tempstr, *stringp;

		if (!command->cmda[0])	/* end ? */
			break;
		/* Hide commands that start with '_' */
		if ((command->cmda[0])[0] == '_')
			continue;
		tris_join(fullcmd, sizeof(fullcmd), command->cmda);

		fprintf(htmlfile, "<TR><TD><TABLE BORDER=\"1\" CELLPADDING=\"5\" WIDTH=\"100%%\">\n");
		fprintf(htmlfile, "<TR><TH ALIGN=\"CENTER\"><B>%s - %s</B></TH></TR>\n", fullcmd, command->summary);
#ifdef TRIS_XML_DOCS
		stringptmp = tris_xmldoc_printable(command->usage, 0);
		stringp = stringptmp;
#else
		stringp = command->usage;
#endif
		tempstr = strsep(&stringp, "\n");

		fprintf(htmlfile, "<TR><TD ALIGN=\"CENTER\">");
		write_html_escaped(htmlfile, tempstr);
		fprintf(htmlfile, "</TD></TR>\n");
		fprintf(htmlfile, "<TR><TD ALIGN=\"CENTER\">\n");

		while ((tempstr = strsep(&stringp, "\n")) != NULL) {
			write_html_escaped(htmlfile, tempstr);
			fprintf(htmlfile, "<BR>\n");
		}
		fprintf(htmlfile, "</TD></TR>\n");
		fprintf(htmlfile, "</TABLE></TD></TR>\n\n");
#ifdef TRIS_XML_DOCS
		tris_free(stringptmp);
#endif
	}
	TRIS_RWLIST_UNLOCK(&agi_commands);
	fprintf(htmlfile, "</TABLE>\n</BODY>\n</HTML>\n");
	fclose(htmlfile);
	return 0;
}

static char *handle_cli_agi_dump_html(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "agi dump html";
		e->usage =
			"Usage: agi dump html <filename>\n"
			"       Dumps the AGI command list in HTML format to the given\n"
			"       file.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != e->args + 1)
		return CLI_SHOWUSAGE;

	if (write_htmldump(a->argv[e->args]) < 0) {
		tris_cli(a->fd, "Could not create file '%s'\n", a->argv[e->args]);
		return CLI_SHOWUSAGE;
	}
	tris_cli(a->fd, "AGI HTML commands dumped to: %s\n", a->argv[e->args]);
	return CLI_SUCCESS;
}

static int agi_exec_full(struct tris_channel *chan, void *data, int enhanced, int dead)
{
	enum agi_result res;
	char buf[AGI_BUF_LEN] = "", *tmp = buf;
	int fds[2], efd = -1, pid;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(arg)[MAX_ARGS];
	);
	AGI agi;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "AGI requires an argument (script)\n");
		return -1;
	}
	if (dead)
		tris_debug(3, "Hungup channel detected, running agi in dead mode.\n");
	tris_copy_string(buf, data, sizeof(buf));
	memset(&agi, 0, sizeof(agi));
	TRIS_STANDARD_APP_ARGS(args, tmp);
	args.argv[args.argc] = NULL;
#if 0
	 /* Answer if need be */
	if (chan->_state != TRIS_STATE_UP) {
		if (tris_answer(chan))
			return -1;
	}
#endif
	res = launch_script(chan, args.argv[0], args.argv, fds, enhanced ? &efd : NULL, &pid);
	/* Async AGI do not require run_agi(), so just proceed if normal AGI
	   or Fast AGI are setup with success. */
	if (res == AGI_RESULT_SUCCESS || res == AGI_RESULT_SUCCESS_FAST) {
		int status = 0;
		agi.fd = fds[1];
		agi.ctrl = fds[0];
		agi.audio = efd;
		agi.fast = (res == AGI_RESULT_SUCCESS_FAST) ? 1 : 0;
		res = run_agi(chan, args.argv[0], &agi, pid, &status, dead, args.argc, args.argv);
		/* If the fork'd process returns non-zero, set AGISTATUS to FAILURE */
		if ((res == AGI_RESULT_SUCCESS || res == AGI_RESULT_SUCCESS_FAST) && status)
			res = AGI_RESULT_FAILURE;
		if (fds[1] != fds[0])
			close(fds[1]);
		if (efd > -1)
			close(efd);
	}
	tris_safe_fork_cleanup();

	switch (res) {
	case AGI_RESULT_SUCCESS:
	case AGI_RESULT_SUCCESS_FAST:
	case AGI_RESULT_SUCCESS_ASYNC:
		pbx_builtin_setvar_helper(chan, "AGISTATUS", "SUCCESS");
		break;
	case AGI_RESULT_FAILURE:
		pbx_builtin_setvar_helper(chan, "AGISTATUS", "FAILURE");
		break;
	case AGI_RESULT_NOTFOUND:
		pbx_builtin_setvar_helper(chan, "AGISTATUS", "NOTFOUND");
		break;
	case AGI_RESULT_HANGUP:
		pbx_builtin_setvar_helper(chan, "AGISTATUS", "HANGUP");
		return -1;
	}

	return 0;
}

static int agi_exec(struct tris_channel *chan, void *data)
{
	if (!tris_check_hangup(chan))
		return agi_exec_full(chan, data, 0, 0);
	else
		return agi_exec_full(chan, data, 0, 1);
}

static int eagi_exec(struct tris_channel *chan, void *data)
{
	int readformat, res;

	if (tris_check_hangup(chan)) {
		tris_log(LOG_ERROR, "EAGI cannot be run on a dead/hungup channel, please use AGI.\n");
		return 0;
	}
	readformat = chan->readformat;
	if (tris_set_read_format(chan, TRIS_FORMAT_SLINEAR)) {
		tris_log(LOG_WARNING, "Unable to set channel '%s' to linear mode\n", chan->name);
		return -1;
	}
	res = agi_exec_full(chan, data, 1, 0);
	if (!res) {
		if (tris_set_read_format(chan, readformat)) {
			tris_log(LOG_WARNING, "Unable to restore channel '%s' to format %s\n", chan->name, tris_getformatname(readformat));
		}
	}
	return res;
}

static int deadagi_exec(struct tris_channel *chan, void *data)
{
	tris_log(LOG_WARNING, "DeadAGI has been deprecated, please use AGI in all cases!\n");
	return agi_exec(chan, data);
}

static struct tris_cli_entry cli_agi[] = {
	TRIS_CLI_DEFINE(handle_cli_agi_add_cmd,   "Add AGI command to a channel in Async AGI"),
	TRIS_CLI_DEFINE(handle_cli_agi_debug,     "Enable/Disable AGI debugging"),
	TRIS_CLI_DEFINE(handle_cli_agi_show,      "List AGI commands or specific help"),
	TRIS_CLI_DEFINE(handle_cli_agi_dump_html, "Dumps a list of AGI commands in HTML format")
};

static int unload_module(void)
{
	tris_cli_unregister_multiple(cli_agi, ARRAY_LEN(cli_agi));
	/* we can safely ignore the result of tris_agi_unregister_multiple() here, since it cannot fail, as
	   we know that these commands were registered by this module and are still registered
	*/
	(void) tris_agi_unregister_multiple(tris_module_info->self, commands, ARRAY_LEN(commands));
	tris_unregister_application(eapp);
	tris_unregister_application(deadapp);
	tris_manager_unregister("AGI");
	return tris_unregister_application(app);
}

static int load_module(void)
{
	tris_cli_register_multiple(cli_agi, ARRAY_LEN(cli_agi));
	/* we can safely ignore the result of tris_agi_register_multiple() here, since it cannot fail, as
	   no other commands have been registered yet
	*/
	(void) tris_agi_register_multiple(tris_module_info->self, commands, ARRAY_LEN(commands));
	tris_register_application(deadapp, deadagi_exec, deadsynopsis, descrip);
	tris_register_application(eapp, eagi_exec, esynopsis, descrip);
	tris_manager_register2("AGI", EVENT_FLAG_AGI, action_add_agi_cmd, "Add an AGI command to execute by Async AGI", mandescr_asyncagi);
	return tris_register_application(app, agi_exec, synopsis, descrip);
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_GLOBAL_SYMBOLS, "Trismedia Gateway Interface (AGI)",
		.load = load_module,
		.unload = unload_module,
		);
