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
 * \brief Comedian Mail - Voicemail System
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \extref Unixodbc - http://www.unixodbc.org
 * \extref A source distribution of University of Washington's IMAP
c-client (http://www.washington.edu/imap/
 * 
 * \par See also
 * \arg \ref Config_vm
 * \note For information about voicemail IMAP storage, read doc/imapstorage.txt
 * \ingroup applications
 * \note This module requires res_adsi to load. This needs to be optional
 * during compilation.
 *
 *
 *
 * \note  This file is now almost impossible to work with, due to all \#ifdefs.
 *        Feels like the database code before realtime. Someone - please come up
 *        with a plan to clean this up.
 */

/*** MODULEINFO
	<depend>res_smdi</depend>
 ***/

/*** MAKEOPTS
<category name="MENUSELECT_OPTS_app_voicemail" displayname="Voicemail Build Options" positive_output="yes" remove_on_change="apps/app_voicemail.o apps/app_directory.o">
	<member name="ODBC_STORAGE" displayname="Storage of Voicemail using ODBC">
		<depend>unixodbc</depend>
		<depend>ltdl</depend>
		<conflict>IMAP_STORAGE</conflict>
		<defaultenabled>no</defaultenabled>
	</member>
	<member name="IMAP_STORAGE" displayname="Storage of Voicemail using IMAP4">
		<depend>imap_tk</depend>
		<conflict>ODBC_STORAGE</conflict>
		<use>ssl</use>
		<defaultenabled>no</defaultenabled>
	</member>
</category>
 ***/

#include "trismedia.h"

#ifdef IMAP_STORAGE
#include <ctype.h>
#include <signal.h>
#include <pwd.h>
#ifdef USE_SYSTEM_IMAP
#include <imap/c-client.h>
#include <imap/imap4r1.h>
#include <imap/linkage.h>
#elif defined (USE_SYSTEM_CCLIENT)
#include <c-client/c-client.h>
#include <c-client/imap4r1.h>
#include <c-client/linkage.h>
#else
#include "c-client.h"
#include "imap4r1.h"
#include "linkage.h"
#endif
#endif

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 160388 $")

#include "trismedia/paths.h"	/* use tris_config_TRIS_SPOOL_DIR */
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <dirent.h>

#include "trismedia/logger.h"
#include "trismedia/lock.h"
#include "trismedia/file.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/config.h"
#include "trismedia/say.h"
#include "trismedia/module.h"
#include "trismedia/adsi.h"
#include "trismedia/app.h"
#include "trismedia/manager.h"
#include "trismedia/dsp.h"
#include "trismedia/localtime.h"
#include "trismedia/cli.h"
#include "trismedia/utils.h"
#include "trismedia/stringfields.h"
#include "trismedia/smdi.h"
#include "trismedia/event.h"

//#ifdef ODBC_STORAGE
#include "trismedia/res_odbc.h"
//#endif

#ifdef IMAP_STORAGE
static char imapserver[48];
static char imapport[8];
static char imapflags[128];
static char imapfolder[64];
static char imapparentfolder[64] = "\0";
static char greetingfolder[64];
static char authuser[32];
static char authpassword[42];

static int expungeonhangup = 1;
static int imapgreetings = 0;
static char delimiter = '\0';

struct vm_state;
struct tris_vm_user;

/* Forward declarations for IMAP */
static int init_mailstream(struct vm_state *vms, int box);
static void write_file(char *filename, char *buffer, unsigned long len);
static char *get_header_by_tag(char *header, char *tag, char *buf, size_t len);
static void vm_imap_delete(int msgnum, struct tris_vm_user *vmu);
static char *get_user_by_mailbox(char *mailbox, char *buf, size_t len);
static struct vm_state *get_vm_state_by_imapuser(char *user, int interactive);
static struct vm_state *get_vm_state_by_mailbox(const char *mailbox, int interactive);
static struct vm_state *create_vm_state_from_user(struct tris_vm_user *vmu);
static void vmstate_insert(struct vm_state *vms);
static void vmstate_delete(struct vm_state *vms);
static void set_update(MAILSTREAM * stream);
static void init_vm_state(struct vm_state *vms);
static int save_body(BODY *body, struct vm_state *vms, char *section, char *format);
static void get_mailbox_delimiter(MAILSTREAM *stream);
static void mm_parsequota (MAILSTREAM *stream, unsigned char *msg, QUOTALIST *pquota);
static void imap_mailbox_name(char *spec, size_t len, struct vm_state *vms, int box, int target);
static int imap_store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum, struct tris_channel *chan, struct tris_vm_user *vmu, char *fmt, int duration, struct vm_state *vms);
static void update_messages_by_imapuser(const char *user, unsigned long number);

static int imap_remove_file (char *dir, int msgnum);
static int imap_retrieve_file (const char *dir, const int msgnum, const char *mailbox, const char *context);
static int imap_delete_old_greeting (char *dir, struct vm_state *vms);
static void check_quota(struct vm_state *vms, char *mailbox);
static int open_mailbox(struct vm_state *vms, struct tris_vm_user *vmu, int box);
struct vmstate {
	struct vm_state *vms;
	TRIS_LIST_ENTRY(vmstate) list;
};

static TRIS_LIST_HEAD_STATIC(vmstates, vmstate);

#endif

#define SMDI_MWI_WAIT_TIMEOUT 1000 /* 1 second */

#define COMMAND_TIMEOUT 5000
/* Don't modify these here; set your umask at runtime instead */
#define	VOICEMAIL_DIR_MODE	0777
#define	VOICEMAIL_FILE_MODE	0666
#define	CHUNKSIZE	65536

#define VOICEMAIL_CONFIG "voicemail.conf"
#define TRISMEDIA_USERNAME "trismedia"

/* Define fast-forward, pause, restart, and reverse keys
   while listening to a voicemail message - these are
   strings, not characters */
#define DEFAULT_LISTEN_CONTROL_FORWARD_KEY "9"
#define DEFAULT_LISTEN_CONTROL_REVERSE_KEY "7"
#define DEFAULT_LISTEN_CONTROL_PAUSE_KEY "8"
#define DEFAULT_LISTEN_CONTROL_RESTART_KEY "5"
#define DEFAULT_LISTEN_CONTROL_STOP_KEY "012346*#"
#define VALID_DTMF "1234567890*#" /* Yes ABCD are valid dtmf but what phones have those? */

/* Default mail command to mail voicemail. Change it with the
    mailcmd= command in voicemail.conf */
#define SENDMAIL "/usr/sbin/sendmail -t"

#define INTRO "voicemail/record_your_message"

#define MAXMSG 20
#define MAXMSGLIMIT 9999
#define DEFAULT_MAXSECS	600

#define BASELINELEN 72
#define BASEMAXINLINE 256
#define eol "\r\n"

#define MAX_DATETIME_FORMAT	512
#define MAX_NUM_CID_CONTEXTS 10

#define VM_REVIEW        (1 << 0)
#define VM_OPERATOR      (1 << 1)
#define VM_SAYCID        (1 << 2)
#define VM_SVMAIL        (1 << 3)
#define VM_ENVELOPE      (1 << 4)
#define VM_SAYDURATION   (1 << 5)
#define VM_SKIPAFTERCMD  (1 << 6)
#define VM_FORCENAME     (1 << 7)   /*!< Have new users record their name */
#define VM_FORCEGREET    (1 << 8)   /*!< Have new users record their greetings */
#define VM_PBXSKIP       (1 << 9)
#define VM_DIRECFORWARD  (1 << 10)  /*!< directory_forward */
#define VM_ATTACH        (1 << 11)
#define VM_DELETE        (1 << 12)
#define VM_ALLOCED       (1 << 13)
#define VM_SEARCH        (1 << 14)
#define VM_TEMPGREETWARN (1 << 15)  /*!< Remind user tempgreeting is set */
#define VM_MOVEHEARD     (1 << 16)  /*!< Move a "heard" message to Old after listening to it */
#define ERROR_LOCK_PATH  -100
#define ERROR_MAILBOX_FULL	-200


enum {
	NEW_FOLDER,
	OLD_FOLDER,
	SAVED_FOLDER,
	DELETED_FOLDER,
	WORK_FOLDER,
	FAMILY_FOLDER,
	FRIENDS_FOLDER,
	GREETINGS_FOLDER
} vm_box;

enum {
	OPT_SILENT =           (1 << 0),
	OPT_BUSY_GREETING =    (1 << 1),
	OPT_UNAVAIL_GREETING = (1 << 2),
	OPT_RECORDGAIN =       (1 << 3),
	OPT_PREPEND_MAILBOX =  (1 << 4),
	OPT_AUTOPLAY =         (1 << 6),
	OPT_DTMFEXIT =         (1 << 7),
	OPT_COMMANDER =         (1 << 8 ),
} vm_option_flags;

enum {
	OPT_ARG_RECORDGAIN = 0,
	OPT_ARG_PLAYFOLDER = 1,
	OPT_ARG_DTMFEXIT   = 2,
	/* This *must* be the last value in this enum! */
	OPT_ARG_ARRAY_SIZE = 3,
} vm_option_args;

TRIS_APP_OPTIONS(vm_app_options, {
	TRIS_APP_OPTION('s', OPT_SILENT),
	TRIS_APP_OPTION('b', OPT_BUSY_GREETING),
	TRIS_APP_OPTION('u', OPT_UNAVAIL_GREETING),
	TRIS_APP_OPTION_ARG('g', OPT_RECORDGAIN, OPT_ARG_RECORDGAIN),
	TRIS_APP_OPTION_ARG('d', OPT_DTMFEXIT, OPT_ARG_DTMFEXIT),
	TRIS_APP_OPTION('p', OPT_PREPEND_MAILBOX),
	TRIS_APP_OPTION_ARG('a', OPT_AUTOPLAY, OPT_ARG_PLAYFOLDER),
	TRIS_APP_OPTION('c', OPT_COMMANDER),
});

static int load_config(int reload);

/*! \page vmlang Voicemail Language Syntaxes Supported

	\par Syntaxes supported, not really language codes.
	\arg \b en    - English
	\arg \b de    - German
	\arg \b es    - Spanish
	\arg \b fr    - French
	\arg \b it    - Italian
	\arg \b nl    - Dutch
	\arg \b pt    - Portuguese
	\arg \b pt_BR - Portuguese (Brazil)
	\arg \b gr    - Greek
	\arg \b no    - Norwegian
	\arg \b se    - Swedish
	\arg \b tw    - Chinese (Taiwan)
	\arg \b ua - Ukrainian

German requires the following additional soundfile:
\arg \b 1F	einE (feminine)

Spanish requires the following additional soundfile:
\arg \b 1M      un (masculine)

Dutch, Portuguese & Spanish require the following additional soundfiles:
\arg \b vm-INBOXs	singular of 'new'
\arg \b vm-Olds		singular of 'old/heard/read'

NB these are plural:
\arg \b vm-INBOX	nieuwe (nl)
\arg \b vm-Old		oude (nl)

Polish uses:
\arg \b vm-new-a	'new', feminine singular accusative
\arg \b vm-new-e	'new', feminine plural accusative
\arg \b vm-new-ych	'new', feminine plural genitive
\arg \b vm-old-a	'old', feminine singular accusative
\arg \b vm-old-e	'old', feminine plural accusative
\arg \b vm-old-ych	'old', feminine plural genitive
\arg \b digits/1-a	'one', not always same as 'digits/1'
\arg \b digits/2-ie	'two', not always same as 'digits/2'

Swedish uses:
\arg \b vm-nytt		singular of 'new'
\arg \b vm-nya		plural of 'new'
\arg \b vm-gammalt	singular of 'old'
\arg \b vm-gamla	plural of 'old'
\arg \b digits/ett	'one', not always same as 'digits/1'

Norwegian uses:
\arg \b vm-ny		singular of 'new'
\arg \b vm-nye		plural of 'new'
\arg \b vm-gammel	singular of 'old'
\arg \b vm-gamle	plural of 'old'

Dutch also uses:
\arg \b nl-om		'at'?

Spanish also uses:
\arg \b vm-youhaveno

Ukrainian requires the following additional soundfile:
\arg \b vm-nove		'nove'
\arg \b vm-stare	'stare'
\arg \b digits/ua/1e	'odne'

Italian requires the following additional soundfile:

For vm_intro_it:
\arg \b vm-nuovo	new
\arg \b vm-nuovi	new plural
\arg \b vm-vecchio	old
\arg \b vm-vecchi	old plural

Chinese (Taiwan) requires the following additional soundfile:
\arg \b vm-tong		A class-word for call (tong1)
\arg \b vm-ri		A class-word for day (ri4)
\arg \b vm-you		You (ni3)
\arg \b vm-haveno   Have no (mei2 you3)
\arg \b vm-have     Have (you3)
\arg \b vm-listen   To listen (yao4 ting1)


\note Don't use vm-INBOX or vm-Old, because they are the name of the INBOX and Old folders,
spelled among others when you have to change folder. For the above reasons, vm-INBOX
and vm-Old are spelled plural, to make them sound more as folder name than an adjective.

*/

struct baseio {
	int iocp;
	int iolen;
	int linelength;
	int ateof;
	unsigned char iobuf[BASEMAXINLINE];
};

/*! Structure for linked list of users 
 * Use tris_vm_user_destroy() to free one of these structures. */
struct tris_vm_user {
	char context[TRIS_MAX_CONTEXT];   /*!< Voicemail context */
	char mailbox[TRIS_MAX_EXTENSION]; /*!< Mailbox id, unique within vm context */
	char password[80];               /*!< Secret pin code, numbers only */
	char fullname[80];               /*!< Full name, for directory app */
	char email[80];                  /*!< E-mail address */
	char pager[80];                  /*!< E-mail address to pager (no attachment) */
	char serveremail[80];            /*!< From: Mail address */
	char mailcmd[160];               /*!< Configurable mail command */
	char language[MAX_LANGUAGE];     /*!< Config: Language setting */
	char zonetag[80];                /*!< Time zone */
	char callback[80];
	char dialout[80];
	char uniqueid[80];               /*!< Unique integer identifier */
	char exit[80];
	char attachfmt[20];              /*!< Attachment format */
	unsigned int flags;              /*!< VM_ flags */	
	int saydurationm;
	int maxmsg;                      /*!< Maximum number of msgs per folder for this mailbox */
	int maxdeletedmsg;               /*!< Maximum number of deleted msgs saved for this mailbox */
	int maxsecs;                     /*!< Maximum number of seconds per message for this mailbox */
#ifdef IMAP_STORAGE
	char imapuser[80];               /*!< IMAP server login */
	char imappassword[80];           /*!< IMAP server password if authpassword not defined */
#endif
	double volgain;                  /*!< Volume gain for voicemails sent via email */
	TRIS_LIST_ENTRY(tris_vm_user) list;
};

/*! Voicemail time zones */
struct vm_zone {
	TRIS_LIST_ENTRY(vm_zone) list;
	char name[80];
	char timezone[80];
	char msg_format[512];
};

/*! Voicemail mailbox state */
struct vm_state {
	char curbox[80];
	char username[80];
	char curdir[PATH_MAX];
	char vmbox[PATH_MAX];
	char fn[PATH_MAX];
	char fn2[PATH_MAX];
	int *deleted;
	int *heard;
	int curmsg;
	int lastmsg;
	int newmessages;
	int oldmessages;
	int starting;
	int repeats;
#ifdef IMAP_STORAGE
	tris_mutex_t lock;
	int updated;                         /*!< decremented on each mail check until 1 -allows delay */
	long msgArray[256];
	MAILSTREAM *mailstream;
	int vmArrayIndex;
	char imapuser[80];                   /*!< IMAP server login */
	int interactive;
	unsigned int quota_limit;
	unsigned int quota_usage;
	struct vm_state *persist_vms;
#endif
};

#ifdef ODBC_STORAGE
static char odbc_database[80];
static char odbc_table[80];
#define RETRIEVE(a,b,c,d) retrieve_file(a,b)
#define DISPOSE(a,b) remove_file(a,b)
#define STORE(a,b,c,d,e,f,g,h,i) store_file(a,b,c,d)
#define EXISTS(a,b,c,d) (message_exists(a,b))
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(a,b,c,d,e,f))
#define COPY(a,b,c,d,e,f,g,h) (copy_file(a,b,c,d,e,f))
#define DELETE(a,b,c,d) (delete_file(a,b))
#else
#ifdef IMAP_STORAGE
#define DISPOSE(a,b) (imap_remove_file(a,b))
#define STORE(a,b,c,d,e,f,g,h,i) (imap_store_file(a,b,c,d,e,f,g,h,i))
#define EXISTS(a,b,c,d) (tris_fileexists(c, NULL, d) > 0)
#define RETRIEVE(a,b,c,d) imap_retrieve_file(a,b,c,d)
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(g,h));
#define COPY(a,b,c,d,e,f,g,h) (copy_file(g,h));
#define DELETE(a,b,c,d) (vm_imap_delete(b,d))
#else
#define RETRIEVE(a,b,c,d)
#define DISPOSE(a,b)
#define STORE(a,b,c,d,e,f,g,h,i)
#define EXISTS(a,b,c,d) (tris_fileexists(c, NULL, d) > 0)
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(g,h));
#define COPY(a,b,c,d,e,f,g,h) (copy_plain_file(g,h));
#define DELETE(a,b,c,d) (vm_delete(c))
#endif
#endif

static char VM_SPOOL_DIR[PATH_MAX];

static char ext_pass_cmd[128];

int my_umask;

#define PWDCHANGE_INTERNAL (1 << 1)
#define PWDCHANGE_EXTERNAL (1 << 2)
static int pwdchange = PWDCHANGE_INTERNAL;

#ifdef ODBC_STORAGE
#define tdesc "Comedian Mail (Voicemail System) with ODBC Storage"
#else
# ifdef IMAP_STORAGE
# define tdesc "Comedian Mail (Voicemail System) with IMAP Storage"
# else
# define tdesc "Comedian Mail (Voicemail System)"
# endif
#endif

static char userscontext[TRIS_MAX_EXTENSION] = "default";

static char *addesc = "Comedian Mail";

static char *synopsis_vm = "Leave a Voicemail message";

static char *descrip_vm =
	"  VoiceMail(mailbox[@context][&mailbox[@context]][...][,options]): This\n"
	"application allows the calling party to leave a message for the specified\n"
	"list of mailboxes. When multiple mailboxes are specified, the greeting will\n"
	"be taken from the first mailbox specified. Dialplan execution will stop if the\n"
	"specified mailbox does not exist.\n"
	"  The Voicemail application will exit if any of the following DTMF digits are\n"
	"received:\n"
	"    0 - Jump to the 'o' extension in the current dialplan context.\n"
	"    * - Jump to the 'a' extension in the current dialplan context.\n"
	"  This application will set the following channel variable upon completion:\n"
	"    VMSTATUS - This indicates the status of the execution of the VoiceMail\n"
	"               application. The possible values are:\n"
	"               SUCCESS | USEREXIT | FAILED\n\n"
	"  Options:\n"
	"    b      - Play the 'busy' greeting to the calling party.\n"
	"    d([c]) - Accept digits for a new extension in context c, if played during\n"
	"             the greeting.  Context defaults to the current context.\n"
	"    g(#)   - Use the specified amount of gain when recording the voicemail\n"
	"             message. The units are whole-number decibels (dB).\n"
	"             Only works on supported technologies, which is DAHDI only.\n"
	"    s      - Skip the playback of instructions for leaving a message to the\n"
	"             calling party.\n"
	"    u      - Play the 'unavailable' greeting.\n";

static char *synopsis_vmain = "Check Voicemail messages";

static char *descrip_vmain =
	"  VoiceMailMain([mailbox][@context][,options]): This application allows the\n"
	"calling party to check voicemail messages. A specific mailbox, and optional\n"
	"corresponding context, may be specified. If a mailbox is not provided, the\n"
	"calling party will be prompted to enter one. If a context is not specified,\n"
	"the 'default' context will be used.\n\n"
	"  Options:\n"
	"    p    - Consider the mailbox parameter as a prefix to the mailbox that\n"
	"           is entered by the caller.\n"
	"    g(#) - Use the specified amount of gain when recording a voicemail\n"
	"           message. The units are whole-number decibels (dB).\n"
	"    s    - Skip checking the passcode for the mailbox.\n"
	"    a(#) - Skip folder prompt and go directly to folder specified.\n"
	"           Defaults to INBOX\n";

static char *synopsis_vm_box_exists =
"Check to see if Voicemail mailbox exists";

static char *descrip_vm_box_exists =
	"  MailboxExists(mailbox[@context][,options]): Check to see if the specified\n"
	"mailbox exists. If no voicemail context is specified, the 'default' context\n"
	"will be used.\n"
	"  This application will set the following channel variable upon completion:\n"
	"    VMBOXEXISTSSTATUS - This will contain the status of the execution of the\n"
	"                        MailboxExists application. Possible values include:\n"
	"                        SUCCESS | FAILED\n\n"
	"  Options: (none)\n";

static char *synopsis_vmauthenticate = "Authenticate with Voicemail passwords";

static char *descrip_vmauthenticate =
	"  VMAuthenticate([mailbox][@context][,options]): This application behaves the\n"
	"same way as the Authenticate application, but the passwords are taken from\n"
	"voicemail.conf.\n"
	"  If the mailbox is specified, only that mailbox's password will be considered\n"
	"valid. If the mailbox is not specified, the channel variable AUTH_MAILBOX will\n"
	"be set with the authenticated mailbox.\n\n"
	"  Options:\n"
	"    s - Skip playing the initial prompts.\n";

static char *descrip_cmd = 
	"LeaveCommand([roomno][@context][,options])\n";
static char *descrip_rprt = 
	"LeaveReport([roomno][@context][,options])\n";

static char *synopsis_cmd = "Leave command";
static char *synopsis_rprt = "Leave report";

static char *descrip_cmdmain = 
	"ListenCommand([roomno][@context][,options])\n"
	"  Options:\n"
	"    c - This caller is commander.\n";
static char *descrip_rprtmain = 
	"ListenReport([roomno][@context][,options])\n";

static char *synopsis_cmdmain = "Listen command";
static char *synopsis_rprtmain = "Listen report";

/* Leave a message */
static char *app = "VoiceMail";

/* Check mail, control, etc */
static char *app2 = "VoiceMailMain";

static char *app3 = "MailboxExists";
static char *app4 = "VMAuthenticate";

/* Leave a command */
static char *app5 = "LeaveCommand";
/* Leave a report */
static char *app6 = "LeaveReport";

/* Check mail, control, etc */
static char *app7 = "ListenCommand";
/* Check mail, control, etc */
static char *app8 = "ListenReport";


static TRIS_LIST_HEAD_STATIC(users, tris_vm_user);
static TRIS_LIST_HEAD_STATIC(zones, vm_zone);
static int maxsilence;
static int maxmsg;
static int maxdeletedmsg;
static int silencethreshold = 128;
static char serveremail[80];
static char mailcmd[160];	/* Configurable mail cmd */
static char externnotify[160]; 
static struct tris_smdi_interface *smdi_iface = NULL;
static char vmfmts[80];
static double volgain;
static int vmminsecs;
static int vmmaxsecs;
static int maxgreet;
static int skipms;
static int maxlogins;

/*! Poll mailboxes for changes since there is something external to
 *  app_voicemail that may change them. */
static unsigned int poll_mailboxes;

/*! Polling frequency */
static unsigned int poll_freq;
/*! By default, poll every 30 seconds */
#define DEFAULT_POLL_FREQ 30

TRIS_MUTEX_DEFINE_STATIC(poll_lock);
static tris_cond_t poll_cond = PTHREAD_COND_INITIALIZER;
static pthread_t poll_thread = TRIS_PTHREADT_NULL;
static unsigned char poll_thread_run;

/*! Subscription to ... MWI event subscriptions */
static struct tris_event_sub *mwi_sub_sub;
/*! Subscription to ... MWI event un-subscriptions */
static struct tris_event_sub *mwi_unsub_sub;

/*!
 * \brief An MWI subscription
 *
 * This is so we can keep track of which mailboxes are subscribed to.
 * This way, we know which mailboxes to poll when the pollmailboxes
 * option is being used.
 */
struct mwi_sub {
	TRIS_RWLIST_ENTRY(mwi_sub) entry;
	int old_new;
	int old_old;
	uint32_t uniqueid;
	char mailbox[1];
};

static TRIS_RWLIST_HEAD_STATIC(mwi_subs, mwi_sub);

/* custom audio control prompts for voicemail playback */
static char listen_control_forward_key[12];
static char listen_control_reverse_key[12];
static char listen_control_pause_key[12];
static char listen_control_restart_key[12];
static char listen_control_stop_key[12];

/* custom password sounds */
static char vm_password[80] = "voicemail/vm-password";
static char vm_newpassword[80] = "voicemail/vm-newpassword";
static char vm_passchanged[80] = "voicemail/vm-passchanged";
static char vm_reenterpassword[80] = "voicemail/vm-reenterpassword";
static char vm_mismatch[80] = "voicemail/vm-mismatch";

static struct tris_flags globalflags = {0};

static int saydurationminfo;

static char dialcontext[TRIS_MAX_CONTEXT] = "";
static char callcontext[TRIS_MAX_CONTEXT] = "";
static char exitcontext[TRIS_MAX_CONTEXT] = "";

static char cidinternalcontexts[MAX_NUM_CID_CONTEXTS][64];


static char *emailbody = NULL;
static char *emailsubject = NULL;
static char *pagerbody = NULL;
static char *pagersubject = NULL;
static char fromstring[100];
static char pagerfromstring[100];
static char charset[32] = "ISO-8859-1";

static unsigned char adsifdn[4] = "\x00\x00\x00\x0F";
static unsigned char adsisec[4] = "\x9B\xDB\xF7\xAC";
static int adsiver = 1;
static char emaildateformat[32] = "%A, %B %d, %Y at %r";

/* Forward declarations - generic */
static int open_mailbox(struct vm_state *vms, struct tris_vm_user *vmu, int box);
static int advanced_options(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms, int msg, int option, signed char record_gain);
static int dialout(struct tris_channel *chan, struct tris_vm_user *vmu, char *num, char *outgoing_context);
static int store_vmfile(struct tris_channel *chan, char *tempfile, char *context, char *mailbox, char *ext, int duration, char *fmt);
static int play_record_review(struct tris_channel *chan, char *playfile, char *recordfile, int maxtime,
			char *fmt, int outsidecaller, struct tris_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms);
static int play_record_review_cmd(struct tris_channel *chan, char *playfile, char *recordfile, int maxtime,
			char *fmt, int outsidecaller, struct tris_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms);
static int play_record_review_rprt(struct tris_channel *chan, char *playfile, char *recordfile, int maxtime,
			char *fmt, int outsidecaller, struct tris_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms, char *ext);
static int vm_tempgreeting(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain);
static int vm_play_folder_name(struct tris_channel *chan, char *mbox);
static int notify_new_message(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms, int msgnum, long duration, char *fmt, char *cidnum, char *cidname);
static void make_email_file(FILE *p, char *srcemail, struct tris_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct tris_channel *chan, const char *category, int imap);
static void apply_options(struct tris_vm_user *vmu, const char *options);
static int is_valid_dtmf(const char *key);

#if !(defined(ODBC_STORAGE) || defined(IMAP_STORAGE))
static int __has_voicemail(const char *context, const char *mailbox, const char *folder, int shortcircuit);
#endif

static char *strip_control(const char *input, char *buf, size_t buflen)
{
	char *bufptr = buf;
	for (; *input; input++) {
		if (*input < 32) {
			continue;
		}
		*bufptr++ = *input;
		if (bufptr == buf + buflen - 1) {
			break;
		}
	}
	*bufptr = '\0';
	return buf;
}

static void populate_defaults(struct tris_vm_user *vmu)
{
	tris_copy_flags(vmu, (&globalflags), TRIS_FLAGS_ALL);	
	if (saydurationminfo)
		vmu->saydurationm = saydurationminfo;
	tris_copy_string(vmu->callback, callcontext, sizeof(vmu->callback));
	tris_copy_string(vmu->dialout, dialcontext, sizeof(vmu->dialout));
	tris_copy_string(vmu->exit, exitcontext, sizeof(vmu->exit));
	if (vmmaxsecs)
		vmu->maxsecs = vmmaxsecs;
	if (maxmsg)
		vmu->maxmsg = maxmsg;
	if (maxdeletedmsg)
		vmu->maxdeletedmsg = maxdeletedmsg;
	vmu->volgain = volgain;
}

static struct tris_vm_user *find_or_create(const char *context, const char *mbox)
{
	struct tris_vm_user *vmu;

	TRIS_LIST_TRAVERSE(&users, vmu, list) {
		if (tris_test_flag((&globalflags), VM_SEARCH) && !strcasecmp(mbox, vmu->mailbox))
			break;
		if (context && (!strcasecmp(context, vmu->context)) && (!strcasecmp(mbox, vmu->mailbox)))
			break;
	}

	if (vmu)
		return vmu;
	
	if (!(vmu = tris_calloc(1, sizeof(*vmu))))
		return NULL;
	
	tris_copy_string(vmu->context, context, sizeof(vmu->context));
	tris_copy_string(vmu->mailbox, mbox, sizeof(vmu->mailbox));

	TRIS_LIST_INSERT_TAIL(&users, vmu, list);
	
	return vmu;
}

static struct tris_vm_user* create_user(struct tris_vm_user *ivm, const char *context, char* usernm)
{
	struct tris_vm_user* vmu = NULL;

	if (!context && !tris_test_flag((&globalflags), VM_SEARCH))
		context = "default";


	/* Make a copy, so that on a reload, we have no race */
	if ((vmu = (ivm ? ivm : tris_malloc(sizeof(*vmu))))) {
		tris_set2_flag(vmu, !ivm, VM_ALLOCED);
		tris_copy_string(vmu->context, context, sizeof(vmu->context));
		tris_copy_string(vmu->mailbox, usernm, sizeof(vmu->mailbox));
		
		populate_defaults(vmu);

		vmu->password[0] = '\0';
	}
	
	return vmu;
}

static int vm_user_exist(char *ext)
{
	char sql[256];
	char result[32];
	
	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE username='%s'", ext);
	sql_select_query_execute(result, sql);

	if(tris_strlen_zero(result)){
		return 0;
	}
	return 1;

}

static int cmdroom_exist(char *roomno)
{
	char sql[256];
	char result[32];
	
	snprintf(sql, sizeof(sql), "SELECT roomno FROM general_command WHERE roomno='%s'", roomno);
	sql_select_query_execute(result, sql);

	if (!tris_strlen_zero(result)) 
		return 1;
	return 0;
}

static int rprtroom_exist(char *roomno)
{
	char sql[256];
	char result[32];

	snprintf(sql, sizeof(sql), "SELECT roomno FROM report_listener WHERE roomno='%s'", roomno);
	sql_select_query_execute(result, sql);
	if (!tris_strlen_zero(result))
		return 1;
	return 0;
}

static int check_reporter(char *roomno, char *ext)
{
	char sql[256];
	char result[32];

	snprintf(sql, sizeof(sql), "SELECT reporter_uid FROM reporter WHERE roomno='%s' AND reporter_uid = '%s'", roomno, ext);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;
	return 1;
}

static int check_reporter_pin(char *ext, char *pin)
{
	char sql[256];
	char result[32];

	snprintf(sql, sizeof(sql), "SELECT pin FROM uri AS u LEFT JOIN credentials AS c on u.uid = c.uid WHERE username='%s'", ext);
	sql_select_query_execute(result, sql);

	if(!strcmp(result, pin)){
		return 1;
	}
	return 0;

}

static int vm_login(char *ext, char* password) 
{
	char sql[256];
	char result[32];
	
	snprintf(sql, sizeof(sql), "SELECT pin FROM uri AS u LEFT JOIN credentials AS c on u.uid = c.uid WHERE username='%s'", ext);
	sql_select_query_execute(result, sql);

	if(!strcmp(result, password)){
		return 1;
	}
	return 0;
}

static void apply_option(struct tris_vm_user *vmu, const char *var, const char *value)
{
	int x;
	if (!strcasecmp(var, "attach")) {
		tris_set2_flag(vmu, tris_true(value), VM_ATTACH);
	} else if (!strcasecmp(var, "attachfmt")) {
		tris_copy_string(vmu->attachfmt, value, sizeof(vmu->attachfmt));
	} else if (!strcasecmp(var, "serveremail")) {
		tris_copy_string(vmu->serveremail, value, sizeof(vmu->serveremail));
	} else if (!strcasecmp(var, "language")) {
		tris_copy_string(vmu->language, value, sizeof(vmu->language));
	} else if (!strcasecmp(var, "tz")) {
		tris_copy_string(vmu->zonetag, value, sizeof(vmu->zonetag));
#ifdef IMAP_STORAGE
	} else if (!strcasecmp(var, "imapuser")) {
		tris_copy_string(vmu->imapuser, value, sizeof(vmu->imapuser));
	} else if (!strcasecmp(var, "imappassword") || !strcasecmp(var, "imapsecret")) {
		tris_copy_string(vmu->imappassword, value, sizeof(vmu->imappassword));
#endif
	} else if (!strcasecmp(var, "delete") || !strcasecmp(var, "deletevoicemail")) {
		tris_set2_flag(vmu, tris_true(value), VM_DELETE);	
	} else if (!strcasecmp(var, "saycid")) {
		tris_set2_flag(vmu, tris_true(value), VM_SAYCID);	
	} else if (!strcasecmp(var, "sendvoicemail")) {
		tris_set2_flag(vmu, tris_true(value), VM_SVMAIL);	
	} else if (!strcasecmp(var, "review")) {
		tris_set2_flag(vmu, tris_true(value), VM_REVIEW);
	} else if (!strcasecmp(var, "tempgreetwarn")) {
		tris_set2_flag(vmu, tris_true(value), VM_TEMPGREETWARN);	
	} else if (!strcasecmp(var, "operator")) {
		tris_set2_flag(vmu, tris_true(value), VM_OPERATOR);	
	} else if (!strcasecmp(var, "envelope")) {
		tris_set2_flag(vmu, tris_true(value), VM_ENVELOPE);	
	} else if (!strcasecmp(var, "moveheard")) {
		tris_set2_flag(vmu, tris_true(value), VM_MOVEHEARD);
	} else if (!strcasecmp(var, "sayduration")) {
		tris_set2_flag(vmu, tris_true(value), VM_SAYDURATION);	
	} else if (!strcasecmp(var, "saydurationm")) {
		if (sscanf(value, "%d", &x) == 1) {
			vmu->saydurationm = x;
		} else {
			tris_log(LOG_WARNING, "Invalid min duration for say duration\n");
		}
	} else if (!strcasecmp(var, "forcename")) {
		tris_set2_flag(vmu, tris_true(value), VM_FORCENAME);	
	} else if (!strcasecmp(var, "forcegreetings")) {
		tris_set2_flag(vmu, tris_true(value), VM_FORCEGREET);	
	} else if (!strcasecmp(var, "callback")) {
		tris_copy_string(vmu->callback, value, sizeof(vmu->callback));
	} else if (!strcasecmp(var, "dialout")) {
		tris_copy_string(vmu->dialout, value, sizeof(vmu->dialout));
	} else if (!strcasecmp(var, "exitcontext")) {
		tris_copy_string(vmu->exit, value, sizeof(vmu->exit));
	} else if (!strcasecmp(var, "maxmessage") || !strcasecmp(var, "maxsecs")) {
		if (vmu->maxsecs <= 0) {
			tris_log(LOG_WARNING, "Invalid max message length of %s. Using global value %d\n", value, vmmaxsecs);
			vmu->maxsecs = vmmaxsecs;
		} else {
			vmu->maxsecs = atoi(value);
		}
		if (!strcasecmp(var, "maxmessage"))
			tris_log(LOG_WARNING, "Option 'maxmessage' has been deprecated in favor of 'maxsecs'.  Please make that change in your voicemail config.\n");
	} else if (!strcasecmp(var, "maxmsg")) {
		vmu->maxmsg = atoi(value);
		if (vmu->maxmsg <= 0) {
			tris_log(LOG_WARNING, "Invalid number of messages per folder maxmsg=%s. Using default value %d\n", value, MAXMSG);
			vmu->maxmsg = MAXMSG;
		} else if (vmu->maxmsg > MAXMSGLIMIT) {
			tris_log(LOG_WARNING, "Maximum number of messages per folder is %d. Cannot accept value maxmsg=%s\n", MAXMSGLIMIT, value);
			vmu->maxmsg = MAXMSGLIMIT;
		}
	} else if (!strcasecmp(var, "backupdeleted")) {
		if (sscanf(value, "%d", &x) == 1)
			vmu->maxdeletedmsg = x;
		else if (tris_true(value))
			vmu->maxdeletedmsg = MAXMSG;
		else
			vmu->maxdeletedmsg = MAXMSG;

		if (vmu->maxdeletedmsg < 0) {
			tris_log(LOG_WARNING, "Invalid number of deleted messages saved per mailbox backupdeleted=%s. Using default value %d\n", value, MAXMSG);
			vmu->maxdeletedmsg = MAXMSG;
		} else if (vmu->maxdeletedmsg > MAXMSGLIMIT) {
			tris_log(LOG_WARNING, "Maximum number of deleted messages saved per mailbox is %d. Cannot accept value backupdeleted=%s\n", MAXMSGLIMIT, value);
			vmu->maxdeletedmsg = MAXMSGLIMIT;
		}
	} else if (!strcasecmp(var, "volgain")) {
		sscanf(value, "%lf", &vmu->volgain);
	} else if (!strcasecmp(var, "options")) {
		apply_options(vmu, value);
	}
}

static int change_password_realtime(struct tris_vm_user *vmu, const char *password)
{
	int res;
	if (!tris_strlen_zero(vmu->uniqueid)) {
		res = tris_update_realtime("voicemail", "uniqueid", vmu->uniqueid, "password", password, NULL);
		if (res > 0) {
			tris_copy_string(vmu->password, password, sizeof(vmu->password));
			res = 0;
		} else if (!res) {
			res = -1;
		}
		return res;
	}
	return -1;
}

static void apply_options(struct tris_vm_user *vmu, const char *options)
{	/* Destructively Parse options and apply */
	char *stringp;
	char *s;
	char *var, *value;
	stringp = tris_strdupa(options);
	while ((s = strsep(&stringp, "|"))) {
		value = s;
		if ((var = strsep(&value, "=")) && value) {
			apply_option(vmu, var, value);
		}
	}	
}

static void apply_options_full(struct tris_vm_user *retval, struct tris_variable *var)
{
	struct tris_variable *tmp;
	tmp = var;
	while (tmp) {
		if (!strcasecmp(tmp->name, "vmsecret")) {
			tris_copy_string(retval->password, tmp->value, sizeof(retval->password));
		} else if (!strcasecmp(tmp->name, "secret") || !strcasecmp(tmp->name, "password")) { /* don't overwrite vmsecret if it exists */
			if (tris_strlen_zero(retval->password))
				tris_copy_string(retval->password, tmp->value, sizeof(retval->password));
		} else if (!strcasecmp(tmp->name, "uniqueid")) {
			tris_copy_string(retval->uniqueid, tmp->value, sizeof(retval->uniqueid));
		} else if (!strcasecmp(tmp->name, "pager")) {
			tris_copy_string(retval->pager, tmp->value, sizeof(retval->pager));
		} else if (!strcasecmp(tmp->name, "email")) {
			tris_copy_string(retval->email, tmp->value, sizeof(retval->email));
		} else if (!strcasecmp(tmp->name, "fullname")) {
			tris_copy_string(retval->fullname, tmp->value, sizeof(retval->fullname));
		} else if (!strcasecmp(tmp->name, "context")) {
			tris_copy_string(retval->context, tmp->value, sizeof(retval->context));
#ifdef IMAP_STORAGE
		} else if (!strcasecmp(tmp->name, "imapuser")) {
			tris_copy_string(retval->imapuser, tmp->value, sizeof(retval->imapuser));
		} else if (!strcasecmp(tmp->name, "imappassword") || !strcasecmp(tmp->name, "imapsecret")) {
			tris_copy_string(retval->imappassword, tmp->value, sizeof(retval->imappassword));
#endif
		} else
			apply_option(retval, tmp->name, tmp->value);
		tmp = tmp->next;
	} 
}

static int is_valid_dtmf(const char *key)
{
	int i;
	char *local_key = tris_strdupa(key);

	for (i = 0; i < strlen(key); ++i) {
		if (!strchr(VALID_DTMF, *local_key)) {
			tris_log(LOG_WARNING, "Invalid DTMF key \"%c\" used in voicemail configuration file\n", *local_key);
			return 0;
		}
		local_key++;
	}
	return 1;
}

static struct tris_vm_user *find_user_realtime(struct tris_vm_user *ivm, const char *context, const char *mailbox)
{
	struct tris_variable *var;
	struct tris_vm_user *retval;

	if ((retval = (ivm ? ivm : tris_calloc(1, sizeof(*retval))))) {
		if (!ivm)
			tris_set_flag(retval, VM_ALLOCED);	
		else
			memset(retval, 0, sizeof(*retval));
		if (mailbox) 
			tris_copy_string(retval->mailbox, mailbox, sizeof(retval->mailbox));
		populate_defaults(retval);
		if (!context && tris_test_flag((&globalflags), VM_SEARCH))
			var = tris_load_realtime("voicemail", "mailbox", mailbox, NULL);
		else
			var = tris_load_realtime("voicemail", "mailbox", mailbox, "context", context, NULL);
		if (var) {
			apply_options_full(retval, var);
			tris_variables_destroy(var);
		} else { 
			if (!ivm) 
				tris_free(retval);
			retval = NULL;
		}	
	} 
	return retval;
}

static struct tris_vm_user *find_user(struct tris_vm_user *ivm, const char *context, const char *mailbox)
{
	/* This function could be made to generate one from a database, too */
	struct tris_vm_user *vmu = NULL, *cur;
	TRIS_LIST_LOCK(&users);

	if (!context && !tris_test_flag((&globalflags), VM_SEARCH))
		context = "default";

	TRIS_LIST_TRAVERSE(&users, cur, list) {
		if (tris_test_flag((&globalflags), VM_SEARCH) && !strcasecmp(mailbox, cur->mailbox))
			break;
		if (context && (!strcasecmp(context, cur->context)) && (!strcasecmp(mailbox, cur->mailbox)))
			break;
	}
	if (cur) {
		/* Make a copy, so that on a reload, we have no race */
		if ((vmu = (ivm ? ivm : tris_malloc(sizeof(*vmu))))) {
			memcpy(vmu, cur, sizeof(*vmu));
			tris_set2_flag(vmu, !ivm, VM_ALLOCED);
			TRIS_LIST_NEXT(vmu, list) = NULL;
		}
	} else
		vmu = find_user_realtime(ivm, context, mailbox);
	TRIS_LIST_UNLOCK(&users);
	return vmu;
}

static int reset_user_pw(const char *context, const char *mailbox, const char *newpass)
{
	/* This function could be made to generate one from a database, too */
	struct tris_vm_user *cur;
	int res = -1;
	TRIS_LIST_LOCK(&users);
	TRIS_LIST_TRAVERSE(&users, cur, list) {
		if ((!context || !strcasecmp(context, cur->context)) &&
			(!strcasecmp(mailbox, cur->mailbox)))
				break;
	}
	if (cur) {
		tris_copy_string(cur->password, newpass, sizeof(cur->password));
		res = 0;
	}
	TRIS_LIST_UNLOCK(&users);
	return res;
}

static void vm_change_password(struct tris_vm_user *vmu, const char *newpassword)
{
	struct tris_config *cfg = NULL;
	struct tris_variable *var = NULL;
	struct tris_category *cat = NULL;
	char *category = NULL, *value = NULL, *new = NULL;
	const char *tmp = NULL;
	struct tris_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS };
	if (!change_password_realtime(vmu, newpassword))
		return;

	/* check voicemail.conf */
	if ((cfg = tris_config_load(VOICEMAIL_CONFIG, config_flags))) {
		while ((category = tris_category_browse(cfg, category))) {
			if (!strcasecmp(category, vmu->context)) {
				if (!(tmp = tris_variable_retrieve(cfg, category, vmu->mailbox))) {
					tris_log(LOG_WARNING, "We could not find the mailbox.\n");
					break;
				}
				value = strstr(tmp, ",");
				if (!value) {
					tris_log(LOG_WARNING, "variable has bad format.\n");
					break;
				}
				new = alloca(strlen(value) + strlen(newpassword) + 1);
				sprintf(new, "%s%s", newpassword, value);
				if (!(cat = tris_category_get(cfg, category))) {
					tris_log(LOG_WARNING, "Failed to get category structure.\n");
					break;
				}
				tris_variable_update(cat, vmu->mailbox, new, NULL, 0);
			}
		}
		/* save the results */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);
		tris_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		tris_config_text_file_save(VOICEMAIL_CONFIG, cfg, "AppVoicemail");
	}
	category = NULL;
	var = NULL;
	/* check users.conf and update the password stored for the mailbox*/
	/* if no vmsecret entry exists create one. */
	if ((cfg = tris_config_load("users.conf", config_flags))) {
		tris_debug(4, "we are looking for %s\n", vmu->mailbox);
		while ((category = tris_category_browse(cfg, category))) {
			tris_debug(4, "users.conf: %s\n", category);
			if (!strcasecmp(category, vmu->mailbox)) {
				if (!(tmp = tris_variable_retrieve(cfg, category, "vmsecret"))) {
					tris_debug(3, "looks like we need to make vmsecret!\n");
					var = tris_variable_new("vmsecret", newpassword, "");
				} 
				new = alloca(strlen(newpassword) + 1);
				sprintf(new, "%s", newpassword);
				if (!(cat = tris_category_get(cfg, category))) {
					tris_debug(4, "failed to get category!\n");
					break;
				}
				if (!var)		
					tris_variable_update(cat, "vmsecret", new, NULL, 0);
				else
					tris_variable_append(cat, var);
			}
		}
		/* save the results and clean things up */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);	
		tris_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		tris_config_text_file_save("users.conf", cfg, "AppVoicemail");
	}
}

static void vm_change_password_shell(struct tris_vm_user *vmu, char *newpassword)
{
	char buf[255];
	snprintf(buf, sizeof(buf), "%s %s %s %s", ext_pass_cmd, vmu->context, vmu->mailbox, newpassword);
	if (!tris_safe_system(buf)) {
		tris_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		/* Reset the password in memory, too */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);
	}
}

static int make_dir(char *dest, int len, const char *context, const char *ext, const char *folder)
{
	return snprintf(dest, len, "%s%s/%s/%s", VM_SPOOL_DIR, context, ext, folder);
}

/*! 
 * \brief Creates a file system path expression for a folder within the voicemail data folder and the appropriate context.
 * \param dest The variable to hold the output generated path expression. This buffer should be of size PATH_MAX.
 * \param len The length of the path string that was written out.
 * 
 * The path is constructed as 
 * 	VM_SPOOL_DIRcontext/ext/folder
 *
 * \return zero on success, -1 on error.
 */
static int make_file(char *dest, const int len, const char *dir, const int num)
{
	return snprintf(dest, len, "%s/msg%04d", dir, num);
}

/* same as mkstemp, but return a FILE * */
static FILE *vm_mkftemp(char *template)
{
	FILE *p = NULL;
	int pfd = mkstemp(template);
	chmod(template, VOICEMAIL_FILE_MODE & ~my_umask);
	if (pfd > -1) {
		p = fdopen(pfd, "w+");
		if (!p) {
			close(pfd);
			pfd = -1;
		}
	}
	return p;
}

/*! \brief basically mkdir -p $dest/$context/$ext/$folder
 * \param dest    String. base directory.
 * \param len     Length of dest.
 * \param context String. Ignored if is null or empty string.
 * \param ext     String. Ignored if is null or empty string.
 * \param folder  String. Ignored if is null or empty string. 
 * \return -1 on failure, 0 on success.
 */
static int create_dirpath(char *dest, int len, const char *context, const char *ext, const char *folder)
{
	mode_t	mode = VOICEMAIL_DIR_MODE;
	int res;

	make_dir(dest, len, context, ext, folder);
	if ((res = tris_mkdir(dest, mode))) {
		tris_log(LOG_WARNING, "tris_mkdir '%s' failed: %s\n", dest, strerror(res));
		return -1;
	}
	return 0;
}

static const char *mbox(int id)
{
	static const char *msgs[] = {
#ifdef IMAP_STORAGE
		imapfolder,
#else
		"INBOX",
#endif
		"OLD",
		"SAVED",
		"DELETED",
		"Work",
		"Family",
		"Friends",
		"Cust1",
		"Cust2",
		"Cust3",
		"Cust4",
		"Cust5",
		"Urgent"
	};
	return (id >= 0 && id < (sizeof(msgs)/sizeof(msgs[0]))) ? msgs[id] : "Unknown";
}

static void free_user(struct tris_vm_user *vmu)
{
	if (tris_test_flag(vmu, VM_ALLOCED))
		tris_free(vmu);
}

/* All IMAP-specific functions should go in this block. This
* keeps them from being spread out all over the code */
#ifdef IMAP_STORAGE
static void vm_imap_delete(int msgnum, struct tris_vm_user *vmu)
{
	char arg[10];
	struct vm_state *vms;
	unsigned long messageNum;

	/* Greetings aren't stored in IMAP, so we can't delete them there */
	if (msgnum < 0) {
		return;
	}

	if (!(vms = get_vm_state_by_mailbox(vmu->mailbox, 1)) && !(vms = get_vm_state_by_mailbox(vmu->mailbox, 0))) {
		tris_log(LOG_WARNING, "Couldn't find a vm_state for mailbox %s. Unable to set \\DELETED flag for message %d\n", vmu->mailbox, msgnum);
		return;
	}

	/* find real message number based on msgnum */
	/* this may be an index into vms->msgArray based on the msgnum. */
	messageNum = vms->msgArray[msgnum];
	if (messageNum == 0) {
		tris_log(LOG_WARNING, "msgnum %d, mailbox message %lu is zero.\n", msgnum, messageNum);
		return;
	}
	tris_debug(3, "deleting msgnum %d, which is mailbox message %lu\n", msgnum, messageNum);
	/* delete message */
	snprintf(arg, sizeof(arg), "%lu", messageNum);
	mail_setflag(vms->mailstream, arg, "\\DELETED");
}

static int imap_retrieve_greeting (const char *dir, const int msgnum, struct tris_vm_user *vmu)
{
	struct vm_state *vms_p;
	char *file, *filename;
	char *attachment;
	int ret = 0, i;
	BODY *body;

	/* This function is only used for retrieval of IMAP greetings
	* regular messages are not retrieved this way, nor are greetings
	* if they are stored locally*/
	if (msgnum > -1 || !imapgreetings) {
		return 0;
	} else {
		file = strrchr(tris_strdupa(dir), '/');
		if (file)
			*file++ = '\0';
		else {
			tris_debug (1, "Failed to procure file name from directory passed.\n");
			return -1;
		}
	}

	/* check if someone is accessing this box right now... */
	if (!(vms_p = get_vm_state_by_mailbox(vmu->mailbox, 1)) ||!(vms_p = get_vm_state_by_mailbox(vmu->mailbox, 0))) {
		tris_log(LOG_ERROR, "Voicemail state not found!\n");
		return -1;
	}
	
	ret = init_mailstream(vms_p, GREETINGS_FOLDER);
	if (!vms_p->mailstream) {
		tris_log(LOG_ERROR, "IMAP mailstream is NULL\n");
		return -1;
	}

	/*XXX Yuck, this could probably be done a lot better */
	for (i = 0; i < vms_p->mailstream->nmsgs; i++) {
		mail_fetchstructure(vms_p->mailstream, i + 1, &body);
		/* We have the body, now we extract the file name of the first attachment. */
		if (body->nested.part && body->nested.part->next && body->nested.part->next->body.parameter->value) {
			attachment = tris_strdupa(body->nested.part->next->body.parameter->value);
		} else {
			tris_log(LOG_ERROR, "There is no file attached to this IMAP message.\n");
			return -1;
		}
		filename = strsep(&attachment, ".");
		if (!strcmp(filename, file)) {
			tris_copy_string(vms_p->fn, dir, sizeof(vms_p->fn));
			vms_p->msgArray[vms_p->curmsg] = i + 1;
			save_body(body, vms_p, "2", attachment);
			return 0;
		}
	}

	return -1;
}

static int imap_retrieve_file(const char *dir, const int msgnum, const char *mailbox, const char *context)
{
	BODY *body;
	char *header_content;
	char *attachedfilefmt;
	char buf[80];
	struct vm_state *vms;
	char text_file[PATH_MAX];
	FILE *text_file_ptr;
	int res = 0;
	struct tris_vm_user *vmu;

	if (!(vmu = find_user(NULL, context, mailbox))) {
		tris_log(LOG_WARNING, "Couldn't find user with mailbox %s@%s\n", mailbox, context);
		return -1;
	}
	
	if (msgnum < 0) {
		if (imapgreetings) {
			res = imap_retrieve_greeting(dir, msgnum, vmu);
			goto exit;
		} else {
			res = 0;
			goto exit;
		}
	}

	/* Before anything can happen, we need a vm_state so that we can
	* actually access the imap server through the vms->mailstream
	*/
	if (!(vms = get_vm_state_by_mailbox(vmu->mailbox, 1)) && !(vms = get_vm_state_by_mailbox(vmu->mailbox, 0))) {
		/* This should not happen. If it does, then I guess we'd
		* need to create the vm_state, extract which mailbox to
		* open, and then set up the msgArray so that the correct
		* IMAP message could be accessed. If I have seen correctly
		* though, the vms should be obtainable from the vmstates list
		* and should have its msgArray properly set up.
		*/
		tris_log(LOG_ERROR, "Couldn't find a vm_state for mailbox %s!!! Oh no!\n", vmu->mailbox);
		res = -1;
		goto exit;
	}
	
	make_file(vms->fn, sizeof(vms->fn), dir, msgnum);

	/* Don't try to retrieve a message from IMAP if it already is on the file system */
	if (tris_fileexists(vms->fn, NULL, NULL) > 0) {
		res = 0;
		goto exit;
	}

	if (option_debug > 2)
		tris_log (LOG_DEBUG,"Before mail_fetchheaders, curmsg is: %d, imap messages is %lu\n", msgnum, vms->msgArray[msgnum]);
	if (vms->msgArray[msgnum] == 0) {
		tris_log (LOG_WARNING,"Trying to access unknown message\n");
		res = -1;
		goto exit;
	}

	/* This will only work for new messages... */
	header_content = mail_fetchheader (vms->mailstream, vms->msgArray[msgnum]);
	/* empty string means no valid header */
	if (tris_strlen_zero(header_content)) {
		tris_log (LOG_ERROR,"Could not fetch header for message number %ld\n",vms->msgArray[msgnum]);
		res = -1;
		goto exit;
	}

	mail_fetchstructure (vms->mailstream,vms->msgArray[msgnum],&body);
	
	/* We have the body, now we extract the file name of the first attachment. */
	if (body->nested.part && body->nested.part->next && body->nested.part->next->body.parameter->value) {
		attachedfilefmt = tris_strdupa(body->nested.part->next->body.parameter->value);
	} else {
		tris_log(LOG_ERROR, "There is no file attached to this IMAP message.\n");
		res = -1;
		goto exit;
	}
	
	/* Find the format of the attached file */

	strsep(&attachedfilefmt, ".");
	if (!attachedfilefmt) {
		tris_log(LOG_ERROR, "File format could not be obtained from IMAP message attachment\n");
		res = -1;
		goto exit;
	}
	
	save_body(body, vms, "2", attachedfilefmt);

	/* Get info from headers!! */
	snprintf(text_file, sizeof(text_file), "%s.%s", vms->fn, "txt");

	if (!(text_file_ptr = fopen(text_file, "w"))) {
		tris_log(LOG_WARNING, "Unable to open/create file %s: %s\n", text_file, strerror(errno));
	}

	fprintf(text_file_ptr, "%s\n", "[message]");

	get_header_by_tag(header_content, "X-Trismedia-VM-Caller-ID-Name:", buf, sizeof(buf));
	fprintf(text_file_ptr, "callerid=\"%s\" ", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Trismedia-VM-Caller-ID-Num:", buf, sizeof(buf));
	fprintf(text_file_ptr, "<%s>\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Trismedia-VM-Context:", buf, sizeof(buf));
	fprintf(text_file_ptr, "context=%s\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Trismedia-VM-Orig-time:", buf, sizeof(buf));
	fprintf(text_file_ptr, "origtime=%s\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Trismedia-VM-Duration:", buf, sizeof(buf));
	fprintf(text_file_ptr, "duration=%s\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Trismedia-VM-Category:", buf, sizeof(buf));
	fprintf(text_file_ptr, "category=%s\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Trismedia-VM-Flag:", buf, sizeof(buf));
	fprintf(text_file_ptr, "flag=%s\n", S_OR(buf, ""));
	fclose(text_file_ptr);

exit:
	free_user(vmu);
	return res;
}

static int folder_int(const char *folder)
{
	/*assume a NULL folder means INBOX*/
	if (!folder)
		return 0;
	if (!strcasecmp(folder, "INBOX"))
		return 0;
	else if (!strcasecmp(folder, "OLD"))
		return 1;
	else if (!strcasecmp(folder, "SAVED"))
		return 2;
	else if (!strcasecmp(folder, "DELETED"))
		return 3;
	else if (!strcasecmp(folder, "Work"))
		return 4;
	else if (!strcasecmp(folder, "Family"))
		return 5;
	else if (!strcasecmp(folder, "Friends"))
		return 6;
	else if (!strcasecmp(folder, "Cust1"))
		return 7;
	else if (!strcasecmp(folder, "Cust2"))
		return 8;
	else if (!strcasecmp(folder, "Cust3"))
		return 9;
	else if (!strcasecmp(folder, "Cust4"))
		return 10;
	else if (!strcasecmp(folder, "Cust5"))
		return 11;
	else /*assume they meant INBOX if folder is not found otherwise*/
		return 0;
}

static int imap_store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum, struct tris_channel *chan, struct tris_vm_user *vmu, char *fmt, int duration, struct vm_state *vms)
{
	char *myserveremail = serveremail;
	char fn[PATH_MAX];
	char mailbox[256];
	char *stringp;
	FILE *p = NULL;
	char tmp[80] = "/tmp/astmail-XXXXXX";
	long len;
	void *buf;
	int tempcopy = 0;
	STRING str;
	
	/* Attach only the first format */
	fmt = tris_strdupa(fmt);
	stringp = fmt;
	strsep(&stringp, "|");

	if (!tris_strlen_zero(vmu->serveremail))
		myserveremail = vmu->serveremail;

	if (msgnum > -1)
		make_file(fn, sizeof(fn), dir, msgnum);
	else
		tris_copy_string (fn, dir, sizeof(fn));
	
	if (tris_strlen_zero(vmu->email)) {
		/* We need the vmu->email to be set when we call make_email_file, but
		* if we keep it set, a duplicate e-mail will be created. So at the end
		* of this function, we will revert back to an empty string if tempcopy
		* is 1.
		*/
		tris_copy_string(vmu->email, vmu->imapuser, sizeof(vmu->email));
		tempcopy = 1;
	}

	if (!strcmp(fmt, "wav49"))
		fmt = "WAV";
	tris_debug(3, "Storing file '%s', format '%s'\n", fn, fmt);

	/* Make a temporary file instead of piping directly to sendmail, in case the mail
	command hangs. */
	if (!(p = vm_mkftemp(tmp))) {
		tris_log(LOG_WARNING, "Unable to store '%s' (can't create temporary file)\n", fn);
		if (tempcopy)
			*(vmu->email) = '\0';
		return -1;
	}

	if (msgnum < 0 && imapgreetings) {
		init_mailstream(vms, GREETINGS_FOLDER);
		imap_delete_old_greeting(fn, vms);
	}
	
	make_email_file(p, myserveremail, vmu, msgnum, vmu->context, vmu->mailbox, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL), fn, fmt, duration, 1, chan, NULL, 1);
	/* read mail file to memory */		
	len = ftell(p);
	rewind(p);
	if (!(buf = tris_malloc(len + 1))) {
		tris_log(LOG_ERROR, "Can't allocate %ld bytes to read message\n", len + 1);
		fclose(p);
		if (tempcopy)
			*(vmu->email) = '\0';
		return -1;
	}
	fread(buf, len, 1, p);
	((char *)buf)[len] = '\0';
	INIT(&str, mail_string, buf, len);
	init_mailstream(vms, NEW_FOLDER);
	imap_mailbox_name(mailbox, sizeof(mailbox), vms, NEW_FOLDER, 1);
	if (!mail_append(vms->mailstream, mailbox, &str))
		tris_log(LOG_ERROR, "Error while sending the message to %s\n", mailbox);
	fclose(p);
	unlink(tmp);
	tris_free(buf);
	tris_debug(3, "%s stored\n", fn);
	
	if (tempcopy)
		*(vmu->email) = '\0';
	
	return 0;

}

static int messagecount(const char *context, const char *mailbox, const char *folder)
{
	SEARCHPGM *pgm;
	SEARCHHEADER *hdr;

	struct tris_vm_user *vmu, vmus;
	struct vm_state *vms_p;
	int ret = 0;
	int fold = folder_int(folder);
	
	if (tris_strlen_zero(mailbox))
		return 0;

	/* We have to get the user before we can open the stream! */
	/* tris_log(LOG_DEBUG, "Before find_user, context is %s and mailbox is %s\n", context, mailbox); */
	vmu = find_user(&vmus, context, mailbox);
	if (!vmu) {
		tris_log(LOG_ERROR, "Couldn't find mailbox %s in context %s\n", mailbox, context);
		return -1;
	} else {
		/* No IMAP account available */
		if (vmu->imapuser[0] == '\0') {
			tris_log(LOG_WARNING, "IMAP user not set for mailbox %s\n", vmu->mailbox);
			return -1;
		}
	}
	
	/* No IMAP account available */
	if (vmu->imapuser[0] == '\0') {
		tris_log(LOG_WARNING, "IMAP user not set for mailbox %s\n", vmu->mailbox);
		free_user(vmu);
		return -1;
	}

	/* check if someone is accessing this box right now... */
	vms_p = get_vm_state_by_imapuser(vmu->imapuser, 1);
	if (!vms_p) {
		vms_p = get_vm_state_by_mailbox(mailbox, 1);
	}
	if (vms_p) {
		tris_debug(3, "Returning before search - user is logged in\n");
		if (fold == 0) { /* INBOX */
			return vms_p->newmessages;
		}
		if (fold == 1) { /* Old messages */
			return vms_p->oldmessages;
		}
	}

	/* add one if not there... */
	vms_p = get_vm_state_by_imapuser(vmu->imapuser, 0);
	if (!vms_p) {
		vms_p = get_vm_state_by_mailbox(mailbox, 0);
	}

	if (!vms_p) {
		tris_debug(3, "Adding new vmstate for %s\n", vmu->imapuser);
		if (!(vms_p = tris_calloc(1, sizeof(*vms_p)))) {
			return -1;
		}
		tris_copy_string(vms_p->imapuser, vmu->imapuser, sizeof(vms_p->imapuser));
		tris_copy_string(vms_p->username, mailbox, sizeof(vms_p->username)); /* save for access from interactive entry point */
		vms_p->mailstream = NIL; /* save for access from interactive entry point */
		tris_debug(3, "Copied %s to %s\n", vmu->imapuser, vms_p->imapuser);
		vms_p->updated = 1;
		/* set mailbox to INBOX! */
		tris_copy_string(vms_p->curbox, mbox(fold), sizeof(vms_p->curbox));
		init_vm_state(vms_p);
		vmstate_insert(vms_p);
	}
	ret = init_mailstream(vms_p, fold);
	if (!vms_p->mailstream) {
		tris_log(LOG_ERROR, "Houston we have a problem - IMAP mailstream is NULL\n");
		return -1;
	}
	if (ret == 0) {
		pgm = mail_newsearchpgm ();
		hdr = mail_newsearchheader ("X-Trismedia-VM-Extension", (char *)mailbox);
		pgm->header = hdr;
		if (fold != 1) {
			pgm->unseen = 1;
			pgm->seen = 0;
		}
		/* In the special case where fold is 1 (old messages) we have to do things a bit
		* differently. Old messages are stored in the INBOX but are marked as "seen"
		*/
		else {
			pgm->unseen = 0;
			pgm->seen = 1;
		}
		pgm->undeleted = 1;
		pgm->deleted = 0;

		vms_p->vmArrayIndex = 0;
		mail_search_full (vms_p->mailstream, NULL, pgm, NIL);
		if (fold == 0)
			vms_p->newmessages = vms_p->vmArrayIndex;
		if (fold == 1)
			vms_p->oldmessages = vms_p->vmArrayIndex;
		/* Freeing the searchpgm also frees the searchhdr */
		mail_free_searchpgm(&pgm);
		vms_p->updated = 0;
		return vms_p->vmArrayIndex;
	} else {  
		mail_ping(vms_p->mailstream);
	}
	return 0;
}
static int inboxcount(const char *mailbox_context, int *newmsgs, int *oldmsgs)
{
	char tmp[PATH_MAX] = "";
	char *mailboxnc;
	char *context;
	char *mb;
	char *cur;
	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;

	tris_debug(3, "Mailbox is set to %s\n", mailbox_context);
	/* If no mailbox, return immediately */
	if (tris_strlen_zero(mailbox_context))
		return 0;
	
	tris_copy_string(tmp, mailbox_context, sizeof(tmp));
	context = strchr(tmp, '@');
	if (strchr(mailbox_context, ',')) {
		int tmpnew, tmpold;
		tris_copy_string(tmp, mailbox_context, sizeof(tmp));
		mb = tmp;
		while ((cur = strsep(&mb, ", "))) {
			if (!tris_strlen_zero(cur)) {
				if (inboxcount(cur, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
					return -1;
				else {
					if (newmsgs)
						*newmsgs += tmpnew; 
					if (oldmsgs)
						*oldmsgs += tmpold;
				}
			}
		}
		return 0;
	}
	if (context) {
		*context = '\0';
		mailboxnc = tmp;
		context++;
	} else {
		context = "default";
		mailboxnc = (char *)mailbox_context;
	}
	if (newmsgs) {
		if ((*newmsgs = messagecount(context, mailboxnc, imapfolder)) < 0)
			return -1;
	}
	if (oldmsgs) {
		if ((*oldmsgs = messagecount(context, mailboxnc, "Old")) < 0)
			return -1;
	}
	return 0;
}

/*!
 * \brief Gets the number of messages that exist in the inbox folder.
 * \param mailbox_context
 * \param newmsgs The variable that is updated with the count of new messages within this inbox.
 * \param oldmsgs The variable that is updated with the count of old messages within this inbox.
 * \param urgentmsgs The variable that is updated with the count of urgent messages within this inbox.
 * 
 * This method is used when IMAP backend is used.
 * Simultaneously determines the count of new,old, and urgent messages. The total messages would then be the sum of these three.
 *
 * \return zero on success, -1 on error.
 */

static int inboxcount2(const char *mailbox_context, int *urgentmsgs, int *newmsgs, int *oldmsgs)
{
	char tmp[PATH_MAX] = "";
	char *mailboxnc;
	char *context;
	char *mb;
	char *cur;
	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;
	if (urgentmsgs)
		*urgentmsgs = 0;

	tris_debug(3,"Mailbox is set to %s\n",mailbox_context);
	/* If no mailbox, return immediately */
	if (tris_strlen_zero(mailbox_context))
		return 0;
	
	tris_copy_string(tmp, mailbox_context, sizeof(tmp));
	context = strchr(tmp, '@');
	if (strchr(mailbox_context, ',')) {
		int tmpnew, tmpold, tmpurgent;
		tris_copy_string(tmp, mailbox_context, sizeof(tmp));
		mb = tmp;
		while ((cur = strsep(&mb, ", "))) {
			if (!tris_strlen_zero(cur)) {
				if (inboxcount2(cur, urgentmsgs ? &tmpurgent : NULL, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
					return -1;
				else {
					if (newmsgs)
						*newmsgs += tmpnew; 
					if (oldmsgs)
						*oldmsgs += tmpold;
					if (urgentmsgs)
						*urgentmsgs += tmpurgent;
				}
			}
		}
		return 0;
	}
	if (context) {
		*context = '\0';
		mailboxnc = tmp;
		context++;
	} else {
		context = "default";
		mailboxnc = (char *)mailbox_context;
	}
	if (newmsgs) {
		if ((*newmsgs = __messagecount(context, mailboxnc, imapfolder)) < 0) {
			return -1;
		}
	}
	if (oldmsgs) {
		if ((*oldmsgs = __messagecount(context, mailboxnc, "Old")) < 0) {
			return -1;
		}
	}
	if (urgentmsgs) {
		if ((*urgentmsgs = __messagecount(context, mailboxnc, "Urgent")) < 0) {
			return -1;
		}
	}
	return 0;
}

/** 
* \brief Determines if the given folder has messages.
* \param mailbox The @ delimited string for user@context. If no context is found, uses 'default' for the context.
* \param folder the folder to look in
*
* This function is used when the mailbox is stored in an IMAP back end.
* This invokes the messagecount(). Here we are interested in the presence of messages (> 0) only, not the actual count.
* \return 1 if the folder has one or more messages. zero otherwise.
*/

static int has_voicemail(const char *mailbox, const char *folder)
{
	char tmp[256], *tmp2, *mbox, *context;
	tris_copy_string(tmp, mailbox, sizeof(tmp));
	tmp2 = tmp;
	if (strchr(tmp2, ',')) {
		while ((mbox = strsep(&tmp2, ","))) {
			if (!tris_strlen_zero(mbox)) {
				if (has_voicemail(mbox, folder))
					return 1;
			}
		}
	}
	if ((context= strchr(tmp, '@')))
		*context++ = '\0';
	else
		context = "default";
	return messagecount(context, tmp, folder) ? 1 : 0;
}

/*!
* \brief Copies a message from one mailbox to another.
* \param chan
* \param vmu
* \param imbox
* \param msgnum
* \param duration
* \param recip
* \param fmt
* \param dir
*
* This works with IMAP storage based mailboxes.
*
* \return zero on success, -1 on error.
*/
static int copy_message(struct tris_channel *chan, struct tris_vm_user *vmu, int imbox, int msgnum, long duration, struct tris_vm_user *recip, char *fmt, char *dir)
{
	struct vm_state *sendvms = NULL, *destvms = NULL;
	char messagestring[10]; /*I guess this could be a problem if someone has more than 999999999 messages...*/
	if (msgnum >= recip->maxmsg) {
		tris_log(LOG_WARNING, "Unable to copy mail, mailbox %s is full\n", recip->mailbox);
		return -1;
	}
	if (!(sendvms = get_vm_state_by_imapuser(vmu->imapuser, 0))) {
		tris_log(LOG_ERROR, "Couldn't get vm_state for originator's mailbox!!\n");
		return -1;
	}
	if (!(destvms = get_vm_state_by_imapuser(recip->imapuser, 0))) {
		tris_log(LOG_ERROR, "Couldn't get vm_state for destination mailbox!\n");
		return -1;
	}
	snprintf(messagestring, sizeof(messagestring), "%ld", sendvms->msgArray[msgnum]);
	if ((mail_copy(sendvms->mailstream, messagestring, (char *) mbox(imbox)) == T))
		return 0;
	tris_log(LOG_WARNING, "Unable to copy message from mailbox %s to mailbox %s\n", vmu->mailbox, recip->mailbox);
	return -1;
}

static void imap_mailbox_name(char *spec, size_t len, struct vm_state *vms, int box, int use_folder)
{
	char tmp[256], *t = tmp;
	size_t left = sizeof(tmp);
	
	if (box == OLD_FOLDER) {
		tris_copy_string(vms->curbox, mbox(NEW_FOLDER), sizeof(vms->curbox));
	} else {
		tris_copy_string(vms->curbox, mbox(box), sizeof(vms->curbox));
	}

	if (box == NEW_FOLDER) {
		tris_copy_string(vms->vmbox, "voicemail/vm-INBOX", sizeof(vms->vmbox));
	} else {
		snprintf(vms->vmbox, sizeof(vms->vmbox), "voicemail/vm-%s", mbox(box));
	}

	/* Build up server information */
	tris_build_string(&t, &left, "{%s:%s/imap", imapserver, imapport);

	/* Add authentication user if present */
	if (!tris_strlen_zero(authuser))
		tris_build_string(&t, &left, "/authuser=%s", authuser);

	/* Add flags if present */
	if (!tris_strlen_zero(imapflags))
		tris_build_string(&t, &left, "/%s", imapflags);

	/* End with username */
	tris_build_string(&t, &left, "/user=%s}", vms->imapuser);
	if (box == NEW_FOLDER || box == OLD_FOLDER)
		snprintf(spec, len, "%s%s", tmp, use_folder? imapfolder: "INBOX");
	else if (box == GREETINGS_FOLDER)
		snprintf(spec, len, "%s%s", tmp, greetingfolder);
	else {	/* Other folders such as Friends, Family, etc... */
		if (!tris_strlen_zero(imapparentfolder)) {
			/* imapparentfolder would typically be set to INBOX */
			snprintf(spec, len, "%s%s%c%s", tmp, imapparentfolder, delimiter, mbox(box));
		} else {
			snprintf(spec, len, "%s%s", tmp, mbox(box));
		}
	}
}

static int init_mailstream(struct vm_state *vms, int box)
{
	MAILSTREAM *stream = NIL;
	long debug;
	char tmp[256];
	
	if (!vms) {
		tris_log (LOG_ERROR,"vm_state is NULL!\n");
		return -1;
	}
	if (option_debug > 2)
		tris_log (LOG_DEBUG,"vm_state user is:%s\n",vms->imapuser);
	if (vms->mailstream == NIL || !vms->mailstream) {
		if (option_debug)
			tris_log (LOG_DEBUG,"mailstream not set.\n");
	} else {
		stream = vms->mailstream;
	}
	/* debug = T;  user wants protocol telemetry? */
	debug = NIL;  /* NO protocol telemetry? */

	if (delimiter == '\0') {		/* did not probe the server yet */
		char *cp;
#ifdef USE_SYSTEM_IMAP
#include <imap/linkage.c>
#elif defined(USE_SYSTEM_CCLIENT)
#include <c-client/linkage.c>
#else
#include "linkage.c"
#endif
		/* Connect to INBOX first to get folders delimiter */
		imap_mailbox_name(tmp, sizeof(tmp), vms, 0, 1);
		tris_mutex_lock(&vms->lock);
		stream = mail_open (stream, tmp, debug ? OP_DEBUG : NIL);
		tris_mutex_unlock(&vms->lock);
		if (stream == NIL) {
			tris_log (LOG_ERROR, "Can't connect to imap server %s\n", tmp);
			return -1;
		}
		get_mailbox_delimiter(stream);
		/* update delimiter in imapfolder */
		for (cp = imapfolder; *cp; cp++)
			if (*cp == '/')
				*cp = delimiter;
	}
	/* Now connect to the target folder */
	imap_mailbox_name(tmp, sizeof(tmp), vms, box, 1);
	if (option_debug > 2)
		tris_log (LOG_DEBUG,"Before mail_open, server: %s, box:%d\n", tmp, box);
	tris_mutex_lock(&vms->lock);
	vms->mailstream = mail_open (stream, tmp, debug ? OP_DEBUG : NIL);
	tris_mutex_unlock(&vms->lock);
	if (vms->mailstream == NIL) {
		return -1;
	} else {
		return 0;
	}
}

static int open_mailbox(struct vm_state *vms, struct tris_vm_user *vmu, int box)
{
	SEARCHPGM *pgm;
	SEARCHHEADER *hdr;
	int ret, urgent = 0;

	/* If Urgent, then look at INBOX */
	if (box == 11) {
		box = NEW_FOLDER;
		urgent = 1;
	}

	tris_copy_string(vms->imapuser,vmu->imapuser, sizeof(vms->imapuser));
	tris_debug(3,"Before init_mailstream, user is %s\n",vmu->imapuser);

	if ((ret = init_mailstream(vms, box)) || !vms->mailstream) {
		tris_log(LOG_ERROR, "Could not initialize mailstream\n");
		return -1;
	}
	
	create_dirpath(vms->curdir, sizeof(vms->curdir), vmu->context, vms->username, vms->curbox);
	
	/* Check Quota */
	if  (box == 0)  {
		tris_debug(3, "Mailbox name set to: %s, about to check quotas\n", mbox(box));
		check_quota(vms,(char *)mbox(box));
	}

	pgm = mail_newsearchpgm();

	/* Check IMAP folder for Trismedia messages only... */
	hdr = mail_newsearchheader("X-Trismedia-VM-Extension", vmu->mailbox);
	pgm->header = hdr;
	pgm->deleted = 0;
	pgm->undeleted = 1;

	/* if box = NEW_FOLDER, check for new, if box = OLD_FOLDER, check for read */
	if (box == NEW_FOLDER && urgent == 1) {
		pgm->unseen = 1;
		pgm->seen = 0;
		pgm->flagged = 1;
		pgm->unflagged = 0;
	} else if (box == NEW_FOLDER && urgent == 0) {
		pgm->unseen = 1;
		pgm->seen = 0;
		pgm->flagged = 0;
		pgm->unflagged = 1;
	} else if (box == OLD_FOLDER) {
		pgm->seen = 1;
		pgm->unseen = 0;
	}

	tris_debug(3,"Before mail_search_full, user is %s\n",vmu->imapuser);

	vms->vmArrayIndex = 0;
	mail_search_full (vms->mailstream, NULL, pgm, NIL);
	vms->lastmsg = vms->vmArrayIndex - 1;
	mail_free_searchpgm(&pgm);

	return 0;
}

static void write_file(char *filename, char *buffer, unsigned long len)
{
	FILE *output;

	output = fopen (filename, "w");
	fwrite (buffer, len, 1, output);
	fclose (output);
}

static void update_messages_by_imapuser(const char *user, unsigned long number)
{
	struct vmstate *vlist = NULL;

	TRIS_LIST_LOCK(&vmstates);
	TRIS_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			tris_debug(3, "error: vms is NULL for %s\n", user);
			continue;
		}
		if (!vlist->vms->imapuser) {
			tris_debug(3, "error: imapuser is NULL for %s\n", user);
			continue;
		}
		tris_debug(3, "saving mailbox message number %lu as message %d. Interactive set to %d\n", number, vlist->vms->vmArrayIndex, vlist->vms->interactive);
		vlist->vms->msgArray[vlist->vms->vmArrayIndex++] = number;
	}
	TRIS_LIST_UNLOCK(&vmstates);
}

void mm_searched(MAILSTREAM *stream, unsigned long number)
{
	char *mailbox = stream->mailbox, buf[1024] = "", *user;

	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))))
		return;

	update_messages_by_imapuser(user, number);
}

static struct tris_vm_user *find_user_realtime_imapuser(const char *imapuser)
{
	struct tris_variable *var;
	struct tris_vm_user *vmu;

	vmu = tris_calloc(1, sizeof *vmu);
	if (!vmu)
		return NULL;
	tris_set_flag(vmu, VM_ALLOCED);
	populate_defaults(vmu);

	var = tris_load_realtime("voicemail", "imapuser", imapuser, NULL);
	if (var) {
		apply_options_full(vmu, var);
		tris_variables_destroy(var);
		return vmu;
	} else {
		free(vmu);
		return NULL;
	}
}

/* Interfaces to C-client */

void mm_exists(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if new mail! */
	tris_debug(4, "Entering EXISTS callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_expunged(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if expunged mail! */
	tris_debug(4, "Entering EXPUNGE callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_flags(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if read mail! */
	tris_debug(4, "Entering FLAGS callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_notify(MAILSTREAM * stream, char *string, long errflg)
{
	tris_debug(5, "Entering NOTIFY callback, errflag is %ld, string is %s\n", errflg, string);
	mm_log (string, errflg);
}


void mm_list(MAILSTREAM * stream, int delim, char *mailbox, long attributes)
{
	if (delimiter == '\0') {
		delimiter = delim;
	}

	tris_debug(5, "Delimiter set to %c and mailbox %s\n",delim, mailbox);
	if (attributes & LATT_NOINFERIORS)
		tris_debug(5, "no inferiors\n");
	if (attributes & LATT_NOSELECT)
		tris_debug(5, "no select\n");
	if (attributes & LATT_MARKED)
		tris_debug(5, "marked\n");
	if (attributes & LATT_UNMARKED)
		tris_debug(5, "unmarked\n");
}


void mm_lsub(MAILSTREAM * stream, int delimiter, char *mailbox, long attributes)
{
	tris_debug(5, "Delimiter set to %c and mailbox %s\n",delimiter, mailbox);
	if (attributes & LATT_NOINFERIORS)
		tris_debug(5, "no inferiors\n");
	if (attributes & LATT_NOSELECT)
		tris_debug(5, "no select\n");
	if (attributes & LATT_MARKED)
		tris_debug(5, "marked\n");
	if (attributes & LATT_UNMARKED)
		tris_debug(5, "unmarked\n");
}


void mm_status(MAILSTREAM * stream, char *mailbox, MAILSTATUS * status)
{
	tris_log(LOG_NOTICE, " Mailbox %s", mailbox);
	if (status->flags & SA_MESSAGES)
		tris_log(LOG_NOTICE, ", %lu messages", status->messages);
	if (status->flags & SA_RECENT)
		tris_log(LOG_NOTICE, ", %lu recent", status->recent);
	if (status->flags & SA_UNSEEN)
		tris_log(LOG_NOTICE, ", %lu unseen", status->unseen);
	if (status->flags & SA_UIDVALIDITY)
		tris_log(LOG_NOTICE, ", %lu UID validity", status->uidvalidity);
	if (status->flags & SA_UIDNEXT)
		tris_log(LOG_NOTICE, ", %lu next UID", status->uidnext);
	tris_log(LOG_NOTICE, "\n");
}


void mm_log(char *string, long errflg)
{
	switch ((short) errflg) {
		case NIL:
			tris_debug(1,"IMAP Info: %s\n", string);
			break;
		case PARSE:
		case WARN:
			tris_log(LOG_WARNING, "IMAP Warning: %s\n", string);
			break;
		case ERROR:
			tris_log(LOG_ERROR, "IMAP Error: %s\n", string);
			break;
	}
}


void mm_dlog(char *string)
{
	tris_log(LOG_NOTICE, "%s\n", string);
}


void mm_login(NETMBX * mb, char *user, char *pwd, long trial)
{
	struct tris_vm_user *vmu;

	tris_debug(4, "Entering callback mm_login\n");

	tris_copy_string(user, mb->user, MAILTMPLEN);

	/* We should only do this when necessary */
	if (!tris_strlen_zero(authpassword)) {
		tris_copy_string(pwd, authpassword, MAILTMPLEN);
	} else {
		TRIS_LIST_TRAVERSE(&users, vmu, list) {
			if (!strcasecmp(mb->user, vmu->imapuser)) {
				tris_copy_string(pwd, vmu->imappassword, MAILTMPLEN);
				break;
			}
		}
		if (!vmu) {
			if ((vmu = find_user_realtime_imapuser(mb->user))) {
				tris_copy_string(pwd, vmu->imappassword, MAILTMPLEN);
				free_user(vmu);
			}
		}
	}
}


void mm_critical(MAILSTREAM * stream)
{
}


void mm_nocritical(MAILSTREAM * stream)
{
}


long mm_diskerror(MAILSTREAM * stream, long errcode, long serious)
{
	kill (getpid (), SIGSTOP);
	return NIL;
}


void mm_fatal(char *string)
{
	tris_log(LOG_ERROR, "IMAP access FATAL error: %s\n", string);
}

/* C-client callback to handle quota */
static void mm_parsequota(MAILSTREAM *stream, unsigned char *msg, QUOTALIST *pquota)
{
	struct vm_state *vms;
	char *mailbox = stream->mailbox, *user;
	char buf[1024] = "";
	unsigned long usage = 0, limit = 0;
	
	while (pquota) {
		usage = pquota->usage;
		limit = pquota->limit;
		pquota = pquota->next;
	}
	
	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))) || !(vms = get_vm_state_by_imapuser(user, 2))) {
		tris_log(LOG_ERROR, "No state found.\n");
		return;
	}

	tris_debug(3, "User %s usage is %lu, limit is %lu\n", user, usage, limit);

	vms->quota_usage = usage;
	vms->quota_limit = limit;
}

static char *get_header_by_tag(char *header, char *tag, char *buf, size_t len)
{
	char *start, *eol_pnt;
	int taglen;

	if (tris_strlen_zero(header) || tris_strlen_zero(tag))
		return NULL;

	taglen = strlen(tag) + 1;
	if (taglen < 1)
		return NULL;

	if (!(start = strstr(header, tag)))
		return NULL;

	/* Since we can be called multiple times we should clear our buffer */
	memset(buf, 0, len);

	tris_copy_string(buf, start+taglen, len);
	if ((eol_pnt = strchr(buf,'\r')) || (eol_pnt = strchr(buf,'\n')))
		*eol_pnt = '\0';
	return buf;
}

static char *get_user_by_mailbox(char *mailbox, char *buf, size_t len)
{
	char *start, *quote, *eol_pnt;

	if (tris_strlen_zero(mailbox))
		return NULL;

	if (!(start = strstr(mailbox, "/user=")))
		return NULL;

	tris_copy_string(buf, start+6, len);

	if (!(quote = strchr(buf, '\"'))) {
		if (!(eol_pnt = strchr(buf, '/')))
			eol_pnt = strchr(buf,'}');
		*eol_pnt = '\0';
		return buf;
	} else {
		eol_pnt = strchr(buf+1,'\"');
		*eol_pnt = '\0';
		return buf+1;
	}
}

static struct vm_state *create_vm_state_from_user(struct tris_vm_user *vmu)
{
	struct vm_state *vms_p;

	if (option_debug > 4)
		tris_log(LOG_DEBUG,"Adding new vmstate for %s\n",vmu->imapuser);
	if (!(vms_p = tris_calloc(1, sizeof(*vms_p))))
		return NULL;
	tris_copy_string(vms_p->imapuser, vmu->imapuser, sizeof(vms_p->imapuser));
	tris_copy_string(vms_p->username, vmu->mailbox, sizeof(vms_p->username)); /* save for access from interactive entry point */
	vms_p->mailstream = NIL; /* save for access from interactive entry point */
	if (option_debug > 4)
		tris_log(LOG_DEBUG,"Copied %s to %s\n",vmu->imapuser,vms_p->imapuser);
	vms_p->updated = 1;
	/* set mailbox to INBOX! */
	tris_copy_string(vms_p->curbox, mbox(0), sizeof(vms_p->curbox));
	init_vm_state(vms_p);
	vmstate_insert(vms_p);
	return vms_p;
}

static struct vm_state *get_vm_state_by_imapuser(char *user, int interactive)
{
	struct vmstate *vlist = NULL;

	TRIS_LIST_LOCK(&vmstates);
	TRIS_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			tris_debug(3, "error: vms is NULL for %s\n", user);
			continue;
		}
		if (!vlist->vms->imapuser) {
			tris_debug(3, "error: imapuser is NULL for %s\n", user);
			continue;
		}

		if (!strcmp(vlist->vms->imapuser, user) && (interactive == 2 || vlist->vms->interactive == interactive)) {
			TRIS_LIST_UNLOCK(&vmstates);
			return vlist->vms;
		}
	}
	TRIS_LIST_UNLOCK(&vmstates);

	tris_debug(3, "%s not found in vmstates\n", user);

	return NULL;
}

static struct vm_state *get_vm_state_by_mailbox(const char *mailbox, int interactive)
{

	struct vmstate *vlist = NULL;

	TRIS_LIST_LOCK(&vmstates);
	TRIS_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			tris_debug(3, "error: vms is NULL for %s\n", mailbox);
			continue;
		}
		if (!vlist->vms->username) {
			tris_debug(3, "error: username is NULL for %s\n", mailbox);
			continue;
		}

		tris_debug(3, "comparing mailbox %s (i=%d) to vmstate mailbox %s (i=%d)\n", mailbox, interactive, vlist->vms->username, vlist->vms->interactive);
		
		if (!strcmp(vlist->vms->username,mailbox) && vlist->vms->interactive == interactive) {
			tris_debug(3, "Found it!\n");
			TRIS_LIST_UNLOCK(&vmstates);
			return vlist->vms;
		}
	}
	TRIS_LIST_UNLOCK(&vmstates);

	tris_debug(3, "%s not found in vmstates\n", mailbox);

	return NULL;
}

static void vmstate_insert(struct vm_state *vms) 
{
	struct vmstate *v;
	struct vm_state *altvms;

	/* If interactive, it probably already exists, and we should
	use the one we already have since it is more up to date.
	We can compare the username to find the duplicate */
	if (vms->interactive == 1) {
		altvms = get_vm_state_by_mailbox(vms->username,0);
		if (altvms) {	
			tris_debug(3, "Duplicate mailbox %s, copying message info...\n",vms->username);
			vms->newmessages = altvms->newmessages;
			vms->oldmessages = altvms->oldmessages;
			vms->vmArrayIndex = altvms->vmArrayIndex;
			vms->lastmsg = altvms->lastmsg;
			vms->curmsg = altvms->curmsg;
			/* get a pointer to the persistent store */
			vms->persist_vms = altvms;
			/* Reuse the mailstream? */
			vms->mailstream = altvms->mailstream;
			/* vms->mailstream = NIL; */
		}
	}

	if (!(v = tris_calloc(1, sizeof(*v))))
		return;
	
	v->vms = vms;

	tris_debug(3, "Inserting vm_state for user:%s, mailbox %s\n",vms->imapuser,vms->username);

	TRIS_LIST_LOCK(&vmstates);
	TRIS_LIST_INSERT_TAIL(&vmstates, v, list);
	TRIS_LIST_UNLOCK(&vmstates);
}

static void vmstate_delete(struct vm_state *vms) 
{
	struct vmstate *vc = NULL;
	struct vm_state *altvms = NULL;

	/* If interactive, we should copy pertinent info
	back to the persistent state (to make update immediate) */
	if (vms->interactive == 1 && (altvms = vms->persist_vms)) {
		tris_debug(3, "Duplicate mailbox %s, copying message info...\n", vms->username);
		altvms->newmessages = vms->newmessages;
		altvms->oldmessages = vms->oldmessages;
		altvms->updated = 1;
	}
	
	tris_debug(3, "Removing vm_state for user:%s, mailbox %s\n", vms->imapuser, vms->username);
	
	TRIS_LIST_LOCK(&vmstates);
	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&vmstates, vc, list) {
		if (vc->vms == vms) {
			TRIS_LIST_REMOVE_CURRENT(list);
			break;
		}
	}
	TRIS_LIST_TRAVERSE_SAFE_END
	TRIS_LIST_UNLOCK(&vmstates);
	
	if (vc) {
		tris_mutex_destroy(&vc->vms->lock);
		tris_free(vc);
	}
	else
		tris_log(LOG_ERROR, "No vmstate found for user:%s, mailbox %s\n", vms->imapuser, vms->username);
}

static void set_update(MAILSTREAM * stream) 
{
	struct vm_state *vms;
	char *mailbox = stream->mailbox, *user;
	char buf[1024] = "";

	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))) || !(vms = get_vm_state_by_imapuser(user, 0))) {
		if (user && option_debug > 2)
			tris_log(LOG_WARNING, "User %s mailbox not found for update.\n", user);
		return;
	}

	tris_debug(3, "User %s mailbox set for update.\n", user);

	vms->updated = 1; /* Set updated flag since mailbox changed */
}

static void init_vm_state(struct vm_state *vms) 
{
	int x;
	vms->vmArrayIndex = 0;
	for (x = 0; x < 256; x++) {
		vms->msgArray[x] = 0;
	}
	tris_mutex_init(&vms->lock);
}

static int save_body(BODY *body, struct vm_state *vms, char *section, char *format) 
{
	char *body_content;
	char *body_decoded;
	unsigned long len;
	unsigned long newlen;
	char filename[256];
	
	if (!body || body == NIL)
		return -1;
	body_content = mail_fetchbody (vms->mailstream, vms->msgArray[vms->curmsg], section, &len);
	if (body_content != NIL) {
		snprintf(filename, sizeof(filename), "%s.%s", vms->fn, format);
		/* tris_log (LOG_DEBUG,body_content); */
		body_decoded = rfc822_base64 ((unsigned char *)body_content, len, &newlen);
		write_file (filename, (char *) body_decoded, newlen);
	}
	return 0;
}
/*! 
* \brief Get delimiter via mm_list callback 
* \param stream
*
* Determines the delimiter character that is used by the underlying IMAP based mail store.
*/
static void get_mailbox_delimiter(MAILSTREAM *stream) {
	char tmp[50];
	snprintf(tmp, sizeof(tmp), "{%s}", imapserver);
	mail_list(stream, tmp, "*");
}

/*! 
* \brief Check Quota for user 
* \param vms a pointer to a vm_state struct, will use the mailstream property of this.
* \param mailbox the mailbox to check the quota for.
*
* Calls imap_getquotaroot, which will populate its results into the vm_state vms input structure.
*/
static void check_quota(struct vm_state *vms, char *mailbox) {
	mail_parameters(NULL, SET_QUOTA, (void *) mm_parsequota);
	tris_debug(3, "Mailbox name set to: %s, about to check quotas\n", mailbox);
	if (vms && vms->mailstream != NULL) {
		imap_getquotaroot(vms->mailstream, mailbox);
	} else {
		tris_log(LOG_WARNING, "Mailstream not available for mailbox: %s\n", mailbox);
	}
}

#endif /* IMAP_STORAGE */

	/*! \brief Lock file path
		only return failure if tris_lock_path returns 'timeout',
	not if the path does not exist or any other reason
	*/
	static int vm_lock_path(const char *path)
	{
		switch (tris_lock_path(path)) {
		case TRIS_LOCK_TIMEOUT:
			return -1;
		default:
			return 0;
		}
	}


#ifdef ODBC_STORAGE

static int retrieve_file(char *dir, int msgnum)
{
	int x = 0;
	int res;
	int fd = -1;
	size_t fdlen = 0;
	void *fdm = MAP_FAILED;
	SQLSMALLINT colcount = 0;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char fmt[80] = "";
	char *c;
	char coltitle[256];
	SQLSMALLINT collen;
	SQLSMALLINT datatype;
	SQLSMALLINT decimaldigits;
	SQLSMALLINT nullable;
	SQLULEN colsize;
	SQLLEN colsize2;
	FILE *f = NULL;
	char rowdata[80];
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char msgnums[80];
	char *argv[] = { dir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 2, .argv = argv };

	struct odbc_obj *obj;
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		tris_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			tris_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		
		if (!(f = fopen(full_fn, "w+"))) {
			tris_log(LOG_WARNING, "Failed to open/create '%s'\n", full_fn);
			goto yuck;
		}
		
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE dir=? AND msgnum=?", odbc_table);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if (res == SQL_NO_DATA) {
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		} else if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		fd = open(full_fn, O_RDWR | O_CREAT | O_TRUNC, VOICEMAIL_FILE_MODE);
		if (fd < 0) {
			tris_log(LOG_WARNING, "Failed to write '%s': %s\n", full_fn, strerror(errno));
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLNumResultCols(stmt, &colcount);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {	
			tris_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		if (f) 
			fprintf(f, "[message]\n");
		for (x = 0; x < colcount; x++) {
			rowdata[0] = '\0';
			collen = sizeof(coltitle);
			res = SQLDescribeCol(stmt, x + 1, (unsigned char *)coltitle, sizeof(coltitle), &collen, 
						&datatype, &colsize, &decimaldigits, &nullable);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				tris_log(LOG_WARNING, "SQL Describe Column error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				tris_odbc_release_obj(obj);
				goto yuck;
			}
			if (!strcasecmp(coltitle, "recording")) {
				off_t offset;
				res = SQLGetData(stmt, x + 1, SQL_BINARY, rowdata, 0, &colsize2);
				fdlen = colsize2;
				if (fd > -1) {
					char tmp[1] = "";
					lseek(fd, fdlen - 1, SEEK_SET);
					if (write(fd, tmp, 1) != 1) {
						close(fd);
						fd = -1;
						continue;
					}
					/* Read out in small chunks */
					for (offset = 0; offset < colsize2; offset += CHUNKSIZE) {
						if ((fdm = mmap(NULL, CHUNKSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset)) == MAP_FAILED) {
							tris_log(LOG_WARNING, "Could not mmap the output file: %s (%d)\n", strerror(errno), errno);
							SQLFreeHandle(SQL_HANDLE_STMT, stmt);
							tris_odbc_release_obj(obj);
							goto yuck;
						} else {
							res = SQLGetData(stmt, x + 1, SQL_BINARY, fdm, CHUNKSIZE, NULL);
							munmap(fdm, CHUNKSIZE);
							if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
								tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
								unlink(full_fn);
								SQLFreeHandle(SQL_HANDLE_STMT, stmt);
								tris_odbc_release_obj(obj);
								goto yuck;
							}
						}
					}
					if (truncate(full_fn, fdlen) < 0) {
						tris_log(LOG_WARNING, "Unable to truncate '%s': %s\n", full_fn, strerror(errno));
					}
				}
			} else {
				SQLLEN ind;
				res = SQLGetData(stmt, x + 1, SQL_CHAR, rowdata, sizeof(rowdata), &ind);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					SQLINTEGER nativeerror = 0;
					SQLSMALLINT diagbytes = 0;
					unsigned char state[10], diagnostic[256];
					SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &nativeerror, diagnostic, sizeof(diagnostic), &diagbytes);
					tris_log(LOG_WARNING, "SQL Get Data error: %s: %s!\n[%s]\n\n", state, diagnostic, sql);
					SQLFreeHandle (SQL_HANDLE_STMT, stmt);
					tris_odbc_release_obj(obj);
					goto yuck;
				}
				if (strcasecmp(coltitle, "msgnum") && strcasecmp(coltitle, "dir") && f)
					fprintf(f, "%s=%s\n", coltitle, rowdata);
			}
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (f)
		fclose(f);
	if (fd > -1)
		close(fd);
	return x - 1;
}

/*!
* \brief Determines the highest message number in use for a given user and mailbox folder.
* \param vmu 
* \param dir the folder the mailbox folder to look for messages. Used to construct the SQL where clause.
*
* This method is used when mailboxes are stored in an ODBC back end.
* Typical use to set the msgnum would be to take the value returned from this method and add one to it.
*
* \return the value of zero or greaterto indicate the last message index in use, -1 to indicate none.
*/
static int last_message_index(struct tris_vm_user *vmu, char *dir)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char *argv[] = { dir };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 1, .argv = argv };

	struct odbc_obj *obj;
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=?", odbc_table);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			tris_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x - 1;
}

static int message_exists(char *dir, int msgnum)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char msgnums[20];
	char *argv[] = { dir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 2, .argv = argv };

	struct odbc_obj *obj;
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=? AND msgnum=?", odbc_table);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			tris_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x;
}

static int count_messages(struct tris_vm_user *vmu, char *dir)
{
	return last_message_index(vmu, dir) + 1;
}

static void delete_file(char *sdir, int smsg)
{
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	char *argv[] = { sdir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 2, .argv = argv };

	struct odbc_obj *obj;
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE dir=? AND msgnum=?", odbc_table);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt)
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
		else
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	return;	
}

static void copy_file(char *sdir, int smsg, char *ddir, int dmsg, char *dmailboxuser, char *dmailboxcontext)
{
	SQLHSTMT stmt;
	char sql[512];
	char msgnums[20];
	char msgnumd[20];
	struct odbc_obj *obj;
	char *argv[] = { ddir, msgnumd, dmailboxuser, dmailboxcontext, sdir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 6, .argv = argv };

	delete_file(ddir, dmsg);
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		snprintf(sql, sizeof(sql), "INSERT INTO %s (dir, msgnum, context, macrocontext, callerid, origtime, duration, recording, mailboxuser, mailboxcontext) SELECT ?,?,context,macrocontext,callerid,origtime,duration,recording,?,? FROM %s WHERE dir=? AND msgnum=?", odbc_table, odbc_table);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt)
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s] (You probably don't have MySQL 4.1 or later installed)\n\n", sql);
		else
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	return;	
}

struct insert_cb_struct {
	char *dir;
	char *msgnum;
	void *recording;
	size_t recordinglen;
	SQLLEN indlen;
	const char *context;
	const char *macrocontext;
	const char *callerid;
	const char *origtime;
	const char *duration;
	char *mailboxuser;
	char *mailboxcontext;
	const char *category;
	char *sql;
};

static SQLHSTMT insert_cb(struct odbc_obj *obj, void *vd)
{
	struct insert_cb_struct *d = vd;
	int res;
	SQLHSTMT stmt;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
		return NULL;
	}

	res = SQLPrepare(stmt, (unsigned char *)d->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL Prepare failed![%s]\n", d->sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->dir), 0, (void *)d->dir, 0, NULL);
	SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->msgnum), 0, (void *)d->msgnum, 0, NULL);
	SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY, d->recordinglen, 0, (void *)d->recording, 0, &d->indlen);
	SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->context), 0, (void *)d->context, 0, NULL);
	SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->macrocontext), 0, (void *)d->macrocontext, 0, NULL);
	SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->callerid), 0, (void *)d->callerid, 0, NULL);
	SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->origtime), 0, (void *)d->origtime, 0, NULL);
	SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->duration), 0, (void *)d->duration, 0, NULL);
	SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->mailboxuser), 0, (void *)d->mailboxuser, 0, NULL);
	SQLBindParameter(stmt, 10, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->mailboxcontext), 0, (void *)d->mailboxcontext, 0, NULL);
	if (!tris_strlen_zero(d->category)) {
		SQLBindParameter(stmt, 11, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->category), 0, (void *)d->category, 0, NULL);
	}

	return stmt;
}

static int store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum)
{
	int x = 0;
	int fd = -1;
	void *fdm = MAP_FAILED;
	size_t fdlen = -1;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char fmt[80] = "";
	char *c;
	struct insert_cb_struct d = {
		.dir = dir,
		.msgnum = msgnums,
		.context = "",
		.macrocontext = "",
		.callerid = "",
		.origtime = "",
		.duration = "",
		.mailboxuser = mailboxuser,
		.mailboxcontext = mailboxcontext,
		.category = "",
		.sql = sql
	};
	struct tris_config *cfg = NULL;
	struct odbc_obj *obj;
	struct tris_flags config_flags = { CONFIG_FLAG_NOCACHE };

	delete_file(dir, msgnum);
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		tris_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			tris_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		cfg = tris_config_load(full_fn, config_flags);
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		fd = open(full_fn, O_RDWR);
		if (fd < 0) {
			tris_log(LOG_WARNING, "Open of sound file '%s' failed: %s\n", full_fn, strerror(errno));
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		if (cfg) {
			d.context = tris_variable_retrieve(cfg, "message", "context");
			if (!d.context) d.context = "";
			d.macrocontext = tris_variable_retrieve(cfg, "message", "macrocontext");
			if (!d.macrocontext) d.macrocontext = "";
			d.callerid = tris_variable_retrieve(cfg, "message", "callerid");
			if (!d.callerid) d.callerid = "";
			d.origtime = tris_variable_retrieve(cfg, "message", "origtime");
			if (!d.origtime) d.origtime = "";
			d.duration = tris_variable_retrieve(cfg, "message", "duration");
			if (!d.duration) d.duration = "";
			d.category = tris_variable_retrieve(cfg, "message", "category");
			if (!d.category) d.category = "";
		}
		fdlen = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		printf("Length is %zd\n", fdlen);
		fdm = mmap(NULL, fdlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (fdm == MAP_FAILED) {
			tris_log(LOG_WARNING, "Memory map failed!\n");
			tris_odbc_release_obj(obj);
			goto yuck;
		} 
		d.recording = fdm;
		d.recordinglen = d.indlen = fdlen; /* SQL_LEN_DATA_AT_EXEC(fdlen); */
		if (!tris_strlen_zero(d.category)) 
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext,category) VALUES (?,?,?,?,?,?,?,?,?,?,?)", odbc_table);
		else
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext) VALUES (?,?,?,?,?,?,?,?,?,?)", odbc_table);
		stmt = tris_odbc_prepare_and_execute(obj, insert_cb, &d);
		if (stmt) {
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		}
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (cfg)
		tris_config_destroy(cfg);
	if (fdm != MAP_FAILED)
		munmap(fdm, fdlen);
	if (fd > -1)
		close(fd);
	return x;
}

static void rename_file(char *sdir, int smsg, char *mailboxuser, char *mailboxcontext, char *ddir, int dmsg)
{
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	char msgnumd[20];
	struct odbc_obj *obj;
	char *argv[] = { ddir, msgnumd, mailboxuser, mailboxcontext, sdir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 6, .argv = argv };

	delete_file(ddir, dmsg);
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		snprintf(sql, sizeof(sql), "UPDATE %s SET dir=?, msgnum=?, mailboxuser=?, mailboxcontext=? WHERE dir=? AND msgnum=?", odbc_table);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt)
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
		else
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	return;	
}

/*!
 * \brief Removes a voicemail message file.
 * \param dir the path to the message file.
 * \param msgnum the unique number for the message within the mailbox.
 *
 * Removes the message content file and the information file.
 * This method is used by the DISPOSE macro when mailboxes are stored in an ODBC back end.
 * Typical use is to clean up after a RETRIEVE operation. 
 * Note that this does not remove the message from the mailbox folders, to do that we would use delete_file().
 * \return zero on success, -1 on error.
 */
static int remove_file(char *dir, int msgnum)
{
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char msgnums[80];
	
	if (msgnum > -1) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		make_file(fn, sizeof(fn), dir, msgnum);
	} else
		tris_copy_string(fn, dir, sizeof(fn));
	tris_filedelete(fn, NULL);	
	snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
	unlink(full_fn);
	return 0;
}
#else
#ifndef IMAP_STORAGE
static int count_messages(struct tris_vm_user *vmu, char *dir)
{
	/* Find all .txt files - even if they are not in sequence from 0000 */

	int vmcount = 0;
	DIR *vmdir = NULL;
	struct dirent *vment = NULL;

	if (vm_lock_path(dir))
		return ERROR_LOCK_PATH;

	if ((vmdir = opendir(dir))) {
		while ((vment = readdir(vmdir))) {
			if (strlen(vment->d_name) > 7 && !strncmp(vment->d_name + 7, ".txt", 4)) 
				vmcount++;
		}
		closedir(vmdir);
	}
	tris_unlock_path(dir);
	
	return vmcount;
}

static void rename_file(char *sfn, char *dfn)
{
	char stxt[PATH_MAX];
	char dtxt[PATH_MAX];
	tris_filerename(sfn, dfn, NULL);
	snprintf(stxt, sizeof(stxt), "%s.txt", sfn);
	snprintf(dtxt, sizeof(dtxt), "%s.txt", dfn);
	if (tris_check_realtime("voicemail_data")) {
		tris_update_realtime("voicemail_data", "filename", sfn, "filename", dfn, NULL);
	}
	rename(stxt, dtxt);
}
#endif

#ifndef IMAP_STORAGE
/*! \brief
* A negative return value indicates an error.
* \note Should always be called with a lock already set on dir.
*/
static int last_message_index(struct tris_vm_user *vmu, char *dir)
{
	int x;
	unsigned char map[MAXMSGLIMIT] = "";
	DIR *msgdir;
	struct dirent *msgdirent;
	int msgdirint;

	/* Reading the entire directory into a file map scales better than
	* doing a stat repeatedly on a predicted sequence.  I suspect this
	* is partially due to stat(2) internally doing a readdir(2) itself to
	* find each file. */
	msgdir = opendir(dir);
	while ((msgdirent = readdir(msgdir))) {
		if (sscanf(msgdirent->d_name, "msg%d", &msgdirint) == 1 && msgdirint < MAXMSGLIMIT)
			map[msgdirint] = 1;
	}
	closedir(msgdir);

	for (x = 0; x < vmu->maxmsg; x++) {
		if (map[x] == 0)
			break;
	}

	return x - 1;
}

#endif /* #ifndef IMAP_STORAGE */
#endif /* #else of #ifdef ODBC_STORAGE */

static int copy(char *infile, char *outfile)
{
	int ifd;
	int ofd;
	int res;
	int len;
	char buf[4096];

#ifdef HARDLINK_WHEN_POSSIBLE
	/* Hard link if possible; saves disk space & is faster */
	if (link(infile, outfile)) {
#endif
		if ((ifd = open(infile, O_RDONLY)) < 0) {
			tris_log(LOG_WARNING, "Unable to open %s in read-only mode: %s\n", infile, strerror(errno));
			return -1;
		}
		if ((ofd = open(outfile, O_WRONLY | O_TRUNC | O_CREAT, VOICEMAIL_FILE_MODE)) < 0) {
			tris_log(LOG_WARNING, "Unable to open %s in write-only mode: %s\n", outfile, strerror(errno));
			close(ifd);
			return -1;
		}
		do {
			len = read(ifd, buf, sizeof(buf));
			if (len < 0) {
				tris_log(LOG_WARNING, "Read failed on %s: %s\n", infile, strerror(errno));
				close(ifd);
				close(ofd);
				unlink(outfile);
			}
			if (len) {
				res = write(ofd, buf, len);
				if (errno == ENOMEM || errno == ENOSPC || res != len) {
					tris_log(LOG_WARNING, "Write failed on %s (%d of %d): %s\n", outfile, res, len, strerror(errno));
					close(ifd);
					close(ofd);
					unlink(outfile);
				}
			}
		} while (len);
		close(ifd);
		close(ofd);
		return 0;
#ifdef HARDLINK_WHEN_POSSIBLE
	} else {
		/* Hard link succeeded */
		return 0;
	}
#endif
}

static void copy_plain_file(char *frompath, char *topath)
{
	char frompath2[PATH_MAX], topath2[PATH_MAX];
	struct tris_variable *tmp, *var = NULL;
	const char *origmailbox = NULL, *context = NULL, *macrocontext = NULL, *exten = NULL, *priority = NULL, *callerchan = NULL, *callerid = NULL, *origdate = NULL, *origtime = NULL, *category = NULL, *duration = NULL;
	tris_filecopy(frompath, topath, NULL);
	snprintf(frompath2, sizeof(frompath2), "%s.txt", frompath);
	snprintf(topath2, sizeof(topath2), "%s.txt", topath);
	if (tris_check_realtime("voicemail_data")) {
		var = tris_load_realtime("voicemail_data", "filename", frompath, NULL);
		/* This cycle converts tris_variable linked list, to va_list list of arguments, may be there is a better way to do it? */
		for (tmp = var; tmp; tmp = tmp->next) {
			if (!strcasecmp(tmp->name, "origmailbox")) {
				origmailbox = tmp->value;
			} else if (!strcasecmp(tmp->name, "context")) {
				context = tmp->value;
			} else if (!strcasecmp(tmp->name, "macrocontext")) {
				macrocontext = tmp->value;
			} else if (!strcasecmp(tmp->name, "exten")) {
				exten = tmp->value;
			} else if (!strcasecmp(tmp->name, "priority")) {
				priority = tmp->value;
			} else if (!strcasecmp(tmp->name, "callerchan")) {
				callerchan = tmp->value;
			} else if (!strcasecmp(tmp->name, "callerid")) {
				callerid = tmp->value;
			} else if (!strcasecmp(tmp->name, "origdate")) {
				origdate = tmp->value;
			} else if (!strcasecmp(tmp->name, "origtime")) {
				origtime = tmp->value;
			} else if (!strcasecmp(tmp->name, "category")) {
				category = tmp->value;
			} else if (!strcasecmp(tmp->name, "duration")) {
				duration = tmp->value;
			}
		}
		tris_store_realtime("voicemail_data", "filename", topath, "origmailbox", origmailbox, "context", context, "macrocontext", macrocontext, "exten", exten, "priority", priority, "callerchan", callerchan, "callerid", callerid, "origdate", origdate, "origtime", origtime, "category", category, "duration", duration, NULL);
	}
	copy(frompath2, topath2);
	tris_variables_destroy(var);
}


#if (!defined(ODBC_STORAGE) && !defined(IMAP_STORAGE))
/*! 
* \brief Removes the voicemail sound and information file.
* \param file The path to the sound file. This will be the the folder and message index, without the extension.
*
* This is used by the DELETE macro when voicemails are stored on the file system.
*
* \return zero on success, -1 on error.
*/
static int vm_delete(char *file)
{
	char *txt;
	int txtsize = 0;

	txtsize = (strlen(file) + 5) * sizeof(char);
	txt = alloca(txtsize);
	/* Sprintf here would safe because we alloca'd exactly the right length,
	* but trying to eliminate all sprintf's anyhow
	*/
	if (tris_check_realtime("voicemail_data")) {
		tris_destroy_realtime("voicemail_data", "filename", file, NULL);
	}
	snprintf(txt, txtsize, "%s.txt", file);
	unlink(txt);
	return tris_filedelete(file, NULL);
}
#endif
static int inbuf(struct baseio *bio, FILE *fi)
{
	int l;

	if (bio->ateof)
		return 0;

	if ((l = fread(bio->iobuf, 1, BASEMAXINLINE, fi)) <= 0) {
		if (ferror(fi))
			return -1;

		bio->ateof = 1;
		return 0;
	}

	bio->iolen = l;
	bio->iocp = 0;

	return 1;
}

static int inchar(struct baseio *bio, FILE *fi)
{
	if (bio->iocp >= bio->iolen) {
		if (!inbuf(bio, fi))
			return EOF;
	}

	return bio->iobuf[bio->iocp++];
}

static int ochar(struct baseio *bio, int c, FILE *so)
{
	if (bio->linelength >= BASELINELEN) {
		if (fputs(eol, so) == EOF)
			return -1;

		bio->linelength= 0;
	}

	if (putc(((unsigned char)c), so) == EOF)
		return -1;

	bio->linelength++;

	return 1;
}

static int base_encode(char *filename, FILE *so)
{
	static const unsigned char dtable[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
		'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
		'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0',
		'1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};
	int i, hiteof = 0;
	FILE *fi;
	struct baseio bio;

	memset(&bio, 0, sizeof(bio));
	bio.iocp = BASEMAXINLINE;

	if (!(fi = fopen(filename, "rb"))) {
		tris_log(LOG_WARNING, "Failed to open file: %s: %s\n", filename, strerror(errno));
		return -1;
	}

	while (!hiteof) {
		unsigned char igroup[3], ogroup[4];
		int c, n;

		igroup[0] = igroup[1] = igroup[2] = 0;

		for (n = 0; n < 3; n++) {
			if ((c = inchar(&bio, fi)) == EOF) {
				hiteof = 1;
				break;
			}

			igroup[n] = (unsigned char)c;
		}

		if (n > 0) {
			ogroup[0] = dtable[igroup[0] >> 2];
			ogroup[1] = dtable[((igroup[0] & 3) << 4) | (igroup[1] >> 4)];
			ogroup[2] = dtable[((igroup[1] & 0xF) << 2) | (igroup[2] >> 6)];
			ogroup[3] = dtable[igroup[2] & 0x3F];

			if (n < 3) {
				ogroup[3] = '=';

				if (n < 2)
					ogroup[2] = '=';
			}

			for (i = 0; i < 4; i++)
				ochar(&bio, ogroup[i], so);
		}
	}

	fclose(fi);
	
	if (fputs(eol, so) == EOF)
		return 0;

	return 1;
}

static void prep_email_sub_vars(struct tris_channel *ast, struct tris_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *dur, char *date, char *passdata, size_t passdatasize, const char *category)
{
	char callerid[256];
	/* Prepare variables for substitution in email body and subject */
	pbx_builtin_setvar_helper(ast, "VM_NAME", vmu->fullname);
	pbx_builtin_setvar_helper(ast, "VM_DUR", dur);
	snprintf(passdata, passdatasize, "%d", msgnum);
	pbx_builtin_setvar_helper(ast, "VM_MSGNUM", passdata);
	pbx_builtin_setvar_helper(ast, "VM_CONTEXT", context);
	pbx_builtin_setvar_helper(ast, "VM_MAILBOX", mailbox);
	pbx_builtin_setvar_helper(ast, "VM_CALLERID", (!tris_strlen_zero(cidname) || !tris_strlen_zero(cidnum)) ?
		tris_callerid_merge(callerid, sizeof(callerid), cidname, cidnum, NULL) : "an unknown caller");
	pbx_builtin_setvar_helper(ast, "VM_CIDNAME", (!tris_strlen_zero(cidname) ? cidname : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_CIDNUM", (!tris_strlen_zero(cidnum) ? cidnum : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_DATE", date);
	pbx_builtin_setvar_helper(ast, "VM_CATEGORY", category ? tris_strdupa(category) : "no category");
}

static char *quote(const char *from, char *to, size_t len)
{
	char *ptr = to;
	*ptr++ = '"';
	for (; ptr < to + len - 1; from++) {
		if (*from == '"')
			*ptr++ = '\\';
		else if (*from == '\0')
			break;
		*ptr++ = *from;
	}
	if (ptr < to + len - 1)
		*ptr++ = '"';
	*ptr = '\0';
	return to;
}

/*! \brief
* fill in *tm for current time according to the proper timezone, if any.
* Return tm so it can be used as a function argument.
*/
static const struct tris_tm *vmu_tm(const struct tris_vm_user *vmu, struct tris_tm *tm)
{
	const struct vm_zone *z = NULL;
	struct timeval t = tris_tvnow();

	/* Does this user have a timezone specified? */
	if (!tris_strlen_zero(vmu->zonetag)) {
		/* Find the zone in the list */
		TRIS_LIST_LOCK(&zones);
		TRIS_LIST_TRAVERSE(&zones, z, list) {
			if (!strcmp(z->name, vmu->zonetag))
				break;
		}
		TRIS_LIST_UNLOCK(&zones);
	}
	tris_localtime(&t, tm, z ? z->timezone : NULL);
	return tm;
}

/*!\brief Check if the string would need encoding within the MIME standard, to
 * avoid confusing certain mail software that expects messages to be 7-bit
 * clean.
 */
static int check_mime(const char *str)
{
	for (; *str; str++) {
		if (*str > 126 || *str < 32 || strchr("()<>@,:;/\"[]?.=", *str)) {
			return 1;
		}
	}
	return 0;
}

/*!\brief Encode a string according to the MIME rules for encoding strings
 * that are not 7-bit clean or contain control characters.
 *
 * Additionally, if the encoded string would exceed the MIME limit of 76
 * characters per line, then the encoding will be broken up into multiple
 * sections, separated by a space character, in order to facilitate
 * breaking up the associated header across multiple lines.
 *
 * \param start A string to be encoded
 * \param end An expandable buffer for holding the result
 * \param preamble The length of the first line already used for this string,
 * to ensure that each line maintains a maximum length of 76 chars.
 * \param postamble the length of any additional characters appended to the
 * line, used to ensure proper field wrapping.
 * \retval The encoded string.
 */
static char *encode_mime_str(const char *start, char *end, size_t endsize, size_t preamble, size_t postamble)
{
	char tmp[80];
	int first_section = 1;
	size_t endlen = 0, tmplen = 0;
	*end = '\0';

	tmplen = snprintf(tmp, sizeof(tmp), "=?%s?Q?", charset);
	for (; *start; start++) {
		int need_encoding = 0;
		if (*start < 33 || *start > 126 || strchr("()<>@,:;/\"[]?.=_", *start)) {
			need_encoding = 1;
		}
		if ((first_section && need_encoding && preamble + tmplen > 70) ||
			(first_section && !need_encoding && preamble + tmplen > 72) ||
			(!first_section && need_encoding && tmplen > 70) ||
			(!first_section && !need_encoding && tmplen > 72)) {
			/* Start new line */
			endlen += snprintf(end + endlen, endsize - endlen, "%s%s?=", first_section ? "" : " ", tmp);
			tmplen = snprintf(tmp, sizeof(tmp), "=?%s?Q?", charset);
			first_section = 0;
		}
		if (need_encoding && *start == ' ') {
			tmplen += snprintf(tmp + tmplen, sizeof(tmp) - tmplen, "_");
		} else if (need_encoding) {
			tmplen += snprintf(tmp + tmplen, sizeof(tmp) - tmplen, "=%hhX", *start);
		} else {
			tmplen += snprintf(tmp + tmplen, sizeof(tmp) - tmplen, "%c", *start);
		}
	}
	snprintf(end + endlen, endsize - endlen, "%s%s?=%s", first_section ? "" : " ", tmp, endlen + postamble > 74 ? " " : "");
	return end;
}

/*!
 * \brief Creates the email file to be sent to indicate a new voicemail exists for a user.
 * \param p The output file to generate the email contents into.
 * \param srcemail The email address to send the email to, presumably the email address for the owner of the mailbox.
 * \param vmu The voicemail user who is sending the voicemail.
 * \param msgnum The message index in the mailbox folder.
 * \param context 
 * \param mailbox The voicemail box to read the voicemail to be notified in this email.
 * \param cidnum The caller ID number.
 * \param cidname The caller ID name.
 * \param attach the name of the sound file to be attached to the email, if attach_user_voicemail == 1.
 * \param format The message sound file format. i.e. .wav
 * \param duration The time of the message content, in seconds.
 * \param attach_user_voicemail if 1, the sound file is attached to the email.
 * \param chan
 * \param category
 * \param imap if == 1, indicates the target folder for the email notification to be sent to will be an IMAP mailstore. This causes additional mailbox headers to be set, which would facilitate searching for the email in the destination IMAP folder.
 *
 * The email body, and base 64 encoded attachement (if any) are stored to the file identified by *p. This method does not actually send the email.  That is done by invoking the configure 'mailcmd' and piping this generated file into it, or with the sendemail() function.
 */
static void make_email_file(FILE *p, char *srcemail, struct tris_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct tris_channel *chan, const char *category, int imap)
{
	char date[256];
	char host[MAXHOSTNAMELEN] = "";
	char who[256];
	char bound[256];
	char fname[256];
	char dur[256];
	char tmpcmd[256];
	struct tris_tm tm;
	char enc_cidnum[256] = "", enc_cidname[256] = "";
	char *passdata = NULL, *passdata2;
	size_t len_passdata = 0, len_passdata2, tmplen;
	char *greeting_attachment;

#ifdef IMAP_STORAGE
#define ENDL "\r\n"
#else
#define ENDL "\n"
#endif

	/* One alloca for multiple fields */
	len_passdata2 = strlen(vmu->fullname);
	if (emailsubject && (tmplen = strlen(emailsubject)) > len_passdata2) {
		len_passdata2 = tmplen;
	}
	if ((tmplen = strlen(fromstring)) > len_passdata2) {
		len_passdata2 = tmplen;
	}
	len_passdata2 = len_passdata2 * 3 + 200;
	passdata2 = alloca(len_passdata2);

	if (!tris_strlen_zero(cidnum)) {
		strip_control(cidnum, enc_cidnum, sizeof(enc_cidnum));
	}
	if (!tris_strlen_zero(cidname)) {
		strip_control(cidname, enc_cidname, sizeof(enc_cidname));
	}
	gethostname(host, sizeof(host) - 1);

	if (strchr(srcemail, '@'))
		tris_copy_string(who, srcemail, sizeof(who));
	else 
		snprintf(who, sizeof(who), "%s@%s", srcemail, host);
	
	greeting_attachment = strrchr(tris_strdupa(attach), '/');
	if (greeting_attachment)
		*greeting_attachment++ = '\0';

	snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
	tris_strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", vmu_tm(vmu, &tm));
	fprintf(p, "Date: %s" ENDL, date);

	/* Set date format for voicemail mail */
	tris_strftime(date, sizeof(date), emaildateformat, &tm);

	if (!tris_strlen_zero(fromstring)) {
		struct tris_channel *ast;
		if ((ast = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *ptr;
			memset(passdata2, 0, len_passdata2);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, enc_cidnum, enc_cidname, dur, date, passdata2, len_passdata2, category);
			pbx_substitute_variables_helper(ast, fromstring, passdata2, len_passdata2);
			len_passdata = strlen(passdata2) * 3 + 300;
			passdata = alloca(len_passdata);
			if (check_mime(passdata2)) {
				int first_line = 1;
				encode_mime_str(passdata2, passdata, len_passdata, strlen("From: "), strlen(who) + 3);
				while ((ptr = strchr(passdata, ' '))) {
					*ptr = '\0';
					fprintf(p, "%s %s" ENDL, first_line ? "From:" : "", passdata);
					first_line = 0;
					passdata = ptr + 1;
				}
				fprintf(p, "%s %s <%s>" ENDL, first_line ? "From:" : "", passdata, who);
			} else {
				fprintf(p, "From: %s <%s>" ENDL, quote(passdata2, passdata, len_passdata), who);
			}
			tris_channel_free(ast);
		} else {
			tris_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		}
	} else {
		fprintf(p, "From: Trismedia PBX <%s>" ENDL, who);
	}

	if (check_mime(vmu->fullname)) {
		int first_line = 1;
		char *ptr;
		encode_mime_str(vmu->fullname, passdata2, len_passdata2, strlen("To: "), strlen(vmu->email) + 3);
		while ((ptr = strchr(passdata2, ' '))) {
			*ptr = '\0';
			fprintf(p, "%s %s" ENDL, first_line ? "To:" : "", passdata2);
			first_line = 0;
			passdata2 = ptr + 1;
		}
		fprintf(p, "%s %s <%s>" ENDL, first_line ? "To:" : "", passdata2, vmu->email);
	} else {
		fprintf(p, "To: %s <%s>" ENDL, quote(vmu->fullname, passdata2, len_passdata2), vmu->email);
	}
	if (!tris_strlen_zero(emailsubject)) {
		struct tris_channel *ast;
		if ((ast = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			int vmlen = strlen(emailsubject) * 3 + 200;
			/* Only allocate more space if the previous was not large enough */
			if (vmlen > len_passdata) {
				passdata = alloca(vmlen);
				len_passdata = vmlen;
			}

			memset(passdata, 0, len_passdata);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, len_passdata, category);
			pbx_substitute_variables_helper(ast, emailsubject, passdata, len_passdata);
			if (check_mime(passdata)) {
				int first_line = 1;
				char *ptr;
				encode_mime_str(passdata, passdata2, len_passdata2, strlen("Subject: "), 0);
				while ((ptr = strchr(passdata2, ' '))) {
					*ptr = '\0';
					fprintf(p, "%s %s" ENDL, first_line ? "Subject:" : "", passdata2);
					first_line = 0;
					passdata2 = ptr + 1;
				}
				fprintf(p, "%s %s" ENDL, first_line ? "Subject:" : "", passdata2);
			} else {
				fprintf(p, "Subject: %s" ENDL, passdata);
			}
			tris_channel_free(ast);
		} else {
			tris_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		}
	} else if (tris_test_flag((&globalflags), VM_PBXSKIP)) {
		fprintf(p, "Subject: New message %d in mailbox %s" ENDL, msgnum + 1, mailbox);
	} else {
		fprintf(p, "Subject: [PBX]: New message %d in mailbox %s" ENDL, msgnum + 1, mailbox);
	}

	fprintf(p, "Message-ID: <Trismedia-%d-%d-%s-%d@%s>" ENDL, msgnum + 1, (unsigned int)tris_random(), mailbox, (int)getpid(), host);
	if (imap) {
		/* additional information needed for IMAP searching */
		fprintf(p, "X-Trismedia-VM-Message-Num: %d" ENDL, msgnum + 1);
		/* fprintf(p, "X-Trismedia-VM-Orig-Mailbox: %s" ENDL, ext); */
		fprintf(p, "X-Trismedia-VM-Server-Name: %s" ENDL, fromstring);
		fprintf(p, "X-Trismedia-VM-Context: %s" ENDL, context);
		fprintf(p, "X-Trismedia-VM-Extension: %s" ENDL, mailbox);
		fprintf(p, "X-Trismedia-VM-Priority: %d" ENDL, chan->priority);
		fprintf(p, "X-Trismedia-VM-Caller-channel: %s" ENDL, chan->name);
		fprintf(p, "X-Trismedia-VM-Caller-ID-Num: %s" ENDL, enc_cidnum);
		fprintf(p, "X-Trismedia-VM-Caller-ID-Name: %s" ENDL, enc_cidname);
		fprintf(p, "X-Trismedia-VM-Duration: %d" ENDL, duration);
		if (!tris_strlen_zero(category)) {
			fprintf(p, "X-Trismedia-VM-Category: %s" ENDL, category);
		} else {
			fprintf(p, "X-Trismedia-VM-Category: " ENDL);
		}
		fprintf(p, "X-Trismedia-VM-Message-Type: %s" ENDL, msgnum > -1 ? "Message" : greeting_attachment);
		fprintf(p, "X-Trismedia-VM-Orig-date: %s" ENDL, date);
		fprintf(p, "X-Trismedia-VM-Orig-time: %ld" ENDL, (long)time(NULL));
	}
	if (!tris_strlen_zero(cidnum)) {
		fprintf(p, "X-Trismedia-CallerID: %s" ENDL, enc_cidnum);
	}
	if (!tris_strlen_zero(cidname)) {
		fprintf(p, "X-Trismedia-CallerIDName: %s" ENDL, enc_cidname);
	}
	fprintf(p, "MIME-Version: 1.0" ENDL);
	if (attach_user_voicemail) {
		/* Something unique. */
		snprintf(bound, sizeof(bound), "----voicemail_%d%s%d%d", msgnum + 1, mailbox, (int)getpid(), (unsigned int)tris_random());

		fprintf(p, "Content-Type: multipart/mixed; boundary=\"%s\"" ENDL, bound);
		fprintf(p, ENDL ENDL "This is a multi-part message in MIME format." ENDL ENDL);
		fprintf(p, "--%s" ENDL, bound);
	}
	fprintf(p, "Content-Type: text/plain; charset=%s" ENDL "Content-Transfer-Encoding: 8bit" ENDL ENDL, charset);
	if (emailbody) {
		struct tris_channel *ast;
		if ((ast = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(emailbody) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, emailbody, passdata, vmlen);
			fprintf(p, "%s" ENDL, passdata);
			tris_channel_free(ast);
		} else
			tris_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else if (msgnum > -1) {
		fprintf(p, "Dear %s:" ENDL ENDL "\tJust wanted to let you know you were just left a %s long message (number %d)" ENDL

		"in mailbox %s from %s, on %s so you might" ENDL
		"want to check it when you get a chance.  Thanks!" ENDL ENDL "\t\t\t\t--Trismedia" ENDL ENDL, vmu->fullname, 
		dur, msgnum + 1, mailbox, (cidname ? cidname : (cidnum ? cidnum : "an unknown caller")), date);
	} else {
		fprintf(p, "This message is to let you know that your greeting was changed on %s." ENDL
				"Please do not delete this message, lest your greeting vanish with it." ENDL ENDL, date);
	}
	if (attach_user_voicemail) {
		/* Eww. We want formats to tell us their own MIME type */
		char *ctype = (!strcasecmp(format, "ogg")) ? "application/" : "audio/x-";
		char tmpdir[256], newtmp[256];
		int tmpfd = -1;
	
		if (vmu->volgain < -.001 || vmu->volgain > .001) {
			create_dirpath(tmpdir, sizeof(tmpdir), vmu->context, vmu->mailbox, "tmp");
			snprintf(newtmp, sizeof(newtmp), "%s/XXXXXX", tmpdir);
			tmpfd = mkstemp(newtmp);
			chmod(newtmp, VOICEMAIL_FILE_MODE & ~my_umask);
			tris_debug(3, "newtmp: %s\n", newtmp);
			if (tmpfd > -1) {
				int soxstatus;
				snprintf(tmpcmd, sizeof(tmpcmd), "sox -v %.4f %s.%s %s.%s", vmu->volgain, attach, format, newtmp, format);
				if ((soxstatus = tris_safe_system(tmpcmd)) == 0) {
					attach = newtmp;
					tris_debug(3, "VOLGAIN: Stored at: %s.%s - Level: %.4f - Mailbox: %s\n", attach, format, vmu->volgain, mailbox);
				} else {
					tris_log(LOG_WARNING, "Sox failed to reencode %s.%s: %s (have you installed support for all sox file formats?)\n", attach, format,
						soxstatus == 1 ? "Problem with command line options" : "An error occurred during file processing");
					tris_log(LOG_WARNING, "Voicemail attachment will have no volume gain.\n");
				}
			}
		}
		fprintf(p, "--%s" ENDL, bound);
		if (msgnum > -1)
			fprintf(p, "Content-Type: %s%s; name=\"msg%04d.%s\"" ENDL, ctype, format, msgnum + 1, format);
		else
			fprintf(p, "Content-Type: %s%s; name=\"%s.%s\"" ENDL, ctype, format, greeting_attachment, format);
		fprintf(p, "Content-Transfer-Encoding: base64" ENDL);
		fprintf(p, "Content-Description: Voicemail sound attachment." ENDL);
		if (msgnum > -1)
			fprintf(p, "Content-Disposition: attachment; filename=\"msg%04d.%s\"" ENDL ENDL, msgnum + 1, format);
		else
			fprintf(p, "Content-Disposition: attachment; filename=\"%s.%s\"" ENDL ENDL, greeting_attachment, format);
		snprintf(fname, sizeof(fname), "%s.%s", attach, format);
		base_encode(fname, p);
		fprintf(p, ENDL "--%s--" ENDL "." ENDL, bound);
		if (tmpfd > -1) {
			unlink(fname);
			close(tmpfd);
			unlink(newtmp);
		}
	}
#undef ENDL
}

static int sendmail(char *srcemail, struct tris_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct tris_channel *chan, const char *category)
{
	FILE *p = NULL;
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[256];

	if (vmu && tris_strlen_zero(vmu->email)) {
		tris_log(LOG_WARNING, "E-mail address missing for mailbox [%s].  E-mail will not be sent.\n", vmu->mailbox);
		return(0);
	}
	if (!strcmp(format, "wav49"))
		format = "WAV";
	tris_debug(3, "Attaching file '%s', format '%s', uservm is '%d', global is %d\n", attach, format, attach_user_voicemail, tris_test_flag((&globalflags), VM_ATTACH));
	/* Make a temporary file instead of piping directly to sendmail, in case the mail
	command hangs */
	if ((p = vm_mkftemp(tmp)) == NULL) {
		tris_log(LOG_WARNING, "Unable to launch '%s' (can't create temporary file)\n", mailcmd);
		return -1;
	} else {
		make_email_file(p, srcemail, vmu, msgnum, context, mailbox, cidnum, cidname, attach, format, duration, attach_user_voicemail, chan, category, 0);
		fclose(p);
		snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
		tris_safe_system(tmp2);
		tris_debug(1, "Sent mail to %s with command '%s'\n", vmu->email, mailcmd);
	}
	return 0;
}

static int sendpage(char *srcemail, char *pager, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, int duration, struct tris_vm_user *vmu, const char *category)
{
	char date[256];
	char host[MAXHOSTNAMELEN] = "";
	char who[256];
	char dur[PATH_MAX];
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[PATH_MAX];
	struct tris_tm tm;
	FILE *p;

	if ((p = vm_mkftemp(tmp)) == NULL) {
		tris_log(LOG_WARNING, "Unable to launch '%s' (can't create temporary file)\n", mailcmd);
		return -1;
	}
	gethostname(host, sizeof(host) - 1);
	if (strchr(srcemail, '@'))
		tris_copy_string(who, srcemail, sizeof(who));
	else 
		snprintf(who, sizeof(who), "%s@%s", srcemail, host);
	snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
	tris_strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", vmu_tm(vmu, &tm));
	fprintf(p, "Date: %s\n", date);

	if (*pagerfromstring) {
		struct tris_channel *ast;
		if ((ast = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(fromstring) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, pagerfromstring, passdata, vmlen);
			fprintf(p, "From: %s <%s>\n", passdata, who);
			tris_channel_free(ast);
		} else 
			tris_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else
		fprintf(p, "From: Trismedia PBX <%s>\n", who);
	fprintf(p, "To: %s\n", pager);
	if (pagersubject) {
		struct tris_channel *ast;
		if ((ast = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(pagersubject) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, pagersubject, passdata, vmlen);
			fprintf(p, "Subject: %s\n\n", passdata);
			tris_channel_free(ast);
		} else
			tris_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else
		fprintf(p, "Subject: New VM\n\n");

	tris_strftime(date, sizeof(date), "%A, %B %d, %Y at %r", &tm);
	if (pagerbody) {
		struct tris_channel *ast;
		if ((ast = tris_channel_alloc(0, TRIS_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(pagerbody) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, pagerbody, passdata, vmlen);
			fprintf(p, "%s\n", passdata);
			tris_channel_free(ast);
		} else
			tris_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else {
		fprintf(p, "New %s long msg in box %s\n"
				"from %s, on %s", dur, mailbox, (cidname ? cidname : (cidnum ? cidnum : "unknown")), date);
	}
	fclose(p);
	snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
	tris_safe_system(tmp2);
	tris_debug(1, "Sent page to %s with command '%s'\n", pager, mailcmd);
	return 0;
}

static int get_date(char *s, int len, char *fmt)
{
	struct tris_tm tm;
	struct timeval t = tris_tvnow();
	
	tris_localtime(&t, &tm, NULL);
	tris_strftime(s, len, fmt, &tm);
	return tm.tm_mday;
}

static int play_greeting(struct tris_channel *chan, struct tris_vm_user *vmu, char *filename, char *ecodes)
{
	int res = -2;
	
#ifdef ODBC_STORAGE
	int success = 
#endif
	RETRIEVE(filename, -1, vmu->mailbox, vmu->context);
	if (tris_fileexists(filename, NULL, NULL) > 0) {
		res = tris_streamfile(chan, filename, chan->language);
		if (res > -1) 
			res = tris_waitstream(chan, ecodes);
#ifdef ODBC_STORAGE
		if (success == -1) {
			/* We couldn't retrieve the file from the database, but we found it on the file system. Let's put it in the database. */
			tris_debug(1, "Greeting not retrieved from database, but found in file storage. Inserting into database\n");
			store_file(filename, vmu->mailbox, vmu->context, -1);
		}
#endif
	}
	DISPOSE(filename, -1);

	return res;
}

static void free_zone(struct vm_zone *z)
{
	tris_free(z);
}

#ifdef ODBC_STORAGE
/*! XXX \todo Fix this function to support multiple mailboxes in the intput string */
static int inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	int x = -1;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char tmp[PATH_MAX] = "";
	struct odbc_obj *obj;
	char *context;
	struct generic_prepare_struct gps = { .sql = sql, .argc = 0 };

	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;

	/* If no mailbox, return immediately */
	if (tris_strlen_zero(mailbox))
		return 0;

	tris_copy_string(tmp, mailbox, sizeof(tmp));
	
	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
	} else
		context = "default";
	
	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "INBOX");
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		*newmsgs = atoi(rowdata);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);

		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "OLD");
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		*oldmsgs = atoi(rowdata);
		x = 0;
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
		
yuck:	
	return x;
}

static int messagecount(const char *context, const char *mailbox, const char *folder)
{
	struct odbc_obj *obj = NULL;
	int nummsgs = 0;
	int res;
	SQLHSTMT stmt = NULL;
	char sql[PATH_MAX];
	char rowdata[20];
	struct generic_prepare_struct gps = { .sql = sql, .argc = 0 };
	if (!folder)
		folder = "INBOX";
	/* If no mailbox, return immediately */
	if (tris_strlen_zero(mailbox))
		return 0;

	obj = tris_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, mailbox, folder);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		nummsgs = atoi(rowdata);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);

yuck:
	if (obj)
		tris_odbc_release_obj(obj);
	return nummsgs;
}

static int inboxcount2(const char *mailbox, int *urgentmsgs, int *newmsgs, int *oldmsgs)
{
	int x = -1;
	int res;
	SQLHSTMT stmt = NULL;
	char sql[PATH_MAX];
	char rowdata[20];
	char tmp[PATH_MAX] = "";
	struct odbc_obj *obj = NULL;
	char *context;
	struct generic_prepare_struct gps = { .sql = sql, .argc = 0 };

	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;
	if (urgentmsgs)
		*urgentmsgs = 0;

	/* If no mailbox, return immediately */
	if (tris_strlen_zero(mailbox))
		return 0;

	tris_copy_string(tmp, mailbox, sizeof(tmp));

	if (strchr(mailbox, ' ') || strchr(mailbox, ',')) {
		int u, n, o;
		char *next, *remaining = tmp;
		while ((next = strsep(&remaining, " ,"))) {
			if (inboxcount2(next, urgentmsgs ? &u : NULL, &n, &o)) {
				return -1;
			}
			if (urgentmsgs) {
				*urgentmsgs += u;
			}
			if (newmsgs) {
				*newmsgs += n;
			}
			if (oldmsgs) {
				*oldmsgs += o;
			}
		}
		return 0;
	}

	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
	} else
		context = "default";

	if ((obj = tris_odbc_request_obj(odbc_database, 0))) {
		do {
			if (newmsgs) {
				snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "INBOX");
				if (!(stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps))) {
					tris_log(TRIS_LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
					break;
				}
				res = SQLFetch(stmt);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					tris_log(TRIS_LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
					break;
				}
				res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					tris_log(TRIS_LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
					break;
				}
				*newmsgs = atoi(rowdata);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			}

			if (oldmsgs) {
				snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "Old");
				if (!(stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps))) {
					tris_log(TRIS_LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
					break;
				}
				res = SQLFetch(stmt);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					tris_log(TRIS_LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
					break;
				}
				res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					tris_log(TRIS_LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
					break;
				}
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				*oldmsgs = atoi(rowdata);
			}

			if (urgentmsgs) {
				snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "Urgent");
				if (!(stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps))) {
					tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
					break;
				}
				res = SQLFetch(stmt);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
					break;
				}
				res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
					break;
				}
				*urgentmsgs = atoi(rowdata);
			}

			x = 0;
		} while (0);
	} else {
		tris_log(TRIS_LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	}

	if (stmt) {
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	}
	if (obj) {
		tris_odbc_release_obj(obj);
	}

	return x;
}

static int has_voicemail(const char *mailbox, const char *folder)
{
	char tmp[256], *tmp2 = tmp, *mbox, *context;
	tris_copy_string(tmp, mailbox, sizeof(tmp));
	while ((context = mbox = strsep(&tmp2, ","))) {
		strsep(&context, "@");
		if (tris_strlen_zero(context))
			context = "default";
		if (messagecount(context, mbox, folder))
			return 1;
	}
	return 0;
}

#elif defined(IMAP_STORAGE)

#endif
#ifndef IMAP_STORAGE
	/* copy message only used by file storage */
	static int copy_message(struct tris_channel *chan, struct tris_vm_user *vmu, int imbox, int msgnum, long duration, struct tris_vm_user *recip, char *fmt, char *dir)
	{
		char fromdir[PATH_MAX], todir[PATH_MAX], frompath[PATH_MAX], topath[PATH_MAX];
		const char *frombox = mbox(imbox);
		int recipmsgnum;

		tris_log(LOG_NOTICE, "Copying message from %s@%s to %s@%s\n", vmu->mailbox, vmu->context, recip->mailbox, recip->context);

		create_dirpath(todir, sizeof(todir), recip->context, recip->mailbox, "INBOX");
		
		if (!dir)
			make_dir(fromdir, sizeof(fromdir), vmu->context, vmu->mailbox, frombox);
		else
			tris_copy_string(fromdir, dir, sizeof(fromdir));

		make_file(frompath, sizeof(frompath), fromdir, msgnum);
		make_dir(todir, sizeof(todir), recip->context, recip->mailbox, "INBOX");

		if (vm_lock_path(todir))
			return ERROR_LOCK_PATH;

		recipmsgnum = last_message_index(recip, todir) + 1;
		if (recipmsgnum < recip->maxmsg) {
			make_file(topath, sizeof(topath), todir, recipmsgnum);
			COPY(fromdir, msgnum, todir, recipmsgnum, recip->mailbox, recip->context, frompath, topath);
		} else {
			tris_log(LOG_ERROR, "Recipient mailbox %s@%s is full\n", recip->mailbox, recip->context);
		}
		tris_unlock_path(todir);
		notify_new_message(chan, recip, NULL, recipmsgnum, duration, fmt, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL));
		
		return 0;
	}
#endif
#if !(defined(IMAP_STORAGE) || defined(ODBC_STORAGE))
	static int messagecount(const char *context, const char *mailbox, const char *folder)
	{
		return __has_voicemail(context, mailbox, folder, 0);
	}


	static int __has_voicemail(const char *context, const char *mailbox, const char *folder, int shortcircuit)
	{
		DIR *dir;
		struct dirent *de;
		char fn[256];
		int ret = 0;

		/* If no mailbox, return immediately */
		if (tris_strlen_zero(mailbox))
			return 0;

		if (tris_strlen_zero(folder))
			folder = "INBOX";
		if (tris_strlen_zero(context))
			context = "default";

		snprintf(fn, sizeof(fn), "%s%s/%s/%s", VM_SPOOL_DIR, context, mailbox, folder);

		if (!(dir = opendir(fn)))
			return 0;

		while ((de = readdir(dir))) {
			if (!strncasecmp(de->d_name, "msg", 3)) {
				if (shortcircuit) {
					ret = 1;
					break;
				} else if (!strncasecmp(de->d_name + 8, "txt", 3))
					ret++;
			}
		}

		closedir(dir);

		return ret;
	}


	static int inboxcount2(const char *mailbox, int *urgentmsgs, int *newmsgs, int *oldmsgs)
	{
		char tmp[256];
		char *context;
	
		/* If no mailbox, return immediately */
		if (tris_strlen_zero(mailbox))
			return 0;
	
		if (newmsgs)
			*newmsgs = 0;
		if (oldmsgs)
			*oldmsgs = 0;
		if (urgentmsgs)
			*urgentmsgs = 0;
	
		if (strchr(mailbox, ',')) {
			int tmpnew, tmpold, tmpurgent;
			char *mb, *cur;
	
			tris_copy_string(tmp, mailbox, sizeof(tmp));
			mb = tmp;
			while ((cur = strsep(&mb, ", "))) {
				if (!tris_strlen_zero(cur)) {
					if (inboxcount2(cur, urgentmsgs ? &tmpurgent : NULL, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
						return -1;
					else {
						if (newmsgs)
							*newmsgs += tmpnew; 
						if (oldmsgs)
							*oldmsgs += tmpold;
						if (urgentmsgs)
							*urgentmsgs += tmpurgent;
					}
				}
			}
			return 0;
		}
	
		tris_copy_string(tmp, mailbox, sizeof(tmp));
		
		if ((context = strchr(tmp, '@')))
			*context++ = '\0';
		else
			context = "default";
	
		if (newmsgs)
			*newmsgs = __has_voicemail(context, tmp, "INBOX", 0);
		if (oldmsgs)
			*oldmsgs = __has_voicemail(context, tmp, "Old", 0);
		if (urgentmsgs)
			*urgentmsgs = __has_voicemail(context, tmp, "Urgent", 0);
	
		return 0;
	}

	static int has_voicemail(const char *mailbox, const char *folder)
	{
		char tmp[256], *tmp2 = tmp, *mbox, *context;
		tris_copy_string(tmp, mailbox, sizeof(tmp));
		while ((mbox = strsep(&tmp2, ","))) {
			if ((context = strchr(mbox, '@')))
				*context++ = '\0';
			else
				context = "default";
			if (__has_voicemail(context, mbox, folder, 1))
				return 1;
		}
		return 0;
	}


	static int inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
	{
		char tmp[256];
		char *context;

		/* If no mailbox, return immediately */
		if (tris_strlen_zero(mailbox))
			return 0;

		if (newmsgs)
			*newmsgs = 0;
		if (oldmsgs)
			*oldmsgs = 0;

		if (strchr(mailbox, ',')) {
			int tmpnew, tmpold;
			char *mb, *cur;

			tris_copy_string(tmp, mailbox, sizeof(tmp));
			mb = tmp;
			while ((cur = strsep(&mb, ", "))) {
				if (!tris_strlen_zero(cur)) {
					if (inboxcount(cur, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
						return -1;
					else {
						if (newmsgs)
							*newmsgs += tmpnew; 
						if (oldmsgs)
							*oldmsgs += tmpold;
					}
				}
			}
			return 0;
		}

		tris_copy_string(tmp, mailbox, sizeof(tmp));
		
		if ((context = strchr(tmp, '@')))
			*context++ = '\0';
		else
			context = "default";

		if (newmsgs)
			*newmsgs = __has_voicemail(context, tmp, "INBOX", 0);
		if (oldmsgs)
			*oldmsgs = __has_voicemail(context, tmp, "OLD", 0);

		return 0;
	}

	
	static int __get_vmlist(const char* context, const char *mailbox, const char * folder, char *vmlist)
	{
		DIR *dir;
		struct dirent *de;
		char fn[256];
		char tmp[80];
		char filename[256];
		int ret = 0;
		struct tris_config* msg_cfg;
		struct tris_flags config_flags = { CONFIG_FLAG_NOCACHE };
		const char *cid, *datetime, *duration;
		char *name, *callerid;

		/* If no mailbox, return immediately */
		if (tris_strlen_zero(mailbox))
			return 0;

		if (tris_strlen_zero(folder))
			folder = "INBOX";
		if (tris_strlen_zero(context))
			context = "default";

		snprintf(fn, sizeof(fn), "%s%s/%s/%s", VM_SPOOL_DIR, context, mailbox, folder);

		if (!(dir = opendir(fn)))
			return 0;
		
		while ((de = readdir(dir))) {
			if (!strncasecmp(de->d_name, "msg", 3)) {
				if (!strncasecmp(de->d_name + 8, "txt", 3)){
					
					snprintf(filename, sizeof(filename), "%s/%s", fn ,de->d_name);
					msg_cfg = tris_config_load(filename, config_flags);
					if (!msg_cfg) {
						tris_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
						return 0;
					}
					
					if (!(cid = tris_variable_retrieve(msg_cfg, "message", "callerid"))) {
						tris_config_destroy(msg_cfg);
						return 0;
					}
					tris_callerid_parse((char*)cid, &name, &callerid);
					if (!(datetime = tris_variable_retrieve(msg_cfg, "message", "origdate"))) {
						tris_config_destroy(msg_cfg);
						return 0;
					}
					if (!(duration = tris_variable_retrieve(msg_cfg, "message", "duration"))) {
						tris_config_destroy(msg_cfg);
						return 0;
					}
					snprintf(tmp, sizeof(tmp), ",%s,%s,%s,%s\r\n", name, callerid, datetime, duration);
					strncat(vmlist, de->d_name, 7);
					strcat(vmlist, tmp);
					tris_config_destroy(msg_cfg);
				}
			}
		}

		closedir(dir);
		
		tris_verbose("%s\n", vmlist);
		
		return ret;
	}
	
	static int get_vmlist(const char *mailbox, const char *folder, char *vmlist) 
	{
		char tmp[256], *tmp2 = tmp, *mbox, *context;

		vmlist[0]='\0';
				
		tris_copy_string(tmp, mailbox, sizeof(tmp));
		while ((mbox = strsep(&tmp2, ","))) {
			if ((context = strchr(mbox, '@')))
				*context++ = '\0';
			else
				context = "default";
			if (__get_vmlist(context, mbox, folder, vmlist))
				return 1;
		}
		return 0;
	}

#endif

	static void run_externnotify(char *context, char *extension)
	{
		char arguments[255];
		char ext_context[256] = "";
		int newvoicemails = 0, oldvoicemails = 0;
		struct tris_smdi_mwi_message *mwi_msg;

		if (!tris_strlen_zero(context))
			snprintf(ext_context, sizeof(ext_context), "%s@%s", extension, context);
		else
			tris_copy_string(ext_context, extension, sizeof(ext_context));

		if (smdi_iface) {
			if (tris_app_has_voicemail(ext_context, NULL)) 
				tris_smdi_mwi_set(smdi_iface, extension);
			else
				tris_smdi_mwi_unset(smdi_iface, extension);

			if ((mwi_msg = tris_smdi_mwi_message_wait_station(smdi_iface, SMDI_MWI_WAIT_TIMEOUT, extension))) {
				tris_log(LOG_ERROR, "Error executing SMDI MWI change for %s\n", extension);
				if (!strncmp(mwi_msg->cause, "INV", 3))
					tris_log(LOG_ERROR, "Invalid MWI extension: %s\n", mwi_msg->fwd_st);
				else if (!strncmp(mwi_msg->cause, "BLK", 3))
					tris_log(LOG_WARNING, "MWI light was already on or off for %s\n", mwi_msg->fwd_st);
				tris_log(LOG_WARNING, "The switch reported '%s'\n", mwi_msg->cause);
				ASTOBJ_UNREF(mwi_msg, tris_smdi_mwi_message_destroy);
			} else {
				tris_debug(1, "Successfully executed SMDI MWI change for %s\n", extension);
			}
		}

		if (!tris_strlen_zero(externnotify)) {
			if (inboxcount(ext_context, &newvoicemails, &oldvoicemails)) {
				tris_log(LOG_ERROR, "Problem in calculating number of voicemail messages available for extension %s\n", extension);
			} else {
				snprintf(arguments, sizeof(arguments), "%s %s %s %d&", externnotify, context, extension, newvoicemails);
				tris_debug(1, "Executing %s\n", arguments);
				tris_safe_system(arguments);
			}
		}
	}

	struct leave_vm_options {
		unsigned int flags;
		signed char record_gain;
		char *exitcontext;
	};

	static int leave_voicemail(struct tris_channel *chan, char *ext, struct leave_vm_options *options)
	{
		char tmptxtfile[PATH_MAX];
		struct vm_state *vms = NULL;
		int res = 0;
		int duration = 0;
		int maxsecs = 0;
		char fmt[80];
		char ecodes[17] = "#";
		struct tris_vm_user *vmu = NULL, vmus;
		char *context;

		if (!tris_strlen_zero(ext)) { /* leave voicemail for user calling */
			context = strchr(ext, '@');
			if (!vm_user_exist(ext)) {
				return 0;
			} else {
				if (!(vmu = create_user(&vmus, context, ext))) {
					return 0;
				}
				maxsecs = vmu->maxsecs;
			}
		} 
		
		res = tris_play_and_wait(chan, INTRO);
		res = tris_stream_and_wait(chan, "beep", ecodes);

		tris_copy_string(fmt, vmfmts, sizeof(fmt));
		res = play_record_review(chan, NULL, tmptxtfile, maxsecs, fmt, 1, vmu, &duration, NULL, options->record_gain, vms);

		return res;
	}
	
	static int check_commander(char *roomno, char * ext)
	{
		char sql[256];
		char result[1024];
		char *tmp = 0, *cur;
	
		snprintf(sql, sizeof(sql), "SELECT commander_uid FROM general_command WHERE roomno='%s' AND commander_uid REGEXP '.*%s.*'", roomno, ext);
		sql_select_query_execute(result, sql);
		
		if(tris_strlen_zero(result)){
			return 0;
		}

		cur = result;
		while (cur) {
			tmp = strsep(&cur, ",");
			if (!tmp)
				return 0;
			if (strlen(tmp) == strlen(ext) && !strncmp(tmp, ext, strlen(ext)))
				return 1;
		}
	
		return 0;
	
	}

	static int leave_cmd(struct tris_channel *chan, struct leave_vm_options *options)
	{
		char tmptxtfile[PATH_MAX];
		struct vm_state *vms = NULL;
		int res = 0;
		int duration = 0;
		int maxsecs = 0;
		char fmt[80];
		char cmd[40];
		char ecodes[17] = "#";
		struct tris_vm_user *vmu = NULL, vmus;
		char *context, ext[256];
		char password[256];
		int logentry = 0;

choice_roomno:
		res = tris_app_getdata(chan, "voicemail/cmd_choice_roomno",cmd, sizeof(cmd)-1, 0);
		//cmd = tris_play_and_wait(chan, "voicemail/cmd_choice_roomno");
		//if(!cmd)
		//	cmd = tris_waitfordigit(chan, 6000);

		/* check commander id and pin */

		if (!cmdroom_exist(cmd)) {
			tris_verbose("There is no command room");
			if(!tris_strlen_zero(cmd))
				tris_play_and_wait(chan, "voicemail/cmd_not_found_room");
			logentry ++;
			if (logentry > 2) {
				tris_play_and_wait(chan, "voicemail/bye");
				return 0;
			}
			goto choice_roomno;
		} else {
			sprintf(ext, "%s", cmd);
			context = tris_strdupa("cmd");
			if (!(vmu = create_user(&vmus,context, ext))) {
				tris_verbose("Failed in create user ");
				return 0;
			}
		}

		if (!check_commander(cmd, chan->cid.cid_num)) {
			tris_play_and_wait(chan, "voicemail/cmd_no_commander");
			tris_play_and_wait(chan, "voicemail/bye");
			return 0;
		}

		logentry = 0;
		res = tris_app_getdata(chan, "voicemail/enter_pin", password, sizeof(password) - 1, 0);
		while (res >= 0 && !vm_login(chan->cid.cid_num, password)) {
			if(!res)
				tris_play_and_wait(chan, "voicemail/invalid_pin");
			logentry ++;
			if (logentry > 2) {
				tris_play_and_wait(chan, "voicemail/bye");
				return 0;
			}
			res = tris_app_getdata(chan, "voicemail/enter_pin", password, sizeof(password) - 1, 0);
		}
		
		res = tris_play_and_wait(chan, "voicemail/cmd_record_msg");
		res = tris_stream_and_wait(chan, "beep", ecodes);
		
		tris_copy_string(fmt, vmfmts, sizeof(fmt));
		res = play_record_review_cmd(chan, NULL, tmptxtfile, maxsecs, fmt, 1, vmu, &duration, NULL, options->record_gain, vms);

		return res;
	}

	static int leave_rprt(struct tris_channel *chan, struct leave_vm_options *options)
	{
		char tmptxtfile[PATH_MAX];
		struct vm_state *vms = NULL;
		int res = 0;
		int duration = 0;
		int maxsecs = 0;
		char fmt[80];
		char cmd[40];
		char ecodes[17] = "#";
		struct tris_vm_user *vmu = NULL, vmus;
		char *context, ext[256];
		char password[256];
		int logentry = 0;

		while(res >=0) {
			res = tris_app_getdata(chan, "voicemail/rprt_choice_roomno", cmd, sizeof(cmd)-1, 0);
			//if (!cmd)
			//	cmd = tris_waitfordigit(chan, 6000);

			/* check commander id and pin */
			logentry ++;
			if(tris_strlen_zero(cmd)) {
				if (logentry > 2) {
					tris_play_and_wait(chan, "voicemail/bye");
					return 0;
				} 
				continue;
			}

			if(res == -1) return 0;
			
			res = rprtroom_exist(cmd);
			if (res != 1) {
				tris_verbose("There is no report room\n");
				tris_play_and_wait(chan, "voicemail/rprt_not_found_room");
				if (logentry > 2) {
					tris_play_and_wait(chan, "voicemail/bye");
					return 0;
				} 
			} else {
				sprintf(ext, "%s", cmd);
				context = tris_strdupa("report");
				if (!(vmu = create_user(&vmus, context, ext))) {
					tris_verbose("Failed in create user ");
					return 0;
				}
				break;
			}
		}

		if(res == -1) 
			return 0;
		
		/* Get extn */
		/*logentry = 0;
		res = tris_app_getdata(chan, "voicemail/dial_extn_pound", ext, sizeof(ext) - 1, 0);
		while (res >= 0 && !check_reporter(cmd, ext)) {
			tris_verbose("             -- %d                 \n", res);
			if(!res)
				tris_play_and_wait(chan, "voicemail/rprt_is_not_reporter");
			logentry ++;
			if (logentry > 2) {
				tris_play_and_wait(chan, "voicemail/bye");
				return 0;
			}
			res = tris_app_getdata(chan, "voicemail/dial_extn_pound", ext, sizeof(ext) - 1, 0);
		}
		if(res == -1) return 0;*/
		if (!chan->cid.cid_num || tris_strlen_zero(chan->cid.cid_num) || !check_reporter(cmd, chan->cid.cid_num)) {
			tris_play_and_wait(chan, "voicemail/rprt_is_not_reporter");
			tris_play_and_wait(chan, "voicemail/bye");
			return 0;
		}
		strcpy(ext, chan->cid.cid_num);
		
		/* Get pin */
		logentry = 0;
		res = tris_app_getdata(chan, "voicemail/enter_pin", password, sizeof(password) - 1, 0);
		while (res >= 0 && !check_reporter_pin(ext, password)) {
			if(!res)
				tris_play_and_wait(chan, "voicemail/invalid_pin");
			logentry ++;
			if (logentry > 2) {
				tris_play_and_wait(chan, "voicemail/bye");
				return 0;
			}
			res = tris_app_getdata(chan, "voicemail/enter_pin", password, sizeof(password) - 1, 0);
		}

		if(res == -1) return 0;
		
		res = tris_play_and_wait(chan, "voicemail/rprt_record_report");
		res = tris_stream_and_wait(chan, "beep", ecodes);
		
		tris_copy_string(fmt, vmfmts, sizeof(fmt));
		res = play_record_review_rprt(chan, NULL, tmptxtfile, maxsecs, fmt, 1, vmu, &duration, NULL, options->record_gain, vms, ext);

		return res;
	}

#ifndef IMAP_STORAGE
	static int resequence_mailbox(struct tris_vm_user *vmu, char *dir)
	{
		/* we know max messages, so stop process when number is hit */

		int x, dest;
		char sfn[PATH_MAX];
		char dfn[PATH_MAX];

		if (vm_lock_path(dir))
			return ERROR_LOCK_PATH;

		for (x = 0, dest = 0; x < vmu->maxmsg; x++) {
			make_file(sfn, sizeof(sfn), dir, x);
			if (EXISTS(dir, x, sfn, NULL)) {
				
				if (x != dest) {
					make_file(dfn, sizeof(dfn), dir, dest);
					RENAME(dir, x, vmu->mailbox, vmu->context, dir, dest, sfn, dfn);
				}
				
				dest++;
			}
		}
		tris_unlock_path(dir);

		return 0;
	}
#endif

	static int say_and_wait(struct tris_channel *chan, int num, const char *language)
	{
		int d;
		d = tris_say_number(chan, num, TRIS_DIGIT_ANY, language, NULL);
		return d;
	}

	static int save_to_folder(struct tris_vm_user *vmu, struct vm_state *vms, int msg, int box)
	{
#ifdef IMAP_STORAGE
		/* we must use mbox(x) folder names, and copy the message there */
		/* simple. huh? */
		char sequence[10];
		char mailbox[256];
		/* get the real IMAP message number for this message */
		snprintf(sequence, sizeof(sequence), "%ld", vms->msgArray[msg]);
		
		tris_debug(3, "Copying sequence %s to mailbox %s\n", sequence, mbox(box));
		if (box == OLD_FOLDER) {
			mail_setflag(vms->mailstream, sequence, "\\Seen");
		} else if (box == NEW_FOLDER) {
			mail_clearflag(vms->mailstream, sequence, "\\Seen");
		}
		if (!strcasecmp(mbox(NEW_FOLDER), vms->curbox) && (box == NEW_FOLDER || box == OLD_FOLDER))
			return 0;
		/* Create the folder if it don't exist */
		imap_mailbox_name(mailbox, sizeof(mailbox), vms, box, 1); /* Get the full mailbox name */
		tris_debug(5, "Checking if folder exists: %s\n",mailbox);
		if (mail_create(vms->mailstream, mailbox) == NIL) 
			tris_debug(5, "Folder exists.\n");
		else
			tris_log(LOG_NOTICE, "Folder %s created!\n",mbox(box));
		return !mail_copy(vms->mailstream, sequence, (char *)mbox(box));
#else
		char *dir = vms->curdir;
		char *username = vms->username;
		char *context = vmu->context;
		char sfn[PATH_MAX];
		char dfn[PATH_MAX];
		char ddir[PATH_MAX];
		char curdir[PATH_MAX];
		const char *dbox = mbox(box);
		int x, i;
		create_dirpath(ddir, sizeof(ddir), context, username, dbox);

		if (vm_lock_path(ddir))
			return ERROR_LOCK_PATH;

		x = last_message_index(vmu, ddir) + 1;

		if (box == DELETED_FOLDER && x >= vmu->maxdeletedmsg) { /* "Deleted" folder*/
			x--;
			for (i = 1; i <= x; i++) {
				/* Push files down a "slot".  The oldest file (msg0000) will be deleted. */
				make_file(sfn, sizeof(sfn), ddir, i);
				make_file(dfn, sizeof(dfn), ddir, i - 1);
				if (EXISTS(ddir, i, sfn, NULL)) {
					RENAME(ddir, i, vmu->mailbox, vmu->context, ddir, i - 1, sfn, dfn);
				} else
					break;
			}
		} else if(box == OLD_FOLDER && x >= vmu->maxmsg) {
			tris_copy_string(curdir, vms->curdir, sizeof(curdir));
			create_dirpath(vms->curdir, sizeof(vms->curdir), context, username, mbox(OLD_FOLDER));
			// msg0000 will be sent to Deleted Box
			save_to_folder(vmu, vms, 0, DELETED_FOLDER);
			tris_copy_string(vms->curdir, curdir, sizeof(vms->curdir));
			x--;
			for (i = 1; i <= x; i++) {
				/* Push files down a "slot".  The oldest file (msg0000) will be deleted. */
				make_file(sfn, sizeof(sfn), ddir, i);
				make_file(dfn, sizeof(dfn), ddir, i - 1);
				if (EXISTS(ddir, i, sfn, NULL)) {
					RENAME(ddir, i, vmu->mailbox, vmu->context, ddir, i - 1, sfn, dfn);
				} else
					break;
			}
			
		} else {
			//if (x >= vmu->maxmsg) {
			//	tris_unlock_path(ddir);
			//	return ERROR_MAILBOX_FULL;
			//}
		}
		make_file(sfn, sizeof(sfn), dir, msg);
		make_file(dfn, sizeof(dfn), ddir, x);
		if (strcmp(sfn, dfn)) {
			COPY(dir, msg, ddir, x, username, context, sfn, dfn);
		}
		tris_unlock_path(ddir);
#endif
		return 0;
	}

	static int adsi_logo(unsigned char *buf)
	{
		int bytes = 0;
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_CENT, 0, "Comedian Mail", "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_CENT, 0, "(C)2002-2006 Digium, Inc.", "");
		return bytes;
	}

	static int adsi_load_vmail(struct tris_channel *chan, int *useadsi)
	{
		unsigned char buf[256];
		int bytes = 0;
		int x;
		char num[5];

		*useadsi = 0;
		bytes += tris_adsi_data_mode(buf + bytes);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

		bytes = 0;
		bytes += adsi_logo(buf);
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
#ifdef DISPLAY
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .", "");
#endif
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += tris_adsi_data_mode(buf + bytes);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

		if (tris_adsi_begin_download(chan, addesc, adsifdn, adsisec, adsiver)) {
			bytes = 0;
			bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Cancelled.", "");
			bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
			bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
			bytes += tris_adsi_voice_mode(buf + bytes, 0);
			tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
			return 0;
		}

#ifdef DISPLAY
		/* Add a dot */
		bytes = 0;
		bytes += tris_adsi_logo(buf);
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ..", "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif
		bytes = 0;
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 0, "Listen", "Listen", "1", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 1, "Folder", "Folder", "2", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 2, "Advanced", "Advnced", "3", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Options", "Options", "0", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 4, "Help", "Help", "*", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 5, "Exit", "Exit", "#", 1);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
		/* Add another dot */
		bytes = 0;
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ...", "");
		bytes += tris_adsi_voice_mode(buf + bytes, 0);

		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

		bytes = 0;
		/* These buttons we load but don't use yet */
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 6, "Previous", "Prev", "4", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 8, "Repeat", "Repeat", "5", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 7, "Delete", "Delete", "7", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 9, "Next", "Next", "6", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 10, "Save", "Save", "9", 1);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 11, "Undelete", "Restore", "7", 1);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
		/* Add another dot */
		bytes = 0;
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ....", "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

		bytes = 0;
		for (x = 0; x < 5; x++) {
			snprintf(num, sizeof(num), "%d", x);
			bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + x, mbox(x), mbox(x), num, 1);
		}
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + 5, "Cancel", "Cancel", "#", 1);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
		/* Add another dot */
		bytes = 0;
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .....", "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

		if (tris_adsi_end_download(chan)) {
			bytes = 0;
			bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Download Unsuccessful.", "");
			bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
			bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
			bytes += tris_adsi_voice_mode(buf + bytes, 0);
			tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
			return 0;
		}
		bytes = 0;
		bytes += tris_adsi_download_disconnect(buf + bytes);
		bytes += tris_adsi_voice_mode(buf + bytes, 0);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

		tris_debug(1, "Done downloading scripts...\n");

#ifdef DISPLAY
		/* Add last dot */
		bytes = 0;
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "   ......", "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
#endif
		tris_debug(1, "Restarting session...\n");

		bytes = 0;
		/* Load the session now */
		if (tris_adsi_load_session(chan, adsifdn, adsiver, 1) == 1) {
			*useadsi = 1;
			bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Scripts Loaded!", "");
		} else
			bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Failed!", "");

		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
		return 0;
	}

	static void adsi_begin(struct tris_channel *chan, int *useadsi)
	{
		int x;
		if (!tris_adsi_available(chan))
			return;
		x = tris_adsi_load_session(chan, adsifdn, adsiver, 1);
		if (x < 0)
			return;
		if (!x) {
			if (adsi_load_vmail(chan, useadsi)) {
				tris_log(LOG_WARNING, "Unable to upload voicemail scripts\n");
				return;
			}
		} else
			*useadsi = 1;
	}

	static void adsi_login(struct tris_channel *chan)
	{
		unsigned char buf[256];
		int bytes = 0;
		unsigned char keys[8];
		int x;
		if (!tris_adsi_available(chan))
			return;

		for (x = 0; x < 8; x++)
			keys[x] = 0;
		/* Set one key for next */
		keys[3] = ADSI_KEY_APPS + 3;

		bytes += adsi_logo(buf + bytes);
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, " ", "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, " ", "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += tris_adsi_input_format(buf + bytes, 1, ADSI_DIR_FROM_LEFT, 0, "Mailbox: ******", "");
		bytes += tris_adsi_input_control(buf + bytes, ADSI_COMM_PAGE, 4, 1, 1, ADSI_JUST_LEFT);
		bytes += tris_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Enter", "Enter", "#", 1);
		bytes += tris_adsi_set_keys(buf + bytes, keys);
		bytes += tris_adsi_voice_mode(buf + bytes, 0);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_message(struct tris_channel *chan, struct vm_state *vms)
	{
		int bytes = 0;
		unsigned char buf[256]; 
		char buf1[256], buf2[256];
		char fn2[PATH_MAX];

		char cid[256] = "";
		char *val;
		char *name, *num;
		char datetime[21] = "";
		FILE *f;

		unsigned char keys[8];

		int x;

		if (!tris_adsi_available(chan))
			return;

		/* Retrieve important info */
		snprintf(fn2, sizeof(fn2), "%s.txt", vms->fn);
		f = fopen(fn2, "r");
		if (f) {
			while (!feof(f)) {	
				if (!fgets((char *)buf, sizeof(buf), f)) {
					continue;
				}
				if (!feof(f)) {
					char *stringp = NULL;
					stringp = (char *)buf;
					strsep(&stringp, "=");
					val = strsep(&stringp, "=");
					if (!tris_strlen_zero(val)) {
						if (!strcmp((char *)buf, "callerid"))
							tris_copy_string(cid, val, sizeof(cid));
						if (!strcmp((char *)buf, "origdate"))
							tris_copy_string(datetime, val, sizeof(datetime));
					}
				}
			}
			fclose(f);
		}
		/* New meaning for keys */
		for (x = 0; x < 5; x++)
			keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);
		keys[6] = 0x0;
		keys[7] = 0x0;

		if (!vms->curmsg) {
			/* No prev key, provide "Folder" instead */
			keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
		}
		if (vms->curmsg >= vms->lastmsg) {
			/* If last message ... */
			if (vms->curmsg) {
				/* but not only message, provide "Folder" instead */
				keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
				bytes += tris_adsi_voice_mode(buf + bytes, 0);

			} else {
				/* Otherwise if only message, leave blank */
				keys[3] = 1;
			}
		}

		if (!tris_strlen_zero(cid)) {
			tris_callerid_parse(cid, &name, &num);
			if (!name)
				name = num;
		} else
			name = "Unknown Caller";

		/* If deleted, show "undeleted" */

		if (vms->deleted[vms->curmsg])
			keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

		/* Except "Exit" */
		keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
		snprintf(buf1, sizeof(buf1), "%s%s", vms->curbox,
			strcasecmp(vms->curbox, "INBOX") ? " Messages" : "");
		snprintf(buf2, sizeof(buf2), "Message %d of %d", vms->curmsg + 1, vms->lastmsg + 1);

		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, name, "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, datetime, "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += tris_adsi_set_keys(buf + bytes, keys);
		bytes += tris_adsi_voice_mode(buf + bytes, 0);

		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_delete(struct tris_channel *chan, struct vm_state *vms)
	{
		int bytes = 0;
		unsigned char buf[256];
		unsigned char keys[8];

		int x;

		if (!tris_adsi_available(chan))
			return;

		/* New meaning for keys */
		for (x = 0; x < 5; x++)
			keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);

		keys[6] = 0x0;
		keys[7] = 0x0;

		if (!vms->curmsg) {
			/* No prev key, provide "Folder" instead */
			keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
		}
		if (vms->curmsg >= vms->lastmsg) {
			/* If last message ... */
			if (vms->curmsg) {
				/* but not only message, provide "Folder" instead */
				keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
			} else {
				/* Otherwise if only message, leave blank */
				keys[3] = 1;
			}
		}

		/* If deleted, show "undeleted" */
		if (vms->deleted[vms->curmsg]) 
			keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

		/* Except "Exit" */
		keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
		bytes += tris_adsi_set_keys(buf + bytes, keys);
		bytes += tris_adsi_voice_mode(buf + bytes, 0);

		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_status(struct tris_channel *chan, struct vm_state *vms)
	{
		unsigned char buf[256] = "";
		char buf1[256] = "", buf2[256] = "";
		int bytes = 0;
		unsigned char keys[8];
		int x;

		char *newm = (vms->newmessages == 1) ? "message" : "messages";
		char *oldm = (vms->oldmessages == 1) ? "message" : "messages";
		if (!tris_adsi_available(chan))
			return;
		if (vms->newmessages) {
			snprintf(buf1, sizeof(buf1), "You have %d new", vms->newmessages);
			if (vms->oldmessages) {
				strncat(buf1, " and", sizeof(buf1) - strlen(buf1) - 1);
				snprintf(buf2, sizeof(buf2), "%d old %s.", vms->oldmessages, oldm);
			} else {
				snprintf(buf2, sizeof(buf2), "%s.", newm);
			}
		} else if (vms->oldmessages) {
			snprintf(buf1, sizeof(buf1), "You have %d old", vms->oldmessages);
			snprintf(buf2, sizeof(buf2), "%s.", oldm);
		} else {
			strcpy(buf1, "You have no messages.");
			buf2[0] = ' ';
			buf2[1] = '\0';
		}
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);

		for (x = 0; x < 6; x++)
			keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + x);
		keys[6] = 0;
		keys[7] = 0;

		/* Don't let them listen if there are none */
		if (vms->lastmsg < 0)
			keys[0] = 1;
		bytes += tris_adsi_set_keys(buf + bytes, keys);

		bytes += tris_adsi_voice_mode(buf + bytes, 0);

		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	/*
	static void adsi_clear(struct tris_channel *chan)
	{
		char buf[256];
		int bytes = 0;
		if (!tris_adsi_available(chan))
			return;
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += tris_adsi_voice_mode(buf + bytes, 0);

		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}
	*/

	static void adsi_goodbye(struct tris_channel *chan)
	{
		unsigned char buf[256];
		int bytes = 0;

		if (!tris_adsi_available(chan))
			return;
		bytes += adsi_logo(buf + bytes);
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, " ", "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Goodbye", "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += tris_adsi_voice_mode(buf + bytes, 0);

		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	/*!\brief get_folder: Folder menu
	*	Plays "press 1 for INBOX messages" etc.
	*	Should possibly be internationalized
	*/
	static int get_folder(struct tris_channel *chan, int start)
	{
		int x;
		int d;
		char fn[PATH_MAX];
		d = tris_play_and_wait(chan, "voicemail/vm-press");	/* "Press" */
		if (d)
			return d;
		for (x = start; x < 5; x++) {	/* For all folders */
			if ((d = tris_say_number(chan, x, TRIS_DIGIT_ANY, chan->language, NULL)))
				return d;
			d = tris_play_and_wait(chan, "voicemail/vm-for");	/* "for" */
			if (d)
				return d;
			snprintf(fn, sizeof(fn), "voicemail/vm-%s", mbox(x));	/* Folder name */
			d = vm_play_folder_name(chan, fn);
			if (d)
				return d;
			d = tris_waitfordigit(chan, 5000);
			if (d)
				return d;
		}
		d = tris_play_and_wait(chan, "voicemail/vm-tocancel"); /* "or pound to cancel" */
		if (d)
			return d;
		d = tris_waitfordigit(chan, 4000);
		return d;
	}

	static int vm_forwardoptions(struct tris_channel *chan, struct tris_vm_user *vmu, char *curdir, int curmsg, char *vmfmts,
				char *context, signed char record_gain, long *duration, struct vm_state *vms)
	{
		int cmd = 0;
		int retries = 0, prepend_duration = 0, already_recorded = 0;
		signed char zero_gain = 0;
		struct tris_flags config_flags = { CONFIG_FLAG_NOCACHE };
		struct tris_config *msg_cfg;
		const char *duration_str;
		char msgfile[PATH_MAX], backup[PATH_MAX];
		char textfile[PATH_MAX];

		/* Must always populate duration correctly */
		make_file(msgfile, sizeof(msgfile), curdir, curmsg);
		strcpy(textfile, msgfile);
		strcpy(backup, msgfile);
		strncat(textfile, ".txt", sizeof(textfile) - strlen(textfile) - 1);
		strncat(backup, "-bak", sizeof(backup) - strlen(backup) - 1);

		if ((msg_cfg = tris_config_load(textfile, config_flags)) && (duration_str = tris_variable_retrieve(msg_cfg, "message", "duration"))) {
			*duration = atoi(duration_str);
		} else {
			*duration = 0;
		}

		while ((cmd >= 0) && (cmd != 't') && (cmd != '*')) {
			if (cmd)
				retries = 0;
			switch (cmd) {
			case '1': 
				/* prepend a message to the current message, update the metadata and return */
				prepend_duration = 0;

				/* if we can't read the message metadata, stop now */
				if (!msg_cfg) {
					cmd = 0;
					break;
				}

				/* Back up the original file, so we can retry the prepend */
				if (already_recorded)
					tris_filecopy(backup, msgfile, NULL);
				else
					tris_filecopy(msgfile, backup, NULL);
				already_recorded = 1;

				if (record_gain)
					tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);

				cmd = tris_play_and_prepend(chan, NULL, msgfile, 0, vmfmts, &prepend_duration, 1, silencethreshold, maxsilence);
				if (record_gain)
					tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);

				if (prepend_duration) {
					struct tris_category *msg_cat;
					/* need enough space for a maximum-length message duration */
					char duration_str[12];

					prepend_duration += *duration;
					msg_cat = tris_category_get(msg_cfg, "message");
					snprintf(duration_str, sizeof(duration_str), "%d", prepend_duration);
					if (!tris_variable_update(msg_cat, "duration", duration_str, NULL, 0)) {
						tris_config_text_file_save(textfile, msg_cfg, "app_voicemail");
						STORE(curdir, vmu->mailbox, context, curmsg, chan, vmu, vmfmts, prepend_duration, vms);
					}
				}

				break;
			case '2': 
				cmd = 't';
				break;
			case '*':
				cmd = '*';
				break;
			default: 
				cmd = tris_play_and_wait(chan, "voicemail/vm-forwardoptions");
					/* "Press 1 to prepend a message or 2 to forward the message without prepending" */
				if (!cmd)
					cmd = tris_play_and_wait(chan, "voicemail/vm-starmain");
					/* "press star to return to the main menu" */
				if (!cmd)
					cmd = tris_waitfordigit(chan, 6000);
				if (!cmd)
					retries++;
				if (retries > 3)
					cmd = 't';
			}
		}

		if (msg_cfg)
			tris_config_destroy(msg_cfg);
		if (already_recorded)
			tris_filedelete(backup, NULL);
		if (prepend_duration)
			*duration = prepend_duration;

		if (cmd == 't' || cmd == 'S')
			cmd = 0;
		return cmd;
	}

	static void queue_mwi_event(const char *mbox, int new, int old)
	{
		struct tris_event *event;
		char *mailbox, *context;

		/* Strip off @default */
		context = mailbox = tris_strdupa(mbox);
		strsep(&context, "@");
		if (tris_strlen_zero(context))
			context = "default";

		if (!(event = tris_event_new(TRIS_EVENT_MWI,
				TRIS_EVENT_IE_MAILBOX, TRIS_EVENT_IE_PLTYPE_STR, mailbox,
				TRIS_EVENT_IE_CONTEXT, TRIS_EVENT_IE_PLTYPE_STR, context,
				TRIS_EVENT_IE_NEWMSGS, TRIS_EVENT_IE_PLTYPE_UINT, new,
				TRIS_EVENT_IE_OLDMSGS, TRIS_EVENT_IE_PLTYPE_UINT, old,
				TRIS_EVENT_IE_END))) {
			return;
		}

		tris_event_queue_and_cache(event);
	}

	static int notify_new_message(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms, int msgnum, long duration, char *fmt, char *cidnum, char *cidname)
	{
		char todir[PATH_MAX], fn[PATH_MAX], ext_context[PATH_MAX], *stringp;
		int newmsgs = 0, oldmsgs = 0;
		const char *category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");
		char *myserveremail = serveremail;

		make_dir(todir, sizeof(todir), vmu->context, vmu->mailbox, "INBOX");
		make_file(fn, sizeof(fn), todir, msgnum);
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vmu->mailbox, vmu->context);

		if (!tris_strlen_zero(vmu->attachfmt)) {
			if (strstr(fmt, vmu->attachfmt))
				fmt = vmu->attachfmt;
			else
				tris_log(LOG_WARNING, "Attachment format '%s' is not one of the recorded formats '%s'.  Falling back to default format for '%s@%s'.\n", vmu->attachfmt, fmt, vmu->mailbox, vmu->context);
		}

		/* Attach only the first format */
		fmt = tris_strdupa(fmt);
		stringp = fmt;
		strsep(&stringp, "|");

		if (!tris_strlen_zero(vmu->serveremail))
			myserveremail = vmu->serveremail;

		if (!tris_strlen_zero(vmu->email)) {
			int attach_user_voicemail = tris_test_flag(vmu, VM_ATTACH);
			if (!attach_user_voicemail)
				attach_user_voicemail = tris_test_flag((&globalflags), VM_ATTACH);

			if (attach_user_voicemail)
				RETRIEVE(todir, msgnum, vmu->mailbox, vmu->context);

			/* XXX possible imap issue, should category be NULL XXX */
			sendmail(myserveremail, vmu, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, fn, fmt, duration, attach_user_voicemail, chan, category);

			if (attach_user_voicemail)
				DISPOSE(todir, msgnum);
		}

		if (!tris_strlen_zero(vmu->pager))
			sendpage(myserveremail, vmu->pager, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, duration, vmu, category);

		if (tris_test_flag(vmu, VM_DELETE))
			DELETE(todir, msgnum, fn, vmu);

		/* Leave voicemail for someone */
		if (tris_app_has_voicemail(ext_context, NULL)) 
			tris_app_inboxcount(ext_context, &newmsgs, &oldmsgs);

		queue_mwi_event(ext_context, newmsgs, oldmsgs);

		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s@%s\r\nWaiting: %d\r\nNew: %d\r\nOld: %d\r\n", vmu->mailbox, vmu->context, tris_app_has_voicemail(ext_context, NULL), newmsgs, oldmsgs);
		run_externnotify(vmu->context, vmu->mailbox);

		return 0;
	}

	static int wait_file2(struct tris_channel *chan, struct vm_state *vms, char *file)
	{
		int res;
		if ((res = tris_stream_and_wait(chan, file, TRIS_DIGIT_ANY)) < 0) 
			tris_log(LOG_WARNING, "Unable to play message %s\n", file); 
		return res;
	}

	static int wait_file(struct tris_channel *chan, struct vm_state *vms, char *file) 
	{
		return tris_control_streamfile(chan, file, listen_control_forward_key, listen_control_reverse_key, listen_control_stop_key, listen_control_pause_key, listen_control_restart_key, skipms, NULL);
	}

	static int play_message_datetime(struct tris_channel *chan, struct tris_vm_user *vmu, const char *origtime, const char *filename)
	{
		int res = 0;
		struct vm_zone *the_zone = NULL;
		time_t t;

		if (tris_get_time_t(origtime, &t, 0, NULL)) {
			tris_log(LOG_WARNING, "Couldn't find origtime in %s\n", filename);
			return 0;
		}

		/* Does this user have a timezone specified? */
		if (!tris_strlen_zero(vmu->zonetag)) {
			/* Find the zone in the list */
			struct vm_zone *z;
			TRIS_LIST_LOCK(&zones);
			TRIS_LIST_TRAVERSE(&zones, z, list) {
				if (!strcmp(z->name, vmu->zonetag)) {
					the_zone = z;
					break;
				}
			}
			TRIS_LIST_UNLOCK(&zones);
		}

	/* No internal variable parsing for now, so we'll comment it out for the time being */
#if 0
		/* Set the DIFF_* variables */
		tris_localtime(&t, &time_now, NULL);
		tv_now = tris_tvnow();
		tris_localtime(&tv_now, &time_then, NULL);

		/* Day difference */
		if (time_now.tm_year == time_then.tm_year)
			snprintf(temp, sizeof(temp), "%d", time_now.tm_yday);
		else
			snprintf(temp, sizeof(temp), "%d", (time_now.tm_year - time_then.tm_year) * 365 + (time_now.tm_yday - time_then.tm_yday));
		pbx_builtin_setvar_helper(chan, "DIFF_DAY", temp);

		/* Can't think of how other diffs might be helpful, but I'm sure somebody will think of something. */
#endif
		if (the_zone) {
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, the_zone->msg_format, the_zone->timezone);
		}
		else if (!strcasecmp(chan->language,"pl"))       /* POLISH syntax */
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' Q HM", NULL);
		else if (!strcasecmp(chan->language, "se"))       /* SWEDISH syntax */
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' dB 'digits/at' k 'and' M", NULL);
		else if (!strcasecmp(chan->language, "no"))       /* NORWEGIAN syntax */
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' Q 'digits/at' HM", NULL);
		else if (!strcasecmp(chan->language, "de"))       /* GERMAN syntax */
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' Q 'digits/at' HM", NULL);
		else if (!strcasecmp(chan->language, "nl"))      /* DUTCH syntax */
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' q 'digits/nl-om' HM", NULL);
		else if (!strcasecmp(chan->language, "it"))      /* ITALIAN syntax */
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' q 'digits/at' 'digits/hours' k 'digits/e' M 'digits/minutes'", NULL);
		else if (!strcasecmp(chan->language, "gr"))
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' q  H 'digits/kai' M ", NULL);
		else if (!strcasecmp(chan->language, "pt_BR"))
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "'voicemail/vm-received' Ad 'digits/pt-de' B 'digits/pt-de' Y 'digits/pt-as' HM ", NULL);
		else if (!strcasecmp(chan->language, "tw"))      /* CHINESE (Taiwan) syntax */
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "qR 'voicemail/vm-received'", NULL);		
		else {
			res = tris_say_date_with_format(chan, t, TRIS_DIGIT_ANY, chan->language, "Q pIM", NULL);
		}
#if 0
		pbx_builtin_setvar_helper(chan, "DIFF_DAY", NULL);
#endif
		return res;
	}



	static int play_message_callerid(struct tris_channel *chan, struct vm_state *vms, char *cid, const char *context, int callback)
	{
		int res = 0;
		int i;
		char *callerid, *name;
		char prefile[PATH_MAX] = "";
		

		/* If voicemail cid is not enabled, or we didn't get cid or context from
		* the attribute file, leave now.
		*
		* TODO Still need to change this so that if this function is called by the
		* message envelope (and someone is explicitly requesting to hear the CID),
		* it does not check to see if CID is enabled in the config file.
		*/
		if ((cid == NULL)||(context == NULL))
			return res;

		/* Strip off caller ID number from name */
		tris_debug(1, "VM-CID: composite caller ID received: %s, context: %s\n", cid, context);
		tris_callerid_parse(cid, &name, &callerid);
		if ((!tris_strlen_zero(callerid)) && strcmp(callerid, "Unknown")) {
			/* Check for internal contexts and only */
			/* say extension when the call didn't come from an internal context in the list */
			for (i = 0 ; i < MAX_NUM_CID_CONTEXTS ; i++) {
				tris_debug(1, "VM-CID: comparing internalcontext: %s\n", cidinternalcontexts[i]);
				if ((strcmp(cidinternalcontexts[i], context) == 0))
					break;
			}
			if (i != MAX_NUM_CID_CONTEXTS) { /* internal context? */
				if (!res) {
					snprintf(prefile, sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, context, callerid);
					if (!tris_strlen_zero(prefile)) {
					/* See if we can find a recorded name for this person instead of their extension number */
						if (tris_fileexists(prefile, NULL, NULL) > 0) {
							tris_verb(3, "Playing envelope info: CID number '%s' matches mailbox number, playing recorded name\n", callerid);
							if (!callback)
								res = wait_file2(chan, vms, "voicemail/vm-from");
							res = tris_stream_and_wait(chan, prefile, "");
						} else {
							tris_verb(3, "Playing envelope info: message from '%s'\n", callerid);
							/* Say "from extension" as one saying to sound smoother */
							if (!callback)
								res = wait_file2(chan, vms, "voicemail/vm-from-extension");
							res = tris_say_digit_str(chan, callerid, "", chan->language);
						}
					}
				}
			} else if (!res) {
				if (option_debug > 2)
					tris_log(LOG_DEBUG, "VM-CID: Numeric caller id: (%s)\n",callerid);
				/* BB: Since this is all nicely figured out, why not say "from phone number" in this case" */
				if (!callback)
					res = wait_file2(chan, vms, "voicemail/extension");
				res = tris_say_digit_str(chan, callerid,TRIS_DIGIT_ANY, chan->language);
				if (!callback)
					res = wait_file2(chan, vms, "voicemail/from");
				}
		} else {
			/* Number unknown */
			tris_debug(1, "VM-CID: From an unknown number\n");
			/* Say "from an unknown caller" as one phrase - it is already recorded by "the voice" anyhow */
			res = wait_file2(chan, vms, "voicemail/an_outside_caller");
			res = wait_file2(chan, vms, "voicemail/from");
		}
		return res;
	}

	static int play_message(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms)
	{
		int res = 0;
		char filename[256], *cid;
		const char *origtime, *context, *category, *duration;
		struct tris_config *msg_cfg;
		struct tris_flags config_flags = { CONFIG_FLAG_NOCACHE };

		vms->starting = 0; 
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
		adsi_message(chan, vms);

		/* Retrieve info from VM attribute file */
		make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
		snprintf(filename, sizeof(filename), "%s.txt", vms->fn2);
		RETRIEVE(vms->curdir, vms->curmsg, vmu->mailbox, vmu->context);
		msg_cfg = tris_config_load(filename, config_flags);
		if (!msg_cfg) {
			tris_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
			return 0;
		}

		if (!(origtime = tris_variable_retrieve(msg_cfg, "message", "origtime"))) {
			tris_log(LOG_WARNING, "No origtime?!\n");
			DISPOSE(vms->curdir, vms->curmsg);
			tris_config_destroy(msg_cfg);
			return 0;
		}

		cid = tris_strdupa(tris_variable_retrieve(msg_cfg, "message", "callerid"));
		duration = tris_variable_retrieve(msg_cfg, "message", "duration");
		category = tris_variable_retrieve(msg_cfg, "message", "category");

		context = tris_variable_retrieve(msg_cfg, "message", "context");
		if (!strncasecmp("macro", context, 5)) /* Macro names in contexts are useless for our needs */
			context = tris_variable_retrieve(msg_cfg, "message", "macrocontext");
		/* Allow pressing '1' to skip envelope / callerid */
		if (res == '1')
			res = 0;
		tris_config_destroy(msg_cfg);

		if (!res) {
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
			vms->heard[vms->curmsg] = 1;
			if ((res = wait_file(chan, vms, vms->fn)) < 0) {
				tris_log(LOG_WARNING, "Playback of message %s failed\n", vms->fn);
				res = 0;
			}
		}
		DISPOSE(vms->curdir, vms->curmsg);
		return res;
	}

#ifdef IMAP_STORAGE
static int imap_remove_file(char *dir, int msgnum)
{
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	
	if (msgnum > -1) {
		make_file(fn, sizeof(fn), dir, msgnum);
	} else
		tris_copy_string(fn, dir, sizeof(fn));
	
	if ((msgnum < 0 && imapgreetings) || msgnum > -1) {
		tris_filedelete(fn, NULL);
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		unlink(full_fn);
	}
	return 0;
}

static int imap_delete_old_greeting (char *dir, struct vm_state *vms)
{
	char *file, *filename;
	char *attachment;
	char arg[10];
	int i;
	BODY* body;

	
	file = strrchr(tris_strdupa(dir), '/');
	if (file)
		*file++ = '\0';
	else {
		tris_log(LOG_ERROR, "Failed to procure file name from directory passed. You should never see this.\n");
		return -1;
	}
	
	for (i = 0; i < vms->mailstream->nmsgs; i++) {
		mail_fetchstructure(vms->mailstream, i + 1, &body);
		/* We have the body, now we extract the file name of the first attachment. */
		if (body->nested.part->next && body->nested.part->next->body.parameter->value) {
			attachment = tris_strdupa(body->nested.part->next->body.parameter->value);
		} else {
			tris_log(LOG_ERROR, "There is no file attached to this IMAP message.\n");
			return -1;
		}
		filename = strsep(&attachment, ".");
		if (!strcmp(filename, file)) {
			sprintf (arg, "%d", i + 1);
			mail_setflag(vms->mailstream, arg, "\\DELETED");
		}
	}
	mail_expunge(vms->mailstream);
	return 0;
}

#else
#ifndef IMAP_STORAGE
static int count_all_msgs(struct vm_state *vms, struct tris_vm_user *vmu)
{
	int count_msg=0, res;
	char dir[PATH_MAX];
	unsigned int i=0;

	for(i=0; i<4; i++) {
		create_dirpath(dir, sizeof(dir), vmu->context, vms->username, mbox(i));
		res = count_messages(vmu, dir);
		if(res > 0) 
			count_msg += res;
	}
	return count_msg;
}

static int open_mailbox(struct vm_state *vms, struct tris_vm_user *vmu, int box)
{
	int res = 0;
	int count_msg, last_msg;

	tris_copy_string(vms->curbox, mbox(box), sizeof(vms->curbox));
	
	/* Rename the member vmbox HERE so that we don't try to return before
	 * we know what's going on.
	 */
	snprintf(vms->vmbox, sizeof(vms->vmbox), "voicemail/vm-%s", vms->curbox);
	
	/* Faster to make the directory than to check if it exists. */
	create_dirpath(vms->curdir, sizeof(vms->curdir), vmu->context, vms->username, vms->curbox);

	count_msg = count_messages(vmu, vms->curdir);
	if (count_msg < 0)
		return count_msg;
	else
		vms->lastmsg = count_msg - 1;

	/*
	The following test is needed in case sequencing gets messed up.
	There appears to be more than one way to mess up sequence, so
	we will not try to find all of the root causes--just fix it when
	detected.
	*/

	if (vm_lock_path(vms->curdir)) {
		tris_log(LOG_ERROR, "Could not open mailbox %s:  mailbox is locked\n", vms->curdir);
		return -1;
	}

	last_msg = last_message_index(vmu, vms->curdir);
	tris_unlock_path(vms->curdir);

	if (last_msg < 0) 
		return last_msg;
	else if (vms->lastmsg != last_msg) {
		tris_log(LOG_NOTICE, "Resequencing Mailbox: %s\n", vms->curdir);
		res = resequence_mailbox(vmu, vms->curdir);
		if (res)
			return res;
	}

	return 0;
}
#endif
#endif

static int close_mailbox(struct vm_state *vms, struct tris_vm_user *vmu)
{
	int x = 0;
	//int cmd_or_report = 0;
#ifndef IMAP_STORAGE
	int res = 0, nummsg;
#endif

	if (vms->lastmsg <= -1)
		goto done;

	vms->curmsg = -1; 
#ifndef IMAP_STORAGE
	/* Get the deleted messages fixed */ 
	if (vm_lock_path(vms->curdir))
		return ERROR_LOCK_PATH;

	//cmd_or_report = (!strcasecmp(vmu->context, "cmd") || !strcasecmp(vmu->context, "report"));
	for (x = 0; x < vmu->maxmsg; x++) { 
		if (!vms->deleted[x] && ( !strcasecmp(vmu->context, "cmd") || strcasecmp(vms->curbox, "INBOX") || !vms->heard[x] || (vms->heard[x] && !tris_test_flag(vmu, VM_MOVEHEARD)))) { 
			/* Save this message.  It's not in INBOX or hasn't been heard */ 
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, x); 
			if (!EXISTS(vms->curdir, x, vms->fn, NULL)) 
				break;
			vms->curmsg++; 
			make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg); 
			if (strcmp(vms->fn, vms->fn2)) { 
				RENAME(vms->curdir, x, vmu->mailbox, vmu->context, vms->curdir, vms->curmsg, vms->fn, vms->fn2);
			} 
		} else if ((strcasecmp(vmu->context, "cmd")) && !strcasecmp(vms->curbox, "INBOX") && vms->heard[x] && tris_test_flag(vmu, VM_MOVEHEARD) && !vms->deleted[x]) { 
			/* Move to old folder before deleting */ 
			res = save_to_folder(vmu, vms, x, OLD_FOLDER);
			if (res == ERROR_LOCK_PATH || res == ERROR_MAILBOX_FULL) {
				/* If save failed do not delete the message */
				tris_log(LOG_WARNING, "Save failed.  Not moving message: %s.\n", res == ERROR_LOCK_PATH ? "unable to lock path" : "destination folder full");
				vms->deleted[x] = 0;
				vms->heard[x] = 0;
				--x;
			}
		} else if ((vms->deleted[x] == 1 && vmu->maxdeletedmsg) && strcasecmp(vms->curbox, "DELETED")){
			/* Move to deleted folder */ 
			res = save_to_folder(vmu, vms, x, DELETED_FOLDER);
			if (res == ERROR_LOCK_PATH) {
				/* If save failed do not delete the message */
				vms->deleted[x] = 0;
				vms->heard[x] = 0;
				--x;
			}
		} else if (vms->deleted[x] && tris_check_realtime("voicemail_data")) {
			/* If realtime storage enabled - we should explicitly delete this message,
			cause RENAME() will overwrite files, but will keep duplicate records in RT-storage */
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, x);
			if (EXISTS(vms->curdir, x, vms->fn, NULL))
				DELETE(vms->curdir, x, vms->fn, vmu);
		}
	} 

	/* Delete ALL remaining messages */
	nummsg = x - 1;
	for (x = vms->curmsg + 1; x <= nummsg; x++) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, x);
		if (EXISTS(vms->curdir, x, vms->fn, NULL))
			DELETE(vms->curdir, x, vms->fn, vmu);
	}
	tris_unlock_path(vms->curdir);
#else
	if (vms->deleted) {
		for (x = 0; x < vmu->maxmsg; x++) { 
			if (vms->deleted[x]) { 
				tris_debug(3, "IMAP delete of %d\n", x);
				DELETE(vms->curdir, x, vms->fn, vmu);
			}
		}
	}
#endif

done:
	if (vms->deleted)
		memset(vms->deleted, 0, vmu->maxmsg * sizeof(int)); 
	if (vms->heard)
		memset(vms->heard, 0, vmu->maxmsg * sizeof(int)); 

	return 0;
}

static int manage_mailbox(const char *mailbox, int folder, int *msglist, int msgcount, const char* command, char *result) 
{
	char *context;
	struct vm_state vms;
	struct tris_vm_user vmus, *vmu=NULL;
	char ddir[256];
	int res=0, cmd=0, x;

	if ((context = strchr(mailbox, '@')))
		*context++ = '\0';
	else
		context = "default";
	
	memset(&vms, 0, sizeof(vms));
	vms.lastmsg = -1;

	memset(&vmus, 0, sizeof(vmus));

	tris_copy_string(vms.username, mailbox, sizeof(vms.username));

	vmu = create_user(&vmus, context, vms.username);

	if (!(vms.deleted = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		tris_log(LOG_ERROR, "Could not allocate memory for deleted message storage!\n");
	}
	if (!(vms.heard = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}

	res = open_mailbox(&vms, vmu, folder);
	if (res == ERROR_LOCK_PATH)
		return -1;
	
	if(!strcasecmp(command, "HEARD") ) {
		/* Add the vm_state to the active list and keep it active */
		if(folder==NEW_FOLDER) {
			//for(x=0; x<msgcount; x++) {
			make_file(vms.fn, sizeof(vms.fn), vms.curdir, msglist[0]);
			vms.heard[msglist[0]] = 1;
			DISPOSE(vms.curdir, msglist[0]);

			create_dirpath(ddir, sizeof(ddir), context, mailbox, "OLD");
			x = last_message_index(vmu, ddir) + 1;
			if(x >= vmu->maxmsg)  x = vmu->maxmsg - 1;
			sprintf(result, "%s%s/%s/OLD/msg%04d.wav", VM_SPOOL_DIR, context, mailbox,x);
			//}
		} else{
			make_file(vms.fn, sizeof(vms.fn), vms.curdir, msglist[0]);
			sprintf(result, "%s.wav",vms.fn);
			DISPOSE(vms.curdir, msglist[0]);
		}

	} else if(!strcasecmp(command, "SAVED")) {

		//if(folder == DELETED_FOLDER) 
		//	return 1;
		for(x=0; x<msgcount; x++) {
			cmd = save_to_folder(vmu, &vms, msglist[x], SAVED_FOLDER);
			if (cmd == ERROR_LOCK_PATH) {
				res = cmd;
				goto mm_out;
			} else if(!cmd) {
				vms.deleted[msglist[x]] = 1;
			}

			make_file(vms.fn, sizeof(vms.fn), vms.curdir, msglist[x]);
		}
		
	} else if(!strcasecmp(command, "DELETED")) {

		for(x=0; x<msgcount; x++) {
			vms.deleted[msglist[x]] = 1;
		}

	}
	
mm_out:
	res = close_mailbox(&vms, vmu);
	if (res == ERROR_LOCK_PATH)
		return -1;

	return 0;
	
}

/* In Greek even though we CAN use a syntax like "friends messages"
 * ("filika mynhmata") it is not elegant. This also goes for "work/family messages"
 * ("ergasiaka/oikogeniaka mynhmata"). Therefore it is better to use a reversed 
 * syntax for the above three categories which is more elegant. 
 */

static int vm_play_folder_name_gr(struct tris_channel *chan, char *mbox)
{
	int cmd;
	char *buf;

	buf = alloca(strlen(mbox) + 2); 
	strcpy(buf, mbox);
	strcat(buf, "s");

	if (!strcasecmp(mbox, "voicemail/vm-INBOX") || !strcasecmp(mbox, "voicemail/vm-Old")) {
		cmd = tris_play_and_wait(chan, buf); /* "NEA / PALIA" */
		return cmd ? cmd : tris_play_and_wait(chan, "voicemail/vm-messages"); /* "messages" -> "MYNHMATA" */
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/vm-messages"); /* "messages" -> "MYNHMATA" */
		return cmd ? cmd : tris_play_and_wait(chan, mbox); /* friends/family/work... -> "FILWN"/"OIKOGENIAS"/"DOULEIAS"*/
	}
}

static int vm_play_folder_name_pl(struct tris_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(mbox, "voicemail/vm-INBOX") || !strcasecmp(mbox, "voicemail/vm-Old")) {
		if (!strcasecmp(mbox, "voicemail/vm-INBOX"))
			cmd = tris_play_and_wait(chan, "voicemail/vm-new-e");
		else
			cmd = tris_play_and_wait(chan, "voicemail/vm-old-e");
		return cmd ? cmd : tris_play_and_wait(chan, "voicemail/vm-messages");
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/vm-messages");
		return cmd ? cmd : tris_play_and_wait(chan, mbox);
	}
}

static int vm_play_folder_name_ua(struct tris_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(mbox, "voicemail/vm-Family") || !strcasecmp(mbox, "voicemail/vm-Friends") || !strcasecmp(mbox, "voicemail/vm-Work")) {
		cmd = tris_play_and_wait(chan, "voicemail/vm-messages");
		return cmd ? cmd : tris_play_and_wait(chan, mbox);
	} else {
		cmd = tris_play_and_wait(chan, mbox);
		return cmd ? cmd : tris_play_and_wait(chan, "voicemail/vm-messages");
	}
}

static int vm_play_folder_name(struct tris_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(chan->language, "it") || !strcasecmp(chan->language, "es") || !strcasecmp(chan->language, "pt") || !strcasecmp(chan->language, "pt_BR")) { /* Italian, Spanish, French or Portuguese syntax */
		cmd = tris_play_and_wait(chan, "voicemail/vm-messages"); /* "messages */
		return cmd ? cmd : tris_play_and_wait(chan, mbox);
	} else if (!strcasecmp(chan->language, "gr")) {
		return vm_play_folder_name_gr(chan, mbox);
	} else if (!strcasecmp(chan->language, "pl")) {
		return vm_play_folder_name_pl(chan, mbox);
	} else if (!strcasecmp(chan->language, "ua")) {  /* Ukrainian syntax */
		return vm_play_folder_name_ua(chan, mbox);
	} else {  /* Default English */
		cmd = tris_play_and_wait(chan, mbox);
		return cmd ? cmd : tris_play_and_wait(chan, "voicemail/vm-messages"); /* "messages */
	}
}

/* GREEK SYNTAX 
	In greek the plural for old/new is
	different so we need the following files
	We also need vm-denExeteMynhmata because 
	this syntax is different.
	
	-> vm-Olds.wav	: "Palia"
	-> vm-INBOXs.wav : "Nea"
	-> vm-denExeteMynhmata : "den exete mynhmata"
*/
					
	
static int vm_intro_gr(struct tris_channel *chan, struct vm_state *vms)
{
	int res = 0;

	if (vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-youhave");
		if (!res) 
			res = tris_say_number(chan, vms->newmessages, TRIS_DIGIT_ANY, chan->language, NULL);
		if (!res) {
			if ((vms->newmessages == 1)) {
				res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-message");
			} else {
				res = tris_play_and_wait(chan, "voicemail/vm-INBOXs");
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
	} else if (vms->oldmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-youhave");
		if (!res)
			res = tris_say_number(chan, vms->oldmessages, TRIS_DIGIT_ANY, chan->language, NULL);
		if ((vms->oldmessages == 1)) {
			res = tris_play_and_wait(chan, "voicemail/vm-Old");
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-message");
		} else {
			res = tris_play_and_wait(chan, "voicemail/vm-Olds");
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-messages");
		}
	} else if (!vms->oldmessages && !vms->newmessages) 
		res = tris_play_and_wait(chan, "voicemail/vm-denExeteMynhmata"); 
	return res;
}
	
/* Default English syntax */
static int vm_intro_en(struct tris_channel *chan, struct vm_state *vms)
{
	int res;
	char nextmsg[256];
	/* Introduce messages they have */

//	if (!vms->oldmessages && !vms->newmessages) {
//		res = tris_play_and_wait(chan, "voicemail/no_msgs_in_inbox");
//		return 't';
//	}
	
	if (vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/unheard_msg");
		snprintf(nextmsg,sizeof(nextmsg), "digits/piece-%d", vms->newmessages);
		if(!res)
			res = tris_play_and_wait(chan,nextmsg);
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-received");
	}
	else {
		res = tris_play_and_wait(chan, "voicemail/no_unheard_msg");
	}
/*	if (!res && vms->oldmessages) {
		res = say_and_wait(chan, vms->oldmessages, chan->language);
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-Old");
		if (!res) {
			if (vms->oldmessages == 1)
				res = tris_play_and_wait(chan, "voicemail/vm-message");
			else
				res = tris_play_and_wait(chan, "voicemail/vm-messages");
		}
	}
*/
	return res;
}

/* ITALIAN syntax */
static int vm_intro_it(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages)
		res =	tris_play_and_wait(chan, "voicemail/vm-no") ||
			tris_play_and_wait(chan, "voicemail/vm-message");
	else
		res =	tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res && vms->newmessages) {
		res = (vms->newmessages == 1) ?
			tris_play_and_wait(chan, "digits/un") ||
			tris_play_and_wait(chan, "voicemail/vm-nuovo") ||
			tris_play_and_wait(chan, "voicemail/vm-message") :
			/* 2 or more new messages */
			say_and_wait(chan, vms->newmessages, chan->language) ||
			tris_play_and_wait(chan, "voicemail/vm-nuovi") ||
			tris_play_and_wait(chan, "voicemail/vm-messages");
		if (!res && vms->oldmessages)
			res =	tris_play_and_wait(chan, "voicemail/vm-and");
	}
	if (!res && vms->oldmessages) {
		res = (vms->oldmessages == 1) ?
			tris_play_and_wait(chan, "digits/un") ||
			tris_play_and_wait(chan, "voicemail/vm-vecchio") ||
			tris_play_and_wait(chan, "voicemail/vm-message") :
			/* 2 or more old messages */
			say_and_wait(chan, vms->oldmessages, chan->language) ||
			tris_play_and_wait(chan, "voicemail/vm-vecchi") ||
			tris_play_and_wait(chan, "voicemail/vm-messages");
	}
	return res;
}

/* POLISH syntax */
static int vm_intro_pl(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	div_t num;

	if (!vms->oldmessages && !vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-no");
		res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		return res;
	} else {
		res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	}

	if (vms->newmessages) {
		num = div(vms->newmessages, 10);
		if (vms->newmessages == 1) {
			res = tris_play_and_wait(chan, "digits/1-a");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-new-a");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-message");
		} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
			if (num.rem == 2) {
				if (!num.quot) {
					res = tris_play_and_wait(chan, "digits/2-ie");
				} else {
					res = say_and_wait(chan, vms->newmessages - 2 , chan->language);
					res = res ? res : tris_play_and_wait(chan, "digits/2-ie");
				}
			} else {
				res = say_and_wait(chan, vms->newmessages, chan->language);
			}
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-new-e");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-new-ych");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		}
		if (!res && vms->oldmessages)
			res = tris_play_and_wait(chan, "voicemail/vm-and");
	}
	if (!res && vms->oldmessages) {
		num = div(vms->oldmessages, 10);
		if (vms->oldmessages == 1) {
			res = tris_play_and_wait(chan, "digits/1-a");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-old-a");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-message");
		} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
			if (num.rem == 2) {
				if (!num.quot) {
					res = tris_play_and_wait(chan, "digits/2-ie");
				} else {
					res = say_and_wait(chan, vms->oldmessages - 2 , chan->language);
					res = res ? res : tris_play_and_wait(chan, "digits/2-ie");
				}
			} else {
				res = say_and_wait(chan, vms->oldmessages, chan->language);
			}
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-old-e");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-old-ych");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		}
	}

	return res;
}

/* SWEDISH syntax */
static int vm_intro_se(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;

	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (res)
		return res;

	if (!vms->oldmessages && !vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-no");
		res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		return res;
	}

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = tris_play_and_wait(chan, "digits/ett");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-nytt");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-nya");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		}
		if (!res && vms->oldmessages)
			res = tris_play_and_wait(chan, "voicemail/vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = tris_play_and_wait(chan, "digits/ett");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-gammalt");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-gamla");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		}
	}

	return res;
}

/* NORWEGIAN syntax */
static int vm_intro_no(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;

	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (res)
		return res;

	if (!vms->oldmessages && !vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-no");
		res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		return res;
	}

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = tris_play_and_wait(chan, "digits/1");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-ny");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-nye");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		}
		if (!res && vms->oldmessages)
			res = tris_play_and_wait(chan, "voicemail/vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = tris_play_and_wait(chan, "digits/1");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-gamel");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-gamle");
			res = res ? res : tris_play_and_wait(chan, "voicemail/vm-messages");
		}
	}

	return res;
}

/* GERMAN syntax */
static int vm_intro_de(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if ((vms->newmessages == 1))
				res = tris_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
			if (vms->oldmessages && !res)
				res = tris_play_and_wait(chan, "voicemail/vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = tris_play_and_wait(chan, "voicemail/vm-message");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			if (vms->oldmessages == 1)
				res = tris_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = tris_play_and_wait(chan, "voicemail/vm-message");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = tris_play_and_wait(chan, "voicemail/vm-no");
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
	}
	return res;
}

/* SPANISH syntax */
static int vm_intro_es(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-youhaveno");
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-messages");
	} else {
		res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	}
	if (!res) {
		if (vms->newmessages) {
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = tris_play_and_wait(chan, "digits/1M");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-message");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-INBOXs");
				} else {
					res = say_and_wait(chan, vms->newmessages, chan->language);
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-messages");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = tris_play_and_wait(chan, "voicemail/vm-and");
		}
		if (vms->oldmessages) {
			if (!res) {
				if (vms->oldmessages == 1) {
					res = tris_play_and_wait(chan, "digits/1M");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-message");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-Olds");
				} else {
					res = say_and_wait(chan, vms->oldmessages, chan->language);
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-messages");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-Old");
				}
			}
		}
	}
return res;
}

/* BRAZILIAN PORTUGUESE syntax */
static int vm_intro_pt_BR(struct tris_channel *chan, struct vm_state *vms) {
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-nomessages");
		return res;
	} else {
		res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	}
	if (vms->newmessages) {
		if (!res)
			res = tris_say_number(chan, vms->newmessages, TRIS_DIGIT_ANY, chan->language, "f");
		if ((vms->newmessages == 1)) {
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-message");
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-INBOXs");
		} else {
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-messages");
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
		}
		if (vms->oldmessages && !res)
			res = tris_play_and_wait(chan, "voicemail/vm-and");
	}
	if (vms->oldmessages) {
		if (!res)
			res = tris_say_number(chan, vms->oldmessages, TRIS_DIGIT_ANY, chan->language, "f");
		if (vms->oldmessages == 1) {
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-message");
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-Olds");
		} else {
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-messages");
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-Old");
		}
	}
	return res;
}

/* FRENCH syntax */
static int vm_intro_fr(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
			if (vms->oldmessages && !res)
				res = tris_play_and_wait(chan, "voicemail/vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = tris_play_and_wait(chan, "voicemail/vm-message");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = tris_play_and_wait(chan, "voicemail/vm-message");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = tris_play_and_wait(chan, "voicemail/vm-no");
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
	}
	return res;
}

/* DUTCH syntax */
static int vm_intro_nl(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res) {
				if (vms->newmessages == 1)
					res = tris_play_and_wait(chan, "voicemail/vm-INBOXs");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
			}
			if (vms->oldmessages && !res)
				res = tris_play_and_wait(chan, "voicemail/vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = tris_play_and_wait(chan, "voicemail/vm-message");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if (vms->oldmessages == 1)
					res = tris_play_and_wait(chan, "voicemail/vm-Olds");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-Old");
			}
			if (!res) {
				if (vms->oldmessages == 1)
					res = tris_play_and_wait(chan, "voicemail/vm-message");
				else
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = tris_play_and_wait(chan, "voicemail/vm-no");
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
	}
	return res;
}

/* PORTUGUESE syntax */
static int vm_intro_pt(struct tris_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = tris_say_number(chan, vms->newmessages, TRIS_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = tris_play_and_wait(chan, "voicemail/vm-message");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-INBOXs");
				} else {
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = tris_play_and_wait(chan, "voicemail/vm-and");
		}
		if (!res && vms->oldmessages) {
			res = tris_say_number(chan, vms->oldmessages, TRIS_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if (vms->oldmessages == 1) {
					res = tris_play_and_wait(chan, "voicemail/vm-message");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-Olds");
				} else {
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-Old");
				}
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = tris_play_and_wait(chan, "voicemail/vm-no");
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-messages");
			}
		}
	}
	return res;
}


/* CZECH syntax */
/* in czech there must be declension of word new and message
 * czech        : english        : czech      : english
 * --------------------------------------------------------
 * vm-youhave   : you have 
 * vm-novou     : one new        : vm-zpravu  : message
 * vm-nove      : 2-4 new        : vm-zpravy  : messages
 * vm-novych    : 5-infinite new : vm-zprav   : messages
 * vm-starou	: one old
 * vm-stare     : 2-4 old 
 * vm-starych   : 5-infinite old
 * jednu        : one	- falling 4. 
 * vm-no        : no  ( no messages )
 */

static int vm_intro_cz(struct tris_channel *chan, struct vm_state *vms)
{
	int res;
	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if (vms->newmessages == 1) {
				res = tris_play_and_wait(chan, "digits/jednu");
			} else {
				res = say_and_wait(chan, vms->newmessages, chan->language);
			}
			if (!res) {
				if ((vms->newmessages == 1))
					res = tris_play_and_wait(chan, "voicemail/vm-novou");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = tris_play_and_wait(chan, "voicemail/vm-nove");
				if (vms->newmessages > 4)
					res = tris_play_and_wait(chan, "voicemail/vm-novych");
			}
			if (vms->oldmessages && !res)
				res = tris_play_and_wait(chan, "voicemail/vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = tris_play_and_wait(chan, "voicemail/vm-zpravu");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = tris_play_and_wait(chan, "voicemail/vm-zpravy");
				if (vms->newmessages > 4)
					res = tris_play_and_wait(chan, "voicemail/vm-zprav");
			}
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if ((vms->oldmessages == 1))
					res = tris_play_and_wait(chan, "voicemail/vm-starou");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = tris_play_and_wait(chan, "voicemail/vm-stare");
				if (vms->oldmessages > 4)
					res = tris_play_and_wait(chan, "voicemail/vm-starych");
			}
			if (!res) {
				if ((vms->oldmessages == 1))
					res = tris_play_and_wait(chan, "voicemail/vm-zpravu");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = tris_play_and_wait(chan, "voicemail/vm-zpravy");
				if (vms->oldmessages > 4)
					res = tris_play_and_wait(chan, "voicemail/vm-zprav");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = tris_play_and_wait(chan, "voicemail/vm-no");
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-zpravy");
			}
		}
	}
	return res;
}

static int get_lastdigits(int num)
{
	num %= 100;
	return (num < 20) ? num : num % 10;
}

static int vm_intro_ru(struct tris_channel *chan, struct vm_state *vms)
{
	int res;
	int lastnum = 0;
	int dcnum;

	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res && vms->newmessages) {
		lastnum = get_lastdigits(vms->newmessages);
		dcnum = vms->newmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = tris_play_and_wait(chan, "digits/odno");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = tris_play_and_wait(chan, (lastnum == 1) ? "voicemail/vm-novoe" : "voicemail/vm-novyh");

		if (!res && vms->oldmessages)
			res = tris_play_and_wait(chan, "voicemail/vm-and");
	}

	if (!res && vms->oldmessages) {
		lastnum = get_lastdigits(vms->oldmessages);
		dcnum = vms->oldmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = tris_play_and_wait(chan, "digits/odno");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = tris_play_and_wait(chan, (lastnum == 1) ? "voicemail/vm-staroe" : "voicemail/vm-staryh");
	}

	if (!res && !vms->newmessages && !vms->oldmessages) {
		lastnum = 0;
		res = tris_play_and_wait(chan, "voicemail/vm-no");
	}

	if (!res) {
		switch (lastnum) {
		case 1:
			res = tris_play_and_wait(chan, "voicemail/vm-soobshenie");
			break;
		case 2:
		case 3:
		case 4:
			res = tris_play_and_wait(chan, "voicemail/vm-soobsheniya");
			break;
		default:
			res = tris_play_and_wait(chan, "voicemail/vm-soobsheniy");
			break;
		}
	}

	return res;
}

/* CHINESE (Taiwan) syntax */
static int vm_intro_tw(struct tris_channel *chan, struct vm_state *vms)
{
	int res;
	/* Introduce messages they have */
	res = tris_play_and_wait(chan, "voicemail/vm-you");

	if (!res && vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-have");
		if (!res)
			res = say_and_wait(chan, vms->newmessages, chan->language);
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-tong");
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-INBOX");
		if (vms->oldmessages && !res)
			res = tris_play_and_wait(chan, "voicemail/vm-and");
		else if (!res) 
			res = tris_play_and_wait(chan, "voicemail/vm-messages");
	}
	if (!res && vms->oldmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-have");
		if (!res)
			res = say_and_wait(chan, vms->oldmessages, chan->language);
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-tong");
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-Old");
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-messages");
	}
	if (!res && !vms->oldmessages && !vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/vm-haveno");
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-messages");
	}
	return res;
}

/* UKRAINIAN syntax */
/* in ukrainian the syntax is different so we need the following files
 * --------------------------------------------------------
 * /digits/ua/1e 'odne'
 * vm-nove       'nove'
 * vm-stare      'stare'
 */
static int vm_intro_ua(struct tris_channel *chan, struct vm_state *vms)
{
	int res;
	int lastnum = 0;
	int dcnum;

	res = tris_play_and_wait(chan, "voicemail/vm-youhave");
	if (!res && vms->newmessages) {
		lastnum = get_lastdigits(vms->newmessages);
		dcnum = vms->newmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = tris_play_and_wait(chan, "digits/ua/1e");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = tris_play_and_wait(chan, (lastnum == 1) ? "voicemail/vm-nove" : "voicemail/vm-INBOX");

		if (!res && vms->oldmessages)
			res = tris_play_and_wait(chan, "voicemail/vm-and");
	}

	if (!res && vms->oldmessages) {
		lastnum = get_lastdigits(vms->oldmessages);
		dcnum = vms->oldmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = tris_play_and_wait(chan, "digits/ua/1e");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = tris_play_and_wait(chan, (lastnum == 1) ? "voicemail/vm-stare" : "voicemail/vm-Old");
	}

	if (!res && !vms->newmessages && !vms->oldmessages) {
		lastnum = 0;
		res = tris_play_and_wait(chan, "voicemail/vm-no");
	}

	if (!res) {
		switch (lastnum) {
		case 1:
		case 2:
		case 3:
		case 4:
			res = tris_play_and_wait(chan, "voicemail/vm-message");
			break;
		default:
			res = tris_play_and_wait(chan, "voicemail/vm-messages");
			break;
		}
	}

	return res;
}

static int vm_intro(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms)
{
	char prefile[256];
	
	/* Notify the user that the temp greeting is set and give them the option to remove it */
	snprintf(prefile, sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
	if (tris_test_flag(vmu, VM_TEMPGREETWARN)) {
		RETRIEVE(prefile, -1, vmu->mailbox, vmu->context);
		if (tris_fileexists(prefile, NULL, NULL) > 0) {
			tris_play_and_wait(chan, "voicemail/vm-tempgreetactive");
		}
		DISPOSE(prefile, -1);
	}

	/* Play voicemail intro - syntax is different for different languages */
	if (!strcasecmp(chan->language, "de")) {	/* GERMAN syntax */
		return vm_intro_de(chan, vms);
	} else if (!strcasecmp(chan->language, "es")) { /* SPANISH syntax */
		return vm_intro_es(chan, vms);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN syntax */
		return vm_intro_it(chan, vms);
	} else if (!strcasecmp(chan->language, "fr")) {	/* FRENCH syntax */
		return vm_intro_fr(chan, vms);
	} else if (!strcasecmp(chan->language, "nl")) {	/* DUTCH syntax */
		return vm_intro_nl(chan, vms);
	} else if (!strcasecmp(chan->language, "pt")) {	/* PORTUGUESE syntax */
		return vm_intro_pt(chan, vms);
	} else if (!strcasecmp(chan->language, "pt_BR")) {	/* BRAZILIAN PORTUGUESE syntax */
		return vm_intro_pt_BR(chan, vms);
	} else if (!strcasecmp(chan->language, "cz")) {	/* CZECH syntax */
		return vm_intro_cz(chan, vms);
	} else if (!strcasecmp(chan->language, "gr")) {	/* GREEK syntax */
		return vm_intro_gr(chan, vms);
	} else if (!strcasecmp(chan->language, "pl")) {	/* POLISH syntax */
		return vm_intro_pl(chan, vms);
	} else if (!strcasecmp(chan->language, "se")) {	/* SWEDISH syntax */
		return vm_intro_se(chan, vms);
	} else if (!strcasecmp(chan->language, "no")) {	/* NORWEGIAN syntax */
		return vm_intro_no(chan, vms);
	} else if (!strcasecmp(chan->language, "ru")) { /* RUSSIAN syntax */
		return vm_intro_ru(chan, vms);
	} else if (!strcasecmp(chan->language, "tw")) { /* CHINESE (Taiwan) syntax */
		return vm_intro_tw(chan, vms);
	} else if (!strcasecmp(chan->language, "ua")) { /* UKRAINIAN syntax */
		return vm_intro_ua(chan, vms);
	} else {					/* Default to ENGLISH */
		return vm_intro_en(chan, vms);
	}
}

static int vm_instructions_en(struct tris_channel *chan, struct vm_state *vms, int skipadvanced)
{
	int res = 0;
	int repeats = 0;
	/* Play instructions and wait for new command */
	while (!res) {
		if (vms->starting) {
			res = -1;
		} else {
			char filename[256];
			if (!strcasecmp(vms->curbox, "DELETED"))
				strcpy(filename, "voicemail/msg_listen_options_deleted");
			else if (!strcasecmp(vms->curbox, "SAVED"))
				strcpy(filename, "voicemail/msg_listen_options_no_save");
			else
				strcpy(filename, "voicemail/msg_listen_options");
			
			res = tris_play_and_wait(chan, filename);
			if (!res)
				res = tris_waitfordigit(chan, 6000);
			if (!res) {
				repeats++;
				if (repeats > 2) {
					res = 't';
				}
			}
	
		}
	}
	return res;
}

static int vm_instructions_tw(struct tris_channel *chan, struct vm_state *vms, int skipadvanced)
{
	int res = 0;
	/* Play instructions and wait for new command */
	while (!res) {
		if (vms->lastmsg > -1) {
			res = tris_play_and_wait(chan, "voicemail/vm-listen");
			if (!res)
				res = vm_play_folder_name(chan, vms->vmbox);
			if (!res)
				res = tris_play_and_wait(chan, "press");
			if (!res)
				res = tris_play_and_wait(chan, "digits/1");
		}
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-opts");
		if (!res) {
			vms->starting = 0;
			return vm_instructions_en(chan, vms, skipadvanced);
		}
	}
	return res;
}

static int vm_instructions(struct tris_channel *chan, struct vm_state *vms, int skipadvanced)
{
	if (vms->starting && !strcasecmp(chan->language, "tw")) { /* CHINESE (Taiwan) syntax */
		return vm_instructions_tw(chan, vms, skipadvanced);
	} else {					/* Default to ENGLISH */
		return vm_instructions_en(chan, vms, skipadvanced);
	}
}

static int vm_tempgreeting(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	char prefile[PATH_MAX] = "";
	unsigned char buf[256];
	int bytes = 0;

	if (tris_adsi_available(chan)) {
		bytes += adsi_logo(buf + bytes);
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Temp Greeting Menu", "");
		bytes += tris_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += tris_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += tris_adsi_voice_mode(buf + bytes, 0);
		tris_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	snprintf(prefile, sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
	while ((cmd >= 0) && (cmd != 't')) {
		if (cmd)
			retries = 0;
		RETRIEVE(prefile, -1, vmu->mailbox, vmu->context);
		if (tris_fileexists(prefile, NULL, NULL) <= 0) {
			play_record_review(chan, "voicemail/vm-rec-temp", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			cmd = 't';	
		} else {
			switch (cmd) {
			case '1':
				cmd = play_record_review(chan, "voicemail/vm-rec-temp", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
				break;
			case '2':
				DELETE(prefile, -1, prefile, vmu);
				tris_play_and_wait(chan, "voicemail/vm-tempremoved");
				cmd = 't';	
				break;
			case '*': 
				cmd = 't';
				break;
			default:
				cmd = tris_play_and_wait(chan,
					tris_fileexists(prefile, NULL, NULL) > 0 ? /* XXX always true ? */
						"voicemail/vm-tempgreeting2" : "voicemail/vm-tempgreeting");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 6000);
				if (!cmd)
					retries++;
				if (retries > 3)
					cmd = 't';
			}
		}
		DISPOSE(prefile, -1);
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

/* GREEK SYNTAX */
	
static int vm_browse_messages_gr(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/vm-youhaveno");
		if (!strcasecmp(vms->vmbox, "voicemail/vm-INBOX") ||!strcasecmp(vms->vmbox, "voicemail/vm-Old")) {
			if (!cmd) {
				snprintf(vms->fn, sizeof(vms->fn), "voicemail/vm-%ss", vms->curbox);
				cmd = tris_play_and_wait(chan, vms->fn);
			}
			if (!cmd)
				cmd = tris_play_and_wait(chan, "voicemail/vm-messages");
		} else {
			if (!cmd)
				cmd = tris_play_and_wait(chan, "voicemail/vm-messages");
			if (!cmd) {
				snprintf(vms->fn, sizeof(vms->fn), "voicemail/vm-%s", vms->curbox);
				cmd = tris_play_and_wait(chan, vms->fn);
			}
		}
	} 
	return cmd;
}

/* Default English syntax */
static int vm_browse_messages_en(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/end_of_messages");
		cmd = '0'; // goto MainMenu
	}
	return cmd;
}

/* ITALIAN syntax */
static int vm_browse_messages_it(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/vm-no");
		if (!cmd)
			cmd = tris_play_and_wait(chan, "voicemail/vm-message");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "voicemail/vm-%s", vms->curbox);
			cmd = tris_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

/* SPANISH syntax */
static int vm_browse_messages_es(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/vm-youhaveno");
		if (!cmd)
			cmd = tris_play_and_wait(chan, "voicemail/vm-messages");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "voicemail/vm-%s", vms->curbox);
			cmd = tris_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

/* PORTUGUESE syntax */
static int vm_browse_messages_pt(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/vm-no");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "voicemail/vm-%s", vms->curbox);
			cmd = tris_play_and_wait(chan, vms->fn);
		}
		if (!cmd)
			cmd = tris_play_and_wait(chan, "voicemail/vm-messages");
	}
	return cmd;
}

/* Chinese (Taiwan)syntax */
static int vm_browse_messages_tw(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/vm-you");
		if (!cmd) 
			cmd = tris_play_and_wait(chan, "voicemail/vm-haveno");
		if (!cmd)
			cmd = tris_play_and_wait(chan, "voicemail/vm-messages");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "voicemail/vm-%s", vms->curbox);
			cmd = tris_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

static int vm_browse_messages(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	if (!strcasecmp(chan->language, "es")) {	/* SPANISH */
		return vm_browse_messages_es(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN */
		return vm_browse_messages_it(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "pt") || !strcasecmp(chan->language, "pt_BR")) {	/* PORTUGUESE */
		return vm_browse_messages_pt(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "gr")) {
		return vm_browse_messages_gr(chan, vms, vmu);   /* GREEK */
	} else if (!strcasecmp(chan->language, "tw")) {
		return vm_browse_messages_tw(chan, vms, vmu);   /* CHINESE (Taiwan) */
	} else {	/* Default to English syntax */
		return vm_browse_messages_en(chan, vms, vmu);
	}
}

static int play_message_withinfo(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms)
{
	int cmd = 0;
	cmd = advanced_options(chan, vmu, vms, vms->curmsg, 3, 0);
	cmd = play_message(chan, vmu, vms);
	return cmd;
}

static int cmd_browse_messages(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message_withinfo(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/cmd_end_of_messages");
		cmd = 't'; // The End
	}
	return cmd;
}

static int rprt_browse_messages(struct tris_channel *chan, struct vm_state *vms, struct tris_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message_withinfo(chan, vmu, vms);
	} else {
		cmd = tris_play_and_wait(chan, "voicemail/rprt_no_report_msg");
		cmd = '*'; // The End
	}
	return cmd;
}

static int vm_authenticate(struct tris_channel *chan, char *mailbox, int mailbox_size,
			struct tris_vm_user *res_vmu, const char *context, const char *prefix,
			int *skipuser, int maxlogins, int silent)
{
	int useadsi = 0, valid = 0, logretries = 0, i;
	char password[TRIS_MAX_EXTENSION] = "";
	struct tris_vm_user vmus, *vmu = NULL;

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);
	if (!(*skipuser) && useadsi)
		adsi_login(chan);

	password[1] = '\0';
	if (!silent && !(*skipuser)) {
		password[0] = '#';
	}
	else {
		for(i=0; i<=3; i++) {
			password[0] = tris_play_and_wait(chan, "voicemail/vm-login");
			
			if(!password[0])
				password[0] = tris_waitfordigit(chan, 5000);
			if(password[0])
				break;
		}
		if(!password[0]) {
			tris_stopstream(chan);
			tris_play_and_wait(chan, "goodbye");
			return -1;
		}
	}
	
	/* Authenticate them and get their mailbox/password */
	if(password[0] == '#') {
		if (tris_streamfile(chan, "voicemail/dial_extn_pound", chan->language)) {
			tris_log(LOG_WARNING, "Unable to stream dial_extn_pound file\n");
			return -1;
		}
		mailbox[0] = '\0';
		*skipuser = 0;
	} 
	
	while (!(*skipuser) && (logretries < maxlogins)) {
		if(tris_readstring(chan, mailbox+strlen(mailbox), mailbox_size - 1, 2000, 10000, "#") < 0) {
			tris_log(LOG_WARNING, "Couldn't read username\n");
			return -1;
		}
		

		if (!tris_strlen_zero(prefix)) {
			char fullusername[80] = "";
			tris_copy_string(fullusername, prefix, sizeof(fullusername));
			strncat(fullusername, mailbox, sizeof(fullusername) - 1 - strlen(fullusername));
			tris_copy_string(mailbox, fullusername, mailbox_size);
		}

		tris_debug(1, "Before find user for mailbox %s\n", mailbox);
		if(vm_user_exist(mailbox)) {
			vmu = create_user(&vmus, context, mailbox);
			password[1] = '\0';
			password[0]=tris_play_and_wait(chan, "voicemail/enter_pin");
			logretries = -1;
			break;
		}
		
		logretries++;
		if (!(*skipuser)){
			if(!tris_strlen_zero(mailbox)){
				mailbox[1] = '\0';
				mailbox[0]=tris_play_and_wait(chan, "voicemail/is_not_found");
			}
			
			if(logretries >= maxlogins) {
				break;
			}
			else if(!mailbox[0]){
				mailbox[1] = '\0';
				mailbox[0]=tris_play_and_wait(chan, "voicemail/dial_extn_pound");
			}
			if (tris_waitstream(chan, ""))	/* Channel is hung up */
				return -1;
		}
			
	}

	while (!valid && (logretries < maxlogins)) {
		if (password[0] != '#') {
			if (tris_readstring(chan, password+strlen(password), sizeof(password) - 1, 2000, 10000, "#") < 0) {
				tris_log(LOG_WARNING, "Unable to read password\n");
				return -1;
			}
		}
		else {
			password[0] = '\0';
		}

		if (vm_login(mailbox, password))
			valid++;
		
		logretries++;
		if (!valid) {
			if(!tris_strlen_zero(password)) {
				password[1] = '\0';
				password[0]=tris_play_and_wait(chan, "voicemail/invalid_pin");
			}
			
			if (logretries >= maxlogins) {
				break;
			} else if(!password[0]){
				password[1] = '\0';
				password[0]=tris_play_and_wait(chan, "voicemail/enter_pin");
			}
			if (tris_waitstream(chan, ""))	/* Channel is hung up */
				return -1;
		}
	}
	
	if (!valid && (logretries >= maxlogins)) {
		tris_stopstream(chan);
		tris_play_and_wait(chan, "goodbye");
		return -1;
	}
	if (vmu && !(*skipuser)) {
		memcpy(res_vmu, vmu, sizeof(struct tris_vm_user));
	}
	return 0;
}

static int vm_execmain(struct tris_channel *chan, void *data)
{
	/* XXX This is, admittedly, some pretty horrendous code.  For some
	   reason it just seemed a lot easier to do with GOTO's.  I feel
	   like I'm back in my GWBASIC days. XXX */
	int res = -1;
	int cmd = 0;
	int valid = 0;
	char prefixstr[80] = "";
	char ext_context[256] = "";
	int box;
	int useadsi = 0;
	int skipuser = 0;
	struct vm_state I_vms, O_vms;
	struct vm_state *vms =&I_vms;
	struct tris_vm_user *vmu = NULL, vmus;
	char *context = NULL;
	int silentexit = 0;
	struct tris_flags flags = { 0 };
	signed char record_gain = 0;
	int play_auto = 0;
	int play_folder = 0;
#ifdef IMAP_STORAGE
	int deleted = 0;
#endif
	char sql[256], exten[80];	

	/* Add the vm_state to the active list and keep it active */
	memset(&I_vms, 0, sizeof(I_vms));
	I_vms.lastmsg = -1;

	memset(&O_vms, 0, sizeof(O_vms));
	O_vms.lastmsg = -1;
	memset(&vmus, 0, sizeof(vmus));

	if (chan->_state != TRIS_STATE_UP) {
		tris_debug(1, "Before tris_answer\n");
		tris_answer(chan);
	}


	if (!tris_strlen_zero(data)) {
		char *opts[OPT_ARG_ARRAY_SIZE];
		char *parse;
		TRIS_DECLARE_APP_ARGS(args,
			TRIS_APP_ARG(argv0);
			TRIS_APP_ARG(argv1);
		);

		parse = tris_strdupa(data);

		TRIS_STANDARD_APP_ARGS(args, parse);

		if (args.argc == 2) {
			if (tris_app_parse_options(vm_app_options, &flags, opts, args.argv1))
				return -1;
			if (tris_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;
				if (!tris_strlen_zero(opts[OPT_ARG_RECORDGAIN])) {
					if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
						tris_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
						return -1;
					} else {
						record_gain = (signed char) gain;
					}
				} else {
					tris_log(LOG_WARNING, "Invalid Gain level set with option g\n");
				}
			}
			if (tris_test_flag(&flags, OPT_AUTOPLAY) ) {
				play_auto = 1;
				if (opts[OPT_ARG_PLAYFOLDER]) {
					if (sscanf(opts[OPT_ARG_PLAYFOLDER], "%d", &play_folder) != 1) {
						tris_log(LOG_WARNING, "Invalid value '%s' provided for folder autoplay option\n", opts[OPT_ARG_PLAYFOLDER]);
					}
				} else {
					tris_log(LOG_WARNING, "Invalid folder set with option a\n");
				}	
				if (play_folder > 9 || play_folder < 0) {
					tris_log(LOG_WARNING, "Invalid value '%d' provided for folder autoplay option\n", play_folder);
					play_folder = 0;
				}
			}
		} else {
			/* old style options parsing */
			while (*(args.argv0)) {
				if (*(args.argv0) == 's')
					tris_set_flag(&flags, OPT_SILENT);
				else if (*(args.argv0) == 'p')
					tris_set_flag(&flags, OPT_PREPEND_MAILBOX);
				else 
					break;
				(args.argv0)++;
			}

		}

		valid = tris_test_flag(&flags, OPT_SILENT);

		if ((context = strchr(args.argv0, '@')))
			*context++ = '\0';

		if (tris_test_flag(&flags, OPT_PREPEND_MAILBOX))
			tris_copy_string(prefixstr, args.argv0, sizeof(prefixstr));
		else {
			if(vm_user_exist(args.argv0)) {
				
				snprintf(sql, sizeof(sql), "SELECT extension FROM uri WHERE username='%s'", args.argv0);
				sql_select_query_execute(exten, sql);
				if(!tris_strlen_zero(exten) && strcmp(exten,args.argv0)) {
					strcpy(args.argv0, exten);
				}

				tris_copy_string(I_vms.username, args.argv0, sizeof(I_vms.username));
				tris_copy_string(O_vms.username, args.argv0, sizeof(O_vms.username));
			} else
				I_vms.username[0] = '\0';
		}

		if (!tris_strlen_zero(I_vms.username) && (vmu = create_user(&vmus, context, I_vms.username))) 
			skipuser++;
		else
			valid = 0;
	}

	if (!valid)
		res = vm_authenticate(chan, I_vms.username, sizeof(I_vms.username), &vmus, context, prefixstr, &skipuser, maxlogins, 0);

	tris_debug(1, "After vm_authenticate\n");
	if (!res) {
		valid = 1;
		if (!skipuser) {
			vmu = &vmus;
			tris_copy_string(O_vms.username, I_vms.username, sizeof(O_vms.username));
		}
	} else {
		res = 0;
	}

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);

/*#ifdef IMAP_STORAGE
	vms.interactive = 1;
	vms.updated = 1;
	vmstate_insert(&vms);
	init_vm_state(&vms);
#endif */
	if (!valid)
		goto out;

	if (!(I_vms.deleted = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		tris_log(LOG_ERROR, "Could not allocate memory for deleted message storage!\n");
		cmd = tris_play_and_wait(chan, "an-error-has-occured");
	}
	if (!(O_vms.deleted = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	if (!(I_vms.heard = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	if (!(O_vms.heard = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	
	/* Set language from config to override channel language */
//	if (!tris_strlen_zero(vmu->language))
//		tris_string_field_set(chan, language, vmu->language);
	/* Retrieve old and new message counts */
	tris_debug(1, "Before open_mailbox\n");
	
	res = open_mailbox(&O_vms, vmu, OLD_FOLDER);
	if (res == ERROR_LOCK_PATH)
		goto out;
	res = open_mailbox(&I_vms, vmu, NEW_FOLDER);
	if (res == ERROR_LOCK_PATH)
		goto out;
	
	I_vms.oldmessages = O_vms.lastmsg + 1;
	O_vms.oldmessages = O_vms.lastmsg + 1;
	I_vms.newmessages = I_vms.lastmsg + 1;
	O_vms.newmessages = I_vms.lastmsg + 1;
	tris_debug(1, "Number of new messages: %d\n", I_vms.newmessages);
		
	/* Select proper mailbox FIRST!! */
	if (play_auto) {
		res = open_mailbox(&O_vms, vmu, play_folder);
		if (res == ERROR_LOCK_PATH)
			goto out;

		/* If there are no new messages, inform the user and hangup */
		if (O_vms.lastmsg == -1) {
			cmd = vm_browse_messages(chan, &O_vms, vmu);
			res = 0;
			goto out;
		}
		vms = &O_vms;
	} else {
		if (!I_vms.newmessages && I_vms.oldmessages) {
			/* If we only have old messages start here */
			vms = &O_vms;
			play_folder = OLD_FOLDER;
			if (res == ERROR_LOCK_PATH)
				goto out;
		}
	}

	if (useadsi)
		adsi_status(chan, vms);
	res = 0;

	/* Check to see if this is a new user */

/*#ifdef IMAP_STORAGE
		tris_debug(3, "Checking quotas: comparing %u to %u\n", vms.quota_usage, vms.quota_limit);
		if (vms.quota_limit && vms.quota_usage >= vms.quota_limit) {
			tris_debug(1, "*** QUOTA EXCEEDED!!\n");
			cmd = tris_play_and_wait(chan, "voicemail/vm-mailboxfull");
		}
		tris_debug(3, "Checking quotas: User has %d messages and limit is %d.\n", (vms.newmessages + vms.oldmessages), vmu->maxmsg);
		if ((vms.newmessages + vms.oldmessages) >= vmu->maxmsg) {
			tris_log(LOG_WARNING, "No more messages possible.  User has %d messages and limit is %d.\n", (vms.newmessages + vms.oldmessages), vmu->maxmsg);
			cmd = tris_play_and_wait(chan, "voicemail/vm-mailboxfull");
		}
#endif*/
	if (play_auto) {
		cmd = '1';
	} else {

		if(!count_all_msgs(vms, vmu)) {
			res = tris_play_and_wait(chan, "voicemail/no_msgs_in_inbox");
			cmd = 't';
		} else {
			cmd = vm_intro(chan, vmu, vms);
			if(cmd != 't')
				cmd = '0'; //goto MainMenu
		}
	}

	//vms->repeats = 0;
	vms->starting = 1;
	while ((cmd > -1) && (cmd != 't')) {
		/* Run main menu */
		switch (cmd) {
		case '1':
			if (vms->lastmsg > -1 && !vms->starting) {
				cmd = advanced_options(chan, vmu, vms, vms->curmsg, 3, record_gain);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
				}
				cmd = 0;
				break;
			} else
				vms->curmsg=0;
			/* Fall through */
		case '2':
			if(vms->lastmsg < 0 && play_folder == NEW_FOLDER){
				vms = &O_vms;
				play_folder = OLD_FOLDER;
				vms->curmsg = 0;
				vms->starting = 1;
			}
			cmd = vm_browse_messages(chan, vms, vmu);
			break;

//		case '2': /* Change folders */
/*			if (useadsi)
				adsi_folders(chan, 0, "Change to folder...");
			cmd = get_folder2(chan, "voicemail/vm-changeto", 0);
			if (cmd == '#') {
				cmd = 0;
			} else if (cmd > 0) {
				cmd = cmd - '0';
				res = close_mailbox(&vms, vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
				res = open_mailbox(&vms, vmu, cmd);
				if (res == ERROR_LOCK_PATH)
					goto out;
				play_folder = cmd;
				cmd = 0;
			}
			if (useadsi)
				adsi_status2(chan, &vms);
				
			if (!cmd)
				cmd = vm_play_folder_name(chan, vms.vmbox);

			vms.starting = 1;
			break;
*/
		case '3': /* save*/
			if (play_folder == SAVED_FOLDER ) {
				cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
				break;
			}
			
			if (vms->curmsg < 0 || vms->curmsg > vms->lastmsg) {
				/* No message selected */
				cmd = 0;
				break;
			}

			box = SAVED_FOLDER;	/* Shut up compiler */
			if(!vms->deleted[vms->curmsg])
				cmd = save_to_folder(vmu, vms, vms->curmsg, box);
			if (cmd == ERROR_LOCK_PATH) {
				res = cmd;
				goto out;
#ifndef IMAP_STORAGE
			} else if (!cmd) {
				vms->deleted[vms->curmsg] = 2;  // if set 2, msg is remove completely
#endif
			} else {
				vms->deleted[vms->curmsg] = 0;
				vms->heard[vms->curmsg] = 0;
			}

			cmd = tris_play_and_wait(chan, "voicemail/msg_saved");
			
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
			if (useadsi)
				adsi_message(chan, vms);

			cmd = '#';

			break;
		case '*':
			if (vms->curmsg) {
				vms->curmsg--;
				if(vms->deleted[vms->curmsg])
					break;
				cmd = play_message(chan, vmu, vms);
			} 
			else {
				if(play_folder == OLD_FOLDER && I_vms.lastmsg > -1) {
					vms = &I_vms;
					play_folder = NEW_FOLDER;
					vms->curmsg = I_vms.lastmsg;
					cmd = '2';
					vms->starting = 1;
				}
				else
					cmd = tris_play_and_wait(chan, "voicemail/nomore_before_msg");
			}
			break;
		case '#':
			if (vms->curmsg < vms->lastmsg && vms->curmsg < vmu->maxmsg) {
				vms->curmsg++;
				if(vms->deleted[vms->curmsg])
					break;
				cmd = play_message(chan, vmu, vms);
			} else {
				if(play_folder == NEW_FOLDER && O_vms.lastmsg > -1) {
					vms = &O_vms;
					play_folder = OLD_FOLDER;
					vms->curmsg = 0;
					cmd = '2';
					vms->starting = 1;
				}
				else{
					cmd = tris_stream_and_wait(chan, "voicemail/nomore_after_msg", "");
					cmd ='0';
				}
			}
			break;
		case '4':
			if (vms->curmsg >= 0 && vms->curmsg <= vms->lastmsg && vms->curmsg < vmu->maxmsg) {
				vms->deleted[vms->curmsg] = !vms->deleted[vms->curmsg];
				if (useadsi)
					adsi_delete(chan, vms);
				if (vms->deleted[vms->curmsg]) {
					if (play_folder == NEW_FOLDER)
						vms->newmessages--;
					else if (play_folder == OLD_FOLDER)
						vms->oldmessages--;
					if(play_folder == DELETED_FOLDER)
						cmd = tris_play_and_wait(chan, "voicemail/msg_deleted_forever");
					else
						cmd = tris_play_and_wait(chan, "voicemail/msg_deleted");
				} else {
					if (play_folder == NEW_FOLDER)
						vms->newmessages++;
					else if (play_folder == OLD_FOLDER)
						vms->oldmessages++;
					cmd = tris_play_and_wait(chan, "voicemail/cancelled");
				}

				cmd = '#';


			} else /* Delete not valid if we haven't selected a message */
				cmd = 0;
			break;
		case '0':
		{
			int repeat = 0;
			
			res = close_mailbox(&O_vms,vmu);
			if (res == ERROR_LOCK_PATH)
				goto out;
			
			if (play_folder == NEW_FOLDER || play_folder == OLD_FOLDER) {
				res = close_mailbox(&I_vms,vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
			}
			
/*			res = open_mailbox(&O_vms, vmu, OLD_FOLDER);
			if (res == ERROR_LOCK_PATH)
				goto out;
			res = open_mailbox(&I_vms, vmu, NEW_FOLDER);
			if (res == ERROR_LOCK_PATH)
				goto out;
*/
			cmd = tris_play_and_wait(chan,"voicemail/main_menu");
			if(!cmd)
				cmd = tris_waitfordigit(chan, 6000);
			while((cmd > -1) && (cmd != 't')) {
				
				if(cmd >= '1' && cmd <= '3')
				{
					res = open_mailbox(&O_vms, vmu, cmd - '0');
					if (res == ERROR_LOCK_PATH)
						goto out;

					vms = &O_vms;
					play_folder = cmd -'0';
					
					if(cmd == '1') {
						res = open_mailbox(&I_vms, vmu, NEW_FOLDER);
						if (res == ERROR_LOCK_PATH)
							goto out;
						vms = &I_vms;
						play_folder = NEW_FOLDER;
					}
					
					//vms->repeats = 0;
					vms->starting = 1;
					cmd = '1';
					break;
				}
				else if(cmd == '*') { // The End
					cmd = 't';
					break;
				}
				
				if(!cmd){
					cmd = tris_play_and_wait(chan,"voicemail/main_menu");
					if(!cmd)
						cmd = tris_waitfordigit(chan, 6000);
				}
				else{
					cmd = tris_play_and_wait(chan,"voicemail/invalid_entry_try_again");
				}
				
				repeat++;
				if(repeat > 2) 
					cmd = 't';
			}
			
		}	break;
		case '5':
		case '6':
		case '7': 
		case '8':
		case '9':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		default:	/* Nothing */
			cmd = vm_instructions(chan, vms, 0);
			break;
		}
	}
	if ((cmd == 't') || (cmd == '*')) {
		/* Timeout */
		res = 0;
	} else {
		/* Hangup */
		res = -1;
	}

out:
	if (res > -1) {
		tris_stopstream(chan);
		adsi_goodbye(chan);
		if (valid) {
			if (silentexit)
				res = tris_play_and_wait(chan, "voicemail/vm-dialout");
			else 
				res = tris_play_and_wait(chan, "goodbye");
			if (res > 0)
				res = 0;
		}
		if (useadsi)
			tris_adsi_unload_session(chan);
	}
	if (vmu) {
		close_mailbox(&I_vms, vmu);
		close_mailbox(&O_vms, vmu);
	}
	if (valid) {
		int new = 0, old = 0;
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vms->username, vmu->context);
		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s\r\nWaiting: %d\r\n", ext_context, has_voicemail(ext_context, NULL));
		run_externnotify(vmu->context, vmu->mailbox);
		tris_app_inboxcount(ext_context, &new, &old);
		queue_mwi_event(ext_context, new, old);
	}
#ifdef IMAP_STORAGE
	/* expunge message - use UID Expunge if supported on IMAP server*/
	tris_debug(3, "*** Checking if we can expunge, deleted set to %d, expungeonhangup set to %d\n", deleted, expungeonhangup);
	if (vmu && deleted == 1 && expungeonhangup == 1) {
			mail_expunge(vms->mailstream);
	}
	/*  before we delete the state, we should copy pertinent info
	 *  back to the persistent model */
	vmstate_delete(vms);
#endif
	if (vmu)
		free_user(vmu);
	if (I_vms.deleted)
		tris_free(I_vms.deleted);
	if (I_vms.heard)
		tris_free(I_vms.heard);

	if (O_vms.deleted)
		tris_free(O_vms.deleted);
	if (O_vms.heard)
		tris_free(O_vms.heard);

	return res;
}

#define USAGE_PERMIT_CBONBUSY		(1<<13)

static int vm_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	int cmd = 0;
	int trycount = 0;
	const char *errcode, *busy_peer;
	const char* ext;
	char filename[PATH_MAX] = "";
	char sql[256], exten[80], fifoname[256];
	struct leave_vm_options leave_options;
	int to_usage_media_flag = 0, from_usage_media_flag = 0;
	int len = 0;
	FILE *f = NULL;
	
	memset(&leave_options, 0, sizeof(leave_options));
		
	snprintf(sql, sizeof(sql), "SELECT extension FROM uri WHERE username='%s'", chan->cid.cid_num);
	sql_select_query_execute(exten, sql);
	if(!tris_strlen_zero(exten) && strcmp(exten,chan->cid.cid_num)) {
		strcpy(chan->cid.cid_num, exten);
	}

	errcode = pbx_builtin_getvar_helper(chan, "Error-Info");
	ext = pbx_builtin_getvar_helper(chan, "Vm-User");
		
	snprintf(filename, sizeof(filename), "%sdefault/%s/greeting_y", VM_SPOOL_DIR, ext);

	if (!tris_strlen_zero(errcode)) {
		if ((busy_peer = strchr(errcode, ','))) {
			if (!strncmp(errcode,  "480", 3) || !strncmp(errcode,  "486", 3)) {
				tris_play_and_wait(chan, "voicemail/is_used");
				tris_play_and_wait(chan, "dial/dial-exten-num-is");
				tris_say_digit_str(chan, busy_peer+1, "", chan->language);
				tris_play_and_wait(chan, "dial/dial-is");
			}
		} else {
			if (!strcmp(errcode, "404")) {
				tris_play_and_wait(chan, "voicemail/is_not_found");
				return -1;
			} else if (!strcmp(errcode,  "4800")) {
				tris_play_and_wait(chan,
					tris_fileexists(filename, NULL, NULL) > 0 ? filename : "voicemail/is_expired");
			} else if (!strcmp(errcode,  "480") || !strcmp(errcode,  "486") || !strcmp(errcode,  "4860")) {
				tris_play_and_wait(chan, "voicemail/is_used");
			} else if (!strcmp(errcode,  "408")) {
				tris_play_and_wait(chan,
					tris_fileexists(filename, NULL, NULL) > 0 ? filename : "voicemail/is_not_accept");
			} else if (!strcmp(errcode,  "4031")) {
				tris_play_and_wait(chan, "voicemail/stop_use");
				return -1;
			} else if (!strcmp(errcode,  "4032")) {
				tris_play_and_wait(chan, "voicemail/refuse_call");
				return -1;
			} else if (!strcmp(errcode,  "4033")) {
				tris_play_and_wait(chan, "voicemail/cant_call");
				return -1;
			} else if (!strcmp(errcode,  "4034")) {
				tris_play_and_wait(chan, "voicemail/cant_local_phone");
				return -1;
			} else if (!strcmp(errcode,  "4035")) {
				tris_play_and_wait(chan, "voicemail/cant_trunk_call");
				return -1;
			} else if (!strcmp(errcode,  "4036")) {
				tris_play_and_wait(chan, "voicemail/cant_bu_call");
				return -1;
			} else if (!strcmp(errcode,  "4037")) {
				tris_play_and_wait(chan, "voicemail/cant_outside_call");
				return -1;
			} else if (!strcmp(errcode,  "4038")) {
				tris_play_and_wait(chan, "voicemail/no_media_service");
				return -1;
			} else if (!strcmp(errcode,  "4039")) {
				tris_play_and_wait(chan, "voicemail/cant_hunt_call");
				return -1;
			} else if (!strcmp(errcode,  "402")) {
				tris_play_and_wait(chan, "voicemail/no_money");
				return -1;
			} else if (!strcmp(errcode,  "410")) {
				tris_play_and_wait(chan, "voicemail/cant_outline");
				return -1;
			} else if (!strcmp(errcode,  "502")) {
				tris_play_and_wait(chan, "voicemail/all-circuits-busy-now");
				return -1;
			} else if (!strcmp(errcode,  "503")) {
				tris_play_and_wait(chan, "voicemail/line-failure");
				return -1;
			} else if (!strcmp(errcode,  "4040")) {
				tris_play_and_wait(chan, "voicemail/is_not_found0");
				return -1;
			} else if (!strcmp(errcode,  "5030")) {
				tris_play_and_wait(chan, "voicemail/line-failure0");
				return -1;
			} else if (!strcmp(errcode,  "704")) {
				if (chan->_state != TRIS_STATE_UP)
					tris_answer(chan);
				tris_play_and_wait(chan, "voicemail/cid_callback_set_ok");
				return -1;
			} else if (!strcmp(errcode,  "709")) {
				if (chan->_state != TRIS_STATE_UP)
					tris_answer(chan);
				tris_play_and_wait(chan, "callforward/extension-not-exist");
				return -1;
			}
		}
		
		if(!vm_user_exist((char*)ext)) 
			return -1;

	} else {
		ext = "";
	}

	if (tris_strlen_zero(ext))
		snprintf(sql, sizeof(sql), "SELECT vmpermit FROM user_info where uid='%s' or extension = '%s'", chan->cid.cid_num, chan->cid.cid_num);
	else
		snprintf(sql, sizeof(sql), "SELECT vmpermit FROM user_info where uid='%s' or extension = '%s'", ext, ext);
	sql_select_query_execute(exten, sql);
	to_usage_media_flag = atoi(exten);

	snprintf(sql, sizeof(sql), "SELECT usage_permit_flag FROM user_info WHERE extension = '%s'", chan->cid.cid_num);
	sql_select_query_execute(exten, sql);
	from_usage_media_flag = atoi(exten);
	if (strncmp(errcode, "4860", 4))
		from_usage_media_flag &= ~USAGE_PERMIT_CBONBUSY;

	if (!(from_usage_media_flag & USAGE_PERMIT_CBONBUSY) && !to_usage_media_flag) {
		if (tris_strlen_zero(ext))
			tris_play_and_wait(chan, "voicemail/cant_call");
		return 0;
	}

	while (!(cmd == '5' && (from_usage_media_flag & USAGE_PERMIT_CBONBUSY)) &&
			!(cmd == '1' && to_usage_media_flag) && !tris_strlen_zero(ext)) {
		if (trycount > 2) {
			tris_play_and_wait(chan, "goodbye");
			return 0;
		}
		if ((from_usage_media_flag & USAGE_PERMIT_CBONBUSY) && to_usage_media_flag)
			cmd = tris_play_and_wait(chan, "voicemail/to_callback_or_leave_a_msg");
		else if (from_usage_media_flag & USAGE_PERMIT_CBONBUSY)
			cmd = tris_play_and_wait(chan, "voicemail/to_callback");
		else
			cmd = tris_play_and_wait(chan, "voicemail/to_leave_a_msg");
		if (!cmd)
			cmd = tris_waitfordigit(chan, 6000);

		trycount ++;
	}
	
	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	if (cmd == '5' && (from_usage_media_flag & USAGE_PERMIT_CBONBUSY)) {
		snprintf(fifoname, sizeof(fifoname), "/tmp/trismedia_replyfifo-%s-%s", chan->cid.cid_num, ext);
		if (mkfifo(fifoname, 0) < 0) {
			tris_log(LOG_ERROR, "Can't make fifo file\n");
			tris_play_and_wait(chan, "voicemail/failed_to_callback");
			return 0;
		}
		
		len = snprintf(sql, sizeof(sql), ":b2blogic.register_callback_onbusy:trismedia_replyfifo-%s-%s\n%s\n%s\n%s\n\n",
				chan->cid.cid_num, ext, chan->cid.cid_num, ext, ext);
		res = write2fifo(sql, len);
		f = fopen(fifoname, "r");
		if (!f) {
			tris_log(LOG_ERROR, "Can't open fifo file descriptor\n");
			goto error;
		}
		fgets(sql, sizeof(sql), f);
		if (strstr(sql, "300")) {
			tris_play_and_wait(chan, "voicemail/already_callback");
		} else if (strstr(sql, "400")) {
			tris_play_and_wait(chan, "voicemail/destination_isnot_busy");
		} else if (strstr(sql, "500")) {
			tris_play_and_wait(chan, "voicemail/failed_to_callback");
		} else {
			tris_play_and_wait(chan, "voicemail/success_to_callback");
		}
		fclose(f);
		unlink(fifoname);
		tris_play_and_wait(chan, "goodbye");
	} else {
		res = leave_voicemail(chan, (char*)ext, &leave_options);
	}

	if (res == ERROR_LOCK_PATH) {
		tris_log(LOG_ERROR, "Could not leave voicemail. The path is already locked.\n");
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		res = 0;
	}

	return res;
error:
	tris_play_and_wait(chan, "voicemail/failed_to_callback");
	unlink(fifoname);
	return 0;
}

static int listen_cmd(struct tris_channel *chan, void *data)
{
	/* XXX This is, admittedly, some pretty horrendous code.  For some
	   reason it just seemed a lot easier to do with GOTO's.  I feel
	   like I'm back in my GWBASIC days. XXX */
	int res = -1;
	int cmd = 0;
	int valid = 0;
	char prefixstr[80] = "";
	char ext_context[256] = "";
	//int box;
	int useadsi = 0;
	int skipuser = 0;
	struct vm_state vms;
	struct tris_vm_user *vmu = NULL, vmus;
	char *context = NULL;
	int silentexit = 0;
	struct tris_flags flags = { 0 };
	signed char record_gain = 0;
	int play_auto = 0;
	int play_folder = 0;
#ifdef IMAP_STORAGE
	int deleted = 0;
#endif

	/* Add the vm_state to the active list and keep it active */
	memset(&vms, 0, sizeof(vms));
	vms.lastmsg = -1;

	memset(&vmus, 0, sizeof(vmus));

	if (chan->_state != TRIS_STATE_UP) {
		tris_debug(1, "Before tris_answer\n");
		tris_answer(chan);
	}


	if (!tris_strlen_zero(data)) {
		char *opts[OPT_ARG_ARRAY_SIZE];
		char *parse;
		TRIS_DECLARE_APP_ARGS(args,
			TRIS_APP_ARG(argv0);
			TRIS_APP_ARG(argv1);
		);

		parse = tris_strdupa(data);

		TRIS_STANDARD_APP_ARGS(args, parse);

		if (args.argc == 2) {
			if (tris_app_parse_options(vm_app_options, &flags, opts, args.argv1))
				return -1;
			if (tris_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;
				if (!tris_strlen_zero(opts[OPT_ARG_RECORDGAIN])) {
					if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
						tris_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
						return -1;
					} else {
						record_gain = (signed char) gain;
					}
				} else {
					tris_log(LOG_WARNING, "Invalid Gain level set with option g\n");
				}
			}
			if (tris_test_flag(&flags, OPT_AUTOPLAY) ) {
				play_auto = 1;
				if (opts[OPT_ARG_PLAYFOLDER]) {
					if (sscanf(opts[OPT_ARG_PLAYFOLDER], "%d", &play_folder) != 1) {
						tris_log(LOG_WARNING, "Invalid value '%s' provided for folder autoplay option\n", opts[OPT_ARG_PLAYFOLDER]);
					}
				} else {
					tris_log(LOG_WARNING, "Invalid folder set with option a\n");
				}	
				if (play_folder > 9 || play_folder < 0) {
					tris_log(LOG_WARNING, "Invalid value '%d' provided for folder autoplay option\n", play_folder);
					play_folder = 0;
				}
			}
		} else {
			/* old style options parsing */
			while (*(args.argv0)) {
				if (*(args.argv0) == 's')
					tris_set_flag(&flags, OPT_SILENT);
				else if (*(args.argv0) == 'p')
					tris_set_flag(&flags, OPT_PREPEND_MAILBOX);
				else 
					break;
				(args.argv0)++;
			}

		}

		valid = tris_test_flag(&flags, OPT_SILENT);

		if ((context = strchr(args.argv0, '@')))
			*context++ = '\0';

		if (tris_test_flag(&flags, OPT_PREPEND_MAILBOX))
			tris_copy_string(prefixstr, args.argv0, sizeof(prefixstr));
		else
			tris_copy_string(vms.username, args.argv0, sizeof(vms.username));

		if (!tris_strlen_zero(vms.username) && (vmu = create_user(&vmus, context, vms.username)))
			skipuser++;
		else
			valid = 0;
	}

//	if (!valid)
//		res = vm_authenticate(chan, vms.username, sizeof(vms.username), &vmus, context, prefixstr, skipuser, maxlogins, 0);

	tris_debug(1, "After vm_authenticate\n");
	if (!res) {
		valid = 1;
		if (!skipuser)
			vmu = &vmus;
	} else {
		res = 0;
	}

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);

/*#ifdef IMAP_STORAGE
	vms.interactive = 1;
	vms.updated = 1;
	vmstate_insert(&vms);
	init_vm_state(&vms);
#endif */
	if (!valid)
		goto out;

	if (!(vms.deleted = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		tris_log(LOG_ERROR, "Could not allocate memory for deleted message storage!\n");
		cmd = tris_play_and_wait(chan, "an-error-has-occured");
	}
	if (!(vms.heard = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	
	/* Set language from config to override channel language */
	if (!tris_strlen_zero(vmu->language))
		tris_string_field_set(chan, language, vmu->language);
	/* Retrieve old and new message counts */
	tris_debug(1, "Before open_mailbox\n");
	
//	res = open_mailbox(&vms, vmu, OLD_FOLDER);
//	if (res == ERROR_LOCK_PATH)
//		goto out;
//
//	vms.oldmessages = vms.lastmsg + 1;
	
	res = open_mailbox(&vms, vmu, NEW_FOLDER);
	if (res == ERROR_LOCK_PATH)
		goto out;
	
	vms.newmessages = vms.lastmsg + 1;
	tris_debug(1, "Number of new messages: %d\n", vms.newmessages);
		
	/* Select proper mailbox FIRST!! */
/*	if (play_auto) {
		res = open_mailbox(&vms, vmu, play_folder);
		if (res == ERROR_LOCK_PATH)
			goto out;

		// If there are no new messages, inform the user and hangup 
		if (vms.lastmsg == -1) {
			cmd = vm_browse_messages(chan, &vms, vmu);
			res = 0;
			goto out;
		}
	} else {
		if (!vms.newmessages && vms.oldmessages) {
			// If we only have old messages start here 
			res = open_mailbox(&vms, vmu, OLD_FOLDER);
			play_folder = 1;
			if (res == ERROR_LOCK_PATH)
				goto out;
		}
	}
*/
	if (useadsi)
		adsi_status(chan, &vms);
	res = 0;

	/* Check to see if this is a new user */

/*#ifdef IMAP_STORAGE
		tris_debug(3, "Checking quotas: comparing %u to %u\n", vms.quota_usage, vms.quota_limit);
		if (vms.quota_limit && vms.quota_usage >= vms.quota_limit) {
			tris_debug(1, "*** QUOTA EXCEEDED!!\n");
			cmd = tris_play_and_wait(chan, "voicemail/vm-mailboxfull");
		}
		tris_debug(3, "Checking quotas: User has %d messages and limit is %d.\n", (vms.newmessages + vms.oldmessages), vmu->maxmsg);
		if ((vms.newmessages + vms.oldmessages) >= vmu->maxmsg) {
			tris_log(LOG_WARNING, "No more messages possible.  User has %d messages and limit is %d.\n", (vms.newmessages + vms.oldmessages), vmu->maxmsg);
			cmd = tris_play_and_wait(chan, "voicemail/vm-mailboxfull");
		}
#endif*/
	if (play_auto) {
		cmd = '1';
	} else {
//		cmd = vm_intro(chan, vmu, &vms);
		cmd = '1'; //play first msg
	}

	//vms->repeats = 0;
	vms.starting = 1;
	vms.curmsg = vms.lastmsg;
	while ((cmd > -1) && (cmd != 't')) {
		/* Run main menu */
		switch (cmd) {
/*		case '1':
			if (vms.lastmsg > -1 && !vms.starting) {
				cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 3, record_gain);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
				}
				cmd = 0;
				break;
			} else
				vms.curmsg=0;
*/
			/* Fall through */
		case '1':
			cmd = cmd_browse_messages(chan, &vms, vmu);
			break;

//		case '2': /* Change folders */
/*			if (useadsi)
				adsi_folders(chan, 0, "Change to folder...");
			cmd = get_folder2(chan, "voicemail/vm-changeto", 0);
			if (cmd == '#') {
				cmd = 0;
			} else if (cmd > 0) {
				cmd = cmd - '0';
				res = close_mailbox(&vms, vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
				res = open_mailbox(&vms, vmu, cmd);
				if (res == ERROR_LOCK_PATH)
					goto out;
				play_folder = cmd;
				cmd = 0;
			}
			if (useadsi)
				adsi_status2(chan, &vms);
				
			if (!cmd)
				cmd = vm_play_folder_name(chan, vms.vmbox);

			vms.starting = 1;
			break;
*/
		case '2':
			if (vms.curmsg > 0) {
				vms.curmsg--;
				cmd = play_message_withinfo(chan, vmu, &vms);
			} 
			else {
				cmd = tris_play_and_wait(chan, "voicemail/cmd_no_before_msg");
			}
			break;
		case '3':
			if (vms.curmsg < vms.lastmsg) {
				vms.curmsg++;
				cmd = play_message_withinfo(chan, vmu, &vms);
			} else {
				cmd = tris_play_and_wait(chan, "voicemail/cmd_no_after_msg");
			}
			break;
		case '4':
			if(!tris_test_flag(&flags, OPT_COMMANDER)) {
				cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
				break;
			}
			
			if (vms.curmsg >= 0 && vms.curmsg <= vms.lastmsg) {
				vms.deleted[vms.curmsg] = !vms.deleted[vms.curmsg];
				if (useadsi)
					adsi_delete(chan, &vms);
				if (vms.deleted[vms.curmsg]) {
					if (play_folder == NEW_FOLDER)
						vms.newmessages--;
//					else if (play_folder == OLD_FOLDER)
//						vms.oldmessages--;
					if(play_folder == DELETED_FOLDER)
						cmd = tris_play_and_wait(chan, "voicemail/cmd_msg_deleted");
					else
						cmd = tris_play_and_wait(chan, "voicemail/cmd_msg_deleted");

				} else {
					if (play_folder == NEW_FOLDER)
						vms.newmessages++;
//					else if (play_folder == OLD_FOLDER)
//						vms.oldmessages++;
					cmd = tris_play_and_wait(chan, "voicemail/cancelled");
				}

				cmd = '3';

			} else /* Delete not valid if we haven't selected a message */
				cmd = 0;
			break;
		case '*':
			cmd = 't';
			break;
		case '5':
		case '6':
		case '7': 
		case '8':
		case '9':
		case '#':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		default:	/* Nothing */
			{
				int res = 0;
				int repeats = 0;
				/* Play instructions and wait for new command */
				while (!res) {
					if (vms.starting) {
						res = -1;
					} else {
						if(tris_test_flag(&flags, OPT_COMMANDER)) 
							res = tris_play_and_wait(chan, "voicemail/cmd_general_listen_options_admin");
						else
							res = tris_play_and_wait(chan, "voicemail/cmd_general_listen_options");
						
						if (!res)
							res = tris_waitfordigit(chan, 6000);
						if (!res) {
							repeats++;
							if (repeats > 2) {
								res = 't';
							}
						}
				
					}
				}
				cmd = res;
				break;
			}
		}
	}
	if ((cmd == 't') || (cmd == '*')) {
		/* Timeout */
		res = 0;
	} else {
		/* Hangup */
		res = -1;
	}

out:
	if (res > -1) {
		tris_stopstream(chan);
		adsi_goodbye(chan);
		if (valid) {
			if (silentexit)
				res = tris_play_and_wait(chan, "voicemail/vm-dialout");
			else 
				res = tris_play_and_wait(chan, "goodbye");
			if (res > 0)
				res = 0;
		}
		if (useadsi)
			tris_adsi_unload_session(chan);
	}
	if (vmu) {
		close_mailbox(&vms, vmu);
	}
	if (valid) {
		int new = 0, old = 0;
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vms.username, vmu->context);
		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s\r\nWaiting: %d\r\n", ext_context, has_voicemail(ext_context, NULL));
		run_externnotify(vmu->context, vmu->mailbox);
		tris_app_inboxcount(ext_context, &new, &old);
		queue_mwi_event(ext_context, new, old);
	}
#ifdef IMAP_STORAGE
	/* expunge message - use UID Expunge if supported on IMAP server*/
	tris_debug(3, "*** Checking if we can expunge, deleted set to %d, expungeonhangup set to %d\n", deleted, expungeonhangup);
	if (vmu && deleted == 1 && expungeonhangup == 1) {
			mail_expunge(vms.mailstream);
	}
	/*  before we delete the state, we should copy pertinent info
	 *  back to the persistent model */
	vmstate_delete(vms);
#endif
	if (vmu)
		free_user(vmu);
	if (vms.deleted)
		tris_free(vms.deleted);
	if (vms.heard)
		tris_free(vms.heard);

	return res;
}

static int check_command_listener(char *roomno, char *ext, char* cid_num) 
{
	char sql[256];
	char result[32];
	int accessmode = 0;

	snprintf(sql, sizeof(sql), "SELECT accessmode FROM general_command WHERE roomno='%s'", roomno);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		accessmode = 0;
	else
		accessmode = atoi(result);

	if (accessmode == 1 && strcmp(ext, cid_num))
		return 0;
	
	if (check_commander(roomno, ext))
		return 1;
	
	snprintf(sql, sizeof(sql), "SELECT listener_uid FROM general_cmd_listener WHERE roomno='%s' and listener_uid='%s'",
			roomno, ext);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;
	return 1;
}

static int check_command_publicity(char *roomno) 
{
	char sql[256];
	char result[32];

	snprintf(sql, sizeof(sql), "SELECT accessmode FROM general_command WHERE roomno='%s'", roomno);
	sql_select_query_execute(result, sql);
	if (tris_strlen_zero(result))
		return 0;
	if (result[0] == '0')
		return 0;
	return 1;
}

static int cmd_execmain(struct tris_channel *chan, void *data)
{
	int res = 0;
	int tries = 3;
	char roomno[256] = "";
	char phonenum[256] = "";
	char passwd[256] = "";
	char options[256] = "";
	
	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}
	
	if (tris_strlen_zero(chan->cid.cid_num)) {
		return -1;
	}

	while (tries && !res) {
		res = tris_app_getdata(chan, "voicemail/cmd_choice_roomno", roomno, sizeof(roomno)-1, 5000);
		if (!cmdroom_exist(roomno)) {
			tris_verbose("There is no command room\n");
			if(!tris_strlen_zero(roomno))
				tris_play_and_wait(chan, "voicemail/cmd_not_found_room");
			res = 0;
			tries--;
			continue;
		}
		res = 1;
		break;
	}

	if (!res) {
		tris_play_and_wait(chan, "goodbye");
		return 0;
	}

	snprintf(options, sizeof(options), "%s@cmd,s", roomno);
	if (!check_command_publicity(roomno)) {
		if (check_commander(roomno, chan->cid.cid_num))
			snprintf(options, sizeof(options), "%s@cmd,sc", roomno);
		else
			snprintf(options, sizeof(options), "%s@cmd,s", roomno);
		listen_cmd(chan, options);
		return 0;
	}

	tries = 3;
	res = 0;
	while (tries && !res) {
		res = tris_app_getdata(chan, "voicemail/dial_extn_pound", phonenum, sizeof(phonenum) - 1, 5000);
		if (!check_command_listener(roomno, phonenum, chan->cid.cid_num)) {
			tris_verbose("There is no phonenum\n");
			if(!tris_strlen_zero(phonenum))
				tris_play_and_wait(chan, "voicemail/cmd_invalid_num");
			res = 0;
			tries--;
			continue;
		}
		res = 1;
		break;
	}

	if (!res) {
		tris_play_and_wait(chan, "goodbye");
		return 0;
	}

	tries = 3;
	res = 0;
	while (tries && !res) {
		res = tris_app_getdata(chan, "voicemail/enter_pin", passwd, sizeof(passwd) - 1, 5000);
		if (!vm_login(phonenum, passwd)) {
			tris_verbose("There is no pin\n");
			if(!tris_strlen_zero(roomno))
				tris_play_and_wait(chan, "voicemail/invalid_pin");
			res = 0;
			tries--;
			continue;
		}
		res = 1;
		break;
	}
	if (!res) {
		tris_play_and_wait(chan, "goodbye");
		return 0;
	}
	if (check_commander(roomno, phonenum))
		snprintf(options, sizeof(options), "%s@cmd,sc", roomno);
	else
		snprintf(options, sizeof(options), "%s@cmd,s", roomno);
	listen_cmd(chan, options);
	return 0;
}

static int cmd_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct leave_vm_options leave_options;
	
	memset(&leave_options, 0, sizeof(leave_options));

	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	res = leave_cmd(chan, &leave_options);

	if (res == ERROR_LOCK_PATH) {
		tris_log(LOG_ERROR, "Could not leave command. The path is already locked.\n");
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		res = 0;
	}

	return 0;
}

static int report_intro(struct tris_channel *chan, struct vm_state *vms)
{
	int res;
	char nextmsg[256];
	/* Introduce messages they have */

	if (!vms->oldmessages && !vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/rprt_no_report_msg");
		return 't';
	}
	
	if (vms->newmessages) {
		res = tris_play_and_wait(chan, "voicemail/rprt_new_msg");
		snprintf(nextmsg,sizeof(nextmsg), "digits/piece-%d", vms->newmessages);
		if(!res)
			res = tris_play_and_wait(chan,nextmsg);
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-received");
	}
	else {
		res = tris_play_and_wait(chan, "voicemail/rprt_no_new_msg");
	}
/*	if (!res && vms->oldmessages) {
		res = say_and_wait(chan, vms->oldmessages, chan->language);
		if (!res)
			res = tris_play_and_wait(chan, "voicemail/vm-Old");
		if (!res) {
			if (vms->oldmessages == 1)
				res = tris_play_and_wait(chan, "voicemail/vm-message");
			else
				res = tris_play_and_wait(chan, "voicemail/vm-messages");
		}
	}
*/
	return res;
}

static int get_dirlist(char *context, char *mailbox, char list[][9], int *blen)
{
	DIR *dir;
	struct dirent *de;
	char fn[256];
	int i = 0;

	/* If no mailbox, return immediately */
	if (tris_strlen_zero(mailbox))
		return 0;

	if (tris_strlen_zero(context))
		context = "default";

	snprintf(fn, sizeof(fn), "%s%s/%s", VM_SPOOL_DIR, context, mailbox);

	if (!(dir = opendir(fn)))
		return 0;

	while ((de = readdir(dir))) {
		
		if (strlen(de->d_name) == 8) {
			strcpy(list[i++], de->d_name);
		}
	}

	closedir(dir);
	*blen = i;

	return 1;

}
	
static void array_sort (char list[][9], int len)
{
	int x,y;
	char c[9];
	for(x=0; x<len; x++)  {
		for(y=x+1; y<len; y++) 
			if(strcmp(list[x], list[y]) > 0) {
				strcpy(c,list[x]);
				strcpy(list[x],list[y]);
				strcpy(list[y],c);
			}
	}
	for(y=0; y<len; y++) 
		tris_verbose("%s\n", list[y]);
}

static int listen_rprt(struct tris_channel *chan, void *data)
{
	/* XXX This is, admittedly, some pretty horrendous code.  For some
	   reason it just seemed a lot easier to do with GOTO's.  I feel
	   like I'm back in my GWBASIC days. XXX */
	int res = -1;
	int cmd = 0;
	int valid = 0;
	char prefixstr[80] = "";
	char ext_context[256] = "";
	char blist[365][9];
	//int box;
	int useadsi = 0;
	int skipuser = 0;
	struct vm_state I_vms, O_vms;
	struct vm_state *vms =&I_vms;
	struct tris_vm_user *vmu = NULL, vmus;
	char *context = NULL;
	int silentexit = 0;
	struct tris_flags flags = { 0 };
	signed char record_gain = 0;
	int play_auto = 0;
	int play_folder = 0;
	int playingstate = 0;
	int cur_date = 1, blen = 1;
#ifdef IMAP_STORAGE
	int deleted = 0;
#endif

	/* Add the vm_state to the active list and keep it active */
	memset(&I_vms, 0, sizeof(I_vms));
	I_vms.lastmsg = -1;

	memset(&O_vms, 0, sizeof(O_vms));
	O_vms.lastmsg = -1;
	memset(&vmus, 0, sizeof(vmus));

	if (chan->_state != TRIS_STATE_UP) {
		tris_debug(1, "Before tris_answer\n");
		tris_answer(chan);
	}

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(argv0);
		TRIS_APP_ARG(argv1);
	);
	
	if (!tris_strlen_zero(data)) {
		char *opts[OPT_ARG_ARRAY_SIZE];
		char *parse;
		
		parse = tris_strdupa(data);

		TRIS_STANDARD_APP_ARGS(args, parse);

		if (args.argc == 2) {
			if (tris_app_parse_options(vm_app_options, &flags, opts, args.argv1))
				return -1;
			if (tris_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;
				if (!tris_strlen_zero(opts[OPT_ARG_RECORDGAIN])) {
					if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
						tris_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
						return -1;
					} else {
						record_gain = (signed char) gain;
					}
				} else {
					tris_log(LOG_WARNING, "Invalid Gain level set with option g\n");
				}
			}
			if (tris_test_flag(&flags, OPT_AUTOPLAY) ) {
				play_auto = 1;
				if (opts[OPT_ARG_PLAYFOLDER]) {
					if (sscanf(opts[OPT_ARG_PLAYFOLDER], "%d", &play_folder) != 1) {
						tris_log(LOG_WARNING, "Invalid value '%s' provided for folder autoplay option\n", opts[OPT_ARG_PLAYFOLDER]);
					}
				} else {
					tris_log(LOG_WARNING, "Invalid folder set with option a\n");
				}	
				if (play_folder > 9 || play_folder < 0) {
					tris_log(LOG_WARNING, "Invalid value '%d' provided for folder autoplay option\n", play_folder);
					play_folder = 0;
				}
			}
		} else {
			/* old style options parsing */
			while (*(args.argv0)) {
				if (*(args.argv0) == 's')
					tris_set_flag(&flags, OPT_SILENT);
				else if (*(args.argv0) == 'p')
					tris_set_flag(&flags, OPT_PREPEND_MAILBOX);
				else 
					break;
				(args.argv0)++;
			}

		}

		valid = tris_test_flag(&flags, OPT_SILENT);

		if ((context = strchr(args.argv0, '@')))
			*context++ = '\0';

		if (tris_test_flag(&flags, OPT_PREPEND_MAILBOX))
			tris_copy_string(prefixstr, args.argv0, sizeof(prefixstr));
		else {
			//tris_copy_string(I_vms.username, args.argv0, sizeof(I_vms.username));
			
			get_dirlist("report", args.argv0, blist, &blen);
			array_sort(blist, blen);
			
			sprintf(I_vms.username, "%s/%s", args.argv0, blist[blen-1]);
			tris_copy_string(O_vms.username, I_vms.username, sizeof(O_vms.username));
		}

		if (!tris_strlen_zero(I_vms.username) && (vmu = create_user(&vmus, context, I_vms.username)))
			skipuser++;
		else
			valid = 0;
	}

//	if (!valid)
//		res = vm_authenticate(chan, vms.username, sizeof(vms.username), &vmus, context, prefixstr, skipuser, maxlogins, 0);

/* ***** report authenticate begin ***** 
	tris_streamfile(chan, "voicemail/enter_pin",chan->language);
	int logretries = 0;
	char password[80]= "";
	while (!valid && (logretries < maxlogins)) {
		if (password[0] != '#') {
			if (tris_readstring(chan, password+strlen(password), sizeof(password) - 1, 2000, 10000, "#") < 0) {
				tris_log(LOG_WARNING, "Unable to read password\n");
				return -1;
			}
		}
		else {
			password[0] = '\0';
		}

		if (check_report_listener_pin(I_vms.username, password))
			valid++;
		
		logretries++;
		if (!valid) {
			if(!tris_strlen_zero(password)) {
				password[1] = '\0';
				password[0]=tris_play_and_wait(chan, "voicemail/invalid_pin");
			}
			
			if (logretries >= maxlogins) {
				break;
			} else {
				tris_streamfile(chan, "voicemail/enter_pin",chan->language);
			}
			if (tris_waitstream(chan, ""))	// Channel is hung up
				return -1;
		}
	}
	
	if (!valid && (logretries >= maxlogins)) {
		tris_stopstream(chan);
		tris_play_and_wait(chan, "goodbye");
	}
 ***** report authenticate end ***** */

	tris_debug(1, "After vm_authenticate\n");
	if (!res) {
		valid = 1;
		if (!skipuser)
			vmu = &vmus;
	} else {
		res = 0;
	}

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);

/*#ifdef IMAP_STORAGE
	vms.interactive = 1;
	vms.updated = 1;
	vmstate_insert(&vms);
	init_vm_state(&vms);
#endif */
	if (!valid)
		goto out;

	if (!(I_vms.deleted = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		tris_log(LOG_ERROR, "Could not allocate memory for deleted message storage!\n");
		cmd = tris_play_and_wait(chan, "an-error-has-occured");
	}
	if (!(O_vms.deleted = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	if (!(I_vms.heard = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	if (!(O_vms.heard = tris_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	
	/* Set language from config to override channel language */
	if (!tris_strlen_zero(vmu->language))
		tris_string_field_set(chan, language, vmu->language);
	/* Retrieve old and new message counts */
	tris_debug(1, "Before open_mailbox\n");
	
	res = open_mailbox(&O_vms, vmu, OLD_FOLDER);
	if (res == ERROR_LOCK_PATH)
		goto out;
	res = open_mailbox(&I_vms, vmu, NEW_FOLDER);
	if (res == ERROR_LOCK_PATH)
		goto out;
	
	I_vms.oldmessages = O_vms.lastmsg + 1;
	O_vms.oldmessages = O_vms.lastmsg + 1;
	I_vms.newmessages = I_vms.lastmsg + 1;
	O_vms.newmessages = I_vms.lastmsg + 1;
	tris_debug(1, "Number of new messages: %d\n", vms->newmessages);
		
	/* Select proper mailbox FIRST!! */
	if (play_auto) {
		res = open_mailbox(&O_vms, vmu, play_folder);
		if (res == ERROR_LOCK_PATH)
			goto out;

		/* If there are no new messages, inform the user and hangup */
		if (O_vms.lastmsg == -1) {
			cmd = rprt_browse_messages(chan, &O_vms, vmu);
			if(cmd=='*') 
				playingstate = 0;
			res = 0;
			goto out;
		}
		vms = &O_vms;
	} else {
		if (!I_vms.newmessages && I_vms.oldmessages) {
			/* If we only have old messages start here */
			vms = &O_vms;
			play_folder = OLD_FOLDER;
			if (res == ERROR_LOCK_PATH)
				goto out;
		}
	}

	if (useadsi)
		adsi_status(chan, vms);
	res = 0;

	/* Check to see if this is a new user */

/*#ifdef IMAP_STORAGE
		tris_debug(3, "Checking quotas: comparing %u to %u\n", vms->quota_usage, vms->quota_limit);
		if (vms->quota_limit && vms->quota_usage >= vms->quota_limit) {
			tris_debug(1, "*** QUOTA EXCEEDED!!\n");
			cmd = tris_play_and_wait(chan, "voicemail/vm-mailboxfull");
		}
		tris_debug(3, "Checking quotas: User has %d messages and limit is %d.\n", (vms->newmessages + vms->oldmessages), vmu->maxmsg);
		if ((vms->newmessages + vms->oldmessages) >= vmu->maxmsg) {
			tris_log(LOG_WARNING, "No more messages possible.  User has %d messages and limit is %d.\n", (vms->newmessages + vms->oldmessages), vmu->maxmsg);
			cmd = tris_play_and_wait(chan, "voicemail/vm-mailboxfull");
		}
#endif*/
	if (play_auto) {
		cmd = '1';
	} else {
		cmd = report_intro(chan, vms);
		if(cmd != 't')
			cmd = '*'; //play first msg
	}

	//vms->repeats = 0;
	cur_date = blen - 1;
	vms->starting = 1;
	while ((cmd > -1) && (cmd != 't')) {
		/* Run main menu */
		switch (cmd) {
/*		case '1':
			if (vms->lastmsg > -1 && !vms->starting) {
				cmd = advanced_options(chan, vmu, &vms, vms->curmsg, 3, record_gain);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
				}
				cmd = 0;
				break;
			} else
				vms->curmsg=0;
*/
			/* Fall through */
		case '1':
			if(vms->lastmsg < 0 && play_folder == NEW_FOLDER){
				vms = &O_vms;
				play_folder = OLD_FOLDER;
				vms->curmsg = 0;
				vms->starting = 1;
			}
			cmd = rprt_browse_messages(chan, vms, vmu);
			if(cmd == '*') playingstate = 0;
			break;

//		case '2': /* Change folders */
/*			if (useadsi)
				adsi_folders(chan, 0, "Change to folder...");
			cmd = get_folder2(chan, "voicemail/vm-changeto", 0);
			if (cmd == '#') {
				cmd = 0;
			} else if (cmd > 0) {
				cmd = cmd - '0';
				res = close_mailbox(&vms, vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
				res = open_mailbox(&vms, vmu, cmd);
				if (res == ERROR_LOCK_PATH)
					goto out;
				play_folder = cmd;
				cmd = 0;
			}
			if (useadsi)
				adsi_status2(chan, &vms);
				
			if (!cmd)
				cmd = vm_play_folder_name(chan, vms->vmbox);

			vms->starting = 1;
			break;
*/
		case '2':
			if (vms->curmsg) {
				vms->curmsg--;
				cmd = play_message_withinfo(chan, vmu, vms);
			} 
			else {
				if(play_folder == OLD_FOLDER && I_vms.lastmsg > -1) {
					vms = &I_vms;
					play_folder = NEW_FOLDER;
					vms->curmsg = I_vms.lastmsg;
					cmd = '1';
					vms->starting = 1;
				}
				else
					cmd = tris_play_and_wait(chan, "voicemail/rprt_no_before_msg");
			}
			break;
		case '3':
			if (vms->curmsg < vms->lastmsg) {
				vms->curmsg++;
				cmd = play_message_withinfo(chan, vmu, vms);
			} else {
				if(play_folder == NEW_FOLDER && O_vms.lastmsg > -1) {
					vms = &O_vms;
					play_folder = OLD_FOLDER;
					vms->curmsg = 0;
					cmd = '1';
					vms->starting = 1;
				}
				else
					cmd = tris_play_and_wait(chan, "voicemail/rprt_no_after_msg");
			}
			break;
		case '4':
			if (vms->curmsg >= 0 && vms->curmsg <= vms->lastmsg) {
				vms->deleted[vms->curmsg] = !vms->deleted[vms->curmsg];
				if (useadsi)
					adsi_delete(chan, vms);
				if (vms->deleted[vms->curmsg]) {
					if (play_folder == NEW_FOLDER)
						vms->newmessages--;
					else if (play_folder == OLD_FOLDER)
						vms->oldmessages--;
					if(play_folder == DELETED_FOLDER)
						cmd = tris_play_and_wait(chan, "voicemail/rprt_msg_deleted");
					else
						cmd = tris_play_and_wait(chan, "voicemail/rprt_msg_deleted");

				} else {
					if (play_folder == NEW_FOLDER)
						vms->newmessages++;
					else if (play_folder == OLD_FOLDER)
						vms->oldmessages++;
					cmd = tris_play_and_wait(chan, "voicemail/cancelled");
				}

				if (vms->curmsg < vms->lastmsg) {
					vms->curmsg++;
					cmd = play_message_withinfo(chan, vmu, vms);
				} else {
					if(play_folder == NEW_FOLDER && O_vms.lastmsg > -1) {
						vms = &O_vms;
						play_folder = OLD_FOLDER;
						vms->curmsg = 0;
						cmd = '1';
						vms->starting = 1;
					}
					else{
						cmd = tris_play_and_wait(chan, "voicemail/rprt_no_after_msg");
					}
				}

			} else /* Delete not valid if we haven't selected a message */
				cmd = 0;
			break;
		case '*':
			if (playingstate) {
				cmd = 't';
			} else {
				int repeat = 0;
				
				res = close_mailbox(&O_vms,vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
				
				if (play_folder == NEW_FOLDER || play_folder == OLD_FOLDER) {
					res = close_mailbox(&I_vms,vmu);
					if (res == ERROR_LOCK_PATH)
						goto out;
				}
				
	/*			res = open_mailbox(&O_vms, vmu, OLD_FOLDER);
				if (res == ERROR_LOCK_PATH)
					goto out;
				res = open_mailbox(&I_vms, vmu, NEW_FOLDER);
				if (res == ERROR_LOCK_PATH)
					goto out;
	*/
				cmd = tris_play_and_wait(chan,"voicemail/rprt_main_menu");
				if(!cmd)
					cmd = tris_waitfordigit(chan, 6000);
				while((cmd > -1) && (cmd != 't')) {

					if(cmd >= '1' && cmd <= '3')
					{
						switch(cmd) {
						case '1':
							cur_date = blen - 1;
							break;
						case '2':
							if(cur_date) 
								cur_date--;
							else
								cmd = tris_play_and_wait(chan,"voicemail/rprt_no_before_day");
							
							break;
						case '3':
							if(cur_date < blen-1) 
								cur_date++;
							else
								cmd = tris_play_and_wait(chan,"voicemail/rprt_no_after_day");

							break;
						}
						if(cmd) {
							//sprintf(I_vms.username, "%s/%02d", args.argv0, cur_date);
							sprintf(I_vms.username, "%s/%s", args.argv0, blist[cur_date]);
							tris_copy_string(O_vms.username, I_vms.username, sizeof(O_vms.username));
							
							res = open_mailbox(&O_vms, vmu, OLD_FOLDER);
							if (res == ERROR_LOCK_PATH)
								goto out;

							res = open_mailbox(&I_vms, vmu, NEW_FOLDER);
							if (res == ERROR_LOCK_PATH)
								goto out;
							vms = &I_vms;
							play_folder = NEW_FOLDER;
							
							//vms->repeats = 0;
							vms->starting = 1;
							cmd = '1';
							break;
						}
					}
					else if(cmd == '*') { // The End
						cmd = 't';
						break;
					}
					
					if(!cmd){
						cmd = tris_play_and_wait(chan,"voicemail/rprt_main_menu");
						if(!cmd)
							cmd = tris_waitfordigit(chan, 6000);
					}
					else{
						cmd = tris_play_and_wait(chan,"voicemail/invalid_entry_try_again");
					}
					
					repeat++;
					if(repeat > 2) 
						cmd = 't';
				}
				
			}	
			break;
			
		case '5':
		case '6':
		case '7': 
		case '8':
		case '9':
		case '#':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		default:	/* Nothing */
			{
				int res = 0;
				int repeats = 0;
				/* Play instructions and wait for new command */
				while (!res) {
					if (vms->starting) {
						res = -1;
					} else {
						res = tris_play_and_wait(chan, "voicemail/rprt_msg_listen_options");
						if (!res)
							res = tris_waitfordigit(chan, 6000);
						if (!res) {
							repeats++;
							if (repeats > 2) {
								res = 't';
							}
						}
				
					}
				}
				cmd = res;
				break;
			}
		}
	}
	if ((cmd == 't') || (cmd == '*')) {
		/* Timeout */
		res = 0;
	} else {
		/* Hangup */
		res = -1;
	}

out:
	if (res > -1) {
		tris_stopstream(chan);
		adsi_goodbye(chan);
		if (valid) {
			if (silentexit)
				res = tris_play_and_wait(chan, "voicemail/vm-dialout");
			else 
				res = tris_play_and_wait(chan, "goodbye");
			if (res > 0)
				res = 0;
		}
		if (useadsi)
			tris_adsi_unload_session(chan);
	}
	if (vmu) {
		close_mailbox(vms, vmu);
	}
	if (valid) {
		int new = 0, old = 0;
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vms->username, vmu->context);
		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s\r\nWaiting: %d\r\n", ext_context, has_voicemail(ext_context, NULL));
		run_externnotify(vmu->context, vmu->mailbox);
		tris_app_inboxcount(ext_context, &new, &old);
		queue_mwi_event(ext_context, new, old);
	}
#ifdef IMAP_STORAGE
	/* expunge message - use UID Expunge if supported on IMAP server*/
	tris_debug(3, "*** Checking if we can expunge, deleted set to %d, expungeonhangup set to %d\n", deleted, expungeonhangup);
	if (vmu && deleted == 1 && expungeonhangup == 1) {
			mail_expunge(vms->mailstream);
	}
	/*  before we delete the state, we should copy pertinent info
	 *  back to the persistent model */
	vmstate_delete(vms);
#endif
	if (vmu)
		free_user(vmu);
	if (vms->deleted)
		tris_free(vms->deleted);
	if (vms->heard)
		tris_free(vms->heard);

	return res;
}

static int check_report_listener(char *roomno, char * ext)
{
	char sql[256];
	char result[1024];
	char *tmp = 0, *cur;

	snprintf(sql, sizeof(sql), "SELECT listener_uid FROM report_listener WHERE roomno='%s' AND listener_uid REGEXP '.*%s.*'",
			roomno, ext);
	sql_select_query_execute(result, sql);
	
	if(tris_strlen_zero(result)){
		return 0;
	}

	cur = result;
	while (cur) {
		tmp = strsep(&cur, ",");
		if (!tmp)
			return 0;
		if (strlen(tmp) == strlen(ext) && !strncmp(tmp, ext, strlen(ext)))
			return 1;
	}

	return 0;

}

static int rprt_execmain(struct tris_channel *chan, void *data)
{
	int res = 0;
	int tries = 3;
	char roomno[256] = "";
	char passwd[256] = "";
	char options[256] = "";
	
	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}
	
	if (tris_strlen_zero(chan->cid.cid_num)) {
		return -1;
	}

	while (tries && !res) {
		res = tris_app_getdata(chan, "voicemail/rprt_choice_roomno", roomno, sizeof(roomno)-1, 5000);
		if (!check_report_listener(roomno, chan->cid.cid_num)) {
			tris_verbose("There is no report room\n");
			if(!tris_strlen_zero(roomno))
				tris_play_and_wait(chan, "voicemail/rprt_not_found_room");
			res = 0;
			tries--;
			continue;
		}
		res = 1;
		break;
	}

	if (!res) {
		tris_play_and_wait(chan, "goodbye");
		return 0;
	}

	tries = 3;
	res = 0;
	while (tries && !res) {
		res = tris_app_getdata(chan, "voicemail/enter_pin", passwd, sizeof(passwd) - 1, 5000);
		if (!vm_login(chan->cid.cid_num, passwd)) {
			tris_verbose("There is no pin\n");
			if(!tris_strlen_zero(roomno))
				tris_play_and_wait(chan, "voicemail/invalid_pin");
			res = 0;
			tries--;
			continue;
		}
		res = 1;
		break;
	}
	if (!res) {
		tris_play_and_wait(chan, "goodbye");
		return 0;
	}
	snprintf(options, sizeof(options), "%s@report,s", roomno);
	listen_rprt(chan, options);
	return 0;
}

static int rprt_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct leave_vm_options leave_options;
	
	memset(&leave_options, 0, sizeof(leave_options));

	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	res = leave_rprt(chan, &leave_options);

	if (res == ERROR_LOCK_PATH) {
		tris_log(LOG_ERROR, "Could not leave report. The path is already locked.\n");
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		res = 0;
	}

	return 0;
}


static int append_mailbox(const char *context, const char *mbox, const char *data)
{
	/* Assumes lock is already held */
	char *tmp;
	char *stringp;
	char *s;
	struct tris_vm_user *vmu;
	char *mailbox_full;
	int new = 0, old = 0;

	tmp = tris_strdupa(data);

	if (!(vmu = find_or_create(context, mbox)))
		return -1;
	
	populate_defaults(vmu);

	stringp = tmp;
	if ((s = strsep(&stringp, ","))) 
		tris_copy_string(vmu->password, s, sizeof(vmu->password));
	if (stringp && (s = strsep(&stringp, ","))) 
		tris_copy_string(vmu->fullname, s, sizeof(vmu->fullname));
	if (stringp && (s = strsep(&stringp, ","))) 
		tris_copy_string(vmu->email, s, sizeof(vmu->email));
	if (stringp && (s = strsep(&stringp, ","))) 
		tris_copy_string(vmu->pager, s, sizeof(vmu->pager));
	if (stringp && (s = strsep(&stringp, ","))) 
		apply_options(vmu, s);

	mailbox_full = alloca(strlen(mbox) + strlen(context) + 1);
	strcpy(mailbox_full, mbox);
	strcat(mailbox_full, "@");
	strcat(mailbox_full, context);

	inboxcount(mailbox_full, &new, &old);
	queue_mwi_event(mailbox_full, new, old);

	return 0;
}

static int vm_box_exists(struct tris_channel *chan, void *data) 
{
	struct tris_vm_user svm;
	char *context, *box;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(mbox);
		TRIS_APP_ARG(options);
	);
	static int dep_warning = 0;

	if (tris_strlen_zero(data)) {
		tris_log(LOG_ERROR, "MailboxExists requires an argument: (vmbox[@context][|options])\n");
		return -1;
	}

	if (!dep_warning) {
		dep_warning = 1;
		tris_log(LOG_WARNING, "MailboxExists is deprecated.  Please use ${MAILBOX_EXISTS(%s)} instead.\n", (char *)data);
	}

	box = tris_strdupa(data);

	TRIS_STANDARD_APP_ARGS(args, box);

	if (args.options) {
	}

	if ((context = strchr(args.mbox, '@'))) {
		*context = '\0';
		context++;
	}

	if (find_user(&svm, context, args.mbox)) {
		pbx_builtin_setvar_helper(chan, "VMBOXEXISTSSTATUS", "SUCCESS");
	} else
		pbx_builtin_setvar_helper(chan, "VMBOXEXISTSSTATUS", "FAILED");

	return 0;
}

static int acf_mailbox_exists(struct tris_channel *chan, const char *cmd, char *args, char *buf, size_t len)
{
	struct tris_vm_user svm;
	TRIS_DECLARE_APP_ARGS(arg,
		TRIS_APP_ARG(mbox);
		TRIS_APP_ARG(context);
	);

	TRIS_NONSTANDARD_APP_ARGS(arg, args, '@');

	tris_copy_string(buf, find_user(&svm, tris_strlen_zero(arg.context) ? "default" : arg.context, arg.mbox) ? "1" : "0", len);
	return 0;
}

static struct tris_custom_function mailbox_exists_acf = {
	.name = "MAILBOX_EXISTS",
	.synopsis = "Tell if a mailbox is configured",
	.desc =
"Returns a boolean of whether the corresponding mailbox exists.  If context\n"
"is not specified, defaults to the \"default\" context.\n",
	.syntax = "MAILBOX_EXISTS(<vmbox>[@<context>])",
	.read = acf_mailbox_exists,
};

static int vmauthenticate(struct tris_channel *chan, void *data)
{
	char *s = data, *user = NULL, *context = NULL, mailbox[TRIS_MAX_EXTENSION] = "";
	struct tris_vm_user vmus;
	char *options = NULL;
	int silent = 0, skipuser = 0;
	int res = -1;
	
	if (s) {
		s = tris_strdupa(s);
		user = strsep(&s, ",");
		options = strsep(&s, ",");
		if (user) {
			s = user;
			user = strsep(&s, "@");
			context = strsep(&s, "");
			if (!tris_strlen_zero(user))
				skipuser++;
			tris_copy_string(mailbox, user, sizeof(mailbox));
		}
	}

	if (options) {
		silent = (strchr(options, 's')) != NULL;
	}

	if (!vm_authenticate(chan, mailbox, sizeof(mailbox), &vmus, context, NULL, &skipuser, 3, silent)) {
		pbx_builtin_setvar_helper(chan, "AUTH_MAILBOX", mailbox);
		pbx_builtin_setvar_helper(chan, "AUTH_CONTEXT", vmus.context);
		tris_play_and_wait(chan, "auth-thankyou");
		res = 0;
	}

	return res;
}

static char *show_users_realtime(int fd, const char *context)
{
	struct tris_config *cfg;
	const char *cat = NULL;

	if (!(cfg = tris_load_realtime_multientry("voicemail", 
		"context", context, NULL))) {
		return CLI_FAILURE;
	}

	tris_cli(fd,
		"\n"
		"=============================================================\n"
		"=== Configured Voicemail Users ==============================\n"
		"=============================================================\n"
		"===\n");

	while ((cat = tris_category_browse(cfg, cat))) {
		struct tris_variable *var = NULL;
		tris_cli(fd,
			"=== Mailbox ...\n"
			"===\n");
		for (var = tris_variable_browse(cfg, cat); var; var = var->next)
			tris_cli(fd, "=== ==> %s: %s\n", var->name, var->value);
		tris_cli(fd,
			"===\n"
			"=== ---------------------------------------------------------\n"
			"===\n");
	}

	tris_cli(fd,
		"=============================================================\n"
		"\n");

	return CLI_SUCCESS;
}

static char *complete_voicemail_show_users(const char *line, const char *word, int pos, int state)
{
	int which = 0;
	int wordlen;
	struct tris_vm_user *vmu;
	const char *context = "";

	/* 0 - show; 1 - voicemail; 2 - users; 3 - for; 4 - <context> */
	if (pos > 4)
		return NULL;
	if (pos == 3)
		return (state == 0) ? tris_strdup("for") : NULL;
	wordlen = strlen(word);
	TRIS_LIST_TRAVERSE(&users, vmu, list) {
		if (!strncasecmp(word, vmu->context, wordlen)) {
			if (context && strcmp(context, vmu->context) && ++which > state)
				return tris_strdup(vmu->context);
			/* ignore repeated contexts ? */
			context = vmu->context;
		}
	}
	return NULL;
}

/*! \brief Show a list of voicemail users in the CLI */
static char *handle_voicemail_show_users(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct tris_vm_user *vmu;
#define HVSU_OUTPUT_FORMAT "%-10s %-5s %-25s %-10s %6s\n"
	const char *context = NULL;
	int users_counter = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "voicemail show users";
		e->usage =
			"Usage: voicemail show users [for <context>]\n"
			"       Lists all mailboxes currently set up\n";
		return NULL;
	case CLI_GENERATE:
		return complete_voicemail_show_users(a->line, a->word, a->pos, a->n);
	}	

	if ((a->argc < 3) || (a->argc > 5) || (a->argc == 4))
		return CLI_SHOWUSAGE;
	if (a->argc == 5) {
		if (strcmp(a->argv[3], "for"))
			return CLI_SHOWUSAGE;
		context = a->argv[4];
	}

	if (tris_check_realtime("voicemail")) {
		if (!context) {
			tris_cli(a->fd, "You must specify a specific context to show users from realtime!\n");
			return CLI_SHOWUSAGE;
		}
		return show_users_realtime(a->fd, context);
	}

	TRIS_LIST_LOCK(&users);
	if (TRIS_LIST_EMPTY(&users)) {
		tris_cli(a->fd, "There are no voicemail users currently defined\n");
		TRIS_LIST_UNLOCK(&users);
		return CLI_FAILURE;
	}
	if (a->argc == 3)
		tris_cli(a->fd, HVSU_OUTPUT_FORMAT, "Context", "Mbox", "User", "Zone", "NewMsg");
	else {
		int count = 0;
		TRIS_LIST_TRAVERSE(&users, vmu, list) {
			if (!strcmp(context, vmu->context))
				count++;
		}
		if (count) {
			tris_cli(a->fd, HVSU_OUTPUT_FORMAT, "Context", "Mbox", "User", "Zone", "NewMsg");
		} else {
			tris_cli(a->fd, "No such voicemail context \"%s\"\n", context);
			TRIS_LIST_UNLOCK(&users);
			return CLI_FAILURE;
		}
	}
	TRIS_LIST_TRAVERSE(&users, vmu, list) {
		int newmsgs = 0, oldmsgs = 0;
		char count[12], tmp[256] = "";

		if ((a->argc == 3) || ((a->argc == 5) && !strcmp(context, vmu->context))) {
			snprintf(tmp, sizeof(tmp), "%s@%s", vmu->mailbox, tris_strlen_zero(vmu->context) ? "default" : vmu->context);
			inboxcount(tmp, &newmsgs, &oldmsgs);
			snprintf(count, sizeof(count), "%d", newmsgs);
			tris_cli(a->fd, HVSU_OUTPUT_FORMAT, vmu->context, vmu->mailbox, vmu->fullname, vmu->zonetag, count);
			users_counter++;
		}
	}
	TRIS_LIST_UNLOCK(&users);
	tris_cli(a->fd, "%d voicemail users configured.\n", users_counter);
	return CLI_SUCCESS;
}

/*! \brief Show a list of voicemail zones in the CLI */
static char *handle_voicemail_show_zones(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	struct vm_zone *zone;
#define HVSZ_OUTPUT_FORMAT "%-15s %-20s %-45s\n"
	char *res = CLI_SUCCESS;

	switch (cmd) {
	case CLI_INIT:
		e->command = "voicemail show zones";
		e->usage =
			"Usage: voicemail show zones\n"
			"       Lists zone message formats\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	TRIS_LIST_LOCK(&zones);
	if (!TRIS_LIST_EMPTY(&zones)) {
		tris_cli(a->fd, HVSZ_OUTPUT_FORMAT, "Zone", "Timezone", "Message Format");
		TRIS_LIST_TRAVERSE(&zones, zone, list) {
			tris_cli(a->fd, HVSZ_OUTPUT_FORMAT, zone->name, zone->timezone, zone->msg_format);
		}
	} else {
		tris_cli(a->fd, "There are no voicemail zones currently defined\n");
		res = CLI_FAILURE;
	}
	TRIS_LIST_UNLOCK(&zones);

	return res;
}

/*! \brief Reload voicemail configuration from the CLI */
static char *handle_voicemail_reload(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "voicemail reload";
		e->usage =
			"Usage: voicemail reload\n"
			"       Reload voicemail configuration\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	tris_cli(a->fd, "Reloading voicemail configuration...\n");	
	load_config(1);
	
	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_voicemail[] = {
	TRIS_CLI_DEFINE(handle_voicemail_show_users, "List defined voicemail boxes"),
	TRIS_CLI_DEFINE(handle_voicemail_show_zones, "List zone message formats"),
	TRIS_CLI_DEFINE(handle_voicemail_reload, "Reload voicemail configuration"),
};

static void poll_subscribed_mailboxes(void)
{
	struct mwi_sub *mwi_sub;

	TRIS_RWLIST_RDLOCK(&mwi_subs);
	TRIS_RWLIST_TRAVERSE(&mwi_subs, mwi_sub, entry) {
		int new = 0, old = 0;

		if (tris_strlen_zero(mwi_sub->mailbox))
			continue;

		inboxcount(mwi_sub->mailbox, &new, &old);

		if (new != mwi_sub->old_new || old != mwi_sub->old_old) {
			mwi_sub->old_new = new;
			mwi_sub->old_old = old;
			queue_mwi_event(mwi_sub->mailbox, new, old);
		}
	}
	TRIS_RWLIST_UNLOCK(&mwi_subs);
}

static void *mb_poll_thread(void *data)
{
	while (poll_thread_run) {
		struct timespec ts = { 0, };
		struct timeval tv;

		tv = tris_tvadd(tris_tvnow(), tris_samp2tv(poll_freq, 1));
		ts.tv_sec = tv.tv_sec;
		ts.tv_nsec = tv.tv_usec * 1000;

		tris_mutex_lock(&poll_lock);
		tris_cond_timedwait(&poll_cond, &poll_lock, &ts);
		tris_mutex_unlock(&poll_lock);

		if (!poll_thread_run)
			break;

		poll_subscribed_mailboxes();
	}

	return NULL;
}

static void mwi_sub_destroy(struct mwi_sub *mwi_sub)
{
	tris_free(mwi_sub);
}

static void mwi_unsub_event_cb(const struct tris_event *event, void *userdata)
{
	uint32_t uniqueid;
	struct mwi_sub *mwi_sub;

	if (tris_event_get_type(event) != TRIS_EVENT_UNSUB)
		return;

	if (tris_event_get_ie_uint(event, TRIS_EVENT_IE_EVENTTYPE) != TRIS_EVENT_MWI)
		return;

	uniqueid = tris_event_get_ie_uint(event, TRIS_EVENT_IE_UNIQUEID);

	TRIS_RWLIST_WRLOCK(&mwi_subs);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&mwi_subs, mwi_sub, entry) {
		if (mwi_sub->uniqueid == uniqueid) {
			TRIS_LIST_REMOVE_CURRENT(entry);
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END
	TRIS_RWLIST_UNLOCK(&mwi_subs);

	if (mwi_sub)
		mwi_sub_destroy(mwi_sub);
}

static void mwi_sub_event_cb(const struct tris_event *event, void *userdata)
{
	const char *mailbox;
	const char *context;
	uint32_t uniqueid;
	unsigned int len;
	struct mwi_sub *mwi_sub;

	if (tris_event_get_type(event) != TRIS_EVENT_SUB)
		return;

	if (tris_event_get_ie_uint(event, TRIS_EVENT_IE_EVENTTYPE) != TRIS_EVENT_MWI)
		return;

	mailbox = tris_event_get_ie_str(event, TRIS_EVENT_IE_MAILBOX);
	context = tris_event_get_ie_str(event, TRIS_EVENT_IE_CONTEXT);
	uniqueid = tris_event_get_ie_uint(event, TRIS_EVENT_IE_UNIQUEID);

	len = sizeof(*mwi_sub);
	if (!tris_strlen_zero(mailbox))
		len += strlen(mailbox);

	if (!tris_strlen_zero(context))
		len += strlen(context) + 1; /* Allow for seperator */

	if (!(mwi_sub = tris_calloc(1, len)))
		return;

	mwi_sub->uniqueid = uniqueid;
	if (!tris_strlen_zero(mailbox))
		strcpy(mwi_sub->mailbox, mailbox);

	if (!tris_strlen_zero(context)) {
		strcat(mwi_sub->mailbox, "@");
		strcat(mwi_sub->mailbox, context);
	}

	TRIS_RWLIST_WRLOCK(&mwi_subs);
	TRIS_RWLIST_INSERT_TAIL(&mwi_subs, mwi_sub, entry);
	TRIS_RWLIST_UNLOCK(&mwi_subs);
}

static void start_poll_thread(void)
{
	pthread_attr_t attr;

	mwi_sub_sub = tris_event_subscribe(TRIS_EVENT_SUB, mwi_sub_event_cb, NULL,
		TRIS_EVENT_IE_EVENTTYPE, TRIS_EVENT_IE_PLTYPE_UINT, TRIS_EVENT_MWI,
		TRIS_EVENT_IE_END);

	mwi_unsub_sub = tris_event_subscribe(TRIS_EVENT_UNSUB, mwi_unsub_event_cb, NULL,
		TRIS_EVENT_IE_EVENTTYPE, TRIS_EVENT_IE_PLTYPE_UINT, TRIS_EVENT_MWI,
		TRIS_EVENT_IE_END);

	if (mwi_sub_sub)
		tris_event_report_subs(mwi_sub_sub);

	poll_thread_run = 1;

	pthread_attr_init(&attr);
	tris_pthread_create(&poll_thread, &attr, mb_poll_thread, NULL);
	pthread_attr_destroy(&attr);
}

static void stop_poll_thread(void)
{
	poll_thread_run = 0;

	if (mwi_sub_sub) {
		tris_event_unsubscribe(mwi_sub_sub);
		mwi_sub_sub = NULL;
	}

	if (mwi_unsub_sub) {
		tris_event_unsubscribe(mwi_unsub_sub);
		mwi_unsub_sub = NULL;
	}

	tris_mutex_lock(&poll_lock);
	tris_cond_signal(&poll_cond);
	tris_mutex_unlock(&poll_lock);

	pthread_join(poll_thread, NULL);

	poll_thread = TRIS_PTHREADT_NULL;
}

/*! \brief Manager list voicemail users command */
static int manager_list_voicemail_users(struct mansession *s, const struct message *m)
{
	struct tris_vm_user *vmu = NULL;
	const char *id = astman_get_header(m, "ActionID");
	char actionid[128] = "";

	if (!tris_strlen_zero(id))
		snprintf(actionid, sizeof(actionid), "ActionID: %s\r\n", id);

	TRIS_LIST_LOCK(&users);

	if (TRIS_LIST_EMPTY(&users)) {
		astman_send_ack(s, m, "There are no voicemail users currently defined.");
		TRIS_LIST_UNLOCK(&users);
		astman_append(s, "Event: VoicemailUserEntryComplete\r\n%s\r\n", actionid);
		return RESULT_SUCCESS;
	}
	
	astman_send_ack(s, m, "Voicemail user list will follow");
	
	TRIS_LIST_TRAVERSE(&users, vmu, list) {
		char dirname[256];

#ifdef IMAP_STORAGE
		int new, old;
		inboxcount (vmu->mailbox, &new, &old);
#endif
		
		make_dir(dirname, sizeof(dirname), vmu->context, vmu->mailbox, "INBOX");
		astman_append(s,
			"%s"
			"Event: VoicemailUserEntry\r\n"
			"VMContext: %s\r\n"
			"VoiceMailbox: %s\r\n"
			"Fullname: %s\r\n"
			"Email: %s\r\n"
			"Pager: %s\r\n"
			"ServerEmail: %s\r\n"
			"MailCommand: %s\r\n"
			"Language: %s\r\n"
			"TimeZone: %s\r\n"
			"Callback: %s\r\n"
			"Dialout: %s\r\n"
			"UniqueID: %s\r\n"
			"ExitContext: %s\r\n"
			"SayDurationMinimum: %d\r\n"
			"SayEnvelope: %s\r\n"
			"SayCID: %s\r\n"
			"AttachMessage: %s\r\n"
			"AttachmentFormat: %s\r\n"
			"DeleteMessage: %s\r\n"
			"VolumeGain: %.2f\r\n"
			"CanReview: %s\r\n"
			"CallOperator: %s\r\n"
			"MaxMessageCount: %d\r\n"
			"MaxMessageLength: %d\r\n"
			"NewMessageCount: %d\r\n"
#ifdef IMAP_STORAGE
			"OldMessageCount: %d\r\n"
			"IMAPUser: %s\r\n"
#endif
			"\r\n",
			actionid,
			vmu->context,
			vmu->mailbox,
			vmu->fullname,
			vmu->email,
			vmu->pager,
			vmu->serveremail,
			vmu->mailcmd,
			vmu->language,
			vmu->zonetag,
			vmu->callback,
			vmu->dialout,
			vmu->uniqueid,
			vmu->exit,
			vmu->saydurationm,
			tris_test_flag(vmu, VM_ENVELOPE) ? "Yes" : "No",
			tris_test_flag(vmu, VM_SAYCID) ? "Yes" : "No",
			tris_test_flag(vmu, VM_ATTACH) ? "Yes" : "No",
			vmu->attachfmt,
			tris_test_flag(vmu, VM_DELETE) ? "Yes" : "No",
			vmu->volgain,
			tris_test_flag(vmu, VM_REVIEW) ? "Yes" : "No",
			tris_test_flag(vmu, VM_OPERATOR) ? "Yes" : "No",
			vmu->maxmsg,
			vmu->maxsecs,
#ifdef IMAP_STORAGE
			new, old, vmu->imapuser
#else
			count_messages(vmu, dirname)
#endif
			);
	}		
	astman_append(s, "Event: VoicemailUserEntryComplete\r\n%s\r\n", actionid);

	TRIS_LIST_UNLOCK(&users);

	return RESULT_SUCCESS;
}

/*! \brief Free the users structure. */
static void free_vm_users(void) 
{
	struct tris_vm_user *cur;
	TRIS_LIST_LOCK(&users);
	while ((cur = TRIS_LIST_REMOVE_HEAD(&users, list))) {
		tris_set_flag(cur, VM_ALLOCED);
		free_user(cur);
	}
	TRIS_LIST_UNLOCK(&users);
}

/*! \brief Free the zones structure. */
static void free_vm_zones(void)
{
	struct vm_zone *zcur;
	TRIS_LIST_LOCK(&zones);
	while ((zcur = TRIS_LIST_REMOVE_HEAD(&zones, list)))
		free_zone(zcur);
	TRIS_LIST_UNLOCK(&zones);
}

static int load_config(int reload)
{
	struct tris_vm_user *cur;
	struct tris_config *cfg, *ucfg;
	char *cat;
	struct tris_variable *var;
	const char *val;
	char *q, *stringp;
	int x;
	int tmpadsi[4];
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	if ((cfg = tris_config_load(VOICEMAIL_CONFIG, config_flags)) == CONFIG_STATUS_FILEUNCHANGED) {
		if ((ucfg = tris_config_load("users.conf", config_flags)) == CONFIG_STATUS_FILEUNCHANGED)
			return 0;
		tris_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		cfg = tris_config_load(VOICEMAIL_CONFIG, config_flags);
	} else {
		tris_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		ucfg = tris_config_load("users.conf", config_flags);
	}
#ifdef IMAP_STORAGE
	tris_copy_string(imapparentfolder, "\0", sizeof(imapparentfolder));
#endif
	/* set audio control prompts */
	strcpy(listen_control_forward_key, DEFAULT_LISTEN_CONTROL_FORWARD_KEY);
	strcpy(listen_control_reverse_key, DEFAULT_LISTEN_CONTROL_REVERSE_KEY);
	strcpy(listen_control_pause_key, DEFAULT_LISTEN_CONTROL_PAUSE_KEY);
	strcpy(listen_control_restart_key, DEFAULT_LISTEN_CONTROL_RESTART_KEY);
	strcpy(listen_control_stop_key, DEFAULT_LISTEN_CONTROL_STOP_KEY);

	/* Free all the users structure */	
	free_vm_users();

	/* Free all the zones structure */
	free_vm_zones();

	TRIS_LIST_LOCK(&users);	

	memset(ext_pass_cmd, 0, sizeof(ext_pass_cmd));

	if (cfg) {
		/* General settings */

		if (!(val = tris_variable_retrieve(cfg, "general", "userscontext")))
			val = "default";
		tris_copy_string(userscontext, val, sizeof(userscontext));
		/* Attach voice message to mail message ? */
		if (!(val = tris_variable_retrieve(cfg, "general", "attach"))) 
			val = "yes";
		tris_set2_flag((&globalflags), tris_true(val), VM_ATTACH);	

		if (!(val = tris_variable_retrieve(cfg, "general", "searchcontexts")))
			val = "no";
		tris_set2_flag((&globalflags), tris_true(val), VM_SEARCH);

		volgain = 0.0;
		if ((val = tris_variable_retrieve(cfg, "general", "volgain")))
			sscanf(val, "%lf", &volgain);

#ifdef ODBC_STORAGE
		strcpy(odbc_database, "trismedia");
		if ((val = tris_variable_retrieve(cfg, "general", "odbcstorage"))) {
			tris_copy_string(odbc_database, val, sizeof(odbc_database));
		}
		strcpy(odbc_table, "voicemessages");
		if ((val = tris_variable_retrieve(cfg, "general", "odbctable"))) {
			tris_copy_string(odbc_table, val, sizeof(odbc_table));
		}
#endif		
		/* Mail command */
		strcpy(mailcmd, SENDMAIL);
		if ((val = tris_variable_retrieve(cfg, "general", "mailcmd")))
			tris_copy_string(mailcmd, val, sizeof(mailcmd)); /* User setting */

		maxsilence = 0;
		if ((val = tris_variable_retrieve(cfg, "general", "maxsilence"))) {
			maxsilence = atoi(val);
			if (maxsilence > 0)
				maxsilence *= 1000;
		}
		
		if (!(val = tris_variable_retrieve(cfg, "general", "maxmsg"))) {
			maxmsg = MAXMSG;
		} else {
			maxmsg = atoi(val);
			if (maxmsg <= 0) {
				tris_log(LOG_WARNING, "Invalid number of messages per folder '%s'. Using default value %i\n", val, MAXMSG);
				maxmsg = MAXMSG;
			} else if (maxmsg > MAXMSGLIMIT) {
				tris_log(LOG_WARNING, "Maximum number of messages per folder is %i. Cannot accept value '%s'\n", MAXMSGLIMIT, val);
				maxmsg = MAXMSGLIMIT;
			}
		}

		if (!(val = tris_variable_retrieve(cfg, "general", "backupdeleted"))) {
			maxdeletedmsg = MAXMSG;
		} else {
			if (sscanf(val, "%d", &x) == 1)
				maxdeletedmsg = x;
			else if (tris_true(val))
				maxdeletedmsg = MAXMSG;
			else
				maxdeletedmsg = 20;

			if (maxdeletedmsg < 0) {
				tris_log(LOG_WARNING, "Invalid number of deleted messages saved per mailbox '%s'. Using default value %i\n", val, MAXMSG);
				maxdeletedmsg = MAXMSG;
			} else if (maxdeletedmsg > MAXMSGLIMIT) {
				tris_log(LOG_WARNING, "Maximum number of deleted messages saved per mailbox is %i. Cannot accept value '%s'\n", MAXMSGLIMIT, val);
				maxdeletedmsg = MAXMSGLIMIT;
			}
		}

		/* Load date format config for voicemail mail */
		if ((val = tris_variable_retrieve(cfg, "general", "emaildateformat"))) {
			tris_copy_string(emaildateformat, val, sizeof(emaildateformat));
		}

		/* External password changing command */
		if ((val = tris_variable_retrieve(cfg, "general", "externpass"))) {
			tris_copy_string(ext_pass_cmd, val, sizeof(ext_pass_cmd));
			pwdchange = PWDCHANGE_EXTERNAL;
		} else if ((val = tris_variable_retrieve(cfg, "general", "externpassnotify"))) {
			tris_copy_string(ext_pass_cmd, val, sizeof(ext_pass_cmd));
			pwdchange = PWDCHANGE_EXTERNAL | PWDCHANGE_INTERNAL;
		}

#ifdef IMAP_STORAGE
		/* IMAP server address */
		if ((val = tris_variable_retrieve(cfg, "general", "imapserver"))) {
			tris_copy_string(imapserver, val, sizeof(imapserver));
		} else {
			tris_copy_string(imapserver, "localhost", sizeof(imapserver));
		}
		/* IMAP server port */
		if ((val = tris_variable_retrieve(cfg, "general", "imapport"))) {
			tris_copy_string(imapport, val, sizeof(imapport));
		} else {
			tris_copy_string(imapport, "143", sizeof(imapport));
		}
		/* IMAP server flags */
		if ((val = tris_variable_retrieve(cfg, "general", "imapflags"))) {
			tris_copy_string(imapflags, val, sizeof(imapflags));
		}
		/* IMAP server master username */
		if ((val = tris_variable_retrieve(cfg, "general", "authuser"))) {
			tris_copy_string(authuser, val, sizeof(authuser));
		}
		/* IMAP server master password */
		if ((val = tris_variable_retrieve(cfg, "general", "authpassword"))) {
			tris_copy_string(authpassword, val, sizeof(authpassword));
		}
		/* Expunge on exit */
		if ((val = tris_variable_retrieve(cfg, "general", "expungeonhangup"))) {
			if (tris_false(val))
				expungeonhangup = 0;
			else
				expungeonhangup = 1;
		} else {
			expungeonhangup = 1;
		}
		/* IMAP voicemail folder */
		if ((val = tris_variable_retrieve(cfg, "general", "imapfolder"))) {
			tris_copy_string(imapfolder, val, sizeof(imapfolder));
		} else {
			tris_copy_string(imapfolder, "INBOX", sizeof(imapfolder));
		}
		if ((val = tris_variable_retrieve(cfg, "general", "imapparentfolder"))) {
			tris_copy_string(imapparentfolder, val, sizeof(imapparentfolder));
		}
		if ((val = tris_variable_retrieve(cfg, "general", "imapgreetings"))) {
			imapgreetings = tris_true(val);
		} else {
			imapgreetings = 0;
		}
		if ((val = tris_variable_retrieve(cfg, "general", "greetingfolder"))) {
			tris_copy_string(greetingfolder, val, sizeof(greetingfolder));
		} else {
			tris_copy_string(greetingfolder, imapfolder, sizeof(greetingfolder));
		}

		/* There is some very unorthodox casting done here. This is due
		 * to the way c-client handles the argument passed in. It expects a 
		 * void pointer and casts the pointer directly to a long without
		 * first dereferencing it. */
		if ((val = tris_variable_retrieve(cfg, "general", "imapreadtimeout"))) {
			mail_parameters(NIL, SET_READTIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_READTIMEOUT, (void *) 60L);
		}

		if ((val = tris_variable_retrieve(cfg, "general", "imapwritetimeout"))) {
			mail_parameters(NIL, SET_WRITETIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_WRITETIMEOUT, (void *) 60L);
		}

		if ((val = tris_variable_retrieve(cfg, "general", "imapopentimeout"))) {
			mail_parameters(NIL, SET_OPENTIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_OPENTIMEOUT, (void *) 60L);
		}

		if ((val = tris_variable_retrieve(cfg, "general", "imapclosetimeout"))) {
			mail_parameters(NIL, SET_CLOSETIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_CLOSETIMEOUT, (void *) 60L);
		}

#endif
		/* External voicemail notify application */
		if ((val = tris_variable_retrieve(cfg, "general", "externnotify"))) {
			tris_copy_string(externnotify, val, sizeof(externnotify));
			tris_debug(1, "found externnotify: %s\n", externnotify);
		} else {
			externnotify[0] = '\0';
		}

		/* SMDI voicemail notification */
		if ((val = tris_variable_retrieve(cfg, "general", "smdienable")) && tris_true(val)) {
			tris_debug(1, "Enabled SMDI voicemail notification\n");
			if ((val = tris_variable_retrieve(cfg, "general", "smdiport"))) {
				smdi_iface = tris_smdi_interface_find(val);
			} else {
				tris_debug(1, "No SMDI interface set, trying default (/dev/ttyS0)\n");
				smdi_iface = tris_smdi_interface_find("/dev/ttyS0");
			}
			if (!smdi_iface) {
				tris_log(LOG_ERROR, "No valid SMDI interface specfied, disabling SMDI voicemail notification\n");
			}
		}

		/* Silence treshold */
		silencethreshold = 256;
		if ((val = tris_variable_retrieve(cfg, "general", "silencethreshold")))
			silencethreshold = atoi(val);
		
		if (!(val = tris_variable_retrieve(cfg, "general", "serveremail"))) 
			val = TRISMEDIA_USERNAME;
		tris_copy_string(serveremail, val, sizeof(serveremail));
		
		vmmaxsecs = DEFAULT_MAXSECS;
		if ((val = tris_variable_retrieve(cfg, "general", "maxsecs"))) {
			if (sscanf(val, "%d", &x) == 1) {
				vmmaxsecs = x;
			} else {
				tris_log(LOG_WARNING, "Invalid max message time length\n");
			}
		} else if ((val = tris_variable_retrieve(cfg, "general", "maxmessage"))) {
			static int maxmessage_deprecate = 0;
			if (maxmessage_deprecate == 0) {
				maxmessage_deprecate = 1;
				tris_log(LOG_WARNING, "Setting 'maxmessage' has been deprecated in favor of 'maxsecs'.\n");
			}
			if (sscanf(val, "%d", &x) == 1) {
				vmmaxsecs = x;
			} else {
				tris_log(LOG_WARNING, "Invalid max message time length\n");
			}
		}

		vmminsecs = 0;
		if ((val = tris_variable_retrieve(cfg, "general", "minsecs"))) {
			if (sscanf(val, "%d", &x) == 1) {
				vmminsecs = x;
				if (maxsilence <= vmminsecs)
					tris_log(LOG_WARNING, "maxsilence should be less than minmessage or you may get empty messages\n");
			} else {
				tris_log(LOG_WARNING, "Invalid min message time length\n");
			}
		} else if ((val = tris_variable_retrieve(cfg, "general", "minmessage"))) {
			static int maxmessage_deprecate = 0;
			if (maxmessage_deprecate == 0) {
				maxmessage_deprecate = 1;
				tris_log(LOG_WARNING, "Setting 'minmessage' has been deprecated in favor of 'minsecs'.\n");
			}
			if (sscanf(val, "%d", &x) == 1) {
				vmminsecs = x;
				if (maxsilence <= vmminsecs)
					tris_log(LOG_WARNING, "maxsilence should be less than minmessage or you may get empty messages\n");
			} else {
				tris_log(LOG_WARNING, "Invalid min message time length\n");
			}
		}

		val = tris_variable_retrieve(cfg, "general", "format");
		if (!val)
			val = "wav";	
		tris_copy_string(vmfmts, val, sizeof(vmfmts));

		skipms = 3000;
		if ((val = tris_variable_retrieve(cfg, "general", "maxgreet"))) {
			if (sscanf(val, "%d", &x) == 1) {
				maxgreet = x;
			} else {
				tris_log(LOG_WARNING, "Invalid max message greeting length\n");
			}
		}

		if ((val = tris_variable_retrieve(cfg, "general", "skipms"))) {
			if (sscanf(val, "%d", &x) == 1) {
				skipms = x;
			} else {
				tris_log(LOG_WARNING, "Invalid skipms value\n");
			}
		}

		maxlogins = 3;
		if ((val = tris_variable_retrieve(cfg, "general", "maxlogins"))) {
			if (sscanf(val, "%d", &x) == 1) {
				maxlogins = x;
			} else {
				tris_log(LOG_WARNING, "Invalid max failed login attempts\n");
			}
		}

		/* Force new user to record name ? */
		if (!(val = tris_variable_retrieve(cfg, "general", "forcename"))) 
			val = "no";
		tris_set2_flag((&globalflags), tris_true(val), VM_FORCENAME);

		/* Force new user to record greetings ? */
		if (!(val = tris_variable_retrieve(cfg, "general", "forcegreetings"))) 
			val = "no";
		tris_set2_flag((&globalflags), tris_true(val), VM_FORCEGREET);

		if ((val = tris_variable_retrieve(cfg, "general", "cidinternalcontexts"))) {
			tris_debug(1, "VM_CID Internal context string: %s\n", val);
			stringp = tris_strdupa(val);
			for (x = 0; x < MAX_NUM_CID_CONTEXTS; x++) {
				if (!tris_strlen_zero(stringp)) {
					q = strsep(&stringp, ",");
					while ((*q == ' ')||(*q == '\t')) /* Eat white space between contexts */
						q++;
					tris_copy_string(cidinternalcontexts[x], q, sizeof(cidinternalcontexts[x]));
					tris_debug(1, "VM_CID Internal context %d: %s\n", x, cidinternalcontexts[x]);
				} else {
					cidinternalcontexts[x][0] = '\0';
				}
			}
		}
		if (!(val = tris_variable_retrieve(cfg, "general", "review"))) {
			tris_debug(1, "VM Review Option disabled globally\n");
			val = "no";
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_REVIEW);	

		/* Temporary greeting reminder */
		if (!(val = tris_variable_retrieve(cfg, "general", "tempgreetwarn"))) {
			tris_debug(1, "VM Temporary Greeting Reminder Option disabled globally\n");
			val = "no";
		} else {
			tris_debug(1, "VM Temporary Greeting Reminder Option enabled globally\n");
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_TEMPGREETWARN);

		if (!(val = tris_variable_retrieve(cfg, "general", "operator"))) {
			tris_debug(1, "VM Operator break disabled globally\n");
			val = "no";
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_OPERATOR);	

		if (!(val = tris_variable_retrieve(cfg, "general", "saycid"))) {
			tris_debug(1, "VM CID Info before msg disabled globally\n");
			val = "no";
		} 
		tris_set2_flag((&globalflags), tris_true(val), VM_SAYCID);	

		if (!(val = tris_variable_retrieve(cfg, "general", "sendvoicemail"))) {
			tris_debug(1, "Send Voicemail msg disabled globally\n");
			val = "no";
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_SVMAIL);
	
		if (!(val = tris_variable_retrieve(cfg, "general", "envelope"))) {
			tris_debug(1, "ENVELOPE before msg enabled globally\n");
			val = "yes";
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_ENVELOPE);	

		if (!(val = tris_variable_retrieve(cfg, "general", "moveheard"))) {
			tris_debug(1, "Move Heard enabled globally\n");
			val = "yes";
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_MOVEHEARD);	

		if (!(val = tris_variable_retrieve(cfg, "general", "sayduration"))) {
			tris_debug(1, "Duration info before msg enabled globally\n");
			val = "yes";
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_SAYDURATION);	

		saydurationminfo = 2;
		if ((val = tris_variable_retrieve(cfg, "general", "saydurationm"))) {
			if (sscanf(val, "%d", &x) == 1) {
				saydurationminfo = x;
			} else {
				tris_log(LOG_WARNING, "Invalid min duration for say duration\n");
			}
		}

		if (!(val = tris_variable_retrieve(cfg, "general", "nextaftercmd"))) {
			tris_debug(1, "We are not going to skip to the next msg after save/delete\n");
			val = "no";
		}
		tris_set2_flag((&globalflags), tris_true(val), VM_SKIPAFTERCMD);

		if ((val = tris_variable_retrieve(cfg, "general", "dialout"))) {
			tris_copy_string(dialcontext, val, sizeof(dialcontext));
			tris_debug(1, "found dialout context: %s\n", dialcontext);
		} else {
			dialcontext[0] = '\0';	
		}
		
		if ((val = tris_variable_retrieve(cfg, "general", "callback"))) {
			tris_copy_string(callcontext, val, sizeof(callcontext));
			tris_debug(1, "found callback context: %s\n", callcontext);
		} else {
			callcontext[0] = '\0';
		}

		if ((val = tris_variable_retrieve(cfg, "general", "exitcontext"))) {
			tris_copy_string(exitcontext, val, sizeof(exitcontext));
			tris_debug(1, "found operator context: %s\n", exitcontext);
		} else {
			exitcontext[0] = '\0';
		}
		
		/* load password sounds configuration */
		if ((val = tris_variable_retrieve(cfg, "general", "voicemail/vm-password")))
			tris_copy_string(vm_password, val, sizeof(vm_password));
		if ((val = tris_variable_retrieve(cfg, "general", "voicemail/vm-newpassword")))
			tris_copy_string(vm_newpassword, val, sizeof(vm_newpassword));
		if ((val = tris_variable_retrieve(cfg, "general", "voicemail/vm-passchanged")))
			tris_copy_string(vm_passchanged, val, sizeof(vm_passchanged));
		if ((val = tris_variable_retrieve(cfg, "general", "voicemail/vm-reenterpassword")))
			tris_copy_string(vm_reenterpassword, val, sizeof(vm_reenterpassword));
		if ((val = tris_variable_retrieve(cfg, "general", "voicemail/vm-mismatch")))
			tris_copy_string(vm_mismatch, val, sizeof(vm_mismatch));
		/* load configurable audio prompts */
		if ((val = tris_variable_retrieve(cfg, "general", "listen-control-forward-key")) && is_valid_dtmf(val))
			tris_copy_string(listen_control_forward_key, val, sizeof(listen_control_forward_key));
		if ((val = tris_variable_retrieve(cfg, "general", "listen-control-reverse-key")) && is_valid_dtmf(val))
			tris_copy_string(listen_control_reverse_key, val, sizeof(listen_control_reverse_key));
		if ((val = tris_variable_retrieve(cfg, "general", "listen-control-pause-key")) && is_valid_dtmf(val))
			tris_copy_string(listen_control_pause_key, val, sizeof(listen_control_pause_key));
		if ((val = tris_variable_retrieve(cfg, "general", "listen-control-restart-key")) && is_valid_dtmf(val))
			tris_copy_string(listen_control_restart_key, val, sizeof(listen_control_restart_key));
		if ((val = tris_variable_retrieve(cfg, "general", "listen-control-stop-key")) && is_valid_dtmf(val))
			tris_copy_string(listen_control_stop_key, val, sizeof(listen_control_stop_key));

		if (!(val = tris_variable_retrieve(cfg, "general", "usedirectory"))) 
			val = "no";
		tris_set2_flag((&globalflags), tris_true(val), VM_DIRECFORWARD);	

		poll_freq = DEFAULT_POLL_FREQ;
		if ((val = tris_variable_retrieve(cfg, "general", "pollfreq"))) {
			if (sscanf(val, "%u", &poll_freq) != 1) {
				poll_freq = DEFAULT_POLL_FREQ;
				tris_log(LOG_ERROR, "'%s' is not a valid value for the pollfreq option!\n", val);
			}
		}

		poll_mailboxes = 0;
		if ((val = tris_variable_retrieve(cfg, "general", "pollmailboxes")))
			poll_mailboxes = tris_true(val);

		if (ucfg) {	
			for (cat = tris_category_browse(ucfg, NULL); cat; cat = tris_category_browse(ucfg, cat)) {
				if (!tris_true(tris_config_option(ucfg, cat, "hasvoicemail")))
					continue;
				if ((cur = find_or_create(userscontext, cat))) {
					populate_defaults(cur);
					apply_options_full(cur, tris_variable_browse(ucfg, cat));
					tris_copy_string(cur->context, userscontext, sizeof(cur->context));
				}
			}
			tris_config_destroy(ucfg);
		}
		cat = tris_category_browse(cfg, NULL);
		while (cat) {
			if (strcasecmp(cat, "general")) {
				var = tris_variable_browse(cfg, cat);
				if (strcasecmp(cat, "zonemessages")) {
					/* Process mailboxes in this context */
					while (var) {
						append_mailbox(cat, var->name, var->value);
						var = var->next;
					}
				} else {
					/* Timezones in this context */
					while (var) {
						struct vm_zone *z;
						if ((z = tris_malloc(sizeof(*z)))) {
							char *msg_format, *timezone;
							msg_format = tris_strdupa(var->value);
							timezone = strsep(&msg_format, "|");
							if (msg_format) {
								tris_copy_string(z->name, var->name, sizeof(z->name));
								tris_copy_string(z->timezone, timezone, sizeof(z->timezone));
								tris_copy_string(z->msg_format, msg_format, sizeof(z->msg_format));
								TRIS_LIST_LOCK(&zones);
								TRIS_LIST_INSERT_HEAD(&zones, z, list);
								TRIS_LIST_UNLOCK(&zones);
							} else {
								tris_log(LOG_WARNING, "Invalid timezone definition at line %d\n", var->lineno);
								tris_free(z);
							}
						} else {
							TRIS_LIST_UNLOCK(&users);
							tris_config_destroy(cfg);
							return -1;
						}
						var = var->next;
					}
				}
			}
			cat = tris_category_browse(cfg, cat);
		}
		memset(fromstring, 0, sizeof(fromstring));
		memset(pagerfromstring, 0, sizeof(pagerfromstring));
		strcpy(charset, "ISO-8859-1");
		if (emailbody) {
			tris_free(emailbody);
			emailbody = NULL;
		}
		if (emailsubject) {
			tris_free(emailsubject);
			emailsubject = NULL;
		}
		if (pagerbody) {
			tris_free(pagerbody);
			pagerbody = NULL;
		}
		if (pagersubject) {
			tris_free(pagersubject);
			pagersubject = NULL;
		}
		if ((val = tris_variable_retrieve(cfg, "general", "pbxskip")))
			tris_set2_flag((&globalflags), tris_true(val), VM_PBXSKIP);
		if ((val = tris_variable_retrieve(cfg, "general", "fromstring")))
			tris_copy_string(fromstring, val, sizeof(fromstring));
		if ((val = tris_variable_retrieve(cfg, "general", "pagerfromstring")))
			tris_copy_string(pagerfromstring, val, sizeof(pagerfromstring));
		if ((val = tris_variable_retrieve(cfg, "general", "charset")))
			tris_copy_string(charset, val, sizeof(charset));
		if ((val = tris_variable_retrieve(cfg, "general", "adsifdn"))) {
			sscanf(val, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x = 0; x < 4; x++) {
				memcpy(&adsifdn[x], &tmpadsi[x], 1);
			}
		}
		if ((val = tris_variable_retrieve(cfg, "general", "adsisec"))) {
			sscanf(val, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x = 0; x < 4; x++) {
				memcpy(&adsisec[x], &tmpadsi[x], 1);
			}
		}
		if ((val = tris_variable_retrieve(cfg, "general", "adsiver"))) {
			if (atoi(val)) {
				adsiver = atoi(val);
			}
		}
		if ((val = tris_variable_retrieve(cfg, "general", "emailsubject")))
			emailsubject = tris_strdup(val);
		if ((val = tris_variable_retrieve(cfg, "general", "emailbody"))) {
			char *tmpread, *tmpwrite;
			emailbody = tris_strdup(val);

			/* substitute strings \t and \n into the appropriate characters */
			tmpread = tmpwrite = emailbody;
			while ((tmpwrite = strchr(tmpread, '\\'))) {
				switch (tmpwrite[1]) {
				case 'r':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\r';
					break;
				case 'n':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\n';
					break;
				case 't':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\t';
					break;
				default:
					tris_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n", tmpwrite[1]);
				}
				tmpread = tmpwrite + 1;
			}
		}
		if ((val = tris_variable_retrieve(cfg, "general", "pagersubject")))
			pagersubject = tris_strdup(val);
		if ((val = tris_variable_retrieve(cfg, "general", "pagerbody"))) {
			char *tmpread, *tmpwrite;
			pagerbody = tris_strdup(val);

			/* substitute strings \t and \n into the appropriate characters */
			tmpread = tmpwrite = pagerbody;
			while ((tmpwrite = strchr(tmpread, '\\'))) {
				switch (tmpwrite[1]) {
				case 'r':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\r';
					break;
				case 'n':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\n';
					break;
				case 't':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\t';
					break;
				default:
					tris_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n", tmpwrite[1]);
				}
				tmpread = tmpwrite + 1;
			}
		}
		TRIS_LIST_UNLOCK(&users);
		tris_config_destroy(cfg);

		if (poll_mailboxes && poll_thread == TRIS_PTHREADT_NULL)
			start_poll_thread();
		if (!poll_mailboxes && poll_thread != TRIS_PTHREADT_NULL)
			stop_poll_thread();;

		return 0;
	} else {
		TRIS_LIST_UNLOCK(&users);
		tris_log(LOG_WARNING, "Failed to load configuration file.\n");
		if (ucfg)
			tris_config_destroy(ucfg);
		return 0;
	}
}

static int sayname(struct tris_channel *chan, const char *mailbox, const char *context)
{
	int res = -1;
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s%s/%s/greet", VM_SPOOL_DIR, context, mailbox);
	tris_debug(2, "About to try retrieving name file %s\n", dir);
	RETRIEVE(dir, -1, mailbox, context);
	if (tris_fileexists(dir, NULL, NULL)) {
		res = tris_stream_and_wait(chan, dir, TRIS_DIGIT_ANY);
	}
	DISPOSE(dir, -1);
	return res;
}

static int reload(void)
{
	return load_config(1);
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app);
	res |= tris_unregister_application(app2);
	res |= tris_unregister_application(app3);
	res |= tris_unregister_application(app4);
	res |= tris_custom_function_unregister(&mailbox_exists_acf);
	res |= tris_manager_unregister("VoicemailUsersList");
	tris_cli_unregister_multiple(cli_voicemail, sizeof(cli_voicemail) / sizeof(struct tris_cli_entry));
	tris_uninstall_vm_functions();

	if (poll_thread != TRIS_PTHREADT_NULL)
		stop_poll_thread();


	free_vm_users();
	free_vm_zones();
	return res;
}

static int load_module(void)
{
	int res;
	my_umask = umask(0);
	umask(my_umask);

	/* compute the location of the voicemail spool directory */
	snprintf(VM_SPOOL_DIR, sizeof(VM_SPOOL_DIR), "%s/voicemail/", tris_config_TRIS_SPOOL_DIR);

	if ((res = load_config(0)))
		return res;

	res = tris_register_application(app, vm_exec, synopsis_vm, descrip_vm);
	res |= tris_register_application(app2, vm_execmain, synopsis_vmain, descrip_vmain);
	res |= tris_register_application(app3, vm_box_exists, synopsis_vm_box_exists, descrip_vm_box_exists);
	res |= tris_register_application(app4, vmauthenticate, synopsis_vmauthenticate, descrip_vmauthenticate);
	res |= tris_register_application(app5, cmd_exec, synopsis_cmd, descrip_cmd);
	res |= tris_register_application(app6, rprt_exec, synopsis_rprt, descrip_rprt);
	res |= tris_register_application(app7, cmd_execmain, synopsis_cmdmain, descrip_cmdmain);
	res |= tris_register_application(app8, rprt_execmain, synopsis_rprtmain, descrip_rprtmain);

	res |= tris_custom_function_register(&mailbox_exists_acf);
	res |= tris_manager_register("VoicemailUsersList", EVENT_FLAG_CALL | EVENT_FLAG_REPORTING, manager_list_voicemail_users, "List All Voicemail User Information");
	if (res)
		return res;

	tris_cli_register_multiple(cli_voicemail, sizeof(cli_voicemail) / sizeof(struct tris_cli_entry));

	tris_install_vm_functions(has_voicemail, inboxcount, inboxcount2, messagecount, sayname, get_vmlist, manage_mailbox);

	return res;
}

static int dialout(struct tris_channel *chan, struct tris_vm_user *vmu, char *num, char *outgoing_context) 
{
	int cmd = 0;
	char destination[80] = "";
	int retries = 0;

	if (!num) {
		tris_verb(3, "Destination number will be entered manually\n");
		while (retries < 3 && cmd != 't') {
			destination[1] = '\0';
			destination[0] = cmd = tris_play_and_wait(chan, "voicemail/vm-enter-num-to-call");
			if (!cmd)
				destination[0] = cmd = tris_play_and_wait(chan, "voicemail/vm-then-pound");
			if (!cmd)
				destination[0] = cmd = tris_play_and_wait(chan, "voicemail/vm-star-cancel");
			if (!cmd) {
				cmd = tris_waitfordigit(chan, 6000);
				if (cmd)
					destination[0] = cmd;
			}
			if (!cmd) {
				retries++;
			} else {

				if (cmd < 0)
					return 0;
				if (cmd == '*') {
					tris_verb(3, "User hit '*' to cancel outgoing call\n");
					return 0;
				}
				if ((cmd = tris_readstring(chan, destination + strlen(destination), sizeof(destination)-1, 6000, 10000, "#")) < 0) 
					retries++;
				else
					cmd = 't';
			}
		}
		if (retries >= 3) {
			return 0;
		}
		
	} else {
		tris_verb(3, "Destination number is CID number '%s'\n", num);
		tris_copy_string(destination, num, sizeof(destination));
	}

	if (!tris_strlen_zero(destination)) {
		if (destination[strlen(destination) -1 ] == '*')
			return 0; 
		tris_verb(3, "Placing outgoing call to extension '%s' in context '%s' from context '%s'\n", destination, outgoing_context, chan->context);
		tris_copy_string(chan->exten, destination, sizeof(chan->exten));
		tris_copy_string(chan->context, outgoing_context, sizeof(chan->context));
		chan->priority = 0;
		return 9;
	}
	return 0;
}

static int advanced_options(struct tris_channel *chan, struct tris_vm_user *vmu, struct vm_state *vms, int msg, int option, signed char record_gain)
{
	int res = 0;
	char filename[PATH_MAX];
	struct tris_config *msg_cfg = NULL;
	const char *origtime, *context;
	char *name, *num;
	int retries = 0;
	char *cid;
	struct tris_flags config_flags = { CONFIG_FLAG_NOCACHE, };

	vms->starting = 0; 

	make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);

	/* Retrieve info from VM attribute file */

	make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
	snprintf(filename, sizeof(filename), "%s.txt", vms->fn2);
	RETRIEVE(vms->curdir, vms->curmsg, vmu->mailbox, vmu->context);
	msg_cfg = tris_config_load(filename, config_flags);
	DISPOSE(vms->curdir, vms->curmsg);
	if (!msg_cfg) {
		tris_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
		return 0;
	}

	if (!(origtime = tris_variable_retrieve(msg_cfg, "message", "origtime"))) {
		tris_config_destroy(msg_cfg);
		return 0;
	}

	cid = tris_strdupa(tris_variable_retrieve(msg_cfg, "message", "callerid"));

	context = tris_variable_retrieve(msg_cfg, "message", "context");
	if (!strncasecmp("macro", context, 5)) /* Macro names in contexts are useless for our needs */
		context = tris_variable_retrieve(msg_cfg, "message", "macrocontext");
	switch (option) {
	case 3:
		if (!res)
			res = play_message_datetime(chan, vmu, origtime, filename);
		if (!res)
			res = play_message_callerid(chan, vms, cid, context, 0);

		res = 't';
		break;

	case 2:	/* Call back */

		if (tris_strlen_zero(cid))
			break;

		tris_callerid_parse(cid, &name, &num);
		while ((res > -1) && (res != 't')) {
			switch (res) {
			case '1':
				if (num) {
					/* Dial the CID number */
					res = dialout(chan, vmu, num, vmu->callback);
					if (res) {
						tris_config_destroy(msg_cfg);
						return 9;
					}
				} else {
					res = '2';
				}
				break;

			case '2':
				/* Want to enter a different number, can only do this if there's a dialout context for this user */
				if (!tris_strlen_zero(vmu->dialout)) {
					res = dialout(chan, vmu, NULL, vmu->dialout);
					if (res) {
						tris_config_destroy(msg_cfg);
						return 9;
					}
				} else {
					tris_verb(3, "Caller can not specify callback number - no dialout context available\n");
					res = tris_play_and_wait(chan, "voicemail/vm-sorry");
				}
				tris_config_destroy(msg_cfg);
				return res;
			case '*':
				res = 't';
				break;
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			case '0':

				res = tris_play_and_wait(chan, "voicemail/vm-sorry");
				retries++;
				break;
			default:
				if (num) {
					tris_verb(3, "Confirm CID number '%s' is number to use for callback\n", num);
					res = tris_play_and_wait(chan, "voicemail/vm-num-i-have");
					if (!res)
						res = play_message_callerid(chan, vms, num, vmu->context, 1);
					if (!res)
						res = tris_play_and_wait(chan, "voicemail/vm-tocallnum");
					/* Only prompt for a caller-specified number if there is a dialout context specified */
					if (!tris_strlen_zero(vmu->dialout)) {
						if (!res)
							res = tris_play_and_wait(chan, "voicemail/vm-calldiffnum");
					}
				} else {
					res = tris_play_and_wait(chan, "voicemail/vm-nonumber");
					if (!tris_strlen_zero(vmu->dialout)) {
						if (!res)
							res = tris_play_and_wait(chan, "voicemail/vm-toenternumber");
					}
				}
				if (!res)
					res = tris_play_and_wait(chan, "voicemail/vm-star-cancel");
				if (!res)
					res = tris_waitfordigit(chan, 6000);
				if (!res) {
					retries++;
					if (retries > 3)
						res = 't';
				}
				break; 
				
			}
			if (res == 't')
				res = 0;
			else if (res == '*')
				res = -1;
		}
		break;
		
	case 1:	/* Reply */
		/* Send reply directly to sender */
		if (tris_strlen_zero(cid))
			break;

		tris_callerid_parse(cid, &name, &num);
		if (!num) {
			tris_verb(3, "No CID number available, no reply sent\n");
			if (!res)
				res = tris_play_and_wait(chan, "voicemail/vm-nonumber");
			tris_config_destroy(msg_cfg);
			return res;
		} else {
			struct tris_vm_user vmu2;
			if (find_user(&vmu2, vmu->context, num)) {
				struct leave_vm_options leave_options;
				char mailbox[TRIS_MAX_EXTENSION * 2 + 2];
				snprintf(mailbox, sizeof(mailbox), "%s@%s", num, vmu->context);

				tris_verb(3, "Leaving voicemail for '%s' in context '%s'\n", num, vmu->context);
				
				memset(&leave_options, 0, sizeof(leave_options));
				leave_options.record_gain = record_gain;
				res = leave_voicemail(chan, mailbox, &leave_options);
				if (!res)
					res = 't';
				tris_config_destroy(msg_cfg);
				return res;
			} else {
				/* Sender has no mailbox, can't reply */
				tris_verb(3, "No mailbox number '%s' in context '%s', no reply sent\n", num, vmu->context);
				tris_play_and_wait(chan, "voicemail/vm-nobox");
				res = 't';
				tris_config_destroy(msg_cfg);
				return res;
			}
		} 
		res = 0;

		break;
	}

#ifndef IMAP_STORAGE
	tris_config_destroy(msg_cfg);

	if (!res) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);
		vms->heard[msg] = 1;
		res = wait_file(chan, vms, vms->fn);
	}
#endif
	return res;
}

static int store_vmfile(struct tris_channel *chan, char *tempfile, char *context, char *mailbox, char *ext, int duration, char *fmt)
{
	char priority[16];
	char origtime[16];
	char date[256];
	int rtmsgid = 0;
	const char *category = NULL;
	struct tris_vm_user *vmu, vmus;
	FILE *txt;
	char dir[PATH_MAX], tmpdir[PATH_MAX];
	char fn[PATH_MAX];
	char txtfile[PATH_MAX], tmptxtfile[PATH_MAX];
	char tmp[256];
	char callerid[256];
	int msgnum = 0;
	int txtdes;
//	char sql[256], uid[32];


	/* Context and category */
	tris_copy_string(tmp, mailbox, sizeof(tmp));
	if (tris_strlen_zero(context)) {
		context = strchr(tmp, '@');
	}
	category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");

	tris_debug(3, "Before find_user\n");
	if (!(vmu = create_user(&vmus, context, mailbox))) {
			return 0;
	}

	if(!strcasecmp(vmu->context, "report")) {
		char today[256];
		get_date(today, sizeof(today), "%Y%m%d");
//		strcat(mailbox, today);
		sprintf(mailbox, "%s/%s", mailbox, today);
	}
	create_dirpath(dir, sizeof(dir), vmu->context, mailbox, "INBOX");

	/* Message count full ? 
	if (count_messages(vmu, dir) >= vmu->maxmsg) {
		res = tris_streamfile(chan, "voicemail/vm-mailboxfull", chan->language);
		if (!res)
			res = tris_waitstream(chan, "");
		tris_log(LOG_WARNING, "No more messages possible\n");
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		return 0;
	} */

	/* Real filename */
	msgnum = last_message_index(vmu, dir) + 1;
	if(msgnum >= vmu->maxmsg) 
		msgnum = vmu->maxmsg - 1;
	make_file(fn, sizeof(fn), dir, msgnum);
	
	/* Store information into txt file 
	if (txtdes < 0) {
		res = tris_streamfile(chan, "voicemail/vm-mailboxfull", chan->language);
		if (!res)
			res = tris_waitstream(chan, "");
		tris_log(LOG_ERROR, "Unable to create message file: %s\n", strerror(errno));
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		return 0;
	} */

	/* Get uid by extension */
//	snprintf(sql, sizeof(sql), "SELECT uid FROM uri WHERE extension='%s'", ext);
//	sql_select_query_execute(uid, sql);

	/* Store information in real-time storage */
	if (tris_check_realtime("voicemail_data")) {
		snprintf(priority, sizeof(priority), "%d", chan->priority);
		snprintf(origtime, sizeof(origtime), "%ld", (long)time(NULL));
		get_date(date, sizeof(date), "%F %T");
		rtmsgid = tris_store_realtime("voicemail_data", "origmailbox", mailbox, "context", chan->context, "macrocontext", chan->macrocontext, "exten", chan->exten, "priority", priority, "callerchan", chan->name, "callerid", tris_callerid_merge(callerid, sizeof(callerid), chan->cid.cid_name, chan->cid.cid_num, "Unknown"), "origdate", date, "origtime", origtime, "category", category ? category : "", NULL);
	}

	/* Store information */
	create_dirpath(tmpdir, sizeof(tmpdir), vmu->context, mailbox, "tmp");
	snprintf(tmptxtfile, sizeof(tmptxtfile), "%s/XXXXXX", tmpdir);
	txtdes = mkstemp(tmptxtfile);
	chmod(tmptxtfile, VOICEMAIL_FILE_MODE & ~my_umask);
	
	txt = fdopen(txtdes, "w+");
	if (txt) {
		get_date(date, sizeof(date),"%F %T");
		fprintf(txt, 
			";\n"
			"; Message Information file\n"
			";\n"
			"[message]\n"
			"origmailbox=%s\n"
			"context=%s\n"
			"macrocontext=%s\n"
			"exten=%s\n"
			"priority=%d\n"
			"callerchan=%s\n"
			"callerid=%s\n"
			"origdate=%s\n"
			"origtime=%ld\n"
			"category=%s\n",
			mailbox,
			vmu->context,
			chan->macrocontext, 
			chan->exten,
			chan->priority,
			chan->name,
			tris_callerid_merge(callerid, sizeof(callerid), S_OR(chan->cid.cid_name, NULL), S_OR(chan->cid.cid_num, NULL), "Unknown"),
			date, (long)time(NULL),
			category ? category : ""); 
	}
	fprintf(txt, "duration=%d\n", duration);
	fclose(txt);

	snprintf(txtfile, sizeof(txtfile), "%s.txt", fn);
	rename(tmptxtfile, txtfile);

	/* Otherwise 1 is to save the existing message */
	tris_verb(3, "Saving message as is\n");

	/* Store message 
		copy instead rename for multi user 
	tris_filerename(tempfile, fn, NULL); */
	tris_filecopy(tempfile, fn, NULL);
	STORE(dir, vmu->mailbox, vmu->context, msgnum, chan, vmu, fmt, duration, vms);

	free_user(vmu);

	return 1;
}

static int play_record_review(struct tris_channel *chan, char *playfile, char *origfile, int maxtime, char *fmt,
			int outsidecaller, struct tris_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms)
{
	/* Record message & let caller review or re-record it, or set options if applicable */
	int res = 0;
	int cmd = 0;
	int max_attempts = 3;
	int attempts = 0;
	int retry_dial = 0;
	int recorded = 0;
	int message_exists = 0;
	int message_saved = 0;
	signed char zero_gain = 0;
	char tempfile[PATH_MAX];
	char *acceptdtmf = "#";
	char *canceldtmf = "";
	char *ext, *callerid;
	const char *errcode;
	char tmp[256];

	/* Note that urgent and private are for flagging messages as such in the future */

	/* barf if no pointer passed to store duration in */
	if (duration == NULL) {
		tris_log(LOG_WARNING, "Error play_record_review called without duration pointer\n");
		return -1;
	}

	if (!outsidecaller)		
		snprintf(tempfile, sizeof(tempfile), "%sXXXXXXX", VM_SPOOL_DIR);
	else {
		snprintf(tempfile, sizeof(tempfile), "%sXXXXXXX", VM_SPOOL_DIR);
		int fd = mkstemp(tempfile);
		close(fd);
		unlink(tempfile); 
	}

	cmd = '3';  /* Want to start by recording */

	while ((cmd >= 0) && (cmd != 't')) {
		switch (cmd) {
		case '1':
			if (!message_saved) {
			/* Review */
				tris_verb(3, "Reviewing the message\n");
				tris_stream_and_wait(chan, tempfile, TRIS_DIGIT_ANY);
				cmd = 0;
				break;
			} else {
				/* Get extn */
				retry_dial = 0;
				cmd = '2';
				break;
			}
		case '2':
			if (!message_exists) {
				/* In this case, 1 is to record a message */
				cmd = '3';
				break;
			} else {
				/* Get extn */
				if (vmu) {
					ext = tris_strdupa(vmu->mailbox);
				} else {
					res = tris_app_getdata(chan, "voicemail/dial_extn_pound", tmp, sizeof(tmp) - 1, 0);
/*					if (res < 0) {
						tris_filedelete(tempfile, NULL);
						return res;
					}
*/
					while(retry_dial < maxlogins && !vm_user_exist(tmp)) {
					
						if(!tris_strlen_zero(tmp)){
							tris_stream_and_wait(chan, "voicemail/is_not_found", "");
						}
						
						retry_dial++;
						if(retry_dial >= maxlogins) {
							tris_play_and_wait(chan, "goodbye");
							tris_filedelete(tempfile, NULL);
							return 0;
						}
						
						res = tris_app_getdata(chan, "voicemail/dial_extn_pound", tmp, sizeof(tmp) - 1, 0);

						if (tris_waitstream(chan, "")) {	/* Channel is hung up */
							tris_verbose("   ## \n");
							tris_filedelete(tempfile, NULL);
							return -1;
						}

					}
					ext = tris_strdupa(tmp);
				}
				callerid = tris_strdupa(chan->cid.cid_num);

				/* Store voicemail file */
				if (!store_vmfile(chan, tempfile, NULL, ext, callerid, *duration, fmt)) {
					tris_log(LOG_WARNING, "No entry in uri table for '%s'\n", ext);
					pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
					tris_stream_and_wait(chan, "voicemail/extn_invalid", "");
					cmd = '2';
				} else {
					tris_stream_and_wait(chan, "voicemail/msg_sent", "");
					errcode = pbx_builtin_getvar_helper(chan, "Error-Info");
					if(!tris_strlen_zero(errcode)) {
						message_saved = 1;
						cmd = 't';
					} else {
						message_saved = 1;
						cmd = 0;
					}
				}
				break;
			}
		case '3':
			message_exists = 0;
			/* Record */
			if (recorded == 1) 
				tris_verb(3, "Re-recording the message\n");
			else	
				tris_verb(3, "Recording the message\n");
			
			if (recorded && outsidecaller) {
				cmd = tris_play_and_wait(chan, INTRO);
				cmd = tris_play_and_wait(chan, "beep");
			}
			recorded = 1;
			/* After an attempt has been made to record message, we have to take care of INTRO and beep for incoming messages, but not for greetings */
			if (record_gain)
				tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);
//			if (tris_test_flag(vmu, VM_OPERATOR)){
//				canceldtmf = "0";
//			}
			cmd = tris_play_and_record_full(chan, playfile, tempfile, maxtime, fmt, duration, silencethreshold, maxsilence, unlockdir, acceptdtmf, canceldtmf);
			if (record_gain)
				tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);
			if (cmd == -1) {
				/* User has hung up, no options to give */
				tris_filedelete(tempfile, NULL);
				return cmd;
			}
			if (cmd == '0') {
				break;
			} else if (cmd == '*') {
				break;
#if 0
			} else if (vmu->review && (*duration < 5)) {
				/* Message is too short */
				tris_verb(3, "Message too short\n");
				cmd = tris_play_and_wait(chan, "voicemail/vm-tooshort");
				cmd = tris_filedelete(tempfile, NULL);
				break;
			} else if (vmu->review && (cmd == 2 && *duration < (maxsilence + 3))) {
				/* Message is all silence */
				tris_verb(3, "Nothing recorded\n");
				cmd = tris_filedelete(tempfile, NULL);
				cmd = tris_play_and_wait(chan, "voicemail/vm-nothingrecorded");
				if (!cmd)
					cmd = tris_play_and_wait(chan, "voicemail/vm-speakup");
				break;
#endif
			} else {
				/* If all is well, a message exists */
				message_exists = 1;
				cmd = 0;
			}
			break;
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		case '*':
			tris_filedelete(tempfile, NULL);
			tris_play_and_wait(chan, "goodbye");
			return 0;
		case '#':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
#if 0 
/*  XXX Commented out for the moment because of the dangers of deleting
    a message while recording (can put the message numbers out of sync) */
		case '*':
			/* Cancel recording, delete message, offer to take another message*/
			cmd = tris_play_and_wait(chan, "voicemail/vm-deleted");
			cmd = tris_filedelete(tempfile, NULL);
			if (outsidecaller) {
				res = vm_exec(chan, NULL);
				return res;
			}
			else
				return 1;
#endif
		/*
		case '0':
			if (!tris_test_flag(vmu, VM_OPERATOR)) {
				cmd = tris_play_and_wait(chan, "voicemail/vm-sorry");
				break;
			}
			if (message_exists || recorded) {
				cmd = tris_play_and_wait(chan, "voicemail/vm-saveoper");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 3000);
				if (cmd == '1') {
					tris_play_and_wait(chan, "voicemail/vm-msgsaved");
					cmd = '0';
				} else {
					tris_play_and_wait(chan, "voicemail/vm-deleted");
					DELETE(tempfile, -1, tempfile, vmu);
					cmd = '0';
				}
			}
			return cmd;*/
		default:
			/* If the caller is an ouside caller, and the review option is enabled,
			   allow them to review the message, but let the owner of the box review
			   their OGM's */
			/*if (!outsidecaller && !tris_test_flag(vmu, VM_REVIEW))
				return cmd;*/
			if (message_saved) {
				cmd = tris_play_and_wait(chan, "voicemail/to_deliver_to_another_address");
				if(!cmd)
					cmd = tris_waitfordigit(chan, 5000);
				break;
			} else {
				if (message_exists) {
					cmd = tris_play_and_wait(chan, "voicemail/deposit_options");
					if (!cmd)
						cmd = tris_waitfordigit(chan, 5000);
					if (!cmd)
						goto check;
					break;
				} else {
					cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
					if (!cmd)
						cmd = tris_waitfordigit(chan, 5000);
				}
			}
			
			if (!cmd && outsidecaller && tris_test_flag(vmu, VM_OPERATOR)) {
				cmd = tris_play_and_wait(chan, "voicemail/vm-reachoper");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 5000);
			}
#if 0
			if (!cmd)
				cmd = tris_play_and_wait(chan, "voicemail/vm-tocancelmsg");
#endif
			if (!cmd)
				cmd = tris_waitfordigit(chan, 6000);
check:
			if (!cmd) {
				attempts++;
			}
			if (attempts > max_attempts) {
				cmd = 't';
			}
		}
	}

	tris_filedelete(tempfile, NULL);
	if (outsidecaller)
		tris_play_and_wait(chan, "voicemail/bye");
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

static int play_record_review_cmd(struct tris_channel *chan, char *playfile, char *origfile, int maxtime, char *fmt,
			int outsidecaller, struct tris_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms)
{
	/* Record message & let caller review or re-record it, or set options if applicable */
	int cmd = 0;
	int max_attempts = 3;
	int attempts = 0;
	int recorded = 0;
	int message_exists = 0;
	signed char zero_gain = 0;
	char tempfile[PATH_MAX];
	char *acceptdtmf = "#";
	char *canceldtmf = "";
	char *context, *ext, *callerid;

	/* Note that urgent and private are for flagging messages as such in the future */

	/* barf if no pointer passed to store duration in */
	if (duration == NULL) {
		tris_log(LOG_WARNING, "Error play_record_review called without duration pointer\n");
		return -1;
	}

	if (!outsidecaller)		
		snprintf(tempfile, sizeof(tempfile), "%sXXXXXXX", VM_SPOOL_DIR);
	else {
		snprintf(tempfile, sizeof(tempfile), "%sXXXXXXX", VM_SPOOL_DIR);
		int fd = mkstemp(tempfile);
		close(fd);
		unlink(tempfile); 
	}

	cmd = '3';  /* Want to start by recording */
	context = tris_strdupa("cmd");

	while ((cmd >= 0) && (cmd != 't')) {
		switch (cmd) {
		case '1':
			/* Review */
			tris_verb(3, "Reviewing the command\n");
			cmd = tris_stream_and_wait(chan, tempfile, TRIS_DIGIT_ANY);
			break;
		case '2':
			if (!message_exists) {
				/* In this case, 1 is to record a message */
				cmd = '3';
				break;
			} else {
				ext = tris_strdupa(vmu->mailbox);
				callerid = tris_strdupa(chan->cid.cid_num);

				/* Store voicemail file */
				if (!store_vmfile(chan, tempfile, context, ext, callerid, *duration, fmt)) {
					tris_log(LOG_WARNING, "No entry in uri table for '%s'\n", ext);
					pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
					cmd = '2';
				} else {
					tris_play_and_wait(chan, "voicemail/cmd_sent");
					goto cmd_success;
				}
				break;
			}
		case '3':
			message_exists = 0;
			/* Record */
			if (recorded == 1) 
				tris_verb(3, "Re-recording the command\n");
			else	
				tris_verb(3, "Recording the command\n");
			
			if (recorded && outsidecaller) {
				cmd = tris_play_and_wait(chan, "voicemail/cmd_record_msg");
				cmd = tris_play_and_wait(chan, "beep");
			}
			recorded = 1;
			/* After an attempt has been made to record message, we have to take care of INTRO and beep for incoming messages, but not for greetings */
			if (record_gain)
				tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);
			cmd = tris_play_and_record_full(chan, playfile, tempfile, maxtime, fmt, duration, silencethreshold, maxsilence, unlockdir, acceptdtmf, canceldtmf);
			if (record_gain)
				tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);
			if (cmd == -1) {
				/* User has hung up, no options to give */
				tris_filedelete(tempfile, NULL);
				return cmd;
			}
			if (cmd == '0') {
				break;
			} else if (cmd == '*') {
				break;
			} else {
				/* If all is well, a message exists */
				message_exists = 1;
				cmd = 0;
			}
			break;
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		case '*':
			tris_filedelete(tempfile, NULL);
			tris_play_and_wait(chan, "voicemail/bye");
			return 0;
		case '#':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		default:
			/* If the caller is an ouside caller, and the review option is enabled,
			   allow them to review the message, but let the owner of the box review
			   their OGM's */
			/*if (!outsidecaller && !tris_test_flag(vmu, VM_REVIEW))
				return cmd;*/
			if (message_exists) {
				cmd = tris_play_and_wait(chan, "voicemail/cmd_deposit_options");
				break;
			} else {
				cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 6000);
			}
			
			if (!cmd && outsidecaller && tris_test_flag(vmu, VM_OPERATOR)) {
				cmd = tris_play_and_wait(chan, "voicemail/vm-reachoper");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 6000);
			}
			if (!cmd)
				cmd = tris_waitfordigit(chan, 6000);
			if (!cmd) {
				attempts++;
			}
			if (attempts > max_attempts) {
				cmd = 't';
			}
		}
	}

cmd_success:
	tris_filedelete(tempfile, NULL);
	if (outsidecaller)
		tris_play_and_wait(chan, "voicemail/bye");
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

static int play_record_review_rprt(struct tris_channel *chan, char *playfile, char *origfile, int maxtime, char *fmt,
			int outsidecaller, struct tris_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms, char *ext)
{
	/* Record message & let caller review or re-record it, or set options if applicable */
	int cmd = 0;
	int max_attempts = 3;
	int attempts = 0;
	int recorded = 0;
	int message_exists = 0;
	signed char zero_gain = 0;
	char tempfile[PATH_MAX];
	char *acceptdtmf = "#";
	char *canceldtmf = "";
	char *context;

	/* Note that urgent and private are for flagging messages as such in the future */

	/* barf if no pointer passed to store duration in */
	if (duration == NULL) {
		tris_log(LOG_WARNING, "Error play_record_review called without duration pointer\n");
		return -1;
	}

	if (!outsidecaller)		
		snprintf(tempfile, sizeof(tempfile), "%sXXXXXXX", VM_SPOOL_DIR);
	else {
		snprintf(tempfile, sizeof(tempfile), "%sXXXXXXX", VM_SPOOL_DIR);
		int fd = mkstemp(tempfile);
		close(fd);
		unlink(tempfile); 
	}

	cmd = '3';  /* Want to start by recording */
	context = tris_strdupa("report");

	while ((cmd >= 0) && (cmd != 't')) {
		switch (cmd) {
		case '1':
			/* Review */
			tris_verb(3, "Reviewing the command\n");
			cmd = tris_stream_and_wait(chan, tempfile, TRIS_DIGIT_ANY);
			break;
		case '2':
			if (!message_exists) {
				/* In this case, 1 is to record a message */
				cmd = '3';
				break;
			} else {
				/* Store voicemail file */
				if (!store_vmfile(chan, tempfile, context, vmu->mailbox, ext, *duration, fmt)) {
					tris_log(LOG_WARNING, "No entry in uri table for '%s'\n", ext);
					pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
					cmd = '2';
				} else {
					tris_play_and_wait(chan, "voicemail/rprt_msg_sent");
					goto rprt_success;
				}
				break;
			}
		case '3':
			message_exists = 0;
			/* Record */
			if (recorded == 1) 
				tris_verb(3, "Re-recording the command\n");
			else	
				tris_verb(3, "Recording the command\n");
			
			if (recorded && outsidecaller) {
				cmd = tris_play_and_wait(chan, "voicemail/rprt_record_report");
				cmd = tris_play_and_wait(chan, "beep");
			}
			recorded = 1;
			/* After an attempt has been made to record message, we have to take care of INTRO and beep for incoming messages, but not for greetings */
			if (record_gain)
				tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);
			cmd = tris_play_and_record_full(chan, playfile, tempfile, maxtime, fmt, duration, silencethreshold, maxsilence, unlockdir, acceptdtmf, canceldtmf);
			if (record_gain)
				tris_channel_setoption(chan, TRIS_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);
			if (cmd == -1) {
				/* User has hung up, no options to give */
				tris_filedelete(tempfile, NULL);
				return cmd;
			}
			if (cmd == '0') {
				break;
			} else if (cmd == '*') {
				break;
			} else {
				/* If all is well, a message exists */
				message_exists = 1;
				cmd = 0;
			}
			break;
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		case '*':
			tris_filedelete(tempfile, NULL);
			tris_play_and_wait(chan, "voicemail/bye");
			return 0;
		case '#':
			cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
			break;
		default:
			/* If the caller is an ouside caller, and the review option is enabled,
			   allow them to review the message, but let the owner of the box review
			   their OGM's */
			/*if (!outsidecaller && !tris_test_flag(vmu, VM_REVIEW))
				return cmd;*/
			if (message_exists) {
				cmd = tris_play_and_wait(chan, "voicemail/rprt_deposit_options");
				break;
			} else {
				cmd = tris_play_and_wait(chan, "voicemail/invalid_entry_try_again");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 6000);
			}
			
			if (!cmd && outsidecaller && tris_test_flag(vmu, VM_OPERATOR)) {
				cmd = tris_play_and_wait(chan, "voicemail/vm-reachoper");
				if (!cmd)
					cmd = tris_waitfordigit(chan, 6000);
			}
			if (!cmd)
				cmd = tris_waitfordigit(chan, 6000);
			if (!cmd) {
				attempts++;
			}
			if (attempts > max_attempts) {
				cmd = 't';
			}
		}
	}

rprt_success:
	tris_filedelete(tempfile, NULL);
	if (outsidecaller)
		tris_play_and_wait(chan, "voicemail/bye");
	if (cmd == 't')
		cmd = 0;
	return cmd;
}



/* This is a workaround so that menuselect displays a proper description
 * TRIS_MODULE_INFO(, , "Comedian Mail (Voicemail System)"
 */

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, tdesc,
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
		);
