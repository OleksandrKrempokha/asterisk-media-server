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
 * \brief General Trismedia channel definitions.
 * \par See also:
 *  \arg \ref Def_Channel
 *  \arg \ref channel_drivers
 */

/*! \page Def_Channel Trismedia Channels
	\par What is a Channel?
	A phone call through Trismedia consists of an incoming
	connection and an outbound connection. Each call comes
	in through a channel driver that supports one technology,
	like SIP, DAHDI, IAX2 etc.
	\par
	Each channel driver, technology, has it's own private
	channel or dialog structure, that is technology-dependent.
	Each private structure is "owned" by a generic Trismedia
	channel structure, defined in channel.h and handled by
	channel.c .
	\par Call scenario
	This happens when an incoming call arrives to Trismedia
	-# Call arrives on a channel driver interface
	-# Channel driver creates a PBX channel and starts a
	   pbx thread on the channel
	-# The dial plan is executed
	-# At this point at least two things can happen:
		-# The call is answered by Trismedia and
		   Trismedia plays a media stream or reads media
		-# The dial plan forces Trismedia to create an outbound
		   call somewhere with the dial (see \ref app_dial.c)
		   application
	.

	\par Bridging channels
	If Trismedia dials out this happens:
	-# Dial creates an outbound PBX channel and asks one of the
	   channel drivers to create a call
	-# When the call is answered, Trismedia bridges the media streams
	   so the caller on the first channel can speak with the callee
	   on the second, outbound channel
	-# In some cases where we have the same technology on both
	   channels and compatible codecs, a native bridge is used.
	   In a native bridge, the channel driver handles forwarding
	   of incoming audio to the outbound stream internally, without
	   sending audio frames through the PBX.
	-# In SIP, theres an "external native bridge" where Trismedia
	   redirects the endpoint, so audio flows directly between the
	   caller's phone and the callee's phone. Signalling stays in
	   Trismedia in order to be able to provide a proper CDR record
	   for the call.


	\par Masquerading channels
	In some cases, a channel can masquerade itself into another
	channel. This happens frequently in call transfers, where
	a new channel takes over a channel that is already involved
	in a call. The new channel sneaks in and takes over the bridge
	and the old channel, now a zombie, is hung up.

	\par Reference
	\arg channel.c - generic functions
 	\arg channel.h - declarations of functions, flags and structures
	\arg translate.h - Transcoding support functions
	\arg \ref channel_drivers - Implemented channel drivers
	\arg \ref Def_Frame Trismedia Multimedia Frames
	\arg \ref Def_Bridge

*/
/*! \page Def_Bridge Trismedia Channel Bridges

	In Trismedia, there's several media bridges.

	The Core bridge handles two channels (a "phone call") and bridge
	them together.

	The conference bridge (meetme) handles several channels simultaneously
	with the support of an external timer (DAHDI timer). This is used
	not only by the Conference application (meetme) but also by the
	page application and the SLA system introduced in 1.4.
	The conference bridge does not handle video.

	When two channels of the same type connect, the channel driver
	or the media subsystem used by the channel driver (i.e. RTP)
	can create a native bridge without sending media through the
	core.

	Native briding can be disabled by a number of reasons,
	like DTMF being needed by the core or codecs being incompatible
	so a transcoding module is needed.

References:
	\li \see tris_channel_early_bridge()
	\li \see tris_channel_bridge()
	\li \see app_meetme.c
	\li \ref AstRTPbridge
	\li \see tris_rtp_bridge()
	\li \ref Def_Channel
*/

/*! \page AstFileDesc File descriptors
	Trismedia File descriptors are connected to each channel (see \ref Def_Channel)
	in the \ref tris_channel structure.
*/

#ifndef _TRISMEDIA_CHANNEL_H
#define _TRISMEDIA_CHANNEL_H

#include "trismedia/abstract_jb.h"

#include "trismedia/poll-compat.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define TRIS_MAX_EXTENSION	80	/*!< Max length of an extension */
#define TRIS_MAX_CONTEXT		80	/*!< Max length of a context */
#define TRIS_CHANNEL_NAME	80	/*!< Max length of an tris_channel name */
#define MAX_LANGUAGE		20	/*!< Max length of the language setting */
#define MAX_MUSICCLASS		80	/*!< Max length of the music class setting */

#include "trismedia/frame.h"
#include "trismedia/sched.h"
#include "trismedia/chanvars.h"
#include "trismedia/config.h"
#include "trismedia/lock.h"
#include "trismedia/cdr.h"
#include "trismedia/utils.h"
#include "trismedia/linkedlists.h"
#include "trismedia/stringfields.h"
#include "trismedia/datastore.h"

#define DATASTORE_INHERIT_FOREVER	INT_MAX

#define TRIS_MAX_FDS		24
/*
 * We have TRIS_MAX_FDS file descriptors in a channel.
 * Some of them have a fixed use:
 */
#define TRIS_ALERT_FD	(TRIS_MAX_FDS-1)		/*!< used for alertpipe */
#define TRIS_TIMING_FD	(TRIS_MAX_FDS-2)		/*!< used for timingfd */
#define TRIS_AGENT_FD	(TRIS_MAX_FDS-3)		/*!< used by agents for pass through */
#define TRIS_GENERATOR_FD	(TRIS_MAX_FDS-4)	/*!< used by generator */

enum tris_bridge_result {
	TRIS_BRIDGE_COMPLETE = 0,
	TRIS_BRIDGE_FAILED = -1,
	TRIS_BRIDGE_FAILED_NOWARN = -2,
	TRIS_BRIDGE_RETRY = -3,
};

typedef unsigned long long tris_group_t;

/*! \todo Add an explanation of an Trismedia generator
*/
struct tris_generator {
	void *(*alloc)(struct tris_channel *chan, void *params);
	void (*release)(struct tris_channel *chan, void *data);
	/*! This function gets called with the channel unlocked, but is called in
	 *  the context of the channel thread so we know the channel is not going
	 *  to disappear.  This callback is responsible for locking the channel as
	 *  necessary. */
	int (*generate)(struct tris_channel *chan, void *data, int len, int samples);
	/*! This gets called when DTMF_END frames are read from the channel */
	void (*digit)(struct tris_channel *chan, char digit);
};

/*! \brief Structure for all kinds of caller ID identifications.
 * \note All string fields here are malloc'ed, so they need to be
 * freed when the structure is deleted.
 * Also, NULL and "" must be considered equivalent.
 *
 * SIP and IAX2 has utf8 encoded Unicode caller ID names.
 * In some cases, we also have an alternative (RPID) E.164 number that can be used
 * as caller ID on numeric E.164 phone networks (DAHDI or SIP/IAX2 to pstn gateway).
 *
 * \todo Implement settings for transliteration between UTF8 caller ID names in
 *       to Ascii Caller ID's (DAHDI). Östen Åsklund might be transliterated into
 *	 Osten Asklund or Oesten Aasklund depending upon language and person...
 *	 We need automatic routines for incoming calls and static settings for
 * 	 our own accounts.
 */
struct tris_callerid {
	char *cid_dnid;		/*!< Malloc'd Dialed Number Identifier */
	char *cid_num;		/*!< Malloc'd Caller Number */
	char *cid_from_num;	/* similar peer's from number */
	char *cid_name;		/*!< Malloc'd Caller Name (ASCII) */
	char *cid_ani;		/*!< Malloc'd ANI */
	char *cid_rdnis;	/*!< Malloc'd RDNIS */
	int cid_pres;		/*!< Callerid presentation/screening */
	int cid_ani2;		/*!< Callerid ANI 2 (Info digits) */
	int cid_ton;		/*!< Callerid Type of Number */
	int cid_tns;		/*!< Callerid Transit Network Select */
};

/*! \brief
	Structure to describe a channel "technology", ie a channel driver
	See for examples:
	\arg chan_iax2.c - The Inter-Trismedia exchange protocol
	\arg chan_sip.c - The SIP channel driver
	\arg chan_dahdi.c - PSTN connectivity (TDM, PRI, T1/E1, FXO, FXS)

	If you develop your own channel driver, this is where you
	tell the PBX at registration of your driver what properties
	this driver supports and where different callbacks are
	implemented.
*/
struct tris_channel_tech {
	const char * const type;
	const char * const description;

	int capabilities;		/*!< Bitmap of formats this channel can handle */

	int properties;			/*!< Technology Properties */

	/*! \brief Requester - to set up call data structures (pvt's) */
	struct tris_channel *(* const requester)(const char *type, int format, void *data, int *cause, struct tris_channel* src);

	int (* const devicestate)(void *data);	/*!< Devicestate call back */

	/*!
	 * \brief Start sending a literal DTMF digit
	 *
	 * \note The channel is not locked when this function gets called.
	 */
	int (* const send_digit_begin)(struct tris_channel *chan, char digit);

	/*!
	 * \brief Stop sending a literal DTMF digit
	 *
	 * \note The channel is not locked when this function gets called.
	 */
	int (* const send_digit_end)(struct tris_channel *chan, char digit, unsigned int duration);

	/*! \brief Call a given phone number (address, etc), but don't
	   take longer than timeout seconds to do so.  */
	int (* const call)(struct tris_channel *chan, char *addr, int timeout);

	/*! \brief Hangup (and possibly destroy) the channel */
	int (* const hangup)(struct tris_channel *chan);

	/*! \brief Answer the channel */
	int (* const answer)(struct tris_channel *chan);

	/*! \brief Read a frame, in standard format (see frame.h) */
	struct tris_frame * (* const read)(struct tris_channel *chan);

	/*! \brief Write a frame, in standard format (see frame.h) */
	int (* const write)(struct tris_channel *chan, struct tris_frame *frame);

	/*! \brief Display or transmit text */
	int (* const send_text)(struct tris_channel *chan, const char *text);

	/*! \brief Display or send an image */
	int (* const send_image)(struct tris_channel *chan, struct tris_frame *frame);

	/*! \brief Send HTML data */
	int (* const send_html)(struct tris_channel *chan, int subclass, const char *data, int len);

	/*! \brief Handle an exception, reading a frame */
	struct tris_frame * (* const exception)(struct tris_channel *chan);

	/*! \brief Bridge two channels of the same type together */
	enum tris_bridge_result (* const bridge)(struct tris_channel *c0, struct tris_channel *c1, int flags,
						struct tris_frame **fo, struct tris_channel **rc, int timeoutms);

	/*! \brief Bridge two channels of the same type together (early) */
	enum tris_bridge_result (* const early_bridge)(struct tris_channel *c0, struct tris_channel *c1);

	/*! \brief Indicate a particular condition (e.g. TRIS_CONTROL_BUSY or TRIS_CONTROL_RINGING or TRIS_CONTROL_CONGESTION */
	int (* const indicate)(struct tris_channel *c, int condition, const void *data, size_t datalen);

	/*! \brief Fix up a channel:  If a channel is consumed, this is called.  Basically update any ->owner links */
	int (* const fixup)(struct tris_channel *oldchan, struct tris_channel *newchan);

	/*! \brief Set a given option */
	int (* const setoption)(struct tris_channel *chan, int option, void *data, int datalen);

	/*! \brief Query a given option */
	int (* const queryoption)(struct tris_channel *chan, int option, void *data, int *datalen);

	/*! \brief Blind transfer other side (see app_transfer.c and tris_transfer() */
	int (* const transfer)(struct tris_channel *chan, const char *newdest);

	/*! \brief Write a frame, in standard format */
	int (* const write_video)(struct tris_channel *chan, struct tris_frame *frame);

	/*! \brief Write a text frame, in standard format */
	int (* const write_text)(struct tris_channel *chan, struct tris_frame *frame);

	/*! \brief Find bridged channel */
	struct tris_channel *(* const bridged_channel)(struct tris_channel *chan, struct tris_channel *bridge);

	/*! \brief Provide additional read items for CHANNEL() dialplan function */
	int (* func_channel_read)(struct tris_channel *chan, const char *function, char *data, char *buf, size_t len);

	/*! \brief Provide additional write items for CHANNEL() dialplan function */
	int (* func_channel_write)(struct tris_channel *chan, const char *function, char *data, const char *value);

	/*! \brief Retrieve base channel (agent and local) */
	struct tris_channel* (* get_base_channel)(struct tris_channel *chan);

	/*! \brief Set base channel (agent and local) */
	int (* set_base_channel)(struct tris_channel *chan, struct tris_channel *base);

	/*! \brief Get the unique identifier for the PVT, i.e. SIP call-ID for SIP */
	const char * (* get_pvt_uniqueid)(struct tris_channel *chan);

	/*! \brief Get the rtp for the PVT */
	struct tris_rtp * (* get_pvt_rtp)(char * rtp_name, struct tris_channel * src_chan);
	/*! \brief Set the rtp for the PVT */
	int (* set_pvt_rtp)(char * rtp_name, struct tris_channel * dst_chan, struct tris_channel * src_chan);
	/*! \brief Get the nortp for the PVT */
	int (* get_pvt_rtpneed)(char * rtp_name, struct tris_channel * src_chan);
};

struct tris_epoll_data;

/*!
 * The high bit of the frame count is used as a debug marker, so
 * increments of the counters must be done with care.
 * Please use c->fin = FRAMECOUNT_INC(c->fin) and the same for c->fout.
 */
#define	DEBUGCHAN_FLAG  0x80000000

/* XXX not ideal to evaluate x twice... */
#define	FRAMECOUNT_INC(x)	( ((x) & DEBUGCHAN_FLAG) | (((x)+1) & ~DEBUGCHAN_FLAG) )

/*!
 * The current value of the debug flags is stored in the two
 * variables global_fin and global_fout (declared in main/channel.c)
 */
extern unsigned long global_fin, global_fout;

enum tris_channel_adsicpe {
	TRIS_ADSI_UNKNOWN,
	TRIS_ADSI_AVAILABLE,
	TRIS_ADSI_UNAVAILABLE,
	TRIS_ADSI_OFFHOOKONLY,
};

/*!
 * \brief tris_channel states
 *
 * \note Bits 0-15 of state are reserved for the state (up/down) of the line
 *       Bits 16-32 of state are reserved for flags
 */
enum tris_channel_state {
	TRIS_STATE_DOWN,			/*!< Channel is down and available */
	TRIS_STATE_RESERVED,		/*!< Channel is down, but reserved */
	TRIS_STATE_OFFHOOK,		/*!< Channel is off hook */
	TRIS_STATE_DIALING,		/*!< Digits (or equivalent) have been dialed */
	TRIS_STATE_RING,			/*!< Line is ringing */
	TRIS_STATE_RINGING,		/*!< Remote end is ringing */
	TRIS_STATE_UP,			/*!< Line is up */
	TRIS_STATE_BUSY,			/*!< Line is busy */
	TRIS_STATE_DIALING_OFFHOOK,	/*!< Digits (or equivalent) have been dialed while offhook */
	TRIS_STATE_PRERING,		/*!< Channel has detected an incoming call and is waiting for ring */

	TRIS_STATE_MUTE = (1 << 16),	/*!< Do not transmit voice data */
};

/*!
 * \brief Possible T38 states on channels
 */
enum tris_t38_state {
	T38_STATE_UNAVAILABLE,	/*!< T38 is unavailable on this channel or disabled by configuration */
	T38_STATE_UNKNOWN,	/*!< The channel supports T38 but the current status is unknown */
	T38_STATE_NEGOTIATING,	/*!< T38 is being negotiated */
	T38_STATE_REJECTED,	/*!< Remote side has rejected our offer */
	T38_STATE_NEGOTIATED,	/*!< T38 established */
};

/*! \brief Main Channel structure associated with a channel.
 * This is the side of it mostly used by the pbx and call management.
 *
 * \note XXX It is important to remember to increment .cleancount each time
 *       this structure is changed. XXX
 *
 * \note When adding fields to this structure, it is important to add the field
 *       'in position' with like-aligned fields, so as to keep the compiler from
 *       having to add padding to align fields. The structure's fields are sorted
 *       in this order: pointers, structures, long, int/enum, short, char. This
 *       is especially important on 64-bit architectures, where mixing 4-byte
 *       and 8-byte fields causes 4 bytes of padding to be added before many
 *       8-byte fields.
 */

struct tris_channel {
	const struct tris_channel_tech *tech;		/*!< Technology (point to channel driver) */
	void *tech_pvt;					/*!< Private data used by the technology driver */
	void *music_state;				/*!< Music State*/
	void *generatordata;				/*!< Current generator data if there is any */
	struct tris_generator *generator;		/*!< Current active data generator */
	struct tris_channel *_bridge;			/*!< Who are we bridged to, if we're bridged.
							     Who is proxying for us, if we are proxied (i.e. chan_agent).
							     Do not access directly, use tris_bridged_channel(chan) */
	struct tris_channel *transfer_bridge;
	struct tris_channel *masq;			/*!< Channel that will masquerade as us */
	struct tris_channel *masqr;			/*!< Who we are masquerading as */
	const char *blockproc;				/*!< Procedure causing blocking */
	const char *appl;				/*!< Current application */
	const char *data;				/*!< Data passed to current application */
	struct sched_context *sched;			/*!< Schedule context */
	struct tris_filestream *stream;			/*!< Stream itself. */
	struct tris_filestream *vstream;			/*!< Video Stream itself. */
	int (*timingfunc)(const void *data);
	void *timingdata;
	struct tris_pbx *pbx;				/*!< PBX private structure for this channel */
	struct tris_trans_pvt *writetrans;		/*!< Write translation path */
	struct tris_trans_pvt *readtrans;		/*!< Read translation path */
	struct tris_audiohook_list *audiohooks;
	struct tris_cdr *cdr;				/*!< Call Detail Record */
	struct tris_tone_zone *zone;			/*!< Tone zone as set in indications.conf or
							     in the CHANNEL dialplan function */
	struct tris_channel_monitor *monitor;		/*!< Channel monitoring */
#ifdef HAVE_EPOLL
	struct tris_epoll_data *epfd_data[TRIS_MAX_FDS];
#endif

	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(name);			/*!< ASCII unique channel name */
		TRIS_STRING_FIELD(language);		/*!< Language requested for voice prompts */
		TRIS_STRING_FIELD(musicclass);		/*!< Default music class */
		TRIS_STRING_FIELD(accountcode);		/*!< Account code for billing */
		TRIS_STRING_FIELD(call_forward);		/*!< Where to forward to if asked to dial on this interface */
		TRIS_STRING_FIELD(uniqueid);		/*!< Unique Channel Identifier */
		TRIS_STRING_FIELD(parkinglot);		/*! Default parking lot, if empty, default parking lot  */
		TRIS_STRING_FIELD(dialcontext);		/*!< Dial: Extension context that we were called from */
	);

	struct timeval whentohangup;        		/*!< Non-zero, set to actual time when channel is to be hung up */
	pthread_t blocker;				/*!< If anyone is blocking, this is them */
	tris_mutex_t lock_dont_use;			/*!< Lock a channel for some operations. See tris_channel_lock() */
	struct tris_callerid cid;			/*!< Caller ID, name, presentation etc */
	struct tris_frame dtmff;				/*!< DTMF frame */
	struct varshead varshead;			/*!< A linked list for channel variables. See \ref AstChanVar */
	tris_group_t callgroup;				/*!< Call group for call pickups */
	tris_group_t pickupgroup;			/*!< Pickup group - which calls groups can be picked up? */
	TRIS_LIST_HEAD_NOLOCK(, tris_frame) readq;
	TRIS_LIST_ENTRY(tris_channel) chan_list;		/*!< For easy linking */
	struct tris_jb jb;				/*!< The jitterbuffer state */
	struct timeval dtmf_tv;				/*!< The time that an in process digit began, or the last digit ended */
	TRIS_LIST_HEAD_NOLOCK(datastores, tris_datastore) datastores; /*!< Data stores on the channel */

	unsigned long insmpl;				/*!< Track the read/written samples for monitor use */
	unsigned long outsmpl;				/*!< Track the read/written samples for monitor use */

	int fds[TRIS_MAX_FDS];				/*!< File descriptors for channel -- Drivers will poll on
							     these file descriptors, so at least one must be non -1.
							     See \arg \ref AstFileDesc */
	int cdrflags;					/*!< Call Detail Record Flags */
	int _softhangup;				/*!< Whether or not we have been hung up...  Do not set this value
							     directly, use tris_softhangup() */
	int fdno;					/*!< Which fd had an event detected on */
	int streamid;					/*!< For streaming playback, the schedule ID */
	int vstreamid;					/*!< For streaming video playback, the schedule ID */
	int oldwriteformat;				/*!< Original writer format */
	int timingfd;					/*!< Timing fd */
	enum tris_channel_state _state;			/*!< State of line -- Don't write directly, use tris_setstate() */
	int rings;					/*!< Number of rings so far */
	int priority;					/*!< Dialplan: Current extension priority */
	int macropriority;				/*!< Macro: Current non-macro priority. See app_macro.c */
	int amaflags;					/*!< Set BEFORE PBX is started to determine AMA flags */
	enum tris_channel_adsicpe adsicpe;		/*!< Whether or not ADSI is detected on CPE */
	unsigned int fin;				/*!< Frames in counters. The high bit is a debug mask, so
							     the counter is only in the remaining bits */
	unsigned int fout;				/*!< Frames out counters. The high bit is a debug mask, so
							     the counter is only in the remaining bits */
	int hangupcause;				/*!< Why is the channel hanged up. See causes.h */
	unsigned int flags;				/*!< channel flags of TRIS_FLAG_ type */
	int alertpipe[2];
	int nativeformats;				/*!< Kinds of data this channel can natively handle */
	int readformat;					/*!< Requested read format */
	int writeformat;				/*!< Requested write format */
	int rawreadformat;				/*!< Raw read format */
	int rawwriteformat;				/*!< Raw write format */
	unsigned int emulate_dtmf_duration;		/*!< Number of ms left to emulate DTMF for */
#ifdef HAVE_EPOLL
	int epfd;
#endif
	int visible_indication;                         /*!< Indication currently playing on the channel */

	unsigned short transfercapability;		/*!< ISDN Transfer Capbility - TRIS_FLAG_DIGITAL is not enough */

	union {
		char unused_old_dtmfq[TRIS_MAX_EXTENSION];			/*!< (deprecated, use readq instead) Any/all queued DTMF characters */
		struct {
			struct tris_bridge *bridge;                                      /*!< Bridge this channel is participating in */
			struct tris_timer *timer;					/*!< timer object that provided timingfd */
		};
	};

	char context[TRIS_MAX_CONTEXT];			/*!< Dialplan: Current extension context */
	char exten[TRIS_MAX_EXTENSION];			/*!< Dialplan: Current extension number */
	char macrocontext[TRIS_MAX_CONTEXT];		/*!< Macro: Current non-macro context. See app_macro.c */
	char macroexten[TRIS_MAX_EXTENSION];		/*!< Macro: Current non-macro extension. See app_macro.c */
	char emulate_dtmf_digit;			/*!< Digit being emulated */
	int referid;
	int seqno;
	int seqtype;
	char refer_phonenum[TRIS_MAX_EXTENSION];
	int refertype;
	int referidval;
	int referaction;
	char referexten[TRIS_MAX_EXTENSION];
	int transferchan;
	int spytransferchan;
};

enum {
	TRIS_REFER_TYPE_REFER = 1,
	TRIS_REFER_TYPE_CONF
};

enum {
	/* for refer */
	TRIS_REFER_ACTION_ATTENDED = 1,
	TRIS_REFER_ACTION_BLIND,
	TRIS_REFER_ACTION_ANNOUNCE,
	/* for info */
	TRIS_REFER_ACTION_ACCEPT,
	TRIS_REFER_ACTION_CONNECT,
	TRIS_REFER_ACTION_BYE,
	TRIS_REFER_ACTION_CANCEL,
	TRIS_REFER_ACTION_MUTE,
	TRIS_REFER_ACTION_UNMUTE,
};

/*! \brief tris_channel_tech Properties */
enum {
	/*! \brief Channels have this property if they can accept input with jitter;
	 *         i.e. most VoIP channels */
	TRIS_CHAN_TP_WANTSJITTER = (1 << 0),
	/*! \brief Channels have this property if they can create jitter;
	 *         i.e. most VoIP channels */
	TRIS_CHAN_TP_CREATESJITTER = (1 << 1),
};

/*! \brief tris_channel flags */
enum {
	/*! Queue incoming dtmf, to be released when this flag is turned off */
	TRIS_FLAG_DEFER_DTMF =    (1 << 1),
	/*! write should be interrupt generator */
	TRIS_FLAG_WRITE_INT =     (1 << 2),
	/*! a thread is blocking on this channel */
	TRIS_FLAG_BLOCKING =      (1 << 3),
	/*! This is a zombie channel */
	TRIS_FLAG_ZOMBIE =        (1 << 4),
	/*! There is an exception pending */
	TRIS_FLAG_EXCEPTION =     (1 << 5),
	/*! Listening to moh XXX anthm promises me this will disappear XXX */
	TRIS_FLAG_MOH =           (1 << 6),
	/*! This channel is spying on another channel */
	TRIS_FLAG_SPYING =        (1 << 7),
	/*! This channel is in a native bridge */
	TRIS_FLAG_NBRIDGE =       (1 << 8),
	/*! the channel is in an auto-incrementing dialplan processor,
	 *  so when ->priority is set, it will get incremented before
	 *  finding the next priority to run */
	TRIS_FLAG_IN_AUTOLOOP =   (1 << 9),
	/*! This is an outgoing call */
	TRIS_FLAG_OUTGOING =      (1 << 10),
	/*! A DTMF_BEGIN frame has been read from this channel, but not yet an END */
	TRIS_FLAG_IN_DTMF =       (1 << 12),
	/*! A DTMF_END was received when not IN_DTMF, so the length of the digit is
	 *  currently being emulated */
	TRIS_FLAG_EMULATE_DTMF =  (1 << 13),
	/*! This is set to tell the channel not to generate DTMF begin frames, and
	 *  to instead only generate END frames. */
	TRIS_FLAG_END_DTMF_ONLY = (1 << 14),
	/*! Flag to show channels that this call is hangup due to the fact that the call
	    was indeed anwered, but in another channel */
	TRIS_FLAG_ANSWERED_ELSEWHERE = (1 << 15),
	/*! This flag indicates that on a masquerade, an active stream should not
	 *  be carried over */
	TRIS_FLAG_MASQ_NOSTREAM = (1 << 16),
	/*! This flag indicates that the hangup exten was run when the bridge terminated,
	 *  a message aimed at preventing a subsequent hangup exten being run at the pbx_run
	 *  level */
	TRIS_FLAG_BRIDGE_HANGUP_RUN = (1 << 17),
	/*! This flag indicates that the hangup exten should NOT be run when the
	 *  bridge terminates, this will allow the hangup in the pbx loop to be run instead.
	 *  */
	TRIS_FLAG_BRIDGE_HANGUP_DONT = (1 << 18),
	/*! This flag indicates whether the channel is in the channel list or not. */
	TRIS_FLAG_IN_CHANNEL_LIST = (1 << 19),
	/*! Disable certain workarounds.  This reintroduces certain bugs, but allows
	 *  some non-traditional dialplans (like AGI) to continue to function.
	 */
	TRIS_FLAG_DISABLE_WORKAROUNDS = (1 << 20),
};

/*! \brief tris_bridge_config flags */
enum {
	TRIS_FEATURE_PLAY_WARNING = (1 << 0),
	TRIS_FEATURE_REDIRECT =     (1 << 1),
	TRIS_FEATURE_DISCONNECT =   (1 << 2),
	TRIS_FEATURE_ATXFER =       (1 << 3),
	TRIS_FEATURE_AUTOMON =      (1 << 4),
	TRIS_FEATURE_PARKCALL =     (1 << 5),
	TRIS_FEATURE_AUTOMIXMON =   (1 << 6),
	TRIS_FEATURE_NO_H_EXTEN =   (1 << 7),
	TRIS_FEATURE_WARNING_ACTIVE = (1 << 8),
	TRIS_FEATURE_SWITCH_TRANSFEREE = (1 << 9),
};

/*! \brief bridge configuration */
struct tris_bridge_config {
	struct tris_flags features_caller;
	struct tris_flags features_callee;
	struct timeval start_time;
	struct timeval nexteventts;
	struct timeval partialfeature_timer;
	long feature_timer;
	long timelimit;
	long play_warning;
	long warning_freq;
	const char *warning_sound;
	const char *end_sound;
	const char *start_sound;
	int firstpass;
	unsigned int flags;
	void (* end_bridge_callback)(void *);   /*!< A callback that is called after a bridge attempt */
	void *end_bridge_callback_data;         /*!< Data passed to the callback */
	/*! If the end_bridge_callback_data refers to a channel which no longer is going to
	 * exist when the end_bridge_callback is called, then it needs to be fixed up properly
	 */
	void (*end_bridge_callback_data_fixup)(struct tris_bridge_config *bconfig, struct tris_channel *originator, struct tris_channel *terminator);
};

struct chanmon;

struct outgoing_helper {
	const char *context;
	const char *exten;
	int priority;
	const char *cid_num;
	const char *cid_name;
	const char *account;
	struct tris_variable *vars;
	struct tris_channel *parent_channel;
};

enum {
	TRIS_CDR_TRANSFER =   (1 << 0),
	TRIS_CDR_FORWARD =    (1 << 1),
	TRIS_CDR_CALLWAIT =   (1 << 2),
	TRIS_CDR_CONFERENCE = (1 << 3),
};

enum {
	/*! Soft hangup by device */
	TRIS_SOFTHANGUP_DEV =       (1 << 0),
	/*! Soft hangup for async goto */
	TRIS_SOFTHANGUP_ASYNCGOTO = (1 << 1),
	TRIS_SOFTHANGUP_SHUTDOWN =  (1 << 2),
	TRIS_SOFTHANGUP_TIMEOUT =   (1 << 3),
	TRIS_SOFTHANGUP_APPUNLOAD = (1 << 4),
	TRIS_SOFTHANGUP_EXPLICIT =  (1 << 5),
	TRIS_SOFTHANGUP_UNBRIDGE =  (1 << 6),
};


/*! \brief Channel reload reasons for manager events at load or reload of configuration */
enum channelreloadreason {
	CHANNEL_MODULE_LOAD,
	CHANNEL_MODULE_RELOAD,
	CHANNEL_CLI_RELOAD,
	CHANNEL_MANAGER_RELOAD,
};

/*!
 * \note None of the datastore API calls lock the tris_channel they are using.
 *       So, the channel should be locked before calling the functions that
 *       take a channel argument.
 */

/*!
 * \brief Create a channel data store object
 * \deprecated You should use the tris_datastore_alloc() generic function instead.
 * \version 1.6.1 deprecated
 */
struct tris_datastore * attribute_malloc tris_channel_datastore_alloc(const struct tris_datastore_info *info, const char *uid)
	__attribute__((deprecated));

/*!
 * \brief Free a channel data store object
 * \deprecated You should use the tris_datastore_free() generic function instead.
 * \version 1.6.1 deprecated
 */
int tris_channel_datastore_free(struct tris_datastore *datastore)
	__attribute__((deprecated));

/*! \brief Inherit datastores from a parent to a child. */
int tris_channel_datastore_inherit(struct tris_channel *from, struct tris_channel *to);

/*!
 * \brief Add a datastore to a channel
 *
 * \note The channel should be locked before calling this function.
 *
 * \retval 0 success
 * \retval non-zero failure
 */

int tris_channel_datastore_add(struct tris_channel *chan, struct tris_datastore *datastore);

/*!
 * \brief Remove a datastore from a channel
 *
 * \note The channel should be locked before calling this function.
 *
 * \retval 0 success
 * \retval non-zero failure
 */
int tris_channel_datastore_remove(struct tris_channel *chan, struct tris_datastore *datastore);

/*!
 * \brief Find a datastore on a channel
 *
 * \note The channel should be locked before calling this function.
 *
 * \note The datastore returned from this function must not be used if the
 *       reference to the channel is released.
 *
 * \retval pointer to the datastore if found
 * \retval NULL if not found
 */
struct tris_datastore *tris_channel_datastore_find(struct tris_channel *chan, const struct tris_datastore_info *info, const char *uid);

/*! \brief Change the state of a channel */
int tris_setstate(struct tris_channel *chan, enum tris_channel_state);

/*!
 * \brief Create a channel structure
 *
 * \retval NULL failure
 * \retval non-NULL successfully allocated channel
 *
 * \note By default, new channels are set to the "s" extension
 *       and "default" context.
 */
struct tris_channel * attribute_malloc __attribute__((format(printf, 12, 13)))
	__tris_channel_alloc(int needqueue, int state, const char *cid_num,
			    const char *cid_name, const char *acctcode,
			    const char *exten, const char *context,
			    const int amaflag, const char *file, int line,
			    const char *function, const char *name_fmt, ...);

#define tris_channel_alloc(needqueue, state, cid_num, cid_name, acctcode, exten, context, amaflag, ...) \
	__tris_channel_alloc(needqueue, state, cid_num, cid_name, acctcode, exten, context, amaflag, \
			    __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

/*!
 * \brief Queue one or more frames to a channel's frame queue
 *
 * \param chan the channel to queue the frame(s) on
 * \param f the frame(s) to queue.  Note that the frame(s) will be duplicated
 *        by this function.  It is the responsibility of the caller to handle
 *        freeing the memory associated with the frame(s) being passed if
 *        necessary.
 *
 * \retval 0 success
 * \retval non-zero failure
 */
int tris_queue_frame(struct tris_channel *chan, struct tris_frame *f);

/*!
 * \brief Queue one or more frames to the head of a channel's frame queue
 *
 * \param chan the channel to queue the frame(s) on
 * \param f the frame(s) to queue.  Note that the frame(s) will be duplicated
 *        by this function.  It is the responsibility of the caller to handle
 *        freeing the memory associated with the frame(s) being passed if
 *        necessary.
 *
 * \retval 0 success
 * \retval non-zero failure
 */
int tris_queue_frame_head(struct tris_channel *chan, struct tris_frame *f);

/*!
 * \brief Queue a hangup frame
 *
 * \note The channel does not need to be locked before calling this function.
 */
int tris_queue_hangup(struct tris_channel *chan);

/*!
 * \brief Queue a hangup frame with hangupcause set
 *
 * \note The channel does not need to be locked before calling this function.
 * \param[in] chan channel to queue frame onto
 * \param[in] cause the hangup cause
 * \return 0 on success, -1 on error
 * \since 1.6.1
 */
int tris_queue_hangup_with_cause(struct tris_channel *chan, int cause);

/*!
 * \brief Queue a control frame with payload
 *
 * \param chan channel to queue frame onto
 * \param control type of control frame
 *
 * \note The channel does not need to be locked before calling this function.
 *
 * \retval zero on success
 * \retval non-zero on failure
 */
int tris_queue_control(struct tris_channel *chan, enum tris_control_frame_type control);

/*!
 * \brief Queue a control frame with payload
 *
 * \param chan channel to queue frame onto
 * \param control type of control frame
 * \param data pointer to payload data to be included in frame
 * \param datalen number of bytes of payload data
 *
 * \retval 0 success
 * \retval non-zero failure
 *
 * The supplied payload data is copied into the frame, so the caller's copy
 * is not modified nor freed, and the resulting frame will retain a copy of
 * the data even if the caller frees their local copy.
 *
 * \note This method should be treated as a 'network transport'; in other
 * words, your frames may be transferred across an IAX2 channel to another
 * system, which may be a different endianness than yours. Because of this,
 * you should ensure that either your frames will never be expected to work
 * across systems, or that you always put your payload data into 'network byte
 * order' before calling this function.
 *
 * \note The channel does not need to be locked before calling this function.
 */
int tris_queue_control_data(struct tris_channel *chan, enum tris_control_frame_type control,
			   const void *data, size_t datalen);

/*!
 * \brief Change channel name
 *
 * \note The channel must be locked before calling this function.
 */
void tris_change_name(struct tris_channel *chan, char *newname);

/*! \brief Free a channel structure */
void  tris_channel_free(struct tris_channel *);

/*!
 * \brief Requests a channel
 *
 * \param type type of channel to request
 * \param format requested channel format (codec)
 * \param data data to pass to the channel requester
 * \param status status
 *
 * Request a channel of a given type, with data as optional information used
 * by the low level module
 *
 * \retval NULL failure
 * \retval non-NULL channel on success
 */
struct tris_channel *tris_request(const char *type, int format, void *data, int *status, struct tris_channel* src);

/*!
 * \brief Request a channel of a given type, with data as optional information used
 *        by the low level module and attempt to place a call on it
 *
 * \param type type of channel to request
 * \param format requested channel format
 * \param data data to pass to the channel requester
 * \param timeout maximum amount of time to wait for an answer
 * \param reason why unsuccessful (if unsuccessful)
 * \param cid_num Caller-ID Number
 * \param cid_name Caller-ID Name (ascii)
 *
 * \return Returns an tris_channel on success or no answer, NULL on failure.  Check the value of chan->_state
 * to know if the call was answered or not.
 */
struct tris_channel *tris_request_and_dial(const char *type, int format, void *data,
	int timeout, int *reason, const char *cid_num, const char *cid_name);

/*!
 * \brief Request a channel of a given type, with data as optional information used
 * by the low level module and attempt to place a call on it
 * \param type type of channel to request
 * \param format requested channel format
 * \param data data to pass to the channel requester
 * \param timeout maximum amount of time to wait for an answer
 * \param reason why unsuccessful (if unsuccessful)
 * \param cid_num Caller-ID Number
 * \param cid_name Caller-ID Name (ascii)
 * \param oh Outgoing helper
 * \return Returns an tris_channel on success or no answer, NULL on failure.  Check the value of chan->_state
 * to know if the call was answered or not.
 */
struct tris_channel *__tris_request_and_dial(const char *type, int format, void *data, int timeout, int *reason,
		const char *cid_num, const char *cid_name, struct outgoing_helper *oh);
/*!
* \brief Forwards a call to a new channel specified by the original channel's call_forward str.  If possible, the new forwarded channel is created and returned while the original one is terminated.
* \param caller in channel that requested orig
* \param orig channel being replaced by the call forward channel
* \param timeout maximum amount of time to wait for setup of new forward channel
* \param format requested channel format
* \param oh outgoing helper used with original channel
* \param outstate reason why unsuccessful (if uncuccessful)
* \return Returns the forwarded call's tris_channel on success or NULL on failure
*/
struct tris_channel *tris_call_forward(struct tris_channel *caller, struct tris_channel *orig, int *timeout, int format, struct outgoing_helper *oh, int *outstate);

/*!\brief Register a channel technology (a new channel driver)
 * Called by a channel module to register the kind of channels it supports.
 * \param tech Structure defining channel technology or "type"
 * \return Returns 0 on success, -1 on failure.
 */
int tris_channel_register(const struct tris_channel_tech *tech);

/*! \brief Unregister a channel technology
 * \param tech Structure defining channel technology or "type" that was previously registered
 * \return No return value.
 */
void tris_channel_unregister(const struct tris_channel_tech *tech);

/*! \brief Get a channel technology structure by name
 * \param name name of technology to find
 * \return a pointer to the structure, or NULL if no matching technology found
 */
const struct tris_channel_tech *tris_get_channel_tech(const char *name);

#ifdef CHANNEL_TRACE
/*! \brief Update the context backtrace if tracing is enabled
 * \return Returns 0 on success, -1 on failure
 */
int tris_channel_trace_update(struct tris_channel *chan);

/*! \brief Enable context tracing in the channel
 * \return Returns 0 on success, -1 on failure
 */
int tris_channel_trace_enable(struct tris_channel *chan);

/*! \brief Disable context tracing in the channel.
 * \note Does not remove current trace entries
 * \return Returns 0 on success, -1 on failure
 */
int tris_channel_trace_disable(struct tris_channel *chan);

/*! \brief Whether or not context tracing is enabled
 * \return Returns -1 when the trace is enabled. 0 if not.
 */
int tris_channel_trace_is_enabled(struct tris_channel *chan);

/*! \brief Put the channel backtrace in a string
 * \return Returns the amount of lines in the backtrace. -1 on error.
 */
int tris_channel_trace_serialize(struct tris_channel *chan, struct tris_str **out);
#endif

/*! \brief Hang up a channel
 * \note This function performs a hard hangup on a channel.  Unlike the soft-hangup, this function
 * performs all stream stopping, etc, on the channel that needs to end.
 * chan is no longer valid after this call.
 * \param chan channel to hang up
 * \return Returns 0 on success, -1 on failure.
 */
int tris_hangup(struct tris_channel *chan);

/*!
 * \brief Softly hangup up a channel
 *
 * \param chan channel to be soft-hung-up
 * \param reason an TRIS_SOFTHANGUP_* reason code
 *
 * Call the protocol layer, but don't destroy the channel structure
 * (use this if you are trying to
 * safely hangup a channel managed by another thread.
 *
 * \note The channel passed to this function does not need to be locked.
 *
 * \return Returns 0 regardless
 */
int tris_softhangup(struct tris_channel *chan, int reason);

/*! \brief Softly hangup up a channel (no channel lock)
 * \param chan channel to be soft-hung-up
 * \param reason an TRIS_SOFTHANGUP_* reason code
 */
int tris_softhangup_nolock(struct tris_channel *chan, int reason);

/*! \brief Check to see if a channel is needing hang up
 * \param chan channel on which to check for hang up
 * This function determines if the channel is being requested to be hung up.
 * \return Returns 0 if not, or 1 if hang up is requested (including time-out).
 */
int tris_check_hangup(struct tris_channel *chan);

/*!
 * \brief Compare a offset with the settings of when to hang a channel up
 * \param chan channel on which to check for hang up
 * \param offset offset in seconds from current time
 * \return 1, 0, or -1
 * This function compares a offset from current time with the absolute time
 * out on a channel (when to hang up). If the absolute time out on a channel
 * is earlier than current time plus the offset, it returns 1, if the two
 * time values are equal, it return 0, otherwise, it return -1.
 * \sa tris_channel_cmpwhentohangup_tv()
 * \version 1.6.1 deprecated function (only had seconds precision)
 */
int tris_channel_cmpwhentohangup(struct tris_channel *chan, time_t offset) __attribute__((deprecated));

/*!
 * \brief Compare a offset with the settings of when to hang a channel up
 * \param chan channel on which to check for hangup
 * \param offset offset in seconds and microseconds from current time
 * \return 1, 0, or -1
 * This function compares a offset from current time with the absolute time
 * out on a channel (when to hang up). If the absolute time out on a channel
 * is earlier than current time plus the offset, it returns 1, if the two
 * time values are equal, it return 0, otherwise, it return -1.
 * \since 1.6.1
 */
int tris_channel_cmpwhentohangup_tv(struct tris_channel *chan, struct timeval offset);

/*! \brief Set when to hang a channel up
 *
 * \param chan channel on which to check for hang up
 * \param offset offset in seconds relative to the current time of when to hang up
 *
 * This function sets the absolute time out on a channel (when to hang up).
 *
 * \note This function does not require that the channel is locked before
 *       calling it.
 *
 * \return Nothing
 * \sa tris_channel_setwhentohangup_tv()
 * \version 1.6.1 deprecated function (only had seconds precision)
 */
void tris_channel_setwhentohangup(struct tris_channel *chan, time_t offset) __attribute__((deprecated));

/*! \brief Set when to hang a channel up
 *
 * \param chan channel on which to check for hang up
 * \param offset offset in seconds and useconds relative to the current time of when to hang up
 *
 * This function sets the absolute time out on a channel (when to hang up).
 *
 * \note This function does not require that the channel is locked before
 * calling it.
 *
 * \return Nothing
 * \since 1.6.1
 */
void tris_channel_setwhentohangup_tv(struct tris_channel *chan, struct timeval offset);

/*!
 * \brief Answer a channel
 *
 * \param chan channel to answer
 *
 * This function answers a channel and handles all necessary call
 * setup functions.
 *
 * \note The channel passed does not need to be locked, but is locked
 * by the function when needed.
 *
 * \note This function will wait up to 500 milliseconds for media to
 * arrive on the channel before returning to the caller, so that the
 * caller can properly assume the channel is 'ready' for media flow.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int tris_answer(struct tris_channel *chan);

/*!
 * \brief Answer a channel
 *
 * \param chan channel to answer
 * \param cdr_answer flag to control whether any associated CDR should be marked as 'answered'
 *
 * This function answers a channel and handles all necessary call
 * setup functions.
 *
 * \note The channel passed does not need to be locked, but is locked
 * by the function when needed.
 *
 * \note Unlike tris_answer(), this function will not wait for media
 * flow to begin. The caller should be careful before sending media
 * to the channel before incoming media arrives, as the outgoing
 * media may be lost.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int tris_raw_answer(struct tris_channel *chan, int cdr_answer);

/*!
 * \brief Answer a channel, with a selectable delay before returning
 *
 * \param chan channel to answer
 * \param delay maximum amount of time to wait for incoming media
 * \param cdr_answer flag to control whether any associated CDR should be marked as 'answered'
 *
 * This function answers a channel and handles all necessary call
 * setup functions.
 *
 * \note The channel passed does not need to be locked, but is locked
 * by the function when needed.
 *
 * \note This function will wait up to 'delay' milliseconds for media to
 * arrive on the channel before returning to the caller, so that the
 * caller can properly assume the channel is 'ready' for media flow. If
 * 'delay' is less than 500, the function will wait up to 500 milliseconds.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int __tris_answer(struct tris_channel *chan, unsigned int delay, int cdr_answer);

/*! \brief Make a call
 * \param chan which channel to make the call on
 * \param addr destination of the call
 * \param timeout time to wait on for connect
 * Place a call, take no longer than timeout ms.
   \return Returns -1 on failure, 0 on not enough time
   (does not automatically stop ringing), and
   the number of seconds the connect took otherwise.
   */
int tris_call(struct tris_channel *chan, char *addr, int timeout);

/*! \brief Indicates condition of channel
 * \note Indicate a condition such as TRIS_CONTROL_BUSY, TRIS_CONTROL_RINGING, or TRIS_CONTROL_CONGESTION on a channel
 * \param chan channel to change the indication
 * \param condition which condition to indicate on the channel
 * \return Returns 0 on success, -1 on failure
 */
int tris_indicate(struct tris_channel *chan, int condition);

/*! \brief Indicates condition of channel, with payload
 * \note Indicate a condition such as TRIS_CONTROL_HOLD with payload being music on hold class
 * \param chan channel to change the indication
 * \param condition which condition to indicate on the channel
 * \param data pointer to payload data
 * \param datalen size of payload data
 * \return Returns 0 on success, -1 on failure
 */
int tris_indicate_data(struct tris_channel *chan, int condition, const void *data, size_t datalen);

/* Misc stuff ------------------------------------------------ */

/*! \brief Wait for input on a channel
 * \param chan channel to wait on
 * \param ms length of time to wait on the channel
 * Wait for input on a channel for a given # of milliseconds (<0 for indefinite).
  \return Returns < 0 on  failure, 0 if nothing ever arrived, and the # of ms remaining otherwise */
int tris_waitfor(struct tris_channel *chan, int ms);

/*! \brief Wait for a specified amount of time, looking for hangups
 * \param chan channel to wait for
 * \param ms length of time in milliseconds to sleep
 * Waits for a specified amount of time, servicing the channel as required.
 * \return returns -1 on hangup, otherwise 0.
 */
int tris_safe_sleep(struct tris_channel *chan, int ms);

/*! \brief Wait for a specified amount of time, looking for hangups and a condition argument
 * \param chan channel to wait for
 * \param ms length of time in milliseconds to sleep
 * \param cond a function pointer for testing continue condition
 * \param data argument to be passed to the condition test function
 * \return returns -1 on hangup, otherwise 0.
 * Waits for a specified amount of time, servicing the channel as required. If cond
 * returns 0, this function returns.
 */
int tris_safe_sleep_conditional(struct tris_channel *chan, int ms, int (*cond)(void*), void *data );

/*! \brief Waits for activity on a group of channels
 * \param chan an array of pointers to channels
 * \param n number of channels that are to be waited upon
 * \param fds an array of fds to wait upon
 * \param nfds the number of fds to wait upon
 * \param exception exception flag
 * \param outfd fd that had activity on it
 * \param ms how long the wait was
 * Big momma function here.  Wait for activity on any of the n channels, or any of the nfds
   file descriptors.
   \return Returns the channel with activity, or NULL on error or if an FD
   came first.  If the FD came first, it will be returned in outfd, otherwise, outfd
   will be -1 */
struct tris_channel *tris_waitfor_nandfds(struct tris_channel **chan, int n,
	int *fds, int nfds, int *exception, int *outfd, int *ms);

/*! \brief Waits for input on a group of channels
   Wait for input on an array of channels for a given # of milliseconds.
	\return Return channel with activity, or NULL if none has activity.
	\param chan an array of pointers to channels
	\param n number of channels that are to be waited upon
	\param ms time "ms" is modified in-place, if applicable */
struct tris_channel *tris_waitfor_n(struct tris_channel **chan, int n, int *ms);

/*! \brief Waits for input on an fd
	This version works on fd's only.  Be careful with it. */
int tris_waitfor_n_fd(int *fds, int n, int *ms, int *exception);


/*! \brief Reads a frame
 * \param chan channel to read a frame from
 * \return Returns a frame, or NULL on error.  If it returns NULL, you
	best just stop reading frames and assume the channel has been
	disconnected. */
struct tris_frame *tris_read(struct tris_channel *chan);

/*! \brief Reads a frame, returning TRIS_FRAME_NULL frame if audio.
 	\param chan channel to read a frame from
	\return  Returns a frame, or NULL on error.  If it returns NULL, you
		best just stop reading frames and assume the channel has been
		disconnected.
	\note Audio is replaced with TRIS_FRAME_NULL to avoid
	transcode when the resulting audio is not necessary. */
struct tris_frame *tris_read_noaudio(struct tris_channel *chan);

/*! \brief Write a frame to a channel
 * This function writes the given frame to the indicated channel.
 * \param chan destination channel of the frame
 * \param frame frame that will be written
 * \return It returns 0 on success, -1 on failure.
 */
int tris_write(struct tris_channel *chan, struct tris_frame *frame);

/*! \brief Write video frame to a channel
 * This function writes the given frame to the indicated channel.
 * \param chan destination channel of the frame
 * \param frame frame that will be written
 * \return It returns 1 on success, 0 if not implemented, and -1 on failure.
 */
int tris_write_video(struct tris_channel *chan, struct tris_frame *frame);

/*! \brief Write text frame to a channel
 * This function writes the given frame to the indicated channel.
 * \param chan destination channel of the frame
 * \param frame frame that will be written
 * \return It returns 1 on success, 0 if not implemented, and -1 on failure.
 */
int tris_write_text(struct tris_channel *chan, struct tris_frame *frame);

/*! \brief Send empty audio to prime a channel driver */
int tris_prod(struct tris_channel *chan);

/*! \brief Sets read format on channel chan
 * Set read format for channel to whichever component of "format" is best.
 * \param chan channel to change
 * \param format format to change to
 * \return Returns 0 on success, -1 on failure
 */
int tris_set_read_format(struct tris_channel *chan, int format);

/*! \brief Sets write format on channel chan
 * Set write format for channel to whichever component of "format" is best.
 * \param chan channel to change
 * \param format new format for writing
 * \return Returns 0 on success, -1 on failure
 */
int tris_set_write_format(struct tris_channel *chan, int format);

/*!
 * \brief Sends text to a channel
 *
 * \param chan channel to act upon
 * \param text string of text to send on the channel
 *
 * Write text to a display on a channel
 *
 * \note The channel does not need to be locked before calling this function.
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_sendtext(struct tris_channel *chan, const char *text);

/*! \brief Receives a text character from a channel
 * \param chan channel to act upon
 * \param timeout timeout in milliseconds (0 for infinite wait)
 * Read a char of text from a channel
 * Returns 0 on success, -1 on failure
 */
int tris_recvchar(struct tris_channel *chan, int timeout);

/*! \brief Send a DTMF digit to a channel
 * Send a DTMF digit to a channel.
 * \param chan channel to act upon
 * \param digit the DTMF digit to send, encoded in ASCII
 * \param duration the duration of the digit ending in ms
 * \return Returns 0 on success, -1 on failure
 */
int tris_senddigit(struct tris_channel *chan, char digit, unsigned int duration);

/*! \brief Send a DTMF digit to a channel
 * Send a DTMF digit to a channel.
 * \param chan channel to act upon
 * \param digit the DTMF digit to send, encoded in ASCII
 * \return Returns 0 on success, -1 on failure
 */
int tris_senddigit_begin(struct tris_channel *chan, char digit);

/*! \brief Send a DTMF digit to a channel

 * Send a DTMF digit to a channel.
 * \param chan channel to act upon
 * \param digit the DTMF digit to send, encoded in ASCII
 * \param duration the duration of the digit ending in ms
 * \return Returns 0 on success, -1 on failure
 */
int tris_senddigit_end(struct tris_channel *chan, char digit, unsigned int duration);

/*! \brief Receives a text string from a channel
 * Read a string of text from a channel
 * \param chan channel to act upon
 * \param timeout timeout in milliseconds (0 for infinite wait)
 * \return the received text, or NULL to signify failure.
 */
char *tris_recvtext(struct tris_channel *chan, int timeout);

/*! \brief Browse channels in use
 * Browse the channels currently in use
 * \param prev where you want to start in the channel list
 * \return Returns the next channel in the list, NULL on end.
 * 	If it returns a channel, that channel *has been locked*!
 */
struct tris_channel *tris_channel_walk_locked(const struct tris_channel *prev);

/*! \brief Get channel by name or uniqueid (locks channel) */
struct tris_channel *tris_get_channel_by_name_locked(const char *chan);

/*! \brief Get channel by name or uniqueid prefix (locks channel) */
struct tris_channel *tris_get_channel_by_name_prefix_locked(const char *name, const int namelen);

/*! \brief Get channel by name or uniqueid prefix (locks channel) */
struct tris_channel *tris_walk_channel_by_name_prefix_locked(const struct tris_channel *chan, const char *name, const int namelen);

/*! \brief Get channel by exten (and optionally context) and lock it */
struct tris_channel *tris_get_channel_by_exten_locked(const char *exten, const char *context);

/*! \brief Get next channel by exten (and optionally context) and lock it */
struct tris_channel *tris_walk_channel_by_exten_locked(const struct tris_channel *chan, const char *exten,
						     const char *context);

/*! \brief Search for a channel based on the passed channel matching callback
 * Search for a channel based on the specified is_match callback, and return the
 * first channel that we match.  When returned, the channel will be locked.  Note
 * that the is_match callback is called with the passed channel locked, and should
 * return 0 if there is no match, and non-zero if there is.
 * \param is_match callback executed on each channel until non-zero is returned, or we
 *        run out of channels to search.
 * \param data data passed to the is_match callback during each invocation.
 * \return Returns the matched channel, or NULL if no channel was matched.
 */
struct tris_channel *tris_channel_search_locked(int (*is_match)(struct tris_channel *, void *), void *data);

/*! ! \brief Waits for a digit
 * \param c channel to wait for a digit on
 * \param ms how many milliseconds to wait
 * \return Returns <0 on error, 0 on no entry, and the digit on success. */
int tris_waitfordigit(struct tris_channel *c, int ms);

/*! \brief Wait for a digit
 Same as tris_waitfordigit() with audio fd for outputting read audio and ctrlfd to monitor for reading.
 * \param c channel to wait for a digit on
 * \param ms how many milliseconds to wait
 * \param audiofd audio file descriptor to write to if audio frames are received
 * \param ctrlfd control file descriptor to monitor for reading
 * \return Returns 1 if ctrlfd becomes available */
int tris_waitfordigit_full(struct tris_channel *c, int ms, int audiofd, int ctrlfd);

/*! Reads multiple digits
 * \param c channel to read from
 * \param s string to read in to.  Must be at least the size of your length
 * \param len how many digits to read (maximum)
 * \param timeout how long to timeout between digits
 * \param rtimeout timeout to wait on the first digit
 * \param enders digits to end the string
 * Read in a digit string "s", max length "len", maximum timeout between
   digits "timeout" (-1 for none), terminated by anything in "enders".  Give them rtimeout
   for the first digit.  Returns 0 on normal return, or 1 on a timeout.  In the case of
   a timeout, any digits that were read before the timeout will still be available in s.
   RETURNS 2 in full version when ctrlfd is available, NOT 1*/
int tris_readstring(struct tris_channel *c, char *s, int len, int timeout, int rtimeout, char *enders);
int tris_readstring_full(struct tris_channel *c, char *s, int len, int timeout, int rtimeout, char *enders, int audiofd, int ctrlfd);

/*! \brief Report DTMF on channel 0 */
#define TRIS_BRIDGE_DTMF_CHANNEL_0		(1 << 0)
/*! \brief Report DTMF on channel 1 */
#define TRIS_BRIDGE_DTMF_CHANNEL_1		(1 << 1)
/*! \brief Return all voice frames on channel 0 */
#define TRIS_BRIDGE_REC_CHANNEL_0		(1 << 2)
/*! \brief Return all voice frames on channel 1 */
#define TRIS_BRIDGE_REC_CHANNEL_1		(1 << 3)
/*! \brief Ignore all signal frames except NULL */
#define TRIS_BRIDGE_IGNORE_SIGS			(1 << 4)


/*! \brief Makes two channel formats compatible
 * \param c0 first channel to make compatible
 * \param c1 other channel to make compatible
 * Set two channels to compatible formats -- call before tris_channel_bridge in general .
 * \return Returns 0 on success and -1 if it could not be done */
int tris_channel_make_compatible(struct tris_channel *c0, struct tris_channel *c1);

/*! Bridge two channels together (early)
 * \param c0 first channel to bridge
 * \param c1 second channel to bridge
 * Bridge two channels (c0 and c1) together early. This implies either side may not be answered yet.
 * \return Returns 0 on success and -1 if it could not be done */
int tris_channel_early_bridge(struct tris_channel *c0, struct tris_channel *c1);

/*! Bridge two channels together
 * \param c0 first channel to bridge
 * \param c1 second channel to bridge
 * \param config config for the channels
 * \param fo destination frame(?)
 * \param rc destination channel(?)
 * Bridge two channels (c0 and c1) together.  If an important frame occurs, we return that frame in
   *rf (remember, it could be NULL) and which channel (0 or 1) in rc */
/* int tris_channel_bridge(struct tris_channel *c0, struct tris_channel *c1, int flags, struct tris_frame **fo, struct tris_channel **rc); */
int tris_channel_bridge(struct tris_channel *c0,struct tris_channel *c1,
	struct tris_bridge_config *config, struct tris_frame **fo, struct tris_channel **rc);

/*!
 * \brief Weird function made for call transfers
 *
 * \param original channel to make a copy of
 * \param clone copy of the original channel
 *
 * This is a very strange and freaky function used primarily for transfer.  Suppose that
 * "original" and "clone" are two channels in random situations.  This function takes
 * the guts out of "clone" and puts them into the "original" channel, then alerts the
 * channel driver of the change, asking it to fixup any private information (like the
 * p->owner pointer) that is affected by the change.  The physical layer of the original
 * channel is hung up.
 *
 * \note Neither channel passed here needs to be locked before calling this function.
 */
int tris_channel_masquerade(struct tris_channel *original, struct tris_channel *clone);

/*! Gives the string form of a given cause code */
/*!
 * \param state cause to get the description of
 * Give a name to a cause code
 * Returns the text form of the binary cause code given
 */
const char *tris_cause2str(int state) attribute_pure;

/*! Convert the string form of a cause code to a number */
/*!
 * \param name string form of the cause
 * Returns the cause code
 */
int tris_str2cause(const char *name) attribute_pure;

/*! Gives the string form of a given channel state */
/*!
 * \param tris_channel_state state to get the name of
 * Give a name to a state
 * Returns the text form of the binary state given
 */
const char *tris_state2str(enum tris_channel_state);

/*! Gives the string form of a given transfer capability */
/*!
 * \param transfercapability transfercapabilty to get the name of
 * Give a name to a transfercapbility
 * See above
 * Returns the text form of the binary transfer capability
 */
char *tris_transfercapability2str(int transfercapability) attribute_const;

/* Options: Some low-level drivers may implement "options" allowing fine tuning of the
   low level channel.  See frame.h for options.  Note that many channel drivers may support
   none or a subset of those features, and you should not count on this if you want your
   trismedia application to be portable.  They're mainly useful for tweaking performance */

/*! Sets an option on a channel */
/*!
 * \param channel channel to set options on
 * \param option option to change
 * \param data data specific to option
 * \param datalen length of the data
 * \param block blocking or not
 * Set an option on a channel (see frame.h), optionally blocking awaiting the reply
 * Returns 0 on success and -1 on failure
 */
int tris_channel_setoption(struct tris_channel *channel, int option, void *data, int datalen, int block);

/*! Pick the best codec  */
/* Choose the best codec...  Uhhh...   Yah. */
int tris_best_codec(int fmts);


/*! Checks the value of an option */
/*!
 * Query the value of an option
 * Works similarly to setoption except only reads the options.
 */
int tris_channel_queryoption(struct tris_channel *channel, int option, void *data, int *datalen, int block);

/*! Checks for HTML support on a channel */
/*! Returns 0 if channel does not support HTML or non-zero if it does */
int tris_channel_supports_html(struct tris_channel *channel);

/*! Sends HTML on given channel */
/*! Send HTML or URL on link.  Returns 0 on success or -1 on failure */
int tris_channel_sendhtml(struct tris_channel *channel, int subclass, const char *data, int datalen);

/*! Sends a URL on a given link */
/*! Send URL on link.  Returns 0 on success or -1 on failure */
int tris_channel_sendurl(struct tris_channel *channel, const char *url);

/*! Defers DTMF */
/*! Defer DTMF so that you only read things like hangups and audio.  Returns
   non-zero if channel was already DTMF-deferred or 0 if channel is just now
   being DTMF-deferred */
int tris_channel_defer_dtmf(struct tris_channel *chan);

/*! Undo defer.  tris_read will return any dtmf characters that were queued */
void tris_channel_undefer_dtmf(struct tris_channel *chan);

/*! Initiate system shutdown -- prevents new channels from being allocated.
    If "hangup" is non-zero, all existing channels will receive soft
     hangups */
void tris_begin_shutdown(int hangup);

/*! Cancels an existing shutdown and returns to normal operation */
void tris_cancel_shutdown(void);

/*! Returns number of active/allocated channels */
int tris_active_channels(void);

/*! Returns non-zero if Trismedia is being shut down */
int tris_shutting_down(void);

/*! Activate a given generator */
int tris_activate_generator(struct tris_channel *chan, struct tris_generator *gen, void *params);

/*! Deactivate an active generator */
void tris_deactivate_generator(struct tris_channel *chan);

/*!
 * \brief Set caller ID number, name and ANI
 *
 * \note The channel does not need to be locked before calling this function.
 */
void tris_set_callerid(struct tris_channel *chan, const char *cid_num, const char *cid_name, const char *cid_ani);

/*! Set the file descriptor on the channel */
void tris_channel_set_fd(struct tris_channel *chan, int which, int fd);

/*! Add a channel to an optimized waitfor */
void tris_poll_channel_add(struct tris_channel *chan0, struct tris_channel *chan1);

/*! Delete a channel from an optimized waitfor */
void tris_poll_channel_del(struct tris_channel *chan0, struct tris_channel *chan1);

/*! Start a tone going */
int tris_tonepair_start(struct tris_channel *chan, int freq1, int freq2, int duration, int vol);
/*! Stop a tone from playing */
void tris_tonepair_stop(struct tris_channel *chan);
/*! Play a tone pair for a given amount of time */
int tris_tonepair(struct tris_channel *chan, int freq1, int freq2, int duration, int vol);

/*!
 * \brief Automatically service a channel for us...
 *
 * \retval 0 success
 * \retval -1 failure, or the channel is already being autoserviced
 */
int tris_autoservice_start(struct tris_channel *chan);

/*!
 * \brief Stop servicing a channel for us...
 *
 * \note if chan is locked prior to calling tris_autoservice_stop, it
 * is likely that there will be a deadlock between the thread that calls
 * tris_autoservice_stop and the autoservice thread. It is important
 * that chan is not locked prior to this call
 *
 * \retval 0 success
 * \retval -1 error, or the channel has been hungup
 */
int tris_autoservice_stop(struct tris_channel *chan);

/*!
 * \brief Enable or disable timer ticks for a channel
 *
 * \param rate number of timer ticks per second
 *
 * If timers are supported, force a scheduled expiration on the
 * timer fd, at which point we call the callback function / data
 *
 * Call this function with a rate of 0 to turn off the timer ticks
 *
 * \version 1.6.1 changed samples parameter to rate, accomodates new timing methods
 */
int tris_settimeout(struct tris_channel *c, unsigned int rate, int (*func)(const void *data), void *data);

/*!	\brief Transfer a channel (if supported).  Returns -1 on error, 0 if not supported
   and 1 if supported and requested
	\param chan current channel
	\param dest destination extension for transfer
*/
int tris_transfer(struct tris_channel *chan, char *dest);

/*!	\brief  Start masquerading a channel
	XXX This is a seriously whacked out operation.  We're essentially putting the guts of
           the clone channel into the original channel.  Start by killing off the original
           channel's backend.   I'm not sure we're going to keep this function, because
           while the features are nice, the cost is very high in terms of pure nastiness. XXX
	\param chan 	Channel to masquerade
*/
int tris_do_masquerade(struct tris_channel *chan);

/*!	\brief Find bridged channel
	\param chan Current channel
*/
struct tris_channel *tris_bridged_channel(struct tris_channel *chan);

/*!
  \brief Inherits channel variable from parent to child channel
  \param parent Parent channel
  \param child Child channel

  Scans all channel variables in the parent channel, looking for those
  that should be copied into the child channel.
  Variables whose names begin with a single '_' are copied into the
  child channel with the prefix removed.
  Variables whose names begin with '__' are copied into the child
  channel with their names unchanged.
*/
void tris_channel_inherit_variables(const struct tris_channel *parent, struct tris_channel *child);

/*!
  \brief adds a list of channel variables to a channel
  \param chan the channel
  \param vars a linked list of variables

  Variable names can be for a regular channel variable or a dialplan function
  that has the ability to be written to.
*/
void tris_set_variables(struct tris_channel *chan, struct tris_variable *vars);

/*!
  \brief An opaque 'object' structure use by silence generators on channels.
 */
struct tris_silence_generator;

/*!
  \brief Starts a silence generator on the given channel.
  \param chan The channel to generate silence on
  \return An tris_silence_generator pointer, or NULL if an error occurs

  This function will cause SLINEAR silence to be generated on the supplied
  channel until it is disabled; if the channel cannot be put into SLINEAR
  mode then the function will fail.

  The pointer returned by this function must be preserved and passed to
  tris_channel_stop_silence_generator when you wish to stop the silence
  generation.
 */
struct tris_silence_generator *tris_channel_start_silence_generator(struct tris_channel *chan);

/*!
  \brief Stops a previously-started silence generator on the given channel.
  \param chan The channel to operate on
  \param state The tris_silence_generator pointer return by a previous call to
  tris_channel_start_silence_generator.
  \return nothing

  This function will stop the operating silence generator and return the channel
  to its previous write format.
 */
void tris_channel_stop_silence_generator(struct tris_channel *chan, struct tris_silence_generator *state);

/*!
  \brief Check if the channel can run in internal timing mode.
  \param chan The channel to check
  \return boolean

  This function will return 1 if internal timing is enabled and the timing
  device is available.
 */
int tris_internal_timing_enabled(struct tris_channel *chan);

/* Misc. functions below */

/*! \brief if fd is a valid descriptor, set *pfd with the descriptor
 * \return Return 1 (not -1!) if added, 0 otherwise (so we can add the
 * return value to the index into the array)
 */
static inline int tris_add_fd(struct pollfd *pfd, int fd)
{
	pfd->fd = fd;
	pfd->events = POLLIN | POLLPRI;
	return fd >= 0;
}

/*! \brief Helper function for migrating select to poll */
static inline int tris_fdisset(struct pollfd *pfds, int fd, int maximum, int *start)
{
	int x;
	int dummy = 0;

	if (fd < 0)
		return 0;
	if (!start)
		start = &dummy;
	for (x = *start; x < maximum; x++)
		if (pfds[x].fd == fd) {
			if (x == *start)
				(*start)++;
			return pfds[x].revents;
		}
	return 0;
}

#ifndef HAVE_TIMERSUB
static inline void timersub(struct timeval *tvend, struct timeval *tvstart, struct timeval *tvdiff)
{
	tvdiff->tv_sec = tvend->tv_sec - tvstart->tv_sec;
	tvdiff->tv_usec = tvend->tv_usec - tvstart->tv_usec;
	if (tvdiff->tv_usec < 0) {
		tvdiff->tv_sec --;
		tvdiff->tv_usec += 1000000;
	}

}
#endif

/*! \brief Waits for activity on a group of channels
 * \param nfds the maximum number of file descriptors in the sets
 * \param rfds file descriptors to check for read availability
 * \param wfds file descriptors to check for write availability
 * \param efds file descriptors to check for exceptions (OOB data)
 * \param tvp timeout while waiting for events
 * This is the same as a standard select(), except it guarantees the
 * behaviour where the passed struct timeval is updated with how much
 * time was not slept while waiting for the specified events
 */
static inline int tris_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *tvp)
{
#ifdef __linux__
	return select(nfds, rfds, wfds, efds, tvp);
#else
	if (tvp) {
		struct timeval tv, tvstart, tvend, tvlen;
		int res;

		tv = *tvp;
		gettimeofday(&tvstart, NULL);
		res = select(nfds, rfds, wfds, efds, tvp);
		gettimeofday(&tvend, NULL);
		timersub(&tvend, &tvstart, &tvlen);
		timersub(&tv, &tvlen, tvp);
		if (tvp->tv_sec < 0 || (tvp->tv_sec == 0 && tvp->tv_usec < 0)) {
			tvp->tv_sec = 0;
			tvp->tv_usec = 0;
		}
		return res;
	}
	else
		return select(nfds, rfds, wfds, efds, NULL);
#endif
}

/*! \brief Retrieves the current T38 state of a channel */
static inline enum tris_t38_state tris_channel_get_t38_state(struct tris_channel *chan)
{
	enum tris_t38_state state = T38_STATE_UNAVAILABLE;
	int datalen = sizeof(state);

	tris_channel_queryoption(chan, TRIS_OPTION_T38_STATE, &state, &datalen, 0);

	return state;
}

#define CHECK_BLOCKING(c) do { 	 \
	if (tris_test_flag(c, TRIS_FLAG_BLOCKING)) {\
		if (option_debug) \
			tris_log(LOG_DEBUG, "Thread %ld Blocking '%s', already blocked by thread %ld in procedure %s\n", (long) pthread_self(), (c)->name, (long) (c)->blocker, (c)->blockproc); \
	} else { \
		(c)->blocker = pthread_self(); \
		(c)->blockproc = __PRETTY_FUNCTION__; \
		tris_set_flag(c, TRIS_FLAG_BLOCKING); \
	} } while (0)

tris_group_t tris_get_group(const char *s);

/*! \brief print call- and pickup groups into buffer */
char *tris_print_group(char *buf, int buflen, tris_group_t group);

/*! \brief Convert enum channelreloadreason to text string for manager event
	\param reason	Enum channelreloadreason - reason for reload (manager, cli, start etc)
*/
const char *channelreloadreason2txt(enum channelreloadreason reason);

/*! \brief return an tris_variable list of channeltypes */
struct tris_variable *tris_channeltype_list(void);

/*!
  \brief return an english explanation of the code returned thru __tris_request_and_dial's 'outstate' argument
  \param reason  The integer argument, usually taken from TRIS_CONTROL_ macros
  \return char pointer explaining the code
 */
const char *tris_channel_reason2str(int reason);

void *tris_broad3channel_hangup_locked(const struct tris_channel *chan, const char *cid_num, const char *exten);
int tris_broad3channel_search_locked(const char *exten, const char *cid);
void tris_rakwonchannel_hangup(const struct tris_channel *chan);


/*! \brief channel group info
 */
struct tris_group_info {
	struct tris_channel *chan;
	char *category;
	char *group;
	TRIS_LIST_ENTRY(tris_group_info) group_list;
};


#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_CHANNEL_H */
