// SPDX-License-Identifier: GPL-2.0
/*
 * The switch half of IGMP snooping: the ACL rule that copies (or takes) the
 * host's IGMP to the CPU, the drop of unregistered IPv4 multicast from the
 * PON port, the bookkeeping that lets them be taken back, and the narrow way
 * a taken report goes back up. odi_switch_mcast.h says what each does and
 * why.
 *
 * The rule's words follow the ACL layout decoded from another open
 * implementation's measurements (a rule is two entries of one table, the
 * bits that must be 0 and the bits that must be 1; the valid bit is in the
 * second; the action is a third entry with its own enable word). The one
 * rule the stock replay writes splits its template number the same way
 * (template 3 as ones 0b011, zeros 0b100) and keeps the ports it does not
 * name in the zeros plane. A rule acts on frames of its own port and no
 * other, measured by that implementation on both the UNI and the PON port.
 *
 * Entry layout, 160 bits in five words, word 0 first (bits 159:128):
 *
 *   bits 2:0    template number
 *   bits 15:10  ingress ports 0..5
 *   bits 16k+31 .. 16k+16   compare slot k, k = 0..7
 *   bit  144    valid (ones plane only)
 *
 * so slot 0 is the high half of word 4 and slot 1 the low half of word 3.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "odi_switch_mcast.h"
#include "odi_switch_dal.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_tbl.h"
#include "odi_igmp_tx.h"

#define ODI_SW_MC_EPERM		(-1)
#define ODI_SW_MC_EINVAL	(-22)
#define ODI_SW_MC_ENOSPC	(-28)

volatile uint32_t odi_sw_mcast_copy_on;
volatile uint32_t odi_sw_mcast_trap_on;
void (*odi_sw_mcast_rx_enable)(void);
int (*odi_sw_mcast_tx)(const uint8_t *frame, uint32_t len, uint32_t flow);

/* What the copy rule changed, so it can be put back. */
static uint32_t held;
static uint32_t saved_tmpl[2];
static uint32_t saved_ip4_pon;
static uint32_t saved_cfg_rsv4;
static uint8_t tracked[ODI_SW_MC_TRACK_MAX][6];
static uint32_t ntracked;

/* ---- the ACL rule ------------------------------------------------------ */

/* Slot value 0 is "compares nothing". Template 6 holds the EtherType in
 * slot 0 and the IPv4 protocol in slot 1; slots 2..7 are cleared so a
 * stray code left in the table cannot make the rule compare something
 * else.
 */
#define MC_TMPL_FIRST_SLOT	(8U * ODI_SW_MC_ACL_TEMPLATE)
#define MC_TMPL_WORD(n)		(ODI_SW_ACL_TMPL_FIELD_OFF + 4U * ((n) / 4U))
#define MC_TMPL_SHIFT(n)	(7U * ((n) % 4U))
#define MC_TMPL_SLOT_MASK(n)	(0x7fU << MC_TMPL_SHIFT(n))

static void mc_template_save_and_set(void)
{
	uint32_t w0 = MC_TMPL_WORD(MC_TMPL_FIRST_SLOT);
	uint32_t w1 = MC_TMPL_WORD(MC_TMPL_FIRST_SLOT + 4U);
	uint32_t v0, v1;

	saved_tmpl[0] = odi_reg_read(w0);
	saved_tmpl[1] = odi_reg_read(w1);
	/* Slots 0..3 live in the first word, 4..7 in the second; bits 31:28
	 * of a word are not slots and stay as they were. */
	v0 = (saved_tmpl[0] & ~0x0fffffffU) |
	     (ODI_SW_ACL_FIELD_ETHERTYPE << MC_TMPL_SHIFT(MC_TMPL_FIRST_SLOT)) |
	     (ODI_SW_ACL_FIELD_IPV4_PROTO << MC_TMPL_SHIFT(MC_TMPL_FIRST_SLOT + 1U));
	v1 = saved_tmpl[1] & ~0x0fffffffU;
	odi_reg_write(w0, v0);
	odi_reg_write(w1, v1);
}

static void mc_template_restore(void)
{
	odi_reg_write(MC_TMPL_WORD(MC_TMPL_FIRST_SLOT), saved_tmpl[0]);
	odi_reg_write(MC_TMPL_WORD(MC_TMPL_FIRST_SLOT + 4U), saved_tmpl[1]);
}

/* The two planes of the rule. Ingress port: the UNI (0) alone, so the
 * zeros plane holds ports 1..5. EtherType 0x0800, all 16 bits; IPv4
 * protocol 2 in the low octet of slot 1 (the high octet, the TOS, is not
 * compared). Care and value give the planes: ones = care & value, zeros =
 * care & ~value.
 */
#define MC_PORT_SHIFT		10U
#define MC_PORTS_ALL		0x3fU
#define MC_TMPL_BITS		7U
#define MC_ETHERTYPE_IPV4	0x0800U
#define MC_PROTO_IGMP		2U
#define MC_PROTO_CARE		0x00ffU

static void mc_rule_planes(uint32_t ones[ODI_SW_MC_RULE_WORDS],
			   uint32_t zeros[ODI_SW_MC_RULE_WORDS])
{
	memset(ones, 0, ODI_SW_MC_RULE_WORDS * sizeof(uint32_t));
	memset(zeros, 0, ODI_SW_MC_RULE_WORDS * sizeof(uint32_t));
	ones[0] = ODI_SW_MC_RULE_VALID;
	ones[4] = (MC_ETHERTYPE_IPV4 << 16) | (1U << MC_PORT_SHIFT) |
		  ODI_SW_MC_ACL_TEMPLATE;
	zeros[4] = ((~MC_ETHERTYPE_IPV4 & 0xffffU) << 16) |
		   ((MC_PORTS_ALL & ~1U) << MC_PORT_SHIFT) |
		   (MC_TMPL_BITS & ~ODI_SW_MC_ACL_TEMPLATE);
	ones[3] = MC_PROTO_IGMP;
	zeros[3] = MC_PROTO_CARE & ~MC_PROTO_IGMP;
}

/* The forwarding decision is bits 42:41 of the 96-bit action entry
 * (word 1 bits 10:9), 2 meaning "mirror", and its port mask bits 40:37
 * (word 1 bits 8:5): the CPU port, 3, alone. 3 is "trap", with no port:
 * the frame goes to the CPU and nowhere else. Both were measured on a stick
 * with diag acl-probe on the host's IGMP (docs/SWITCH.md, "Measured on a
 * stick"): a mirror reached the CPU and still left by the PON, a trap
 * reached the CPU by the rule's reason and nothing left by the PON.
 */
#define MC_ACT_FWD_MIRROR	2U
#define MC_ACT_FWD_TRAP		3U
#define MC_ACT_CPU_PORT		3U

static void mc_action(int trap, uint32_t act[ODI_SW_MC_ACTION_WORDS])
{
	act[0] = 0;
	act[1] = trap ? MC_ACT_FWD_TRAP << 9 :
		 (MC_ACT_FWD_MIRROR << 9) | ((1U << MC_ACT_CPU_PORT) << 5);
	act[2] = 0;
}

static int mc_acl_install(int trap)
{
	static const uint32_t zero[ODI_SW_MC_RULE_WORDS] = { 0 };
	uint32_t ones[ODI_SW_MC_RULE_WORDS], zeros[ODI_SW_MC_RULE_WORDS];
	uint32_t act[ODI_SW_MC_ACTION_WORDS];
	uint32_t permit, en;
	int rc;

	mc_rule_planes(ones, zeros);
	mc_action(trap, act);

	/* A permit for a frame no rule matches must be in place before the
	 * lookup is on for the port: without it a port with lookup on drops
	 * everything. It is on already (the stock init writes 0xf); this
	 * only makes sure of it and never takes a bit away. */
	permit = odi_reg_read(ODI_SW_ACL_DEFAULT_PERMIT_OFF);
	if (!(permit & 1U))
		odi_reg_write(ODI_SW_ACL_DEFAULT_PERMIT_OFF, permit | 1U);
	en = odi_reg_read(ODI_SW_MC_ACL_PORT_EN_OFF);
	if (!(en & 1U))
		odi_reg_write(ODI_SW_MC_ACL_PORT_EN_OFF, en | 1U);

	mc_template_save_and_set();
	/* The rule cannot match until its valid bit, in the ones plane, is
	 * written last. */
	rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, ODI_SW_MC_ACL_ENTRY,
				    zero, ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN_MASK, ODI_SW_MC_ACL_ENTRY,
					    zeros, ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, ODI_SW_MC_ACL_ENTRY,
					    act, ODI_SW_MC_ACTION_WORDS);
	if (rc) {
		mc_template_restore();
		return rc;
	}
	odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * ODI_SW_MC_ACL_ENTRY,
		      ODI_SW_ACL_ACT_EN_FWD);
	rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, ODI_SW_MC_ACL_ENTRY,
				    ones, ODI_SW_MC_RULE_WORDS);
	if (rc) {
		odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * ODI_SW_MC_ACL_ENTRY,
			      ODI_SW_ACL_ACT_EN_RESET);
		mc_template_restore();
		return rc;
	}
	return 0;
}

/* The rule in place, mirror to take or back: only the action entry is
 * rewritten, so the rule never stops matching on the way. */
static int mc_acl_mode(int trap)
{
	uint32_t act[ODI_SW_MC_ACTION_WORDS];

	mc_action(trap, act);
	return odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, ODI_SW_MC_ACL_ENTRY,
				      act, ODI_SW_MC_ACTION_WORDS);
}

static void mc_acl_remove(void)
{
	static const uint32_t zero[ODI_SW_MC_RULE_WORDS] = { 0 };
	static const uint32_t zact[ODI_SW_MC_ACTION_WORDS] = { 0, 0, 0 };

	/* Valid bit first, so the rule stops matching before anything under
	 * it changes. */
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, ODI_SW_MC_ACL_ENTRY,
				     zero, ODI_SW_MC_RULE_WORDS);
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN_MASK, ODI_SW_MC_ACL_ENTRY,
				     zero, ODI_SW_MC_RULE_WORDS);
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, ODI_SW_MC_ACL_ENTRY,
				     zact, ODI_SW_MC_ACTION_WORDS);
	/* A released entry gets the reset value back, never zero: zero chains
	 * an entry to the one before it. */
	odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * ODI_SW_MC_ACL_ENTRY,
		      ODI_SW_ACL_ACT_EN_RESET);
	mc_template_restore();
}

/* ---- the drop of unregistered IPv4 multicast from the PON -------------- */

#define MC_PON_PORT		2U
#define MC_UNKNOWN_ACT_SHIFT(p)	(2U * (p))
#define MC_UNKNOWN_ACT_MASK(p)	(0x3U << MC_UNKNOWN_ACT_SHIFT(p))
#define MC_UNKNOWN_ACT_DROP	1U
#define MC_CFG_RSV4_SHIFT	3U
#define MC_CFG_RSV4_MASK	(0x3U << MC_CFG_RSV4_SHIFT)
#define MC_CFG_RSV4_FLOOD	1U

static void mc_pon_drop_on(void)
{
	uint32_t ip4 = odi_reg_read(ODI_SW_MC_UNKNOWN_IP4_OFF);
	uint32_t cfg = odi_reg_read(ODI_SW_MC_UNKNOWN_CFG_OFF);

	saved_ip4_pon = (ip4 & MC_UNKNOWN_ACT_MASK(MC_PON_PORT)) >> MC_UNKNOWN_ACT_SHIFT(MC_PON_PORT);
	saved_cfg_rsv4 = (cfg & MC_CFG_RSV4_MASK) >> MC_CFG_RSV4_SHIFT;
	/* The reserved IPv4 groups first: with them not exempt, the OLT's
	 * general queries to 224.0.0.1 would be dropped as unregistered, the
	 * host would never report, and the stream would lapse. */
	odi_reg_write(ODI_SW_MC_UNKNOWN_CFG_OFF,
		      (cfg & ~MC_CFG_RSV4_MASK) | (MC_CFG_RSV4_FLOOD << MC_CFG_RSV4_SHIFT));
	odi_reg_write(ODI_SW_MC_UNKNOWN_IP4_OFF,
		      (ip4 & ~MC_UNKNOWN_ACT_MASK(MC_PON_PORT)) |
		      (MC_UNKNOWN_ACT_DROP << MC_UNKNOWN_ACT_SHIFT(MC_PON_PORT)));
}

static void mc_pon_drop_off(void)
{
	uint32_t ip4 = odi_reg_read(ODI_SW_MC_UNKNOWN_IP4_OFF);
	uint32_t cfg = odi_reg_read(ODI_SW_MC_UNKNOWN_CFG_OFF);

	odi_reg_write(ODI_SW_MC_UNKNOWN_IP4_OFF,
		      (ip4 & ~MC_UNKNOWN_ACT_MASK(MC_PON_PORT)) |
		      (saved_ip4_pon << MC_UNKNOWN_ACT_SHIFT(MC_PON_PORT)));
	odi_reg_write(ODI_SW_MC_UNKNOWN_CFG_OFF,
		      (cfg & ~MC_CFG_RSV4_MASK) | (saved_cfg_rsv4 << MC_CFG_RSV4_SHIFT));
}

/* ---- the flags --------------------------------------------------------- */

uint32_t odi_switch_mcast_flags(void)
{
	return held;
}

int odi_switch_mcast_snoop_set(uint32_t flags)
{
	int rc;

	if (flags & ~ODI_SW_MC_SNOOP_ALL)
		return ODI_SW_MC_EINVAL;
	/* The drop only makes sense with the copy: without it no report
	 * reaches the snooper, no row is ever written, and every group would
	 * be dropped. The take is a mode of the copy rule. */
	if ((flags & (ODI_SW_MC_SNOOP_PON_DROP | ODI_SW_MC_SNOOP_TRAP)) &&
	    !(flags & ODI_SW_MC_SNOOP_COPY))
		return ODI_SW_MC_EINVAL;

	/* Off first, on last, so the order of a change never leaves the drop
	 * running without the copy. */
	if ((held & ODI_SW_MC_SNOOP_PON_DROP) && !(flags & ODI_SW_MC_SNOOP_PON_DROP)) {
		mc_pon_drop_off();
		held &= ~ODI_SW_MC_SNOOP_PON_DROP;
	}
	if ((held & ODI_SW_MC_SNOOP_COPY) && !(flags & ODI_SW_MC_SNOOP_COPY)) {
		odi_sw_mcast_copy_on = 0;
		odi_sw_mcast_trap_on = 0;
		mc_acl_remove();
		held &= ~(ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_TRAP);
	}
	/* A change of mode with the rule in: the hook's mark goes on before
	 * the take and off after it, so a report in between is at worst sent
	 * up twice (once by the switch, once by igmpd), never not at all. */
	if ((held & ODI_SW_MC_SNOOP_COPY) && (flags & ODI_SW_MC_SNOOP_COPY) &&
	    (held & ODI_SW_MC_SNOOP_TRAP) != (flags & ODI_SW_MC_SNOOP_TRAP)) {
		int trap = (flags & ODI_SW_MC_SNOOP_TRAP) != 0;

		if (trap)
			odi_sw_mcast_trap_on = 1;
		rc = mc_acl_mode(trap);
		if (rc) {
			odi_sw_mcast_trap_on = (held & ODI_SW_MC_SNOOP_TRAP) != 0;
			return rc;
		}
		if (!trap)
			odi_sw_mcast_trap_on = 0;
		held = (held & ~ODI_SW_MC_SNOOP_TRAP) | (flags & ODI_SW_MC_SNOOP_TRAP);
	}
	if ((flags & ODI_SW_MC_SNOOP_COPY) && !(held & ODI_SW_MC_SNOOP_COPY)) {
		int trap = (flags & ODI_SW_MC_SNOOP_TRAP) != 0;

		if (odi_sw_mcast_rx_enable)
			odi_sw_mcast_rx_enable();
		odi_sw_mcast_trap_on = (uint32_t)trap;
		rc = mc_acl_install(trap);
		if (rc) {
			odi_sw_mcast_trap_on = 0;
			return rc;
		}
		held |= flags & (ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_TRAP);
		odi_sw_mcast_copy_on = 1;
	}
	if ((flags & ODI_SW_MC_SNOOP_PON_DROP) && !(held & ODI_SW_MC_SNOOP_PON_DROP)) {
		mc_pon_drop_on();
		held |= ODI_SW_MC_SNOOP_PON_DROP;
	}
	return 0;
}

/* ---- the rows ---------------------------------------------------------- */

static int mc_find(const uint8_t *mac)
{
	uint32_t i;

	for (i = 0; i < ntracked; i++)
		if (!memcmp(tracked[i], mac, 6))
			return (int)i;
	return -1;
}

int odi_switch_mcast_row_add(const struct odi_sw_l2_mcast_req *req, uint32_t *index)
{
	int rc, known = mc_find(req->mac);

	if (known < 0 && ntracked >= ODI_SW_MC_TRACK_MAX)
		return ODI_SW_MC_ENOSPC;
	rc = odi_switch_l2_mcast_add(req, index);
	if (rc)
		return rc;
	if (known < 0)
		memcpy(tracked[ntracked++], req->mac, 6);
	return 0;
}

int odi_switch_mcast_row_del(const struct odi_sw_l2_mcast_req *req, int *found)
{
	int rc = odi_switch_l2_mcast_del(req, found);
	int at;

	if (rc)
		return rc;
	at = mc_find(req->mac);
	if (at >= 0) {
		ntracked--;
		if ((uint32_t)at != ntracked)
			memcpy(tracked[at], tracked[ntracked], 6);
	}
	return 0;
}

void odi_switch_mcast_release(void)
{
	struct odi_sw_l2_mcast_req req;

	/* The flags first: with the drop off the PON floods again before the
	 * rows go, so no group is ever dropped on the way out. */
	(void)odi_switch_mcast_snoop_set(0);
	while (ntracked) {
		memset(&req, 0, sizeof req);
		memcpy(req.mac, tracked[--ntracked], 6);
		/* A busy table engine gets one more try; a row still there
		 * after that is said, since nothing tracks it any more. */
		if (odi_switch_l2_mcast_del(&req, NULL) &&
		    odi_switch_l2_mcast_del(&req, NULL))
			ODI_SW_WARN("snooping row %02x:%02x:%02x:%02x:%02x:%02x not removed\n",
				    req.mac[0], req.mac[1], req.mac[2], req.mac[3],
				    req.mac[4], req.mac[5]);
	}
}

/* ---- the way back up --------------------------------------------------- */

int odi_switch_mcast_route(struct odi_sw_mc_route *r)
{
	struct odi_sw_us_tags t;
	struct odi_sw_us_treat tr;
	uint32_t flow, row;

	if ((r->mode != ODI_SW_MC_ROUTE_FRAME && r->mode != ODI_SW_MC_ROUTE_TAG) ||
	    r->stag > 1U || r->ctag > 1U || r->svid > 4095U || r->cvid > 4095U ||
	    r->spri > 7U || r->cpri > 7U || r->ethertype > 0xffffU ||
	    (r->mode == ODI_SW_MC_ROUTE_TAG && (r->want_vid > 4094U || r->want_pri > 7U)))
		return ODI_SW_MC_EINVAL;
	r->verdict = ODI_SW_US_NONE;
	r->via = r->row = r->flow = 0;
	r->c_act = r->c_vid = r->c_pri = r->c_src = 0;
	r->s_act = r->s_vid = r->s_pri = r->s_src = 0;

	if (r->mode == ODI_SW_MC_ROUTE_TAG &&
	    !odi_switch_bdgconn_us_by_tag(r->want_vid, r->want_pri, &flow, &row)) {
		r->verdict = ODI_SW_US_FWD;
		r->via = ODI_SW_MC_ROUTE_TAG;
		r->row = row;
		r->flow = flow;
		return 0;
	}
	t.stag = r->stag;
	t.svid = r->svid;
	t.spri = r->spri;
	t.ctag = r->ctag;
	t.cvid = r->cvid;
	t.cpri = r->cpri;
	t.ethertype = r->ethertype;
	odi_switch_bdgconn_us_classify(&t, &tr);
	r->verdict = tr.verdict;
	r->via = ODI_SW_MC_ROUTE_FRAME;
	r->row = tr.row;
	r->flow = tr.flow;
	r->c_act = tr.c_act;
	r->c_vid = tr.c_vid;
	r->c_pri = tr.c_pri;
	r->c_src = tr.c_src;
	r->s_act = tr.s_act;
	r->s_vid = tr.s_vid;
	r->s_pri = tr.s_pri;
	r->s_src = tr.s_src;
	return 0;
}

int odi_switch_mcast_send(const uint8_t *frame, uint32_t len, uint32_t flow)
{
	/* Only while the reports are taken: with the copy alone the switch
	 * has already sent the report, and with no lease nothing is igmpd's
	 * to send. */
	if (!(held & ODI_SW_MC_SNOOP_TRAP))
		return ODI_SW_MC_EPERM;
	if (!odi_igmp_tx_frame_ok(frame, len) || flow > ODI_IGMP_TX_FLOW_MAX ||
	    !odi_switch_bdgconn_us_flow_known(flow))
		return ODI_SW_MC_EINVAL;
	if (!odi_sw_mcast_tx)
		return ODI_SW_MC_EPERM;
	return odi_sw_mcast_tx(frame, len, flow);
}
