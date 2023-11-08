/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C)  2004 - 2006, Tilghman Lesher
 *
 * Tilghman Lesher <curl-20050919@the-tilghman.com>
 * and Brian Wilkins <bwilkins@cfl.rr.com> (Added POST option)
 *
 * app_curl.c is distributed with no restrictions on usage or
 * redistribution.
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 * 
 * \brief Curl - Load a URL
 *
 * \author Tilghman Lesher <curl-20050919@the-tilghman.com>
 *
 * \note Brian Wilkins <bwilkins@cfl.rr.com> (Added POST option) 
 *
 * \extref Depends on the CURL library  - http://curl.haxx.se/
 * 
 * \ingroup functions
 */
 
/*** MODULEINFO
	<depend>curl</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 212250 $")

#include <curl/curl.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/cli.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/utils.h"
#include "trismedia/threadstorage.h"

#define CURLVERSION_ATLEAST(a,b,c) \
	((LIBCURL_VERSION_MAJOR > (a)) || ((LIBCURL_VERSION_MAJOR == (a)) && (LIBCURL_VERSION_MINOR > (b))) || ((LIBCURL_VERSION_MAJOR == (a)) && (LIBCURL_VERSION_MINOR == (b)) && (LIBCURL_VERSION_PATCH >= (c))))

#define CURLOPT_SPECIAL_HASHCOMPAT -500

static void curlds_free(void *data);

static struct tris_datastore_info curl_info = {
	.type = "CURL",
	.destroy = curlds_free,
};

struct curl_settings {
	TRIS_LIST_ENTRY(curl_settings) list;
	CURLoption key;
	void *value;
};

TRIS_LIST_HEAD_STATIC(global_curl_info, curl_settings);

static void curlds_free(void *data)
{
	TRIS_LIST_HEAD(global_curl_info, curl_settings) *list = data;
	struct curl_settings *setting;
	if (!list) {
		return;
	}
	while ((setting = TRIS_LIST_REMOVE_HEAD(list, list))) {
		free(setting);
	}
	TRIS_LIST_HEAD_DESTROY(list);
}

enum optiontype {
	OT_BOOLEAN,
	OT_INTEGER,
	OT_INTEGER_MS,
	OT_STRING,
	OT_ENUM,
};

static int parse_curlopt_key(const char *name, CURLoption *key, enum optiontype *ot)
{
	if (!strcasecmp(name, "header")) {
		*key = CURLOPT_HEADER;
		*ot = OT_BOOLEAN;
	} else if (!strcasecmp(name, "proxy")) {
		*key = CURLOPT_PROXY;
		*ot = OT_STRING;
	} else if (!strcasecmp(name, "proxyport")) {
		*key = CURLOPT_PROXYPORT;
		*ot = OT_INTEGER;
	} else if (!strcasecmp(name, "proxytype")) {
		*key = CURLOPT_PROXYTYPE;
		*ot = OT_ENUM;
	} else if (!strcasecmp(name, "dnstimeout")) {
		*key = CURLOPT_DNS_CACHE_TIMEOUT;
		*ot = OT_INTEGER;
	} else if (!strcasecmp(name, "userpwd")) {
		*key = CURLOPT_USERPWD;
		*ot = OT_STRING;
	} else if (!strcasecmp(name, "proxyuserpwd")) {
		*key = CURLOPT_PROXYUSERPWD;
		*ot = OT_STRING;
	} else if (!strcasecmp(name, "maxredirs")) {
		*key = CURLOPT_MAXREDIRS;
		*ot = OT_INTEGER;
	} else if (!strcasecmp(name, "referer")) {
		*key = CURLOPT_REFERER;
		*ot = OT_STRING;
	} else if (!strcasecmp(name, "useragent")) {
		*key = CURLOPT_USERAGENT;
		*ot = OT_STRING;
	} else if (!strcasecmp(name, "cookie")) {
		*key = CURLOPT_COOKIE;
		*ot = OT_STRING;
	} else if (!strcasecmp(name, "ftptimeout")) {
		*key = CURLOPT_FTP_RESPONSE_TIMEOUT;
		*ot = OT_INTEGER;
	} else if (!strcasecmp(name, "httptimeout")) {
#if CURLVERSION_ATLEAST(7,16,2)
		*key = CURLOPT_TIMEOUT_MS;
		*ot = OT_INTEGER_MS;
#else
		*key = CURLOPT_TIMEOUT;
		*ot = OT_INTEGER;
#endif
	} else if (!strcasecmp(name, "conntimeout")) {
#if CURLVERSION_ATLEAST(7,16,2)
		*key = CURLOPT_CONNECTTIMEOUT_MS;
		*ot = OT_INTEGER_MS;
#else
		*key = CURLOPT_CONNECTTIMEOUT;
		*ot = OT_INTEGER;
#endif
	} else if (!strcasecmp(name, "ftptext")) {
		*key = CURLOPT_TRANSFERTEXT;
		*ot = OT_BOOLEAN;
	} else if (!strcasecmp(name, "ssl_verifypeer")) {
		*key = CURLOPT_SSL_VERIFYPEER;
		*ot = OT_BOOLEAN;
	} else if (!strcasecmp(name, "hashcompat")) {
		*key = CURLOPT_SPECIAL_HASHCOMPAT;
		*ot = OT_BOOLEAN;
	} else {
		return -1;
	}
	return 0;
}

static int acf_curlopt_write(struct tris_channel *chan, const char *cmd, char *name, const char *value)
{
	struct tris_datastore *store;
	struct global_curl_info *list;
	struct curl_settings *cur, *new = NULL;
	CURLoption key;
	enum optiontype ot;

	if (chan) {
		if (!(store = tris_channel_datastore_find(chan, &curl_info, NULL))) {
			/* Create a new datastore */
			if (!(store = tris_datastore_alloc(&curl_info, NULL))) {
				tris_log(LOG_ERROR, "Unable to allocate new datastore.  Cannot set any CURL options\n");
				return -1;
			}

			if (!(list = tris_calloc(1, sizeof(*list)))) {
				tris_log(LOG_ERROR, "Unable to allocate list head.  Cannot set any CURL options\n");
				tris_datastore_free(store);
			}

			store->data = list;
			TRIS_LIST_HEAD_INIT(list);
			tris_channel_datastore_add(chan, store);
		} else {
			list = store->data;
		}
	} else {
		/* Populate the global structure */
		list = &global_curl_info;
	}

	if (!parse_curlopt_key(name, &key, &ot)) {
		if (ot == OT_BOOLEAN) {
			if ((new = tris_calloc(1, sizeof(*new)))) {
				new->value = (void *)((long) tris_true(value));
			}
		} else if (ot == OT_INTEGER) {
			long tmp = atol(value);
			if ((new = tris_calloc(1, sizeof(*new)))) {
				new->value = (void *)tmp;
			}
		} else if (ot == OT_INTEGER_MS) {
			long tmp = atof(value) * 1000.0;
			if ((new = tris_calloc(1, sizeof(*new)))) {
				new->value = (void *)tmp;
			}
		} else if (ot == OT_STRING) {
			if ((new = tris_calloc(1, sizeof(*new) + strlen(value) + 1))) {
				new->value = (char *)new + sizeof(*new);
				strcpy(new->value, value);
			}
		} else if (ot == OT_ENUM) {
			if (key == CURLOPT_PROXYTYPE) {
				long ptype =
#if CURLVERSION_ATLEAST(7,10,0)
					CURLPROXY_HTTP;
#else
					CURLPROXY_SOCKS5;
#endif
				if (0) {
#if CURLVERSION_ATLEAST(7,15,2)
				} else if (!strcasecmp(value, "socks4")) {
					ptype = CURLPROXY_SOCKS4;
#endif
#if CURLVERSION_ATLEAST(7,18,0)
				} else if (!strcasecmp(value, "socks4a")) {
					ptype = CURLPROXY_SOCKS4A;
#endif
#if CURLVERSION_ATLEAST(7,18,0)
				} else if (!strcasecmp(value, "socks5")) {
					ptype = CURLPROXY_SOCKS5;
#endif
#if CURLVERSION_ATLEAST(7,18,0)
				} else if (!strncasecmp(value, "socks5", 6)) {
					ptype = CURLPROXY_SOCKS5_HOSTNAME;
#endif
				}

				if ((new = tris_calloc(1, sizeof(*new)))) {
					new->value = (void *)ptype;
				}
			} else {
				/* Highly unlikely */
				goto yuck;
			}
		}

		/* Memory allocation error */
		if (!new) {
			return -1;
		}

		new->key = key;
	} else {
yuck:
		tris_log(LOG_ERROR, "Unrecognized option: %s\n", name);
		return -1;
	}

	/* Remove any existing entry */
	TRIS_LIST_LOCK(list);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(list, cur, list) {
		if (cur->key == new->key) {
			TRIS_LIST_REMOVE_CURRENT(list);
			free(cur);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END

	/* Insert new entry */
	tris_debug(1, "Inserting entry %p with key %d and value %p\n", new, new->key, new->value);
	TRIS_LIST_INSERT_TAIL(list, new, list);
	TRIS_LIST_UNLOCK(list);

	return 0;
}

static int acf_curlopt_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_datastore *store;
	struct global_curl_info *list[2] = { &global_curl_info, NULL };
	struct curl_settings *cur = NULL;
	CURLoption key;
	enum optiontype ot;
	int i;

	if (parse_curlopt_key(data, &key, &ot)) {
		tris_log(LOG_ERROR, "Unrecognized option: '%s'\n", data);
		return -1;
	}

	if (chan && (store = tris_channel_datastore_find(chan, &curl_info, NULL))) {
		list[0] = store->data;
		list[1] = &global_curl_info;
	}

	for (i = 0; i < 2; i++) {
		if (!list[i]) {
			break;
		}
		TRIS_LIST_LOCK(list[i]);
		TRIS_LIST_TRAVERSE(list[i], cur, list) {
			if (cur->key == key) {
				if (ot == OT_BOOLEAN || ot == OT_INTEGER) {
					snprintf(buf, len, "%ld", (long)cur->value);
				} else if (ot == OT_INTEGER_MS) {
					if ((long)cur->value % 1000 == 0) {
						snprintf(buf, len, "%ld", (long)cur->value / 1000);
					} else {
						snprintf(buf, len, "%.3f", (double)((long)cur->value) / 1000.0);
					}
				} else if (ot == OT_STRING) {
					tris_debug(1, "Found entry %p, with key %d and value %p\n", cur, cur->key, cur->value);
					tris_copy_string(buf, cur->value, len);
				} else if (key == CURLOPT_PROXYTYPE) {
					if (0) {
#if CURLVERSION_ATLEAST(7,15,2)
					} else if ((long)cur->value == CURLPROXY_SOCKS4) {
						tris_copy_string(buf, "socks4", len);
#endif
#if CURLVERSION_ATLEAST(7,18,0)
					} else if ((long)cur->value == CURLPROXY_SOCKS4A) {
						tris_copy_string(buf, "socks4a", len);
#endif
					} else if ((long)cur->value == CURLPROXY_SOCKS5) {
						tris_copy_string(buf, "socks5", len);
#if CURLVERSION_ATLEAST(7,18,0)
					} else if ((long)cur->value == CURLPROXY_SOCKS5_HOSTNAME) {
						tris_copy_string(buf, "socks5hostname", len);
#endif
#if CURLVERSION_ATLEAST(7,10,0)
					} else if ((long)cur->value == CURLPROXY_HTTP) {
						tris_copy_string(buf, "http", len);
#endif
					} else {
						tris_copy_string(buf, "unknown", len);
					}
				}
				break;
			}
		}
		TRIS_LIST_UNLOCK(list[i]);
		if (cur) {
			break;
		}
	}

	return cur ? 0 : -1;
}

static size_t WriteMemoryCallback(void *ptr, size_t size, size_t nmemb, void *data)
{
	register int realsize = size * nmemb;
	struct tris_str **pstr = (struct tris_str **)data;

	tris_debug(3, "Called with data=%p, str=%p, realsize=%d, len=%zu, used=%zu\n", data, *pstr, realsize, tris_str_size(*pstr), tris_str_strlen(*pstr));

	tris_str_append_substr(pstr, 0, ptr, realsize);

	tris_debug(3, "Now, len=%zu, used=%zu\n", tris_str_size(*pstr), tris_str_strlen(*pstr));

	return realsize;
}

static const char *global_useragent = "trismedia-libcurl-agent/1.0";

static int curl_instance_init(void *data)
{
	CURL **curl = data;

	if (!(*curl = curl_easy_init()))
		return -1;

	curl_easy_setopt(*curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(*curl, CURLOPT_TIMEOUT, 180);
	curl_easy_setopt(*curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(*curl, CURLOPT_USERAGENT, global_useragent);

	return 0;
}

static void curl_instance_cleanup(void *data)
{
	CURL **curl = data;

	curl_easy_cleanup(*curl);

	tris_free(data);
}

TRIS_THREADSTORAGE_CUSTOM(curl_instance, curl_instance_init, curl_instance_cleanup);

static int acf_curl_exec(struct tris_channel *chan, const char *cmd, char *info, char *buf, size_t len)
{
	struct tris_str *str = tris_str_create(16);
	int ret = -1;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(url);
		TRIS_APP_ARG(postdata);
	);
	CURL **curl;
	struct curl_settings *cur;
	struct tris_datastore *store = NULL;
	int hashcompat = 0;
	TRIS_LIST_HEAD(global_curl_info, curl_settings) *list = NULL;

	*buf = '\0';
	
	if (tris_strlen_zero(info)) {
		tris_log(LOG_WARNING, "CURL requires an argument (URL)\n");
		tris_free(str);
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, info);	

	if (chan) {
		tris_autoservice_start(chan);
	}

	if (!(curl = tris_threadstorage_get(&curl_instance, sizeof(*curl)))) {
		tris_log(LOG_ERROR, "Cannot allocate curl structure\n");
		return -1;
	}

	TRIS_LIST_LOCK(&global_curl_info);
	TRIS_LIST_TRAVERSE(&global_curl_info, cur, list) {
		if (cur->key == CURLOPT_SPECIAL_HASHCOMPAT) {
			hashcompat = (cur->value != NULL) ? 1 : 0;
		} else {
			curl_easy_setopt(*curl, cur->key, cur->value);
		}
	}

	if (chan && (store = tris_channel_datastore_find(chan, &curl_info, NULL))) {
		list = store->data;
		TRIS_LIST_LOCK(list);
		TRIS_LIST_TRAVERSE(list, cur, list) {
			if (cur->key == CURLOPT_SPECIAL_HASHCOMPAT) {
				hashcompat = (cur->value != NULL) ? 1 : 0;
			} else {
				curl_easy_setopt(*curl, cur->key, cur->value);
			}
		}
	}

	curl_easy_setopt(*curl, CURLOPT_URL, args.url);
	curl_easy_setopt(*curl, CURLOPT_FILE, (void *) &str);

	if (args.postdata) {
		curl_easy_setopt(*curl, CURLOPT_POST, 1);
		curl_easy_setopt(*curl, CURLOPT_POSTFIELDS, args.postdata);
	}

	curl_easy_perform(*curl);

	if (store) {
		TRIS_LIST_UNLOCK(list);
	}
	TRIS_LIST_UNLOCK(&global_curl_info);

	if (args.postdata) {
		curl_easy_setopt(*curl, CURLOPT_POST, 0);
	}

	if (tris_str_strlen(str)) {
		tris_str_trim_blanks(str);

		tris_debug(3, "str='%s'\n", tris_str_buffer(str));
		if (hashcompat) {
			char *remainder = tris_str_buffer(str);
			char *piece;
			struct tris_str *fields = tris_str_create(tris_str_strlen(str) / 2);
			struct tris_str *values = tris_str_create(tris_str_strlen(str) / 2);
			int rowcount = 0;
			while ((piece = strsep(&remainder, "&"))) {
				char *name = strsep(&piece, "=");
				tris_uri_decode(piece);
				tris_uri_decode(name);
				tris_str_append(&fields, 0, "%s%s", rowcount ? "," : "", name);
				tris_str_append(&values, 0, "%s%s", rowcount ? "," : "", piece);
				rowcount++;
			}
			pbx_builtin_setvar_helper(chan, "~ODBCFIELDS~", tris_str_buffer(fields));
			tris_copy_string(buf, tris_str_buffer(values), len);
			tris_free(fields);
			tris_free(values);
		} else {
			tris_copy_string(buf, tris_str_buffer(str), len);
		}
		ret = 0;
	}
	tris_free(str);

	if (chan)
		tris_autoservice_stop(chan);
	
	return ret;
}

struct tris_custom_function acf_curl = {
	.name = "CURL",
	.synopsis = "Retrieves the contents of a URL",
	.syntax = "CURL(url[,post-data])",
	.desc =
	"  url       - URL to retrieve\n"
	"  post-data - Optional data to send as a POST (GET is default action)\n",
	.read = acf_curl_exec,
};

struct tris_custom_function acf_curlopt = {
	.name = "CURLOPT",
	.synopsis = "Set options for use with the CURL() function",
	.syntax = "CURLOPT(<option>)",
	.desc =
"  cookie         - Send cookie with request [none]\n"
"  conntimeout    - Number of seconds to wait for connection\n"
"  dnstimeout     - Number of seconds to wait for DNS response\n"
"  ftptext        - For FTP, force a text transfer (boolean)\n"
"  ftptimeout     - For FTP, the server response timeout\n"
"  header         - Retrieve header information (boolean)\n"
"  httptimeout    - Number of seconds to wait for HTTP response\n"
"  maxredirs      - Maximum number of redirects to follow\n"
"  proxy          - Hostname or IP to use as a proxy\n"
"  proxytype      - http, socks4, or socks5\n"
"  proxyport      - port number of the proxy\n"
"  proxyuserpwd   - A <user>:<pass> to use for authentication\n"
"  referer        - Referer URL to use for the request\n"
"  useragent      - UserAgent string to use\n"
"  userpwd        - A <user>:<pass> to use for authentication\n"
"  ssl_verifypeer - Whether to verify the peer certificate (boolean)\n"
"  hashcompat     - Result data will be compatible for use with HASH()\n"
"",
	.read = acf_curlopt_read,
	.write = acf_curlopt_write,
};

static int unload_module(void)
{
	int res;

	res = tris_custom_function_unregister(&acf_curl);
	res |= tris_custom_function_unregister(&acf_curlopt);

	return res;
}

static int load_module(void)
{
	int res;

	if (!tris_module_check("res_curl.so")) {
		if (tris_load_resource("res_curl.so") != TRIS_MODULE_LOAD_SUCCESS) {
			tris_log(LOG_ERROR, "Cannot load res_curl, so func_curl cannot be loaded\n");
			return TRIS_MODULE_LOAD_DECLINE;
		}
	}

	res = tris_custom_function_register(&acf_curl);
	res |= tris_custom_function_register(&acf_curlopt);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Load external URL");

