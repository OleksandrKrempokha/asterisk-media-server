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
 * \brief I/O Management (derived from Cheops-NG)
 */

#ifndef _TRISMEDIA_IO_H
#define _TRISMEDIA_IO_H

#include "trismedia/poll-compat.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! Input ready */
#define TRIS_IO_IN 	POLLIN
/*! Output ready */
#define TRIS_IO_OUT 	POLLOUT
/*! Priority input ready */
#define TRIS_IO_PRI	POLLPRI

/* Implicitly polled for */
/*! Error condition (errno or getsockopt) */
#define TRIS_IO_ERR	POLLERR
/*! Hangup */
#define TRIS_IO_HUP	POLLHUP
/*! Invalid fd */
#define TRIS_IO_NVAL	POLLNVAL

/*! \brief
 * An Trismedia IO callback takes its id, a file descriptor, list of events, and
 * callback data as arguments and returns 0 if it should not be
 * run again, or non-zero if it should be run again.
 */

struct io_context;

/*! 
 * \brief Creates a context 
 * Create a context for I/O operations
 * Basically mallocs an IO structure and sets up some default values.
 * \return an allocated io_context structure
 */
struct io_context *io_context_create(void);

/*! 
 * \brief Destroys a context 
 * \param ioc structure to destroy
 * Destroy a context for I/O operations
 * Frees all memory associated with the given io_context structure along with the structure itself
 */
void io_context_destroy(struct io_context *ioc);

typedef int (*tris_io_cb)(int *id, int fd, short events, void *cbdata);
#define TRIS_IO_CB(a) ((tris_io_cb)(a))

/*! 
 * \brief Adds an IO context 
 * \param ioc which context to use
 * \param fd which fd to monitor
 * \param callback callback function to run
 * \param events event mask of events to wait for
 * \param data data to pass to the callback
 * Watch for any of revents activites on fd, calling callback with data as
 * callback data.  
 * \retval a pointer to ID of the IO event
 * \retval NULL on failure
 */
int *tris_io_add(struct io_context *ioc, int fd, tris_io_cb callback, short events, void *data);

/*! 
 * \brief Changes an IO handler 
 * \param ioc which context to use
 * \param id
 * \param fd the fd you wish it to contain now
 * \param callback new callback function
 * \param events event mask to wait for
 * \param data data to pass to the callback function
 * Change an I/O handler, updating fd if > -1, callback if non-null, 
 * and revents if >-1, and data if non-null.
 * \retval a pointer to the ID of the IO event
 * \retval NULL on failure
 */
int *tris_io_change(struct io_context *ioc, int *id, int fd, tris_io_cb callback, short events, void *data);

/*! 
 * \brief Removes an IO context 
 * \param ioc which io_context to remove it from
 * \param id which ID to remove
 * Remove an I/O id from consideration  
 * \retval 0 on success
 * \retval -1 on failure
 */
int tris_io_remove(struct io_context *ioc, int *id);

/*! 
 * \brief Waits for IO 
 * \param ioc which context to act upon
 * \param howlong how many milliseconds to wait
 * Wait for I/O to happen, returning after
 * howlong milliseconds, and after processing
 * any necessary I/O.  
 * \return he number of I/O events which took place.
 */
int tris_io_wait(struct io_context *ioc, int howlong);

/*! 
 * \brief Dumps the IO array.
 * Debugging: Dump everything in the I/O array
 */
void tris_io_dump(struct io_context *ioc);

/*! Set fd into non-echoing mode (if fd is a tty) */

int tris_hide_password(int fd);

/*! 
 * \brief Restores TTY mode.
 * Call with result from previous tris_hide_password
 */
int tris_restore_tty(int fd, int oldstatus);

int tris_get_termcols(int fd);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_IO_H */
