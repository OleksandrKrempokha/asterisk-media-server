/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Includes code and algorithms from the Zapata library.
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
 * \brief Comma Separated Value CDR records.
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \arg See also \ref AstCDR
 * \ingroup cdr_drivers
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 158374 $")

#include <time.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_LOG_DIR */
#include "trismedia/config.h"
#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/module.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"

#define CSV_LOG_DIR "/cdr-csv"
#define CSV_MASTER  "/Master.csv"

#define DATE_FORMAT "%Y-%m-%d %T"

static int usegmtime = 0;
static int loguniqueid = 0;
static int loguserfield = 0;
static int loaded = 0;
static char *config = "cdr.conf";

/* #define CSV_LOGUNIQUEID 1 */
/* #define CSV_LOGUSERFIELD 1 */

/*----------------------------------------------------
  The values are as follows:


  "accountcode", 	accountcode is the account name of detail records, Master.csv contains all records *
  			Detail records are configured on a channel basis, IAX and SIP are determined by user *
			DAHDI is determined by channel in dahdi.conf
  "source",
  "destination",
  "destination context",
  "callerid",
  "channel",
  "destination channel",	(if applicable)
  "last application",	Last application run on the channel
  "last app argument",	argument to the last channel
  "start time",
  "answer time",
  "end time",
  duration,   		Duration is the whole length that the entire call lasted. ie. call rx'd to hangup
  			"end time" minus "start time"
  billable seconds, 	the duration that a call was up after other end answered which will be <= to duration
  			"end time" minus "answer time"
  "disposition",    	ANSWERED, NO ANSWER, BUSY
  "amaflags",       	DOCUMENTATION, BILL, IGNORE etc, specified on a per channel basis like accountcode.
  "uniqueid",           unique call identifier
  "userfield"		user field set via SetCDRUserField
----------------------------------------------------------*/

static char *name = "csv";

TRIS_MUTEX_DEFINE_STATIC(mf_lock);
TRIS_MUTEX_DEFINE_STATIC(acf_lock);

static int load_config(int reload)
{
	struct tris_config *cfg;
	struct tris_variable *var;
	const char *tmp;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	if (!(cfg = tris_config_load(config, config_flags)) || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING, "unable to load config: %s\n", config);
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED)
		return 1;

	usegmtime = 0;
	loguniqueid = 0;
	loguserfield = 0;

	if (!(var = tris_variable_browse(cfg, "csv"))) {
		tris_config_destroy(cfg);
		return 0;
	}

	if ((tmp = tris_variable_retrieve(cfg, "csv", "usegmtime"))) {
		usegmtime = tris_true(tmp);
		if (usegmtime)
			tris_debug(1, "logging time in GMT\n");
	}

	if ((tmp = tris_variable_retrieve(cfg, "csv", "loguniqueid"))) {
		loguniqueid = tris_true(tmp);
		if (loguniqueid)
			tris_debug(1, "logging CDR field UNIQUEID\n");
	}

	if ((tmp = tris_variable_retrieve(cfg, "csv", "loguserfield"))) {
		loguserfield = tris_true(tmp);
		if (loguserfield)
			tris_debug(1, "logging CDR user-defined field\n");
	}

	tris_config_destroy(cfg);
	return 1;
}

static int append_string(char *buf, char *s, size_t bufsize)
{
	int pos = strlen(buf), spos = 0, error = -1;

	if (pos >= bufsize - 4)
		return -1;

	buf[pos++] = '\"';

	while(pos < bufsize - 3) {
		if (!s[spos]) {
			error = 0;
			break;
		}
		if (s[spos] == '\"')
			buf[pos++] = '\"';
		buf[pos++] = s[spos];
		spos++;
	}

	buf[pos++] = '\"';
	buf[pos++] = ',';
	buf[pos++] = '\0';

	return error;
}

static int append_int(char *buf, int s, size_t bufsize)
{
	char tmp[32];
	int pos = strlen(buf);

	snprintf(tmp, sizeof(tmp), "%d", s);

	if (pos + strlen(tmp) > bufsize - 3)
		return -1;

	strncat(buf, tmp, bufsize - strlen(buf) - 1);
	pos = strlen(buf);
	buf[pos++] = ',';
	buf[pos++] = '\0';

	return 0;
}

static int append_date(char *buf, struct timeval when, size_t bufsize)
{
	char tmp[80] = "";
	struct tris_tm tm;

	if (strlen(buf) > bufsize - 3)
		return -1;

	if (tris_tvzero(when)) {
		strncat(buf, ",", bufsize - strlen(buf) - 1);
		return 0;
	}

	tris_localtime(&when, &tm, usegmtime ? "GMT" : NULL);
	tris_strftime(tmp, sizeof(tmp), DATE_FORMAT, &tm);

	return append_string(buf, tmp, bufsize);
}

static int build_csv_record(char *buf, size_t bufsize, struct tris_cdr *cdr)
{

	buf[0] = '\0';
	/* Account code */
	append_string(buf, cdr->accountcode, bufsize);
	/* Source */
	append_string(buf, cdr->src, bufsize);
	/* Destination */
	append_string(buf, cdr->dst, bufsize);
	/* Destination context */
	append_string(buf, cdr->dcontext, bufsize);
	/* Caller*ID */
	append_string(buf, cdr->clid, bufsize);
	/* Channel */
	append_string(buf, cdr->channel, bufsize);
	/* Destination Channel */
	append_string(buf, cdr->dstchannel, bufsize);
	/* Last Application */
	append_string(buf, cdr->lastapp, bufsize);
	/* Last Data */
	append_string(buf, cdr->lastdata, bufsize);
	/* Start Time */
	append_date(buf, cdr->start, bufsize);
	/* Answer Time */
	append_date(buf, cdr->answer, bufsize);
	/* End Time */
	append_date(buf, cdr->end, bufsize);
	/* Duration */
	append_int(buf, cdr->duration, bufsize);
	/* Billable seconds */
	append_int(buf, cdr->billsec, bufsize);
	/* Disposition */
	append_string(buf, tris_cdr_disp2str(cdr->disposition), bufsize);
	/* AMA Flags */
	append_string(buf, tris_cdr_flags2str(cdr->amaflags), bufsize);
	/* Unique ID */
	if (loguniqueid)
		append_string(buf, cdr->uniqueid, bufsize);
	/* append the user field */
	if(loguserfield)
		append_string(buf, cdr->userfield,bufsize);
	/* If we hit the end of our buffer, log an error */
	if (strlen(buf) < bufsize - 5) {
		/* Trim off trailing comma */
		buf[strlen(buf) - 1] = '\0';
		strncat(buf, "\n", bufsize - strlen(buf) - 1);
		return 0;
	}
	return -1;
}

static int writefile(char *s, char *acc)
{
	char tmp[PATH_MAX];
	FILE *f;

	if (strchr(acc, '/') || (acc[0] == '.')) {
		tris_log(LOG_WARNING, "Account code '%s' insecure for writing file\n", acc);
		return -1;
	}

	snprintf(tmp, sizeof(tmp), "%s/%s/%s.csv", tris_config_TRIS_LOG_DIR,CSV_LOG_DIR, acc);

	tris_mutex_lock(&acf_lock);
	if (!(f = fopen(tmp, "a"))) {
		tris_mutex_unlock(&acf_lock);
		tris_log(LOG_ERROR, "Unable to open file %s : %s\n", tmp, strerror(errno));
		return -1;
	}
	fputs(s, f);
	fflush(f);
	fclose(f);
	tris_mutex_unlock(&acf_lock);

	return 0;
}


static int csv_log(struct tris_cdr *cdr)
{
	FILE *mf = NULL;
	/* Make sure we have a big enough buf */
	char buf[1024];
	char csvmaster[PATH_MAX];
	snprintf(csvmaster, sizeof(csvmaster),"%s/%s/%s", tris_config_TRIS_LOG_DIR, CSV_LOG_DIR, CSV_MASTER);
#if 0
	printf("[CDR] %s ('%s' -> '%s') Dur: %ds Bill: %ds Disp: %s Flags: %s Account: [%s]\n", cdr->channel, cdr->src, cdr->dst, cdr->duration, cdr->billsec, tris_cdr_disp2str(cdr->disposition), tris_cdr_flags2str(cdr->amaflags), cdr->accountcode);
#endif
	if (build_csv_record(buf, sizeof(buf), cdr)) {
		tris_log(LOG_WARNING, "Unable to create CSV record in %d bytes.  CDR not recorded!\n", (int)sizeof(buf));
		return 0;
	}

	/* because of the absolutely unconditional need for the
	   highest reliability possible in writing billing records,
	   we open write and close the log file each time */
	tris_mutex_lock(&mf_lock);
	if ((mf = fopen(csvmaster, "a"))) {
		fputs(buf, mf);
		fflush(mf); /* be particularly anal here */
		fclose(mf);
		mf = NULL;
		tris_mutex_unlock(&mf_lock);
	} else {
		tris_mutex_unlock(&mf_lock);
		tris_log(LOG_ERROR, "Unable to re-open master file %s : %s\n", csvmaster, strerror(errno));
	}

	if (!tris_strlen_zero(cdr->accountcode)) {
		if (writefile(buf, cdr->accountcode))
			tris_log(LOG_WARNING, "Unable to write CSV record to account file '%s' : %s\n", cdr->accountcode, strerror(errno));
	}

	return 0;
}

static int unload_module(void)
{
	tris_cdr_unregister(name);
	loaded = 0;
	return 0;
}

static int load_module(void)
{
	int res;

	if(!load_config(0))
		return TRIS_MODULE_LOAD_DECLINE;

	if ((res = tris_cdr_register(name, tris_module_info->description, csv_log))) {
		tris_log(LOG_ERROR, "Unable to register CSV CDR handling\n");
	} else {
		loaded = 1;
	}
	return res;
}

static int reload(void)
{
	if (load_config(1)) {
		loaded = 1;
	} else {
		loaded = 0;
		tris_log(LOG_WARNING, "No [csv] section in cdr.conf.  Unregistering backend.\n");
		tris_cdr_unregister(name);
	}

	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Comma Separated Values CDR Backend",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
