/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_sfp_mirror_test.c -- host test of the host-side SFP EEPROM
 * (odi_sfp_mirror.c), against the register model (test/odi_switch_mock.h)
 * with two devices behind it:
 *
 *   - the module on the optics bus, A0h and A2h, through odi_i2c.c's host
 *     hook odi_i2c_mock_byte(); a NACK is the I2C_CMD write answered with
 *     NOT_ACKED;
 *   - the SoC slave's SRAM, behind the CPU access register: a write hook
 *     runs each command (store WDATA at ADDR, or put the byte in RDATA) and
 *     clears CMD, as the hardware does when the byte is done.
 *
 * What is checked: the fill (A0h as the module has it, A2h with the
 * password entry and the page select zeroed, the upper half only from page
 * 0), the order (no write to either enable register before all 512 SRAM
 * bytes and the 16 write protect words), the write protect pattern, the A0h
 * byte 93 adjustment and its checksum, the enable and disable as bit-exact
 * read-modify-writes of 0x2ac bit 0 and 0x23018 bit 23 (disable in the
 * reverse order), the refresh of A2h 96-119 (changed bytes only), a failed
 * module read logged once and its end once, an enable bit cleared behind
 * the mirror's back, and every failure of the start: nothing enabled, or
 * enabled and cleared again.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_i2c.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_i2c.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_ddm.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_ddm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_sfp_mirror.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_sfp_mirror.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* ---- the module */
static uint8_t mod_a0[256], mod_a2[256];
static int mod_nack;

uint8_t odi_i2c_mock_byte(uint32_t sel, uint32_t addr)
{
	if (sel == ODI_I2C_SEL_A0)
		return mod_a0[addr & 0xffU];
	if (sel == ODI_I2C_SEL_A2)
		return mod_a2[addr & 0xffU];
	fprintf(stderr, "odi_sfp_mirror_test: read of an unexpected device 0x%08x\n", sel);
	abort();
}

/* ---- the SRAM */
static uint8_t sram[ODI_SFPM_SIZE];
static unsigned int sram_writes, ro_writes;
static unsigned char sram_written[ODI_SFPM_SIZE];
static int sram_stuck;		/* every SRAM command times out */
static int sram_bad_addr = -1;	/* this byte stores the complement */
static int corrupt_on_enable;	/* the pin enable changes SRAM byte 0 */
static int enable_too_early;	/* an enable register written before the fill was complete */
static unsigned int enable_writes;

static unsigned int distinct_written(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < ODI_SFPM_SIZE; i++)
		n += sram_written[i];
	return n;
}

static void hook(uint32_t off, uint32_t val)
{
	if (off == ODI_I2C_CMD && mod_nack && (val & ODI_I2C_CMD_START)) {
		odi_mock.regs[odi_mock_slot(off)] |= ODI_I2C_CMD_NOT_ACKED;
		return;
	}
	if (off == ODI_SFPM_ACCESS_OFF && (val & ODI_SFPM_ACCESS_CMD)) {
		unsigned int a = (val >> ODI_SFPM_ACCESS_ADDR_SHIFT) & ODI_SFPM_ACCESS_ADDR_MASK;

		if (sram_stuck) {
			odi_mock.poll_stuck = 1;
			return;
		}
		if (val & ODI_SFPM_ACCESS_WRITE) {
			uint8_t d = (uint8_t)(val >> ODI_SFPM_ACCESS_WDATA_SHIFT);

			sram[a] = (int)a == sram_bad_addr ? (uint8_t)~d : d;
			sram_written[a] = 1;
			sram_writes++;
		}
		odi_mock.regs[odi_mock_slot(off)] = (val & ~ODI_SFPM_ACCESS_CMD &
						     ~ODI_SFPM_ACCESS_RDATA_MASK) | sram[a];
		return;
	}
	if (off >= ODI_SFPM_HOST_RO_OFF && off < ODI_SFPM_HOST_RO_OFF + 4U * ODI_SFPM_HOST_RO_WORDS) {
		ro_writes++;
		return;
	}
	if (off == ODI_SFPM_CTRL_OFF || off == ODI_SFPM_IO_MODE_OFF) {
		enable_writes++;
		if (distinct_written() < ODI_SFPM_SIZE || ro_writes < ODI_SFPM_HOST_RO_WORDS)
			enable_too_early = 1;
		if (off == ODI_SFPM_IO_MODE_OFF && (val & ODI_SFPM_IO_MODE_SLAVE_PINS) &&
		    corrupt_on_enable)
			sram[0] ^= 0x55U;
	}
}

/* ---- the log */
static unsigned int n_info, n_warn;
static char last_line[256];

void odi_sfpm_log(int warn, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(last_line, sizeof(last_line), fmt, ap);
	va_end(ap);
	if (warn)
		n_warn++;
	else
		n_info++;
	if (getenv("V"))
		printf("  log %s: %s\n", warn ? "warn" : "info", last_line);
}

/* A module as a GN25L95 stick shows it: identity, diagnostics implemented
 * and internally calibrated (92 = 0x68), alarm flags and soft RX_LOS (93 =
 * 0x90), valid checksums; A2h thresholds, calibration constants and live
 * readings as a stock v08 router dump shows them; page 0 selected; the
 * password bytes not zero, so that their zeroing shows.
 */
static void module_reset(void)
{
	static const uint8_t thr[] = {
		0x64, 0x00, 0xd8, 0x00, 0x5f, 0x00, 0xec, 0x00, 0x8c, 0xa0, 0x71, 0x48,
		0x88, 0xb8, 0x75, 0x30, 0xaf, 0xc8, 0x01, 0xf4, 0x9c, 0x40, 0x05, 0xdc,
		0x9b, 0x83, 0x1f, 0x07, 0x7b, 0x87, 0x27, 0x10, 0x07, 0xcb, 0x00, 0x0d,
		0x06, 0x31, 0x00, 0x10,
	};
	static const uint8_t live[] = {
		0x15, 0xb0, 0x7d, 0x5d, 0x15, 0x18, 0x53, 0x50, 0x00, 0x61, 0xff, 0xff,
		0x00, 0x00, 0x08, 0x00,
	};
	unsigned int i, sum;

	for (i = 0; i < 256; i++) {
		mod_a0[i] = (uint8_t)(i * 7U + 3U);
		mod_a2[i] = (uint8_t)(i * 13U + 1U);
	}
	mod_a0[0] = 0x03;
	mod_a0[92] = 0x68;
	mod_a0[93] = 0x90;
	for (sum = 0, i = 0; i < 63; i++)
		sum += mod_a0[i];
	mod_a0[63] = (uint8_t)sum;
	for (sum = 0, i = 64; i < 95; i++)
		sum += mod_a0[i];
	mod_a0[95] = (uint8_t)sum;
	memcpy(mod_a2, thr, sizeof(thr));
	memcpy(mod_a2 + 96, live, sizeof(live));
	memset(mod_a2 + 112, 0, 8);
	mod_a2[123] = 0x11; mod_a2[124] = 0x22; mod_a2[125] = 0x33; mod_a2[126] = 0x44;
	mod_a2[127] = 0;
	mod_nack = 0;
}

static void world_reset(uint32_t io_mode, uint32_t ctrl)
{
	odi_mock_reset();
	odi_mock_write_hook = hook;
	module_reset();
	memset(sram, 0, sizeof(sram));
	memset(sram_written, 0, sizeof(sram_written));
	sram_writes = ro_writes = enable_writes = 0;
	sram_stuck = 0;
	sram_bad_addr = -1;
	corrupt_on_enable = 0;
	enable_too_early = 0;
	n_info = n_warn = 0;
	odi_mock.regs[odi_mock_slot(ODI_SFPM_IO_MODE_OFF)] = io_mode;
	odi_mock.regs[odi_mock_slot(ODI_SFPM_CTRL_OFF)] = ctrl;
}

static uint32_t reg(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

static void check_sram_is_module(const char *what)
{
	char msg[160];
	unsigned int i;
	int bad = -1;

	for (i = 0; i < 256 && bad < 0; i++)
		if (sram[i] != mod_a0[i])
			bad = (int)i;
	for (i = 0; i < 256 && bad < 0; i++) {
		uint8_t want = (i >= 123 && i <= 127) ? 0 : mod_a2[i];

		if (sram[256 + i] != want)
			bad = (int)(256 + i);
	}
	snprintf(msg, sizeof(msg), "%s: SRAM is the module A0h/A2h (first difference at %d)", what, bad);
	CHECK(bad < 0, msg);
}

static void test_ro_words(void)
{
	unsigned int w;

	for (w = 0; w < 12; w++)
		CHECK(odi_sfpm_host_ro_word(w) == 0xffffffffU, "write protect: A0h and A2h 0-127 read-only");
	for (w = 12; w < 15; w++)
		CHECK(odi_sfpm_host_ro_word(w) == 0U, "write protect: A2h 128-223 writable");
	CHECK(odi_sfpm_host_ro_word(15) == 0xff000000U, "write protect: A2h 224-247 writable, 248-255 not");
}

static void test_start(void)
{
	struct odi_sfpm m;
	unsigned int w;
	int rc;

	world_reset(0x01026040U, 0x6U);
	rc = odi_sfpm_start(&m);
	CHECK(rc == 0, "start: succeeds");
	CHECK(odi_mock_locks_idle(), "start: no lock left held");
	check_sram_is_module("start");
	CHECK(memcmp(m.img, sram, sizeof(sram)) == 0, "start: the image is what the SRAM holds");
	CHECK(sram_writes == ODI_SFPM_SIZE, "start: every SRAM byte written once");
	CHECK(!enable_too_early, "start: no enable write before the whole SRAM and the write protect");
	CHECK(enable_writes == 2, "start: one write to each enable register");
	for (w = 0; w < ODI_SFPM_HOST_RO_WORDS; w++)
		CHECK(reg(ODI_SFPM_HOST_RO_OFF + 4U * w) == odi_sfpm_host_ro_word(w),
		      "start: the write protect words in place");
	CHECK(reg(ODI_SFPM_IO_MODE_OFF) == 0x01826040U, "start: 0x23018 gets bit 23 and nothing else (stock v08: 0x01824040, bit 13 left)");
	CHECK(reg(ODI_SFPM_CTRL_OFF) == 0x7U, "start: 0x2ac gets bit 0 and nothing else (stock v08: 0x7)");
	CHECK(sram[92] == 0x68 && sram[93] == 0x90, "start: A0h 92/93 as the module has them");
	CHECK(n_warn == 0 && n_info == 1, "start: one info line, no warning");
}

static void test_order_in_log(void)
{
	struct odi_sfpm m;
	unsigned int i, last_sram = 0, first_enable = 0, last_ro = 0;

	world_reset(0x01026040U, 0x6U);
	CHECK(odi_sfpm_start(&m) == 0, "order: start succeeds");
	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *e = &odi_mock.log[i];

		if (e->kind != 'W')
			continue;
		if (e->addr == ODI_SFPM_ACCESS_OFF && (e->val & ODI_SFPM_ACCESS_WRITE))
			last_sram = i;
		if (e->addr >= ODI_SFPM_HOST_RO_OFF && e->addr < ODI_SFPM_HOST_RO_OFF + 64U)
			last_ro = i;
		if (!first_enable && (e->addr == ODI_SFPM_CTRL_OFF || e->addr == ODI_SFPM_IO_MODE_OFF))
			first_enable = i;
	}
	CHECK(first_enable > last_sram && first_enable > last_ro,
	      "order: the write log has the first enable write after the last SRAM and write protect writes");
	CHECK(odi_mock.log[first_enable].addr == ODI_SFPM_CTRL_OFF, "order: slave enable before the pin mode");
}

static void test_options_byte(void)
{
	struct odi_sfpm m;
	unsigned int i, sum = 0;

	world_reset(0x01026040U, 0x6U);
	mod_a0[93] = 0xfe;	/* every control advertised */
	for (i = 64; i < 95; i++)
		sum += mod_a0[i];
	mod_a0[95] = (uint8_t)sum;
	CHECK(odi_sfpm_start(&m) == 0, "options: start succeeds");
	CHECK(sram[93] == 0xb0, "options: byte 93 keeps alarm flags, soft TX_FAULT and RX_LOS only");
	for (sum = 0, i = 64; i < 95; i++)
		sum += sram[i];
	CHECK(sram[95] == (uint8_t)sum, "options: CC_EXT still the checksum of 64-94");
	CHECK(n_info == 2, "options: the change is logged");

	world_reset(0x01026040U, 0x6U);
	mod_a0[93] = 0xb0;
	mod_a0[95] = 0x5a;	/* wrong on purpose: left alone when 93 is not changed */
	CHECK(odi_sfpm_start(&m) == 0 && sram[93] == 0xb0 && sram[95] == 0x5a,
	      "options: 0xb0 (the stock v08 mirror) passes as it is");
}

static void test_page(void)
{
	struct odi_sfpm m;
	unsigned int i, nz = 0;

	world_reset(0x01026040U, 0x6U);
	mod_a2[127] = 2;
	CHECK(odi_sfpm_start(&m) == 0, "page: start succeeds with another page selected");
	for (i = 256 + 128; i < 512; i++)
		nz += sram[i] != 0;
	CHECK(nz == 0, "page: A2h 128-255 empty when page 0 was not selected");
	CHECK(sram[256 + 127] == 0, "page: the page select reads 0");
	CHECK(n_warn == 1, "page: logged");
}

static void test_refresh(void)
{
	struct odi_sfpm m;
	unsigned int before;

	world_reset(0x01026040U, 0x6U);
	CHECK(odi_sfpm_start(&m) == 0, "refresh: start succeeds");
	before = sram_writes;
	CHECK(odi_sfpm_refresh(&m) == 0, "refresh: nothing changed");
	CHECK(sram_writes == before, "refresh: no SRAM write when nothing changed");

	mod_a2[96] = 0x2f; mod_a2[97] = 0x71;	/* temperature */
	mod_a2[104] = 0x01; mod_a2[105] = 0x02;	/* rx power */
	mod_a2[110] = 0x82;			/* TX_DISABLE state, RX_LOS */
	mod_a2[112] = 0x40;			/* temperature low alarm */
	mod_a2[120] = 0x99;			/* outside the refresh set */
	CHECK(odi_sfpm_refresh(&m) == 0, "refresh: succeeds");
	CHECK(sram[256 + 96] == 0x2f && sram[256 + 97] == 0x71, "refresh: temperature");
	CHECK(sram[256 + 104] == 0x01 && sram[256 + 105] == 0x02, "refresh: rx power");
	CHECK(sram[256 + 110] == 0x82, "refresh: byte 110 as the module has it");
	CHECK(sram[256 + 112] == 0x40, "refresh: alarm flags");
	CHECK(sram[256 + 120] != 0x99, "refresh: nothing past byte 119");
	CHECK(sram_writes == before + 6, "refresh: only the six changed bytes written");
	CHECK(memcmp(m.img, sram, sizeof(sram)) == 0, "refresh: the image follows the SRAM");
	CHECK(n_warn == 0, "refresh: no warning");
	CHECK(odi_mock_locks_idle(), "refresh: no lock left held");
}

static void test_refresh_fail(void)
{
	struct odi_sfpm m;
	uint8_t keep[ODI_SFPM_SIZE];
	unsigned int info;

	world_reset(0x01026040U, 0x6U);
	CHECK(odi_sfpm_start(&m) == 0, "read failure: start succeeds");
	memcpy(keep, sram, sizeof(sram));
	info = n_info;
	mod_a2[96] = 0x00;
	mod_nack = 1;
	CHECK(odi_sfpm_refresh(&m) != 0, "read failure: reported");
	CHECK(odi_sfpm_refresh(&m) != 0, "read failure: still reported");
	CHECK(memcmp(keep, sram, sizeof(sram)) == 0, "read failure: the host keeps the last values");
	CHECK(n_warn == 1, "read failure: logged once");
	CHECK(odi_mock_locks_idle(), "read failure: no lock left held");
	mod_nack = 0;
	CHECK(odi_sfpm_refresh(&m) == 0, "read failure: recovers");
	CHECK(sram[256 + 96] == 0x00, "read failure: the new value after the recovery");
	CHECK(n_info == info + 1 && n_warn == 1, "read failure: the recovery logged once");
}

static void test_lost_bit(void)
{
	struct odi_sfpm m;

	world_reset(0x01026040U, 0x6U);
	CHECK(odi_sfpm_start(&m) == 0, "lost bit: start succeeds");
	odi_mock.regs[odi_mock_slot(ODI_SFPM_IO_MODE_OFF)] = 0x01026040U;	/* a full-word writer */
	odi_sfpm_refresh(&m);
	CHECK(reg(ODI_SFPM_IO_MODE_OFF) == 0x01826040U, "lost bit: set again");
	odi_mock.regs[odi_mock_slot(ODI_SFPM_IO_MODE_OFF)] = 0x01026040U;
	odi_sfpm_refresh(&m);
	CHECK(n_warn == 1, "lost bit: logged once while it keeps happening");
	odi_sfpm_refresh(&m);
	odi_mock.regs[odi_mock_slot(ODI_SFPM_CTRL_OFF)] = 0x6U;
	odi_sfpm_refresh(&m);
	CHECK(reg(ODI_SFPM_CTRL_OFF) == 0x7U && n_warn == 2, "lost bit: a new loss after a clean refresh is logged again");
}

static void test_stop(void)
{
	struct odi_sfpm m;
	unsigned int i, from, io_at = 0, ctrl_at = 0;

	world_reset(0x01026040U, 0x6U);
	CHECK(odi_sfpm_start(&m) == 0, "stop: start succeeds");
	from = odi_mock.log_n;
	odi_sfpm_stop();
	CHECK(reg(ODI_SFPM_IO_MODE_OFF) == 0x01026040U, "stop: 0x23018 back to the exact value");
	CHECK(reg(ODI_SFPM_CTRL_OFF) == 0x6U, "stop: 0x2ac back to the exact value");
	for (i = from; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].addr == ODI_SFPM_IO_MODE_OFF)
			io_at = i;
		if (odi_mock.log[i].addr == ODI_SFPM_CTRL_OFF)
			ctrl_at = i;
	}
	CHECK(io_at && ctrl_at && io_at < ctrl_at, "stop: pin mode cleared before the slave enable");
	check_sram_is_module("stop: the SRAM is left as it is");
	from = odi_mock.log_n;
	odi_sfpm_stop();
	CHECK(odi_mock.log_n == from, "stop: nothing written when already off");

	/* Every other bit kept, both ways. */
	world_reset(0xff7fffffU, 0xfffffffeU);
	CHECK(odi_sfpm_start(&m) == 0, "rmw: start succeeds");
	CHECK(reg(ODI_SFPM_IO_MODE_OFF) == 0xffffffffU && reg(ODI_SFPM_CTRL_OFF) == 0xffffffffU,
	      "rmw: only the two bits set, every other bit kept");
	odi_sfpm_stop();
	CHECK(reg(ODI_SFPM_IO_MODE_OFF) == 0xff7fffffU && reg(ODI_SFPM_CTRL_OFF) == 0xfffffffeU,
	      "rmw: only the two bits cleared");
}

static void check_untouched(const char *what)
{
	char msg[160];

	snprintf(msg, sizeof(msg), "%s: neither enable register written", what);
	CHECK(enable_writes == 0, msg);
	snprintf(msg, sizeof(msg), "%s: enable registers unchanged", what);
	CHECK(reg(ODI_SFPM_IO_MODE_OFF) == 0x01026040U && reg(ODI_SFPM_CTRL_OFF) == 0x6U, msg);
	snprintf(msg, sizeof(msg), "%s: logged", what);
	CHECK(n_warn >= 1, msg);
	snprintf(msg, sizeof(msg), "%s: no lock left held", what);
	CHECK(odi_mock_locks_idle(), msg);
}

static void test_start_failures(void)
{
	struct odi_sfpm m;

	world_reset(0x01026040U, 0x6U);
	mod_nack = 1;
	CHECK(odi_sfpm_start(&m) != 0, "module silent: start fails");
	CHECK(sram_writes == 0, "module silent: the SRAM is not touched");
	check_untouched("module silent");

	world_reset(0x01026040U, 0x6U);
	sram_stuck = 1;
	CHECK(odi_sfpm_start(&m) == -ETIMEDOUT, "SRAM stuck: start fails with a timeout");
	check_untouched("SRAM stuck");

	world_reset(0x01026040U, 0x6U);
	sram_bad_addr = 300;
	CHECK(odi_sfpm_start(&m) == -EIO, "SRAM byte wrong: start fails");
	check_untouched("SRAM byte wrong");

	world_reset(0x01026040U, 0x6U);
	corrupt_on_enable = 1;
	CHECK(odi_sfpm_start(&m) == -EIO, "self-check: start fails");
	CHECK(reg(ODI_SFPM_IO_MODE_OFF) == 0x01026040U && reg(ODI_SFPM_CTRL_OFF) == 0x6U,
	      "self-check: both bits cleared again");
	CHECK(n_warn == 2, "self-check: the mismatch and the disable logged");
	CHECK(odi_mock_locks_idle(), "self-check: no lock left held");
}

int main(void)
{
	test_ro_words();
	test_start();
	test_order_in_log();
	test_options_byte();
	test_page();
	test_refresh();
	test_refresh_fail();
	test_lost_bit();
	test_stop();
	test_start_failures();
	if (failures) {
		fprintf(stderr, "odi_sfp_mirror_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_sfp_mirror_test: all passed\n");
	return 0;
}
