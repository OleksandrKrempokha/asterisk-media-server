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
 * \brief Core PBX routines and definitions.
 */

#ifndef _TRISMEDIA_PBX_H
#define _TRISMEDIA_PBX_H

#include "trismedia/sched.h"
#include "trismedia/devicestate.h"
#include "trismedia/chanvars.h"
#include "trismedia/hashtab.h"
#include "trismedia/stringfields.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define TRIS_MAX_APP	32	/*!< Max length of an application */

#define TRIS_PBX_KEEP    0
#define TRIS_PBX_REPLACE 1

/*! \brief Special return values from applications to the PBX
 * @{ */
#define TRIS_PBX_HANGUP                -1    /*!< Jump to the 'h' exten */
#define TRIS_PBX_OK                     0    /*!< No errors */
#define TRIS_PBX_ERROR                  1    /*!< Jump to the 'e' exten */
#define TRIS_PBX_INCOMPLETE             12   /*!< Return to PBX matching, allowing more digits for the extension */
/*! @} */

#define PRIORITY_HINT	-1	/*!< Special Priority for a hint */

/*! \brief Extension states 
	\note States can be combined 
	- \ref AstExtState
*/
enum tris_extension_states {
	TRIS_EXTENSION_REMOVED = -2,	/*!< Extension removed */
	TRIS_EXTENSION_DEACTIVATED = -1,	/*!< Extension hint removed */
	TRIS_EXTENSION_NOT_INUSE = 0,	/*!< No device INUSE or BUSY  */
	TRIS_EXTENSION_INUSE = 1 << 0,	/*!< One or more devices INUSE */
	TRIS_EXTENSION_BUSY = 1 << 1,	/*!< All devices BUSY */
	TRIS_EXTENSION_UNAVAILABLE = 1 << 2, /*!< All devices UNAVAILABLE/UNREGISTERED */
	TRIS_EXTENSION_RINGING = 1 << 3,	/*!< All devices RINGING */
	TRIS_EXTENSION_ONHOLD = 1 << 4,	/*!< All devices ONHOLD */
};


struct tris_context;
struct tris_exten;     
struct tris_include;
struct tris_ignorepat;
struct tris_sw;

/*! \brief Typedef for devicestate and hint callbacks */
typedef int (*tris_state_cb_type)(char *context, char* id, enum tris_extension_states state, void *data);

/*! \brief From where the documentation come from */
enum tris_doc_src {
	TRIS_XML_DOC,            /*!< From XML documentation */
	TRIS_STATIC_DOC          /*!< From application/function registration */
};

/*! \brief Data structure associated with a custom dialplan function */
struct tris_custom_function {
	const char *name;			/*!< Name */
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(synopsis);     /*!< Synopsis text for 'show functions' */
		TRIS_STRING_FIELD(desc);		/*!< Description (help text) for 'show functions &lt;name&gt;' */
		TRIS_STRING_FIELD(syntax);       /*!< Syntax text for 'core show functions' */
		TRIS_STRING_FIELD(arguments);    /*!< Arguments description */
		TRIS_STRING_FIELD(seealso);      /*!< See also */
	);
	enum tris_doc_src docsrc;		/*!< Where the documentation come from */
	int (*read)(struct tris_channel *, const char *, char *, char *, size_t);	/*!< Read function, if read is supported */
	int (*write)(struct tris_channel *, const char *, char *, const char *);		/*!< Write function, if write is supported */
	struct tris_module *mod;         /*!< Module this custom function belongs to */
	TRIS_RWLIST_ENTRY(tris_custom_function) acflist;
};

/*! \brief All switch functions have the same interface, so define a type for them */
typedef int (tris_switch_f)(struct tris_channel *chan, const char *context,
	const char *exten, int priority, const char *callerid, const char *data);

/*!< Data structure associated with an Trismedia switch */
struct tris_switch {
	TRIS_LIST_ENTRY(tris_switch) list;
	const char *name;			/*!< Name of the switch */
	const char *description;		/*!< Description of the switch */
	
	tris_switch_f *exists;
	tris_switch_f *canmatch;
	tris_switch_f *exec;
	tris_switch_f *matchmore;
};

struct tris_timing {
	int hastime;                    /*!< If time construct exists */
	unsigned int monthmask;         /*!< Mask for month */
	unsigned int daymask;           /*!< Mask for date */
	unsigned int dowmask;           /*!< Mask for day of week (sun-sat) */
	unsigned int minmask[48];       /*!< Mask for minute */
	char *timezone;                 /*!< NULL, or zoneinfo style timezone */
};

/*!\brief Construct a timing bitmap, for use in time-based conditionals.
 * \param i Pointer to an tris_timing structure.
 * \param info Standard string containing a timerange, weekday range, monthday range, and month range, as well as an optional timezone.
 * \retval Returns 1 on success or 0 on failure.
 */
int tris_build_timing(struct tris_timing *i, const char *info);

/*!\brief Evaluate a pre-constructed bitmap as to whether the current time falls within the range specified.
 * \param i Pointer to an tris_timing structure.
 * \retval Returns 1, if the time matches or 0, if the current time falls outside of the specified range.
 */
int tris_check_timing(const struct tris_timing *i);

/*!\brief Deallocates memory structures associated with a timing bitmap.
 * \param i Pointer to an tris_timing structure.
 * \retval 0 success
 * \retval non-zero failure (number suitable to pass to \see strerror)
 */
int tris_destroy_timing(struct tris_timing *i);

struct tris_pbx {
	int dtimeoutms;				/*!< Timeout between digits (milliseconds) */
	int rtimeoutms;				/*!< Timeout for response (milliseconds) */
};


/*!
 * \brief Register an alternative dialplan switch
 *
 * \param sw switch to register
 *
 * This function registers a populated tris_switch structure with the
 * trismedia switching architecture.
 *
 * \retval 0 success
 * \retval non-zero failure
 */
int tris_register_switch(struct tris_switch *sw);

/*!
 * \brief Unregister an alternative switch
 *
 * \param sw switch to unregister
 * 
 * Unregisters a switch from trismedia.
 *
 * \return nothing
 */
void tris_unregister_switch(struct tris_switch *sw);

/*!
 * \brief Look up an application
 *
 * \param app name of the app
 *
 * This function searches for the tris_app structure within
 * the apps that are registered for the one with the name
 * you passed in.
 *
 * \return the tris_app structure that matches on success, or NULL on failure
 */
struct tris_app *pbx_findapp(const char *app);

/*!
 * \brief Execute an application
 *
 * \param c channel to execute on
 * \param app which app to execute
 * \param data the data passed into the app
 *
 * This application executes an application on a given channel.  It
 * saves the stack and executes the given application passing in
 * the given data.
 *
 * \retval 0 success
 * \retval -1 failure
 */
int pbx_exec(struct tris_channel *c, struct tris_app *app, void *data);

/*!
 * \brief Register a new context or find an existing one
 *
 * \param extcontexts pointer to the tris_context structure pointer
 * \param exttable pointer to the hashtable that contains all the elements in extcontexts
 * \param name name of the new context
 * \param registrar registrar of the context
 *
 * This function allows you to play in two environments: the global contexts (active dialplan)
 * or an external context set of your choosing. To act on the external set, make sure extcontexts
 * and exttable are set; for the globals, make sure both extcontexts and exttable are NULL.
 *
 * This will first search for a context with your name.  If it exists already, it will not
 * create a new one.  If it does not exist, it will create a new one with the given name
 * and registrar.
 *
 * \return NULL on failure, and an tris_context structure on success
 */
struct tris_context *tris_context_find_or_create(struct tris_context **extcontexts, struct tris_hashtab *exttable, const char *name, const char *registrar);

/*!
 * \brief Merge the temporary contexts into a global contexts list and delete from the 
 *        global list the ones that are being added
 *
 * \param extcontexts pointer to the tris_context structure
 * \param exttable pointer to the tris_hashtab structure that contains all the elements in extcontexts
 * \param registrar of the context; if it's set the routine will delete all contexts 
 *        that belong to that registrar; if NULL only the contexts that are specified 
 *        in extcontexts
 */
void tris_merge_contexts_and_delete(struct tris_context **extcontexts, struct tris_hashtab *exttable, const char *registrar);

/*!
 * \brief Destroy a context (matches the specified context (or ANY context if NULL)
 *
 * \param con context to destroy
 * \param registrar who registered it
 *
 * You can optionally leave out either parameter.  It will find it
 * based on either the tris_context or the registrar name.
 *
 * \return nothing
 */
void tris_context_destroy(struct tris_context *con, const char *registrar);

/*!
 * \brief Find a context
 *
 * \param name name of the context to find
 *
 * Will search for the context with the given name.
 *
 * \return the tris_context on success, NULL on failure.
 */
struct tris_context *tris_context_find(const char *name);

/*! \brief The result codes when starting the PBX on a channel with \see tris_pbx_start.
	TRIS_PBX_CALL_LIMIT refers to the maxcalls call limit in trismedia.conf
 */
enum tris_pbx_result {
	TRIS_PBX_SUCCESS = 0,
	TRIS_PBX_FAILED = -1,
	TRIS_PBX_CALL_LIMIT = -2,
};

/*!
 * \brief Create a new thread and start the PBX
 *
 * \param c channel to start the pbx on
 *
 * \see tris_pbx_run for a synchronous function to run the PBX in the
 * current thread, as opposed to starting a new one.
 *
 * \retval Zero on success
 * \retval non-zero on failure
 */
enum tris_pbx_result tris_pbx_start(struct tris_channel *c);

/*!
 * \brief Execute the PBX in the current thread
 *
 * \param c channel to run the pbx on
 *
 * This executes the PBX on a given channel. It allocates a new
 * PBX structure for the channel, and provides all PBX functionality.
 * See tris_pbx_start for an asynchronous function to run the PBX in a
 * new thread as opposed to the current one.
 * 
 * \retval Zero on success
 * \retval non-zero on failure
 */
enum tris_pbx_result tris_pbx_run(struct tris_channel *c);

/*!
 * \brief Options for tris_pbx_run()
 */
struct tris_pbx_args {
	union {
		/*! Pad this out so that we have plenty of room to add options
		 *  but still maintain ABI compatibility over time. */
		uint64_t __padding;
		struct {
			/*! Do not hangup the channel when the PBX is complete. */
			unsigned int no_hangup_chan:1;
		};
	};
};

/*!
 * \brief Execute the PBX in the current thread
 *
 * \param c channel to run the pbx on
 * \param args options for the pbx
 *
 * This executes the PBX on a given channel. It allocates a new
 * PBX structure for the channel, and provides all PBX functionality.
 * See tris_pbx_start for an asynchronous function to run the PBX in a
 * new thread as opposed to the current one.
 * 
 * \retval Zero on success
 * \retval non-zero on failure
 */
enum tris_pbx_result tris_pbx_run_args(struct tris_channel *c, struct tris_pbx_args *args);

/*! 
 * \brief Add and extension to an extension context.  
 * 
 * \param context context to add the extension to
 * \param replace
 * \param extension extension to add
 * \param priority priority level of extension addition
 * \param label extension label
 * \param callerid pattern to match CallerID, or NULL to match any CallerID
 * \param application application to run on the extension with that priority level
 * \param data data to pass to the application
 * \param datad
 * \param registrar who registered the extension
 *
 * \retval 0 success 
 * \retval -1 failure
 */
int tris_add_extension(const char *context, int replace, const char *extension, 
	int priority, const char *label, const char *callerid,
	const char *application, void *data, void (*datad)(void *), const char *registrar);

/*! 
 * \brief Add an extension to an extension context, this time with an tris_context *.
 *
 * \note For details about the arguments, check tris_add_extension()
 */
int tris_add_extension2(struct tris_context *con, int replace, const char *extension,
	int priority, const char *label, const char *callerid, 
	const char *application, void *data, void (*datad)(void *), const char *registrar);

/*!
 * \brief Map devstate to an extension state.
 *
 * \param[in] device state
 *
 * \return the extension state mapping.
 */
enum tris_extension_states tris_devstate_to_extenstate(enum tris_device_state devstate);

/*! 
 * \brief Uses hint and devicestate callback to get the state of an extension
 *
 * \param c this is not important
 * \param context which context to look in
 * \param exten which extension to get state
 *
 * \return extension state as defined in the tris_extension_states enum
 */
int tris_extension_state(struct tris_channel *c, const char *context, const char *exten);

/*! 
 * \brief Return string representation of the state of an extension
 * 
 * \param extension_state is the numerical state delivered by tris_extension_state
 *
 * \return the state of an extension as string
 */
const char *tris_extension_state2str(int extension_state);

/*!
 * \brief Registers a state change callback
 * 
 * \param context which context to look in
 * \param exten which extension to get state
 * \param callback callback to call if state changed
 * \param data to pass to callback
 *
 * The callback is called if the state of an extension is changed.
 *
 * \retval -1 on failure
 * \retval ID on success
 */ 
int tris_extension_state_add(const char *context, const char *exten, 
			    tris_state_cb_type callback, void *data);

/*! 
 * \brief Deletes a registered state change callback by ID
 * 
 * \param id of the callback to delete
 * \param callback callback
 *
 * Removes the callback from list of callbacks
 *
 * \retval 0 success 
 * \retval -1 failure
 */
int tris_extension_state_del(int id, tris_state_cb_type callback);

/*! 
 * \brief If an extension hint exists, return non-zero
 * 
 * \param hint buffer for hint
 * \param hintsize size of hint buffer, in bytes
 * \param name buffer for name portion of hint
 * \param namesize size of name buffer
 * \param c Channel from which to return the hint.  This is only important when the hint or name contains an expression to be expanded.
 * \param context which context to look in
 * \param exten which extension to search for
 *
 * \return If an extension within the given context with the priority PRIORITY_HINT
 * is found, a non zero value will be returned.
 * Otherwise, 0 is returned.
 */
int tris_get_hint(char *hint, int hintsize, char *name, int namesize,
	struct tris_channel *c, const char *context, const char *exten);

/*!
 * \brief Determine whether an extension exists
 *
 * \param c this is not important
 * \param context which context to look in
 * \param exten which extension to search for
 * \param priority priority of the action within the extension
 * \param callerid callerid to search for
 *
 * \note It is possible for autoservice to be started and stopped on c during this
 * function call, it is important that c is not locked prior to calling this. Otherwise
 * a deadlock may occur
 *
 * \return If an extension within the given context(or callerid) with the given priority 
 *         is found a non zero value will be returned. Otherwise, 0 is returned.
 */
int tris_exists_extension(struct tris_channel *c, const char *context, const char *exten, 
	int priority, const char *callerid);

/*! 
 * \brief Find the priority of an extension that has the specified label
 * 
 * \param c this is not important
 * \param context which context to look in
 * \param exten which extension to search for
 * \param label label of the action within the extension to match to priority
 * \param callerid callerid to search for
 *
 * \note It is possible for autoservice to be started and stopped on c during this
 * function call, it is important that c is not locked prior to calling this. Otherwise
 * a deadlock may occur
 *
 * \retval the priority which matches the given label in the extension
 * \retval -1 if not found.
 */
int tris_findlabel_extension(struct tris_channel *c, const char *context, 
	const char *exten, const char *label, const char *callerid);

/*!
 * \brief Find the priority of an extension that has the specified label
 *
 * \note It is possible for autoservice to be started and stopped on c during this
 * function call, it is important that c is not locked prior to calling this. Otherwise
 * a deadlock may occur
 *
 * \note This function is the same as tris_findlabel_extension, except that it accepts
 * a pointer to an tris_context structure to specify the context instead of the
 * name of the context. Otherwise, the functions behave the same.
 */
int tris_findlabel_extension2(struct tris_channel *c, struct tris_context *con, 
	const char *exten, const char *label, const char *callerid);

/*! 
 * \brief Looks for a valid matching extension
 * 
 * \param c not really important
 * \param context context to serach within
 * \param exten extension to check
 * \param priority priority of extension path
 * \param callerid callerid of extension being searched for
 *
 * \note It is possible for autoservice to be started and stopped on c during this
 * function call, it is important that c is not locked prior to calling this. Otherwise
 * a deadlock may occur
 *
 * \return If "exten" *could be* a valid extension in this context with or without
 * some more digits, return non-zero.  Basically, when this returns 0, no matter
 * what you add to exten, it's not going to be a valid extension anymore
 */
int tris_canmatch_extension(struct tris_channel *c, const char *context, 
	const char *exten, int priority, const char *callerid);

/*! 
 * \brief Looks to see if adding anything to this extension might match something. (exists ^ canmatch)
 *
 * \param c not really important XXX
 * \param context context to serach within
 * \param exten extension to check
 * \param priority priority of extension path
 * \param callerid callerid of extension being searched for
 *
 * \note It is possible for autoservice to be started and stopped on c during this
 * function call, it is important that c is not locked prior to calling this. Otherwise
 * a deadlock may occur
 *
 * \return If "exten" *could match* a valid extension in this context with
 * some more digits, return non-zero.  Does NOT return non-zero if this is
 * an exact-match only.  Basically, when this returns 0, no matter
 * what you add to exten, it's not going to be a valid extension anymore
 */
int tris_matchmore_extension(struct tris_channel *c, const char *context, 
	const char *exten, int priority, const char *callerid);

/*! 
 * \brief Determine if a given extension matches a given pattern (in NXX format)
 * 
 * \param pattern pattern to match
 * \param extension extension to check against the pattern.
 *
 * Checks whether or not the given extension matches the given pattern.
 *
 * \retval 1 on match
 * \retval 0 on failure
 */
int tris_extension_match(const char *pattern, const char *extension);

int tris_extension_close(const char *pattern, const char *data, int needmore);

/*! 
 * \brief Determine if one extension should match before another
 * 
 * \param a extension to compare with b
 * \param b extension to compare with a
 *
 * Checks whether or extension a should match before extension b
 *
 * \retval 0 if the two extensions have equal matching priority
 * \retval 1 on a > b
 * \retval -1 on a < b
 */
int tris_extension_cmp(const char *a, const char *b);

/*! 
 * \brief Launch a new extension (i.e. new stack)
 * 
 * \param c not important
 * \param context which context to generate the extension within
 * \param exten new extension to add
 * \param priority priority of new extension
 * \param callerid callerid of extension
 * \param found
 * \param combined_find_spawn 
 *
 * This adds a new extension to the trismedia extension list.
 *
 * \note It is possible for autoservice to be started and stopped on c during this
 * function call, it is important that c is not locked prior to calling this. Otherwise
 * a deadlock may occur
 *
 * \retval 0 on success 
 * \retval -1 on failure.
 */
int tris_spawn_extension(struct tris_channel *c, const char *context, 
      const char *exten, int priority, const char *callerid, int *found, int combined_find_spawn);

/*! 
 * \brief Add a context include
 *
 * \param context context to add include to
 * \param include new include to add
 * \param registrar who's registering it
 *
 * Adds an include taking a char * string as the context parameter
 *
 * \retval 0 on success 
 * \retval -1 on error
*/
int tris_context_add_include(const char *context, const char *include, 
	const char *registrar);

/*! 
 * \brief Add a context include
 * 
 * \param con context to add the include to
 * \param include include to add
 * \param registrar who registered the context
 *
 * Adds an include taking a struct tris_context as the first parameter
 *
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_context_add_include2(struct tris_context *con, const char *include, 
	const char *registrar);

/*! 
 * \brief Remove a context include
 * 
 * \note See tris_context_add_include for information on arguments
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_context_remove_include(const char *context, const char *include, 
	const char *registrar);

/*! 
 * \brief Removes an include by an tris_context structure 
 * 
 * \note See tris_context_add_include2 for information on arguments
 *
 * \retval 0 on success
 * \retval -1 on success
 */
int tris_context_remove_include2(struct tris_context *con, const char *include, 
	const char *registrar);

/*! 
 * \brief Verifies includes in an tris_contect structure
 * 
 * \param con context in which to verify the includes
 *
 * \retval 0 if no problems found 
 * \retval -1 if there were any missing context
 */
int tris_context_verify_includes(struct tris_context *con);
	  
/*! 
 * \brief Add a switch
 * 
 * \param context context to which to add the switch
 * \param sw switch to add
 * \param data data to pass to switch
 * \param eval whether to evaluate variables when running switch
 * \param registrar whoever registered the switch
 *
 * This function registers a switch with the trismedia switch architecture
 *
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_context_add_switch(const char *context, const char *sw, const char *data, 
	int eval, const char *registrar);

/*! 
 * \brief Adds a switch (first param is a tris_context)
 * 
 * \note See tris_context_add_switch() for argument information, with the exception of
 *       the first argument. In this case, it's a pointer to an tris_context structure
 *       as opposed to the name.
 */
int tris_context_add_switch2(struct tris_context *con, const char *sw, const char *data, 
	int eval, const char *registrar);

/*! 
 * \brief Remove a switch
 * 
 * Removes a switch with the given parameters
 *
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_context_remove_switch(const char *context, const char *sw, 
	const char *data, const char *registrar);

int tris_context_remove_switch2(struct tris_context *con, const char *sw, 
	const char *data, const char *registrar);

/*! 
 * \brief Simply remove extension from context
 * 
 * \param context context to remove extension from
 * \param extension which extension to remove
 * \param priority priority of extension to remove (0 to remove all)
 * \param callerid NULL to remove all; non-NULL to match a single record per priority
 * \param matchcid non-zero to match callerid element (if non-NULL); 0 to match default case
 * \param registrar registrar of the extension
 *
 * This function removes an extension from a given context.
 *
 * \retval 0 on success 
 * \retval -1 on failure
 *
 * @{
 */
int tris_context_remove_extension(const char *context, const char *extension, int priority,
	const char *registrar);

int tris_context_remove_extension2(struct tris_context *con, const char *extension,
	int priority, const char *registrar, int already_locked);

int tris_context_remove_extension_callerid(const char *context, const char *extension,
	int priority, const char *callerid, int matchcid, const char *registrar);

int tris_context_remove_extension_callerid2(struct tris_context *con, const char *extension,
	int priority, const char *callerid, int matchcid, const char *registrar,
	int already_locked);
/*! @} */

/*! 
 * \brief Add an ignorepat
 * 
 * \param context which context to add the ignorpattern to
 * \param ignorepat ignorepattern to set up for the extension
 * \param registrar registrar of the ignore pattern
 *
 * Adds an ignore pattern to a particular context.
 *
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_context_add_ignorepat(const char *context, const char *ignorepat, const char *registrar);

int tris_context_add_ignorepat2(struct tris_context *con, const char *ignorepat, const char *registrar);

/* 
 * \brief Remove an ignorepat
 * 
 * \param context context from which to remove the pattern
 * \param ignorepat the pattern to remove
 * \param registrar the registrar of the ignore pattern
 *
 * This removes the given ignorepattern
 *
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_context_remove_ignorepat(const char *context, const char *ignorepat, const char *registrar);

int tris_context_remove_ignorepat2(struct tris_context *con, const char *ignorepat, const char *registrar);

/*! 
 * \brief Checks to see if a number should be ignored
 * 
 * \param context context to search within
 * \param pattern to check whether it should be ignored or not
 *
 * Check if a number should be ignored with respect to dialtone cancellation.
 *
 * \retval 0 if the pattern should not be ignored 
 * \retval non-zero if the pattern should be ignored 
 */
int tris_ignore_pattern(const char *context, const char *pattern);

/* Locking functions for outer modules, especially for completion functions */

/*! 
 * \brief Write locks the context list
 *
 * \retval 0 on success 
 * \retval -1 on error
 */
int tris_wrlock_contexts(void);

/*!
 * \brief Read locks the context list
 *
 * \retval 0 on success
 * \retval -1 on error
 */
int tris_rdlock_contexts(void);

/*! 
 * \brief Unlocks contexts
 * 
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_unlock_contexts(void);

/*! 
 * \brief Write locks a given context
 * 
 * \param con context to lock
 *
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_wrlock_context(struct tris_context *con);

/*!
 * \brief Read locks a given context
 *
 * \param con context to lock
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_rdlock_context(struct tris_context *con);

/*! 
 * \retval Unlocks the given context
 * 
 * \param con context to unlock
 *
 * \retval 0 on success 
 * \retval -1 on failure
 */
int tris_unlock_context(struct tris_context *con);

/*! 
 * \brief locks the macrolock in the given given context
 *
 * \param macrocontext name of the macro-context to lock
 *
 * Locks the given macro-context to ensure only one thread (call) can execute it at a time
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_context_lockmacro(const char *macrocontext);

/*!
 * \brief Unlocks the macrolock in the given context
 *
 * \param macrocontext name of the macro-context to unlock
 *
 * Unlocks the given macro-context so that another thread (call) can execute it
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_context_unlockmacro(const char *macrocontext);

/*!\brief Set the channel to next execute the specified dialplan location.
 * \see tris_async_parseable_goto, tris_async_goto_if_exists
 */
int tris_async_goto(struct tris_channel *chan, const char *context, const char *exten, int priority);

/*!\brief Set the channel to next execute the specified dialplan location.
 */
int tris_async_goto_by_name(const char *chan, const char *context, const char *exten, int priority);

/*! Synchronously or asynchronously make an outbound call and send it to a
   particular extension */
int tris_pbx_outgoing_exten(const char *type, int format, void *data, int timeout, const char *context, const char *exten, int priority, int *reason, int sync, const char *cid_num, const char *cid_name, struct tris_variable *vars, const char *account, struct tris_channel **locked_channel);

/*! Synchronously or asynchronously make an outbound call and send it to a
   particular application with given extension */
int tris_pbx_outgoing_app(const char *type, int format, void *data, int timeout, const char *app, const char *appdata, int *reason, int sync, const char *cid_num, const char *cid_name, struct tris_variable *vars, const char *account, struct tris_channel **locked_channel);

/*!
 * \brief Evaluate a condition
 *
 * \retval 0 if the condition is NULL or of zero length
 * \retval int If the string is an integer, the integer representation of
 *             the integer is returned
 * \retval 1 Any other non-empty string
 */
int pbx_checkcondition(const char *condition);

/*! @name 
 * Functions for returning values from structures */
/*! @{ */
const char *tris_get_context_name(struct tris_context *con);
const char *tris_get_extension_name(struct tris_exten *exten);
struct tris_context *tris_get_extension_context(struct tris_exten *exten);
const char *tris_get_include_name(struct tris_include *include);
const char *tris_get_ignorepat_name(struct tris_ignorepat *ip);
const char *tris_get_switch_name(struct tris_sw *sw);
const char *tris_get_switch_data(struct tris_sw *sw);
int tris_get_switch_eval(struct tris_sw *sw);
	
/*! @} */

/*! @name Other Extension stuff */
/*! @{ */
int tris_get_extension_priority(struct tris_exten *exten);
int tris_get_extension_matchcid(struct tris_exten *e);
const char *tris_get_extension_cidmatch(struct tris_exten *e);
const char *tris_get_extension_app(struct tris_exten *e);
const char *tris_get_extension_label(struct tris_exten *e);
void *tris_get_extension_app_data(struct tris_exten *e);
/*! @} */

/*! @name Registrar info functions ... */
/*! @{ */
const char *tris_get_context_registrar(struct tris_context *c);
const char *tris_get_extension_registrar(struct tris_exten *e);
const char *tris_get_include_registrar(struct tris_include *i);
const char *tris_get_ignorepat_registrar(struct tris_ignorepat *ip);
const char *tris_get_switch_registrar(struct tris_sw *sw);
/*! @} */

/*! @name Walking functions ... */
/*! @{ */
struct tris_context *tris_walk_contexts(struct tris_context *con);
struct tris_exten *tris_walk_context_extensions(struct tris_context *con,
	struct tris_exten *priority);
struct tris_exten *tris_walk_extension_priorities(struct tris_exten *exten,
	struct tris_exten *priority);
struct tris_include *tris_walk_context_includes(struct tris_context *con,
	struct tris_include *inc);
struct tris_ignorepat *tris_walk_context_ignorepats(struct tris_context *con,
	struct tris_ignorepat *ip);
struct tris_sw *tris_walk_context_switches(struct tris_context *con, struct tris_sw *sw);
/*! @} */

/*!\brief Create a human-readable string, specifying all variables and their corresponding values.
 * \param chan Channel from which to read variables
 * \param buf Dynamic string in which to place the result (should be allocated with \see tris_str_create).
 * \note Will lock the channel.
 */
int pbx_builtin_serialize_variables(struct tris_channel *chan, struct tris_str **buf);

/*!\brief Return a pointer to the value of the corresponding channel variable.
 * \note Will lock the channel.
 *
 * \note This function will return a pointer to the buffer inside the channel
 * variable.  This value should only be accessed with the channel locked.  If
 * the value needs to be kept around, it should be done by using the following
 * thread-safe code:
 * \code
 *		const char *var;
 *
 *		tris_channel_lock(chan);
 *		if ((var = pbx_builtin_getvar_helper(chan, "MYVAR"))) {
 *			var = tris_strdupa(var);
 *		}
 *		tris_channel_unlock(chan);
 * \endcode
 */
const char *pbx_builtin_getvar_helper(struct tris_channel *chan, const char *name);

/*!\brief Add a variable to the channel variable stack, without removing any previously set value.
 * \note Will lock the channel.
 */
void pbx_builtin_pushvar_helper(struct tris_channel *chan, const char *name, const char *value);

/*!\brief Add a variable to the channel variable stack, removing the most recently set value for the same name.
 * \note Will lock the channel.  May also be used to set a channel dialplan function to a particular value.
 * \see tris_func_write
 */
void pbx_builtin_setvar_helper(struct tris_channel *chan, const char *name, const char *value);

/*!\brief Retrieve the value of a builtin variable or variable from the channel variable stack.
 * \note Will lock the channel.
 */
void pbx_retrieve_variable(struct tris_channel *c, const char *var, char **ret, char *workspace, int workspacelen, struct varshead *headp);
void pbx_builtin_clear_globals(void);

/*!\brief Parse and set a single channel variable, where the name and value are separated with an '=' character.
 * \note Will lock the channel.
 */
int pbx_builtin_setvar(struct tris_channel *chan, void *data);

/*!\brief Parse and set multiple channel variables, where the pairs are separated by the ',' character, and name and value are separated with an '=' character.
 * \note Will lock the channel.
 */
int pbx_builtin_setvar_multiple(struct tris_channel *chan, void *data);

int pbx_builtin_raise_exception(struct tris_channel *chan, void *data);

/*! @name Substitution routines, using static string buffers
 * @{ */
void pbx_substitute_variables_helper(struct tris_channel *c, const char *cp1, char *cp2, int count);
void pbx_substitute_variables_varshead(struct varshead *headp, const char *cp1, char *cp2, int count);
void pbx_substitute_variables_helper_full(struct tris_channel *c, struct varshead *headp, const char *cp1, char *cp2, int cp2_size, size_t *used);
void tris_str_substitute_variables(struct tris_str **buf, size_t maxlen, struct tris_channel *chan, const char *templ);
/*! @} */

int tris_extension_patmatch(const char *pattern, const char *data);

/*! Set "autofallthrough" flag, if newval is <0, does not actually set.  If
  set to 1, sets to auto fall through.  If newval set to 0, sets to no auto
  fall through (reads extension instead).  Returns previous value. */
int pbx_set_autofallthrough(int newval);

/*! Set "extenpatternmatchnew" flag, if newval is <0, does not actually set.  If
  set to 1, sets to use the new Trie-based pattern matcher.  If newval set to 0, sets to use
  the old linear-search algorithm.  Returns previous value. */
int pbx_set_extenpatternmatchnew(int newval);

/*! Set "overrideswitch" field.  If set and of nonzero length, all contexts
 * will be tried directly through the named switch prior to any other
 * matching within that context.
 * \since 1.6.1
 */ 
void pbx_set_overrideswitch(const char *newval);

/*!
 * \note This function will handle locking the channel as needed.
 */
int tris_goto_if_exists(struct tris_channel *chan, const char *context, const char *exten, int priority);

/*!
 * \note This function will handle locking the channel as needed.
 */
int tris_parseable_goto(struct tris_channel *chan, const char *goto_string);

/*!
 * \note This function will handle locking the channel as needed.
 */
int tris_async_parseable_goto(struct tris_channel *chan, const char *goto_string);

/*!
 * \note This function will handle locking the channel as needed.
 */
int tris_explicit_goto(struct tris_channel *chan, const char *context, const char *exten, int priority);

/*!
 * \note This function will handle locking the channel as needed.
 */
int tris_async_goto_if_exists(struct tris_channel *chan, const char *context, const char *exten, int priority);

struct tris_custom_function* tris_custom_function_find(const char *name);

/*!
 * \brief Unregister a custom function
 */
int tris_custom_function_unregister(struct tris_custom_function *acf);

/*!
 * \brief Register a custom function
 */
#define tris_custom_function_register(acf) __tris_custom_function_register(acf, tris_module_info->self)

/*!
 * \brief Register a custom function
 */
int __tris_custom_function_register(struct tris_custom_function *acf, struct tris_module *mod);

/*! 
 * \brief Retrieve the number of active calls
 */
int tris_active_calls(void);

/*! 
 * \brief Retrieve the total number of calls processed through the PBX since last restart
 */
int tris_processed_calls(void);
	
/*!
 * \brief executes a read operation on a function 
 *
 * \param chan Channel to execute on
 * \param function Data containing the function call string (will be modified)
 * \param workspace A pointer to safe memory to use for a return value 
 * \param len the number of bytes in workspace
 *
 * This application executes a function in read mode on a given channel.
 *
 * \retval 0 success
 * \retval non-zero failure
 */
int tris_func_read(struct tris_channel *chan, const char *function, char *workspace, size_t len);

/*!
 * \brief executes a write operation on a function
 *
 * \param chan Channel to execute on
 * \param function Data containing the function call string (will be modified)
 * \param value A value parameter to pass for writing
 *
 * This application executes a function in write mode on a given channel.
 *
 * \retval 0 success
 * \retval non-zero failure
 */
int tris_func_write(struct tris_channel *chan, const char *function, const char *value);

/*!
 * When looking up extensions, we can have different requests
 * identified by the 'action' argument, as follows.
 * Note that the coding is such that the low 4 bits are the
 * third argument to extension_match_core.
 */

enum ext_match_t {
	E_MATCHMORE = 	0x00,	/* extension can match but only with more 'digits' */
	E_CANMATCH =	0x01,	/* extension can match with or without more 'digits' */
	E_MATCH =	0x02,	/* extension is an exact match */
	E_MATCH_MASK =	0x03,	/* mask for the argument to extension_match_core() */
	E_SPAWN =	0x12,	/* want to spawn an extension. Requires exact match */
	E_FINDLABEL =	0x22	/* returns the priority for a given label. Requires exact match */
};

#define STATUS_NO_CONTEXT	1
#define STATUS_NO_EXTENSION	2
#define STATUS_NO_PRIORITY	3
#define STATUS_NO_LABEL		4
#define STATUS_SUCCESS		5 
#define TRIS_PBX_MAX_STACK  128

/* request and result for pbx_find_extension */
struct pbx_find_info {
#if 0
	const char *context;
	const char *exten;
	int priority;
#endif

	char *incstack[TRIS_PBX_MAX_STACK];      /* filled during the search */
	int stacklen;                   /* modified during the search */
	int status;                     /* set on return */
	struct tris_switch *swo;         /* set on return */
	const char *data;               /* set on return */
	const char *foundcontext;       /* set on return */
};
 
struct tris_exten *pbx_find_extension(struct tris_channel *chan,
									 struct tris_context *bypass, struct pbx_find_info *q,
									 const char *context, const char *exten, int priority,
									 const char *label, const char *callerid, enum ext_match_t action);


/* every time a write lock is obtained for contexts,
   a counter is incremented. You can check this via the
   following func */

int tris_wrlock_contexts_version(void);
	

/*!\brief hashtable functions for contexts */
/*! @{ */
int tris_hashtab_compare_contexts(const void *ah_a, const void *ah_b);
unsigned int tris_hashtab_hash_contexts(const void *obj);
/*! @} */

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_PBX_H */
