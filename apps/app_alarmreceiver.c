/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C)  2004 - 2005 Steve Rodgers
 *
 * Steve Rodgers <hwstar@rodgers.sdcoxmail.com>
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
 * \brief Central Station Alarm receiver for Ademco Contact ID
 * \author Steve Rodgers <hwstar@rodgers.sdcoxmail.com>
 *
 * *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING ***
 *
 * Use at your own risk. Please consult the GNU GPL license document included with Trismedia.         *
 *
 * *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING *** WARNING ***
 *
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 211580 $")

#include <math.h>
#include <sys/wait.h>
#include <sys/time.h>

#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/ulaw.h"
#include "trismedia/app.h"
#include "trismedia/dsp.h"
#include "trismedia/config.h"
#include "trismedia/localtime.h"
#include "trismedia/callerid.h"
#include "trismedia/astdb.h"
#include "trismedia/utils.h"

#define ALMRCV_CONFIG "alarmreceiver.conf"
#define ADEMCO_CONTACT_ID "ADEMCO_CONTACT_ID"

struct event_node{
	char data[17];
	struct event_node *next;
};

typedef struct event_node event_node_t;

static char *app = "AlarmReceiver";
/*** DOCUMENTATION
	<application name="AlarmReceiver" language="en_US">
		<synopsis>
			Provide support for receiving alarm reports from a burglar or fire alarm panel.
		</synopsis>
		<syntax />
		<description>
			<para>This application should be called whenever there is an alarm panel calling in to dump its events.
			The application will handshake with the alarm panel, and receive events, validate them, handshake them,
			and store them until the panel hangs up. Once the panel hangs up, the application will run the system
			command specified by the eventcmd setting in <filename>alarmreceiver.conf</filename> and pipe the
			events to the standard input of the application.
			The configuration file also contains settings for DTMF timing, and for the loudness of the
			acknowledgement tones.</para>
			<note><para>Only 1 signalling format is supported at this time: Ademco Contact ID.</para></note>
		</description>
		<see-also>
			<ref type="filename">alarmreceiver.conf</ref>
		</see-also>
	</application>
 ***/

/* Config Variables */
static int fdtimeout = 2000;
static int sdtimeout = 200;
static int toneloudness = 4096;
static int log_individual_events = 0;
static char event_spool_dir[128] = {'\0'};
static char event_app[128] = {'\0'};
static char db_family[128] = {'\0'};
static char time_stamp_format[128] = {"%a %b %d, %Y @ %H:%M:%S %Z"};

/* Misc variables */
static char event_file[14] = "/event-XXXXXX";

/*
* Attempt to access a database variable and increment it,
* provided that the user defined db-family in alarmreceiver.conf
* The alarmreceiver app will write statistics to a few variables
* in this family if it is defined. If the new key doesn't exist in the
* family, then create it and set its value to 1.
*/
static void database_increment( char *key )
{
	int res = 0;
	unsigned v;
	char value[16];
	
	
	if (tris_strlen_zero(db_family))
		return; /* If not defined, don't do anything */
	
	res = tris_db_get(db_family, key, value, sizeof(value) - 1);
	
	if (res) {
		tris_verb(4, "AlarmReceiver: Creating database entry %s and setting to 1\n", key);
		/* Guess we have to create it */
		res = tris_db_put(db_family, key, "1");
		return;
	}
	
	sscanf(value, "%30u", &v);
	v++;

	tris_verb(4, "AlarmReceiver: New value for %s: %u\n", key, v);

	snprintf(value, sizeof(value), "%u", v);

	res = tris_db_put(db_family, key, value);

	if (res)
		tris_verb(4, "AlarmReceiver: database_increment write error\n");

	return;
}


/*
* Build a MuLaw data block for a single frequency tone
*/
static void make_tone_burst(unsigned char *data, float freq, float loudness, int len, int *x)
{
	int     i;
	float   val;

	for (i = 0; i < len; i++) {
		val = loudness * sin((freq * 2.0 * M_PI * (*x)++)/8000.0);
		data[i] = TRIS_LIN2MU((int)val);
	}

	/* wrap back around from 8000 */

	if (*x >= 8000)
		*x = 0;
	return;
}

/*
* Send a single tone burst for a specifed duration and frequency.
* Returns 0 if successful
*/
static int send_tone_burst(struct tris_channel *chan, float freq, int duration, int tldn)
{
	int res = 0;
	int i = 0;
	int x = 0;
	struct tris_frame *f, wf;
	
	struct {
		unsigned char offset[TRIS_FRIENDLY_OFFSET];
		unsigned char buf[640];
	} tone_block;

	for (;;) {

		if (tris_waitfor(chan, -1) < 0) {
			res = -1;
			break;
		}

		f = tris_read(chan);
		if (!f) {
			res = -1;
			break;
		}

		if (f->frametype == TRIS_FRAME_VOICE) {
			wf.frametype = TRIS_FRAME_VOICE;
			wf.subclass = TRIS_FORMAT_ULAW;
			wf.offset = TRIS_FRIENDLY_OFFSET;
			wf.mallocd = 0;
			wf.data.ptr = tone_block.buf;
			wf.datalen = f->datalen;
			wf.samples = wf.datalen;
			
			make_tone_burst(tone_block.buf, freq, (float) tldn, wf.datalen, &x);

			i += wf.datalen / 8;
			if (i > duration) {
				tris_frfree(f);
				break;
			}
			if (tris_write(chan, &wf)) {
				tris_verb(4, "AlarmReceiver: Failed to write frame on %s\n", chan->name);
				tris_log(LOG_WARNING, "AlarmReceiver Failed to write frame on %s\n",chan->name);
				res = -1;
				tris_frfree(f);
				break;
			}
		}

		tris_frfree(f);
	}
	return res;
}

/*
* Receive a string of DTMF digits where the length of the digit string is known in advance. Do not give preferential
* treatment to any digit value, and allow separate time out values to be specified for the first digit and all subsequent
* digits.
*
* Returns 0 if all digits successfully received.
* Returns 1 if a digit time out occurred
* Returns -1 if the caller hung up or there was a channel error.
*
*/
static int receive_dtmf_digits(struct tris_channel *chan, char *digit_string, int length, int fdto, int sdto)
{
	int res = 0;
	int i = 0;
	int r;
	struct tris_frame *f;
	struct timeval lastdigittime;

	lastdigittime = tris_tvnow();
	for (;;) {
		/* if outa time, leave */
		if (tris_tvdiff_ms(tris_tvnow(), lastdigittime) > ((i > 0) ? sdto : fdto)) {
			tris_verb(4, "AlarmReceiver: DTMF Digit Timeout on %s\n", chan->name);
			tris_debug(1,"AlarmReceiver: DTMF timeout on chan %s\n",chan->name);
			res = 1;
			break;
		}

		if ((r = tris_waitfor(chan, -1) < 0)) {
			tris_debug(1, "Waitfor returned %d\n", r);
			continue;
		}

		f = tris_read(chan);

		if (f == NULL) {
			res = -1;
			break;
		}

		/* If they hung up, leave */
		if ((f->frametype == TRIS_FRAME_CONTROL) && (f->subclass == TRIS_CONTROL_HANGUP)) {
			if (f->data.uint32) {
				chan->hangupcause = f->data.uint32;
			}
			tris_frfree(f);
			res = -1;
			break;
		}

		/* if not DTMF, just do it again */
		if (f->frametype != TRIS_FRAME_DTMF) {
			tris_frfree(f);
			continue;
		}

		digit_string[i++] = f->subclass;  /* save digit */

		tris_frfree(f);

		/* If we have all the digits we expect, leave */
		if(i >= length)
			break;

		lastdigittime = tris_tvnow();
	}

	digit_string[i] = '\0'; /* Nul terminate the end of the digit string */
	return res;
}

/*
* Write the metadata to the log file
*/
static int write_metadata( FILE *logfile, char *signalling_type, struct tris_channel *chan)
{
	int res = 0;
	struct timeval t;
	struct tris_tm now;
	char *cl,*cn;
	char workstring[80];
	char timestamp[80];
	
	/* Extract the caller ID location */
	if (chan->cid.cid_num)
		tris_copy_string(workstring, chan->cid.cid_num, sizeof(workstring));
	workstring[sizeof(workstring) - 1] = '\0';

	tris_callerid_parse(workstring, &cn, &cl);
	if (cl)
		tris_shrink_phone_number(cl);

	/* Get the current time */
	t = tris_tvnow();
	tris_localtime(&t, &now, NULL);

	/* Format the time */
	tris_strftime(timestamp, sizeof(timestamp), time_stamp_format, &now);

	res = fprintf(logfile, "\n\n[metadata]\n\n");

	if (res >= 0)
		res = fprintf(logfile, "PROTOCOL=%s\n", signalling_type);

	if (res >= 0)
		res = fprintf(logfile, "CALLINGFROM=%s\n", (!cl) ? "<unknown>" : cl);

	if (res >- 0)
		res = fprintf(logfile, "CALLERNAME=%s\n", (!cn) ? "<unknown>" : cn);

	if (res >= 0)
		res = fprintf(logfile, "TIMESTAMP=%s\n\n", timestamp);

	if (res >= 0)
		res = fprintf(logfile, "[events]\n\n");

	if (res < 0) {
		tris_verb(3, "AlarmReceiver: can't write metadata\n");
		tris_debug(1,"AlarmReceiver: can't write metadata\n");
	} else
		res = 0;

	return res;
}

/*
* Write a single event to the log file
*/
static int write_event( FILE *logfile,  event_node_t *event)
{
	int res = 0;

	if (fprintf(logfile, "%s\n", event->data) < 0)
		res = -1;

	return res;
}

/*
* If we are configured to log events, do so here.
*
*/
static int log_events(struct tris_channel *chan,  char *signalling_type, event_node_t *event)
{

	int res = 0;
	char workstring[sizeof(event_spool_dir)+sizeof(event_file)] = "";
	int fd;
	FILE *logfile;
	event_node_t *elp = event;
	
	if (!tris_strlen_zero(event_spool_dir)) {
		
		/* Make a template */
		tris_copy_string(workstring, event_spool_dir, sizeof(workstring));
		strncat(workstring, event_file, sizeof(workstring) - strlen(workstring) - 1);
		
		/* Make the temporary file */
		fd = mkstemp(workstring);
		
		if (fd == -1) {
			tris_verb(3, "AlarmReceiver: can't make temporary file\n");
			tris_debug(1,"AlarmReceiver: can't make temporary file\n");
			res = -1;
		}

		if (!res) {
			logfile = fdopen(fd, "w");
			if (logfile) {
				/* Write the file */
				res = write_metadata(logfile, signalling_type, chan);
				if (!res)
					while ((!res) && (elp != NULL)) {
						res = write_event(logfile, elp);
						elp = elp->next;
					}
				if (!res) {
					if (fflush(logfile) == EOF)
						res = -1;
					if (!res) {
						if (fclose(logfile) == EOF)
							res = -1;
					}
				}
			} else
				res = -1;
		}
	}

	return res;
}

/*
* This function implements the logic to receive the Ademco contact ID  format.
*
* The function will return 0 when the caller hangs up, else a -1 if there was a problem.
*/
static int receive_ademco_contact_id( struct tris_channel *chan, void *data, int fdto, int sdto, int tldn, event_node_t **ehead)
{
	int i, j;
	int res = 0;
	int checksum;
	char event[17];
	event_node_t *enew, *elp;
	int got_some_digits = 0;
	int events_received = 0;
	int ack_retries = 0;
	
	static char digit_map[15] = "0123456789*#ABC";
	static unsigned char digit_weights[15] = {10,1,2,3,4,5,6,7,8,9,11,12,13,14,15};

	database_increment("calls-received");

	/* Wait for first event */
	tris_verb(4, "AlarmReceiver: Waiting for first event from panel\n");

	while (res >= 0) {
		if (got_some_digits == 0) {
			/* Send ACK tone sequence */
			tris_verb(4, "AlarmReceiver: Sending 1400Hz 100ms burst (ACK)\n");
			res = send_tone_burst(chan, 1400.0, 100, tldn);
			if (!res)
				res = tris_safe_sleep(chan, 100);
			if (!res) {
				tris_verb(4, "AlarmReceiver: Sending 2300Hz 100ms burst (ACK)\n");
				res = send_tone_burst(chan, 2300.0, 100, tldn);
			}
		}
		if ( res >= 0)
			res = receive_dtmf_digits(chan, event, sizeof(event) - 1, fdto, sdto);
		if (res < 0) {
			if (events_received == 0) {
				/* Hangup with no events received should be logged in the DB */
				database_increment("no-events-received");
			} else {
				if (ack_retries) {
					tris_verb(4, "AlarmReceiver: ACK retries during this call: %d\n", ack_retries);
					database_increment("ack-retries");
				}
			}
			tris_verb(4, "AlarmReceiver: App exiting...\n");
			res = -1;
			break;
		}

		if (res != 0) {
			/* Didn't get all of the digits */
			tris_verb(2, "AlarmReceiver: Incomplete string: %s, trying again...\n", event);

			if (!got_some_digits) {
				got_some_digits = (!tris_strlen_zero(event)) ? 1 : 0;
				ack_retries++;
			}
			continue;
		}

		got_some_digits = 1;

		tris_verb(2, "AlarmReceiver: Received Event %s\n", event);
		tris_debug(1, "AlarmReceiver: Received event: %s\n", event);

		/* Calculate checksum */

		for (j = 0, checksum = 0; j < 16; j++) {
			for (i = 0; i < sizeof(digit_map); i++) {
				if (digit_map[i] == event[j])
					break;
			}

			if (i == 16)
				break;

			checksum += digit_weights[i];
		}
		if (i == 16) {
			tris_verb(2, "AlarmReceiver: Bad DTMF character %c, trying again\n", event[j]);
			continue; /* Bad character */
		}

		/* Checksum is mod(15) of the total */

		checksum = checksum % 15;

		if (checksum) {
			database_increment("checksum-errors");
			tris_verb(2, "AlarmReceiver: Nonzero checksum\n");
			tris_debug(1, "AlarmReceiver: Nonzero checksum\n");
			continue;
		}

		/* Check the message type for correctness */

		if (strncmp(event + 4, "18", 2)) {
			if (strncmp(event + 4, "98", 2)) {
				database_increment("format-errors");
				tris_verb(2, "AlarmReceiver: Wrong message type\n");
				tris_debug(1, "AlarmReceiver: Wrong message type\n");
			continue;
			}
		}

		events_received++;

		/* Queue the Event */
		if (!(enew = tris_calloc(1, sizeof(*enew)))) {
			res = -1;
			break;
		}

		enew->next = NULL;
		tris_copy_string(enew->data, event, sizeof(enew->data));

		/*
		* Insert event onto end of list
		*/
		if (*ehead == NULL)
			*ehead = enew;
		else {
			for(elp = *ehead; elp->next != NULL; elp = elp->next)
			;
			elp->next = enew;
		}

		if (res > 0)
			res = 0;

		/* Let the user have the option of logging the single event before sending the kissoff tone */
		if ((res == 0) && (log_individual_events))
			res = log_events(chan, ADEMCO_CONTACT_ID, enew);
		/* Wait 200 msec before sending kissoff */
		if (res == 0)
			res = tris_safe_sleep(chan, 200);

		/* Send the kissoff tone */
		if (res == 0)
			res = send_tone_burst(chan, 1400.0, 900, tldn);
	}

	return res;
}

/*
* This is the main function called by Trismedia Core whenever the App is invoked in the extension logic.
* This function will always return 0.
*/
static int alarmreceiver_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	event_node_t *elp, *efree;
	char signalling_type[64] = "";
	event_node_t *event_head = NULL;

	/* Set write and read formats to ULAW */
	tris_verb(4, "AlarmReceiver: Setting read and write formats to ULAW\n");

	if (tris_set_write_format(chan,TRIS_FORMAT_ULAW)) {
		tris_log(LOG_WARNING, "AlarmReceiver: Unable to set write format to Mu-law on %s\n",chan->name);
		return -1;
	}

	if (tris_set_read_format(chan,TRIS_FORMAT_ULAW)) {
		tris_log(LOG_WARNING, "AlarmReceiver: Unable to set read format to Mu-law on %s\n",chan->name);
		return -1;
	}

	/* Set default values for this invocation of the application */
	tris_copy_string(signalling_type, ADEMCO_CONTACT_ID, sizeof(signalling_type));

	/* Answer the channel if it is not already */
	tris_verb(4, "AlarmReceiver: Answering channel\n");
	if (chan->_state != TRIS_STATE_UP) {
		if ((res = tris_answer(chan)))
			return -1;
	}

	/* Wait for the connection to settle post-answer */
	tris_verb(4, "AlarmReceiver: Waiting for connection to stabilize\n");
	res = tris_safe_sleep(chan, 1250);

	/* Attempt to receive the events */
	if (!res) {
		/* Determine the protocol to receive in advance */
		/* Note: Ademco contact is the only one supported at this time */
		/* Others may be added later */
		if(!strcmp(signalling_type, ADEMCO_CONTACT_ID))
			receive_ademco_contact_id(chan, data, fdtimeout, sdtimeout, toneloudness, &event_head);
		else
			res = -1;
	}

	/* Events queued by receiver, write them all out here if so configured */
	if ((!res) && (log_individual_events == 0))
		res = log_events(chan, signalling_type, event_head);

	/*
	* Do we exec a command line at the end?
	*/
	if ((!res) && (!tris_strlen_zero(event_app)) && (event_head)) {
		tris_debug(1,"Alarmreceiver: executing: %s\n", event_app);
		tris_safe_system(event_app);
	}

	/*
	* Free up the data allocated in our linked list
	*/
	for (elp = event_head; (elp != NULL);) {
		efree = elp;
		elp = elp->next;
		tris_free(efree);
	}

	return 0;
}

/*
* Load the configuration from the configuration file
*/
static int load_config(void)
{
	struct tris_config *cfg;
	const char *p;
	struct tris_flags config_flags = { 0 };

	/* Read in the config file */
	cfg = tris_config_load(ALMRCV_CONFIG, config_flags);

	if (!cfg) {
		tris_verb(4, "AlarmReceiver: No config file\n");
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Config file %s is in an invalid format.  Aborting.\n", ALMRCV_CONFIG);
		return 0;
	} else {
		p = tris_variable_retrieve(cfg, "general", "eventcmd");
		if (p) {
			tris_copy_string(event_app, p, sizeof(event_app));
			event_app[sizeof(event_app) - 1] = '\0';
		}
		p = tris_variable_retrieve(cfg, "general", "loudness");
		if (p) {
			toneloudness = atoi(p);
			if(toneloudness < 100)
				toneloudness = 100;
			if(toneloudness > 8192)
				toneloudness = 8192;
		}
		p = tris_variable_retrieve(cfg, "general", "fdtimeout");
		if (p) {
			fdtimeout = atoi(p);
			if(fdtimeout < 1000)
				fdtimeout = 1000;
			if(fdtimeout > 10000)
				fdtimeout = 10000;
		}

		p = tris_variable_retrieve(cfg, "general", "sdtimeout");
		if (p) {
			sdtimeout = atoi(p);
			if(sdtimeout < 110)
				sdtimeout = 110;
			if(sdtimeout > 4000)
				sdtimeout = 4000;
		}

		p = tris_variable_retrieve(cfg, "general", "logindividualevents");
		if (p)
			log_individual_events = tris_true(p);

		p = tris_variable_retrieve(cfg, "general", "eventspooldir");
		if (p) {
			tris_copy_string(event_spool_dir, p, sizeof(event_spool_dir));
			event_spool_dir[sizeof(event_spool_dir) - 1] = '\0';
		}

		p = tris_variable_retrieve(cfg, "general", "timestampformat");
		if (p) {
			tris_copy_string(time_stamp_format, p, sizeof(time_stamp_format));
			time_stamp_format[sizeof(time_stamp_format) - 1] = '\0';
		}

		p = tris_variable_retrieve(cfg, "general", "db-family");
		if (p) {
			tris_copy_string(db_family, p, sizeof(db_family));
			db_family[sizeof(db_family) - 1] = '\0';
		}
		tris_config_destroy(cfg);
	}
	return 1;
}

/*
* These functions are required to implement an Trismedia App.
*/
static int unload_module(void)
{
	return tris_unregister_application(app);
}

static int load_module(void)
{
	if (load_config()) {
		if (tris_register_application_xml(app, alarmreceiver_exec))
			return TRIS_MODULE_LOAD_FAILURE;
		return TRIS_MODULE_LOAD_SUCCESS;
	} else
		return TRIS_MODULE_LOAD_DECLINE;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Alarm Receiver for Trismedia");
