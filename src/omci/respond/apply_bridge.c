/* The bridge connections: the service table, the bridge rule of cmd 51
 * (bdgconn_add()), the VLAN rules, and the rebuild that derives every
 * connection from the MIB (bdgconn_rebuild()). See omcid.h.
 */
#include "omcid.h"
#include "../extvlan.h"
#include "../mcastvlan.h"

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

/* The host lane: the SFP's Ethernet side, toward the router, which is the
 * first Ethernet UNI slot of the capability table. -1 without one. */
static int host_lane_port(void)
{
	uint32_t m = all_eth_uni_mask();

	for (int port = 0; port < 32; port++)
		if (m & (1u << port))
			return port;
	return -1;
}

/* Whether IP host `id` is the voice one: a SIP agent (class 150) names, by
 * its TCP/UDP pointer (attribute 5), a TCP/UDP config data (136) whose IP
 * host pointer (4) names it -- the path the SIP signalling takes (G.988
 * 9.9.3, 9.4.3). A pointer decides, not the class: an OLT builds IP hosts for
 * other things too, TR-069 management among them, and those are not the
 * router's business. With OMCI_POTS_PORTS at 0 no SIP agent is ever held. */
int iphost_is_voice(uint16_t id)
{
	const struct omci_class *c150 = find_class(OMCI_ME_SIP_AGENT_CFG_DATA);
	const struct omci_class *c136 = find_class(OMCI_ME_TCP_UDP_CFG_DATA);

	if (!c150 || !c136)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *r = mib_row_at(i), *tu;

		if (!r || r->classId != OMCI_ME_SIP_AGENT_CFG_DATA)
			continue;
		tu = mib_find(OMCI_ME_TCP_UDP_CFG_DATA, (uint16_t)row_u32(r, c150, 5));
		if (tu && row_u32(tu, c136, 4) == id)
			return 1;
	}
	return 0;
}

/* The switch port the bridge port of IP host `id` stands for. The stock
 * choice is the CPU port, and that is what it stays for every IP host but
 * the voice one, and for that one too while OMCI_POTS_PORTS is 0. With phone
 * ports on, the voice IP host's SIP client is the router: the host lane.
 * Never the CPU port toward the fibre either way -- the PON isolation cuts
 * it, and no bridge rule is built for any other IP host at all. */
int iphost_switch_port(uint16_t id)
{
	if (!caps_ok)
		return -1;
	if (pots_ports && iphost_is_voice(id))
		return host_lane_port();
	return (int)caps_u32(OMCI_CAPS_OFF_CPUPORT);
}

/* The switch port of an ingress entity: a VEIP is the PON port, an IP
 * host what iphost_switch_port() says, anything else a UNI slot of the
 * capability table.
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
		return iphost_switch_port(id);
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

/* The priority of the tag the manual rules add: VLAN_MANU_TAG_PRI, or the
 * one the OLT's class 171 names (olt_tag()). Set by bdgconn_rebuild(). */
static int tag_pri;

/* Whether that tag came from the OLT's class 171 rather than the manual
 * settings. Set by bdgconn_rebuild(). */
static int tag_from_olt;

/* The tag the OLT's class 171 provisions on the UNI, when the stick is not in
 * manual mode (VLAN_CFG_TYPE other than 1, as the stock firmware gates it):
 * the one shape evtocd_olt_tag() accepts, in the instance associated with the
 * Ethernet UNI (AssociationType 2). Our ISPs run type 1, so neither reaches
 * this (ISP1's UNI instance adds VID 1, which is not its service VID 11).
 * Returns the VID with *pri set, or -1 for none; any other shape stays
 * transparent and is logged once, which is what an operator reports.
 * Called before the rules are built, so a change of class 171 (Set, Create,
 * Delete) is picked up by the rebuild it sets conn_dirty for. */
#define EXTVLAN_ASSOC_ETH_UNI 2
#define EXTVLAN_ENTRIES_MAX   12

static const char *olt_logged;
static int olt_logged_vid = -1, olt_logged_pri = -1;

/* Say once, and again only when it changes, why the OLT's class 171 was not
 * applied. NULL forgets what was said, so a reason that comes back after the
 * table was applied is said again. */
static void olt_note(const char *why)
{
	if (!why) {
		olt_logged = 0;
		return;
	}
	if (why != olt_logged) {
		out_fmt("   ext vlan: OLT rule not applied (%s), transparent\n", why);
		olt_logged = why;
		olt_logged_vid = -1;
		olt_logged_pri = -1;
	}
}

static int olt_tag(int *pri, const char **why_out)
{
	const struct omci_class *c171 = find_class(OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA);
	const char *why = 0;
	int vid = -1;

	*why_out = 0;
	if (!c171 || !cfg_olt_tagging())
		return -1;
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *r = mib_row_at(i);
		uint8_t tbl[EXTVLAN_ENTRIES_MAX * EVTOCD_ENTRY_LEN];
		int n = 0, p = 0, v = 0;

		if (!r || r->classId != OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA ||
		    row_u32(r, c171, 1) != EXTVLAN_ASSOC_ETH_UNI)
			continue;
		for (uint8_t t = r->tbl_head; t != MIB_TBL_NONE &&
		     n < EXTVLAN_ENTRIES_MAX; t = tblpool[t].next, n++)
			for (int b = 0; b < EVTOCD_ENTRY_LEN; b++)
				tbl[n * EVTOCD_ENTRY_LEN + b] = tblpool[t].data[b];
		why = evtocd_olt_tag(tbl, n, (uint16_t)row_u32(r, c171, 4), &v, &p);
		if (!why) {
			vid = v;
			*pri = p;
			if (olt_logged || olt_logged_vid != v || olt_logged_pri != p)
				out_fmt("   ext vlan 171/%04x: untagged frames get VLAN %d pri %d from the OLT\n",
					(long)r->inst, (long)v, (long)p);
			olt_logged = 0;
			olt_logged_vid = v;
			olt_logged_pri = p;
			return vid;
		}
		break;
	}
	*why_out = why;
	return -1;
}

/* The rules of one GEM-side bridge port: at most one per filter entry, one
 * for untagged frames, one for tagged ones. */
#define BP_RULES_MAX 14

struct bp_rule {
	struct omci_vlan_oper vr;
	const char *kind;
	int vid, pri;                    /* for the log line; -1 none */
};

/* What an ingress whose class 171 table is applied (ev_build()) leaves of the
 * class 84 rules. The table's own entries stand in for the frames they handle;
 * what is left is whatever its default rules do not discard. */
struct ev_ctl {
	int skip_untagged;       /* an entry handles untagged frames */
	int skip_tagged;         /* the single-tag default rule discards */
};

/* The two kinds that name the VID a frame carries, the only class 84 rules a
 * bridge port on the host lane gets for an IP host (lane_rule_ok()). */
static const char KIND_VID_FILTER[] = "vid-filter";
static const char KIND_TCI_FILTER[] = "tci-filter";

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

/* Untagged frames passed unchanged and nothing else: all that is left of a
 * "forward everything" port once the class 171 default rule discards tagged
 * frames. None at all when an entry of the table handles the untagged ones. */
static void ev_untagged_only(struct bp_rule *rules, int *n, const struct ev_ctl *ev)
{
	if (ev->skip_untagged)
		return;
	*n = bp_put(rules, *n, "untagged", -1, -1);
	gen_untagged_rule(&rules[*n - 1].vr);
}

/* The manual tag, when on, is the untagged handoff (kb and SETTINGS.md,
 * "manual VLAN"): VLAN_MANU_TAG_VID added to what comes in untagged. The
 * stock stack builds it for the class 84 entry that carries that VID, and
 * with no class 84 at all; so does this, and it stands in for the plain
 * untagged rule of a code that bridges untagged frames. */
/* *manual says whether the list itself names the manual VID: such a port
 * is installed first (the caller orders by it, as the stock stack does). */
static int bp_rules(uint16_t bp, int pbit, int mvid, uint32_t dir,
		    struct bp_rule *rules, int *manual, const struct ev_ctl *ev)
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
		n = bp_put(rules, n, "manual-remove", mvid, tag_pri);
		gen_manual_vlan_rule(&rules[0].vr, mvid, tag_pri, 1);
		return n;
	}
	/* No class 84, or one with an empty list: as before any FwdOp was
	 * read -- the manual tag, else everything forwarded. An investigating
	 * code over an empty list would discard every tagged frame, which no
	 * line has been seen to mean. */
	if (!vt || !nent) {
		if (mvid >= 0) {
			n = bp_put(rules, n, "manual-add", mvid, tag_pri);
			gen_manual_vlan_rule(&rules[0].vr, mvid, tag_pri, 0);
		} else if (ev && ev->skip_tagged) {
			ev_untagged_only(rules, &n, ev);
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
		if (ev && ev->skip_tagged) {
			ev_untagged_only(rules, &n, ev);
		} else {
			n = bp_put(rules, n, "forward-all", -1, -1);
			gen_no_vlan_filter_rule(&rules[0].vr);
		}
		return n;
	}
	if (ev && ev->skip_tagged)
		f.tagged = FWD_TAG_NONE;
	for (unsigned e = 0; e < nent && f.tagged >= FWD_TAG_VID; e++) {
		int vid = (int)(((unsigned)tbl[2 * e] << 8 | tbl[2 * e + 1]) & 0xfff);
		int pri = tbl[2 * e] >> 5;
		int had = n;

		if (f.tagged != FWD_TAG_PRI && vid == mvid && !*manual) {
			/* The service VID is the untagged handoff. OEM still has
			 * a class-84 row for it, but builds the manual add-tag
			 * rule. */
			n = bp_put(rules, n, "manual-add", mvid, tag_pri);
			if (n > had) {
				gen_manual_vlan_rule(&rules[had].vr, mvid, tag_pri, 0);
				*manual = 1;
			}
			had = n;
		}
		/* A manual tag replaces the entry. A tag from class 171 does not:
		 * that table adds a tag to untagged frames only, and G.988 leaves
		 * frames that arrive tagged on its default rule (forward as they
		 * are), so the entry still passes them. The add-tag rule is
		 * installed first and so still takes the downstream frames. */
		if (f.tagged != FWD_TAG_PRI && vid == mvid && !tag_from_olt)
			continue;
		if (f.tagged == FWD_TAG_VID) {
			n = bp_put(rules, n, KIND_VID_FILTER, vid, pbit);
			if (n > had)
				gen_vid_filter_rule(&rules[had].vr, (unsigned)vid, pbit);
		} else if (f.tagged == FWD_TAG_TCI) {
			n = bp_put(rules, n, KIND_TCI_FILTER, vid, pri);
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
	if (f.untagged_fwd && !*manual && !(ev && ev->skip_untagged)) {
		int had = n;

		if (mvid >= 0) {
			n = bp_put(rules, n, "manual-add", mvid, tag_pri);
			if (n > had)
				gen_manual_vlan_rule(&rules[had].vr, mvid, tag_pri, 0);
		} else {
			n = bp_put(rules, n, "untagged", -1, -1);
			if (n > had)
				gen_untagged_rule(&rules[had].vr);
		}
	}
	return n;
}

/* ------------------------------------------------------ class 171, in full
 *
 * With the manual tag off and the one shape olt_tag() accepts not present,
 * the OLT's extended VLAN tagging operation is applied entry by entry
 * (extvlan.c, evtocd_plan()): one bridge rule upstream per entry, and the
 * downstream rule that undoes it when the entry's DsMode has one. The table
 * is applied whole or not at all -- one entry the classifier cannot carry out
 * leaves every frame as it comes, and says which entry and why, once. A line
 * on the manual tag never gets here, so none of this touches the shapes our
 * two lines run.
 *
 * What the class 84 filter of the GEM-side port still says is kept: an entry
 * whose result is a VID the list does not admit is not installed on that port
 * (the ONU would discard it), and the pass-through rules the list gives for
 * the VIDs it names stand after the entries, unless the table's single-tag
 * default rule discards, in which case they are not made.
 *
 * Which GEM port an entry goes up is the priority of the tag it leaves the
 * frame with, through the 802.1p mapper; an entry that leaves it following
 * the frame's own, on a mapper that splits the priorities over several GEM
 * ports, becomes one rule per priority. Frames left untagged go to the port
 * of priority 0. */
#define EV_RULES_MAX   24
#define EV_DROPS_MAX   8
/* Each rule is a bridge rule per EtherType (PPPoE has two) and per GEM port
 * its priorities spread over; a discard is one rule a priority it covers. */
#define EV_OUT_MAX     (EV_RULES_MAX * 8 + EV_DROPS_MAX * 8)

struct ev_rule {
	struct evtocd_plan u, d;
	uint8_t dsmode;
	uint8_t entry;           /* index in the table, for the log */
	uint16_t inst;
};

/* An entry that discards what it names, and the priorities of its VID the
 * ACL may discard (the rest belong to entries that name them). */
struct ev_drop {
	struct evtocd_drop d;
	unsigned pmask;
	uint8_t entry;
	uint16_t inst;
};

struct ev_ing {
	int active, nrules, ndrops;
	struct ev_ctl ctl;
	struct ev_rule rules[EV_RULES_MAX];
	struct ev_drop drops[EV_DROPS_MAX];
};

static struct ev_ing ev_tab[8];

/* The first entry that was not a plan, of the table being read. */
static const char *ev_bad_why;
static int ev_bad_entry;
static uint16_t ev_bad_inst;

static void ev_bad(const char *why, int entry, uint16_t inst)
{
	if (ev_bad_why)
		return;
	ev_bad_why = why;
	ev_bad_entry = entry;
	ev_bad_inst = inst;
}

/* One class 171 instance's entries into `ev`. Returns the rules it added. */
static int ev_read(const struct omci_class *c171, struct mib_row *r, struct ev_ing *ev)
{
	uint16_t in_tpid = (uint16_t)row_u32(r, c171, 3), out_tpid = (uint16_t)row_u32(r, c171, 4);
	uint8_t dsmode = (uint8_t)row_u32(r, c171, 5);
	int n = 0, added = 0;
	uint8_t tbl[EXTVLAN_ENTRIES_MAX * EVTOCD_ENTRY_LEN];

	if (!in_tpid)
		in_tpid = EVTOCD_TPID_C;
	if (!out_tpid)
		out_tpid = EVTOCD_TPID_C;
	for (uint8_t t = r->tbl_head; t != MIB_TBL_NONE && n < EXTVLAN_ENTRIES_MAX;
	     t = tblpool[t].next, n++)
		for (int b = 0; b < EVTOCD_ENTRY_LEN; b++)
			tbl[n * EVTOCD_ENTRY_LEN + b] = tblpool[t].data[b];

	for (int i = 0; i < n; i++) {
		struct evtocd_entry e;
		struct evtocd_action a;
		const char *why;
		struct ev_rule *er;
		int dt;

		evtocd_decode(tbl + i * EVTOCD_ENTRY_LEN, &e);
		dt = evtocd_default_rule_tags(&e);
		if (dt >= 0) {
			/* A default rule discards or passes; one that adds or
			 * removes a tag is not a shape the classifier has. */
			evtocd_action(&e, in_tpid, out_tpid, &a);
			if (a.discard) {
				if (dt == 1)
					ev->ctl.skip_tagged = 1;
			} else if (!a.transparent) {
				ev_bad("a default rule adds or removes tags", i, r->inst);
			}
			continue;
		}
		if (e.remove_tags == EVTOCD_T_DISCARD_FRAME) {
			struct ev_drop *dr = &ev->drops[ev->ndrops];

			if (ev->ndrops >= EV_DROPS_MAX) {
				ev_bad("more discards than omcid keeps", i, r->inst);
				continue;
			}
			why = evtocd_drop(&e, in_tpid, &dr->d);
			if (why) {
				ev_bad(why, i, r->inst);
				continue;
			}
			dr->pmask = evtocd_drop_all(&dr->d);
			dr->entry = (uint8_t)i;
			dr->inst = r->inst;
			ev->ndrops++;
			added++;
			continue;
		}
		if (ev->nrules >= EV_RULES_MAX) {
			ev_bad("more rules than omcid keeps", i, r->inst);
			continue;
		}
		er = &ev->rules[ev->nrules];
		why = evtocd_plan(&e, in_tpid, out_tpid, &er->u);
		if (why) {
			ev_bad(why, i, r->inst);
			continue;
		}
		er->dsmode = dsmode;
		er->entry = (uint8_t)i;
		er->inst = r->inst;
		/* DsMode 5 to 8 say what else the OLT wants discarded; the
		 * classifier has no drop action here, so it is forwarded. */
		if (dsmode >= EVTOCD_DSMODE_VID_PRI_DROP && dsmode <= EVTOCD_DSMODE_DROP_ALL)
			out_fmt("   ext vlan 171/%04x entry %d: downstream mode %d discards frames, which is not done\n",
				(long)r->inst, (long)i, (long)dsmode);
		if (!er->u.s.in && !er->u.c.in)
			ev->ctl.skip_untagged = 1;
		ev->nrules++;
		added++;
	}
	return added;
}

/* Whether a class 171 instance is the one for this ingress: its pointer
 * names the UNI or VEIP (association type 2 or 10) or the UNI-side bridge
 * port (0). `legacy` is the fallback of olt_tag(): any instance on a PPTP
 * Ethernet UNI, whatever it points at. */
static int ev_match(const struct omci_class *c171, const struct mib_row *r,
		    uint16_t ingress, uint16_t bport, int legacy)
{
	uint32_t type = row_u32(r, c171, 1), point = row_u32(r, c171, 7);

	if (legacy)
		return type == EXTVLAN_ASSOC_ETH_UNI;
	if (type == EXTVLAN_ASSOC_ETH_UNI || type == 10)
		return point == ingress;
	if (type == 0)
		return point == bport;
	return 0;
}

/* Read the table for every ingress into ev_tab. Returns how many ingresses
 * have one that is applied; 0, with ev_bad_why set, when an entry was not
 * a plan. An IP host's (`iphost`) is not read here: lane_ev_build(). */
static int ev_build(const int *ingress, const int *bport, const int *iphost, int ning)
{
	const struct omci_class *c171 = find_class(OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA);
	int active = 0, anygood = 0;

	ev_bad_why = 0;
	for (int g = 0; g < 8; g++)
		ev_tab[g].active = 0;
	if (!c171 || !cfg_olt_tagging())
		return 0;
	for (int g = 0; g < ning; g++) {
		struct ev_ing *ev = &ev_tab[g];
		int got = 0;

		if (iphost[g])
			continue;
		ev->nrules = ev->ndrops = 0;
		ev->ctl.skip_untagged = ev->ctl.skip_tagged = 0;
		for (int pass = 0; pass < 2 && !got; pass++) {
			ev->nrules = ev->ndrops = 0;
			ev->ctl.skip_untagged = ev->ctl.skip_tagged = 0;
			for (int i = 0; i < MIB_ROWS; i++) {
				struct mib_row *r = mib_row_at(i);

				if (!r || r->classId != OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA ||
				    !ev_match(c171, r, (uint16_t)ingress[g], (uint16_t)bport[g], pass))
					continue;
				got += ev_read(c171, r, ev);
			}
		}
		/* A discard is an ACL rule, which decides before the classifier:
		 * the priorities of its VID that another entry names are left
		 * out of it, and an overlap nothing orders is not a table to
		 * apply. */
		for (int k = 0; k < ev->ndrops; k++) {
			struct ev_drop *dr = &ev->drops[k];

			for (int i = 0; i < ev->nrules; i++) {
				const char *why = evtocd_drop_shadow(&dr->d, &ev->rules[i].u, &dr->pmask);

				if (why) {
					ev_bad(why, dr->entry, dr->inst);
					break;
				}
			}
		}
		ev->active = got > 0;
		active += ev->active;
		anygood += got;
	}
	/* Not one entry was a plan: that is not a table to apply, and the
	 * reason olt_tag() found is the one to give. */
	if (ev_bad_why && !anygood)
		ev_bad_why = 0;
	if (ev_bad_why) {
		for (int g = 0; g < 8; g++)
			ev_tab[g].active = 0;
		return 0;
	}
	return active;
}

/* Say what is wrong with the table, once per reason. */
static const char *ev_said_why;
static int ev_said_entry = -1;
static uint16_t ev_said_inst;

static void ev_note_bad(void)
{
	if (ev_bad_why == ev_said_why && ev_bad_entry == ev_said_entry &&
	    ev_bad_inst == ev_said_inst)
		return;
	out_fmt("   ext vlan 171/%04x entry %d: OLT rule not applied (%s), transparent\n",
		(long)ev_bad_inst, (long)ev_bad_entry, ev_bad_why);
	ev_said_why = ev_bad_why;
	ev_said_entry = ev_bad_entry;
	ev_said_inst = ev_bad_inst;
}

/* ---- an IP host's table, on the host lane
 *
 * With OMCI_POTS_PORTS on, the bridge port of the voice IP host is the host
 * lane (iphost_switch_port()), and the class 171 instance that names the IP host
 * (association type 3) or its bridge port (0) says what its frames carry,
 * the voice VLAN. It is read whatever VLAN_CFG_TYPE says: the manual tag is
 * the untagged handoff of the data service, which the voice is not. Each
 * entry is made into what the lane can carry (evtocd_plan_lane()): the router
 * sends the voice tagged, as the OLT sets it, and nothing that comes in
 * untagged is taken. The table's default rules and discards are about the IP
 * host's other frames, which on the lane are other services', so they are
 * not applied there. One entry that cannot go on the lane leaves the table
 * out, said once, and the IP host gets the class 84 rules that name a VID
 * (lane_rule_ok()). */
#define EXTVLAN_ASSOC_IP_HOST 3

static const char *lane_said_why;
static int lane_said_entry = -1;
static uint16_t lane_said_inst;

static int plane_same(const struct evtocd_plane *a, const struct evtocd_plane *b)
{
	return a->in == b->in && a->out == b->out && a->op == b->op &&
	       a->f_vid == b->f_vid && a->f_pri == b->f_pri &&
	       a->vid == b->vid && a->pri == b->pri;
}

static int lane_ev_build(struct ev_ing *ev, uint16_t iphost, uint16_t bport)
{
	const struct omci_class *c171 = find_class(OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA);
	int n = 0;

	ev->active = ev->nrules = ev->ndrops = 0;
	ev->ctl.skip_untagged = ev->ctl.skip_tagged = 0;
	ev_bad_why = 0;
	if (!c171)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *r = mib_row_at(i);
		uint32_t type, point;

		if (!r || r->classId != OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA)
			continue;
		type = row_u32(r, c171, 1);
		point = row_u32(r, c171, 7);
		if ((type == EXTVLAN_ASSOC_IP_HOST && point == iphost) ||
		    (type == 0 && point == bport))
			ev_read(c171, r, ev);
	}
	ev->ndrops = 0;
	ev->ctl.skip_untagged = ev->ctl.skip_tagged = 0;
	/* Made into lane rules in place; entries that become the same rule
	 * (IPoE and ARP of one VLAN, say) are one. */
	for (int i = 0; i < ev->nrules && !ev_bad_why; i++) {
		struct ev_rule *er = &ev->rules[i];
		const char *why = evtocd_plan_lane(&er->u, &er->dsmode);
		int k;

		if (why) {
			ev_bad(why, er->entry, er->inst);
			break;
		}
		for (k = 0; k < n; k++)
			if (plane_same(&ev->rules[k].u.c, &er->u.c) &&
			    plane_same(&ev->rules[k].u.s, &er->u.s) &&
			    ev->rules[k].u.et_n == er->u.et_n &&
			    ev->rules[k].dsmode == er->dsmode)
				break;
		if (k == n)
			ev->rules[n++] = *er;
	}
	if (ev_bad_why) {
		if (ev_bad_why != lane_said_why || ev_bad_entry != lane_said_entry ||
		    ev_bad_inst != lane_said_inst)
			out_fmt("   ext vlan 171/%04x entry %d: IP host %04x rule not applied on the host lane (%s)\n",
				(long)ev_bad_inst, (long)ev_bad_entry, (long)iphost, ev_bad_why);
		lane_said_why = ev_bad_why;
		lane_said_entry = ev_bad_entry;
		lane_said_inst = ev_bad_inst;
		ev_bad_why = 0;
		ev->nrules = 0;
		return 0;
	}
	ev->nrules = n;
	ev->active = n > 0;
	return ev->active;
}

/* Whether a class 84 rule may go on the host lane for an IP host: one that
 * names the VID, which the router sends tagged. Any other would take frames
 * of every service the lane carries -- untagged, or tagged with any VID. */
static int lane_rule_ok(const char *kind)
{
	return kind == KIND_VID_FILTER || kind == KIND_TCI_FILTER;
}

static void lane_refused(uint16_t bp, uint32_t gem, const char *kind, int iphost)
{
	out_fmt("   [%s] 47/%04x gem %d rule %s: not for IP host %04x, on the host lane it takes every service's frames\n",
		apply_hw ? "hw" : "dry", (long)bp, (long)gem, kind, (long)iphost);
}

/* ---- what one GEM-side bridge port gets of it */

struct ev_out {
	struct omci_vlan_oper us, ds;
	int has_ds;
	int vid, pri;            /* the match, for the log */
	uint16_t inst;
	uint8_t entry;
	const char *kind;
};
static struct ev_out ev_outs[EV_OUT_MAX];

/* Whether the class 84 filter of GEM-side port `bp` lets a frame whose
 * outermost tag is (vid, pri), or which has none (vid < 0 and tags 0)
 * through. No filter, an empty list, or a code that bridges everything:
 * everything. */
static int ev_admits(uint16_t bp, int tags, int vid, int pri)
{
	const struct omci_class *c84 = find_class(OMCI_ME_VLAN_TAGGING_FILTER_DATA);
	struct mib_row *vt = c84 ? mib_find(OMCI_ME_VLAN_TAGGING_FILTER_DATA, bp) : 0;
	uint32_t nent, code;
	struct fwdop f;
	uint8_t tbl[24];

	if (!vt)
		return 1;
	nent = row_u32(vt, c84, 3);
	code = row_u32(vt, c84, 2);
	if (!nent)
		return 1;
	f = fwdop_decode(code);
	if (!f.supported)
		f = fwdop_decode(FWDOP_MANDATORY);
	if (!tags)
		return f.untagged_fwd;
	if (f.tagged == FWD_TAG_ALL)
		return 1;
	if (f.tagged == FWD_TAG_NONE)
		return 0;
	if (vid < 0 && pri < 0)
		return 1;             /* the frame's own VID is not known here */
	if (nent > 12)
		nent = 12;
	for (unsigned b = 0; b < sizeof tbl; b++)
		tbl[b] = 0;
	attr_value(c84, bp, 1, tbl);
	for (unsigned e = 0; e < nent; e++) {
		int lv = (int)(((unsigned)tbl[2 * e] << 8 | tbl[2 * e + 1]) & 0xfff);
		int lp = tbl[2 * e] >> 5;

		if (f.tagged == FWD_TAG_VID && (vid < 0 || lv == vid))
			return 1;
		if (f.tagged == FWD_TAG_PRI && (pri < 0 || lp == pri))
			return 1;
		if (f.tagged == FWD_TAG_TCI && (vid < 0 || lv == vid) && (pri < 0 || lp == pri))
			return 1;
	}
	return 0;
}

/* The plane of the outermost tag the frame leaves with, or NULL. */
static struct evtocd_plane *ev_outer(struct evtocd_plan *u)
{
	if (u->s.out)
		return &u->s;
	if (u->c.out)
		return &u->c;
	return 0;
}

/* The match a plan names, for the log. */
static void ev_match_of(const struct evtocd_plan *u, int *vid, int *pri)
{
	const struct evtocd_plane *q = u->cmp == EVTOCD_CMP_S ? &u->s : &u->c;

	*vid = q->f_vid;
	*pri = q->f_pri;
}

/* `rest`: the rule is for untagged frames and the table has rules that name
 * an EtherType, which must come first. An entry that names one is a rule for
 * each EtherType (PPPoE is two); the downstream rule undoes the tag by its
 * VID, so it is sent once, with the first. */
static int ev_add_out(int n, const struct ev_rule *er, const struct evtocd_plan *u, int rest)
{
	struct evtocd_plan d;
	const char *dwhy = 0;
	int nrules = u->et_n ? u->et_n : 1;

	for (int k = 0; k < nrules; k++) {
		struct ev_out *o;

		if (n >= EV_OUT_MAX)
			return n;
		o = &ev_outs[n];
		evtocd_plan_oper(u, &o->us);
		if (u->et_n)
			evtocd_oper_ethertype(&o->us, u->et[k]);
		else if (rest)
			evtocd_oper_rest(&o->us);
		o->has_ds = k == 0 && evtocd_plan_ds(u, er->dsmode, &d, &dwhy);
		if (o->has_ds)
			evtocd_plan_oper(&d, &o->ds);
		ev_match_of(u, &o->vid, &o->pri);
		o->inst = er->inst;
		o->entry = er->entry;
		o->kind = u->et_n ? "ext-vlan ethertype" : "ext-vlan";
		n++;
	}
	return n;
}

static int ev_add_drop(int n, const struct ev_drop *dr, int pri)
{
	struct ev_out *o;

	if (n >= EV_OUT_MAX)
		return n;
	o = &ev_outs[n];
	evtocd_drop_oper(&dr->d, pri, &o->us);
	o->has_ds = 0;
	o->vid = dr->d.vid;
	o->pri = pri;
	o->inst = dr->inst;
	o->entry = dr->entry;
	o->kind = "ext-vlan discard";
	return n + 1;
}

/* The rules of ingress `g` for one GEM-side port, whose 802.1p mapper sends
 * the priorities in `pmask` (bit n: priority n) to this GEM port. */
static int ev_for_port(const struct ev_ing *ev, uint16_t bp, unsigned pmask)
{
	int n = 0, has_et = 0;

	for (int i = 0; i < ev->nrules; i++)
		has_et |= ev->rules[i].u.et_n != 0;
	for (int i = 0; i < ev->nrules; i++) {
		const struct ev_rule *er = &ev->rules[i];
		struct evtocd_plan u = er->u;
		struct evtocd_plane *q = ev_outer(&u);
		int rest = has_et && !u.et_n && !u.s.in && !u.c.in;

		if (!u.out_tags) {
			/* Left untagged: it goes where the default priority 0
			 * goes, and is admitted by the untagged half of FwdOp. */
			if ((pmask & 1u) && ev_admits(bp, 0, -1, -1))
				n = ev_add_out(n, er, &u, rest);
			continue;
		}
		if (u.out_pri >= 0) {
			if ((pmask >> u.out_pri & 1u) && ev_admits(bp, u.out_tags, u.out_vid, u.out_pri))
				n = ev_add_out(n, er, &u, rest);
			continue;
		}
		/* Its priority is the frame's own. One GEM port for every
		 * priority needs no split; otherwise a rule for each priority
		 * this port carries, which needs the filter to be able to name
		 * it. */
		if (pmask == 0xffu) {
			if (ev_admits(bp, u.out_tags, u.out_vid, -1))
				n = ev_add_out(n, er, &u, rest);
			continue;
		}
		{
			const struct evtocd_plane *oth = q == &u.s ? &u.c : &u.s;

			if (!q || (oth->in && (oth->f_vid >= 0 || oth->f_pri >= 0))) {
				if (pmask & 1u)
					out_fmt("   ext vlan 171/%04x entry %d: not applied, its priorities are split over GEM ports and the rule names another tag\n",
						(long)er->inst, (long)er->entry);
				continue;
			}
		}
		for (int pr = 0; pr < 8; pr++) {
			struct evtocd_plan v = er->u;
			struct evtocd_plane *w = ev_outer(&v);

			if (!(pmask >> pr & 1u) || !ev_admits(bp, v.out_tags, v.out_vid, pr))
				continue;
			w->f_pri = (int8_t)pr;
			v.out_pri = (int8_t)pr;
			v.cmp = w == &v.s ? EVTOCD_CMP_S : EVTOCD_CMP_C;
			n = ev_add_out(n, er, &v, 0);
		}
	}
	/* The discards are no GEM port's: they are sent once, with the port
	 * that carries priority 0. */
	if (pmask & 1u)
		for (int k = 0; k < ev->ndrops; k++) {
			const struct ev_drop *dr = &ev->drops[k];
			unsigned all = evtocd_drop_all(&dr->d);

			if (dr->pmask == all && dr->d.pri < 0) {
				n = ev_add_drop(n, dr, -1);
				continue;
			}
			for (int pr = 0; pr < 8; pr++)
				if (dr->pmask >> pr & 1u)
					n = ev_add_drop(n, dr, pr);
		}
	return n;
}

/* ------------------------------------------- the multicast tag control
 *
 * Class 309 attribute 16 (../mcastvlan.h) as rules of the multicast bridge
 * ports, in front of the rules the port has from the VLAN keys or class 84
 * (the manual tag's removal, or forward-all). The switch tells multicast
 * from unicast by the VLAN alone, so the treatment goes on the VLANs the
 * profile's access control lists name, and a VLAN a unicast rule of the line
 * names is left to that rule: uc_vids is every VID the unicast rules of this
 * rebuild named, which is why the plan is made when the multicast ports'
 * pass begins. */
static uint8_t uc_vids[MCV_VIDMAP_BYTES];
static struct mcv_out mcv, mcv_said;
static int mcv_said_valid;

/* Whether what the attribute came to differs from what was last logged. */
static int mcv_changed(void)
{
	if (!mcv_said_valid || mcv.type != mcv_said.type || mcv.why != mcv_said.why ||
	    mcv.nrule != mcv_said.nrule || mcv.nskip != mcv_said.nskip)
		return 1;
	for (int i = 0; i < mcv.nrule; i++)
		if (mcv.rule[i].vid != mcv_said.rule[i].vid)
			return 1;
	for (int i = 0; i < mcv.nskip; i++)
		if (mcv.skip[i].vid != mcv_said.skip[i].vid ||
		    mcv.skip[i].why != mcv_said.skip[i].why)
			return 1;
	return 0;
}

static void mcast_tag_plan(void)
{
	uint16_t ani[32];
	struct mcv_in in;

	in.force_strip = (custom_mcast & CUSTOM_MCAST_DS_UNTAGGED) != 0;
	in.ani = ani;
	in.unicast = uc_vids;
	if (mcast_ds_tag_input(in.tci, ani, 32, &in.nani)) {
		mcv_plan(&in, &mcv);
	} else {
		mcv.type = MCV_PASS;     /* no profile: nothing to apply */
		mcv.why = 0;
		mcv.nrule = 0;
		mcv.nskip = 0;
	}
	/* Said when it changes, not on every rebuild: a line whose profile
	 * asks for what cannot be done says so once. */
	if (!mcv_changed())
		return;
	mcv_said = mcv;
	mcv_said_valid = 1;
	if (mcv.why)
		out_fmt("   mcast: downstream tag control %d (309 attribute 16) not "
			"applied: %s\n", (long)mcv.type, mcv.why);
	for (int i = 0; i < mcv.nskip; i++)
		out_fmt("   mcast: downstream tag control %d, VLAN %d left to the "
			"rules of the line: %s\n", (long)mcv.type, (long)mcv.skip[i].vid,
			mcv.skip[i].why);
}

void bdgconn_rebuild(void)
{
	const struct omci_class *c47 = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	const struct omci_class *c266 = find_class(OMCI_ME_GEM_IW_TP);
	const struct omci_class *c268 = find_class(OMCI_ME_GEM_PORT_CTP);
	const struct omci_class *c281 = find_class(OMCI_ME_MCAST_GEM_IW_TP);
	const struct omci_class *c84 = find_class(OMCI_ME_VLAN_TAGGING_FILTER_DATA);
	const struct omci_class *c130 = find_class(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE);
	int ingress[8], ingress_bridge[8], ingress_bport[8], ingress_iphost[8];
	int ning = 0, added = 0, mvid, nev = 0;

	for (int i = 0; i < conn_nservs; i++) {
		if (apply_hw)
			omci_deactiveBdgConn((uint32_t)conn_servs[i]);
		if (conn_servs[i] >= 0 && conn_servs[i] < SERV_MAX)
			servtab[conn_servs[i]].in_use = 0;
	}
	conn_nservs = 0;
	for (unsigned i = 0; i < sizeof uc_vids; i++)
		uc_vids[i] = 0;
	if (!c47 || !c266 || !c268 || !c281 || !c84)
		return;
	for (int i = 0; i < MIB_ROWS && ning < 8; i++) {
		struct mib_row *r = mib_row_at(i);
		uint32_t tp;

		if (!r || r->classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		tp = row_u32(r, c47, 3);
		/* Ethernet UNI, VEIP; and with phone ports on, the voice IP host,
		 * whose bridge port is the host lane (iphost_switch_port()). Any
		 * other IP host's bridge port, and every one with the key off,
		 * gets no rule, as in the stock stack, where it is the CPU port
		 * and the PON isolation cuts it anyway. */
		if ((tp == 1 || tp == 11 ||
		     (tp == MBPCD_TP_IP_HOST && pots_ports &&
		      iphost_is_voice((uint16_t)row_u32(r, c47, 4)))) &&
		    ning < 8) {
			ingress_bridge[ning] = (int)row_u32(r, c47, 1);
			ingress_bport[ning] = r->inst;
			ingress_iphost[ning] = tp == MBPCD_TP_IP_HOST;
			ingress[ning++] = (int)row_u32(r, c47, 4);
		}
	}
	/* The manual tag, gated on VLAN_CFG_TYPE and VLAN_MANU_MODE as the
	 * stock stack gates it (cfgstore.c); -1 builds no manual tag. */
	mvid = cfg_manual_vid();
	tag_pri = vlanCfg.pri;
	tag_from_olt = 0;
	for (int g = 0; g < 8; g++)
		ev_tab[g].active = 0;
	if (mvid < 0) {
		const char *why;

		mvid = olt_tag(&tag_pri, &why);
		tag_from_olt = mvid >= 0;
		if (mvid < 0) {
			/* The one shape was not there; the whole table, entry by
			 * entry, may still be. */
			nev = ev_build(ingress, ingress_bport, ingress_iphost, ning);
			if (nev)
				olt_note(0);
			else if (ev_bad_why)
				ev_note_bad();
			else if (why)
				olt_note(why);
		}
	}
	for (int g = 0; g < ning; g++)
		if (ingress_iphost[g])
			lane_ev_build(&ev_tab[g], (uint16_t)ingress[g], (uint16_t)ingress_bport[g]);
	/* Service ids are classifier priority. OEM installs the configured
	 * untagged service first, the other unicast services in reverse MIB order,
	 * and the broad downstream multicast rule last. */
	for (int pass = 0; pass < 3; pass++) {
		if (pass == 2)
			mcast_tag_plan();
		for (int i = 0; i < MIB_ROWS; i++) {
			int row = pass == 1 ? MIB_ROWS - 1 - i : i;
			struct mib_row *r = mib_row_at(row), *iw, *ctp, *mapper;
			struct mib_row *tgt_iw[8];
			int tgt_pbit[8], ntgt = 0;
			unsigned tgt_pmask[8];   /* the priorities each target carries */
			uint32_t tpType;

			if (!r || r->classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
				continue;
			tpType = row_u32(r, c47, 3);
			if ((pass < 2 && tpType != 3 && tpType != MBPCD_TP_GEM_IWTP) ||
			    (pass == 2 && tpType != 6))
				continue;
			/* TPType 3 ("802.1p mapper", G.988): ISP1 points TPPointer at
			 * the GEM IW TP (mapper via ServProPtr), ISP2 at the mapper. One
			 * connection per distinct IW TP, with its p-bit when exactly
			 * one lands on it, VID-only when several do. */
			/* TPType 5 ("GEM interworking TP", G.988): the port names the
			 * GEM IW TP itself, with no mapper, so one connection with no
			 * p-bit. Seen on a line in 2026-10: skipping it left the stick
			 * with no service at all (provision_end services=0). An IW TP
			 * with interworking option 6 is the downstream broadcast GEM,
			 * which apply_qos.c programs as the broadcast flow instead. */
			if (tpType == MBPCD_TP_GEM_IWTP) {
				iw = mib_find(OMCI_ME_GEM_IW_TP, (uint16_t)row_u32(r, c47, 4));
				if (iw && row_u32(iw, c266, 2) != 6) {
					tgt_iw[0] = iw; tgt_pbit[0] = -1; tgt_pmask[0] = 0xffu; ntgt = 1;
				}
			} else if (tpType == 3) {
				uint16_t ptr = (uint16_t)row_u32(r, c47, 4);

				iw = mib_find(OMCI_ME_GEM_IW_TP, ptr);
				if (iw) {
					int pbit = -1;
					unsigned pmask = 0xffu;

					mapper = mib_find(OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE, (uint16_t)row_u32(iw, c266, 3));
					if (mapper) {
						int matches = 0;

						pmask = 0;
						for (int pri = 0; pri < 8; pri++)
							if (row_u32(mapper, c130, 2 + pri) == iw->inst) {
								pbit = pri;
								matches++;
								pmask |= 1u << pri;
							}
						if (matches != 1)
							pbit = -1;
					}
					tgt_iw[0] = iw; tgt_pbit[0] = pbit; tgt_pmask[0] = pmask; ntgt = 1;
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
							tgt_pmask[k] |= 1u << pri;
							continue;
						}
						iw = mib_find(OMCI_ME_GEM_IW_TP, (uint16_t)id);
						if (!iw || ntgt >= 8)
							continue;
						tgt_iw[ntgt] = iw; tgt_pbit[ntgt] = pri; tgt_pmask[ntgt] = 1u << pri; ntgt++;
					}
					/* All eight on one IW TP is the plain service, not a
					 * p-bit 0 service. */
					if (ntgt == 1) {
						tgt_pbit[0] = -1;
						tgt_pmask[0] = 0xffu;
					}
				}
			} else {
				iw = mib_find(OMCI_ME_MCAST_GEM_IW_TP, (uint16_t)row_u32(r, c47, 4));
				if (iw) {
					tgt_iw[0] = iw; tgt_pbit[0] = -1; tgt_pmask[0] = 0xffu; ntgt = 1;
				}
			}
			for (int t = 0; t < ntgt; t++) {
			struct bp_rule rules[BP_RULES_MAX];
			uint32_t port, dir;
			int nrules, manual, use_ev, plain = 0;

			iw = tgt_iw[t];
			ctp = mib_find(OMCI_ME_GEM_PORT_CTP, (uint16_t)row_u32(iw, tpType == 6 ? c281 : c266, 1));
			if (!ctp)
				continue;
			port = row_u32(ctp, c268, 1);
			dir = row_u32(ctp, c268, 3);
			nrules = bp_rules(r->inst, tgt_pbit[t], mvid, dir, rules, &manual, 0);
			if (tpType != 6 && ((pass == 0 && !manual) ||
					    (pass == 1 && manual)))
				continue;
			/* An ingress with its class 171 table applied gets the
			 * table's rules and what is left of the class 84 ones
			 * (ev_for_port()); the others the rules above. */
			use_ev = 0;
			if (tpType != 6 && (dir & OMCI_DIR_US))
				for (int g = 0; g < ning; g++)
					if (ev_tab[g].active &&
					    ingress_bridge[g] == (int)row_u32(r, c47, 1))
						use_ev++;
			for (int g = 0; g < ning; g++)
				if (ingress_bridge[g] == (int)row_u32(r, c47, 1) &&
				    !(use_ev && ev_tab[g].active))
					plain++;
			/* A multicast port: the profile's tag control first, on
			 * the VLANs it names; the port's own rules below take
			 * every other frame. Downstream only, as the port is. */
			for (int k = 0; tpType == 6 && k < mcv.nrule; k++) {
				out_fmt("   [%s] 47/%04x gem %d rule %s vid %d pri %d\n",
					apply_hw ? "hw" : "dry", (long)r->inst, (long)port,
					mcv.rule[k].kind, (long)mcv.rule[k].vid, -1L);
				for (int g = 0; g < ning; g++) {
					int id;

					if (ingress_bridge[g] != (int)row_u32(r, c47, 1))
						continue;
					id = bdgconn_add(ingress[g], (uint16_t)port, OMCI_DIR_DS,
							 &mcv.rule[k].vr);
					if (id >= 0 && conn_nservs < SERV_MAX) {
						conn_servs[conn_nservs++] = id;
						added++;
					}
				}
			}
			for (int k = 0; k < nrules && (!use_ev || plain); k++) {
			out_fmt("   [%s] 47/%04x gem %d rule %s vid %d pri %d\n",
				apply_hw ? "hw" : "dry", (long)r->inst, (long)port,
				rules[k].kind, (long)rules[k].vid, (long)rules[k].pri);
			if (tpType != 6)
				mcv_mark(uc_vids, &rules[k].vr);
			for (int g = 0; g < ning; g++) {
				int id;

				/* Only ingresses of the SAME bridge. Two bridges are
				 * covered only by the ISP2 fixture in QEMU; every stick
				 * seen has one. */
				if (ingress_bridge[g] != (int)row_u32(r, c47, 1))
					continue;
				if (use_ev && ev_tab[g].active)
					continue;
				if (ingress_iphost[g] && !lane_rule_ok(rules[k].kind)) {
					lane_refused(r->inst, port, rules[k].kind, ingress[g]);
					continue;
				}
				id = bdgconn_add(ingress[g], (uint16_t)port, dir, &rules[k].vr);
				if (id >= 0 && conn_nservs < SERV_MAX) {
					conn_servs[conn_nservs++] = id;
					added++;
				}
			}
			}                        /* rules of this target */
			for (int g = 0; use_ev && g < ning; g++) {
				struct ev_ing *ev = &ev_tab[g];
				struct bp_rule rest[BP_RULES_MAX];
				int nout, nrest, mf;

				if (!ev->active || ingress_bridge[g] != (int)row_u32(r, c47, 1))
					continue;
				nout = ev_for_port(ev, r->inst, tgt_pmask[t]);
				for (int k = 0; k < nout; k++) {
					const struct ev_out *o = &ev_outs[k];
					int id;

					out_fmt("   [%s] 47/%04x gem %d rule %s vid %d pri %d\n",
						apply_hw ? "hw" : "dry", (long)r->inst, (long)port,
						o->kind, (long)o->vid, (long)o->pri);
					mcv_mark(uc_vids, &o->us);
					if (o->has_ds)
						mcv_mark(uc_vids, &o->ds);
					id = bdgconn_add(ingress[g], (uint16_t)port, OMCI_DIR_US, &o->us);
					if (id >= 0 && conn_nservs < SERV_MAX) {
						conn_servs[conn_nservs++] = id;
						added++;
					}
					if (o->has_ds && (dir & OMCI_DIR_DS)) {
						id = bdgconn_add(ingress[g], (uint16_t)port, OMCI_DIR_DS, &o->ds);
						if (id >= 0 && conn_nservs < SERV_MAX) {
							conn_servs[conn_nservs++] = id;
							added++;
						}
					}
				}
				nrest = bp_rules(r->inst, tgt_pbit[t], mvid, dir, rest, &mf, &ev->ctl);
				for (int k = 0; k < nrest; k++) {
					int id;

					if (ingress_iphost[g] && !lane_rule_ok(rest[k].kind)) {
						lane_refused(r->inst, port, rest[k].kind, ingress[g]);
						continue;
					}
					out_fmt("   [%s] 47/%04x gem %d rule %s vid %d pri %d\n",
						apply_hw ? "hw" : "dry", (long)r->inst, (long)port,
						rest[k].kind, (long)rest[k].vid, (long)rest[k].pri);
					mcv_mark(uc_vids, &rest[k].vr);
					id = bdgconn_add(ingress[g], (uint16_t)port, dir, &rest[k].vr);
					if (id >= 0 && conn_nservs < SERV_MAX) {
						conn_servs[conn_nservs++] = id;
						added++;
					}
				}
			}
			}                        /* targets of this bridge port */
		}
	}
	out_fmt("   [%s] bridge connections rebuilt: %d ingress x gem = %d\n",
		apply_hw ? "hw" : "dry", (long)ning, (long)added);
}
