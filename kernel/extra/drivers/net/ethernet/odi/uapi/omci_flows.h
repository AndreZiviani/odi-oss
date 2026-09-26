/* Read back the GEM flows odi_switch has programmed -- ours, not a vendor
 * OMCI command.
 *
 * The vendor switch driver answered per-flow table reads over a raw-socket
 * option, and a probe of ours used those; a 6.18 image has no such driver.
 * odi_switch_cmd() (kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c)
 * keeps what omcid asked it for in cmd 25 anyway -- the downstream table row
 * and the upstream slot are both the flow id omcid sent -- so this
 * command hands that record back. It is software state, what odi_switch
 * wrote, not a hardware table read: it cannot see a row something else
 * programmed, and reads back nothing from the switch. It is read-only.
 *
 * The command number is outside the driver command range (0..79) so it can never
 * collide with a vendor command. Fields are host-endian u32, like every other
 * command argument on this path.
 */
#ifndef OMCI_FLOWS_H
#define OMCI_FLOWS_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define OMCI_FLOWS_CMD   0x100u
#define OMCI_FLOWS_MAX   15

struct omci_flows {
	uint32_t ds_count;                 /* 1 + highest DS flow id programmed */
	uint32_t us_count;                 /* 1 + highest US flow id programmed */
	uint32_t ds_gem[OMCI_FLOWS_MAX];   /* row i: GEM port id, 0 if unused */
	uint32_t ds_cfg[OMCI_FLOWS_MAX];   /* row i: traffic type byte written */
	uint32_t us_gem[OMCI_FLOWS_MAX];   /* slot i: GEM port id */
};

#define OMCI_FLOWS_LEN   ((uint32_t)sizeof(struct omci_flows))    /* 188 */

#endif
