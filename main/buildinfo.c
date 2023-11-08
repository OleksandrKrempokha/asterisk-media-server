/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Digium, Inc.
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
 *
 * \brief Build timestamp variables
 *
 * \author Kevin P. Fleming <kpfleming@digium.com>
 */

#include "trismedia/buildinfo.h"
#include "trismedia/build.h"

const char *tris_build_hostname = BUILD_HOSTNAME;
const char *tris_build_kernel = BUILD_KERNEL;
const char *tris_build_machine = BUILD_MACHINE;
const char *tris_build_os = BUILD_OS;
const char *tris_build_date = BUILD_DATE;
const char *tris_build_user = BUILD_USER;
