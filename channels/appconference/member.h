
// $Id: member.h 872 2007-03-05 23:43:10Z sbalea $

/*
 * app_conference
 *
 * A channel independent conference application for Triscore
 *
 * Copyright (C) 2002, 2003 Junghanns.NET GmbH
 * Copyright (C) 2003, 2004 HorizonLive.com, Inc.
 * Copyright (C) 2005, 2006 HorizonWimba, Inc.
 * Copyright (C) 2007 Wimba, Inc.
 *
 * Klaus-Peter Junghanns <kapejod@ns1.jnetdns.de>
 *
 * Video Conferencing support added by
 * Neil Stratford <neils@vipadia.com>
 * Copyright (C) 2005, 2005 Vipadia Limited
 *
 * VAD driven video conferencing, text message support
 * and miscellaneous enhancements added by
 * Mihai Balea <mihai at hates dot ms>
 *
 * This program may be modified and distributed under the
 * terms of the GNU General Public License. You should have received
 * a copy of the GNU General Public License along with this
 * program; if not, write to the Free Software Foundation, Inc.
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _APP_CONF_MEMBER_H
#define _APP_CONF_MEMBER_H

//
// includes
//

#include "app_conference.h"
#include "conference.h"
#include "common.h"

//
// struct declarations
//

struct tris_conf_soundq
{
	char name[256];
	struct tris_filestream *stream; // the stream
	int muted; // should incoming audio be muted while we play?
	struct tris_conf_soundq *next;
};

struct tris_conf_member
{
	tris_mutex_t lock ; // member data mutex

	struct tris_channel* chan ; // member's channel
	char* channel_name ; // member's channel name

	// values passed to create_member () via *data
	int priority ;	// highest priority gets the channel
	char* flags ;	// raw member-type flags
	char type ;		// L = ListenOnly, M = Moderator, S = Standard (Listen/Talk)
	char* conf_name ;		// name of the conference that own this member

	char *callerid;
	char *callername;

	// voice flags
	int vad_flag;
	int denoise_flag;
	int agc_flag;
	int via_telephone;

	// video conference params
	int id;
	int initial_id;
	int req_id;

	// muting options - this member will not be heard/seen
	int mute_audio;
	int backup_mute_audio;
	int mute_video;

	// this member will not hear/see
	int norecv_audio;
	int backup_norecv_audio;
	int norecv_video;

	// this member does not have a camera
	int no_camera;

	// is this person a moderator?
	int ismoderator;

	int is_dialouted;

	// determine by flags and channel name
	char connection_type ; // T = telephone, X = iaxclient, S = sip

	// vad voice probability thresholds
	float vad_prob_start ;
	float vad_prob_continue ;

	// ready flag
	short ready_for_outgoing ;

	// input frame queue
	conf_frame* inFramesP ;
	conf_frame* inFramesS ;
	conf_frame* inFramesTailP ;
	conf_frame* inFramesTailS ;
	unsigned int inFramesCountP ;
	unsigned int inFramesCountS ;
	conf_frame* inVideoFrames ;
	conf_frame* inVideoFramesTail ;
	unsigned int inVideoFramesCount ;
	conf_frame* inDTMFFrames ;
	conf_frame* inDTMFFramesTail ;
	unsigned int inDTMFFramesCount ;
	conf_frame* inTextFrames ;
	conf_frame* inTextFramesTail ;
	unsigned int inTextFramesCount ;


	// input/output smoother
	struct tris_smoother *inSmoother;
	struct tris_packer *outPacker;
	int smooth_size_in;
	int smooth_size_out;
	int smooth_multiple;

	// frames needed by conference_exec
	unsigned int inFramesNeeded ;
	unsigned int inVideoFramesNeeded ;

	// used when caching last frame
	conf_frame* inFramesLastP ;
	unsigned int inFramesRepeatLastP ;
	unsigned short okayToCacheLastP ;

	// used when caching last frame
	conf_frame* inFramesLastS ;
	unsigned int inFramesRepeatLastS ;
	unsigned short okayToCacheLastS ;

	// LL output frame queue
	conf_frame* outFrames ;
	conf_frame* outFramesTail ;
	unsigned int outFramesCount ;
	conf_frame* outVideoFrames ;
	conf_frame* outVideoFramesTail ;
	unsigned int outVideoFramesCount ;
	conf_frame* outDTMFFrames ;
	conf_frame* outDTMFFramesTail ;
	unsigned int outDTMFFramesCount ;
	conf_frame* outTextFrames ;
	conf_frame* outTextFramesTail ;
	unsigned int outTextFramesCount ;

	// LL video switched flag
	short conference;

	// switch video by VAD?
	short vad_switch;
	// switch by dtmf?
	short dtmf_switch;
	// relay dtmf to manager?
	short dtmf_relay;
	// initial nat delay flag
	short first_frame_received;
	// does text messages?
	short does_text;


	// time we last dropped a frame
	struct timeval ltris_in_dropped ;
	struct timeval ltris_out_dropped ;

	// ( not currently used )
	// int samplesperframe ;

	// used for determining need to mix frames
	// and for management interface notification
	// and for VAD based video switching
	short speaking_state_notify ;
	short speaking_state ; // This flag will be true if this member or any of its drivers is speaking
	struct timeval ltris_state_change;
	int speaker_count; // Number of drivers (including this member) that are speaking

	// pointer to next member in single-linked list
	struct tris_conf_member* next ;

	// accounting values
	unsigned long frames_in ;
	unsigned long frames_in_dropped ;
	unsigned long frames_out ;
	unsigned long frames_out_dropped ;

	unsigned long video_frames_in ;
	unsigned long video_frames_in_dropped ;
	unsigned long video_frames_out ;
	unsigned long video_frames_out_dropped ;

	unsigned long dtmf_frames_in ;
	unsigned long dtmf_frames_in_dropped ;
	unsigned long dtmf_frames_out ;
	unsigned long dtmf_frames_out_dropped ;

	unsigned long text_frames_in ;
	unsigned long text_frames_in_dropped ;
	unsigned long text_frames_out ;
	unsigned long text_frames_out_dropped ;

	// for counting sequentially dropped frames
	unsigned int sequential_drops ;
	unsigned long since_dropped ;

	// start time
	struct timeval time_entered ;
	struct timeval lastsent_timeval ;

	// flag indicating we should remove this member
	short remove_flag ;
	short kick_flag ;

#if ( SILDET == 2 )
	// pointer to speex preprocessor dsp
	SpeexPreprocessState *dsp ;
        // number of frames to ignore speex_preprocess()
	int ignore_speex_count;
#else
	// placeholder when preprocessing is not enabled
	void* dsp ;
#endif

	// audio format this member is using
	int write_format ;
	int read_format ;

	int write_format_index ;
	int read_format_index ;

	// member frame translators
	struct tris_trans_pvt* to_slinear_P ;
	struct tris_trans_pvt* to_slinear_S ;
	struct tris_trans_pvt* from_slinear ;

	// For playing sounds
	struct tris_conf_soundq *soundq;
	struct tris_conf_soundq *videoq;

	// Pointer to another member that will be driven from this member's audio
	struct tris_conf_member *driven_member;
} ;

struct conf_member
{
	struct tris_conf_member* realmember ;
	struct conf_member* next ;
} ;

//
// function declarations
//

int member_exec( struct tris_channel* chan, const char* data );

struct tris_conf_member* check_active_video( int id, struct tris_conference *conf );

struct tris_conf_member* create_member( struct tris_channel* chan, const char* data ) ;
struct tris_conf_member* delete_member( struct tris_conf_member* member ) ;

// incoming queue
int queue_incoming_frameP( struct tris_conf_member* member, struct tris_frame* fr ) ;
int queue_incoming_frameS( struct tris_conf_member* member, struct tris_frame* fr ) ;
int queue_incoming_video_frame( struct tris_conf_member* member, const struct tris_frame* fr ) ;
int queue_incoming_dtmf_frame( struct tris_conf_member* member, const struct tris_frame* fr ) ;
conf_frame* get_incoming_frameP( struct tris_conf_member* member ) ;
conf_frame* get_incoming_frameS( struct tris_conf_member* member ) ;
conf_frame* get_incoming_video_frame( struct tris_conf_member* member ) ;
conf_frame* get_incoming_dtmf_frame( struct tris_conf_member* member ) ;

// outgoing queue
int queue_outgoing_frame( struct tris_conf_member* member, const struct tris_frame* fr, struct timeval delivery ) ;
int __queue_outgoing_frame( struct tris_conf_member* member, const struct tris_frame* fr, struct timeval delivery ) ;
conf_frame* get_outgoing_frame( struct tris_conf_member* member ) ;

int queue_outgoing_video_frame( struct tris_conf_member* member, const struct tris_frame* fr, struct timeval delivery ) ;
conf_frame* get_outgoing_video_frame( struct tris_conf_member* member ) ;
int queue_outgoing_dtmf_frame( struct tris_conf_member* member, const struct tris_frame* fr ) ;
int queue_outgoing_text_frame( struct tris_conf_member* member, const struct tris_frame* fr ) ;
conf_frame* get_outgoing_dtmf_frame( struct tris_conf_member* member ) ;
conf_frame* get_outgoing_text_frame( struct tris_conf_member* member ) ;

void send_state_change_notifications( struct tris_conf_member* member ) ;

int increment_speaker_count(struct tris_conf_member *member, int lock);
int decrement_speaker_count(struct tris_conf_member *member, int lock);

void member_process_spoken_frames(struct tris_conference* conf,
				  struct tris_conf_member *member,
				  struct conf_frame **spoken_frames,
				  long time_diff,
				 int *listener_count,
				 int *speaker_count);

void member_process_outgoing_frames(struct tris_conference* conf,
				    struct tris_conf_member *member,
				    struct conf_frame *send_frames);

//
// packer functions
//

struct tris_packer;

extern struct tris_packer *tris_packer_new(int bytes);
extern void tris_packer_set_flags(struct tris_packer *packer, int flags);
extern int tris_packer_get_flags(struct tris_packer *packer);
extern void tris_packer_free(struct tris_packer *s);
extern void tris_packer_reset(struct tris_packer *s, int bytes);
extern int tris_packer_feed(struct tris_packer *s, const struct tris_frame *f);
extern struct tris_frame *tris_packer_read(struct tris_packer *s);
#endif
