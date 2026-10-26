/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_mc_count.h -- ACL rules that count the frames or bytes of one
 * multicast destination entering the PON (or the UNI) port, and change
 * nothing else.
 *
 * What they are for: igmpd meters each joined group with one (src/igmp,
 * meter.h), for the lost groups of the multicast profile and its alarm, and
 * for the bandwidth the subscriber monitor reports (G.988 9.3.27 and
 * 9.3.29); and diag mc-count installs them by hand, which is how
 * tools/mcast-probe/count-proof.sh checks on a stick that a rule on the PON
 * ingress counts at all.
 *
 * A rule matches either a destination MAC under a mask (what igmpd uses: a
 * group's row in the L2 table is keyed on its MAC, so the counter sees the
 * frames that row forwards, whatever their VLAN tags) or an IPv4
 * destination under a mask with EtherType 0x0800. Its action is the log
 * class alone (class word 0x08): the switch forwards the frame exactly as
 * it would without the rule, which the probe run measured on the UNI
 * (docs/SWITCH.md, "Measured on a stick"). Nothing here sends a frame
 * anywhere.
 *
 * The rules are held by the open file of /dev/odi_sw that installed the
 * first one (odi_reg.c): its close removes every one, so igmpd dying leaves
 * the switch as it found it. They and the ACL probes exclude each other
 * (odi_switch_acl_probe.h): of two rules that match the same frame only the
 * lower entry counts it.
 *
 * Plain C with no kernel dependency, like odi_switch_acl_probe.c: the host
 * test (test/odi_switch_mc_count_test.c) builds it against
 * test/odi_switch_mock.h. Every function is called with odi_switch_lock
 * held.
 */
#ifndef ODI_SWITCH_MC_COUNT_H
#define ODI_SWITCH_MC_COUNT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Sixteen rules, ACL entries 48 to 63, on template 1; slot n counts in log
 * counter n, so the slots own pairs 0 to 7 of LOG_TYPE. Entries 64 to 79
 * are kept for more rules should the counters ever allow them. Template 1:
 * the module-load replay writes field codes into it but no rule uses it
 * (the ACL tables are emptied at boot and the replayed rule is entry 1 on
 * template 3); the probes keep template 4, the snooping copy 6, the
 * extended VLAN discards 5 (docs/SWITCH.md has the whole map).
 */
#define ODI_SW_MCC_SLOTS		16U
#define ODI_SW_MCC_ENTRY_BASE		48U
#define ODI_SW_MCC_TEMPLATE		1U
#define ODI_SW_MCC_COUNTER_BASE		0U

/* struct odi_sw_mc_count .op */
#define ODI_SW_MCC_SET		1U	/* install a rule in .slot */
#define ODI_SW_MCC_GET		2U	/* read .slot back, with its count */
#define ODI_SW_MCC_DEL		3U	/* remove .slot, or every slot with ODI_SW_MCC_ALL */
#define ODI_SW_MCC_ALL		0xffffffffU

/* .match */
#define ODI_SW_MCC_MATCH_MAC	1U	/* .mac under .mac_mask */
#define ODI_SW_MCC_MATCH_IPV4	2U	/* EtherType 0x0800, .dst under .dst_mask */

/* .port: the ingress ports a rule may name, the UNI and the PON. */
#define ODI_SW_MCC_PORT_UNI	0U
#define ODI_SW_MCC_PORT_PON	2U

/* One request of ODI_SW_IOC_MC_COUNT (odi_reg.h), 64 bytes. SET reads
 * .slot to .dst_mask; SET and GET fill the rest, GET echoing the rule's own
 * fields. .total is what the rule counted since it went in, 64 bits as two
 * words: the 32-bit counter is folded into it on every GET (and SET reads
 * the counter the moment before the rule can match), so a reader that GETs
 * more often than the counter can wrap never loses a count. A byte counter
 * wraps in 2^32 bytes: 13.8 s at the 2.488 Gbit/s of the whole downstream,
 * so igmpd reads every second.
 */
struct odi_sw_mc_count {
	uint32_t op;
	uint32_t slot;
	uint32_t port;		/* ODI_SW_MCC_PORT_* */
	uint32_t match;		/* ODI_SW_MCC_MATCH_* */
	uint32_t bytes;		/* 1: count bytes (FCS included), 0: frames */
	uint8_t mac[6];		/* MATCH_MAC: the destination MAC */
	uint8_t mac_mask[6];	/* the bits of .mac compared */
	uint32_t dst;		/* MATCH_IPV4: the IPv4 destination */
	uint32_t dst_mask;	/* the bits of .dst compared */
	uint32_t in_use;	/* GET: the slot holds a rule */
	uint32_t entry;		/* the slot's ACL entry */
	uint32_t counter;	/* the slot's log counter */
	uint32_t now;		/* the counter word as read */
	uint32_t total_lo;	/* what the rule counted, low word */
	uint32_t total_hi;	/* and high word */
};

/* One request, under odi_switch_lock. Returns 0 or a negative errno:
 * -EINVAL for a slot, port, match or unit out of range, or a mask that
 * compares nothing (a rule must name a destination); -EBUSY for a SET on a
 * slot in use, while an ACL probe is in, or for a unit other than the one
 * the other slot of its pair counts in; the table engine's error when a
 * write timed out (the slot is left empty).
 */
int odi_sw_mc_count_request(struct odi_sw_mc_count *req);

/* Whether any slot holds a rule: the end of the last one is the end of the
 * hold (odi_reg.c), and the probes wait for it. */
int odi_sw_mc_count_active(void);

/* Remove every rule: the close of the holder's file. */
void odi_sw_mc_count_release(void);

#endif /* ODI_SWITCH_MC_COUNT_H */
