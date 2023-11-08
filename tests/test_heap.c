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

/*! \file
 *
 * \brief Heap data structure test module
 *
 * \author Russell Bryant <russell@digium.com>
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 187677 $")

#include "trismedia/module.h"
#include "trismedia/cli.h"
#include "trismedia/utils.h"
#include "trismedia/heap.h"

struct node {
	long val;
	size_t index;
};

static int node_cmp(void *_n1, void *_n2)
{
	struct node *n1 = _n1;
	struct node *n2 = _n2;

	if (n1->val < n2->val) {
		return -1;
	} else if (n1->val == n2->val) {
		return 0;
	} else {
		return 1;
	}
}

static int test1(int fd)
{
	struct tris_heap *h;
	struct node *obj;
	struct node nodes[3] = {
		{ 1, },
		{ 2, },
		{ 3, },
	};

	if (!(h = tris_heap_create(8, node_cmp, offsetof(struct node, index)))) {
		return -1;
	}

	/* Pushing 1 2 3, and then popping 3 elements */

	tris_cli(fd, "Test #1 - Push a few elements onto a heap and make sure that they "
			"come back off in the right order.\n");

	tris_heap_push(h, &nodes[0]);

	tris_heap_push(h, &nodes[1]);

	tris_heap_push(h, &nodes[2]);

	obj = tris_heap_pop(h);
	if (obj->val != 3) {
		return -2;
	}

	obj = tris_heap_pop(h);
	if (obj->val != 2) {
		return -3;
	}

	obj = tris_heap_pop(h);
	if (obj->val != 1) {
		return -4;
	}

	obj = tris_heap_pop(h);
	if (obj) {
		return -5;
	}

	h = tris_heap_destroy(h);

	tris_cli(fd, "Test #1 successful.\n");

	return 0;
}

static int test2(int fd)
{
	struct tris_heap *h = NULL;
	static const unsigned int one_million = 1000000;
	struct node *nodes = NULL;
	struct node *node;
	unsigned int i = one_million;
	long last = LONG_MAX, cur;
	int res = 0;

	tris_cli(fd, "Test #2 - Push a million random elements on to a heap, "
			"verify that the heap has been properly constructed, "
			"and then ensure that the elements are come back off in the proper order\n");

	if (!(nodes = tris_malloc(one_million * sizeof(*node)))) {
		res = -1;
		goto return_cleanup;
	}

	if (!(h = tris_heap_create(20, node_cmp, offsetof(struct node, index)))) {
		res = -2;
		goto return_cleanup;
	}

	while (i--) {
		nodes[i].val = tris_random();
		tris_heap_push(h, &nodes[i]);
	}

	if (tris_heap_verify(h)) {
		res = -3;
		goto return_cleanup;
	}

	i = 0;
	while ((node = tris_heap_pop(h))) {
		cur = node->val;
		if (cur > last) {
			tris_cli(fd, "i: %u, cur: %ld, last: %ld\n", i, cur, last);
			res = -4;
			goto return_cleanup;
		}
		last = cur;
		i++;
	}

	if (i != one_million) {
		tris_cli(fd, "Stopped popping off after only getting %u nodes\n", i);
		res = -5;
		goto return_cleanup;
	}

	tris_cli(fd, "Test #2 successful.\n");

return_cleanup:
	if (h) {
		h = tris_heap_destroy(h);
	}
	if (nodes) {
		tris_free(nodes);
	}

	return res;
}

static char *handle_cli_heap_test(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "heap test";
		e->usage = ""
			"Usage: heap test\n"
			"";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args) {
		return CLI_SHOWUSAGE;
	}

	if ((res = test1(a->fd))) {
		tris_cli(a->fd, "Test 1 failed! (%d)\n", res);
		return CLI_FAILURE;
	}

	if ((res = test2(a->fd))) {
		tris_cli(a->fd, "Test 2 failed! (%d)\n", res);
		return CLI_FAILURE;
	}

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_heap[] = {
	TRIS_CLI_DEFINE(handle_cli_heap_test, "Test the heap implementation"),
};

static int unload_module(void)
{
	tris_cli_unregister_multiple(cli_heap, ARRAY_LEN(cli_heap));
	return 0;
}

static int load_module(void)
{
	tris_cli_register_multiple(cli_heap, ARRAY_LEN(cli_heap));
	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Heap test module");
