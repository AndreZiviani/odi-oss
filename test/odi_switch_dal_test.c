/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_dal_test.c -- dumps one ring-dump bracket per implemented
 * switch-core leaf (odi_switch_dal.h), using the parameters recovered from the boot5
 * instance named in each fixture's header comment
 * (test/fixtures/dal-cmd*.txt). odi_switch_dal_test.sh runs compare.py
 * fixture-vs-actual per leaf and requires every one to PASS. The ten
 * leaves from part 1 were originally checked against boot3 (same
 * content, different line numbers -- confirmed identical this pass);
 * their fixtures now cite boot5 lines throughout, one source of truth.
 * Part 2 adds cmd 51's table payload: twelve instances
 * (dal-cmd51-<n>.txt), one per boot5 bracket, since the CF-triple count
 * and the VLAN per-connection rows both grow bracket to bracket.
 *
 * Usage: odi_switch_dal_test <out-dir>. One file per leaf/instance is
 * written as <out-dir>/dal-cmd<NAME>.txt, same basenames as the fixtures.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"

static void dump_bracket(const char *out_dir, const char *name, uint32_t cmd)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", out_dir, name);
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "odi_switch_dal_test: cannot open %s\n", path);
		return;
	}
	fprintf(f, "# actual write log, cmd %u\n", cmd);
	odi_mock_dump(f);
	fclose(f);
}

// ---- bracket 1 (lines 231-432) ----
static const struct odi_sw_cf_entry cf1[] = {
	{ 254, 1, 0x00010000U, 0x00000000U, 0x00000000U, 0x8000001fU, 0x00000000U, 0x0024005aU, 0x04a40004U },
	{ 64, 0, 0x00010000U, 0x80005808U, 0x00000000U, 0x007fa710U, 0x00000000U, 0x00240005U, 0x88240003U },
};
static const struct odi_sw_vlan_override vo1[] = {
	{ 11, 0x00000015U },
};
// n_cf=2 n_over=1

// ---- bracket 2 (lines 433-623) ----
static const struct odi_sw_cf_entry cf2[] = {
	{ 64, 0, 0x00010000U, 0x80005808U, 0x00010000U, 0x007fa710U, 0x00000000U, 0x00240005U, 0x88240003U },
};
static const struct odi_sw_vlan_override vo2[] = {
	{ 11, 0x00000015U },
};
// n_cf=1 n_over=1

// ---- bracket 3 (lines 624-830) ----
static const struct odi_sw_cf_entry cf3[] = {
	{ 65, 1, 0x00010000U, 0x00007000U, 0x00000000U, 0x807f8807U, 0x00000000U, 0x00240006U, 0x03a40004U },
	{ 66, 0, 0x00010000U, 0x80007000U, 0x00000000U, 0x007f8800U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo3[] = {
	{ 11, 0x00000015U },
	{ 14, 0x00000005U },
};
// n_cf=2 n_over=2

// ---- bracket 4 (lines 831-1026) ----
static const struct odi_sw_cf_entry cf4[] = {
	{ 66, 0, 0x00010000U, 0x80007000U, 0x00010000U, 0x007f8800U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo4[] = {
	{ 11, 0x00000015U },
	{ 14, 0x00000005U },
};
// n_cf=1 n_over=2

// ---- bracket 5 (lines 1027-1277) ----
static const struct odi_sw_cf_entry cf5[] = {
	{ 67, 0, 0x00010000U, 0x80007000U, 0x00000000U, 0x007f8800U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 66, 1, 0x00010000U, 0x00007000U, 0x00010000U, 0x807f8807U, 0x00000000U, 0x00240006U, 0x03a40004U },
	{ 65, 1, 0x00010000U, 0x00006c00U, 0x00010000U, 0x807f9307U, 0x00000000U, 0x00240006U, 0x02a40004U },
	{ 68, 0, 0x00010000U, 0x80007000U, 0x00000000U, 0x007f8800U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 67, 1, 0x00010000U, 0x00007000U, 0x00010000U, 0x807f8807U, 0x00000000U, 0x00240006U, 0x03a40004U },
	{ 66, 0, 0x00010000U, 0x80006c00U, 0x00010000U, 0x007f9300U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo5[] = {
	{ 11, 0x00000015U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=6 n_over=3

// ---- bracket 6 (lines 1278-1473) ----
static const struct odi_sw_cf_entry cf6[] = {
	{ 66, 0, 0x00010000U, 0x80006c00U, 0x00010000U, 0x007f9300U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo6[] = {
	{ 11, 0x00000015U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=1 n_over=3

// ---- bracket 7 (lines 1474-1722) ----
static const struct odi_sw_cf_entry cf7[] = {
	{ 69, 0, 0x00010000U, 0x80007000U, 0x00000000U, 0x007f8800U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 68, 1, 0x00010000U, 0x00007000U, 0x00010000U, 0x807f8807U, 0x00000000U, 0x00240006U, 0x03a40004U },
	{ 67, 1, 0x00010000U, 0x00006500U, 0x00010000U, 0x807f9a07U, 0x00000000U, 0x00240006U, 0x01a40004U },
	{ 70, 0, 0x00010000U, 0x80007000U, 0x00000000U, 0x007f8800U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 69, 1, 0x00010000U, 0x00007000U, 0x00010000U, 0x807f8807U, 0x00000000U, 0x00240006U, 0x03a40004U },
	{ 68, 0, 0x00010000U, 0x80006500U, 0x00010000U, 0x007f9a00U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo7[] = {
	{ 11, 0x00000015U },
	{ 12, 0x00000005U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=6 n_over=4

// ---- bracket 8 (lines 1723-1916) ----
static const struct odi_sw_cf_entry cf8[] = {
	{ 68, 0, 0x00010000U, 0x80006500U, 0x00010000U, 0x007f9a00U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo8[] = {
	{ 11, 0x00000015U },
	{ 12, 0x00000005U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=1 n_over=4

// ---- bracket 9 (lines 1917-2123) ----
static const struct odi_sw_cf_entry cf9[] = {
	{ 71, 1, 0x00010000U, 0x00005000U, 0x00000000U, 0x807fa807U, 0x00000000U, 0x00240006U, 0x00a40004U },
	{ 72, 0, 0x00010000U, 0x80005000U, 0x00000000U, 0x007fa800U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo9[] = {
	{ 10, 0x00000005U },
	{ 11, 0x00000015U },
	{ 12, 0x00000005U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=2 n_over=5

// ---- bracket 10 (lines 2124-2319) ----
static const struct odi_sw_cf_entry cf10[] = {
	{ 72, 0, 0x00010000U, 0x80005000U, 0x00010000U, 0x007fa800U, 0x00000000U, 0x00240007U, 0x88240004U },
};
static const struct odi_sw_vlan_override vo10[] = {
	{ 10, 0x00000005U },
	{ 11, 0x00000015U },
	{ 12, 0x00000005U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=1 n_over=5

// ---- bracket 11 (lines 2320-2603) ----
static const struct odi_sw_cf_entry cf11[] = {
	{ 73, 0, 0x00010000U, 0x80005000U, 0x00000000U, 0x007fa800U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 72, 1, 0x00010000U, 0x00005000U, 0x00010000U, 0x807fa807U, 0x00000000U, 0x00240006U, 0x00a40004U },
	{ 71, 0, 0x00010000U, 0x80007000U, 0x00010000U, 0x007f8800U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 70, 1, 0x00010000U, 0x00007000U, 0x00010000U, 0x807f8807U, 0x00000000U, 0x00240006U, 0x03a40004U },
	{ 69, 0, 0x00010000U, 0x80006500U, 0x00010000U, 0x007f9a00U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 68, 1, 0x00010000U, 0x00006500U, 0x00010000U, 0x807f9a07U, 0x00000000U, 0x00240006U, 0x01a40004U },
	{ 67, 0, 0x00010000U, 0x80006c00U, 0x00010000U, 0x007f9300U, 0x00000000U, 0x00240007U, 0x88240004U },
	{ 66, 1, 0x00010000U, 0x00006c00U, 0x00010000U, 0x807f9307U, 0x00000000U, 0x00240006U, 0x02a40004U },
	{ 65, 0, 0x00010000U, 0x80000008U, 0x00010000U, 0x007fff10U, 0x00000000U, 0x0024005dU, 0x88240003U },
};
static const struct odi_sw_vlan_override vo11[] = {
	{ 10, 0x00000005U },
	{ 11, 0x00000015U },
	{ 12, 0x00000005U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=9 n_over=5

// ---- bracket 12 (lines 2604-2799) ----
static const struct odi_sw_cf_entry cf12[] = {
	{ 65, 0, 0x00010000U, 0x80000008U, 0x00010000U, 0x007fff10U, 0x00000000U, 0x0024005dU, 0x98240003U },
};
static const struct odi_sw_vlan_override vo12[] = {
	{ 10, 0x00000005U },
	{ 11, 0x00000015U },
	{ 12, 0x00000005U },
	{ 13, 0x00000005U },
	{ 14, 0x00000005U },
};
// n_cf=1 n_over=5


int main(int argc, char **argv)
{
	const char *out_dir;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <out-dir>\n", argv[0]);
		return 2;
	}
	out_dir = argv[1];

	/* cmd 62 -- setAgeingTime, boot5 lines 4-6. */
	odi_mock_reset();
	odi_mock_mark(62);
	odi_sw_l2_aging_set(3000, 1);
	odi_mock_mark(0x80000000U | 62U);
	dump_bracket(out_dir, "dal-cmd62.txt", 62);

	/* cmd 30 -- setPortAutoNegoAbility, boot5 lines 7-23. */
	odi_mock_reset();
	odi_mock_mark(30);
	odi_sw_port_autoneg_get();
	odi_sw_port_autoneg_set(0xde1, 0x0, 0x3a00);
	odi_mock_mark(0x80000000U | 30U);
	dump_bracket(out_dir, "dal-cmd30.txt", 30);

	/* cmd 32 -- setPortState (admin enable + MAC force ability),
	 * boot5 lines 24-26.
	 */
	odi_mock_reset();
	odi_mock_mark(32);
	(void)odi_sw_port_force_get(0);
	odi_sw_port_force_set(0);
	odi_mock_mark(0x80000000U | 32U);
	dump_bracket(out_dir, "dal-cmd32.txt", 32);

	/* cmd 64 -- setFloodingPortMask, boot5 lines 29-34. */
	odi_mock_reset();
	odi_mock_mark(64);
	(void)odi_sw_l2_flood_mask_get();
	odi_sw_l2_flood_mask_set(7);
	odi_mock_mark(0x80000000U | 64U);
	dump_bracket(out_dir, "dal-cmd64.txt", 64);

	/* cmd 23, instance 1 -- setPriQueue baseline, boot5 lines 73-75. */
	odi_mock_reset();
	odi_mock_mark(23);
	odi_sw_ponmac_queue_add(0xd4);
	odi_mock_mark(0x80000000U | 23U);
	dump_bracket(out_dir, "dal-cmd23-base.txt", 23);

	/* cmd 23, instance 9 -- setPriQueue counter-mask shape,
	 * boot5 lines 143-151.
	 */
	odi_mock_reset();
	odi_mock_mark(23);
	odi_sw_ponmac_queue_add_ext(0, 0x10001, 0x6, 0x1, 0, 0x100401, 0);
	odi_mock_mark(0x80000000U | 23U);
	dump_bracket(out_dir, "dal-cmd23-ext.txt", 23);

	/* cmd 10, instance 1 -- getTransceiverStatus, boot5 lines 99-106. */
	odi_mock_reset();
	odi_mock_mark(10);
	odi_sw_ponmac_transceiver_get(0x62, 0x63);
	odi_mock_mark(0x80000000U | 10U);
	dump_bracket(out_dir, "dal-cmd10.txt", 10);

	/* cmd 25, DS instance 1 -- cfgGemFlow DS side, boot5 lines 35-40. */
	odi_mock_reset();
	odi_mock_mark(25);
	odi_sw_gpon_usflow_set(0, 0xfff, 3);
	odi_mock_mark(0x80000000U | 25U);
	dump_bracket(out_dir, "dal-cmd25-ds.txt", 25);

	/* cmd 25, US instance 7 -- cfgGemFlow US side, boot5 lines 196-202. */
	odi_mock_reset();
	odi_mock_mark(25);
	odi_sw_ponmac_flow_queue_set(0, 0x59a, 1, 0x1ee01b08, 0x1ee01e40, 0x07efdf80, 0);
	odi_mock_mark(0x80000000U | 25U);
	dump_bracket(out_dir, "dal-cmd25-us.txt", 25);

	/* cmd 51, all twelve boot5 instances -- classifier entry add,
	 * complete: W + table payload. Per-instance CF triples and VLAN
	 * row overrides recovered mechanically from boot5 (a small script,
	 * not hand-transcribed -- see the cfN[]/voN[] arrays below).
	 */
	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf1, sizeof(cf1) / sizeof(cf1[0]),
				     vo1, sizeof(vo1) / sizeof(vo1[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-1.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf2, sizeof(cf2) / sizeof(cf2[0]),
				     vo2, sizeof(vo2) / sizeof(vo2[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-2.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf3, sizeof(cf3) / sizeof(cf3[0]),
				     vo3, sizeof(vo3) / sizeof(vo3[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-3.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf4, sizeof(cf4) / sizeof(cf4[0]),
				     vo4, sizeof(vo4) / sizeof(vo4[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-4.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf5, sizeof(cf5) / sizeof(cf5[0]),
				     vo5, sizeof(vo5) / sizeof(vo5[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-5.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf6, sizeof(cf6) / sizeof(cf6[0]),
				     vo6, sizeof(vo6) / sizeof(vo6[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-6.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf7, sizeof(cf7) / sizeof(cf7[0]),
				     vo7, sizeof(vo7) / sizeof(vo7[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-7.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf8, sizeof(cf8) / sizeof(cf8[0]),
				     vo8, sizeof(vo8) / sizeof(vo8[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-8.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf9, sizeof(cf9) / sizeof(cf9[0]),
				     vo9, sizeof(vo9) / sizeof(vo9[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-9.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf10, sizeof(cf10) / sizeof(cf10[0]),
				     vo10, sizeof(vo10) / sizeof(vo10[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-10.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf11, sizeof(cf11) / sizeof(cf11[0]),
				     vo11, sizeof(vo11) / sizeof(vo11[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-11.txt", 51);

	odi_mock_reset();
	odi_mock_mark(51);
	odi_sw_cf_add(cf12, sizeof(cf12) / sizeof(cf12[0]),
				     vo12, sizeof(vo12) / sizeof(vo12[0]), 0);
	odi_mock_mark(0x80000000U | 51U);
	dump_bracket(out_dir, "dal-cmd51-12.txt", 51);

	return 0;
}
