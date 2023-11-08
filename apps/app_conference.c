/*
 * app_conference
 *
 * A channel independent conference application for Trismedia
 *
 * Copyright (C) 2002, 2003 Junghanns.NET GmbH
 * Copyright (C) 2003, 2004 HorizonLive.com, Inc.
 * Copyright (C) 2005, 2006 HorizonWimba, Inc.
 * Copyright (C) 2007 Wimba, Inc.
 *
 * Klaus-Peter Junghanns <kapejod@ns1.jnetdns.de>
 *
 * Video Conferencing support added by
 * Neil Stratford <neils@vipadia.com>
 * Copyright (C) 2005, 2005 Vipadia Limited
 *
 * VAD driven video conferencing, text message support
 * and miscellaneous enhancements added by
 * Mihai Balea <mihai at hates dot ms>
 *
 * This program may be modified and distributed under the
 * terms of the GNU General Public License. You should have received
 * a copy of the GNU General Public License along with this
 * program; if not, write to the Free Software Foundation, Inc.
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "trismedia.h"

// SVN revision number, provided by make
#ifndef REVISION
#define REVISION "unknown"
#endif

static char *revision = REVISION;

TRISMEDIA_FILE_VERSION(__FILE__, REVISION)

#include "appconference/app_conference.h"
#include "appconference/common.h"
#include "trismedia/res_odbc.h"

/*
 * a conference has n + 1 threads, where n is the number of
 * members and 1 is a conference thread which sends audio
 * back to the members.
 *
 * each member thread reads frames from the channel and
 * add's them to the member's frame queue.
 *
 * the conference thread reads frames from each speaking members
 * queue, mixes them, and then re-queues them for the member thread
 * to send back to the user.
 */

static char *app = "Conference";
static char *synopsis = "Channel Independent Conference";
static char *descrip = "Channel Independent Conference Application";

static const char *app_schedulevideoconf = "ScheduleVideoConf";
static const char *schedulevideoconf_synopsis = "Make a scheduled video conference";

struct user_obj {
	char *sql;
	char name[64];
	char job[256];
	char groupname[256];
	SQLLEN err;
};

static SQLHSTMT user_prepare(struct odbc_obj *obj, void *data)
{
	struct user_obj *q = data;
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

	SQLBindCol(sth, 1, SQL_C_CHAR, q->name, sizeof(q->name), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->job, sizeof(q->job), &q->err);
	SQLBindCol(sth, 3, SQL_C_CHAR, q->groupname, sizeof(q->groupname), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

/* 2012-07-16 */

static int user_info(char *result, const char *extension, struct odbc_obj *obj)
{
	char sqlbuf[1024];
	SQLHSTMT stmt;
	struct user_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;

	result[0] = '\0';

	if (tris_strlen_zero(extension)) {
		return 0;
	}

	memset(&q, 0, sizeof(q));

	if (!obj)
		return 0;
	
	//snprintf(sqlbuf, sizeof(sqlbuf), 
	//	"SELECT name, job FROM user_info WHERE extension = '%s' ", extension);
	snprintf(sqlbuf, sizeof(sqlbuf), 
		"SELECT u.name, u.job, c.grp_name FROM user_info AS u LEFT JOIN groups AS c ON u.gid = c.gid WHERE u.extension = '%s' ", extension);

	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, user_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		return -1;
	}

	char tmp[1024];
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		if(tris_strlen_zero(result))
			snprintf(tmp, sizeof(tmp), "%s,%s %s", q.name, q.groupname, q.job);
		else
			snprintf(tmp, sizeof(tmp), ",%s,%s %s", q.name, q.groupname, q.job);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	
	if(tris_strlen_zero(result))
		strcpy(result, "<unknown>,<unknown>,<unknown>");
	
	return 0;
}

static int action_videoconfuserdetail(struct mansession *s, const struct message *m)
{
	const char *userid = astman_get_header(m, "UserID");
	char result[5120]="";
	struct odbc_obj *obj;
	int res = 0;

	if (tris_strlen_zero(userid)) {
		astman_send_error(s, m, "UserID not specified");
		return 0;
	}
	
	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;

	res = user_info(result, userid, obj);
	
	astman_send_ack(s, m, "User info will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

struct room_obj {
	char *sql;
	char roomno[16];
	char roomname[40];
	char sponsoruid[64];
	SQLLEN err;
};

static SQLHSTMT room_prepare(struct odbc_obj *obj, void *data)
{
	struct room_obj *q = data;
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

	SQLBindCol(sth, 1, SQL_C_CHAR, q->roomno, sizeof(q->roomno), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->roomname, sizeof(q->roomname), &q->err);
	SQLBindCol(sth, 3, SQL_C_CHAR, q->sponsoruid, sizeof(q->sponsoruid), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}

/* 2012-07-16 */


static char mandescr_videoconflist[] =
"Description: Videoconf List.\n"
"Variables: (Names marked with * are required)\n"
"	*Sponosr: Sponsor ID\n"
"Returns videoconf list that <Sponsor ID> could open.\n"
"\n";

static int action_videoconflist(struct mansession *s, const struct message *m)
{
	const char *sponsor = astman_get_header(m, "Sponsor");
	char result[5120]="", sqlbuf[1024];
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	struct room_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;

	if (tris_strlen_zero(sponsor)) {
		astman_send_error(s, m, "Sponosr not specified");
		return 0;
	}
	
	memset(&q, 0, sizeof(q));

	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;
	
	snprintf(sqlbuf, sizeof(sqlbuf), "SELECT roomno, roomname, sponseruid FROM videoconf_room WHERE sponseruid REGEXP '.*%s.*'", sponsor);
	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, room_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	char tmp[1024];
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		if(tris_strlen_zero(result)) 
			snprintf(tmp, sizeof(tmp), "%s,%s", q.roomno, q.roomname);
		else
			snprintf(tmp, sizeof(tmp), ",%s,%s", q.roomno, q.roomname);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	astman_send_ack(s, m, "Videoconf list will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

static int action_videoconfcanparticipate(struct mansession *s, const struct message *m)
{
	const char *participant = astman_get_header(m, "Participant");
	char result[5120]="", sqlbuf[1024], totalcount[30];
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	struct room_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;

	if (tris_strlen_zero(participant)) {
		astman_send_error(s, m, "Participant not specified");
		return 0;
	}
	
	memset(&q, 0, sizeof(q));

	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;
	
	snprintf(sqlbuf, sizeof(sqlbuf), " SELECT c.roomno, c.roomname, c.sponseruid FROM videoconf_member AS u LEFT JOIN videoconf_room AS c ON u.roomno = c.roomno WHERE memberuid='%s'", participant);
	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, room_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	char tmp[1024];
	int usercount;
	tris_conference_stats stats;
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		usercount = 0;
		snprintf(sqlbuf, sizeof(sqlbuf), "SELECT COUNT(*) FROM videoconf_member WHERE roomno='%s'", q.roomno);
		sql_select_query_execute(totalcount, sqlbuf);
		
		/* Find the right conference */
		usercount = get_conference_stats_by_name(&stats, q.roomno);
		
		char all_info[2048] = "", u_info[1024], *exten, *tmp2 = q.sponsoruid;
		while ((exten = strsep(&tmp2, ","))) {
			user_info(u_info, exten, obj);
			snprintf(tmp, sizeof(tmp), ",%s,%s", exten, u_info);
			strcat(all_info, tmp);
		}
		
		if(tris_strlen_zero(result)) 
			snprintf(tmp, sizeof(tmp), "%s,%s%s,%d/%s", q.roomno, q.roomname, all_info, usercount,totalcount);
		else
			snprintf(tmp, sizeof(tmp), "!%s,%s%s,%d/%s", q.roomno, q.roomname, all_info, usercount,totalcount);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	astman_send_ack(s, m, "List will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

static int action_videoconfaddmember(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "Roomno");
	const char *memberid = astman_get_header(m, "MemberID");
	char sqlbuf[1024], result[256];

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}
	if (tris_strlen_zero(memberid)) {
		astman_send_error(s, m, "MemberID not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), "INSERT INTO videoconf_member(roomno, memberuid, mempermit) VALUES('%s', '%s', '1')", 
		roomno, memberid);
	sql_select_query_execute(result, sqlbuf);
	astman_send_listack(s, m, "Successfully completed", result);
	return 0;

}

static int action_videoconfremovemember(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "Roomno");
	const char *memberid = astman_get_header(m, "MemberID");
	char sqlbuf[1024], result[256];

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}
	if (tris_strlen_zero(memberid)) {
		astman_send_error(s, m, "MemberID not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), "DELETE FROM videoconf_member WHERE roomno='%s' AND memberuid='%s'", 
		roomno, memberid);
	sql_select_query_execute(result, sqlbuf);
	astman_send_listack(s, m, "Successfully completed", result);
	return 0;

}

static int action_videoconfsettalking(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "Roomno");
	const char *memberid = astman_get_header(m, "MemberID");
	const char *talking = astman_get_header(m, "Talking");
	char sqlbuf[1024], result[256];

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}
	if (tris_strlen_zero(memberid)) {
		astman_send_error(s, m, "MemberID not specified");
		return 0;
	}
	if (tris_strlen_zero(talking)) {
		astman_send_error(s, m, "Talking not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), 
		"UPDATE videoconf_member SET mempermit='%s' WHERE roomno='%s' AND memberuid='%s'", 
		strcasecmp(talking,"true")?"0":"1", roomno, memberid);
	sql_select_query_execute(result, sqlbuf);
	astman_send_listack(s, m, "Successfully completed", result);
	return 0;

}


struct member_obj {
	char *sql;
	char memberuid[64];
	char mempermit[10];
	SQLLEN err;
};

static SQLHSTMT member_prepare(struct odbc_obj *obj, void *data)
{
	struct member_obj *q = data;
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

	SQLBindCol(sth, 1, SQL_C_CHAR, q->memberuid, sizeof(q->memberuid), &q->err);
	SQLBindCol(sth, 2, SQL_C_CHAR, q->mempermit, sizeof(q->mempermit), &q->err);
//	SQLBindCol(sth, 3, SQL_C_CHAR, q->sponsoruid, sizeof(q->sponsoruid), &q->err);
//	SQLBindCol(sth, 4, SQL_C_CHAR, q->var_val, sizeof(q->var_val), &q->err);

	return sth;
}
static char mandescr_videoconfroomdetail[] =
"Description: Videoconf Room Detail.\n"
"Variables: (Names marked with * are required)\n"
"	*Roomno: Room number\n"
"	Sponosr: Sponsor ID\n"
"Returns participant list for Roomno.\n"
"\n";

static int action_videoconfroomdetail(struct mansession *s, const struct message *m)
{
	const char *roomno = astman_get_header(m, "roomno");
	char result[5120]="", sqlbuf[1024], roomname[40];
	struct odbc_obj *obj;
	SQLHSTMT stmt;
	struct member_obj q;
	SQLSMALLINT rowcount = 0;
	int res = 0;

	if (tris_strlen_zero(roomno)) {
		astman_send_error(s, m, "roomno not specified");
		return 0;
	}

	snprintf(sqlbuf, sizeof(sqlbuf), "SELECT roomname FROM videoconf_room where roomno='%s' ", roomno);
	sql_select_query_execute(roomname, sqlbuf);
	
	memset(&q, 0, sizeof(q));

	obj = tris_odbc_request_obj("trisdb", 0);
	if (!obj)
		return 0;
	
	snprintf(sqlbuf, sizeof(sqlbuf), "SELECT memberuid,mempermit FROM videoconf_member WHERE roomno='%s' ", roomno);
	//tris_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");
	q.sql = sqlbuf;

	stmt = tris_odbc_prepare_and_execute(obj, member_prepare, &q);

	if (!stmt) {
		tris_log(LOG_WARNING, "SQL select error!\n[%s]\n\n", sqlbuf);
		tris_odbc_release_obj(obj);
		return 0;
	}

	res = SQLNumResultCols(stmt, &rowcount);

	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		tris_log(LOG_WARNING, "SQL NumResultCols error!\n[%s]\n\n", sqlbuf);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		tris_odbc_release_obj(obj);
		return 0;
	}

	if (!rowcount) {
		tris_log(LOG_NOTICE, "found nothing\n");
		tris_odbc_release_obj(obj);
		return -1;
	}

	snprintf(result, sizeof(result), "%s,%s", roomno, roomname);
	
	char tmp[1024], u_info[1024];
	while ((res = SQLFetch(stmt)) != SQL_NO_DATA) {
		user_info(u_info, q.memberuid, obj);
		snprintf(tmp, sizeof(tmp), ",%s,%s,%s", q.memberuid, u_info, q.mempermit);
		strcat(result, tmp);
		
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	tris_odbc_release_obj(obj);
	
	astman_send_ack(s, m, "Videoconf list will follow");
	astman_append(s,   "%s\r\n", result);
	return 0;
}

static int app_conference_main(struct tris_channel* chan, void* data)
{
	int res ;
	struct tris_module_user *u ;

	u = tris_module_user_add(chan);

	// call member thread function
	res = member_exec( chan, data ) ;

	tris_module_user_remove(u);

	return res ;
}

static int schedulevideoconf_exec(struct tris_channel *chan, void *data)
{
	int res ;
	struct tris_module_user *u ;

	u = tris_module_user_add(chan);

	// call member thread function
	res = member_exec( chan, data ) ;

	tris_module_user_remove(u);

	return res ;
	
}

static int unload_module( void )
{
	int res = 0;
//	if (!(max_service & SERVICE_CONFERENCE))
//		return 0;
	
	tris_log( LOG_NOTICE, "unloading app_conference module\n" ) ;

	tris_module_user_hangup_all();

	unregister_conference_cli() ;
	res = tris_unregister_application( app );
	res |= tris_unregister_application(app_schedulevideoconf);
	res |= tris_manager_unregister("VideoconfList");
	res |= tris_manager_unregister("VideoconfRoomDetail");
	res |= tris_manager_unregister("VideoconfCanParticipate");
	res |= tris_manager_unregister("VideoconfAddMember");
	res |= tris_manager_unregister("VideoconfRemoveMember");
	res |= tris_manager_unregister("VideoconfSetTalking");
	res |= tris_manager_unregister("VideoconfUserDetail");

	return res;
}

static int load_module( void )
{
	int res = 0;
//	if (!(max_service & SERVICE_CONFERENCE))
//		return 0;
	
	tris_log( LOG_NOTICE, "Loading app_conference module, revision=%s\n", revision) ;

	init_conference() ;

	register_conference_cli() ;

	res = tris_register_application( app, app_conference_main, synopsis, descrip );
	res |= tris_register_application(app_schedulevideoconf, schedulevideoconf_exec, schedulevideoconf_synopsis, "ScheduleVideoConf(ConfNo)");
	res |= tris_manager_register2("VideoconfList", EVENT_FLAG_CALL, 
				    action_videoconflist, "Videoconf List", mandescr_videoconflist);
	res |= tris_manager_register2("VideoconfRoomDetail", EVENT_FLAG_CALL, 
				    action_videoconfroomdetail, "Videoconf Room Detail", mandescr_videoconfroomdetail);
	res |= tris_manager_register("VideoconfCanParticipate", EVENT_FLAG_CALL, 
				    action_videoconfcanparticipate, "List that one can participant");
	res |= tris_manager_register("VideoconfAddMember", EVENT_FLAG_CALL, 
				    action_videoconfaddmember, "Add Member");
	res |= tris_manager_register("VideoconfRemoveMember", EVENT_FLAG_CALL, 
				    action_videoconfremovemember, "Remove Member");
	res |= tris_manager_register("VideoconfSetTalking", EVENT_FLAG_CALL, 
				    action_videoconfsettalking, "Set Talking");
	res |= tris_manager_register("VideoconfUserDetail", EVENT_FLAG_CALL, 
				    action_videoconfuserdetail, "User Detail");

	return res;
}

// increment a timeval by ms milliseconds
void add_milliseconds(struct timeval* tv, long ms)
{
	// add the microseconds to the microseconds field
	tv->tv_usec += ( ms * 1000 ) ;

	// calculate the number of seconds to increment
	long s = ( tv->tv_usec / 1000000 ) ;

	// adjust the microsends field
	if ( s > 0 ) tv->tv_usec -= ( s * 1000000 ) ;

	// increment the seconds field
	tv->tv_sec += s ;
}

//#define TRIS_MODULE "Conference"
TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Channel Independent Conference Application");
//#undef TRIS_MODULE

