/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Digium, Inc.
 *
 * Russell Bryant <russell@digium.com>
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

/*!
 * \file
 * \brief Max Heap data structure
 * \author Russell Bryant <russell@digium.com>
 */

#ifndef __TRIS_HEAP_H__
#define __TRIS_HEAP_H__

/*!
 * \brief A max heap.
 *
 * \note Thread-safety is left to the user of the API.  The heap API provides
 *       no locking of its own.  If the heap will be accessed by multiple threads,
 *       then a lock must be used to ensure that only a single operation is
 *       done on the heap at a time.  For the sake of convenience, a lock is
 *       provided for the user of the API to use if another lock is not already
 *       available to protect the heap.
 */
struct tris_heap;

/*!
 * \brief Function type for comparing nodes in a heap
 *
 * \param elm1 the first element
 * \param elm2 the second element
 *
 * \retval negative if elm1 < elm2
 * \retval 0 if elm1 == elm2
 * \retval positive if elm1 > elm2
 *
 * \note This implementation is of a max heap.  However, if a min heap is
 *       desired, simply swap the return values of this function.
 *
 * \since 1.6.1
 */
typedef int (*tris_heap_cmp_fn)(void *elm1, void *elm2);

/*!
 * \brief Create a max heap
 *
 * \param init_height The initial height of the heap to allocate space for.
 *        To start out, there will be room for (2 ^ init_height) - 1 entries.
 *        However, the heap will grow as needed.
 * \param cmp_fn The function that should be used to compare elements in the heap.
 * \param index_offset This parameter is optional, but must be provided to be able
 *        to use tris_heap_remove().  This is the number of bytes into the element
 *        where an ssize_t has been made available for the heap's internal
 *        use.  The heap will use this field to keep track of the element's current
 *        position in the heap.  The offsetof() macro is useful for providing a
 *        proper value for this argument.  If tris_heap_remove() will not be used,
 *        then a negative value can be provided to indicate that no field for an
 *        offset has been allocated.
 *
 * Example Usage:
 *
 * \code
 *
 * struct myobj {
 *    int foo;
 *    int bar;
 *    char stuff[8];
 *    char things[8];
 *    ssize_t __heap_index;
 * };
 *
 * ...
 *
 * static int myobj_cmp(void *obj1, void *obj2);
 *
 * ...
 *
 * struct tris_heap *h;
 *
 * h = tris_heap_create(8, myobj_cmp, offsetof(struct myobj, __heap_index));
 *
 * \endcode
 *
 * \return An instance of a max heap
 * \since 1.6.1
 */
#ifdef MALLOC_DEBUG
struct tris_heap *_tris_heap_create(unsigned int init_height, tris_heap_cmp_fn cmp_fn,
		ssize_t index_offset, const char *file, int lineno, const char *func);
#define	tris_heap_create(a,b,c)	_tris_heap_create(a,b,c,__FILE__,__LINE__,__PRETTY_FUNCTION__)
#else
struct tris_heap *tris_heap_create(unsigned int init_height, tris_heap_cmp_fn cmp_fn,
		ssize_t index_offset);
#endif

/*!
 * \brief Destroy a max heap
 *
 * \param h the heap to destroy
 *
 * \return NULL for convenience
 * \since 1.6.1
 */
struct tris_heap *tris_heap_destroy(struct tris_heap *h);

/*!
 * \brief Push an element on to a heap
 *
 * \param h the heap being added to
 * \param elm the element being put on the heap
 *
 * \retval 0 success
 * \retval non-zero failure
 * \since 1.6.1
 */
#ifdef MALLOC_DEBUG
int _tris_heap_push(struct tris_heap *h, void *elm, const char *file, int lineno, const char *func);
#define	tris_heap_push(a,b)	_tris_heap_push(a,b,__FILE__,__LINE__,__PRETTY_FUNCTION__)
#else
int tris_heap_push(struct tris_heap *h, void *elm);
#endif

/*!
 * \brief Pop the max element off of the heap
 *
 * \param h the heap
 *
 * \return this will return the element on the top of the heap, which has the
 *         largest value according to the element comparison function that was
 *         provided when the heap was created.  The element will be removed before
 *         being returned.
 * \since 1.6.1
 */
void *tris_heap_pop(struct tris_heap *h);

/*!
 * \brief Remove a specific element from a heap
 *
 * \param h the heap to remove from
 * \param elm the element to remove
 *
 * \return elm, if the removal was successful, or NULL if it failed
 *
 * \note the index_offset parameter to tris_heap_create() is required to be able
 *       to use this function.
 * \since 1.6.1
 */
void *tris_heap_remove(struct tris_heap *h, void *elm);

/*!
 * \brief Peek at an element on a heap
 *
 * \param h the heap
 * \param index index of the element to return.  The first element is at index 1,
 *        and the last element is at the index == the size of the heap.
 *
 * \return an element at the specified index on the heap.  This element will \b not
 *         be removed before being returned.
 *
 * \note If this function is being used in combination with tris_heap_size() for
 *       purposes of traversing the heap, the heap must be locked for the entire
 *       duration of the traversal.
 *
 * Example code for a traversal:
 * \code
 *
 * struct tris_heap *h;
 *
 * ...
 *
 * size_t size, i;
 * void *cur_obj;
 *
 * tris_heap_rdlock(h);
 *
 * size = tris_heap_size(h);
 *
 * for (i = 1; i <= size && (cur_obj = tris_heap_peek(h, i)); i++) {
 *     ... Do stuff with cur_obj ...
 * }
 *
 * tris_heap_unlock(h);
 *
 * \endcode
 * \since 1.6.1
 */
void *tris_heap_peek(struct tris_heap *h, unsigned int index);

/*!
 * \brief Get the current size of a heap
 *
 * \param h the heap
 *
 * \return the number of elements currently in the heap
 * \since 1.6.1
 */
size_t tris_heap_size(struct tris_heap *h);

#ifndef DEBUG_THREADS

/*!
 * \brief Write-Lock a heap
 *
 * \param h the heap
 *
 * A lock is provided for convenience.  It can be assumed that none of the
 * tris_heap API calls are thread safe.  This lock does not have to be used
 * if another one is already available to protect the heap.
 *
 * \return see the documentation for pthread_rwlock_wrlock()
 * \since 1.6.1
 */
int tris_heap_wrlock(struct tris_heap *h);

/*!
 * \brief Read-Lock a heap
 *
 * \param h the heap
 *
 * A lock is provided for convenience.  It can be assumed that none of the
 * tris_heap API calls are thread safe.  This lock does not have to be used
 * if another one is already available to protect the heap.
 *
 * \return see the documentation for pthread_rwlock_rdlock()
 * \since 1.6.1
 */
int tris_heap_rdlock(struct tris_heap *h);

/*!
 * \brief Unlock a heap
 *
 * \param h the heap
 *
 * \return see the documentation for pthread_rwlock_unlock()
 * \since 1.6.1
 */
int tris_heap_unlock(struct tris_heap *h);

#else /* DEBUG_THREADS */

#define tris_heap_wrlock(h) __tris_heap_wrlock(h, __FILE__, __PRETTY_FUNCTION__, __LINE__)
int __tris_heap_wrlock(struct tris_heap *h, const char *file, const char *func, int line);
#define tris_heap_rdlock(h) __tris_heap_rdlock(h, __FILE__, __PRETTY_FUNCTION__, __LINE__)
int __tris_heap_rdlock(struct tris_heap *h, const char *file, const char *func, int line);
#define tris_heap_unlock(h) __tris_heap_unlock(h, __FILE__, __PRETTY_FUNCTION__, __LINE__)
int __tris_heap_unlock(struct tris_heap *h, const char *file, const char *func, int line);

#endif /* DEBUG_THREADS */

/*!
 * \brief Verify that a heap has been properly constructed
 *
 * \param h a heap
 *
 * \retval 0 success
 * \retval non-zero failure
 *
 * \note This function is mostly for debugging purposes.  It traverses an existing
 *       heap and verifies that every node is properly placed relative to its children.
 * \since 1.6.1
 */
int tris_heap_verify(struct tris_heap *h);

#endif /* __TRIS_HEAP_H__ */
