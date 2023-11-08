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
 *
 * \brief Stubs for res_crypto routines
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 148200 $")

#include "trismedia/crypto.h"
#include "trismedia/logger.h"

static struct tris_key *stub_tris_key_get(const char *kname, int ktype)
{
	tris_log(LOG_NOTICE, "Crypto support not loaded!\n");
	return NULL;
}

#ifdef SKREP
#define build_stub(func_name,...) \
static int stub_ ## func_name(__VA_ARGS__) \
{ \
	tris_log(LOG_NOTICE, "Crypto support not loaded!\n"); \
	return -1; \
} \
\
int (*func_name)(__VA_ARGS__) = \
	stub_ ## func_name;
#endif
#define build_stub(func_name,...) \
static int stub_##func_name(__VA_ARGS__) \
{ \
	tris_log(LOG_NOTICE, "Crypto support not loaded!\n"); \
	return -1; \
} \
\
int (*func_name)(__VA_ARGS__) = \
	stub_##func_name;

struct tris_key *(*tris_key_get)(const char *key, int type) =
stub_tris_key_get;

build_stub(tris_check_signature, struct tris_key *key, const char *msg, const char *sig);
build_stub(tris_check_signature_bin, struct tris_key *key, const char *msg, int msglen, const unsigned char *sig);
build_stub(tris_sign, struct tris_key *key, char *msg, char *sig);
build_stub(tris_sign_bin, struct tris_key *key, const char *msg, int msglen, unsigned char *sig);
build_stub(tris_encrypt_bin, unsigned char *dst, const unsigned char *src, int srclen, struct tris_key *key);
build_stub(tris_decrypt_bin, unsigned char *dst, const unsigned char *src, int srclen, struct tris_key *key);
