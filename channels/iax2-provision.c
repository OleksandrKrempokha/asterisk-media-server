/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief IAX Provisioning Protocol 
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <sys/socket.h>

#include "trismedia/config.h"
#include "trismedia/cli.h"
#include "trismedia/lock.h"
#include "trismedia/frame.h"
#include "trismedia/md5.h"
#include "trismedia/astdb.h"
#include "trismedia/utils.h"
#include "trismedia/acl.h"
#include "iax2.h"
#include "iax2-provision.h"
#include "iax2-parser.h"

static int provinit = 0;

struct iax_template {
	int dead;
	char name[80];
	char src[80];
	char user[20];
	char pass[20];
	char lang[10];
	unsigned short port;
	unsigned int server;
	unsigned short serverport;
	unsigned int altserver;
	unsigned int flags;
	unsigned int format;
	unsigned int tos;
	TRIS_LIST_ENTRY(iax_template) list;
};

static TRIS_LIST_HEAD_NOLOCK_STATIC(templates, iax_template);

TRIS_MUTEX_DEFINE_STATIC(provlock);

static struct iax_flag {
	char *name;
	int value;
} iax_flags[] = {
	{ "register", PROV_FLAG_REGISTER },
	{ "secure", PROV_FLAG_SECURE },
	{ "heartbeat", PROV_FLAG_HEARTBEAT },
	{ "debug", PROV_FLAG_DEBUG },
	{ "disablecid", PROV_FLAG_DIS_CALLERID },
	{ "disablecw", PROV_FLAG_DIS_CALLWAIT },
	{ "disablecidcw", PROV_FLAG_DIS_CIDCW },
	{ "disable3way", PROV_FLAG_DIS_THREEWAY },
};

char *iax_provflags2str(char *buf, int buflen, unsigned int flags)
{
	int x;

	if (!buf || buflen < 1)
		return NULL;
	
	buf[0] = '\0';

	for (x = 0; x < ARRAY_LEN(iax_flags); x++) {
		if (flags & iax_flags[x].value){
			strncat(buf, iax_flags[x].name, buflen - strlen(buf) - 1);
			strncat(buf, ",", buflen - strlen(buf) - 1);
		}
	}

	if (!tris_strlen_zero(buf)) 
		buf[strlen(buf) - 1] = '\0';
	else
		strncpy(buf, "none", buflen - 1);

	return buf;
}

static unsigned int iax_str2flags(const char *buf)
{
	int x;
	int len;
	int found;
	unsigned int flags = 0;
	char *e;
	while(buf && *buf) {
		e = strchr(buf, ',');
		if (e)
			len = e - buf;
		else
			len = 0;
		found = 0;
		for (x = 0; x < ARRAY_LEN(iax_flags); x++) {
			if ((len && !strncasecmp(iax_flags[x].name, buf, len)) ||
			    (!len && !strcasecmp(iax_flags[x].name, buf))) {
				flags |= iax_flags[x].value;
				break;
			}
		}
		if (e) {
			buf = e + 1;
			while(*buf && (*buf < 33))
				buf++;
		} else
			break;
	}
	return flags;
}

static void iax_template_copy(struct iax_template *dst, struct iax_template *src)
{
	if (!dst || !src) {
		return;
	}

	dst->dead = src->dead;
	tris_copy_string(dst->name, src->name, sizeof(dst->name));
	tris_copy_string(dst->src, src->src, sizeof(dst->src));
	tris_copy_string(dst->user, src->user, sizeof(dst->user));
	tris_copy_string(dst->pass, src->pass, sizeof(dst->pass));
	tris_copy_string(dst->lang, src->lang, sizeof(dst->lang));
	dst->port = src->port;
	dst->server = src->server;
	dst->altserver = src->altserver;
	dst->flags = src->flags;
	dst->format = src->format;
	dst->tos = src->tos;
}

static struct iax_template *iax_template_find(const char *s, int allowdead)
{
	struct iax_template *cur;

	TRIS_LIST_TRAVERSE(&templates, cur, list) {
		if (!strcasecmp(s, cur->name)) {
			if (!allowdead && cur->dead) {
				cur = NULL;
			}
			break;
		}
	}

	return cur;
}

char *iax_prov_complete_template(const char *line, const char *word, int pos, int state)
{
	struct iax_template *c;
	int which=0;
	char *ret = NULL;
	int wordlen = strlen(word);

	if (pos == 3) {
		tris_mutex_lock(&provlock);
		TRIS_LIST_TRAVERSE(&templates, c, list) {
			if (!strncasecmp(word, c->name, wordlen) && ++which > state) {
				ret = tris_strdup(c->name);
				break;
			}
		}
		tris_mutex_unlock(&provlock);
	}
	return ret;
}

static unsigned int prov_ver_calc(struct iax_ie_data *provdata)
{
	struct MD5Context md5;
	unsigned int tmp[4];
	MD5Init(&md5);
	MD5Update(&md5, provdata->buf, provdata->pos);
	MD5Final((unsigned char *)tmp, &md5);
	return tmp[0] ^ tmp[1] ^ tmp[2] ^ tmp[3];
}

int iax_provision_build(struct iax_ie_data *provdata, unsigned int *signature, const char *template, int force)
{
	struct iax_template *cur;
	unsigned int sig;
	char tmp[40];
	memset(provdata, 0, sizeof(*provdata));
	tris_mutex_lock(&provlock);
	cur = iax_template_find(template, 1);
	/* If no match, try searching for '*' */
	if (!cur)
		cur = iax_template_find("*", 1);
	if (cur) {
		/* found it -- add information elements as appropriate */
		if (force || strlen(cur->user))
			iax_ie_append_str(provdata, PROV_IE_USER, cur->user);
		if (force || strlen(cur->pass))
			iax_ie_append_str(provdata, PROV_IE_PASS, cur->pass);
		if (force || strlen(cur->lang))
			iax_ie_append_str(provdata, PROV_IE_LANG, cur->lang);
		if (force || cur->port)
			iax_ie_append_short(provdata, PROV_IE_PORTNO, cur->port);
		if (force || cur->server)
			iax_ie_append_int(provdata, PROV_IE_SERVERIP, cur->server);
		if (force || cur->serverport)
			iax_ie_append_short(provdata, PROV_IE_SERVERPORT, cur->serverport);
		if (force || cur->altserver)
			iax_ie_append_int(provdata, PROV_IE_ALTSERVER, cur->altserver);
		if (force || cur->flags)
			iax_ie_append_int(provdata, PROV_IE_FLAGS, cur->flags);
		if (force || cur->format)
			iax_ie_append_int(provdata, PROV_IE_FORMAT, cur->format);
		if (force || cur->tos)
			iax_ie_append_byte(provdata, PROV_IE_TOS, cur->tos);
		
		/* Calculate checksum of message so far */
		sig = prov_ver_calc(provdata);
		if (signature)
			*signature = sig;
		/* Store signature */
		iax_ie_append_int(provdata, PROV_IE_PROVVER, sig);
		/* Cache signature for later verification so we need not recalculate all this */
		snprintf(tmp, sizeof(tmp), "v0x%08x", sig);
		tris_db_put("iax/provisioning/cache", template, tmp);
	} else
		tris_db_put("iax/provisioning/cache", template, "u");
	tris_mutex_unlock(&provlock);
	return cur ? 0 : -1;
}

int iax_provision_version(unsigned int *version, const char *template, int force)
{
	char tmp[80] = "";
	struct iax_ie_data ied;
	int ret=0;
	memset(&ied, 0, sizeof(ied));

	tris_mutex_lock(&provlock);
	tris_db_get("iax/provisioning/cache", template, tmp, sizeof(tmp));
	if (sscanf(tmp, "v%30x", version) != 1) {
		if (strcmp(tmp, "u")) {
			ret = iax_provision_build(&ied, version, template, force);
			if (ret)
				tris_debug(1, "Unable to create provisioning packet for '%s'\n", template);
		} else
			ret = -1;
	} else
		tris_debug(1, "Retrieved cached version '%s' = '%08x'\n", tmp, *version);
	tris_mutex_unlock(&provlock);
	return ret;
}

static int iax_template_parse(struct iax_template *cur, struct tris_config *cfg, const char *s, const char *def)
{
	struct tris_variable *v;
	int foundportno = 0;
	int foundserverportno = 0;
	int x;
	struct in_addr ia;
	struct hostent *hp;
	struct tris_hostent h;
	struct iax_template *src, tmp;
	const char *t;
	if (def) {
		t = tris_variable_retrieve(cfg, s ,"template");
		src = NULL;
		if (t && strlen(t)) {
			src = iax_template_find(t, 0);
			if (!src)
				tris_log(LOG_WARNING, "Unable to find base template '%s' for creating '%s'.  Trying '%s'\n", t, s, def);
			else
				def = t;
		} 
		if (!src) {
			src = iax_template_find(def, 0);
			if (!src)
				tris_log(LOG_WARNING, "Unable to locate default base template '%s' for creating '%s', omitting.\n", def, s);
		}
		if (!src)
			return -1;
		tris_mutex_lock(&provlock);
		/* Backup old data */
		iax_template_copy(&tmp, cur);
		/* Restore from src */
		iax_template_copy(cur, src);
		/* Restore important headers */
		memcpy(cur->name, tmp.name, sizeof(cur->name));
		cur->dead = tmp.dead;
		tris_mutex_unlock(&provlock);
	}
	if (def)
		strncpy(cur->src, def, sizeof(cur->src) - 1);
	else
		cur->src[0] = '\0';
	v = tris_variable_browse(cfg, s);
	while(v) {
		if (!strcasecmp(v->name, "port") || !strcasecmp(v->name, "serverport")) {
			if ((sscanf(v->value, "%5d", &x) == 1) && (x > 0) && (x < 65535)) {
				if (!strcasecmp(v->name, "port")) {
					cur->port = x;
					foundportno = 1;
				} else {
					cur->serverport = x;
					foundserverportno = 1;
				}
			} else
				tris_log(LOG_WARNING, "Ignoring invalid %s '%s' for '%s' at line %d\n", v->name, v->value, s, v->lineno);
		} else if (!strcasecmp(v->name, "server") || !strcasecmp(v->name, "altserver")) {
			hp = tris_gethostbyname(v->value, &h);
			if (hp) {
				memcpy(&ia, hp->h_addr, sizeof(ia));
				if (!strcasecmp(v->name, "server"))
					cur->server = ntohl(ia.s_addr);
				else
					cur->altserver = ntohl(ia.s_addr);
			} else 
				tris_log(LOG_WARNING, "Ignoring invalid %s '%s' for '%s' at line %d\n", v->name, v->value, s, v->lineno);
		} else if (!strcasecmp(v->name, "codec")) {
			if ((x = tris_getformatbyname(v->value)) > 0) {
				cur->format = x;
			} else
				tris_log(LOG_WARNING, "Ignoring invalid codec '%s' for '%s' at line %d\n", v->value, s, v->lineno);
		} else if (!strcasecmp(v->name, "tos")) {
			if (tris_str2tos(v->value, &cur->tos))
				tris_log(LOG_WARNING, "Invalid tos value at line %d, refer to QoS documentation\n", v->lineno);
		} else if (!strcasecmp(v->name, "user")) {
			strncpy(cur->user, v->value, sizeof(cur->user) - 1);
			if (strcmp(cur->user, v->value))
				tris_log(LOG_WARNING, "Truncating username from '%s' to '%s' for '%s' at line %d\n", v->value, cur->user, s, v->lineno);
		} else if (!strcasecmp(v->name, "pass")) {
			strncpy(cur->pass, v->value, sizeof(cur->pass) - 1);
			if (strcmp(cur->pass, v->value))
				tris_log(LOG_WARNING, "Truncating password from '%s' to '%s' for '%s' at line %d\n", v->value, cur->pass, s, v->lineno);
		} else if (!strcasecmp(v->name, "language")) {
			strncpy(cur->lang, v->value, sizeof(cur->lang) - 1);
			if (strcmp(cur->lang, v->value))
				tris_log(LOG_WARNING, "Truncating language from '%s' to '%s' for '%s' at line %d\n", v->value, cur->lang, s, v->lineno);
		} else if (!strcasecmp(v->name, "flags")) {
			cur->flags = iax_str2flags(v->value);
		} else if (!strncasecmp(v->name, "flags", 5) && strchr(v->name, '+')) {
			cur->flags |= iax_str2flags(v->value);
		} else if (!strncasecmp(v->name, "flags", 5) && strchr(v->name, '-')) {
			cur->flags &= ~iax_str2flags(v->value);
		} else if (strcasecmp(v->name, "template")) {
			tris_log(LOG_WARNING, "Unknown keyword '%s' in definition of '%s' at line %d\n", v->name, s, v->lineno);
		}
		v = v->next;
	}
	if (!foundportno)
		cur->port = IAX_DEFAULT_PORTNO;
	if (!foundserverportno)
		cur->serverport = IAX_DEFAULT_PORTNO;
	return 0;
}

static int iax_process_template(struct tris_config *cfg, char *s, char *def)
{
	/* Find an already existing one if there */
	struct iax_template *cur;
	int mallocd = 0;

	cur = iax_template_find(s, 1 /* allow dead */);
	if (!cur) {
		mallocd = 1;
		cur = tris_calloc(1, sizeof(*cur));
		if (!cur) {
			tris_log(LOG_WARNING, "Out of memory!\n");
			return -1;
		}
		/* Initialize entry */
		strncpy(cur->name, s, sizeof(cur->name) - 1);
		cur->dead = 1;
	}
	if (!iax_template_parse(cur, cfg, s, def))
		cur->dead = 0;

	/* Link if we're mallocd */
	if (mallocd) {
		tris_mutex_lock(&provlock);
		TRIS_LIST_INSERT_HEAD(&templates, cur, list);
		tris_mutex_unlock(&provlock);
	}
	return 0;
}

static const char *ifthere(const char *s)
{
	if (strlen(s))
		return s;
	else
		return "<unspecified>";
}

static const char *iax_server(unsigned int addr)
{
	struct in_addr ia;
	
	if (!addr)
		return "<unspecified>";
	
	ia.s_addr = htonl(addr);

	return tris_inet_ntoa(ia);
}


static char *iax_show_provisioning(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct iax_template *cur;
	char server[INET_ADDRSTRLEN];
	char alternate[INET_ADDRSTRLEN];
	char flags[80];	/* Has to be big enough for 'flags' too */
	int found = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "iax2 show provisioning";
		e->usage =
			"Usage: iax2 show provisioning [template]\n"
			"       Lists all known IAX provisioning templates or a\n"
			"       specific one if specified.\n";
		return NULL;
	case CLI_GENERATE:
		return iax_prov_complete_template(a->line, a->word, a->pos, a->n);
	}

	if ((a->argc != 3) && (a->argc != 4))
		return CLI_SHOWUSAGE;

	tris_mutex_lock(&provlock);
	TRIS_LIST_TRAVERSE(&templates, cur, list) {
		if ((a->argc == 3) || (!strcasecmp(a->argv[3], cur->name)))  {
			if (found) 
				tris_cli(a->fd, "\n");
			tris_copy_string(server, iax_server(cur->server), sizeof(server));
			tris_copy_string(alternate, iax_server(cur->altserver), sizeof(alternate));
			tris_cli(a->fd, "== %s ==\n", cur->name);
			tris_cli(a->fd, "Base Templ:   %s\n", strlen(cur->src) ? cur->src : "<none>");
			tris_cli(a->fd, "Username:     %s\n", ifthere(cur->user));
			tris_cli(a->fd, "Secret:       %s\n", ifthere(cur->pass));
			tris_cli(a->fd, "Language:     %s\n", ifthere(cur->lang));
			tris_cli(a->fd, "Bind Port:    %d\n", cur->port);
			tris_cli(a->fd, "Server:       %s\n", server);
			tris_cli(a->fd, "Server Port:  %d\n", cur->serverport);
			tris_cli(a->fd, "Alternate:    %s\n", alternate);
			tris_cli(a->fd, "Flags:        %s\n", iax_provflags2str(flags, sizeof(flags), cur->flags));
			tris_cli(a->fd, "Format:       %s\n", tris_getformatname(cur->format));
			tris_cli(a->fd, "TOS:          0x%x\n", cur->tos);
			found++;
		}
	}
	tris_mutex_unlock(&provlock);
	if (!found) {
		if (a->argc == 3)
			tris_cli(a->fd, "No provisioning templates found\n");
		else
			tris_cli(a->fd, "No provisioning template matching '%s' found\n", a->argv[3]);
	}
	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_iax2_provision[] = {
	TRIS_CLI_DEFINE(iax_show_provisioning, "Display iax provisioning"),
};

static int iax_provision_init(void)
{
	tris_cli_register_multiple(cli_iax2_provision, sizeof(cli_iax2_provision) / sizeof(struct tris_cli_entry));
	provinit = 1;
	return 0;
}

static void iax_provision_free_templates(int dead)
{
	struct iax_template *cur;

	/* Drop dead or not (depending on dead) entries while locked */
	tris_mutex_lock(&provlock);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&templates, cur, list) {
		if ((dead && cur->dead) || !dead) {
			TRIS_LIST_REMOVE_CURRENT(list);
			tris_free(cur);
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	tris_mutex_unlock(&provlock);
}

int iax_provision_unload(void)
{
	provinit = 0;
	tris_cli_unregister_multiple(cli_iax2_provision, sizeof(cli_iax2_provision) / sizeof(struct tris_cli_entry));
	iax_provision_free_templates(0 /* Remove all templates. */);

	return 0;
}

int iax_provision_reload(int reload)
{
	struct tris_config *cfg;
	struct iax_template *cur;
	char *cat;
	int found = 0;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	if (!provinit)
		iax_provision_init();
	
	cfg = tris_config_load2("iaxprov.conf", "chan_iax2", config_flags);
	if (cfg != NULL && cfg != CONFIG_STATUS_FILEUNCHANGED && cfg != CONFIG_STATUS_FILEINVALID) {
		/* Mark all as dead.  No need for locking */
		TRIS_LIST_TRAVERSE(&templates, cur, list) {
			cur->dead = 1;
		}

		/* Load as appropriate */
		cat = tris_category_browse(cfg, NULL);
		while(cat) {
			if (strcasecmp(cat, "general")) {
				iax_process_template(cfg, cat, found ? "default" : NULL);
				found++;
				tris_verb(3, "Loaded provisioning template '%s'\n", cat);
			}
			cat = tris_category_browse(cfg, cat);
		}
		tris_config_destroy(cfg);
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED)
		return 0;
	else
		tris_log(LOG_NOTICE, "No IAX provisioning configuration found, IAX provisioning disabled.\n");

	iax_provision_free_templates(1 /* remove only marked as dead */);

	/* Purge cached signature DB entries */
	tris_db_deltree("iax/provisioning/cache", NULL);
	return 0;
}
