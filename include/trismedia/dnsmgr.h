/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Kevin P. Fleming <kpfleming@digium.com>
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
 * \brief Background DNS update manager
 */

#ifndef _TRISMEDIA_DNSMGR_H
#define _TRISMEDIA_DNSMGR_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include "trismedia/network.h"
#include "trismedia/srv.h"

/*!
 * \brief A DNS manager entry
 *
 * This is an opaque type.
 */
struct tris_dnsmgr_entry;

/*!
 * \brief Allocate a new DNS manager entry
 *
 * \param name the hostname
 * \param result where the DNS manager should store the IP address as it refreshes it.
 *      it.
 *
 * This function allocates a new DNS manager entry object, and fills it with the
 * provided hostname and IP address.  This function does not force an initial lookup
 * of the IP address.  So, generally, this should be used when the initial address
 * is already known.
 *
 * \return a DNS manager entry
 * \version 1.6.1 result changed from struct in_addr to struct sockaddr_in to store port number
 */
struct tris_dnsmgr_entry *tris_dnsmgr_get(const char *name, struct sockaddr_in *result, const char *service);

/*!
 * \brief Free a DNS manager entry
 *
 * \param entry the DNS manager entry to free
 *
 * \return nothing
 */
void tris_dnsmgr_release(struct tris_dnsmgr_entry *entry);

/*!
 * \brief Allocate and initialize a DNS manager entry
 *
 * \param name the hostname
 * \param result where to store the IP address as the DNS manager refreshes it
 * \param dnsmgr Where to store the allocate DNS manager entry
 *
 * This function allocates a new DNS manager entry object, and fills it with
 * the provided hostname and IP address.  This function _does_ force an initial
 * lookup, so it may block for some period of time.
 *
 * \retval 0 success
 * \retval non-zero failure
 * \version 1.6.1 result changed from struct in_addr to struct aockaddr_in to store port number
 */
int tris_dnsmgr_lookup(const char *name, struct sockaddr_in *result, struct tris_dnsmgr_entry **dnsmgr, const char *service);

/*!
 * \brief Force a refresh of a dnsmgr entry
 *
 * \retval non-zero if the result is different than the previous result
 * \retval zero if the result is the same as the previous result
 */
int tris_dnsmgr_refresh(struct tris_dnsmgr_entry *entry);

/*!
 * \brief Check is see if a dnsmgr entry has changed
 *
 * \retval non-zero if the dnsmgr entry has changed since the last call to
 *                  this function
 * \retval zero     if the dnsmgr entry has not changed since the last call to
 *                  this function
 */
int tris_dnsmgr_changed(struct tris_dnsmgr_entry *entry);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif /* c_plusplus */

#endif /* TRISMEDIA_DNSMGR_H */
