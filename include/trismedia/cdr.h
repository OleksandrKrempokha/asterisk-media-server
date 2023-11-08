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
 * \brief Call Detail Record API 
 */

#ifndef _TRISMEDIA_CDR_H
#define _TRISMEDIA_CDR_H

#include <sys/time.h>

/*! \name CDR Flags */
/*@{ */
#define TRIS_CDR_FLAG_KEEP_VARS		(1 << 0)
#define TRIS_CDR_FLAG_POSTED			(1 << 1)
#define TRIS_CDR_FLAG_LOCKED			(1 << 2)
#define TRIS_CDR_FLAG_CHILD			(1 << 3)
#define TRIS_CDR_FLAG_POST_DISABLED	(1 << 4)
#define TRIS_CDR_FLAG_BRIDGED		(1 << 5)
#define TRIS_CDR_FLAG_MAIN			(1 << 6)
#define TRIS_CDR_FLAG_ENABLE			(1 << 7)
#define TRIS_CDR_FLAG_ANSLOCKED      (1 << 8)
#define TRIS_CDR_FLAG_DONT_TOUCH     (1 << 9)
#define TRIS_CDR_FLAG_POST_ENABLE    (1 << 10)
#define TRIS_CDR_FLAG_DIALED         (1 << 11)
#define TRIS_CDR_FLAG_ORIGINATED		(1 << 12)
/*@} */

/*! \name CDR Flags - Disposition */
/*@{ */
#define TRIS_CDR_NOANSWER			0
#define TRIS_CDR_NULL                (1 << 0)
#define TRIS_CDR_FAILED				(1 << 1)
#define TRIS_CDR_BUSY				(1 << 2)
#define TRIS_CDR_ANSWERED			(1 << 3)
/*@} */

/*! \name CDR AMA Flags */
/*@{ */
#define TRIS_CDR_OMIT				(1)
#define TRIS_CDR_BILLING				(2)
#define TRIS_CDR_DOCUMENTATION		(3)
/*@} */

#define TRIS_MAX_USER_FIELD			256
#define TRIS_MAX_ACCOUNT_CODE		20

/* Include channel.h after relevant declarations it will need */
#include "trismedia/channel.h"
#include "trismedia/utils.h"

/*! \brief Responsible for call detail data */
struct tris_cdr {
	/*! Caller*ID with text */
	char clid[TRIS_MAX_EXTENSION];
	/*! Caller*ID number */
	char src[TRIS_MAX_EXTENSION];		
	/*! Destination extension */
	char dst[TRIS_MAX_EXTENSION];		
	/*! Destination context */
	char dcontext[TRIS_MAX_EXTENSION];	
	
	char channel[TRIS_MAX_EXTENSION];
	/*! Destination channel if appropriate */
	char dstchannel[TRIS_MAX_EXTENSION];	
	/*! Last application if appropriate */
	char lastapp[TRIS_MAX_EXTENSION];	
	/*! Last application data */
	char lastdata[TRIS_MAX_EXTENSION];	
	
	struct timeval start;
	
	struct timeval answer;
	
	struct timeval end;
	/*! Total time in system, in seconds */
	long int duration;				
	/*! Total time call is up, in seconds */
	long int billsec;				
	/*! What happened to the call */
	long int disposition;			
	/*! What flags to use */
	long int amaflags;				
	/*! What account number to use */
	char accountcode[TRIS_MAX_ACCOUNT_CODE];			
	/*! flags */
	unsigned int flags;				
	/*! Unique Channel Identifier
	 * 150 = 127 (max systemname) + "-" + 10 (epoch timestamp) + "." + 10 (monotonically incrementing integer) + NULL */
	char uniqueid[150];
	/*! User field */
	char userfield[TRIS_MAX_USER_FIELD];

	/*! A linked list for variables */
	struct varshead varshead;

	struct tris_cdr *next;
};

int tris_cdr_isset_unanswered(void);
void tris_cdr_getvar(struct tris_cdr *cdr, const char *name, char **ret, char *workspace, int workspacelen, int recur, int raw);
int tris_cdr_setvar(struct tris_cdr *cdr, const char *name, const char *value, int recur);
int tris_cdr_serialize_variables(struct tris_cdr *cdr, struct tris_str **buf, char delim, char sep, int recur);
void tris_cdr_free_vars(struct tris_cdr *cdr, int recur);
int tris_cdr_copy_vars(struct tris_cdr *to_cdr, struct tris_cdr *from_cdr);

/*!
 * \brief CDR backend callback
 * \warning CDR backends should NOT attempt to access the channel associated
 * with a CDR record.  This channel is not guaranteed to exist when the CDR
 * backend is invoked.
 */
typedef int (*tris_cdrbe)(struct tris_cdr *cdr);

/*! \brief Return TRUE if CDR subsystem is enabled */
int check_cdr_enabled(void);

/*! 
 * \brief Allocate a CDR record 
 * \retval a malloc'd tris_cdr structure
 * \retval NULL on error (malloc failure)
 */
struct tris_cdr *tris_cdr_alloc(void);

/*! 
 * \brief Duplicate a record 
 * \retval a malloc'd tris_cdr structure, 
 * \retval NULL on error (malloc failure)
 */
struct tris_cdr *tris_cdr_dup(struct tris_cdr *cdr);

/*! 
 * \brief Free a CDR record 
 * \param cdr tris_cdr structure to free
 * Returns nothing
 */
void tris_cdr_free(struct tris_cdr *cdr);

/*! 
 * \brief Discard and free a CDR record 
 * \param cdr tris_cdr structure to free
 * Returns nothing  -- same as free, but no checks or complaints
 */
void tris_cdr_discard(struct tris_cdr *cdr);

/*! 
 * \brief Initialize based on a channel
 * \param cdr Call Detail Record to use for channel
 * \param chan Channel to bind CDR with
 * Initializes a CDR and associates it with a particular channel
 * \return 0 by default
 */
int tris_cdr_init(struct tris_cdr *cdr, struct tris_channel *chan);

/*! 
 * \brief Initialize based on a channel 
 * \param cdr Call Detail Record to use for channel
 * \param chan Channel to bind CDR with
 * Initializes a CDR and associates it with a particular channel
 * \return 0 by default
 */
int tris_cdr_setcid(struct tris_cdr *cdr, struct tris_channel *chan);

/*! 
 * \brief Register a CDR handling engine 
 * \param name name associated with the particular CDR handler
 * \param desc description of the CDR handler
 * \param be function pointer to a CDR handler
 * Used to register a Call Detail Record handler.
 * \retval 0 on success.
 * \retval -1 on error
 */
int tris_cdr_register(const char *name, const char *desc, tris_cdrbe be);

/*! 
 * \brief Unregister a CDR handling engine 
 * \param name name of CDR handler to unregister
 * Unregisters a CDR by it's name
 */
void tris_cdr_unregister(const char *name);

/*! 
 * \brief Start a call 
 * \param cdr the cdr you wish to associate with the call
 * Starts all CDR stuff necessary for monitoring a call
 * Returns nothing
 */
void tris_cdr_start(struct tris_cdr *cdr);

/*! \brief Answer a call 
 * \param cdr the cdr you wish to associate with the call
 * Starts all CDR stuff necessary for doing CDR when answering a call
 * \note NULL argument is just fine.
 */
void tris_cdr_answer(struct tris_cdr *cdr);

/*! 
 * \brief A call wasn't answered 
 * \param cdr the cdr you wish to associate with the call
 * Marks the channel disposition as "NO ANSWER"
 * Will skip CDR's in chain with ANS_LOCK bit set. (see
 * forkCDR() application.
 */
extern void tris_cdr_noanswer(struct tris_cdr *cdr);

/*! 
 * \brief Busy a call 
 * \param cdr the cdr you wish to associate with the call
 * Marks the channel disposition as "BUSY"
 * Will skip CDR's in chain with ANS_LOCK bit set. (see
 * forkCDR() application.
 * Returns nothing
 */
void tris_cdr_busy(struct tris_cdr *cdr);

/*! 
 * \brief Fail a call 
 * \param cdr the cdr you wish to associate with the call
 * Marks the channel disposition as "FAILED"
 * Will skip CDR's in chain with ANS_LOCK bit set. (see
 * forkCDR() application.
 * Returns nothing
 */
void tris_cdr_failed(struct tris_cdr *cdr);

/*! 
 * \brief Save the result of the call based on the TRIS_CAUSE_*
 * \param cdr the cdr you wish to associate with the call
 * \param cause the TRIS_CAUSE_*
 * Returns nothing
 */
int tris_cdr_disposition(struct tris_cdr *cdr, int cause);
	
/*! 
 * \brief End a call
 * \param cdr the cdr you have associated the call with
 * Registers the end of call time in the cdr structure.
 * Returns nothing
 */
void tris_cdr_end(struct tris_cdr *cdr);

/*! 
 * \brief Detaches the detail record for posting (and freeing) either now or at a
 * later time in bulk with other records during batch mode operation.
 * \param cdr Which CDR to detach from the channel thread
 * Prevents the channel thread from blocking on the CDR handling
 * Returns nothing
 */
void tris_cdr_detach(struct tris_cdr *cdr);

/*! 
 * \brief Spawns (possibly) a new thread to submit a batch of CDRs to the backend engines 
 * \param shutdown Whether or not we are shutting down
 * Blocks the trismedia shutdown procedures until the CDR data is submitted.
 * Returns nothing
 */
void tris_cdr_submit_batch(int shutdown);

/*! 
 * \brief Set the destination channel, if there was one 
 * \param cdr Which cdr it's applied to
 * \param chan Channel to which dest will be
 * Sets the destination channel the CDR is applied to
 * Returns nothing
 */
void tris_cdr_setdestchan(struct tris_cdr *cdr, const char *chan);

/*! 
 * \brief Set the last executed application 
 * \param cdr which cdr to act upon
 * \param app the name of the app you wish to change it to
 * \param data the data you want in the data field of app you set it to
 * Changes the value of the last executed app
 * Returns nothing
 */
void tris_cdr_setapp(struct tris_cdr *cdr, const char *app, const char *data);

/*!
 * \brief Set the answer time for a call
 * \param cdr the cdr you wish to associate with the call
 * \param t the answer time
 * Starts all CDR stuff necessary for doing CDR when answering a call
 * NULL argument is just fine.
 */
void tris_cdr_setanswer(struct tris_cdr *cdr, struct timeval t);

/*!
 * \brief Set the disposition for a call
 * \param cdr the cdr you wish to associate with the call
 * \param disposition the new disposition
 * Set the disposition on a call.
 * NULL argument is just fine.
 */
void tris_cdr_setdisposition(struct tris_cdr *cdr, long int disposition);

/*! 
 * \brief Convert a string to a detail record AMA flag 
 * \param flag string form of flag
 * Converts the string form of the flag to the binary form.
 * \return the binary form of the flag
 */
int tris_cdr_amaflags2int(const char *flag);

/*! 
 * \brief Disposition to a string 
 * \param disposition input binary form
 * Converts the binary form of a disposition to string form.
 * \return a pointer to the string form
 */
char *tris_cdr_disp2str(int disposition);

/*! 
 * \brief Reset the detail record, optionally posting it first 
 * \param cdr which cdr to act upon
 * \param flags |TRIS_CDR_FLAG_POSTED whether or not to post the cdr first before resetting it
 *              |TRIS_CDR_FLAG_LOCKED whether or not to reset locked CDR's
 */
void tris_cdr_reset(struct tris_cdr *cdr, struct tris_flags *flags);

/*! Reset the detail record times, flags */
/*!
 * \param cdr which cdr to act upon
 * \param flags |TRIS_CDR_FLAG_POSTED whether or not to post the cdr first before resetting it
 *              |TRIS_CDR_FLAG_LOCKED whether or not to reset locked CDR's
 */
void tris_cdr_specialized_reset(struct tris_cdr *cdr, struct tris_flags *flags);

/*! Flags to a string */
/*!
 * \param flags binary flag
 * Converts binary flags to string flags
 * Returns string with flag name
 */
char *tris_cdr_flags2str(int flags);

/*! 
 * \brief Move the non-null data from the "from" cdr to the "to" cdr
 * \param to the cdr to get the goodies
 * \param from the cdr to give the goodies
 */
void tris_cdr_merge(struct tris_cdr *to, struct tris_cdr *from);

/*! \brief Set account code, will generate AMI event */
int tris_cdr_setaccount(struct tris_channel *chan, const char *account);

/*! \brief Set AMA flags for channel */
int tris_cdr_setamaflags(struct tris_channel *chan, const char *amaflags);

/*! \brief Set CDR user field for channel (stored in CDR) */
int tris_cdr_setuserfield(struct tris_channel *chan, const char *userfield);
/*! \brief Append to CDR user field for channel (stored in CDR) */
int tris_cdr_appenduserfield(struct tris_channel *chan, const char *userfield);


/*! Update CDR on a channel */
int tris_cdr_update(struct tris_channel *chan);


extern int tris_default_amaflags;

extern char tris_default_accountcode[TRIS_MAX_ACCOUNT_CODE];

struct tris_cdr *tris_cdr_append(struct tris_cdr *cdr, struct tris_cdr *newcdr);

/*! \brief Reload the configuration file cdr.conf and start/stop CDR scheduling thread */
int tris_cdr_engine_reload(void);

/*! \brief Load the configuration file cdr.conf and possibly start the CDR scheduling thread */
int tris_cdr_engine_init(void);

/*! Submit any remaining CDRs and prepare for shutdown */
void tris_cdr_engine_term(void);

#endif /* _TRISMEDIA_CDR_H */
