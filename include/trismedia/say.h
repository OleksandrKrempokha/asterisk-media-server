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
 * \brief Say numbers and dates (maybe words one day too)
 */

#ifndef _TRISMEDIA_SAY_H
#define _TRISMEDIA_SAY_H

#include "trismedia/channel.h"
#include "trismedia/file.h"

#include <time.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! \brief
 * The basic tris_say_* functions are implemented as function pointers,
 * initialized to the function say_stub() which simply returns an error.
 * Other interfaces, declared here as regular functions, are simply
 * wrappers around the basic functions.
 *
 * An implementation of the basic tris_say functions (e.g. from say.c or from
 * a dynamically loaded module) will just have to reassign the pointers
 * to the relevant functions to override the previous implementation.
 *
 * \todo XXX
 * As the conversion from the old implementation of say.c to the new
 * implementation will be completed, and the API suitably reworked by
 * removing redundant functions and/or arguments, this mechanism may be
 * reverted back to pure static functions, if needed.
 */
#if defined(SAY_STUBS)
/* provide declarations for the *say*() functions
 * and initialize them to the stub function
 */
static int say_stub(struct tris_channel *chan, ...)
{
	tris_log(LOG_WARNING, "no implementation for the say() functions\n");
        return -1;
};

#undef SAY_STUBS
#define	SAY_INIT(x)	 = (typeof (x))say_stub
#define	SAY_EXTERN
#else
#define SAY_INIT(x)
#define	SAY_EXTERN	extern
#endif

/*!
 * \brief says a number
 * \param chan channel to say them number on
 * \param num number to say on the channel
 * \param ints which dtmf to interrupt on
 * \param lang language to speak the number
 * \param options set to 'f' for female, 'm' for male, 'c' for commune, 'n' for neuter, 'p' for plural
 * \details
 * Vocally says a number on a given channel
 * \retval 0 on success
 * \retval DTMF digit on interrupt
 * \retval -1 on failure
 */
int tris_say_number(struct tris_channel *chan, int num,
	const char *ints, const char *lang, const char *options);

/* Same as above with audiofd for received audio and returns 1 on ctrlfd being readable */
SAY_EXTERN int (* tris_say_number_full)(struct tris_channel *chan, int num, const char *ints, const char *lang, const char *options, int audiofd, int ctrlfd) SAY_INIT(tris_say_number_full);

/*!
 * \brief says an enumeration
 * \param chan channel to say them enumeration on
 * \param num number to say on the channel
 * \param ints which dtmf to interrupt on
 * \param lang language to speak the enumeration
 * \param options set to 'f' for female, 'm' for male, 'c' for commune, 'n' for neuter, 'p' for plural
 * \details
 * Vocally says an enumeration on a given channel (first, sencond, third, forth, thirtyfirst, hundredth, ....)
 * Especially useful for dates and messages. Says 'last' if num equals to INT_MAX
 * \retval 0 on success
 * \retval DTMF digit on interrupt
 * \retval -1 on failure
 */
int tris_say_enumeration(struct tris_channel *chan, int num,
	const char *ints, const char *lang, const char *options);

SAY_EXTERN int (* tris_say_enumeration_full)(struct tris_channel *chan, int num, const char *ints, const char *lang, const char *options, int audiofd, int ctrlfd) SAY_INIT(tris_say_enumeration_full);

/*!
 * \brief says digits
 * \param chan channel to act upon
 * \param num number to speak
 * \param ints which dtmf to interrupt on
 * \param lang language to speak
 * \details
 * Vocally says digits of a given number
 * \retval 0 on success
 * \retval DTMF if interrupted
 * \retval -1 on failure
 */
int tris_say_digits(struct tris_channel *chan, int num,
	const char *ints, const char *lang);

int tris_say_digits_full(struct tris_channel *chan, int num,
	const char *ints, const char *lang, int audiofd, int ctrlfd);

/*!
 * \brief says digits of a string
 * \param chan channel to act upon
 * \param num string to speak
 * \param ints which dtmf to interrupt on
 * \param lang language to speak in
 * \details
 * Vocally says the digits of a given string
 * \retval 0 on succes
 * \retval DTMF if interrupted
 * \retval -1 on failure
 */
int tris_say_digit_str(struct tris_channel *chan, const char *num,
	const char *ints, const char *lang);

SAY_EXTERN int (* tris_say_digit_str_full)(struct tris_channel *chan, const char *num, const char *ints, const char *lang, int audiofd, int ctrlfd) SAY_INIT(tris_say_digit_str_full);

/*
 * the generic 'say' routine, with the first chars in the string
 * defining the format to use
 */
SAY_EXTERN int (* tris_say_full)(struct tris_channel *chan, const char *num, const char *ints, const char *lang, const char *options, int audiofd, int ctrlfd) SAY_INIT(tris_say_full);

/*
 * other function to pronounce character and phonetic strings
 */
int tris_say_character_str(struct tris_channel *chan, const char *num,
	const char *ints, const char *lang);

SAY_EXTERN int (* tris_say_character_str_full)(struct tris_channel *chan, const char *num, const char *ints, const char *lang, int audiofd, int ctrlfd) SAY_INIT(tris_say_character_str_full);

int tris_say_phonetic_str(struct tris_channel *chan, const char *num,
	const char *ints, const char *lang);

SAY_EXTERN int (* tris_say_phonetic_str_full)(struct tris_channel *chan, const char *num, const char *ints, const char *lang, int audiofd, int ctrlfd) SAY_INIT(tris_say_phonetic_str_full);

SAY_EXTERN int (* tris_say_datetime)(struct tris_channel *chan, time_t t, const char *ints, const char *lang) SAY_INIT(tris_say_datetime);
SAY_EXTERN int (* tris_say_time)(struct tris_channel *chan, time_t t, const char *ints, const char *lang) SAY_INIT(tris_say_time);

SAY_EXTERN int (* tris_say_date)(struct tris_channel *chan, time_t t, const char *ints, const char *lang) SAY_INIT(tris_say_date);

SAY_EXTERN int (* tris_say_datetime_from_now)(struct tris_channel *chan, time_t t, const char *ints, const char *lang) SAY_INIT(tris_say_datetime_from_now);

SAY_EXTERN int (* tris_say_date_with_format)(struct tris_channel *chan, time_t t, const char *ints, const char *lang, const char *format, const char *timezone) SAY_INIT(tris_say_date_with_format);

int tris_say_counted_noun(struct tris_channel *chan, int num, const char *noun);

int tris_say_counted_adjective(struct tris_channel *chan, int num, const char *adjective, const char *gender);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_SAY_H */
