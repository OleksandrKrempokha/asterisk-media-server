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
 * \brief Device state management
 *
 * To subscribe to device state changes, use the generic tris_event_subscribe
 * method.  For an example, see apps/app_queue.c.
 *
 * \todo Currently, when the state of a device changes, the device state provider
 * calls one of the functions defined here to queue an object to say that the
 * state of a device has changed.  However, this does not include the new state.
 * Another thread processes these device state change objects and calls the
 * device state provider's callback to figure out what the new state is.  It
 * would make a lot more sense for the new state to be included in the original
 * function call that says the state of a device has changed.  However, it
 * will take a lot of work to change this.
 *
 * \arg See \ref AstExtState
 */

#ifndef _TRISMEDIA_DEVICESTATE_H
#define _TRISMEDIA_DEVICESTATE_H

#include "trismedia/channel.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! \brief Device States
 *  \note The order of these states may not change because they are included
 *        in Trismedia events which may be transmitted across the network to
 *        other servers.
 */
enum tris_device_state {
	TRIS_DEVICE_UNKNOWN,      /*!< Device is valid but channel didn't know state */
	TRIS_DEVICE_NOT_INUSE,    /*!< Device is not used */
	TRIS_DEVICE_INUSE,        /*!< Device is in use */
	TRIS_DEVICE_BUSY,         /*!< Device is busy */
	TRIS_DEVICE_INVALID,      /*!< Device is invalid */
	TRIS_DEVICE_UNAVAILABLE,  /*!< Device is unavailable */
	TRIS_DEVICE_RINGING,      /*!< Device is ringing */
	TRIS_DEVICE_RINGINUSE,    /*!< Device is ringing *and* in use */
	TRIS_DEVICE_ONHOLD,       /*!< Device is on hold */
	TRIS_DEVICE_TOTAL,        /*/ Total num of device states, used for testing */
};

/*! \brief Devicestate provider call back */
typedef enum tris_device_state (*tris_devstate_prov_cb_type)(const char *data);

/*!
 * \brief Convert channel state to devicestate
 *
 * \param chanstate Current channel state
 * \since 1.6.1
 */
enum tris_device_state tris_state_chan2dev(enum tris_channel_state chanstate);

/*!
 * \brief Convert device state to text string for output
 *
 * \param devstate Current device state
 */
const char *devstate2str(enum tris_device_state devstate) attribute_pure __attribute__((deprecated));
const char *tris_devstate2str(enum tris_device_state devstate) attribute_pure;

/*!
 * \brief Convert device state to text string that is easier to parse
 *
 * \param devstate Current device state
 */
const char *tris_devstate_str(enum tris_device_state devstate) attribute_pure;

/*!
 * \brief Convert device state from text to integer value
 *
 * \param val The text representing the device state.  Valid values are anything
 *        that comes after TRIS_DEVICE_ in one of the defined values.
 *
 * \return The TRIS_DEVICE_ integer value
 */
enum tris_device_state tris_devstate_val(const char *val);

/*!
 * \brief Search the Channels by Name
 *
 * \param device like a dial string
 *
 * Search the Device in active channels by compare the channel name against
 * the device name. Compared are only the first chars to the first '-' char.
 *
 * \retval TRIS_DEVICE_UNKNOWN if no channel found
 * \retval TRIS_DEVICE_INUSE if a channel is found
 */
enum tris_device_state tris_parse_device_state(const char *device);

/*!
 * \brief Asks a channel for device state
 *
 * \param device like a dial string
 *
 * Asks a channel for device state, data is normally a number from a dial string
 * used by the low level module
 * Tries the channel device state callback if not supported search in the
 * active channels list for the device.
 *
 * \retval an TRIS_DEVICE_??? state
 * \retval -1 on failure
 */
enum tris_device_state tris_device_state(const char *device);

/*!
 * \brief Tells Trismedia the State for Device is changed
 *
 * \param state the new state of the device
 * \param fmt device name like a dial string with format parameters
 *
 * The new state of the device will be sent off to any subscribers
 * of device states.  It will also be stored in the internal event
 * cache.
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_devstate_changed(enum tris_device_state state, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/*!
 * \brief Tells Trismedia the State for Device is changed
 *
 * \param state the new state of the device
 * \param device device name like a dial string with format parameters
 *
 * The new state of the device will be sent off to any subscribers
 * of device states.  It will also be stored in the internal event
 * cache.
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_devstate_changed_literal(enum tris_device_state state, const char *device);

/*!
 * \brief Tells Trismedia the State for Device is changed.
 * (Accept change notification, add it to change queue.)
 *
 * \param fmt device name like a dial string with format parameters
 *
 * Trismedia polls the new extension states and calls the registered
 * callbacks for the changed extensions
 *
 * \retval 0 on success
 * \retval -1 on failure
 *
 * \note This is deprecated in favor of tris_devstate_changed()
 * \version 1.6.1 deprecated
 */
int tris_device_state_changed(const char *fmt, ...)
	__attribute__((deprecated,format(printf, 1, 2)));

/*!
 * \brief Tells Trismedia the State for Device is changed
 *
 * \param device device name like a dial string
 *
 * Trismedia polls the new extension states and calls the registered
 * callbacks for the changed extensions
 *
 * \retval 0 on success
 * \retval -1 on failure
 *
 * \note This is deprecated in favor of tris_devstate_changed_literal()
 * \version 1.6.1 deprecated
 */
int tris_device_state_changed_literal(const char *device)
	__attribute__((deprecated));

/*!
 * \brief Add device state provider
 *
 * \param label to use in hint, like label:object
 * \param callback Callback
 *
 * \retval 0 success
 * \retval -1 failure
 */
int tris_devstate_prov_add(const char *label, tris_devstate_prov_cb_type callback);

/*!
 * \brief Remove device state provider
 *
 * \param label to use in hint, like label:object
 *
 * \retval -1 on failure
 * \retval 0 on success
 */
int tris_devstate_prov_del(const char *label);

/*!
 * \brief An object to hold state when calculating aggregate device state
 */
struct tris_devstate_aggregate;

/*!
 * \brief Initialize aggregate device state
 *
 * \param[in] agg the state object
 *
 * \return nothing
 * \since 1.6.1
 */
void tris_devstate_aggregate_init(struct tris_devstate_aggregate *agg);

/*!
 * \brief Add a device state to the aggregate device state
 *
 * \param[in] agg the state object
 * \param[in] state the state to add
 *
 * \return nothing
 * \since 1.6.1
 */
void tris_devstate_aggregate_add(struct tris_devstate_aggregate *agg, enum tris_device_state state);

/*!
 * \brief Get the aggregate device state result
 *
 * \param[in] agg the state object
 *
 * \return the aggregate device state after adding some number of device states.
 * \since 1.6.1
 */
enum tris_device_state tris_devstate_aggregate_result(struct tris_devstate_aggregate *agg);

/*!
 * \brief You shouldn't care about the contents of this struct
 *
 * This struct is only here so that it can be easily declared on the stack.
 */
struct tris_devstate_aggregate {
	unsigned int all_unknown:1;
	unsigned int all_unavail:1;
	unsigned int all_busy:1;
	unsigned int all_free:1;
	unsigned int on_hold:1;
	unsigned int busy:1;
	unsigned int in_use:1;
	unsigned int ring:1;
};

/*!
 * \brief Enable distributed device state processing.
 *
 * \details
 * By default, Trismedia assumes that device state change events will only be
 * originating from one instance.  If a module gets loaded and configured such
 * that multiple instances of Trismedia will be sharing device state, this
 * function should be called to enable distributed device state processing.
 * It is off by default to save on unnecessary processing.
 *
 * \retval 0 success
 * \retval -1 failure
 */
int tris_enable_distributed_devstate(void);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_DEVICESTATE_H */
