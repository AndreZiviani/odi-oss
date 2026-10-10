/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_i2c_test.c -- host-side unit test for odi_i2c.c's
 * odi_i2c_read_bytes(): the register-write shape (I2C_MASTER_SETUP re-asserted,
 * I2C_BYTE_ADDR incrementing once per byte, I2C_CMD.START set) against
 * test/odi_switch_mock.h's write log, and the returned bytes against
 * odi_i2c_mock_byte() -- this test's own table, sourced from a capture of
 * a working stock-firmware DDM read (the vendor-name and temperature brackets:
 * the first covers a multi-byte ASCII run at the A0h identification
 * address, the second the two-byte A2h numeric read every other DDM
 * field shares the same shape as).
 *
 * odi_i2c_write_byte(): the page select write shape (device select, data,
 * address, I2C_CMD START|WRITE), and the refusal of every other target
 * before any register is touched. odi_i2c_read_bytes() also refuses a run
 * past byte 255.
 *
 * The busy-poll timeout path is not exercised here: the host
 * odi_poll_reg() (test/odi_switch_mock.h) finishes every operation at
 * once, and the START write leaves IN_PROGRESS clear in the mock.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_i2c.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_i2c.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* The mock byte table odi_i2c.c's host build calls into. Keyed by (sel,
 * addr); a miss aborts loudly rather than returning silent garbage.
 */
struct mock_entry { uint32_t sel; uint32_t addr; uint8_t val; };

static const struct mock_entry *mock_table;
static unsigned int mock_table_n;

uint8_t odi_i2c_mock_byte(uint32_t sel, uint32_t addr)
{
	unsigned int i;

	for (i = 0; i < mock_table_n; i++) {
		if (mock_table[i].sel == sel && mock_table[i].addr == addr)
			return mock_table[i].val;
	}
	fprintf(stderr, "odi_i2c_test: no mock byte for sel=0x%08x addr=%u\n",
		sel, addr);
	abort();
}

/* Vendor-name bytes 20-22, A0h -- "ODI" (regtrace bracket "== ddm
 * vendor-name": ADR 20/21/22 read back 0x4f/0x44/0x49).
 */
static const struct mock_entry vendor_name3[] = {
	{ ODI_I2C_SEL_A0, 20, 0x4f },
	{ ODI_I2C_SEL_A0, 21, 0x44 },
	{ ODI_I2C_SEL_A0, 22, 0x49 },
};

/* Temperature bytes 96-97, A2h -- 0x264b (regtrace bracket "== ddm
 * temperature": ADR 96/97 read back 0x26/0x4b, ddm_format() renders this
 * as "38.292969 C").
 */
static const struct mock_entry temperature2[] = {
	{ ODI_I2C_SEL_A2, 96, 0x26 },
	{ ODI_I2C_SEL_A2, 97, 0x4b },
};

int main(void)
{
	uint8_t out[3];
	int rc;

	puts("odi_i2c_read_bytes: vendor-name shape (A0h, 3 bytes at address 20):");
	odi_mock_reset();
	mock_table = vendor_name3;
	mock_table_n = sizeof vendor_name3 / sizeof vendor_name3[0];
	memset(out, 0xaa, sizeof out);
	rc = odi_i2c_read_bytes(ODI_I2C_SEL_A0, 20, out, 3);
	CHECK(rc == 0, "returns 0");
	CHECK(out[0] == 0x4f && out[1] == 0x44 && out[2] == 0x49,
	      "bytes come back 'O' 'D' 'I', in order");
	{
		/* Every write this primitive makes for 3 bytes: (CONFIG, ADR,
		 * CMD) per byte, matching the capture's own per-byte shape
		 * (I2C_MASTER_SETUP re-asserted every byte, not just once per run)
		 * -- 3*3 = 9 W entries, no other kind.
		 */
		unsigned int i, cfg_seen = 0;
		uint32_t last_adr = 0xffffffffU;
		int adr_increments = 1;

		CHECK(odi_mock.log_n == 9, "exactly 9 register writes logged");
		for (i = 0; i < odi_mock.log_n; i++) {
			struct odi_mock_write *e = &odi_mock.log[i];

			CHECK(e->kind == 'W', "every logged entry is a plain write");
			if (e->addr == ODI_I2C_MASTER_SETUP) {
				CHECK(e->val == ODI_I2C_SEL_A0, "I2C_MASTER_SETUP carries the A0h select");
				cfg_seen++;
			} else if (e->addr == ODI_I2C_BYTE_ADDR) {
				if (last_adr != 0xffffffffU && e->val != last_adr + 1)
					adr_increments = 0;
				last_adr = e->val;
			} else if (e->addr == ODI_I2C_CMD) {
				CHECK(e->val == ODI_I2C_CMD_START,
				      "I2C_CMD write is START only (a read, WRITE clear)");
			} else {
				CHECK(0, "unexpected write address");
			}
		}
		CHECK(cfg_seen == 3, "I2C_MASTER_SETUP re-asserted every byte, matching the capture");
		CHECK(adr_increments, "I2C_BYTE_ADDR increments by one every byte");
		CHECK(last_adr == 22, "the last address written is the third byte, 22");
	}

	puts("odi_i2c_read_bytes: temperature shape (A2h, 2 bytes at address 96):");
	odi_mock_reset();
	mock_table = temperature2;
	mock_table_n = sizeof temperature2 / sizeof temperature2[0];
	{
		uint8_t t[2] = { 0xaa, 0xaa };

		rc = odi_i2c_read_bytes(ODI_I2C_SEL_A2, 96, t, 2);
		CHECK(rc == 0, "returns 0");
		CHECK(t[0] == 0x26 && t[1] == 0x4b, "bytes come back 0x26 0x4b (v=0x264b)");
	}
	{
		unsigned int i, cfg_seen = 0;

		CHECK(odi_mock.log_n == 6, "2*(CONFIG+ADR+CMD) = 6 writes for 2 bytes");
		for (i = 0; i < odi_mock.log_n; i++) {
			if (odi_mock.log[i].addr == ODI_I2C_MASTER_SETUP) {
				CHECK(odi_mock.log[i].val == ODI_I2C_SEL_A2,
				      "I2C_MASTER_SETUP carries the A2h select");
				cfg_seen++;
			}
		}
		CHECK(cfg_seen == 2, "I2C_MASTER_SETUP re-asserted every byte, matching the capture");
	}

	/* odi_i2c_read_bytes() holds odi_i2c_lock for the whole run; the host
	 * lock model (odi_switch_mock.h) aborts on a second acquire, so two
	 * reads back to back also prove it was released.
	 */
	CHECK(odi_mock_locks_idle(), "odi_i2c_lock released on return");

	puts("odi_i2c_write_byte: the page select, A2h byte 127:");
	odi_mock_reset();
	rc = odi_i2c_write_byte(ODI_I2C_SEL_A2, ODI_I2C_A2_PAGE_SELECT, 0x04);
	CHECK(rc == 0, "returns 0");
	CHECK(odi_mock.log_n == 4, "4 register writes: select, data, address, command");
	if (odi_mock.log_n == 4) {
		CHECK(odi_mock.log[0].addr == ODI_I2C_MASTER_SETUP &&
		      odi_mock.log[0].val == ODI_I2C_SEL_A2, "I2C_MASTER_SETUP carries the A2h select");
		CHECK(odi_mock.log[1].addr == ODI_I2C_WRITE_DATA &&
		      odi_mock.log[1].val == 0x04, "I2C_WRITE_DATA carries the byte");
		CHECK(odi_mock.log[2].addr == ODI_I2C_BYTE_ADDR &&
		      odi_mock.log[2].val == ODI_I2C_A2_PAGE_SELECT, "I2C_BYTE_ADDR is 127");
		CHECK(odi_mock.log[3].addr == ODI_I2C_CMD &&
		      odi_mock.log[3].val == (ODI_I2C_CMD_START | ODI_I2C_CMD_WRITE),
		      "I2C_CMD write is START|WRITE, and it comes last");
	}
	CHECK(odi_mock_locks_idle(), "odi_i2c_lock released after a write");

	puts("odi_i2c_write_byte: every other target is refused untouched:");
	{
		static const struct { uint32_t sel, addr; } refused[] = {
			{ ODI_I2C_SEL_A2, 0x7e },		/* the byte below the page select */
			{ ODI_I2C_SEL_A2, 0x80 },		/* the paged upper half */
			{ ODI_I2C_SEL_A2, 0x00 },
			{ ODI_I2C_SEL_A0, ODI_I2C_A2_PAGE_SELECT },	/* same byte, the A0h EEPROM */
			{ ODI_I2C_SETUP(0x52), ODI_I2C_A2_PAGE_SELECT },
		};
		unsigned int i;

		for (i = 0; i < sizeof refused / sizeof refused[0]; i++) {
			odi_mock_reset();
			rc = odi_i2c_write_byte(refused[i].sel, refused[i].addr, 0x02);
			CHECK(rc == -EPERM, "refused with -EPERM");
			CHECK(odi_mock.log_n == 0, "and no register written");
		}
	}

	puts("odi_i2c_read_bytes: a run past byte 255 is refused untouched:");
	{
		uint8_t b[8];

		odi_mock_reset();
		rc = odi_i2c_read_bytes(ODI_I2C_SEL_A2, 250, b, 8);
		CHECK(rc == -EINVAL, "250 + 8 bytes: -EINVAL");
		rc = odi_i2c_read_bytes(ODI_I2C_SEL_A2, 0x100, b, 1);
		CHECK(rc == -EINVAL, "address 256: -EINVAL");
		CHECK(odi_mock.log_n == 0, "no register written");
		CHECK(odi_mock_locks_idle(), "odi_i2c_lock never taken");
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
