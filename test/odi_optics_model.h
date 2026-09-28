/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_optics_model.h -- a scriptable SFF-8472 transceiver, host build only,
 * sitting behind the same modelled I2C controller test/odi_switch_mock.h
 * already gives odi_i2c.c: two 256-byte pages (A0h identity, A2h DDMI) plus
 * the IO_GPIO_EN routing gate (docs/kb/dfp34x-optics-on-i2c-port1-gated-
 * by-io-gpio-en.md) and an "absent module" mode, both surfaced as I2C NACK
 * exactly the way the real device does.
 *
 * Uses the one hook test/odi_switch_mock.h already provides for this
 * purpose (odi_mock_write_hook, the same seam odi_switch_l2_test.c uses to
 * make TABLE_CMD answer like the lookup table): installed once via
 * odi_optics_model_reset(), it watches every I2C_CMD write and, before the
 * driver's own odi_i2c_poll_done() ever reads it back, decides whether this
 * transaction is acknowledged.
 *
 * odi_i2c.c's host build never reads a byte value through this model
 * (odi_i2c.c's own header comment explains why: the mock's flat register
 * array cannot answer "whatever byte sits at the address just latched"
 * with fresh data every read) -- it still calls odi_i2c_mock_byte(), the
 * same per-test hook test/odi_i2c_test.c and test/odi_ddm_test.c already
 * define. A test built against this model defines odi_i2c_mock_byte()
 * to read the modelled pages below instead of a fixed table, so a whole
 * odi_ddm_get() call is served by one consistent device rather than by
 * per-field mock entries.
 *
 * Alarm and warning flags (A2h bytes 112/113, 116/117) and the RX_LOS
 * status bit (byte 110) are set directly by the scenario, not derived from
 * threshold math here: a real SFF-8472 module computes these itself against
 * its own internal calibration and exposes only the resulting bits, which
 * is exactly what odi_ddm_get()'s ODI_DDM_ALARM_STATUS selector reads
 * (odi_ddm.c). A scenario that sets rx-power to a low value and also
 * asserts the matching alarm bit is stating what the module would report
 * at that power level, the same way test/odi_ddm_test.c's existing tables
 * state a capture's raw bytes rather than recomputing them.
 */
#ifndef ODI_OPTICS_MODEL_H
#define ODI_OPTICS_MODEL_H

#ifdef __KERNEL__
#error "host-only: never build this into a kernel object"
#endif

#include <string.h>

/* Pulled in by every test that uses this model, ahead of this header:
 * odi_switch_mock.h (odi_mock, odi_mock_write_hook, odi_mock_slot()),
 * odi_switch_hw.h (ODI_SW_PIN_GPIO_SELECT), odi_i2c.h (ODI_I2C_* register
 * and select constants).
 */

/* PIN_GPIO_SELECT value odi_board_optics() writes at boot (odi_board.c);
 * restated here as a plain data value, not included from odi_board.c,
 * the same "capture, not vendor source" posture odi_i2c.h and odi_ddm.c
 * both already take for their own constants.
 */
#define ODI_OPTICS_GPIO_SELECT_VALUE 0x08082001U

struct odi_optics_model {
	uint8_t a0[256];
	uint8_t a2[256];
	int present;		/* 0: every transaction NACKs, module unplugged */
	int gpio_routed;	/* mirrors whatever PIN_GPIO_SELECT currently holds */
};

static struct odi_optics_model odi_optics;

/* ODI_I2C_CMD write hook: decides NACK vs acknowledge before
 * odi_i2c_poll_done() (odi_i2c.c) ever reads the register back. Runs after
 * odi_reg_write() has already stored the driver's own value, so it only
 * needs to patch NOT_ACKED/IN_PROGRESS in place. */
static void odi_optics_write_hook(uint32_t off, uint32_t val)
{
	uint32_t gpio, cmd;
	int nack;

	if (off != ODI_I2C_CMD || !(val & ODI_I2C_CMD_START))
		return;

	gpio = odi_mock.regs[odi_mock_slot(ODI_SW_PIN_GPIO_SELECT(0))];
	odi_optics.gpio_routed = (gpio == ODI_OPTICS_GPIO_SELECT_VALUE);
	nack = !odi_optics.present || !odi_optics.gpio_routed;

	cmd = val & ~(uint32_t)ODI_I2C_CMD_IN_PROGRESS;
	if (nack)
		cmd |= ODI_I2C_CMD_NOT_ACKED;
	else
		cmd &= ~(uint32_t)ODI_I2C_CMD_NOT_ACKED;
	odi_mock.regs[odi_mock_slot(ODI_I2C_CMD)] = cmd;
}

/* Vendor name / part number, the same isp1 capture strings every other
 * host test in this tree uses (test/odi_ddm_test.c), so a scenario that
 * never touches identity still reads back something real. */
static void odi_optics_model_defaults(void)
{
	memset(&odi_optics, 0, sizeof odi_optics);
	memset(odi_optics.a0, ' ', sizeof odi_optics.a0);
	memset(odi_optics.a2, 0, sizeof odi_optics.a2);
	memcpy(&odi_optics.a0[20], "ODI", 3);
	memcpy(&odi_optics.a0[40], "DFP-34X-2C2", 11);
	odi_optics.present = 1;
	odi_optics.gpio_routed = 0; /* the test scripts the GPIO write itself */
}

/* Resets the model to its defaults and (re)installs the write hook. Call
 * once per test, after odi_mock_reset() (which clears odi_mock_write_hook
 * along with every register). */
static void odi_optics_model_reset(void)
{
	odi_optics_model_defaults();
	odi_mock_write_hook = odi_optics_write_hook;
}

static void odi_optics_set_present(int present)
{
	odi_optics.present = present;
}

/* raw is the big-endian SFF-8472 A2h field value (see odi_ddm.c's own
 * offsets): temperature/voltage/bias/tx-power/rx-power all live at their
 * documented two-byte offset. */
static void odi_optics_set_a2_word(unsigned int off, uint16_t raw)
{
	odi_optics.a2[off] = (uint8_t)(raw >> 8);
	odi_optics.a2[off + 1] = (uint8_t)raw;
}

/* alarms/warnings: bit 15 down to bit 6 across the two-byte pair, same
 * layout src/diag/src/commands.c's cmd_transceiver_alarms() decodes.
 * los: A2h byte 110 bit 1 (SFF-8472 Optional Status/Control). */
static void odi_optics_set_alarms(uint16_t alarms, uint16_t warnings, int los)
{
	odi_optics.a2[110] = los ? 0x02 : 0x00;
	odi_optics.a2[112] = (uint8_t)(alarms >> 8);
	odi_optics.a2[113] = (uint8_t)alarms;
	odi_optics.a2[116] = (uint8_t)(warnings >> 8);
	odi_optics.a2[117] = (uint8_t)warnings;
}

/* odi_i2c.c's host-build hook: the byte the device would answer with for
 * (sel, addr), served from whichever modelled page sel selects. */
uint8_t odi_i2c_mock_byte(uint32_t sel, uint32_t addr)
{
	const uint8_t *page = (sel == ODI_I2C_SEL_A0) ? odi_optics.a0 : odi_optics.a2;

	if (addr >= 256U) {
		fprintf(stderr, "odi_optics_model: address %u out of range\n", addr);
		abort();
	}
	return page[addr];
}

#endif /* ODI_OPTICS_MODEL_H */
