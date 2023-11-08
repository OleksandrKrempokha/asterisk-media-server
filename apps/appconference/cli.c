
// $Id: cli.c 884 2007-06-27 14:56:21Z sbalea $

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

#include "trismedia/autoconfig.h"
#include "cli.h"
#include "trismedia/astdb.h"	/* trisdb */


struct trisdb_conf_entry {
	char id[64];
	char title[88];
	char adminpin[88];
	char memberpin[88];
	char admins[1024];
	char members[1024];
	struct trisdb_conf_entry *next;
};

static char *conf_para1_list[] = {
	"title",
	"adminpin",
	"memberpin",
	"admins",
	"members"
};

static char conference_restart_usage[] =
	"usage: conference restart\n"
	"       kick all users in all conferences\n"
;

char* conference_restart(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference restart";
		e->usage = conference_restart_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	
	if ( a->argc < 2 )
		return CLI_SHOWUSAGE ;

	kick_all();
	return CLI_SUCCESS ;
}


//
// debug functions
//

static char conference_debug_usage[] =
	"usage: conference debug <conference_name> [ on | off ]\n"
	"       enable debugging for a conference\n"
;

char* conference_debug(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference debug";
		e->usage = conference_debug_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 3 )
		return CLI_SHOWUSAGE ;

	// get the conference name
	const char* name = a->argv[2] ;

   	// get the new state
	int state = 0 ;

	if ( a->argc == 3 )
	{
		// no state specified, so toggle it
		state = -1 ;
	}
	else
	{
		if ( strncasecmp( a->argv[3], "on", 4 ) == 0 )
			state = 1 ;
		else if ( strncasecmp( a->argv[3], "off", 3 ) == 0 )
			state = 0 ;
		else
			return CLI_SHOWUSAGE ;
	}

	int new_state = set_conference_debugging( name, state ) ;

	if ( new_state == 1 )
	{
		tris_cli( a->fd, "enabled conference debugging, name => %s, new_state => %d\n",
			name, new_state ) ;
	}
	else if ( new_state == 0 )
	{
		tris_cli( a->fd, "disabled conference debugging, name => %s, new_state => %d\n",
			name, new_state ) ;
	}
	else
	{
		// error setting state
		tris_cli( a->fd, "\nunable to set debugging state, name => %s\n\n", name ) ;
	}

	return CLI_SUCCESS ;
}

//
// stats functions
//

static char conference_show_stats_usage[] =
	"usage: conference showstats\n"
	"       display stats for active conferences.\n"
;

char* conference_show_stats( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference showstats";
		e->usage = conference_show_stats_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 2 )
		return CLI_SHOWUSAGE ;

	// get count of active conferences
	int count = get_conference_count() ;

	tris_cli( a->fd, "\n\nCONFERENCE STATS, ACTIVE( %d )\n\n", count ) ;

	// if zero, go no further
	if ( count <= 0 )
		return CLI_SUCCESS ;

	//
	// get the conference stats
	//

	// array of stats structs
	tris_conference_stats stats[ count ] ;

	// get stats structs
	count = get_conference_stats( stats, count ) ;

	// make sure we were able to fetch some
	if ( count <= 0 )
	{
		tris_cli( a->fd, "!!! error fetching conference stats, available => %d !!!\n", count ) ;
		return CLI_SUCCESS ;
	}

	//
	// output the conference stats
	//

	// output header
	tris_cli( a->fd, "%-20.20s  %-40.40s\n", "Name", "Stats") ;
	tris_cli( a->fd, "%-20.20s  %-40.40s\n", "----", "-----") ;

	tris_conference_stats* s = NULL ;

	int i;

	for ( i = 0 ; i < count ; ++i )
	{
		s = &(stats[i]) ;

		// output this conferences stats
		tris_cli( a->fd, "%-20.20s\n", (char*)( &(s->name) )) ;
	}

	tris_cli( a->fd, "\n" ) ;

	//
	// drill down to specific stats
	//

	if ( a->argc == 4 )
	{
		// show stats for a particular conference
		conference_show_stats_name( a->fd, a->argv[3] ) ;
	}

	return CLI_SUCCESS ;
}

char* conference_show_stats_name( int fd, const char* name )
{
	// not implemented yet
	return CLI_SUCCESS ;
}

static char conference_list_usage[] =
	"usage: conference list {<conference_name>}\n"
	"       list members of a conference\n"
;

char* conference_list( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	int index;
	
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference list";
		e->usage = conference_list_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 2 )
		return CLI_SHOWUSAGE ;

	if (a->argc >= 3)
	{
		for (index = 2; index < a->argc; index++)
		{
			// get the conference name
			const char* name = a->argv[index] ;
			show_conference_list( a->fd, name );
		}
	}
	else
	{
		show_conference_stats(a->fd);
	}
	return CLI_SUCCESS ;
}

static char conference_kick_usage[] =
	"usage: conference kick <conference> <member id>\n"
	"       kick member <member id> from conference <conference>\n"
;

char* conference_kick( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference kick";
		e->usage = conference_kick_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	
	if ( a->argc < 4 )
		return CLI_SHOWUSAGE ;

	// get the conference name
	const char* name = a->argv[2] ;

	int member_id;
	sscanf(a->argv[3], "%d", &member_id);

	int res = kick_member( name, member_id );

	if (res) tris_cli( a->fd, "User #: %d kicked\n", member_id) ;

	return CLI_SUCCESS ;
}

static char conference_kickchannel_usage[] =
	"usage: conference kickchannel <conference_name> <channel>\n"
	"       kick channel from conference\n"
;

char* conference_kickchannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference kickchannel";
		e->usage = conference_kickchannel_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 4 )
		return CLI_SHOWUSAGE ;

	const char *name = a->argv[2] ;
	const char *channel = a->argv[3];

	int res = kick_channel( name, channel );

	if ( !res )
	{
		tris_cli( a->fd, "Cannot kick channel %s in conference %s\n", channel, name);
		return CLI_FAILURE;
	}

	return CLI_SUCCESS ;
}

static char conference_mute_usage[] =
	"usage: conference mute <conference_name> <member id>\n"
	"       mute member in a conference\n"
;

char* conference_mute( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference mute";
		e->usage = conference_mute_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 4 )
		return CLI_SHOWUSAGE ;

	// get the conference name
	const char* name = a->argv[2] ;

	int member_id;
	sscanf(a->argv[3], "%d", &member_id);

	int res = mute_member( name, member_id );

	if (res) tris_cli( a->fd, "User #: %d muted\n", member_id) ;

	return CLI_SUCCESS ;
}

static char conference_mutechannel_usage[] =
	"usage: conference mutechannel <channel>\n"
	"       mute channel in a conference\n"
;

char* conference_mutechannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
  	struct tris_conf_member *member;
	char *channel;
	
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference mutechannel";
		e->usage = conference_mutechannel_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	
	if ( a->argc < 3 )
		return CLI_SHOWUSAGE ;

	channel = a->argv[2];

	member = find_member(channel, 1);
	if(!member) {
	    tris_cli(a->fd, "Member %s not found\n", channel);
	    return CLI_FAILURE;
	}

	member->mute_audio = 1;
	tris_mutex_unlock( &member->lock ) ;

	tris_cli( a->fd, "Channel #: %s muted\n", a->argv[2]) ;

	return CLI_SUCCESS ;
}

static char conference_viewstream_usage[] =
	"usage: conference viewstream <conference_name> <member id> <stream no>\n"
	"       member <member id> will receive video stream <stream no>\n"
;

char* conference_viewstream( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	int res;
	
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference viewstream";
		e->usage = conference_viewstream_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	
	if ( a->argc < 5 )
		return CLI_SHOWUSAGE ;

	// get the conference name
	const char* switch_name = a->argv[2] ;

	int member_id, viewstream_id;
	sscanf(a->argv[3], "%d", &member_id);
	sscanf(a->argv[4], "%d", &viewstream_id);

	res = viewstream_switch( switch_name, member_id, viewstream_id );

	if (res) tris_cli( a->fd, "User #: %d viewing %d\n", member_id, viewstream_id) ;

	return CLI_SUCCESS ;
}

static char conference_viewchannel_usage[] =
	"usage: conference viewchannel <conference_name> <dest channel> <src channel>\n"
	"       channel <dest channel> will receive video stream <src channel>\n"
;

char* conference_viewchannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference viewchannel";
		e->usage = conference_viewchannel_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}
	
	if ( a->argc < 5 )
		return CLI_SHOWUSAGE ;

	// get the conference name
	const char* switch_name = a->argv[2] ;

	res = viewchannel_switch( switch_name, a->argv[3], a->argv[4] );

	if (res) tris_cli( a->fd, "Channel #: %s viewing %s\n", a->argv[3], a->argv[4]) ;

	return CLI_SUCCESS ;
}

static char conference_unmute_usage[] =
	"usage: conference unmute <conference_name> <member id>\n"
	"       unmute member in a conference\n"
;

char* conference_unmute( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference unmute";
		e->usage = conference_unmute_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 4 )
		return CLI_SHOWUSAGE ;

	// get the conference name
	const char* name = a->argv[2] ;

	int member_id;
	sscanf(a->argv[3], "%d", &member_id);

	int res = unmute_member( name, member_id );

	if (res) tris_cli( a->fd, "User #: %d unmuted\n", member_id) ;

	return CLI_SUCCESS ;
}

static char conference_unmutechannel_usage[] =
	"usage: conference unmutechannel <channel>\n"
	"       unmute channel in a conference\n"
;

char* conference_unmutechannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	struct tris_conf_member *member;
	char *channel;

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference unmutechannel";
		e->usage = conference_unmutechannel_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 3 )
		return CLI_SHOWUSAGE ;

	channel = a->argv[2];

	member = find_member(channel, 1);
	if(!member) {
	    tris_cli(a->fd, "Member %s not found\n", channel);
	    return CLI_FAILURE;
	}

	member->mute_audio = 0;
	tris_mutex_unlock( &member->lock ) ;

	tris_cli( a->fd, "Channel #: %s unmuted\n", a->argv[2]) ;

	return CLI_SUCCESS ;
}

//
// play sound
//
static char conference_play_sound_usage[] =
	"usage: conference play sound <channel-id> <sound-file> [mute]\n"
	"       play sound <sound-file> to conference member <channel-id>.\n"
	"       If mute is specified, all other audio is muted while the sound is played back.\n"
;

char* conference_play_sound( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	char *channel, *file;
	int mute = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference play sound";
		e->usage = conference_play_sound_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 5 )
		return CLI_SHOWUSAGE ;

	channel = a->argv[3];
	file = a->argv[4];

	if(a->argc > 5 && !strcmp(a->argv[5], "mute"))
	    mute = 1;

	int res = play_sound_channel(a->fd, channel, file, mute);

	if ( !res )
	{
		tris_cli(a->fd, "Sound playback failed failed\n");
		return CLI_FAILURE;
	}
	return CLI_SUCCESS ;
}

//
// stop sounds
//

static char conference_stop_sounds_usage[] =
	"usage: conference stop sounds <channel-id>\n"
	"       stop sounds for conference member <channel-id>.\n"
;

char* conference_stop_sounds( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{
	char *channel;

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference stop sounds";
		e->usage = conference_stop_sounds_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	if ( a->argc < 4 )
		return CLI_SHOWUSAGE ;

	channel = a->argv[3];

	int res = stop_sound_channel(a->fd, channel);

	if ( !res )
	{
		tris_cli(a->fd, "Sound stop failed failed\n");
		return CLI_FAILURE;
	}
	return CLI_SUCCESS ;
}

//
// end conference
//

static char conference_end_usage[] =
	"usage: conference end <conference name>\n"
	"       ends a conference.\n"
;

char* conference_end( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a )
{

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference end";
		e->usage = conference_end_usage;
		return NULL;
	case CLI_GENERATE:
		return NULL;	
	}

	// check the args length
	if ( a->argc < 3 )
		return CLI_SHOWUSAGE ;

	// conference name
	const char* name = a->argv[2] ;

	// get the conference
	if ( end_conference( name, 1 ) != 0 )
	{
		tris_cli( a->fd, "Failed! unable to end the conference, name => %s\n", name ) ;
		return CLI_SHOWUSAGE ;
	} else {
		tris_cli( a->fd, "OK! Successfully completed!");
	}

	return CLI_SUCCESS ;
}

//
// E.BUU - Manager conference end. Additional option to just kick everybody out
// without hangin up channels
//
int manager_conference_end(struct mansession *s, const struct message *m)
{
	const char *confname = astman_get_header(m,"Conference");
	int hangup = 1;

	const char * h =  astman_get_header(m, "Hangup");
	if (h)
	{
		hangup = atoi(h);
	}

	tris_log( LOG_NOTICE, "Terminating conference %s on manager's request. Hangup: %s.\n", confname, hangup?"YES":"NO" );
        if ( end_conference( confname, hangup ) != 0 )
        {
		tris_log( LOG_ERROR, "manager end conf: unable to terminate conference %s.\n", confname );
		astman_send_error(s, m, "Failed to terminate\r\n");
		return RESULT_FAILURE;
	}

	astman_send_ack(s, m, "Conference terminated");
	return RESULT_SUCCESS;
}

static char *complete_conf_parameter(struct tris_cli_args *a)
{
	struct tris_db_entry *db_entry=NULL, *db_tree=NULL;
	const char name[16] = "CONFERENCE";
	char *result = NULL;
	int i, which = 0;
	int wordlen = strlen( a->word );

	if ( a->pos == 2 ) {
		db_entry = db_tree = tris_db_gettree(name, NULL);
		for ( ; db_entry; db_entry = db_entry->next ) {
			if ( !strchr( db_entry->key + strlen(name) + 2, '/' )) {
				if ( !strncasecmp( a->word, db_entry->key + strlen(name) + 2, wordlen ) && ++which > a->n ) {
					result = tris_strdup( db_entry->key + strlen(name) + 2 );
					break;
				}
			}
		}
		tris_db_freetree( db_tree );
		db_tree = NULL;
	} else if ( a->pos > 2 && a->pos%2) {
		for(i = 0; i < ARRAY_LEN(conf_para1_list); i++) {
			if (!strncasecmp( a->word, conf_para1_list[i], wordlen ) && ++which > a->n) {
				result = tris_strdup( conf_para1_list[i] );
				break;
			}
		}

	}

	return result;
}

static inline int trisdb_conference_add(const char *conf_name, const char *type)
{
	char buf_data[128] = "";
	int res;
	
	res = tris_db_get("CONFERENCE", conf_name, buf_data, sizeof(buf_data));
	if (!res){
		return -100; // The Conference is already added.
	}else{
		res = tris_db_put("CONFERENCE", conf_name, conf_name);
	}
	return res;
}

static inline int trisdb_conference_set(struct tris_cli_args *a)
{
	unsigned i, j;
	int exist_flg = 0;
	int res = 0;
	char *ext;
	char type[32], db_key[88];
	
	for(i=3; i<a->argc; i+=2) {
		exist_flg = 0;
		for(j = 0; j < ARRAY_LEN(conf_para1_list); j++) {
			if (!strcasecmp( a->argv[i], conf_para1_list[j] ) ) {
				exist_flg = 1;
				break;
			}
		}
		if(exist_flg) {
			if (!strcasecmp(a->argv[i], "admins") || !strcasecmp(a->argv[i], "members")) {
				strcpy(type, a->argv[i]);
				type[strlen(type) - 1] = '\0';
				while ((ext = strsep(&a->argv[i+1], ","))) {
					snprintf(db_key, sizeof(db_key), "%s/%s/%s", a->argv[2], type, ext);
					res = tris_db_put("CONFERENCE", db_key, "");
					if (res)  {
						tris_cli(a->fd, "Failed! Failed to set %s for <%s>\n", type, ext);
					} else {
						tris_cli(a->fd, "OK! Set %s for <%s> successfully\n", type, ext);
					}
				}
			} else {
				snprintf(db_key, sizeof(db_key), "%s/%s", a->argv[2], a->argv[i]);
				res = tris_db_put("CONFERENCE", db_key, a->argv[i+1]);
				if (res)  {
					tris_cli(a->fd, "Failed! Failed to set %s.\n", a->argv[i]);
				} else {
					tris_cli(a->fd, "OK! Set %s for <%s> successfully\n", a->argv[i],  a->argv[i+1]);
				}
			} 
		} else {
			tris_cli(a->fd, "Failed! Invalid Parameter %s \n", a->argv[i]);
		}
	}
	return res;

}

/*! \brief  CLI Command 'Conference Add' */
char* conference_add(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference add";
		e->usage =
			"Usage: conference add <conference name> [<key> <value> ...]\n"
			"       Add a Conference in the Trismedia database for a given conference name.\n";
		return NULL;
	case CLI_GENERATE:
		return complete_conf_parameter( a);
	}

	if (a->argc < 3 || !a->argc%2)
		return CLI_SHOWUSAGE;

	//if (tris_check_extension(a->argv[2])) {
	//	tris_cli(a->fd, "Failed! Extension is duplicated\n");
	//	return CLI_SUCCESS;
	//}
	res = trisdb_conference_add(a->argv[2], "conference");
	if (!res){
		tris_cli(a->fd, "OK! Added Conference %s successfully.\n", a->argv[2]);
		res = trisdb_conference_set(a);
	}else if (res == -100)
		tris_cli(a->fd, "Failed! Conference %s is already existing.\n", a->argv[2]);
	else
		tris_cli(a->fd, "Failed! Failed to add Conference %s.\n", a->argv[2]);

	return CLI_SUCCESS;
}

/*! \brief  CLI Command 'Conference Remove' */
char* conference_remove(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "conference remove";
		e->usage =
			"Usage: conference remove <conference name>\n"
			"       Remove a Conference in the Trismedia database for a given conference name.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 3)
		return CLI_SHOWUSAGE;

	res = tris_db_deltree("CONFERENCE", a->argv[2]);
	if (res <= 0) {
		tris_cli(a->fd, "Failed! Conference <%s> do not exist.\n", a->argv[2]);
	} else {
		tris_cli(a->fd, "OK! Conference <%s> removed.\n", a->argv[2]);
	}

	return CLI_SUCCESS;
}

static inline int trisdb_free_conf_list(struct trisdb_conf_entry *entry)
{
	struct trisdb_conf_entry *last;
	while (entry) {
		last = entry;
		entry = entry->next;
		tris_free(last);
	}
	return 0;
}

static inline struct trisdb_conf_entry* trisdb_conference_get(const char *confname)
{
	char *parameter, *id;
	struct tris_db_entry *dbtree = NULL, *tmp = NULL;
	struct trisdb_conf_entry *conf_entry = NULL, *new_conf_entry = NULL, *ret = NULL;
	char ext[88];
	
	dbtree = tris_db_gettree("CONFERENCE", confname);
	
	if (!dbtree) {
		return NULL;
	}
	
	for (tmp = dbtree; tmp; tmp = tmp->next) {
		parameter = tris_strdupa(tmp->key);
		id = strsep(&parameter, "/");
		id = strsep(&parameter, "/");
		id = strsep(&parameter, "/");
		
		if (!parameter) { //conference descriptor

			new_conf_entry = tris_malloc(sizeof(struct trisdb_conf_entry));
			memset(new_conf_entry, 0, sizeof(struct trisdb_conf_entry));
			tris_copy_string(new_conf_entry->id, id, sizeof(new_conf_entry->id));
			if (conf_entry) {
				conf_entry->next = new_conf_entry;
			} else {
				ret = new_conf_entry;
			}
			conf_entry = new_conf_entry;
		} else {
			if (!new_conf_entry) {
				/* oh invalid conference */
				continue;
			}
			if (!strncmp(parameter, "admin/",  6)) {
				snprintf(ext, sizeof(ext), "%s,", strchr(parameter, '/')+1);
				strcat(new_conf_entry->admins, ext);
			} else if (!strncmp(parameter, "member/",  7)) {
				snprintf(ext, sizeof(ext), "%s,", strchr(parameter, '/')+1);
				strcat(new_conf_entry->members, ext);
			} else if (!strcmp(parameter, "adminpin")) {
				tris_copy_string(new_conf_entry->adminpin, tmp->data, sizeof(new_conf_entry->adminpin));
			} else if (!strcmp(parameter, "memberpin")) {
				tris_copy_string(new_conf_entry->memberpin, tmp->data, sizeof(new_conf_entry->memberpin));
			} else if (!strcmp(parameter, "title")) {
				tris_copy_string(new_conf_entry->title, tmp->data, sizeof(new_conf_entry->title));
			}
		}
	}
	
	tris_db_freetree(dbtree);
	
	return ret;


}

/*! \brief  CLI Command 'Conference show' */
char* conference_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct trisdb_conf_entry *conf_entry = NULL, *tmp = NULL;
	int count = 0;
	
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference show";
		e->usage =
			"Usage: conference show [conference name]\n"
			"       Show a conference in the Trismedia database for a given conference name.\n"
			"       if conference name is not specified, it will show all the conferences.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 2)
		return CLI_SHOWUSAGE;

	conf_entry = trisdb_conference_get(a->argc >= 3 ? a->argv[2] : NULL);
	tris_cli(a->fd, "OK!");
	if (conf_entry) {
		
		for (tmp = conf_entry; tmp; tmp = tmp->next) {
			tmp->admins[ strlen(tmp->admins) - 1] = '\0';
			tmp->members[ strlen(tmp->members) - 1] = '\0';
			tris_cli(a->fd, " \n* Conference : %s\n", tmp->id);
			tris_cli(a->fd, "  %-18s : %s\n", "title", tmp->title);
			tris_cli(a->fd, "  %-18s : %s\n", "adminpin", tmp->adminpin);
			tris_cli(a->fd, "  %-18s : %s\n", "memberpin", tmp->memberpin);
			tris_cli(a->fd, "  %-18s : %s\n", "admins", tmp->admins);
			tris_cli(a->fd, "  %-18s : %s\n", "members", tmp->members);
			count++;
		}
		
		trisdb_free_conf_list(conf_entry);
	}
	
	if ( count == 0 )
		tris_cli( a->fd, "\nConference not found.\n" );
	else {
		if ( count == 1 )
			tris_cli( a->fd, "\nThere is %d conference.\n", count );
		else
			tris_cli( a->fd, "\nThere are %d conferences.\n", count );
	}

	return CLI_SUCCESS;
}

/*! \brief  CLI Command 'Conference set' */
char* conference_set(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;
	char buf_data[128] = "";
	
	switch (cmd) {
	case CLI_INIT:
		e->command = "conference set";
		e->usage =
			"Usage: conference set <conference name> <key> <value> [...]\n";
		return NULL;
	case CLI_GENERATE:
		return complete_conf_parameter( a);
	}

	if (a->argc < 5 || !a->argc%2)
		return CLI_SHOWUSAGE;

	res = tris_db_get("CONFERENCE", a->argv[2], buf_data, sizeof(buf_data));
	if (res){
		tris_cli(a->fd, "Failed! Conference %s is not found\n", a->argv[2]);
	} else {
		res = trisdb_conference_set(a);
	}
	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_conference[] = {
	TRIS_CLI_DEFINE(conference_restart, "restart a conference"),
	TRIS_CLI_DEFINE(conference_debug, "enable debugging for a conference"),
	TRIS_CLI_DEFINE(conference_show_stats, "show conference stats"),
	TRIS_CLI_DEFINE(conference_list, "list members of a conference"),
	TRIS_CLI_DEFINE(conference_kick, "kick member from a conference"),
	TRIS_CLI_DEFINE(conference_kickchannel, "kick channel from conference"),
	TRIS_CLI_DEFINE(conference_mute, "mute member in a conference"),
	TRIS_CLI_DEFINE(conference_mutechannel, "mute channel in a conference"),
	TRIS_CLI_DEFINE(conference_viewstream, "switch view in a conference"),
	TRIS_CLI_DEFINE(conference_viewchannel, "switch channel in a conference"),
	TRIS_CLI_DEFINE(conference_unmute, "unmute member in a conference"),
	TRIS_CLI_DEFINE(conference_unmutechannel, "unmute channel in a conference"),
	TRIS_CLI_DEFINE(conference_play_sound, "play a sound to a conference member"),
	TRIS_CLI_DEFINE(conference_stop_sounds, "stop sounds for a conference member"),
	TRIS_CLI_DEFINE(conference_end, "stops a conference"),
	TRIS_CLI_DEFINE(conference_add, "add a conference"),
	TRIS_CLI_DEFINE(conference_remove, "remove a conference"),
	TRIS_CLI_DEFINE(conference_show, "show conferences"),
	TRIS_CLI_DEFINE(conference_set, "set info of a conference")
};


//
// cli initialization function
//

void register_conference_cli( void )
{
	tris_cli_register_multiple(cli_conference, ARRAY_LEN(cli_conference));
/*
	tris_cli_register( &cli_restart );
	tris_cli_register( &cli_debug ) ;
	tris_cli_register( &cli_show_stats ) ;
	tris_cli_register( &cli_list );
	tris_cli_register( &cli_kick );
	tris_cli_register( &cli_kickchannel );
	tris_cli_register( &cli_mute );
	tris_cli_register( &cli_mutechannel );
	tris_cli_register( &cli_viewstream );
	tris_cli_register( &cli_viewchannel );
	tris_cli_register( &cli_unmute );
	tris_cli_register( &cli_unmutechannel );
	tris_cli_register( &cli_play_sound ) ;
	tris_cli_register( &cli_stop_sounds ) ;
	tris_cli_register( &cli_end );
	tris_cli_register( &cli_lock );
	tris_cli_register( &cli_lockchannel );
	tris_cli_register( &cli_unlock );
	tris_cli_register( &cli_set_default );
	tris_cli_register( &cli_set_defaultchannel );
	tris_cli_register( &cli_video_mute ) ;
	tris_cli_register( &cli_video_unmute ) ;
	tris_cli_register( &cli_video_mutechannel ) ;
	tris_cli_register( &cli_video_unmutechannel ) ;
	tris_cli_register( &cli_text );
	tris_cli_register( &cli_textchannel );
	tris_cli_register( &cli_textbroadcast );
	tris_cli_register( &cli_drive );
	tris_cli_register( &cli_drivechannel ); */
	tris_manager_register( "ConferenceList", EVENT_FLAG_CALL, manager_conference_list, "Conference List" );
	tris_manager_register( "ConferenceEnd", EVENT_FLAG_CALL, manager_conference_end, "Terminate a conference" );

}

void unregister_conference_cli( void )
{
	tris_cli_unregister_multiple(cli_conference, ARRAY_LEN(cli_conference));	
	
/*	tris_cli_unregister( &cli_restart );
	tris_cli_unregister( &cli_debug ) ;
	tris_cli_unregister( &cli_show_stats ) ;
	tris_cli_unregister( &cli_list );
	tris_cli_unregister( &cli_kick );
	tris_cli_unregister( &cli_kickchannel );
	tris_cli_unregister( &cli_mute );
	tris_cli_unregister( &cli_mutechannel );
	tris_cli_unregister( &cli_viewstream );
	tris_cli_unregister( &cli_viewchannel );
	tris_cli_unregister( &cli_unmute );
	tris_cli_unregister( &cli_unmutechannel );
	tris_cli_unregister( &cli_play_sound ) ;
	tris_cli_unregister( &cli_stop_sounds ) ;
	tris_cli_unregister( &cli_end );
	tris_cli_unregister( &cli_lock );
	tris_cli_unregister( &cli_lockchannel );
	tris_cli_unregister( &cli_unlock );
	tris_cli_unregister( &cli_set_default );
	tris_cli_unregister( &cli_set_defaultchannel );
	tris_cli_unregister( &cli_video_mute ) ;
	tris_cli_unregister( &cli_video_unmute ) ;
	tris_cli_unregister( &cli_video_mutechannel ) ;
	tris_cli_unregister( &cli_video_unmutechannel ) ;
	tris_cli_unregister( &cli_text );
	tris_cli_unregister( &cli_textchannel );
	tris_cli_unregister( &cli_textbroadcast );
	tris_cli_unregister( &cli_drive );
	tris_cli_unregister( &cli_drivechannel ); */
	tris_manager_unregister( "ConferenceList" );
	tris_manager_unregister( "ConferenceEnd" );
}
