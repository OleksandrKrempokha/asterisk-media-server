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
 * \brief Execute arbitrary authenticate commands
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 154578 $")

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/astdb.h"
#include "trismedia/utils.h"

enum {
	OPT_ACCOUNT = (1 << 0),
	OPT_DATABASE = (1 << 1),
	OPT_MULTIPLE = (1 << 3),
	OPT_REMOVE = (1 << 4),
} auth_option_flags;

TRIS_APP_OPTIONS(auth_app_options, {
	TRIS_APP_OPTION('a', OPT_ACCOUNT),
	TRIS_APP_OPTION('d', OPT_DATABASE),
	TRIS_APP_OPTION('m', OPT_MULTIPLE),
	TRIS_APP_OPTION('r', OPT_REMOVE),
});


static char *app = "Authenticate";
/*** DOCUMENTATION
	<application name="Authenticate" language="en_US">
		<synopsis>
			Authenticate a user
		</synopsis>
		<syntax>
			<parameter name="password" required="true">
				<para>Password the user should know</para>
			</parameter>
			<parameter name="options" required="false">
				<optionlist>
					<option name="a">
						<para>Set the channels' account code to the password that is entered</para>
					</option>
					<option name="d">
						<para>Interpret the given path as database key, not a literal file</para>
					</option>
					<option name="m">
						<para>Interpret the given path as a file which contains a list of account
						codes and password hashes delimited with <literal>:</literal>, listed one per line in
						the file. When one of the passwords is matched, the channel will have
						its account code set to the corresponding account code in the file.</para>
					</option>
					<option name="r">
						<para>Remove the database key upon successful entry (valid with <literal>d</literal> only)</para>
					</option>
				</optionlist>
			</parameter>
			<parameter name="maxdigits" required="false">
				<para>maximum acceptable number of digits. Stops reading after
				maxdigits have been entered (without requiring the user to press the <literal>#</literal> key).
				Defaults to 0 - no limit - wait for the user press the <literal>#</literal> key.</para>
			</parameter>
			<parameter name="prompt" required="false">
				<para>Override the agent-pass prompt file.</para>
			</parameter>
		</syntax>
		<description>
			<para>This application asks the caller to enter a given password in order to continue dialplan execution.</para>
			<para>If the password begins with the <literal>/</literal> character, 
			it is interpreted as a file which contains a list of valid passwords, listed 1 password per line in the file.</para>
			<para>When using a database key, the value associated with the key can be anything.</para>
			<para>Users have three attempts to authenticate before the channel is hung up.</para>
		</description>
		<see-also>
			<ref type="application">VMAuthenticate</ref>
			<ref type="application">DISA</ref>
		</see-also>
	</application>
 ***/

static int auth_exec(struct tris_channel *chan, void *data)
{
	int res = 0, retries, maxdigits;
	char passwd[256], *prompt = "agent-pass", *argcopy = NULL;
	struct tris_flags flags = {0};

	TRIS_DECLARE_APP_ARGS(arglist,
		TRIS_APP_ARG(password);
		TRIS_APP_ARG(options);
		TRIS_APP_ARG(maxdigits);
		TRIS_APP_ARG(prompt);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Authenticate requires an argument(password)\n");
		return -1;
	}

	if (chan->_state != TRIS_STATE_UP) {
		if ((res = tris_answer(chan)))
			return -1;
	}

	argcopy = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(arglist, argcopy);

	if (!tris_strlen_zero(arglist.options))
		tris_app_parse_options(auth_app_options, &flags, NULL, arglist.options);

	if (!tris_strlen_zero(arglist.maxdigits)) {
		maxdigits = atoi(arglist.maxdigits);
		if ((maxdigits<1) || (maxdigits>sizeof(passwd)-2))
			maxdigits = sizeof(passwd) - 2;
	} else {
		maxdigits = sizeof(passwd) - 2;
	}

	if (!tris_strlen_zero(arglist.prompt)) {
		prompt = arglist.prompt;
	} else {
		prompt = "agent-pass";
	}
   
	/* Start asking for password */
	for (retries = 0; retries < 3; retries++) {
		if ((res = tris_app_getdata(chan, prompt, passwd, maxdigits, 0)) < 0)
			break;

		res = 0;

		if (arglist.password[0] != '/') {
			/* Compare against a fixed password */
			if (!strcmp(passwd, arglist.password))
				break;
		} else if (tris_test_flag(&flags,OPT_DATABASE)) {
			char tmp[256];
			/* Compare against a database key */
			if (!tris_db_get(arglist.password + 1, passwd, tmp, sizeof(tmp))) {
				/* It's a good password */
				if (tris_test_flag(&flags,OPT_REMOVE))
					tris_db_del(arglist.password + 1, passwd);
				break;
			}
		} else {
			/* Compare against a file */
			FILE *f;
			char buf[256] = "", md5passwd[33] = "", *md5secret = NULL;

			if (!(f = fopen(arglist.password, "r"))) {
				tris_log(LOG_WARNING, "Unable to open file '%s' for authentication: %s\n", arglist.password, strerror(errno));
				continue;
			}

			for (;;) {
				size_t len;

				if (feof(f))
					break;

				if (!fgets(buf, sizeof(buf), f)) {
					continue;
				}

				if (tris_strlen_zero(buf))
					continue;

				len = strlen(buf) - 1;
				if (buf[len] == '\n')
					buf[len] = '\0';

				if (tris_test_flag(&flags, OPT_MULTIPLE)) {
					md5secret = buf;
					strsep(&md5secret, ":");
					if (!md5secret)
						continue;
					tris_md5_hash(md5passwd, passwd);
					if (!strcmp(md5passwd, md5secret)) {
						if (tris_test_flag(&flags,OPT_ACCOUNT))
							tris_cdr_setaccount(chan, buf);
						break;
					}
				} else {
					if (!strcmp(passwd, buf)) {
						if (tris_test_flag(&flags, OPT_ACCOUNT))
							tris_cdr_setaccount(chan, buf);
						break;
					}
				}
			}

			fclose(f);

			if (!tris_strlen_zero(buf)) {
				if (tris_test_flag(&flags, OPT_MULTIPLE)) {
					if (md5secret && !strcmp(md5passwd, md5secret))
						break;
				} else {
					if (!strcmp(passwd, buf))
						break;
				}
			}
		}
		prompt = "auth-incorrect";
	}

	if ((retries < 3) && !res) {
		if (tris_test_flag(&flags,OPT_ACCOUNT) && !tris_test_flag(&flags,OPT_MULTIPLE))
			tris_cdr_setaccount(chan, passwd);
		if (!(res = tris_streamfile(chan, "auth-thankyou", chan->language)))
			res = tris_waitstream(chan, "");
	} else {
		if (!tris_streamfile(chan, "goodbye", chan->language))
			res = tris_waitstream(chan, "");
		res = -1;
	}

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	if (tris_register_application_xml(app, auth_exec))
		return TRIS_MODULE_LOAD_FAILURE;
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Authentication Application");
