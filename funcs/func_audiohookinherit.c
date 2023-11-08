/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Digium, Inc.
 *
 * Mark Michelson <mmichelson@digium.com>
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
 * Please follow coding guidelines
 * http://svn.digium.com/view/trismedia/trunk/doc/CODING-GUIDELINES
 */

/*! \file
 *
 * \brief Audiohook inheritance function
 *
 * \author Mark Michelson <mmichelson@digium.com>
 *
 * \ingroup functions
 */

#include "trismedia.h"
#include "trismedia/datastore.h"
#include "trismedia/channel.h"
#include "trismedia/logger.h"
#include "trismedia/audiohook.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"

/*** DOCUMENTATION
 	<function name = "AUDIOHOOK_INHERIT" language="en_US">
		<synopsis>
			Set whether an audiohook may be inherited to another channel
		</synopsis>
		<syntax>
			<parameter name="source" required="true">
				<para>The built-in sources in Trismedia are</para>
				<enumlist>
					<enum name="MixMonitor" />
					<enum name="Chanspy" />
					<enum name="Volume" />
					<enum name="Speex" />
					<enum name="JACK_HOOK" />
				</enumlist>
				<para>Note that the names are not case-sensitive</para>
			</parameter>
		</syntax>
		<description>
			<para>By enabling audiohook inheritance on the channel, you are giving
			permission for an audiohook to be inherited by a descendent channel.
			Inheritance may be be disabled at any point as well.</para>

			<para>Example scenario:</para>
			<para>exten => 2000,1,MixMonitor(blah.wav)</para>
			<para>exten => 2000,n,Set(AUDIOHOOK_INHERIT(MixMonitor)=yes)</para>
			<para>exten => 2000,n,Dial(SIP/2000)</para>
			<para>
			</para>
			<para>exten => 4000,1,Dial(SIP/4000)</para>
			<para>
			</para>
			<para>exten => 5000,1,MixMonitor(blah2.wav)</para>
			<para>exten => 5000,n,Dial(SIP/5000)</para>
			<para>
			</para>
			<para>In this basic dialplan scenario, let's consider the following sample calls</para>
			<para>Call 1: Caller dials 2000. The person who answers then executes an attended</para>
			<para>        transfer to 4000.</para>
			<para>Result: Since extension 2000 set MixMonitor to be inheritable, after the</para>
			<para>        transfer to 4000 has completed, the call will continue to be recorded
			to blah.wav</para>
			<para>
			</para>
			<para>Call 2: Caller dials 5000. The person who answers then executes an attended</para>
			<para>        transfer to 4000.</para>
			<para>Result: Since extension 5000 did not set MixMonitor to be inheritable, the</para>
			<para>        recording will stop once the call has been transferred to 4000.</para>
		</description>
	</function>
 ***/

struct inheritable_audiohook {
	TRIS_LIST_ENTRY(inheritable_audiohook) list;
	char source[1];
};

struct audiohook_inheritance_datastore {
	TRIS_LIST_HEAD (, inheritable_audiohook) allowed_list;
};

static void audiohook_inheritance_fixup(void *data, struct tris_channel *old_chan, struct tris_channel *new_chan);
static void audiohook_inheritance_destroy (void *data);
static const struct tris_datastore_info audiohook_inheritance_info = {
	.type = "audiohook inheritance",
	.destroy = audiohook_inheritance_destroy,
	.chan_fixup = audiohook_inheritance_fixup,
};

/*! \brief Move audiohooks as defined by previous calls to the AUDIOHOOK_INHERIT function
 *
 * Move allowed audiohooks from the old channel to the new channel.
 *
 * \param data The tris_datastore containing audiohook inheritance information that will be moved
 * \param old_chan The "clone" channel from a masquerade. We are moving the audiohook in question off of this channel
 * \param new_chan The "original" channel from a masquerade. We are moving the audiohook in question to this channel
 * \return Void
 */
static void audiohook_inheritance_fixup(void *data, struct tris_channel *old_chan, struct tris_channel *new_chan)
{
	struct inheritable_audiohook *audiohook = NULL;
	struct audiohook_inheritance_datastore *datastore = data;

	tris_debug(2, "inheritance fixup occurring for channels %s(%p) and %s(%p)", old_chan->name, old_chan, new_chan->name, new_chan);

	TRIS_LIST_TRAVERSE(&datastore->allowed_list, audiohook, list) {
		tris_audiohook_move_by_source(old_chan, new_chan, audiohook->source);
		tris_debug(3, "Moved audiohook %s from %s(%p) to %s(%p)\n",
			audiohook->source, old_chan->name, old_chan, new_chan->name, new_chan);
	}
	return;
}

/*! \brief Destroy dynamically allocated data on an audiohook_inheritance_datastore
 *
 * \param data Pointer to the audiohook_inheritance_datastore in question.
 * \return Void
 */
static void audiohook_inheritance_destroy(void *data)
{
	struct audiohook_inheritance_datastore *audiohook_inheritance_datastore = data;
	struct inheritable_audiohook *inheritable_audiohook = NULL;

	while ((inheritable_audiohook = TRIS_LIST_REMOVE_HEAD(&audiohook_inheritance_datastore->allowed_list, list))) {
		tris_free(inheritable_audiohook);
	}

	tris_free(audiohook_inheritance_datastore);
}

/*! \brief create an audiohook_inheritance_datastore and attach it to a channel
 *
 * \param chan The channel to which we wish to attach the new datastore
 * \return Returns the newly created audiohook_inheritance_datastore or NULL on error
 */
static struct audiohook_inheritance_datastore *setup_inheritance_datastore(struct tris_channel *chan)
{
	struct tris_datastore *datastore = NULL;
	struct audiohook_inheritance_datastore *audiohook_inheritance_datastore = NULL;

	if (!(datastore = tris_datastore_alloc(&audiohook_inheritance_info, NULL))) {
		return NULL;
	}

	if (!(audiohook_inheritance_datastore = tris_calloc(1, sizeof(*audiohook_inheritance_datastore)))) {
		tris_datastore_free(datastore);
		return NULL;
	}

	datastore->data = audiohook_inheritance_datastore;
	tris_channel_lock(chan);
	tris_channel_datastore_add(chan, datastore);
	tris_channel_unlock(chan);
	return audiohook_inheritance_datastore;
}

/*! \brief Create a new inheritable_audiohook structure and add it to an audiohook_inheritance_datastore
 *
 * \param audiohook_inheritance_datastore The audiohook_inheritance_datastore we want to add the new inheritable_audiohook to
 * \param source The audiohook source for the newly created inheritable_audiohook
 * \retval 0 Success
 * \retval non-zero Failure
 */
static int setup_inheritable_audiohook(struct audiohook_inheritance_datastore *audiohook_inheritance_datastore, const char *source)
{
	struct inheritable_audiohook *inheritable_audiohook = NULL;

	inheritable_audiohook = tris_calloc(1, sizeof(*inheritable_audiohook) + strlen(source));

	if (!inheritable_audiohook) {
		return -1;
	}

	strcpy(inheritable_audiohook->source, source);
	TRIS_LIST_INSERT_TAIL(&audiohook_inheritance_datastore->allowed_list, inheritable_audiohook, list);
	tris_debug(3, "Set audiohook %s to be inheritable\n", source);
	return 0;
}

/*! \brief Set the permissibility of inheritance for a particular audiohook source on a channel
 *
 * For details regarding what happens in the function, see the inline comments
 *
 * \param chan The channel we are operating on
 * \param function The name of the dialplan function (AUDIOHOOK_INHERIT)
 * \param data The audiohook source for which we are setting inheritance permissions
 * \param value The value indicating the permission for audiohook inheritance
 */
static int func_inheritance_write(struct tris_channel *chan, const char *function, char *data, const char *value)
{
	int allow;
	struct tris_datastore *datastore = NULL;
	struct audiohook_inheritance_datastore *inheritance_datastore = NULL;
	struct inheritable_audiohook *inheritable_audiohook;

	/* Step 1: Get data from function call */
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "No argument provided to INHERITANCE function.\n");
		return -1;
	}

	if (tris_strlen_zero(value)) {
		tris_log(LOG_WARNING, "No value provided to INHERITANCE function.\n");
		return -1;
	}

	allow = tris_true(value);

	/* Step 2: retrieve or set up datastore */
	tris_channel_lock(chan);
	if (!(datastore = tris_channel_datastore_find(chan, &audiohook_inheritance_info, NULL))) {
		tris_channel_unlock(chan);
		/* In the case where we cannot find the datastore, we can take a few shortcuts */
		if (!allow) {
			tris_debug(1, "Audiohook %s is already set to not be inheritable on channel %s\n", data, chan->name);
			return 0;
		} else if (!(inheritance_datastore = setup_inheritance_datastore(chan))) {
			tris_log(LOG_WARNING, "Unable to set up audiohook inheritance datastore on channel %s\n", chan->name);
			return -1;
		} else {
			return setup_inheritable_audiohook(inheritance_datastore, data);
		}
	} else {
		inheritance_datastore = datastore->data;
	}
	tris_channel_unlock(chan);

	/* Step 3: Traverse the list to see if we're trying something redundant */

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&inheritance_datastore->allowed_list, inheritable_audiohook, list) {
		if (!strcasecmp(inheritable_audiohook->source, data)) {
			if (allow) {
				tris_debug(2, "Audiohook source %s is already set up to be inherited from channel %s\n", data, chan->name);
				return 0;
			} else {
				tris_debug(2, "Removing inheritability of audiohook %s from channel %s\n", data, chan->name);
				TRIS_LIST_REMOVE_CURRENT(list);
				tris_free(inheritable_audiohook);
				return 0;
			}
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	/* Step 4: There is no step 4 */

	/* Step 5: This means we are addressing an audiohook source which we have not encountered yet for the channel. Create a new inheritable
	 * audiohook structure if we're allowing inheritance, or just return if not
	 */

	if (allow) {
		return setup_inheritable_audiohook(inheritance_datastore, data);
	} else {
		tris_debug(1, "Audiohook %s is already set to not be inheritable on channel %s\n", data, chan->name);
		return 0;
	}
}

static struct tris_custom_function inheritance_function = {
	.name = "AUDIOHOOK_INHERIT",
	.write = func_inheritance_write,
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&inheritance_function);
}

static int load_module(void)
{
	if (tris_custom_function_register(&inheritance_function)) {
		return TRIS_MODULE_LOAD_DECLINE;
	} else {
		return TRIS_MODULE_LOAD_SUCCESS;
	}
}
TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Audiohook inheritance function");
