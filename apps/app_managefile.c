/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Matthew Fredrickson <creslin@digium.com>
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
 * \brief Trivial application to record a sound file
 *
 * \author Matthew Fredrickson <creslin@digium.com>
 *
 * \ingroup applications
 */
 
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include "trismedia/file.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/app.h"
#include "trismedia/channel.h"
#include "trismedia/dsp.h"	/* use dsp routines for silence detection */
#include "trismedia/res_odbc.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


static char *app = "ManageFile";

static int managefile_exec(struct tris_channel *chan, void *data)
{
	int cmd = 0, retry = 0;
	char filename[256], path[256], sql[256], result[80];
	int fd;
	char *exten;
	if(tris_strlen_zero(data))	{
		return -1;
	}
	exten = tris_strdupa(data);
	snprintf(sql, sizeof(sql), "SELECT extension FROM managefile WHERE extension='%s'", exten);
	sql_select_query_execute(result, sql);
	if(strcmp(exten, result)) {
		return -1;
	}
	while(retry < 3) {
		switch(cmd) {
		case '1':
			tris_app_getdata(chan, "managefile/dial_filename", filename, sizeof(filename) - 1, 7000);
			if(tris_strlen_zero(filename)) {
				goto end_exec;
			}
			snprintf(path, sizeof(path), "/home/%s", filename);
			fd = open(path, O_CREAT|O_WRONLY, 0777);
			if(fd < 0) {
				tris_play_and_wait(chan, "managefile/cant_create");
			} else {
				tris_play_and_wait(chan, "managefile/file_created");
				close(fd);
			}
			goto end_exec;
		case '2':
			tris_app_getdata(chan, "managefile/dial_filename", filename, sizeof(filename) - 1, 7000);
			if(tris_strlen_zero(filename)) {
				goto end_exec;
			}
			snprintf(path, sizeof(path), "/home/%s", filename);
			if(unlink(path)) {
				if(errno == ENOENT) {
					tris_play_and_wait(chan, "managefile/no_such_file");
				} else {
					tris_play_and_wait(chan, "managefile/cant_delete");
				}
			} else {
				tris_play_and_wait(chan, "managefile/file_deleted");
			}
			goto end_exec;
		case '*':
			goto end_exec;
		}
		retry++;
		if(retry >= 3) 
			break;
		if(!cmd) {
			cmd = tris_play_and_wait(chan, "managefile/main_menu");
		} else {
			cmd = tris_play_and_wait(chan, "managefile/invalid_entry_try_again");
		}
		if(!cmd)
			cmd = tris_waitfordigit(chan, 5000);
	}

end_exec:
	tris_play_and_wait(chan, "managefile/bye");
	return 0;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	return tris_register_application_xml(app, managefile_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Trivial Record Application");
