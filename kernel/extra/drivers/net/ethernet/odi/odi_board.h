/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_board.h -- CONFIG_ODI_BOARD (depends on CONFIG_ODI_INTR): the
 * board-init switch-core/LED/I2C setup, done as a single ordered
 * register/SoC-window write replay captured from a clean stock boot
 * (isp1-260923-g4-boot.txt, the "== 2.94 boot" window: 88 entries,
 * write-only, nothing dropped, ending right before the first
 * `/proc/rtk_init` verb), in place of the proprietary core/I2C/LED init
 * calls the stock board code makes at that point.
 *
 * With this in place, board init references no proprietary symbol.
 *
 * LED runtime behaviour: the captured LED configuration writes select
 * which link states (10/100/500/1000 Mb/s, ACT) light the two hardware LED
 * ports (15 = UTP0/FE-LAN, 2 = UTP1/GE-LAN) -- purely a one-time hardware
 * config of the switch's own LED controller, which then blinks the LEDs
 * off link/activity state autonomously in silicon. Nothing else this
 * codebase ships (kernel/extra, rootfs/skeleton) touches the LED
 * controller again after board init, so there is no runtime software LED
 * toggle to preserve: replaying this capture's own writes reproduces the
 * ENTIRE LED behaviour, not just its boot state.
 *
 * I2C: the I2C core is first set up here, at board init, BEFORE
 * rcS's own `i2c`/`i2cen` /proc/rtk_init steps (odi_switch_sdkinit.c,
 * CONFIG_ODI_SDKINIT) replay their own captured state later -- two
 * separate entry points into the same I2C core from two different
 * callers. odi_board_init() below replays the board-init pair's own two
 * SoC writes (0xb8000044 then 0xb8003324/0xb8003328) verbatim and in
 * order, leaving the controller in exactly the state the capture shows at
 * this point in boot; the `i2c`/`i2cen` sdkinit replay downstream is
 * unaffected -- it replays its OWN window of the same capture,
 * independent of this one.
 */
#ifndef ODI_BOARD_H
#define ODI_BOARD_H

#include "odi_switch_dal.h" /* struct odi_sw_modload_event */

/* odi_board_init_events[]/_count: the 88-entry ordered replay, HAND
 * TRANSCRIBED (not machine-generated -- small and fixed enough that a
 * generator would be more machinery than the data needs) from
 * isp1-260923-g4-boot.txt's own "== 2.94 boot" window,
 * odi_board_data.c.
 */
extern const struct odi_sw_modload_event odi_board_init_events[];
extern const unsigned int odi_board_init_event_count;

/* odi_board_init() -- applies odi_board_init_events[] in order (REG kind
 * through odi_reg_write(), after odi_switch_mmio_ensure(); SOC kind
 * through odi_switch_soc_write(), the sdkinit allowlist mechanism), then
 * logs one line (event count). Never fails outward: a refused SoC write
 * (not on the allowlist) is logged by odi_switch_soc_write() itself and
 * skipped, the same posture odi_switch_sdkinit_apply() already has --
 * losing one write costs that write, not the boot. Called once from
 * the 6.18 port board code (arch/mips/rtl8686/board.c,
 * kernel/618/patches/0002-mips-rtl8686-board.patch).
 */
void odi_board_init(void);

#endif /* ODI_BOARD_H */
