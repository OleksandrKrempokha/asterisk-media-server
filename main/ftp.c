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

/*! 
 * \file 
 *
 * \brief Supports FTP and FTCP with Symmetric FTP support for NAT traversal.
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \note FTP is defined in RFC 3550.
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 241721 $")

#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <math.h> 

#include "trismedia/ftp.h"
#include "trismedia/pbx.h"
#include "trismedia/frame.h"
#include "trismedia/channel.h"
#include "trismedia/acl.h"
#include "trismedia/config.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"
#include "trismedia/netsock.h"
#include "trismedia/cli.h"
#include "trismedia/manager.h"
#include "trismedia/unaligned.h"

#define MAX_TIMESTAMP_SKEW	640

#define FTP_SEQ_MOD     (1<<16) 	/*!< A sequence number can't be more than 16 bits */
#define FTCP_DEFAULT_INTERVALMS   5000	/*!< Default milli-seconds between FTCP reports we send */
#define FTCP_MIN_INTERVALMS       500	/*!< Min milli-seconds between FTCP reports we send */
#define FTCP_MAX_INTERVALMS       60000	/*!< Max milli-seconds between FTCP reports we send */

#define FTCP_PT_FUR     192
#define FTCP_PT_SR      200
#define FTCP_PT_RR      201
#define FTCP_PT_SDES    202
#define FTCP_PT_BYE     203
#define FTCP_PT_APP     204

#define FTP_MTU		1200

#define DEFAULT_DTMF_TIMEOUT (150 * (8000 / 1000))	/*!< samples */

static int dtmftimeout = DEFAULT_DTMF_TIMEOUT;

static int ftpstart = 5000;     /*!< First port for FTP sessions (set in rtp.conf) */
static int ftpend = 31000;      /*!< Last port for FTP sessions (set in rtp.conf) */
static int ftpdebug;			/*!< Are we debugging? */
static int ftcpdebug;			/*!< Are we debugging FTCP? */
static int ftcpstats;			/*!< Are we debugging FTCP? */
static int ftcpinterval = FTCP_DEFAULT_INTERVALMS; /*!< Time between ftcp reports in millisecs */
static int stundebug;			/*!< Are we debugging stun? */
static struct sockaddr_in ftpdebugaddr;	/*!< Debug packets to/from this host */
static struct sockaddr_in ftcpdebugaddr;	/*!< Debug FTCP packets to/from this host */
#ifdef SO_NO_CHECK
static int nochecksums;
#endif
static int strictftp;

enum strict_ftp_state {
	STRICT_FTP_OPEN = 0, /*! No FTP packets should be dropped, all sources accepted */
	STRICT_FTP_LEARN,    /*! Accept next packet as source */
	STRICT_FTP_CLOSED,   /*! Drop all FTP packets not coming from source that was learned */
};

/* Uncomment this to enable more intense native bridging, but note: this is currently buggy */
/* #define P2P_INTENSE */

/*!
 * \brief Structure representing a FTP session.
 *
 * FTP session is defined on page 9 of RFC 3550: "An association among a set of participants communicating with FTP.  A participant may be involved in multiple FTP sessions at the same time [...]"
 *
 */

/*! \brief FTP session description */
struct tris_ftp {
	int s;
	struct tris_frame f;
	unsigned char rawdata[8192 + TRIS_FRIENDLY_OFFSET];
	unsigned int ssrc;		/*!< Synchronization source, RFC 3550, page 10. */
	unsigned int themssrc;		/*!< Their SSRC */
	unsigned int rxssrc;
	unsigned int lastts;
	unsigned int lastrxts;
	unsigned int lastividtimestamp;
	unsigned int lastovidtimestamp;
	unsigned int lastitexttimestamp;
	unsigned int lastotexttimestamp;
	unsigned int lasteventseqn;
	int lastrxseqno;                /*!< Last received sequence number */
	unsigned short seedrxseqno;     /*!< What sequence number did they start with?*/
	unsigned int seedrxts;          /*!< What FTP timestamp did they start with? */
	unsigned int rxcount;           /*!< How many packets have we received? */
	unsigned int rxoctetcount;      /*!< How many octets have we received? should be rxcount *160*/
	unsigned int txcount;           /*!< How many packets have we sent? */
	unsigned int txoctetcount;      /*!< How many octets have we sent? (txcount*160)*/
	unsigned int cycles;            /*!< Shifted count of sequence number cycles */
	double rxjitter;                /*!< Interarrival jitter at the moment */
	double rxtransit;               /*!< Relative transit time for previous packet */
	int lasttxformat;
	int lastrxformat;

	int ftptimeout;			/*!< FTP timeout time (negative or zero means disabled, negative value means temporarily disabled) */
	int ftpholdtimeout;		/*!< FTP timeout when on hold (negative or zero means disabled, negative value means temporarily disabled). */
	int ftpkeepalive;		/*!< Send FTP comfort noice packets for keepalive */

	int connection;
	/* DTMF Reception Variables */
	char resp;
	unsigned int lastevent;
	unsigned int dtmf_duration;     /*!< Total duration in samples since the digit start event */
	unsigned int dtmf_timeout;      /*!< When this timestamp is reached we consider END frame lost and forcibly abort digit */
	unsigned int dtmfsamples;
	/* DTMF Transmission Variables */
	unsigned int lastdigitts;
	char sending_digit;	/*!< boolean - are we sending digits */
	char send_digit;	/*!< digit we are sending */
	int send_payload;
	int send_duration;
	int nat;
	unsigned int flags;
	struct sockaddr_in us;		/*!< Socket representation of the local endpoint. */
	struct sockaddr_in them;	/*!< Socket representation of the remote endpoint. */
	struct sockaddr_in altthem;	/*!< Alternate source of remote media */
	struct timeval rxcore;
	struct timeval txcore;
	double drxcore;                 /*!< The double representation of the first received packet */
	struct timeval lastrx;          /*!< timeval when we last received a packet */
	struct timeval dtmfmute;
	struct tris_smoother *smoother;
	int *ioid;
	unsigned short seqno;		/*!< Sequence number, RFC 3550, page 13. */
	unsigned short rxseqno;
	struct sched_context *sched;
	struct io_context *io;
	void *data;
	tris_ftp_callback callback;
#ifdef P2P_INTENSE
	tris_mutex_t bridge_lock;
#endif
	struct ftpPayloadType current_FTP_PT[MAX_FTP_PT];
	int ftp_lookup_code_cache_isAstFormat; /*!< a cache for the result of ftp_lookup_code(): */
	int ftp_lookup_code_cache_code;
	int ftp_lookup_code_cache_result;
	struct tris_ftcp *ftcp;
	struct tris_codec_pref pref;
	struct tris_ftp *bridged;        /*!< Who we are Packet bridged to */

	enum strict_ftp_state strict_ftp_state; /*!< Current state that strict FTP protection is in */
	struct sockaddr_in strict_ftp_address;  /*!< Remote address information for strict FTP purposes */

	int set_marker_bit:1;           /*!< Whether to set the marker bit or not */
	unsigned int constantssrc:1;
	struct ftp_red *red;
};

static struct tris_frame *red_t140_to_red(struct ftp_red *red);
static int red_write(const void *data);
 
struct ftp_red {
	struct tris_frame t140;  /*!< Primary data  */
	struct tris_frame t140red;   /*!< Redundant t140*/
	unsigned char pt[RED_MAX_GENERATION];  /*!< Payload types for redundancy data */
	unsigned char ts[RED_MAX_GENERATION]; /*!< Time stamps */
	unsigned char len[RED_MAX_GENERATION]; /*!< length of each generation */
	int num_gen; /*!< Number of generations */
	int schedid; /*!< Timer id */
	int ti; /*!< How long to buffer data before send */
	unsigned char t140red_data[64000];  
	unsigned char buf_data[64000]; /*!< buffered primary data */
	int hdrlen; 
	long int prev_ts;
};

/* Forward declarations */
static int tris_ftcp_write(const void *data);
static void timeval2ntp(struct timeval tv, unsigned int *msw, unsigned int *lsw);
static int tris_ftcp_write_sr(const void *data);
static int tris_ftcp_write_rr(const void *data);
static unsigned int tris_ftcp_calc_interval(struct tris_ftp *ftp);
static int tris_ftp_senddigit_continuation(struct tris_ftp *ftp);
int tris_ftp_senddigit_end(struct tris_ftp *ftp, char digit);

#define FLAG_3389_WARNING		(1 << 0)
#define FLAG_NAT_ACTIVE			(3 << 1)
#define FLAG_NAT_INACTIVE		(0 << 1)
#define FLAG_NAT_INACTIVE_NOWARN	(1 << 1)
#define FLAG_HAS_DTMF			(1 << 3)
#define FLAG_P2P_SENT_MARK              (1 << 4)
#define FLAG_P2P_NEED_DTMF              (1 << 5)
#define FLAG_CALLBACK_MODE              (1 << 6)
#define FLAG_DTMF_COMPENSATE            (1 << 7)
#define FLAG_HAS_STUN                   (1 << 8)

/*!
 * \brief Structure defining an FTCP session.
 * 
 * The concept "FTCP session" is not defined in RFC 3550, but since 
 * this structure is analogous to tris_ftp, which tracks a FTP session, 
 * it is logical to think of this as a FTCP session.
 *
 * FTCP packet is defined on page 9 of RFC 3550.
 * 
 */
struct tris_ftcp {
	int ftcp_info;
	int s;				/*!< Socket */
	struct sockaddr_in us;		/*!< Socket representation of the local endpoint. */
	struct sockaddr_in them;	/*!< Socket representation of the remote endpoint. */
	struct sockaddr_in altthem;	/*!< Alternate source for FTCP */
	unsigned int soc;		/*!< What they told us */
	unsigned int spc;		/*!< What they told us */
	unsigned int themrxlsr;		/*!< The middle 32 bits of the NTP timestamp in the last received SR*/
	struct timeval rxlsr;		/*!< Time when we got their last SR */
	struct timeval txlsr;		/*!< Time when we sent or last SR*/
	unsigned int expected_prior;	/*!< no. packets in previous interval */
	unsigned int received_prior;	/*!< no. packets received in previous interval */
	int schedid;			/*!< Schedid returned from tris_sched_add() to schedule FTCP-transmissions*/
	unsigned int rr_count;		/*!< number of RRs we've sent, not including report blocks in SR's */
	unsigned int sr_count;		/*!< number of SRs we've sent */
	unsigned int lastsrtxcount;     /*!< Transmit packet count when last SR sent */
	double accumulated_transit;	/*!< accumulated a-dlsr-lsr */
	double rtt;			/*!< Last reported rtt */
	unsigned int reported_jitter;	/*!< The contents of their last jitter entry in the RR */
	unsigned int reported_lost;	/*!< Reported lost packets in their RR */
	char quality[TRIS_MAX_USER_FIELD];
	char quality_jitter[TRIS_MAX_USER_FIELD];
	char quality_loss[TRIS_MAX_USER_FIELD];
	char quality_rtt[TRIS_MAX_USER_FIELD];

	double reported_maxjitter;
	double reported_minjitter;
	double reported_normdev_jitter;
	double reported_stdev_jitter;
	unsigned int reported_jitter_count;

	double reported_maxlost;
	double reported_minlost;
	double reported_normdev_lost;
	double reported_stdev_lost;

	double rxlost;
	double maxrxlost;
	double minrxlost;
	double normdev_rxlost;
	double stdev_rxlost;
	unsigned int rxlost_count;

	double maxrxjitter;
	double minrxjitter;
	double normdev_rxjitter;
	double stdev_rxjitter;
	unsigned int rxjitter_count;
	double maxrtt;
	double minrtt;
	double normdevrtt;
	double stdevrtt;
	unsigned int rtt_count;
	int sendfur;
};

/*!
 * \brief STUN support code
 *
 * This code provides some support for doing STUN transactions.
 * Eventually it should be moved elsewhere as other protocols
 * than FTP can benefit from it - e.g. SIP.
 * STUN is described in RFC3489 and it is based on the exchange
 * of UDP packets between a client and one or more servers to
 * determine the externally visible address (and port) of the client
 * once it has gone through the NAT boxes that connect it to the
 * outside.
 * The simplest request packet is just the header defined in
 * struct stun_header, and from the response we may just look at
 * one attribute, STUN_MAPPED_ADDRESS, that we find in the response.
 * By doing more transactions with different server addresses we
 * may determine more about the behaviour of the NAT boxes, of
 * course - the details are in the RFC.
 *
 * All STUN packets start with a simple header made of a type,
 * length (excluding the header) and a 16-byte random transaction id.
 * Following the header we may have zero or more attributes, each
 * structured as a type, length and a value (whose format depends
 * on the type, but often contains addresses).
 * Of course all fields are in network format.
 */

typedef struct { unsigned int id[4]; } __attribute__((packed)) stun_trans_id;

struct stun_header {
	unsigned short msgtype;
	unsigned short msglen;
	stun_trans_id  id;
	unsigned char ies[0];
} __attribute__((packed));

struct stun_attr {
	unsigned short attr;
	unsigned short len;
	unsigned char value[0];
} __attribute__((packed));

/*
 * The format normally used for addresses carried by STUN messages.
 */
struct stun_addr {
	unsigned char unused;
	unsigned char family;
	unsigned short port;
	unsigned int addr;
} __attribute__((packed));

#define STUN_IGNORE		(0)
#define STUN_ACCEPT		(1)

/*! \brief STUN message types
 * 'BIND' refers to transactions used to determine the externally
 * visible addresses. 'SEC' refers to transactions used to establish
 * a session key for subsequent requests.
 * 'SEC' functionality is not supported here.
 */
 
#define STUN_BINDREQ	0x0001
#define STUN_BINDRESP	0x0101
#define STUN_BINDERR	0x0111
#define STUN_SECREQ	0x0002
#define STUN_SECRESP	0x0102
#define STUN_SECERR	0x0112

/*! \brief Basic attribute types in stun messages.
 * Messages can also contain custom attributes (codes above 0x7fff)
 */
#define STUN_MAPPED_ADDRESS	0x0001
#define STUN_RESPONSE_ADDRESS	0x0002
#define STUN_CHANGE_REQUEST	0x0003
#define STUN_SOURCE_ADDRESS	0x0004
#define STUN_CHANGED_ADDRESS	0x0005
#define STUN_USERNAME		0x0006
#define STUN_PASSWORD		0x0007
#define STUN_MESSAGE_INTEGRITY	0x0008
#define STUN_ERROR_CODE		0x0009
#define STUN_UNKNOWN_ATTRIBUTES	0x000a
#define STUN_REFLECTED_FROM	0x000b

/*! \brief helper function to print message names */
static const char *stun_msg2str(int msg)
{
	switch (msg) {
	case STUN_BINDREQ:
		return "Binding Request";
	case STUN_BINDRESP:
		return "Binding Response";
	case STUN_BINDERR:
		return "Binding Error Response";
	case STUN_SECREQ:
		return "Shared Secret Request";
	case STUN_SECRESP:
		return "Shared Secret Response";
	case STUN_SECERR:
		return "Shared Secret Error Response";
	}
	return "Non-RFC3489 Message";
}

/*! \brief helper function to print attribute names */
static const char *stun_attr2str(int msg)
{
	switch (msg) {
	case STUN_MAPPED_ADDRESS:
		return "Mapped Address";
	case STUN_RESPONSE_ADDRESS:
		return "Response Address";
	case STUN_CHANGE_REQUEST:
		return "Change Request";
	case STUN_SOURCE_ADDRESS:
		return "Source Address";
	case STUN_CHANGED_ADDRESS:
		return "Changed Address";
	case STUN_USERNAME:
		return "Username";
	case STUN_PASSWORD:
		return "Password";
	case STUN_MESSAGE_INTEGRITY:
		return "Message Integrity";
	case STUN_ERROR_CODE:
		return "Error Code";
	case STUN_UNKNOWN_ATTRIBUTES:
		return "Unknown Attributes";
	case STUN_REFLECTED_FROM:
		return "Reflected From";
	}
	return "Non-RFC3489 Attribute";
}

/*! \brief here we store credentials extracted from a message */
struct stun_state {
	const char *username;
	const char *password;
};

static int stun_process_attr(struct stun_state *state, struct stun_attr *attr)
{
	if (stundebug)
		tris_verbose("Found STUN Attribute %s (%04x), length %d\n",
			    stun_attr2str(ntohs(attr->attr)), ntohs(attr->attr), ntohs(attr->len));
	switch (ntohs(attr->attr)) {
	case STUN_USERNAME:
		state->username = (const char *) (attr->value);
		break;
	case STUN_PASSWORD:
		state->password = (const char *) (attr->value);
		break;
	default:
		if (stundebug)
			tris_verbose("Ignoring STUN attribute %s (%04x), length %d\n", 
				    stun_attr2str(ntohs(attr->attr)), ntohs(attr->attr), ntohs(attr->len));
	}
	return 0;
}

/*! \brief append a string to an STUN message */
static void append_attr_string(struct stun_attr **attr, int attrval, const char *s, int *len, int *left)
{
	int size = sizeof(**attr) + strlen(s);
	if (*left > size) {
		(*attr)->attr = htons(attrval);
		(*attr)->len = htons(strlen(s));
		memcpy((*attr)->value, s, strlen(s));
		(*attr) = (struct stun_attr *)((*attr)->value + strlen(s));
		*len += size;
		*left -= size;
	}
}

/*! \brief append an address to an STUN message */
static void append_attr_address(struct stun_attr **attr, int attrval, struct sockaddr_in *sock_in, int *len, int *left)
{
	int size = sizeof(**attr) + 8;
	struct stun_addr *addr;
	if (*left > size) {
		(*attr)->attr = htons(attrval);
		(*attr)->len = htons(8);
		addr = (struct stun_addr *)((*attr)->value);
		addr->unused = 0;
		addr->family = 0x01;
		addr->port = sock_in->sin_port;
		addr->addr = sock_in->sin_addr.s_addr;
		(*attr) = (struct stun_attr *)((*attr)->value + 8);
		*len += size;
		*left -= size;
	}
}

/*! \brief wrapper to send an STUN message */
static int stun_send(int s, struct sockaddr_in *dst, struct stun_header *resp)
{
	return sendto(s, resp, ntohs(resp->msglen) + sizeof(*resp), 0,
		      (struct sockaddr *)dst, sizeof(*dst));
}

/*! \brief helper function to generate a random request id */
static void stun_req_id(struct stun_header *req)
{
	int x;
	for (x = 0; x < 4; x++)
		req->id.id[x] = tris_random();
}

size_t tris_ftp_alloc_size(void)
{
	return sizeof(struct tris_ftp);
}

/*! \brief callback type to be invoked on stun responses. */
typedef int (stun_cb_f)(struct stun_attr *attr, void *arg);

/*! \brief handle an incoming STUN message.
 *
 * Do some basic sanity checks on packet size and content,
 * try to extract a bit of information, and possibly reply.
 * At the moment this only processes BIND requests, and returns
 * the externally visible address of the request.
 * If a callback is specified, invoke it with the attribute.
 */
static int stun_handle_packet(int s, struct sockaddr_in *src,
	unsigned char *data, size_t len, stun_cb_f *stun_cb, void *arg)
{
	struct stun_header *hdr = (struct stun_header *)data;
	struct stun_attr *attr;
	struct stun_state st;
	int ret = STUN_IGNORE;	
	int x;

	/* On entry, 'len' is the length of the udp payload. After the
	 * initial checks it becomes the size of unprocessed options,
	 * while 'data' is advanced accordingly.
	 */
	if (len < sizeof(struct stun_header)) {
		tris_debug(1, "Runt STUN packet (only %d, wanting at least %d)\n", (int) len, (int) sizeof(struct stun_header));
		return -1;
	}
	len -= sizeof(struct stun_header);
	data += sizeof(struct stun_header);
	x = ntohs(hdr->msglen);	/* len as advertised in the message */
	if (stundebug)
		tris_verbose("STUN Packet, msg %s (%04x), length: %d\n", stun_msg2str(ntohs(hdr->msgtype)), ntohs(hdr->msgtype), x);
	if (x > len) {
		tris_debug(1, "Scrambled STUN packet length (got %d, expecting %d)\n", x, (int)len);
	} else
		len = x;
	memset(&st, 0, sizeof(st));
	while (len) {
		if (len < sizeof(struct stun_attr)) {
			tris_debug(1, "Runt Attribute (got %d, expecting %d)\n", (int)len, (int) sizeof(struct stun_attr));
			break;
		}
		attr = (struct stun_attr *)data;
		/* compute total attribute length */
		x = ntohs(attr->len) + sizeof(struct stun_attr);
		if (x > len) {
			tris_debug(1, "Inconsistent Attribute (length %d exceeds remaining msg len %d)\n", x, (int)len);
			break;
		}
		if (stun_cb)
			stun_cb(attr, arg);
		if (stun_process_attr(&st, attr)) {
			tris_debug(1, "Failed to handle attribute %s (%04x)\n", stun_attr2str(ntohs(attr->attr)), ntohs(attr->attr));
			break;
		}
		/* Clear attribute id: in case previous entry was a string,
		 * this will act as the terminator for the string.
		 */
		attr->attr = 0;
		data += x;
		len -= x;
	}
	/* Null terminate any string.
	 * XXX NOTE, we write past the size of the buffer passed by the
	 * caller, so this is potentially dangerous. The only thing that
	 * saves us is that usually we read the incoming message in a
	 * much larger buffer in the struct tris_ftp
	 */
	*data = '\0';

	/* Now prepare to generate a reply, which at the moment is done
	 * only for properly formed (len == 0) STUN_BINDREQ messages.
	 */
	if (len == 0) {
		unsigned char respdata[1024];
		struct stun_header *resp = (struct stun_header *)respdata;
		int resplen = 0;	/* len excluding header */
		int respleft = sizeof(respdata) - sizeof(struct stun_header);

		resp->id = hdr->id;
		resp->msgtype = 0;
		resp->msglen = 0;
		attr = (struct stun_attr *)resp->ies;
		switch (ntohs(hdr->msgtype)) {
		case STUN_BINDREQ:
			if (stundebug)
				tris_verbose("STUN Bind Request, username: %s\n", 
					    st.username ? st.username : "<none>");
			if (st.username)
				append_attr_string(&attr, STUN_USERNAME, st.username, &resplen, &respleft);
			append_attr_address(&attr, STUN_MAPPED_ADDRESS, src, &resplen, &respleft);
			resp->msglen = htons(resplen);
			resp->msgtype = htons(STUN_BINDRESP);
			stun_send(s, src, resp);
			ret = STUN_ACCEPT;
			break;
		default:
			if (stundebug)
				tris_verbose("Dunno what to do with STUN message %04x (%s)\n", ntohs(hdr->msgtype), stun_msg2str(ntohs(hdr->msgtype)));
		}
	}
	return ret;
}

/*! \brief Extract the STUN_MAPPED_ADDRESS from the stun response.
 * This is used as a callback for stun_handle_response
 * when called from tris_stun_request.
 */
static int stun_get_mapped(struct stun_attr *attr, void *arg)
{
	struct stun_addr *addr = (struct stun_addr *)(attr + 1);
	struct sockaddr_in *sa = (struct sockaddr_in *)arg;

	if (ntohs(attr->attr) != STUN_MAPPED_ADDRESS || ntohs(attr->len) != 8)
		return 1;	/* not us. */
	sa->sin_port = addr->port;
	sa->sin_addr.s_addr = addr->addr;
	return 0;
}

/*! \brief Generic STUN request
 * Send a generic stun request to the server specified,
 * possibly waiting for a reply and filling the 'reply' field with
 * the externally visible address. Note that in this case the request
 * will be blocking.
 * (Note, the interface may change slightly in the future).
 *
 * \param s the socket used to send the request
 * \param dst the address of the STUN server
 * \param username if non null, add the username in the request
 * \param answer if non null, the function waits for a response and
 *    puts here the externally visible address.
 * \return 0 on success, other values on error.
 */
int tris_stun_ftp_request(int s, struct sockaddr_in *dst,
	const char *username, struct sockaddr_in *answer)
{
	struct stun_header *req;
	unsigned char reqdata[1024];
	int reqlen, reqleft;
	struct stun_attr *attr;
	int res = 0;
	int retry;
	
	req = (struct stun_header *)reqdata;
	stun_req_id(req);
	reqlen = 0;
	reqleft = sizeof(reqdata) - sizeof(struct stun_header);
	req->msgtype = 0;
	req->msglen = 0;
	attr = (struct stun_attr *)req->ies;
	if (username)
		append_attr_string(&attr, STUN_USERNAME, username, &reqlen, &reqleft);
	req->msglen = htons(reqlen);
	req->msgtype = htons(STUN_BINDREQ);
	for (retry = 0; retry < 3; retry++) {	/* XXX make retries configurable */
		/* send request, possibly wait for reply */
		unsigned char reply_buf[1024];
		fd_set rfds;
		struct timeval to = { 3, 0 };	/* timeout, make it configurable */
		struct sockaddr_in src;
		socklen_t srclen;

		res = stun_send(s, dst, req);
		if (res < 0) {
			tris_log(LOG_WARNING, "tris_stun_request send #%d failed error %d, retry\n",
				retry, res);
			continue;
		}
		if (answer == NULL)
			break;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		res = tris_select(s + 1, &rfds, NULL, NULL, &to);
		if (res <= 0)	/* timeout or error */
			continue;
		memset(&src, '\0', sizeof(src));
		srclen = sizeof(src);
		/* XXX pass -1 in the size, because stun_handle_packet might
		 * write past the end of the buffer.
		 */
		res = recvfrom(s, reply_buf, sizeof(reply_buf) - 1,
			0, (struct sockaddr *)&src, &srclen);
		if (res < 0) {
			tris_log(LOG_WARNING, "tris_stun_request recvfrom #%d failed error %d, retry\n",
				retry, res);
			continue;
		}
		memset(answer, '\0', sizeof(struct sockaddr_in));
		stun_handle_packet(s, &src, reply_buf, res,
			stun_get_mapped, answer);
		res = 0; /* signal regular exit */
		break;
	}
	return res;
}

/*! \brief send a STUN BIND request to the given destination.
 * Optionally, add a username if specified.
 */
void tris_ftp_stun_request(struct tris_ftp *ftp, struct sockaddr_in *suggestion, const char *username)
{
	tris_stun_ftp_request(ftp->s, suggestion, username, NULL);
}

/*! \brief List of current sessions */
static TRIS_RWLIST_HEAD_STATIC(protos, tris_ftp_protocol);

static void timeval2ntp(struct timeval when, unsigned int *msw, unsigned int *lsw)
{
	unsigned int sec, usec, frac;
	sec = when.tv_sec + 2208988800u; /* Sec between 1900 and 1970 */
	usec = when.tv_usec;
	frac = (usec << 12) + (usec << 8) - ((usec * 3650) >> 6);
	*msw = sec;
	*lsw = frac;
}

int tris_ftp_fd(struct tris_ftp *ftp)
{
	return ftp->s;
}

int tris_ftcp_fd(struct tris_ftp *ftp)
{
	if (ftp->ftcp)
		return ftp->ftcp->s;
	return -1;
}

static int ftp_get_rate(int subclass)
{
	return (subclass == TRIS_FORMAT_G722) ? 8000 : tris_format_rate(subclass);
}

unsigned int tris_ftcp_calc_interval(struct tris_ftp *ftp)
{
	unsigned int interval;
	/*! \todo XXX Do a more reasonable calculation on this one
	 * Look in RFC 3550 Section A.7 for an example*/
	interval = ftcpinterval;
	return interval;
}

/* \brief Put FTP timeout timers on hold during another transaction, like T.38 */
void tris_ftp_set_ftptimers_onhold(struct tris_ftp *ftp)
{
	ftp->ftptimeout = (-1) * ftp->ftptimeout;
	ftp->ftpholdtimeout = (-1) * ftp->ftpholdtimeout;
}

/*! \brief Set ftp timeout */
void tris_ftp_set_ftptimeout(struct tris_ftp *ftp, int timeout)
{
	ftp->ftptimeout = timeout;
}

/*! \brief Set ftp hold timeout */
void tris_ftp_set_ftpholdtimeout(struct tris_ftp *ftp, int timeout)
{
	ftp->ftpholdtimeout = timeout;
}

/*! \brief set FTP keepalive interval */
void tris_ftp_set_ftpkeepalive(struct tris_ftp *ftp, int period)
{
	ftp->ftpkeepalive = period;
}

/*! \brief Get ftp timeout */
int tris_ftp_get_ftptimeout(struct tris_ftp *ftp)
{
	if (ftp->ftptimeout < 0)	/* We're not checking, but remembering the setting (during T.38 transmission) */
		return 0;
	return ftp->ftptimeout;
}

/*! \brief Get ftp hold timeout */
int tris_ftp_get_ftpholdtimeout(struct tris_ftp *ftp)
{
	if (ftp->ftptimeout < 0)	/* We're not checking, but remembering the setting (during T.38 transmission) */
		return 0;
	return ftp->ftpholdtimeout;
}

/*! \brief Get FTP keepalive interval */
int tris_ftp_get_ftpkeepalive(struct tris_ftp *ftp)
{
	return ftp->ftpkeepalive;
}

void tris_ftp_set_data(struct tris_ftp *ftp, void *data)
{
	ftp->data = data;
}

void tris_ftp_set_callback(struct tris_ftp *ftp, tris_ftp_callback callback)
{
	ftp->callback = callback;
}

void tris_ftp_setnat(struct tris_ftp *ftp, int nat)
{
	ftp->nat = nat;
}

int tris_ftp_getnat(struct tris_ftp *ftp)
{
	return tris_test_flag(ftp, FLAG_NAT_ACTIVE);
}

void tris_ftp_setdtmf(struct tris_ftp *ftp, int dtmf)
{
	tris_set2_flag(ftp, dtmf ? 1 : 0, FLAG_HAS_DTMF);
}

void tris_ftp_setdtmfcompensate(struct tris_ftp *ftp, int compensate)
{
	tris_set2_flag(ftp, compensate ? 1 : 0, FLAG_DTMF_COMPENSATE);
}

void tris_ftp_setstun(struct tris_ftp *ftp, int stun_enable)
{
	tris_set2_flag(ftp, stun_enable ? 1 : 0, FLAG_HAS_STUN);
}

static void ftp_bridge_lock(struct tris_ftp *ftp)
{
#ifdef P2P_INTENSE
	tris_mutex_lock(&ftp->bridge_lock);
#endif
	return;
}

static void ftp_bridge_unlock(struct tris_ftp *ftp)
{
#ifdef P2P_INTENSE
	tris_mutex_unlock(&ftp->bridge_lock);
#endif
	return;
}

/*! \brief Calculate normal deviation */
static double normdev_compute(double normdev, double sample, unsigned int sample_count)
{
	normdev = normdev * sample_count + sample;
	sample_count++;

	return normdev / sample_count;
}

static double stddev_compute(double stddev, double sample, double normdev, double normdev_curent, unsigned int sample_count)
{
/*
		for the formula check http://www.cs.umd.edu/~austinjp/constSD.pdf
		return sqrt( (sample_count*pow(stddev,2) + sample_count*pow((sample-normdev)/(sample_count+1),2) + pow(sample-normdev_curent,2)) / (sample_count+1));
		we can compute the sigma^2 and that way we would have to do the sqrt only 1 time at the end and would save another pow 2 compute
		optimized formula
*/
#define SQUARE(x) ((x) * (x))

	stddev = sample_count * stddev;
	sample_count++;

	return stddev + 
	       ( sample_count * SQUARE( (sample - normdev) / sample_count ) ) + 
	       ( SQUARE(sample - normdev_curent) / sample_count );

#undef SQUARE
}

static struct tris_frame *send_dtmf(struct tris_ftp *ftp, enum tris_frame_type type)
{
	if (((tris_test_flag(ftp, FLAG_DTMF_COMPENSATE) && type == TRIS_FRAME_DTMF_END) ||
	     (type == TRIS_FRAME_DTMF_BEGIN)) && tris_tvcmp(tris_tvnow(), ftp->dtmfmute) < 0) {
		tris_debug(1, "Ignore potential DTMF echo from '%s'\n", tris_inet_ntoa(ftp->them.sin_addr));
		ftp->resp = 0;
		ftp->dtmfsamples = 0;
		return &tris_null_frame;
	}
	tris_debug(1, "Sending dtmf: %d (%c), at %s\n", ftp->resp, ftp->resp, tris_inet_ntoa(ftp->them.sin_addr));
	if (ftp->resp == 'X') {
		ftp->f.frametype = TRIS_FRAME_CONTROL;
		ftp->f.subclass = TRIS_CONTROL_FLASH;
	} else {
		ftp->f.frametype = type;
		ftp->f.subclass = ftp->resp;
	}
	ftp->f.datalen = 0;
	ftp->f.samples = 0;
	ftp->f.mallocd = 0;
	ftp->f.src = "FTP";
	return &ftp->f;
	
}

static inline int ftp_debug_test_addr(struct sockaddr_in *addr)
{
	if (ftpdebug == 0)
		return 0;
	if (ftpdebugaddr.sin_addr.s_addr) {
		if (((ntohs(ftpdebugaddr.sin_port) != 0)
		     && (ftpdebugaddr.sin_port != addr->sin_port))
		    || (ftpdebugaddr.sin_addr.s_addr != addr->sin_addr.s_addr))
			return 0;
	}
	return 1;
}

static inline int ftcp_debug_test_addr(struct sockaddr_in *addr)
{
	if (ftcpdebug == 0)
		return 0;
	if (ftcpdebugaddr.sin_addr.s_addr) {
		if (((ntohs(ftcpdebugaddr.sin_port) != 0)
		     && (ftcpdebugaddr.sin_port != addr->sin_port))
		    || (ftcpdebugaddr.sin_addr.s_addr != addr->sin_addr.s_addr))
			return 0;
	}
	return 1;
}


static struct tris_frame *process_cisco_dtmf(struct tris_ftp *ftp, unsigned char *data, int len)
{
	unsigned int event;
	char resp = 0;
	struct tris_frame *f = NULL;
	unsigned char seq;
	unsigned int flags;
	unsigned int power;

	/* We should have at least 4 bytes in FTP data */
	if (len < 4)
		return f;

	/*	The format of Cisco FTP DTMF packet looks like next:
		+0				- sequence number of DTMF FTP packet (begins from 1,
						  wrapped to 0)
		+1				- set of flags
		+1 (bit 0)		- flaps by different DTMF digits delimited by audio
						  or repeated digit without audio???
		+2 (+4,+6,...)	- power level? (rises from 0 to 32 at begin of tone
						  then falls to 0 at its end)
		+3 (+5,+7,...)	- detected DTMF digit (0..9,*,#,A-D,...)
		Repeated DTMF information (bytes 4/5, 6/7) is history shifted right
		by each new packet and thus provides some redudancy.
		
		Sample of Cisco FTP DTMF packet is (all data in hex):
			19 07 00 02 12 02 20 02
		showing end of DTMF digit '2'.

		The packets
			27 07 00 02 0A 02 20 02
			28 06 20 02 00 02 0A 02
		shows begin of new digit '2' with very short pause (20 ms) after
		previous digit '2'. Bit +1.0 flips at begin of new digit.
		
		Cisco FTP DTMF packets comes as replacement of audio FTP packets
		so its uses the same sequencing and timestamping rules as replaced
		audio packets. Repeat interval of DTMF packets is 20 ms and not rely
		on audio framing parameters. Marker bit isn't used within stream of
		DTMFs nor audio stream coming immediately after DTMF stream. Timestamps
		are not sequential at borders between DTMF and audio streams,
	*/

	seq = data[0];
	flags = data[1];
	power = data[2];
	event = data[3] & 0x1f;

	if (option_debug > 2 || ftpdebug)
		tris_debug(0, "Cisco DTMF Digit: %02x (len=%d, seq=%d, flags=%02x, power=%d, history count=%d)\n", event, len, seq, flags, power, (len - 4) / 2);
	if (event < 10) {
		resp = '0' + event;
	} else if (event < 11) {
		resp = '*';
	} else if (event < 12) {
		resp = '#';
	} else if (event < 16) {
		resp = 'A' + (event - 12);
	} else if (event < 17) {
		resp = 'X';
	}
	if ((!ftp->resp && power) || (ftp->resp && (ftp->resp != resp))) {
		ftp->resp = resp;
		/* Why we should care on DTMF compensation at reception? */
		if (!tris_test_flag(ftp, FLAG_DTMF_COMPENSATE)) {
			f = send_dtmf(ftp, TRIS_FRAME_DTMF_BEGIN);
			ftp->dtmfsamples = 0;
		}
	} else if ((ftp->resp == resp) && !power) {
		f = send_dtmf(ftp, TRIS_FRAME_DTMF_END);
		f->samples = ftp->dtmfsamples * (ftp_get_rate(f->subclass) / 1000);
		ftp->resp = 0;
	} else if (ftp->resp == resp)
		ftp->dtmfsamples += 20 * (ftp_get_rate(f->subclass) / 1000);
	ftp->dtmf_timeout = dtmftimeout;
	return f;
}

/*! 
 * \brief Process FTP DTMF and events according to RFC 2833.
 * 
 * RFC 2833 is "FTP Payload for DTMF Digits, Telephony Tones and Telephony Signals".
 * 
 * \param ftp
 * \param data
 * \param len
 * \param seqno
 * \param timestamp
 * \returns
 */
static struct tris_frame *process_rfc2833(struct tris_ftp *ftp, unsigned char *data, int len, unsigned int seqno, unsigned int timestamp)
{
	unsigned int event;
	unsigned int event_end;
	unsigned int samples;
	char resp = 0;
	struct tris_frame *f = NULL;

	/* Figure out event, event end, and samples */
	event = ntohl(*((unsigned int *)(data)));
	event >>= 24;
	event_end = ntohl(*((unsigned int *)(data)));
	event_end <<= 8;
	event_end >>= 24;
	samples = ntohl(*((unsigned int *)(data)));
	samples &= 0xFFFF;

	/* Print out debug if turned on */
	if (ftpdebug || option_debug > 2)
		tris_debug(0, "- FTP 2833 Event: %08x (len = %d)\n", event, len);

	/* Figure out what digit was pressed */
	if (event < 10) {
		resp = '0' + event;
	} else if (event < 11) {
		resp = '*';
	} else if (event < 12) {
		resp = '#';
	} else if (event < 16) {
		resp = 'A' + (event - 12);
	} else if (event < 17) {	/* Event 16: Hook flash */
		resp = 'X';	
	} else {
		/* Not a supported event */
		tris_log(LOG_DEBUG, "Ignoring FTP 2833 Event: %08x. Not a DTMF Digit.\n", event);
		return &tris_null_frame;
	}

	if (tris_test_flag(ftp, FLAG_DTMF_COMPENSATE)) {
		if ((ftp->lastevent != timestamp) || (ftp->resp && ftp->resp != resp)) {
			ftp->resp = resp;
			ftp->dtmf_timeout = 0;
			f = send_dtmf(ftp, TRIS_FRAME_DTMF_END);
			f->len = 0;
			ftp->lastevent = timestamp;
		}
	} else {
		/*  The duration parameter measures the complete
		    duration of the event (from the beginning) - RFC2833.
		    Account for the fact that duration is only 16 bits long
		    (about 8 seconds at 8000 Hz) and can wrap is digit
		    is hold for too long. */
		unsigned int new_duration = ftp->dtmf_duration;
		unsigned int last_duration = new_duration & 0xFFFF;

		if (last_duration > 64000 && samples < last_duration)
			new_duration += 0xFFFF + 1;
		new_duration = (new_duration & ~0xFFFF) | samples;

		if (event_end & 0x80) {
			/* End event */
			if ((ftp->lastevent != seqno) && ftp->resp) {
				ftp->dtmf_duration = new_duration;
				f = send_dtmf(ftp, TRIS_FRAME_DTMF_END);
				f->len = tris_tvdiff_ms(tris_samp2tv(ftp->dtmf_duration, ftp_get_rate(f->subclass)), tris_tv(0, 0));
				ftp->resp = 0;
				ftp->dtmf_duration = ftp->dtmf_timeout = 0;
			}
		} else {
			/* Begin/continuation */

			if (ftp->resp && ftp->resp != resp) {
				/* Another digit already began. End it */
				f = send_dtmf(ftp, TRIS_FRAME_DTMF_END);
				f->len = tris_tvdiff_ms(tris_samp2tv(ftp->dtmf_duration, ftp_get_rate(f->subclass)), tris_tv(0, 0));
				ftp->resp = 0;
				ftp->dtmf_duration = ftp->dtmf_timeout = 0;
			}


			if (ftp->resp) {
				/* Digit continues */
				ftp->dtmf_duration = new_duration;
			} else {
				/* New digit began */
				ftp->resp = resp;
				f = send_dtmf(ftp, TRIS_FRAME_DTMF_BEGIN);
				ftp->dtmf_duration = samples;
			}

			ftp->dtmf_timeout = timestamp + ftp->dtmf_duration + dtmftimeout;
		}

		ftp->lastevent = seqno;
	}

	ftp->dtmfsamples = samples;

	return f;
}

/*!
 * \brief Process Comfort Noise FTP.
 * 
 * This is incomplete at the moment.
 * 
*/
static struct tris_frame *process_rfc3389(struct tris_ftp *ftp, unsigned char *data, int len)
{
	struct tris_frame *f = NULL;
	/* Convert comfort noise into audio with various codecs.  Unfortunately this doesn't
	   totally help us out becuase we don't have an engine to keep it going and we are not
	   guaranteed to have it every 20ms or anything */
	if (ftpdebug)
		tris_debug(0, "- FTP 3389 Comfort noise event: Level %d (len = %d)\n", ftp->lastrxformat, len);

	if (!(tris_test_flag(ftp, FLAG_3389_WARNING))) {
		tris_log(LOG_NOTICE, "Comfort noise support incomplete in Trismedia (RFC 3389). Please turn off on client if possible. Client IP: %s\n",
			tris_inet_ntoa(ftp->them.sin_addr));
		tris_set_flag(ftp, FLAG_3389_WARNING);
	}
	
	/* Must have at least one byte */
	if (!len)
		return NULL;
	if (len < 24) {
		ftp->f.data.ptr = ftp->rawdata + TRIS_FRIENDLY_OFFSET;
		ftp->f.datalen = len - 1;
		ftp->f.offset = TRIS_FRIENDLY_OFFSET;
		memcpy(ftp->f.data.ptr, data + 1, len - 1);
	} else {
		ftp->f.data.ptr = NULL;
		ftp->f.offset = 0;
		ftp->f.datalen = 0;
	}
	ftp->f.frametype = TRIS_FRAME_CNG;
	ftp->f.subclass = data[0] & 0x7f;
	ftp->f.samples = 0;
	ftp->f.delivery.tv_usec = ftp->f.delivery.tv_sec = 0;
	f = &ftp->f;
	return f;
}

static int ftpread(int *id, int fd, short events, void *cbdata)
{
	struct tris_ftp *ftp = cbdata;
	struct tris_frame *f;
	f = tris_ftp_read(ftp);
	if (f) {
		if (ftp->callback)
			ftp->callback(ftp, f, ftp->data);
	}
	return 1;
}

struct tris_frame *tris_ftcp_read(struct tris_ftp *ftp)
{
	socklen_t len;
	int position, i, packetwords;
	int res;
	struct sockaddr_in sock_in;
	unsigned int ftcpdata[8192 + TRIS_FRIENDLY_OFFSET];
	unsigned int *ftcpheader;
	int pt;
	struct timeval now;
	unsigned int length;
	int rc;
	double rttsec;
	uint64_t rtt = 0;
	unsigned int dlsr;
	unsigned int lsr;
	unsigned int msw;
	unsigned int lsw;
	unsigned int comp;
	struct tris_frame *f = &tris_null_frame;
	
	double reported_jitter;
	double reported_normdev_jitter_current;
	double normdevrtt_current;
	double reported_lost;
	double reported_normdev_lost_current;

	if (!ftp || !ftp->ftcp)
		return &tris_null_frame;

	len = sizeof(sock_in);
	
	res = recvfrom(ftp->ftcp->s, ftcpdata + TRIS_FRIENDLY_OFFSET, sizeof(ftcpdata) - sizeof(unsigned int) * TRIS_FRIENDLY_OFFSET,
					0, (struct sockaddr *)&sock_in, &len);
	ftcpheader = (unsigned int *)(ftcpdata + TRIS_FRIENDLY_OFFSET);
	
	if (res < 0) {
		tris_assert(errno != EBADF);
		if (errno != EAGAIN) {
			tris_log(LOG_WARNING, "FTCP Read error: %s.  Hanging up.\n", strerror(errno));
			return NULL;
		}
		return &tris_null_frame;
	}

	packetwords = res / 4;
	
	if (ftp->nat) {
		/* Send to whoever sent to us */
		if (((ftp->ftcp->them.sin_addr.s_addr != sock_in.sin_addr.s_addr) ||
		    (ftp->ftcp->them.sin_port != sock_in.sin_port)) && 
		    ((ftp->ftcp->altthem.sin_addr.s_addr != sock_in.sin_addr.s_addr) || 
		    (ftp->ftcp->altthem.sin_port != sock_in.sin_port))) {
			memcpy(&ftp->ftcp->them, &sock_in, sizeof(ftp->ftcp->them));
			if (option_debug || ftpdebug)
				tris_debug(0, "FTCP NAT: Got FTCP from other end. Now sending to address %s:%d\n", tris_inet_ntoa(ftp->ftcp->them.sin_addr), ntohs(ftp->ftcp->them.sin_port));
		}
	}

	tris_debug(1, "Got FTCP report of %d bytes\n", res);

	/* Process a compound packet */
	position = 0;
	while (position < packetwords) {
		i = position;
		length = ntohl(ftcpheader[i]);
		pt = (length & 0xff0000) >> 16;
		rc = (length & 0x1f000000) >> 24;
		length &= 0xffff;
 
		if ((i + length) > packetwords) {
			if (option_debug || ftpdebug)
				tris_log(LOG_DEBUG, "FTCP Read too short\n");
			return &tris_null_frame;
		}
		
		if (ftcp_debug_test_addr(&sock_in)) {
		  	tris_verbose("\n\nGot FTCP from %s:%d\n", tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port));
		  	tris_verbose("PT: %d(%s)\n", pt, (pt == 200) ? "Sender Report" : (pt == 201) ? "Receiver Report" : (pt == 192) ? "H.261 FUR" : "Unknown");
		  	tris_verbose("Reception reports: %d\n", rc);
		  	tris_verbose("SSRC of sender: %u\n", ftcpheader[i + 1]);
		}
 
		i += 2; /* Advance past header and ssrc */
		
		switch (pt) {
		case FTCP_PT_SR:
			gettimeofday(&ftp->ftcp->rxlsr,NULL); /* To be able to populate the dlsr */
			ftp->ftcp->spc = ntohl(ftcpheader[i+3]);
			ftp->ftcp->soc = ntohl(ftcpheader[i + 4]);
			ftp->ftcp->themrxlsr = ((ntohl(ftcpheader[i]) & 0x0000ffff) << 16) | ((ntohl(ftcpheader[i + 1]) & 0xffff0000) >> 16); /* Going to LSR in RR*/
 
			if (ftcp_debug_test_addr(&sock_in)) {
				tris_verbose("NTP timestamp: %lu.%010lu\n", (unsigned long) ntohl(ftcpheader[i]), (unsigned long) ntohl(ftcpheader[i + 1]) * 4096);
				tris_verbose("FTP timestamp: %lu\n", (unsigned long) ntohl(ftcpheader[i + 2]));
				tris_verbose("SPC: %lu\tSOC: %lu\n", (unsigned long) ntohl(ftcpheader[i + 3]), (unsigned long) ntohl(ftcpheader[i + 4]));
			}
			i += 5;
			if (rc < 1)
				break;
			/* Intentional fall through */
		case FTCP_PT_RR:
			/* Don't handle multiple reception reports (rc > 1) yet */
			/* Calculate RTT per RFC */
			gettimeofday(&now, NULL);
			timeval2ntp(now, &msw, &lsw);
			if (ntohl(ftcpheader[i + 4]) && ntohl(ftcpheader[i + 5])) { /* We must have the LSR && DLSR */
				comp = ((msw & 0xffff) << 16) | ((lsw & 0xffff0000) >> 16);
				lsr = ntohl(ftcpheader[i + 4]);
				dlsr = ntohl(ftcpheader[i + 5]);
				rtt = comp - lsr - dlsr;

				/* Convert end to end delay to usec (keeping the calculation in 64bit space)
				   sess->ee_delay = (eedelay * 1000) / 65536; */
				if (rtt < 4294) {
				    rtt = (rtt * 1000000) >> 16;
				} else {
				    rtt = (rtt * 1000) >> 16;
				    rtt *= 1000;
				}
				rtt = rtt / 1000.;
				rttsec = rtt / 1000.;
				ftp->ftcp->rtt = rttsec;

				if (comp - dlsr >= lsr) {
					ftp->ftcp->accumulated_transit += rttsec;

					if (ftp->ftcp->rtt_count == 0) 
						ftp->ftcp->minrtt = rttsec;

					if (ftp->ftcp->maxrtt<rttsec)
						ftp->ftcp->maxrtt = rttsec;

					if (ftp->ftcp->minrtt>rttsec)
						ftp->ftcp->minrtt = rttsec;

					normdevrtt_current = normdev_compute(ftp->ftcp->normdevrtt, rttsec, ftp->ftcp->rtt_count);

					ftp->ftcp->stdevrtt = stddev_compute(ftp->ftcp->stdevrtt, rttsec, ftp->ftcp->normdevrtt, normdevrtt_current, ftp->ftcp->rtt_count);

					ftp->ftcp->normdevrtt = normdevrtt_current;

					ftp->ftcp->rtt_count++;
				} else if (ftcp_debug_test_addr(&sock_in)) {
					tris_verbose("Internal FTCP NTP clock skew detected: "
							   "lsr=%u, now=%u, dlsr=%u (%d:%03dms), "
							   "diff=%d\n",
							   lsr, comp, dlsr, dlsr / 65536,
							   (dlsr % 65536) * 1000 / 65536,
							   dlsr - (comp - lsr));
				}
			}

			ftp->ftcp->reported_jitter = ntohl(ftcpheader[i + 3]);
			reported_jitter = (double) ftp->ftcp->reported_jitter;

			if (ftp->ftcp->reported_jitter_count == 0) 
				ftp->ftcp->reported_minjitter = reported_jitter;

			if (reported_jitter < ftp->ftcp->reported_minjitter) 
				ftp->ftcp->reported_minjitter = reported_jitter;

			if (reported_jitter > ftp->ftcp->reported_maxjitter) 
				ftp->ftcp->reported_maxjitter = reported_jitter;

			reported_normdev_jitter_current = normdev_compute(ftp->ftcp->reported_normdev_jitter, reported_jitter, ftp->ftcp->reported_jitter_count);

			ftp->ftcp->reported_stdev_jitter = stddev_compute(ftp->ftcp->reported_stdev_jitter, reported_jitter, ftp->ftcp->reported_normdev_jitter, reported_normdev_jitter_current, ftp->ftcp->reported_jitter_count);

			ftp->ftcp->reported_normdev_jitter = reported_normdev_jitter_current;

			ftp->ftcp->reported_lost = ntohl(ftcpheader[i + 1]) & 0xffffff;

			reported_lost = (double) ftp->ftcp->reported_lost;

			/* using same counter as for jitter */
			if (ftp->ftcp->reported_jitter_count == 0)
				ftp->ftcp->reported_minlost = reported_lost;

			if (reported_lost < ftp->ftcp->reported_minlost)
				ftp->ftcp->reported_minlost = reported_lost;

			if (reported_lost > ftp->ftcp->reported_maxlost) 
				ftp->ftcp->reported_maxlost = reported_lost;

			reported_normdev_lost_current = normdev_compute(ftp->ftcp->reported_normdev_lost, reported_lost, ftp->ftcp->reported_jitter_count);

			ftp->ftcp->reported_stdev_lost = stddev_compute(ftp->ftcp->reported_stdev_lost, reported_lost, ftp->ftcp->reported_normdev_lost, reported_normdev_lost_current, ftp->ftcp->reported_jitter_count);

			ftp->ftcp->reported_normdev_lost = reported_normdev_lost_current;

			ftp->ftcp->reported_jitter_count++;

			if (ftcp_debug_test_addr(&sock_in)) {
				tris_verbose("  Fraction lost: %ld\n", (((long) ntohl(ftcpheader[i + 1]) & 0xff000000) >> 24));
				tris_verbose("  Packets lost so far: %d\n", ftp->ftcp->reported_lost);
				tris_verbose("  Highest sequence number: %ld\n", (long) (ntohl(ftcpheader[i + 2]) & 0xffff));
				tris_verbose("  Sequence number cycles: %ld\n", (long) (ntohl(ftcpheader[i + 2]) & 0xffff) >> 16);
				tris_verbose("  Interarrival jitter: %u\n", ftp->ftcp->reported_jitter);
				tris_verbose("  Last SR(our NTP): %lu.%010lu\n",(unsigned long) ntohl(ftcpheader[i + 4]) >> 16,((unsigned long) ntohl(ftcpheader[i + 4]) << 16) * 4096);
				tris_verbose("  DLSR: %4.4f (sec)\n",ntohl(ftcpheader[i + 5])/65536.0);
				if (rtt)
					tris_verbose("  RTT: %lu(sec)\n", (unsigned long) rtt);
			}

			if (rtt) {
				manager_event(EVENT_FLAG_REPORTING, "FTCPReceived", "From: %s:%d\r\n"
								    "PT: %d(%s)\r\n"
								    "ReceptionReports: %d\r\n"
								    "SenderSSRC: %u\r\n"
								    "FractionLost: %ld\r\n"
								    "PacketsLost: %d\r\n"
								    "HighestSequence: %ld\r\n"
								    "SequenceNumberCycles: %ld\r\n"
								    "IAJitter: %u\r\n"
								    "LastSR: %lu.%010lu\r\n"
								    "DLSR: %4.4f(sec)\r\n"
								    "RTT: %llu(sec)\r\n",
								    tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port),
								    pt, (pt == 200) ? "Sender Report" : (pt == 201) ? "Receiver Report" : (pt == 192) ? "H.261 FUR" : "Unknown",
								    rc,
								    ftcpheader[i + 1],
								    (((long) ntohl(ftcpheader[i + 1]) & 0xff000000) >> 24),
								    ftp->ftcp->reported_lost,
								    (long) (ntohl(ftcpheader[i + 2]) & 0xffff),
								    (long) (ntohl(ftcpheader[i + 2]) & 0xffff) >> 16,
								    ftp->ftcp->reported_jitter,
								    (unsigned long) ntohl(ftcpheader[i + 4]) >> 16, ((unsigned long) ntohl(ftcpheader[i + 4]) << 16) * 4096,
								    ntohl(ftcpheader[i + 5])/65536.0,
								    (unsigned long long)rtt);
			} else {
				manager_event(EVENT_FLAG_REPORTING, "FTCPReceived", "From: %s:%d\r\n"
								    "PT: %d(%s)\r\n"
								    "ReceptionReports: %d\r\n"
								    "SenderSSRC: %u\r\n"
								    "FractionLost: %ld\r\n"
								    "PacketsLost: %d\r\n"
								    "HighestSequence: %ld\r\n"
								    "SequenceNumberCycles: %ld\r\n"
								    "IAJitter: %u\r\n"
								    "LastSR: %lu.%010lu\r\n"
								    "DLSR: %4.4f(sec)\r\n",
								    tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port),
								    pt, (pt == 200) ? "Sender Report" : (pt == 201) ? "Receiver Report" : (pt == 192) ? "H.261 FUR" : "Unknown",
								    rc,
								    ftcpheader[i + 1],
								    (((long) ntohl(ftcpheader[i + 1]) & 0xff000000) >> 24),
								    ftp->ftcp->reported_lost,
								    (long) (ntohl(ftcpheader[i + 2]) & 0xffff),
								    (long) (ntohl(ftcpheader[i + 2]) & 0xffff) >> 16,
								    ftp->ftcp->reported_jitter,
								    (unsigned long) ntohl(ftcpheader[i + 4]) >> 16,
								    ((unsigned long) ntohl(ftcpheader[i + 4]) << 16) * 4096,
								    ntohl(ftcpheader[i + 5])/65536.0);
			}
			break;
		case FTCP_PT_FUR:
			if (ftcp_debug_test_addr(&sock_in))
				tris_verbose("Received an FTCP Fast Update Request\n");
			ftp->f.frametype = TRIS_FRAME_CONTROL;
			ftp->f.subclass = TRIS_CONTROL_VIDUPDATE;
			ftp->f.datalen = 0;
			ftp->f.samples = 0;
			ftp->f.mallocd = 0;
			ftp->f.src = "FTP";
			f = &ftp->f;
			break;
		case FTCP_PT_SDES:
			if (ftcp_debug_test_addr(&sock_in))
				tris_verbose("Received an SDES from %s:%d\n", tris_inet_ntoa(ftp->ftcp->them.sin_addr), ntohs(ftp->ftcp->them.sin_port));
			break;
		case FTCP_PT_BYE:
			if (ftcp_debug_test_addr(&sock_in))
				tris_verbose("Received a BYE from %s:%d\n", tris_inet_ntoa(ftp->ftcp->them.sin_addr), ntohs(ftp->ftcp->them.sin_port));
			break;
		default:
			tris_debug(1, "Unknown FTCP packet (pt=%d) received from %s:%d\n", pt, tris_inet_ntoa(ftp->ftcp->them.sin_addr), ntohs(ftp->ftcp->them.sin_port));
			break;
		}
		position += (length + 1);
	}
	ftp->ftcp->ftcp_info = 1;	
	return f;
}

static void calc_rxstamp(struct timeval *when, struct tris_ftp *ftp, unsigned int timestamp, int mark)
{
	struct timeval now;
	struct timeval tmp;
	double transit;
	double current_time;
	double d;
	double dtv;
	double prog;
	double normdev_rxjitter_current;
	int rate = ftp_get_rate(ftp->f.subclass);

	if ((!ftp->rxcore.tv_sec && !ftp->rxcore.tv_usec) || mark) {
		gettimeofday(&ftp->rxcore, NULL);
		ftp->drxcore = (double) ftp->rxcore.tv_sec + (double) ftp->rxcore.tv_usec / 1000000;
		/* map timestamp to a real time */
		ftp->seedrxts = timestamp; /* Their FTP timestamp started with this */
		tmp = tris_samp2tv(timestamp, rate);
		ftp->rxcore = tris_tvsub(ftp->rxcore, tmp);
		/* Round to 0.1ms for nice, pretty timestamps */
		ftp->rxcore.tv_usec -= ftp->rxcore.tv_usec % 100;
	}

	gettimeofday(&now,NULL);
	/* rxcore is the mapping between the FTP timestamp and _our_ real time from gettimeofday() */
	tmp = tris_samp2tv(timestamp, rate);
	*when = tris_tvadd(ftp->rxcore, tmp);

	prog = (double)((timestamp-ftp->seedrxts)/(float)(rate));
	dtv = (double)ftp->drxcore + (double)(prog);
	current_time = (double)now.tv_sec + (double)now.tv_usec/1000000;
	transit = current_time - dtv;
	d = transit - ftp->rxtransit;
	ftp->rxtransit = transit;
	if (d<0)
		d=-d;
	ftp->rxjitter += (1./16.) * (d - ftp->rxjitter);

	if (ftp->ftcp) {
		if (ftp->rxjitter > ftp->ftcp->maxrxjitter)
			ftp->ftcp->maxrxjitter = ftp->rxjitter;
		if (ftp->ftcp->rxjitter_count == 1) 
			ftp->ftcp->minrxjitter = ftp->rxjitter;
		if (ftp->rxjitter < ftp->ftcp->minrxjitter)
			ftp->ftcp->minrxjitter = ftp->rxjitter;
			
		normdev_rxjitter_current = normdev_compute(ftp->ftcp->normdev_rxjitter,ftp->rxjitter,ftp->ftcp->rxjitter_count);
		ftp->ftcp->stdev_rxjitter = stddev_compute(ftp->ftcp->stdev_rxjitter,ftp->rxjitter,ftp->ftcp->normdev_rxjitter,normdev_rxjitter_current,ftp->ftcp->rxjitter_count);

		ftp->ftcp->normdev_rxjitter = normdev_rxjitter_current;
		ftp->ftcp->rxjitter_count++;
	}
}

/*! \brief Perform a Packet2Packet FTP write */
static int bridge_p2p_ftp_write(struct tris_ftp *ftp, struct tris_ftp *bridged, unsigned int *ftpheader, int len, int hdrlen)
{
	int res = 0, payload = 0, bridged_payload = 0, mark;
	struct ftpPayloadType ftpPT;
	int reconstruct = ntohl(ftpheader[0]);

	/* Get fields from packet */
	payload = (reconstruct & 0x7f0000) >> 16;
	mark = (((reconstruct & 0x800000) >> 23) != 0);

	/* Check what the payload value should be */
	ftpPT = tris_ftp_lookup_pt(ftp, payload);

	/* If the payload is DTMF, and we are listening for DTMF - then feed it into the core */
	if (tris_test_flag(ftp, FLAG_P2P_NEED_DTMF) && !ftpPT.isAstFormat && ftpPT.code == TRIS_FTP_DTMF)
		return -1;

	/* Otherwise adjust bridged payload to match */
	bridged_payload = tris_ftp_lookup_code(bridged, ftpPT.isAstFormat, ftpPT.code);

	/* If the payload coming in is not one of the negotiated ones then send it to the core, this will cause formats to change and the bridge to break */
	if (!bridged->current_FTP_PT[bridged_payload].code)
		return -1;


	/* If the mark bit has not been sent yet... do it now */
	if (!tris_test_flag(ftp, FLAG_P2P_SENT_MARK)) {
		mark = 1;
		tris_set_flag(ftp, FLAG_P2P_SENT_MARK);
	}

	/* Reconstruct part of the packet */
	reconstruct &= 0xFF80FFFF;
	reconstruct |= (bridged_payload << 16);
	reconstruct |= (mark << 23);
	ftpheader[0] = htonl(reconstruct);

	/* Send the packet back out */
	res = sendto(bridged->s, (void *)ftpheader, len, 0, (struct sockaddr *)&bridged->them, sizeof(bridged->them));
	if (res < 0) {
		if (!bridged->nat || (bridged->nat && (tris_test_flag(bridged, FLAG_NAT_ACTIVE) == FLAG_NAT_ACTIVE))) {
			tris_debug(1, "FTP Transmission error of packet to %s:%d: %s\n", tris_inet_ntoa(bridged->them.sin_addr), ntohs(bridged->them.sin_port), strerror(errno));
		} else if (((tris_test_flag(bridged, FLAG_NAT_ACTIVE) == FLAG_NAT_INACTIVE) || ftpdebug) && !tris_test_flag(bridged, FLAG_NAT_INACTIVE_NOWARN)) {
			if (option_debug || ftpdebug)
				tris_debug(0, "FTP NAT: Can't write FTP to private address %s:%d, waiting for other end to send audio...\n", tris_inet_ntoa(bridged->them.sin_addr), ntohs(bridged->them.sin_port));
			tris_set_flag(bridged, FLAG_NAT_INACTIVE_NOWARN);
		}
		return 0;
	} else if (ftp_debug_test_addr(&bridged->them))
			tris_verbose("Sent FTP P2P packet to %s:%u (type %-2.2d, len %-6.6u)\n", tris_inet_ntoa(bridged->them.sin_addr), ntohs(bridged->them.sin_port), bridged_payload, len - hdrlen);

	return 0;
}

struct tris_frame *tris_ftp_read_orig(struct tris_ftp *ftp)
{
	int res;
	struct sockaddr_in sock_in;
	socklen_t len;
	unsigned int seqno;
	int version;
	int payloadtype;
	int hdrlen = 12;
	int padding;
	int mark;
	int ext;
	int cc;
	unsigned int ssrc;
	unsigned int timestamp;
	unsigned int *ftpheader;
	struct ftpPayloadType ftpPT;
	struct tris_ftp *bridged = NULL;
	int prev_seqno;
	
	/* If time is up, kill it */
	if (ftp->sending_digit)
		tris_ftp_senddigit_continuation(ftp);

	len = sizeof(sock_in);
	
	/* Cache where the header will go */
	res = recvfrom(ftp->s, ftp->rawdata + TRIS_FRIENDLY_OFFSET, sizeof(ftp->rawdata) - TRIS_FRIENDLY_OFFSET,
					0, (struct sockaddr *)&sock_in, &len);

	/* If strict FTP protection is enabled see if we need to learn this address or if the packet should be dropped */
	if (ftp->strict_ftp_state == STRICT_FTP_LEARN) {
		/* Copy over address that this packet was received on */
		memcpy(&ftp->strict_ftp_address, &sock_in, sizeof(ftp->strict_ftp_address));
		/* Now move over to actually protecting the FTP port */
		ftp->strict_ftp_state = STRICT_FTP_CLOSED;
		tris_debug(1, "Learned remote address is %s:%d for strict FTP purposes, now protecting the port.\n", tris_inet_ntoa(ftp->strict_ftp_address.sin_addr), ntohs(ftp->strict_ftp_address.sin_port));
	} else if (ftp->strict_ftp_state == STRICT_FTP_CLOSED) {
		/* If the address we previously learned doesn't match the address this packet came in on simply drop it */
		if ((ftp->strict_ftp_address.sin_addr.s_addr != sock_in.sin_addr.s_addr) || (ftp->strict_ftp_address.sin_port != sock_in.sin_port)) {
			tris_debug(1, "Received FTP packet from %s:%d, dropping due to strict FTP protection. Expected it to be from %s:%d\n", tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port), tris_inet_ntoa(ftp->strict_ftp_address.sin_addr), ntohs(ftp->strict_ftp_address.sin_port));
			return &tris_null_frame;
		}
	}

	ftpheader = (unsigned int *)(ftp->rawdata + TRIS_FRIENDLY_OFFSET);
	if (res < 0) {
		tris_assert(errno != EBADF);
		if (errno != EAGAIN) {
			tris_log(LOG_WARNING, "FTP Read error: %s.  Hanging up.\n", strerror(errno));
			return NULL;
		}
		return &tris_null_frame;
	}
	
	if (res < hdrlen) {
		tris_log(LOG_WARNING, "FTP Read too short\n");
		return &tris_null_frame;
	}

	/* Get fields */
	seqno = ntohl(ftpheader[0]);

	/* Check FTP version */
	version = (seqno & 0xC0000000) >> 30;
	if (!version) {
		/* If the two high bits are 0, this might be a
		 * STUN message, so process it. stun_handle_packet()
		 * answers to requests, and it returns STUN_ACCEPT
		 * if the request is valid.
		 */
		if ((stun_handle_packet(ftp->s, &sock_in, ftp->rawdata + TRIS_FRIENDLY_OFFSET, res, NULL, NULL) == STUN_ACCEPT) &&
			(!ftp->them.sin_port && !ftp->them.sin_addr.s_addr)) {
			memcpy(&ftp->them, &sock_in, sizeof(ftp->them));
		}
		return &tris_null_frame;
	}

#if 0	/* Allow to receive FTP stream with closed transmission path */
	/* If we don't have the other side's address, then ignore this */
	if (!ftp->them.sin_addr.s_addr || !ftp->them.sin_port)
		return &tris_null_frame;
#endif

	/* Send to whoever send to us if NAT is turned on */
	if (ftp->nat) {
		if (((ftp->them.sin_addr.s_addr != sock_in.sin_addr.s_addr) ||
		    (ftp->them.sin_port != sock_in.sin_port)) && 
		    ((ftp->altthem.sin_addr.s_addr != sock_in.sin_addr.s_addr) ||
		    (ftp->altthem.sin_port != sock_in.sin_port))) {
			ftp->them = sock_in;
			if (ftp->ftcp) {
				int h = 0;
				memcpy(&ftp->ftcp->them, &sock_in, sizeof(ftp->ftcp->them));
				h = ntohs(ftp->them.sin_port);
				ftp->ftcp->them.sin_port = htons(h + 1);
			}
			ftp->rxseqno = 0;
			tris_set_flag(ftp, FLAG_NAT_ACTIVE);
			if (option_debug || ftpdebug)
				tris_debug(0, "FTP NAT: Got audio from other end. Now sending to address %s:%d\n", tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port));
		}
	}

	/* If we are bridged to another FTP stream, send direct */
	if ((bridged = tris_ftp_get_bridged(ftp)) && !bridge_p2p_ftp_write(ftp, bridged, ftpheader, res, hdrlen))
		return &tris_null_frame;

	if (version != 2)
		return &tris_null_frame;

	payloadtype = (seqno & 0x7f0000) >> 16;
	padding = seqno & (1 << 29);
	mark = seqno & (1 << 23);
	ext = seqno & (1 << 28);
	cc = (seqno & 0xF000000) >> 24;
	seqno &= 0xffff;
	timestamp = ntohl(ftpheader[1]);
	ssrc = ntohl(ftpheader[2]);
	
	if (!mark && ftp->rxssrc && ftp->rxssrc != ssrc) {
		if (option_debug || ftpdebug)
			tris_debug(0, "Forcing Marker bit, because SSRC has changed\n");
		mark = 1;
	}

	ftp->rxssrc = ssrc;
	
	if (padding) {
		/* Remove padding bytes */
		res -= ftp->rawdata[TRIS_FRIENDLY_OFFSET + res - 1];
	}
	
	if (cc) {
		/* CSRC fields present */
		hdrlen += cc*4;
	}

	if (ext) {
		/* FTP Extension present */
		hdrlen += (ntohl(ftpheader[hdrlen/4]) & 0xffff) << 2;
		hdrlen += 4;
		if (option_debug) {
			int profile;
			profile = (ntohl(ftpheader[3]) & 0xffff0000) >> 16;
			if (profile == 0x505a)
				tris_debug(1, "Found Zfone extension in FTP stream - zftp - not supported.\n");
			else
				tris_debug(1, "Found unknown FTP Extensions %x\n", profile);
		}
	}

	if (res < hdrlen) {
		tris_log(LOG_WARNING, "FTP Read too short (%d, expecting %d)\n", res, hdrlen);
		return &tris_null_frame;
	}

	ftp->rxcount++; /* Only count reasonably valid packets, this'll make the ftcp stats more accurate */

	if (ftp->rxcount==1) {
		/* This is the first FTP packet successfully received from source */
		ftp->seedrxseqno = seqno;
	}

	/* Do not schedule RR if FTCP isn't run */
	if (ftp->ftcp && ftp->ftcp->them.sin_addr.s_addr && ftp->ftcp->schedid < 1) {
		/* Schedule transmission of Receiver Report */
		ftp->ftcp->schedid = tris_sched_add(ftp->sched, tris_ftcp_calc_interval(ftp), tris_ftcp_write, ftp);
	}
	if ((int)ftp->lastrxseqno - (int)seqno  > 100) /* if so it would indicate that the sender cycled; allow for misordering */
		ftp->cycles += FTP_SEQ_MOD;
	
	prev_seqno = ftp->lastrxseqno;

	ftp->lastrxseqno = seqno;
	
	if (!ftp->themssrc)
		ftp->themssrc = ntohl(ftpheader[2]); /* Record their SSRC to put in future RR */
	
	if (ftp_debug_test_addr(&sock_in))
		tris_verbose("Got  FTP packet from    %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u)\n",
			tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port), payloadtype, seqno, timestamp,res - hdrlen);

	ftpPT = tris_ftp_lookup_pt(ftp, payloadtype);
	if (!ftpPT.isAstFormat) {
		struct tris_frame *f = NULL;

		/* This is special in-band data that's not one of our codecs */
		if (ftpPT.code == TRIS_FTP_DTMF) {
			/* It's special -- rfc2833 process it */
			if (ftp_debug_test_addr(&sock_in)) {
				unsigned char *data;
				unsigned int event;
				unsigned int event_end;
				unsigned int duration;
				data = ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen;
				event = ntohl(*((unsigned int *)(data)));
				event >>= 24;
				event_end = ntohl(*((unsigned int *)(data)));
				event_end <<= 8;
				event_end >>= 24;
				duration = ntohl(*((unsigned int *)(data)));
				duration &= 0xFFFF;
				tris_verbose("Got  FTP RFC2833 from   %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u, mark %d, event %08x, end %d, duration %-5.5d) \n", tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port), payloadtype, seqno, timestamp, res - hdrlen, (mark?1:0), event, ((event_end & 0x80)?1:0), duration);
			}
			f = process_rfc2833(ftp, ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen, res - hdrlen, seqno, timestamp);
		} else if (ftpPT.code == TRIS_FTP_CISCO_DTMF) {
			/* It's really special -- process it the Cisco way */
			if (ftp->lastevent <= seqno || (ftp->lastevent >= 65530 && seqno <= 6)) {
				f = process_cisco_dtmf(ftp, ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen, res - hdrlen);
				ftp->lastevent = seqno;
			}
		} else if (ftpPT.code == TRIS_FTP_CN) {
			/* Comfort Noise */
			f = process_rfc3389(ftp, ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen, res - hdrlen);
		} else {
			tris_log(LOG_NOTICE, "Unknown FTP codec %d received from '%s'\n", payloadtype, tris_inet_ntoa(ftp->them.sin_addr));
		}
		return f ? f : &tris_null_frame;
	}
	ftp->lastrxformat = ftp->f.subclass = ftpPT.code;
	ftp->f.frametype = (ftp->f.subclass & TRIS_FORMAT_AUDIO_MASK) ? TRIS_FRAME_VOICE : (ftp->f.subclass & TRIS_FORMAT_VIDEO_MASK) ? TRIS_FRAME_VIDEO : TRIS_FRAME_TEXT;

	ftp->rxseqno = seqno;

	if (ftp->dtmf_timeout && ftp->dtmf_timeout < timestamp) {
		ftp->dtmf_timeout = 0;

		if (ftp->resp) {
			struct tris_frame *f;
			f = send_dtmf(ftp, TRIS_FRAME_DTMF_END);
			f->len = tris_tvdiff_ms(tris_samp2tv(ftp->dtmf_duration, ftp_get_rate(f->subclass)), tris_tv(0, 0));
			ftp->resp = 0;
			ftp->dtmf_timeout = ftp->dtmf_duration = 0;
			return f;
		}
	}

	/* Record received timestamp as last received now */
	ftp->lastrxts = timestamp;

	ftp->f.mallocd = 0;
	ftp->f.datalen = res - hdrlen;
	ftp->f.data.ptr = ftp->rawdata + hdrlen + TRIS_FRIENDLY_OFFSET;
	ftp->f.offset = hdrlen + TRIS_FRIENDLY_OFFSET;
	ftp->f.seqno = seqno;

	if (ftp->f.subclass == TRIS_FORMAT_T140 && (int)seqno - (prev_seqno+1) > 0 && (int)seqno - (prev_seqno+1) < 10) {
		  unsigned char *data = ftp->f.data.ptr;
		  
		  memmove(ftp->f.data.ptr+3, ftp->f.data.ptr, ftp->f.datalen);
		  ftp->f.datalen +=3;
		  *data++ = 0xEF;
		  *data++ = 0xBF;
		  *data = 0xBD;
	}
 
	if (ftp->f.subclass == TRIS_FORMAT_T140RED) {
		unsigned char *data = ftp->f.data.ptr;
		unsigned char *header_end;
		int num_generations;
		int header_length;
		int length;
		int diff =(int)seqno - (prev_seqno+1); /* if diff = 0, no drop*/
		int x;

		ftp->f.subclass = TRIS_FORMAT_T140;
		header_end = memchr(data, ((*data) & 0x7f), ftp->f.datalen);
		if (header_end == NULL) {
			return &tris_null_frame;
		}
		header_end++;
		
		header_length = header_end - data;
		num_generations = header_length / 4;
		length = header_length;

		if (!diff) {
			for (x = 0; x < num_generations; x++)
				length += data[x * 4 + 3];
			
			if (!(ftp->f.datalen - length))
				return &tris_null_frame;
			
			ftp->f.data.ptr += length;
			ftp->f.datalen -= length;
		} else if (diff > num_generations && diff < 10) {
			length -= 3;
			ftp->f.data.ptr += length;
			ftp->f.datalen -= length;
			
			data = ftp->f.data.ptr;
			*data++ = 0xEF;
			*data++ = 0xBF;
			*data = 0xBD;
		} else 	{
			for ( x = 0; x < num_generations - diff; x++) 
				length += data[x * 4 + 3];
			
			ftp->f.data.ptr += length;
			ftp->f.datalen -= length;
		}
	}

	if (ftp->f.subclass & TRIS_FORMAT_AUDIO_MASK) {
		ftp->f.samples = tris_codec_get_samples(&ftp->f);
		if (ftp->f.subclass == TRIS_FORMAT_SLINEAR) 
			tris_frame_byteswap_be(&ftp->f);
		calc_rxstamp(&ftp->f.delivery, ftp, timestamp, mark);
		/* Add timing data to let tris_generic_bridge() put the frame into a jitterbuf */
		tris_set_flag(&ftp->f, TRIS_FRFLAG_HAS_TIMING_INFO);
		ftp->f.ts = timestamp / (ftp_get_rate(ftp->f.subclass) / 1000);
		ftp->f.len = ftp->f.samples / ((tris_format_rate(ftp->f.subclass) / 1000));
	} else if (ftp->f.subclass & TRIS_FORMAT_VIDEO_MASK) {
		/* Video -- samples is # of samples vs. 90000 */
		if (!ftp->lastividtimestamp)
			ftp->lastividtimestamp = timestamp;
		ftp->f.samples = timestamp - ftp->lastividtimestamp;
		ftp->lastividtimestamp = timestamp;
		ftp->f.delivery.tv_sec = 0;
		ftp->f.delivery.tv_usec = 0;
		/* Pass the FTP marker bit as bit 0 in the subclass field.
		 * This is ok because subclass is actually a bitmask, and
		 * the low bits represent audio formats, that are not
		 * involved here since we deal with video.
		 */
		if (mark)
			ftp->f.subclass |= 0x1;
	} else {
		/* TEXT -- samples is # of samples vs. 1000 */
		if (!ftp->lastitexttimestamp)
			ftp->lastitexttimestamp = timestamp;
		ftp->f.samples = timestamp - ftp->lastitexttimestamp;
		ftp->lastitexttimestamp = timestamp;
		ftp->f.delivery.tv_sec = 0;
		ftp->f.delivery.tv_usec = 0;
	}
	ftp->f.src = "FTP";
	return &ftp->f;
}

struct tris_frame *tris_ftp_read(struct tris_ftp *ftp)
{
	int res;
	
	/* Cache where the header will go */
	if (!ftp->connection)
		return &tris_null_frame;

	res = recv(ftp->s, ftp->rawdata + TRIS_FRIENDLY_OFFSET, sizeof(ftp->rawdata) - TRIS_FRIENDLY_OFFSET, 0);
	if (res <= 0)
		return &tris_null_frame;
	
	ftp->f.frametype = TRIS_FRAME_FILE;
	ftp->f.mallocd = 0;
	ftp->f.datalen = res;
	ftp->f.data.ptr = ftp->rawdata + TRIS_FRIENDLY_OFFSET;
	ftp->f.offset = TRIS_FRIENDLY_OFFSET;
	return &ftp->f;

#if 0	
	res = recvfrom(ftp->s, ftp->rawdata + TRIS_FRIENDLY_OFFSET, sizeof(ftp->rawdata) - TRIS_FRIENDLY_OFFSET,
					0, (struct sockaddr *)&sock_in, &len);

	/* If strict FTP protection is enabled see if we need to learn this address or if the packet should be dropped */
	if (ftp->strict_ftp_state == STRICT_FTP_LEARN) {
		/* Copy over address that this packet was received on */
		memcpy(&ftp->strict_ftp_address, &sock_in, sizeof(ftp->strict_ftp_address));
		/* Now move over to actually protecting the FTP port */
		ftp->strict_ftp_state = STRICT_FTP_CLOSED;
		tris_debug(1, "Learned remote address is %s:%d for strict FTP purposes, now protecting the port.\n", tris_inet_ntoa(ftp->strict_ftp_address.sin_addr), ntohs(ftp->strict_ftp_address.sin_port));
	} else if (ftp->strict_ftp_state == STRICT_FTP_CLOSED) {
		/* If the address we previously learned doesn't match the address this packet came in on simply drop it */
		if ((ftp->strict_ftp_address.sin_addr.s_addr != sock_in.sin_addr.s_addr) || (ftp->strict_ftp_address.sin_port != sock_in.sin_port)) {
			tris_debug(1, "Received FTP packet from %s:%d, dropping due to strict FTP protection. Expected it to be from %s:%d\n", tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port), tris_inet_ntoa(ftp->strict_ftp_address.sin_addr), ntohs(ftp->strict_ftp_address.sin_port));
			return &tris_null_frame;
		}
	}

	ftpheader = (unsigned int *)(ftp->rawdata + TRIS_FRIENDLY_OFFSET);
	if (res < 0) {
		tris_assert(errno != EBADF);
		if (errno != EAGAIN) {
			tris_log(LOG_WARNING, "FTP Read error: %s.  Hanging up.\n", strerror(errno));
			return NULL;
		}
		return &tris_null_frame;
	}
	
	if (res < hdrlen) {
		tris_log(LOG_WARNING, "FTP Read too short\n");
		return &tris_null_frame;
	}

	/* Get fields */
	seqno = ntohl(ftpheader[0]);

	/* Check FTP version */
	version = (seqno & 0xC0000000) >> 30;
	if (!version) {
		/* If the two high bits are 0, this might be a
		 * STUN message, so process it. stun_handle_packet()
		 * answers to requests, and it returns STUN_ACCEPT
		 * if the request is valid.
		 */
		if ((stun_handle_packet(ftp->s, &sock_in, ftp->rawdata + TRIS_FRIENDLY_OFFSET, res, NULL, NULL) == STUN_ACCEPT) &&
			(!ftp->them.sin_port && !ftp->them.sin_addr.s_addr)) {
			memcpy(&ftp->them, &sock_in, sizeof(ftp->them));
		}
		return &tris_null_frame;
	}

#if 0	/* Allow to receive FTP stream with closed transmission path */
	/* If we don't have the other side's address, then ignore this */
	if (!ftp->them.sin_addr.s_addr || !ftp->them.sin_port)
		return &tris_null_frame;
#endif

	/* Send to whoever send to us if NAT is turned on */
	if (ftp->nat) {
		if (((ftp->them.sin_addr.s_addr != sock_in.sin_addr.s_addr) ||
		    (ftp->them.sin_port != sock_in.sin_port)) && 
		    ((ftp->altthem.sin_addr.s_addr != sock_in.sin_addr.s_addr) ||
		    (ftp->altthem.sin_port != sock_in.sin_port))) {
			ftp->them = sock_in;
			if (ftp->ftcp) {
				int h = 0;
				memcpy(&ftp->ftcp->them, &sock_in, sizeof(ftp->ftcp->them));
				h = ntohs(ftp->them.sin_port);
				ftp->ftcp->them.sin_port = htons(h + 1);
			}
			ftp->rxseqno = 0;
			tris_set_flag(ftp, FLAG_NAT_ACTIVE);
			if (option_debug || ftpdebug)
				tris_debug(0, "FTP NAT: Got audio from other end. Now sending to address %s:%d\n", tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port));
		}
	}

	/* If we are bridged to another FTP stream, send direct */
	if ((bridged = tris_ftp_get_bridged(ftp)) && !bridge_p2p_ftp_write(ftp, bridged, ftpheader, res, hdrlen))
		return &tris_null_frame;

	if (version != 2)
		return &tris_null_frame;

	payloadtype = (seqno & 0x7f0000) >> 16;
	padding = seqno & (1 << 29);
	mark = seqno & (1 << 23);
	ext = seqno & (1 << 28);
	cc = (seqno & 0xF000000) >> 24;
	seqno &= 0xffff;
	timestamp = ntohl(ftpheader[1]);
	ssrc = ntohl(ftpheader[2]);
	
	if (!mark && ftp->rxssrc && ftp->rxssrc != ssrc) {
		if (option_debug || ftpdebug)
			tris_debug(0, "Forcing Marker bit, because SSRC has changed\n");
		mark = 1;
	}

	ftp->rxssrc = ssrc;
	
	if (padding) {
		/* Remove padding bytes */
		res -= ftp->rawdata[TRIS_FRIENDLY_OFFSET + res - 1];
	}
	
	if (cc) {
		/* CSRC fields present */
		hdrlen += cc*4;
	}

	if (ext) {
		/* FTP Extension present */
		hdrlen += (ntohl(ftpheader[hdrlen/4]) & 0xffff) << 2;
		hdrlen += 4;
		if (option_debug) {
			int profile;
			profile = (ntohl(ftpheader[3]) & 0xffff0000) >> 16;
			if (profile == 0x505a)
				tris_debug(1, "Found Zfone extension in FTP stream - zftp - not supported.\n");
			else
				tris_debug(1, "Found unknown FTP Extensions %x\n", profile);
		}
	}

	if (res < hdrlen) {
		tris_log(LOG_WARNING, "FTP Read too short (%d, expecting %d)\n", res, hdrlen);
		return &tris_null_frame;
	}

	ftp->rxcount++; /* Only count reasonably valid packets, this'll make the ftcp stats more accurate */

	if (ftp->rxcount==1) {
		/* This is the first FTP packet successfully received from source */
		ftp->seedrxseqno = seqno;
	}

	/* Do not schedule RR if FTCP isn't run */
	if (ftp->ftcp && ftp->ftcp->them.sin_addr.s_addr && ftp->ftcp->schedid < 1) {
		/* Schedule transmission of Receiver Report */
		ftp->ftcp->schedid = tris_sched_add(ftp->sched, tris_ftcp_calc_interval(ftp), tris_ftcp_write, ftp);
	}
	if ((int)ftp->lastrxseqno - (int)seqno  > 100) /* if so it would indicate that the sender cycled; allow for misordering */
		ftp->cycles += FTP_SEQ_MOD;
	
	prev_seqno = ftp->lastrxseqno;

	ftp->lastrxseqno = seqno;
	
	if (!ftp->themssrc)
		ftp->themssrc = ntohl(ftpheader[2]); /* Record their SSRC to put in future RR */
	
	if (ftp_debug_test_addr(&sock_in))
		tris_verbose("Got  FTP packet from    %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u)\n",
			tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port), payloadtype, seqno, timestamp,res - hdrlen);

	ftpPT = tris_ftp_lookup_pt(ftp, payloadtype);
	if (!ftpPT.isAstFormat) {
		struct tris_frame *f = NULL;

		/* This is special in-band data that's not one of our codecs */
		if (ftpPT.code == TRIS_FTP_DTMF) {
			/* It's special -- rfc2833 process it */
			if (ftp_debug_test_addr(&sock_in)) {
				unsigned char *data;
				unsigned int event;
				unsigned int event_end;
				unsigned int duration;
				data = ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen;
				event = ntohl(*((unsigned int *)(data)));
				event >>= 24;
				event_end = ntohl(*((unsigned int *)(data)));
				event_end <<= 8;
				event_end >>= 24;
				duration = ntohl(*((unsigned int *)(data)));
				duration &= 0xFFFF;
				tris_verbose("Got  FTP RFC2833 from   %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u, mark %d, event %08x, end %d, duration %-5.5d) \n", tris_inet_ntoa(sock_in.sin_addr), ntohs(sock_in.sin_port), payloadtype, seqno, timestamp, res - hdrlen, (mark?1:0), event, ((event_end & 0x80)?1:0), duration);
			}
			f = process_rfc2833(ftp, ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen, res - hdrlen, seqno, timestamp);
		} else if (ftpPT.code == TRIS_FTP_CISCO_DTMF) {
			/* It's really special -- process it the Cisco way */
			if (ftp->lastevent <= seqno || (ftp->lastevent >= 65530 && seqno <= 6)) {
				f = process_cisco_dtmf(ftp, ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen, res - hdrlen);
				ftp->lastevent = seqno;
			}
		} else if (ftpPT.code == TRIS_FTP_CN) {
			/* Comfort Noise */
			f = process_rfc3389(ftp, ftp->rawdata + TRIS_FRIENDLY_OFFSET + hdrlen, res - hdrlen);
		} else {
			tris_log(LOG_NOTICE, "Unknown FTP codec %d received from '%s'\n", payloadtype, tris_inet_ntoa(ftp->them.sin_addr));
		}
		return f ? f : &tris_null_frame;
	}
	ftp->lastrxformat = ftp->f.subclass = ftpPT.code;
	ftp->f.frametype = (ftp->f.subclass & TRIS_FORMAT_AUDIO_MASK) ? TRIS_FRAME_VOICE : (ftp->f.subclass & TRIS_FORMAT_VIDEO_MASK) ? TRIS_FRAME_VIDEO : TRIS_FRAME_TEXT;

	ftp->rxseqno = seqno;

	if (ftp->dtmf_timeout && ftp->dtmf_timeout < timestamp) {
		ftp->dtmf_timeout = 0;

		if (ftp->resp) {
			struct tris_frame *f;
			f = send_dtmf(ftp, TRIS_FRAME_DTMF_END);
			f->len = tris_tvdiff_ms(tris_samp2tv(ftp->dtmf_duration, ftp_get_rate(f->subclass)), tris_tv(0, 0));
			ftp->resp = 0;
			ftp->dtmf_timeout = ftp->dtmf_duration = 0;
			return f;
		}
	}

	/* Record received timestamp as last received now */
	ftp->lastrxts = timestamp;

	ftp->f.mallocd = 0;
	ftp->f.datalen = res - hdrlen;
	ftp->f.data.ptr = ftp->rawdata + hdrlen + TRIS_FRIENDLY_OFFSET;
	ftp->f.offset = hdrlen + TRIS_FRIENDLY_OFFSET;
	ftp->f.seqno = seqno;

	if (ftp->f.subclass == TRIS_FORMAT_T140 && (int)seqno - (prev_seqno+1) > 0 && (int)seqno - (prev_seqno+1) < 10) {
		  unsigned char *data = ftp->f.data.ptr;
		  
		  memmove(ftp->f.data.ptr+3, ftp->f.data.ptr, ftp->f.datalen);
		  ftp->f.datalen +=3;
		  *data++ = 0xEF;
		  *data++ = 0xBF;
		  *data = 0xBD;
	}
 
	if (ftp->f.subclass == TRIS_FORMAT_T140RED) {
		unsigned char *data = ftp->f.data.ptr;
		unsigned char *header_end;
		int num_generations;
		int header_length;
		int length;
		int diff =(int)seqno - (prev_seqno+1); /* if diff = 0, no drop*/
		int x;

		ftp->f.subclass = TRIS_FORMAT_T140;
		header_end = memchr(data, ((*data) & 0x7f), ftp->f.datalen);
		if (header_end == NULL) {
			return &tris_null_frame;
		}
		header_end++;
		
		header_length = header_end - data;
		num_generations = header_length / 4;
		length = header_length;

		if (!diff) {
			for (x = 0; x < num_generations; x++)
				length += data[x * 4 + 3];
			
			if (!(ftp->f.datalen - length))
				return &tris_null_frame;
			
			ftp->f.data.ptr += length;
			ftp->f.datalen -= length;
		} else if (diff > num_generations && diff < 10) {
			length -= 3;
			ftp->f.data.ptr += length;
			ftp->f.datalen -= length;
			
			data = ftp->f.data.ptr;
			*data++ = 0xEF;
			*data++ = 0xBF;
			*data = 0xBD;
		} else 	{
			for ( x = 0; x < num_generations - diff; x++) 
				length += data[x * 4 + 3];
			
			ftp->f.data.ptr += length;
			ftp->f.datalen -= length;
		}
	}

	if (ftp->f.subclass & TRIS_FORMAT_AUDIO_MASK) {
		ftp->f.samples = tris_codec_get_samples(&ftp->f);
		if (ftp->f.subclass == TRIS_FORMAT_SLINEAR) 
			tris_frame_byteswap_be(&ftp->f);
		calc_rxstamp(&ftp->f.delivery, ftp, timestamp, mark);
		/* Add timing data to let tris_generic_bridge() put the frame into a jitterbuf */
		tris_set_flag(&ftp->f, TRIS_FRFLAG_HAS_TIMING_INFO);
		ftp->f.ts = timestamp / (ftp_get_rate(ftp->f.subclass) / 1000);
		ftp->f.len = ftp->f.samples / ((tris_format_rate(ftp->f.subclass) / 1000));
	} else if (ftp->f.subclass & TRIS_FORMAT_VIDEO_MASK) {
		/* Video -- samples is # of samples vs. 90000 */
		if (!ftp->lastividtimestamp)
			ftp->lastividtimestamp = timestamp;
		ftp->f.samples = timestamp - ftp->lastividtimestamp;
		ftp->lastividtimestamp = timestamp;
		ftp->f.delivery.tv_sec = 0;
		ftp->f.delivery.tv_usec = 0;
		/* Pass the FTP marker bit as bit 0 in the subclass field.
		 * This is ok because subclass is actually a bitmask, and
		 * the low bits represent audio formats, that are not
		 * involved here since we deal with video.
		 */
		if (mark)
			ftp->f.subclass |= 0x1;
	} else {
		/* TEXT -- samples is # of samples vs. 1000 */
		if (!ftp->lastitexttimestamp)
			ftp->lastitexttimestamp = timestamp;
		ftp->f.samples = timestamp - ftp->lastitexttimestamp;
		ftp->lastitexttimestamp = timestamp;
		ftp->f.delivery.tv_sec = 0;
		ftp->f.delivery.tv_usec = 0;
	}
	ftp->f.src = "FTP";
	return &ftp->f;
#endif
}

/* The following array defines the MIME Media type (and subtype) for each
   of our codecs, or FTP-specific data type. */
static const struct mimeType {
	struct ftpPayloadType payloadType;
	char *type;
	char *subtype;
	unsigned int sample_rate;
} mimeTypes[] = {
	{{1, TRIS_FORMAT_G723_1}, "audio", "G723", 8000},
	{{1, TRIS_FORMAT_GSM}, "audio", "GSM", 8000},
	{{1, TRIS_FORMAT_ULAW}, "audio", "PCMU", 8000},
	{{1, TRIS_FORMAT_ULAW}, "audio", "G711U", 8000},
	{{1, TRIS_FORMAT_ALAW}, "audio", "PCMA", 8000},
	{{1, TRIS_FORMAT_ALAW}, "audio", "G711A", 8000},
	{{1, TRIS_FORMAT_G726}, "audio", "G726-32", 8000},
	{{1, TRIS_FORMAT_ADPCM}, "audio", "DVI4", 8000},
	{{1, TRIS_FORMAT_SLINEAR}, "audio", "L16", 8000},
	{{1, TRIS_FORMAT_LPC10}, "audio", "LPC", 8000},
	{{1, TRIS_FORMAT_G729A}, "audio", "G729", 8000},
	{{1, TRIS_FORMAT_G729A}, "audio", "G729A", 8000},
	{{1, TRIS_FORMAT_G729A}, "audio", "G.729", 8000},
	{{1, TRIS_FORMAT_SPEEX}, "audio", "speex", 8000},
	{{1, TRIS_FORMAT_ILBC}, "audio", "iLBC", 8000},
	/* this is the sample rate listed in the FTP profile for the G.722
	   codec, *NOT* the actual sample rate of the media stream
	*/
	{{1, TRIS_FORMAT_G722}, "audio", "G722", 8000},
	{{1, TRIS_FORMAT_G726_AAL2}, "audio", "AAL2-G726-32", 8000},
	{{0, TRIS_FTP_DTMF}, "audio", "telephone-event", 8000},
	{{0, TRIS_FTP_CISCO_DTMF}, "audio", "cisco-telephone-event", 8000},
	{{0, TRIS_FTP_CN}, "audio", "CN", 8000},
	{{1, TRIS_FORMAT_JPEG}, "video", "JPEG", 90000},
	{{1, TRIS_FORMAT_PNG}, "video", "PNG", 90000},
	{{1, TRIS_FORMAT_H261}, "video", "H261", 90000},
	{{1, TRIS_FORMAT_H263}, "video", "H263", 90000},
	{{1, TRIS_FORMAT_H263_PLUS}, "video", "h263-1998", 90000},
	{{1, TRIS_FORMAT_H264}, "video", "H264", 90000},
	{{1, TRIS_FORMAT_MP4_VIDEO}, "video", "MP4V-ES", 90000},
	{{1, TRIS_FORMAT_T140RED}, "text", "RED", 1000},
	{{1, TRIS_FORMAT_T140}, "text", "T140", 1000},
	{{1, TRIS_FORMAT_SIREN7}, "audio", "G7221", 16000},
	{{1, TRIS_FORMAT_SIREN14}, "audio", "G7221", 32000},
};

/*! 
 * \brief Mapping between Trismedia codecs and ftp payload types
 *
 * Static (i.e., well-known) FTP payload types for our "TRIS_FORMAT..."s:
 * also, our own choices for dynamic payload types.  This is our master
 * table for transmission 
 * 
 * See http://www.iana.org/assignments/ftp-parameters for a list of
 * assigned values
 */
static const struct ftpPayloadType static_FTP_PT[MAX_FTP_PT] = {
	[0] = {1, TRIS_FORMAT_ULAW},
#ifdef USE_DEPRECATED_G726
	[2] = {1, TRIS_FORMAT_G726}, /* Technically this is G.721, but if Cisco can do it, so can we... */
#endif
	[3] = {1, TRIS_FORMAT_GSM},
	[4] = {1, TRIS_FORMAT_G723_1},
	[5] = {1, TRIS_FORMAT_ADPCM}, /* 8 kHz */
	[6] = {1, TRIS_FORMAT_ADPCM}, /* 16 kHz */
	[7] = {1, TRIS_FORMAT_LPC10},
	[8] = {1, TRIS_FORMAT_ALAW},
	[9] = {1, TRIS_FORMAT_G722},
	[10] = {1, TRIS_FORMAT_SLINEAR}, /* 2 channels */
	[11] = {1, TRIS_FORMAT_SLINEAR}, /* 1 channel */
	[13] = {0, TRIS_FTP_CN},
	[16] = {1, TRIS_FORMAT_ADPCM}, /* 11.025 kHz */
	[17] = {1, TRIS_FORMAT_ADPCM}, /* 22.050 kHz */
	[18] = {1, TRIS_FORMAT_G729A},
	[19] = {0, TRIS_FTP_CN},		/* Also used for CN */
	[26] = {1, TRIS_FORMAT_JPEG},
	[31] = {1, TRIS_FORMAT_H261},
	[34] = {1, TRIS_FORMAT_H263},
	[97] = {1, TRIS_FORMAT_ILBC},
	[98] = {1, TRIS_FORMAT_H263_PLUS},
	[99] = {1, TRIS_FORMAT_H264},
	[101] = {0, TRIS_FTP_DTMF},
	[102] = {1, TRIS_FORMAT_SIREN7},
	[103] = {1, TRIS_FORMAT_H263_PLUS},
	[104] = {1, TRIS_FORMAT_MP4_VIDEO},
	[105] = {1, TRIS_FORMAT_T140RED},	/* Real time text chat (with redundancy encoding) */
	[106] = {1, TRIS_FORMAT_T140},	/* Real time text chat */
	[110] = {1, TRIS_FORMAT_SPEEX},
	[111] = {1, TRIS_FORMAT_G726},
	[112] = {1, TRIS_FORMAT_G726_AAL2},
	[115] = {1, TRIS_FORMAT_SIREN14},
	[121] = {0, TRIS_FTP_CISCO_DTMF}, /* Must be type 121 */
};

void tris_ftp_pt_clear(struct tris_ftp* ftp) 
{
	int i;

	if (!ftp)
		return;

	ftp_bridge_lock(ftp);

	for (i = 0; i < MAX_FTP_PT; ++i) {
		ftp->current_FTP_PT[i].isAstFormat = 0;
		ftp->current_FTP_PT[i].code = 0;
	}

	ftp->ftp_lookup_code_cache_isAstFormat = 0;
	ftp->ftp_lookup_code_cache_code = 0;
	ftp->ftp_lookup_code_cache_result = 0;

	ftp_bridge_unlock(ftp);
}

void tris_ftp_pt_default(struct tris_ftp* ftp) 
{
	int i;

	ftp_bridge_lock(ftp);

	/* Initialize to default payload types */
	for (i = 0; i < MAX_FTP_PT; ++i) {
		ftp->current_FTP_PT[i].isAstFormat = static_FTP_PT[i].isAstFormat;
		ftp->current_FTP_PT[i].code = static_FTP_PT[i].code;
	}

	ftp->ftp_lookup_code_cache_isAstFormat = 0;
	ftp->ftp_lookup_code_cache_code = 0;
	ftp->ftp_lookup_code_cache_result = 0;

	ftp_bridge_unlock(ftp);
}

void tris_ftp_pt_copy(struct tris_ftp *dest, struct tris_ftp *src)
{
	unsigned int i;

	ftp_bridge_lock(dest);
	ftp_bridge_lock(src);

	for (i = 0; i < MAX_FTP_PT; ++i) {
		dest->current_FTP_PT[i].isAstFormat = 
			src->current_FTP_PT[i].isAstFormat;
		dest->current_FTP_PT[i].code = 
			src->current_FTP_PT[i].code; 
	}
	dest->ftp_lookup_code_cache_isAstFormat = 0;
	dest->ftp_lookup_code_cache_code = 0;
	dest->ftp_lookup_code_cache_result = 0;

	ftp_bridge_unlock(src);
	ftp_bridge_unlock(dest);
}

/*! \brief Get channel driver interface structure */
static struct tris_ftp_protocol *get_proto(struct tris_channel *chan)
{
	struct tris_ftp_protocol *cur = NULL;

	TRIS_RWLIST_RDLOCK(&protos);
	TRIS_RWLIST_TRAVERSE(&protos, cur, list) {
		if (cur->type == chan->tech->type)
			break;
	}
	TRIS_RWLIST_UNLOCK(&protos);

	return cur;
}

int tris_ftp_early_bridge(struct tris_channel *c0, struct tris_channel *c1)
{
	struct tris_ftp *destp = NULL, *srcp = NULL;		/* Audio FTP Channels */
	struct tris_ftp *vdestp = NULL, *vsrcp = NULL;		/* Video FTP channels */
	struct tris_ftp *tdestp = NULL, *tsrcp = NULL;		/* Text FTP channels */
	struct tris_ftp_protocol *destpr = NULL, *srcpr = NULL;
	enum tris_ftp_get_result audio_dest_res = TRIS_FTP_GET_FAILED, video_dest_res = TRIS_FTP_GET_FAILED, text_dest_res = TRIS_FTP_GET_FAILED;
	enum tris_ftp_get_result audio_src_res = TRIS_FTP_GET_FAILED, video_src_res = TRIS_FTP_GET_FAILED, text_src_res = TRIS_FTP_GET_FAILED;
	int srccodec, destcodec, nat_active = 0;

	/* Lock channels */
	tris_channel_lock(c0);
	if (c1) {
		while (tris_channel_trylock(c1)) {
			tris_channel_unlock(c0);
			usleep(1);
			tris_channel_lock(c0);
		}
	}

	/* Find channel driver interfaces */
	destpr = get_proto(c0);
	if (c1)
		srcpr = get_proto(c1);
	if (!destpr) {
		tris_debug(1, "Channel '%s' has no FTP, not doing anything\n", c0->name);
		tris_channel_unlock(c0);
		if (c1)
			tris_channel_unlock(c1);
		return -1;
	}
	if (!srcpr) {
		tris_debug(1, "Channel '%s' has no FTP, not doing anything\n", c1 ? c1->name : "<unspecified>");
		tris_channel_unlock(c0);
		if (c1)
			tris_channel_unlock(c1);
		return -1;
	}

	/* Get audio, video  and text interface (if native bridge is possible) */
	audio_dest_res = destpr->get_ftp_info(c0, &destp);
	video_dest_res = destpr->get_vftp_info ? destpr->get_vftp_info(c0, &vdestp) : TRIS_FTP_GET_FAILED;
	text_dest_res = destpr->get_tftp_info ? destpr->get_tftp_info(c0, &tdestp) : TRIS_FTP_GET_FAILED;
	if (srcpr) {
		audio_src_res = srcpr->get_ftp_info(c1, &srcp);
		video_src_res = srcpr->get_vftp_info ? srcpr->get_vftp_info(c1, &vsrcp) : TRIS_FTP_GET_FAILED;
		text_src_res = srcpr->get_tftp_info ? srcpr->get_tftp_info(c1, &tsrcp) : TRIS_FTP_GET_FAILED;
	}

	/* Check if bridge is still possible (In SIP directmedia=no stops this, like NAT) */
	if (audio_dest_res != TRIS_FTP_TRY_NATIVE || (video_dest_res != TRIS_FTP_GET_FAILED && video_dest_res != TRIS_FTP_TRY_NATIVE)) {
		/* Somebody doesn't want to play... */
		tris_channel_unlock(c0);
		if (c1)
			tris_channel_unlock(c1);
		return -1;
	}
	if (audio_src_res == TRIS_FTP_TRY_NATIVE && (video_src_res == TRIS_FTP_GET_FAILED || video_src_res == TRIS_FTP_TRY_NATIVE) && srcpr->get_codec)
		srccodec = srcpr->get_codec(c1);
	else
		srccodec = 0;
	if (audio_dest_res == TRIS_FTP_TRY_NATIVE && (video_dest_res == TRIS_FTP_GET_FAILED || video_dest_res == TRIS_FTP_TRY_NATIVE) && destpr->get_codec)
		destcodec = destpr->get_codec(c0);
	else
		destcodec = 0;
	/* Ensure we have at least one matching codec */
	if (srcp && !(srccodec & destcodec)) {
		tris_channel_unlock(c0);
		tris_channel_unlock(c1);
		return 0;
	}
	/* Consider empty media as non-existent */
	if (audio_src_res == TRIS_FTP_TRY_NATIVE && !srcp->them.sin_addr.s_addr)
		srcp = NULL;
	if (srcp && (srcp->nat || tris_test_flag(srcp, FLAG_NAT_ACTIVE)))
		nat_active = 1;
	/* Bridge media early */
	if (destpr->set_ftp_peer(c0, srcp, vsrcp, tsrcp, srccodec, nat_active))
		tris_log(LOG_WARNING, "Channel '%s' failed to setup early bridge to '%s'\n", c0->name, c1 ? c1->name : "<unspecified>");
	tris_channel_unlock(c0);
	if (c1)
		tris_channel_unlock(c1);
	tris_debug(1, "Setting early bridge SDP of '%s' with that of '%s'\n", c0->name, c1 ? c1->name : "<unspecified>");
	return 0;
}

int tris_ftp_make_compatible(struct tris_channel *dest, struct tris_channel *src, int media)
{
	struct tris_ftp *destp = NULL, *srcp = NULL;		/* Audio FTP Channels */
	struct tris_ftp *vdestp = NULL, *vsrcp = NULL;		/* Video FTP channels */
	struct tris_ftp *tdestp = NULL, *tsrcp = NULL;		/* Text FTP channels */
	struct tris_ftp_protocol *destpr = NULL, *srcpr = NULL;
	enum tris_ftp_get_result audio_dest_res = TRIS_FTP_GET_FAILED, video_dest_res = TRIS_FTP_GET_FAILED, text_dest_res = TRIS_FTP_GET_FAILED;
	enum tris_ftp_get_result audio_src_res = TRIS_FTP_GET_FAILED, video_src_res = TRIS_FTP_GET_FAILED, text_src_res = TRIS_FTP_GET_FAILED; 
	int srccodec, destcodec;

	/* Lock channels */
	tris_channel_lock(dest);
	while (tris_channel_trylock(src)) {
		tris_channel_unlock(dest);
		usleep(1);
		tris_channel_lock(dest);
	}

	/* Find channel driver interfaces */
	if (!(destpr = get_proto(dest))) {
		tris_debug(1, "Channel '%s' has no FTP, not doing anything\n", dest->name);
		tris_channel_unlock(dest);
		tris_channel_unlock(src);
		return 0;
	}
	if (!(srcpr = get_proto(src))) {
		tris_debug(1, "Channel '%s' has no FTP, not doing anything\n", src->name);
		tris_channel_unlock(dest);
		tris_channel_unlock(src);
		return 0;
	}

	/* Get audio and video interface (if native bridge is possible) */
	audio_dest_res = destpr->get_ftp_info(dest, &destp);
	video_dest_res = destpr->get_vftp_info ? destpr->get_vftp_info(dest, &vdestp) : TRIS_FTP_GET_FAILED;
	text_dest_res = destpr->get_tftp_info ? destpr->get_tftp_info(dest, &tdestp) : TRIS_FTP_GET_FAILED;
	audio_src_res = srcpr->get_ftp_info(src, &srcp);
	video_src_res = srcpr->get_vftp_info ? srcpr->get_vftp_info(src, &vsrcp) : TRIS_FTP_GET_FAILED;
	text_src_res = srcpr->get_tftp_info ? srcpr->get_tftp_info(src, &tsrcp) : TRIS_FTP_GET_FAILED;

	/* Ensure we have at least one matching codec */
	if (srcpr->get_codec)
		srccodec = srcpr->get_codec(src);
	else
		srccodec = 0;
	if (destpr->get_codec)
		destcodec = destpr->get_codec(dest);
	else
		destcodec = 0;

	/* Check if bridge is still possible (In SIP directmedia=no stops this, like NAT) */
	if (audio_dest_res != TRIS_FTP_TRY_NATIVE || (video_dest_res != TRIS_FTP_GET_FAILED && video_dest_res != TRIS_FTP_TRY_NATIVE) || audio_src_res != TRIS_FTP_TRY_NATIVE || (video_src_res != TRIS_FTP_GET_FAILED && video_src_res != TRIS_FTP_TRY_NATIVE) || !(srccodec & destcodec)) {
		/* Somebody doesn't want to play... */
		tris_channel_unlock(dest);
		tris_channel_unlock(src);
		return 0;
	}
	tris_ftp_pt_copy(destp, srcp);
	if (vdestp && vsrcp)
		tris_ftp_pt_copy(vdestp, vsrcp);
	if (tdestp && tsrcp)
		tris_ftp_pt_copy(tdestp, tsrcp);
	if (media) {
		/* Bridge early */
		if (destpr->set_ftp_peer(dest, srcp, vsrcp, tsrcp, srccodec, tris_test_flag(srcp, FLAG_NAT_ACTIVE)))
			tris_log(LOG_WARNING, "Channel '%s' failed to setup early bridge to '%s'\n", dest->name, src->name);
	}
	tris_channel_unlock(dest);
	tris_channel_unlock(src);
	tris_debug(1, "Seeded SDP of '%s' with that of '%s'\n", dest->name, src->name);
	return 1;
}

/*! \brief  Make a note of a FTP payload type that was seen in a SDP "m=" line.
 * By default, use the well-known value for this type (although it may 
 * still be set to a different value by a subsequent "a=ftpmap:" line)
 */
void tris_ftp_set_m_type(struct tris_ftp* ftp, int pt) 
{
	if (pt < 0 || pt >= MAX_FTP_PT || static_FTP_PT[pt].code == 0) 
		return; /* bogus payload type */

	ftp_bridge_lock(ftp);
	ftp->current_FTP_PT[pt] = static_FTP_PT[pt];
	ftp_bridge_unlock(ftp);
} 

/*! \brief remove setting from payload type list if the ftpmap header indicates
	an unknown media type */
void tris_ftp_unset_m_type(struct tris_ftp* ftp, int pt) 
{
	if (pt < 0 || pt >= MAX_FTP_PT)
		return; /* bogus payload type */

	ftp_bridge_lock(ftp);
	ftp->current_FTP_PT[pt].isAstFormat = 0;
	ftp->current_FTP_PT[pt].code = 0;
	ftp_bridge_unlock(ftp);
}

/*! \brief Make a note of a FTP payload type (with MIME type) that was seen in
 * an SDP "a=ftpmap:" line.
 * \return 0 if the MIME type was found and set, -1 if it wasn't found
 */
int tris_ftp_set_ftpmap_type_rate(struct tris_ftp *ftp, int pt,
				 char *mimeType, char *mimeSubtype,
				 enum tris_ftp_options options,
				 unsigned int sample_rate)
{
	unsigned int i;
	int found = 0;

	if (pt < 0 || pt >= MAX_FTP_PT)
		return -1; /* bogus payload type */

	ftp_bridge_lock(ftp);

	for (i = 0; i < ARRAY_LEN(mimeTypes); ++i) {
		const struct mimeType *t = &mimeTypes[i];

		if (strcasecmp(mimeSubtype, t->subtype)) {
			continue;
		}

		if (strcasecmp(mimeType, t->type)) {
			continue;
		}

		/* if both sample rates have been supplied, and they don't match,
		   then this not a match; if one has not been supplied, then the
		   rates are not compared */
		if (sample_rate && t->sample_rate &&
		    (sample_rate != t->sample_rate)) {
			continue;
		}

		found = 1;
		ftp->current_FTP_PT[pt] = t->payloadType;

		if ((t->payloadType.code == TRIS_FORMAT_G726) &&
		    t->payloadType.isAstFormat &&
		    (options & TRIS_FTP_OPT_G726_NONSTANDARD)) {
			ftp->current_FTP_PT[pt].code = TRIS_FORMAT_G726_AAL2;
		}

		break;
	}

	ftp_bridge_unlock(ftp);

	return (found ? 0 : -2);
}

int tris_ftp_set_ftpmap_type(struct tris_ftp *ftp, int pt,
			    char *mimeType, char *mimeSubtype,
			    enum tris_ftp_options options)
{
	return tris_ftp_set_ftpmap_type_rate(ftp, pt, mimeType, mimeSubtype, options, 0);
}

/*! \brief Return the union of all of the codecs that were set by ftp_set...() calls 
 * They're returned as two distinct sets: TRIS_FORMATs, and TRIS_FTPs */
void tris_ftp_get_current_formats(struct tris_ftp* ftp,
				 int* astFormats, int* nonAstFormats)
{
	int pt;
	
	ftp_bridge_lock(ftp);
	
	*astFormats = *nonAstFormats = 0;
	for (pt = 0; pt < MAX_FTP_PT; ++pt) {
		if (ftp->current_FTP_PT[pt].isAstFormat) {
			*astFormats |= ftp->current_FTP_PT[pt].code;
		} else {
			*nonAstFormats |= ftp->current_FTP_PT[pt].code;
		}
	}

	ftp_bridge_unlock(ftp);
}

struct ftpPayloadType tris_ftp_lookup_pt(struct tris_ftp* ftp, int pt) 
{
	struct ftpPayloadType result;

	result.isAstFormat = result.code = 0;

	if (pt < 0 || pt >= MAX_FTP_PT) 
		return result; /* bogus payload type */

	/* Start with negotiated codecs */
	ftp_bridge_lock(ftp);
	result = ftp->current_FTP_PT[pt];
	ftp_bridge_unlock(ftp);

	/* If it doesn't exist, check our static FTP type list, just in case */
	if (!result.code) 
		result = static_FTP_PT[pt];

	return result;
}

/*! \brief Looks up an FTP code out of our *static* outbound list */
int tris_ftp_lookup_code(struct tris_ftp* ftp, const int isAstFormat, const int code)
{
	int pt = 0;

	ftp_bridge_lock(ftp);

	if (isAstFormat == ftp->ftp_lookup_code_cache_isAstFormat &&
		code == ftp->ftp_lookup_code_cache_code) {
		/* Use our cached mapping, to avoid the overhead of the loop below */
		pt = ftp->ftp_lookup_code_cache_result;
		ftp_bridge_unlock(ftp);
		return pt;
	}

	/* Check the dynamic list first */
	for (pt = 0; pt < MAX_FTP_PT; ++pt) {
		if (ftp->current_FTP_PT[pt].code == code && ftp->current_FTP_PT[pt].isAstFormat == isAstFormat) {
			ftp->ftp_lookup_code_cache_isAstFormat = isAstFormat;
			ftp->ftp_lookup_code_cache_code = code;
			ftp->ftp_lookup_code_cache_result = pt;
			ftp_bridge_unlock(ftp);
			return pt;
		}
	}

	/* Then the static list */
	for (pt = 0; pt < MAX_FTP_PT; ++pt) {
		if (static_FTP_PT[pt].code == code && static_FTP_PT[pt].isAstFormat == isAstFormat) {
			ftp->ftp_lookup_code_cache_isAstFormat = isAstFormat;
  			ftp->ftp_lookup_code_cache_code = code;
			ftp->ftp_lookup_code_cache_result = pt;
			ftp_bridge_unlock(ftp);
			return pt;
		}
	}

	ftp_bridge_unlock(ftp);

	return -1;
}

const char *tris_ftp_lookup_mime_subtype(const int isAstFormat, const int code,
				  enum tris_ftp_options options)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(mimeTypes); ++i) {
		if ((mimeTypes[i].payloadType.code == code) && (mimeTypes[i].payloadType.isAstFormat == isAstFormat)) {
			if (isAstFormat &&
			    (code == TRIS_FORMAT_G726_AAL2) &&
			    (options & TRIS_FTP_OPT_G726_NONSTANDARD))
				return "G726-32";
			else
				return mimeTypes[i].subtype;
		}
	}

	return "";
}

unsigned int tris_ftp_lookup_sample_rate(int isAstFormat, int code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(mimeTypes); ++i) {
		if ((mimeTypes[i].payloadType.code == code) && (mimeTypes[i].payloadType.isAstFormat == isAstFormat)) {
			return mimeTypes[i].sample_rate;
		}
	}

	return 0;
}

char *tris_ftp_lookup_mime_multiple(char *buf, size_t size, const int capability,
				   const int isAstFormat, enum tris_ftp_options options)
{
	int format;
	unsigned len;
	char *end = buf;
	char *start = buf;

	if (!buf || !size)
		return NULL;

	snprintf(end, size, "0x%x (", capability);

	len = strlen(end);
	end += len;
	size -= len;
	start = end;

	for (format = 1; format < TRIS_FTP_MAX; format <<= 1) {
		if (capability & format) {
			const char *name = tris_ftp_lookup_mime_subtype(isAstFormat, format, options);

			snprintf(end, size, "%s|", name);
			len = strlen(end);
			end += len;
			size -= len;
		}
	}

	if (start == end)
		tris_copy_string(start, "nothing)", size); 
	else if (size > 1)
		*(end -1) = ')';
	
	return buf;
}

/*! \brief Open FTP or FTCP socket for a session.
 * Print a message on failure. 
 */
static int ftp_socket(const char *type)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		if (type == NULL)
			type = "FTP/FTCP";
		tris_log(LOG_WARNING, "Unable to allocate %s socket: %s\n", type, strerror(errno));
	} else {
		long flags = fcntl(s, F_GETFL);
		fcntl(s, F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NO_CHECK
		if (nochecksums)
			setsockopt(s, SOL_SOCKET, SO_NO_CHECK, &nochecksums, sizeof(nochecksums));
#endif
	}
	return s;
}

/*!
 * \brief Initialize a new FTCP session.
 * 
 * \returns The newly initialized FTCP session.
 */
static struct tris_ftcp *tris_ftcp_new(void)
{
	struct tris_ftcp *ftcp;

	if (!(ftcp = tris_calloc(1, sizeof(*ftcp))))
		return NULL;
	ftcp->s = ftp_socket("FTCP");
	ftcp->us.sin_family = AF_INET;
	ftcp->them.sin_family = AF_INET;
	ftcp->schedid = -1;

	if (ftcp->s < 0) {
		tris_free(ftcp);
		return NULL;
	}

	return ftcp;
}

/*!
 * \brief Initialize a new FTP structure.
 *
 */
void tris_ftp_new_init(struct tris_ftp *ftp)
{
#ifdef P2P_INTENSE
	tris_mutex_init(&ftp->bridge_lock);
#endif

	ftp->them.sin_family = AF_INET;
	ftp->us.sin_family = AF_INET;
	ftp->ssrc = tris_random();
	ftp->seqno = tris_random() & 0xffff;
	tris_set_flag(ftp, FLAG_HAS_DTMF);
	ftp->strict_ftp_state = (strictftp ? STRICT_FTP_LEARN : STRICT_FTP_OPEN);
}

struct tris_ftp *tris_ftp_new_with_bindaddr(struct sched_context *sched, struct io_context *io, int ftcpenable, int callbackmode, struct in_addr addr)
{
	struct tris_ftp *ftp;
	int x;
	int staftplace;
	
	if (!(ftp = tris_calloc(1, sizeof(*ftp))))
		return NULL;

	tris_ftp_new_init(ftp);

	ftp->s = ftp_socket("FTP");
	if (ftp->s < 0)
		goto fail;
	if (sched && ftcpenable) {
		ftp->sched = sched;
		ftp->ftcp = tris_ftcp_new();
	}
	
	/*
	 * Try to bind the FTP port, x, and possibly the FTCP port, x+1 as well.
	 * Start from a random (even, by FTP spec) port number, and
	 * iterate until success or no ports are available.
	 * Note that the requirement of FTP port being even, or FTCP being the
	 * next one, cannot be enforced in presence of a NAT box because the
	 * mapping is not under our control.
	 */
	x = (ftpend == ftpstart) ? ftpstart : (tris_random() % (ftpend - ftpstart)) + ftpstart;
	x = x & ~1;		/* make it an even number */
	staftplace = x;		/* remember the starting point */
	/* this is constant across the loop */
	ftp->us.sin_addr = addr;
	if (ftp->ftcp)
		ftp->ftcp->us.sin_addr = addr;
	for (;;) {
		ftp->us.sin_port = htons(x);
		if (!bind(ftp->s, (struct sockaddr *)&ftp->us, sizeof(ftp->us))) {
			/* bind succeeded, if no ftcp then we are done */
			if (!ftp->ftcp)
				break;
			/* have ftcp, try to bind it */
			ftp->ftcp->us.sin_port = htons(x + 1);
			if (!bind(ftp->ftcp->s, (struct sockaddr *)&ftp->ftcp->us, sizeof(ftp->ftcp->us)))
				break;	/* success again, we are really done */
			/*
			 * FTCP bind failed, so close and recreate the
			 * already bound FTP socket for the next round.
			 */
			close(ftp->s);
			ftp->s = ftp_socket("FTP");
			if (ftp->s < 0)
				goto fail;
		}
		/*
		 * If we get here, there was an error in one of the bind()
		 * calls, so make sure it is nothing unexpected.
		 */
		if (errno != EADDRINUSE) {
			/* We got an error that wasn't expected, abort! */
			tris_log(LOG_ERROR, "Unexpected bind error: %s\n", strerror(errno));
			goto fail;
		}
		/*
		 * One of the ports is in use. For the next iteration,
		 * increment by two and handle wraparound.
		 * If we reach the starting point, then declare failure.
		 */
		x += 2;
		if (x > ftpend)
			x = (ftpstart + 1) & ~1;
		if (x == staftplace) {
			tris_log(LOG_ERROR, "No FTP ports remaining. Can't setup media stream for this call.\n");
			goto fail;
		}
	}
	ftp->sched = sched;
	ftp->io = io;
	/*if (callbackmode) {
		ftp->ioid = tris_io_add(ftp->io, ftp->s, ftpread, TRIS_IO_IN, ftp);
		tris_set_flag(ftp, FLAG_CALLBACK_MODE);
	}*/
	tris_ftp_pt_default(ftp);
	return ftp;

fail:
	if (ftp->s >= 0)
		close(ftp->s);
	if (ftp->ftcp) {
		close(ftp->ftcp->s);
		tris_free(ftp->ftcp);
	}
	tris_free(ftp);
	return NULL;
}

struct tris_ftp *tris_ftp_new(struct sched_context *sched, struct io_context *io, int ftcpenable, int callbackmode)
{
	struct in_addr ia;

	memset(&ia, 0, sizeof(ia));
	return tris_ftp_new_with_bindaddr(sched, io, ftcpenable, callbackmode, ia);
}

int tris_ftp_setqos(struct tris_ftp *ftp, int type_of_service, int class_of_service, char *desc)
{
	return tris_netsock_set_qos(ftp->s, type_of_service, class_of_service, desc);
}

void tris_ftp_set_constantssrc(struct tris_ftp *ftp)
{
	ftp->constantssrc = 1;
}

void tris_ftp_new_source(struct tris_ftp *ftp)
{
	if (ftp) {
		ftp->set_marker_bit = 1;
		if (!ftp->constantssrc) {
			ftp->ssrc = tris_random();
		}
	}
}

void tris_ftp_set_peer(struct tris_ftp *ftp, struct sockaddr_in *them)
{
	ftp->them.sin_port = them->sin_port;
	ftp->them.sin_addr = them->sin_addr;
	if (ftp->ftcp) {
		int h = ntohs(them->sin_port);
		ftp->ftcp->them.sin_port = htons(h + 1);
		ftp->ftcp->them.sin_addr = them->sin_addr;
	}
	ftp->rxseqno = 0;
	/* If strict FTP protection is enabled switch back to the learn state so we don't drop packets from above */
	if (strictftp)
		ftp->strict_ftp_state = STRICT_FTP_LEARN;
}

void tris_ftp_set_alt_peer(struct tris_ftp *ftp, struct sockaddr_in *alt)
{
	ftp->altthem.sin_port = alt->sin_port;
	ftp->altthem.sin_addr = alt->sin_addr;
	if (ftp->ftcp) {
		ftp->ftcp->altthem.sin_port = htons(ntohs(alt->sin_port) + 1);
		ftp->ftcp->altthem.sin_addr = alt->sin_addr;
	}
}

int tris_ftp_get_peer(struct tris_ftp *ftp, struct sockaddr_in *them)
{
	if ((them->sin_family != AF_INET) ||
		(them->sin_port != ftp->them.sin_port) ||
		(them->sin_addr.s_addr != ftp->them.sin_addr.s_addr)) {
		them->sin_family = AF_INET;
		them->sin_port = ftp->them.sin_port;
		them->sin_addr = ftp->them.sin_addr;
		return 1;
	}
	return 0;
}

void tris_ftp_get_us(struct tris_ftp *ftp, struct sockaddr_in *us)
{
	*us = ftp->us;
}

struct tris_ftp *tris_ftp_get_bridged(struct tris_ftp *ftp)
{
	struct tris_ftp *bridged = NULL;

	ftp_bridge_lock(ftp);
	bridged = ftp->bridged;
	ftp_bridge_unlock(ftp);

	return bridged;
}

void tris_ftp_stop(struct tris_ftp *ftp)
{
	if (ftp->ftcp) {
		TRIS_SCHED_DEL(ftp->sched, ftp->ftcp->schedid);
	}
	if (ftp->red) {
		TRIS_SCHED_DEL(ftp->sched, ftp->red->schedid);
		free(ftp->red);
		ftp->red = NULL;
	}

	memset(&ftp->them.sin_addr, 0, sizeof(ftp->them.sin_addr));
	memset(&ftp->them.sin_port, 0, sizeof(ftp->them.sin_port));
	if (ftp->ftcp) {
		memset(&ftp->ftcp->them.sin_addr, 0, sizeof(ftp->ftcp->them.sin_addr));
		memset(&ftp->ftcp->them.sin_port, 0, sizeof(ftp->ftcp->them.sin_port));
	}
	
	tris_clear_flag(ftp, FLAG_P2P_SENT_MARK);
}

void tris_ftp_reset(struct tris_ftp *ftp)
{
	memset(&ftp->rxcore, 0, sizeof(ftp->rxcore));
	memset(&ftp->txcore, 0, sizeof(ftp->txcore));
	memset(&ftp->dtmfmute, 0, sizeof(ftp->dtmfmute));
	ftp->lastts = 0;
	ftp->lastdigitts = 0;
	ftp->lastrxts = 0;
	ftp->lastividtimestamp = 0;
	ftp->lastovidtimestamp = 0;
	ftp->lastitexttimestamp = 0;
	ftp->lastotexttimestamp = 0;
	ftp->lasteventseqn = 0;
	ftp->lastevent = 0;
	ftp->lasttxformat = 0;
	ftp->lastrxformat = 0;
	ftp->dtmf_timeout = 0;
	ftp->dtmfsamples = 0;
	ftp->seqno = 0;
	ftp->rxseqno = 0;
}

/*! Get QoS values from FTP and FTCP data (used in "sip show channelstats") */
unsigned int tris_ftp_get_qosvalue(struct tris_ftp *ftp, enum tris_ftp_qos_vars value)
{
	if (ftp == NULL) {
		if (option_debug > 1)
			tris_log(LOG_DEBUG, "NO FTP Structure? Kidding me? \n");
		return 0;
	}
	if (option_debug > 1 && ftp->ftcp == NULL) {
		tris_log(LOG_DEBUG, "NO FTCP structure. Maybe in FTP p2p bridging mode? \n");
	}

	switch (value) {
	case TRIS_FTP_TXCOUNT:
		return (unsigned int) ftp->txcount;
	case TRIS_FTP_RXCOUNT:
		return (unsigned int) ftp->rxcount;
	case TRIS_FTP_TXJITTER:
		return (unsigned int) (ftp->rxjitter * 1000.0);
	case TRIS_FTP_RXJITTER:
		return (unsigned int) (ftp->ftcp ? (ftp->ftcp->reported_jitter / (unsigned int) 65536.0) : 0);
	case TRIS_FTP_RXPLOSS:
		return ftp->ftcp ? (ftp->ftcp->expected_prior - ftp->ftcp->received_prior) : 0;
	case TRIS_FTP_TXPLOSS:
		return ftp->ftcp ? ftp->ftcp->reported_lost : 0;
	case TRIS_FTP_RTT:
		return (unsigned int) (ftp->ftcp ? (ftp->ftcp->rtt * 100) : 0);
	}
	return 0;	/* To make the compiler happy */
}

static double __tris_ftp_get_qos(struct tris_ftp *ftp, const char *qos, int *found)
{
	*found = 1;

	if (!strcasecmp(qos, "remote_maxjitter"))
		return ftp->ftcp->reported_maxjitter * 1000.0;
	if (!strcasecmp(qos, "remote_minjitter"))
		return ftp->ftcp->reported_minjitter * 1000.0;
	if (!strcasecmp(qos, "remote_normdevjitter"))
		return ftp->ftcp->reported_normdev_jitter * 1000.0;
	if (!strcasecmp(qos, "remote_stdevjitter"))
		return sqrt(ftp->ftcp->reported_stdev_jitter) * 1000.0;

	if (!strcasecmp(qos, "local_maxjitter"))
		return ftp->ftcp->maxrxjitter * 1000.0;
	if (!strcasecmp(qos, "local_minjitter"))
		return ftp->ftcp->minrxjitter * 1000.0;
	if (!strcasecmp(qos, "local_normdevjitter"))
		return ftp->ftcp->normdev_rxjitter * 1000.0;
	if (!strcasecmp(qos, "local_stdevjitter"))
		return sqrt(ftp->ftcp->stdev_rxjitter) * 1000.0;

	if (!strcasecmp(qos, "maxrtt"))
		return ftp->ftcp->maxrtt * 1000.0;
	if (!strcasecmp(qos, "minrtt"))
		return ftp->ftcp->minrtt * 1000.0;
	if (!strcasecmp(qos, "normdevrtt"))
		return ftp->ftcp->normdevrtt * 1000.0;
	if (!strcasecmp(qos, "stdevrtt"))
		return sqrt(ftp->ftcp->stdevrtt) * 1000.0;

	*found = 0;

	return 0.0;
}

int tris_ftp_get_qos(struct tris_ftp *ftp, const char *qos, char *buf, unsigned int buflen)
{
	double value;
	int found;

	value = __tris_ftp_get_qos(ftp, qos, &found);

	if (!found)
		return -1;

	snprintf(buf, buflen, "%.0lf", value);

	return 0;
}

void tris_ftp_set_vars(struct tris_channel *chan, struct tris_ftp *ftp) {
	char *audioqos;
	char *audioqos_jitter;
	char *audioqos_loss;
	char *audioqos_rtt;
	struct tris_channel *bridge;

	if (!ftp || !chan)
		return;

	bridge = tris_bridged_channel(chan);

	audioqos        = tris_ftp_get_quality(ftp, NULL, FTPQOS_SUMMARY);
	audioqos_jitter = tris_ftp_get_quality(ftp, NULL, FTPQOS_JITTER);
	audioqos_loss   = tris_ftp_get_quality(ftp, NULL, FTPQOS_LOSS);
	audioqos_rtt    = tris_ftp_get_quality(ftp, NULL, FTPQOS_RTT);

	pbx_builtin_setvar_helper(chan, "FTPAUDIOQOS", audioqos);
	pbx_builtin_setvar_helper(chan, "FTPAUDIOQOSJITTER", audioqos_jitter);
	pbx_builtin_setvar_helper(chan, "FTPAUDIOQOSLOSS", audioqos_loss);
	pbx_builtin_setvar_helper(chan, "FTPAUDIOQOSRTT", audioqos_rtt);

	if (!bridge)
		return;

	pbx_builtin_setvar_helper(bridge, "FTPAUDIOQOSBRIDGED", audioqos);
	pbx_builtin_setvar_helper(bridge, "FTPAUDIOQOSJITTERBRIDGED", audioqos_jitter);
	pbx_builtin_setvar_helper(bridge, "FTPAUDIOQOSLOSSBRIDGED", audioqos_loss);
	pbx_builtin_setvar_helper(bridge, "FTPAUDIOQOSRTTBRIDGED", audioqos_rtt);
}

static char *__tris_ftp_get_quality_jitter(struct tris_ftp *ftp)
{
	/*
	*ssrc          our ssrc
	*themssrc      their ssrc
	*lp            lost packets
	*rxjitter      our calculated jitter(rx)
	*rxcount       no. received packets
	*txjitter      reported jitter of the other end
	*txcount       transmitted packets
	*rlp           remote lost packets
	*rtt           round trip time
	*/
#define FTCP_JITTER_FORMAT1 \
	"minrxjitter=%f;" \
	"maxrxjitter=%f;" \
	"avgrxjitter=%f;" \
	"stdevrxjitter=%f;" \
	"reported_minjitter=%f;" \
	"reported_maxjitter=%f;" \
	"reported_avgjitter=%f;" \
	"reported_stdevjitter=%f;"

#define FTCP_JITTER_FORMAT2 \
	"rxjitter=%f;"

	if (ftp->ftcp && ftp->ftcp->ftcp_info) {
		snprintf(ftp->ftcp->quality_jitter, sizeof(ftp->ftcp->quality_jitter), FTCP_JITTER_FORMAT1,
			ftp->ftcp->minrxjitter,
			ftp->ftcp->maxrxjitter,
			ftp->ftcp->normdev_rxjitter,
			sqrt(ftp->ftcp->stdev_rxjitter),
			ftp->ftcp->reported_minjitter,
			ftp->ftcp->reported_maxjitter,
			ftp->ftcp->reported_normdev_jitter,
			sqrt(ftp->ftcp->reported_stdev_jitter)
		);
	} else {
		snprintf(ftp->ftcp->quality_jitter, sizeof(ftp->ftcp->quality_jitter), FTCP_JITTER_FORMAT2,
			ftp->rxjitter
		);
	}

	return ftp->ftcp->quality_jitter;

#undef FTCP_JITTER_FORMAT1
#undef FTCP_JITTER_FORMAT2
}

static char *__tris_ftp_get_quality_loss(struct tris_ftp *ftp)
{
	unsigned int lost;
	unsigned int extended;
	unsigned int expected;
	int fraction;

#define FTCP_LOSS_FORMAT1 \
	"minrxlost=%f;" \
	"maxrxlost=%f;" \
	"avgrxlostr=%f;" \
	"stdevrxlost=%f;" \
	"reported_minlost=%f;" \
	"reported_maxlost=%f;" \
	"reported_avglost=%f;" \
	"reported_stdevlost=%f;"

#define FTCP_LOSS_FORMAT2 \
	"lost=%d;" \
	"expected=%d;"
	
	if (ftp->ftcp && ftp->ftcp->ftcp_info && ftp->ftcp->maxrxlost > 0) {
		snprintf(ftp->ftcp->quality_loss, sizeof(ftp->ftcp->quality_loss), FTCP_LOSS_FORMAT1,
			ftp->ftcp->minrxlost,
			ftp->ftcp->maxrxlost,
			ftp->ftcp->normdev_rxlost,
			sqrt(ftp->ftcp->stdev_rxlost),
			ftp->ftcp->reported_minlost,
			ftp->ftcp->reported_maxlost,
			ftp->ftcp->reported_normdev_lost,
			sqrt(ftp->ftcp->reported_stdev_lost)
		);
	} else {
		extended = ftp->cycles + ftp->lastrxseqno;
		expected = extended - ftp->seedrxseqno + 1;
		if (ftp->rxcount > expected) 
			expected += ftp->rxcount - expected;
		lost = expected - ftp->rxcount;

		if (!expected || lost <= 0)
			fraction = 0;
		else
			fraction = (lost << 8) / expected;

		snprintf(ftp->ftcp->quality_loss, sizeof(ftp->ftcp->quality_loss), FTCP_LOSS_FORMAT2,
			lost,
			expected
		);
	}

	return ftp->ftcp->quality_loss;

#undef FTCP_LOSS_FORMAT1
#undef FTCP_LOSS_FORMAT2
}

static char *__tris_ftp_get_quality_rtt(struct tris_ftp *ftp)
{
	if (ftp->ftcp && ftp->ftcp->ftcp_info) {
		snprintf(ftp->ftcp->quality_rtt, sizeof(ftp->ftcp->quality_rtt), "minrtt=%f;maxrtt=%f;avgrtt=%f;stdevrtt=%f;",
			ftp->ftcp->minrtt,
			ftp->ftcp->maxrtt,
			ftp->ftcp->normdevrtt,
			sqrt(ftp->ftcp->stdevrtt)
		);
	} else {
		snprintf(ftp->ftcp->quality_rtt, sizeof(ftp->ftcp->quality_rtt), "Not available");
	}

	return ftp->ftcp->quality_rtt;
}

static char *__tris_ftp_get_quality(struct tris_ftp *ftp)
{
	/*
	*ssrc          our ssrc
	*themssrc      their ssrc
	*lp            lost packets
	*rxjitter      our calculated jitter(rx)
	*rxcount       no. received packets
	*txjitter      reported jitter of the other end
	*txcount       transmitted packets
	*rlp           remote lost packets
	*rtt           round trip time
	*/	

	if (ftp->ftcp && ftp->ftcp->ftcp_info) {
		snprintf(ftp->ftcp->quality, sizeof(ftp->ftcp->quality),
			"ssrc=%u;themssrc=%u;lp=%u;rxjitter=%f;rxcount=%u;txjitter=%f;txcount=%u;rlp=%u;rtt=%f",
			ftp->ssrc,
			ftp->themssrc,
			ftp->ftcp->expected_prior - ftp->ftcp->received_prior,
			ftp->rxjitter,
			ftp->rxcount,
			(double)ftp->ftcp->reported_jitter / 65536.0,
			ftp->txcount,
			ftp->ftcp->reported_lost,
			ftp->ftcp->rtt
		);
	} else {
		snprintf(ftp->ftcp->quality, sizeof(ftp->ftcp->quality), "ssrc=%u;themssrc=%u;rxjitter=%f;rxcount=%u;txcount=%u;",
			ftp->ssrc,
			ftp->themssrc,
			ftp->rxjitter,
			ftp->rxcount,
			ftp->txcount
		);
	}

	return ftp->ftcp->quality;
}

char *tris_ftp_get_quality(struct tris_ftp *ftp, struct tris_ftp_quality *qual, enum tris_ftp_quality_type qtype) 
{
	if (qual && ftp) {
		qual->local_ssrc   = ftp->ssrc;
		qual->local_jitter = ftp->rxjitter;
		qual->local_count  = ftp->rxcount;
		qual->remote_ssrc  = ftp->themssrc;
		qual->remote_count = ftp->txcount;

		if (ftp->ftcp) {
			qual->local_lostpackets  = ftp->ftcp->expected_prior - ftp->ftcp->received_prior;
			qual->remote_lostpackets = ftp->ftcp->reported_lost;
			qual->remote_jitter      = ftp->ftcp->reported_jitter / 65536.0;
			qual->rtt                = ftp->ftcp->rtt;
		}
	}

	switch (qtype) {
	case FTPQOS_SUMMARY:
		return __tris_ftp_get_quality(ftp);
	case FTPQOS_JITTER:
		return __tris_ftp_get_quality_jitter(ftp);
	case FTPQOS_LOSS:
		return __tris_ftp_get_quality_loss(ftp);
	case FTPQOS_RTT:
		return __tris_ftp_get_quality_rtt(ftp);
	}

	return NULL;
}

void tris_ftp_destroy(struct tris_ftp *ftp)
{
	if (ftcp_debug_test_addr(&ftp->them) || ftcpstats) {
		/*Print some info on the call here */
		tris_verbose("  FTP-stats\n");
		tris_verbose("* Our Receiver:\n");
		tris_verbose("  SSRC:		 %u\n", ftp->themssrc);
		tris_verbose("  Received packets: %u\n", ftp->rxcount);
		tris_verbose("  Lost packets:	 %u\n", ftp->ftcp ? (ftp->ftcp->expected_prior - ftp->ftcp->received_prior) : 0);
		tris_verbose("  Jitter:		 %.4f\n", ftp->rxjitter);
		tris_verbose("  Transit:		 %.4f\n", ftp->rxtransit);
		tris_verbose("  RR-count:	 %u\n", ftp->ftcp ? ftp->ftcp->rr_count : 0);
		tris_verbose("* Our Sender:\n");
		tris_verbose("  SSRC:		 %u\n", ftp->ssrc);
		tris_verbose("  Sent packets:	 %u\n", ftp->txcount);
		tris_verbose("  Lost packets:	 %u\n", ftp->ftcp ? ftp->ftcp->reported_lost : 0);
		tris_verbose("  Jitter:		 %u\n", ftp->ftcp ? (ftp->ftcp->reported_jitter / (unsigned int)65536.0) : 0);
		tris_verbose("  SR-count:	 %u\n", ftp->ftcp ? ftp->ftcp->sr_count : 0);
		tris_verbose("  RTT:		 %f\n", ftp->ftcp ? ftp->ftcp->rtt : 0);
	}

	if (ftp->ftcp) {
		manager_event(EVENT_FLAG_REPORTING, "FTPReceiverStat", "SSRC: %u\r\n"
					    "ReceivedPackets: %u\r\n"
					    "LostPackets: %u\r\n"
					    "Jitter: %.4f\r\n"
					    "Transit: %.4f\r\n"
					    "RRCount: %u\r\n",
					    ftp->themssrc,
					    ftp->rxcount,
					    ftp->ftcp ? (ftp->ftcp->expected_prior - ftp->ftcp->received_prior) : 0,
					    ftp->rxjitter,
					    ftp->rxtransit,
					    ftp->ftcp ? ftp->ftcp->rr_count : 0);
		manager_event(EVENT_FLAG_REPORTING, "FTPSenderStat", "SSRC: %u\r\n"
					    "SentPackets: %u\r\n"
					    "LostPackets: %u\r\n"
					    "Jitter: %u\r\n"
					    "SRCount: %u\r\n"
					    "RTT: %f\r\n",
					    ftp->ssrc,
					    ftp->txcount,
					    ftp->ftcp ? ftp->ftcp->reported_lost : 0,
					    ftp->ftcp ? ftp->ftcp->reported_jitter : 0,
					    ftp->ftcp ? ftp->ftcp->sr_count : 0,
					    ftp->ftcp ? ftp->ftcp->rtt : 0);
	}
	if (ftp->smoother)
		tris_smoother_free(ftp->smoother);
	if (ftp->ioid)
		tris_io_remove(ftp->io, ftp->ioid);
	if (ftp->s > -1)
		close(ftp->s);
	if (ftp->ftcp) {
		TRIS_SCHED_DEL(ftp->sched, ftp->ftcp->schedid);
		close(ftp->ftcp->s);
		tris_free(ftp->ftcp);
		ftp->ftcp=NULL;
	}
#ifdef P2P_INTENSE
	tris_mutex_destroy(&ftp->bridge_lock);
#endif
	tris_free(ftp);
}

static unsigned int calc_txstamp(struct tris_ftp *ftp, struct timeval *delivery)
{
	struct timeval t;
	long ms;
	if (tris_tvzero(ftp->txcore)) {
		ftp->txcore = tris_tvnow();
		/* Round to 20ms for nice, pretty timestamps */
		ftp->txcore.tv_usec -= ftp->txcore.tv_usec % 20000;
	}
	/* Use previous txcore if available */
	t = (delivery && !tris_tvzero(*delivery)) ? *delivery : tris_tvnow();
	ms = tris_tvdiff_ms(t, ftp->txcore);
	if (ms < 0)
		ms = 0;
	/* Use what we just got for next time */
	ftp->txcore = t;
	return (unsigned int) ms;
}

/*! \brief Send begin frames for DTMF */
int tris_ftp_senddigit_begin(struct tris_ftp *ftp, char digit)
{
	unsigned int *ftpheader;
	int hdrlen = 12, res = 0, i = 0, payload = 0;
	char data[256];

	if ((digit <= '9') && (digit >= '0'))
		digit -= '0';
	else if (digit == '*')
		digit = 10;
	else if (digit == '#')
		digit = 11;
	else if ((digit >= 'A') && (digit <= 'D'))
		digit = digit - 'A' + 12;
	else if ((digit >= 'a') && (digit <= 'd'))
		digit = digit - 'a' + 12;
	else {
		tris_log(LOG_WARNING, "Don't know how to represent '%c'\n", digit);
		return 0;
	}

	/* If we have no peer, return immediately */	
	if (!ftp->them.sin_addr.s_addr || !ftp->them.sin_port)
		return 0;

	payload = tris_ftp_lookup_code(ftp, 0, TRIS_FTP_DTMF);

	ftp->dtmfmute = tris_tvadd(tris_tvnow(), tris_tv(0, 500000));
	ftp->send_duration = 160;
	ftp->lastdigitts = ftp->lastts + ftp->send_duration;
	
	/* Get a pointer to the header */
	ftpheader = (unsigned int *)data;
	ftpheader[0] = htonl((2 << 30) | (1 << 23) | (payload << 16) | (ftp->seqno));
	ftpheader[1] = htonl(ftp->lastdigitts);
	ftpheader[2] = htonl(ftp->ssrc); 

	for (i = 0; i < 2; i++) {
		ftpheader[3] = htonl((digit << 24) | (0xa << 16) | (ftp->send_duration));
		res = sendto(ftp->s, (void *) ftpheader, hdrlen + 4, 0, (struct sockaddr *) &ftp->them, sizeof(ftp->them));
		if (res < 0) 
			tris_log(LOG_ERROR, "FTP Transmission error to %s:%u: %s\n",
				tris_inet_ntoa(ftp->them.sin_addr),
				ntohs(ftp->them.sin_port), strerror(errno));
		if (ftp_debug_test_addr(&ftp->them))
			tris_verbose("Sent FTP DTMF packet to %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u)\n",
				    tris_inet_ntoa(ftp->them.sin_addr),
				    ntohs(ftp->them.sin_port), payload, ftp->seqno, ftp->lastdigitts, res - hdrlen);
		/* Increment sequence number */
		ftp->seqno++;
		/* Increment duration */
		ftp->send_duration += 160;
		/* Clear marker bit and set seqno */
		ftpheader[0] = htonl((2 << 30) | (payload << 16) | (ftp->seqno));
	}

	/* Since we received a begin, we can safely store the digit and disable any compensation */
	ftp->sending_digit = 1;
	ftp->send_digit = digit;
	ftp->send_payload = payload;

	return 0;
}

/*! \brief Send continuation frame for DTMF */
static int tris_ftp_senddigit_continuation(struct tris_ftp *ftp)
{
	unsigned int *ftpheader;
	int hdrlen = 12, res = 0;
	char data[256];

	if (!ftp->them.sin_addr.s_addr || !ftp->them.sin_port)
		return 0;

	/* Setup packet to send */
	ftpheader = (unsigned int *)data;
	ftpheader[0] = htonl((2 << 30) | (1 << 23) | (ftp->send_payload << 16) | (ftp->seqno));
	ftpheader[1] = htonl(ftp->lastdigitts);
	ftpheader[2] = htonl(ftp->ssrc);
	ftpheader[3] = htonl((ftp->send_digit << 24) | (0xa << 16) | (ftp->send_duration));
	ftpheader[0] = htonl((2 << 30) | (ftp->send_payload << 16) | (ftp->seqno));
	
	/* Transmit */
	res = sendto(ftp->s, (void *) ftpheader, hdrlen + 4, 0, (struct sockaddr *) &ftp->them, sizeof(ftp->them));
	if (res < 0)
		tris_log(LOG_ERROR, "FTP Transmission error to %s:%d: %s\n",
			tris_inet_ntoa(ftp->them.sin_addr),
			ntohs(ftp->them.sin_port), strerror(errno));
	if (ftp_debug_test_addr(&ftp->them))
		tris_verbose("Sent FTP DTMF packet to %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u)\n",
			    tris_inet_ntoa(ftp->them.sin_addr),
			    ntohs(ftp->them.sin_port), ftp->send_payload, ftp->seqno, ftp->lastdigitts, res - hdrlen);

	/* Increment sequence number */
	ftp->seqno++;
	/* Increment duration */
	ftp->send_duration += 160;

	return 0;
}

/*! \brief Send end packets for DTMF */
int tris_ftp_senddigit_end(struct tris_ftp *ftp, char digit)
{
	unsigned int *ftpheader;
	int hdrlen = 12, res = 0, i = 0;
	char data[256];
	
	/* If no address, then bail out */
	if (!ftp->them.sin_addr.s_addr || !ftp->them.sin_port)
		return 0;
	
	if ((digit <= '9') && (digit >= '0'))
		digit -= '0';
	else if (digit == '*')
		digit = 10;
	else if (digit == '#')
		digit = 11;
	else if ((digit >= 'A') && (digit <= 'D'))
		digit = digit - 'A' + 12;
	else if ((digit >= 'a') && (digit <= 'd'))
		digit = digit - 'a' + 12;
	else {
		tris_log(LOG_WARNING, "Don't know how to represent '%c'\n", digit);
		return 0;
	}

	ftp->dtmfmute = tris_tvadd(tris_tvnow(), tris_tv(0, 500000));

	ftpheader = (unsigned int *)data;
	ftpheader[1] = htonl(ftp->lastdigitts);
	ftpheader[2] = htonl(ftp->ssrc);
	ftpheader[3] = htonl((digit << 24) | (0xa << 16) | (ftp->send_duration));
	/* Set end bit */
	ftpheader[3] |= htonl((1 << 23));

	/* Send 3 termination packets */
	for (i = 0; i < 3; i++) {
		ftpheader[0] = htonl((2 << 30) | (ftp->send_payload << 16) | (ftp->seqno));
		res = sendto(ftp->s, (void *) ftpheader, hdrlen + 4, 0, (struct sockaddr *) &ftp->them, sizeof(ftp->them));
		ftp->seqno++;
		if (res < 0)
			tris_log(LOG_ERROR, "FTP Transmission error to %s:%d: %s\n",
				tris_inet_ntoa(ftp->them.sin_addr),
				ntohs(ftp->them.sin_port), strerror(errno));
		if (ftp_debug_test_addr(&ftp->them))
			tris_verbose("Sent FTP DTMF packet to %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u)\n",
				    tris_inet_ntoa(ftp->them.sin_addr),
				    ntohs(ftp->them.sin_port), ftp->send_payload, ftp->seqno, ftp->lastdigitts, res - hdrlen);
	}
	ftp->lastts += ftp->send_duration;
	ftp->sending_digit = 0;
	ftp->send_digit = 0;

	return res;
}

/*! \brief Public function: Send an H.261 fast update request, some devices need this rather than SIP XML */
int tris_ftcp_send_h261fur(void *data)
{
	struct tris_ftp *ftp = data;
	int res;

	ftp->ftcp->sendfur = 1;
	res = tris_ftcp_write(data);
	
	return res;
}

/*! \brief Send FTCP sender's report */
static int tris_ftcp_write_sr(const void *data)
{
	struct tris_ftp *ftp = (struct tris_ftp *)data;
	int res;
	int len = 0;
	struct timeval now;
	unsigned int now_lsw;
	unsigned int now_msw;
	unsigned int *ftcpheader;
	unsigned int lost;
	unsigned int extended;
	unsigned int expected;
	unsigned int expected_interval;
	unsigned int received_interval;
	int lost_interval;
	int fraction;
	struct timeval dlsr;
	char bdata[512];

	/* Commented condition is always not NULL if ftp->ftcp is not NULL */
	if (!ftp || !ftp->ftcp/* || (&ftp->ftcp->them.sin_addr == 0)*/)
		return 0;
	
	if (!ftp->ftcp->them.sin_addr.s_addr) {  /* This'll stop ftcp for this ftp session */
		tris_verbose("FTCP SR transmission error, ftcp halted\n");
		TRIS_SCHED_DEL(ftp->sched, ftp->ftcp->schedid);
		return 0;
	}

	gettimeofday(&now, NULL);
	timeval2ntp(now, &now_msw, &now_lsw); /* fill thses ones in from utils.c*/
	ftcpheader = (unsigned int *)bdata;
	ftcpheader[1] = htonl(ftp->ssrc);               /* Our SSRC */
	ftcpheader[2] = htonl(now_msw);                 /* now, MSW. gettimeofday() + SEC_BETWEEN_1900_AND_1970*/
	ftcpheader[3] = htonl(now_lsw);                 /* now, LSW */
	ftcpheader[4] = htonl(ftp->lastts);             /* FIXME shouldn't be that, it should be now */
	ftcpheader[5] = htonl(ftp->txcount);            /* No. packets sent */
	ftcpheader[6] = htonl(ftp->txoctetcount);       /* No. bytes sent */
	len += 28;
	
	extended = ftp->cycles + ftp->lastrxseqno;
	expected = extended - ftp->seedrxseqno + 1;
	if (ftp->rxcount > expected) 
		expected += ftp->rxcount - expected;
	lost = expected - ftp->rxcount;
	expected_interval = expected - ftp->ftcp->expected_prior;
	ftp->ftcp->expected_prior = expected;
	received_interval = ftp->rxcount - ftp->ftcp->received_prior;
	ftp->ftcp->received_prior = ftp->rxcount;
	lost_interval = expected_interval - received_interval;
	if (expected_interval == 0 || lost_interval <= 0)
		fraction = 0;
	else
		fraction = (lost_interval << 8) / expected_interval;
	timersub(&now, &ftp->ftcp->rxlsr, &dlsr);
	ftcpheader[7] = htonl(ftp->themssrc);
	ftcpheader[8] = htonl(((fraction & 0xff) << 24) | (lost & 0xffffff));
	ftcpheader[9] = htonl((ftp->cycles) | ((ftp->lastrxseqno & 0xffff)));
	ftcpheader[10] = htonl((unsigned int)(ftp->rxjitter * 65536.));
	ftcpheader[11] = htonl(ftp->ftcp->themrxlsr);
	ftcpheader[12] = htonl((((dlsr.tv_sec * 1000) + (dlsr.tv_usec / 1000)) * 65536) / 1000);
	len += 24;
	
	ftcpheader[0] = htonl((2 << 30) | (1 << 24) | (FTCP_PT_SR << 16) | ((len/4)-1));

	if (ftp->ftcp->sendfur) {
		ftcpheader[13] = htonl((2 << 30) | (0 << 24) | (FTCP_PT_FUR << 16) | 1);
		ftcpheader[14] = htonl(ftp->ssrc);               /* Our SSRC */
		len += 8;
		ftp->ftcp->sendfur = 0;
	}
	
	/* Insert SDES here. Probably should make SDES text equal to mimetypes[code].type (not subtype 'cos */ 
	/* it can change mid call, and SDES can't) */
	ftcpheader[len/4]     = htonl((2 << 30) | (1 << 24) | (FTCP_PT_SDES << 16) | 2);
	ftcpheader[(len/4)+1] = htonl(ftp->ssrc);               /* Our SSRC */
	ftcpheader[(len/4)+2] = htonl(0x01 << 24);                    /* Empty for the moment */
	len += 12;
	
	res = sendto(ftp->ftcp->s, (unsigned int *)ftcpheader, len, 0, (struct sockaddr *)&ftp->ftcp->them, sizeof(ftp->ftcp->them));
	if (res < 0) {
		tris_log(LOG_ERROR, "FTCP SR transmission error to %s:%d, ftcp halted %s\n",tris_inet_ntoa(ftp->ftcp->them.sin_addr), ntohs(ftp->ftcp->them.sin_port), strerror(errno));
		TRIS_SCHED_DEL(ftp->sched, ftp->ftcp->schedid);
		return 0;
	}
	
	/* FIXME Don't need to get a new one */
	gettimeofday(&ftp->ftcp->txlsr, NULL);
	ftp->ftcp->sr_count++;

	ftp->ftcp->lastsrtxcount = ftp->txcount;	
	
	if (ftcp_debug_test_addr(&ftp->ftcp->them)) {
		tris_verbose("* Sent FTCP SR to %s:%d\n", tris_inet_ntoa(ftp->ftcp->them.sin_addr), ntohs(ftp->ftcp->them.sin_port));
		tris_verbose("  Our SSRC: %u\n", ftp->ssrc);
		tris_verbose("  Sent(NTP): %u.%010u\n", (unsigned int)now.tv_sec, (unsigned int)now.tv_usec*4096);
		tris_verbose("  Sent(FTP): %u\n", ftp->lastts);
		tris_verbose("  Sent packets: %u\n", ftp->txcount);
		tris_verbose("  Sent octets: %u\n", ftp->txoctetcount);
		tris_verbose("  Report block:\n");
		tris_verbose("  Fraction lost: %u\n", fraction);
		tris_verbose("  Cumulative loss: %u\n", lost);
		tris_verbose("  IA jitter: %.4f\n", ftp->rxjitter);
		tris_verbose("  Their last SR: %u\n", ftp->ftcp->themrxlsr);
		tris_verbose("  DLSR: %4.4f (sec)\n\n", (double)(ntohl(ftcpheader[12])/65536.0));
	}
	manager_event(EVENT_FLAG_REPORTING, "FTCPSent", "To: %s:%d\r\n"
					    "OurSSRC: %u\r\n"
					    "SentNTP: %u.%010u\r\n"
					    "SentFTP: %u\r\n"
					    "SentPackets: %u\r\n"
					    "SentOctets: %u\r\n"
					    "ReportBlock:\r\n"
					    "FractionLost: %u\r\n"
					    "CumulativeLoss: %u\r\n"
					    "IAJitter: %.4f\r\n"
					    "TheirLastSR: %u\r\n"
					    "DLSR: %4.4f (sec)\r\n",
					    tris_inet_ntoa(ftp->ftcp->them.sin_addr), ntohs(ftp->ftcp->them.sin_port),
					    ftp->ssrc,
					    (unsigned int)now.tv_sec, (unsigned int)now.tv_usec*4096,
					    ftp->lastts,
					    ftp->txcount,
					    ftp->txoctetcount,
					    fraction,
					    lost,
					    ftp->rxjitter,
					    ftp->ftcp->themrxlsr,
					    (double)(ntohl(ftcpheader[12])/65536.0));
	return res;
}

/*! \brief Send FTCP recipient's report */
static int tris_ftcp_write_rr(const void *data)
{
	struct tris_ftp *ftp = (struct tris_ftp *)data;
	int res;
	int len = 32;
	unsigned int lost;
	unsigned int extended;
	unsigned int expected;
	unsigned int expected_interval;
	unsigned int received_interval;
	int lost_interval;
	struct timeval now;
	unsigned int *ftcpheader;
	char bdata[1024];
	struct timeval dlsr;
	int fraction;

	double rxlost_current;
	
	if (!ftp || !ftp->ftcp || (&ftp->ftcp->them.sin_addr == 0))
		return 0;
	  
	if (!ftp->ftcp->them.sin_addr.s_addr) {
		tris_log(LOG_ERROR, "FTCP RR transmission error, ftcp halted\n");
		TRIS_SCHED_DEL(ftp->sched, ftp->ftcp->schedid);
		return 0;
	}

	extended = ftp->cycles + ftp->lastrxseqno;
	expected = extended - ftp->seedrxseqno + 1;
	lost = expected - ftp->rxcount;
	expected_interval = expected - ftp->ftcp->expected_prior;
	ftp->ftcp->expected_prior = expected;
	received_interval = ftp->rxcount - ftp->ftcp->received_prior;
	ftp->ftcp->received_prior = ftp->rxcount;
	lost_interval = expected_interval - received_interval;

	if (lost_interval <= 0)
		ftp->ftcp->rxlost = 0;
	else ftp->ftcp->rxlost = ftp->ftcp->rxlost;
	if (ftp->ftcp->rxlost_count == 0)
		ftp->ftcp->minrxlost = ftp->ftcp->rxlost;
	if (lost_interval < ftp->ftcp->minrxlost) 
		ftp->ftcp->minrxlost = ftp->ftcp->rxlost;
	if (lost_interval > ftp->ftcp->maxrxlost) 
		ftp->ftcp->maxrxlost = ftp->ftcp->rxlost;

	rxlost_current = normdev_compute(ftp->ftcp->normdev_rxlost, ftp->ftcp->rxlost, ftp->ftcp->rxlost_count);
	ftp->ftcp->stdev_rxlost = stddev_compute(ftp->ftcp->stdev_rxlost, ftp->ftcp->rxlost, ftp->ftcp->normdev_rxlost, rxlost_current, ftp->ftcp->rxlost_count);
	ftp->ftcp->normdev_rxlost = rxlost_current;
	ftp->ftcp->rxlost_count++;

	if (expected_interval == 0 || lost_interval <= 0)
		fraction = 0;
	else
		fraction = (lost_interval << 8) / expected_interval;
	gettimeofday(&now, NULL);
	timersub(&now, &ftp->ftcp->rxlsr, &dlsr);
	ftcpheader = (unsigned int *)bdata;
	ftcpheader[0] = htonl((2 << 30) | (1 << 24) | (FTCP_PT_RR << 16) | ((len/4)-1));
	ftcpheader[1] = htonl(ftp->ssrc);
	ftcpheader[2] = htonl(ftp->themssrc);
	ftcpheader[3] = htonl(((fraction & 0xff) << 24) | (lost & 0xffffff));
	ftcpheader[4] = htonl((ftp->cycles) | ((ftp->lastrxseqno & 0xffff)));
	ftcpheader[5] = htonl((unsigned int)(ftp->rxjitter * 65536.));
	ftcpheader[6] = htonl(ftp->ftcp->themrxlsr);
	ftcpheader[7] = htonl((((dlsr.tv_sec * 1000) + (dlsr.tv_usec / 1000)) * 65536) / 1000);

	if (ftp->ftcp->sendfur) {
		ftcpheader[8] = htonl((2 << 30) | (0 << 24) | (FTCP_PT_FUR << 16) | 1); /* Header from page 36 in RFC 3550 */
		ftcpheader[9] = htonl(ftp->ssrc);               /* Our SSRC */
		len += 8;
		ftp->ftcp->sendfur = 0;
	}

	/*! \note Insert SDES here. Probably should make SDES text equal to mimetypes[code].type (not subtype 'cos 
	it can change mid call, and SDES can't) */
	ftcpheader[len/4]     = htonl((2 << 30) | (1 << 24) | (FTCP_PT_SDES << 16) | 2);
	ftcpheader[(len/4)+1] = htonl(ftp->ssrc);               /* Our SSRC */
	ftcpheader[(len/4)+2] = htonl(0x01 << 24);              /* Empty for the moment */
	len += 12;
	
	res = sendto(ftp->ftcp->s, (unsigned int *)ftcpheader, len, 0, (struct sockaddr *)&ftp->ftcp->them, sizeof(ftp->ftcp->them));

	if (res < 0) {
		tris_log(LOG_ERROR, "FTCP RR transmission error, ftcp halted: %s\n",strerror(errno));
		/* Remove the scheduler */
		TRIS_SCHED_DEL(ftp->sched, ftp->ftcp->schedid);
		return 0;
	}

	ftp->ftcp->rr_count++;

	if (ftcp_debug_test_addr(&ftp->ftcp->them)) {
		tris_verbose("\n* Sending FTCP RR to %s:%d\n"
			"  Our SSRC: %u\nTheir SSRC: %u\niFraction lost: %d\nCumulative loss: %u\n" 
			"  IA jitter: %.4f\n" 
			"  Their last SR: %u\n" 
			"  DLSR: %4.4f (sec)\n\n",
			tris_inet_ntoa(ftp->ftcp->them.sin_addr),
			ntohs(ftp->ftcp->them.sin_port),
			ftp->ssrc, ftp->themssrc, fraction, lost,
			ftp->rxjitter,
			ftp->ftcp->themrxlsr,
			(double)(ntohl(ftcpheader[7])/65536.0));
	}

	return res;
}

/*! \brief Write and FTCP packet to the far end
 * \note Decide if we are going to send an SR (with Reception Block) or RR 
 * RR is sent if we have not sent any ftp packets in the previous interval */
static int tris_ftcp_write(const void *data)
{
	struct tris_ftp *ftp = (struct tris_ftp *)data;
	int res;
	
	if (!ftp || !ftp->ftcp)
		return 0;

	if (ftp->txcount > ftp->ftcp->lastsrtxcount)
		res = tris_ftcp_write_sr(data);
	else
		res = tris_ftcp_write_rr(data);
	
	return res;
}

/*! \brief generate comfort noice (CNG) */
int tris_ftp_sendcng(struct tris_ftp *ftp, int level)
{
	unsigned int *ftpheader;
	int hdrlen = 12;
	int res;
	int payload;
	char data[256];
	level = 127 - (level & 0x7f);
	payload = tris_ftp_lookup_code(ftp, 0, TRIS_FTP_CN);

	/* If we have no peer, return immediately */	
	if (!ftp->them.sin_addr.s_addr)
		return 0;

	ftp->dtmfmute = tris_tvadd(tris_tvnow(), tris_tv(0, 500000));

	/* Get a pointer to the header */
	ftpheader = (unsigned int *)data;
	ftpheader[0] = htonl((2 << 30) | (1 << 23) | (payload << 16) | (ftp->seqno++));
	ftpheader[1] = htonl(ftp->lastts);
	ftpheader[2] = htonl(ftp->ssrc); 
	data[12] = level;
	if (ftp->them.sin_port && ftp->them.sin_addr.s_addr) {
		res = sendto(ftp->s, (void *)ftpheader, hdrlen + 1, 0, (struct sockaddr *)&ftp->them, sizeof(ftp->them));
		if (res <0) 
			tris_log(LOG_ERROR, "FTP Comfort Noise Transmission error to %s:%d: %s\n", tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port), strerror(errno));
		if (ftp_debug_test_addr(&ftp->them))
			tris_verbose("Sent Comfort Noise FTP packet to %s:%u (type %d, seq %u, ts %u, len %d)\n"
					, tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port), payload, ftp->seqno, ftp->lastts,res - hdrlen);		   
		   
	}
	return 0;
}

/*! \brief Write FTP packet with audio or video media frames into UDP packet */
static int tris_ftp_raw_write(struct tris_ftp *ftp, struct tris_frame *f, int codec)
{
	unsigned char *ftpheader;
	int hdrlen = 12;
	int res;
	unsigned int ms;
	int pred;
	int mark = 0;
	int rate = ftp_get_rate(f->subclass) / 1000;

	if (f->subclass == TRIS_FORMAT_G722) {
		f->samples /= 2;
	}

	if (ftp->sending_digit) {
		return 0;
	}

	ms = calc_txstamp(ftp, &f->delivery);
	/* Default prediction */
	if (f->frametype == TRIS_FRAME_VOICE) {
		pred = ftp->lastts + f->samples;

		/* Re-calculate last TS */
		ftp->lastts = ftp->lastts + ms * rate;
		if (tris_tvzero(f->delivery)) {
			/* If this isn't an absolute delivery time, Check if it is close to our prediction, 
			   and if so, go with our prediction */
			if (abs(ftp->lastts - pred) < MAX_TIMESTAMP_SKEW)
				ftp->lastts = pred;
			else {
				tris_debug(3, "Difference is %d, ms is %d\n", abs(ftp->lastts - pred), ms);
				mark = 1;
			}
		}
	} else if (f->frametype == TRIS_FRAME_VIDEO) {
		mark = f->subclass & 0x1;
		pred = ftp->lastovidtimestamp + f->samples;
		/* Re-calculate last TS */
		ftp->lastts = ftp->lastts + ms * 90;
		/* If it's close to our prediction, go for it */
		if (tris_tvzero(f->delivery)) {
			if (abs(ftp->lastts - pred) < 7200) {
				ftp->lastts = pred;
				ftp->lastovidtimestamp += f->samples;
			} else {
				tris_debug(3, "Difference is %d, ms is %d (%d), pred/ts/samples %d/%d/%d\n", abs(ftp->lastts - pred), ms, ms * 90, ftp->lastts, pred, f->samples);
				ftp->lastovidtimestamp = ftp->lastts;
			}
		}
	} else {
		pred = ftp->lastotexttimestamp + f->samples;
		/* Re-calculate last TS */
		ftp->lastts = ftp->lastts + ms;
		/* If it's close to our prediction, go for it */
		if (tris_tvzero(f->delivery)) {
			if (abs(ftp->lastts - pred) < 7200) {
				ftp->lastts = pred;
				ftp->lastotexttimestamp += f->samples;
			} else {
				tris_debug(3, "Difference is %d, ms is %d, pred/ts/samples %d/%d/%d\n", abs(ftp->lastts - pred), ms, ftp->lastts, pred, f->samples);
				ftp->lastotexttimestamp = ftp->lastts;
			}
		}
	}

	/* If we have been explicitly told to set the marker bit do so */
	if (ftp->set_marker_bit) {
		mark = 1;
		ftp->set_marker_bit = 0;
	}

	/* If the timestamp for non-digit packets has moved beyond the timestamp
	   for digits, update the digit timestamp.
	*/
	if (ftp->lastts > ftp->lastdigitts)
		ftp->lastdigitts = ftp->lastts;

	if (tris_test_flag(f, TRIS_FRFLAG_HAS_TIMING_INFO))
		ftp->lastts = f->ts * rate;

	/* Get a pointer to the header */
	ftpheader = (unsigned char *)(f->data.ptr - hdrlen);

	put_unaligned_uint32(ftpheader, htonl((2 << 30) | (codec << 16) | (ftp->seqno) | (mark << 23)));
	put_unaligned_uint32(ftpheader + 4, htonl(ftp->lastts));
	put_unaligned_uint32(ftpheader + 8, htonl(ftp->ssrc)); 

	if (ftp->them.sin_port && ftp->them.sin_addr.s_addr) {
		res = sendto(ftp->s, (void *)ftpheader, f->datalen + hdrlen, 0, (struct sockaddr *)&ftp->them, sizeof(ftp->them));
		if (res < 0) {
			if (!ftp->nat || (ftp->nat && (tris_test_flag(ftp, FLAG_NAT_ACTIVE) == FLAG_NAT_ACTIVE))) {
				tris_debug(1, "FTP Transmission error of packet %d to %s:%d: %s\n", ftp->seqno, tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port), strerror(errno));
			} else if (((tris_test_flag(ftp, FLAG_NAT_ACTIVE) == FLAG_NAT_INACTIVE) || ftpdebug) && !tris_test_flag(ftp, FLAG_NAT_INACTIVE_NOWARN)) {
				/* Only give this error message once if we are not FTP debugging */
				if (option_debug || ftpdebug)
					tris_debug(0, "FTP NAT: Can't write FTP to private address %s:%d, waiting for other end to send audio...\n", tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port));
				tris_set_flag(ftp, FLAG_NAT_INACTIVE_NOWARN);
			}
		} else {
			ftp->txcount++;
			ftp->txoctetcount +=(res - hdrlen);
			
			/* Do not schedule RR if FTCP isn't run */
			if (ftp->ftcp && ftp->ftcp->them.sin_addr.s_addr && ftp->ftcp->schedid < 1) {
				ftp->ftcp->schedid = tris_sched_add(ftp->sched, tris_ftcp_calc_interval(ftp), tris_ftcp_write, ftp);
			}
		}
				
		if (ftp_debug_test_addr(&ftp->them))
			tris_verbose("Sent FTP packet to      %s:%u (type %-2.2d, seq %-6.6u, ts %-6.6u, len %-6.6u)\n",
					tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port), codec, ftp->seqno, ftp->lastts,res - hdrlen);
	}

	ftp->seqno++;

	return 0;
}

void tris_ftp_codec_setpref(struct tris_ftp *ftp, struct tris_codec_pref *prefs)
{
	struct tris_format_list current_format_old, current_format_new;

	/* if no packets have been sent through this session yet, then
	 *  changing preferences does not require any extra work
	 */
	if (ftp->lasttxformat == 0) {
		ftp->pref = *prefs;
		return;
	}

	current_format_old = tris_codec_pref_getsize(&ftp->pref, ftp->lasttxformat);

	ftp->pref = *prefs;

	current_format_new = tris_codec_pref_getsize(&ftp->pref, ftp->lasttxformat);

	/* if the framing desired for the current format has changed, we may have to create
	 * or adjust the smoother for this session
	 */
	if ((current_format_new.inc_ms != 0) &&
	    (current_format_new.cur_ms != current_format_old.cur_ms)) {
		int new_size = (current_format_new.cur_ms * current_format_new.fr_len) / current_format_new.inc_ms;

		if (ftp->smoother) {
			tris_smoother_reconfigure(ftp->smoother, new_size);
			if (option_debug) {
				tris_log(LOG_DEBUG, "Adjusted smoother to %d ms and %d bytes\n", current_format_new.cur_ms, new_size);
			}
		} else {
			if (!(ftp->smoother = tris_smoother_new(new_size))) {
				tris_log(LOG_WARNING, "Unable to create smoother: format: %d ms: %d len: %d\n", ftp->lasttxformat, current_format_new.cur_ms, new_size);
				return;
			}
			if (current_format_new.flags) {
				tris_smoother_set_flags(ftp->smoother, current_format_new.flags);
			}
			if (option_debug) {
				tris_log(LOG_DEBUG, "Created smoother: format: %d ms: %d len: %d\n", ftp->lasttxformat, current_format_new.cur_ms, new_size);
			}
		}
	}

}

struct tris_codec_pref *tris_ftp_codec_getpref(struct tris_ftp *ftp)
{
	return &ftp->pref;
}

int tris_ftp_codec_getformat(int pt)
{
	if (pt < 0 || pt >= MAX_FTP_PT)
		return 0; /* bogus payload type */

	if (static_FTP_PT[pt].isAstFormat)
		return static_FTP_PT[pt].code;
	else
		return 0;
}

int tris_ftp_write_orig(struct tris_ftp *ftp, struct tris_frame *_f)
{
	struct tris_frame *f;
	int codec;
	int hdrlen = 12;
	int subclass;
	

	/* If we have no peer, return immediately */	
	if (!ftp->them.sin_addr.s_addr)
		return 0;

	/* If there is no data length, return immediately */
	if (!_f->datalen && !ftp->red)
		return 0;
	
	/* Make sure we have enough space for FTP header */
	if ((_f->frametype != TRIS_FRAME_VOICE) && (_f->frametype != TRIS_FRAME_VIDEO) && (_f->frametype != TRIS_FRAME_TEXT)) {
		tris_log(LOG_WARNING, "FTP can only send voice, video and text\n");
		return -1;
	}

	if (ftp->red) {
		/* return 0; */
		/* no primary data or generations to send */
		if ((_f = red_t140_to_red(ftp->red)) == NULL) 
			return 0;
	}

	/* The bottom bit of a video subclass contains the marker bit */
	subclass = _f->subclass;
	if (_f->frametype == TRIS_FRAME_VIDEO)
		subclass &= ~0x1;

	codec = tris_ftp_lookup_code(ftp, 1, subclass);
	if (codec < 0) {
		tris_log(LOG_WARNING, "Don't know how to send format %s packets with FTP\n", tris_getformatname(_f->subclass));
		return -1;
	}

	if (ftp->lasttxformat != subclass) {
		/* New format, reset the smoother */
		tris_debug(1, "Ooh, format changed from %s to %s\n", tris_getformatname(ftp->lasttxformat), tris_getformatname(subclass));
		ftp->lasttxformat = subclass;
		if (ftp->smoother)
			tris_smoother_free(ftp->smoother);
		ftp->smoother = NULL;
	}

	if (!ftp->smoother) {
		struct tris_format_list fmt = tris_codec_pref_getsize(&ftp->pref, subclass);

		switch (subclass) {
		case TRIS_FORMAT_SPEEX:
		case TRIS_FORMAT_G723_1:
		case TRIS_FORMAT_SIREN7:
		case TRIS_FORMAT_SIREN14:
			/* these are all frame-based codecs and cannot be safely run through
			   a smoother */
			break;
		default:
			if (fmt.inc_ms) { /* if codec parameters is set / avoid division by zero */
				if (!(ftp->smoother = tris_smoother_new((fmt.cur_ms * fmt.fr_len) / fmt.inc_ms))) {
					tris_log(LOG_WARNING, "Unable to create smoother: format: %d ms: %d len: %d\n", subclass, fmt.cur_ms, ((fmt.cur_ms * fmt.fr_len) / fmt.inc_ms));
					return -1;
				}
				if (fmt.flags)
					tris_smoother_set_flags(ftp->smoother, fmt.flags);
				tris_debug(1, "Created smoother: format: %d ms: %d len: %d\n", subclass, fmt.cur_ms, ((fmt.cur_ms * fmt.fr_len) / fmt.inc_ms));
			}
		}
	}
	if (ftp->smoother) {
		if (tris_smoother_test_flag(ftp->smoother, TRIS_SMOOTHER_FLAG_BE)) {
			tris_smoother_feed_be(ftp->smoother, _f);
		} else {
			tris_smoother_feed(ftp->smoother, _f);
		}

		while ((f = tris_smoother_read(ftp->smoother)) && (f->data.ptr)) {
			tris_ftp_raw_write(ftp, f, codec);
		}
	} else {
		/* Don't buffer outgoing frames; send them one-per-packet: */
		if (_f->offset < hdrlen) 
			f = tris_frdup(_f);	/*! \bug XXX this might never be free'd. Why do we do this? */
		else
			f = _f;
		if (f->data.ptr)
			tris_ftp_raw_write(ftp, f, codec);
		if (f != _f)
			tris_frfree(f);
	}
		
	return 0;
}

int tris_ftp_write(struct tris_ftp *ftp, struct tris_frame *_f)
{
	if (!ftp->connection)
		return 0;

	if (_f->datalen <=0)
		return 0;
	
	send(ftp->s, _f->data.ptr, _f->datalen, 0);
		
	return 0;
}

/*! \brief Unregister interface to channel driver */
void tris_ftp_proto_unregister(struct tris_ftp_protocol *proto)
{
	TRIS_RWLIST_WRLOCK(&protos);
	TRIS_RWLIST_REMOVE(&protos, proto, list);
	TRIS_RWLIST_UNLOCK(&protos);
}

/*! \brief Register interface to channel driver */
int tris_ftp_proto_register(struct tris_ftp_protocol *proto)
{
	struct tris_ftp_protocol *cur;

	TRIS_RWLIST_WRLOCK(&protos);
	TRIS_RWLIST_TRAVERSE(&protos, cur, list) {	
		if (!strcmp(cur->type, proto->type)) {
			tris_log(LOG_WARNING, "Tried to register same protocol '%s' twice\n", cur->type);
			TRIS_RWLIST_UNLOCK(&protos);
			return -1;
		}
	}
	TRIS_RWLIST_INSERT_HEAD(&protos, proto, list);
	TRIS_RWLIST_UNLOCK(&protos);
	
	return 0;
}

/*! \brief Bridge loop for true native bridge (reinvite) */
static enum tris_bridge_result bridge_native_loop(struct tris_channel *c0, struct tris_channel *c1, struct tris_ftp *p0, struct tris_ftp *p1, struct tris_ftp *vp0, struct tris_ftp *vp1, struct tris_ftp *tp0, struct tris_ftp *tp1, struct tris_ftp_protocol *pr0, struct tris_ftp_protocol *pr1, int codec0, int codec1, int timeoutms, int flags, struct tris_frame **fo, struct tris_channel **rc, void *pvt0, void *pvt1)
{
	struct tris_frame *fr = NULL;
	struct tris_channel *who = NULL, *other = NULL, *cs[3] = {NULL, };
	int oldcodec0 = codec0, oldcodec1 = codec1;
	struct sockaddr_in ac1 = {0,}, vac1 = {0,}, tac1 = {0,}, ac0 = {0,}, vac0 = {0,}, tac0 = {0,};
	struct sockaddr_in t1 = {0,}, vt1 = {0,}, tt1 = {0,}, t0 = {0,}, vt0 = {0,}, tt0 = {0,};
	
	/* Set it up so audio goes directly between the two endpoints */

	/* Test the first channel */
	if (!(pr0->set_ftp_peer(c0, p1, vp1, tp1, codec1, tris_test_flag(p1, FLAG_NAT_ACTIVE)))) {
		tris_ftp_get_peer(p1, &ac1);
		if (vp1)
			tris_ftp_get_peer(vp1, &vac1);
		if (tp1)
			tris_ftp_get_peer(tp1, &tac1);
	} else
		tris_log(LOG_WARNING, "Channel '%s' failed to talk to '%s'\n", c0->name, c1->name);
	
	/* Test the second channel */
	if (!(pr1->set_ftp_peer(c1, p0, vp0, tp0, codec0, tris_test_flag(p0, FLAG_NAT_ACTIVE)))) {
		tris_ftp_get_peer(p0, &ac0);
		if (vp0)
			tris_ftp_get_peer(vp0, &vac0);
		if (tp0)
			tris_ftp_get_peer(tp0, &tac0);
	} else
		tris_log(LOG_WARNING, "Channel '%s' failed to talk to '%s'\n", c1->name, c0->name);

	/* Now we can unlock and move into our loop */
	tris_channel_unlock(c0);
	tris_channel_unlock(c1);

	tris_poll_channel_add(c0, c1);

	/* Throw our channels into the structure and enter the loop */
	cs[0] = c0;
	cs[1] = c1;
	cs[2] = NULL;
	for (;;) {
		/* Check if anything changed */
		if ((c0->tech_pvt != pvt0) ||
		    (c1->tech_pvt != pvt1) ||
		    (c0->masq || c0->masqr || c1->masq || c1->masqr) ||
		    (c0->monitor || c0->audiohooks || c1->monitor || c1->audiohooks)) {
			tris_debug(1, "Oooh, something is weird, backing out\n");
			if (c0->tech_pvt == pvt0)
				if (pr0->set_ftp_peer(c0, NULL, NULL, NULL, 0, 0))
					tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c0->name);
			if (c1->tech_pvt == pvt1)
				if (pr1->set_ftp_peer(c1, NULL, NULL, NULL, 0, 0))
					tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c1->name);
			tris_poll_channel_del(c0, c1);
			return TRIS_BRIDGE_RETRY;
		}

		/* Check if they have changed their address */
		tris_ftp_get_peer(p1, &t1);
		if (vp1)
			tris_ftp_get_peer(vp1, &vt1);
		if (tp1)
			tris_ftp_get_peer(tp1, &tt1);
		if (pr1->get_codec)
			codec1 = pr1->get_codec(c1);
		tris_ftp_get_peer(p0, &t0);
		if (vp0)
			tris_ftp_get_peer(vp0, &vt0);
		if (tp0)
			tris_ftp_get_peer(tp0, &tt0);
		if (pr0->get_codec)
			codec0 = pr0->get_codec(c0);
		if ((inaddrcmp(&t1, &ac1)) ||
		    (vp1 && inaddrcmp(&vt1, &vac1)) ||
		    (tp1 && inaddrcmp(&tt1, &tac1)) ||
		    (codec1 != oldcodec1)) {
			tris_debug(2, "Oooh, '%s' changed end address to %s:%d (format %d)\n",
				c1->name, tris_inet_ntoa(t1.sin_addr), ntohs(t1.sin_port), codec1);
			tris_debug(2, "Oooh, '%s' changed end vaddress to %s:%d (format %d)\n",
				c1->name, tris_inet_ntoa(vt1.sin_addr), ntohs(vt1.sin_port), codec1);
			tris_debug(2, "Oooh, '%s' changed end taddress to %s:%d (format %d)\n",
				c1->name, tris_inet_ntoa(tt1.sin_addr), ntohs(tt1.sin_port), codec1);
			tris_debug(2, "Oooh, '%s' was %s:%d/(format %d)\n",
				c1->name, tris_inet_ntoa(ac1.sin_addr), ntohs(ac1.sin_port), oldcodec1);
			tris_debug(2, "Oooh, '%s' was %s:%d/(format %d)\n",
				c1->name, tris_inet_ntoa(vac1.sin_addr), ntohs(vac1.sin_port), oldcodec1);
			tris_debug(2, "Oooh, '%s' was %s:%d/(format %d)\n",
				c1->name, tris_inet_ntoa(tac1.sin_addr), ntohs(tac1.sin_port), oldcodec1);
			if (pr0->set_ftp_peer(c0, t1.sin_addr.s_addr ? p1 : NULL, vt1.sin_addr.s_addr ? vp1 : NULL, tt1.sin_addr.s_addr ? tp1 : NULL, codec1, tris_test_flag(p1, FLAG_NAT_ACTIVE)))
				tris_log(LOG_WARNING, "Channel '%s' failed to update to '%s'\n", c0->name, c1->name);
			memcpy(&ac1, &t1, sizeof(ac1));
			memcpy(&vac1, &vt1, sizeof(vac1));
			memcpy(&tac1, &tt1, sizeof(tac1));
			oldcodec1 = codec1;
		}
		if ((inaddrcmp(&t0, &ac0)) ||
		    (vp0 && inaddrcmp(&vt0, &vac0)) ||
		    (tp0 && inaddrcmp(&tt0, &tac0)) ||
		    (codec0 != oldcodec0)) {
			tris_debug(2, "Oooh, '%s' changed end address to %s:%d (format %d)\n",
				c0->name, tris_inet_ntoa(t0.sin_addr), ntohs(t0.sin_port), codec0);
			tris_debug(2, "Oooh, '%s' was %s:%d/(format %d)\n",
				c0->name, tris_inet_ntoa(ac0.sin_addr), ntohs(ac0.sin_port), oldcodec0);
			if (pr1->set_ftp_peer(c1, t0.sin_addr.s_addr ? p0 : NULL, vt0.sin_addr.s_addr ? vp0 : NULL, tt0.sin_addr.s_addr ? tp0 : NULL, codec0, tris_test_flag(p0, FLAG_NAT_ACTIVE)))
				tris_log(LOG_WARNING, "Channel '%s' failed to update to '%s'\n", c1->name, c0->name);
			memcpy(&ac0, &t0, sizeof(ac0));
			memcpy(&vac0, &vt0, sizeof(vac0));
			memcpy(&tac0, &tt0, sizeof(tac0));
			oldcodec0 = codec0;
		}

		/* Wait for frame to come in on the channels */
		if (!(who = tris_waitfor_n(cs, 2, &timeoutms))) {
			if (!timeoutms) {
				if (pr0->set_ftp_peer(c0, NULL, NULL, NULL, 0, 0))
					tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c0->name);
				if (pr1->set_ftp_peer(c1, NULL, NULL, NULL, 0, 0))
					tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c1->name);
				return TRIS_BRIDGE_RETRY;
			}
			tris_debug(1, "Ooh, empty read...\n");
			if (tris_check_hangup(c0) || tris_check_hangup(c1))
				break;
			continue;
		}
		fr = tris_read(who);
		other = (who == c0) ? c1 : c0;
		if (!fr || ((fr->frametype == TRIS_FRAME_DTMF_BEGIN || fr->frametype == TRIS_FRAME_DTMF_END) &&
			    (((who == c0) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_0)) ||
			     ((who == c1) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_1))))) {
			/* Break out of bridge */
			*fo = fr;
			*rc = who;
			tris_debug(1, "Oooh, got a %s\n", fr ? "digit" : "hangup");
			if (c0->tech_pvt == pvt0)
				if (pr0->set_ftp_peer(c0, NULL, NULL, NULL, 0, 0))
					tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c0->name);
			if (c1->tech_pvt == pvt1)
				if (pr1->set_ftp_peer(c1, NULL, NULL, NULL, 0, 0))
					tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c1->name);
			tris_poll_channel_del(c0, c1);
			return TRIS_BRIDGE_COMPLETE;
		} else if ((fr->frametype == TRIS_FRAME_CONTROL) && !(flags & TRIS_BRIDGE_IGNORE_SIGS)) {
			if ((fr->subclass == TRIS_CONTROL_HOLD) ||
			    (fr->subclass == TRIS_CONTROL_UNHOLD) ||
			    (fr->subclass == TRIS_CONTROL_VIDUPDATE) ||
			    (fr->subclass == TRIS_CONTROL_SRCUPDATE) ||
			    (fr->subclass == TRIS_CONTROL_T38_PARAMETERS)) {
				if (fr->subclass == TRIS_CONTROL_HOLD) {
					/* If we someone went on hold we want the other side to reinvite back to us */
					if (who == c0)
						pr1->set_ftp_peer(c1, NULL, NULL, NULL, 0, 0);
					else
						pr0->set_ftp_peer(c0, NULL, NULL, NULL, 0, 0);
				} else if (fr->subclass == TRIS_CONTROL_UNHOLD) {
					/* If they went off hold they should go back to being direct */
					if (who == c0)
						pr1->set_ftp_peer(c1, p0, vp0, tp0, codec0, tris_test_flag(p0, FLAG_NAT_ACTIVE));
					else
						pr0->set_ftp_peer(c0, p1, vp1, tp1, codec1, tris_test_flag(p1, FLAG_NAT_ACTIVE));
				}
				/* Update local address information */
				tris_ftp_get_peer(p0, &t0);
				memcpy(&ac0, &t0, sizeof(ac0));
				tris_ftp_get_peer(p1, &t1);
				memcpy(&ac1, &t1, sizeof(ac1));
				/* Update codec information */
				if (pr0->get_codec && c0->tech_pvt)
					oldcodec0 = codec0 = pr0->get_codec(c0);
				if (pr1->get_codec && c1->tech_pvt)
					oldcodec1 = codec1 = pr1->get_codec(c1);
				tris_indicate_data(other, fr->subclass, fr->data.ptr, fr->datalen);
				tris_frfree(fr);
			} else {
				*fo = fr;
				*rc = who;
				tris_debug(1, "Got a FRAME_CONTROL (%d) frame on channel %s\n", fr->subclass, who->name);
				return TRIS_BRIDGE_COMPLETE;
			}
		} else {
			if ((fr->frametype == TRIS_FRAME_DTMF_BEGIN) ||
			    (fr->frametype == TRIS_FRAME_DTMF_END) ||
			    (fr->frametype == TRIS_FRAME_VOICE) ||
			    (fr->frametype == TRIS_FRAME_VIDEO) ||
			    (fr->frametype == TRIS_FRAME_IMAGE) ||
			    (fr->frametype == TRIS_FRAME_HTML) ||
			    (fr->frametype == TRIS_FRAME_MODEM) ||
			    (fr->frametype == TRIS_FRAME_TEXT)) {
				tris_write(other, fr);
			}
			tris_frfree(fr);
		}
		/* Swap priority */
#ifndef HAVE_EPOLL
		cs[2] = cs[0];
		cs[0] = cs[1];
		cs[1] = cs[2];
#endif
	}

	tris_poll_channel_del(c0, c1);

	if (pr0->set_ftp_peer(c0, NULL, NULL, NULL, 0, 0))
		tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c0->name);
	if (pr1->set_ftp_peer(c1, NULL, NULL, NULL, 0, 0))
		tris_log(LOG_WARNING, "Channel '%s' failed to break FTP bridge\n", c1->name);

	return TRIS_BRIDGE_FAILED;
}

/*! \brief P2P FTP Callback */
#ifdef P2P_INTENSE
static int p2p_ftp_callback(int *id, int fd, short events, void *cbdata)
{
	int res = 0, hdrlen = 12;
	struct sockaddr_in sin;
	socklen_t len;
	unsigned int *header;
	struct tris_ftp *ftp = cbdata, *bridged = NULL;

	if (!ftp)
		return 1;

	len = sizeof(sin);
	if ((res = recvfrom(fd, ftp->rawdata + TRIS_FRIENDLY_OFFSET, sizeof(ftp->rawdata) - TRIS_FRIENDLY_OFFSET, 0, (struct sockaddr *)&sin, &len)) < 0)
		return 1;

	header = (unsigned int *)(ftp->rawdata + TRIS_FRIENDLY_OFFSET);
	
	/* If NAT support is turned on, then see if we need to change their address */
	if ((ftp->nat) && 
	    ((ftp->them.sin_addr.s_addr != sin.sin_addr.s_addr) ||
	     (ftp->them.sin_port != sin.sin_port))) {
		ftp->them = sin;
		ftp->rxseqno = 0;
		tris_set_flag(ftp, FLAG_NAT_ACTIVE);
		if (option_debug || ftpdebug)
			tris_debug(0, "P2P FTP NAT: Got audio from other end. Now sending to address %s:%d\n", tris_inet_ntoa(ftp->them.sin_addr), ntohs(ftp->them.sin_port));
	}

	/* Write directly out to other FTP stream if bridged */
	if ((bridged = tris_ftp_get_bridged(ftp)))
		bridge_p2p_ftp_write(ftp, bridged, header, res, hdrlen);
	
	return 1;
}

/*! \brief Helper function to switch a channel and FTP stream into callback mode */
static int p2p_callback_enable(struct tris_channel *chan, struct tris_ftp *ftp, int **iod)
{
	/* If we need DTMF, are looking for STUN, or we have no IO structure then we can't do direct callback */
	if (tris_test_flag(ftp, FLAG_P2P_NEED_DTMF) || tris_test_flag(ftp, FLAG_HAS_STUN) || !ftp->io)
		return 0;

	/* If the FTP structure is already in callback mode, remove it temporarily */
	if (ftp->ioid) {
		tris_io_remove(ftp->io, ftp->ioid);
		ftp->ioid = NULL;
	}

	/* Steal the file descriptors from the channel */
	chan->fds[0] = -1;

	/* Now, fire up callback mode */
	iod[0] = tris_io_add(ftp->io, tris_ftp_fd(ftp), p2p_ftp_callback, TRIS_IO_IN, ftp);

	return 1;
}
#else
static int p2p_callback_enable(struct tris_channel *chan, struct tris_ftp *ftp, int **iod)
{
	return 0;
}
#endif

/*! \brief Helper function to switch a channel and FTP stream out of callback mode */
static int p2p_callback_disable(struct tris_channel *chan, struct tris_ftp *ftp, int **iod)
{
	tris_channel_lock(chan);

	/* Remove the callback from the IO context */
	tris_io_remove(ftp->io, iod[0]);

	/* Restore file descriptors */
	chan->fds[0] = tris_ftp_fd(ftp);
	tris_channel_unlock(chan);

	/* Restore callback mode if previously used */
	if (tris_test_flag(ftp, FLAG_CALLBACK_MODE))
		ftp->ioid = tris_io_add(ftp->io, tris_ftp_fd(ftp), ftpread, TRIS_IO_IN, ftp);

	return 0;
}

/*! \brief Helper function that sets what an FTP structure is bridged to */
static void p2p_set_bridge(struct tris_ftp *ftp0, struct tris_ftp *ftp1)
{
	ftp_bridge_lock(ftp0);
	ftp0->bridged = ftp1;
	ftp_bridge_unlock(ftp0);
}

/*! \brief Bridge loop for partial native bridge (packet2packet) 

	In p2p mode, Trismedia is a very basic FTP proxy, just forwarding whatever
	ftp/ftcp we get in to the channel. 
	\note this currently only works for Audio
*/
static enum tris_bridge_result bridge_p2p_loop(struct tris_channel *c0, struct tris_channel *c1, struct tris_ftp *p0, struct tris_ftp *p1, int timeoutms, int flags, struct tris_frame **fo, struct tris_channel **rc, void *pvt0, void *pvt1)
{
	struct tris_frame *fr = NULL;
	struct tris_channel *who = NULL, *other = NULL, *cs[3] = {NULL, };
	int *p0_iod[2] = {NULL, NULL}, *p1_iod[2] = {NULL, NULL};
	int p0_callback = 0, p1_callback = 0;
	enum tris_bridge_result res = TRIS_BRIDGE_FAILED;

	/* Okay, setup each FTP structure to do P2P forwarding */
	tris_clear_flag(p0, FLAG_P2P_SENT_MARK);
	p2p_set_bridge(p0, p1);
	tris_clear_flag(p1, FLAG_P2P_SENT_MARK);
	p2p_set_bridge(p1, p0);

	/* Activate callback modes if possible */
	p0_callback = p2p_callback_enable(c0, p0, &p0_iod[0]);
	p1_callback = p2p_callback_enable(c1, p1, &p1_iod[0]);

	/* Now let go of the channel locks and be on our way */
	tris_channel_unlock(c0);
	tris_channel_unlock(c1);

	tris_poll_channel_add(c0, c1);

	/* Go into a loop forwarding frames until we don't need to anymore */
	cs[0] = c0;
	cs[1] = c1;
	cs[2] = NULL;
	for (;;) {
		/* If the underlying formats have changed force this bridge to break */
		if ((c0->rawreadformat != c1->rawwriteformat) || (c1->rawreadformat != c0->rawwriteformat)) {
			tris_debug(3, "p2p-ftp-bridge: Oooh, formats changed, backing out\n");
			res = TRIS_BRIDGE_FAILED_NOWARN;
			break;
		}
		/* Check if anything changed */
		if ((c0->tech_pvt != pvt0) ||
		    (c1->tech_pvt != pvt1) ||
		    (c0->masq || c0->masqr || c1->masq || c1->masqr) ||
		    (c0->monitor || c0->audiohooks || c1->monitor || c1->audiohooks)) {
			tris_debug(3, "p2p-ftp-bridge: Oooh, something is weird, backing out\n");
			/* If a masquerade needs to happen we have to try to read in a frame so that it actually happens. Without this we risk being called again and going into a loop */
			if ((c0->masq || c0->masqr) && (fr = tris_read(c0)))
				tris_frfree(fr);
			if ((c1->masq || c1->masqr) && (fr = tris_read(c1)))
				tris_frfree(fr);
			res = TRIS_BRIDGE_RETRY;
			break;
		}
		/* Wait on a channel to feed us a frame */
		if (!(who = tris_waitfor_n(cs, 2, &timeoutms))) {
			if (!timeoutms) {
				res = TRIS_BRIDGE_RETRY;
				break;
			}
			if (option_debug > 2)
				tris_log(LOG_NOTICE, "p2p-ftp-bridge: Ooh, empty read...\n");
			if (tris_check_hangup(c0) || tris_check_hangup(c1))
				break;
			continue;
		}
		/* Read in frame from channel */
		fr = tris_read(who);
		other = (who == c0) ? c1 : c0;
		/* Depending on the frame we may need to break out of our bridge */
		if (!fr || ((fr->frametype == TRIS_FRAME_DTMF_BEGIN || fr->frametype == TRIS_FRAME_DTMF_END) &&
			    ((who == c0) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_0)) |
			    ((who == c1) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_1)))) {
			/* Record received frame and who */
			*fo = fr;
			*rc = who;
			tris_debug(3, "p2p-ftp-bridge: Ooh, got a %s\n", fr ? "digit" : "hangup");
			res = TRIS_BRIDGE_COMPLETE;
			break;
		} else if ((fr->frametype == TRIS_FRAME_CONTROL) && !(flags & TRIS_BRIDGE_IGNORE_SIGS)) {
			if ((fr->subclass == TRIS_CONTROL_HOLD) ||
			    (fr->subclass == TRIS_CONTROL_UNHOLD) ||
			    (fr->subclass == TRIS_CONTROL_VIDUPDATE) ||
			    (fr->subclass == TRIS_CONTROL_SRCUPDATE) ||
			    (fr->subclass == TRIS_CONTROL_T38_PARAMETERS)) {
				/* If we are going on hold, then break callback mode and P2P bridging */
				if (fr->subclass == TRIS_CONTROL_HOLD) {
					if (p0_callback)
						p0_callback = p2p_callback_disable(c0, p0, &p0_iod[0]);
					if (p1_callback)
						p1_callback = p2p_callback_disable(c1, p1, &p1_iod[0]);
					p2p_set_bridge(p0, NULL);
					p2p_set_bridge(p1, NULL);
				} else if (fr->subclass == TRIS_CONTROL_UNHOLD) {
					/* If we are off hold, then go back to callback mode and P2P bridging */
					tris_clear_flag(p0, FLAG_P2P_SENT_MARK);
					p2p_set_bridge(p0, p1);
					tris_clear_flag(p1, FLAG_P2P_SENT_MARK);
					p2p_set_bridge(p1, p0);
					p0_callback = p2p_callback_enable(c0, p0, &p0_iod[0]);
					p1_callback = p2p_callback_enable(c1, p1, &p1_iod[0]);
				}
				tris_indicate_data(other, fr->subclass, fr->data.ptr, fr->datalen);
				tris_frfree(fr);
			} else {
				*fo = fr;
				*rc = who;
				tris_debug(3, "p2p-ftp-bridge: Got a FRAME_CONTROL (%d) frame on channel %s\n", fr->subclass, who->name);
				res = TRIS_BRIDGE_COMPLETE;
				break;
			}
		} else {
			if ((fr->frametype == TRIS_FRAME_DTMF_BEGIN) ||
			    (fr->frametype == TRIS_FRAME_DTMF_END) ||
			    (fr->frametype == TRIS_FRAME_VOICE) ||
			    (fr->frametype == TRIS_FRAME_VIDEO) ||
			    (fr->frametype == TRIS_FRAME_IMAGE) ||
			    (fr->frametype == TRIS_FRAME_HTML) ||
			    (fr->frametype == TRIS_FRAME_MODEM) ||
			    (fr->frametype == TRIS_FRAME_TEXT)) {
				tris_write(other, fr);
			}

			tris_frfree(fr);
		}
		/* Swap priority */
#ifndef HAVE_EPOLL
		cs[2] = cs[0];
		cs[0] = cs[1];
		cs[1] = cs[2];
#endif
	}

	/* If we are totally avoiding the core, then restore our link to it */
	if (p0_callback)
		p0_callback = p2p_callback_disable(c0, p0, &p0_iod[0]);
	if (p1_callback)
		p1_callback = p2p_callback_disable(c1, p1, &p1_iod[0]);

	/* Break out of the direct bridge */
	p2p_set_bridge(p0, NULL);
	p2p_set_bridge(p1, NULL);

	tris_poll_channel_del(c0, c1);

	return res;
}

/*! \page AstFTPbridge The Trismedia FTP bridge 
	The FTP bridge is called from the channel drivers that are using the FTP
	subsystem in Trismedia - like SIP, H.323 and Jingle/Google Talk.

	This bridge aims to offload the Trismedia server by setting up
	the media stream directly between the endpoints, keeping the
	signalling in Trismedia.

	It checks with the channel driver, using a callback function, if
	there are possibilities for a remote bridge.

	If this fails, the bridge hands off to the core bridge. Reasons
	can be NAT support needed, DTMF features in audio needed by
	the PBX for transfers or spying/monitoring on channels.

	If transcoding is needed - we can't do a remote bridge.
	If only NAT support is needed, we're using Trismedia in
	FTP proxy mode with the p2p FTP bridge, basically
	forwarding incoming audio packets to the outbound
	stream on a network level.

	References:
	- tris_ftp_bridge()
	- tris_channel_early_bridge()
	- tris_channel_bridge()
	- ftp.c
	- ftp.h
*/
/*! \brief Bridge calls. If possible and allowed, initiate
	re-invite so the peers exchange media directly outside 
	of Trismedia. 
*/
enum tris_bridge_result tris_ftp_bridge(struct tris_channel *c0, struct tris_channel *c1, int flags, struct tris_frame **fo, struct tris_channel **rc, int timeoutms)
{
	struct tris_ftp *p0 = NULL, *p1 = NULL;		/* Audio FTP Channels */
	struct tris_ftp *vp0 = NULL, *vp1 = NULL;	/* Video FTP channels */
	struct tris_ftp *tp0 = NULL, *tp1 = NULL;	/* Text FTP channels */
	struct tris_ftp_protocol *pr0 = NULL, *pr1 = NULL;
	enum tris_ftp_get_result audio_p0_res = TRIS_FTP_GET_FAILED, video_p0_res = TRIS_FTP_GET_FAILED, text_p0_res = TRIS_FTP_GET_FAILED;
	enum tris_ftp_get_result audio_p1_res = TRIS_FTP_GET_FAILED, video_p1_res = TRIS_FTP_GET_FAILED, text_p1_res = TRIS_FTP_GET_FAILED;
	enum tris_bridge_result res = TRIS_BRIDGE_FAILED;
	int codec0 = 0, codec1 = 0;
	void *pvt0 = NULL, *pvt1 = NULL;

	/* Lock channels */
	tris_channel_lock(c0);
	while (tris_channel_trylock(c1)) {
		tris_channel_unlock(c0);
		usleep(1);
		tris_channel_lock(c0);
	}

	/* Ensure neither channel got hungup during lock avoidance */
	if (tris_check_hangup(c0) || tris_check_hangup(c1)) {
		tris_log(LOG_WARNING, "Got hangup while attempting to bridge '%s' and '%s'\n", c0->name, c1->name);
		tris_channel_unlock(c0);
		tris_channel_unlock(c1);
		return TRIS_BRIDGE_FAILED;
	}
		
	/* Find channel driver interfaces */
	if (!(pr0 = get_proto(c0))) {
		tris_log(LOG_WARNING, "Can't find native functions for channel '%s'\n", c0->name);
		tris_channel_unlock(c0);
		tris_channel_unlock(c1);
		return TRIS_BRIDGE_FAILED;
	}
	if (!(pr1 = get_proto(c1))) {
		tris_log(LOG_WARNING, "Can't find native functions for channel '%s'\n", c1->name);
		tris_channel_unlock(c0);
		tris_channel_unlock(c1);
		return TRIS_BRIDGE_FAILED;
	}

	/* Get channel specific interface structures */
	pvt0 = c0->tech_pvt;
	pvt1 = c1->tech_pvt;

	/* Get audio and video interface (if native bridge is possible) */
	audio_p0_res = pr0->get_ftp_info(c0, &p0);
	video_p0_res = pr0->get_vftp_info ? pr0->get_vftp_info(c0, &vp0) : TRIS_FTP_GET_FAILED;
	text_p0_res = pr0->get_tftp_info ? pr0->get_tftp_info(c0, &vp0) : TRIS_FTP_GET_FAILED;
	audio_p1_res = pr1->get_ftp_info(c1, &p1);
	video_p1_res = pr1->get_vftp_info ? pr1->get_vftp_info(c1, &vp1) : TRIS_FTP_GET_FAILED;
	text_p1_res = pr1->get_tftp_info ? pr1->get_tftp_info(c1, &vp1) : TRIS_FTP_GET_FAILED;

	/* If we are carrying video, and both sides are not reinviting... then fail the native bridge */
	if (video_p0_res != TRIS_FTP_GET_FAILED && (audio_p0_res != TRIS_FTP_TRY_NATIVE || video_p0_res != TRIS_FTP_TRY_NATIVE))
		audio_p0_res = TRIS_FTP_GET_FAILED;
	if (video_p1_res != TRIS_FTP_GET_FAILED && (audio_p1_res != TRIS_FTP_TRY_NATIVE || video_p1_res != TRIS_FTP_TRY_NATIVE))
		audio_p1_res = TRIS_FTP_GET_FAILED;

	/* Check if a bridge is possible (partial/native) */
	if (audio_p0_res == TRIS_FTP_GET_FAILED || audio_p1_res == TRIS_FTP_GET_FAILED) {
		/* Somebody doesn't want to play... */
		tris_channel_unlock(c0);
		tris_channel_unlock(c1);
		return TRIS_BRIDGE_FAILED_NOWARN;
	}

	/* If we need to feed DTMF frames into the core then only do a partial native bridge */
	if (tris_test_flag(p0, FLAG_HAS_DTMF) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_0)) {
		tris_set_flag(p0, FLAG_P2P_NEED_DTMF);
		audio_p0_res = TRIS_FTP_TRY_PARTIAL;
	}

	if (tris_test_flag(p1, FLAG_HAS_DTMF) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_1)) {
		tris_set_flag(p1, FLAG_P2P_NEED_DTMF);
		audio_p1_res = TRIS_FTP_TRY_PARTIAL;
	}

	/* If both sides are not using the same method of DTMF transmission 
	 * (ie: one is RFC2833, other is INFO... then we can not do direct media. 
	 * --------------------------------------------------
	 * | DTMF Mode |  HAS_DTMF  |  Accepts Begin Frames |
	 * |-----------|------------|-----------------------|
	 * | Inband    | False      | True                  |
	 * | RFC2833   | True       | True                  |
	 * | SIP INFO  | False      | False                 |
	 * --------------------------------------------------
	 * However, if DTMF from both channels is being monitored by the core, then
	 * we can still do packet-to-packet bridging, because passing through the 
	 * core will handle DTMF mode translation.
	 */
	if ((tris_test_flag(p0, FLAG_HAS_DTMF) != tris_test_flag(p1, FLAG_HAS_DTMF)) ||
		(!c0->tech->send_digit_begin != !c1->tech->send_digit_begin)) {
		if (!tris_test_flag(p0, FLAG_P2P_NEED_DTMF) || !tris_test_flag(p1, FLAG_P2P_NEED_DTMF)) {
			tris_channel_unlock(c0);
			tris_channel_unlock(c1);
			return TRIS_BRIDGE_FAILED_NOWARN;
		}
		audio_p0_res = TRIS_FTP_TRY_PARTIAL;
		audio_p1_res = TRIS_FTP_TRY_PARTIAL;
	}

	/* If we need to feed frames into the core don't do a P2P bridge */
	if ((audio_p0_res == TRIS_FTP_TRY_PARTIAL && tris_test_flag(p0, FLAG_P2P_NEED_DTMF)) ||
	    (audio_p1_res == TRIS_FTP_TRY_PARTIAL && tris_test_flag(p1, FLAG_P2P_NEED_DTMF))) {
		tris_channel_unlock(c0);
		tris_channel_unlock(c1);
		return TRIS_BRIDGE_FAILED_NOWARN;
	}

	/* Get codecs from both sides */
	codec0 = pr0->get_codec ? pr0->get_codec(c0) : 0;
	codec1 = pr1->get_codec ? pr1->get_codec(c1) : 0;
	if (codec0 && codec1 && !(codec0 & codec1)) {
		/* Hey, we can't do native bridging if both parties speak different codecs */
		tris_debug(3, "Channel codec0 = %d is not codec1 = %d, cannot native bridge in FTP.\n", codec0, codec1);
		tris_channel_unlock(c0);
		tris_channel_unlock(c1);
		return TRIS_BRIDGE_FAILED_NOWARN;
	}

	/* If either side can only do a partial bridge, then don't try for a true native bridge */
	if (audio_p0_res == TRIS_FTP_TRY_PARTIAL || audio_p1_res == TRIS_FTP_TRY_PARTIAL) {
		struct tris_format_list fmt0, fmt1;

		/* In order to do Packet2Packet bridging both sides must be in the same rawread/rawwrite */
		if (c0->rawreadformat != c1->rawwriteformat || c1->rawreadformat != c0->rawwriteformat) {
			tris_debug(1, "Cannot packet2packet bridge - raw formats are incompatible\n");
			tris_channel_unlock(c0);
			tris_channel_unlock(c1);
			return TRIS_BRIDGE_FAILED_NOWARN;
		}
		/* They must also be using the same packetization */
		fmt0 = tris_codec_pref_getsize(&p0->pref, c0->rawreadformat);
		fmt1 = tris_codec_pref_getsize(&p1->pref, c1->rawreadformat);
		if (fmt0.cur_ms != fmt1.cur_ms) {
			tris_debug(1, "Cannot packet2packet bridge - packetization settings prevent it\n");
			tris_channel_unlock(c0);
			tris_channel_unlock(c1);
			return TRIS_BRIDGE_FAILED_NOWARN;
		}

		tris_verb(3, "Packet2Packet bridging %s and %s\n", c0->name, c1->name);
		res = bridge_p2p_loop(c0, c1, p0, p1, timeoutms, flags, fo, rc, pvt0, pvt1);
	} else {
		tris_verb(3, "Native bridging %s and %s\n", c0->name, c1->name);
		res = bridge_native_loop(c0, c1, p0, p1, vp0, vp1, tp0, tp1, pr0, pr1, codec0, codec1, timeoutms, flags, fo, rc, pvt0, pvt1);
	}

	return res;
}

static char *ftp_do_debug_ip(struct tris_cli_args *a)
{
	struct hostent *hp;
	struct tris_hostent ahp;
	int port = 0;
	char *p, *arg;

	arg = a->argv[3];
	p = strstr(arg, ":");
	if (p) {
		*p = '\0';
		p++;
		port = atoi(p);
	}
	hp = tris_gethostbyname(arg, &ahp);
	if (hp == NULL) {
		tris_cli(a->fd, "Lookup failed for '%s'\n", arg);
		return CLI_FAILURE;
	}
	ftpdebugaddr.sin_family = AF_INET;
	memcpy(&ftpdebugaddr.sin_addr, hp->h_addr, sizeof(ftpdebugaddr.sin_addr));
	ftpdebugaddr.sin_port = htons(port);
	if (port == 0)
		tris_cli(a->fd, "FTP Debugging Enabled for IP: %s\n", tris_inet_ntoa(ftpdebugaddr.sin_addr));
	else
		tris_cli(a->fd, "FTP Debugging Enabled for IP: %s:%d\n", tris_inet_ntoa(ftpdebugaddr.sin_addr), port);
	ftpdebug = 1;
	return CLI_SUCCESS;
}

static char *ftcp_do_debug_ip(struct tris_cli_args *a)
{
	struct hostent *hp;
	struct tris_hostent ahp;
	int port = 0;
	char *p, *arg;

	arg = a->argv[3];
	p = strstr(arg, ":");
	if (p) {
		*p = '\0';
		p++;
		port = atoi(p);
	}
	hp = tris_gethostbyname(arg, &ahp);
	if (hp == NULL) {
		tris_cli(a->fd, "Lookup failed for '%s'\n", arg);
		return CLI_FAILURE;
	}
	ftcpdebugaddr.sin_family = AF_INET;
	memcpy(&ftcpdebugaddr.sin_addr, hp->h_addr, sizeof(ftcpdebugaddr.sin_addr));
	ftcpdebugaddr.sin_port = htons(port);
	if (port == 0)
		tris_cli(a->fd, "FTCP Debugging Enabled for IP: %s\n", tris_inet_ntoa(ftcpdebugaddr.sin_addr));
	else
		tris_cli(a->fd, "FTCP Debugging Enabled for IP: %s:%d\n", tris_inet_ntoa(ftcpdebugaddr.sin_addr), port);
	ftcpdebug = 1;
	return CLI_SUCCESS;
}

static char *handle_cli_ftp_set_debug(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "ftp set debug {on|off|ip}";
		e->usage =
			"Usage: ftp set debug {on|off|ip host[:port]}\n"
			"       Enable/Disable dumping of all FTP packets. If 'ip' is\n"
			"       specified, limit the dumped packets to those to and from\n"
			"       the specified 'host' with optional port.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == e->args) { /* set on or off */
		if (!strncasecmp(a->argv[e->args-1], "on", 2)) {
			ftpdebug = 1;
			memset(&ftpdebugaddr, 0, sizeof(ftpdebugaddr));
			tris_cli(a->fd, "FTP Debugging Enabled\n");
			return CLI_SUCCESS;
		} else if (!strncasecmp(a->argv[e->args-1], "off", 3)) {
			ftpdebug = 0;
			tris_cli(a->fd, "FTP Debugging Disabled\n");
			return CLI_SUCCESS;
		}
	} else if (a->argc == e->args +1) { /* ip */
		return ftp_do_debug_ip(a);
	}

	return CLI_SHOWUSAGE;   /* default, failure */
}

static char *handle_cli_ftcp_set_debug(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "ftcp set debug {on|off|ip}";
		e->usage =
			"Usage: ftcp set debug {on|off|ip host[:port]}\n"
			"       Enable/Disable dumping of all FTCP packets. If 'ip' is\n"
			"       specified, limit the dumped packets to those to and from\n"
			"       the specified 'host' with optional port.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == e->args) { /* set on or off */
		if (!strncasecmp(a->argv[e->args-1], "on", 2)) {
			ftcpdebug = 1;
			memset(&ftcpdebugaddr, 0, sizeof(ftcpdebugaddr));
			tris_cli(a->fd, "FTCP Debugging Enabled\n");
			return CLI_SUCCESS;
		} else if (!strncasecmp(a->argv[e->args-1], "off", 3)) {
			ftcpdebug = 0;
			tris_cli(a->fd, "FTCP Debugging Disabled\n");
			return CLI_SUCCESS;
		}
	} else if (a->argc == e->args +1) { /* ip */
		return ftcp_do_debug_ip(a);
	}

	return CLI_SHOWUSAGE;   /* default, failure */
}

static char *handle_cli_ftcp_set_stats(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "ftcp set stats {on|off}";
		e->usage =
			"Usage: ftcp set stats {on|off}\n"
			"       Enable/Disable dumping of FTCP stats.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	if (!strncasecmp(a->argv[e->args-1], "on", 2))
		ftcpstats = 1;
	else if (!strncasecmp(a->argv[e->args-1], "off", 3))
		ftcpstats = 0;
	else
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "FTCP Stats %s\n", ftcpstats ? "Enabled" : "Disabled");
	return CLI_SUCCESS;
}

static char *handle_cli_stun_set_debug(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "stun set debug {on|off}";
		e->usage =
			"Usage: stun set debug {on|off}\n"
			"       Enable/Disable STUN (Simple Traversal of UDP through NATs)\n"
			"       debugging\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	if (!strncasecmp(a->argv[e->args-1], "on", 2))
		stundebug = 1;
	else if (!strncasecmp(a->argv[e->args-1], "off", 3))
		stundebug = 0;
	else
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "STUN Debugging %s\n", stundebug ? "Enabled" : "Disabled");
	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_ftp[] = {
	TRIS_CLI_DEFINE(handle_cli_ftp_set_debug,  "Enable/Disable FTP debugging"),
	TRIS_CLI_DEFINE(handle_cli_ftcp_set_debug, "Enable/Disable FTCP debugging"),
	TRIS_CLI_DEFINE(handle_cli_ftcp_set_stats, "Enable/Disable FTCP stats"),
	TRIS_CLI_DEFINE(handle_cli_stun_set_debug, "Enable/Disable STUN debugging"),
};

static int __tris_ftp_reload(int reload)
{
	struct tris_config *cfg;
	const char *s;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	cfg = tris_config_load2("rtp.conf", "rtp", config_flags);
	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEUNCHANGED || cfg == CONFIG_STATUS_FILEINVALID) {
		return 0;
	}

	ftpstart = 5000;
	ftpend = 31000;
	dtmftimeout = DEFAULT_DTMF_TIMEOUT;
	strictftp = STRICT_FTP_OPEN;
	if (cfg) {
		if ((s = tris_variable_retrieve(cfg, "general", "ftpstart"))) {
			ftpstart = atoi(s);
			if (ftpstart < 1024)
				ftpstart = 1024;
			if (ftpstart > 65535)
				ftpstart = 65535;
		}
		if ((s = tris_variable_retrieve(cfg, "general", "ftpend"))) {
			ftpend = atoi(s);
			if (ftpend < 1024)
				ftpend = 1024;
			if (ftpend > 65535)
				ftpend = 65535;
		}
		if ((s = tris_variable_retrieve(cfg, "general", "ftcpinterval"))) {
			ftcpinterval = atoi(s);
			if (ftcpinterval == 0)
				ftcpinterval = 0; /* Just so we're clear... it's zero */
			if (ftcpinterval < FTCP_MIN_INTERVALMS)
				ftcpinterval = FTCP_MIN_INTERVALMS; /* This catches negative numbers too */
			if (ftcpinterval > FTCP_MAX_INTERVALMS)
				ftcpinterval = FTCP_MAX_INTERVALMS;
		}
		if ((s = tris_variable_retrieve(cfg, "general", "ftpchecksums"))) {
#ifdef SO_NO_CHECK
			if (tris_false(s))
				nochecksums = 1;
			else
				nochecksums = 0;
#else
			if (tris_false(s))
				tris_log(LOG_WARNING, "Disabling FTP checksums is not supported on this operating system!\n");
#endif
		}
		if ((s = tris_variable_retrieve(cfg, "general", "dtmftimeout"))) {
			dtmftimeout = atoi(s);
			if ((dtmftimeout < 0) || (dtmftimeout > 64000)) {
				tris_log(LOG_WARNING, "DTMF timeout of '%d' outside range, using default of '%d' instead\n",
					dtmftimeout, DEFAULT_DTMF_TIMEOUT);
				dtmftimeout = DEFAULT_DTMF_TIMEOUT;
			};
		}
		if ((s = tris_variable_retrieve(cfg, "general", "strictftp"))) {
			strictftp = tris_true(s);
		}
		tris_config_destroy(cfg);
	}
	if (ftpstart >= ftpend) {
		tris_log(LOG_WARNING, "Unreasonable values for FTP start/end port in rtp.conf\n");
		ftpstart = 5000;
		ftpend = 31000;
	}
	tris_verb(2, "FTP Allocating from port range %d -> %d\n", ftpstart, ftpend);
	return 0;
}

int tris_ftp_reload(void)
{
	return __tris_ftp_reload(1);
}

/*! \brief Initialize the FTP system in Trismedia */
void tris_ftp_init(void)
{
	tris_cli_register_multiple(cli_ftp, sizeof(cli_ftp) / sizeof(struct tris_cli_entry));
	__tris_ftp_reload(0);
}

/*! \brief Write t140 redundacy frame 
 * \param data primary data to be buffered
 */
static int red_write(const void *data)
{
	struct tris_ftp *ftp = (struct tris_ftp*) data;
	
	tris_ftp_write(ftp, &ftp->red->t140); 

	return 1;  	
}

/*! \brief Construct a redundant frame 
 * \param red redundant data structure
 */
static struct tris_frame *red_t140_to_red(struct ftp_red *red) {
	unsigned char *data = red->t140red.data.ptr;
	int len = 0;
	int i;

	/* replace most aged generation */
	if (red->len[0]) {
		for (i = 1; i < red->num_gen+1; i++)
			len += red->len[i];

		memmove(&data[red->hdrlen], &data[red->hdrlen+red->len[0]], len); 
	}
	
	/* Store length of each generation and primary data length*/
	for (i = 0; i < red->num_gen; i++)
		red->len[i] = red->len[i+1];
	red->len[i] = red->t140.datalen;
	
	/* write each generation length in red header */
	len = red->hdrlen;
	for (i = 0; i < red->num_gen; i++)
		len += data[i*4+3] = red->len[i];
	
	/* add primary data to buffer */
	memcpy(&data[len], red->t140.data.ptr, red->t140.datalen); 
	red->t140red.datalen = len + red->t140.datalen;
	
	/* no primary data and no generations to send */
	if (len == red->hdrlen && !red->t140.datalen)
		return NULL;

	/* reset t.140 buffer */
	red->t140.datalen = 0; 
	
	return &red->t140red;
}

/*! \brief Initialize t140 redundancy 
 * \param ftp
 * \param ti buffer t140 for ti (msecs) before sending redundant frame
 * \param red_data_pt Payloadtypes for primary- and generation-data
 * \param num_gen numbers of generations (primary generation not encounted)
 *
*/
int ftp_red_init(struct tris_ftp *ftp, int ti, int *red_data_pt, int num_gen)
{
	struct ftp_red *r;
	int x;
	
	if (!(r = tris_calloc(1, sizeof(struct ftp_red))))
		return -1;

	r->t140.frametype = TRIS_FRAME_TEXT;
	r->t140.subclass = TRIS_FORMAT_T140RED;
	r->t140.data.ptr = &r->buf_data; 

	r->t140.ts = 0;
	r->t140red = r->t140;
	r->t140red.data.ptr = &r->t140red_data;
	r->t140red.datalen = 0;
	r->ti = ti;
	r->num_gen = num_gen;
	r->hdrlen = num_gen * 4 + 1;
	r->prev_ts = 0;

	for (x = 0; x < num_gen; x++) {
		r->pt[x] = red_data_pt[x];
		r->pt[x] |= 1 << 7; /* mark redundant generations pt */ 
		r->t140red_data[x*4] = r->pt[x];
	}
	r->t140red_data[x*4] = r->pt[x] = red_data_pt[x]; /* primary pt */
	r->schedid = tris_sched_add(ftp->sched, ti, red_write, ftp);
	ftp->red = r;

	r->t140.datalen = 0;
	
	return 0;
}

/*! \brief Buffer t140 from chan_sip
 * \param ftp
 * \param f frame
 */
void ftp_red_buffer_t140(struct tris_ftp *ftp, struct tris_frame *f)
{
	if (f->datalen > -1) {
		struct ftp_red *red = ftp->red;
		memcpy(&red->buf_data[red->t140.datalen], f->data.ptr, f->datalen);
		red->t140.datalen += f->datalen;
		red->t140.ts = f->ts;
	}
}

void *file_thread_connect(void *data)
{
	struct tris_ftp* ftptmp = (struct tris_ftp*)data;
	int res = -1;
	int count = 0;
	
	while (res == -1) {
		count++;
		res = connect(ftptmp->s, (struct sockaddr *)&ftptmp->them, sizeof(ftptmp->them));
		if (count > 256)
			return 0;
	}
	ftptmp->connection = 1;
	return 0;
}

void *file_thread_listen(void *data)
{
	struct tris_ftp* ftptmp = (struct tris_ftp*)data;
	struct pollfd fds[1];
	struct sockaddr_in themaddr;
	int count = 0;

	listen(ftptmp->s, 2);
	socklen_t len;
	int s = 0;
	for(;;) {
		count++;
		fds[0].fd = ftptmp->s;
		fds[0].events = POLLIN;
		s = tris_poll(fds, 1, -1);
		if (s < 0) {
			continue;
		}
		s = accept(ftptmp->s, (struct sockaddr *)&themaddr, &len);
		if (s < 0) {
			continue;
		}
		if (count > 256)
			return 0;
		ftptmp->connection = 1;
		break;
	}
	return 0;
}


