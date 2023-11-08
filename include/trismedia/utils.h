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
 * \brief Utility functions
 */

#ifndef _TRISMEDIA_UTILS_H
#define _TRISMEDIA_UTILS_H

#include "trismedia/network.h"

#include <time.h>	/* we want to override localtime_r */
#include <unistd.h>

#include "trismedia/lock.h"
#include "trismedia/time.h"
#include "trismedia/logger.h"
#include "trismedia/localtime.h"

/*! 
\note \verbatim
   Note:
   It is very important to use only unsigned variables to hold
   bit flags, as otherwise you can fall prey to the compiler's
   sign-extension antics if you try to use the top two bits in
   your variable.

   The flag macros below use a set of compiler tricks to verify
   that the caller is using an "unsigned int" variable to hold
   the flags, and nothing else. If the caller uses any other
   type of variable, a warning message similar to this:

   warning: comparison of distinct pointer types lacks cast
   will be generated.

   The "dummy" variable below is used to make these comparisons.

   Also note that at -O2 or above, this type-safety checking
   does _not_ produce any additional object code at all.
 \endverbatim
*/

extern unsigned int __unsigned_int_flags_dummy;

#define tris_test_flag(p,flag) 		({ \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags & (flag)); \
					})

#define tris_set_flag(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags |= (flag)); \
					} while(0)

#define tris_clear_flag(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags &= ~(flag)); \
					} while(0)

#define tris_copy_flags(dest,src,flagz)	do { \
					typeof ((dest)->flags) __d = (dest)->flags; \
					typeof ((src)->flags) __s = (src)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__d == &__x); \
					(void) (&__s == &__x); \
					(dest)->flags &= ~(flagz); \
					(dest)->flags |= ((src)->flags & (flagz)); \
					} while (0)

#define tris_set2_flag(p,value,flag)	do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					if (value) \
						(p)->flags |= (flag); \
					else \
						(p)->flags &= ~(flag); \
					} while (0)

#define tris_set_flags_to(p,flag,value)	do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					(p)->flags &= ~(flag); \
					(p)->flags |= (value); \
					} while (0)


/* The following 64-bit flag code can most likely be erased after app_dial
   is reorganized to either reduce the large number of options, or handle
   them in some other way. At the time of this writing, app_dial would be
   the only user of 64-bit option flags */

extern uint64_t __unsigned_int_flags_dummy64;

#define tris_test_flag64(p,flag) 		({ \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy64) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags & (flag)); \
					})

#define tris_set_flag64(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy64) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags |= (flag)); \
					} while(0)

#define tris_clear_flag64(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy64) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags &= ~(flag)); \
					} while(0)

#define tris_copy_flags64(dest,src,flagz)	do { \
					typeof ((dest)->flags) __d = (dest)->flags; \
					typeof ((src)->flags) __s = (src)->flags; \
					typeof (__unsigned_int_flags_dummy64) __x = 0; \
					(void) (&__d == &__x); \
					(void) (&__s == &__x); \
					(dest)->flags &= ~(flagz); \
					(dest)->flags |= ((src)->flags & (flagz)); \
					} while (0)

#define tris_set2_flag64(p,value,flag)	do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy64) __x = 0; \
					(void) (&__p == &__x); \
					if (value) \
						(p)->flags |= (flag); \
					else \
						(p)->flags &= ~(flag); \
					} while (0)

#define tris_set_flags_to64(p,flag,value)	do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy64) __x = 0; \
					(void) (&__p == &__x); \
					(p)->flags &= ~(flag); \
					(p)->flags |= (value); \
					} while (0)


/* Non-type checking variations for non-unsigned int flags.  You
   should only use non-unsigned int flags where required by 
   protocol etc and if you know what you're doing :)  */
#define tris_test_flag_nonstd(p,flag) \
					((p)->flags & (flag))

#define tris_set_flag_nonstd(p,flag) 		do { \
					((p)->flags |= (flag)); \
					} while(0)

#define tris_clear_flag_nonstd(p,flag) 		do { \
					((p)->flags &= ~(flag)); \
					} while(0)

#define tris_copy_flags_nonstd(dest,src,flagz)	do { \
					(dest)->flags &= ~(flagz); \
					(dest)->flags |= ((src)->flags & (flagz)); \
					} while (0)

#define tris_set2_flag_nonstd(p,value,flag)	do { \
					if (value) \
						(p)->flags |= (flag); \
					else \
						(p)->flags &= ~(flag); \
					} while (0)

#define TRIS_FLAGS_ALL UINT_MAX

/*! \brief Structure used to handle boolean flags 
*/
struct tris_flags {
	unsigned int flags;
};

/*! \brief Structure used to handle a large number of boolean flags == used only in app_dial?
*/
struct tris_flags64 {
	uint64_t flags;
};

struct tris_hostent {
	struct hostent hp;
	char buf[1024];
};

/*! \brief Thread-safe gethostbyname function to use in Trismedia */
struct hostent *tris_gethostbyname(const char *host, struct tris_hostent *hp);

/*!  \brief Produces MD5 hash based on input string */
void tris_md5_hash(char *output, char *input);
/*! \brief Produces SHA1 hash based on input string */
void tris_sha1_hash(char *output, char *input);

int tris_base64encode_full(char *dst, const unsigned char *src, int srclen, int max, int linebreaks);

#undef MIN
#define MIN(a, b) ({ typeof(a) __a = (a); typeof(b) __b = (b); ((__a > __b) ? __b : __a);})
#undef MAX
#define MAX(a, b) ({ typeof(a) __a = (a); typeof(b) __b = (b); ((__a < __b) ? __b : __a);})

/*!
 * \brief Encode data in base64
 * \param dst the destination buffer
 * \param src the source data to be encoded
 * \param srclen the number of bytes present in the source buffer
 * \param max the maximum number of bytes to write into the destination
 *        buffer, *including* the terminating NULL character.
 */
int tris_base64encode(char *dst, const unsigned char *src, int srclen, int max);

/*!
 * \brief Decode data from base64
 * \param dst the destination buffer
 * \param src the source buffer
 * \param max The maximum number of bytes to write into the destination
 *            buffer.  Note that this function will not ensure that the
 *            destination buffer is NULL terminated.  So, in general,
 *            this parameter should be sizeof(dst) - 1.
 */
int tris_base64decode(unsigned char *dst, const char *src, int max);

/*!  \brief Turn text string to URI-encoded %XX version 

\note 	At this point, we're converting from ISO-8859-x (8-bit), not UTF8
	as in the SIP protocol spec 
	If doreserved == 1 we will convert reserved characters also.
	RFC 2396, section 2.4
	outbuf needs to have more memory allocated than the instring
	to have room for the expansion. Every char that is converted
	is replaced by three ASCII characters.
	\param string	String to be converted
	\param outbuf	Resulting encoded string
	\param buflen	Size of output buffer
	\param doreserved	Convert reserved characters
*/

char *tris_uri_encode(const char *string, char *outbuf, int buflen, int doreserved);

/*!	\brief Decode URI, URN, URL (overwrite string)
	\param s	String to be decoded 
 */
void tris_uri_decode(char *s);

static force_inline void tris_slinear_saturated_add(short *input, short *value)
{
	int res;

	res = (int) *input + *value;
	if (res > 32767)
		*input = 32767;
	else if (res < -32767)
		*input = -32767;
	else
		*input = (short) res;
}

static force_inline void tris_slinear_saturated_subtract(short *input, short *value)
{
	int res;

	res = (int) *input - *value;
	if (res > 32767)
		*input = 32767;
	else if (res < -32767)
		*input = -32767;
	else
		*input = (short) res;
}
	
static force_inline void tris_slinear_saturated_multiply(short *input, short *value)
{
	int res;

	res = (int) *input * *value;
	if (res > 32767)
		*input = 32767;
	else if (res < -32767)
		*input = -32767;
	else
		*input = (short) res;
}

static force_inline void tris_slinear_saturated_divide(short *input, short *value)
{
	*input /= *value;
}

#ifdef localtime_r
#undef localtime_r
#endif
#define localtime_r __dont_use_localtime_r_use_tris_localtime_instead__

int tris_utils_init(void);
int tris_wait_for_input(int fd, int ms);

/*!
	\brief Try to write string, but wait no more than ms milliseconds
	before timing out.

	\note If you are calling tris_carefulwrite, it is assumed that you are calling
	it on a file descriptor that _DOES_ have NONBLOCK set.  This way,
	there is only one system call made to do a write, unless we actually
	have a need to wait.  This way, we get better performance.
*/
int tris_carefulwrite(int fd, char *s, int len, int timeoutms);

/*!
 * \brief Write data to a file stream with a timeout
 *
 * \param f the file stream to write to
 * \param fd the file description to poll on to know when the file stream can
 *        be written to without blocking.
 * \param s the buffer to write from
 * \param len the number of bytes to write
 * \param timeoutms The maximum amount of time to block in this function trying
 *        to write, specified in milliseconds.
 *
 * \note This function assumes that the associated file stream has been set up
 *       as non-blocking.
 *
 * \retval 0 success
 * \retval -1 error
 */
int tris_careful_fwrite(FILE *f, int fd, const char *s, size_t len, int timeoutms);

/*
 * Thread management support (should be moved to lock.h or a different header)
 */

#define TRIS_STACKSIZE (((sizeof(void *) * 8 * 8) - 16) * 1024)

#if defined(LOW_MEMORY)
#define TRIS_BACKGROUND_STACKSIZE (((sizeof(void *) * 8 * 2) - 16) * 1024)
#else
#define TRIS_BACKGROUND_STACKSIZE TRIS_STACKSIZE
#endif

void tris_register_thread(char *name);
void tris_unregister_thread(void *id);

int tris_pthread_create_stack(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *),
			     void *data, size_t stacksize, const char *file, const char *caller,
			     int line, const char *start_fn);

int tris_pthread_create_detached_stack(pthread_t *thread, pthread_attr_t *attr, void*(*start_routine)(void *),
				 void *data, size_t stacksize, const char *file, const char *caller,
				 int line, const char *start_fn);

#define tris_pthread_create(a, b, c, d) 				\
	tris_pthread_create_stack(a, b, c, d,			\
		0, __FILE__, __FUNCTION__, __LINE__, #c)

#define tris_pthread_create_detached(a, b, c, d)			\
	tris_pthread_create_detached_stack(a, b, c, d,		\
		0, __FILE__, __FUNCTION__, __LINE__, #c)

#define tris_pthread_create_background(a, b, c, d)		\
	tris_pthread_create_stack(a, b, c, d,			\
		TRIS_BACKGROUND_STACKSIZE,			\
		__FILE__, __FUNCTION__, __LINE__, #c)

#define tris_pthread_create_detached_background(a, b, c, d)	\
	tris_pthread_create_detached_stack(a, b, c, d,		\
		TRIS_BACKGROUND_STACKSIZE,			\
		__FILE__, __FUNCTION__, __LINE__, #c)

/* End of thread management support */

/*!
	\brief Process a string to find and replace characters
	\param start The string to analyze
	\param find The character to find
	\param replace_with The character that will replace the one we are looking for
*/
char *tris_process_quotes_and_slashes(char *start, char find, char replace_with);

long int tris_random(void);


/*! 
 * \brief free() wrapper
 *
 * tris_free_ptr should be used when a function pointer for free() needs to be passed
 * as the argument to a function. Otherwise, astmm will cause seg faults.
 */
#ifdef __TRIS_DEBUG_MALLOC
static void tris_free_ptr(void *ptr) attribute_unused;
static void tris_free_ptr(void *ptr)
{
	tris_free(ptr);
}
#else
#define tris_free free
#define tris_free_ptr tris_free
#endif

#ifndef __TRIS_DEBUG_MALLOC

#define MALLOC_FAILURE_MSG \
	tris_log(LOG_ERROR, "Memory Allocation Failure in function %s at line %d of %s\n", func, lineno, file);
/*!
 * \brief A wrapper for malloc()
 *
 * tris_malloc() is a wrapper for malloc() that will generate an Trismedia log
 * message in the case that the allocation fails.
 *
 * The argument and return value are the same as malloc()
 */
#define tris_malloc(len) \
	_tris_malloc((len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

TRIS_INLINE_API(
void * attribute_malloc _tris_malloc(size_t len, const char *file, int lineno, const char *func),
{
	void *p;

	if (!(p = malloc(len)))
		MALLOC_FAILURE_MSG;

	return p;
}
)

/*!
 * \brief A wrapper for calloc()
 *
 * tris_calloc() is a wrapper for calloc() that will generate an Trismedia log
 * message in the case that the allocation fails.
 *
 * The arguments and return value are the same as calloc()
 */
#define tris_calloc(num, len) \
	_tris_calloc((num), (len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

TRIS_INLINE_API(
void * attribute_malloc _tris_calloc(size_t num, size_t len, const char *file, int lineno, const char *func),
{
	void *p;

	if (!(p = calloc(num, len)))
		MALLOC_FAILURE_MSG;

	return p;
}
)

/*!
 * \brief A wrapper for calloc() for use in cache pools
 *
 * tris_calloc_cache() is a wrapper for calloc() that will generate an Trismedia log
 * message in the case that the allocation fails. When memory debugging is in use,
 * the memory allocated by this function will be marked as 'cache' so it can be
 * distinguished from normal memory allocations.
 *
 * The arguments and return value are the same as calloc()
 */
#define tris_calloc_cache(num, len) \
	_tris_calloc((num), (len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

/*!
 * \brief A wrapper for realloc()
 *
 * tris_realloc() is a wrapper for realloc() that will generate an Trismedia log
 * message in the case that the allocation fails.
 *
 * The arguments and return value are the same as realloc()
 */
#define tris_realloc(p, len) \
	_tris_realloc((p), (len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

TRIS_INLINE_API(
void * attribute_malloc _tris_realloc(void *p, size_t len, const char *file, int lineno, const char *func),
{
	void *newp;

	if (!(newp = realloc(p, len)))
		MALLOC_FAILURE_MSG;

	return newp;
}
)

/*!
 * \brief A wrapper for strdup()
 *
 * tris_strdup() is a wrapper for strdup() that will generate an Trismedia log
 * message in the case that the allocation fails.
 *
 * tris_strdup(), unlike strdup(), can safely accept a NULL argument. If a NULL
 * argument is provided, tris_strdup will return NULL without generating any
 * kind of error log message.
 *
 * The argument and return value are the same as strdup()
 */
#define tris_strdup(str) \
	_tris_strdup((str), __FILE__, __LINE__, __PRETTY_FUNCTION__)

TRIS_INLINE_API(
char * attribute_malloc _tris_strdup(const char *str, const char *file, int lineno, const char *func),
{
	char *newstr = NULL;

	if (str) {
		if (!(newstr = strdup(str)))
			MALLOC_FAILURE_MSG;
	}

	return newstr;
}
)

/*!
 * \brief A wrapper for strndup()
 *
 * tris_strndup() is a wrapper for strndup() that will generate an Trismedia log
 * message in the case that the allocation fails.
 *
 * tris_strndup(), unlike strndup(), can safely accept a NULL argument for the
 * string to duplicate. If a NULL argument is provided, tris_strdup will return  
 * NULL without generating any kind of error log message.
 *
 * The arguments and return value are the same as strndup()
 */
#define tris_strndup(str, len) \
	_tris_strndup((str), (len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

TRIS_INLINE_API(
char * attribute_malloc _tris_strndup(const char *str, size_t len, const char *file, int lineno, const char *func),
{
	char *newstr = NULL;

	if (str) {
		if (!(newstr = strndup(str, len)))
			MALLOC_FAILURE_MSG;
	}

	return newstr;
}
)

/*!
 * \brief A wrapper for asprintf()
 *
 * tris_asprintf() is a wrapper for asprintf() that will generate an Trismedia log
 * message in the case that the allocation fails.
 *
 * The arguments and return value are the same as asprintf()
 */
#define tris_asprintf(ret, fmt, ...) \
	_tris_asprintf((ret), __FILE__, __LINE__, __PRETTY_FUNCTION__, fmt, __VA_ARGS__)

int __attribute__((format(printf, 5, 6)))
	_tris_asprintf(char **ret, const char *file, int lineno, const char *func, const char *fmt, ...);

/*!
 * \brief A wrapper for vasprintf()
 *
 * tris_vasprintf() is a wrapper for vasprintf() that will generate an Trismedia log
 * message in the case that the allocation fails.
 *
 * The arguments and return value are the same as vasprintf()
 */
#define tris_vasprintf(ret, fmt, ap) \
	_tris_vasprintf((ret), __FILE__, __LINE__, __PRETTY_FUNCTION__, (fmt), (ap))

TRIS_INLINE_API(
__attribute__((format(printf, 5, 0)))
int _tris_vasprintf(char **ret, const char *file, int lineno, const char *func, const char *fmt, va_list ap),
{
	int res;

	if ((res = vasprintf(ret, fmt, ap)) == -1)
		MALLOC_FAILURE_MSG;

	return res;
}
)

#endif /* TRIS_DEBUG_MALLOC */

#if !defined(tris_strdupa) && defined(__GNUC__)
/*!
  \brief duplicate a string in memory from the stack
  \param s The string to duplicate

  This macro will duplicate the given string.  It returns a pointer to the stack
  allocatted memory for the new string.
*/
#define tris_strdupa(s)                                                    \
	(__extension__                                                    \
	({                                                                \
		const char *__old = (s);                                  \
		size_t __len = strlen(__old) + 1;                         \
		char *__new = __builtin_alloca(__len);                    \
		memcpy (__new, __old, __len);                             \
		__new;                                                    \
	}))
#endif

/*!
  \brief Disable PMTU discovery on a socket
  \param sock The socket to manipulate
  \return Nothing

  On Linux, UDP sockets default to sending packets with the Dont Fragment (DF)
  bit set. This is supposedly done to allow the application to do PMTU
  discovery, but Trismedia does not do this.

  Because of this, UDP packets sent by Trismedia that are larger than the MTU
  of any hop in the path will be lost. This function can be called on a socket
  to ensure that the DF bit will not be set.
 */
void tris_enable_packet_fragmentation(int sock);

/*!
  \brief Recursively create directory path
  \param path The directory path to create
  \param mode The permissions with which to try to create the directory
  \return 0 on success or an error code otherwise

  Creates a directory path, creating parent directories as needed.
 */
int tris_mkdir(const char *path, int mode);

#define ARRAY_LEN(a) (sizeof(a) / sizeof(0[a]))

#ifdef TRIS_DEVMODE
#define tris_assert(a) _tris_assert(a, # a, __FILE__, __LINE__, __PRETTY_FUNCTION__)
static void force_inline _tris_assert(int condition, const char *condition_str, 
	const char *file, int line, const char *function)
{
	if (__builtin_expect(!condition, 1)) {
		/* Attempt to put it into the logger, but hope that at least someone saw the
		 * message on stderr ... */
		tris_log(__LOG_ERROR, file, line, function, "FRACK!, Failed assertion %s (%d)\n",
			condition_str, condition);
		fprintf(stderr, "FRACK!, Failed assertion %s (%d) at line %d in %s of %s\n",
			condition_str, condition, line, function, file);
		/* Give the logger a chance to get the message out, just in case we abort(), or
		 * Trismedia crashes due to whatever problem just happened after we exit tris_assert(). */
		usleep(1);
#ifdef DO_CRASH
		abort();
		/* Just in case abort() doesn't work or something else super silly,
		 * and for Qwell's amusement. */
		*((int*)0)=0;
#endif
	}
}
#else
#define tris_assert(a)
#endif

#include "trismedia/strings.h"

/*!
 * \brief An Entity ID is essentially a MAC address, brief and unique 
 */
struct tris_eid {
	unsigned char eid[6];
} __attribute__((__packed__));

/*!
 * \brief Global EID
 *
 * This is set in trismedia.conf, or determined automatically by taking the mac
 * address of an Ethernet interface on the system.
 */
extern struct tris_eid tris_eid_default;

/*!
 * \brief Fill in an tris_eid with the default eid of this machine
 * \since 1.6.1
 */
void tris_set_default_eid(struct tris_eid *eid);

/*!
 * /brief Convert an EID to a string
 * \since 1.6.1
 */
char *tris_eid_to_str(char *s, int maxlen, struct tris_eid *eid);

/*!
 * \brief Convert a string into an EID
 *
 * This function expects an EID in the format:
 *    00:11:22:33:44:55
 *
 * \return 0 success, non-zero failure
 * \since 1.6.1
 */
int tris_str_to_eid(struct tris_eid *eid, const char *s);

/*!
 * \brief Compare two EIDs
 *
 * \return 0 if the two are the same, non-zero otherwise
 * \since 1.6.1
 */
int tris_eid_cmp(const struct tris_eid *eid1, const struct tris_eid *eid2);

int write2fifo(char *fifo_cmd, int len);

#endif /* _TRISMEDIA_UTILS_H */
