// SPDX-License-Identifier: GPL-2.0
/*
 * odi_ddm.c -- odi_ddm.h's own implementation: per-selector (device, byte
 * address, length) table plus the odi_i2c_read_bytes() call and the
 * output-buffer fill. See odi_ddm.h and odi_i2c.h for where each of these
 * addresses comes from.
 *
 * SFF-8472 layout used here: A0h identification bytes 20-35 (vendor
 * name) and 40-55 (part number), both 16-byte ASCII fields, space/NUL
 * padded; A2h DDMI bytes 96-97 (temperature), 98-99 (voltage), 100-101
 * (bias current), 102-103 (Tx power), 104-105 (Rx power), each a
 * big-endian uint16 -- public SFF-8472 layout. Confirmed
 * against a capture of a working stock DDM read: every byte address
 * this table names is the exact I2C_BYTE_ADDR value the capture writes
 * before the read that produced the matching output value.
 */
#include "odi_ddm.h"
#include "odi_i2c.h"

#ifdef __KERNEL__
#include <linux/printk.h>
/*
 * linux/ratelimit.h: pr_info_ratelimited() needs DEFINE_RATELIMIT_STATE,
 * which this kernel's linux/printk.h does not pull in on its own.
 */
#include <linux/ratelimit.h>
#include <linux/errno.h> /* odi_ddm_get's -EINVAL, below */
#else
#include <errno.h> /* same, host build */
#endif

/* SFF-8472 byte offsets and field lengths, the public standard layout --
 * <linux/sfp.h> names these same offsets (SFP_VENDOR_NAME, SFP_VENDOR_PN,
 * SFP_TEMP, SFP_VCC, SFP_TX_BIAS, SFP_TX_POWER, SFP_RX_POWER), but that
 * header is __KERNEL__-only and this file is host-tested as plain C
 * (test/odi_ddm_test.c), so the names are restated here rather than
 * pulled in, to keep one portable definition instead of a kernel-only
 * one plus a host-side fallback that could drift from it.
 */
#define ODI_DDM_SFF8472_A0_VENDOR_NAME_OFF	20U
#define ODI_DDM_SFF8472_A0_PART_NUMBER_OFF	40U
#define ODI_DDM_SFF8472_A0_STRING_LEN		16U
#define ODI_DDM_SFF8472_A2_TEMPERATURE_OFF	96U
#define ODI_DDM_SFF8472_A2_VOLTAGE_OFF		98U
#define ODI_DDM_SFF8472_A2_BIAS_CURRENT_OFF	100U
#define ODI_DDM_SFF8472_A2_TX_POWER_OFF	102U
#define ODI_DDM_SFF8472_A2_RX_POWER_OFF	104U
#define ODI_DDM_SFF8472_A2_FIELD_LEN		2U

/* Optional Status/Control (byte 110, bit 1 = RX_LOS) through the warning
 * flags (byte 117), read as one contiguous 8-byte run: 110-111 (status/
 * control, reserved), 112-113 (alarm flags), 114-115 (reserved), 116-117
 * (warning flags) -- public SFF-8472 layout, same section that already
 * documents the five numeric fields above.
 */
#define ODI_DDM_SFF8472_A2_ALARM_WARN_OFF	110U
#define ODI_DDM_SFF8472_A2_ALARM_WARN_LEN	8U

int odi_ddm_get(int type, uint8_t out[ODI_DDM_BUF_LEN])
{
	uint32_t sel, addr;
	unsigned int n, i;

	switch (type) {
	case ODI_DDM_VENDOR_NAME:
		sel = ODI_I2C_SEL_A0; addr = ODI_DDM_SFF8472_A0_VENDOR_NAME_OFF;
		n = ODI_DDM_SFF8472_A0_STRING_LEN;
		break;
	case ODI_DDM_PART_NUMBER:
		sel = ODI_I2C_SEL_A0; addr = ODI_DDM_SFF8472_A0_PART_NUMBER_OFF;
		n = ODI_DDM_SFF8472_A0_STRING_LEN;
		break;
	case ODI_DDM_TEMPERATURE:
		sel = ODI_I2C_SEL_A2; addr = ODI_DDM_SFF8472_A2_TEMPERATURE_OFF;
		n = ODI_DDM_SFF8472_A2_FIELD_LEN;
		break;
	case ODI_DDM_VOLTAGE:
		sel = ODI_I2C_SEL_A2; addr = ODI_DDM_SFF8472_A2_VOLTAGE_OFF;
		n = ODI_DDM_SFF8472_A2_FIELD_LEN;
		break;
	case ODI_DDM_BIAS_CURRENT:
		sel = ODI_I2C_SEL_A2; addr = ODI_DDM_SFF8472_A2_BIAS_CURRENT_OFF;
		n = ODI_DDM_SFF8472_A2_FIELD_LEN;
		break;
	case ODI_DDM_TX_POWER:
		sel = ODI_I2C_SEL_A2; addr = ODI_DDM_SFF8472_A2_TX_POWER_OFF;
		n = ODI_DDM_SFF8472_A2_FIELD_LEN;
		break;
	case ODI_DDM_RX_POWER:
		sel = ODI_I2C_SEL_A2; addr = ODI_DDM_SFF8472_A2_RX_POWER_OFF;
		n = ODI_DDM_SFF8472_A2_FIELD_LEN;
		break;
	case ODI_DDM_ALARM_STATUS:
		sel = ODI_I2C_SEL_A2; addr = ODI_DDM_SFF8472_A2_ALARM_WARN_OFF;
		n = ODI_DDM_SFF8472_A2_ALARM_WARN_LEN;
		break;
	default:
#ifdef __KERNEL__
		pr_info_ratelimited("odi_ddm: bad selector type=%d\n", type);
#endif
		return -EINVAL;
	}

	for (i = 0; i < ODI_DDM_BUF_LEN; i++)
		out[i] = 0;

	{
		int i2c_rc = odi_i2c_read_bytes(sel, addr, out, n);

		if (i2c_rc != 0) {
#ifdef __KERNEL__
			pr_info_ratelimited("odi_ddm: i2c read failed type=%d sel=0x%08x addr=%u len=%u\n",
					     type, sel, addr, n);
#endif
			return i2c_rc;
		}
	}
	return 0;
}
