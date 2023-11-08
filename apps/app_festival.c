/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2002, Christos Ricudis
 *
 * Christos Ricudis <ricudis@itc.auth.gr>
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
 * \brief Connect to festival
 *
 * \author Christos Ricudis <ricudis@itc.auth.gr>
 *
 * \extref  The Festival Speech Synthesis System - http://www.cstr.ed.ac.uk/projects/festival/
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 208116 $")

#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>

#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/md5.h"
#include "trismedia/config.h"
#include "trismedia/utils.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"

#define FESTIVAL_CONFIG "festival.conf"
#define MAXLEN 180
#define MAXFESTLEN 2048

/*** DOCUMENTATION
	<application name="Festival" language="en_US">
		<synopsis>
			Say text to the user.
		</synopsis>
		<syntax>
			<parameter name="text" required="true" />
			<parameter name="intkeys" />
		</syntax>
		<description>
			<para>Connect to Festival, send the argument, get back the waveform, play it to the user,
			allowing any given interrupt keys to immediately terminate and return the value, or
			<literal>any</literal> to allow any number back (useful in dialplan).</para>
		</description>
	</application>
 ***/

static char *app = "Festival";

static char *socket_receive_file_to_buff(int fd, int *size)
{
	/* Receive file (probably a waveform file) from socket using
	 * Festival key stuff technique, but long winded I know, sorry
	 * but will receive any file without closing the stream or
	 * using OOB data
	 */
	static char *file_stuff_key = "ft_StUfF_key"; /* must == Festival's key */
	char *buff, *tmp;
	int bufflen;
	int n,k,i;
	char c;

	bufflen = 1024;
	if (!(buff = tris_malloc(bufflen)))
		return NULL;
	*size = 0;

	for (k = 0; file_stuff_key[k] != '\0';) {
		n = read(fd, &c, 1);
		if (n == 0)
			break;  /* hit stream eof before end of file */
		if ((*size) + k + 1 >= bufflen) {
			/* +1 so you can add a terminating NULL if you want */
			bufflen += bufflen / 4;
			if (!(tmp = tris_realloc(buff, bufflen))) {
				tris_free(buff);
				return NULL;
			}
			buff = tmp;
		}
		if (file_stuff_key[k] == c)
			k++;
		else if ((c == 'X') && (file_stuff_key[k+1] == '\0')) {
			/* It looked like the key but wasn't */
			for (i = 0; i < k; i++, (*size)++)
				buff[*size] = file_stuff_key[i];
			k = 0;
			/* omit the stuffed 'X' */
		} else {
			for (i = 0; i < k; i++, (*size)++)
				buff[*size] = file_stuff_key[i];
			k = 0;
			buff[*size] = c;
			(*size)++;
		}
	}

	return buff;
}

static int send_waveform_to_fd(char *waveform, int length, int fd)
{
	int res;
#ifdef __PPC__ 
	int x;
	char c;
#endif

	res = tris_safe_fork(0);
	if (res < 0)
		tris_log(LOG_WARNING, "Fork failed\n");
	if (res) {
		return res;
	}
	dup2(fd, 0);
	tris_close_fds_above_n(0);
	if (tris_opt_high_priority)
		tris_set_priority(0);
#ifdef __PPC__  
	for (x = 0; x < length; x += 2) {
		c = *(waveform + x + 1);
		*(waveform + x + 1) = *(waveform + x);
		*(waveform + x) = c;
	}
#endif
	
	if (write(fd, waveform, length) < 0) {
		tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
	}

	close(fd);
	exit(0);
}

static int send_waveform_to_channel(struct tris_channel *chan, char *waveform, int length, char *intkeys)
{
	int res = 0;
	int fds[2];
	int pid = -1;
	int needed = 0;
	int owriteformat;
	struct tris_frame *f;
	struct myframe {
		struct tris_frame f;
		char offset[TRIS_FRIENDLY_OFFSET];
		char frdata[2048];
	} myf = {
		.f = { 0, },
	};

	if (pipe(fds)) {
		tris_log(LOG_WARNING, "Unable to create pipe\n");
		return -1;
	}

	/* Answer if it's not already going */
	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);
	tris_stopstream(chan);
	tris_indicate(chan, -1);
	
	owriteformat = chan->writeformat;
	res = tris_set_write_format(chan, TRIS_FORMAT_SLINEAR);
	if (res < 0) {
		tris_log(LOG_WARNING, "Unable to set write format to signed linear\n");
		return -1;
	}
	
	res = send_waveform_to_fd(waveform, length, fds[1]);
	if (res >= 0) {
		pid = res;
		/* Order is important -- there's almost always going to be mp3...  we want to prioritize the
		   user */
		for (;;) {
			res = tris_waitfor(chan, 1000);
			if (res < 1) {
				res = -1;
				break;
			}
			f = tris_read(chan);
			if (!f) {
				tris_log(LOG_WARNING, "Null frame == hangup() detected\n");
				res = -1;
				break;
			}
			if (f->frametype == TRIS_FRAME_DTMF) {
				tris_debug(1, "User pressed a key\n");
				if (intkeys && strchr(intkeys, f->subclass)) {
					res = f->subclass;
					tris_frfree(f);
					break;
				}
			}
			if (f->frametype == TRIS_FRAME_VOICE) {
				/* Treat as a generator */
				needed = f->samples * 2;
				if (needed > sizeof(myf.frdata)) {
					tris_log(LOG_WARNING, "Only able to deliver %d of %d requested samples\n",
						(int)sizeof(myf.frdata) / 2, needed/2);
					needed = sizeof(myf.frdata);
				}
				res = read(fds[0], myf.frdata, needed);
				if (res > 0) {
					myf.f.frametype = TRIS_FRAME_VOICE;
					myf.f.subclass = TRIS_FORMAT_SLINEAR;
					myf.f.datalen = res;
					myf.f.samples = res / 2;
					myf.f.offset = TRIS_FRIENDLY_OFFSET;
					myf.f.src = __PRETTY_FUNCTION__;
					myf.f.data.ptr = myf.frdata;
					if (tris_write(chan, &myf.f) < 0) {
						res = -1;
						tris_frfree(f);
						break;
					}
					if (res < needed) { /* last frame */
						tris_debug(1, "Last frame\n");
						res = 0;
						tris_frfree(f);
						break;
					}
				} else {
					tris_debug(1, "No more waveform\n");
					res = 0;
				}
			}
			tris_frfree(f);
		}
	}
	close(fds[0]);
	close(fds[1]);

#if 0
	if (pid > -1)
		kill(pid, SIGKILL);
#endif
	if (!res && owriteformat)
		tris_set_write_format(chan, owriteformat);
	return res;
}

static int festival_exec(struct tris_channel *chan, void *vdata)
{
	int usecache;
	int res = 0;
	struct sockaddr_in serv_addr;
	struct hostent *serverhost;
	struct tris_hostent ahp;
	int fd;
	FILE *fs;
	const char *host;
	const char *cachedir;
	const char *temp;
	const char *festivalcommand;
	int port = 1314;
	int n;
	char ack[4];
	char *waveform;
	int filesize;
	int wave;
	char bigstring[MAXFESTLEN];
	int i;
	struct MD5Context md5ctx;
	unsigned char MD5Res[16];
	char MD5Hex[33] = "";
	char koko[4] = "";
	char cachefile[MAXFESTLEN]="";
	int readcache = 0;
	int writecache = 0;
	int strln;
	int fdesc = -1;
	char buffer[16384];
	int seekpos = 0;	
	char *data;	
	struct tris_config *cfg;
	char *newfestivalcommand;
	struct tris_flags config_flags = { 0 };
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(text);
		TRIS_APP_ARG(interrupt);
	);

	if (tris_strlen_zero(vdata)) {
		tris_log(LOG_WARNING, "festival requires an argument (text)\n");
		return -1;
	}

	cfg = tris_config_load(FESTIVAL_CONFIG, config_flags);
	if (!cfg) {
		tris_log(LOG_WARNING, "No such configuration file %s\n", FESTIVAL_CONFIG);
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file " FESTIVAL_CONFIG " is in an invalid format.  Aborting.\n");
		return -1;
	}

	if (!(host = tris_variable_retrieve(cfg, "general", "host"))) {
		host = "localhost";
	}
	if (!(temp = tris_variable_retrieve(cfg, "general", "port"))) {
		port = 1314;
	} else {
		port = atoi(temp);
	}
	if (!(temp = tris_variable_retrieve(cfg, "general", "usecache"))) {
		usecache = 0;
	} else {
		usecache = tris_true(temp);
	}
	if (!(cachedir = tris_variable_retrieve(cfg, "general", "cachedir"))) {
		cachedir = "/tmp/";
	}

	data = tris_strdupa(vdata);
	TRIS_STANDARD_APP_ARGS(args, data);

	if (!(festivalcommand = tris_variable_retrieve(cfg, "general", "festivalcommand"))) {
		const char *startcmd = "(tts_texttrismedia \"";
		const char *endcmd = "\" 'file)(quit)\n";

		strln = strlen(startcmd) + strlen(args.text) + strlen(endcmd) + 1;
		newfestivalcommand = alloca(strln);
		snprintf(newfestivalcommand, strln, "%s%s%s", startcmd, args.text, endcmd);
		festivalcommand = newfestivalcommand;
	} else { /* This else parses the festivalcommand that we're sent from the config file for \n's, etc */
		int x, j;
		newfestivalcommand = alloca(strlen(festivalcommand) + strlen(args.text) + 1);

		for (x = 0, j = 0; x < strlen(festivalcommand); x++) {
			if (festivalcommand[x] == '\\' && festivalcommand[x + 1] == 'n') {
				newfestivalcommand[j++] = '\n';
				x++;
			} else if (festivalcommand[x] == '\\') {
				newfestivalcommand[j++] = festivalcommand[x + 1];
				x++;
			} else if (festivalcommand[x] == '%' && festivalcommand[x + 1] == 's') {
				sprintf(&newfestivalcommand[j], "%s", args.text); /* we know it is big enough */
				j += strlen(args.text);
				x++;
			} else
				newfestivalcommand[j++] = festivalcommand[x];
		}
		newfestivalcommand[j] = '\0';
		festivalcommand = newfestivalcommand;
	}
	
	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = TRIS_DIGIT_ANY;

	tris_debug(1, "Text passed to festival server : %s\n", args.text);
	/* Connect to local festival server */
	
	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (fd < 0) {
		tris_log(LOG_WARNING, "festival_client: can't get socket\n");
		tris_config_destroy(cfg);
		return -1;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));

	if ((serv_addr.sin_addr.s_addr = inet_addr(host)) == -1) {
		/* its a name rather than an ipnum */
		serverhost = tris_gethostbyname(host, &ahp);

		if (serverhost == NULL) {
			tris_log(LOG_WARNING, "festival_client: gethostbyname failed\n");
			tris_config_destroy(cfg);
			return -1;
		}
		memmove(&serv_addr.sin_addr, serverhost->h_addr, serverhost->h_length);
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);

	if (connect(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
		tris_log(LOG_WARNING, "festival_client: connect to server failed\n");
		tris_config_destroy(cfg);
		return -1;
	}

	/* Compute MD5 sum of string */
	MD5Init(&md5ctx);
	MD5Update(&md5ctx, (unsigned char *)args.text, strlen(args.text));
	MD5Final(MD5Res, &md5ctx);
	MD5Hex[0] = '\0';

	/* Convert to HEX and look if there is any matching file in the cache 
		directory */
	for (i = 0; i < 16; i++) {
		snprintf(koko, sizeof(koko), "%X", MD5Res[i]);
		strncat(MD5Hex, koko, sizeof(MD5Hex) - strlen(MD5Hex) - 1);
	}
	readcache = 0;
	writecache = 0;
	if (strlen(cachedir) + strlen(MD5Hex) + 1 <= MAXFESTLEN && (usecache == -1)) {
		snprintf(cachefile, sizeof(cachefile), "%s/%s", cachedir, MD5Hex);
		fdesc = open(cachefile, O_RDWR);
		if (fdesc == -1) {
			fdesc = open(cachefile, O_CREAT | O_RDWR, TRIS_FILE_MODE);
			if (fdesc != -1) {
				writecache = 1;
				strln = strlen(args.text);
				tris_debug(1, "line length : %d\n", strln);
    				if (write(fdesc,&strln,sizeof(int)) < 0) {
					tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
				}
    				if (write(fdesc,data,strln) < 0) {
					tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
				}
				seekpos = lseek(fdesc, 0, SEEK_CUR);
				tris_debug(1, "Seek position : %d\n", seekpos);
			}
		} else {
    			if (read(fdesc,&strln,sizeof(int)) != sizeof(int)) {
				tris_log(LOG_WARNING, "read() failed: %s\n", strerror(errno));
			}
			tris_debug(1, "Cache file exists, strln=%d, strlen=%d\n", strln, (int)strlen(args.text));
			if (strlen(args.text) == strln) {
				tris_debug(1, "Size OK\n");
    				if (read(fdesc,&bigstring,strln) != strln) {
					tris_log(LOG_WARNING, "read() failed: %s\n", strerror(errno));
				}
				bigstring[strln] = 0;
				if (strcmp(bigstring, args.text) == 0) { 
					readcache = 1;
				} else {
					tris_log(LOG_WARNING, "Strings do not match\n");
				}
			} else {
				tris_log(LOG_WARNING, "Size mismatch\n");
			}
		}
	}

	if (readcache == 1) {
		close(fd);
		fd = fdesc;
		tris_debug(1, "Reading from cache...\n");
	} else {
		tris_debug(1, "Passing text to festival...\n");
		fs = fdopen(dup(fd), "wb");

		fprintf(fs, "%s", festivalcommand);
		fflush(fs);
		fclose(fs);
	}
	
	/* Write to cache and then pass it down */
	if (writecache == 1) {
		tris_debug(1, "Writing result to cache...\n");
		while ((strln = read(fd, buffer, 16384)) != 0) {
			if (write(fdesc,buffer,strln) < 0) {
				tris_log(LOG_WARNING, "write() failed: %s\n", strerror(errno));
			}
		}
		close(fd);
		close(fdesc);
		fd = open(cachefile, O_RDWR);
		lseek(fd, seekpos, SEEK_SET);
	}
	
	tris_debug(1, "Passing data to channel...\n");

	/* Read back info from server */
	/* This assumes only one waveform will come back, also LP is unlikely */
	wave = 0;
	do {
		int read_data;
		for (n = 0; n < 3; ) {
			read_data = read(fd, ack + n, 3 - n);
			/* this avoids falling in infinite loop
			 * in case that festival server goes down
			 */
			if (read_data == -1) {
				tris_log(LOG_WARNING, "Unable to read from cache/festival fd\n");
				close(fd);
				tris_config_destroy(cfg);
				return -1;
			}
			n += read_data;
		}
		ack[3] = '\0';
		if (strcmp(ack, "WV\n") == 0) {         /* receive a waveform */
			tris_debug(1, "Festival WV command\n");
			if ((waveform = socket_receive_file_to_buff(fd, &filesize))) {
				res = send_waveform_to_channel(chan, waveform, filesize, args.interrupt);
				tris_free(waveform);
			}
			break;
		} else if (strcmp(ack, "LP\n") == 0) {   /* receive an s-expr */
			tris_debug(1, "Festival LP command\n");
			if ((waveform = socket_receive_file_to_buff(fd, &filesize))) {
				waveform[filesize] = '\0';
				tris_log(LOG_WARNING, "Festival returned LP : %s\n", waveform);
				tris_free(waveform);
			}
		} else if (strcmp(ack, "ER\n") == 0) {    /* server got an error */
			tris_log(LOG_WARNING, "Festival returned ER\n");
			res = -1;
			break;
		}
	} while (strcmp(ack, "OK\n") != 0);
	close(fd);
	tris_config_destroy(cfg);
	return res;
}

static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	struct tris_flags config_flags = { 0 };
	struct tris_config *cfg = tris_config_load(FESTIVAL_CONFIG, config_flags);
	if (!cfg) {
		tris_log(LOG_WARNING, "No such configuration file %s\n", FESTIVAL_CONFIG);
		return TRIS_MODULE_LOAD_DECLINE;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file " FESTIVAL_CONFIG " is in an invalid format.  Aborting.\n");
		return TRIS_MODULE_LOAD_DECLINE;
	}
	tris_config_destroy(cfg);
	return tris_register_application_xml(app, festival_exec);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Simple Festival Interface");
