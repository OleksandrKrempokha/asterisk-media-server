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

/*
 *
 * Reverse compile extensions.conf code into prototype AEL code
 *
 */

/*** MODULEINFO
	<depend>res_ael_share</depend>
 ***/

#include "trismedia.h"
TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 201679 $")

#include "trismedia/paths.h"	/* CONFIG_DIR */
#include <locale.h>
#include <ctype.h>
#if !defined(SOLARIS) && !defined(__CYGWIN__)
#include <err.h>
#endif
#include <regex.h>

#include "trismedia.h"
#include "trismedia/pbx.h"
#include "trismedia/tris_expr.h"
#include "trismedia/channel.h"
#include "trismedia/chanvars.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/config.h"
#include "trismedia/options.h"
#include "trismedia/callerid.h"
#include "trismedia/lock.h"
#include "trismedia/hashtab.h"
#include "trismedia/ael_structs.h"
#include "trismedia/devicestate.h"
#include "trismedia/stringfields.h"
#include "trismedia/pval.h"
#include "trismedia/extconf.h"

struct tris_flags tris_compat = { 7 };
const char *tris_config_TRIS_CONFIG_DIR = "/etc/trismedia";	/* placeholder */

void get_start_stop(unsigned int *word, int bitsperword, int totalbits, int *start, int *end);
int all_bits_set(unsigned int *word, int bitsperword, int totalbits);
extern char *days[];
extern char *months[];

char *config = "extensions.conf";

/* 
static char *registrar = "conf2ael";
static char userscontext[TRIS_MAX_EXTENSION] = "default";
static int static_config = 0;
static int write_protect_config = 1;
static int autofallthrough_config = 0;
static int clearglobalvars_config = 0;
char tris_config_TRIS_SYSTEM_NAME[20] = ""; */

/*! Go no deeper than this through includes (not counting loops) */
#define TRIS_PBX_MAX_STACK	128
/* static TRIS_RWLIST_HEAD_STATIC(acf_root, tris_custom_function); */
//extern char tris_config_TRIS_CONFIG_DIR[PATH_MAX];
int option_debug = 0;
int option_verbose = 0;

void tris_register_file_version(const char *file, const char *version);
void tris_register_file_version(const char *file, const char *version)
{
}

void tris_unregister_file_version(const char *file);
void tris_unregister_file_version(const char *file)
{
}
#if !defined(LOW_MEMORY)
int tris_add_profile(const char *x, uint64_t scale) { return 0;}
#endif
/* Our own version of tris_log, since the expr parser uses it. -- stolen from utils/check_expr.c */
void tris_log(int level, const char *file, int line, const char *function, const char *fmt, ...) __attribute__((format(printf,5,6)));

void tris_log(int level, const char *file, int line, const char *function, const char *fmt, ...)
{
	va_list vars;
	va_start(vars,fmt);
	
	printf("LOG: lev:%d file:%s  line:%d func: %s  ",
		   level, file, line, function);
	vprintf(fmt, vars);
	fflush(stdout);
	va_end(vars);
}

/* stolen from pbx.c */
struct tris_context;
struct tris_app;
#ifdef LOW_MEMORY
#define EXT_DATA_SIZE 256
#else
#define EXT_DATA_SIZE 8192
#endif

#define SWITCH_DATA_LENGTH 256

#define VAR_BUF_SIZE 4096

#define	VAR_NORMAL		1
#define	VAR_SOFTTRAN	2
#define	VAR_HARDTRAN	3

#define BACKGROUND_SKIP		(1 << 0)
#define BACKGROUND_NOANSWER	(1 << 1)
#define BACKGROUND_MATCHEXTEN	(1 << 2)
#define BACKGROUND_PLAYBACK	(1 << 3)

/*!
   \brief tris_exten: An extension
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


/*! \brief tris_include: include= support in extensions.conf */
struct tris_include {
	const char *name;
	const char *rname;			/*!< Context to include */
	const char *registrar;			/*!< Registrar */
	int hastime;				/*!< If time construct exists */
	struct tris_timing timing;               /*!< time construct */
	struct tris_include *next;		/*!< Link them together */
	char stuff[0];
};

/*! \brief tris_sw: Switch statement in extensions.conf */
struct tris_sw {
	char *name;
	const char *registrar;			/*!< Registrar */
	char *data;				/*!< Data load */
	int eval;
	TRIS_LIST_ENTRY(tris_sw) list;
	char *tmpdata;
	char stuff[0];
};

/*! \brief tris_ignorepat: Ignore patterns in dial plan */
struct tris_ignorepat {
	const char *registrar;
	struct tris_ignorepat *next;
	const char pattern[0];
};

/*! \brief tris_context: An extension context */
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


/*! \brief tris_app: A registered application */
struct tris_app {
	int (*execute)(struct tris_channel *chan, void *data);
	const char *synopsis;			/*!< Synopsis text for 'show applications' */
	const char *description;		/*!< Description (help text) for 'show application &lt;name&gt;' */
	TRIS_RWLIST_ENTRY(tris_app) list;		/*!< Next app in list */
	struct module *module;			/*!< Module this app belongs to */
	char name[0];				/*!< Name of the application */
};

/*! \brief tris_state_cb: An extension state notify register item */
struct tris_state_cb {
	int id;
	void *data;
	tris_state_cb_type callback;
	struct tris_state_cb *next;
};

/*! \brief Structure for dial plan hints

  \note Hints are pointers from an extension in the dialplan to one or
  more devices (tech/name) 
	- See \ref AstExtState
*/
struct tris_hint {
	struct tris_exten *exten;	/*!< Extension */
	int laststate; 			/*!< Last known state */
	struct tris_state_cb *callbacks;	/*!< Callback list for this extension */
	TRIS_RWLIST_ENTRY(tris_hint) list;/*!< Pointer to next hint in list */
};

struct store_hint {
	char *context;
	char *exten;
	struct tris_state_cb *callbacks;
	int laststate;
	TRIS_LIST_ENTRY(store_hint) list;
	char data[1];
};

TRIS_LIST_HEAD(store_hints, store_hint);

static const struct cfextension_states {
	int extension_state;
	const char * const text;
} extension_states[] = {
	{ TRIS_EXTENSION_NOT_INUSE,                     "Idle" },
	{ TRIS_EXTENSION_INUSE,                         "InUse" },
	{ TRIS_EXTENSION_BUSY,                          "Busy" },
	{ TRIS_EXTENSION_UNAVAILABLE,                   "Unavailable" },
	{ TRIS_EXTENSION_RINGING,                       "Ringing" },
	{ TRIS_EXTENSION_INUSE | TRIS_EXTENSION_RINGING, "InUse&Ringing" },
	{ TRIS_EXTENSION_ONHOLD,                        "Hold" },
	{ TRIS_EXTENSION_INUSE | TRIS_EXTENSION_ONHOLD,  "InUse&Hold" }
};
#define STATUS_NO_CONTEXT	1
#define STATUS_NO_EXTENSION	2
#define STATUS_NO_PRIORITY	3
#define STATUS_NO_LABEL		4
#define STATUS_SUCCESS		5

extern struct tris_context *local_contexts;
extern struct tris_context *contexts;


struct tris_custom_function *tris_custom_function_find(const char *name);


struct tris_custom_function *tris_custom_function_find(const char *name)
{
	return 0; /* in "standalone" mode, functions are just not avail */
}


struct profile_entry {
	const char *name;
	uint64_t	scale;	/* if non-zero, values are scaled by this */
	int64_t	mark;
	int64_t	value;
	int64_t	events;
};

struct profile_data {
	int entries;
	int max_size;
	struct profile_entry e[0];
};

static int bit_at(unsigned int *word, int bitsperword, int bitnum)
{
	return word[bitnum/bitsperword] & (1 << (bitnum % bitsperword));
}

void get_start_stop(unsigned int *word, int bitsperword, int totalbits, int *start, int *end)
{
	int i;
	int thisbit, thatbit = bit_at(word, bitsperword, totalbits-1);
	
	for (i=0; i<totalbits; i++) {
		thisbit = bit_at(word, bitsperword, i);
		
		if (thisbit != thatbit ) {
			if (thisbit) {
				*start = i;
			} else {
				*end = i;
			}
		}
		thatbit = thisbit;
	}
}

int all_bits_set(unsigned int *word, int bitsperword, int totalbits )
{
	
	int i, total=totalbits/bitsperword,bitmask = 0;
	
	for (i=0; i<bitsperword; i++)
	{
		bitmask |= (1 << i);
	}
	
	for (i=0; i<total; i++)
	{
		if (word[i] != bitmask)
			return 0;
	}
	return 1;
}


int main(int argc, char **argv)
{
	struct tris_context *tmp;
	struct tris_exten *e, *eroot;
	pval *tree, *tmptree, *sws;
	struct tris_include *tmpi;
	struct tris_sw *sw = 0;
	struct tris_ignorepat *ipi;
	pval *incl=0;
	int localdir = 0, i;

	tree = 0;
	tmptree = 0;

	/* process the command line args */
	for (i=1; i<argc; i++)
	{
		if (strcmp(argv[i],"-d")==0)
			localdir =1;
	}
	
	/* 3 simple steps: */
	/*   1. read in the extensions.conf config file 
	 *   2. traverse, and build an AEL tree
	 *   3. Output the AEL tree into a file
	 */
	printf("WARNING: This is an EXTREMELY preliminary version of a program\n");
	printf("         that will someday hopefully do a thoughful and intelligent\n");
	printf("         job of transforming your extensions.conf file into an\n");
	printf("         extensions.ael file.\n");
	printf("         This version has absolutely no intelligence, and pretty\n");
	printf("         much just does a direct conversion\n");
	printf("         The result will most likely need careful attention to\n");
	printf("         finish the job!!!!!\n");

	if (!localdir)
		printf(" (You could use -d the use the extensions.conf in the current directory!)\n");

	printf("Loading %s/%s...\n", tris_config_TRIS_CONFIG_DIR, config);

	if (!localdir)
		localized_use_conf_dir();
	localized_pbx_load_module();
	
	printf("... Done!\n");
	
	tmp = 0;
	while ((tmp = localized_walk_contexts(tmp)) ) {
		printf("Context: %s\n", tmp->name);
	}
	printf("=========\n");
	tmp = 0;
	while ((tmp = localized_walk_contexts(tmp)) ) {
		/* printf("Context: %s\n", tmp->name); */
		tmptree = pvalCreateNode(PV_CONTEXT);
		if (!tree)
			tree = tmptree;
		else
			pvalTopLevAddObject(tree, tmptree);
		
		pvalContextSetName(tmptree, tris_strdup(tmp->name));
		
		if (tmp->includes) {
			incl = pvalCreateNode(PV_INCLUDES);
			pvalContextAddStatement(tmptree, incl);
			for (tmpi = tmp->includes; tmpi; ) { /* includes */
				if (strchr(tmpi->name,'|')==0) {
					if (tmpi->hastime)
					{
						char timerange[15];
						char dowrange[10];
						char domrange[10];
						char monrange[10];
						int startbit=0, endbit=0;
						
						if (all_bits_set(tmpi->timing.minmask, 30, 720))
							strcpy(timerange, "*");
						else {
							int hr, min;
							char tbuf[20];
							get_start_stop(tmpi->timing.minmask, 30, 720, &startbit, &endbit);
							hr = startbit/30;
							min = (startbit % 30) * 2;
							sprintf(tbuf,"%02d:%02d", hr, min);
							strcpy(timerange, tbuf);
							hr = endbit/30;
							min = (endbit % 30) * 2;
							sprintf(tbuf,"%02d:%02d", hr, min);
							strcat(timerange,"-");
							strcat(timerange,tbuf);
						}
						
						if (all_bits_set(&tmpi->timing.dowmask, 7, 7))
							strcpy(dowrange, "*");
						else {
							get_start_stop(&tmpi->timing.dowmask, 7, 7, &startbit, &endbit);
							strcpy(dowrange, days[startbit]);
							strcat(dowrange,"-");
							strcat(dowrange, days[endbit]);
						}
						
						if (all_bits_set(&tmpi->timing.monthmask, 12, 12))
							strcpy(monrange, "*");
						else {
							get_start_stop(&tmpi->timing.monthmask, 12, 12, &startbit, &endbit);
							strcpy(monrange, months[startbit]);
							strcat(monrange,"-");
							strcat(monrange, months[endbit]);
						}
						
						if (all_bits_set(&tmpi->timing.daymask, 31, 31))
							strcpy(domrange, "*");
						else {
							char tbuf[20];
							get_start_stop(&tmpi->timing.daymask, 31, 31, &startbit, &endbit);
							sprintf(tbuf,"%d", startbit);
							strcpy(domrange, tbuf);
							strcat(domrange,"-");
							sprintf(tbuf,"%d", endbit);
							strcat(domrange, tbuf);
						}
						/* now all 4 fields are set; what do we do? */
						pvalIncludesAddIncludeWithTimeConstraints(incl, strdup(tmpi->name), strdup(timerange), strdup(domrange), strdup(dowrange), strdup(monrange));
						
					} else {
						pvalIncludesAddInclude(incl, strdup(tmpi->name));
					}
				} else { /* it appears the timing constraint info is tacked onto the name, carve it up and divvy it out */
					char *dow,*dom,*mon;
					char *all = strdup(tmpi->name);
					char *hr = strchr(all,'|');
					if (hr) {
						*hr++ = 0;
						dow = strchr(hr,'|');
						if (dow) {
							*dow++ = 0;
							dom = strchr(dow,'|');
							if (dom) {
								*dom++ = 0;
								mon = strchr(dom,'|');
								if (mon) {
									*mon++ = 0;
									/* now all 4 fields are set; what do we do? */
									pvalIncludesAddIncludeWithTimeConstraints(incl, strdup(all), strdup(hr), strdup(dow), strdup(dom), strdup(mon));
									/* the original data is always best to keep (no 2-min rounding) */
								} else {
									tris_log(LOG_ERROR,"No month spec attached to include!\n");
								}
							} else {
								tris_log(LOG_ERROR,"No day of month spec attached to include!\n");
							}
						} else {
							tris_log(LOG_ERROR,"No day of week spec attached to include!\n");
						}
					}
					free(all);
				}
				tmpi = tmpi->next;
			}
		}
		for (ipi = tmp->ignorepats; ipi; ) { /* ignorepats */
			incl = pvalCreateNode(PV_IGNOREPAT);
			pvalIgnorePatSetPattern(incl,(char *)ipi->pattern);
			pvalContextAddStatement(tmptree, incl);
			ipi = ipi->next;
		}
		eroot=0;
		while ( (eroot = localized_walk_context_extensions(tmp, eroot)) ) {
			pval *exten = pvalCreateNode(PV_EXTENSION);
			pvalContextAddStatement(tmptree, exten);
			pvalExtenSetName(exten, tris_strdup(eroot->exten));
		
			if (eroot->peer) {
				pval *block = pvalCreateNode(PV_STATEMENTBLOCK);
				pvalExtenSetStatement(exten, block);
				
				e = 0;
				while ( (e = localized_walk_extension_priorities(eroot, e)) ) {
					
					pval *statemnt = pvalCreateNode(PV_APPLICATION_CALL);
					pval *args = pvalCreateNode(PV_WORD);
					
					/* printf("           %s(%s)\n", e->app, (char*)e->data); */

					pvalAppCallSetAppName(statemnt, tris_strdup(e->app));
					pvalWordSetString(args, tris_strdup(e->data));
					pvalAppCallAddArg(statemnt, args);
					
					pvalStatementBlockAddStatement(block, statemnt);
				}
			} else if (eroot->priority == -1) {

				pval *statemnt = pvalCreateNode(PV_APPLICATION_CALL);
				pval *args = pvalCreateNode(PV_WORD);

				/* printf("Mike, we have a hint on exten %s with data %s\n", eroot->exten, eroot->app); */

				pvalAppCallSetAppName(statemnt, "NoOp");
				pvalWordSetString(args, tris_strdup(eroot->app));


				pvalExtenSetStatement(exten, statemnt);
				pvalExtenSetHints(exten, tris_strdup(eroot->app));
			} else {

				pval *statemnt = pvalCreateNode(PV_APPLICATION_CALL);
				pval *args = pvalCreateNode(PV_WORD);
	
				/* printf("           %s (%s)\n", eroot->app, (char *)eroot->data); */
				
				pvalAppCallSetAppName(statemnt, tris_strdup(eroot->app));
				pvalWordSetString(args, tris_strdup(eroot->data));

				
				pvalAppCallAddArg(statemnt, args);
				pvalExtenSetStatement(exten, statemnt);
			}

			/* printf("   extension: %s\n", eroot->exten); */
		}
		if (TRIS_LIST_FIRST(&tmp->alts)) {
			sws = pvalCreateNode(PV_SWITCHES);
			pvalContextAddStatement(tmptree,sws);
			
			sw = 0;
			while ((sw = localized_walk_context_switches(tmp,sw)) ) {
				pvalSwitchesAddSwitch(sws, tris_strdup(sw->name));
			}
		}
	}
	printf("Generating aelout.ael file...\n");
	
	ael2_print("aelout.ael", tree);
	
	printf("...Done!\n");
	return 0;
}


/* ==================================== for linking internal stuff to external stuff */

int pbx_builtin_setvar(struct tris_channel *chan, void *data)
{
	return localized_pbx_builtin_setvar(chan, data);
}

void pbx_substitute_variables_helper(struct tris_channel *c,const char *cp1,char *cp2,int count);
void pbx_substitute_variables_helper(struct tris_channel *c,const char *cp1,char *cp2,int count)
{
	if (cp1 && *cp1)
		strncpy(cp2,cp1,TRIS_MAX_EXTENSION); /* Right now, this routine is ONLY being called for 
											   a possible var substitution on extension names,
											   so....! */
	else
		*cp2 = 0;
}

int tris_add_extension2(struct tris_context *con,
					   int replace, const char *extension, int priority, const char *label, const char *callerid,
					   const char *application, void *data, void (*datad)(void *),
					   const char *registrar)
{
	return localized_add_extension2(con, replace, extension, priority, label, callerid, application, data, datad, registrar);
}

int tris_context_add_ignorepat2(struct tris_context *con, const char *value, const char *registrar)
{
	
	return localized_context_add_ignorepat2(con, value, registrar);
}

int tris_context_add_switch2(struct tris_context *con, const char *value,
								 const char *data, int eval, const char *registrar)
{
	
	return localized_context_add_switch2(con, value, data, eval, registrar);
}

int tris_context_add_include2(struct tris_context *con, const char *value,
								  const char *registrar)
{
	
	return localized_context_add_include2(con, value,registrar);
}

struct tris_context *tris_context_find_or_create(struct tris_context **extcontexts, struct tris_hashtab *exttable, const char *name, const char *registrar)
{
	printf("find/Creating context %s, registrar=%s\n", name, registrar);
	
	return localized_context_find_or_create(extcontexts, exttable, name, registrar);
}

void tris_cli_register_multiple(void);

void tris_cli_register_multiple(void)
{
}

void tris_module_register(const struct tris_module_info *x)
{
}

void tris_module_unregister(const struct tris_module_info *x)
{
}

void tris_cli_unregister_multiple(void);

void tris_cli_unregister_multiple(void)
{
}

struct tris_context *tris_walk_contexts(struct tris_context *con);
struct tris_context *tris_walk_contexts(struct tris_context *con)
{
	return localized_walk_contexts(con);
}

void tris_context_destroy(struct tris_context *con, const char *registrar);

void tris_context_destroy(struct tris_context *con, const char *registrar)
{
	return localized_context_destroy(con, registrar);
}

int tris_context_verify_includes(struct tris_context *con);

int tris_context_verify_includes(struct tris_context *con)
{
	return  localized_context_verify_includes(con);
}

void tris_merge_contexts_and_delete(struct tris_context **extcontexts, struct tris_hashtab *exttable, const char *registrar);

void tris_merge_contexts_and_delete(struct tris_context **extcontexts, struct tris_hashtab *exttable, const char *registrar)
{
	localized_merge_contexts_and_delete(extcontexts, exttable, registrar);
}

struct tris_exten *pbx_find_extension(struct tris_channel *chan,
									 struct tris_context *bypass,
									 struct pbx_find_info *q,
									 const char *context, 
									 const char *exten, 
									 int priority,
									 const char *label, 
									 const char *callerid, 
									 enum ext_match_t action);

struct tris_exten *pbx_find_extension(struct tris_channel *chan,
									 struct tris_context *bypass,
									 struct pbx_find_info *q,
									 const char *context, 
									 const char *exten, 
									 int priority,
									 const char *label, 
									 const char *callerid, 
									 enum ext_match_t action)
{
	return localized_find_extension(bypass, q, context, exten, priority, label, callerid, action);
}

int tris_hashtab_compare_contexts(const void *ah_a, const void *ah_b);

int tris_hashtab_compare_contexts(const void *ah_a, const void *ah_b)
{
	return 0;
}

unsigned int tris_hashtab_hash_contexts(const void *obj);

unsigned int tris_hashtab_hash_contexts(const void *obj)
{
	return 0;
}

#ifdef DEBUG_THREADS
#if !defined(LOW_MEMORY)
void tris_mark_lock_acquired(void *lock_addr)
{
}
#ifdef HAVE_BKTR
void tris_remove_lock_info(void *lock_addr, struct tris_bt *bt)
{
}

void tris_store_lock_info(enum tris_lock_type type, const char *filename,
	int line_num, const char *func, const char *lock_name, void *lock_addr, struct tris_bt *bt)
{
}

int tris_bt_get_addresses(struct tris_bt *bt)
{
	return 0;
}

#else
void tris_remove_lock_info(void *lock_addr)
{
}

void tris_store_lock_info(enum tris_lock_type type, const char *filename,
	int line_num, const char *func, const char *lock_name, void *lock_addr)
{
}
#endif /* HAVE_BKTR */
#endif /* !defined(LOW_MEMORY) */
#endif /* DEBUG_THREADS */
