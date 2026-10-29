// SPDX-License-Identifier: GPL-2.0
/*
 * odi_sfp_mirror.c -- odi_sfp_mirror.h's implementation: the fill, the
 * enable, the refresh, and (kernel build) the request state and the
 * delayed work behind odi_switch.sfp_mirror.
 *
 * The core above the __KERNEL__ block is plain C over odi_reg_read()/
 * odi_reg_write()/odi_poll_reg() and odi_i2c_read_bytes(), so the host test
 * runs it against the register model (test/odi_sfp_mirror_test.c).
 *
 * Behaviour follows a stock v08 stick as measured from its host (the
 * router's dump of A0h and A2h): A0h as the module has it, A2h with the
 * thresholds and calibration of the module and its live readings, kept
 * current. Here A2h 96-119 (the five diagnostics, the status/control byte
 * 110 whose bits 7, 2 and 1 are the module's own TX_DISABLE, TX_FAULT and
 * RX_LOS state, and the alarm and warning flags) is refreshed once a
 * second.
 */
#include "odi_sfp_mirror.h"
#include "odi_board.h"
#include "odi_i2c.h"
#include "odi_ddm.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#define ODI_SFPM_INFO(fmt, ...)	pr_info("odi_sfp_mirror: " fmt "\n", ##__VA_ARGS__)
#define ODI_SFPM_WARN(fmt, ...)	pr_warn("odi_sfp_mirror: " fmt "\n", ##__VA_ARGS__)
#else
#include <errno.h>
#include <string.h>
/* Host build: the test provides the log, so it can count the lines. */
void odi_sfpm_log(int warn, const char *fmt, ...);
#define ODI_SFPM_INFO(fmt, ...)	odi_sfpm_log(0, fmt, ##__VA_ARGS__)
#define ODI_SFPM_WARN(fmt, ...)	odi_sfpm_log(1, fmt, ##__VA_ARGS__)
#endif

_Static_assert(ODI_SFPM_PIN_SEL_OFF == ODI_SW_PIN_GPIO_SELECT_BASE, "PIN_GPIO_SELECT moved");
_Static_assert(ODI_SFPM_PIN_SEL_SFP_SLAVE == ODI_BOARD_PIN_SFP_SLAVE, "SFP slave pin moved");

static int odi_sfpm_access(uint32_t cmd, uint32_t *last)
{
	odi_reg_write(ODI_SFPM_ACCESS_OFF, cmd);
	return odi_poll_reg(ODI_SFPM_ACCESS_OFF, ODI_SFPM_ACCESS_CMD, 0,
			    ODI_SFPM_ACCESS_POLL_DELAY_US, ODI_SFPM_ACCESS_POLL_US, last) ?
		-ETIMEDOUT : 0;
}

int odi_sfpm_sram_write(unsigned int addr, uint8_t val)
{
	uint32_t last;

	return odi_sfpm_access(ODI_SFPM_ACCESS_CMD | ODI_SFPM_ACCESS_WRITE |
			       ((addr & ODI_SFPM_ACCESS_ADDR_MASK) << ODI_SFPM_ACCESS_ADDR_SHIFT) |
			       ((uint32_t)val << ODI_SFPM_ACCESS_WDATA_SHIFT), &last);
}

int odi_sfpm_sram_read(unsigned int addr, uint8_t *val)
{
	uint32_t last;
	int rc = odi_sfpm_access(ODI_SFPM_ACCESS_CMD |
				 ((addr & ODI_SFPM_ACCESS_ADDR_MASK) << ODI_SFPM_ACCESS_ADDR_SHIFT),
				 &last);

	if (!rc)
		*val = (uint8_t)(last & ODI_SFPM_ACCESS_RDATA_MASK);
	return rc;
}

/* Sets and clears bits of one register; writes only when that changes it. */
static void odi_sfpm_rmw(uint32_t off, uint32_t set, uint32_t clr)
{
	uint32_t v = odi_reg_read(off);
	uint32_t n = (v & ~clr) | set;

	if (n != v)
		odi_reg_write(off, n);
}

static bool odi_sfpm_enabled(uint32_t *ctrl, uint32_t *io, uint32_t *pin)
{
	*ctrl = odi_reg_read(ODI_SFPM_CTRL_OFF);
	*io = odi_reg_read(ODI_SFPM_IO_MODE_OFF);
	*pin = odi_reg_read(ODI_SFPM_PIN_SEL_OFF);
	return (*ctrl & ODI_SFPM_CTRL_SLAVE_EN) && (*io & ODI_SFPM_IO_MODE_SLAVE_PINS) &&
	       !(*pin & ODI_SFPM_PIN_SEL_SFP_SLAVE);
}

/* The order of odi_sfp_mirror.h: slave, pin mode, then the pin itself. */
static void odi_sfpm_enable(void)
{
	odi_sfpm_rmw(ODI_SFPM_CTRL_OFF, ODI_SFPM_CTRL_SLAVE_EN, 0);
	odi_sfpm_rmw(ODI_SFPM_IO_MODE_OFF, ODI_SFPM_IO_MODE_SLAVE_PINS, 0);
	odi_sfpm_rmw(ODI_SFPM_PIN_SEL_OFF, 0, ODI_SFPM_PIN_SEL_SFP_SLAVE);
}

void odi_sfpm_stop(struct odi_sfpm *m)
{
	if (m->pin_held) {
		odi_board_sfp_slave_pin(0);
		odi_sfpm_rmw(ODI_SFPM_PIN_SEL_OFF,
			     m->pin_was_gpio ? ODI_SFPM_PIN_SEL_SFP_SLAVE : 0, 0);
		m->pin_held = 0;
	}
	odi_sfpm_rmw(ODI_SFPM_IO_MODE_OFF, 0, ODI_SFPM_IO_MODE_SLAVE_PINS);
	odi_sfpm_rmw(ODI_SFPM_CTRL_OFF, 0, ODI_SFPM_CTRL_SLAVE_EN);
}

int odi_sfpm_read_live(uint8_t live[ODI_SFPM_A2_LIVE_LEN])
{
	static const int words[] = {
		ODI_DDM_TEMPERATURE, ODI_DDM_VOLTAGE, ODI_DDM_BIAS_CURRENT,
		ODI_DDM_TX_POWER, ODI_DDM_RX_POWER,
	};
	uint8_t buf[ODI_DDM_BUF_LEN];
	unsigned int i;
	int rc;

	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		rc = odi_ddm_get(words[i], buf);
		if (rc)
			return rc;
		live[2U * i] = buf[0];
		live[2U * i + 1U] = buf[1];
	}
	/* 106-119: reserved and optional words, status/control, the flags. */
	return odi_i2c_read_bytes(ODI_I2C_SEL_A2, ODI_SFPM_A2_LIVE_FIRST + 10U, live + 10,
				  ODI_SFPM_A2_LIVE_LEN - 10U);
}

/* Bytes the self-check reads back once the slave is on: identifier,
 * diagnostic type, enhanced options, the temperature word and byte 110.
 */
static const uint16_t odi_sfpm_check_bytes[] = {
	0, 92, ODI_SFPM_A0_OPTIONS, ODI_SFPM_A2(96), ODI_SFPM_A2(97), ODI_SFPM_A2(110),
};

int odi_sfpm_start(struct odi_sfpm *m)
{
	uint8_t a0[256], a2[256], live[ODI_SFPM_A2_LIVE_LEN];
	uint32_t ctrl, io, pin;
	unsigned int i;
	bool page0;
	int rc;

	m->pin_held = 0;

	/* Each page in one run: one hold of odi_i2c_lock, so nothing (a page
	 * select through /dev/i2c-0) lands in the middle of the copy.
	 */
	rc = odi_i2c_read_bytes(ODI_I2C_SEL_A0, 0, a0, 256);
	if (!rc)
		rc = odi_i2c_read_bytes(ODI_I2C_SEL_A2, 0, a2, 256);
	if (!rc)
		rc = odi_sfpm_read_live(live);
	if (rc) {
		ODI_SFPM_WARN("module EEPROM read failed (%d): host EEPROM not enabled", rc);
		return rc;
	}
	memcpy(a2 + ODI_SFPM_A2_LIVE_FIRST, live, sizeof(live));
	page0 = a2[ODI_SFPM_A2_PAGE_SELECT] == 0;
	if (!page0)
		ODI_SFPM_WARN("A2h page %u was selected: the host gets A2h 128-255 empty",
			      a2[ODI_SFPM_A2_PAGE_SELECT]);
	{
		uint8_t opt = a0[ODI_SFPM_A0_OPTIONS];

		if (odi_sfpm_a0_adjust(a0))
			ODI_SFPM_INFO("A0h byte 93 0x%02x served as 0x%02x: the host cannot write the controls it advertises",
				      opt, a0[ODI_SFPM_A0_OPTIONS]);
	}
	odi_sfpm_a2_adjust(a2, page0);
	memcpy(m->img, a0, 256);
	memcpy(m->img + 256, a2, 256);

	for (i = 0; i < ODI_SFPM_SIZE; i++) {
		rc = odi_sfpm_sram_write(i, m->img[i]);
		if (rc) {
			ODI_SFPM_WARN("SRAM write of byte %u did not complete: host EEPROM not enabled", i);
			return rc;
		}
	}
	for (i = 0; i < ODI_SFPM_SIZE; i++) {
		uint8_t v;

		rc = odi_sfpm_sram_read(i, &v);
		if (rc) {
			ODI_SFPM_WARN("SRAM read of byte %u did not complete: host EEPROM not enabled", i);
			return rc;
		}
		if (v != m->img[i]) {
			ODI_SFPM_WARN("SRAM byte %u reads 0x%02x, 0x%02x written: host EEPROM not enabled",
				      i, v, m->img[i]);
			return -EIO;
		}
	}
	for (i = 0; i < ODI_SFPM_HOST_RO_WORDS; i++)
		odi_reg_write(ODI_SFPM_HOST_RO_OFF + 4U * i, odi_sfpm_host_ro_word(i));

	m->pin_was_gpio = !!(odi_reg_read(ODI_SFPM_PIN_SEL_OFF) & ODI_SFPM_PIN_SEL_SFP_SLAVE);
	m->pin_held = 1;
	odi_board_sfp_slave_pin(1);
	odi_sfpm_enable();

	rc = odi_sfpm_enabled(&ctrl, &io, &pin) ? 0 : -EIO;
	if (rc)
		ODI_SFPM_WARN("enable bits did not stick (0x2ac 0x%08x, 0x23018 0x%08x, 0x48 0x%08x)",
			      ctrl, io, pin);
	for (i = 0; !rc && i < sizeof(odi_sfpm_check_bytes) / sizeof(odi_sfpm_check_bytes[0]); i++) {
		unsigned int a = odi_sfpm_check_bytes[i];
		uint8_t v = 0;

		rc = odi_sfpm_sram_read(a, &v);
		if (!rc && v != m->img[a])
			rc = -EIO;
		if (rc)
			ODI_SFPM_WARN("self-check: SRAM byte %u reads 0x%02x, 0x%02x written (%d)",
				      a, v, m->img[a], rc);
	}
	if (rc) {
		odi_sfpm_stop(m);
		ODI_SFPM_WARN("host EEPROM disabled again after the failed self-check");
		return rc;
	}
	m->warned = 0;
	ODI_SFPM_INFO("host EEPROM on: the module A0h/A2h, diagnostic type 0x%02x, options 0x%02x, A2h 96-119 refreshed every %u ms",
		      m->img[92], m->img[ODI_SFPM_A0_OPTIONS], ODI_SFPM_REFRESH_MS);
	return 0;
}

int odi_sfpm_refresh(struct odi_sfpm *m)
{
	uint8_t live[ODI_SFPM_A2_LIVE_LEN];
	uint32_t ctrl, io, pin;
	unsigned int i;
	int rc = odi_sfpm_read_live(live);

	if (rc) {
		if (!(m->warned & ODI_SFPM_W_READ))
			ODI_SFPM_WARN("module A2h read failed (%d): the host keeps the last values", rc);
		m->warned |= ODI_SFPM_W_READ;
	} else {
		unsigned int a = 0;
		int wrc = 0;

		if (m->warned & ODI_SFPM_W_READ)
			ODI_SFPM_INFO("module A2h answers again");
		m->warned &= ~ODI_SFPM_W_READ;
		for (i = 0; i < ODI_SFPM_A2_LIVE_LEN; i++) {
			a = ODI_SFPM_A2(ODI_SFPM_A2_LIVE_FIRST + i);
			if (live[i] == m->img[a])
				continue;
			wrc = odi_sfpm_sram_write(a, live[i]);
			if (wrc)
				break;
			m->img[a] = live[i];
		}
		if (wrc && !(m->warned & ODI_SFPM_W_SRAM))
			ODI_SFPM_WARN("SRAM write of byte %u did not complete: the host may read stale values", a);
		else if (!wrc && (m->warned & ODI_SFPM_W_SRAM))
			ODI_SFPM_INFO("SRAM writes complete again");
		if (wrc)
			m->warned |= ODI_SFPM_W_SRAM;
		else
			m->warned &= ~ODI_SFPM_W_SRAM;
	}

	if (!odi_sfpm_enabled(&ctrl, &io, &pin)) {
		if (!(m->warned & ODI_SFPM_W_LOST))
			ODI_SFPM_WARN("an enable bit was undone (0x2ac 0x%08x, 0x23018 0x%08x, 0x48 0x%08x): set again",
				      ctrl, io, pin);
		m->warned |= ODI_SFPM_W_LOST;
		odi_sfpm_enable();
	} else {
		m->warned &= ~ODI_SFPM_W_LOST;
	}
	return rc;
}

#ifdef __KERNEL__
/* ---- the request, its state and the refresh work.
 *
 * odi_sfpm_lock (mutex, process context) protects the state below, the
 * image, and the SFP slave registers (the CPU access register is one
 * command at a time). It is taken by the work and by a request; it nests
 * outside odi_i2c_lock (the module reads) and never with odi_switch_lock.
 * odi_sfpm_req_lock serialises requests, so that the cancel of an off
 * cannot fall after the start of the on that follows it.
 */
enum { ODI_SFPM_OFF, ODI_SFPM_FILLING, ODI_SFPM_ON, ODI_SFPM_FAILED };

static const char *const odi_sfpm_state_names[] = {
	[ODI_SFPM_OFF] = "off",
	[ODI_SFPM_FILLING] = "filling",
	[ODI_SFPM_ON] = "on",
	[ODI_SFPM_FAILED] = "failed",
};

static DEFINE_MUTEX(odi_sfpm_lock);
static DEFINE_MUTEX(odi_sfpm_req_lock);
static struct odi_sfpm odi_sfpm;
static int odi_sfpm_st = ODI_SFPM_OFF;
static bool odi_sfpm_want;

static void odi_sfpm_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(odi_sfpm_work, odi_sfpm_work_fn);

static void odi_sfpm_work_fn(struct work_struct *w)
{
	bool again;

	mutex_lock(&odi_sfpm_lock);
	if (odi_sfpm_want && odi_sfpm_st == ODI_SFPM_FILLING)
		odi_sfpm_st = odi_sfpm_start(&odi_sfpm) ? ODI_SFPM_FAILED : ODI_SFPM_ON;
	else if (odi_sfpm_want && odi_sfpm_st == ODI_SFPM_ON)
		odi_sfpm_refresh(&odi_sfpm);
	again = odi_sfpm_want && odi_sfpm_st == ODI_SFPM_ON;
	mutex_unlock(&odi_sfpm_lock);
	if (again)
		schedule_delayed_work(&odi_sfpm_work, msecs_to_jiffies(ODI_SFPM_REFRESH_MS));
}

void odi_sfp_mirror_request(bool on)
{
	bool kick = false;

	mutex_lock(&odi_sfpm_req_lock);
	mutex_lock(&odi_sfpm_lock);
	odi_sfpm_want = on;
	if (on && (odi_sfpm_st == ODI_SFPM_OFF || odi_sfpm_st == ODI_SFPM_FAILED)) {
		odi_sfpm_st = ODI_SFPM_FILLING;
		kick = true;
	} else if (!on) {
		if (odi_sfpm_st == ODI_SFPM_ON) {
			odi_sfpm_stop(&odi_sfpm);
			ODI_SFPM_INFO("host EEPROM off");
		}
		odi_sfpm_st = ODI_SFPM_OFF;
	}
	mutex_unlock(&odi_sfpm_lock);
	/* The work takes odi_sfpm_lock: never wait for it while holding it.
	 * Once off, a run that was waiting finds nothing to do and does not
	 * schedule itself again.
	 */
	if (kick)
		mod_delayed_work(system_wq, &odi_sfpm_work, 0);
	else if (!on)
		cancel_delayed_work_sync(&odi_sfpm_work);
	mutex_unlock(&odi_sfpm_req_lock);
}

bool odi_sfp_mirror_requested(void)
{
	return READ_ONCE(odi_sfpm_want);
}

const char *odi_sfp_mirror_state(void)
{
	return odi_sfpm_state_names[READ_ONCE(odi_sfpm_st)];
}
#endif /* __KERNEL__ */
