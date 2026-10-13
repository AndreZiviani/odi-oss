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
#include "odi_i2c.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/string.h>
#define ODI_BOARD_LOG(fmt, ...) pr_info("odi_board: " fmt, ##__VA_ARGS__)
#else
#include <errno.h>
#include <stdio.h>
#include <string.h>
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

static struct odi_board_laser odi_board_laser_state = { .laser_on = 0, .ldd_id = -1 };

void odi_board_laser_get(struct odi_board_laser *out)
{
	*out = odi_board_laser_state;
}

/* The id byte of the laser driver behind A2h: select page 2, read byte
 * 0xa0, put the page select back. -1 when any step fails.
 */
static int odi_board_ldd_id(void)
{
	uint8_t page, id;
	int rc;

	if (odi_i2c_read_bytes(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, &page, 1))
		return -1;
	if (odi_i2c_write_byte(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, ODI_BOARD_LDD_ID_PAGE))
		return -1;
	rc = odi_i2c_read_bytes(ODI_I2C_SEL_A2, ODI_BOARD_LDD_ID_OFF, &id, 1);
	if (page != ODI_BOARD_LDD_ID_PAGE)
		(void)odi_i2c_write_byte(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, page);
	return rc ? -1 : (int)id;
}

int odi_board_optics(void)
{
	odi_reg_write(ODI_SW_PIN_GPIO_SELECT(0), ODI_BOARD_PIN_GPIO_SELECT);
	ODI_BOARD_LOG("optics: PIN_GPIO_SELECT 0x%08x\n", (unsigned int)ODI_BOARD_PIN_GPIO_SELECT);
	return 0;
}

int odi_board_laser(const char *arg)
{
	const uint32_t bit = 1U << ODI_BOARD_GPIO_TX_DISABLE;
	int unverified = arg && !strcmp(arg, "unverified");
	int rc, id;

	id = odi_board_ldd_id();
	odi_board_laser_state.ldd_id = id;
	odi_board_laser_state.unverified = unverified;
	if (id != (int)ODI_BOARD_LDD_ID_GN25L95 && !unverified) {
		odi_board_laser_state.laser_on = 0;
		if (id < 0)
			ODI_BOARD_LOG("laser: no answer from the laser driver id byte; laser left OFF, GPON activation refused\n");
		else
			ODI_BOARD_LOG("laser: laser driver id 0x%02x is not the GN25L95 (0x%02x) this image was validated with; laser left OFF, GPON activation refused (ODI_LASER_UNVERIFIED=1 overrides, only on an OLT port with no other ONU)\n",
				      (unsigned int)id, (unsigned int)ODI_BOARD_LDD_ID_GN25L95);
		return -ENODEV;
	}

	/* The level first, then the direction, so the pin never drives high. */
	rc = odi_soc_write(SOC_GPIO_DATA, odi_soc_read(SOC_GPIO_DATA) & ~bit);
	if (!rc)
		rc = odi_soc_write(SOC_GPIO_DIR, odi_soc_read(SOC_GPIO_DIR) | bit);
	odi_board_laser_state.laser_on = !rc;
	ODI_BOARD_LOG("laser: laser driver id 0x%02x%s, GPIO %u low: dir 0x%08x dat 0x%08x, rc %d\n",
		      (unsigned int)(id < 0 ? 0xff : id),
		      id == (int)ODI_BOARD_LDD_ID_GN25L95 ? " (GN25L95)" : " (UNVERIFIED, forced on)",
		      ODI_BOARD_GPIO_TX_DISABLE,
		      (unsigned int)odi_soc_read(SOC_GPIO_DIR),
		      (unsigned int)odi_soc_read(SOC_GPIO_DATA), rc);
	return rc;
}
