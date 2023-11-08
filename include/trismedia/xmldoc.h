/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Eliel C. Sardanons (LU1ALY) <eliels@gmail.com>
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

#ifndef _TRISMEDIA_XMLDOC_H
#define _TRISMEDIA_XMLDOC_H

/*! \file
 *  \brief Trismedia XML Documentation API
 */

#include "trismedia/xml.h"

#ifdef TRIS_XML_DOCS

/*!
 *  \brief Get the syntax for a specified application or function.
 *  \param type Application, Function or AGI ?
 *  \param name Name of the application or function.
 *  \retval NULL on error.
 *  \retval The generated syntax in a tris_malloc'ed string.
 */
char *tris_xmldoc_build_syntax(const char *type, const char *name);

/*!
 *  \brief Parse the <see-also> node content.
 *  \param type 'application', 'function' or 'agi'.
 *  \param name Application or functions name.
 *  \retval NULL on error.
 *  \retval Content of the see-also node.
 */
char *tris_xmldoc_build_seealso(const char *type, const char *name);

/*!
 *  \brief Generate the [arguments] tag based on type of node ('application',
 *         'function' or 'agi') and name.
 *  \param type 'application', 'function' or 'agi' ?
 *  \param name Name of the application or function to build the 'arguments' tag.
 *  \retval NULL on error.
 *  \retval Output buffer with the [arguments] tag content.
 */
char *tris_xmldoc_build_arguments(const char *type, const char *name);

/*!
 *  \brief Colorize and put delimiters (instead of tags) to the xmldoc output.
 *  \param bwinput Not colorized input with tags.
 *  \param withcolors Result output with colors.
 *  \retval NULL on error.
 *  \retval New malloced buffer colorized and with delimiters.
 */
char *tris_xmldoc_printable(const char *bwinput, int withcolors);

/*!
 *  \brief Generate synopsis documentation from XML.
 *  \param type The source of documentation (application, function, etc).
 *  \param name The name of the application, function, etc.
 *  \retval NULL on error.
 *  \retval A malloc'ed string with the synopsis.
 */
char *tris_xmldoc_build_synopsis(const char *type, const char *name);

/*!
 *  \brief Generate description documentation from XML.
 *  \param type The source of documentation (application, function, etc).
 *  \param name The name of the application, function, etc.
 *  \retval NULL on error.
 *  \retval A malloc'ed string with the formatted description.
 */
char *tris_xmldoc_build_description(const char *type, const char *name);

#endif /* TRIS_XML_DOCS */

#endif /* _TRISMEDIA_XMLDOC_H */
