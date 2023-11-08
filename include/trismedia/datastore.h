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
 * \brief Trismedia datastore objects
 */

#ifndef _TRISMEDIA_DATASTORE_H
#define _TRISMEDIA_DATASTORE_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include "trismedia/linkedlists.h"

/*! \brief Structure for a data store type */
struct tris_datastore_info {
	const char *type;			/*!< Type of data store */
	void *(*duplicate)(void *data);		/*!< Duplicate item data (used for inheritance) */
	void (*destroy)(void *data);		/*!< Destroy function */

	/*!
	 * \brief Fix up channel references
	 *
	 * \arg data The datastore data
	 * \arg old_chan The old channel owning the datastore
	 * \arg new_chan The new channel owning the datastore
	 *
	 * This is exactly like the fixup callback of the channel technology interface.
	 * It allows a datastore to fix any pointers it saved to the owning channel
	 * in case that the owning channel has changed.  Generally, this would happen
	 * when the datastore is set to be inherited, and a masquerade occurs.
	 *
	 * \return nothing.
	 */
	void (*chan_fixup)(void *data, struct tris_channel *old_chan, struct tris_channel *new_chan);
};

/*! \brief Structure for a data store object */
struct tris_datastore {
	const char *uid;			/*!< Unique data store identifier */
	void *data;				/*!< Contained data */
	const struct tris_datastore_info *info;	/*!< Data store type information */
	unsigned int inheritance;		/*!< Number of levels this item will continue to be inherited */
	TRIS_LIST_ENTRY(tris_datastore) entry; 	/*!< Used for easy linking */
};

/*!
 * \brief Create a data store object
 * \param[in] info information describing the data store object
 * \param[in] uid unique identifer
 * \version 1.6.1 moved here and renamed from tris_channel_datastore_alloc
 */
struct tris_datastore * attribute_malloc __tris_datastore_alloc(const struct tris_datastore_info *info, const char *uid,
							      const char *file, int line, const char *function);

#define tris_datastore_alloc(info, uid) __tris_datastore_alloc(info, uid, __FILE__, __LINE__, __PRETTY_FUNCTION__)

/*!
 * \brief Free a data store object
 * \param[in] datastore datastore to free
 * \version 1.6.1 moved here and renamed from tris_channel_datastore_free
 */
int tris_datastore_free(struct tris_datastore *datastore);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_DATASTORE_H */
