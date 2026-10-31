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
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#define ODI_BOARD_LOG(fmt, ...) pr_info("odi_board: " fmt, ##__VA_ARGS__)
#define ODI_BOARD_SLEEP_MS(ms)	msleep(ms)
/* The GPIO data word is shared by every pin: the laser verb (process
 * context) and the O7 gate (GPON interrupt and timers) read-modify-write it.
 */
static DEFINE_SPINLOCK(odi_board_gpio_lock);
#define ODI_BOARD_GPIO_LOCK(f)		spin_lock_irqsave(&odi_board_gpio_lock, f)
#define ODI_BOARD_GPIO_UNLOCK(f)	spin_unlock_irqrestore(&odi_board_gpio_lock, f)
#else
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define ODI_BOARD_LOG(fmt, ...) printf("odi_board: " fmt, ##__VA_ARGS__)
#define ODI_BOARD_SLEEP_MS(ms)	((void)(ms))
#define ODI_BOARD_GPIO_LOCK(f)		((void)(f))
#define ODI_BOARD_GPIO_UNLOCK(f)	((void)(f))
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

#define ODI_BOARD_GPIO_TX_DISABLE	13U

static struct odi_board_laser odi_board_laser_state = { .laser_on = 0, .ldd_id = -1 };

void odi_board_laser_get(struct odi_board_laser *out)
{
	*out = odi_board_laser_state;
}

/* A write to A2h byte 127 makes some laser drivers stop acknowledging for
 * a while (another open implementation measured this on the same part),
 * and the byte is backed by the driver's non-volatile memory, so a write
 * that changes nothing is wear and a stall for no gain. Writes the page
 * select only when `cur` (what it reads now) differs from `want`, then
 * waits for it to read back as written: the read-back is also the "answers
 * again" test. Bounded: ODI_BOARD_PAGE_TRIES reads ODI_BOARD_PAGE_WAIT_MS
 * apart, 200 ms in all. 0, or the last error.
 */
#define ODI_BOARD_PAGE_TRIES		20
#define ODI_BOARD_PAGE_WAIT_MS		10

static int odi_board_select_page(uint8_t want, uint8_t cur)
{
	int i, rc;

	if (cur == want)
		return 0;
	rc = odi_i2c_write_byte(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, want);
	if (rc)
		return rc;
	for (i = 0; i < ODI_BOARD_PAGE_TRIES; i++) {
		rc = odi_i2c_read_bytes(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, &cur, 1);
		if (!rc && cur == want)
			return 0;
		if (!rc)
			rc = -EIO;
		ODI_BOARD_SLEEP_MS(ODI_BOARD_PAGE_WAIT_MS);
	}
	return rc;
}

/* The id byte of the laser driver behind A2h: select page 2 (when it is
 * not already), read byte 0xa0, put the page select back. -1 when any step
 * fails. *rest is the page to put back: taken from the first attempt that
 * reads the select byte and kept for the next ones, because a failed
 * attempt can leave page 2 selected and a retry would otherwise take that
 * for the page at rest.
 */
static int odi_board_ldd_id_once(int *rest)
{
	uint8_t page, id;
	int rc;

	if (odi_i2c_read_bytes(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, &page, 1))
		return -1;
	if (*rest < 0)
		*rest = page;
	if (odi_board_select_page(ODI_BOARD_LDD_ID_PAGE, page))
		return -1;
	rc = odi_i2c_read_bytes(ODI_I2C_SEL_A2, ODI_BOARD_LDD_ID_OFF, &id, 1);
	(void)odi_board_select_page((uint8_t)*rest, ODI_BOARD_LDD_ID_PAGE);
	return rc ? -1 : (int)id;
}

/* The module can stop acknowledging for a while (around an EEPROM copy, or
 * after a page-select write), and one failed read here leaves the laser off
 * and GPON refused for the whole boot. So a failed read is tried again,
 * bounded: ODI_BOARD_LDD_TRIES attempts ODI_BOARD_LDD_WAIT_MS apart. A read
 * that succeeds with another id is not retried: that is an answer.
 */
#define ODI_BOARD_LDD_TRIES	3
#define ODI_BOARD_LDD_WAIT_MS	200

static int odi_board_ldd_id(void)
{
	int rest = -1, id = -1, i;

	for (i = 0; i < ODI_BOARD_LDD_TRIES; i++) {
		if (i)
			ODI_BOARD_SLEEP_MS(ODI_BOARD_LDD_WAIT_MS);
		id = odi_board_ldd_id_once(&rest);
		if (id >= 0)
			return id;
		ODI_BOARD_LOG("laser: laser driver id read failed (try %d of %d)\n",
			      i + 1, ODI_BOARD_LDD_TRIES);
	}
	/* Gave up: leave the page select where it was found. */
	if (rest >= 0) {
		uint8_t cur;

		if (!odi_i2c_read_bytes(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, &cur, 1))
			(void)odi_board_select_page((uint8_t)rest, cur);
	}
	return -1;
}

/* Set and cleared by the host-side SFP EEPROM under its own lock, read here
 * under odi_switch_lock: a plain word, one store and one load. A verb that
 * reads it just before the mirror takes the pin writes bit 27 back; the
 * mirror finds that at its next refresh and clears it again.
 */
static int odi_board_sfp_pin_held;

void odi_board_sfp_slave_pin(int held)
{
#ifdef __KERNEL__
	WRITE_ONCE(odi_board_sfp_pin_held, held);
#else
	odi_board_sfp_pin_held = held;
#endif
}

int odi_board_optics(void)
{
	uint32_t v = ODI_BOARD_PIN_GPIO_SELECT;

#ifdef __KERNEL__
	if (READ_ONCE(odi_board_sfp_pin_held))
#else
	if (odi_board_sfp_pin_held)
#endif
		v &= ~ODI_BOARD_PIN_SFP_SLAVE;
	odi_reg_write(ODI_SW_PIN_GPIO_SELECT(0), v);
	ODI_BOARD_LOG("optics: PIN_GPIO_SELECT 0x%08x\n", (unsigned int)v);
	return 0;
}

int odi_board_laser(const char *arg)
{
	const uint32_t bit = 1U << ODI_BOARD_GPIO_TX_DISABLE;
	int unverified = arg && !strcmp(arg, "unverified");
	unsigned long flags = 0;
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
	ODI_BOARD_GPIO_LOCK(flags);
	rc = odi_soc_write(SOC_GPIO_DATA, odi_soc_read(SOC_GPIO_DATA) & ~bit);
	if (!rc)
		rc = odi_soc_write(SOC_GPIO_DIR, odi_soc_read(SOC_GPIO_DIR) | bit);
	odi_board_laser_state.laser_on = !rc;
	odi_board_laser_state.emergency_stop = 0;
	ODI_BOARD_GPIO_UNLOCK(flags);
	ODI_BOARD_LOG("laser: laser driver id 0x%02x%s, GPIO %u low: dir 0x%08x dat 0x%08x, rc %d\n",
		      (unsigned int)(id < 0 ? 0xff : id),
		      id == (int)ODI_BOARD_LDD_ID_GN25L95 ? " (GN25L95)" : " (UNVERIFIED, forced on)",
		      ODI_BOARD_GPIO_TX_DISABLE,
		      (unsigned int)odi_soc_read(SOC_GPIO_DIR),
		      (unsigned int)odi_soc_read(SOC_GPIO_DATA), rc);
	return rc;
}

int odi_board_tx_disable(int disable)
{
	const uint32_t bit = 1U << ODI_BOARD_GPIO_TX_DISABLE;
	unsigned long flags = 0;
	int rc = 0;

	ODI_BOARD_GPIO_LOCK(flags);
	if (!odi_board_laser_state.laser_on) {
		/* The laser verb never drove the line (an unknown laser driver,
		 * or the verb not run): it stays an input, the laser off. Not
		 * an error: there is nothing to assert, and nothing to release.
		 */
		ODI_BOARD_GPIO_UNLOCK(flags);
		return 0;
	}
	rc = odi_soc_write(SOC_GPIO_DATA, disable ? (odi_soc_read(SOC_GPIO_DATA) | bit)
						  : (odi_soc_read(SOC_GPIO_DATA) & ~bit));
	if (!rc)
		odi_board_laser_state.emergency_stop = disable ? 1 : 0;
	ODI_BOARD_GPIO_UNLOCK(flags);
	ODI_BOARD_LOG("laser: %s (GPIO %u %s), rc %d\n",
		      disable ? "OFF, emergency stop (O7)" : "on again, emergency stop left",
		      ODI_BOARD_GPIO_TX_DISABLE, disable ? "high" : "low", rc);
	return rc;
}
