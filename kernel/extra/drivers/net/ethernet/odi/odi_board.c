// SPDX-License-Identifier: GPL-2.0
/*
 * odi_board.c -- odi_board.h's own implementation. Two portable pieces
 * (odi_board_init() itself, and the tiny host/kernel log split every
 * other file in this driver set already carries), no kernel-only half
 * beyond the log macros: everything odi_board_init() calls
 * (odi_switch_mmio_ensure(), odi_reg_write(), odi_switch_soc_write()) is
 * already host-testable through test/odi_switch_mock.h, the same way
 * odi_switch_sdkinit_test.c exercises odi_switch_sdkinit_apply().
 */
#include "odi_board.h"
/*
 * odi_switch_reg.h: odi_reg_write()/odi_switch_mmio_ensure() prototypes,
 * __KERNEL__ only -- a no-op on the host build; test/odi_switch_mock.h's
 * own static inline definitions (included ahead of this file in the
 * unity-build test .c) cover the host build instead, same posture
 * as odi_switch_sdkinit.c.
 */
#include "odi_switch_reg.h"
/*
 * odi_switch_sdkinit.h: odi_switch_soc_write() -- the checked, allowlisted
 * SoC-window write this file reuses rather than keeping a second allowlist.
 */
#include "odi_switch_sdkinit.h"

#ifdef __KERNEL__
#include <linux/printk.h>
#define ODI_BOARD_LOG(fmt, ...) pr_info("odi_board: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define ODI_BOARD_LOG(fmt, ...) printf("odi_board: " fmt, ##__VA_ARGS__)
#endif

void odi_board_init(void)
{
	unsigned int i;

	/* Whichever of this device_initcall and odi_switch_init() the linker
	 * placed first, this call maps the switch-core MMIO window if it is
	 * not mapped already (odi_switch.c's own header comment on this
	 * function has the ordering argument) -- the board LED init must
	 * never assume odi_switch_init() already ran.
	 */
	if (odi_switch_mmio_ensure() != 0) {
		ODI_BOARD_LOG("MMIO not mapped, board init replay skipped\n");
		return;
	}

	for (i = 0; i < odi_board_init_event_count; i++) {
		const struct odi_sw_modload_event *e = &odi_board_init_events[i];

		if (e->kind == ODI_SW_MODLOAD_SOC)
			(void)odi_switch_soc_write(e->offset, e->value);
		else
			odi_reg_write(e->offset, e->value);
	}

	ODI_BOARD_LOG("board init replay: %u events applied\n", odi_board_init_event_count);
}
