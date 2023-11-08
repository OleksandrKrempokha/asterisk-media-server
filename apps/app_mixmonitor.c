/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Anthony Minessale II
 * Copyright (C) 2005 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Kevin P. Fleming <kpfleming@digium.com>
 *
 * Based on app_muxmon.c provided by
 * Anthony Minessale II <anthmct@yahoo.com>
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
 * \brief MixMonitor() - Record a call and mix the audio during the recording
 * \ingroup applications
 *
 * \author Mark Spencer <markster@digium.com>
 * \author Kevin P. Fleming <kpfleming@digium.com>
 *
 * \note Based on app_muxmon.c provided by
 * Anthony Minessale II <anthmct@yahoo.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 237657 $")

#include "trismedia/paths.h"	/* use tris_config_TRIS_MONITOR_DIR */
#include "trismedia/file.h"
#include "trismedia/audiohook.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/cli.h"
#include "trismedia/app.h"
#include "trismedia/channel.h"

/*** DOCUMENTATION
	<application name="MixMonitor" language="en_US">
		<synopsis>
			Record a call and mix the audio during the recording.  Use of StopMixMonitor is required
			to guarantee the audio file is available for processing during dialplan execution.
		</synopsis>
		<syntax>
			<parameter name="file" required="true" argsep=".">
				<argument name="filename" required="true">
					<para>If <replaceable>filename</replaceable> is an absolute path, uses that path, otherwise
					creates the file in the configured monitoring directory from <filename>trismedia.conf.</filename></para>
				</argument>
				<argument name="extension" required="true" />
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="a">
						<para>Append to the file instead of overwriting it.</para>
					</option>
					<option name="b">
						<para>Only save audio to the file while the channel is bridged.</para>
						<note><para>Does not include conferences or sounds played to each bridged party</para></note>
						<note><para>If you utilize this option inside a Local channel, you must make sure the Local
						channel is not optimized away. To do this, be sure to call your Local channel with the
						<literal>/n</literal> option. For example: Dial(Local/start@mycontext/n)</para></note>
					</option>
					<option name="v">
						<para>Adjust the <emphasis>heard</emphasis> volume by a factor of <replaceable>x</replaceable>
						(range <literal>-4</literal> to <literal>4</literal>)</para>
						<argument name="x" required="true" />
					</option>
					<option name="V">
						<para>Adjust the <emphasis>spoken</emphasis> volume by a factor
						of <replaceable>x</replaceable> (range <literal>-4</literal> to <literal>4</literal>)</para>
						<argument name="x" required="true" />
					</option>
					<option name="W">
						<para>Adjust both, <emphasis>heard and spoken</emphasis> volumes by a factor
						of <replaceable>x</replaceable> (range <literal>-4</literal> to <literal>4</literal>)</para>
						<argument name="x" required="true" />
					</option>
				</optionlist>
			</parameter>
			<parameter name="command">
				<para>Will be executed when the recording is over.</para>
				<para>Any strings matching <literal>^{X}</literal> will be unescaped to <variable>X</variable>.</para>
				<para>All variables will be evaluated at the time MixMonitor is called.</para>
			</parameter>
		</syntax>
		<description>
			<para>Records the audio on the current channel to the specified file.</para>
			<variablelist>
				<variable name="MIXMONITOR_FILENAME">
					<para>Will contain the filename used to record.</para>
				</variable>
			</variablelist>	
		</description>
		<see-also>
			<ref type="application">Monitor</ref>
			<ref type="application">StopMixMonitor</ref>
			<ref type="application">PauseMonitor</ref>
			<ref type="application">UnpauseMonitor</ref>
		</see-also>
	</application>
	<application name="StopMixMonitor" language="en_US">
		<synopsis>
			Stop recording a call through MixMonitor, and free the recording's file handle.
		</synopsis>
		<syntax />
		<description>
			<para>Stops the audio recording that was started with a call to <literal>MixMonitor()</literal>
			on the current channel.</para>
		</description>
		<see-also>
			<ref type="application">MixMonitor</ref>
		</see-also>
	</application>
		
 ***/

#define get_volfactor(x) x ? ((x > 0) ? (1 << x) : ((1 << abs(x)) * -1)) : 0

static const char *app = "MixMonitor";

static const char *stop_app = "StopMixMonitor";

struct module_symbols *me;

static const char *mixmonitor_spy_type = "MixMonitor";

struct mixmonitor {
	struct tris_audiohook audiohook;
	char *filename;
	char *post_process;
	char *name;
	unsigned int flags;
	struct mixmonitor_ds *mixmonitor_ds;
};

enum {
	MUXFLAG_APPEND = (1 << 1),
	MUXFLAG_BRIDGED = (1 << 2),
	MUXFLAG_VOLUME = (1 << 3),
	MUXFLAG_READVOLUME = (1 << 4),
	MUXFLAG_WRITEVOLUME = (1 << 5),
} mixmonitor_flags;

enum {
	OPT_ARG_READVOLUME = 0,
	OPT_ARG_WRITEVOLUME,
	OPT_ARG_VOLUME,
	OPT_ARG_ARRAY_SIZE,
} mixmonitor_args;

TRIS_APP_OPTIONS(mixmonitor_opts, {
	TRIS_APP_OPTION('a', MUXFLAG_APPEND),
	TRIS_APP_OPTION('b', MUXFLAG_BRIDGED),
	TRIS_APP_OPTION_ARG('v', MUXFLAG_READVOLUME, OPT_ARG_READVOLUME),
	TRIS_APP_OPTION_ARG('V', MUXFLAG_WRITEVOLUME, OPT_ARG_WRITEVOLUME),
	TRIS_APP_OPTION_ARG('W', MUXFLAG_VOLUME, OPT_ARG_VOLUME),
});

/* This structure is used as a means of making sure that our pointer to
 * the channel we are monitoring remains valid. This is very similar to 
 * what is used in app_chanspy.c.
 */
struct mixmonitor_ds {
	struct tris_channel *chan;
	/* These condition variables are used to be sure that the channel
	 * hangup code completes before the mixmonitor thread attempts to
	 * free this structure. The combination of a bookean flag and a
	 * tris_cond_t ensure that no matter what order the threads run in,
	 * we are guaranteed to never have the waiting thread block forever
	 * in the case that the signaling thread runs first.
	 */
	unsigned int destruction_ok;
	tris_cond_t destruction_condition;
	tris_mutex_t lock;

	/* The filestream is held in the datastore so it can be stopped
	 * immediately during stop_mixmonitor or channel destruction. */
	int fs_quit;
	struct tris_filestream *fs;
	struct tris_audiohook *audiohook;
};

 /*!
  * \internal
  * \pre mixmonitor_ds must be locked before calling this function
  */
static void mixmonitor_ds_close_fs(struct mixmonitor_ds *mixmonitor_ds)
{
	if (mixmonitor_ds->fs) {
		tris_closestream(mixmonitor_ds->fs);
		mixmonitor_ds->fs = NULL;
		mixmonitor_ds->fs_quit = 1;
		tris_verb(2, "MixMonitor close filestream\n");
	}
}

static void mixmonitor_ds_destroy(void *data)
{
	struct mixmonitor_ds *mixmonitor_ds = data;

	tris_mutex_lock(&mixmonitor_ds->lock);
	mixmonitor_ds->chan = NULL;
	mixmonitor_ds->audiohook = NULL;
	mixmonitor_ds->destruction_ok = 1;
	tris_cond_signal(&mixmonitor_ds->destruction_condition);
	tris_mutex_unlock(&mixmonitor_ds->lock);
}

static void mixmonitor_ds_chan_fixup(void *data, struct tris_channel *old_chan, struct tris_channel *new_chan)
{
	struct mixmonitor_ds *mixmonitor_ds = data;

	tris_mutex_lock(&mixmonitor_ds->lock);
	mixmonitor_ds->chan = new_chan;
	tris_mutex_unlock(&mixmonitor_ds->lock);
}

static struct tris_datastore_info mixmonitor_ds_info = {
	.type = "mixmonitor",
	.destroy = mixmonitor_ds_destroy,
	.chan_fixup = mixmonitor_ds_chan_fixup,
};

static void destroy_monitor_audiohook(struct mixmonitor *mixmonitor)
{
	if (mixmonitor->mixmonitor_ds) {
		tris_mutex_lock(&mixmonitor->mixmonitor_ds->lock);
		mixmonitor->mixmonitor_ds->audiohook = NULL;
		tris_mutex_unlock(&mixmonitor->mixmonitor_ds->lock);
	}
	/* kill the audiohook.*/
	tris_audiohook_lock(&mixmonitor->audiohook);
	tris_audiohook_detach(&mixmonitor->audiohook);
	tris_audiohook_unlock(&mixmonitor->audiohook);
	tris_audiohook_destroy(&mixmonitor->audiohook);
}

static int startmon(struct tris_channel *chan, struct tris_audiohook *audiohook) 
{
	struct tris_channel *peer = NULL;
	int res = 0;

	if (!chan)
		return -1;

	tris_audiohook_attach(chan, audiohook);

	if (!res && tris_test_flag(chan, TRIS_FLAG_NBRIDGE) && (peer = tris_bridged_channel(chan)))
		tris_softhangup(peer, TRIS_SOFTHANGUP_UNBRIDGE);	

	return res;
}

#define SAMPLES_PER_FRAME 160

static void mixmonitor_free(struct mixmonitor *mixmonitor)
{
	if (mixmonitor) {
		if (mixmonitor->mixmonitor_ds) {
			tris_mutex_destroy(&mixmonitor->mixmonitor_ds->lock);
			tris_cond_destroy(&mixmonitor->mixmonitor_ds->destruction_condition);
			tris_free(mixmonitor->mixmonitor_ds);
		}
		tris_free(mixmonitor);
	}
}

static void *mixmonitor_thread(void *obj) 
{
	struct mixmonitor *mixmonitor = obj;
	struct tris_filestream **fs = NULL;
	unsigned int oflags;
	char *ext;
	int errflag = 0;

	tris_verb(2, "Begin MixMonitor Recording %s\n", mixmonitor->name);

	fs = &mixmonitor->mixmonitor_ds->fs;

	/* The audiohook must enter and exit the loop locked */
	tris_audiohook_lock(&mixmonitor->audiohook);
	while (mixmonitor->audiohook.status == TRIS_AUDIOHOOK_STATUS_RUNNING && !mixmonitor->mixmonitor_ds->fs_quit) {
		struct tris_frame *fr = NULL;

		tris_audiohook_trigger_wait(&mixmonitor->audiohook);

		if (mixmonitor->audiohook.status != TRIS_AUDIOHOOK_STATUS_RUNNING)
			break;

		if (!(fr = tris_audiohook_read_frame(&mixmonitor->audiohook, SAMPLES_PER_FRAME, TRIS_AUDIOHOOK_DIRECTION_BOTH, TRIS_FORMAT_SLINEAR)))
			continue;

		/* audiohook lock is not required for the next block.
		 * Unlock it, but remember to lock it before looping or exiting */
		tris_audiohook_unlock(&mixmonitor->audiohook);

		tris_mutex_lock(&mixmonitor->mixmonitor_ds->lock);
		if (!tris_test_flag(mixmonitor, MUXFLAG_BRIDGED) || (mixmonitor->mixmonitor_ds->chan && tris_bridged_channel(mixmonitor->mixmonitor_ds->chan))) {
			/* Initialize the file if not already done so */
			if (!*fs && !errflag && !mixmonitor->mixmonitor_ds->fs_quit) {
				oflags = O_CREAT | O_WRONLY;
				oflags |= tris_test_flag(mixmonitor, MUXFLAG_APPEND) ? O_APPEND : O_TRUNC;

				if ((ext = strrchr(mixmonitor->filename, '.')))
					*(ext++) = '\0';
				else
					ext = "raw";

				if (!(*fs = tris_writefile(mixmonitor->filename, ext, NULL, oflags, 0, 0666))) {
					tris_log(LOG_ERROR, "Cannot open %s.%s\n", mixmonitor->filename, ext);
					errflag = 1;
				}
			}

			/* Write out the frame(s) */
			if (*fs) {
				struct tris_frame *cur;

				for (cur = fr; cur; cur = TRIS_LIST_NEXT(cur, frame_list)) {
					tris_writestream(*fs, cur);
				}
			}
		}
		tris_mutex_unlock(&mixmonitor->mixmonitor_ds->lock);

		/* All done! free it. */
		tris_frame_free(fr, 0);
		tris_audiohook_lock(&mixmonitor->audiohook);
	}

	tris_audiohook_unlock(&mixmonitor->audiohook);

	/* Datastore cleanup.  close the filestream and wait for ds destruction */
	tris_mutex_lock(&mixmonitor->mixmonitor_ds->lock);
	mixmonitor_ds_close_fs(mixmonitor->mixmonitor_ds);
	if (!mixmonitor->mixmonitor_ds->destruction_ok) {
		tris_cond_wait(&mixmonitor->mixmonitor_ds->destruction_condition, &mixmonitor->mixmonitor_ds->lock);
	}
	tris_mutex_unlock(&mixmonitor->mixmonitor_ds->lock);

	/* kill the audiohook */
	destroy_monitor_audiohook(mixmonitor);

	if (mixmonitor->post_process) {
		tris_verb(2, "Executing [%s]\n", mixmonitor->post_process);
		tris_safe_system(mixmonitor->post_process);
	}

	tris_verb(2, "End MixMonitor Recording %s\n", mixmonitor->name);
	mixmonitor_free(mixmonitor);
	return NULL;
}

static int setup_mixmonitor_ds(struct mixmonitor *mixmonitor, struct tris_channel *chan)
{
	struct tris_datastore *datastore = NULL;
	struct mixmonitor_ds *mixmonitor_ds;

	if (!(mixmonitor_ds = tris_calloc(1, sizeof(*mixmonitor_ds)))) {
		return -1;
	}

	tris_mutex_init(&mixmonitor_ds->lock);
	tris_cond_init(&mixmonitor_ds->destruction_condition, NULL);

	if (!(datastore = tris_datastore_alloc(&mixmonitor_ds_info, NULL))) {
		tris_mutex_destroy(&mixmonitor_ds->lock);
		tris_cond_destroy(&mixmonitor_ds->destruction_condition);
		tris_free(mixmonitor_ds);
		return -1;
	}

	/* No need to lock mixmonitor_ds since this is still operating in the channel's thread */
	mixmonitor_ds->chan = chan;
	mixmonitor_ds->audiohook = &mixmonitor->audiohook;
	datastore->data = mixmonitor_ds;

	tris_channel_lock(chan);
	tris_channel_datastore_add(chan, datastore);
	tris_channel_unlock(chan);

	mixmonitor->mixmonitor_ds = mixmonitor_ds;
	return 0;
}

static void launch_monitor_thread(struct tris_channel *chan, const char *filename, unsigned int flags,
				  int readvol, int writevol, const char *post_process) 
{
	pthread_t thread;
	struct mixmonitor *mixmonitor;
	char postprocess2[1024] = "";
	size_t len;

	len = sizeof(*mixmonitor) + strlen(chan->name) + strlen(filename) + 2;

	postprocess2[0] = 0;
	/* If a post process system command is given attach it to the structure */
	if (!tris_strlen_zero(post_process)) {
		char *p1, *p2;

		p1 = tris_strdupa(post_process);
		for (p2 = p1; *p2 ; p2++) {
			if (*p2 == '^' && *(p2+1) == '{') {
				*p2 = '$';
			}
		}
		pbx_substitute_variables_helper(chan, p1, postprocess2, sizeof(postprocess2) - 1);
		if (!tris_strlen_zero(postprocess2))
			len += strlen(postprocess2) + 1;
	}

	/* Pre-allocate mixmonitor structure and spy */
	if (!(mixmonitor = tris_calloc(1, len))) {
		return;
	}

	/* Setup the actual spy before creating our thread */
	if (tris_audiohook_init(&mixmonitor->audiohook, TRIS_AUDIOHOOK_TYPE_SPY, mixmonitor_spy_type)) {
		mixmonitor_free(mixmonitor);
		return;
	}

	/* Copy over flags and channel name */
	mixmonitor->flags = flags;
	if (setup_mixmonitor_ds(mixmonitor, chan)) {
		mixmonitor_free(mixmonitor);
		return;
	}
	mixmonitor->name = (char *) mixmonitor + sizeof(*mixmonitor);
	strcpy(mixmonitor->name, chan->name);
	if (!tris_strlen_zero(postprocess2)) {
		mixmonitor->post_process = mixmonitor->name + strlen(mixmonitor->name) + strlen(filename) + 2;
		strcpy(mixmonitor->post_process, postprocess2);
	}

	mixmonitor->filename = (char *) mixmonitor + sizeof(*mixmonitor) + strlen(chan->name) + 1;
	strcpy(mixmonitor->filename, filename);

	tris_set_flag(&mixmonitor->audiohook, TRIS_AUDIOHOOK_TRIGGER_SYNC);

	if (readvol)
		mixmonitor->audiohook.options.read_volume = readvol;
	if (writevol)
		mixmonitor->audiohook.options.write_volume = writevol;

	if (startmon(chan, &mixmonitor->audiohook)) {
		tris_log(LOG_WARNING, "Unable to add '%s' spy to channel '%s'\n",
			mixmonitor_spy_type, chan->name);
		tris_audiohook_destroy(&mixmonitor->audiohook);
		mixmonitor_free(mixmonitor);
		return;
	}

	tris_pthread_create_detached_background(&thread, NULL, mixmonitor_thread, mixmonitor);
}

static int mixmonitor_exec(struct tris_channel *chan, void *data)
{
	int x, readvol = 0, writevol = 0;
	struct tris_flags flags = {0};
	char *parse, *tmp, *slash;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(filename);
		TRIS_APP_ARG(options);
		TRIS_APP_ARG(post_process);
	);
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "MixMonitor requires an argument (filename)\n");
		return -1;
	}

	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);
	
	if (tris_strlen_zero(args.filename)) {
		tris_log(LOG_WARNING, "MixMonitor requires an argument (filename)\n");
		return -1;
	}

	if (args.options) {
		char *opts[OPT_ARG_ARRAY_SIZE] = { NULL, };

		tris_app_parse_options(mixmonitor_opts, &flags, opts, args.options);

		if (tris_test_flag(&flags, MUXFLAG_READVOLUME)) {
			if (tris_strlen_zero(opts[OPT_ARG_READVOLUME])) {
				tris_log(LOG_WARNING, "No volume level was provided for the heard volume ('v') option.\n");
			} else if ((sscanf(opts[OPT_ARG_READVOLUME], "%2d", &x) != 1) || (x < -4) || (x > 4)) {
				tris_log(LOG_NOTICE, "Heard volume must be a number between -4 and 4, not '%s'\n", opts[OPT_ARG_READVOLUME]);
			} else {
				readvol = get_volfactor(x);
			}
		}
		
		if (tris_test_flag(&flags, MUXFLAG_WRITEVOLUME)) {
			if (tris_strlen_zero(opts[OPT_ARG_WRITEVOLUME])) {
				tris_log(LOG_WARNING, "No volume level was provided for the spoken volume ('V') option.\n");
			} else if ((sscanf(opts[OPT_ARG_WRITEVOLUME], "%2d", &x) != 1) || (x < -4) || (x > 4)) {
				tris_log(LOG_NOTICE, "Spoken volume must be a number between -4 and 4, not '%s'\n", opts[OPT_ARG_WRITEVOLUME]);
			} else {
				writevol = get_volfactor(x);
			}
		}
		
		if (tris_test_flag(&flags, MUXFLAG_VOLUME)) {
			if (tris_strlen_zero(opts[OPT_ARG_VOLUME])) {
				tris_log(LOG_WARNING, "No volume level was provided for the combined volume ('W') option.\n");
			} else if ((sscanf(opts[OPT_ARG_VOLUME], "%2d", &x) != 1) || (x < -4) || (x > 4)) {
				tris_log(LOG_NOTICE, "Combined volume must be a number between -4 and 4, not '%s'\n", opts[OPT_ARG_VOLUME]);
			} else {
				readvol = writevol = get_volfactor(x);
			}
		}
	}

	/* if not provided an absolute path, use the system-configured monitoring directory */
	if (args.filename[0] != '/') {
		char *build;

		build = alloca(strlen(tris_config_TRIS_MONITOR_DIR) + strlen(args.filename) + 3);
		sprintf(build, "%s/%s", tris_config_TRIS_MONITOR_DIR, args.filename);
		args.filename = build;
	}

	tmp = tris_strdupa(args.filename);
	if ((slash = strrchr(tmp, '/')))
		*slash = '\0';
	tris_mkdir(tmp, 0777);

	pbx_builtin_setvar_helper(chan, "MIXMONITOR_FILENAME", args.filename);
	launch_monitor_thread(chan, args.filename, flags.flags, readvol, writevol, args.post_process);

	return 0;
}

static int stop_mixmonitor_exec(struct tris_channel *chan, void *data)
{
	struct tris_datastore *datastore = NULL;

	tris_channel_lock(chan);
	tris_audiohook_detach_source(chan, mixmonitor_spy_type);
	if ((datastore = tris_channel_datastore_find(chan, &mixmonitor_ds_info, NULL))) {
		struct mixmonitor_ds *mixmonitor_ds = datastore->data;

		tris_mutex_lock(&mixmonitor_ds->lock);

		/* closing the filestream here guarantees the file is avaliable to the dialplan
	 	 * after calling StopMixMonitor */
		mixmonitor_ds_close_fs(mixmonitor_ds);

		/* The mixmonitor thread may be waiting on the audiohook trigger.
		 * In order to exit from the mixmonitor loop before waiting on channel
		 * destruction, poke the audiohook trigger. */
		if (mixmonitor_ds->audiohook) {
			tris_audiohook_lock(mixmonitor_ds->audiohook);
			tris_cond_signal(&mixmonitor_ds->audiohook->trigger);
			tris_audiohook_unlock(mixmonitor_ds->audiohook);
			mixmonitor_ds->audiohook = NULL;
		}

		tris_mutex_unlock(&mixmonitor_ds->lock);

		/* Remove the datastore so the monitor thread can exit */
		if (!tris_channel_datastore_remove(chan, datastore)) {
			tris_datastore_free(datastore);
		}
	}
	tris_channel_unlock(chan);

	return 0;
}

static char *handle_cli_mixmonitor(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_channel *chan;

	switch (cmd) {
	case CLI_INIT:
		e->command = "mixmonitor {start|stop}";
		e->usage =
			"Usage: mixmonitor <start|stop> <chan_name> [args]\n"
			"       The optional arguments are passed to the MixMonitor\n"
			"       application when the 'start' command is used.\n";
		return NULL;
	case CLI_GENERATE:
		return tris_complete_channels(a->line, a->word, a->pos, a->n, 2);
	}

	if (a->argc < 3)
		return CLI_SHOWUSAGE;

	if (!(chan = tris_get_channel_by_name_prefix_locked(a->argv[2], strlen(a->argv[2])))) {
		tris_cli(a->fd, "No channel matching '%s' found.\n", a->argv[2]);
		/* Technically this is a failure, but we don't want 2 errors printing out */
		return CLI_SUCCESS;
	}

	if (!strcasecmp(a->argv[1], "start")) {
		mixmonitor_exec(chan, a->argv[3]);
		tris_channel_unlock(chan);
	} else {
		tris_channel_unlock(chan);
		tris_audiohook_detach_source(chan, mixmonitor_spy_type);
	}

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_mixmonitor[] = {
	TRIS_CLI_DEFINE(handle_cli_mixmonitor, "Execute a MixMonitor command")
};

static int unload_module(void)
{
	int res;

	tris_cli_unregister_multiple(cli_mixmonitor, ARRAY_LEN(cli_mixmonitor));
	res = tris_unregister_application(stop_app);
	res |= tris_unregister_application(app);
	
	return res;
}

static int load_module(void)
{
	int res;

	tris_cli_register_multiple(cli_mixmonitor, ARRAY_LEN(cli_mixmonitor));
	res = tris_register_application_xml(app, mixmonitor_exec);
	res |= tris_register_application_xml(stop_app, stop_mixmonitor_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Mixed Audio Monitoring Application");
