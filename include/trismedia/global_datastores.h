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
 * \brief globally accessible channel datastores
 * \author Mark Michelson <mmichelson@digium.com>
 */

#ifndef _TRISMEDIA_GLOBAL_DATASTORE_H
#define _TRISMEDIA_GLOBAL_DATASTORE_H

#include "trismedia/channel.h"

extern const struct tris_datastore_info dialed_interface_info;

struct tris_dialed_interface {
	TRIS_LIST_ENTRY(tris_dialed_interface) list;
	char interface[1];
};

#endif
