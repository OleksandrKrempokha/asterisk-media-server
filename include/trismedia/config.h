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
 * \brief Configuration File Parser
 */

#ifndef _TRISMEDIA_CONFIG_H
#define _TRISMEDIA_CONFIG_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include "trismedia/utils.h"
#include "trismedia/inline_api.h"

struct tris_config;

struct tris_category;

/*! Options for tris_config_load()
 */
enum {
	/*! Load the configuration, including comments */
	CONFIG_FLAG_WITHCOMMENTS  = (1 << 0),
	/*! On a reload, give us a -1 if the file hasn't changed. */
	CONFIG_FLAG_FILEUNCHANGED = (1 << 1),
	/*! Don't attempt to cache mtime on this config file. */
	CONFIG_FLAG_NOCACHE       = (1 << 2),
};

#define	CONFIG_STATUS_FILEMISSING	(void *)0
#define	CONFIG_STATUS_FILEUNCHANGED	(void *)-1
#define	CONFIG_STATUS_FILEINVALID	(void *)-2

/*!
 * \brief Types used in tris_realtime_require_field
 */
typedef enum {
	RQ_INTEGER1,
	RQ_UINTEGER1,
	RQ_INTEGER2,
	RQ_UINTEGER2,
	RQ_INTEGER3,
	RQ_UINTEGER3,
	RQ_INTEGER4,
	RQ_UINTEGER4,
	RQ_INTEGER8,
	RQ_UINTEGER8,
	RQ_CHAR,
	RQ_FLOAT,
	RQ_DATE,
	RQ_DATETIME,
} require_type;

/*! \brief Structure for variables, used for configurations and for channel variables
*/
struct tris_variable {
	const char *name;
	const char *value;
	struct tris_variable *next;

	char *file;

	int lineno;
	int object;		/*!< 0 for variable, 1 for object */
	int blanklines; 	/*!< Number of blanklines following entry */
	struct tris_comment *precomments;
	struct tris_comment *sameline;
	struct tris_comment *trailing; /*!< the last object in the list will get assigned any trailing comments when EOF is hit */
	char stuff[0];
};

typedef struct tris_config *config_load_func(const char *database, const char *table, const char *configfile, struct tris_config *config, struct tris_flags flags, const char *suggested_include_file, const char *who_asked);
typedef struct tris_variable *realtime_var_get(const char *database, const char *table, va_list ap);
typedef struct tris_config *realtime_multi_get(const char *database, const char *table, va_list ap);
typedef int realtime_update(const char *database, const char *table, const char *keyfield, const char *entity, va_list ap);
typedef int realtime_update2(const char *database, const char *table, va_list ap);
typedef int realtime_store(const char *database, const char *table, va_list ap);
typedef int realtime_destroy(const char *database, const char *table, const char *keyfield, const char *entity, va_list ap);

/*!
 * \brief Function pointer called to ensure database schema is properly configured for realtime use
 * \since 1.6.1
 */
typedef int realtime_require(const char *database, const char *table, va_list ap);

/*!
 * \brief Function pointer called to clear the database cache and free resources used for such
 * \since 1.6.1
 */
typedef int realtime_unload(const char *database, const char *table);

/*! \brief Configuration engine structure, used to define realtime drivers */
struct tris_config_engine {
	char *name;
	config_load_func *load_func;
	realtime_var_get *realtime_func;
	realtime_multi_get *realtime_multi_func;
	realtime_update *update_func;
	realtime_update2 *update2_func;
	realtime_store *store_func;
	realtime_destroy *destroy_func;
	realtime_require *require_func;
	realtime_unload *unload_func;
	struct tris_config_engine *next;
};

/*! \brief Load a config file
 * \param filename path of file to open.  If no preceding '/' character, path is considered relative to TRIS_CONFIG_DIR
 * Create a config structure from a given configuration file.
 * \param who_asked The module which is making this request.
 * \param flags Optional flags:
 * CONFIG_FLAG_WITHCOMMENTS - load the file with comments intact;
 * CONFIG_FLAG_FILEUNCHANGED - check the file mtime and return CONFIG_STATUS_FILEUNCHANGED if the mtime is the same; or
 * CONFIG_FLAG_NOCACHE - don't cache file mtime (main purpose of this option is to save memory on temporary files).
 *
 * \return an tris_config data structure on success
 * \retval NULL on error
 */
struct tris_config *tris_config_load2(const char *filename, const char *who_asked, struct tris_flags flags);

#define tris_config_load(filename, flags)	tris_config_load2(filename, TRIS_MODULE, flags)

/*! \brief Destroys a config
 * \param config pointer to config data structure
 * Free memory associated with a given config
 *
 */
void tris_config_destroy(struct tris_config *config);

/*! \brief returns the root tris_variable of a config
 * \param config pointer to an tris_config data structure
 * \param cat name of the category for which you want the root
 *
 * Returns the category specified
 */
struct tris_variable *tris_category_root(struct tris_config *config, char *cat);

/*! \brief Goes through categories
 * \param config Which config structure you wish to "browse"
 * \param prev A pointer to a previous category.
 * This function is kind of non-intuitive in it's use.  To begin, one passes NULL as the second argument.  It will return a pointer to the string of the first category in the file.  From here on after, one must then pass the previous usage's return value as the second pointer, and it will return a pointer to the category name afterwards.
 *
 * \retval a category on success
 * \retval NULL on failure/no-more-categories
 */
char *tris_category_browse(struct tris_config *config, const char *prev);

/*!
 * \brief Goes through variables
 * Somewhat similar in intent as the tris_category_browse.
 * List variables of config file category
 *
 * \retval tris_variable list on success
 * \retval NULL on failure
 */
struct tris_variable *tris_variable_browse(const struct tris_config *config, const char *category);

/*!
 * \brief given a pointer to a category, return the root variable.
 * This is equivalent to tris_variable_browse(), but more efficient if we
 * already have the struct tris_category * (e.g. from tris_category_get())
 */
struct tris_variable *tris_category_first(struct tris_category *cat);

/*!
 * \brief Gets a variable
 * \param config which (opened) config to use
 * \param category category under which the variable lies
 * \param variable which variable you wish to get the data for
 * Goes through a given config file in the given category and searches for the given variable
 *
 * \retval The variable value on success
 * \retval NULL if unable to find it.
 */
const char *tris_variable_retrieve(const struct tris_config *config, const char *category, const char *variable);

/*!
 * \brief Retrieve a category if it exists
 * \param config which config to use
 * \param category_name name of the category you're looking for
 * This will search through the categories within a given config file for a match.
 *
 * \retval pointer to category if found
 * \retval NULL if not.
 */
struct tris_category *tris_category_get(const struct tris_config *config, const char *category_name);

/*!
 * \brief Check for category duplicates
 * \param config which config to use
 * \param category_name name of the category you're looking for
 * This will search through the categories within a given config file for a match.
 *
 * \return non-zero if found
 */
int tris_category_exist(const struct tris_config *config, const char *category_name);

/*!
 * \brief Retrieve realtime configuration
 * \param family which family/config to lookup
 * This will use builtin configuration backends to look up a particular
 * entity in realtime and return a variable list of its parameters.  Note
 * that unlike the variables in tris_config, the resulting list of variables
 * MUST be freed with tris_variables_destroy() as there is no container.
 *
 * The difference between these two calls is that tris_load_realtime excludes
 * fields whose values are NULL, while tris_load_realtime_all loads all columns.
 *
 * Note that you should use the constant SENTINEL to terminate arguments, in
 * order to preserve cross-platform compatibility.
 */
struct tris_variable *tris_load_realtime(const char *family, ...) attribute_sentinel;
struct tris_variable *tris_load_realtime_all(const char *family, ...) attribute_sentinel;

/*!
 * \brief Release any resources cached for a realtime family
 * \param family which family/config to destroy
 * Various backends may cache attributes about a realtime data storage
 * facility; on reload, a front end resource may request to purge that cache.
 * \retval 0 If any cache was purged
 * \retval -1 If no cache was found
 * \since 1.6.1
 */
int tris_unload_realtime(const char *family);

/*!
 * \brief Inform realtime what fields that may be stored
 * \param family which family/config is referenced
 * This will inform builtin configuration backends that particular fields
 * may be updated during the use of that configuration section.  This is
 * mainly to be used during startup routines, to ensure that various fields
 * exist in the backend.  The backends may take various actions, such as
 * creating new fields in the data store or warning the administrator that
 * new fields may need to be created, in order to ensure proper function.
 *
 * The arguments are specified in groups of 3:  column name, column type,
 * and column size.  The column types are specified as integer constants,
 * defined by the enum require_type.  Note that the size is specified as
 * the number of equivalent character fields that a field may take up, even
 * if a field is otherwise specified as an integer type.  This is due to
 * the fact that some fields have historically been specified as character
 * types, even if they contained integer values.
 *
 * A family should always specify its fields to the minimum necessary
 * requirements to fulfill all possible values (within reason; for example,
 * a timeout value may reasonably be specified as an INTEGER2, with size 5.
 * Even though values above 32767 seconds are possible, they are unlikely
 * to be useful, and we should not complain about that size).
 *
 * \retval 0 Required fields met specified standards
 * \retval -1 One or more fields was missing or insufficient
 *
 * Note that you should use the constant SENTINEL to terminate arguments, in
 * order to preserve cross-platform compatibility.
 *
 * \since 1.6.1
 */
int tris_realtime_require_field(const char *family, ...) attribute_sentinel;

/*!
 * \brief Retrieve realtime configuration
 * \param family which family/config to lookup
 *
 * This will use builtin configuration backends to look up a particular
 * entity in realtime and return a variable list of its parameters. Unlike
 * the tris_load_realtime, this function can return more than one entry and
 * is thus stored inside a traditional tris_config structure rather than
 * just returning a linked list of variables.
 *
 * Note that you should use the constant SENTINEL to terminate arguments, in
 * order to preserve cross-platform compatibility.
 */
struct tris_config *tris_load_realtime_multientry(const char *family, ...) attribute_sentinel;

/*!
 * \brief Update realtime configuration
 * \param family which family/config to be updated
 * \param keyfield which field to use as the key
 * \param lookup which value to look for in the key field to match the entry.
 * This function is used to update a parameter in realtime configuration space.
 * \return Number of rows affected, or -1 on error.
 *
 * Note that you should use the constant SENTINEL to terminate arguments, in
 * order to preserve cross-platform compatibility.
 */
int tris_update_realtime(const char *family, const char *keyfield, const char *lookup, ...) attribute_sentinel;

/*!
 * \brief Update realtime configuration
 * \param family which family/config to be updated
 * This function is used to update a parameter in realtime configuration space.
 * It includes the ability to lookup a row based upon multiple key criteria.
 * As a result, this function includes two sentinel values, one to terminate
 * lookup values and the other to terminate the listing of fields to update.
 * \return Number of rows affected, or -1 on error.
 *
 * Note that you should use the constant SENTINEL to terminate arguments, in
 * order to preserve cross-platform compatibility.
 */
int tris_update2_realtime(const char *family, ...) attribute_sentinel;

/*!
 * \brief Create realtime configuration
 * \param family which family/config to be created
 * This function is used to create a parameter in realtime configuration space.
 * \return Number of rows affected, or -1 on error.
 * On the MySQL engine only, for reasons of backwards compatibility, the return
 * value is the insert ID.  This value is nonportable and may be changed in a
 * future version to match the other engines.
 *
 * Note that you should use the constant SENTINEL to terminate arguments, in
 * order to preserve cross-platform compatibility.
 */
int tris_store_realtime(const char *family, ...) attribute_sentinel;

/*!
 * \brief Destroy realtime configuration
 * \param family which family/config to be destroyed
 * \param keyfield which field to use as the key
 * \param lookup which value to look for in the key field to match the entry.
 * This function is used to destroy an entry in realtime configuration space.
 * Additional params are used as keys.
 * \return Number of rows affected, or -1 on error.
 *
 * Note that you should use the constant SENTINEL to terminate arguments, in
 * order to preserve cross-platform compatibility.
 */
int tris_destroy_realtime(const char *family, const char *keyfield, const char *lookup, ...) attribute_sentinel;

/*!
 * \brief Check if realtime engine is configured for family
 * \param family which family/config to be checked
 * \return 1 if family is configured in realtime and engine exists
*/
int tris_check_realtime(const char *family);

/*! \brief Check if there's any realtime engines loaded */
int tris_realtime_enabled(void);

/*! \brief Free variable list
 * \param var the linked list of variables to free
 * This function frees a list of variables.
 */
void tris_variables_destroy(struct tris_variable *var);

/*! \brief Register config engine
 * \retval 1 Always
 */
int tris_config_engine_register(struct tris_config_engine *newconfig);

/*! \brief Deregister config engine
 * \retval 0 Always
 */
int tris_config_engine_deregister(struct tris_config_engine *del);

/*!\brief Exposed initialization method for core process
 * This method is intended for use only with the core initialization and is
 * not designed to be called from any user applications.
 */
int register_config_cli(void);

/*!\brief Exposed re-initialization method for core process
 * This method is intended for use only with the core re-initialization and is
 * not designed to be called from any user applications.
 */
int read_config_maps(void);

/*!\brief Create a new base configuration structure */
struct tris_config *tris_config_new(void);

/*!\brief Retrieve the current category name being built.
 * API for backend configuration engines while building a configuration set.
 */
struct tris_category *tris_config_get_current_category(const struct tris_config *cfg);

/*!\brief Set the category within the configuration as being current.
 * API for backend configuration engines while building a configuration set.
 */
void tris_config_set_current_category(struct tris_config *cfg, const struct tris_category *cat);

/*!\brief Retrieve a configuration variable within the configuration set.
 * Retrieves the named variable \p var within category \p cat of configuration
 * set \p cfg.  If not found, attempts to retrieve the named variable \p var
 * from within category \em general.
 * \return Value of \p var, or NULL if not found.
 */
const char *tris_config_option(struct tris_config *cfg, const char *cat, const char *var);

/*!\brief Create a category structure */
struct tris_category *tris_category_new(const char *name, const char *in_file, int lineno);
void tris_category_append(struct tris_config *config, struct tris_category *cat);

/*!
 * \brief Inserts new category
 * \param config which config to use
 * \param cat newly created category to insert
 * \param match which category to insert above
 * This function is used to insert a new category above another category
 * matching the match parameter.
 */
void tris_category_insert(struct tris_config *config, struct tris_category *cat, const char *match);
int tris_category_delete(struct tris_config *cfg, const char *category);

/*!\brief Removes and destroys all variables within a category
 * \retval 0 if the category was found and emptied
 * \retval -1 if the category was not found
 */
int tris_category_empty(struct tris_config *cfg, const char *category);
void tris_category_destroy(struct tris_category *cat);
struct tris_variable *tris_category_detach_variables(struct tris_category *cat);
void tris_category_rename(struct tris_category *cat, const char *name);

#ifdef MALLOC_DEBUG
struct tris_variable *_tris_variable_new(const char *name, const char *value, const char *filename, const char *file, const char *function, int lineno);
#define tris_variable_new(a, b, c) _tris_variable_new(a, b, c, __FILE__, __PRETTY_FUNCTION__, __LINE__)
#else
struct tris_variable *tris_variable_new(const char *name, const char *value, const char *filename);
#endif
struct tris_config_include *tris_include_new(struct tris_config *conf, const char *from_file, const char *included_file, int is_exec, const char *exec_file, int from_lineno, char *real_included_file_name, int real_included_file_name_size);
struct tris_config_include *tris_include_find(struct tris_config *conf, const char *included_file);
void tris_include_rename(struct tris_config *conf, const char *from_file, const char *to_file);
void tris_variable_append(struct tris_category *category, struct tris_variable *variable);
void tris_variable_insert(struct tris_category *category, struct tris_variable *variable, const char *line);
int tris_variable_delete(struct tris_category *category, const char *variable, const char *match, const char *line);

/*! \brief Update variable value within a config
 * \param category Category element within the config
 * \param variable Name of the variable to change
 * \param value New value of the variable
 * \param match If set, previous value of the variable (if NULL or zero-length, no matching will be done)
 * \param object Boolean of whether to make the new variable an object
 * \return 0 on success or -1 on failure.
 */
int tris_variable_update(struct tris_category *category, const char *variable,
						const char *value, const char *match, unsigned int object);

int tris_config_text_file_save(const char *filename, const struct tris_config *cfg, const char *generator);
int config_text_file_save(const char *filename, const struct tris_config *cfg, const char *generator) __attribute__((deprecated));

struct tris_config *tris_config_internal_load(const char *configfile, struct tris_config *cfg, struct tris_flags flags, const char *suggested_incl_file, const char *who_asked);

/*! \brief Support code to parse config file arguments
 *
 * The function tris_parse_arg() provides a generic interface to parse
 * strings (e.g. numbers, network addresses and so on) in a flexible
 * way, e.g. by doing proper error and bound checks, provide default
 * values, and so on.
 * The function (described later) takes a string as an argument,
 * a set of flags to specify the result format and checks to perform,
 * a pointer to the result, and optionally some additional arguments.
 * It returns 0 on success, != 0 otherwise.
 *
 */
enum tris_parse_flags {
	/* low 4 bits of flags are used for the operand type */
	PARSE_TYPE	=	0x000f,
	/* numeric types, with optional default value and bound checks.
	 * Additional arguments are passed by value.
	 */
	PARSE_INT32	= 	0x0001,
	PARSE_UINT32	= 	0x0002,
	PARSE_DOUBLE	= 	0x0003,
#if 0	/* not supported yet */
	PARSE_INT16	= 	0x0004,
	PARSE_UINT16	= 	0x0005,
#endif
	/* Returns a struct sockaddr_in, with optional default value
	 * (passed by reference) and port handling (accept, ignore,
	 * require, forbid). The format is 'host.name[:port]'
	 */
	PARSE_INADDR	= 	0x000f,

	/* Other data types can be added as needed */

	/* If PARSE_DEFAULT is set, next argument is a default value
	 * which is returned in case of error. The argument is passed
	 * by value in case of numeric types, by reference in other cases.
 	 */
	PARSE_DEFAULT	=	0x0010,	/* assign default on error */

	/* Request a range check, applicable to numbers. Two additional
	 * arguments are passed by value, specifying the low-high end of
	 * the range (inclusive). An error is returned if the value
	 * is outside or inside the range, respectively.
	 */
	PARSE_IN_RANGE =	0x0020,	/* accept values inside a range */
	PARSE_OUT_RANGE =	0x0040,	/* accept values outside a range */

	/* Port handling, for sockaddr_in. accept/ignore/require/forbid
	 * port number after the hostname or address.
	 */
	PARSE_PORT_MASK =	0x0300, /* 0x000: accept port if present */
	PARSE_PORT_IGNORE =	0x0100, /* 0x100: ignore port if present */
	PARSE_PORT_REQUIRE =	0x0200, /* 0x200: require port number */
	PARSE_PORT_FORBID =	0x0300, /* 0x100: forbid port number */
};

/*! \brief The argument parsing routine.
 * \param arg the string to parse. It is not modified.
 * \param flags combination of tris_parse_flags to specify the
 *	return type and additional checks.
 * \param result pointer to the result. NULL is valid here, and can
 *	be used to perform only the validity checks.
 * \param ... extra arguments are required according to flags.
 * \retval 0 in case of success, != 0 otherwise.
 * \retval result returns the parsed value in case of success,
 *	the default value in case of error, or it is left unchanged
 *	in case of error and no default specified. Note that in certain
 *	cases (e.g. sockaddr_in, with multi-field return values) some
 *	of the fields in result may be changed even if an error occurs.
 *
 * Examples of use:
 *	tris_parse_arg("223", PARSE_INT32|PARSE_IN_RANGE,
 *		&a, -1000, 1000);
 *              returns 0, a = 223
 *	tris_parse_arg("22345", PARSE_INT32|PARSE_IN_RANGE|PARSE_DEFAULT,
 *		&a, 9999, 10, 100);
 *              returns 1, a = 9999
 *      tris_parse_arg("22345ssf", PARSE_UINT32|PARSE_IN_RANGE, &b, 10, 100);
 *		returns 1, b unchanged
 *      tris_parse_arg("www.foo.biz:44", PARSE_INADDR, &sa);
 *		returns 0, sa contains address and port
 *      tris_parse_arg("www.foo.biz", PARSE_INADDR|PARSE_PORT_REQUIRE, &sa);
 *		returns 1 because port is missing, sa contains address
 */
int tris_parse_arg(const char *arg, enum tris_parse_flags flags,
        void *result, ...);

/*
 * Parsing config file options in C is slightly annoying because we cannot use
 * string in a switch() statement, yet we need a similar behaviour, with many
 * branches and a break on a matching one.
 * The following somehow simplifies the job: we create a block using
 * the 	CV_START and CV_END macros, and then within the block we can run
 * actions such as "if (condition) { body; break; }"
 * Additional macros are present to run simple functions (e.g. tris_copy_string)
 * or to pass arguments to tris_parse_arg()
 *
 * As an example:

	CV_START(v->name, v->value);	// start the block
	CV_STR("foo", x_foo);		// static string
	CV_DSTR("bar", y_bar);		// malloc'ed string
	CV_F("bar", ...);		// call a generic function
	CV_END;				// end the block
 */

/*! \brief the macro to open a block for variable parsing */
#define CV_START(__in_var, __in_val) 		\
	do {					\
		const char *__var = __in_var;	\
		const char *__val = __in_val;

/*! \brief close a variable parsing block */
#define	CV_END			} while (0)

/*! \brief call a generic function if the name matches. */
#define	CV_F(__pattern, __body)	if (!strcasecmp((__var), __pattern)) { __body; break; }

/*! \brief helper macros to assign the value to a BOOL, UINT, static string and
 * dynamic string
 */
#define	CV_BOOL(__x, __dst)	CV_F(__x, (__dst) = tris_true(__val) )
#define CV_UINT(__x, __dst)	CV_F(__x, (__dst) = strtoul(__val, NULL, 0) )
#define CV_STR(__x, __dst)	CV_F(__x, tris_copy_string(__dst, __val, sizeof(__dst)))
#define CV_DSTR(__x, __dst)	CV_F(__x, if (__dst) tris_free(__dst); __dst = tris_strdup(__val))
#define CV_STRFIELD(__x, __obj, __field) CV_F(__x, tris_string_field_set(__obj, __field, __val))

/*!\brief Check if require type is an integer type */
TRIS_INLINE_API(
int tris_rq_is_int(require_type type),
{
	switch (type) {
	case RQ_INTEGER1:
	case RQ_UINTEGER1:
	case RQ_INTEGER2:
	case RQ_UINTEGER2:
	case RQ_INTEGER3:
	case RQ_UINTEGER3:
	case RQ_INTEGER4:
	case RQ_UINTEGER4:
	case RQ_INTEGER8:
	case RQ_UINTEGER8:
		return 1;
	default:
		return 0;
	}
}
)

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_CONFIG_H */
