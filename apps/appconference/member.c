
// $Id: member.c 885 2007-06-27 15:41:18Z sbalea $

/*
 * app_conference
 *
 * A channel independent conference application for Trismedia
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

#include <stdio.h>
#include "trismedia/autoconfig.h"
#include "member.h"
#include "trismedia/app.h"
#include "trismedia/say.h"
#include "trismedia/dial.h"
#include "trismedia/astdb.h"	/* trisdb */
#include "trismedia/acl.h"
#include "trismedia/rtp.h"
#include "trismedia/channel.h"
#include "trismedia/frame.h"

#include "common.h"

#define CONFERENCE_URGENCY_CONF 1

// set non-speaker's desktop media option
int set_members_desktop(struct tris_conference * conf, struct tris_conf_member * speaker)
{
	char nodesktop = 0;
	
	if(conf == NULL || !speaker)
		return -1;
	
	if(conf->default_video_source_id != speaker->id){
		tris_log(LOG_DEBUG, "yz: We only process for the speaker.\n");
		return 1;
	}

	if(speaker->chan->tech->get_pvt_rtpneed)
		nodesktop = speaker->chan->tech->get_pvt_rtpneed("desktop", speaker->chan);
	
	if((speaker->desktop && !nodesktop) || (!speaker->desktop && nodesktop)){
		tris_log(LOG_DEBUG, "yz: We only process on desktop change for the speaker.\n");
		return 0;
	}

	struct tris_rtp * src_drtp = NULL;
	if(speaker->chan->tech->get_pvt_rtp)
		src_drtp = speaker->chan->tech->get_pvt_rtp("desktop", speaker->chan);
	if(!src_drtp || nodesktop){
		speaker->desktop = 0;
	}else{
		speaker->desktop = 1;
	}
	
	struct tris_conf_member * member = conf->memberlist;
	while(member){
		if(member == speaker){
			member = member->next;
			continue;
		}

		/* copying rtp info from speaker to others... */
		if(member->chan->tech->set_pvt_rtp)
			member->chan->tech->set_pvt_rtp("desktop", member->chan, speaker->chan);

		/* let send re-invite request */
		tris_log(LOG_DEBUG, "yz: notify SRCUPDATE to member=%s\n", member->callerid);
		if (member->chan && !tris_check_hangup(member->chan) && !member->remove_flag)
			tris_indicate(member->chan, TRIS_CONTROL_SRCUPDATE);
		
		member = member->next;
	}
	
	return 0;
}

// If speaker share desktop, we must send re-invite to member
int determin_reinvite(struct tris_conference * conf, struct tris_conf_member * member)
{
	struct tris_conf_member * speaker = NULL, * others;
	struct tris_rtp * src_drtp = NULL;//, * dst_drtp = NULL;

	/* if member has rtp for desktop, we return now */
	/*if(member->chan->tech->get_pvt_rtp)
		dst_drtp = member->chan->tech->get_pvt_rtp("desktop", member->chan);
	if(dst_drtp){
		return 0;
	}*/

	if(conf == NULL || conf->default_video_source_id < 0)
		return 0;

	/* find speaker by id in conference */
	tris_mutex_lock( &conf->lock ) ;
	for(others = conf->memberlist; others != NULL; others = others->next){
		if(others->id == conf->default_video_source_id){
			speaker = others;
			break;
		}
	}
	tris_mutex_unlock( &conf->lock ) ;

	if(speaker == NULL || !speaker->desktop)
		return 0;

	if(speaker->chan->tech->get_pvt_rtp)
		src_drtp = speaker->chan->tech->get_pvt_rtp("desktop", speaker->chan);

	if(!src_drtp){
		return 0;
	}
	
	/* if speaker is sharing desktop and member has no rtp for desktop, 
		we copy rtp info for desktop from speaker and 
		indicate the member's channel to send re-invite */
	if(member->chan->tech->set_pvt_rtp){
		tris_log(LOG_DEBUG, "yz: we'll send re-invite. speaker's desktop=%d\n", speaker->desktop);
		member->chan->tech->set_pvt_rtp("desktop", member->chan, speaker->chan);

		if (member->chan && !tris_check_hangup(member->chan) && !member->remove_flag)
			tris_indicate(member->chan, TRIS_CONTROL_SRCUPDATE);
	}
	
	return 0;
}

// send NOTIFY to all members
// PARAMS: flag=1 => notify when member comes in, flag=2 => notify when member goes out.
int notify_members(struct tris_conference * conf, struct tris_conf_member * newmember, int flag)
{
	struct tris_conf_member * member = conf->memberlist;
	char str2mem[1024];	str2mem[0]='\0';
	char str2others[1024];	str2others[0]='\0';
	int len=0;

	if(newmember->chan->cid.cid_num){
		strcat(str2others, newmember->chan->cid.cid_num);
		if(flag == 1)
			strcat(str2others, " is in");
		else if(flag == 2)
			strcat(str2others, " is out");
	}

	tris_mutex_lock(&conf->lock);
	while(member){
		if(member == newmember){
			member = member->next;
			continue;
		}

		/* let send NOTIFY request */
		//tris_log(LOG_DEBUG, "yz: notify to member=%s\n", member->callerid);
		if(strlen(str2others) > 0){
			if (member->chan && !tris_check_hangup(member->chan) && !member->remove_flag)
				tris_indicate_data(member->chan, TRIS_CONTROL_NOTIFY_NEWMEMBER, str2others, strlen(str2others) + 1);
		}else{
			if (member->chan && !tris_check_hangup(member->chan) && !member->remove_flag)
				tris_indicate(member->chan, TRIS_CONTROL_NOTIFY_NEWMEMBER);
		}

		if(flag == 1 && member->callerid){
			strcat(str2mem, member->callerid);
			strcat(str2mem, ",");
			len += strlen(member->callerid) + 1;
		}
		
		member = member->next;
	}
	tris_mutex_unlock(&conf->lock);

	if(len > 0){		
		str2mem[len-1] = '\0';	//remove ',' at the end of string.
		strcat(str2mem, " are in Conference");
		//if(strlen(str2others)>0){
		//	tris_log(LOG_DEBUG, "yz: notify to member=%s '%s'\n", newmember->chan->cid.cid_num, str2mem);
		//}
		if (newmember->chan && !tris_check_hangup(newmember->chan) && !newmember->remove_flag)
			tris_indicate_data(newmember->chan, TRIS_CONTROL_NOTIFY_NEWMEMBER, str2mem, strlen(str2mem) + 1);
	}
	
	return 0;
}

// process an incoming frame.  Returns 0 normally, 1 if hangup was received.
static int process_incoming(struct tris_conf_member *member, struct tris_conference *conf, struct tris_frame *f)
{
	int silent_frame = 0;
	struct tris_conf_member *src_member ;

	// In Trismedia 1.4 TRIS_FRAME_DTMF is equivalent to TRIS_FRAME_DTMF_END
	if (f->frametype == TRIS_FRAME_DTMF)
	{
		if (member->dtmf_switch)
		{
			tris_mutex_lock( &member->lock ) ;
			switch (f->subclass) {
			case '0' :member->req_id=0;
				break;
			case '1' :member->req_id=1;
				break;
			case '2' :member->req_id=2;
				break;
			case '3' :member->req_id=3;
				break;
			case '4' :member->req_id=4;
				break;
			case '5' :member->req_id=5;
				break;
			case '6' :member->req_id=6;
				break;
			case '7' :member->req_id=7;
				break;
			case '8' :member->req_id=8;
				break;
			case '9' :member->req_id=9;
				break;
			case '*' :
				if (member->mute_video == 0 && member->mute_audio == 0)
				{
					member->mute_video = 1;
					member->mute_audio = 1;
				}
				else if (member->mute_video == 1 && member->mute_audio == 1)
				{
					member->mute_video = 0;
					member->mute_audio = 0;
				}
				break;
			
			}
			member->conference = 1; // switch me
			tris_mutex_unlock( &member->lock ) ;
		}
		if (member->dtmf_relay)
		{
			// output to manager...
			manager_event(
				EVENT_FLAG_CALL,
				"ConferenceDTMF",
				"ConferenceName: %s\r\n"
				"Channel: %s\r\n"
				"CallerID: %s\r\n"
				"CallerIDName: %s\r\n"
				"Key: %c\r\n",
				conf->name,
				member->channel_name,
				member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
				member->chan->cid.cid_name ? member->chan->cid.cid_name : "unknown",
				f->subclass
				) ;

		}
		if (!member->dtmf_switch && !member->dtmf_relay)
		{
			// relay this to the listening channels
			queue_incoming_dtmf_frame( member, f );
		}
	} else if (f->frametype == TRIS_FRAME_DTMF_BEGIN)
	{
		if (!member->dtmf_switch && !member->dtmf_relay)
		{
			// relay this to the listening channels
			queue_incoming_dtmf_frame( member, f );
		}
	}

	tris_mutex_lock( &member->lock ) ;
	// Handle a local or remote conference
	if (member->conference)
	{
		int req_id = member->req_id;
		tris_mutex_unlock( &member->lock );
		// this will return NULL or a locked member
		src_member = check_active_video(req_id,conf);
		// Stream a picture to the recipient if no active video
		if (!src_member)
		{
			// Mihai: we don't want to send video here, we cannot negotiate codec
			// and we don't know what codec the conference is using
			//if (member->norecv_video == 0)
			//{
			//	if(!tris_streamfile(member->chan,"novideo",member->chan->language))
			//	{
			//		tris_waitstream(member->chan,"");
			//	}
			//}
		}
		else
		{
			// Send a FIR to the new sender
			if (src_member->chan && !tris_check_hangup(src_member->chan) && !src_member->remove_flag)
				tris_indicate(src_member->chan,TRIS_CONTROL_VIDUPDATE);
			// we will have locked in check_active_video()
			tris_mutex_unlock( &src_member->lock);
		}
		tris_mutex_lock( &member->lock );
		member->conference = 0;
	}
	tris_mutex_unlock( &member->lock );


	if ((f->frametype == TRIS_FRAME_VOICE && member->mute_audio == 1) || (f->frametype == TRIS_FRAME_VIDEO && member->mute_video == 1))
	{
		// this is a listen-only user, ignore the frame
		//tris_log( TRIS_CONF_DEBUG, "Listen only user frame");
		tris_frfree( f ) ;
		f = NULL ;
	}
	else if ( f->frametype == TRIS_FRAME_VOICE )
	{	//tris_log( TRIS_CONF_DEBUG, "Got voice frame");
		// reset silence detection flag
		silent_frame = 0 ;

		// accounting: count the incoming frame
		member->frames_in++ ;

#if ( SILDET == 2 )
		//
		// make sure we have a valid dsp and frame type
		//
		if (
			member->dsp != NULL
			&& f->subclass == TRIS_FORMAT_SLINEAR
			&& f->datalen == TRIS_CONF_FRAME_DATA_SIZE
			)
		{
			// send the frame to the preprocessor
			int spx_ret;
			spx_ret = speex_preprocess( member->dsp, f->data, NULL );
#ifdef DEBUG_USE_TIMELOG
			TIMELOG(spx_ret, 3, "speex_preprocess");
#endif
			if ( spx_ret == 0 )
			{
				//
				// we ignore the preprocessor's outcome if we've seen voice frames
				// in within the last TRIS_CONF_SKIP_SPEEX_PREPROCESS frames
				//
				if ( member->ignore_speex_count > 0 )
				{
					// tris_log( TRIS_CONF_DEBUG, "ignore_speex_count => %d\n", ignore_speex_count ) ;

					// skip speex_preprocess(), and decrement counter
					--member->ignore_speex_count ;
				}
				else
				{
					// set silent_frame flag
					silent_frame = 1 ;
				}
			}
			else
			{
				// voice detected, reset skip count
				member->ignore_speex_count = TRIS_CONF_SKIP_SPEEX_PREPROCESS ;
			}
		}
#endif
		if ( !silent_frame )
			queue_incoming_frame( member, f );

		// free the original frame
		tris_frfree( f ) ;
		f = NULL ;

	}
	else if (f->frametype == TRIS_FRAME_VIDEO)
	{
		queue_incoming_video_frame( member, f );

		// free the original frame
		tris_frfree( f ) ;
		f = NULL ;

	}
	else if (f->frametype == TRIS_FRAME_DESKTOP)
	{
		queue_incoming_desktop_frame( member, f );

		// free the original frame
		tris_frfree( f ) ;
		f = NULL ;

	}
	else if (
		f->frametype == TRIS_FRAME_CONTROL
		&& f->subclass == TRIS_CONTROL_HANGUP
		)
	{
		// hangup received

		// free the frame
		tris_frfree( f ) ;
		f = NULL ;

		// break out of the while ( 42 == 42 )
		return 1;
	}
	else if (
		f->frametype == TRIS_FRAME_CONTROL
		&& f->subclass == TRIS_CONTROL_VIDUPDATE
		)
	{
		// say we have switched to cause a FIR to
		// be sent to the sender
		tris_mutex_lock( &member->lock ) ;
		member->conference = 1;
		tris_mutex_unlock( &member->lock ) ;

		// free the original frame
		tris_frfree( f ) ;
		f = NULL ;
	}
	else if ( f->frametype == TRIS_FRAME_TEXT  && member->does_text )
	{
		if ( strncmp(f->data.ptr, TRIS_CONF_CONTROL_CAMERA_DISABLED, strlen(TRIS_CONF_CONTROL_CAMERA_DISABLED)) == 0 )
		{
			tris_mutex_lock(&member->lock);
			manager_event(EVENT_FLAG_CALL,
			              "ConferenceCameraDisabled",
			              "ConferenceName: %s\r\nChannel: %s\r\n",
			              conf->name,
			              member->channel_name);
			member->no_camera = 1;
			tris_mutex_unlock(&member->lock);
		} else if ( strncmp(f->data.ptr, TRIS_CONF_CONTROL_CAMERA_ENABLED, strlen(TRIS_CONF_CONTROL_CAMERA_ENABLED)) == 0 )
		{
			tris_mutex_lock(&member->lock);
			manager_event(EVENT_FLAG_CALL,
			              "ConferenceCameraEnabled",
			              "ConferenceName: %s\r\nChannel: %s\r\n",
			              conf->name,
			              member->channel_name);
			member->no_camera = 0;
			tris_mutex_unlock(&member->lock);
		} else if ( strncmp(f->data.ptr, TRIS_CONF_CONTROL_STOP_VIDEO_TRANSMIT, strlen(TRIS_CONF_CONTROL_STOP_VIDEO_TRANSMIT)) == 0 )
		{
			tris_mutex_lock(&member->lock);
			manager_event(EVENT_FLAG_CALL,
			              "ConferenceStopVideoTransmit",
			              "ConferenceName: %s\r\nChannel: %s\r\n",
			              conf->name,
			              member->channel_name);
			member->norecv_video = 1;
			tris_mutex_unlock(&member->lock);
		} else if ( strncmp(f->data.ptr, TRIS_CONF_CONTROL_START_VIDEO_TRANSMIT, strlen(TRIS_CONF_CONTROL_START_VIDEO_TRANSMIT)) == 0 )
		{
			tris_mutex_lock(&member->lock);
			manager_event(EVENT_FLAG_CALL,
			              "ConferenceStartVideoTransmit",
			              "ConferenceName: %s\r\nChannel: %s\r\n",
			              conf->name,
			              member->channel_name);
			member->norecv_video = 0;
			tris_mutex_unlock(&member->lock);
		}
		tris_frfree(f);
		f = NULL;
	}
	else if (
		f->frametype == TRIS_FRAME_CONTROL
		&& f->subclass == TRIS_CONTROL_SRCUPDATE
		)
	{
		// if member is speaker and has m=desktop option,
		// we must notify all others it.
		tris_mutex_lock( &member->lock ) ;
		if(member->belongto != NULL && member->belongto->default_video_source_id == member->id){
			tris_log(LOG_DEBUG, "yz: We found re-invite for desktop sharing on speaker. Notifying all others.\n");
			set_members_desktop(member->belongto, member);
		}
		tris_mutex_unlock( &member->lock ) ;

		// free the original frame
		tris_frfree( f ) ;
		f = NULL ;
	}
	else
	{
		// undesirables
		tris_frfree( f ) ;
		f = NULL ;
	}

	return 0;
}

// get the next frame from the soundq;  must be called with member locked.
static struct tris_frame *get_next_soundframe(struct tris_conf_member *member, struct tris_frame
    *exampleframe) {
    struct tris_frame *f;

again:
    f=tris_readframe(member->soundq->stream);

    if(!f) { // we're done with this sound; remove it from the queue, and try again
	struct tris_conf_soundq *toboot = member->soundq;

	tris_closestream(toboot->stream);
	member->soundq = toboot->next;

	//tris_log( LOG_WARNING, "finished playing a sound, next = %x\n", member->soundq);
	// notify applications via mgr interface that this sound has been played
	manager_event(
		EVENT_FLAG_CALL,
		"ConferenceSoundComplete",
		"Channel: %s\r\n"
		"Sound: %s\r\n",
		member->channel_name,
		toboot->name
	);

	free(toboot);
	if(member->soundq) goto again;

	// if we get here, we've gotten to the end of the queue; reset write format
	if ( tris_set_write_format( member->chan, member->write_format ) < 0 )
	{
		tris_log( LOG_ERROR, "unable to set write format to %d\n",
		    member->write_format ) ;
	}
    } else {
	// copy delivery from exampleframe
	f->delivery = exampleframe->delivery;
    }

    return f;
}


// process outgoing frames for the channel, playing either normal conference audio,
// or requested sounds
static int process_outgoing(struct tris_conf_member *member)
{
	conf_frame* cf ; // frame read from the output queue
	struct tris_frame *f;
	struct tris_frame *realframe = NULL;

	for(;;)
	{
		// acquire member mutex and grab a frame.
		tris_mutex_lock( &member->lock ) ;
		cf = get_outgoing_frame( member ) ;

                // if there's no frames exit the loop.
		if ( !cf )
		{
			tris_mutex_unlock( &member->lock ) ;
			break;
		}


		f = cf->fr;

		// if we're playing sounds, we can just replace the frame with the
		// next sound frame, and send it instead
		if ( member->soundq )
		{
			realframe = f;
			f = get_next_soundframe(member, f);
			if ( !f )
			{
				// if we didn't get anything, just revert to "normal"
				f = realframe;
				realframe = NULL;
			} else
			{
				// We have a sound frame now, but we need to make sure it's the same
				// format as our channel write format
				int wf = member->chan->writeformat & TRIS_FORMAT_AUDIO_MASK;
				if ( f->frametype == TRIS_FRAME_VOICE && !(wf & f->subclass) )
				{
					// We need to change our channel's write format
					tris_set_write_format(member->chan, f->subclass);
				}
			}
		}

		tris_mutex_unlock(&member->lock);


#ifdef DEBUG_FRAME_TIMESTAMPS
		// !!! TESTING !!!
		int delivery_diff = usecdiff( &f->delivery, &member->lastsent_timeval ) ;
		if ( delivery_diff != TRIS_CONF_FRAME_INTERVAL )
		{
			tris_log( TRIS_CONF_DEBUG, "unanticipated delivery time, delivery_diff => %d, delivery.tv_usec => %ld\n",
				 delivery_diff, f->delivery.tv_usec ) ;
		}

		// !!! TESTING !!!
		if (
			f->delivery.tv_sec < member->lastsent_timeval.tv_sec
			|| (
				f->delivery.tv_sec == member->lastsent_timeval.tv_sec
				&& f->delivery.tv_usec <= member->lastsent_timeval.tv_usec
				)
			)
		{
			tris_log( LOG_WARNING, "queued frame timestamped in the past, %ld.%ld <= %ld.%ld\n",
				 f->delivery.tv_sec, f->delivery.tv_usec,
				 member->lastsent_timeval.tv_sec, member->lastsent_timeval.tv_usec ) ;
		}
		member->lastsent_timeval = f->delivery ;
#endif

#ifdef DEBUG_USE_TIMELOG
		TIMELOG( tris_write( member->chan, f ), 10, "member: tris_write");
#else

		// send the voice frame
		if ( tris_write( member->chan, f ) == 0 )
		{
			struct timeval tv = tris_tvnow();
			tris_log( TRIS_CONF_DEBUG, "SENT VOICE FRAME, channel => %s, frames_out => %ld, s => %ld, ms => %ld\n",
				 member->channel_name, member->frames_out, tv.tv_sec, tv.tv_usec ) ;
		}
		else
		{
			// log 'dropped' outgoing frame
			tris_log( LOG_ERROR, "unable to write voice frame to channel, channel => %s\n", member->channel_name ) ;

			// accounting: count dropped outgoing frames
			member->frames_out_dropped++ ;
		}
#endif
		// clean up frame
		delete_conf_frame( cf ) ;

	}

	// Do the same for video, suck it dry
	for(;;)
	{
		// grab a frame.
		cf = get_outgoing_video_frame( member ) ;

                // if there's no frames exit the loop.
		if(!cf){
			break;
		}

		f = cf->fr;

		// send the video frame
		if ( tris_write_video( member->chan, f ) == 1 )
		{
			struct timeval tv = tris_tvnow();
			tris_log( TRIS_CONF_DEBUG, "SENT VIDEO FRAME, channel => %s, frames_out => %ld, s => %ld, ms => %ld\n",
				 member->channel_name, member->frames_out, tv.tv_sec, tv.tv_usec ) ;
		}
		else
		{
			// log 'dropped' outgoing frame
			tris_log( TRIS_CONF_DEBUG, "unable to write video frame to channel, channel => %s\n", member->channel_name ) ;

			// accounting: count dropped outgoing frames
			member->video_frames_out_dropped++ ;
		}

		// clean up frame
		delete_conf_frame( cf ) ;

	}

	// Do the same for desktop, suck it dry
	for(;;)
	{
		// grab a frame.
		cf = get_outgoing_desktop_frame( member ) ;

                // if there's no frames exit the loop.
		if(!cf){
			break;
		}

		f = cf->fr;

		// send the video frame
		if ( tris_write( member->chan, f ) == 1 )
		{
			struct timeval tv = tris_tvnow();
			tris_log( TRIS_CONF_DEBUG, "SENT VIDEO FRAME, channel => %s, frames_out => %ld, s => %ld, ms => %ld\n",
				 member->channel_name, member->desktop_frames_out, tv.tv_sec, tv.tv_usec ) ;
		}
		else
		{
			// log 'dropped' outgoing frame
			tris_log( TRIS_CONF_DEBUG, "unable to write video frame to channel, channel => %s\n", member->channel_name ) ;

			// accounting: count dropped outgoing frames
			member->desktop_frames_out_dropped++ ;
		}

		// clean up frame
		delete_conf_frame( cf ) ;

	}

        // Do the same for dtmf, suck it dry
	for(;;)
	{
		// acquire member mutex and grab a frame.
		cf = get_outgoing_dtmf_frame( member ) ;

		// if there's no frames exit the loop.
		if(!cf) break;

		// send the dtmf frame
		if ( tris_write( member->chan, cf->fr ) == 0 )
		{
			struct timeval tv = tris_tvnow();
			tris_log( TRIS_CONF_DEBUG, "SENT DTMF FRAME, channel => %s, frames_out => %ld, s => %ld, ms => %ld\n",
				 member->channel_name, member->frames_out, tv.tv_sec, tv.tv_usec ) ;

		}
		else
		{
			// log 'dropped' outgoing frame
			tris_log( TRIS_CONF_DEBUG, "unable to write dtmf frame to channel, channel => %s\n", member->channel_name ) ;

			// accounting: count dropped outgoing frames
			member->dtmf_frames_out_dropped++ ;
		}

		// clean up frame
		delete_conf_frame( cf ) ;
	}

        // Do the same for text, hell, why not?
	for(;;)
	{
		// acquire member mutex and grab a frame.
		cf = get_outgoing_text_frame( member ) ;

		// if there's no frames exit the loop.
		if(!cf) break;

		// send the text frame
		if ( tris_write( member->chan, cf->fr ) == 0 )
		{
			struct timeval tv = tris_tvnow();
			tris_log( TRIS_CONF_DEBUG, "SENT TEXT FRAME, channel => %s, frames_out => %ld, s => %ld, ms => %ld\n",
				 member->channel_name, member->frames_out, tv.tv_sec, tv.tv_usec ) ;

		}
		else
		{
			// log 'dropped' outgoing frame
			tris_log( TRIS_CONF_DEBUG, "unable to write text frame to channel, channel => %s\n", member->channel_name ) ;

			// accounting: count dropped outgoing frames
			member->text_frames_out_dropped++ ;
		}

		// clean up frame
		delete_conf_frame( cf ) ;
	}


	return 0;
}

static int member_checkkick( struct tris_conf_member *member )
{
	int kick;
	tris_mutex_lock( &member->lock ) ;
	kick = member->kick_flag;
	tris_mutex_unlock( &member->lock ) ;
	return kick;
}

static void member_mute_norecv_and_backup(struct tris_conf_member *member) 
{
	member->backup_norecv_audio = member->norecv_audio;
	member->backup_mute_audio = member->mute_audio;
	member->norecv_audio = 1;
	member->mute_audio = 1;
}

static void member_restore_mute_norecv(struct tris_conf_member *member) 
{
	member->norecv_audio = member->backup_norecv_audio;
	member->mute_audio = member->backup_mute_audio;
}

static struct tris_channel* dial_out(struct tris_conf_member *member, struct tris_dial **dials, int *pos, const char* conf_name, const char* data, const char *addin_flags, const char *cancel_dtmfs)
{
	struct tris_dial *dial;
	enum tris_dial_result dial_res;
	struct in_addr ourip;
	struct sockaddr_in bindaddr;
	char calling_uri[100], conf_opts[88];
	char *tech_data = calling_uri;
	char *tech;
	struct tris_channel *callee_chan = NULL, *chan;
	unsigned int done = 0, res;

	if (!data || strlen(data) == 0)
		return NULL;
	
	chan = member->chan;

	snprintf(conf_opts, sizeof(conf_opts), "Conference,%s/%s", conf_name, addin_flags);
	
	memset(&bindaddr, 0, sizeof(struct sockaddr_in));
	tris_find_ourip(&ourip, bindaddr);
	snprintf(calling_uri, sizeof(calling_uri), "SIP/%s@%s:5060", data, tris_inet_ntoa(ourip));

	if (!(dial = tris_dial_create())) {
		return NULL;
	}	
	
	tech = strsep(&tech_data, "/");
	if (tris_dial_append(dial, tech, tech_data) == -1) {
		tris_dial_destroy(dial);
		return NULL;
	}	
	/* Set ANSWER_EXEC as global option */
	tris_dial_option_global_enable(dial, TRIS_DIAL_OPTION_ANSWER_EXEC, conf_opts);
	/* Run this dial in async mode */
	dial_res = tris_dial_run(dial, chan, 1, 0);
	
	if (dial_res != TRIS_DIAL_RESULT_TRYING) {
		tris_dial_destroy(dial);
		return NULL;
	}

	member_mute_norecv_and_backup(member);	

	//tris_indicate(chan, TRIS_CONTROL_RINGING);
	tris_streamfile(chan, "conference/ringing", chan->language);
	while(!done) {
		dial_res = tris_dial_state(dial);
		switch (dial_res) {
		case TRIS_DIAL_RESULT_ANSWERED:
			callee_chan = tris_dial_answered(dial);
			done = 1;
			break;
		case TRIS_DIAL_RESULT_BUSY:
		case TRIS_DIAL_RESULT_CONGESTION:
			tris_play_and_wait(chan, "conference/pbx-busy");
			done = 1;
			break;
		case TRIS_DIAL_RESULT_FORBIDDEN:
			tris_play_and_wait(chan, "conference/pbx-forbidden");
			done = 1;
			break;
		case TRIS_DIAL_RESULT_OFFHOOK:
			tris_play_and_wait(chan, "conference/pbx-not-found");
			done = 1;
			break;
		case TRIS_DIAL_RESULT_TAKEOFFHOOK:
			tris_play_and_wait(chan, "conference/pbx-not-registered");
			done = 1;
			break;
		case TRIS_DIAL_RESULT_TIMEOUT:
			tris_play_and_wait(chan, "conference/pbx-no-answer");
			done = 1;
			break;
		case TRIS_DIAL_RESULT_HANGUP:
		case TRIS_DIAL_RESULT_INVALID:
		case TRIS_DIAL_RESULT_FAILED:
		case TRIS_DIAL_RESULT_UNANSWERED:
			done = 1;
			break;
		default:
			break;
		}
		res = tris_waitfordigit(chan,10);
		if(res && strchr(cancel_dtmfs, res)) {
			tris_dial_join(dial);
			tris_dial_destroy(dial);
			tris_stopstream(chan);
			tris_play_and_wait(chan, "conference/calling-cancelled");
			member_restore_mute_norecv(member);
			return NULL;
		}
	}
	tris_stopstream(chan);
	if (!callee_chan) {
		tris_dial_join(dial);
		tris_dial_destroy(dial);
		member_restore_mute_norecv(member);
		return NULL;
	}
	member_restore_mute_norecv(member);
	dials[(*pos)++] = dial;

	return callee_chan;
}

//
// main member thread function
//
int member_exec( struct tris_channel* chan, void* data )
{
//	struct timeval start, end ;
//	start = tris_tvnow();

	struct tris_conference *conf ;
	struct tris_conf_member *member ;
	char ext_buf[88] = "";
	struct tris_dial *dial_list[128];
	int kick = 0;

	struct tris_frame *f ; // frame received from tris_read()

	int left = 0 ;
	int res;
	int pos = 0;
	unsigned int i;
	char sql[256], admin_telnum[80];
	char subjectbuf[81];

	tris_log( TRIS_CONF_DEBUG, "Begin processing member thread, channel => %s\n", chan->name ) ;

	//
	// If the call has not yet been answered, answer the call
	// Note: trismedia apps seem to check _state, but it seems like it's safe
	// to just call tris_answer.  It will just do nothing if it is up.
	// it will also return -1 if the channel is a zombie, or has hung up.
	//

	res = tris_answer( chan ) ;
	if ( res )
	{
		tris_log( LOG_ERROR, "unable to answer call\n" ) ;
		return -1 ;
	}

	//
	// create a new member for the conference
 	//

//	tris_log( TRIS_CONF_DEBUG, "creating new member, id => %s, flags => %s, p => %s\n",
//		id, flags, priority ) ;
	
	member = create_member( chan, (const char*)( data ) ) ; // flags, atoi( priority ) ) ;

	// unable to create member, return an error
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to create member\n" ) ;
		return -1 ;
	}

	if(member->conf_type != CONFERENCE_URGENCY_CONF && !member->is_admin) {
		snprintf(sql, sizeof(sql), "SELECT sponseruid FROM videoconf_room WHERE roomno='%s'", member->conf_name);
		sql_select_query_execute(admin_telnum, sql);
		snprintf(subjectbuf, sizeof(subjectbuf), "subject: %s", member->conf_name);
		struct tris_app *the_app;
		the_app = pbx_findapp("SIPAddHeader");
		if (the_app)
			pbx_exec(chan, the_app, subjectbuf);
		/* I don't need to call the admin on conference. so I comment below code blocks */
		/*struct tris_channel* callee_chan = dial_out(member, dial_list, &pos, member->conf_name, admin_telnum, "aCLl", "");
		// copy the channel name
		if(callee_chan) {
			member->receiver_channel_name = malloc( strlen( callee_chan->name ) + 1 ) ;
			strcpy( member->receiver_channel_name, callee_chan->name ) ;
			tris_verbose("  --  %s\n", member->receiver_channel_name);
		}
		if(member->is_dialouted) {
			tris_stream_and_wait(chan, "conference/you-now", "");
			tris_say_digit_str(chan, member->conf_name + 5, "", chan->language);
			tris_stream_and_wait(chan, "conference/entering-conf", "");
			member->is_dialouted = 0;
		} else {
		
		}*/
	}
	
	
	/*res = tris_streamfile(chan, "beep", chan->language);
	if (!res)
		res = tris_waitstream(chan, "");*/
	
	//
	// setup trismedia read/write formats
	//
#if 0
	tris_log( TRIS_CONF_DEBUG, "CHANNEL INFO, CHANNEL => %s, DNID => %s, CALLER_ID => %s, ANI => %s\n",
		chan->name, chan->dnid, chan->callerid, chan->ani ) ;

	tris_log( TRIS_CONF_DEBUG, "CHANNEL CODECS, CHANNEL => %s, NATIVE => %d, READ => %d, WRITE => %d\n",
		chan->name, chan->nativeformats, member->read_format, member->write_format ) ;
#endif
	if ( tris_set_read_format( chan, member->read_format ) < 0 )
	{
		tris_log( LOG_ERROR, "unable to set read format to signed linear\n" ) ;
		delete_member( member ) ;
		return -1 ;
	}

	if ( tris_set_write_format( chan, member->write_format ) < 0 ) // TRIS_FORMAT_SLINEAR, chan->nativeformats
	{
		tris_log( LOG_ERROR, "unable to set write format to signed linear\n" ) ;
		delete_member( member ) ;
		return -1 ;
	}

	//
	// setup a conference for the new member
	//

	conf = start_conference( member ) ;

	if ( conf == NULL )
	{
		tris_log( LOG_ERROR, "unable to setup member conference\n" ) ;
		delete_member( member) ;
		return -1 ;
	}


	manager_event(
		EVENT_FLAG_CALL,
		"ConferenceJoin",
		"ConferenceName: %s\r\n"
		"Member: %d\r\n"
		"Channel: %s\r\n"
		"CallerID: %s\r\n"
		"CallerIDName: %s\r\n"
		"Count: %d\r\n",
		conf->name,
		member->id,
		member->channel_name,
		member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
		member->chan->cid.cid_name ? member->chan->cid.cid_name: "unknown",
		conf->membercount
	) ;

	// Store the CID information
	if ( member->chan->cid.cid_num )
	{
		if ( (member->callerid = malloc(strlen(member->chan->cid.cid_num)+1)) )
			memcpy(member->callerid,member->chan->cid.cid_num, strlen(member->chan->cid.cid_num)+1);
	} else
		member->callerid = NULL;

	if ( member->chan->cid.cid_name )
	{
		if ( (member->callername = malloc(strlen(member->chan->cid.cid_name)+1)) )
			memcpy(member->callername, member->chan->cid.cid_name, strlen(member->chan->cid.cid_name)+1);
	} else
		member->callername = NULL;

	// set the id of default video source, if member's callerid is same as sponser
	if(member->callerid && !strcmp(admin_telnum, member->callerid)){
		// below 2 lines need for validating set_default_id()...
		member->mute_video = 0;
		member->vad_switch = 1;
		
		tris_log(LOG_DEBUG, "yz: set default video source to '%s'\n", member->callerid);
		set_default_id(member->conf_name, member->id);
	}

	// set the conference to which member belongs
	if(member->conf_name)
		member->belongto = find_conf(member->conf_name);

	//
	// process loop for new member ( this runs in it's own thread )
	//

	tris_log( TRIS_CONF_DEBUG, "begin member event loop, channel => %s\n", chan->name ) ;

	// timer timestamps
	struct timeval base, curr ;
	base = tris_tvnow();

	// tell conference_exec we're ready for frames
	member->ready_for_outgoing = 1 ;
	while ( 42 == 42 )
	{
		// make sure we have a channel to process
		if ( chan == NULL )
		{
			tris_log( LOG_NOTICE, "member channel has closed\n" ) ;
			break ;
		}

		//-----------------//
		// INCOMING FRAMES //
		//-----------------//

		// wait for an event on this channel
		left = tris_waitfor( chan, TRIS_CONF_WAITFOR_LATENCY ) ;

		//tris_log( TRIS_CONF_DEBUG, "received event on channel, name => %s, left => %d\n", chan->name, left ) ;

		if ( left < 0 )
		{
			// an error occured
			tris_log(
				LOG_NOTICE,
				"an error occured waiting for a frame, channel => %s, error => %d\n",
				chan->name, left
			) ;
			break; // out of the 42==42
		}
		else if ( left == 0 )
		{
			// no frame has arrived yet
			// tris_log( LOG_NOTICE, "no frame available from channel, channel => %s\n", chan->name ) ;
		}
		else if ( left > 0 )
		{
			// a frame has come in before the latency timeout
			// was reached, so we process the frame

			f = tris_read( chan ) ;

			if ( f == NULL )
			{
				if (conf->debug_flag)
				{
					tris_log( LOG_NOTICE, "unable to read from channel, channel => %s\n", chan->name ) ;
				// They probably want to hangup...
				}
				break ;
			}

			// actually process the frame: break if we got hangup.
			if(process_incoming(member, conf, f)) break;

			if (member->ismoderator && f->frametype == TRIS_FRAME_DTMF)
			{
				
				if(f->subclass == '#') {
					
					res = tris_app_getdata(chan, "conference/dial_extn_pound", ext_buf, sizeof(ext_buf) - 1, 7000);
					
					if(!tris_strlen_zero(ext_buf)) {
						
						if(find_member2(member->conf_name, ext_buf, 0) != NULL) {
							tris_play_and_wait(chan, "conference/already_existing");
							member_restore_mute_norecv(member);
							continue;
						}

						if (member->conf_type & CONFERENCE_URGENCY_CONF)
							dial_out(member, dial_list, &pos, member->conf_name, ext_buf, "Ud", "*");
						else
							dial_out(member, dial_list, &pos, member->conf_name, ext_buf, "d", "*");

					}
				} else if(f->subclass == '*') {
					member_mute_norecv_and_backup(member);
					res = tris_streamfile(chan, "conference/dial_extn_star", chan->language);
					if (res < 0) {
						member_restore_mute_norecv(member);
						continue;
					}
					res = tris_readstring(chan, ext_buf, sizeof(ext_buf) - 1, 7000, 5000, "*");
					if(!tris_strlen_zero(ext_buf)) {
						res = kick_channel(member->conf_name, ext_buf);
						if(!res) {
							tris_play_and_wait(chan, "conference/not_found_user");
						}
					}
					member_restore_mute_norecv(member);
				} else if(f->subclass == '1') {
					manager_event(EVENT_FLAG_CALL, "ConferenceRequestRight", 
						"Channel: %s\r\n"
						"Uniqueid: %s\r\n"
						"Confno: %s\r\n"
						"UserID: %i\r\n",
						chan->name, chan->uniqueid, conf->name, member->id);
				}
				
			}

		}

		kick = member_checkkick(member);
		if (kick) {
			member_mute_norecv_and_backup(member);
			if (kick == 1) {
				tris_play_and_wait(chan, "conference/you-are-kicked");
			} else if (kick == 2) {
				tris_play_and_wait(chan, "conference/end_conf");
			}
			member_restore_mute_norecv(member);
			break;
		}

		//-----------------//
		// OUTGOING FRAMES //
		//-----------------//

		// update the current timestamps
		curr = tris_tvnow();

		process_outgoing(member);
		// back to process incoming frames
		continue ;
	}

	tris_log( TRIS_CONF_DEBUG, "end member event loop, time_entered => %ld\n", member->time_entered.tv_sec ) ;

	//
	// clean up
	//

#ifdef DEBUG_OUTPUT_PCM
	// !!! TESTING !!!
	if ( incoming_fh != NULL )
		fclose( incoming_fh ) ;
#endif

	tris_mutex_lock( &member->lock ) ;
	if(member->belongto != NULL && member->belongto->default_video_source_id == member->id){
		tris_log(LOG_DEBUG, "yz: We found bye for desktop sharing on speaker. Bye all others.\n");
		end_conference(member->conf_name, 1);
	}else{
		tris_log(LOG_DEBUG, "yz: We notify others that member is out.\n");
		if (!member->isbargein3conf)
			notify_members(conf, member, 2);
	}
	tris_mutex_unlock( &member->lock ) ;
	

	// If we're driving another member, make sure its speaker count is correct
	if ( member != NULL ) member->remove_flag = 1 ;

	if (member && member->ismoderator)
		sleep(2);
	for (i = 0; i < pos; i++) {
		struct tris_dial *dial = dial_list[i];

		if (!dial)
			continue;
		/* We have to wait for the async thread to exit as it's possible Meetme won't throw them out immediately */
		tris_dial_join(dial);

		/* Hangup all channels */
		tris_dial_hangup(dial);

		/* Destroy dialing structure */
		tris_dial_destroy(dial);
	}
	
//	end = tris_tvnow();
//	int expected_frames = ( int )( floor( (double)( msecdiff( &end, &start ) / TRIS_CONF_FRAME_INTERVAL ) ) ) ;
//	tris_log( TRIS_CONF_DEBUG, "expected_frames => %d\n", expected_frames ) ;

	return 0 ;
}



struct tris_conf_member *check_active_video( int id, struct tris_conference *conf )
{
     struct tris_conf_member *member;

     // acquire the conference lock
     tris_mutex_lock( &conf->lock ) ;

     member = conf->memberlist;
     while (member)
     {
	     if (member->id == id)
	     {
		     // lock this member
		     tris_mutex_lock( &member->lock ) ;
	      	     tris_mutex_unlock( &conf->lock ) ;
		     return member;
	     }
	     member = member->next;
     }
     tris_mutex_unlock( &conf->lock ) ;
     return NULL;
}

//
// manange member functions
//

struct tris_conf_member* create_member( struct tris_channel *chan, const char* data )
{
	//
	// check input
	//

	if ( chan == NULL )
	{
		tris_log( LOG_ERROR, "unable to create member with null channel\n" ) ;
		return NULL ;
	}

	if ( chan->name == NULL )
	{
		tris_log( LOG_ERROR, "unable to create member with null channel name\n" ) ;
		return NULL ;
	}

	//
	// allocate memory for new conference member
	//

	struct tris_conf_member *member = calloc( 1,  sizeof( struct tris_conf_member ) ) ;

	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to malloc tris_conf_member\n" ) ;
		return NULL ;
	}

	// initialize mutex
	tris_mutex_init( &member->lock ) ;

	//
	// initialize member with passed data values
	//

	char argstr[80] ;
	char *stringp, *token, *confname ;

	member->isbargein3conf = 0;
	// copy the passed data
	strncpy( argstr, data, sizeof(argstr) - 1 ) ;
	if (!strlen(argstr) && (confname = pbx_builtin_getvar_helper(chan, "Conf-Name"))) {
		strncpy(argstr, confname, sizeof(argstr)-1);
		member->isbargein3conf = 1;
	}

	// point to the copied data
	stringp = argstr ;

	tris_log( TRIS_CONF_DEBUG, "attempting to parse passed params, stringp => %s\n", stringp ) ;

	// parse the id
	if ( ( token = strsep( &stringp, "/" ) ) != NULL )
	{
		member->conf_name = malloc( strlen( token ) + 1 ) ;
		strcpy( member->conf_name, token ) ;
	}
	else
	{
		tris_log( LOG_ERROR, "unable to parse member id\n" ) ;
		free( member ) ;
		return NULL ;
	}

	// parse the flags
	if ( ( token = strsep( &stringp, "/" ) ) != NULL )
	{
		member->flags = malloc( strlen( token ) + 1 ) ;
		strcpy( member->flags, token ) ;
	}
	else
	{
		// make member->flags something
		member->flags = malloc( sizeof( char ) ) ;
		memset( member->flags, 0x0, sizeof( char ) ) ;
	}

	// parse the priority
	member->priority = ( token = strsep( &stringp, "/" ) ) != NULL
		? atoi( token )
		: 0
	;

	// parse the vad_prob_start
	member->vad_prob_start = ( token = strsep( &stringp, "/" ) ) != NULL
		? atof( token )
		: TRIS_CONF_PROB_START
	;

	// parse the vad_prob_continue
	member->vad_prob_continue = ( token = strsep( &stringp, "/" ) ) != NULL
		? atof( token )
		: TRIS_CONF_PROB_CONTINUE
	;

	// debugging
	tris_log(
		TRIS_CONF_DEBUG,
		"parsed data params, id => %s, flags => %s, priority => %d, vad_prob_start => %f, vad_prob_continue => %f\n",
		member->conf_name, member->flags, member->priority, member->vad_prob_start, member->vad_prob_continue
	) ;

	//
	// initialize member with default values
	//

	// keep pointer to member's channel
	member->chan = chan ;

	// copy the channel name
	member->channel_name = malloc( strlen( chan->name ) + 1 ) ;
	strcpy( member->channel_name, chan->name ) ;

	// video receiver channel name is NULL
	member->receiver_channel_name = NULL;

	// ( default can be overridden by passed flags )
	member->mute_audio = 0;
	member->mute_video = 0;
	member->norecv_audio = 0;
	member->norecv_video = 0;
	member->no_camera = 0;

	// moderator?
	member->ismoderator = 0;
	member->is_admin = 0;
	member->is_dialouted = 0;
	member->conf_type = 0;

	// ready flag
	member->ready_for_outgoing = 0 ;

	// incoming frame queue
	member->inFrames = NULL ;
	member->inFramesTail = NULL ;
	member->inFramesCount = 0 ;

	member->inVideoFrames = NULL ;
	member->inVideoFramesTail = NULL ;
	member->inVideoFramesCount = 0 ;

	member->inDTMFFrames = NULL ;
	member->inDTMFFramesTail = NULL ;
	member->inDTMFFramesCount = 0 ;

	member->inTextFrames = NULL ;
	member->inTextFramesTail = NULL ;
	member->inTextFramesCount = 0 ;

	member->conference = 1; // we have switched req_id
	member->dtmf_switch = 0; // no dtmf switch by default
	member->dtmf_relay = 0; // no dtmf relay by default
	member->desktop = 0; // we have no switched by default

	// start of day video ids
	member->req_id = -1;
	member->id = -1;

	member->first_frame_received = 0; // cause a FIR after NAT delay

	// last frame caching
	member->inFramesRepeatLast = 0 ;
	member->inFramesLast = NULL ;
	member->okayToCacheLast = 0 ;

	// outgoing frame queue
	member->outFrames = NULL ;
	member->outFramesTail = NULL ;
	member->outFramesCount = 0 ;

	member->outVideoFrames = NULL ;
	member->outVideoFramesTail = NULL ;
	member->outVideoFramesCount = 0 ;

	member->outDTMFFrames = NULL ;
	member->outDTMFFramesTail = NULL ;
	member->outDTMFFramesCount = 0 ;

	member->outTextFrames = NULL ;
	member->outTextFramesTail = NULL ;
	member->outTextFramesCount = 0 ;

	// ( not currently used )
	// member->samplesperframe = TRIS_CONF_BLOCK_SAMPLES ;

	// used for determining need to mix frames
	// and for management interface notification
	// and for VAD based video switching
	member->speaking_state_notify = 0 ;
	member->speaking_state = 0 ;
	member->local_speaking_state = 0;
	member->speaker_count = 0;
	member->driven_member = NULL;

	// linked-list pointer
	member->next = NULL ;

	// account data
	member->frames_in = 0 ;
	member->frames_in_dropped = 0 ;
	member->frames_out = 0 ;
	member->frames_out_dropped = 0 ;
	member->video_frames_in = 0 ;
	member->video_frames_in_dropped = 0 ;
	member->video_frames_out = 0 ;
	member->video_frames_out_dropped = 0 ;
	member->dtmf_frames_in = 0 ;
	member->dtmf_frames_in_dropped = 0 ;
	member->dtmf_frames_out = 0 ;
	member->dtmf_frames_out_dropped = 0 ;
	member->text_frames_in = 0 ;
	member->text_frames_in_dropped = 0 ;
	member->text_frames_out = 0 ;
	member->text_frames_out_dropped = 0 ;

	// for counting sequentially dropped frames
	member->sequential_drops = 0 ;
	member->since_dropped = 0 ;

	// flags
	member->remove_flag = 0 ;
	member->kick_flag = 0;

	// record start time
	// init dropped frame timestamps
	// init state change timestamp
	member->time_entered =
		member->ltris_in_dropped =
		member->ltris_out_dropped =
		member->ltris_state_change = tris_tvnow();

	//
	// parse passed flags
	//

	// silence detection flags w/ defaults
	member->vad_flag = 0 ;
	member->denoise_flag = 0 ;
	member->agc_flag = 0 ;

	// is this member using the telephone?
	member->via_telephone = 0 ;

	// temp pointer to flags string
	char* flags = member->flags ;

	int i;

	for ( i = 0 ; i < strlen( flags ) ; ++i )
	{

		if (flags[i] >= (int)'0' && flags[i] <= (int)'9')
		{
			if (member->req_id < 0)
			{
				member->req_id = flags[i] - (int)'0';
			}
			else
			{
				int newid = flags[i] - (int)'0';
				// need to boot anyone with this id already
				// will happen in add_member
				member->id = newid;
			}
		}
		else
		{
			// allowed flags are C, c, L, l, V, d, D, A, C, X, R, T, t, M, S
			// mute/no_recv options
			switch ( flags[i] )
			{
			case 'd':
				member->is_dialouted = 1;
				break;
			case 'C':
				member->mute_video = 1;
				break ;
			case 'c':
				member->norecv_video = 1;
				break ;
			case 'L':
				member->mute_audio = 1;
				break ;
			case 'l':
				member->norecv_audio = 1;
				break;

				// speex preprocessing options
			case 'V':
				member->vad_flag = 1 ;
				break ;
			case 'D':
				member->denoise_flag = 1 ;
				break ;
			case 'A':
				member->agc_flag = 1 ;
				break ;
			case 'a':
				member->is_admin = 1;
				break;

				// dtmf/moderator/video switching options
			case 'X':
				member->dtmf_switch = 1;
				break;
			case 'R':
				member->dtmf_relay = 1;
				break;
			case 'S':
				member->vad_switch = 1;
				break;
			case 'M':
				member->ismoderator = 1;
				break;
			case 'N':
				member->no_camera = 1;
				break;
			case 't':
				member->does_text = 1;
				break;

				//Telephone connection
			case 'T':
				member->via_telephone = 1;
				break;
			case 'U':
				member->conf_type = CONFERENCE_URGENCY_CONF;
				break;
			default:
				tris_log( LOG_WARNING, "received invalid flag, chan => %s, flag => %c\n",
					 chan->name, flags[i] );
				break ;
			}
		}
	}

	// set the dsp to null so silence detection is disabled by default
	member->dsp = NULL ;

#if ( SILDET == 2 )
	//
	// configure silence detection and preprocessing
	// if the user is coming in via the telephone,
	// and is not listen-only
	//
	if (
		member->via_telephone == 1
		&& member->type != 'L'
	)
	{
		// create a speex preprocessor
		member->dsp = speex_preprocess_state_init( TRIS_CONF_BLOCK_SAMPLES, TRIS_CONF_SAMPLE_RATE ) ;

		if ( member->dsp == NULL )
		{
			tris_log( LOG_WARNING, "unable to initialize member dsp, channel => %s\n", chan->name ) ;
		}
		else
		{
			tris_log( LOG_NOTICE, "member dsp initialized, channel => %s, v => %d, d => %d, a => %d\n",
				chan->name, member->vad_flag, member->denoise_flag, member->agc_flag ) ;

			// set speex preprocessor options
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_VAD, &(member->vad_flag) ) ;
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_DENOISE, &(member->denoise_flag) ) ;
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_AGC, &(member->agc_flag) ) ;

			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_PROB_START, &member->vad_prob_start ) ;
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &member->vad_prob_continue ) ;

			tris_log( TRIS_CONF_DEBUG, "speech_prob_start => %f, speech_prob_continue => %f\n",
				member->dsp->speech_prob_start, member->dsp->speech_prob_continue ) ;
		}
	}
#endif

	//
	// set connection type
	//

	if ( member->via_telephone == 1 )
	{
		member->connection_type = 'T' ;
	}
	else if ( strncmp( member->channel_name, "SIP", 3 ) == 0 )
	{
		member->connection_type = 'S' ;
	}
	else // default to iax
	{
		member->connection_type = 'X' ;
	}

	//
	// read, write, and translation options
	//

	// set member's audio formats, taking dsp preprocessing into account
	// ( chan->nativeformats, TRIS_FORMAT_SLINEAR, TRIS_FORMAT_ULAW, TRIS_FORMAT_GSM )
	member->read_format = ( member->dsp == NULL ) ? chan->nativeformats : TRIS_FORMAT_SLINEAR ;

	member->write_format = chan->nativeformats;

	// 1.2 or 1.3+
#ifdef TRIS_FORMAT_AUDIO_MASK

	member->read_format &= TRIS_FORMAT_AUDIO_MASK;
	member->write_format &= TRIS_FORMAT_AUDIO_MASK;
#endif

	// translation paths ( tris_translator_build_path() returns null if formats match )
	member->to_slinear = tris_translator_build_path( TRIS_FORMAT_SLINEAR, member->read_format ) ;
	member->from_slinear = tris_translator_build_path( member->write_format, TRIS_FORMAT_SLINEAR ) ;

	tris_log( TRIS_CONF_DEBUG, "TRIS_FORMAT_SLINEAR => %d\n", TRIS_FORMAT_SLINEAR ) ;

	// index for converted_frames array
	switch ( member->write_format )
	{
		case TRIS_FORMAT_SLINEAR:
			member->write_format_index = AC_SLINEAR_INDEX ;
			break ;

		case TRIS_FORMAT_ULAW:
			member->write_format_index = AC_ULAW_INDEX ;
			break ;

	        case TRIS_FORMAT_ALAW:
			member->write_format_index = AC_ALAW_INDEX ;
			break ;

		case TRIS_FORMAT_GSM:
			member->write_format_index = AC_GSM_INDEX ;
			break ;

		case TRIS_FORMAT_SPEEX:
			member->write_format_index = AC_SPEEX_INDEX;
			break;

#ifdef AC_USE_G729A
		case TRIS_FORMAT_G729A:
			member->write_format_index = AC_G729A_INDEX;
			break;
#endif

		default:
			member->write_format_index = 0 ;
	}

	// index for converted_frames array
	switch ( member->read_format )
	{
		case TRIS_FORMAT_SLINEAR:
			member->read_format_index = AC_SLINEAR_INDEX ;
			break ;

		case TRIS_FORMAT_ULAW:
			member->read_format_index = AC_ULAW_INDEX ;
			break ;

		case TRIS_FORMAT_ALAW:
			member->read_format_index = AC_ALAW_INDEX ;
			break ;

		case TRIS_FORMAT_GSM:
			member->read_format_index = AC_GSM_INDEX ;
			break ;

		case TRIS_FORMAT_SPEEX:
			member->read_format_index = AC_SPEEX_INDEX;
			break;

#ifdef AC_USE_G729A
		case TRIS_FORMAT_G729A:
			member->read_format_index = AC_G729A_INDEX;
			break;
#endif

		default:
			member->read_format_index = 0 ;
	}

	// smoother defaults.
	member->smooth_multiple =1;
	member->smooth_size_in = -1;
	member->smooth_size_out = -1;
	member->inSmoother= NULL;
	member->outPacker= NULL;

	switch (member->read_format){
		/* these assumptions may be incorrect */
		case TRIS_FORMAT_ULAW:
		case TRIS_FORMAT_ALAW:
			member->smooth_size_in  = 160; //bytes
			member->smooth_size_out = 160; //samples
			break;
		case TRIS_FORMAT_GSM:
			/*
			member->smooth_size_in  = 33; //bytes
			member->smooth_size_out = 160;//samples
			*/
			break;
		case TRIS_FORMAT_SPEEX:
		case TRIS_FORMAT_G729A:
			/* this assumptions are wrong
			member->smooth_multiple = 2 ;  // for testing, force to dual frame
			member->smooth_size_in  = 39;  // bytes
			member->smooth_size_out = 160; // samples
			*/
			break;
		case TRIS_FORMAT_SLINEAR:
			member->smooth_size_in  = 320; //bytes
			member->smooth_size_out = 160; //samples
			break;
		default:
			member->inSmoother = NULL; //don't use smoother for this type.
			//tris_log( TRIS_CONF_DEBUG, "smoother is NULL for member->read_format => %d\n", member->read_format);
	}

	if (member->smooth_size_in > 0){
		member->inSmoother = tris_smoother_new(member->smooth_size_in);
		tris_log( TRIS_CONF_DEBUG, "created smoother(%d) for %d\n", member->smooth_size_in , member->read_format);
	}

	//
	// finish up
	//

	tris_log( TRIS_CONF_DEBUG, "created member, type => %c, priority => %d, readformat => %d\n",
		member->type, member->priority, chan->readformat ) ;

	return member ;
}

struct tris_conf_member* delete_member( struct tris_conf_member* member )
{
	// !!! NO RETURN TEST !!!
	// do { sleep(1) ; } while (1) ;

	// !!! CRASH TEST !!!
	// *((int *)0) = 0;

	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to the delete null member\n" ) ;
		return NULL ;
	}

	tris_mutex_lock(&member->lock);

	// If member is driving another member, make sure its speaker count is correct
	if ( member->driven_member != NULL && member->speaking_state == 1 )
		decrement_speaker_count(member->driven_member, 1);

	//
	// clean up member flags
	//

	if ( member->flags != NULL )
	{
		// !!! DEBUGING !!!
		tris_log( TRIS_CONF_DEBUG, "freeing member flags, name => %s\n",
			member->channel_name ) ;
		free( member->flags ) ;
	}

	//
	// delete the members frames
	//

	conf_frame* cf ;

	// !!! DEBUGING !!!
	tris_log( TRIS_CONF_DEBUG, "deleting member input frames, name => %s\n",
		member->channel_name ) ;

	// incoming frames
	cf = member->inFrames ;

	while ( cf != NULL )
	{
		cf = delete_conf_frame( cf ) ;
	}

	if (member->inSmoother != NULL)
		tris_smoother_free(member->inSmoother);

	cf = member->inVideoFrames ;

	while ( cf != NULL )
	{
		cf = delete_conf_frame( cf ) ;
	}

	// !!! DEBUGING !!!
	tris_log( TRIS_CONF_DEBUG, "deleting member output frames, name => %s\n",
		member->channel_name ) ;

	// outgoing frames
	cf = member->outFrames ;

	while ( cf != NULL )
	{
		cf = delete_conf_frame( cf ) ;
	}

	cf = member->outVideoFrames ;

	while ( cf != NULL )
	{
		cf = delete_conf_frame( cf ) ;
	}

#if ( SILDET == 2 )
	if ( member->dsp != NULL )
	{
		// !!! DEBUGING !!!
		tris_log( TRIS_CONF_DEBUG, "destroying member preprocessor, name => %s\n",
			member->channel_name ) ;
		speex_preprocess_state_destroy( member->dsp ) ;
	}
#endif

	// !!! DEBUGING !!!
	tris_log( TRIS_CONF_DEBUG, "freeing member translator paths, name => %s\n",
		member->channel_name ) ;

	// free the mixing translators
	tris_translator_free_path( member->to_slinear ) ;
	tris_translator_free_path( member->from_slinear ) ;

	// get a pointer to the next
	// member so we can return it
	struct tris_conf_member* nm = member->next ;

	tris_mutex_unlock(&member->lock);

	// !!! DEBUGING !!!
	tris_log( TRIS_CONF_DEBUG, "freeing member channel name, name => %s\n",
		member->channel_name ) ;

	// free the member's copy for the channel name
	free( member->channel_name ) ;

	// free the member's copy of the conference name
	free(member->conf_name);

	// !!! DEBUGING !!!
	tris_log( TRIS_CONF_DEBUG, "freeing member\n" ) ;

	// free the member's memory
	if(member->callerid) free(member->callerid);
	if(member->callername) free(member->callername);

	free( member ) ;
	member = NULL ;

	return nm ;
}

//
// incoming frame functions
//

conf_frame* get_incoming_video_frame( struct tris_conf_member *member )
{
  	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	tris_mutex_lock(&member->lock);

	if ( member->inVideoFramesCount == 0 )
	{
		tris_mutex_unlock(&member->lock);
		return NULL ;
	}

	//
	// return the next frame in the queue
	//

	conf_frame* cfr = NULL ;

	// get first frame in line
	cfr = member->inVideoFramesTail ;

	// if it's the only frame, reset the queue,
	// else, move the second frame to the front
	if ( member->inVideoFramesTail == member->inVideoFrames )
	{
		member->inVideoFramesTail = NULL ;
		member->inVideoFrames = NULL ;
	}
	else
	{
		// move the pointer to the next frame
		member->inVideoFramesTail = member->inVideoFramesTail->prev ;

		// reset it's 'next' pointer
		if ( member->inVideoFramesTail != NULL )
			member->inVideoFramesTail->next = NULL ;
	}

	// separate the conf frame from the list
	cfr->next = NULL ;
	cfr->prev = NULL ;

	// decrement frame count
	member->inVideoFramesCount-- ;

	tris_mutex_unlock(&member->lock);
	return cfr ;

}
conf_frame* get_incoming_desktop_frame( struct tris_conf_member *member )
{
  	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	tris_mutex_lock(&member->lock);

	if ( member->inDesktopFramesCount == 0 )
	{
		tris_mutex_unlock(&member->lock);
		return NULL ;
	}

	//
	// return the next frame in the queue
	//

	conf_frame* cfr = NULL ;

	// get first frame in line
	cfr = member->inDesktopFramesTail ;

	// if it's the only frame, reset the queue,
	// else, move the second frame to the front
	if ( member->inDesktopFramesTail == member->inDesktopFrames )
	{
		member->inDesktopFramesTail = NULL ;
		member->inDesktopFrames = NULL ;
	}
	else
	{
		// move the pointer to the next frame
		member->inDesktopFramesTail = member->inDesktopFramesTail->prev ;

		// reset it's 'next' pointer
		if ( member->inDesktopFramesTail != NULL )
			member->inDesktopFramesTail->next = NULL ;
	}

	// separate the conf frame from the list
	cfr->next = NULL ;
	cfr->prev = NULL ;

	// decrement frame count
	member->inDesktopFramesCount-- ;

	tris_mutex_unlock(&member->lock);
	return cfr ;

}
conf_frame* get_incoming_dtmf_frame( struct tris_conf_member *member )
{
  	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	tris_mutex_lock(&member->lock);

	if ( member->inDTMFFramesCount == 0 )
	{
		tris_mutex_unlock(&member->lock);
		return NULL ;
	}

	//
	// return the next frame in the queue
	//

	conf_frame* cfr = NULL ;

	// get first frame in line
	cfr = member->inDTMFFramesTail ;

	// if it's the only frame, reset the queue,
	// else, move the second frame to the front
	if ( member->inDTMFFramesTail == member->inDTMFFrames )
	{
		member->inDTMFFramesTail = NULL ;
		member->inDTMFFrames = NULL ;
	}
	else
	{
		// move the pointer to the next frame
		member->inDTMFFramesTail = member->inDTMFFramesTail->prev ;

		// reset it's 'next' pointer
		if ( member->inDTMFFramesTail != NULL )
			member->inDTMFFramesTail->next = NULL ;
	}

	// separate the conf frame from the list
	cfr->next = NULL ;
	cfr->prev = NULL ;

	// decriment frame count
	member->inDTMFFramesCount-- ;

	tris_mutex_unlock(&member->lock);
	return cfr ;

}


conf_frame* get_incoming_frame( struct tris_conf_member *member )
{
	conf_frame *cf_result;
	//
	// sanity checks
	//

	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	tris_mutex_lock(&member->lock);

 	//
 	// repeat last frame a couple times to smooth transition
 	//

#ifdef TRIS_CONF_CACHE_LTRIS_FRAME
	if ( member->inFramesCount == 0 )
	{
		// nothing to do if there's no cached frame
		if ( member->inFramesLast == NULL ) {
			tris_mutex_unlock(&member->lock);
			return NULL ;
		}

		// turn off 'okay to cache' flag
		member->okayToCacheLast = 0 ;

		if ( member->inFramesRepeatLast >= TRIS_CONF_CACHE_LTRIS_FRAME )
		{
			// already used this frame TRIS_CONF_CACHE_LTRIS_FRAME times

			// reset repeat count
			member->inFramesRepeatLast = 0 ;

			// clear the cached frame
			delete_conf_frame( member->inFramesLast ) ;
			member->inFramesLast = NULL ;

			// return null
			tris_mutex_unlock(&member->lock);
			return NULL ;
		}
		else
		{
			tris_log( TRIS_CONF_DEBUG, "repeating cached frame, channel => %s, inFramesRepeatLast => %d\n",
				member->channel_name, member->inFramesRepeatLast ) ;

			// increment counter
			member->inFramesRepeatLast++ ;

			// return a copy of the cached frame
			cf_result = copy_conf_frame( member->inFramesLast ) ;
			tris_mutex_unlock(&member->lock);
			return cf_result;
		}
	}
	else if ( member->okayToCacheLast == 0 && member->inFramesCount >= 3 )
	{
		tris_log( TRIS_CONF_DEBUG, "enabling cached frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inFramesCount, member->outFramesCount ) ;

		// turn on 'okay to cache' flag
		member->okayToCacheLast = 1 ;
	}
#else
	if ( member->inFramesCount == 0 ) {
		tris_mutex_unlock(&member->lock);
		return NULL ;
	}
#endif // TRIS_CONF_CACHE_LTRIS_FRAME

	//
	// return the next frame in the queue
	//

	conf_frame* cfr = NULL ;

	// get first frame in line
	cfr = member->inFramesTail ;

	// if it's the only frame, reset the queue,
	// else, move the second frame to the front
	if ( member->inFramesTail == member->inFrames )
	{
		member->inFramesTail = NULL ;
		member->inFrames = NULL ;
	}
	else
	{
		// move the pointer to the next frame
		member->inFramesTail = member->inFramesTail->prev ;

		// reset it's 'next' pointer
		if ( member->inFramesTail != NULL )
			member->inFramesTail->next = NULL ;
	}

	// separate the conf frame from the list
	cfr->next = NULL ;
	cfr->prev = NULL ;

	// decriment frame count
	member->inFramesCount-- ;

#ifdef TRIS_CONF_CACHE_LTRIS_FRAME
	// copy frame if queue is now empty
	if (
		member->inFramesCount == 0
		&& member->okayToCacheLast == 1
	)
	{
		// reset repeat count
		member->inFramesRepeatLast = 0 ;

		// clear cached frame
		if ( member->inFramesLast != NULL )
		{
			delete_conf_frame( member->inFramesLast ) ;
			member->inFramesLast = NULL ;
		}

		// cache new frame
		member->inFramesLast = copy_conf_frame( cfr ) ;
	}
#endif // TRIS_CONF_CACHE_LTRIS_FRAME

	tris_mutex_unlock(&member->lock);
	return cfr ;
}

int queue_incoming_video_frame( struct tris_conf_member* member, const struct tris_frame* fr )
{
	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	// lock the member
	tris_mutex_lock(&member->lock);

	if (!member->first_frame_received)
	{
		// nat=yes will be correct now
		member->first_frame_received = 1;
		member->conference = 1;
	}

	// We have to drop if the queue is full!
	if ( member->inVideoFramesCount >= TRIS_CONF_MAX_VIDEO_QUEUE )
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue incoming VIDEO frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inVideoFramesCount, member->outVideoFramesCount
		) ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	// ( member->inFrames may be null at this point )
	conf_frame* cfr = create_conf_frame( member, member->inVideoFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to malloc conf_frame\n" ) ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	// copy frame data pointer to conf frame
	// cfr->fr = fr ;

	//
	// add new frame to speaking members incoming frame queue
	// ( i.e. save this frame data, so we can distribute it in conference_exec later )
	//

	if ( member->inVideoFrames == NULL )
	{
		// this is the first frame in the buffer
		member->inVideoFramesTail = cfr ;
		member->inVideoFrames = cfr ;
	}
	else
	{
		// put the new frame at the head of the list
		member->inVideoFrames = cfr ;
	}

	// increment member frame count
	member->inVideoFramesCount++ ;

	tris_mutex_unlock(&member->lock);

        // Everything has gone okay!
	return 0;
}

int queue_incoming_desktop_frame( struct tris_conf_member* member, const struct tris_frame* fr )
{
	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	// lock the member
	tris_mutex_lock(&member->lock);

	if (!member->first_frame_received)
	{
		// nat=yes will be correct now
		member->first_frame_received = 1;
	}

	// We have to drop if the queue is full!
	if ( member->inDesktopFramesCount >= TRIS_CONF_MAX_DESKTOP_QUEUE )
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue incoming DESKTOP frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inDesktopFramesCount, member->outDesktopFramesCount
		) ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	// ( member->inFrames may be null at this point )
	conf_frame* cfr = create_conf_frame( member, member->inDesktopFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to malloc conf_frame\n" ) ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	// copy frame data pointer to conf frame
	// cfr->fr = fr ;

	//
	// add new frame to speaking members incoming frame queue
	// ( i.e. save this frame data, so we can distribute it in conference_exec later )
	//

	if ( member->inDesktopFrames == NULL )
	{
		// this is the first frame in the buffer
		member->inDesktopFramesTail = cfr ;
		member->inDesktopFrames = cfr ;
	}
	else
	{
		// put the new frame at the head of the list
		member->inDesktopFrames = cfr ;
	}

	// increment member frame count
	member->inDesktopFramesCount++ ;

	tris_mutex_unlock(&member->lock);

        // Everything has gone okay!
	return 0;
}

int queue_incoming_dtmf_frame( struct tris_conf_member* member, const struct tris_frame* fr )
{
  //tris_log( TRIS_CONF_DEBUG, "queue incoming video frame\n");

	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	tris_mutex_lock(&member->lock);

	// We have to drop if the queue is full!
	if ( member->inDTMFFramesCount >= TRIS_CONF_MAX_DTMF_QUEUE )
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue incoming DTMF frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inDTMFFramesCount, member->outDTMFFramesCount
		) ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	// ( member->inFrames may be null at this point )
	conf_frame* cfr = create_conf_frame( member, member->inDTMFFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to malloc conf_frame\n" ) ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	// copy frame data pointer to conf frame
	// cfr->fr = fr ;

	//
	// add new frame to speaking members incoming frame queue
	// ( i.e. save this frame data, so we can distribute it in conference_exec later )
	//

	if ( member->inDTMFFrames == NULL )
	{
		// this is the first frame in the buffer
		member->inDTMFFramesTail = cfr ;
		member->inDTMFFrames = cfr ;
	}
	else
	{
		// put the new frame at the head of the list
		member->inDTMFFrames = cfr ;
	}

	// increment member frame count
	member->inDTMFFramesCount++ ;

	tris_mutex_unlock(&member->lock);

	// Everything has gone okay!
	return 0;
}

int queue_incoming_frame( struct tris_conf_member* member, struct tris_frame* fr )
{
	//
	// sanity checks
	//

	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	tris_mutex_lock(&member->lock);

	if ( member->inFramesCount > member->inFramesNeeded )
	{
		if ( member->inFramesCount > TRIS_CONF_QUEUE_DROP_THRESHOLD )
		{
			struct timeval curr = tris_tvnow();

			// time since last dropped frame
			long diff = tris_tvdiff_ms(curr, member->ltris_in_dropped);

			// number of milliseconds which must pass between frame drops
			// ( 15 frames => -100ms, 10 frames => 400ms, 5 frames => 900ms, 0 frames => 1400ms, etc. )
			long time_limit = 1000 - ( ( member->inFramesCount - TRIS_CONF_QUEUE_DROP_THRESHOLD ) * 100 ) ;

			if ( diff >= time_limit )
			{
				// count sequential drops
				member->sequential_drops++ ;

				tris_log(
					TRIS_CONF_DEBUG,
					"dropping frame from input buffer, channel => %s, incoming => %d, outgoing => %d\n",
					member->channel_name, member->inFramesCount, member->outFramesCount
				) ;

				// accounting: count dropped incoming frames
				member->frames_in_dropped++ ;

				// reset frames since dropped
				member->since_dropped = 0 ;

				// delete the frame
				delete_conf_frame( get_incoming_frame( member ) ) ;

				member->ltris_in_dropped = tris_tvnow();
			}
			else
			{
/*
				tris_log(
					TRIS_CONF_DEBUG,
					"input buffer larger than drop threshold, channel => %s, incoming => %d, outgoing => %d\n",
					member->channel_name, member->inFramesCount, member->outFramesCount
				) ;
*/
			}
		}
	}

	//
	// if we have to drop frames, we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//

	if ( member->inFramesCount >= TRIS_CONF_MAX_QUEUE )
	{
		// count sequential drops
		member->sequential_drops++ ;

		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue incoming frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inFramesCount, member->outFramesCount
		) ;

		// accounting: count dropped incoming frames
		member->frames_in_dropped++ ;

		// reset frames since dropped
		member->since_dropped = 0 ;

		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	// reset sequential drops
	member->sequential_drops = 0 ;

	// increment frames since dropped
	member->since_dropped++ ;

	//
	// create new conf frame from passed data frame
	//

	// ( member->inFrames may be null at this point )
	if (member->inSmoother == NULL ){
		conf_frame* cfr = create_conf_frame( member, member->inFrames, fr ) ;
		if ( cfr == NULL )
		{
			tris_log( LOG_ERROR, "unable to malloc conf_frame\n" ) ;
			tris_mutex_unlock(&member->lock);
			return -1 ;
		}

		//
		// add new frame to speaking members incoming frame queue
		// ( i.e. save this frame data, so we can distribute it in conference_exec later )
		//

		if ( member->inFrames == NULL ) {
			member->inFramesTail = cfr ;
		}
		member->inFrames = cfr ;
		member->inFramesCount++ ;
	} else {
		//feed frame(fr) into the smoother

		// smoother tmp frame
		struct tris_frame *sfr;
		int multiple = 1;
		int i=0;

#if 0
		if ( (member->smooth_size_in > 0 ) && (member->smooth_size_in * member->smooth_multiple != fr->datalen) )
		{
			tris_log( TRIS_CONF_DEBUG, "resetting smooth_size_in. old size=> %d, multiple =>%d, datalen=> %d\n", member->smooth_size_in, member->smooth_multiple, fr->datalen );
			if ( fr->datalen % member->smooth_multiple != 0) {
				// if datalen not divisible by smooth_multiple, assume we're just getting normal encoding.
			//	tris_log(TRIS_CONF_DEBUG,"smooth_multiple does not divide datalen. changing smooth size from %d to %d, multiple => 1\n", member->smooth_size_in, fr->datalen);
				member->smooth_size_in = fr->datalen;
				member->smooth_multiple = 1;
			} else {
				// assume a fixed multiple, so divide into datalen.
				int newsmooth = fr->datalen / member->smooth_multiple ;
			//	tris_log(TRIS_CONF_DEBUG,"datalen is divisible by smooth_multiple, changing smooth size from %d to %d\n", member->smooth_size_in, newsmooth);
				member->smooth_size_in = newsmooth;
			}

			//free input smoother.
			if (member->inSmoother != NULL)
				tris_smoother_free(member->inSmoother);

			//make new input smoother.
			member->inSmoother = tris_smoother_new(member->smooth_size_in);
		}
#endif

		tris_smoother_feed( member->inSmoother, fr );
tris_log (TRIS_CONF_DEBUG, "SMOOTH:Feeding frame into inSmoother, timestamp => %ld.%ld\n", fr->delivery.tv_sec, fr->delivery.tv_usec);

		if ( multiple > 1 )
			fr->samples /= multiple;

		// read smoothed version of frames, add to queue
		while( ( sfr = tris_smoother_read( member->inSmoother ) ) ){

			++i;
tris_log( TRIS_CONF_DEBUG , "\treading new frame [%d] from smoother, inFramesCount[%d], \n\tsfr->frametype -> %d , sfr->subclass -> %d , sfr->datalen => %d sfr->samples => %d\n", i , member->inFramesCount , sfr->frametype, sfr->subclass, sfr->datalen, sfr->samples);
tris_log (TRIS_CONF_DEBUG, "SMOOTH:Reading frame from inSmoother, i=>%d, timestamp => %ld.%ld\n",i, sfr->delivery.tv_sec, sfr->delivery.tv_usec);
			conf_frame* cfr = create_conf_frame( member, member->inFrames, sfr ) ;
			if ( cfr == NULL )
			{
				tris_log( LOG_ERROR, "unable to malloc conf_frame\n" ) ;
				tris_mutex_unlock(&member->lock);
				return -1 ;
			}

			//
			// add new frame to speaking members incoming frame queue
			// ( i.e. save this frame data, so we can distribute it in conference_exec later )
			//

			if ( member->inFrames == NULL ) {
				member->inFramesTail = cfr ;
			}
			member->inFrames = cfr ;
			member->inFramesCount++ ;
		}
	}
	tris_mutex_unlock(&member->lock);
	return 0 ;
}

//
// outgoing frame functions
//

conf_frame* get_outgoing_frame( struct tris_conf_member *member )
{
	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	conf_frame* cfr ;

	// tris_log( TRIS_CONF_DEBUG, "getting member frames, count => %d\n", member->outFramesCount ) ;

	tris_mutex_lock(&member->lock);

	if ( member->outFramesCount > TRIS_CONF_MIN_QUEUE )
	{
		cfr = member->outFramesTail ;

		// if it's the only frame, reset the queu,
		// else, move the second frame to the front
		if ( member->outFramesTail == member->outFrames )
		{
			member->outFrames = NULL ;
			member->outFramesTail = NULL ;
		}
		else
		{
			// move the pointer to the next frame
			member->outFramesTail = member->outFramesTail->prev ;

			// reset it's 'next' pointer
			if ( member->outFramesTail != NULL )
				member->outFramesTail->next = NULL ;
		}

		// separate the conf frame from the list
		cfr->next = NULL ;
		cfr->prev = NULL ;

		// decriment frame count
		member->outFramesCount-- ;
		tris_mutex_unlock(&member->lock);
		return cfr ;
	}
	tris_mutex_unlock(&member->lock);
	return NULL ;
}

int __queue_outgoing_frame( struct tris_conf_member* member, const struct tris_frame* fr, struct timeval delivery )
{
	// accounting: count the number of outgoing frames for this member
	member->frames_out++ ;

	//
	// we have to drop frames, so we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//
	if ( member->outFramesCount >= TRIS_CONF_MAX_QUEUE )
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue outgoing frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inFramesCount, member->outFramesCount
		) ;

		// accounting: count dropped outgoing frames
		member->frames_out_dropped++ ;
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	conf_frame* cfr = create_conf_frame( member, member->outFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to create new conf frame\n" ) ;

		// accounting: count dropped outgoing frames
		member->frames_out_dropped++ ;
		return -1 ;
	}

	// set delivery timestamp
	cfr->fr->delivery = delivery ;

	//
	// add new frame to speaking members incoming frame queue
	// ( i.e. save this frame data, so we can distribute it in conference_exec later )
	//

	if ( member->outFrames == NULL ) {
		member->outFramesTail = cfr ;
	}
	member->outFrames = cfr ;
	member->outFramesCount++ ;

	// return success
	return 0 ;
}

int queue_outgoing_frame( struct tris_conf_member* member, const struct tris_frame* fr, struct timeval delivery )
{
	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	if ( ( member->outPacker == NULL ) && ( member->smooth_multiple > 1 ) && ( member->smooth_size_out > 0 ) ){
		//tris_log (TRIS_CONF_DEBUG, "creating outPacker with size => %d \n\t( multiple => %d ) * ( size => %d )\n", member->smooth_multiple * member-> smooth_size_out, member->smooth_multiple , member->smooth_size_out);
		member->outPacker = tris_packer_new( member->smooth_multiple * member->smooth_size_out);
	}

	if (member->outPacker == NULL ){
		return __queue_outgoing_frame( member, fr, delivery ) ;
	}
	else
	{
		struct tris_frame *sfr;
		int exitval = 0;
//tris_log (TRIS_CONF_DEBUG, "sending fr into outPacker, datalen=>%d, samples=>%d\n",fr->datalen, fr->samples);
		tris_packer_feed( member->outPacker , fr );
		while( (sfr = tris_packer_read( member->outPacker ) ) )
		{
//tris_log (TRIS_CONF_DEBUG, "read sfr from outPacker, datalen=>%d, samples=>%d\n",sfr->datalen, sfr->samples);
			if ( __queue_outgoing_frame( member, sfr, delivery ) == -1 ) {
				exitval = -1;
			}
		}

		return exitval;
	}
}

//
// outgoing frame functions
//

conf_frame* get_outgoing_video_frame( struct tris_conf_member *member )
{
	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	conf_frame* cfr ;

	tris_mutex_lock(&member->lock);

	// tris_log( TRIS_CONF_DEBUG, "getting member frames, count => %d\n", member->outFramesCount ) ;

	if ( member->outVideoFramesCount > TRIS_CONF_MIN_QUEUE )
	{
		cfr = member->outVideoFramesTail ;

		// if it's the only frame, reset the queu,
		// else, move the second frame to the front
		if ( member->outVideoFramesTail == member->outVideoFrames )
		{
			member->outVideoFrames = NULL ;
			member->outVideoFramesTail = NULL ;
		}
		else
		{
			// move the pointer to the next frame
			member->outVideoFramesTail = member->outVideoFramesTail->prev ;

			// reset it's 'next' pointer
			if ( member->outVideoFramesTail != NULL )
				member->outVideoFramesTail->next = NULL ;
		}

		// separate the conf frame from the list
		cfr->next = NULL ;
		cfr->prev = NULL ;

		// decriment frame count
		member->outVideoFramesCount-- ;
		tris_mutex_unlock(&member->lock);
		return cfr ;
	}

	tris_mutex_unlock(&member->lock);
	return NULL ;
}



int queue_outgoing_video_frame( struct tris_conf_member* member, const struct tris_frame* fr, struct timeval delivery )
{
	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	tris_mutex_lock(&member->lock);

	// accounting: count the number of outgoing frames for this member
	member->video_frames_out++ ;

	//
	// we have to drop frames, so we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//
	if ( member->outVideoFramesCount >= TRIS_CONF_MAX_VIDEO_QUEUE)
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue outgoing VIDEO frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inVideoFramesCount, member->outVideoFramesCount
		) ;

		// accounting: count dropped outgoing frames
		member->video_frames_out_dropped++ ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	conf_frame* cfr = create_conf_frame( member, member->outVideoFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to create new conf frame\n" ) ;

		// accounting: count dropped outgoing frames
		member->video_frames_out_dropped++ ;
                tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	// set delivery timestamp
#ifdef VIDEO_SETTIMESTAMP
	cfr->fr->delivery = delivery ;
#else
	cfr->fr->delivery.tv_sec = 0;
	cfr->fr->delivery.tv_usec = 0;
#endif
	//tris_log (LOG_WARNING,"%d\n",cfr->fr->seqno);

#ifdef RTP_SEQNO_ZERO
	cfr->fr->seqno = 0;
#endif

	if ( member->outVideoFrames == NULL )
	{
		// this is the first frame in the buffer
		member->outVideoFramesTail = cfr ;
		member->outVideoFrames = cfr ;
	}
	else
	{
		// put the new frame at the head of the list
		member->outVideoFrames = cfr ;
	}

	// increment member frame count
	member->outVideoFramesCount++ ;

	tris_mutex_unlock(&member->lock);

	// return success
	return 0 ;
}

conf_frame* get_outgoing_desktop_frame( struct tris_conf_member *member )
{
	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	conf_frame* cfr ;

	tris_mutex_lock(&member->lock);

	// tris_log( TRIS_CONF_DEBUG, "getting member frames, count => %d\n", member->outFramesCount ) ;

	if ( member->outDesktopFramesCount > TRIS_CONF_MIN_QUEUE )
	{
		cfr = member->outDesktopFramesTail ;

		// if it's the only frame, reset the queu,
		// else, move the second frame to the front
		if ( member->outDesktopFramesTail == member->outDesktopFrames )
		{
			member->outDesktopFrames = NULL ;
			member->outDesktopFramesTail = NULL ;
		}
		else
		{
			// move the pointer to the next frame
			member->outDesktopFramesTail = member->outDesktopFramesTail->prev ;

			// reset it's 'next' pointer
			if ( member->outDesktopFramesTail != NULL )
				member->outDesktopFramesTail->next = NULL ;
		}

		// separate the conf frame from the list
		cfr->next = NULL ;
		cfr->prev = NULL ;

		// decriment frame count
		member->outDesktopFramesCount-- ;
		tris_mutex_unlock(&member->lock);
		return cfr ;
	}

	tris_mutex_unlock(&member->lock);
	return NULL ;
}



int queue_outgoing_desktop_frame( struct tris_conf_member* member, const struct tris_frame* fr, struct timeval delivery )
{
	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	tris_mutex_lock(&member->lock);

	// accounting: count the number of outgoing frames for this member
	member->desktop_frames_out++ ;

	//
	// we have to drop frames, so we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//
	if ( member->outDesktopFramesCount >= TRIS_CONF_MAX_DESKTOP_QUEUE)
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue outgoing DESKTOP frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inDesktopFramesCount, member->outDesktopFramesCount
		) ;

		// accounting: count dropped outgoing frames
		member->desktop_frames_out_dropped++ ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	conf_frame* cfr = create_conf_frame( member, member->outDesktopFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to create new conf frame\n" ) ;

		// accounting: count dropped outgoing frames
		member->desktop_frames_out_dropped++ ;
                tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	// set delivery timestamp
#ifdef VIDEO_SETTIMESTAMP
	cfr->fr->delivery = delivery ;
#else
	cfr->fr->delivery.tv_sec = 0;
	cfr->fr->delivery.tv_usec = 0;
#endif
	//tris_log (LOG_WARNING,"%d\n",cfr->fr->seqno);

#ifdef RTP_SEQNO_ZERO
	cfr->fr->seqno = 0;
#endif

	if ( member->outDesktopFrames == NULL )
	{
		// this is the first frame in the buffer
		member->outDesktopFramesTail = cfr ;
		member->outDesktopFrames = cfr ;
	}
	else
	{
		// put the new frame at the head of the list
		member->outDesktopFrames = cfr ;
	}

	// increment member frame count
	member->outDesktopFramesCount++ ;

	tris_mutex_unlock(&member->lock);

	// return success
	return 0 ;
}

conf_frame* get_outgoing_dtmf_frame( struct tris_conf_member *member )
{
	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	conf_frame* cfr ;

	// tris_log( TRIS_CONF_DEBUG, "getting member frames, count => %d\n", member->outFramesCount ) ;

	tris_mutex_lock(&member->lock);

	if ( member->outDTMFFramesCount > TRIS_CONF_MIN_QUEUE )
	{
		cfr = member->outDTMFFramesTail ;

		// if it's the only frame, reset the queu,
		// else, move the second frame to the front
		if ( member->outDTMFFramesTail == member->outDTMFFrames )
		{
			member->outDTMFFrames = NULL ;
			member->outDTMFFramesTail = NULL ;
		}
		else
		{
			// move the pointer to the next frame
			member->outDTMFFramesTail = member->outDTMFFramesTail->prev ;

			// reset it's 'next' pointer
			if ( member->outDTMFFramesTail != NULL )
				member->outDTMFFramesTail->next = NULL ;
		}

		// separate the conf frame from the list
		cfr->next = NULL ;
		cfr->prev = NULL ;

		// decriment frame count
		member->outDTMFFramesCount-- ;
		tris_mutex_unlock(&member->lock);
		return cfr ;
	}
	tris_mutex_unlock(&member->lock);
	return NULL ;
}

conf_frame* get_outgoing_text_frame( struct tris_conf_member *member )
{
	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to get frame from null member\n" ) ;
		return NULL ;
	}

	conf_frame* cfr ;

	// tris_log( TRIS_CONF_DEBUG, "getting member frames, count => %d\n", member->outFramesCount ) ;

	tris_mutex_lock(&member->lock);

	if ( member->outTextFramesCount > TRIS_CONF_MIN_QUEUE )
	{
		cfr = member->outTextFramesTail ;

		// if it's the only frame, reset the queu,
		// else, move the second frame to the front
		if ( member->outTextFramesTail == member->outTextFrames )
		{
			member->outTextFrames = NULL ;
			member->outTextFramesTail = NULL ;
		}
		else
		{
			// move the pointer to the next frame
			member->outTextFramesTail = member->outTextFramesTail->prev ;

			// reset it's 'next' pointer
			if ( member->outTextFramesTail != NULL )
				member->outTextFramesTail->next = NULL ;
		}

		// separate the conf frame from the list
		cfr->next = NULL ;
		cfr->prev = NULL ;

		// decriment frame count
		member->outTextFramesCount-- ;
		tris_mutex_unlock(&member->lock);
		return cfr ;
	}
	tris_mutex_unlock(&member->lock);
	return NULL ;
}


int queue_outgoing_dtmf_frame( struct tris_conf_member* member, const struct tris_frame* fr )
{
	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	tris_mutex_lock(&member->lock);

	// accounting: count the number of outgoing frames for this member
	member->dtmf_frames_out++ ;

	//
	// we have to drop frames, so we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//
	if ( member->outDTMFFramesCount >= TRIS_CONF_MAX_DTMF_QUEUE)
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue outgoing DTMF frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inDTMFFramesCount, member->outDTMFFramesCount
		) ;

		// accounting: count dropped outgoing frames
		member->dtmf_frames_out_dropped++ ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	conf_frame* cfr = create_conf_frame( member, member->outDTMFFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to create new conf frame\n" ) ;

		// accounting: count dropped outgoing frames
		member->dtmf_frames_out_dropped++ ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

#ifdef RTP_SEQNO_ZERO
	cfr->fr->seqno = 0;
#endif

	if ( member->outDTMFFrames == NULL )
	{
		// this is the first frame in the buffer
		member->outDTMFFramesTail = cfr ;
		member->outDTMFFrames = cfr ;
	}
	else
	{
		// put the new frame at the head of the list
		member->outDTMFFrames = cfr ;
	}

	// increment member frame count
	member->outDTMFFramesCount++ ;

	tris_mutex_unlock(&member->lock);
	// return success
	return 0 ;
}

int queue_outgoing_text_frame( struct tris_conf_member* member, const struct tris_frame* fr)
{
	// check on frame
	if ( fr == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue null frame\n" ) ;
		return -1 ;
	}

	// check on member
	if ( member == NULL )
	{
		tris_log( LOG_ERROR, "unable to queue frame for null member\n" ) ;
		return -1 ;
	}

	tris_mutex_lock(&member->lock);

	// accounting: count the number of outgoing frames for this member
	member->text_frames_out++ ;

	//
	// we have to drop frames, so we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//
	if ( member->outTextFramesCount >= TRIS_CONF_MAX_TEXT_QUEUE)
	{
		tris_log(
			TRIS_CONF_DEBUG,
			"unable to queue outgoing text frame, channel => %s, incoming => %d, outgoing => %d\n",
			member->channel_name, member->inTextFramesCount, member->outTextFramesCount
		) ;

		// accounting: count dropped outgoing frames
		member->text_frames_out_dropped++ ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

	//
	// create new conf frame from passed data frame
	//

	conf_frame* cfr = create_conf_frame( member, member->outTextFrames, fr ) ;

	if ( cfr == NULL )
	{
		tris_log( LOG_ERROR, "unable to create new conf frame\n" ) ;

		// accounting: count dropped outgoing frames
		member->text_frames_out_dropped++ ;
		tris_mutex_unlock(&member->lock);
		return -1 ;
	}

#ifdef RTP_SEQNO_ZERO
	cfr->fr->seqno = 0;
#endif

	if ( member->outTextFrames == NULL )
	{
		// this is the first frame in the buffer
		member->outTextFramesTail = cfr ;
		member->outTextFrames = cfr ;
	}
	else
	{
		// put the new frame at the head of the list
		member->outTextFrames = cfr ;
	}

	// increment member frame count
	member->outTextFramesCount++ ;

	tris_mutex_unlock(&member->lock);
	// return success
	return 0 ;
}


//
// manager functions
//

void send_state_change_notifications( struct tris_conf_member* member )
{
	// tris_log( TRIS_CONF_DEBUG, "sending state change notification\n" ) ;

	// loop through list of members, sending state changes
	while ( member != NULL )
	{
		// has the state changed since last time through this loop?
		if ( member->speaking_state_notify )
		{
			manager_event(
				EVENT_FLAG_CALL,
				"ConferenceState",
				"Channel: %s\r\n"
				"State: %s\r\n",
				member->channel_name,
				( ( member->speaking_state == 1 ) ? "speaking" : "silent" )
			) ;

			tris_log( TRIS_CONF_DEBUG, "member state changed, channel => %s, state => %d, incoming => %d, outgoing => %d\n",
				member->channel_name, member->speaking_state, member->inFramesCount, member->outFramesCount ) ;

			member->speaking_state_notify = 0;
		}

		// move the pointer to the next member
		member = member->next ;
	}

	return ;
}

//
// tris_packer, adapted from tris_smoother
// pack multiple frames together into one packet on the wire.
//

#define PACKER_SIZE  8000
#define PACKER_QUEUE 10 // store at most 10 complete packets in the queue

struct tris_packer {
	int framesize; // number of frames per packet on the wire.
	int size;
	int packet_index;
	int format;
	int readdata;
	int optimizablestream;
	int flags;
	float samplesperbyte;
	struct tris_frame f;
	struct timeval delivery;
	char data[PACKER_SIZE];
	char framedata[PACKER_SIZE + TRIS_FRIENDLY_OFFSET];
	int samples;
	int sample_queue[PACKER_QUEUE];
	int len_queue[PACKER_QUEUE];
	struct tris_frame *opt;
	int len;
};

void tris_packer_reset(struct tris_packer *s, int framesize)
{
	memset(s, 0, sizeof(struct tris_packer));
	s->framesize = framesize;
	s->packet_index=0;
	s->len=0;
}

struct tris_packer *tris_packer_new(int framesize)
{
	struct tris_packer *s;
	if (framesize < 1)
		return NULL;
	s = malloc(sizeof(struct tris_packer));
	if (s)
		tris_packer_reset(s, framesize);
	return s;
}

int tris_packer_get_flags(struct tris_packer *s)
{
	return s->flags;
}

void tris_packer_set_flags(struct tris_packer *s, int flags)
{
	s->flags = flags;
}

int tris_packer_feed(struct tris_packer *s, const struct tris_frame *f)
{
	if (f->frametype != TRIS_FRAME_VOICE) {
		tris_log(LOG_WARNING, "Huh?  Can't pack a non-voice frame!\n");
		return -1;
	}
	if (!s->format) {
		s->format = f->subclass;
		s->samples=0;
	} else if (s->format != f->subclass) {
		tris_log(LOG_WARNING, "Packer was working on %d format frames, now trying to feed %d?\n", s->format, f->subclass);
		return -1;
	}
	if (s->len + f->datalen > PACKER_SIZE) {
		tris_log(LOG_WARNING, "Out of packer space\n");
		return -1;
	}
	if (s->packet_index >= PACKER_QUEUE ){
		tris_log(LOG_WARNING, "Out of packer queue space\n");
		return -1;
	}

	memcpy(s->data + s->len, f->data.ptr, f->datalen);
	/* If either side is empty, reset the delivery time */
	if (!s->len || (!f->delivery.tv_sec && !f->delivery.tv_usec) ||
			(!s->delivery.tv_sec && !s->delivery.tv_usec))
		s->delivery = f->delivery;
	s->len += f->datalen;
//packer stuff
	s->len_queue[s->packet_index]    += f->datalen;
	s->sample_queue[s->packet_index] += f->samples;
	s->samples += f->samples;

	if (s->samples > s->framesize )
		++s->packet_index;

	return 0;
}

struct tris_frame *tris_packer_read(struct tris_packer *s)
{
	struct tris_frame *opt;
	int len;
	/* IF we have an optimization frame, send it */
	if (s->opt) {
		opt = s->opt;
		s->opt = NULL;
		return opt;
	}

	/* Make sure we have enough data */
	if (s->samples < s->framesize ){
			return NULL;
	}
	len = s->len_queue[0];
	if (len > s->len)
		len = s->len;
	/* Make frame */
	s->f.frametype = TRIS_FRAME_VOICE;
	s->f.subclass = s->format;
	s->f.data.ptr = s->framedata + TRIS_FRIENDLY_OFFSET;
	s->f.offset = TRIS_FRIENDLY_OFFSET;
	s->f.datalen = len;
	s->f.samples = s->sample_queue[0];
	s->f.delivery = s->delivery;
	/* Fill Data */
	memcpy(s->f.data.ptr, s->data, len);
	s->len -= len;
	/* Move remaining data to the front if applicable */
	if (s->len) {
		/* In principle this should all be fine because if we are sending
		   G.729 VAD, the next timestamp will take over anyawy */
		memmove(s->data, s->data + len, s->len);
		if (s->delivery.tv_sec || s->delivery.tv_usec) {
			/* If we have delivery time, increment it, otherwise, leave it at 0 */
			s->delivery.tv_sec +=  s->sample_queue[0] / 8000.0;
			s->delivery.tv_usec += (((int)(s->sample_queue[0])) % 8000) * 125;
			if (s->delivery.tv_usec > 1000000) {
				s->delivery.tv_usec -= 1000000;
				s->delivery.tv_sec += 1;
			}
		}
	}
	int j;
	s->samples -= s->sample_queue[0];
	if( s->packet_index > 0 ){
		for (j=0; j<s->packet_index -1 ; j++){
			s->len_queue[j]=s->len_queue[j+1];
			s->sample_queue[j]=s->sample_queue[j+1];
		}
		s->len_queue[s->packet_index]=0;
		s->sample_queue[s->packet_index]=0;
		s->packet_index--;
	} else {
		s->len_queue[0]=0;
		s->sample_queue[0]=0;
	}


	/* Return frame */
	return &s->f;
}

void tris_packer_free(struct tris_packer *s)
{
	free(s);
}

int queue_frame_for_listener(
	struct tris_conference* conf,
	struct tris_conf_member* member,
	conf_frame* frame
)
{
	//
	// check inputs
	//

	if ( conf == NULL )
	{
		tris_log( LOG_WARNING, "unable to queue listener frame with null conference\n" ) ;
		return -1 ;
	}

	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to queue listener frame with null member\n" ) ;
		return -1 ;
	}

	//
	// loop over spoken frames looking for member's appropriate match
	//

	short found_flag = 0 ;
	struct tris_frame* qf ;

	for ( ; frame != NULL ; frame = frame->next )
	{
		// we're looking for a null or matching member
		if ( frame->member != NULL && frame->member != member )
			continue ;

		if ( frame->fr == NULL )
		{
			tris_log( LOG_WARNING, "unknown error queueing frame for listener, frame->fr == NULL\n" ) ;
			continue ;
		}

		// first, try for a pre-converted frame
		qf = frame->converted[ member->write_format_index ] ;

		// convert ( and store ) the frame
		if ( qf == NULL )
		{
			// make a copy of the slinear version of the frame
			qf = tris_frdup( frame->fr ) ;

			if ( qf == NULL )
			{
				tris_log( LOG_WARNING, "unable to duplicate frame\n" ) ;
				continue ;
			}

			// convert using the conference's translation path
			qf = convert_frame_from_slinear( conf->from_slinear_paths[ member->write_format_index ], qf ) ;

			// store the converted frame
			// ( the frame will be free'd next time through the loop )
			frame->converted[ member->write_format_index ] = qf ;
		}

		if ( qf != NULL )
		{
			// duplicate the frame before queue'ing it
			// ( since this member doesn't own this _shared_ frame )
			// qf = tris_frdup( qf ) ;



			if ( queue_outgoing_frame( member, qf, conf->delivery_time ) != 0 )
			{
				// free the new frame if it couldn't be queue'd
				// XXX NEILS - WOULD BE FREED IN CLEANUPtris_frfree( qf ) ;
				//qf = NULL ;
			}
		}
		else
		{
			tris_log( LOG_WARNING, "unable to translate outgoing listener frame, channel => %s\n", member->channel_name ) ;
		}

		// set found flag
		found_flag = 1 ;

		// break from for loop
		break ;
	}

	// queue a silent frame
	if ( found_flag == 0 )
		queue_silent_frame( conf, member ) ;

	return 0 ;
}


int queue_frame_for_speaker(
	struct tris_conference* conf,
	struct tris_conf_member* member,
	conf_frame* frame
)
{
	//
	// check inputs
	//

	if ( conf == NULL )
	{
		tris_log( LOG_WARNING, "unable to queue speaker frame with null conference\n" ) ;
		return -1 ;
	}

	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to queue speaker frame with null member\n" ) ;
		return -1 ;
	}

	//
	// loop over spoken frames looking for member's appropriate match
	//

	short found_flag = 0 ;
	struct tris_frame* qf ;

	for ( ; frame != NULL ; frame = frame->next )
	{
		if ( frame->member != member )
		{
			continue ;
		}

		if ( frame->fr == NULL )
		{
			tris_log( LOG_WARNING, "unable to queue speaker frame with null data\n" ) ;
			continue ;
		}

		//
		// convert and queue frame
		//

		// short-cut pointer to the tris_frame
		qf = frame->fr ;

		if ( qf->subclass == member->write_format )
		{
			// frame is already in correct format, so just queue it

			queue_outgoing_frame( member, qf, conf->delivery_time ) ;
		}
		else
		{
			//
			// convert frame to member's write format
			// ( calling tris_frdup() to make sure the translator's copy sticks around )
			//
			qf = convert_frame_from_slinear( member->from_slinear, tris_frdup( qf ) ) ;

			if ( qf != NULL )
			{
				// queue frame
				queue_outgoing_frame( member, qf, conf->delivery_time ) ;

				// free frame ( the translator's copy )
				tris_frfree( qf ) ;
			}
			else
			{
				tris_log( LOG_WARNING, "unable to translate outgoing speaker frame, channel => %s\n", member->channel_name ) ;
			}
		}

		// set found flag
		found_flag = 1 ;

		// we found the frame, skip to the next member
		break ;
	}

	// queue a silent frame
	if ( found_flag == 0 )
		queue_silent_frame( conf, member ) ;

	return 0 ;
}


int queue_silent_frame(
	struct tris_conference* conf,
	struct tris_conf_member* member
)
{
  int c;
#ifdef APP_CONFERENCE_DEBUG
	//
	// check inputs
	//

	if ( conf == NULL )
	{
		tris_log( TRIS_CONF_DEBUG, "unable to queue silent frame for null conference\n" ) ;
		return -1 ;
	}

	if ( member == NULL )
	{
		tris_log( TRIS_CONF_DEBUG, "unable to queue silent frame for null member\n" ) ;
		return -1 ;
	}
#endif // APP_CONFERENCE_DEBUG

	//
	// initialize static variables
	//

	static conf_frame* silent_frame = NULL ;
	static struct tris_frame* qf = NULL ;

	if ( silent_frame == NULL )
	{
		if ( ( silent_frame = get_silent_frame() ) == NULL )
		{
			tris_log( LOG_WARNING, "unable to initialize static silent frame\n" ) ;
			return -1 ;
		}
	}


	// get the appropriate silent frame
	qf = silent_frame->converted[ member->write_format_index ] ;

	if ( qf == NULL )
	{
		//
		// we need to do this to avoid echo on the speaker's line.
		// translators seem to be single-purpose, i.e. they
		// can't be used simultaneously for multiple audio streams
		//

		struct tris_trans_pvt* trans = tris_translator_build_path( member->write_format, TRIS_FORMAT_SLINEAR ) ;

		if ( trans != NULL )
		{
			// attempt ( five times ) to get a silent frame
			// to make sure we provice the translator with enough data
			for ( c = 0 ; c < 5 ; ++c )
			{
				// translate the frame
				qf = tris_translate( trans, silent_frame->fr, 0 ) ;

				// break if we get a frame
				if ( qf != NULL ) break ;
			}

			if ( qf != NULL )
			{
				// isolate the frame so we can keep it around after trans is free'd
				qf = tris_frisolate( qf ) ;

				// cache the new, isolated frame
				silent_frame->converted[ member->write_format_index ] = qf ;
			}

			tris_translator_free_path( trans ) ;
		}
	}

	//
	// queue the frame, if it's not null,
	// otherwise there was an error
	//
	if ( qf != NULL )
	{
		queue_outgoing_frame( member, qf, conf->delivery_time ) ;
	}
	else
	{
		tris_log( LOG_ERROR, "unable to translate outgoing silent frame, channel => %s\n", member->channel_name ) ;
	}

	return 0 ;
}



void member_process_outgoing_frames(struct tris_conference* conf,
				  struct tris_conf_member *member,
				  struct conf_frame *send_frames)
{
	tris_mutex_lock(&member->lock);

	// skip members that are not ready
	if ( member->ready_for_outgoing == 0 )
	{
		tris_mutex_unlock(&member->lock);
		return ;
	}

	// skip no receive audio clients
	if ( member->norecv_audio )
	{
		tris_mutex_unlock(&member->lock);
		return;
	}

	if ( member->local_speaking_state == 0 )
	{
		// queue listener frame
		queue_frame_for_listener( conf, member, send_frames ) ;
	}
	else
	{
		// queue speaker frame
		queue_frame_for_speaker( conf, member, send_frames ) ;
	}
	tris_mutex_unlock(&member->lock);
}

// Functions that will increase and decrease speaker_count in a secure way, locking the member mutex if required
// Will also set speaking_state flag.
// Returns the previous speaking state
int increment_speaker_count(struct tris_conf_member *member, int lock)
{
	int old_state;

	if ( lock )
		tris_mutex_lock(&member->lock);

	old_state = member->speaking_state;
	member->speaker_count++;
	member->speaking_state = 1;

	tris_log(TRIS_CONF_DEBUG, "Increment speaker count: id=%d, count=%d\n", member->id, member->speaker_count);

	// If this is a state change, update the timestamp
	if ( old_state == 0 )
	{
		member->speaking_state_notify = 1;
		member->ltris_state_change = tris_tvnow();
	}

	if ( lock )
		tris_mutex_unlock(&member->lock);

	return old_state;
}

int decrement_speaker_count(struct tris_conf_member *member, int lock)
{
	int old_state;

	if ( lock )
		tris_mutex_lock(&member->lock);

	old_state = member->speaking_state;
	if ( member->speaker_count > 0 )
		member->speaker_count--;
	if ( member->speaker_count == 0 )
		member->speaking_state = 0;

	tris_log(TRIS_CONF_DEBUG, "Decrement speaker count: id=%d, count=%d\n", member->id, member->speaker_count);

	// If this is a state change, update the timestamp
	if ( old_state == 1 && member->speaking_state == 0 )
	{
		member->speaking_state_notify = 1;
		member->ltris_state_change = tris_tvnow();
	}

	if ( lock )
		tris_mutex_unlock(&member->lock);

	return old_state;
}

void member_process_spoken_frames(struct tris_conference* conf,
				 struct tris_conf_member *member,
				 struct conf_frame **spoken_frames,
				 long time_diff,
				 int *listener_count,
				 int *speaker_count
	)
{
	struct conf_frame *cfr;

	// acquire member mutex
	TIMELOG(tris_mutex_lock( &member->lock ),1,"conf thread member lock") ;

	// check for dead members
	if ( member->remove_flag == 1 )
	{
		// If this member is the default video source for the conference, then change the default to -1
		if ( member->id == conf->default_video_source_id )
			conf->default_video_source_id = -1;

		if (conf->debug_flag)
		{
			tris_log( LOG_NOTICE, "found member slated for removal, channel => %s\n", member->channel_name ) ;
		}
		remove_member( member, conf ) ;
		return;
	}

	// tell member the number of frames we're going to need ( used to help dropping algorithm )
	member->inFramesNeeded = ( time_diff / TRIS_CONF_FRAME_INTERVAL ) - 1 ;

	// !!! TESTING !!!
	if (
		conf->debug_flag == 1
		&& member->inFramesNeeded > 0
		)
	{
		tris_log( TRIS_CONF_DEBUG, "channel => %s, inFramesNeeded => %d, inFramesCount => %d\n",
			 member->channel_name, member->inFramesNeeded, member->inFramesCount ) ;
	}

	// non-listener member should have frames,
	// unless silence detection dropped them
	cfr = get_incoming_frame( member ) ;

	// handle retrieved frames
	if ( cfr == NULL || cfr->fr == NULL )
	{
		// Decrement speaker count for us and for driven members
		// This happens only for the first missed frame, since we want to
		// decrement only on state transitions
		if ( member->local_speaking_state == 1 )
		{
			decrement_speaker_count(member, 0);
			member->local_speaking_state = 0;
			// If we're driving another member, decrement its speaker count as well
			if ( member->driven_member != NULL )
				decrement_speaker_count(member->driven_member, 1);
		}

		// count the listeners
		(*listener_count)++ ;
	}
	else
	{
		// append the frame to the list of spoken frames
		if ( *spoken_frames != NULL )
		{
			// add new frame to end of list
			cfr->next = *spoken_frames ;
			(*spoken_frames)->prev = cfr ;
		}

		// point the list at the new frame
		*spoken_frames = cfr ;

		// Increment speaker count for us and for driven members
		// This happens only on the first received frame, since we want to
		// increment only on state transitions
		if ( member->local_speaking_state == 0 )
		{
			increment_speaker_count(member, 0);
			member->local_speaking_state = 1;

			// If we're driving another member, increment its speaker count as well
			if ( member->driven_member != NULL )
				increment_speaker_count(member->driven_member, 1);
		}

		// count the speakers
		(*speaker_count)++ ;
	}

	// release member mutex
	tris_mutex_unlock( &member->lock ) ;

	return;
}
