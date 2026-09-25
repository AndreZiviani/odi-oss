/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_board_test.c -- CONFIG_ODI_BOARD own odi_board_init() (odi_board.c),
 * exercised against the mock the same way odi_switch_sdkinit_test.c
 * exercises odi_switch_sdkinit_apply(): two things.
 *
 * 1. Unit checks: replaying odi_board_init_events[] applies exactly
 *    odi_board_init_event_count writes, in order, kind for kind, address
 *    for address, value for value -- a REG entry through odi_reg_write()
 *    (mock kind 'W'), a SOC entry through odi_switch_soc_write() (mock
 *    kind 'w', and only ever reaches the log at all if the address is on
 *    odi_switch_sdkinit.c own allowlist -- so a missing allowlist entry
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

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
/* odi_switch_dal.c unconditionally defines odi_switch_init_modload(),
 * which decodes records through odi_replay_blob.c, and odi_switch_sdkinit.c
 * loads its table through odi_replay_fw_load() -- all built into the same
 * image alongside board on target (obj-$(CONFIG_ODI_SWITCH)/
 * obj-$(CONFIG_ODI_SDKINIT)/obj-$(CONFIG_ODI_BOARD)), so the unity build
 * needs both here too even though this test never calls either.
 */
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_sdkinit.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_board_data.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_board.c"

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
		const struct odi_sw_modload_event *e = &odi_board_init_events[i];
		const struct odi_mock_write *w = &odi_mock.log[i];
		char expect_kind = (e->kind == ODI_SW_MODLOAD_SOC) ? 'w' : 'W';

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

/* The two board-init SoC addresses (0xb8000044, 0xb8003324, 0xb8003328)
 * must already be on odi_switch_sdkinit.c own allowlist -- a regression
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
		CHECK(odi_switch_soc_write(expect[i], 0x1) == 0,
		      "board-init SoC address is on the sdkinit allowlist");
		CHECK(odi_mock.log_n == 1 && odi_mock.log[0].kind == 'w'
		      && odi_mock.log[0].addr == expect[i],
		      "allowed SoC write reaches the mock log");
	}
}

static int run_unit_checks(void)
{
	test_replay_order_and_kinds();
	test_soc_addresses_allowlisted();

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
