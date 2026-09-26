/* The device capability blob -- driver command 3, 120 bytes.
 *
 * OMCI_Init reads it straight into gInfo+112, so every gInfo offset the
 * vendor's code quotes is this blob plus 112. That is what makes the two
 * runtime tables the rest of this subsystem needed readable without a
 * debugger: they are not built by omci_app at all, they are what the driver
 * answers when asked.
 *
 * The layout, as far as this project reads it (the 120 bytes are accounted
 * for by the values observed on the device and where the daemon reads them):
 *
 *     +0    32 UNI slots of 2 bytes   { u8 slot type, u8 port id in type }
 *     +64   u32 FE port count
 *     +68   u32 GE port count
 *     +72   i32 CPU port
 *     +76   i32 PON port
 *     +80   i32 RGMII port
 *     +84   u32 POTS port count
 *     +88   u32 T-CONT count
 *     +92   u32 GEM port count
 *     +96   u32 T-CONT queue count
 *     +100  u32 queues per UNI
 *     +104  u8, u8                     (two bytes of padding follow)
 *     +108  u32 meter count
 *     +112  u32 a reserved meter id
 *     +116  u32 L2 table size
 *
 * Note +88: it is the T-CONT count, NOT the priority-queue count. This header
 * called it OMCI_CAPS_OFF_PRIQ and printed it as "priority queues" until that
 * was checked; the queue count is at +96.
 *
 * Observed on an ODI DFP-34X-2C2:
 *
 *     02 00 | 00 ff * 31            one UNI, slot 0, type 2
 *     ... 00000010 00000040 ...     16 T-CONTs, 64 GEM ports
 *
 * Command 3 is a `get`, so reading it does not disturb a running omci_app.
 */
#ifndef OMCI_CAPS_H
#define OMCI_CAPS_H

/* Kernel-portable guard, see omci_gemflow.h. */
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define OMCI_CAPS_CMD        3
#define OMCI_CAPS_LEN        120

#define OMCI_CAPS_UNI_SLOTS  32         /* 2 bytes each, blob +0 .. +63 */
#define OMCI_CAPS_OFF_CPUPORT   72
#define OMCI_CAPS_OFF_PONPORT   76
#define OMCI_CAPS_OFF_TCONTS    88      /* T-CONT count -- not the queues */
#define OMCI_CAPS_OFF_FLOWS     92      /* GEM port count */
#define OMCI_CAPS_OFF_PRIQ      96      /* T-CONT queue count */
#define OMCI_CAPS_OFF_UNIQ     100      /* queues per UNI */

/* The UNI slot's first byte, as pptp_eth_uni_me_id_to_switch_port compares it.
 * The pairing is the opposite way round from the obvious reading, and it is the
 * code that settles it: a VEIP entity id matches a slot of type 1, and a PPTP
 * Ethernet UNI entity id matches a slot of type 2. */
#define OMCI_UNI_SLOT_VEIP   1
#define OMCI_UNI_SLOT_PPTP   2

/* The vendor gates the search on the entity id's high byte: 1 for a PPTP
 * Ethernet UNI, and gInfo+328 -- set at startup, 6 on isp1, whose VEIP is
 * 0x0601 -- for a VEIP. gInfo is omci_app's own bss and not readable from
 * here, so the caller passes the slot type it wants instead; it always knows,
 * because it dispatched on the managed entity's class. */

#endif
