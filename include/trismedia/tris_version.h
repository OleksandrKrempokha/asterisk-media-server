/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Digium, Inc.
 *
 * Russell Bryant <russell@digium.com>
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

/*!
 * \file
 * \brief Trismedia version information
 * \author Russell Bryant <russell@digium.com>
 */

#ifndef __TRIS_VERSION_H
#define __TRIS_VERSION_H

/*!
 * \brief Retrieve the Trismedia version string.
 */
const char *tris_get_version(void);

/*!
 * \brief Retrieve the numeric Trismedia version
 *
 * Format ABBCC
 * AABB - Major version (1.4 would be 104)
 * CC - Minor version
 *
 * 1.4.17 would be 10417.
 */
const char *tris_get_version_num(void);

#endif /* __TRIS_VERSION_H */
