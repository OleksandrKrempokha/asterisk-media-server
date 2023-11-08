/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
 *
 * Kevin P. Fleming <kpfleming@digium.com>
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
  \brief String fields in structures

  This file contains objects and macros used to manage string
  fields in structures without requiring them to be allocated
  as fixed-size buffers or requiring individual allocations for
  for each field.

  Using this functionality is quite simple. An example structure
  with three fields is defined like this:
  
  \code
  struct sample_fields {
	  int x1;
	  TRIS_DECLARE_STRING_FIELDS(
		  TRIS_STRING_FIELD(foo);
		  TRIS_STRING_FIELD(bar);
		  TRIS_STRING_FIELD(blah);
	  );
	  long x2;
  };
  \endcode
  
  When an instance of this structure is allocated (either statically or
  dynamically), the fields and the pool of storage for them must be
  initialized:
  
  \code
  struct sample_fields *x;
  
  x = tris_calloc(1, sizeof(*x));
  if (x == NULL || tris_string_field_init(x, 252)) {
	if (x)
		tris_free(x);
	x = NULL;
  	... handle error
  }
  \endcode

  Fields will default to pointing to an empty string, and will revert to
  that when tris_string_field_set() is called with a NULL argument.
  A string field will \b never contain NULL (this feature is not used
  in this code, but comes from external requirements).

  tris_string_field_init(x, 0) will reset fields to the
  initial value while keeping the pool allocated.
  
  Reading the fields is much like using 'const char * const' fields in the
  structure: you cannot write to the field or to the memory it points to
  (XXX perhaps the latter is too much of a restriction since values
  are not shared).

  Writing to the fields must be done using the wrapper macros listed below;
  and assignments are always by value (i.e. strings are copied):
  * tris_string_field_set() stores a simple value;
  * tris_string_field_build() builds the string using a printf-style;
  * tris_string_field_build_va() is the varargs version of the above (for
    portability reasons it uses two vararg);
  * variants of these function allow passing a pointer to the field
    as an argument.
  \code
  tris_string_field_set(x, foo, "infinite loop");
  tris_string_field_set(x, foo, NULL); // set to an empty string
  tris_string_field_ptr_set(x, &x->bar, "right way");

  tris_string_field_build(x, blah, "%d %s", zipcode, city);
  tris_string_field_ptr_build(x, &x->blah, "%d %s", zipcode, city);

  tris_string_field_build_va(x, bar, fmt, args1, args2)
  tris_string_field_ptr_build_va(x, &x->bar, fmt, args1, args2)
  \endcode

  When the structure instance is no longer needed, the fields
  and their storage pool must be freed:
  
  \code
  tris_string_field_free_memory(x);
  tris_free(x);
  \endcode

  This completes the API description.
*/

#ifndef _TRISMEDIA_STRINGFIELDS_H
#define _TRISMEDIA_STRINGFIELDS_H

#include "trismedia/inline_api.h"

/*!
  \internal
  \brief An opaque type for managed string fields in structures

  Don't declare instances of this type directly; use the TRIS_STRING_FIELD()
  macro instead.
*/
typedef const char * tris_string_field;

/*!
  \internal
  \brief A constant empty string used for fields that have no other value
*/
extern const char __tris_string_field_empty[];

/*!
  \internal
  \brief Structure used to hold a pool of space for string fields
*/
struct tris_string_field_pool {
	struct tris_string_field_pool *prev;	/*!< pointer to the previous pool, if any */
	char base[0];				/*!< storage space for the fields */
};

/*!
  \internal
  \brief Structure used to manage the storage for a set of string fields.
  Because of the way pools are managed, we can only allocate from the topmost
  pool, so the numbers here reflect just that.
*/
struct tris_string_field_mgr {
	size_t size;				/*!< the total size of the current pool */
	size_t used;				/*!< the space used in the current pool */
	tris_string_field last_alloc;		/*!< the last field allocated */
#if defined(__TRIS_DEBUG_MALLOC)
	const char *owner_file;				/*!< filename of owner */
	const char *owner_func;				/*!< function name of owner */
	int owner_line;					/*!< line number of owner */
#endif
};

/*!
  \internal
  \brief Attempt to 'grow' an already allocated field to a larger size
  \param mgr Pointer to the pool manager structure
  \param needed Amount of space needed for this field
  \param ptr Pointer to a field within the structure
  \return 0 on success, non-zero on failure

  This function will attempt to increase the amount of space allocated to
  an existing field to the amount requested; this is only possible if the
  field was the last field allocated from the current storage pool and
  the pool has enough space available. If so, the additional space will be
  allocated to this field and the field's address will not be changed.
*/
int __tris_string_field_ptr_grow(struct tris_string_field_mgr *mgr, size_t needed,
				const tris_string_field *ptr);

/*!
  \internal
  \brief Allocate space for a field
  \param mgr Pointer to the pool manager structure
  \param needed Amount of space needed for this field
  \param fields Pointer to the first entry of the field array
  \return NULL on failure, an address for the field on success.

  This function will allocate the requested amount of space from
  the field pool. If the requested amount of space is not available,
  an additional pool will be allocated.
*/
tris_string_field __tris_string_field_alloc_space(struct tris_string_field_mgr *mgr,
						struct tris_string_field_pool **pool_head, size_t needed);

/*!
  \internal
  \brief Set a field to a complex (built) value
  \param mgr Pointer to the pool manager structure
  \param fields Pointer to the first entry of the field array
  \param ptr Pointer to a field within the structure
  \param format printf-style format string
  \return nothing
*/
void __tris_string_field_ptr_build(struct tris_string_field_mgr *mgr,
				  struct tris_string_field_pool **pool_head,
				  tris_string_field *ptr, const char *format, ...) __attribute__((format(printf, 4, 5)));

/*!
  \internal
  \brief Set a field to a complex (built) value
  \param mgr Pointer to the pool manager structure
  \param fields Pointer to the first entry of the field array
  \param ptr Pointer to a field within the structure
  \param format printf-style format string
  \param args va_list of the args for the format_string
  \param args_again a copy of the first va_list for the sake of bsd not having a copy routine
  \return nothing
*/
void __tris_string_field_ptr_build_va(struct tris_string_field_mgr *mgr,
				     struct tris_string_field_pool **pool_head,
				     tris_string_field *ptr, const char *format, va_list a1, va_list a2) __attribute__((format(printf, 4, 0)));

/*!
  \brief Declare a string field
  \param name The field name
*/
#define TRIS_STRING_FIELD(name) const tris_string_field name

/*!
  \brief Declare the fields needed in a structure
  \param field_list The list of fields to declare, using TRIS_STRING_FIELD() for each one.
  Internally, string fields are stored as a pointer to the head of the pool,
  followed by individual string fields, and then a struct tris_string_field_mgr
  which describes the space allocated.
  We split the two variables so they can be used as markers around the
  field_list, and this allows us to determine how many entries are in
  the field, and play with them.
  In particular, for writing to the fields, we rely on __field_mgr_pool to be
  a non-const pointer, so we know it has the same size as tris_string_field,
  and we can use it to locate the fields.
*/
#define TRIS_DECLARE_STRING_FIELDS(field_list) \
	struct tris_string_field_pool *__field_mgr_pool;	\
	field_list					\
	struct tris_string_field_mgr __field_mgr

/*!
  \brief Initialize a field pool and fields
  \param x Pointer to a structure containing fields
  \param size Amount of storage to allocate.
	Use 0 to reset fields to the default value,
	and release all but the most recent pool.
	size<0 (used internally) means free all pools.
  \return 0 on success, non-zero on failure
*/
#define tris_string_field_init(x, size) \
	__tris_string_field_init(&(x)->__field_mgr, &(x)->__field_mgr_pool, size, __FILE__, __LINE__, __PRETTY_FUNCTION__)

/*! \brief free all memory - to be called before destroying the object */
#define tris_string_field_free_memory(x)	\
	__tris_string_field_init(&(x)->__field_mgr, &(x)->__field_mgr_pool, -1, __FILE__, __LINE__, __PRETTY_FUNCTION__)

/*!
 * \internal
 * \brief internal version of tris_string_field_init
 */
int __tris_string_field_init(struct tris_string_field_mgr *mgr, struct tris_string_field_pool **pool_head,
			    int needed, const char *file, int lineno, const char *func);

/*!
  \brief Set a field to a simple string value
  \param x Pointer to a structure containing fields
  \param ptr Pointer to a field within the structure
  \param data String value to be copied into the field
  \return nothing
*/
#define tris_string_field_ptr_set(x, ptr, data) do { 		\
	const char *__d__ = (data);				\
	size_t __dlen__ = (__d__) ? strlen(__d__) + 1 : 1;	\
	const char **__p__ = (const char **) (ptr);		\
	char *__q__; \
	if (__dlen__ == 1)					\
		*__p__ = __tris_string_field_empty;		\
	else if (!__tris_string_field_ptr_grow(&(x)->__field_mgr, __dlen__, ptr)) { \
		__q__ = (char *) *__p__; \
		memcpy(__q__, __d__, __dlen__);	\
	} else if ((*__p__ = __tris_string_field_alloc_space(&(x)->__field_mgr, &(x)->__field_mgr_pool, __dlen__))) { \
		__q__ = (char *) *__p__; \
		memcpy(__q__, __d__, __dlen__);	\
	} \
	} while (0)

/*!
  \brief Set a field to a simple string value
  \param x Pointer to a structure containing fields
  \param field Name of the field to set
  \param data String value to be copied into the field
  \return nothing
*/
#define tris_string_field_set(x, field, data) do {		\
	tris_string_field_ptr_set(x, &(x)->field, data);		\
	} while (0)

/*!
  \brief Set a field to a complex (built) value
  \param x Pointer to a structure containing fields
  \param ptr Pointer to a field within the structure
  \param fmt printf-style format string
  \param args Arguments for format string
  \return nothing
*/
#define tris_string_field_ptr_build(x, ptr, fmt, args...) \
	__tris_string_field_ptr_build(&(x)->__field_mgr, &(x)->__field_mgr_pool, (tris_string_field *) ptr, fmt, args)

/*!
  \brief Set a field to a complex (built) value
  \param x Pointer to a structure containing fields
  \param field Name of the field to set
  \param fmt printf-style format string
  \param args Arguments for format string
  \return nothing
*/
#define tris_string_field_build(x, field, fmt, args...) \
	__tris_string_field_ptr_build(&(x)->__field_mgr, &(x)->__field_mgr_pool, (tris_string_field *) &(x)->field, fmt, args)

/*!
  \brief Set a field to a complex (built) value with prebuilt va_lists.
  \param x Pointer to a structure containing fields
  \param ptr Pointer to a field within the structure
  \param fmt printf-style format string
  \param args1 Arguments for format string in va_list format
  \param args2 a second copy of the va_list for the sake of bsd, with no va_list copy operation
  \return nothing
*/
#define tris_string_field_ptr_build_va(x, ptr, fmt, args1, args2) \
	__tris_string_field_ptr_build_va(&(x)->__field_mgr, &(x)->__field_mgr_pool, (tris_string_field *) ptr, fmt, args1, args2)

/*!
  \brief Set a field to a complex (built) value
  \param x Pointer to a structure containing fields
  \param field Name of the field to set
  \param fmt printf-style format string
  \param args1 argument one
  \param args2 argument two
  \return nothing
*/
#define tris_string_field_build_va(x, field, fmt, args1, args2) \
	__tris_string_field_ptr_build_va(&(x)->__field_mgr, &(x)->__field_mgr_pool, (tris_string_field *) &(x)->field, fmt, args1, args2)

#endif /* _TRISMEDIA_STRINGFIELDS_H */
