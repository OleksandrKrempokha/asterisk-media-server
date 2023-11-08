/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Trivial application to playback a sound file
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 220292 $")

#include "trismedia/file.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
/* This file provides config-file based 'say' functions, and implenents
 * some CLI commands.
 */
#include "trismedia/say.h"	/* provides config-file based 'say' functions */
#include "trismedia/cli.h"

/*** DOCUMENTATION
	<application name="Playback" language="en_US">
		<synopsis>
			Play a file.
		</synopsis>
		<syntax>
			<parameter name="filenames" required="true" argsep="&amp;">
				<argument name="filename" required="true" />
				<argument name="filename2" multiple="true" />
			</parameter>
			<parameter name="options">
				<para>Comma separated list of options</para>
				<optionlist>
					<option name="skip">
						<para>Do not play if not answered</para>
					</option>
					<option name="noanswer">
						<para>Playback without answering, otherwise the channel will
						be answered before the sound is played.</para>
						<note><para>Not all channel types support playing messages while still on hook.</para></note>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Plays back given filenames (do not put extension of wav/alaw etc).
			The playback command answer the channel if no options are specified.
			If the file is non-existant it will fail</para>
			<para>This application sets the following channel variable upon completion:</para>
			<variablelist>
				<variable name="PLAYBACKSTATUS">
					<para>The status of the playback attempt as a text string.</para>
					<value name="SUCCESS"/>
					<value name="FAILED"/>
				</variable>
			</variablelist>
			<para>See Also: Background (application) -- for playing sound files that are interruptible</para>
			<para>WaitExten (application) -- wait for digits from caller, optionally play music on hold</para>
		</description>
	</application>
 ***/

static char *app = "Playback";

static struct tris_config *say_cfg = NULL;
/* save the say' api calls.
 * The first entry is NULL if we have the standard source,
 * otherwise we are sourcing from here.
 * 'say load [new|old]' will enable the new or old method, or report status
 */
static const void *say_api_buf[40];
static const char *say_old = "old";
static const char *say_new = "new";

static void save_say_mode(const void *arg)
{
	int i = 0;
	say_api_buf[i++] = arg;

	say_api_buf[i++] = tris_say_number_full;
	say_api_buf[i++] = tris_say_enumeration_full;
	say_api_buf[i++] = tris_say_digit_str_full;
	say_api_buf[i++] = tris_say_character_str_full;
	say_api_buf[i++] = tris_say_phonetic_str_full;
	say_api_buf[i++] = tris_say_datetime;
	say_api_buf[i++] = tris_say_time;
	say_api_buf[i++] = tris_say_date;
	say_api_buf[i++] = tris_say_datetime_from_now;
	say_api_buf[i++] = tris_say_date_with_format;
}

static void restore_say_mode(void *arg)
{
	int i = 0;
	say_api_buf[i++] = arg;

	tris_say_number_full = say_api_buf[i++];
	tris_say_enumeration_full = say_api_buf[i++];
	tris_say_digit_str_full = say_api_buf[i++];
	tris_say_character_str_full = say_api_buf[i++];
	tris_say_phonetic_str_full = say_api_buf[i++];
	tris_say_datetime = say_api_buf[i++];
	tris_say_time = say_api_buf[i++];
	tris_say_date = say_api_buf[i++];
	tris_say_datetime_from_now = say_api_buf[i++];
	tris_say_date_with_format = say_api_buf[i++];
}

/* 
 * Typical 'say' arguments in addition to the date or number or string
 * to say. We do not include 'options' because they may be different
 * in recursive calls, and so they are better left as an external
 * parameter.
 */
typedef struct {
	struct tris_channel *chan;
	const char *ints;
	const char *language;
	int audiofd;
	int ctrlfd;
} say_args_t;

static int s_streamwait3(const say_args_t *a, const char *fn)
{
	int res = tris_streamfile(a->chan, fn, a->language);
	if (res) {
		tris_log(LOG_WARNING, "Unable to play message %s\n", fn);
		return res;
	}
	res = (a->audiofd  > -1 && a->ctrlfd > -1) ?
	tris_waitstream_full(a->chan, a->ints, a->audiofd, a->ctrlfd) :
	tris_waitstream(a->chan, a->ints);
	tris_stopstream(a->chan);
	return res;  
}

/*
 * the string is 'prefix:data' or prefix:fmt:data'
 * with ':' being invalid in strings.
 */
static int do_say(say_args_t *a, const char *s, const char *options, int depth)
{
	struct tris_variable *v;
	char *lang, *x, *rule = NULL;
	int ret = 0;   
	struct varshead head = { .first = NULL, .last = NULL };
	struct tris_var_t *n;

	tris_debug(2, "string <%s> depth <%d>\n", s, depth);
	if (depth++ > 10) {
		tris_log(LOG_WARNING, "recursion too deep, exiting\n");
		return -1;
	} else if (!say_cfg) {
		tris_log(LOG_WARNING, "no say.conf, cannot spell '%s'\n", s);
		return -1;
	}

	/* scan languages same as in file.c */
	if (a->language == NULL)
		a->language = "kp";     /* default */
	tris_debug(2, "try <%s> in <%s>\n", s, a->language);
	lang = tris_strdupa(a->language);
	for (;;) {
		for (v = tris_variable_browse(say_cfg, lang); v ; v = v->next) {
			if (tris_extension_match(v->name, s)) {
				rule = tris_strdupa(v->value);
				break;
			}
		}
		if (rule)
			break;
		if ( (x = strchr(lang, '_')) )
			*x = '\0';      /* try without suffix */
		else if (strcmp(lang, "kp"))
			lang = "kp";    /* last resort, try 'en' if not done yet */
		else
			break;
	}
	if (!rule)
		return 0;

	/* skip up to two prefixes to get the value */
	if ( (x = strchr(s, ':')) )
		s = x + 1;
	if ( (x = strchr(s, ':')) )
		s = x + 1;
	tris_debug(2, "value is <%s>\n", s);
	n = tris_var_assign("SAY", s);
	TRIS_LIST_INSERT_HEAD(&head, n, entries);

	/* scan the body, one piece at a time */
	while ( !ret && (x = strsep(&rule, ",")) ) { /* exit on key */
		char fn[128];
		const char *p, *fmt, *data; /* format and data pointers */

		/* prepare a decent file name */
		x = tris_skip_blanks(x);
		tris_trim_blanks(x);

		/* replace variables */
		pbx_substitute_variables_varshead(&head, x, fn, sizeof(fn));
		tris_debug(2, "doing [%s]\n", fn);

		/* locate prefix and data, if any */
		fmt = strchr(fn, ':');
		if (!fmt || fmt == fn)	{	/* regular filename */
			ret = s_streamwait3(a, fn);
			continue;
		}
		fmt++;
		data = strchr(fmt, ':');	/* colon before data */
		if (!data || data == fmt) {	/* simple prefix-fmt */
			ret = do_say(a, fn, options, depth);
			continue;
		}
		/* prefix:fmt:data */
		for (p = fmt; p < data && ret <= 0; p++) {
			char fn2[sizeof(fn)];
			if (*p == ' ' || *p == '\t')	/* skip blanks */
				continue;
			if (*p == '\'') {/* file name - we trim them */
				char *y;
				strcpy(fn2, tris_skip_blanks(p+1));	/* make a full copy */
				y = strchr(fn2, '\'');
				if (!y) {
					p = data;	/* invalid. prepare to end */
					break;
				}
				*y = '\0';
				tris_trim_blanks(fn2);
				p = strchr(p+1, '\'');
				ret = s_streamwait3(a, fn2);
			} else {
				int l = fmt-fn;
				strcpy(fn2, fn); /* copy everything */
				/* after prefix, append the format */
				fn2[l++] = *p;
				strcpy(fn2 + l, data);
				ret = do_say(a, fn2, options, depth);
			}
			
			if (ret) {
				break;
			}
		}
	}
	tris_var_delete(n);
	return ret;
}

static int say_full(struct tris_channel *chan, const char *string,
	const char *ints, const char *lang, const char *options,
	int audiofd, int ctrlfd)
{
	say_args_t a = { chan, ints, lang, audiofd, ctrlfd };
	return do_say(&a, string, options, 0);
}

static int say_number_full(struct tris_channel *chan, int num,
	const char *ints, const char *lang, const char *options,
	int audiofd, int ctrlfd)
{
	char buf[64];
	say_args_t a = { chan, ints, lang, audiofd, ctrlfd };
	snprintf(buf, sizeof(buf), "num:%d", num);
	return do_say(&a, buf, options, 0);
}

static int say_enumeration_full(struct tris_channel *chan, int num,
	const char *ints, const char *lang, const char *options,
	int audiofd, int ctrlfd)
{
	char buf[64];
	say_args_t a = { chan, ints, lang, audiofd, ctrlfd };
	snprintf(buf, sizeof(buf), "enum:%d", num);
	return do_say(&a, buf, options, 0);
}

static int say_date_generic(struct tris_channel *chan, time_t t,
	const char *ints, const char *lang, const char *format, const char *timezonename, const char *prefix)
{
	char buf[128];
	struct tris_tm tm;
	struct timeval when = { t, 0 };
	say_args_t a = { chan, ints, lang, -1, -1 };
	if (format == NULL)
		format = "";

	tris_localtime(&when, &tm, NULL);
	snprintf(buf, sizeof(buf), "%s:%s:%04d%02d%02d%02d%02d.%02d-%d-%3d",
		prefix,
		format,
		tm.tm_year+1900,
		tm.tm_mon+1,
		tm.tm_mday,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec,
		tm.tm_wday,
		tm.tm_yday);
	return do_say(&a, buf, NULL, 0);
}

static int say_date_with_format(struct tris_channel *chan, time_t t,
	const char *ints, const char *lang, const char *format, const char *timezonename)
{
	return say_date_generic(chan, t, ints, lang, format, timezonename, "datetime");
}

static int say_date(struct tris_channel *chan, time_t t, const char *ints, const char *lang)
{
	return say_date_generic(chan, t, ints, lang, "", NULL, "date");
}

static int say_time(struct tris_channel *chan, time_t t, const char *ints, const char *lang)
{
	return say_date_generic(chan, t, ints, lang, "", NULL, "time");
}

static int say_datetime(struct tris_channel *chan, time_t t, const char *ints, const char *lang)
{
	return say_date_generic(chan, t, ints, lang, "", NULL, "datetime");
}

/*
 * remap the 'say' functions to use those in this file
 */
static int say_init_mode(const char *mode) {
	if (!strcmp(mode, say_new)) {
		if (say_cfg == NULL) {
			tris_log(LOG_ERROR, "There is no say.conf file to use new mode\n");
			return -1;
		}
		save_say_mode(say_new);
		tris_say_number_full = say_number_full;

		tris_say_enumeration_full = say_enumeration_full;
#if 0
		tris_say_digits_full = say_digits_full;
		tris_say_digit_str_full = say_digit_str_full;
		tris_say_character_str_full = say_character_str_full;
		tris_say_phonetic_str_full = say_phonetic_str_full;
		tris_say_datetime_from_now = say_datetime_from_now;
#endif
		tris_say_datetime = say_datetime;
		tris_say_time = say_time;
		tris_say_date = say_date;
		tris_say_date_with_format = say_date_with_format;
	} else if (!strcmp(mode, say_old) && say_api_buf[0] == say_new) {
		restore_say_mode(NULL);
	} else if (strcmp(mode, say_old)) {
		tris_log(LOG_WARNING, "unrecognized mode %s\n", mode);
		return -1;
	}
	
	return 0;
}

static char *__say_cli_init(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	const char *old_mode = say_api_buf[0] ? say_new : say_old;
	char *mode;
	switch (cmd) {
	case CLI_INIT:
		e->command = "say load [new|old]";
		e->usage = 
			"Usage: say load [new|old]\n"
			"       say load\n"
			"           Report status of current say mode\n"
			"       say load new\n"
			"           Set say method, configured in say.conf\n"
			"       say load old\n"
			"           Set old say method, coded in trismedia core\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc == 2) {
		tris_cli(a->fd, "say mode is [%s]\n", old_mode);
		return CLI_SUCCESS;
	} else if (a->argc != e->args)
		return CLI_SHOWUSAGE;
	mode = a->argv[2];
	if (!strcmp(mode, old_mode))
		tris_cli(a->fd, "say mode is %s already\n", mode);
	else
		if (say_init_mode(mode) == 0)
			tris_cli(a->fd, "setting say mode from %s to %s\n", old_mode, mode);

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_playback[] = {
	TRIS_CLI_DEFINE(__say_cli_init, "Set or show the say mode"),
};

static int playback_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	int mres = 0;
	char *tmp;
	int option_skip=0;
	int option_say=0;
	int option_noanswer = 0;

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(filenames);
		TRIS_APP_ARG(options);
	);
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Playback requires an argument (filename)\n");
		return -1;
	}

	tmp = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, tmp);

	if (args.options) {
		if (strcasestr(args.options, "skip"))
			option_skip = 1;
		if (strcasestr(args.options, "say"))
			option_say = 1;
		if (strcasestr(args.options, "noanswer"))
			option_noanswer = 1;
	} 
	if (chan->_state != TRIS_STATE_UP) {
		if (option_skip) {
			/* At the user's option, skip if the line is not up */
			goto done;
		} else if (!option_noanswer) {
			/* Otherwise answer unless we're supposed to send this while on-hook */
			res = tris_answer(chan);
		}
	}
	if (!res) {
		char *back = args.filenames;
		char *front;

		tris_stopstream(chan);
		while (!res && (front = strsep(&back, "&"))) {
			if (option_say)
				res = say_full(chan, front, "", chan->language, NULL, -1, -1);
			else
				res = tris_streamfile(chan, front, chan->language);
			if (!res) { 
				res = tris_waitstream(chan, "");	
				tris_stopstream(chan);
			} else {
				tris_log(LOG_WARNING, "tris_streamfile failed on %s for %s\n", chan->name, (char *)data);
				res = 0;
				mres = 1;
			}
		}
	}
done:
	pbx_builtin_setvar_helper(chan, "PLAYBACKSTATUS", mres ? "FAILED" : "SUCCESS");
	return res;
}

static int reload(void)
{
	struct tris_variable *v;
	struct tris_flags config_flags = { CONFIG_FLAG_FILEUNCHANGED };
	struct tris_config *newcfg;

	if ((newcfg = tris_config_load("say.conf", config_flags)) == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	} else if (newcfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file say.conf is in an invalid format.  Aborting.\n");
		return 0;
	}

	if (say_cfg) {
		tris_config_destroy(say_cfg);
		tris_log(LOG_NOTICE, "Reloading say.conf\n");
		say_cfg = newcfg;
	}

	if (say_cfg) {
		for (v = tris_variable_browse(say_cfg, "general"); v ; v = v->next) {
    			if (tris_extension_match(v->name, "mode")) {
				say_init_mode(v->value);
				break;
			}
		}
	}
	
	/*
	 * XXX here we should sort rules according to the same order
	 * we have in pbx.c so we have the same matching behaviour.
	 */
	return 0;
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app);

	tris_cli_unregister_multiple(cli_playback, ARRAY_LEN(cli_playback));

	if (say_cfg)
		tris_config_destroy(say_cfg);

	return res;	
}

static int load_module(void)
{
	struct tris_variable *v;
	struct tris_flags config_flags = { 0 };

	say_cfg = tris_config_load("say.conf", config_flags);
	if (say_cfg && say_cfg != CONFIG_STATUS_FILEINVALID) {
		for (v = tris_variable_browse(say_cfg, "general"); v ; v = v->next) {
    			if (tris_extension_match(v->name, "mode")) {
				say_init_mode(v->value);
				break;
			}
		}
	}

	tris_cli_register_multiple(cli_playback, ARRAY_LEN(cli_playback));
	return tris_register_application_xml(app, playback_exec);
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Sound File Playback Application",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
