/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2008, Digium, Inc.
 *
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
 * \author Russell Bryant <russell@digium.com>
 * \brief Generic event system
 */

#ifndef TRIS_EVENT_DEFS_H
#define TRIS_EVENT_DEFS_H

/*! \brief Event types
 * \note These values can *never* change. */
enum tris_event_type {
	/*! Reserved to provide the ability to subscribe to all events.  A specific
	 *  event should never have a payload of 0. */
	TRIS_EVENT_ALL                 = 0x00,
	/*! This event type is reserved for use by third-party modules to create
	 *  custom events without having to modify this file. 
	 *  \note There are no "custom" IE types, because IEs only have to be
	 *  unique to the event itself, not necessarily across all events. */
	TRIS_EVENT_CUSTOM              = 0x01,
	/*! Voicemail message waiting indication */
	TRIS_EVENT_MWI                 = 0x02,
	/*! Someone has subscribed to events */
	TRIS_EVENT_SUB                 = 0x03,
	/*! Someone has unsubscribed from events */
	TRIS_EVENT_UNSUB               = 0x04,
	/*! The aggregate state of a device across all servers configured to be
	 *  a part of a device state cluster has changed. */
	TRIS_EVENT_DEVICE_STATE        = 0x05,
	/*! The state of a device has changed on _one_ server.  This should not be used
	 *  directly, in general.  Use TRIS_EVENT_DEVICE_STATE instead. */
	TRIS_EVENT_DEVICE_STATE_CHANGE = 0x06,
	/*! Number of event types.  This should be the last event type + 1 */
	TRIS_EVENT_TOTAL               = 0x07,
};

/*! \brief Event Information Element types */
enum tris_event_ie_type {
	/*! Used to terminate the arguments to event functions */
	TRIS_EVENT_IE_END       = -1,

	/*! 
	 * \brief Number of new messages
	 * Used by: TRIS_EVENT_MWI 
	 * Payload type: UINT
	 */
	TRIS_EVENT_IE_NEWMSGS   = 0x01,
	/*! 
	 * \brief Number of
	 * Used by: TRIS_EVENT_MWI 
	 * Payload type: UINT
	 */
	TRIS_EVENT_IE_OLDMSGS   = 0x02,
	/*! 
	 * \brief Mailbox name \verbatim (mailbox[@context]) \endverbatim
	 * Used by: TRIS_EVENT_MWI 
	 * Payload type: STR
	 */
	TRIS_EVENT_IE_MAILBOX   = 0x03,
	/*! 
	 * \brief Unique ID
	 * Used by: TRIS_EVENT_SUB, TRIS_EVENT_UNSUB
	 * Payload type: UINT
	 */
	TRIS_EVENT_IE_UNIQUEID  = 0x04,
	/*! 
	 * \brief Event type 
	 * Used by: TRIS_EVENT_SUB, TRIS_EVENT_UNSUB
	 * Payload type: UINT
	 */
	TRIS_EVENT_IE_EVENTTYPE = 0x05,
	/*!
	 * \brief Hint that someone cares that an IE exists
	 * Used by: TRIS_EVENT_SUB
	 * Payload type: UINT (tris_event_ie_type)
	 */
	TRIS_EVENT_IE_EXISTS    = 0x06,
	/*!
	 * \brief Device Name
	 * Used by TRIS_EVENT_DEVICE_STATE_CHANGE
	 * Payload type: STR
	 */
	TRIS_EVENT_IE_DEVICE    = 0x07,
	/*!
	 * \brief Generic State IE
	 * Used by TRIS_EVENT_DEVICE_STATE_CHANGE
	 * Payload type: UINT
	 * The actual state values depend on the event which
	 * this IE is a part of.
	 */
	 TRIS_EVENT_IE_STATE    = 0x08,
	 /*!
	  * \brief Context IE
	  * Used by TRIS_EVENT_MWI
	  * Payload type: str
	  */
	 TRIS_EVENT_IE_CONTEXT  = 0x09,
	 /*!
	  * \brief Entity ID
	  * Used by All events
	  * Payload type: RAW
	  * This IE indicates which server the event originated from
	  */
	 TRIS_EVENT_IE_EID      = 0x0A,
};

#define TRIS_EVENT_IE_MAX TRIS_EVENT_IE_EID

/*!
 * \brief Payload types for event information elements
 */
enum tris_event_ie_pltype {
	TRIS_EVENT_IE_PLTYPE_UNKNOWN = -1,
	/*! Just check if it exists, not the value */
	TRIS_EVENT_IE_PLTYPE_EXISTS,
	/*! Unsigned Integer (Can be used for signed, too ...) */
	TRIS_EVENT_IE_PLTYPE_UINT,
	/*! String */
	TRIS_EVENT_IE_PLTYPE_STR,
	/*! Raw data, compared with memcmp */
	TRIS_EVENT_IE_PLTYPE_RAW,
};

/*!
 * \brief Results for checking for subscribers
 *
 * \ref tris_event_check_subscriber()
 */
enum tris_event_subscriber_res {
	/*! No subscribers exist */
	TRIS_EVENT_SUB_NONE,
	/*! At least one subscriber exists */
	TRIS_EVENT_SUB_EXISTS,
};

struct tris_event;
struct tris_event_ie;
struct tris_event_sub;
struct tris_event_iterator;

/*!
 * \brief supposed to be an opaque type
 *
 * This is only here so that it can be declared on the stack.
 */
struct tris_event_iterator {
	uint16_t event_len;
	const struct tris_event *event;
	struct tris_event_ie *ie;
};

#endif /* TRIS_EVENT_DEFS_H */
