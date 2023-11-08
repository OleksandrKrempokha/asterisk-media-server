/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006-2011, voipteam.com.
 *
 * White_Night <white_night@rns.edu.kp>
 *
 * See http://www.rns.edu.kp for more information about
 * the Tris project.
 *
 */

/* Headers */
#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 250253 $")

#include <ctype.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/signal.h>
#include <regex.h>
#include <time.h>
#include <wchar.h>

#include "trismedia/network.h"
#include "trismedia/paths.h"	/* need tris_config_TRIS_SYSTEM_NAME */

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/config.h"
#include "trismedia/module.h"
#include "trismedia/pbx.h"
#include "trismedia/sched.h"
#include "trismedia/io.h"
#include "trismedia/rtp.h"
#include "trismedia/udptl.h"
#include "trismedia/acl.h"
#include "trismedia/manager.h"
#include "trismedia/callerid.h"
#include "trismedia/cli.h"
#include "trismedia/app.h"
#include "trismedia/musiconhold.h"
#include "trismedia/dsp.h"
#include "trismedia/features.h"
#include "trismedia/srv.h"
#include "trismedia/astdb.h"
#include "trismedia/causes.h"
#include "trismedia/utils.h"
#include "trismedia/file.h"
#include "trismedia/astobj.h"

#include "trismedia/astobj2.h"
#include "trismedia/dnsmgr.h"
#include "trismedia/devicestate.h"
#include "trismedia/linkedlists.h"
#include "trismedia/stringfields.h"
#include "trismedia/monitor.h"
#include "trismedia/netsock.h"
#include "trismedia/localtime.h"
#include "trismedia/abstract_jb.h"
#include "trismedia/threadstorage.h"
#include "trismedia/translate.h"
#include "trismedia/tris_version.h"
#include "trismedia/event.h"
#include "trismedia/tcptls.h"
#include "trismedia/strings.h"

#include "trismedia/res_odbc.h"
#include "appconference/member.h"
#include "appconference/common.h"

//define sql relation
#define MAX_SQL_DATA		256
#define MAX_SQL_STAT		800		

// Identity for conference Rakwon Server/Client packet
#define PACKET_IDENTITY		0x19740525
#define RAKWON_MAXLEN_SIGPACK	1024 /*4096*/ //initialize size of memory to allocate for rakwon packets
//Rakwon server table name
#define TRIS_TB_RAKWON_SERVER      "rakwon_servers"
//Rakwon User Type
enum rakwon_user_type{
	RAKWON_USER_PROMOTER,
	RAKWON_USER_COMMON,
	RAKWON_USER_CONTROLLER
};
enum rakwon_transport {
	RAKWON_TRANSPORT_UDP = 1,		/*!< Unreliable transport for RAKWON, needs retransmissions */
	RAKWON_TRANSPORT_TCP = 1 << 1,	/*!< Reliable, but unsecure */
	RAKWON_TRANSPORT_TLS = 1 << 2,	/*!< TCP/TLS - reliable and secure transport for signalling */
};
#define XMIT_ERROR		-2
/*! \brief States for the INVITE transaction, not the dialog 
	\note this is for the INVITE that sets up the dialog
*/
enum invitestates {
	INV_NONE = 0,	        /*!< No state at all, maybe not an INVITE dialog */
	INV_CALLING = 1,	/*!< Invite sent, no answer */
	INV_PROCEEDING = 2,	/*!< We got/sent 1xx message */
	INV_EARLY_MEDIA = 3,    /*!< We got 18x message with to-tag back */
	INV_COMPLETED = 4,	/*!< Got final response with error. Wait for ACK, then CONFIRMED */
	INV_CONFIRMED = 5,	/*!< Confirmed response - we've got an ack (Incoming calls only) */
	INV_TERMINATED = 6,	/*!< Transaction done - either successful (TRIS_STATE_UP) or failed, but done 
				     The only way out of this is a BYE from one side */
	INV_CANCELLED = 7,	/*!< Transaction cancelled by client or server in non-terminated state */
};

/* relation on rakwon config */
static enum channelreloadreason rakwon_reloadreason;
//static struct tris_flags global_flags[2] = {{0}};        /*!< global Rakwon_ flags */
struct rakwon_server{
	char exten[20];
	char ip[40];
	int port;
	char subject[300];
	int seats;
	struct rakwon_server * next;
	struct rakwon_server * prev;
};
struct rakwon_cfg{
	char server_extens[200];
	char db[20];
	char member_table[20];
	char default_server[40];
	int default_port;
	char default_subject[300];
	int default_seats;
	
	struct rakwon_server * serverlist;
};
static struct rakwon_cfg rakwon_conf;

static char speaker_agent[200];

//Rakwon packets' types --------------------------------------------------------------
#define TYPE_NONE						0x00
#define TYPE_REQ_REGISTER_USER			0x01
#define TYPE_RES_REGISTER_USER			0x02
#define TYPE_REQ_LOGIN_CHECK			0x03
#define TYPE_RES_LOGIN_CHECK			0x04
#define TYPE_REQ_USER_READY				0x05
#define TYPE_RES_USER_READY				0x06
#define TYPE_NOTIFY_USER_LOGIN			0x07
#define TYPE_NOTIFY_USER_EXIT			0x08
#define TYPE_REQ_SEND_TEXT				0x09
#define TYPE_NOTIFY_SEND_TEXT			0x10
#define TYPE_REQ_SET_SPEAKING			0x11
#define TYPE_RES_SET_SPEAKING			0x12
#define TYPE_NOTIFY_CHANGE_FPS			0x13
#define TYPE_NOTIFY_ACCEPT_FAILED		0x14
#define TYPE_NOTIFY_START_CONF			0x15
#define TYPE_REQ_USER_EXIT				0x16
#define TYPE_RES_USER_EXIT				0x17
#define TYPE_NOTIFY_LOUDING				0x18

//Rakwon response&request's code  -------------------------------
#define ERR_RES_UNKNOWN					0xFF
#define ERR_RES_SOCKET_CLOSED			0xFE

#define ERR_RES_REGISTER_USER_SUCCESS	0
#define ERR_RES_REGISTER_USER_EXISTS	1

#define ERR_RES_LOGIN_CHECK_SUCCESS				0
#define ERR_RES_LOGIN_CHECK_INVALID_PASSWORD	1
#define ERR_RES_LOGIN_CHECK_UNALLOWED_USER		2
#define ERR_RES_LOGIN_CHECK_DELETED_USER		3
#define ERR_RES_LOGIN_CHECK_INVALID_USER		4
#define ERR_RES_LOGIN_CHECK_LOGGED_IN			5
#define ERR_RES_LOGIN_CHECK_EXCEED_LIMIT		6
#define ERR_RES_LOGIN_CHECK_KEY_RECEIVING		7
#define ERR_RES_LOGIN_CHECK_PROMOTER_EXIST		8

#define ERR_REQ_USER_READY_INIT					0
#define ERR_REQ_USER_READY_START				1

#define ERR_RES_USER_READY_INVALID_USER			0

#define ERR_RES_SET_SPEAKING_ACCEPTED			0
#define ERR_RES_SET_SPEAKING_REJECTED			1
#define ERR_RES_SET_SPEAKING_CANCELED			2

#define ERR_NOTIFY_CHANGE_FPS_UP				0
#define ERR_NOTIFY_CHANGE_FPS_DOWN				1

#define ERR_RES_USER_EXIT_BY_SERVER				1
#define ERR_RES_USER_EXIT_BY_PROMOTER			2

//MALE Flag
#define MALEMAN							0x00
#define MALEWOMAN						0x01

#define MAX_COUNT_5		3
#define MAX_COUNT_25		25
#define MAX_COUNT_50		52
#define MAX_COUNT_70		70
#define MAX_COUNT_100		97

#define SORT_MODE_AUTO	1
#define SORT_MODE_MANUAL	2

#define VIDEO_SIZE_CIF		1
#define VIDEO_SIZE_QCIF		2

#define VFRAME_SIZE_NORMAL	1
#define VFRAME_SIZE_SMALL	2

#define VIDEO_WIDTH_SMALL	80
#define VIDEO_HEIGHT_SMALL	64

// Unique ID definitions
#define UID_UNKNOWN						-1

#define	MAX_CONNECTION					120
// Maximum count of members
#define MAX_MEMBER_COUNT				120
// Maximum count of remote members
// Count of remote peers is equal to the count of total members minus 1
#define MAX_REMOTE_COUNT				(MAX_MEMBER_COUNT - 1)

#define SERVER_TCP_PORT					5186
#define RTP_BASE_PORT					5188
#define RTP_BASE_PORT_SRVR				7188

#define RTP_AUDIO_PORT(UID)				(RTP_BASE_PORT + UID * 8)
#define RTP_VIDEO_PORT(UID)				(RTP_BASE_PORT + UID * 8 + 2)
#define RTP_VIDEO_AUX_PORT(UID)			(RTP_BASE_PORT + MAX_MEMBER_COUNT * 8 + UID * 2 - 2) // only for promoter

#define RTP_AUDIO_PORT_SRVR(UID)		(RTP_BASE_PORT_SRVR + UID * 8)
#define RTP_VIDEO_PORT_SRVR(UID)		(RTP_BASE_PORT_SRVR + UID * 8 + 2)
#define RTP_VIDEO_AUX_PORT_SRVR(UID)	(RTP_BASE_PORT_SRVR + MAX_MEMBER_COUNT * 8 + UID * 2 - 2) // only for promoter

#define RAKWONBUFSIZE 256
static struct sched_context *sched;     /*!< The scheduling context */
static struct io_context *io;           /*!< The IO context */
//static unsigned int global_tos_rakwon;		/*!< IP type of service for RAKWON packets */
static unsigned int global_tos_audio;		/*!< IP type of service for audio RTP packets */
//static unsigned int global_tos_video;		/*!< IP type of service for video RTP packets */
//static unsigned int global_tos_text;		/*!< IP type of service for text RTP packets */
//static unsigned int global_cos_rakwon;		/*!< 802.1p class of service for RAKWON packets */
static unsigned int global_cos_audio;		/*!< 802.1p class of service for audio RTP packets */
//static unsigned int global_cos_video;		/*!< 802.1p class of service for video RTP packets */
//static unsigned int global_cos_text;		/*!< 802.1p class of service for text RTP packets */
static int global_capability = TRIS_FORMAT_ILBC | TRIS_FORMAT_SPEEX | TRIS_FORMAT_H264;
static int global_rtptimeout;		/*!< Time out call if no RTP */
static int global_rtpholdtimeout;	/*!< Time out call if no RTP during hold */
static int global_rtpkeepalive;		/*!< Send RTP keepalives */
#ifdef LOW_MEMORY
static int hash_dialog_size = 17;
#else
static int hash_dialog_size = 563;
#endif
static struct sockaddr_in serverip;
static struct sockaddr_in internip;
static char default_language[MAX_LANGUAGE];
static unsigned int chan_idx;


//RakWon Packet Header
struct PACKET_HEADER {
	unsigned int	dwPacketID;
	unsigned char	byPacketType;
	unsigned int	dwDataLen;
};
static void initPH(struct PACKET_HEADER * ph)
{
	ph->dwPacketID = PACKET_IDENTITY;
	ph->byPacketType = TYPE_NONE;
	ph->dwDataLen = 0;
}
static int getSizeOfPH(struct PACKET_HEADER * ph)
{
	return sizeof(ph->dwPacketID) + sizeof(ph->byPacketType) + sizeof(ph->dwDataLen);
}
static int getSizeOfPacket(struct PACKET_HEADER * ph)
{
	return getSizeOfPH(ph) + ph->dwDataLen;
}
static void writePH(char * pszBuf, struct PACKET_HEADER * ph)
{
	int nPos = 0;

	memcpy(pszBuf, &ph->dwPacketID, sizeof(ph->dwPacketID));
	nPos += sizeof(ph->dwPacketID);

	memcpy(pszBuf + nPos, &ph->byPacketType, sizeof(ph->byPacketType));
	nPos += sizeof(ph->byPacketType);

	memcpy(pszBuf + nPos, &ph->dwDataLen, sizeof(ph->dwDataLen));
}
static void readPH(struct PACKET_HEADER * ph, char * pszBuf)
{
	int nPos = 0;

	memcpy(&ph->dwPacketID, pszBuf, sizeof(ph->dwPacketID));
	nPos += sizeof(ph->dwPacketID);

	memcpy(&ph->byPacketType, pszBuf + nPos, sizeof(ph->byPacketType));
	nPos += sizeof(ph->byPacketType);

	memcpy(&ph->dwDataLen, pszBuf + nPos, sizeof(ph->dwDataLen));
}

//Rakwon Common Request
struct REQ_COMMON {
	struct PACKET_HEADER	ph;
	int				byIndex;
	unsigned char		byCode;

};
static void initReqCommon(struct REQ_COMMON * req)
{
	initPH(&req->ph);
	req->byIndex = -1;
	req->byCode = 0;
}
static int getSizeOfReqComm(struct REQ_COMMON * req)
{
	return getSizeOfPH(&req->ph) +
		sizeof(req->byIndex) +
		sizeof(req->byCode);
}
static void writeReqComm(char * pszBuf, struct REQ_COMMON * req)
{
	int nPos = getSizeOfPH(&req->ph);

	memcpy(pszBuf + nPos, &req->byIndex, sizeof(req->byIndex));
	nPos += sizeof(req->byIndex);

	memcpy(pszBuf + nPos, &req->byCode, sizeof(req->byCode));
	nPos += sizeof(req->byCode);

	req->ph.dwDataLen = nPos - getSizeOfPH(&req->ph);
	writePH(pszBuf, &req->ph);
}
/*static void readReqComm(struct REQ_COMMON * req, char * pszBuf)
{
	int nPos = 0;

	readPH(&req->ph, pszBuf);
	nPos += getSizeOfPH(&req->ph);

	memcpy(&req->byIndex, pszBuf + nPos, sizeof(req->byIndex));
	nPos += sizeof(req->byIndex);

	memcpy(&req->byCode, pszBuf + nPos, sizeof(req->byCode));
	nPos += sizeof(req->byCode);
}*/

// Rakwon Common Response
struct RES_COMMON {
	struct PACKET_HEADER	ph;
	int				byIndex;
	unsigned char		byErrCode;

};
static void initResCommon(struct RES_COMMON * res)
{
	initPH(&res->ph);
	res->byIndex = -1;
	res->byErrCode = 0;
}
/*static int getSizeOfResComm(struct RES_COMMON * res)
{
	return getSizeOfPH(&res->ph) + 
		sizeof(res->byIndex) +
		sizeof(res->byErrCode);
}
static void writeResComm(char * pszBuf, struct RES_COMMON * res)
{
	int nPos = getSizeOfPH(&res->ph);

	memcpy(pszBuf + nPos, &res->byIndex, sizeof(res->byIndex));
	nPos += sizeof(res->byIndex);

	memcpy(pszBuf + nPos, &res->byErrCode, sizeof(res->byErrCode));
	nPos += sizeof(res->byErrCode);

	res->ph.dwDataLen = nPos - getSizeOfPH(&res->ph);
	writePH(pszBuf, &res->ph);
}*/
static void readResComm(struct RES_COMMON * res, char * pszBuf)
{
	int nPos = 0;

	readPH(&res->ph, pszBuf);
	nPos += getSizeOfPH(&res->ph);

	memcpy(&res->byIndex, pszBuf + nPos, sizeof(res->byIndex));
	nPos += sizeof(res->byIndex);

	memcpy(&res->byErrCode, pszBuf + nPos, sizeof(res->byErrCode));
	nPos += sizeof(res->byErrCode);
}

//Rakwon Login_check Request
struct REQ_LOGIN_CHECK {
	struct PACKET_HEADER	ph;
	unsigned short			wUserIDLen;
	unsigned short			strUserID[MAX_SQL_DATA];
	unsigned short			wUserPasswordLen;
	unsigned short			strUserPassword[MAX_SQL_DATA];
	enum rakwon_user_type	byUserType;

};
static int getSizeOfReqLogin(struct REQ_LOGIN_CHECK * req)
{
	return getSizeOfPH(&req->ph) +
		sizeof(req->wUserIDLen) +
		req->wUserIDLen +
		sizeof(req->wUserPasswordLen) +
		req->wUserPasswordLen + 
		sizeof(req->byUserType);
}
static void initReqLogin(struct REQ_LOGIN_CHECK * req)
{
	initPH(&req->ph);
	req->ph.byPacketType = TYPE_REQ_LOGIN_CHECK;
	req->wUserIDLen = 0;
	req->wUserPasswordLen = 0;
	req->byUserType = 1;
}
static void writeReqLogin(char * pszBuf, struct REQ_LOGIN_CHECK * req)
{
	int nPos = getSizeOfPH(&req->ph);

	memcpy(pszBuf + nPos, &req->wUserIDLen, sizeof(req->wUserIDLen));
	nPos += sizeof(req->wUserIDLen);

	memcpy(pszBuf + nPos, req->strUserID, req->wUserIDLen);
	nPos += req->wUserIDLen;

	memcpy(pszBuf + nPos, &req->wUserPasswordLen, sizeof(req->wUserPasswordLen));
	nPos += sizeof(req->wUserPasswordLen);

	memcpy(pszBuf + nPos, req->strUserPassword, req->wUserPasswordLen);
	nPos += req->wUserPasswordLen;

	memcpy(pszBuf + nPos, &req->byUserType, sizeof(req->byUserType));
	nPos += sizeof(req->byUserType);

	req->ph.dwDataLen = nPos - getSizeOfPH(&req->ph);
	writePH(pszBuf, &req->ph);
}
/*static void readReqLogin(struct REQ_LOGIN_CHECK * req, char * pszBuf)
{
	int nPos = 0;

	readPH(&req->ph, pszBuf);
	nPos += getSizeOfPH(&req->ph);

	memcpy(&req->wUserIDLen, pszBuf + nPos, sizeof(req->wUserIDLen));
	nPos += sizeof(req->wUserIDLen);

	memcpy(req->strUserID, pszBuf + nPos, req->wUserIDLen);
	nPos += req->wUserIDLen;

	memcpy(&req->wUserPasswordLen, pszBuf + nPos, sizeof(req->wUserPasswordLen));
	nPos += sizeof(req->wUserPasswordLen);

	memcpy(req->strUserPassword, pszBuf + nPos, req->wUserPasswordLen);
	nPos += req->wUserPasswordLen;

	memcpy(&req->byUserType, pszBuf + nPos, sizeof(req->byUserType));
	nPos += sizeof(req->byUserType);
}*/

//RakWon LOGIN_CHECK RESPONSE
struct RES_LOGIN_CHECK {
	struct PACKET_HEADER	ph;
	int				byIndex;
	unsigned short	wUserIDLen;
	unsigned short	strUserID[MAX_SQL_DATA];
	unsigned short	wUserNameLen;
	unsigned short	strUserName[MAX_SQL_DATA];
	unsigned short	wUserJobLen;
	unsigned short	strUserJob[MAX_SQL_DATA];
	unsigned char		byUserGender;
	int				nMaxMemberCount;
	unsigned char		bySortMode;
	unsigned char		byVFrameSize;
	unsigned char		byErrCode;

};
/*static int getSizeOfResLogin(struct RES_LOGIN_CHECK *res)
{
	return getSizeOfPH(&res->ph) +
		sizeof(res->byIndex) +
		sizeof(res->wUserIDLen) +
		res->wUserIDLen +
		sizeof(res->wUserNameLen) +
		res->wUserNameLen+
		sizeof(res->wUserJobLen) +
		res->wUserJobLen +
		sizeof(res->byUserGender) +
		sizeof(res->nMaxMemberCount) +
		sizeof(res->bySortMode) +
		sizeof(res->byVFrameSize) +
		sizeof(res->byErrCode);
}*/
static void initResLogin(struct RES_LOGIN_CHECK * res)
{
	initPH(&res->ph);
	res->ph.byPacketType = TYPE_RES_LOGIN_CHECK;
	res->byIndex = -1;		// -1
	res->wUserIDLen = 0;
	res->wUserNameLen = 0;
	res->wUserJobLen = 0;
	res->byUserGender = MALEMAN;
	res->nMaxMemberCount = MAX_COUNT_25;
	res->bySortMode = SORT_MODE_AUTO;
	res->byVFrameSize = VIDEO_SIZE_QCIF;	// 176 x 144
	res->byErrCode = ERR_RES_UNKNOWN;	// 0xFF : -1
}
/*static void writeResLogin(char * pszBuf, struct RES_LOGIN_CHECK * res)
{
	int nPos = getSizeOfPH(&res->ph);

	memcpy(pszBuf + nPos, &res->byIndex, sizeof(res->byIndex));
	nPos += sizeof(res->byIndex);

	memcpy(pszBuf + nPos, &res->wUserIDLen, sizeof(res->wUserIDLen));
	nPos += sizeof(res->wUserIDLen);

	memcpy(pszBuf + nPos, res->strUserID, res->wUserIDLen);
	nPos += res->wUserIDLen;

	memcpy(pszBuf + nPos, &res->wUserNameLen, sizeof(res->wUserNameLen));
	nPos += sizeof(res->wUserNameLen);

	memcpy(pszBuf + nPos, res->strUserName, res->wUserNameLen);
	nPos += res->wUserNameLen;

	memcpy(pszBuf + nPos, &res->wUserJobLen, sizeof(res->wUserJobLen));
	nPos += sizeof(res->wUserJobLen);

	memcpy(pszBuf + nPos, res->strUserJob, res->wUserJobLen);
	nPos += res->wUserJobLen;

	memcpy(pszBuf + nPos, &res->byUserGender, sizeof(res->byUserGender));
	nPos += sizeof(res->byUserGender);

	memcpy(pszBuf + nPos, &res->nMaxMemberCount, sizeof(res->nMaxMemberCount));
	nPos += sizeof(res->nMaxMemberCount);

	memcpy(pszBuf + nPos, &res->bySortMode, sizeof(res->bySortMode));
	nPos += sizeof(res->bySortMode);

	memcpy(pszBuf + nPos, &res->byVFrameSize, sizeof(res->byVFrameSize));
	nPos += sizeof(res->byVFrameSize);

	memcpy(pszBuf + nPos, &res->byErrCode, sizeof(res->byErrCode));
	nPos += sizeof(res->byErrCode);

	res->ph.dwDataLen = nPos - getSizeOfPH(&res->ph);
	writePH(pszBuf, &res->ph);
}*/
static void readResLogin(struct RES_LOGIN_CHECK * res, char * pszBuf)
{
	int nPos = 0;

	readPH(&res->ph, pszBuf);
	nPos += getSizeOfPH(&res->ph);

	memcpy(&res->byIndex, pszBuf + nPos, sizeof(res->byIndex));
	nPos += sizeof(res->byIndex);

	memcpy(&res->wUserIDLen, pszBuf + nPos, sizeof(res->wUserIDLen));
	nPos += sizeof(res->wUserIDLen);

	memcpy(res->strUserID, pszBuf + nPos, res->wUserIDLen);
	nPos += res->wUserIDLen;

	memcpy(&res->wUserNameLen, pszBuf + nPos, sizeof(res->wUserNameLen));
	nPos += sizeof(res->wUserNameLen);

	memcpy(res->strUserName, pszBuf + nPos, res->wUserNameLen);
	nPos += res->wUserNameLen;

	memcpy(&res->wUserJobLen, pszBuf + nPos, sizeof(res->wUserJobLen));
	nPos += sizeof(res->wUserJobLen);

	memcpy(res->strUserJob, pszBuf + nPos, res->wUserJobLen);
	nPos += res->wUserJobLen;

	memcpy(&res->byUserGender, pszBuf + nPos, sizeof(res->byUserGender));
	nPos += sizeof(res->byUserGender);

	memcpy(&res->nMaxMemberCount, pszBuf + nPos, sizeof(res->nMaxMemberCount));
	nPos += sizeof(res->nMaxMemberCount);

	memcpy(&res->bySortMode, pszBuf + nPos, sizeof(res->bySortMode));
	nPos += sizeof(res->bySortMode);

	memcpy(&res->byVFrameSize, pszBuf + nPos, sizeof(res->byVFrameSize));
	nPos += sizeof(res->byVFrameSize);

	memcpy(&res->byErrCode, pszBuf + nPos, sizeof(res->byErrCode));
	nPos += sizeof(res->byErrCode);
}


/*struct NOTIFY_LOGIN {
	struct PACKET_HEADER	ph;
	int				byIndex;
	unsigned short	wUserIDLen;
	unsigned short	strUserID[MAX_SQL_DATA];
	unsigned short	wUserNameLen;
	unsigned short	strUserName[MAX_SQL_DATA];
	unsigned short	wUserJobLen;
	unsigned short	strUserJob[MAX_SQL_DATA];
	unsigned char		byUserGender;
	int				nSortIndex;

};

static void initNotifyLogin(struct NOTIFY_LOGIN *nl)
{
	nl->ph.byPacketType = TYPE_NOTIFY_USER_LOGIN;
	nl->byIndex = UID_UNKNOWN;		// -1
	nl->wUserIDLen = 0;
	nl->wUserNameLen = 0;
	nl->wUserJobLen = 0;
	nl->byUserGender = MALEMAN;
	nl->nSortIndex = UID_UNKNOWN;
}

static int getSizeOfNL(struct NOTIFY_LOGIN *nl)
{
	return getSizeOfPH(&nl->ph) +
		sizeof(nl->byIndex) +
		sizeof(nl->wUserIDLen) +
		nl->wUserIDLen+
		sizeof(nl->wUserNameLen) +
		nl->wUserNameLen+
		sizeof(nl->wUserJobLen) +
		nl->wUserJobLen+
		sizeof(nl->byUserGender) +
		sizeof(nl->nSortIndex);
}

static void writeNotifyLogin(struct NOTIFY_LOGIN *nl, char * pszBuf)
{
	int nPos = getSizeOfPH(&nl->ph);

	memcpy(pszBuf + nPos, &nl->byIndex, sizeof(nl->byIndex));
	nPos += sizeof(nl->byIndex);

	memcpy(pszBuf + nPos, &nl->wUserIDLen, sizeof(nl->wUserIDLen));
	nPos += sizeof(nl->wUserIDLen);

	memcpy(pszBuf + nPos, nl->strUserID, nl->wUserIDLen);
	nPos += nl->wUserIDLen;

	memcpy(pszBuf + nPos, &nl->wUserNameLen, sizeof(nl->wUserNameLen));
	nPos += sizeof(nl->wUserNameLen);

	memcpy(pszBuf + nPos, nl->strUserName, nl->wUserNameLen);
	nPos += nl->wUserNameLen;

	memcpy(pszBuf + nPos, &nl->wUserJobLen, sizeof(nl->wUserJobLen));
	nPos += sizeof(nl->wUserJobLen);

	memcpy(pszBuf + nPos, nl->strUserJob, nl->wUserJobLen);
	nPos += nl->wUserJobLen;

	memcpy(pszBuf + nPos, &nl->byUserGender, sizeof(nl->byUserGender));
	nPos += sizeof(nl->byUserGender);

	memcpy(pszBuf + nPos, &nl->nSortIndex, sizeof(nl->nSortIndex));
	nPos += sizeof(nl->nSortIndex);

	nl->ph.dwDataLen = nPos - getSizeOfPH(&nl->ph);
	writePH(pszBuf, &nl->ph);
}

static void readNotifyLogin(struct NOTIFY_LOGIN *nl, char * pszBuf)
{
	int nPos = 0;

	readPH(&nl->ph, pszBuf);
	nPos += getSizeOfPH(&nl->ph);

	memcpy(&nl->byIndex, pszBuf + nPos, sizeof(nl->byIndex));
	nPos += sizeof(nl->byIndex);

	memcpy(&nl->wUserIDLen, pszBuf + nPos, sizeof(nl->wUserIDLen));
	nPos += sizeof(nl->wUserIDLen);

	memcpy(nl->strUserID, pszBuf + nPos, nl->wUserIDLen);
	nPos += nl->wUserIDLen;

	memcpy(&nl->wUserNameLen, pszBuf + nPos, sizeof(nl->wUserNameLen));
	nPos += sizeof(nl->wUserNameLen);

	memcpy(nl->strUserName, pszBuf + nPos, nl->wUserNameLen);
	nPos += nl->wUserNameLen;

	memcpy(&nl->wUserJobLen, pszBuf + nPos, sizeof(nl->wUserJobLen));
	nPos += sizeof(nl->wUserJobLen);

	memcpy(nl->strUserJob, pszBuf + nPos, nl->wUserJobLen);
	nPos += nl->wUserJobLen;

	memcpy(&nl->byUserGender, pszBuf + nPos, sizeof(nl->byUserGender));
	nPos += sizeof(nl->byUserGender);

	memcpy(&nl->nSortIndex, pszBuf + nPos, sizeof(nl->nSortIndex));
	nPos += sizeof(nl->nSortIndex);
}*/

struct rakwon_user_info {
	int 			byIndex;
	unsigned char	strUserID[MAX_SQL_DATA];
	unsigned char	strUserPassword[MAX_SQL_DATA];
	unsigned char strUserName[MAX_SQL_DATA];
	unsigned char strUserJob[MAX_SQL_DATA];
	int byUserGender;
	int nSortIndex;
	struct rakwon_user_info *next;
};

/*! \brief Structure used for each RAKWON dialog, ie. a call, a registration, a subscribe.
 * Created and initialized by rakwon_alloc(), the descriptor goes into the list of
 * descriptors (dialoglist).
 */
struct rakwon_pvt {
	struct rakwon_pvt *next;			/*!< Next dialog in chain */
	int m_b_disconnect;
	int m_b_speaker;
	int m_b_reqspeaking;
	int m_i_member_count;
	int m_i_speaker;
	int m_i_video_promoter;
	int m_i_alive;
	struct rakwon_user_info m_local_user_info;
	struct tris_rtp *m_audio;
	struct tris_rtp *m_video_promoter;
	struct tris_rtp *m_video_speaker;
	struct tris_channel *owner;		/*!< Who owns us (if we have an owner) */
	struct tris_tcptls_session_instance *m_tcp_session;
	pthread_t m_thread;
	struct sockaddr_in m_server_address;
	struct sockaddr_in m_local_address;
	unsigned int m_ui_server_port;
	struct tris_conference *conf ;
	struct tris_conf_member *member;
	int alert_pipe[2];
	struct rakwon_user_info m_user_info;
	enum invitestates invitestate;
	struct tris_codec_pref prefs;		/*!< codec prefs */
	int capability;				/*!< Special capability (codec) */
	int jointcapability;			/*!< Supported capability at both ends (codecs) */
	int peercapability;			/*!< Supported peer capability */
	int prefcodec;				/*!< Preferred codec (outbound only) */
	int noncodeccapability;			/*!< DTMF RFC2833 telephony-event */
	int jointnoncodeccapability;            /*!< Joint Non codec capability */
	int redircodecs;			/*!< Redirect codecs */
	int maxcallbitrate;			/*!< Maximum Call Bitrate for Video Calls */	
	struct tris_flags flags[2];		/*!< RAKWON_ flags */
	time_t lastrtprx;			/*!< Last RTP received */
	time_t lastrtptx;			/*!< Last RTP sent */
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(uri);		/*!< Original requested URI */
		TRIS_STRING_FIELD(useragent);	/*!< User agent in SIP request */
	);
}; 

/*-- utility functions --*/
//static float uc_charlen(unsigned short *ucText, int len);
static int utf8_to_unicode(const unsigned char *utf8, int cc, unsigned short *unicode16);
//static  int unicode_to_utf8 (const unsigned short *unicode16, int len, char* utf8);

/*--- PBX Interface Functions ---*/
static struct tris_channel *rakwon_request_call(const char *type, int format, void *data, int *cause, struct tris_channel* src);
//static int rakwon_devicestate(void *data);
static int rakwon_sendtext(struct tris_channel *ast, const char *text);
static int rakwon_call(struct tris_channel *ast, char *dest, int timeout);
static int rakwon_hangup(struct tris_channel *ast);
static int rakwon_answer(struct tris_channel *ast);
static struct tris_frame *rakwon_read(struct tris_channel *ast);
static int rakwon_write(struct tris_channel *ast, struct tris_frame *frame);
static int rakwon_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen);
//static const char *rakwon_get_callid(struct tris_channel *chan);

/*--- Transmitting responses and requests */
static int rakwon_prepare_socket(struct rakwon_pvt *p);
static int transmit_request(struct rakwon_pvt *p, int sigmethod);
static void handle_response_login(struct rakwon_pvt * p, char * pBuf);
static void handle_response_ready(struct rakwon_pvt * p, char * pBuf);
static void handle_response_exit(struct rakwon_pvt * p, char * pBuf);
static void handle_response_speaking(struct rakwon_pvt * p, char * pBuf);
static void handle_notify_exit(struct rakwon_pvt * p, char * pBuf);
static int rakwon_tcptls_write(struct tris_tcptls_session_instance *tcptls_session, const void *buf, size_t len);
static int rakwon_tcptls_stop(struct tris_tcptls_session_instance *tcptls_session);

/*--- Dialog Related Functions */
static struct rakwon_pvt * rakwon_destroy(struct rakwon_pvt *p);
static struct rakwon_pvt *rakwon_alloc(tris_string_field callid, struct sockaddr_in *sin);
static void *dialog_unlink_all(struct rakwon_pvt *dialog, int lockowner, int lockdialoglist);
static struct tris_channel *rakwon_new(struct rakwon_pvt *i, int state, const char *title);

static enum tris_rtp_get_result rakwon_get_vrtp_peer(struct tris_channel *chan, struct tris_rtp **rtp);
static enum tris_rtp_get_result rakwon_get_trtp_peer(struct tris_channel *chan, struct tris_rtp **rtp);
static int rakwon_set_rtp_peer(struct tris_channel *chan, struct tris_rtp *rtp, struct tris_rtp *vrtp, struct tris_rtp *trtp, int codecs, int nat_active);
static int rakwon_get_codec(struct tris_channel *chan);
static int rakwon_senddigit_end(struct tris_channel *ast, char digit, unsigned int duration);
static const char* rakwon_useragent(char * exten);

/* convert utf8 as 3 bytes long to unicode16, if utf8 is ratin then it occupies 2 bytes */
static int utf8_to_unicode(const unsigned char *utf8, int cc, unsigned short *unicode16)
{
    int count = 0;
    unsigned char c0, c1;
    uint32_t scalar;

    if (cc < 0)
        cc = strlen((char*)utf8);
    
    while(--cc >= 0) {
        c0 = *utf8++;
        /*DPRINTF("Trying: %02x\n",c0);*/

        if ( c0 < 0x80 ) {
            /* Plain ASCII character, simple translation :-) */
            *unicode16++ = c0;
            count++;
            continue;
        }

        if ( (c0 & 0xc0) == 0x80 )
            /* Illegal; starts with 10xxxxxx */
            return -1;

        /* c0 must be 11xxxxxx if we get here => at least 2 bytes */
        scalar = c0;
        if(--cc < 0)
            return -1;
        c1 = *utf8++;
        /*DPRINTF("c1=%02x\n",c1);*/
        if ( (c1 & 0xc0) != 0x80 )
            /* Bad byte */
            return -1;
        scalar <<= 6;
        scalar |= (c1 & 0x3f);

        if ( !(c0 & 0x20) ) {
            /* Two bytes UTF-8 */
            if ( (scalar != 0) && (scalar < 0x80) )
                return -1;	/* Overlong encoding */
            *unicode16++ = scalar & 0x7ff;
            count++;
            continue;
        }

        /* c0 must be 111xxxxx if we get here => at least 3 bytes */
        if(--cc < 0)
            return -1;
        c1 = *utf8++;
        /*DPRINTF("c1=%02x\n",c1);*/
        if ( (c1 & 0xc0) != 0x80 )
            /* Bad byte */
            return -1;
        scalar <<= 6;
        scalar |= (c1 & 0x3f);

        if ( !(c0 & 0x10) ) {
            /*DPRINTF("####\n");*/
            /* Three bytes UTF-8 */
            if ( scalar < 0x800 )
                return -1;	/* Overlong encoding */
            if ( scalar >= 0xd800 && scalar < 0xe000 )
                return -1;	/* UTF-16 high/low halfs */
            *unicode16++ = scalar & 0xffff;
            count++;
            continue;
        }

        /* c0 must be 1111xxxx if we get here => at least 4 bytes */
        c1 = *utf8++;
        if(--cc < 0)
            return -1;
        /*DPRINTF("c1=%02x\n",c1);*/
        if ( (c1 & 0xc0) != 0x80 )
            /* Bad byte */
            return -1;
        scalar <<= 6;
        scalar |= (c1 & 0x3f);

        if ( !(c0 & 0x08) ) {
            /* Four bytes UTF-8, needs encoding as surrogates */
            if ( scalar < 0x10000 )
                return -1;	/* Overlong encoding */
            scalar -= 0x10000;
            *unicode16++ = ((scalar >> 10) & 0x3ff) + 0xd800;
            *unicode16++ = (scalar & 0x3ff) + 0xdc00;
            count += 2;
            continue;
        }

        return -1;	/* No support for more than four byte UTF-8 */
    }
    return count;
}

/*static float uc_charlen(unsigned short *ucText, int len)
{
	float len_by_fullchar = 0.0;
	int i=0;
	for (; i<len; i++) {
		if (ucText[i] > 0xFF)
			len_by_fullchar += 1.0;
		else
			len_by_fullchar += 0.5;
	}
	return len_by_fullchar;
}*/

/*#define UTF8_LENGTH(Char)                       \
  ((Char) < 0x80 ? 1 :                          \
   ((Char) < 0x800 ? 2 : 3))

static int g_unichar_to_utf8 (unsigned short c, char * outbuf)
 {
   unsigned int len = 0;
   int first;
   int i;
 
   if (c < 0x80)
     {
       first = 0;
       len = 1;
     }
   else if (c < 0x800)
     {
       first = 0xc0;
       len = 2;
     }
   else
     {
       first = 0xe0;
       len = 3;
     }
 
   if (outbuf)
     {
       for (i = len - 1; i > 0; --i)
         {
           outbuf[i] = (c & 0x3f) | 0x80;
           c >>= 6;
         }
       outbuf[0] = c | first;
     }
 
   return len;
 }*/
 
/* convert unicode16 to utf8 as 3 bytes long , if unicode16 is ratin then it occupies 2 bytes */
/*static  int unicode_to_utf8 (const unsigned short *unicode16, int len, char* utf8)
 {
   int result_length;
   char *p;
   int i;
 
   result_length = 0;
   for (i = 0; len < 0 || i < len; i++)
     {
       if (!unicode16[i])
         break;
 
       result_length += UTF8_LENGTH (unicode16[i]);
     }
 
   p = utf8;
 
   i = 0;
   while (p < utf8 + result_length)
     p += g_unichar_to_utf8 (unicode16[i++], p);
 
   *p = '\0';
 
	return p-utf8;
 
 }*/


/*! \brief Definition of this channel for PBX channel registration */
static const struct tris_channel_tech rakwon_tech = {
	.type = "RAKWON",
	.description = "Rakwon Video Conference (RVC)",
	.capabilities = TRIS_FORMAT_AUDIO_MASK, /* all audio formats */
	.properties = TRIS_CHAN_TP_WANTSJITTER | TRIS_CHAN_TP_CREATESJITTER,
	.requester = rakwon_request_call,			/* called with chan unlocked */
/*	.devicestate = rakwon_devicestate, 		 called with chan unlocked (not chan-specific) */
	.call = rakwon_call,			/* called with chan locked */
	.hangup = rakwon_hangup,			/* called with chan locked */
	.answer = rakwon_answer,			/* called with chan locked */
	.read = rakwon_read,			/* called with chan locked */
	.write = rakwon_write, 		/* called with chan locked */
	.write_video = rakwon_write,		/* called with chan locked */
	.write_text = rakwon_write,
	.indicate = rakwon_indicate,		/* called with chan locked */
	.bridge = tris_rtp_bridge,			/* XXX chan unlocked ? */
	.early_bridge = tris_rtp_early_bridge,
	.send_text = rakwon_sendtext,		/* called with chan locked */
	.send_digit_end = rakwon_senddigit_end,
/*	.get_pvt_uniqueid = rakwon_get_callid,*/
};

/*static int rakwon_devicestate(void *data)
{
	tris_log(LOG_NOTICE, "XXX Implement RakWon Devicestate XXX\n");

	return -1;
}*/

static int rakwon_answer(struct tris_channel *ast)
{
	tris_log(LOG_NOTICE, "XXX Implement RakWon Answer XXX\n");

	return -1;
}

static int rakwon_indicate(struct tris_channel *ast, int condition, const void *data, size_t datalen)
{
	tris_log(LOG_NOTICE, "XXX Implement RakWon indicate XXX\n");

	return -1;
}

static int rakwon_sendtext(struct tris_channel *ast, const char *text)
{
	tris_log(LOG_NOTICE, "XXX Implement RakWon sendtext XXX\n");

	return -1;
}

/*static const char* rakwon_get_callid(struct tris_channel *chan)
{
	tris_log(LOG_NOTICE, "XXX Implement RakWon get_callid XXX\n");

	return 0;
}*/

/*! \brief Interface structure with callbacks used to connect to RTP module */
static struct tris_rtp_protocol rakwon_rtp = {
	.type = "RAKWON",
	.get_rtp_info = rakwon_get_rtp_peer,
	.get_vrtp_info = rakwon_get_vrtp_peer,
	.get_trtp_info = rakwon_get_trtp_peer,
	.set_rtp_peer = rakwon_set_rtp_peer,
	.get_codec = rakwon_get_codec,
};

/*! \brief Definition of a thread that handles a socket */
struct rakwon_threadinfo {
	int stop;
	int alert_pipe[2]; /*! Used to alert tcptls thread when packet is ready to be written */
	pthread_t threadid;
	struct tris_tcptls_session_instance *tcptls_session;
	enum rakwon_transport type;	/*!< We keep a copy of the type here so we can display it in the connection list */
	TRIS_LIST_HEAD_NOLOCK(, tcptls_packet) packet_q;
};

enum rakwon_tcptls_alert {
	/*! \brief There is new data to be sent out */
	TCPTLS_ALERT_DATA,
	/*! \brief A request to stop the tcp_handler thread */
	TCPTLS_ALERT_STOP,
};

struct tcptls_packet {
	TRIS_LIST_ENTRY(tcptls_packet) entry;
	struct tris_str *data;
	size_t len;
};

/*! \brief  The table of TCP threads */
static struct ao2_container *threadt;

#define rakwon_pvt_lock(x) ao2_lock(x)
#define rakwon_pvt_trylock(x) ao2_trylock(x)
#define rakwon_pvt_unlock(x) ao2_unlock(x)

static const char * rakwon_useragent(char * exten)
{
	char sql[MAX_SQL_STAT];
	char tmp[200];
	char * user_agent = malloc(200);
	
	snprintf(sql, sizeof(sql), "select user_agent from location where uid like '%s' limit 1", exten);
	sql_select_query_execute(tmp, sql);
	if(strlen(tmp) == 0){
		tris_log(LOG_ERROR, "Cannot pick the user agent corresponding caller '%s'.\n", exten);
		strcpy(tmp, "unkonw");
	}
	
	strcpy(user_agent, tmp);
	return user_agent;
}

/*! \brief Send DTMF character on RAKWON channel
	within one call, we're able to transmit in many methods simultaneously */
static int rakwon_senddigit_end(struct tris_channel *ast, char digit, unsigned int duration)
{
	struct rakwon_pvt *p = ast->tech_pvt;
	int res = 0;

	rakwon_pvt_lock(p);
	if (digit == '5') {
		if (!p->m_b_speaker && !p->m_b_reqspeaking) {
				if(ast->_bridge) tris_play_and_wait(ast->_bridge, "videoconf/requested_speaking");
				transmit_request(p, TYPE_REQ_SET_SPEAKING);
				p->m_b_reqspeaking = 1;
		}
	} else if (digit == '1') {
		p->m_i_video_promoter = 1;
	} else if (digit == '2') {
		p->m_i_video_promoter = 0;
	}
	rakwon_pvt_unlock(p);

	return res;
}

/*! \brief Execute destruction of RAKWON dialog structure, release memory */
static void __rakwon_destroy(struct rakwon_pvt *p, int lockowner, int lockdialoglist)
{
	/* Unlink us from the owner if we have one */
	if (p->owner) {
		if (lockowner)
			tris_channel_lock(p->owner);
		if (option_debug)
			tris_log(LOG_DEBUG, "Detaching from %s\n", p->owner->name);
		p->owner->tech_pvt = NULL;
		/* Make sure that the channel knows its backend is going away */
		p->owner->_softhangup |= TRIS_SOFTHANGUP_DEV;
		if (lockowner)
			tris_channel_unlock(p->owner);
		/* Give the channel a chance to react before deallocation */
		usleep(1);
	}

	/* Remove link from peer to subscription of MWI */
	if (p->m_audio) {
		tris_rtp_destroy(p->m_audio);
	}
	if (p->m_video_promoter) {
		tris_rtp_destroy(p->m_video_promoter);
	}
	if (p->m_video_speaker) {
		tris_rtp_destroy(p->m_video_speaker);
	}

	tris_string_field_free_memory(p);

	if (p->m_tcp_session) {
		ao2_ref(p->m_tcp_session, -1);
		p->m_tcp_session = NULL;
	}
}

static struct rakwon_pvt * rakwon_destroy(struct rakwon_pvt *p)
{
	__rakwon_destroy(p, 1, 1);
	return NULL;
}

static void rakwon_destroy_fn(void *p)
{
	rakwon_destroy(p);
}


/*! \brief Allocate rakwon_pvt structure, set defaults and link in the container.
 * Returns a reference to the object so whoever uses it later must
 * remember to release the reference.
 */
static struct rakwon_pvt *rakwon_alloc(tris_string_field callid, struct sockaddr_in *sin)
{
	struct rakwon_pvt *p;

	if (!(p = ao2_t_alloc(sizeof(*p), rakwon_destroy_fn, "allocate a dialog(pvt) struct")))
		return NULL;

	if (tris_string_field_init(p, 512)) {
		ao2_t_ref(p, -1, "failed to string_field_init, drop p");
		return NULL;
	}

	p->m_audio = tris_rtp_new_with_bindaddr(sched, io, 1, 0, internip.sin_addr);
	tris_rtp_setqos(p->m_audio, global_tos_audio, global_cos_audio, "RAKWON AUDIO");
	tris_rtp_setdtmf(p->m_audio, 0);
	tris_rtp_setdtmfcompensate(p->m_audio, 0);
	tris_rtp_set_rtptimeout(p->m_audio, global_rtptimeout);
	tris_rtp_set_rtpholdtimeout(p->m_audio, global_rtpholdtimeout);
	tris_rtp_set_rtpkeepalive(p->m_audio, global_rtpkeepalive);

	p->m_i_video_promoter = 1;

	p->m_video_promoter = tris_rtp_new_with_bindaddr(sched, io, 1, 0, internip.sin_addr);
	tris_rtp_setqos(p->m_video_promoter, global_tos_audio, global_cos_audio, "RAKWON PVIDEO");
	tris_rtp_setdtmf(p->m_video_promoter, 0);
	tris_rtp_setdtmfcompensate(p->m_video_promoter, 0);
	tris_rtp_set_rtptimeout(p->m_video_promoter, global_rtptimeout);
	tris_rtp_set_rtpholdtimeout(p->m_video_promoter, global_rtpholdtimeout);
	tris_rtp_set_rtpkeepalive(p->m_video_promoter, global_rtpkeepalive);

	p->m_video_speaker = tris_rtp_new_with_bindaddr(sched, io, 1, 0, internip.sin_addr);
	tris_rtp_setqos(p->m_video_speaker, global_tos_audio, global_cos_audio, "RAKWON SVIDEO");
	tris_rtp_setdtmf(p->m_video_speaker, 0);
	tris_rtp_setdtmfcompensate(p->m_video_speaker, 0);
	tris_rtp_set_rtptimeout(p->m_video_speaker, global_rtptimeout);
	tris_rtp_set_rtpholdtimeout(p->m_video_speaker, global_rtpholdtimeout);
	tris_rtp_set_rtpkeepalive(p->m_video_speaker, global_rtpkeepalive);

	tris_rtp_codec_setpref(p->m_audio, &p->prefs);

	p->alert_pipe[0] = p->alert_pipe[1] = -1;
	if (pipe(p->alert_pipe) == -1) {
		ao2_t_ref(p, -1, "Failed to open alert pipe on rakwon_threadinfo");
		tris_log(LOG_ERROR, "Could not create rakwon alert pipe in tcptls thread, error %s\n", strerror(errno));
		return NULL;
	}

	p->m_i_alive = 1;
	return p;
}

static void tcptls_packet_destructor(void *obj)
{
	struct tcptls_packet *packet = obj;

	tris_free(packet->data);
}

static void rakwon_tcptls_client_args_destructor(void *obj)
{
	struct tris_tcptls_session_args *args = obj;
	if (args->tls_cfg) {
		tris_free(args->tls_cfg->certfile);
		tris_free(args->tls_cfg->cipher);
		tris_free(args->tls_cfg->cafile);
		tris_free(args->tls_cfg->capath);
	}
	tris_free(args->tls_cfg);
	tris_free((char *) args->name);
}

static void rakwon_threadinfo_destructor(void *obj)
{
	struct rakwon_threadinfo *th = obj;
	struct tcptls_packet *packet;
	if (th->alert_pipe[1] > -1) {
		close(th->alert_pipe[0]);
	}
	if (th->alert_pipe[1] > -1) {
		close(th->alert_pipe[1]);
	}
	th->alert_pipe[0] = th->alert_pipe[1] = -1;

	while ((packet = TRIS_LIST_REMOVE_HEAD(&th->packet_q, entry))) {
		ao2_t_ref(packet, -1, "thread destruction, removing packet from frame queue");
	}

	if (th->tcptls_session) {
		ao2_t_ref(th->tcptls_session, -1, "remove tcptls_session for rakwon_threadinfo object");
	}
}

/*! \brief creates a rakwon_threadinfo object and links it into the threadt table. */
static struct rakwon_threadinfo *rakwon_threadinfo_create(struct tris_tcptls_session_instance *tcptls_session)
{
	struct rakwon_threadinfo *th;

	if (!tcptls_session || !(th = ao2_alloc(sizeof(*th), rakwon_threadinfo_destructor))) {
		return NULL;
	}

	th->alert_pipe[0] = th->alert_pipe[1] = -1;

	if (pipe(th->alert_pipe) == -1) {
		ao2_t_ref(th, -1, "Failed to open alert pipe on rakwon_threadinfo");
		tris_log(LOG_ERROR, "Could not create rakwon alert pipe in tcptls thread, error %s\n", strerror(errno));
		return NULL;
	}
	ao2_t_ref(tcptls_session, +1, "tcptls_session ref for rakwon_threadinfo object");
	th->tcptls_session = tcptls_session;
	ao2_t_link(threadt, th, "Adding new tcptls helper thread");
	ao2_t_ref(th, -1, "Decrementing threadinfo ref from alloc, only table ref remains");
	return th;
}

/* encoding & decoding functions for packet on channel to RakwonServer */
static void encode_buffer(char* buf, int bufsize) {
	unsigned short usTemp;
	unsigned char ucTemp;
	int i;

	for (i = 0; i < bufsize; i++) {
		ucTemp = (unsigned char)buf[i];
		usTemp = ucTemp;
		if (usTemp < 0x50)
			usTemp = usTemp + 0x100;
		usTemp = usTemp - 0x50;
		ucTemp = (unsigned char)(usTemp & 0x00FF);
		buf[i] = (char)ucTemp;
	}
}
static void decode_buffer(char* buf, int bufsize) {
	unsigned short usTemp;
	unsigned char ucTemp;
	int i;

	for (i = 0; i < bufsize; i++) {
		ucTemp = (unsigned char)buf[i];
		usTemp = ucTemp + 0x50;
		if (usTemp >= 0x100)
			usTemp = usTemp - 0x100;
		ucTemp = (unsigned char)(usTemp & 0x00FF);
		buf[i] = (char)ucTemp;
	}
}

/*! \brief RAKWON TCP thread management function 
	This function reads from the socket, parses the packet into a request
*/
static void *_rakwon_tcp_helper_thread(struct rakwon_pvt *pvt, struct tris_tcptls_session_instance *tcptls_session) 
{
	int res, is_off = 0;
	struct PACKET_HEADER ph;
	int nBufSize, nReceivedSize;
	struct rakwon_threadinfo *me = NULL;
	char buf[RAKWON_MAXLEN_SIGPACK], *pBuf;
	struct pollfd fds[2] = { { 0 }, { 0 }, };
	struct tris_tcptls_session_args *ca = NULL;

	/* If this is a server session, then the connection has already been setup,
	 * simply create the threadinfo object so we can access this thread for writing.
	 * 
	 * if this is a client connection more work must be done.
	 * 1. We own the parent session args for a client connection.  This pointer needs
	 *    to be held on to so we can decrement it's ref count on thread destruction.
	 * 2. The threadinfo object was created before this thread was launched, however
	 *    it must be found within the threadt table.
	 * 3. Last, the tcptls_session must be started.
	 */
	if (!tcptls_session->client) {
		if (!(me = rakwon_threadinfo_create(tcptls_session))) {
			goto cleanup;
		}
		ao2_t_ref(me, +1, "Adding threadinfo ref for tcp_helper_thread");
	} else {
		struct rakwon_threadinfo tmp = {
			.tcptls_session = tcptls_session,
		};

		if ((!(ca = tcptls_session->parent)) ||
			(!(me = ao2_t_find(threadt, &tmp, OBJ_POINTER, "ao2_find, getting rakwon_threadinfo in tcp helper thread"))) ||
			(!(tcptls_session = tris_tcptls_client_start(tcptls_session)))) {

			goto cleanup;
		}
	}

	me->threadid = pthread_self();
	tris_debug(2, "Starting thread for %s server\n", tcptls_session->ssl ? "SSL" : "TCP");

	/* set up pollfd to watch for reads on both the socket and the alert_pipe */
	fds[0].fd = tcptls_session->fd;
	fds[1].fd = me->alert_pipe[0];
	fds[0].events = fds[1].events = POLLIN | POLLPRI;

	for (;;) {
		res = tris_poll(fds, 2, -1); /* polls for both socket and alert_pipe */

		if (res < 0) {
			tris_debug(2, "RAKWON %s server :: tris_wait_for_input returned %d\n", tcptls_session->ssl ? "SSL": "TCP", res);
			goto cleanup;
		}

		/* handle the socket event, check for both reads from the socket fd,
		 * and writes from alert_pipe fd */
		if (fds[0].revents) { /* there is data on the socket to be read */
			//tris_verbose("Receive Event!!!\n");
			fds[0].revents = 0;

			/* clear request structure */
			memset(buf, 0, sizeof(buf));

			//read from socket
			nReceivedSize = 0;
			nReceivedSize = tris_tcptls_server_read(tcptls_session, buf, sizeof(buf));
			
			if(nReceivedSize > 0)
				tris_verbose("Received packet data from socket, Data Len: %d; Tx: %s, Rx: %s\n", nReceivedSize, (const char*)pvt->m_user_info.strUserID, tris_inet_ntoa(tcptls_session->remote_address.sin_addr));
			else{
				//continue;
				tris_log(LOG_ERROR, "Closed the tcp connection to RakwonServer.\n");
				break;
			}

			decode_buffer(buf, nReceivedSize);
			pBuf = buf;
			
handle_buf:
			//handle response
			initPH(&ph);
			nBufSize = getSizeOfPH(&ph);
			
			if (nReceivedSize < nBufSize) {
				// Full message has not been received yet
				tris_log(LOG_WARNING, "Full message has not been received yet. PaketHeader Len: %d, Received Len: %d\n", nBufSize, nReceivedSize);
				goto cleanup;
			}
			
			char* pszBuf = malloc(nBufSize);
			memcpy(pszBuf, pBuf, nBufSize);

			readPH(&ph, pszBuf);
			free(pszBuf);

			// Check if the received message is valid
			if (ph.dwPacketID != PACKET_IDENTITY) {
				// Invalid TCP Server/Client message has been received
				tris_log(LOG_WARNING, "Invalid message on rakwon channel. packet_id: %x\n", ph.dwPacketID);
				goto cleanup;
			}

			nBufSize = getSizeOfPacket(&ph);
				
			// Valid TCP Server/Client message has been received
			// Process the received message on socket
			if (nReceivedSize < nBufSize) {
				// Full message has not been received yet
				tris_log(LOG_WARNING, "Full message has not been received yet. Total Len: %d, Received Len: %d\n", nBufSize, nReceivedSize);
				goto cleanup;
			}

			tris_verbose("Received Packet Type: %x, Packet Size: %d\n", ph.byPacketType, nBufSize);
			switch (ph.byPacketType) {
				//case TYPE_RES_REGISTER_USER:
				case TYPE_NOTIFY_USER_EXIT:
					handle_notify_exit(pvt, pBuf);
					break;
				//case TYPE_NOTIFY_CHANGE_FPS:
				case TYPE_RES_SET_SPEAKING:
					handle_response_speaking(pvt, pBuf);
					break;
				case TYPE_NOTIFY_ACCEPT_FAILED:
				case TYPE_RES_USER_READY:
					break;
				case TYPE_NOTIFY_START_CONF:
					handle_response_ready(pvt, pBuf);
					break;
				case TYPE_RES_LOGIN_CHECK:
					handle_response_login(pvt, pBuf);
					break;
				case TYPE_NOTIFY_USER_LOGIN:
				//	received_notify_user_login(pBuf);
					break;
				//case TYPE_NOTIFY_SEND_TEXT:
				//	received_notify_send_text(pBuf);
					break;
				case TYPE_RES_USER_EXIT:
					handle_response_exit(pvt, pBuf);
					break;
			}

			//sometimes, occur event receiving burst message at once.
			if (nReceivedSize > nBufSize) {
				pBuf += nBufSize;
				nReceivedSize -= nBufSize;
				goto handle_buf;
			}
		}

		if (fds[1].revents) { /* alert_pipe indicates there is data in the send queue to be sent */
			enum rakwon_tcptls_alert alert;
			struct tcptls_packet *packet;
			//tris_verbose("Send Event!!!\n");
			
			fds[1].revents = 0;

			if (read(me->alert_pipe[0], &alert, sizeof(alert)) == -1) {
				tris_log(LOG_ERROR, "read() failed: %s\n", strerror(errno));
				continue;
			}

			switch (alert) {
			case TCPTLS_ALERT_STOP:
				tris_log(LOG_WARNING, "TCPTLS thread alert_pipe indicated packet should be stop.\n");
				pvt->owner->_state = TRIS_STATE_OFFHOOK;
				goto cleanup;
			case TCPTLS_ALERT_DATA:
				ao2_lock(me);
				if (!(packet = TRIS_LIST_REMOVE_HEAD(&me->packet_q, entry))) {
					tris_log(LOG_WARNING, "TCPTLS thread alert_pipe indicated packet should be sent, but frame_q is empty");
				} else if (tris_tcptls_server_write(tcptls_session, tris_str_buffer(packet->data), packet->len) == -1) {
					tris_log(LOG_WARNING, "Failure to write to tcp/tls socket\n");
				}

				if (packet) {
					ao2_t_ref(packet, -1, "tcptls packet sent, this is no longer needed");
				}
				ao2_unlock(me);
				break;
			default:
				tris_log(LOG_ERROR, "Unknown tcptls thread alert '%d'\n", alert);
			}
		}
	}

	tris_debug(2, "Shutting down thread for %s server\n", tcptls_session->ssl ? "SSL" : "TCP");
	is_off = 1;

cleanup:
	if (me) {
		ao2_t_unlink(threadt, me, "Removing tcptls helper thread, thread is closing");
		ao2_t_ref(me, -1, "Removing tcp_helper_threads threadinfo ref");
	}

	/* if client, we own the parent session arguments and must decrement ref */
	if (ca) {
		ao2_t_ref(ca, -1, "closing tcptls thread, getting rid of client tcptls_session arguments");
	}

	if (tcptls_session) {
		tris_mutex_lock(&tcptls_session->lock);
		if (tcptls_session->f) {
			fclose(tcptls_session->f);
			tcptls_session->f = NULL;
		}
		if (tcptls_session->fd != -1) {
			close(tcptls_session->fd);
			tcptls_session->fd = -1;
		}
		tcptls_session->parent = NULL;
		tris_mutex_unlock(&tcptls_session->lock);

		ao2_ref(tcptls_session, -1);
		tcptls_session = NULL;
		pvt->m_tcp_session = NULL;
	}
	
	if(pvt->owner->_state == TRIS_STATE_DOWN){
		tris_verbose("Unable to connect to Rakwon Server.\n");
		char file2play[] = "videoconf/cannot_videoconf";
		tris_queue_control_data(pvt->owner, TRIS_CONTROL_TAKEOFFHOOK, file2play, strlen(file2play));
	}

	/* we hangup call because of server down. */
	if(is_off == 1){
		if(pvt->owner->_bridge) tris_play_and_wait(pvt->owner->_bridge, "videoconf/end_of_conf");
		tris_queue_control(pvt->owner, TRIS_CONTROL_HANGUP);
	}
	
	return NULL;
}

/*! \brief RAKWON TCP connection handler */
static void *rakwon_tcp_worker_fn(void *data)
{
	struct rakwon_pvt* p = data;
	struct tris_tcptls_session_instance *tcptls_session = p->m_tcp_session;

	return _rakwon_tcp_helper_thread(p, tcptls_session);
}

/*! \todo Get socket for dialog, prepare if needed, and return file handle  */
static int rakwon_prepare_socket(struct rakwon_pvt *p) 
{
	static const char name[] = "RAKWON socket";
	struct rakwon_threadinfo *th = NULL;
	struct tris_tcptls_session_args *ca;

	if (p->m_tcp_session && p->m_tcp_session->fd != -1) {
		return p->m_tcp_session->fd;
	}

	/* 3.  Create a new TCP/TLS client connection */
	/* create new session arguments for the client connection */
	if (!(ca = ao2_alloc(sizeof(*ca), rakwon_tcptls_client_args_destructor)) ||
		!(ca->name = tris_strdup(name))) {
		goto create_tcptls_session_fail;
	}
	ca->accept_fd = -1;
	memcpy(&ca->remote_address.sin_addr, &p->m_server_address.sin_addr, sizeof(ca->remote_address.sin_addr));
	ca->remote_address.sin_port = htons(p->m_ui_server_port);
	ca->remote_address.sin_family = AF_INET;

	/* Create a client connection for address, this does not start the connection, just sets it up. */
	if (!(p->m_tcp_session = tris_tcptls_client_create(ca))) {
		goto create_tcptls_session_fail;
	}

	/* client connections need to have the rakwon_threadinfo object created before
	 * the thread is detached.  This ensures the alert_pipe is up before it will
	 * be used.  Note that this function links the new threadinfo object into the
	 * threadt container. */
	if (!(th = rakwon_threadinfo_create(p->m_tcp_session))) {
		goto create_tcptls_session_fail;

	}

	/* Give the new thread a reference to the tcptls_session */
	ao2_ref(p->m_tcp_session, +1);

	if (tris_pthread_create_background(&ca->master, NULL, rakwon_tcp_worker_fn, p)) {
		tris_debug(1, "Unable to launch '%s'.", ca->name);
		ao2_ref(p->m_tcp_session, -1); /* take away the thread ref we just gave it */
		goto create_tcptls_session_fail;
	}

	return p->m_tcp_session->fd;

create_tcptls_session_fail:
	if (ca) {
		ao2_t_ref(ca, -1, "failed to create client, getting rid of client tcptls_session arguments");
	}
	if (p->m_tcp_session) {
		close(p->m_tcp_session->fd);
		p->m_tcp_session->fd = -1;
		ao2_ref(p->m_tcp_session, -1);
		p->m_tcp_session = NULL;
	}
	if (th) {
		ao2_t_unlink(threadt, th, "Removing tcptls thread info object, thread failed to open");
	}

	return -1;
}

static struct rakwon_pvt *dialog_ref(struct rakwon_pvt *p, char *tag)
{
	if (p)
		ao2_ref(p, 1);
	else
		tris_log(LOG_ERROR, "Attempt to Ref a null pointer\n");
	return p;
}

static struct rakwon_pvt *dialog_unref(struct rakwon_pvt *p, char *tag)
{
	if (p)
		ao2_ref(p, -1);
	return NULL;
}

/*!
 * \brief Unlink a dialog from the dialogs container, as well as any other places
 * that it may be currently stored.
 *
 * \note A reference to the dialog must be held before calling this function, and this
 * function does not release that reference.
 */
static void *dialog_unlink_all(struct rakwon_pvt *dialog, int lockowner, int lockdialoglist)
{
	dialog_ref(dialog, "Let's bump the count in the unlink so it doesn't accidentally become dead before we are done");

	/* Unlink us from the owner (channel) if we have one */
	if (dialog->owner) {
		if (lockowner)
			tris_channel_lock(dialog->owner);
		tris_debug(1, "Detaching from channel %s\n", dialog->owner->name);
		dialog->owner->tech_pvt = dialog_unref(dialog->owner->tech_pvt, "resetting channel dialog ptr in unlink_all");
		if (lockowner)
			tris_channel_unlock(dialog->owner);
	}
	
	dialog_unref(dialog, "Let's unbump the count in the unlink so the poor pvt can disappear if it is time");
	return NULL;
}


/*! \brief Initiate a call in the RAKWON channel
	called from rakwon_request_call (calls from the pbx ) for outbound channels
	and from handle_request_invite for inbound channels
	
*/
static struct tris_channel *rakwon_new(struct rakwon_pvt *i, int state, const char *title)
{
	struct tris_channel *tmp;
	int fmt;
	int what;
	int video;
	int text;
	int needvideo = 0;
	char buf[RAKWONBUFSIZE];

	{
		const char *my_name;	/* pick a good name */
	
		if (title) {
			my_name = title;
		} else {
			my_name = tris_strdupa(tris_inet_ntoa(i->m_server_address.sin_addr));
		}

		rakwon_pvt_unlock(i);
		/* Don't hold a rakwon pvt lock while we allocate a channel */
		//tmp = tris_channel_alloc(1, state, i->cid_num, i->cid_name, i->accountcode, i->exten, i->context, i->amaflags, "RAKWON/%s-%08x", my_name, tris_atomic_fetchadd_int((int *)&chan_idx, +1));
		tmp = tris_channel_alloc(1, state, "", "", "", "", "", 0, "RAKWON/%s-%08x", my_name, tris_atomic_fetchadd_int((int *)&chan_idx, +1));

	}
	if (!tmp) {
		tris_log(LOG_WARNING, "Unable to allocate AST channel structure for RAKWON channel\n");
		rakwon_pvt_lock(i);
		return NULL;
	}
	if (title && strncmp(title, "spc", 3))
		rakwon_pvt_lock(i);

	tmp->tech = &rakwon_tech;

	/* Select our native format based on codec preference until we receive
	   something from another device to the contrary. */
	if (i->jointcapability) { 	/* The joint capabilities of us and peer */
		what = i->jointcapability;
		video = i->jointcapability & TRIS_FORMAT_VIDEO_MASK;
		text = i->jointcapability & TRIS_FORMAT_TEXT_MASK;
	} else if (i->capability) {		/* Our configured capability for this peer */
		what = i->capability;
		video = i->capability & TRIS_FORMAT_VIDEO_MASK;
		text = i->capability & TRIS_FORMAT_TEXT_MASK;
	} else {
		what = global_capability;	/* Global codec support */
		video = global_capability & TRIS_FORMAT_VIDEO_MASK;
		text = global_capability & TRIS_FORMAT_TEXT_MASK;
	}

	/* Set the native formats for audio  and merge in video */
	tmp->nativeformats = tris_codec_choose(&i->prefs, what, 1) | video | text;
	tris_debug(3, "*** Our native formats are %s \n", tris_getformatname_multiple(buf, RAKWONBUFSIZE, tmp->nativeformats));
	tris_debug(3, "*** Joint capabilities are %s \n", tris_getformatname_multiple(buf, RAKWONBUFSIZE, i->jointcapability));
	tris_debug(3, "*** Our capabilities are %s \n", tris_getformatname_multiple(buf, RAKWONBUFSIZE, i->capability));
	tris_debug(3, "*** TRIS_CODEC_CHOOSE formats are %s \n", tris_getformatname_multiple(buf, RAKWONBUFSIZE, tris_codec_choose(&i->prefs, what, 1)));
	if (i->prefcodec)
		tris_debug(3, "*** Our preferred formats from the incoming channel are %s \n", tris_getformatname_multiple(buf, RAKWONBUFSIZE, i->prefcodec));

	/* XXX Why are we choosing a codec from the native formats?? */
	fmt = tris_best_codec(tmp->nativeformats);

	/* If we have a prefcodec setting, we have an inbound channel that set a 
	   preferred format for this call. Otherwise, we check the jointcapability
	   We also check for vrtp. If it's not there, we are not allowed do any video anyway.
	 */
	if (i->m_i_video_promoter) {
		if (i->m_video_promoter) {
			needvideo = TRIS_FORMAT_VIDEO_MASK;
		}
	} else {
		if (i->m_video_speaker) {
			needvideo = TRIS_FORMAT_VIDEO_MASK;
		}
	}

	if (needvideo) 
		tris_debug(3, "This channel can handle video! HOLLYWOOD next!\n");
	else
		tris_debug(3, "This channel will not be able to handle video.\n");

/*	if ((tris_test_flag(&i->flags[0], RAKWON_DTMF) == RAKWON_DTMF_INBAND) || (tris_test_flag(&i->flags[0], RAKWON_DTMF) == RAKWON_DTMF_AUTO) ||
	    (tris_test_flag(&i->flags[1], RAKWON_PAGE2_FAX_DETECT))) {
		int features = 0;

		if ((tris_test_flag(&i->flags[0], RAKWON_DTMF) == RAKWON_DTMF_INBAND) || (tris_test_flag(&i->flags[0], RAKWON_DTMF) == RAKWON_DTMF_AUTO)) {
			features |= DSP_FEATURE_DIGIT_DETECT;
		}

		if (tris_test_flag(&i->flags[1], RAKWON_PAGE2_FAX_DETECT)) {
			features |= DSP_FEATURE_FAX_DETECT;
		}

		i->dsp = tris_dsp_new();
		tris_dsp_set_features(i->dsp, features);
		if (global_relaxdtmf)
			tris_dsp_set_digitmode(i->dsp, DSP_DIGITMODE_DTMF | DSP_DIGITMODE_RELAXDTMF);
         }
*/
	if (state == TRIS_STATE_RING)
		tmp->rings = 1;
	tmp->adsicpe = TRIS_ADSI_UNAVAILABLE;
	tmp->writeformat = fmt;
	tmp->rawwriteformat = fmt;
	tmp->readformat = fmt;
	tmp->rawreadformat = fmt;
	tmp->tech_pvt = dialog_ref(i, "rakwon_new: set chan->tech_pvt to i");

/*	tmp->callgroup = i->callgroup;
	tmp->pickupgroup = i->pickupgroup;
	tmp->cid.cid_pres = i->callingpres;
	if (!tris_strlen_zero(i->parkinglot))
		tris_string_field_set(tmp, parkinglot, i->parkinglot);
	if (!tris_strlen_zero(i->accountcode))
		tris_string_field_set(tmp, accountcode, i->accountcode);
	if (i->amaflags)
		tmp->amaflags = i->amaflags;
*/	if(!tris_strlen_zero(default_language))
		tris_string_field_set(tmp, language, default_language);
/*	else if (!tris_strlen_zero(i->language))
		tris_string_field_set(tmp, language, i->language);
*/	i->owner = tmp;
	tris_module_ref(tris_module_info->self);
//	tris_copy_string(tmp->context, i->context, sizeof(tmp->context));
	/*Since it is valid to have extensions in the dialplan that have unescaped characters in them
	 * we should decode the uri before storing it in the channel, but leave it encoded in the rakwon_pvt
	 * structure so that there aren't issues when forming URI's
	 */
//	decoded_exten = tris_strdupa(i->exten);
//	tris_uri_decode(decoded_exten);
//	tris_copy_string(tmp->exten, decoded_exten, sizeof(tmp->exten));

	tmp->priority = 1;
//	if (i->rtp)
//		tris_jb_configure(tmp, &global_jbconf);

	return tmp;
}

/*! \brief PBX interface function -build RAKWON pvt structure 
 *	RAKWON calls initiated by the PBX arrive here. 
 *
 * \verbatim	
 * 	RAKWON Dial string syntax
 *		RAKWON/exten@host!dnid
 *	or	RAKWON/host/exten!dnid
 *	or	RAKWON/host!dnid
 * \endverbatim
*/
static struct tris_channel *rakwon_request_call(const char *type, int format, void *data, int *cause, struct tris_channel* src)
{
	struct rakwon_pvt *p;
	struct tris_channel *tmpc = NULL;
	char *ext = NULL, *host;
	char tmp[256];
	char *dest = data;
 	char *secret = NULL;
 	char *md5secret = NULL;
 	char *authname = NULL;
	char *trans = NULL;
	int oldformat = TRIS_FORMAT_SPEEX | TRIS_FORMAT_H264;
	char *caller_ext = NULL;
	//struct rakwon_server * server;
	char sql[MAX_SQL_STAT];
	
	//caller id
	//caller_ext = src->exten;
	caller_ext = src->cid.cid_num;
	if(!caller_ext){
		tris_log(LOG_ERROR, "Caller Empty.\n");
		*cause = TRIS_CAUSE_INCOMING_CALL_BARRED;
		
		//tris_streamfile(src, "rakwon/invalid_caller", src);
		tris_play_and_wait(src, "videoconf/cannot_videoconf");
		
		return NULL;
	}
	
	/* mask request with some set of allowed formats.
	 * XXX this needs to be fixed.
	 * The original code uses TRIS_FORMAT_AUDIO_MASK, but it is
	 * unclear what to use here. We have global_capabilities, which is
	 * configured from rakwon.conf, and rakwon_tech.capabilities, which is
	 * hardwired to all audio formats.
	 */
	format &= TRIS_FORMAT_AUDIO_MASK;
	if (!format) {
		tris_log(LOG_NOTICE, "Asked to get a channel of unsupported format %s while capability is %s\n", tris_getformatname(oldformat), tris_getformatname(global_capability));
		*cause = TRIS_CAUSE_BEARERCAPABILITY_NOTAVAIL;	/* Can't find codec to connect to host */

		tris_play_and_wait(src, "videoconf/cannot_videoconf");
		return NULL;
	}
	tris_debug(1, "Asked to create a RAKWON channel with formats: %s\n", tris_getformatname_multiple(tmp, sizeof(tmp), oldformat));

	if (!(p = rakwon_alloc(NULL, NULL))) {
		tris_log(LOG_ERROR, "Unable to build rakwon pvt data for '%s' (Out of memory or socket error)\n", dest);
		*cause = TRIS_CAUSE_SWITCH_CONGESTION;

		tris_play_and_wait(src, "videoconf/cannot_videoconf");
		return NULL;
	}

	/* get rakwon member info from db based on caller_ext */
	snprintf(sql, sizeof(sql), "select uid from %s where extension like '%s' limit 1", rakwon_conf.member_table, caller_ext);
	sql_select_query_execute(tmp, sql);
	if(strlen(tmp) == 0){
		tris_log(LOG_ERROR, "Cannot find rakwon member corresponding caller extension.\n");
		*cause = TRIS_CAUSE_UNREGISTERED;

		tris_play_and_wait(src, "videoconf/invalid_user");
		return NULL;
	}
	strcpy((char*)p->m_user_info.strUserID, tmp);
	
	snprintf(sql, sizeof(sql), "select pw from %s where extension like '%s' limit 1", rakwon_conf.member_table, caller_ext);
	sql_select_query_execute(tmp, sql);
	strcpy((char*)p->m_user_info.strUserPassword, tmp);
	
	/* Save the destination, the RAKWON dial string */
	tris_copy_string(tmp, dest, sizeof(tmp));
	
	tris_verbose("New call on Rakwon Channel, caller exten: %s, callee uri: %s\n", caller_ext, tmp);

	/* Find at sign - @ */
	host = strchr(tmp, '@');
	if (host) {
		*host++ = '\0';
		ext = tmp;
		secret = strchr(ext, ':');
	}
	if (secret) {
		*secret++ = '\0';
		md5secret = strchr(secret, ':');
	}
	if (md5secret) {
		*md5secret++ = '\0';
		authname = strchr(md5secret, ':');
	}
	if (authname) {
		*authname++ = '\0';
		trans = strchr(authname, ':');
	}

	if (!host) {
		ext = strchr(tmp, '/');
		if (ext) 
			*ext++ = '\0';
		host = tmp;
	}

	/* We now have 
		host = peer name, DNS host name or DNS domain (for SRV) 
		ext = extension (user part of URI)
		dnid = destination of the call (applies to the To: header)
	*/
	
	//get rakwon server's ip & port from conf based on callee's ext
	/*for(server = rakwon_conf.serverlist; server; server = server->next){
		if(!strcasecmp(server->exten, host)) break;
	}
	if(!server || strcasecmp(server->exten, host)){
		tris_log(LOG_WARNING, "Cannot find rakwon server corresponding destination extension.\n");
		*cause = TRIS_CAUSE_UNREGISTERED;
		return NULL;
	}*/

	//if(inet_pton(AF_INET, server->ip, &serverip.sin_addr)){
	if(inet_pton(AF_INET, rakwon_conf.default_server, &serverip.sin_addr)){
		memcpy(&p->m_server_address.sin_addr, &serverip.sin_addr, sizeof(p->m_server_address.sin_addr));
		p->m_server_address.sin_family = AF_INET;
	}else{
		tris_log(LOG_ERROR, "Cannot convert hostname '%s' to IN address.\n", host);
		*cause = TRIS_CAUSE_BEARERCAPABILITY_NOTIMPL;

		tris_play_and_wait(src, "videoconf/cannot_videoconf");
		return NULL;
	}
	//tris_string_field_set(p, m_server_address, server->ip);
	//p->m_ui_server_port = server->port;
	p->m_ui_server_port = rakwon_conf.default_port;

#if 0
	printf("Setting up to call extension '%s' at '%s'\n", ext ? ext : "<none>", host);
#endif
	p->prefcodec = oldformat;				/* Format for this call */
	p->jointcapability = oldformat;
	tris_string_field_set(p, useragent, rakwon_useragent(caller_ext));	/* set user_agent of sip_pvt */
	tris_verbose("rakwon_request_call() --- set user_agent with '%s'.\n", p->useragent);

	rakwon_pvt_lock(p);
	tmpc = rakwon_new(p, TRIS_STATE_DOWN, host);	/* Place the call */
	rakwon_pvt_unlock(p);
	if (!tmpc) {
		tris_play_and_wait(src, "videoconf/cannot_videoconf");
		
		dialog_unlink_all(p, 1, 1);
		/* rakwon_destroy(p); */
	}

	if(rakwon_prepare_socket(p) < 0){
		tris_log(LOG_ERROR, "Cannot create socket to RakwonServer at address %s:%d\n", tris_inet_ntoa(p->m_server_address.sin_addr), p->m_ui_server_port);
		*cause = TRIS_CAUSE_BEARERCAPABILITY_NOTIMPL;

		tris_play_and_wait(src, "videoconf/cannot_videoconf");
		return NULL;
	}else{
		tris_verbose("Creat Socket for RakwonServer at address %s:%d.\n", tris_inet_ntoa(p->m_server_address.sin_addr), p->m_ui_server_port);
	}
	
	dialog_unref(p, "toss pvt ptr at end of rakwon_request_call");
	tris_update_use_count();
//	restart_monitor();

	tris_verbose("Success new request to rakwon channel.\n");

	return tmpc;
}

/*! \brief Initiate Rakwon call from PBX 
 *      used from the dial() application      */
static int rakwon_call(struct tris_channel *ast, char *dest, int timeout)
{
	int res;
	int xmit;
	struct rakwon_pvt *p = ast->tech_pvt;	/* chan is locked, so the reference cannot go away */

	if ((ast->_state != TRIS_STATE_DOWN) && (ast->_state != TRIS_STATE_RESERVED)) {
		tris_log(LOG_WARNING, "rakwon_call called on %s, neither down nor reserved\n", ast->name);
		return -1;
	}

	res = 0;

	//res = update_call_counter(p, INC_CALL_RINGING);
	/*if (res == -1) {
		ast->hangupcause = TRIS_CAUSE_USER_BUSY;
		return res;
	}*/

	/* If there are no audio formats left to offer, punt */

	xmit = transmit_request(p, TYPE_REQ_LOGIN_CHECK);
	
	if (xmit == XMIT_ERROR){
		return XMIT_ERROR;
	}
	p->invitestate = INV_CALLING;

	/* Initialize auto-congest time */
/*	TRIS_SCHED_REPLACE_UNREF(p->initid, sched, p->timer_b, auto_congest, p, 
							dialog_unref(_data, "dialog ptr dec when SCHED_REPLACE del op succeeded"), 
							dialog_unref(p, "dialog ptr dec when SCHED_REPLACE add failed"),
							dialog_ref(p, "dialog ptr inc when SCHED_REPLACE add succeeded") );
*/	
	tris_verbose("Success call to RakwonServer.\n");
	
	return res;
}

/*! \brief Build req_login_check/req_user_ready message and transmit it 
 \param p rakwon_pvt structure
 \param sigmethod unknown 
*/
static int transmit_request(struct rakwon_pvt *p, int sigmethod)
{
	int res = -1;
	char * pszBuf = NULL;
	int nBufSize = 0, nStrLen = 0;
	struct REQ_LOGIN_CHECK lreq;
	struct REQ_COMMON creq;

	switch(sigmethod){
		case TYPE_REQ_LOGIN_CHECK:
			
			initReqLogin(&lreq);
			nStrLen = utf8_to_unicode(p->m_user_info.strUserID, strlen((char*)p->m_user_info.strUserID), lreq.strUserID);
			lreq.wUserIDLen = (nStrLen < 0) ? 0 : nStrLen * 2;
			nStrLen = utf8_to_unicode(p->m_user_info.strUserPassword, strlen((char*)p->m_user_info.strUserPassword), lreq.strUserPassword);
			lreq.wUserPasswordLen = (nStrLen < 0) ? 0 : nStrLen * 2;
			lreq.byUserType = RAKWON_USER_COMMON;

			nBufSize = getSizeOfReqLogin(&lreq);
			pszBuf = malloc(nBufSize);

			writeReqLogin(pszBuf, &lreq);
			encode_buffer(pszBuf, nBufSize);
			
			res = rakwon_tcptls_write(p->m_tcp_session, pszBuf, nBufSize);
			break;
		case TYPE_REQ_USER_READY:
			
			initReqCommon(&creq);
			creq.ph.byPacketType = sigmethod;
			creq.byIndex = p->m_local_user_info.byIndex;
			creq.byCode = ERR_REQ_USER_READY_INIT;

			nBufSize = getSizeOfReqComm(&creq);
			pszBuf = malloc(nBufSize);
			writeReqComm(pszBuf, &creq);
			encode_buffer(pszBuf, nBufSize);
			
			res = rakwon_tcptls_write(p->m_tcp_session, pszBuf, nBufSize);
			if(res < 0){
				tris_queue_control(p->owner, TRIS_CONTROL_TAKEOFFHOOK);
			}
			break;
		case TYPE_REQ_SET_SPEAKING:
		case TYPE_NOTIFY_LOUDING:
			
			initReqCommon(&creq);
			creq.ph.byPacketType = sigmethod;
			creq.byIndex = p->m_local_user_info.byIndex;
			creq.byCode = (sigmethod == TYPE_REQ_SET_SPEAKING) ? 0 : 1;
		
			nBufSize = getSizeOfReqComm(&creq);
			pszBuf = malloc(nBufSize);
			writeReqComm(pszBuf, &creq);
			encode_buffer(pszBuf, nBufSize);
			
			res = rakwon_tcptls_write(p->m_tcp_session, pszBuf, nBufSize);
			break;
		default:
			break;
	}
	
	/*sock_fd = rakwon_prepare_socket(p);
	if(sock_fd == -1){
		tris_log("Cannot connect to RakwonServer");
		return -1;
	}*/

	if (res == -1) {
		res = XMIT_ERROR;	/* Don't bother with trying to transmit again */
		tris_log(LOG_ERROR, "failed in rakwon_xmit: sigmethod: %d, paket: %p, len: %d\n", sigmethod, pszBuf, nBufSize);
	}
	if (res != 0)
		tris_log(LOG_NOTICE, "rakwon_xmit of %p (len %d) to %s:%d returned %d: %s\n", pszBuf, nBufSize, tris_inet_ntoa(p->m_server_address.sin_addr), p->m_ui_server_port, res, strerror(errno));

	if(pszBuf) free(pszBuf);
	
	return res;
}

/*! \brief used to indicate to a tcptls thread that data is ready to be written */
static int rakwon_tcptls_write(struct tris_tcptls_session_instance *tcptls_session, const void *buf, size_t len)
{
	int res = len;
	struct rakwon_threadinfo *th = NULL;
	struct tcptls_packet *packet = NULL;
	struct rakwon_threadinfo tmp = {
		.tcptls_session = tcptls_session,
	};
	enum rakwon_tcptls_alert alert = TCPTLS_ALERT_DATA;

	if (!tcptls_session) {
		return XMIT_ERROR;
	}

	tris_mutex_lock(&tcptls_session->lock);

	if ((tcptls_session->fd == -1) ||
		!(th = ao2_t_find(threadt, &tmp, OBJ_POINTER, "ao2_find, getting rakwon_threadinfo in tcp helper thread")) ||
		!(packet = ao2_alloc(sizeof(*packet), tcptls_packet_destructor)) ||
		!(packet->data = tris_str_create(len))) {
		goto tcptls_write_setup_error;
	}

	/* goto tcptls_write_error should _NOT_ be used beyond this point */
	tris_str_set(&packet->data, 0, "%s", (char *) buf);
	packet->len = len;

	/* alert tcptls thread handler that there is a packet to be sent.
	 * must lock the thread info object to guarantee control of the
	 * packet queue */
	ao2_lock(th);
	if (write(th->alert_pipe[1], &alert, sizeof(alert)) == -1) {
		tris_log(LOG_ERROR, "write() to alert pipe failed: %s\n", strerror(errno));
		ao2_t_ref(packet, -1, "could not write to alert pipe, remove packet");
		packet = NULL;
		res = XMIT_ERROR;
	} else { /* it is safe to queue the frame after issuing the alert when we hold the threadinfo lock */
		TRIS_LIST_INSERT_TAIL(&th->packet_q, packet, entry);
	}
	ao2_unlock(th);

	tris_mutex_unlock(&tcptls_session->lock);
	ao2_t_ref(th, -1, "In rakwon_tcptls_write, unref threadinfo object after finding it");
	return res;

tcptls_write_setup_error:
	if (th) {
		ao2_t_ref(th, -1, "In rakwon_tcptls_write, unref threadinfo obj, could not create packet");
	}
	if (packet) {
		tris_log(LOG_ERROR, "socket: %d, paket: yes, data: %p.\n", tcptls_session->fd, packet->data);
		ao2_t_ref(packet, -1, "could not allocate packet's data");
	}else{
		tris_log(LOG_ERROR, "socket: %d, paket: no.\n", tcptls_session->fd);
	}
	tris_mutex_unlock(&tcptls_session->lock);

	return XMIT_ERROR;
}

/*! \brief used to indicate to a tcptls thread that connection should be closed */
static int rakwon_tcptls_stop(struct tris_tcptls_session_instance *tcptls_session)
{
	struct rakwon_threadinfo *th = NULL;
	struct rakwon_threadinfo tmp = {
		.tcptls_session = tcptls_session,
	};
	enum rakwon_tcptls_alert alert = TCPTLS_ALERT_STOP;
	int res = 0;

	if (!tcptls_session) {
		return XMIT_ERROR;
	}

	tris_mutex_lock(&tcptls_session->lock);

	if ((tcptls_session->fd == -1) ||
		!(th = ao2_t_find(threadt, &tmp, OBJ_POINTER, "ao2_find, getting rakwon_threadinfo in tcp helper thread"))) {
		goto tcptls_write_setup_error;
	}

	/* alert tcptls thread handler that there is a packet to be sent.
	 * must lock the thread info object to guarantee control of the
	 * packet queue */
	ao2_lock(th);
	if (write(th->alert_pipe[1], &alert, sizeof(alert)) == -1) {
		tris_log(LOG_ERROR, "write stop to alert pipe failed: %s\n", strerror(errno));
		res = XMIT_ERROR;
	}
	ao2_unlock(th);

	tris_mutex_unlock(&tcptls_session->lock);
	ao2_t_ref(th, -1, "In rakwon_tcptls_stop, unref threadinfo object after finding it");
	return res;

tcptls_write_setup_error:
	if (th) {
		ao2_t_ref(th, -1, "In rakwon_tcptls_stop, unref threadinfo obj, could not close connection.");
	}
	tris_mutex_unlock(&tcptls_session->lock);

	return XMIT_ERROR;
}

/*! \brief Generate 32 byte random string for callid's etc */
static char *generate_random_string(char *buf, size_t size)
{
	long val[4];
	int x;

	for (x=0; x<4; x++)
		val[x] = tris_random();
	snprintf(buf, size, "%08lx%08lx%08lx%08lx", val[0], val[1], val[2], val[3]);

	return buf;
}
/* check if the channel's owner is speaker, this function will be used by mixer */
extern int isRakwonSpeaker(struct tris_channel *chan){
	if(chan->tech_pvt){
		struct rakwon_pvt * pvt = chan->tech_pvt;
		return pvt->m_b_speaker;
	}
	return 0;
}

static void *rakwon_mixer_fn(void *data)
{
	struct tris_channel* chan = (struct tris_channel*)data;
	char buf[64];
	member_exec(chan, generate_random_string(buf, sizeof(buf)));
	//char * buf = strdup("Rakwon");
	//member_exec(chan, buf);
	return NULL;
}

/* pBuf : decoded buffer */
static void handle_response_login(struct rakwon_pvt * p, char * pBuf)
{
	tris_verbose("Handling response logincheck...\n");
	struct RES_LOGIN_CHECK res;
	int iLocalPort;
	int iRemotePort;
	int iLocalPVideoPort;
	int iLocalSVideoPort;
	int iPromoterVideoPort;
	int iSpeakerVideoPort;
	struct sockaddr_in serverAddress;
	struct sockaddr_in localAddress;
	struct sockaddr_in sin;		/*!< media socket address */
	char file2play[40];

	initResLogin(&res);
	readResLogin(&res, pBuf);

	switch(res.byErrCode){
		case ERR_RES_LOGIN_CHECK_SUCCESS:
			p->m_local_user_info.byIndex = res.byIndex;

			iLocalPort = RTP_AUDIO_PORT(res.byIndex);
			iRemotePort = RTP_AUDIO_PORT_SRVR(res.byIndex);
			serverAddress = serverip;
			localAddress = serverip;
			iLocalPVideoPort = RTP_VIDEO_PORT(res.byIndex) + 1 * 2;
			iLocalSVideoPort = RTP_VIDEO_PORT(res.byIndex) + 2 * 2;
			iPromoterVideoPort = RTP_VIDEO_PORT_SRVR(res.byIndex);
			iSpeakerVideoPort = RTP_VIDEO_PORT_SRVR(res.byIndex);

			sin.sin_family = AF_INET;
			sin.sin_port = htons(iRemotePort);
			sin.sin_addr = serverAddress.sin_addr;
			tris_rtp_set_peer(p->m_audio, &sin);
			sin.sin_port = htons(iPromoterVideoPort);
			tris_rtp_set_peer(p->m_video_promoter, &sin);
			sin.sin_port = htons(iSpeakerVideoPort);
			tris_rtp_set_peer(p->m_video_speaker, &sin);
			sin.sin_port = htons(iLocalPort);
			sin.sin_addr = localAddress.sin_addr;
			tris_rtp_set_us(p->m_audio, &sin);
			sin.sin_port = htons(iLocalPVideoPort);
			tris_rtp_set_us(p->m_video_promoter, &sin);
			sin.sin_port = htons(iLocalSVideoPort);
			tris_rtp_set_us(p->m_video_speaker, &sin);

			/* Set file descriptors for audio, video, realtime text and UDPTL as needed */
			if (p->owner) {
				if (p->alert_pipe[0] > 0) {
					tris_channel_set_fd(p->owner, 0, p->alert_pipe[0]);
				}
				if (p->m_video_promoter) {
					tris_channel_set_fd(p->owner, 1, tris_rtp_fd(p->m_video_promoter));
				}
				if (p->m_video_speaker) {
					tris_channel_set_fd(p->owner, 2, tris_rtp_fd(p->m_video_speaker));
				}

				if (tris_pthread_create_background(&p->m_thread, NULL, rakwon_mixer_fn, p->owner)) {
					tris_debug(1, "Unable to launch mixer.");
				}
				
			}
			
			/* if you have successed, request user_ready to RakwonServer, then either answer OK to rakwon channel or not. */
			strcpy(file2play, "videoconf/wait_moment");
			tris_queue_control_data(p->owner, TRIS_CONTROL_ANSWER, file2play, strlen(file2play));

			/* it takes about 4s time to play greeting, so we wait a second before sending REQ_USER_READY.
			 this is necessary for normal signaling progress. */
			sleep(5);

			transmit_request(p, TYPE_REQ_USER_READY);
			break;
		case ERR_RES_LOGIN_CHECK_LOGGED_IN:
			tris_verbose("Either logged in RakwonServer is already at other place.\n");
			//strcpy(file2play, "videoconf/logged_in");
			//tris_queue_control_data(p->owner, TRIS_CONTROL_TAKEOFFHOOK, file2play, strlen(file2play));
			tris_rakwonchannel_hangup(p->owner);
			sleep(4);
			transmit_request(p, TYPE_REQ_LOGIN_CHECK);
			break;
		case ERR_RES_LOGIN_CHECK_EXCEED_LIMIT:
			tris_verbose("Cannot login the Rakwon Server cause of exceeding limit.\n");
			strcpy(file2play, "videoconf/exceed_limit");
			tris_queue_control_data(p->owner, TRIS_CONTROL_TAKEOFFHOOK, file2play, strlen(file2play));
			break;
		case ERR_RES_LOGIN_CHECK_DELETED_USER:
			tris_verbose("Cannot login the Rakwon Server cause of deleted user.\n");
			strcpy(file2play, "videoconf/deleted_user");
			tris_queue_control_data(p->owner, TRIS_CONTROL_TAKEOFFHOOK, file2play, strlen(file2play));
			break;
		case ERR_RES_LOGIN_CHECK_INVALID_PASSWORD:
			tris_verbose("Cannot login the Rakwon Server cause of incorrecting password.\n");
			strcpy(file2play, "videoconf/invalid_password");
			tris_queue_control_data(p->owner, TRIS_CONTROL_TAKEOFFHOOK, file2play, strlen(file2play));
			break;
		case ERR_RES_LOGIN_CHECK_INVALID_USER:
			tris_verbose("Cannot login the Rakwon Server cause of invalid user.\n");
			strcpy(file2play, "videoconf/invalid_user");
			tris_queue_control_data(p->owner, TRIS_CONTROL_TAKEOFFHOOK, file2play, strlen(file2play));
			break;
		case ERR_RES_LOGIN_CHECK_KEY_RECEIVING:
		case ERR_RES_LOGIN_CHECK_UNALLOWED_USER:
			tris_verbose("Fail to check login.\n");
			strcpy(file2play, "videoconf/unallowed_user");
			tris_queue_control_data(p->owner, TRIS_CONTROL_TAKEOFFHOOK, file2play, strlen(file2play));
			break;
		default:
			tris_verbose("Nothing to check login.\n");
			tris_queue_control(p->owner, TRIS_CONTROL_FORBIDDEN);
			break;
	}
	
	return;
}

static void handle_response_ready(struct rakwon_pvt * p, char * pBuf)
{
	tris_verbose("Handling response notify_conf_start...\n");
	struct RES_COMMON res;

	initResCommon(&res);
	readResComm(&res, pBuf);

	if(res.ph.byPacketType == TYPE_NOTIFY_START_CONF){
		/* playing start... */
		if(p->owner->_bridge) tris_play_and_wait(p->owner->_bridge, "videoconf/conf_start");
		
		/* send req_user_ready */
		struct REQ_COMMON creq;
		int nBufSize = 0;
		char * pszBuf;
		int ret;
		
		initReqCommon(&creq);
		creq.ph.byPacketType = TYPE_REQ_USER_READY;
		creq.byIndex = p->m_local_user_info.byIndex;
		creq.byCode = ERR_REQ_USER_READY_START;

		nBufSize = getSizeOfReqComm(&creq);
		pszBuf = malloc(nBufSize);
		writeReqComm(pszBuf, &creq);
		encode_buffer(pszBuf, nBufSize);
		
		ret = rakwon_tcptls_write(p->m_tcp_session, pszBuf, nBufSize);
		if (ret == -1) {
			tris_log(LOG_ERROR, "Cannot send message of user_ready_start to RakwonServer.\n");
			
			if(p->owner->_bridge) tris_play_and_wait(p->owner->_bridge, "videoconf/cannot_videoconf");
			tris_queue_control(p->owner, TRIS_CONTROL_HANGUP);
			rakwon_tcptls_stop(p->m_tcp_session);
		}

		free(pszBuf);
			
		/* start conference sessions */
		
	}	else if(res.ph.byPacketType == TYPE_RES_USER_READY && res.byErrCode == ERR_RES_USER_READY_INVALID_USER){
		tris_log(LOG_ERROR, "Invalid user. There is a ready user already.\n");
		
		if(p->owner->_bridge) tris_play_and_wait(p->owner->_bridge, "videoconf/logged_in");
		tris_queue_control(p->owner, TRIS_CONTROL_HANGUP);
		rakwon_tcptls_stop(p->m_tcp_session);
	}
	return;
}

static void handle_response_speaking(struct rakwon_pvt * p, char * pBuf)
{
	struct RES_COMMON res;

	initResCommon(&res);
	readResComm(&res, pBuf);

	if (res.byErrCode == ERR_RES_SET_SPEAKING_ACCEPTED && res.byIndex == p->m_local_user_info.byIndex && !p->m_b_speaker) {
		if(p->owner->_bridge) tris_play_and_wait(p->owner->_bridge, "videoconf/right_speaking");
		p->m_i_speaker = res.byIndex;
		p->m_b_speaker = 1;
		p->m_b_reqspeaking = 0;
		strcpy(speaker_agent, p->useragent);
		tris_log(LOG_WARNING, "handle_response_speaking() --- set user_agent of speaker with '%s'.\n", speaker_agent);
	} else if (res.byErrCode == ERR_RES_SET_SPEAKING_REJECTED && res.byIndex == p->m_local_user_info.byIndex) {
		tris_log(LOG_WARNING, "Set speaking rejected\n");
		p->m_b_reqspeaking = 0;
	} else if (res.byErrCode == ERR_RES_SET_SPEAKING_ACCEPTED && res.byIndex != p->m_local_user_info.byIndex) {
		if (p->m_b_speaker) {
			if(p->owner->_bridge) tris_play_and_wait(p->owner->_bridge, "videoconf/not_right_speaking");
			res.byErrCode = ERR_RES_SET_SPEAKING_CANCELED;
			p->m_b_speaker = 0;
		}
		p->m_i_speaker = res.byIndex;
	}
	
	return;
}

static void handle_response_exit(struct rakwon_pvt * p, char * pBuf)
{
	if(p->owner->_bridge) tris_play_and_wait(p->owner->_bridge, "videoconf/out_of_conf");
	
	/* stop media stream */
	
	/* stop video output */
	
	/* call hangup on rakwon channel */
	tris_queue_control(p->owner, TRIS_CONTROL_HANGUP);
	
	/* do rest of all */
	//close the signaling connection to RakwonServer
	if (p)
		rakwon_tcptls_stop(p->m_tcp_session);

	/* free channel pvt */
	p = dialog_unref(p, "unref chan->tech_pvt");
}

static void handle_notify_exit(struct rakwon_pvt * p, char * pBuf)
{
	struct RES_COMMON res;

	initResCommon(&res);
	readResComm(&res, pBuf);

	if (res.byIndex == 0 && p->owner->_bridge) {
		tris_play_and_wait(p->owner->_bridge, "videoconf/end_of_conf");
		tris_play_and_wait(p->owner->_bridge, "videoconf/wait_moment");

		/* it may be necessary to release media session.
		 RakwonServer runs cleanMedia() of all clients when promoter went out. */
	}
	
	return;
}

static int rakwon_hangup(struct tris_channel * chan)
{
	struct rakwon_pvt *p = NULL;

	if ((p = chan->tech_pvt)) {
		p->m_i_alive = 0;
		usleep(100000);
	}
	/* if BYE from Rakwon channel then answer */
	if(chan->_state == TRIS_STATE_UP){
		tris_queue_control(chan, TRIS_CONTROL_ANSWER);
	}

	/* request EXIT to rakwon server. */
	//do nothing. rather we close our tcp_socket to server through below operation.
	
	/* stop media stream */
	
	/* stop video output */

	/* do rest of all */
	//close the signaling connection to RakwonServer
	if (p)
		rakwon_tcptls_stop(((struct rakwon_pvt*)(chan->tech_pvt))->m_tcp_session);
	
	/* free channel pvt */
	chan->tech_pvt = dialog_unref(chan->tech_pvt, "unref chan->tech_pvt");
	
	return 0;
}

static int threadt_hash_cb(const void *obj, const int flags)
{
	const struct rakwon_threadinfo *th = obj;

	return (int) th->tcptls_session->remote_address.sin_addr.s_addr;
}

static int threadt_cmp_cb(void *obj, void *arg, int flags)
{
	struct rakwon_threadinfo *th = obj, *th2 = arg;

	return (th->tcptls_session == th2->tcptls_session) ? CMP_MATCH | CMP_STOP : 0;
}

/*! \brief Read RTP from network */
static struct tris_frame *rakwon_rtp_read(struct tris_channel *ast, struct rakwon_pvt *p, int *faxdetect)
{
	/* Retrieve audio/etc from channel.  Assumes p->lock is already held. */
	struct tris_frame *f;
	
	if (!p->m_audio) {
		/* We have no RTP allocated for this channel */
		return &tris_null_frame;
	}

	switch(ast->fdno) {
	case 0:
		f = rakwon_mixed_audio_read(p->m_audio, p->alert_pipe[0]);	/* RTP Audio */
		break;
	case 1:
		f = rakwon_video_read(p->m_video_promoter);	/* RTP Video */
		if (!p->m_i_video_promoter) {
			f = &tris_null_frame;
		}
		break;
	case 2:
		f = rakwon_video_read(p->m_video_speaker);	/* RTP Text */
		if (p->m_i_video_promoter || (!strncmp(speaker_agent, "VideoPhone", 10) && strncmp(p->useragent, "VideoPhone", 10))) {
			f = &tris_null_frame;
		}
		break;
	default:
		f = &tris_null_frame;
	}
	/* We already hold the channel lock */
	if (!p->owner || (f && f->frametype != TRIS_FRAME_VOICE))
		return f;

	if (f && f->subclass != (p->owner->nativeformats & TRIS_FORMAT_AUDIO_MASK)) {
		if (!(f->subclass & p->jointcapability)) {
			tris_debug(1, "Bogus frame of format '%s' received from '%s'!\n",
				tris_getformatname(f->subclass), p->owner->name);
			return &tris_null_frame;
		}
		tris_debug(1, "Oooh, format changed to %d %s\n",
			f->subclass, tris_getformatname(f->subclass));
		p->owner->nativeformats = (p->owner->nativeformats & (TRIS_FORMAT_VIDEO_MASK | TRIS_FORMAT_TEXT_MASK)) | f->subclass;
		tris_set_read_format(p->owner, p->owner->readformat);
		tris_set_write_format(p->owner, p->owner->writeformat);
	}

	return f;
}

/*! \brief Read RAKWON RTP from channel */
static struct tris_frame *rakwon_read(struct tris_channel *ast)
{
	struct tris_frame *fr;
	struct rakwon_pvt *p = ast->tech_pvt;
	int faxdetected = 0;

	rakwon_pvt_lock(p);
	fr = rakwon_rtp_read(ast, p, &faxdetected);

	/* Only allow audio through if they sent progress with SDP, or if the channel is actually answered */
	if (fr && fr->frametype == TRIS_FRAME_VOICE && ast->_state != TRIS_STATE_UP) {
		fr = &tris_null_frame;
	}

	rakwon_pvt_unlock(p);

	return fr;
}

/*! \brief Send frame to media channel (rtp) */
static int rakwon_write(struct tris_channel *ast, struct tris_frame *frame)
{
	struct rakwon_pvt *p = ast->tech_pvt;
	int res = 0;

	switch (frame->frametype) {
	case TRIS_FRAME_VOICE:
		if (p->m_b_speaker) {
			if (!(frame->subclass & ast->nativeformats)) {
				char s1[512], s2[512], s3[512];
				tris_log(LOG_WARNING, "Asked to transmit frame type %d, while native formats is %s(%d) read/write = %s(%d)/%s(%d)\n",
					frame->subclass, 
					tris_getformatname_multiple(s1, sizeof(s1) - 1, ast->nativeformats & TRIS_FORMAT_AUDIO_MASK),
					ast->nativeformats & TRIS_FORMAT_AUDIO_MASK,
					tris_getformatname_multiple(s2, sizeof(s2) - 1, ast->readformat),
					ast->readformat,
					tris_getformatname_multiple(s3, sizeof(s3) - 1, ast->writeformat),
					ast->writeformat);
				return 0;
			}
			if (p) {
				rakwon_pvt_lock(p);
				if (p->m_audio) {
					/* If channel is not up, activate early media session */
					p->lastrtptx = time(NULL);
					res = rakwon_rtp_write(p->m_audio, frame, 0);
				}
				rakwon_pvt_unlock(p);
			}
		}
		break;
	case TRIS_FRAME_VIDEO:
		if (p) {
			rakwon_pvt_lock(p);
			if (p->m_video_promoter) {
				/* Activate video early media */
				p->lastrtptx = time(NULL);
				res = rakwon_rtp_write(p->m_video_promoter, frame, 1);
			}
			rakwon_pvt_unlock(p);
		}
		break;
	case TRIS_FRAME_IMAGE:
		return 0;
		break;
	default: 
		tris_log(LOG_WARNING, "Can't send %d type frames with RAKWON write\n", frame->frametype);
		return 0;
	}

	return res;
}

struct tris_conf_member* rakwon_get_conf_member(struct tris_channel *chan)
{
	struct rakwon_pvt *p = NULL;

	if (!(p = chan->tech_pvt))
		return NULL;

	if (!(p->member)) {
		return NULL;
	}

	return p->member;
}

int rakwon_set_conf_member(struct tris_channel *chan, struct tris_conf_member* member)
{
	struct rakwon_pvt *p = NULL;

	if (!(p = chan->tech_pvt))
		return -1;

	p->member = member;
	return 0;
}

int rakwon_get_read_audiofd(struct tris_channel *chan)
{
	struct rakwon_pvt *p = NULL;

	if (!(p = chan->tech_pvt))
		return 0;

	return p->alert_pipe[0];
}

int rakwon_get_write_audiofd(struct tris_channel *chan)
{
	struct rakwon_pvt *p = NULL;

	if (!(p = chan->tech_pvt))
		return 0;

	return p->alert_pipe[1];
}

/*! \brief Returns null if we can't reinvite audio (part of RTP interface) */
enum tris_rtp_get_result rakwon_get_rtp_peer(struct tris_channel *chan, struct tris_rtp **rtp)
{
	struct rakwon_pvt *p = NULL;
	enum tris_rtp_get_result res = TRIS_RTP_TRY_PARTIAL;

	if (!(p = chan->tech_pvt))
		return TRIS_RTP_GET_FAILED;

	rakwon_pvt_lock(p);
	if (!(p->m_audio)) {
		rakwon_pvt_unlock(p);
		return TRIS_RTP_GET_FAILED;
	}

	*rtp = p->m_audio;

	rakwon_pvt_unlock(p);

	return res;
}

int rakwon_is_alive(struct tris_channel *chan)
{
	struct rakwon_pvt *p = NULL;

	if (!(p = chan->tech_pvt))
		return 0;

	return p->m_i_alive;
}

/*! \brief Returns null if we can't reinvite video (part of RTP interface) */
static enum tris_rtp_get_result rakwon_get_vrtp_peer(struct tris_channel *chan, struct tris_rtp **rtp)
{
	struct rakwon_pvt *p = NULL;
	enum tris_rtp_get_result res = TRIS_RTP_TRY_PARTIAL;
	
	if (!(p = chan->tech_pvt))
		return TRIS_RTP_GET_FAILED;

	rakwon_pvt_lock(p);
	if (p->m_i_video_promoter) {
		if (!(p->m_video_promoter)) {
			rakwon_pvt_unlock(p);
			return TRIS_RTP_GET_FAILED;
		}
		*rtp = p->m_video_promoter;
	} else {
		if (!(p->m_video_speaker)) {
			rakwon_pvt_unlock(p);
			return TRIS_RTP_GET_FAILED;
		}
		*rtp = p->m_video_speaker;
	}

	rakwon_pvt_unlock(p);

	return res;
}

/*! \brief Returns null if we can't reinvite text (part of RTP interface) */
static enum tris_rtp_get_result rakwon_get_trtp_peer(struct tris_channel *chan, struct tris_rtp **rtp)
{
	struct rakwon_pvt *p = NULL;
//	enum tris_rtp_get_result res = TRIS_RTP_TRY_PARTIAL;
	
	if (!(p = chan->tech_pvt))
		return TRIS_RTP_GET_FAILED;

	return TRIS_RTP_GET_FAILED;

//	return res;
}

/*! \brief Set the RTP peer for this call */
static int rakwon_set_rtp_peer(struct tris_channel *chan, struct tris_rtp *rtp, struct tris_rtp *vrtp, struct tris_rtp *trtp, int codecs, int nat_active)
{
	return 0;
}

/*! \brief Return RAKWON UA's codec (part of the RTP interface) */
static int rakwon_get_codec(struct tris_channel *chan)
{
	struct rakwon_pvt *p = chan->tech_pvt;
	return p->jointcapability ? p->jointcapability : p->capability;	
}


/*! \brief Re-read Rakwon.conf config file
 */
static int reload_config(enum channelreloadreason reason)
{
	const char config[] = "rakwon.conf";
	struct tris_config *cfg;
	char *cat;
	struct tris_flags config_flags = { reason == CHANNEL_MODULE_LOAD ? 0 :  0};
	struct tris_variable * v;
	
	cfg = tris_config_load(config, config_flags);

	/* We *must* have a config file otherwise stop immediately */
	if (!cfg) {
		tris_log(LOG_NOTICE, "Unable to load config %s\n", config);
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		/* Must reread both files, because one changed */
		tris_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		if ((cfg = tris_config_load(config, config_flags)) == CONFIG_STATUS_FILEINVALID) {
			tris_log(LOG_ERROR, "Contents of %s are invalid and cannot be parsed\n", config);
			return 1;
		}
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		tris_log(LOG_ERROR, "Contents of %s are invalid and cannot be parsed\n", config);
		return 1;
	} else {
		tris_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
	}

	if (cfg) {
		struct tris_variable *gen;
		struct rakwon_server * servers = NULL;
		const char * db_name, * table_name;
		const char *default_server, *default_port, *subject, *seats;

		/* read "genera" section and all of rakwon exten sections */
		gen = tris_variable_browse(cfg, "general");
		cat = tris_category_browse(cfg, NULL);
		while (cat) {
			if (!strcasecmp(cat, "general")) {
				/*extens = tris_variable_retrieve(cfg, cat, "servers");
				if (!tris_strlen_zero(extens)) {
					tris_copy_string(rakwon_conf.server_extens, extens, sizeof(rakwon_conf.server_extens));
				}else{
					rakwon_conf.server_extens = NULL;
				}*/
				db_name = tris_variable_retrieve(cfg, cat, "database");
				table_name = tris_variable_retrieve(cfg, cat, "member_table");
				default_server = tris_variable_retrieve(cfg, cat, "default_server");
				default_port = tris_variable_retrieve(cfg, cat, "default_port");
				subject = tris_variable_retrieve(cfg, cat, "subject");
				seats = tris_variable_retrieve(cfg, cat, "seats");

				tris_copy_string(rakwon_conf.db, db_name, sizeof(rakwon_conf.db));
				tris_copy_string(rakwon_conf.member_table, table_name, sizeof(rakwon_conf.member_table));
				tris_copy_string(rakwon_conf.default_server, default_server, sizeof(rakwon_conf.default_server));
				tris_copy_string(rakwon_conf.default_subject, subject, sizeof(rakwon_conf.default_subject));
				rakwon_conf.default_port = atoi(default_port);
				rakwon_conf.default_seats = atoi(seats);
			}else{
				struct rakwon_server * s;
				s = malloc(sizeof(struct rakwon_server));
				
				tris_copy_string(s->exten, cat, sizeof(s->exten));
				for (v = tris_variable_browse(cfg, (const char *)cat); v; v = v->next) {
					if(!strcasecmp(v->name, "ip"))	tris_copy_string(s->ip, v->value, sizeof(s->ip));
					else if(!strcasecmp(v->name, "port")) s->port = atoi(v->value);
					else if(!strcasecmp(v->name, "subject")) tris_copy_string(s->subject, v->value, sizeof(s->subject));
					else if(!strcasecmp(v->name, "seats")) s->port = atoi(v->value);
				}
				s->next = s->prev = NULL;
				
				if(!servers){
					servers = s;
				}else{
					servers->prev = s;
					s->next = servers;
					servers = s;
				}
			}
			cat = tris_category_browse(cfg, cat);
		}

		/* link server list from cfg file onto */
		rakwon_conf.serverlist = servers;
		
		tris_config_destroy(cfg);
	}

	/* Done, tell the manager */
	manager_event(EVENT_FLAG_SYSTEM, "ChannelReload", "ChannelType: Rakwon\r\nReloadReason: %s", channelreloadreason2txt(reason));

	return 0;
}

static int load_module(void)
{
	tris_verbose("Rakwon channel loading...\n");
	
	threadt = ao2_t_container_alloc(hash_dialog_size, threadt_hash_cb, threadt_cmp_cb, "allocate threadt table");
	
	rakwon_reloadreason = CHANNEL_MODULE_LOAD;

	if(reload_config(rakwon_reloadreason))	/* Load the configuration from rakwon.conf */
		return TRIS_MODULE_LOAD_DECLINE;

	if (!(sched = sched_context_create())) {
		tris_log(LOG_ERROR, "Unable to create scheduler context\n");
		return TRIS_MODULE_LOAD_FAILURE;
	}

	if (tris_channel_register(&rakwon_tech)) {
		tris_log(LOG_ERROR, "Unable to register channel type 'RAKWON'\n");
		return TRIS_MODULE_LOAD_FAILURE;
	}

	if (tris_rtp_proto_register(&rakwon_rtp) < 0) {
		return 0;
	}
	
	return 0;
}

static int unload_module(void)
{
	struct ao2_iterator i;
	struct rakwon_threadinfo *th;

	/* free resource of rakwon_conf */
	struct rakwon_server * s;
	for(s = rakwon_conf.serverlist; s; s = s->next){
		//free(s);
	}
	//free(rakwon_conf);

	tris_sched_dump(sched);
	
	/* Disconnect from the RTP subsystem */
	tris_rtp_proto_unregister(&rakwon_rtp);
	
	/* First, take us out of the channel type list */
	tris_channel_unregister(&rakwon_tech);
	
	/* Kill all existing TCP/TLS threads */
	i = ao2_iterator_init(threadt, 0);
	while ((th = ao2_t_iterator_next(&i, "iterate through tcp threads for 'rakwon show tcp'"))) {
		pthread_t thread = th->threadid;
		th->stop = 1;
		pthread_kill(thread, SIGURG);
		pthread_join(thread, NULL);
		ao2_t_ref(th, -1, "decrement ref from iterator");
	}
	ao2_iterator_destroy(&i);

	ao2_t_ref(threadt, -1, "unref the thread table");
	sched_context_destroy(sched);

	return 0;
}

static int reload(void)
{
	return 0;
}

TRIS_MODULE_INFO(TRISMEDIA_GPL_KEY, TRIS_MODFLAG_DEFAULT, "Rakwon Video Conference Protocol (Rakwon)",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );



