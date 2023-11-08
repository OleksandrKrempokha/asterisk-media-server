/*
 * Trismedia -- A telephony toolkit for Linux.
 *
 * General Definitions for Trismedia top level program
 *
 * Copyright (C) 1999-2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

/*! \file
 * \brief Trismedia main include file. File version handling, generic pbx functions.
 */

#ifndef _TRISMEDIA_H
#define _TRISMEDIA_H

#include "trismedia/autoconfig.h"

#if !defined(NO_MALLOC_DEBUG) && !defined(STANDALONE) && defined(MALLOC_DEBUG)
#include "trismedia/astmm.h"
#endif

#include "trismedia/compat.h"

/* Default to allowing the umask or filesystem ACLs to determine actual file
 * creation permissions
 */
#ifndef TRIS_DIR_MODE
#define TRIS_DIR_MODE 0777
#endif
#ifndef TRIS_FILE_MODE
#define TRIS_FILE_MODE 0666
#endif

#define DEFAULT_LANGUAGE "kp"

#define DEFAULT_SAMPLE_RATE 8000
#define DEFAULT_SAMPLES_PER_MS  ((DEFAULT_SAMPLE_RATE)/1000)
#define	setpriority	__PLEASE_USE_tris_set_priority_INSTEAD_OF_setpriority__
#define	sched_setscheduler	__PLEASE_USE_tris_set_priority_INSTEAD_OF_sched_setscheduler__

#if defined(DEBUG_FD_LEAKS) && !defined(STANDALONE) && !defined(STANDALONE_AEL)
/* These includes are all about ordering */
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#define	open(a,...)	__tris_fdleak_open(__FILE__,__LINE__,__PRETTY_FUNCTION__, a, __VA_ARGS__)
#define pipe(a)	__tris_fdleak_pipe(a, __FILE__,__LINE__,__PRETTY_FUNCTION__)
#define socket(a,b,c)	__tris_fdleak_socket(a, b, c, __FILE__,__LINE__,__PRETTY_FUNCTION__)
#define close(a)	__tris_fdleak_close(a)
#define	fopen(a,b)	__tris_fdleak_fopen(a, b, __FILE__,__LINE__,__PRETTY_FUNCTION__)
#define	fclose(a)	__tris_fdleak_fclose(a)
#define	dup2(a,b)	__tris_fdleak_dup2(a, b, __FILE__,__LINE__,__PRETTY_FUNCTION__)
#define dup(a)	__tris_fdleak_dup(a, __FILE__,__LINE__,__PRETTY_FUNCTION__)

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif
int __tris_fdleak_open(const char *file, int line, const char *func, const char *path, int flags, ...);
int __tris_fdleak_pipe(int *fds, const char *file, int line, const char *func);
int __tris_fdleak_socket(int domain, int type, int protocol, const char *file, int line, const char *func);
int __tris_fdleak_close(int fd);
FILE *__tris_fdleak_fopen(const char *path, const char *mode, const char *file, int line, const char *func);
int __tris_fdleak_fclose(FILE *ptr);
int __tris_fdleak_dup2(int oldfd, int newfd, const char *file, int line, const char *func);
int __tris_fdleak_dup(int oldfd, const char *file, int line, const char *func);
#if defined(__cplusplus) || defined(c_plusplus)
}
#endif
#endif

int tris_set_priority(int);			/*!< Provided by trismedia.c */
int tris_fd_init(void);				/*!< Provided by astfd.c */

/*!
 * \brief Register a function to be executed before Trismedia exits.
 * \param func The callback function to use.
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
int tris_register_atexit(void (*func)(void));

/*!
 * \brief Unregister a function registered with tris_register_atexit().
 * \param func The callback function to unregister.
 */
void tris_unregister_atexit(void (*func)(void));

#if !defined(LOW_MEMORY)
/*!
 * \brief Register the version of a source code file with the core.
 * \param file the source file name
 * \param version the version string (typically a SVN revision keyword string)
 * \return nothing
 *
 * This function should not be called directly, but instead the
 * TRISMEDIA_FILE_VERSION macro should be used to register a file with the core.
 */
void tris_register_file_version(const char *file, const char *version);

/*!
 * \brief Unregister a source code file from the core.
 * \param file the source file name
 * \return nothing
 *
 * This function should not be called directly, but instead the
 * TRISMEDIA_FILE_VERSION macro should be used to automatically unregister
 * the file when the module is unloaded.
 */
void tris_unregister_file_version(const char *file);

/*! \brief Find version for given module name
 * \param file Module name (i.e. chan_sip.so)
 * \return version string or NULL if the module is not found
 */
const char *tris_file_version_find(const char *file);

char *tris_complete_source_filename(const char *partial, int n);

/*!
 * \brief Register/unregister a source code file with the core.
 * \param file the source file name
 * \param version the version string (typically a SVN revision keyword string)
 *
 * This macro will place a file-scope constructor and destructor into the
 * source of the module using it; this will cause the version of this file
 * to registered with the Trismedia core (and unregistered) at the appropriate
 * times.
 *
 * Example:
 *
 * \code
 * TRISMEDIA_FILE_VERSION(__FILE__, "\$Revision\$")
 * \endcode
 *
 * \note The dollar signs above have been protected with backslashes to keep
 * SVN from modifying them in this file; under normal circumstances they would
 * not be present and SVN would expand the Revision keyword into the file's
 * revision number.
 */
#ifdef MTX_PROFILE
#define	HAVE_MTX_PROFILE	/* used in lock.h */
#define TRISMEDIA_FILE_VERSION(file, version) \
	static int mtx_prof = -1;       /* profile mutex */	\
	static void __attribute__((constructor)) __register_file_version(void) \
	{ \
		mtx_prof = tris_add_profile("mtx_lock_" file, 0);	\
		tris_register_file_version(file, version); \
	} \
	static void __attribute__((destructor)) __unregister_file_version(void) \
	{ \
		tris_unregister_file_version(file); \
	}
#else /* !MTX_PROFILE */
#define TRISMEDIA_FILE_VERSION(file, version) \
	static void __attribute__((constructor)) __register_file_version(void) \
	{ \
		tris_register_file_version(file, version); \
	} \
	static void __attribute__((destructor)) __unregister_file_version(void) \
	{ \
		tris_unregister_file_version(file); \
	}
#endif /* !MTX_PROFILE */
#else /* LOW_MEMORY */
#define TRISMEDIA_FILE_VERSION(file, x)
#endif /* LOW_MEMORY */

#if !defined(LOW_MEMORY)
/*!
 * \brief support for event profiling
 *
 * (note, this must be documented a lot more)
 * tris_add_profile allocates a generic 'counter' with a given name,
 * which can be shown with the command 'core show profile &lt;name&gt;'
 *
 * The counter accumulates positive or negative values supplied by
 * \see tris_add_profile(), dividing them by the 'scale' value passed in the
 * create call, and also counts the number of 'events'.
 * Values can also be taked by the TSC counter on ia32 architectures,
 * in which case you can mark the start of an event calling tris_mark(id, 1)
 * and then the end of the event with tris_mark(id, 0).
 * For non-i386 architectures, these two calls return 0.
 */
int tris_add_profile(const char *, uint64_t scale);
int64_t tris_profile(int, int64_t);
int64_t tris_mark(int, int start1_stop0);
#else /* LOW_MEMORY */
#define tris_add_profile(a, b) 0
#define tris_profile(a, b) do { } while (0)
#define tris_mark(a, b) do { } while (0)
#endif /* LOW_MEMORY */

/*! \brief
 * Definition of various structures that many trismedia files need,
 * but only because they need to know that the type exists.
 *
 */

struct tris_channel;
struct tris_frame;
struct tris_module;
struct tris_variable;
struct tris_str;

#ifdef bzero
#undef bzero
#endif

#ifdef bcopy
#undef bcopy
#endif

#define bzero  0x__dont_use_bzero__use_memset_instead""
#define bcopy  0x__dont_use_bcopy__use_memmove_instead()

#endif /* _TRISMEDIA_H */
