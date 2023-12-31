/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Environment related dialplan functions
 * 
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 182278 $")

#include <sys/stat.h>

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"

/*** DOCUMENTATION
	<function name="ENV" language="en_US">
		<synopsis>
			Gets or sets the environment variable specified.
		</synopsis>
		<syntax>
			<parameter name="varname" required="true">
				<para>Environment variable name</para>
			</parameter>
		</syntax>
		<description>
		</description>
	</function>
	<function name="STAT" language="en_US">
		<synopsis>
			Does a check on the specified file.
		</synopsis>
		<syntax>
			<parameter name="flag" required="true">
				<para>Flag may be one of the following:</para>
				<para>d - Checks if the file is a directory.</para>
				<para>e - Checks if the file exists.</para>
				<para>f - Checks if the file is a regular file.</para>
				<para>m - Returns the file mode (in octal)</para>
				<para>s - Returns the size (in bytes) of the file</para>
				<para>A - Returns the epoch at which the file was last accessed.</para>
				<para>C - Returns the epoch at which the inode was last changed.</para>
				<para>M - Returns the epoch at which the file was last modified.</para>
			</parameter>
			<parameter name="filename" required="true" />
		</syntax>
		<description>
		</description>
	</function>
	<function name="FILE" language="en_US">
		<synopsis>
			Obtains the contents of a file.
		</synopsis>
		<syntax>
			<parameter name="filename" required="true" />
			<parameter name="offset" required="true">
				<para>Maybe specified as any number. If negative, <replaceable>offset</replaceable> specifies the number
				of bytes back from the end of the file.</para>
			</parameter>
			<parameter name="length" required="true">
				<para>If specified, will limit the length of the data read to that size. If negative,
				trims <replaceable>length</replaceable> bytes from the end of the file.</para>
			</parameter>
		</syntax>
		<description>
		</description>
	</function>
 ***/

static int env_read(struct tris_channel *chan, const char *cmd, char *data,
		    char *buf, size_t len)
{
	char *ret = NULL;

	*buf = '\0';

	if (data)
		ret = getenv(data);

	if (ret)
		tris_copy_string(buf, ret, len);

	return 0;
}

static int env_write(struct tris_channel *chan, const char *cmd, char *data,
		     const char *value)
{
	if (!tris_strlen_zero(data)) {
		if (!tris_strlen_zero(value)) {
			setenv(data, value, 1);
		} else {
			unsetenv(data);
		}
	}

	return 0;
}

static int stat_read(struct tris_channel *chan, const char *cmd, char *data,
		     char *buf, size_t len)
{
	char *action;
	struct stat s;

	tris_copy_string(buf, "0", len);

	action = strsep(&data, ",");
	if (stat(data, &s)) {
		return 0;
	} else {
		switch (*action) {
		case 'e':
			strcpy(buf, "1");
			break;
		case 's':
			snprintf(buf, len, "%d", (unsigned int) s.st_size);
			break;
		case 'f':
			snprintf(buf, len, "%d", S_ISREG(s.st_mode) ? 1 : 0);
			break;
		case 'd':
			snprintf(buf, len, "%d", S_ISDIR(s.st_mode) ? 1 : 0);
			break;
		case 'M':
			snprintf(buf, len, "%d", (int) s.st_mtime);
			break;
		case 'A':
			snprintf(buf, len, "%d", (int) s.st_mtime);
			break;
		case 'C':
			snprintf(buf, len, "%d", (int) s.st_ctime);
			break;
		case 'm':
			snprintf(buf, len, "%o", (int) s.st_mode);
			break;
		}
	}

	return 0;
}

static int file_read(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(filename);
		TRIS_APP_ARG(offset);
		TRIS_APP_ARG(length);
	);
	int offset = 0, length, res = 0;
	char *contents;
	size_t contents_len;

	TRIS_STANDARD_APP_ARGS(args, data);
	if (args.argc > 1) {
		offset = atoi(args.offset);
	}

	if (args.argc > 2) {
		/* The +1/-1 in this code section is to accomodate for the terminating NULL. */
		if ((length = atoi(args.length) + 1) > len) {
			tris_log(LOG_WARNING, "Length %d is greater than the max (%d).  Truncating output.\n", length - 1, (int)len - 1);
			length = len;
		}
	} else {
		length = len;
	}

	if (!(contents = tris_read_textfile(args.filename))) {
		return -1;
	}

	do {
		contents_len = strlen(contents);
		if (offset > contents_len) {
			res = -1;
			break;
		}

		if (offset >= 0) {
			if (length < 0) {
				if (contents_len - offset + length < 0) {
					/* Nothing left after trimming */
					res = -1;
					break;
				}
				tris_copy_string(buf, &contents[offset], contents_len + length);
			} else {
				tris_copy_string(buf, &contents[offset], length);
			}
		} else {
			if (offset * -1 > contents_len) {
				tris_log(LOG_WARNING, "Offset is larger than the file size.\n");
				offset = contents_len * -1;
			}
			tris_copy_string(buf, &contents[contents_len + offset], length);
		}
	} while (0);

	tris_free(contents);

	return res;
}

static struct tris_custom_function env_function = {
	.name = "ENV",
	.read = env_read,
	.write = env_write
};

static struct tris_custom_function stat_function = {
	.name = "STAT",
	.read = stat_read
};

static struct tris_custom_function file_function = {
	.name = "FILE",
	.read = file_read
	/*
	 * Some enterprising programmer could probably add write functionality
	 * to FILE(), although I'm not sure how useful it would be.  Hence why
	 * it's called FILE and not READFILE (like the app was).
	 */
};

static int unload_module(void)
{
	int res = 0;

	res |= tris_custom_function_unregister(&env_function);
	res |= tris_custom_function_unregister(&stat_function);
	res |= tris_custom_function_unregister(&file_function);

	return res;
}

static int load_module(void)
{
	int res = 0;

	res |= tris_custom_function_register(&env_function);
	res |= tris_custom_function_register(&stat_function);
	res |= tris_custom_function_register(&file_function);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Environment/filesystem dialplan functions");
