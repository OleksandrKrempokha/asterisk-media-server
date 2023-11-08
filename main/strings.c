/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Digium, Inc.
 *
 * Tilghman Lesher <tlesher@digium.com>
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
 * \brief String manipulation API
 *
 * \author Tilghman Lesher <tilghman@digium.com>
 */

/*** MAKEOPTS
<category name="MENUSELECT_CFLAGS" displayname="Compiler Flags" positive_output="yes" remove_on_change=".lastclean">
	<member name="DEBUG_OPAQUE" displayname="Change tris_str internals to detect improper usage">
		<defaultenabled>yes</defaultenabled>
	</member>
</category>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 247337 $")

#include "trismedia/strings.h"
#include "trismedia/pbx.h"

/*!
 * core handler for dynamic strings.
 * This is not meant to be called directly, but rather through the
 * various wrapper macros
 *	tris_str_set(...)
 *	tris_str_append(...)
 *	tris_str_set_va(...)
 *	tris_str_append_va(...)
 */

#if (defined(MALLOC_DEBUG) && !defined(STANDALONE))
int __tris_debug_str_helper(struct tris_str **buf, ssize_t max_len,
	int append, const char *fmt, va_list ap, const char *file, int lineno, const char *function)
#else
int __tris_str_helper(struct tris_str **buf, ssize_t max_len,
	int append, const char *fmt, va_list ap)
#endif
{
	int res, need;
	int offset = (append && (*buf)->__TRIS_STR_LEN) ? (*buf)->__TRIS_STR_USED : 0;
	va_list aq;

	do {
		if (max_len < 0) {
			max_len = (*buf)->__TRIS_STR_LEN;	/* don't exceed the allocated space */
		}
		/*
		 * Ask vsnprintf how much space we need. Remember that vsnprintf
		 * does not count the final <code>'\0'</code> so we must add 1.
		 */
		va_copy(aq, ap);
		res = vsnprintf((*buf)->__TRIS_STR_STR + offset, (*buf)->__TRIS_STR_LEN - offset, fmt, aq);

		need = res + offset + 1;
		/*
		 * If there is not enough space and we are below the max length,
		 * reallocate the buffer and return a message telling to retry.
		 */
		if (need > (*buf)->__TRIS_STR_LEN && (max_len == 0 || (*buf)->__TRIS_STR_LEN < max_len) ) {
			if (max_len && max_len < need) {	/* truncate as needed */
				need = max_len;
			} else if (max_len == 0) {	/* if unbounded, give more room for next time */
				need += 16 + need / 4;
			}
			if (0) {	/* debugging */
				tris_verbose("extend from %d to %d\n", (int)(*buf)->__TRIS_STR_LEN, need);
			}
			if (
#if (defined(MALLOC_DEBUG) && !defined(STANDALONE))
					_tris_str_make_space(buf, need, file, lineno, function)
#else
					tris_str_make_space(buf, need)
#endif
				) {
				tris_verbose("failed to extend from %d to %d\n", (int)(*buf)->__TRIS_STR_LEN, need);
				va_end(aq);
				return TRIS_DYNSTR_BUILD_FAILED;
			}
			(*buf)->__TRIS_STR_STR[offset] = '\0';	/* Truncate the partial write. */

			/* Restart va_copy before calling vsnprintf() again. */
			va_end(aq);
			continue;
		}
		va_end(aq);
		break;
	} while (1);
	/* update space used, keep in mind the truncation */
	(*buf)->__TRIS_STR_USED = (res + offset > (*buf)->__TRIS_STR_LEN) ? (*buf)->__TRIS_STR_LEN - 1 : res + offset;

	return res;
}

void tris_str_substitute_variables(struct tris_str **buf, size_t maxlen, struct tris_channel *chan, const char *template)
{
	int first = 1;
	do {
		tris_str_make_space(buf, maxlen ? maxlen :
			(first ? strlen(template) * 2 : (*buf)->__TRIS_STR_LEN * 2));
		pbx_substitute_variables_helper_full(chan, NULL, template, (*buf)->__TRIS_STR_STR, (*buf)->__TRIS_STR_LEN - 1, &((*buf)->__TRIS_STR_USED));
		first = 0;
	} while (maxlen == 0 && (*buf)->__TRIS_STR_LEN - 5 < (*buf)->__TRIS_STR_USED);
}

char *__tris_str_helper2(struct tris_str **buf, ssize_t maxlen, const char *src, size_t maxsrc, int append, int escapecommas)
{
	int dynamic = 0;
	char *ptr = append ? &((*buf)->__TRIS_STR_STR[(*buf)->__TRIS_STR_USED]) : (*buf)->__TRIS_STR_STR;

	if (maxlen < 1) {
		if (maxlen == 0) {
			dynamic = 1;
		}
		maxlen = (*buf)->__TRIS_STR_LEN;
	}

	while (*src && maxsrc && maxlen && (!escapecommas || (maxlen - 1))) {
		if (escapecommas && (*src == '\\' || *src == ',')) {
			*ptr++ = '\\';
			maxlen--;
			(*buf)->__TRIS_STR_USED++;
		}
		*ptr++ = *src++;
		maxsrc--;
		maxlen--;
		(*buf)->__TRIS_STR_USED++;

		if ((ptr >= (*buf)->__TRIS_STR_STR + (*buf)->__TRIS_STR_LEN - 3) ||
			(dynamic && (!maxlen || (escapecommas && !(maxlen - 1))))) {
			char *oldbase = (*buf)->__TRIS_STR_STR;
			size_t old = (*buf)->__TRIS_STR_LEN;
			if (tris_str_make_space(buf, (*buf)->__TRIS_STR_LEN * 2)) {
				/* If the buffer can't be extended, end it. */
				break;
			}
			/* What we extended the buffer by */
			maxlen = old;

			ptr += (*buf)->__TRIS_STR_STR - oldbase;
		}
	}
	if (__builtin_expect(!maxlen, 0)) {
		ptr--;
	}
	*ptr = '\0';
	return (*buf)->__TRIS_STR_STR;
}

