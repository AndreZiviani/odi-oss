/* Hardware access, through the interfaces our kernel provides.
 *
 *   /dev/odi_sw      switch-core register get/set, per-port MIB counters,
 *                    the transceiver DDM block and the L2 lookup table
 *                    (odi_reg.c, odi_ddm.c, odi_switch_l2.c)
 *   /proc/odi_gpon/status
 *                    the GPON state machine state and the last LOS sample
 *                    (odi_gpon.c)
 *   /proc/odi_gpon/flows
 *                    the per-flow GEM counters the driver accumulates
 *                    (odi_gpon.c)
 *   netlink          the GEM flows odi_switch recorded (odi_omci.c,
 *                    OMCI_FLOWS_CMD)
 *
 * Each accessor has exactly one path. They return 0 on success and a
 * negative value otherwise; the caller prints the failure line.
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

/* Alarm/warning flags plus the optical LOS status bit: one contiguous
 * 8-byte A2h read (odi_ddm.h's ODI_DDM_ALARM_STATUS, which this mirrors).
 * Not one of the DDM_SEL_COUNT selectors above -- it is not part of the
 * seven-reading RTK_OPT_TRANSCEIVER mirror -- so it goes through its own
 * accessor, hw_transceiver_alarms_get(), rather than hw_transceiver_get().
 */
#define DDM_SEL_ALARM_STATUS 7

/* The module's description of its own readings: A0h 92-94 (monitoring type)
 * and the alarm/warning limits, A2h 0-23 and 24-39 (odi_ddm.h's
 * ODI_DDM_MONITOR_TYPE, ODI_DDM_LIMITS_A, ODI_DDM_LIMITS_B, which these
 * mirror). Read raw through hw_transceiver_block_get(). */
#define DDM_SEL_MONITOR_TYPE 8
#define DDM_SEL_LIMITS_A     9
#define DDM_SEL_LIMITS_B     10

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

/* The raw block of DDM_SEL_MONITOR_TYPE, DDM_SEL_LIMITS_A or DDM_SEL_LIMITS_B
 * (anything else is refused). */
int hw_transceiver_block_get(int sel, uint8_t out[DDM_RAW_LEN]);

/* Alarm and warning flags for temperature, voltage, bias current, tx power
 * and rx power (SFF-8472 A2h bytes 112-113/116-117: bit 15 down to bit 6,
 * two bits -- high, low -- per field, in the field order above), plus the
 * optical RX_LOS status bit (byte 110 bit 1). Returns 0 on success. */
int hw_transceiver_alarms_get(uint32_t *alarms, uint32_t *warnings, int *los);

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

/* One ACL probe request (odi_sw_ioctl.h), answered in place. Returns 0 or
 * the driver's negative errno: -22 for a request it refuses, -16 while
 * another process holds the probes or the snooping lease is held, -25 on a
 * kernel without the probes. */
struct odi_sw_acl_probe;
int hw_acl_probe(struct odi_sw_acl_probe *p);

/* One multicast group counter request (odi_sw_ioctl.h), answered in place.
 * Returns 0 or the driver's negative errno: -22 for a request it refuses,
 * -16 while another process holds the counters or a probe is in, or for a
 * unit the other slot of the pair does not count in, -25 on a kernel
 * without them. */
struct odi_sw_mc_count;
int hw_mc_count(struct odi_sw_mc_count *c);

/* The GEM flows odi_switch recorded. Returns 0 when answered, 1 when there is
 * no netlink command path, and 2 when the path answered but the kernel has no
 * flow readback (EOPNOTSUPP). */
struct omci_flows;
int hw_gpon_flows_get(struct omci_flows *f);

/* The per-flow GEM counters, as /proc/odi_gpon/flows has them, handed to
 * emit() in pieces as they are read. The hardware registers behind them clear
 * when read, so the driver is their only reader and this is the only path:
 * never read them through hw_addr_get(). Returns 0 when the file was read,
 * 1 when it cannot be opened (a kernel without per-flow counters). */
int hw_gpon_flow_counters_get(void (*emit)(const char *s, int n));

#endif
