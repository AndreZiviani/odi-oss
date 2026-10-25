/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_acl_probe.h -- ACL rules installed by hand, to measure what the
 * switch does with multicast before anything relies on it: a rule that
 * counts the frames of one IPv4 protocol or one group entering the UNI or the
 * PON port, and a rule that copies (mirror) or takes (trap) the IGMP entering
 * the UNI port to the CPU. Nothing in the image installs one; diag does, on
 * request (`diag acl-probe`, docs/TOOLS.md), and tools/mcast-probe runs the
 * measurements docs/SWITCH.md, "Multicast", lists.
 *
 * What a rule may do is narrow on purpose. A counting rule takes the log
 * class alone, so the frames it matches are forwarded exactly as before. A
 * rule that sends anything to the CPU matches IGMP entering the UNI port and
 * nothing else: never a frame from the PON (docs/SWITCH.md, "PON port
 * isolation"), and never a class of frames the host could flood the CPU
 * with. A frame such a rule hands the CPU is counted (odi_omci.c calls
 * odi_sw_probe_rx()) and then goes where it went before: the PON port has no
 * RX hook that takes it, and the NIC drops it.
 *
 * The rules are held by the open file of /dev/odi_sw that installed them
 * (odi_reg.c): its close removes every one, so a probe that dies leaves the
 * switch as it found it. They are not installed while the snooping lease is
 * held (odi_switch_mcast.h), and the lease is not given while they are: the
 * copy rule and a probe of the same frames would decide for each other.
 *
 * Plain C with no kernel dependency, like odi_switch_mcast.c: the host test
 * (test/odi_switch_acl_probe_test.c) builds it against test/odi_switch_mock.h.
 * Every function but odi_sw_probe_rx() is called with odi_switch_lock held.
 */
#ifndef ODI_SWITCH_ACL_PROBE_H
#define ODI_SWITCH_ACL_PROBE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Eight rules, ACL entries 88 to 95, all on template 4 (EtherType, IPv4
 * protocol and destination address in slots 0 to 3). The module-load replay
 * keeps entry 1 and template 3, the snooping copy entry 8 and template 6,
 * the extended VLAN discards entries 16 to 47 and template 5. Slot n counts
 * in log counter 16 + 2n, the first of the pair 8 + n, so each slot owns
 * its pair's frames-or-bytes bit in LOG_TYPE.
 */
#define ODI_SW_PROBE_SLOTS		8U
#define ODI_SW_PROBE_ENTRY_BASE		88U
#define ODI_SW_PROBE_TEMPLATE		4U
#define ODI_SW_PROBE_COUNTER_BASE	16U

/* The log counters: 32 words of 32 bits from LOG_CNT, and LOG_TYPE, a bit a
 * pair of counters, 1 counting bytes and 0 frames (from another open
 * implementation's measurements). Nothing else in this tree programs a log
 * counter, so LOG_TYPE is written whole from what the probes want and never
 * read: it is outside every register read made on a stick so far, and a
 * register is not read here on the strength of a guess.
 */
#define ODI_SW_LOG_CNT_OFF		0x32600U
#define ODI_SW_LOG_TYPE_OFF		0x34008U

/* The receive reason of a frame an ACL rule sent the CPU: 128 plus the
 * entry (another open implementation's measurements, for both a trap and a
 * mirror). odi_sw_probe_rx() counts by it, so a wrong value shows as
 * frames counted under uni_igmp_other instead of a slot.
 */
#define ODI_SW_PROBE_REASON_ACL0	128U

/* struct odi_sw_acl_probe .op */
#define ODI_SW_PROBE_SET	1U	/* install a rule in .slot */
#define ODI_SW_PROBE_GET	2U	/* read .slot back, with its counts */
#define ODI_SW_PROBE_DEL	3U	/* remove .slot, or every slot with ODI_SW_PROBE_ALL */

/* .action */
#define ODI_SW_PROBE_COUNT	1U	/* count, forward as before */
#define ODI_SW_PROBE_MIRROR	2U	/* count, forward as before, and a copy to the CPU */
#define ODI_SW_PROBE_TRAP	3U	/* count, and to the CPU instead of onwards */

#define ODI_SW_PROBE_ANY	0x100U	/* .proto: any protocol */
#define ODI_SW_PROBE_ALL	0xffffffffU	/* .slot of a DEL: every slot */

/* The ingress ports a rule may name: the UNI and the PON. */
#define ODI_SW_PROBE_PORT_UNI	0U
#define ODI_SW_PROBE_PORT_PON	2U

/* One request of ODI_SW_IOC_ACL_PROBE (odi_reg.h), 72 bytes. SET reads
 * .slot to .dst_mask; GET and SET fill the rest, and GET echoes the rule's
 * own fields. .base and .now are raw counter words: what the rule counted
 * is now - base, modulo 2^32 (the counter is not cleared, and is read the
 * moment before the rule goes valid).
 */
struct odi_sw_acl_probe {
	uint32_t op;
	uint32_t slot;
	uint32_t port;		/* ODI_SW_PROBE_PORT_* */
	uint32_t action;	/* ODI_SW_PROBE_COUNT/_MIRROR/_TRAP */
	uint32_t bytes;		/* 1: the counter counts bytes, 0: frames */
	uint32_t proto;		/* IPv4 protocol, or ODI_SW_PROBE_ANY */
	uint32_t dst;		/* IPv4 destination address */
	uint32_t dst_mask;	/* the bits of .dst compared; 0 compares none */
	uint32_t in_use;	/* GET: the slot holds a rule */
	uint32_t entry;		/* the slot's ACL entry */
	uint32_t counter;	/* the slot's log counter */
	uint32_t base;		/* the counter when the rule went in */
	uint32_t now;		/* GET: the counter now */
	uint32_t cpu_hits;	/* GET: frames the CPU received with this entry's reason */
	uint32_t cpu_ports;	/* GET: the ports those came in by, a bit a port */
	uint32_t uni_igmp_other; /* GET: IGMP from the UNI that reached the CPU with another reason */
	uint32_t other_reason;	/* GET: the reason of the last of those */
	uint32_t cpu_bytes;	/* GET: the length of the cpu_hits frames as the CPU got them */
};

/* One request, under odi_switch_lock. Returns 0 or a negative errno:
 * -EINVAL for a slot, port, action or protocol out of range, a copy or a
 * trap of anything but IGMP from the UNI, or a counter mode not 0 or 1;
 * -EBUSY for a SET on a slot in use or while the snooping lease is held; the
 * table engine's error when a write timed out (the slot is left empty).
 */
int odi_sw_probe_request(struct odi_sw_acl_probe *req);

/* Whether any slot holds a rule: the end of the last one is the end of the
 * hold (odi_reg.c). */
int odi_sw_probe_active(void);

/* Remove every rule: the close of the holder's file. */
void odi_sw_probe_release(void);

/* From the RX path (odi_omci.c), without the lock, for every frame no
 * earlier hook took while a rule is in: its receive reason, the port it
 * came in by, its length and whether it is IGMP. Counts only.
 */
void odi_sw_probe_rx(uint32_t reason, uint32_t src_port, uint32_t len, int igmp);
extern volatile uint32_t odi_sw_probe_on;

/* Called once before the first rule is written, so a frame it sends the
 * CPU is never missed; odi_omci.c registers its RX hook then. NULL in the
 * host test. */
extern void (*odi_sw_probe_rx_enable)(void);

#endif /* ODI_SWITCH_ACL_PROBE_H */
