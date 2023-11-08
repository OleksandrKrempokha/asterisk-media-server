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
 * \brief Trismedia memory usage debugging
 */


#ifdef __cplusplus
extern "C" {
#endif

#ifndef _TRISMEDIA_ASTMM_H
#define _TRISMEDIA_ASTMM_H

#ifndef STANDALONE

#define __TRIS_DEBUG_MALLOC

#include "trismedia.h"

/* Include these now to prevent them from being needed later */
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Undefine any macros */
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef strndup
#undef asprintf
#undef vasprintf
#undef free

void *__tris_calloc(size_t nmemb, size_t size, const char *file, int lineno, const char *func);
void *__tris_calloc_cache(size_t nmemb, size_t size, const char *file, int lineno, const char *func);
void *__tris_malloc(size_t size, const char *file, int lineno, const char *func);
void __tris_free(void *ptr, const char *file, int lineno, const char *func);
void *__tris_realloc(void *ptr, size_t size, const char *file, int lineno, const char *func);
char *__tris_strdup(const char *s, const char *file, int lineno, const char *func);
char *__tris_strndup(const char *s, size_t n, const char *file, int lineno, const char *func);
int __tris_asprintf(const char *file, int lineno, const char *func, char **strp, const char *format, ...)
	__attribute__((format(printf, 5, 6)));
int __tris_vasprintf(char **strp, const char *format, va_list ap, const char *file, int lineno, const char *func)
	__attribute__((format(printf, 2, 0)));
void __tris_mm_init(void);


/* Provide our own definitions */
#define calloc(a,b) \
	__tris_calloc(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_calloc(a,b) \
	__tris_calloc(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_calloc_cache(a,b) \
	__tris_calloc_cache(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define malloc(a) \
	__tris_malloc(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_malloc(a) \
	__tris_malloc(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define free(a) \
	__tris_free(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_free(a) \
	__tris_free(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define realloc(a,b) \
	__tris_realloc(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_realloc(a,b) \
	__tris_realloc(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define strdup(a) \
	__tris_strdup(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_strdup(a) \
	__tris_strdup(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define strndup(a,b) \
	__tris_strndup(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_strndup(a,b) \
	__tris_strndup(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define asprintf(a, b, c...) \
	__tris_asprintf(__FILE__, __LINE__, __PRETTY_FUNCTION__, a, b, c)

#define tris_asprintf(a, b, c...) \
	__tris_asprintf(__FILE__, __LINE__, __PRETTY_FUNCTION__, a, b, c)

#define vasprintf(a,b,c) \
	__tris_vasprintf(a,b,c,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define tris_vasprintf(a,b,c) \
	__tris_vasprintf(a,b,c,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#endif /* !STANDALONE */

#else
#error "NEVER INCLUDE astmm.h DIRECTLY!!"
#endif /* _TRISMEDIA_ASTMM_H */

#ifdef __cplusplus
}
#endif
