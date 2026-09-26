/* The switch accessors igmpd programs through: three /dev/odi_sw ioctls,
 * answered by odi_switch_l2.c in our kernel (odi_reg.c dispatches them under
 * odi_switch_lock).
 *
 *   igmp_sw_mode        how IPv4 multicast is looked up (L2_LOOKUP_SETUP bit 23):
 *                       0 on MAC + VID/FID, the mode these entries serve
 *   igmp_sw_mcast_add   place a static L2 multicast entry by the hardware
 *                       hash; m->index is the row it landed on
 *   igmp_sw_mcast_del   remove the entry with the same key; m->found says
 *                       whether there was one
 *
 * These replace three stock-driver socket options that no 6.18 kernel
 * answers (every call failed with -99). The file keeps its name so the
 * history of that change stays in one place.
 *
 * Each returns 0 or a negative errno: -2 when /dev/odi_sw is missing, -25
 * when the kernel predates these ioctls, -22 for an entry the driver
 * refuses, -28 when the key hash bucket is full, -16 when the table engine
 * stayed busy.
 */
#ifndef ODI_IGMP_SWITCH_OPT_H
#define ODI_IGMP_SWITCH_OPT_H

#include <stdint.h>
#include "odi_sw_ioctl.h"

int igmp_sw_mode(uint32_t *ipmc_on_group);
int igmp_sw_mcast_add(struct odi_sw_l2_mcast *m);
int igmp_sw_mcast_del(struct odi_sw_l2_mcast *m);

#endif
