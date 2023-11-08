/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Digium, Inc.
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
 * \brief AES encryption/decryption dialplan functions
 *
 * \author David Vossel <dvossel@digium.com>
 * \ingroup functions
 */


#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 172548 $")

#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/app.h"
#include "trismedia/aes.h"

#define AES_BLOCK_SIZE 16

/*** DOCUMENTATION
	<function name="AES_ENCRYPT" language="en_US">
		<synopsis>
			Encrypt a string with AES given a 16 character key.
		</synopsis>
		<syntax>
			<parameter name="key" required="true">
				<para>AES Key</para>
			</parameter>
			<parameter name="string" required="true">
				<para>Input string</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns an AES encrypted string encoded in base64.</para>
		</description>
	</function>
	<function name="AES_DECRYPT" language="en_US">
		<synopsis>
			Decrypt a string encoded in base64 with AES given a 16 character key.
		</synopsis>
		<syntax>
			<parameter name="key" required="true">
				<para>AES Key</para>
			</parameter>
			<parameter name="string" required="true">
				<para>Input string.</para>
			</parameter>
		</syntax>
		<description>
			<para>Returns the plain text string.</para>
		</description>
	</function>
 ***/


static int aes_helper(struct tris_channel *chan, const char *cmd, char *data,
	       char *buf, size_t len)
{
	unsigned char curblock[AES_BLOCK_SIZE] = { 0, };
	char *tmp;
	char *tmpP;
	int data_len, encrypt;
	tris_aes_encrypt_key ecx;                        /*  AES 128 Encryption context */
	tris_aes_decrypt_key dcx;

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(key);
		TRIS_APP_ARG(data);
	);

	TRIS_STANDARD_APP_ARGS(args, data);

	if (tris_strlen_zero(args.data) || tris_strlen_zero(args.key)) {
		tris_log(LOG_WARNING, "Syntax: %s(<key>,<data>) - missing argument!\n", cmd);
		return -1;
	}

	if (strlen(args.key) != AES_BLOCK_SIZE) {        /* key must be of 16 characters in length, 128 bits */
		tris_log(LOG_WARNING, "Syntax: %s(<key>,<data>) - <key> parameter must be exactly 16 characters!\n", cmd);
		return -1;
	}

	tris_aes_encrypt_key((unsigned char *) args.key, &ecx);   /* encryption:  plaintext -> encryptedtext -> base64 */
	tris_aes_decrypt_key((unsigned char *) args.key, &dcx);   /* decryption:  base64 -> encryptedtext -> plaintext */
	tmp = tris_calloc(1, len);                     /* requires a tmp buffer for the base64 decode */
	tmpP = tmp;
	encrypt = strcmp("AES_DECRYPT", cmd);           /* -1 if encrypting, 0 if decrypting */

	if (encrypt) {                                  /* if decrypting first decode src to base64 */
		tris_copy_string(tmp, args.data, len);
		data_len = strlen(tmp);
	} else {
		data_len = tris_base64decode((unsigned char *) tmp, args.data, len);
	}

	if (data_len >= len) {                        /* make sure to not go over buffer len */
		tris_log(LOG_WARNING, "Syntax: %s(<keys>,<data>) - <data> exceeds buffer length.  Result may be truncated!\n", cmd);
		data_len = len - 1;
	}

	while (data_len > 0) {
		memset(curblock, 0, AES_BLOCK_SIZE);
		memcpy(curblock, tmpP, (data_len < AES_BLOCK_SIZE) ? data_len : AES_BLOCK_SIZE);
		if (encrypt) {
			tris_aes_encrypt(curblock, (unsigned char *) tmpP, &ecx);
		} else {
			tris_aes_decrypt(curblock, (unsigned char *) tmpP, &dcx);
		}
		tmpP += AES_BLOCK_SIZE;
		data_len -= AES_BLOCK_SIZE;
	}

	if (encrypt) {                            /* if encrypting encode result to base64 */
		tris_base64encode(buf, (unsigned char *) tmp, strlen(tmp), len);
	} else {
		memcpy(buf, tmp, len);
	}
	tris_free(tmp);

	return 0;
}

static struct tris_custom_function aes_encrypt_function = {
	.name = "AES_ENCRYPT",
	.read = aes_helper,
};

static struct tris_custom_function aes_decrypt_function = {
	.name = "AES_DECRYPT",
	.read = aes_helper,
};

static int unload_module(void)
{
	int res = tris_custom_function_unregister(&aes_decrypt_function);
	return res | tris_custom_function_unregister(&aes_encrypt_function);
}

static int load_module(void)
{
	int res = tris_custom_function_register(&aes_decrypt_function);
	res |= tris_custom_function_register(&aes_encrypt_function);
	return res ? TRIS_MODULE_LOAD_DECLINE : TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "AES dialplan functions");
