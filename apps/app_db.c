/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 * Copyright (C) 2003, Jefferson Noxon
 *
 * Mark Spencer <markster@digium.com>
 * Jefferson Noxon <jeff@debian.org>
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
 * \brief Database access functions
 *
 * \author Mark Spencer <markster@digium.com>
 * \author Jefferson Noxon <jeff@debian.org>
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 154542 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/astdb.h"
#include "trismedia/lock.h"

/*** DOCUMENTATION
	<application name="DBdel" language="en_US">
		<synopsis>
			Delete a key from the trismedia database.
		</synopsis>
		<syntax argsep="/">
			<parameter name="family" required="true" />
			<parameter name="key" required="true" />
		</syntax>
		<description>
			<para>This application will delete a <replaceable>key</replaceable> from the Trismedia
			database.</para>
			<note><para>This application has been DEPRECATED in favor of the DB_DELETE function.</para></note>
		</description>
		<see-also>
			<ref type="function">DB_DELETE</ref>
			<ref type="application">DBdeltree</ref>
			<ref type="function">DB</ref>
		</see-also>
	</application>
	<application name="DBdeltree" language="en_US">
		<synopsis>
			Delete a family or keytree from the trismedia database.
		</synopsis>
		<syntax argsep="/">
			<parameter name="family" required="true" />
			<parameter name="keytree" />
		</syntax>
		<description>
			<para>This application will delete a <replaceable>family</replaceable> or <replaceable>keytree</replaceable>
			from the Trismedia database.</para>
		</description>
		<see-also>
			<ref type="function">DB_DELETE</ref>
			<ref type="application">DBdel</ref>
			<ref type="function">DB</ref>
		</see-also>
	</application>
 ***/

/*! \todo XXX Remove this application after 1.4 is relased */
static char *d_app = "DBdel";
static char *dt_app = "DBdeltree";

static int deltree_exec(struct tris_channel *chan, void *data)
{
	char *argv, *family, *keytree;

	argv = tris_strdupa(data);

	if (strchr(argv, '/')) {
		family = strsep(&argv, "/");
		keytree = strsep(&argv, "\0");
		if (!family || !keytree) {
			tris_debug(1, "Ignoring; Syntax error in argument\n");
			return 0;
		}
		if (tris_strlen_zero(keytree))
			keytree = 0;
	} else {
		family = argv;
		keytree = 0;
	}

	if (keytree)
		tris_verb(3, "DBdeltree: family=%s, keytree=%s\n", family, keytree);
	else
		tris_verb(3, "DBdeltree: family=%s\n", family);

	if (tris_db_deltree(family, keytree))
		tris_verb(3, "DBdeltree: Error deleting key from database.\n");

	return 0;
}

static int del_exec(struct tris_channel *chan, void *data)
{
	char *argv, *family, *key;
	static int deprecation_warning = 0;

	if (!deprecation_warning) {
		deprecation_warning = 1;
		tris_log(LOG_WARNING, "The DBdel application has been deprecated in favor of the DB_DELETE dialplan function!\n");
	}

	argv = tris_strdupa(data);

	if (strchr(argv, '/')) {
		family = strsep(&argv, "/");
		key = strsep(&argv, "\0");
		if (!family || !key) {
			tris_debug(1, "Ignoring; Syntax error in argument\n");
			return 0;
		}
		tris_verb(3, "DBdel: family=%s, key=%s\n", family, key);
		if (tris_db_del(family, key))
			tris_verb(3, "DBdel: Error deleting key from database.\n");
	} else {
		tris_debug(1, "Ignoring, no parameters\n");
	}

	return 0;
}

static int unload_module(void)
{
	int retval;

	retval = tris_unregister_application(dt_app);
	retval |= tris_unregister_application(d_app);

	return retval;
}

static int load_module(void)
{
	int retval;

	retval = tris_register_application_xml(d_app, del_exec);
	retval |= tris_register_application_xml(dt_app, deltree_exec);

	return retval;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Database Access Functions");
