/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2008, Digium, Inc.
 *
 * Luigi Rizzo (TCP and TLS server code)
 * Brett Bryant <brettbryant@gmail.com> (updated for client support)
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
 * \brief Code to support TCP and TLS server/client
 *
 * \author Luigi Rizzo
 * \author Brett Bryant <brettbryant@gmail.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 246982 $")

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <sys/signal.h>

#include "trismedia/compat.h"
#include "trismedia/tcptls.h"
#include "trismedia/http.h"
#include "trismedia/utils.h"
#include "trismedia/strings.h"
#include "trismedia/options.h"
#include "trismedia/manager.h"
#include "trismedia/astobj2.h"

/*! \brief
 * replacement read/write functions for SSL support.
 * We use wrappers rather than SSL_read/SSL_write directly so
 * we can put in some debugging.
 */

#ifdef DO_SSL
static HOOK_T ssl_read(void *cookie, char *buf, LEN_T len)
{
	int i = SSL_read(cookie, buf, len-1);
#if 0
	if (i >= 0)
		buf[i] = '\0';
	tris_verb(0, "ssl read size %d returns %d <%s>\n", (int)len, i, buf);
#endif
	return i;
}

static HOOK_T ssl_write(void *cookie, const char *buf, LEN_T len)
{
#if 0
	char *s = alloca(len+1);
	strncpy(s, buf, len);
	s[len] = '\0';
	tris_verb(0, "ssl write size %d <%s>\n", (int)len, s);
#endif
	return SSL_write(cookie, buf, len);
}

static int ssl_close(void *cookie)
{
	close(SSL_get_fd(cookie));
	SSL_shutdown(cookie);
	SSL_free(cookie);
	return 0;
}
#endif	/* DO_SSL */

HOOK_T tris_tcptls_server_read(struct tris_tcptls_session_instance *tcptls_session, void *buf, size_t count)
{
	if (tcptls_session->fd == -1) {
		tris_log(LOG_ERROR, "server_read called with an fd of -1\n");
		errno = EIO;
		return -1;
	}

#ifdef DO_SSL
	if (tcptls_session->ssl)
		return ssl_read(tcptls_session->ssl, buf, count);
#endif
	return read(tcptls_session->fd, buf, count);
}

HOOK_T tris_tcptls_server_write(struct tris_tcptls_session_instance *tcptls_session, const void *buf, size_t count)
{
	if (tcptls_session->fd == -1) {
		tris_log(LOG_ERROR, "server_write called with an fd of -1\n");
		errno = EIO;
		return -1;
	}

#ifdef DO_SSL
	if (tcptls_session->ssl)
		return ssl_write(tcptls_session->ssl, buf, count);
#endif
	return write(tcptls_session->fd, buf, count);
}

static void session_instance_destructor(void *obj)
{
	struct tris_tcptls_session_instance *i = obj;
	tris_mutex_destroy(&i->lock);
}

/*! \brief
* creates a FILE * from the fd passed by the accept thread.
* This operation is potentially expensive (certificate verification),
* so we do it in the child thread context.
*
* \note must decrement ref count before returning NULL on error
*/
static void *handle_tcptls_connection(void *data)
{
	struct tris_tcptls_session_instance *tcptls_session = data;
#ifdef DO_SSL
	int (*ssl_setup)(SSL *) = (tcptls_session->client) ? SSL_connect : SSL_accept;
	int ret;
	char err[256];
#endif

	/*
	* open a FILE * as appropriate.
	*/
	if (!tcptls_session->parent->tls_cfg) {
		tcptls_session->f = fdopen(tcptls_session->fd, "w+");
		if (tcptls_session->f) {
			setvbuf(tcptls_session->f, NULL, _IONBF, 0);
		}
	}
#ifdef DO_SSL
	else if ( (tcptls_session->ssl = SSL_new(tcptls_session->parent->tls_cfg->ssl_ctx)) ) {
		SSL_set_fd(tcptls_session->ssl, tcptls_session->fd);
		if ((ret = ssl_setup(tcptls_session->ssl)) <= 0) {
			tris_verb(2, "Problem setting up ssl connection: %s\n", ERR_error_string(ERR_get_error(), err));
		} else {
#if defined(HAVE_FUNOPEN)	/* the BSD interface */
			tcptls_session->f = funopen(tcptls_session->ssl, ssl_read, ssl_write, NULL, ssl_close);

#elif defined(HAVE_FOPENCOOKIE)	/* the glibc/linux interface */
			static const cookie_io_functions_t cookie_funcs = {
				ssl_read, ssl_write, NULL, ssl_close
			};
			tcptls_session->f = fopencookie(tcptls_session->ssl, "w+", cookie_funcs);
#else
			/* could add other methods here */
			tris_debug(2, "no tcptls_session->f methods attempted!");
#endif
			if ((tcptls_session->client && !tris_test_flag(&tcptls_session->parent->tls_cfg->flags, TRIS_SSL_DONT_VERIFY_SERVER))
				|| (!tcptls_session->client && tris_test_flag(&tcptls_session->parent->tls_cfg->flags, TRIS_SSL_VERIFY_CLIENT))) {
				X509 *peer;
				long res;
				peer = SSL_get_peer_certificate(tcptls_session->ssl);
				if (!peer)
					tris_log(LOG_WARNING, "No peer SSL certificate\n");
				res = SSL_get_verify_result(tcptls_session->ssl);
				if (res != X509_V_OK)
					tris_log(LOG_ERROR, "Certificate did not verify: %s\n", X509_verify_cert_error_string(res));
				if (!tris_test_flag(&tcptls_session->parent->tls_cfg->flags, TRIS_SSL_IGNORE_COMMON_NAME)) {
					ASN1_STRING *str;
					unsigned char *str2;
					X509_NAME *name = X509_get_subject_name(peer);
					int pos = -1;
					int found = 0;
				
					for (;;) {
						/* Walk the certificate to check all available "Common Name" */
						/* XXX Probably should do a gethostbyname on the hostname and compare that as well */
						pos = X509_NAME_get_index_by_NID(name, NID_commonName, pos);
						if (pos < 0)
							break;
						str = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, pos));
						ASN1_STRING_to_UTF8(&str2, str);
						if (str2) {
							if (!strcasecmp(tcptls_session->parent->hostname, (char *) str2))
								found = 1;
							tris_debug(3, "SSL Common Name compare s1='%s' s2='%s'\n", tcptls_session->parent->hostname, str2);
							OPENSSL_free(str2);
						}
						if (found)
							break;
					}
					if (!found) {
						tris_log(LOG_ERROR, "Certificate common name did not match (%s)\n", tcptls_session->parent->hostname);
						if (peer)
							X509_free(peer);
						close(tcptls_session->fd);
						fclose(tcptls_session->f);
						ao2_ref(tcptls_session, -1);
						return NULL;
					}
				}
				if (peer)
					X509_free(peer);
			}
		}
		if (!tcptls_session->f)	/* no success opening descriptor stacking */
			SSL_free(tcptls_session->ssl);
   }
#endif /* DO_SSL */

	if (!tcptls_session->f) {
		close(tcptls_session->fd);
		tris_log(LOG_WARNING, "FILE * open failed!\n");
#ifndef DO_SSL
		if (tcptls_session->parent->tls_cfg) {
			tris_log(LOG_WARNING, "Attempted a TLS connection without OpenSSL support.  This will not work!\n");
		}
#endif
		ao2_ref(tcptls_session, -1);
		return NULL;
	}

	if (tcptls_session && tcptls_session->parent->worker_fn)
		return tcptls_session->parent->worker_fn(tcptls_session);
	else
		return tcptls_session;
}

void *tris_tcptls_server_root(void *data)
{
	struct tris_tcptls_session_args *desc = data;
	int fd;
	struct sockaddr_in sin;
	socklen_t sinlen;
	struct tris_tcptls_session_instance *tcptls_session;
	pthread_t launched;
	
	for (;;) {
		int i, flags;

		if (desc->periodic_fn)
			desc->periodic_fn(desc);
		i = tris_wait_for_input(desc->accept_fd, desc->poll_timeout);
		if (i <= 0)
			continue;
		sinlen = sizeof(sin);
		fd = accept(desc->accept_fd, (struct sockaddr *) &sin, &sinlen);
		if (fd < 0) {
			if ((errno != EAGAIN) && (errno != EINTR))
				tris_log(LOG_WARNING, "Accept failed: %s\n", strerror(errno));
			continue;
		}
		tcptls_session = ao2_alloc(sizeof(*tcptls_session), session_instance_destructor);
		if (!tcptls_session) {
			tris_log(LOG_WARNING, "No memory for new session: %s\n", strerror(errno));
			close(fd);
			continue;
		}

		tris_mutex_init(&tcptls_session->lock);

		flags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
		tcptls_session->fd = fd;
		tcptls_session->parent = desc;
		memcpy(&tcptls_session->remote_address, &sin, sizeof(tcptls_session->remote_address));

		tcptls_session->client = 0;
			
		/* This thread is now the only place that controls the single ref to tcptls_session */
		if (tris_pthread_create_detached_background(&launched, NULL, handle_tcptls_connection, tcptls_session)) {
			tris_log(LOG_WARNING, "Unable to launch helper thread: %s\n", strerror(errno));
			close(tcptls_session->fd);
			ao2_ref(tcptls_session, -1);
		}
	}
	return NULL;
}

static int __ssl_setup(struct tris_tls_config *cfg, int client)
{
#ifndef DO_SSL
	cfg->enabled = 0;
	return 0;
#else
	if (!cfg->enabled)
		return 0;

	SSL_load_error_strings();
	SSLeay_add_ssl_algorithms();

	if (!(cfg->ssl_ctx = SSL_CTX_new( client ? SSLv23_client_method() : SSLv23_server_method() ))) {
		tris_debug(1, "Sorry, SSL_CTX_new call returned null...\n");
		cfg->enabled = 0;
		return 0;
	}
	if (!tris_strlen_zero(cfg->certfile)) {
		if (SSL_CTX_use_certificate_file(cfg->ssl_ctx, cfg->certfile, SSL_FILETYPE_PEM) == 0 ||
		    SSL_CTX_use_PrivateKey_file(cfg->ssl_ctx, cfg->certfile, SSL_FILETYPE_PEM) == 0 ||
		    SSL_CTX_check_private_key(cfg->ssl_ctx) == 0 ) {
			if (!client) {
				/* Clients don't need a certificate, but if its setup we can use it */
				tris_verb(0, "SSL cert error <%s>", cfg->certfile);
				sleep(2);
				cfg->enabled = 0;
				return 0;
			}
		}
	}
	if (!tris_strlen_zero(cfg->cipher)) {
		if (SSL_CTX_set_cipher_list(cfg->ssl_ctx, cfg->cipher) == 0 ) {
			if (!client) {
				tris_verb(0, "SSL cipher error <%s>", cfg->cipher);
				sleep(2);
				cfg->enabled = 0;
				return 0;
			}
		}
	}
	if (!tris_strlen_zero(cfg->cafile) || !tris_strlen_zero(cfg->capath)) {
		if (SSL_CTX_load_verify_locations(cfg->ssl_ctx, S_OR(cfg->cafile, NULL), S_OR(cfg->capath,NULL)) == 0)
			tris_verb(0, "SSL CA file(%s)/path(%s) error\n", cfg->cafile, cfg->capath);
	}

	tris_verb(0, "SSL certificate ok\n");
	return 1;
#endif
}

int tris_ssl_setup(struct tris_tls_config *cfg)
{
	return __ssl_setup(cfg, 0);
}

struct tris_tcptls_session_instance *tris_tcptls_client_start(struct tris_tcptls_session_instance *tcptls_session)
{
	struct tris_tcptls_session_args *desc;
	int flags;

	if (!(desc = tcptls_session->parent)) {
		goto client_start_error;
	}

	if (connect(desc->accept_fd, (const struct sockaddr *) &desc->remote_address, sizeof(desc->remote_address))) {
		tris_log(LOG_ERROR, "Unable to connect %s to %s:%d: %s\n",
			desc->name,
			tris_inet_ntoa(desc->remote_address.sin_addr), ntohs(desc->remote_address.sin_port),
			strerror(errno));
		goto client_start_error;
	}

	flags = fcntl(desc->accept_fd, F_GETFL);
	fcntl(desc->accept_fd, F_SETFL, flags & ~O_NONBLOCK);

	if (desc->tls_cfg) {
		desc->tls_cfg->enabled = 1;
		__ssl_setup(desc->tls_cfg, 1);
	}

	return handle_tcptls_connection(tcptls_session);

client_start_error:
	close(desc->accept_fd);
	desc->accept_fd = -1;
	if (tcptls_session) {
		ao2_ref(tcptls_session, -1);
	}
	return NULL;

}

struct tris_tcptls_session_instance *tris_tcptls_client_create(struct tris_tcptls_session_args *desc)
{
	int x = 1;
	struct tris_tcptls_session_instance *tcptls_session = NULL;

	/* Do nothing if nothing has changed */
	if (!memcmp(&desc->old_address, &desc->remote_address, sizeof(desc->old_address))) {
		tris_debug(1, "Nothing changed in %s\n", desc->name);
		return NULL;
	}

	desc->old_address = desc->remote_address;

	if (desc->accept_fd != -1)
		close(desc->accept_fd);

	desc->accept_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (desc->accept_fd < 0) {
		tris_log(LOG_WARNING, "Unable to allocate socket for %s: %s\n",
			desc->name, strerror(errno));
		return NULL;
	}

	/* if a local address was specified, bind to it so the connection will
	   originate from the desired address */
	if (desc->local_address.sin_family != 0) {
		setsockopt(desc->accept_fd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
		if (bind(desc->accept_fd, (struct sockaddr *) &desc->local_address, sizeof(desc->local_address))) {
			tris_log(LOG_ERROR, "Unable to bind %s to %s:%d: %s\n",
			desc->name,
				tris_inet_ntoa(desc->local_address.sin_addr), ntohs(desc->local_address.sin_port),
				strerror(errno));
			goto error;
		}
	}

	if (!(tcptls_session = ao2_alloc(sizeof(*tcptls_session), session_instance_destructor)))
		goto error;

	tris_mutex_init(&tcptls_session->lock);
	tcptls_session->client = 1;
	tcptls_session->fd = desc->accept_fd;
	tcptls_session->parent = desc;
	tcptls_session->parent->worker_fn = NULL;
	memcpy(&tcptls_session->remote_address, &desc->remote_address, sizeof(tcptls_session->remote_address));

	return tcptls_session;

error:
	close(desc->accept_fd);
	desc->accept_fd = -1;
	if (tcptls_session)
		ao2_ref(tcptls_session, -1);
	return NULL;
}

void tris_tcptls_server_start(struct tris_tcptls_session_args *desc)
{
	int flags;
	int x = 1;
	
	/* Do nothing if nothing has changed */
	if (!memcmp(&desc->old_address, &desc->local_address, sizeof(desc->old_address))) {
		tris_debug(1, "Nothing changed in %s\n", desc->name);
		return;
	}
	
	desc->old_address = desc->local_address;
	
	/* Shutdown a running server if there is one */
	if (desc->master != TRIS_PTHREADT_NULL) {
		pthread_cancel(desc->master);
		pthread_kill(desc->master, SIGURG);
		pthread_join(desc->master, NULL);
	}
	
	if (desc->accept_fd != -1)
		close(desc->accept_fd);

	/* If there's no new server, stop here */
	if (desc->local_address.sin_family == 0) {
		tris_debug(2, "Server disabled:  %s\n", desc->name);
		return;
	}

	desc->accept_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (desc->accept_fd < 0) {
		tris_log(LOG_ERROR, "Unable to allocate socket for %s: %s\n", desc->name, strerror(errno));
		return;
	}
	
	setsockopt(desc->accept_fd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
	if (bind(desc->accept_fd, (struct sockaddr *) &desc->local_address, sizeof(desc->local_address))) {
		tris_log(LOG_ERROR, "Unable to bind %s to %s:%d: %s\n",
			desc->name,
			tris_inet_ntoa(desc->local_address.sin_addr), ntohs(desc->local_address.sin_port),
			strerror(errno));
		goto error;
	}
	if (listen(desc->accept_fd, 10)) {
		tris_log(LOG_ERROR, "Unable to listen for %s!\n", desc->name);
		goto error;
	}
	flags = fcntl(desc->accept_fd, F_GETFL);
	fcntl(desc->accept_fd, F_SETFL, flags | O_NONBLOCK);
	if (tris_pthread_create_background(&desc->master, NULL, desc->accept_fn, desc)) {
		tris_log(LOG_ERROR, "Unable to launch thread for %s on %s:%d: %s\n",
			desc->name,
			tris_inet_ntoa(desc->local_address.sin_addr), ntohs(desc->local_address.sin_port),
			strerror(errno));
		goto error;
	}
	return;

error:
	close(desc->accept_fd);
	desc->accept_fd = -1;
}

void tris_tcptls_server_stop(struct tris_tcptls_session_args *desc)
{
	if (desc->master != TRIS_PTHREADT_NULL) {
		pthread_cancel(desc->master);
		pthread_kill(desc->master, SIGURG);
		pthread_join(desc->master, NULL);
	}
	if (desc->accept_fd != -1)
		close(desc->accept_fd);
	desc->accept_fd = -1;
	tris_debug(2, "Stopped server :: %s\n", desc->name);
}
