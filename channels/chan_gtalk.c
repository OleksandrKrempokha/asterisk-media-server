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
 * \brief Gtalk Channel Driver, until google/libjingle works with jingle spec
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
#include <ctype.h>

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

#define GOOGLE_CONFIG "gtalk.conf"

#define GOOGLE_NS "http://www.google.com/session"


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

enum gtalk_protocol {
	AJI_PROTOCOL_UDP = 1,
	AJI_PROTOCOL_SSLTCP = 2,
};

enum gtalk_connect_type {
	AJI_CONNECT_STUN = 1,
	AJI_CONNECT_LOCAL = 2,
	AJI_CONNECT_RELAY = 3,
};

struct gtalk_pvt {
	tris_mutex_t lock;                /*!< Channel private lock */
	time_t laststun;
	struct gtalk *parent;	         /*!< Parent client */
	char sid[100];
	char us[AJI_MAX_JIDLEN];
	char them[AJI_MAX_JIDLEN];
	char ring[10];                   /*!< Message ID of ring */
	iksrule *ringrule;               /*!< Rule for matching RING request */
	int initiator;                   /*!< If we're the initiator */
	int alreadygone;
	int capability;
	struct tris_codec_pref prefs;
	struct gtalk_candidate *theircandidates;
	struct gtalk_candidate *ourcandidates;
	char cid_num[80];                /*!< Caller ID num */
	char cid_name[80];               /*!< Caller ID name */
	char exten[80];                  /*!< Called extension */
	struct tris_channel *owner;       /*!< Master Channel */
	struct tris_rtp *rtp;             /*!< RTP audio session */
	struct tris_rtp *vrtp;            /*!< RTP video session */
	int jointcapability;             /*!< Supported capability at both ends (codecs ) */
	int peercapability;
	struct gtalk_pvt *next;	/* Next entity */
};

struct gtalk_candidate {
	char name[100];
	enum gtalk_protocol protocol;
	double preference;
	char username[100];
	char password[100];
	enum gtalk_connect_type type;
	char network[6];
	int generation;
	char ip[16];
	int port;
	int receipt;
	struct gtalk_candidate *next;
};

struct gtalk {
	ASTOBJ_COMPONENTS(struct gtalk);
	struct aji_client *connection;
	struct aji_buddy *buddy;
	struct gtalk_pvt *p;
	struct tris_codec_pref prefs;
	int amaflags;			/*!< AMA Flags */
	char user[AJI_MAX_JIDLEN];
	char context[TRIS_MAX_CONTEXT];
	char parkinglot[TRIS_MAX_CONTEXT];	/*!<  Parkinglot */
	char accountcode[TRIS_MAX_ACCOUNT_CODE];	/*!< Account code */
	int capability;
	tris_group_t callgroup;	/*!< Call group */
	tris_group_t pickupgroup;	/*!< Pickup group */
	int callingpres;		/*!< Calling presentation */
	int allowguest;
	char language[MAX_LANGUAGE];	/*!<  Default language for prompts */
	char musicclass[MAX_MUSICCLASS];	/*!<  Music on Hold class */
};

struct gtalk_container {
	ASTOBJ_CONTAINER_COMPONENTS(struct gtalk);
};

static const char desc[] = "Gtalk Channel";

static int global_capability = TRIS_FORMAT_ULAW | TRIS_FORMAT_ALAW | TRIS_FORMAT_GSM | TRIS_FORMAT_H263;

TRIS_MUTEX_DEFINE_STATIC(gtalklock); /*!< Protect the interface list (of gtalk_pvt's) */

/* Forward declarations */
static struct tris_channel *gtalk_request(const char *type, int format, void *data, int *cause, struct tris_channel* src);
static int gtalk_digit(struct tris_channel *ast, char digit, unsigned int duration);
static int gtalk_digit_begin(struct tris_channel *ast, char digit);
static int gtalk_digit_end(struct tris_channel *ast, char digit, unsigned int duration);
static int gtalk_call(struct tris_channel *ast, char *dest, int timeout);
static int gtalk_hangup(struct tris_channel *ast);
static int gtalk_answer(struct tris_channel *ast);
static int gtalk_action(struct gtalk *client, struct gtalk_pvt *p, const char *action);
static void gtalk_free_pvt(struct gtalk *client, struct gtalk_pvt *p);
static int gtalk_newcall(struct gtalk *client, ikspak *pak);
static struct tris_frame *gtalk_read(struct tris_channel *ast);
static int gtalk_write(struct tris_channel *ast, struct tris_frame *f);
static int gtalk_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen);
static int gtalk_fixup(struct tris_channel *oldchan, struct tris_channel *newchan);
static int gtalk_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen);
static struct gtalk_pvt *gtalk_alloc(struct gtalk *client, const char *us, const char *them, const char *sid);
static char *gtalk_do_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
static char *gtalk_show_channels(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
/*----- RTP interface functions */
static int gtalk_set_rtp_peer(struct tris_channel *chan, struct tris_rtp *rtp,
							   struct tris_rtp *vrtp, struct tris_rtp *trtp, int codecs, int nat_active);
static enum tris_rtp_get_result gtalk_get_rtp_peer(struct tris_channel *chan, struct tris_rtp **rtp);
static int gtalk_get_codec(struct tris_channel *chan);

/*! \brief PBX interface structure for channel registration */
static const struct tris_channel_tech gtalk_tech = {
	.type = "Gtalk",
	.description = "Gtalk Channel Driver",
	.capabilities = TRIS_FORMAT_AUDIO_MASK,
	.requester = gtalk_request,
	.send_digit_begin = gtalk_digit_begin,
	.send_digit_end = gtalk_digit_end,
	.bridge = tris_rtp_bridge,
	.call = gtalk_call,
	.hangup = gtalk_hangup,
	.answer = gtalk_answer,
	.read = gtalk_read,
	.write = gtalk_write,
	.exception = gtalk_read,
	.indicate = gtalk_indicate,
	.fixup = gtalk_fixup,
	.send_html = gtalk_sendhtml,
	.properties = TRIS_CHAN_TP_WANTSJITTER | TRIS_CHAN_TP_CREATESJITTER
};

static struct sockaddr_in bindaddr = { 0, };	/*!< The address we bind to */

static struct sched_context *sched;	/*!< The scheduling context */
static struct io_context *io;	/*!< The IO context */
static struct in_addr __ourip;

/*! \brief RTP driver interface */
static struct tris_rtp_protocol gtalk_rtp = {
	type: "Gtalk",
	get_rtp_info: gtalk_get_rtp_peer,
	set_rtp_peer: gtalk_set_rtp_peer,
	get_codec: gtalk_get_codec,
};

static struct tris_cli_entry gtalk_cli[] = {
	TRIS_CLI_DEFINE(gtalk_do_reload, "Reload GoogleTalk configuration"),
	TRIS_CLI_DEFINE(gtalk_show_channels, "Show GoogleTalk channels"),
};

static char externip[16];

static struct gtalk_container gtalk_list;

static void gtalk_member_destroy(struct gtalk *obj)
{
	tris_free(obj);
}

static struct gtalk *find_gtalk(char *name, char *connection)
{
	struct gtalk *gtalk = NULL;
	char *domain = NULL , *s = NULL;

	if (strchr(connection, '@')) {
		s = tris_strdupa(connection);
		domain = strsep(&s, "@");
		tris_verbose("OOOOH domain = %s\n", domain);
	}
	gtalk = ASTOBJ_CONTAINER_FIND(&gtalk_list, name);
	if (!gtalk && strchr(name, '@'))
		gtalk = ASTOBJ_CONTAINER_FIND_FULL(&gtalk_list, name, user,,, strcasecmp);

	if (!gtalk) {				
		/* guest call */
		ASTOBJ_CONTAINER_TRAVERSE(&gtalk_list, 1, {
			ASTOBJ_RDLOCK(iterator);
			if (!strcasecmp(iterator->name, "guest")) {
				gtalk = iterator;
			}
			ASTOBJ_UNLOCK(iterator);

			if (gtalk)
				break;
		});

	}
	return gtalk;
}


static int add_codec_to_answer(const struct gtalk_pvt *p, int codec, iks *dcodecs)
{
	int res = 0;
	char *format = tris_getformatname(codec);

	if (!strcasecmp("ulaw", format)) {
		iks *payload_eg711u, *payload_pcmu;
		payload_pcmu = iks_new("payload-type");
		payload_eg711u = iks_new("payload-type");
	
		if(!payload_eg711u || !payload_pcmu) {
			iks_delete(payload_pcmu);
			iks_delete(payload_eg711u);
			tris_log(LOG_WARNING,"Failed to allocate iks node");
			return -1;
		}
		iks_insert_attrib(payload_pcmu, "id", "0");
		iks_insert_attrib(payload_pcmu, "name", "PCMU");
		iks_insert_attrib(payload_pcmu, "clockrate","8000");
		iks_insert_attrib(payload_pcmu, "bitrate","64000");
		iks_insert_attrib(payload_eg711u, "id", "100");
		iks_insert_attrib(payload_eg711u, "name", "EG711U");
		iks_insert_attrib(payload_eg711u, "clockrate","8000");
		iks_insert_attrib(payload_eg711u, "bitrate","64000");
		iks_insert_node(dcodecs, payload_pcmu);
		iks_insert_node(dcodecs, payload_eg711u);
		res ++;
	}
	if (!strcasecmp("alaw", format)) {
		iks *payload_eg711a, *payload_pcma;
		payload_pcma = iks_new("payload-type");
		payload_eg711a = iks_new("payload-type");
		if(!payload_eg711a || !payload_pcma) {
			iks_delete(payload_eg711a);
			iks_delete(payload_pcma);
			tris_log(LOG_WARNING,"Failed to allocate iks node");
			return -1;
		}
		iks_insert_attrib(payload_pcma, "id", "8");
		iks_insert_attrib(payload_pcma, "name", "PCMA");
		iks_insert_attrib(payload_pcma, "clockrate","8000");
		iks_insert_attrib(payload_pcma, "bitrate","64000");
		payload_eg711a = iks_new("payload-type");
		iks_insert_attrib(payload_eg711a, "id", "101");
		iks_insert_attrib(payload_eg711a, "name", "EG711A");
		iks_insert_attrib(payload_eg711a, "clockrate","8000");
		iks_insert_attrib(payload_eg711a, "bitrate","64000");
		iks_insert_node(dcodecs, payload_pcma);
		iks_insert_node(dcodecs, payload_eg711a);
		res ++;
	}
	if (!strcasecmp("ilbc", format)) {
		iks *payload_ilbc = iks_new("payload-type");
		if(!payload_ilbc) {
			tris_log(LOG_WARNING,"Failed to allocate iks node");
			return -1;
		}
		iks_insert_attrib(payload_ilbc, "id", "97");
		iks_insert_attrib(payload_ilbc, "name", "iLBC");
		iks_insert_attrib(payload_ilbc, "clockrate","8000");
		iks_insert_attrib(payload_ilbc, "bitrate","13300");
		iks_insert_node(dcodecs, payload_ilbc);
		res ++;
	}
	if (!strcasecmp("g723", format)) {
		iks *payload_g723 = iks_new("payload-type");
		if(!payload_g723) {
			tris_log(LOG_WARNING,"Failed to allocate iks node");
			return -1;
		}
		iks_insert_attrib(payload_g723, "id", "4");
		iks_insert_attrib(payload_g723, "name", "G723");
		iks_insert_attrib(payload_g723, "clockrate","8000");
		iks_insert_attrib(payload_g723, "bitrate","6300");
		iks_insert_node(dcodecs, payload_g723);
		res ++;
	}
	if (!strcasecmp("speex", format)) {
		iks *payload_speex = iks_new("payload-type");
		if(!payload_speex) {
			tris_log(LOG_WARNING,"Failed to allocate iks node");
			return -1;
		}
		iks_insert_attrib(payload_speex, "id", "110");
		iks_insert_attrib(payload_speex, "name", "speex");
		iks_insert_attrib(payload_speex, "clockrate","8000");
		iks_insert_attrib(payload_speex, "bitrate","11000");
		iks_insert_node(dcodecs, payload_speex);
		res++;
	}
	if (!strcasecmp("gsm", format)) {
		iks *payload_gsm = iks_new("payload-type");
		if(!payload_gsm) {
			tris_log(LOG_WARNING,"Failed to allocate iks node");
			return -1;
		}
		iks_insert_attrib(payload_gsm, "id", "103");
		iks_insert_attrib(payload_gsm, "name", "gsm");
		iks_insert_node(dcodecs, payload_gsm);
		res++;
	}
	tris_rtp_lookup_code(p->rtp, 1, codec);
	return res;
}

static int gtalk_invite(struct gtalk_pvt *p, char *to, char *from, char *sid, int initiator)
{
	struct gtalk *client = p->parent;
	iks *iq, *gtalk, *dcodecs, *payload_telephone, *transport;
	int x;
	int pref_codec = 0;
	int alreadysent = 0;
	int codecs_num = 0;
	char *lowerto = NULL;

	iq = iks_new("iq");
	gtalk = iks_new("session");
	dcodecs = iks_new("description");
	transport = iks_new("transport");
	payload_telephone = iks_new("payload-type");
	if (!(iq && gtalk && dcodecs && transport && payload_telephone)){
		iks_delete(iq);
		iks_delete(gtalk);
		iks_delete(dcodecs);
		iks_delete(transport);
		iks_delete(payload_telephone);
		
		tris_log(LOG_ERROR, "Could not allocate iksemel nodes\n");
		return 0;
	}
	iks_insert_attrib(dcodecs, "xmlns", "http://www.google.com/session/phone");
	iks_insert_attrib(dcodecs, "xml:lang", "en");

	for (x = 0; x < 32; x++) {
		if (!(pref_codec = tris_codec_pref_index(&client->prefs, x)))
			break;
		if (!(client->capability & pref_codec))
			continue;
		if (alreadysent & pref_codec)
			continue;
		codecs_num = add_codec_to_answer(p, pref_codec, dcodecs);
		alreadysent |= pref_codec;
	}
	
	if (codecs_num) {
		/* only propose DTMF within an audio session */
		iks_insert_attrib(payload_telephone, "id", "106");
		iks_insert_attrib(payload_telephone, "name", "telephone-event");
		iks_insert_attrib(payload_telephone, "clockrate", "8000");
	}
	iks_insert_attrib(transport,"xmlns","http://www.google.com/transport/p2p");
	
	iks_insert_attrib(iq, "type", "set");
	iks_insert_attrib(iq, "to", to);
	iks_insert_attrib(iq, "from", from);
	iks_insert_attrib(iq, "id", client->connection->mid);
	tris_aji_increment_mid(client->connection->mid);

	iks_insert_attrib(gtalk, "xmlns", "http://www.google.com/session");
	iks_insert_attrib(gtalk, "type",initiator ? "initiate": "accept");
	/* put the initiator attribute to lower case if we receive the call 
	 * otherwise GoogleTalk won't establish the session */
	if (!initiator) {
	        char c;
	        char *t = lowerto = tris_strdupa(to);
		while (((c = *t) != '/') && (*t++ = tolower(c)));
	}
	iks_insert_attrib(gtalk, "initiator", initiator ? from : lowerto);
	iks_insert_attrib(gtalk, "id", sid);
	iks_insert_node(iq, gtalk);
	iks_insert_node(gtalk, dcodecs);
	iks_insert_node(gtalk, transport);
	iks_insert_node(dcodecs, payload_telephone);

	tris_aji_send(client->connection, iq);

	iks_delete(payload_telephone);
	iks_delete(transport);
	iks_delete(dcodecs);
	iks_delete(gtalk);
	iks_delete(iq);
	return 1;
}

static int gtalk_invite_response(struct gtalk_pvt *p, char *to , char *from, char *sid, int initiator)
{
	iks *iq, *session, *transport;
	char *lowerto = NULL;

	iq = iks_new("iq");
	session = iks_new("session");
	transport = iks_new("transport");
	if(!(iq && session && transport)) {
		iks_delete(iq);
		iks_delete(session);
		iks_delete(transport);
		tris_log(LOG_ERROR, " Unable to allocate IKS node\n");
		return -1;
	}
	iks_insert_attrib(iq, "from", from);
	iks_insert_attrib(iq, "to", to);
	iks_insert_attrib(iq, "type", "set");
	iks_insert_attrib(iq, "id",p->parent->connection->mid);
	tris_aji_increment_mid(p->parent->connection->mid);
	iks_insert_attrib(session, "type", "transport-accept");
	iks_insert_attrib(session, "id", sid);
	/* put the initiator attribute to lower case if we receive the call 
	 * otherwise GoogleTalk won't establish the session */
	if (!initiator) {
	        char c;
		char *t = lowerto = tris_strdupa(to);
		while (((c = *t) != '/') && (*t++ = tolower(c)));
	}
	iks_insert_attrib(session, "initiator", initiator ? from : lowerto);
	iks_insert_attrib(session, "xmlns", "http://www.google.com/session");
	iks_insert_attrib(transport, "xmlns", "http://www.google.com/transport/p2p");
	iks_insert_node(iq,session);
	iks_insert_node(session,transport);
	tris_aji_send(p->parent->connection, iq);

	iks_delete(transport);
	iks_delete(session);
	iks_delete(iq);
	return 1;

}

static int gtalk_ringing_ack(void *data, ikspak *pak)
{
	struct gtalk_pvt *p = data;

	if (p->ringrule)
		iks_filter_remove_rule(p->parent->connection->f, p->ringrule);
	p->ringrule = NULL;
	if (p->owner)
		tris_queue_control(p->owner, TRIS_CONTROL_RINGING);
	return IKS_FILTER_EAT;
}

static int gtalk_answer(struct tris_channel *ast)
{
	struct gtalk_pvt *p = ast->tech_pvt;
	int res = 0;
	
	tris_debug(1, "Answer!\n");
	tris_mutex_lock(&p->lock);
	gtalk_invite(p, p->them, p->us,p->sid, 0);
 	manager_event(EVENT_FLAG_SYSTEM, "ChannelUpdate", "Channel: %s\r\nChanneltype: %s\r\nGtalk-SID: %s\r\n",
		ast->name, "GTALK", p->sid);
	tris_mutex_unlock(&p->lock);
	return res;
}

static enum tris_rtp_get_result gtalk_get_rtp_peer(struct tris_channel *chan, struct tris_rtp **rtp)
{
	struct gtalk_pvt *p = chan->tech_pvt;
	enum tris_rtp_get_result res = TRIS_RTP_GET_FAILED;

	if (!p)
		return res;

	tris_mutex_lock(&p->lock);
	if (p->rtp){
		*rtp = p->rtp;
		res = TRIS_RTP_TRY_PARTIAL;
	}
	tris_mutex_unlock(&p->lock);

	return res;
}

static int gtalk_get_codec(struct tris_channel *chan)
{
	struct gtalk_pvt *p = chan->tech_pvt;
	return p->peercapability;
}

static int gtalk_set_rtp_peer(struct tris_channel *chan, struct tris_rtp *rtp, struct tris_rtp *vrtp, struct tris_rtp *trtp, int codecs, int nat_active)
{
	struct gtalk_pvt *p;

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

static int gtalk_response(struct gtalk *client, char *from, ikspak *pak, const char *reasonstr, const char *reasonstr2)
{
	iks *response = NULL, *error = NULL, *reason = NULL;
	int res = -1;

	response = iks_new("iq");
	if (response) {
		iks_insert_attrib(response, "type", "result");
		iks_insert_attrib(response, "from", from);
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

static int gtalk_is_answered(struct gtalk *client, ikspak *pak)
{
	struct gtalk_pvt *tmp;
	char *from;
	iks *codec;
	char s1[BUFSIZ], s2[BUFSIZ], s3[BUFSIZ];
	int peernoncodeccapability;

	tris_log(LOG_DEBUG, "The client is %s\n", client->name);
	/* Make sure our new call doesn't exist yet */
	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, "session", "id", tmp->sid))
			break;
	}

	/* codec points to the first <payload-type/> tag */
	codec = iks_first_tag(iks_first_tag(iks_first_tag(pak->x)));
	while (codec) {
		tris_rtp_set_m_type(tmp->rtp, atoi(iks_find_attrib(codec, "id")));
		tris_rtp_set_rtpmap_type(tmp->rtp, atoi(iks_find_attrib(codec, "id")), "audio", iks_find_attrib(codec, "name"), 0);
		codec = iks_next_tag(codec);
	}
	
	/* Now gather all of the codecs that we are asked for */
	tris_rtp_get_current_formats(tmp->rtp, &tmp->peercapability, &peernoncodeccapability);
	
	/* at this point, we received an awser from the remote Gtalk client,
	   which allows us to compare capabilities */
	tmp->jointcapability = tmp->capability & tmp->peercapability;
	if (!tmp->jointcapability) {
		tris_log(LOG_WARNING, "Capabilities don't match : us - %s, peer - %s, combined - %s \n", tris_getformatname_multiple(s1, BUFSIZ, tmp->capability),
			tris_getformatname_multiple(s2, BUFSIZ, tmp->peercapability),
			tris_getformatname_multiple(s3, BUFSIZ, tmp->jointcapability));
		/* close session if capabilities don't match */
		tris_queue_hangup(tmp->owner);

		return -1;

	}	
	
	from = iks_find_attrib(pak->x, "to");
	if(!from)
		from = client->connection->jid->full;

	if (tmp) {
		if (tmp->owner)
			tris_queue_control(tmp->owner, TRIS_CONTROL_ANSWER);
	} else
		tris_log(LOG_NOTICE, "Whoa, didn't find call!\n");
	gtalk_response(client, from, pak, NULL, NULL);
	return 1;
}

static int gtalk_is_accepted(struct gtalk *client, ikspak *pak)
{
	struct gtalk_pvt *tmp;
	char *from;

	tris_log(LOG_DEBUG, "The client is %s\n", client->name);
	/* find corresponding call */
	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, "session", "id", tmp->sid))
			break;
	}

	from = iks_find_attrib(pak->x, "to");
	if(!from)
		from = client->connection->jid->full;

	if (!tmp)
		tris_log(LOG_NOTICE, "Whoa, didn't find call!\n");

	/* answer 'iq' packet to let the remote peer know that we're alive */
	gtalk_response(client, from, pak, NULL, NULL);
	return 1;
}

static int gtalk_handle_dtmf(struct gtalk *client, ikspak *pak)
{
	struct gtalk_pvt *tmp;
	iks *dtmfnode = NULL, *dtmfchild = NULL;
	char *dtmf;
	char *from;
	/* Make sure our new call doesn't exist yet */
	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, "session", "id", tmp->sid) || iks_find_with_attrib(pak->x, "gtalk", "sid", tmp->sid))
			break;
	}
	from = iks_find_attrib(pak->x, "to");
	if(!from)
		from = client->connection->jid->full;


	if (tmp) {
		if(iks_find_with_attrib(pak->x, "dtmf-method", "method", "rtp")) {
			gtalk_response(client, from, pak,
					"feature-not-implemented xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'",
					"unsupported-dtmf-method xmlns='http://jabber.org/protocol/gtalk/info/dtmf#errors'");
			return -1;
		}
		if ((dtmfnode = iks_find(pak->x, "dtmf"))) {
			if((dtmf = iks_find_attrib(dtmfnode, "code"))) {
				if(iks_find_with_attrib(pak->x, "dtmf", "action", "button-up")) {
					struct tris_frame f = {TRIS_FRAME_DTMF_BEGIN, };
					f.subclass = dtmf[0];
					tris_queue_frame(tmp->owner, &f);
					tris_verbose("GOOGLE! DTMF-relay event received: %c\n", f.subclass);
				} else if(iks_find_with_attrib(pak->x, "dtmf", "action", "button-down")) {
					struct tris_frame f = {TRIS_FRAME_DTMF_END, };
					f.subclass = dtmf[0];
					tris_queue_frame(tmp->owner, &f);
					tris_verbose("GOOGLE! DTMF-relay event received: %c\n", f.subclass);
				} else if(iks_find_attrib(pak->x, "dtmf")) { /* 250 millasecond default */
					struct tris_frame f = {TRIS_FRAME_DTMF, };
					f.subclass = dtmf[0];
					tris_queue_frame(tmp->owner, &f);
					tris_verbose("GOOGLE! DTMF-relay event received: %c\n", f.subclass);
				}
			}
		} else if ((dtmfnode = iks_find_with_attrib(pak->x, "gtalk", "action", "session-info"))) {
			if((dtmfchild = iks_find(dtmfnode, "dtmf"))) {
				if((dtmf = iks_find_attrib(dtmfchild, "code"))) {
					if(iks_find_with_attrib(dtmfnode, "dtmf", "action", "button-up")) {
						struct tris_frame f = {TRIS_FRAME_DTMF_END, };
						f.subclass = dtmf[0];
						tris_queue_frame(tmp->owner, &f);
						tris_verbose("GOOGLE! DTMF-relay event received: %c\n", f.subclass);
					} else if(iks_find_with_attrib(dtmfnode, "dtmf", "action", "button-down")) {
						struct tris_frame f = {TRIS_FRAME_DTMF_BEGIN, };
						f.subclass = dtmf[0];
						tris_queue_frame(tmp->owner, &f);
						tris_verbose("GOOGLE! DTMF-relay event received: %c\n", f.subclass);
					}
				}
			}
		}
		gtalk_response(client, from, pak, NULL, NULL);
		return 1;
	} else
		tris_log(LOG_NOTICE, "Whoa, didn't find call!\n");

	gtalk_response(client, from, pak, NULL, NULL);
	return 1;
}

static int gtalk_hangup_farend(struct gtalk *client, ikspak *pak)
{
	struct gtalk_pvt *tmp;
	char *from;

	tris_debug(1, "The client is %s\n", client->name);
	/* Make sure our new call doesn't exist yet */
	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, "session", "id", tmp->sid))
			break;
	}
	from = iks_find_attrib(pak->x, "to");
	if(!from)
		from = client->connection->jid->full;

	if (tmp) {
		tmp->alreadygone = 1;
		if (tmp->owner)
			tris_queue_hangup(tmp->owner);
	} else
		tris_log(LOG_NOTICE, "Whoa, didn't find call!\n");
	gtalk_response(client, from, pak, NULL, NULL);
	return 1;
}

static int gtalk_create_candidates(struct gtalk *client, struct gtalk_pvt *p, char *sid, char *from, char *to)
{
	struct gtalk_candidate *tmp;
	struct aji_client *c = client->connection;
	struct gtalk_candidate *ours1 = NULL, *ours2 = NULL;
	struct sockaddr_in sin;
	struct sockaddr_in dest;
	struct in_addr us;
	iks *iq, *gtalk, *candidate, *transport;
	char user[17], pass[17], preference[5], port[7];
	char *lowerfrom = NULL;


	iq = iks_new("iq");
	gtalk = iks_new("session");
	candidate = iks_new("candidate");
	transport = iks_new("transport");
	if (!iq || !gtalk || !candidate || !transport) {
		tris_log(LOG_ERROR, "Memory allocation error\n");
		goto safeout;
	}
	ours1 = tris_calloc(1, sizeof(*ours1));
	ours2 = tris_calloc(1, sizeof(*ours2));
	if (!ours1 || !ours2)
		goto safeout;

	iks_insert_attrib(transport, "xmlns","http://www.google.com/transport/p2p");
	iks_insert_node(iq, gtalk);
	iks_insert_node(gtalk,transport);
	iks_insert_node(transport, candidate);

	for (; p; p = p->next) {
		if (!strcasecmp(p->sid, sid))
			break;
	}

	if (!p) {
		tris_log(LOG_NOTICE, "No matching gtalk session - SID %s!\n", sid);
		goto safeout;
	}

	tris_rtp_get_us(p->rtp, &sin);
	tris_find_ourip(&us, bindaddr);
	if (!strcmp(tris_inet_ntoa(us), "127.0.0.1")) {
		tris_log(LOG_WARNING, "Found a loopback IP on the system, check your network configuration or set the bindaddr attribute.");
	}

	/* Setup our gtalk candidates */
	tris_copy_string(ours1->name, "rtp", sizeof(ours1->name));
	ours1->port = ntohs(sin.sin_port);
	ours1->preference = 1;
	snprintf(user, sizeof(user), "%08lx%08lx", tris_random(), tris_random());
	snprintf(pass, sizeof(pass), "%08lx%08lx", tris_random(), tris_random());
	tris_copy_string(ours1->username, user, sizeof(ours1->username));
	tris_copy_string(ours1->password, pass, sizeof(ours1->password));
	tris_copy_string(ours1->ip, tris_inet_ntoa(us), sizeof(ours1->ip));
	ours1->protocol = AJI_PROTOCOL_UDP;
	ours1->type = AJI_CONNECT_LOCAL;
	ours1->generation = 0;
	p->ourcandidates = ours1;

	if (!tris_strlen_zero(externip)) {
		/* XXX We should really stun for this one not just go with externip XXX */
		snprintf(user, sizeof(user), "%08lx%08lx", tris_random(), tris_random());
		snprintf(pass, sizeof(pass), "%08lx%08lx", tris_random(), tris_random());
		tris_copy_string(ours2->username, user, sizeof(ours2->username));
		tris_copy_string(ours2->password, pass, sizeof(ours2->password));
		tris_copy_string(ours2->ip, externip, sizeof(ours2->ip));
		tris_copy_string(ours2->name, "rtp", sizeof(ours1->name));
		ours2->port = ntohs(sin.sin_port);
		ours2->preference = 0.9;
		ours2->protocol = AJI_PROTOCOL_UDP;
		ours2->type = AJI_CONNECT_STUN;
		ours2->generation = 0;
		ours1->next = ours2;
		ours2 = NULL;
	}
	ours1 = NULL;
	dest.sin_addr = __ourip;
	dest.sin_port = sin.sin_port;


	for (tmp = p->ourcandidates; tmp; tmp = tmp->next) {
		snprintf(port, sizeof(port), "%d", tmp->port);
		snprintf(preference, sizeof(preference), "%.2f", tmp->preference);
		iks_insert_attrib(iq, "from", to);
		iks_insert_attrib(iq, "to", from);
		iks_insert_attrib(iq, "type", "set");
		iks_insert_attrib(iq, "id", c->mid);
		tris_aji_increment_mid(c->mid);
		iks_insert_attrib(gtalk, "type", "transport-info");
		iks_insert_attrib(gtalk, "id", sid);
		/* put the initiator attribute to lower case if we receive the call 
		 * otherwise GoogleTalk won't establish the session */
		if (!p->initiator) {
		        char c;
			char *t = lowerfrom = tris_strdupa(from);
			while (((c = *t) != '/') && (*t++ = tolower(c)));
		}
		iks_insert_attrib(gtalk, "initiator", (p->initiator) ? to : lowerfrom);
		iks_insert_attrib(gtalk, "xmlns", GOOGLE_NS);
		iks_insert_attrib(candidate, "name", tmp->name);
		iks_insert_attrib(candidate, "address", tmp->ip);
		iks_insert_attrib(candidate, "port", port);
		iks_insert_attrib(candidate, "username", tmp->username);
		iks_insert_attrib(candidate, "password", tmp->password);
		iks_insert_attrib(candidate, "preference", preference);
		if (tmp->protocol == AJI_PROTOCOL_UDP)
			iks_insert_attrib(candidate, "protocol", "udp");
		if (tmp->protocol == AJI_PROTOCOL_SSLTCP)
			iks_insert_attrib(candidate, "protocol", "ssltcp");
		if (tmp->type == AJI_CONNECT_STUN)
			iks_insert_attrib(candidate, "type", "stun");
		if (tmp->type == AJI_CONNECT_LOCAL)
			iks_insert_attrib(candidate, "type", "local");
		if (tmp->type == AJI_CONNECT_RELAY)
			iks_insert_attrib(candidate, "type", "relay");
		iks_insert_attrib(candidate, "network", "0");
		iks_insert_attrib(candidate, "generation", "0");
		tris_aji_send(c, iq);
	}
	p->laststun = 0;

safeout:
	if (ours1)
		tris_free(ours1);
	if (ours2)
		tris_free(ours2);
	iks_delete(iq);
	iks_delete(gtalk);
	iks_delete(candidate);
	iks_delete(transport);

	return 1;
}

static struct gtalk_pvt *gtalk_alloc(struct gtalk *client, const char *us, const char *them, const char *sid)
{
	struct gtalk_pvt *tmp = NULL;
	struct aji_resource *resources = NULL;
	struct aji_buddy *buddy;
	char idroster[200];
	char *data, *exten = NULL;

	tris_debug(1, "The client is %s for alloc\n", client->name);
	if (!sid && !strchr(them, '/')) {	/* I started call! */
		if (!strcasecmp(client->name, "guest")) {
			buddy = ASTOBJ_CONTAINER_FIND(&client->connection->buddies, them);
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
			snprintf(idroster, sizeof(idroster), "%s/%s", them, resources->resource);
		else {
			tris_log(LOG_ERROR, "no gtalk capable clients to talk to.\n");
			return NULL;
		}
	}
	if (!(tmp = tris_calloc(1, sizeof(*tmp)))) {
		return NULL;
	}

	memcpy(&tmp->prefs, &client->prefs, sizeof(struct tris_codec_pref));

	if (sid) {
		tris_copy_string(tmp->sid, sid, sizeof(tmp->sid));
		tris_copy_string(tmp->them, them, sizeof(tmp->them));
		tris_copy_string(tmp->us, us, sizeof(tmp->us));
	} else {
		snprintf(tmp->sid, sizeof(tmp->sid), "%08lx%08lx", tris_random(), tris_random());
		tris_copy_string(tmp->them, idroster, sizeof(tmp->them));
		tris_copy_string(tmp->us, us, sizeof(tmp->us));
		tmp->initiator = 1;
	}
	/* clear codecs */
	tmp->rtp = tris_rtp_new_with_bindaddr(sched, io, 1, 0, bindaddr.sin_addr);
	tris_rtp_pt_clear(tmp->rtp);

	/* add user configured codec capabilites */
	if (client->capability)
		tmp->capability = client->capability;
	else if (global_capability)
		tmp->capability = global_capability;

	tmp->parent = client;
	if (!tmp->rtp) {
		tris_log(LOG_WARNING, "Out of RTP sessions?\n");
		tris_free(tmp);
		return NULL;
	}

	/* Set CALLERID(name) to the full JID of the remote peer */
	tris_copy_string(tmp->cid_name, tmp->them, sizeof(tmp->cid_name));

	if(strchr(tmp->us, '/')) {
		data = tris_strdupa(tmp->us);
		exten = strsep(&data, "/");
	} else
		exten = tmp->us;
	tris_copy_string(tmp->exten,  exten, sizeof(tmp->exten));
	tris_mutex_init(&tmp->lock);
	tris_mutex_lock(&gtalklock);
	tmp->next = client->p;
	client->p = tmp;
	tris_mutex_unlock(&gtalklock);
	return tmp;
}

/*! \brief Start new gtalk channel */
static struct tris_channel *gtalk_new(struct gtalk *client, struct gtalk_pvt *i, int state, const char *title)
{
	struct tris_channel *tmp;
	int fmt;
	int what;
	const char *n2;

	if (title)
		n2 = title;
	else
		n2 = i->us;
	tmp = tris_channel_alloc(1, state, i->cid_num, i->cid_name, client->accountcode, i->exten, client->context, client->amaflags, "Gtalk/%s-%04lx", n2, tris_random() & 0xffff);
	if (!tmp) {
		tris_log(LOG_WARNING, "Unable to allocate Gtalk channel structure!\n");
		return NULL;
	}
	tmp->tech = &gtalk_tech;

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
		tris_rtp_setstun(i->rtp, 1);
		tris_channel_set_fd(tmp, 0, tris_rtp_fd(i->rtp));
		tris_channel_set_fd(tmp, 1, tris_rtcp_fd(i->rtp));
	}
	if (i->vrtp) {
		tris_rtp_setstun(i->rtp, 1);
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
	if (!tris_strlen_zero(client->parkinglot))
		tris_string_field_set(tmp, parkinglot, client->parkinglot);
	i->owner = tmp;
	tris_module_ref(tris_module_info->self);
	tris_copy_string(tmp->context, client->context, sizeof(tmp->context));
	tris_copy_string(tmp->exten, i->exten, sizeof(tmp->exten));

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
	} else {
		manager_event(EVENT_FLAG_SYSTEM, "ChannelUpdate",
			"Channel: %s\r\nChanneltype: %s\r\nGtalk-SID: %s\r\n",
			i->owner ? i->owner->name : "", "Gtalk", i->sid);
	}
	return tmp;
}

static int gtalk_action(struct gtalk *client, struct gtalk_pvt *p, const char *action)
{
	iks *request, *session = NULL;
	int res = -1;
	char *lowerthem = NULL;

	request = iks_new("iq");
	if (request) {
		iks_insert_attrib(request, "type", "set");
		iks_insert_attrib(request, "from", p->us);
		iks_insert_attrib(request, "to", p->them);
		iks_insert_attrib(request, "id", client->connection->mid);
		tris_aji_increment_mid(client->connection->mid);
		session = iks_new("session");
		if (session) {
			iks_insert_attrib(session, "type", action);
			iks_insert_attrib(session, "id", p->sid);
			/* put the initiator attribute to lower case if we receive the call 
			 * otherwise GoogleTalk won't establish the session */
			if (!p->initiator) {
			        char c;
				char *t = lowerthem = tris_strdupa(p->them);
				while (((c = *t) != '/') && (*t++ = tolower(c)));
			}
			iks_insert_attrib(session, "initiator", p->initiator ? p->us : lowerthem);
			iks_insert_attrib(session, "xmlns", "http://www.google.com/session");
			iks_insert_node(request, session);
			tris_aji_send(client->connection, request);
			res = 0;
		}
	}

	iks_delete(session);
	iks_delete(request);

	return res;
}

static void gtalk_free_candidates(struct gtalk_candidate *candidate)
{
	struct gtalk_candidate *last;
	while (candidate) {
		last = candidate;
		candidate = candidate->next;
		tris_free(last);
	}
}

static void gtalk_free_pvt(struct gtalk *client, struct gtalk_pvt *p)
{
	struct gtalk_pvt *cur, *prev = NULL;
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
	gtalk_free_candidates(p->theircandidates);
	tris_free(p);
}


static int gtalk_newcall(struct gtalk *client, ikspak *pak)
{
	struct gtalk_pvt *p, *tmp = client->p;
	struct tris_channel *chan;
	int res;
	iks *codec;
	char *from = NULL;
	char s1[BUFSIZ], s2[BUFSIZ], s3[BUFSIZ];
	int peernoncodeccapability;

	/* Make sure our new call doesn't exist yet */
	from = iks_find_attrib(pak->x,"to");
	if(!from)
		from = client->connection->jid->full;
	
	while (tmp) {
		if (iks_find_with_attrib(pak->x, "session", "id", tmp->sid)) {
			tris_log(LOG_NOTICE, "Ignoring duplicate call setup on SID %s\n", tmp->sid);
			gtalk_response(client, from, pak, "out-of-order", NULL);
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

	p = gtalk_alloc(client, from, pak->from->full, iks_find_attrib(pak->query, "id"));
	if (!p) {
		tris_log(LOG_WARNING, "Unable to allocate gtalk structure!\n");
		return -1;
	}

	chan = gtalk_new(client, p, TRIS_STATE_DOWN, pak->from->user);
	if (!chan) {
		gtalk_free_pvt(client, p);
		return -1;
	}

	tris_mutex_lock(&p->lock);
	tris_copy_string(p->them, pak->from->full, sizeof(p->them));
	if (iks_find_attrib(pak->query, "id")) {
		tris_copy_string(p->sid, iks_find_attrib(pak->query, "id"),
				sizeof(p->sid));
	}

	/* codec points to the first <payload-type/> tag */	
	codec = iks_first_tag(iks_first_tag(iks_first_tag(pak->x)));
	
	while (codec) {
		tris_rtp_set_m_type(p->rtp, atoi(iks_find_attrib(codec, "id")));
		tris_rtp_set_rtpmap_type(p->rtp, atoi(iks_find_attrib(codec, "id")), "audio", iks_find_attrib(codec, "name"), 0);
		codec = iks_next_tag(codec);
	}
	
	/* Now gather all of the codecs that we are asked for */
	tris_rtp_get_current_formats(p->rtp, &p->peercapability, &peernoncodeccapability);
	p->jointcapability = p->capability & p->peercapability;
	tris_mutex_unlock(&p->lock);
		
	tris_setstate(chan, TRIS_STATE_RING);
	if (!p->jointcapability) {
		tris_log(LOG_WARNING, "Capabilities don't match : us - %s, peer - %s, combined - %s \n", tris_getformatname_multiple(s1, BUFSIZ, p->capability),
			tris_getformatname_multiple(s2, BUFSIZ, p->peercapability),
			tris_getformatname_multiple(s3, BUFSIZ, p->jointcapability));
		/* close session if capabilities don't match */
		gtalk_action(client, p, "reject");
		p->alreadygone = 1;
		gtalk_hangup(chan);
		tris_channel_free(chan);
		return -1;
	}	

	res = tris_pbx_start(chan);
	
	switch (res) {
	case TRIS_PBX_FAILED:
		tris_log(LOG_WARNING, "Failed to start PBX :(\n");
		gtalk_response(client, from, pak, "service-unavailable", NULL);
		break;
	case TRIS_PBX_CALL_LIMIT:
		tris_log(LOG_WARNING, "Failed to start PBX (call limit reached) \n");
		gtalk_response(client, from, pak, "service-unavailable", NULL);
		break;
	case TRIS_PBX_SUCCESS:
		gtalk_response(client, from, pak, NULL, NULL);
		gtalk_invite_response(p, p->them, p->us,p->sid, 0);
		gtalk_create_candidates(client, p, p->sid, p->them, p->us);
		/* nothing to do */
		break;
	}

	return 1;
}

static int gtalk_update_stun(struct gtalk *client, struct gtalk_pvt *p)
{
	struct gtalk_candidate *tmp;
	struct hostent *hp;
	struct tris_hostent ahp;
	struct sockaddr_in sin;
	struct sockaddr_in aux;

	if (time(NULL) == p->laststun)
		return 0;

	tmp = p->theircandidates;
	p->laststun = time(NULL);
	while (tmp) {
		char username[256];

		/* Find the IP address of the host */
		hp = tris_gethostbyname(tmp->ip, &ahp);
		sin.sin_family = AF_INET;
		memcpy(&sin.sin_addr, hp->h_addr, sizeof(sin.sin_addr));
		sin.sin_port = htons(tmp->port);
		snprintf(username, sizeof(username), "%s%s", tmp->username,
			 p->ourcandidates->username);
		
		/* Find out the result of the STUN */
		tris_rtp_get_peer(p->rtp, &aux);

		/* If the STUN result is different from the IP of the hostname,
			lock on the stun IP of the hostname advertised by the
			remote client */
		if (aux.sin_addr.s_addr && 
		    aux.sin_addr.s_addr != sin.sin_addr.s_addr)
			tris_rtp_stun_request(p->rtp, &aux, username);
		else 
			tris_rtp_stun_request(p->rtp, &sin, username);
		
		if (aux.sin_addr.s_addr) {
			tris_debug(4, "Receiving RTP traffic from IP %s, matches with remote candidate's IP %s\n", tris_inet_ntoa(aux.sin_addr), tmp->ip);
			tris_debug(4, "Sending STUN request to %s\n", tmp->ip);
		}

		tmp = tmp->next;
	}
	return 1;
}

static int gtalk_add_candidate(struct gtalk *client, ikspak *pak)
{
	struct gtalk_pvt *p = NULL, *tmp = NULL;
	struct aji_client *c = client->connection;
	struct gtalk_candidate *newcandidate = NULL;
	iks *traversenodes = NULL, *receipt = NULL;
	char *from;

	from = iks_find_attrib(pak->x,"to");
	if(!from)
		from = c->jid->full;

	for (tmp = client->p; tmp; tmp = tmp->next) {
		if (iks_find_with_attrib(pak->x, "session", "id", tmp->sid)) {
			p = tmp;
			break;
		}
	}

	if (!p)
		return -1;

	traversenodes = pak->query;
	while(traversenodes) {
		if(!strcasecmp(iks_name(traversenodes), "session")) {
			traversenodes = iks_first_tag(traversenodes);
			continue;
		}
		if(!strcasecmp(iks_name(traversenodes), "transport")) {
			traversenodes = iks_first_tag(traversenodes);
			continue;
		}
		if(!strcasecmp(iks_name(traversenodes), "candidate")) {
			newcandidate = tris_calloc(1, sizeof(*newcandidate));
			if (!newcandidate)
				return 0;
			tris_copy_string(newcandidate->name, iks_find_attrib(traversenodes, "name"),
							sizeof(newcandidate->name));
			tris_copy_string(newcandidate->ip, iks_find_attrib(traversenodes, "address"),
							sizeof(newcandidate->ip));
			newcandidate->port = atoi(iks_find_attrib(traversenodes, "port"));
			tris_copy_string(newcandidate->username, iks_find_attrib(traversenodes, "username"),
							sizeof(newcandidate->username));
			tris_copy_string(newcandidate->password, iks_find_attrib(traversenodes, "password"),
							sizeof(newcandidate->password));
			newcandidate->preference = atof(iks_find_attrib(traversenodes, "preference"));
			if (!strcasecmp(iks_find_attrib(traversenodes, "protocol"), "udp"))
				newcandidate->protocol = AJI_PROTOCOL_UDP;
			if (!strcasecmp(iks_find_attrib(traversenodes, "protocol"), "ssltcp"))
				newcandidate->protocol = AJI_PROTOCOL_SSLTCP;
		
			if (!strcasecmp(iks_find_attrib(traversenodes, "type"), "stun"))
				newcandidate->type = AJI_CONNECT_STUN;
			if (!strcasecmp(iks_find_attrib(traversenodes, "type"), "local"))
				newcandidate->type = AJI_CONNECT_LOCAL;
			if (!strcasecmp(iks_find_attrib(traversenodes, "type"), "relay"))
				newcandidate->type = AJI_CONNECT_RELAY;
			tris_copy_string(newcandidate->network, iks_find_attrib(traversenodes, "network"),
							sizeof(newcandidate->network));
			newcandidate->generation = atoi(iks_find_attrib(traversenodes, "generation"));
			newcandidate->next = NULL;
		
			newcandidate->next = p->theircandidates;
			p->theircandidates = newcandidate;
			p->laststun = 0;
			gtalk_update_stun(p->parent, p);
			newcandidate = NULL;
		}
		traversenodes = iks_next_tag(traversenodes);
	}
	
	receipt = iks_new("iq");
	iks_insert_attrib(receipt, "type", "result");
	iks_insert_attrib(receipt, "from", from);
	iks_insert_attrib(receipt, "to", iks_find_attrib(pak->x, "from"));
	iks_insert_attrib(receipt, "id", iks_find_attrib(pak->x, "id"));
	tris_aji_send(c, receipt);

	iks_delete(receipt);

	return 1;
}

static struct tris_frame *gtalk_rtp_read(struct tris_channel *ast, struct gtalk_pvt *p)
{
	struct tris_frame *f;

	if (!p->rtp)
		return &tris_null_frame;
	f = tris_rtp_read(p->rtp);
	gtalk_update_stun(p->parent, p);
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
				if (option_debug && f && (f->frametype == TRIS_FRAME_DTMF))
					tris_debug(1, "* Detected inband DTMF '%c'\n", f->subclass);
		        } */
		}
	}
	return f;
}

static struct tris_frame *gtalk_read(struct tris_channel *ast)
{
	struct tris_frame *fr;
	struct gtalk_pvt *p = ast->tech_pvt;

	tris_mutex_lock(&p->lock);
	fr = gtalk_rtp_read(ast, p);
	tris_mutex_unlock(&p->lock);
	return fr;
}

/*! \brief Send frame to media channel (rtp) */
static int gtalk_write(struct tris_channel *ast, struct tris_frame *frame)
{
	struct gtalk_pvt *p = ast->tech_pvt;
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
		tris_log(LOG_WARNING, "Can't send %d type frames with Gtalk write\n",
				frame->frametype);
		return 0;
	}

	return res;
}

static int gtalk_fixup(struct tris_channel *oldchan, struct tris_channel *newchan)
{
	struct gtalk_pvt *p = newchan->tech_pvt;
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

static int gtalk_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen)
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

static int gtalk_digit_begin(struct tris_channel *chan, char digit)
{
	return gtalk_digit(chan, digit, 0);
}

static int gtalk_digit_end(struct tris_channel *chan, char digit, unsigned int duration)
{
	return gtalk_digit(chan, digit, duration);
}

static int gtalk_digit(struct tris_channel *ast, char digit, unsigned int duration)
{
	struct gtalk_pvt *p = ast->tech_pvt;
	struct gtalk *client = p->parent;
	iks *iq, *gtalk, *dtmf;
	char buffer[2] = {digit, '\0'};
	char *lowerthem = NULL;
	iq = iks_new("iq");
	gtalk = iks_new("gtalk");
	dtmf = iks_new("dtmf");
	if(!iq || !gtalk || !dtmf) {
		iks_delete(iq);
		iks_delete(gtalk);
		iks_delete(dtmf);
		tris_log(LOG_ERROR, "Did not send dtmf do to memory issue\n");
		return -1;
	}

	iks_insert_attrib(iq, "type", "set");
	iks_insert_attrib(iq, "to", p->them);
	iks_insert_attrib(iq, "from", p->us);
	iks_insert_attrib(iq, "id", client->connection->mid);
	tris_aji_increment_mid(client->connection->mid);
	iks_insert_attrib(gtalk, "xmlns", "http://jabber.org/protocol/gtalk");
	iks_insert_attrib(gtalk, "action", "session-info");
	/* put the initiator attribute to lower case if we receive the call 
	 * otherwise GoogleTalk won't establish the session */
	if (!p->initiator) {
	        char c;
	        char *t = lowerthem = tris_strdupa(p->them);
	        while (((c = *t) != '/') && (*t++ = tolower(c)));
	}
	iks_insert_attrib(gtalk, "initiator", p->initiator ? p->us: lowerthem);
	iks_insert_attrib(gtalk, "sid", p->sid);
	iks_insert_attrib(dtmf, "xmlns", "http://jabber.org/protocol/gtalk/info/dtmf");
	iks_insert_attrib(dtmf, "code", buffer);
	iks_insert_node(iq, gtalk);
	iks_insert_node(gtalk, dtmf);

	tris_mutex_lock(&p->lock);
	if (ast->dtmff.frametype == TRIS_FRAME_DTMF_BEGIN || duration == 0) {
		iks_insert_attrib(dtmf, "action", "button-down");
	} else if (ast->dtmff.frametype == TRIS_FRAME_DTMF_END || duration != 0) {
		iks_insert_attrib(dtmf, "action", "button-up");
	}
	tris_aji_send(client->connection, iq);

	iks_delete(iq);
	iks_delete(gtalk);
	iks_delete(dtmf);
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int gtalk_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen)
{
	tris_log(LOG_NOTICE, "XXX Implement gtalk sendhtml XXX\n");

	return -1;
}

/* Not in use right now.
static int gtalk_auto_congest(void *nothing)
{
	struct gtalk_pvt *p = nothing;

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
static int gtalk_call(struct tris_channel *ast, char *dest, int timeout)
{
	struct gtalk_pvt *p = ast->tech_pvt;

	if ((ast->_state != TRIS_STATE_DOWN) && (ast->_state != TRIS_STATE_RESERVED)) {
		tris_log(LOG_WARNING, "gtalk_call called on %s, neither down nor reserved\n", ast->name);
		return -1;
	}

	tris_setstate(ast, TRIS_STATE_RING);
	if (!p->ringrule) {
		tris_copy_string(p->ring, p->parent->connection->mid, sizeof(p->ring));
		p->ringrule = iks_filter_add_rule(p->parent->connection->f, gtalk_ringing_ack, p,
							IKS_RULE_ID, p->ring, IKS_RULE_DONE);
	} else
		tris_log(LOG_WARNING, "Whoa, already have a ring rule!\n");

	gtalk_invite(p, p->them, p->us, p->sid, 1);
	gtalk_create_candidates(p->parent, p, p->sid, p->them, p->us);

	return 0;
}

/*! \brief Hangup a call through the gtalk proxy channel */
static int gtalk_hangup(struct tris_channel *ast)
{
	struct gtalk_pvt *p = ast->tech_pvt;
	struct gtalk *client;

	tris_mutex_lock(&p->lock);
	client = p->parent;
	p->owner = NULL;
	ast->tech_pvt = NULL;
	if (!p->alreadygone)
		gtalk_action(client, p, "terminate");
	tris_mutex_unlock(&p->lock);

	gtalk_free_pvt(client, p);
	tris_module_unref(tris_module_info->self);

	return 0;
}

/*! \brief Part of PBX interface */
static struct tris_channel *gtalk_request(const char *type, int format, void *data, int *cause, struct tris_channel* src)
{
	struct gtalk_pvt *p = NULL;
	struct gtalk *client = NULL;
	char *sender = NULL, *to = NULL, *s = NULL;
	struct tris_channel *chan = NULL;

	if (data) {
		s = tris_strdupa(data);
		if (s) {
			sender = strsep(&s, "/");
			if (sender && (sender[0] != '\0'))
				to = strsep(&s, "/");
			if (!to) {
				tris_log(LOG_ERROR, "Bad arguments in Gtalk Dialstring: %s\n", (char*) data);
				return NULL;
			}
		}
	}

	client = find_gtalk(to, sender);
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
			ASTOBJ_UNREF(client, gtalk_member_destroy);
			return NULL;
		}
	}
       
	ASTOBJ_WRLOCK(client);
	p = gtalk_alloc(client, strchr(sender, '@') ? sender : client->connection->jid->full, strchr(to, '@') ? to : client->user, NULL);
	if (p)
		chan = gtalk_new(client, p, TRIS_STATE_DOWN, to);

	ASTOBJ_UNLOCK(client);
	return chan;
}

/*! \brief CLI command "gtalk show channels" */
static char *gtalk_show_channels(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMAT  "%-30.30s  %-30.30s  %-15.15s  %-5.5s %-5.5s \n"
	struct gtalk_pvt *p;
	struct tris_channel *chan;
	int numchans = 0;
	char them[AJI_MAX_JIDLEN];
	char *jid = NULL;
	char *resource = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "gtalk show channels";
		e->usage =
			"Usage: gtalk show channels\n"
			"       Shows current state of the Gtalk channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	tris_mutex_lock(&gtalklock);
	tris_cli(a->fd, FORMAT, "Channel", "Jabber ID", "Resource", "Read", "Write");
	ASTOBJ_CONTAINER_TRAVERSE(&gtalk_list, 1, {
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

	tris_mutex_unlock(&gtalklock);

	tris_cli(a->fd, "%d active gtalk channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return CLI_SUCCESS;
#undef FORMAT
}

/*! \brief CLI command "gtalk reload" */
static char *gtalk_do_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "gtalk reload";
		e->usage =
			"Usage: gtalk reload\n"
			"       Reload gtalk channel driver.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}	
	
	tris_verbose("IT DOES WORK!\n");
	return CLI_SUCCESS;
}

static int gtalk_parser(void *data, ikspak *pak)
{
	struct gtalk *client = ASTOBJ_REF((struct gtalk *) data);

	if (iks_find_attrib(pak->x, "type") && !strcmp(iks_find_attrib (pak->x, "type"),"error")) {
		tris_log(LOG_NOTICE, "Remote peer reported an error, trying to establish the call anyway\n");
	}
	else if (iks_find_with_attrib(pak->x, "session", "type", "initiate")) {
		/* New call */
		gtalk_newcall(client, pak);
	} else if (iks_find_with_attrib(pak->x, "session", "type", "candidates") || iks_find_with_attrib(pak->x, "session", "type", "transport-info")) {
		tris_debug(3, "About to add candidate!\n");
		gtalk_add_candidate(client, pak);
		tris_debug(3, "Candidate Added!\n");
	} else if (iks_find_with_attrib(pak->x, "session", "type", "accept")) {
		gtalk_is_answered(client, pak);
	} else if (iks_find_with_attrib(pak->x, "session", "type", "transport-accept")) {
		gtalk_is_accepted(client, pak);
	} else if (iks_find_with_attrib(pak->x, "session", "type", "content-info") || iks_find_with_attrib(pak->x, "gtalk", "action", "session-info")) {
		gtalk_handle_dtmf(client, pak);
	} else if (iks_find_with_attrib(pak->x, "session", "type", "terminate")) {
		gtalk_hangup_farend(client, pak);
	} else if (iks_find_with_attrib(pak->x, "session", "type", "reject")) {
		gtalk_hangup_farend(client, pak);
	}
	ASTOBJ_UNREF(client, gtalk_member_destroy);
	return IKS_FILTER_EAT;
}

/* Not using this anymore probably take out soon 
static struct gtalk_candidate *gtalk_create_candidate(char *args)
{
	char *name, *type, *preference, *protocol;
	struct gtalk_candidate *res;
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
		if (!strcasecmp("stun", type))
			res->type = AJI_CONNECT_STUN;
		if (!strcasecmp("local", type))
			res->type = AJI_CONNECT_LOCAL;
		if (!strcasecmp("relay", type))
			res->type = AJI_CONNECT_RELAY;
	}

	return res;
}
*/

static int gtalk_create_member(char *label, struct tris_variable *var, int allowguest,
								struct tris_codec_pref prefs, char *context,
								struct gtalk *member)
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
		struct gtalk_candidate *candidate = NULL;
#endif
		if (!strcasecmp(var->name, "username"))
			tris_copy_string(member->user, var->value, sizeof(member->user));
		else if (!strcasecmp(var->name, "disallow"))
			tris_parse_allow_disallow(&member->prefs, &member->capability, var->value, 0);
		else if (!strcasecmp(var->name, "allow"))
			tris_parse_allow_disallow(&member->prefs, &member->capability, var->value, 1);
		else if (!strcasecmp(var->name, "context"))
			tris_copy_string(member->context, var->value, sizeof(member->context));
		else if (!strcasecmp(var->name, "parkinglot"))
			tris_copy_string(member->parkinglot, var->value, sizeof(member->parkinglot));
#if 0
		else if (!strcasecmp(var->name, "candidate")) {
			candidate = gtalk_create_candidate(var->value);
			if (candidate) {
				candidate->next = member->ourcandidates;
				member->ourcandidates = candidate;
			}
		}
#endif
		else if (!strcasecmp(var->name, "connection")) {
			if ((client = tris_aji_get_client(var->value))) {
				member->connection = client;
				iks_filter_add_rule(client->f, gtalk_parser, member, 
						    IKS_RULE_TYPE, IKS_PAK_IQ, 
						    IKS_RULE_FROM_PARTIAL, member->user,
						    IKS_RULE_NS, "http://www.google.com/session",
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

static int gtalk_load_config(void)
{
	char *cat = NULL;
	struct tris_config *cfg = NULL;
	char context[TRIS_MAX_CONTEXT];
	char parkinglot[TRIS_MAX_CONTEXT];
	int allowguest = 1;
	struct tris_variable *var;
	struct gtalk *member;
	struct tris_codec_pref prefs;
	struct aji_client_container *clients;
	struct gtalk_candidate *global_candidates = NULL;
	struct hostent *hp;
	struct tris_hostent ahp;
	struct tris_flags config_flags = { 0 };

	cfg = tris_config_load(GOOGLE_CONFIG, config_flags);
	if (!cfg) {
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file %s is in an invalid format.  Aborting.\n", GOOGLE_CONFIG);
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
		else if (!strcasecmp(var->name, "parkinglot"))
			tris_copy_string(parkinglot, var->value, sizeof(parkinglot));
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
			candidate = gtalk_create_candidate(var->value);
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
				tris_copy_string(member->parkinglot, parkinglot, sizeof(member->parkinglot));
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
						candidate = gtalk_create_candidate(var->value);
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
						iks_filter_add_rule(iterator->f, gtalk_parser, member, IKS_RULE_TYPE, IKS_PAK_IQ, IKS_RULE_NS, "http://www.google.com/session", IKS_RULE_DONE);
						iks_filter_add_rule(iterator->f, gtalk_parser, member, IKS_RULE_TYPE, IKS_PAK_IQ, IKS_RULE_NS, "http://jabber.org/protocol/gtalk", IKS_RULE_DONE);
						ASTOBJ_UNLOCK(member);
						ASTOBJ_UNLOCK(iterator);
					});
					ASTOBJ_CONTAINER_LINK(&gtalk_list, member);
					ASTOBJ_UNREF(member, gtalk_member_destroy);
				} else {
					ASTOBJ_UNLOCK(member);
					ASTOBJ_UNREF(member, gtalk_member_destroy);
				}
			} else {
				ASTOBJ_UNLOCK(member);
				if (gtalk_create_member(cat, var, allowguest, prefs, context, member))
					ASTOBJ_CONTAINER_LINK(&gtalk_list, member);
				ASTOBJ_UNREF(member, gtalk_member_destroy);
			}
		}
		cat = tris_category_browse(cfg, cat);
	}
	gtalk_free_candidates(global_candidates);
	return 1;
}

/*! \brief Load module into PBX, register channel */
static int load_module(void)
{
	char *jabber_loaded = tris_module_helper("", "res_jabber.so", 0, 0, 0, 0);
	free(jabber_loaded);
	if (!jabber_loaded) {
		/* If embedded, check for a different module name */
		jabber_loaded = tris_module_helper("", "res_jabber", 0, 0, 0, 0);
		free(jabber_loaded);
		if (!jabber_loaded) {
			tris_log(LOG_ERROR, "chan_gtalk.so depends upon res_jabber.so\n");
			return TRIS_MODULE_LOAD_DECLINE;
		}
	}

	ASTOBJ_CONTAINER_INIT(&gtalk_list);
	if (!gtalk_load_config()) {
		tris_log(LOG_ERROR, "Unable to read config file %s. Not loading module.\n", GOOGLE_CONFIG);
		return 0;
	}

	sched = sched_context_create();
	if (!sched) 
		tris_log(LOG_WARNING, "Unable to create schedule context\n");

	io = io_context_create();
	if (!io) 
		tris_log(LOG_WARNING, "Unable to create I/O context\n");

	if (tris_find_ourip(&__ourip, bindaddr)) {
		tris_log(LOG_WARNING, "Unable to get own IP address, Gtalk disabled\n");
		return 0;
	}

	tris_rtp_proto_register(&gtalk_rtp);
	tris_cli_register_multiple(gtalk_cli, ARRAY_LEN(gtalk_cli));

	/* Make sure we can register our channel type */
	if (tris_channel_register(&gtalk_tech)) {
		tris_log(LOG_ERROR, "Unable to register channel class %s\n", gtalk_tech.type);
		return -1;
	}
	return 0;
}

/*! \brief Reload module */
static int reload(void)
{
	return 0;
}

/*! \brief Unload the gtalk channel from Trismedia */
static int unload_module(void)
{
	struct gtalk_pvt *privates = NULL;
	tris_cli_unregister_multiple(gtalk_cli, ARRAY_LEN(gtalk_cli));
	/* First, take us out of the channel loop */
	tris_channel_unregister(&gtalk_tech);
	tris_rtp_proto_unregister(&gtalk_rtp);

	if (!tris_mutex_lock(&gtalklock)) {
		/* Hangup all interfaces if they have an owner */
		ASTOBJ_CONTAINER_TRAVERSE(&gtalk_list, 1, {
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
		tris_mutex_unlock(&gtalklock);
	} else {
		tris_log(LOG_WARNING, "Unable to lock the monitor\n");
		return -1;
	}
	ASTOBJ_CONTAINER_DESTROYALL(&gtalk_list, gtalk_member_destroy);
	ASTOBJ_CONTAINER_DESTROY(&gtalk_list);
	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Gtalk Channel Driver",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
