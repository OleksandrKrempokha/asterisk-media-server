/*
 * abstract_jb: common implementation-independent jitterbuffer stuff
 *
 * Copyright (C) 2005, Attractel OOD
 *
 * Contributors:
 * Slav Klenov <slav@securax.org>
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
 *
 * A license has been granted to Digium (via disclaimer) for the use of
 * this code.
 */

/*! \file
 *
 * \brief Common implementation-independent jitterbuffer stuff.
 * 
 * \author Slav Klenov <slav@securax.org>
 */

#ifndef _ABSTRACT_JB_H_
#define _ABSTRACT_JB_H_

#include <sys/time.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

struct tris_frame;

/* Configuration flags */
enum {
	TRIS_JB_ENABLED = (1 << 0),
	TRIS_JB_FORCED =  (1 << 1),
	TRIS_JB_LOG =     (1 << 2)
};

#define TRIS_JB_IMPL_NAME_SIZE 12

/*!
 * \brief General jitterbuffer configuration.
 */
struct tris_jb_conf
{
	/*! \brief Combination of the TRIS_JB_ENABLED, TRIS_JB_FORCED and TRIS_JB_LOG flags. */
	unsigned int flags;
	/*! \brief Max size of the jitterbuffer implementation. */
	long max_size;
	/*! \brief Resynchronization threshold of the jitterbuffer implementation. */
	long resync_threshold;
	/*! \brief Name of the jitterbuffer implementation to be used. */
	char impl[TRIS_JB_IMPL_NAME_SIZE];
	/*! \brief amount of additional jitterbuffer adjustment */
	long target_extra;
};


/* Jitterbuffer configuration property names */
#define TRIS_JB_CONF_PREFIX "jb"
#define TRIS_JB_CONF_ENABLE "enable"
#define TRIS_JB_CONF_FORCE "force"
#define TRIS_JB_CONF_MAX_SIZE "maxsize"
#define TRIS_JB_CONF_RESYNCH_THRESHOLD "resyncthreshold"
#define TRIS_JB_CONF_TARGET_EXTRA "targetextra"
#define TRIS_JB_CONF_IMPL "impl"
#define TRIS_JB_CONF_LOG "log"


struct tris_jb_impl;


/*!
 * \brief General jitterbuffer state.
 */
struct tris_jb
{
	/*! \brief Jitterbuffer configuration. */
	struct tris_jb_conf conf;
	/*! \brief Jitterbuffer implementation to be used. */
	struct tris_jb_impl *impl;
	/*! \brief Jitterbuffer object, passed to the implementation. */
	void *jbobj;
	/*! \brief The time the jitterbuffer was created. */
	struct timeval timebase;
	/*! \brief The time the next frame should be played. */
	long next;
	/*! \brief Voice format of the last frame in. */
	int last_format;
	/*! \brief File for frame timestamp tracing. */
	FILE *logfile;
	/*! \brief Jitterbuffer internal state flags. */
	unsigned int flags;
};


/*!
 * \brief Checks the need of a jb use in a generic bridge.
 * \param c0 first bridged channel.
 * \param c1 second bridged channel.
 *
 * Called from tris_generic_bridge() when two channels are entering in a bridge.
 * The function checks the need of a jitterbuffer, depending on both channel's
 * configuration and technology properties. As a result, this function sets
 * appropriate internal jb flags to the channels, determining further behaviour
 * of the bridged jitterbuffers.
 *
 * \retval zero if there are no jitter buffers in use
 * \retval non-zero if there are
 */
int tris_jb_do_usecheck(struct tris_channel *c0, struct tris_channel *c1);


/*!
 * \brief Calculates the time, left to the closest delivery moment in a bridge.
 * \param c0 first bridged channel.
 * \param c1 second bridged channel.
 * \param time_left bridge time limit, or -1 if not set.
 *
 * Called from tris_generic_bridge() to determine the maximum time to wait for
 * activity in tris_waitfor_n() call. If neihter of the channels is using jb,
 * this function returns the time limit passed.
 *
 * \return maximum time to wait.
 */
int tris_jb_get_when_to_wakeup(struct tris_channel *c0, struct tris_channel *c1, int time_left);


/*!
 * \brief Puts a frame into a channel jitterbuffer.
 * \param chan channel.
 * \param f frame.
 *
 * Called from tris_generic_bridge() to put a frame into a channel's jitterbuffer.
 * The function will successfuly enqueue a frame if and only if:
 * 1. the channel is using a jitterbuffer (as determined by tris_jb_do_usecheck()),
 * 2. the frame's type is TRIS_FRAME_VOICE,
 * 3. the frame has timing info set and has length >= 2 ms,
 * 4. there is no some internal error happened (like failed memory allocation).
 * Frames, successfuly queued, should be delivered by the channel's jitterbuffer,
 * when their delivery time has came.
 * Frames, not successfuly queued, should be delivered immediately.
 * Dropped by the jb implementation frames are considered successfuly enqueued as
 * far as they should not be delivered at all.
 *
 * \retval 0 if the frame was queued
 * \retval -1 if not
 */
int tris_jb_put(struct tris_channel *chan, struct tris_frame *f);


/*!
 * \brief Deliver the queued frames that should be delivered now for both channels.
 * \param c0 first bridged channel.
 * \param c1 second bridged channel.
 *
 * Called from tris_generic_bridge() to deliver any frames, that should be delivered
 * for the moment of invocation. Does nothing if neihter of the channels is using jb
 * or has any frames currently queued in. The function delivers frames usig tris_write()
 * each of the channels.
 */
void tris_jb_get_and_deliver(struct tris_channel *c0, struct tris_channel *c1);


/*!
 * \brief Destroys jitterbuffer on a channel.
 * \param chan channel.
 *
 * Called from tris_channel_free() when a channel is destroyed.
 */
void tris_jb_destroy(struct tris_channel *chan);


/*!
 * \brief Sets jitterbuffer configuration property.
 * \param conf configuration to store the property in.
 * \param varname property name.
 * \param value property value.
 *
 * Called from a channel driver to build a jitterbuffer configuration typically when
 * reading a configuration file. It is not necessary for a channel driver to know
 * each of the jb configuration property names. The jitterbuffer itself knows them.
 * The channel driver can pass each config var it reads through this function. It will
 * return 0 if the variable was consumed from the jb conf.
 *
 * \return zero if the property was set to the configuration, -1 if not.
 */
int tris_jb_read_conf(struct tris_jb_conf *conf, const char *varname, const char *value);


/*!
 * \brief Configures a jitterbuffer on a channel.
 * \param chan channel to configure.
 * \param conf configuration to apply.
 *
 * Called from a channel driver when a channel is created and its jitterbuffer needs
 * to be configured.
 */
void tris_jb_configure(struct tris_channel *chan, const struct tris_jb_conf *conf);


/*!
 * \brief Copies a channel's jitterbuffer configuration.
 * \param chan channel.
 * \param conf destination.
 */
void tris_jb_get_config(const struct tris_channel *chan, struct tris_jb_conf *conf);

/*!
 * \brief drops all frames from a jitterbuffer and resets it
 * \param c0 one channel of a bridge
 * \param c1 the other channel of the bridge
 */
void tris_jb_empty_and_reset(struct tris_channel *c0, struct tris_channel *c1);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ABSTRACT_JB_H_ */