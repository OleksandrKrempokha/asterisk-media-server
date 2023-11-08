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
 * \file ftp.h
 * \brief Supports FTP and FTCP with Symmetric FTP support for NAT traversal.
 *
 * FTP is defined in RFC 3550.
 */

#ifndef _TRISMEDIA_FTP_H
#define _TRISMEDIA_FTP_H

#include "trismedia/network.h"

#include "trismedia/frame.h"
#include "trismedia/io.h"
#include "trismedia/sched.h"
#include "trismedia/channel.h"
#include "trismedia/linkedlists.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/* Codes for FTP-specific data - not defined by our TRIS_FORMAT codes */
/*! DTMF (RFC2833) */
#define TRIS_FTP_DTMF            	(1 << 0)
/*! 'Comfort Noise' (RFC3389) */
#define TRIS_FTP_CN              	(1 << 1)
/*! DTMF (Cisco Proprietary) */
#define TRIS_FTP_CISCO_DTMF      	(1 << 2)
/*! Maximum FTP-specific code */
#define TRIS_FTP_MAX             	TRIS_FTP_CISCO_DTMF

/*! Maxmum number of payload defintions for a FTP session */
#define MAX_FTP_PT			256

/*! T.140 Redundancy Maxium number of generations */
#define RED_MAX_GENERATION 5

#define FLAG_3389_WARNING		(1 << 0)

enum tris_ftp_options {
	TRIS_FTP_OPT_G726_NONSTANDARD = (1 << 0),
};

enum tris_ftp_get_result {
	/*! Failed to find the FTP structure */
	TRIS_FTP_GET_FAILED = 0,
	/*! FTP structure exists but true native bridge can not occur so try partial */
	TRIS_FTP_TRY_PARTIAL,
	/*! FTP structure exists and native bridge can occur */
	TRIS_FTP_TRY_NATIVE,
};

/*! \brief Variables used in tris_ftcp_get function */
enum tris_ftp_qos_vars {
	TRIS_FTP_TXCOUNT,
	TRIS_FTP_RXCOUNT,
	TRIS_FTP_TXJITTER,
	TRIS_FTP_RXJITTER,
	TRIS_FTP_RXPLOSS,
	TRIS_FTP_TXPLOSS,
	TRIS_FTP_RTT
};

struct tris_ftp;
/*! T.140 Redundancy structure*/
struct ftp_red;

/*! \brief The value of each payload format mapping: */
struct ftpPayloadType {
	int isAstFormat;		/*!< whether the following code is an TRIS_FORMAT */
	int code;
};

/*! \brief This is the structure that binds a channel (SIP/Jingle/H.323) to the FTP subsystem 
*/
struct tris_ftp_protocol {
	/*! Get FTP struct, or NULL if unwilling to transfer */
	enum tris_ftp_get_result (* const get_ftp_info)(struct tris_channel *chan, struct tris_ftp **ftp);
	/*! Get FTP struct, or NULL if unwilling to transfer */
	enum tris_ftp_get_result (* const get_vftp_info)(struct tris_channel *chan, struct tris_ftp **ftp);
	/*! Get FTP struct, or NULL if unwilling to transfer */
	enum tris_ftp_get_result (* const get_tftp_info)(struct tris_channel *chan, struct tris_ftp **ftp);
	/*! Set FTP peer */
	int (* const set_ftp_peer)(struct tris_channel *chan, struct tris_ftp *peer, struct tris_ftp *vpeer, struct tris_ftp *tpeer, int codecs, int nat_active);
	int (* const get_codec)(struct tris_channel *chan);
	const char * const type;
	TRIS_LIST_ENTRY(tris_ftp_protocol) list;
};

enum tris_ftp_quality_type {
	FTPQOS_SUMMARY = 0,
	FTPQOS_JITTER,
	FTPQOS_LOSS,
	FTPQOS_RTT
};

/*! \brief FTCP quality report storage */
struct tris_ftp_quality {
	unsigned int local_ssrc;          /*!< Our SSRC */
	unsigned int local_lostpackets;   /*!< Our lost packets */
	double       local_jitter;        /*!< Our calculated jitter */
	unsigned int local_count;         /*!< Number of received packets */
	unsigned int remote_ssrc;         /*!< Their SSRC */
	unsigned int remote_lostpackets;  /*!< Their lost packets */
	double       remote_jitter;       /*!< Their reported jitter */
	unsigned int remote_count;        /*!< Number of transmitted packets */
	double       rtt;                 /*!< Round trip time */
};

/*! FTP callback structure */
typedef int (*tris_ftp_callback)(struct tris_ftp *ftp, struct tris_frame *f, void *data);

/*!
 * \brief Get the amount of space required to hold an FTP session
 * \return number of bytes required
 */
size_t tris_ftp_alloc_size(void);

/*!
 * \brief Initializate a FTP session.
 *
 * \param sched
 * \param io
 * \param ftcpenable
 * \param callbackmode
 * \return A representation (structure) of an FTP session.
 */
struct tris_ftp *tris_ftp_new(struct sched_context *sched, struct io_context *io, int ftcpenable, int callbackmode);

/*!
 * \brief Initializate a FTP session using an in_addr structure.
 *
 * This fuction gets called by tris_ftp_new().
 *
 * \param sched
 * \param io
 * \param ftcpenable
 * \param callbackmode
 * \param in
 * \return A representation (structure) of an FTP session.
 */
struct tris_ftp *tris_ftp_new_with_bindaddr(struct sched_context *sched, struct io_context *io, int ftcpenable, int callbackmode, struct in_addr in);

void tris_ftp_set_peer(struct tris_ftp *ftp, struct sockaddr_in *them);

/*! 
 * \since 1.4.26
 * \brief set potential alternate source for FTP media
 *
 * This function may be used to give the FTP stack a hint that there is a potential
 * second source of media. One case where this is used is when the SIP stack receives
 * a REINVITE to which it will be replying with a 491. In such a scenario, the IP and
 * port information in the SDP of that REINVITE lets us know that we may receive media
 * from that source/those sources even though the SIP transaction was unable to be completed
 * successfully
 *
 * \param ftp The FTP structure we wish to set up an alternate host/port on
 * \param alt The address information for the alternate media source
 * \retval void
 */
void tris_ftp_set_alt_peer(struct tris_ftp *ftp, struct sockaddr_in *alt);

/* Copies from ftp to them and returns 1 if there was a change or 0 if it was already the same */
int tris_ftp_get_peer(struct tris_ftp *ftp, struct sockaddr_in *them);

void tris_ftp_get_us(struct tris_ftp *ftp, struct sockaddr_in *us);

struct tris_ftp *tris_ftp_get_bridged(struct tris_ftp *ftp);

/*! Destroy FTP session */
void tris_ftp_destroy(struct tris_ftp *ftp);

void tris_ftp_reset(struct tris_ftp *ftp);

/*! Stop FTP session, do not destroy structure */
void tris_ftp_stop(struct tris_ftp *ftp);

void tris_ftp_set_callback(struct tris_ftp *ftp, tris_ftp_callback callback);

void tris_ftp_set_data(struct tris_ftp *ftp, void *data);

int tris_ftp_write(struct tris_ftp *ftp, struct tris_frame *f);

struct tris_frame *tris_ftp_read(struct tris_ftp *ftp);

struct tris_frame *tris_ftcp_read(struct tris_ftp *ftp);

int tris_ftp_fd(struct tris_ftp *ftp);

int tris_ftcp_fd(struct tris_ftp *ftp);

int tris_ftp_senddigit_begin(struct tris_ftp *ftp, char digit);

int tris_ftp_senddigit_end(struct tris_ftp *ftp, char digit);

int tris_ftp_sendcng(struct tris_ftp *ftp, int level);

int tris_ftp_setqos(struct tris_ftp *ftp, int tos, int cos, char *desc);

/*! \brief When changing sources, don't generate a new SSRC */
void tris_ftp_set_constantssrc(struct tris_ftp *ftp);

void tris_ftp_new_source(struct tris_ftp *ftp);

/*! \brief  Setting FTP payload types from lines in a SDP description: */
void tris_ftp_pt_clear(struct tris_ftp* ftp);
/*! \brief Set payload types to defaults */
void tris_ftp_pt_default(struct tris_ftp* ftp);

/*! \brief Copy payload types between FTP structures */
void tris_ftp_pt_copy(struct tris_ftp *dest, struct tris_ftp *src);

/*! \brief Activate payload type */
void tris_ftp_set_m_type(struct tris_ftp* ftp, int pt);

/*! \brief clear payload type */
void tris_ftp_unset_m_type(struct tris_ftp* ftp, int pt);

/*! \brief Set payload type to a known MIME media type for a codec
 *
 * \param ftp FTP structure to modify
 * \param pt Payload type entry to modify
 * \param mimeType top-level MIME type of media stream (typically "audio", "video", "text", etc.)
 * \param mimeSubtype MIME subtype of media stream (typically a codec name)
 * \param options Zero or more flags from the tris_ftp_options enum
 *
 * This function 'fills in' an entry in the list of possible formats for
 * a media stream associated with an FTP structure.
 *
 * \retval 0 on success
 * \retval -1 if the payload type is out of range
 * \retval -2 if the mimeType/mimeSubtype combination was not found
 */
int tris_ftp_set_ftpmap_type(struct tris_ftp* ftp, int pt,
			     char *mimeType, char *mimeSubtype,
			     enum tris_ftp_options options);

/*! \brief Set payload type to a known MIME media type for a codec with a specific sample rate
 *
 * \param ftp FTP structure to modify
 * \param pt Payload type entry to modify
 * \param mimeType top-level MIME type of media stream (typically "audio", "video", "text", etc.)
 * \param mimeSubtype MIME subtype of media stream (typically a codec name)
 * \param options Zero or more flags from the tris_ftp_options enum
 * \param sample_rate The sample rate of the media stream
 *
 * This function 'fills in' an entry in the list of possible formats for
 * a media stream associated with an FTP structure.
 *
 * \retval 0 on success
 * \retval -1 if the payload type is out of range
 * \retval -2 if the mimeType/mimeSubtype combination was not found
 */
int tris_ftp_set_ftpmap_type_rate(struct tris_ftp* ftp, int pt,
				 char *mimeType, char *mimeSubtype,
				 enum tris_ftp_options options,
				 unsigned int sample_rate);

/*! \brief  Mapping between FTP payload format codes and Trismedia codes: */
struct ftpPayloadType tris_ftp_lookup_pt(struct tris_ftp* ftp, int pt);
int tris_ftp_lookup_code(struct tris_ftp* ftp, int isAstFormat, int code);

void tris_ftp_get_current_formats(struct tris_ftp* ftp,
				 int* astFormats, int* nonAstFormats);

/*! \brief  Mapping an Trismedia code into a MIME subtype (string): */
const char *tris_ftp_lookup_mime_subtype(int isAstFormat, int code,
					enum tris_ftp_options options);

/*! \brief Get the sample rate associated with known FTP payload types
 *
 * \param isAstFormat True if the value in the 'code' parameter is an TRIS_FORMAT value
 * \param code Format code, either from TRIS_FORMAT list or from TRIS_FTP list
 *
 * \return the sample rate if the format was found, zero if it was not found
 */
unsigned int tris_ftp_lookup_sample_rate(int isAstFormat, int code);

/*! \brief Build a string of MIME subtype names from a capability list */
char *tris_ftp_lookup_mime_multiple(char *buf, size_t size, const int capability,
				   const int isAstFormat, enum tris_ftp_options options);

void tris_ftp_setnat(struct tris_ftp *ftp, int nat);

int tris_ftp_getnat(struct tris_ftp *ftp);

/*! \brief Indicate whether this FTP session is carrying DTMF or not */
void tris_ftp_setdtmf(struct tris_ftp *ftp, int dtmf);

/*! \brief Compensate for devices that send RFC2833 packets all at once */
void tris_ftp_setdtmfcompensate(struct tris_ftp *ftp, int compensate);

/*! \brief Enable STUN capability */
void tris_ftp_setstun(struct tris_ftp *ftp, int stun_enable);

/*! \brief Generic STUN request
 * send a generic stun request to the server specified.
 * \param s the socket used to send the request
 * \param dst the address of the STUN server
 * \param username if non null, add the username in the request
 * \param answer if non null, the function waits for a response and
 *    puts here the externally visible address.
 * \return 0 on success, other values on error.
 * The interface it may change in the future.
 */
int tris_stun_ftp_request(int s, struct sockaddr_in *dst,
	const char *username, struct sockaddr_in *answer);

/*! \brief Send STUN request for an FTP socket
 * Deprecated, this is just a wrapper for tris_ftp_stun_request()
 */
void tris_ftp_stun_request(struct tris_ftp *ftp, struct sockaddr_in *suggestion, const char *username);

/*! \brief The FTP bridge.
	\arg \ref AstFTPbridge
*/
int tris_ftp_bridge(struct tris_channel *c0, struct tris_channel *c1, int flags, struct tris_frame **fo, struct tris_channel **rc, int timeoutms);

/*! \brief Register an FTP channel client */
int tris_ftp_proto_register(struct tris_ftp_protocol *proto);

/*! \brief Unregister an FTP channel client */
void tris_ftp_proto_unregister(struct tris_ftp_protocol *proto);

int tris_ftp_make_compatible(struct tris_channel *dest, struct tris_channel *src, int media);

/*! \brief If possible, create an early bridge directly between the devices without
           having to send a re-invite later */
int tris_ftp_early_bridge(struct tris_channel *c0, struct tris_channel *c1);

/*! \brief Get QOS stats on a FTP channel
 * \since 1.6.1
 */
int tris_ftp_get_qos(struct tris_ftp *ftp, const char *qos, char *buf, unsigned int buflen);

/*! \brief Return FTP and FTCP QoS values
 * \since 1.6.1
 */
unsigned int tris_ftp_get_qosvalue(struct tris_ftp *ftp, enum tris_ftp_qos_vars value);

/*! \brief Set FTPAUDIOQOS(...) variables on a channel when it is being hung up
 * \since 1.6.1
 */
void tris_ftp_set_vars(struct tris_channel *chan, struct tris_ftp *ftp);

/*! \brief Return FTCP quality string 
 *
 *  \param ftp An ftp structure to get qos information about.
 *
 *  \param qual An (optional) ftp quality structure that will be 
 *              filled with the quality information described in 
 *              the tris_ftp_quality structure. This structure is
 *              not dependent on any qtype, so a call for any
 *              type of information would yield the same results
 *              because tris_ftp_quality is not a data type 
 *              specific to any qos type.
 *
 *  \param qtype The quality type you'd like, default should be
 *               FTPQOS_SUMMARY which returns basic information
 *               about the call. The return from FTPQOS_SUMMARY
 *               is basically tris_ftp_quality in a string. The
 *               other types are FTPQOS_JITTER, FTPQOS_LOSS and
 *               FTPQOS_RTT which will return more specific 
 *               statistics.
 * \version 1.6.1 added qtype parameter
 */
char *tris_ftp_get_quality(struct tris_ftp *ftp, struct tris_ftp_quality *qual, enum tris_ftp_quality_type qtype);
/*! \brief Send an H.261 fast update request. Some devices need this rather than the XML message  in SIP */
int tris_ftcp_send_h261fur(void *data);

void tris_ftp_init(void);                                      /*! Initialize FTP subsystem */
int tris_ftp_reload(void);                                     /*! reload ftp configuration */
void tris_ftp_new_init(struct tris_ftp *ftp);

/*! \brief Set codec preference */
void tris_ftp_codec_setpref(struct tris_ftp *ftp, struct tris_codec_pref *prefs);

/*! \brief Get codec preference */
struct tris_codec_pref *tris_ftp_codec_getpref(struct tris_ftp *ftp);

/*! \brief get format from predefined dynamic payload format */
int tris_ftp_codec_getformat(int pt);

/*! \brief Set ftp timeout */
void tris_ftp_set_ftptimeout(struct tris_ftp *ftp, int timeout);
/*! \brief Set ftp hold timeout */
void tris_ftp_set_ftpholdtimeout(struct tris_ftp *ftp, int timeout);
/*! \brief set FTP keepalive interval */
void tris_ftp_set_ftpkeepalive(struct tris_ftp *ftp, int period);
/*! \brief Get FTP keepalive interval */
int tris_ftp_get_ftpkeepalive(struct tris_ftp *ftp);
/*! \brief Get ftp hold timeout */
int tris_ftp_get_ftpholdtimeout(struct tris_ftp *ftp);
/*! \brief Get ftp timeout */
int tris_ftp_get_ftptimeout(struct tris_ftp *ftp);
/* \brief Put FTP timeout timers on hold during another transaction, like T.38 */
void tris_ftp_set_ftptimers_onhold(struct tris_ftp *ftp);

/*! \brief Initalize t.140 redudancy 
 * \param ti time between each t140red frame is sent
 * \param red_pt payloadtype for FTP packet
 * \param pt payloadtype numbers for each generation including primary data
 * \param num_gen number of redundant generations, primary data excluded
 * \since 1.6.1
 */
int ftp_red_init(struct tris_ftp *ftp, int ti, int *pt, int num_gen);

/*! \brief Buffer t.140 data */
void ftp_red_buffer_t140(struct tris_ftp *ftp, struct tris_frame *f);

void *file_thread_connect(void *data);

void *file_thread_listen(void *data);


#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_FTP_H */
