#include "extvlan.h"

static uint32_t word(const uint8_t *e, int w)
{
	const uint8_t *p = e + w * 4;

	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] << 8) | p[3];
}

void evtocd_decode(const uint8_t *e, struct evtocd_entry *out)
{
	uint32_t w0 = word(e, 0), w1 = word(e, 1);
	uint32_t w2 = word(e, 2), w3 = word(e, 3);

	out->f_out_pri  = (uint8_t)(w0 >> 28);
	out->f_out_vid  = (uint16_t)((w0 >> 15) & 0x1fff);
	out->f_out_tpid = (uint8_t)((w0 >> 12) & 7);

	out->f_in_pri   = (uint8_t)(w1 >> 28);
	out->f_in_vid   = (uint16_t)((w1 >> 15) & 0x1fff);
	out->f_in_tpid  = (uint8_t)((w1 >> 12) & 7);
	out->f_ethertype = (uint8_t)(w1 & 0xf);

	out->remove_tags = (uint8_t)(w2 >> 30);
	out->t_out_pri  = (uint8_t)((w2 >> 16) & 0xf);
	out->t_out_vid  = (uint16_t)((w2 >> 3) & 0x1fff);
	out->t_out_tpid = (uint8_t)(w2 & 7);

	out->t_in_pri   = (uint8_t)((w3 >> 16) & 0xf);
	out->t_in_vid   = (uint16_t)((w3 >> 3) & 0x1fff);
	out->t_in_tpid  = (uint8_t)(w3 & 7);
}

int evtocd_ds_skipped(const struct evtocd_entry *e)
{
	return e->f_ethertype > EVTOCD_F_ET_IPOE_0800 ||
	       e->remove_tags == EVTOCD_T_DISCARD_FRAME;
}

int evtocd_default_rule_tags(const struct evtocd_entry *e)
{
	int outer_default = e->f_out_pri == EVTOCD_F_PRI_DEFAULT_RULE;
	int inner_default = e->f_in_pri == EVTOCD_F_PRI_DEFAULT_RULE;
	int outer_ignored = e->f_out_pri == EVTOCD_F_PRI_IGNORE_OTHER;

	/* Which tag count a default rule is FOR is the vendor branch it lands
	 * in, and this returned one too few until the description test
	 * disagreed with it.
	 *
	 *   outer IGNORE, inner DEFAULT   the single-tag branch,
	 *                                 commented "do nothing, C-F->C-F"
	 *   outer DEFAULT, inner DEFAULT  the DOUBLE-tag branch,
	 *                                 commented "do nothing, S-C-F->S-C-F"
	 *
	 * There is no both-halves-DEFAULT untagged rule: the untagged branch is
	 * reached by outer IGNORE with inner IGNORE, and it has no DEFAULT case
	 * at all -- it tests the treatment for DISCARD directly.
	 */
	if (outer_default && inner_default)
		return 2;
	if (outer_ignored && inner_default)
		return 1;
	return -1;
}

/* ------------------------------------------------ what an entry means ----- */

/* The TPID a treatment word names.
 *
 * 0 and 1 copy it from the tag the FILTER matched -- inner and outer
 * respectively -- so they can only be resolved when that filter named a tpid.
 * 4 is literally 0x8100. 2, 3, 6 and 7 all resolve to the configured
 * OutputTPID; the DEI variants differ in a bit the stock firmware does not
 * model either.
 */
static uint16_t filter_tpid(uint8_t id, uint16_t input_tpid)
{
	switch (id) {
	case EVTOCD_F_TPID_8100:
		return 0x8100;
	case EVTOCD_F_TPID_INPUT:
	case EVTOCD_F_TPID_INPUT_DEI_0:
	case EVTOCD_F_TPID_INPUT_DEI_1:
		return input_tpid;
	}
	return 0;               /* do not filter: nothing to copy from */
}

static uint16_t treat_tpid(uint8_t id, const struct evtocd_entry *e,
			   uint16_t input_tpid, uint16_t output_tpid)
{
	switch (id) {
	case EVTOCD_T_TPID_COPY_INNER:
		return filter_tpid(e->f_in_tpid, input_tpid);
	case EVTOCD_T_TPID_COPY_OUTER:
		return filter_tpid(e->f_out_tpid, input_tpid);
	case EVTOCD_T_TPID_8100:
		return 0x8100;
	case EVTOCD_T_TPID_DEI_CP_INNER:
	case EVTOCD_T_TPID_DEI_CP_OUTER:
	case EVTOCD_T_TPID_OUTPUT_DEI_0:
	case EVTOCD_T_TPID_OUTPUT_DEI_1:
		return output_tpid;
	}
	return 0;
}

/* Priority action for an extended VLAN rule.
 *
 * Two edges the vendor handles and a naive reading does not: copy-from-inner
 * on a single-tag rule falls back to copy-from-OUTER, with its own comment
 * saying it tolerates the impossible case rather than failing; and either copy
 * on an untagged rule is an error, because there is no tag to copy from.
 */
static void fill_pri(struct evtocd_tagout *t, uint8_t pri, uint8_t tags_in)
{
	switch (pri) {
	case EVTOCD_T_PRI_COPY_INNER:
		if (tags_in == EVTOCD_UNTAGGED)
			t->pri_src = EVTOCD_PRI_IMPOSSIBLE;
		else if (tags_in == EVTOCD_DOUBLE_TAG)
			t->pri_src = EVTOCD_PRI_COPY_INNER;
		else
			t->pri_src = EVTOCD_PRI_COPY_OUTER;
		return;
	case EVTOCD_T_PRI_COPY_OUTER:
		t->pri_src = (tags_in == EVTOCD_UNTAGGED) ? EVTOCD_PRI_IMPOSSIBLE
							  : EVTOCD_PRI_COPY_OUTER;
		return;
	case EVTOCD_T_PRI_FROM_DSCP:
		t->pri_src = EVTOCD_PRI_DSCP;
		return;
	}
	t->pri_src = EVTOCD_PRI_ASSIGN;
	t->pri = pri;
}

static void fill_vid(struct evtocd_tagout *t, uint16_t vid)
{
	if (vid == EVTOCD_T_VID_COPY_INNER)
		t->vid_src = EVTOCD_VID_COPY_INNER;
	else if (vid == EVTOCD_T_VID_COPY_OUTER)
		t->vid_src = EVTOCD_VID_COPY_OUTER;
	else {
		t->vid_src = EVTOCD_VID_ASSIGN;
		t->vid = vid;
	}
}

void evtocd_action(const struct evtocd_entry *e, uint16_t input_tpid,
		   uint16_t output_tpid, struct evtocd_action *out)
{
	for (unsigned i = 0; i < sizeof *out; i++)
		((uint8_t *)out)[i] = 0;

	if (e->f_out_pri != EVTOCD_F_PRI_IGNORE_OTHER)
		out->tags_in = EVTOCD_DOUBLE_TAG;
	else if (e->f_in_pri != EVTOCD_F_PRI_IGNORE_OTHER)
		out->tags_in = EVTOCD_SINGLE_TAG;
	else
		out->tags_in = EVTOCD_UNTAGGED;

	if (e->remove_tags == EVTOCD_T_DISCARD_FRAME) {
		out->discard = 1;
		return;
	}
	out->tags_removed = e->remove_tags;

	/* The single added tag comes from the INNER treatment word, with the
	 * outer set to DO_NOT_ADD. An implementation that reaches for the outer
	 * word first gets every single-VLAN rule wrong, and that is the common
	 * case on this device. */
	if (e->t_out_pri == EVTOCD_T_PRI_DO_NOT_ADD &&
	    e->t_in_pri == EVTOCD_T_PRI_DO_NOT_ADD) {
		out->tags_added = 0;
	} else if (e->t_out_pri == EVTOCD_T_PRI_DO_NOT_ADD) {
		out->tags_added = 1;
		fill_pri(&out->add[0], e->t_in_pri, out->tags_in);
		fill_vid(&out->add[0], e->t_in_vid);
		out->add[0].tpid = treat_tpid(e->t_in_tpid, e, input_tpid,
					      output_tpid);
	} else {
		out->tags_added = 2;
		fill_pri(&out->add[0], e->t_out_pri, out->tags_in);
		fill_vid(&out->add[0], e->t_out_vid);
		out->add[0].tpid = treat_tpid(e->t_out_tpid, e, input_tpid,
					      output_tpid);
		fill_pri(&out->add[1], e->t_in_pri, out->tags_in);
		fill_vid(&out->add[1], e->t_in_vid);
		out->add[1].tpid = treat_tpid(e->t_in_tpid, e, input_tpid,
					      output_tpid);
	}

	out->transparent = (out->tags_removed == 0 && out->tags_added == 0);
}

/* ------------------------------------------------ rendering one entry ----- */
/*
 * Freestanding, so no snprintf: a cursor and explicit bounds. Every append
 * checks the remaining room and truncates rather than overrunning, which is
 * what a 1973-command CLI with no libc has to do everywhere anyway.
 */
struct buf {
	char *p;
	int n, max;
};

static void b_str(struct buf *b, const char *s)
{
	while (*s && b->n < b->max - 1)
		b->p[b->n++] = *s++;
}

static void b_dec(struct buf *b, unsigned v)
{
	char t[6];
	int i = 0;

	if (v == 0) {
		b_str(b, "0");
		return;
	}
	while (v && i < (int)sizeof t) {
		t[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i--)
		if (b->n < b->max - 1)
			b->p[b->n++] = t[i];
}

static void b_hex16(struct buf *b, unsigned v)
{
	static const char d[] = "0123456789abcdef";

	b_str(b, "0x");
	for (int s = 12; s >= 0; s -= 4)
		if (b->n < b->max - 1)
			b->p[b->n++] = d[(v >> s) & 0xf];
}

static void b_tag(struct buf *b, const struct evtocd_tagout *t)
{
	b_str(b, "VLAN ");
	switch (t->vid_src) {
	case EVTOCD_VID_COPY_INNER: b_str(b, "copied from the inner tag"); break;
	case EVTOCD_VID_COPY_OUTER: b_str(b, "copied from the outer tag"); break;
	default:                    b_dec(b, t->vid); break;
	}
	b_str(b, " pri ");
	switch (t->pri_src) {
	case EVTOCD_PRI_COPY_INNER: b_str(b, "copied from the inner tag"); break;
	case EVTOCD_PRI_COPY_OUTER: b_str(b, "copied from the outer tag"); break;
	case EVTOCD_PRI_DSCP:       b_str(b, "from DSCP"); break;
	/* The vendor returns an error here rather than picking something, so
	 * saying so is more honest than printing a number it never uses. */
	case EVTOCD_PRI_IMPOSSIBLE: b_str(b, "UNDEFINED (no tag to copy)"); break;
	default:                    b_dec(b, t->pri); break;
	}
	if (t->tpid) {
		b_str(b, " tpid ");
		b_hex16(b, t->tpid);
	} else {
		/* A treatment that copies the tpid from a filter which does not
		 * name one. Real, and worth showing rather than printing 0x0000. */
		b_str(b, " tpid unresolved");
	}
}

int evtocd_describe(const struct evtocd_entry *e, uint16_t input_tpid,
		    uint16_t output_tpid, char *out, int max)
{
	struct evtocd_action a;
	struct buf b = { out, 0, max };

	if (max < 1)
		return 0;
	evtocd_action(e, input_tpid, output_tpid, &a);

	switch (a.tags_in) {
	case EVTOCD_UNTAGGED:   b_str(&b, "untagged frames"); break;
	case EVTOCD_SINGLE_TAG: b_str(&b, "single-tagged frames"); break;
	default:                b_str(&b, "double-tagged frames"); break;
	}
	if (evtocd_default_rule_tags(e) >= 0)
		b_str(&b, " (the default rule)");
	b_str(&b, ": ");

	if (a.discard) {
		b_str(&b, "DISCARD");
		out[b.n] = 0;
		return b.n;
	}
	if (a.transparent) {
		b_str(&b, "pass through unchanged");
		out[b.n] = 0;
		return b.n;
	}
	if (a.tags_removed) {
		b_str(&b, "remove ");
		b_dec(&b, a.tags_removed);
		b_str(&b, a.tags_removed == 1 ? " tag" : " tags");
		if (a.tags_added)
			b_str(&b, ", ");
	}
	if (a.tags_added) {
		b_str(&b, "add ");
		b_tag(&b, &a.add[0]);
		if (a.tags_added == 2) {
			b_str(&b, " outside ");
			b_tag(&b, &a.add[1]);
		}
	}
	out[b.n] = 0;
	return b.n;
}
