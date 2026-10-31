/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_hwmon.h -- the SoC die temperature sensor, read-only, and its alarms.
 *
 * Two words of the switch-core register file (odi_reg_read(), offsets from
 * ODI_SWITCH_MMIO_BASE, the same offset space as diag register get):
 *
 *   0x138  control: bit 16 enables the sensor. The loader leaves it enabled
 *          (0x00034ad7 on a booted stick), and so does the stock firmware.
 *   0x13c  status: bits 22:16 hold the reading in degrees C.
 *
 * Nothing here writes either word. The scale and the offset are inferred,
 * not calibrated: the reading was 62 C on a stick whose optics module read
 * 46 C, and the stock firmware reads 63 to 65 C in the same state. Treat
 * the value as a die temperature good to a few degrees, a trend more than
 * a measurement.
 *
 * The alarm limits (temp1_max, temp1_crit) are soft: they only raise the
 * hwmon alarm flags and one log line per crossing. Nothing powers off or
 * throttles, because the scale above is not verified. The defaults, 95 C
 * and 110 C, match what another open implementation's measurements chose
 * for the same chip, for the same reason.
 *
 * Pure decode and alarm state machine, shared with the host test
 * (test/odi_hwmon_test.c).
 */
#ifndef ODI_HWMON_H
#define ODI_HWMON_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#else
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#endif

#define ODI_THERM_CTRL_OFF	0x000138U
#define ODI_THERM_STATUS_OFF	0x00013cU
#define ODI_THERM_CTRL_ENABLE	(1U << 16)
#define ODI_THERM_READ_SHIFT	16
#define ODI_THERM_READ_MASK	0x7fU

/* Millidegrees C from the two words, or -ENODATA when the sensor is not
 * enabled (the status word then holds nothing meaningful).
 */
static inline int odi_therm_decode(uint32_t ctrl, uint32_t status, long *mdeg)
{
	if (!(ctrl & ODI_THERM_CTRL_ENABLE))
		return -ENODATA;
	*mdeg = (long)((status >> ODI_THERM_READ_SHIFT) & ODI_THERM_READ_MASK) * 1000L;
	return 0;
}

#define ODI_THERM_MAX_DEFAULT	95000L	/* millidegrees C */
#define ODI_THERM_CRIT_DEFAULT	110000L
#define ODI_THERM_LIMIT_MIN	0L
#define ODI_THERM_LIMIT_MAX	150000L
#define ODI_THERM_HYST		2000L	/* an alarm clears this far below its limit */

/* Transition bits reported by odi_therm_alarm_sample(). */
#define ODI_THERM_EV_MAX_SET	0x1U
#define ODI_THERM_EV_MAX_CLR	0x2U
#define ODI_THERM_EV_CRIT_SET	0x4U
#define ODI_THERM_EV_CRIT_CLR	0x8U

struct odi_therm_alarms {
	long max;	/* millidegrees C */
	long crit;
	bool max_alarm;
	bool crit_alarm;
};

static inline void odi_therm_alarms_init(struct odi_therm_alarms *a)
{
	a->max = ODI_THERM_MAX_DEFAULT;
	a->crit = ODI_THERM_CRIT_DEFAULT;
	a->max_alarm = false;
	a->crit_alarm = false;
}

/* Limits stay within 0..150000 and max <= crit; -EINVAL leaves them as
 * they were. Raising max above the current crit needs crit raised first.
 */
static inline int odi_therm_set_max(struct odi_therm_alarms *a, long v)
{
	if (v < ODI_THERM_LIMIT_MIN || v > ODI_THERM_LIMIT_MAX || v > a->crit)
		return -EINVAL;
	a->max = v;
	return 0;
}

static inline int odi_therm_set_crit(struct odi_therm_alarms *a, long v)
{
	if (v < ODI_THERM_LIMIT_MIN || v > ODI_THERM_LIMIT_MAX || v < a->max)
		return -EINVAL;
	a->crit = v;
	return 0;
}

/* One alarm: set at reading >= limit, cleared at reading <= limit - hysteresis,
 * unchanged in between.
 */
static inline bool odi_therm_alarm_next(bool now, long mdeg, long limit)
{
	if (mdeg >= limit)
		return true;
	if (mdeg <= limit - ODI_THERM_HYST)
		return false;
	return now;
}

/* Decode one reading and update both alarms from it. A reading that is not
 * valid (-ENODATA) changes nothing and reports no event. *events gets the
 * ODI_THERM_EV_* transitions of this call.
 */
static inline int odi_therm_alarm_sample(struct odi_therm_alarms *a,
					 uint32_t ctrl, uint32_t status,
					 long *mdeg, unsigned int *events)
{
	bool m, c;
	int rc = odi_therm_decode(ctrl, status, mdeg);

	*events = 0;
	if (rc)
		return rc;
	m = odi_therm_alarm_next(a->max_alarm, *mdeg, a->max);
	c = odi_therm_alarm_next(a->crit_alarm, *mdeg, a->crit);
	if (m != a->max_alarm)
		*events |= m ? ODI_THERM_EV_MAX_SET : ODI_THERM_EV_MAX_CLR;
	if (c != a->crit_alarm)
		*events |= c ? ODI_THERM_EV_CRIT_SET : ODI_THERM_EV_CRIT_CLR;
	a->max_alarm = m;
	a->crit_alarm = c;
	return 0;
}

#endif /* ODI_HWMON_H */
