/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2007, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
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
 * \brief Dialing API
 */

#ifndef _TRISMEDIA_DIAL_H
#define _TRISMEDIA_DIAL_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! \brief Main dialing structure. Contains global options, channels being dialed, and more! */
struct tris_dial;

/*! \brief Dialing channel structure. Contains per-channel dialing options, trismedia channel, and more! */
struct tris_dial_channel;

typedef void (*tris_dial_state_callback)(struct tris_dial *);

/*! \brief List of options that are applicable either globally or per dialed channel */
enum tris_dial_option {
	TRIS_DIAL_OPTION_RINGING,                 /*!< Always indicate ringing to caller */
	TRIS_DIAL_OPTION_ANSWER_EXEC,             /*!< Execute application upon answer in async mode */
	TRIS_DIAL_OPTION_MUSIC,                   /*!< Play music on hold instead of ringing to the calling channel */
	TRIS_DIAL_OPTION_DISABLE_CALL_FORWARDING, /*!< Disable call forwarding on channels */
	TRIS_DIAL_OPTION_MAX,                     /*!< End terminator -- must always remain last */
};

/*! \brief List of return codes for dial run API calls */
enum tris_dial_result {
	TRIS_DIAL_RESULT_INVALID,     /*!< Invalid options were passed to run function */
	TRIS_DIAL_RESULT_FAILED,      /*!< Attempts to dial failed before reaching critical state */
	TRIS_DIAL_RESULT_TRYING,      /*!< Currently trying to dial */
	TRIS_DIAL_RESULT_RINGING,     /*!< Dial is presently ringing */
	TRIS_DIAL_RESULT_PROGRESS,    /*!< Dial is presently progressing */
	TRIS_DIAL_RESULT_PROCEEDING,  /*!< Dial is presently proceeding */
	TRIS_DIAL_RESULT_ANSWERED,    /*!< A channel was answered */
	TRIS_DIAL_RESULT_TIMEOUT,     /*!< Timeout was tripped, nobody answered */
	TRIS_DIAL_RESULT_HANGUP,      /*!< Caller hung up */
	TRIS_DIAL_RESULT_UNANSWERED,  /*!< Nobody answered */
	TRIS_DIAL_RESULT_BUSY,			/* busy */
	TRIS_DIAL_RESULT_CONGESTION,		/* busy */
	TRIS_DIAL_RESULT_FORBIDDEN, /* can not call */
	TRIS_DIAL_RESULT_OFFHOOK,		/* is not found */
	TRIS_DIAL_RESULT_TAKEOFFHOOK,	/* not registered */
};

/*! \brief New dialing structure
 * \note Create a dialing structure
 * \return Returns a calloc'd tris_dial structure, NULL on failure
 */
struct tris_dial *tris_dial_create(void);

/*! \brief Append a channel
 * \note Appends a channel to a dialing structure
 * \return Returns channel reference number on success, -1 on failure
 */
int tris_dial_append(struct tris_dial *dial, const char *tech, const char *device);
int tris_dial_unset_chan(struct tris_dial *dial);
int tris_dial_check(struct tris_dial *dial, int referid);
int tris_dial_send_notify(struct tris_dial *dial, const char *phonenum, enum tris_control_frame_type type);

/*! \brief Execute dialing synchronously or asynchronously
 * \note Dials channels in a dial structure.
 * \return Returns dial result code. (TRYING/INVALID/FAILED/ANSWERED/TIMEOUT/UNANSWERED).
 */
enum tris_dial_result tris_dial_run(struct tris_dial *dial, struct tris_channel *chan, int async, int referid);

/*! \brief Return channel that answered
 * \note Returns the Trismedia channel that answered
 * \param dial Dialing structure
 */
struct tris_channel *tris_dial_answered(struct tris_dial *dial);

/*! \brief Steal the channel that answered
 * \note Returns the Trismedia channel that answered and removes it from the dialing structure
 * \param dial Dialing structure
 */
struct tris_channel *tris_dial_answered_steal(struct tris_dial *dial);

/*! \brief Return state of dial
 * \note Returns the state of the dial attempt
 * \param dial Dialing structure
 */
enum tris_dial_result tris_dial_state(struct tris_dial *dial);

/*! \brief Cancel async thread
 * \note Cancel a running async thread
 * \param dial Dialing structure
 */
enum tris_dial_result tris_dial_join(struct tris_dial *dial);

/*! \brief Hangup channels
 * \note Hangup all active channels
 * \param dial Dialing structure
 */
void tris_dial_hangup(struct tris_dial *dial);

/*! \brief Destroys a dialing structure
 * \note Cancels dialing and destroys (free's) the given tris_dial structure
 * \param dial Dialing structure to free
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_destroy(struct tris_dial *dial);

/*! \brief Enables an option globally
 * \param dial Dial structure to enable option on
 * \param option Option to enable
 * \param data Data to pass to this option (not always needed)
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_global_enable(struct tris_dial *dial, enum tris_dial_option option, void *data);

/*! \brief Enables an option per channel
 * \param dial Dial structure
 * \param num Channel number to enable option on
 * \param option Option to enable
 * \param data Data to pass to this option (not always needed)
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_enable(struct tris_dial *dial, int num, enum tris_dial_option option, void *data);

/*! \brief Disables an option globally
 * \param dial Dial structure to disable option on
 * \param option Option to disable
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_global_disable(struct tris_dial *dial, enum tris_dial_option option);

/*! \brief Disables an option per channel
 * \param dial Dial structure
 * \param num Channel number to disable option on
 * \param option Option to disable
 * \return Returns 0 on success, -1 on failure
 */
int tris_dial_option_disable(struct tris_dial *dial, int num, enum tris_dial_option option);

/*! \brief Set a callback for state changes
 * \param dial The dial structure to watch for state changes
 * \param callback the callback
 * \return nothing
 */
void tris_dial_set_state_callback(struct tris_dial *dial, tris_dial_state_callback callback);

/*! \brief Set the maximum time (globally) allowed for trying to ring phones
 * \param dial The dial structure to apply the time limit to
 * \param timeout Maximum time allowed in milliseconds
 * \return nothing
 */
void tris_dial_set_global_timeout(struct tris_dial *dial, int timeout);

/*! \brief Set the maximum time (per channel) allowed for trying to ring the phone
 * \param dial The dial structure the channel belongs to
 * \param num Channel number to set timeout on
 * \param timeout Maximum time allowed in milliseconds
 * \return nothing
 */
void tris_dial_set_timeout(struct tris_dial *dial, int num, int timeout);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_DIAL_H */
