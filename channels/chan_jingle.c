/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Matt O'Gorman <mogorman@digium.com>
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
 * \author Matt O'Gorman <mogorman@digium.com>
 *
 * \brief Jingle Channel Driver
 *
 * \extref Iksemel http://iksemel.jabberstudio.org/
 * 
 * \ingroup channel_drivers
 */

/*** MODULEINFO
	<depend>iksemel</depend>
	<depend>res_jabber</depend>
	<use>openssl</use>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 249895 $")

#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/signal.h>
#include <iksemel.h>
#include <pthread.h>

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/sched.h"
#include "trismedia/io.h"
#include "trismedia/rtp.h"
#include "trismedia/acl.h"
#include "trismedia/callerid.h"
#include "trismedia/file.h"
#include "trismedia/cli.h"
#include "trismedia/app.h"
#include "trismedia/musiconhold.h"
#include "trismedia/manager.h"
#include "trismedia/stringfields.h"
#include "trismedia/utils.h"
#include "trismedia/causes.h"
#include "trismedia/astobj.h"
#include "trismedia/abstract_jb.h"
#include "trismedia/jabber.h"
#include "trismedia/jingle.h"

#define JINGLE_CONFIG "jingle.conf"

/*! Global jitterbuffer configuration - by default, jb is disabled */
static struct tris_jb_conf default_jbconf =
{
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = "",
	.target_extra = -1,
};
static struct tris_jb_conf global_jbconf;

enum jingle_protocol {
	AJI_PROTOCOL_UDP,
	AJI_PROTOCOL_SSLTCP,
};

enum jingle_connect_type {
	AJI_CONNECT_HOST,
	AJI_CONNECT_PRFLX,
	AJI_CONNECT_RELAY,
	AJI_CONNECT_SRFLX,
};

struct jingle_pvt {
	tris_mutex_t lock;                /*!< Channel private lock */
	time_t laststun;
	struct jingle *parent;	         /*!< Parent client */
	char sid[100];
	char them[AJI_MAX_JIDLEN];
	char ring[10];                   /*!< Message ID of ring */
	iksrule *ringrule;               /*!< Rule for matching RING request */
	int initiator;                   /*!< If we're the initiator */
	int alreadygone;
	int capability;
	struct tris_codec_pref prefs;
	struct jingle_candidate *theircandidates;
	struct jingle_candidate *ourcandidates;
	char cid_num[80];                /*!< Caller ID num */
	char cid_name[80];               /*!< Caller ID name */
	char exten[80];                  /*!< Called extension */
	struct tris_channel *owner;       /*!< Master Channel */
	char audio_content_name[100];    /*!< name attribute of content tag */
	struct tris_rtp *rtp;             /*!< RTP audio session */
	char video_content_name[100];    /*!< name attribute of content tag */
	struct tris_rtp *vrtp;            /*!< RTP video session */
	int jointcapability;             /*!< Supported capability at both ends (codecs ) */
	int peercapability;
	struct jingle_pvt *next;	/* Next entity */
};

struct jingle_candidate {
	unsigned int component;          /*!< ex. : 1 for RTP, 2 for RTCP */
	unsigned int foundation;         /*!< Function of IP, protocol, type */
	unsigned int generation;
	char ip[16];
	unsigned int network;
	unsigned int port;
	unsigned int priority;
	enum jingle_protocol protocol;
	char password[100];
	enum jingle_connect_type type;
	char ufrag[100];
	unsigned int preference;
	struct jingle_candidate *next;
};

struct jingle {
	ASTOBJ_COMPONENTS(struct jingle);
	struct aji_client *connection;
	struct aji_buddy *buddy;
	struct jingle_pvt *p;
	struct tris_codec_pref prefs;
	int amaflags;			/*!< AMA Flags */
	char user[100];
	char context[100];
	char accountcode[TRIS_MAX_ACCOUNT_CODE];	/*!< Account code */
	int capability;
	tris_group_t callgroup;	/*!< Call group */
	tris_group_t pickupgroup;	/*!< Pickup group */
	int callingpres;		/*!< Calling presentation */
	int allowguest;
	char language[MAX_LANGUAGE];	/*!<  Default language for prompts */
	char musicclass[MAX_MUSICCLASS];	/*!<  Music on Hold class */
	char parkinglot[TRIS_MAX_CONTEXT];   /*!< Parkinglot */
};

struct jingle_container {
	ASTOBJ_CONTAINER_COMPONENTS(struct jingle);
};

static const char desc[] = "Jingle Channel";
static const char channel_type[] = "Jingle";

static int global_capability = TRIS_FORMAT_ULAW | TRIS_FORMAT_ALAW | TRIS_FORMAT_GSM | TRIS_FORMAT_H263;

TRIS_MUTEX_DEFINE_STATIC(jinglelock); /*!< Protect the interface list (of jingle_pvt's) */

/* Forward declarations */
static struct tris_channel *jingle_request(const char *type, int format, void *data, int *cause, struct tris_channel* src);
static int jingle_digit_begin(struct tris_channel *ast, char digit);
static int jingle_digit_end(struct tris_channel *ast, char digit, unsigned int duration);
static int jingle_call(struct tris_channel *ast, char *dest, int timeout);
static int jingle_hangup(struct tris_channel *ast);
static int jingle_answer(struct tris_channel *ast);
static int jingle_newcall(struct jingle *client, ikspak *pak);
static struct tris_frame *jingle_read(struct tris_channel *ast);
static int jingle_write(struct tris_channel *ast, struct tris_frame *f);
static int jingle_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen);
static int jingle_fixup(struct tris_channel *oldchan, struct tris_channel *newchan);
static int jingle_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen);
static struct jingle_pvt *jingle_alloc(struct jingle *client, const char *from, const char *sid);
static char *jingle_show_channels(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
static char *jingle_do_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
/*----- RTP interface functions */
static int jingle_set_rtp_peer(struct tris_channel *chan, struct tris_rtp *rtp,
							   struct tris_rtp *vrtp, struct tris_rtp *tpeer, int codecs, int nat_active);
static enum tris_rtp_get_result jingle_get_rtp_peer(struct tris_channel *chan, struct tris_rtp **rtp);
static int jingle_get_codec(struct tris_channel *chan);

/*! \brief PBX interface structure for channel registration */
static const struct tris_channel_tech jingle_tech = {
	.type = "Jingle",
	.description = "Jingle Channel Driver",
	.capabilities = TRIS_FORMAT_AUDIO_MASK,
	.requester = jingle_request,
	.send_digit_begin = jingle_digit_begin,
	.send_digit_end = jingle_digit_end,
	.bridge = tris_rtp_bridge,
	.call = jingle_call,
	.hangup = jingle_hangup,
	.answer = jingle_answer,
	.read = jingle_read,
	.write = jingle_write,
	.exception = jingle_read,
	.indicate = jingle_indicate,
	.fixup = jingle_fixup,
	.send_html = jingle_sendhtml,
	.properties = TRIS_CHAN_TP_WANTSJITTER | TRIS_CHAN_TP_CREATESJITTER
};

static struct sockaddr_in bindaddr = { 0, };	/*!< The address we bind to */

static struct sched_context *sched;	/*!< The scheduling context */
static struct io_context *io;	/*!< The IO context */
static struct in_addr __ourip;


/*! \brief RTP driver interface */
static struct tris_rtp_protocol jingle_rtp = {
	type: "Jingle",
	get_rtp_info: jingle_get_rtp_peer,
	set_rtp_peer: jingle_set_rtp_peer,
	get_codec: jingle_get_codec,
};

static struct tris_cli_entry jingle_cli[] = {
	TRIS_CLI_DEFINE(jingle_do_reload, "Reload Jingle configuration"),
	TRIS_CLI_DEFINE(jingle_show_channels, "Show Jingle channels"),
};


static char externip[16];

static struct jingle_container jingle_list;

static void jingle_member_destroy(struct jingle *obj)
{
	tris_free(obj);
}

static struct jingle *find_jingle(char *name, char *connection)
{
	struct jingle *jingle = NULL;

	jingle = ASTOBJ_CONTAINER_FIND(&jingle_list, name);
	if (!jingle && strchr(name, '@'))
		jingle = ASTOBJ_CONTAINER_FIND_FULL(&jingle_list, name, user,,, strcasecmp);

	if (!jingle) {				
		/* guest call */
		ASTOBJ_CONTAINER_TRAVERSE(&jingle_list, 1, {
			ASTOBJ_RDLOCK(iterator);
			if (!strcasecmp(iterator->name, "guest")) {
				jingle = iterator;
			}
			ASTOBJ_UNLOCK(iterator);

			if (jingle)
				break;
		});

	}
	return jingle;
}


static void add_codec_to_answer(const struct jingle_pvt *p, int codec, iks *dcodecs)
{
	char *format = tris_getformatname(codec);

	if (!strcasecmp("ulaw", format)) {
		iks *payload_eg711u, *payload_pcmu;
		payload_pcmu = iks_new("payload-type");
		iks_insert_attrib(payload_pcmu, "id", "0");
		iks_insert_attrib(payload_pcmu, "name", "PCMU");
		payload_eg711u = iks_new("payload-type");
		iks_insert_attrib(payload_eg711u, "id", "100");
		iks_insert_attrib(payload_eg711u, "name", "EG711U");
		iks_insert_node(dcodecs, payload_pcmu);
		iks_insert_node(dcodecs, payload_eg711u);
	}
	if (!strcasecmp("alaw", format)) {
		iks *payload_eg711a;
		iks *payload_pcma = iks_new("payload-type");
		iks_insert_attrib(payload_pcma, "id", "8");
		iks_insert_attrib(payload_pcma, "name", "PCMA");
		payload_eg711a = iks_new("payload-type");
		iks_insert_attrib(payload_eg711a, "id", "101");
		iks_insert_attrib(payload_eg711a, "name", "EG711A");
		iks_insert_node(dcodecs, payload_pcma);
		iks_insert_node(dcodecs, payload_eg711a);
	}
	if (!strcasecmp("ilbc", format)) {
		iks *payload_ilbc = iks_new("payload-type");
		iks_insert_attrib(payload_ilbc, "id", "97");
		iks_insert_attrib(payload_ilbc, "name", "iLBC");
		iks_insert_node(dcodecs, payload_ilbc);
	}
	if (!strcasecmp("g723", format)) {
		iks *payload_g723 = iks_new("payload-type");
		iks_insert_attrib(payload_g723, "id", "4");
		iks_insert_attrib(payload_g723, "name", "G723");
		iks_insert_node(dcodecs, payload_g723);
	}
	tris_rtp_lookup_code(p->rtp, 1, codec);
}

static int jingle_accept_call(struct jingle *client, struct jingle_pvt *p)
{
	struct jingle_pvt *tmp = client->p;
	struct aji_client *c = client->connection;
	iks *iq, *jingle, *dcodecs, *payload_red, *payload_audio, *payload_cn;
	int x;
	int pref_codec = 0;
	int alreadysent = 0;

	if (p->initiator)
		return 1;

	iq = iks_new("iq");
	jingle = iks_new(JINGLE_NODE);
	dcodecs = iks_new("description");
	if (iq && jingle && dcodecs) {
		iks_insert_attrib(dcodecs, "xmlns", JINGLE_AUDIO_RTP_NS);

		for (x = 0; x < 32; x++) {
			if (!(pref_codec = tris_codec_pref_index(&client->prefs, x)))
				break;
			if (!(client->capability & pref_codec))
				continue;
			if (alreadysent & pref_codec)
				continue;
			add_codec_to_answer(p, pref_codec, dcodecs);
			alreadysent |= pref_codec;
		}
		payload_red = iks_new("payload-type");
		iks_insert_attrib(payload_red, "id", "117");
		iks_insert_attrib(payload_red, "name", "red");
		payload_audio = iks_new("payload-type");
		iks_insert_attrib(payload_audio, "id", "106");
		iks_insert_attrib(payload_audio, "name", "audio/telephone-event");
		payload_cn = iks_new("payload-type");
		iks_insert_attrib(payload_cn, "id", "13");
		iks_insert_attrib(payload_cn, "name", "CN");


		iks_insert_attrib(iq, "type", "set");
		iks_insert_attrib(iq, "to", (p->them) ? p->them : client->user);
		iks_insert_attrib(iq, "id", client->connection->mid);
		tris_aji_increment_mid(client->connection->mid);

		iks_insert_attrib(jingle, "xmlns", JINGLE_NS);
		iks_insert_attrib(jingle, "action", JINGLE_ACCEPT);
		iks_insert_attrib(jingle, "initiator", p->initiator ? client->connection->jid->full : p->them);
		iks_insert_attrib(jingle, JINGLE_SID, tmp->sid);
		iks_insert_node(iq, jingle);
		iks_insert_node(jingle, dcodecs);
		iks_insert_node(dcodecs, payload_red);
		iks_insert_node(dcodecs, payload_audio);
		iks_insert_node(dcodecs, payload_cn);

		tris_aji_send(c, iq);

		iks_delete(payload_red);
		iks_delete(payload_audio);
		iks_delete(payload_cn);
		iks_delete(dcodecs);
		iks_delete(jingle);
		iks_delete(iq);
	}
	return 1;
}

static int jingle_ringing_ack(void *data, ikspak *pak)
{
	struct jingle_pvt *p = data;

	if (p->ringrule)
		iks_filter_remove_rule(p->parent->connection->f, p->ringrule);
	p->ringrule = NULL;
	if (p->owner)
		tris_queue_control(p->owner, TRIS_CONTROL_RINGING);
	return IKS_FILTER_EAT;
}

static int jingle_answer(struct tris_channel *ast)
{
	struct jingle_pvt *p = ast->tech_pvt;
	struct jingle *client = p->parent;
	int res = 0;

	tris_debug(1, "Answer!\n");
	tris_mutex_lock(&p->lock);
	jingle_accept_call(client, p);
	tris_mutex_unlock(&p->lock);
	return res;
}

static enum tris_rtp_get_result jingle_get_rtp_peer(struct tris_channel *chan, struct tris_rtp **rtp)
{
	struct jingle_pvt *p = chan->tech_pvt;
	enum tris_rtp_get_result res = TRIS_RTP_GET_FAILED;

	if (!p)
		return res;

	tris_mutex_lock(&p->lock);
	if (p->rtp) {
		*rtp = p->rtp;
		res = TRIS_RTP_TRY_PARTIAL;
	}
	tris_mutex_unlock(&p->lock);

	return res;
}

static int jingle_get_codec(struct tris_channel *chan)
{
	struct jingle_pvt *p = chan->tech_pvt;
	return p->peercapability;
}

static int jingle_set_rtp_peer(struct tris_channel *chan, struct tris_rtp *rtp, struct tris_rtp *vrtp, struct tris_rtp *tpeer, int codecs, int nat_active)
{
	struct jingle_pvt *p;

	p = chan->tech_pvt;
	if (!p)
		return -1;
	tris_mutex_lock(&p->lock);

/*	if (rtp)
		tris_rtp_get_peer(rtp, &p->redirip);
	else
		memset(&p->redirip, 0, sizeof(p->redirip));
	p->redircodecs = codecs; */

	/* Reset lastrtprx timer */
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int jingle_response(struct jingle *client, ikspak *pak, const char *reasonstr, const char *reasonstr2)
{
	iks *response = NULL, *error = NULL, *reason = NULL;
	int res = -1;

	response = iks_new("iq");
	if (response) {
		iks_insert_attrib(response, "type", "result");
		iks_insert_attrib(response, "from", client->connection->jid->full);
		iks_insert_attrib(response, "to", iks_find_attrib(pak->x, "from"));
		iks_insert_attrib(response, "id", iks_find_attrib(pak->x, "id"));
		if (reasonstr) {
			error = iks_new("error");
			if (error) {
				iks_insert_attrib(error, "type", "cancel");
				reason = iks_new(reasonstr);
				if (reason)
					iks_insert_node(error, reason);
				iks_insert_node(response, error);
			}
		}
		tris_aji_send(client->connection, response);
		res = 0;
	}
	
	iks_delete(reason);
	iks_delete(error);
	iks_delete(response);

	return res;
}

static int jingle_is_answered(struct jingle *client, ikspak *pak)
{
	struct jingle_pvt *tmp;

	tris_debug(1, "The client is %s\n", client->name);
	/* Make sure our new call doesn't exist yet */
	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, JINGLE_NODE, JINGLE_SID, tmp->sid))
			break;
	}

	if (tmp) {
		if (tmp->owner)
			tris_queue_control(tmp->owner, TRIS_CONTROL_ANSWER);
	} else
		tris_log(LOG_NOTICE, "Whoa, didn't find call!\n");
	jingle_response(client, pak, NULL, NULL);
	return 1;
}

static int jingle_handle_dtmf(struct jingle *client, ikspak *pak)
{
	struct jingle_pvt *tmp;
	iks *dtmfnode = NULL, *dtmfchild = NULL;
	char *dtmf;
	/* Make sure our new call doesn't exist yet */
	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, JINGLE_NODE, JINGLE_SID, tmp->sid))
			break;
	}

	if (tmp) {
		if(iks_find_with_attrib(pak->x, "dtmf-method", "method", "rtp")) {
			jingle_response(client,pak,
					"feature-not-implemented xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'",
					"unsupported-dtmf-method xmlns='http://www.xmpp.org/extensions/xep-0181.html#ns-errors'");
			return -1;
		}
		if ((dtmfnode = iks_find(pak->x, "dtmf"))) {
			if((dtmf = iks_find_attrib(dtmfnode, "code"))) {
				if(iks_find_with_attrib(pak->x, "dtmf", "action", "button-up")) {
					struct tris_frame f = {TRIS_FRAME_DTMF_BEGIN, };
					f.subclass = dtmf[0];
					tris_queue_frame(tmp->owner, &f);
					tris_verbose("JINGLE! DTMF-relay event received: %c\n", f.subclass);
				} else if(iks_find_with_attrib(pak->x, "dtmf", "action", "button-down")) {
					struct tris_frame f = {TRIS_FRAME_DTMF_END, };
					f.subclass = dtmf[0];
					tris_queue_frame(tmp->owner, &f);
					tris_verbose("JINGLE! DTMF-relay event received: %c\n", f.subclass);
				} else if(iks_find_attrib(pak->x, "dtmf")) { /* 250 millasecond default */
					struct tris_frame f = {TRIS_FRAME_DTMF, };
					f.subclass = dtmf[0];
					tris_queue_frame(tmp->owner, &f);
					tris_verbose("JINGLE! DTMF-relay event received: %c\n", f.subclass);
				}
			}
		} else if ((dtmfnode = iks_find_with_attrib(pak->x, JINGLE_NODE, "action", "session-info"))) {
			if((dtmfchild = iks_find(dtmfnode, "dtmf"))) {
				if((dtmf = iks_find_attrib(dtmfchild, "code"))) {
					if(iks_find_with_attrib(dtmfnode, "dtmf", "action", "button-up")) {
						struct tris_frame f = {TRIS_FRAME_DTMF_END, };
						f.subclass = dtmf[0];
						tris_queue_frame(tmp->owner, &f);
						tris_verbose("JINGLE! DTMF-relay event received: %c\n", f.subclass);
					} else if(iks_find_with_attrib(dtmfnode, "dtmf", "action", "button-down")) {
						struct tris_frame f = {TRIS_FRAME_DTMF_BEGIN, };
						f.subclass = dtmf[0];
						tris_queue_frame(tmp->owner, &f);
						tris_verbose("JINGLE! DTMF-relay event received: %c\n", f.subclass);
					}
				}
			}
		}
		jingle_response(client, pak, NULL, NULL);
		return 1;
	} else
		tris_log(LOG_NOTICE, "Whoa, didn't find call!\n");

	jingle_response(client, pak, NULL, NULL);
	return 1;
}


static int jingle_hangup_farend(struct jingle *client, ikspak *pak)
{
	struct jingle_pvt *tmp;

	tris_debug(1, "The client is %s\n", client->name);
	/* Make sure our new call doesn't exist yet */
	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, JINGLE_NODE, JINGLE_SID, tmp->sid))
			break;
	}

	if (tmp) {
		tmp->alreadygone = 1;
		if (tmp->owner)
			tris_queue_hangup(tmp->owner);
	} else
		tris_log(LOG_NOTICE, "Whoa, didn't find call!\n");
	jingle_response(client, pak, NULL, NULL);
	return 1;
}

static int jingle_create_candidates(struct jingle *client, struct jingle_pvt *p, char *sid, char *from)
{
	struct jingle_candidate *tmp;
	struct aji_client *c = client->connection;
	struct jingle_candidate *ours1 = NULL, *ours2 = NULL;
	struct sockaddr_in sin;
	struct sockaddr_in dest;
	struct in_addr us;
	struct in_addr externaddr;
	iks *iq, *jingle, *content, *transport, *candidate;
	char component[16], foundation[16], generation[16], network[16], pass[16], port[7], priority[16], user[16];


	iq = iks_new("iq");
	jingle = iks_new(JINGLE_NODE);
	content = iks_new("content");
	transport = iks_new("transport");
	candidate = iks_new("candidate");
	if (!iq || !jingle || !content || !transport || !candidate) {
		tris_log(LOG_ERROR, "Memory allocation error\n");
		goto safeout;
	}
	ours1 = tris_calloc(1, sizeof(*ours1));
	ours2 = tris_calloc(1, sizeof(*ours2));
	if (!ours1 || !ours2)
		goto safeout;

	iks_insert_node(iq, jingle);
	iks_insert_node(jingle, content);
	iks_insert_node(content, transport);
	iks_insert_node(transport, candidate);

	for (; p; p = p->next) {
		if (!strcasecmp(p->sid, sid))
			break;
	}

	if (!p) {
		tris_log(LOG_NOTICE, "No matching jingle session - SID %s!\n", sid);
		goto safeout;
	}

	tris_rtp_get_us(p->rtp, &sin);
	tris_find_ourip(&us, bindaddr);

	/* Setup our first jingle candidate */
	ours1->component = 1;
	ours1->foundation = (unsigned int)bindaddr.sin_addr.s_addr | AJI_CONNECT_HOST | AJI_PROTOCOL_UDP;
	ours1->generation = 0;
	tris_copy_string(ours1->ip, tris_inet_ntoa(us), sizeof(ours1->ip));
	ours1->network = 0;
	ours1->port = ntohs(sin.sin_port);
	ours1->priority = 1678246398;
	ours1->protocol = AJI_PROTOCOL_UDP;
	snprintf(pass, sizeof(pass), "%08lx%08lx", tris_random(), tris_random());
	tris_copy_string(ours1->password, pass, sizeof(ours1->password));
	ours1->type = AJI_CONNECT_HOST;
	snprintf(user, sizeof(user), "%08lx%08lx", tris_random(), tris_random());
	tris_copy_string(ours1->ufrag, user, sizeof(ours1->ufrag));
	p->ourcandidates = ours1;

	if (!tris_strlen_zero(externip)) {
		/* XXX We should really stun for this one not just go with externip XXX */
		if (inet_aton(externip, &externaddr))
			tris_log(LOG_WARNING, "Invalid extern IP : %s\n", externip);

		ours2->component = 1;
		ours2->foundation = (unsigned int)externaddr.s_addr | AJI_CONNECT_PRFLX | AJI_PROTOCOL_UDP;
		ours2->generation = 0;
		tris_copy_string(ours2->ip, externip, sizeof(ours2->ip));
		ours2->network = 0;
		ours2->port = ntohs(sin.sin_port);
		ours2->priority = 1678246397;
		ours2->protocol = AJI_PROTOCOL_UDP;
		snprintf(pass, sizeof(pass), "%08lx%08lx", tris_random(), tris_random());
		tris_copy_string(ours2->password, pass, sizeof(ours2->password));
		ours2->type = AJI_CONNECT_PRFLX;

		snprintf(user, sizeof(user), "%08lx%08lx", tris_random(), tris_random());
		tris_copy_string(ours2->ufrag, user, sizeof(ours2->ufrag));
		ours1->next = ours2;
		ours2 = NULL;
	}
	ours1 = NULL;
	dest.sin_addr = __ourip;
	dest.sin_port = sin.sin_port;


	for (tmp = p->ourcandidates; tmp; tmp = tmp->next) {
		snprintf(component, sizeof(component), "%u", tmp->component);
		snprintf(foundation, sizeof(foundation), "%u", tmp->foundation);
		snprintf(generation, sizeof(generation), "%u", tmp->generation);
		snprintf(network, sizeof(network), "%u", tmp->network);
		snprintf(port, sizeof(port), "%u", tmp->port);
		snprintf(priority, sizeof(priority), "%u", tmp->priority);

		iks_insert_attrib(iq, "from", c->jid->full);
		iks_insert_attrib(iq, "to", from);
		iks_insert_attrib(iq, "type", "set");
		iks_insert_attrib(iq, "id", c->mid);
		tris_aji_increment_mid(c->mid);
		iks_insert_attrib(jingle, "action", JINGLE_NEGOTIATE);
		iks_insert_attrib(jingle, JINGLE_SID, sid);
		iks_insert_attrib(jingle, "initiator", (p->initiator) ? c->jid->full : from);
		iks_insert_attrib(jingle, "xmlns", JINGLE_NS);
		iks_insert_attrib(content, "creator", p->initiator ? "initiator" : "responder");
		iks_insert_attrib(content, "name", "trismedia-audio-content");
		iks_insert_attrib(transport, "xmlns", JINGLE_ICE_UDP_NS);
		iks_insert_attrib(candidate, "component", component);
		iks_insert_attrib(candidate, "foundation", foundation);
		iks_insert_attrib(candidate, "generation", generation);
		iks_insert_attrib(candidate, "ip", tmp->ip);
		iks_insert_attrib(candidate, "network", network);
		iks_insert_attrib(candidate, "port", port);
		iks_insert_attrib(candidate, "priority", priority);
		switch (tmp->protocol) {
		case AJI_PROTOCOL_UDP:
			iks_insert_attrib(candidate, "protocol", "udp");
			break;
		case AJI_PROTOCOL_SSLTCP:
			iks_insert_attrib(candidate, "protocol", "ssltcp");
			break;
		}
		iks_insert_attrib(candidate, "pwd", tmp->password);
		switch (tmp->type) {
		case AJI_CONNECT_HOST:
			iks_insert_attrib(candidate, "type", "host");
			break;
		case AJI_CONNECT_PRFLX:
			iks_insert_attrib(candidate, "type", "prflx");
			break;
		case AJI_CONNECT_RELAY:
			iks_insert_attrib(candidate, "type", "relay");
			break;
		case AJI_CONNECT_SRFLX:
			iks_insert_attrib(candidate, "type", "srflx");
			break;
		}
		iks_insert_attrib(candidate, "ufrag", tmp->ufrag);

		tris_aji_send(c, iq);
	}
	p->laststun = 0;

safeout:
	if (ours1)
		tris_free(ours1);
	if (ours2)
		tris_free(ours2);
	iks_delete(iq);
	iks_delete(jingle);
	iks_delete(content);
	iks_delete(transport);
	iks_delete(candidate);

	return 1;
}

static struct jingle_pvt *jingle_alloc(struct jingle *client, const char *from, const char *sid)
{
	struct jingle_pvt *tmp = NULL;
	struct aji_resource *resources = NULL;
	struct aji_buddy *buddy;
	char idroster[200];

	tris_debug(1, "The client is %s for alloc\n", client->name);
	if (!sid && !strchr(from, '/')) {	/* I started call! */
		if (!strcasecmp(client->name, "guest")) {
			buddy = ASTOBJ_CONTAINER_FIND(&client->connection->buddies, from);
			if (buddy)
				resources = buddy->resources;
		} else if (client->buddy)
			resources = client->buddy->resources;
		while (resources) {
			if (resources->cap->jingle) {
				break;
			}
			resources = resources->next;
		}
		if (resources)
			snprintf(idroster, sizeof(idroster), "%s/%s", from, resources->resource);
		else {
			tris_log(LOG_ERROR, "no jingle capable clients to talk to.\n");
			return NULL;
		}
	}
	if (!(tmp = tris_calloc(1, sizeof(*tmp)))) {
		return NULL;
	}

	memcpy(&tmp->prefs, &client->prefs, sizeof(tmp->prefs));

	if (sid) {
		tris_copy_string(tmp->sid, sid, sizeof(tmp->sid));
		tris_copy_string(tmp->them, from, sizeof(tmp->them));
	} else {
		snprintf(tmp->sid, sizeof(tmp->sid), "%08lx%08lx", tris_random(), tris_random());
		tris_copy_string(tmp->them, idroster, sizeof(tmp->them));
		tmp->initiator = 1;
	}
	tmp->rtp = tris_rtp_new_with_bindaddr(sched, io, 1, 0, bindaddr.sin_addr);
	tmp->parent = client;
	if (!tmp->rtp) {
		tris_log(LOG_WARNING, "Out of RTP sessions?\n");
		tris_free(tmp);
		return NULL;
	}
	tris_copy_string(tmp->exten, "s", sizeof(tmp->exten));
	tris_mutex_init(&tmp->lock);
	tris_mutex_lock(&jinglelock);
	tmp->next = client->p;
	client->p = tmp;
	tris_mutex_unlock(&jinglelock);
	return tmp;
}

/*! \brief Start new jingle channel */
static struct tris_channel *jingle_new(struct jingle *client, struct jingle_pvt *i, int state, const char *title)
{
	struct tris_channel *tmp;
	int fmt;
	int what;
	const char *str;

	if (title)
		str = title;
	else
		str = i->them;
	tmp = tris_channel_alloc(1, state, i->cid_num, i->cid_name, "", "", "", 0, "Jingle/%s-%04lx", str, tris_random() & 0xffff);
	if (!tmp) {
		tris_log(LOG_WARNING, "Unable to allocate Jingle channel structure!\n");
		return NULL;
	}
	tmp->tech = &jingle_tech;

	/* Select our native format based on codec preference until we receive
	   something from another device to the contrary. */
	if (i->jointcapability)
		what = i->jointcapability;
	else if (i->capability)
		what = i->capability;
	else
		what = global_capability;

	/* Set Frame packetization */
	if (i->rtp)
		tris_rtp_codec_setpref(i->rtp, &i->prefs);

	tmp->nativeformats = tris_codec_choose(&i->prefs, what, 1) | (i->jointcapability & TRIS_FORMAT_VIDEO_MASK);
	fmt = tris_best_codec(tmp->nativeformats);

	if (i->rtp) {
		tris_channel_set_fd(tmp, 0, tris_rtp_fd(i->rtp));
		tris_channel_set_fd(tmp, 1, tris_rtcp_fd(i->rtp));
	}
	if (i->vrtp) {
		tris_channel_set_fd(tmp, 2, tris_rtp_fd(i->vrtp));
		tris_channel_set_fd(tmp, 3, tris_rtcp_fd(i->vrtp));
	}
	if (state == TRIS_STATE_RING)
		tmp->rings = 1;
	tmp->adsicpe = TRIS_ADSI_UNAVAILABLE;
	tmp->writeformat = fmt;
	tmp->rawwriteformat = fmt;
	tmp->readformat = fmt;
	tmp->rawreadformat = fmt;
	tmp->tech_pvt = i;

	tmp->callgroup = client->callgroup;
	tmp->pickupgroup = client->pickupgroup;
	tmp->cid.cid_pres = client->callingpres;
	if (!tris_strlen_zero(client->accountcode))
		tris_string_field_set(tmp, accountcode, client->accountcode);
	if (client->amaflags)
		tmp->amaflags = client->amaflags;
	if (!tris_strlen_zero(client->language))
		tris_string_field_set(tmp, language, client->language);
	if (!tris_strlen_zero(client->musicclass))
		tris_string_field_set(tmp, musicclass, client->musicclass);
	i->owner = tmp;
	tris_copy_string(tmp->context, client->context, sizeof(tmp->context));
	tris_copy_string(tmp->exten, i->exten, sizeof(tmp->exten));
	/* Don't use tris_set_callerid() here because it will
	 * generate an unnecessary NewCallerID event  */
	tmp->cid.cid_ani = tris_strdup(i->cid_num);
	if (!tris_strlen_zero(i->exten) && strcmp(i->exten, "s"))
		tmp->cid.cid_dnid = tris_strdup(i->exten);
	tmp->priority = 1;
	if (i->rtp)
		tris_jb_configure(tmp, &global_jbconf);
	if (state != TRIS_STATE_DOWN && tris_pbx_start(tmp)) {
		tris_log(LOG_WARNING, "Unable to start PBX on %s\n", tmp->name);
		tmp->hangupcause = TRIS_CAUSE_SWITCH_CONGESTION;
		tris_hangup(tmp);
		tmp = NULL;
	}

	return tmp;
}

static int jingle_action(struct jingle *client, struct jingle_pvt *p, const char *action)
{
	iks *iq, *jingle = NULL;
	int res = -1;

	iq = iks_new("iq");
	jingle = iks_new("jingle");
	
	if (iq) {
		iks_insert_attrib(iq, "type", "set");
		iks_insert_attrib(iq, "from", client->connection->jid->full);
		iks_insert_attrib(iq, "to", p->them);
		iks_insert_attrib(iq, "id", client->connection->mid);
		tris_aji_increment_mid(client->connection->mid);
		if (jingle) {
			iks_insert_attrib(jingle, "action", action);
			iks_insert_attrib(jingle, JINGLE_SID, p->sid);
			iks_insert_attrib(jingle, "initiator", p->initiator ? client->connection->jid->full : p->them);
			iks_insert_attrib(jingle, "xmlns", JINGLE_NS);

			iks_insert_node(iq, jingle);

			tris_aji_send(client->connection, iq);
			res = 0;
		}
	}
	
	iks_delete(jingle);
	iks_delete(iq);
	
	return res;
}

static void jingle_free_candidates(struct jingle_candidate *candidate)
{
	struct jingle_candidate *last;
	while (candidate) {
		last = candidate;
		candidate = candidate->next;
		tris_free(last);
	}
}

static void jingle_free_pvt(struct jingle *client, struct jingle_pvt *p)
{
	struct jingle_pvt *cur, *prev = NULL;
	cur = client->p;
	while (cur) {
		if (cur == p) {
			if (prev)
				prev->next = p->next;
			else
				client->p = p->next;
			break;
		}
		prev = cur;
		cur = cur->next;
	}
	if (p->ringrule)
		iks_filter_remove_rule(p->parent->connection->f, p->ringrule);
	if (p->owner)
		tris_log(LOG_WARNING, "Uh oh, there's an owner, this is going to be messy.\n");
	if (p->rtp)
		tris_rtp_destroy(p->rtp);
	if (p->vrtp)
		tris_rtp_destroy(p->vrtp);
	jingle_free_candidates(p->theircandidates);
	tris_free(p);
}


static int jingle_newcall(struct jingle *client, ikspak *pak)
{
	struct jingle_pvt *p, *tmp = client->p;
	struct tris_channel *chan;
	int res;
	iks *codec, *content, *description;
	char *from = NULL;

	/* Make sure our new call doesn't exist yet */
	from = iks_find_attrib(pak->x,"to");
	if(!from)
		from = client->connection->jid->full;

	while (tmp) {
		if (iks_find_with_attrib(pak->x, JINGLE_NODE, JINGLE_SID, tmp->sid)) {
			tris_log(LOG_NOTICE, "Ignoring duplicate call setup on SID %s\n", tmp->sid);
			jingle_response(client, pak, "out-of-order", NULL);
			return -1;
		}
		tmp = tmp->next;
	}

 	if (!strcasecmp(client->name, "guest")){
 		/* the guest account is not tied to any configured XMPP client,
 		   let's set it now */
 		client->connection = tris_aji_get_client(from);
 		if (!client->connection) {
 			tris_log(LOG_ERROR, "No XMPP client to talk to, us (partial JID) : %s\n", from);
 			return -1;
 		}
 	}

	p = jingle_alloc(client, pak->from->partial, iks_find_attrib(pak->query, JINGLE_SID));
	if (!p) {
		tris_log(LOG_WARNING, "Unable to allocate jingle structure!\n");
		return -1;
	}
	chan = jingle_new(client, p, TRIS_STATE_DOWN, pak->from->user);
	if (!chan) {
		jingle_free_pvt(client, p);
		return -1;
	}
	tris_mutex_lock(&p->lock);
	tris_copy_string(p->them, pak->from->full, sizeof(p->them));
	if (iks_find_attrib(pak->query, JINGLE_SID)) {
		tris_copy_string(p->sid, iks_find_attrib(pak->query, JINGLE_SID),
				sizeof(p->sid));
	}
	
	/* content points to the first <content/> tag */	
	content = iks_child(iks_child(pak->x));
	while (content) {
		description = iks_find_with_attrib(content, "description", "xmlns", JINGLE_AUDIO_RTP_NS);
		if (description) {
			/* audio content found */
			codec = iks_child(iks_child(content));
		        tris_copy_string(p->audio_content_name, iks_find_attrib(content, "name"), sizeof(p->audio_content_name));

			while (codec) {
				tris_rtp_set_m_type(p->rtp, atoi(iks_find_attrib(codec, "id")));
				tris_rtp_set_rtpmap_type(p->rtp, atoi(iks_find_attrib(codec, "id")), "audio", iks_find_attrib(codec, "name"), 0);
				codec = iks_next(codec);
			}
		}
		
		description = NULL;
		codec = NULL;

		description = iks_find_with_attrib(content, "description", "xmlns", JINGLE_VIDEO_RTP_NS);
		if (description) {
			/* video content found */
			codec = iks_child(iks_child(content));
		        tris_copy_string(p->video_content_name, iks_find_attrib(content, "name"), sizeof(p->video_content_name));

			while (codec) {
				tris_rtp_set_m_type(p->rtp, atoi(iks_find_attrib(codec, "id")));
				tris_rtp_set_rtpmap_type(p->rtp, atoi(iks_find_attrib(codec, "id")), "audio", iks_find_attrib(codec, "name"), 0);
				codec = iks_next(codec);
			}
		}
		
		content = iks_next(content);
	}

	tris_mutex_unlock(&p->lock);
	tris_setstate(chan, TRIS_STATE_RING);
	res = tris_pbx_start(chan);
	
	switch (res) {
	case TRIS_PBX_FAILED:
		tris_log(LOG_WARNING, "Failed to start PBX :(\n");
		jingle_response(client, pak, "service-unavailable", NULL);
		break;
	case TRIS_PBX_CALL_LIMIT:
		tris_log(LOG_WARNING, "Failed to start PBX (call limit reached) \n");
		jingle_response(client, pak, "service-unavailable", NULL);
		break;
	case TRIS_PBX_SUCCESS:
		jingle_response(client, pak, NULL, NULL);
		jingle_create_candidates(client, p,
					 iks_find_attrib(pak->query, JINGLE_SID),
					 iks_find_attrib(pak->x, "from"));
		/* nothing to do */
		break;
	}

	return 1;
}

static int jingle_update_stun(struct jingle *client, struct jingle_pvt *p)
{
	struct jingle_candidate *tmp;
	struct hostent *hp;
	struct tris_hostent ahp;
	struct sockaddr_in sin;

	if (time(NULL) == p->laststun)
		return 0;

	tmp = p->theircandidates;
	p->laststun = time(NULL);
	while (tmp) {
		char username[256];
		hp = tris_gethostbyname(tmp->ip, &ahp);
		sin.sin_family = AF_INET;
		memcpy(&sin.sin_addr, hp->h_addr, sizeof(sin.sin_addr));
		sin.sin_port = htons(tmp->port);
		snprintf(username, sizeof(username), "%s:%s", tmp->ufrag, p->ourcandidates->ufrag);

		tris_rtp_stun_request(p->rtp, &sin, username);
		tmp = tmp->next;
	}
	return 1;
}

static int jingle_add_candidate(struct jingle *client, ikspak *pak)
{
	struct jingle_pvt *p = NULL, *tmp = NULL;
	struct aji_client *c = client->connection;
	struct jingle_candidate *newcandidate = NULL;
	iks *traversenodes = NULL, *receipt = NULL;

	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, JINGLE_NODE, JINGLE_SID, tmp->sid)) {
			p = tmp;
			break;
		}
	}

	if (!p)
		return -1;

	traversenodes = pak->query;
	while(traversenodes) {
		if(!strcasecmp(iks_name(traversenodes), "jingle")) {
			traversenodes = iks_child(traversenodes);
			continue;
		}
		if(!strcasecmp(iks_name(traversenodes), "content")) {
			traversenodes = iks_child(traversenodes);
			continue;
		}
		if(!strcasecmp(iks_name(traversenodes), "transport")) {
			traversenodes = iks_child(traversenodes);
			continue;
		}

		if(!strcasecmp(iks_name(traversenodes), "candidate")) {
			newcandidate = tris_calloc(1, sizeof(*newcandidate));
			if (!newcandidate)
				return 0;
			tris_copy_string(newcandidate->ip, iks_find_attrib(traversenodes, "ip"), sizeof(newcandidate->ip));
			newcandidate->port = atoi(iks_find_attrib(traversenodes, "port"));
			tris_copy_string(newcandidate->password, iks_find_attrib(traversenodes, "pwd"), sizeof(newcandidate->password));
			if (!strcasecmp(iks_find_attrib(traversenodes, "protocol"), "udp"))
				newcandidate->protocol = AJI_PROTOCOL_UDP;
			else if (!strcasecmp(iks_find_attrib(traversenodes, "protocol"), "ssltcp"))
				newcandidate->protocol = AJI_PROTOCOL_SSLTCP;
			
			if (!strcasecmp(iks_find_attrib(traversenodes, "type"), "host"))
				newcandidate->type = AJI_CONNECT_HOST;
			else if (!strcasecmp(iks_find_attrib(traversenodes, "type"), "prflx"))
				newcandidate->type = AJI_CONNECT_PRFLX;
			else if (!strcasecmp(iks_find_attrib(traversenodes, "type"), "relay"))
				newcandidate->type = AJI_CONNECT_RELAY;
			else if (!strcasecmp(iks_find_attrib(traversenodes, "type"), "srflx"))
				newcandidate->type = AJI_CONNECT_SRFLX;

			newcandidate->network = atoi(iks_find_attrib(traversenodes, "network"));
			newcandidate->generation = atoi(iks_find_attrib(traversenodes, "generation"));
			newcandidate->next = NULL;
		
			newcandidate->next = p->theircandidates;
			p->theircandidates = newcandidate;
			p->laststun = 0;
			jingle_update_stun(p->parent, p);
			newcandidate = NULL;
		}
		traversenodes = iks_next(traversenodes);
	}
	
	receipt = iks_new("iq");
	iks_insert_attrib(receipt, "type", "result");
	iks_insert_attrib(receipt, "from", c->jid->full);
	iks_insert_attrib(receipt, "to", iks_find_attrib(pak->x, "from"));
	iks_insert_attrib(receipt, "id", iks_find_attrib(pak->x, "id"));
	tris_aji_send(c, receipt);

	iks_delete(receipt);

	return 1;
}

static struct tris_frame *jingle_rtp_read(struct tris_channel *ast, struct jingle_pvt *p)
{
	struct tris_frame *f;

	if (!p->rtp)
		return &tris_null_frame;
	f = tris_rtp_read(p->rtp);
	jingle_update_stun(p->parent, p);
	if (p->owner) {
		/* We already hold the channel lock */
		if (f->frametype == TRIS_FRAME_VOICE) {
			if (f->subclass != (p->owner->nativeformats & TRIS_FORMAT_AUDIO_MASK)) {
				tris_debug(1, "Oooh, format changed to %d\n", f->subclass);
				p->owner->nativeformats =
					(p->owner->nativeformats & TRIS_FORMAT_VIDEO_MASK) | f->subclass;
				tris_set_read_format(p->owner, p->owner->readformat);
				tris_set_write_format(p->owner, p->owner->writeformat);
			}
/*			if ((tris_test_flag(p, SIP_DTMF) == SIP_DTMF_INBAND) && p->vad) {
				f = tris_dsp_process(p->owner, p->vad, f);
				if (f && (f->frametype == TRIS_FRAME_DTMF))
					tris_debug(1, "* Detected inband DTMF '%c'\n", f->subclass);
		        } */
		}
	}
	return f;
}

static struct tris_frame *jingle_read(struct tris_channel *ast)
{
	struct tris_frame *fr;
	struct jingle_pvt *p = ast->tech_pvt;

	tris_mutex_lock(&p->lock);
	fr = jingle_rtp_read(ast, p);
	tris_mutex_unlock(&p->lock);
	return fr;
}

/*! \brief Send frame to media channel (rtp) */
static int jingle_write(struct tris_channel *ast, struct tris_frame *frame)
{
	struct jingle_pvt *p = ast->tech_pvt;
	int res = 0;

	switch (frame->frametype) {
	case TRIS_FRAME_VOICE:
		if (!(frame->subclass & ast->nativeformats)) {
			tris_log(LOG_WARNING,
					"Asked to transmit frame type %d, while native formats is %d (read/write = %d/%d)\n",
					frame->subclass, ast->nativeformats, ast->readformat,
					ast->writeformat);
			return 0;
		}
		if (p) {
			tris_mutex_lock(&p->lock);
			if (p->rtp) {
				res = tris_rtp_write(p->rtp, frame);
			}
			tris_mutex_unlock(&p->lock);
		}
		break;
	case TRIS_FRAME_VIDEO:
		if (p) {
			tris_mutex_lock(&p->lock);
			if (p->vrtp) {
				res = tris_rtp_write(p->vrtp, frame);
			}
			tris_mutex_unlock(&p->lock);
		}
		break;
	case TRIS_FRAME_IMAGE:
		return 0;
		break;
	default:
		tris_log(LOG_WARNING, "Can't send %d type frames with Jingle write\n",
				frame->frametype);
		return 0;
	}

	return res;
}

static int jingle_fixup(struct tris_channel *oldchan, struct tris_channel *newchan)
{
	struct jingle_pvt *p = newchan->tech_pvt;
	tris_mutex_lock(&p->lock);

	if ((p->owner != oldchan)) {
		tris_mutex_unlock(&p->lock);
		return -1;
	}
	if (p->owner == oldchan)
		p->owner = newchan;
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int jingle_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen)
{
	int res = 0;

	switch (condition) {
	case TRIS_CONTROL_HOLD:
		tris_moh_start(ast, data, NULL);
		break;
	case TRIS_CONTROL_UNHOLD:
		tris_moh_stop(ast);
		break;
	default:
		tris_log(LOG_NOTICE, "Don't know how to indicate condition '%d'\n", condition);
		res = -1;
	}

	return res;
}

static int jingle_digit(struct tris_channel *ast, char digit, unsigned int duration)
{
	struct jingle_pvt *p = ast->tech_pvt;
	struct jingle *client = p->parent;
	iks *iq, *jingle, *dtmf;
	char buffer[2] = {digit, '\0'};
	iq = iks_new("iq");
	jingle = iks_new("jingle");
	dtmf = iks_new("dtmf");
	if(!iq || !jingle || !dtmf) {
		iks_delete(iq);
		iks_delete(jingle);
		iks_delete(dtmf);
		tris_log(LOG_ERROR, "Did not send dtmf do to memory issue\n");
		return -1;
	}

	iks_insert_attrib(iq, "type", "set");
	iks_insert_attrib(iq, "to", p->them);
	iks_insert_attrib(iq, "from", client->connection->jid->full);
	iks_insert_attrib(iq, "id", client->connection->mid);
	tris_aji_increment_mid(client->connection->mid);
	iks_insert_attrib(jingle, "xmlns", JINGLE_NS);
	iks_insert_attrib(jingle, "action", "session-info");
	iks_insert_attrib(jingle, "initiator", p->initiator ? client->connection->jid->full : p->them);
	iks_insert_attrib(jingle, "sid", p->sid);
	iks_insert_attrib(dtmf, "xmlns", JINGLE_DTMF_NS);
	iks_insert_attrib(dtmf, "code", buffer);
	iks_insert_node(iq, jingle);
	iks_insert_node(jingle, dtmf);

	tris_mutex_lock(&p->lock);
	if (ast->dtmff.frametype == TRIS_FRAME_DTMF_BEGIN || duration == 0) {
		iks_insert_attrib(dtmf, "action", "button-down");
	} else if (ast->dtmff.frametype == TRIS_FRAME_DTMF_END || duration != 0) {
		iks_insert_attrib(dtmf, "action", "button-up");
	}
	tris_aji_send(client->connection, iq);

	iks_delete(iq);
	iks_delete(jingle);
	iks_delete(dtmf);
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int jingle_digit_begin(struct tris_channel *chan, char digit)
{
	return jingle_digit(chan, digit, 0);
}

static int jingle_digit_end(struct tris_channel *ast, char digit, unsigned int duration)
{
	return jingle_digit(ast, digit, duration);
}

static int jingle_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen)
{
	tris_log(LOG_NOTICE, "XXX Implement jingle sendhtml XXX\n");

	return -1;
}
static int jingle_transmit_invite(struct jingle_pvt *p)
{
	struct jingle *aux = NULL;
	struct aji_client *client = NULL;
	iks *iq, *jingle, *content, *description, *transport;
	iks *payload_eg711u, *payload_pcmu;

	aux = p->parent;
	client = aux->connection;
	iq = iks_new("iq");
	jingle = iks_new(JINGLE_NODE);
	content = iks_new("content");
	description = iks_new("description");
	transport = iks_new("transport");
	payload_pcmu = iks_new("payload-type");
	payload_eg711u = iks_new("payload-type");

	tris_copy_string(p->audio_content_name, "trismedia-audio-content", sizeof(p->audio_content_name));

	iks_insert_attrib(iq, "type", "set");
	iks_insert_attrib(iq, "to", p->them);
	iks_insert_attrib(iq, "from", client->jid->full);
	iks_insert_attrib(iq, "id", client->mid);
	tris_aji_increment_mid(client->mid);
	iks_insert_attrib(jingle, "action", JINGLE_INITIATE);
	iks_insert_attrib(jingle, JINGLE_SID, p->sid);
	iks_insert_attrib(jingle, "initiator", client->jid->full);
	iks_insert_attrib(jingle, "xmlns", JINGLE_NS);

	/* For now, we only send one audio based content */
	iks_insert_attrib(content, "creator", "initiator");
	iks_insert_attrib(content, "name", p->audio_content_name);
	iks_insert_attrib(content, "profile", "RTP/AVP");
	iks_insert_attrib(description, "xmlns", JINGLE_AUDIO_RTP_NS);
	iks_insert_attrib(transport, "xmlns", JINGLE_ICE_UDP_NS);
	iks_insert_attrib(payload_pcmu, "id", "0");
	iks_insert_attrib(payload_pcmu, "name", "PCMU");
	iks_insert_attrib(payload_eg711u, "id", "100");
	iks_insert_attrib(payload_eg711u, "name", "EG711U");
	iks_insert_node(description, payload_pcmu);
	iks_insert_node(description, payload_eg711u);
	iks_insert_node(content, description);
	iks_insert_node(content, transport);
	iks_insert_node(jingle, content);
	iks_insert_node(iq, jingle);

	tris_aji_send(client, iq);

	iks_delete(iq);
	iks_delete(jingle);
	iks_delete(content);
	iks_delete(description);
	iks_delete(transport);
	iks_delete(payload_eg711u);
	iks_delete(payload_pcmu);
	return 0;
}

/* Not in use right now.
static int jingle_auto_congest(void *nothing)
{
	struct jingle_pvt *p = nothing;

	tris_mutex_lock(&p->lock);
	if (p->owner) {
		if (!tris_channel_trylock(p->owner)) {
			tris_log(LOG_NOTICE, "Auto-congesting %s\n", p->owner->name);
			tris_queue_control(p->owner, TRIS_CONTROL_CONGESTION);
			tris_channel_unlock(p->owner);
		}
	}
	tris_mutex_unlock(&p->lock);
	return 0;
}
*/

/*! \brief Initiate new call, part of PBX interface 
 * 	dest is the dial string */
static int jingle_call(struct tris_channel *ast, char *dest, int timeout)
{
	struct jingle_pvt *p = ast->tech_pvt;

	if ((ast->_state != TRIS_STATE_DOWN) && (ast->_state != TRIS_STATE_RESERVED)) {
		tris_log(LOG_WARNING, "jingle_call called on %s, neither down nor reserved\n", ast->name);
		return -1;
	}

	tris_setstate(ast, TRIS_STATE_RING);
	p->jointcapability = p->capability;
	if (!p->ringrule) {
		tris_copy_string(p->ring, p->parent->connection->mid, sizeof(p->ring));
		p->ringrule = iks_filter_add_rule(p->parent->connection->f, jingle_ringing_ack, p,
							IKS_RULE_ID, p->ring, IKS_RULE_DONE);
	} else
		tris_log(LOG_WARNING, "Whoa, already have a ring rule!\n");

	jingle_transmit_invite(p);
	jingle_create_candidates(p->parent, p, p->sid, p->them);

	return 0;
}

/*! \brief Hangup a call through the jingle proxy channel */
static int jingle_hangup(struct tris_channel *ast)
{
	struct jingle_pvt *p = ast->tech_pvt;
	struct jingle *client;

	tris_mutex_lock(&p->lock);
	client = p->parent;
	p->owner = NULL;
	ast->tech_pvt = NULL;
	if (!p->alreadygone)
		jingle_action(client, p, JINGLE_TERMINATE);
	tris_mutex_unlock(&p->lock);

	jingle_free_pvt(client, p);

	return 0;
}

/*! \brief Part of PBX interface */
static struct tris_channel *jingle_request(const char *request_type, int format, void *data, int *cause, struct tris_channel* src)
{
	struct jingle_pvt *p = NULL;
	struct jingle *client = NULL;
	char *sender = NULL, *to = NULL, *s = NULL;
	struct tris_channel *chan = NULL;

	if (data) {
		s = tris_strdupa(data);
		if (s) {
			sender = strsep(&s, "/");
			if (sender && (sender[0] != '\0'))
				to = strsep(&s, "/");
			if (!to) {
				tris_log(LOG_ERROR, "Bad arguments in Jingle Dialstring: %s\n", (char*) data);
				return NULL;
			}
		}
	}

	client = find_jingle(to, sender);
	if (!client) {
		tris_log(LOG_WARNING, "Could not find recipient.\n");
		return NULL;
	}
	if (!strcasecmp(client->name, "guest")){
		/* the guest account is not tied to any configured XMPP client,
		   let's set it now */
		client->connection = tris_aji_get_client(sender);
		if (!client->connection) {
			tris_log(LOG_ERROR, "No XMPP client to talk to, us (partial JID) : %s\n", sender);
			return NULL;
		}
	}
       
	ASTOBJ_WRLOCK(client);
	p = jingle_alloc(client, to, NULL);
	if (p)
		chan = jingle_new(client, p, TRIS_STATE_DOWN, to);
	ASTOBJ_UNLOCK(client);

	return chan;
}

/*! \brief CLI command "jingle show channels" */
static char *jingle_show_channels(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMAT  "%-30.30s  %-30.30s  %-15.15s  %-5.5s %-5.5s \n"
	struct jingle_pvt *p;
	struct tris_channel *chan;
	int numchans = 0;
	char them[AJI_MAX_JIDLEN];
	char *jid = NULL;
	char *resource = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "jingle show channels";
		e->usage =
			"Usage: jingle show channels\n"
			"       Shows current state of the Jingle channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	tris_mutex_lock(&jinglelock);
	tris_cli(a->fd, FORMAT, "Channel", "Jabber ID", "Resource", "Read", "Write");
	ASTOBJ_CONTAINER_TRAVERSE(&jingle_list, 1, {
		ASTOBJ_WRLOCK(iterator);
		p = iterator->p;
		while(p) {
			chan = p->owner;
			tris_copy_string(them, p->them, sizeof(them));
			jid = them;
			resource = strchr(them, '/');
			if (!resource)
				resource = "None";
			else {
				*resource = '\0';
				resource ++;
			}
			if (chan)
				tris_cli(a->fd, FORMAT, 
					chan->name,
					jid,
					resource,
					tris_getformatname(chan->readformat),
					tris_getformatname(chan->writeformat)					
					);
			else 
				tris_log(LOG_WARNING, "No available channel\n");
			numchans ++;
			p = p->next;
		}
		ASTOBJ_UNLOCK(iterator);
	});

	tris_mutex_unlock(&jinglelock);

	tris_cli(a->fd, "%d active jingle channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return CLI_SUCCESS;
#undef FORMAT
}

/*! \brief CLI command "jingle reload" */
static char *jingle_do_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "jingle reload";
		e->usage =
			"Usage: jingle reload\n"
			"       Reload jingle channel driver.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}	
	
	return CLI_SUCCESS;
}

static int jingle_parser(void *data, ikspak *pak)
{
	struct jingle *client = ASTOBJ_REF((struct jingle *) data);
	tris_log(LOG_NOTICE, "Filter matched\n");

	if (iks_find_with_attrib(pak->x, JINGLE_NODE, "action", JINGLE_INITIATE)) {
		/* New call */
		jingle_newcall(client, pak);
	} else if (iks_find_with_attrib(pak->x, JINGLE_NODE, "action", JINGLE_NEGOTIATE)) {
		tris_debug(3, "About to add candidate!\n");
		jingle_add_candidate(client, pak);
		tris_debug(3, "Candidate Added!\n");
	} else if (iks_find_with_attrib(pak->x, JINGLE_NODE, "action", JINGLE_ACCEPT)) {
		jingle_is_answered(client, pak);
	} else if (iks_find_with_attrib(pak->x, JINGLE_NODE, "action", JINGLE_INFO)) {
		jingle_handle_dtmf(client, pak);
	} else if (iks_find_with_attrib(pak->x, JINGLE_NODE, "action", JINGLE_TERMINATE)) {
		jingle_hangup_farend(client, pak);
	} else if (iks_find_with_attrib(pak->x, JINGLE_NODE, "action", "reject")) {
		jingle_hangup_farend(client, pak);
	}
	ASTOBJ_UNREF(client, jingle_member_destroy);
	return IKS_FILTER_EAT;
}
/* Not using this anymore probably take out soon 
static struct jingle_candidate *jingle_create_candidate(char *args)
{
	char *name, *type, *preference, *protocol;
	struct jingle_candidate *res;
	res = tris_calloc(1, sizeof(*res));
	if (args)
		name = args;
	if ((args = strchr(args, ','))) {
		*args = '\0';
		args++;
		preference = args;
	}
	if ((args = strchr(args, ','))) {
		*args = '\0';
		args++;
		protocol = args;
	}
	if ((args = strchr(args, ','))) {
		*args = '\0';
		args++;
		type = args;
	}
	if (name)
		tris_copy_string(res->name, name, sizeof(res->name));
	if (preference) {
		res->preference = atof(preference);
	}
	if (protocol) {
		if (!strcasecmp("udp", protocol))
			res->protocol = AJI_PROTOCOL_UDP;
		if (!strcasecmp("ssltcp", protocol))
			res->protocol = AJI_PROTOCOL_SSLTCP;
	}
	if (type) {
		if (!strcasecmp("host", type))
			res->type = AJI_CONNECT_HOST;
		if (!strcasecmp("prflx", type))
			res->type = AJI_CONNECT_PRFLX;
		if (!strcasecmp("relay", type))
			res->type = AJI_CONNECT_RELAY;
		if (!strcasecmp("srflx", type))
			res->type = AJI_CONNECT_SRFLX;
	}

	return res;
}
*/

static int jingle_create_member(char *label, struct tris_variable *var, int allowguest,
								struct tris_codec_pref prefs, char *context,
								struct jingle *member)
{
	struct aji_client *client;

	if (!member)
		tris_log(LOG_WARNING, "Out of memory.\n");

	tris_copy_string(member->name, label, sizeof(member->name));
	tris_copy_string(member->user, label, sizeof(member->user));
	tris_copy_string(member->context, context, sizeof(member->context));
	member->allowguest = allowguest;
	member->prefs = prefs;
	while (var) {
#if 0
		struct jingle_candidate *candidate = NULL;
#endif
		if (!strcasecmp(var->name, "username"))
			tris_copy_string(member->user, var->value, sizeof(member->user));
		else if (!strcasecmp(var->name, "disallow"))
			tris_parse_allow_disallow(&member->prefs, &member->capability, var->value, 0);
		else if (!strcasecmp(var->name, "allow"))
			tris_parse_allow_disallow(&member->prefs, &member->capability, var->value, 1);
		else if (!strcasecmp(var->name, "context"))
			tris_copy_string(member->context, var->value, sizeof(member->context));
#if 0
		else if (!strcasecmp(var->name, "candidate")) {
			candidate = jingle_create_candidate(var->value);
			if (candidate) {
				candidate->next = member->ourcandidates;
				member->ourcandidates = candidate;
			}
		}
#endif
		else if (!strcasecmp(var->name, "connection")) {
			if ((client = tris_aji_get_client(var->value))) {
				member->connection = client;
				iks_filter_add_rule(client->f, jingle_parser, member,
						    IKS_RULE_TYPE, IKS_PAK_IQ,
						    IKS_RULE_FROM_PARTIAL, member->user,
						    IKS_RULE_NS, JINGLE_NS,
						    IKS_RULE_DONE);
			} else {
				tris_log(LOG_ERROR, "connection referenced not found!\n");
				return 0;
			}
		}
		var = var->next;
	}
	if (member->connection && member->user)
		member->buddy = ASTOBJ_CONTAINER_FIND(&member->connection->buddies, member->user);
	else {
		tris_log(LOG_ERROR, "No Connection or Username!\n");
	}
	return 1;
}

static int jingle_load_config(void)
{
	char *cat = NULL;
	struct tris_config *cfg = NULL;
	char context[100];
	int allowguest = 1;
	struct tris_variable *var;
	struct jingle *member;
	struct hostent *hp;
	struct tris_hostent ahp;
	struct tris_codec_pref prefs;
	struct aji_client_container *clients;
	struct jingle_candidate *global_candidates = NULL;
	struct tris_flags config_flags = { 0 };

	cfg = tris_config_load(JINGLE_CONFIG, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		return 0;
	}

	/* Copy the default jb config over global_jbconf */
	memcpy(&global_jbconf, &default_jbconf, sizeof(struct tris_jb_conf));

	cat = tris_category_browse(cfg, NULL);
	for (var = tris_variable_browse(cfg, "general"); var; var = var->next) {
		/* handle jb conf */
		if (!tris_jb_read_conf(&global_jbconf, var->name, var->value))
			continue;

		if (!strcasecmp(var->name, "allowguest"))
			allowguest =
				(tris_true(tris_variable_retrieve(cfg, "general", "allowguest"))) ? 1 : 0;
		else if (!strcasecmp(var->name, "disallow"))
			tris_parse_allow_disallow(&prefs, &global_capability, var->value, 0);
		else if (!strcasecmp(var->name, "allow"))
			tris_parse_allow_disallow(&prefs, &global_capability, var->value, 1);
		else if (!strcasecmp(var->name, "context"))
			tris_copy_string(context, var->value, sizeof(context));
		else if (!strcasecmp(var->name, "externip"))
			tris_copy_string(externip, var->value, sizeof(externip));
		else if (!strcasecmp(var->name, "bindaddr")) {
			if (!(hp = tris_gethostbyname(var->value, &ahp))) {
				tris_log(LOG_WARNING, "Invalid address: %s\n", var->value);
			} else {
				memcpy(&bindaddr.sin_addr, hp->h_addr, sizeof(bindaddr.sin_addr));
			}
		}
/*  Idea to allow for custom candidates  */
/*
		else if (!strcasecmp(var->name, "candidate")) {
			candidate = jingle_create_candidate(var->value);
			if (candidate) {
				candidate->next = global_candidates;
				global_candidates = candidate;
			}
		}
*/
	}
	while (cat) {
		if (strcasecmp(cat, "general")) {
			var = tris_variable_browse(cfg, cat);
			member = tris_calloc(1, sizeof(*member));
			ASTOBJ_INIT(member);
			ASTOBJ_WRLOCK(member);
			if (!strcasecmp(cat, "guest")) {
				tris_copy_string(member->name, "guest", sizeof(member->name));
				tris_copy_string(member->user, "guest", sizeof(member->user));
				tris_copy_string(member->context, context, sizeof(member->context));
				member->allowguest = allowguest;
				member->prefs = prefs;
				while (var) {
					if (!strcasecmp(var->name, "disallow"))
						tris_parse_allow_disallow(&member->prefs, &member->capability,
												 var->value, 0);
					else if (!strcasecmp(var->name, "allow"))
						tris_parse_allow_disallow(&member->prefs, &member->capability,
												 var->value, 1);
					else if (!strcasecmp(var->name, "context"))
						tris_copy_string(member->context, var->value,
										sizeof(member->context));
					else if (!strcasecmp(var->name, "parkinglot"))
						tris_copy_string(member->parkinglot, var->value,
										sizeof(member->parkinglot));
/*  Idea to allow for custom candidates  */
/*
					else if (!strcasecmp(var->name, "candidate")) {
						candidate = jingle_create_candidate(var->value);
						if (candidate) {
							candidate->next = member->ourcandidates;
							member->ourcandidates = candidate;
						}
					}
*/
					var = var->next;
				}
				ASTOBJ_UNLOCK(member);
				clients = tris_aji_get_clients();
				if (clients) {
					ASTOBJ_CONTAINER_TRAVERSE(clients, 1, {
						ASTOBJ_WRLOCK(iterator);
						ASTOBJ_WRLOCK(member);
						member->connection = NULL;
						iks_filter_add_rule(iterator->f, jingle_parser, member, IKS_RULE_TYPE, IKS_PAK_IQ, IKS_RULE_NS,	JINGLE_NS, IKS_RULE_DONE);
						iks_filter_add_rule(iterator->f, jingle_parser, member, IKS_RULE_TYPE, IKS_PAK_IQ, IKS_RULE_NS,	JINGLE_DTMF_NS, IKS_RULE_DONE);
						ASTOBJ_UNLOCK(member);
						ASTOBJ_UNLOCK(iterator);
					});
					ASTOBJ_CONTAINER_LINK(&jingle_list, member);
				} else {
					ASTOBJ_UNLOCK(member);
					ASTOBJ_UNREF(member, jingle_member_destroy);
				}
			} else {
				ASTOBJ_UNLOCK(member);
				if (jingle_create_member(cat, var, allowguest, prefs, context, member))
					ASTOBJ_CONTAINER_LINK(&jingle_list, member);
				ASTOBJ_UNREF(member, jingle_member_destroy);
			}
		}
		cat = tris_category_browse(cfg, cat);
	}
	jingle_free_candidates(global_candidates);
	return 1;
}

/*! \brief Load module into PBX, register channel */
static int load_module(void)
{
	char *jabber_loaded = tris_module_helper("", "res_jabber.so", 0, 0, 0, 0);
	free(jabber_loaded);
	if (!jabber_loaded) {
		/* Dependency module has a different name, if embedded */
		jabber_loaded = tris_module_helper("", "res_jabber", 0, 0, 0, 0);
		free(jabber_loaded);
		if (!jabber_loaded) {
			tris_log(LOG_ERROR, "chan_jingle.so depends upon res_jabber.so\n");
			return TRIS_MODULE_LOAD_DECLINE;
		}
	}

	ASTOBJ_CONTAINER_INIT(&jingle_list);
	if (!jingle_load_config()) {
		tris_log(LOG_ERROR, "Unable to read config file %s. Not loading module.\n", JINGLE_CONFIG);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	sched = sched_context_create();
	if (!sched) 
		tris_log(LOG_WARNING, "Unable to create schedule context\n");

	io = io_context_create();
	if (!io) 
		tris_log(LOG_WARNING, "Unable to create I/O context\n");

	if (tris_find_ourip(&__ourip, bindaddr)) {
		tris_log(LOG_WARNING, "Unable to get own IP address, Jingle disabled\n");
		return 0;
	}

	tris_rtp_proto_register(&jingle_rtp);
	tris_cli_register_multiple(jingle_cli, ARRAY_LEN(jingle_cli));
	/* Make sure we can register our channel type */
	if (tris_channel_register(&jingle_tech)) {
		tris_log(LOG_ERROR, "Unable to register channel class %s\n", channel_type);
		return -1;
	}
	return 0;
}

/*! \brief Reload module */
static int reload(void)
{
	return 0;
}

/*! \brief Unload the jingle channel from Trismedia */
static int unload_module(void)
{
	struct jingle_pvt *privates = NULL;
	tris_cli_unregister_multiple(jingle_cli, ARRAY_LEN(jingle_cli));
	/* First, take us out of the channel loop */
	tris_channel_unregister(&jingle_tech);
	tris_rtp_proto_unregister(&jingle_rtp);

	if (!tris_mutex_lock(&jinglelock)) {
		/* Hangup all interfaces if they have an owner */
		ASTOBJ_CONTAINER_TRAVERSE(&jingle_list, 1, {
			ASTOBJ_WRLOCK(iterator);
			privates = iterator->p;
			while(privates) {
				if (privates->owner)
					tris_softhangup(privates->owner, TRIS_SOFTHANGUP_APPUNLOAD);
				privates = privates->next;
			}
			iterator->p = NULL;
			ASTOBJ_UNLOCK(iterator);
		});
		tris_mutex_unlock(&jinglelock);
	} else {
		tris_log(LOG_WARNING, "Unable to lock the monitor\n");
		return -1;
	}
	ASTOBJ_CONTAINER_DESTROYALL(&jingle_list, jingle_member_destroy);
	ASTOBJ_CONTAINER_DESTROY(&jingle_list);
	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Jingle Channel Driver",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
