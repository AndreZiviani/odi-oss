/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_sdkinit_test.c -- two things:
 *
 * 1. Unit checks on odi_switch_sdkinit_verb() own contract: an unknown
 *    verb (e.g. "intr"/"irq", never generated), a known verb with zero
 *    events for this capture ("trunk"/"gpio" in the reference capture --
 *    a real zero-write boot phase, not a generation gap), the mask
 *    gate (clearing one verb own bit skips its replay and touches nothing,
 *    setting it back applies normally), and the `switch` verb own
 *    SoC-window PBO IP-enable write (0xb800063c) landing BEFORE every
 *    write into the PBO block it gates -- against the REAL generated
 *    table, rootfs/skeleton/lib/firmware/odi/sdkinit.bin, not synthetic
 *    data, since the table own shape (which verbs are populated) is
 *    exactly what these checks need to describe. Every verb call that
 *    loads the file must also release it.
 * 2. `dump` mode: replays EVERY verb in odi_switch_sdkinit_verb_names[]
 *    order through the mock, mask = ODI_SW_SDKINIT_MASK_ALL, one
 *    before/after mark bracket per verb (tag == the verb own index, the
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

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
/* odi_switch_dal.c unconditionally defines odi_switch_init_modload(),
 * which decodes records through odi_replay_blob.c; odi_switch_sdkinit.c
 * loads sdkinit.bin through odi_replay_fw_load(), here the host half
 * (odi_replay_fw_host.h), reading the same file the image ships.
 */
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"
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
	struct odi_sw_modload_event e;
	uint32_t i;
	uint8_t verb;
	int n = 0;

	if (id < 0 || odi_replay_fw_load(ODI_REPLAY_TABLE_SDKINIT, &fw))
		return -1;
	for (i = 0; i < fw.blob.count; i++) {
		odi_replay_blob_switch_event(&fw.blob, i, &e, &verb);
		if (verb == (uint8_t)id)
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

/* A verb the table knows about but the reference capture recorded zero
 * events for (trunk, gpio) must ALSO refuse -- same as unknown, so
 * the /proc/rtk_init dispatch logs -ENOSYS either way.
 */
static void test_zero_event_verb_refused(void)
{
	int id = find_verb("trunk");

	CHECK(id >= 0, "test setup: trunk is in the verb table");
	CHECK(verb_record_count(id) == 0, "test setup: trunk has zero events in this capture");

	odi_mock_reset();
	CHECK(odi_switch_sdkinit_verb("trunk") == -ENOENT, "zero-event verb (trunk) returns -ENOENT");
	CHECK(odi_mock.log_n == 0, "zero-event verb (trunk): nothing touched the mock");
}

/* A verb with real events applies when its mask bit is set, and refuses
 * (touching nothing) when it is cleared -- the bisection contract
 * odi_switch_sdkinit.h documents.
 */
static void test_mask_gate(void)
{
	int bit = find_verb("svlan");
	unsigned int loads;

	CHECK(verb_record_count(bit) > 0, "test setup: svlan has events in this capture");
	CHECK(bit == ODI_SDKINIT_SVLAN, "test setup: svlan own bit index was found");

	odi_switch_sdkinit_mask_set(ODI_SW_SDKINIT_MASK_ALL & ~ODI_SW_SDKINIT_ITEM(bit));
	odi_mock_reset();
	loads = odi_replay_fw_host_loads;
	CHECK(odi_switch_sdkinit_verb("svlan") == -ENOENT, "svlan with its own bit clear returns -ENOENT");
	CHECK(odi_mock.log_n == 0, "svlan with its own bit clear: nothing touched the mock");
	CHECK(odi_replay_fw_host_loads == loads, "svlan with its own bit clear: sdkinit.bin not even loaded");

	odi_switch_sdkinit_mask_set(ODI_SW_SDKINIT_MASK_ALL);
	odi_mock_reset();
	CHECK(odi_switch_sdkinit_verb("svlan") == 0, "svlan with every bit set returns 0");
	CHECK(odi_mock.log_n > 0, "svlan with every bit set: something touched the mock");
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

	odi_switch_sdkinit_mask_set(ODI_SW_SDKINIT_MASK_ALL);
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

/* odi_switch_sdkinit_mask_get()/_set() round-trip, and the default is
 * every verb this table knows about.
 */
static void test_mask_default_and_roundtrip(void)
{
	CHECK(odi_switch_sdkinit_mask_get() == ODI_SW_SDKINIT_MASK_ALL,
	      "default mask is ODI_SW_SDKINIT_MASK_ALL (module-level default, first check to run)");

	odi_switch_sdkinit_mask_set(0x00000001U);
	CHECK(odi_switch_sdkinit_mask_get() == 0x00000001U, "mask_set/mask_get round-trip");
	odi_switch_sdkinit_mask_set(ODI_SW_SDKINIT_MASK_ALL);
}

/* No sdkinit.bin (a scratch directory with nothing in it): the verb fails
 * with the loader error, not -ENOENT-as-success, and touches nothing.
 */
static void test_missing_table_refused(void)
{
	odi_switch_sdkinit_mask_set(ODI_SW_SDKINIT_MASK_ALL);
	odi_replay_fw_host_dir = "/nonexistent-odi-replay-fw-dir";
	odi_mock_reset();
	CHECK(odi_switch_sdkinit_verb("switch") != 0, "switch verb without sdkinit.bin fails");
	CHECK(odi_mock.log_n == 0, "switch verb without sdkinit.bin: nothing touched the mock");
	odi_replay_fw_host_dir = NULL;
}

static int run_unit_checks(void)
{
	test_mask_default_and_roundtrip();
	test_unknown_verb_refused();
	test_zero_event_verb_refused();
	test_mask_gate();
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
	odi_switch_sdkinit_mask_set(ODI_SW_SDKINIT_MASK_ALL);

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
