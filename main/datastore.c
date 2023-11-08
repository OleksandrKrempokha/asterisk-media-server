/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2008, Digium, Inc.
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
 * \brief Trismedia datastore objects
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 192355 $")

#include "trismedia/_private.h"

#include "trismedia/datastore.h"
#include "trismedia/utils.h"

struct tris_datastore *__tris_datastore_alloc(const struct tris_datastore_info *info, const char *uid,
					    const char *file, int line, const char *function)
{
	struct tris_datastore *datastore = NULL;

	/* Make sure we at least have type so we can identify this */
	if (!info) {
		return NULL;
	}

#if defined(__TRIS_DEBUG_MALLOC)
	if (!(datastore = __tris_calloc(1, sizeof(*datastore), file, line, function))) {
		return NULL;
	}
#else
	if (!(datastore = tris_calloc(1, sizeof(*datastore)))) {
		return NULL;
	}
#endif

	datastore->info = info;

	datastore->uid = tris_strdup(uid);

	return datastore;
}

int tris_datastore_free(struct tris_datastore *datastore)
{
	int res = 0;

	/* Using the destroy function (if present) destroy the data */
	if (datastore->info->destroy != NULL && datastore->data != NULL) {
		datastore->info->destroy(datastore->data);
		datastore->data = NULL;
	}

	/* Free allocated UID memory */
	if (datastore->uid != NULL) {
		tris_free((void *) datastore->uid);
		datastore->uid = NULL;
	}

	/* Finally free memory used by ourselves */
	tris_free(datastore);

	return res;
}

/* DO NOT PUT ADDITIONAL FUNCTIONS BELOW THIS BOUNDARY
 *
 * ONLY FUNCTIONS FOR PROVIDING BACKWARDS ABI COMPATIBILITY BELONG HERE
 *
 */

/* Provide binary compatibility for modules that call tris_datastore_alloc() directly;
 * newly compiled modules will call __tris_datastore_alloc() via the macros in datastore.h
 */
#undef tris_datastore_alloc
struct tris_datastore *tris_datastore_alloc(const struct tris_datastore_info *info, const char *uid);
struct tris_datastore *tris_datastore_alloc(const struct tris_datastore_info *info, const char *uid)
{
	return __tris_datastore_alloc(info, uid, __FILE__, __LINE__, __FUNCTION__);
}
