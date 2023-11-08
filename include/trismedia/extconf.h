/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Digium, Inc.
 *
 * Steve Murphy <murf@digium.com>
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
 * \brief External configuration handlers (realtime and static configuration)
 * \author Steve Murphy <murf@digium.com>
 *
 */

#ifndef _TRISMEDIA_EXTCONF_H
#define _TRISMEDIA_EXTCONF_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#ifdef NOTYET
/* I'm going to define all the structs mentioned below, to avoid
   possible conflicts in declarations that might be introduced,
   if we just include the files that define them-- this may be
   unnecessary */

struct tris_comment {
	struct tris_comment *next;
	char cmt[0];
};

struct tris_variable {
	char *name;
	char *value;
	int lineno;
	int object;		/*!< 0 for variable, 1 for object */
	int blanklines; 	/*!< Number of blanklines following entry */
	struct tris_comment *precomments;
	struct tris_comment *sameline;
	struct tris_variable *next;
	char stuff[0];
};

struct tris_category {
	char name[80];
	int ignored;			/*!< do not let user of the config see this category */
	int include_level;
	struct tris_comment *precomments;
	struct tris_comment *sameline;
	struct tris_variable *root;
	struct tris_variable *last;
	struct tris_category *next;
};

struct tris_config {
	struct tris_category *root;
	struct tris_category *last;
	struct tris_category *current;
	struct tris_category *last_browse;		/*!< used to cache the last category supplied via category_browse */
	int include_level;
	int max_include_level;
};

/* ================== above: the config world; below, the dialplan world */

/*! \brief A registered application */
struct tris_app {
	int (*execute)(struct tris_channel *chan, void *data);
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(synopsis);     /*!< Synopsis text for 'show applications' */
		TRIS_STRING_FIELD(description);  /*!< Description (help text) for 'show application &lt;name&gt;' */
		TRIS_STRING_FIELD(syntax);       /*!< Syntax text for 'core show applications' */
		TRIS_STRING_FIELD(arguments);    /*!< Arguments description */
		TRIS_STRING_FIELD(seealso);      /*!< See also */
	);
	enum tris_xmldoc_src docsrc;		/*!< Where the documentation come from. */
	TRIS_RWLIST_ENTRY(tris_app) list;		/*!< Next app in list */
	void *module;			/*!< Module this app belongs to */
	char name[0];				/*!< Name of the application */
};
/*!
   \brief An extension
	The dialplan is saved as a linked list with each context
	having it's own linked list of extensions - one item per
	priority.
*/
struct tris_exten {
	char *exten;			/*!< Extension name */
	int matchcid;			/*!< Match caller id ? */
	const char *cidmatch;		/*!< Caller id to match for this extension */
	int priority;			/*!< Priority */
	const char *label;		/*!< Label */
	struct tris_context *parent;	/*!< The context this extension belongs to  */
	const char *app; 		/*!< Application to execute */
	struct tris_app *cached_app;     /*!< Cached location of application */
	void *data;			/*!< Data to use (arguments) */
	void (*datad)(void *);		/*!< Data destructor */
	struct tris_exten *peer;		/*!< Next higher priority with our extension */
	const char *registrar;		/*!< Registrar */
	struct tris_exten *next;		/*!< Extension with a greater ID */
	char stuff[0];
};
/* from pbx.h */
typedef int (*tris_state_cb_type)(char *context, char* id, enum tris_extension_states state, void *data);
struct tris_timing {
	int hastime;				/*!< If time construct exists */
	unsigned int monthmask;			/*!< Mask for month */
	unsigned int daymask;			/*!< Mask for date */
	unsigned int dowmask;			/*!< Mask for day of week (mon-sun) */
	unsigned int minmask[24];		/*!< Mask for minute */
};
/*! \brief include= support in extensions.conf */
struct tris_include {
	const char *name;
	const char *rname;			/*!< Context to include */
	const char *registrar;			/*!< Registrar */
	int hastime;				/*!< If time construct exists */
	struct tris_timing timing;               /*!< time construct */
	struct tris_include *next;		/*!< Link them together */
	char stuff[0];
};

/*! \brief Switch statement in extensions.conf */
struct tris_sw {
	char *name;
	const char *registrar;			/*!< Registrar */
	char *data;				/*!< Data load */
	int eval;
	TRIS_LIST_ENTRY(tris_sw) list;
	char *tmpdata;
	char stuff[0];
};

*! \brief Ignore patterns in dial plan */
struct tris_ignorepat {
	const char *registrar;
	struct tris_ignorepat *next;
	const char pattern[0];
};

/*! \brief An extension context */
struct tris_context {
	tris_rwlock_t lock; 			/*!< A lock to prevent multiple threads from clobbering the context */
	struct tris_exten *root;			/*!< The root of the list of extensions */
	struct tris_context *next;		/*!< Link them together */
	struct tris_include *includes;		/*!< Include other contexts */
	struct tris_ignorepat *ignorepats;	/*!< Patterns for which to continue playing dialtone */
	const char *registrar;			/*!< Registrar */
	TRIS_LIST_HEAD_NOLOCK(, tris_sw) alts;	/*!< Alternative switches */
	tris_mutex_t macrolock;			/*!< A lock to implement "exclusive" macros - held whilst a call is executing in the macro */
	char name[0];				/*!< Name of the context */
};

#endif

struct tris_config *localized_config_load(const char *filename);
struct tris_config *localized_config_load_with_comments(const char *filename);
struct tris_category *localized_category_get(const struct tris_config *config, const char *category_name);
int localized_config_text_file_save(const char *configfile, const struct tris_config *cfg, const char *generator);
struct tris_context *localized_walk_contexts(struct tris_context *con);
struct tris_exten *localized_walk_context_extensions(struct tris_context *con,
													struct tris_exten *exten);
struct tris_exten *localized_walk_extension_priorities(struct tris_exten *exten,
													  struct tris_exten *priority);
struct tris_include *localized_walk_context_includes(struct tris_context *con,
													struct tris_include *inc);
struct tris_sw *localized_walk_context_switches(struct tris_context *con,
											   struct tris_sw *sw);

void localized_context_destroy(struct tris_context *con, const char *registrar);
int localized_pbx_load_module(void);

/*!
 * \version 1.6.1 added tab parameter
 * \version 1.6.1 renamed function from localized_context_create to localized_context_find_or_create
 */
struct tris_context *localized_context_find_or_create(struct tris_context **extcontexts, void *tab, const char *name, const char *registrar);
int localized_pbx_builtin_setvar(struct tris_channel *chan, void *data);
int localized_context_add_ignorepat2(struct tris_context *con, const char *value, const char *registrar);
int localized_context_add_switch2(struct tris_context *con, const char *value,
								 const char *data, int eval, const char *registrar);
int localized_context_add_include2(struct tris_context *con, const char *value,
								  const char *registrar);
int localized_add_extension2(struct tris_context *con,
							 int replace, const char *extension, int priority, const char *label, const char *callerid,
							 const char *application, void *data, void (*datad)(void *),
							 const char *registrar);

/*!
 * \version 1.6.1 added tab parameter
 */
void localized_merge_contexts_and_delete(struct tris_context **extcontexts, void *tab, const char *registrar);
int localized_context_verify_includes(struct tris_context *con);
void localized_use_conf_dir(void);
void localized_use_local_dir(void);


#ifndef _TRISMEDIA_PBX_H
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

#define STATUS_NO_CONTEXT	1
#define STATUS_NO_EXTENSION	2
#define STATUS_NO_PRIORITY	3
#define STATUS_NO_LABEL		4
#define STATUS_SUCCESS		5

#endif

struct tris_exten *localized_find_extension(struct tris_context *bypass,
										  struct pbx_find_info *q,
										  const char *context,
										  const char *exten,
										  int priority,
										  const char *label,
										  const char *callerid,
										  enum ext_match_t action);


#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_PBX_H */
