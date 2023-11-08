
// $Id: conference.c 886 2007-08-06 14:33:34Z bcholew $

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

#include "trismedia/autoconfig.h"
#include "conference.h"
#include "trismedia/utils.h"

//
// static variables
//

// single-linked list of current conferences
struct tris_conference *conflist = NULL ;

// mutex for synchronizing access to conflist
//static tris_mutex_t conflist_lock = TRIS_MUTEX_INITIALIZER ;
TRIS_MUTEX_DEFINE_STATIC(conflist_lock);

static int conference_count = 0 ;


//
// main conference function
//

// increment a timeval by ms milliseconds
void add_milliseconds(struct timeval* tv, long ms)
{
	// add the microseconds to the microseconds field
	tv->tv_usec += ( ms * 1000 ) ;

	// calculate the number of seconds to increment
	long s = ( tv->tv_usec / 1000000 ) ;

	// adjust the microsends field
	if ( s > 0 ) tv->tv_usec -= ( s * 1000000 ) ;

	// increment the seconds field
	tv->tv_sec += s ;
}

void conference_exec( struct tris_conference *conf )
{

	struct tris_conf_member *next_member;
	struct tris_conf_member *member, *video_source_member, *dtmf_source_member;;
	struct conf_frame *spoken_frames, *send_frames;

	// count number of speakers, number of listeners
	int speaker_count ;
	int listener_count ;

	tris_log( TRIS_CONF_DEBUG, "Entered conference_exec, name => %s\n", conf->name ) ;

	// timer timestamps
	struct timeval base, curr, notify ;
	base = notify = tris_tvnow();

	// holds differences of curr and base
	long time_diff = 0 ;
	long time_sleep = 0 ;

	int since_ltris_slept = 0 ;

	//
	// variables for checking thread frequency
	//

	// count to TRIS_CONF_FRAMES_PER_SECOND
	int tf_count = 0 ;
	long tf_diff = 0 ;
	float tf_frequency = 0.0 ;

	struct timeval tf_base, tf_curr ;
	tf_base = tris_tvnow();

	//
	// main conference thread loop
	//


	while ( 42 == 42 )
	{
		// update the current timestamp
		curr = tris_tvnow();

		// calculate difference in timestamps
		time_diff = tris_tvdiff_ms(curr, base);

		// calculate time we should sleep
		time_sleep = TRIS_CONF_FRAME_INTERVAL - time_diff ;

		if ( time_sleep > 0 )
		{
			// sleep for sleep_time ( as milliseconds )
			usleep( time_sleep * 1000 ) ;

			// reset since last slept counter
			since_ltris_slept = 0 ;

			continue ;
		}
		else
		{
			// long sleep warning
			if (
				since_ltris_slept == 0
				&& time_diff > TRIS_CONF_CONFERENCE_SLEEP * 2
			)
			{
				tris_log(
					TRIS_CONF_DEBUG,
					"long scheduling delay, time_diff => %ld, TRIS_CONF_FRAME_INTERVAL => %d\n",
					time_diff, TRIS_CONF_FRAME_INTERVAL
				) ;
			}

			// increment times since last slept
			++since_ltris_slept ;

			// sleep every other time
			if ( since_ltris_slept % 2 )
				usleep( 0 ) ;
		}

		// adjust the timer base ( it will be used later to timestamp outgoing frames )
		add_milliseconds( &base, TRIS_CONF_FRAME_INTERVAL ) ;

		//
		// check thread frequency
		//

		if ( ++tf_count >= TRIS_CONF_FRAMES_PER_SECOND )
		{
			// update current timestamp
			tf_curr = tris_tvnow();

			// compute timestamp difference
			tf_diff = tris_tvdiff_ms(tf_curr, tf_base);

			// compute sampling frequency
			tf_frequency = ( float )( tf_diff ) / ( float )( tf_count ) ;

			if (
				( tf_frequency <= ( float )( TRIS_CONF_FRAME_INTERVAL - 1 ) )
				|| ( tf_frequency >= ( float )( TRIS_CONF_FRAME_INTERVAL + 1 ) )
			)
			{
				tris_log(
					LOG_WARNING,
					"processed frame frequency variation, name => %s, tf_count => %d, tf_diff => %ld, tf_frequency => %2.4f\n",
					conf->name, tf_count, tf_diff, tf_frequency
				) ;
			}

			// reset values
			tf_base = tf_curr ;
			tf_count = 0 ;
		}

		//-----------------//
		// INCOMING FRAMES //
		//-----------------//

		// tris_log( TRIS_CONF_DEBUG, "PROCESSING FRAMES, conference => %s, step => %d, ms => %ld\n",
		//	conf->name, step, ( base.tv_usec / 20000 ) ) ;

		//
		// check if the conference is empty and if so
		// remove it and break the loop
		//

		// acquire the conference list lock
		tris_mutex_lock(&conflist_lock);

		// acquire the conference mutex
		tris_mutex_lock(&conf->lock);

		if ( conf->membercount == 0 )
		{
			if (conf->debug_flag)
			{
				tris_log( LOG_NOTICE, "removing conference, count => %d, name => %s\n", conf->membercount, conf->name ) ;
			}
			remove_conf( conf ) ; // stop the conference

			// We don't need to release the conf mutex, since it was destroyed anyway

			// release the conference list lock
			tris_mutex_unlock(&conflist_lock);

			break ; // break from main processing loop
		}

		// release the conference mutex
		tris_mutex_unlock(&conf->lock);

		// release the conference list lock
		tris_mutex_unlock(&conflist_lock);


		//
		// Start processing frames
		//

		// acquire conference mutex
		TIMELOG(tris_mutex_lock( &conf->lock ),1,"conf thread conf lock");

		if ( conf->membercount == 0 )
		{
			// release the conference mutex
			tris_mutex_unlock(&conf->lock);
			continue; // We'll check again at the top of the loop
		}

		// update the current delivery time
		conf->delivery_time = base ;

		//
		// loop through the list of members
		// ( conf->memberlist is a single-linked list )
		//

		// tris_log( TRIS_CONF_DEBUG, "begin processing incoming audio, name => %s\n", conf->name ) ;

		// reset speaker and listener count
		speaker_count = 0 ;
		listener_count = 0 ;

		// get list of conference members
		member = conf->memberlist ;

		// reset pointer lists
		spoken_frames = NULL ;

		// reset video source
		video_source_member = NULL;

                // reset dtmf source
		dtmf_source_member = NULL;

		// loop over member list to retrieve queued frames
		while ( member != NULL )
		{
			// take note of next member - before it's too late
			next_member = member->next;

			// this MIGHT delete member
			member_process_spoken_frames(conf,member,&spoken_frames,time_diff,
						     &listener_count, &speaker_count);

			// adjust our pointer to the next inline
			member = next_member;
		}

		// tris_log( TRIS_CONF_DEBUG, "finished processing incoming audio, name => %s\n", conf->name ) ;


		//---------------//
		// MIXING FRAMES //
		//---------------//

		// mix frames and get batch of outgoing frames
		send_frames = mix_frames( spoken_frames, speaker_count, listener_count ) ;

		// accounting: if there are frames, count them as one incoming frame
		if ( send_frames != NULL )
		{
			// set delivery timestamp
			//set_conf_frame_delivery( send_frames, base ) ;
//			tris_log ( LOG_WARNING, "base = %d,%d: conf->delivery_time = %d,%d\n",base.tv_sec,base.tv_usec, conf->delivery_time.tv_sec, conf->delivery_time.tv_usec);

			// tris_log( TRIS_CONF_DEBUG, "base => %ld.%ld %d\n", base.tv_sec, base.tv_usec, ( int )( base.tv_usec / 1000 ) ) ;

			conf->stats.frames_in++ ;
		}

		//-----------------//
		// OUTGOING FRAMES //
		//-----------------//

		//
		// loop over member list to queue outgoing frames
		//
		for ( member = conf->memberlist ; member != NULL ; member = member->next )
		{
			member_process_outgoing_frames(conf, member, send_frames);
		}

		//---------//
		// CLEANUP //
		//---------//

		// clean up send frames
		while ( send_frames != NULL )
		{
			// accouting: count all frames and mixed frames
			if ( send_frames->member == NULL )
				conf->stats.frames_out++ ;
			else
				conf->stats.frames_mixed++ ;

			// delete the frame
			send_frames = delete_conf_frame( send_frames ) ;
		}

		//
		// notify the manager of state changes every 100 milliseconds
		// we piggyback on this for VAD switching logic
		//

		if ( ( tris_tvdiff_ms(curr, notify) / TRIS_CONF_NOTIFICATION_SLEEP ) >= 1 )
		{
			// Do VAD switching logic
			// We need to do this here since send_state_change_notifications
			// resets the flags
			if ( !conf->video_locked )
				do_VAD_switching(conf);

			// send the notifications
			send_state_change_notifications( conf->memberlist ) ;

			// increment the notification timer base
			add_milliseconds( &notify, TRIS_CONF_NOTIFICATION_SLEEP ) ;
		}

		// release conference mutex
		tris_mutex_unlock( &conf->lock ) ;

		// !!! TESTING !!!
		// usleep( 1 ) ;
	}
	// end while ( 42 == 42 )

	//
	// exit the conference thread
	//

	tris_log( TRIS_CONF_DEBUG, "exit conference_exec\n" ) ;

	// exit the thread
	pthread_exit( NULL ) ;

	return ;
}

//
// manange conference functions
//

// called by app_conference.c:load_module()
void init_conference( void )
{
	tris_mutex_init( &conflist_lock ) ;
}

struct tris_conference* start_conference( struct tris_conf_member* member )
{
	// check input
	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to handle null member\n" ) ;
		return NULL ;
	}

	struct tris_conference* conf = NULL ;

	// acquire the conference list lock
	tris_mutex_lock(&conflist_lock);



	// look for an existing conference
	tris_log( TRIS_CONF_DEBUG, "attempting to find requested conference\n" ) ;
	conf = find_conf( member->conf_name ) ;

	// unable to find an existing conference, try to create one
	if ( conf == NULL )
	{
		// create a new conference
		tris_log( TRIS_CONF_DEBUG, "attempting to create requested conference\n" ) ;

		// create the new conference with one member
		conf = create_conf( member->conf_name, member ) ;

		// return an error if create_conf() failed
		if ( conf == NULL )
			tris_log( LOG_ERROR, "unable to find or create requested conference\n" ) ;
	}
	else
	{
		//
		// existing conference found, add new member to the conference
		//
		// once we call add_member(), this thread
		// is responsible for calling delete_member()
		//
		add_member( member, conf ) ;
	}

	// release the conference list lock
	tris_mutex_unlock(&conflist_lock);

	return conf ;
}

// This function should be called with conflist_lock mutex being held
struct tris_conference* find_conf( const char* name )
{
	// no conferences exist
	if ( conflist == NULL )
	{
		tris_log( TRIS_CONF_DEBUG, "conflist has not yet been initialized, name => %s\n", name ) ;
		return NULL ;
	}

	struct tris_conference *conf = conflist ;

	// loop through conf list
	while ( conf != NULL )
	{
		if ( strncasecmp( (char*)&(conf->name), name, 80 ) == 0 )
		{
			// found conf name match
			tris_log( TRIS_CONF_DEBUG, "found conference in conflist, name => %s\n", name ) ;
			return conf;
		}
		conf = conf->next ;
	}

	tris_log( TRIS_CONF_DEBUG, "unable to find conference in conflist, name => %s\n", name ) ;
	return NULL;
}

// This function should be called with conflist_lock held
struct tris_conference* create_conf( char* name, struct tris_conf_member* member )
{
	tris_log( TRIS_CONF_DEBUG, "entered create_conf, name => %s\n", name ) ;

	//
	// allocate memory for conference
	//

	struct tris_conference *conf = malloc( sizeof( struct tris_conference ) ) ;

	if ( conf == NULL )
	{
		tris_log( LOG_ERROR, "unable to malloc tris_conference\n" ) ;
		return NULL ;
	}

	//
	// initialize conference
	//

	conf->next = NULL ;
	conf->memberlist = NULL ;

	conf->membercount = 0 ;
	conf->conference_thread = -1 ;

	conf->debug_flag = 0 ;

	conf->id_count = 0;

	conf->default_video_source_id = -1;
	conf->current_video_source_id = -1;
	//conf->current_video_source_timestamp = tris_tvnow();
	conf->video_locked = 0;

	// zero stats
	memset(	&conf->stats, 0x0, sizeof( tris_conference_stats ) ) ;

	// record start time
	conf->stats.time_entered = tris_tvnow();

	// copy name to conference
	strncpy( (char*)&(conf->name), name, sizeof(conf->name) - 1 ) ;
	strncpy( (char*)&(conf->stats.name), name, sizeof(conf->name) - 1 ) ;

	// initialize mutexes
	tris_mutex_init( &conf->lock ) ;

	// build translation paths
	conf->from_slinear_paths[ AC_SLINEAR_INDEX ] = NULL ;
	conf->from_slinear_paths[ AC_ULAW_INDEX ] = tris_translator_build_path( TRIS_FORMAT_ULAW, TRIS_FORMAT_SLINEAR ) ;
	conf->from_slinear_paths[ AC_ALAW_INDEX ] = tris_translator_build_path( TRIS_FORMAT_ALAW, TRIS_FORMAT_SLINEAR ) ;
	conf->from_slinear_paths[ AC_GSM_INDEX ] = tris_translator_build_path( TRIS_FORMAT_GSM, TRIS_FORMAT_SLINEAR ) ;
	conf->from_slinear_paths[ AC_SPEEX_INDEX ] = tris_translator_build_path( TRIS_FORMAT_SPEEX, TRIS_FORMAT_SLINEAR ) ;
#ifdef AC_USE_G729A
	conf->from_slinear_paths[ AC_G729A_INDEX ] = tris_translator_build_path( TRIS_FORMAT_G729A, TRIS_FORMAT_SLINEAR ) ;
#endif

	// add the initial member
	add_member( member, conf ) ;

	tris_log( TRIS_CONF_DEBUG, "added new conference to conflist, name => %s\n", name ) ;

	//
	// spawn thread for new conference, using conference_exec( conf )
	//
	// acquire conference mutexes
	tris_mutex_lock( &conf->lock ) ;

	if ( tris_pthread_create( &conf->conference_thread, NULL, (void*)conference_exec, conf ) == 0 )
	{
		// detach the thread so it doesn't leak
		pthread_detach( conf->conference_thread ) ;

		// prepend new conference to conflist
		conf->next = conflist ;
		conflist = conf ;

		// release conference mutexes
		tris_mutex_unlock( &conf->lock ) ;

		tris_log( TRIS_CONF_DEBUG, "started conference thread for conference, name => %s\n", conf->name ) ;
	}
	else
	{
		tris_log( LOG_ERROR, "unable to start conference thread for conference %s\n", conf->name ) ;

		conf->conference_thread = -1 ;

		// release conference mutexes
		tris_mutex_unlock( &conf->lock ) ;

		// clean up conference
		free( conf ) ;
		conf = NULL ;
	}

	// count new conference
	if ( conf != NULL )
		++conference_count ;

	return conf ;
}

//This function should be called with conflist_lock and conf->lock held
void remove_conf( struct tris_conference *conf )
{
  int c;

	// tris_log( TRIS_CONF_DEBUG, "attempting to remove conference, name => %s\n", conf->name ) ;

	struct tris_conference *conf_current = conflist ;
	struct tris_conference *conf_temp = NULL ;

	// loop through list of conferences
	while ( conf_current != NULL )
	{
		// if conf_current point to the passed conf,
		if ( conf_current == conf )
		{
			if ( conf_temp == NULL )
			{
				// this is the first conf in the list, so we just point
				// conflist past the current conf to the next
				conflist = conf_current->next ;
			}
			else
			{
				// this is not the first conf in the list, so we need to
				// point the preceeding conf to the next conf in the list
				conf_temp->next = conf_current->next ;
			}

			//
			// do some frame clean up
			//

			for ( c = 0 ; c < AC_SUPPORTED_FORMATS ; ++c )
			{
				// free the translation paths
				if ( conf_current->from_slinear_paths[ c ] != NULL )
				{
					tris_translator_free_path( conf_current->from_slinear_paths[ c ] ) ;
					conf_current->from_slinear_paths[ c ] = NULL ;
				}
			}

			// calculate time in conference
			// total time converted to seconds
			long tt = tris_tvdiff_ms(tris_tvnow(),
					conf_current->stats.time_entered) / 1000;

			// report accounting information
			if (conf->debug_flag)
			{
				tris_log( LOG_NOTICE, "conference accounting, fi => %ld, fo => %ld, fm => %ld, tt => %ld\n",
					 conf_current->stats.frames_in, conf_current->stats.frames_out, conf_current->stats.frames_mixed, tt ) ;

				tris_log( TRIS_CONF_DEBUG, "removed conference, name => %s\n", conf_current->name ) ;
			}

			tris_mutex_unlock( &conf_current->lock ) ;

			free( conf_current ) ;
			conf_current = NULL ;

			break ;
		}

		// save a refence to the soon to be previous conf
		conf_temp = conf_current ;

		// move conf_current to the next in the list
		conf_current = conf_current->next ;
	}

	// count new conference
	--conference_count ;

	return ;
}

int get_new_id( struct tris_conference *conf )
{
	// must have the conf lock when calling this
	int newid;
	struct tris_conf_member *othermember;
	// get a video ID for this member
	newid = 0;
	othermember = conf->memberlist;
	while (othermember)
	{
	    if (othermember->id == newid)
	    {
		    newid++;
		    othermember = conf->memberlist;
	    }
	    else
	    {
		    othermember = othermember->next;
	    }
	}
	return newid;
}


int end_conference(const char *name, int hangup )
{
	struct tris_conference *conf;

	// acquire the conference list lock
	tris_mutex_lock(&conflist_lock);

	conf = find_conf(name);
	if ( conf == NULL )
	{
		tris_log( LOG_WARNING, "could not find conference\n" ) ;

		// release the conference list lock
		tris_mutex_unlock(&conflist_lock);

		return -1 ;
	}

	// acquire the conference lock
	tris_mutex_lock( &conf->lock ) ;

	// get list of conference members
	struct tris_conf_member* member = conf->memberlist ;

	// loop over member list and request hangup
	while ( member != NULL )
	{
		// acquire member mutex and request hangup
		// or just kick
		tris_mutex_lock( &member->lock ) ;
		if (hangup)
			tris_softhangup( member->chan, 1 ) ;
		else
			member->kick_flag = 1;
		tris_mutex_unlock( &member->lock ) ;

		// go on to the next member
		// ( we have the conf lock, so we know this is okay )
		member = member->next ;
	}

	// release the conference lock
	tris_mutex_unlock( &conf->lock ) ;

	// release the conference list lock
	tris_mutex_unlock(&conflist_lock);

	return 0 ;
}

//
// member-related functions
//

// This function should be called with conflist_lock held
void add_member( struct tris_conf_member *member, struct tris_conference *conf )
{
        int newid = 0, ltris_id;
        struct tris_conf_member *othermember;
				int count;

	if ( conf == NULL )
	{
		tris_log( LOG_ERROR, "unable to add member to NULL conference\n" ) ;
		return ;
	}

	// acquire the conference lock
	tris_mutex_lock( &conf->lock ) ;

	if (member->id < 0)
	{
		// get an ID for this member
		newid = get_new_id( conf );
		member->id = newid;
	} else
	{
		// boot anyone who has this id already
		othermember = conf->memberlist;
		while (othermember)
		{
			if (othermember->id == member->id)
				othermember->id = -1;
			othermember = othermember->next;
		}
	}

	if ( member->mute_video )
	{
		send_text_message_to_member(member, TRIS_CONF_CONTROL_STOP_VIDEO);
	}

	// set a long term id
	int new_initial_id = 0;
	othermember = conf->memberlist;
	while (othermember)
	{
		if (othermember->initial_id >= new_initial_id)
			new_initial_id++;

		othermember = othermember->next;
	}
	member->initial_id = new_initial_id;


	tris_log( TRIS_CONF_DEBUG, "new video id %d\n", newid) ;

	if (conf->memberlist) ltris_id = conf->memberlist->id;
	else ltris_id = 0;

	if (member->req_id < 0) // otherwise pre-selected in create_member
	{
		// want to watch the last person to 0 or 1 (for now)
		if (member->id > 0) member->req_id = 0;
		else member->req_id = 1;
	}

	member->next = conf->memberlist ; // next is now list
	conf->memberlist = member ; // member is now at head of list

	// update conference stats
	count = count_member( member, conf, 1 ) ;

	tris_log( TRIS_CONF_DEBUG, "member added to conference, name => %s\n", conf->name ) ;

	// release the conference lock
	tris_mutex_unlock( &conf->lock ) ;

	return ;
}

int remove_member( struct tris_conf_member* member, struct tris_conference* conf )
{
	// check for member
	if ( member == NULL )
	{
		tris_log( LOG_WARNING, "unable to remove NULL member\n" ) ;
		return -1 ;
	}

	// check for conference
	if ( conf == NULL )
	{
		tris_log( LOG_WARNING, "unable to remove member from NULL conference\n" ) ;
		return -1 ;
	}

	//
	// loop through the member list looking
	// for the requested member
	//

	tris_mutex_lock( &conf->lock );

	struct tris_conf_member *member_list = conf->memberlist ;
	struct tris_conf_member *member_temp = NULL ;

	int count = -1 ; // default return code

	while ( member_list != NULL )
	{
		// set conference to send no_video to anyone who was watching us
		tris_mutex_lock( &member_list->lock ) ;
		if (member_list->req_id == member->id)
		{
			member_list->conference = 1;
		}
		tris_mutex_unlock( &member_list->lock ) ;
		member_list = member_list->next ;
	}

	member_list = conf->memberlist ;

	int member_is_moderator = member->ismoderator;

	while ( member_list != NULL )
	{
		// If member is driven by the currently visited member, break the association
		if ( member_list->driven_member == member )
		{
			// Acquire member mutex
			tris_mutex_lock(&member_list->lock);

			member_list->driven_member = NULL;

			// Release member mutex
			tris_mutex_unlock(&member_list->lock);
		}

		if ( member_list == member )
		{

			//
			// log some accounting information
			//

			// calculate time in conference (in seconds)
			long tt = tris_tvdiff_ms(tris_tvnow(),
					member->time_entered) / 1000;

			if (conf->debug_flag)
			{
				tris_log(
					LOG_NOTICE,
					"member accounting, channel => %s, te => %ld, fi => %ld, fid => %ld, fo => %ld, fod => %ld, tt => %ld\n",
					member->channel_name,
					member->time_entered.tv_sec, member->frames_in, member->frames_in_dropped,
					member->frames_out, member->frames_out_dropped, tt
					) ;
			}

			//
			// if this is the first member in the linked-list,
			// skip over the first member in the list, else
			//
			// point the previous 'next' to the current 'next',
			// thus skipping the current member in the list
			//
			if ( member_temp == NULL )
				conf->memberlist = member->next ;
			else
				member_temp->next = member->next ;

			// update conference stats
			count = count_member( member, conf, 0 ) ;

			// Check if member is the default or current video source
			if ( conf->current_video_source_id == member->id )
			{
				if ( conf->video_locked )
					unlock_conference(conf->name);
				do_video_switching(conf, conf->default_video_source_id, 0);
			} else if ( conf->default_video_source_id == member->id )
			{
				conf->default_video_source_id = -1;
			}

			// output to manager...
			manager_event(
				EVENT_FLAG_SYSTEM,
				"ConferenceLeave",
				"ConferenceName: %s\r\n"
				"Member: %d\r\n"
				"Channel: %s\r\n"
				"CallerID: %s\r\n"
				"CallerIDName: %s\r\n"
				"Duration: %ld\r\n"
				"Count: %d\r\n",
				conf->name,
				member->id,
				member->channel_name,
				member->callerid,
				member->callername,
				tt, count
			) ;

			// save a pointer to the current member,
			// and then point to the next member in the list
			member_list = member_list->next ;

			// leave member_temp alone.
			// it already points to the previous (or NULL).
			// it will still be the previous after member is deleted

			// delete the member
			delete_member( member ) ;

			tris_log( TRIS_CONF_DEBUG, "removed member from conference, name => %s, remaining => %d\n",
					conf->name, conf->membercount ) ;

			//break ;
		}
		else
		{
			// if member is a moderator, we end the conference when they leave
			if ( member_is_moderator )
			{
				tris_mutex_lock( &member_list->lock ) ;
				member_list->kick_flag = 2;
				tris_mutex_unlock( &member_list->lock ) ;
			}

			// save a pointer to the current member,
			// and then point to the next member in the list
			member_temp = member_list ;
			member_list = member_list->next ;
		}
	}
	tris_mutex_unlock( &conf->lock );

	// return -1 on error, or the number of members
	// remaining if the requested member was deleted
	return count ;
}

int count_member( struct tris_conf_member* member, struct tris_conference* conf, short add_member )
{
	if ( member == NULL || conf == NULL )
	{
		tris_log( LOG_WARNING, "unable to count member\n" ) ;
		return -1 ;
	}

	short delta = ( add_member == 1 ) ? 1 : -1 ;

	// increment member count
	conf->membercount += delta ;

	return conf->membercount ;
}

struct tris_conf_member *find_member (const char *chan, int lock)
{
	struct tris_conf_member *found = NULL;
	struct tris_conf_member *member;
	struct tris_conference *conf;

	tris_mutex_lock( &conflist_lock ) ;

	conf = conflist;

	// loop through conf list
	while ( conf != NULL && !found )
	{
		// lock conference
		tris_mutex_lock( &conf->lock );

		member = conf->memberlist ;

		while (member != NULL)
		{
		    if(!strcmp(member->channel_name, chan)) {
			found = member;
			if(lock)
			    tris_mutex_lock(&member->lock);
			break;
		    }
		    member = member->next;
		}

		// unlock conference
		tris_mutex_unlock( &conf->lock );

		conf = conf->next ;
	}

	// release mutex
	tris_mutex_unlock( &conflist_lock ) ;

	return found;
}

// All the VAD-based video switching magic happens here
// This function should be called inside conference_exec
// The conference mutex should be locked, we don't have to do it here
void do_VAD_switching(struct tris_conference *conf)
{
	struct tris_conf_member *member;
	struct timeval         current_time;
	long                   longest_speaking;
	struct tris_conf_member *longest_speaking_member;
	int                    current_silent, current_no_camera, current_video_mute;
	int                    default_no_camera, default_video_mute;

	current_time = tris_tvnow();

	// Scan the member list looking for the longest speaking member
	// We also check if the currently speaking member has been silent for a while
	// Also, we check for camera disabled or video muted members
	// We say that a member is speaking after his speaking state has been on for
	// at least TRIS_CONF_VIDEO_START_TIMEOUT ms
	// We say that a member is silent after his speaking state has been off for
	// at least TRIS_CONF_VIDEO_STOP_TIMEOUT ms
	longest_speaking = 0;
	longest_speaking_member = NULL;
	current_silent = 0;
	current_no_camera = 0;
	current_video_mute = 0;
	default_no_camera = 0;
	default_video_mute = 0;
	for ( member = conf->memberlist ;
	      member != NULL ;
	      member = member->next )
	{
		// Has the state changed since last time through this loop? Notify!
		if ( member->speaking_state_notify )
		{
/*			fprintf(stderr, "Mihai: member %d, channel %s has changed state to %s\n",
				member->id,
				member->channel_name,
				((member->speaking_state == 1 ) ? "speaking" : "silent")
			       );			*/
		}

		// If a member connects via telephone, they don't have video
		if ( member->via_telephone )
			continue;

		// We check for no VAD switching, video-muted or camera disabled
		// If yes, this member will not be considered as a candidate for switching
		// If this is the currently speaking member, then mark it so we force a switch
		if ( !member->vad_switch )
			continue;

		if ( member->mute_video )
		{
			if ( member->id == conf->default_video_source_id )
				default_video_mute = 1;
			if ( member->id == conf->current_video_source_id )
				current_video_mute = 1;
			else
				continue;
		}

		if ( member->no_camera )
		{
			if ( member->id == conf->default_video_source_id )
				default_no_camera = 1;
			if ( member->id == conf->current_video_source_id )
				current_no_camera = 1;
			else
				continue;
		}

		// Check if current speaker has been silent for a while
		if ( member->id == conf->current_video_source_id &&
		     member->speaking_state == 0 &&
		     tris_tvdiff_ms(current_time, member->ltris_state_change) > TRIS_CONF_VIDEO_STOP_TIMEOUT )
		{
			current_silent = 1;
		}

		// Find a candidate to switch to by looking for the longest speaking member
		// We exclude the current video source from the search
		if ( member->id != conf->current_video_source_id && member->speaking_state == 1 )
		{
			long tmp = tris_tvdiff_ms(current_time, member->ltris_state_change);
			if ( tmp > TRIS_CONF_VIDEO_START_TIMEOUT && tmp > longest_speaking )
			{
				longest_speaking = tmp;
				longest_speaking_member = member;
			}
		}
	}

	// We got our results, now let's make a decision
	// If the currently speaking member has been marked as silent, then we take the longest
	// speaking member.  If no member is speaking, we go to default
	// As a policy we don't want to switch away from a member that is speaking
	// however, we might need to refine this to avoid a situation when a member has a
	// low noise threshold or its VAD is simply stuck
	if ( current_silent || current_no_camera || current_video_mute || conf->current_video_source_id < 0 )
	{
		if ( longest_speaking_member != NULL )
		{
			do_video_switching(conf, longest_speaking_member->id, 0);
		} else
		{
			// If there's nobody speaking and we have a default that can send video, switch to it
			// If not, then switch to empty (-1)
			if ( conf->default_video_source_id >= 0 &&
			     !default_no_camera &&
			     !default_video_mute
			   )
				do_video_switching(conf, conf->default_video_source_id, 0);
			else
				do_video_switching(conf, -1, 0);
		}
	}
}

int unlock_conference(const char *conference)
{
	struct tris_conference  *conf;
	int                   res;

	if ( conference == NULL )
		return -1;

	// acquire conference list mutex
	tris_mutex_lock( &conflist_lock ) ;

	// Look for conference
	res = 0;
	for ( conf = conflist ; conf != NULL ; conf = conf->next )
	{
		if ( strcmp(conference, conf->name) == 0 )
		{
			conf->video_locked = 0;
			manager_event(EVENT_FLAG_SYSTEM, "ConferenceUnlock", "ConferenceName: %s\r\n", conf->name);
			do_video_switching(conf, conf->default_video_source_id, 0);
			res = 1;

			break;
		}
	}

	// release conference list mutex
	tris_mutex_unlock( &conflist_lock ) ;

	return res;
}

// Creates a text frame and sends it to a given member
// Returns 0 on success, -1 on failure
int send_text_message_to_member(struct tris_conf_member *member, const char *text)
{
	struct tris_frame *f;

	if ( member == NULL || text == NULL ) return -1;

	if ( member->does_text )
	{
		f = create_text_frame(text, 1);
		if ( f == NULL || queue_outgoing_text_frame(member, f) != 0) return -1;
		tris_frfree(f);
	}

	return 0;
}

// Switches video source
// Sends a manager event as well as
// a text message notifying members of a video switch
// The notification is sent to the current member and to the new member
// The function locks the conference mutex as required
void do_video_switching(struct tris_conference *conf, int new_id, int lock)
{
	struct tris_conf_member *member;
	struct tris_conf_member *new_member = NULL;

	if ( conf == NULL ) return;

	if ( lock )
	{
		// acquire conference mutex
		tris_mutex_lock( &conf->lock );
	}

	//fprintf(stderr, "Mihai: video switch from %d to %d\n", conf->current_video_source_id, new_id);

	// No need to do anything if the current member is the same as the new member
	if ( new_id != conf->current_video_source_id )
	{
		for ( member = conf->memberlist ; member != NULL ; member = member->next )
		{
			if ( member->id == conf->current_video_source_id )
			{
				send_text_message_to_member(member, TRIS_CONF_CONTROL_STOP_VIDEO);
			}
			if ( member->id == new_id )
			{
				send_text_message_to_member(member, TRIS_CONF_CONTROL_START_VIDEO);
				new_member = member;
			}
		}

		conf->current_video_source_id = new_id;

		if ( new_member != NULL )
		{
			manager_event(EVENT_FLAG_SYSTEM,
				"ConferenceVideoSwitch",
				"ConferenceName: %s\r\nChannel: %s\r\n",
				conf->name,
				new_member->channel_name);
		} else
		{
			manager_event(EVENT_FLAG_SYSTEM,
				"ConferenceVideoSwitch",
				"ConferenceName: %s\r\nChannel: empty\r\n",
				conf->name);
		}
	}

	if ( lock )
	{
		// release conference mutex
		tris_mutex_unlock( &conf->lock );
	}
}
