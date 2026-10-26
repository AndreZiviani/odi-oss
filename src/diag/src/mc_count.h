/* `mc-count`: hand-installed multicast group counters, the rules igmpd meters
 * its groups with (odi_switch_mc_count.h in the kernel tree has what they
 * match and why they only count). Like the probes, the rules belong to the
 * process that installed the first of them and go when it exits: install
 * and read them from one diag reading its commands from stdin
 * (tools/mcast-probe/count-proof.sh does).
 *
 * A MAC is given as two numbers, the first two octets and the last four, and
 * a prefix length from the top: 33:33:00:00:00:01 is `0x3333 0x00000001 48`,
 * every IPv6 multicast MAC `0x3333 0 16`.
 *
 * Every answer is one line of key=value words, for a script to read:
 *
 *   slot=0 entry=48 counter=0 port=2 match=mac dst=33:33:00:00:00:01/48
 *   unit=frames counted=17 now=0x00000011
 *
 * (one line on the device), or `slot=3 entry=51 counter=3 empty`. counted
 * is what the rule counted since it went in, folded to 64 bits by the kernel
 * on every read.
 */
#ifndef ODI_MC_COUNT_H
#define ODI_MC_COUNT_H

#include <stdint.h>

int cmd_mcc_set(uint32_t sel, const uint32_t *u);
int cmd_mcc_show(uint32_t slot);
int cmd_mcc_delete(uint32_t slot);

#endif
