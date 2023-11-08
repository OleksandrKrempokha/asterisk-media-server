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
 * \brief Music on hold handling
 */

#ifndef _TRISMEDIA_MOH_H
#define _TRISMEDIA_MOH_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*!
 * \brief Turn on music on hold on a given channel 
 *
 * \param chan The channel structure that will get music on hold
 * \param mclass The class to use if the musicclass is not currently set on
 *               the channel structure.
 * \param interpclass The class to use if the musicclass is not currently set on
 *                    the channel structure or in the mclass argument.
 *
 * \retval Zero on success
 * \retval non-zero on failure
 */
int tris_moh_start(struct tris_channel *chan, const char *mclass, const char *interpclass);

/*! Turn off music on hold on a given channel */
void tris_moh_stop(struct tris_channel *chan);

void tris_install_music_functions(int (*start_ptr)(struct tris_channel *, const char *, const char *),
				 void (*stop_ptr)(struct tris_channel *),
				 void (*cleanup_ptr)(struct tris_channel *));

void tris_uninstall_music_functions(void);

void tris_moh_cleanup(struct tris_channel *chan);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_MOH_H */
