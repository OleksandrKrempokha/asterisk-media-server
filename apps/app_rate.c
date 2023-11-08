
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

static const char *app = "CheckRate";

static const char *synopsis_rate = "Check Rate";

static const char *descrip_rate =
"  CheckRate(Exten): Plays back the rate of specified exten\n"
"";

static char rate_database[80] = "money_test";

static int load_config(int reload);

static int load_config(int reload)
{
	struct tris_config *cfg;
	struct tris_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
//	const char *val;

	if (!(cfg = tris_config_load(CONFIG_FILE_NAME,config_flags)))
		return 0;

/*
	if ((val = tris_variable_retrieve(cfg, "general", "schedule")))
		rt_schedule = tris_true(val);
	if ((val = tris_variable_retrieve(cfg, "general", "logmembercount")))
		rt_log_members = tris_true(val);
	if ((val = tris_variable_retrieve(cfg, "general", "fuzzystart"))) {
		if ((sscanf(val, "%d", &fuzzystart) != 1)) {
			tris_log(LOG_WARNING, "fuzzystart must be a number, not '%s'\n", val);
			fuzzystart = 0;
		} 
	}
	if ((val = tris_variable_retrieve(cfg, "general", "earlyalert"))) {
		if ((sscanf(val, "%d", &earlyalert) != 1)) {
			tris_log(LOG_WARNING, "earlyalert must be a number, not '%s'\n", val);
			earlyalert = 0;
		} 
	}
	if ((val = tris_variable_retrieve(cfg, "general", "endalert"))) {
		if ((sscanf(val, "%d", &endalert) != 1)) {
			tris_log(LOG_WARNING, "endalert must be a number, not '%s'\n", val);
			endalert = 0;
		} 
	}
*/
	tris_config_destroy(cfg);
	return 1;
}

static int get_monday(void)
{
	int res = -1;
	time_t t = time(0);
	struct tm* tm;
	tm = localtime(&t);
	char mon[3];
	strftime(mon, sizeof(mon), "%m", tm);
	if(sscanf(mon,"%d", &res) != 1)
		tris_log(LOG_WARNING, "Failed to get monday!\n");
	return res;
}

static void mssql_execute(char *result,char *sql)
{
	int res;
	SQLHSTMT stmt;
	//char sql[PATH_MAX];
	char rowdata[20];
	//char msgnums[20];
	//char *argv[] = { dir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc =0, .argv = NULL };

	struct odbc_obj *obj;
	obj = tris_odbc_request_obj(rate_database, 0);

	if (obj) {
		//snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		//snprintf(sql, sizeof(sql), "SELECT %s FROM view%s WHERE tel='%s'", field, pre, tel);//rate_table);
		tris_verbose("%s\n", sql);
		stmt = tris_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			tris_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			tris_odbc_release_obj(obj);
			result[0]='\0';
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			result[0]='\0';
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		tris_verbose(" COOL (^_^) rowdata = %s\n",rowdata);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			tris_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			tris_odbc_release_obj(obj);
			result[0]='\0';
			goto yuck;
		}
		strcpy(result,rowdata);
//		if (sscanf(rowdata, "%d", &x) != 1)
//			tris_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
	} else{
		tris_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", rate_database);
		result[0]='\0';
	}
yuck:	
	return;
}

static int get_rate(char *result, char *tel)
{
	char sql[PATH_MAX];
	char field[20], pre[4];
	
	int mon = get_monday();
	
	if(mon == 1) mon = 12;
	else mon--;
		
	sprintf(field, "namege%d",mon);
	tris_verbose("monday = %s\n", field);

	tris_copy_string(pre, tel, 4);
	tris_verbose("%s\n", pre);
	
	snprintf(sql,sizeof(sql), "SELECT telhead FROM tbl_telorder WHERE telhead='%s'",pre);
	mssql_execute(result, sql);
	if(tris_strlen_zero(result))
		return 0;

	snprintf(sql,sizeof(sql), "SELECT bc FROM a%s WHERE tel='%s'", pre, tel);
	mssql_execute(result, sql);
	if(tris_strlen_zero(result) || strcmp(result,"2"))
		return 0;
	
	snprintf(sql, sizeof(sql), "SELECT %s FROM view%s WHERE tel='%s'", field, pre, tel);
	mssql_execute(result, sql);
	if(tris_strlen_zero(result))
		return 0;

	return 1;
	
}

static void play_rate(struct tris_channel *chan, char* tel, char *str_money)
{
	char tmp[30];
	tris_verbose("~~~~~~~~~~~~ num = %s\n",tel);
	
	int money, sign = 1;
	int div, num;
	
	sscanf(str_money, "%d", &money);
	
	if(money < 0) {
	    sign = 0;
	    money *= -1;
	}
	
	tris_stream_and_wait(chan, "rate/extension", "");
	while(*tel != '\0')
	{
	    if(*tel != '-') {
		sprintf(tmp,"rate/%c",*tel);
		tris_stream_and_wait(chan, tmp, "");
	    }
	    tel++;
	}
	tris_stream_and_wait(chan, "rate/money_of", "");
	
	if(money == 0) {
	    tris_stream_and_wait(chan, "rate/zero", "");
	    tris_stream_and_wait(chan, "rate/remain", "");
	    return;
	}
	
	money %= 100000;
	div = 10000;
	
	while(1) {
	    num = money / div * div;
	    money %= div;
	    
	    if(money == 0) {
		    if(div == 1) {
			if(sign)
				sprintf(tmp, "rate/%dremain", num);
			else 
				sprintf(tmp, "rate/%dexceed", num);
			tris_stream_and_wait(chan, tmp, "");    
			
		    }
		    else {
			sprintf(tmp, "rate/%d", num);
			tris_stream_and_wait(chan, tmp, "");
			if(sign)
				tris_stream_and_wait(chan, "rate/remain", "");
			else
				tris_stream_and_wait(chan, "rate/exceed", "");
		    }
		    
		    break;
	    }
	    else if(num != 0) {
		    sprintf(tmp, "rate/%d", num);
		    tris_stream_and_wait(chan, tmp, "");
	    }
	    
	    div /= 10;
	}
	
}


static void change_tel_type(char *tel2)
{
	unsigned int i;
	tel2 += 7;
	for(i = 0; i<5; i++){
    	    *(tel2+1) = *tel2;
    	    tel2--;
	}
	*(tel2+1) = '-';
}

static int rate_exec(struct tris_channel *chan, void *data)
{
	int res = 0;
	char *localdata;
	char telnum[20]; 
	char money[20];
	char *tel = telnum;
	char othernum[MAX_UID_LEN];

	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(telnum);
	);

	if (chan->_state != TRIS_STATE_UP)
		tris_answer(chan);
	
//	if (tris_strlen_zero(data)) {
//		tris_log(LOG_WARNING, "Rate requires an argument (telnum)\n");
//		return -1;
//	}
	
	if (!(localdata = tris_strdupa(data)))
		return -1;

	TRIS_STANDARD_APP_ARGS(args, localdata);
	
	if (args.telnum) {
		tris_copy_string(telnum, args.telnum, sizeof(telnum));
		if (tris_strlen_zero(telnum)) {
			return -1;
		}
	}

	if(telnum[0] == '2') tel = telnum+1;
	if(telnum[0] == '0' && telnum[1] == '2') tel = telnum+2;
	
	change_tel_type(tel);

	mssql_execute(money,"SELECT telhead FROM tbl_telorder");
	if(tris_strlen_zero(money)){
		tris_stream_and_wait(chan, "rate/server_error", "");
		return -1;
	}
	
	res = tris_stream_and_wait(chan, "rate/money_menu", "#");
	if(!res){
		
		if(!get_rate(money,tel)){
			res = tris_stream_and_wait(chan, "rate/no_client", "#");
		}
		else{
			play_rate(chan, tel, money);
		}
		
		if(!res)
			res = tris_stream_and_wait(chan, "rate/to_use_other", "#");
		if(!res)
			res = tris_waitfordigit(chan,3000);
		if(!res)
			return -1;

		
	}
	if(res == '#') {
		int cmd = tris_play_and_wait(chan, "rate/dial_telnum");
		if(!cmd) 
			cmd = tris_waitfordigit(chan,3000);
	
		if(!cmd) return -1;
		
		othernum[0] = cmd;
		othernum[1] = '\0';
		cmd = tris_readstring(chan, othernum + strlen(othernum), sizeof(othernum) - 2
				, 5000, 3000, "#");
		if(cmd < 0) return -1;
		
		tel = othernum;
		change_tel_type(tel);
		if(!get_rate(money,tel)){
			res = tris_stream_and_wait(chan, "rate/no_client", "#");
		}
		else{
			play_rate(chan, tel, money);
		}

	}
	return -1;
}

static int reload(void)
{
	return load_config(1);
}

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(app);
	
	return res;
}


static int load_module(void)
{
	int res = 0;

	res |= load_config(0);

	res |= tris_register_application(app, rate_exec, synopsis_rate, descrip_rate);

	return res;
}


TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Rate Application",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
		);


