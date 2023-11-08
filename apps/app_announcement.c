
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 160392 $")

#include "trismedia/paths.h"	/* use tris_config_TRIS_SPOOL_DIR */
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <dirent.h>

#include "trismedia/cdr.h"
#include "trismedia/res_odbc.h"

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
//#include "trismedia/event.h"


#define CONFIG_FILE_NAME "rate.conf"

#define MAX_UID_LEN 8

static const char *app_ann = "Announcement";
static const char *app_prompt = "PromptMsg";
static const char *app_playopera = "PlayOpera";
static const char *app_autoattend = "AutoAttendance";

static const char *synopsis_ann = "Announcement Service";
static const char *synopsis_prompt = "Prompt Msg";
static const char *synopsis_playopera = "Play Opera";
static const char *synopsis_autoattend = "Play Auto Attendance";


static const char *descrip_ann =
"  Announcement: Annuncement Service\n"
"";
static const char *descrip_prompt =
"  CheckRate(Exten): Plays a message for specified exten\n"
"";
static const char *descrip_playopera =
"  PlayOpera : Plays a opera message\n"
"";
static const char *descrip_autoattend =
"  PlayOpera : Plays an auto attendance\n"
"";

static int repeatcount = 2;

static char ann_table[80] = "announcement";

static const char config[] = "announcement.conf";

static int play_file(struct tris_channel *chan, const char *filename)
{
	int res;

	if (tris_strlen_zero(filename)) {
		return 0;
	}

	tris_stopstream(chan);

	res = tris_streamfile(chan, filename, chan->language);
	if (!res)
		res = tris_waitstream(chan, "");

	tris_stopstream(chan);

	return res;
}

static int playopera_exec(struct tris_channel *chan, void *data)
{
	char playfile[80]={0};
	const char *exten;
	int i,j;
	int exten_count;
	
	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);
	
	exten = pbx_builtin_getvar_helper(chan, "XFERTO");
	if (!exten){
		tris_log(LOG_WARNING, "exten is empty.\n");
		return 0;
	}	

	// say 
	play_file(chan, "announcement/saynum");
	
	exten_count = strlen(exten);
	for (i=0; i<repeatcount; i++) {
		for (j=0; j<exten_count; j++) {
			switch (exten[j]) {
				case '0':
					snprintf(playfile, sizeof(playfile), "digits/0");break;
				case '1':
					snprintf(playfile, sizeof(playfile), "digits/1");break;
				case '2':
					snprintf(playfile, sizeof(playfile), "digits/2");break;
				case '3':
					snprintf(playfile, sizeof(playfile), "digits/3");break;
				case '4':
					snprintf(playfile, sizeof(playfile), "digits/4");break;
				case '5':
					snprintf(playfile, sizeof(playfile), "digits/5");break;
				case '6':
					snprintf(playfile, sizeof(playfile), "digits/6");break;
				case '7':
					snprintf(playfile, sizeof(playfile), "digits/7");break;
				case '8':
					snprintf(playfile, sizeof(playfile), "digits/8");break;
				case '9':
					snprintf(playfile, sizeof(playfile), "digits/9");break;
				default:
					snprintf(playfile, sizeof(playfile), "%s", "");break;
			}
			play_file(chan, playfile);
			if (exten_count==7 && j==2) play_file(chan, "announcement/empty");			
			if (exten_count==5 && j==1) play_file(chan, "announcement/empty");			
		}	
		if (i<repeatcount-1)
			play_file(chan, "announcement/sayagain");
	}	
	play_file(chan, "announcement/goodbye");

	return 0;
}


static int setup_announcement(int reload){
	struct tris_config *cfg;
	const char *tmp;
	struct tris_flags config_flags = { reload == 1 ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	cfg = tris_config_load(config, config_flags);

	/* Error if we have no config file */
	if (!cfg) {
		tris_log(LOG_ERROR, "Unable to load config %s\n", config);
		return -1;
	} 
	if (cfg == CONFIG_STATUS_FILEUNCHANGED) 
		return -1;

	 if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "File %s cannot be parsed.  Aborting.\n", config);
		return -1;
	}

	tmp = tris_variable_retrieve(cfg,"general","repeatcount");
	
	if (tmp != NULL){
		repeatcount = atoi(tmp);
	}

	tris_config_destroy(cfg);

	return 0;
}


static void replace_ann_pin(char *exten, char *pin)
{
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	struct odbc_obj *obj;
	char *argv[] = {pin, exten};
	struct generic_prepare_struct gps = { .sql = sql, .argc = 2, .argv = argv };
	tris_verbose(" ==(announcement service) == %s, %s\n",exten, pin);
	obj = tris_odbc_request_obj(tris_database, 0);
	if (obj) {
		snprintf(sql, sizeof(sql), "UPDATE %s SET password=? WHERE itemkey=?", ann_table);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt)
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
		else
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", tris_database);
	return;	
}


static void change_pin(struct tris_channel *chan, char *exten)
{
	char sql[256], old_password[40] = "";
	char new_password[40] = "", password[40]="";
	int login = 0, i, res=0, valid=0;
	
	snprintf(sql, sizeof(sql), "SELECT password FROM announcement WHERE itemkey='%s'", exten);
	sql_select_query_execute(old_password, sql);
	if(!tris_strlen_zero(old_password)) {
		for(i=0; i<3; i++) {
			res = tris_app_getdata(chan, "announcement/enter_old_pin", password, sizeof(password)-1, 5000);
			if(!tris_strlen_zero(password)) {
				if(!strcmp(old_password, password)) {
					login=1;
					break;
				}else {
					tris_stream_and_wait(chan, "announcement/invalid_old_pin", "");
				}
			} 
		}
	}else {
		login = 1;
	}

	if(!login) {
		return;
	}
	for(i=0; i<3; i++) {

		res = tris_app_getdata(chan, "announcement/enter_new_pin", new_password, sizeof(new_password)-1, 5000);
		if(!tris_strlen_zero(new_password)) {
			res = tris_app_getdata(chan, "announcement/enter_new_pin_again", password, sizeof(password)-1, 5000);
			if(!strcmp(new_password, password)) {
				valid = 1;
				break;
			}else {
				tris_stream_and_wait(chan, "announcement/invalid_pin","");
			}
		}
	}

	if(!valid) {
		tris_play_and_wait(chan, "announcement/pin_not_changed");
	} else {
		replace_ann_pin(exten, new_password);
		tris_play_and_wait(chan, "announcement/pin_changed");
	}
	
}

static void record_announce(struct tris_channel *chan, const char *exten)
{
	char sql[256], old_password[40] = "";
	char password[40]="";
	char tempfile[PATH_MAX], dest_file[PATH_MAX];
	int login = 0, i, res=0, duration = 0;
	
	snprintf(sql, sizeof(sql), "SELECT password FROM announcement WHERE itemkey='%s'", exten);
	sql_select_query_execute(old_password, sql);
	if(!tris_strlen_zero(old_password)) {
		for(i=0; i<3; i++) {
			res = tris_app_getdata(chan, "announcement/enter_pin", password, sizeof(password)-1, 5000);
			if(!tris_strlen_zero(password)) {
				if(!strcmp(old_password, password)) {
					login=1;
					break;
				}else {
					tris_stream_and_wait(chan, "announcement/invalid_pin", "");
				}
			} 
		}
	}else {
		login = 1;
	}

	if(!login) {
		return;
	}

	snprintf(dest_file, sizeof(dest_file), "%s/sounds/%s/announcement/ann_%s",tris_config_TRIS_VAR_DIR,chan->language,exten);
	snprintf(tempfile, sizeof(tempfile), "%s/XXXXXXX", tris_config_TRIS_VAR_DIR);
	res = tris_play_and_record(chan, "announcement/record_announcement", tempfile, 0, "wav", &duration, 128, 0, NULL);
	res= 0;
	while(1) {
		switch(res) {
		case '1':
			res = tris_play_and_wait(chan, tempfile);
			break;
		case '2':
			tris_filerename(tempfile, dest_file, NULL);
			tris_play_and_wait(chan, "announcement/announce_restored");
			return;
		case '3':
			tris_play_and_record(chan, "announcement/record_announcement", tempfile, 0, "wav", &duration, 128, 0, NULL);
			res = 0;
			break;
		case '*':
			if(tris_fileexists(tempfile, "wav", NULL)>0)
				tris_filedelete(tempfile, "wav");
			tris_play_and_wait(chan, "goodbye");
			return;
		default:
			res = tris_play_and_wait(chan, "announcement/ann_deposit_options");
			if(!res) 
				res = tris_waitfordigit(chan, 5000);
			break;


		}
	}
	if(tris_fileexists(tempfile, "wav", NULL)>0)
		tris_filedelete(tempfile, "wav");

}
static int prompt_exec(struct tris_channel *chan, void *data)
{
	int res = 0, i;
	char *exten, playfile[80]; 

	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);
	
	if (tris_strlen_zero(data)) {
		tris_log(LOG_WARNING, "PromptMsg requires an argument (exten)\n");
		return -1;
	}
	exten = tris_strdupa(data);
	snprintf(playfile, sizeof(playfile), "announcement/ann_%s", exten);
	
	for(i=0; i<3; i++) {
		res = tris_stream_and_wait(chan, playfile, "*");
		if(res == '*') {
			res = tris_waitfordigit(chan, 1000);
			if(res == '*') {
				change_pin(chan, exten);
				return 0;
			} else if(res == '#') {
				record_announce(chan, exten);
				return 0;
			}
		}
	}
	
	return 0;
}

static int ann_exec(struct tris_channel *chan, void *data)
{
	int res = 0, i;
	char sql[256], result[32];

	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);

	for(i=0; i<3; i++) {
		if(!res) 
			res = tris_play_and_wait(chan,"announcement/announcement");
		if(!res)
			res = tris_waitfordigit(chan, 5000);
		if(res == '*') {
			tris_play_and_wait(chan, "goodbye");
			return 0;
		}
		if(res) {
			snprintf(sql, sizeof(sql), "SELECT itemkey FROM announcement WHERE itemkey='%c'", res);
			sql_select_query_execute(result, sql);
			if(!tris_strlen_zero(result)) {
				prompt_exec(chan, result);
				return 0;
			} else{
				res = tris_play_and_wait(chan, "announcement/invalid_entry_try_again");
				if(!res) 
					tris_waitfordigit(chan, 5000);
				
			}
		}
	}
	
	tris_play_and_wait(chan, "goodbye");
	return 0;
}

struct autoattend_obj {
	char *sql;
	char itemid[256];
	char itemkey[256];
	SQLLEN err;
};

static SQLHSTMT autoattend_prepare(struct odbc_obj *obj, void *data)
{
	struct autoattend_obj *q = data;
	SQLHSTMT sth;
	int res;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &sth);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Failure in AllocStatement %d\n", res);
		return NULL;
	}

	res = SQLPrepare(sth, (unsigned char *)q->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_verb(4, "Error in PREPARE %d\n", res);
		SQLFreeHandle(SQL_HANDLE_STMT, sth);
		return NULL;
	}

	SQLBindCol(sth, 1, SQL_C_CHAR, q->itemid, sizeof(q->itemid), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->itemkey, sizeof(q->itemkey), &q->err);

	return sth;
}

struct autoattend_item {
	char itemid[256];
	char itemkey[256];
	struct autoattend_item *next;
};

static int run_service(struct tris_channel *chan, struct autoattend_item *ai)
{
	char args[256] = "";
	struct tris_app *tris_app = NULL;
	int len = 0;

	if (!ai) {
		return 0;
	}
	len = strlen(ai->itemid);
	
	if (len == 8 && !strncmp(ai->itemid, "leave_vm", 8)) {
		tris_app = pbx_findapp("Voicemail");
	} else if (len == 9 && !strncmp(ai->itemid, "listen_vm", 9)) {
		tris_app = pbx_findapp("VoicemailMain");
		snprintf(args, sizeof(args), "%s", chan->cid.cid_num);
	} else if (len == 12 && !strncmp(ai->itemid, "scheduleconf", 12)) {
		tris_app = pbx_findapp("Scheduleconf");
	} else if (len == 11 && !strncmp(ai->itemid, "urgencyconf", 11)) {
		tris_app = pbx_findapp("Urgencyconf");
	} else if (len == 9 && !strncmp(ai->itemid, "broadcast", 9)) {
		tris_app = pbx_findapp("CmdBroadcast");
	} else if (len == 12 && !strncmp(ai->itemid, "announcement", 12)) {
		tris_app = pbx_findapp("Announcement");
	} else if (len == 8 && !strncmp(ai->itemid, "greeting", 8)) {
		tris_app = pbx_findapp("Greeting");
	} else if (len == 8 && !strncmp(ai->itemid, "callconf", 8)) {
		tris_app = pbx_findapp("Callconf");
	} else if (len == 4 && !strncmp(ai->itemid, "rate", 4)) {
		tris_app = pbx_findapp("CheckRate");
	}

	if (tris_app) {
		pbx_exec(chan, tris_app, args);
	}
	return 0;
}

static int check_item_id(struct tris_channel *chan, struct autoattend_item *ai, int cmd)
{
	struct autoattend_item *temp = ai;
	int found = 0;
	while (temp) {
		if (strlen(temp->itemkey) > 0 && temp->itemkey[0] == cmd) {
			found = 1;
			break;
		}
		temp = temp->next;
	}
	if (!found) {
		return tris_play_and_wait(chan, "autoattendance/invalid_entry_try_again");
	} else {
		run_service(chan, temp);
		return 'p';
	}
}

static int autoattend_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	struct odbc_obj *obj;
	SQLSMALLINT rowcount = 0;
	SQLHSTMT stmt;
	struct autoattend_obj q;
	char sql[256];
	struct autoattend_item *ai = NULL, *temp;
	int useTTS = 1;
	int cmd = 0, tries = 3;
	char playfile[256] = "";
	
	if (!chan->cid.cid_num)	
		return -1;
	
	if (chan->_state != TRIS_STATE_UP) {
		res = tris_answer(chan);
	}
	
	if (tris_strlen_zero(chan->cid.cid_num)) {
		return -1;
	}

	/* Go through parsing/calling each device */

	memset(&q, 0, sizeof(q));
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;

	snprintf(sql, sizeof(sql), "SELECT itemid, itemkey FROM auto_attendance order by itemkey desc");
	q.sql = sql;
	
	stmt = tris_odbc_prepare_and_execute(obj, autoattend_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sql);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	while ((SQLFetch(stmt)) != SQL_NO_DATA) {
		if (strlen(q.itemid) == 6 && !strncmp(q.itemid, "useTTS", 6)) {
			if (strlen(q.itemkey) == 1 && !strncmp(q.itemkey, "n", 1))
				useTTS = 0;
		} else if (strlen(q.itemid) == 6 && !strncmp(q.itemid, "cancel", 6)) {
			;
		} else {
			temp = malloc(sizeof(struct autoattend_item));
			if (!temp) {
				tris_log(LOG_ERROR, "There's no memory left\n");
				tris_odbc_release_obj(obj);
				return -1;
			}
			memset(temp, 0, sizeof(struct autoattend_item));
			snprintf(temp->itemid, sizeof(temp->itemid), "%s", q.itemid);
			snprintf(temp->itemkey, sizeof(temp->itemkey), "%s", q.itemkey);
			temp->next = ai;
			ai = temp;
		}
	}
	/* remove recording temp file */
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);

	while (tries > 0 && cmd != 'p' && cmd != '*') {
		if (useTTS) {
			cmd = tris_play_and_wait(chan, "autoattendance/welcome");
			if (cmd)
				goto check_cmd;
			temp = ai;
			while (temp) {
				snprintf(playfile, sizeof(playfile), "autoattendance/to_%s", temp->itemid);
				cmd = tris_play_and_wait(chan, playfile);
				if (cmd < 0)
					break;
				else if (cmd > 0)
					goto check_cmd;
				snprintf(playfile, sizeof(playfile), "autoattendance/%s_key", temp->itemkey);
				cmd = tris_play_and_wait(chan, playfile);
				if (cmd < 0)
					break;
				else if (cmd > 0)
					goto check_cmd;
				temp = temp->next;
			}
			cmd = tris_play_and_wait(chan, "autoattendance/press_and");
			if (cmd < 0)
				break;
			else if (cmd > 0)
				goto check_cmd;
			cmd = tris_play_and_wait(chan, "autoattendance/to_cancel_press_star");
			if (cmd < 0)
				break;
			else if (cmd > 0)
				goto check_cmd;
		} else {
			cmd = tris_play_and_wait(chan, "autoattendance/autoattendance");
			if (cmd < 0)
				break;
			else if (cmd > 0)
				goto check_cmd;
		}
		if (!cmd)
			cmd = tris_waitfordigit(chan, 3000);
check_cmd:
		if (cmd > 0) {
			if (cmd == '*')
				break;
			cmd = check_item_id(chan, ai, cmd);
			if (cmd < 0)
				break;
			else if (cmd > 0 && cmd != 'p')
				goto check_cmd;
		} else if (cmd < 0) {
			break;
		}
		tries--;
	}
	temp = ai;
	while (temp) {
		ai = temp->next;
		free(temp);
		temp = ai;
	}
	if (cmd >= 0 && cmd != 'p')
		tris_play_and_wait(chan, "goodbye");

	return 0;

}

static int reload(void)
{
	return setup_announcement(1);
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app_ann);
	res |= tris_unregister_application(app_prompt);
	res |= tris_unregister_application(app_playopera);
	res |= tris_unregister_application(app_autoattend);
	
	return res;
}


static int load_module(void)
{
	int res = 0;

	res = setup_announcement(0);

	if (res)
		return TRIS_MODULE_LOAD_DECLINE;

	res |= tris_register_application(app_ann, ann_exec, synopsis_ann, descrip_ann);
	res |= tris_register_application(app_prompt, prompt_exec, synopsis_prompt, descrip_prompt);
	res |= tris_register_application(app_playopera, playopera_exec, synopsis_playopera, descrip_playopera);
	res |= tris_register_application(app_autoattend, autoattend_exec, synopsis_autoattend, descrip_autoattend);

	return res;
}


TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Ann Application",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
		);

