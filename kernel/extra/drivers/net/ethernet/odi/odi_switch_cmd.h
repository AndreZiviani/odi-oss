/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_cmd.h -- odi_switch_cmd(), the OMCI driver command dispatch
 * omcid drives through the odi_omci netlink socket, keyed on the command
 * number omcid passes to omci_drv_call() (src/omci/respond, and the
 * OMCI_*_CMD constants of uapi/). The argument structs are the driver ABI
 * in uapi/.
 *
 * The commands an ISP1 or ISP2 provisioning run sends are implemented,
 * with the state they need: the T-CONTs by Alloc-ID CAM row with their
 * block of upstream queues (cmd 21, 23, 24), the downstream and upstream
 * GEM flow tables indexed by the flow id omcid sends, and for cmd 51/50
 * the service list and the
 * CF table derived from the bridge rules (odi_switch_bdgconn.c). Every
 * other command is a no-op success where the stock driver writes nothing,
 * a value from that state, or -EOPNOTSUPP, logged once.
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

/* Well above one ISP1 provisioning run (6 DS and 5 US GEM flows, 12
 * bridge connections), so a second provisioning pass does not wrap into
 * another service slot. The T-CONTs are the hardware's 16 data T-CONTs
 * (ODI_SW_PONQ_TCONTS, odi_switch_dal.h).
 */
#define ODI_SW_CMD_GEM_DS_MAX	64
#define ODI_SW_CMD_GEM_US_MAX	64
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
#define OMCI_SIGNAL_PARAMETER_CMD	11	/* setSignalParameter */
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

/* Who is told about the GEM flows cmd 25 creates: the GPON core, which
 * samples the per-flow counters of the flows that exist (odi_gpon_flow.h).
 * set runs after a flow is programmed, once per cmd 25 that succeeded
 * (us 0: downstream, 1: upstream; idx the flow id omcid sent), and clear
 * when odi_switch_cmd_reset_state() empties the flow tables. Both run in
 * process context with no switch spinlock held. NULL (the default, and in
 * the host tests) tells nobody.
 */
struct odi_switch_flow_ops {
	void (*set)(int us, uint32_t idx, uint32_t gem_port);
	void (*clear)(void);
};
void odi_switch_cmd_set_flow_ops(const struct odi_switch_flow_ops *ops);


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
