/* The switch side of the snooper. See program.c for why the writes are
 * opt-in. */
#ifndef ODI_IGMP_PROGRAM_H
#define ODI_IGMP_PROGRAM_H

#include <stdint.h>
#include "odi_sw_ioctl.h"

void igmp_hw_set_enabled(int on);
int  igmp_hw_enabled(void);

/* Read and print the IPv4 multicast lookup mode once. The entry is keyed
 * on filtering id 0 (SVL) either way; program.c says why. */
int igmp_hw_probe(void);

/* Take or change the snooping lease (see switch_opt.h). Returns 0 or the
 * negative errno; in a dry run it only says what it would do. */
int igmp_hw_snoop(uint32_t flags);

int igmp_hw_group_set(uint16_t vid, uint32_t group, uint32_t ports);
int igmp_hw_group_del(uint16_t vid, uint32_t group);

/* The same row by its MAC: what an IPv6 group (mld.h) is keyed on. */
int igmp_hw_mac_set(uint16_t vid, const uint8_t mac[6], uint32_t ports);
int igmp_hw_mac_del(uint16_t vid, const uint8_t mac[6]);

/* The way up for a taken report (uplink.h): the route the kernel gives, and
 * the send. Never called in a dry run, which sends nothing. Each returns 0
 * or the negative errno, and says what failed. */
int igmp_hw_route(struct odi_sw_mc_route *r);
int igmp_hw_send(const uint8_t *frame, uint32_t len, uint32_t flow);

/* The group counters meter.h reads: one per group MAC on the PON ingress,
 * counting bytes. Install and remove say what failed; the read is silent,
 * once a second as it is, and gives the low 32 bits of the total. Each
 * returns 0 or the negative errno. Never called in a dry run. */
int igmp_hw_count_set(uint32_t slot, uint32_t group);
int igmp_hw_count_del(uint32_t slot);
int igmp_hw_count_get(uint32_t slot, uint32_t *total);

#endif
