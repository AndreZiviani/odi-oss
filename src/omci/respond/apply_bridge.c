/* The bridge connections: the service table, the bridge rule of cmd 51
 * (bdgconn_add()), the VLAN rules, and the rebuild that derives every
 * connection from the MIB (bdgconn_rebuild()). See omcid.h.
 */
#include "omcid.h"

int conn_dirty;

/* ------------------------------------------------- the bridge connection
 *
 * Command 51 takes a 160-byte bridge rule descriptor and is what makes the
 * stick forward: with the connections torn down by deactiveBdgConn, upstream
 * traffic stops even with every GEM flow and T-CONT in place. Three
 * behaviours of the stock daemon are reproduced:
 *
 * 1. A rule is not a zeroed struct. Six fields start set, two to non-zero
 *    "ignore" sentinels (OMCI_PRI_ANY 8, OMCI_VID_ANY 4096); a zeroed rule
 *    filters on VID 0 and priority 0, is accepted, and drops everything.
 * 2. Two ingresses that differ in nothing else share one service, the new
 *    ingress ORed into the stored entry: on ISP1 all six services read
 *    UNIMASK=5 while the connection dump prints twelve rules.
 * 3. A refused descriptor gives its service id back, or the table drifts
 *    out of step with the driver on the first failure.
 */
#define SERV_MAX 256                     /* the service-id limit, and the number of
					  * rows omcicli dump srvflow prints */

struct omci_bdgconn servtab[SERV_MAX];


static int serv_avail(void)
{
	for (int i = 0; i < SERV_MAX; i++)
		if (!servtab[i].in_use)
			return i;
	return -1;
}

/* Whether two rules are the same service apart from the ingress: vlan_op,
 * both flow ids and the direction. A merge may differ in uni_mask,
 * service_id, in_use, latch, the two Dp fields, is_mcast and ds_tag_op;
 * the last two live inside vlan_op, so its word compare skips them. */
static int serv_same_except_uni(const struct omci_bdgconn *a,
				const struct omci_bdgconn *b)
{
	const uint32_t *x = (const uint32_t *)&a->vlan_op;
	const uint32_t *y = (const uint32_t *)&b->vlan_op;
	/* Word indices within vlan_op: out starts at +88, so is_mcast is
	 * at +92 and ds_tag_op at +108. */
	enum { W_IS_MC_RULE = 92 / 4, W_DS_TAG_OPER = 108 / 4 };

	if (a->us_flow != b->us_flow || a->ds_flow != b->ds_flow ||
	    a->dir != b->dir)
		return 0;
	for (unsigned i = 0; i < sizeof a->vlan_op / 4; i++) {
		if (i == W_IS_MC_RULE || i == W_DS_TAG_OPER)
			continue;
		if (x[i] != y[i])
			return 0;
	}
	return 1;
}

/* Merge into an existing service if one matches, else
 * store at the id the caller already allocated. Returns the service id. */
static int serv_update(const struct omci_bdgconn *r)
{
	for (int i = 0; i < SERV_MAX; i++)
		if (servtab[i].in_use && serv_same_except_uni(r, &servtab[i])) {
			servtab[i].uni_mask |= r->uni_mask;
			return i;
		}
	if (r->service_id < 0 || r->service_id >= SERV_MAX)
		return -1;
	servtab[r->service_id] = *r;
	return r->service_id;
}

/* The initial state of a new rule: zero plus the six fields that are not
 * (point 1 above). */
static void rule_init(struct omci_bdgconn *r)
{
	for (unsigned i = 0; i < sizeof *r / 4; i++)
		((uint32_t *)r)[i] = 0;
	r->vlan_op.filter.outer_mode = OMCI_TAGF_ANY;
	r->vlan_op.filter.inner_mode = OMCI_TAGF_ANY;
	r->vlan_op.outer_act.tag_op = OMCI_TAGOP_PASS;
	r->vlan_op.inner_act.tag_op = OMCI_TAGOP_PASS;
	r->vlan_op.out.out_tag.pri = OMCI_PRI_ANY;
	r->vlan_op.out.out_tag.vid = OMCI_VID_ANY;
	r->vlan_op.out.is_mcast = 0;
}

/* The Ethernet UNIs a VEIP fronts (the class 298 question, not which port
 * the VEIP is: see ingress_switch_port()): the capability slot of the VEIP
 * when the blob has one (PPTP is type 2, VEIP type 1;
 * docs/kb/rtl9601-omci-device-capabilities.md), else every PPTP slot. The
 * UNI-G rows the stock stack follows carry no values here; the blob gives
 * the same set. */
uint32_t veip_uni_port_mask(uint16_t veipId)
{
	int own = uni_switch_port(veipId, OMCI_UNI_SLOT_VEIP);

	if (own >= 0)
		return 1u << (unsigned)own;
	return all_eth_uni_mask();
}

/* The switch port of an ingress entity: a VEIP is the PON port, an IP
 * host the CPU port, anything else a UNI slot of the capability table.
 * Returns 0xFFFF for "no particular UNI" (the stock all-UNIs-plus-PON
 * sentinel) and -1, fatal for the rule, for an entity in no slot. These
 * are ONU-created, so omci_autonomous[] is checked as well as the store.
 * Measured: UNIMASK=5 on ISP1 is the VEIP PON bit (1<<2) | the UNI (1<<0).
 */
static int ingress_switch_port(int ingress)
{
	uint16_t id = (uint16_t)ingress;

	if (ingress < 0)
		return 0xFFFF;
	if (!caps_ok)
		return -1;       /* without the blob there is no port map, and
				  * guessing one puts a rule on the wrong port */
	if (mib_find(OMCI_ME_VEIP, id) || instance_is_autonomous(OMCI_ME_VEIP, id))
		return (int)caps_u32(OMCI_CAPS_OFF_PONPORT);
	if (mib_find(OMCI_ME_IP_HOST_CONFIG_DATA, id) ||
	    instance_is_autonomous(OMCI_ME_IP_HOST_CONFIG_DATA, id))
		return (int)caps_u32(OMCI_CAPS_OFF_CPUPORT);
	return uni_switch_port(id, OMCI_UNI_SLOT_PPTP);
}

/* Every Ethernet UNI switch port, for the 0xFFFF case, from the capability
 * table. */
uint32_t all_eth_uni_mask(void)
{
	uint32_t m = 0;

	if (!caps_ok)
		return 0;
	for (unsigned i = 0; i < OMCI_CAPS_UNI_SLOTS; i++)
		if (caps[i * 2] == OMCI_UNI_SLOT_PPTP)
			m |= 1u << i;
	return m;
}

/* The bridge rule for one ingress and GEM port, sent as cmd 51. `vr` is the
 * VLAN rule, already generated; the rest is filled in here. Returns the
 * service id, or -1. */
int bdgconn_add(int ingress, uint16_t gemPort, uint32_t dir,
		       const struct omci_vlan_oper *vr)
{
	struct omci_bdgconn r, scratch;
	uint32_t nflows = caps_flows();
	int uniPort, service_id, rc;

	rule_init(&r);
	r.vlan_op = *vr;
	r.dir = dir;

	uniPort = ingress_switch_port(ingress);
	if (uniPort < 0) {
		out_fmt("   [hw] no UNI for ingress %04x gem %d\n",
			(long)ingress, (long)gemPort);
		return -1;
	}

	/* Not found is the GEM port count, the convention of setDsBcGemFlow.
	 * Either flow missing is fatal: a rule naming a flow that does not
	 * exist is worse than no rule. */
	if (dir == OMCI_DIR_US || dir == OMCI_DIR_BI) {
		int id = flow_find(0, gemPort);

		r.us_flow = id < 0 ? nflows : (uint32_t)id;
		/* The stock daemon starts us_dp_flow here; zero would name a
		 * real, different flow and install a second rule. */
		r.us_dp_flow = r.us_flow;
	}
	if (dir == OMCI_DIR_DS || dir == OMCI_DIR_BI) {
		int id = flow_find(1, gemPort);

		r.ds_flow = id < 0 ? nflows : (uint32_t)id;
	}
	if (r.us_flow == nflows || r.ds_flow == nflows) {
		out_fmt("   [hw] no flow id for ingress %04x gem %d\n",
			(long)ingress, (long)gemPort);
		return -1;
	}

	r.uni_mask = (uniPort == 0xFFFF)
		  ? all_eth_uni_mask() | (1u << caps_u32(OMCI_CAPS_OFF_PONPORT))
		  : (1u << (unsigned)uniPort);
	/* A VEIP ingress fronts the Ethernet UNIs, so its mask carries them:
	 * on ISP2 the VEIP is the only ingress and a mask of 4 alone leaves
	 * host frames on port 0 unmatched, although the stock dump reads 4
	 * (docs/kb/rtl9601-omci-bridge-connection.md). */
	if (mib_find(OMCI_ME_VEIP, (uint16_t)ingress) ||
	    instance_is_autonomous(OMCI_ME_VEIP, (uint16_t)ingress))
		r.uni_mask |= veip_uni_port_mask((uint16_t)ingress);

	service_id = serv_avail();
	if (service_id < 0) {
		out("   [hw] service table full\n");
		return -1;
	}
	r.service_id = service_id;
	r.in_use = 1;

	service_id = serv_update(&r);
	if (service_id < 0)
		return -1;

	/* Send the stored entry, not the local copy: a merge put another
	 * ingress in its uni_mask and the local copy does not have it. Through a
	 * scratch buffer, as the stock daemon does. */
	scratch = servtab[service_id];

	if (!apply_hw) {
		/* Dry run: print the 160 bytes instead, to be read and diffed
		 * against a provisioned stick with nothing at risk. */
		const uint8_t *b = (const uint8_t *)&scratch;

		out_fmt("   [dry] serv %d ingress %04x gem %d dir %d\n",
			(long)service_id, (long)ingress, (long)gemPort, (long)dir);
		for (unsigned i = 0; i < OMCI_BDGCONN_LEN; i += 16) {
			out("   ");
			out_hex(i, 3);
			out("  ");
			for (unsigned k = 0; k < 16; k++)
				out_hex(b[i + k], 2);
			out_char('\n');
		}
		return service_id;
	}

	rc = omci_activeBdgConn(&scratch);
	out_fmt("   [hw] bdgconn ingress %04x gem %d dir %d serv %d "
		"us %d ds %d unimask %d -> %d\n",
		(long)ingress, (long)gemPort, (long)dir, (long)service_id,
		(long)servtab[service_id].us_flow, (long)servtab[service_id].ds_flow,
		(long)servtab[service_id].uni_mask, (long)rc);
	if (rc != 0) {
		servtab[service_id].in_use = 0;      /* give the slot back */
		return -1;
	}
	sys_nanosleep(0, 1000 * 1000);            /* 1 ms between sends, as the stock daemon paces them */
	return service_id;
}

/* The no-VLAN-filter rule for one ingress, the N:1 all-pass case only: the
 * initialised rule plus rule_gen = OMCI_VLAN_OPER_FORWARD_ALL. The 802.1p
 * and extended-VLAN variants are not modelled; a guess would be accepted
 * and wrong.
 */
void gen_no_vlan_filter_rule(struct omci_vlan_oper *vr)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FORWARD_ALL;
}

/* The rule a stick in manual VLAN mode runs, every field pinned by the
 * stock `omcicli dump conn` (ISP2 adds VID 10, ISP1 11: `VLAN_MANU_TAG_VID`
 * from the config store; class 171 carries only the 4096 "no VID").
 * Upstream (`isMc` 0): accept untagged, ADD a C-TAG. Downstream multicast
 * (`isMc` 1): match priority/VID zero, remove both tags, emit no tag.
 */
void gen_manual_vlan_rule(struct omci_vlan_oper *vr, int vid, int pri, int isMc)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_EXTVLAN;
	/* Untagged in, both tags. NO_TAG rather than NO_CARE: the stock
	 * dump reads "filter S-TAG Mode: NO TAG". */
	vr->filter.outer_mode = OMCI_TAGF_UNTAGGED;
	vr->filter.inner_mode = isMc ?
		(OMCI_TAGF_VID | OMCI_TAGF_PRI) : OMCI_TAGF_UNTAGGED;
	vr->filter.outer.pri = 0;
	vr->filter.outer.vid = 0;
	vr->filter.outer.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.inner.pri = 0;
	vr->filter.inner.vid = isMc ? 0u : (uint32_t)vid;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->outer_act.tag_op = isMc ? OMCI_TAGOP_POP : OMCI_TAGOP_PASS;
	vr->outer_act.vid_op  = OMCI_VIDOP_SET;
	vr->outer_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.tag_op = isMc ? OMCI_TAGOP_POP : OMCI_TAGOP_PUSH;
	vr->inner_act.vid_op  = OMCI_VIDOP_SET;
	vr->inner_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.set_tag.pri  = (uint32_t)pri;
	vr->inner_act.set_tag.vid  = (uint32_t)vid;
	vr->inner_act.set_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	vr->out.is_default = 0;
	vr->out.is_mcast = isMc ? 1u : 0u;
	vr->out.tag_count = isMc ? 0u : 1u;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.ds_mode = 0;
	vr->out.ds_tag_op = 0;
	if (isMc) {
		vr->out.out_tag.pri  = 0;
		vr->out.out_tag.vid  = 0;
		vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	} else {
		vr->out.out_tag.pri  = (uint32_t)pri;
		vr->out.out_tag.vid  = (uint32_t)vid;
		vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	}
}


/* ------------------------------------------------ connections from the MIB
 *
 * As the stock `omcicli dump conn` shows on ISP1: one connection per
 * (UNI-side bridge port) x (GEM-side bridge port), direction Both, ingress
 * 0x0101 (PPTP Ethernet UNI) and 0x0601 (VEIP), egress the GEM port of the
 * GEM IW TP of the bridge port, and the VLAN rules from the class 84 row on
 * the GEM-side port (bp_rules()): per its forward operation, one filter per
 * list entry, an untagged or a tagged rule, or with no filter entries
 * untagged in and the manual VLAN added (stock service 0: NO TAG, assign
 * VID 11).
 * Multicast (direction 2 CTPs) is left to bc_gem_update. Rebuilt whole --
 * tear down, derive, add -- OLT_QUIET_MS after the last OMCI frame, since
 * the OLT sends these entities in no reliable order. */
static int conn_servs[SERV_MAX];
static int conn_nservs;

static void gen_vid_filter_rule(struct omci_vlan_oper *vr, unsigned vid, int pbit)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FILTER_SINGLETAG;
	vr->filter.outer_mode = OMCI_TAGF_ANY;
	vr->filter.inner_mode = OMCI_TAGF_VID |
		(pbit >= 0 ? OMCI_TAGF_PRI : 0);
	vr->filter.inner.pri = pbit >= 0 ? (uint32_t)pbit : 0;
	vr->filter.inner.vid = vid;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->outer_act.tag_op = OMCI_TAGOP_PASS;
	vr->outer_act.vid_op  = OMCI_VIDOP_SET;
	vr->outer_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.tag_op = OMCI_TAGOP_PASS;
	vr->inner_act.vid_op  = OMCI_VIDOP_SET;
	vr->inner_act.pri_op  = OMCI_PRIOP_SET;
	vr->inner_act.set_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	vr->out.is_default = 0;
	vr->out.is_mcast = 0;
	vr->out.tag_count = 1;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.ds_mode = 0;
	vr->out.ds_tag_op = 0;
	/* The stock dump carries the p-bit in the out tag as well as in the
	 * filter for 802.1P-mapped services (PRI 4 for VID 13, 5 for VID 12),
	 * and the ignore sentinel 8 only for the VID-only ones. */
	vr->out.out_tag.pri  = pbit >= 0 ? (uint32_t)pbit : OMCI_PRI_ANY;
	vr->out.out_tag.vid  = vid;
	vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
}

/* Class 84, VLAN tagging filter data: its forward operation (FwdOp), G.988
 * table 9.3.11-1. Each code is two halves: what happens to a tagged frame
 * (bridged without looking, discarded, or investigated against the VLAN
 * filter list by VID, priority or the whole TCI) and to an untagged one
 * (bridged or discarded). The list holds up to twelve TCIs, the first
 * NumOfEntries valid.
 *
 * What a bridge rule can say: forward everything, forward untagged only,
 * forward tagged only, and a positive filter on VID, priority or both --
 * the (h) action, "admitted if and only if it matches, both ways". It
 * cannot say (g), negative filtering (forward all BUT the listed TCIs), or
 * (j), positive filtering by TCI and destination MAC with no flooding:
 * those codes are built as 0x10 is, and logged once (event=vlan_fwdop).
 * The table repeats itself (0x0f/0x1c are 0x03 again, and so on); the
 * repeats differ only in the letter, not in what reaches a rule here. */
enum { FWD_TAG_ALL, FWD_TAG_NONE, FWD_TAG_VID, FWD_TAG_PRI, FWD_TAG_TCI };

struct fwdop { uint8_t tagged, untagged_fwd, supported; };

#define FWDOP_MANDATORY 0x10

static struct fwdop fwdop_decode(uint32_t code)
{
	struct fwdop f = { FWD_TAG_VID, 0, 1 };

	switch (code) {
	case 0x00: f.tagged = FWD_TAG_ALL;  f.untagged_fwd = 1; break;
	case 0x01: f.tagged = FWD_TAG_NONE; f.untagged_fwd = 1; break;
	case 0x02: case 0x15:
		f.tagged = FWD_TAG_ALL; break;
	case 0x03: case 0x0f: case 0x1c:
		f.untagged_fwd = 1; break;
	case 0x04: case 0x10: case 0x1d:
		break;
	case 0x07: case 0x11: case 0x1e:
		f.tagged = FWD_TAG_PRI; f.untagged_fwd = 1; break;
	case 0x08: case 0x12: case 0x1f:
		f.tagged = FWD_TAG_PRI; break;
	case 0x0b: case 0x13: case 0x20:
		f.tagged = FWD_TAG_TCI; f.untagged_fwd = 1; break;
	case 0x0c: case 0x14: case 0x21:
		f.tagged = FWD_TAG_TCI; break;
	default:                         /* (g), (j), and past the table */
		f.supported = 0;
		break;
	}
	return f;
}

/* Every frame from the UNI into the bridge, untagged only: no tag added,
 * none removed, and the downstream leg matches untagged frames only. */
static void gen_untagged_rule(struct omci_vlan_oper *vr)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FORWARD_UNTAG;
	vr->filter.outer_mode = OMCI_TAGF_UNTAGGED;
	vr->filter.inner_mode = OMCI_TAGF_UNTAGGED;
	vr->filter.outer.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->out.tag_count = 0;
	vr->out.tpid = OMCI_OUT_TPID_8100;
}

/* Tagged frames of any VID and priority, unchanged. */
static void gen_tagged_rule(struct omci_vlan_oper *vr)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FORWARD_SINGLETAG;
	vr->filter.inner_mode = OMCI_TAGF_TAGGED;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->out.tag_count = 1;
	vr->out.tpid = OMCI_OUT_TPID_8100;
}

/* Tagged frames of one priority, any VID, unchanged: the (h) action on the
 * user priority, the 802.1p mapper code 0x12. */
static void gen_pri_filter_rule(struct omci_vlan_oper *vr, unsigned pri)
{
	struct omci_bdgconn tmp;

	rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FILTER_INNER_PRI;
	vr->filter.inner_mode = OMCI_TAGF_TAGGED | OMCI_TAGF_PRI;
	vr->filter.inner.pri = pri;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->out.tag_count = 1;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.out_tag.pri = pri;
}

/* The rules of one GEM-side bridge port: at most one per filter entry, one
 * for untagged frames, one for tagged ones. */
#define BP_RULES_MAX 14

struct bp_rule {
	struct omci_vlan_oper vr;
	const char *kind;
	int vid, pri;                    /* for the log line; -1 none */
};

static int bp_put(struct bp_rule *rules, int n, const char *kind, int vid, int pri)
{
	for (int i = 0; i < n; i++)
		if (rules[i].kind == kind && rules[i].vid == vid && rules[i].pri == pri)
			return n;        /* a repeated list entry: one rule */
	if (n >= BP_RULES_MAX)
		return n;
	rules[n].kind = kind;
	rules[n].vid = vid;
	rules[n].pri = pri;
	return n + 1;
}

/* The manual tag, when on, is the untagged handoff (kb and SETTINGS.md,
 * "manual VLAN"): VLAN_MANU_TAG_VID added to what comes in untagged. The
 * stock stack builds it for the class 84 entry that carries that VID, and
 * with no class 84 at all; so does this, and it stands in for the plain
 * untagged rule of a code that bridges untagged frames. */
/* *manual says whether the list itself names the manual VID: such a port
 * is installed first (the caller orders by it, as the stock stack does). */
static int bp_rules(uint16_t bp, int pbit, int mvid, uint32_t dir,
		    struct bp_rule *rules, int *manual)
{
	const struct omci_class *c84 = find_class(OMCI_ME_VLAN_TAGGING_FILTER_DATA);
	struct mib_row *vt = c84 ? mib_find(OMCI_ME_VLAN_TAGGING_FILTER_DATA, bp) : 0;
	uint32_t nent = vt ? row_u32(vt, c84, 3) : 0, code = vt ? row_u32(vt, c84, 2) : 0;
	struct fwdop f;
	uint8_t tbl[24];
	int n = 0;

	*manual = 0;
	/* Downstream only (multicast) under the manual tag: the tag comes off. */
	if (dir == 2 && mvid >= 0) {
		n = bp_put(rules, n, "manual-remove", mvid, vlanCfg.pri);
		gen_manual_vlan_rule(&rules[0].vr, mvid, vlanCfg.pri, 1);
		return n;
	}
	/* No class 84, or one with an empty list: as before any FwdOp was
	 * read -- the manual tag, else everything forwarded. An investigating
	 * code over an empty list would discard every tagged frame, which no
	 * line has been seen to mean. */
	if (!vt || !nent) {
		if (mvid >= 0) {
			n = bp_put(rules, n, "manual-add", mvid, vlanCfg.pri);
			gen_manual_vlan_rule(&rules[0].vr, mvid, vlanCfg.pri, 0);
		} else {
			n = bp_put(rules, n, "forward-all", -1, -1);
			gen_no_vlan_filter_rule(&rules[0].vr);
		}
		return n;
	}
	f = fwdop_decode(code);
	if (!f.supported) {
		ev_vlan_fwdop(bp, (uint8_t)code, FWDOP_MANDATORY);
		f = fwdop_decode(FWDOP_MANDATORY);
	}
	if (nent > 12)
		nent = 12;
	for (unsigned b = 0; b < sizeof tbl; b++)
		tbl[b] = 0;
	attr_value(c84, bp, 1, tbl);

	if (f.tagged == FWD_TAG_ALL && f.untagged_fwd && mvid < 0) {
		n = bp_put(rules, n, "forward-all", -1, -1);
		gen_no_vlan_filter_rule(&rules[0].vr);
		return n;
	}
	for (unsigned e = 0; e < nent && f.tagged >= FWD_TAG_VID; e++) {
		int vid = (int)(((unsigned)tbl[2 * e] << 8 | tbl[2 * e + 1]) & 0xfff);
		int pri = tbl[2 * e] >> 5;
		int had = n;

		if (f.tagged != FWD_TAG_PRI && vid == mvid) {
			/* The service VID is the untagged handoff. OEM still has
			 * a class-84 row for it, but builds the manual add-tag
			 * rule. */
			if (*manual)
				continue;
			n = bp_put(rules, n, "manual-add", mvid, vlanCfg.pri);
			if (n > had) {
				gen_manual_vlan_rule(&rules[had].vr, mvid, vlanCfg.pri, 0);
				*manual = 1;
			}
		} else if (f.tagged == FWD_TAG_VID) {
			n = bp_put(rules, n, "vid-filter", vid, pbit);
			if (n > had)
				gen_vid_filter_rule(&rules[had].vr, (unsigned)vid, pbit);
		} else if (f.tagged == FWD_TAG_TCI) {
			n = bp_put(rules, n, "tci-filter", vid, pri);
			if (n > had)
				gen_vid_filter_rule(&rules[had].vr, (unsigned)vid, pri);
		} else {
			n = bp_put(rules, n, "pri-filter", -1, pri);
			if (n > had)
				gen_pri_filter_rule(&rules[had].vr, (unsigned)pri);
		}
	}
	if (f.tagged == FWD_TAG_ALL) {
		int had = n;

		n = bp_put(rules, n, "tagged", -1, -1);
		if (n > had)
			gen_tagged_rule(&rules[had].vr);
	}
	if (f.untagged_fwd && !*manual) {
		int had = n;

		if (mvid >= 0) {
			n = bp_put(rules, n, "manual-add", mvid, vlanCfg.pri);
			if (n > had)
				gen_manual_vlan_rule(&rules[had].vr, mvid, vlanCfg.pri, 0);
		} else {
			n = bp_put(rules, n, "untagged", -1, -1);
			if (n > had)
				gen_untagged_rule(&rules[had].vr);
		}
	}
	return n;
}

void bdgconn_rebuild(void)
{
	const struct omci_class *c47 = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	const struct omci_class *c266 = find_class(OMCI_ME_GEM_IW_TP);
	const struct omci_class *c268 = find_class(OMCI_ME_GEM_PORT_CTP);
	const struct omci_class *c281 = find_class(OMCI_ME_MCAST_GEM_IW_TP);
	const struct omci_class *c84 = find_class(OMCI_ME_VLAN_TAGGING_FILTER_DATA);
	const struct omci_class *c130 = find_class(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE);
	int ingress[8], ingress_bridge[8], ning = 0, added = 0, mvid;

	for (int i = 0; i < conn_nservs; i++) {
		if (apply_hw)
			omci_deactiveBdgConn((uint32_t)conn_servs[i]);
		if (conn_servs[i] >= 0 && conn_servs[i] < SERV_MAX)
			servtab[conn_servs[i]].in_use = 0;
	}
	conn_nservs = 0;
	if (!c47 || !c266 || !c268 || !c281 || !c84)
		return;
	for (int i = 0; i < MIB_ROWS && ning < 8; i++) {
		struct mib_row *r = mib_row_at(i);
		uint32_t tp;

		if (!r || r->classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		tp = row_u32(r, c47, 3);
		if ((tp == 1 || tp == 11) && ning < 8) { /* Ethernet UNI, VEIP */
			ingress_bridge[ning] = (int)row_u32(r, c47, 1);
			ingress[ning++] = (int)row_u32(r, c47, 4);
		}
	}
	/* The manual tag, gated on VLAN_CFG_TYPE and VLAN_MANU_MODE as the
	 * stock stack gates it (cfgstore.c); -1 builds no manual tag. */
	mvid = cfg_manual_vid();
	/* Service ids are classifier priority. OEM installs the configured
	 * untagged service first, the other unicast services in reverse MIB order,
	 * and the broad downstream multicast rule last. */
	for (int pass = 0; pass < 3; pass++) {
		for (int i = 0; i < MIB_ROWS; i++) {
			int row = pass == 1 ? MIB_ROWS - 1 - i : i;
			struct mib_row *r = mib_row_at(row), *iw, *ctp, *mapper;
			struct mib_row *tgt_iw[8];
			int tgt_pbit[8], ntgt = 0;
			uint32_t tpType;

			if (!r || r->classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
				continue;
			tpType = row_u32(r, c47, 3);
			if ((pass < 2 && tpType != 3) || (pass == 2 && tpType != 6))
				continue;
			/* TPType 3 ("802.1p mapper", G.988): ISP1 points TPPointer at
			 * the GEM IW TP (mapper via ServProPtr), ISP2 at the mapper. One
			 * connection per distinct IW TP, with its p-bit when exactly
			 * one lands on it, VID-only when several do. */
			if (tpType == 3) {
				uint16_t ptr = (uint16_t)row_u32(r, c47, 4);

				iw = mib_find(OMCI_ME_GEM_IW_TP, ptr);
				if (iw) {
					int pbit = -1;

					mapper = mib_find(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE, (uint16_t)row_u32(iw, c266, 3));
					if (mapper) {
						int matches = 0;

						for (int pri = 0; pri < 8; pri++)
							if (row_u32(mapper, c130, 2 + pri) == iw->inst) {
								pbit = pri;
								matches++;
							}
						if (matches != 1)
							pbit = -1;
					}
					tgt_iw[0] = iw; tgt_pbit[0] = pbit; ntgt = 1;
				} else if ((mapper = mib_find(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE, ptr)) != 0) {
					for (int pri = 0; pri < 8; pri++) {
						uint32_t id = row_u32(mapper, c130, 2 + pri);
						int k;

						if (id == 0 || id == 0xffff)
							continue;
						for (k = 0; k < ntgt; k++)
							if (tgt_iw[k]->inst == id)
								break;
						if (k < ntgt) {
							tgt_pbit[k] = -1;    /* several p-bits: VID only */
							continue;
						}
						iw = mib_find(OMCI_ME_GEM_IW_TP, (uint16_t)id);
						if (!iw || ntgt >= 8)
							continue;
						tgt_iw[ntgt] = iw; tgt_pbit[ntgt] = pri; ntgt++;
					}
					/* All eight on one IW TP is the plain service, not a
					 * p-bit 0 service. */
					if (ntgt == 1)
						tgt_pbit[0] = -1;
				}
			} else {
				iw = mib_find(OMCI_ME_MCAST_GEM_IW_TP, (uint16_t)row_u32(r, c47, 4));
				if (iw) {
					tgt_iw[0] = iw; tgt_pbit[0] = -1; ntgt = 1;
				}
			}
			for (int t = 0; t < ntgt; t++) {
			struct bp_rule rules[BP_RULES_MAX];
			uint32_t port, dir;
			int nrules, manual;

			iw = tgt_iw[t];
			ctp = mib_find(OMCI_ME_GEM_PORT_CTP, (uint16_t)row_u32(iw, tpType == 3 ? c266 : c281, 1));
			if (!ctp)
				continue;
			port = row_u32(ctp, c268, 1);
			dir = row_u32(ctp, c268, 3);
			nrules = bp_rules(r->inst, tgt_pbit[t], mvid, dir, rules, &manual);
			if (tpType == 3 && ((pass == 0 && !manual) ||
					    (pass == 1 && manual)))
				continue;
			for (int k = 0; k < nrules; k++) {
			out_fmt("   [%s] 47/%04x gem %d rule %s vid %d pri %d\n",
				apply_hw ? "hw" : "dry", (long)r->inst, (long)port,
				rules[k].kind, (long)rules[k].vid, (long)rules[k].pri);
			for (int g = 0; g < ning; g++) {
				int id;

				/* Only ingresses of the SAME bridge. Two bridges are
				 * covered only by the ISP2 fixture in QEMU; every stick
				 * seen has one. */
				if (ingress_bridge[g] != (int)row_u32(r, c47, 1))
					continue;
				id = bdgconn_add(ingress[g], (uint16_t)port, dir, &rules[k].vr);
				if (id >= 0 && conn_nservs < SERV_MAX) {
					conn_servs[conn_nservs++] = id;
					added++;
				}
			}
			}                        /* rules of this target */
			}                        /* targets of this bridge port */
		}
	}
	out_fmt("   [%s] bridge connections rebuilt: %d ingress x gem = %d\n",
		apply_hw ? "hw" : "dry", (long)ning, (long)added);
}
