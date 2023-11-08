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
 * \brief Access Control of various sorts
 */

#ifndef _TRISMEDIA_ACL_H
#define _TRISMEDIA_ACL_H


#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include "trismedia/network.h"
#include "trismedia/io.h"

#define TRIS_SENSE_DENY                  0
#define TRIS_SENSE_ALLOW                 1

/* Host based access control */

/*! \brief internal representation of acl entries
 * In principle user applications would have no need for this,
 * but there is sometimes a need to extract individual items,
 * e.g. to print them, and rather than defining iterators to
 * navigate the list, and an externally visible 'struct tris_ha_entry',
 * at least in the short term it is more convenient to make the whole
 * thing public and let users play with them.
 */
struct tris_ha {
        /* Host access rule */
        struct in_addr netaddr;  
        struct in_addr netmask;
        int sense;
        struct tris_ha *next;
};

/*! \brief Free host access list */
void tris_free_ha(struct tris_ha *ha);

/*! \brief Copy ha structure */
void tris_copy_ha(const struct tris_ha *from, struct tris_ha *to);

/*! \brief Append ACL entry to host access list. */
struct tris_ha *tris_append_ha(const char *sense, const char *stuff, struct tris_ha *path, int *error);

/*! \brief Check IP address with host access list */
int tris_apply_ha(struct tris_ha *ha, struct sockaddr_in *sin);

/*! \brief Copy host access list */
struct tris_ha *tris_duplicate_ha_list(struct tris_ha *original);

int tris_get_ip(struct sockaddr_in *sin, const char *value);

int tris_get_ip_or_srv(struct sockaddr_in *sin, const char *value, const char *service);

int tris_ouraddrfor(struct in_addr *them, struct in_addr *us);

int tris_find_ourip(struct in_addr *ourip, struct sockaddr_in bindaddr);

int tris_str2cos(const char *value, unsigned int *cos);

int tris_str2tos(const char *value, unsigned int *tos);
const char *tris_tos2str(unsigned int tos);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_ACL_H */
