/* Building the L2 multicast entry igmpd hands the switch.
 *
 * Separate from the ioctl for the same reason frame.c is separate from the
 * daemon: laying out a request is arithmetic, and arithmetic is testable
 * without a device. The request is struct odi_sw_l2_mcast_req (the
 * /dev/odi_sw ABI, restated in diag/src/odi_sw_ioctl.h); the kernel turns
 * it into a lookup-table row and refuses what it cannot place.
 */
#ifndef ODI_IGMP_ENTRY_H
#define ODI_IGMP_ENTRY_H

#include <stdint.h>
#include "odi_sw_ioctl.h"

/* The member ports the switch has: bits 0..3, one per hardware port (0 the
 * UNI, 2 the PON, 3 the CPU). */
#define IGMP_HW_PORTS_MASK  0xfu

/* Fill an L2 multicast request for an IPv4 group.
 *
 * `vid_valid` picks the key: set, the entry is keyed on the VID (IVL);
 * clear, on filtering id 0 (SVL) and the VID plays no part. The daemon
 * always passes 0 -- every VLAN on this image is shared, so an IVL entry
 * would never match (program.c has the evidence).
 *
 * `ports` is the member mask, bit (port - 1) for the one-based port the
 * redirect prefix carries -- which is bit (hardware port). Pass 0 for a
 * delete: the key is the MAC and the VID or filtering id, and nothing else.
 *
 * Returns 0, or -1 when `ports` names a bit past the switch ports: that
 * mask would otherwise be cut down to the ports that exist, which forwards
 * to fewer ports than the group state says, so it is refused instead.
 */
int igmp_mac_entry(struct odi_sw_l2_mcast *m, uint16_t vid, uint32_t group,
		   uint32_t ports, int vid_valid);

/* The Ethernet address an IPv4 multicast group maps to: 01:00:5e and the low
 * 23 bits of the address. Exposed because the daemon prints it and the test
 * checks it. */
void igmp_group_mac(uint8_t *mac, uint32_t group);

/* Command-line numbers: a dotted IPv4 address, and an unsigned number in
 * decimal or 0x hex. Both return 1 and set *out, or 0 on anything else. */
int igmp_parse_ipv4(const char *s, uint32_t *out);
int igmp_parse_num(const char *s, uint32_t *out);

#endif
