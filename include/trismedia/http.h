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

#ifndef _TRISMEDIA_HTTP_H
#define _TRISMEDIA_HTTP_H

#include "trismedia/config.h"
#include "trismedia/tcptls.h"
#include "trismedia/linkedlists.h"

/*!
 * \file http.h
 * \brief Support for Private Trismedia HTTP Servers.
 * \note Note: The Trismedia HTTP servers are extremely simple and minimal and
 *      only support the "GET" method.
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \note In order to have TLS/SSL support, we need the openssl libraries.
 * Still we can decide whether or not to use them by commenting
 * in or out the DO_SSL macro.
 * TLS/SSL support is basically implemented by reading from a config file
 * (currently http.conf) the names of the certificate and cipher to use,
 * and then run ssl_setup() to create an appropriate SSL_CTX (ssl_ctx)
 * If we support multiple domains, presumably we need to read multiple
 * certificates.
 * When we are requested to open a TLS socket, we run make_file_from_fd()
 * on the socket, to do the necessary setup. At the moment the context's name
 * is hardwired in the function, but we can certainly make it into an extra
 * parameter to the function.
 * We declare most of ssl support variables unconditionally,
 * because their number is small and this simplifies the code.
 *
 * \note: the ssl-support variables (ssl_ctx, do_ssl, certfile, cipher)
 * and their setup should be moved to a more central place, e.g. trismedia.conf
 * and the source files that processes it. Similarly, ssl_setup() should
 * be run earlier in the startup process so modules have it available.
 */


/*! \brief HTTP Callbacks take the socket

   \note The method and the path as arguments and should
   return the content, allocated with malloc().  Status should be changed to reflect
   the status of the request if it isn't 200 and title may be set to a malloc()'d string
   to an appropriate title for non-200 responses.  Content length may also be specified.
\verbatim
   The return value may include additional headers at the front and MUST include a blank
   line with \r\n to provide separation between user headers and content (even if no
   content is specified)
\endverbatim
*/

enum tris_http_method {
	TRIS_HTTP_GET = 0,
	TRIS_HTTP_POST,
};
struct tris_http_uri;

typedef struct tris_str *(*tris_http_callback)(struct tris_tcptls_session_instance *ser, const struct tris_http_uri *urih, const char *uri, enum tris_http_method method, struct tris_variable *params, struct tris_variable *headers, int *status, char **title, int *contentlength);

/*! \brief Definition of a URI handler */
struct tris_http_uri {
	TRIS_LIST_ENTRY(tris_http_uri) entry;
	const char *description;
	const char *uri;
	tris_http_callback callback;
	unsigned int has_subtree:1;
	/*! This handler serves static content */
	unsigned int static_content:1;
	/*! This handler accepts GET requests */
	unsigned int supports_get:1;
	/*! This handler accepts POST requests */
	unsigned int supports_post:1;
	/*! Structure is malloc'd */
	unsigned int mallocd:1;
	/*! Data structure is malloc'd */
	unsigned int dmallocd:1;
	/*! Data to bind to the uri if needed */
	void *data;
	/*! Key to be used for unlinking if multiple URIs registered */
	const char *key;
};

/*! \brief Register a URI handler */
int tris_http_uri_link(struct tris_http_uri *urihandler);

/*! \brief Unregister a URI handler */
void tris_http_uri_unlink(struct tris_http_uri *urihandler);

/*! \brief Unregister all handlers with matching key */
void tris_http_uri_unlink_all_with_key(const char *key);

/*! \brief Return an tris_str malloc()'d string containing an HTTP error message */
struct tris_str *tris_http_error(int status, const char *title, const char *extra_header, const char *text);

/*!
 * \brief Return the current prefix
 * \param buf[out] destination buffer for previous
 * \param len[in] length of prefix to copy
 * \since 1.6.1
 */
void tris_http_prefix(char *buf, int len);

#endif /* _TRISMEDIA_SRV_H */
