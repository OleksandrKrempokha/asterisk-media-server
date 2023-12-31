/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (c) 2006, Tilghman Lesher.  All rights reserved.
 *
 * Tilghman Lesher <app_morsecode__v001@the-tilghman.com>
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
 * \brief Morsecode application
 *
 * \author Tilghman Lesher <app_morsecode__v001@the-tilghman.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/indications.h"

/*** DOCUMENTATION
	<application name="Morsecode" language="en_US">
		<synopsis>
			Plays morse code.
		</synopsis>
		<syntax>
			<parameter name="string" required="true">
				<para>String to playback as morse code to channel</para>
			</parameter>
		</syntax>
		<description>
			<para>Plays the Morse code equivalent of the passed string.</para>

			<para>This application uses the following variables:</para>
			<variablelist>
				<variable name="MORSEDITLEN">
					<para>Use this value in (ms) for length of dit</para>
				</variable>
				<variable name="MORSETONE">
					<para>The pitch of the tone in (Hz), default is 800</para>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">SayAlpha</ref>
			<ref type="application">SayPhonetic</ref>
		</see-also>
	</application>
 ***/	
static char *app_morsecode = "Morsecode";

static char *morsecode[] = {
	"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", /*  0-15 */
	"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", /* 16-31 */
	" ",      /* 32 - <space> */
	".-.-.-", /* 33 - ! */
	".-..-.", /* 34 - " */
	"",       /* 35 - # */
	"",       /* 36 - $ */
	"",       /* 37 - % */
	"",       /* 38 - & */
	".----.", /* 39 - ' */
	"-.--.-", /* 40 - ( */
	"-.--.-", /* 41 - ) */
	"",       /* 42 - * */
	"",       /* 43 - + */
	"--..--", /* 44 - , */
	"-....-", /* 45 - - */
	".-.-.-", /* 46 - . */
	"-..-.",  /* 47 - / */
	"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.", /* 48-57 - 0-9 */
	"---...", /* 58 - : */
	"-.-.-.", /* 59 - ; */
	"",       /* 60 - < */
	"-...-",  /* 61 - = */
	"",       /* 62 - > */
	"..--..", /* 63 - ? */
	".--.-.", /* 64 - @ */
	".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
	"-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
	"-.--.-", /* 91 - [ (really '(') */
	"-..-.",  /* 92 - \ (really '/') */
	"-.--.-", /* 93 - ] (really ')') */
	"",       /* 94 - ^ */
	"..--.-", /* 95 - _ */
	".----.", /* 96 - ` */
	".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
	"-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
	"-.--.-", /* 123 - { (really '(') */
	"",       /* 124 - | */
	"-.--.-", /* 125 - } (really ')') */
	"-..-.",  /* 126 - ~ (really bar) */
	". . .",  /* 127 - <del> (error) */
};

static void playtone(struct tris_channel *chan, int tone, int len)
{
	char dtmf[20];
	snprintf(dtmf, sizeof(dtmf), "%d/%d", tone, len);
	tris_playtones_start(chan, 0, dtmf, 0);
	tris_safe_sleep(chan, len);
	tris_playtones_stop(chan);
}

static int morsecode_exec(struct tris_channel *chan, void *data)
{
	int res=0, ditlen, tone;
	char *digit;
	const char *ditlenc, *tonec;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Syntax: Morsecode(<string>) - no argument found\n");
		return 0;
	}

	/* Use variable MORESEDITLEN, if set (else 80) */
	tris_channel_lock(chan);
	ditlenc = pbx_builtin_getvar_helper(chan, "MORSEDITLEN");
	if (tris_strlen_zero(ditlenc) || (sscanf(ditlenc, "%30d", &ditlen) != 1)) {
		ditlen = 80;
	}
	tris_channel_unlock(chan);

	/* Use variable MORSETONE, if set (else 800) */
	tris_channel_lock(chan);
	tonec = pbx_builtin_getvar_helper(chan, "MORSETONE");
	if (tris_strlen_zero(tonec) || (sscanf(tonec, "%30d", &tone) != 1)) {
		tone = 800;
	}
	tris_channel_unlock(chan);

	for (digit = data; *digit; digit++) {
		int digit2 = *digit;
		char *dahdit;
		if (digit2 < 0) {
			continue;
		}
		for (dahdit = morsecode[digit2]; *dahdit; dahdit++) {
			if (*dahdit == '-') {
				playtone(chan, tone, 3 * ditlen);
			} else if (*dahdit == '.') {
				playtone(chan, tone, 1 * ditlen);
			} else {
				/* Account for ditlen of silence immediately following */
				playtone(chan, 0, 2 * ditlen);
			}

			/* Pause slightly between each dit and dah */
			playtone(chan, 0, 1 * ditlen);
		}
		/* Pause between characters */
		playtone(chan, 0, 2 * ditlen);
	}

	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app_morsecode);
}

static int load_module(void)
{
	return tris_register_application_xml(app_morsecode, morsecode_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Morse code");
