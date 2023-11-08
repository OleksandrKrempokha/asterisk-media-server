/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Implementation of Agents (proxy channel)
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * This file is the implementation of Agents modules.
 * It is a dynamic module that is loaded by Trismedia. 
 * \par See also
 * \arg \ref Config_agent
 *
 * \ingroup channel_drivers
 */
/*** MODULEINFO
        <depend>chan_local</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 241318 $")

#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/signal.h>

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/sched.h"
#include "trismedia/io.h"
#include "trismedia/rtp.h"
#include "trismedia/acl.h"
#include "trismedia/callerid.h"
#include "trismedia/file.h"
#include "trismedia/cli.h"
#include "trismedia/app.h"
#include "trismedia/musiconhold.h"
#include "trismedia/manager.h"
#include "trismedia/features.h"
#include "trismedia/utils.h"
#include "trismedia/causes.h"
#include "trismedia/astdb.h"
#include "trismedia/devicestate.h"
#include "trismedia/monitor.h"
#include "trismedia/stringfields.h"
#include "trismedia/event.h"

/*** DOCUMENTATION
	<application name="AgentLogin" language="en_US">
		<synopsis>
			Call agent login.
		</synopsis>
		<syntax>
			<parameter name="AgentNo" />
			<parameter name="options">
				<optionlist>
					<option name="s">
						<para>silent login - do not announce the login ok segment after
						agent logged on/off</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Asks the agent to login to the system. Always returns <literal>-1</literal>.
			While logged in, the agent can receive calls and will hear a <literal>beep</literal>
			when a new call comes in. The agent can dump the call by pressing the star key.</para>
		</description>
		<see-also>
			<ref type="application">Queue</ref>
			<ref type="application">AddQueueMember</ref>
			<ref type="application">RemoveQueueMember</ref>
			<ref type="application">PauseQueueMember</ref>
			<ref type="application">UnpauseQueueMember</ref>
			<ref type="function">AGENT</ref>
			<ref type="filename">agents.conf</ref>
			<ref type="filename">queues.conf</ref>
		</see-also>
	</application>
	<application name="AgentMonitorOutgoing" language="en_US">
		<synopsis>
			Record agent's outgoing call.
		</synopsis>
		<syntax>
			<parameter name="options">
				<optionlist>
					<option name="d">
						<para>make the app return <literal>-1</literal> if there is an error condition.</para>
					</option>
					<option name="c">
						<para>change the CDR so that the source of the call is
						<literal>Agent/agent_id</literal></para>
					</option>
					<option name="n">
						<para>don't generate the warnings when there is no callerid or the
						agentid is not known. It's handy if you want to have one context
						for agent and non-agent calls.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Tries to figure out the id of the agent who is placing outgoing call based on
			comparison of the callerid of the current interface and the global variable
			placed by the AgentCallbackLogin application. That's why it should be used only
			with the AgentCallbackLogin app. Uses the monitoring functions in chan_agent
			instead of Monitor application. That has to be configured in the
			<filename>agents.conf</filename> file.</para>
			<para>Normally the app returns <literal>0</literal> unless the options are passed.</para>
		</description>
		<see-also>
			<ref type="filename">agents.conf</ref>
		</see-also>
	</application>
	<function name="AGENT" language="en_US">
		<synopsis>
			Gets information about an Agent
		</synopsis>
		<syntax argsep=":">
			<parameter name="agentid" required="true" />
			<parameter name="item">
				<para>The valid items to retrieve are:</para>
				<enumlist>
					<enum name="status">
						<para>(default) The status of the agent (LOGGEDIN | LOGGEDOUT)</para>
					</enum>
					<enum name="password">
						<para>The password of the agent</para>
					</enum>
					<enum name="name">
						<para>The name of the agent</para>
					</enum>
					<enum name="mohclass">
						<para>MusicOnHold class</para>
					</enum>
					<enum name="exten">
						<para>The callback extension for the Agent (AgentCallbackLogin)</para>
					</enum>
					<enum name="channel">
						<para>The name of the active channel for the Agent (AgentLogin)</para>
					</enum>
				</enumlist>
			</parameter>
		</syntax>
		<description />
	</function>
 ***/

static const char tdesc[] = "Call Agent Proxy Channel";
static const char config[] = "agents.conf";

static const char app[] = "AgentLogin";
static const char app3[] = "AgentMonitorOutgoing";

static const char mandescr_agents[] =
"Description: Will list info about all possible agents.\n"
"Variables: NONE\n";

static const char mandescr_agent_logoff[] =
"Description: Sets an agent as no longer logged in.\n"
"Variables: (Names marked with * are required)\n"
"	*Agent: Agent ID of the agent to log off\n"
"	Soft: Set to 'true' to not hangup existing calls\n";

static char moh[80] = "default";

#define TRIS_MAX_AGENT	80                          /*!< Agent ID or Password max length */
#define TRIS_MAX_BUF	256
#define TRIS_MAX_FILENAME_LEN	256

static const char pa_family[] = "Agents";          /*!< Persistent Agents astdb family */
#define PA_MAX_LEN 2048                             /*!< The maximum length of each persistent member agent database entry */

static int persistent_agents = 0;                   /*!< queues.conf [general] option */
static void dump_agents(void);

#define DEFAULT_ACCEPTDTMF '#'
#define DEFAULT_ENDDTMF '*'

static tris_group_t group;
static int autologoff;
static int wrapuptime;
static int ackcall;
static int endcall;
static int multiplelogin = 1;
static int autologoffunavail = 0;
static char acceptdtmf = DEFAULT_ACCEPTDTMF;
static char enddtmf = DEFAULT_ENDDTMF;

static int maxlogintries = 3;
static char agentgoodbye[TRIS_MAX_FILENAME_LEN] = "goodbye";

static int recordagentcalls = 0;
static char recordformat[TRIS_MAX_BUF] = "";
static char recordformatext[TRIS_MAX_BUF] = "";
static char urlprefix[TRIS_MAX_BUF] = "";
static char savecallsin[TRIS_MAX_BUF] = "";
static int updatecdr = 0;
static char beep[TRIS_MAX_BUF] = "beep";

#define GETAGENTBYCALLERID	"AGENTBYCALLERID"

enum {
	AGENT_FLAG_ACKCALL = (1 << 0),
	AGENT_FLAG_AUTOLOGOFF = (1 << 1),
	AGENT_FLAG_WRAPUPTIME = (1 << 2),
	AGENT_FLAG_ACCEPTDTMF = (1 << 3),
	AGENT_FLAG_ENDDTMF = (1 << 4),
};

/*! \brief Structure representing an agent.  */
struct agent_pvt {
	tris_mutex_t lock;              /*!< Channel private lock */
	int dead;                      /*!< Poised for destruction? */
	int pending;                   /*!< Not a real agent -- just pending a match */
	int abouttograb;               /*!< About to grab */
	int autologoff;                /*!< Auto timeout time */
	int ackcall;                   /*!< ackcall */
	int deferlogoff;               /*!< Defer logoff to hangup */
	char acceptdtmf;
	char enddtmf;
	time_t loginstart;             /*!< When agent first logged in (0 when logged off) */
	time_t start;                  /*!< When call started */
	struct timeval lastdisc;       /*!< When last disconnected */
	int wrapuptime;                /*!< Wrapup time in ms */
	tris_group_t group;             /*!< Group memberships */
	int acknowledged;              /*!< Acknowledged */
	char moh[80];                  /*!< Which music on hold */
	char agent[TRIS_MAX_AGENT];     /*!< Agent ID */
	char password[TRIS_MAX_AGENT];  /*!< Password for Agent login */
	char name[TRIS_MAX_AGENT];
	tris_mutex_t app_lock;          /**< Synchronization between owning applications */
	int app_lock_flag;
	tris_cond_t app_complete_cond;
	volatile int app_sleep_cond;   /**< Sleep condition for the login app */
	struct tris_channel *owner;     /**< Agent */
	char loginchan[80];            /**< channel they logged in from */
	char logincallerid[80];        /**< Caller ID they had when they logged in */
	struct tris_channel *chan;      /**< Channel we use */
	unsigned int flags;            /**< Flags show if settings were applied with channel vars */
	TRIS_LIST_ENTRY(agent_pvt) list;	/**< Next Agent in the linked list. */
};

static TRIS_LIST_HEAD_STATIC(agents, agent_pvt);	/*!< Holds the list of agents (loaded form agents.conf). */

#define CHECK_FORMATS(ast, p) do { \
	if (p->chan) {\
		if (ast->nativeformats != p->chan->nativeformats) { \
			tris_debug(1, "Native formats changing from %d to %d\n", ast->nativeformats, p->chan->nativeformats); \
			/* Native formats changed, reset things */ \
			ast->nativeformats = p->chan->nativeformats; \
			tris_debug(1, "Resetting read to %d and write to %d\n", ast->readformat, ast->writeformat);\
			tris_set_read_format(ast, ast->readformat); \
			tris_set_write_format(ast, ast->writeformat); \
		} \
		if (p->chan->readformat != ast->rawreadformat && !p->chan->generator)  \
			tris_set_read_format(p->chan, ast->rawreadformat); \
		if (p->chan->writeformat != ast->rawwriteformat && !p->chan->generator) \
			tris_set_write_format(p->chan, ast->rawwriteformat); \
	} \
} while(0)

/*! \brief Cleanup moves all the relevant FD's from the 2nd to the first, but retains things
   properly for a timingfd XXX This might need more work if agents were logged in as agents or other
   totally impractical combinations XXX */

#define CLEANUP(ast, p) do { \
	int x; \
	if (p->chan) { \
		for (x=0;x<TRIS_MAX_FDS;x++) {\
			if (x != TRIS_TIMING_FD) \
				tris_channel_set_fd(ast, x, p->chan->fds[x]); \
		} \
		tris_channel_set_fd(ast, TRIS_AGENT_FD, p->chan->fds[TRIS_TIMING_FD]); \
	} \
} while(0)

/*--- Forward declarations */
static struct tris_channel *agent_request(const char *type, int format, void *data, int *cause, struct tris_channel* src);
static int agent_devicestate(void *data);
static void agent_logoff_maintenance(struct agent_pvt *p, char *loginchan, long logintime, const char *uniqueid, char *logcommand);
static int agent_digit_begin(struct tris_channel *ast, char digit);
static int agent_digit_end(struct tris_channel *ast, char digit, unsigned int duration);
static int agent_call(struct tris_channel *ast, char *dest, int timeout);
static int agent_hangup(struct tris_channel *ast);
static int agent_answer(struct tris_channel *ast);
static struct tris_frame *agent_read(struct tris_channel *ast);
static int agent_write(struct tris_channel *ast, struct tris_frame *f);
static int agent_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen);
static int agent_sendtext(struct tris_channel *ast, const char *text);
static int agent_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen);
static int agent_fixup(struct tris_channel *oldchan, struct tris_channel *newchan);
static struct tris_channel *agent_bridgedchannel(struct tris_channel *chan, struct tris_channel *bridge);
static void set_agentbycallerid(const char *callerid, const char *agent);
static char *complete_agent_logoff_cmd(const char *line, const char *word, int pos, int state);
static struct tris_channel* agent_get_base_channel(struct tris_channel *chan);
static int agent_set_base_channel(struct tris_channel *chan, struct tris_channel *base);
static int agent_logoff(const char *agent, int soft);

/*! \brief Channel interface description for PBX integration */
static const struct tris_channel_tech agent_tech = {
	.type = "Agent",
	.description = tdesc,
	.capabilities = -1,
	.requester = agent_request,
	.devicestate = agent_devicestate,
	.send_digit_begin = agent_digit_begin,
	.send_digit_end = agent_digit_end,
	.call = agent_call,
	.hangup = agent_hangup,
	.answer = agent_answer,
	.read = agent_read,
	.write = agent_write,
	.write_video = agent_write,
	.send_html = agent_sendhtml,
	.send_text = agent_sendtext,
	.exception = agent_read,
	.indicate = agent_indicate,
	.fixup = agent_fixup,
	.bridged_channel = agent_bridgedchannel,
	.get_base_channel = agent_get_base_channel,
	.set_base_channel = agent_set_base_channel,
};

/*!
 * Adds an agent to the global list of agents.
 *
 * \param agent A string with the username, password and real name of an agent. As defined in agents.conf. Example: "13,169,John Smith"
 * \param pending If it is pending or not.
 * @return The just created agent.
 * \sa agent_pvt, agents.
 */
static struct agent_pvt *add_agent(const char *agent, int pending)
{
	char *parse;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(agt);
		TRIS_APP_ARG(password);
		TRIS_APP_ARG(name);
	);
	char *password = NULL;
	char *name = NULL;
	char *agt = NULL;
	struct agent_pvt *p;

	parse = tris_strdupa(agent);

	/* Extract username (agt), password and name from agent (args). */
	TRIS_STANDARD_APP_ARGS(args, parse);

	if(args.argc == 0) {
		tris_log(LOG_WARNING, "A blank agent line!\n");
		return NULL;
	}

	if(tris_strlen_zero(args.agt) ) {
		tris_log(LOG_WARNING, "An agent line with no agentid!\n");
		return NULL;
	} else
		agt = args.agt;

	if(!tris_strlen_zero(args.password)) {
		password = args.password;
		while (*password && *password < 33) password++;
	}
	if(!tris_strlen_zero(args.name)) {
		name = args.name;
		while (*name && *name < 33) name++;
	}
	
	/* Are we searching for the agent here ? To see if it exists already ? */
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		if (!pending && !strcmp(p->agent, agt))
			break;
	}
	if (!p) {
		// Build the agent.
		if (!(p = tris_calloc(1, sizeof(*p))))
			return NULL;
		tris_copy_string(p->agent, agt, sizeof(p->agent));
		tris_mutex_init(&p->lock);
		tris_mutex_init(&p->app_lock);
		tris_cond_init(&p->app_complete_cond, NULL);
		p->app_lock_flag = 0;
		p->app_sleep_cond = 1;
		p->group = group;
		p->pending = pending;
		TRIS_LIST_INSERT_TAIL(&agents, p, list);
	}
	
	tris_copy_string(p->password, password ? password : "", sizeof(p->password));
	tris_copy_string(p->name, name ? name : "", sizeof(p->name));
	tris_copy_string(p->moh, moh, sizeof(p->moh));
	if (!tris_test_flag(p, AGENT_FLAG_ACKCALL)) {
		p->ackcall = ackcall;
	}
	if (!tris_test_flag(p, AGENT_FLAG_AUTOLOGOFF)) {
		p->autologoff = autologoff;
	}
	if (!tris_test_flag(p, AGENT_FLAG_ACCEPTDTMF)) {
		p->acceptdtmf = acceptdtmf;
	}
	if (!tris_test_flag(p, AGENT_FLAG_ENDDTMF)) {
		p->enddtmf = enddtmf;
	}

	/* If someone reduces the wrapuptime and reloads, we want it
	 * to change the wrapuptime immediately on all calls */
	if (!tris_test_flag(p, AGENT_FLAG_WRAPUPTIME) && p->wrapuptime > wrapuptime) {
		struct timeval now = tris_tvnow();
		/* XXX check what is this exactly */

		/* We won't be pedantic and check the tv_usec val */
		if (p->lastdisc.tv_sec > (now.tv_sec + wrapuptime/1000)) {
			p->lastdisc.tv_sec = now.tv_sec + wrapuptime/1000;
			p->lastdisc.tv_usec = now.tv_usec;
		}
	}
	p->wrapuptime = wrapuptime;

	if (pending)
		p->dead = 1;
	else
		p->dead = 0;
	return p;
}

/*!
 * Deletes an agent after doing some clean up.
 * Further documentation: How safe is this function ? What state should the agent be to be cleaned.
 * \param p Agent to be deleted.
 * \returns Always 0.
 */
static int agent_cleanup(struct agent_pvt *p)
{
	struct tris_channel *chan = p->owner;
	p->owner = NULL;
	chan->tech_pvt = NULL;
	p->app_sleep_cond = 1;
	/* Release ownership of the agent to other threads (presumably running the login app). */
	p->app_lock_flag = 0;
	tris_cond_signal(&p->app_complete_cond);
	if (chan)
		tris_channel_free(chan);
	if (p->dead) {
		tris_mutex_destroy(&p->lock);
		tris_mutex_destroy(&p->app_lock);
		tris_cond_destroy(&p->app_complete_cond);
		tris_free(p);
        }
	return 0;
}

static int check_availability(struct agent_pvt *newlyavailable, int needlock);

static int agent_answer(struct tris_channel *ast)
{
	tris_log(LOG_WARNING, "Huh?  Agent is being asked to answer?\n");
	return -1;
}

static int __agent_start_monitoring(struct tris_channel *ast, struct agent_pvt *p, int needlock)
{
	char tmp[TRIS_MAX_BUF],tmp2[TRIS_MAX_BUF], *pointer;
	char filename[TRIS_MAX_BUF];
	int res = -1;
	if (!p)
		return -1;
	if (!ast->monitor) {
		snprintf(filename, sizeof(filename), "agent-%s-%s",p->agent, ast->uniqueid);
		/* substitute . for - */
		if ((pointer = strchr(filename, '.')))
			*pointer = '-';
		snprintf(tmp, sizeof(tmp), "%s%s", savecallsin, filename);
		tris_monitor_start(ast, recordformat, tmp, needlock, X_REC_IN | X_REC_OUT);
		tris_monitor_setjoinfiles(ast, 1);
		snprintf(tmp2, sizeof(tmp2), "%s%s.%s", urlprefix, filename, recordformatext);
#if 0
		tris_verbose("name is %s, link is %s\n",tmp, tmp2);
#endif
		if (!ast->cdr)
			ast->cdr = tris_cdr_alloc();
		tris_cdr_setuserfield(ast, tmp2);
		res = 0;
	} else
		tris_log(LOG_ERROR, "Recording already started on that call.\n");
	return res;
}

static int agent_start_monitoring(struct tris_channel *ast, int needlock)
{
	return __agent_start_monitoring(ast, ast->tech_pvt, needlock);
}

static struct tris_frame *agent_read(struct tris_channel *ast)
{
	struct agent_pvt *p = ast->tech_pvt;
	struct tris_frame *f = NULL;
	static struct tris_frame answer_frame = { TRIS_FRAME_CONTROL, TRIS_CONTROL_ANSWER };
	const char *status;
	int cur_time = time(NULL);
	tris_mutex_lock(&p->lock);
	CHECK_FORMATS(ast, p);
	if (!p->start) {
		p->start = cur_time;
	}
	if (p->chan) {
		tris_copy_flags(p->chan, ast, TRIS_FLAG_EXCEPTION);
		p->chan->fdno = (ast->fdno == TRIS_AGENT_FD) ? TRIS_TIMING_FD : ast->fdno;
		f = tris_read(p->chan);
	} else
		f = &tris_null_frame;
	if (!f) {
		/* If there's a channel, hang it up (if it's on a callback) make it NULL */
		if (p->chan) {
			p->chan->_bridge = NULL;
			/* Note that we don't hangup if it's not a callback because Trismedia will do it
			   for us when the PBX instance that called login finishes */
			if (!tris_strlen_zero(p->loginchan)) {
				if (p->chan)
					tris_debug(1, "Bridge on '%s' being cleared (2)\n", p->chan->name);
				if (p->owner->_state != TRIS_STATE_UP) {
					int howlong = cur_time - p->start;
					if (p->autologoff && howlong >= p->autologoff) {
						p->loginstart = 0;
							tris_log(LOG_NOTICE, "Agent '%s' didn't answer/confirm within %d seconds (waited %d)\n", p->name, p->autologoff, howlong);
						agent_logoff_maintenance(p, p->loginchan, (cur_time = p->loginstart), ast->uniqueid, "Autologoff");
					}
				}
				status = pbx_builtin_getvar_helper(p->chan, "CHANLOCALSTATUS");
				if (autologoffunavail && status && !strcasecmp(status, "CHANUNAVAIL")) {
					long logintime = cur_time - p->loginstart;
					p->loginstart = 0;
					tris_log(LOG_NOTICE, "Agent read: '%s' is not available now, auto logoff\n", p->name);
					agent_logoff_maintenance(p, p->loginchan, logintime, ast->uniqueid, "Chanunavail");
				}
				tris_hangup(p->chan);
				if (p->wrapuptime && p->acknowledged)
					p->lastdisc = tris_tvadd(tris_tvnow(), tris_samp2tv(p->wrapuptime, 1000));
			}
			p->chan = NULL;
			tris_devstate_changed(TRIS_DEVICE_UNAVAILABLE, "Agent/%s", p->agent);
			p->acknowledged = 0;
		}
	} else {
		/* if acknowledgement is not required, and the channel is up, we may have missed
			an TRIS_CONTROL_ANSWER (if there was one), so mark the call acknowledged anyway */
		if (!p->ackcall && !p->acknowledged && p->chan && (p->chan->_state == TRIS_STATE_UP)) {
			p->acknowledged = 1;
		}

		if (!p->acknowledged) {
			int howlong = cur_time - p->start;
			if (p->autologoff && (howlong >= p->autologoff)) {
				tris_log(LOG_NOTICE, "Agent '%s' didn't answer/confirm within %d seconds (waited %d)\n", p->name, p->autologoff, howlong);
				agent_logoff_maintenance(p, p->loginchan, (cur_time - p->loginstart), ast->uniqueid, "Autologoff");
				if (p->owner || p->chan) {
					while (p->owner && tris_channel_trylock(p->owner)) {
						DEADLOCK_AVOIDANCE(&p->lock);
					}
					if (p->owner) {
						tris_softhangup(p->owner, TRIS_SOFTHANGUP_EXPLICIT);
						tris_channel_unlock(p->owner);
					}

					while (p->chan && tris_channel_trylock(p->chan)) {
						DEADLOCK_AVOIDANCE(&p->lock);
					}
					if (p->chan) {
						tris_softhangup(p->chan, TRIS_SOFTHANGUP_EXPLICIT);
						tris_channel_unlock(p->chan);
					}
				} else {
					long logintime;
					logintime = time(NULL) - p->loginstart;
					p->loginstart = 0;
					agent_logoff_maintenance(p, p->loginchan, logintime, NULL, "CommandLogoff");
				}
			}
		}
		switch (f->frametype) {
		case TRIS_FRAME_CONTROL:
			if (f->subclass == TRIS_CONTROL_ANSWER) {
				if (p->ackcall) {
					tris_verb(3, "%s answered, waiting for '%c' to acknowledge\n", p->chan->name, p->acceptdtmf);
					/* Don't pass answer along */
					tris_frfree(f);
					f = &tris_null_frame;
				} else {
					p->acknowledged = 1;
					/* Use the builtin answer frame for the 
					   recording start check below. */
					tris_frfree(f);
					f = &answer_frame;
				}
			}
			break;
		case TRIS_FRAME_DTMF_BEGIN:
			/*ignore DTMF begin's as it can cause issues with queue announce files*/
			if((!p->acknowledged && f->subclass == p->acceptdtmf) || (f->subclass == p->enddtmf && endcall)){
				tris_frfree(f);
				f = &tris_null_frame;
			}
			break;
		case TRIS_FRAME_DTMF_END:
			if (!p->acknowledged && (f->subclass == p->acceptdtmf)) {
				tris_verb(3, "%s acknowledged\n", p->chan->name);
				p->acknowledged = 1;
				tris_frfree(f);
				f = &answer_frame;
			} else if (f->subclass == p->enddtmf && endcall) {
				/* terminates call */
				tris_frfree(f);
				f = NULL;
			}
			break;
		case TRIS_FRAME_VOICE:
		case TRIS_FRAME_VIDEO:
			/* don't pass voice or video until the call is acknowledged */
			if (!p->acknowledged) {
				tris_frfree(f);
				f = &tris_null_frame;
			}
		default:
			/* pass everything else on through */
			break;
		}
	}

	CLEANUP(ast,p);
	if (p->chan && !p->chan->_bridge) {
		if (strcasecmp(p->chan->tech->type, "Local")) {
			p->chan->_bridge = ast;
			if (p->chan)
				tris_debug(1, "Bridge on '%s' being set to '%s' (3)\n", p->chan->name, p->chan->_bridge->name);
		}
	}
	tris_mutex_unlock(&p->lock);
	if (recordagentcalls && f == &answer_frame)
		agent_start_monitoring(ast,0);
	return f;
}

static int agent_sendhtml(struct tris_channel *ast, int subclass, const char *data, int datalen)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	tris_mutex_lock(&p->lock);
	if (p->chan) 
		res = tris_channel_sendhtml(p->chan, subclass, data, datalen);
	tris_mutex_unlock(&p->lock);
	return res;
}

static int agent_sendtext(struct tris_channel *ast, const char *text)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	tris_mutex_lock(&p->lock);
	if (p->chan) 
		res = tris_sendtext(p->chan, text);
	tris_mutex_unlock(&p->lock);
	return res;
}

static int agent_write(struct tris_channel *ast, struct tris_frame *f)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	CHECK_FORMATS(ast, p);
	tris_mutex_lock(&p->lock);
	if (!p->chan) 
		res = 0;
	else {
		if ((f->frametype != TRIS_FRAME_VOICE) ||
		    (f->frametype != TRIS_FRAME_VIDEO) ||
		    (f->subclass == p->chan->writeformat)) {
			res = tris_write(p->chan, f);
		} else {
			tris_debug(1, "Dropping one incompatible %s frame on '%s' to '%s'\n", 
				f->frametype == TRIS_FRAME_VOICE ? "audio" : "video",
				ast->name, p->chan->name);
			res = 0;
		}
	}
	CLEANUP(ast, p);
	tris_mutex_unlock(&p->lock);
	return res;
}

static int agent_fixup(struct tris_channel *oldchan, struct tris_channel *newchan)
{
	struct agent_pvt *p = newchan->tech_pvt;
	tris_mutex_lock(&p->lock);
	if (p->owner != oldchan) {
		tris_log(LOG_WARNING, "old channel wasn't %p but was %p\n", oldchan, p->owner);
		tris_mutex_unlock(&p->lock);
		return -1;
	}
	p->owner = newchan;
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int agent_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	tris_mutex_lock(&p->lock);
	if (p->chan && !tris_check_hangup(p->chan)) {
		while (tris_channel_trylock(p->chan)) {
			tris_channel_unlock(ast);
			usleep(1);
			tris_channel_lock(ast);
		}
  		res = p->chan->tech->indicate ? p->chan->tech->indicate(p->chan, condition, data, datalen) : -1;
		tris_channel_unlock(p->chan);
	} else
		res = 0;
	tris_mutex_unlock(&p->lock);
	return res;
}

static int agent_digit_begin(struct tris_channel *ast, char digit)
{
	struct agent_pvt *p = ast->tech_pvt;
	tris_mutex_lock(&p->lock);
	if (p->chan) {
		tris_senddigit_begin(p->chan, digit);
	}
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int agent_digit_end(struct tris_channel *ast, char digit, unsigned int duration)
{
	struct agent_pvt *p = ast->tech_pvt;
	tris_mutex_lock(&p->lock);
	if (p->chan) {
		tris_senddigit_end(p->chan, digit, duration);
	}
	tris_mutex_unlock(&p->lock);
	return 0;
}

static int agent_call(struct tris_channel *ast, char *dest, int timeout)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	int newstate=0;
	tris_mutex_lock(&p->lock);
	p->acknowledged = 0;
	if (!p->chan) {
		if (p->pending) {
			tris_debug(1, "Pretending to dial on pending agent\n");
			newstate = TRIS_STATE_DIALING;
			res = 0;
		} else {
			tris_log(LOG_NOTICE, "Whoa, they hung up between alloc and call...  what are the odds of that?\n");
			res = -1;
		}
		tris_mutex_unlock(&p->lock);
		if (newstate)
			tris_setstate(ast, newstate);
		return res;
	} else if (!tris_strlen_zero(p->loginchan)) {
		time(&p->start);
		/* Call on this agent */
		tris_verb(3, "outgoing agentcall, to agent '%s', on '%s'\n", p->agent, p->chan->name);
		tris_set_callerid(p->chan,
			ast->cid.cid_num, ast->cid.cid_name, NULL);
		tris_channel_inherit_variables(ast, p->chan);
		res = tris_call(p->chan, p->loginchan, 0);
		CLEANUP(ast,p);
		tris_mutex_unlock(&p->lock);
		return res;
	}
	tris_verb(3, "agent_call, call to agent '%s' call on '%s'\n", p->agent, p->chan->name);
	tris_debug(3, "Playing beep, lang '%s'\n", p->chan->language);
	res = tris_streamfile(p->chan, beep, p->chan->language);
	tris_debug(3, "Played beep, result '%d'\n", res);
	if (!res) {
		res = tris_waitstream(p->chan, "");
		tris_debug(3, "Waited for stream, result '%d'\n", res);
	}
	if (!res) {
		res = tris_set_read_format(p->chan, tris_best_codec(p->chan->nativeformats));
		tris_debug(3, "Set read format, result '%d'\n", res);
		if (res)
			tris_log(LOG_WARNING, "Unable to set read format to %s\n", tris_getformatname(tris_best_codec(p->chan->nativeformats)));
	} else {
		/* Agent hung-up */
		p->chan = NULL;
		tris_devstate_changed(TRIS_DEVICE_UNAVAILABLE, "Agent/%s", p->agent);
	}

	if (!res) {
		res = tris_set_write_format(p->chan, tris_best_codec(p->chan->nativeformats));
		tris_debug(3, "Set write format, result '%d'\n", res);
		if (res)
			tris_log(LOG_WARNING, "Unable to set write format to %s\n", tris_getformatname(tris_best_codec(p->chan->nativeformats)));
	}
	if(!res) {
		/* Call is immediately up, or might need ack */
		if (p->ackcall > 1)
			newstate = TRIS_STATE_RINGING;
		else {
			newstate = TRIS_STATE_UP;
			if (recordagentcalls)
				agent_start_monitoring(ast, 0);
			p->acknowledged = 1;
		}
		res = 0;
	}
	CLEANUP(ast, p);
	tris_mutex_unlock(&p->lock);
	if (newstate)
		tris_setstate(ast, newstate);
	return res;
}

/*! \brief store/clear the global variable that stores agentid based on the callerid */
static void set_agentbycallerid(const char *callerid, const char *agent)
{
	char buf[TRIS_MAX_BUF];

	/* if there is no Caller ID, nothing to do */
	if (tris_strlen_zero(callerid))
		return;

	snprintf(buf, sizeof(buf), "%s_%s", GETAGENTBYCALLERID, callerid);
	pbx_builtin_setvar_helper(NULL, buf, agent);
}

/*! \brief return the channel or base channel if one exists.  This function assumes the channel it is called on is already locked */
struct tris_channel* agent_get_base_channel(struct tris_channel *chan)
{
	struct agent_pvt *p = NULL;
	struct tris_channel *base = chan;

	/* chan is locked by the calling function */
	if (!chan || !chan->tech_pvt) {
		tris_log(LOG_ERROR, "whoa, you need a channel (0x%ld) with a tech_pvt (0x%ld) to get a base channel.\n", (long)chan, (chan)?(long)chan->tech_pvt:(long)NULL);
		return NULL;
	}
	p = chan->tech_pvt;
	if (p->chan) 
		base = p->chan;
	return base;
}

int agent_set_base_channel(struct tris_channel *chan, struct tris_channel *base)
{
	struct agent_pvt *p = NULL;
	
	if (!chan || !base) {
		tris_log(LOG_ERROR, "whoa, you need a channel (0x%ld) and a base channel (0x%ld) for setting.\n", (long)chan, (long)base);
		return -1;
	}
	p = chan->tech_pvt;
	if (!p) {
		tris_log(LOG_ERROR, "whoa, channel %s is missing his tech_pvt structure!!.\n", chan->name);
		return -1;
	}
	p->chan = base;
	return 0;
}

static int agent_hangup(struct tris_channel *ast)
{
	struct agent_pvt *p = ast->tech_pvt;
	int howlong = 0;
	const char *status;
	tris_mutex_lock(&p->lock);
	p->owner = NULL;
	ast->tech_pvt = NULL;
	p->app_sleep_cond = 1;
	p->acknowledged = 0;

	/* if they really are hung up then set start to 0 so the test
	 * later if we're called on an already downed channel
	 * doesn't cause an agent to be logged out like when
	 * agent_request() is followed immediately by agent_hangup()
	 * as in apps/app_chanisavail.c:chanavail_exec()
	 */

	tris_debug(1, "Hangup called for state %s\n", tris_state2str(ast->_state));
	if (p->start && (ast->_state != TRIS_STATE_UP)) {
		howlong = time(NULL) - p->start;
		p->start = 0;
	} else if (ast->_state == TRIS_STATE_RESERVED) 
		howlong = 0;
	else
		p->start = 0; 
	if (p->chan) {
		p->chan->_bridge = NULL;
		/* If they're dead, go ahead and hang up on the agent now */
		if (!tris_strlen_zero(p->loginchan)) {
			/* Store last disconnect time */
			if (p->wrapuptime)
				p->lastdisc = tris_tvadd(tris_tvnow(), tris_samp2tv(p->wrapuptime, 1000));
			else
				p->lastdisc = tris_tv(0,0);
			if (p->chan) {
				status = pbx_builtin_getvar_helper(p->chan, "CHANLOCALSTATUS");
				if (autologoffunavail && status && !strcasecmp(status, "CHANUNAVAIL")) {
					long logintime = time(NULL) - p->loginstart;
					p->loginstart = 0;
					tris_log(LOG_NOTICE, "Agent hangup: '%s' is not available now, auto logoff\n", p->name);
					agent_logoff_maintenance(p, p->loginchan, logintime, ast->uniqueid, "Chanunavail");
				}
				/* Recognize the hangup and pass it along immediately */
				tris_hangup(p->chan);
				p->chan = NULL;
				tris_devstate_changed(TRIS_DEVICE_UNAVAILABLE, "Agent/%s", p->agent);
			}
			tris_debug(1, "Hungup, howlong is %d, autologoff is %d\n", howlong, p->autologoff);
			if ((p->deferlogoff) || (howlong && p->autologoff && (howlong > p->autologoff))) {
				long logintime = time(NULL) - p->loginstart;
				p->loginstart = 0;
				if (!p->deferlogoff)
					tris_log(LOG_NOTICE, "Agent '%s' didn't answer/confirm within %d seconds (waited %d)\n", p->name, p->autologoff, howlong);
				p->deferlogoff = 0;
				agent_logoff_maintenance(p, p->loginchan, logintime, ast->uniqueid, "Autologoff");
				if (persistent_agents)
					dump_agents();
			}
		} else if (p->dead) {
			tris_channel_lock(p->chan);
			tris_softhangup(p->chan, TRIS_SOFTHANGUP_EXPLICIT);
			tris_channel_unlock(p->chan);
		} else if (p->loginstart) {
			tris_channel_lock(p->chan);
			tris_indicate_data(p->chan, TRIS_CONTROL_HOLD, 
				S_OR(p->moh, NULL),
				!tris_strlen_zero(p->moh) ? strlen(p->moh) + 1 : 0);
			tris_channel_unlock(p->chan);
		}
	}
	tris_mutex_unlock(&p->lock);

	/* Only register a device state change if the agent is still logged in */
	if (!p->loginstart) {
		p->loginchan[0] = '\0';
		p->logincallerid[0] = '\0';
		if (persistent_agents)
			dump_agents();
	} else {
		tris_devstate_changed(TRIS_DEVICE_NOT_INUSE, "Agent/%s", p->agent);
	}

	if (p->pending) {
		TRIS_LIST_LOCK(&agents);
		TRIS_LIST_REMOVE(&agents, p, list);
		TRIS_LIST_UNLOCK(&agents);
	}
	if (p->abouttograb) {
		/* Let the "about to grab" thread know this isn't valid anymore, and let it
		   kill it later */
		p->abouttograb = 0;
	} else if (p->dead) {
		tris_mutex_destroy(&p->lock);
		tris_mutex_destroy(&p->app_lock);
		tris_cond_destroy(&p->app_complete_cond);
		tris_free(p);
	} else {
		if (p->chan) {
			/* Not dead -- check availability now */
			tris_mutex_lock(&p->lock);
			/* Store last disconnect time */
			p->lastdisc = tris_tvadd(tris_tvnow(), tris_samp2tv(p->wrapuptime, 1000));
			tris_mutex_unlock(&p->lock);
		}
		/* Release ownership of the agent to other threads (presumably running the login app). */
		if (tris_strlen_zero(p->loginchan)) {
			p->app_lock_flag = 0;
			tris_cond_signal(&p->app_complete_cond);
		}
	}
	return 0;
}

static int agent_cont_sleep( void *data )
{
	struct agent_pvt *p;
	int res;

	p = (struct agent_pvt *)data;

	tris_mutex_lock(&p->lock);
	res = p->app_sleep_cond;
	if (p->lastdisc.tv_sec) {
		if (tris_tvdiff_ms(tris_tvnow(), p->lastdisc) > 0) 
			res = 1;
	}
	tris_mutex_unlock(&p->lock);

	if (!res)
		tris_debug(5, "agent_cont_sleep() returning %d\n", res );

	return res;
}

static int agent_ack_sleep(void *data)
{
	struct agent_pvt *p;
	int res=0;
	int to = 1000;
	struct tris_frame *f;

	/* Wait a second and look for something */

	p = (struct agent_pvt *) data;
	if (!p->chan) 
		return -1;

	for(;;) {
		to = tris_waitfor(p->chan, to);
		if (to < 0) 
			return -1;
		if (!to) 
			return 0;
		f = tris_read(p->chan);
		if (!f) 
			return -1;
		if (f->frametype == TRIS_FRAME_DTMF)
			res = f->subclass;
		else
			res = 0;
		tris_frfree(f);
		tris_mutex_lock(&p->lock);
		if (!p->app_sleep_cond) {
			tris_mutex_unlock(&p->lock);
			return 0;
		} else if (res == p->acceptdtmf) {
			tris_mutex_unlock(&p->lock);
			return 1;
		}
		tris_mutex_unlock(&p->lock);
		res = 0;
	}
	return res;
}

static struct tris_channel *agent_bridgedchannel(struct tris_channel *chan, struct tris_channel *bridge)
{
	struct agent_pvt *p = bridge->tech_pvt;
	struct tris_channel *ret = NULL;

	if (p) {
		if (chan == p->chan)
			ret = bridge->_bridge;
		else if (chan == bridge->_bridge)
			ret = p->chan;
	}

	tris_debug(1, "Asked for bridged channel on '%s'/'%s', returning '%s'\n", chan->name, bridge->name, ret ? ret->name : "<none>");
	return ret;
}

/*! \brief Create new agent channel */
static struct tris_channel *agent_new(struct agent_pvt *p, int state)
{
	struct tris_channel *tmp;
	int alreadylocked;
#if 0
	if (!p->chan) {
		tris_log(LOG_WARNING, "No channel? :(\n");
		return NULL;
	}
#endif	
	if (p->pending)
		tmp = tris_channel_alloc(0, state, 0, 0, "", p->chan ? p->chan->exten:"", p->chan ? p->chan->context:"", 0, "Agent/P%s-%d", p->agent, (int) tris_random() & 0xffff);
	else
		tmp = tris_channel_alloc(0, state, 0, 0, "", p->chan ? p->chan->exten:"", p->chan ? p->chan->context:"", 0, "Agent/%s", p->agent);
	if (!tmp) {
		tris_log(LOG_WARNING, "Unable to allocate agent channel structure\n");
		return NULL;
	}

	tmp->tech = &agent_tech;
	if (p->chan) {
		tmp->nativeformats = p->chan->nativeformats;
		tmp->writeformat = p->chan->writeformat;
		tmp->rawwriteformat = p->chan->writeformat;
		tmp->readformat = p->chan->readformat;
		tmp->rawreadformat = p->chan->readformat;
		tris_string_field_set(tmp, language, p->chan->language);
		tris_copy_string(tmp->context, p->chan->context, sizeof(tmp->context));
		tris_copy_string(tmp->exten, p->chan->exten, sizeof(tmp->exten));
		/* XXX Is this really all we copy form the originating channel?? */
	} else {
		tmp->nativeformats = TRIS_FORMAT_SLINEAR;
		tmp->writeformat = TRIS_FORMAT_SLINEAR;
		tmp->rawwriteformat = TRIS_FORMAT_SLINEAR;
		tmp->readformat = TRIS_FORMAT_SLINEAR;
		tmp->rawreadformat = TRIS_FORMAT_SLINEAR;
	}
	/* Safe, agentlock already held */
	tmp->tech_pvt = p;
	p->owner = tmp;
	tmp->priority = 1;
	/* Wake up and wait for other applications (by definition the login app)
	 * to release this channel). Takes ownership of the agent channel
	 * to this thread only.
	 * For signalling the other thread, tris_queue_frame is used until we
	 * can safely use signals for this purpose. The pselect() needs to be
	 * implemented in the kernel for this.
	 */
	p->app_sleep_cond = 0;

	alreadylocked = p->app_lock_flag;
	p->app_lock_flag = 1;

	if(tris_strlen_zero(p->loginchan) && alreadylocked) {
		if (p->chan) {
			tris_queue_frame(p->chan, &tris_null_frame);
			tris_mutex_unlock(&p->lock);	/* For other thread to read the condition. */
			p->app_lock_flag = 1;
			tris_mutex_lock(&p->lock);
		} else {
			tris_log(LOG_WARNING, "Agent disconnected while we were connecting the call\n");
			p->owner = NULL;
			tmp->tech_pvt = NULL;
			p->app_sleep_cond = 1;
			tris_channel_free( tmp );
			tris_mutex_unlock(&p->lock);	/* For other thread to read the condition. */
			p->app_lock_flag = 0;
			tris_cond_signal(&p->app_complete_cond);
			return NULL;
		}
	} else if (!tris_strlen_zero(p->loginchan)) {
		if (p->chan)
			tris_queue_frame(p->chan, &tris_null_frame);
		if (!p->chan) {
			tris_log(LOG_WARNING, "Agent disconnected while we were connecting the call\n");
			p->owner = NULL;
			tmp->tech_pvt = NULL;
			p->app_sleep_cond = 1;
			tris_channel_free( tmp );
			tris_mutex_unlock(&p->lock);     /* For other thread to read the condition. */
			return NULL;
		}	
	} 
	if (p->chan)
		tris_indicate(p->chan, TRIS_CONTROL_UNHOLD);
	return tmp;
}


/*!
 * Read configuration data. The file named agents.conf.
 *
 * \returns Always 0, or so it seems.
 */
static int read_agent_config(int reload)
{
	struct tris_config *cfg;
	struct tris_config *ucfg;
	struct tris_variable *v;
	struct agent_pvt *p;
	const char *general_val;
	const char *catname;
	const char *hasagent;
	int genhasagent;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	group = 0;
	autologoff = 0;
	wrapuptime = 0;
	ackcall = 0;
	endcall = 1;
	cfg = tris_config_load(config, config_flags);
	if (!cfg) {
		tris_log(LOG_NOTICE, "No agent configuration found -- agent support disabled\n");
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "%s contains a parsing error.  Aborting\n", config);
		return 0;
	}
	if ((ucfg = tris_config_load("users.conf", config_flags))) {
		if (ucfg == CONFIG_STATUS_FILEUNCHANGED) {
			ucfg = NULL;
		} else if (ucfg == CONFIG_STATUS_FILEINVALID) {
			tris_log(LOG_ERROR, "users.conf contains a parsing error.  Aborting\n");
			return 0;
		}
	}

	TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		p->dead = 1;
	}
	strcpy(moh, "default");
	/* set the default recording values */
	recordagentcalls = 0;
	strcpy(recordformat, "wav");
	strcpy(recordformatext, "wav");
	urlprefix[0] = '\0';
	savecallsin[0] = '\0';

	/* Read in [general] section for persistence */
	if ((general_val = tris_variable_retrieve(cfg, "general", "persistentagents")))
		persistent_agents = tris_true(general_val);
	multiplelogin = tris_true(tris_variable_retrieve(cfg, "general", "multiplelogin"));

	/* Read in the [agents] section */
	v = tris_variable_browse(cfg, "agents");
	while(v) {
		/* Create the interface list */
		if (!strcasecmp(v->name, "agent")) {
			add_agent(v->value, 0);
		} else if (!strcasecmp(v->name, "group")) {
			group = tris_get_group(v->value);
		} else if (!strcasecmp(v->name, "autologoff")) {
			autologoff = atoi(v->value);
			if (autologoff < 0)
				autologoff = 0;
		} else if (!strcasecmp(v->name, "ackcall")) {
			if (!strcasecmp(v->value, "always"))
				ackcall = 2;
			else if (tris_true(v->value))
				ackcall = 1;
			else
				ackcall = 0;
		} else if (!strcasecmp(v->name, "endcall")) {
			endcall = tris_true(v->value);
		} else if (!strcasecmp(v->name, "acceptdtmf")) {
			acceptdtmf = *(v->value);
			tris_log(LOG_NOTICE, "Set acceptdtmf to %c\n", acceptdtmf);
		} else if (!strcasecmp(v->name, "enddtmf")) {
			enddtmf = *(v->value);
		} else if (!strcasecmp(v->name, "wrapuptime")) {
			wrapuptime = atoi(v->value);
			if (wrapuptime < 0)
				wrapuptime = 0;
		} else if (!strcasecmp(v->name, "maxlogintries") && !tris_strlen_zero(v->value)) {
			maxlogintries = atoi(v->value);
			if (maxlogintries < 0)
				maxlogintries = 0;
		} else if (!strcasecmp(v->name, "goodbye") && !tris_strlen_zero(v->value)) {
			strcpy(agentgoodbye,v->value);
		} else if (!strcasecmp(v->name, "musiconhold")) {
			tris_copy_string(moh, v->value, sizeof(moh));
		} else if (!strcasecmp(v->name, "updatecdr")) {
			if (tris_true(v->value))
				updatecdr = 1;
			else
				updatecdr = 0;
		} else if (!strcasecmp(v->name, "autologoffunavail")) {
			if (tris_true(v->value))
				autologoffunavail = 1;
			else
				autologoffunavail = 0;
		} else if (!strcasecmp(v->name, "recordagentcalls")) {
			recordagentcalls = tris_true(v->value);
		} else if (!strcasecmp(v->name, "recordformat")) {
			tris_copy_string(recordformat, v->value, sizeof(recordformat));
			if (!strcasecmp(v->value, "wav49"))
				strcpy(recordformatext, "WAV");
			else
				tris_copy_string(recordformatext, v->value, sizeof(recordformatext));
		} else if (!strcasecmp(v->name, "urlprefix")) {
			tris_copy_string(urlprefix, v->value, sizeof(urlprefix));
			if (urlprefix[strlen(urlprefix) - 1] != '/')
				strncat(urlprefix, "/", sizeof(urlprefix) - strlen(urlprefix) - 1);
		} else if (!strcasecmp(v->name, "savecallsin")) {
			if (v->value[0] == '/')
				tris_copy_string(savecallsin, v->value, sizeof(savecallsin));
			else
				snprintf(savecallsin, sizeof(savecallsin) - 2, "/%s", v->value);
			if (savecallsin[strlen(savecallsin) - 1] != '/')
				strncat(savecallsin, "/", sizeof(savecallsin) - strlen(savecallsin) - 1);
		} else if (!strcasecmp(v->name, "custom_beep")) {
			tris_copy_string(beep, v->value, sizeof(beep));
		}
		v = v->next;
	}
	if (ucfg) {
		genhasagent = tris_true(tris_variable_retrieve(ucfg, "general", "hasagent"));
		catname = tris_category_browse(ucfg, NULL);
		while(catname) {
			if (strcasecmp(catname, "general")) {
				hasagent = tris_variable_retrieve(ucfg, catname, "hasagent");
				if (tris_true(hasagent) || (!hasagent && genhasagent)) {
					char tmp[256];
					const char *fullname = tris_variable_retrieve(ucfg, catname, "fullname");
					const char *secret = tris_variable_retrieve(ucfg, catname, "secret");
					if (!fullname)
						fullname = "";
					if (!secret)
						secret = "";
					snprintf(tmp, sizeof(tmp), "%s,%s,%s", catname, secret,fullname);
					add_agent(tmp, 0);
				}
			}
			catname = tris_category_browse(ucfg, catname);
		}
		tris_config_destroy(ucfg);
	}
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&agents, p, list) {
		if (p->dead) {
			TRIS_LIST_REMOVE_CURRENT(list);
			/* Destroy if  appropriate */
			if (!p->owner) {
				if (!p->chan) {
					tris_mutex_destroy(&p->lock);
					tris_mutex_destroy(&p->app_lock);
					tris_cond_destroy(&p->app_complete_cond);
					tris_free(p);
				} else {
					/* Cause them to hang up */
					tris_softhangup(p->chan, TRIS_SOFTHANGUP_EXPLICIT);
				}
			}
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END;
	TRIS_LIST_UNLOCK(&agents);
	tris_config_destroy(cfg);
	return 1;
}

static int check_availability(struct agent_pvt *newlyavailable, int needlock)
{
	struct tris_channel *chan=NULL, *parent=NULL;
	struct agent_pvt *p;
	int res;

	tris_debug(1, "Checking availability of '%s'\n", newlyavailable->agent);
	if (needlock)
		TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		if (p == newlyavailable) {
			continue;
		}
		tris_mutex_lock(&p->lock);
		if (!p->abouttograb && p->pending && ((p->group && (newlyavailable->group & p->group)) || !strcmp(p->agent, newlyavailable->agent))) {
			tris_debug(1, "Call '%s' looks like a winner for agent '%s'\n", p->owner->name, newlyavailable->agent);
			/* We found a pending call, time to merge */
			chan = agent_new(newlyavailable, TRIS_STATE_DOWN);
			parent = p->owner;
			p->abouttograb = 1;
			tris_mutex_unlock(&p->lock);
			break;
		}
		tris_mutex_unlock(&p->lock);
	}
	if (needlock)
		TRIS_LIST_UNLOCK(&agents);
	if (parent && chan)  {
		if (newlyavailable->ackcall > 1) {
			/* Don't do beep here */
			res = 0;
		} else {
			tris_debug(3, "Playing beep, lang '%s'\n", newlyavailable->chan->language);
			res = tris_streamfile(newlyavailable->chan, beep, newlyavailable->chan->language);
			tris_debug(3, "Played beep, result '%d'\n", res);
			if (!res) {
				res = tris_waitstream(newlyavailable->chan, "");
				tris_debug(1, "Waited for stream, result '%d'\n", res);
			}
		}
		if (!res) {
			/* Note -- parent may have disappeared */
			if (p->abouttograb) {
				newlyavailable->acknowledged = 1;
				/* Safe -- agent lock already held */
				tris_setstate(parent, TRIS_STATE_UP);
				tris_setstate(chan, TRIS_STATE_UP);
				tris_copy_string(parent->context, chan->context, sizeof(parent->context));
				/* Go ahead and mark the channel as a zombie so that masquerade will
				   destroy it for us, and we need not call tris_hangup */
				tris_set_flag(chan, TRIS_FLAG_ZOMBIE);
				tris_channel_masquerade(parent, chan);
				p->abouttograb = 0;
			} else {
				tris_debug(1, "Sneaky, parent disappeared in the mean time...\n");
				agent_cleanup(newlyavailable);
			}
		} else {
			tris_debug(1, "Ugh...  Agent hung up at exactly the wrong time\n");
			agent_cleanup(newlyavailable);
		}
	}
	return 0;
}

static int check_beep(struct agent_pvt *newlyavailable, int needlock)
{
	struct agent_pvt *p;
	int res=0;

	tris_debug(1, "Checking beep availability of '%s'\n", newlyavailable->agent);
	if (needlock)
		TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		if (p == newlyavailable) {
			continue;
		}
		tris_mutex_lock(&p->lock);
		if (!p->abouttograb && p->pending && ((p->group && (newlyavailable->group & p->group)) || !strcmp(p->agent, newlyavailable->agent))) {
			tris_debug(1, "Call '%s' looks like a would-be winner for agent '%s'\n", p->owner->name, newlyavailable->agent);
			tris_mutex_unlock(&p->lock);
			break;
		}
		tris_mutex_unlock(&p->lock);
	}
	if (needlock)
		TRIS_LIST_UNLOCK(&agents);
	if (p) {
		tris_mutex_unlock(&newlyavailable->lock);
		tris_debug(3, "Playing beep, lang '%s'\n", newlyavailable->chan->language);
		res = tris_streamfile(newlyavailable->chan, beep, newlyavailable->chan->language);
		tris_debug(1, "Played beep, result '%d'\n", res);
		if (!res) {
			res = tris_waitstream(newlyavailable->chan, "");
			tris_debug(1, "Waited for stream, result '%d'\n", res);
		}
		tris_mutex_lock(&newlyavailable->lock);
	}
	return res;
}

/*! \brief Part of the Trismedia interface */
static struct tris_channel *agent_request(const char *type, int format, void *data, int *cause, struct tris_channel* src)
{
	struct agent_pvt *p;
	struct tris_channel *chan = NULL;
	char *s;
	tris_group_t groupmatch;
	int groupoff;
	int waitforagent=0;
	int hasagent = 0;
	struct timeval now;

	s = data;
	if ((s[0] == '@') && (sscanf(s + 1, "%30d", &groupoff) == 1)) {
		groupmatch = (1 << groupoff);
	} else if ((s[0] == ':') && (sscanf(s + 1, "%30d", &groupoff) == 1)) {
		groupmatch = (1 << groupoff);
		waitforagent = 1;
	} else 
		groupmatch = 0;

	/* Check actual logged in agents first */
	TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		tris_mutex_lock(&p->lock);
		if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent)) &&
		    tris_strlen_zero(p->loginchan)) {
			if (p->chan)
				hasagent++;
			now = tris_tvnow();
			if (!p->lastdisc.tv_sec || (now.tv_sec >= p->lastdisc.tv_sec)) {
				p->lastdisc = tris_tv(0, 0);
				/* Agent must be registered, but not have any active call, and not be in a waiting state */
				if (!p->owner && p->chan) {
					/* Fixed agent */
					chan = agent_new(p, TRIS_STATE_DOWN);
				}
				if (chan) {
					tris_mutex_unlock(&p->lock);
					break;
				}
			}
		}
		tris_mutex_unlock(&p->lock);
	}
	if (!p) {
		TRIS_LIST_TRAVERSE(&agents, p, list) {
			tris_mutex_lock(&p->lock);
			if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent))) {
				if (p->chan || !tris_strlen_zero(p->loginchan))
					hasagent++;
				now = tris_tvnow();
#if 0
				tris_log(LOG_NOTICE, "Time now: %ld, Time of lastdisc: %ld\n", now.tv_sec, p->lastdisc.tv_sec);
#endif
				if (!p->lastdisc.tv_sec || (now.tv_sec >= p->lastdisc.tv_sec)) {
					p->lastdisc = tris_tv(0, 0);
					/* Agent must be registered, but not have any active call, and not be in a waiting state */
					if (!p->owner && p->chan) {
						/* Could still get a fixed agent */
						chan = agent_new(p, TRIS_STATE_DOWN);
					} else if (!p->owner && !tris_strlen_zero(p->loginchan)) {
						/* Adjustable agent */
						p->chan = tris_request("Local", format, p->loginchan, cause, 0);
						if (p->chan)
							chan = agent_new(p, TRIS_STATE_DOWN);
					}
					if (chan) {
						tris_mutex_unlock(&p->lock);
						break;
					}
				}
			}
			tris_mutex_unlock(&p->lock);
		}
	}

	if (!chan && waitforagent) {
		/* No agent available -- but we're requesting to wait for one.
		   Allocate a place holder */
		if (hasagent) {
			tris_debug(1, "Creating place holder for '%s'\n", s);
			p = add_agent(data, 1);
			p->group = groupmatch;
			chan = agent_new(p, TRIS_STATE_DOWN);
			if (!chan) 
				tris_log(LOG_WARNING, "Weird...  Fix this to drop the unused pending agent\n");
		} else {
			tris_debug(1, "Not creating place holder for '%s' since nobody logged in\n", s);
		}
	}
	*cause = hasagent ? TRIS_CAUSE_BUSY : TRIS_CAUSE_UNREGISTERED;
	TRIS_LIST_UNLOCK(&agents);
	return chan;
}

static force_inline int powerof(unsigned int d)
{
	int x = ffs(d);

	if (x)
		return x - 1;

	return 0;
}

/*!
 * Lists agents and their status to the Manager API.
 * It is registered on load_module() and it gets called by the manager backend.
 * \param s
 * \param m
 * \returns 
 * \sa action_agent_logoff(), load_module().
 */
static int action_agents(struct mansession *s, const struct message *m)
{
	const char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	char chanbuf[256];
	struct agent_pvt *p;
	char *username = NULL;
	char *loginChan = NULL;
	char *talkingto = NULL;
	char *talkingtoChan = NULL;
	char *status = NULL;

	if (!tris_strlen_zero(id))
		snprintf(idText, sizeof(idText) ,"ActionID: %s\r\n", id);
	astman_send_ack(s, m, "Agents will follow");
	TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
        	tris_mutex_lock(&p->lock);

		/* Status Values:
		   AGENT_LOGGEDOFF - Agent isn't logged in
		   AGENT_IDLE      - Agent is logged in, and waiting for call
		   AGENT_ONCALL    - Agent is logged in, and on a call
		   AGENT_UNKNOWN   - Don't know anything about agent. Shouldn't ever get this. */

		username = S_OR(p->name, "None");

		/* Set a default status. It 'should' get changed. */
		status = "AGENT_UNKNOWN";

		if (!tris_strlen_zero(p->loginchan) && !p->chan) {
			loginChan = p->loginchan;
			talkingto = "n/a";
			talkingtoChan = "n/a";
			status = "AGENT_IDLE";
			if (p->acknowledged) {
				snprintf(chanbuf, sizeof(chanbuf), " %s (Confirmed)", p->loginchan);
				loginChan = chanbuf;
			}
		} else if (p->chan) {
			loginChan = tris_strdupa(p->chan->name);
			if (p->owner && p->owner->_bridge) {
				talkingto = p->chan->cid.cid_num;
				if (tris_bridged_channel(p->owner))
					talkingtoChan = tris_strdupa(tris_bridged_channel(p->owner)->name);
				else
					talkingtoChan = "n/a";
        			status = "AGENT_ONCALL";
			} else {
				talkingto = "n/a";
				talkingtoChan = "n/a";
        			status = "AGENT_IDLE";
			}
		} else {
			loginChan = "n/a";
			talkingto = "n/a";
			talkingtoChan = "n/a";
			status = "AGENT_LOGGEDOFF";
		}

		astman_append(s, "Event: Agents\r\n"
			"Agent: %s\r\n"
			"Name: %s\r\n"
			"Status: %s\r\n"
			"LoggedInChan: %s\r\n"
			"LoggedInTime: %d\r\n"
			"TalkingTo: %s\r\n"
			"TalkingToChan: %s\r\n"
			"%s"
			"\r\n",
			p->agent, username, status, loginChan, (int)p->loginstart, talkingto, talkingtoChan, idText);
		tris_mutex_unlock(&p->lock);
	}
	TRIS_LIST_UNLOCK(&agents);
	astman_append(s, "Event: AgentsComplete\r\n"
		"%s"
		"\r\n",idText);
	return 0;
}

static void agent_logoff_maintenance(struct agent_pvt *p, char *loginchan, long logintime, const char *uniqueid, char *logcommand)
{
	char *tmp = NULL;
	char agent[TRIS_MAX_AGENT];

	if (!tris_strlen_zero(logcommand))
		tmp = logcommand;
	else
		tmp = tris_strdupa("");

	snprintf(agent, sizeof(agent), "Agent/%s", p->agent);

	if (!tris_strlen_zero(uniqueid)) {
		manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogoff",
				"Agent: %s\r\n"
				"Reason: %s\r\n"
				"Loginchan: %s\r\n"
				"Logintime: %ld\r\n"
				"Uniqueid: %s\r\n", 
				p->agent, tmp, loginchan, logintime, uniqueid);
	} else {
		manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogoff",
				"Agent: %s\r\n"
				"Reason: %s\r\n"
				"Loginchan: %s\r\n"
				"Logintime: %ld\r\n",
				p->agent, tmp, loginchan, logintime);
	}

	tris_queue_log("NONE", tris_strlen_zero(uniqueid) ? "NONE" : uniqueid, agent, "AGENTCALLBACKLOGOFF", "%s|%ld|%s", loginchan, logintime, tmp);
	set_agentbycallerid(p->logincallerid, NULL);
	p->loginchan[0] ='\0';
	p->logincallerid[0] = '\0';
	tris_devstate_changed(TRIS_DEVICE_UNAVAILABLE, "Agent/%s", p->agent);
	if (persistent_agents)
		dump_agents();

}

static int agent_logoff(const char *agent, int soft)
{
	struct agent_pvt *p;
	long logintime;
	int ret = -1; /* Return -1 if no agent if found */

	TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		if (!strcasecmp(p->agent, agent)) {
			ret = 0;
			if (p->owner || p->chan) {
				if (!soft) {
					tris_mutex_lock(&p->lock);

					while (p->owner && tris_channel_trylock(p->owner)) {
						DEADLOCK_AVOIDANCE(&p->lock);
					}
					if (p->owner) {
						tris_softhangup(p->owner, TRIS_SOFTHANGUP_EXPLICIT);
						tris_channel_unlock(p->owner);
					}

					while (p->chan && tris_channel_trylock(p->chan)) {
						DEADLOCK_AVOIDANCE(&p->lock);
					}
					if (p->chan) {
						tris_softhangup(p->chan, TRIS_SOFTHANGUP_EXPLICIT);
						tris_channel_unlock(p->chan);
					}

					tris_mutex_unlock(&p->lock);
				} else
					p->deferlogoff = 1;
			} else {
				logintime = time(NULL) - p->loginstart;
				p->loginstart = 0;
				agent_logoff_maintenance(p, p->loginchan, logintime, NULL, "CommandLogoff");
			}
			break;
		}
	}
	TRIS_LIST_UNLOCK(&agents);

	return ret;
}

static char *agent_logoff_cmd(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	int ret;
	char *agent;

	switch (cmd) {
	case CLI_INIT:
		e->command = "agent logoff";
		e->usage =
			"Usage: agent logoff <channel> [soft]\n"
			"       Sets an agent as no longer logged in.\n"
			"       If 'soft' is specified, do not hangup existing calls.\n";
		return NULL;
	case CLI_GENERATE:
		return complete_agent_logoff_cmd(a->line, a->word, a->pos, a->n); 
	}

	if (a->argc < 3 || a->argc > 4)
		return CLI_SHOWUSAGE;
	if (a->argc == 4 && strcasecmp(a->argv[3], "soft"))
		return CLI_SHOWUSAGE;

	agent = a->argv[2] + 6;
	ret = agent_logoff(agent, a->argc == 4);
	if (ret == 0)
		tris_cli(a->fd, "Logging out %s\n", agent);

	return CLI_SUCCESS;
}

/*!
 * Sets an agent as no longer logged in in the Manager API.
 * It is registered on load_module() and it gets called by the manager backend.
 * \param s
 * \param m
 * \returns 
 * \sa action_agents(), load_module().
 */
static int action_agent_logoff(struct mansession *s, const struct message *m)
{
	const char *agent = astman_get_header(m, "Agent");
	const char *soft_s = astman_get_header(m, "Soft"); /* "true" is don't hangup */
	int soft;
	int ret; /* return value of agent_logoff */

	if (tris_strlen_zero(agent)) {
		astman_send_error(s, m, "No agent specified");
		return 0;
	}

	soft = tris_true(soft_s) ? 1 : 0;
	ret = agent_logoff(agent, soft);
	if (ret == 0)
		astman_send_ack(s, m, "Agent logged out");
	else
		astman_send_error(s, m, "No such agent");

	return 0;
}

static char *complete_agent_logoff_cmd(const char *line, const char *word, int pos, int state)
{
	char *ret = NULL;

	if (pos == 2) {
		struct agent_pvt *p;
		char name[TRIS_MAX_AGENT];
		int which = 0, len = strlen(word);

		TRIS_LIST_LOCK(&agents);
		TRIS_LIST_TRAVERSE(&agents, p, list) {
			snprintf(name, sizeof(name), "Agent/%s", p->agent);
			if (!strncasecmp(word, name, len) && p->loginstart && ++which > state) {
				ret = tris_strdup(name);
				break;
			}
		}
		TRIS_LIST_UNLOCK(&agents);
	} else if (pos == 3 && state == 0) 
		return tris_strdup("soft");
	
	return ret;
}

/*!
 * Show agents in cli.
 */
static char *agents_show(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct agent_pvt *p;
	char username[TRIS_MAX_BUF];
	char location[TRIS_MAX_BUF] = "";
	char talkingto[TRIS_MAX_BUF] = "";
	char music[TRIS_MAX_BUF];
	int count_agents = 0;		/*!< Number of agents configured */
	int online_agents = 0;		/*!< Number of online agents */
	int offline_agents = 0;		/*!< Number of offline agents */

	switch (cmd) {
	case CLI_INIT:
		e->command = "agent show";
		e->usage =
			"Usage: agent show\n"
			"       Provides summary information on agents.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 2)
		return CLI_SHOWUSAGE;

	TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		tris_mutex_lock(&p->lock);
		if (p->pending) {
			if (p->group)
				tris_cli(a->fd, "-- Pending call to group %d\n", powerof(p->group));
			else
				tris_cli(a->fd, "-- Pending call to agent %s\n", p->agent);
		} else {
			if (!tris_strlen_zero(p->name))
				snprintf(username, sizeof(username), "(%s) ", p->name);
			else
				username[0] = '\0';
			if (p->chan) {
				snprintf(location, sizeof(location), "logged in on %s", p->chan->name);
				if (p->owner && tris_bridged_channel(p->owner))
					snprintf(talkingto, sizeof(talkingto), " talking to %s", tris_bridged_channel(p->owner)->name);
				 else 
					strcpy(talkingto, " is idle");
				online_agents++;
			} else if (!tris_strlen_zero(p->loginchan)) {
				if (tris_tvdiff_ms(tris_tvnow(), p->lastdisc) > 0 || !(p->lastdisc.tv_sec)) 
					snprintf(location, sizeof(location) - 20, "available at '%s'", p->loginchan);
				else 
					snprintf(location, sizeof(location) - 20, "wrapping up at '%s'", p->loginchan);
				talkingto[0] = '\0';
				online_agents++;
				if (p->acknowledged)
					strncat(location, " (Confirmed)", sizeof(location) - strlen(location) - 1);
			} else {
				strcpy(location, "not logged in");
				talkingto[0] = '\0';
				offline_agents++;
			}
			if (!tris_strlen_zero(p->moh))
				snprintf(music, sizeof(music), " (musiconhold is '%s')", p->moh);
			tris_cli(a->fd, "%-12.12s %s%s%s%s\n", p->agent, 
				username, location, talkingto, music);
			count_agents++;
		}
		tris_mutex_unlock(&p->lock);
	}
	TRIS_LIST_UNLOCK(&agents);
	if ( !count_agents ) 
		tris_cli(a->fd, "No Agents are configured in %s\n",config);
	else 
		tris_cli(a->fd, "%d agents configured [%d online , %d offline]\n",count_agents, online_agents, offline_agents);
	tris_cli(a->fd, "\n");
	                
	return CLI_SUCCESS;
}


static char *agents_show_online(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct agent_pvt *p;
	char username[TRIS_MAX_BUF];
	char location[TRIS_MAX_BUF] = "";
	char talkingto[TRIS_MAX_BUF] = "";
	char music[TRIS_MAX_BUF];
	int count_agents = 0;           /* Number of agents configured */
	int online_agents = 0;          /* Number of online agents */
	int agent_status = 0;           /* 0 means offline, 1 means online */

	switch (cmd) {
	case CLI_INIT:
		e->command = "agent show online";
		e->usage =
			"Usage: agent show online\n"
			"       Provides a list of all online agents.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		agent_status = 0;       /* reset it to offline */
		tris_mutex_lock(&p->lock);
		if (!tris_strlen_zero(p->name))
			snprintf(username, sizeof(username), "(%s) ", p->name);
		else
			username[0] = '\0';
		if (p->chan) {
			snprintf(location, sizeof(location), "logged in on %s", p->chan->name);
			if (p->owner && tris_bridged_channel(p->owner)) 
				snprintf(talkingto, sizeof(talkingto), " talking to %s", tris_bridged_channel(p->owner)->name);
			else 
				strcpy(talkingto, " is idle");
			agent_status = 1;
			online_agents++;
		} else if (!tris_strlen_zero(p->loginchan)) {
			snprintf(location, sizeof(location) - 20, "available at '%s'", p->loginchan);
			talkingto[0] = '\0';
			agent_status = 1;
			online_agents++;
			if (p->acknowledged)
				strncat(location, " (Confirmed)", sizeof(location) - strlen(location) - 1);
		}
		if (!tris_strlen_zero(p->moh))
			snprintf(music, sizeof(music), " (musiconhold is '%s')", p->moh);
		if (agent_status)
			tris_cli(a->fd, "%-12.12s %s%s%s%s\n", p->agent, username, location, talkingto, music);
		count_agents++;
		tris_mutex_unlock(&p->lock);
	}
	TRIS_LIST_UNLOCK(&agents);
	if (!count_agents) 
		tris_cli(a->fd, "No Agents are configured in %s\n", config);
	else
		tris_cli(a->fd, "%d agents online\n", online_agents);
	tris_cli(a->fd, "\n");
	return CLI_SUCCESS;
}

static const char agent_logoff_usage[] =
"Usage: agent logoff <channel> [soft]\n"
"       Sets an agent as no longer logged in.\n"
"       If 'soft' is specified, do not hangup existing calls.\n";

static struct tris_cli_entry cli_agents[] = {
	TRIS_CLI_DEFINE(agents_show, "Show status of agents"),
	TRIS_CLI_DEFINE(agents_show_online, "Show all online agents"),
	TRIS_CLI_DEFINE(agent_logoff_cmd, "Sets an agent offline"),
};

/*!
 * Called by the AgentLogin application (from the dial plan).
 * 
 * \brief Log in agent application.
 *
 * \param chan
 * \param data
 * \returns
 * \sa agentmonitoroutgoing_exec(), load_module().
 */
static int login_exec(struct tris_channel *chan, void *data)
{
	int res=0;
	int tries = 0;
	int max_login_tries = maxlogintries;
	struct agent_pvt *p;
	struct tris_module_user *u;
	int login_state = 0;
	char user[TRIS_MAX_AGENT] = "";
	char pass[TRIS_MAX_AGENT];
	char agent[TRIS_MAX_AGENT] = "";
	char xpass[TRIS_MAX_AGENT] = "";
	char *errmsg;
	char *parse;
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(agent_id);
			     TRIS_APP_ARG(options);
			     TRIS_APP_ARG(extension);
		);
	const char *tmpoptions = NULL;
	int play_announcement = 1;
	char agent_goodbye[TRIS_MAX_FILENAME_LEN];
	int update_cdr = updatecdr;
	char *filename = "agent-loginok";

	u = tris_module_user_add(chan);

	parse = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, parse);

	tris_copy_string(agent_goodbye, agentgoodbye, sizeof(agent_goodbye));

	tris_channel_lock(chan);
	/* Set Channel Specific Login Overrides */
	if (!tris_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTLMAXLOGINTRIES"))) {
		max_login_tries = atoi(pbx_builtin_getvar_helper(chan, "AGENTMAXLOGINTRIES"));
		if (max_login_tries < 0)
			max_login_tries = 0;
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTMAXLOGINTRIES");
		tris_verb(3, "Saw variable AGENTMAXLOGINTRIES=%s, setting max_login_tries to: %d on Channel '%s'.\n",tmpoptions,max_login_tries,chan->name);
	}
	if (!tris_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR"))) {
		if (tris_true(pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR")))
			update_cdr = 1;
		else
			update_cdr = 0;
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR");
		tris_verb(3, "Saw variable AGENTUPDATECDR=%s, setting update_cdr to: %d on Channel '%s'.\n",tmpoptions,update_cdr,chan->name);
	}
	if (!tris_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTGOODBYE"))) {
		strcpy(agent_goodbye, pbx_builtin_getvar_helper(chan, "AGENTGOODBYE"));
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTGOODBYE");
		tris_verb(3, "Saw variable AGENTGOODBYE=%s, setting agent_goodbye to: %s on Channel '%s'.\n",tmpoptions,agent_goodbye,chan->name);
	}
	tris_channel_unlock(chan);
	/* End Channel Specific Login Overrides */
	
	if (!tris_strlen_zero(args.options)) {
		if (strchr(args.options, 's')) {
			play_announcement = 0;
		}
	}

	if (chan->_state != TRIS_STATE_UP)
		res = tris_answer(chan);
	if (!res) {
		if (!tris_strlen_zero(args.agent_id))
			tris_copy_string(user, args.agent_id, TRIS_MAX_AGENT);
		else
			res = tris_app_getdata(chan, "agent-user", user, sizeof(user) - 1, 0);
	}
	while (!res && (max_login_tries==0 || tries < max_login_tries)) {
		tries++;
		/* Check for password */
		TRIS_LIST_LOCK(&agents);
		TRIS_LIST_TRAVERSE(&agents, p, list) {
			if (!strcmp(p->agent, user) && !p->pending)
				tris_copy_string(xpass, p->password, sizeof(xpass));
		}
		TRIS_LIST_UNLOCK(&agents);
		if (!res) {
			if (!tris_strlen_zero(xpass))
				res = tris_app_getdata(chan, "agent-pass", pass, sizeof(pass) - 1, 0);
			else
				pass[0] = '\0';
		}
		errmsg = "agent-incorrect";

#if 0
		tris_log(LOG_NOTICE, "user: %s, pass: %s\n", user, pass);
#endif		

		/* Check again for accuracy */
		TRIS_LIST_LOCK(&agents);
		TRIS_LIST_TRAVERSE(&agents, p, list) {
			int unlock_channel = 1;
			tris_channel_lock(chan);
			tris_mutex_lock(&p->lock);
			if (!strcmp(p->agent, user) &&
			    !strcmp(p->password, pass) && !p->pending) {
				login_state = 1; /* Successful Login */

				/* Ensure we can't be gotten until we're done */
				p->lastdisc = tris_tvnow();
				p->lastdisc.tv_sec++;

				/* Set Channel Specific Agent Overrides */
				if (!tris_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTACKCALL"))) {
					if (!strcasecmp(pbx_builtin_getvar_helper(chan, "AGENTACKCALL"), "always"))
						p->ackcall = 2;
					else if (tris_true(pbx_builtin_getvar_helper(chan, "AGENTACKCALL")))
						p->ackcall = 1;
					else
						p->ackcall = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTACKCALL");
					tris_verb(3, "Saw variable AGENTACKCALL=%s, setting ackcall to: %d for Agent '%s'.\n", tmpoptions, p->ackcall, p->agent);
					tris_set_flag(p, AGENT_FLAG_ACKCALL);
				} else {
					p->ackcall = ackcall;
				}
				if (!tris_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF"))) {
					p->autologoff = atoi(pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF"));
					if (p->autologoff < 0)
						p->autologoff = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF");
					tris_verb(3, "Saw variable AGENTAUTOLOGOFF=%s, setting autologff to: %d for Agent '%s'.\n", tmpoptions, p->autologoff, p->agent);
					tris_set_flag(p, AGENT_FLAG_AUTOLOGOFF);
				} else {
					p->autologoff = autologoff;
				}
				if (!tris_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME"))) {
					p->wrapuptime = atoi(pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME"));
					if (p->wrapuptime < 0)
						p->wrapuptime = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME");
					tris_verb(3, "Saw variable AGENTWRAPUPTIME=%s, setting wrapuptime to: %d for Agent '%s'.\n", tmpoptions, p->wrapuptime, p->agent);
					tris_set_flag(p, AGENT_FLAG_WRAPUPTIME);
				} else {
					p->wrapuptime = wrapuptime;
				}
				tmpoptions = pbx_builtin_getvar_helper(chan, "AGENTACCEPTDTMF");
				if (!tris_strlen_zero(tmpoptions)) {
					p->acceptdtmf = *tmpoptions;
					tris_verb(3, "Saw variable AGENTACCEPTDTMF=%s, setting acceptdtmf to: %c for Agent '%s'.\n", tmpoptions, p->acceptdtmf, p->agent);
					tris_set_flag(p, AGENT_FLAG_ACCEPTDTMF);
				}
				tmpoptions = pbx_builtin_getvar_helper(chan, "AGENTENDDTMF");
				if (!tris_strlen_zero(tmpoptions)) {
					p->enddtmf = *tmpoptions;
					tris_verb(3, "Saw variable AGENTENDDTMF=%s, setting enddtmf to: %c for Agent '%s'.\n", tmpoptions, p->enddtmf, p->agent);
					tris_set_flag(p, AGENT_FLAG_ENDDTMF);
				}
				tris_channel_unlock(chan);
				unlock_channel = 0;
				/* End Channel Specific Agent Overrides */
				if (!p->chan) {
					long logintime;
					snprintf(agent, sizeof(agent), "Agent/%s", p->agent);

					p->loginchan[0] = '\0';
					p->logincallerid[0] = '\0';
					p->acknowledged = 0;
					
					tris_mutex_unlock(&p->lock);
					TRIS_LIST_UNLOCK(&agents);
					if( !res && play_announcement==1 )
						res = tris_streamfile(chan, filename, chan->language);
					if (!res)
						tris_waitstream(chan, "");
					TRIS_LIST_LOCK(&agents);
					tris_mutex_lock(&p->lock);
					if (!res) {
						res = tris_set_read_format(chan, tris_best_codec(chan->nativeformats));
						if (res)
							tris_log(LOG_WARNING, "Unable to set read format to %d\n", tris_best_codec(chan->nativeformats));
					}
					if (!res) {
						res = tris_set_write_format(chan, tris_best_codec(chan->nativeformats));
						if (res)
							tris_log(LOG_WARNING, "Unable to set write format to %d\n", tris_best_codec(chan->nativeformats));
					}
					/* Check once more just in case */
					if (p->chan)
						res = -1;
					if (!res) {
						tris_indicate_data(chan, TRIS_CONTROL_HOLD, 
							S_OR(p->moh, NULL), 
							!tris_strlen_zero(p->moh) ? strlen(p->moh) + 1 : 0);
						if (p->loginstart == 0)
							time(&p->loginstart);
						manager_event(EVENT_FLAG_AGENT, "Agentlogin",
							      "Agent: %s\r\n"
							      "Channel: %s\r\n"
							      "Uniqueid: %s\r\n",
							      p->agent, chan->name, chan->uniqueid);
						if (update_cdr && chan->cdr)
							snprintf(chan->cdr->channel, sizeof(chan->cdr->channel), "Agent/%s", p->agent);
						tris_queue_log("NONE", chan->uniqueid, agent, "AGENTLOGIN", "%s", chan->name);
						tris_verb(2, "Agent '%s' logged in (format %s/%s)\n", p->agent,
								    tris_getformatname(chan->readformat), tris_getformatname(chan->writeformat));
						/* Login this channel and wait for it to go away */
						p->chan = chan;
						if (p->ackcall > 1)
							check_beep(p, 0);
						else
							check_availability(p, 0);
						tris_mutex_unlock(&p->lock);
						TRIS_LIST_UNLOCK(&agents);
						tris_devstate_changed(TRIS_DEVICE_NOT_INUSE, "Agent/%s", p->agent);
						while (res >= 0) {
							tris_mutex_lock(&p->lock);
							if (p->deferlogoff && p->chan) {
								tris_softhangup(p->chan, TRIS_SOFTHANGUP_EXPLICIT);
								p->deferlogoff = 0;
							}
							if (p->chan != chan)
								res = -1;
							tris_mutex_unlock(&p->lock);
							/* Yield here so other interested threads can kick in. */
							sched_yield();
							if (res)
								break;

							TRIS_LIST_LOCK(&agents);
							tris_mutex_lock(&p->lock);
							if (p->lastdisc.tv_sec) {
								if (tris_tvdiff_ms(tris_tvnow(), p->lastdisc) > 0) {
									tris_debug(1, "Wrapup time for %s expired!\n", p->agent);
									p->lastdisc = tris_tv(0, 0);
									tris_devstate_changed(TRIS_DEVICE_NOT_INUSE, "Agent/%s", p->agent);
									if (p->ackcall > 1)
										check_beep(p, 0);
									else
										check_availability(p, 0);
								}
							}
							tris_mutex_unlock(&p->lock);
							TRIS_LIST_UNLOCK(&agents);
							/*	Synchronize channel ownership between call to agent and itself. */
							tris_mutex_lock(&p->app_lock);
							if (p->app_lock_flag == 1) {
								tris_cond_wait(&p->app_complete_cond, &p->app_lock);
							}
							tris_mutex_unlock(&p->app_lock);
							tris_mutex_lock(&p->lock);
							tris_mutex_unlock(&p->lock);
							if (p->ackcall > 1) 
								res = agent_ack_sleep(p);
							else
								res = tris_safe_sleep_conditional( chan, 1000, agent_cont_sleep, p );
							if ((p->ackcall > 1)  && (res == 1)) {
								TRIS_LIST_LOCK(&agents);
								tris_mutex_lock(&p->lock);
								check_availability(p, 0);
								tris_mutex_unlock(&p->lock);
								TRIS_LIST_UNLOCK(&agents);
								res = 0;
							}
							sched_yield();
						}
						tris_mutex_lock(&p->lock);
						if (res && p->owner) 
							tris_log(LOG_WARNING, "Huh?  We broke out when there was still an owner?\n");
						/* Log us off if appropriate */
						if (p->chan == chan) {
							p->chan = NULL;
						}
						p->acknowledged = 0;
						logintime = time(NULL) - p->loginstart;
						p->loginstart = 0;
						tris_mutex_unlock(&p->lock);
						manager_event(EVENT_FLAG_AGENT, "Agentlogoff",
							      "Agent: %s\r\n"
							      "Logintime: %ld\r\n"
							      "Uniqueid: %s\r\n",
							      p->agent, logintime, chan->uniqueid);
						tris_queue_log("NONE", chan->uniqueid, agent, "AGENTLOGOFF", "%s|%ld", chan->name, logintime);
						tris_verb(2, "Agent '%s' logged out\n", p->agent);
						/* If there is no owner, go ahead and kill it now */
						tris_devstate_changed(TRIS_DEVICE_UNAVAILABLE, "Agent/%s", p->agent);
						if (p->dead && !p->owner) {
							tris_mutex_destroy(&p->lock);
							tris_mutex_destroy(&p->app_lock);
							tris_cond_destroy(&p->app_complete_cond);
							tris_free(p);
						}
					}
					else {
						tris_mutex_unlock(&p->lock);
						p = NULL;
					}
					res = -1;
				} else {
					tris_mutex_unlock(&p->lock);
					errmsg = "agent-alreadyon";
					p = NULL;
				}
				break;
			}
			tris_mutex_unlock(&p->lock);
			if (unlock_channel) {
				tris_channel_unlock(chan);
			}
		}
		if (!p)
			TRIS_LIST_UNLOCK(&agents);

		if (!res && (max_login_tries==0 || tries < max_login_tries))
			res = tris_app_getdata(chan, errmsg, user, sizeof(user) - 1, 0);
	}
		
	if (!res)
		res = tris_safe_sleep(chan, 500);

	tris_module_user_remove(u);
	
 	return -1;
}

/*!
 *  \brief Called by the AgentMonitorOutgoing application (from the dial plan).
 *
 * \param chan
 * \param data
 * \returns
 * \sa login_exec(), load_module().
 */
static int agentmonitoroutgoing_exec(struct tris_channel *chan, void *data)
{
	int exitifnoagentid = 0;
	int nowarnings = 0;
	int changeoutgoing = 0;
	int res = 0;
	char agent[TRIS_MAX_AGENT];

	if (data) {
		if (strchr(data, 'd'))
			exitifnoagentid = 1;
		if (strchr(data, 'n'))
			nowarnings = 1;
		if (strchr(data, 'c'))
			changeoutgoing = 1;
	}
	if (chan->cid.cid_num) {
		const char *tmp;
		char agentvar[TRIS_MAX_BUF];
		snprintf(agentvar, sizeof(agentvar), "%s_%s", GETAGENTBYCALLERID, chan->cid.cid_num);
		if ((tmp = pbx_builtin_getvar_helper(NULL, agentvar))) {
			struct agent_pvt *p;
			tris_copy_string(agent, tmp, sizeof(agent));
			TRIS_LIST_LOCK(&agents);
			TRIS_LIST_TRAVERSE(&agents, p, list) {
				if (!strcasecmp(p->agent, tmp)) {
					if (changeoutgoing) snprintf(chan->cdr->channel, sizeof(chan->cdr->channel), "Agent/%s", p->agent);
					__agent_start_monitoring(chan, p, 1);
					break;
				}
			}
			TRIS_LIST_UNLOCK(&agents);
			
		} else {
			res = -1;
			if (!nowarnings)
				tris_log(LOG_WARNING, "Couldn't find the global variable %s, so I can't figure out which agent (if it's an agent) is placing outgoing call.\n", agentvar);
		}
	} else {
		res = -1;
		if (!nowarnings)
			tris_log(LOG_WARNING, "There is no callerid on that call, so I can't figure out which agent (if it's an agent) is placing outgoing call.\n");
	}
	if (res) {
		if (exitifnoagentid)
			return res;
	}
	return 0;
}

/*!
 * \brief Dump AgentCallbackLogin agents to the ASTdb database for persistence
 */
static void dump_agents(void)
{
	struct agent_pvt *cur_agent = NULL;
	char buf[256];

	TRIS_LIST_TRAVERSE(&agents, cur_agent, list) {
		if (cur_agent->chan)
			continue;

		if (!tris_strlen_zero(cur_agent->loginchan)) {
			snprintf(buf, sizeof(buf), "%s;%s", cur_agent->loginchan, cur_agent->logincallerid);
			if (tris_db_put(pa_family, cur_agent->agent, buf))
				tris_log(LOG_WARNING, "failed to create persistent entry in ASTdb for %s!\n", buf);
			else
				tris_debug(1, "Saved Agent: %s on %s\n", cur_agent->agent, cur_agent->loginchan);
		} else {
			/* Delete -  no agent or there is an error */
			tris_db_del(pa_family, cur_agent->agent);
		}
	}
}

/*!
 * \brief Reload the persistent agents from astdb.
 */
static void reload_agents(void)
{
	char *agent_num;
	struct tris_db_entry *db_tree;
	struct tris_db_entry *entry;
	struct agent_pvt *cur_agent;
	char agent_data[256];
	char *parse;
	char *agent_chan;
	char *agent_callerid;

	db_tree = tris_db_gettree(pa_family, NULL);

	TRIS_LIST_LOCK(&agents);
	for (entry = db_tree; entry; entry = entry->next) {
		agent_num = entry->key + strlen(pa_family) + 2;
		TRIS_LIST_TRAVERSE(&agents, cur_agent, list) {
			tris_mutex_lock(&cur_agent->lock);
			if (strcmp(agent_num, cur_agent->agent) == 0)
				break;
			tris_mutex_unlock(&cur_agent->lock);
		}
		if (!cur_agent) {
			tris_db_del(pa_family, agent_num);
			continue;
		} else
			tris_mutex_unlock(&cur_agent->lock);
		if (!tris_db_get(pa_family, agent_num, agent_data, sizeof(agent_data)-1)) {
			tris_debug(1, "Reload Agent from AstDB: %s on %s\n", cur_agent->agent, agent_data);
			parse = agent_data;
			agent_chan = strsep(&parse, ";");
			agent_callerid = strsep(&parse, ";");
			tris_copy_string(cur_agent->loginchan, agent_chan, sizeof(cur_agent->loginchan));
			if (agent_callerid) {
				tris_copy_string(cur_agent->logincallerid, agent_callerid, sizeof(cur_agent->logincallerid));
				set_agentbycallerid(cur_agent->logincallerid, cur_agent->agent);
			} else
				cur_agent->logincallerid[0] = '\0';
			if (cur_agent->loginstart == 0)
				time(&cur_agent->loginstart);
			tris_devstate_changed(TRIS_DEVICE_UNKNOWN, "Agent/%s", cur_agent->agent);	
		}
	}
	TRIS_LIST_UNLOCK(&agents);
	if (db_tree) {
		tris_log(LOG_NOTICE, "Agents successfully reloaded from database.\n");
		tris_db_freetree(db_tree);
	}
}

/*! \brief Part of PBX channel interface */
static int agent_devicestate(void *data)
{
	struct agent_pvt *p;
	char *s;
	tris_group_t groupmatch;
	int groupoff;
	int res = TRIS_DEVICE_INVALID;
	
	s = data;
	if ((s[0] == '@') && (sscanf(s + 1, "%30d", &groupoff) == 1))
		groupmatch = (1 << groupoff);
	else if ((s[0] == ':') && (sscanf(s + 1, "%30d", &groupoff) == 1)) {
		groupmatch = (1 << groupoff);
	} else 
		groupmatch = 0;

	/* Check actual logged in agents first */
	TRIS_LIST_LOCK(&agents);
	TRIS_LIST_TRAVERSE(&agents, p, list) {
		tris_mutex_lock(&p->lock);
		if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent))) {
			if (p->owner) {
				if (res != TRIS_DEVICE_INUSE)
					res = TRIS_DEVICE_BUSY;
			} else {
				if (res == TRIS_DEVICE_BUSY)
					res = TRIS_DEVICE_INUSE;
				if (p->chan || !tris_strlen_zero(p->loginchan)) {
					if (res == TRIS_DEVICE_INVALID)
						res = TRIS_DEVICE_UNKNOWN;
				} else if (res == TRIS_DEVICE_INVALID)	
					res = TRIS_DEVICE_UNAVAILABLE;
			}
			if (!strcmp(data, p->agent)) {
				tris_mutex_unlock(&p->lock);
				break;
			}
		}
		tris_mutex_unlock(&p->lock);
	}
	TRIS_LIST_UNLOCK(&agents);
	return res;
}

/*!
 * \note This function expects the agent list to be locked
 */
static struct agent_pvt *find_agent(char *agentid)
{
	struct agent_pvt *cur;

	TRIS_LIST_TRAVERSE(&agents, cur, list) {
		if (!strcmp(cur->agent, agentid))
			break;	
	}

	return cur;	
}

static int function_agent(struct tris_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *parse;    
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(agentid);
		TRIS_APP_ARG(item);
	);
	char *tmp;
	struct agent_pvt *agent;

	buf[0] = '\0';

	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "The AGENT function requires an argument - agentid!\n");
		return -1;
	}

	parse = tris_strdupa(data);

	TRIS_NONSTANDARD_APP_ARGS(args, parse, ':');
	if (!args.item)
		args.item = "status";

	TRIS_LIST_LOCK(&agents);

	if (!(agent = find_agent(args.agentid))) {
		TRIS_LIST_UNLOCK(&agents);
		tris_log(LOG_WARNING, "Agent '%s' not found!\n", args.agentid);
		return -1;
	}

	if (!strcasecmp(args.item, "status")) {
		char *status = "LOGGEDOUT";
		if (agent->chan || !tris_strlen_zero(agent->loginchan)) 
			status = "LOGGEDIN";	
		tris_copy_string(buf, status, len);
	} else if (!strcasecmp(args.item, "password")) 
		tris_copy_string(buf, agent->password, len);
	else if (!strcasecmp(args.item, "name"))
		tris_copy_string(buf, agent->name, len);
	else if (!strcasecmp(args.item, "mohclass"))
		tris_copy_string(buf, agent->moh, len);
	else if (!strcasecmp(args.item, "channel")) {
		if (agent->chan) {
			tris_copy_string(buf, agent->chan->name, len);
			tmp = strrchr(buf, '-');
			if (tmp)
				*tmp = '\0';
		} 
	} else if (!strcasecmp(args.item, "exten"))
		tris_copy_string(buf, agent->loginchan, len);	

	TRIS_LIST_UNLOCK(&agents);

	return 0;
}

struct tris_custom_function agent_function = {
	.name = "AGENT",
	.read = function_agent,
};


/*!
 * \brief Initialize the Agents module.
 * This function is being called by Trismedia when loading the module. 
 * Among other things it registers applications, cli commands and reads the cofiguration file.
 *
 * \returns int Always 0.
 */
static int load_module(void)
{
	/* Make sure we can register our agent channel type */
	if (tris_channel_register(&agent_tech)) {
		tris_log(LOG_ERROR, "Unable to register channel class 'Agent'\n");
		return TRIS_MODULE_LOAD_FAILURE;
	}
	/* Read in the config */
	if (!read_agent_config(0))
		return TRIS_MODULE_LOAD_DECLINE;
	if (persistent_agents)
		reload_agents();
	/* Dialplan applications */
	tris_register_application_xml(app, login_exec);
	tris_register_application_xml(app3, agentmonitoroutgoing_exec);

	/* Manager commands */
	tris_manager_register2("Agents", EVENT_FLAG_AGENT, action_agents, "Lists agents and their status", mandescr_agents);
	tris_manager_register2("AgentLogoff", EVENT_FLAG_AGENT, action_agent_logoff, "Sets an agent as no longer logged in", mandescr_agent_logoff);

	/* CLI Commands */
	tris_cli_register_multiple(cli_agents, ARRAY_LEN(cli_agents));

	/* Dialplan Functions */
	tris_custom_function_register(&agent_function);

	return TRIS_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	if (!read_agent_config(1)) {
		if (persistent_agents)
			reload_agents();
	}
	return 0;
}

static int unload_module(void)
{
	struct agent_pvt *p;
	/* First, take us out of the channel loop */
	tris_channel_unregister(&agent_tech);
	/* Unregister dialplan functions */
	tris_custom_function_unregister(&agent_function);	
	/* Unregister CLI commands */
	tris_cli_unregister_multiple(cli_agents, ARRAY_LEN(cli_agents));
	/* Unregister dialplan applications */
	tris_unregister_application(app);
	tris_unregister_application(app3);
	/* Unregister manager command */
	tris_manager_unregister("Agents");
	tris_manager_unregister("AgentLogoff");
	/* Unregister channel */
	TRIS_LIST_LOCK(&agents);
	/* Hangup all interfaces if they have an owner */
	while ((p = TRIS_LIST_REMOVE_HEAD(&agents, list))) {
		if (p->owner)
			tris_softhangup(p->owner, TRIS_SOFTHANGUP_APPUNLOAD);
		tris_free(p);
	}
	TRIS_LIST_UNLOCK(&agents);
	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Agent Proxy Channel",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
