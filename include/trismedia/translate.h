/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Support for translation of data formats.
 * \ref translate.c
 */

#ifndef _TRISMEDIA_TRANSLATE_H
#define _TRISMEDIA_TRANSLATE_H

#define MAX_AUDIO_FORMAT 15 /* Do not include video here */
#define MAX_FORMAT 32	/* Do include video here */

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#if 1	/* need lots of stuff... */
#include "trismedia/frame.h"
#include "trismedia/plc.h"
#include "trismedia/linkedlists.h"
// XXX #include "trismedia/module.h"
#endif

struct tris_trans_pvt;	/* declared below */

/*! \brief
 * Descriptor of a translator. 
 *
 * Name, callbacks, and various options
 * related to run-time operation (size of buffers, auxiliary
 * descriptors, etc).
 *
 * A codec registers itself by filling the relevant fields
 * of a structure and passing it as an argument to
 * tris_register_translator(). The structure should not be
 * modified after a successful registration, and its address
 * must be used as an argument to tris_unregister_translator().
 *
 * As a minimum, a translator should supply name, srcfmt and dstfmt,
 * the required buf_size (in bytes) and buffer_samples (in samples),
 * and a few callbacks (framein, frameout, sample).
 * The outbuf is automatically prepended by TRIS_FRIENDLY_OFFSET
 * spare bytes so generic routines can place data in there.
 *
 * Note, the translator is not supposed to do any memory allocation
 * or deallocation, nor any locking, because all of this is done in
 * the generic code.
 *
 * Translators using generic plc (packet loss concealment) should
 * supply a non-zero plc_samples indicating the size (in samples)
 * of artificially generated frames and incoming data.
 * Generic plc is only available for dstfmt = SLINEAR
 */
struct tris_translator {
	const char name[80];		/*!< Name of translator */
	int srcfmt;			/*!< Source format (note: bit position,
					  converted to index during registration) */
	int dstfmt;			/*!< Destination format (note: bit position,
					  converted to index during registration) */

	int (*newpvt)(struct tris_trans_pvt *); /*!< initialize private data 
					associated with the translator */

	int (*framein)(struct tris_trans_pvt *pvt, struct tris_frame *in);
					/*!< Input frame callback. Store 
					     (and possibly convert) input frame. */

	struct tris_frame * (*frameout)(struct tris_trans_pvt *pvt);
					/*!< Output frame callback. Generate a frame 
					    with outbuf content. */

	void (*destroy)(struct tris_trans_pvt *pvt);
					/*!< cleanup private data, if needed 
						(often unnecessary). */

	struct tris_frame * (*sample)(void);	/*!< Generate an example frame */

	/*! \brief size of outbuf, in samples. Leave it 0 if you want the framein
	 * callback deal with the frame. Set it appropriately if you
	 * want the code to checks if the incoming frame fits the
	 * outbuf (this is e.g. required for plc).
	 */
	int buffer_samples;	/*< size of outbuf, in samples */

	/*! \brief size of outbuf, in bytes. Mandatory. The wrapper code will also
	 * allocate an TRIS_FRIENDLY_OFFSET space before.
	 */
	int buf_size;

	int desc_size;			/*!< size of private descriptor in pvt->pvt, if any */
	int plc_samples;		/*!< set to the plc block size if used, 0 otherwise */
	int useplc;			/*!< current status of plc, changed at runtime */
	int native_plc;			/*!< true if the translator can do native plc */

	struct tris_module *module;	/* opaque reference to the parent module */

	int cost;			/*!< Cost in milliseconds for encoding/decoding 1 second of sound */
	int active;			/*!< Whether this translator should be used or not */
	TRIS_LIST_ENTRY(tris_translator) list;	/*!< link field */
};

/*! \brief
 * Default structure for translators, with the basic fields and buffers,
 * all allocated as part of the same chunk of memory. The buffer is
 * preceded by \ref TRIS_FRIENDLY_OFFSET bytes in front of the user portion.
 * 'buf' points right after this space.
 *
 * *_framein() routines operate in two ways:
 * 1. some convert on the fly and place the data directly in outbuf;
 *    in this case 'samples' and 'datalen' contain the number of samples
 *    and number of bytes available in the buffer.
 *    In this case we can use a generic *_frameout() routine that simply
 *    takes whatever is there and places it into the output frame.
 * 2. others simply store the (unconverted) samples into a working
 *    buffer, and leave the conversion task to *_frameout().
 *    In this case, the intermediate buffer must be in the private
 *    descriptor, 'datalen' is left to 0, while 'samples' is still
 *    updated with the number of samples received.
 */
struct tris_trans_pvt {
	struct tris_translator *t;
	struct tris_frame f;         /*!< used in frameout */
	int samples;                /*!< samples available in outbuf */
	/*! \brief actual space used in outbuf */
	int datalen;
	void *pvt;                  /*!< more private data, if any */
	union {
		char *c;                /*!< the useful portion of the buffer */
		unsigned char *uc;      /*!< the useful portion of the buffer */
		int16_t *i16;
		uint8_t *ui8;
	} outbuf; 
	plc_state_t *plc;           /*!< optional plc pointer */
	struct tris_trans_pvt *next; /*!< next in translator chain */
	struct timeval nextin;
	struct timeval nextout;
	unsigned int destroy:1;
};

/*! \brief generic frameout function */
struct tris_frame *tris_trans_frameout(struct tris_trans_pvt *pvt,
        int datalen, int samples);

struct tris_trans_pvt;

/*!
 * \brief Register a translator
 * This registers a codec translator with trismedia
 * \param t populated tris_translator structure
 * \param module handle to the module that owns this translator
 * \return 0 on success, -1 on failure
 */
int __tris_register_translator(struct tris_translator *t, struct tris_module *module);

/*! \brief See \ref __tris_register_translator() */
#define tris_register_translator(t) __tris_register_translator(t, tris_module_info->self)

/*!
 * \brief Unregister a translator
 * Unregisters the given tranlator
 * \param t translator to unregister
 * \return 0 on success, -1 on failure
 */
int tris_unregister_translator(struct tris_translator *t);

/*!
 * \brief Activate a previously deactivated translator
 * \param t translator to activate
 * \return nothing
 *
 * Enables the specified translator for use.
 */
void tris_translator_activate(struct tris_translator *t);

/*!
 * \brief Deactivate a translator
 * \param t translator to deactivate
 * \return nothing
 *
 * Disables the specified translator from being used.
 */
void tris_translator_deactivate(struct tris_translator *t);

/*!
 * \brief Chooses the best translation path
 *
 * Given a list of sources, and a designed destination format, which should
 * I choose? 
 * \return Returns 0 on success, -1 if no path could be found.  
 * \note Modifies dests and srcs in place 
 */
int tris_translator_best_choice(int *dsts, int *srcs);

/*! 
 * \brief Builds a translator path
 * Build a path (possibly NULL) from source to dest 
 * \param dest destination format
 * \param source source format
 * \return tris_trans_pvt on success, NULL on failure
 * */
struct tris_trans_pvt *tris_translator_build_path(int dest, int source);

/*!
 * \brief Frees a translator path
 * Frees the given translator path structure
 * \param tr translator path to get rid of
 */
void tris_translator_free_path(struct tris_trans_pvt *tr);

/*!
 * \brief translates one or more frames
 * Apply an input frame into the translator and receive zero or one output frames.  Consume
 * determines whether the original frame should be freed
 * \param tr translator structure to use for translation
 * \param f frame to translate
 * \param consume Whether or not to free the original frame
 * \return an tris_frame of the new translation format on success, NULL on failure
 */
struct tris_frame *tris_translate(struct tris_trans_pvt *tr, struct tris_frame *f, int consume);

/*!
 * \brief Returns the number of steps required to convert from 'src' to 'dest'.
 * \param dest destination format
 * \param src source format
 * \return the number of translation steps required, or -1 if no path is available
 */
unsigned int tris_translate_path_steps(unsigned int dest, unsigned int src);

/*!
 * \brief Mask off unavailable formats from a format bitmask
 * \param dest possible destination formats
 * \param src source formats
 * \return the destination formats that are available in the source or translatable
 *
 * The result will include all formats from 'dest' that are either present
 * in 'src' or translatable from a format present in 'src'.
 *
 * \note Only a single audio format and a single video format can be
 * present in 'src', or the function will produce unexpected results.
 */
unsigned int tris_translate_available_formats(unsigned int dest, unsigned int src);

/*!
 * \brief Hint that a frame from a translator has been freed
 *
 * This is sort of a hack.  This function gets called when tris_frame_free() gets
 * called on a frame that has the TRIS_FRFLAG_FROM_TRANSLATOR flag set.  This is
 * because it is possible for a translation path to be destroyed while a frame
 * from a translator is still in use.  Specifically, this happens if a masquerade
 * happens after a call to tris_read() but before the frame is done being processed, 
 * since the frame processing is generally done without the channel lock held.
 *
 * \return nothing
 */
void tris_translate_frame_freed(struct tris_frame *fr);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_TRANSLATE_H */
