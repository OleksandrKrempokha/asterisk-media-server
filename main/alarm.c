 /*
 * $Id: alarm.h,v 1.0 2014/05/18 09:57 $
 * 
 *
 * Copyright (C) 2006-2008 TriS
 *
 * This file is part of TrisMedia, a SIP server.
 *
 * For a license to use the TrisMedia software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact voip.rns.edu.kp by e-mail at the following addresses:
 *    white_night@voip.rns.edu.kp
 *
 *
 * Alarm module 
 *
 * History:
 * --------
 *  2014-05-18  created by KYJ
 */
 
#include "trismedia.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "trismedia/res_odbc.h"
#include "trismedia/logger.h"
#include "trismedia/utils.h"
#include "trismedia/alarm.h"


struct alarm_table * als = NULL;

tris_odbc_connect_f tris_odbc_connect = NULL;
tris_odbc_disconnect_f tris_odbc_disconnect = NULL;
tris_query_execute_f tris_query_execute = NULL;


#define AL_DB_DATABASE		"trisdb"
#define AL_DB_TABLE			"alarm_history"
#define AL_DB_NUMBER_COL	"alarm_number"
#define AL_DB_DATE_COL		"alarm_date"
#define AL_DB_SOURCE_COL	"alarm_source"
#define AL_DB_PARAM_COL	"alarm_param"
#define AL_DB_ITEM1_COL	"item1"
#define AL_DB_ITEM2_COL	"item2"
#define AL_DB_ITEM3_COL	"item3"
#define AL_DB_NOTADD		4


/*
 * Module functions that are defined later
 */

void destroy_alarm_list(struct alarm_list * list);
#define al_hash_key(v)	(v) % AL_MAX_HASH_SIZE
int time2str(time_t tv, char * strdst, int strlen);
int add_db_alarmhistory(struct alarm_list * list);


int init_als(void)
{
	als = (struct alarm_table *)tris_malloc(sizeof(struct alarm_table));
	if(!als){
		tris_log(TRIS_LOG_ERROR, "cannot allocate memory\n");
		return -1;
	}
	memset(als, 0, sizeof(struct alarm_table));
	if(tris_mutex_init(&als->lock) < 0){
		tris_log(TRIS_LOG_ERROR, "al_init() :: unable to initialize als\n");
		tris_free(als);
		als = NULL;
		return -1;
	}

	return 0;
}

/*
 * Module initialization function that is called before the main process forks
 */
int al_init(void)
{

	tris_log(TRIS_LOG_NOTICE, "alarm stack module - initializing\n");

	/* Bind database */

	if (init_als() < 0) {
		tris_log(TRIS_LOG_ERROR, "ag_init: Unable to initialize alarm table.\n");
		return -1;
	}
	
	return 0;

}


void destroy_all_als(void)
{
	int i;
	struct alarm_list * list, * next;
	
	if (!als)
		return;

	tris_mutex_lock(&als->lock);
	for(i=0; i<AL_MAX_HASH_SIZE; i++){
		list = als->list[i];
		while(list){
			next = list->next;
			destroy_alarm_list(list);
			list = next;
		}
		als->list[i] = 0;
	}
	tris_mutex_unlock(&als->lock);
	tris_mutex_destroy(&als->lock);
	tris_free(als);
	als = NULL;
}

void al_destroy(void)
{
	destroy_all_als();
}

int time2str(time_t tv, char * strdst, int strlen)
{
	struct tm * t;
	int len;

	if(!strdst || ! tv)
		return -1;

	t = localtime(&tv);
	len = (int)strftime(strdst, strlen, "%Y-%m-%d %H:%M:%S", t);
	
	return len > 0 ? 0 : -2;
}

char *str_duplicate(char *src)
{
	char *tmp;
	int len = strlen(src);
	tmp = tris_malloc(len+1);
	memcpy(tmp, src, len);
	tmp[len] = '\0';
	return tmp;
}

void tris_alarm(int al_num, char * al_source, char * al_param, char * al_item1, char * al_item2, char * al_item3)
{
	char al_time[AL_MAX_STR_SIZE];
	int al_idx, al_status, key;
	struct alarm_list * list;

	if(!als)
		return;

	if(time2str(time(NULL), al_time, sizeof(al_time)) < 0)
		return;

	if(al_num > 10000){
		al_idx = al_num - 10000;
		al_status = AL_STATUS_RECOVERY;
	}else{
		al_idx = al_num;
		al_status = AL_STATUS_FAULT;
	}

	tris_mutex_lock(&als->lock);

	key = al_hash_key(al_idx);
	list = als->list[key];
	while(list){
		if(al_param && list->al_param && strcmp(al_param, list->al_param)) {
			list = list->next;
			continue;
		}
		break;
	}

	if(list) { // found list
		if (al_status == AL_STATUS_FAULT|| al_status == AL_STATUS_RECOVERY){ // al_status is either fault or recovery
			if(al_status != (list->al_status & ~AL_DB_NOTADD)){
				list->al_status = al_status;
				list->al_num = al_num;
			}else{
				tris_log(TRIS_LOG_NOTICE, "alarm() :: alarm '%d' duplicated, we don't add alarm history into db.\n", al_num);
				goto end;
			}
		}
	}else{ //not found list
		list = tris_malloc(sizeof(struct alarm_list));
		if(!list){
			tris_log(TRIS_LOG_DEBUG, "alarm() :: can't alloc memory for alarm list.\n");
			goto end;
		}
		memset(list, 0, sizeof(struct alarm_list));

		if(al_param){
			list->al_param = str_duplicate(al_param);
		}
		
		list->al_num = al_num;
		list->al_status = al_status;
		list->next = als->list[key];
		als->list[key] = list;
	}

	list->al_time = str_duplicate(al_time);
	
	if(add_db_alarmhistory(list) < 0){
		list->al_status |=  AL_DB_NOTADD;
	}

end:
	tris_mutex_unlock(&als->lock);
	return;

}

void destroy_alarm_list(struct alarm_list * list)
{
	if(list->al_time)
		tris_free(list->al_time);
	if(list->al_param)
		tris_free(list->al_param);
	tris_free(list);
}

int add_db_alarmhistory(struct alarm_list * list)
{
	struct odbc_obj * odbc;
	char sql[MAX_SQL_LENGTH];
	int ret;

#ifdef DEBUG_THREADS
	odbc = (struct odbc_obj *)tris_odbc_connect(AL_DB_DATABASE, 0, __FILE__, __PRETTY_FUNCTION__, __LINE__);
#else
	odbc = (struct odbc_obj *)tris_odbc_connect(AL_DB_DATABASE, 0);
#endif
	if(!odbc){
		tris_log(TRIS_LOG_ERROR, "Alarm module :: db connecting error\n");
		ret = -1;
		goto end;
	}

	
	sprintf(sql, "insert into %s (%s, %s, %s) values ('%d', '%s', '%s')"
		, AL_DB_TABLE
		, AL_DB_NUMBER_COL, AL_DB_DATE_COL, AL_DB_PARAM_COL
		, list->al_num, list->al_time?list->al_time:"", list->al_param?list->al_param:"");

	if(tris_query_execute(odbc, sql) < 0){
		tris_log(TRIS_LOG_ERROR, "Alarm module :: Error while inserting alarm history info, sql: '%s'\n", sql);
		ret = -2;
		goto err;
	}
	ret = 0;
err:
	tris_odbc_disconnect(odbc);
end:
	return ret;
}

