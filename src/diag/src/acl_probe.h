/* `acl-probe`: hand-installed ACL rules that count multicast entering the UNI
 * or the PON port, or copy or take the host's IGMP to the CPU, for measuring
 * the switch (odi_switch_acl_probe.h in the kernel tree has what each may
 * do). The rules belong to the process that installed the first of them and
 * go when it exits, so a one-shot `diag acl-probe count ...` installs a rule
 * that is gone again before the prompt comes back: install and read them
 * from one diag reading its commands from stdin (tools/mcast-probe does).
 *
 * Every answer is one line of key=value words, for a script to read:
 *
 *   slot=0 entry=88 counter=16 port=2 action=count unit=frames proto=2
 *   dst=0.0.0.0/0 counted=17 base=0x00000000 now=0x00000011 cpu=0
 *   cpu_ports=0x0 cpu_bytes=0 uni_igmp_other=0 other_reason=0
 *
 * (one line on the device), or `slot=3 entry=91 counter=22 empty`.
 */
#ifndef ODI_ACL_PROBE_H
#define ODI_ACL_PROBE_H

#include <stdint.h>

int cmd_probe_set(uint32_t sel, const uint32_t *u);
int cmd_probe_show(uint32_t slot);
int cmd_probe_delete(uint32_t slot);

#endif
