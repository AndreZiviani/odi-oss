/* The switch side of the snooper. See program.c for why the writes are
 * opt-in. */
#ifndef ODI_IGMP_PROGRAM_H
#define ODI_IGMP_PROGRAM_H

#include <stdint.h>

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

#endif
