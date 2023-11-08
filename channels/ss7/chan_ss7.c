/* chan_ss7.c - Implementation of SS7 (MTP2, MTP3, and ISUP) for Trismedia.
 *
 * Copyright (C) 2005-2006, Sifira A/S.
 *
 * Author: Kristian Nielsen <kn@sifira.dk>,
 *         Anders Baekgaard <ab@sifira.dk>
 *         Anders Baekgaard <ab@dicea.dk>
 *
 * This file is part of chan_ss7.
 *
 * chan_ss7 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * chan_ss7 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with chan_ss7; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <unistd.h>
#include <sys/poll.h>
#include <netinet/in.h>

#include "trismedia/autoconfig.h"
#include "trismedia/compiler.h"
#include "trismedia/channel.h"
#include "trismedia/module.h"
#include "trismedia/logger.h"
#include "trismedia/options.h"
#include "trismedia/utils.h"
#include "trismedia/sched.h"
#include "trismedia/cli.h"
#include "trismedia/lock.h"
#include "trismedia/buildopts.h"

#include "trisversion.h"
#include "config.h"
#include "lffifo.h"
#include "utils.h"
#include "mtp.h"
#include "transport.h"
#include "isup.h"
#include "l4isup.h"
#include "cluster.h"
#include "mtp3io.h"
#include "trisstubs.h"

#ifdef USE_TRISMEDIA_1_2
#define TRIS_MODULE_LOAD_SUCCESS  0
#define TRIS_MODULE_LOAD_DECLINE  1
#define TRIS_MODULE_LOAD_FAILURE -1
#endif

/* Send fifo for sending control requests to the MTP thread.
   The fifo is lock-free (one thread may put and another get simultaneously),
   but multiple threads doing put must be serialized with this mutex. */
TRIS_MUTEX_DEFINE_STATIC(mtp_control_mutex);
static struct lffifo *mtp_control_fifo = NULL;

/* This is the MTP2/MTP3 thread, which runs at high real-time priority
   and is careful not to wait for locks in order not to loose MTP
   frames. */
static pthread_t mtp_thread = TRIS_PTHREADT_NULL;
static int mtp_thread_running = 0;


/* This is the monitor thread which mainly handles scheduling/timeouts. */
static pthread_t monitor_thread = TRIS_PTHREADT_NULL;
static int monitor_running = 0;



/* State for dumps. */
TRIS_MUTEX_DEFINE_STATIC(dump_mutex);
static FILE *dump_in_fh = NULL;
static FILE *dump_out_fh = NULL;
static int dump_do_fisu, dump_do_lssu, dump_do_msu;


static const char desc[] = "SS7 Protocol Support";
static const char config[] = "ss7.conf";




static int cmd_version(int fd, int argc, char *argv[]);
static int cmd_dump_status(int fd, int argc, char *argv[]);
static int cmd_dump_stop(int fd, int argc, char *argv[]);
static int cmd_dump_start(int fd, int argc, char *argv[]);
static char *complete_dump_stop(const char *line, const char *word, int pos, int state);
static char *complete_dump_start(const char *line, const char *word, int pos, int state);
static int cmd_link_up(int fd, int argc, char *argv[]);
static int cmd_link_down(int fd, int argc, char *argv[]);
static int cmd_link_status(int fd, int argc, char *argv[]);
static int cmd_ss7_status(int fd, int argc, char *argv[]);

static void dump_pcap(FILE *f, struct mtp_event *event)
{
  unsigned int sec  = event->dump.stamp.tv_sec;
  unsigned int usec  = event->dump.stamp.tv_usec - (event->dump.stamp.tv_usec % 1000) +
    event->dump.slinkno*2 + /* encode link number in usecs */
    event->dump.out /* encode direction in/out */;

  fwrite(&sec, sizeof(sec), 1, f);
  fwrite(&usec, sizeof(usec), 1, f);
  fwrite(&event->len, sizeof(event->len), 1, f); /* number of bytes of packet in file */
  fwrite(&event->len, sizeof(event->len), 1, f); /* actual length of packet */
  fwrite(event->buf, 1, event->len, f);
  fflush(f);
}

static void init_pcap_file(FILE *f)
{
  unsigned int magic = 0xa1b2c3d4;  /* text2pcap does this */
  unsigned short version_major = 2;
  unsigned short version_minor = 4;
  unsigned int thiszone = 0;
  unsigned int sigfigs = 0;
  unsigned int snaplen = 102400;
  unsigned int linktype = 140;

  fwrite(&magic, sizeof(magic), 1, f);
  fwrite(&version_major, sizeof(version_major), 1, f);
  fwrite(&version_minor, sizeof(version_minor), 1, f);
  fwrite(&thiszone, sizeof(thiszone), 1, f);
  fwrite(&sigfigs, sizeof(sigfigs), 1, f);
  fwrite(&snaplen, sizeof(snaplen), 1, f);
  fwrite(&linktype, sizeof(linktype), 1, f);
}

/* Queue a request to the MTP thread. */
static void mtp_enqueue_control(struct mtp_req *req) {
  int res;

  tris_mutex_lock(&mtp_control_mutex);
  res = lffifo_put(mtp_control_fifo, (unsigned char *)req, sizeof(struct mtp_req) + req->len);
  tris_mutex_unlock(&mtp_control_mutex);
  if(res != 0) {
    tris_log(LOG_WARNING, "MTP control fifo full (MTP thread hanging?).\n");
  }
}


static int start_mtp_thread(void)
{
  return start_thread(&mtp_thread, mtp_thread_main, &mtp_thread_running, 15);
}

static void stop_mtp_thread(void)
{
    mtp_thread_signal_stop();
    stop_thread(&mtp_thread, &mtp_thread_running);
}

static int cmd_link_up_down(int fd, int argc, char *argv[], int updown) {
  static unsigned char buf[sizeof(struct mtp_req)];
  struct mtp_req *req = (struct mtp_req *)buf;
  int i;

  req->typ = updown;
  req->len = sizeof(req->link);
  if(argc > 3) {
    for (i = 3; i < argc; i++) {
      int link_ix = atoi(argv[i]);
      tris_log(LOG_DEBUG, "MTP control link %s %d\n", updown == MTP_REQ_LINK_UP ? "up" : "down", link_ix);
      if (link_ix >= this_host->n_schannels) {
	tris_log(LOG_ERROR, "Link index out of range %d, max %d.\n", link_ix, this_host->n_schannels);
	return RESULT_FAILURE;
      }
      req->link.link_ix = link_ix;
      mtp_enqueue_control(req);
    }
  }
  else {
    for (i=0; i < this_host->n_schannels; i++) {
      tris_log(LOG_DEBUG, "MTP control link %s %d\n", updown == MTP_REQ_LINK_UP ? "up" : "down", i);
      req->link.link_ix = i;
      mtp_enqueue_control(req);
    }
  }
  return RESULT_SUCCESS;
}


static int cmd_link_down(int fd, int argc, char *argv[]) {
  return cmd_link_up_down(fd, argc, argv, MTP_REQ_LINK_DOWN);
}


static int cmd_link_up(int fd, int argc, char *argv[]) {
  return cmd_link_up_down(fd, argc, argv, MTP_REQ_LINK_UP);
}


static int cmd_link_status(int fd, int argc, char *argv[]) {
  char buff[256];
  int i;

  for (i = 0; i < this_host->n_schannels; i++) {
    if (mtp_cmd_linkstatus(buff, i) == 0)
      tris_cli(fd, buff);
  }
  return RESULT_SUCCESS;
}


static char *complete_generic(const char *word, int state, char **options, int entries) {
  int which = 0;
  int i;

  for(i = 0; i < entries; i++) {
    if(0 == strncasecmp(word, options[i], strlen(word))) {
      if(++which > state) {
        return strdup(options[i]);
      }
    }
  }
  return NULL;
}

static char *dir_options[] = { "in", "out", "both", };
static char *filter_options[] = { "fisu", "lssu", "msu", };

static char *complete_dump_start(const char *line, const char *word, int pos, int state)
{
  if(pos == 4) {
    return complete_generic(word, state, dir_options,
                            sizeof(dir_options)/sizeof(dir_options[0]));
  } else if(pos > 4) {
    return complete_generic(word, state, filter_options,
                            sizeof(filter_options)/sizeof(filter_options[0]));
  } else {
    /* We won't attempt to complete file names, that's not worth it. */
    return NULL;
  }
}

static char *complete_dump_stop(const char *line, const char *word, int pos, int state)
{
  if(pos == 3) {
    return complete_generic(word, state, dir_options,
                            sizeof(dir_options)/sizeof(dir_options[0]));
  } else {
    return NULL;
  }
}

static int cmd_dump_start(int fd, int argc, char *argv[]) {
  int in, out;
  int i;
  int fisu,lssu,msu;
  FILE *fh;

  if(argc < 4) {
    return RESULT_SHOWUSAGE;
  }

  if(argc == 4) {
    in = 1;
    out = 1;
  } else {
    if(0 == strcasecmp(argv[4], "in")) {
      in = 1;
      out = 0;
    } else if(0 == strcasecmp(argv[4], "out")) {
      in = 0;
      out = 1;
    } else if(0 == strcasecmp(argv[4], "both")) {
      in = 1;
      out = 1;
    } else {
      return RESULT_SHOWUSAGE;
    }
  }

  tris_mutex_lock(&dump_mutex);
  if((in && dump_in_fh != NULL) || (out && dump_out_fh != NULL)) {
    tris_cli(fd, "Dump already running, must be stopped (with 'ss7 stop dump') "
            "before new can be started.\n");
    tris_mutex_unlock(&dump_mutex);
    return RESULT_FAILURE;
  }

  if(argc <= 5) {
    fisu = 0;
    lssu = 0;
    msu = 1;
  } else {
    fisu = 0;
    lssu = 0;
    msu = 0;
    for(i = 5; i < argc; i++) {
      if(0 == strcasecmp(argv[i], "fisu")) {
        fisu = 1;
      } else if(0 == strcasecmp(argv[i], "lssu")) {
        lssu = 1;
      } else if(0 == strcasecmp(argv[i], "msu")) {
        msu = 1;
      } else {
        tris_mutex_unlock(&dump_mutex);
        return RESULT_SHOWUSAGE;
      }
    }
  }

  fh = fopen(argv[3], "w");
  if(fh == NULL) {
    tris_cli(fd, "Error opening file '%s': %s.\n", argv[3], strerror(errno));
    tris_mutex_unlock(&dump_mutex);
    return RESULT_FAILURE;
  }

  if(in) {
    dump_in_fh = fh;
  }
  if(out) {
    dump_out_fh = fh;
  }
  dump_do_fisu = fisu;
  dump_do_lssu = lssu;
  dump_do_msu = msu;
  init_pcap_file(fh);

  tris_mutex_unlock(&dump_mutex);
  return RESULT_SUCCESS;
}

static int cmd_dump_stop(int fd, int argc, char *argv[]) {
  int in, out;

  if(argc == 3) {
    in = 1;
    out = 1;
  } else if(argc == 4) {
    if(0 == strcasecmp(argv[3], "in")) {
      in = 1;
      out = 0;
    } else if(0 == strcasecmp(argv[3], "out")) {
      in = 0;
      out = 1;
    } else if(0 == strcasecmp(argv[3], "both")) {
      in = 1;
      out = 1;
    } else {
      return RESULT_SHOWUSAGE;
    }
  } else {
    return RESULT_SHOWUSAGE;
  }

  tris_mutex_lock(&dump_mutex);

  if((in && !out && dump_in_fh == NULL) ||
     (out && !in && dump_out_fh == NULL) ||
     (in && out && dump_in_fh == NULL && dump_out_fh == NULL)) {
    tris_cli(fd, "No dump running.\n");
    tris_mutex_unlock(&dump_mutex);
    return RESULT_SUCCESS;
  }

  if(in && dump_in_fh != NULL) {
    if(dump_out_fh == dump_in_fh) {
      /* Avoid closing it twice. */
      dump_out_fh = NULL;
    }
    fclose(dump_in_fh);
    dump_in_fh = NULL;
  }
  if(out && dump_out_fh != NULL) {
    fclose(dump_out_fh);
    dump_out_fh = NULL;
  }

  tris_mutex_unlock(&dump_mutex);
  return RESULT_SUCCESS;
}

static int cmd_dump_status(int fd, int argc, char *argv[]) {
  tris_mutex_lock(&dump_mutex);

  /* ToDo: This doesn't seem to work, the output is getting lost somehow.
     Not sure why, but could be related to tris_carefulwrite() called in
     tris_cli(). */
  tris_cli(fd, "Yuck! what is going on here?!?\n");
  if(dump_in_fh != NULL) {
    tris_cli(fd, "Dump of incoming frames is running.\n");
  }
  if(dump_out_fh != NULL) {
    tris_cli(fd, "Dump of outgoing frames is running.\n");
  }
  if(dump_in_fh != NULL || dump_out_fh != NULL) {
    tris_cli(fd, "Filter:%s%s%s.\n",
            (dump_do_fisu ? " fisu" : ""),
            (dump_do_lssu ? " lssu" : ""),
            (dump_do_msu ? " msu" : ""));
  }

  tris_mutex_unlock(&dump_mutex);
  return RESULT_SUCCESS;
}


static int cmd_version(int fd, int argc, char *argv[])
{
  tris_cli(fd, "chan_ss7 version %s\n", CHAN_SS7_VERSION);

  return RESULT_SUCCESS;
}


static int cmd_ss7_status(int fd, int argc, char *argv[])
{
  cmd_linkset_status(fd, argc, argv);
  return RESULT_SUCCESS;
}


static void process_event(struct mtp_event* event)
{
  FILE *dump_fh;

  switch(event->typ) {
  case MTP_EVENT_ISUP:
    l4isup_event(event);
    break;
  case MTP_EVENT_SCCP:
    break;
  case MTP_EVENT_REQ_REGISTER:
    if (event->regist.ss7_protocol == 5) {
      struct link* link = &links[event->regist.isup.slinkix];
      mtp3_register_isup(link->mtp3fd, link->linkix);
    }
    break;
  case MTP_EVENT_LOG:
    tris_log(event->log.level, event->log.file, event->log.line,
	    event->log.function, "%s", event->buf);
    break;

  case MTP_EVENT_DUMP:
    tris_mutex_lock(&dump_mutex);

    if(event->dump.out) {
      dump_fh = dump_out_fh;
    } else {
      dump_fh = dump_in_fh;
    }
    if(dump_fh != NULL) {
      if(event->len < 3 ||
	 ( !(event->buf[2] == 0 && !dump_do_fisu) &&
	   !((event->buf[2] == 1 || event->buf[2] == 2) && !dump_do_lssu) &&
	   !(event->buf[2] > 2 && !dump_do_msu)))
	dump_pcap(dump_fh, event);
    }

    tris_mutex_unlock(&dump_mutex);
    break;

  case MTP_EVENT_STATUS:
    {
      struct link* link = event->status.link;
      char* name = link ? link->name : "(peer)";
      switch(event->status.link_state) {
      case MTP_EVENT_STATUS_LINK_UP:
	l4isup_link_status_change(link, 1);
	tris_log(LOG_WARNING, "MTP is now UP on link '%s'.\n", name);
	break;
      case MTP_EVENT_STATUS_LINK_DOWN:
	l4isup_link_status_change(link, 0);
	tris_log(LOG_WARNING, "MTP is now DOWN on link '%s'.\n", name);
	break;
      case MTP_EVENT_STATUS_INSERVICE:
	tris_log(LOG_WARNING, "Signaling ready for linkset '%s'.\n", link->linkset->name);
	l4isup_inservice(link);
	break;
      default:
	tris_log(LOG_NOTICE, "Unknown event type STATUS (%d), "
		"not processed.\n", event->status.link_state);
      }
    }
    break;

  default:
    tris_log(LOG_NOTICE, "Unexpected mtp event type %d.\n", event->typ);
  }
}

/* Monitor thread main loop.
   Monitor reads events from the realtime MTP thread, and processes them at
   non-realtime priority. It also handles timers for ISUP etc.
*/
static void *monitor_main(void *data) {
  int res, nres;
  struct pollfd fds[(MAX_LINKS+1)];
  int i, n_fds = 0;
  int rebuild_fds = 1;
  struct lffifo *receive_fifo = mtp_get_receive_fifo();

  tris_verbose_ss7(VERBOSE_PREFIX_3 "Starting monitor thread, pid=%d.\n", getpid());

  fds[0].fd = get_receive_pipe();
  fds[0].events = POLLIN;
  while(monitor_running) {
    if (rebuild_fds) {
      if (rebuild_fds > 1)
	poll(fds, 0, 200); /* sleep */
      rebuild_fds = 0;
      n_fds = 1;
      for (i = 0; i < n_linksets; i++) {
	struct linkset* linkset = &linksets[i];
	int j;
	for (j = 0; j < linkset->n_links; j++) {
	  int k;
	  struct link* link = linkset->links[j];
	  for (k = 0; k < this_host->n_spans; k++) {
	    if (this_host->spans[k].link == link)
	      break;
	    if ((this_host->spans[k].link->linkset == link->linkset) ||
		(is_combined_linkset(this_host->spans[k].link->linkset, link->linkset)))
	      break;
	  }
	  if (k < this_host->n_spans) {
	    if (link->remote) {
	      if (link->mtp3fd == -1) {
		link->mtp3fd = mtp3_connect_socket(link->mtp3server_host, link->mtp3server_port);
		if (link->mtp3fd != -1)
		  res = mtp3_register_isup(link->mtp3fd, link->linkix);
		if ((link->mtp3fd == -1) || (res == -1))
		  rebuild_fds += 2;
	      }
	      fds[n_fds].fd = link->mtp3fd;
	      fds[n_fds++].events = POLLIN|POLLERR|POLLNVAL|POLLHUP;
	    }
	  }
	}
      }
    }
    int timeout = timers_wait();

    nres = poll(fds, n_fds, timeout);
    if(nres < 0) {
      if(errno == EINTR) {
        /* Just try again. */
      } else {
        tris_log(LOG_ERROR, "poll() failure, errno=%d: %s\n",
                errno, strerror(errno));
      }
    } else if(nres > 0) {
      for (i = 0; (i < n_fds) && (nres > 0); i++) {
	unsigned char eventbuf[MTP_EVENT_MAX_SIZE];
	struct mtp_event *event = (struct mtp_event*) eventbuf;
	struct link* link = NULL;
	if(fds[i].revents) {
	  int j;
	  for (j = 0; j < n_links; j++) {
	    if (links[j].remote && (links[j].mtp3fd == fds[i].fd)) {
	      link = &links[j];
	      break;
	    }
	  }
	}
	else
	  continue;
	if(fds[i].revents & (POLLERR|POLLNVAL|POLLHUP)) {
	  if (i == 0) { /* receivepipe */
	    tris_log(LOG_ERROR, "poll() return bad revents for receivepipe, 0x%04x\n", fds[i].revents);
	  }
	  close(fds[i].fd);
	  if (link)
	    link->mtp3fd = -1;
	  rebuild_fds++; rebuild_fds++; /* when > 1, use short sleep */
	  nres--;
	  continue;
	}
	if(!(fds[i].revents & POLLIN))
	  continue;
	if (i == 0) {
	  /* Events waiting in the receive buffer. */
	  unsigned char dummy[512];

	  /* Empty the pipe before pulling from fifo. This way the race
	     condition between mtp and monitor threads may cause spurious
	     wakeups, but not loss/delay of messages. */
	  read(fds[i].fd, dummy, sizeof(dummy));

	  /* Process all available events. */
	  while((res = lffifo_get(receive_fifo, eventbuf, sizeof(eventbuf))) != 0) {
	    if(res < 0) {
	      tris_log(LOG_ERROR, "Yuck! oversized frame in receive fifo, bailing out.\n");
	      return NULL;
	    }
	    process_event(event);
	  }
	}
	else {
#if MTP3_SOCKET == SOCK_STREAM
	  res = read(fds[i].fd, eventbuf, sizeof(struct mtp_event));
	  if ((res > 0) && (event->len > 0)) {
	    int p = res;
	    int len = event->len;
	    if (sizeof(struct mtp_event) + event->len > MTP_EVENT_MAX_SIZE) {
	      tris_log(LOG_NOTICE, "Got too large packet: len %u, max %u, discarded", sizeof(struct mtp_event) + event->len, MTP_EVENT_MAX_SIZE);
	      len = 0;
	      res = 0;
	    }
	    do {
	      res = read(fds[i].fd, &eventbuf[p], len);
	      if (res > 0) {
		p += res;
		len -= res;
	      }
	      else if ((res < 0) && (errno != EINTR)) {
		len = 0;
	      }
	      else {
		len = 0;
	      }
	    } while (len > 0);
	  }
#else
	  res = read(fds[i].fd, eventbuf, sizeof(eventbuf)+MTP_MAX_PCK_SIZE);
#endif
	  if (res > 0) {
	    if (event->typ == MTP_EVENT_ISUP) {
	      event->isup.link = NULL;
	      event->isup.slink = &links[event->isup.slinkix];
	    }
	    process_event(event);
	  }
	  else if (res == 0) {
	    int j;
	    for (j = 0; j < n_links; j++) {
	      struct link* link = &links[j];
	      if (link->remote && (link->mtp3fd == fds[i].fd)) {
		close(fds[i].fd);
		link->mtp3fd = -1;
		rebuild_fds++;
	      }
	    }
	  }
	}
	nres--;
      }
    }

    /* We need to lock the global glock mutex around tris_sched_runq() so that
       we avoid a race with ss7_hangup. With the lock, invalidating the
       channel in ss7_hangup() and removing associated monitor_sched entries
       is an atomic operation, so that we avoid calling timer handlers with
       references to invalidated channels. */
    run_timers();
  }
  return NULL;
}


static void stop_monitor(void) {
  int i;

  if(monitor_running) {
    monitor_running = 0;
    /* Monitor wakes up every 1/2 sec, so no need to signal it explicitly. */
    pthread_join(monitor_thread, NULL);
  }
  for (i = 0; i < n_links; i++) {
    struct link* link = &links[i];
    if (link->remote && (link->mtp3fd > -1))
      close(link->mtp3fd);
  }
}

/* ================================ KimPH code block ==================================== */

static char *k_cmd_version(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 version";
		e->usage = 
			"Usage: ss7 version\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}

	if (cmd_version(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}

static char *k_cmd_dump_start(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 dump start";
		e->usage = 
			"Usage: ss7 dump start <file> [in|out|both] [fisu] [lssu] [msu]\n"
			"	Start mtp2 dump to file. Either incoming, outgoing, or both(default).\n"
			"	Optionally specify which of fisu, lssu, and msu should be dumped.\n"
			"	The output is in PCAP format(can be read by wireshark).\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		if (complete_dump_start(a->line, a->word, a->pos, a->n) != RESULT_SUCCESS) {
			tris_verbose_ss7("Oh it's failure\n");
		}
		return NULL;
	}
	if (cmd_dump_start(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}

static char *k_cmd_dump_stop(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 dump stop";
		e->usage = 
			"Usage: ss7 dump stop [in|out|both]\n"
			"	Stop mtp2 dump started with \"ss7 start dump\". Either incoming,\n"
			"	outgoing, or both(default).\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return complete_dump_stop(a->line, a->word, a->pos, a->n);
		return NULL;
	}
	
	if(cmd_dump_stop(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}

static char *k_cmd_dump_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 dump status";
		e->usage = 
			"Usage: ss7 dump status\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
	if(cmd_dump_status(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}

static char *k_cmd_link_down(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 link down";
		e->usage = 
			"Usage: ss7 link down [logical-link-no]...\n"
			"	Take the link(s) down; it will be down until started explicitly with\n"
			"	'ss7 link up'.\n"
			"	If no logical-link-no argument is given, all links are affected.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}

	if(cmd_link_down(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {	
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}

static char *k_cmd_link_up(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 link up";
		e->usage = 
			"Usage: ss7 link up\n"
			"	Attempt to take the MTP2 link(s) up with the initial alignment procedure.\n"
			"	If no logical-link-no argument is given, all links are affected.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_link_up(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_link_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 link status";
		e->usage = 
			"Usage: ss7 link status\n"
			"	Show the status of the MTP2 links.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_link_status(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_block(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 block";
		e->usage = 
			"Usage: ss7 block <first> <count> [<linksetname>]\n"
			"	Set <count> lines into local maintenance blocked mode, starting at circuit <first> on\n"
			"	linkset <linksetname>\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_block(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_unblock(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 unblock";
		e->usage = 
			"Usage: ss7 unblock <first> <count> [<linksetname>]\n"
			"	Remove <count> lines from local maintenance blocked mode, starting at circuit <first> on\n"
			"	linkset <linksetname>\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_unblock(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_linestat(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 linestat";
		e->usage = 
			"Usage: ss7 linestat\n"
			"	Show status for all circuits.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_linestat(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}

/*
static char *cmd_linestat1(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 show channels";
		e->usage = 
			"Usage: ss7 show channels.\n"
			"	Show status for all channels.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_linestat(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
*/

static char *k_cmd_cluster_start(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 cluster start";
		e->usage = 
			"Usage: ss7 cluster start\n"
			"	Start the cluster.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_cluster_start(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_cluster_stop(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 cluster stop";
		e->usage = 
			"Usage: ss7 cluster stop\n"
			"	Stop the cluster.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_cluster_stop(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_cluster_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 cluster status";
		e->usage = 
			"Usage: ss7 cluster status\n"
			"	Show the status of the cluster.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_cluster_status(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_reset(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 reset";
		e->usage = 
			"Usage: ss7 reset\n"
			"	Reset all circuits.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_reset(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_mtp_cmd_data(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 mtp data";
		e->usage = 
			"Usage: ss7 mtp data string\n"
			"	Copy hex encoded string to MTP.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(mtp_cmd_data(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_ss7_status(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 status";
		e->usage = 
			"Usage: ss7 status\n"
			"	Show status/statistics of ss7.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_ss7_status(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}

#ifdef MODULETEST
static char *k_cmd_testfailover(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 testfailover";
		e->usage = 
			"Usage: ss7 testfailover\n"
			"	Test the failover mechanism.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_testfailover(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
static char *k_cmd_moduletest(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a) 
{
	if (cmd == CLI_INIT) {
		e->command = "ss7 moduletest";
		e->usage = 
			"Usage: ss7 moduletest <no>\n"
			"	Run moduletest <no>.\n";
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
		
	if(cmd_moduletest(a->fd, a->argc, a->argv) != RESULT_SUCCESS) {
		tris_verbose_ss7("Oh it's failure\n");
	}
	return NULL;
}
#endif

static struct tris_cli_entry my_clis[] = {
	TRIS_CLI_DEFINE(k_cmd_version, "Show current version of chan_ss7"),
	TRIS_CLI_DEFINE(k_cmd_dump_start, "Start MTP2 dump to a file"),
	TRIS_CLI_DEFINE(k_cmd_dump_stop, "Stop a running MTP2 dump"),
	TRIS_CLI_DEFINE(k_cmd_dump_status, "Stop what dumps are running"),
#ifndef MODULETEST
	TRIS_CLI_DEFINE(k_cmd_link_down, "Stop the MTP2 link(s) [logical-link-no]..."),
	TRIS_CLI_DEFINE(k_cmd_link_up, "Start the MTP2 link(s) [logical-link-no]..."),
	TRIS_CLI_DEFINE(k_cmd_link_status, "Show status of the MTP2 links"),
#endif
	TRIS_CLI_DEFINE(k_cmd_block, "Set circuits in local maintenance blocked mode"),
	TRIS_CLI_DEFINE(k_cmd_unblock, "Remove local maintenance blocked mode from circuits"),
	TRIS_CLI_DEFINE(k_cmd_linestat, "Show line states"),
	TRIS_CLI_DEFINE(k_cmd_linestat, "Show channel states"),
	TRIS_CLI_DEFINE(k_cmd_cluster_start, "Start cluster"),
	TRIS_CLI_DEFINE(k_cmd_cluster_stop, "Stop cluster"),
	TRIS_CLI_DEFINE(k_cmd_cluster_status, "Show status of the cluster"),
	TRIS_CLI_DEFINE(k_cmd_reset, "Reset all circuits"),
	TRIS_CLI_DEFINE(k_mtp_cmd_data, "Copy hex encoded string to MTP"),
	TRIS_CLI_DEFINE(k_cmd_ss7_status, "Show status of ss7"),
#ifdef MODULETEST
	TRIS_CLI_DEFINE(k_cmd_testfailover, "Test the failover mechanism"),
	TRIS_CLI_DEFINE(k_cmd_moduletest, "Run a moduletest"),
#endif
};

/* ====================================================================================== */

static int ss7_reload_module(void) {
  tris_log(LOG_NOTICE, "SS7 reload not implemented.\n");
  return TRIS_MODULE_LOAD_SUCCESS;
}


static int ss7_load_module(void)
{
  if(load_config(0)) {
    return TRIS_MODULE_LOAD_DECLINE;
  }

  if (timers_init()) {
    tris_log(LOG_ERROR, "Unable to initialize timers.\n");
    return TRIS_MODULE_LOAD_DECLINE;
  }
  if (isup_init()) {
    tris_log(LOG_ERROR, "Unable to initialize ISUP.\n");
    return TRIS_MODULE_LOAD_DECLINE;
  }
#ifdef SCCP
  if (sccp_init()) {
    tris_log(LOG_ERROR, "Unable to initialize SCCP.\n");
    return TRIS_MODULE_LOAD_DECLINE;
  }
#endif

  if(mtp_init()) {
    tris_log(LOG_ERROR, "Unable to initialize MTP.\n");
    return TRIS_MODULE_LOAD_DECLINE;
  }
  if(start_mtp_thread()) {
    tris_log(LOG_ERROR, "Unable to start MTP thread.\n");
    return TRIS_MODULE_LOAD_DECLINE;
  }
  mtp_control_fifo = mtp_get_control_fifo();

  monitor_running = 1;          /* Otherwise there is a race, and
                                   monitor may exit immediately */
  if(tris_pthread_create(&monitor_thread, NULL, monitor_main, NULL) < 0) {
    tris_log(LOG_ERROR, "Unable to start monitor thread.\n");
    monitor_running = 0;
    return TRIS_MODULE_LOAD_DECLINE;
  }


  tris_cli_register_multiple(my_clis, sizeof(my_clis)/ sizeof(my_clis[0]));

  tris_verbose_ss7(VERBOSE_PREFIX_3 "SS7 channel loaded successfully.\n");
  return TRIS_MODULE_LOAD_SUCCESS;
}


static int ss7_unload_module(void)
{
  tris_cli_unregister_multiple(my_clis, sizeof(my_clis)/ sizeof(my_clis[0]));

#ifdef SCCP
  sccp_cleanup();
#endif
  isup_cleanup();

  tris_mutex_lock(&dump_mutex);
  if(dump_in_fh != NULL) {
    if(dump_in_fh == dump_out_fh) {
      dump_out_fh = NULL;
    }
    fclose(dump_in_fh);
    dump_in_fh = NULL;
  }
  if(dump_out_fh != NULL) {
    fclose(dump_out_fh);
    dump_out_fh = NULL;
  }
  tris_mutex_unlock(&dump_mutex);

  if(monitor_running) {
    stop_monitor();
  }
  stop_mtp_thread();
  mtp_cleanup();
  timers_cleanup();


  destroy_config();
  tris_verbose_ss7(VERBOSE_PREFIX_3 "SS7 channel unloaded.\n");
  return TRIS_MODULE_LOAD_SUCCESS;
}


#ifdef USE_TRISMEDIA_1_2
int reload(void)
{
  return ss7_reload_module();
}
int load_module(void)
{
  return ss7_load_module();
}
int unload_module(void)
{
  return ss7_unload_module();
}
char *description() {
  return (char *) desc;
}

char *key() {
  return TRISMEDIA_GPL_KEY;
}
#else
#define TRIS_MODULE "chan_ss7"
TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, desc,
                .load = ss7_load_module,
                .unload = ss7_unload_module,
                .reload = ss7_reload_module,
);
#endif
