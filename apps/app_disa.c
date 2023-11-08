/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 *
 * Made only slightly more sane by Mark Spencer <markster@digium.com>
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
 * \brief DISA -- Direct Inward System Access Application
 *
 * \author Jim Dixon <jim@lambdatel.com>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 220292 $")

#include <math.h>
#include <sys/time.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/app.h"
#include "trismedia/indications.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/ulaw.h"
#include "trismedia/callerid.h"
#include "trismedia/stringfields.h"

/*** DOCUMENTATION
	<application name="DISA" language="en_US">
		<synopsis>
			Direct Inward System Access.
		</synopsis>
		<syntax>
			<parameter name="passcode|filename" required="true">
				<para>If you need to present a DISA dialtone without entering a password,
				simply set <replaceable>passcode</replaceable> to <literal>no-password</literal></para>
				<para>You may specified a <replaceable>filename</replaceable> instead of a
				<replaceable>passcode</replaceable>, this filename must contain individual passcodes</para>
			</parameter>
			<parameter name="context">
				<para>Specifies the dialplan context in which the user-entered extension
				will be matched. If no context is specified, the DISA application defaults
				to the <literal>disa</literal> context. Presumably a normal system will have a special
				context set up for DISA use with some or a lot of restrictions.</para>
			</parameter>
			<parameter name="cid">
				<para>Specifies a new (different) callerid to be used for this call.</para>
			</parameter>
			<parameter name="mailbox" argsep="@">
				<para>Will cause a stutter-dialtone (indication <emphasis>dialrecall</emphasis>)
				to be used, if the specified mailbox contains any new messages.</para>
				<argument name="mailbox" required="true" />
				<argument name="context" required="false" />
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="n">
						<para>The DISA application will not answer initially.</para>
					</option>
					<option name="p">
						<para>The extension entered will be considered complete when a <literal>#</literal>
						is entered.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>The DISA, Direct Inward System Access, application allows someone from
			outside the telephone switch (PBX) to obtain an <emphasis>internal</emphasis> system
			dialtone and to place calls from it as if they were placing a call from
			within the switch.
			DISA plays a dialtone. The user enters their numeric passcode, followed by
			the pound sign <literal>#</literal>. If the passcode is correct, the user is then given
			system dialtone within <replaceable>context</replaceable> on which a call may be placed.
			If the user enters an invalid extension and extension <literal>i</literal> exists in the specified
			<replaceable>context</replaceable>, it will be used.
			</para>
			<para>Be aware that using this may compromise the security of your PBX.</para>
			<para>The arguments to this application (in <filename>extensions.conf</filename>) allow either
			specification of a single global <replaceable>passcode</replaceable> (that everyone uses), or
			individual passcodes contained in a file (<replaceable>filename</replaceable>).</para>
			<para>The file that contains the passcodes (if used) allows a complete
			specification of all of the same arguments available on the command
			line, with the sole exception of the options. The file may contain blank
			lines, or comments starting with <literal>#</literal> or <literal>;</literal>.</para>
		</description>
		<see-also>
			<ref type="application">Authenticate</ref>
			<ref type="application">VMAuthenticate</ref>
		</see-also>
	</application>
 ***/
static char *app = "DISA";

enum {
	NOANSWER_FLAG = (1 << 0),
	POUND_TO_END_FLAG = (1 << 1),
} option_flags;

TRIS_APP_OPTIONS(app_opts, {
	TRIS_APP_OPTION('n', NOANSWER_FLAG),
	TRIS_APP_OPTION('p', POUND_TO_END_FLAG),
});

static void play_dialtone(struct tris_channel *chan, char *mailbox)
{
	struct tris_tone_zone_sound *ts = NULL;

	if (tris_app_has_voicemail(mailbox, NULL)) {
		ts = tris_get_indication_tone(chan->zone, "dialrecall");
	} else {
		ts = tris_get_indication_tone(chan->zone, "dial");
	}

	if (ts) {
		tris_playtones_start(chan, 0, ts->data, 0);
		ts = tris_tone_zone_sound_unref(ts);
	} else {
		tris_tonepair_start(chan, 350, 440, 0, 0);
	}
}

static int disa_exec(struct tris_channel *chan, void *data)
{
	int i = 0, j, k = 0, did_ignore = 0, special_noanswer = 0;
	int firstdigittimeout = (chan->pbx ? chan->pbx->rtimeoutms : 20000);
	int digittimeout = (chan->pbx ? chan->pbx->dtimeoutms : 10000);
	struct tris_flags flags;
	char *tmp, exten[TRIS_MAX_EXTENSION] = "", acctcode[20]="";
	char pwline[256];
	char ourcidname[256],ourcidnum[256];
	struct tris_frame *f;
	struct timeval lastdigittime;
	int res;
	FILE *fp;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(passcode);
		TRIS_APP_ARG(context);
		TRIS_APP_ARG(cid);
		TRIS_APP_ARG(mailbox);
		TRIS_APP_ARG(options);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "DISA requires an argument (passcode/passcode file)\n");
		return -1;
	}

	tris_debug(1, "Digittimeout: %d\n", digittimeout);
	tris_debug(1, "Responsetimeout: %d\n", firstdigittimeout);

	tmp = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, tmp);

	if (tris_strlen_zero(args.context))
		args.context = "disa";
	if (tris_strlen_zero(args.mailbox))
		args.mailbox = "";
	if (!tris_strlen_zero(args.options))
		tris_app_parse_options(app_opts, &flags, NULL, args.options);

	tris_debug(1, "Mailbox: %s\n",args.mailbox);

	if (!tris_test_flag(&flags, NOANSWER_FLAG)) {
		if (chan->_state != TRIS_STATE_UP) {
			/* answer */
			tris_answer(chan);
		}
	} else special_noanswer = 1;

	tris_debug(1, "Context: %s\n",args.context);

	if (!strcasecmp(args.passcode, "no-password")) {
		k |= 1; /* We have the password */
		tris_debug(1, "DISA no-password login success\n");
	}

	lastdigittime = tris_tvnow();

	play_dialtone(chan, args.mailbox);

	tris_set_flag(chan, TRIS_FLAG_END_DTMF_ONLY);

	for (;;) {
		  /* if outa time, give em reorder */
		if (tris_tvdiff_ms(tris_tvnow(), lastdigittime) > ((k&2) ? digittimeout : firstdigittimeout)) {
			tris_debug(1,"DISA %s entry timeout on chan %s\n",
				((k&1) ? "extension" : "password"),chan->name);
			break;
		}

		if ((res = tris_waitfor(chan, -1) < 0)) {
			tris_debug(1, "Waitfor returned %d\n", res);
			continue;
		}

		if (!(f = tris_read(chan))) {
			tris_clear_flag(chan, TRIS_FLAG_END_DTMF_ONLY);
			return -1;
		}

		if ((f->frametype == TRIS_FRAME_CONTROL) && (f->subclass == TRIS_CONTROL_HANGUP)) {
			if (f->data.uint32)
				chan->hangupcause = f->data.uint32;
			tris_frfree(f);
			tris_clear_flag(chan, TRIS_FLAG_END_DTMF_ONLY);
			return -1;
		}

		/* If the frame coming in is not DTMF, just drop it and continue */
		if (f->frametype != TRIS_FRAME_DTMF) {
			tris_frfree(f);
			continue;
		}

		j = f->subclass;  /* save digit */
		tris_frfree(f);

		if (!i) {
			k |= 2; /* We have the first digit */
			tris_playtones_stop(chan);
		}

		lastdigittime = tris_tvnow();

		/* got a DTMF tone */
		if (i < TRIS_MAX_EXTENSION) { /* if still valid number of digits */
			if (!(k&1)) { /* if in password state */
				if (j == '#') { /* end of password */
					  /* see if this is an integer */
					if (sscanf(args.passcode,"%30d",&j) < 1) { /* nope, it must be a filename */
						fp = fopen(args.passcode,"r");
						if (!fp) {
							tris_log(LOG_WARNING,"DISA password file %s not found on chan %s\n",args.passcode,chan->name);
							tris_clear_flag(chan, TRIS_FLAG_END_DTMF_ONLY);
							return -1;
						}
						pwline[0] = 0;
						while(fgets(pwline,sizeof(pwline) - 1,fp)) {
							if (!pwline[0])
								continue;
							if (pwline[strlen(pwline) - 1] == '\n')
								pwline[strlen(pwline) - 1] = 0;
							if (!pwline[0])
								continue;
							 /* skip comments */
							if (pwline[0] == '#')
								continue;
							if (pwline[0] == ';')
								continue;

							TRIS_STANDARD_APP_ARGS(args, pwline);

							tris_debug(1, "Mailbox: %s\n",args.mailbox);

							/* password must be in valid format (numeric) */
							if (sscanf(args.passcode,"%30d", &j) < 1)
								continue;
							 /* if we got it */
							if (!strcmp(exten,args.passcode)) {
								if (tris_strlen_zero(args.context))
									args.context = "disa";
								if (tris_strlen_zero(args.mailbox))
									args.mailbox = "";
								break;
							}
						}
						fclose(fp);
					}
					/* compare the two */
					if (strcmp(exten,args.passcode)) {
						tris_log(LOG_WARNING,"DISA on chan %s got bad password %s\n",chan->name,exten);
						goto reorder;

					}
					 /* password good, set to dial state */
					tris_debug(1,"DISA on chan %s password is good\n",chan->name);
					play_dialtone(chan, args.mailbox);

					k|=1; /* In number mode */
					i = 0;  /* re-set buffer pointer */
					exten[sizeof(acctcode)] = 0;
					tris_copy_string(acctcode, exten, sizeof(acctcode));
					exten[0] = 0;
					tris_debug(1,"Successful DISA log-in on chan %s\n", chan->name);
					continue;
				}
			} else {
				if (j == '#') { /* end of extension .. maybe */
					if (i == 0 && 
							(tris_matchmore_extension(chan, args.context, "#", 1, chan->cid.cid_num) ||
							 tris_exists_extension(chan, args.context, "#", 1, chan->cid.cid_num)) ) {
						/* Let the # be the part of, or the entire extension */
					} else {
						break;
					}
				}
			}

			exten[i++] = j;  /* save digit */
			exten[i] = 0;
			if (!(k&1))
				continue; /* if getting password, continue doing it */
			/* if this exists */

			/* user wants end of number, remove # */
			if (tris_test_flag(&flags, POUND_TO_END_FLAG) && j == '#') {
				exten[--i] = 0;
				break;
			}

			if (tris_ignore_pattern(args.context, exten)) {
				play_dialtone(chan, "");
				did_ignore = 1;
			} else
				if (did_ignore) {
					tris_playtones_stop(chan);
					did_ignore = 0;
				}

			/* if can do some more, do it */
			if (!tris_matchmore_extension(chan,args.context,exten,1, chan->cid.cid_num)) {
				break;
			}
		}
	}

	tris_clear_flag(chan, TRIS_FLAG_END_DTMF_ONLY);

	if (k == 3) {
		int recheck = 0;
		struct tris_flags cdr_flags = { TRIS_CDR_FLAG_POSTED };

		if (!tris_exists_extension(chan, args.context, exten, 1, chan->cid.cid_num)) {
			pbx_builtin_setvar_helper(chan, "INVALID_EXTEN", exten);
			exten[0] = 'i';
			exten[1] = '\0';
			recheck = 1;
		}
		if (!recheck || tris_exists_extension(chan, args.context, exten, 1, chan->cid.cid_num)) {
			tris_playtones_stop(chan);
			/* We're authenticated and have a target extension */
			if (!tris_strlen_zero(args.cid)) {
				tris_callerid_split(args.cid, ourcidname, sizeof(ourcidname), ourcidnum, sizeof(ourcidnum));
				tris_set_callerid(chan, ourcidnum, ourcidname, ourcidnum);
			}

			if (!tris_strlen_zero(acctcode))
				tris_string_field_set(chan, accountcode, acctcode);

			if (special_noanswer) cdr_flags.flags = 0;
			tris_cdr_reset(chan->cdr, &cdr_flags);
			tris_explicit_goto(chan, args.context, exten, 1);
			return 0;
		}
	}

	/* Received invalid, but no "i" extension exists in the given context */

reorder:
	/* Play congestion for a bit */
	tris_indicate(chan, TRIS_CONTROL_CONGESTION);
	tris_safe_sleep(chan, 10*1000);

	tris_playtones_stop(chan);

	return -1;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, disa_exec) ?
		TRIS_MODULE_LOAD_DECLINE : TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "DISA (Direct Inward System Access) Application");
