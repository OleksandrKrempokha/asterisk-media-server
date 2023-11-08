 /*
 * $Id: alarm.h,v 1.0 2014/05/18 09:57 $
 * 
 *
 * Copyright (C) 2006-2008 TriS
 *
 *
 * Alarm module 
 *
 * History:
 * --------
 *  2014-05-18  created by KYJ
 */


#ifndef AL_MOD_H
#define AL_MOD_H

#include <stdio.h>
#include "trismedia/lock.h"
#include "trismedia/strings.h"
#include "trismedia/res_odbc.h"

/*
 * Type definitions
 */

#define AL_MAX_HASH_SIZE	64
#define AL_MAX_STR_SIZE	64
#define MAX_SQL_LENGTH		256

#define AL_TDMTRUNK_FAULT			2003
#define AL_TDMTRUNK_RECOVERY		12003

enum TRIS_ALARM_STATUS {
	AL_STATUS_FAULT = 1,
	AL_STATUS_EVENT,
	AL_STATUS_RECOVERY
};

struct alarm_list{
	int al_num;
	char * al_source;
	char * al_time;
	char * al_param;
	char * al_item1;
	char * al_item2;
	char * al_item3;
	enum TRIS_ALARM_STATUS al_status;
	struct alarm_list * next;
};

struct alarm_table{
	struct alarm_list * list[AL_MAX_HASH_SIZE];
	tris_mutex_t lock;
};

#ifdef DEBUG_THREADS
typedef void * (*tris_odbc_connect_f)(char *db_name, int check_sanity, const char *file, const char *function, int lineno);
#else
typedef void * (*tris_odbc_connect_f)(char *db_name, int check_sanity);
#endif
typedef void (*tris_odbc_disconnect_f)(struct odbc_obj *obj);
typedef int (*tris_query_execute_f)(struct odbc_obj *obj, char *sql);

extern tris_odbc_connect_f tris_odbc_connect;
extern tris_odbc_disconnect_f tris_odbc_disconnect;
extern tris_query_execute_f tris_query_execute;

int al_init(void);       /* Module initialization function */
void al_destroy(void);
void tris_alarm(int al_num, char * al_source, char * al_param, char * al_item1, char * al_item2, char * al_item3);       /* setting alarm function */

#endif /* AL_MOD_H */

