/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_acl_clean_test.c -- the ACL tables at platform init: rows
 * left in the table RAM by whatever ran before (another image's rules
 * after a warm boot, random words after a power cut) are gone once
 * odi_switch_init_platform() returns, every row of the rule pattern,
 * rule mask and action tables reads back zero, and that happens before
 * ACL ingress is enabled on any port. odi_switch_acl_clean_test.sh also
 * checks the two log lines (the counts found before the clean, and the
 * clean after it).
 *
 * The switch mock stores no table rows, so this test puts a small model
 * of the table engine behind TABLE_CMD (odi_mock_write_hook): a write
 * with START stores TABLE_WRITE_WORD[0..4] at (kind, row), a read copies
 * the stored row into TABLE_READ_WORD[0..4]. Rows are held in the
 * engine's own word order, so the test plants them through the same
 * reversal odi_switch_table_write() applies. Part of `make test-host`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"

#define MODEL_KINDS	8U
#define MODEL_ROWS	4096U
#define MODEL_WORDS	5U

static uint32_t model[MODEL_KINDS][MODEL_ROWS][MODEL_WORDS];

/* The first log entry that enables ACL ingress, and the number of ACL
 * table writes the model saw before it.
 */
static int acl_enabled;
static unsigned int acl_writes_after_enable;

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-64s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

static void engine_hook(uint32_t off, uint32_t val)
{
	uint32_t kind, row, i;

	if (off == ODI_SW_ACL_PORT_ENABLE_OFF && val)
		acl_enabled = 1;
	if (off != ODI_SW_TABLE_CMD_OFF || !(val & (1U << 31)))
		return;
	kind = val & 0x7U;
	row = (val >> 9) & 0xfffU;
	if (val & (1U << 3)) {
		for (i = 0; i < MODEL_WORDS; i++)
			model[kind][row][i] = odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(i))];
		if (acl_enabled && (kind == 2U || kind == 3U))
			acl_writes_after_enable++;
	} else {
		for (i = 0; i < MODEL_WORDS; i++)
			odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_READ_WORD(i))] = model[kind][row][i];
	}
}

/* Kind and row offset of the three ACL tables (odi_switch_hw.h). */
#define KIND_RULE	2U
#define KIND_ACTION	3U
#define PATTERN_ROW(r)	((r) + 128U)
#define MASK_ROW(r)	(r)

/* A row as odi_switch_table_write() takes it (word 0 first), stored the
 * way the engine holds it.
 */
static void plant(uint32_t kind, uint32_t row, const uint32_t *data, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		model[kind][row][i] = data[n - 1U - i];
}

static int acl_all_zero(void)
{
	uint32_t r, i;

	for (r = 0; r < 96U; r++)
		for (i = 0; i < MODEL_WORDS; i++)
			if (model[KIND_RULE][PATTERN_ROW(r)][i] || model[KIND_RULE][MASK_ROW(r)][i] ||
			    model[KIND_ACTION][r][i])
				return 0;
	return 1;
}

static void run_platform(void)
{
	odi_mock_reset();
	odi_mock_write_hook = engine_hook;
	acl_enabled = 0;
	acl_writes_after_enable = 0;
	odi_switch_init_platform();
	odi_mock_write_hook = NULL;
}

int main(void)
{
	/* Rule 1 as the module-load replay writes it (a previous boot of this
	 * image), a stock-firmware rule 3 with VALID clear and an address in
	 * its pattern, a rule 95 of random words with VALID set, and an
	 * action row 50 with no rule.
	 */
	static const uint32_t rule1_pat[5] = { 0x00010040U, 0, 0, 0, 0x00001c03U };
	static const uint32_t rule1_mask[5] = { 0, 0, 0, 0, 0x0000e004U };
	static const uint32_t act1[3] = { 0, 0x00001000U, 0 };
	static const uint32_t rule3_pat[5] = { 0x00000040U, 0xc0a80001U, 0, 0, 0x00001c03U };
	static const uint32_t rule95_pat[5] = { 0x5a5bffffU, 0x1234abcdU, 0xffffffffU, 0, 0x8001U };
	static const uint32_t act50[3] = { 0xdeadbeefU, 0, 0x7U };

	memset(model, 0, sizeof model);
	plant(KIND_RULE, PATTERN_ROW(1), rule1_pat, 5);
	plant(KIND_RULE, MASK_ROW(1), rule1_mask, 5);
	plant(KIND_ACTION, 1, act1, 3);
	plant(KIND_RULE, PATTERN_ROW(3), rule3_pat, 5);
	plant(KIND_RULE, PATTERN_ROW(95), rule95_pat, 5);
	plant(KIND_ACTION, 50, act50, 3);
	/* A CF row next to them, which the ACL clean must not touch (the cf
	 * sweep clears CLS_RULE_B and both CF actions, not CLS_MASK_B).
	 */
	model[4][7][0] = 0x11U;

	printf("leftover ACL rows at platform init:\n");
	run_platform();
	ok(acl_all_zero(), "every pattern, mask and action row reads back zero");
	ok(acl_writes_after_enable == 0, "no ACL row written after ACL ingress is enabled");
	ok(model[4][7][0] == 0x11U, "the CF mask row is left alone");
	ok(odi_mock_locks_idle(), "no lock left held");

	printf("a clean table:\n");
	run_platform();
	ok(acl_all_zero(), "still zero");

	printf("an engine that never finishes:\n");
	memset(model, 0, sizeof model);
	plant(KIND_RULE, PATTERN_ROW(1), rule1_pat, 5);
	odi_mock_reset();
	odi_mock.poll_stuck = 1;
	odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_STATUS_OFF)] = ODI_SW_TABLE_STATUS_IN_PROGRESS;
	odi_switch_init_platform();
	ok(odi_mock_locks_idle(), "platform init returns, no lock left held");

	if (failures) {
		printf("odi_switch_acl_clean_test: %d FAILED\n", failures);
		return 1;
	}
	printf("odi_switch_acl_clean_test: ok\n");
	return 0;
}
