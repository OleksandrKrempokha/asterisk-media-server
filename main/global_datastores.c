/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2007, Digium, Inc.
 *
 * Mark Michelson <mmichelson@digium.com>
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
 * \brief globally-accessible datastore information and callbacks
 *
 * \author Mark Michelson <mmichelson@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 172580 $")

#include "trismedia/global_datastores.h"
#include "trismedia/linkedlists.h"

static void dialed_interface_destroy(void *data)
{
	struct tris_dialed_interface *di = NULL;
	TRIS_LIST_HEAD(, tris_dialed_interface) *dialed_interface_list = data;
	
	if (!dialed_interface_list) {
		return;
	}

	TRIS_LIST_LOCK(dialed_interface_list);
	while ((di = TRIS_LIST_REMOVE_HEAD(dialed_interface_list, list)))
		tris_free(di);
	TRIS_LIST_UNLOCK(dialed_interface_list);

	TRIS_LIST_HEAD_DESTROY(dialed_interface_list);
	tris_free(dialed_interface_list);
}

static void *dialed_interface_duplicate(void *data)
{
	struct tris_dialed_interface *di = NULL;
	TRIS_LIST_HEAD(, tris_dialed_interface) *old_list;
	TRIS_LIST_HEAD(, tris_dialed_interface) *new_list = NULL;

	if(!(old_list = data)) {
		return NULL;
	}

	if(!(new_list = tris_calloc(1, sizeof(*new_list)))) {
		return NULL;
	}

	TRIS_LIST_HEAD_INIT(new_list);
	TRIS_LIST_LOCK(old_list);
	TRIS_LIST_TRAVERSE(old_list, di, list) {
		struct tris_dialed_interface *di2 = tris_calloc(1, sizeof(*di2) + strlen(di->interface));
		if(!di2) {
			TRIS_LIST_UNLOCK(old_list);
			dialed_interface_destroy(new_list);
			return NULL;
		}
		strcpy(di2->interface, di->interface);
		TRIS_LIST_INSERT_TAIL(new_list, di2, list);
	}
	TRIS_LIST_UNLOCK(old_list);

	return new_list;
}

const struct tris_datastore_info dialed_interface_info = {
	.type = "dialed-interface",
	.destroy = dialed_interface_destroy,
	.duplicate = dialed_interface_duplicate,
};
