/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_board_test.c -- CONFIG_ODI_BOARD own odi_board_init() (odi_board.c),
 * exercised against the mock the same way odi_switch_sdkinit_test.c
 * exercises the sdkinit replay: two things.
 *
 * 1. Unit checks: replaying odi_board_init_events[] applies exactly
 *    odi_board_init_event_count writes, in order, kind for kind, address
 *    for address, value for value -- a REG entry through odi_reg_write()
 *    (mock kind 'W'), a SOC entry through odi_replay_soc_write() (mock
 *    kind 'w', and only ever reaches the log at all if the address is on
 *    the odi_replay.c allowlist -- so a missing allowlist entry
 *    fails this count check, not silently drops one write).
 * 2. `dump` mode: replays odi_board_init_events[] wrapped in ONE synthetic
 *    command-bracket mark pair (tag 0, the same "before/after" shape every
 *    other dump mode in this test suite uses) and writes a ring-dump file
 *    compare.py can read. odi_board_test.sh runs this and diffs the single
 *    resulting bracket, write for write, against test/fixtures/
 *    isp1-260923-g4-board-init-filtered.txt -- the reference capture own
 *    "== 2.94 boot" window, wrapped the same way (that window carries no
 *    real command mark of its own: it is before rcS ever runs). This is
 *    the host test for "the board replay order": a transposition, a
 *    dropped write, or a REG/SOC kind mismatch anywhere in the 88 entries
 *    fails this diff even though check 1 above (self-consistent against
 *    the same array) would not catch a transcription error shared by both.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_sdkinit.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_board_data.c"
#include <errno.h>

/* The laser driver behind A2h, as odi_board_optics() sees it: a page
 * select and the id byte at page 2 0xa0 (fake_ldd_id; -1 = no answer).
 * odi_switch_unity.h links the real odi_i2c.c (cmd 10 reads DDM through
 * it), so these two stand in for it under their own names, and only
 * odi_board.c is pointed at them (the defines around its include).
 */
static int fake_ldd_id = 0x6a;
static uint8_t fake_a2_page;
static int fake_page_writes;
static int fake_i2c_read_bytes(uint32_t sel, uint32_t addr, uint8_t *out, unsigned int n)
{
	(void)sel;
	if (n != 1 || fake_ldd_id < 0)
		return -ENXIO;
	*out = addr == 0x7fU ? fake_a2_page :
	       (fake_a2_page == 2U && addr == 0xa0U ? (uint8_t)fake_ldd_id : 0U);
	return 0;
}
static int fake_i2c_write_byte(uint32_t sel, uint32_t addr, uint8_t val)
{
	(void)sel;
	if (addr != 0x7fU)
		return -EPERM;
	fake_a2_page = val;
	fake_page_writes++;
	return 0;
}

#define odi_i2c_read_bytes fake_i2c_read_bytes
#define odi_i2c_write_byte fake_i2c_write_byte
#include "../kernel/extra/drivers/net/ethernet/odi/odi_board.c"
#undef odi_i2c_read_bytes
#undef odi_i2c_write_byte

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static void test_replay_order_and_kinds(void)
{
	unsigned int i;

	odi_mock_reset();
	odi_board_init();

	CHECK(odi_mock.log_n == odi_board_init_event_count,
	      "odi_board_init() logs exactly odi_board_init_event_count writes");

	for (i = 0; i < odi_board_init_event_count && i < odi_mock.log_n; i++) {
		const struct odi_replay_event *e = &odi_board_init_events[i];
		const struct odi_mock_write *w = &odi_mock.log[i];
		char expect_kind = (e->kind == ODI_REPLAY_SOC) ? 'w' : 'W';

		if (w->kind != expect_kind || w->addr != e->offset || w->val != e->value) {
			fprintf(stderr,
				"FAIL: event %u: expected kind %c addr 0x%08x val 0x%08x, "
				"got kind %c addr 0x%08x val 0x%08x (%s:%d)\n",
				i, expect_kind, e->offset, e->value,
				w->kind, w->addr, w->val, __FILE__, __LINE__);
			failures++;
		}
	}
}

/* The three board-init SoC addresses (0xb8000044, 0xb8003324, 0xb8003328)
 * must be on the odi_soc.c allowlist -- a regression
 * that removed one would silently drop that write instead of failing
 * loudly, which is exactly what the count check in the test above catches;
 * this second check names which address it would be.
 */
static void test_soc_addresses_allowlisted(void)
{
	static const uint32_t expect[] = { 0xb8000044U, 0xb8003324U, 0xb8003328U };
	unsigned int i;

	for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
		odi_mock_reset();
		CHECK(odi_replay_soc_write(expect[i], 0x1) == 0,
		      "board-init SoC address is on the replay allowlist");
		CHECK(odi_mock.log_n == 1 && odi_mock.log[0].kind == 'w'
		      && odi_mock.log[0].addr == expect[i],
		      "allowed SoC write reaches the mock log");
	}
}

/* The optics verb: PIN_GPIO_SELECT, then GPIO 13 low, level before
 * direction, every other GPIO bit left as it was.
 */
static void optics_setup(int ldd_id, uint8_t page)
{
	odi_mock_reset();
	odi_soc_mock_reset();
	odi_soc_mock.regs[SOC_GPIO_DATA / 4] = 0x00002001U;
	odi_soc_mock.regs[SOC_GPIO_DIR / 4] = 0x00000004U;
	fake_ldd_id = ldd_id;
	fake_a2_page = page;
	fake_page_writes = 0;
}

/* Any laser driver but the GN25L95, or none answering: the I2C pins are
 * routed by optics, the laser verb leaves GPIO 13 alone, the laser stays
 * off and GPON activation is refused (odi_gpon.c reads laser_on).
 */
static void test_optics_unknown_laser_driver(void)
{
	struct odi_board_laser l;

	optics_setup(0x55, 0U);
	CHECK(odi_board_optics() == 0 && odi_board_laser(NULL) == -ENODEV, "unknown laser driver: -ENODEV");
	CHECK(odi_mock.log_n == 1 && odi_mock.log[0].addr == 0x48U,
	      "unknown laser driver: only PIN_GPIO_SELECT is written, GPIO 13 untouched");
	CHECK(odi_soc_mock.regs[SOC_GPIO_DIR / 4] == 0x00000004U, "unknown laser driver: GPIO 13 still an input");
	odi_board_laser_get(&l);
	CHECK(!l.laser_on && l.ldd_id == 0x55 && !l.unverified, "unknown laser driver: laser off, id recorded");
	CHECK(fake_a2_page == 0U, "unknown laser driver: page select put back");

	optics_setup(-1, 0U);
	CHECK(odi_board_laser(NULL) == -ENODEV, "no answer: -ENODEV");
	odi_board_laser_get(&l);
	CHECK(!l.laser_on && l.ldd_id == -1, "no answer: laser off, id -1");

	optics_setup(0x55, 0U);
	CHECK(odi_board_laser("unverified") == 0, "unverified: forced on");
	odi_board_laser_get(&l);
	CHECK(l.laser_on && l.unverified && l.ldd_id == 0x55, "unverified: laser on, flagged");
	CHECK(odi_soc_mock.regs[SOC_GPIO_DIR / 4] == 0x00002004U, "unverified: GPIO 13 an output");
}

static void test_optics(void)
{
	struct odi_board_laser l;

	optics_setup(0x6a, 2U);
	CHECK(odi_board_optics() == 0 && odi_board_laser(NULL) == 0, "optics and laser return 0");
	odi_board_laser_get(&l);
	CHECK(l.laser_on && l.ldd_id == 0x6a, "GN25L95: laser on");
	CHECK(fake_a2_page == 2U && fake_page_writes == 1, "page 2 already selected: left as found, no restore write");
	CHECK(odi_mock.log_n == 3, "optics makes three writes");
	CHECK(odi_mock.log[0].kind == 'W' && odi_mock.log[0].addr == 0x48U &&
	      odi_mock.log[0].val == 0x08082001U, "first PIN_GPIO_SELECT = 0x08082001");
	CHECK(odi_mock.log[1].kind == 'w' && odi_mock.log[1].addr == 0xb800330cU &&
	      odi_mock.log[1].val == 0x00000001U, "then GPIO 13 low, the other data bits kept");
	CHECK(odi_mock.log[2].kind == 'w' && odi_mock.log[2].addr == 0xb8003308U &&
	      odi_mock.log[2].val == 0x00002004U, "then GPIO 13 an output, the other direction bits kept");
}

static int run_unit_checks(void)
{
	test_replay_order_and_kinds();
	test_soc_addresses_allowlisted();
	test_optics();
	test_optics_unknown_laser_driver();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_board_test: ok\n");
	return 0;
}

static int run_dump(const char *out_path)
{
	FILE *f = fopen(out_path, "w");

	if (!f) {
		fprintf(stderr, "odi_board_test: cannot open %s for writing\n", out_path);
		return 1;
	}

	odi_mock_reset();
	odi_mock_mark(0);
	odi_board_init();
	odi_mock_mark(0x80000000U);

	odi_mock_dump(f);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "dump") == 0)
		return run_dump(argv[2]);
	return run_unit_checks();
}
