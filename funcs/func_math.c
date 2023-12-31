/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2006, Andy Powell 
 *
 * Updated by Mark Spencer <markster@digium.com>
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
 * \brief Math related dialplan function
 *
 * \author Andy Powell
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup functions
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 244334 $")

#include <math.h>

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/pbx.h"
#include "trismedia/utils.h"
#include "trismedia/app.h"
#include "trismedia/config.h"

/*** DOCUMENTATION
	<function name="MATH" language="en_US">
		<synopsis>
			Performs Mathematical Functions.
		</synopsis>
		<syntax>
			<parameter name="expression" required="true">
				<para>Is of the form:
				<replaceable>number1</replaceable><replaceable>op</replaceable><replaceable>number2</replaceable>
				where the possible values for <replaceable>op</replaceable>
				are:</para>
				<para>+,-,/,*,%,&lt;&lt;,&gt;&gt;,^,AND,OR,XOR,&lt;,%gt;,&gt;=,&lt;=,== (and behave as their C equivalents)</para>
			</parameter>
			<parameter name="type">
				<para>Wanted type of result:</para>
				<para>f, float - float(default)</para>
				<para>i, int - integer</para>
				<para>h, hex - hex</para>
				<para>c, char - char</para>
			</parameter>
		</syntax>
		<description>
			<para>Performs mathematical functions based on two parameters and an operator.  The returned
			value type is <replaceable>type</replaceable></para>
			<para>Example: Set(i=${MATH(123%16,int)}) - sets var i=11</para>
		</description>
	</function>
 ***/

enum TypeOfFunctions {
	ADDFUNCTION,
	DIVIDEFUNCTION,
	MULTIPLYFUNCTION,
	SUBTRACTFUNCTION,
	MODULUSFUNCTION,
	POWFUNCTION,
	SHLEFTFUNCTION,
	SHRIGHTFUNCTION,
	BITWISEANDFUNCTION,
	BITWISEXORFUNCTION,
	BITWISEORFUNCTION,
	GTFUNCTION,
	LTFUNCTION,
	GTEFUNCTION,
	LTEFUNCTION,
	EQFUNCTION
};

enum TypeOfResult {
	FLOAT_RESULT,
	INT_RESULT,
	HEX_RESULT,
	CHAR_RESULT
};

static int math(struct tris_channel *chan, const char *cmd, char *parse,
		char *buf, size_t len)
{
	double fnum1;
	double fnum2;
	double ftmp = 0;
	char *op;
	int iaction = -1;
	int type_of_result = FLOAT_RESULT;
	char *mvalue1, *mvalue2 = NULL, *mtype_of_result;
	int negvalue1 = 0;
	TRIS_DECLARE_APP_ARGS(args,
			     TRIS_APP_ARG(argv0);
			     TRIS_APP_ARG(argv1);
	);

	if (tris_strlen_zero(parse)) {
		tris_log(LOG_WARNING, "Syntax: MATH(<number1><op><number 2>[,<type_of_result>]) - missing argument!\n");
		return -1;
	}

	TRIS_STANDARD_APP_ARGS(args, parse);

	if (args.argc < 1) {
		tris_log(LOG_WARNING, "Syntax: MATH(<number1><op><number 2>[,<type_of_result>]) - missing argument!\n");
		return -1;
	}

	mvalue1 = args.argv0;

	if (mvalue1[0] == '-') {
		negvalue1 = 1;
		mvalue1++;
	}

	if ((op = strchr(mvalue1, '*'))) {
		iaction = MULTIPLYFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '/'))) {
		iaction = DIVIDEFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '%'))) {
		iaction = MODULUSFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '^'))) {
		iaction = POWFUNCTION;
		*op = '\0';
	} else if ((op = strstr(mvalue1, "AND"))) {
		iaction = BITWISEANDFUNCTION;
		*op = '\0';
		op += 2;
	} else if ((op = strstr(mvalue1, "XOR"))) {
		iaction = BITWISEXORFUNCTION;
		*op = '\0';
		op += 2;
	} else if ((op = strstr(mvalue1, "OR"))) {
		iaction = BITWISEORFUNCTION;
		*op = '\0';
		++op;
	} else if ((op = strchr(mvalue1, '>'))) {
		iaction = GTFUNCTION;
		*op = '\0';
		if (*(op + 1) == '=') {
			iaction = GTEFUNCTION;
			++op;
		} else if (*(op + 1) == '>') {
			iaction = SHRIGHTFUNCTION;
			++op;
		}
	} else if ((op = strchr(mvalue1, '<'))) {
		iaction = LTFUNCTION;
		*op = '\0';
		if (*(op + 1) == '=') {
			iaction = LTEFUNCTION;
			++op;
		} else if (*(op + 1) == '<') {
			iaction = SHLEFTFUNCTION;
			++op;
		}
	} else if ((op = strchr(mvalue1, '='))) {
		*op = '\0';
		if (*(op + 1) == '=') {
			iaction = EQFUNCTION;
			++op;
		} else
			op = NULL;
	} else if ((op = strchr(mvalue1, '+'))) {
		iaction = ADDFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '-'))) { /* subtraction MUST always be last, in case we have a negative second number */
		iaction = SUBTRACTFUNCTION;
		*op = '\0';
	}

	if (op)
		mvalue2 = op + 1;

	/* detect wanted type of result */
	mtype_of_result = args.argv1;
	if (mtype_of_result) {
		if (!strcasecmp(mtype_of_result, "float")
		    || !strcasecmp(mtype_of_result, "f"))
			type_of_result = FLOAT_RESULT;
		else if (!strcasecmp(mtype_of_result, "int")
			 || !strcasecmp(mtype_of_result, "i"))
			type_of_result = INT_RESULT;
		else if (!strcasecmp(mtype_of_result, "hex")
			 || !strcasecmp(mtype_of_result, "h"))
			type_of_result = HEX_RESULT;
		else if (!strcasecmp(mtype_of_result, "char")
			 || !strcasecmp(mtype_of_result, "c"))
			type_of_result = CHAR_RESULT;
		else {
			tris_log(LOG_WARNING, "Unknown type of result requested '%s'.\n",
					mtype_of_result);
			return -1;
		}
	}

	if (!mvalue1 || !mvalue2) {
		tris_log(LOG_WARNING,
				"Supply all the parameters - just this once, please\n");
		return -1;
	}

	if (sscanf(mvalue1, "%30lf", &fnum1) != 1) {
		tris_log(LOG_WARNING, "'%s' is not a valid number\n", mvalue1);
		return -1;
	}

	if (sscanf(mvalue2, "%30lf", &fnum2) != 1) {
		tris_log(LOG_WARNING, "'%s' is not a valid number\n", mvalue2);
		return -1;
	}

	if (negvalue1)
		fnum1 = 0 - fnum1;

	switch (iaction) {
	case ADDFUNCTION:
		ftmp = fnum1 + fnum2;
		break;
	case DIVIDEFUNCTION:
		if (fnum2 <= 0)
			ftmp = 0;			/* can't do a divide by 0 */
		else
			ftmp = (fnum1 / fnum2);
		break;
	case MULTIPLYFUNCTION:
		ftmp = (fnum1 * fnum2);
		break;
	case SUBTRACTFUNCTION:
		ftmp = (fnum1 - fnum2);
		break;
	case MODULUSFUNCTION:
		{
			int inum1 = fnum1;
			int inum2 = fnum2;

			if (inum2 == 0) {
				ftmp = 0;
			} else {
				ftmp = (inum1 % inum2);
			}

			break;
		}
	case POWFUNCTION:
		ftmp = pow(fnum1, fnum2);
		break;
	case SHLEFTFUNCTION:
		{
			int inum1 = fnum1;
			int inum2 = fnum2;

			ftmp = (inum1 << inum2);
			break;
		}
	case SHRIGHTFUNCTION:
		{
			int inum1 = fnum1;
			int inum2 = fnum2;

			ftmp = (inum1 >> inum2);
			break;
		}
	case BITWISEANDFUNCTION:
		{
			int inum1 = fnum1;
			int inum2 = fnum2;
			ftmp = (inum1 & inum2);
			break;
		}
	case BITWISEXORFUNCTION:
		{
			int inum1 = fnum1;
			int inum2 = fnum2;
			ftmp = (inum1 ^ inum2);
			break;
		}
	case BITWISEORFUNCTION:
		{
			int inum1 = fnum1;
			int inum2 = fnum2;
			ftmp = (inum1 | inum2);
			break;
		}
	case GTFUNCTION:
		tris_copy_string(buf, (fnum1 > fnum2) ? "TRUE" : "FALSE", len);
		break;
	case LTFUNCTION:
		tris_copy_string(buf, (fnum1 < fnum2) ? "TRUE" : "FALSE", len);
		break;
	case GTEFUNCTION:
		tris_copy_string(buf, (fnum1 >= fnum2) ? "TRUE" : "FALSE", len);
		break;
	case LTEFUNCTION:
		tris_copy_string(buf, (fnum1 <= fnum2) ? "TRUE" : "FALSE", len);
		break;
	case EQFUNCTION:
		tris_copy_string(buf, (fnum1 == fnum2) ? "TRUE" : "FALSE", len);
		break;
	default:
		tris_log(LOG_WARNING,
				"Something happened that neither of us should be proud of %d\n",
				iaction);
		return -1;
	}

	if (iaction < GTFUNCTION || iaction > EQFUNCTION) {
		if (type_of_result == FLOAT_RESULT)
			snprintf(buf, len, "%f", ftmp);
		else if (type_of_result == INT_RESULT)
			snprintf(buf, len, "%i", (int) ftmp);
		else if (type_of_result == HEX_RESULT)
			snprintf(buf, len, "%x", (unsigned int) ftmp);
		else if (type_of_result == CHAR_RESULT)
			snprintf(buf, len, "%c", (unsigned char) ftmp);
	}

	return 0;
}

static struct tris_custom_function math_function = {
	.name = "MATH",
	.read = math
};

static int unload_module(void)
{
	return tris_custom_function_unregister(&math_function);
}

static int load_module(void)
{
	return tris_custom_function_register(&math_function);
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "Mathematical dialplan function");
