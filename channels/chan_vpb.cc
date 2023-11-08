/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2003, Paul Bagyenda
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * Copyright (C) 2004 - 2005, Ben Kramer
 * Ben Kramer <ben@voicetronix.com.au>
 *
 * Daniel Bichara <daniel@bichara.com.br> - Brazilian CallerID detection (c)2004 
 *
 * Welber Silveira - welberms@magiclink.com.br - (c)2004
 * Copying CLID string to propper structure after detection
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
 * \brief VoiceTronix Interface driver
 * 
 * \ingroup channel_drivers
 */

/*** MODULEINFO
	<depend>vpb</depend>
 ***/

#include <vpbapi.h>

extern "C" {

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 228093 $")

#include "trismedia/lock.h"
#include "trismedia/utils.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/callerid.h"
#include "trismedia/dsp.h"
#include "trismedia/features.h"
#include "trismedia/musiconhold.h"
}

#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <ctype.h>

#include <assert.h>

#ifdef pthread_create
#undef pthread_create
#endif

#define DEFAULT_GAIN 0
#define DEFAULT_ECHO_CANCEL 1
  
#define VPB_SAMPLES 160 
#define VPB_MAX_BUF VPB_SAMPLES*4 + TRIS_FRIENDLY_OFFSET

#define VPB_NULL_EVENT 200

#define VPB_WAIT_TIMEOUT 4000

#define MAX_VPB_GAIN 12.0
#define MIN_VPB_GAIN -12.0

#define DTMF_CALLERID  
#define DTMF_CID_START 'D'
#define DTMF_CID_STOP 'C'

/**/
#if defined(__cplusplus) || defined(c_plusplus)
 extern "C" {
#endif
/**/

static const char desc[] = "VoiceTronix V6PCI/V12PCI/V4PCI  API Support";
static const char tdesc[] = "Standard VoiceTronix API Driver";
static const char config[] = "vpb.conf";

/* Default context for dialtone mode */
static char context[TRIS_MAX_EXTENSION] = "default";

/* Default language */
static char language[MAX_LANGUAGE] = "";

static int gruntdetect_timeout = 3600000; /* Grunt detect timeout is 1hr. */

static const int prefformat = TRIS_FORMAT_SLINEAR;

/* Protect the interface list (of vpb_pvt's) */
TRIS_MUTEX_DEFINE_STATIC(iflock);

/* Protect the monitoring thread, so only one process can kill or start it, and not
   when it's doing something critical. */
TRIS_MUTEX_DEFINE_STATIC(monlock);

/* This is the thread for the monitor which checks for input on the channels
   which are not currently in use.  */
static pthread_t monitor_thread;

static int mthreadactive = -1; /* Flag for monitoring monitorthread.*/


static int restart_monitor(void);

/* The private structures of the VPB channels are 
   linked for selecting outgoing channels */
   
#define MODE_DIALTONE 	1
#define MODE_IMMEDIATE	2
#define MODE_FXO	3

/* Pick a country or add your own! */
/* These are the tones that are played to the user */
#define TONES_AU
/* #define TONES_USA */

#ifdef TONES_AU
static VPB_TONE Dialtone     = {440,   	440, 	440, 	-10,  	-10, 	-10, 	5000,	0   };
static VPB_TONE Busytone     = {470,   	0,   	0, 	-10,  	-100, 	-100,   5000, 	0 };
static VPB_TONE Ringbacktone = {400,   	50,   	440, 	-10,  	-10, 	-10,  	1400, 	800 };
#endif
#ifdef TONES_USA
static VPB_TONE Dialtone     = {350, 440,   0, -16,   -16, -100, 10000,    0};
static VPB_TONE Busytone     = {480, 620,   0, -10,   -10, -100,   500,  500};
static VPB_TONE Ringbacktone = {440, 480,   0, -20,   -20, -100,  2000, 4000};
#endif

/* grunt tone defn's */
#if 0
static VPB_DETECT toned_grunt = { 3, VPB_GRUNT, 1, 2000, 3000, 0, 0, -40, 0, 0, 0, 40, { { VPB_DELAY, 1000, 0, 0 }, { VPB_RISING, 0, 40, 0 }, { 0, 100, 0, 0 } } };
#endif
static VPB_DETECT toned_ungrunt = { 2, VPB_GRUNT, 1, 2000, 1, 0, 0, -40, 0, 0, 30, 40, { { 0, 0, 0, 0 } } };

/* Use loop polarity detection for CID */
static int UsePolarityCID=0;

/* Use loop drop detection */
static int UseLoopDrop=1;

/* To use or not to use Native bridging */
static int UseNativeBridge=1;

/* Use Trismedia Indication or VPB */
static int use_tris_ind=0;

/* Use Trismedia DTMF detection or VPB */
static int use_tris_dtmfdet=0;

static int relaxdtmf=0;

/* Use Trismedia DTMF play back or VPB */
static int use_tris_dtmf=0;

/* Break for DTMF on native bridge ? */
static int break_for_dtmf=1;

/* Set EC suppression threshold */
static short ec_supp_threshold=-1;

/* Inter Digit Delay for collecting DTMF's */
static int dtmf_idd = 3000;

#define TIMER_PERIOD_RINGBACK 2000
#define TIMER_PERIOD_BUSY 700
#define TIMER_PERIOD_RING 4000
static int timer_period_ring = TIMER_PERIOD_RING;
	  
#define VPB_EVENTS_ALL (VPB_MRING|VPB_MDIGIT|VPB_MDTMF|VPB_MTONEDETECT|VPB_MTIMEREXP \
			|VPB_MSTATION_OFFHOOK|VPB_MSTATION_ONHOOK \
			|VPB_MRING_OFF|VPB_MDROP|VPB_MSTATION_FLASH)
#define VPB_EVENTS_NODROP (VPB_MRING|VPB_MDIGIT|VPB_MDTMF|VPB_MTONEDETECT|VPB_MTIMEREXP \
			|VPB_MSTATION_OFFHOOK|VPB_MSTATION_ONHOOK \
			|VPB_MRING_OFF|VPB_MSTATION_FLASH)
#define VPB_EVENTS_NODTMF (VPB_MRING|VPB_MDIGIT|VPB_MTONEDETECT|VPB_MTIMEREXP \
			|VPB_MSTATION_OFFHOOK|VPB_MSTATION_ONHOOK \
			|VPB_MRING_OFF|VPB_MDROP|VPB_MSTATION_FLASH)
#define VPB_EVENTS_STAT (VPB_MRING|VPB_MDIGIT|VPB_MDTMF|VPB_MTONEDETECT|VPB_MTIMEREXP \
			|VPB_MSTATION_OFFHOOK|VPB_MSTATION_ONHOOK \
			|VPB_MRING_OFF|VPB_MSTATION_FLASH)


/* Dialing parameters for Australia */
/* #define DIAL_WITH_CALL_PROGRESS */
VPB_TONE_MAP DialToneMap[] = { 	{ VPB_BUSY, VPB_CALL_DISCONNECT, 0 },
  				{ VPB_DIAL, VPB_CALL_DIALTONE, 0 },
				{ VPB_RINGBACK, VPB_CALL_RINGBACK, 0 },
				{ VPB_BUSY, VPB_CALL_BUSY, 0 },
				{ VPB_GRUNT, VPB_CALL_GRUNT, 0 },
				{ 0, 0, 1 } };
#define VPB_DIALTONE_WAIT 2000 /* Wait up to 2s for a dialtone */
#define VPB_RINGWAIT 4000 /* Wait up to 4s for ring tone after dialing */
#define VPB_CONNECTED_WAIT 4000 /* If no ring tone detected for 4s then consider call connected */
#define TIMER_PERIOD_NOANSWER 120000 /* Let it ring for 120s before deciding theres noone there */

#define MAX_BRIDGES_V4PCI 2
#define MAX_BRIDGES_V12PCI 128

/* port states */
#define VPB_STATE_ONHOOK	0
#define VPB_STATE_OFFHOOK	1
#define VPB_STATE_DIALLING	2
#define VPB_STATE_JOINED	3
#define VPB_STATE_GETDTMF	4
#define VPB_STATE_PLAYDIAL	5
#define VPB_STATE_PLAYBUSY	6
#define VPB_STATE_PLAYRING	7

#define VPB_GOT_RXHWG		1
#define VPB_GOT_TXHWG		2
#define VPB_GOT_RXSWG		4
#define VPB_GOT_TXSWG		8

typedef struct  {
	int inuse;
	struct tris_channel *c0, *c1, **rc;
	struct tris_frame **fo;
	int flags;
	tris_mutex_t lock;
	tris_cond_t cond;
	int endbridge;
} vpb_bridge_t;

static vpb_bridge_t * bridges;
static int max_bridges = MAX_BRIDGES_V4PCI;

TRIS_MUTEX_DEFINE_STATIC(bridge_lock);

typedef enum {
	vpb_model_unknown = 0, 
	vpb_model_v4pci,
	vpb_model_v12pci
} vpb_model_t;

static struct vpb_pvt {

	tris_mutex_t owner_lock;           /*!< Protect blocks that expect ownership to remain the same */
	struct tris_channel *owner;        /*!< Channel who owns us, possibly NULL */

	int golock;                       /*!< Got owner lock ? */

	int mode;                         /*!< fxo/imediate/dialtone */
	int handle;                       /*!< Handle for vpb interface */

	int state;                        /*!< used to keep port state (internal to driver) */

	int group;                        /*!< Which group this port belongs to */
	tris_group_t callgroup;            /*!< Call group */
	tris_group_t pickupgroup;          /*!< Pickup group */


	char dev[256];                    /*!< Device name, eg vpb/1-1 */
	vpb_model_t vpb_model;            /*!< card model */

	struct tris_frame f, fr;           /*!< Trismedia frame interface */
	char buf[VPB_MAX_BUF];            /*!< Static buffer for reading frames */

	int dialtone;                     /*!< NOT USED */
	float txgain, rxgain;             /*!< Hardware gain control */
	float txswgain, rxswgain;         /*!< Software gain control */

	int wantdtmf;                     /*!< Waiting for DTMF. */
	char context[TRIS_MAX_EXTENSION];  /*!< The context for this channel */

	char ext[TRIS_MAX_EXTENSION];      /*!< DTMF buffer for the ext[ens] */
	char language[MAX_LANGUAGE];      /*!< language being used */
	char callerid[TRIS_MAX_EXTENSION]; /*!< CallerId used for directly connected phone */
	int  callerid_type;               /*!< Caller ID type: 0=>none 1=>vpb 2=>AstV23 3=>AstBell */
	char cid_num[TRIS_MAX_EXTENSION];
	char cid_name[TRIS_MAX_EXTENSION];

	int dtmf_caller_pos;              /*!< DTMF CallerID detection (Brazil)*/

	int lastoutput;                   /*!< Holds the last Audio format output'ed */
	int lastinput;                    /*!< Holds the last Audio format input'ed */
	int last_ignore_dtmf;

	void *busy_timer;                 /*!< Void pointer for busy vpb_timer */
	int busy_timer_id;                /*!< unique timer ID for busy timer */

	void *ringback_timer;             /*!< Void pointer for ringback vpb_timer */
	int ringback_timer_id;            /*!< unique timer ID for ringback timer */

	void *ring_timer;                 /*!< Void pointer for ring vpb_timer */
	int ring_timer_id;                /*!< unique timer ID for ring timer */

	void *dtmfidd_timer;              /*!< Void pointer for DTMF IDD vpb_timer */
	int dtmfidd_timer_id;             /*!< unique timer ID for DTMF IDD timer */

	struct tris_dsp *vad;              /*!< AST  Voice Activation Detection dsp */

	struct timeval lastgrunt;         /*!< time stamp of last grunt event */

	tris_mutex_t lock;                 /*!< This one just protects bridge ptr below */
	vpb_bridge_t *bridge;

	int stopreads;                    /*!< Stop reading...*/
	int read_state;                   /*!< Read state */
	int chuck_count;                  /*!< a count of packets weve chucked away!*/
	pthread_t readthread;             /*!< For monitoring read channel. One per owned channel. */

	tris_mutex_t record_lock;          /*!< This one prevents reentering a record_buf block */
	tris_mutex_t play_lock;            /*!< This one prevents reentering a play_buf block */
	int  play_buf_time;               /*!< How long the last play_buf took */
	struct timeval lastplay;          /*!< Last play time */

	tris_mutex_t play_dtmf_lock;
	char play_dtmf[16];

	int faxhandled;                   /*!< has a fax tone been handled ? */

	struct vpb_pvt *next;             /*!< Next channel in list */

} *iflist = NULL;

static struct tris_channel *vpb_new(struct vpb_pvt *i, enum tris_channel_state state, const char *context);
static void *do_chanreads(void *pvt);
static struct tris_channel *vpb_request(const char *type, int format, void *data, int *cause);
static int vpb_digit_begin(struct tris_channel *ast, char digit);
static int vpb_digit_end(struct tris_channel *ast, char digit, unsigned int duration);
static int vpb_call(struct tris_channel *ast, char *dest, int timeout);
static int vpb_hangup(struct tris_channel *ast);
static int vpb_answer(struct tris_channel *ast);
static struct tris_frame *vpb_read(struct tris_channel *ast);
static int vpb_write(struct tris_channel *ast, struct tris_frame *frame);
static enum tris_bridge_result tris_vpb_bridge(struct tris_channel *c0, struct tris_channel *c1, int flags, struct tris_frame **fo, struct tris_channel **rc, int timeoutms);
static int vpb_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen);
static int vpb_fixup(struct tris_channel *oldchan, struct tris_channel *newchan);

static struct tris_channel_tech vpb_tech = {
	type: "vpb",
	description: tdesc,
	capabilities: TRIS_FORMAT_SLINEAR,
	properties: 0,
	requester: vpb_request,
	devicestate: NULL,
	send_digit_begin: vpb_digit_begin,
	send_digit_end: vpb_digit_end,
	call: vpb_call,
	hangup: vpb_hangup,
	answer: vpb_answer,
	read: vpb_read,
	write: vpb_write,
	send_text: NULL,
	send_image: NULL,
	send_html: NULL,
	exception: NULL,
	bridge: tris_vpb_bridge,
	early_bridge: NULL,
	indicate: vpb_indicate,
	fixup: vpb_fixup,
	setoption: NULL,
	queryoption: NULL,
	transfer: NULL,
	write_video: NULL,
	write_text: NULL,
	bridged_channel: NULL,
	func_channel_read: NULL,
	func_channel_write: NULL,
	get_base_channel: NULL,
	set_base_channel: NULL
};

static struct tris_channel_tech vpb_tech_indicate = {
	type: "vpb",
	description: tdesc,
	capabilities: TRIS_FORMAT_SLINEAR,
	properties: 0,
	requester: vpb_request,
	devicestate: NULL,
	send_digit_begin: vpb_digit_begin,
	send_digit_end: vpb_digit_end,
	call: vpb_call,
	hangup: vpb_hangup,
	answer: vpb_answer,
	read: vpb_read,
	write: vpb_write,
	send_text: NULL,
	send_image: NULL,
	send_html: NULL,
	exception: NULL,
	bridge: tris_vpb_bridge,
	early_bridge: NULL,
	indicate: NULL,
	fixup: vpb_fixup,
	setoption: NULL,
	queryoption: NULL,
	transfer: NULL,
	write_video: NULL,
	write_text: NULL,
	bridged_channel: NULL,
	func_channel_read: NULL,
	func_channel_write: NULL,
	get_base_channel: NULL,
	set_base_channel: NULL
};

/* Can't get tris_vpb_bridge() working on v4pci without either a horrible 
*  high pitched feedback noise or bad hiss noise depending on gain settings
*  Get trismedia to do the bridging
*/
#define BAD_V4PCI_BRIDGE

/* This one enables a half duplex bridge which may be required to prevent high pitched
 * feedback when getting trismedia to do the bridging and when using certain gain settings.
 */
/* #define HALF_DUPLEX_BRIDGE */

/* This is the Native bridge code, which Trismedia will try before using its own bridging code */
static enum tris_bridge_result tris_vpb_bridge(struct tris_channel *c0, struct tris_channel *c1, int flags, struct tris_frame **fo, struct tris_channel **rc, int timeoutms)
{
	struct vpb_pvt *p0 = (struct vpb_pvt *)c0->tech_pvt;
	struct vpb_pvt *p1 = (struct vpb_pvt *)c1->tech_pvt;
	int i;
	int res;
	struct tris_channel *cs[3];
	struct tris_channel *who;
	struct tris_frame *f;

	cs[0] = c0;
	cs[1] = c1;

	#ifdef BAD_V4PCI_BRIDGE
	if (p0->vpb_model == vpb_model_v4pci)
		return TRIS_BRIDGE_FAILED_NOWARN;
	#endif
	if (UseNativeBridge != 1) {
		return TRIS_BRIDGE_FAILED_NOWARN;
	}

/*
	tris_mutex_lock(&p0->lock);
	tris_mutex_lock(&p1->lock);
*/

	/* Bridge channels, check if we can.  I believe we always can, so find a slot.*/

	tris_mutex_lock(&bridge_lock);
	for (i = 0; i < max_bridges; i++) 
		if (!bridges[i].inuse)
			break;
	if (i < max_bridges) {
		bridges[i].inuse = 1;
		bridges[i].endbridge = 0;
		bridges[i].flags = flags;
		bridges[i].rc = rc;
		bridges[i].fo = fo;
		bridges[i].c0 = c0;
		bridges[i].c1 = c1;
	} 	       
	tris_mutex_unlock(&bridge_lock); 

	if (i == max_bridges) {
		tris_log(LOG_WARNING, "%s: vpb_bridge: Failed to bridge %s and %s!\n", p0->dev, c0->name, c1->name);
		tris_mutex_unlock(&p0->lock);
		tris_mutex_unlock(&p1->lock);
		return TRIS_BRIDGE_FAILED_NOWARN;
	} else {
		/* Set bridge pointers. You don't want to take these locks while holding bridge lock.*/
		tris_mutex_lock(&p0->lock);
		p0->bridge = &bridges[i];
		tris_mutex_unlock(&p0->lock);

		tris_mutex_lock(&p1->lock);
		p1->bridge = &bridges[i];
		tris_mutex_unlock(&p1->lock);

		tris_verb(2, "%s: vpb_bridge: Bridging call entered with [%s, %s]\n", p0->dev, c0->name, c1->name);
	}

	tris_verb(3, "Native bridging %s and %s\n", c0->name, c1->name);

	#ifdef HALF_DUPLEX_BRIDGE

	tris_debug(2, "%s: vpb_bridge: Starting half-duplex bridge [%s, %s]\n", p0->dev, c0->name, c1->name);

	int dir = 0;

	memset(p0->buf, 0, sizeof(p0->buf));
	memset(p1->buf, 0, sizeof(p1->buf));

	vpb_record_buf_start(p0->handle, VPB_ALAW);
	vpb_record_buf_start(p1->handle, VPB_ALAW);

	vpb_play_buf_start(p0->handle, VPB_ALAW);
	vpb_play_buf_start(p1->handle, VPB_ALAW);

	while (!bridges[i].endbridge) {
		struct vpb_pvt *from, *to;
		if (++dir % 2) {
			from = p0;
			to = p1;
		} else {
			from = p1;
			to = p0;
		}
		vpb_record_buf_sync(from->handle, from->buf, VPB_SAMPLES);
		vpb_play_buf_sync(to->handle, from->buf, VPB_SAMPLES);
	}

	vpb_record_buf_finish(p0->handle);
	vpb_record_buf_finish(p1->handle);

	vpb_play_buf_finish(p0->handle);
	vpb_play_buf_finish(p1->handle);

	tris_debug(2, "%s: vpb_bridge: Finished half-duplex bridge [%s, %s]\n", p0->dev, c0->name, c1->name);

	res = VPB_OK;

	#else

	res = vpb_bridge(p0->handle, p1->handle, VPB_BRIDGE_ON);
	if (res == VPB_OK) {
		/* pthread_cond_wait(&bridges[i].cond, &bridges[i].lock);*/ /* Wait for condition signal. */
		while (!bridges[i].endbridge) {
			/* Are we really ment to be doing nothing ?!?! */
			who = tris_waitfor_n(cs, 2, &timeoutms);
			if (!who) {
				if (!timeoutms) {
					res = TRIS_BRIDGE_RETRY;
					break;
				}
				tris_debug(1, "%s: vpb_bridge: Empty frame read...\n", p0->dev);
				/* check for hangup / whentohangup */
				if (tris_check_hangup(c0) || tris_check_hangup(c1))
					break;
				continue;
			}
			f = tris_read(who);
			if (!f || ((f->frametype == TRIS_FRAME_DTMF) &&
					   (((who == c0) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_0)) || 
				       ((who == c1) && (flags & TRIS_BRIDGE_DTMF_CHANNEL_1))))) {
				*fo = f;
				*rc = who;
				tris_debug(1, "%s: vpb_bridge: Got a [%s]\n", p0->dev, f ? "digit" : "hangup");
#if 0
				if ((c0->tech_pvt == pvt0) && (!tris_check_hangup(c0))) {
					if (pr0->set_rtp_peer(c0, NULL, NULL, 0)) 
						tris_log(LOG_WARNING, "Channel '%s' failed to revert\n", c0->name);
				}
				if ((c1->tech_pvt == pvt1) && (!tris_check_hangup(c1))) {
					if (pr1->set_rtp_peer(c1, NULL, NULL, 0)) 
						tris_log(LOG_WARNING, "Channel '%s' failed to revert back\n", c1->name);
				}
				/* That's all we needed */
				return 0;
#endif
				/* Check if we need to break */
				if (break_for_dtmf) {
					break;
				} else if ((f->frametype == TRIS_FRAME_DTMF) && ((f->subclass == '#') || (f->subclass == '*'))) {
					break;
				}
			} else {
				if ((f->frametype == TRIS_FRAME_DTMF) || 
					(f->frametype == TRIS_FRAME_VOICE) || 
					(f->frametype == TRIS_FRAME_VIDEO)) 
					{
					/* Forward voice or DTMF frames if they happen upon us */
					/* Actually I dont think we want to forward on any frames!
					if (who == c0) {
						tris_write(c1, f);
					} else if (who == c1) {
						tris_write(c0, f);
					}
					*/
				}
				tris_frfree(f);
			}
			/* Swap priority not that it's a big deal at this point */
			cs[2] = cs[0];
			cs[0] = cs[1];
			cs[1] = cs[2];
		};
		vpb_bridge(p0->handle, p1->handle, VPB_BRIDGE_OFF); 
	}

	#endif

	tris_mutex_lock(&bridge_lock);
	bridges[i].inuse = 0;
	tris_mutex_unlock(&bridge_lock); 

	p0->bridge = NULL;
	p1->bridge = NULL;


	tris_verb(2, "Bridging call done with [%s, %s] => %d\n", c0->name, c1->name, res);

/*
	tris_mutex_unlock(&p0->lock);
	tris_mutex_unlock(&p1->lock);
*/
	return (res == VPB_OK) ? TRIS_BRIDGE_COMPLETE : TRIS_BRIDGE_FAILED;
}

/* Caller ID can be located in different positions between the rings depending on your Telco
 * Australian (Telstra) callerid starts 700ms after 1st ring and finishes 1.5s after first ring
 * Use ANALYSE_CID to record rings and determine location of callerid
 */
/* #define ANALYSE_CID */
#define RING_SKIP 300
#define CID_MSECS 2000

static void get_callerid(struct vpb_pvt *p)
{
	short buf[CID_MSECS*8]; /* 8kHz sampling rate */
	struct timeval cid_record_time;
	int rc;
	struct tris_channel *owner = p->owner;
/*
	char callerid[TRIS_MAX_EXTENSION] = ""; 
*/
#ifdef ANALYSE_CID
	void * ws;
	char * file="cidsams.wav";
#endif


	if (tris_mutex_trylock(&p->record_lock) == 0) {

		cid_record_time = tris_tvnow();
		tris_verb(4, "CID record - start\n");

		/* Skip any trailing ringtone */
		if (UsePolarityCID != 1){
			vpb_sleep(RING_SKIP);
		}

		tris_verb(4, "CID record - skipped %dms trailing ring\n",
				 tris_tvdiff_ms(tris_tvnow(), cid_record_time));
		cid_record_time = tris_tvnow();

		/* Record bit between the rings which contains the callerid */
		vpb_record_buf_start(p->handle, VPB_LINEAR);
		rc = vpb_record_buf_sync(p->handle, (char*)buf, sizeof(buf));
		vpb_record_buf_finish(p->handle);
#ifdef ANALYSE_CID
		vpb_wave_open_write(&ws, file, VPB_LINEAR);
		vpb_wave_write(ws, (char *)buf, sizeof(buf));
		vpb_wave_close_write(ws);
#endif

		tris_verb(4, "CID record - recorded %dms between rings\n",
				 tris_tvdiff_ms(tris_tvnow(), cid_record_time));

		tris_mutex_unlock(&p->record_lock);

		if (rc != VPB_OK) {
			tris_log(LOG_ERROR, "Failed to record caller id sample on %s\n", p->dev);
			return;
		}

		VPB_CID *cli_struct = new VPB_CID;
		cli_struct->ra_cldn[0] = 0;
		cli_struct->ra_cn[0] = 0;
		/* This decodes FSK 1200baud type callerid */
		if ((rc = vpb_cid_decode2(cli_struct, buf, CID_MSECS * 8)) == VPB_OK ) {
			/*
			if (owner->cid.cid_num)
				tris_free(owner->cid.cid_num);
			owner->cid.cid_num=NULL;
			if (owner->cid.cid_name)
				tris_free(owner->cid.cid_name);
			owner->cid.cid_name=NULL;
			*/
			
			if (cli_struct->ra_cldn[0] == '\0') {
				/*
				owner->cid.cid_num = tris_strdup(cli_struct->cldn);
				owner->cid.cid_name = tris_strdup(cli_struct->cn);
				*/
				if (owner) {
					tris_set_callerid(owner, cli_struct->cldn, cli_struct->cn, cli_struct->cldn);
				} else {
					strcpy(p->cid_num, cli_struct->cldn);
					strcpy(p->cid_name, cli_struct->cn);
				}
				tris_verb(4, "CID record - got [%s] [%s]\n", owner->cid.cid_num, owner->cid.cid_name);
				snprintf(p->callerid, sizeof(p->callerid), "%s %s", cli_struct->cldn, cli_struct->cn);
			} else {
				tris_log(LOG_ERROR, "CID record - No caller id avalable on %s \n", p->dev);
			}

		} else {
			tris_log(LOG_ERROR, "CID record - Failed to decode caller id on %s - %d\n", p->dev, rc);
			tris_copy_string(p->callerid, "unknown", sizeof(p->callerid));
		}
		delete cli_struct;

	} else 
		tris_log(LOG_ERROR, "CID record - Failed to set record mode for caller id on %s\n", p->dev);
}

static void get_callerid_ast(struct vpb_pvt *p)
{
	struct callerid_state *cs;
	char buf[1024];
	char *name = NULL, *number = NULL;
	int flags;
	int rc = 0, vrc;
	int sam_count = 0;
	struct tris_channel *owner = p->owner;
	int which_cid;
/*
	float old_gain;
*/
#ifdef ANALYSE_CID
	void * ws;
	char * file = "cidsams.wav";
#endif

	if (p->callerid_type == 1) {
		tris_verb(4, "Collected caller ID already\n");
		return;
	}
	else if (p->callerid_type == 2 ) {
		which_cid = CID_SIG_V23;
		tris_verb(4, "Collecting Caller ID v23...\n");
	}
	else if (p->callerid_type == 3) {
		which_cid = CID_SIG_BELL;
		tris_verb(4, "Collecting Caller ID bell...\n");
	} else {
		tris_verb(4, "Caller ID disabled\n");
		return;
	}
/*	vpb_sleep(RING_SKIP); */
/*	vpb_record_get_gain(p->handle, &old_gain); */
	cs = callerid_new(which_cid);
	if (cs) {
#ifdef ANALYSE_CID
		vpb_wave_open_write(&ws, file, VPB_MULAW); 
		vpb_record_set_gain(p->handle, 3.0); 
		vpb_record_set_hw_gain(p->handle, 12.0); 
#endif
		vpb_record_buf_start(p->handle, VPB_MULAW);
		while ((rc == 0) && (sam_count < 8000 * 3)) {
			vrc = vpb_record_buf_sync(p->handle, (char*)buf, sizeof(buf));
			if (vrc != VPB_OK)
				tris_log(LOG_ERROR, "%s: Caller ID couldn't read audio buffer!\n", p->dev);
			rc = callerid_feed(cs, (unsigned char *)buf, sizeof(buf), TRIS_FORMAT_ULAW);
#ifdef ANALYSE_CID
			vpb_wave_write(ws, (char *)buf, sizeof(buf)); 
#endif
			sam_count += sizeof(buf);
			tris_verb(4, "Collecting Caller ID samples [%d][%d]...\n", sam_count, rc);
		}
		vpb_record_buf_finish(p->handle);
#ifdef ANALYSE_CID
		vpb_wave_close_write(ws);
#endif
		if (rc == 1) {
			callerid_get(cs, &name, &number, &flags);
			tris_debug(1, "%s: Caller ID name [%s] number [%s] flags [%d]\n", p->dev, name, number, flags);
		} else {
			tris_log(LOG_ERROR, "%s: Failed to decode Caller ID \n", p->dev);
		}
/*		vpb_record_set_gain(p->handle, old_gain); */
/*		vpb_record_set_hw_gain(p->handle,6.0); */
	} else {
		tris_log(LOG_ERROR, "%s: Failed to create Caller ID struct\n", p->dev);
	}
	if (owner->cid.cid_num) {
		tris_free(owner->cid.cid_num);
		owner->cid.cid_num = NULL;
	}
	if (owner->cid.cid_name) {
		tris_free(owner->cid.cid_name);
		owner->cid.cid_name = NULL;
	}
	if (number)
		tris_shrink_phone_number(number);
	tris_set_callerid(owner,
		number, name,
		owner->cid.cid_ani ? NULL : number);
	if (!tris_strlen_zero(name)){
		snprintf(p->callerid, sizeof(p->callerid), "%s %s", number, name);
	} else {
		tris_copy_string(p->callerid, number, sizeof(p->callerid));
	}
	if (cs)
		callerid_free(cs);
}

/* Terminate any tones we are presently playing */
static void stoptone(int handle)
{
	int ret;
	VPB_EVENT je;
	while (vpb_playtone_state(handle) != VPB_OK) {
		vpb_tone_terminate(handle);
		ret = vpb_get_event_ch_async(handle, &je);
		if ((ret == VPB_OK) && (je.type != VPB_DIALEND)) {
			tris_verb(4, "Stop tone collected a wrong event!![%d]\n", je.type);
/*			vpb_put_event(&je); */
		}
		vpb_sleep(10);
	}
}

/* Safe vpb_playtone_async */
static int playtone( int handle, VPB_TONE *tone)
{
	int ret = VPB_OK;
	stoptone(handle);
	tris_verb(4, "[%02d]: Playing tone\n", handle);
	ret = vpb_playtone_async(handle, tone);
	return ret;
}

static inline int monitor_handle_owned(struct vpb_pvt *p, VPB_EVENT *e)
{
	struct tris_frame f = {TRIS_FRAME_CONTROL}; /* default is control, Clear rest. */
	int endbridge = 0;
	int res = 0;

	tris_verb(4, "%s: handle_owned: got event: [%d=>%d]\n", p->dev, e->type, e->data);

	f.src = "vpb";
	switch (e->type) {
	case VPB_RING:
		if (p->mode == MODE_FXO) {
			f.subclass = TRIS_CONTROL_RING;
			vpb_timer_stop(p->ring_timer);
			vpb_timer_start(p->ring_timer);
		} else
			f.frametype = TRIS_FRAME_NULL; /* ignore ring on station port. */
		break;

	case VPB_RING_OFF:
		f.frametype = TRIS_FRAME_NULL;
		break;

	case VPB_TIMEREXP:
		if (e->data == p->busy_timer_id) {
			playtone(p->handle, &Busytone);
			p->state = VPB_STATE_PLAYBUSY;
			vpb_timer_stop(p->busy_timer);
			vpb_timer_start(p->busy_timer);
			f.frametype = TRIS_FRAME_NULL;
		} else if (e->data == p->ringback_timer_id) {
			playtone(p->handle, &Ringbacktone);
			vpb_timer_stop(p->ringback_timer);
			vpb_timer_start(p->ringback_timer);
			f.frametype = TRIS_FRAME_NULL;
		} else if (e->data == p->ring_timer_id) {
			/* We didnt get another ring in time! */
			if (p->owner->_state != TRIS_STATE_UP)  {
				 /* Assume caller has hung up */
				vpb_timer_stop(p->ring_timer);
				f.subclass = TRIS_CONTROL_HANGUP;
			} else {
				vpb_timer_stop(p->ring_timer);
				f.frametype = TRIS_FRAME_NULL;
			}
				
		} else {
				f.frametype = TRIS_FRAME_NULL; /* Ignore. */
		}
		break;

	case VPB_DTMF_DOWN:
	case VPB_DTMF:
		if (use_tris_dtmfdet) {
			f.frametype = TRIS_FRAME_NULL;
		} else if (p->owner->_state == TRIS_STATE_UP) {
			f.frametype = TRIS_FRAME_DTMF;
			f.subclass = e->data;
		} else
			f.frametype = TRIS_FRAME_NULL;
		break;

	case VPB_TONEDETECT:
		if (e->data == VPB_BUSY || e->data == VPB_BUSY_308 || e->data == VPB_BUSY_AUST ) {
			tris_debug(4, "%s: handle_owned: got event: BUSY\n", p->dev);
			if (p->owner->_state == TRIS_STATE_UP) {
				f.subclass = TRIS_CONTROL_HANGUP;
			} else {
				f.subclass = TRIS_CONTROL_BUSY;
			}
		} else if (e->data == VPB_FAX) {
			if (!p->faxhandled) {
				if (strcmp(p->owner->exten, "fax")) {
					const char *target_context = S_OR(p->owner->macrocontext, p->owner->context);

					if (tris_exists_extension(p->owner, target_context, "fax", 1, p->owner->cid.cid_num)) {
						tris_verb(3, "Redirecting %s to fax extension\n", p->owner->name);
						/* Save the DID/DNIS when we transfer the fax call to a "fax" extension */
						pbx_builtin_setvar_helper(p->owner, "FAXEXTEN", p->owner->exten);
						if (tris_async_goto(p->owner, target_context, "fax", 1)) {
							tris_log(LOG_WARNING, "Failed to async goto '%s' into fax of '%s'\n", p->owner->name, target_context);
						}
					} else {
						tris_log(LOG_NOTICE, "Fax detected, but no fax extension\n");
					}
				} else {
					tris_debug(1, "Already in a fax extension, not redirecting\n");
				}
			} else {
				tris_debug(1, "Fax already handled\n");
			}
		} else if (e->data == VPB_GRUNT) {
			if (tris_tvdiff_ms(tris_tvnow(), p->lastgrunt) > gruntdetect_timeout) {
				/* Nothing heard on line for a very long time
				 * Timeout connection */
				tris_verb(3, "grunt timeout\n");
				tris_log(LOG_NOTICE, "%s: Line hangup due of lack of conversation\n", p->dev); 
				f.subclass = TRIS_CONTROL_HANGUP;
			} else {
				p->lastgrunt = tris_tvnow();
				f.frametype = TRIS_FRAME_NULL;
			}
		} else {
			f.frametype = TRIS_FRAME_NULL;
		}
		break;

	case VPB_CALLEND:
		#ifdef DIAL_WITH_CALL_PROGRESS
		if (e->data == VPB_CALL_CONNECTED) {
			f.subclass = TRIS_CONTROL_ANSWER;
		} else if (e->data == VPB_CALL_NO_DIAL_TONE || e->data == VPB_CALL_NO_RING_BACK) {
			f.subclass =  TRIS_CONTROL_CONGESTION;
		} else if (e->data == VPB_CALL_NO_ANSWER || e->data == VPB_CALL_BUSY) {
			f.subclass = TRIS_CONTROL_BUSY;
		} else if (e->data  == VPB_CALL_DISCONNECTED) {
			f.subclass = TRIS_CONTROL_HANGUP;
		}
		#else
		tris_log(LOG_NOTICE, "%s: Got call progress callback but blind dialing \n", p->dev); 
		f.frametype = TRIS_FRAME_NULL;
		#endif
		break;

	case VPB_STATION_OFFHOOK:
		f.subclass = TRIS_CONTROL_ANSWER;
		break;

	case VPB_DROP:
		if ((p->mode == MODE_FXO) && (UseLoopDrop)) { /* ignore loop drop on stations */
			if (p->owner->_state == TRIS_STATE_UP) {
				f.subclass = TRIS_CONTROL_HANGUP;
			} else {
				f.frametype = TRIS_FRAME_NULL;
			}
		}
		break;
	case VPB_LOOP_ONHOOK:
		if (p->owner->_state == TRIS_STATE_UP) {
			f.subclass = TRIS_CONTROL_HANGUP;
		} else {
			f.frametype = TRIS_FRAME_NULL;
		}
		break;
	case VPB_STATION_ONHOOK:
		f.subclass = TRIS_CONTROL_HANGUP;
		break;

	case VPB_STATION_FLASH:
		f.subclass = TRIS_CONTROL_FLASH;
		break;

	/* Called when dialing has finished and ringing starts
	 * No indication that call has really been answered when using blind dialing
	 */
	case VPB_DIALEND:
		if (p->state < 5) {
			f.subclass = TRIS_CONTROL_ANSWER;
			tris_verb(2, "%s: Dialend\n", p->dev);
		} else {
			f.frametype = TRIS_FRAME_NULL;
		}
		break;

/*	case VPB_PLAY_UNDERFLOW:
		f.frametype = TRIS_FRAME_NULL;
		vpb_reset_play_fifo_alarm(p->handle);
		break;

	case VPB_RECORD_OVERFLOW:
		f.frametype = TRIS_FRAME_NULL;
		vpb_reset_record_fifo_alarm(p->handle);
		break;
*/
	default:
		f.frametype = TRIS_FRAME_NULL;
		break;
	}

/*
	tris_verb(4, "%s: LOCKING in handle_owned [%d]\n", p->dev,res);
	res = tris_mutex_lock(&p->lock); 
	tris_verb(4, "%s: LOCKING count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
*/
	if (p->bridge) { /* Check what happened, see if we need to report it. */
		switch (f.frametype) {
		case TRIS_FRAME_DTMF:
			if (	!(p->bridge->c0 == p->owner && 
					(p->bridge->flags & TRIS_BRIDGE_DTMF_CHANNEL_0) ) &&
					!(p->bridge->c1 == p->owner && 
					(p->bridge->flags & TRIS_BRIDGE_DTMF_CHANNEL_1) )) {
				/* Kill bridge, this is interesting. */
				endbridge = 1;
			}
			break;

		case TRIS_FRAME_CONTROL:
			if (!(p->bridge->flags & TRIS_BRIDGE_IGNORE_SIGS)) {
			#if 0
			if (f.subclass == TRIS_CONTROL_BUSY ||
			f.subclass == TRIS_CONTROL_CONGESTION ||
			f.subclass == TRIS_CONTROL_HANGUP ||
			f.subclass == TRIS_CONTROL_FLASH)
			#endif
				endbridge = 1;
			}
			break;

		default:
			break;
		}

		if (endbridge) {
			if (p->bridge->fo) {
				*p->bridge->fo = tris_frisolate(&f);
			}

			if (p->bridge->rc) {
				*p->bridge->rc = p->owner;
			}

			tris_mutex_lock(&p->bridge->lock);
			p->bridge->endbridge = 1;
			tris_cond_signal(&p->bridge->cond);
			tris_mutex_unlock(&p->bridge->lock); 	       		   
		}	  
	}

	if (endbridge) {
		res = tris_mutex_unlock(&p->lock);
/*
		tris_verb(4, "%s: unLOCKING in handle_owned [%d]\n", p->dev,res);
*/
		return 0;
	}

	tris_verb(4, "%s: handle_owned: Prepared frame type[%d]subclass[%d], bridge=%p owner=[%s]\n",
			p->dev, f.frametype, f.subclass, (void *)p->bridge, p->owner->name);

	/* Trylock used here to avoid deadlock that can occur if we
	 * happen to be in here handling an event when hangup is called
	 * Problem is that hangup holds p->owner->lock
	 */
	if ((f.frametype >= 0) && (f.frametype != TRIS_FRAME_NULL) && (p->owner)) {
		if (tris_channel_trylock(p->owner) == 0) {
			tris_queue_frame(p->owner, &f);
			tris_channel_unlock(p->owner);
			tris_verb(4, "%s: handled_owned: Queued Frame to [%s]\n", p->dev, p->owner->name);
		} else {
			tris_verbose("%s: handled_owned: Missed event %d/%d \n",
				p->dev, f.frametype, f.subclass);
		}
	}
	res = tris_mutex_unlock(&p->lock);
/*
	tris_verb(4, "%s: unLOCKING in handle_owned [%d]\n", p->dev,res);
*/

	return 0;
}

static inline int monitor_handle_notowned(struct vpb_pvt *p, VPB_EVENT *e)
{
	char s[2] = {0};
	struct tris_channel *owner = p->owner;
	char cid_num[256];
	char cid_name[256];
/*
	struct tris_channel *c;
*/

	char str[VPB_MAX_STR];

	vpb_translate_event(e, str);
	tris_verb(4, "%s: handle_notowned: mode=%d, event[%d][%s]=[%d]\n", p->dev, p->mode, e->type,str, e->data);

	switch (e->type) {
	case VPB_LOOP_ONHOOK:
	case VPB_LOOP_POLARITY:
		if (UsePolarityCID == 1) {
			tris_verb(4, "Polarity reversal\n");
			if (p->callerid_type == 1) {
				tris_verb(4, "Using VPB Caller ID\n");
				get_callerid(p);        /* UK CID before 1st ring*/
			}
/*			get_callerid_ast(p); */   /* Caller ID using the ast functions */
		}
		break;
	case VPB_RING:
		if (p->mode == MODE_FXO) /* FXO port ring, start * */ {
			vpb_new(p, TRIS_STATE_RING, p->context);
			if (UsePolarityCID != 1) {
				if (p->callerid_type == 1) {
					tris_verb(4, "Using VPB Caller ID\n");
					get_callerid(p);        /* Australian CID only between 1st and 2nd ring  */
				}
				get_callerid_ast(p);    /* Caller ID using the ast functions */
			} else {
				tris_log(LOG_ERROR, "Setting caller ID: %s %s\n", p->cid_num, p->cid_name);
				tris_set_callerid(p->owner, p->cid_num, p->cid_name, p->cid_num);
				p->cid_num[0] = 0;
				p->cid_name[0] = 0;
			}

			vpb_timer_stop(p->ring_timer);
			vpb_timer_start(p->ring_timer);
		}
		break;

	case VPB_RING_OFF:
		break;

	case VPB_STATION_OFFHOOK:
		if (p->mode == MODE_IMMEDIATE) {
			vpb_new(p,TRIS_STATE_RING, p->context);
		} else {
			tris_verb(4, "%s: handle_notowned: playing dialtone\n", p->dev);
			playtone(p->handle, &Dialtone);
			p->state = VPB_STATE_PLAYDIAL;
			p->wantdtmf = 1;
			p->ext[0] = 0;	/* Just to be sure & paranoid.*/
		}
		break;

	case VPB_DIALEND:
		if (p->mode == MODE_DIALTONE) {
			if (p->state == VPB_STATE_PLAYDIAL) {
				playtone(p->handle, &Dialtone);
				p->wantdtmf = 1;
				p->ext[0] = 0;	/* Just to be sure & paranoid. */
			}
#if 0
			/* These are not needed as they have timers to restart them */
			else if (p->state == VPB_STATE_PLAYBUSY) {
				playtone(p->handle, &Busytone);
				p->wantdtmf = 1;
				p->ext[0] = 0;	
			} else if (p->state == VPB_STATE_PLAYRING) {
				playtone(p->handle, &Ringbacktone);
				p->wantdtmf = 1;
				p->ext[0] = 0;
			}
#endif
		} else {
			tris_verb(4, "%s: handle_notowned: Got a DIALEND when not really expected\n",p->dev);
		}
		break;

	case VPB_STATION_ONHOOK:	/* clear ext */
		stoptone(p->handle);
		p->wantdtmf = 1 ;
		p->ext[0] = 0;
		p->state = VPB_STATE_ONHOOK;
		break;
	case VPB_TIMEREXP:
		if (e->data == p->dtmfidd_timer_id) {
			if (tris_exists_extension(NULL, p->context, p->ext, 1, p->callerid)){
				tris_verb(4, "%s: handle_notowned: DTMF IDD timer out, matching on [%s] in [%s]\n", p->dev, p->ext, p->context);

				vpb_new(p, TRIS_STATE_RING, p->context);
			}
		} else if (e->data == p->ring_timer_id) {
			/* We didnt get another ring in time! */
			if (p->owner) {
				if (p->owner->_state != TRIS_STATE_UP) {
					 /* Assume caller has hung up */
					vpb_timer_stop(p->ring_timer);
				}
			} else {
				 /* No owner any more, Assume caller has hung up */
				vpb_timer_stop(p->ring_timer);
			}
		} 
		break;

	case VPB_DTMF:
		if (p->state == VPB_STATE_ONHOOK){
			/* DTMF's being passed while on-hook maybe Caller ID */
			if (p->mode == MODE_FXO) {
				if (e->data == DTMF_CID_START) { /* CallerID Start signal */
					p->dtmf_caller_pos = 0; /* Leaves the first digit out */
					memset(p->callerid, 0, sizeof(p->callerid));
				} else if (e->data == DTMF_CID_STOP) { /* CallerID End signal */
					p->callerid[p->dtmf_caller_pos] = '\0';
					tris_verb(3, " %s: DTMF CallerID %s\n", p->dev, p->callerid);
					if (owner) {
						/*
						if (owner->cid.cid_num)
							tris_free(owner->cid.cid_num);
						owner->cid.cid_num=NULL;
						if (owner->cid.cid_name)
							tris_free(owner->cid.cid_name);
						owner->cid.cid_name=NULL;
						owner->cid.cid_num = strdup(p->callerid);
						*/
						cid_name[0] = '\0';
						cid_num[0] = '\0';
						tris_callerid_split(p->callerid, cid_name, sizeof(cid_name), cid_num, sizeof(cid_num));
						tris_set_callerid(owner, cid_num, cid_name, cid_num);

					} else {
						tris_verb(3, " %s: DTMF CallerID: no owner to assign CID \n", p->dev);
					}
				} else if (p->dtmf_caller_pos < TRIS_MAX_EXTENSION) {
					if (p->dtmf_caller_pos >= 0) {
						p->callerid[p->dtmf_caller_pos] = e->data;
					}
					p->dtmf_caller_pos++;
				}
			}
			break;
		}
		if (p->wantdtmf == 1) {
			stoptone(p->handle);
			p->wantdtmf = 0;
		}
		p->state = VPB_STATE_GETDTMF;
		s[0] = e->data;
		strncat(p->ext, s, sizeof(p->ext) - strlen(p->ext) - 1);
		#if 0
		if (!strcmp(p->ext, tris_pickup_ext())) {
			/* Call pickup has been dialled! */
			if (tris_pickup_call(c)) {
				/* Call pickup wasnt possible */
			}
		} else 
		#endif
		if (tris_exists_extension(NULL, p->context, p->ext, 1, p->callerid)) {
			if (tris_canmatch_extension(NULL, p->context, p->ext, 1, p->callerid)) {
				tris_verb(4, "%s: handle_notowned: Multiple matches on [%s] in [%s]\n", p->dev, p->ext, p->context);
				/* Start DTMF IDD timer */
				vpb_timer_stop(p->dtmfidd_timer);
				vpb_timer_start(p->dtmfidd_timer);
			} else {
				tris_verb(4, "%s: handle_notowned: Matched on [%s] in [%s]\n", p->dev, p->ext , p->context);
				vpb_new(p, TRIS_STATE_UP, p->context);
			}
		} else if (!tris_canmatch_extension(NULL, p->context, p->ext, 1, p->callerid)) {
			if (tris_exists_extension(NULL, "default", p->ext, 1, p->callerid)) {
				vpb_new(p, TRIS_STATE_UP, "default");
			} else if (!tris_canmatch_extension(NULL, "default", p->ext, 1, p->callerid)) {
				tris_verb(4, "%s: handle_notowned: can't match anything in %s or default\n", p->dev, p->context);
				playtone(p->handle, &Busytone);
				vpb_timer_stop(p->busy_timer);
				vpb_timer_start(p->busy_timer);
				p->state = VPB_STATE_PLAYBUSY;
			}
		}
		break;

	default:
		/* Ignore.*/
		break;
	}

	tris_verb(4, "%s: handle_notowned: mode=%d, [%d=>%d]\n", p->dev, p->mode, e->type, e->data);

	return 0;
}

static void *do_monitor(void *unused)
{

	/* Monitor thread, doesn't die until explicitly killed. */

	tris_verb(2, "Starting vpb monitor thread[%ld]\n", pthread_self());

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	for (;;) {
		VPB_EVENT e;
		VPB_EVENT je;
		char str[VPB_MAX_STR];
		struct vpb_pvt *p;

		/*
		tris_verb(4, "Monitor waiting for event\n");
		*/

		int res = vpb_get_event_sync(&e, VPB_WAIT_TIMEOUT);
		if ((res == VPB_NO_EVENTS) || (res == VPB_TIME_OUT)) {
			/*
			if (res == VPB_NO_EVENTS) {
				tris_verb(4, "No events....\n");
			} else {
				tris_verb(4, "No events, timed out....\n");
			}
			*/
			continue;
		}

		if (res != VPB_OK) {
			tris_log(LOG_ERROR,"Monitor get event error %d\n", res );
			tris_verbose("Monitor get event error %d\n", res );
			continue;
		}

		str[0] = 0;

		p = NULL;

		tris_mutex_lock(&monlock);
		if (e.type == VPB_NULL_EVENT) {
			tris_verb(4, "Monitor got null event\n");
		} else {
			vpb_translate_event(&e, str);
			if (*str && *(str + 1)) {
				str[strlen(str) - 1] = '\0';
			}

			tris_mutex_lock(&iflock);
			for (p = iflist; p && p->handle != e.handle; p = p->next);
			tris_mutex_unlock(&iflock);

			if (p) {
				tris_verb(4, "%s: Event [%d=>%s]\n",
					p ? p->dev : "null", e.type, str);
			}
		}

		tris_mutex_unlock(&monlock); 

		if (!p) {
			if (e.type != VPB_NULL_EVENT) {
				tris_log(LOG_WARNING, "Got event [%s][%d], no matching iface!\n", str, e.type);    
				tris_verb(4, "vpb/ERR: No interface for Event [%d=>%s] \n", e.type, str);
			}
			continue;
		} 

		/* flush the event from the channel event Q */
		vpb_get_event_ch_async(e.handle, &je);
		vpb_translate_event(&je, str);
		tris_verb(5, "%s: Flushing event [%d]=>%s\n", p->dev, je.type, str);

		/* Check for ownership and locks */
		if ((p->owner) && (!p->golock)) {
			/* Need to get owner lock */
			/* Safely grab both p->lock and p->owner->lock so that there
			cannot be a race with something from the other side */
			/*
			tris_mutex_lock(&p->lock);
			while (tris_mutex_trylock(&p->owner->lock)) {
				tris_mutex_unlock(&p->lock);
				usleep(1);
				tris_mutex_lock(&p->lock);
				if (!p->owner)
					break;
			}
			if (p->owner)
				p->golock = 1;
			*/
		}
		/* Two scenarios: Are you owned or not. */
		if (p->owner) {
			monitor_handle_owned(p, &e);
		} else {
			monitor_handle_notowned(p, &e);
		}
		/* if ((!p->owner)&&(p->golock)) {
			tris_mutex_unlock(&p->owner->lock);
			tris_mutex_unlock(&p->lock);
		}
		*/

	}

	return NULL;
}

static int restart_monitor(void)
{
	int error = 0;

	/* If we're supposed to be stopped -- stay stopped */
	if (mthreadactive == -2)
		return 0;

	tris_verb(4, "Restarting monitor\n");

	tris_mutex_lock(&monlock);
	if (monitor_thread == pthread_self()) {
		tris_log(LOG_WARNING, "Cannot kill myself\n");
		error = -1;
		tris_verb(4, "Monitor trying to kill monitor\n");
	} else {
		if (mthreadactive != -1) {
			/* Why do other drivers kill the thread? No need says I, simply awake thread with event. */
			VPB_EVENT e;
			e.handle = 0;
			e.type = VPB_EVT_NONE;
			e.data = 0;

			tris_verb(4, "Trying to reawake monitor\n");

			vpb_put_event(&e);
		} else {
			/* Start a new monitor */
			int pid = tris_pthread_create(&monitor_thread, NULL, do_monitor, NULL); 
			tris_verb(4, "Created new monitor thread %d\n", pid);
			if (pid < 0) {
				tris_log(LOG_ERROR, "Unable to start monitor thread.\n");
				error = -1;
			} else {
				mthreadactive = 0; /* Started the thread!*/
			}
		}
	}
	tris_mutex_unlock(&monlock);

	tris_verb(4, "Monitor restarted\n");

	return error;
}

/* Per board config that must be called after vpb_open() */
static void mkbrd(vpb_model_t model, int echo_cancel)
{
	if (!bridges) {
		if (model == vpb_model_v4pci) {
			max_bridges = MAX_BRIDGES_V4PCI;
		}
		bridges = (vpb_bridge_t *)tris_calloc(1, max_bridges * sizeof(vpb_bridge_t));
		if (!bridges) {
			tris_log(LOG_ERROR, "Failed to initialize bridges\n");
		} else {
			int i;
			for (i = 0; i < max_bridges; i++) {
				tris_mutex_init(&bridges[i].lock);
				tris_cond_init(&bridges[i].cond, NULL);
			}
		}
	}
	if (!echo_cancel) {
		if (model == vpb_model_v4pci) {
			vpb_echo_canc_disable();
			tris_log(LOG_NOTICE, "Voicetronix echo cancellation OFF\n");
		} else {
			/* need to do it port by port for OpenSwitch */
		}
	} else {
		if (model == vpb_model_v4pci) {
			vpb_echo_canc_enable();
			tris_log(LOG_NOTICE, "Voicetronix echo cancellation ON\n");
			if (ec_supp_threshold > -1) {
				vpb_echo_canc_set_sup_thresh(0, &ec_supp_threshold);
				tris_log(LOG_NOTICE, "Voicetronix EC Sup Thres set\n");
			}
		} else {
			/* need to do it port by port for OpenSwitch */
		}
	}
}

static struct vpb_pvt *mkif(int board, int channel, int mode, int gains, float txgain, float rxgain,
			 float txswgain, float rxswgain, int bal1, int bal2, int bal3,
			 char * callerid, int echo_cancel, int group, tris_group_t callgroup, tris_group_t pickupgroup )
{
	struct vpb_pvt *tmp;
	char buf[64];

	tmp = (vpb_pvt *)tris_calloc(1, sizeof(*tmp));

	if (!tmp)
		return NULL;

	tmp->handle = vpb_open(board, channel);

	if (tmp->handle < 0) {	  
		tris_log(LOG_WARNING, "Unable to create channel vpb/%d-%d: %s\n", 
					board, channel, strerror(errno));
		tris_free(tmp);
		return NULL;
	}
	       
	snprintf(tmp->dev, sizeof(tmp->dev), "vpb/%d-%d", board, channel);

	tmp->mode = mode;

	tmp->group = group;
	tmp->callgroup = callgroup;
	tmp->pickupgroup = pickupgroup;

	/* Initialize dtmf caller ID position variable */
	tmp->dtmf_caller_pos = 0;

	tris_copy_string(tmp->language, language, sizeof(tmp->language));
	tris_copy_string(tmp->context, context, sizeof(tmp->context));

	tmp->callerid_type = 0;
	if (callerid) { 
		if (strcasecmp(callerid, "on") == 0) {
			tmp->callerid_type = 1;
			tris_copy_string(tmp->callerid, "unknown", sizeof(tmp->callerid));
		} else if (strcasecmp(callerid, "v23") == 0) {
			tmp->callerid_type = 2;
			tris_copy_string(tmp->callerid, "unknown", sizeof(tmp->callerid));
		} else if (strcasecmp(callerid, "bell") == 0) {
			tmp->callerid_type = 3;
			tris_copy_string(tmp->callerid, "unknown", sizeof(tmp->callerid));
		} else {
			tris_copy_string(tmp->callerid, callerid, sizeof(tmp->callerid));
		}
	} else {
		tris_copy_string(tmp->callerid, "unknown", sizeof(tmp->callerid));
	}

	/* check if codec balances have been set in the config file */
	if (bal3 >= 0) {
		if ((bal1>=0) && !(bal1 & 32)) bal1 |= 32;
			vpb_set_codec_reg(tmp->handle, 0x42, bal3);
	}
	if (bal1 >= 0) {
		vpb_set_codec_reg(tmp->handle, 0x32, bal1);
	}
	if (bal2 >= 0) {
		vpb_set_codec_reg(tmp->handle, 0x3a, bal2);
	}

	if (gains & VPB_GOT_TXHWG) {
		if (txgain > MAX_VPB_GAIN) {
			tmp->txgain = MAX_VPB_GAIN;
		} else if (txgain < MIN_VPB_GAIN) {
			tmp->txgain = MIN_VPB_GAIN;
		} else {
			tmp->txgain = txgain;
		}
		
		tris_log(LOG_NOTICE, "VPB setting Tx Hw gain to [%f]\n", tmp->txgain);
		vpb_play_set_hw_gain(tmp->handle, tmp->txgain);
	}

	if (gains & VPB_GOT_RXHWG) {
		if (rxgain > MAX_VPB_GAIN) {
			tmp->rxgain = MAX_VPB_GAIN;
		} else if (rxgain < MIN_VPB_GAIN) {
			tmp->rxgain = MIN_VPB_GAIN;
		} else {
			tmp->rxgain = rxgain;
		}
		tris_log(LOG_NOTICE, "VPB setting Rx Hw gain to [%f]\n", tmp->rxgain);
		vpb_record_set_hw_gain(tmp->handle, tmp->rxgain);
	}

	if (gains & VPB_GOT_TXSWG) {
		tmp->txswgain = txswgain;
		tris_log(LOG_NOTICE, "VPB setting Tx Sw gain to [%f]\n", tmp->txswgain);
		vpb_play_set_gain(tmp->handle, tmp->txswgain);
	}

	if (gains & VPB_GOT_RXSWG) {
		tmp->rxswgain = rxswgain;
		tris_log(LOG_NOTICE, "VPB setting Rx Sw gain to [%f]\n", tmp->rxswgain);
		vpb_record_set_gain(tmp->handle, tmp->rxswgain);
	}

	tmp->vpb_model = vpb_model_unknown;
	if (vpb_get_model(tmp->handle, buf) == VPB_OK) {
		if (strcmp(buf, "V12PCI") == 0) {
			tmp->vpb_model = vpb_model_v12pci;
		} else if (strcmp(buf, "VPB4") == 0) {
			tmp->vpb_model = vpb_model_v4pci;
		}
	}

	tris_mutex_init(&tmp->owner_lock);
	tris_mutex_init(&tmp->lock);
	tris_mutex_init(&tmp->record_lock);
	tris_mutex_init(&tmp->play_lock);
	tris_mutex_init(&tmp->play_dtmf_lock);

	/* set default read state */
	tmp->read_state = 0;
	
	tmp->golock = 0;

	tmp->busy_timer_id = vpb_timer_get_unique_timer_id();
	vpb_timer_open(&tmp->busy_timer, tmp->handle, tmp->busy_timer_id, TIMER_PERIOD_BUSY);

	tmp->ringback_timer_id = vpb_timer_get_unique_timer_id();
	vpb_timer_open(&tmp->ringback_timer, tmp->handle, tmp->ringback_timer_id, TIMER_PERIOD_RINGBACK);

	tmp->ring_timer_id = vpb_timer_get_unique_timer_id();
	vpb_timer_open(&tmp->ring_timer, tmp->handle, tmp->ring_timer_id, timer_period_ring);
	      
	tmp->dtmfidd_timer_id = vpb_timer_get_unique_timer_id();
	vpb_timer_open(&tmp->dtmfidd_timer, tmp->handle, tmp->dtmfidd_timer_id, dtmf_idd);
	      
	if (mode == MODE_FXO){
		if (use_tris_dtmfdet)
			vpb_set_event_mask(tmp->handle, VPB_EVENTS_NODTMF);
		else
			vpb_set_event_mask(tmp->handle, VPB_EVENTS_ALL);
	} else {
/*
		if (use_tris_dtmfdet)
			vpb_set_event_mask(tmp->handle, VPB_EVENTS_NODTMF);
		else
*/
			vpb_set_event_mask(tmp->handle, VPB_EVENTS_STAT);
	}

	if ((tmp->vpb_model == vpb_model_v12pci) && (echo_cancel)) {
		vpb_hostecho_on(tmp->handle);
	}
	if (use_tris_dtmfdet) {
		tmp->vad = tris_dsp_new();
		tris_dsp_set_features(tmp->vad, DSP_FEATURE_DIGIT_DETECT);
		tris_dsp_set_digitmode(tmp->vad, DSP_DIGITMODE_DTMF);
		if (relaxdtmf)
			tris_dsp_set_digitmode(tmp->vad, DSP_DIGITMODE_DTMF|DSP_DIGITMODE_RELAXDTMF);
	} else {
		tmp->vad = NULL;
	}

	/* define grunt tone */
	vpb_settonedet(tmp->handle,&toned_ungrunt);

	tris_log(LOG_NOTICE,"Voicetronix %s channel %s initialized (rxsg=%f/txsg=%f/rxhg=%f/txhg=%f)(0x%x/0x%x/0x%x)\n",
		(tmp->vpb_model == vpb_model_v4pci) ? "V4PCI" :
		(tmp->vpb_model == vpb_model_v12pci) ? "V12PCI" : "[Unknown model]",
		tmp->dev, tmp->rxswgain, tmp->txswgain, tmp->rxgain, tmp->txgain, bal1, bal2, bal3);

	return tmp;
}

static int vpb_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen)
{
	struct vpb_pvt *p = (struct vpb_pvt *)ast->tech_pvt;
	int res = 0;
	int tmp = 0;

	if (use_tris_ind == 1) {
		tris_verb(4, "%s: vpb_indicate called when using Ast Indications !?!\n", p->dev);
		return 0;
	}

	tris_verb(4, "%s: vpb_indicate [%d] state[%d]\n", p->dev, condition,ast->_state);
/*
	if (ast->_state != TRIS_STATE_UP) {
		tris_verb(4, "%s: vpb_indicate Not in TRIS_STATE_UP\n", p->dev, condition,ast->_state);
		return res;
	}
*/

/*
	tris_verb(4, "%s: LOCKING in indicate \n", p->dev);
	tris_verb(4, "%s: LOCKING count[%d] owner[%d] \n", p->dev, p->lock.__m_count, p->lock.__m_owner);
*/
	tris_mutex_lock(&p->lock);
	switch (condition) {
	case TRIS_CONTROL_BUSY:
	case TRIS_CONTROL_CONGESTION:
		if (ast->_state == TRIS_STATE_UP) {
			playtone(p->handle, &Busytone);
			p->state = VPB_STATE_PLAYBUSY;
			vpb_timer_stop(p->busy_timer); 
			vpb_timer_start(p->busy_timer); 
		}
		break;
	case TRIS_CONTROL_RINGING:
		if (ast->_state == TRIS_STATE_UP) {
			playtone(p->handle, &Ringbacktone);
			p->state = VPB_STATE_PLAYRING;
			tris_verb(4, "%s: vpb indicate: setting ringback timer [%d]\n", p->dev,p->ringback_timer_id);

			vpb_timer_stop(p->ringback_timer);
			vpb_timer_start(p->ringback_timer);
		}
		break;	    
	case TRIS_CONTROL_ANSWER:
	case -1: /* -1 means stop playing? */
		vpb_timer_stop(p->ringback_timer);
		vpb_timer_stop(p->busy_timer);
		stoptone(p->handle);
		break;
	case TRIS_CONTROL_HANGUP:
		if (ast->_state == TRIS_STATE_UP) {
			playtone(p->handle, &Busytone);
			p->state = VPB_STATE_PLAYBUSY;
			vpb_timer_stop(p->busy_timer);
			vpb_timer_start(p->busy_timer);
		}
		break;
	case TRIS_CONTROL_HOLD:
		tris_moh_start(ast, (const char *) data, NULL);
		break;
	case TRIS_CONTROL_UNHOLD:
		tris_moh_stop(ast);
		break;
	default:
		res = 0;
		break;
	}
	tmp = tris_mutex_unlock(&p->lock);
/*
	tris_verb(4, "%s: unLOCKING in indicate [%d]\n", p->dev,tmp);
*/
	return res;
}

static int vpb_fixup(struct tris_channel *oldchan, struct tris_channel *newchan)
{
	struct vpb_pvt *p = (struct vpb_pvt *)newchan->tech_pvt;
	int res = 0;

/*
	tris_verb(4, "%s: LOCKING in fixup \n", p->dev);
	tris_verb(4, "%s: LOCKING count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
*/
	tris_mutex_lock(&p->lock);
	tris_debug(1, "New owner for channel %s is %s\n", p->dev, newchan->name);

	if (p->owner == oldchan) {
		p->owner = newchan;
	}

	if (newchan->_state == TRIS_STATE_RINGING){
		if (use_tris_ind == 1) {
			tris_verb(4, "%s: vpb_fixup Calling tris_indicate\n", p->dev);
			tris_indicate(newchan, TRIS_CONTROL_RINGING);
		} else {
			tris_verb(4, "%s: vpb_fixup Calling vpb_indicate\n", p->dev);
			vpb_indicate(newchan, TRIS_CONTROL_RINGING, NULL, 0);
		}
	}

	res = tris_mutex_unlock(&p->lock);
/*
	tris_verb(4, "%s: unLOCKING in fixup [%d]\n", p->dev,res);
*/
	return 0;
}

static int vpb_digit_begin(struct tris_channel *ast, char digit)
{
	/* XXX Modify this callback to let Trismedia control the length of DTMF */
	return 0;
}
static int vpb_digit_end(struct tris_channel *ast, char digit, unsigned int duration)
{
	struct vpb_pvt *p = (struct vpb_pvt *)ast->tech_pvt;
	char s[2];
	int res = 0;

	if (use_tris_dtmf) {
		tris_verb(4, "%s: vpb_digit: asked to play digit[%c] but we are using trismedia dtmf play back?!\n", p->dev, digit);
		return 0;
	}

/*
	tris_verb(4, "%s: LOCKING in digit \n", p->dev);
	tris_verb(4, "%s: LOCKING count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
*/
	tris_mutex_lock(&p->lock);


	s[0] = digit;
	s[1] = '\0';

	tris_verb(4, "%s: vpb_digit: asked to play digit[%s]\n", p->dev, s);

	tris_mutex_lock(&p->play_dtmf_lock);
	strncat(p->play_dtmf, s, sizeof(*p->play_dtmf) - strlen(p->play_dtmf) - 1);
	tris_mutex_unlock(&p->play_dtmf_lock);

	res = tris_mutex_unlock(&p->lock);
/*
	tris_verb(4, "%s: unLOCKING in digit [%d]\n", p->dev,res);
*/
	return 0;
}

/* Places a call out of a VPB channel */
static int vpb_call(struct tris_channel *ast, char *dest, int timeout)
{
	struct vpb_pvt *p = (struct vpb_pvt *)ast->tech_pvt;
	int res = 0, i;
	char *s = strrchr(dest, '/');
	char dialstring[254] = "";
	int tmp = 0;

/*
	tris_verb(4, "%s: LOCKING in call \n", p->dev);
	tris_verb(4, "%s: LOCKING count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
*/
	tris_mutex_lock(&p->lock);
	tris_verb(4, "%s: starting call to [%s]\n", p->dev, dest);

	if (s)
		s = s + 1;
	else
		s = dest;
	tris_copy_string(dialstring, s, sizeof(dialstring));
	for (i = 0; dialstring[i] != '\0'; i++) {
		if ((dialstring[i] == 'w') || (dialstring[i] == 'W'))
			dialstring[i] = ',';
		else if ((dialstring[i] == 'f') || (dialstring[i] == 'F'))
			dialstring[i] = '&';
	}

	if (ast->_state != TRIS_STATE_DOWN && ast->_state != TRIS_STATE_RESERVED) {
		tris_log(LOG_WARNING, "vpb_call on %s neither down nor reserved!\n", ast->name);
		tmp = tris_mutex_unlock(&p->lock);
/*
		tris_verb(4, "%s: unLOCKING in call [%d]\n", p->dev,tmp);
*/
		return -1;
	}
	if (p->mode != MODE_FXO)  /* Station port, ring it. */
		vpb_ring_station_async(p->handle, 2);
	else {
		VPB_CALL call;
		int j;

		/* Dial must timeout or it can leave channels unuseable */
		if (timeout == 0) {
			timeout = TIMER_PERIOD_NOANSWER;
		} else {
			timeout = timeout * 1000; /* convert from secs to ms. */
		}

		/* These timeouts are only used with call progress dialing */
		call.dialtones = 1; /* Number of dialtones to get outside line */
		call.dialtone_timeout = VPB_DIALTONE_WAIT; /* Wait this long for dialtone (ms) */
		call.ringback_timeout = VPB_RINGWAIT; /* Wait this long for ringing after dialing (ms) */
		call.inter_ringback_timeout = VPB_CONNECTED_WAIT; /* If ringing stops for this long consider it connected (ms) */
		call.answer_timeout = timeout; /* Time to wait for answer after ringing starts (ms) */
		memcpy(&call.tone_map,  DialToneMap, sizeof(DialToneMap));
		vpb_set_call(p->handle, &call);

		tris_verb(2, "%s: Calling %s on %s \n",p->dev, dialstring, ast->name);

		tris_verb(2, "%s: Dial parms for %s %d/%dms/%dms/%dms/%dms\n", p->dev,
				ast->name, call.dialtones, call.dialtone_timeout,
				call.ringback_timeout, call.inter_ringback_timeout,
				call.answer_timeout);
		for (j = 0; !call.tone_map[j].terminate; j++) {
			tris_verb(2, "%s: Dial parms for %s tone %d->%d\n", p->dev,
					ast->name, call.tone_map[j].tone_id, call.tone_map[j].call_id); 
		}

		tris_verb(4, "%s: Disabling Loop Drop detection\n", p->dev);
		vpb_disable_event(p->handle, VPB_MDROP);
		vpb_sethook_sync(p->handle, VPB_OFFHOOK);
		p->state = VPB_STATE_OFFHOOK;

		#ifndef DIAL_WITH_CALL_PROGRESS
		vpb_sleep(300);
		tris_verb(4, "%s: Enabling Loop Drop detection\n", p->dev);
		vpb_enable_event(p->handle, VPB_MDROP);
		res = vpb_dial_async(p->handle, dialstring);
		#else
		tris_verb(4, "%s: Enabling Loop Drop detection\n", p->dev);
		vpb_enable_event(p->handle, VPB_MDROP);
		res = vpb_call_async(p->handle, dialstring);
		#endif

		if (res != VPB_OK) {
			tris_debug(1, "Call on %s to %s failed: %d\n", ast->name, s, res);
			res = -1;
		} else
			res = 0;
	}

	tris_verb(3, "%s: VPB Calling %s [t=%d] on %s returned %d\n", p->dev , s, timeout, ast->name, res);
	if (res == 0) {
		tris_setstate(ast, TRIS_STATE_RINGING);
		tris_queue_control(ast, TRIS_CONTROL_RINGING);
	}

	if (!p->readthread) {
		tris_pthread_create(&p->readthread, NULL, do_chanreads, (void *)p);
	}

	tmp = tris_mutex_unlock(&p->lock);
/*
	tris_verb(4, "%s: unLOCKING in call [%d]\n", p->dev,tmp);
*/
	return res;
}

static int vpb_hangup(struct tris_channel *ast)
{
	struct vpb_pvt *p = (struct vpb_pvt *)ast->tech_pvt;
	VPB_EVENT je;
	char str[VPB_MAX_STR];
	int res = 0;

/*
	tris_verb(4, "%s: LOCKING in hangup \n", p->dev);
	tris_verb(4, "%s: LOCKING in hangup count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
	tris_verb(4, "%s: LOCKING pthread_self(%d)\n", p->dev,pthread_self());
	tris_mutex_lock(&p->lock);
*/
	tris_verb(2, "%s: Hangup requested\n", ast->name);

	if (!ast->tech || !ast->tech_pvt) {
		tris_log(LOG_WARNING, "%s: channel not connected?\n", ast->name);
		res = tris_mutex_unlock(&p->lock);
/*
		tris_verb(4, "%s: unLOCKING in hangup [%d]\n", p->dev,res);
*/
		/* Free up ast dsp if we have one */
		if (use_tris_dtmfdet && p->vad) {
			tris_dsp_free(p->vad);
			p->vad = NULL;
		}
		return 0;
	}



	/* Stop record */
	p->stopreads = 1;
	if (p->readthread) {
		pthread_join(p->readthread, NULL); 
		tris_verb(4, "%s: stopped record thread \n", ast->name);
	}

	/* Stop play */
	if (p->lastoutput != -1) {
		tris_verb(2, "%s: Ending play mode \n", ast->name);
		vpb_play_terminate(p->handle);
		tris_mutex_lock(&p->play_lock);
		vpb_play_buf_finish(p->handle);
		tris_mutex_unlock(&p->play_lock);
	}

	tris_verb(4, "%s: Setting state down\n", ast->name);
	tris_setstate(ast, TRIS_STATE_DOWN);


/*
	tris_verb(4, "%s: LOCKING in hangup \n", p->dev);
	tris_verb(4, "%s: LOCKING in hangup count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
	tris_verb(4, "%s: LOCKING pthread_self(%d)\n", p->dev,pthread_self());
*/
	tris_mutex_lock(&p->lock);

	if (p->mode != MODE_FXO) {
		/* station port. */
		vpb_ring_station_async(p->handle, 0);
		if (p->state != VPB_STATE_ONHOOK) {
			/* This is causing a "dial end" "play tone" loop
			playtone(p->handle, &Busytone); 
			p->state = VPB_STATE_PLAYBUSY;
			tris_verb(5, "%s: Station offhook[%d], playing busy tone\n",
								ast->name,p->state);
			*/
		} else {
			stoptone(p->handle);
		}
		#ifdef VPB_PRI
		vpb_setloop_async(p->handle, VPB_OFFHOOK);
		vpb_sleep(100);
		vpb_setloop_async(p->handle, VPB_ONHOOK);
		#endif
	} else {
		stoptone(p->handle); /* Terminates any dialing */
		vpb_sethook_sync(p->handle, VPB_ONHOOK);
		p->state=VPB_STATE_ONHOOK;
	}
	while (VPB_OK == vpb_get_event_ch_async(p->handle, &je)) {
		vpb_translate_event(&je, str);
		tris_verb(4, "%s: Flushing event [%d]=>%s\n", ast->name, je.type, str);
	}

	p->readthread = 0;
	p->lastoutput = -1;
	p->lastinput = -1;
	p->last_ignore_dtmf = 1;
	p->ext[0] = 0;
	p->dialtone = 0;

	p->owner = NULL;
	ast->tech_pvt = NULL;

	/* Free up ast dsp if we have one */
	if (use_tris_dtmfdet && p->vad) {
		tris_dsp_free(p->vad);
		p->vad = NULL;
	}

	tris_verb(2, "%s: Hangup complete\n", ast->name);

	restart_monitor();
/*
	tris_verb(4, "%s: LOCKING in hangup count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
*/
	res = tris_mutex_unlock(&p->lock);
/*
	tris_verb(4, "%s: unLOCKING in hangup [%d]\n", p->dev,res);
	tris_verb(4, "%s: LOCKING in hangup count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
*/
	return 0;
}

static int vpb_answer(struct tris_channel *ast)
{
	struct vpb_pvt *p = (struct vpb_pvt *)ast->tech_pvt;
	int res = 0;
/*
	VPB_EVENT je;
	int ret;
	tris_verb(4, "%s: LOCKING in answer \n", p->dev);
	tris_verb(4, "%s: LOCKING count[%d] owner[%d] \n", p->dev, p->lock.__m_count,p->lock.__m_owner);
*/
	tris_mutex_lock(&p->lock);

	tris_verb(4, "%s: Answering channel\n", p->dev);

	if (p->mode == MODE_FXO) {
		tris_verb(4, "%s: Disabling Loop Drop detection\n", p->dev);
		vpb_disable_event(p->handle, VPB_MDROP);
	}

	if (ast->_state != TRIS_STATE_UP) {
		if (p->mode == MODE_FXO) {
			vpb_sethook_sync(p->handle, VPB_OFFHOOK);
			p->state = VPB_STATE_OFFHOOK;
/*			vpb_sleep(500);
			ret = vpb_get_event_ch_async(p->handle, &je);
			if ((ret == VPB_OK) && ((je.type != VPB_DROP)&&(je.type != VPB_RING))){
				tris_verb(4, "%s: Answer collected a wrong event!!\n", p->dev);
				vpb_put_event(&je);
			}
*/
		}
		tris_setstate(ast, TRIS_STATE_UP);

		tris_verb(2, "%s: Answered call on %s [%s]\n", p->dev,
					 ast->name, (p->mode == MODE_FXO) ? "FXO" : "FXS");

		ast->rings = 0;
		if (!p->readthread) {
	/*		res = tris_mutex_unlock(&p->lock); */
	/*		tris_verbose("%s: unLOCKING in answer [%d]\n", p->dev,res); */
			tris_pthread_create(&p->readthread, NULL, do_chanreads, (void *)p);
		} else {
			tris_verb(4, "%s: Record thread already running!!\n", p->dev);
		}
	} else {
		tris_verb(4, "%s: Answered state is up\n", p->dev);
	/*	res = tris_mutex_unlock(&p->lock); */
	/*	tris_verbose("%s: unLOCKING in answer [%d]\n", p->dev,res); */
	}
	vpb_sleep(500);
	if (p->mode == MODE_FXO) {
		tris_verb(4, "%s: Re-enabling Loop Drop detection\n", p->dev);
		vpb_enable_event(p->handle, VPB_MDROP);
	}
	res = tris_mutex_unlock(&p->lock);
/*
	tris_verb(4, "%s: unLOCKING in answer [%d]\n", p->dev,res);
*/
	return 0;
}

static struct tris_frame *vpb_read(struct tris_channel *ast)
{
	struct vpb_pvt *p = (struct vpb_pvt *)ast->tech_pvt; 
	static struct tris_frame f = { TRIS_FRAME_NULL }; 

	f.src = "vpb";
	tris_log(LOG_NOTICE, "%s: vpb_read: should never be called!\n", p->dev);
	tris_verbose("%s: vpb_read: should never be called!\n", p->dev);

	return &f;
}

static inline AudioCompress ast2vpbformat(int tris_format)
{
	switch (tris_format) {
	case TRIS_FORMAT_ALAW:
		return VPB_ALAW;
	case TRIS_FORMAT_SLINEAR:
		return VPB_LINEAR;
	case TRIS_FORMAT_ULAW:
		return VPB_MULAW;
	case TRIS_FORMAT_ADPCM:
		return VPB_OKIADPCM;
	default:
		return VPB_RAW;
	}
}

static inline const char * ast2vpbformatname(int tris_format)
{
	switch(tris_format) {
	case TRIS_FORMAT_ALAW:
		return "TRIS_FORMAT_ALAW:VPB_ALAW";
	case TRIS_FORMAT_SLINEAR:
		return "TRIS_FORMAT_SLINEAR:VPB_LINEAR";
	case TRIS_FORMAT_ULAW:
		return "TRIS_FORMAT_ULAW:VPB_MULAW";
	case TRIS_FORMAT_ADPCM:
		return "TRIS_FORMAT_ADPCM:VPB_OKIADPCM";
	default:
		return "UNKN:UNKN";
	}
}

static inline int astformatbits(int tris_format)
{
	switch (tris_format) {
	case TRIS_FORMAT_SLINEAR:
		return 16;
	case TRIS_FORMAT_ADPCM:
		return 4;
	case TRIS_FORMAT_ALAW:
	case TRIS_FORMAT_ULAW:
	default:
		return 8;
	}
}

int a_gain_vector(float g, short *v, int n) 
{
	int i;
	float tmp;
	for (i = 0; i < n; i++) {
		tmp = g * v[i];
		if (tmp > 32767.0)
			tmp = 32767.0;
		if (tmp < -32768.0)
			tmp = -32768.0;
		v[i] = (short)tmp;	
	}  
	return i;
}

/* Writes a frame of voice data to a VPB channel */
static int vpb_write(struct tris_channel *ast, struct tris_frame *frame)
{
	struct vpb_pvt *p = (struct vpb_pvt *)ast->tech_pvt; 
	int res = 0;
	AudioCompress fmt = VPB_RAW;
	struct timeval play_buf_time_start;
	int tdiff;

/*	tris_mutex_lock(&p->lock); */
	tris_verb(6, "%s: vpb_write: Writing to channel\n", p->dev);

	if (frame->frametype != TRIS_FRAME_VOICE) {
		tris_verb(4, "%s: vpb_write: Don't know how to handle from type %d\n", ast->name, frame->frametype);
/*		tris_mutex_unlock(&p->lock); */
		return 0;
	} else if (ast->_state != TRIS_STATE_UP) {
		tris_verb(4, "%s: vpb_write: Attempt to Write frame type[%d]subclass[%d] on not up chan(state[%d])\n",ast->name, frame->frametype, frame->subclass,ast->_state);
		p->lastoutput = -1;
/*		tris_mutex_unlock(&p->lock); */
		return 0;
	}
/*	tris_debug(1, "%s: vpb_write: Checked frame type..\n", p->dev); */


	fmt = ast2vpbformat(frame->subclass);
	if (fmt < 0) {
		tris_log(LOG_WARNING, "%s: vpb_write: Cannot handle frames of %d format!\n", ast->name, frame->subclass);
		return -1;
	}

	tdiff = tris_tvdiff_ms(tris_tvnow(), p->lastplay);
	tris_debug(1, "%s: vpb_write: time since last play(%d) \n", p->dev, tdiff); 
	if (tdiff < (VPB_SAMPLES / 8 - 1)) {
		tris_debug(1, "%s: vpb_write: Asked to play too often (%d) (%d)\n", p->dev, tdiff, frame->datalen); 
/*		return 0; */
	}
	p->lastplay = tris_tvnow();
/*
	tris_debug(1, "%s: vpb_write: Checked frame format..\n", p->dev); 
*/

	tris_mutex_lock(&p->play_lock);

/*
	tris_debug(1, "%s: vpb_write: Got play lock..\n", p->dev); 
*/

	/* Check if we have set up the play_buf */
	if (p->lastoutput == -1) {
		vpb_play_buf_start(p->handle, fmt);
		tris_verb(2, "%s: vpb_write: Starting play mode (codec=%d)[%s]\n", p->dev, fmt, ast2vpbformatname(frame->subclass));
		p->lastoutput = fmt;
		tris_mutex_unlock(&p->play_lock);
		return 0;
	} else if (p->lastoutput != fmt) {
		vpb_play_buf_finish(p->handle);
		vpb_play_buf_start(p->handle, fmt);
		tris_verb(2, "%s: vpb_write: Changed play format (%d=>%d)\n", p->dev, p->lastoutput, fmt);
		tris_mutex_unlock(&p->play_lock);
		return 0;
	}
	p->lastoutput = fmt;



	/* Apply extra gain ! */
	if( p->txswgain > MAX_VPB_GAIN )
		a_gain_vector(p->txswgain - MAX_VPB_GAIN , (short*)frame->data.ptr, frame->datalen / sizeof(short));

/*	tris_debug(1, "%s: vpb_write: Applied gain..\n", p->dev); */
/*	tris_debug(1, "%s: vpb_write: play_buf_time %d\n", p->dev, p->play_buf_time); */

	if ((p->read_state == 1) && (p->play_buf_time < 5)){
		play_buf_time_start = tris_tvnow();
/*		res = vpb_play_buf_sync(p->handle, (char *)frame->data, tdiff * 8 * 2); */
		res = vpb_play_buf_sync(p->handle, (char *)frame->data.ptr, frame->datalen);
		if(res == VPB_OK) {
			short * data = (short*)frame->data.ptr;
			tris_verb(6, "%s: vpb_write: Wrote chan (codec=%d) %d %d\n", p->dev, fmt, data[0], data[1]);
		}
		p->play_buf_time = tris_tvdiff_ms(tris_tvnow(), play_buf_time_start);
	} else {
		p->chuck_count++;
		tris_debug(1, "%s: vpb_write: Tossed data away, tooooo much data!![%d]\n", p->dev, p->chuck_count);
		p->play_buf_time = 0;
	}

	tris_mutex_unlock(&p->play_lock);
/*	tris_mutex_unlock(&p->lock); */
	tris_verb(6, "%s: vpb_write: Done Writing to channel\n", p->dev);
	return 0;
}

/* Read monitor thread function. */
static void *do_chanreads(void *pvt)
{
	struct vpb_pvt *p = (struct vpb_pvt *)pvt;
	struct tris_frame *fr = &p->fr;
	char *readbuf = ((char *)p->buf) + TRIS_FRIENDLY_OFFSET;
	int bridgerec = 0;
	int afmt, readlen, res, trycnt=0;
	AudioCompress fmt;
	int ignore_dtmf;
	const char * getdtmf_var = NULL;

	fr->frametype = TRIS_FRAME_VOICE;
	fr->src = "vpb";
	fr->mallocd = 0;
	fr->delivery.tv_sec = 0;
	fr->delivery.tv_usec = 0;
	fr->samples = VPB_SAMPLES;
	fr->offset = TRIS_FRIENDLY_OFFSET;
	memset(p->buf, 0, sizeof p->buf);

	tris_verb(3, "%s: chanreads: starting thread\n", p->dev);
	tris_mutex_lock(&p->record_lock);

	p->stopreads = 0; 
	p->read_state = 1;
	while (!p->stopreads && p->owner) {

		tris_verb(5, "%s: chanreads: Starting cycle ...\n", p->dev);
		tris_verb(5, "%s: chanreads: Checking bridge \n", p->dev);
		if (p->bridge) {
			if (p->bridge->c0 == p->owner && (p->bridge->flags & TRIS_BRIDGE_REC_CHANNEL_0))
				bridgerec = 1;
			else if (p->bridge->c1 == p->owner && (p->bridge->flags & TRIS_BRIDGE_REC_CHANNEL_1))
				bridgerec = 1;
			else 
				bridgerec = 0;
		} else {
			tris_verb(5, "%s: chanreads: No native bridge.\n", p->dev);
			if (p->owner->_bridge) {
				tris_verb(5, "%s: chanreads: Got Trismedia bridge with [%s].\n", p->dev, p->owner->_bridge->name);
				bridgerec = 1;
			} else {
				bridgerec = 0;
			}
		}

/*		if ((p->owner->_state != TRIS_STATE_UP) || !bridgerec) */
		if ((p->owner->_state != TRIS_STATE_UP)) {
			if (p->owner->_state != TRIS_STATE_UP) {
				tris_verb(5, "%s: chanreads: Im not up[%d]\n", p->dev, p->owner->_state);
			} else {
				tris_verb(5, "%s: chanreads: No bridgerec[%d]\n", p->dev, bridgerec);
			}
			vpb_sleep(10);
			continue;
		}

		/* Voicetronix DTMF detection can be triggered off ordinary speech
		 * This leads to annoying beeps during the conversation
		 * Avoid this problem by just setting VPB_GETDTMF when you want to listen for DTMF
		 */
		/* ignore_dtmf = 1; */
		ignore_dtmf = 0; /* set this to 1 to turn this feature on */
		getdtmf_var = pbx_builtin_getvar_helper(p->owner, "VPB_GETDTMF");
		if (getdtmf_var && strcasecmp(getdtmf_var, "yes") == 0)
			ignore_dtmf = 0;

		if ((ignore_dtmf != p->last_ignore_dtmf) && (!use_tris_dtmfdet)){
			tris_verb(2, "%s:Now %s DTMF \n",
					p->dev, ignore_dtmf ? "ignoring" : "listening for");
			vpb_set_event_mask(p->handle, ignore_dtmf ? VPB_EVENTS_NODTMF : VPB_EVENTS_ALL );
		}
		p->last_ignore_dtmf = ignore_dtmf;

		/* Play DTMF digits here to avoid problem you get if playing a digit during 
		 * a record operation
		 */
		tris_verb(6, "%s: chanreads: Checking dtmf's \n", p->dev);
		tris_mutex_lock(&p->play_dtmf_lock);
		if (p->play_dtmf[0]) {
			/* Try to ignore DTMF event we get after playing digit */
			/* This DTMF is played by trismedia and leads to an annoying trailing beep on CISCO phones */
			if (!ignore_dtmf) {
				vpb_set_event_mask(p->handle, VPB_EVENTS_NODTMF );
			}
			if (p->bridge == NULL) {
				vpb_dial_sync(p->handle, p->play_dtmf);
				tris_verb(2, "%s: chanreads: Played DTMF %s\n", p->dev, p->play_dtmf);
			} else {
				tris_verb(2, "%s: chanreads: Not playing DTMF frame on native bridge\n", p->dev);
			}
			p->play_dtmf[0] = '\0';
			tris_mutex_unlock(&p->play_dtmf_lock);
			vpb_sleep(700); /* Long enough to miss echo and DTMF event */
			if( !ignore_dtmf) 
				vpb_set_event_mask(p->handle, VPB_EVENTS_ALL);
			continue;
		}
		tris_mutex_unlock(&p->play_dtmf_lock);

/*		afmt = (p->owner) ? p->owner->rawreadformat : TRIS_FORMAT_SLINEAR; */
		if (p->owner) {
			afmt = p->owner->rawreadformat;
/*			tris_debug(1,"%s: Record using owner format [%s]\n", p->dev, ast2vpbformatname(afmt)); */
		} else {
			afmt = TRIS_FORMAT_SLINEAR;
/*			tris_debug(1,"%s: Record using default format [%s]\n", p->dev, ast2vpbformatname(afmt)); */
		}
		fmt = ast2vpbformat(afmt);
		if (fmt < 0) {
			tris_log(LOG_WARNING, "%s: Record failure (unsupported format %d)\n", p->dev, afmt);
			return NULL;
		}
		readlen = VPB_SAMPLES * astformatbits(afmt) / 8;

		if (p->lastinput == -1) {
			vpb_record_buf_start(p->handle, fmt);
/*			vpb_reset_record_fifo_alarm(p->handle); */
			p->lastinput = fmt;
			tris_verb(2, "%s: Starting record mode (codec=%d)[%s]\n", p->dev, fmt, ast2vpbformatname(afmt));
			continue;
		} else if (p->lastinput != fmt) {
			vpb_record_buf_finish(p->handle);
			vpb_record_buf_start(p->handle, fmt);
			p->lastinput = fmt;
			tris_verb(2, "%s: Changed record format (%d=>%d)\n", p->dev, p->lastinput, fmt);
			continue;
		}

		/* Read only if up and not bridged, or a bridge for which we can read. */
		tris_verb(6, "%s: chanreads: getting buffer!\n", p->dev);
		if( (res = vpb_record_buf_sync(p->handle, readbuf, readlen) ) == VPB_OK ) {
			tris_verb(6, "%s: chanreads: got buffer!\n", p->dev);
			/* Apply extra gain ! */
			if( p->rxswgain > MAX_VPB_GAIN )
				a_gain_vector(p->rxswgain - MAX_VPB_GAIN, (short *)readbuf, readlen / sizeof(short));
			tris_verb(6, "%s: chanreads: applied gain\n", p->dev);

			fr->subclass = afmt;
			fr->data.ptr = readbuf;
			fr->datalen = readlen;
			fr->frametype = TRIS_FRAME_VOICE;

			if ((use_tris_dtmfdet)&&(p->vad)) {
				fr = tris_dsp_process(p->owner,p->vad,fr);
				if (fr && (fr->frametype == TRIS_FRAME_DTMF)) {
					tris_debug(1, "%s: chanreads: Detected DTMF '%c'\n", p->dev, fr->subclass);
				} else if (fr->subclass == 'f') {
				}
			}
			/* Using trylock here to prevent deadlock when channel is hungup
			 * (tris_hangup() immediately gets lock)
			 */
			if (p->owner && !p->stopreads) {
				tris_verb(6, "%s: chanreads: queueing buffer on read frame q (state[%d])\n", p->dev, p->owner->_state);
				do {
					res = tris_channel_trylock(p->owner);
					trycnt++;
				} while ((res !=0 ) && (trycnt < 300));
				if (res == 0) {
					tris_queue_frame(p->owner, fr);
					tris_channel_unlock(p->owner);
				} else {
					tris_verb(5, "%s: chanreads: Couldnt get lock after %d tries!\n", p->dev, trycnt);
				}
				trycnt = 0;

/*
				res = tris_mutex_trylock(&p->owner->lock);
				if (res == 0)  {
					tris_queue_frame(p->owner, fr);
					tris_mutex_unlock(&p->owner->lock);
				} else {
					if (res == EINVAL)
						tris_verb(5, "%s: chanreads: try owner->lock gave me EINVAL[%d]\n", p->dev, res);
					else if (res == EBUSY)
						tris_verb(5, "%s: chanreads: try owner->lock gave me EBUSY[%d]\n", p->dev, res);
					while (res != 0) {
					res = tris_mutex_trylock(&p->owner->lock);
					}
					if (res == 0) {
						tris_queue_frame(p->owner, fr);
						tris_mutex_unlock(&p->owner->lock);
					} else {
						if (res == EINVAL) {
							tris_verb(5, "%s: chanreads: try owner->lock gave me EINVAL[%d]\n", p->dev, res);
						} else if (res == EBUSY) {
							tris_verb(5, "%s: chanreads: try owner->lock gave me EBUSY[%d]\n", p->dev, res);
						}
						tris_verb(5, "%s: chanreads: Couldnt get lock on owner[%s][%d][%d] channel to send frame!\n", p->dev, p->owner->name, (int)p->owner->lock.__m_owner, (int)p->owner->lock.__m_count);
					}
				}
*/
					short *data = (short *)readbuf;
				tris_verb(7, "%s: Read channel (codec=%d) %d %d\n", p->dev, fmt, data[0], data[1]);
			} else {
				tris_verb(5, "%s: p->stopreads[%d] p->owner[%p]\n", p->dev, p->stopreads, (void *)p->owner);
			}
		}
		tris_verb(5, "%s: chanreads: Finished cycle...\n", p->dev);
	}
	p->read_state = 0;

	/* When stopreads seen, go away! */
	vpb_record_buf_finish(p->handle);
	p->read_state = 0;
	tris_mutex_unlock(&p->record_lock);

	tris_verb(2, "%s: Ending record mode (%d/%s)\n",
			 p->dev, p->stopreads, p->owner ? "yes" : "no");     
	return NULL;
}

static struct tris_channel *vpb_new(struct vpb_pvt *me, enum tris_channel_state state, const char *context)
{
	struct tris_channel *tmp; 
	char cid_num[256];
	char cid_name[256];

	if (me->owner) {
	    tris_log(LOG_WARNING, "Called vpb_new on owned channel (%s) ?!\n", me->dev);
	    return NULL;
	}
	tris_verb(4, "%s: New call for context [%s]\n", me->dev, context);
	    
	tmp = tris_channel_alloc(1, state, 0, 0, "", me->ext, me->context, 0, "%s", me->dev);
	if (tmp) {
		if (use_tris_ind == 1){
			tmp->tech = &vpb_tech_indicate;
		} else {
			tmp->tech = &vpb_tech;
		}

		tmp->callgroup = me->callgroup;
		tmp->pickupgroup = me->pickupgroup;
	       
		/* Linear is the preferred format. Although Voicetronix supports other formats
		 * they are all converted to/from linear in the vpb code. Best for us to use
		 * linear since we can then adjust volume in this modules.
		 */
		tmp->nativeformats = prefformat;
		tmp->rawreadformat = TRIS_FORMAT_SLINEAR;
		tmp->rawwriteformat =  TRIS_FORMAT_SLINEAR;
		if (state == TRIS_STATE_RING) {
			tmp->rings = 1;
			cid_name[0] = '\0';
			cid_num[0] = '\0';
			tris_callerid_split(me->callerid, cid_name, sizeof(cid_name), cid_num, sizeof(cid_num));
			tris_set_callerid(tmp, cid_num, cid_name, cid_num);
		}
		tmp->tech_pvt = me;
		
		tris_copy_string(tmp->context, context, sizeof(tmp->context));
		if (!tris_strlen_zero(me->ext))
			tris_copy_string(tmp->exten, me->ext, sizeof(tmp->exten));
		else
			strcpy(tmp->exten, "s");
		if (!tris_strlen_zero(me->language))
			tris_string_field_set(tmp, language, me->language);

		me->owner = tmp;

		me->bridge = NULL;
		me->lastoutput = -1;
		me->lastinput = -1;
		me->last_ignore_dtmf = 1;
		me->readthread = 0;
		me->play_dtmf[0] = '\0';
		me->faxhandled = 0;
		
		me->lastgrunt = tris_tvnow(); /* Assume at least one grunt tone seen now. */
		me->lastplay = tris_tvnow(); /* Assume at least one grunt tone seen now. */

		if (state != TRIS_STATE_DOWN) {
			if ((me->mode != MODE_FXO) && (state != TRIS_STATE_UP)) {
				vpb_answer(tmp);
			}
			if (tris_pbx_start(tmp)) {
				tris_log(LOG_WARNING, "Unable to start PBX on %s\n", tmp->name);
				tris_hangup(tmp);
		   	}
		}
	} else {
		tris_log(LOG_WARNING, "Unable to allocate channel structure\n");
	}
	return tmp;
}

static struct tris_channel *vpb_request(const char *type, int format, void *vdata, int *cause) 
{
	int oldformat;
	struct vpb_pvt *p;
	struct tris_channel *tmp = NULL;
	char *sepstr, *data = (char *)vdata, *name;
	const char *s;
	int group = -1;

	oldformat = format;
	format &= prefformat;
	if (!format) {
		tris_log(LOG_NOTICE, "Asked to get a channel of unsupported format '%d'\n", oldformat);
		return NULL;
	}

	name = tris_strdup(S_OR(data, ""));

	sepstr = name;
	s = strsep(&sepstr, "/"); /* Handle / issues */
	if (!s)
		s = "";
	/* Check if we are looking for a group */
	if (toupper(name[0]) == 'G' || toupper(name[0]) == 'R') {
		group = atoi(name + 1);
	}
	/* Search for an unowned channel */
	tris_mutex_lock(&iflock);
	for (p = iflist; p; p = p->next) {
		if (group == -1) {
			if (strncmp(s, p->dev + 4, sizeof p->dev) == 0) {
				if (!p->owner) {
					tmp = vpb_new(p, TRIS_STATE_DOWN, p->context);
					break;
				}
			}
		} else {
			if ((p->group == group) && (!p->owner)) {
				tmp = vpb_new(p, TRIS_STATE_DOWN, p->context);
				break;
			}
		}
	}
	tris_mutex_unlock(&iflock);


	tris_verb(2, " %s requested, got: [%s]\n", name, tmp ? tmp->name : "None");

	tris_free(name);

	restart_monitor();
	return tmp;
}

static float parse_gain_value(const char *gain_type, const char *value)
{
	float gain;

	/* try to scan number */
	if (sscanf(value, "%f", &gain) != 1) {
		tris_log(LOG_ERROR, "Invalid %s value '%s' in '%s' config\n", value, gain_type, config);
		return DEFAULT_GAIN;
	}


	/* percentage? */
	/*if (value[strlen(value) - 1] == '%') */
	/*	return gain / (float)100; */

	return gain;
}


static int unload_module(void)
{
	struct vpb_pvt *p;
	/* First, take us out of the channel loop */
	if (use_tris_ind == 1){
		tris_channel_unregister(&vpb_tech_indicate);
	} else {
		tris_channel_unregister(&vpb_tech);
	}

	tris_mutex_lock(&iflock);
	/* Hangup all interfaces if they have an owner */
	for (p = iflist; p; p = p->next) {
		if (p->owner)
			tris_softhangup(p->owner, TRIS_SOFTHANGUP_APPUNLOAD);
	}
	iflist = NULL;
	tris_mutex_unlock(&iflock);

	tris_mutex_lock(&monlock);
	if (mthreadactive > -1) {
		pthread_cancel(monitor_thread);
		pthread_join(monitor_thread, NULL);
	}
	mthreadactive = -2;
	tris_mutex_unlock(&monlock);

	tris_mutex_lock(&iflock);
	/* Destroy all the interfaces and free their memory */

	while (iflist) {
		p = iflist;		    
		tris_mutex_destroy(&p->lock);
		pthread_cancel(p->readthread);
		tris_mutex_destroy(&p->owner_lock);
		tris_mutex_destroy(&p->record_lock);
		tris_mutex_destroy(&p->play_lock);
		tris_mutex_destroy(&p->play_dtmf_lock);
		p->readthread = 0;

		vpb_close(p->handle);

		iflist = iflist->next;

		tris_free(p);
	}
	iflist = NULL;
	tris_mutex_unlock(&iflock);

	if (bridges) {
		tris_mutex_lock(&bridge_lock);
		memset(bridges, 0, sizeof bridges);
		tris_mutex_unlock(&bridge_lock);
		tris_mutex_destroy(&bridge_lock);
		for (int i = 0; i < max_bridges; i++) {
			tris_mutex_destroy(&bridges[i].lock);
			tris_cond_destroy(&bridges[i].cond);
		}
		tris_free(bridges);
	}

	return 0;
}

static enum tris_module_load_result load_module()
{
	struct tris_config *cfg;
	struct tris_variable *v;
	struct tris_flags config_flags = { 0 };
	struct vpb_pvt *tmp;
	int board = 0, group = 0;
	tris_group_t	callgroup = 0;
	tris_group_t	pickupgroup = 0;
	int mode = MODE_IMMEDIATE;
	float txgain = DEFAULT_GAIN, rxgain = DEFAULT_GAIN; 
	float txswgain = 0, rxswgain = 0; 
	int got_gain=0;
	int first_channel = 1;
	int echo_cancel = DEFAULT_ECHO_CANCEL;
	enum tris_module_load_result error = TRIS_MODULE_LOAD_SUCCESS; /* Error flag */
	int bal1 = -1; /* Special value - means do not set */
	int bal2 = -1; 
	int bal3 = -1;
	char * callerid = NULL;

	int num_cards = 0;
	try {
		num_cards = vpb_get_num_cards();
	} catch (std::exception e) {
		tris_log(LOG_ERROR, "No Voicetronix cards detected\n");
		return TRIS_MODULE_LOAD_DECLINE;
	}

	int ports_per_card[num_cards];
	for (int i = 0; i < num_cards; ++i)
		ports_per_card[i] = vpb_get_ports_per_card(i);

	cfg = tris_config_load(config, config_flags);

	/* We *must* have a config file otherwise stop immediately */
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Unable to load config %s\n", config);
		return TRIS_MODULE_LOAD_DECLINE;
	}  

	tris_mutex_lock(&iflock);
	v = tris_variable_browse(cfg, "general");
	while (v){
		if (strcasecmp(v->name, "cards") == 0) {
			tris_log(LOG_NOTICE, "VPB Driver configured to use [%d] cards\n", atoi(v->value));
		} else if (strcasecmp(v->name, "indication") == 0) {
			use_tris_ind = 1;
			tris_log(LOG_NOTICE, "VPB driver using Trismedia Indication functions!\n");
		} else if (strcasecmp(v->name, "break-for-dtmf") == 0) {
			if (tris_true(v->value)) {
				break_for_dtmf = 1;
			} else {
				break_for_dtmf = 0;
				tris_log(LOG_NOTICE, "VPB driver not stopping for DTMF's in native bridge\n");
			}
		} else if (strcasecmp(v->name, "ast-dtmf") == 0) {
			use_tris_dtmf = 1;
			tris_log(LOG_NOTICE, "VPB driver using Trismedia DTMF play functions!\n");
		} else if (strcasecmp(v->name, "ast-dtmf-det") == 0) {
			use_tris_dtmfdet = 1;
			tris_log(LOG_NOTICE, "VPB driver using Trismedia DTMF detection functions!\n");
		} else if (strcasecmp(v->name, "relaxdtmf") == 0) {
			relaxdtmf = 1;
			tris_log(LOG_NOTICE, "VPB driver using Relaxed DTMF with Trismedia DTMF detections functions!\n");
		} else if (strcasecmp(v->name, "timer_period_ring") == 0) {
			timer_period_ring = atoi(v->value);
		} else if (strcasecmp(v->name, "ecsuppthres") == 0) {
			ec_supp_threshold = (short)atoi(v->value);
		} else if (strcasecmp(v->name, "dtmfidd") == 0) {
			dtmf_idd = atoi(v->value);
			tris_log(LOG_NOTICE, "VPB Driver setting DTMF IDD to [%d]ms\n", dtmf_idd);
		}
		v = v->next;
	}
	
	v = tris_variable_browse(cfg, "interfaces");
	while (v) {
		/* Create the interface list */
		if (strcasecmp(v->name, "board") == 0) {
			board = atoi(v->value);
		} else if (strcasecmp(v->name, "group") == 0) {
			group = atoi(v->value);
		} else if (strcasecmp(v->name, "callgroup") == 0) {
			callgroup = tris_get_group(v->value);
		} else if (strcasecmp(v->name, "pickupgroup") == 0) {
			pickupgroup = tris_get_group(v->value);
		} else if (strcasecmp(v->name, "usepolaritycid") == 0) {
			UsePolarityCID = atoi(v->value);
		} else if (strcasecmp(v->name, "useloopdrop") == 0) {
			UseLoopDrop = atoi(v->value);
		} else if (strcasecmp(v->name, "usenativebridge") == 0) {
			UseNativeBridge = atoi(v->value);
		} else if (strcasecmp(v->name, "channel") == 0) {
			int channel = atoi(v->value);
			if (board >= num_cards || board < 0 || channel < 0 || channel >= ports_per_card[board]) {
				tris_log(LOG_ERROR, "Invalid board/channel (%d/%d) for channel '%s'\n", board, channel, v->value);
				error = TRIS_MODULE_LOAD_FAILURE;
				goto done;
			}
			tmp = mkif(board, channel, mode, got_gain, txgain, rxgain, txswgain, rxswgain, bal1, bal2, bal3, callerid, echo_cancel,group,callgroup,pickupgroup);
			if (tmp) {
				if (first_channel) {
					mkbrd(tmp->vpb_model, echo_cancel);
					first_channel = 0;
				}
				tmp->next = iflist;
				iflist = tmp;
			} else {
				tris_log(LOG_ERROR, "Unable to register channel '%s'\n", v->value);
				error = TRIS_MODULE_LOAD_FAILURE;
				goto done;
			}
		} else if (strcasecmp(v->name, "language") == 0) {
			tris_copy_string(language, v->value, sizeof(language));
		} else if (strcasecmp(v->name, "callerid") == 0) {
			callerid = tris_strdup(v->value);
		} else if (strcasecmp(v->name, "mode") == 0) {
			if (strncasecmp(v->value, "di", 2) == 0) {
				mode = MODE_DIALTONE;
			} else if (strncasecmp(v->value, "im", 2) == 0) {
				mode = MODE_IMMEDIATE;
			} else if (strncasecmp(v->value, "fx", 2) == 0) {
				mode = MODE_FXO;
			} else {
				tris_log(LOG_WARNING, "Unknown mode: %s\n", v->value);
			}
		} else if (!strcasecmp(v->name, "context")) {
			tris_copy_string(context, v->value, sizeof(context));
		} else if (!strcasecmp(v->name, "echocancel")) {
			if (!strcasecmp(v->value, "off")) {
				echo_cancel = 0;
			}
		} else if (strcasecmp(v->name, "txgain") == 0) {
			txswgain = parse_gain_value(v->name, v->value);
			got_gain |= VPB_GOT_TXSWG;
		} else if (strcasecmp(v->name, "rxgain") == 0) {
			rxswgain = parse_gain_value(v->name, v->value);
			got_gain |= VPB_GOT_RXSWG;
		} else if (strcasecmp(v->name, "txhwgain") == 0) {
			txgain = parse_gain_value(v->name, v->value);
			got_gain |= VPB_GOT_TXHWG;
		} else if (strcasecmp(v->name, "rxhwgain") == 0) {
			rxgain = parse_gain_value(v->name, v->value);
			got_gain |= VPB_GOT_RXHWG;
		} else if (strcasecmp(v->name, "bal1") == 0) {
			bal1 = strtol(v->value, NULL, 16);
			if (bal1 < 0 || bal1 > 255) {
				tris_log(LOG_WARNING, "Bad bal1 value: %d\n", bal1);
				bal1 = -1;
			}
		} else if (strcasecmp(v->name, "bal2") == 0) {
			bal2 = strtol(v->value, NULL, 16);
			if (bal2 < 0 || bal2 > 255) {
				tris_log(LOG_WARNING, "Bad bal2 value: %d\n", bal2);
				bal2 = -1;
			}
		} else if (strcasecmp(v->name, "bal3") == 0) {
			bal3 = strtol(v->value, NULL, 16);
			if (bal3 < 0 || bal3 > 255) {
				tris_log(LOG_WARNING, "Bad bal3 value: %d\n", bal3);
				bal3 = -1;
			}
		} else if (strcasecmp(v->name, "grunttimeout") == 0) {
			gruntdetect_timeout = 1000 * atoi(v->value);
		}
		v = v->next;
	}

	if (gruntdetect_timeout < 1000)
		gruntdetect_timeout = 1000;

	done: (void)0;
	tris_mutex_unlock(&iflock);

	tris_config_destroy(cfg);

	if (use_tris_ind == 1) {
		if (error == TRIS_MODULE_LOAD_SUCCESS && tris_channel_register(&vpb_tech_indicate) != 0) {
			tris_log(LOG_ERROR, "Unable to register channel class 'vpb'\n");
			error = TRIS_MODULE_LOAD_FAILURE;
		} else {
			tris_log(LOG_NOTICE, "VPB driver Registered (w/AstIndication)\n");
		}
	} else {
		if (error == TRIS_MODULE_LOAD_SUCCESS && tris_channel_register(&vpb_tech) != 0) {
			tris_log(LOG_ERROR, "Unable to register channel class 'vpb'\n");
			error = TRIS_MODULE_LOAD_FAILURE;
		} else {
			tris_log(LOG_NOTICE, "VPB driver Registered )\n");
		}
	}


	if (error != TRIS_MODULE_LOAD_SUCCESS)
		unload_module();
	else 
		restart_monitor(); /* And start the monitor for the first time */

	return error;
}

/**/
#if defined(__cplusplus) || defined(c_plusplus)
 }
#endif
/**/

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Voicetronix API driver");
