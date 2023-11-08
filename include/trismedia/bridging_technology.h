/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Digium, Inc.
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
 * \brief Channel Bridging API
 * \author Joshua Colp <jcolp@digium.com>
 */

#ifndef _TRISMEDIA_BRIDGING_TECHNOLOGY_H
#define _TRISMEDIA_BRIDGING_TECHNOLOGY_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! \brief Preference for choosing the bridge technology */
enum tris_bridge_preference {
	/*! Bridge technology should have high precedence over other bridge technologies */
	TRIS_BRIDGE_PREFERENCE_HIGH = 0,
	/*! Bridge technology is decent, not the best but should still be considered over low */
	TRIS_BRIDGE_PREFERENCE_MEDIUM,
	/*! Bridge technology is low, it should not be considered unless it is absolutely needed */
	TRIS_BRIDGE_PREFERENCE_LOW,
};

/*!
 * \brief Structure that is the essence of a bridge technology
 */
struct tris_bridge_technology {
	/*! Unique name to this bridge technology */
	const char *name;
	/*! The capabilities that this bridge technology is capable of */
	int capabilities;
	/*! Preference level that should be used when determining whether to use this bridge technology or not */
	enum tris_bridge_preference preference;
	/*! Callback for when a bridge is being created */
	int (*create)(struct tris_bridge *bridge);
	/*! Callback for when a bridge is being destroyed */
	int (*destroy)(struct tris_bridge *bridge);
	/*! Callback for when a channel is being added to a bridge */
	int (*join)(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel);
	/*! Callback for when a channel is leaving a bridge */
	int (*leave)(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel);
	/*! Callback for when a channel is suspended from the bridge */
	void (*suspend)(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel);
	/*! Callback for when a channel is unsuspended from the bridge */
	void (*unsuspend)(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel);
	/*! Callback to see if a channel is compatible with the bridging technology */
	int (*compatible)(struct tris_bridge_channel *bridge_channel);
	/*! Callback for writing a frame into the bridging technology */
	enum tris_bridge_write_result (*write)(struct tris_bridge *bridge, struct tris_bridge_channel *bridged_channel, struct tris_frame *frame);
	/*! Callback for when a file descriptor trips */
	int (*fd)(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, int fd);
	/*! Callback for replacement thread function */
	int (*thread)(struct tris_bridge *bridge);
	/*! Callback for poking a bridge thread */
	int (*poke)(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel);
	/*! Formats that the bridge technology supports */
	int formats;
	/*! Bit to indicate whether the bridge technology is currently suspended or not */
	unsigned int suspended:1;
	/*! Module this bridge technology belongs to. Is used for reference counting when creating/destroying a bridge. */
	struct tris_module *mod;
	/*! Linked list information */
	TRIS_RWLIST_ENTRY(tris_bridge_technology) entry;
};

/*! \brief Register a bridge technology for use
 *
 * \param technology The bridge technology to register
 * \param module The module that is registering the bridge technology
 *
 * \retval 0 on success
 * \retval -1 on failure
 *
 * Example usage:
 *
 * \code
 * tris_bridge_technology_register(&simple_bridge_tech);
 * \endcode
 *
 * This registers a bridge technology declared as the structure
 * simple_bridge_tech with the bridging core and makes it available for
 * use when creating bridges.
 */
int __tris_bridge_technology_register(struct tris_bridge_technology *technology, struct tris_module *mod);

/*! \brief See \ref __tris_bridge_technology_register() */
#define tris_bridge_technology_register(technology) __tris_bridge_technology_register(technology, tris_module_info->self)

/*! \brief Unregister a bridge technology from use
 *
 * \param technology The bridge technology to unregister
 *
 * \retval 0 on success
 * \retval -1 on failure
 *
 * Example usage:
 *
 * \code
 * tris_bridge_technology_unregister(&simple_bridge_tech);
 * \endcode
 *
 * This unregisters a bridge technlogy declared as the structure
 * simple_bridge_tech with the bridging core. It will no longer be
 * considered when creating a new bridge.
 */
int tris_bridge_technology_unregister(struct tris_bridge_technology *technology);

/*! \brief Feed notification that a frame is waiting on a channel into the bridging core
 *
 * \param bridge The bridge that the notification should influence
 * \param bridge_channel Bridge channel the notification was received on (if known)
 * \param chan Channel the notification was received on (if known)
 * \param outfd File descriptor that the notification was received on (if known)
 *
 * Example usage:
 *
 * \code
 * tris_bridge_handle_trip(bridge, NULL, chan, -1);
 * \endcode
 *
 * This tells the bridging core that a frame has been received on
 * the channel pointed to by chan and that it should be read and handled.
 *
 * \note This should only be used by bridging technologies.
 */
void tris_bridge_handle_trip(struct tris_bridge *bridge, struct tris_bridge_channel *bridge_channel, struct tris_channel *chan, int outfd);

/*! \brief Suspend a bridge technology from consideration
 *
 * \param technology The bridge technology to suspend
 *
 * Example usage:
 *
 * \code
 * tris_bridge_technology_suspend(&simple_bridge_tech);
 * \endcode
 *
 * This suspends the bridge technology simple_bridge_tech from being considered
 * when creating a new bridge. Existing bridges using the bridge technology
 * are not affected.
 */
void tris_bridge_technology_suspend(struct tris_bridge_technology *technology);

/*! \brief Unsuspend a bridge technology
 *
 * \param technology The bridge technology to unsuspend
 *
 * Example usage:
 *
 * \code
 * tris_bridge_technology_unsuspend(&simple_bridge_tech);
 * \endcode
 *
 * This makes the bridge technology simple_bridge_tech considered when
 * creating a new bridge again.
 */
void tris_bridge_technology_unsuspend(struct tris_bridge_technology *technology);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_BRIDGING_TECHNOLOGY_H */
