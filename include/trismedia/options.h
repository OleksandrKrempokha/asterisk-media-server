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
 * \brief Options provided by main trismedia program
 */

#ifndef _TRISMEDIA_OPTIONS_H
#define _TRISMEDIA_OPTIONS_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define TRIS_CACHE_DIR_LEN 	512
#define TRIS_FILENAME_MAX	80
#define TRIS_CHANNEL_NAME    80  /*!< Max length of an tris_channel name */


/*! \ingroup main_options */
enum tris_option_flags {
	/*! Allow \#exec in config files */
	TRIS_OPT_FLAG_EXEC_INCLUDES = (1 << 0),
	/*! Do not fork() */
	TRIS_OPT_FLAG_NO_FORK = (1 << 1),
	/*! Keep quiet */
	TRIS_OPT_FLAG_QUIET = (1 << 2),
	/*! Console mode */
	TRIS_OPT_FLAG_CONSOLE = (1 << 3),
	/*! Run in realtime Linux priority */
	TRIS_OPT_FLAG_HIGH_PRIORITY = (1 << 4),
	/*! Initialize keys for RSA authentication */
	TRIS_OPT_FLAG_INIT_KEYS = (1 << 5),
	/*! Remote console */
	TRIS_OPT_FLAG_REMOTE = (1 << 6),
	/*! Execute an trismedia CLI command upon startup */
	TRIS_OPT_FLAG_EXEC = (1 << 7),
	/*! Don't use termcap colors */
	TRIS_OPT_FLAG_NO_COLOR = (1 << 8),
	/*! Are we fully started yet? */
	TRIS_OPT_FLAG_FULLY_BOOTED = (1 << 9),
	/*! Trascode via signed linear */
	TRIS_OPT_FLAG_TRANSCODE_VIA_SLIN = (1 << 10),
	/*! Dump core on a seg fault */
	TRIS_OPT_FLAG_DUMP_CORE = (1 << 12),
	/*! Cache sound files */
	TRIS_OPT_FLAG_CACHE_RECORD_FILES = (1 << 13),
	/*! Display timestamp in CLI verbose output */
	TRIS_OPT_FLAG_TIMESTAMP = (1 << 14),
	/*! Override config */
	TRIS_OPT_FLAG_OVERRIDE_CONFIG = (1 << 15),
	/*! Reconnect */
	TRIS_OPT_FLAG_RECONNECT = (1 << 16),
	/*! Transmit Silence during Record() and DTMF Generation */
	TRIS_OPT_FLAG_TRANSMIT_SILENCE = (1 << 17),
	/*! Suppress some warnings */
	TRIS_OPT_FLAG_DONT_WARN = (1 << 18),
	/*! End CDRs before the 'h' extension */
	TRIS_OPT_FLAG_END_CDR_BEFORE_H_EXTEN = (1 << 19),
	/*! Use DAHDI Timing for generators if available */
	TRIS_OPT_FLAG_INTERNAL_TIMING = (1 << 20),
	/*! Always fork, even if verbose or debug settings are non-zero */
	TRIS_OPT_FLAG_ALWAYS_FORK = (1 << 21),
	/*! Disable log/verbose output to remote consoles */
	TRIS_OPT_FLAG_MUTE = (1 << 22),
	/*! There is a per-file debug setting */
	TRIS_OPT_FLAG_DEBUG_FILE = (1 << 23),
	/*! There is a per-file verbose setting */
	TRIS_OPT_FLAG_VERBOSE_FILE = (1 << 24),
	/*! Terminal colors should be adjusted for a light-colored background */
	TRIS_OPT_FLAG_LIGHT_BACKGROUND = (1 << 25),
	/*! Count Initiated seconds in CDR's */
	TRIS_OPT_FLAG_INITIATED_SECONDS = (1 << 26),
	/*! Force black background */
	TRIS_OPT_FLAG_FORCE_BLACK_BACKGROUND = (1 << 27),
	/*! Hide remote console connect messages on console */
	TRIS_OPT_FLAG_HIDE_CONSOLE_CONNECT = (1 << 28),
};

/*! These are the options that set by default when Trismedia starts */
#define TRIS_DEFAULT_OPTIONS TRIS_OPT_FLAG_TRANSCODE_VIA_SLIN

#define tris_opt_exec_includes		tris_test_flag(&tris_options, TRIS_OPT_FLAG_EXEC_INCLUDES)
#define tris_opt_no_fork			tris_test_flag(&tris_options, TRIS_OPT_FLAG_NO_FORK)
#define tris_opt_quiet			tris_test_flag(&tris_options, TRIS_OPT_FLAG_QUIET)
#define tris_opt_console			tris_test_flag(&tris_options, TRIS_OPT_FLAG_CONSOLE)
#define tris_opt_high_priority		tris_test_flag(&tris_options, TRIS_OPT_FLAG_HIGH_PRIORITY)
#define tris_opt_init_keys		tris_test_flag(&tris_options, TRIS_OPT_FLAG_INIT_KEYS)
#define tris_opt_remote			tris_test_flag(&tris_options, TRIS_OPT_FLAG_REMOTE)
#define tris_opt_exec			tris_test_flag(&tris_options, TRIS_OPT_FLAG_EXEC)
#define tris_opt_no_color		tris_test_flag(&tris_options, TRIS_OPT_FLAG_NO_COLOR)
#define tris_fully_booted		tris_test_flag(&tris_options, TRIS_OPT_FLAG_FULLY_BOOTED)
#define tris_opt_transcode_via_slin	tris_test_flag(&tris_options, TRIS_OPT_FLAG_TRANSCODE_VIA_SLIN)
#define tris_opt_dump_core		tris_test_flag(&tris_options, TRIS_OPT_FLAG_DUMP_CORE)
#define tris_opt_cache_record_files	tris_test_flag(&tris_options, TRIS_OPT_FLAG_CACHE_RECORD_FILES)
#define tris_opt_timestamp		tris_test_flag(&tris_options, TRIS_OPT_FLAG_TIMESTAMP)
#define tris_opt_override_config		tris_test_flag(&tris_options, TRIS_OPT_FLAG_OVERRIDE_CONFIG)
#define tris_opt_reconnect		tris_test_flag(&tris_options, TRIS_OPT_FLAG_RECONNECT)
#define tris_opt_transmit_silence	tris_test_flag(&tris_options, TRIS_OPT_FLAG_TRANSMIT_SILENCE)
#define tris_opt_dont_warn		tris_test_flag(&tris_options, TRIS_OPT_FLAG_DONT_WARN)
#define tris_opt_end_cdr_before_h_exten	tris_test_flag(&tris_options, TRIS_OPT_FLAG_END_CDR_BEFORE_H_EXTEN)
#define tris_opt_internal_timing		tris_test_flag(&tris_options, TRIS_OPT_FLAG_INTERNAL_TIMING)
#define tris_opt_always_fork		tris_test_flag(&tris_options, TRIS_OPT_FLAG_ALWAYS_FORK)
#define tris_opt_mute			tris_test_flag(&tris_options, TRIS_OPT_FLAG_MUTE)
#define tris_opt_dbg_file		tris_test_flag(&tris_options, TRIS_OPT_FLAG_DEBUG_FILE)
#define tris_opt_verb_file		tris_test_flag(&tris_options, TRIS_OPT_FLAG_VERBOSE_FILE)
#define tris_opt_light_background		tris_test_flag(&tris_options, TRIS_OPT_FLAG_LIGHT_BACKGROUND)
#define tris_opt_force_black_background		tris_test_flag(&tris_options, TRIS_OPT_FLAG_FORCE_BLACK_BACKGROUND)
#define tris_opt_hide_connect		tris_test_flag(&tris_options, TRIS_OPT_FLAG_HIDE_CONSOLE_CONNECT)

extern struct tris_flags tris_options;

enum tris_compat_flags {
	TRIS_COMPAT_DELIM_PBX_REALTIME = (1 << 0),
	TRIS_COMPAT_DELIM_RES_AGI = (1 << 1),
	TRIS_COMPAT_APP_SET = (1 << 2),
};

#define	tris_compat_pbx_realtime	tris_test_flag(&tris_compat, TRIS_COMPAT_DELIM_PBX_REALTIME)
#define tris_compat_res_agi	tris_test_flag(&tris_compat, TRIS_COMPAT_DELIM_RES_AGI)
#define	tris_compat_app_set	tris_test_flag(&tris_compat, TRIS_COMPAT_APP_SET)

extern struct tris_flags tris_compat;

extern int option_verbose;
extern int option_maxfiles;		/*!< Max number of open file handles (files, sockets) */
extern int option_debug;		/*!< Debugging */
extern int option_maxcalls;		/*!< Maximum number of simultaneous channels */
extern double option_maxload;
#if defined(HAVE_SYSINFO)
extern long option_minmemfree;		/*!< Minimum amount of free system memory - stop accepting calls if free memory falls below this watermark */
#endif
extern char defaultlanguage[];

extern struct timeval tris_startuptime;
extern struct timeval tris_lastreloadtime;
extern pid_t tris_mainpid;

extern char record_cache_dir[TRIS_CACHE_DIR_LEN];
extern char dahdi_chan_name[TRIS_CHANNEL_NAME];
extern int dahdi_chan_name_len;

extern int tris_language_is_prefix;

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_OPTIONS_H */
