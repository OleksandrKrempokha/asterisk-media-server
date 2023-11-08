/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Russell Bryant <russelb@clemson.edu>
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
 * \brief Applications to test connection and produce report in text file
 *
 * \author Mark Spencer <markster@digium.com>
 * \author Russell Bryant <russelb@clemson.edu>
 * 
 * \ingroup applications
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 237924 $")

#include <sys/stat.h>

#include "trismedia/paths.h"	/* use tris_config_TRIS_LOG_DIR */
#include "trismedia/channel.h"
#include "trismedia/module.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"

/*** DOCUMENTATION
	<application name="TestServer" language="en_US">
		<synopsis>
			Execute Interface Test Server.
		</synopsis>
		<syntax />
		<description>
			<para>Perform test server function and write call report. Results stored in
			<filename>/var/log/trismedia/testreports/&lt;testid&gt;-server.txt</filename></para>
		</description>
		<see-also>
			<ref type="application">TestClient</ref>
		</see-also>
	</application>
	<application name="TestClient" language="en_US">
		<synopsis>
			Execute Interface Test Client.
		</synopsis>
		<syntax>
			<parameter name="testid" required="true">
				<para>An ID to identify this test.</para>
			</parameter>
		</syntax>
		<description>
			<para>Executes test client with given <replaceable>testid</replaceable>. Results stored in
			<filename>/var/log/trismedia/testreports/&lt;testid&gt;-client.txt</filename></para>
		</description>
		<see-also>
			<ref type="application">TestServer</ref>
		</see-also>
	</application>
 ***/

static char *tests_app = "TestServer";
static char *testc_app = "TestClient";

static int measurenoise(struct tris_channel *chan, int ms, char *who)
{
	int res=0;
	int mssofar;
	int noise=0;
	int samples=0;
	int x;
	short *foo;
	struct timeval start;
	struct tris_frame *f;
	int rformat;
	rformat = chan->readformat;
	if (tris_set_read_format(chan, TRIS_FORMAT_SLINEAR)) {
		tris_log(LOG_NOTICE, "Unable to set to linear mode!\n");
		return -1;
	}
	start = tris_tvnow();
	for(;;) {
		mssofar = tris_tvdiff_ms(tris_tvnow(), start);
		if (mssofar > ms)
			break;
		res = tris_waitfor(chan, ms - mssofar);
		if (res < 1)
			break;
		f = tris_read(chan);
		if (!f) {
			res = -1;
			break;
		}
		if ((f->frametype == TRIS_FRAME_VOICE) && (f->subclass == TRIS_FORMAT_SLINEAR)) {
			foo = (short *)f->data.ptr;
			for (x=0;x<f->samples;x++) {
				noise += abs(foo[x]);
				samples++;
			}
		}
		tris_frfree(f);
	}

	if (rformat) {
		if (tris_set_read_format(chan, rformat)) {
			tris_log(LOG_NOTICE, "Unable to restore original format!\n");
			return -1;
		}
	}
	if (res < 0)
		return res;
	if (!samples) {
		tris_log(LOG_NOTICE, "No samples were received from the other side!\n");
		return -1;
	}
	tris_debug(1, "%s: Noise: %d, samples: %d, avg: %d\n", who, noise, samples, noise / samples);
	return (noise / samples);
}

static int sendnoise(struct tris_channel *chan, int ms) 
{
	int res;
	res = tris_tonepair_start(chan, 1537, 2195, ms, 8192);
	if (!res) {
		res = tris_waitfordigit(chan, ms);
		tris_tonepair_stop(chan);
	}
	return res;	
}

static int testclient_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char *testid=data;
	char fn[80];
	char serverver[80];
	FILE *f;
	
	/* Check for test id */
	if (tris_strlen_zero(testid)) {
		tris_log(LOG_WARNING, "TestClient requires an argument - the test id\n");
		return -1;
	}
	
	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);
	
	/* Wait a few just to be sure things get started */
	res = tris_safe_sleep(chan, 3000);
	/* Transmit client version */
	if (!res)
		res = tris_dtmf_stream(chan, NULL, "8378*1#", 0, 0);
	tris_debug(1, "Transmit client version\n");
	
	/* Read server version */
	tris_debug(1, "Read server version\n");
	if (!res) 
		res = tris_app_getdata(chan, NULL, serverver, sizeof(serverver) - 1, 0);
	if (res > 0)
		res = 0;
	tris_debug(1, "server version: %s\n", serverver);
		
	if (res > 0)
		res = 0;

	if (!res)
		res = tris_safe_sleep(chan, 1000);
	/* Send test id */
	if (!res) 
		res = tris_dtmf_stream(chan, NULL, testid, 0, 0);		
	if (!res) 
		res = tris_dtmf_stream(chan, NULL, "#", 0, 0);		
	tris_debug(1, "send test identifier: %s\n", testid);

	if ((res >=0) && (!tris_strlen_zero(testid))) {
		/* Make the directory to hold the test results in case it's not there */
		snprintf(fn, sizeof(fn), "%s/testresults", tris_config_TRIS_LOG_DIR);
		tris_mkdir(fn, 0777);
		snprintf(fn, sizeof(fn), "%s/testresults/%s-client.txt", tris_config_TRIS_LOG_DIR, testid);
		if ((f = fopen(fn, "w+"))) {
			setlinebuf(f);
			fprintf(f, "CLIENTCHAN:    %s\n", chan->name);
			fprintf(f, "CLIENTTEST ID: %s\n", testid);
			fprintf(f, "ANSWER:        PASS\n");
			res = 0;
			
			if (!res) {
				/* Step 1: Wait for "1" */
				tris_debug(1, "TestClient: 2.  Wait DTMF 1\n");
				res = tris_waitfordigit(chan, 3000);
				fprintf(f, "WAIT DTMF 1:   %s\n", (res != '1') ? "FAIL" : "PASS");
				if (res == '1')
					res = 0;
				else
					res = -1;
			}
			if (!res)
				res = tris_safe_sleep(chan, 1000);
			if (!res) {
				/* Step 2: Send "2" */
				tris_debug(1, "TestClient: 2.  Send DTMF 2\n");
				res = tris_dtmf_stream(chan, NULL, "2", 0, 0);
				fprintf(f, "SEND DTMF 2:   %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}
			if (!res) {
				/* Step 3: Wait one second */
				tris_debug(1, "TestClient: 3.  Wait one second\n");
				res = tris_safe_sleep(chan, 1000);
				fprintf(f, "WAIT 1 SEC:    %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}			
			if (!res) {
				/* Step 4: Measure noise */
				tris_debug(1, "TestClient: 4.  Measure noise\n");
				res = measurenoise(chan, 5000, "TestClient");
				fprintf(f, "MEASURENOISE:  %s (%d)\n", (res < 0) ? "FAIL" : "PASS", res);
				if (res > 0)
					res = 0;
			}
			if (!res) {
				/* Step 5: Wait for "4" */
				tris_debug(1, "TestClient: 5.  Wait DTMF 4\n");
				res = tris_waitfordigit(chan, 3000);
				fprintf(f, "WAIT DTMF 4:   %s\n", (res != '4') ? "FAIL" : "PASS");
				if (res == '4')
					res = 0;
				else
					res = -1;
			}
			if (!res) {
				/* Step 6: Transmit tone noise */
				tris_debug(1, "TestClient: 6.  Transmit tone\n");
				res = sendnoise(chan, 6000);
				fprintf(f, "SENDTONE:      %s\n", (res < 0) ? "FAIL" : "PASS");
			}
			if (!res || (res == '5')) {
				/* Step 7: Wait for "5" */
				tris_debug(1, "TestClient: 7.  Wait DTMF 5\n");
				if (!res)
					res = tris_waitfordigit(chan, 3000);
				fprintf(f, "WAIT DTMF 5:   %s\n", (res != '5') ? "FAIL" : "PASS");
				if (res == '5')
					res = 0;
				else
					res = -1;
			}
			if (!res) {
				/* Step 8: Wait one second */
				tris_debug(1, "TestClient: 8.  Wait one second\n");
				res = tris_safe_sleep(chan, 1000);
				fprintf(f, "WAIT 1 SEC:    %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}
			if (!res) {
				/* Step 9: Measure noise */
				tris_debug(1, "TestClient: 6.  Measure tone\n");
				res = measurenoise(chan, 4000, "TestClient");
				fprintf(f, "MEASURETONE:   %s (%d)\n", (res < 0) ? "FAIL" : "PASS", res);
				if (res > 0)
					res = 0;
			}
			if (!res) {
				/* Step 10: Send "7" */
				tris_debug(1, "TestClient: 7.  Send DTMF 7\n");
				res = tris_dtmf_stream(chan, NULL, "7", 0, 0);
				fprintf(f, "SEND DTMF 7:   %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res =0;
			}
			if (!res) {
				/* Step 11: Wait for "8" */
				tris_debug(1, "TestClient: 11.  Wait DTMF 8\n");
				res = tris_waitfordigit(chan, 3000);
				fprintf(f, "WAIT DTMF 8:   %s\n", (res != '8') ? "FAIL" : "PASS");
				if (res == '8')
					res = 0;
				else
					res = -1;
			}
			if (!res) {
				res = tris_safe_sleep(chan, 1000);
			}
			if (!res) {
				/* Step 12: Hangup! */
				tris_debug(1, "TestClient: 12.  Hangup\n");
			}

			tris_debug(1, "-- TEST COMPLETE--\n");
			fprintf(f, "-- END TEST--\n");
			fclose(f);
			res = -1;
		} else
			res = -1;
	} else {
		tris_log(LOG_NOTICE, "Did not read a test ID on '%s'\n", chan->name);
		res = -1;
	}
	return res;
}

static int testserver_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char testid[80]="";
	char fn[80];
	FILE *f;
	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);
	/* Read version */
	tris_debug(1, "Read client version\n");
	if (!res) 
		res = tris_app_getdata(chan, NULL, testid, sizeof(testid) - 1, 0);
	if (res > 0)
		res = 0;

	tris_debug(1, "client version: %s\n", testid);
	tris_debug(1, "Transmit server version\n");

	res = tris_safe_sleep(chan, 1000);
	if (!res)
		res = tris_dtmf_stream(chan, NULL, "8378*1#", 0, 0);
	if (res > 0)
		res = 0;

	if (!res) 
		res = tris_app_getdata(chan, NULL, testid, sizeof(testid) - 1, 0);		
	tris_debug(1, "read test identifier: %s\n", testid);
	/* Check for sneakyness */
	if (strchr(testid, '/'))
		res = -1;
	if ((res >=0) && (!tris_strlen_zero(testid))) {
		/* Got a Test ID!  Whoo hoo! */
		/* Make the directory to hold the test results in case it's not there */
		snprintf(fn, sizeof(fn), "%s/testresults", tris_config_TRIS_LOG_DIR);
		tris_mkdir(fn, 0777);
		snprintf(fn, sizeof(fn), "%s/testresults/%s-server.txt", tris_config_TRIS_LOG_DIR, testid);
		if ((f = fopen(fn, "w+"))) {
			setlinebuf(f);
			fprintf(f, "SERVERCHAN:    %s\n", chan->name);
			fprintf(f, "SERVERTEST ID: %s\n", testid);
			fprintf(f, "ANSWER:        PASS\n");
			tris_debug(1, "Processing Test ID '%s'\n", testid);
			res = tris_safe_sleep(chan, 1000);
			if (!res) {
				/* Step 1: Send "1" */
				tris_debug(1, "TestServer: 1.  Send DTMF 1\n");
				res = tris_dtmf_stream(chan, NULL, "1", 0,0 );
				fprintf(f, "SEND DTMF 1:   %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}
			if (!res) {
				/* Step 2: Wait for "2" */
				tris_debug(1, "TestServer: 2.  Wait DTMF 2\n");
				res = tris_waitfordigit(chan, 3000);
				fprintf(f, "WAIT DTMF 2:   %s\n", (res != '2') ? "FAIL" : "PASS");
				if (res == '2')
					res = 0;
				else
					res = -1;
			}
			if (!res) {
				/* Step 3: Measure noise */
				tris_debug(1, "TestServer: 3.  Measure noise\n");
				res = measurenoise(chan, 6000, "TestServer");
				fprintf(f, "MEASURENOISE:  %s (%d)\n", (res < 0) ? "FAIL" : "PASS", res);
				if (res > 0)
					res = 0;
			}
			if (!res) {
				/* Step 4: Send "4" */
				tris_debug(1, "TestServer: 4.  Send DTMF 4\n");
				res = tris_dtmf_stream(chan, NULL, "4", 0, 0);
				fprintf(f, "SEND DTMF 4:   %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}
		
			if (!res) {
				/* Step 5: Wait one second */
				tris_debug(1, "TestServer: 5.  Wait one second\n");
				res = tris_safe_sleep(chan, 1000);
				fprintf(f, "WAIT 1 SEC:    %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}
		
			if (!res) {
				/* Step 6: Measure noise */
				tris_debug(1, "TestServer: 6.  Measure tone\n");
				res = measurenoise(chan, 4000, "TestServer");
				fprintf(f, "MEASURETONE:   %s (%d)\n", (res < 0) ? "FAIL" : "PASS", res);
				if (res > 0)
					res = 0;
			}

			if (!res) {
				/* Step 7: Send "5" */
				tris_debug(1, "TestServer: 7.  Send DTMF 5\n");
				res = tris_dtmf_stream(chan, NULL, "5", 0, 0);
				fprintf(f, "SEND DTMF 5:   %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}

			if (!res) {
				/* Step 8: Transmit tone noise */
				tris_debug(1, "TestServer: 8.  Transmit tone\n");
				res = sendnoise(chan, 6000);
				fprintf(f, "SENDTONE:      %s\n", (res < 0) ? "FAIL" : "PASS");
			}
		
			if (!res || (res == '7')) {
				/* Step 9: Wait for "7" */
				tris_debug(1, "TestServer: 9.  Wait DTMF 7\n");
				if (!res)
					res = tris_waitfordigit(chan, 3000);
				fprintf(f, "WAIT DTMF 7:   %s\n", (res != '7') ? "FAIL" : "PASS");
				if (res == '7')
					res = 0;
				else
					res = -1;
			}
			if (!res)
				res = tris_safe_sleep(chan, 1000);
			if (!res) {
				/* Step 10: Send "8" */
				tris_debug(1, "TestServer: 10.  Send DTMF 8\n");
				res = tris_dtmf_stream(chan, NULL, "8", 0, 0);
				fprintf(f, "SEND DTMF 8:   %s\n", (res < 0) ? "FAIL" : "PASS");
				if (res > 0)
					res = 0;
			}
			if (!res) {
				/* Step 11: Wait for hangup to arrive! */
				tris_debug(1, "TestServer: 11.  Waiting for hangup\n");
				res = tris_safe_sleep(chan, 10000);
				fprintf(f, "WAIT HANGUP:   %s\n", (res < 0) ? "PASS" : "FAIL");
			}

			tris_log(LOG_NOTICE, "-- TEST COMPLETE--\n");
			fprintf(f, "-- END TEST--\n");
			fclose(f);
			res = -1;
		} else
			res = -1;
	} else {
		tris_log(LOG_NOTICE, "Did not read a test ID on '%s'\n", chan->name);
		res = -1;
	}
	return res;
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(testc_app);
	res |= tris_unregister_application(tests_app);

	return res;	
}

static int load_module(void)
{
	int res;

	res = tris_register_application_xml(testc_app, testclient_exec);
	res |= tris_register_application_xml(tests_app, testserver_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Interface Test Application");
