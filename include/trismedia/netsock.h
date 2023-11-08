/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief Network socket handling
 */

#ifndef _TRISMEDIA_NETSOCK_H
#define _TRISMEDIA_NETSOCK_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include "trismedia/network.h"
#include "trismedia/io.h"

struct tris_netsock;

struct tris_netsock_list;

struct tris_netsock_list *tris_netsock_list_alloc(void);

int tris_netsock_init(struct tris_netsock_list *list);

struct tris_netsock *tris_netsock_bind(struct tris_netsock_list *list, struct io_context *ioc,
				     const char *bindinfo, int defaultport, int tos, int cos, tris_io_cb callback, void *data);

struct tris_netsock *tris_netsock_bindaddr(struct tris_netsock_list *list, struct io_context *ioc,
					 struct sockaddr_in *bindaddr, int tos, int cos, tris_io_cb callback, void *data);

int tris_netsock_release(struct tris_netsock_list *list);

struct tris_netsock *tris_netsock_find(struct tris_netsock_list *list,
				     struct sockaddr_in *sa);

int tris_netsock_set_qos(int netsocket, int tos, int cos, const char *desc);

int tris_netsock_sockfd(const struct tris_netsock *ns);

const struct sockaddr_in *tris_netsock_boundaddr(const struct tris_netsock *ns);

void *tris_netsock_data(const struct tris_netsock *ns);

void tris_netsock_unref(struct tris_netsock *ns);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_NETSOCK_H */
