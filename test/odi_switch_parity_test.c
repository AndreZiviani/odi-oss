/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_parity_test.c -- asserts odi_switch_init_parity()'s exact
 * per-entry read-modify-write behaviour and mask selection ("v6 parity
 * table"). The table itself (odi_switch_parity_table[],
 * odi_switch_dal.c) is built from a live A/B register dump, not a trace,
 * so this test checks the RMW mechanics -- offset/value/mask applied
 * correctly, mask bit n selects entry n, mask 0 and a mask with bits past
 * the table's length are both safe -- rather than replaying a capture.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static int nth_w_val(uint32_t addr, unsigned int n, uint32_t *out_val)
{
	unsigned int i, seen = 0;

	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr) {
			if (seen == n) {
				*out_val = odi_mock.log[i].val;
				return 1;
			}
			seen++;
		}
	}
	return 0;
}

static unsigned int count_writes(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

/* Every entry starts from a fresh (zero) mock register, so the RMW's
 * written value collapses to (value & mask) for each -- checked against the
 * table's own value/mask fields directly (not hardcoded a second time),
 * so this only breaks if odi_switch_init_parity() applies the wrong
 * offset/mask/value to the wrong entry, not if a future entry is added.
 */
static void test_full_mask_matches_table(void)
{
	unsigned int i;
	uint32_t mask, v;

	odi_mock_reset();
	mask = (odi_switch_parity_table_count >= 32) ?
		0xffffffffU : (1U << odi_switch_parity_table_count) - 1U;
	odi_switch_init_parity(mask);

	for (i = 0; i < odi_switch_parity_table_count; i++) {
		const struct odi_sw_parity_entry *e = &odi_switch_parity_table[i];
		uint32_t expect = e->value & e->mask; /* reg started at 0 */

		CHECK(nth_w_val(e->offset, 0, &v) && v == expect, e->name);
	}
}

/* Known-good coordinates ("v6 parity table"): entry
 * 0 is MAX_FRAME_LEN_1 = 0x000007ef, entry 13 is VLAN_INGRESS_CHECK = 0x0000000f
 * with mask 0x0000000f -- pinned here as literal values so a future edit to
 * odi_switch_parity_table[] that silently changes either entry's content is
 * caught even though test_full_mask_matches_table() above would not catch
 * it (that check is self-referential against the table, not against
 * independently known values).
 */
static void test_known_entries_pinned(void)
{
	CHECK(odi_switch_parity_table_count >= 14, "at least the 14 coordinator entries exist");
	CHECK(odi_switch_parity_table[0].offset == 0x011018U &&
	      odi_switch_parity_table[0].value == 0x000007efU &&
	      odi_switch_parity_table[0].mask == 0xffffffffU,
	      "entry 0: MAX_FRAME_LEN_1, full-word mask, v6's 0x7ef");
	CHECK(odi_switch_parity_table[13].offset == ODI_SW_VLAN_INGRESS_CHECK_BASE &&
	      odi_switch_parity_table[13].value == 0x0000000fU &&
	      odi_switch_parity_table[13].mask == 0x0000000fU,
	      "entry 13: VLAN_INGRESS_CHECK, narrowed mask, v6's 0x0f (reverses item 4's port-1 write)");
}

/* Bitmask selection, same contract as odi_switch_init_platform(): only the
 * entries named by the mask write anything.
 */
static void test_partial_mask(void)
{
	unsigned int i;

	odi_mock_reset();
	odi_switch_init_parity(ODI_SWITCH_INIT_PARITY_ENTRY(0) | ODI_SWITCH_INIT_PARITY_ENTRY(13));

	CHECK(count_writes(odi_switch_parity_table[0].offset) == 1,
	      "partial mask: entry 0 runs when selected");
	CHECK(count_writes(odi_switch_parity_table[13].offset) == 1,
	      "partial mask: entry 13 runs when selected");

	for (i = 1; i < 13; i++)
		CHECK(count_writes(odi_switch_parity_table[i].offset) == 0,
		      "partial mask: an unselected entry does not run");
}

/* mask == 0 must be a documented no-op, not "run everything" or a crash --
 * same contract as odi_switch_init_platform().
 */
static void test_empty_mask(void)
{
	odi_mock_reset();
	odi_switch_init_parity(0);

	CHECK(odi_mock.log_n == 0, "empty mask: no writes of any kind");
}

/* A mask bit past the table's own length must be ignored, not treated as a
 * crash or an out-of-bounds table access -- odi_switch_init_parity() only
 * loops i < odi_switch_parity_table_count, so bit 31 here can only ever
 * select entry 31, which does not exist yet.
 */
static void test_mask_beyond_table_ignored(void)
{
	uint32_t v;

	odi_mock_reset();
	odi_switch_init_parity(ODI_SWITCH_INIT_PARITY_ENTRY(0) | (1U << 31));

	CHECK(nth_w_val(odi_switch_parity_table[0].offset, 0, &v) &&
	      v == (odi_switch_parity_table[0].value & odi_switch_parity_table[0].mask),
	      "mask beyond table: the in-range entry still runs");
	CHECK(odi_mock.log_n == 1,
	      "mask beyond table: exactly one register access (entry 0's write), nothing for bit 31");
}

/* entry 13 (VLAN_INGRESS_CHECK)'s narrowed mask (0x0000000f) is the one entry
 * this table does not apply as a full-word write -- confirm it actually
 * preserves bits outside the mask (reserved/unknown upper bits here,
 * standing in for whatever odi_switch_init_platform()'s item 4 or a
 * concurrent write left there) instead of stomping the whole word the way
 * every other entry's full-word mask does.
 */
static void test_vlan_ingress_narrow_mask_preserves_bits(void)
{
	uint32_t v;

	odi_mock_reset();
	odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK_BASE, 0x000000f0U); /* bits outside 0x0f pre-set */
	odi_switch_init_parity(ODI_SWITCH_INIT_PARITY_ENTRY(13));

	/* (0xf0 & ~0x0f) | (0x0f & 0x0f) = 0xf0 | 0x0f = 0xff. */
	CHECK(nth_w_val(ODI_SW_VLAN_INGRESS_CHECK_BASE, 1, &v) && v == 0x000000ffU,
	      "entry 13: narrow mask preserves bits outside 0x0000000f");
}

/* --- the loader: odi_switch_parity_add()/_clear() and the active table --
 * ("parity table from a file"). odi_switch_parity_clear() at the
 * top of every test below is not optional cleanup: the loaded table and
 * the using-loaded flag are file-scope static state in odi_switch_dal.c,
 * not reset by odi_mock_reset() (that resets registers and the mock log
 * only), so a test that leaves entries loaded would leak into whichever
 * test runs after it in this same process.
 */

static void test_parity_add_accepts_and_activates(void)
{
	const struct odi_sw_parity_entry *t;

	odi_mock_reset();
	odi_switch_parity_clear();

	CHECK(odi_switch_parity_is_loaded() == 0, "before any parity_add: compiled default active");
	CHECK(odi_switch_parity_active_count() == odi_switch_parity_table_count,
	      "before any parity_add: active count is the compiled default's");

	CHECK(odi_switch_parity_add(0x011018U, 0x000007efU, 0xffffffffU) == 0,
	      "parity_add: an in-bounds offset is accepted");

	CHECK(odi_switch_parity_is_loaded() == 1, "one accepted parity_add switches to the loaded table");
	CHECK(odi_switch_parity_active_count() == 1, "loaded table now has exactly 1 entry");

	t = odi_switch_parity_active_table();
	CHECK(t[0].offset == 0x011018U && t[0].value == 0x000007efU && t[0].mask == 0xffffffffU,
	      "the loaded entry's offset/value/mask match what was added");

	odi_switch_parity_clear();
}

/* ODI_SWITCH_MMIO_SIZE (odi_switch_hw.h) is 0xF10000 -- well past that is
 * refused by odi_switch_mmio_offset_in_bounds(), the same guard
 * odi_reg_read/_write() themselves use, so a bad offset from a corrupted
 * or hand-edited table file cannot reach a real MMIO access.
 */
static void test_parity_add_rejects_bad_offset(void)
{
	odi_mock_reset();
	odi_switch_parity_clear();

	CHECK(odi_switch_parity_add(0xffffffffU, 0x1U, 0xffffffffU) == -1,
	      "parity_add: an offset far outside the mapped window is refused");
	CHECK(odi_switch_parity_active_count() == odi_switch_parity_table_count,
	      "a refused entry does not switch the active table or count as loaded");
	CHECK(odi_switch_parity_is_loaded() == 0,
	      "a refused entry alone does not flip the loaded flag");

	odi_switch_parity_clear();
}

/* ODI_SWITCH_PARITY_MAX_LOADED (odi_switch_dal.h) is 64 -- the 65th
 * parity_add on an otherwise-empty loaded table must be refused, and the
 * count must stay pinned at 64, not silently grow or wrap.
 */
static void test_parity_add_table_full(void)
{
	unsigned int i;
	int last_rc = 0;

	odi_mock_reset();
	odi_switch_parity_clear();

	for (i = 0; i < 65; i++)
		last_rc = odi_switch_parity_add(0x011018U, 0x1U, 0xffffffffU);

	CHECK(last_rc == -1, "parity_add: the 65th entry on a 64-capacity table is refused");
	CHECK(odi_switch_parity_active_count() == 64U,
	      "parity_add: the loaded table stays pinned at the 64-entry cap");

	odi_switch_parity_clear();
}

static void test_parity_clear_reverts_to_default(void)
{
	odi_mock_reset();
	odi_switch_parity_clear();

	(void)odi_switch_parity_add(0x011018U, 0x1U, 0xffffffffU);
	CHECK(odi_switch_parity_is_loaded() == 1, "sanity: parity_add did switch to the loaded table");

	odi_switch_parity_clear();

	CHECK(odi_switch_parity_is_loaded() == 0, "parity_clear: reverts to the compiled default");
	CHECK(odi_switch_parity_active_count() == odi_switch_parity_table_count,
	      "parity_clear: active count is the compiled default's again");
	CHECK(odi_switch_parity_active_table() == odi_switch_parity_table,
	      "parity_clear: active table pointer is the compiled default array again");
}

/* The actual point of odi_switch_init_parity_all(): reach entry 32 and
 * beyond of a loaded table, which a uint32_t bitmask (odi_switch_init_
 * parity()) cannot address at all. 40 entries, all the same offset/value/
 * mask (the loader does not require distinct offsets), spot-checked at
 * index 0, 31, 32 and 39 -- the pair either side of the 32-bit boundary is
 * the case that would have silently failed before odi_switch_init_parity_
 * all() existed (odi_switch_init_parity()'s own mask literally cannot
 * name entry 32).
 */
static void test_init_parity_all_reaches_past_32_entries(void)
{
	unsigned int i;
	uint32_t v;

	odi_mock_reset();
	odi_switch_parity_clear();

	for (i = 0; i < 40; i++)
		CHECK(odi_switch_parity_add(0x011018U + 4U * i, 0x000000abU, 0xffffffffU) == 0,
		      "test setup: 40 in-bounds parity_add calls all accepted");
	CHECK(odi_switch_parity_active_count() == 40U, "test setup: 40 entries loaded");

	odi_switch_init_parity_all();

	CHECK(nth_w_val(0x011018U, 0, &v) && v == 0x000000abU, "init_parity_all: entry 0 applied");
	CHECK(nth_w_val(0x011018U + 4U * 31U, 0, &v) && v == 0x000000abU, "init_parity_all: entry 31 applied");
	CHECK(nth_w_val(0x011018U + 4U * 32U, 0, &v) && v == 0x000000abU,
	      "init_parity_all: entry 32 applied (unreachable via a 32-bit mask)");
	CHECK(nth_w_val(0x011018U + 4U * 39U, 0, &v) && v == 0x000000abU, "init_parity_all: entry 39 (last) applied");

	odi_switch_parity_clear();
}

/* The documented flip side of the above: odi_switch_init_parity()'s
 * bitmask, even set to all 1s, only ever reaches entries 0-31 of whatever
 * table is active -- entry 32 must NOT be touched by a mask-driven call,
 * only by the bare/_all() form.
 */
static void test_init_parity_mask_does_not_reach_entry_32(void)
{
	unsigned int i;

	odi_mock_reset();
	odi_switch_parity_clear();

	for (i = 0; i < 33; i++)
		(void)odi_switch_parity_add(0x011018U + 4U * i, 0x000000abU, 0xffffffffU);
	CHECK(odi_switch_parity_active_count() == 33U, "test setup: 33 entries loaded");

	odi_switch_init_parity(0xffffffffU);

	CHECK(count_writes(0x011018U + 4U * 31U) == 1, "init_parity(all 1s): entry 31 applied");
	CHECK(count_writes(0x011018U + 4U * 32U) == 0,
	      "init_parity(all 1s): entry 32 NOT applied -- a uint32_t mask cannot name it");

	odi_switch_parity_clear();
}

int main(void)
{
	test_full_mask_matches_table();
	test_known_entries_pinned();
	test_partial_mask();
	test_empty_mask();
	test_mask_beyond_table_ignored();
	test_vlan_ingress_narrow_mask_preserves_bits();
	test_parity_add_accepts_and_activates();
	test_parity_add_rejects_bad_offset();
	test_parity_add_table_full();
	test_parity_clear_reverts_to_default();
	test_init_parity_all_reaches_past_32_entries();
	test_init_parity_mask_does_not_reach_entry_32();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_parity_test: ok\n");
	return 0;
}
