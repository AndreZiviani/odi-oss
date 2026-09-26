// SPDX-License-Identifier: GPL-2.0
/*
 * odi_board.c -- odi_board_init(), odi_board.h has the contract. Host
 * testable: everything it calls is modelled by test/odi_switch_mock.h.
 */
#include "odi_board.h"
#include "odi_replay_blob.h"
#include "odi_switch_reg.h"
#include "odi_soc.h"
#include "odi_switch_hw.h"

#ifdef __KERNEL__
#include <linux/printk.h>
#define ODI_BOARD_LOG(fmt, ...) pr_info("odi_board: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define ODI_BOARD_LOG(fmt, ...) printf("odi_board: " fmt, ##__VA_ARGS__)
#endif

void odi_board_init(void)
{
	const struct odi_replay_blob board = {
		.events = odi_board_init_events,
		.count = odi_board_init_event_count,
	};
	const struct odi_replay_opts opts = { .rmw = 0, .verb = ODI_REPLAY_ALL_VERBS };

	/* The board code that calls this links before odi_switch_init()
	 * (the odi Makefile), so neither window may be mapped yet.
	 */
	if (odi_switch_mmio_ensure() != 0 || odi_soc_ensure() != 0) {
		ODI_BOARD_LOG("MMIO not mapped, board init replay skipped\n");
		return;
	}
	(void)odi_replay_run(&board, &opts);
	ODI_BOARD_LOG("board init replay: %u events applied\n", odi_board_init_event_count);
}

#define ODI_BOARD_PIN_GPIO_SELECT	0x08082001U
#define ODI_BOARD_GPIO_TX_DISABLE	13U

int odi_board_optics(void)
{
	const uint32_t bit = 1U << ODI_BOARD_GPIO_TX_DISABLE;
	int rc;

	odi_reg_write(ODI_SW_PIN_GPIO_SELECT(0), ODI_BOARD_PIN_GPIO_SELECT);
	/* The level first, then the direction, so the pin never drives high. */
	rc = odi_soc_write(SOC_GPIO_DATA, odi_soc_read(SOC_GPIO_DATA) & ~bit);
	if (!rc)
		rc = odi_soc_write(SOC_GPIO_DIR, odi_soc_read(SOC_GPIO_DIR) | bit);
	ODI_BOARD_LOG("optics: PIN_GPIO_SELECT 0x%08x, GPIO %u low: dir 0x%08x dat 0x%08x, rc %d\n",
		      (unsigned int)ODI_BOARD_PIN_GPIO_SELECT, ODI_BOARD_GPIO_TX_DISABLE,
		      (unsigned int)odi_soc_read(SOC_GPIO_DIR),
		      (unsigned int)odi_soc_read(SOC_GPIO_DATA), rc);
	return rc;
}
