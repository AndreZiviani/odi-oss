/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_cmd.h -- odi_switch_cmd(), the OMCI driver command dispatch
 * omcid drives through the odi_omci netlink socket, keyed on the command
 * number omcid passes to omci_drv_call() (src/omci/respond, and the
 * OMCI_*_CMD constants of uapi/). The argument structs are the driver ABI
 * in uapi/.
 *
 * The commands an ISP1 or ISP2 provisioning run sends are implemented,
 * with the state they need: a T-CONT list, the downstream and upstream
 * GEM flow tables indexed by the flow id omcid sends, the queue ordinal
 * of cmd 23, and for cmd 51/50 the service list and the CF table derived
 * from the bridge rules (odi_switch_bdgconn.c). Every other command is a
 * no-op success where the stock driver writes nothing, a value from that
 * state, or -EOPNOTSUPP, logged once.
 *
 * odi_switch_cmd_reset_state() empties all of it: once at boot, and in
 * the host tests between scenarios.
 */
#ifndef ODI_SWITCH_CMD_H
#define ODI_SWITCH_CMD_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Well above one ISP1 provisioning run (5 T-CONTs, 6 DS and 5 US GEM
 * flows, 12 bridge connections, 13 priority-queue calls), so a second
 * provisioning pass does not wrap into another service slot.
 */
#define ODI_SW_CMD_TCONT_MAX	32
#define ODI_SW_CMD_GEM_DS_MAX	64
#define ODI_SW_CMD_GEM_US_MAX	64
#define ODI_SW_CMD_PRIQ_MAX	64
#define ODI_SW_CMD_BDGCONN_MAX	128

/* The command numbers the dispatch handles that have no OMCI_*_CMD
 * constant in uapi/, named here (kernel side only) with the stock driver
 * name. Same numbering as OMCI_*_CMD.
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

#ifdef __KERNEL__
#include <linux/printk.h>
#define ODI_SW_CMD_LOG(fmt, ...) pr_info_once("odi_switch_cmd: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define ODI_SW_CMD_LOG(fmt, ...) printf("odi_switch_cmd: " fmt, ##__VA_ARGS__)
#endif

void odi_switch_cmd_reset_state(void);

/* cmd 51 and cmd 50 (odi_switch_bdgconn.c): the bridge rule, or the
 * 4-byte service id, as omcid sends it. _reset clears the service list
 * and the CF table; odi_switch_cmd_reset_state() calls it.
 */
int odi_switch_bdgconn_activate(void *buf, uint32_t len);
int odi_switch_bdgconn_deactivate(void *buf, uint32_t len);
void odi_switch_bdgconn_reset(void);

/* buf/len: the argument struct omcid built for command cmd, as
 * omci_drv_call() sent it. Returns 0, or a negative errno (-EOPNOTSUPP,
 * -95, for a command not implemented).
 */
int odi_switch_cmd(uint32_t cmd, void *buf, uint32_t len);

#endif /* ODI_SWITCH_CMD_H */
