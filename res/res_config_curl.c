/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
 *
 * Tilghman Lesher <res_config_curl_v1@the-tilghman.com>
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
 * \brief curl plugin for portable configuration engine
 *
 * \author Tilghman Lesher <res_config_curl_v1@the-tilghman.com>
 *
 * \extref Depends on the CURL library  - http://curl.haxx.se/
 * 
 */

/*** MODULEINFO
	<depend>curl</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include <curl/curl.h>

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"

/*!
 * \brief Execute a curl query and return tris_variable list
 * \param url The base URL from which to retrieve data
 * \param unused Not currently used
 * \param ap list containing one or more field/operator/value set.
 *
 * \retval var on success
 * \retval NULL on failure
*/
static struct tris_variable *realtime_curl(const char *url, const char *unused, va_list ap)
{
	struct tris_str *query;
	char buf1[200], buf2[200];
	const char *newparam, *newval;
	char *stringp, *pair, *key;
	int i;
	struct tris_variable *var=NULL, *prev=NULL;
	const int EncodeSpecialChars = 1, bufsize = 64000;
	char *buffer;

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return NULL;
	}

	if (!(query = tris_str_create(1000)))
		return NULL;

	if (!(buffer = tris_malloc(bufsize))) {
		tris_free(query);
		return NULL;
	}

	tris_str_set(&query, 0, "${CURL(%s/single,", url);

	for (i = 0; (newparam = va_arg(ap, const char *)); i++) {
		newval = va_arg(ap, const char *);
		tris_uri_encode(newparam, buf1, sizeof(buf1), EncodeSpecialChars);
		tris_uri_encode(newval, buf2, sizeof(buf2), EncodeSpecialChars);
		tris_str_append(&query, 0, "%s%s=%s", i > 0 ? "&" : "", buf1, buf2);
	}
	va_end(ap);

	tris_str_append(&query, 0, ")}");
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, bufsize);

	/* Remove any trailing newline characters */
	if ((stringp = strchr(buffer, '\r')) || (stringp = strchr(buffer, '\n')))
		*stringp = '\0';

	stringp = buffer;
	while ((pair = strsep(&stringp, "&"))) {
		key = strsep(&pair, "=");
		tris_uri_decode(key);
		if (pair)
			tris_uri_decode(pair);

		if (!tris_strlen_zero(key)) {
			if (prev) {
				prev->next = tris_variable_new(key, S_OR(pair, ""), "");
				if (prev->next)
					prev = prev->next;
			} else 
				prev = var = tris_variable_new(key, S_OR(pair, ""), "");
		}
	}

	tris_free(buffer);
	tris_free(query);
	return var;
}

/*!
 * \brief Excute an Select query and return tris_config list
 * \param url
 * \param unused
 * \param ap list containing one or more field/operator/value set.
 *
 * \retval struct tris_config pointer on success
 * \retval NULL on failure
*/
static struct tris_config *realtime_multi_curl(const char *url, const char *unused, va_list ap)
{
	struct tris_str *query;
	char buf1[200], buf2[200];
	const char *newparam, *newval;
	char *stringp, *line, *pair, *key, *initfield = NULL;
	int i;
	const int EncodeSpecialChars = 1, bufsize = 256000;
	struct tris_variable *var=NULL;
	struct tris_config *cfg=NULL;
	struct tris_category *cat=NULL;
	char *buffer;

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return NULL;
	}

	if (!(query = tris_str_create(1000)))
		return NULL;

	if (!(buffer = tris_malloc(bufsize))) {
		tris_free(query);
		return NULL;
	}

	tris_str_set(&query, 0, "${CURL(%s/multi,", url);

	for (i = 0; (newparam = va_arg(ap, const char *)); i++) {
		newval = va_arg(ap, const char *);
		if (i == 0) {
			char *op;
			initfield = tris_strdupa(newparam);
			if ((op = strchr(initfield, ' ')))
				*op = '\0';
		}
		tris_uri_encode(newparam, buf1, sizeof(buf1), EncodeSpecialChars);
		tris_uri_encode(newval, buf2, sizeof(buf2), EncodeSpecialChars);
		tris_str_append(&query, 0, "%s%s=%s", i > 0 ? "&" : "", buf1, buf2);
	}
	va_end(ap);

	tris_str_append(&query, 0, ")}");

	/* Do the CURL query */
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, bufsize);

	if (!(cfg = tris_config_new()))
		goto exit_multi;

	/* Line oriented output */
	stringp = buffer;
	while ((line = strsep(&stringp, "\r\n"))) {
		if (tris_strlen_zero(line))
			continue;

		if (!(cat = tris_category_new("", "", 99999)))
			continue;

		while ((pair = strsep(&line, "&"))) {
			key = strsep(&pair, "=");
			tris_uri_decode(key);
			if (pair)
				tris_uri_decode(pair);

			if (!strcasecmp(key, initfield) && pair)
				tris_category_rename(cat, pair);

			if (!tris_strlen_zero(key)) {
				var = tris_variable_new(key, S_OR(pair, ""), "");
				tris_variable_append(cat, var);
			}
		}
		tris_category_append(cfg, cat);
	}

exit_multi:
	tris_free(buffer);
	tris_free(query);
	return cfg;
}

/*!
 * \brief Execute an UPDATE query
 * \param url
 * \param unused
 * \param keyfield where clause field
 * \param lookup value of field for where clause
 * \param ap list containing one or more field/value set(s).
 *
 * Update a database table, prepare the sql statement using keyfield and lookup
 * control the number of records to change. All values to be changed are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int update_curl(const char *url, const char *unused, const char *keyfield, const char *lookup, va_list ap)
{
	struct tris_str *query;
	char buf1[200], buf2[200];
	const char *newparam, *newval;
	char *stringp;
	int i, rowcount = -1;
	const int EncodeSpecialChars = 1, bufsize = 100;
	char *buffer;

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return -1;
	}

	if (!(query = tris_str_create(1000)))
		return -1;

	if (!(buffer = tris_malloc(bufsize))) {
		tris_free(query);
		return -1;
	}

	tris_uri_encode(keyfield, buf1, sizeof(buf1), EncodeSpecialChars);
	tris_uri_encode(lookup, buf2, sizeof(buf2), EncodeSpecialChars);
	tris_str_set(&query, 0, "${CURL(%s/update?%s=%s,", url, buf1, buf2);

	for (i = 0; (newparam = va_arg(ap, const char *)); i++) {
		newval = va_arg(ap, const char *);
		tris_uri_encode(newparam, buf1, sizeof(buf1), EncodeSpecialChars);
		tris_uri_encode(newval, buf2, sizeof(buf2), EncodeSpecialChars);
		tris_str_append(&query, 0, "%s%s=%s", i > 0 ? "&" : "", buf1, buf2);
	}
	va_end(ap);

	tris_str_append(&query, 0, ")}");
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, bufsize);

	/* Line oriented output */
	stringp = buffer;
	while (*stringp <= ' ')
		stringp++;
	sscanf(stringp, "%30d", &rowcount);

	tris_free(buffer);
	tris_free(query);

	if (rowcount >= 0)
		return (int)rowcount;

	return -1;
}

static int update2_curl(const char *url, const char *unused, va_list ap)
{
	struct tris_str *query;
	char buf1[200], buf2[200];
	const char *newparam, *newval;
	char *stringp;
	int rowcount = -1, lookup = 1, first = 1;
	const int EncodeSpecialChars = 1, bufsize = 100;
	char *buffer;

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return -1;
	}

	if (!(query = tris_str_create(1000)))
		return -1;

	if (!(buffer = tris_malloc(bufsize))) {
		tris_free(query);
		return -1;
	}

	tris_str_set(&query, 0, "${CURL(%s/update?", url);

	for (;;) {
		if ((newparam = va_arg(ap, const char *)) == SENTINEL) {
			if (lookup) {
				lookup = 0;
				tris_str_append(&query, 0, ",");
				/* Back to the first parameter; we don't need a starting '&' */
				first = 1;
				continue;
			} else {
				break;
			}
		}
		newval = va_arg(ap, const char *);
		tris_uri_encode(newparam, buf1, sizeof(buf1), EncodeSpecialChars);
		tris_uri_encode(newval, buf2, sizeof(buf2), EncodeSpecialChars);
		tris_str_append(&query, 0, "%s%s=%s", first ? "" : "&", buf1, buf2);
	}
	va_end(ap);

	tris_str_append(&query, 0, ")}");
	/* TODO: Make proxies work */
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, bufsize);

	/* Line oriented output */
	stringp = buffer;
	while (*stringp <= ' ')
		stringp++;
	sscanf(stringp, "%30d", &rowcount);

	tris_free(buffer);
	tris_free(query);

	if (rowcount >= 0)
		return (int)rowcount;

	return -1;
}

/*!
 * \brief Execute an INSERT query
 * \param url
 * \param unused
 * \param ap list containing one or more field/value set(s)
 *
 * Insert a new record into database table, prepare the sql statement.
 * All values to be changed are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int store_curl(const char *url, const char *unused, va_list ap)
{
	struct tris_str *query;
	char buf1[200], buf2[200];
	const char *newparam, *newval;
	char *stringp;
	int i, rowcount = -1;
	const int EncodeSpecialChars = 1, bufsize = 100;
	char *buffer;

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return -1;
	}

	if (!(query = tris_str_create(1000)))
		return -1;

	if (!(buffer = tris_malloc(bufsize))) {
		tris_free(query);
		return -1;
	}

	tris_str_set(&query, 0, "${CURL(%s/store,", url);

	for (i = 0; (newparam = va_arg(ap, const char *)); i++) {
		newval = va_arg(ap, const char *);
		tris_uri_encode(newparam, buf1, sizeof(buf1), EncodeSpecialChars);
		tris_uri_encode(newval, buf2, sizeof(buf2), EncodeSpecialChars);
		tris_str_append(&query, 0, "%s%s=%s", i > 0 ? "&" : "", buf1, buf2);
	}
	va_end(ap);

	tris_str_append(&query, 0, ")}");
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, bufsize);

	stringp = buffer;
	while (*stringp <= ' ')
		stringp++;
	sscanf(stringp, "%30d", &rowcount);

	tris_free(buffer);
	tris_free(query);

	if (rowcount >= 0)
		return (int)rowcount;

	return -1;
}

/*!
 * \brief Execute an DELETE query
 * \param url
 * \param unused
 * \param keyfield where clause field
 * \param lookup value of field for where clause
 * \param ap list containing one or more field/value set(s)
 *
 * Delete a row from a database table, prepare the sql statement using keyfield and lookup
 * control the number of records to change. Additional params to match rows are stored in ap list.
 * Sub-in the values to the prepared statement and execute it.
 *
 * \retval number of rows affected
 * \retval -1 on failure
*/
static int destroy_curl(const char *url, const char *unused, const char *keyfield, const char *lookup, va_list ap)
{
	struct tris_str *query;
	char buf1[200], buf2[200];
	const char *newparam, *newval;
	char *stringp;
	int i, rowcount = -1;
	const int EncodeSpecialChars = 1, bufsize = 100;
	char *buffer;

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return -1;
	}

	if (!(query = tris_str_create(1000)))
		return -1;

	if (!(buffer = tris_malloc(bufsize))) {
		tris_free(query);
		return -1;
	}

	tris_uri_encode(keyfield, buf1, sizeof(buf1), EncodeSpecialChars);
	tris_uri_encode(lookup, buf2, sizeof(buf2), EncodeSpecialChars);
	tris_str_set(&query, 0, "${CURL(%s/destroy,%s=%s&", url, buf1, buf2);

	for (i = 0; (newparam = va_arg(ap, const char *)); i++) {
		newval = va_arg(ap, const char *);
		tris_uri_encode(newparam, buf1, sizeof(buf1), EncodeSpecialChars);
		tris_uri_encode(newval, buf2, sizeof(buf2), EncodeSpecialChars);
		tris_str_append(&query, 0, "%s%s=%s", i > 0 ? "&" : "", buf1, buf2);
	}
	va_end(ap);

	tris_str_append(&query, 0, ")}");
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, bufsize);

	/* Line oriented output */
	stringp = buffer;
	while (*stringp <= ' ')
		stringp++;
	sscanf(stringp, "%30d", &rowcount);

	tris_free(buffer);
	tris_free(query);

	if (rowcount >= 0)
		return (int)rowcount;

	return -1;
}

static int require_curl(const char *url, const char *unused, va_list ap)
{
	struct tris_str *query;
	char *elm, field[256], buffer[128];
	int type, size;
	const int EncodeSpecialChars = 1;

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return -1;
	}

	if (!(query = tris_str_create(100))) {
		return -1;
	}

	tris_str_set(&query, 0, "${CURL(%s/require,", url);

	while ((elm = va_arg(ap, char *))) {
		type = va_arg(ap, require_type);
		size = va_arg(ap, int);
		tris_uri_encode(elm, field, sizeof(field), EncodeSpecialChars);
		tris_str_append(&query, 0, "%s=%s%%3A%d", field,
			type == RQ_CHAR ? "char" :
			type == RQ_INTEGER1 ? "integer1" :
			type == RQ_UINTEGER1 ? "uinteger1" :
			type == RQ_INTEGER2 ? "integer2" :
			type == RQ_UINTEGER2 ? "uinteger2" :
			type == RQ_INTEGER3 ? "integer3" :
			type == RQ_UINTEGER3 ? "uinteger3" :
			type == RQ_INTEGER4 ? "integer4" :
			type == RQ_UINTEGER4 ? "uinteger4" :
			type == RQ_INTEGER8 ? "integer8" :
			type == RQ_UINTEGER8 ? "uinteger8" :
			type == RQ_DATE ? "date" :
			type == RQ_DATETIME ? "datetime" :
			type == RQ_FLOAT ? "float" :
			"unknown", size);
	}
	va_end(ap);

	tris_str_append(&query, 0, ")}");
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, sizeof(buffer));
	return atoi(buffer);
}

static struct tris_config *config_curl(const char *url, const char *unused, const char *file, struct tris_config *cfg, struct tris_flags flags, const char *sugg_incl, const char *who_asked)
{
	struct tris_str *query;
	char buf1[200];
	char *stringp, *line, *pair, *key;
	const int EncodeSpecialChars = 1, bufsize = 256000;
	int last_cat_metric = -1, cat_metric = -1;
	struct tris_category *cat=NULL;
	char *buffer, *cur_cat = "";
	char *category = "", *var_name = "", *var_val = "";
	struct tris_flags loader_flags = { 0 };

	if (!tris_custom_function_find("CURL")) {
		tris_log(LOG_ERROR, "func_curl.so must be loaded in order to use res_config_curl.so!!\n");
		return NULL;
	}

	if (!(query = tris_str_create(1000)))
		return NULL;

	if (!(buffer = tris_malloc(bufsize))) {
		tris_free(query);
		return NULL;
	}

	tris_uri_encode(file, buf1, sizeof(buf1), EncodeSpecialChars);
	tris_str_set(&query, 0, "${CURL(%s/static?file=%s)}", url, buf1);

	/* Do the CURL query */
	pbx_substitute_variables_helper(NULL, tris_str_buffer(query), buffer, bufsize);

	/* Line oriented output */
	stringp = buffer;
	cat = tris_config_get_current_category(cfg);

	while ((line = strsep(&stringp, "\r\n"))) {
		if (tris_strlen_zero(line))
			continue;

		while ((pair = strsep(&line, "&"))) {
			key = strsep(&pair, "=");
			tris_uri_decode(key);
			if (pair)
				tris_uri_decode(pair);

			if (!strcasecmp(key, "category"))
				category = S_OR(pair, "");
			else if (!strcasecmp(key, "var_name"))
				var_name = S_OR(pair, "");
			else if (!strcasecmp(key, "var_val"))
				var_val = S_OR(pair, "");
			else if (!strcasecmp(key, "cat_metric"))
				cat_metric = pair ? atoi(pair) : 0;
		}

		if (!strcmp(var_name, "#include")) {
			if (!tris_config_internal_load(var_val, cfg, loader_flags, "", who_asked))
				return NULL;
		}

		if (strcmp(category, cur_cat) || last_cat_metric != cat_metric) {
			if (!(cat = tris_category_new(category, "", 99999)))
				break;
			cur_cat = category;
			last_cat_metric = cat_metric;
			tris_category_append(cfg, cat);
		}
		tris_variable_append(cat, tris_variable_new(var_name, var_val, ""));
	}

	tris_free(buffer);
	tris_free(query);
	return cfg;
}

static struct tris_config_engine curl_engine = {
	.name = "curl",
	.load_func = config_curl,
	.realtime_func = realtime_curl,
	.realtime_multi_func = realtime_multi_curl,
	.store_func = store_curl,
	.destroy_func = destroy_curl,
	.update_func = update_curl,
	.update2_func = update2_curl,
	.require_func = require_curl,
};

static int unload_module(void)
{
	tris_config_engine_deregister(&curl_engine);
	tris_verb(1, "res_config_curl unloaded.\n");
	return 0;
}

static int load_module(void)
{
	if (!tris_module_check("res_curl.so")) {
		if (tris_load_resource("res_curl.so") != TRIS_MODULE_LOAD_SUCCESS) {
			tris_log(LOG_ERROR, "Cannot load res_curl, so res_config_curl cannot be loaded\n");
			return TRIS_MODULE_LOAD_DECLINE;
		}
	}

	tris_config_engine_register(&curl_engine);
	tris_verb(1, "res_config_curl loaded.\n");
	return 0;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Realtime Curl configuration");
