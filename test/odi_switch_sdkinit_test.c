/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_switch_sdkinit_test.c -- two things:
 *
 * 1. Unit checks on odi_switch_sdkinit_verb() own contract: an unknown
 *    verb (e.g. "intr"/"irq", never generated), a known verb with zero
 *    events for this capture ("trunk"/"gpio" in the reference capture --
 *    a real zero-write boot phase, not a generation gap), and the `switch`
 *    verb own
 *    SoC-window PBO IP-enable write (0xb800063c) landing BEFORE every
 *    write into the PBO block it gates -- against the REAL generated
 *    table, sdkinit.bin as built from tools/regtrace/tables/, not synthetic
 *    data, since the table own shape (which verbs are populated) is
 *    exactly what these checks need to describe. Every verb call that
 *    loads the file must also release it.
 * 2. `dump` mode: replays EVERY verb in odi_switch_sdkinit_verb_names[]
 *    order through the mock, one before/after mark bracket per verb (tag == the verb own index, the
 *    same order tools/regtrace/replayblob.py own SDKINIT_VERBS and the
 *    enum in odi_switch_sdkinit.h use), and writes a ring-dump file compare.py can
 *    read -- odi_switch_sdkinit_replay_test.sh runs this and diffs the
 *    result, bracket by bracket, against test/fixtures/
 *    isp1-260923-g4-sdkinit-filtered.txt (one bracket per verb, in the
 *    raw capture own order, R kept in its own run form -- never
 *    expanded, same convention compare.py own bracket mode already uses).
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_sdkinit.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* The verb index of name, or -1 when the table has no such verb. */
static int find_verb(const char *name)
{
	unsigned int i;

	for (i = 0; i < ODI_SDKINIT_VERB_ID_COUNT; i++)
		if (strcmp(odi_switch_sdkinit_verb_names[i], name) == 0)
			return (int)i;
	return -1;
}

/* Records sdkinit.bin carries for one verb, or -1 when it does not load. */
static int verb_record_count(int id)
{
	struct odi_replay_fw fw;
	struct odi_replay_event e;
	uint32_t i;
	int n = 0;

	if (id < 0 || odi_replay_fw_load(ODI_REPLAY_TABLE_SDKINIT, &fw))
		return -1;
	for (i = 0; i < fw.blob.count; i++) {
		odi_replay_blob_event(&fw.blob, i, &e);
		if (e.verb == (uint8_t)id)
			n++;
	}
	odi_replay_fw_release(&fw);
	return n;
}

/* intr/irq are never generated (a later phase owns them, not a data
 * replay) -- odi_switch_sdkinit_verb() must refuse them, not crash or
 * silently invent an entry.
 */
static void test_unknown_verb_refused(void)
{
	odi_mock_reset();
	CHECK(find_verb("intr") < 0, "test setup: intr is not in the verb table");
	CHECK(odi_switch_sdkinit_verb("intr") == -ENOENT, "unknown verb (intr) returns -ENOENT");
	CHECK(odi_mock.log_n == 0, "unknown verb (intr): nothing touched the mock");
}

/* trunk and gpio: the reference capture holds no writes for them, a boot
 * phase that writes nothing. They succeed and touch nothing, so rcS does
 * not log a failure for them on every boot.
 */
static void test_zero_event_verb_ok(void)
{
	static const char *const empty[] = { "trunk", "gpio" };
	unsigned int k;

	for (k = 0; k < 2; k++) {
		int id = find_verb(empty[k]);

		CHECK(id >= 0, "test setup: the verb is in the verb table");
		CHECK(verb_record_count(id) == 0, "test setup: the verb has zero events in this capture");
		odi_mock_reset();
		CHECK(odi_switch_sdkinit_verb(empty[k]) == 0, "zero-write verb (trunk, gpio) returns 0");
		CHECK(odi_mock.log_n == 0, "zero-write verb (trunk, gpio): nothing touched the mock");
	}
}

/* The list of verbs that may be empty matches the table both ways: a verb
 * that lost its records (a generation gap) is not on it and fails, and a
 * verb on it that gained records is noticed here so the list is trimmed.
 */
static void test_empty_list_matches_table(void)
{
	unsigned int id;

	for (id = 0; id < ODI_SDKINIT_VERB_ID_COUNT; id++) {
		int n = verb_record_count((int)id);

		CHECK(n >= 0, "test setup: sdkinit.bin loads");
		if ((n == 0) != odi_switch_sdkinit_verb_may_be_empty(id)) {
			fprintf(stderr, "  verb %s: %d records, may_be_empty %d\n",
				odi_switch_sdkinit_verb_names[id], n,
				odi_switch_sdkinit_verb_may_be_empty(id));
			CHECK(0, "a verb has no records exactly when it is listed as may be empty");
		}
	}
}

/* The trial f1 bug, pinned down: the `switch` verb own replay must apply the
 * SoC-window PBO IP-enable write (kind 'w', 0xb800063c) BEFORE any write
 * into the PBO block itself (0x00f0xxxx, PONQ_0xf02190 among them) -- in
 * that order, applying the enable after (or never) is exactly what stalled
 * the bus with interrupts off and the hardware watchdog unable to recover.
 * Checked
 * against the real generated table (sdkinit.bin), not
 * synthetic data, because the ordering itself -- not just the presence of
 * the write somewhere -- is the whole fix.
 */
static void test_switch_verb_soc_enable_before_pbo_write(void)
{
	unsigned int i;
	int enable_idx = -1;
	int first_pbo_idx = -1;

	CHECK(verb_record_count(find_verb("switch")) > 0, "test setup: switch has events in this capture");

	odi_mock_reset();
	CHECK(odi_switch_sdkinit_verb("switch") == 0, "switch verb replays");

	for (i = 0; i < odi_mock.log_n; i++) {
		struct odi_mock_write *e = &odi_mock.log[i];

		if (e->kind == 'w' && e->addr == 0xb800063cU && enable_idx < 0)
			enable_idx = (int)i;
		if (e->kind == 'W' && (e->addr & 0xfff00000U) == 0x00f00000U && first_pbo_idx < 0)
			first_pbo_idx = (int)i;
	}

	CHECK(enable_idx == 0,
	      "switch verb: the SoC PBO IP-enable write (0xb800063c) is the very first thing applied");
	CHECK(first_pbo_idx > enable_idx,
	      "switch verb: every write into the PBO block (0x00f0xxxx) comes after the IP-enable write");
}

/* No sdkinit.bin (a scratch directory with nothing in it): the verb fails
 * with the loader error, not -ENOENT-as-success, and touches nothing.
 */
static void test_missing_table_refused(void)
{
	odi_replay_fw_host_dir = "/nonexistent-odi-replay-fw-dir";
	odi_mock_reset();
	CHECK(odi_switch_sdkinit_verb("switch") != 0, "switch verb without sdkinit.bin fails");
	CHECK(odi_mock.log_n == 0, "switch verb without sdkinit.bin: nothing touched the mock");
	odi_replay_fw_host_dir = NULL;
}

static int run_unit_checks(void)
{
	test_unknown_verb_refused();
	test_zero_event_verb_ok();
	test_empty_list_matches_table();
	test_switch_verb_soc_enable_before_pbo_write();
	test_missing_table_refused();
	CHECK(odi_replay_fw_host_loads > 0 &&
	      odi_replay_fw_host_loads == odi_replay_fw_host_releases,
	      "every sdkinit.bin load was released");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_sdkinit_test: ok\n");
	return 0;
}

static int run_dump(const char *out_path)
{
	FILE *f = fopen(out_path, "w");
	unsigned int i;

	if (!f) {
		fprintf(stderr, "odi_switch_sdkinit_test: cannot open %s for writing\n", out_path);
		return 1;
	}

	odi_mock_reset();

	for (i = 0; i < ODI_SDKINIT_VERB_ID_COUNT; i++) {
		odi_mock_mark(i);
		(void)odi_switch_sdkinit_verb(odi_switch_sdkinit_verb_names[i]);
		odi_mock_mark(i | 0x80000000U);
	}

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
