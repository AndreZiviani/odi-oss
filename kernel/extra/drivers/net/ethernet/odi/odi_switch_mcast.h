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
 *             224.0.0.1) keep flooding. IPv6 is left alone: igmpd does not
 *             do MLD.
 *
 * Both are held by one open file of /dev/odi_sw (odi_reg.c): the file's
 * close, or the daemon's death, takes both back and deletes every row the
 * holder added, so a dead snooper leaves the switch flooding as it did.
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
#define ODI_SW_MC_SNOOP_ALL		(ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP)

/* The ACL entry and template the copy rule owns. The ACL tables are emptied
 * at every boot (odi_switch_platform.c) and the only rule written after that
 * is the replayed one (entry 1, template 3), so these are free.
 */
#define ODI_SW_MC_ACL_ENTRY		8U
#define ODI_SW_MC_ACL_TEMPLATE		6U

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

/* MC_UNKNOWN_IP4: 0x01c014, two bits per ingress port, 0 flood, 1 drop.
 * MC_UNKNOWN_CFG: 0x01c01c, bits 4:3 the reserved IPv4 groups (1: flooded
 * whatever the port says).
 */
#define ODI_SW_MC_UNKNOWN_IP4_OFF	0x1c014U
#define ODI_SW_MC_UNKNOWN_CFG_OFF	0x1c01cU

/* Set the flags held: ODI_SW_MC_SNOOP_* bits. 0 takes everything back. A
 * flag already held is left as it is, one not held is installed, one no
 * longer wanted is removed. Returns 0 or a negative errno; on a failure
 * the flags actually held are what odi_switch_mcast_flags() says.
 */
int odi_switch_mcast_snoop_set(uint32_t flags);
uint32_t odi_switch_mcast_flags(void);

/* Whether the IGMP copy is on, for the RX hook (odi_omci.c): read without
 * the switch lock, from NAPI context.
 */
extern volatile uint32_t odi_sw_mcast_copy_on;

/* Called once, the first time the copy is installed, before the rule is
 * written: odi_omci.c registers the RX hook then, so an image that never
 * snoops pays nothing per host frame (the hook table has no unregister, and
 * walking it takes a lock). NULL in the host test. */
extern void (*odi_sw_mcast_rx_enable)(void);

/* The rows the holder programs, so that taking the lease back can delete
 * them: odi_switch_l2_mcast_add()/_del() plus the bookkeeping. An add that
 * would be the 129th distinct row is refused with -ENOSPC and leaves the
 * table as it was.
 */
#define ODI_SW_MC_TRACK_MAX		128U
int odi_switch_mcast_row_add(const struct odi_sw_l2_mcast_req *req, uint32_t *index);
int odi_switch_mcast_row_del(const struct odi_sw_l2_mcast_req *req, int *found);

/* Remove every tracked row and every flag: the end of the lease. */
void odi_switch_mcast_release(void);

#endif /* ODI_SWITCH_MCAST_H */
