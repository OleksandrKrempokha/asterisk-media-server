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
 * \brief Provide a directory of extensions
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>app_voicemail</depend>
 ***/
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 249953 $")

#include <ctype.h>

#include "trismedia/paths.h" /* use tris_config_TRIS_SPOOL_DIR */
#include "trismedia/file.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/say.h"
#include "trismedia/app.h"
#include "trismedia/utils.h"

/*** DOCUMENTATION
	<application name="Directory" language="en_US">
		<synopsis>
			Provide directory of voicemail extensions.
		</synopsis>
		<syntax>
			<parameter name="vm-context">
				<para>This is the context within voicemail.conf to use for the Directory. If not 
				specified and <literal>searchcontexts=no</literal> in 
				<filename>voicemail.conf</filename>, then <literal>default</literal> 
				will be assumed.</para>
			</parameter>
			<parameter name="dial-context" required="false">
				<para>This is the dialplan context to use when looking for an
				extension that the user has selected, or when jumping to the
				<literal>o</literal> or <literal>a</literal> extension.</para>
			</parameter>
			<parameter name="options" required="false">
				<optionlist>
					<option name="e">
						<para>In addition to the name, also read the extension number to the
						caller before presenting dialing options.</para>
					</option>
					<option name="f">
						<para>Allow the caller to enter the first name of a user in the
						directory instead of using the last name.  If specified, the
						optional number argument will be used for the number of
						characters the user should enter.</para>
						<argument name="n" required="true" />
					</option>
					<option name="l">
						<para>Allow the caller to enter the last name of a user in the
						directory.  This is the default.  If specified, the
						optional number argument will be used for the number of
						characters the user should enter.</para>
						<argument name="n" required="true" />
					</option>
					<option name="b">
						<para> Allow the caller to enter either the first or the last name
						of a user in the directory.  If specified, the optional number
						argument will be used for the number of characters the user should enter.</para>
						<argument name="n" required="true" />
					</option>
					<option name="m">
						<para>Instead of reading each name sequentially and asking for
						confirmation, create a menu of up to 8 names.</para>
					</option>
					<option name="p">
						<para>Pause for n milliseconds after the digits are typed.  This is
						helpful for people with cellphones, who are not holding the
						receiver to their ear while entering DTMF.</para>
						<argument name="n" required="true" />
					</option>
				</optionlist>
				<note><para>Only one of the <replaceable>f</replaceable>, <replaceable>l</replaceable>, or <replaceable>b</replaceable>
				options may be specified. <emphasis>If more than one is specified</emphasis>, then Directory will act as 
				if <replaceable>b</replaceable> was specified.  The number
				of characters for the user to type defaults to <literal>3</literal>.</para></note>
			</parameter>
		</syntax>
		<description>
			<para>This application will present the calling channel with a directory of extensions from which they can search
			by name. The list of names and corresponding extensions is retrieved from the
			voicemail configuration file, <filename>voicemail.conf</filename>.</para>
			<para>This application will immediately exit if one of the following DTMF digits are
			received and the extension to jump to exists:</para>
			<para><literal>0</literal> - Jump to the 'o' extension, if it exists.</para>
			<para><literal>*</literal> - Jump to the 'a' extension, if it exists.</para>
		</description>
	</application>

 ***/
static char *app = "Directory";

/* For simplicity, I'm keeping the format compatible with the voicemail config,
   but i'm open to suggestions for isolating it */

#define VOICEMAIL_CONFIG "voicemail.conf"

enum {
	OPT_LISTBYFIRSTNAME = (1 << 0),
	OPT_SAYEXTENSION =    (1 << 1),
	OPT_FROMVOICEMAIL =   (1 << 2),
	OPT_SELECTFROMMENU =  (1 << 3),
	OPT_LISTBYLASTNAME =  (1 << 4),
	OPT_LISTBYEITHER =    OPT_LISTBYFIRSTNAME | OPT_LISTBYLASTNAME,
	OPT_PAUSE =           (1 << 5),
} directory_option_flags;

enum {
	OPT_ARG_FIRSTNAME =   0,
	OPT_ARG_LASTNAME =    1,
	OPT_ARG_EITHER =      2,
	OPT_ARG_PAUSE =       3,
	/* This *must* be the last value in this enum! */
	OPT_ARG_ARRAY_SIZE =  4,
};

struct directory_item {
	char exten[TRIS_MAX_EXTENSION + 1];
	char name[TRIS_MAX_EXTENSION + 1];
	char context[TRIS_MAX_CONTEXT + 1];
	char key[50]; /* Text to order items. Either lastname+firstname or firstname+lastname */

	TRIS_LIST_ENTRY(directory_item) entry;
};

TRIS_APP_OPTIONS(directory_app_options, {
	TRIS_APP_OPTION_ARG('f', OPT_LISTBYFIRSTNAME, OPT_ARG_FIRSTNAME),
	TRIS_APP_OPTION_ARG('l', OPT_LISTBYLASTNAME, OPT_ARG_LASTNAME),
	TRIS_APP_OPTION_ARG('b', OPT_LISTBYEITHER, OPT_ARG_EITHER),
	TRIS_APP_OPTION_ARG('p', OPT_PAUSE, OPT_ARG_PAUSE),
	TRIS_APP_OPTION('e', OPT_SAYEXTENSION),
	TRIS_APP_OPTION('v', OPT_FROMVOICEMAIL),
	TRIS_APP_OPTION('m', OPT_SELECTFROMMENU),
});

static int compare(const char *text, const char *template)
{
	char digit;

	if (tris_strlen_zero(text)) {
		return -1;
	}

	while (*template) {
		digit = toupper(*text++);
		switch (digit) {
		case 0:
			return -1;
		case '1':
			digit = '1';
			break;
		case '2':
		case 'A':
		case 'B':
		case 'C':
			digit = '2';
			break;
		case '3':
		case 'D':
		case 'E':
		case 'F':
			digit = '3';
			break;
		case '4':
		case 'G':
		case 'H':
		case 'I':
			digit = '4';
			break;
		case '5':
		case 'J':
		case 'K':
		case 'L':
			digit = '5';
			break;
		case '6':
		case 'M':
		case 'N':
		case 'O':
			digit = '6';
			break;
		case '7':
		case 'P':
		case 'Q':
		case 'R':
		case 'S':
			digit = '7';
			break;
		case '8':
		case 'T':
		case 'U':
		case 'V':
			digit = '8';
			break;
		case '9':
		case 'W':
		case 'X':
		case 'Y':
		case 'Z':
			digit = '9';
			break;

		default:
			if (digit > ' ')
				return -1;
			continue;
		}

		if (*template++ != digit)
			return -1;
	}

	return 0;
}

/* play name of mailbox owner.
 * returns:  -1 for bad or missing extension
 *           '1' for selected entry from directory
 *           '*' for skipped entry from directory
 */
static int play_mailbox_owner(struct tris_channel *chan, const char *context,
	const char *ext, const char *name, struct tris_flags *flags)
{
	int res = 0;
	if ((res = tris_app_sayname(chan, ext, context)) >= 0) {
		tris_stopstream(chan);
		/* If Option 'e' was specified, also read the extension number with the name */
		if (tris_test_flag(flags, OPT_SAYEXTENSION)) {
			tris_stream_and_wait(chan, "voicemail/vm-extension", TRIS_DIGIT_ANY);
			res = tris_say_character_str(chan, ext, TRIS_DIGIT_ANY, chan->language);
		}
	} else {
		res = tris_say_character_str(chan, S_OR(name, ext), TRIS_DIGIT_ANY, chan->language);
		if (!tris_strlen_zero(name) && tris_test_flag(flags, OPT_SAYEXTENSION)) {
			tris_stream_and_wait(chan, "voicemail/vm-extension", TRIS_DIGIT_ANY);
			res = tris_say_character_str(chan, ext, TRIS_DIGIT_ANY, chan->language);
		}
	}

	return res;
}

static int select_entry(struct tris_channel *chan, const char *dialcontext, const struct directory_item *item, struct tris_flags *flags)
{
	tris_debug(1, "Selecting '%s' - %s@%s\n", item->name, item->exten, S_OR(dialcontext, item->context));

	if (tris_test_flag(flags, OPT_FROMVOICEMAIL)) {
		/* We still want to set the exten though */
		tris_copy_string(chan->exten, item->exten, sizeof(chan->exten));
	} else if (tris_goto_if_exists(chan, S_OR(dialcontext, item->context), item->exten, 1)) {
		tris_log(LOG_WARNING,
			"Can't find extension '%s' in context '%s'.  "
			"Did you pass the wrong context to Directory?\n",
			item->exten, S_OR(dialcontext, item->context));
		return -1;
	}

	return 0;
}

static int select_item_seq(struct tris_channel *chan, struct directory_item **items, int count, const char *dialcontext, struct tris_flags *flags)
{
	struct directory_item *item, **ptr;
	int i, res, loop;

	for (ptr = items, i = 0; i < count; i++, ptr++) {
		item = *ptr;

		for (loop = 3 ; loop > 0; loop--) {
			res = play_mailbox_owner(chan, item->context, item->exten, item->name, flags);

			if (!res)
				res = tris_stream_and_wait(chan, "dir-instr", TRIS_DIGIT_ANY);
			if (!res)
				res = tris_waitfordigit(chan, 3000);
			tris_stopstream(chan);
	
			if (res == '1') { /* Name selected */
				return select_entry(chan, dialcontext, item, flags) ? -1 : 1;
			} else if (res == '*') {
				/* Skip to next match in list */
				break;
			}

			if (res < 0)
				return -1;

			res = 0;
		}
	}

	/* Nothing was selected */
	return 0;
}

static int select_item_menu(struct tris_channel *chan, struct directory_item **items, int count, const char *dialcontext, struct tris_flags *flags)
{
	struct directory_item **block, *item;
	int i, limit, res = 0;
	char buf[9];

	for (block = items; count; block += limit, count -= limit) {
		limit = count;
		if (limit > 8)
			limit = 8;

		for (i = 0; i < limit && !res; i++) {
			item = block[i];

			snprintf(buf, sizeof(buf), "digits/%d", i + 1);
			/* Press <num> for <name>, [ extension <ext> ] */
			res = tris_streamfile(chan, "dir-multi1", chan->language);
			if (!res)
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
			if (!res)
				res = tris_streamfile(chan, buf, chan->language);
			if (!res)
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
			if (!res)
				res = tris_streamfile(chan, "dir-multi2", chan->language);
			if (!res)
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
			if (!res)
				res = play_mailbox_owner(chan, item->context, item->exten, item->name, flags);
			if (!res)
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
			if (!res)
				res = tris_waitfordigit(chan, 800);
		}

		/* Press "9" for more names. */
		if (!res && count > limit) {
			res = tris_streamfile(chan, "dir-multi9", chan->language);
			if (!res)
				res = tris_waitstream(chan, TRIS_DIGIT_ANY);
		}

		if (!res) {
			res = tris_waitfordigit(chan, 3000);
		}

		if (res && res > '0' && res < '1' + limit) {
			return select_entry(chan, dialcontext, block[res - '1'], flags) ? -1 : 1;
		}

		if (res < 0)
			return -1;

		res = 0;
	}

	/* Nothing was selected */
	return 0;
}

static struct tris_config *realtime_directory(char *context)
{
	struct tris_config *cfg;
	struct tris_config *rtdata;
	struct tris_category *cat;
	struct tris_variable *var;
	char *mailbox;
	const char *fullname;
	const char *hidefromdir, *searchcontexts = NULL;
	char tmp[100];
	struct tris_flags config_flags = { 0 };

	/* Load flat file config. */
	cfg = tris_config_load(VOICEMAIL_CONFIG, config_flags);

	if (!cfg) {
		/* Loading config failed. */
		tris_log(LOG_WARNING, "Loading config failed.\n");
		return NULL;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file %s is in an invalid format.  Aborting.\n", VOICEMAIL_CONFIG);
		return NULL;
	}

	/* Get realtime entries, categorized by their mailbox number
	   and present in the requested context */
	if (tris_strlen_zero(context) && (searchcontexts = tris_variable_retrieve(cfg, "general", "searchcontexts"))) {
		if (tris_true(searchcontexts)) {
			rtdata = tris_load_realtime_multientry("voicemail", "mailbox LIKE", "%", SENTINEL);
			context = NULL;
		} else {
			rtdata = tris_load_realtime_multientry("voicemail", "mailbox LIKE", "%", "context", "default", SENTINEL);
			context = "default";
		}
	} else {
		rtdata = tris_load_realtime_multientry("voicemail", "mailbox LIKE", "%", "context", context, SENTINEL);
	}

	/* if there are no results, just return the entries from the config file */
	if (!rtdata) {
		return cfg;
	}

	mailbox = NULL;
	while ( (mailbox = tris_category_browse(rtdata, mailbox)) ) {
		const char *context = tris_variable_retrieve(rtdata, mailbox, "context");

		fullname = tris_variable_retrieve(rtdata, mailbox, "fullname");
		if (tris_true((hidefromdir = tris_variable_retrieve(rtdata, mailbox, "hidefromdir")))) {
			/* Skip hidden */
			continue;
		}
		snprintf(tmp, sizeof(tmp), "no-password,%s", S_OR(fullname, ""));

		/* Does the context exist within the config file? If not, make one */
		if (!(cat = tris_category_get(cfg, context))) {
			if (!(cat = tris_category_new(context, "", 99999))) {
				tris_log(LOG_WARNING, "Out of memory\n");
				tris_config_destroy(cfg);
				if (rtdata) {
					tris_config_destroy(rtdata);
				}
				return NULL;
			}
			tris_category_append(cfg, cat);
		}

		if ((var = tris_variable_new(mailbox, tmp, ""))) {
			tris_variable_append(cat, var);
		} else {
			tris_log(LOG_WARNING, "Out of memory adding mailbox '%s'\n", mailbox);
		}
	}
	tris_config_destroy(rtdata);

	return cfg;
}

static int check_match(struct directory_item **result, const char *item_context, const char *item_fullname, const char *item_ext, const char *pattern_ext, int use_first_name)
{
	struct directory_item *item;
	const char *key = NULL;
	int namelen;

	if (tris_strlen_zero(item_fullname)) {
		return 0;
	}

	/* Set key to last name or first name depending on search mode */
	if (!use_first_name)
		key = strchr(item_fullname, ' ');

	if (key)
		key++;
	else
		key = item_fullname;

	if (compare(key, pattern_ext))
		return 0;

	tris_debug(1, "Found match %s@%s\n", item_ext, item_context);

	/* Match */
	item = tris_calloc(1, sizeof(*item));
	if (!item)
		return -1;
	tris_copy_string(item->context, item_context, sizeof(item->context));
	tris_copy_string(item->name, item_fullname, sizeof(item->name));
	tris_copy_string(item->exten, item_ext, sizeof(item->exten));

	tris_copy_string(item->key, key, sizeof(item->key));
	if (key != item_fullname) {
		/* Key is the last name. Append first name to key in order to sort Last,First */
		namelen = key - item_fullname - 1;
		if (namelen > sizeof(item->key) - strlen(item->key) - 1)
			namelen = sizeof(item->key) - strlen(item->key) - 1;
		strncat(item->key, item_fullname, namelen);
	}

	*result = item;
	return 1;
}

typedef TRIS_LIST_HEAD_NOLOCK(, directory_item) itemlist;

static int search_directory_sub(const char *context, struct tris_config *vmcfg, struct tris_config *ucfg, const char *ext, struct tris_flags flags, itemlist *alist)
{
	struct tris_variable *v;
	char buf[TRIS_MAX_EXTENSION + 1], *pos, *bufptr, *cat;
	struct directory_item *item;
	int res;

	tris_debug(2, "Pattern: %s\n", ext);

	for (v = tris_variable_browse(vmcfg, context); v; v = v->next) {

		/* Ignore hidden */
		if (strcasestr(v->value, "hidefromdir=yes"))
			continue;

		tris_copy_string(buf, v->value, sizeof(buf));
		bufptr = buf;

		/* password,Full Name,email,pager,options */
		strsep(&bufptr, ",");
		pos = strsep(&bufptr, ",");

		/* No name to compare against */
		if (tris_strlen_zero(pos)) {
			continue;
		}

		res = 0;
		if (tris_test_flag(&flags, OPT_LISTBYLASTNAME)) {
			res = check_match(&item, context, pos, v->name, ext, 0 /* use_first_name */);
		}
		if (!res && tris_test_flag(&flags, OPT_LISTBYFIRSTNAME)) {
			res = check_match(&item, context, pos, v->name, ext, 1 /* use_first_name */);
		}

		if (!res)
			continue;
		else if (res < 0)
			return -1;

		TRIS_LIST_INSERT_TAIL(alist, item, entry);
	}

	if (ucfg) {
		for (cat = tris_category_browse(ucfg, NULL); cat ; cat = tris_category_browse(ucfg, cat)) {
			const char *position;
			if (!strcasecmp(cat, "general"))
				continue;
			if (!tris_true(tris_config_option(ucfg, cat, "hasdirectory")))
				continue;

			/* Find all candidate extensions */
			position = tris_variable_retrieve(ucfg, cat, "fullname");
			if (!position)
				continue;

			res = 0;
			if (tris_test_flag(&flags, OPT_LISTBYLASTNAME)) {
				res = check_match(&item, context, position, cat, ext, 0 /* use_first_name */);
			}
			if (!res && tris_test_flag(&flags, OPT_LISTBYFIRSTNAME)) {
				res = check_match(&item, context, position, cat, ext, 1 /* use_first_name */);
			}

			if (!res)
				continue;
			else if (res < 0)
				return -1;

			TRIS_LIST_INSERT_TAIL(alist, item, entry);
		}
	}
	return 0;
}

static int search_directory(const char *context, struct tris_config *vmcfg, struct tris_config *ucfg, const char *ext, struct tris_flags flags, itemlist *alist)
{
	const char *searchcontexts = tris_variable_retrieve(vmcfg, "general", "searchcontexts");
	if (tris_strlen_zero(context)) {
		if (!tris_strlen_zero(searchcontexts) && tris_true(searchcontexts)) {
			/* Browse each context for a match */
			int res;
			const char *catg;
			for (catg = tris_category_browse(vmcfg, NULL); catg; catg = tris_category_browse(vmcfg, catg)) {
				if (!strcmp(catg, "general") || !strcmp(catg, "zonemessages")) {
					continue;
				}

				if ((res = search_directory_sub(catg, vmcfg, ucfg, ext, flags, alist))) {
					return res;
				}
			}
			return 0;
		} else {
			tris_debug(1, "Searching by category default\n");
			return search_directory_sub("default", vmcfg, ucfg, ext, flags, alist);
		}
	} else {
		/* Browse only the listed context for a match */
		tris_debug(1, "Searching by category %s\n", context);
		return search_directory_sub(context, vmcfg, ucfg, ext, flags, alist);
	}
}

static void sort_items(struct directory_item **sorted, int count)
{
	int reordered, i;
	struct directory_item **ptr, *tmp;

	if (count < 2)
		return;

	/* Bubble-sort items by the key */
	do {
		reordered = 0;
		for (ptr = sorted, i = 0; i < count - 1; i++, ptr++) {
			if (strcasecmp(ptr[0]->key, ptr[1]->key) > 0) {
				tmp = ptr[0];
				ptr[0] = ptr[1];
				ptr[1] = tmp;
				reordered++;
			}
		}
	} while (reordered);
}

static int goto_exten(struct tris_channel *chan, const char *dialcontext, char *ext)
{
	if (!tris_goto_if_exists(chan, dialcontext, ext, 1) ||
		(!tris_strlen_zero(chan->macrocontext) &&
		!tris_goto_if_exists(chan, chan->macrocontext, ext, 1))) {
		return 0;
	} else {
		tris_log(LOG_WARNING, "Can't find extension '%s' in current context.  "
			"Not Exiting the Directory!\n", ext);
		return -1;
	}
}

static int do_directory(struct tris_channel *chan, struct tris_config *vmcfg, struct tris_config *ucfg, char *context, char *dialcontext, char digit, int digits, struct tris_flags *flags)
{
	/* Read in the first three digits..  "digit" is the first digit, already read */
	int res = 0;
	itemlist alist = TRIS_LIST_HEAD_NOLOCK_INIT_VALUE;
	struct directory_item *item, **ptr, **sorted = NULL;
	int count, i;
	char ext[10] = "";

	if (digit == '0' && !goto_exten(chan, S_OR(dialcontext, "default"), "o")) {
		return digit;
	}

	if (digit == '*' && !goto_exten(chan, S_OR(dialcontext, "default"), "a")) {
		return digit;
	}

	ext[0] = digit;
	if (tris_readstring(chan, ext + 1, digits - 1, 3000, 3000, "#") < 0)
		return -1;

	res = search_directory(context, vmcfg, ucfg, ext, *flags, &alist);
	if (res)
		goto exit;

	/* Count items in the list */
	count = 0;
	TRIS_LIST_TRAVERSE(&alist, item, entry) {
		count++;
	}

	if (count < 1) {
		res = tris_streamfile(chan, "dir-nomatch", chan->language);
		goto exit;
	}


	/* Create plain array of pointers to items (for sorting) */
	sorted = tris_calloc(count, sizeof(*sorted));

	ptr = sorted;
	TRIS_LIST_TRAVERSE(&alist, item, entry) {
		*ptr++ = item;
	}

	/* Sort items */
	sort_items(sorted, count);

	if (option_debug) {
		tris_debug(2, "Listing matching entries:\n");
		for (ptr = sorted, i = 0; i < count; i++, ptr++) {
			tris_debug(2, "%s: %s\n", ptr[0]->exten, ptr[0]->name);
		}
	}

	if (tris_test_flag(flags, OPT_SELECTFROMMENU)) {
		/* Offer multiple entries at the same time */
		res = select_item_menu(chan, sorted, count, dialcontext, flags);
	} else {
		/* Offer entries one by one */
		res = select_item_seq(chan, sorted, count, dialcontext, flags);
	}

	if (!res) {
		res = tris_streamfile(chan, "dir-nomore", chan->language);
	}

exit:
	if (sorted)
		tris_free(sorted);

	while ((item = TRIS_LIST_REMOVE_HEAD(&alist, entry)))
		tris_free(item);

	return res;
}

static int directory_exec(struct tris_channel *chan, void *data)
{
	int res = 0, digit = 3;
	struct tris_config *cfg, *ucfg;
	const char *dirintro;
	char *parse, *opts[OPT_ARG_ARRAY_SIZE] = { 0, };
	struct tris_flags flags = { 0 };
	struct tris_flags config_flags = { 0 };
	enum { FIRST, LAST, BOTH } which = LAST;
	char digits[9] = "digits/3";
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(vmcontext);
		TRIS_APP_ARG(dialcontext);
		TRIS_APP_ARG(options);
	);

	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (args.options && tris_app_parse_options(directory_app_options, &flags, opts, args.options))
		return -1;

	if (!(cfg = realtime_directory(args.vmcontext))) {
		tris_log(LOG_ERROR, "Unable to read the configuration data!\n");
		return -1;
	}

	if ((ucfg = tris_config_load("users.conf", config_flags)) == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file users.conf is in an invalid format.  Aborting.\n");
		ucfg = NULL;
	}

	dirintro = tris_variable_retrieve(cfg, args.vmcontext, "directoryintro");
	if (tris_strlen_zero(dirintro))
		dirintro = tris_variable_retrieve(cfg, "general", "directoryintro");

	if (tris_test_flag(&flags, OPT_LISTBYFIRSTNAME) && tris_test_flag(&flags, OPT_LISTBYLASTNAME)) {
		if (!tris_strlen_zero(opts[OPT_ARG_EITHER])) {
			digit = atoi(opts[OPT_ARG_EITHER]);
		}
		which = BOTH;
	} else if (tris_test_flag(&flags, OPT_LISTBYFIRSTNAME)) {
		if (!tris_strlen_zero(opts[OPT_ARG_FIRSTNAME])) {
			digit = atoi(opts[OPT_ARG_FIRSTNAME]);
		}
		which = FIRST;
	} else {
		if (!tris_strlen_zero(opts[OPT_ARG_LASTNAME])) {
			digit = atoi(opts[OPT_ARG_LASTNAME]);
		}
		which = LAST;
	}

	/* If no options specified, search by last name */
	if (!tris_test_flag(&flags, OPT_LISTBYFIRSTNAME) && !tris_test_flag(&flags, OPT_LISTBYLASTNAME)) {
		tris_set_flag(&flags, OPT_LISTBYLASTNAME);
		which = LAST;
	}

	if (digit > 9) {
		digit = 9;
	} else if (digit < 1) {
		digit = 3;
	}
	digits[7] = digit + '0';

	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);

	for (;;) {
		if (!tris_strlen_zero(dirintro) && !res) {
			res = tris_stream_and_wait(chan, dirintro, TRIS_DIGIT_ANY);
		} else if (!res) {
			/* Stop playing sounds as soon as we have a digit. */
			res = tris_stream_and_wait(chan, "dir-welcome", TRIS_DIGIT_ANY);
			if (!res) {
				res = tris_stream_and_wait(chan, "dir-pls-enter", TRIS_DIGIT_ANY);
			}
			if (!res) {
				res = tris_stream_and_wait(chan, digits, TRIS_DIGIT_ANY);
			}
			if (!res) {
				res = tris_stream_and_wait(chan, 
					which == FIRST ? "dir-first" :
					which == LAST ? "dir-last" :
					"dir-firstlast", TRIS_DIGIT_ANY);
			}
			if (!res) {
				res = tris_stream_and_wait(chan, "dir-usingkeypad", TRIS_DIGIT_ANY);
			}
		}
		tris_stopstream(chan);
		if (!res)
			res = tris_waitfordigit(chan, 5000);

		if (res <= 0)
			break;

		res = do_directory(chan, cfg, ucfg, args.vmcontext, args.dialcontext, res, digit, &flags);
		if (res)
			break;

		res = tris_waitstream(chan, TRIS_DIGIT_ANY);
		tris_stopstream(chan);

		if (res)
			break;
	}

	if (ucfg)
		tris_config_destroy(ucfg);
	tris_config_destroy(cfg);

	return res < 0 ? -1 : 0;
}

static int unload_module(void)
{
	int res;
	res = tris_unregister_application(app);
	return res;
}

static int load_module(void)
{
	return tris_register_application_xml(app, directory_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Extension Directory");
