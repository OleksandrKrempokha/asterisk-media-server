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
 * \brief Image Management
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 105840 $")

#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_DATA_DIR */
#include "trismedia/sched.h"
#include "trismedia/channel.h"
#include "trismedia/file.h"
#include "trismedia/image.h"
#include "trismedia/translate.h"
#include "trismedia/cli.h"
#include "trismedia/lock.h"

/* XXX Why don't we just use the formats struct for this? */
static TRIS_RWLIST_HEAD_STATIC(imagers, tris_imager);

int tris_image_register(struct tris_imager *img)
{
	TRIS_RWLIST_WRLOCK(&imagers);
	TRIS_RWLIST_INSERT_HEAD(&imagers, img, list);
	TRIS_RWLIST_UNLOCK(&imagers);
	tris_verb(2, "Registered format '%s' (%s)\n", img->name, img->desc);
	return 0;
}

void tris_image_unregister(struct tris_imager *img)
{
	TRIS_RWLIST_WRLOCK(&imagers);
	img = TRIS_RWLIST_REMOVE(&imagers, img, list);
	TRIS_RWLIST_UNLOCK(&imagers);

	if (img)
		tris_verb(2, "Unregistered format '%s' (%s)\n", img->name, img->desc);
}

int tris_supports_images(struct tris_channel *chan)
{
	if (!chan || !chan->tech)
		return 0;
	if (!chan->tech->send_image)
		return 0;
	return 1;
}

static int file_exists(char *filename)
{
	int res;
	struct stat st;
	res = stat(filename, &st);
	if (!res)
		return st.st_size;
	return 0;
}

static void make_filename(char *buf, int len, char *filename, const char *preflang, char *ext)
{
	if (filename[0] == '/') {
		if (!tris_strlen_zero(preflang))
			snprintf(buf, len, "%s-%s.%s", filename, preflang, ext);
		else
			snprintf(buf, len, "%s.%s", filename, ext);
	} else {
		if (!tris_strlen_zero(preflang))
			snprintf(buf, len, "%s/%s/%s-%s.%s", tris_config_TRIS_DATA_DIR, "images", filename, preflang, ext);
		else
			snprintf(buf, len, "%s/%s/%s.%s", tris_config_TRIS_DATA_DIR, "images", filename, ext);
	}
}

struct tris_frame *tris_read_image(char *filename, const char *preflang, int format)
{
	struct tris_imager *i;
	char buf[256];
	char tmp[80];
	char *e;
	struct tris_imager *found = NULL;
	int fd;
	int len=0;
	struct tris_frame *f = NULL;
	
	TRIS_RWLIST_RDLOCK(&imagers);
	TRIS_RWLIST_TRAVERSE(&imagers, i, list) {
		if (i->format & format) {
			char *stringp=NULL;
			tris_copy_string(tmp, i->exts, sizeof(tmp));
			stringp = tmp;
			e = strsep(&stringp, "|");
			while (e) {
				make_filename(buf, sizeof(buf), filename, preflang, e);
				if ((len = file_exists(buf))) {
					found = i;
					break;
				}
				make_filename(buf, sizeof(buf), filename, NULL, e);
				if ((len = file_exists(buf))) {
					found = i;
					break;
				}
				e = strsep(&stringp, "|");
			}
		}
		if (found)
			break;	
	}

	if (found) {
		fd = open(buf, O_RDONLY);
		if (fd > -1) {
			if (!found->identify || found->identify(fd)) {
				/* Reset file pointer */
				lseek(fd, 0, SEEK_SET);
				f = found->read_image(fd, len); 
			} else
				tris_log(LOG_WARNING, "%s does not appear to be a %s file\n", buf, found->name);
			close(fd);
		} else
			tris_log(LOG_WARNING, "Unable to open '%s': %s\n", buf, strerror(errno));
	} else
		tris_log(LOG_WARNING, "Image file '%s' not found\n", filename);
	
	TRIS_RWLIST_UNLOCK(&imagers);
	
	return f;
}

int tris_send_image(struct tris_channel *chan, char *filename)
{
	struct tris_frame *f;
	int res = -1;
	if (chan->tech->send_image) {
		f = tris_read_image(filename, chan->language, -1);
		if (f) {
			res = chan->tech->send_image(chan, f);
			tris_frfree(f);
		}
	}
	return res;
}

static char *handle_core_show_image_formats(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define FORMAT "%10s %10s %50s %10s\n"
#define FORMAT2 "%10s %10s %50s %10s\n"
	struct tris_imager *i;
	int count_fmt = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show image formats";
		e->usage =
			"Usage: core show image formats\n"
			"       Displays currently registered image formats (if any).\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != 4)
		return CLI_SHOWUSAGE;
	tris_cli(a->fd, FORMAT, "Name", "Extensions", "Description", "Format");
	tris_cli(a->fd, FORMAT, "----", "----------", "-----------", "------");
	TRIS_RWLIST_RDLOCK(&imagers);
	TRIS_RWLIST_TRAVERSE(&imagers, i, list) {
		tris_cli(a->fd, FORMAT2, i->name, i->exts, i->desc, tris_getformatname(i->format));
		count_fmt++;
	}
	TRIS_RWLIST_UNLOCK(&imagers);
	tris_cli(a->fd, "\n%d image format%s registered.\n", count_fmt, count_fmt == 1 ? "" : "s");
	return CLI_SUCCESS;
}

struct tris_cli_entry cli_image[] = {
	TRIS_CLI_DEFINE(handle_core_show_image_formats, "Displays image formats")
};

int tris_image_init(void)
{
	tris_cli_register_multiple(cli_image, ARRAY_LEN(cli_image));
	return 0;
}
