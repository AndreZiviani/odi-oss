// SPDX-License-Identifier: GPL-2.0
/*
 * odi_i2c.c -- odi_i2c.h's own implementation.
 *
 * Register access is through odi_reg_read()/odi_reg_write(), the same
 * odi_switch MMIO primitive every other leaf in this driver set uses:
 * real MMIO under __KERNEL__ (odi_switch.c), the host register model
 * (test/odi_switch_mock.h) otherwise. The one place that model cannot
 * stand in for real hardware is the data register (I2C_READ_DATA): a real
 * I2C read answers with whatever byte sits at the address just latched,
 * but the mock's flat array can only ever return "whatever was last
 * written" to that address -- there is no device behind it to answer
 * with new data. So, host build only, the byte comes from
 * odi_i2c_mock_byte(), a table the test itself provides (keyed by
 * device select and address, both already recorded in the same call);
 * see test/odi_i2c_test.c. Every register WRITE this file makes still
 * goes through the real odi_reg_write() on both builds, so the setup-
 * write shape stays host-checkable the same way every other primitive
 * here is.
 */
#include "odi_i2c.h"
/*
 * odi_switch_reg.h: odi_reg_read()/odi_reg_write() prototypes, __KERNEL__
 * only -- a no-op on the host build; test/odi_switch_mock.h's own static
 * inline definitions (included ahead of this file in the unity-build
 * test .c) cover the host build instead, same posture as
 * odi_switch_sdkinit.c/odi_board.c.
 */
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/printk.h>
/*
 * linux/ratelimit.h: pr_info_ratelimited() needs DEFINE_RATELIMIT_STATE,
 * which this kernel's linux/printk.h does not pull in on its own.
 */
#include <linux/ratelimit.h>
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#ifndef __KERNEL__
/* Host build only: the byte the device would have answered with for this
 * (sel, addr) pair, provided by the test. Never defined for the target
 * build -- a link error there would mean this stub leaked into a real
 * kernel object, which must not happen.
 */
uint8_t odi_i2c_mock_byte(uint32_t sel, uint32_t addr);
#endif

/* Waits for IN_PROGRESS to clear: 0, -ENXIO when the device did not
 * acknowledge, -ETIMEDOUT. *last_cmd is the last I2C_CMD value read, for
 * the failure log. One byte at the bus speed takes on the order of 100 us,
 * so the bound is time, not reads: ODI_I2C_POLL_US in steps of
 * ODI_I2C_POLL_DELAY_US.
 */
static int odi_i2c_poll_done(uint32_t *last_cmd)
{
	int rc = odi_poll_reg(ODI_I2C_CMD, ODI_I2C_CMD_IN_PROGRESS, 0,
			      ODI_I2C_POLL_DELAY_US, ODI_I2C_POLL_US, last_cmd);

	if (rc)
		return -ETIMEDOUT;
	return (*last_cmd & ODI_I2C_CMD_NOT_ACKED) ? -ENXIO : 0;
}

int odi_i2c_read_bytes(uint32_t sel, uint32_t addr, uint8_t *out, unsigned int n)
{
	unsigned int i;
	int rc = 0;

	if (n == 0)
		return 0;

	/* Every byte is five steps on one controller, and the controller keeps
	 * no per-caller state: a second reader (the exporter DDM poll against
	 * a /proc/odi_omci ddm write, say) interleaved between them gets
	 * bytes meant for the other caller. odi_i2c_lock (odi_switch.c) holds
	 * the whole run, all n bytes, so a multi-byte field is also read in
	 * one piece. Process context only: the poll busy-waits in udelay()
	 * steps under a mutex, never under a spinlock.
	 */
	mutex_lock(&odi_i2c_lock);
	for (i = 0; i < n; i++) {
		uint32_t a = addr + i;

		/* Setup writes, in the capture's own order: re-assert the
		 * device select, then the target byte address. The capture
		 * reads each register before writing it (the old value is
		 * never used for anything); reproduced here for the same
		 * transaction shape, discarded the same way.
		 */
		(void)odi_reg_read(ODI_I2C_MASTER_SETUP);
		odi_reg_write(ODI_I2C_MASTER_SETUP, sel);
		(void)odi_reg_read(ODI_I2C_BYTE_ADDR);
		odi_reg_write(ODI_I2C_BYTE_ADDR, a);

		/* Start: START set, WRITE clear -- a read. */
		odi_reg_write(ODI_I2C_CMD, ODI_I2C_CMD_START);

		{
			uint32_t cmd_val;
			int poll_rc = odi_i2c_poll_done(&cmd_val);

			if (poll_rc != 0) {
#ifdef __KERNEL__
				if (cmd_val & ODI_I2C_CMD_NOT_ACKED)
					pr_info_ratelimited(
						"odi_i2c: nack sel=0x%08x addr=0x%08x cmd=0x%08x\n",
						sel, a, cmd_val);
				else
					pr_info_ratelimited(
						"odi_i2c: poll timeout sel=0x%08x addr=0x%08x cmd=0x%08x\n",
						sel, a, cmd_val);
#endif
				rc = poll_rc;
				break;
			}
		}

#ifdef __KERNEL__
		out[i] = (uint8_t)(odi_reg_read(ODI_I2C_READ_DATA) & 0xffU);
#else
		out[i] = odi_i2c_mock_byte(sel, a);
		(void)odi_reg_read(ODI_I2C_READ_DATA); /* touched for shape symmetry, unused */
#endif
	}
	mutex_unlock(&odi_i2c_lock);
	return rc;
}
