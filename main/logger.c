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
 * \brief Trismedia Logger
 * 
 * Logging routines
 *
 * \author Mark Spencer <markster@digium.com>
 */

/*
 * define _TRISMEDIA_LOGGER_H to prevent the inclusion of logger.h;
 * it redefines LOG_* which we need to define syslog_level_map.
 * later, we force the inclusion of logger.h again.
 */
#define _TRISMEDIA_LOGGER_H
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 248643 $")

/*
 * WARNING: additional #include directives should NOT be placed here, they 
 * should be placed AFTER '#undef _TRISMEDIA_LOGGER_H' below
 */
#include "trismedia/_private.h"
#include "trismedia/paths.h"	/* use tris_config_TRIS_LOG_DIR */
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_BKTR
#include <execinfo.h>
#define MAX_BACKTRACE_FRAMES 20
#endif

#define SYSLOG_NAMES /* so we can map syslog facilities names to their numeric values,
		        from <syslog.h> which is included by logger.h */
#include <syslog.h>

static int syslog_level_map[] = {
	LOG_DEBUG,
	LOG_INFO,    /* arbitrary equivalent of LOG_EVENT */
	LOG_NOTICE,
	LOG_WARNING,
	LOG_ERR,
	LOG_DEBUG,
	LOG_DEBUG
};

#define SYSLOG_NLEVELS sizeof(syslog_level_map) / sizeof(int)

#undef _TRISMEDIA_LOGGER_H	/* now include logger.h */
#include "trismedia/logger.h"
#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/term.h"
#include "trismedia/cli.h"
#include "trismedia/utils.h"
#include "trismedia/manager.h"
#include "trismedia/threadstorage.h"
#include "trismedia/strings.h"
#include "trismedia/pbx.h"
#include "trismedia/app.h"

#if defined(__linux__) && !defined(__NR_gettid)
#include <asm/unistd.h>
#endif

#if defined(__linux__) && defined(__NR_gettid)
#define GETTID() syscall(__NR_gettid)
#else
#define GETTID() getpid()
#endif

static char dateformat[256] = "%b %e %T";		/* Original Trismedia Format */

static char queue_log_name[256] = QUEUELOG;
static char exec_after_rotate[256] = "";

static int filesize_reload_needed;
static int global_logmask = -1;

static float maxlogsize=100;

enum rotatestrategy {
	SEQUENTIAL = 1 << 0,     /* Original method - create a new file, in order */
	ROTATE = 1 << 1,         /* Rotate all files, such that the oldest file has the highest suffix */
	TIMESTAMP = 1 << 2,      /* Append the epoch timestamp onto the end of the archived file */
	BACKUP = 1 << 3,		/* create <logfilename>.0 */
} rotatestrategy = BACKUP;

static struct {
	unsigned int queue_log:1;
	unsigned int event_log:1;
} logfiles = { 1, 1 };

static char hostname[MAXHOSTNAMELEN];

enum logtypes {
	LOGTYPE_SYSLOG,
	LOGTYPE_FILE,
	LOGTYPE_CONSOLE,
};

struct logchannel {
	int logmask;			/* What to log to this channel */
	int disabled;			/* If this channel is disabled or not */
	int facility; 			/* syslog facility */
	enum logtypes type;		/* Type of log channel */
	FILE *fileptr;			/* logfile logging file pointer */
	char filename[256];		/* Filename */
	TRIS_LIST_ENTRY(logchannel) list;
};

static TRIS_RWLIST_HEAD_STATIC(logchannels, logchannel);

enum logmsgtypes {
	LOGMSG_NORMAL = 0,
	LOGMSG_VERBOSE,
};

struct logmsg {
	enum logmsgtypes type;
	char date[256];
	int level;
	char file[80];
	int line;
	char function[80];
	long process_id;
	TRIS_LIST_ENTRY(logmsg) list;
	char str[0];
};

static TRIS_LIST_HEAD_STATIC(logmsgs, logmsg);
static pthread_t logthread = TRIS_PTHREADT_NULL;
static tris_cond_t logcond;
static int close_logger_thread = 0;

static FILE *eventlog;
static FILE *qlog;

/*! \brief Logging channels used in the Trismedia logging system */
static char *levels[] = {
	"DEBUG",
	"EVENT",
	"NOTICE",
	"WARNING",
	"ERROR",
	"VERBOSE",
	"DTMF",
	"TRACE"
};

/*! \brief Colors used in the console for logging */
static int colors[] = {
	COLOR_BRGREEN,
	COLOR_BRBLUE,
	COLOR_YELLOW,
	COLOR_BRRED,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_BRGREEN
};

TRIS_THREADSTORAGE(verbose_buf);
#define VERBOSE_BUF_INIT_SIZE   256

TRIS_THREADSTORAGE(log_buf);
#define LOG_BUF_INIT_SIZE       256

static int make_components(const char *s, int lineno)
{
	char *w;
	int res = 0;
	char *stringp = tris_strdupa(s);

	while ((w = strsep(&stringp, ","))) {
		w = tris_skip_blanks(w);
		if (!strcasecmp(w, "error")) 
			res |= (1 << __LOG_ERROR);
		else if (!strcasecmp(w, "warning"))
			res |= (1 << __LOG_WARNING);
		else if (!strcasecmp(w, "notice"))
			res |= (1 << __LOG_NOTICE);
		else if (!strcasecmp(w, "event"))
			res |= (1 << __LOG_EVENT);
		else if (!strcasecmp(w, "debug"))
			res |= (1 << __LOG_DEBUG);
		else if (!strcasecmp(w, "verbose"))
			res |= (1 << __LOG_VERBOSE);
		else if (!strcasecmp(w, "dtmf"))
			res |= (1 << __LOG_DTMF);
		else if (!strcasecmp(w, "trace"))
			res |= (1 << __LOG_TRACE);
		else {
			fprintf(stderr, "Logfile Warning: Unknown keyword '%s' at line %d of logger.conf\n", w, lineno);
		}
	}

	return res;
}

static struct logchannel *make_logchannel(const char *channel, const char *components, int lineno)
{
	struct logchannel *chan;
	char *facility;
#ifndef SOLARIS
	CODE *cptr;
#endif

	if (tris_strlen_zero(channel) || !(chan = tris_calloc(1, sizeof(*chan))))
		return NULL;

	if (!strcasecmp(channel, "console")) {
		chan->type = LOGTYPE_CONSOLE;
	} else if (!strncasecmp(channel, "syslog", 6)) {
		/*
		* syntax is:
		*  syslog.facility => level,level,level
		*/
		facility = strchr(channel, '.');
		if (!facility++ || !facility) {
			facility = "local0";
		}

#ifndef SOLARIS
		/*
 		* Walk through the list of facilitynames (defined in sys/syslog.h)
		* to see if we can find the one we have been given
		*/
		chan->facility = -1;
 		cptr = facilitynames;
		while (cptr->c_name) {
			if (!strcasecmp(facility, cptr->c_name)) {
		 		chan->facility = cptr->c_val;
				break;
			}
			cptr++;
		}
#else
		chan->facility = -1;
		if (!strcasecmp(facility, "kern")) 
			chan->facility = LOG_KERN;
		else if (!strcasecmp(facility, "USER")) 
			chan->facility = LOG_USER;
		else if (!strcasecmp(facility, "MAIL")) 
			chan->facility = LOG_MAIL;
		else if (!strcasecmp(facility, "DAEMON")) 
			chan->facility = LOG_DAEMON;
		else if (!strcasecmp(facility, "AUTH")) 
			chan->facility = LOG_AUTH;
		else if (!strcasecmp(facility, "SYSLOG")) 
			chan->facility = LOG_SYSLOG;
		else if (!strcasecmp(facility, "LPR")) 
			chan->facility = LOG_LPR;
		else if (!strcasecmp(facility, "NEWS")) 
			chan->facility = LOG_NEWS;
		else if (!strcasecmp(facility, "UUCP")) 
			chan->facility = LOG_UUCP;
		else if (!strcasecmp(facility, "CRON")) 
			chan->facility = LOG_CRON;
		else if (!strcasecmp(facility, "LOCAL0")) 
			chan->facility = LOG_LOCAL0;
		else if (!strcasecmp(facility, "LOCAL1")) 
			chan->facility = LOG_LOCAL1;
		else if (!strcasecmp(facility, "LOCAL2")) 
			chan->facility = LOG_LOCAL2;
		else if (!strcasecmp(facility, "LOCAL3")) 
			chan->facility = LOG_LOCAL3;
		else if (!strcasecmp(facility, "LOCAL4")) 
			chan->facility = LOG_LOCAL4;
		else if (!strcasecmp(facility, "LOCAL5")) 
			chan->facility = LOG_LOCAL5;
		else if (!strcasecmp(facility, "LOCAL6")) 
			chan->facility = LOG_LOCAL6;
		else if (!strcasecmp(facility, "LOCAL7")) 
			chan->facility = LOG_LOCAL7;
#endif /* Solaris */

		if (0 > chan->facility) {
			fprintf(stderr, "Logger Warning: bad syslog facility in logger.conf\n");
			tris_free(chan);
			return NULL;
		}

		chan->type = LOGTYPE_SYSLOG;
		snprintf(chan->filename, sizeof(chan->filename), "%s", channel);
		openlog("trismedia", LOG_PID, chan->facility);
	} else {
		if (!tris_strlen_zero(hostname)) {
			snprintf(chan->filename, sizeof(chan->filename), "%s/%s.%s",
				 channel[0] != '/' ? tris_config_TRIS_LOG_DIR : "", channel, hostname);
		} else {
			snprintf(chan->filename, sizeof(chan->filename), "%s/%s",
				 channel[0] != '/' ? tris_config_TRIS_LOG_DIR : "", channel);
		}
		chan->fileptr = fopen(chan->filename, "a");
		if (!chan->fileptr) {
			/* Can't log here, since we're called with a lock */
			fprintf(stderr, "Logger Warning: Unable to open log file '%s': %s\n", chan->filename, strerror(errno));
		} 
		chan->type = LOGTYPE_FILE;
	}
	chan->logmask = make_components(components, lineno);
	return chan;
}

static void init_logger_chain(int locked)
{
	struct logchannel *chan;
	struct tris_config *cfg;
	struct tris_variable *var;
	const char *s;
	struct tris_flags config_flags = { 0 };

	if (!(cfg = tris_config_load2("logger.conf", "logger", config_flags)) || cfg == CONFIG_STATUS_FILEINVALID)
		return;

	/* delete our list of log channels */
	if (!locked)
		TRIS_RWLIST_WRLOCK(&logchannels);
	while ((chan = TRIS_RWLIST_REMOVE_HEAD(&logchannels, list)))
		tris_free(chan);
	if (!locked)
		TRIS_RWLIST_UNLOCK(&logchannels);
	
	global_logmask = 0;
	errno = 0;
	/* close syslog */
	closelog();
	
	/* If no config file, we're fine, set default options. */
	if (!cfg) {
		if (errno)
			fprintf(stderr, "Unable to open logger.conf: %s; default settings will be used.\n", strerror(errno));
		else
			fprintf(stderr, "Errors detected in logger.conf: see above; default settings will be used.\n");
		if (!(chan = tris_calloc(1, sizeof(*chan))))
			return;
		chan->type = LOGTYPE_CONSOLE;
		chan->logmask = 28; /*warning,notice,error */
		if (!locked)
			TRIS_RWLIST_WRLOCK(&logchannels);
		TRIS_RWLIST_INSERT_HEAD(&logchannels, chan, list);
		if (!locked)
			TRIS_RWLIST_UNLOCK(&logchannels);
		global_logmask |= chan->logmask;
		return;
	}
	if ((s = tris_variable_retrieve(cfg, "general", "maxlogsize"))) {
		float fsize;
		sscanf(s, "%f", &fsize);
		if(fsize) 
			maxlogsize = fsize;
	}
	if ((s = tris_variable_retrieve(cfg, "general", "appendhostname"))) {
		if (tris_true(s)) {
			if (gethostname(hostname, sizeof(hostname) - 1)) {
				tris_copy_string(hostname, "unknown", sizeof(hostname));
				fprintf(stderr, "What box has no hostname???\n");
			}
		} else
			hostname[0] = '\0';
	} else
		hostname[0] = '\0';
	if ((s = tris_variable_retrieve(cfg, "general", "dateformat")))
		tris_copy_string(dateformat, s, sizeof(dateformat));
	else
		tris_copy_string(dateformat, "%b %e %T", sizeof(dateformat));
	if ((s = tris_variable_retrieve(cfg, "general", "queue_log")))
		logfiles.queue_log = tris_true(s);
	if ((s = tris_variable_retrieve(cfg, "general", "event_log")))
		logfiles.event_log = tris_true(s);
	if ((s = tris_variable_retrieve(cfg, "general", "queue_log_name")))
		tris_copy_string(queue_log_name, s, sizeof(queue_log_name));
	if ((s = tris_variable_retrieve(cfg, "general", "exec_after_rotate")))
		tris_copy_string(exec_after_rotate, s, sizeof(exec_after_rotate));
	if ((s = tris_variable_retrieve(cfg, "general", "rotatestrategy"))) {
		if (strcasecmp(s, "timestamp") == 0)
			rotatestrategy = TIMESTAMP;
		else if (strcasecmp(s, "rotate") == 0)
			rotatestrategy = ROTATE;
		else if (strcasecmp(s, "sequential") == 0)
			rotatestrategy = SEQUENTIAL;
		else if (strcasecmp(s, "backup") == 0)
			rotatestrategy = BACKUP;
		else
			fprintf(stderr, "Unknown rotatestrategy: %s\n", s);
	} else {
		if ((s = tris_variable_retrieve(cfg, "general", "rotatetimestamp"))) {
			rotatestrategy = tris_true(s) ? TIMESTAMP : SEQUENTIAL;
			fprintf(stderr, "rotatetimestamp option has been deprecated.  Please use rotatestrategy instead.\n");
		}
	}

	if (!locked)
		TRIS_RWLIST_WRLOCK(&logchannels);
	var = tris_variable_browse(cfg, "logfiles");
	for (; var; var = var->next) {
		if (!(chan = make_logchannel(var->name, var->value, var->lineno)))
			continue;
		TRIS_RWLIST_INSERT_HEAD(&logchannels, chan, list);
		global_logmask |= chan->logmask;
	}
	if (!locked)
		TRIS_RWLIST_UNLOCK(&logchannels);

	tris_config_destroy(cfg);
}

void tris_child_verbose(int level, const char *fmt, ...)
{
	char *msg = NULL, *emsg = NULL, *sptr, *eptr;
	va_list ap, aq;
	int size;

	/* Don't bother, if the level isn't that high */
	if (option_verbose < level) {
		return;
	}

	va_start(ap, fmt);
	va_copy(aq, ap);
	if ((size = vsnprintf(msg, 0, fmt, ap)) < 0) {
		va_end(ap);
		va_end(aq);
		return;
	}
	va_end(ap);

	if (!(msg = tris_malloc(size + 1))) {
		va_end(aq);
		return;
	}

	vsnprintf(msg, size + 1, fmt, aq);
	va_end(aq);

	if (!(emsg = tris_malloc(size * 2 + 1))) {
		tris_free(msg);
		return;
	}

	for (sptr = msg, eptr = emsg; ; sptr++) {
		if (*sptr == '"') {
			*eptr++ = '\\';
		}
		*eptr++ = *sptr;
		if (*sptr == '\0') {
			break;
		}
	}
	tris_free(msg);

	fprintf(stdout, "verbose \"%s\" %d\n", emsg, level);
	fflush(stdout);
	tris_free(emsg);
}

void tris_queue_log(const char *queuename, const char *callid, const char *agent, const char *event, const char *fmt, ...)
{
	va_list ap;
	char qlog_msg[8192];
	int qlog_len;
	char time_str[16];

	if (tris_check_realtime("queue_log")) {
		va_start(ap, fmt);
		vsnprintf(qlog_msg, sizeof(qlog_msg), fmt, ap);
		va_end(ap);
		snprintf(time_str, sizeof(time_str), "%ld", (long)time(NULL));
		tris_store_realtime("queue_log", "time", time_str, 
						"callid", callid, 
						"queuename", queuename, 
						"agent", agent, 
						"event", event,
						"data", qlog_msg,
						SENTINEL);
	} else {
		if (qlog) {
			va_start(ap, fmt);
			qlog_len = snprintf(qlog_msg, sizeof(qlog_msg), "%ld|%s|%s|%s|%s|", (long)time(NULL), callid, queuename, agent, event);
			vsnprintf(qlog_msg + qlog_len, sizeof(qlog_msg) - qlog_len, fmt, ap);
			va_end(ap);
		}
		TRIS_RWLIST_RDLOCK(&logchannels);
		if (qlog) {
			fprintf(qlog, "%s\n", qlog_msg);
			fflush(qlog);
		}
		TRIS_RWLIST_UNLOCK(&logchannels);
	}
}

static int rotate_file(struct logchannel* c, const char *filename)
{
	char old[PATH_MAX];
	char new[PATH_MAX];
	int x, y, which, found, res = 0, fd;
	char *suffixes[4] = { "", ".gz", ".bz2", ".Z" };

	switch (rotatestrategy) {
	/* cool added */
	case BACKUP:
		snprintf(old, sizeof(old), filename);
		snprintf(new, sizeof(new), "%s.0", filename);
		if (rename(filename, new)) {
			fprintf(stderr, "Unable to rename file '%s' to '%s'\n", filename, new);
			res = -1;
		}
		if (c->fileptr) {
			fclose(c->fileptr);
			c->fileptr = fopen(old, "a");
		}
		break;
		
	case SEQUENTIAL:
		for (x = 0; ; x++) {
			snprintf(new, sizeof(new), "%s.%d", filename, x);
			fd = open(new, O_RDONLY);
			if (fd > -1)
				close(fd);
			else
				break;
		}
		if (rename(filename, new)) {
			fprintf(stderr, "Unable to rename file '%s' to '%s'\n", filename, new);
			res = -1;
		}
		break;
	case TIMESTAMP:
		snprintf(new, sizeof(new), "%s.%ld", filename, (long)time(NULL));
		if (rename(filename, new)) {
			fprintf(stderr, "Unable to rename file '%s' to '%s'\n", filename, new);
			res = -1;
		}
		break;
	case ROTATE:
		
		/* Find the next empty slot, including a possible suffix */
		for (x = 0; ; x++) {
			found = 0;
			for (which = 0; which < ARRAY_LEN(suffixes); which++) {
				snprintf(new, sizeof(new), "%s.%d%s", filename, x, suffixes[which]);
				fd = open(new, O_RDONLY);
				if (fd > -1) {
					close(fd);
					found = 1;
					break;
				}
			}
			if (!found) {
				break;
			}
		}

		/* Found an empty slot */
		for (y = x; y > 0; y--) {
			for (which = 0; which < ARRAY_LEN(suffixes); which++) {
				snprintf(old, sizeof(old), "%s.%d%s", filename, y - 1, suffixes[which]);
				fd = open(old, O_RDONLY);
				if (fd > -1) {
					/* Found the right suffix */
					close(fd);
					snprintf(new, sizeof(new), "%s.%d%s", filename, y, suffixes[which]);
					if (rename(old, new)) {
						fprintf(stderr, "Unable to rename file '%s' to '%s'\n", old, new);
						res = -1;
					}
					break;
				}
			}
		}

		/* Finally, rename the current file */
		snprintf(new, sizeof(new), "%s.0", filename);
		if (rename(filename, new)) {
			fprintf(stderr, "Unable to rename file '%s' to '%s'\n", filename, new);
			res = -1;
		}
	}

	if (!tris_strlen_zero(exec_after_rotate)) {
		struct tris_channel *c = tris_channel_alloc(0, 0, "", "", "", "", "", 0, "Logger/rotate");
		char buf[512];
		pbx_builtin_setvar_helper(c, "filename", filename);
		pbx_substitute_variables_helper(c, exec_after_rotate, buf, sizeof(buf));
		if (tris_safe_system(buf) == -1) {
			tris_log(LOG_WARNING, "error executing '%s'\n", buf);
		}
		tris_channel_free(c);
	}
	return res;
}

static int reload_logger(int rotate)
{
	char old[PATH_MAX] = "";
	int event_rotate = rotate, queue_rotate = rotate;
	struct logchannel *f;
	int res = 0;
	struct stat st;

	TRIS_RWLIST_WRLOCK(&logchannels);

	if (eventlog) {
		if (rotate < 0) {
			/* Check filesize - this one typically doesn't need an auto-rotate */
			snprintf(old, sizeof(old), "%s/%s", tris_config_TRIS_LOG_DIR, EVENTLOG);
			if (stat(old, &st) != 0 || st.st_size > 0x40000000) { /* Arbitrarily, 1 GB */
				fclose(eventlog);
				eventlog = NULL;
			} else
				event_rotate = 0;
		} else {
			fclose(eventlog);
			eventlog = NULL;
		}
	} else
		event_rotate = 0;

	if (qlog) {
		if (rotate < 0) {
			/* Check filesize - this one typically doesn't need an auto-rotate */
			snprintf(old, sizeof(old), "%s/%s", tris_config_TRIS_LOG_DIR, queue_log_name);
			if (stat(old, &st) != 0 || st.st_size > 0x40000000) { /* Arbitrarily, 1 GB */
				fclose(qlog);
				qlog = NULL;
			} else
				queue_rotate = 0;
		} else {
			fclose(qlog);
			qlog = NULL;
		}
	} else 
		queue_rotate = 0;

	tris_mkdir(tris_config_TRIS_LOG_DIR, 0777);

	TRIS_RWLIST_TRAVERSE(&logchannels, f, list) {
		if (f->disabled) {
			f->disabled = 0;	/* Re-enable logging at reload */
			manager_event(EVENT_FLAG_SYSTEM, "LogChannel", "Channel: %s\r\nEnabled: Yes\r\n", f->filename);
		}
		if (f->fileptr && (f->fileptr != stdout) && (f->fileptr != stderr)) {
			fclose(f->fileptr);	/* Close file */
			f->fileptr = NULL;
			if (rotate)
				rotate_file(f, f->filename);
		}
	}

	filesize_reload_needed = 0;

	init_logger_chain(1 /* locked */);

	if (logfiles.event_log) {
		snprintf(old, sizeof(old), "%s/%s", tris_config_TRIS_LOG_DIR, EVENTLOG);
		if (event_rotate)
			rotate_file(f, old);

		eventlog = fopen(old, "a");
		if (eventlog) {
			tris_log(LOG_EVENT, "Restarted Trismedia Event Logger\n");
			tris_verb(1, "Trismedia Event Logger restarted\n");
		} else {
			tris_log(LOG_ERROR, "Unable to create event log: %s\n", strerror(errno));
			res = -1;
		}
	}

	if (logfiles.queue_log) {
		snprintf(old, sizeof(old), "%s/%s", tris_config_TRIS_LOG_DIR, queue_log_name);
		if (queue_rotate)
			rotate_file(f, old);

		qlog = fopen(old, "a");
		if (qlog) {
			TRIS_RWLIST_UNLOCK(&logchannels);
			tris_queue_log("NONE", "NONE", "NONE", "CONFIGRELOAD", "%s", "");
			TRIS_RWLIST_WRLOCK(&logchannels);
			tris_log(LOG_EVENT, "Restarted Trismedia Queue Logger\n");
			tris_verb(1, "Trismedia Queue Logger restarted\n");
		} else {
			tris_log(LOG_ERROR, "Unable to create queue log: %s\n", strerror(errno));
			res = -1;
		}
	}

	TRIS_RWLIST_UNLOCK(&logchannels);

	return res;
}

/*! \brief Reload the logger module without rotating log files (also used from loader.c during
	a full Trismedia reload) */
int logger_reload(void)
{
	if(reload_logger(0))
		return RESULT_FAILURE;
	return RESULT_SUCCESS;
}

static char *handle_logger_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger reload";
		e->usage = 
			"Usage: logger reload\n"
			"       Reloads the logger subsystem state.  Use after restarting syslogd(8) if you are using syslog logging.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (reload_logger(0)) {
		tris_cli(a->fd, "Failed to reload the logger\n");
		return CLI_FAILURE;
	}
	return CLI_SUCCESS;
}

static char *handle_logger_rotate(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger rotate";
		e->usage = 
			"Usage: logger rotate\n"
			"       Rotates and Reopens the log files.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	if (reload_logger(1)) {
		tris_cli(a->fd, "Failed to reload the logger and rotate log files\n");
		return CLI_FAILURE;
	} 
	return CLI_SUCCESS;
}

static char *handle_logger_set_level(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int x;
	int state;
	int level = -1;

	switch (cmd) {
	case CLI_INIT:
		e->command = "logger set level";
		e->usage = 
			"Usage: logger set level\n"
			"       Set a specific log level to enabled/disabled for this console.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 5)
		return CLI_SHOWUSAGE;

	for (x = 0; x <= NUMLOGLEVELS; x++) {
		if (!strcasecmp(a->argv[3], levels[x])) {
			level = x;
			break;
		}
	}

	state = tris_true(a->argv[4]) ? 1 : 0;

	if (level != -1) {
		tris_console_toggle_loglevel(a->fd, level, state);
		tris_cli(a->fd, "Logger status for '%s' has been set to '%s'.\n", levels[level], state ? "on" : "off");
	} else
		return CLI_SHOWUSAGE;

	return CLI_SUCCESS;
}

/*! \brief CLI command to show logging system configuration */
static char *handle_logger_show_channels(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMATL	"%-35.35s %-8.8s %-9.9s "
	struct logchannel *chan;
	switch (cmd) {
	case CLI_INIT:
		e->command = "logger show channels";
		e->usage = 
			"Usage: logger show channels\n"
			"       List configured logger channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	tris_cli(a->fd, FORMATL, "Channel", "Type", "Status");
	tris_cli(a->fd, "Configuration\n");
	tris_cli(a->fd, FORMATL, "-------", "----", "------");
	tris_cli(a->fd, "-------------\n");
	TRIS_RWLIST_RDLOCK(&logchannels);
	TRIS_RWLIST_TRAVERSE(&logchannels, chan, list) {
		tris_cli(a->fd, FORMATL, chan->filename, chan->type == LOGTYPE_CONSOLE ? "Console" : (chan->type == LOGTYPE_SYSLOG ? "Syslog" : "File"),
			chan->disabled ? "Disabled" : "Enabled");
		tris_cli(a->fd, " - ");
		if (chan->logmask & (1 << __LOG_DEBUG)) 
			tris_cli(a->fd, "Debug ");
		if (chan->logmask & (1 << __LOG_DTMF)) 
			tris_cli(a->fd, "DTMF ");
		if (chan->logmask & (1 << __LOG_TRACE)) 
			tris_cli(a->fd, "TRACE ");
		if (chan->logmask & (1 << __LOG_VERBOSE)) 
			tris_cli(a->fd, "Verbose ");
		if (chan->logmask & (1 << __LOG_WARNING)) 
			tris_cli(a->fd, "Warning ");
		if (chan->logmask & (1 << __LOG_NOTICE)) 
			tris_cli(a->fd, "Notice ");
		if (chan->logmask & (1 << __LOG_ERROR)) 
			tris_cli(a->fd, "Error ");
		if (chan->logmask & (1 << __LOG_EVENT)) 
			tris_cli(a->fd, "Event ");
		tris_cli(a->fd, "\n");
	}
	TRIS_RWLIST_UNLOCK(&logchannels);
	tris_cli(a->fd, "\n");
 		
	return CLI_SUCCESS;
}

struct verb {
	void (*verboser)(const char *string);
	TRIS_LIST_ENTRY(verb) list;
};

static TRIS_RWLIST_HEAD_STATIC(verbosers, verb);

static struct tris_cli_entry cli_logger[] = {
	TRIS_CLI_DEFINE(handle_logger_show_channels, "List configured log channels"),
	TRIS_CLI_DEFINE(handle_logger_reload, "Reopens the log files"),
	TRIS_CLI_DEFINE(handle_logger_rotate, "Rotates and reopens the log files"),
	TRIS_CLI_DEFINE(handle_logger_set_level, "Enables/Disables a specific logging level for this console")
};

static int handle_SIGXFSZ(int sig) 
{
	/* Indicate need to reload */
	filesize_reload_needed = 1;
	return 0;
}

static void tris_log_vsyslog(int level, const char *file, int line, const char *function, char *str, long pid)
{
	char buf[BUFSIZ];

	if (level >= SYSLOG_NLEVELS) {
		/* we are locked here, so cannot tris_log() */
		fprintf(stderr, "tris_log_vsyslog called with bogus level: %d\n", level);
		return;
	}

	if (level == __LOG_VERBOSE) {
		snprintf(buf, sizeof(buf), "VERBOSE[%ld]: %s", pid, str);
		level = __LOG_DEBUG;
	} else if (level == __LOG_DTMF) {
		snprintf(buf, sizeof(buf), "DTMF[%ld]: %s", pid, str);
		level = __LOG_DEBUG;
	} else if (level == __LOG_TRACE) {
		snprintf(buf, sizeof(buf), "TRACE[%ld]: %s", pid, str);
		level = __LOG_DEBUG;
	} else {
		snprintf(buf, sizeof(buf), "%s[%ld]: %s:%d in %s: %s",
			 levels[level], pid, file, line, function, str);
	}

	term_strip(buf, buf, strlen(buf) + 1);
	syslog(syslog_level_map[level], "%s", buf);
}

/*! \brief Print a normal log message to the channels */
static void logger_print_normal(struct logmsg *logmsg)
{
	struct logchannel *chan = NULL;
	char buf[BUFSIZ];

	TRIS_RWLIST_RDLOCK(&logchannels);

	if (logfiles.event_log && logmsg->level == __LOG_EVENT) {
		fprintf(eventlog, "%s trismedia[%ld]: %s", logmsg->date, (long)getpid(), logmsg->str);
		fflush(eventlog);
		TRIS_RWLIST_UNLOCK(&logchannels);
		return;
	}

	if (!TRIS_RWLIST_EMPTY(&logchannels)) {
		TRIS_RWLIST_TRAVERSE(&logchannels, chan, list) {
			/* If the channel is disabled, then move on to the next one */
			if (chan->disabled)
				continue;
			/* Check syslog channels */
			if (chan->type == LOGTYPE_SYSLOG && (chan->logmask & (1 << logmsg->level))) {
				tris_log_vsyslog(logmsg->level, logmsg->file, logmsg->line, logmsg->function, logmsg->str, logmsg->process_id);
			/* Console channels */
			} else if (chan->type == LOGTYPE_CONSOLE && (chan->logmask & (1 << logmsg->level))) {
				char linestr[128];
				char tmp1[80], tmp2[80], tmp3[80], tmp4[80];

				/* If the level is verbose, then skip it */
				if (logmsg->level == __LOG_VERBOSE)
					continue;

				/* Turn the numerical line number into a string */
				snprintf(linestr, sizeof(linestr), "%d", logmsg->line);
				/* Build string to print out */
				snprintf(buf, sizeof(buf), "[%s] %s[%ld]: %s:%s %s: %s",
					 logmsg->date,
					 term_color(tmp1, levels[logmsg->level], colors[logmsg->level], 0, sizeof(tmp1)),
					 logmsg->process_id,
					 term_color(tmp2, logmsg->file, COLOR_BRWHITE, 0, sizeof(tmp2)),
					 term_color(tmp3, linestr, COLOR_BRWHITE, 0, sizeof(tmp3)),
					 term_color(tmp4, logmsg->function, COLOR_BRWHITE, 0, sizeof(tmp4)),
					 logmsg->str);
				/* Print out */
				tris_console_puts_mutable(buf, logmsg->level);
			/* File channels */
			} else if (chan->type == LOGTYPE_FILE && (chan->logmask & (1 << logmsg->level))) {
				int res = 0;

				/* If no file pointer exists, skip it */
				if (!chan->fileptr) {
					continue;
				}
				/* cool added */
				struct stat st;
				if(!stat(chan->filename, &st)) {
					if(st.st_size > 1024*1024*maxlogsize) {
						rotate_file(chan, chan->filename);
					}
				}

				/* Print out to the file */
				if (logmsg->level == __LOG_TRACE) {
					res = fprintf(chan->fileptr, "%s",
						      term_strip(buf, logmsg->str, BUFSIZ));
				} else {
					res = fprintf(chan->fileptr, "[%s] %s[%ld] %s: %s",
						      logmsg->date, levels[logmsg->level], logmsg->process_id, logmsg->file, term_strip(buf, logmsg->str, BUFSIZ));
				}
				if (res <= 0 && !tris_strlen_zero(logmsg->str)) {
					fprintf(stderr, "**** Trismedia Logging Error: ***********\n");
					if (errno == ENOMEM || errno == ENOSPC)
						fprintf(stderr, "Trismedia logging error: Out of disk space, can't log to log file %s\n", chan->filename);
					else
						fprintf(stderr, "Logger Warning: Unable to write to log file '%s': %s (disabled)\n", chan->filename, strerror(errno));
					manager_event(EVENT_FLAG_SYSTEM, "LogChannel", "Channel: %s\r\nEnabled: No\r\nReason: %d - %s\r\n", chan->filename, errno, strerror(errno));
					chan->disabled = 1;
				} else if (res > 0) {
					fflush(chan->fileptr);
				}
			}
		}
	} else if (logmsg->level != __LOG_VERBOSE) {
		fputs(logmsg->str, stdout);
	}

	TRIS_RWLIST_UNLOCK(&logchannels);
	
	/* If we need to reload because of the file size, then do so */
	if (filesize_reload_needed) {
		reload_logger(-1);
		tris_log(LOG_EVENT, "Rotated Logs Per SIGXFSZ (Exceeded file size limit)\n");
		tris_verb(1, "Rotated Logs Per SIGXFSZ (Exceeded file size limit)\n");
	}

	return;
}

/*! \brief Print a verbose message to the verbosers */
static void logger_print_verbose(struct logmsg *logmsg)
{
	struct verb *v = NULL;

	/* Iterate through the list of verbosers and pass them the log message string */
	TRIS_RWLIST_RDLOCK(&verbosers);
	TRIS_RWLIST_TRAVERSE(&verbosers, v, list)
		v->verboser(logmsg->str);
	TRIS_RWLIST_UNLOCK(&verbosers);

	return;
}

/*! \brief Actual logging thread */
static void *logger_thread(void *data)
{
	struct logmsg *next = NULL, *msg = NULL;

	for (;;) {
		/* We lock the message list, and see if any message exists... if not we wait on the condition to be signalled */
		TRIS_LIST_LOCK(&logmsgs);
		if (TRIS_LIST_EMPTY(&logmsgs)) {
			if (close_logger_thread) {
				break;
			} else {
				tris_cond_wait(&logcond, &logmsgs.lock);
			}
		}
		next = TRIS_LIST_FIRST(&logmsgs);
		TRIS_LIST_HEAD_INIT_NOLOCK(&logmsgs);
		TRIS_LIST_UNLOCK(&logmsgs);

		/* Otherwise go through and process each message in the order added */
		while ((msg = next)) {
			/* Get the next entry now so that we can free our current structure later */
			next = TRIS_LIST_NEXT(msg, list);

			/* Depending on the type, send it to the proper function */
			if (msg->type == LOGMSG_NORMAL)
				logger_print_normal(msg);
			else if (msg->type == LOGMSG_VERBOSE)
				logger_print_verbose(msg);

			/* Free the data since we are done */
			tris_free(msg);
		}

		/* If we should stop, then stop */
		if (close_logger_thread)
			break;
	}

	return NULL;
}

int init_logger(void)
{
	char tmp[256];
	int res = 0;

	/* auto rotate if sig SIGXFSZ comes a-knockin */
	(void) signal(SIGXFSZ, (void *) handle_SIGXFSZ);

	/* start logger thread */
	tris_cond_init(&logcond, NULL);
	if (tris_pthread_create(&logthread, NULL, logger_thread, NULL) < 0) {
		tris_cond_destroy(&logcond);
		return -1;
	}

	/* register the logger cli commands */
	tris_cli_register_multiple(cli_logger, ARRAY_LEN(cli_logger));

	tris_mkdir(tris_config_TRIS_LOG_DIR, 0777);
  
	/* create log channels */
	init_logger_chain(0 /* locked */);

	/* create the eventlog */
	if (logfiles.event_log) {
		snprintf(tmp, sizeof(tmp), "%s/%s", tris_config_TRIS_LOG_DIR, EVENTLOG);
		eventlog = fopen(tmp, "a");
		if (eventlog) {
			tris_log(LOG_EVENT, "Started Trismedia Event Logger\n");
			tris_verb(1, "Trismedia Event Logger Started %s\n", tmp);
		} else {
			tris_log(LOG_ERROR, "Unable to create event log: %s\n", strerror(errno));
			res = -1;
		}
	}

	if (logfiles.queue_log) {
		snprintf(tmp, sizeof(tmp), "%s/%s", tris_config_TRIS_LOG_DIR, queue_log_name);
		qlog = fopen(tmp, "a");
		tris_queue_log("NONE", "NONE", "NONE", "QUEUESTART", "%s", "");
	}
	return res;
}

void close_logger(void)
{
	struct logchannel *f = NULL;

	/* Stop logger thread */
	TRIS_LIST_LOCK(&logmsgs);
	close_logger_thread = 1;
	tris_cond_signal(&logcond);
	TRIS_LIST_UNLOCK(&logmsgs);

	if (logthread != TRIS_PTHREADT_NULL)
		pthread_join(logthread, NULL);

	TRIS_RWLIST_WRLOCK(&logchannels);

	if (eventlog) {
		fclose(eventlog);
		eventlog = NULL;
	}

	if (qlog) {
		fclose(qlog);
		qlog = NULL;
	}

	TRIS_RWLIST_TRAVERSE(&logchannels, f, list) {
		if (f->fileptr && (f->fileptr != stdout) && (f->fileptr != stderr)) {
			fclose(f->fileptr);
			f->fileptr = NULL;
		}
	}

	closelog(); /* syslog */

	TRIS_RWLIST_UNLOCK(&logchannels);

	return;
}

/*!
 * \brief send log messages to syslog and/or the console
 */
void tris_log(int level, const char *file, int line, const char *function, const char *fmt, ...)
{
	struct logmsg *logmsg = NULL;
	struct tris_str *buf = NULL;
	struct tris_tm tm;
	struct timeval now = tris_tvnow();
	int res = 0;
	va_list ap;

	if (!(buf = tris_str_thread_get(&log_buf, LOG_BUF_INIT_SIZE)))
		return;

	if (TRIS_RWLIST_EMPTY(&logchannels)) {
		/*
		 * we don't have the logger chain configured yet,
		 * so just log to stdout
		 */
		if (level != __LOG_VERBOSE) {
			int result;
			va_start(ap, fmt);
			result = tris_str_set_va(&buf, BUFSIZ, fmt, ap); /* XXX BUFSIZ ? */
			va_end(ap);
			if (result != TRIS_DYNSTR_BUILD_FAILED) {
				term_filter_escapes(tris_str_buffer(buf));
				fputs(tris_str_buffer(buf), stdout);
			}
		}
		return;
	}
	
	/* don't display LOG_DEBUG messages unless option_verbose _or_ option_debug
	   are non-zero; LOG_DEBUG messages can still be displayed if option_debug
	   is zero, if option_verbose is non-zero (this allows for 'level zero'
	   LOG_DEBUG messages to be displayed, if the logmask on any channel
	   allows it)
	*/
	if (!option_verbose && !option_debug && (level == __LOG_DEBUG))
		return;

	/* Ignore anything that never gets logged anywhere */
	if (!(global_logmask & (1 << level)))
		return;
	
	/* Build string */
	va_start(ap, fmt);
	res = tris_str_set_va(&buf, BUFSIZ, fmt, ap);
	va_end(ap);

	/* If the build failed, then abort and free this structure */
	if (res == TRIS_DYNSTR_BUILD_FAILED)
		return;

	/* Create a new logging message */
	if (!(logmsg = tris_calloc(1, sizeof(*logmsg) + res + 1)))
		return;

	/* Copy string over */
	strcpy(logmsg->str, tris_str_buffer(buf));

	/* Set type to be normal */
	logmsg->type = LOGMSG_NORMAL;

	/* Create our date/time */
	tris_localtime(&now, &tm, NULL);
	tris_strftime(logmsg->date, sizeof(logmsg->date), dateformat, &tm);

	/* Copy over data */
	logmsg->level = level;
	logmsg->line = line;
	tris_copy_string(logmsg->file, file, sizeof(logmsg->file));
	tris_copy_string(logmsg->function, function, sizeof(logmsg->function));
	logmsg->process_id = (long) GETTID();

	/* If the logger thread is active, append it to the tail end of the list - otherwise skip that step */
	if (logthread != TRIS_PTHREADT_NULL) {
		TRIS_LIST_LOCK(&logmsgs);
		TRIS_LIST_INSERT_TAIL(&logmsgs, logmsg, list);
		tris_cond_signal(&logcond);
		TRIS_LIST_UNLOCK(&logmsgs);
	} else {
		logger_print_normal(logmsg);
		tris_free(logmsg);
	}

	return;
}

#ifdef HAVE_BKTR

struct tris_bt *tris_bt_create(void) 
{
	struct tris_bt *bt = tris_calloc(1, sizeof(*bt));
	if (!bt) {
		tris_log(LOG_ERROR, "Unable to allocate memory for backtrace structure!\n");
		return NULL;
	}

	bt->alloced = 1;

	tris_bt_get_addresses(bt);

	return bt;
}

int tris_bt_get_addresses(struct tris_bt *bt)
{
	bt->num_frames = backtrace(bt->addresses, TRIS_MAX_BT_FRAMES);

	return 0;
}

void *tris_bt_destroy(struct tris_bt *bt)
{
	if (bt->alloced) {
		tris_free(bt);
	}

	return NULL;
}

#endif /* HAVE_BKTR */

void tris_backtrace(void)
{
#ifdef HAVE_BKTR
	struct tris_bt *bt;
	int i = 0;
	char **strings;

	if (!(bt = tris_bt_create())) {
		tris_log(LOG_WARNING, "Unable to allocate space for backtrace structure\n");
		return;
	}

	if ((strings = backtrace_symbols(bt->addresses, bt->num_frames))) {
		tris_debug(1, "Got %d backtrace record%c\n", bt->num_frames, bt->num_frames != 1 ? 's' : ' ');
		for (i = 0; i < bt->num_frames; i++) {
			tris_log(LOG_DEBUG, "#%d: [%p] %s\n", i, bt->addresses[i], strings[i]);
		}
		free(strings);
	} else {
		tris_debug(1, "Could not allocate memory for backtrace\n");
	}
	tris_bt_destroy(bt);
#else
	tris_log(LOG_WARNING, "Must run configure with '--with-execinfo' for stack backtraces.\n");
#endif
}

void __tris_verbose_ap(const char *file, int line, const char *func, const char *fmt, va_list ap)
{
	struct logmsg *logmsg = NULL;
	struct tris_str *buf = NULL;
	int res = 0;

	if (!(buf = tris_str_thread_get(&verbose_buf, VERBOSE_BUF_INIT_SIZE)))
		return;

	if (tris_opt_timestamp) {
		struct timeval now;
		struct tris_tm tm;
		char date[40];
		char *datefmt;

		now = tris_tvnow();
		tris_localtime(&now, &tm, NULL);
		tris_strftime(date, sizeof(date), dateformat, &tm);
		datefmt = alloca(strlen(date) + 3 + strlen(fmt) + 1);
		sprintf(datefmt, "%c[%s] %s", 127, date, fmt);
		fmt = datefmt;
	} else {
		char *tmp = alloca(strlen(fmt) + 2);
		sprintf(tmp, "%c%s", 127, fmt);
		fmt = tmp;
	}

	/* Build string */
	res = tris_str_set_va(&buf, 0, fmt, ap);

	/* If the build failed then we can drop this allocated message */
	if (res == TRIS_DYNSTR_BUILD_FAILED)
		return;

	if (!(logmsg = tris_calloc(1, sizeof(*logmsg) + res + 1)))
		return;

	strcpy(logmsg->str, tris_str_buffer(buf));

	tris_log(__LOG_VERBOSE, file, line, func, "%s", logmsg->str + 1);

	/* Set type */
	logmsg->type = LOGMSG_VERBOSE;
	
	/* Add to the list and poke the thread if possible */
	if (logthread != TRIS_PTHREADT_NULL) {
		TRIS_LIST_LOCK(&logmsgs);
		TRIS_LIST_INSERT_TAIL(&logmsgs, logmsg, list);
		tris_cond_signal(&logcond);
		TRIS_LIST_UNLOCK(&logmsgs);
	} else {
		logger_print_verbose(logmsg);
		tris_free(logmsg);
	}
}

void __tris_verbose(const char *file, int line, const char *func, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	__tris_verbose_ap(file, line, func, fmt, ap);
	va_end(ap);
}

/* No new code should use this directly, but we have the ABI for backwards compat */
#undef tris_verbose
void __attribute__((format(printf, 1,2))) tris_verbose(const char *fmt, ...);
void tris_verbose(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	__tris_verbose_ap("", 0, "", fmt, ap);
	va_end(ap);
}

int tris_register_verbose(void (*v)(const char *string)) 
{
	struct verb *verb;

	if (!(verb = tris_malloc(sizeof(*verb))))
		return -1;

	verb->verboser = v;

	TRIS_RWLIST_WRLOCK(&verbosers);
	TRIS_RWLIST_INSERT_HEAD(&verbosers, verb, list);
	TRIS_RWLIST_UNLOCK(&verbosers);
	
	return 0;
}

int tris_unregister_verbose(void (*v)(const char *string))
{
	struct verb *cur;

	TRIS_RWLIST_WRLOCK(&verbosers);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&verbosers, cur, list) {
		if (cur->verboser == v) {
			TRIS_RWLIST_REMOVE_CURRENT(list);
			tris_free(cur);
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;
	TRIS_RWLIST_UNLOCK(&verbosers);
	
	return cur ? 0 : -1;
}
