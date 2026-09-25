/* Hardware access, through the interfaces our own kernel provides.
 *
 *   /dev/odi_sw      switch-core register get/set, per-port MIB counters,
 *                    the transceiver DDM block and the L2 lookup table
 *                    (odi_reg.c, odi_ddm.c, odi_switch_l2.c)
 *   /proc/odi_gpon   the GPON state machine state and the last LOS sample
 *                    (odi_gpon.c)
 *   netlink          the GEM flows odi_switch recorded (odi_omci.c,
 *                    OMCI_FLOWS_CMD)
 *
 * Each accessor has exactly one path. They return 0 on success and a
 * negative value otherwise; callers print their own failure line.
 *
 * The device node is mode 0600 and the netlink command path needs
 * CAP_NET_ADMIN, so diag runs as root, as everything on the stick does.
 */
#ifndef ODI_HW_H
#define ODI_HW_H

#include <stdint.h>

/* Transceiver DDM selectors, in the order the ODI_SW_IOC_DDM_GET ioctl
 * numbers them (odi_ddm.h, ODI_DDM_*). */
enum ddm_sel {
	DDM_VENDOR_NAME  = 0,
	DDM_PART_NUMBER  = 1,
	DDM_TEMPERATURE  = 2,
	DDM_VOLTAGE      = 3,
	DDM_BIAS_CURRENT = 4,
	DDM_TX_POWER     = 5,
	DDM_RX_POWER     = 6,
	DDM_SEL_COUNT    = 7,
};

/* The raw DDMI block one selector returns. */
#define DDM_RAW_LEN 24

/* The switch-core register window. Addresses are offsets into it, must be
 * 4-byte aligned and must lie below SWCORE_SIZE. The driver checks both as
 * well; checking here too gives a clear message instead of an errno. */
#define SWCORE_SIZE 0x02000000u

/* The GPON downstream interrupt status register. Its low bits are the live
 * LOS, LOF and LOM conditions (GPON_GTC_DS_INTR_STS, odi_gpon_hw.h). Reading
 * it has no side effect; odi_gpon reads it the same way in its ISR. */
#define GPON_DS_INTR_STS 0x701008u

/* The highest port number a port list can name. */
#define HW_PORT_MAX 31

int hw_addr_get(uint32_t addr, uint32_t *value);
int hw_addr_set(uint32_t addr, uint32_t value);

/* Fills DDM_RAW_LEN bytes of raw DDMI for `sel`. */
int hw_transceiver_get(int sel, uint8_t out[DDM_RAW_LEN]);

/* The GPON state machine's current state, 0-7. */
int hw_gpon_status_get(uint32_t *state);

/* LOS, LOF and LOM, as GPON_ALARM_* bits (gpon_status.h). *alarms gets the
 * asserted bits, *known the bits this answer covers: all three when the live
 * register can be read, LOS alone when only the /proc sample can. */
int hw_gpon_alarms_get(uint32_t *alarms, uint32_t *known);

/* One 64-bit MIB counter. `counter` indexes mib_names[] (mib.h). The driver
 * refuses a port the board does not have and a counter it has no register
 * for; both come back as a failure. */
#define MIB_COUNT 69
int hw_stat_port_get(uint32_t port, uint32_t counter, uint64_t *value);

/* The L2 lookup table (odi_switch_l2.c). hw_l2_get reads one row by number,
 * valid or not; hw_l2_next the first valid row at or after *index, setting
 * *index to it, and returns 1 when there is none left. hw_l2_mode gives the
 * number of rows and the IPv4 multicast lookup mode. All three return a
 * negative value when the driver refuses or has no such ioctl. */
struct odi_sw_l2_row;
int hw_l2_get(uint32_t index, struct odi_sw_l2_row *row);
int hw_l2_next(uint32_t *index, struct odi_sw_l2_row *row);
int hw_l2_mode(uint32_t *rows, uint32_t *ipmc_on_group);

/* The GEM flows odi_switch recorded. Returns 0 when answered, 1 when there is
 * no netlink command path, and 2 when the path answered but the kernel has no
 * flow readback (EOPNOTSUPP). */
struct omci_flows;
int hw_gpon_flows_get(struct omci_flows *f);

#endif
