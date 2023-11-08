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
 *
 * \brief Privacy Routines
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 89511 $")

#include <sys/time.h>
#include <signal.h>
#include <dirent.h>

#include "trismedia/channel.h"
#include "trismedia/file.h"
#include "trismedia/app.h"
#include "trismedia/dsp.h"
#include "trismedia/astdb.h"
#include "trismedia/callerid.h"
#include "trismedia/privacy.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"

int tris_privacy_check(char *dest, char *cid)
{
	char tmp[256] = "";
	char *trimcid = "";
	char *n, *l;
	int res;
	char key[256], result[256];
	if (cid)
		tris_copy_string(tmp, cid, sizeof(tmp));
	tris_callerid_parse(tmp, &n, &l);
	if (l) {
		tris_shrink_phone_number(l);
		trimcid = l;
	}
	snprintf(key, sizeof(key), "%s/%s", dest, trimcid);
	res = tris_db_get("privacy", key, result, sizeof(result));
	if (!res) {
		if (!strcasecmp(result, "allow"))
			return TRIS_PRIVACY_ALLOW;
		if (!strcasecmp(result, "deny"))
			return TRIS_PRIVACY_DENY;
		if (!strcasecmp(result, "kill"))
			return TRIS_PRIVACY_KILL;
		if (!strcasecmp(result, "torture"))
			return TRIS_PRIVACY_TORTURE;
	}
	return TRIS_PRIVACY_UNKNOWN;
}

int tris_privacy_reset(char *dest)
{
	if (!dest)
		return -1;
	return tris_db_deltree("privacy", dest);
}

int tris_privacy_set(char *dest, char *cid, int status)
{
	char tmp[256] = "";
	char *trimcid = "";
	char *n, *l;
	int res;
	char key[256];
	if (cid)
		tris_copy_string(tmp, cid, sizeof(tmp));
	tris_callerid_parse(tmp, &n, &l);
	if (l) {
		tris_shrink_phone_number(l);
		trimcid = l;
	}
	if (tris_strlen_zero(trimcid)) {
		/* Don't store anything for empty Caller*ID */
		return 0;
	}
	snprintf(key, sizeof(key), "%s/%s", dest, trimcid);
	if (status == TRIS_PRIVACY_UNKNOWN) 
		res = tris_db_del("privacy", key);
	else if (status == TRIS_PRIVACY_ALLOW)
		res = tris_db_put("privacy", key, "allow");
	else if (status == TRIS_PRIVACY_DENY)
		res = tris_db_put("privacy", key, "deny");
	else if (status == TRIS_PRIVACY_KILL)
		res = tris_db_put("privacy", key, "kill");
	else if (status == TRIS_PRIVACY_TORTURE)
		res = tris_db_put("privacy", key, "torture");
	else
		res = -1;
	return res;
}
