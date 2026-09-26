/* Parses the first line of /proc/odi_gpon (kernel/extra/drivers/net/ethernet/
 * odi/odi_gpon.c: "state %d\n", odi_gpon_onu_state()), the ONU FSM state.
 *
 * Split out of hw.c so the parsing itself builds and is tested natively: it
 * touches no syscalls, same reasoning as ddm.c/ddm.h (hw_gpon_status_get()
 * is the syscall-using half, MIPS-only, and hands this the raw bytes it read
 * from the file).
 */
#ifndef ODI_GPON_STATUS_H
#define ODI_GPON_STATUS_H

#include <stdint.h>

/* buf need not be NUL-terminated; len is the byte count the read returned
 * (short reads are fine, only the first line matters). Returns 0
 * and fills *state (0-7, per gpon_state_name()) on a well-formed "state N"
 * first line; -1 otherwise -- wrong prefix, no digits, or N out of range,
 * which the caller treats the same as the file being absent. */
int gpon_proc_parse_state(const char *buf, unsigned len, uint32_t *state);

/* The alarms our own GPON driver can answer for, as bits of the mask
 * gpon_alarms_from_sts() and hw_gpon_alarms_get() return. They are the
 * three live condition bits of GPON_GTC_DS_INTR_STS (0x701008; the fourth,
 * bit 2, is FEC state, not an alarm). SF, SD and the two TX alarms are not
 * tracked by odi_gpon at all, so there is no bit for them. */
#define GPON_ALARM_LOS  (1u << 0)
#define GPON_ALARM_LOF  (1u << 1)
#define GPON_ALARM_LOM  (1u << 3)
#define GPON_ALARM_KNOWN (GPON_ALARM_LOS | GPON_ALARM_LOF | GPON_ALARM_LOM)

static inline uint32_t gpon_alarms_from_sts(uint32_t sts)
{
	return sts & GPON_ALARM_KNOWN;
}

/* The "last_los_ms <ms> state <0|1>" line of /proc/odi_gpon: the DS LOS bit
 * as the driver last sampled it, at its most recent interrupt. The live
 * register is better (it needs no interrupt to have happened); this is the
 * fallback for when /dev/odi_sw cannot be read. Returns 0 and *los (0 or
 * 1) on a well-formed line anywhere in buf, -1 otherwise. */
int gpon_proc_parse_los(const char *buf, unsigned len, uint32_t *los);

#endif
