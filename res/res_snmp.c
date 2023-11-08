/*
 * Copyright (C) 2006 Voop as
 * Thorsten Lockert <tholo@voop.as>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief SNMP Agent / SubAgent support for Trismedia
 *
 * \author Thorsten Lockert <tholo@voop.as>
 *
 * \extref Uses the Net-SNMP libraries available at
 *	 http://net-snmp.sourceforge.net/
 */

/*** MODULEINFO
	<depend>netsnmp</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 142992 $")

#include "trismedia/channel.h"
#include "trismedia/module.h"

#include "snmp/agent.h"

#define	MODULE_DESCRIPTION	"SNMP [Sub]Agent for Trismedia"

int res_snmp_agentx_subagent;
int res_snmp_dont_stop;
int res_snmp_enabled;

static pthread_t thread = TRIS_PTHREADT_NULL;

/*!
 * \brief Load res_snmp.conf config file
 * \return 1 on load, 0 file does not exist
*/
static int load_config(void)
{
	struct tris_variable *var;
	struct tris_config *cfg;
	struct tris_flags config_flags = { 0 };
	char *cat;

	res_snmp_enabled = 0;
	res_snmp_agentx_subagent = 1;
	cfg = tris_config_load("res_snmp.conf", config_flags);
	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING, "Could not load res_snmp.conf\n");
		return 0;
	}
	cat = tris_category_browse(cfg, NULL);
	while (cat) {
		var = tris_variable_browse(cfg, cat);

		if (strcasecmp(cat, "general") == 0) {
			while (var) {
				if (strcasecmp(var->name, "subagent") == 0) {
					if (tris_true(var->value))
						res_snmp_agentx_subagent = 1;
					else if (tris_false(var->value))
						res_snmp_agentx_subagent = 0;
					else {
						tris_log(LOG_ERROR, "Value '%s' does not evaluate to true or false.\n", var->value);
						tris_config_destroy(cfg);
						return 1;
					}
				} else if (strcasecmp(var->name, "enabled") == 0) {
					res_snmp_enabled = tris_true(var->value);
				} else {
					tris_log(LOG_ERROR, "Unrecognized variable '%s' in category '%s'\n", var->name, cat);
					tris_config_destroy(cfg);
					return 1;
				}
				var = var->next;
			}
		} else {
			tris_log(LOG_ERROR, "Unrecognized category '%s'\n", cat);
			tris_config_destroy(cfg);
			return 1;
		}

		cat = tris_category_browse(cfg, cat);
	}
	tris_config_destroy(cfg);
	return 1;
}

static int load_module(void)
{
	if(!load_config())
		return TRIS_MODULE_LOAD_DECLINE;

	tris_verb(1, "Loading [Sub]Agent Module\n");

	res_snmp_dont_stop = 1;
	if (res_snmp_enabled)
		return tris_pthread_create_background(&thread, NULL, agent_thread, NULL);
	else
		return 0;
}

static int unload_module(void)
{
	tris_verb(1, "Unloading [Sub]Agent Module\n");

	res_snmp_dont_stop = 0;
	return ((thread != TRIS_PTHREADT_NULL) ? pthread_join(thread, NULL) : 0);
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_GLOBAL_SYMBOLS, "SNMP [Sub]Agent for Trismedia",
		.load = load_module,
		.unload = unload_module,
		);
