/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Kevin P. Fleming <kpfleming@digium.com>
 *
 * Portions taken from the file-based music-on-hold work
 * created by Anthony Minessale II in res_musiconhold.c
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
 * \brief External IVR application interface
 *
 * \author Kevin P. Fleming <kpfleming@digium.com>
 *
 * \note Portions taken from the file-based music-on-hold work
 * created by Anthony Minessale II in res_musiconhold.c
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 232813 $")

#include <signal.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/linkedlists.h"
#include "trismedia/app.h"
#include "trismedia/utils.h"
#include "trismedia/tcptls.h"
#include "trismedia/astobj2.h"

static const char *app = "ExternalIVR";

static const char *synopsis = "Interfaces with an external IVR application";
static const char *descrip =
"  ExternalIVR(command|ivr://ivrhosti([,arg[,arg...]])[,options]): Either forks a process\n"
"to run given command or makes a socket to connect to given host and starts\n"
"a generator on the channel. The generator's play list is controlled by the\n"
"external application, which can add and clear entries via simple commands\n"
"issued over its stdout. The external application will receive all DTMF events\n"
"received on the channel, and notification if the channel is hung up. The\n"
"application will not be forcibly terminated when the channel is hung up.\n"
"See doc/externalivr.txt for a protocol specification.\n"
"The 'n' option tells ExternalIVR() not to answer the channel. \n"
"The 'i' option tells ExternalIVR() not to send a hangup and exit when the\n"
"  channel receives a hangup, instead it sends an 'I' informative message\n"
"  meaning that the external application MUST hang up the call with an H command\n"
"The 'd' option tells ExternalIVR() to run on a channel that has been hung up\n"
"  and will not look for hangups.  The external application must exit with\n"
"  an 'E' command.\n";

/* XXX the parser in gcc 2.95 gets confused if you don't put a space between 'name' and the comma */
#define tris_chan_log(level, channel, format, ...) tris_log(level, "%s: " format, channel->name , ## __VA_ARGS__)

enum {
	noanswer = (1 << 0),
	ignore_hangup = (1 << 1),
	run_dead = (1 << 2),
} options_flags;

TRIS_APP_OPTIONS(app_opts, {
	TRIS_APP_OPTION('n', noanswer),
	TRIS_APP_OPTION('i', ignore_hangup),
	TRIS_APP_OPTION('d', run_dead),
});

struct playlist_entry {
	TRIS_LIST_ENTRY(playlist_entry) list;
	char filename[1];
};

struct ivr_localuser {
	struct tris_channel *chan;
	TRIS_LIST_HEAD(playlist, playlist_entry) playlist;
	TRIS_LIST_HEAD(finishlist, playlist_entry) finishlist;
	int abort_current_sound;
	int playing_silence;
	int option_autoclear;
	int gen_active;
};


struct gen_state {
	struct ivr_localuser *u;
	struct tris_filestream *stream;
	struct playlist_entry *current;
	int sample_queue;
};

static int eivr_comm(struct tris_channel *chan, struct ivr_localuser *u, 
	int *eivr_events_fd, int *eivr_commands_fd, int *eivr_errors_fd, 
	const struct tris_str *args, const struct tris_flags flags);

int eivr_connect_socket(struct tris_channel *chan, const char *host, int port);

static void send_eivr_event(FILE *handle, const char event, const char *data,
	const struct tris_channel *chan)
{
	struct tris_str *tmp = tris_str_create(12);

	tris_str_append(&tmp, 0, "%c,%10d", event, (int)time(NULL));
	if (data) {
		tris_str_append(&tmp, 0, ",%s", data);
	}

	fprintf(handle, "%s\n", tris_str_buffer(tmp));
	tris_debug(1, "sent '%s'\n", tris_str_buffer(tmp));
}

static void *gen_alloc(struct tris_channel *chan, void *params)
{
	struct ivr_localuser *u = params;
	struct gen_state *state;

	if (!(state = tris_calloc(1, sizeof(*state))))
		return NULL;

	state->u = u;

	return state;
}

static void gen_closestream(struct gen_state *state)
{
	if (!state->stream)
		return;

	tris_closestream(state->stream);
	state->u->chan->stream = NULL;
	state->stream = NULL;
}

static void gen_release(struct tris_channel *chan, void *data)
{
	struct gen_state *state = data;

	gen_closestream(state);
	tris_free(data);
}

/* caller has the playlist locked */
static int gen_nextfile(struct gen_state *state)
{
	struct ivr_localuser *u = state->u;
	char *file_to_stream;

	u->abort_current_sound = 0;
	u->playing_silence = 0;
	gen_closestream(state);

	while (!state->stream) {
		state->current = TRIS_LIST_REMOVE_HEAD(&u->playlist, list);
		if (state->current) {
			file_to_stream = state->current->filename;
		} else {
			file_to_stream = "silence/10";
			u->playing_silence = 1;
		}

		if (!(state->stream = tris_openstream_full(u->chan, file_to_stream, u->chan->language, 1))) {
			tris_chan_log(LOG_WARNING, u->chan, "File '%s' could not be opened: %s\n", file_to_stream, strerror(errno));
			if (!u->playing_silence) {
				continue;
			} else {
				break;
			}
		}
	}

	return (!state->stream);
}

static struct tris_frame *gen_readframe(struct gen_state *state)
{
	struct tris_frame *f = NULL;
	struct ivr_localuser *u = state->u;

	if (u->abort_current_sound ||
		(u->playing_silence && TRIS_LIST_FIRST(&u->playlist))) {
		gen_closestream(state);
		TRIS_LIST_LOCK(&u->playlist);
		gen_nextfile(state);
		TRIS_LIST_UNLOCK(&u->playlist);
	}

	if (!(state->stream && (f = tris_readframe(state->stream)))) {
		if (state->current) {
			TRIS_LIST_LOCK(&u->finishlist);
			TRIS_LIST_INSERT_TAIL(&u->finishlist, state->current, list);
			TRIS_LIST_UNLOCK(&u->finishlist);
			state->current = NULL;
		}
		if (!gen_nextfile(state))
			f = tris_readframe(state->stream);
	}

	return f;
}

static int gen_generate(struct tris_channel *chan, void *data, int len, int samples)
{
	struct gen_state *state = data;
	struct tris_frame *f = NULL;
	int res = 0;

	state->sample_queue += samples;

	while (state->sample_queue > 0) {
		if (!(f = gen_readframe(state)))
			return -1;

		res = tris_write(chan, f);
		tris_frfree(f);
		if (res < 0) {
			tris_chan_log(LOG_WARNING, chan, "Failed to write frame: %s\n", strerror(errno));
			return -1;
		}
		state->sample_queue -= f->samples;
	}

	return res;
}

static struct tris_generator gen =
{
	alloc: gen_alloc,
	release: gen_release,
	generate: gen_generate,
};

static void tris_eivr_getvariable(struct tris_channel *chan, char *data, char *outbuf, int outbuflen)
{
	/* original input data: "G,var1,var2," */
	/* data passed as "data":  "var1,var2" */

	char *inbuf, *variable;
	const char *value;
	int j;
	struct tris_str *newstring = tris_str_alloca(outbuflen); 

	outbuf[0] = '\0';

	for (j = 1, inbuf = data; ; j++) {
		variable = strsep(&inbuf, ",");
		if (variable == NULL) {
			int outstrlen = strlen(outbuf);
			if (outstrlen && outbuf[outstrlen - 1] == ',') {
				outbuf[outstrlen - 1] = 0;
			}
			break;
		}
		
		tris_channel_lock(chan);
		if (!(value = pbx_builtin_getvar_helper(chan, variable))) {
			value = "";
		}

		tris_str_append(&newstring, 0, "%s=%s,", variable, value);
		tris_channel_unlock(chan);
		tris_copy_string(outbuf, tris_str_buffer(newstring), outbuflen);
	}
}

static void tris_eivr_setvariable(struct tris_channel *chan, char *data)
{
	char *value;

	char *inbuf = tris_strdupa(data), *variable;

	for (variable = strsep(&inbuf, ","); variable; variable = strsep(&inbuf, ",")) {
		tris_debug(1, "Setting up a variable: %s\n", variable);
		/* variable contains "varname=value" */
		value = strchr(variable, '=');
		if (!value) {
			value = "";
		} else {
			*value++ = '\0';
		}
		pbx_builtin_setvar_helper(chan, variable, value);
	}
}

static struct playlist_entry *make_entry(const char *filename)
{
	struct playlist_entry *entry;

	if (!(entry = tris_calloc(1, sizeof(*entry) + strlen(filename) + 10))) /* XXX why 10 ? */
		return NULL;

	strcpy(entry->filename, filename);

	return entry;
}

static int app_exec(struct tris_channel *chan, void *data)
{
	struct tris_flags flags = { 0, };
	char *opts[0];
	struct playlist_entry *entry;
	int child_stdin[2] = { -1, -1 };
	int child_stdout[2] = { -1, -1 };
	int child_stderr[2] = { -1, -1 };
	int res = -1;
	int pid;

	char hostname[1024];
	char *port_str = NULL;
	int port = 0;
	struct tris_tcptls_session_instance *ser = NULL;

	struct ivr_localuser foo = {
		.playlist = TRIS_LIST_HEAD_INIT_VALUE,
		.finishlist = TRIS_LIST_HEAD_INIT_VALUE,
		.gen_active = 0,
	};
	struct ivr_localuser *u = &foo;

	char *buf;
	int j;
	char *s, **app_args, *e; 
	struct tris_str *pipe_delim_args = tris_str_create(100);

	TRIS_DECLARE_APP_ARGS(eivr_args,
		TRIS_APP_ARG(cmd)[32];
	);
	TRIS_DECLARE_APP_ARGS(application_args,
		TRIS_APP_ARG(cmd)[32];
	);

	u->abort_current_sound = 0;
	u->chan = chan;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "ExternalIVR requires a command to execute\n");
		return -1;
	}

	buf = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(eivr_args, buf);

	if ((s = strchr(eivr_args.cmd[0], '('))) {
		s[0] = ',';
		if (( e = strrchr(s, ')')) ) {
			*e = '\0';
		} else {
			tris_log(LOG_ERROR, "Parse error, no closing paren?\n");
		}
		TRIS_STANDARD_APP_ARGS(application_args, eivr_args.cmd[0]);
		app_args = application_args.argv;

		/* Put the application + the arguments in a | delimited list */
		tris_str_reset(pipe_delim_args);
		for (j = 0; application_args.cmd[j] != NULL; j++) {
			tris_str_append(&pipe_delim_args, 0, "%s%s", j == 0 ? "" : ",", application_args.cmd[j]);
		}

		/* Parse the ExternalIVR() arguments */
		if (option_debug)
			tris_debug(1, "Parsing options from: [%s]\n", eivr_args.cmd[1]);
		tris_app_parse_options(app_opts, &flags, opts, eivr_args.cmd[1]);
		if (option_debug) {
			if (tris_test_flag(&flags, noanswer))
				tris_debug(1, "noanswer is set\n");
			if (tris_test_flag(&flags, ignore_hangup))
				tris_debug(1, "ignore_hangup is set\n");
			if (tris_test_flag(&flags, run_dead))
				tris_debug(1, "run_dead is set\n");
		}

	} else {
		app_args = eivr_args.argv;
		for (j = 0; eivr_args.cmd[j] != NULL; j++) {
			tris_str_append(&pipe_delim_args, 0, "%s%s", j == 0 ? "" : "|", eivr_args.cmd[j]);
		}
	}
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "ExternalIVR requires a command to execute\n");
		return -1;
	}

	if (!(tris_test_flag(&flags, noanswer))) {
		tris_chan_log(LOG_WARNING, chan, "Answering channel and starting generator\n");
		if (chan->_state != TRIS_STATE_UP) {
			if (tris_test_flag(&flags, run_dead)) {
				tris_chan_log(LOG_WARNING, chan, "Running ExternalIVR with 'd'ead flag on non-hungup channel isn't supported\n");
				goto exit;
			}
			tris_answer(chan);
		}
		if (tris_activate_generator(chan, &gen, u) < 0) {
			tris_chan_log(LOG_WARNING, chan, "Failed to activate generator\n");
			goto exit;
		} else {
			u->gen_active = 1;
		}
	}

	if (!strncmp(app_args[0], "ivr://", 6)) {
		struct tris_tcptls_session_args ivr_desc = {
			.accept_fd = -1,
			.name = "IVR",
		};
		struct tris_hostent hp;

		/*communicate through socket to server*/
		tris_debug(1, "Parsing hostname:port for socket connect from \"%s\"\n", app_args[0]);
		tris_copy_string(hostname, app_args[0] + 6, sizeof(hostname));
		if ((port_str = strchr(hostname, ':')) != NULL) {
			port_str[0] = 0;
			port_str += 1;
			port = atoi(port_str);
		}
		if (!port) {
			port = 2949;  /* default port, if one is not provided */
		}

		tris_gethostbyname(hostname, &hp);
		ivr_desc.local_address.sin_family = AF_INET;
		ivr_desc.local_address.sin_port = htons(port);
		memcpy(&ivr_desc.local_address.sin_addr.s_addr, hp.hp.h_addr, hp.hp.h_length);
		if (!(ser = tris_tcptls_client_create(&ivr_desc)) || !(ser = tris_tcptls_client_start(ser))) {
			goto exit;
		}
		res = eivr_comm(chan, u, &ser->fd, &ser->fd, NULL, pipe_delim_args, flags);

	} else {
		if (pipe(child_stdin)) {
			tris_chan_log(LOG_WARNING, chan, "Could not create pipe for child input: %s\n", strerror(errno));
			goto exit;
		}
		if (pipe(child_stdout)) {
			tris_chan_log(LOG_WARNING, chan, "Could not create pipe for child output: %s\n", strerror(errno));
			goto exit;
		}
		if (pipe(child_stderr)) {
			tris_chan_log(LOG_WARNING, chan, "Could not create pipe for child errors: %s\n", strerror(errno));
			goto exit;
		}
	
		pid = tris_safe_fork(0);
		if (pid < 0) {
			tris_log(LOG_WARNING, "Failed to fork(): %s\n", strerror(errno));
			goto exit;
		}
	
		if (!pid) {
			/* child process */
			if (tris_opt_high_priority)
				tris_set_priority(0);
	
			dup2(child_stdin[0], STDIN_FILENO);
			dup2(child_stdout[1], STDOUT_FILENO);
			dup2(child_stderr[1], STDERR_FILENO);
			tris_close_fds_above_n(STDERR_FILENO);
			execv(app_args[0], app_args);
			fprintf(stderr, "Failed to execute '%s': %s\n", app_args[0], strerror(errno));
			_exit(1);
		} else {
			/* parent process */
			close(child_stdin[0]);
			child_stdin[0] = -1;
			close(child_stdout[1]);
			child_stdout[1] = -1;
			close(child_stderr[1]);
			child_stderr[1] = -1;
			res = eivr_comm(chan, u, &child_stdin[1], &child_stdout[0], &child_stderr[0], pipe_delim_args, flags);
		}
	}

	exit:
	if (u->gen_active) {
		tris_deactivate_generator(chan);
	}
	if (child_stdin[0] > -1) {
		close(child_stdin[0]);
	}
	if (child_stdin[1] > -1) {
		close(child_stdin[1]);
	}
	if (child_stdout[0] > -1) {
		close(child_stdout[0]);
	}
	if (child_stdout[1] > -1) {
		close(child_stdout[1]);
	}
	if (child_stderr[0] > -1) {
		close(child_stderr[0]);
	}
	if (child_stderr[1] > -1) {
		close(child_stderr[1]);
	}
	if (ser) {
		ao2_ref(ser, -1);
	}
	while ((entry = TRIS_LIST_REMOVE_HEAD(&u->playlist, list))) {
		tris_free(entry);
	}
	return res;
}

static int eivr_comm(struct tris_channel *chan, struct ivr_localuser *u, 
 				int *eivr_events_fd, int *eivr_commands_fd, int *eivr_errors_fd, 
 				const struct tris_str *args, const struct tris_flags flags)
{
	struct playlist_entry *entry;
	struct tris_frame *f;
	int ms;
 	int exception;
 	int ready_fd;
	int waitfds[2] = { *eivr_commands_fd, (eivr_errors_fd) ? *eivr_errors_fd : -1 };
 	struct tris_channel *rchan;
 	char *command;
 	int res = -1;
	int test_available_fd = -1;
	int hangup_info_sent = 0;
  
 	FILE *eivr_commands = NULL;
 	FILE *eivr_errors = NULL;
 	FILE *eivr_events = NULL;

	if (!(eivr_events = fdopen(*eivr_events_fd, "w"))) {
		tris_chan_log(LOG_WARNING, chan, "Could not open stream to send events\n");
		goto exit;
	}
	if (!(eivr_commands = fdopen(*eivr_commands_fd, "r"))) {
		tris_chan_log(LOG_WARNING, chan, "Could not open stream to receive commands\n");
		goto exit;
	}
	if (eivr_errors_fd) {  /* if opening a socket connection, error stream will not be used */
 		if (!(eivr_errors = fdopen(*eivr_errors_fd, "r"))) {
 			tris_chan_log(LOG_WARNING, chan, "Could not open stream to receive errors\n");
 			goto exit;
 		}
	}

	test_available_fd = open("/dev/null", O_RDONLY);
 
 	setvbuf(eivr_events, NULL, _IONBF, 0);
 	setvbuf(eivr_commands, NULL, _IONBF, 0);
 	if (eivr_errors) {
		setvbuf(eivr_errors, NULL, _IONBF, 0);
	}

	res = 0;
 
 	while (1) {
 		if (tris_test_flag(chan, TRIS_FLAG_ZOMBIE)) {
 			tris_chan_log(LOG_NOTICE, chan, "Is a zombie\n");
 			res = -1;
 			break;
 		}
 		if (!hangup_info_sent && !(tris_test_flag(&flags, run_dead)) && tris_check_hangup(chan)) {
			if (tris_test_flag(&flags, ignore_hangup)) {
				tris_chan_log(LOG_NOTICE, chan, "Got check_hangup, but ignore_hangup set so sending 'I' command\n");
				send_eivr_event(eivr_events, 'I', "HANGUP", chan);
				hangup_info_sent = 1;
			} else {
 				tris_chan_log(LOG_NOTICE, chan, "Got check_hangup\n");
 				send_eivr_event(eivr_events, 'H', NULL, chan);
 				res = -1;
	 			break;
			}
 		}
 
 		ready_fd = 0;
 		ms = 100;
 		errno = 0;
 		exception = 0;
 
 		rchan = tris_waitfor_nandfds(&chan, 1, waitfds, (eivr_errors_fd) ? 2 : 1, &exception, &ready_fd, &ms);
 
 		if (chan->_state == TRIS_STATE_UP && !TRIS_LIST_EMPTY(&u->finishlist)) {
 			TRIS_LIST_LOCK(&u->finishlist);
 			while ((entry = TRIS_LIST_REMOVE_HEAD(&u->finishlist, list))) {
 				send_eivr_event(eivr_events, 'F', entry->filename, chan);
 				tris_free(entry);
 			}
 			TRIS_LIST_UNLOCK(&u->finishlist);
 		}
 
 		if (chan->_state == TRIS_STATE_UP && !(tris_check_hangup(chan)) && rchan) {
 			/* the channel has something */
 			f = tris_read(chan);
 			if (!f) {
 				tris_chan_log(LOG_NOTICE, chan, "Returned no frame\n");
 				send_eivr_event(eivr_events, 'H', NULL, chan);
 				res = -1;
 				break;
 			}
 			if (f->frametype == TRIS_FRAME_DTMF) {
 				send_eivr_event(eivr_events, f->subclass, NULL, chan);
 				if (u->option_autoclear) {
  					if (!u->abort_current_sound && !u->playing_silence)
 						send_eivr_event(eivr_events, 'T', NULL, chan);
  					TRIS_LIST_LOCK(&u->playlist);
  					while ((entry = TRIS_LIST_REMOVE_HEAD(&u->playlist, list))) {
 						send_eivr_event(eivr_events, 'D', entry->filename, chan);
  						tris_free(entry);
  					}
  					if (!u->playing_silence)
  						u->abort_current_sound = 1;
  					TRIS_LIST_UNLOCK(&u->playlist);
  				}
 			} else if ((f->frametype == TRIS_FRAME_CONTROL) && (f->subclass == TRIS_CONTROL_HANGUP)) {
 				tris_chan_log(LOG_NOTICE, chan, "Got TRIS_CONTROL_HANGUP\n");
 				send_eivr_event(eivr_events, 'H', NULL, chan);
				if (f->data.uint32) {
					chan->hangupcause = f->data.uint32;
				}
 				tris_frfree(f);
 				res = -1;
 				break;
 			}
 			tris_frfree(f);
 		} else if (ready_fd == *eivr_commands_fd) {
 			char input[1024];
 
 			if (exception || (dup2(*eivr_commands_fd, test_available_fd) == -1) || feof(eivr_commands)) {
 				tris_chan_log(LOG_WARNING, chan, "Child process went away\n");
 				res = -1;
  				break;
  			}
  
 			if (!fgets(input, sizeof(input), eivr_commands))
 				continue;
 
 			command = tris_strip(input);
  
 			if (option_debug)
 				tris_debug(1, "got command '%s'\n", input);
  
 			if (strlen(input) < 4)
 				continue;
  
			if (input[0] == 'P') {
				struct tris_str *tmp = (struct tris_str *) args;
 				send_eivr_event(eivr_events, 'P', tris_str_buffer(tmp), chan);
			} else if ( input[0] == 'T' ) {
				tris_chan_log(LOG_WARNING, chan, "Answering channel if needed and starting generator\n");
				if (chan->_state != TRIS_STATE_UP) {
					if (tris_test_flag(&flags, run_dead)) {
						tris_chan_log(LOG_WARNING, chan, "Running ExternalIVR with 'd'ead flag on non-hungup channel isn't supported\n");
						send_eivr_event(eivr_events, 'Z', "ANSWER_FAILURE", chan);
						continue;
					}
					tris_answer(chan);
				}
				if (!(u->gen_active)) {
					if (tris_activate_generator(chan, &gen, u) < 0) {
						tris_chan_log(LOG_WARNING, chan, "Failed to activate generator\n");
						send_eivr_event(eivr_events, 'Z', "GENERATOR_FAILURE", chan);
					} else {
						u->gen_active = 1;
					}
				}
 			} else if (input[0] == 'S') {
				if (chan->_state != TRIS_STATE_UP || tris_check_hangup(chan)) {
					tris_chan_log(LOG_WARNING, chan, "Queue 'S'et called on unanswered channel\n");
					send_eivr_event(eivr_events, 'Z', NULL, chan);
					continue;
				}
 				if (tris_fileexists(&input[2], NULL, u->chan->language) == -1) {
 					tris_chan_log(LOG_WARNING, chan, "Unknown file requested '%s'\n", &input[2]);
 					send_eivr_event(eivr_events, 'Z', NULL, chan);
 					strcpy(&input[2], "exception");
 				}
 				if (!u->abort_current_sound && !u->playing_silence)
 					send_eivr_event(eivr_events, 'T', NULL, chan);
 				TRIS_LIST_LOCK(&u->playlist);
 				while ((entry = TRIS_LIST_REMOVE_HEAD(&u->playlist, list))) {
 					send_eivr_event(eivr_events, 'D', entry->filename, chan);
 					tris_free(entry);
 				}
 				if (!u->playing_silence)
 					u->abort_current_sound = 1;
 				entry = make_entry(&input[2]);
 				if (entry)
 					TRIS_LIST_INSERT_TAIL(&u->playlist, entry, list);
 				TRIS_LIST_UNLOCK(&u->playlist);
 			} else if (input[0] == 'A') {
				if (chan->_state != TRIS_STATE_UP || tris_check_hangup(chan)) {
					tris_chan_log(LOG_WARNING, chan, "Queue 'A'ppend called on unanswered channel\n");
					send_eivr_event(eivr_events, 'Z', NULL, chan);
					continue;
				}
 				if (tris_fileexists(&input[2], NULL, u->chan->language) == -1) {
 					tris_chan_log(LOG_WARNING, chan, "Unknown file requested '%s'\n", &input[2]);
 					send_eivr_event(eivr_events, 'Z', NULL, chan);
 					strcpy(&input[2], "exception");
 				}
 				entry = make_entry(&input[2]);
 				if (entry) {
 					TRIS_LIST_LOCK(&u->playlist);
 					TRIS_LIST_INSERT_TAIL(&u->playlist, entry, list);
 					TRIS_LIST_UNLOCK(&u->playlist);
 				}
 			} else if (input[0] == 'G') {
 				/* A get variable message:  "G,variable1,variable2,..." */
 				char response[2048];

 				tris_chan_log(LOG_NOTICE, chan, "Getting a Variable out of the channel: %s\n", &input[2]);
 				tris_eivr_getvariable(chan, &input[2], response, sizeof(response));
 				send_eivr_event(eivr_events, 'G', response, chan);
 			} else if (input[0] == 'V') {
 				/* A set variable message:  "V,variablename=foo" */
 				tris_chan_log(LOG_NOTICE, chan, "Setting a Variable up: %s\n", &input[2]);
 				tris_eivr_setvariable(chan, &input[2]);
 			} else if (input[0] == 'L') {
 				tris_chan_log(LOG_NOTICE, chan, "Log message from EIVR: %s\n", &input[2]);
 			} else if (input[0] == 'X') {
 				tris_chan_log(LOG_NOTICE, chan, "Exiting ExternalIVR: %s\n", &input[2]);
 				/*! \todo add deprecation debug message for X command here */
 				res = 0;
 				break;
			} else if (input[0] == 'E') {
 				tris_chan_log(LOG_NOTICE, chan, "Exiting: %s\n", &input[2]);
 				send_eivr_event(eivr_events, 'E', NULL, chan);
 				res = 0;
 				break;
 			} else if (input[0] == 'H') {
 				tris_chan_log(LOG_NOTICE, chan, "Hanging up: %s\n", &input[2]);
 				send_eivr_event(eivr_events, 'H', NULL, chan);
 				res = -1;
 				break;
 			} else if (input[0] == 'O') {
				if (chan->_state != TRIS_STATE_UP || tris_check_hangup(chan)) {
					tris_chan_log(LOG_WARNING, chan, "Option called on unanswered channel\n");
					send_eivr_event(eivr_events, 'Z', NULL, chan);
					continue;
				}
 				if (!strcasecmp(&input[2], "autoclear"))
 					u->option_autoclear = 1;
 				else if (!strcasecmp(&input[2], "noautoclear"))
 					u->option_autoclear = 0;
 				else
 					tris_chan_log(LOG_WARNING, chan, "Unknown option requested '%s'\n", &input[2]);
 			}
 		} else if (eivr_errors_fd && (ready_fd == *eivr_errors_fd)) {
 			char input[1024];
  
 			if (exception || feof(eivr_errors)) {
 				tris_chan_log(LOG_WARNING, chan, "Child process went away\n");
 				res = -1;
 				break;
 			}
 			if (fgets(input, sizeof(input), eivr_errors)) {
 				command = tris_strip(input);
 				tris_chan_log(LOG_NOTICE, chan, "stderr: %s\n", command);
 			}
 		} else if ((ready_fd < 0) && ms) { 
 			if (errno == 0 || errno == EINTR)
 				continue;
 
 			tris_chan_log(LOG_WARNING, chan, "Wait failed (%s)\n", strerror(errno));
 			break;
 		}
 	}
 
	exit:
	if (test_available_fd > -1) {
		close(test_available_fd);
	}
	if (eivr_events) {
 		fclose(eivr_events);
		*eivr_events_fd = -1;
	}
	if (eivr_commands) {
		fclose(eivr_commands);
		*eivr_commands_fd = -1;
	}
	if (eivr_errors) {
		fclose(eivr_errors);
		*eivr_errors_fd = -1;
	}
  	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application(app, app_exec, synopsis, descrip);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "External IVR Interface Application");
