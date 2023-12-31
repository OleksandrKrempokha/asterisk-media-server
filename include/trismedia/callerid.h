/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief CallerID (and other GR30) management and generation
 * Includes code and algorithms from the Zapata library.
 *
 * \ref CID
 *
 */

/*!
 * \page CID Caller ID names and numbers
 *
 * Caller ID names are currently 8 bit characters, propably
 * ISO8859-1, depending on what your channel drivers handle.
 *
 * IAX2 and SIP caller ID names are UTF8
 * On ISDN Caller ID names are 7 bit, Almost ASCII
 * (See http://www.zytrax.com/tech/ia5.html )
 *
 * \note Trismedia does not currently support SIP utf8 caller ID names or caller ID's.
 *
 * \par See also
 * 	\arg \ref callerid.c
 * 	\arg \ref callerid.h
 *	\arg \ref Def_CallerPres
 */

#ifndef _TRISMEDIA_CALLERID_H
#define _TRISMEDIA_CALLERID_H

#define MAX_CALLERID_SIZE 32000

#define CID_PRIVATE_NAME 		(1 << 0)
#define CID_PRIVATE_NUMBER		(1 << 1)
#define CID_UNKNOWN_NAME		(1 << 2)
#define CID_UNKNOWN_NUMBER		(1 << 3)
#define CID_MSGWAITING			(1 << 4)
#define CID_NOMSGWAITING		(1 << 5)

#define CID_SIG_BELL	1
#define CID_SIG_V23	2
#define CID_SIG_DTMF	3
#define CID_SIG_V23_JP	4
#define CID_SIG_SMDI	5

#define CID_START_RING	1
#define CID_START_POLARITY 2
#define CID_START_POLARITY_IN 3

/* defines dealing with message waiting indication generation */
/*! MWI SDMF format */
#define CID_MWI_TYPE_SDMF		0x00
/*! MWI MDMF format -- generate only MWI field */
#define CID_MWI_TYPE_MDMF		0x01
/*! MWI MDMF format -- generate name, callerid, date and MWI fields */
#define CID_MWI_TYPE_MDMF_FULL	0x02

#define TRIS_LIN2X(a) ((codec == TRIS_FORMAT_ALAW) ? (TRIS_LIN2A(a)) : (TRIS_LIN2MU(a)))
#define TRIS_XLAW(a) ((codec == TRIS_FORMAT_ALAW) ? (TRIS_ALAW(a)) : (TRIS_MULAW(a)))


struct callerid_state;
typedef struct callerid_state CIDSTATE;

/*! \brief CallerID Initialization
 * \par
 * Initializes the callerid system.  Mostly stuff for inverse FFT
 */
void callerid_init(void);

/*! \brief Generates a CallerID FSK stream in ulaw format suitable for transmission.
 * \param buf Buffer to use. If "buf" is supplied, it will use that buffer instead of allocating its own.  "buf" must be at least 32000 bytes in size of you want to be sure you don't have an overrun.
 * \param number Use NULL for no number or "P" for "private"
 * \param name name to be used
 * \param flags passed flags
 * \param callwaiting callwaiting flag
 * \param codec -- either TRIS_FORMAT_ULAW or TRIS_FORMAT_ALAW
 * This function creates a stream of callerid (a callerid spill) data in ulaw format.
 * \return It returns the size
 * (in bytes) of the data (if it returns a size of 0, there is probably an error)
*/
int callerid_generate(unsigned char *buf, const char *number, const char *name, int flags, int callwaiting, int codec);

/*! \brief Create a callerID state machine
 * \param cid_signalling Type of signalling in use
 *
 * This function returns a malloc'd instance of the callerid_state data structure.
 * \return Returns a pointer to a malloc'd callerid_state structure, or NULL on error.
 */
struct callerid_state *callerid_new(int cid_signalling);

/*! \brief Read samples into the state machine.
 * \param cid Which state machine to act upon
 * \param ubuf containing your samples
 * \param samples number of samples contained within the buffer.
 * \param codec which codec (TRIS_FORMAT_ALAW or TRIS_FORMAT_ULAW)
 *
 * Send received audio to the Caller*ID demodulator.
 * \return Returns -1 on error, 0 for "needs more samples",
 * and 1 if the CallerID spill reception is complete.
 */
int callerid_feed(struct callerid_state *cid, unsigned char *ubuf, int samples, int codec);

/*! \brief Read samples into the state machine.
 * \param cid Which state machine to act upon
 * \param ubuf containing your samples
 * \param samples number of samples contained within the buffer.
 * \param codec which codec (TRIS_FORMAT_ALAW or TRIS_FORMAT_ULAW)
 *
 * Send received audio to the Caller*ID demodulator (for japanese style lines).
 * \return Returns -1 on error, 0 for "needs more samples",
 * and 1 if the CallerID spill reception is complete.
 */
int callerid_feed_jp(struct callerid_state *cid, unsigned char *ubuf, int samples, int codec);

/*! \brief Extract info out of callerID state machine.  Flags are listed above
 * \param cid Callerid state machine to act upon
 * \param number Pass the address of a pointer-to-char (will contain the phone number)
 * \param name Pass the address of a pointer-to-char (will contain the name)
 * \param flags Pass the address of an int variable(will contain the various callerid flags)
 *
 * This function extracts a callerid string out of a callerid_state state machine.
 * If no number is found, *number will be set to NULL.  Likewise for the name.
 * Flags can contain any of the following:
 *
 * \return Returns nothing.
 */
void callerid_get(struct callerid_state *cid, char **number, char **name, int *flags);

/*! Get and parse DTMF-based callerid  */
/*!
 * \param cidstring The actual transmitted string.
 * \param number The cid number is returned here.
 * \param flags The cid flags are returned here.
 * This function parses DTMF callerid.
 */
void callerid_get_dtmf(char *cidstring, char *number, int *flags);

/*! \brief Free a callerID state
 * \param cid This is the callerid_state state machine to free
 * This function frees callerid_state cid.
 */
void callerid_free(struct callerid_state *cid);

/*! \brief Generate Caller-ID spill from the "callerid" field of trismedia (in e-mail address like format)
 * \param buf buffer for output samples. See callerid_generate() for details regarding buffer.
 * \param name Caller-ID Name
 * \param number Caller-ID Number
 * \param codec Trismedia codec (either TRIS_FORMAT_ALAW or TRIS_FORMAT_ULAW)
 *
 * Acts like callerid_generate except uses an trismedia format callerid string.
 */
int tris_callerid_generate(unsigned char *buf, const char *name, const char *number, int codec);

/*! \brief Generate message waiting indicator
 *  \param active The message indicator state
 *  -- either 0 no messages in mailbox or 1 messages in mailbox
 *  \param type Format of message (any of CID_MWI_TYPE_*)
 *  \see callerid_generate() for more info as it use the same encoding
 *  \version 1.6.1 changed mdmf parameter to type, added name, number and flags for caller id message generation
*/
int vmwi_generate(unsigned char *buf, int active, int type, int codec, const char *name,
	const char *number, int flags);

/*! \brief Generate Caller-ID spill but in a format suitable for Call Waiting(tm)'s Caller*ID(tm)
 * See tris_callerid_generate() for other details
 */
int tris_callerid_callwaiting_generate(unsigned char *buf, const char *name, const char *number, int codec);

/*! \brief Destructively parse inbuf into name and location (or number)
 * Parses callerid stream from inbuf and changes into useable form, outputed in name and location.
 * \param instr buffer of callerid stream (in audio form) to be parsed. Warning, data in buffer is changed.
 * \param name address of a pointer-to-char for the name value of the stream.
 * \param location address of a pointer-to-char for the phone number value of the stream.
 * \return Returns 0 on success, -1 on failure.
 */
int tris_callerid_parse(char *instr, char **name, char **location);

/*! Generate a CAS (CPE Alert Signal) tone for 'n' samples */
/*!
 * \param outbuf Allocated buffer for data.  Must be at least 2400 bytes unless no SAS is desired
 * \param sas Non-zero if CAS should be preceeded by SAS
 * \param len How many samples to generate.
 * \param codec Which codec (TRIS_FORMAT_ALAW or TRIS_FORMAT_ULAW)
 * \return Returns -1 on error (if len is less than 2400), 0 on success.
 */
int tris_gen_cas(unsigned char *outbuf, int sas, int len, int codec);

/*! \brief Shrink a phone number in place to just digits (more accurately it just removes ()'s, .'s, and -'s... */
/*!
 * \param n The number to be stripped/shrunk
 * \return Returns nothing important
 */
void tris_shrink_phone_number(char *n);

/*! \brief Check if a string consists only of digits and + \#
    \param n number to be checked.
    \return Returns 0 if n is a number, 1 if it's not.
 */
int tris_isphonenumber(const char *n);

/*! \brief Check if a string consists only of digits and and + \# ( ) - .
	(meaning it can be cleaned with tris_shrink_phone_number)
    \param exten The extension (or URI) to be checked.
    \return Returns 0 if n is a number, 1 if it's not.
 */
int tris_is_shrinkable_phonenumber(const char *exten);

int tris_callerid_split(const char *src, char *name, int namelen, char *num, int numlen);

char *tris_callerid_merge(char *buf, int bufsiz, const char *name, const char *num, const char *unknown);

/*
 * Caller*ID and other GR-30 compatible generation
 * routines (used by ADSI for example)
 */

extern float cid_dr[4];
extern float cid_di[4];
extern float clidsb;

static inline float callerid_getcarrier(float *cr, float *ci, int bit)
{
	/* Move along.  There's nothing to see here... */
	float t;
	t = *cr * cid_dr[bit] - *ci * cid_di[bit];
	*ci = *cr * cid_di[bit] + *ci * cid_dr[bit];
	*cr = t;

	t = 2.0 - (*cr * *cr + *ci * *ci);
	*cr *= t;
	*ci *= t;
	return *cr;
}

#define PUT_BYTE(a) do { \
	*(buf++) = (a); \
	bytes++; \
} while(0)

#define PUT_AUDIO_SAMPLE(y) do { \
	int __sample_idx = (short)(rint(8192.0 * (y))); \
	*(buf++) = TRIS_LIN2X(__sample_idx); \
	bytes++; \
} while(0)

#define PUT_CLID_MARKMS do { \
	int __clid_x; \
	for (__clid_x=0;__clid_x<8;__clid_x++) \
		PUT_AUDIO_SAMPLE(callerid_getcarrier(&cr, &ci, 1)); \
} while(0)

#define PUT_CLID_BAUD(bit) do { \
	while(scont < clidsb) { \
		PUT_AUDIO_SAMPLE(callerid_getcarrier(&cr, &ci, bit)); \
		scont += 1.0; \
	} \
	scont -= clidsb; \
} while(0)


#define PUT_CLID(byte) do { \
	int z; \
	unsigned char b = (byte); \
	PUT_CLID_BAUD(0); 	/* Start bit */ \
	for (z=0;z<8;z++) { \
		PUT_CLID_BAUD(b & 1); \
		b >>= 1; \
	} \
	PUT_CLID_BAUD(1);	/* Stop bit */ \
} while(0)

/* Various defines and bits for handling PRI- and SS7-type restriction */

#define TRIS_PRES_NUMBER_TYPE				0x03
#define TRIS_PRES_USER_NUMBER_UNSCREENED			0x00
#define TRIS_PRES_USER_NUMBER_PASSED_SCREEN		0x01
#define TRIS_PRES_USER_NUMBER_FAILED_SCREEN		0x02
#define TRIS_PRES_NETWORK_NUMBER				0x03

#define TRIS_PRES_RESTRICTION				0x60
#define TRIS_PRES_ALLOWED				0x00
#define TRIS_PRES_RESTRICTED				0x20
#define TRIS_PRES_UNAVAILABLE				0x40
#define TRIS_PRES_RESERVED				0x60

#define TRIS_PRES_ALLOWED_USER_NUMBER_NOT_SCREENED \
	TRIS_PRES_USER_NUMBER_UNSCREENED + TRIS_PRES_ALLOWED

#define TRIS_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN \
	TRIS_PRES_USER_NUMBER_PASSED_SCREEN + TRIS_PRES_ALLOWED

#define TRIS_PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN \
	TRIS_PRES_USER_NUMBER_FAILED_SCREEN + TRIS_PRES_ALLOWED

#define TRIS_PRES_ALLOWED_NETWORK_NUMBER	\
	TRIS_PRES_NETWORK_NUMBER + TRIS_PRES_ALLOWED

#define TRIS_PRES_PROHIB_USER_NUMBER_NOT_SCREENED \
	TRIS_PRES_USER_NUMBER_UNSCREENED + TRIS_PRES_RESTRICTED

#define TRIS_PRES_PROHIB_USER_NUMBER_PASSED_SCREEN \
	TRIS_PRES_USER_NUMBER_PASSED_SCREEN + TRIS_PRES_RESTRICTED

#define TRIS_PRES_PROHIB_USER_NUMBER_FAILED_SCREEN \
	TRIS_PRES_USER_NUMBER_FAILED_SCREEN + TRIS_PRES_RESTRICTED

#define TRIS_PRES_PROHIB_NETWORK_NUMBER \
	TRIS_PRES_NETWORK_NUMBER + TRIS_PRES_RESTRICTED

#define TRIS_PRES_NUMBER_NOT_AVAILABLE \
	TRIS_PRES_NETWORK_NUMBER + TRIS_PRES_UNAVAILABLE

int tris_parse_caller_presentation(const char *data);
const char *tris_describe_caller_presentation(int data);
const char *tris_named_caller_presentation(int data);

/*! \page Def_CallerPres Caller ID Presentation

	Caller ID presentation values are used to set properties to a
	caller ID in PSTN networks, and as RPID value in SIP transactions.

	The following values are available to use:
	\arg \b Defined value, text string in config file, explanation

	\arg \b TRIS_PRES_ALLOWED_USER_NUMBER_NOT_SCREENED, "allowed_not_screened", Presentation Allowed, Not Screened,
	\arg \b TRIS_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN, "allowed_passed_screen", Presentation Allowed, Passed Screen,
	\arg \b TRIS_PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN, "allowed_failed_screen", Presentation Allowed, Failed Screen,
	\arg \b TRIS_PRES_ALLOWED_NETWORK_NUMBER, "allowed", Presentation Allowed, Network Number,
	\arg \b TRIS_PRES_PROHIB_USER_NUMBER_NOT_SCREENED, "prohib_not_screened", Presentation Prohibited, Not Screened,
	\arg \b TRIS_PRES_PROHIB_USER_NUMBER_PASSED_SCREEN, "prohib_passed_screen", Presentation Prohibited, Passed Screen,
	\arg \b TRIS_PRES_PROHIB_USER_NUMBER_FAILED_SCREEN, "prohib_failed_screen", Presentation Prohibited, Failed Screen,
	\arg \b TRIS_PRES_PROHIB_NETWORK_NUMBER, "prohib", Presentation Prohibited, Network Number,

	\par References
	\arg \ref callerid.h Definitions
	\arg \ref callerid.c Functions
	\arg \ref CID Caller ID names and numbers
*/


#endif /* _TRISMEDIA_CALLERID_H */
