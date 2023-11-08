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
 * \brief Channel Variables
 */

#ifndef _TRISMEDIA_CHANVARS_H
#define _TRISMEDIA_CHANVARS_H

#include "trismedia/linkedlists.h"

struct tris_var_t {
	TRIS_LIST_ENTRY(tris_var_t) entries;
	char *value;
	char name[0];
};

TRIS_LIST_HEAD_NOLOCK(varshead, tris_var_t);

#ifdef MALLOC_DEBUG
struct tris_var_t *_tris_var_assign(const char *name, const char *value, const char *file, int lineno, const char *function);
#define tris_var_assign(a,b)	_tris_var_assign(a,b,__FILE__,__LINE__,__PRETTY_FUNCTION__)
#else
struct tris_var_t *tris_var_assign(const char *name, const char *value);
#endif
void tris_var_delete(struct tris_var_t *var);
const char *tris_var_name(const struct tris_var_t *var);
const char *tris_var_full_name(const struct tris_var_t *var);
const char *tris_var_value(const struct tris_var_t *var);

#endif /* _TRISMEDIA_CHANVARS_H */
