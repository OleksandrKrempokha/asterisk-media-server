/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Digium, Inc.
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
 * \author Russell Bryant <russell@digium.com>
 *
 * \brief DAHDI timing interface 
 */

/*** MODULEINFO
	<depend>dahdi</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 199744 $");

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>

#include <dahdi/user.h>

#include "trismedia/module.h"
#include "trismedia/timing.h"
#include "trismedia/utils.h"

static void *timing_funcs_handle;

static int dahdi_timer_open(void);
static void dahdi_timer_close(int handle);
static int dahdi_timer_set_rate(int handle, unsigned int rate);
static void dahdi_timer_ack(int handle, unsigned int quantity);
static int dahdi_timer_enable_continuous(int handle);
static int dahdi_timer_disable_continuous(int handle);
static enum tris_timer_event dahdi_timer_get_event(int handle);
static unsigned int dahdi_timer_get_max_rate(int handle);

static struct tris_timing_interface dahdi_timing = {
	.name = "DAHDI",
	.priority = 100,
	.timer_open = dahdi_timer_open,
	.timer_close = dahdi_timer_close,
	.timer_set_rate = dahdi_timer_set_rate,
	.timer_ack = dahdi_timer_ack,
	.timer_enable_continuous = dahdi_timer_enable_continuous,
	.timer_disable_continuous = dahdi_timer_disable_continuous,
	.timer_get_event = dahdi_timer_get_event,
	.timer_get_max_rate = dahdi_timer_get_max_rate,
};

static int dahdi_timer_open(void)
{
	return open("/dev/dahdi/timer", O_RDWR);
}

static void dahdi_timer_close(int handle)
{
	close(handle);
}

static int dahdi_timer_set_rate(int handle, unsigned int rate)
{
	int samples;

	/* DAHDI timers are configured using a number of samples,
	 * based on an 8 kHz sample rate. */
	samples = (unsigned int) roundf((8000.0 / ((float) rate)));

	if (ioctl(handle, DAHDI_TIMERCONFIG, &samples)) {
		tris_log(LOG_ERROR, "Failed to configure DAHDI timing fd for %u sample timer ticks\n",
			samples);
		return -1;
	}

	return 0;
}

static void dahdi_timer_ack(int handle, unsigned int quantity)
{
	ioctl(handle, DAHDI_TIMERACK, &quantity);
}

static int dahdi_timer_enable_continuous(int handle)
{
	int flags = 1;

	return ioctl(handle, DAHDI_TIMERPING, &flags) ? -1 : 0;
}

static int dahdi_timer_disable_continuous(int handle)
{
	int flags = -1;

	return ioctl(handle, DAHDI_TIMERPONG, &flags) ? -1 : 0;
}

static enum tris_timer_event dahdi_timer_get_event(int handle)
{
	int res;
	int event;

	res = ioctl(handle, DAHDI_GETEVENT, &event);

	if (res) {
		event = DAHDI_EVENT_TIMER_EXPIRED;
	}

	switch (event) {
	case DAHDI_EVENT_TIMER_PING:
		return TRIS_TIMING_EVENT_CONTINUOUS;
	case DAHDI_EVENT_TIMER_EXPIRED:
	default:
		return TRIS_TIMING_EVENT_EXPIRED;	
	}
}

static unsigned int dahdi_timer_get_max_rate(int handle)
{
	return 1000;
}

#define SEE_TIMING "For more information on Trismedia timing modules, including ways to potentially fix this problem, please see doc/timing.txt\n"

static int dahdi_test_timer(void)
{
	int fd;
	int x = 160;
	
	fd = open("/dev/dahdi/timer", O_RDWR);

	if (fd < 0) {
		return -1;
	}

	if (ioctl(fd, DAHDI_TIMERCONFIG, &x)) {
		tris_log(LOG_ERROR, "You have DAHDI built and drivers loaded, but the DAHDI timer test failed to set DAHDI_TIMERCONFIG to %d.\n" SEE_TIMING, x);
		close(fd);
		return -1;
	}

	if ((x = tris_wait_for_input(fd, 300)) < 0) {
		tris_log(LOG_ERROR, "You have DAHDI built and drivers loaded, but the DAHDI timer could not be polled during the DAHDI timer test.\n" SEE_TIMING);
		close(fd);
		return -1;
	}

	if (!x) {
		const char dahdi_timer_error[] = {
			"Trismedia has detected a problem with your DAHDI configuration and will shutdown for your protection.  You have options:"
			"\n\t1. You only have to compile DAHDI support into Trismedia if you need it.  One option is to recompile without DAHDI support."
			"\n\t2. You only have to load DAHDI drivers if you want to take advantage of DAHDI services.  One option is to unload DAHDI modules if you don't need them."
			"\n\t3. If you need DAHDI services, you must correctly configure DAHDI."
		};
		tris_log(LOG_ERROR, "%s\n" SEE_TIMING, dahdi_timer_error);
		usleep(100);
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static int load_module(void)
{
	if (dahdi_test_timer()) {
		return TRIS_MODULE_LOAD_DECLINE;
	}

	return (timing_funcs_handle = tris_register_timing_interface(&dahdi_timing)) ?
		TRIS_MODULE_LOAD_SUCCESS : TRIS_MODULE_LOAD_DECLINE;
}

static int unload_module(void)
{
	if (timing_funcs_handle) {
		return tris_unregister_timing_interface(timing_funcs_handle);
	}

	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_LOAD_ORDER, "DAHDI Timing Interface",
		.load = load_module,
		.unload = unload_module,
		.load_pri = 10,
		);
