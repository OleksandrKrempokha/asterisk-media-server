/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief Application convenience functions, designed to give consistent
 *        look and feel to Trismedia apps.
 */

#ifndef _TRISMEDIA_APP_H
#define _TRISMEDIA_APP_H

#include "trismedia/strings.h"
#include "trismedia/threadstorage.h"

struct tris_flags64;

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

TRIS_THREADSTORAGE_EXTERNAL(tris_str_thread_global_buf);

/* IVR stuff */

/*! \brief Callback function for IVR
    \return returns 0 on completion, -1 on hangup or digit if interrupted
  */
typedef int (*tris_ivr_callback)(struct tris_channel *chan, char *option, void *cbdata);

typedef enum {
	TRIS_ACTION_UPONE,	/*!< adata is unused */
	TRIS_ACTION_EXIT,	/*!< adata is the return value for tris_ivr_menu_run if channel was not hungup */
	TRIS_ACTION_CALLBACK,	/*!< adata is an tris_ivr_callback */
	TRIS_ACTION_PLAYBACK,	/*!< adata is file to play */
	TRIS_ACTION_BACKGROUND,	/*!< adata is file to play */
	TRIS_ACTION_PLAYLIST,	/*!< adata is list of files, separated by ; to play */
	TRIS_ACTION_MENU,	/*!< adata is a pointer to an tris_ivr_menu */
	TRIS_ACTION_REPEAT,	/*!< adata is max # of repeats, cast to a pointer */
	TRIS_ACTION_RESTART,	/*!< adata is like repeat, but resets repeats to 0 */
	TRIS_ACTION_TRANSFER,	/*!< adata is a string with exten\verbatim[@context]\endverbatim */
	TRIS_ACTION_WAITOPTION,	/*!< adata is a timeout, or 0 for defaults */
	TRIS_ACTION_NOOP,	/*!< adata is unused */
	TRIS_ACTION_BACKLIST,	/*!< adata is list of files separated by ; allows interruption */
} tris_ivr_action;

/*!
    Special "options" are:
   \arg "s" - "start here (one time greeting)"
   \arg "g" - "greeting/instructions"
   \arg "t" - "timeout"
   \arg "h" - "hangup"
   \arg "i" - "invalid selection"

*/
struct tris_ivr_option {
	char *option;
	tris_ivr_action action;
	void *adata;
};

struct tris_ivr_menu {
	char *title;		/*!< Title of menu */
	unsigned int flags;	/*!< Flags */
	struct tris_ivr_option *options;	/*!< All options */
};

#define TRIS_IVR_FLAG_AUTORESTART (1 << 0)

#define TRIS_IVR_DECLARE_MENU(holder, title, flags, foo...) \
	static struct tris_ivr_option __options_##holder[] = foo;\
	static struct tris_ivr_menu holder = { title, flags, __options_##holder }


/*!	\brief Runs an IVR menu
	\return returns 0 on successful completion, -1 on hangup, or -2 on user error in menu */
int tris_ivr_menu_run(struct tris_channel *c, struct tris_ivr_menu *menu, void *cbdata);

/*! \brief Plays a stream and gets DTMF data from a channel
 * \param c Which channel one is interacting with
 * \param prompt File to pass to tris_streamfile (the one that you wish to play).
 *        It is also valid for this to be multiple files concatenated by "&".
 *        For example, "file1&file2&file3".
 * \param s The location where the DTMF data will be stored
 * \param maxlen Max Length of the data
 * \param timeout Timeout length waiting for data(in milliseconds).  Set to 0 for standard timeout(six seconds), or -1 for no time out.
 *
 *  This function was designed for application programmers for situations where they need
 *  to play a message and then get some DTMF data in response to the message.  If a digit
 *  is pressed during playback, it will immediately break out of the message and continue
 *  execution of your code.
 */
int tris_app_getdata(struct tris_channel *c, const char *prompt, char *s, int maxlen, int timeout);

int tris_meetme_dialout_getdata(struct tris_channel *c, const char *prompt, char *s, int maxlen, int timeout, char *endcodes); 

int meetme_readstring(struct tris_channel *c, char *s, int len, int timeout, int ftimeout, char *enders);
int meetme_readstring_full(struct tris_channel *c, char *s, int len, int timeout, int ftimeout, char *enders, int audiofd, int ctrlfd);

/*! \brief Full version with audiofd and controlfd.  NOTE: returns '2' on ctrlfd available, not '1' like other full functions */
int tris_app_getdata_full(struct tris_channel *c, char *prompt, char *s, int maxlen, int timeout, int audiofd, int ctrlfd);

/*!
 * \brief Set voicemail function callbacks
 * \param[in] inboxcount2_func set function pointer
 * \param[in] sayname_func set function pointer
 * \param[in] inboxcount_func set function pointer
 * \param[in] messagecount_func set function pointer
 * \version 1.6.1 Added inboxcount2_func, sayname_func
 */
void tris_install_vm_functions(int (*has_voicemail_func)(const char *mailbox, const char *folder),
			      int (*inboxcount_func)(const char *mailbox, int *newmsgs, int *oldmsgs),
			      int (*inboxcount2_func)(const char *mailbox, int *urgentmsgs, int *newmsgs, int *oldmsgs),
			      int (*messagecount_func)(const char *context, const char *mailbox, const char *folder),
			      int (*sayname_func)(struct tris_channel *chan, const char *mailbox, const char *context),
			      int (*getvmlist_func)(const char *mailbox, const char*folder, char *vmlist),
			      int (*managemailbox_func)(const char * mailbox, int folder, int * msglist, int msgcount, const char * command, char *result));

void tris_uninstall_vm_functions(void);

/*!
 * \brief Determine if a given mailbox has any voicemail
 * If folder is NULL, defaults to "INBOX".  If folder is "INBOX", includes the
 * number of messages in the "Urgent" folder.
 * \retval 1 Mailbox has voicemail
 * \retval 0 No new voicemail in specified mailbox
 * \retval -1 Failure
 * \since 1.0
 */
int tris_app_has_voicemail(const char *mailbox, const char *folder);

/*!
 * \brief Determine number of new/old messages in a mailbox
 * \since 1.0
 * \param[in] mailbox Mailbox specification in the format mbox[@context][&mbox2[@context2]][...]
 * \param[out] newmsgs Number of messages in the "INBOX" folder.  Includes number of messages in the "Urgent" folder, if any.
 * \param[out] oldmsgs Number of messages in the "Old" folder.
 * \retval 0 Success
 * \retval -1 Failure
 */
int tris_app_inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs);

/*!
 * \brief Determine number of urgent/new/old messages in a mailbox
 * \param[in] mailbox the mailbox context to use
 * \param[out] urgentmsgs the urgent message count
 * \param[out] newmsgs the new message count
 * \param[out] oldmsgs the old message count
 * \return Returns 0 for success, negative upon error
 * \since 1.6.1
 */
int tris_app_inboxcount2(const char *mailbox, int *urgentmsgs, int *newmsgs, int *oldmsgs);

/*!
 * \brief Given a mailbox and context, play that mailbox owner's name to the channel specified
 * \param[in] chan Channel on which to play the name
 * \param[in] mailbox Mailbox number from which to retrieve the recording
 * \param[in] context Mailbox context from which to locate the mailbox number
 * \retval 0 Name played without interruption
 * \retval dtmf ASCII value of the DTMF which interrupted playback.
 * \retval -1 Unable to locate mailbox or hangup occurred.
 * \since 1.6.1
 */
int tris_app_sayname(struct tris_channel *chan, const char *mailbox, const char *context);

/*!
 * \brief Check number of messages in a given context, mailbox, and folder
 * \since 1.4
 * \param[in] context Mailbox context
 * \param[in] mailbox Mailbox number
 * \param[in] folder Mailbox folder
 * \return Number of messages in the given context, mailbox, and folder.  If folder is NULL, folder "INBOX" is assumed.  If folder is "INBOX", includes number of messages in the "Urgent" folder.
 */
int tris_app_messagecount(const char *context, const char *mailbox, const char *folder);

int tris_app_get_vmlist(const char *mailbox, const char *folder, char *vmlist);

int tris_app_manage_mailbox(const char *mailbox, int folder, int *msglist, int msgcount, const char *command, char *result);

/*! \brief Safely spawn an external program while closing file descriptors
	\note This replaces the \b system call in all Trismedia modules
*/
int tris_safe_system(const char *s);

/*!
 * \brief Replace the SIGCHLD handler
 *
 * Normally, Trismedia has a SIGCHLD handler that is cleaning up all zombie
 * processes from forking elsewhere in Trismedia.  However, if you want to
 * wait*() on the process to retrieve information about it's exit status,
 * then this signal handler needs to be temporarily replaced.
 *
 * Code that executes this function *must* call tris_unreplace_sigchld()
 * after it is finished doing the wait*().
 */
void tris_replace_sigchld(void);

/*!
 * \brief Restore the SIGCHLD handler
 *
 * This function is called after a call to tris_replace_sigchld.  It restores
 * the SIGCHLD handler that cleans up any zombie processes.
 */
void tris_unreplace_sigchld(void);

/*!
  \brief Send DTMF to a channel

  \param chan    The channel that will receive the DTMF frames
  \param peer    (optional) Peer channel that will be autoserviced while the
                 primary channel is receiving DTMF
  \param digits  This is a string of characters representing the DTMF digits
                 to be sent to the channel.  Valid characters are
                 "0123456789*#abcdABCD".  Note: You can pass arguments 'f' or
                 'F', if you want to Flash the channel (if supported by the
                 channel), or 'w' to add a 500 millisecond pause to the DTMF
                 sequence.
  \param between This is the number of milliseconds to wait in between each
                 DTMF digit.  If zero milliseconds is specified, then the
                 default value of 100 will be used.
  \param duration This is the duration that each DTMF digit should have.
*/
int tris_dtmf_stream(struct tris_channel *chan, struct tris_channel *peer, const char *digits, int between, unsigned int duration);

/*! \brief Stream a filename (or file descriptor) as a generator. */
int tris_linear_stream(struct tris_channel *chan, const char *filename, int fd, int allowoverride);

/*!
 * \brief Stream a file with fast forward, pause, reverse, restart.
 * \param chan
 * \param file filename
 * \param fwd, rev, stop, pause, restart, skipms, offsetms
 *
 * Before calling this function, set this to be the number
 * of ms to start from the beginning of the file.  When the function
 * returns, it will be the number of ms from the beginning where the
 * playback stopped.  Pass NULL if you don't care.
 */
int tris_control_streamfile(struct tris_channel *chan, const char *file, const char *fwd, const char *rev, const char *stop, const char *pause, const char *restart, int skipms, long *offsetms);

/*! \brief Play a stream and wait for a digit, returning the digit that was pressed */
int tris_play_and_wait(struct tris_channel *chan, const char *fn);

int tris_play_and_record_full(struct tris_channel *chan, const char *playfile, const char *recordfile, int maxtime_sec, const char *fmt, int *duration, int silencethreshold, int maxsilence_ms, const char *path, const char *acceptdtmf, const char *canceldtmf);

/*! \brief Record a file for a max amount of time (in seconds), in a given list of formats separated by '|', outputting the duration of the recording, and with a maximum
 \n
 permitted silence time in milliseconds of 'maxsilence' under 'silencethreshold' or use '-1' for either or both parameters for defaults.
     calls tris_unlock_path() on 'path' if passed */
int tris_play_and_record(struct tris_channel *chan, const char *playfile, const char *recordfile, int maxtime_sec, const char *fmt, int *duration, int silencethreshold, int maxsilence_ms, const char *path);

/*! \brief Record a message and prepend the message to the given record file after
    playing the optional playfile (or a beep), storing the duration in
    'duration' and with a maximum permitted silence time in milliseconds of 'maxsilence' under
    'silencethreshold' or use '-1' for either or both parameters for defaults. */
int tris_play_and_prepend(struct tris_channel *chan, char *playfile, char *recordfile, int maxtime_sec, char *fmt, int *duration, int beep, int silencethreshold, int maxsilence_ms);

enum tris_getdata_result {
	TRIS_GETDATA_FAILED = -1,
	TRIS_GETDATA_COMPLETE = 0,
	TRIS_GETDATA_TIMEOUT = 1,
	TRIS_GETDATA_INTERRUPTED = 2,
	/*! indicates a user terminated empty string rather than an empty string resulting 
	 * from a timeout or other factors */
	TRIS_GETDATA_EMPTY_END_TERMINATED = 3,
};

enum TRIS_LOCK_RESULT {
	TRIS_LOCK_SUCCESS = 0,
	TRIS_LOCK_TIMEOUT = -1,
	TRIS_LOCK_PATH_NOT_FOUND = -2,
	TRIS_LOCK_FAILURE = -3,
};

/*! \brief Type of locking to use in tris_lock_path / tris_unlock_path */
enum TRIS_LOCK_TYPE {
	TRIS_LOCK_TYPE_LOCKFILE = 0,
	TRIS_LOCK_TYPE_FLOCK = 1,
};

/*!
 * \brief Set the type of locks used by tris_lock_path()
 * \param type the locking type to use
 */
void tris_set_lock_type(enum TRIS_LOCK_TYPE type);

/*!
 * \brief Lock a filesystem path.
 * \param path the path to be locked
 * \return one of \ref TRIS_LOCK_RESULT values
 */
enum TRIS_LOCK_RESULT tris_lock_path(const char *path);

/*! \brief Unlock a path */
int tris_unlock_path(const char *path);

/*! \brief Read a file into trismedia*/
char *tris_read_textfile(const char *file);

struct tris_group_info;

/*! \brief Split a group string into group and category, returning a default category if none is provided. */
int tris_app_group_split_group(const char *data, char *group, int group_max, char *category, int category_max);

/*! \brief Set the group for a channel, splitting the provided data into group and category, if specified. */
int tris_app_group_set_channel(struct tris_channel *chan, const char *data);

/*! \brief Get the current channel count of the specified group and category. */
int tris_app_group_get_count(const char *group, const char *category);

/*! \brief Get the current channel count of all groups that match the specified pattern and category. */
int tris_app_group_match_get_count(const char *groupmatch, const char *category);

/*! \brief Discard all group counting for a channel */
int tris_app_group_discard(struct tris_channel *chan);

/*! \brief Update all group counting for a channel to a new one */
int tris_app_group_update(struct tris_channel *oldchan, struct tris_channel *newchan);

/*! \brief Write Lock the group count list */
int tris_app_group_list_wrlock(void);

/*! \brief Read Lock the group count list */
int tris_app_group_list_rdlock(void);

/*! \brief Get the head of the group count list */
struct tris_group_info *tris_app_group_list_head(void);

/*! \brief Unlock the group count list */
int tris_app_group_list_unlock(void);

/*!
  \brief Define an application argument
  \param name The name of the argument
*/
#define TRIS_APP_ARG(name) char *name

/*!
  \brief Declare a structure to hold an application's arguments.
  \param name The name of the structure
  \param arglist The list of arguments, defined using TRIS_APP_ARG

  This macro declares a structure intended to be used in a call
  to tris_app_separate_args(). The structure includes all the
  arguments specified, plus an argv array that overlays them and an
  argc argument counter. The arguments must be declared using TRIS_APP_ARG,
  and they will all be character pointers (strings).

  \note The structure is <b>not</b> initialized, as the call to
  tris_app_separate_args() will perform that function before parsing
  the arguments.
 */
#define TRIS_DECLARE_APP_ARGS(name, arglist) TRIS_DEFINE_APP_ARGS_TYPE(, arglist) name

/*!
  \brief Define a structure type to hold an application's arguments.
  \param type The name of the structure type
  \param arglist The list of arguments, defined using TRIS_APP_ARG

  This macro defines a structure type intended to be used in a call
  to tris_app_separate_args(). The structure includes all the
  arguments specified, plus an argv array that overlays them and an
  argc argument counter. The arguments must be declared using TRIS_APP_ARG,
  and they will all be character pointers (strings).

  \note This defines a structure type, but does not declare an instance
  of the structure. That must be done separately.
 */
#define TRIS_DEFINE_APP_ARGS_TYPE(type, arglist) \
	struct type { \
		unsigned int argc; \
		char *argv[0]; \
		arglist \
	}

/*!
  \brief Performs the 'standard' argument separation process for an application.
  \param args An argument structure defined using TRIS_DECLARE_APP_ARGS
  \param parse A modifiable buffer containing the input to be parsed

  This function will separate the input string using the standard argument
  separator character ',' and fill in the provided structure, including
  the argc argument counter field.
 */
#define TRIS_STANDARD_APP_ARGS(args, parse) \
	args.argc = __tris_app_separate_args(parse, ',', 1, args.argv, ((sizeof(args) - offsetof(typeof(args), argv)) / sizeof(args.argv[0])))
#define TRIS_STANDARD_RAW_ARGS(args, parse) \
	args.argc = __tris_app_separate_args(parse, ',', 0, args.argv, ((sizeof(args) - offsetof(typeof(args), argv)) / sizeof(args.argv[0])))

/*!
  \brief Performs the 'nonstandard' argument separation process for an application.
  \param args An argument structure defined using TRIS_DECLARE_APP_ARGS
  \param parse A modifiable buffer containing the input to be parsed
  \param sep A nonstandard separator character

  This function will separate the input string using the nonstandard argument
  separator character and fill in the provided structure, including
  the argc argument counter field.
 */
#define TRIS_NONSTANDARD_APP_ARGS(args, parse, sep) \
	args.argc = __tris_app_separate_args(parse, sep, 1, args.argv, ((sizeof(args) - offsetof(typeof(args), argv)) / sizeof(args.argv[0])))
#define TRIS_NONSTANDARD_RAW_ARGS(args, parse, sep) \
	args.argc = __tris_app_separate_args(parse, sep, 0, args.argv, ((sizeof(args) - offsetof(typeof(args), argv)) / sizeof(args.argv[0])))

/*!
  \brief Separate a string into arguments in an array
  \param buf The string to be parsed (this must be a writable copy, as it will be modified)
  \param delim The character to be used to delimit arguments
  \param remove_chars Remove backslashes and quote characters, while parsing
  \param array An array of 'char *' to be filled in with pointers to the found arguments
  \param arraylen The number of elements in the array (i.e. the number of arguments you will accept)

  Note: if there are more arguments in the string than the array will hold, the last element of
  the array will contain the remaining arguments, not separated.

  The array will be completely zeroed by this function before it populates any entries.

  \return The number of arguments found, or zero if the function arguments are not valid.
*/
unsigned int __tris_app_separate_args(char *buf, char delim, int remove_chars, char **array, int arraylen);
#define tris_app_separate_args(a,b,c,d)	__tris_app_separate_args(a,b,1,c,d)

/*!
  \brief A structure to hold the description of an application 'option'.

  Application 'options' are single-character flags that can be supplied
  to the application to affect its behavior; they can also optionally
  accept arguments enclosed in parenthesis.

  These structures are used by the tris_app_parse_options function, uses
  this data to fill in a flags structure (to indicate which options were
  supplied) and array of argument pointers (for those options that had
  arguments supplied).
 */
struct tris_app_option {
	/*! \brief The flag bit that represents this option. */
	uint64_t flag;
	/*! \brief The index of the entry in the arguments array
	  that should be used for this option's argument. */
	unsigned int arg_index;
};

#define BEGIN_OPTIONS {
#define END_OPTIONS }

/*!
  \brief Declares an array of options for an application.
  \param holder The name of the array to be created
  \param options The actual options to be placed into the array
  \sa tris_app_parse_options

  This macro declares a 'static const' array of \c struct \c tris_option
  elements to hold the list of available options for an application.
  Each option must be declared using either the TRIS_APP_OPTION()
  or TRIS_APP_OPTION_ARG() macros.

  Example usage:
  \code
  enum {
        OPT_JUMP = (1 << 0),
        OPT_BLAH = (1 << 1),
        OPT_BLORT = (1 << 2),
  } my_app_option_flags;

  enum {
        OPT_ARG_BLAH = 0,
        OPT_ARG_BLORT,
        !! this entry tells how many possible arguments there are,
           and must be the last entry in the list
        OPT_ARG_ARRAY_SIZE,
  } my_app_option_args;

  TRIS_APP_OPTIONS(my_app_options, {
        TRIS_APP_OPTION('j', OPT_JUMP),
        TRIS_APP_OPTION_ARG('b', OPT_BLAH, OPT_ARG_BLAH),
        TRIS_APP_OPTION_BLORT('B', OPT_BLORT, OPT_ARG_BLORT),
  });

  static int my_app_exec(struct tris_channel *chan, void *data)
  {
  	char *options;
	struct tris_flags opts = { 0, };
	char *opt_args[OPT_ARG_ARRAY_SIZE];

  	... do any argument parsing here ...

	if (tris_parseoptions(my_app_options, &opts, opt_args, options)) {
		tris_module_user_remove(u);
		return -1;
	}
  }
  \endcode
 */
#define TRIS_APP_OPTIONS(holder, options...) \
	static const struct tris_app_option holder[128] = options

/*!
  \brief Declares an application option that does not accept an argument.
  \param option The single character representing the option
  \param flagno The flag index to be set if this option is present
  \sa TRIS_APP_OPTIONS, tris_app_parse_options
 */
#define TRIS_APP_OPTION(option, flagno) \
	[option] = { .flag = flagno }

/*!
  \brief Declares an application option that accepts an argument.
  \param option The single character representing the option
  \param flagno The flag index to be set if this option is present
  \param argno The index into the argument array where the argument should
  be placed
  \sa TRIS_APP_OPTIONS, tris_app_parse_options
 */
#define TRIS_APP_OPTION_ARG(option, flagno, argno) \
	[option] = { .flag = flagno, .arg_index = argno + 1 }

/*!
  \brief Parses a string containing application options and sets flags/arguments.
  \param options The array of possible options declared with TRIS_APP_OPTIONS
  \param flags The flag structure to have option flags set
  \param args The array of argument pointers to hold arguments found
  \param optstr The string containing the options to be parsed
  \return zero for success, non-zero if an error occurs
  \sa TRIS_APP_OPTIONS
 */
int tris_app_parse_options(const struct tris_app_option *options, struct tris_flags *flags, char **args, char *optstr);

	/*!
  \brief Parses a string containing application options and sets flags/arguments.
  \param options The array of possible options declared with TRIS_APP_OPTIONS
  \param flags The 64-bit flag structure to have option flags set
  \param args The array of argument pointers to hold arguments found
  \param optstr The string containing the options to be parsed
  \return zero for success, non-zero if an error occurs
  \sa TRIS_APP_OPTIONS
 */
int tris_app_parse_options64(const struct tris_app_option *options, struct tris_flags64 *flags, char **args, char *optstr);

/*! \brief Given a list of options array, return an option string based on passed flags
	\param options The array of possible options declared with TRIS_APP_OPTIONS
	\param flags The flags of the options that you wish to populate the buffer with
	\param buf The buffer to fill with the string of options
	\param len The maximum length of buf
*/
void tris_app_options2str64(const struct tris_app_option *options, struct tris_flags64 *flags, char *buf, size_t len);

/*! \brief Present a dialtone and collect a certain length extension.
    \return Returns 1 on valid extension entered, -1 on hangup, or 0 on invalid extension.
\note Note that if 'collect' holds digits already, new digits will be appended, so be sure it's initialized properly */
int tris_app_dtget(struct tris_channel *chan, const char *context, char *collect, size_t size, int maxlen, int timeout);

/*! \brief Allow to record message and have a review option */
int tris_record_review(struct tris_channel *chan, const char *playfile, const char *recordfile, int maxtime, const char *fmt, int *duration, const char *path);

/*! \brief Decode an encoded control or extended ASCII character 
    \return Returns a pointer to the result string
*/
int tris_get_encoded_char(const char *stream, char *result, size_t *consumed);

/*! \brief Decode a stream of encoded control or extended ASCII characters */
char *tris_get_encoded_str(const char *stream, char *result, size_t result_len);

/*! \brief Decode a stream of encoded control or extended ASCII characters */
int tris_str_get_encoded_str(struct tris_str **str, int maxlen, const char *stream);

/*!
 * \brief Common routine for child processes, to close all fds prior to exec(2)
 * \param[in] n starting file descriptor number for closing all higher file descriptors
 * \since 1.6.1
 */
void tris_close_fds_above_n(int n);

/*!
 * \brief Common routine to safely fork without a chance of a signal handler firing badly in the child
 * \param[in] stop_reaper flag to determine if sigchld handler is replaced or not
 * \since 1.6.1
 */
int tris_safe_fork(int stop_reaper);

/*!
 * \brief Common routine to cleanup after fork'ed process is complete (if reaping was stopped)
 * \since 1.6.1
 */
void tris_safe_fork_cleanup(void);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_APP_H */
