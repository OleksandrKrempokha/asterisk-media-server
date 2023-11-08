/*
 * Trismedia -- A telephony toolkit for Linux.
 *
 * Copyright (C) 2005-2008, Digium, Inc.
 *
 * Matthew A. Nicholson <mnicholson@digium.com>
 * Russell Bryant <russell@digium.com>
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

/*!
 * \file
 * \brief SMDI support for Trismedia.
 * \author Matthew A. Nicholson <mnicholson@digium.com>
 * \author Russell Bryant <russell@digium.com>
 *
 * Here is a useful mailing list post that describes SMDI protocol details:
 * \ref http://lists.digium.com/pipermail/trismedia-dev/2003-June/000884.html
 *
 * \todo This module currently has its own mailbox monitoring thread.  This should
 * be converted to MWI subscriptions and just let the optional global voicemail
 * polling thread handle it.
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include <termios.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>

#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"
#define TRIS_API_MODULE
#include "trismedia/smdi.h"
#include "trismedia/config.h"
#include "trismedia/astobj.h"
#include "trismedia/io.h"
#include "trismedia/stringfields.h"
#include "trismedia/linkedlists.h"
#include "trismedia/app.h"
#include "trismedia/pbx.h"
#include "trismedia/channel.h"

/* Message expiry time in milliseconds */
#define SMDI_MSG_EXPIRY_TIME	30000 /* 30 seconds */

static const char config_file[] = "smdi.conf";

/*! \brief SMDI message desk message queue. */
struct tris_smdi_md_queue {
	ASTOBJ_CONTAINER_COMPONENTS(struct tris_smdi_md_message);
};

/*! \brief SMDI message waiting indicator message queue. */
struct tris_smdi_mwi_queue {
	ASTOBJ_CONTAINER_COMPONENTS(struct tris_smdi_mwi_message);
};

struct tris_smdi_interface {
	ASTOBJ_COMPONENTS_FULL(struct tris_smdi_interface, SMDI_MAX_FILENAME_LEN, 1);
	struct tris_smdi_md_queue md_q;
	tris_mutex_t md_q_lock;
	tris_cond_t md_q_cond;
	struct tris_smdi_mwi_queue mwi_q;
	tris_mutex_t mwi_q_lock;
	tris_cond_t mwi_q_cond;
	FILE *file;
	int fd;
	pthread_t thread;
	struct termios mode;
	int msdstrip;
	long msg_expiry;
};

/*! \brief SMDI interface container. */
struct tris_smdi_interface_container {
	ASTOBJ_CONTAINER_COMPONENTS(struct tris_smdi_interface);
} smdi_ifaces;

/*! \brief A mapping between an SMDI mailbox ID and an Trismedia mailbox */
struct mailbox_mapping {
	/*! This is the current state of the mailbox.  It is simply on or
	 *  off to indicate if there are messages waiting or not. */
	unsigned int cur_state:1;
	/*! A Pointer to the appropriate SMDI interface */
	struct tris_smdi_interface *iface;
	TRIS_DECLARE_STRING_FIELDS(
		/*! The Name of the mailbox for the SMDI link. */
		TRIS_STRING_FIELD(smdi);
		/*! The name of the mailbox on the Trismedia side */
		TRIS_STRING_FIELD(mailbox);
		/*! The name of the voicemail context in use */
		TRIS_STRING_FIELD(context);
	);
	TRIS_LIST_ENTRY(mailbox_mapping) entry;
};

/*! 10 seconds */
#define DEFAULT_POLLING_INTERVAL 10

/*! \brief Data that gets used by the SMDI MWI monitoring thread */
static struct {
	/*! The thread ID */
	pthread_t thread;
	tris_mutex_t lock;
	tris_cond_t cond;
	/*! A list of mailboxes that need to be monitored */
	TRIS_LIST_HEAD_NOLOCK(, mailbox_mapping) mailbox_mappings;
	/*! Polling Interval for checking mailbox status */
	unsigned int polling_interval;
	/*! Set to 1 to tell the polling thread to stop */
	unsigned int stop:1;
	/*! The time that the last poll began */
	struct timeval last_poll;
} mwi_monitor = {
	.thread = TRIS_PTHREADT_NULL,
};

static void tris_smdi_interface_destroy(struct tris_smdi_interface *iface)
{
	if (iface->thread != TRIS_PTHREADT_NULL && iface->thread != TRIS_PTHREADT_STOP) {
		pthread_cancel(iface->thread);
		pthread_join(iface->thread, NULL);
	}
	
	iface->thread = TRIS_PTHREADT_STOP;
	
	if (iface->file) 
		fclose(iface->file);
	
	ASTOBJ_CONTAINER_DESTROYALL(&iface->md_q, tris_smdi_md_message_destroy);
	ASTOBJ_CONTAINER_DESTROYALL(&iface->mwi_q, tris_smdi_mwi_message_destroy);
	ASTOBJ_CONTAINER_DESTROY(&iface->md_q);
	ASTOBJ_CONTAINER_DESTROY(&iface->mwi_q);

	tris_mutex_destroy(&iface->md_q_lock);
	tris_cond_destroy(&iface->md_q_cond);

	tris_mutex_destroy(&iface->mwi_q_lock);
	tris_cond_destroy(&iface->mwi_q_cond);

	free(iface);

	tris_module_unref(tris_module_info->self);
}

void tris_smdi_interface_unref(struct tris_smdi_interface *iface)
{
	ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
}

/*! 
 * \internal
 * \brief Push an SMDI message to the back of an interface's message queue.
 * \param iface a pointer to the interface to use.
 * \param md_msg a pointer to the message to use.
 */
static void tris_smdi_md_message_push(struct tris_smdi_interface *iface, struct tris_smdi_md_message *md_msg)
{
	tris_mutex_lock(&iface->md_q_lock);
	ASTOBJ_CONTAINER_LINK_END(&iface->md_q, md_msg);
	tris_cond_broadcast(&iface->md_q_cond);
	tris_mutex_unlock(&iface->md_q_lock);
}

/*!
 * \internal
 * \brief Push an SMDI message to the back of an interface's message queue.
 * \param iface a pointer to the interface to use.
 * \param mwi_msg a pointer to the message to use.
 */
static void tris_smdi_mwi_message_push(struct tris_smdi_interface *iface, struct tris_smdi_mwi_message *mwi_msg)
{
	tris_mutex_lock(&iface->mwi_q_lock);
	ASTOBJ_CONTAINER_LINK_END(&iface->mwi_q, mwi_msg);
	tris_cond_broadcast(&iface->mwi_q_cond);
	tris_mutex_unlock(&iface->mwi_q_lock);
}

static int smdi_toggle_mwi(struct tris_smdi_interface *iface, const char *mailbox, int on)
{
	FILE *file;
	int i;
	
	if (!(file = fopen(iface->name, "w"))) {
		tris_log(LOG_ERROR, "Error opening SMDI interface %s (%s) for writing\n", iface->name, strerror(errno));
		return 1;
	}	
	
	ASTOBJ_WRLOCK(iface);
	
	fprintf(file, "%s:MWI ", on ? "OP" : "RMV");
	
	for (i = 0; i < iface->msdstrip; i++)
		fprintf(file, "0");

	fprintf(file, "%s!\x04", mailbox);

	fclose(file);

	ASTOBJ_UNLOCK(iface);
	tris_debug(1, "Sent MWI set message for %s on %s\n", mailbox, iface->name);

	return 0;
}

int tris_smdi_mwi_set(struct tris_smdi_interface *iface, const char *mailbox)
{
	return smdi_toggle_mwi(iface, mailbox, 1);
}

int tris_smdi_mwi_unset(struct tris_smdi_interface *iface, const char *mailbox)
{
	return smdi_toggle_mwi(iface, mailbox, 0);
}

void tris_smdi_md_message_putback(struct tris_smdi_interface *iface, struct tris_smdi_md_message *md_msg)
{
	tris_mutex_lock(&iface->md_q_lock);
	ASTOBJ_CONTAINER_LINK_START(&iface->md_q, md_msg);
	tris_cond_broadcast(&iface->md_q_cond);
	tris_mutex_unlock(&iface->md_q_lock);
}

void tris_smdi_mwi_message_putback(struct tris_smdi_interface *iface, struct tris_smdi_mwi_message *mwi_msg)
{
	tris_mutex_lock(&iface->mwi_q_lock);
	ASTOBJ_CONTAINER_LINK_START(&iface->mwi_q, mwi_msg);
	tris_cond_broadcast(&iface->mwi_q_cond);
	tris_mutex_unlock(&iface->mwi_q_lock);
}

enum smdi_message_type {
	SMDI_MWI,
	SMDI_MD,
};

static inline int lock_msg_q(struct tris_smdi_interface *iface, enum smdi_message_type type)
{
	switch (type) {
	case SMDI_MWI:
		return tris_mutex_lock(&iface->mwi_q_lock);
	case SMDI_MD:	
		return tris_mutex_lock(&iface->md_q_lock);
	}
	
	return -1;
}

static inline int unlock_msg_q(struct tris_smdi_interface *iface, enum smdi_message_type type)
{
	switch (type) {
	case SMDI_MWI:
		return tris_mutex_unlock(&iface->mwi_q_lock);
	case SMDI_MD:
		return tris_mutex_unlock(&iface->md_q_lock);
	}

	return -1;
}

static inline void *unlink_from_msg_q(struct tris_smdi_interface *iface, enum smdi_message_type type)
{
	switch (type) {
	case SMDI_MWI:
		return ASTOBJ_CONTAINER_UNLINK_START(&iface->mwi_q);
	case SMDI_MD:
		return ASTOBJ_CONTAINER_UNLINK_START(&iface->md_q);
	}
	return NULL;
}

static inline struct timeval msg_timestamp(void *msg, enum smdi_message_type type)
{
	struct tris_smdi_md_message *md_msg = msg;
	struct tris_smdi_mwi_message *mwi_msg = msg;

	switch (type) {
	case SMDI_MWI:
		return mwi_msg->timestamp;
	case SMDI_MD:
		return md_msg->timestamp;
	}

	return tris_tv(0, 0);
}

static inline void unref_msg(void *msg, enum smdi_message_type type)
{
	struct tris_smdi_md_message *md_msg = msg;
	struct tris_smdi_mwi_message *mwi_msg = msg;

	switch (type) {
	case SMDI_MWI:
		ASTOBJ_UNREF(mwi_msg, tris_smdi_mwi_message_destroy);
		break;
	case SMDI_MD:
		ASTOBJ_UNREF(md_msg, tris_smdi_md_message_destroy);
		break;
	}
}

static void purge_old_messages(struct tris_smdi_interface *iface, enum smdi_message_type type)
{
	struct timeval now = tris_tvnow();
	long elapsed = 0;
	void *msg;
	
	lock_msg_q(iface, type);
	msg = unlink_from_msg_q(iface, type);
	unlock_msg_q(iface, type);

	/* purge old messages */
	while (msg) {
		elapsed = tris_tvdiff_ms(now, msg_timestamp(msg, type));

		if (elapsed > iface->msg_expiry) {
			/* found an expired message */
			unref_msg(msg, type);
			tris_log(LOG_NOTICE, "Purged expired message from %s SMDI %s message queue.  "
				"Message was %ld milliseconds too old.\n",
				iface->name, (type == SMDI_MD) ? "MD" : "MWI", 
				elapsed - iface->msg_expiry);

			lock_msg_q(iface, type);
			msg = unlink_from_msg_q(iface, type);
			unlock_msg_q(iface, type);
		} else {
			/* good message, put it back and return */
			switch (type) {
			case SMDI_MD:
				tris_smdi_md_message_push(iface, msg);
				break;
			case SMDI_MWI:
				tris_smdi_mwi_message_push(iface, msg);
				break;
			}
			unref_msg(msg, type);
			break;
		}
	}
}

static void *smdi_msg_pop(struct tris_smdi_interface *iface, enum smdi_message_type type)
{
	void *msg;

	purge_old_messages(iface, type);

	lock_msg_q(iface, type);
	msg = unlink_from_msg_q(iface, type);
	unlock_msg_q(iface, type);

	return msg;
}

enum {
	OPT_SEARCH_TERMINAL = (1 << 0),
	OPT_SEARCH_NUMBER   = (1 << 1),
};

static void *smdi_msg_find(struct tris_smdi_interface *iface,
	enum smdi_message_type type, const char *search_key, struct tris_flags options)
{
	void *msg = NULL;

	purge_old_messages(iface, type);

	switch (type) {
	case SMDI_MD:
		if (tris_strlen_zero(search_key)) {
			struct tris_smdi_md_message *md_msg = NULL;

			/* No search key provided (the code from chan_dahdi does this).
			 * Just pop the top message off of the queue. */

			ASTOBJ_CONTAINER_TRAVERSE(&iface->md_q, !md_msg, do {
				md_msg = ASTOBJ_REF(iterator);
			} while (0); );

			msg = md_msg;
		} else if (tris_test_flag(&options, OPT_SEARCH_TERMINAL)) {
			struct tris_smdi_md_message *md_msg = NULL;

			/* Searching by the message desk terminal */

			ASTOBJ_CONTAINER_TRAVERSE(&iface->md_q, !md_msg, do {
				if (!strcasecmp(iterator->mesg_desk_term, search_key))
					md_msg = ASTOBJ_REF(iterator);
			} while (0); );

			msg = md_msg;
		} else if (tris_test_flag(&options, OPT_SEARCH_NUMBER)) {
			struct tris_smdi_md_message *md_msg = NULL;

			/* Searching by the message desk number */

			ASTOBJ_CONTAINER_TRAVERSE(&iface->md_q, !md_msg, do {
				if (!strcasecmp(iterator->mesg_desk_num, search_key))
					md_msg = ASTOBJ_REF(iterator);
			} while (0); );

			msg = md_msg;
		} else {
			/* Searching by the forwarding station */
			msg = ASTOBJ_CONTAINER_FIND(&iface->md_q, search_key);
		}
		break;
	case SMDI_MWI:
		if (tris_strlen_zero(search_key)) {
			struct tris_smdi_mwi_message *mwi_msg = NULL;

			/* No search key provided (the code from chan_dahdi does this).
			 * Just pop the top message off of the queue. */

			ASTOBJ_CONTAINER_TRAVERSE(&iface->mwi_q, !mwi_msg, do {
				mwi_msg = ASTOBJ_REF(iterator);
			} while (0); );

			msg = mwi_msg;
		} else {
			msg = ASTOBJ_CONTAINER_FIND(&iface->mwi_q, search_key);
		}
		break;
	}

	return msg;
}

static void *smdi_message_wait(struct tris_smdi_interface *iface, int timeout, 
	enum smdi_message_type type, const char *search_key, struct tris_flags options)
{
	struct timeval start;
	long diff = 0;
	void *msg;
	tris_cond_t *cond = NULL;
	tris_mutex_t *lock = NULL;

	switch (type) {
	case SMDI_MWI:
		cond = &iface->mwi_q_cond;
		lock = &iface->mwi_q_lock;
		break;
	case SMDI_MD:
		cond = &iface->md_q_cond;
		lock = &iface->md_q_lock;
		break;
	}

	start = tris_tvnow();

	while (diff < timeout) {
		struct timespec ts = { 0, };
		struct timeval wait;

		lock_msg_q(iface, type);

		if ((msg = smdi_msg_find(iface, type, search_key, options))) {
			unlock_msg_q(iface, type);
			return msg;
		}

		wait = tris_tvadd(start, tris_tv(0, timeout));
		ts.tv_sec = wait.tv_sec;
		ts.tv_nsec = wait.tv_usec * 1000;

		/* If there were no messages in the queue, then go to sleep until one
		 * arrives. */

		tris_cond_timedwait(cond, lock, &ts);

		if ((msg = smdi_msg_find(iface, type, search_key, options))) {
			unlock_msg_q(iface, type);
			return msg;
		}

		unlock_msg_q(iface, type);

		/* check timeout */
		diff = tris_tvdiff_ms(tris_tvnow(), start);
	}

	return NULL;
}

struct tris_smdi_md_message *tris_smdi_md_message_pop(struct tris_smdi_interface *iface)
{
	return smdi_msg_pop(iface, SMDI_MD);
}

struct tris_smdi_md_message *tris_smdi_md_message_wait(struct tris_smdi_interface *iface, int timeout)
{
	struct tris_flags options = { 0 };
	return smdi_message_wait(iface, timeout, SMDI_MD, NULL, options);
}

struct tris_smdi_mwi_message *tris_smdi_mwi_message_pop(struct tris_smdi_interface *iface)
{
	return smdi_msg_pop(iface, SMDI_MWI);
}

struct tris_smdi_mwi_message *tris_smdi_mwi_message_wait(struct tris_smdi_interface *iface, int timeout)
{
	struct tris_flags options = { 0 };
	return smdi_message_wait(iface, timeout, SMDI_MWI, NULL, options);
}

struct tris_smdi_mwi_message *tris_smdi_mwi_message_wait_station(struct tris_smdi_interface *iface, int timeout,
	const char *station)
{
	struct tris_flags options = { 0 };
	return smdi_message_wait(iface, timeout, SMDI_MWI, station, options);
}

struct tris_smdi_interface *tris_smdi_interface_find(const char *iface_name)
{
	return (ASTOBJ_CONTAINER_FIND(&smdi_ifaces, iface_name));
}

/*! 
 * \internal
 * \brief Read an SMDI message.
 *
 * \param iface_p the SMDI interface to read from.
 *
 * This function loops and reads from and SMDI interface.  It must be stopped
 * using pthread_cancel().
 */
static void *smdi_read(void *iface_p)
{
	struct tris_smdi_interface *iface = iface_p;
	struct tris_smdi_md_message *md_msg;
	struct tris_smdi_mwi_message *mwi_msg;
	char c = '\0';
	char *cp = NULL;
	int i;
	int start = 0;
		
	/* read an smdi message */
	while ((c = fgetc(iface->file))) {

		/* check if this is the start of a message */
		if (!start) {
			if (c == 'M') {
				tris_log(LOG_DEBUG, "Read an 'M' to start an SMDI message\n");
				start = 1;
			}
			continue;
		}
		
		if (c == 'D') { /* MD message */
			start = 0;

			tris_log(LOG_DEBUG, "Read a 'D' ... it's an MD message.\n");

			if (!(md_msg = tris_calloc(1, sizeof(*md_msg)))) {
				ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
				return NULL;
			}
			
			ASTOBJ_INIT(md_msg);

			/* read the message desk number */
			for (i = 0; i < sizeof(md_msg->mesg_desk_num) - 1; i++) {
				md_msg->mesg_desk_num[i] = fgetc(iface->file);
				tris_log(LOG_DEBUG, "Read a '%c'\n", md_msg->mesg_desk_num[i]);
			}

			md_msg->mesg_desk_num[sizeof(md_msg->mesg_desk_num) - 1] = '\0';
			
			tris_log(LOG_DEBUG, "The message desk number is '%s'\n", md_msg->mesg_desk_num);

			/* read the message desk terminal number */
			for (i = 0; i < sizeof(md_msg->mesg_desk_term) - 1; i++) {
				md_msg->mesg_desk_term[i] = fgetc(iface->file);
				tris_log(LOG_DEBUG, "Read a '%c'\n", md_msg->mesg_desk_term[i]);
			}

			md_msg->mesg_desk_term[sizeof(md_msg->mesg_desk_term) - 1] = '\0';

			tris_log(LOG_DEBUG, "The message desk terminal is '%s'\n", md_msg->mesg_desk_term);

			/* read the message type */
			md_msg->type = fgetc(iface->file);
		 
			tris_log(LOG_DEBUG, "Message type is '%c'\n", md_msg->type);

			/* read the forwarding station number (may be blank) */
			cp = &md_msg->fwd_st[0];
			for (i = 0; i < sizeof(md_msg->fwd_st) - 1; i++) {
				if ((c = fgetc(iface->file)) == ' ') {
					*cp = '\0';
					tris_log(LOG_DEBUG, "Read a space, done looking for the forwarding station\n");
					break;
				}

				/* store c in md_msg->fwd_st */
				if (i >= iface->msdstrip) {
					tris_log(LOG_DEBUG, "Read a '%c' and stored it in the forwarding station buffer\n", c);
					*cp++ = c;
				} else {
					tris_log(LOG_DEBUG, "Read a '%c', but didn't store it in the fwd station buffer, because of the msdstrip setting (%d < %d)\n", c, i, iface->msdstrip);
				}
			}

			/* make sure the value is null terminated, even if this truncates it */
			md_msg->fwd_st[sizeof(md_msg->fwd_st) - 1] = '\0';
			cp = NULL;

			tris_log(LOG_DEBUG, "The forwarding station is '%s'\n", md_msg->fwd_st);

			/* Put the fwd_st in the name field so that we can use ASTOBJ_FIND to look
			 * up a message on this field */
			tris_copy_string(md_msg->name, md_msg->fwd_st, sizeof(md_msg->name));

			/* read the calling station number (may be blank) */
			cp = &md_msg->calling_st[0];
			for (i = 0; i < sizeof(md_msg->calling_st) - 1; i++) {
				if (!isdigit((c = fgetc(iface->file)))) {
					*cp = '\0';
					tris_log(LOG_DEBUG, "Read a '%c', but didn't store it in the calling station buffer because it's not a digit\n", c);
					if (c == ' ') {
						/* Don't break on a space.  We may read the space before the calling station
						 * here if the forwarding station buffer filled up. */
						i--; /* We're still on the same character */
						continue;
					}
					break;
				}

				/* store c in md_msg->calling_st */
				if (i >= iface->msdstrip) {
					tris_log(LOG_DEBUG, "Read a '%c' and stored it in the calling station buffer\n", c);
					*cp++ = c;
				} else {
					tris_log(LOG_DEBUG, "Read a '%c', but didn't store it in the calling station buffer, because of the msdstrip setting (%d < %d)\n", c, i, iface->msdstrip);
				}
			}

			/* make sure the value is null terminated, even if this truncates it */
			md_msg->calling_st[sizeof(md_msg->calling_st) - 1] = '\0';
			cp = NULL;

			tris_log(LOG_DEBUG, "The calling station is '%s'\n", md_msg->calling_st);

			/* add the message to the message queue */
			md_msg->timestamp = tris_tvnow();
			tris_smdi_md_message_push(iface, md_msg);
			tris_log(LOG_DEBUG, "Received SMDI MD message on %s\n", iface->name);
			
			ASTOBJ_UNREF(md_msg, tris_smdi_md_message_destroy);

		} else if (c == 'W') { /* MWI message */
			start = 0;

			tris_log(LOG_DEBUG, "Read a 'W', it's an MWI message. (No more debug coming for MWI messages)\n");

			if (!(mwi_msg = tris_calloc(1, sizeof(*mwi_msg)))) {
				ASTOBJ_UNREF(iface,tris_smdi_interface_destroy);
				return NULL;
			}

			ASTOBJ_INIT(mwi_msg);

			/* discard the 'I' (from 'MWI') */
			fgetc(iface->file);
			
			/* read the forwarding station number (may be blank) */
			cp = &mwi_msg->fwd_st[0];
			for (i = 0; i < sizeof(mwi_msg->fwd_st) - 1; i++) {
				if ((c = fgetc(iface->file)) == ' ') {
					*cp = '\0';
					break;
				}

				/* store c in md_msg->fwd_st */
				if (i >= iface->msdstrip)
					*cp++ = c;
			}

			/* make sure the station number is null terminated, even if this will truncate it */
			mwi_msg->fwd_st[sizeof(mwi_msg->fwd_st) - 1] = '\0';
			cp = NULL;
			
			/* Put the fwd_st in the name field so that we can use ASTOBJ_FIND to look
			 * up a message on this field */
			tris_copy_string(mwi_msg->name, mwi_msg->fwd_st, sizeof(mwi_msg->name));

			/* read the mwi failure cause */
			for (i = 0; i < sizeof(mwi_msg->cause) - 1; i++)
				mwi_msg->cause[i] = fgetc(iface->file);

			mwi_msg->cause[sizeof(mwi_msg->cause) - 1] = '\0';

			/* add the message to the message queue */
			mwi_msg->timestamp = tris_tvnow();
			tris_smdi_mwi_message_push(iface, mwi_msg);
			tris_log(LOG_DEBUG, "Received SMDI MWI message on %s\n", iface->name);
			
			ASTOBJ_UNREF(mwi_msg, tris_smdi_mwi_message_destroy);
		} else {
			tris_log(LOG_ERROR, "Unknown SMDI message type received on %s (M%c).\n", iface->name, c);
			start = 0;
		}
	}

	tris_log(LOG_ERROR, "Error reading from SMDI interface %s, stopping listener thread\n", iface->name);
	ASTOBJ_UNREF(iface,tris_smdi_interface_destroy);
	return NULL;
}

void tris_smdi_md_message_destroy(struct tris_smdi_md_message *msg)
{
	tris_free(msg);
}

void tris_smdi_mwi_message_destroy(struct tris_smdi_mwi_message *msg)
{
	tris_free(msg);
}

static void destroy_mailbox_mapping(struct mailbox_mapping *mm)
{
	tris_string_field_free_memory(mm);
	ASTOBJ_UNREF(mm->iface, tris_smdi_interface_destroy);
	free(mm);
}

static void destroy_all_mailbox_mappings(void)
{
	struct mailbox_mapping *mm;

	tris_mutex_lock(&mwi_monitor.lock);
	while ((mm = TRIS_LIST_REMOVE_HEAD(&mwi_monitor.mailbox_mappings, entry)))
		destroy_mailbox_mapping(mm);
	tris_mutex_unlock(&mwi_monitor.lock);
}

static void append_mailbox_mapping(struct tris_variable *var, struct tris_smdi_interface *iface)
{
	struct mailbox_mapping *mm;
	char *mailbox, *context;

	if (!(mm = tris_calloc(1, sizeof(*mm))))
		return;
	
	if (tris_string_field_init(mm, 32)) {
		free(mm);
		return;
	}

	tris_string_field_set(mm, smdi, var->name);

	context = tris_strdupa(var->value);
	mailbox = strsep(&context, "@");
	if (tris_strlen_zero(context))
		context = "default";

	tris_string_field_set(mm, mailbox, mailbox);
	tris_string_field_set(mm, context, context);

	mm->iface = ASTOBJ_REF(iface);

	tris_mutex_lock(&mwi_monitor.lock);
	TRIS_LIST_INSERT_TAIL(&mwi_monitor.mailbox_mappings, mm, entry);
	tris_mutex_unlock(&mwi_monitor.lock);
}

/*!
 * \note Called with the mwi_monitor.lock locked
 */
static void poll_mailbox(struct mailbox_mapping *mm)
{
	char buf[1024];
	unsigned int state;

	snprintf(buf, sizeof(buf), "%s@%s", mm->mailbox, mm->context);

	state = !!tris_app_has_voicemail(mm->mailbox, NULL);

	if (state != mm->cur_state) {
		if (state)
			tris_smdi_mwi_set(mm->iface, mm->smdi);
		else
			tris_smdi_mwi_unset(mm->iface, mm->smdi);

		mm->cur_state = state;
	}
}

static void *mwi_monitor_handler(void *data)
{
	while (!mwi_monitor.stop) {
		struct timespec ts = { 0, };
		struct timeval polltime;
		struct mailbox_mapping *mm;

		tris_mutex_lock(&mwi_monitor.lock);

		mwi_monitor.last_poll = tris_tvnow();

		TRIS_LIST_TRAVERSE(&mwi_monitor.mailbox_mappings, mm, entry)
			poll_mailbox(mm);

		/* Sleep up to the configured polling interval.  Allow unload_module()
		 * to signal us to wake up and exit. */
		polltime = tris_tvadd(mwi_monitor.last_poll, tris_tv(mwi_monitor.polling_interval, 0));
		ts.tv_sec = polltime.tv_sec;
		ts.tv_nsec = polltime.tv_usec * 1000;
		tris_cond_timedwait(&mwi_monitor.cond, &mwi_monitor.lock, &ts);

		tris_mutex_unlock(&mwi_monitor.lock);
	}

	return NULL;
}

static struct tris_smdi_interface *alloc_smdi_interface(void)
{
	struct tris_smdi_interface *iface;

	if (!(iface = tris_calloc(1, sizeof(*iface))))
		return NULL;

	ASTOBJ_INIT(iface);
	ASTOBJ_CONTAINER_INIT(&iface->md_q);
	ASTOBJ_CONTAINER_INIT(&iface->mwi_q);

	tris_mutex_init(&iface->md_q_lock);
	tris_cond_init(&iface->md_q_cond, NULL);

	tris_mutex_init(&iface->mwi_q_lock);
	tris_cond_init(&iface->mwi_q_cond, NULL);

	return iface;
}

/*!
 * \internal
 * \brief Load and reload SMDI configuration.
 * \param reload this should be 1 if we are reloading and 0 if not.
 *
 * This function loads/reloads the SMDI configuration and starts and stops
 * interfaces accordingly.
 *
 * \return zero on success, -1 on failure, and 1 if no smdi interfaces were started.
 */
static int smdi_load(int reload)
{
	struct tris_config *conf;
	struct tris_variable *v;
	struct tris_smdi_interface *iface = NULL;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	int res = 0;

	/* Config options */
	speed_t baud_rate = B9600;     /* 9600 baud rate */
	tcflag_t paritybit = PARENB;   /* even parity checking */
	tcflag_t charsize = CS7;       /* seven bit characters */
	int stopbits = 0;              /* One stop bit */
	
	int msdstrip = 0;              /* strip zero digits */
	long msg_expiry = SMDI_MSG_EXPIRY_TIME;

	if (!(conf = tris_config_load(config_file, config_flags)) || conf == CONFIG_STATUS_FILEINVALID) {
		if (reload)
			tris_log(LOG_NOTICE, "Unable to reload config %s: SMDI untouched\n", config_file);
		else
			tris_log(LOG_NOTICE, "Unable to load config %s: SMDI disabled\n", config_file);
		return 1;
	} else if (conf == CONFIG_STATUS_FILEUNCHANGED)
		return 0;

	/* Mark all interfaces that we are listening on.  We will unmark them
	 * as we find them in the config file, this way we know any interfaces
	 * still marked after we have finished parsing the config file should
	 * be stopped.
	 */
	if (reload)
		ASTOBJ_CONTAINER_MARKALL(&smdi_ifaces);

	for (v = tris_variable_browse(conf, "interfaces"); v; v = v->next) {
		if (!strcasecmp(v->name, "baudrate")) {
			if (!strcasecmp(v->value, "9600"))
				baud_rate = B9600;
			else if (!strcasecmp(v->value, "4800"))
				baud_rate = B4800;
			else if (!strcasecmp(v->value, "2400"))
				baud_rate = B2400;
			else if (!strcasecmp(v->value, "1200"))
				baud_rate = B1200;
			else {
				tris_log(LOG_NOTICE, "Invalid baud rate '%s' specified in %s (line %d), using default\n", v->value, config_file, v->lineno);
				baud_rate = B9600;
			}
		} else if (!strcasecmp(v->name, "msdstrip")) {
			if (!sscanf(v->value, "%30d", &msdstrip)) {
				tris_log(LOG_NOTICE, "Invalid msdstrip value in %s (line %d), using default\n", config_file, v->lineno);
				msdstrip = 0;
			} else if (0 > msdstrip || msdstrip > 9) {
				tris_log(LOG_NOTICE, "Invalid msdstrip value in %s (line %d), using default\n", config_file, v->lineno);
				msdstrip = 0;
			}
		} else if (!strcasecmp(v->name, "msgexpirytime")) {
			if (!sscanf(v->value, "%30ld", &msg_expiry)) {
				tris_log(LOG_NOTICE, "Invalid msgexpirytime value in %s (line %d), using default\n", config_file, v->lineno);
				msg_expiry = SMDI_MSG_EXPIRY_TIME;
			}
		} else if (!strcasecmp(v->name, "paritybit")) {
			if (!strcasecmp(v->value, "even"))
				paritybit = PARENB;
			else if (!strcasecmp(v->value, "odd"))
				paritybit = PARENB | PARODD;
			else if (!strcasecmp(v->value, "none"))
				paritybit = ~PARENB;
			else {
				tris_log(LOG_NOTICE, "Invalid parity bit setting in %s (line %d), using default\n", config_file, v->lineno);
				paritybit = PARENB;
			}
		} else if (!strcasecmp(v->name, "charsize")) {
			if (!strcasecmp(v->value, "7"))
				charsize = CS7;
			else if (!strcasecmp(v->value, "8"))
				charsize = CS8;
			else {
				tris_log(LOG_NOTICE, "Invalid character size setting in %s (line %d), using default\n", config_file, v->lineno);
				charsize = CS7;
			}
		} else if (!strcasecmp(v->name, "twostopbits")) {
			stopbits = tris_true(v->name);
		} else if (!strcasecmp(v->name, "smdiport")) {
			if (reload) {
				/* we are reloading, check if we are already
				 * monitoring this interface, if we are we do
				 * not want to start it again.  This also has
				 * the side effect of not updating different
				 * setting for the serial port, but it should
				 * be trivial to rewrite this section so that
				 * options on the port are changed without
				 * restarting the interface.  Or the interface
				 * could be restarted with out emptying the
				 * queue. */
				if ((iface = ASTOBJ_CONTAINER_FIND(&smdi_ifaces, v->value))) {
					tris_log(LOG_NOTICE, "SMDI interface %s already running, not restarting\n", iface->name);
					ASTOBJ_UNMARK(iface);
					ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
					continue;
				}
			}
			
			if (!(iface = alloc_smdi_interface()))
				continue;

			tris_copy_string(iface->name, v->value, sizeof(iface->name));

			iface->thread = TRIS_PTHREADT_NULL;

			if (!(iface->file = fopen(iface->name, "r"))) {
				tris_log(LOG_ERROR, "Error opening SMDI interface %s (%s)\n", iface->name, strerror(errno));
				ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
				continue;
			}

			iface->fd = fileno(iface->file);

			/* Set the proper attributes for our serial port. */

			/* get the current attributes from the port */
			if (tcgetattr(iface->fd, &iface->mode)) {
				tris_log(LOG_ERROR, "Error getting atributes of %s (%s)\n", iface->name, strerror(errno));
				ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
				continue;
			}

			/* set the desired speed */
			if (cfsetispeed(&iface->mode, baud_rate) || cfsetospeed(&iface->mode, baud_rate)) {
				tris_log(LOG_ERROR, "Error setting baud rate on %s (%s)\n", iface->name, strerror(errno));
				ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
				continue;
			}
			
			/* set the stop bits */
			if (stopbits)
				iface->mode.c_cflag = iface->mode.c_cflag | CSTOPB;   /* set two stop bits */
			else
				iface->mode.c_cflag = iface->mode.c_cflag & ~CSTOPB;  /* set one stop bit */
			
			/* set the parity */
			iface->mode.c_cflag = (iface->mode.c_cflag & ~PARENB & ~PARODD) | paritybit;
			
			/* set the character size */
			iface->mode.c_cflag = (iface->mode.c_cflag & ~CSIZE) | charsize;
			
			/* commit the desired attributes */
			if (tcsetattr(iface->fd, TCSAFLUSH, &iface->mode)) {
				tris_log(LOG_ERROR, "Error setting attributes on %s (%s)\n", iface->name, strerror(errno));
				ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
				continue;
			}

			/* set the msdstrip */
			iface->msdstrip = msdstrip;

			/* set the message expiry time */
			iface->msg_expiry = msg_expiry;

			/* start the listener thread */
			tris_verb(3, "Starting SMDI monitor thread for %s\n", iface->name);
			if (tris_pthread_create_background(&iface->thread, NULL, smdi_read, iface)) {
				tris_log(LOG_ERROR, "Error starting SMDI monitor thread for %s\n", iface->name);
				ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
				continue;
			}

			ASTOBJ_CONTAINER_LINK(&smdi_ifaces, iface);
			ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);
			tris_module_ref(tris_module_info->self);
		} else {
			tris_log(LOG_NOTICE, "Ignoring unknown option %s in %s\n", v->name, config_file);
		}
	}

	destroy_all_mailbox_mappings();
	mwi_monitor.polling_interval = DEFAULT_POLLING_INTERVAL;
	
	iface = NULL;

	for (v = tris_variable_browse(conf, "mailboxes"); v; v = v->next) {
		if (!strcasecmp(v->name, "smdiport")) {
			if (iface)
				ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);

			if (!(iface = ASTOBJ_CONTAINER_FIND(&smdi_ifaces, v->value))) {
				tris_log(LOG_NOTICE, "SMDI interface %s not found\n", v->value);
				continue;
			}
		} else if (!strcasecmp(v->name, "pollinginterval")) {
			if (sscanf(v->value, "%30u", &mwi_monitor.polling_interval) != 1) {
				tris_log(LOG_ERROR, "Invalid value for pollinginterval: %s\n", v->value);
				mwi_monitor.polling_interval = DEFAULT_POLLING_INTERVAL;
			}
		} else {
			if (!iface) {
				tris_log(LOG_ERROR, "Mailbox mapping ignored, no valid SMDI interface specified in mailboxes section\n");
				continue;
			}
			append_mailbox_mapping(v, iface);
		}
	}

	if (iface)
		ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);

	tris_config_destroy(conf);
	
	if (!TRIS_LIST_EMPTY(&mwi_monitor.mailbox_mappings) && mwi_monitor.thread == TRIS_PTHREADT_NULL
		&& tris_pthread_create_background(&mwi_monitor.thread, NULL, mwi_monitor_handler, NULL)) {
		tris_log(LOG_ERROR, "Failed to start MWI monitoring thread.  This module will not operate.\n");
		return TRIS_MODULE_LOAD_FAILURE;
	}

	/* Prune any interfaces we should no longer monitor. */
	if (reload)
		ASTOBJ_CONTAINER_PRUNE_MARKED(&smdi_ifaces, tris_smdi_interface_destroy);
	
	ASTOBJ_CONTAINER_RDLOCK(&smdi_ifaces);
	/* TODO: this is bad, we need an ASTOBJ method for this! */
	if (!smdi_ifaces.head)
		res = 1;
	ASTOBJ_CONTAINER_UNLOCK(&smdi_ifaces);
	
	return res;
}

struct smdi_msg_datastore {
	unsigned int id;
	struct tris_smdi_interface *iface;
	struct tris_smdi_md_message *md_msg;
};

static void smdi_msg_datastore_destroy(void *data)
{
	struct smdi_msg_datastore *smd = data;

	if (smd->iface)
		ASTOBJ_UNREF(smd->iface, tris_smdi_interface_destroy);

	if (smd->md_msg)
		ASTOBJ_UNREF(smd->md_msg, tris_smdi_md_message_destroy);

	free(smd);
}

static const struct tris_datastore_info smdi_msg_datastore_info = {
	.type = "SMDIMSG",
	.destroy = smdi_msg_datastore_destroy,
};

static int smdi_msg_id;

/*! In milliseconds */
#define SMDI_RETRIEVE_TIMEOUT_DEFAULT 3000

TRIS_APP_OPTIONS(smdi_msg_ret_options, BEGIN_OPTIONS
	TRIS_APP_OPTION('t', OPT_SEARCH_TERMINAL),
	TRIS_APP_OPTION('n', OPT_SEARCH_NUMBER),
END_OPTIONS );

static int smdi_msg_retrieve_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_module_user *u;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(port);
		TRIS_APP_ARG(search_key);
		TRIS_APP_ARG(timeout);
		TRIS_APP_ARG(options);
	);
	struct tris_flags options = { 0 };
	unsigned int timeout = SMDI_RETRIEVE_TIMEOUT_DEFAULT;
	int res = -1;
	char *parse = NULL;
	struct smdi_msg_datastore *smd = NULL;
	struct tris_datastore *datastore = NULL;
	struct tris_smdi_interface *iface = NULL;
	struct tris_smdi_md_message *md_msg = NULL;

	u = tris_module_user_add(chan);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_ERROR, "SMDI_MSG_RETRIEVE requires an argument\n");
		goto return_error;
	}

	if (!chan) {
		tris_log(LOG_ERROR, "SMDI_MSG_RETRIEVE must be used with a channel\n");
		goto return_error;
	}

	tris_autoservice_start(chan);

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (tris_strlen_zero(args.port) || tris_strlen_zero(args.search_key)) {
		tris_log(LOG_ERROR, "Invalid arguments provided to SMDI_MSG_RETRIEVE\n");
		goto return_error;
	}

	if (!(iface = tris_smdi_interface_find(args.port))) {
		tris_log(LOG_ERROR, "SMDI port '%s' not found\n", args.port);
		goto return_error;
	}

	if (!tris_strlen_zero(args.options)) {
		tris_app_parse_options(smdi_msg_ret_options, &options, NULL, args.options);
	}

	if (!tris_strlen_zero(args.timeout)) {
		if (sscanf(args.timeout, "%30u", &timeout) != 1) {
			tris_log(LOG_ERROR, "'%s' is not a valid timeout\n", args.timeout);
			timeout = SMDI_RETRIEVE_TIMEOUT_DEFAULT;
		}
	}

	if (!(md_msg = smdi_message_wait(iface, timeout, SMDI_MD, args.search_key, options))) {
		tris_log(LOG_WARNING, "No SMDI message retrieved for search key '%s' after "
			"waiting %u ms.\n", args.search_key, timeout);
		goto return_error;
	}

	if (!(smd = tris_calloc(1, sizeof(*smd))))
		goto return_error;

	smd->iface = ASTOBJ_REF(iface);
	smd->md_msg = ASTOBJ_REF(md_msg);
	smd->id = tris_atomic_fetchadd_int((int *) &smdi_msg_id, 1);
	snprintf(buf, len, "%u", smd->id);

	if (!(datastore = tris_datastore_alloc(&smdi_msg_datastore_info, buf)))
		goto return_error;

	datastore->data = smd;

	tris_channel_lock(chan);
	tris_channel_datastore_add(chan, datastore);
	tris_channel_unlock(chan);

	res = 0;

return_error:
	if (iface)
		ASTOBJ_UNREF(iface, tris_smdi_interface_destroy);

	if (md_msg)
		ASTOBJ_UNREF(md_msg, tris_smdi_md_message_destroy);

	if (smd && !datastore)
		smdi_msg_datastore_destroy(smd);

	if (parse)
		tris_autoservice_stop(chan);

	tris_module_user_remove(u);

	return res;
}

static int smdi_msg_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct tris_module_user *u;
	int res = -1;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(id);
		TRIS_APP_ARG(component);
	);
	char *parse;
	struct tris_datastore *datastore = NULL;
	struct smdi_msg_datastore *smd = NULL;

	u = tris_module_user_add(chan);

	if (!chan) {
		tris_log(LOG_ERROR, "SMDI_MSG can not be called without a channel\n");
		goto return_error;
	}

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "SMDI_MSG requires an argument\n");
		goto return_error;
	}

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (tris_strlen_zero(args.id)) {
		tris_log(LOG_WARNING, "ID must be supplied to SMDI_MSG\n");
		goto return_error;
	}

	if (tris_strlen_zero(args.component)) {
		tris_log(LOG_WARNING, "ID must be supplied to SMDI_MSG\n");
		goto return_error;
	}

	tris_channel_lock(chan);
	datastore = tris_channel_datastore_find(chan, &smdi_msg_datastore_info, args.id);
	tris_channel_unlock(chan);
	
	if (!datastore) {
		tris_log(LOG_WARNING, "No SMDI message found for message ID '%s'\n", args.id);
		goto return_error;
	}

	smd = datastore->data;

	if (!strcasecmp(args.component, "number")) {
		tris_copy_string(buf, smd->md_msg->mesg_desk_num, len);
	} else if (!strcasecmp(args.component, "terminal")) {
		tris_copy_string(buf, smd->md_msg->mesg_desk_term, len);
	} else if (!strcasecmp(args.component, "station")) {
		tris_copy_string(buf, smd->md_msg->fwd_st, len);
	} else if (!strcasecmp(args.component, "callerid")) {
		tris_copy_string(buf, smd->md_msg->calling_st, len);
	} else if (!strcasecmp(args.component, "type")) {
		snprintf(buf, len, "%c", smd->md_msg->type);
	} else {
		tris_log(LOG_ERROR, "'%s' is not a valid message component for SMDI_MSG\n",
			args.component);
		goto return_error;
	}

	res = 0;

return_error:
	tris_module_user_remove(u);

	return res;
}

static struct tris_custom_function smdi_msg_retrieve_function = {
	.name = "SMDI_MSG_RETRIEVE",
	.synopsis = "Retrieve an SMDI message.",
	.syntax = "SMDI_MSG_RETRIEVE(<smdi port>,<search key>[,timeout[,options]])",
	.desc = 
	"   This function is used to retrieve an incoming SMDI message.  It returns\n"
	"an ID which can be used with the SMDI_MSG() function to access details of\n"
	"the message.  Note that this is a destructive function in the sense that\n"
	"once an SMDI message is retrieved using this function, it is no longer in\n"
	"the global SMDI message queue, and can not be accessed by any other Trismedia\n"
	"channels.  The timeout for this function is optional, and the default is\n"
	"3 seconds.  When providing a timeout, it should be in milliseconds.\n"
	"   The default search is done on the forwarding station ID.  However, if\n"
	"you set one of the search key options in the options field, you can change\n"
	"this behavior.\n"
	"   Options:\n"
	"     t - Instead of searching on the forwarding station, search on the message\n"
	"         desk terminal.\n"
	"     n - Instead of searching on the forwarding station, search on the message\n"
	"         desk number.\n"
	"",
	.read = smdi_msg_retrieve_read,
};

static struct tris_custom_function smdi_msg_function = {
	.name = "SMDI_MSG",
	.synopsis = "Retrieve details about an SMDI message.",
	.syntax = "SMDI_MSG(<message_id>,<component>)",
	.desc = 
	"   This function is used to access details of an SMDI message that was\n"
	"pulled from the incoming SMDI message queue using the SMDI_MSG_RETRIEVE()\n"
	"function.\n"
	"   Valid message components are:\n"
	"      number   - The message desk number\n"
	"      terminal - The message desk terminal\n"
	"      station  - The forwarding station\n"
	"      callerid - The callerID of the calling party that was forwarded\n"
	"      type     - The call type.  The value here is the exact character\n"
	"                 that came in on the SMDI link.  Typically, example values\n"
	"                 are: D - Direct Calls, A - Forward All Calls,\n"
	"                      B - Forward Busy Calls, N - Forward No Answer Calls\n"
	"",
	.read = smdi_msg_read,
};

static int unload_module(void);

static int load_module(void)
{
	int res;
	
	/* initialize our containers */
	memset(&smdi_ifaces, 0, sizeof(smdi_ifaces));
	ASTOBJ_CONTAINER_INIT(&smdi_ifaces);
	
	tris_mutex_init(&mwi_monitor.lock);
	tris_cond_init(&mwi_monitor.cond, NULL);

	tris_custom_function_register(&smdi_msg_retrieve_function);
	tris_custom_function_register(&smdi_msg_function);

	/* load the config and start the listener threads*/
	res = smdi_load(0);
	if (res < 0) {
		unload_module();
		return res;
	} else if (res == 1) {
		unload_module();
		tris_log(LOG_NOTICE, "No SMDI interfaces are available to listen on, not starting SMDI listener.\n");
		return TRIS_MODULE_LOAD_DECLINE;
	}
	
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* this destructor stops any running smdi_read threads */
	ASTOBJ_CONTAINER_DESTROYALL(&smdi_ifaces, tris_smdi_interface_destroy);
	ASTOBJ_CONTAINER_DESTROY(&smdi_ifaces);

	destroy_all_mailbox_mappings();

	tris_mutex_lock(&mwi_monitor.lock);
	mwi_monitor.stop = 1;
	tris_cond_signal(&mwi_monitor.cond);
	tris_mutex_unlock(&mwi_monitor.lock);

	if (mwi_monitor.thread != TRIS_PTHREADT_NULL) {
		pthread_join(mwi_monitor.thread, NULL);
	}

	tris_custom_function_unregister(&smdi_msg_retrieve_function);
	tris_custom_function_unregister(&smdi_msg_function);

	return 0;
}

static int reload(void)
{
	int res;

	res = smdi_load(1);

	if (res < 0) {
		return res;
	} else if (res == 1) {
		tris_log(LOG_WARNING, "No SMDI interfaces were specified to listen on, not starting SDMI listener.\n");
		return 0;
	} else
		return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_GLOBAL_SYMBOLS, "Simplified Message Desk Interface (SMDI) Resource",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
