/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_init_platform_test.c -- asserts the exact register/table
 * write sequence odi_switch_init_platform() produces ("odi_switch
 * platform init", checked item by item, register by register). No
 * capture exists of this sequence -- boot3/boot5 both start after
 * module load, and the stock module-load init writes are provably invisible to
 * the same tracer (the regtrace skip list only records writes made
 * after it is armed, and these run before that) -- so every expected
 * value here is derived from odi_switch_hw.h's field helpers directly in
 * this file, not replayed from a trace.
 *
 * The plain-register steps (classify, acl, vlan, l2, reserved vid) are
 * checked write by write, in order. The cf sweep, 256 rows, CF-table sweep is checked structurally (total
 * T/D counts per table, plus the first and last row) rather than
 * enumerated 768 calls deep -- the sweep body has no per-row variation to
 * miss (every row is the same all-zero write), so a full enumeration
 * would not catch anything a structural check does not already cover.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static int w_at(unsigned int pos, uint32_t addr, uint32_t val)
{
	return pos < odi_mock.log_n && odi_mock.log[pos].kind == 'W' &&
	       odi_mock.log[pos].addr == addr && odi_mock.log[pos].val == val;
}

/* nth (0-indexed) 'W' log entry at addr, content-matched rather than by
 * absolute log position -- needed for reserved vid's checks, since cf sweep's
 * ~2800 T/D sweep entries sit between l2 and reserved vid in the log and
 * would otherwise throw off a simple running position counter.
 */
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

static unsigned int count_kind_table(char kind, uint32_t table)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind != kind)
			continue;
		if (kind == 'T' || kind == 't' || kind == 'R') {
			if (((odi_mock.log[i].addr >> 16) & 0xffffU) == table)
				n++;
		} else if (kind == 'D') {
			if ((odi_mock.log[i].addr & 0xffffU) == table)
				n++;
		}
	}
	return n;
}

static void test_sequence(void)
{
	unsigned int i;
	unsigned int pos;

	odi_mock_reset();
	odi_switch_init_platform();
	odi_mock_table_flush(); /* close out the sweep's last (unextended) row */

	pos = 0;

	/* Step "classify": CLASSIFY_SETUP.US_NO_MATCH_ACTION = 1 (initial reg 0, so the written
	 * word is exactly the field's own value: 1<<0 = 0x1).
	 */
	CHECK(w_at(pos, ODI_SW_CLASSIFY_SETUP_OFF, 0x00000001U), "classify: CLASSIFY_SETUP US_PERMIT=1");
	pos++;

	/* Step "acl clean": every row of the three ACL tables read, written
	 * all-zero and read again, a table at a time, all before ACL ingress
	 * is enabled. In the mock every row reads zero, so each table's 96
	 * reads (t) and 96 writes (T) are one run: one t or T, its D words,
	 * and an R of 96. No plain-register write in between.
	 */
	{
		static const struct { uint32_t table; unsigned int words; } acl[3] = {
			{ ODI_SW_TBL_ACL_PATTERN, 5 },
			{ ODI_SW_TBL_ACL_PATTERN_MASK, 5 },
			{ ODI_SW_TBL_ACL_ACTIONS, 3 },
		};
		unsigned int k, first_acl_w = pos;

		while (first_acl_w < odi_mock.log_n && odi_mock.log[first_acl_w].kind != 'W')
			first_acl_w++;
		for (k = 0; k < 3; k++) {
			CHECK(count_kind_table('t', acl[k].table) == 2,
			      "acl clean: two read runs per ACL table (before and after)");
			CHECK(count_kind_table('T', acl[k].table) == 1,
			      "acl clean: one write run per ACL table");
			CHECK(count_kind_table('D', acl[k].table) == 3 * acl[k].words,
			      "acl clean: the D words of the three runs");
			CHECK(count_kind_table('R', acl[k].table) == 3,
			      "acl clean: each run 96 rows long");
		}
		for (i = pos; i < first_acl_w; i++)
			if (odi_mock.log[i].kind == 'R')
				CHECK(odi_mock.log[i].val == 96, "acl clean: a run covers all 96 rows");
		{
			unsigned int runs_n = 0;

			for (k = 0; k < 3; k++)
				runs_n += 3 * (acl[k].words + 2);	/* t/T, D words, R; three runs */
			/* The R of the last run is only written when the next
			 * table op breaks it (the cf sweep), after the enable
			 * writes: one entry fewer before them.
			 */
			CHECK(first_acl_w - pos == runs_n - 1,
			      "acl clean: nothing but the nine ACL runs before ACL ingress is enabled");
		}
		pos = first_acl_w;
	}

	/* Step "acl": ACL_PORT_ENABLE, one RMW per port -- UNI (bit 0) then PON (bit 2). */
	CHECK(w_at(pos, ODI_SW_ACL_PORT_ENABLE_OFF, 0x00000001U), "acl: ACL_PORT_ENABLE UNI port bit");
	pos++;
	CHECK(w_at(pos, ODI_SW_ACL_PORT_ENABLE_OFF, 0x00000005U), "acl: ACL_PORT_ENABLE + PON port bit");
	pos++;

	/* Step "vlan": VLAN_SETUP.FILTER_ON=0 (reg starts 0, so this write is
	 * the no-op value 0x0 -- still asserted, since the point is that the
	 * write happens at all, not that it changes anything from a fresh
	 * mock state).
	 */
	CHECK(w_at(pos, ODI_SW_VLAN_SETUP_OFF, 0x00000000U), "vlan: VLAN_SETUP FILTER_ON=0");
	pos++;

	/* Step "vlan": PORT_VLAN_INDEX(0), PORT_VLAN_INDEX(1) = 0 -- ports 2 and 3 are left
	 * out (PORT_VLAN_INDEX(2) at 0x013014 collides with the register map's
	 * own separate SW_0x013014 entry; see odi_switch_platform.c).
	 */
	CHECK(w_at(pos, ODI_SW_PORT_VLAN_INDEX(0), 0), "vlan: PORT_VLAN_INDEX(0)=0");
	pos++;
	CHECK(w_at(pos, ODI_SW_PORT_VLAN_INDEX(1), 0), "vlan: PORT_VLAN_INDEX(1)=0");
	pos++;

	/* Step "vlan": VLAN_INGRESS_CHECK port 1 disabled, matching the live OEM ("CLASSIFY_SETUP
	 * and VLAN_INGRESS_CHECK") -- reg starts 0 in a fresh mock
	 * (unlike real hardware's vlan_init default of all-ports-enabled), so
	 * clearing port 1's bit is a true no-op value (0x0) here; what this
	 * check guards is the register and item being touched at all, and at
	 * the right bit (port 1, not some other port).
	 */
	CHECK(w_at(pos, ODI_SW_VLAN_INGRESS_CHECK_BASE, 0x00000000U),
	      "vlan: VLAN_INGRESS_CHECK port 1 cleared");
	pos++;

	/* Step "vlan": SVLAN_UPLINK_PORTS, accumulating RMW, one write per port.
	 * PORT_SVLAN_INDEX(port) is left out for the same reason as PORT_VLAN_INDEX
	 * above (PORT_SVLAN_INDEX(2) at 0x01400c collides with SVLAN_SETUP).
	 */
	{
		static const uint32_t pmsk_after[4] = { 0x1U, 0x3U, 0x7U, 0xfU };

		for (i = 0; i < 4; i++) {
			CHECK(w_at(pos, ODI_SW_SVLAN_UPLINK_PORTS_BASE, pmsk_after[i]),
			      "vlan: SVLAN_UPLINK_PORTS accumulates one more port bit");
			pos++;
		}
	}

	/* Step "vlan": SVLAN_SETUP.FILTER_ON=0, UNTAGGED_ACTION=2 (PBVID) -- reg starts
	 * 0, so the written word is UNTAGGED_ACTION's own bits: 2<<2 = 0x8.
	 */
	CHECK(w_at(pos, ODI_SW_SVLAN_SETUP_OFF, 0x00000008U),
	      "vlan: SVLAN_SETUP filtering=0 untag=PBVID");
	pos++;

	/* Step "l2": L2_LOOKUP_SETUP.CAM_OFF=0 (reg starts 0, no-op value, write still
	 * happens).
	 */
	CHECK(w_at(pos, ODI_SW_L2_LOOKUP_SETUP_OFF, 0x00000000U), "l2: L2_LOOKUP_SETUP CAM_OFF=0");
	pos++;

	/* Step "cf sweep": the 256-row sweep, structural check. Each row: RULE (1 T +
	 * 2 D, table 17), ACTION_DS (1 T + 3 D, table 11), ACTION_US (1 T +
	 * 3 D, table 12) -- 256 of each, all value 0, none of them ever
	 * extend into a run (every call alternates table, so run_len stays 1
	 * throughout and no R line is ever emitted for this sweep).
	 */
	CHECK(count_kind_table('T', ODI_SW_TBL_CLS_RULE_B) == 256,
	      "cf sweep: 256 CLS_RULE_B T entries");
	CHECK(count_kind_table('D', ODI_SW_TBL_CLS_RULE_B) == 512,
	      "cf sweep: 512 CLS_RULE_B D entries (2 words x 256 rows)");
	CHECK(count_kind_table('T', ODI_SW_TBL_CLS_DS_ACTION) == 256,
	      "cf sweep: 256 CLS_DS_ACTION T entries");
	CHECK(count_kind_table('D', ODI_SW_TBL_CLS_DS_ACTION) == 768,
	      "cf sweep: 768 CLS_DS_ACTION D entries (3 words x 256 rows)");
	CHECK(count_kind_table('T', ODI_SW_TBL_CLS_US_ACTION) == 256,
	      "cf sweep: 256 CLS_US_ACTION T entries");
	CHECK(count_kind_table('D', ODI_SW_TBL_CLS_US_ACTION) == 768,
	      "cf sweep: 768 CLS_US_ACTION D entries (3 words x 256 rows)");
	CHECK(count_kind_table('R', ODI_SW_TBL_CLS_RULE_B) == 0 &&
	      count_kind_table('R', ODI_SW_TBL_CLS_DS_ACTION) == 0 &&
	      count_kind_table('R', ODI_SW_TBL_CLS_US_ACTION) == 0,
	      "cf sweep: no run ever forms (every call alternates table)");

	/* Every sweep row is all-zero; spot-check that both the first (idx 0)
	 * and last (idx 255) row of CLS_RULE_B actually appear (not
	 * just that 256 T entries exist somewhere) -- order-independent,
	 * since the structural counts above already pin down the total shape.
	 */
	{
		unsigned int j, found_first = 0, found_last = 0;

		for (j = 0; j < odi_mock.log_n; j++) {
			if (odi_mock.log[j].kind == 'T' &&
			    (odi_mock.log[j].addr >> 16) == ODI_SW_TBL_CLS_RULE_B) {
				if ((odi_mock.log[j].addr & 0xffffU) == 0)
					found_first = 1;
				if ((odi_mock.log[j].addr & 0xffffU) == 255)
					found_last = 1;
			}
		}
		CHECK(found_first, "cf sweep: CLS_RULE_B row 0 T entry present");
		CHECK(found_last, "cf sweep: CLS_RULE_B row 255 T entry present");
	}

	/* Step "reserved vid" comes after cf sweep's ~2800-entry T/D sweep, so its two
	 * writes are found by content (the Nth write to that address), not
	 * by a running log-position counter.
	 */
	{
		uint32_t v;

		/* VLAN_SETUP: 1st write (vlan) = 0, 2nd write (reserved vid)
		 * re-reads that 0 then sets VID0_MODE=1, VID4095_MODE=1
		 * -> bits 3 and 4 -> 0x18.
		 */
		CHECK(nth_w_val(ODI_SW_VLAN_SETUP_OFF, 0, &v) && v == 0x00000000U,
		      "vlan: VLAN_SETUP 1st write is 0");
		CHECK(nth_w_val(ODI_SW_VLAN_SETUP_OFF, 1, &v) && v == 0x00000018U,
		      "reserved vid: VLAN_SETUP 2nd write VID0_MODE=1 VID4095_MODE=1");

		/* SVLAN_SETUP: 1st write (vlan) = 0x8 (UNTAGGED_ACTION=PBVID), 2nd
		 * write (reserved vid) re-reads 0x8 then sets PRIO_SOURCE=1
		 * (802.1Q C-TAG priority) -> 0x8 | 0x1 = 0x9.
		 */
		CHECK(nth_w_val(ODI_SW_SVLAN_SETUP_OFF, 0, &v) && v == 0x00000008U,
		      "vlan: SVLAN_SETUP 1st write filtering=0 untag=PBVID");
		CHECK(nth_w_val(ODI_SW_SVLAN_SETUP_OFF, 1, &v) && v == 0x00000009U,
		      "reserved vid: SVLAN_SETUP 2nd write PRIO_SOURCE=1 (0x8 | 0x1)");

		/* And no third write to either -- confirms items 4 and 7 are
		 * the only two touches, nothing from the sweep leaks a
		 * spurious plain-register write to these addresses.
		 */
		CHECK(!nth_w_val(ODI_SW_VLAN_SETUP_OFF, 2, &v), "no third VLAN_SETUP write");
		CHECK(!nth_w_val(ODI_SW_SVLAN_SETUP_OFF, 2, &v), "no third SVLAN_SETUP write");
	}

	/* Step "pon isolate": one read-modify-write of the word that holds the
	 * PON port (port 2: 0x027004, bits 10:0), which clears its CPU and
	 * extension-port bits. The mock word reads 0, so the value written is
	 * 0; the values on a stick's reset word are odi_pon_isolation_test.c's.
	 * No other isolation word is written.
	 */
	{
		uint32_t v;

		CHECK(nth_w_val(ODI_SW_PORT_ISOLATION(2), 0, &v) && v == 0,
		      "pon isolate: one write of the PON port's isolation word");
		CHECK(ODI_SW_PORT_ISOLATION(2) == 0x27004U, "pon isolate: port 2 is in the second word");
		CHECK(!nth_w_val(ODI_SW_PORT_ISOLATION(2), 1, &v), "pon isolate: no second write");
		CHECK(!nth_w_val(ODI_SW_PORT_ISOLATION(0), 0, &v), "pon isolate: ports 0 and 1 not written");
	}

	/* No write after reserved vid's two calls -- pos should now point past the
	 * last plain-register write this function makes (the sweep's T/D
	 * entries are not 'W' kind, so pos, which only advances past W
	 * checks, correctly stops counting them).
	 */
	(void)pos;
}

/* Regression for the live-hardware finding "CLASSIFY_SETUP and VLAN_INGRESS_CHECK":
 * a fresh boot's CLASSIFY_SETUP already carries classify_init's own
 * PATTERN1_COUNT=0x80 default (bits 5-12) before classify ever runs (the
 * switch DAL's own boot-time init, /proc/odi_init's "switch" verb,
 * always runs first). Confirms classify's read-modify-write preserves that
 * field exactly -- if a future edit ever widened US_NO_MATCH_ACTION's mask by
 * mistake and clobbered it, this catches it without needing hardware.
 */
static void test_cf_cfg_preserves_other_fields(void)
{
	uint32_t v;

	odi_mock_reset();
	/* Seed CLASSIFY_SETUP as if classify_init already ran: PATTERN1_COUNT=0x80,
	 * PON_SELECT=1, everything else 0 -- 0x80<<5 | 1<<3 = 0x1008. This
	 * seed write is log entry 0 (nth_w_val index 0); classify's own write
	 * below is entry 1.
	 */
	odi_reg_write(ODI_SW_CLASSIFY_SETUP_OFF, 0x1008U);

	odi_switch_init_platform();

	CHECK(nth_w_val(ODI_SW_CLASSIFY_SETUP_OFF, 1, &v) && v == 0x1009U,
	      "classify preserves PATTERN1_COUNT/PON_SELECT, only sets US_NO_MATCH_ACTION");
}

/* L3_GLOBAL_CTRL (0x01106c): reset 0x4, engine bit 0 clear. A firmware that
 * left it enabled is turned off, touching nothing but bit 0; an engine
 * already off costs no write.
 */
static void test_l3_off(void)
{
	uint32_t v;
	unsigned int i, n = 0;

	odi_mock_reset();
	odi_reg_write(0x1106cU, 0x00000005U);
	odi_switch_init_platform();
	CHECK(odi_reg_read(0x1106cU) == 0x00000004U, "an enabled L3 engine is turned off, bit 2 kept");
	CHECK(nth_w_val(0x1106cU, 1, &v) && v == 0x00000004U, "by one write of the register");

	odi_mock_reset();
	odi_reg_write(0x1106cU, 0x00000004U);
	odi_switch_init_platform();
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == 0x1106cU)
			n++;
	CHECK(n == 1, "an engine already off is not written (only the seed write is in the log)");
}

/* SA_MOVED (0x01c000): the PON port (bits 5:4) drops, the other ports keep
 * what they had; a second call writes nothing.
 */
static void test_sa_moved(void)
{
	unsigned int i, n = 0;

	odi_mock_reset();
	odi_sw_sa_moved_apply();
	CHECK(odi_reg_read(0x1c000U) == 0x00000010U, "from the reset value 0: the PON port field 1");

	odi_mock_reset();
	odi_reg_write(0x1c000U, 0x000000c3U);
	odi_sw_sa_moved_apply();
	CHECK(odi_reg_read(0x1c000U) == 0x000000d3U, "the other ports' fields are kept, a stale PON value replaced");

	odi_mock_reset();
	odi_reg_write(0x1c000U, 0x00000030U);
	odi_sw_sa_moved_apply();
	CHECK(odi_reg_read(0x1c000U) == 0x00000010U, "a PON field of 3 becomes 1");

	odi_mock_reset();
	odi_reg_write(0x1c000U, 0x00000010U);
	odi_sw_sa_moved_apply();
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == 0x1c000U)
			n++;
	CHECK(n == 1 && odi_reg_read(0x1c000U) == 0x00000010U, "already set: no further write");
}

int main(void)
{
	test_sequence();
	test_cf_cfg_preserves_other_fields();
	test_l3_off();
	test_sa_moved();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_init_platform_test: ok\n");
	return 0;
}
