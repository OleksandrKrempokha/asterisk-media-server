/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2008, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief videodial() & retryvideodial() - Trivial application to videodial a channel and send an URL on answer
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>chan_local</depend>
 ***/


#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 244395 $")

#include <sys/time.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include "trismedia/paths.h" /* use tris_config_TRIS_DATA_DIR */
#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/module.h"
#include "trismedia/translate.h"
#include "trismedia/say.h"
#include "trismedia/config.h"
#include "trismedia/features.h"
#include "trismedia/musiconhold.h"
#include "trismedia/callerid.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"
#include "trismedia/causes.h"
#include "trismedia/rtp.h"
#include "trismedia/cdr.h"
#include "trismedia/manager.h"
#include "trismedia/privacy.h"
#include "trismedia/stringfields.h"
#include "trismedia/global_datastores.h"
#include "trismedia/dsp.h"
#include "trismedia/res_odbc.h"

/*** DOCUMENTATION
	<application name="Videodial" language="en_US">
		<synopsis>
			Attempt to connect to another device or endpoint and bridge the call.
		</synopsis>
		<syntax>
			<parameter name="Technology/Resource" required="true" argsep="&amp;">
				<argument name="Technology/Resource" required="true">
					<para>Specification of the device(s) to videodial.  These must be in the format of
					<literal>Technology/Resource</literal>, where <replaceable>Technology</replaceable>
					represents a particular channel driver, and <replaceable>Resource</replaceable>
					represents a resource available to that particular channel driver.</para>
				</argument>
				<argument name="Technology2/Resource2" required="false" multiple="true">
					<para>Optional extra devices to videodial in parallel</para>
					<para>If you need more then one enter them as
					Technology2/Resource2&amp;Technology3/Resourse3&amp;.....</para>
				</argument>
			</parameter>
			<parameter name="timeout" required="false">
				<para>Specifies the number of seconds we attempt to videodial the specified devices</para>
				<para>If not specified, this defaults to 136 years.</para>
			</parameter>
			<parameter name="options" required="false">
			   <optionlist>
				<option name="A">
					<argument name="x" required="true">
						<para>The file to play to the called party</para>
					</argument>
					<para>Play an announcement to the called party, where <replaceable>x</replaceable> is the prompt to be played</para>
				</option>
				<option name="C">
					<para>Reset the call detail record (CDR) for this call.</para>
				</option>
				<option name="c">
					<para>If the Videodial() application cancels this call, always set the flag to tell the channel
					driver that the call is answered elsewhere.</para>
				</option>
				<option name="d">
					<para>Allow the calling user to videodial a 1 digit extension while waiting for
					a call to be answered. Exit to that extension if it exists in the
					current context, or the context defined in the <variable>EXITCONTEXT</variable> variable,
					if it exists.</para>
				</option>
				<option name="D" argsep=":">
					<argument name="called" />
					<argument name="calling" />
					<para>Send the specified DTMF strings <emphasis>after</emphasis> the called
					party has answered, but before the call gets bridged. The 
					<replaceable>called</replaceable> DTMF string is sent to the called party, and the 
					<replaceable>calling</replaceable> DTMF string is sent to the calling party. Both arguments 
					can be used alone.</para>
				</option>
				<option name="e">
					<para>Execute the <literal>h</literal> extension for peer after the call ends</para>
				</option>
				<option name="f">
					<para>Force the callerid of the <emphasis>calling</emphasis> channel to be set as the
					extension associated with the channel using a videodialplan <literal>hint</literal>.
					For example, some PSTNs do not allow CallerID to be set to anything
					other than the number assigned to the caller.</para>
				</option>
				<option name="F" argsep="^">
					<argument name="context" required="false" />
					<argument name="exten" required="false" />
					<argument name="priority" required="true" />
					<para>When the caller hangs up, transfer the called party
					to the specified destination and continue execution at that location.</para>
				</option>
				<option name="g">
					<para>Proceed with videodialplan execution at the next priority in the current extension if the
					destination channel hangs up.</para>
				</option>
				<option name="G" argsep="^">
					<argument name="context" required="false" />
					<argument name="exten" required="false" />
					<argument name="priority" required="true" />
					<para>If the call is answered, transfer the calling party to
					the specified <replaceable>priority</replaceable> and the called party to the specified 
					<replaceable>priority</replaceable> plus one.</para>
					<note>
						<para>You cannot use any additional action post answer options in conjunction with this option.</para>
					</note>
				</option>
				<option name="h">
					<para>Allow the called party to hang up by sending the <literal>*</literal> DTMF digit.</para>
				</option>
				<option name="H">
					<para>Allow the calling party to hang up by hitting the <literal>*</literal> DTMF digit.</para>
				</option>
				<option name="i">
					<para>Trismedia will ignore any forwarding requests it may receive on this videodial attempt.</para>
				</option>
				<option name="k">
					<para>Allow the called party to enable parking of the call by sending
					the DTMF sequence defined for call parking in <filename>features.conf</filename>.</para>
				</option>
				<option name="K">
					<para>Allow the calling party to enable parking of the call by sending
					the DTMF sequence defined for call parking in <filename>features.conf</filename>.</para>
				</option>
				<option name="L" argsep=":">
					<argument name="x" required="true">
						<para>Maximum call time, in milliseconds</para>
					</argument>
					<argument name="y">
						<para>Warning time, in milliseconds</para>
					</argument>
					<argument name="z">
						<para>Repeat time, in milliseconds</para>
					</argument>
					<para>Limit the call to <replaceable>x</replaceable> milliseconds. Play a warning when <replaceable>y</replaceable> milliseconds are
					left. Repeat the warning every <replaceable>z</replaceable> milliseconds until time expires.</para>
					<para>This option is affected by the following variables:</para>
					<variablelist>
						<variable name="LIMIT_PLAYAUDIO_CALLER">
							<value name="yes" default="true" />
							<value name="no" />
							<para>If set, this variable causes Trismedia to play the prompts to the caller.</para>
						</variable>
						<variable name="LIMIT_PLAYAUDIO_CALLEE">
							<value name="yes" />
							<value name="no" default="true"/>
							<para>If set, this variable causes Trismedia to play the prompts to the callee.</para>
						</variable>
						<variable name="LIMIT_TIMEOUT_FILE">
							<value name="filename"/>
							<para>If specified, <replaceable>filename</replaceable> specifies the sound prompt to play when the timeout is reached.
							If not set, the time remaining will be announced.</para>
						</variable>
						<variable name="LIMIT_CONNECT_FILE">
							<value name="filename"/>
							<para>If specified, <replaceable>filename</replaceable> specifies the sound prompt to play when the call begins.
							If not set, the time remaining will be announced.</para>
						</variable>
						<variable name="LIMIT_WARNING_FILE">
							<value name="filename"/>
							<para>If specified, <replaceable>filename</replaceable> specifies the sound prompt to play as
							a warning when time <replaceable>x</replaceable> is reached. If not set, the time remaining will be announced.</para>
						</variable>
					</variablelist>
				</option>
				<option name="m">
					<argument name="class" required="false"/>
					<para>Provide hold music to the calling party until a requested
					channel answers. A specific music on hold <replaceable>class</replaceable>
					(as defined in <filename>musiconhold.conf</filename>) can be specified.</para>
				</option>
				<option name="M" argsep="^">
					<argument name="macro" required="true">
						<para>Name of the macro that should be executed.</para>
					</argument>
					<argument name="arg" multiple="true">
						<para>Macro arguments</para>
					</argument>
					<para>Execute the specified <replaceable>macro</replaceable> for the <emphasis>called</emphasis> channel 
					before connecting to the calling channel. Arguments can be specified to the Macro
					using <literal>^</literal> as a delimiter. The macro can set the variable
					<variable>MACRO_RESULT</variable> to specify the following actions after the macro is
					finished executing:</para>
					<variablelist>
						<variable name="MACRO_RESULT">
							<para>If set, this action will be taken after the macro finished executing.</para>
							<value name="ABORT">
								Hangup both legs of the call
							</value>
							<value name="CONGESTION">
								Behave as if line congestion was encountered
							</value>
							<value name="BUSY">
								Behave as if a busy signal was encountered
							</value>
							<value name="CONTINUE">
								Hangup the called party and allow the calling party to continue videodialplan execution at the next priority
							</value>
							<!-- TODO: Fix this syntax up, once we've figured out how to specify the GOTO syntax -->
							<value name="GOTO:&lt;context&gt;^&lt;exten&gt;^&lt;priority&gt;">
								Transfer the call to the specified destination.
							</value>
						</variable>
					</variablelist>
					<note>
						<para>You cannot use any additional action post answer options in conjunction
						with this option. Also, pbx services are not run on the peer (called) channel,
						so you will not be able to set timeouts via the TIMEOUT() function in this macro.</para>
					</note>
					<warning><para>Be aware of the limitations that macros have, specifically with regards to use of
					the <literal>WaitExten</literal> application. For more information, see the documentation for
					Macro()</para></warning>
				</option>
				<option name="n">
				        <argument name="delete">
					        <para>With <replaceable>delete</replaceable> either not specified or set to <literal>0</literal>,
						the recorded introduction will not be deleted if the caller hangs up while the remote party has not
						yet answered.</para>
						<para>With <replaceable>delete</replaceable> set to <literal>1</literal>, the introduction will
						always be deleted.</para>
					</argument>
					<para>This option is a modifier for the call screening/privacy mode. (See the 
					<literal>p</literal> and <literal>P</literal> options.) It specifies
					that no introductions are to be saved in the <directory>priv-callerintros</directory>
					directory.</para>
				</option>
				<option name="N">
					<para>This option is a modifier for the call screening/privacy mode. It specifies
					that if Caller*ID is present, do not screen the call.</para>
				</option>
				<option name="o">
					<para>Specify that the Caller*ID that was present on the <emphasis>calling</emphasis> channel
					be set as the Caller*ID on the <emphasis>called</emphasis> channel. This was the
					behavior of Trismedia 1.0 and earlier.</para>
				</option>
				<option name="O">
					<argument name="mode">
						<para>With <replaceable>mode</replaceable> either not specified or set to <literal>1</literal>,
						the originator hanging up will cause the phone to ring back immediately.</para>
						<para>With <replaceable>mode</replaceable> set to <literal>2</literal>, when the operator 
						flashes the trunk, it will ring their phone back.</para>
					</argument>
					<para>Enables <emphasis>operator services</emphasis> mode.  This option only
					works when bridging a DAHDI channel to another DAHDI channel
					only. if specified on non-DAHDI interfaces, it will be ignored.
					When the destination answers (presumably an operator services
					station), the originator no longer has control of their line.
					They may hang up, but the switch will not release their line
					until the destination party (the operator) hangs up.</para>
				</option>
				<option name="p">
					<para>This option enables screening mode. This is basically Privacy mode
					without memory.</para>
				</option>
				<option name="P">
					<argument name="x" />
					<para>Enable privacy mode. Use <replaceable>x</replaceable> as the family/key in the AstDB database if
					it is provided. The current extension is used if a database family/key is not specified.</para>
				</option>
				<option name="r">
					<para>Indicate ringing to the calling party, even if the called party isn't actually ringing. Pass no audio to the calling
					party until the called channel has answered.</para>
				</option>
				<option name="S">
					<argument name="x" required="true" />
					<para>Hang up the call <replaceable>x</replaceable> seconds <emphasis>after</emphasis> the called party has
					answered the call.</para>
				</option>
				<option name="t">
					<para>Allow the called party to transfer the calling party by sending the
					DTMF sequence defined in <filename>features.conf</filename>.</para>
				</option>
				<option name="T">
					<para>Allow the calling party to transfer the called party by sending the
					DTMF sequence defined in <filename>features.conf</filename>.</para>
				</option>
				<option name="U" argsep="^">
					<argument name="x" required="true">
						<para>Name of the subroutine to execute via Gosub</para>
					</argument>
					<argument name="arg" multiple="true" required="false">
						<para>Arguments for the Gosub routine</para>
					</argument>
					<para>Execute via Gosub the routine <replaceable>x</replaceable> for the <emphasis>called</emphasis> channel before connecting
					to the calling channel. Arguments can be specified to the Gosub
					using <literal>^</literal> as a delimiter. The Gosub routine can set the variable
					<variable>GOSUB_RESULT</variable> to specify the following actions after the Gosub returns.</para>
					<variablelist>
						<variable name="GOSUB_RESULT">
							<value name="ABORT">
								Hangup both legs of the call.
							</value>
							<value name="CONGESTION">
								Behave as if line congestion was encountered.
							</value>
							<value name="BUSY">
								Behave as if a busy signal was encountered.
							</value>
							<value name="CONTINUE">
								Hangup the called party and allow the calling party
								to continue videodialplan execution at the next priority.
							</value>
							<!-- TODO: Fix this syntax up, once we've figured out how to specify the GOTO syntax -->
							<value name="GOTO:&lt;context&gt;^&lt;exten&gt;^&lt;priority&gt;">
								Transfer the call to the specified priority. Optionally, an extension, or
								extension and priority can be specified.
							</value>
						</variable>
					</variablelist>
					<note>
						<para>You cannot use any additional action post answer options in conjunction
						with this option. Also, pbx services are not run on the peer (called) channel,
						so you will not be able to set timeouts via the TIMEOUT() function in this routine.</para>
					</note>
				</option>
				<option name="w">
					<para>Allow the called party to enable recording of the call by sending
					the DTMF sequence defined for one-touch recording in <filename>features.conf</filename>.</para>
				</option>
				<option name="W">
					<para>Allow the calling party to enable recording of the call by sending
					the DTMF sequence defined for one-touch recording in <filename>features.conf</filename>.</para>
				</option>
				<option name="x">
					<para>Allow the called party to enable recording of the call by sending
					the DTMF sequence defined for one-touch automixmonitor in <filename>features.conf</filename>.</para>
				</option>
				<option name="X">
					<para>Allow the calling party to enable recording of the call by sending
					the DTMF sequence defined for one-touch automixmonitor in <filename>features.conf</filename>.</para>
				</option>
				</optionlist>
			</parameter>
			<parameter name="URL">
				<para>The optional URL will be sent to the called party if the channel driver supports it.</para>
			</parameter>
		</syntax>
		<description>
			<para>This application will place calls to one or more specified channels. As soon
			as one of the requested channels answers, the originating channel will be
			answered, if it has not already been answered. These two channels will then
			be active in a bridged call. All other channels that were requested will then
			be hung up.</para>

			<para>Unless there is a timeout specified, the Videodial application will wait
			indefinitely until one of the called channels answers, the user hangs up, or
			if all of the called channels are busy or unavailable. Videodialplan executing will
			continue if no requested channels can be called, or if the timeout expires.
			This application will report normal termination if the originating channel
			hangs up, or if the call is bridged and either of the parties in the bridge
			ends the call.</para>

			<para>If the <variable>OUTBOUND_GROUP</variable> variable is set, all peer channels created by this
			application will be put into that group (as in Set(GROUP()=...).
			If the <variable>OUTBOUND_GROUP_ONCE</variable> variable is set, all peer channels created by this
			application will be put into that group (as in Set(GROUP()=...). Unlike OUTBOUND_GROUP,
			however, the variable will be unset after use.</para>

			<para>This application sets the following channel variables:</para>
			<variablelist>
				<variable name="VIDEODIALEDTIME">
					<para>This is the time from videodialing a channel until when it is disconnected.</para>
				</variable>
				<variable name="ANSWEREDTIME">
					<para>This is the amount of time for actual call.</para>
				</variable>
				<variable name="VIDEODIALSTATUS">
					<para>This is the status of the call</para>
					<value name="CHANUNAVAIL" />
					<value name="CONGESTION" />
					<value name="NOANSWER" />
					<value name="BUSY" />
					<value name="ANSWER" />
					<value name="CANCEL" />
					<value name="DONTCALL">
						For the Privacy and Screening Modes.
						Will be set if the called party chooses to send the calling party to the 'Go Away' script.
					</value>
					<value name="TORTURE">
						For the Privacy and Screening Modes.
						Will be set if the called party chooses to send the calling party to the 'torture' script.
					</value>
					<value name="INVALIDARGS" />
				</variable>
			</variablelist>
		</description>
	</application>
	<application name="RetryVideodial" language="en_US">
		<synopsis>
			Place a call, retrying on failure allowing an optional exit extension.
		</synopsis>
		<syntax>
			<parameter name="announce" required="true">
				<para>Filename of sound that will be played when no channel can be reached</para>
			</parameter>
			<parameter name="sleep" required="true">
				<para>Number of seconds to wait after a videodial attempt failed before a new attempt is made</para>
			</parameter>
			<parameter name="retries" required="true">
				<para>Number of retries</para>
				<para>When this is reached flow will continue at the next priority in the videodialplan</para>
			</parameter>
			<parameter name="videodialargs" required="true">
				<para>Same format as arguments provided to the Videodial application</para>
			</parameter>
		</syntax>
		<description>
			<para>This application will attempt to place a call using the normal Videodial application.
			If no channel can be reached, the <replaceable>announce</replaceable> file will be played.
			Then, it will wait <replaceable>sleep</replaceable> number of seconds before retrying the call.
			After <replaceable>retries</replaceable> number of attempts, the calling channel will continue at the next priority in the videodialplan.
			If the <replaceable>retries</replaceable> setting is set to 0, this application will retry endlessly.
			While waiting to retry a call, a 1 digit extension may be videodialed. If that
			extension exists in either the context defined in <variable>EXITCONTEXT</variable> or the current
			one, The call will jump to that extension immediately.
			The <replaceable>videodialargs</replaceable> are specified in the same format that arguments are provided
			to the Videodial application.</para>
		</description>
	</application>
 ***/

static char *app = "Videodial";
static char *rapp = "RetryVideodial";

enum {
	OPT_ANNOUNCE =          (1 << 0),
	OPT_RESETCDR =          (1 << 1),
	OPT_DTMF_EXIT =         (1 << 2),
	OPT_SENDDTMF =          (1 << 3),
	OPT_FORCECLID =         (1 << 4),
	OPT_GO_ON =             (1 << 5),
	OPT_CALLEE_HANGUP =     (1 << 6),
	OPT_CALLER_HANGUP =     (1 << 7),
	OPT_DURATION_LIMIT =    (1 << 9),
	OPT_MUSICBACK =         (1 << 10),
	OPT_CALLEE_MACRO =      (1 << 11),
	OPT_SCREEN_NOINTRO =    (1 << 12),
	OPT_SCREEN_NOCLID =     (1 << 13),
	OPT_ORIGINAL_CLID =     (1 << 14),
	OPT_SCREENING =         (1 << 15),
	OPT_PRIVACY =           (1 << 16),
	OPT_RINGBACK =          (1 << 17),
	OPT_DURATION_STOP =     (1 << 18),
	OPT_CALLEE_TRANSFER =   (1 << 19),
	OPT_CALLER_TRANSFER =   (1 << 20),
	OPT_CALLEE_MONITOR =    (1 << 21),
	OPT_CALLER_MONITOR =    (1 << 22),
	OPT_GOTO =              (1 << 23),
	OPT_OPERMODE =          (1 << 24),
	OPT_CALLEE_PARK =       (1 << 25),
	OPT_CALLER_PARK =       (1 << 26),
	OPT_IGNORE_FORWARDING = (1 << 27),
	OPT_CALLEE_GOSUB =      (1 << 28),
	OPT_CALLEE_MIXMONITOR = (1 << 29),
	OPT_CALLER_MIXMONITOR = (1 << 30),
};

#define VIDEODIAL_STILLGOING      (1 << 31)
#define VIDEODIAL_NOFORWARDHTML   ((uint64_t)1 << 32) /* flags are now 64 bits, so keep it up! */
#define OPT_CANCEL_ELSEWHERE ((uint64_t)1 << 33)
#define OPT_PEER_H           ((uint64_t)1 << 34)
#define OPT_CALLEE_GO_ON     ((uint64_t)1 << 35)

enum {
	OPT_ARG_ANNOUNCE = 0,
	OPT_ARG_SENDDTMF,
	OPT_ARG_GOTO,
	OPT_ARG_DURATION_LIMIT,
	OPT_ARG_MUSICBACK,
	OPT_ARG_CALLEE_MACRO,
	OPT_ARG_CALLEE_GOSUB,
	OPT_ARG_CALLEE_GO_ON,
	OPT_ARG_PRIVACY,
	OPT_ARG_DURATION_STOP,
	OPT_ARG_OPERMODE,
	OPT_ARG_SCREEN_NOINTRO,
	/* note: this entry _MUST_ be the last one in the enum */
	OPT_ARG_ARRAY_SIZE,
};

TRIS_APP_OPTIONS(videodial_exec_options, BEGIN_OPTIONS
	TRIS_APP_OPTION_ARG('A', OPT_ANNOUNCE, OPT_ARG_ANNOUNCE),
	TRIS_APP_OPTION('C', OPT_RESETCDR),
	TRIS_APP_OPTION('c', OPT_CANCEL_ELSEWHERE),
	TRIS_APP_OPTION('d', OPT_DTMF_EXIT),
	TRIS_APP_OPTION_ARG('D', OPT_SENDDTMF, OPT_ARG_SENDDTMF),
	TRIS_APP_OPTION('e', OPT_PEER_H),
	TRIS_APP_OPTION('f', OPT_FORCECLID),
	TRIS_APP_OPTION_ARG('F', OPT_CALLEE_GO_ON, OPT_ARG_CALLEE_GO_ON),
	TRIS_APP_OPTION('g', OPT_GO_ON),
	TRIS_APP_OPTION_ARG('G', OPT_GOTO, OPT_ARG_GOTO),
	TRIS_APP_OPTION('h', OPT_CALLEE_HANGUP),
	TRIS_APP_OPTION('H', OPT_CALLER_HANGUP),
	TRIS_APP_OPTION('i', OPT_IGNORE_FORWARDING),
	TRIS_APP_OPTION('k', OPT_CALLEE_PARK),
	TRIS_APP_OPTION('K', OPT_CALLER_PARK),
	TRIS_APP_OPTION_ARG('L', OPT_DURATION_LIMIT, OPT_ARG_DURATION_LIMIT),
	TRIS_APP_OPTION_ARG('m', OPT_MUSICBACK, OPT_ARG_MUSICBACK),
	TRIS_APP_OPTION_ARG('M', OPT_CALLEE_MACRO, OPT_ARG_CALLEE_MACRO),
	TRIS_APP_OPTION_ARG('n', OPT_SCREEN_NOINTRO, OPT_ARG_SCREEN_NOINTRO),
	TRIS_APP_OPTION('N', OPT_SCREEN_NOCLID),
	TRIS_APP_OPTION('o', OPT_ORIGINAL_CLID),
	TRIS_APP_OPTION_ARG('O', OPT_OPERMODE, OPT_ARG_OPERMODE),
	TRIS_APP_OPTION('p', OPT_SCREENING),
	TRIS_APP_OPTION_ARG('P', OPT_PRIVACY, OPT_ARG_PRIVACY),
	TRIS_APP_OPTION('r', OPT_RINGBACK),
	TRIS_APP_OPTION_ARG('S', OPT_DURATION_STOP, OPT_ARG_DURATION_STOP),
	TRIS_APP_OPTION('t', OPT_CALLEE_TRANSFER),
	TRIS_APP_OPTION('T', OPT_CALLER_TRANSFER),
	TRIS_APP_OPTION_ARG('U', OPT_CALLEE_GOSUB, OPT_ARG_CALLEE_GOSUB),
	TRIS_APP_OPTION('w', OPT_CALLEE_MONITOR),
	TRIS_APP_OPTION('W', OPT_CALLER_MONITOR),
	TRIS_APP_OPTION('x', OPT_CALLEE_MIXMONITOR),
	TRIS_APP_OPTION('X', OPT_CALLER_MIXMONITOR),
END_OPTIONS );

#define CAN_EARLY_BRIDGE(flags,chan,peer) (!tris_test_flag64(flags, OPT_CALLEE_HANGUP | \
	OPT_CALLER_HANGUP | OPT_CALLEE_TRANSFER | OPT_CALLER_TRANSFER | \
	OPT_CALLEE_MONITOR | OPT_CALLER_MONITOR | OPT_CALLEE_PARK |  \
	OPT_CALLER_PARK | OPT_ANNOUNCE | OPT_CALLEE_MACRO | OPT_CALLEE_GOSUB) && \
	!chan->audiohooks && !peer->audiohooks)

/*
 * The list of active channels
 */
struct chanlist {
	struct chanlist *next;
	struct tris_channel *chan;
	uint64_t flags;
};


static void hanguptree(struct chanlist *outgoing, struct tris_channel *exception, int answered_elsewhere)
{
	/* Hang up a tree of stuff */
	struct chanlist *oo;
	while (outgoing) {
		/* Hangup any existing lines we have open */
		if (outgoing->chan && (outgoing->chan != exception)) {
			if (answered_elsewhere) {
				/* The flag is used for local channel inheritance and stuff */
				tris_set_flag(outgoing->chan, TRIS_FLAG_ANSWERED_ELSEWHERE);
				/* This is for the channel drivers */
				outgoing->chan->hangupcause = TRIS_CAUSE_ANSWERED_ELSEWHERE;
			}
			tris_hangup(outgoing->chan);
		}
		oo = outgoing;
		outgoing = outgoing->next;
		tris_free(oo);
	}
}

#define TRIS_MAX_WATCHERS 256

/*
 * argument to handle_cause() and other functions.
 */
struct cause_args {
	struct tris_channel *chan;
	int busy;
	int congestion;
	int nochan;
};

static void handle_cause(int cause, struct cause_args *num)
{
	struct tris_cdr *cdr = num->chan->cdr;

	switch(cause) {
	case TRIS_CAUSE_BUSY:
		if (cdr)
			tris_cdr_busy(cdr);
		num->busy++;
		break;

	case TRIS_CAUSE_CONGESTION:
		if (cdr)
			tris_cdr_failed(cdr);
		num->congestion++;
		break;

	case TRIS_CAUSE_NO_ROUTE_DESTINATION:
	case TRIS_CAUSE_UNREGISTERED:
		if (cdr)
			tris_cdr_failed(cdr);
		num->nochan++;
		break;

	case TRIS_CAUSE_NO_ANSWER:
		if (cdr) {
			tris_cdr_noanswer(cdr);
		}
		break;
	case TRIS_CAUSE_NORMAL_CLEARING:
		break;

	default:
		num->nochan++;
		break;
	}
}

/* free the buffer if allocated, and set the pointer to the second arg */
#define S_REPLACE(s, new_val)		\
	do {				\
		if (s)			\
			tris_free(s);	\
		s = (new_val);		\
	} while (0)

static int onedigit_goto(struct tris_channel *chan, const char *context, char exten, int pri)
{
	char rexten[2] = { exten, '\0' };

	if (context) {
		if (!tris_goto_if_exists(chan, context, rexten, pri))
			return 1;
	} else {
		if (!tris_goto_if_exists(chan, chan->context, rexten, pri))
			return 1;
		else if (!tris_strlen_zero(chan->macrocontext)) {
			if (!tris_goto_if_exists(chan, chan->macrocontext, rexten, pri))
				return 1;
		}
	}
	return 0;
}


static const char *get_cid_name(char *name, int namelen, struct tris_channel *chan)
{
	const char *context = S_OR(chan->macrocontext, chan->context);
	const char *exten = S_OR(chan->macroexten, chan->exten);

	return tris_get_hint(NULL, 0, name, namelen, chan, context, exten) ? name : "";
}

static void sendvideodialevent(struct tris_channel *src, struct tris_channel *dst, const char *videodialstring)
{
	manager_event(EVENT_FLAG_CALL, "Videodial",
		"SubEvent: Begin\r\n"
		"Channel: %s\r\n"
		"Destination: %s\r\n"
		"CallerIDNum: %s\r\n"
		"CallerIDName: %s\r\n"
		"UniqueID: %s\r\n"
		"DestUniqueID: %s\r\n"
		"Videodialstring: %s\r\n",
		src->name, dst->name, S_OR(src->cid.cid_num, "<unknown>"),
		S_OR(src->cid.cid_name, "<unknown>"), src->uniqueid,
		dst->uniqueid, videodialstring ? videodialstring : "");
}

static void sendvideodialendevent(const struct tris_channel *src, const char *videodialstatus)
{
	manager_event(EVENT_FLAG_CALL, "Videodial",
		"SubEvent: End\r\n"
		"Channel: %s\r\n"
		"UniqueID: %s\r\n"
		"VideodialStatus: %s\r\n",
		src->name, src->uniqueid, videodialstatus);
}

/*!
 * helper function for wait_for_answer()
 *
 * XXX this code is highly suspicious, as it essentially overwrites
 * the outgoing channel without properly deleting it.
 */
static void do_forward(struct chanlist *o,
	struct cause_args *num, struct tris_flags64 *peerflags, int single)
{
	char tmpchan[256];
	struct tris_channel *original = o->chan;
	struct tris_channel *c = o->chan; /* the winner */
	struct tris_channel *in = num->chan; /* the input channel */
	char *stuff;
	char *tech;
	int cause;

	tris_copy_string(tmpchan, c->call_forward, sizeof(tmpchan));
	if ((stuff = strchr(tmpchan, '/'))) {
		*stuff++ = '\0';
		tech = tmpchan;
	} else {
		const char *forward_context;
		tris_channel_lock(c);
		forward_context = pbx_builtin_getvar_helper(c, "FORWARD_CONTEXT");
		if (tris_strlen_zero(forward_context)) {
			forward_context = NULL;
		}
		snprintf(tmpchan, sizeof(tmpchan), "%s@%s", c->call_forward, forward_context ? forward_context : c->context);
		tris_channel_unlock(c);
		stuff = tmpchan;
		tech = "Local";
	}
	/* Before processing channel, go ahead and check for forwarding */
	tris_verb(3, "Now forwarding %s to '%s/%s' (thanks to %s)\n", in->name, tech, stuff, c->name);
	/* If we have been told to ignore forwards, just set this channel to null and continue processing extensions normally */
	if (tris_test_flag64(peerflags, OPT_IGNORE_FORWARDING)) {
		tris_verb(3, "Forwarding %s to '%s/%s' prevented.\n", in->name, tech, stuff);
		c = o->chan = NULL;
		cause = TRIS_CAUSE_BUSY;
	} else {
		/* Setup parameters */
		c = o->chan = tris_request(tech, in->nativeformats, stuff, &cause, in);
		if (c) {
			if (single)
				tris_channel_make_compatible(o->chan, in);
			tris_channel_inherit_variables(in, o->chan);
			tris_channel_datastore_inherit(in, o->chan);
		} else
			tris_log(LOG_NOTICE, "Unable to create local channel for call forward to '%s/%s' (cause = %d)\n", tech, stuff, cause);
	}
	if (!c) {
		tris_clear_flag64(o, VIDEODIAL_STILLGOING);
		handle_cause(cause, num);
		tris_hangup(original);
	} else {
		char *new_cid_num, *new_cid_name;
		struct tris_channel *src;

		if (CAN_EARLY_BRIDGE(peerflags, c, in)) {
			tris_rtp_make_compatible(c, in, single);
		}
		if (tris_test_flag64(o, OPT_FORCECLID)) {
			new_cid_num = tris_strdup(S_OR(in->macroexten, in->exten));
			new_cid_name = NULL; /* XXX no name ? */
			src = c; /* XXX possible bug in previous code, which used 'winner' ? it may have changed */
		} else {
			new_cid_num = tris_strdup(in->cid.cid_num);
			new_cid_name = tris_strdup(in->cid.cid_name);
			src = in;
		}
		tris_string_field_set(c, accountcode, src->accountcode);
		c->cdrflags = src->cdrflags;
		S_REPLACE(c->cid.cid_num, new_cid_num);
		S_REPLACE(c->cid.cid_name, new_cid_name);

		if (in->cid.cid_ani) { /* XXX or maybe unconditional ? */
			S_REPLACE(c->cid.cid_ani, tris_strdup(in->cid.cid_ani));
		}
		S_REPLACE(c->cid.cid_rdnis, tris_strdup(S_OR(in->macroexten, in->exten)));
		if (tris_call(c, tmpchan, 0)) {
			tris_log(LOG_NOTICE, "Failed to videodial on local channel for call forward to '%s'\n", tmpchan);
			tris_clear_flag64(o, VIDEODIAL_STILLGOING);
			tris_hangup(original);
			tris_hangup(c);
			c = o->chan = NULL;
			num->nochan++;
		} else {
			sendvideodialevent(in, c, stuff);
			/* After calling, set callerid to extension */
			if (!tris_test_flag64(peerflags, OPT_ORIGINAL_CLID)) {
				char cidname[TRIS_MAX_EXTENSION] = "";
				tris_set_callerid(c, S_OR(in->macroexten, in->exten), get_cid_name(cidname, sizeof(cidname), in), NULL);
			}
			/* Hangup the original channel now, in case we needed it */
			tris_hangup(original);
		}
		if (single) {
			tris_indicate(in, -1);
		}
	}
}

/* argument used for some functions. */
struct privacy_args {
	int sentringing;
	int privdb_val;
	char privcid[256];
	char privintro[1024];
	char status[256];
};

static struct tris_channel *wait_for_answer(struct tris_channel *in,
	struct chanlist *outgoing, int *to, struct tris_flags64 *peerflags,
	struct privacy_args *pa,
	const struct cause_args *num_in, int *result)
{
	struct cause_args num = *num_in;
	int prestart = num.busy + num.congestion + num.nochan;
	int orig = *to;
	struct tris_channel *peer = NULL;
	/* single is set if only one destination is enabled */
	int single = outgoing && !outgoing->next && !tris_test_flag64(outgoing, OPT_MUSICBACK | OPT_RINGBACK);
#ifdef HAVE_EPOLL
	struct chanlist *epollo;
#endif

	if (single) {
		/* Turn off hold music, etc */
		tris_deactivate_generator(in);
		/* If we are calling a single channel, make them compatible for in-band tone purpose */
		tris_channel_make_compatible(outgoing->chan, in);
	}

#ifdef HAVE_EPOLL
	for (epollo = outgoing; epollo; epollo = epollo->next)
		tris_poll_channel_add(in, epollo->chan);
#endif

	while (*to && !peer) {
		struct chanlist *o;
		int pos = 0; /* how many channels do we handle */
		int numlines = prestart;
		struct tris_channel *winner;
		struct tris_channel *watchers[TRIS_MAX_WATCHERS];

		watchers[pos++] = in;
		for (o = outgoing; o; o = o->next) {
			/* Keep track of important channels */
			if (tris_test_flag64(o, VIDEODIAL_STILLGOING) && o->chan)
				watchers[pos++] = o->chan;
			numlines++;
		}
		if (pos == 1) { /* only the input channel is available */
			if (numlines == (num.busy + num.congestion + num.nochan)) {
				tris_verb(2, "Everyone is busy/congested at this time (%d:%d/%d/%d)\n", numlines, num.busy, num.congestion, num.nochan);
				if (num.busy)
					strcpy(pa->status, "BUSY");
				else if (num.congestion)
					strcpy(pa->status, "CONGESTION");
				else if (num.nochan)
					strcpy(pa->status, "CHANUNAVAIL");
			} else {
				tris_verb(3, "No one is available to answer at this time (%d:%d/%d/%d)\n", numlines, num.busy, num.congestion, num.nochan);
			}
			*to = 0;
			return NULL;
		}
		winner = tris_waitfor_n(watchers, pos, to);
		for (o = outgoing; o; o = o->next) {
			struct tris_frame *f;
			struct tris_channel *c = o->chan;

			if (c == NULL)
				continue;
			if (tris_test_flag64(o, VIDEODIAL_STILLGOING) && c->_state == TRIS_STATE_UP) {
				if (!peer) {
					tris_verb(3, "%s answered %s\n", c->name, in->name);
					peer = c;
					tris_copy_flags64(peerflags, o,
						OPT_CALLEE_TRANSFER | OPT_CALLER_TRANSFER |
						OPT_CALLEE_HANGUP | OPT_CALLER_HANGUP |
						OPT_CALLEE_MONITOR | OPT_CALLER_MONITOR |
						OPT_CALLEE_PARK | OPT_CALLER_PARK |
						OPT_CALLEE_MIXMONITOR | OPT_CALLER_MIXMONITOR |
						VIDEODIAL_NOFORWARDHTML);
					tris_string_field_set(c, dialcontext, "");
					tris_copy_string(c->exten, "", sizeof(c->exten));
				}
				continue;
			}
			if (c != winner)
				continue;
			/* here, o->chan == c == winner */
			if (!tris_strlen_zero(c->call_forward)) {
				do_forward(o, &num, peerflags, single);
				continue;
			}
			f = tris_read(winner);
			if (!f) {
				in->hangupcause = c->hangupcause;
#ifdef HAVE_EPOLL
				tris_poll_channel_del(in, c);
#endif
				tris_hangup(c);
				c = o->chan = NULL;
				tris_clear_flag64(o, VIDEODIAL_STILLGOING);
				handle_cause(in->hangupcause, &num);
				continue;
			}
			if (f->frametype == TRIS_FRAME_CONTROL) {
				switch(f->subclass) {
				case TRIS_CONTROL_ANSWER:
					/* This is our guy if someone answered. */
					if (!peer) {
						tris_verb(3, "%s answered %s\n", c->name, in->name);
						peer = c;
						if (peer->cdr) {
							peer->cdr->answer = tris_tvnow();
							peer->cdr->disposition = TRIS_CDR_ANSWERED;
						}
						tris_copy_flags64(peerflags, o,
							OPT_CALLEE_TRANSFER | OPT_CALLER_TRANSFER |
							OPT_CALLEE_HANGUP | OPT_CALLER_HANGUP |
							OPT_CALLEE_MONITOR | OPT_CALLER_MONITOR |
							OPT_CALLEE_PARK | OPT_CALLER_PARK |
							OPT_CALLEE_MIXMONITOR | OPT_CALLER_MIXMONITOR |
							VIDEODIAL_NOFORWARDHTML);
						tris_string_field_set(c, dialcontext, "");
						tris_copy_string(c->exten, "", sizeof(c->exten));
						if (CAN_EARLY_BRIDGE(peerflags, in, peer))
							/* Setup early bridge if appropriate */
							tris_channel_early_bridge(in, peer);
					}
					/* If call has been answered, then the eventual hangup is likely to be normal hangup */
					in->hangupcause = TRIS_CAUSE_NORMAL_CLEARING;
					c->hangupcause = TRIS_CAUSE_NORMAL_CLEARING;
					if(f->datalen > 0 && f->data.ptr){
						char file2play[100];
						strcpy(file2play, f->data.ptr);
						tris_play_and_wait(in, file2play);
					}
					break;
				case TRIS_CONTROL_BUSY:
					tris_verb(3, "%s is busy\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_BUSY, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "BUSY");
					tris_stream_and_wait(in, "dial/pbx-busy", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_CONGESTION:
					tris_verb(3, "%s is circuit-busy\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_CONGESTION, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "CONGEST");
					tris_stream_and_wait(in, "dial/pbx-busy", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_ROUTEFAIL:
					tris_verb(3, "%s is circuit-busy\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_CONGESTION, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "CONGEST");
					tris_stream_and_wait(in, "dial/pbx-busy", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_REJECTED:
					tris_verb(3, "%s is circuit-busy\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_CONGESTION, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "CONGEST");
					tris_stream_and_wait(in, "dial/pbx-busy", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_UNAVAILABLE:
					tris_verb(3, "%s is circuit-busy\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_CONGESTION, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "CONGEST");
					tris_stream_and_wait(in, "dial/pbx-busy", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_FORBIDDEN:
					tris_verb(3, "%s is forbidden\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_CONGESTION, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "FORBIDDEN");
					tris_stream_and_wait(in, "dial/pbx-forbidden", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_OFFHOOK:
					tris_verb(3, "%s is offhook\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_NO_ROUTE_DESTINATION, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "NOTFOUND");
					tris_stream_and_wait(in, "dial/pbx-not-found", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_TAKEOFFHOOK:
					tris_verb(3, "%s is takeoffhook\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_UNREGISTERED, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "UNREGISTERED");
					if(f->datalen > 0 && f->data.ptr){
						char file2play[100];
						strcpy(file2play, f->data.ptr);
						tris_play_and_wait(in, file2play);
					}else{
						tris_stream_and_wait(in, "dial/pbx-not-registered", TRIS_DIGIT_ANY);
					}
					break;
				case TRIS_CONTROL_TIMEOUT:
					tris_verb(3, "%s is timeout\n", c->name);
					in->hangupcause = c->hangupcause;
					tris_hangup(c);
					c = o->chan = NULL;
					tris_clear_flag64(o, VIDEODIAL_STILLGOING);
					handle_cause(TRIS_CAUSE_NO_ANSWER, &num);
					pbx_builtin_setvar_helper(in, "TRANSFERSTATUS", "NOANSWER");
					tris_stream_and_wait(in, "dial/pbx-no-answer", TRIS_DIGIT_ANY);
					break;
				case TRIS_CONTROL_RINGING:
					tris_verb(3, "%s is ringing\n", c->name);
					/* Setup early media if appropriate */
					if (single && CAN_EARLY_BRIDGE(peerflags, in, c))
						tris_channel_early_bridge(in, c);
					if (!(pa->sentringing) && !tris_test_flag64(outgoing, OPT_MUSICBACK)) {
						tris_indicate(in, TRIS_CONTROL_RINGING);
						pa->sentringing++;
					}
					break;
				case TRIS_CONTROL_PROGRESS:
					tris_verb(3, "%s is making progress passing it to %s\n", c->name, in->name);
					/* Setup early media if appropriate */
					if (single && CAN_EARLY_BRIDGE(peerflags, in, c))
						tris_channel_early_bridge(in, c);
					if (!tris_test_flag64(outgoing, OPT_RINGBACK))
						if (single || (!single && !pa->sentringing)) {
							tris_indicate(in, TRIS_CONTROL_PROGRESS);
						}
					break;
				case TRIS_CONTROL_VIDUPDATE:
					tris_verb(3, "%s requested a video update, passing it to %s\n", c->name, in->name);
					tris_indicate(in, TRIS_CONTROL_VIDUPDATE);
					break;
				case TRIS_CONTROL_SRCUPDATE:
					tris_verb(3, "%s requested a source update, passing it to %s\n", c->name, in->name);
					tris_indicate(in, TRIS_CONTROL_SRCUPDATE);
					break;
				case TRIS_CONTROL_PROCEEDING:
					tris_verb(3, "%s is proceeding passing it to %s\n", c->name, in->name);
					if (single && CAN_EARLY_BRIDGE(peerflags, in, c))
						tris_channel_early_bridge(in, c);
					if (!tris_test_flag64(outgoing, OPT_RINGBACK))
						tris_indicate(in, TRIS_CONTROL_PROCEEDING);
					break;
				case TRIS_CONTROL_HOLD:
					tris_verb(3, "Call on %s placed on hold\n", c->name);
					tris_indicate(in, TRIS_CONTROL_HOLD);
					break;
				case TRIS_CONTROL_UNHOLD:
					tris_verb(3, "Call on %s left from hold\n", c->name);
					tris_indicate(in, TRIS_CONTROL_UNHOLD);
					break;
				case TRIS_CONTROL_FLASH:
					/* Ignore going off hook and flash */
					break;
				case -1:
					if (!tris_test_flag64(outgoing, OPT_RINGBACK | OPT_MUSICBACK)) {
						tris_verb(3, "%s stopped sounds\n", c->name);
						tris_indicate(in, -1);
						pa->sentringing = 0;
					}
					break;
				default:
					tris_debug(1, "Dunno what to do with control type %d\n", f->subclass);
				}
			} else if (single) {
				switch (f->frametype) {
					case TRIS_FRAME_VOICE:
					case TRIS_FRAME_IMAGE:
					case TRIS_FRAME_TEXT:
						if (tris_write(in, f)) {
							tris_log(LOG_WARNING, "Unable to write frame\n");
						}
						break;
					case TRIS_FRAME_HTML:
						if (!tris_test_flag64(outgoing, VIDEODIAL_NOFORWARDHTML) && tris_channel_sendhtml(in, f->subclass, f->data.ptr, f->datalen) == -1) {
							tris_log(LOG_WARNING, "Unable to send URL\n");
						}
						break;
					default:
						break;
				}
			}
			tris_frfree(f);
		} /* end for */
		if (winner == in) {
			struct tris_frame *f = tris_read(in);
#if 0
			if (f && (f->frametype != TRIS_FRAME_VOICE))
				printf("Frame type: %d, %d\n", f->frametype, f->subclass);
			else if (!f || (f->frametype != TRIS_FRAME_VOICE))
				printf("Hangup received on %s\n", in->name);
#endif
			if (!f || ((f->frametype == TRIS_FRAME_CONTROL) && (f->subclass == TRIS_CONTROL_HANGUP))) {
				/* Got hung up */
				*to = -1;
				strcpy(pa->status, "CANCEL");
				tris_cdr_noanswer(in->cdr);
				if (f) {
					if (f->data.uint32) {
						in->hangupcause = f->data.uint32;
					}
					tris_frfree(f);
				}
				return NULL;
			}

			/* now f is guaranteed non-NULL */
			if (f->frametype == TRIS_FRAME_DTMF) {
				if (tris_test_flag64(peerflags, OPT_DTMF_EXIT)) {
					const char *context;
					tris_channel_lock(in);
					context = pbx_builtin_getvar_helper(in, "EXITCONTEXT");
					if (onedigit_goto(in, context, (char) f->subclass, 1)) {
						tris_verb(3, "User hit %c to disconnect call.\n", f->subclass);
						*to = 0;
						tris_cdr_noanswer(in->cdr);
						*result = f->subclass;
						strcpy(pa->status, "CANCEL");
						tris_frfree(f);
						tris_channel_unlock(in);
						return NULL;
					}
					tris_channel_unlock(in);
				}

				if (tris_test_flag64(peerflags, OPT_CALLER_HANGUP) &&
						(f->subclass == '*')) { /* hmm it it not guaranteed to be '*' anymore. */
					tris_verb(3, "User hit %c to disconnect call.\n", f->subclass);
					*to = 0;
					strcpy(pa->status, "CANCEL");
					tris_cdr_noanswer(in->cdr);
					tris_frfree(f);
					return NULL;
				}
			}

			/* Forward HTML stuff */
			if (single && (f->frametype == TRIS_FRAME_HTML) && !tris_test_flag64(outgoing, VIDEODIAL_NOFORWARDHTML))
				if (tris_channel_sendhtml(outgoing->chan, f->subclass, f->data.ptr, f->datalen) == -1)
					tris_log(LOG_WARNING, "Unable to send URL\n");

			if (single && ((f->frametype == TRIS_FRAME_VOICE) || (f->frametype == TRIS_FRAME_DTMF_BEGIN) || (f->frametype == TRIS_FRAME_DTMF_END)))  {
				if (tris_write(outgoing->chan, f))
					tris_log(LOG_WARNING, "Unable to forward voice or dtmf\n");
			}
			if (single && (f->frametype == TRIS_FRAME_CONTROL) &&
				((f->subclass == TRIS_CONTROL_HOLD) ||
				(f->subclass == TRIS_CONTROL_UNHOLD) ||
				(f->subclass == TRIS_CONTROL_VIDUPDATE) ||
				 (f->subclass == TRIS_CONTROL_SRCUPDATE))) {
				tris_verb(3, "%s requested special control %d, passing it to %s\n", in->name, f->subclass, outgoing->chan->name);
				tris_indicate_data(outgoing->chan, f->subclass, f->data.ptr, f->datalen);
			}
			tris_frfree(f);
		}
		if (!*to)
			tris_verb(3, "Nobody picked up in %d ms\n", orig);
		if (!*to || tris_check_hangup(in))
			tris_cdr_noanswer(in->cdr);
	}

#ifdef HAVE_EPOLL
	for (epollo = outgoing; epollo; epollo = epollo->next) {
		if (epollo->chan)
			tris_poll_channel_del(in, epollo->chan);
	}
#endif

	return peer;
}

static void replace_macro_delimiter(char *s)
{
	for (; *s; s++)
		if (*s == '^')
			*s = ',';
}

/* returns true if there is a valid privacy reply */
static int valid_priv_reply(struct tris_flags64 *opts, int res)
{
	if (res < '1')
		return 0;
	if (tris_test_flag64(opts, OPT_PRIVACY) && res <= '5')
		return 1;
	if (tris_test_flag64(opts, OPT_SCREENING) && res <= '4')
		return 1;
	return 0;
}

static int do_timelimit(struct tris_channel *chan, struct tris_bridge_config *config,
	char *parse, struct timeval *calldurationlimit)
{
	char *stringp = tris_strdupa(parse);
	char *limit_str, *warning_str, *warnfreq_str;
	const char *var;
	int play_to_caller = 0, play_to_callee = 0;
	int delta;

	limit_str = strsep(&stringp, ":");
	warning_str = strsep(&stringp, ":");
	warnfreq_str = strsep(&stringp, ":");

	config->timelimit = atol(limit_str);
	if (warning_str)
		config->play_warning = atol(warning_str);
	if (warnfreq_str)
		config->warning_freq = atol(warnfreq_str);

	if (!config->timelimit) {
		tris_log(LOG_WARNING, "Videodial does not accept L(%s), hanging up.\n", limit_str);
		config->timelimit = config->play_warning = config->warning_freq = 0;
		config->warning_sound = NULL;
		return -1; /* error */
	} else if ( (delta = config->play_warning - config->timelimit) > 0) {
		int w = config->warning_freq;

		/* If the first warning is requested _after_ the entire call would end,
		   and no warning frequency is requested, then turn off the warning. If
		   a warning frequency is requested, reduce the 'first warning' time by
		   that frequency until it falls within the call's total time limit.
		   Graphically:
				  timelim->|    delta        |<-playwarning
			0__________________|_________________|
					 | w  |    |    |    |

		   so the number of intervals to cut is 1+(delta-1)/w
		*/

		if (w == 0) {
			config->play_warning = 0;
		} else {
			config->play_warning -= w * ( 1 + (delta-1)/w );
			if (config->play_warning < 1)
				config->play_warning = config->warning_freq = 0;
		}
	}
	
	tris_channel_lock(chan);

	var = pbx_builtin_getvar_helper(chan, "LIMIT_PLAYAUDIO_CALLER");

	play_to_caller = var ? tris_true(var) : 1;

	var = pbx_builtin_getvar_helper(chan, "LIMIT_PLAYAUDIO_CALLEE");
	play_to_callee = var ? tris_true(var) : 0;

	if (!play_to_caller && !play_to_callee)
		play_to_caller = 1;

	var = pbx_builtin_getvar_helper(chan, "LIMIT_WARNING_FILE");
	config->warning_sound = !tris_strlen_zero(var) ? tris_strdup(var) : tris_strdup("timeleft");

	/* The code looking at config wants a NULL, not just "", to decide
	 * that the message should not be played, so we replace "" with NULL.
	 * Note, pbx_builtin_getvar_helper _can_ return NULL if the variable is
	 * not found.
	 */

	var = pbx_builtin_getvar_helper(chan, "LIMIT_TIMEOUT_FILE");
	config->end_sound = !tris_strlen_zero(var) ? tris_strdup(var) : NULL;

	var = pbx_builtin_getvar_helper(chan, "LIMIT_CONNECT_FILE");
	config->start_sound = !tris_strlen_zero(var) ? tris_strdup(var) : NULL;

	tris_channel_unlock(chan);

	/* undo effect of S(x) in case they are both used */
	calldurationlimit->tv_sec = 0;
	calldurationlimit->tv_usec = 0;

	/* more efficient to do it like S(x) does since no advanced opts */
	if (!config->play_warning && !config->start_sound && !config->end_sound && config->timelimit) {
		calldurationlimit->tv_sec = config->timelimit / 1000;
		calldurationlimit->tv_usec = (config->timelimit % 1000) * 1000;
		tris_verb(3, "Setting call duration limit to %.3lf seconds.\n",
			calldurationlimit->tv_sec + calldurationlimit->tv_usec / 1000000.0);
		config->timelimit = play_to_caller = play_to_callee =
		config->play_warning = config->warning_freq = 0;
	} else {
		tris_verb(3, "Limit Data for this call:\n");
		tris_verb(4, "timelimit      = %ld\n", config->timelimit);
		tris_verb(4, "play_warning   = %ld\n", config->play_warning);
		tris_verb(4, "play_to_caller = %s\n", play_to_caller ? "yes" : "no");
		tris_verb(4, "play_to_callee = %s\n", play_to_callee ? "yes" : "no");
		tris_verb(4, "warning_freq   = %ld\n", config->warning_freq);
		tris_verb(4, "start_sound    = %s\n", S_OR(config->start_sound, ""));
		tris_verb(4, "warning_sound  = %s\n", config->warning_sound);
		tris_verb(4, "end_sound      = %s\n", S_OR(config->end_sound, ""));
	}
	if (play_to_caller)
		tris_set_flag(&(config->features_caller), TRIS_FEATURE_PLAY_WARNING);
	if (play_to_callee)
		tris_set_flag(&(config->features_callee), TRIS_FEATURE_PLAY_WARNING);
	return 0;
}

static int do_privacy(struct tris_channel *chan, struct tris_channel *peer,
	struct tris_flags64 *opts, char **opt_args, struct privacy_args *pa)
{

	int res2;
	int loopcount = 0;

	/* Get the user's intro, store it in priv-callerintros/$CID,
	   unless it is already there-- this should be done before the
	   call is actually videodialed  */

	/* all ring indications and moh for the caller has been halted as soon as the
	   target extension was picked up. We are going to have to kill some
	   time and make the caller believe the peer hasn't picked up yet */

	if (tris_test_flag64(opts, OPT_MUSICBACK) && !tris_strlen_zero(opt_args[OPT_ARG_MUSICBACK])) {
		char *original_moh = tris_strdupa(chan->musicclass);
		tris_indicate(chan, -1);
		tris_string_field_set(chan, musicclass, opt_args[OPT_ARG_MUSICBACK]);
		tris_moh_start(chan, opt_args[OPT_ARG_MUSICBACK], NULL);
		tris_string_field_set(chan, musicclass, original_moh);
	} else if (tris_test_flag64(opts, OPT_RINGBACK)) {
		tris_indicate(chan, TRIS_CONTROL_RINGING);
		pa->sentringing++;
	}

	/* Start autoservice on the other chan ?? */
	res2 = tris_autoservice_start(chan);
	/* Now Stream the File */
	for (loopcount = 0; loopcount < 3; loopcount++) {
		if (res2 && loopcount == 0) /* error in tris_autoservice_start() */
			break;
		if (!res2) /* on timeout, play the message again */
			res2 = tris_play_and_wait(peer, "priv-callpending");
		if (!valid_priv_reply(opts, res2))
			res2 = 0;
		/* priv-callpending script:
		   "I have a caller waiting, who introduces themselves as:"
		*/
		if (!res2)
			res2 = tris_play_and_wait(peer, pa->privintro);
		if (!valid_priv_reply(opts, res2))
			res2 = 0;
		/* now get input from the called party, as to their choice */
		if (!res2) {
			/* XXX can we have both, or they are mutually exclusive ? */
			if (tris_test_flag64(opts, OPT_PRIVACY))
				res2 = tris_play_and_wait(peer, "priv-callee-options");
			if (tris_test_flag64(opts, OPT_SCREENING))
				res2 = tris_play_and_wait(peer, "screen-callee-options");
		}
		/*! \page VideodialPrivacy Videodial Privacy scripts
		\par priv-callee-options script:
			"Videodial 1 if you wish this caller to reach you directly in the future,
				and immediately connect to their incoming call
			 Videodial 2 if you wish to send this caller to voicemail now and
				forevermore.
			 Videodial 3 to send this caller to the torture menus, now and forevermore.
			 Videodial 4 to send this caller to a simple "go away" menu, now and forevermore.
			 Videodial 5 to allow this caller to come straight thru to you in the future,
				but right now, just this once, send them to voicemail."
		\par screen-callee-options script:
			"Videodial 1 if you wish to immediately connect to the incoming call
			 Videodial 2 if you wish to send this caller to voicemail.
			 Videodial 3 to send this caller to the torture menus.
			 Videodial 4 to send this caller to a simple "go away" menu.
		*/
		if (valid_priv_reply(opts, res2))
			break;
		/* invalid option */
		res2 = tris_play_and_wait(peer, "voicemail/vm-sorry");
	}

	if (tris_test_flag64(opts, OPT_MUSICBACK)) {
		tris_moh_stop(chan);
	} else if (tris_test_flag64(opts, OPT_RINGBACK)) {
		tris_indicate(chan, -1);
		pa->sentringing = 0;
	}
	tris_autoservice_stop(chan);
	if (tris_test_flag64(opts, OPT_PRIVACY) && (res2 >= '1' && res2 <= '5')) {
		/* map keypresses to various things, the index is res2 - '1' */
		static const char *_val[] = { "ALLOW", "DENY", "TORTURE", "KILL", "ALLOW" };
		static const int _flag[] = { TRIS_PRIVACY_ALLOW, TRIS_PRIVACY_DENY, TRIS_PRIVACY_TORTURE, TRIS_PRIVACY_KILL, TRIS_PRIVACY_ALLOW};
		int i = res2 - '1';
		tris_verb(3, "--Set privacy database entry %s/%s to %s\n",
			opt_args[OPT_ARG_PRIVACY], pa->privcid, _val[i]);
		tris_privacy_set(opt_args[OPT_ARG_PRIVACY], pa->privcid, _flag[i]);
	}
	switch (res2) {
	case '1':
		break;
	case '2':
		tris_copy_string(pa->status, "NOANSWER", sizeof(pa->status));
		break;
	case '3':
		tris_copy_string(pa->status, "TORTURE", sizeof(pa->status));
		break;
	case '4':
		tris_copy_string(pa->status, "DONTCALL", sizeof(pa->status));
		break;
	case '5':
		/* XXX should we set status to DENY ? */
		if (tris_test_flag64(opts, OPT_PRIVACY))
			break;
		/* if not privacy, then 5 is the same as "default" case */
	default: /* bad input or -1 if failure to start autoservice */
		/* well, if the user messes up, ... he had his chance... What Is The Best Thing To Do?  */
		/* well, there seems basically two choices. Just patch the caller thru immediately,
			  or,... put 'em thru to voicemail. */
		/* since the callee may have hung up, let's do the voicemail thing, no database decision */
		tris_log(LOG_NOTICE, "privacy: no valid response from the callee. Sending the caller to voicemail, the callee isn't responding\n");
		/* XXX should we set status to DENY ? */
		/* XXX what about the privacy flags ? */
		break;
	}

	if (res2 == '1') { /* the only case where we actually connect */
		/* if the intro is NOCALLERID, then there's no reason to leave it on disk, it'll
		   just clog things up, and it's not useful information, not being tied to a CID */
		if (strncmp(pa->privcid, "NOCALLERID", 10) == 0 || tris_test_flag64(opts, OPT_SCREEN_NOINTRO)) {
			tris_filedelete(pa->privintro, NULL);
			if (tris_fileexists(pa->privintro, NULL, NULL) > 0)
				tris_log(LOG_NOTICE, "privacy: tris_filedelete didn't do its job on %s\n", pa->privintro);
			else
				tris_verb(3, "Successfully deleted %s intro file\n", pa->privintro);
		}
		return 0; /* the good exit path */
	} else {
		tris_hangup(peer); /* hang up on the callee -- he didn't want to talk anyway! */
		return -1;
	}
}

/*! \brief returns 1 if successful, 0 or <0 if the caller should 'goto out' */
static int setup_privacy_args(struct privacy_args *pa,
	struct tris_flags64 *opts, char *opt_args[], struct tris_channel *chan)
{
	char callerid[60];
	int res;
	char *l;
	int silencethreshold;

	if (!tris_strlen_zero(chan->cid.cid_num)) {
		l = tris_strdupa(chan->cid.cid_num);
		tris_shrink_phone_number(l);
		if (tris_test_flag64(opts, OPT_PRIVACY) ) {
			tris_verb(3, "Privacy DB is '%s', clid is '%s'\n", opt_args[OPT_ARG_PRIVACY], l);
			pa->privdb_val = tris_privacy_check(opt_args[OPT_ARG_PRIVACY], l);
		} else {
			tris_verb(3, "Privacy Screening, clid is '%s'\n", l);
			pa->privdb_val = TRIS_PRIVACY_UNKNOWN;
		}
	} else {
		char *tnam, *tn2;

		tnam = tris_strdupa(chan->name);
		/* clean the channel name so slashes don't try to end up in disk file name */
		for (tn2 = tnam; *tn2; tn2++) {
			if (*tn2 == '/')  /* any other chars to be afraid of? */
				*tn2 = '=';
		}
		tris_verb(3, "Privacy-- callerid is empty\n");

		snprintf(callerid, sizeof(callerid), "NOCALLERID_%s%s", chan->exten, tnam);
		l = callerid;
		pa->privdb_val = TRIS_PRIVACY_UNKNOWN;
	}

	tris_copy_string(pa->privcid, l, sizeof(pa->privcid));

	if (strncmp(pa->privcid, "NOCALLERID", 10) != 0 && tris_test_flag64(opts, OPT_SCREEN_NOCLID)) {
		/* if callerid is set and OPT_SCREEN_NOCLID is set also */
		tris_verb(3, "CallerID set (%s); N option set; Screening should be off\n", pa->privcid);
		pa->privdb_val = TRIS_PRIVACY_ALLOW;
	} else if (tris_test_flag64(opts, OPT_SCREEN_NOCLID) && strncmp(pa->privcid, "NOCALLERID", 10) == 0) {
		tris_verb(3, "CallerID blank; N option set; Screening should happen; dbval is %d\n", pa->privdb_val);
	}
	
	if (pa->privdb_val == TRIS_PRIVACY_DENY) {
		tris_verb(3, "Privacy DB reports PRIVACY_DENY for this callerid. Videodial reports unavailable\n");
		tris_copy_string(pa->status, "NOANSWER", sizeof(pa->status));
		return 0;
	} else if (pa->privdb_val == TRIS_PRIVACY_KILL) {
		tris_copy_string(pa->status, "DONTCALL", sizeof(pa->status));
		return 0; /* Is this right? */
	} else if (pa->privdb_val == TRIS_PRIVACY_TORTURE) {
		tris_copy_string(pa->status, "TORTURE", sizeof(pa->status));
		return 0; /* is this right??? */
	} else if (pa->privdb_val == TRIS_PRIVACY_UNKNOWN) {
		/* Get the user's intro, store it in priv-callerintros/$CID,
		   unless it is already there-- this should be done before the
		   call is actually videodialed  */

		/* make sure the priv-callerintros dir actually exists */
		snprintf(pa->privintro, sizeof(pa->privintro), "%s/sounds/priv-callerintros", tris_config_TRIS_DATA_DIR);
		if ((res = tris_mkdir(pa->privintro, 0755))) {
			tris_log(LOG_WARNING, "privacy: can't create directory priv-callerintros: %s\n", strerror(res));
			return -1;
		}

		snprintf(pa->privintro, sizeof(pa->privintro), "priv-callerintros/%s", pa->privcid);
		if (tris_fileexists(pa->privintro, NULL, NULL ) > 0 && strncmp(pa->privcid, "NOCALLERID", 10) != 0) {
			/* the DELUX version of this code would allow this caller the
			   option to hear and retape their previously recorded intro.
			*/
		} else {
			int duration; /* for feedback from play_and_wait */
			/* the file doesn't exist yet. Let the caller submit his
			   vocal intro for posterity */
			/* priv-recordintro script:

			   "At the tone, please say your name:"

			*/
			silencethreshold = tris_dsp_get_threshold_from_settings(THRESHOLD_SILENCE);
			tris_answer(chan);
			res = tris_play_and_record(chan, "priv-recordintro", pa->privintro, 4, "gsm", &duration, silencethreshold, 2000, 0);  /* NOTE: I've reduced the total time to 4 sec */
									/* don't think we'll need a lock removed, we took care of
									   conflicts by naming the pa.privintro file */
			if (res == -1) {
				/* Delete the file regardless since they hung up during recording */
				tris_filedelete(pa->privintro, NULL);
				if (tris_fileexists(pa->privintro, NULL, NULL) > 0)
					tris_log(LOG_NOTICE, "privacy: tris_filedelete didn't do its job on %s\n", pa->privintro);
				else
					tris_verb(3, "Successfully deleted %s intro file\n", pa->privintro);
				return -1;
			}
			if (!tris_streamfile(chan, "voicemail/vm-videodialout", chan->language) )
				tris_waitstream(chan, "");
		}
	}
	return 1; /* success */
}

static void end_bridge_callback(void *data)
{
	char buf[80];
	time_t end;
	struct tris_channel *chan = data;

	if (!chan->cdr) {
		return;
	}

	time(&end);

	tris_channel_lock(chan);
	if (chan->cdr->answer.tv_sec) {
		snprintf(buf, sizeof(buf), "%ld", end - chan->cdr->answer.tv_sec);
		pbx_builtin_setvar_helper(chan, "ANSWEREDTIME", buf);
	}

	if (chan->cdr->start.tv_sec) {
		snprintf(buf, sizeof(buf), "%ld", end - chan->cdr->start.tv_sec);
		pbx_builtin_setvar_helper(chan, "VIDEODIALEDTIME", buf);
	}
	tris_channel_unlock(chan);
}

static void end_bridge_callback_data_fixup(struct tris_bridge_config *bconfig, struct tris_channel *originator, struct tris_channel *terminator) {
	bconfig->end_bridge_callback_data = originator;
}

static int get_monitor_fn(struct tris_channel * chan, char *s, int len)
{
	struct tris_tm tm;
	struct timeval t = tris_tvnow();
	
	tris_localtime(&t, &tm, NULL);
	return snprintf(s, len, "%04d%02d%02d-%02d%02d%02d-%s-%s", 
		tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, 
		tm.tm_sec, S_OR(chan->cid.cid_num, ""), S_OR(chan->exten,""));
}

static int check_mark(struct tris_channel *chan)
{
	char result[80], sql[256];

	snprintf(sql, sizeof(sql), "SELECT user_info.extension FROM user_info left join uri on user_info.uid=uri.uid WHERE uri.username = '%s' AND user_info.tapstart = 1", chan->cid.cid_num);
	sql_select_query_execute(result, sql);
	if (!tris_strlen_zero(result))
		return 1;

	snprintf(sql, sizeof(sql), "SELECT user_info.extension FROM user_info left join uri on user_info.uid=uri.uid WHERE uri.username = '%s' AND user_info.tapstart = 1", chan->exten);
	sql_select_query_execute(result, sql);
	if (!tris_strlen_zero(result))
		return 1;

	snprintf(sql, sizeof(sql), "SELECT pattern FROM mark_pattern WHERE pattern = '%s'", chan->cid.cid_num);
	sql_select_query_execute(result, sql);
	if (!tris_strlen_zero(result))
		return 1;

	snprintf(sql, sizeof(sql), "SELECT pattern FROM mark_pattern WHERE pattern = '%s'", chan->exten);
	sql_select_query_execute(result, sql);
	if (!tris_strlen_zero(result))
		return 1;

	return 0;
}

static void exec_monitor(struct tris_channel *chan) 
{
	char mfn[256], args[256];
	struct tris_app *tris_app = pbx_findapp("Monitor");

	/* If the application was not found, return immediately */
	if (!tris_app)
		return;

	if (!check_mark(chan))
		return;
	
	get_monitor_fn(chan, mfn, sizeof(mfn));
	snprintf(args, sizeof(args), ",%s,m", mfn);
	/* All is well... execute the application */
	pbx_exec(chan, tris_app, args);
}

static int videodial_exec_full(struct tris_channel *chan, void *data, struct tris_flags64 *peerflags, int *continue_exec)
{
	int res = -1; /* default: error */
	char *rest, *cur; /* scan the list of destinations */
	struct chanlist *outgoing = NULL; /* list of destinations */
	struct tris_channel *peer;
	int to; /* timeout */
	struct cause_args num = { chan, 0, 0, 0 };
	int cause;
	char numsubst[256];
	char cidname[TRIS_MAX_EXTENSION] = "";

	struct tris_bridge_config config = { { 0, } };
	struct timeval calldurationlimit = { 0, };
	char *dtmfcalled = NULL, *dtmfcalling = NULL;
	struct privacy_args pa = {
		.sentringing = 0,
		.privdb_val = 0,
		.status = "INVALIDARGS",
	};
	int sentringing = 0, moh = 0;
	const char *outbound_group = NULL;
	int result = 0;
	char *parse;
	int opermode = 0;
	int delprivintro = 0;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(peers);
		TRIS_APP_ARG(timeout);
		TRIS_APP_ARG(options);
		TRIS_APP_ARG(url);
	);
	struct tris_flags64 opts = { 0, };
	char *opt_args[OPT_ARG_ARRAY_SIZE];
	struct tris_datastore *datastore = NULL;
	int fullvideodial = 0, num_videodialed = 0;

	/* Reset all VIDEODIAL variables back to blank, to prevent confusion (in case we don't reset all of them). */
	pbx_builtin_setvar_helper(chan, "VIDEODIALSTATUS", "");
	pbx_builtin_setvar_helper(chan, "VIDEODIALEDPEERNUMBER", "");
	pbx_builtin_setvar_helper(chan, "VIDEODIALEDPEERNAME", "");
	pbx_builtin_setvar_helper(chan, "ANSWEREDTIME", "");
	pbx_builtin_setvar_helper(chan, "VIDEODIALEDTIME", "");

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "Videodial requires an argument (technology/number)\n");
		pbx_builtin_setvar_helper(chan, "VIDEODIALSTATUS", pa.status);
		return -1;
	}

	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (!tris_strlen_zero(args.options) &&
		tris_app_parse_options64(videodial_exec_options, &opts, opt_args, args.options)) {
		pbx_builtin_setvar_helper(chan, "VIDEODIALSTATUS", pa.status);
		goto done;
	}

	if (tris_strlen_zero(args.peers)) {
		tris_log(LOG_WARNING, "Videodial requires an argument (technology/number)\n");
		pbx_builtin_setvar_helper(chan, "VIDEODIALSTATUS", pa.status);
		goto done;
	}


	if (tris_test_flag64(&opts, OPT_SCREEN_NOINTRO) && !tris_strlen_zero(opt_args[OPT_ARG_SCREEN_NOINTRO])) {
		delprivintro = atoi(opt_args[OPT_ARG_SCREEN_NOINTRO]);

		if (delprivintro < 0 || delprivintro > 1) {
			tris_log(LOG_WARNING, "Unknown argument %d specified to n option, ignoring\n", delprivintro);
			delprivintro = 0;
		}
	}

	if (tris_test_flag64(&opts, OPT_OPERMODE)) {
		opermode = tris_strlen_zero(opt_args[OPT_ARG_OPERMODE]) ? 1 : atoi(opt_args[OPT_ARG_OPERMODE]);
		tris_verb(3, "Setting operator services mode to %d.\n", opermode);
	}
	
	if (tris_test_flag64(&opts, OPT_DURATION_STOP) && !tris_strlen_zero(opt_args[OPT_ARG_DURATION_STOP])) {
		calldurationlimit.tv_sec = atoi(opt_args[OPT_ARG_DURATION_STOP]);
		if (!calldurationlimit.tv_sec) {
			tris_log(LOG_WARNING, "Videodial does not accept S(%s), hanging up.\n", opt_args[OPT_ARG_DURATION_STOP]);
			pbx_builtin_setvar_helper(chan, "VIDEODIALSTATUS", pa.status);
			goto done;
		}
		tris_verb(3, "Setting call duration limit to %.3lf seconds.\n", calldurationlimit.tv_sec + calldurationlimit.tv_usec / 1000000.0);
	}

	if (tris_test_flag64(&opts, OPT_SENDDTMF) && !tris_strlen_zero(opt_args[OPT_ARG_SENDDTMF])) {
		dtmfcalling = opt_args[OPT_ARG_SENDDTMF];
		dtmfcalled = strsep(&dtmfcalling, ":");
	}

	if (tris_test_flag64(&opts, OPT_DURATION_LIMIT) && !tris_strlen_zero(opt_args[OPT_ARG_DURATION_LIMIT])) {
		if (do_timelimit(chan, &config, opt_args[OPT_ARG_DURATION_LIMIT], &calldurationlimit))
			goto done;
	}

	if (tris_test_flag64(&opts, OPT_RESETCDR) && chan->cdr)
		tris_cdr_reset(chan->cdr, NULL);
	if (tris_test_flag64(&opts, OPT_PRIVACY) && tris_strlen_zero(opt_args[OPT_ARG_PRIVACY]))
		opt_args[OPT_ARG_PRIVACY] = tris_strdupa(chan->exten);

	if (tris_test_flag64(&opts, OPT_PRIVACY) || tris_test_flag64(&opts, OPT_SCREENING)) {
		res = setup_privacy_args(&pa, &opts, opt_args, chan);
		if (res <= 0)
			goto out;
		res = -1; /* reset default */
	}

	if (tris_test_flag64(&opts, OPT_DTMF_EXIT)) {
		__tris_answer(chan, 0, 0);
	}

	if (continue_exec)
		*continue_exec = 0;

	/* If a channel group has been specified, get it for use when we create peer channels */

	tris_channel_lock(chan);
	if ((outbound_group = pbx_builtin_getvar_helper(chan, "OUTBOUND_GROUP_ONCE"))) {
		outbound_group = tris_strdupa(outbound_group);	
		pbx_builtin_setvar_helper(chan, "OUTBOUND_GROUP_ONCE", NULL);
	} else if ((outbound_group = pbx_builtin_getvar_helper(chan, "OUTBOUND_GROUP"))) {
		outbound_group = tris_strdupa(outbound_group);
	}
	tris_channel_unlock(chan);	
	tris_copy_flags64(peerflags, &opts, OPT_DTMF_EXIT | OPT_GO_ON | OPT_ORIGINAL_CLID | OPT_CALLER_HANGUP | OPT_IGNORE_FORWARDING | OPT_ANNOUNCE | OPT_CALLEE_MACRO | OPT_CALLEE_GOSUB);

	/* loop through the list of videodial destinations */
	rest = args.peers;
	while ((cur = strsep(&rest, "&")) ) {
		struct chanlist *tmp;
		struct tris_channel *tc; /* channel for this destination */
		/* Get a technology/[device:]number pair */
		char *number = cur;
		char *interface = tris_strdupa(number);
		char *tech = strsep(&number, "/");
		/* find if we already videodialed this interface */
		struct tris_dialed_interface *di;
		TRIS_LIST_HEAD(, tris_dialed_interface) *videodialed_interfaces;
		num_videodialed++;
		if (!number) {
			tris_log(LOG_WARNING, "Videodial argument takes format (technology/[device:]number1)\n");
			goto out;
		}
		if (!(tmp = tris_calloc(1, sizeof(*tmp))))
			goto out;
		if (opts.flags) {
			tris_copy_flags64(tmp, &opts,
				OPT_CANCEL_ELSEWHERE |
				OPT_CALLEE_TRANSFER | OPT_CALLER_TRANSFER |
				OPT_CALLEE_HANGUP | OPT_CALLER_HANGUP |
				OPT_CALLEE_MONITOR | OPT_CALLER_MONITOR |
				OPT_CALLEE_PARK | OPT_CALLER_PARK |
				OPT_CALLEE_MIXMONITOR | OPT_CALLER_MIXMONITOR |
				OPT_RINGBACK | OPT_MUSICBACK | OPT_FORCECLID);
			tris_set2_flag64(tmp, args.url, VIDEODIAL_NOFORWARDHTML);
		}
		tris_copy_string(numsubst, number, sizeof(numsubst));
		/* Request the peer */

		tris_channel_lock(chan);
		datastore = tris_channel_datastore_find(chan, &dialed_interface_info, NULL);
		tris_channel_unlock(chan);

		if (datastore)
			videodialed_interfaces = datastore->data;
		else {
			if (!(datastore = tris_datastore_alloc(&dialed_interface_info, NULL))) {
				tris_log(LOG_WARNING, "Unable to create channel datastore for videodialed interfaces. Aborting!\n");
				tris_free(tmp);
				goto out;
			}

			datastore->inheritance = DATASTORE_INHERIT_FOREVER;

			if (!(videodialed_interfaces = tris_calloc(1, sizeof(*videodialed_interfaces)))) {
				tris_datastore_free(datastore);
				tris_free(tmp);
				goto out;
			}

			datastore->data = videodialed_interfaces;
			TRIS_LIST_HEAD_INIT(videodialed_interfaces);

			tris_channel_lock(chan);
			tris_channel_datastore_add(chan, datastore);
			tris_channel_unlock(chan);
		}

		TRIS_LIST_LOCK(videodialed_interfaces);
		TRIS_LIST_TRAVERSE(videodialed_interfaces, di, list) {
			if (!strcasecmp(di->interface, interface)) {
				tris_log(LOG_WARNING, "Skipping videodialing interface '%s' again since it has already been videodialed\n",
					di->interface);
				break;
			}
		}
		TRIS_LIST_UNLOCK(videodialed_interfaces);

		if (di) {
			fullvideodial++;
			tris_free(tmp);
			continue;
		}

		/* It is always ok to videodial a Local interface.  We only keep track of
		 * which "real" interfaces have been videodialed.  The Local channel will
		 * inherit this list so that if it ends up videodialing a real interface,
		 * it won't call one that has already been called. */
		if (strcasecmp(tech, "Local")) {
			if (!(di = tris_calloc(1, sizeof(*di) + strlen(interface)))) {
				TRIS_LIST_UNLOCK(videodialed_interfaces);
				tris_free(tmp);
				goto out;
			}
			strcpy(di->interface, interface);

			TRIS_LIST_LOCK(videodialed_interfaces);
			TRIS_LIST_INSERT_TAIL(videodialed_interfaces, di, list);
			TRIS_LIST_UNLOCK(videodialed_interfaces);
		}

		tc = tris_request(tech, chan->nativeformats, numsubst, &cause, chan);
		if (!tc) {
			/* If we can't, just go on to the next call */
			tris_log(LOG_WARNING, "Unable to create channel of type '%s' (cause %d - %s)\n",
				tech, cause, tris_cause2str(cause));
			handle_cause(cause, &num);
			if (!rest) /* we are on the last destination */
				chan->hangupcause = cause;
			tris_free(tmp);
			continue;
		}
		pbx_builtin_setvar_helper(tc, "VIDEODIALEDPEERNUMBER", numsubst);

		/* Setup outgoing SDP to match incoming one */
		if (CAN_EARLY_BRIDGE(peerflags, chan, tc)) {
			tris_rtp_make_compatible(tc, chan, !outgoing && !rest);
		}
		
		/* Inherit specially named variables from parent channel */
		tris_channel_inherit_variables(chan, tc);
		tris_channel_datastore_inherit(chan, tc);

		tc->appl = "AppVideodial";
		tc->data = "(Outgoing Line)";
		memset(&tc->whentohangup, 0, sizeof(tc->whentohangup));

		S_REPLACE(tc->cid.cid_num, tris_strdup(chan->cid.cid_num));
		S_REPLACE(tc->cid.cid_name, tris_strdup(chan->cid.cid_name));
		S_REPLACE(tc->cid.cid_ani, tris_strdup(chan->cid.cid_ani));
		S_REPLACE(tc->cid.cid_rdnis, tris_strdup(chan->cid.cid_rdnis));
		
		tris_string_field_set(tc, accountcode, chan->accountcode);
		tc->cdrflags = chan->cdrflags;
		if (tris_strlen_zero(tc->musicclass))
			tris_string_field_set(tc, musicclass, chan->musicclass);
		/* Pass callingpres, type of number, tns, ADSI CPE, transfer capability */
		tc->cid.cid_pres = chan->cid.cid_pres;
		tc->cid.cid_ton = chan->cid.cid_ton;
		tc->cid.cid_tns = chan->cid.cid_tns;
		tc->cid.cid_ani2 = chan->cid.cid_ani2;
		tc->adsicpe = chan->adsicpe;
		tc->transfercapability = chan->transfercapability;

		/* If we have an outbound group, set this peer channel to it */
		if (outbound_group)
			tris_app_group_set_channel(tc, outbound_group);
		/* If the calling channel has the ANSWERED_ELSEWHERE flag set, inherit it. This is to support local channels */
		if (tris_test_flag(chan, TRIS_FLAG_ANSWERED_ELSEWHERE))
			tris_set_flag(tc, TRIS_FLAG_ANSWERED_ELSEWHERE);

		/* Check if we're forced by configuration */
		if (tris_test_flag64(&opts, OPT_CANCEL_ELSEWHERE))
			 tris_set_flag(tc, TRIS_FLAG_ANSWERED_ELSEWHERE);


		/* Inherit context and extension */
		tris_string_field_set(tc, dialcontext, tris_strlen_zero(chan->macrocontext) ? chan->context : chan->macrocontext);
		if (!tris_strlen_zero(chan->macroexten))
			tris_copy_string(tc->exten, chan->macroexten, sizeof(tc->exten));
		else
			tris_copy_string(tc->exten, chan->exten, sizeof(tc->exten));

		res = tris_call(tc, numsubst, 0); /* Place the call, but don't wait on the answer */

		/* Save the info in cdr's that we called them */
		if (chan->cdr)
			tris_cdr_setdestchan(chan->cdr, tc->name);

		/* check the results of tris_call */
		if (res) {
			/* Again, keep going even if there's an error */
			tris_debug(1, "ast call on peer returned %d\n", res);
			tris_verb(3, "Couldn't call %s\n", numsubst);
			if (tc->hangupcause) {
				chan->hangupcause = tc->hangupcause;
			}
			tris_hangup(tc);
			tc = NULL;
			tris_free(tmp);
			continue;
		} else {
			sendvideodialevent(chan, tc, numsubst);
			tris_verb(3, "Called %s\n", numsubst);
			if (!tris_test_flag64(peerflags, OPT_ORIGINAL_CLID))
				tris_set_callerid(tc, S_OR(chan->macroexten, chan->exten), get_cid_name(cidname, sizeof(cidname), chan), NULL);
		}
		/* Put them in the list of outgoing thingies...  We're ready now.
		   XXX If we're forcibly removed, these outgoing calls won't get
		   hung up XXX */
		tris_set_flag64(tmp, VIDEODIAL_STILLGOING);
		tmp->chan = tc;
		tmp->next = outgoing;
		outgoing = tmp;
		/* If this line is up, don't try anybody else */
		if (outgoing->chan->_state == TRIS_STATE_UP)
			break;
	}
	
	if (tris_strlen_zero(args.timeout)) {
		to = -1;
	} else {
		to = atoi(args.timeout);
		if (to > 0)
			to *= 1000;
		else {
			tris_log(LOG_WARNING, "Invalid timeout specified: '%s'. Setting timeout to infinite\n", args.timeout);
			to = -1;
		}
	}

	if (!outgoing) {
		strcpy(pa.status, "CHANUNAVAIL");
		if (fullvideodial == num_videodialed) {
			res = -1;
			goto out;
		}
	} else {
		/* Our status will at least be NOANSWER */
		strcpy(pa.status, "NOANSWER");
		if (tris_test_flag64(outgoing, OPT_MUSICBACK)) {
			moh = 1;
			if (!tris_strlen_zero(opt_args[OPT_ARG_MUSICBACK])) {
				char *original_moh = tris_strdupa(chan->musicclass);
				tris_string_field_set(chan, musicclass, opt_args[OPT_ARG_MUSICBACK]);
				tris_moh_start(chan, opt_args[OPT_ARG_MUSICBACK], NULL);
				tris_string_field_set(chan, musicclass, original_moh);
			} else {
				tris_moh_start(chan, NULL, NULL);
			}
			tris_indicate(chan, TRIS_CONTROL_PROGRESS);
		} else if (tris_test_flag64(outgoing, OPT_RINGBACK)) {
			tris_indicate(chan, TRIS_CONTROL_RINGING);
			sentringing++;
		}
	}

	peer = wait_for_answer(chan, outgoing, &to, peerflags, &pa, &num, &result);

	/* The tris_channel_datastore_remove() function could fail here if the
	 * datastore was moved to another channel during a masquerade. If this is
	 * the case, don't free the datastore here because later, when the channel
	 * to which the datastore was moved hangs up, it will attempt to free this
	 * datastore again, causing a crash
	 */
	if (!tris_channel_datastore_remove(chan, datastore))
		tris_datastore_free(datastore);
	if (!peer) {
		if (result) {
			res = result;
		} else if (to) { /* Musta gotten hung up */
			res = -1;
		} else { /* Nobody answered, next please? */
			res = 0;
		}

		/* SIP, in particular, sends back this error code to indicate an
		 * overlap videodialled number needs more digits. */
		if (chan->hangupcause == TRIS_CAUSE_INVALID_NUMBER_FORMAT) {
			res = TRIS_PBX_INCOMPLETE;
		}

		/* almost done, although the 'else' block is 400 lines */
	} else {
		const char *number;

		strcpy(pa.status, "ANSWER");
		pbx_builtin_setvar_helper(chan, "VIDEODIALSTATUS", pa.status);
		/* Ah ha!  Someone answered within the desired timeframe.  Of course after this
		   we will always return with -1 so that it is hung up properly after the
		   conversation.  */
		hanguptree(outgoing, peer, 1);
		outgoing = NULL;
		/* If appropriate, log that we have a destination channel */
		if (chan->cdr)
			tris_cdr_setdestchan(chan->cdr, peer->name);
		if (peer->name)
			pbx_builtin_setvar_helper(chan, "VIDEODIALEDPEERNAME", peer->name);
		
		tris_channel_lock(peer);
		number = pbx_builtin_getvar_helper(peer, "VIDEODIALEDPEERNUMBER"); 
		if (!number)
			number = numsubst;
		pbx_builtin_setvar_helper(chan, "VIDEODIALEDPEERNUMBER", number);
		tris_channel_unlock(peer);

		if (!tris_strlen_zero(args.url) && tris_channel_supports_html(peer) ) {
			tris_debug(1, "app_videodial: sendurl=%s.\n", args.url);
			tris_channel_sendurl( peer, args.url );
		}
		if ( (tris_test_flag64(&opts, OPT_PRIVACY) || tris_test_flag64(&opts, OPT_SCREENING)) && pa.privdb_val == TRIS_PRIVACY_UNKNOWN) {
			if (do_privacy(chan, peer, &opts, opt_args, &pa)) {
				res = 0;
				goto out;
			}
		}
		if (!tris_test_flag64(&opts, OPT_ANNOUNCE) || tris_strlen_zero(opt_args[OPT_ARG_ANNOUNCE])) {
			res = 0;
		} else {
			int digit = 0;
			struct tris_channel *chans[2];
			struct tris_channel *active_chan;

			chans[0] = chan;
			chans[1] = peer;

			/* we need to stream the announcment while monitoring the caller for a hangup */

			/* stream the file */
			res = tris_streamfile(peer, opt_args[OPT_ARG_ANNOUNCE], peer->language);
			if (res) {
				res = 0;
				tris_log(LOG_ERROR, "error streaming file '%s' to callee\n", opt_args[OPT_ARG_ANNOUNCE]);
			}

			tris_set_flag(peer, TRIS_FLAG_END_DTMF_ONLY);
			while (peer->stream) {
				int ms;

				ms = tris_sched_wait(peer->sched);

				if (ms < 0 && !peer->timingfunc) {
					tris_stopstream(peer);
					break;
				}
				if (ms < 0)
					ms = 1000;

				active_chan = tris_waitfor_n(chans, 2, &ms);
				if (active_chan) {
					struct tris_frame *fr = tris_read(active_chan);
					if (!fr) {
						tris_hangup(peer);
						res = -1;
						goto done;
					}
					switch(fr->frametype) {
						case TRIS_FRAME_DTMF_END:
							digit = fr->subclass;
							if (active_chan == peer && strchr(TRIS_DIGIT_ANY, res)) {
								tris_stopstream(peer);
								res = tris_senddigit(chan, digit, 0);
							}
							break;
						case TRIS_FRAME_CONTROL:
							switch (fr->subclass) {
								case TRIS_CONTROL_HANGUP:
									tris_frfree(fr);
									tris_hangup(peer);
									res = -1;
									goto done;
								default:
									break;
							}
							break;
						default:
							/* Ignore all others */
							break;
					}
					tris_frfree(fr);
				}
				tris_sched_runq(peer->sched);
			}
			tris_clear_flag(peer, TRIS_FLAG_END_DTMF_ONLY);
		}

		if (chan && peer && tris_test_flag64(&opts, OPT_GOTO) && !tris_strlen_zero(opt_args[OPT_ARG_GOTO])) {
			replace_macro_delimiter(opt_args[OPT_ARG_GOTO]);
			tris_parseable_goto(chan, opt_args[OPT_ARG_GOTO]);
			/* peer goes to the same context and extension as chan, so just copy info from chan*/
			tris_copy_string(peer->context, chan->context, sizeof(peer->context));
			tris_copy_string(peer->exten, chan->exten, sizeof(peer->exten));
			peer->priority = chan->priority + 2;
			tris_pbx_start(peer);
			hanguptree(outgoing, NULL, tris_test_flag64(&opts, OPT_CANCEL_ELSEWHERE) ? 1 : 0);
			if (continue_exec)
				*continue_exec = 1;
			res = 0;
			goto done;
		}

		if (tris_test_flag64(&opts, OPT_CALLEE_MACRO) && !tris_strlen_zero(opt_args[OPT_ARG_CALLEE_MACRO])) {
			struct tris_app *theapp;
			const char *macro_result;

			res = tris_autoservice_start(chan);
			if (res) {
				tris_log(LOG_ERROR, "Unable to start autoservice on calling channel\n");
				res = -1;
			}

			theapp = pbx_findapp("Macro");

			if (theapp && !res) { /* XXX why check res here ? */
				/* Set peer->exten and peer->context so that MACRO_EXTEN and MACRO_CONTEXT get set */
				tris_copy_string(peer->context, chan->context, sizeof(peer->context));
				tris_copy_string(peer->exten, chan->exten, sizeof(peer->exten));

				replace_macro_delimiter(opt_args[OPT_ARG_CALLEE_MACRO]);
				res = pbx_exec(peer, theapp, opt_args[OPT_ARG_CALLEE_MACRO]);
				tris_debug(1, "Macro exited with status %d\n", res);
				res = 0;
			} else {
				tris_log(LOG_ERROR, "Could not find application Macro\n");
				res = -1;
			}

			if (tris_autoservice_stop(chan) < 0) {
				res = -1;
			}

			tris_channel_lock(peer);

			if (!res && (macro_result = pbx_builtin_getvar_helper(peer, "MACRO_RESULT"))) {
				char *macro_transfer_dest;

				if (!strcasecmp(macro_result, "BUSY")) {
					tris_copy_string(pa.status, macro_result, sizeof(pa.status));
					tris_set_flag64(peerflags, OPT_GO_ON);
					res = -1;
				} else if (!strcasecmp(macro_result, "CONGESTION") || !strcasecmp(macro_result, "CHANUNAVAIL")) {
					tris_copy_string(pa.status, macro_result, sizeof(pa.status));
					tris_set_flag64(peerflags, OPT_GO_ON);
					res = -1;
				} else if (!strcasecmp(macro_result, "CONTINUE")) {
					/* hangup peer and keep chan alive assuming the macro has changed
					   the context / exten / priority or perhaps
					   the next priority in the current exten is desired.
					*/
					tris_set_flag64(peerflags, OPT_GO_ON);
					res = -1;
				} else if (!strcasecmp(macro_result, "ABORT")) {
					/* Hangup both ends unless the caller has the g flag */
					res = -1;
				} else if (!strncasecmp(macro_result, "GOTO:", 5) && (macro_transfer_dest = tris_strdupa(macro_result + 5))) {
					res = -1;
					/* perform a transfer to a new extension */
					if (strchr(macro_transfer_dest, '^')) { /* context^exten^priority*/
						replace_macro_delimiter(macro_transfer_dest);
						if (!tris_parseable_goto(chan, macro_transfer_dest))
							tris_set_flag64(peerflags, OPT_GO_ON);
					}
				}
			}

			tris_channel_unlock(peer);
		}

		if (tris_test_flag64(&opts, OPT_CALLEE_GOSUB) && !tris_strlen_zero(opt_args[OPT_ARG_CALLEE_GOSUB])) {
			struct tris_app *theapp;
			const char *gosub_result;
			char *gosub_args, *gosub_argstart;
			int res9 = -1;

			res9 = tris_autoservice_start(chan);
			if (res9) {
				tris_log(LOG_ERROR, "Unable to start autoservice on calling channel\n");
				res9 = -1;
			}

			theapp = pbx_findapp("Gosub");

			if (theapp && !res9) {
				replace_macro_delimiter(opt_args[OPT_ARG_CALLEE_GOSUB]);

				/* Set where we came from */
				tris_copy_string(peer->context, "app_videodial_gosub_virtual_context", sizeof(peer->context));
				tris_copy_string(peer->exten, "s", sizeof(peer->exten));
				peer->priority = 0;

				gosub_argstart = strchr(opt_args[OPT_ARG_CALLEE_GOSUB], ',');
				if (gosub_argstart) {
					*gosub_argstart = 0;
					if (asprintf(&gosub_args, "%s,s,1(%s)", opt_args[OPT_ARG_CALLEE_GOSUB], gosub_argstart + 1) < 0) {
						tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
						gosub_args = NULL;
					}
					*gosub_argstart = ',';
				} else {
					if (asprintf(&gosub_args, "%s,s,1", opt_args[OPT_ARG_CALLEE_GOSUB]) < 0) {
						tris_log(LOG_WARNING, "asprintf() failed: %s\n", strerror(errno));
						gosub_args = NULL;
					}
				}

				if (gosub_args) {
					res9 = pbx_exec(peer, theapp, gosub_args);
					if (!res9) {
						struct tris_pbx_args args;
						/* A struct initializer fails to compile for this case ... */
						memset(&args, 0, sizeof(args));
						args.no_hangup_chan = 1;
						tris_pbx_run_args(peer, &args);
					}
					tris_free(gosub_args);
					tris_debug(1, "Gosub exited with status %d\n", res9);
				} else {
					tris_log(LOG_ERROR, "Could not Allocate string for Gosub arguments -- Gosub Call Aborted!\n");
				}

			} else if (!res9) {
				tris_log(LOG_ERROR, "Could not find application Gosub\n");
				res9 = -1;
			}

			if (tris_autoservice_stop(chan) < 0) {
				tris_log(LOG_ERROR, "Could not stop autoservice on calling channel\n");
				res9 = -1;
			}
			
			tris_channel_lock(peer);

			if (!res9 && (gosub_result = pbx_builtin_getvar_helper(peer, "GOSUB_RESULT"))) {
				char *gosub_transfer_dest;

				if (!strcasecmp(gosub_result, "BUSY")) {
					tris_copy_string(pa.status, gosub_result, sizeof(pa.status));
					tris_set_flag64(peerflags, OPT_GO_ON);
					res = -1;
				} else if (!strcasecmp(gosub_result, "CONGESTION") || !strcasecmp(gosub_result, "CHANUNAVAIL")) {
					tris_copy_string(pa.status, gosub_result, sizeof(pa.status));
					tris_set_flag64(peerflags, OPT_GO_ON);
					res = -1;
				} else if (!strcasecmp(gosub_result, "CONTINUE")) {
					/* hangup peer and keep chan alive assuming the macro has changed
					   the context / exten / priority or perhaps
					   the next priority in the current exten is desired.
					*/
					tris_set_flag64(peerflags, OPT_GO_ON);
					res = -1;
				} else if (!strcasecmp(gosub_result, "ABORT")) {
					/* Hangup both ends unless the caller has the g flag */
					res = -1;
				} else if (!strncasecmp(gosub_result, "GOTO:", 5) && (gosub_transfer_dest = tris_strdupa(gosub_result + 5))) {
					res = -1;
					/* perform a transfer to a new extension */
					if (strchr(gosub_transfer_dest, '^')) { /* context^exten^priority*/
						replace_macro_delimiter(gosub_transfer_dest);
						if (!tris_parseable_goto(chan, gosub_transfer_dest))
							tris_set_flag64(peerflags, OPT_GO_ON);
					}
				}
			}

			tris_channel_unlock(peer);	
		}

		if (!res) {
			if (!tris_tvzero(calldurationlimit)) {
				struct timeval whentohangup = calldurationlimit;
				peer->whentohangup = tris_tvadd(tris_tvnow(), whentohangup);
			}
			if (!tris_strlen_zero(dtmfcalled)) {
				tris_verb(3, "Sending DTMF '%s' to the called party.\n", dtmfcalled);
				res = tris_dtmf_stream(peer, chan, dtmfcalled, 250, 0);
			}
			if (!tris_strlen_zero(dtmfcalling)) {
				tris_verb(3, "Sending DTMF '%s' to the calling party.\n", dtmfcalling);
				res = tris_dtmf_stream(chan, peer, dtmfcalling, 250, 0);
			}
		}

		if (res) { /* some error */
			res = -1;
		} else {
			if (tris_test_flag64(peerflags, OPT_CALLEE_TRANSFER))
				tris_set_flag(&(config.features_callee), TRIS_FEATURE_REDIRECT);
			if (tris_test_flag64(peerflags, OPT_CALLER_TRANSFER))
				tris_set_flag(&(config.features_caller), TRIS_FEATURE_REDIRECT);
			if (tris_test_flag64(peerflags, OPT_CALLEE_HANGUP))
				tris_set_flag(&(config.features_callee), TRIS_FEATURE_DISCONNECT);
			if (tris_test_flag64(peerflags, OPT_CALLER_HANGUP))
				tris_set_flag(&(config.features_caller), TRIS_FEATURE_DISCONNECT);
			if (tris_test_flag64(peerflags, OPT_CALLEE_MONITOR))
				tris_set_flag(&(config.features_callee), TRIS_FEATURE_AUTOMON);
			if (tris_test_flag64(peerflags, OPT_CALLER_MONITOR))
				tris_set_flag(&(config.features_caller), TRIS_FEATURE_AUTOMON);
			if (tris_test_flag64(peerflags, OPT_CALLEE_PARK))
				tris_set_flag(&(config.features_callee), TRIS_FEATURE_PARKCALL);
			if (tris_test_flag64(peerflags, OPT_CALLER_PARK))
				tris_set_flag(&(config.features_caller), TRIS_FEATURE_PARKCALL);
			if (tris_test_flag64(peerflags, OPT_CALLEE_MIXMONITOR))
				tris_set_flag(&(config.features_callee), TRIS_FEATURE_AUTOMIXMON);
			if (tris_test_flag64(peerflags, OPT_CALLER_MIXMONITOR))
				tris_set_flag(&(config.features_caller), TRIS_FEATURE_AUTOMIXMON);
			if (tris_test_flag64(peerflags, OPT_GO_ON))
				tris_set_flag(&(config.features_caller), TRIS_FEATURE_NO_H_EXTEN);

			config.end_bridge_callback = end_bridge_callback;
			config.end_bridge_callback_data = chan;
			config.end_bridge_callback_data_fixup = end_bridge_callback_data_fixup;
			
			if (moh) {
				moh = 0;
				tris_moh_stop(chan);
			} else if (sentringing) {
				sentringing = 0;
				tris_indicate(chan, -1);
			}
			/* Be sure no generators are left on it */
			tris_deactivate_generator(chan);
			/* Make sure channels are compatible */
			res = tris_channel_make_compatible(chan, peer);
			if (res < 0) {
				tris_log(LOG_WARNING, "Had to drop call because I couldn't make %s compatible with %s\n", chan->name, peer->name);
				tris_hangup(peer);
				res = -1;
				goto done;
			}
			if (opermode) {
				struct oprmode oprmode;

				oprmode.peer = peer;
				oprmode.mode = opermode;

				tris_channel_setoption(chan, TRIS_OPTION_OPRMODE, &oprmode, sizeof(oprmode), 0);
			}

			if (chan->transferchan) {
				if (chan->transfer_bridge) {
					if (!tris_check_hangup(chan))
						chan->hangupcause = chan->transfer_bridge->hangupcause;
					tris_hangup(chan->transfer_bridge);
				}
				chan->transfer_bridge = NULL;
			}

			res = tris_bridge_call(chan, peer, &config);

			if (res == 25)
				return res;
		}

		strcpy(peer->context, chan->context);

		if (tris_test_flag64(&opts, OPT_PEER_H) && tris_exists_extension(peer, peer->context, "h", 1, peer->cid.cid_num)) {
			int autoloopflag;
			int found;
			int res9;
			
			strcpy(peer->exten, "h");
			peer->priority = 1;
			autoloopflag = tris_test_flag(peer, TRIS_FLAG_IN_AUTOLOOP); /* save value to restore at the end */
			tris_set_flag(peer, TRIS_FLAG_IN_AUTOLOOP);

			while ((res9 = tris_spawn_extension(peer, peer->context, peer->exten, peer->priority, peer->cid.cid_num, &found, 1)) == 0)
				peer->priority++;

			if (found && res9) {
				/* Something bad happened, or a hangup has been requested. */
				tris_debug(1, "Spawn extension (%s,%s,%d) exited non-zero on '%s'\n", peer->context, peer->exten, peer->priority, peer->name);
				tris_verb(2, "Spawn extension (%s, %s, %d) exited non-zero on '%s'\n", peer->context, peer->exten, peer->priority, peer->name);
			}
			tris_set2_flag(peer, autoloopflag, TRIS_FLAG_IN_AUTOLOOP);  /* set it back the way it was */
		}
		if (!tris_check_hangup(peer) && tris_test_flag64(&opts, OPT_CALLEE_GO_ON) && !tris_strlen_zero(opt_args[OPT_ARG_CALLEE_GO_ON])) {		
			replace_macro_delimiter(opt_args[OPT_ARG_CALLEE_GO_ON]);
			tris_parseable_goto(peer, opt_args[OPT_ARG_CALLEE_GO_ON]);
			tris_pbx_start(peer);
		} else {
			if (!tris_check_hangup(chan))
				chan->hangupcause = peer->hangupcause;
			tris_hangup(peer);
		}
	}
out:
	if (moh) {
		moh = 0;
		tris_moh_stop(chan);
	} else if (sentringing) {
		sentringing = 0;
		tris_indicate(chan, -1);
	}

	if (delprivintro && tris_fileexists(pa.privintro, NULL, NULL) > 0) {
		tris_filedelete(pa.privintro, NULL);
		if (tris_fileexists(pa.privintro, NULL, NULL) > 0) {
			tris_log(LOG_NOTICE, "privacy: tris_filedelete didn't do its job on %s\n", pa.privintro);
		} else {
			tris_verb(3, "Successfully deleted %s intro file\n", pa.privintro);
		}
	}

	tris_channel_early_bridge(chan, NULL);
	hanguptree(outgoing, NULL, 0); /* In this case, there's no answer anywhere */
	pbx_builtin_setvar_helper(chan, "VIDEODIALSTATUS", pa.status);
	sendvideodialendevent(chan, pa.status);
	tris_debug(1, "Exiting with VIDEODIALSTATUS=%s.\n", pa.status);
	
	if ((tris_test_flag64(peerflags, OPT_GO_ON)) && !tris_check_hangup(chan) && (res != TRIS_PBX_INCOMPLETE)) {
		if (!tris_tvzero(calldurationlimit))
			memset(&chan->whentohangup, 0, sizeof(chan->whentohangup));
		res = 0;
	}

done:
	if (config.warning_sound) {
		tris_free((char *)config.warning_sound);
	}
	if (config.end_sound) {
		tris_free((char *)config.end_sound);
	}
	if (config.start_sound) {
		tris_free((char *)config.start_sound);
	}
	return res;
}

static int videodial_exec(struct tris_channel *chan, void *data)
{
	struct tris_flags64 peerflags;

	memset(&peerflags, 0, sizeof(peerflags));

	if (!chan->monitor) {
		exec_monitor(chan);
	}

	return videodial_exec_full(chan, data, &peerflags, NULL);
}

static int retryvideodial_exec(struct tris_channel *chan, void *data)
{
	char *parse;
	const char *context = NULL;
	int sleepms = 0, loops = 0, res = -1;
	struct tris_flags64 peerflags = { 0, };
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(announce);
		TRIS_APP_ARG(sleep);
		TRIS_APP_ARG(retries);
		TRIS_APP_ARG(videodialdata);
	);

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "RetryVideodial requires an argument!\n");
		return -1;
	}

	parse = tris_strdupa(data);
	TRIS_STANDARD_APP_ARGS(args, parse);

	if (!tris_strlen_zero(args.sleep) && (sleepms = atoi(args.sleep)))
		sleepms *= 1000;

	if (!tris_strlen_zero(args.retries)) {
		loops = atoi(args.retries);
	}

	if (!args.videodialdata) {
		tris_log(LOG_ERROR, "%s requires a 4th argument (videodialdata)\n", rapp);
		goto done;
	}

	if (sleepms < 1000)
		sleepms = 10000;

	if (!loops)
		loops = -1; /* run forever */

	tris_channel_lock(chan);
	context = pbx_builtin_getvar_helper(chan, "EXITCONTEXT");
	context = !tris_strlen_zero(context) ? tris_strdupa(context) : NULL;
	tris_channel_unlock(chan);

	res = 0;
	while (loops) {
		int continue_exec;

		chan->data = "Retrying";
		if (tris_test_flag(chan, TRIS_FLAG_MOH))
			tris_moh_stop(chan);

		res = videodial_exec_full(chan, args.videodialdata, &peerflags, &continue_exec);
		if (continue_exec)
			break;

		if (res == 0) {
			if (tris_test_flag64(&peerflags, OPT_DTMF_EXIT)) {
				if (!tris_strlen_zero(args.announce)) {
					if (tris_fileexists(args.announce, NULL, chan->language) > 0) {
						if (!(res = tris_streamfile(chan, args.announce, chan->language)))
							tris_waitstream(chan, TRIS_DIGIT_ANY);
					} else
						tris_log(LOG_WARNING, "Announce file \"%s\" specified in Retryvideodial does not exist\n", args.announce);
				}
				if (!res && sleepms) {
					if (!tris_test_flag(chan, TRIS_FLAG_MOH))
						tris_moh_start(chan, NULL, NULL);
					res = tris_waitfordigit(chan, sleepms);
				}
			} else {
				if (!tris_strlen_zero(args.announce)) {
					if (tris_fileexists(args.announce, NULL, chan->language) > 0) {
						if (!(res = tris_streamfile(chan, args.announce, chan->language)))
							res = tris_waitstream(chan, "");
					} else
						tris_log(LOG_WARNING, "Announce file \"%s\" specified in Retryvideodial does not exist\n", args.announce);
				}
				if (sleepms) {
					if (!tris_test_flag(chan, TRIS_FLAG_MOH))
						tris_moh_start(chan, NULL, NULL);
					if (!res)
						res = tris_waitfordigit(chan, sleepms);
				}
			}
		}

		if (res < 0 || res == TRIS_PBX_INCOMPLETE) {
			break;
		} else if (res > 0) { /* Trying to send the call elsewhere (1 digit ext) */
			if (onedigit_goto(chan, context, (char) res, 1)) {
				res = 0;
				break;
			}
		}
		loops--;
	}
	if (loops == 0)
		res = 0;
	else if (res == 1)
		res = 0;

	if (tris_test_flag(chan, TRIS_FLAG_MOH))
		tris_moh_stop(chan);
 done:
	return res;
}

static int unload_module(void)
{
	int res;
	struct tris_context *con;

	res = tris_unregister_application(app);
	res |= tris_unregister_application(rapp);

	if ((con = tris_context_find("app_videodial_gosub_virtual_context"))) {
		tris_context_remove_extension2(con, "s", 1, NULL, 0);
		tris_context_destroy(con, "app_videodial"); /* leave nothing behind */
	}

	return res;
}

static int load_module(void)
{
	int res;
	struct tris_context *con;

	con = tris_context_find_or_create(NULL, NULL, "app_videodial_gosub_virtual_context", "app_videodial");
	if (!con)
		tris_log(LOG_ERROR, "Videodial virtual context 'app_videodial_gosub_virtual_context' does not exist and unable to create\n");
	else
		tris_add_extension2(con, 1, "s", 1, NULL, NULL, "NoOp", tris_strdup(""), tris_free_ptr, "app_videodial");

	res = tris_register_application_xml(app, videodial_exec);
	res |= tris_register_application_xml(rapp, retryvideodial_exec);

	return res;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Videodialing Application");
