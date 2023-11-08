/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2005
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
 * \brief Trismedia Call Manager CDR records.
 *
 * See also
 * \arg \ref AstCDR
 * \arg \ref AstAMI
 * \arg \ref Config_ami
 * \ingroup cdr_drivers
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 202265 $")

#include <time.h>

#include "trismedia/channel.h"
#include "trismedia/cdr.h"
#include "trismedia/module.h"
#include "trismedia/utils.h"
#include "trismedia/manager.h"
#include "trismedia/config.h"
#include "trismedia/pbx.h"

#define DATE_FORMAT 	"%Y-%m-%d %T"
#define CONF_FILE	"cdr_manager.conf"
#define CUSTOM_FIELDS_BUF_SIZE 1024

static char *name = "cdr_manager";

static int enablecdr = 0;

static struct tris_str *customfields;
TRIS_RWLOCK_DEFINE_STATIC(customfields_lock);

static int manager_log(struct tris_cdr *cdr);

static int load_config(int reload)
{
	char *cat = NULL;
	struct tris_config *cfg;
	struct tris_variable *v;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	int newenablecdr = 0;

	cfg = tris_config_load(CONF_FILE, config_flags);
	if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	}

	if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file '%s' could not be parsed\n", CONF_FILE);
		return -1;
	}

	if (!cfg) {
		/* Standard configuration */
		tris_log(LOG_WARNING, "Failed to load configuration file. Module not activated.\n");
		if (enablecdr)
			tris_cdr_unregister(name);
		enablecdr = 0;
		return -1;
	}

	if (reload) {
		tris_rwlock_wrlock(&customfields_lock);
	}

	if (reload && customfields) {
		tris_free(customfields);
		customfields = NULL;
	}

	while ( (cat = tris_category_browse(cfg, cat)) ) {
		if (!strcasecmp(cat, "general")) {
			v = tris_variable_browse(cfg, cat);
			while (v) {
				if (!strcasecmp(v->name, "enabled"))
					newenablecdr = tris_true(v->value);

				v = v->next;
			}
		} else if (!strcasecmp(cat, "mappings")) {
			customfields = tris_str_create(CUSTOM_FIELDS_BUF_SIZE);
			v = tris_variable_browse(cfg, cat);
			while (v) {
				if (customfields && !tris_strlen_zero(v->name) && !tris_strlen_zero(v->value)) {
					if ((tris_str_strlen(customfields) + strlen(v->value) + strlen(v->name) + 14) < tris_str_size(customfields)) {
						tris_str_append(&customfields, -1, "%s: ${CDR(%s)}\r\n", v->value, v->name);
						tris_log(LOG_NOTICE, "Added mapping %s: ${CDR(%s)}\n", v->value, v->name);
					} else {
						tris_log(LOG_WARNING, "No more buffer space to add other custom fields\n");
						break;
					}

				}
				v = v->next;
			}
		}
	}

	if (reload) {
		tris_rwlock_unlock(&customfields_lock);
	}

	tris_config_destroy(cfg);

	if (enablecdr && !newenablecdr)
		tris_cdr_unregister(name);
	else if (!enablecdr && newenablecdr)
		tris_cdr_register(name, "Trismedia Manager Interface CDR Backend", manager_log);
	enablecdr = newenablecdr;

	return 0;
}

static int manager_log(struct tris_cdr *cdr)
{
	struct tris_tm timeresult;
	char strStartTime[80] = "";
	char strAnswerTime[80] = "";
	char strEndTime[80] = "";
	char buf[CUSTOM_FIELDS_BUF_SIZE];
	struct tris_channel dummy;

	if (!enablecdr)
		return 0;

	tris_localtime(&cdr->start, &timeresult, NULL);
	tris_strftime(strStartTime, sizeof(strStartTime), DATE_FORMAT, &timeresult);

	if (cdr->answer.tv_sec)	{
		tris_localtime(&cdr->answer, &timeresult, NULL);
		tris_strftime(strAnswerTime, sizeof(strAnswerTime), DATE_FORMAT, &timeresult);
	}

	tris_localtime(&cdr->end, &timeresult, NULL);
	tris_strftime(strEndTime, sizeof(strEndTime), DATE_FORMAT, &timeresult);

	buf[0] = '\0';
	tris_rwlock_rdlock(&customfields_lock);
	if (customfields && tris_str_strlen(customfields)) {
		memset(&dummy, 0, sizeof(dummy));
		dummy.cdr = cdr;
		pbx_substitute_variables_helper(&dummy, tris_str_buffer(customfields), buf, sizeof(buf) - 1);
	}
	tris_rwlock_unlock(&customfields_lock);

	manager_event(EVENT_FLAG_CDR, "Cdr",
	    "AccountCode: %s\r\n"
	    "Source: %s\r\n"
	    "Destination: %s\r\n"
	    "DestinationContext: %s\r\n"
	    "CallerID: %s\r\n"
	    "Channel: %s\r\n"
	    "DestinationChannel: %s\r\n"
	    "LastApplication: %s\r\n"
	    "LastData: %s\r\n"
	    "StartTime: %s\r\n"
	    "AnswerTime: %s\r\n"
	    "EndTime: %s\r\n"
	    "Duration: %ld\r\n"
	    "BillableSeconds: %ld\r\n"
	    "Disposition: %s\r\n"
	    "AMAFlags: %s\r\n"
	    "UniqueID: %s\r\n"
	    "UserField: %s\r\n"
	    "%s",
	    cdr->accountcode, cdr->src, cdr->dst, cdr->dcontext, cdr->clid, cdr->channel,
	    cdr->dstchannel, cdr->lastapp, cdr->lastdata, strStartTime, strAnswerTime, strEndTime,
	    cdr->duration, cdr->billsec, tris_cdr_disp2str(cdr->disposition),
	    tris_cdr_flags2str(cdr->amaflags), cdr->uniqueid, cdr->userfield,buf);

	return 0;
}

static int unload_module(void)
{
	tris_cdr_unregister(name);
	if (customfields)
		tris_free(customfields);

	return 0;
}

static int load_module(void)
{
	if (load_config(0)) {
		return TRIS_MODULE_LOAD_DECLINE;
	}

	return TRIS_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	return load_config(1);
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Trismedia Manager Interface CDR Backend",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
