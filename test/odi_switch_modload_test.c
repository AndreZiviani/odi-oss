/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_modload_test.c -- the module-load replay against the shipped
 * rootfs/skeleton/lib/firmware/odi/modload.bin, loaded through the host
 * half of odi_replay_fw_load() (odi_replay_fw_host.h) as the kernel
 * trigger loads it:
 *
 * 1. unit checks: every record is replayed once, the file carries the
 *    production selection (the three LUT flood masks with the CPU port),
 *    its capture families are those mkmodload.py generated, and the
 *    downstream default-forward rule is in it;
 * 2. `dump` mode: the replay as a ring dump, which
 *    odi_switch_modload_replay_test.sh compares with the capture
 *    (test/fixtures/isp1-260922-v7-modload-filtered.txt).
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
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
static struct odi_replay_event event_at(uint32_t i)
{
	struct odi_replay_event e;

	odi_replay_blob_event(table, i, &e);
	return e;
}

/* The capture family byte of record i (odi_replay_blob.h), which the
 * replay does not read.
 */
static uint8_t raw_category(uint32_t i)
{
	return table->records[(size_t)i * ODI_REPLAY_BLOB_RECORD_SIZE + 1U];
}

static unsigned int count_kind(char kind)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == kind)
			n++;
	return n;
}

static const uint32_t FLOOD_ADDRS[3] = { 0x0001c020U, 0x0001c024U, 0x0001c028U };

static int is_flood_addr(uint32_t addr)
{
	return addr == FLOOD_ADDRS[0] || addr == FLOOD_ADDRS[1] || addr == FLOOD_ADDRS[2];
}

/* Every register record becomes one read and one write, every table
 * record one table op, in file order.
 */
static void test_every_record_replayed(void)
{
	unsigned int i, w = 0, regs = 0, rows = 0, in_order = 1;

	odi_mock_reset();
	odi_switch_init_modload(table);
	odi_mock_table_flush();

	for (i = 0; i < table->count; i++) {
		struct odi_replay_event e = event_at(i);

		if (e.kind != ODI_REPLAY_REG) {
			rows++;
			continue;
		}
		regs++;
		while (w < odi_mock.log_n && odi_mock.log[w].kind != 'W')
			w++;
		if (w >= odi_mock.log_n || odi_mock.log[w].addr != e.offset ||
		    odi_mock.log[w].val != e.value)
			in_order = 0;
		w++;
	}
	CHECK(regs == 2587 && rows == 1032, "2587 register and 1032 table records");
	CHECK(count_kind('W') == regs, "one write per register record");
	CHECK(count_kind('T') == rows, "one table op per table record (no two rows fold)");
	CHECK(in_order, "the writes follow the file, value for value");
}

/* The production selection is in the file: the three LUT flood masks
 * include the CPU port, 0xf.
 */
static void test_flood_masks_include_cpu(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < table->count; i++) {
		struct odi_replay_event e = event_at(i);

		if (e.kind == ODI_REPLAY_REG && is_flood_addr(e.offset)) {
			n++;
			CHECK(e.value == 0x0000000fU, "a flood mask record carries 0xf");
		}
	}
	CHECK(n == 20, "20 flood mask records");
}

/* The capture families mkmodload.py assigned, pinned so a regeneration
 * that splits the capture differently is caught.
 */
static void test_category_counts_pinned(void)
{
	unsigned int counts[6] = { 0 };
	unsigned int i;

	for (i = 0; i < table->count; i++)
		if (raw_category(i) < 6)
			counts[raw_category(i)]++;

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
		struct odi_replay_event ev = event_at(i);
		const struct odi_replay_event *e = &ev;

		if (e->kind != ODI_REPLAY_TABLE || raw_category(i) != 2)
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

static int run_unit_checks(void)
{
	test_every_record_replayed();
	test_flood_masks_include_cpu();
	test_category_counts_pinned();
	test_headline_rule_present();

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
	odi_switch_init_modload(table);
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
