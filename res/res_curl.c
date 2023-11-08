/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2008, Digium, Inc.
 *
 * Tilghman Lesher <res_curl_v1@the-tilghman.com>
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
 * \brief curl resource engine
 *
 * \author Tilghman Lesher <res_curl_v1@the-tilghman.com>
 *
 * \extref Depends on the CURL library  - http://curl.haxx.se/
 * 
 */

/*** MODULEINFO
	<depend>curl</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 125055 $")

#include <curl/curl.h>

#include "trismedia/module.h"

static int unload_module(void)
{
	int res = 0;

	/* If the dependent modules are still in memory, forbid unload */
	if (tris_module_check("func_curl.so")) {
		tris_log(LOG_ERROR, "func_curl.so (dependent module) is still loaded.  Cannot unload res_curl.so\n");
		return -1;
	}

	if (tris_module_check("res_config_curl.so")) {
		tris_log(LOG_ERROR, "res_config_curl.so (dependent module) is still loaded.  Cannot unload res_curl.so\n");
		return -1;
	}

	curl_global_cleanup();

	return res;
}

static int load_module(void)
{
	int res = 0;

	if (curl_global_init(CURL_GLOBAL_ALL)) {
		tris_log(LOG_ERROR, "Unable to initialize the CURL library. Cannot load res_curl\n");
		return TRIS_MODULE_LOAD_DECLINE;
	}	

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "cURL Resource Module");


