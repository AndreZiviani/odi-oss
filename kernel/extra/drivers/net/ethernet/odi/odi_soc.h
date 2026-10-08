/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_soc.h -- the SoC system-controller window: every access our drivers
 * make there goes through odi_soc_read()/odi_soc_write(), which refuse any
 * offset not on the one allowlist in odi_soc.c. An undecoded address in
 * this window can stall the bus until the watchdog resets the board
 * (docs/kb/rtl9602c-undecoded-address-read-takes-the-stick-down.md).
 *
 * Two users keep their own access, outside this file: the ramlog, which
 * runs before ioremap() exists (odi_ramlog.c), and the NIC DMA stop in
 * prom_init (arch/mips/rtl8686/board.c).
 */
#ifndef ODI_SOC_H
#define ODI_SOC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Physical 0x18000000; 16 KB covers every offset below. On this MIPS32
 * target ioremap() of an uncached address under 512 MB returns its KSEG1
 * alias, 0xb8000000, so the mapping costs nothing at run time.
 */
#define ODI_SOC_PHYS		0x18000000U
#define ODI_SOC_SIZE		0x4000U
#define ODI_SOC_KSEG1		0xb8000000U	/* the address the replay tables carry */

#define SOC_CLK_RST_EN		0x0044U	/* clock/reset enable, board init */
#define SOC_IP_EN		0x063cU	/* IP enable; bit 5 is the PON PBO block */
#define SOC_WDT_KICK		0x3260U	/* odi_wdt.h has the three WDT registers */
/* Watchdog interrupt/status: phase 1 pending at bit 31, phase 2 at bit 30.
 * Not read or written by the driver; allowlisted only. Decode cross-checked
 * against an independent driver for a later chip of the same family
 * (mainline realtek_otto_wdt); values unchanged.
 */
#define SOC_WDT_STATUS		0x3264U
#define SOC_WDT_CTRL		0x3268U
/* GPIO: bank 0 (A-D) has direction at 0x3308 and data at 0x330c; bank 1 has
 * its direction at 0x3324 and its data at 0x3328, the same layout 0x1c
 * further on. The bank 1 decode is cross-checked against an independent
 * driver for a later chip of the same family; those two are only replayed,
 * with the values the stock firmware writes, which are unchanged. Which pins
 * the replayed values drive is not decoded.
 */
#define SOC_GPIO_DIR		0x3308U	/* GPIO bank 0 direction, bit n = GPIO n, 1 = output */
#define SOC_GPIO_DATA		0x330cU	/* GPIO bank 0 data, bit n = GPIO n */
#define SOC_GPIO_B1_DIR		0x3324U	/* GPIO bank 1 direction; replayed only */
#define SOC_GPIO_B1_DATA	0x3328U	/* GPIO bank 1 data; replayed only */

/* Maps the window once, whoever asks first: board init, the watchdog and
 * the odi_init verbs each call it from their initcall. 0 or -ENODEV.
 */
int odi_soc_ensure(void);

/* An offset not on the allowlist, or a window not mapped, is logged and
 * refused: a read returns 0, a write stores nothing and returns -EPERM
 * (not mapped: -ENODEV).
 */
uint32_t odi_soc_read(uint32_t off);
int odi_soc_write(uint32_t off, uint32_t val);

#endif /* ODI_SOC_H */
