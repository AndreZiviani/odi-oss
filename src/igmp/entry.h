/* Building the driver structures the six multicast accessors take.
 *
 * Separate from the accessor call for the same reason frame.c is separate from
 * the daemon: laying out a structure is arithmetic, and arithmetic is testable
 * without a device. What is NOT testable without a device is whether the
 * driver likes the result, so every field here is one a vendor caller was
 * observed to write, and nothing is filled in because it seemed likely.
 *
 * The layout came from the pf_* layer inside the vendor igmpd, and the
 * marshalling into the 516-byte exchange buffer from librtk itself.
 */
#ifndef ODI_IGMP_ENTRY_H
#define ODI_IGMP_ENTRY_H

#include <stdint.h>

/* 36 bytes, confirmed four times: three memset sizes in the vendor callers and
 * the copy-loop bound inside librtk. Nine words, because that is what the
 * generated accessor takes. */
#define IGMP_MAC_ENTRY_WORDS  9

/* Bit 1 of the flags word at +24. The vendor sets it in the same branch that
 * writes the VID, so it reads as "this entry is qualified by the VID at +0".
 * Whether it means anything else is not established. */
#define IGMP_ENTRY_F_VID      2

/* Fill a 36-byte L2 multicast entry for an IPv4 group.
 *
 * `vid_valid` follows the vendor: it writes the VID only when ipmcMode is 0.
 * In the other mode the field is left zero and the flag is not set.
 *
 * `ports` is the member mask, bit (port - 1) for the one-based port the
 * redirect prefix carries. Pass 0 for a delete: the vendor delete path never
 * writes +12, and since the whole entry is zeroed first that is the same
 * bytes -- the key is the VID and the MAC, and nothing else.
 */
void igmp_mac_entry(uint32_t *e, uint16_t vid, uint32_t group, uint32_t ports,
		    int vid_valid);

/* The Ethernet address an IPv4 multicast group maps to: 01:00:5e and the low
 * 23 bits of the address. Exposed because the daemon prints it and the test
 * checks it. */
void igmp_group_mac(uint8_t *mac, uint32_t group);

#endif
