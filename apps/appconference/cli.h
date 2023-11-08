
// $Id: cli.h 880 2007-04-25 15:23:59Z jpgrayson $

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

#ifndef _APP_CONF_CLI_H
#define _APP_CONF_CLI_H

//
// includes
//

#include "app_conference.h"
#include "common.h"

//
// function declarations
//

char* conference_show_stats( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_show_stats_name( int fd, const char* name ) ;

char* conference_restart(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);

char* conference_debug(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) ;
char* conference_no_debug( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;

char* conference_list( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_kick( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_kickchannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;

char* conference_mute( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_unmute( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_mutechannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_unmutechannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_viewstream( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_viewchannel( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;

char* conference_play_sound( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_stop_sounds( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;

char* conference_play_video( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;
char* conference_stop_videos( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;

char* conference_end( struct tris_cli_entry *e, int cmd, struct tris_cli_args *a ) ;

char* conference_add(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
char* conference_remove(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
char* conference_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);
char* conference_set(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a);

int manager_conference_end(struct mansession *s, const struct message *m);

void register_conference_cli( void ) ;
void unregister_conference_cli( void ) ;


#endif
