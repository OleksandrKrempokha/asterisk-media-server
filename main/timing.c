/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008 - 2009, Digium, Inc.
 *
 * Kevin P. Fleming <kpfleming@digium.com>
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
 * \brief Timing source management
 *
 * \author Kevin P. Fleming <kpfleming@digium.com>
 * \author Russell Bryant <russell@digium.com>
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 234575 $")

#include "trismedia/_private.h"

#include "trismedia/timing.h"
#include "trismedia/lock.h"
#include "trismedia/cli.h"
#include "trismedia/utils.h"
#include "trismedia/time.h"
#include "trismedia/heap.h"
#include "trismedia/module.h"
#include "trismedia/poll-compat.h"

struct timing_holder {
	/*! Do _not_ move this from the beginning of the struct. */
	ssize_t __heap_index;
	struct tris_module *mod;
	struct tris_timing_interface *iface;
};

static struct tris_heap *timing_interfaces;

struct tris_timer {
	int fd;
	struct timing_holder *holder;
};

static int timing_holder_cmp(void *_h1, void *_h2)
{
	struct timing_holder *h1 = _h1;
	struct timing_holder *h2 = _h2;

	if (h1->iface->priority > h2->iface->priority) {
		return 1;
	} else if (h1->iface->priority == h2->iface->priority) {
		return 0;
	} else {
		return -1;
	}
}

void *_tris_register_timing_interface(struct tris_timing_interface *funcs, 
				     struct tris_module *mod)
{
	struct timing_holder *h;

	if (!funcs->timer_open ||
	    !funcs->timer_close ||
	    !funcs->timer_set_rate ||
	    !funcs->timer_ack ||
	    !funcs->timer_get_event ||
	    !funcs->timer_get_max_rate ||
	    !funcs->timer_enable_continuous ||
	    !funcs->timer_disable_continuous) {
		return NULL;
	}

	if (!(h = tris_calloc(1, sizeof(*h)))) {
		return NULL;
	}

	h->iface = funcs;
	h->mod = mod;

	tris_heap_wrlock(timing_interfaces);
	tris_heap_push(timing_interfaces, h);
	tris_heap_unlock(timing_interfaces);

	return h;
}

int tris_unregister_timing_interface(void *handle)
{
	struct timing_holder *h = handle;
	int res = -1;

	tris_heap_wrlock(timing_interfaces);
	h = tris_heap_remove(timing_interfaces, h);
	tris_heap_unlock(timing_interfaces);

	if (h) {
		tris_free(h);
		h = NULL;
		res = 0;
	}

	return res;
}

struct tris_timer *tris_timer_open(void)
{
	int fd = -1;
	struct timing_holder *h;
	struct tris_timer *t = NULL;

	tris_heap_rdlock(timing_interfaces);

	if ((h = tris_heap_peek(timing_interfaces, 1))) {
		fd = h->iface->timer_open();
		tris_module_ref(h->mod);
	}

	if (fd != -1) {
		if (!(t = tris_calloc(1, sizeof(*t)))) {
			h->iface->timer_close(fd);
		} else {
			t->fd = fd;
			t->holder = h;
		}
	}

	tris_heap_unlock(timing_interfaces);

	return t;
}

void tris_timer_close(struct tris_timer *handle)
{
	handle->holder->iface->timer_close(handle->fd);
	tris_module_unref(handle->holder->mod);
	tris_free(handle);
}

int tris_timer_fd(const struct tris_timer *handle)
{
	return handle->fd;
}

int tris_timer_set_rate(const struct tris_timer *handle, unsigned int rate)
{
	int res = -1;

	res = handle->holder->iface->timer_set_rate(handle->fd, rate);

	return res;
}

void tris_timer_ack(const struct tris_timer *handle, unsigned int quantity)
{
	handle->holder->iface->timer_ack(handle->fd, quantity);
}

int tris_timer_enable_continuous(const struct tris_timer *handle)
{
	int res = -1;

	res = handle->holder->iface->timer_enable_continuous(handle->fd);

	return res;
}

int tris_timer_disable_continuous(const struct tris_timer *handle)
{
	int res = -1;

	res = handle->holder->iface->timer_disable_continuous(handle->fd);

	return res;
}

enum tris_timer_event tris_timer_get_event(const struct tris_timer *handle)
{
	enum tris_timer_event res = -1;

	res = handle->holder->iface->timer_get_event(handle->fd);

	return res;
}

unsigned int tris_timer_get_max_rate(const struct tris_timer *handle)
{
	unsigned int res = 0;

	res = handle->holder->iface->timer_get_max_rate(handle->fd);

	return res;
}

static char *timing_test(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_timer *timer;
	int count = 0;
	struct timeval start, end;
	unsigned int test_rate = 50;

	switch (cmd) {
	case CLI_INIT:
		e->command = "timing test";
		e->usage = "Usage: timing test <rate>\n"
		           "   Test a timer with a specified rate, 50/sec by default.\n"
		           "";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 2 && a->argc != 3) {
		return CLI_SHOWUSAGE;
	}

	if (a->argc == 3) {
		unsigned int rate;
		if (sscanf(a->argv[2], "%30u", &rate) == 1) {
			test_rate = rate;
		} else {
			tris_cli(a->fd, "Invalid rate '%s', using default of %u\n", a->argv[2], test_rate);	
		}
	}

	tris_cli(a->fd, "Attempting to test a timer with %u ticks per second.\n", test_rate);

	if (!(timer = tris_timer_open())) {
		tris_cli(a->fd, "Failed to open timing fd\n");
		return CLI_FAILURE;
	}

	tris_cli(a->fd, "Using the '%s' timing module for this test.\n", timer->holder->iface->name);

	start = tris_tvnow();

	tris_timer_set_rate(timer, test_rate);

	while (tris_tvdiff_ms((end = tris_tvnow()), start) < 1000) {
		int res;
		struct pollfd pfd = {
			.fd = tris_timer_fd(timer),
			.events = POLLIN | POLLPRI,
		};

		res = tris_poll(&pfd, 1, 100);

		if (res == 1) {
			count++;
			tris_timer_ack(timer, 1);
		} else if (!res) {
			tris_cli(a->fd, "poll() timed out!  This is bad.\n");
		} else if (errno != EAGAIN && errno != EINTR) {
			tris_cli(a->fd, "poll() returned error: %s\n", strerror(errno));
		}
	}

	tris_timer_close(timer);

	tris_cli(a->fd, "It has been %d milliseconds, and we got %d timer ticks\n", 
		tris_tvdiff_ms(end, start), count);

	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_timing[] = {
	TRIS_CLI_DEFINE(timing_test, "Run a timing test"),
};

int tris_timing_init(void)
{
	if (!(timing_interfaces = tris_heap_create(2, timing_holder_cmp, 0))) {
		return -1;
	}

	return tris_cli_register_multiple(cli_timing, ARRAY_LEN(cli_timing));
}
