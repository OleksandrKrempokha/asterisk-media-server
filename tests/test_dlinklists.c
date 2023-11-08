/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Steve Murphy
 *
 * Steve Murphy <murf@digium.com>
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
 * \brief Doubly-Linked List Tests
 *
 * \author\verbatim Steve Murphy <murf@digium.com> \endverbatim
 * 
 * This module will run some DLL tests at load time
 * \ingroup tests
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 114172 $")

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"


#include "trismedia/dlinkedlists.h"

/* Tests for DLLists! We really should, and here is a nice place to do it in trismedia */

struct test1
{
	char name[10];
	TRIS_DLLIST_ENTRY(test1) list;
};

struct test_container
{
	TRIS_DLLIST_HEAD(entries, test1) entries;
    int count;
};

static void print_list(struct test_container *x, char *expect)
{
	struct test1 *t1;
	char buff[1000];
	buff[0] = 0;
	TRIS_DLLIST_TRAVERSE(&x->entries, t1, list) {
		strcat(buff,t1->name);
		if (t1 != TRIS_DLLIST_LAST(&x->entries))
			strcat(buff," <=> ");
	}
	
	tris_log(LOG_NOTICE,"Got: %s  [expect %s]\n", buff, expect);
}

static void print_list_backwards(struct test_container *x, char *expect)
{
	struct test1 *t1;
	char buff[1000];
	buff[0] = 0;
	TRIS_DLLIST_TRAVERSE_BACKWARDS(&x->entries, t1, list) {
		strcat(buff,t1->name);
		if (t1 != TRIS_DLLIST_FIRST(&x->entries))
			strcat(buff," <=> ");
	}
	
	tris_log(LOG_NOTICE,"Got: %s  [expect %s]\n", buff, expect);
}

static struct test_container *make_cont(void)
{
	struct test_container *t = tris_calloc(sizeof(struct test_container),1);
	return t;
}

static struct test1 *make_test1(char *name)
{
	struct test1 *t1 = tris_calloc(sizeof(struct test1),1);
	strcpy(t1->name, name);
	return t1;
}

static void destroy_test_container(struct test_container *x)
{
	/* remove all the test1's */
	struct test1 *t1;
	TRIS_DLLIST_TRAVERSE_BACKWARDS_SAFE_BEGIN(&x->entries, t1, list) {
		TRIS_DLLIST_REMOVE_CURRENT(list);
		free(t1);
	}
	TRIS_DLLIST_TRAVERSE_BACKWARDS_SAFE_END;
	free(x);
}

/* Macros to test:
TRIS_DLLIST_LOCK(head)
TRIS_RWDLLIST_WRLOCK(head)
TRIS_RWDLLIST_WRLOCK(head) 
TRIS_RWDLLIST_RDLOCK(head)
TRIS_DLLIST_TRYLOCK(head)
TRIS_RWDLLIST_TRYWRLOCK(head)
TRIS_RWDLLIST_TRYRDLOCK(head)
TRIS_DLLIST_UNLOCK(head)
TRIS_RWDLLIST_UNLOCK(head)

TRIS_DLLIST_HEAD(name, type)
TRIS_RWDLLIST_HEAD(name, type)
TRIS_DLLIST_HEAD_NOLOCK(name, type)
TRIS_DLLIST_HEAD_STATIC(name, type)
TRIS_RWDLLIST_HEAD_STATIC(name, type)
TRIS_DLLIST_HEAD_NOLOCK_STATIC(name, type)
TRIS_DLLIST_HEAD_SET(head, entry)
TRIS_RWDLLIST_HEAD_SET(head, entry)
TRIS_DLLIST_HEAD_SET_NOLOCK(head, entry)
TRIS_DLLIST_HEAD_INIT(head)
TRIS_RWDLLIST_HEAD_INIT(head)
TRIS_DLLIST_HEAD_INIT_NOLOCK(head)

TRIS_RWDLLIST_HEAD_DESTROY(head)

TRIS_DLLIST_ENTRY(type)

--- the above not going to be dealt with here ---

TRIS_DLLIST_INSERT_HEAD(head, elm, field)
TRIS_DLLIST_TRAVERSE(head,var,field)
TRIS_DLLIST_TRAVERSE_BACKWARDS_SAFE_BEGIN(head, var, field)
TRIS_DLLIST_TRAVERSE_BACKWARDS_SAFE_END
TRIS_DLLIST_FIRST(head)
TRIS_DLLIST_LAST(head)
TRIS_DLLIST_NEXT(elm, field)
TRIS_DLLIST_PREV(elm, field)
TRIS_DLLIST_EMPTY(head)
TRIS_DLLIST_TRAVERSE_BACKWARDS(head,var,field)
TRIS_DLLIST_INSERT_AFTER(head, listelm, elm, field)
TRIS_DLLIST_INSERT_TAIL(head, elm, field)
TRIS_DLLIST_REMOVE_HEAD(head, field)
TRIS_DLLIST_REMOVE(head, elm, field)
TRIS_DLLIST_TRAVERSE_SAFE_BEGIN(head, var, field)
TRIS_DLLIST_TRAVERSE_SAFE_END
TRIS_DLLIST_REMOVE_CURRENT(field)
TRIS_DLLIST_MOVE_CURRENT(newhead, field)
TRIS_DLLIST_INSERT_BEFORE_CURRENT(elm, field) 


TRIS_DLLIST_MOVE_CURRENT_BACKWARDS(newhead, field)
TRIS_DLLIST_INSERT_BEFORE_CURRENT_BACKWARDS(elm, field)
TRIS_DLLIST_HEAD_DESTROY(head)

TRIS_DLLIST_APPEND_DLLIST(head, list, field)

*/

static void dll_tests(void)
{
	struct test_container *tc;
	struct test1 *a;
	struct test1 *b;
	struct test1 *c;
	struct test1 *d;
	struct test1 *e;
	
	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_INSERT_HEAD, TRIS_DLLIST_TRAVERSE, TRIS_DLLIST_TRAVERSE_BACKWARDS_SAFE_BEGIN, TRIS_DLLIST_TRAVERSE_BACKWARDS_SAFE_END\n");
	tc = make_cont();
	a = make_test1("A");
	b = make_test1("B");
	c = make_test1("C");
	d = make_test1("D");
	TRIS_DLLIST_INSERT_HEAD(&tc->entries, d, list);
	TRIS_DLLIST_INSERT_HEAD(&tc->entries, c, list);
	TRIS_DLLIST_INSERT_HEAD(&tc->entries, b, list);
	TRIS_DLLIST_INSERT_HEAD(&tc->entries, a, list);
	print_list(tc, "A <=> B <=> C <=> D");

	destroy_test_container(tc);
	
	tc = make_cont();

	if (TRIS_DLLIST_EMPTY(&tc->entries))
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_EMPTY....OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_EMPTY....PROBLEM!!\n");


	a = make_test1("A");
	b = make_test1("B");
	c = make_test1("C");
	d = make_test1("D");
	
	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_INSERT_TAIL\n");
	TRIS_DLLIST_INSERT_TAIL(&tc->entries, a, list);
	TRIS_DLLIST_INSERT_TAIL(&tc->entries, b, list);
	TRIS_DLLIST_INSERT_TAIL(&tc->entries, c, list);
	TRIS_DLLIST_INSERT_TAIL(&tc->entries, d, list);
	print_list(tc, "A <=> B <=> C <=> D");

	if (TRIS_DLLIST_FIRST(&tc->entries) == a)
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_FIRST....OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_FIRST....PROBLEM\n");

	if (TRIS_DLLIST_LAST(&tc->entries) == d)
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_LAST....OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_LAST....PROBLEM\n");

	if (TRIS_DLLIST_NEXT(a,list) == b)
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_NEXT....OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_NEXT....PROBLEM\n");

	if (TRIS_DLLIST_PREV(d,list) == c)
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_PREV....OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_PREV....PROBLEM\n");

	destroy_test_container(tc);

	tc = make_cont();

	a = make_test1("A");
	b = make_test1("B");
	c = make_test1("C");
	d = make_test1("D");

	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_INSERT_AFTER, TRIS_DLLIST_TRAVERSE_BACKWARDS\n");
	TRIS_DLLIST_INSERT_HEAD(&tc->entries, a, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, a, b, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, b, c, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, c, d, list);
	print_list_backwards(tc, "D <=> C <=> B <=> A");

	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE_HEAD\n");
	TRIS_DLLIST_REMOVE_HEAD(&tc->entries, list);
	print_list_backwards(tc, "D <=> C <=> B");
	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE_HEAD\n");
	TRIS_DLLIST_REMOVE_HEAD(&tc->entries, list);
	print_list_backwards(tc, "D <=> C");
	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE_HEAD\n");
	TRIS_DLLIST_REMOVE_HEAD(&tc->entries, list);
	print_list_backwards(tc, "D");
	TRIS_DLLIST_REMOVE_HEAD(&tc->entries, list);

	if (TRIS_DLLIST_EMPTY(&tc->entries))
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE_HEAD....OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE_HEAD....PROBLEM!!\n");

	TRIS_DLLIST_INSERT_HEAD(&tc->entries, a, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, a, b, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, b, c, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, c, d, list);

	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE\n");
	TRIS_DLLIST_REMOVE(&tc->entries, c, list);
	print_list(tc, "A <=> B <=> D");
	TRIS_DLLIST_REMOVE(&tc->entries, a, list);
	print_list(tc, "B <=> D");
	TRIS_DLLIST_REMOVE(&tc->entries, d, list);
	print_list(tc, "B");
	TRIS_DLLIST_REMOVE(&tc->entries, b, list);
	
	if (TRIS_DLLIST_EMPTY(&tc->entries))
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE....OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE....PROBLEM!!\n");

	TRIS_DLLIST_INSERT_HEAD(&tc->entries, a, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, a, b, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, b, c, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, c, d, list);

	TRIS_DLLIST_TRAVERSE_SAFE_BEGIN(&tc->entries, e, list) {
		TRIS_DLLIST_REMOVE_CURRENT(list);
	}
	TRIS_DLLIST_TRAVERSE_SAFE_END;
	if (TRIS_DLLIST_EMPTY(&tc->entries))
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE_CURRENT... OK\n");
	else
		tris_log(LOG_NOTICE,"Test TRIS_DLLIST_REMOVE_CURRENT... PROBLEM\n");
	
	tris_log(LOG_NOTICE,"Test TRIS_DLLIST_MOVE_CURRENT, TRIS_DLLIST_INSERT_BEFORE_CURRENT\n");
	TRIS_DLLIST_INSERT_HEAD(&tc->entries, a, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, a, b, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, b, c, list);
	TRIS_DLLIST_TRAVERSE_SAFE_BEGIN(&tc->entries, e, list) {
		if (e == a) {
			TRIS_DLLIST_INSERT_BEFORE_CURRENT(d, list);  /* D A B C */
		}
		
		if (e == b) {
			TRIS_DLLIST_MOVE_CURRENT(&tc->entries, list); /* D A C B */
		}
		
	}
	TRIS_DLLIST_TRAVERSE_SAFE_END;
	print_list(tc, "D <=> A <=> C <=> B");
	
	destroy_test_container(tc);

	tc = make_cont();

	a = make_test1("A");
	b = make_test1("B");
	c = make_test1("C");
	d = make_test1("D");

	tris_log(LOG_NOTICE,"Test: TRIS_DLLIST_MOVE_CURRENT_BACKWARDS and TRIS_DLLIST_INSERT_BEFORE_CURRENT_BACKWARDS\n");
	TRIS_DLLIST_INSERT_HEAD(&tc->entries, a, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, a, b, list);
	TRIS_DLLIST_INSERT_AFTER(&tc->entries, b, c, list);
	TRIS_DLLIST_TRAVERSE_BACKWARDS_SAFE_BEGIN(&tc->entries, e, list) {
		if (e == c && TRIS_DLLIST_FIRST(&tc->entries) != c) {
			TRIS_DLLIST_MOVE_CURRENT_BACKWARDS(&tc->entries, list); /* C A B */
			print_list(tc, "C <=> A <=> B");
		}

		if (e == b) {
			TRIS_DLLIST_REMOVE_CURRENT(list);  /* C A */
			print_list(tc, "C <=> A");
		}
		if (e == a) {
			TRIS_DLLIST_INSERT_BEFORE_CURRENT_BACKWARDS(d, list); /* C A D */
			print_list(tc, "C <=> A <=> D");
		}
		
	}
	TRIS_DLLIST_TRAVERSE_SAFE_END;
	print_list(tc, "C <=> A <=> D");

}

static int unload_module(void)
{
	return 0;
}

static int load_module(void)
{
	dll_tests();
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Test Doubly-Linked Lists");
