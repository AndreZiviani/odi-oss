/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_modload_test.c -- two things:
 *
 * 1. Mask-selection unit checks on odi_switch_init_modload() (empty mask,
 *    a single category, category isolation) against a handful of
 *    the real generated table, rootfs/skeleton/lib/firmware/odi/
 *    modload.bin, loaded through the host half of odi_replay_fw_load()
 *    (odi_replay_fw_host.h) exactly as the kernel trigger loads it.
 * 2. `dump` mode: replays the REAL modload.bin (mask =
 *    ODI_SWITCH_INIT_MODLOAD_ITEM_ALL) through the mock and writes a
 *    ring-dump file compare.py can read -- odi_switch_modload_replay_
 *    test.sh runs this and diffs the result against test/fixtures/
 *    isp1-260922-v7-modload-filtered.txt: replaying the table through
 *    the mock reproduces the capture bracket-free, W and T/D entries
 *    both.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"
#include "odi_replay_fw_host.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* modload.bin, loaded once by main() for every check below. */
static struct odi_replay_fw modload_fw;
static const struct odi_replay_blob *table = &modload_fw.blob;

/* Record i of modload.bin, decoded. */
static struct odi_sw_modload_event event_at(uint32_t i)
{
	struct odi_sw_modload_event e;
	uint8_t verb;

	odi_replay_blob_switch_event(table, i, &e, &verb);
	return e;
}

static unsigned int count_writes(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

static unsigned int count_kind(char kind)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == kind)
			n++;
	return n;
}

/* mask == 0 must be a documented no-op, not "run everything" or a crash --
 * same contract as odi_switch_init_platform()/init_parity().
 */
static void test_empty_mask(void)
{
	odi_mock_reset();
	odi_switch_init_modload(table, 0);
	odi_mock_table_flush();

	CHECK(odi_mock.log_n == 0, "empty mask: no writes of any kind");
}

/* Bit 0 alone must apply every register event in the real table and none
 * of the table-row events -- checked structurally (T/D counts stay zero),
 * not by re-deriving every register value here.
 */
static void test_register_only_mask(void)
{
	odi_mock_reset();
	odi_switch_init_modload(table, ODI_SWITCH_INIT_MODLOAD_ITEM(0));
	odi_mock_table_flush();

	CHECK(count_kind('T') == 0, "register-only mask: no table writes");
	CHECK(count_kind('D') == 0, "register-only mask: no table data words");
	CHECK(count_kind('W') > 0, "register-only mask: some register writes happened");

	/* First register event in the generated table (modload.bin record
	 * 0) must have actually landed, content-matched against the
	 * table's own recorded offset/value -- catches a category or ordering
	 * regression without hardcoding the real capture's own first value.
	 */
	{
		unsigned int i;
		int found_reg = 0;

		for (i = 0; i < table->count && !found_reg; i++) {
			struct odi_sw_modload_event e = event_at(i);

			if (e.kind == ODI_SW_MODLOAD_REG) {
				CHECK(count_writes(e.offset) >= 1,
				      "register-only mask: first register event's own address was written");
				found_reg = 1;
			}
		}
		CHECK(found_reg, "test setup: the real table has at least one register event");
	}
}

/* Bit 3 (VLAN) alone must write exactly the one VLAN row, and nothing
 * from any other category.
 */
static void test_vlan_only_mask(void)
{
	odi_mock_reset();
	odi_switch_init_modload(table, ODI_SWITCH_INIT_MODLOAD_ITEM(3));
	odi_mock_table_flush();

	CHECK(count_kind('W') == 0, "VLAN-only mask: no register writes");
	CHECK(count_kind('T') == 1, "VLAN-only mask: exactly one table write (the VLAN row)");

	{
		unsigned int i;

		for (i = 0; i < odi_mock.log_n; i++) {
			if (odi_mock.log[i].kind != 'T')
				continue;
			CHECK(((odi_mock.log[i].addr >> 16) & 0xffffU) == ODI_SW_TBL_VLAN_MEMBERS,
			      "VLAN-only mask: the one table write is the VLAN table");
		}
	}
}

/* The generated table's own category counts (from mkmodload.py's own
 * stderr summary) -- pinned here so
 * a silent regeneration-with-different-categorisation is caught. Not
 * pinned to specific offsets/values (that is what the compare.py replay
 * test is for); this only checks the category split itself.
 */
static void test_category_counts_pinned(void)
{
	unsigned int counts[6] = { 0 };
	unsigned int i;

	for (i = 0; i < table->count; i++)
		counts[event_at(i).category]++;

	CHECK(counts[0] == 2587, "category 0 (register) count matches the generated table");
	CHECK(counts[1] == 1024, "category 1 (CF sweep) count matches the generated table");
	CHECK(counts[2] == 3, "category 2 (CF specific rule) count matches the generated table");
	CHECK(counts[3] == 1, "category 3 (VLAN) count matches the generated table");
	CHECK(counts[4] == 1, "category 4 (L2_UNICAST) count matches the generated table");
	CHECK(counts[5] == 3, "category 5 (ACL) count matches the generated table");
	CHECK(table->count == 3619, "total event count matches the generated table");
}

/* The headline finding: the three
 * category-2 events are CLS_RULE_B[255]/CLS_MASK_B[255]/
 * CLS_DS_ACTION[255], and the rule's own words decode to VALID=1, U_D=1
 * (downstream), UNI_ACT=CF_DS_UNI_ACT_FORCE_FORWARD (value 1) -- pinned
 * here as literal values so a regeneration that silently lost or
 * reordered this finding is caught.
 */
static void test_headline_rule_present(void)
{
	unsigned int i;
	int found_rule = 0, found_mask = 0, found_action = 0;

	for (i = 0; i < table->count; i++) {
		struct odi_sw_modload_event ev = event_at(i);
		const struct odi_sw_modload_event *e = &ev;

		if (e->kind != ODI_SW_MODLOAD_TABLE || e->category != 2)
			continue;

		if (e->table == ODI_SW_TBL_CLS_RULE_B && e->offset == 255) {
			CHECK(e->n_words == 2 && e->words[0] == 0x00010000U && e->words[1] == 0x80000000U,
			      "CLS_RULE_B[255]: VALID=1 (word0 bit16), U_D=1 (word1 bit31)");
			found_rule = 1;
		} else if (e->table == ODI_SW_TBL_CLS_MASK_B && e->offset == 255) {
			CHECK(e->n_words == 2 && e->words[0] == 0 && e->words[1] == 0,
			      "CLS_MASK_B[255]: all-zero, every field wildcarded");
			found_mask = 1;
		} else if (e->table == ODI_SW_TBL_CLS_DS_ACTION && e->offset == 255) {
			CHECK(e->n_words == 3 && e->words[0] == 0 &&
			      e->words[1] == 0x00240000U && e->words[2] == 0x80240000U,
			      "CLS_DS_ACTION[255]: UNI_ACT=1 (CF_DS_UNI_ACT_FORCE_FORWARD) among the packed words");
			found_action = 1;
		}
	}
	CHECK(found_rule, "CLS_RULE_B[255] is present as a category-2 event");
	CHECK(found_mask, "CLS_MASK_B[255] is present as a category-2 event");
	CHECK(found_action, "CLS_DS_ACTION[255] is present as a category-2 event");
}

/* --- "modload category 0 split": reg_group / bit-6+ checks -- */

static const uint32_t FLOOD_ADDRS[3] = { 0x0001c020U, 0x0001c024U, 0x0001c028U };

static int is_flood_addr(uint32_t addr)
{
	unsigned int i;

	for (i = 0; i < 3; i++)
		if (FLOOD_ADDRS[i] == addr)
			return 1;
	return 0;
}

/* The highest reg_group any category-0 event actually uses -- computed
 * from the generated table itself rather than hardcoded, so these checks
 * do not need editing every time mkmodload.py's family list changes size.
 */
static unsigned int max_reg_group(void)
{
	unsigned int i, max = 0;

	for (i = 0; i < table->count; i++) {
		struct odi_sw_modload_event ev = event_at(i);
		const struct odi_sw_modload_event *e = &ev;

		if (e->kind == ODI_SW_MODLOAD_REG && e->reg_group > max)
			max = e->reg_group;
	}
	return max;
}

static uint32_t all_categories_1_to_5(void)
{
	return ODI_SWITCH_INIT_MODLOAD_ITEM(1) | ODI_SWITCH_INIT_MODLOAD_ITEM(2) |
	       ODI_SWITCH_INIT_MODLOAD_ITEM(3) | ODI_SWITCH_INIT_MODLOAD_ITEM(4) |
	       ODI_SWITCH_INIT_MODLOAD_ITEM(5);
}

/* Every non-FLOOD family bit, reg_group 1..max_reg_group() -> bits 7..
 * (6+max) -- deliberately NOT bit 6 (FLOOD) and NOT bit 0 (the legacy
 * dominate-everything shortcut) and NOT the CPU-forced-flood bit.
 */
static uint32_t all_family_bits_except_flood(void)
{
	unsigned int g, max = max_reg_group();
	uint32_t mask = 0;

	for (g = 1; g <= max; g++)
		mask |= 1U << ODI_SWITCH_INIT_MODLOAD_REGGROUP_BIT(g);
	return mask;
}

/* Every family bit INCLUDING FLOOD's own bit 6 -- reg_group 0..
 * max_reg_group(), bits 6..(6+max). Still not bit 0 and not the CPU-
 * forced modifier: this is "select every family individually," the
 * bit-6-and-up equivalent of bit 0's "give me everything" shortcut.
 */
static uint32_t all_family_bits_including_flood(void)
{
	return all_family_bits_except_flood() | (1U << ODI_SWITCH_INIT_MODLOAD_REGGROUP_BIT(0));
}

/* s13's own next trial ("modload category 0 split"): the full
 * replay with the FLOOD family excluded must apply every category-0
 * register EXCEPT the 20 writes to the 3 flood addresses, and nothing
 * about the other categories (1-5) changes.
 */
static void test_flood_excluded_removes_only_flood_writes(void)
{
	uint32_t mask = all_categories_1_to_5() | all_family_bits_except_flood();
	unsigned int i;

	odi_mock_reset();
	odi_switch_init_modload(table, mask);
	odi_mock_table_flush();

	for (i = 0; i < 3; i++)
		CHECK(count_writes(FLOOD_ADDRS[i]) == 0,
		      "flood family excluded: none of the 3 flood addresses were written");

	/* Every other register write still happened: total W count is
	 * exactly the category-0 total minus the 20 flood events (pinned
	 * count, test_category_counts_pinned() already checks the 2587
	 * total and the flood family's own 20-event size is implied by
	 * this check passing together with that one).
	 */
	CHECK(count_kind('W') == 2587U - 20U,
	      "flood family excluded: every other category-0 register still wrote (2567 = 2587 - 20)");
}

/* The core claim of the split: the family bits (6 for FLOOD, 7.. for
 * everything else) partition category 0 EXACTLY -- no register event in
 * two families, none missing. Proven by construction: a replay selecting
 * every family bit individually (no bit 0) must produce a BYTE-FOR-BYTE
 * identical log to a replay using bit 0's "give me everything" shortcut,
 * for the exact same categories 1-5. If any event had no family (missing)
 * the family-bits run would be short; if any event had two families
 * (double-counted) it would appear twice; either would show up as a log
 * mismatch here.
 */
static void test_family_bits_partition_category0_exactly(void)
{
	static struct odi_mock_write bit0_log[ODI_MOCK_LOG_MAX];
	unsigned int bit0_log_n;
	unsigned int i;

	odi_mock_reset();
	odi_switch_init_modload(table, ODI_SWITCH_INIT_MODLOAD_ITEM(0) | all_categories_1_to_5());
	odi_mock_table_flush();
	bit0_log_n = odi_mock.log_n;
	memcpy(bit0_log, odi_mock.log, bit0_log_n * sizeof(bit0_log[0]));

	odi_mock_reset();
	odi_switch_init_modload(table, all_family_bits_including_flood() | all_categories_1_to_5());
	odi_mock_table_flush();

	CHECK(odi_mock.log_n == bit0_log_n,
	      "family bits (6..) together produce the same number of log entries as bit 0 alone");
	if (odi_mock.log_n == bit0_log_n) {
		int identical = 1;

		for (i = 0; i < bit0_log_n; i++) {
			if (odi_mock.log[i].kind != bit0_log[i].kind ||
			    odi_mock.log[i].addr != bit0_log[i].addr ||
			    odi_mock.log[i].val != bit0_log[i].val) {
				identical = 0;
				break;
			}
		}
		CHECK(identical,
		      "family bits (6..) together reproduce bit 0's log byte-for-byte -- exact partition");
	}
}

/* The CPU-forced-flood modifier bit (30): every flood-address write
 * becomes 0x0000000f (every port, CPU included), regardless of what was
 * actually captured for that event (0x7 or 0xf) -- and takes priority
 * over bit 6 when both are set, so a trial does not have to clear bit 6
 * first.
 */
static void test_flood_cpu_forced_value(void)
{
	uint32_t mask = all_categories_1_to_5() | all_family_bits_except_flood() |
			 (1U << ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_BIT);
	unsigned int i;

	odi_mock_reset();
	odi_switch_init_modload(table, mask);
	odi_mock_table_flush();

	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind != 'W' || !is_flood_addr(odi_mock.log[i].addr))
			continue;
		CHECK(odi_mock.log[i].val == ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_VALUE,
		      "CPU-forced flood: every flood write is 0x0000000f, not its captured value");
	}
	CHECK(count_kind('W') > 0, "test setup: the CPU-forced-flood run wrote something");

	/* bit 30 set together with bit 6 (FLOOD's own bit): 30 wins, per
	 * odi_switch_dal.h's own documented tie-break.
	 */
	odi_mock_reset();
	odi_switch_init_modload(table, (1U << ODI_SWITCH_INIT_MODLOAD_REGGROUP_BIT(0)) |
					(1U << ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_BIT));
	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind != 'W' || !is_flood_addr(odi_mock.log[i].addr))
			continue;
		CHECK(odi_mock.log[i].val == ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_VALUE,
		      "bit 6 and bit 30 both set: bit 30 (CPU-forced) wins");
	}
}

/* Bit 0 still dominates everything, including the new bits -- setting bit
 * 0 alongside bit 6, a family bit or the CPU-forced bit must behave
 * exactly like bit 0 alone (every register verbatim, no CPU-forced
 * override), matching odi_switch_dal.h's own "when bit 0 is set, the
 * family bits are ignored" contract.
 */
static void test_bit0_dominates_new_bits(void)
{
	uint32_t mask = ODI_SWITCH_INIT_MODLOAD_ITEM(0) |
			 (1U << ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_BIT);
	unsigned int i;
	int saw_captured_0x7 = 0;

	odi_mock_reset();
	odi_switch_init_modload(table, mask);
	odi_mock_table_flush();

	/* The captured flood sequence is mostly 0xf already (matching the
	 * CPU-forced value by coincidence) with the FINAL write settling at
	 * 0x7 (CPU excluded, s13's own suspect) -- so the one value that can
	 * only appear if bit 0 truly ignored the CPU-forced bit is that
	 * final 0x7, not a bare "not all-0xf" check.
	 */
	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind == 'W' && is_flood_addr(odi_mock.log[i].addr) &&
		    odi_mock.log[i].val == 0x00000007U)
			saw_captured_0x7 = 1;
	}
	CHECK(saw_captured_0x7,
	      "bit 0 + CPU-forced bit together: bit 0 wins, the captured 0x7 still lands, not forced to 0xf");
}

static int run_unit_checks(void)
{
	test_empty_mask();
	test_register_only_mask();
	test_vlan_only_mask();
	test_category_counts_pinned();
	test_headline_rule_present();
	test_flood_excluded_removes_only_flood_writes();
	test_family_bits_partition_category0_exactly();
	test_flood_cpu_forced_value();
	test_bit0_dominates_new_bits();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_modload_test: ok\n");
	return 0;
}

static int run_dump(const char *out_path)
{
	FILE *f = fopen(out_path, "w");

	if (!f) {
		fprintf(stderr, "odi_switch_modload_test: cannot open %s for writing\n", out_path);
		return 1;
	}

	odi_mock_reset();
	odi_mock_mark(0x00000000U); /* before, synthetic cmd 0 */
	odi_switch_init_modload(table, ODI_SWITCH_INIT_MODLOAD_ITEM_ALL);
	odi_mock_mark(0x80000000U); /* after, synthetic cmd 0 */
	odi_mock_dump(f);

	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	int rc;

	if (odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &modload_fw)) {
		fprintf(stderr, "odi_switch_modload_test: modload.bin did not load\n");
		return 1;
	}
	if (argc >= 3 && strcmp(argv[1], "dump") == 0)
		rc = run_dump(argv[2]);
	else
		rc = run_unit_checks();
	odi_replay_fw_release(&modload_fw);
	return rc;
}
