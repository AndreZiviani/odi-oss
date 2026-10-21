// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_bdgconn.c -- cmd 51 and cmd 50, activate and deactivate a
 * bridge connection: the service list, the CF table as programmed, and
 * the derivation of CF rows and a VLAN row from an OMCI bridge rule.
 * odi_switch_cmd() dispatches here; odi_sw_cf_add() (odi_switch_cf.c)
 * writes the result.
 */
#include "odi_switch_cmd.h"
#include "odi_switch_dal.h"

#include "uapi/omci_bdgconn.h"

/* One slot per active service id (omci_bdgconn.service_id), holding the
 * CF rows that service owns and the VLAN row it contributes. A second activation of an active
 * service_id (omcid merging another ingress into its uni_mask) updates the
 * service in place; cmd 50 frees it.
 */
struct odi_sw_bdgconn_slot {
	int32_t serv_id;
	int used;
	uint32_t dir;
	int us_row, ds_row;	/* CF row index, -1 when the leg is absent */
	int acl_row;		/* ACL drop entry (odi_sw_acl[] index), -1 none */
	int ds_flow;		/* the downstream flow its GEM port holds, -1 none */
	uint32_t vlan_vid;	/* 0: no VLAN row of its own */
	uint32_t vlan_val;
	uint32_t vlan_any;	/* 0, or its members in every row 2..4094 */
};
static struct odi_sw_bdgconn_slot odi_sw_bdgconn[ODI_SW_CMD_BDGCONN_MAX];

/* The CF table as this dispatch has programmed it, indexed by row. */
struct odi_sw_cf_row {
	int used;
	int is_us;
	int tmpl;		/* compares the S-tag (CLASSIFY_TEMPLATE 1) */
	unsigned int key;	/* ordering class, see cf_key() */
	int et;			/* compares an EtherType: only in the second layout */
	uint32_t rule_w0, mask_w0;	/* the EtherType, bits 15:0, else 0 */
	uint32_t rule_w1, mask_w1;
	uint32_t act[3];
};
#define ODI_SW_CF_ROWS		256U
static struct odi_sw_cf_row odi_sw_cf[ODI_SW_CF_ROWS];

/* The ACL rows of the extended VLAN discards, as programmed. */
struct odi_sw_acl_row {
	int used;
	uint32_t ports;		/* the UNI ports it acts on */
	int vid, pri;
};
static struct odi_sw_acl_row odi_sw_acl[ODI_SW_ACL_DROP_MAX];

/* The priority each downstream flow's frames take, plus one (0: none, the
 * zeroed state): cmd 25 sets it from the queue the flow's GEM port points
 * at, and the downstream rows of the connections on the flow carry it. */
static uint8_t odi_sw_ds_flow_pri[ODI_SW_CMD_GEM_DS_MAX];

void odi_switch_bdgconn_reset(void)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_GEM_DS_MAX; i++)
		odi_sw_ds_flow_pri[i] = 0;
	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		odi_sw_bdgconn[i].used = 0;
	for (i = 0; i < ODI_SW_CF_ROWS; i++)
		odi_sw_cf[i].used = 0;
	for (i = 0; i < ODI_SW_ACL_DROP_MAX; i++) {
		if (odi_sw_acl[i].used)
			odi_sw_acl_drop_del(ODI_SW_ACL_DROP_BASE + i);
		odi_sw_acl[i].used = 0;
	}
	odi_sw_cf_reset();
}

/* cmd 51: the bridge rule (omci_bdgconn, 160 bytes) turned into CF rows
 * and a VLAN row. The row layouts are in odi_switch_dal.h; this decides
 * what goes in them and where. Derived from the twelve ISP1 instances
 * (six services, each sent twice) and checked against the ISP2 stock VLAN
 * rows; a value no capture exercises is marked "not seen".
 *
 * Board ports: 0 the UNI, 2 the PON, 3 the CPU (cmd 3 reports the same).
 * The chip has two UNI-capable ports, 0 and 1; the board wires 0.
 */
#define ODI_SW_PON_PORT		2U
#define ODI_SW_CPU_PORT		3U
#define ODI_SW_CHIP_UNI_PORTS	0x3U
#define ODI_SW_BOARD_UNI_PORTS	0x1U

/* Rows 64.. hold the ordered service rules; rows that care about no VID
 * (the untagged upstream rule) fill downward from 254; 255 is the
 * downstream default the module-load replay installs. Both captures
 * start at 64 and 254.
 */
#define ODI_SW_CF_SORTED_BASE	64U
#define ODI_SW_CF_BOTTOM	254U
#define ODI_SW_CF_KEY_BOTTOM	3U

#define ODI_SW_ENOSPC		(-28)

/* One direction of a service, as rows. */
struct odi_sw_cf_leg {
	int present;
	int is_us;
	int tmpl;
	unsigned int key;
	int et;			/* names an EtherType (rule_w0, mask_w0) */
	uint32_t rule_w0, mask_w0;
	uint32_t rule_w1, mask_w1;
	uint32_t act[3];
};

/* The ports of uni_mask that are UNIs (not the PON or CPU bit). */
static uint32_t uni_ports(uint32_t uni_mask)
{
	return uni_mask & 0xfU & ~(1U << ODI_SW_PON_PORT) & ~(1U << ODI_SW_CPU_PORT);
}

/* Ordering class of a row. ISP1 places, from row 64 up: rules that care
 * about the tag flags AND the priority (class 0), rules that care about
 * one of the two (1), rules that care about the VID alone (2); each new
 * rule goes after the existing rules of its class. A rule caring no VID
 * at all goes to the bottom region.
 */
static unsigned int cf_key(int vid, int pri, int stag, int ctag)
{
	unsigned int n = 0;

	if (vid < 0)
		return ODI_SW_CF_KEY_BOTTOM;
	if (pri >= 0)
		n++;
	if (stag >= 0 || ctag >= 0)
		n++;
	return 2U - n;
}

/* Match words for one leg. -1 in any argument: not cared about. */
static void cf_match(struct odi_sw_cf_leg *l, int ds, int vid, int pri,
		     int stag, int ctag, int uni)
{
	uint32_t data = ds ? ODI_SW_CF_W1_DS : 0, care = ODI_SW_CF_W1_DS;

	if (vid >= 0) {
		data |= ((uint32_t)vid << ODI_SW_CF_W1_VID_SHIFT) & ODI_SW_CF_W1_VID_MASK;
		care |= ODI_SW_CF_W1_VID_MASK;
	}
	if (pri >= 0) {
		data |= ((uint32_t)pri << ODI_SW_CF_W1_PRI_SHIFT) & ODI_SW_CF_W1_PRI_MASK;
		care |= ODI_SW_CF_W1_PRI_MASK;
	}
	if (stag >= 0) {
		data |= stag ? ODI_SW_CF_W1_STAG : 0;
		care |= ODI_SW_CF_W1_STAG;
	}
	if (ctag >= 0) {
		data |= ctag ? ODI_SW_CF_W1_CTAG : 0;
		care |= ODI_SW_CF_W1_CTAG;
	}
	if (uni >= 0) {
		data |= (uint32_t)uni & ODI_SW_CF_W1_UNI_MASK;
		care |= ODI_SW_CF_W1_UNI_MASK;
	}
	l->rule_w1 = data & care;
	l->mask_w1 = care & ~data;
	l->key = cf_key(vid, pri, stag, ctag);
	l->tmpl = 0;
	l->et = 0;
	l->rule_w0 = l->mask_w0 = 0;
	l->present = 1;
	l->is_us = !ds;
}

/* The match a filter describes: the upstream leg of every rule, and the
 * downstream leg of a downstream-only rule. The tag flags are cared about
 * only when the filter says "no S-tag" (ISP1: the untagged data rule and
 * the multicast rule; the VID filters leave both flags free).
 */
static void cf_match_filter(struct odi_sw_cf_leg *l, const struct omci_vlan_filter *f,
			    int ds, int uni)
{
	int vid = -1, pri = -1, stag = -1, ctag = -1;
	uint32_t cm = f->inner_mode;

	if (f->outer_mode & OMCI_TAGF_UNTAGGED)
		stag = 0;
	else if (f->outer_mode & OMCI_TAGF_TAGGED)
		stag = 1;	/* not seen */
	if (f->outer_mode & (OMCI_TAGF_VID | OMCI_TAGF_PRI | OMCI_TAGF_TCI))
		ODI_SW_CMD_LOG("bdgconn: S-tag VID/priority filter not derived, ignored\n");
	if (cm & OMCI_TAGF_UNTAGGED)
		ctag = 0;
	else if (cm & OMCI_TAGF_TAGGED)
		ctag = 1;	/* not seen */
	if ((cm & (OMCI_TAGF_VID | OMCI_TAGF_TCI)) && f->inner.vid < 4096U)
		vid = (int)f->inner.vid;
	if ((cm & (OMCI_TAGF_PRI | OMCI_TAGF_TCI)) && f->inner.pri < 8U)
		pri = (int)f->inner.pri;
	if (ctag < 0 && stag == 0 && (vid >= 0 || pri >= 0))
		ctag = 1;
	if ((cm & OMCI_TAGF_ETHTYPE) || f->ethertype != OMCI_ETHTYPE_NO_CARE)
		ODI_SW_CMD_LOG("bdgconn: ethertype filter not derived, ignored\n");
	cf_match(l, ds, vid, pri, stag, ctag, uni);
}

/* C-tag / S-tag treatment codes as the rule states them (the upstream
 * leg, and a downstream-only rule).
 */
static uint32_t cact_of(uint32_t act)
{
	switch (act) {
	case OMCI_TAGOP_PUSH:		return ODI_SW_CF_CACT_ADD;
	case OMCI_TAGOP_POP:		return ODI_SW_CF_CACT_DEL;
	case OMCI_TAGOP_PASS:	return ODI_SW_CF_CACT_TRANSPARENT;
	case OMCI_TAGOP_REWRITE:		/* not seen: retag with the assigned VID */
		return ODI_SW_CF_CACT_ADD;
	default:			return ODI_SW_CF_CACT_NONE;
	}
}

static uint32_t csact_of(uint32_t act)
{
	switch (act) {
	case OMCI_TAGOP_POP:		return ODI_SW_CF_CSACT_DEL;
	case OMCI_TAGOP_PASS:	return ODI_SW_CF_CSACT_TRANSPARENT;
	case OMCI_TAGOP_PUSH:
	case OMCI_TAGOP_REWRITE:
		ODI_SW_CMD_LOG("bdgconn: S-tag add not derived, left transparent\n");
		return ODI_SW_CF_CSACT_TRANSPARENT;
	default:			return ODI_SW_CF_CSACT_NONE;
	}
}

static uint32_t act_vid(const struct omci_vlan *v)
{
	return v->vid < 4096U ? v->vid : 0;
}

static uint32_t act_pri(const struct omci_vlan *v)
{
	return v->pri < 8U ? v->pri : 0;
}

/* Words 1 and 2 of an action row, the parts both directions share.
 * The VID/priority sources are always "assign": the only mode any rule
 * has used; the other OMCI modes are not seen.
 */
static void cf_act_common(struct odi_sw_cf_leg *l, uint32_t cact, uint32_t c_vid,
			  uint32_t c_pri, uint32_t csact, uint32_t cs_vid, uint32_t cs_pri)
{
	l->act[0] = 0;
	l->act[1] = (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A1_CPRI_ACT_SHIFT) |
		    (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A1_CVID_ACT_SHIFT) |
		    ((c_pri & 7U) << ODI_SW_CF_A1_C_PRI_SHIFT) |
		    ((c_vid & 0xfffU) << ODI_SW_CF_A1_C_VID_SHIFT) |
		    ((cact & 3U) << ODI_SW_CF_A1_CACT_SHIFT);
	l->act[2] = (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A2_CSVID_ACT_SHIFT) |
		    (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A2_CSPRI_ACT_SHIFT) |
		    ((cs_pri & 7U) << ODI_SW_CF_A2_CS_PRI_SHIFT) |
		    ((cs_vid & 0xfffU) << ODI_SW_CF_A2_CS_VID_SHIFT) |
		    (csact & 7U);
}

/* Downstream: forward to pmsk. The 2-bit UNI action straddles words 1
 * and 2 (bits 32..31).
 */
static void cf_act_ds_fwd(struct odi_sw_cf_leg *l, uint32_t pmsk)
{
	l->act[1] |= ODI_SW_CF_DS_UNI_ACT_FWD >> 1;
	l->act[2] |= ((ODI_SW_CF_DS_UNI_ACT_FWD & 1U) << 31) |
		     ((pmsk & 0xfU) << ODI_SW_CF_A2_DS_PMSK_SHIFT);
}

/* Whether a rule forwards tagged frames of any VID unchanged: no VID in its
 * filter, not an untagged-only filter, and both tag actions transparent.
 * omcid builds these for a line with no VLAN filter (the forward-all rule),
 * for class 84 codes that bridge tagged frames without looking at the VID,
 * and for the priority-only filters. No capture of the stock image has one.
 * Such a rule has no VID of its own to put a VLAN row on, so it puts its
 * members on every row: with VLAN filtering on, a tagged frame whose VID
 * row lists neither the UNI nor the PON is dropped at ingress
 * (docs/SWITCH.md, "CF rows"), and the rows the sweep leaves are 0.
 */
static int rule_passes_any_vid(const struct omci_vlan_oper *r)
{
	if ((r->filter.outer_mode | r->filter.inner_mode) & OMCI_TAGF_UNTAGGED)
		return 0;
	if ((r->filter.inner_mode & (OMCI_TAGF_VID | OMCI_TAGF_TCI)) && r->filter.inner.vid < 4096U)
		return 0;
	if (r->out.out_tag.vid < 4096U)
		return 0;
	return r->inner_act.tag_op == OMCI_TAGOP_PASS && r->outer_act.tag_op == OMCI_TAGOP_PASS;
}

/* Both legs of one bridge rule, plus its VLAN row. */
static void bdgconn_derive(const struct omci_bdgconn *b, struct odi_sw_cf_leg *us,
			   struct odi_sw_cf_leg *ds, uint32_t *vlan_vid, uint32_t *vlan_val,
			   uint32_t *vlan_any)
{
	const struct omci_vlan_oper *r = &b->vlan_op;
	const struct omci_vlan_out *o = &r->out;
	uint32_t uni = uni_ports(b->uni_mask);
	int one_uni = -1;
	unsigned int i;

	us->present = 0;
	ds->present = 0;
	*vlan_vid = 0;
	*vlan_val = 0;
	*vlan_any = 0;

	/* The upstream rule names its source port only when uni_mask holds
	 * exactly one UNI (ISP1 1 and 5 alike -> port 0); with none (a VEIP
	 * alone, 4) or several it matches any source port.
	 */
	for (i = 0; i < 4U; i++)
		if (uni == (1U << i))
			one_uni = (int)i;

	if (rule_passes_any_vid(r))
		*vlan_any = ODI_SW_VLAN_ROW((uni ? uni : ODI_SW_BOARD_UNI_PORTS) |
					    (1U << ODI_SW_PON_PORT), 0);

	if (b->dir & OMCI_DIR_US) {
		cf_match_filter(us, &r->filter, 0, one_uni);
		cf_act_common(us, cact_of(r->inner_act.tag_op), act_vid(&r->inner_act.set_tag),
			      act_pri(&r->inner_act.set_tag), csact_of(r->outer_act.tag_op),
			      act_vid(&r->outer_act.set_tag), act_pri(&r->outer_act.set_tag));
		/* Queue on the rule upstream flow. */
		us->act[2] |= ((b->us_flow & 0x7fU) << ODI_SW_CF_A2_US_FLOW_SHIFT) |
			      ODI_SW_CF_A2_US_SID_ACT;
	}

	if ((b->dir & OMCI_DIR_DS) && (b->dir & OMCI_DIR_US)) {
		/* Both ways: the downstream rule matches the tag the upstream
		 * treatment leaves on the PON side (out.out_tag), and
		 * undoes that treatment. ISP1: the add-tag data rule matches
		 * VID 11 priority 0 with a C-tag and no S-tag, and deletes both
		 * tags; the VID filters match their VID (and p-bit when the
		 * out-style carries one) and stay transparent.
		 */
		int vid = o->out_tag.vid < 4096U ? (int)o->out_tag.vid : -1;
		int pri = o->out_tag.pri < 8U ? (int)o->out_tag.pri : -1;
		int stag = -1, ctag = -1;
		uint32_t cact, csact, c_vid = 0, c_pri = 0;

		if (r->filter.outer_mode & OMCI_TAGF_UNTAGGED) {
			stag = 0;
			ctag = o->tag_count >= 1U ? 1 : 0;
		}
		cf_match(ds, 1, vid, pri, stag, ctag, -1);

		switch (r->inner_act.tag_op) {
		case OMCI_TAGOP_PUSH:
			cact = ODI_SW_CF_CACT_DEL;
			break;
		case OMCI_TAGOP_PASS:
			cact = ODI_SW_CF_CACT_TRANSPARENT;
			break;
		case OMCI_TAGOP_POP:
		case OMCI_TAGOP_REWRITE:
			/* not seen: put back the tag the filter matched */
			cact = ODI_SW_CF_CACT_ADD;
			c_vid = act_vid(&r->filter.inner);
			c_pri = act_pri(&r->filter.inner);
			break;
		default:
			cact = ODI_SW_CF_CACT_NONE;
			break;
		}
		if (r->filter.outer_mode & OMCI_TAGF_UNTAGGED)
			csact = ODI_SW_CF_CSACT_DEL;	/* nothing S-tagged reaches this UNI */
		else
			csact = csact_of(r->outer_act.tag_op);
		cf_act_common(ds, cact, c_vid, c_pri, csact, 0, 0);
		cf_act_ds_fwd(ds, uni ? uni : ODI_SW_CHIP_UNI_PORTS);

		if (vid >= 2 && vid <= 4094) {
			uint32_t mbr = uni ? uni : ODI_SW_BOARD_UNI_PORTS;

			*vlan_vid = (uint32_t)vid;
			*vlan_val = ODI_SW_VLAN_ROW(mbr | (1U << ODI_SW_PON_PORT),
						    cact == ODI_SW_CF_CACT_DEL ? mbr : 0);
		}
	} else if (b->dir & OMCI_DIR_DS) {
		/* Downstream only (the multicast rule): the filter is the
		 * downstream match and the treatment applies as stated.
		 * ISP1: VID 0 priority 0 C-tag no S-tag, delete both, and
		 * C_VID carries the assigned VID even though the tag is
		 * deleted. The port mask read 1 with uni_mask 1 and 3 with
		 * uni_mask 5: modelled as "the PON bit adds both chip UNIs".
		 */
		uint32_t pmsk = uni;

		if (b->uni_mask & (1U << ODI_SW_PON_PORT))
			pmsk |= ODI_SW_CHIP_UNI_PORTS;
		if (!pmsk)
			pmsk = ODI_SW_CHIP_UNI_PORTS;
		cf_match_filter(ds, &r->filter, 1, -1);
		cf_act_common(ds, cact_of(r->inner_act.tag_op), act_vid(&r->inner_act.set_tag),
			      act_pri(&r->inner_act.set_tag), csact_of(r->outer_act.tag_op),
			      act_vid(&r->outer_act.set_tag), act_pri(&r->outer_act.set_tag));
		cf_act_ds_fwd(ds, pmsk);
	}
}

/* ---- rule_gen OMCI_VLAN_OPER_EXTVLAN_GEN ---------------------------------
 *
 * The extended VLAN operation of class 171, one direction per descriptor and
 * both halves stated outright (uapi/omci_bdgconn.h). Nothing is inferred
 * here; the descriptor says which frames match and what happens to the tags,
 * and the rest is how the classifier says it.
 *
 *  - The classifier compares one tag's VID and priority per row. Template 0
 *    reads the C-tag (also the one behind an S-tag); template 1, written
 *    when the first row needs it, reads the S-tag. Both tag flags are always
 *    cared about, so a row names exactly which tags the frame has.
 *  - The C-tag action TAG adds a tag to an untagged frame and replaces the
 *    VID (and, when asked, the priority) of one that has it: no CPRI_SET
 *    keeps the priority. DEL takes the tag off, TRANSPARENT leaves it.
 *  - The S-tag action ADD puts an S-tag (TPID 0x88a8) in front of the frame
 *    and replaces the S-tag of one that has it; DEL takes it off.
 *  - A rule with no VID to put a VLAN row on contributes none, and a rule
 *    of this kind never passes "any VID" (that is the class 84 rules' job).
 */
static int gen_cmp(const struct omci_vlan_filter *f, int *vid, int *pri, int *tmpl)
{
	int svid = -1, spri = -1, cvid = -1, cpri = -1;

	if ((f->outer_mode & OMCI_TAGF_VID) && f->outer.vid < 4096U)
		svid = (int)f->outer.vid;
	if ((f->outer_mode & OMCI_TAGF_PRI) && f->outer.pri < 8U)
		spri = (int)f->outer.pri;
	if ((f->inner_mode & OMCI_TAGF_VID) && f->inner.vid < 4096U)
		cvid = (int)f->inner.vid;
	if ((f->inner_mode & OMCI_TAGF_PRI) && f->inner.pri < 8U)
		cpri = (int)f->inner.pri;

	/* One comparison per row: the S-tag's or the C-tag's, not both. */
	if ((svid >= 0 || spri >= 0) && (cvid >= 0 || cpri >= 0))
		return -1;
	if (svid >= 0 || spri >= 0) {
		if (!(f->outer_mode & OMCI_TAGF_TAGGED))
			return -1;
		*vid = svid;
		*pri = spri;
		*tmpl = 1;
	} else {
		if ((cvid >= 0 || cpri >= 0) && !(f->inner_mode & OMCI_TAGF_TAGGED))
			return -1;
		*vid = cvid;
		*pri = cpri;
		*tmpl = 0;
	}
	return 0;
}

/* One tag plane's action word fields. `s` selects the S-tag layout. */
static int gen_plane(const struct omci_vlan_act *a, int s, uint32_t *w)
{
	uint32_t act, vid_act = 0, pri_act = 0;

	switch (a->tag_op) {
	case OMCI_TAGOP_PUSH:
	case OMCI_TAGOP_REWRITE:
		if (a->set_tag.vid > 4094U || a->set_tag.pri > 7U)
			return -1;
		act = s ? ODI_SW_CF_CSACT_ADD : ODI_SW_CF_CACT_ADD;
		vid_act = ODI_SW_CF_TAG_SRC_ASSIGN;
		/* The C-tag keeps its own priority when none is assigned. The
		 * S-tag is not known to, so it is always given one. */
		if (a->pri_op != OMCI_PRIOP_PASS)
			pri_act = ODI_SW_CF_TAG_SRC_ASSIGN;
		else if (s)
			return -1;
		break;
	case OMCI_TAGOP_POP:
		act = s ? ODI_SW_CF_CSACT_DEL : ODI_SW_CF_CACT_DEL;
		break;
	case OMCI_TAGOP_PASS:
		act = s ? ODI_SW_CF_CSACT_TRANSPARENT : ODI_SW_CF_CACT_TRANSPARENT;
		break;
	default:
		act = 0;
		break;
	}
	if (s)
		*w = (vid_act << ODI_SW_CF_A2_CSVID_ACT_SHIFT) |
		     (pri_act << ODI_SW_CF_A2_CSPRI_ACT_SHIFT) |
		     ((vid_act ? a->set_tag.pri & 7U : 0) << ODI_SW_CF_A2_CS_PRI_SHIFT) |
		     ((vid_act ? a->set_tag.vid & 0xfffU : 0) << ODI_SW_CF_A2_CS_VID_SHIFT) |
		     (act & 7U);
	else
		*w = (pri_act << ODI_SW_CF_A1_CPRI_ACT_SHIFT) |
		     (vid_act << ODI_SW_CF_A1_CVID_ACT_SHIFT) |
		     ((pri_act ? a->set_tag.pri & 7U : 0) << ODI_SW_CF_A1_C_PRI_SHIFT) |
		     ((vid_act ? a->set_tag.vid & 0xfffU : 0) << ODI_SW_CF_A1_C_VID_SHIFT) |
		     ((act & 3U) << ODI_SW_CF_A1_CACT_SHIFT);
	return 0;
}

/* Ordering class of an extended VLAN row: the more it names, the lower its
 * index, since the lowest matching row wins. They all sit in the ordered
 * region, whatever they name: a row that names no VID and no priority (the
 * untagged one, a bare "has an S-tag") is not a catch-all that belongs at the
 * bottom, because it still names which tags the frame has.
 */
static unsigned int cf_key_gen(int vid, int pri)
{
	return 2U - (vid >= 0 ? 1U : 0U) - (pri >= 0 ? 1U : 0U);
}

static int bdgconn_derive_gen(const struct omci_bdgconn *b, struct odi_sw_cf_leg *us,
			      struct odi_sw_cf_leg *ds, uint32_t *vlan_vid, uint32_t *vlan_val,
			      uint32_t *vlan_any)
{
	const struct omci_vlan_oper *r = &b->vlan_op;
	struct odi_sw_cf_leg *l;
	uint32_t uni = uni_ports(b->uni_mask), cw, sw, mbr;
	int is_ds, vid, pri, tmpl, stag, ctag, one_uni = -1;
	unsigned int i;

	us->present = 0;
	ds->present = 0;
	*vlan_vid = 0;
	*vlan_val = 0;
	*vlan_any = 0;

	/* One direction per descriptor. */
	if (b->dir != OMCI_DIR_US && b->dir != OMCI_DIR_DS)
		return -1;
	is_ds = b->dir == OMCI_DIR_DS;
	l = is_ds ? ds : us;

	if (gen_cmp(&r->filter, &vid, &pri, &tmpl))
		return -1;
	stag = (r->filter.outer_mode & OMCI_TAGF_TAGGED) ? 1 :
	       (r->filter.outer_mode & OMCI_TAGF_UNTAGGED) ? 0 : -1;
	ctag = (r->filter.inner_mode & OMCI_TAGF_TAGGED) ? 1 :
	       (r->filter.inner_mode & OMCI_TAGF_UNTAGGED) ? 0 : -1;
	if (gen_plane(&r->inner_act, 0, &cw) || gen_plane(&r->outer_act, 1, &sw))
		return -1;

	/* The upstream rule names its source port when uni_mask holds exactly
	 * one UNI, as every other upstream rule does. */
	for (i = 0; i < 4U; i++)
		if (uni == (1U << i))
			one_uni = (int)i;

	cf_match(l, is_ds, vid, pri, stag, ctag, is_ds ? -1 : one_uni);
	l->tmpl = tmpl;
	l->key = cf_key_gen(vid, pri);
	if (r->filter.ethertype != OMCI_ETHTYPE_NO_CARE) {
		/* An EtherType is compared on an untagged frame going up and
		 * nowhere else: the value behind a tag is not known to be in
		 * the same place. A rule that names none while the table
		 * has EtherType rules (REST) goes to the bottom region, after
		 * every one of them. */
		if (is_ds || stag != 0 || ctag != 0 || vid >= 0 || pri >= 0)
			return -1;
		if (r->filter.ethertype == OMCI_ETHTYPE_REST) {
			l->key = ODI_SW_CF_KEY_BOTTOM;
		} else if (r->filter.ethertype & OMCI_ETHTYPE_LITERAL) {
			uint32_t et = r->filter.ethertype & ODI_SW_CF_W0_ET_MASK;

			if (r->filter.ethertype & ~(OMCI_ETHTYPE_LITERAL | ODI_SW_CF_W0_ET_MASK))
				return -1;
			l->et = 1;
			l->rule_w0 = et;
			l->mask_w0 = ~et & ODI_SW_CF_W0_ET_MASK;
		} else {
			return -1;
		}
	}
	l->act[0] = 0;
	l->act[1] = cw;
	l->act[2] = sw;
	if (!is_ds)
		l->act[2] |= ((b->us_flow & 0x7fU) << ODI_SW_CF_A2_US_FLOW_SHIFT) |
			     ODI_SW_CF_A2_US_SID_ACT;
	else
		cf_act_ds_fwd(l, uni ? uni : ODI_SW_CHIP_UNI_PORTS);

	/* The VLAN row of the VID the row matches, tagged on both its ports; a
	 * C-tag taken off on the way to the UNI is untagged there, as the
	 * stock rows do it. */
	if (vid >= 2 && vid <= 4094) {
		mbr = uni ? uni : ODI_SW_BOARD_UNI_PORTS;
		*vlan_vid = (uint32_t)vid;
		*vlan_val = ODI_SW_VLAN_ROW(mbr | (1U << ODI_SW_PON_PORT),
					    is_ds && r->inner_act.tag_op == OMCI_TAGOP_POP ? mbr : 0);
	}
	return 0;
}

/* The rows written by one call, in order, handed to the DAL leaf. */
static struct odi_sw_cf_entry odi_sw_cf_out[2U * ODI_SW_CF_ROWS];
static unsigned int odi_sw_cf_out_n;

static void cf_emit(uint32_t idx, int first)
{
	const struct odi_sw_cf_row *row = &odi_sw_cf[idx];
	struct odi_sw_cf_entry *e;

	if (odi_sw_cf_out_n >= sizeof(odi_sw_cf_out) / sizeof(odi_sw_cf_out[0]))
		return;
	e = &odi_sw_cf_out[odi_sw_cf_out_n++];
	e->idx = idx;
	e->is_us = row->is_us;
	e->tmpl = row->tmpl;
	e->rule_w0 = ODI_SW_CF_W0_VALID | row->rule_w0;
	e->rule_w1 = row->rule_w1;
	/* bit 48 of the mask row: 0 on the first write of an insert, set on
	 * every other write (docs/SWITCH.md#cf-rows).
	 */
	e->mask_w0 = (first ? 0 : ODI_SW_CF_W0_VALID) | row->mask_w0;
	e->mask_w1 = row->mask_w1;
	e->action_w0 = row->act[0];
	e->action_w1 = row->act[1];
	e->action_w2 = row->act[2];
}

static void cf_set(uint32_t idx, const struct odi_sw_cf_leg *l)
{
	struct odi_sw_cf_row *row = &odi_sw_cf[idx];
	unsigned int i;

	row->used = 1;
	row->is_us = l->is_us;
	row->tmpl = l->tmpl;
	row->key = l->key;
	row->et = l->et;
	row->rule_w0 = l->rule_w0;
	row->mask_w0 = l->mask_w0;
	row->rule_w1 = l->rule_w1;
	row->mask_w1 = l->mask_w1;
	for (i = 0; i < 3; i++)
		row->act[i] = l->act[i];
}

/* The slot owning CF row idx gets told it moved to idx + 1. */
static void cf_moved(uint32_t from, uint32_t to)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		if (!odi_sw_bdgconn[i].used)
			continue;
		if (odi_sw_bdgconn[i].us_row == (int)from)
			odi_sw_bdgconn[i].us_row = (int)to;
		else if (odi_sw_bdgconn[i].ds_row == (int)from)
			odi_sw_bdgconn[i].ds_row = (int)to;
	}
}

/* Place a new row, keeping the ordered region ordered: after the last row
 * of the same or a lower class; if that row is taken, every row from
 * there to the next free one moves up by one, the topmost first (ISP1
 * calls 5, 7 and 11 are exactly this). Returns the row, or -1 when full.
 */
static int cf_insert(const struct odi_sw_cf_leg *l)
{
	uint32_t limit = ODI_SW_CF_BOTTOM + 1U, r, p, h;
	int last = -1;

	if (l->key == ODI_SW_CF_KEY_BOTTOM) {
		for (r = ODI_SW_CF_BOTTOM; r >= ODI_SW_CF_SORTED_BASE; r--) {
			if (!odi_sw_cf[r].used) {
				cf_set(r, l);
				cf_emit(r, 1);
				return (int)r;
			}
			if (odi_sw_cf[r].key != ODI_SW_CF_KEY_BOTTOM)
				break;
		}
		return -1;
	}

	/* The ordered region ends where the bottom region starts. */
	for (r = ODI_SW_CF_BOTTOM; r >= ODI_SW_CF_SORTED_BASE; r--) {
		if (odi_sw_cf[r].used && odi_sw_cf[r].key == ODI_SW_CF_KEY_BOTTOM)
			limit = r;
		else if (odi_sw_cf[r].used)
			break;
	}
	/* A row that compares an EtherType is matched with the second rule
	 * layout, which is where the EtherType sits: it takes the first free
	 * row from the boundary up. It names a frame no other row of the
	 * ordered region matches (an untagged frame of that one EtherType; the
	 * rule for the rest of them is in the bottom region), so its place
	 * among them does not matter, and the rows that are placed by order
	 * leave it where it is. */
	if (l->et) {
		uint32_t from = odi_sw_cf_layout1_row();

		for (r = from > ODI_SW_CF_SORTED_BASE ? from : ODI_SW_CF_SORTED_BASE; r < limit; r++) {
			if (!odi_sw_cf[r].used) {
				cf_set(r, l);
				cf_emit(r, 1);
				return (int)r;
			}
		}
		return -1;
	}
	for (r = ODI_SW_CF_SORTED_BASE; r < limit; r++)
		if (odi_sw_cf[r].used && odi_sw_cf[r].key <= l->key && !odi_sw_cf[r].et)
			last = (int)r;
	p = last < 0 ? ODI_SW_CF_SORTED_BASE : (uint32_t)last + 1U;
	for (h = p; h < limit && odi_sw_cf[h].used; h++)
		;
	if (h >= limit)
		return -1;
	/* The S-tag template is not known to work on a row matched with the
	 * second rule layout, so neither the new row nor one pushed up by it
	 * may end on or past the boundary.
	 */
	{
		int tmpl = l->tmpl;
		uint32_t layout1;

		for (r = p; r < h; r++)
			tmpl |= odi_sw_cf[r].tmpl;
		/* Read only when an S-tag row is involved: a line without one
		 * makes no register read it did not make before. */
		if (tmpl) {
			layout1 = odi_sw_cf_layout1_row();
			if (l->tmpl && p >= layout1)
				return -1;
			for (r = p; r < h; r++)
				if (odi_sw_cf[r].tmpl && r + 1U >= layout1)
					return -1;
		}
	}
	for (r = h; r > p; r--) {
		odi_sw_cf[r] = odi_sw_cf[r - 1U];
		cf_moved(r - 1U, r);
		cf_emit(r, r == h);
	}
	cf_set(p, l);
	cf_emit(p, h == p);
	return (int)p;
}

static void cf_remove(int idx)
{
	if (idx < 0 || (uint32_t)idx >= ODI_SW_CF_ROWS || !odi_sw_cf[idx].used)
		return;
	odi_sw_cf_del((uint32_t)idx, odi_sw_cf[idx].is_us);
	odi_sw_cf[idx].used = 0;
}

static int cf_same_match(int idx, const struct odi_sw_cf_leg *l)
{
	if (!l->present)
		return idx < 0;
	if (idx < 0)
		return 0;
	return odi_sw_cf[idx].rule_w1 == l->rule_w1 && odi_sw_cf[idx].mask_w1 == l->mask_w1 &&
	       odi_sw_cf[idx].rule_w0 == l->rule_w0 && odi_sw_cf[idx].mask_w0 == l->mask_w0 &&
	       odi_sw_cf[idx].tmpl == l->tmpl;
}

static int cf_same_row(int idx, const struct odi_sw_cf_leg *l)
{
	return cf_same_match(idx, l) && odi_sw_cf[idx].act[0] == l->act[0] &&
	       odi_sw_cf[idx].act[1] == l->act[1] && odi_sw_cf[idx].act[2] == l->act[2];
}

/* What every row 2..4094 carries: the members of the active services that
 * pass any VID (rule_passes_any_vid()), 0 when there are none.
 */
static uint32_t vlan_default(void)
{
	uint32_t v = 0;
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used)
			v |= odi_sw_bdgconn[i].vlan_any;
	return v;
}

/* The VLAN row of vid as the active services define it together. */
static uint32_t vlan_row_of(uint32_t vid)
{
	uint32_t v = vlan_default();
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].vlan_vid == vid)
			v |= odi_sw_bdgconn[i].vlan_val;
	return v;
}

static struct odi_sw_vlan_override odi_sw_vlan_out[ODI_SW_CMD_BDGCONN_MAX];

static unsigned int vlan_overrides(void)
{
	unsigned int i, k, n = 0;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		uint32_t vid = odi_sw_bdgconn[i].vlan_vid;

		if (!odi_sw_bdgconn[i].used || !vid)
			continue;
		for (k = 0; k < n; k++)
			if (odi_sw_vlan_out[k].idx == vid)
				break;
		if (k < n)
			continue;
		odi_sw_vlan_out[n].idx = vid;
		odi_sw_vlan_out[n].val = vlan_row_of(vid);
		n++;
	}
	return n;
}

/* Word 1 of a downstream row with the priority of its flow: assigned when
 * the flow has one, else the row as it was, which leaves the frame at the
 * port default. */
static uint32_t ds_act1(uint32_t act1, int flow)
{
	act1 &= ~(ODI_SW_CF_A1_PRI_EN | ODI_SW_CF_A1_PRI_MASK);
	if (flow >= 0 && flow < (int)ODI_SW_CMD_GEM_DS_MAX && odi_sw_ds_flow_pri[flow])
		act1 |= ODI_SW_CF_A1_PRI_EN |
			((uint32_t)(odi_sw_ds_flow_pri[flow] - 1U) << ODI_SW_CF_A1_PRI_SHIFT);
	return act1;
}

/* Every downstream row on `flow` (every row when flow is -1) takes the
 * priority the table now says, written only when its action word changed. */
static void ds_rows_refresh(int flow)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		struct odi_sw_bdgconn_slot *sl = &odi_sw_bdgconn[i];
		struct odi_sw_cf_row *row;
		uint32_t a1;

		if (!sl->used || sl->ds_row < 0 || sl->ds_flow < 0 ||
		    (flow >= 0 && sl->ds_flow != flow))
			continue;
		row = &odi_sw_cf[sl->ds_row];
		a1 = ds_act1(row->act[1], sl->ds_flow);
		if (a1 == row->act[1])
			continue;
		row->act[1] = a1;
		odi_sw_cf_act_write((uint32_t)sl->ds_row, 0, row->act);
	}
}

void odi_switch_bdgconn_ds_pri_set(uint32_t flow, int pri)
{
	uint8_t v = pri >= 0 ? (uint8_t)(pri + 1) : 0;

	if (flow >= ODI_SW_CMD_GEM_DS_MAX || odi_sw_ds_flow_pri[flow] == v)
		return;
	odi_sw_ds_flow_pri[flow] = v;
	ds_rows_refresh((int)flow);
}

void odi_switch_bdgconn_ds_pri_clear(void)
{
	unsigned int i;
	int any = 0;

	for (i = 0; i < ODI_SW_CMD_GEM_DS_MAX; i++) {
		any |= odi_sw_ds_flow_pri[i] != 0;
		odi_sw_ds_flow_pri[i] = 0;
	}
	if (any)
		ds_rows_refresh(-1);
}

/* Free a slot: its rows invalidated, its VLAN row recomputed. */
static void bdgconn_release(struct odi_sw_bdgconn_slot *sl)
{
	uint32_t vid = sl->vlan_vid;

	cf_remove(sl->us_row);
	cf_remove(sl->ds_row);
	if (sl->acl_row >= 0 && (unsigned int)sl->acl_row < ODI_SW_ACL_DROP_MAX &&
	    odi_sw_acl[sl->acl_row].used) {
		odi_sw_acl_drop_del(ODI_SW_ACL_DROP_BASE + (unsigned int)sl->acl_row);
		odi_sw_acl[sl->acl_row].used = 0;
	}
	sl->acl_row = -1;
	sl->used = 0;
	if (sl->vlan_any) {
		/* Its members were on every row: take them off all of them. */
		for (vid = 2; vid <= ODI_SW_VLAN_ID_LAST_SWEPT; vid++)
			odi_sw_vlan_row_set(vid, vlan_row_of(vid));
		return;
	}
	if (vid)
		odi_sw_vlan_row_set(vid, vlan_row_of(vid));
}

/* ---- rule_gen OMCI_VLAN_OPER_EXTVLAN_DROP ---------------------------------
 *
 * An entry of the class 171 table that discards the C-tagged frames it names
 * (uapi/omci_bdgconn.h): one ACL row on the UNI ports of the descriptor, no
 * classifier row, no VLAN row. The ACL decides before the classifier, which
 * is why omcid leaves out of a discard the priorities another entry of the
 * same VID names. What the row compares and how it drops is in
 * odi_sw_acl_drop_set().
 */
static int bdgconn_derive_drop(const struct omci_bdgconn *b, uint32_t *ports, int *vid, int *pri)
{
	const struct omci_vlan_filter *f = &b->vlan_op.filter;

	if (b->dir != OMCI_DIR_US)
		return -1;
	*ports = uni_ports(b->uni_mask);
	if (!*ports)
		return -1;
	if (!(f->inner_mode & OMCI_TAGF_TAGGED) || !(f->inner_mode & OMCI_TAGF_VID) ||
	    f->inner.vid < 1U || f->inner.vid > 4094U)
		return -1;
	*vid = (int)f->inner.vid;
	*pri = ((f->inner_mode & OMCI_TAGF_PRI) && f->inner.pri < 8U) ? (int)f->inner.pri : -1;
	return 0;
}

static int bdgconn_activate_drop(const struct omci_bdgconn *b)
{
	struct odi_sw_bdgconn_slot *sl = NULL;
	uint32_t ports;
	int vid, pri, row = -1, rc;
	unsigned int i;

	if (bdgconn_derive_drop(b, &ports, &vid, &pri)) {
		ODI_SW_CMD_LOG("bdgconn: extended VLAN discard not expressible, refused\n");
		return ODI_SW_EOPNOTSUPP;
	}
	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == b->service_id) {
			sl = &odi_sw_bdgconn[i];
			break;
		}
	if (sl) {
		const struct odi_sw_acl_row *a = sl->acl_row >= 0 ? &odi_sw_acl[sl->acl_row] : NULL;

		if (a && a->ports == ports && a->vid == vid && a->pri == pri)
			return 0;
		bdgconn_release(sl);
	}
	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (!odi_sw_bdgconn[i].used) {
			sl = &odi_sw_bdgconn[i];
			break;
		}
	if (!sl)
		return ODI_SW_EOPNOTSUPP;
	for (i = 0; i < ODI_SW_ACL_DROP_MAX; i++)
		if (!odi_sw_acl[i].used) {
			row = (int)i;
			break;
		}
	if (row < 0) {
		ODI_SW_CMD_LOG("bdgconn: no ACL row left for a discard\n");
		return ODI_SW_ENOSPC;
	}
	rc = odi_sw_acl_drop_set(ODI_SW_ACL_DROP_BASE + (unsigned int)row, ports, vid, pri);
	if (rc) {
		ODI_SW_CMD_LOG("bdgconn: the ACL row of a discard did not write\n");
		return rc;
	}
	odi_sw_acl[row].used = 1;
	odi_sw_acl[row].ports = ports;
	odi_sw_acl[row].vid = vid;
	odi_sw_acl[row].pri = pri;
	sl->serv_id = b->service_id;
	sl->dir = b->dir;
	sl->us_row = sl->ds_row = -1;
	sl->acl_row = row;
	sl->ds_flow = -1;
	sl->vlan_vid = sl->vlan_val = sl->vlan_any = 0;
	sl->used = 1;
	return 0;
}

int odi_switch_bdgconn_activate(void *buf, uint32_t len)
{
	struct omci_bdgconn *b = (struct omci_bdgconn *)buf;
	struct odi_sw_bdgconn_slot *sl = NULL;
	struct odi_sw_cf_leg us, ds;
	uint32_t vlan_vid, vlan_val, vlan_any;
	unsigned int i;
	int ds_flow;

	if (!b || len < sizeof(*b))
		return -1;

	if (b->vlan_op.rule_gen == OMCI_VLAN_OPER_EXTVLAN_DROP)
		return bdgconn_activate_drop(b);
	if (b->vlan_op.rule_gen == OMCI_VLAN_OPER_EXTVLAN_GEN) {
		if (bdgconn_derive_gen(b, &us, &ds, &vlan_vid, &vlan_val, &vlan_any)) {
			ODI_SW_CMD_LOG("bdgconn: extended VLAN rule not expressible, refused\n");
			return ODI_SW_EOPNOTSUPP;
		}
	} else {
		bdgconn_derive(b, &us, &ds, &vlan_vid, &vlan_val, &vlan_any);
	}
	odi_sw_cf_out_n = 0;
	/* The flow of the GEM port that carries the downstream leg: its queue
	 * decides the priority the leg's frames take. */
	ds_flow = (ds.present && b->ds_flow < ODI_SW_CMD_GEM_DS_MAX) ? (int)b->ds_flow : -1;
	if (ds.present)
		ds.act[1] = ds_act1(ds.act[1], ds_flow);

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == b->service_id) {
			sl = &odi_sw_bdgconn[i];
			break;
		}
	}

	if (sl && (sl->dir != b->dir || sl->acl_row >= 0 || !cf_same_match(sl->us_row, &us) ||
		   !cf_same_match(sl->ds_row, &ds))) {
		/* The id now names a different rule: replace it. */
		bdgconn_release(sl);
		sl = NULL;
	}

	if (sl) {
		/* Same rule again, another ingress merged in (ISP1: every
		 * service twice, uni_mask 1 then 5). The downstream row is
		 * rewritten in place every time; the upstream row only if it
		 * changed.
		 */
		if (ds.present) {
			cf_set((uint32_t)sl->ds_row, &ds);
			cf_emit((uint32_t)sl->ds_row, 0);
		}
		if (us.present && !cf_same_row(sl->us_row, &us)) {
			cf_set((uint32_t)sl->us_row, &us);
			cf_emit((uint32_t)sl->us_row, 0);
		}
	} else {
		for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
			if (!odi_sw_bdgconn[i].used) {
				sl = &odi_sw_bdgconn[i];
				break;
			}
		}
		if (!sl)
			return ODI_SW_EOPNOTSUPP;
		sl->serv_id = b->service_id;
		sl->dir = b->dir;
		sl->us_row = -1;
		sl->ds_row = -1;
		sl->acl_row = -1;
		sl->ds_flow = -1;
		sl->vlan_vid = 0;
		sl->vlan_val = 0;
		sl->vlan_any = 0;
		sl->used = 1;
		if (us.present)
			sl->us_row = cf_insert(&us);
		if (ds.present)
			sl->ds_row = cf_insert(&ds);
		if ((us.present && sl->us_row < 0) || (ds.present && sl->ds_row < 0)) {
			if (us.present && us.et)
				ODI_SW_CMD_LOG("bdgconn: no free row of the second layout for an EtherType rule\n");
			else
				ODI_SW_CMD_LOG("bdgconn: classification table full\n");
			bdgconn_release(sl);
			return ODI_SW_ENOSPC;
		}
	}
	sl->ds_flow = ds_flow;
	sl->vlan_vid = vlan_vid;
	sl->vlan_val = vlan_val;
	sl->vlan_any = vlan_any;

	odi_sw_cf_add(odi_sw_cf_out, odi_sw_cf_out_n,
				      odi_sw_vlan_out, vlan_overrides(), vlan_default());
	return 0;
}

/* cmd 50 -- deactiveBdgConn, 4 bytes: the service id. Frees the slot,
 * invalidates its CF rows (all-zero rule and action rows; no capture has
 * a delete in it, see odi_sw_cf_del) and rewrites its VLAN
 * row from the services that remain. omcid rebuilds by deactivating every
 * service and activating them again, which refills the freed rows.
 */
int odi_switch_bdgconn_deactivate(void *buf, uint32_t len)
{
	int32_t serv_id;
	unsigned int i;

	if (!buf || len < sizeof(int32_t))
		return -1;
	serv_id = *(const int32_t *)buf;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == serv_id) {
			bdgconn_release(&odi_sw_bdgconn[i]);
			return 0;
		}
	}
	return 0; /* deactivating an unknown/already-gone service_id is not an error */
}
