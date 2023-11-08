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
 * \brief Persistant data storage (akin to *doze registry)
 */

#ifndef _TRISMEDIA_PRIVACY_H
#define _TRISMEDIA_PRIVACY_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define TRIS_PRIVACY_DENY	(1 << 0)		/* Don't bother ringing, send to voicemail */
#define TRIS_PRIVACY_ALLOW   (1 << 1)		/* Pass directly to me */
#define TRIS_PRIVACY_KILL	(1 << 2)		/* Play anti-telemarketer message and hangup */
#define TRIS_PRIVACY_TORTURE	(1 << 3)		/* Send directly to tele-torture */
#define TRIS_PRIVACY_UNKNOWN (1 << 16)

int tris_privacy_check(char *dest, char *cid);

int tris_privacy_set(char *dest, char *cid, int status);

int tris_privacy_reset(char *dest);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_PRIVACY_H */
