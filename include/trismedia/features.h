/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Call Parking and Pickup API 
 * Includes code and algorithms from the Zapata library.
 */

#ifndef _TRIS_FEATURES_H
#define _TRIS_FEATURES_H

#include "trismedia/pbx.h"
#include "trismedia/linkedlists.h"

#define FEATURE_MAX_LEN		11
#define FEATURE_APP_LEN		64
#define FEATURE_APP_ARGS_LEN	256
#define FEATURE_SNAME_LEN	32
#define FEATURE_EXTEN_LEN	32
#define FEATURE_MOH_LEN		80  /* same as MAX_MUSICCLASS from channel.h */

#define PARK_APP_NAME "Park"

/*! \brief main call feature structure */

enum {
	TRIS_FEATURE_FLAG_NEEDSDTMF = (1 << 0),
	TRIS_FEATURE_FLAG_ONPEER =    (1 << 1),
	TRIS_FEATURE_FLAG_ONSELF =    (1 << 2),
	TRIS_FEATURE_FLAG_BYCALLEE =  (1 << 3),
	TRIS_FEATURE_FLAG_BYCALLER =  (1 << 4),
	TRIS_FEATURE_FLAG_BYBOTH	 =   (3 << 3),
};

struct tris_call_feature {
	int feature_mask;
	char *fname;
	char sname[FEATURE_SNAME_LEN];
	char exten[FEATURE_MAX_LEN];
	char default_exten[FEATURE_MAX_LEN];
	int (*operation)(struct tris_channel *chan, struct tris_channel *peer, struct tris_bridge_config *config, char *code, int sense, void *data);
	unsigned int flags;
	char app[FEATURE_APP_LEN];		
	char app_args[FEATURE_APP_ARGS_LEN];
	char moh_class[FEATURE_MOH_LEN];
	TRIS_LIST_ENTRY(tris_call_feature) feature_entry;
};

#define TRIS_FEATURE_RETURN_SWITCHTRANSFEREE         -2
#define TRIS_FEATURE_RETURN_HANGUP                   -1
#define TRIS_FEATURE_RETURN_SUCCESSBREAK             0
#define TRIS_FEATURE_RETURN_PASSDIGITS               21
#define TRIS_FEATURE_RETURN_STOREDIGITS              22
#define TRIS_FEATURE_RETURN_SUCCESS                  23
#define TRIS_FEATURE_RETURN_KEEPTRYING               24
#define TRIS_FEATURE_RETURN_PARKFAILED               25

/*!
 * \brief Park a call and read back parked location 
 * \param chan the channel to actually be parked
 * \param host the channel which will have the parked location read to.
 * \param timeout is a timeout in milliseconds
 * \param extout is a parameter to an int that will hold the parked location, or NULL if you want.
 * 
 * Park the channel chan, and read back the parked location to the host. 
 * If the call is not picked up within a specified period of time, 
 * then the call will return to the last step that it was in 
 * (in terms of exten, priority and context)
 * \retval 0 on success.
 * \retval -1 on failure.
*/
int tris_park_call(struct tris_channel *chan, struct tris_channel *host, int timeout, int *extout);

/*! 
 * \brief Park a call via a masqueraded channel
 * \param rchan the real channel to be parked
 * \param host the channel to have the parking read to.
 * \param timeout is a timeout in milliseconds
 * \param extout is a parameter to an int that will hold the parked location, or NULL if you want.
 * 
 * Masquerade the channel rchan into a new, empty channel which is then parked with tris_park_call
 * \retval 0 on success.
 * \retval -1 on failure.
*/
int tris_masq_park_call(struct tris_channel *rchan, struct tris_channel *host, int timeout, int *extout);

/*! 
 * \brief Determine system parking extension
 * \returns the call parking extension for drivers that provide special call parking help 
*/
const char *tris_parking_ext(void);

/*! \brief Determine system call pickup extension */
const char *tris_pickup_ext(void);

/*! \brief Bridge a call, optionally allowing redirection */
int tris_bridge_call(struct tris_channel *chan, struct tris_channel *peer,struct tris_bridge_config *config);

/*! \brief Pickup a call */
int tris_pickup_call(struct tris_channel *chan);

/*! \brief register new feature into feature_set 
   \param feature an tris_call_feature object which contains a keysequence
   and a callback function which is called when this keysequence is pressed
   during a call. */
void tris_register_feature(struct tris_call_feature *feature);

/*! \brief unregister feature from feature_set
    \param feature the tris_call_feature object which was registered before*/
void tris_unregister_feature(struct tris_call_feature *feature);

/*! \brief look for a call feature entry by its sname
	\param name a string ptr, should match "automon", "blindxfer", "atxfer", etc. */
struct tris_call_feature *tris_find_call_feature(const char *name);

void tris_rdlock_call_features(void);
void tris_unlock_call_features(void);

/*! \brief Reload call features from features.conf */
int tris_features_reload(void);

void set_peers(struct tris_channel **caller, struct tris_channel **callee,
	struct tris_channel *peer, struct tris_channel *chan, int sense);

typedef void (*tris_sql_select_query_execute_f)(char *result,char *sql);
extern tris_sql_select_query_execute_f tris_sql_select_query_execute;
int send_control_notify(struct tris_channel* caller, enum tris_control_frame_type ctype, int referid, int notifycaller);

#endif /* _TRIS_FEATURES_H */
