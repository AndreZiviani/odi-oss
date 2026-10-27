/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_mcast.h -- what IGMP snooping needs of the switch, held for
 * the life of the snooper.
 *
 * igmpd (src/igmp) keeps one static L2 row per joined group. For that it has
 * to see the host's IGMP reports, and the CPU must not be a destination of
 * anything the fibre sends, so the reports come as copies:
 *
 *   COPY      one ACL rule on the UNI port, IPv4 protocol 2, action "mirror
 *             to the CPU port": the frame is forwarded exactly as it was
 *             (the OLT sees every report and leave) and a copy goes to the
 *             CPU, where odi_omci.c hands it to igmpd. The rule has the UNI
 *             port as its only ingress, so a frame from the PON port never
 *             matches it: the PON isolation (docs/SWITCH.md) is untouched.
 *   PON_DROP  IPv4 multicast from the PON port that no L2 row names is
 *             dropped instead of flooded to the host, which is what makes a
 *             group the host did not join stay off the UNI link. The
 *             reserved groups 224.0.0.0/24 (the OLT's general queries go to
 *             224.0.0.1) keep flooding. IPv6 is PON_DROP6's.
 *   TRAP      the same rule takes the report instead of copying it: it
 *             reaches the CPU and goes no further, and igmpd sends upstream
 *             what the OLT's multicast profile allows, tagged as the
 *             profile says (G.988 9.3.27: upstream IGMP tag control, rate,
 *             unauthorized joins). Only with COPY, of which it is a mode.
 *             What igmpd sends goes through odi_switch_mcast_send() alone:
 *             IGMP to an IPv4 multicast MAC (or, with MLD, an MLD report or
 *             done to an IPv6 multicast MAC), onto an upstream flow a bridge
 *             rule queues on, and nothing else.
 *   MLD       the same for the host's MLD (IPv6): three more rules on the
 *             UNI port, ICMPv6 types 130 and 131 (query, v1 report), 132
 *             (v1 done) and 143 (v2 report), copied or taken as the IGMP
 *             rule is (TRAP is the mode of both). Only with COPY.
 *   PON_DROP6 IPv6 multicast from the PON that no L2 row names is dropped,
 *             as PON_DROP does for IPv4. The link-scope groups ff02::/16 and
 *             the solicited-node groups keep flooding whatever the drop
 *             says: router advertisements (ff02::1), neighbour solicitations
 *             and DHCPv6 (ff02::1:2) ride them, and pruning them would cut the
 *             host's IPv6 off. Only with MLD.
 *
 * All of them are held by one open file of /dev/odi_sw (odi_reg.c): the file's
 * close, or the daemon's death, takes them back and deletes every row the
 * holder added, so a dead snooper leaves the switch flooding as it did and
 * the host's reports going upstream by themselves again.
 *
 * Plain C with no kernel dependency, like odi_switch_l2.c: the host test
 * (test/odi_switch_mcast_test.c) builds it against test/odi_switch_mock.h.
 * Every function is called with odi_switch_lock held.
 */
#ifndef ODI_SWITCH_MCAST_H
#define ODI_SWITCH_MCAST_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "odi_switch_l2.h"

#define ODI_SW_MC_SNOOP_COPY		1U
#define ODI_SW_MC_SNOOP_PON_DROP	2U
#define ODI_SW_MC_SNOOP_TRAP		4U
#define ODI_SW_MC_SNOOP_MLD		8U
#define ODI_SW_MC_SNOOP_PON_DROP6	16U
#define ODI_SW_MC_SNOOP_ALL		(ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP | \
					 ODI_SW_MC_SNOOP_TRAP | ODI_SW_MC_SNOOP_MLD | \
					 ODI_SW_MC_SNOOP_PON_DROP6)

/* The ACL entry and template the copy rule owns. The ACL tables are emptied
 * at every boot (odi_switch_platform.c) and the only rule written after that
 * is the replayed one (entry 1, template 3), so these are free.
 */
#define ODI_SW_MC_ACL_ENTRY		8U
#define ODI_SW_MC_ACL_TEMPLATE		6U

/* The MLD rules: entries 9 to 11 (of the 9 to 15 kept for snooping), ahead
 * of every other rule but the IGMP one (the lowest matching entry decides),
 * on template 2, which no rule here names (the extended VLAN discards are on
 * 5, the ACL probes on 4, the replayed rule on 3). Template 2 compares the
 * EtherType, the IPv6 next header behind any extension headers, and the
 * window of user field selector 0, pointed at the start of the IPv6 payload:
 * the type and code of an ICMPv6 message. Both field codes and the
 * selector's origin are another open implementation's measurements on this
 * chip (an MLDv1 report behind its hop-by-hop header reads 58 in the next
 * header field and its type at payload offset 0). Nothing here or in the
 * replays points selector 0 anywhere; the lease saves it and puts it back.
 */
#define ODI_SW_MC_MLD_ENTRY		9U
#define ODI_SW_MC_MLD_RULES		3U
#define ODI_SW_MC_MLD_TEMPLATE		2U
#define ODI_SW_ACL_FIELD_IPV6_NH	0x15U
#define ODI_SW_ACL_FIELD_USER0		0x40U
#define ODI_SW_MC_MLD_FIELDSEL		0U

/* The ACL tables as the table engine takes them (odi_switch_platform.c
 * has the same three numbers for the boot clean): a rule is five words, an
 * action three, and bit 144 of the ones plane, bit 16 of word 0, is the
 * rule's valid bit.
 */
#define ODI_SW_MC_RULE_WORDS		5U
#define ODI_SW_MC_ACTION_WORDS		3U
#define ODI_SW_MC_RULE_VALID		(1U << 16)
#define ODI_SW_MC_ACL_PORT_EN_OFF	0x15040U

/* ACL_TMPL_FIELD: 0x015008, 7 templates of 8 slots, one 7-bit field code a
 * slot, four slots a word. ACL_ACT_EN(i): 0x015048 + 4i, the action classes
 * entry i applies (bit 4 the forwarding decision, 0x7f at reset).
 * ACL_DEFAULT_PERMIT: 0x015044, the verdict for a frame no rule matches.
 */
#define ODI_SW_ACL_TMPL_FIELD_OFF	0x15008U
#define ODI_SW_ACL_DEFAULT_PERMIT_OFF	0x15044U
#define ODI_SW_ACL_ACT_EN_OFF		0x15048U
#define ODI_SW_ACL_ACT_EN_FWD		0x10U
#define ODI_SW_ACL_ACT_EN_RESET		0x7fU
#define ODI_SW_ACL_FIELD_ETHERTYPE	0x07U
#define ODI_SW_ACL_FIELD_IPV4_PROTO	0x14U

/* FIELDSEL_CFG(n): 0x023144 + 4n, user field selector n: a 16-bit window
 * at byte offset bits 10:3 from origin bits 2:0, origin 6 the start of the
 * IP payload behind any IPv6 extension headers.
 */
#define ODI_SW_FIELDSEL_CFG_OFF		0x23144U
#define ODI_SW_FIELDSEL_ORIGIN_IP_PAYLOAD 6U

/* MC_UNKNOWN_IP4: 0x01c014, two bits per ingress port, 0 flood, 1 drop.
 * MC_UNKNOWN_IP6: 0x01c018, the same for IPv6.
 * MC_UNKNOWN_CFG: 0x01c01c, bits 4:3 the reserved IPv4 groups and bits 6:5
 * the reserved IPv6 groups (1: flooded whatever the port says), bit 8 makes
 * the link-scope groups ff02::/16 reserved IPv6 groups and bit 12 the
 * solicited-node groups ff02::1:ff00:0/104. The IPv6 half is another open
 * implementation's measurement on the PON port; out of reset no IPv6 class
 * is selected, so the drop alone would prune both.
 */
#define ODI_SW_MC_UNKNOWN_IP4_OFF	0x1c014U
#define ODI_SW_MC_UNKNOWN_IP6_OFF	0x1c018U
#define ODI_SW_MC_UNKNOWN_CFG_OFF	0x1c01cU

/* Set the flags held: ODI_SW_MC_SNOOP_* bits. 0 takes everything back. A
 * flag already held is left as it is, one not held is installed, one no
 * longer wanted is removed. Returns 0 or a negative errno; on a failure
 * the flags actually held are what odi_switch_mcast_flags() says.
 */
int odi_switch_mcast_snoop_set(uint32_t flags);
uint32_t odi_switch_mcast_flags(void);

/* Whether the IGMP copy is on, whether the MLD one is, and whether they
 * are a take (TRAP), for the RX hook (odi_omci.c): read without the switch
 * lock, from NAPI context. The hook marks a frame received while the take
 * is on, so igmpd sends upstream only a report the switch did not forward
 * itself.
 */
extern volatile uint32_t odi_sw_mcast_copy_on;
extern volatile uint32_t odi_sw_mcast_mld_on;
extern volatile uint32_t odi_sw_mcast_trap_on;

/* Called once, the first time the copy is installed, before the rule is
 * written: odi_omci.c registers the RX hook then, so an image that never
 * snoops pays nothing per host frame (the hook table has no unregister, and
 * walking it takes a lock). NULL in the host test. */
extern void (*odi_sw_mcast_rx_enable)(void);

/* The rows the holder programs, so that taking the lease back can delete
 * them: odi_switch_l2_mcast_add()/_del() plus the bookkeeping. Room for the
 * 128 groups of each family igmpd holds; an add that would be the 257th
 * distinct row is refused with -ENOSPC and leaves the table as it was.
 */
#define ODI_SW_MC_TRACK_MAX		256U
int odi_switch_mcast_row_add(const struct odi_sw_l2_mcast_req *req, uint32_t *index);
int odi_switch_mcast_row_del(const struct odi_sw_l2_mcast_req *req, int *found);

/* Remove every tracked row and every flag: the end of the lease. */
void odi_switch_mcast_release(void);

/* ---- the way back up, while the report is taken (TRAP) ---------------- */

/* What the classifier does with a frame entering the UNI port, answered by
 * odi_switch_bdgconn.c from the rows it programmed: the lowest upstream row
 * whose match the frame's tags fit, after the extended VLAN discards (the
 * ACL decides first). The tags as the frame carries them: an S-tag (TPID
 * 0x88a8) outermost if any, then a C-tag (0x8100); the EtherType of an
 * untagged frame for the rows that compare one.
 */
struct odi_sw_us_tags {
	uint32_t stag, svid, spri;	/* stag: 1 the frame has an S-tag */
	uint32_t ctag, cvid, cpri;	/* ctag: 1 the frame has a C-tag */
	uint32_t ethertype;
};

/* .verdict */
#define ODI_SW_US_NONE		0U	/* no row takes it: no stream, dropped */
#define ODI_SW_US_FWD		1U	/* queued on .flow, tags treated as below */
#define ODI_SW_US_DISCARD	2U	/* an extended VLAN discard, or a row that drops */

/* .c_src / .s_src: which of the row's VID and priority it assigns. A
 * field it does not assign is the frame's own (docs/SWITCH.md, "Extended
 * VLAN rules"). */
#define ODI_SW_US_SRC_VID	1U
#define ODI_SW_US_SRC_PRI	2U

/* The treatment, decoded from the row's action words: the C-tag action
 * (ODI_SW_CF_CACT_*: 0 none, 1 add or replace, 2 delete, 3 transparent)
 * and the S-tag action (ODI_SW_CF_CSACT_*: 1 add or replace, 3 delete, 0
 * and 4 leave it), each with the VID and priority it assigns. */
struct odi_sw_us_treat {
	uint32_t verdict;
	uint32_t row, flow;
	uint32_t c_act, c_vid, c_pri, c_src;
	uint32_t s_act, s_vid, s_pri, s_src;
};

void odi_switch_bdgconn_us_classify(const struct odi_sw_us_tags *t,
				    struct odi_sw_us_treat *out);
/* The lowest upstream row whose frames leave with one C-tag of VID `vid`
 * (one that adds it, or one that passes that VID through) and priority
 * `pri`, or failing that any priority it does not fix: 0 and *flow, *row;
 * -1 when no row carries that VLAN up. */
int odi_switch_bdgconn_us_by_tag(uint32_t vid, uint32_t pri, uint32_t *flow, uint32_t *row);
/* Whether an upstream row queues on `flow`. */
int odi_switch_bdgconn_us_flow_known(uint32_t flow);

/* One request of ODI_SW_IOC_MC_ROUTE (odi_reg.h), 88 bytes: where igmpd's
 * copy of a report goes up. FRAME: as the switch would have sent the
 * report itself (upstream IGMP tag control 0), the frame's tags in, the
 * row's treatment and flow out. TAG: the flow that carries a report re-tagged
 * by igmpd (control 1 to 3) with C-tag want_vid/want_pri, the row that
 * carries that VLAN up (.via TAG), or failing one the flow the report itself
 * would have taken (.via FRAME, the treatment then not to be applied).
 */
#define ODI_SW_MC_ROUTE_FRAME	1U
#define ODI_SW_MC_ROUTE_TAG	2U

struct odi_sw_mc_route {
	uint32_t mode;			/* ODI_SW_MC_ROUTE_* */
	uint32_t ethertype;
	uint32_t stag, svid, spri;
	uint32_t ctag, cvid, cpri;
	uint32_t want_vid, want_pri;	/* TAG */
	uint32_t verdict;		/* ODI_SW_US_* */
	uint32_t via;			/* ODI_SW_MC_ROUTE_*: what found the flow */
	uint32_t row, flow;
	uint32_t c_act, c_vid, c_pri, c_src;
	uint32_t s_act, s_vid, s_pri, s_src;
};

/* -EINVAL for an unknown mode or a field out of range. */
int odi_switch_mcast_route(struct odi_sw_mc_route *r);

/* The largest frame ODI_SW_IOC_MC_SEND carries: a full Ethernet frame
 * behind two tags, rounded up. */
#define ODI_SW_MC_SEND_MAX		1536U

/* One request of ODI_SW_IOC_MC_SEND (odi_reg.h), 1544 bytes. */
struct odi_sw_mc_send {
	uint32_t len;
	uint32_t flow;
	uint8_t frame[ODI_SW_MC_SEND_MAX];
};

/* Send frame up the PON on upstream flow `flow`. Only while the take is
 * held (the caller checks it is the holder), and only what igmpd has any
 * business sending: an IPv4 IGMP frame (behind at most two tags) to an IPv4
 * multicast MAC, or, while MLD is held too, an MLD report or done to an IPv6
 * multicast MAC, 60 bytes or more, on a flow an upstream row queues on.
 * Returns 0, -EPERM without the take, -EINVAL for any other frame or flow,
 * or the transmit's error. */
int odi_switch_mcast_send(const uint8_t *frame, uint32_t len, uint32_t flow);

/* The transmit itself, set by odi_omci.c, which owns the NIC's directed
 * transmit (odi_igmp_tx.h has its descriptor). NULL in the host test, where
 * the test sets its own. */
extern int (*odi_sw_mcast_tx)(const uint8_t *frame, uint32_t len, uint32_t flow);

#endif /* ODI_SWITCH_MCAST_H */
