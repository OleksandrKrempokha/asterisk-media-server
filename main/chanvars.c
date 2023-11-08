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
 *
 * \brief Channel Variables
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 149199 $")

#include "trismedia/chanvars.h"
#include "trismedia/strings.h"
#include "trismedia/utils.h"

#ifdef MALLOC_DEBUG
struct tris_var_t *_tris_var_assign(const char *name, const char *value, const char *file, int lineno, const char *function)
#else
struct tris_var_t *tris_var_assign(const char *name, const char *value)
#endif
{	
	struct tris_var_t *var;
	int name_len = strlen(name) + 1;
	int value_len = strlen(value) + 1;

#ifdef MALLOC_DEBUG
	if (!(var = __tris_calloc(sizeof(*var) + name_len + value_len, sizeof(char), file, lineno, function))) {
#else
	if (!(var = tris_calloc(sizeof(*var) + name_len + value_len, sizeof(char)))) {
#endif
		return NULL;
	}

	tris_copy_string(var->name, name, name_len);
	var->value = var->name + name_len;
	tris_copy_string(var->value, value, value_len);
	
	return var;
}	
	
void tris_var_delete(struct tris_var_t *var)
{
	if (var)
		tris_free(var);
}

const char *tris_var_name(const struct tris_var_t *var)
{
	const char *name;

	if (var == NULL || (name = var->name) == NULL)
		return NULL;
	/* Return the name without the initial underscores */
	if (name[0] == '_') {
		name++;
		if (name[0] == '_')
			name++;
	}
	return name;
}

const char *tris_var_full_name(const struct tris_var_t *var)
{
	return (var ? var->name : NULL);
}

const char *tris_var_value(const struct tris_var_t *var)
{
	return (var ? var->value : NULL);
}


