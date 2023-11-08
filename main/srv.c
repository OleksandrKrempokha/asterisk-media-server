/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Funding provided by nic.at
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
 * \brief DNS SRV Record Lookup Support for Trismedia
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \arg See also \ref AstENUM
 *
 * \note Funding provided by nic.at
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 168523 $")

#include <netinet/in.h>
#include <arpa/nameser.h>
#ifdef __APPLE__
#if __APPLE_CC__ >= 1495
#include <arpa/nameser_compat.h>
#endif
#endif
#include <resolv.h>

#include "trismedia/channel.h"
#include "trismedia/srv.h"
#include "trismedia/dns.h"
#include "trismedia/utils.h"
#include "trismedia/linkedlists.h"

#ifdef __APPLE__
#undef T_SRV
#define T_SRV 33
#endif

struct srv_entry {
	unsigned short priority;
	unsigned short weight;
	unsigned short port;
	unsigned int weight_sum;
	TRIS_LIST_ENTRY(srv_entry) list;
	char host[1];
};

struct srv_context {
	unsigned int have_weights:1;
	TRIS_LIST_HEAD_NOLOCK(srv_entries, srv_entry) entries;
};

static int parse_srv(unsigned char *answer, int len, unsigned char *msg, struct srv_entry **result)
{
	struct srv {
		unsigned short priority;
		unsigned short weight;
		unsigned short port;
	} __attribute__((__packed__)) *srv = (struct srv *) answer;

	int res = 0;
	char repl[256] = "";
	struct srv_entry *entry;

	if (len < sizeof(*srv))
		return -1;

	answer += sizeof(*srv);
	len -= sizeof(*srv);

	if ((res = dn_expand(msg, answer + len, answer, repl, sizeof(repl) - 1)) <= 0) {
		tris_log(LOG_WARNING, "Failed to expand hostname\n");
		return -1;
	}

	/* the magic value "." for the target domain means that this service
	   is *NOT* available at the domain we searched */
	if (!strcmp(repl, "."))
		return -1;

	if (!(entry = tris_calloc(1, sizeof(*entry) + strlen(repl))))
		return -1;
	
	entry->priority = ntohs(srv->priority);
	entry->weight = ntohs(srv->weight);
	entry->port = ntohs(srv->port);
	strcpy(entry->host, repl);

	*result = entry;
	
	return 0;
}

static int srv_callback(void *context, unsigned char *answer, int len, unsigned char *fullanswer)
{
	struct srv_context *c = (struct srv_context *) context;
	struct srv_entry *entry = NULL;
	struct srv_entry *current;

	if (parse_srv(answer, len, fullanswer, &entry))
		return -1;

	if (entry->weight)
		c->have_weights = 1;

	TRIS_LIST_TRAVERSE_SAFE_BEGIN(&c->entries, current, list) {
		/* insert this entry just before the first existing
		   entry with a higher priority */
		if (current->priority <= entry->priority)
			continue;

		TRIS_LIST_INSERT_BEFORE_CURRENT(entry, list);
		entry = NULL;
		break;
	}
	TRIS_LIST_TRAVERSE_SAFE_END;

	/* if we didn't find a place to insert the entry before an existing
	   entry, then just add it to the end */
	if (entry)
		TRIS_LIST_INSERT_TAIL(&c->entries, entry, list);

	return 0;
}

/* Do the bizarre SRV record weight-handling algorithm
   involving sorting and random number generation...

   See RFC 2782 if you want know why this code does this
*/
static void process_weights(struct srv_context *context)
{
	struct srv_entry *current;
	struct srv_entries newlist = TRIS_LIST_HEAD_NOLOCK_INIT_VALUE;

	while (TRIS_LIST_FIRST(&context->entries)) {
		unsigned int random_weight;
		unsigned int weight_sum;
		unsigned short cur_priority = TRIS_LIST_FIRST(&context->entries)->priority;
		struct srv_entries temp_list = TRIS_LIST_HEAD_NOLOCK_INIT_VALUE;
		weight_sum = 0;

		TRIS_LIST_TRAVERSE_SAFE_BEGIN(&context->entries, current, list) {
			if (current->priority != cur_priority)
				break;

			TRIS_LIST_MOVE_CURRENT(&temp_list, list);
		}
		TRIS_LIST_TRAVERSE_SAFE_END;

		while (TRIS_LIST_FIRST(&temp_list)) {
			weight_sum = 0;
			TRIS_LIST_TRAVERSE(&temp_list, current, list)
				current->weight_sum = weight_sum += current->weight;

			/* if all the remaining entries have weight == 0,
			   then just append them to the result list and quit */
			if (weight_sum == 0) {
				TRIS_LIST_APPEND_LIST(&newlist, &temp_list, list);
				break;
			}

			random_weight = 1 + (unsigned int) ((float) weight_sum * (tris_random() / ((float) RAND_MAX + 1.0)));

			TRIS_LIST_TRAVERSE_SAFE_BEGIN(&temp_list, current, list) {
				if (current->weight < random_weight)
					continue;

				TRIS_LIST_MOVE_CURRENT(&newlist, list);
				break;
			}
			TRIS_LIST_TRAVERSE_SAFE_END;
		}

	}

	/* now that the new list has been ordered,
	   put it in place */

	TRIS_LIST_APPEND_LIST(&context->entries, &newlist, list);
}

int tris_get_srv(struct tris_channel *chan, char *host, int hostlen, int *port, const char *service)
{
	struct srv_context context = { .entries = TRIS_LIST_HEAD_NOLOCK_INIT_VALUE };
	struct srv_entry *current;
	int ret;

	if (chan && tris_autoservice_start(chan) < 0)
		return -1;

	ret = tris_search_dns(&context, service, C_IN, T_SRV, srv_callback);

	if (context.have_weights)
		process_weights(&context);

	if (chan)
		ret |= tris_autoservice_stop(chan);

	/* TODO: there could be a "." entry in the returned list of
	   answers... if so, this requires special handling */

	/* the list of entries will be sorted in the proper selection order
	   already, so we just need the first one (if any) */

	if ((ret > 0) && (current = TRIS_LIST_REMOVE_HEAD(&context.entries, list))) {
		tris_copy_string(host, current->host, hostlen);
		*port = current->port;
		tris_free(current);
		tris_verb(4, "tris_get_srv: SRV lookup for '%s' mapped to host %s, port %d\n",
				    service, host, *port);
	} else {
		host[0] = '\0';
		*port = -1;
	}

	while ((current = TRIS_LIST_REMOVE_HEAD(&context.entries, list)))
		tris_free(current);

	return ret;
}
