/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_cmd.h -- the 79-slot OMCI command table, replacing
 * the stock OMCI kernel modules' dispatch. One entry point,
 * keyed on the OMCI driver command number exactly as apply.c already
 * passes it to omci_drv_call() (see src/omci/respond/apply.c and the
 * per-header OMCI_*_CMD constants) -- not a re-numbering of our own.
 *
 * Argument structs are the ones omcid already builds, reused unmodified
 * from src/omci/omci_gemflow.h, omci_bdgconn.h, omci_bridgeport.h (their
 * only change for this task: the <stdint.h> include is now guarded the
 * same way odi_switch_hw.h guards it, so they compile under __KERNEL__
 * too -- no struct or field touched).
 *
 * The 15 commands a isp1 provisioning run exercises are implemented with
 * the bookkeeping named for each: a T-CONT allocator, downstream/upstream
 * GEM-flow tables indexed by the flow id omcid sends, a growing queue-slot
 * ordinal for cmd 23 full PONQ_COUNT_MASK program, and for cmd 51/50
 * (activate/deactivate bridge connection) a service list plus the CF
 * table it derives from each bridge rule. Every other slot returns the appropriate
 * verdict for that command: a no-op success (a stub `_Clear` counter reset
 * with no register work), a value read from software shadow state, or
 * -EOPNOTSUPP logged once (never silently zero) -- see odi_switch_cmd.c
 * switch for the per-command reasoning.
 *
 * odi_switch_cmd_reset_state() clears every allocator back to empty --
 * used by the host test between fixture runs and, on target, once at
 * module load before the first command.
 */
#ifndef ODI_SWITCH_CMD_H
#define ODI_SWITCH_CMD_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Generous over isp1 one provisioning run (5 T-CONTs, 6 DS + 5 US GEM
 * flows, 12 bridge connections, 13 priority-queue calls) so a second
 * provisioning pass does not silently wrap an allocator into another
 * service slot.
 */
#define ODI_SW_CMD_TCONT_MAX	32
#define ODI_SW_CMD_GEM_DS_MAX	64
#define ODI_SW_CMD_GEM_US_MAX	64
#define ODI_SW_CMD_PRIQ_MAX	64
#define ODI_SW_CMD_BDGCONN_MAX	128

/* OMCI driver command numbers odi_switch_cmd() dispatches that have no
 * OMCI_*_CMD constant in any src/omci header (OMCI_CAPS_CMD, OMCI_FLOOD_CMD
 * and friends cover the rest, and are used as-is). src/omci/generated/
 * omci_drv.c is a mechanical table keyed on the bare number for every one
 * of its ~79 commands, so it has no name to reuse for these either; named
 * here, kernel-side only, so the dispatch switch has no bare number where
 * a name already exists anywhere. Same numbering space as OMCI_*_CMD
 * (odi_switch_cmd.c's own dispatch comment has the detail).
 */
#define OMCI_DEV_ID_VERSION_CMD		4	/* getDevIdVersion */
#define OMCI_SERIAL_NUM_CMD		15	/* getSerialNum */
#define OMCI_ONU_STATE_CMD		13	/* getOnuState */
#define OMCI_AGEING_TIME_CMD		62	/* setAgeingTime */
#define OMCI_PORT_AUTO_NEGO_CMD		30	/* setPortAutoNegoAbility */
#define OMCI_PORT_STATE_CMD		32	/* setPortState */
#define OMCI_PHY_PWRDOWN_CMD		38	/* setPhyPwrDown */
#define OMCI_TRANSCEIVER_STATUS_CMD	10	/* getTransceiverStatus */
#define OMCI_DS_BC_GEMFLOW_CMD		26	/* setDsBcGemFlow */
#define OMCI_RESET_US_FLOW_STAT_CMD	45	/* resetUsFlowStat */
#define OMCI_RESET_DS_FLOW_STAT_CMD	47	/* resetDsFlowStat */

void odi_switch_cmd_reset_state(void);

/* buf/len: the argument struct omcid already built for this OMCI driver command
 * number, by value, same as omci_drv_call's own (void *buf, uint32_t
 * len) pair -- the netlink op wires this in, this function does not know
 * or care how buf arrived. Returns 0 on
 * success, a negative errno otherwise (-EOPNOTSUPP, numerically -95, for
 * every command that could not be implemented from trace evidence).
 */
int odi_switch_cmd(uint32_t cmd, void *buf, uint32_t len);

#endif /* ODI_SWITCH_CMD_H */
