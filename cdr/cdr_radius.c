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
 * \brief RADIUS CDR Support
 * \author Philippe Sultan
 * \extref The Radius Client Library - http://developer.berlios.de/projects/radiusclient-ng/
 *
 * \arg See also \ref AstCDR
 * \ingroup cdr_drivers
 */

/*** MODULEINFO
	<depend>radius</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 186233 $")

#include <time.h>
#include <radiusclient-ng.h>

#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/module.h"
#include "trismedia/utils.h"

/*! ISO 8601 standard format */
#define DATE_FORMAT "%Y-%m-%d %T %z"

#define VENDOR_CODE           22736

enum {
	PW_TRIS_ACCT_CODE =    101,
	PW_TRIS_SRC =          102,
	PW_TRIS_DST =          103,
	PW_TRIS_DST_CTX =      104,
	PW_TRIS_CLID =         105,
	PW_TRIS_CHAN =         106,
	PW_TRIS_DST_CHAN =     107,
	PW_TRIS_LAST_APP =     108,
	PW_TRIS_LAST_DATA =    109,
	PW_TRIS_START_TIME =   110,
	PW_TRIS_ANSWER_TIME =  111,
	PW_TRIS_END_TIME =     112,
	PW_TRIS_DURATION =     113,
	PW_TRIS_BILL_SEC =     114,
	PW_TRIS_DISPOSITION =  115,
	PW_TRIS_AMA_FLAGS =    116,
	PW_TRIS_UNIQUE_ID =    117,
	PW_TRIS_USER_FIELD =   118
};

enum {
	/*! Log dates and times in UTC */
	RADIUS_FLAG_USEGMTIME = (1 << 0),
	/*! Log Unique ID */
	RADIUS_FLAG_LOGUNIQUEID = (1 << 1),
	/*! Log User Field */
	RADIUS_FLAG_LOGUSERFIELD = (1 << 2)
};

static char *desc = "RADIUS CDR Backend";
static char *name = "radius";
static char *cdr_config = "cdr.conf";

static char radiuscfg[PATH_MAX] = "/etc/radiusclient-ng/radiusclient.conf";

static struct tris_flags global_flags = { RADIUS_FLAG_USEGMTIME | RADIUS_FLAG_LOGUNIQUEID | RADIUS_FLAG_LOGUSERFIELD };

static rc_handle *rh = NULL;

static int build_radius_record(VALUE_PAIR **tosend, struct tris_cdr *cdr)
{
	int recordtype = PW_STATUS_STOP;
	struct tris_tm tm;
	char timestr[128];
	char *tmp;

	if (!rc_avpair_add(rh, tosend, PW_ACCT_STATUS_TYPE, &recordtype, 0, 0))
		return -1;

	/* Account code */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_ACCT_CODE, &cdr->accountcode, strlen(cdr->accountcode), VENDOR_CODE))
		return -1;

 	/* Source */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_SRC, &cdr->src, strlen(cdr->src), VENDOR_CODE))
		return -1;

 	/* Destination */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_DST, &cdr->dst, strlen(cdr->dst), VENDOR_CODE))
		return -1;

 	/* Destination context */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_DST_CTX, &cdr->dcontext, strlen(cdr->dcontext), VENDOR_CODE))
		return -1;

	/* Caller ID */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_CLID, &cdr->clid, strlen(cdr->clid), VENDOR_CODE))
		return -1;

	/* Channel */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_CHAN, &cdr->channel, strlen(cdr->channel), VENDOR_CODE))
		return -1;

	/* Destination Channel */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_DST_CHAN, &cdr->dstchannel, strlen(cdr->dstchannel), VENDOR_CODE))
		return -1;

	/* Last Application */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_LAST_APP, &cdr->lastapp, strlen(cdr->lastapp), VENDOR_CODE))
		return -1;

	/* Last Data */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_LAST_DATA, &cdr->lastdata, strlen(cdr->lastdata), VENDOR_CODE))
		return -1;


	/* Start Time */
	tris_strftime(timestr, sizeof(timestr), DATE_FORMAT,
		tris_localtime(&cdr->start, &tm,
			tris_test_flag(&global_flags, RADIUS_FLAG_USEGMTIME) ? "GMT" : NULL));
	if (!rc_avpair_add(rh, tosend, PW_TRIS_START_TIME, timestr, strlen(timestr), VENDOR_CODE))
		return -1;

	/* Answer Time */
	tris_strftime(timestr, sizeof(timestr), DATE_FORMAT,
		tris_localtime(&cdr->answer, &tm,
			tris_test_flag(&global_flags, RADIUS_FLAG_USEGMTIME) ? "GMT" : NULL));
	if (!rc_avpair_add(rh, tosend, PW_TRIS_ANSWER_TIME, timestr, strlen(timestr), VENDOR_CODE))
		return -1;

	/* End Time */
	tris_strftime(timestr, sizeof(timestr), DATE_FORMAT,
		tris_localtime(&cdr->end, &tm,
			tris_test_flag(&global_flags, RADIUS_FLAG_USEGMTIME) ? "GMT" : NULL));
	if (!rc_avpair_add(rh, tosend, PW_TRIS_END_TIME, timestr, strlen(timestr), VENDOR_CODE))
		return -1;

 	/* Duration */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_DURATION, &cdr->duration, 0, VENDOR_CODE))
		return -1;

	/* Billable seconds */
	if (!rc_avpair_add(rh, tosend, PW_TRIS_BILL_SEC, &cdr->billsec, 0, VENDOR_CODE))
		return -1;

	/* Disposition */
	tmp = tris_cdr_disp2str(cdr->disposition);
	if (!rc_avpair_add(rh, tosend, PW_TRIS_DISPOSITION, tmp, strlen(tmp), VENDOR_CODE))
		return -1;

	/* AMA Flags */
	tmp = tris_cdr_flags2str(cdr->amaflags);
	if (!rc_avpair_add(rh, tosend, PW_TRIS_AMA_FLAGS, tmp, strlen(tmp), VENDOR_CODE))
		return -1;

	if (tris_test_flag(&global_flags, RADIUS_FLAG_LOGUNIQUEID)) {
		/* Unique ID */
		if (!rc_avpair_add(rh, tosend, PW_TRIS_UNIQUE_ID, &cdr->uniqueid, strlen(cdr->uniqueid), VENDOR_CODE))
			return -1;
	}

	if (tris_test_flag(&global_flags, RADIUS_FLAG_LOGUSERFIELD)) {
		/* append the user field */
		if (!rc_avpair_add(rh, tosend, PW_TRIS_USER_FIELD, &cdr->userfield, strlen(cdr->userfield), VENDOR_CODE))
			return -1;
	}

	/* Setting Acct-Session-Id & User-Name attributes for proper generation
	   of Acct-Unique-Session-Id on server side */
	/* Channel */
	if (!rc_avpair_add(rh, tosend, PW_USER_NAME, &cdr->channel, strlen(cdr->channel), 0))
		return -1;

	/* Unique ID */
	if (!rc_avpair_add(rh, tosend, PW_ACCT_SESSION_ID, &cdr->uniqueid, strlen(cdr->uniqueid), 0))
		return -1;

	return 0;
}

static int radius_log(struct tris_cdr *cdr)
{
	int result = ERROR_RC;
	VALUE_PAIR *tosend = NULL;

	if (build_radius_record(&tosend, cdr)) {
		tris_debug(1, "Unable to create RADIUS record. CDR not recorded!\n");
		goto return_cleanup;
	}

	result = rc_acct(rh, 0, tosend);
	if (result != OK_RC) {
		tris_log(LOG_ERROR, "Failed to record Radius CDR record!\n");
	}

return_cleanup:
	if (tosend) {
		rc_avpair_free(tosend);
	}

	return result;
}

static int unload_module(void)
{
	tris_cdr_unregister(name);
	return 0;
}

static int load_module(void)
{
	struct tris_config *cfg;
	struct tris_flags config_flags = { 0 };
	int res;
	const char *tmp;

	if ((cfg = tris_config_load(cdr_config, config_flags)) && cfg != CONFIG_STATUS_FILEINVALID) {
		tris_set2_flag(&global_flags, tris_true(tris_variable_retrieve(cfg, "radius", "usegmtime")), RADIUS_FLAG_USEGMTIME);
		tris_set2_flag(&global_flags, tris_true(tris_variable_retrieve(cfg, "radius", "loguniqueid")), RADIUS_FLAG_LOGUNIQUEID);
		tris_set2_flag(&global_flags, tris_true(tris_variable_retrieve(cfg, "radius", "loguserfield")), RADIUS_FLAG_LOGUSERFIELD);
		if ((tmp = tris_variable_retrieve(cfg, "radius", "radiuscfg")))
			tris_copy_string(radiuscfg, tmp, sizeof(radiuscfg));
		tris_config_destroy(cfg);
	} else
		return TRIS_MODULE_LOAD_DECLINE;

	/* start logging */
	rc_openlog("trismedia");

	/* read radiusclient-ng config file */
	if (!(rh = rc_read_config(radiuscfg))) {
		tris_log(LOG_NOTICE, "Cannot load radiusclient-ng configuration file %s.\n", radiuscfg);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	/* read radiusclient-ng dictionaries */
	if (rc_read_dictionary(rh, rc_conf_str(rh, "dictionary"))) {
		tris_log(LOG_NOTICE, "Cannot load radiusclient-ng dictionary file.\n");
		return TRIS_MODULE_LOAD_DECLINE;
	}

	res = tris_cdr_register(name, desc, radius_log);
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "RADIUS CDR Backend");
