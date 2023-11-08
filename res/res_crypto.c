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
 *
 * \brief Provide Cryptographic Signature capability
 *
 * \author Mark Spencer <markster@digium.com> 
 *
 * \extref Uses the OpenSSL library, available at
 *	http://www.openssl.org/
 */

/*** MODULEINFO
	<depend>openssl</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 205148 $")

#include "trismedia/paths.h"	/* use tris_config_TRIS_KEY_DIR */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <dirent.h>

#include "trismedia/module.h"
#include "trismedia/crypto.h"
#include "trismedia/md5.h"
#include "trismedia/cli.h"
#include "trismedia/io.h"
#include "trismedia/lock.h"
#include "trismedia/utils.h"

/*
 * Trismedia uses RSA keys with SHA-1 message digests for its
 * digital signatures.  The choice of RSA is due to its higher
 * throughput on verification, and the choice of SHA-1 based
 * on the recently discovered collisions in MD5's compression 
 * algorithm and recommendations of avoiding MD5 in new schemes
 * from various industry experts.
 *
 * We use OpenSSL to provide our crypto routines, although we never
 * actually use full-up SSL
 *
 */

#define KEY_NEEDS_PASSCODE (1 << 16)

struct tris_key {
	/*! Name of entity */
	char name[80];
	/*! File name */
	char fn[256];
	/*! Key type (TRIS_KEY_PUB or TRIS_KEY_PRIV, along with flags from above) */
	int ktype;
	/*! RSA structure (if successfully loaded) */
	RSA *rsa;
	/*! Whether we should be deleted */
	int delme;
	/*! FD for input (or -1 if no input allowed, or -2 if we needed input) */
	int infd;
	/*! FD for output */
	int outfd;
	/*! Last MD5 Digest */
	unsigned char digest[16];
	TRIS_RWLIST_ENTRY(tris_key) list;
};

static TRIS_RWLIST_HEAD_STATIC(keys, tris_key);

/*!
 * \brief setting of priv key
 * \param buf
 * \param size
 * \param rwflag
 * \param userdata
 * \return length of string,-1 on failure
*/
static int pw_cb(char *buf, int size, int rwflag, void *userdata)
{
	struct tris_key *key = (struct tris_key *)userdata;
	char prompt[256];
	int res, tmp;

	if (key->infd < 0) {
		/* Note that we were at least called */
		key->infd = -2;
		return -1;
	}
	
	snprintf(prompt, sizeof(prompt), ">>>> passcode for %s key '%s': ",
		 key->ktype == TRIS_KEY_PRIVATE ? "PRIVATE" : "PUBLIC", key->name);
	if (write(key->outfd, prompt, strlen(prompt)) < 0) {
		tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
		key->infd = -2;
		return -1;
	}
	memset(buf, 0, sizeof(buf));
	tmp = tris_hide_password(key->infd);
	memset(buf, 0, size);
	res = read(key->infd, buf, size);
	tris_restore_tty(key->infd, tmp);
	if (buf[strlen(buf) -1] == '\n')
		buf[strlen(buf) - 1] = '\0';
	return strlen(buf);
}

/*!
 * \brief return the tris_key structure for name
 * \see tris_key_get
*/
static struct tris_key *__tris_key_get(const char *kname, int ktype)
{
	struct tris_key *key;

	TRIS_RWLIST_RDLOCK(&keys);
	TRIS_RWLIST_TRAVERSE(&keys, key, list) {
		if (!strcmp(kname, key->name) &&
		    (ktype == key->ktype))
			break;
	}
	TRIS_RWLIST_UNLOCK(&keys);

	return key;
}

/*!
 * \brief load RSA key from file
 * \param dir directory string
 * \param fname name of file
 * \param ifd incoming file descriptor
 * \param ofd outgoing file descriptor
 * \param not2
 * \retval key on success.
 * \retval NULL on failure.
*/
static struct tris_key *try_load_key(const char *dir, const char *fname, int ifd, int ofd, int *not2)
{
	int ktype = 0, found = 0;
	char *c = NULL, ffname[256];
	unsigned char digest[16];
	FILE *f;
	struct MD5Context md5;
	struct tris_key *key;
	static int notice = 0;

	/* Make sure its name is a public or private key */
	if ((c = strstr(fname, ".pub")) && !strcmp(c, ".pub"))
		ktype = TRIS_KEY_PUBLIC;
	else if ((c = strstr(fname, ".key")) && !strcmp(c, ".key"))
		ktype = TRIS_KEY_PRIVATE;
	else
		return NULL;

	/* Get actual filename */
	snprintf(ffname, sizeof(ffname), "%s/%s", dir, fname);

	/* Open file */
	if (!(f = fopen(ffname, "r"))) {
		tris_log(LOG_WARNING, "Unable to open key file %s: %s\n", ffname, strerror(errno));
		return NULL;
	}

	MD5Init(&md5);
	while(!feof(f)) {
		/* Calculate a "whatever" quality md5sum of the key */
		char buf[256] = "";
		if (!fgets(buf, sizeof(buf), f)) {
			continue;
		}
		if (!feof(f))
			MD5Update(&md5, (unsigned char *) buf, strlen(buf));
	}
	MD5Final(digest, &md5);

	/* Look for an existing key */
	TRIS_RWLIST_TRAVERSE(&keys, key, list) {
		if (!strcasecmp(key->fn, ffname))
			break;
	}

	if (key) {
		/* If the MD5 sum is the same, and it isn't awaiting a passcode 
		   then this is far enough */
		if (!memcmp(digest, key->digest, 16) &&
		    !(key->ktype & KEY_NEEDS_PASSCODE)) {
			fclose(f);
			key->delme = 0;
			return NULL;
		} else {
			/* Preserve keytype */
			ktype = key->ktype;
			/* Recycle the same structure */
			found++;
		}
	}

	/* Make fname just be the normal name now */
	*c = '\0';
	if (!key) {
		if (!(key = tris_calloc(1, sizeof(*key)))) {
			fclose(f);
			return NULL;
		}
	}
	/* First the filename */
	tris_copy_string(key->fn, ffname, sizeof(key->fn));
	/* Then the name */
	tris_copy_string(key->name, fname, sizeof(key->name));
	key->ktype = ktype;
	/* Yes, assume we're going to be deleted */
	key->delme = 1;
	/* Keep the key type */
	memcpy(key->digest, digest, 16);
	/* Can I/O takes the FD we're given */
	key->infd = ifd;
	key->outfd = ofd;
	/* Reset the file back to the beginning */
	rewind(f);
	/* Now load the key with the right method */
	if (ktype == TRIS_KEY_PUBLIC)
		key->rsa = PEM_read_RSA_PUBKEY(f, NULL, pw_cb, key);
	else
		key->rsa = PEM_read_RSAPrivateKey(f, NULL, pw_cb, key);
	fclose(f);
	if (key->rsa) {
		if (RSA_size(key->rsa) == 128) {
			/* Key loaded okay */
			key->ktype &= ~KEY_NEEDS_PASSCODE;
			tris_verb(3, "Loaded %s key '%s'\n", key->ktype == TRIS_KEY_PUBLIC ? "PUBLIC" : "PRIVATE", key->name);
			tris_debug(1, "Key '%s' loaded OK\n", key->name);
			key->delme = 0;
		} else
			tris_log(LOG_NOTICE, "Key '%s' is not expected size.\n", key->name);
	} else if (key->infd != -2) {
		tris_log(LOG_WARNING, "Key load %s '%s' failed\n",key->ktype == TRIS_KEY_PUBLIC ? "PUBLIC" : "PRIVATE", key->name);
		if (ofd > -1)
			ERR_print_errors_fp(stderr);
		else
			ERR_print_errors_fp(stderr);
	} else {
		tris_log(LOG_NOTICE, "Key '%s' needs passcode.\n", key->name);
		key->ktype |= KEY_NEEDS_PASSCODE;
		if (!notice) {
			if (!tris_opt_init_keys) 
				tris_log(LOG_NOTICE, "Add the '-i' flag to the trismedia command line if you want to automatically initialize passcodes at launch.\n");
			notice++;
		}
		/* Keep it anyway */
		key->delme = 0;
		/* Print final notice about "init keys" when done */
		*not2 = 1;
	}

	/* If this is a new key add it to the list */
	if (!found)
		TRIS_RWLIST_INSERT_TAIL(&keys, key, list);

	return key;
}

/*!
 * \brief signs outgoing message with public key
 * \see tris_sign_bin
*/
static int __tris_sign_bin(struct tris_key *key, const char *msg, int msglen, unsigned char *dsig)
{
	unsigned char digest[20];
	unsigned int siglen = 128;
	int res;

	if (key->ktype != TRIS_KEY_PRIVATE) {
		tris_log(LOG_WARNING, "Cannot sign with a public key\n");
		return -1;
	}

	/* Calculate digest of message */
	SHA1((unsigned char *)msg, msglen, digest);

	/* Verify signature */
	if (!(res = RSA_sign(NID_sha1, digest, sizeof(digest), dsig, &siglen, key->rsa))) {
		tris_log(LOG_WARNING, "RSA Signature (key %s) failed\n", key->name);
		return -1;
	}

	if (siglen != 128) {
		tris_log(LOG_WARNING, "Unexpected signature length %d, expecting %d\n", (int)siglen, (int)128);
		return -1;
	}

	return 0;
	
}

/*!
 * \brief decrypt a message
 * \see tris_decrypt_bin
*/
static int __tris_decrypt_bin(unsigned char *dst, const unsigned char *src, int srclen, struct tris_key *key)
{
	int res, pos = 0;

	if (key->ktype != TRIS_KEY_PRIVATE) {
		tris_log(LOG_WARNING, "Cannot decrypt with a public key\n");
		return -1;
	}

	if (srclen % 128) {
		tris_log(LOG_NOTICE, "Tried to decrypt something not a multiple of 128 bytes\n");
		return -1;
	}

	while(srclen) {
		/* Process chunks 128 bytes at a time */
		if ((res = RSA_private_decrypt(128, src, dst, key->rsa, RSA_PKCS1_OAEP_PADDING)) < 0)
			return -1;
		pos += res;
		src += 128;
		srclen -= 128;
		dst += res;
	}

	return pos;
}

/*!
 * \brief encrypt a message
 * \see tris_encrypt_bin
*/
static int __tris_encrypt_bin(unsigned char *dst, const unsigned char *src, int srclen, struct tris_key *key)
{
	int res, bytes, pos = 0;

	if (key->ktype != TRIS_KEY_PUBLIC) {
		tris_log(LOG_WARNING, "Cannot encrypt with a private key\n");
		return -1;
	}
	
	while(srclen) {
		bytes = srclen;
		if (bytes > 128 - 41)
			bytes = 128 - 41;
		/* Process chunks 128-41 bytes at a time */
		if ((res = RSA_public_encrypt(bytes, src, dst, key->rsa, RSA_PKCS1_OAEP_PADDING)) != 128) {
			tris_log(LOG_NOTICE, "How odd, encrypted size is %d\n", res);
			return -1;
		}
		src += bytes;
		srclen -= bytes;
		pos += res;
		dst += res;
	}
	return pos;
}

/*!
 * \brief wrapper for __tris_sign_bin then base64 encode it
 * \see tris_sign
*/
static int __tris_sign(struct tris_key *key, char *msg, char *sig)
{
	unsigned char dsig[128];
	int siglen = sizeof(dsig), res;

	if (!(res = tris_sign_bin(key, msg, strlen(msg), dsig)))
		/* Success -- encode (256 bytes max as documented) */
		tris_base64encode(sig, dsig, siglen, 256);

	return res;
}

/*!
 * \brief check signature of a message
 * \see tris_check_signature_bin
*/
static int __tris_check_signature_bin(struct tris_key *key, const char *msg, int msglen, const unsigned char *dsig)
{
	unsigned char digest[20];
	int res;

	if (key->ktype != TRIS_KEY_PUBLIC) {
		/* Okay, so of course you really *can* but for our purposes
		   we're going to say you can't */
		tris_log(LOG_WARNING, "Cannot check message signature with a private key\n");
		return -1;
	}

	/* Calculate digest of message */
	SHA1((unsigned char *)msg, msglen, digest);

	/* Verify signature */
	if (!(res = RSA_verify(NID_sha1, digest, sizeof(digest), (unsigned char *)dsig, 128, key->rsa))) {
		tris_debug(1, "Key failed verification: %s\n", key->name);
		return -1;
	}

	/* Pass */
	return 0;
}

/*!
 * \brief base64 decode then sent to __tris_check_signature_bin
 * \see tris_check_signature
*/
static int __tris_check_signature(struct tris_key *key, const char *msg, const char *sig)
{
	unsigned char dsig[128];
	int res;

	/* Decode signature */
	if ((res = tris_base64decode(dsig, sig, sizeof(dsig))) != sizeof(dsig)) {
		tris_log(LOG_WARNING, "Signature improper length (expect %d, got %d)\n", (int)sizeof(dsig), (int)res);
		return -1;
	}

	res = tris_check_signature_bin(key, msg, strlen(msg), dsig);

	return res;
}

/*!
 * \brief refresh RSA keys from file
 * \param ifd file descriptor
 * \param ofd file descriptor
 * \return void
*/
static void crypto_load(int ifd, int ofd)
{
	struct tris_key *key;
	DIR *dir = NULL;
	struct dirent *ent;
	int note = 0;

	TRIS_RWLIST_WRLOCK(&keys);

	/* Mark all keys for deletion */
	TRIS_RWLIST_TRAVERSE(&keys, key, list) {
		key->delme = 1;
	}

	/* Load new keys */
	if ((dir = opendir(tris_config_TRIS_KEY_DIR))) {
		while((ent = readdir(dir))) {
			try_load_key(tris_config_TRIS_KEY_DIR, ent->d_name, ifd, ofd, &note);
		}
		closedir(dir);
	} else
		tris_log(LOG_WARNING, "Unable to open key directory '%s'\n", tris_config_TRIS_KEY_DIR);

	if (note)
		tris_log(LOG_NOTICE, "Please run the command 'init keys' to enter the passcodes for the keys\n");

	/* Delete any keys that are no longer present */
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&keys, key, list) {
		if (key->delme) {
			tris_debug(1, "Deleting key %s type %d\n", key->name, key->ktype);
			TRIS_RWLIST_REMOVE_CURRENT(list);
			if (key->rsa)
				RSA_free(key->rsa);
			tris_free(key);
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;

	TRIS_RWLIST_UNLOCK(&keys);
}

static void md52sum(char *sum, unsigned char *md5)
{
	int x;
	for (x = 0; x < 16; x++) 
		sum += sprintf(sum, "%02x", *(md5++));
}

/*! 
 * \brief show the list of RSA keys 
 * \param e CLI command
 * \param cmd
 * \param a list of CLI arguments
 * \return CLI_SUCCESS
*/
static char *handle_cli_keys_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMAT "%-18s %-8s %-16s %-33s\n"

	struct tris_key *key;
	char sum[16 * 2 + 1];
	int count_keys = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "keys show";
		e->usage =
			"Usage: keys show\n"
			"       Displays information about RSA keys known by Trismedia\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	tris_cli(a->fd, FORMAT, "Key Name", "Type", "Status", "Sum");
	tris_cli(a->fd, FORMAT, "------------------", "--------", "----------------", "--------------------------------");

	TRIS_RWLIST_RDLOCK(&keys);
	TRIS_RWLIST_TRAVERSE(&keys, key, list) {
		md52sum(sum, key->digest);
		tris_cli(a->fd, FORMAT, key->name, 
			(key->ktype & 0xf) == TRIS_KEY_PUBLIC ? "PUBLIC" : "PRIVATE",
			key->ktype & KEY_NEEDS_PASSCODE ? "[Needs Passcode]" : "[Loaded]", sum);
		count_keys++;
	}
	TRIS_RWLIST_UNLOCK(&keys);

	tris_cli(a->fd, "\n%d known RSA keys.\n", count_keys);

	return CLI_SUCCESS;

#undef FORMAT
}

/*! 
 * \brief initialize all RSA keys  
 * \param e CLI command
 * \param cmd 
 * \param a list of CLI arguments
 * \return CLI_SUCCESS
*/
static char *handle_cli_keys_init(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_key *key;
	int ign;
	char *kn, tmp[256] = "";

	switch (cmd) {
	case CLI_INIT:
		e->command = "keys init";
		e->usage =
			"Usage: keys init\n"
			"       Initializes private keys (by reading in pass code from\n"
			"       the user)\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 2)
		return CLI_SHOWUSAGE;

	TRIS_RWLIST_WRLOCK(&keys);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&keys, key, list) {
		/* Reload keys that need pass codes now */
		if (key->ktype & KEY_NEEDS_PASSCODE) {
			kn = key->fn + strlen(tris_config_TRIS_KEY_DIR) + 1;
			tris_copy_string(tmp, kn, sizeof(tmp));
			try_load_key(tris_config_TRIS_KEY_DIR, tmp, a->fd, a->fd, &ign);
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END
	TRIS_RWLIST_UNLOCK(&keys);

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_crypto[] = {
	TRIS_CLI_DEFINE(handle_cli_keys_show, "Displays RSA key information"),
	TRIS_CLI_DEFINE(handle_cli_keys_init, "Initialize RSA key passcodes")
};

/*! \brief initialise the res_crypto module */
static int crypto_init(void)
{
	tris_cli_register_multiple(cli_crypto, ARRAY_LEN(cli_crypto));

	/* Install ourselves into stubs */
	tris_key_get = __tris_key_get;
	tris_check_signature = __tris_check_signature;
	tris_check_signature_bin = __tris_check_signature_bin;
	tris_sign = __tris_sign;
	tris_sign_bin = __tris_sign_bin;
	tris_encrypt_bin = __tris_encrypt_bin;
	tris_decrypt_bin = __tris_decrypt_bin;
	return 0;
}

static int reload(void)
{
	crypto_load(-1, -1);
	return 0;
}

static int load_module(void)
{
	crypto_init();
	if (tris_opt_init_keys)
		crypto_load(STDIN_FILENO, STDOUT_FILENO);
	else
		crypto_load(-1, -1);
	return TRIS_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* Can't unload this once we're loaded */
	return -1;
}

/* needs usecount semantics defined */
TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Cryptographic Digital Signatures",
		.load = load_module,
		.unload = unload_module,
		.reload = reload
	);
