#include "extvlan.h"
#include "omci_bdgconn.h"

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

/* ------------------------------------- the one shape the datapath applies ----- */

/* Whether a default rule leaves the datapath alone: it drops the frames it is
 * the default for, or passes them unchanged. A default rule that adds or
 * removes tags is a shape the builder does not model. */
static int default_is_plain(const struct evtocd_entry *e)
{
	struct evtocd_action a;

	evtocd_action(e, 0x8100, 0x8100, &a);
	return a.discard || a.transparent;
}

const char *evtocd_olt_tag(const uint8_t *tbl, int n, uint16_t output_tpid,
			   int *vid, int *pri)
{
	struct evtocd_entry e, add;
	struct evtocd_action a;
	int have = 0;

	if (n < 1)
		return "the table is empty";
	for (int i = 0; i < n; i++) {
		evtocd_decode(tbl + i * EVTOCD_ENTRY_LEN, &e);
		evtocd_action(&e, 0x8100, 0x8100, &a);
		if (evtocd_default_rule_tags(&e) >= 0) {
			if (!default_is_plain(&e))
				return "a default rule adds or removes tags";
			continue;
		}
		if (a.tags_in != EVTOCD_UNTAGGED)
			return "a rule for tagged frames";
		if (have)
			return "more than one rule for untagged frames";
		have = 1;
		add = e;
	}
	if (!have)
		return "no rule for untagged frames";

	evtocd_action(&add, 0x8100, 0x8100, &a);
	if (a.discard)
		return "untagged frames are discarded";
	if (add.f_ethertype != EVTOCD_F_ET_DO_NOT_FILTER)
		return "the untagged rule filters on the EtherType";
	if (a.tags_removed || a.tags_added != 1)
		return "the untagged rule does not add exactly one tag";
	if (a.add[0].vid_src != EVTOCD_VID_ASSIGN || a.add[0].vid < 1 ||
	    a.add[0].vid > EVTOCD_VID_MAX)
		return "the added VID is not a concrete 1..4094";
	if (a.add[0].pri_src != EVTOCD_PRI_ASSIGN ||
	    a.add[0].pri > EVTOCD_PRI_MAX)
		return "the added priority is not a concrete 0..7";
	/* 4 is 0x8100 outright; 0 copies the filter's TPID, which an untagged
	 * rule does not have, and the manual rule leaves it at 0x8100 too. The
	 * output-TPID variants (2, 3, 6) take the class 171 Output TPID, which
	 * is 0x8100 on every line seen, and a DEI of 0: 6 sets it to 0, and 2
	 * and 3 copy it from a tag an untagged frame does not have. Some OLTs
	 * write 6 with Output TPID 0x8100, the same tag as 4. 7 marks the tag drop-eligible, which the manual
	 * rule never does, and an Output TPID of 0x88a8 is an S-tag. */
	switch (add.t_in_tpid) {
	case EVTOCD_T_TPID_8100:
	case EVTOCD_T_TPID_COPY_INNER:
		break;
	case EVTOCD_T_TPID_DEI_CP_INNER:
	case EVTOCD_T_TPID_DEI_CP_OUTER:
	case EVTOCD_T_TPID_OUTPUT_DEI_0:
		if (output_tpid != 0x8100)
			return "the added tag's TPID is not 0x8100";
		break;
	default:
		return "the added tag's TPID is not 0x8100 with DEI 0";
	}
	*vid = a.add[0].vid;
	*pri = a.add[0].pri;
	return 0;
}

/* ------------------------------------- what the classifier can carry out ----- */

/* A tag of the received frame, as the filter names it. */
struct evtag {
	uint8_t kind;           /* 0 none, EVTOCD_CMP_C + 1, EVTOCD_CMP_S + 1 */
	int16_t vid;            /* -1 not examined */
	int8_t  pri;            /* -1 not examined */
};
#define KIND_C   (EVTOCD_CMP_C + 1)
#define KIND_S   (EVTOCD_CMP_S + 1)

/* The classifier knows two TPIDs: 0x8100 for a C-tag and the primary S-VLAN
 * TPID, 0x88a8. Anything else (0x9100, 0x9200) is a tag it cannot tell from
 * payload. */
static uint8_t tpid_kind(uint16_t tpid)
{
	if (tpid == EVTOCD_TPID_C)
		return KIND_C;
	if (tpid == EVTOCD_TPID_S)
		return KIND_S;
	return 0;
}

/* The kind a filter's TPID field names. `dflt` is what a tag that does not
 * name its TPID is taken to be: the input TPID for the tag a frame has
 * first, since that is what InputTPID is for. */
static const char *filter_kind(uint8_t code, uint16_t input_tpid, uint8_t dflt,
			       uint8_t *kind)
{
	switch (code) {
	case EVTOCD_F_TPID_DO_NOT_FILTER:
		if (!dflt)
			return "the input TPID is not 0x8100 or 0x88a8";
		*kind = dflt;
		return 0;
	case EVTOCD_F_TPID_8100:
		*kind = KIND_C;
		return 0;
	case EVTOCD_F_TPID_INPUT:
		*kind = tpid_kind(input_tpid);
		return *kind ? 0 : "the input TPID is not 0x8100 or 0x88a8";
	case EVTOCD_F_TPID_INPUT_DEI_0:
	case EVTOCD_F_TPID_INPUT_DEI_1:
		return "a filter on the DEI bit";
	}
	return "an undefined filter TPID";
}

static const char *filter_tag(uint8_t pri, uint16_t vid, struct evtag *t)
{
	if (pri == EVTOCD_F_PRI_DO_NOT_FILTER)
		t->pri = -1;
	else if (pri <= EVTOCD_PRI_MAX)
		t->pri = (int8_t)pri;
	else
		return "an undefined filter priority";
	if (vid == EVTOCD_F_VID_DO_NOT_FILTER)
		t->vid = -1;
	else if (vid <= EVTOCD_VID_MAX)
		t->vid = (int16_t)vid;
	else
		return "an undefined filter VID";
	return 0;
}

static void plane_zero(struct evtocd_plane *pl)
{
	pl->in = pl->out = pl->op = 0;
	pl->f_vid = -1;
	pl->f_pri = -1;
	pl->vid = -1;
	pl->pri = -1;
}

/* One tag the treatment adds, resolved. */
struct evadd {
	uint8_t kind;
	int16_t vid;
	int8_t  pri;
	const struct evtag *src_vid;    /* where a copied VID comes from */
	const struct evtag *src_pri;
	uint8_t pri_copy;               /* the priority is a copy */
	uint8_t pri_dscp;
};

/* The tag a "copy" names. A single-tagged frame's one tag is both the inner
 * and the outer one -- the vendor falls back to the outer for a copy from the
 * inner, and G.988 has no other tag to mean -- while a double-tagged frame
 * has the two. */
static const struct evtag *copy_src(const struct evtag *tags, int n, int inner)
{
	if (n == 0)
		return 0;
	if (n == 1)
		return &tags[0];
	return inner ? &tags[1] : &tags[0];
}

const char *evtocd_plan(const struct evtocd_entry *e, uint16_t input_tpid,
			uint16_t output_tpid, struct evtocd_plan *p)
{
	struct evtag in[2], res[2];
	struct evadd add[2];
	struct evtocd_action a;
	uint8_t in_dflt = tpid_kind(input_tpid);
	int n, nadd, nres = 0;
	const char *why;
	struct evtocd_plane *pl[2];

	p->out_tags = 0;
	p->out_vid = -1;
	p->out_pri = -1;
	p->ds_wider = 0;
	p->et_n = 0;
	p->et[0] = p->et[1] = 0;
	p->cmp = EVTOCD_CMP_C;
	plane_zero(&p->s);
	plane_zero(&p->c);
	for (int i = 0; i < 2; i++)
		in[i].kind = res[i].kind = 0;
	pl[0] = &p->c;          /* indexed by kind - 1 */
	pl[1] = &p->s;

	if (evtocd_default_rule_tags(e) >= 0)
		return "a default rule";
	if (e->remove_tags == EVTOCD_T_DISCARD_FRAME)
		return "a rule that discards the frames it matches";

	/* ---- the frame the filter describes */
	if (e->f_out_pri != EVTOCD_F_PRI_IGNORE_OTHER) {
		n = 2;
		if ((why = filter_tag(e->f_out_pri, e->f_out_vid, &in[0])) ||
		    (why = filter_tag(e->f_in_pri, e->f_in_vid, &in[1])) ||
		    (why = filter_kind(e->f_out_tpid, input_tpid, in_dflt, &in[0].kind)) ||
		    (why = filter_kind(e->f_in_tpid, input_tpid, KIND_C, &in[1].kind)))
			return why;
		if (in[0].kind != KIND_S || in[1].kind != KIND_C)
			return "a double-tag rule on two tags of one kind";
		if ((in[0].vid >= 0 || in[0].pri >= 0) && (in[1].vid >= 0 || in[1].pri >= 0))
			return "a filter on both the S-tag and the C-tag behind it";
	} else if (e->f_in_pri != EVTOCD_F_PRI_IGNORE_OTHER) {
		n = 1;
		if ((why = filter_tag(e->f_in_pri, e->f_in_vid, &in[0])) ||
		    (why = filter_kind(e->f_in_tpid, input_tpid, in_dflt, &in[0].kind)))
			return why;
	} else {
		n = 0;
	}
	if (e->remove_tags > n)
		return "a rule that removes more tags than the frame has";
	if (e->f_ethertype != EVTOCD_F_ET_DO_NOT_FILTER) {
		/* The EtherType slot of a row compares the frame's own: behind
		 * a tag it is not known to hold the same value. */
		if (n)
			return "an EtherType filter on tagged frames";
		switch (e->f_ethertype) {
		case EVTOCD_F_ET_IPOE_0800:
			p->et_n = 1; p->et[0] = 0x0800;
			break;
		case EVTOCD_F_ET_PPPOE:
			p->et_n = 2; p->et[0] = 0x8863; p->et[1] = 0x8864;
			break;
		case EVTOCD_F_ET_ARP_0806:
			p->et_n = 1; p->et[0] = 0x0806;
			break;
		case EVTOCD_F_ET_IPV6_86DD:
			p->et_n = 1; p->et[0] = 0x86dd;
			break;
		case EVTOCD_F_ET_EAPOL_888E:
			p->et_n = 1; p->et[0] = 0x888e;
			break;
		default:
			return "an undefined EtherType filter";
		}
	}

	/* ---- the tags the treatment adds, outermost first */
	evtocd_action(e, input_tpid, output_tpid, &a);
	nadd = a.tags_added;
	for (int j = 0; j < nadd; j++) {
		const struct evtocd_tagout *t = &a.add[j];
		uint8_t code = (nadd == 1 || j == 1) ? e->t_in_tpid : e->t_out_tpid;
		uint16_t tpid;

		add[j].src_vid = add[j].src_pri = 0;
		add[j].pri_copy = add[j].pri_dscp = 0;
		switch (code) {
		case EVTOCD_T_TPID_COPY_INNER:
		case EVTOCD_T_TPID_COPY_OUTER: {
			const struct evtag *s = copy_src(in, n, code == EVTOCD_T_TPID_COPY_INNER);

			if (!s)
				return "an added TPID copied from a tag the frame has not got";
			tpid = s->kind == KIND_S ? EVTOCD_TPID_S : EVTOCD_TPID_C;
			break;
		}
		case EVTOCD_T_TPID_8100:
			tpid = EVTOCD_TPID_C;
			break;
		case EVTOCD_T_TPID_DEI_CP_INNER:
		case EVTOCD_T_TPID_DEI_CP_OUTER:
		case EVTOCD_T_TPID_OUTPUT_DEI_0:
			tpid = output_tpid;
			break;
		default:
			return "an added tag with the DEI bit set";
		}
		add[j].kind = tpid_kind(tpid);
		if (!add[j].kind)
			return "an added tag whose TPID is not 0x8100 or 0x88a8";

		switch (t->vid_src) {
		case EVTOCD_VID_ASSIGN:
			if (t->vid > EVTOCD_VID_MAX)
				return "an added VID past 4094";
			add[j].vid = (int16_t)t->vid;
			break;
		default: {
			const struct evtag *s = copy_src(in, n, t->vid_src == EVTOCD_VID_COPY_INNER);

			if (!s)
				return "a VID copied from a tag the frame has not got";
			add[j].vid = s->vid;
			if (s->vid < 0)
				return "an added VID copied from a tag whose VID the filter does not name";
			break;
		}
		}
		switch (t->pri_src) {
		case EVTOCD_PRI_ASSIGN:
			if (t->pri > EVTOCD_PRI_MAX)
				return "an added priority past 7";
			add[j].pri = (int8_t)t->pri;
			break;
		case EVTOCD_PRI_DSCP:
			return "a priority taken from the DSCP";
		case EVTOCD_PRI_IMPOSSIBLE:
			return "a priority copied from a tag the frame has not got";
		default: {
			const struct evtag *s = copy_src(in, n, t->pri_src == EVTOCD_PRI_COPY_INNER);

			if (!s)
				return "a priority copied from a tag the frame has not got";
			add[j].pri = s->pri;    /* -1: only a REWRITE of that tag can keep it */
			add[j].pri_copy = 1;
			add[j].src_pri = s;
			break;
		}
		}
	}

	/* ---- what the frame is after: the added tags, then those left */
	for (int j = 0; j < nadd; j++)
		res[nres++].kind = add[j].kind;
	for (int i = e->remove_tags; i < n; i++) {
		if (nres >= 2)
			return "a rule that leaves three tags on the frame";
		res[nres++] = in[i];
	}
	if (nres == 2 && !(res[0].kind == KIND_S && res[1].kind == KIND_C))
		return "a rule that leaves two tags of one kind, or a C-tag outside an S-tag";

	/* ---- the planes */
	for (int i = 0; i < n; i++)
		pl[in[i].kind - 1]->in = 1;
	for (int i = 0; i < n; i++) {
		struct evtocd_plane *q = pl[in[i].kind - 1];

		q->f_vid = in[i].vid;
		q->f_pri = in[i].pri;
	}
	for (int i = 0; i < nres; i++)
		pl[res[i].kind - 1]->out = 1;
	for (int k = 0; k < 2; k++) {
		struct evtocd_plane *q = pl[k];
		int added = -1;

		for (int j = 0; j < nadd; j++)
			if (add[j].kind == k + 1)
				added = j;
		if (q->in && !q->out) {
			q->op = EVTOCD_OP_POP;
		} else if (!q->in && q->out) {
			q->op = EVTOCD_OP_PUSH;
		} else if (q->in && q->out) {
			q->op = added >= 0 ? EVTOCD_OP_REWRITE : EVTOCD_OP_PASS;
		}
		if (added >= 0 && q->out) {
			/* Written values. The S-tag has no "keep" the classifier is
			 * known to honour, and a keep of the C-tag priority only
			 * means something when that very tag is being replaced. */
			q->vid = add[added].vid;
			q->pri = add[added].pri;
			if (q->pri < 0) {
				if (k == EVTOCD_CMP_S || q->op != EVTOCD_OP_REWRITE ||
				    add[added].src_pri->kind != k + 1)
					return "an added priority copied from a tag whose priority the filter does not name";
			}
		}
	}
	if (p->s.in && p->c.in &&
	    (p->s.f_vid >= 0 || p->s.f_pri >= 0) && (p->c.f_vid >= 0 || p->c.f_pri >= 0))
		return "a filter on both the S-tag and the C-tag behind it";
	p->cmp = (p->s.in && (p->s.f_vid >= 0 || p->s.f_pri >= 0)) ? EVTOCD_CMP_S : EVTOCD_CMP_C;

	/* ---- the outermost tag the frame leaves with */
	p->out_tags = (uint8_t)nres;
	if (nres) {
		const struct evtocd_plane *q = pl[res[0].kind - 1];

		if (q->op == EVTOCD_OP_PASS) {
			p->out_vid = q->f_vid;
			p->out_pri = q->f_pri;
		} else {
			p->out_vid = q->vid;
			p->out_pri = q->pri >= 0 ? q->pri : q->f_pri;
		}
	}
	return 0;
}

/* ---- an entry that discards what it matches ----------------------------- */

const char *evtocd_drop(const struct evtocd_entry *e, uint16_t input_tpid,
			struct evtocd_drop *d)
{
	struct evtag t;
	const char *why;

	if (e->remove_tags != EVTOCD_T_DISCARD_FRAME)
		return "not a discard";
	if (evtocd_default_rule_tags(e) >= 0)
		return "a default rule";
	if (e->f_ethertype != EVTOCD_F_ET_DO_NOT_FILTER)
		return "a discard that filters on the EtherType";
	if (e->f_out_pri != EVTOCD_F_PRI_IGNORE_OTHER)
		return "a discard of two-tagged frames";
	if (e->f_in_pri == EVTOCD_F_PRI_IGNORE_OTHER)
		return "a discard of untagged frames, which the ACL cannot tell from VID 0";
	t.kind = 0;
	if ((why = filter_tag(e->f_in_pri, e->f_in_vid, &t)) ||
	    (why = filter_kind(e->f_in_tpid, input_tpid, tpid_kind(input_tpid), &t.kind)))
		return why;
	if (t.kind != KIND_C)
		return "a discard of S-tagged frames";
	if (t.vid < 1)
		return "a discard of every VID or of VID 0, which the ACL cannot name";
	d->vid = t.vid;
	d->pri = t.pri;
	return 0;
}

const char *evtocd_drop_shadow(const struct evtocd_drop *d, const struct evtocd_plan *r,
			       unsigned *pmask)
{
	/* The ACL field is the C-tag of a frame that has only one; what it
	 * holds behind an S-tag is not known, so a table that acts on an
	 * S-tag is not one a discard can sit in. */
	if (r->s.in)
		return "a discard in a table that acts on S-tagged frames";
	if (!r->c.in)
		return 0;               /* untagged: no C-tag to match */
	if (r->c.f_vid >= 0 && r->c.f_vid != d->vid)
		return 0;
	if (r->c.f_vid < 0) {
		/* Any VID: the discard names one, so it is the more specific,
		 * unless the rule names a priority the discard does not. */
		if (r->c.f_pri >= 0 && d->pri < 0)
			return "a discard and a priority-only rule that overlap";
		return 0;
	}
	/* The same VID. */
	if (r->c.f_pri < 0) {
		if (d->pri < 0)
			return "a discard and a rule with the same filter";
		return 0;               /* the discard names a priority: more specific */
	}
	if (d->pri < 0)
		*pmask &= ~(1u << r->c.f_pri);
	else if (d->pri == r->c.f_pri)
		return "a discard and a rule with the same filter";
	return 0;
}

void evtocd_drop_oper(const struct evtocd_drop *d, int pri, struct omci_vlan_oper *vr)
{
	for (unsigned i = 0; i < sizeof *vr / 4; i++)
		((uint32_t *)vr)[i] = 0;
	vr->rule_gen = OMCI_VLAN_OPER_EXTVLAN_DROP;
	vr->filter.outer.pri = OMCI_PRI_ANY;
	vr->filter.outer.vid = OMCI_VID_ANY;
	vr->filter.outer_mode = OMCI_TAGF_ANY;
	vr->filter.inner.pri = pri >= 0 ? (uint32_t)pri : OMCI_PRI_ANY;
	vr->filter.inner.vid = (uint32_t)d->vid;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.inner_mode = OMCI_TAGF_TAGGED | OMCI_TAGF_VID | (pri >= 0 ? OMCI_TAGF_PRI : 0);
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->outer_act.tag_op = OMCI_TAGOP_PASS;
	vr->inner_act.tag_op = OMCI_TAGOP_PASS;
	vr->out.out_tag.pri = OMCI_PRI_ANY;
	vr->out.out_tag.vid = OMCI_VID_ANY;
}

void evtocd_oper_ethertype(struct omci_vlan_oper *vr, uint16_t et)
{
	vr->filter.ethertype = OMCI_ETHTYPE_LITERAL | et;
}

void evtocd_oper_rest(struct omci_vlan_oper *vr)
{
	vr->filter.ethertype = OMCI_ETHTYPE_REST;
}

/* The downstream plane of an upstream one. `ok` goes 0 when the way back
 * cannot be said: a tag the filter did not name is a tag nobody can put
 * back. */
static void plane_ds(const struct evtocd_plane *u, struct evtocd_plane *d,
		     int is_s, int *ok)
{
	plane_zero(d);
	d->in = u->out;
	d->out = u->in;
	/* What the downstream frame is matched on: the tag as the upstream
	 * treatment leaves it. */
	if (u->out) {
		if (u->op == EVTOCD_OP_PASS) {
			d->f_vid = u->f_vid;
			d->f_pri = u->f_pri;
		} else {
			d->f_vid = u->vid;
			d->f_pri = u->pri >= 0 ? u->pri : u->f_pri;
		}
	}
	switch (u->op) {
	case EVTOCD_OP_PASS:
		d->op = EVTOCD_OP_PASS;
		break;
	case EVTOCD_OP_PUSH:
		d->op = EVTOCD_OP_POP;
		break;
	case EVTOCD_OP_POP:
		d->op = EVTOCD_OP_PUSH;
		if (u->f_vid < 0)
			*ok = 0;
		d->vid = u->f_vid;
		/* A priority the filter did not name is lost with the tag; it
		 * goes back as 0, which is what a frame the ONU tags itself gets. */
		d->pri = u->f_pri >= 0 ? u->f_pri : 0;
		break;
	case EVTOCD_OP_REWRITE:
		d->op = EVTOCD_OP_REWRITE;
		if (u->f_vid < 0)
			*ok = 0;
		d->vid = u->f_vid;
		d->pri = u->f_pri;
		/* Priority the filter did not name stays what the frame has;
		 * for the S-tag that "keep" is not known to work. */
		if (d->pri < 0 && is_s)
			*ok = 0;
		break;
	}
}

int evtocd_plan_ds(const struct evtocd_plan *u, uint8_t ds_mode,
		   struct evtocd_plan *d, const char **why)
{
	int ok = 1;
	int wider = 0;

	if (ds_mode == EVTOCD_DSMODE_FORWARD) {
		*why = "downstream frames are forwarded unchanged";
		return 0;
	}
	if (ds_mode == EVTOCD_DSMODE_DROP_ALL) {
		*why = "downstream frames are discarded, which the classifier cannot be told to do here";
		return 0;
	}
	if (ds_mode > EVTOCD_DSMODE_DROP_ALL) {
		*why = "an undefined downstream mode";
		return 0;
	}
	d->out_tags = 0;
	d->out_vid = -1;
	d->out_pri = -1;
	d->ds_wider = 0;
	d->et_n = 0;
	d->et[0] = d->et[1] = 0;
	plane_ds(&u->c, &d->c, 0, &ok);
	plane_ds(&u->s, &d->s, 1, &ok);
	if (!ok) {
		*why = "the rule merges frames the filter did not tell apart, so it cannot be undone";
		return 0;
	}
	/* Modes 5 to 7 want the frames that do not match discarded; without a
	 * drop action the nearest thing is the matching rule alone. */
	if (ds_mode >= EVTOCD_DSMODE_VID_PRI_DROP && ds_mode <= EVTOCD_DSMODE_PRI_DROP) {
		wider = 1;
		ds_mode = (uint8_t)(ds_mode - (EVTOCD_DSMODE_VID_PRI_DROP - EVTOCD_DSMODE_VID_PRI));
	}
	if (ds_mode == EVTOCD_DSMODE_VID || ds_mode == EVTOCD_DSMODE_PRI) {
		struct evtocd_plane *q[2] = { &d->c, &d->s };

		for (int k = 0; k < 2; k++) {
			if (ds_mode == EVTOCD_DSMODE_VID)
				q[k]->f_pri = -1;
			else
				q[k]->f_vid = -1;
		}
	}
	/* One tag is compared. When both of the frame's tags carry something to
	 * match, the S-tag does, and the C-tag behind it is not examined. */
	d->cmp = (d->s.in && (d->s.f_vid >= 0 || d->s.f_pri >= 0)) ? EVTOCD_CMP_S : EVTOCD_CMP_C;
	if (d->s.in && d->c.in && d->cmp == EVTOCD_CMP_S &&
	    (d->c.f_vid >= 0 || d->c.f_pri >= 0)) {
		d->c.f_vid = -1;
		d->c.f_pri = -1;
		wider = 1;
	}
	if ((ds_mode == EVTOCD_DSMODE_VID || ds_mode == EVTOCD_DSMODE_PRI) &&
	    d->c.f_vid < 0 && d->c.f_pri < 0 && d->s.f_vid < 0 && d->s.f_pri < 0) {
		*why = "downstream matches on a field the rule does not set";
		return 0;
	}
	d->ds_wider = (uint8_t)wider;
	d->out_tags = (uint8_t)(u->s.in + u->c.in);
	return 1;
}


const char *evtocd_plan_lane(struct evtocd_plan *p, uint8_t *ds_mode)
{
	struct evtocd_plane *q = &p->c;

	if (p->s.in || p->c.in) {
		const struct evtocd_plane *m = p->cmp == EVTOCD_CMP_S ? &p->s : &p->c;

		if (m->f_vid < 0)
			return "a rule on tagged frames of any VID, which on the host lane are every service's";
		return 0;
	}
	if (!p->out_tags)
		return "a rule that leaves the frames untagged, which on the host lane are every service's";
	if (p->out_tags != 1 || !q->out || q->op != EVTOCD_OP_PUSH)
		return "a rule that adds an S-tag, or two tags, which the router does not send";
	/* PUSH from an untagged match always has a written VID and priority:
	 * evtocd_plan() refuses a copy from a tag the frame has not got. */
	q->in = 1;
	q->f_vid = q->vid;
	q->f_pri = -1;
	q->op = EVTOCD_OP_REWRITE;
	p->cmp = EVTOCD_CMP_C;
	/* The EtherType of a frame behind a tag is not compared (evtocd_plan()):
	 * entries that told IPoE from ARP all come to this one rule. */
	p->et_n = 0;
	p->et[0] = p->et[1] = 0;
	*ds_mode = EVTOCD_DSMODE_VID;
	return 0;
}

/* ---- a plan as the bridge rule the switch driver reads

 * The rule_gen OMCI_VLAN_OPER_EXTVLAN_GEN encoding of omci_bdgconn.h: one
 * direction, filter and treatment both stated outright, the S-tag as the
 * "outer" half and the C-tag as the "inner" one. The driver does not derive
 * a downstream rule from it; the downstream one is a plan of its own. */
static uint32_t tag_op_of(uint8_t op)
{
	switch (op) {
	case EVTOCD_OP_PUSH:    return OMCI_TAGOP_PUSH;
	case EVTOCD_OP_REWRITE: return OMCI_TAGOP_REWRITE;
	case EVTOCD_OP_POP:     return OMCI_TAGOP_POP;
	}
	return OMCI_TAGOP_PASS;
}

static void oper_plane(const struct evtocd_plane *q, struct omci_vlan *f,
		       uint32_t *mode, struct omci_vlan_act *act)
{
	*mode = q->in ? OMCI_TAGF_TAGGED : OMCI_TAGF_UNTAGGED;
	f->pri = OMCI_PRI_ANY;
	f->vid = OMCI_VID_ANY;
	f->tpid = OMCI_FILTER_TPID_DO_NOT;
	if (q->in && q->f_vid >= 0) {
		*mode |= OMCI_TAGF_VID;
		f->vid = (uint32_t)q->f_vid;
	}
	if (q->in && q->f_pri >= 0) {
		*mode |= OMCI_TAGF_PRI;
		f->pri = (uint32_t)q->f_pri;
	}
	act->tag_op = tag_op_of(q->op);
	act->vid_op = OMCI_VIDOP_SET;
	act->pri_op = q->pri < 0 && (q->op == EVTOCD_OP_REWRITE) ?
		      OMCI_PRIOP_PASS : OMCI_PRIOP_SET;
	act->set_tag.pri = q->pri >= 0 ? (uint32_t)q->pri : 0;
	act->set_tag.vid = q->vid >= 0 ? (uint32_t)q->vid : 0;
	act->set_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
}

void evtocd_plan_oper(const struct evtocd_plan *p, struct omci_vlan_oper *vr)
{
	for (unsigned i = 0; i < sizeof *vr / 4; i++)
		((uint32_t *)vr)[i] = 0;
	vr->rule_gen = OMCI_VLAN_OPER_EXTVLAN_GEN;
	oper_plane(&p->s, &vr->filter.outer, &vr->filter.outer_mode, &vr->outer_act);
	oper_plane(&p->c, &vr->filter.inner, &vr->filter.inner_mode, &vr->inner_act);
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->out.is_default = 0;
	vr->out.is_mcast = 0;
	vr->out.tag_count = p->out_tags;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.ds_mode = 0;
	vr->out.ds_tag_op = 0;
	vr->out.out_tag.pri = p->out_pri >= 0 ? (uint32_t)p->out_pri : OMCI_PRI_ANY;
	vr->out.out_tag.vid = p->out_vid >= 0 ? (uint32_t)p->out_vid : OMCI_VID_ANY;
	vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
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

static void b_plane(struct buf *b, const char *name, const struct evtocd_plane *q)
{
	b_str(b, name);
	switch (q->op) {
	case EVTOCD_OP_PUSH:    b_str(b, " add"); break;
	case EVTOCD_OP_REWRITE: b_str(b, " replace"); break;
	case EVTOCD_OP_POP:     b_str(b, " remove"); break;
	default:                b_str(b, " keep"); break;
	}
	if (q->op == EVTOCD_OP_PUSH || q->op == EVTOCD_OP_REWRITE) {
		b_str(b, " VID ");
		b_dec(b, (unsigned)q->vid);
		if (q->pri >= 0) {
			b_str(b, " pri ");
			b_dec(b, (unsigned)q->pri);
		} else {
			b_str(b, " pri kept");
		}
	}
}

int evtocd_plan_describe(const struct evtocd_plan *p, char *out, int max)
{
	struct buf b = { out, 0, max };
	int any = 0;

	if (max < 1)
		return 0;
	b_str(&b, "match ");
	if (!p->s.in && !p->c.in)
		b_str(&b, "untagged");
	if (p->s.in) {
		b_str(&b, "S-tag");
		if (p->s.f_vid >= 0) { b_str(&b, " VID "); b_dec(&b, (unsigned)p->s.f_vid); }
		if (p->s.f_pri >= 0) { b_str(&b, " pri "); b_dec(&b, (unsigned)p->s.f_pri); }
		any = 1;
	}
	if (p->c.in) {
		if (any)
			b_str(&b, " + ");
		b_str(&b, "C-tag");
		if (p->c.f_vid >= 0) { b_str(&b, " VID "); b_dec(&b, (unsigned)p->c.f_vid); }
		if (p->c.f_pri >= 0) { b_str(&b, " pri "); b_dec(&b, (unsigned)p->c.f_pri); }
	}
	b_str(&b, ": ");
	if (p->s.op == EVTOCD_OP_PASS && p->c.op == EVTOCD_OP_PASS) {
		b_str(&b, "pass through unchanged");
	} else {
		if (p->s.op != EVTOCD_OP_PASS)
			b_plane(&b, "S-tag", &p->s);
		if (p->c.op != EVTOCD_OP_PASS) {
			if (p->s.op != EVTOCD_OP_PASS)
				b_str(&b, ", ");
			b_plane(&b, "C-tag", &p->c);
		}
	}
	out[b.n] = 0;
	return b.n;
}
