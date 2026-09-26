/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_reg_test.c -- unit test for the three leaves behind /dev/odi_sw
 * (odi_switch_mib.c: odi_sw_reg_get/_set, odi_sw_mib_get), the phase 3f
 * replacement for the stock RTK_OPT_REGISTER/ADDRESS_GET/SET/STAT_PORT
 * sockopts. Same unity-build shape as odi_switch_dal_test.c (this file
 * needs odi_switch_tbl.c/odi_replay_blob.c linked in for the same
 * reason that one does: the switch core is one translation unit here).
 *
 * odi_sw_mib_get()'s register addresses are checked directly against the
 * ODI_SW_PORT_*_COUNTERS() macros (odi_switch_hw.h) rather than restating
 * the offsets as literals, so a future change to the block base or the
 * per-port stride only has to stay consistent with itself, not with a
 * second hand-copied set of numbers here.
 */
#include <stdio.h>

#include "odi_switch_unity.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

int main(void)
{
	uint32_t v;
	uint64_t v64;

	/* odi_sw_reg_get/_set: a plain word at an in-bounds offset round-trips,
	 * and both refuse an offset the real ioremap does not cover or one
	 * that is not 4-byte aligned -- the same two refusals odi_reg_read()/
	 * odi_reg_write() enforce, checked here at the leaf's own boundary
	 * rather than only indirectly through the mock.
	 */
	CHECK(odi_sw_reg_set(0x1c020U, 0xdeadbeefU) == 0, "reg_set accepts an in-bounds offset");
	CHECK(odi_sw_reg_get(0x1c020U, &v) == 0 && v == 0xdeadbeefU,
	      "reg_get reads back what reg_set wrote");
	CHECK(odi_sw_reg_get(ODI_SWITCH_MMIO_SIZE, &v) != 0,
	      "reg_get refuses an offset the real ioremap does not cover");
	CHECK(odi_sw_reg_set(ODI_SWITCH_MMIO_SIZE, 1U) != 0,
	      "reg_set refuses the same out-of-bounds offset");
	CHECK(odi_sw_reg_get(0x1c021U, &v) != 0, "reg_get refuses an unaligned offset");
	CHECK(odi_sw_reg_set(0x1c021U, 1U) != 0, "reg_set refuses an unaligned offset");

	/* odi_sw_mib_get(): a plain 32-bit counter (etherStatsTxMulticastPkts,
	 * mib.h index 53, PORT_TX_COUNTERS row 0) and a 64-bit combined
	 * counter (ifOutOctets, mib.h index 5, TX_MIB rows 10/11, low word
	 * first) on port 2, plus the two ways this leaf must refuse: a port
	 * this chip does not have, and a counter index with no register row
	 * (etherStatsOctets, mib.h index 27 -- left out deliberately, see
	 * the odi_switch_mib.c mapping-table comment).
	 *
	 * The row10/row11 pair below (0x2 hi, 0x1 lo -- not a hi==lo value
	 * that a swapped read would still pass by accident) pins the fix for
	 * the word-swap bug found on isp1 f5c: `mib dump counter port 2`
	 * read ifInOctets back as 0xb3a800000000 (the true ~46000-octet count
	 * shifted into the high word) because an earlier version of this
	 * leaf combined the pair as {row10 high, row11 low} when the register
	 * map actually lists the low word first.
	 */
	odi_reg_write(ODI_SW_PORT_TX_COUNTERS(2, 0), 0x1234U);
	CHECK(odi_sw_mib_get(2, 53, &v64) == 0 && v64 == 0x1234U,
	      "mib_get reads etherStatsTxMulticastPkts (TX row 0) on port 2");

	odi_reg_write(ODI_SW_PORT_TX_COUNTERS(2, 10), 0x1U);   /* ifOutOctets, low word */
	odi_reg_write(ODI_SW_PORT_TX_COUNTERS(2, 11), 0x2U);   /* ifOutOctets, high word */
	CHECK(odi_sw_mib_get(2, 5, &v64) == 0 && v64 == 0x200000001ULL,
	      "mib_get combines ifOutOctets as {row10 low, row11 high}");
	CHECK(odi_sw_mib_get(2, 42, &v64) == 0 && v64 == 0x200000001ULL,
	      "etherStatsTxOctets (index 42) reads the same counter as ifOutOctets");

	CHECK(odi_sw_mib_get(4, 53, &v64) != 0, "mib_get refuses port 4 (only 0-3 exist)");
	CHECK(odi_sw_mib_get(2, 27, &v64) != 0,
	      "mib_get refuses etherStatsOctets (27), left unmapped -- ambiguous stock diag dup");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_reg_test: all checks passed\n");
	return 0;
}
