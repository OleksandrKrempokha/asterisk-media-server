/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Matthew Brooks <mbrooks@digium.com>
 * Terry Wilson <twilson@digium.com>
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
 * \brief Phone provisioning application for the trismedia internal http server
 *
 * \author Matthew Brooks <mbrooks@digium.com>
 * \author Terry Wilson <twilson@digium.com>
 */

#include "trismedia.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#ifdef SOLARIS
#include <sys/sockio.h>
#endif
TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 222187 $")

#include "trismedia/channel.h"
#include "trismedia/file.h"
#include "trismedia/paths.h"
#include "trismedia/pbx.h"
#include "trismedia/cli.h"
#include "trismedia/module.h"
#include "trismedia/http.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"
#include "trismedia/strings.h"
#include "trismedia/stringfields.h"
#include "trismedia/options.h"
#include "trismedia/config.h"
#include "trismedia/acl.h"
#include "trismedia/astobj2.h"
#include "trismedia/tris_version.h"

#ifdef LOW_MEMORY
#define MAX_PROFILE_BUCKETS 1
#define MAX_ROUTE_BUCKETS 1
#define MAX_USER_BUCKETS 1
#else
#define MAX_PROFILE_BUCKETS 17
#define MAX_ROUTE_BUCKETS 563
#define MAX_USER_BUCKETS 563
#endif /* LOW_MEMORY */

#define VAR_BUF_SIZE 4096

/*! \brief for use in lookup_iface */
static struct in_addr __ourip = { .s_addr = 0x00000000, };

/* \note This enum and the pp_variable_list must be in the same order or
 * bad things happen! */
enum pp_variables {
	PP_MACADDRESS,
	PP_USERNAME,
	PP_FULLNAME,
	PP_SECRET,
	PP_LABEL,
	PP_CALLERID,
	PP_TIMEZONE,
	PP_LINENUMBER,
	PP_LINEKEYS,
	PP_VAR_LIST_LENGTH,	/* This entry must always be the last in the list */
};

/*! \brief Lookup table to translate between users.conf property names and
 * variables for use in phoneprov templates */
static const struct pp_variable_lookup {
	enum pp_variables id;
	const char * const user_var;
	const char * const template_var;
} pp_variable_list[] = {
	{ PP_MACADDRESS, "macaddress", "MAC" },
	{ PP_USERNAME, "username", "USERNAME" },
	{ PP_FULLNAME, "fullname", "DISPLAY_NAME" },
	{ PP_SECRET, "secret", "SECRET" },
	{ PP_LABEL, "label", "LABEL" },
	{ PP_CALLERID, "cid_number", "CALLERID" },
	{ PP_TIMEZONE, "timezone", "TIMEZONE" },
	{ PP_LINENUMBER, "linenumber", "LINE" },
 	{ PP_LINEKEYS, "linekeys", "LINEKEYS" },
};

/*! \brief structure to hold file data */
struct phoneprov_file {
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(format);	/*!< After variable substitution, becomes route->uri */
		TRIS_STRING_FIELD(template); /*!< Template/physical file location */
		TRIS_STRING_FIELD(mime_type);/*!< Mime-type of the file */
	);
	TRIS_LIST_ENTRY(phoneprov_file) entry;
};

/*! \brief structure to hold phone profiles read from phoneprov.conf */
struct phone_profile {
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(name);	/*!< Name of phone profile */
		TRIS_STRING_FIELD(default_mime_type);	/*!< Default mime type if it isn't provided */
		TRIS_STRING_FIELD(staticdir);	/*!< Subdirectory that static files are stored in */
	);
	struct varshead *headp;	/*!< List of variables set with 'setvar' in phoneprov.conf */
	TRIS_LIST_HEAD_NOLOCK(, phoneprov_file) static_files;	/*!< List of static files */
	TRIS_LIST_HEAD_NOLOCK(, phoneprov_file) dynamic_files;	/*!< List of dynamic files */
};

struct extension {
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(name);
	);
	int index;
	struct varshead *headp;	/*!< List of variables to substitute into templates */
	TRIS_LIST_ENTRY(extension) entry;
};

/*! \brief structure to hold users read from users.conf */
struct user {
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(macaddress);	/*!< Mac address of user's phone */
	);
	struct phone_profile *profile;	/*!< Profile the phone belongs to */
	TRIS_LIST_HEAD_NOLOCK(, extension) extensions;
};

/*! \brief structure to hold http routes (valid URIs, and the files they link to) */
struct http_route {
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(uri);	/*!< The URI requested */
	);
	struct phoneprov_file *file;	/*!< The file that links to the URI */
	struct user *user;	/*!< The user that has variables to substitute into the file
						 * NULL in the case of a static route */
};

static struct ao2_container *profiles;
static struct ao2_container *http_routes;
static struct ao2_container *users;

/*! \brief Extensions whose mime types we think we know */
static struct {
	char *ext;
	char *mtype;
} mimetypes[] = {
	{ "png", "image/png" },
	{ "xml", "text/xml" },
	{ "jpg", "image/jpeg" },
	{ "js", "application/x-javascript" },
	{ "wav", "audio/x-wav" },
	{ "mp3", "audio/mpeg" },
};

static char global_server[80] = "";	/*!< Server to substitute into templates */
static char global_serverport[6] = "";	/*!< Server port to substitute into templates */
static char global_default_profile[80] = "";	/*!< Default profile to use if one isn't specified */

/*! \brief List of global variables currently available: VOICEMAIL_EXTEN, EXTENSION_LENGTH */
static struct varshead global_variables;
static tris_mutex_t globals_lock;

/*! \brief Return mime type based on extension */
static char *ftype2mtype(const char *ftype)
{
	int x;

	if (tris_strlen_zero(ftype))
		return NULL;

	for (x = 0;x < ARRAY_LEN(mimetypes);x++) {
		if (!strcasecmp(ftype, mimetypes[x].ext))
			return mimetypes[x].mtype;
	}

	return NULL;
}

/* iface is the interface (e.g. eth0); address is the return value */
static int lookup_iface(const char *iface, struct in_addr *address)
{
	int mysock, res = 0;
	struct ifreq ifr;
	struct sockaddr_in *sin;

	memset(&ifr, 0, sizeof(ifr));
	tris_copy_string(ifr.ifr_name, iface, sizeof(ifr.ifr_name));

	mysock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (mysock < 0) {
		tris_log(LOG_ERROR, "Failed to create socket: %s\n", strerror(errno));
		return -1;
	}

	res = ioctl(mysock, SIOCGIFADDR, &ifr);

	close(mysock);

	if (res < 0) {
		tris_log(LOG_WARNING, "Unable to get IP of %s: %s\n", iface, strerror(errno));
		memcpy(address, &__ourip, sizeof(__ourip));
		return -1;
	} else {
		sin = (struct sockaddr_in *)&ifr.ifr_addr;
		memcpy(address, &sin->sin_addr, sizeof(*address));
		return 0;
	}
}

static struct phone_profile *unref_profile(struct phone_profile *prof)
{
	ao2_ref(prof, -1);

	return NULL;
}

/*! \brief Return a phone profile looked up by name */
static struct phone_profile *find_profile(const char *name)
{
	struct phone_profile tmp = {
		.name = name,
	};

	return ao2_find(profiles, &tmp, OBJ_POINTER);
}

static int profile_hash_fn(const void *obj, const int flags)
{
	const struct phone_profile *profile = obj;

	return tris_str_case_hash(profile->name);
}

static int profile_cmp_fn(void *obj, void *arg, int flags)
{
	const struct phone_profile *profile1 = obj, *profile2 = arg;

	return !strcasecmp(profile1->name, profile2->name) ? CMP_MATCH | CMP_STOP : 0;
}

static void delete_file(struct phoneprov_file *file)
{
	tris_string_field_free_memory(file);
	free(file);
}

static void profile_destructor(void *obj)
{
	struct phone_profile *profile = obj;
	struct phoneprov_file *file;
	struct tris_var_t *var;

	while ((file = TRIS_LIST_REMOVE_HEAD(&profile->static_files, entry)))
		delete_file(file);

	while ((file = TRIS_LIST_REMOVE_HEAD(&profile->dynamic_files, entry)))
		delete_file(file);

	while ((var = TRIS_LIST_REMOVE_HEAD(profile->headp, entries)))
		tris_var_delete(var);

	tris_free(profile->headp);
	tris_string_field_free_memory(profile);
}

static struct http_route *unref_route(struct http_route *route)
{
	ao2_ref(route, -1);

	return NULL;
}

static int routes_hash_fn(const void *obj, const int flags)
{
	const struct http_route *route = obj;

	return tris_str_case_hash(route->uri);
}

static int routes_cmp_fn(void *obj, void *arg, int flags)
{
	const struct http_route *route1 = obj, *route2 = arg;

	return !strcasecmp(route1->uri, route2->uri) ? CMP_MATCH | CMP_STOP : 0;
}

static void route_destructor(void *obj)
{
	struct http_route *route = obj;

	tris_string_field_free_memory(route);
}

/*! \brief Read a TEXT file into a string and return the length */
static int load_file(const char *filename, char **ret)
{
	int len = 0;
	FILE *f;

	if (!(f = fopen(filename, "r"))) {
		*ret = NULL;
		return -1;
	}

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (!(*ret = tris_malloc(len + 1)))
		return -2;

	if (len != fread(*ret, sizeof(char), len, f)) {
		free(*ret);
		*ret = NULL;
		return -3;
	}

	fclose(f);

	(*ret)[len] = '\0';

	return len;
}

/*! \brief Set all timezone-related variables based on a zone (i.e. America/New_York)
	\param headp pointer to list of user variables
	\param zone A time zone. NULL sets variables based on timezone of the machine
*/
static void set_timezone_variables(struct varshead *headp, const char *zone)
{
	time_t utc_time;
	int dstenable;
	time_t dststart;
	time_t dstend;
	struct tris_tm tm_info;
	int tzoffset;
	char buffer[21];
	struct tris_var_t *var;
	struct timeval when;

	time(&utc_time);
	tris_get_dst_info(&utc_time, &dstenable, &dststart, &dstend, &tzoffset, zone);
	snprintf(buffer, sizeof(buffer), "%d", tzoffset);
	var = tris_var_assign("TZOFFSET", buffer);
	if (var)
		TRIS_LIST_INSERT_TAIL(headp, var, entries);

	if (!dstenable)
		return;

	if ((var = tris_var_assign("DST_ENABLE", "1")))
		TRIS_LIST_INSERT_TAIL(headp, var, entries);

	when.tv_sec = dststart;
	tris_localtime(&when, &tm_info, zone);

	snprintf(buffer, sizeof(buffer), "%d", tm_info.tm_mon+1);
	if ((var = tris_var_assign("DST_START_MONTH", buffer)))
		TRIS_LIST_INSERT_TAIL(headp, var, entries);

	snprintf(buffer, sizeof(buffer), "%d", tm_info.tm_mday);
	if ((var = tris_var_assign("DST_START_MDAY", buffer)))
		TRIS_LIST_INSERT_TAIL(headp, var, entries);

	snprintf(buffer, sizeof(buffer), "%d", tm_info.tm_hour);
	if ((var = tris_var_assign("DST_START_HOUR", buffer)))
		TRIS_LIST_INSERT_TAIL(headp, var, entries);

	when.tv_sec = dstend;
	tris_localtime(&when, &tm_info, zone);

	snprintf(buffer, sizeof(buffer), "%d", tm_info.tm_mon + 1);
	if ((var = tris_var_assign("DST_END_MONTH", buffer)))
		TRIS_LIST_INSERT_TAIL(headp, var, entries);

	snprintf(buffer, sizeof(buffer), "%d", tm_info.tm_mday);
	if ((var = tris_var_assign("DST_END_MDAY", buffer)))
		TRIS_LIST_INSERT_TAIL(headp, var, entries);

	snprintf(buffer, sizeof(buffer), "%d", tm_info.tm_hour);
	if ((var = tris_var_assign("DST_END_HOUR", buffer)))
		TRIS_LIST_INSERT_TAIL(headp, var, entries);
}

/*! \brief Callback that is executed everytime an http request is received by this module */
static struct tris_str *phoneprov_callback(struct tris_tcptls_session_instance *ser, const struct tris_http_uri *urih, const char *uri, enum tris_http_method method, struct tris_variable *vars, struct tris_variable *headers, int *status, char **title, int *contentlength)
{
	struct http_route *route;
	struct http_route search_route = {
		.uri = uri,
	};
	struct tris_str *result = tris_str_create(512);
	char path[PATH_MAX];
	char *file = NULL;
	int len;
	int fd;
	char buf[256];
	struct timeval now = tris_tvnow();
	struct tris_tm tm;

	if (!(route = ao2_find(http_routes, &search_route, OBJ_POINTER))) {
		goto out404;
	}

	snprintf(path, sizeof(path), "%s/phoneprov/%s", tris_config_TRIS_DATA_DIR, route->file->template);

	if (!route->user) { /* Static file */

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			goto out500;
		}

		len = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		if (len < 0) {
			tris_log(LOG_WARNING, "Could not load file: %s (%d)\n", path, len);
			close(fd);
			goto out500;
		}

		tris_strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %Z", tris_localtime(&now, &tm, "GMT"));
		fprintf(ser->f, "HTTP/1.1 200 OK\r\n"
			"Server: Trismedia/%s\r\n"
			"Date: %s\r\n"
			"Connection: close\r\n"
			"Cache-Control: no-cache, no-store\r\n"
			"Content-Length: %d\r\n"
			"Content-Type: %s\r\n\r\n",
			tris_get_version(), buf, len, route->file->mime_type);

		while ((len = read(fd, buf, sizeof(buf))) > 0) {
			if (fwrite(buf, 1, len, ser->f) != len) {
				if (errno != EPIPE) {
					tris_log(LOG_WARNING, "fwrite() failed: %s\n", strerror(errno));
				} else {
					tris_debug(3, "Requester closed the connection while downloading '%s'\n", path);
				}
				break;
			}
		}

		close(fd);
		route = unref_route(route);
		return NULL;
	} else { /* Dynamic file */
		int bufsize;
		char *tmp;

		len = load_file(path, &file);
		if (len < 0) {
			tris_log(LOG_WARNING, "Could not load file: %s (%d)\n", path, len);
			if (file) {
				tris_free(file);
			}

			goto out500;
		}

		if (!file) {
			goto out500;
		}

		/* XXX This is a hack -- maybe sum length of all variables in route->user->headp and add that? */
 		bufsize = len + VAR_BUF_SIZE;

		/* malloc() instead of alloca() here, just in case the file is bigger than
		 * we have enough stack space for. */
		if (!(tmp = tris_calloc(1, bufsize))) {
			if (file) {
				tris_free(file);
			}

			goto out500;
		}

		/* Unless we are overridden by serveriface or serveraddr, we set the SERVER variable to
		 * the IP address we are listening on that the phone contacted for this config file */
		if (tris_strlen_zero(global_server)) {
			union {
				struct sockaddr sa;
				struct sockaddr_in sa_in;
			} name;
			socklen_t namelen = sizeof(name.sa);
			int res;

			if ((res = getsockname(ser->fd, &name.sa, &namelen))) {
				tris_log(LOG_WARNING, "Could not get server IP, breakage likely.\n");
			} else {
				struct tris_var_t *var;
				struct extension *exten_iter;

				if ((var = tris_var_assign("SERVER", tris_inet_ntoa(name.sa_in.sin_addr)))) {
					TRIS_LIST_TRAVERSE(&route->user->extensions, exten_iter, entry) {
						TRIS_LIST_INSERT_TAIL(exten_iter->headp, var, entries);
					}
				}
			}
		}

		pbx_substitute_variables_varshead(TRIS_LIST_FIRST(&route->user->extensions)->headp, file, tmp, bufsize);

		if (file) {
			tris_free(file);
		}

		tris_str_append(&result, 0,
			"Content-Type: %s\r\n"
			"Content-length: %d\r\n"
			"\r\n"
			"%s", route->file->mime_type, (int) strlen(tmp), tmp);

		if (tmp) {
			tris_free(tmp);
		}

		route = unref_route(route);

		return result;
	}

out404:
	*status = 404;
	*title = strdup("Not Found");
	*contentlength = 0;
	return tris_http_error(404, "Not Found", NULL, "The requested URL was not found on this server.");

out500:
	route = unref_route(route);
	*status = 500;
	*title = strdup("Internal Server Error");
	*contentlength = 0;
	return tris_http_error(500, "Internal Error", NULL, "An internal error has occured.");
}

/*! \brief Build a route structure and add it to the list of available http routes
	\param pp_file File to link to the route
	\param user User to link to the route (NULL means static route)
	\param uri URI of the route
*/
static void build_route(struct phoneprov_file *pp_file, struct user *user, char *uri)
{
	struct http_route *route;

	if (!(route = ao2_alloc(sizeof(*route), route_destructor))) {
		return;
	}

	if (tris_string_field_init(route, 32)) {
		tris_log(LOG_ERROR, "Couldn't create string fields for %s\n", pp_file->format);
		route = unref_route(route);
		return;
	}

	tris_string_field_set(route, uri, S_OR(uri, pp_file->format));
	route->user = user;
	route->file = pp_file;

	ao2_link(http_routes, route);

	route = unref_route(route);
}

/*! \brief Build a phone profile and add it to the list of phone profiles
	\param name the name of the profile
	\param v tris_variable from parsing phoneprov.conf
*/
static void build_profile(const char *name, struct tris_variable *v)
{
	struct phone_profile *profile;
	struct tris_var_t *var;

	if (!(profile = ao2_alloc(sizeof(*profile), profile_destructor))) {
		return;
	}

	if (tris_string_field_init(profile, 32)) {
		profile = unref_profile(profile);
		return;
	}

	if (!(profile->headp = tris_calloc(1, sizeof(*profile->headp)))) {
		profile = unref_profile(profile);
		return;
	}

	TRIS_LIST_HEAD_INIT_NOLOCK(&profile->static_files);
	TRIS_LIST_HEAD_INIT_NOLOCK(&profile->dynamic_files);

	tris_string_field_set(profile, name, name);
	for (; v; v = v->next) {
		if (!strcasecmp(v->name, "mime_type")) {
			tris_string_field_set(profile, default_mime_type, v->value);
		} else if (!strcasecmp(v->name, "setvar")) {
			struct tris_var_t *variable;
			char *value_copy = tris_strdupa(v->value);

			TRIS_DECLARE_APP_ARGS(args,
				TRIS_APP_ARG(varname);
				TRIS_APP_ARG(varval);
			);

			TRIS_NONSTANDARD_APP_ARGS(args, value_copy, '=');
			do {
				if (tris_strlen_zero(args.varname) || tris_strlen_zero(args.varval))
					break;
				args.varname = tris_strip(args.varname);
				args.varval = tris_strip(args.varval);
				if (tris_strlen_zero(args.varname) || tris_strlen_zero(args.varval))
					break;
				if ((variable = tris_var_assign(args.varname, args.varval)))
					TRIS_LIST_INSERT_TAIL(profile->headp, variable, entries);
			} while (0);
		} else if (!strcasecmp(v->name, "staticdir")) {
			tris_string_field_set(profile, staticdir, v->value);
		} else {
			struct phoneprov_file *pp_file;
			char *file_extension;
			char *value_copy = tris_strdupa(v->value);

			TRIS_DECLARE_APP_ARGS(args,
				TRIS_APP_ARG(filename);
				TRIS_APP_ARG(mimetype);
			);

			if (!(pp_file = tris_calloc(1, sizeof(*pp_file)))) {
				profile = unref_profile(profile);
				return;
			}
			if (tris_string_field_init(pp_file, 32)) {
				tris_free(pp_file);
				profile = unref_profile(profile);
				return;
			}

			if ((file_extension = strrchr(pp_file->format, '.')))
				file_extension++;

			TRIS_STANDARD_APP_ARGS(args, value_copy);

			/* Mime type order of preference
			 * 1) Specific mime-type defined for file in profile
			 * 2) Mime determined by extension
			 * 3) Default mime type specified in profile
			 * 4) text/plain
			 */
			tris_string_field_set(pp_file, mime_type, S_OR(args.mimetype, (S_OR(S_OR(ftype2mtype(file_extension), profile->default_mime_type), "text/plain"))));

			if (!strcasecmp(v->name, "static_file")) {
				tris_string_field_set(pp_file, format, args.filename);
				tris_string_field_build(pp_file, template, "%s%s", profile->staticdir, args.filename);
				TRIS_LIST_INSERT_TAIL(&profile->static_files, pp_file, entry);
				/* Add a route for the static files, as their filenames won't change per-user */
				build_route(pp_file, NULL, NULL);
			} else {
				tris_string_field_set(pp_file, format, v->name);
				tris_string_field_set(pp_file, template, args.filename);
				TRIS_LIST_INSERT_TAIL(&profile->dynamic_files, pp_file, entry);
			}
		}
	}

	/* Append the global variables to the variables list for this profile.
	 * This is for convenience later, when we need to provide a single
	 * variable list for use in substitution. */
	tris_mutex_lock(&globals_lock);
	TRIS_LIST_TRAVERSE(&global_variables, var, entries) {
		struct tris_var_t *new_var;
		if ((new_var = tris_var_assign(var->name, var->value))) {
			TRIS_LIST_INSERT_TAIL(profile->headp, new_var, entries);
		}
	}
	tris_mutex_unlock(&globals_lock);

	ao2_link(profiles, profile);

	profile = unref_profile(profile);
}

static struct extension *delete_extension(struct extension *exten)
{
	struct tris_var_t *var;
	while ((var = TRIS_LIST_REMOVE_HEAD(exten->headp, entries))) {
		tris_var_delete(var);
	}
	tris_free(exten->headp);
	tris_string_field_free_memory(exten);

	tris_free(exten);

	return NULL;
}

static struct extension *build_extension(struct tris_config *cfg, const char *name)
{
	struct extension *exten;
	struct tris_var_t *var;
	const char *tmp;
	int i;

	if (!(exten = tris_calloc(1, sizeof(*exten)))) {
		return NULL;
	}

	if (tris_string_field_init(exten, 32)) {
		tris_free(exten);
		exten = NULL;
		return NULL;
	}

	tris_string_field_set(exten, name, name);

	if (!(exten->headp = tris_calloc(1, sizeof(*exten->headp)))) {
		tris_free(exten);
		exten = NULL;
		return NULL;
	}

	for (i = 0; i < PP_VAR_LIST_LENGTH; i++) {
		tmp = tris_variable_retrieve(cfg, name, pp_variable_list[i].user_var);

		/* If we didn't get a USERNAME variable, set it to the user->name */
		if (i == PP_USERNAME && !tmp) {
			if ((var = tris_var_assign(pp_variable_list[PP_USERNAME].template_var, exten->name))) {
				TRIS_LIST_INSERT_TAIL(exten->headp, var, entries);
			}
			continue;
		} else if (i == PP_TIMEZONE) {
			/* perfectly ok if tmp is NULL, will set variables based on server's time zone */
			set_timezone_variables(exten->headp, tmp);
		} else if (i == PP_LINENUMBER) {
			if (!tmp) {
				tmp = "1";
			}
			exten->index = atoi(tmp);
		} else if (i == PP_LINEKEYS) {
			if (!tmp) {
				tmp = "1";
			}
		}

		if (tmp && (var = tris_var_assign(pp_variable_list[i].template_var, tmp))) {
			TRIS_LIST_INSERT_TAIL(exten->headp, var, entries);
		}
	}

	if (!tris_strlen_zero(global_server)) {
		if ((var = tris_var_assign("SERVER", global_server)))
			TRIS_LIST_INSERT_TAIL(exten->headp, var, entries);
	}

	if (!tris_strlen_zero(global_serverport)) {
		if ((var = tris_var_assign("SERVER_PORT", global_serverport)))
			TRIS_LIST_INSERT_TAIL(exten->headp, var, entries);
	}

	return exten;
}

static struct user *unref_user(struct user *user)
{
	ao2_ref(user, -1);

	return NULL;
}

/*! \brief Return a user looked up by name */
static struct user *find_user(const char *macaddress)
{
	struct user tmp = {
		.macaddress = macaddress,
	};

	return ao2_find(users, &tmp, OBJ_POINTER);
}

static int users_hash_fn(const void *obj, const int flags)
{
	const struct user *user = obj;

	return tris_str_case_hash(user->macaddress);
}

static int users_cmp_fn(void *obj, void *arg, int flags)
{
	const struct user *user1 = obj, *user2 = arg;

	return !strcasecmp(user1->macaddress, user2->macaddress) ? CMP_MATCH | CMP_STOP : 0;
}

/*! \brief Free all memory associated with a user */
static void user_destructor(void *obj)
{
	struct user *user = obj;
	struct extension *exten;

	while ((exten = TRIS_LIST_REMOVE_HEAD(&user->extensions, entry))) {
		exten = delete_extension(exten);
	}

	if (user->profile) {
		user->profile = unref_profile(user->profile);
	}

	tris_string_field_free_memory(user);
}

/*! \brief Delete all users */
static void delete_users(void)
{
	struct ao2_iterator i;
	struct user *user;

	i = ao2_iterator_init(users, 0);
	while ((user = ao2_iterator_next(&i))) {
		ao2_unlink(users, user);
		user = unref_user(user);
	}
	ao2_iterator_destroy(&i);
}

/*! \brief Build and return a user structure based on gathered config data */
static struct user *build_user(const char *mac, struct phone_profile *profile)
{
	struct user *user;

	if (!(user = ao2_alloc(sizeof(*user), user_destructor))) {
		profile = unref_profile(profile);
		return NULL;
	}

	if (tris_string_field_init(user, 32)) {
		profile = unref_profile(profile);
		user = unref_user(user);
		return NULL;
	}

	tris_string_field_set(user, macaddress, mac);
	user->profile = profile; /* already ref counted by find_profile */

	return user;
}

/*! \brief Add an extension to a user ordered by index/linenumber */
static int add_user_extension(struct user *user, struct extension *exten)
{
	struct tris_var_t *var;

	/* Append profile variables here, and substitute variables on profile
	 * setvars, so that we can use user specific variables in them */
	TRIS_LIST_TRAVERSE(user->profile->headp, var, entries) {
		char expand_buf[VAR_BUF_SIZE] = {0,};
		struct tris_var_t *var2;

		pbx_substitute_variables_varshead(exten->headp, var->value, expand_buf, sizeof(expand_buf));
		if ((var2 = tris_var_assign(var->name, expand_buf)))
			TRIS_LIST_INSERT_TAIL(exten->headp, var2, entries);
	}

	if (TRIS_LIST_EMPTY(&user->extensions)) {
		TRIS_LIST_INSERT_HEAD(&user->extensions, exten, entry);
	} else {
		struct extension *exten_iter;

		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&user->extensions, exten_iter, entry) {
			if (exten->index < exten_iter->index) {
				TRIS_LIST_INSERT_BEFORE_CURRENT(exten, entry);
			} else if (exten->index == exten_iter->index) {
				tris_log(LOG_WARNING, "Duplicate linenumber=%d for %s\n", exten->index, user->macaddress);
				return -1;
			} else if (!TRIS_LIST_NEXT(exten_iter, entry)) {
				TRIS_LIST_INSERT_TAIL(&user->extensions, exten, entry);
			}
		}
		TRIS_LIST_TRAVERSE_SAFE_END;
	}

	return 0;
}

/*! \brief Add an http route for dynamic files attached to the profile of the user */
static int build_user_routes(struct user *user)
{
	struct phoneprov_file *pp_file;

	TRIS_LIST_TRAVERSE(&user->profile->dynamic_files, pp_file, entry) {
		char expand_buf[VAR_BUF_SIZE] = { 0, };

		pbx_substitute_variables_varshead(TRIS_LIST_FIRST(&user->extensions)->headp, pp_file->format, expand_buf, sizeof(expand_buf));
		build_route(pp_file, user, expand_buf);
	}

	return 0;
}

/* \brief Parse config files and create appropriate structures */
static int set_config(void)
{
	struct tris_config *cfg, *phoneprov_cfg;
	char *cat;
	struct tris_variable *v;
	struct tris_flags config_flags = { 0 };
	struct tris_var_t *var;

	/* Try to grab the port from sip.conf.  If we don't get it here, we'll set it
	 * to whatever is set in phoneprov.conf or default to 5060 */
	if ((cfg = tris_config_load("sip.conf", config_flags)) && cfg != CONFIG_STATUS_FILEINVALID) {
		tris_copy_string(global_serverport, S_OR(tris_variable_retrieve(cfg, "general", "bindport"), "5060"), sizeof(global_serverport));
		tris_config_destroy(cfg);
	}

	if (!(cfg = tris_config_load("users.conf", config_flags)) || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_WARNING, "Unable to load users.conf\n");
		return 0;
	}

	/* Go ahead and load global variables from users.conf so we can append to profiles */
	for (v = tris_variable_browse(cfg, "general"); v; v = v->next) {
		if (!strcasecmp(v->name, "vmexten")) {
			if ((var = tris_var_assign("VOICEMAIL_EXTEN", v->value))) {
				tris_mutex_lock(&globals_lock);
				TRIS_LIST_INSERT_TAIL(&global_variables, var, entries);
				tris_mutex_unlock(&globals_lock);
			}
		}
		if (!strcasecmp(v->name, "localextenlength")) {
			if ((var = tris_var_assign("EXTENSION_LENGTH", v->value)))
				tris_mutex_lock(&globals_lock);
				TRIS_LIST_INSERT_TAIL(&global_variables, var, entries);
				tris_mutex_unlock(&globals_lock);
		}
	}

	if (!(phoneprov_cfg = tris_config_load("phoneprov.conf", config_flags)) || phoneprov_cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Unable to load config phoneprov.conf\n");
		tris_config_destroy(cfg);
		return -1;
	}

	cat = NULL;
	while ((cat = tris_category_browse(phoneprov_cfg, cat))) {
		if (!strcasecmp(cat, "general")) {
			for (v = tris_variable_browse(phoneprov_cfg, cat); v; v = v->next) {
				if (!strcasecmp(v->name, "serveraddr"))
					tris_copy_string(global_server, v->value, sizeof(global_server));
				else if (!strcasecmp(v->name, "serveriface")) {
					struct in_addr addr;
					lookup_iface(v->value, &addr);
					tris_copy_string(global_server, tris_inet_ntoa(addr), sizeof(global_server));
				} else if (!strcasecmp(v->name, "serverport"))
					tris_copy_string(global_serverport, v->value, sizeof(global_serverport));
				else if (!strcasecmp(v->name, "default_profile"))
					tris_copy_string(global_default_profile, v->value, sizeof(global_default_profile));
			}
		} else
			build_profile(cat, tris_variable_browse(phoneprov_cfg, cat));
	}

	tris_config_destroy(phoneprov_cfg);

	cat = NULL;
	while ((cat = tris_category_browse(cfg, cat))) {
		const char *tmp, *mac;
		struct user *user;
		struct phone_profile *profile;
		struct extension *exten;

		if (!strcasecmp(cat, "general")) {
			continue;
		}

		if (!strcasecmp(cat, "authentication"))
			continue;

		if (!((tmp = tris_variable_retrieve(cfg, cat, "autoprov")) && tris_true(tmp)))
			continue;

		if (!(mac = tris_variable_retrieve(cfg, cat, "macaddress"))) {
			tris_log(LOG_WARNING, "autoprov set for %s, but no mac address - skipping.\n", cat);
			continue;
		}

		tmp = S_OR(tris_variable_retrieve(cfg, cat, "profile"), global_default_profile);
		if (tris_strlen_zero(tmp)) {
			tris_log(LOG_WARNING, "No profile for user [%s] with mac '%s' - skipping\n", cat, mac);
			continue;
		}

		if (!(user = find_user(mac))) {
			if (!(profile = find_profile(tmp))) {
				tris_log(LOG_WARNING, "Could not look up profile '%s' - skipping.\n", tmp);
				continue;
			}

			if (!(user = build_user(mac, profile))) {
				tris_log(LOG_WARNING, "Could not create user for '%s' - skipping\n", user->macaddress);
				continue;
			}

			if (!(exten = build_extension(cfg, cat))) {
				tris_log(LOG_WARNING, "Could not create extension for %s - skipping\n", user->macaddress);
				user = unref_user(user);
				continue;
			}

			if (add_user_extension(user, exten)) {
				tris_log(LOG_WARNING, "Could not add extension '%s' to user '%s'\n", exten->name, user->macaddress);
				user = unref_user(user);
				exten = delete_extension(exten);
				continue;
			}

			if (build_user_routes(user)) {
				tris_log(LOG_WARNING, "Could not create http routes for %s - skipping\n", user->macaddress);
				user = unref_user(user);
				continue;
			}

			ao2_link(users, user);
			user = unref_user(user);
		} else {
			if (!(exten = build_extension(cfg, cat))) {
				tris_log(LOG_WARNING, "Could not create extension for %s - skipping\n", user->macaddress);
				user = unref_user(user);
				continue;
			}

			if (add_user_extension(user, exten)) {
				tris_log(LOG_WARNING, "Could not add extension '%s' to user '%s'\n", exten->name, user->macaddress);
				user = unref_user(user);
				exten = delete_extension(exten);
				continue;
			}

			user = unref_user(user);
		}
	}

	tris_config_destroy(cfg);

	return 0;
}

/*! \brief Delete all http routes, freeing their memory */
static void delete_routes(void)
{
	struct ao2_iterator i;
	struct http_route *route;

	i = ao2_iterator_init(http_routes, 0);
	while ((route = ao2_iterator_next(&i))) {
		ao2_unlink(http_routes, route);
		route = unref_route(route);
	}
	ao2_iterator_destroy(&i);
}

/*! \brief Delete all phone profiles, freeing their memory */
static void delete_profiles(void)
{
	struct ao2_iterator i;
	struct phone_profile *profile;

	i = ao2_iterator_init(profiles, 0);
	while ((profile = ao2_iterator_next(&i))) {
		ao2_unlink(profiles, profile);
		profile = unref_profile(profile);
	}
	ao2_iterator_destroy(&i);
}

/*! \brief A dialplan function that can be used to print a string for each phoneprov user */
static int pp_each_user_exec(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *tmp, expand_buf[VAR_BUF_SIZE] = {0,};
	struct ao2_iterator i;
	struct user *user;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(string);
		TRIS_APP_ARG(exclude_mac);
	);
	TRIS_STANDARD_APP_ARGS(args, data);

	/* Fix data by turning %{ into ${ */
	while ((tmp = strstr(args.string, "%{")))
		*tmp = '$';

	i = ao2_iterator_init(users, 0);
	while ((user = ao2_iterator_next(&i))) {
		if (!tris_strlen_zero(args.exclude_mac) && !strcasecmp(user->macaddress, args.exclude_mac)) {
			continue;
		}
		pbx_substitute_variables_varshead(TRIS_LIST_FIRST(&user->extensions)->headp, args.string, expand_buf, sizeof(expand_buf));
		tris_build_string(&buf, &len, "%s", expand_buf);
		user = unref_user(user);
	}
	ao2_iterator_destroy(&i);

	return 0;
}

static struct tris_custom_function pp_each_user_function = {
	.name = "PP_EACH_USER",
	.synopsis = "Generate a string for each phoneprov user",
	.syntax = "PP_EACH_USER(<string>|<exclude_mac>)",
	.desc =
		"Pass in a string, with phoneprov variables you want substituted in the format of\n"
		"%{VARNAME}, and you will get the string rendered for each user in phoneprov\n"
		"excluding ones with MAC address <exclude_mac>. Probably not useful outside of\n"
		"res_phoneprov.\n"
		"\nExample: ${PP_EACH_USER(<item><fn>%{DISPLAY_NAME}</fn></item>|${MAC})",
	.read = pp_each_user_exec,
};

/*! \brief A dialplan function that can be used to output a template for each extension attached to a user */
static int pp_each_extension_exec(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct user *user;
	struct extension *exten;
	char path[PATH_MAX];
	char *file;
	int filelen;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(mac);
		TRIS_APP_ARG(template);
	);

	TRIS_STANDARD_APP_ARGS(args, data);

	if (tris_strlen_zero(args.mac) || tris_strlen_zero(args.template)) {
		tris_log(LOG_WARNING, "PP_EACH_EXTENSION requries both a macaddress and template filename.\n");
		return 0;
	}

	if (!(user = find_user(args.mac))) {
		tris_log(LOG_WARNING, "Could not find user with mac = '%s'\n", args.mac);
		return 0;
	}

	snprintf(path, sizeof(path), "%s/phoneprov/%s", tris_config_TRIS_DATA_DIR, args.template);
	filelen = load_file(path, &file);
	if (filelen < 0) {
		tris_log(LOG_WARNING, "Could not load file: %s (%d)\n", path, filelen);
		if (file) {
			tris_free(file);
		}
		return 0;
	}

	if (!file) {
		return 0;
	}

	TRIS_LIST_TRAVERSE(&user->extensions, exten, entry) {
		char expand_buf[VAR_BUF_SIZE] = {0,};
		pbx_substitute_variables_varshead(exten->headp, file, expand_buf, sizeof(expand_buf));
		tris_build_string(&buf, &len, "%s", expand_buf);
	}

	tris_free(file);

	user = unref_user(user);

	return 0;
}

static struct tris_custom_function pp_each_extension_function = {
	.name = "PP_EACH_EXTENSION",
	.synopsis = "Execute specified template for each extension",
	.syntax = "PP_EACH_EXTENSION(<mac>|<template>)",
	.desc =
		"Output the specified template for each extension associated with the specified\n"
		"MAC address.",
	.read = pp_each_extension_exec,
};

/*! \brief CLI command to list static and dynamic routes */
static char *handle_show_routes(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMAT "%-40.40s  %-30.30s\n"
	struct ao2_iterator i;
	struct http_route *route;

	switch(cmd) {
	case CLI_INIT:
		e->command = "phoneprov show routes";
		e->usage =
			"Usage: phoneprov show routes\n"
			"       Lists all registered phoneprov http routes.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	/* This currently iterates over routes twice, but it is the only place I've needed
	 * to really separate static an dynamic routes, so I've just left it this way. */
	tris_cli(a->fd, "Static routes\n\n");
	tris_cli(a->fd, FORMAT, "Relative URI", "Physical location");
	i = ao2_iterator_init(http_routes, 0);
	while ((route = ao2_iterator_next(&i))) {
		if (!route->user)
			tris_cli(a->fd, FORMAT, route->uri, route->file->template);
		route = unref_route(route);
	}
	ao2_iterator_destroy(&i);

	tris_cli(a->fd, "\nDynamic routes\n\n");
	tris_cli(a->fd, FORMAT, "Relative URI", "Template");

	i = ao2_iterator_init(http_routes, 0);
	while ((route = ao2_iterator_next(&i))) {
		if (route->user)
			tris_cli(a->fd, FORMAT, route->uri, route->file->template);
		route = unref_route(route);
	}
	ao2_iterator_destroy(&i);

	return CLI_SUCCESS;
}

static struct tris_cli_entry pp_cli[] = {
	TRIS_CLI_DEFINE(handle_show_routes, "Show registered phoneprov http routes"),
};

static struct tris_http_uri phoneprovuri = {
	.callback = phoneprov_callback,
	.description = "Trismedia HTTP Phone Provisioning Tool",
	.uri = "phoneprov",
	.has_subtree = 1,
	.supports_get = 1,
	.data = NULL,
	.key = __FILE__,
};

static int load_module(void)
{
	profiles = ao2_container_alloc(MAX_PROFILE_BUCKETS, profile_hash_fn, profile_cmp_fn);

	http_routes = ao2_container_alloc(MAX_ROUTE_BUCKETS, routes_hash_fn, routes_cmp_fn);

	users = ao2_container_alloc(MAX_USER_BUCKETS, users_hash_fn, users_cmp_fn);

	TRIS_LIST_HEAD_INIT_NOLOCK(&global_variables);
	tris_mutex_init(&globals_lock);

	tris_custom_function_register(&pp_each_user_function);
	tris_custom_function_register(&pp_each_extension_function);
	tris_cli_register_multiple(pp_cli, ARRAY_LEN(pp_cli));

	set_config();
	tris_http_uri_link(&phoneprovuri);

	return 0;
}

static int unload_module(void)
{
	struct tris_var_t *var;

	tris_http_uri_unlink(&phoneprovuri);
	tris_custom_function_unregister(&pp_each_user_function);
	tris_custom_function_unregister(&pp_each_extension_function);
	tris_cli_unregister_multiple(pp_cli, ARRAY_LEN(pp_cli));

	delete_routes();
	delete_users();
	delete_profiles();
	ao2_ref(profiles, -1);
	ao2_ref(http_routes, -1);
	ao2_ref(users, -1);

	tris_mutex_lock(&globals_lock);
	while ((var = TRIS_LIST_REMOVE_HEAD(&global_variables, entries))) {
		tris_var_delete(var);
	}
	tris_mutex_unlock(&globals_lock);

	tris_mutex_destroy(&globals_lock);

	return 0;
}

static int reload(void)
{
	struct tris_var_t *var;

	delete_routes();
	delete_users();
	delete_profiles();

	tris_mutex_lock(&globals_lock);
	while ((var = TRIS_LIST_REMOVE_HEAD(&global_variables, entries))) {
		tris_var_delete(var);
	}
	tris_mutex_unlock(&globals_lock);

	set_config();

	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_GLOBAL_SYMBOLS, "HTTP Phone Provisioning",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	);
