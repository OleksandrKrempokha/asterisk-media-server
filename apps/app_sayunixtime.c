/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2003, 2006 Tilghman Lesher.  All rights reserved.
 * Copyright (c) 2006 Digium, Inc.
 *
 * Tilghman Lesher <app_sayunixtime__200309@the-tilghman.com>
 *
 * This code is released by the author with no restrictions on usage.
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 *
 * \brief SayUnixTime application
 *
 * \author Tilghman Lesher <app_sayunixtime__200309@the-tilghman.com>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 154647 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/say.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<application name="SayUnixTime" language="en_US">
		<synopsis>
			Says a specified time in a custom format.
		</synopsis>
		<syntax>
			<parameter name="unixtime">
				<para>time, in seconds since Jan 1, 1970.  May be negative. Defaults to now.</para>
			</parameter>
			<parameter name="timezone">
				<para>timezone, see <directory>/usr/share/zoneinfo</directory> for a list. Defaults to machine default.</para>
			</parameter>
			<parameter name="format">
				<para>a format the time is to be said in.  See <filename>voicemail.conf</filename>.
				Defaults to <literal>ABdY "digits/at" IMp</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>Uses some of the sound files stored in <directory>/var/lib/trismedia/sounds</directory> to construct a phrase 
			saying the specified date and/or time in the specified format. </para>
		</description>
		<see-also>
			<ref type="function">STRFTIME</ref>
			<ref type="function">STRPTIME</ref>
			<ref type="function">IFTIME</ref>
		</see-also>
	</application>
	<application name="DateTime" language="en_US">
		<synopsis>
			Says a specified time in a custom format.
		</synopsis>
		<syntax>
			<parameter name="unixtime">
				<para>time, in seconds since Jan 1, 1970.  May be negative. Defaults to now.</para>
			</parameter>
			<parameter name="timezone">
				<para>timezone, see <filename>/usr/share/zoneinfo</filename> for a list. Defaults to machine default.</para>
			</parameter>
			<parameter name="format">
				<para>a format the time is to be said in.  See <filename>voicemail.conf</filename>.
				Defaults to <literal>ABdY "digits/at" IMp</literal></para>
			</parameter>
		</syntax>
		<description>
			<para>Say the date and time in a specified format.</para>
		</description>
	</application>

 ***/

static char *app_sayunixtime = "SayUnixTime";
static char *app_datetime = "DateTime";

static int sayunixtime_exec(struct tris_channel *chan, void *data)
{
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(timeval);
		TRIS_APP_ARG(timezone);
		TRIS_APP_ARG(format);
	);
	char *parse;
	int res = 0;
	time_t unixtime;
	
	if (!data)
		return 0;

	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);

	tris_get_time_t(args.timeval, &unixtime, time(NULL), NULL);

	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);

	if (!res)
		res = tris_say_date_with_format(chan, unixtime, TRIS_DIGIT_ANY,
					       chan->language, args.format, args.timezone);

	return res;
}

static int unload_module(void)
{
	int res;
	
	res = tris_unregister_application(app_sayunixtime);
	res |= tris_unregister_application(app_datetime);
	
	return res;
}

static int load_module(void)
{
	int res;
	
	res = tris_register_application_xml(app_sayunixtime, sayunixtime_exec);
	res |= tris_register_application_xml(app_datetime, sayunixtime_exec);
	
	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Say time");
