/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 *
 * \brief Translate via the use of pseudo channels
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 208927 $")

#include <sys/time.h>
#include <sys/resource.h>
#include <math.h>

#include "trismedia/lock.h"
#include "trismedia/channel.h"
#include "trismedia/translate.h"
#include "trismedia/module.h"
#include "trismedia/frame.h"
#include "trismedia/sched.h"
#include "trismedia/cli.h"
#include "trismedia/term.h"

#define MAX_RECALC 1000 /* max sample recalc */

/*! \brief the list of translators */
static TRIS_RWLIST_HEAD_STATIC(translators, tris_translator);

struct translator_path {
	struct tris_translator *step;	/*!< Next step translator */
	unsigned int cost;		/*!< Complete cost to destination */
	unsigned int multistep;		/*!< Multiple conversions required for this translation */
};

/*! \brief a matrix that, for any pair of supported formats,
 * indicates the total cost of translation and the first step.
 * The full path can be reconstricted iterating on the matrix
 * until step->dstfmt == desired_format.
 *
 * Array indexes are 'src' and 'dest', in that order.
 *
 * Note: the lock in the 'translators' list is also used to protect
 * this structure.
 */
static struct translator_path tr_matrix[MAX_FORMAT][MAX_FORMAT];

/*! \todo
 * TODO: sample frames for each supported input format.
 * We build this on the fly, by taking an SLIN frame and using
 * the existing converter to play with it.
 */

/*! \brief returns the index of the lowest bit set */
static force_inline int powerof(unsigned int d)
{
	int x = ffs(d);

	if (x)
		return x - 1;

	tris_log(LOG_WARNING, "No bits set? %d\n", d);

	return -1;
}

/*
 * wrappers around the translator routines.
 */

/*!
 * \brief Allocate the descriptor, required outbuf space,
 * and possibly also plc and desc.
 */
static void *newpvt(struct tris_translator *t)
{
	struct tris_trans_pvt *pvt;
	int len;
	int useplc = t->plc_samples > 0 && t->useplc;	/* cache, because it can change on the fly */
	char *ofs;

	/*
	 * compute the required size adding private descriptor,
	 * plc, buffer, TRIS_FRIENDLY_OFFSET.
	 */
	len = sizeof(*pvt) + t->desc_size;
	if (useplc)
		len += sizeof(plc_state_t);
	if (t->buf_size)
		len += TRIS_FRIENDLY_OFFSET + t->buf_size;
	pvt = tris_calloc(1, len);
	if (!pvt)
		return NULL;
	pvt->t = t;
	ofs = (char *)(pvt + 1);	/* pointer to data space */
	if (t->desc_size) {		/* first comes the descriptor */
		pvt->pvt = ofs;
		ofs += t->desc_size;
	}
	if (useplc) {			/* then plc state */
		pvt->plc = (plc_state_t *)ofs;
		ofs += sizeof(plc_state_t);
	}
	if (t->buf_size)		/* finally buffer and header */
		pvt->outbuf.c = ofs + TRIS_FRIENDLY_OFFSET;
	/* call local init routine, if present */
	if (t->newpvt && t->newpvt(pvt)) {
		tris_free(pvt);
		return NULL;
	}
	tris_module_ref(t->module);
	return pvt;
}

static void destroy(struct tris_trans_pvt *pvt)
{
	struct tris_translator *t = pvt->t;

	if (tris_test_flag(&pvt->f, TRIS_FRFLAG_FROM_TRANSLATOR)) {
		/* If this flag is still set, that means that the translation path has
		 * been torn down, while we still have a frame out there being used.
		 * When tris_frfree() gets called on that frame, this tris_trans_pvt
		 * will get destroyed, too. */

		pvt->destroy = 1;

		return;
	}

	if (t->destroy)
		t->destroy(pvt);
	tris_free(pvt);
	tris_module_unref(t->module);
}

/*! \brief framein wrapper, deals with plc and bound checks.  */
static int framein(struct tris_trans_pvt *pvt, struct tris_frame *f)
{
	int16_t *dst = pvt->outbuf.i16;
	int ret;
	int samples = pvt->samples;	/* initial value */
	
	/* Copy the last in jb timing info to the pvt */
	tris_copy_flags(&pvt->f, f, TRIS_FRFLAG_HAS_TIMING_INFO);
	pvt->f.ts = f->ts;
	pvt->f.len = f->len;
	pvt->f.seqno = f->seqno;

	if (f->samples == 0) {
		tris_log(LOG_WARNING, "no samples for %s\n", pvt->t->name);
	}
	if (pvt->t->buffer_samples) {	/* do not pass empty frames to callback */
		if (f->datalen == 0) { /* perform PLC with nominal framesize of 20ms/160 samples */
			if (pvt->plc) {
				int l = pvt->t->plc_samples;
				if (pvt->samples + l > pvt->t->buffer_samples) {
					tris_log(LOG_WARNING, "Out of buffer space\n");
					return -1;
				}
				l = plc_fillin(pvt->plc, dst + pvt->samples, l);
				pvt->samples += l;
				pvt->datalen = pvt->samples * 2;	/* SLIN has 2bytes for 1sample */
			}
			/* We don't want generic PLC. If the codec has native PLC, then do that */
			if (!pvt->t->native_plc)
				return 0;
		}
		if (pvt->samples + f->samples > pvt->t->buffer_samples) {
			tris_log(LOG_WARNING, "Out of buffer space\n");
			return -1;
		}
	}
	/* we require a framein routine, wouldn't know how to do
	 * it otherwise.
	 */
	ret = pvt->t->framein(pvt, f);
	/* possibly store data for plc */
	if (!ret && pvt->plc) {
		int l = pvt->t->plc_samples;
		if (pvt->samples < l)
			l = pvt->samples;
		plc_rx(pvt->plc, dst + pvt->samples - l, l);
	}
	/* diagnostic ... */
	if (pvt->samples == samples)
		tris_log(LOG_WARNING, "%s did not update samples %d\n",
			pvt->t->name, pvt->samples);
	return ret;
}

/*! \brief generic frameout routine.
 * If samples and datalen are 0, take whatever is in pvt
 * and reset them, otherwise take the values in the caller and
 * leave alone the pvt values.
 */
struct tris_frame *tris_trans_frameout(struct tris_trans_pvt *pvt,
	int datalen, int samples)
{
	struct tris_frame *f = &pvt->f;

	if (samples)
		f->samples = samples;
	else {
		if (pvt->samples == 0)
			return NULL;
		f->samples = pvt->samples;
		pvt->samples = 0;
	}
	if (datalen)
		f->datalen = datalen;
	else {
		f->datalen = pvt->datalen;
		pvt->datalen = 0;
	}

	f->frametype = TRIS_FRAME_VOICE;
	f->subclass = 1 << (pvt->t->dstfmt);
	f->mallocd = 0;
	f->offset = TRIS_FRIENDLY_OFFSET;
	f->src = pvt->t->name;
	f->data.ptr = pvt->outbuf.c;

	tris_set_flag(f, TRIS_FRFLAG_FROM_TRANSLATOR);

	return f;
}

static struct tris_frame *default_frameout(struct tris_trans_pvt *pvt)
{
	return tris_trans_frameout(pvt, 0, 0);
}

/* end of callback wrappers and helpers */

void tris_translator_free_path(struct tris_trans_pvt *p)
{
	struct tris_trans_pvt *pn = p;
	while ( (p = pn) ) {
		pn = p->next;
		destroy(p);
	}
}

/*! \brief Build a chain of translators based upon the given source and dest formats */
struct tris_trans_pvt *tris_translator_build_path(int dest, int source)
{
	struct tris_trans_pvt *head = NULL, *tail = NULL;
	
	source = powerof(source);
	dest = powerof(dest);

	if (source == -1 || dest == -1) {
		tris_log(LOG_WARNING, "No translator path: (%s codec is not valid)\n", source == -1 ? "starting" : "ending");
		return NULL;
	}

	TRIS_RWLIST_RDLOCK(&translators);

	while (source != dest) {
		struct tris_trans_pvt *cur;
		struct tris_translator *t = tr_matrix[source][dest].step;
		if (!t) {
			tris_log(LOG_WARNING, "No translator path from %s to %s\n", 
				tris_getformatname(source), tris_getformatname(dest));
			TRIS_RWLIST_UNLOCK(&translators);
			return NULL;
		}
		if (!(cur = newpvt(t))) {
			tris_log(LOG_WARNING, "Failed to build translator step from %d to %d\n", source, dest);
			if (head)
				tris_translator_free_path(head);	
			TRIS_RWLIST_UNLOCK(&translators);
			return NULL;
		}
		if (!head)
			head = cur;
		else
			tail->next = cur;
		tail = cur;
		cur->nextin = cur->nextout = tris_tv(0, 0);
		/* Keep going if this isn't the final destination */
		source = cur->t->dstfmt;
	}

	TRIS_RWLIST_UNLOCK(&translators);
	return head;
}

/*! \brief do the actual translation */
struct tris_frame *tris_translate(struct tris_trans_pvt *path, struct tris_frame *f, int consume)
{
	struct tris_trans_pvt *p = path;
	struct tris_frame *out = f;
	struct timeval delivery;
	int has_timing_info;
	long ts;
	long len;
	int seqno;

	has_timing_info = tris_test_flag(f, TRIS_FRFLAG_HAS_TIMING_INFO);
	ts = f->ts;
	len = f->len;
	seqno = f->seqno;

	/* XXX hmmm... check this below */
	if (!tris_tvzero(f->delivery)) {
		if (!tris_tvzero(path->nextin)) {
			/* Make sure this is in line with what we were expecting */
			if (!tris_tveq(path->nextin, f->delivery)) {
				/* The time has changed between what we expected and this
				   most recent time on the new packet.  If we have a
				   valid prediction adjust our output time appropriately */
				if (!tris_tvzero(path->nextout)) {
					path->nextout = tris_tvadd(path->nextout,
								  tris_tvsub(f->delivery, path->nextin));
				}
				path->nextin = f->delivery;
			}
		} else {
			/* This is our first pass.  Make sure the timing looks good */
			path->nextin = f->delivery;
			path->nextout = f->delivery;
		}
		/* Predict next incoming sample */
		path->nextin = tris_tvadd(path->nextin, tris_samp2tv(f->samples, tris_format_rate(f->subclass)));
	}
	delivery = f->delivery;
	for ( ; out && p ; p = p->next) {
		framein(p, out);
		if (out != f)
			tris_frfree(out);
		out = p->t->frameout(p);
	}
	if (consume)
		tris_frfree(f);
	if (out == NULL)
		return NULL;
	/* we have a frame, play with times */
	if (!tris_tvzero(delivery)) {
		/* Regenerate prediction after a discontinuity */
		if (tris_tvzero(path->nextout))
			path->nextout = tris_tvnow();

		/* Use next predicted outgoing timestamp */
		out->delivery = path->nextout;
		
		/* Predict next outgoing timestamp from samples in this
		   frame. */
		path->nextout = tris_tvadd(path->nextout, tris_samp2tv(out->samples, tris_format_rate(out->subclass)));
	} else {
		out->delivery = tris_tv(0, 0);
		tris_set2_flag(out, has_timing_info, TRIS_FRFLAG_HAS_TIMING_INFO);
		if (has_timing_info) {
			out->ts = ts;
			out->len = len;
			out->seqno = seqno;
		}
	}
	/* Invalidate prediction if we're entering a silence period */
	if (out->frametype == TRIS_FRAME_CNG)
		path->nextout = tris_tv(0, 0);
	return out;
}

/*! \brief compute the cost of a single translation step */
static void calc_cost(struct tris_translator *t, int seconds)
{
	int num_samples = 0;
	struct tris_trans_pvt *pvt;
	struct rusage start;
	struct rusage end;
	int cost;
	int out_rate = tris_format_rate(t->dstfmt);

	if (!seconds)
		seconds = 1;
	
	/* If they don't make samples, give them a terrible score */
	if (!t->sample) {
		tris_log(LOG_WARNING, "Translator '%s' does not produce sample frames.\n", t->name);
		t->cost = 999999;
		return;
	}

	pvt = newpvt(t);
	if (!pvt) {
		tris_log(LOG_WARNING, "Translator '%s' appears to be broken and will probably fail.\n", t->name);
		t->cost = 999999;
		return;
	}

	getrusage(RUSAGE_SELF, &start);

	/* Call the encoder until we've processed the required number of samples */
	while (num_samples < seconds * out_rate) {
		struct tris_frame *f = t->sample();
		if (!f) {
			tris_log(LOG_WARNING, "Translator '%s' failed to produce a sample frame.\n", t->name);
			destroy(pvt);
			t->cost = 999999;
			return;
		}
		framein(pvt, f);
		tris_frfree(f);
		while ((f = t->frameout(pvt))) {
			num_samples += f->samples;
			tris_frfree(f);
		}
	}

	getrusage(RUSAGE_SELF, &end);

	cost = ((end.ru_utime.tv_sec - start.ru_utime.tv_sec) * 1000000) + end.ru_utime.tv_usec - start.ru_utime.tv_usec;
	cost += ((end.ru_stime.tv_sec - start.ru_stime.tv_sec) * 1000000) + end.ru_stime.tv_usec - start.ru_stime.tv_usec;

	destroy(pvt);

	t->cost = cost / seconds;

	if (!t->cost)
		t->cost = 1;
}

/*!
 * \brief rebuild a translation matrix.
 * \note This function expects the list of translators to be locked
*/
static void rebuild_matrix(int samples)
{
	struct tris_translator *t;
	int x;      /* source format index */
	int y;      /* intermediate format index */
	int z;      /* destination format index */

	tris_debug(1, "Resetting translation matrix\n");

	memset(tr_matrix, '\0', sizeof(tr_matrix));

	/* first, compute all direct costs */
	TRIS_RWLIST_TRAVERSE(&translators, t, list) {
		if (!t->active)
			continue;

		x = t->srcfmt;
		z = t->dstfmt;

		if (samples)
			calc_cost(t, samples);
	  
		if (!tr_matrix[x][z].step || t->cost < tr_matrix[x][z].cost) {
			tr_matrix[x][z].step = t;
			tr_matrix[x][z].cost = t->cost;
		}
	}

	/*
	 * For each triple x, y, z of distinct formats, check if there is
	 * a path from x to z through y which is cheaper than what is
	 * currently known, and in case, update the matrix.
	 * Repeat until the matrix is stable.
	 */
	for (;;) {
		int changed = 0;
		for (x = 0; x < MAX_FORMAT; x++) {      /* source format */
			for (y = 0; y < MAX_FORMAT; y++) {    /* intermediate format */
				if (x == y)                     /* skip ourselves */
					continue;

				for (z = 0; z<MAX_FORMAT; z++) {  /* dst format */
					int newcost;

					if (z == x || z == y)       /* skip null conversions */
						continue;
					if (!tr_matrix[x][y].step)  /* no path from x to y */
						continue;
					if (!tr_matrix[y][z].step)  /* no path from y to z */
						continue;
					newcost = tr_matrix[x][y].cost + tr_matrix[y][z].cost;
					if (tr_matrix[x][z].step && newcost >= tr_matrix[x][z].cost)
						continue;               /* x->y->z is more expensive than
						                         * the existing path */
					/* ok, we can get from x to z via y with a cost that
					   is the sum of the transition from x to y and
					   from y to z */
						 
					tr_matrix[x][z].step = tr_matrix[x][y].step;
					tr_matrix[x][z].cost = newcost;
					tr_matrix[x][z].multistep = 1;
					tris_debug(3, "Discovered %d cost path from %s to %s, via %s\n", tr_matrix[x][z].cost,
						  tris_getformatname(1 << x), tris_getformatname(1 << z), tris_getformatname(1 << y));
					changed++;
				}
			}
		}
		if (!changed)
			break;
	}
}

static char *handle_cli_core_show_translation(struct tris_cli_entry *e, int cmd, struct tris_cli_args *a)
{
#define SHOW_TRANS 16
	int x, y, z;
	int curlen = 0, longest = 0, magnitude[SHOW_TRANS] = { 0, };

	switch (cmd) {
	case CLI_INIT:
		e->command = "core show translation [recalc]";
		e->usage =
			"Usage: core show translation [recalc [<recalc seconds>]]\n"
			"       Displays known codec translators and the cost associated\n"
			"       with each conversion.  If the argument 'recalc' is supplied along\n"
			"       with optional number of seconds to test a new test will be performed\n"
			"       as the chart is being displayed.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc > 5)
		return CLI_SHOWUSAGE;

	if (a->argv[3] && !strcasecmp(a->argv[3], "recalc")) {
		z = a->argv[4] ? atoi(a->argv[4]) : 1;

		if (z <= 0) {
			tris_cli(a->fd, "         Recalc must be greater than 0.  Defaulting to 1.\n");
			z = 1;
		}

		if (z > MAX_RECALC) {
			tris_cli(a->fd, "         Maximum limit of recalc exceeded by %d, truncating value to %d\n", z - MAX_RECALC, MAX_RECALC);
			z = MAX_RECALC;
		}
		tris_cli(a->fd, "         Recalculating Codec Translation (number of sample seconds: %d)\n\n", z);
		TRIS_RWLIST_WRLOCK(&translators);
		rebuild_matrix(z);
		TRIS_RWLIST_UNLOCK(&translators);
	} else if (a->argc > 3)
		return CLI_SHOWUSAGE;

	TRIS_RWLIST_RDLOCK(&translators);

	tris_cli(a->fd, "         Translation times between formats (in microseconds) for one second of data\n");
	tris_cli(a->fd, "          Source Format (Rows) Destination Format (Columns)\n\n");
	/* Get the length of the longest (usable?) codec name, so we know how wide the left side should be */
	for (x = 0; x < SHOW_TRANS; x++) {
		curlen = strlen(tris_getformatname(1 << (x)));
		if (curlen > longest)
			longest = curlen;
		for (y = 0; y < SHOW_TRANS; y++) {
			if (tr_matrix[x][y].cost > pow(10, magnitude[x])) {
				magnitude[y] = floor(log10(tr_matrix[x][y].cost));
			}
		}
	}
	for (x = -1; x < SHOW_TRANS; x++) {
		struct tris_str *out = tris_str_alloca(125);
		/*Go ahead and move to next iteration if dealing with an unknown codec*/
		if(x >= 0 && !strcmp(tris_getformatname(1 << (x)), "unknown"))
			continue;
		tris_str_set(&out, -1, " ");
		for (y = -1; y < SHOW_TRANS; y++) {
			/*Go ahead and move to next iteration if dealing with an unknown codec*/
			if (y >= 0 && !strcmp(tris_getformatname(1 << (y)), "unknown"))
				continue;
			if (y >= 0)
				curlen = strlen(tris_getformatname(1 << (y)));
			if (y >= 0 && magnitude[y] + 1 > curlen) {
				curlen = magnitude[y] + 1;
			}
			if (curlen < 5)
				curlen = 5;
			if (x >= 0 && y >= 0 && tr_matrix[x][y].step) {
				/* Actual codec output */
				tris_str_append(&out, -1, "%*d", curlen + 1, tr_matrix[x][y].cost);
			} else if (x == -1 && y >= 0) {
				/* Top row - use a dynamic size */
				tris_str_append(&out, -1, "%*s", curlen + 1, tris_getformatname(1 << (y)) );
			} else if (y == -1 && x >= 0) {
				/* Left column - use a static size. */
				tris_str_append(&out, -1, "%*s", longest, tris_getformatname(1 << (x)) );
			} else if (x >= 0 && y >= 0) {
				/* Codec not supported */
				tris_str_append(&out, -1, "%*s", curlen + 1, "-");
			} else {
				/* Upper left hand corner */
				tris_str_append(&out, -1, "%*s", longest, "");
			}
		}
		tris_str_append(&out, -1, "\n");
		tris_cli(a->fd, "%s", tris_str_buffer(out));
	}
	TRIS_RWLIST_UNLOCK(&translators);
	return CLI_SUCCESS;
}

static struct tris_cli_entry cli_translate[] = {
	TRIS_CLI_DEFINE(handle_cli_core_show_translation, "Display translation matrix")
};

/*! \brief register codec translator */
int __tris_register_translator(struct tris_translator *t, struct tris_module *mod)
{
	static int added_cli = 0;
	struct tris_translator *u;
	char tmp[80];

	if (!mod) {
		tris_log(LOG_WARNING, "Missing module pointer, you need to supply one\n");
		return -1;
	}

	if (!t->buf_size) {
		tris_log(LOG_WARNING, "empty buf size, you need to supply one\n");
		return -1;
	}

	t->module = mod;

	t->srcfmt = powerof(t->srcfmt);
	t->dstfmt = powerof(t->dstfmt);
	t->active = 1;

	if (t->srcfmt == -1 || t->dstfmt == -1) {
		tris_log(LOG_WARNING, "Invalid translator path: (%s codec is not valid)\n", t->srcfmt == -1 ? "starting" : "ending");
		return -1;
	}
	if (t->plc_samples) {
		if (t->buffer_samples < t->plc_samples) {
			tris_log(LOG_WARNING, "plc_samples %d buffer_samples %d\n",
				t->plc_samples, t->buffer_samples);
			return -1;
		}
		if (t->dstfmt != powerof(TRIS_FORMAT_SLINEAR))
			tris_log(LOG_WARNING, "plc_samples %d format %x\n",
				t->plc_samples, t->dstfmt);
	}
	if (t->srcfmt >= MAX_FORMAT) {
		tris_log(LOG_WARNING, "Source format %s is larger than MAX_FORMAT\n", tris_getformatname(t->srcfmt));
		return -1;
	}

	if (t->dstfmt >= MAX_FORMAT) {
		tris_log(LOG_WARNING, "Destination format %s is larger than MAX_FORMAT\n", tris_getformatname(t->dstfmt));
		return -1;
	}

	if (t->buf_size) {
		/*
		 * Align buf_size properly, rounding up to the machine-specific
		 * alignment for pointers.
		 */
		struct _test_align { void *a, *b; } p;
		int align = (char *)&p.b - (char *)&p.a;

		t->buf_size = ((t->buf_size + align - 1) / align) * align;
	}

	if (t->frameout == NULL)
		t->frameout = default_frameout;
  
	calc_cost(t, 1);

	tris_verb(2, "Registered translator '%s' from format %s to %s, cost %d\n",
			    term_color(tmp, t->name, COLOR_MAGENTA, COLOR_BLACK, sizeof(tmp)),
			    tris_getformatname(1 << t->srcfmt), tris_getformatname(1 << t->dstfmt), t->cost);

	if (!added_cli) {
		tris_cli_register_multiple(cli_translate, ARRAY_LEN(cli_translate));
		added_cli++;
	}

	TRIS_RWLIST_WRLOCK(&translators);

	/* find any existing translators that provide this same srcfmt/dstfmt,
	   and put this one in order based on cost */
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&translators, u, list) {
		if ((u->srcfmt == t->srcfmt) &&
		    (u->dstfmt == t->dstfmt) &&
		    (u->cost > t->cost)) {
			TRIS_RWLIST_INSERT_BEFORE_CURRENT(t, list);
			t = NULL;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;

	/* if no existing translator was found for this format combination,
	   add it to the beginning of the list */
	if (t)
		TRIS_RWLIST_INSERT_HEAD(&translators, t, list);

	rebuild_matrix(0);

	TRIS_RWLIST_UNLOCK(&translators);

	return 0;
}

/*! \brief unregister codec translator */
int tris_unregister_translator(struct tris_translator *t)
{
	char tmp[80];
	struct tris_translator *u;
	int found = 0;

	TRIS_RWLIST_WRLOCK(&translators);
	TRIS_RWLIST_TRAVERSE_SAFE_BEGIN(&translators, u, list) {
		if (u == t) {
			TRIS_RWLIST_REMOVE_CURRENT(list);
			tris_verb(2, "Unregistered translator '%s' from format %s to %s\n", term_color(tmp, t->name, COLOR_MAGENTA, COLOR_BLACK, sizeof(tmp)), tris_getformatname(1 << t->srcfmt), tris_getformatname(1 << t->dstfmt));
			found = 1;
			break;
		}
	}
	TRIS_RWLIST_TRAVERSE_SAFE_END;

	if (found)
		rebuild_matrix(0);

	TRIS_RWLIST_UNLOCK(&translators);

	return (u ? 0 : -1);
}

void tris_translator_activate(struct tris_translator *t)
{
	TRIS_RWLIST_WRLOCK(&translators);
	t->active = 1;
	rebuild_matrix(0);
	TRIS_RWLIST_UNLOCK(&translators);
}

void tris_translator_deactivate(struct tris_translator *t)
{
	TRIS_RWLIST_WRLOCK(&translators);
	t->active = 0;
	rebuild_matrix(0);
	TRIS_RWLIST_UNLOCK(&translators);
}

/*! \brief Calculate our best translator source format, given costs, and a desired destination */
int tris_translator_best_choice(int *dst, int *srcs)
{
	int x,y;
	int best = -1;
	int bestdst = 0;
	int cur, cursrc;
	int besttime = INT_MAX;
	int beststeps = INT_MAX;
	int common = ((*dst) & (*srcs)) & TRIS_FORMAT_AUDIO_MASK;	/* are there common formats ? */

	if (common) { /* yes, pick one and return */
		for (cur = 1, y = 0; y <= MAX_AUDIO_FORMAT; cur <<= 1, y++) {
			if (cur & common)	/* guaranteed to find one */
				break;
		}
		/* We are done, this is a common format to both. */
		*srcs = *dst = cur;
		return 0;
	} else {	/* No, we will need to translate */
		TRIS_RWLIST_RDLOCK(&translators);
		for (cur = 1, y = 0; y <= MAX_AUDIO_FORMAT; cur <<= 1, y++) {
			if (! (cur & *dst))
				continue;
			for (cursrc = 1, x = 0; x <= MAX_AUDIO_FORMAT; cursrc <<= 1, x++) {
				if (!(*srcs & cursrc) || !tr_matrix[x][y].step ||
				    tr_matrix[x][y].cost >  besttime)
					continue;	/* not existing or no better */
				if (tr_matrix[x][y].cost < besttime ||
				    tr_matrix[x][y].multistep < beststeps) {
					/* better than what we have so far */
					best = cursrc;
					bestdst = cur;
					besttime = tr_matrix[x][y].cost;
					beststeps = tr_matrix[x][y].multistep;
				}
			}
		}
		TRIS_RWLIST_UNLOCK(&translators);
		if (best > -1) {
			*srcs = best;
			*dst = bestdst;
			best = 0;
		}
		return best;
	}
}

unsigned int tris_translate_path_steps(unsigned int dest, unsigned int src)
{
	unsigned int res = -1;

	/* convert bitwise format numbers into array indices */
	src = powerof(src);
	dest = powerof(dest);

	if (src == -1 || dest == -1) {
		tris_log(LOG_WARNING, "No translator path: (%s codec is not valid)\n", src == -1 ? "starting" : "ending");
		return -1;
	}
	TRIS_RWLIST_RDLOCK(&translators);

	if (tr_matrix[src][dest].step)
		res = tr_matrix[src][dest].multistep + 1;

	TRIS_RWLIST_UNLOCK(&translators);

	return res;
}

unsigned int tris_translate_available_formats(unsigned int dest, unsigned int src)
{
	unsigned int res = dest;
	unsigned int x;
	unsigned int src_audio = src & TRIS_FORMAT_AUDIO_MASK;
	unsigned int src_video = src & TRIS_FORMAT_VIDEO_MASK;

	/* if we don't have a source format, we just have to try all
	   possible destination formats */
	if (!src)
		return dest;

	/* If we have a source audio format, get its format index */
	if (src_audio)
		src_audio = powerof(src_audio);

	/* If we have a source video format, get its format index */
	if (src_video)
		src_video = powerof(src_video);

	TRIS_RWLIST_RDLOCK(&translators);

	/* For a given source audio format, traverse the list of
	   known audio formats to determine whether there exists
	   a translation path from the source format to the
	   destination format. */
	for (x = 1; src_audio && (x & TRIS_FORMAT_AUDIO_MASK); x <<= 1) {
		/* if this is not a desired format, nothing to do */
		if (!(dest & x))
			continue;

		/* if the source is supplying this format, then
		   we can leave it in the result */
		if (src & x)
			continue;

		/* if we don't have a translation path from the src
		   to this format, remove it from the result */
		if (!tr_matrix[src_audio][powerof(x)].step) {
			res &= ~x;
			continue;
		}

		/* now check the opposite direction */
		if (!tr_matrix[powerof(x)][src_audio].step)
			res &= ~x;
	}

	/* For a given source video format, traverse the list of
	   known video formats to determine whether there exists
	   a translation path from the source format to the
	   destination format. */
	for (; src_video && (x & TRIS_FORMAT_VIDEO_MASK); x <<= 1) {
		/* if this is not a desired format, nothing to do */
		if (!(dest & x))
			continue;

		/* if the source is supplying this format, then
		   we can leave it in the result */
		if (src & x)
			continue;

		/* if we don't have a translation path from the src
		   to this format, remove it from the result */
		if (!tr_matrix[src_video][powerof(x)].step) {
			res &= ~x;
			continue;
		}

		/* now check the opposite direction */
		if (!tr_matrix[powerof(x)][src_video].step)
			res &= ~x;
	}

	TRIS_RWLIST_UNLOCK(&translators);

	return res;
}

void tris_translate_frame_freed(struct tris_frame *fr)
{
	struct tris_trans_pvt *pvt;

	tris_clear_flag(fr, TRIS_FRFLAG_FROM_TRANSLATOR);

	pvt = (struct tris_trans_pvt *) (((char *) fr) - offsetof(struct tris_trans_pvt, f));

	if (!pvt->destroy)
		return;
	
	destroy(pvt);
}
