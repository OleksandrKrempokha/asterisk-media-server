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
 * \brief Header for providers of file and format handling routines.
 * Clients of these routines should include "trismedia/file.h" instead.
 */

#ifndef _TRISMEDIA_MOD_FORMAT_H
#define _TRISMEDIA_MOD_FORMAT_H

#include "trismedia/file.h"
#include "trismedia/frame.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! \brief
 * Each supported file format is described by the following structure.
 *
 * Not all are necessary, the support routine implement default
 * values for some of them.
 * A handler typically fills a structure initializing the desired
 * fields, and then calls tris_format_register() with the (readonly)
 * structure as an argument.
 */
struct tris_format {
	char name[80];		/*!< Name of format */
	char exts[80];		/*!< Extensions (separated by | if more than one) 
	    			this format can read.  First is assumed for writing (e.g. .mp3) */
	int format;		/*!< Format of frames it uses/provides (one only) */
	/*! 
	 * \brief Prepare an input stream for playback. 
	 * \return 0 on success, -1 on error.
	 * The FILE is already open (in s->f) so this function only needs to perform
	 * any applicable validity checks on the file. If none is required, the
	 * function can be omitted.
	 */
	int (*open)(struct tris_filestream *s);
	/*! 
	 * \brief Prepare a stream for output, and comment it appropriately if applicable.
	 * \return 0 on success, -1 on error. 
	 * Same as the open, the FILE is already open so the function just needs to 
	 * prepare any header and other fields, if any. 
	 * The function can be omitted if nothing is needed.
	 */
	int (*rewrite)(struct tris_filestream *s, const char *comment);
	/*! Write a frame to a channel */
	int (*write)(struct tris_filestream *, struct tris_frame *);
	/*! seek num samples into file, whence - like a normal seek but with offset in samples */
	int (*seek)(struct tris_filestream *, off_t, int);
	int (*trunc)(struct tris_filestream *fs);	/*!< trunc file to current position */
	off_t (*tell)(struct tris_filestream *fs);	/*!< tell current position */
	/*! Read the next frame from the filestream (if available) and report
	 * when to get next frame (in samples)
	 */
	struct tris_frame * (*read)(struct tris_filestream *, int *whennext);
	/*! Do any closing actions, if any. The descriptor and structure are closed
	 * and destroyed by the generic routines, so they must not be done here. */
	void (*close)(struct tris_filestream *);
	char * (*getcomment)(struct tris_filestream *);		/*!< Retrieve file comment */

	TRIS_LIST_ENTRY(tris_format) list;			/*!< Link */

	/*!
	 * If the handler needs a buffer (for read, typically)
	 * and/or a private descriptor, put here the
	 * required size (in bytes) and the support routine will allocate them
	 * for you, pointed by s->buf and s->private, respectively.
	 * When allocating a buffer, remember to leave TRIS_FRIENDLY_OFFSET
	 * spare bytes at the bginning.
	 */
	int buf_size;			/*!< size of frame buffer, if any, aligned to 8 bytes. */
	int desc_size;			/*!< size of private descriptor, if any */

	struct tris_module *module;
};

/*! \brief
 * This structure is allocated by file.c in one chunk,
 * together with buf_size and desc_size bytes of memory
 * to be used for private purposes (e.g. buffers etc.)
 */
struct tris_filestream {
	/*! Everybody reserves a block of TRIS_RESERVED_POINTERS pointers for us */
	struct tris_format *fmt;	/* need to write to the lock and usecnt */
	int flags;
	mode_t mode;
	char *filename;
	char *realfilename;
	/*! Video file stream */
	struct tris_filestream *vfs;
	/*! Transparently translate from another format -- just once */
	struct tris_trans_pvt *trans;
	struct tris_tranlator_pvt *tr;
	int lastwriteformat;
	int lasttimeout;
	struct tris_channel *owner;
	FILE *f;
	struct tris_frame fr;	/*!< frame produced by read, typically */
	char *buf;		/*!< buffer pointed to by tris_frame; */
	void *_private;	/*!< pointer to private buffer */
	const char *orig_chan_name;
	char *write_buffer;
};

/*! 
 * \brief Register a new file format capability.
 * Adds a format to Trismedia's format abilities.
 * \retval 0 on success
 * \retval -1 on failure
 */
int __tris_format_register(const struct tris_format *f, struct tris_module *mod);
#define tris_format_register(f) __tris_format_register(f, tris_module_info->self)

/*! 
 * \brief Unregisters a file format 
 * \param name the name of the format you wish to unregister
 * Unregisters a format based on the name of the format.
 * \retval 0 on success
 * \retval -1 on failure to unregister
 */
int tris_format_unregister(const char *name);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_MOD_FORMAT_H */
