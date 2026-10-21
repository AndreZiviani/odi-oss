/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_hwmon.h -- the SoC die temperature sensor, read-only.
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
 * Pure decode, shared with the host test (test/odi_hwmon_test.c).
 */
#ifndef ODI_HWMON_H
#define ODI_HWMON_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#else
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

#endif /* ODI_HWMON_H */
