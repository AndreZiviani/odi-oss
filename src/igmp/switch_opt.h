/* The three switch accessors igmpd programs through: the stock switch
 * driver's L2 multicast socket options.
 *
 * These moved here from diag, which no longer carries any socket-option
 * layer. No option here is answered by our 6.18 kernel -- odi_switch has no
 * netlink op for the L2 multicast table -- so on our image every call fails
 * with -99 (ENOPROTOOPT) and program.c says so. They stay because igmpd is
 * still a dry run by default and its hardware path is not yet ported.
 *
 * An entry is IGMP_MAC_ENTRY_WORDS host words (entry.h); each call packs
 * them big-endian into the driver's 516-byte exchange buffer.
 */
#ifndef ODI_IGMP_SWITCH_OPT_H
#define ODI_IGMP_SWITCH_OPT_H

#include <stdint.h>

int rtk_l2_ipmcMode_get(uint32_t *mode);
int rtk_l2_mcastAddr_add(uint32_t *entry);
int rtk_l2_mcastAddr_del(const uint32_t *entry);

#endif
