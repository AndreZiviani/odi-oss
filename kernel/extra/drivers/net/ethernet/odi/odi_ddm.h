/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_ddm.h -- the seven SFF-8472 DDM readings RTK_OPT_TRANSCEIVER used to
 * answer, now read directly over the SoC I2C controller (odi_i2c.c)
 * instead of the stock sockopt: on the stock firmware that sockopt only
 * answers after its own i2c/i2cen odi_init verbs have run, and
 * this image replays those as captured data (odi_switch_sdkinit.c), so the
 * stock DDM path refuses up front, with no register access at all.
 *
 * Selector values mirror src/diag/src/hw.h's enum ddm_sel 0-6 exactly (the
 * two trees do not share a header, same reasoning odi_reg.h/odi_sw_ioctl.h
 * give for the ioctl ABI -- see odi_reg.h). RTK_DDM_SN, the stock sockopt's
 * own selector 7, is not implemented here: the capture never exercises it,
 * and the module serial number is an A0h read (bytes 68-83) with no
 * captured reference for it -- src/diag has no command that reads it, and
 * ODI_DDM_ALARM_STATUS below reuses the value 7 for something else
 * entirely (this driver set's own selector, not the stock sockopt's one),
 * since the stock sockopt this replaces is gone from this tree entirely
 * and never shared its numbering with ODI_SW_IOC_DDM_GET in the first
 * place.
 */
/* Guard is ODI_SW_DDM_H, not ODI_DDM_H: src/diag/src/ddm.h (a different
 * tree, the formatter this file's own output feeds) already claims
 * ODI_DDM_H, and a host test needs both headers included in one
 * translation unit (test/odi_ddm_test.c) -- a shared guard would silently
 * drop whichever one loses the race.
 */
#ifndef ODI_SW_DDM_H
#define ODI_SW_DDM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define ODI_DDM_VENDOR_NAME  0
#define ODI_DDM_PART_NUMBER  1
#define ODI_DDM_TEMPERATURE  2
#define ODI_DDM_VOLTAGE      3
#define ODI_DDM_BIAS_CURRENT 4
#define ODI_DDM_TX_POWER     5
#define ODI_DDM_RX_POWER     6

/* Alarm/warning flags plus the optical LOS status bit, all in one selector:
 * a single contiguous 8-byte A2h read (offsets 110-117) rather than a
 * separate transaction per condition. Not one of the seven RTK_OPT_TRANSCEIVER
 * mirrors above (that ABI has no such selector), so this is a value of our
 * own, past ODI_DDM_RX_POWER; src/diag/src/hw.h's DDM_SEL_ALARM_STATUS
 * mirrors it the same way the 0-6 selectors mirror each other. Output layout
 * (see odi_ddm.c for the byte offsets this comes from): out[0] = A2h byte 110
 * (bit 1 = RX_LOS asserted), out[2]/out[3] = A2h bytes 112/113 (alarm flags:
 * temp/voltage/bias/tx-power high+low in byte 112, rx-power high+low in the
 * top two bits of byte 113), out[6]/out[7] = A2h bytes 116/117 (the same
 * layout, warning flags). out[1], out[4], out[5] are the reserved bytes the
 * read also carries; unused.
 */
#define ODI_DDM_ALARM_STATUS 7

/* The output buffer size, same value everywhere it is repeated in this
 * driver set (odi_reg.h's struct odi_sw_ddm, odi_omci.c's "ddm" /proc
 * command) -- fixed by the RTK_OPT_TRANSCEIVER +0x28 ABI this replaces
 * (src/diag/src/hw.h), not a free choice, so this is a name for that
 * value, not a new one.
 */
#define ODI_DDM_BUF_LEN 24U

/* Fills ODI_DDM_BUF_LEN bytes of raw DDMI the same way
 * RTK_OPT_TRANSCEIVER's own +0x28 output did (src/diag/src/hw.h), so
 * src/diag/src/ddm.c's ddm_format() needs no change at all: ASCII in
 * out[0..15] for the two string types (space/NUL padded, matching what
 * SFF-8472 actually stores), a big-endian uint16 in out[0..1] for the
 * five numeric types, the rest zeroed. Returns 0 on success, -1 for an
 * unsupported type or an I2C read that never completed.
 */
int odi_ddm_get(int type, uint8_t out[ODI_DDM_BUF_LEN]);

#endif /* ODI_SW_DDM_H */
