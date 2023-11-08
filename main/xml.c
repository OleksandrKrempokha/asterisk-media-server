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

/*! \file
 *
 * \brief XML abstraction layer
 *
 * \author Eliel C. Sardanons (LU1ALY) <eliels@gmail.com>
 */

#include "trismedia.h"
#include "trismedia/xml.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 197375 $")

#if defined(HAVE_LIBXML2)
#ifndef _POSIX_C_SOURCE	/* Needed on Mac OS X */
#define _POSIX_C_SOURCE 200112L
#endif
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xinclude.h>
/* libxml2 tris_xml implementation. */


int tris_xml_init(void)
{
	LIBXML_TEST_VERSION

	return 0;
}

int tris_xml_finish(void)
{
	xmlCleanupParser();

	return 0;
}

struct tris_xml_doc *tris_xml_open(char *filename)
{
	xmlDoc *doc;

	if (!filename) {
		return NULL;
	}

	doc = xmlReadFile(filename, NULL, XML_PARSE_RECOVER);
	if (doc) {
		/* process xinclude elements. */
		if (xmlXIncludeProcess(doc) < 0) {
			xmlFreeDoc(doc);
			return NULL;
		}
	}

	return (struct tris_xml_doc *) doc;
}

void tris_xml_close(struct tris_xml_doc *doc)
{
	if (!doc) {
		return;
	}

	xmlFreeDoc((xmlDoc *) doc);
	doc = NULL;
}


struct tris_xml_node *tris_xml_get_root(struct tris_xml_doc *doc)
{
	xmlNode *root_node;

	if (!doc) {
		return NULL;
	}

	root_node = xmlDocGetRootElement((xmlDoc *) doc);

	return (struct tris_xml_node *) root_node;
}

void tris_xml_free_node(struct tris_xml_node *node)
{
	if (!node) {
		return;
	}

	xmlFreeNode((xmlNode *) node);
	node = NULL;
}

void tris_xml_free_attr(const char *attribute)
{
	if (attribute) {
		xmlFree((char *) attribute);
	}
}

void tris_xml_free_text(const char *text)
{
	if (text) {
		xmlFree((char *) text);
	}
}

const char *tris_xml_get_attribute(struct tris_xml_node *node, const char *attrname)
{
	xmlChar *attrvalue;

	if (!node) {
		return NULL;
	}

	if (!attrname) {
		return NULL;
	}

	attrvalue = xmlGetProp((xmlNode *) node, (xmlChar *) attrname);

	return (const char *) attrvalue;
}

struct tris_xml_node *tris_xml_find_element(struct tris_xml_node *root_node, const char *name, const char *attrname, const char *attrvalue)
{
	struct tris_xml_node *cur;
	const char *attr;

	if (!root_node) {
		return NULL;
	}

	for (cur = root_node; cur; cur = tris_xml_node_get_next(cur)) {
		/* Check if the name matchs */
		if (strcmp(tris_xml_node_get_name(cur), name)) {
			continue;
		}
		/* We need to check for a specific attribute name? */
		if (!attrname || !attrvalue) {
			return cur;
		}
		/* Get the attribute, we need to compare it. */
		if ((attr = tris_xml_get_attribute(cur, attrname))) {
			/* does attribute name/value matches? */
			if (!strcmp(attr, attrvalue)) {
				tris_xml_free_attr(attr);
				return cur;
			}
			tris_xml_free_attr(attr);
		}
	}

	return NULL;
}

const char *tris_xml_get_text(struct tris_xml_node *node)
{
	if (!node) {
		return NULL;
	}

	return (const char *) xmlNodeGetContent((xmlNode *) node);
}

const char *tris_xml_node_get_name(struct tris_xml_node *node)
{
	return (const char *) ((xmlNode *) node)->name;
}

struct tris_xml_node *tris_xml_node_get_children(struct tris_xml_node *node)
{
	return (struct tris_xml_node *) ((xmlNode *) node)->children;
}

struct tris_xml_node *tris_xml_node_get_next(struct tris_xml_node *node)
{
	return (struct tris_xml_node *) ((xmlNode *) node)->next;
}

struct tris_xml_node *tris_xml_node_get_prev(struct tris_xml_node *node)
{
	return (struct tris_xml_node *) ((xmlNode *) node)->prev;
}

struct tris_xml_node *tris_xml_node_get_parent(struct tris_xml_node *node)
{
	return (struct tris_xml_node *) ((xmlNode *) node)->parent;
}

#endif /* defined(HAVE_LIBXML2) */

