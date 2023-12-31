/* transport.c - MTP/audio transport
 * Author: Anders Baekgaard <ab@dicea.dk>
 * This work is included with chan_ss7, see copyright below.
 */

/*
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
#include <time.h>
#include <sys/time.h>
#include <sys/param.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#ifdef DAHDI
#include <dahdi/user.h>
#define FAST_HDLC_NEED_TABLES
#include <dahdi/fasthdlc.h>
#else
#include "zaptel.h"
#define FAST_HDLC_NEED_TABLES
#include "fasthdlc.h"
#define DAHDI_AUDIOMODE ZT_AUDIOMODE
#define DAHDI_AUDIOMODE ZT_AUDIOMODE
#define DAHDI_BUFFERINFO ZT_BUFFERINFO
#define DAHDI_DIAL ZT_DIAL
#define DAHDI_DIAL_OPERATION ZT_DIAL_OPERATION
#define DAHDI_DIAL_OP_APPEND ZT_DIAL_OP_APPEND
#define DAHDI_ECHOCANCEL ZT_ECHOCANCEL
#define DAHDI_ECHOTRAIN ZT_ECHOTRAIN
#define DAHDI_FLUSH ZT_FLUSH
#define DAHDI_FLUSH_ALL ZT_FLUSH_ALL
#define DAHDI_GETEVENT ZT_GETEVENT
#define DAHDI_GET_BUFINFO ZT_GET_BUFINFO
#define DAHDI_LAW_ALAW ZT_LAW_ALAW
#define DAHDI_POLICY_IMMEDIATE ZT_POLICY_IMMEDIATE
#define DAHDI_SETLAW ZT_SETLAW
#define DAHDI_SET_BLOCKSIZE ZT_SET_BLOCKSIZE
#define DAHDI_SET_BUFINFO ZT_SET_BUFINFO
#define DAHDI_SPECIFY ZT_SPECIFY
#define dahdi_bufferinfo zt_bufferinfo
#define dahdi_dialoperation zt_dialoperation
#endif


#ifdef MTP_STANDALONE
#include "trisstubs.h"
#else
#include "trismedia/compiler.h"
#include "trismedia/logger.h"
#endif
#include "config.h"
#include "mtp.h"
#include "transport.h"
#include "utils.h"


static int setnonblock_fd(int s)
{
  int res, flags;

  res = fcntl(s, F_GETFL);
  if(res < 0) {
    tris_log(LOG_WARNING, "Could not obtain flags for socket fd: %s.\n", strerror(errno));
    return -1;
  }
  flags = res | O_NONBLOCK;
  res = fcntl(s, F_SETFL, flags);
  if(res < 0) {
    tris_log(LOG_WARNING, "Could not set socket fd non-blocking: %s.\n", strerror(errno));
    return -1;
  }
  return 0;
}



static int transport_socket(int localport, const char* remotehost, int remoteport);

#ifndef MTP_OVER_UDP
static void set_buffer_info(int fd, int cic, int numbufs)
{
  struct dahdi_bufferinfo bi;
  int res;

  bi.txbufpolicy = DAHDI_POLICY_IMMEDIATE;
  bi.rxbufpolicy = DAHDI_POLICY_IMMEDIATE;
  bi.numbufs = numbufs;
  bi.bufsize = AUDIO_READSIZE;
  res = ioctl(fd, DAHDI_SET_BUFINFO, &bi);
  if(res) {
    tris_log(LOG_WARNING, "Failure to set buffer policy for circuit %d: %s.\n", cic, strerror(errno));
  }
}

int adjust_buffers(int fd, int cic)
{
  struct dahdi_bufferinfo bi;
  int res;

  res = ioctl(fd, DAHDI_GET_BUFINFO, &bi);
  if(res) {
    tris_log(LOG_WARNING, "Failure to get buffer policy for circuit %d: %s.\n", cic, strerror(errno));
    return 0;
  }
  if (bi.numbufs >= 8) {
    static struct timeval lastreport = {0, 0};
    struct timeval now;
    gettimeofday(&now, NULL);
    if (now.tv_sec - lastreport.tv_sec > 10) {
      tris_log(LOG_DEBUG, "Limit exceeded when trying to adjust numbufs to %d, for circuit %d.\n", bi.numbufs, cic);
      lastreport = now;
    }
    return 0;
  }
  set_buffer_info(fd, cic, bi.numbufs + 1);
  tris_log(LOG_DEBUG, "Adjusting numbufs to %d for circuit %d.\n", bi.numbufs + 1, cic);
  return 1;
}


void set_audiomode(int fd)
{
  int res;
  int z = 1;

  res = ioctl(fd, DAHDI_AUDIOMODE, &z);
  if (res)
    tris_log(LOG_WARNING, "Unable to set fd %d to audiomode\n", fd);
}


void clear_audiomode(int fd)
{
  int res;
  int z = 0;

  res = ioctl(fd, DAHDI_AUDIOMODE, &z);
  if (res)
    tris_log(LOG_WARNING, "Unable to clear audiomode on fd %d\n", fd);
}


int openchannel(struct link* link, int channel)
{
  int cic = link->first_cic + channel;
  int zapid = link->first_zapid + channel + 1;
  int fd = open("/dev/dahdi/channel", O_RDWR | O_NONBLOCK); /*KimPH modified zap to dahdi */
  int parm, res;

  tris_log(LOG_DEBUG, "Configuring CIC %d on zaptel device %d.\n", cic, zapid);
  if(fd < 0) {
    tris_log(LOG_ERROR, "Unable to open /dev/dahdi/channel: %s.\n", strerror(errno)); /* KimPH modified zap to dahdi */
    return -1;
  }
  res = ioctl(fd, DAHDI_SPECIFY, &zapid);
  if(res) {
    tris_log(LOG_WARNING, "Failure in DAHDI_SPECIFY for circuit %d: %s.\n", cic, strerror(errno));
    return -1;
  }
  parm = DAHDI_LAW_ALAW;
  res = ioctl(fd, DAHDI_SETLAW, &parm);
  if(res) {
    tris_log(LOG_DEBUG, "Failure to set circuit   %d to ALAW: %s.\n", cic, strerror(errno));
    return -1;
  }
  set_buffer_info(fd, cic, 4);
  parm = AUDIO_READSIZE;
  res = ioctl(fd, DAHDI_SET_BLOCKSIZE, &parm);
  if(res) {
    tris_log(LOG_WARNING, "Failure to set blocksize for circuit %d: %s.\n", cic, strerror(errno));
    return -1;
  }
  res = setnonblock_fd(fd);
  if(res < 0) {
    tris_log(LOG_WARNING, "Could not set non-blocking on circuit %d: %s.\n", cic, strerror(errno));
    return -1;
  }
  return fd;
}

void flushchannel(int fd, int cic)
{
  int parm, res;

  /* Flush timeslot of old data. */
  parm = DAHDI_FLUSH_ALL;
  res = ioctl(fd, DAHDI_FLUSH, &parm);
  if (res) {
    tris_log(LOG_WARNING, "Unable to flush input on circuit %d\n", cic);
  }
  set_buffer_info(fd, cic, 4);
}


int openschannel(struct link* link)
{
  struct dahdi_bufferinfo bi;
  char devname[100];
  int fd, res;
  int zapid = link->schannel + link->first_zapid;

  sprintf(devname, "/dev/dahdi/%d", zapid); /* KimPH modified from zap to dahdi */
  fd = open(devname, O_RDWR);
  if(fd < 0) {
    char devname2[100];
    strcpy(devname2, "/dev/dahdi/channel"); /* KimPH modified from zap to dahdi */
    fd = open(devname2, O_RDWR);
    if(fd < 0) {
      tris_log(LOG_WARNING, "Unable to open signalling link zaptel devices %s and %s: %s\n", devname, devname2, strerror(errno));
      goto fail;
    }
    if (ioctl(fd, DAHDI_SPECIFY, &zapid)) {
      tris_log(LOG_WARNING, "Unable to specify channel %d: %s\n", zapid, strerror(errno));
      goto fail;
    }
  }

  bi.txbufpolicy = DAHDI_POLICY_IMMEDIATE;
  bi.rxbufpolicy = DAHDI_POLICY_IMMEDIATE;
  bi.numbufs = NUM_ZAP_BUF;
  bi.bufsize = ZAP_BUF_SIZE;
  if (ioctl(fd, DAHDI_SET_BUFINFO, &bi)) {
    tris_log(LOG_WARNING, "Unable to set buffering policy on signalling link "
            "zaptel device: %s\n", strerror(errno));
    goto fail;
  }

  res = setnonblock_fd(fd);
  if(res < 0) {
    tris_log(LOG_WARNING, "SS7: Could not set signalling link fd non-blocking: "
            "%s.\n", strerror(errno));
    goto fail;
  }
  return fd;
 fail:
  return -1;
}

int io_get_zaptel_event(int fd, int* e)
{
  return ioctl(fd, DAHDI_GETEVENT, e);
}


int io_enable_echo_cancellation(int fd, int cic, int echocan_taps, int echocan_train)
{
  int res, parm = 1;

  res = ioctl(fd, DAHDI_AUDIOMODE, &parm);
  if (res)
    tris_log(LOG_WARNING, "Unable to set fd %d to audiomode\n", fd);

  res = ioctl(fd, DAHDI_ECHOCANCEL, &echocan_taps);
  if (res) {
    tris_log(LOG_WARNING, "Unable to enable echo cancellation on cic %d\n", cic);
    return res;
  } else {
    tris_log(LOG_DEBUG, "Enabled echo cancellation on cic %d\n", cic);
    res = ioctl(fd, DAHDI_ECHOTRAIN, &echocan_train);
    if (res) {
      tris_log(LOG_WARNING, "Unable to request echo training on cic %d\n", cic);
      return res;
    } else {
      tris_log(LOG_DEBUG, "Engaged echo training on cic %d\n", cic);
    }
  }
  return 0;
}

void io_disable_echo_cancellation(int fd, int cic)
{
  int res;
  int x = 0;

  res = ioctl(fd, DAHDI_ECHOCANCEL, &x);
  if (res) 
    tris_log(LOG_WARNING, "Unable to disable echo cancellation on cic %d\n", cic);
  else
    tris_log(LOG_DEBUG, "disabled echo cancellation on cic %d\n", cic);
}


int io_send_dtmf(int fd, int cic, char digit)
{
  struct dahdi_dialoperation zo;
  int res;

  zo.op = DAHDI_DIAL_OP_APPEND;
  zo.dialstr[0] = 'T';
  zo.dialstr[1] = digit;
  zo.dialstr[2] = 0;
  res = ioctl(fd, DAHDI_DIAL, &zo);
  if(res) {
    tris_log(LOG_WARNING, "DTMF generation of %c failed on CIC=%d.\n", digit, cic);
    return res;
  } else {
    tris_log(LOG_DEBUG, "Passed on digit %c to CIC=%d.\n", digit, cic);
  }
  return 0;
}


#else
#define MTPPORT 11000
int openschannel(struct link* link)
{
  int id = link->schannel + link->first_zapid;
  int i;

  for (i = 0; i < this_host->n_peers; i++) {
    if (this_host->peers[i].link == link)
      return transport_socket(MTPPORT+id, this_host->peers[i].hostname, MTPPORT+id);
  }
  tris_log(LOG_ERROR, "Cannot open schannel, there is no configured peer host for link '%s'\n", link->name);
  return -1;
}


int openchannel(struct link* link, int channel)
{
  int zapid = link->first_zapid + channel + 1;
  int i;

  for (i = 0; i < this_host->n_peers; i++) {
    if (this_host->peers[i].link == link)
      return transport_socket(MTPPORT+zapid, this_host->peers[i].hostname, MTPPORT+zapid);
  }
  tris_log(LOG_ERROR, "Cannot open channel, there is no configured peer host for link '%s'\n", link->name);
  return -1;
}

int adjust_buffers(int fd, int cic)
{
  return 1;
}

void set_audiomode(int fd)
{
}


void clear_audiomode(int fd)
{
}


void flushchannel(int fd, int cic)
{
}

int io_get_zaptel_event(int fd, int* e)
{
  return 0;
}

int io_enable_echo_cancellation(int fd, int cic, int echocan_taps, int echocan_train)
{
  return 0;
}

void io_disable_echo_cancellation(int fd, int cic)
{
}

int io_send_dtmf(int fd, int cic, char digit)
{
  return 0;
}

#endif


static int setup_socket(int localport, int sockettype, int ipproto)
{
  struct sockaddr_in sock;
  in_addr_t addr = INADDR_ANY;
  int parm;
  int s;

  memset(&sock, 0, sizeof(struct sockaddr_in));
  sock.sin_family = AF_INET;
  sock.sin_port = htons(localport);
  memcpy(&sock.sin_addr, &addr, sizeof(addr));

  s = socket(PF_INET, sockettype, ipproto);
  if (s < 0) {
    tris_log(LOG_ERROR, "Cannot create UDP socket, errno=%d: %s\n", errno, strerror(errno));
    return -1;
  }
  parm = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &parm, sizeof(int));
  setnonblock_fd(s);

  if (bind(s, &sock, sizeof(sock)) < 0) {
    tris_log(LOG_ERROR, "Cannot bind receiver socket, errno=%d: %s\n", errno, strerror(errno));
    close(s);
    return -1;
  }
  if (sockettype != SOCK_DGRAM)
    if (listen(s, 8) < 0) {
      tris_log(LOG_ERROR, "Cannot listen on socket, errno=%d: %s\n", errno, strerror(errno));
      close(s);
      return -1;
    }
  return s;
}

static int transport_socket(int localport, const char* remotehost, int remoteport)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  char port[8];
  int s, res;

  s = setup_socket(localport, SOCK_DGRAM, 0);
#ifdef xxxusestcp
  if (listen(s, 1) < 0) {
    tris_log(LOG_ERROR, "Cannot listen on UDP socket, errno=%d: %s\n", errno, strerror(errno));
    close(s);
    s = -1;
    return -1;
  }
#endif


  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  sprintf(port, "%d", remoteport);
  res = getaddrinfo(remotehost, port, NULL, &result);
  if (res != 0) {
    tris_log(LOG_ERROR, "Invalid hostname/IP address '%s' or port '%s': %s.\n", remotehost, port, gai_strerror(res)
	    );
    return -1;
  }
  for (rp = result; rp; rp = rp->ai_next) {
    if ((res = connect(s, rp->ai_addr, rp->ai_addrlen)) != -1)
      break;
  }
  if (rp == NULL) {
    tris_log(LOG_ERROR, "Could not connect to hostname/IP address '%s', port '%s': %s.\n", remotehost, port, strerror(errno));
    close(s);
  }
  freeaddrinfo(result);

  return s;
}
