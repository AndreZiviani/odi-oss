/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_isp2_test.c -- cmd 51 on the service profile of the second stick,
 * the one the isp1-only replay got wrong. ISP2 is PPPoE over C-TAG VID 10
 * (VLAN_MANU_TAG_VID=10) behind a VEIP, and omcid builds two services
 * there: service_id 0, both ways, the untagged handoff adding VID 10, and
 * service_id 1, downstream only, the multicast rule; both with uni_mask 5
 * (the VEIP PON bit plus the UNI behind it).
 *
 * Two kinds of evidence, both from the isp2 stock image, and they check
 * different things:
 *
 * 1. Its boot trace ends inside its last cmd 51 bracket: the ring wrapped,
 *    so the CF rows are gone, but the tail -- the VLAN sweep from row 3107,
 *    the whole plain-register template and the complete row-by-row VLAN
 *    pass -- survived (test/fixtures/isp2-260921-cmd51-tail.txt).
 *    odi_switch_isp2_test.sh compares our second bracket against it write
 *    for write: VLAN row 10 = 0x15, every other row 0.
 *
 * 2. Its classification table, read back with the stock diag (`classf get
 *    entry`): an untagged upstream row that tags VID 10 priority 0 and
 *    queues on flow 0; a downstream VID 10 row (C-tag, no S-tag) that
 *    deletes both tags and forwards to the UNI; a downstream VID 0 row for
 *    multicast. The stock image runs a different driver on different input
 *    (its own omci_app: uni_mask 4, multicast first, multicast out-style
 *    priority 8), so its rows are not byte-comparable with ours; this test
 *    checks the fields both must agree on, below, and pins our exact words
 *    so a change to the derivation shows up here. Where ours differs from
 *    the stock rows (priority 0 cared on the downstream rows, the UNI cared
 *    upstream, egress mask 1 instead of 3 on the unicast row, multicast
 *    tags deleted instead of kept) it follows the r91564 driver, whose
 *    behaviour is what the isp1 replay pins and which carried isp2 with
 *    this same omcid rule before the 6.18 port.
 *
 * Usage: odi_switch_isp2_test <out-file>: writes both brackets as a ring
 * dump for the .sh to compare, and returns non-zero on any field failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

#include "bdgconn_rules.h"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-62s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

/* One CF row as a bracket wrote it: rule, mask, action. */
struct row {
	int seen;
	int is_us;
	uint32_t rule[2], mask[2], act[3];
};

/* Collect the CF rows written between log entries [from, to). */
static void rows_of(unsigned int from, unsigned int to, struct row *rows)
{
	unsigned int i, k;

	memset(rows, 0, sizeof(struct row) * 256);
	for (i = from; i < to; i++) {
		const struct odi_mock_write *e = &odi_mock.log[i];
		uint32_t table, idx, w[3] = { 0, 0, 0 };

		if (e->kind != 'T')
			continue;
		table = e->addr >> 16;
		idx = e->addr & 0xffffU;
		for (k = 0; k < 3 && i + 1 + k < to && odi_mock.log[i + 1 + k].kind == 'D'; k++)
			w[k] = odi_mock.log[i + 1 + k].val;
		if (idx >= 256)
			continue;
		if (table == ODI_SW_TBL_CLS_RULE_B) {
			rows[idx].rule[0] = w[0];
			rows[idx].rule[1] = w[1];
			rows[idx].seen = 1;
		} else if (table == ODI_SW_TBL_CLS_MASK_B) {
			rows[idx].mask[0] = w[0];
			rows[idx].mask[1] = w[1];
		} else if (table == ODI_SW_TBL_CLS_US_ACTION || table == ODI_SW_TBL_CLS_DS_ACTION) {
			rows[idx].is_us = table == ODI_SW_TBL_CLS_US_ACTION;
			memcpy(rows[idx].act, w, sizeof(w));
		}
	}
}

static unsigned int mark_at(uint32_t tag, unsigned int from)
{
	unsigned int i;

	for (i = from; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'M' && odi_mock.log[i].addr == tag)
			return i;
	return odi_mock.log_n;
}

static int exact(const struct row *r, int is_us, const uint32_t rule[2],
		 const uint32_t mask[2], const uint32_t act[3])
{
	return r->seen && r->is_us == is_us && !memcmp(r->rule, rule, 8) &&
	       !memcmp(r->mask, mask, 8) && !memcmp(r->act, act, 12);
}

/* Field readers over a row, the layouts in odi_switch_dal.h. */
static uint32_t care1(const struct row *r) { return r->rule[1] | r->mask[1]; }
static uint32_t cvid(const struct row *r) { return (r->act[1] >> ODI_SW_CF_A1_C_VID_SHIFT) & 0xfffU; }
static uint32_t cpri(const struct row *r) { return (r->act[1] >> ODI_SW_CF_A1_C_PRI_SHIFT) & 7U; }
static uint32_t cact(const struct row *r) { return (r->act[1] >> ODI_SW_CF_A1_CACT_SHIFT) & 3U; }
static uint32_t csact(const struct row *r) { return r->act[2] & 7U; }
static uint32_t uni_act(const struct row *r) { return ((r->act[1] & 1U) << 1) | (r->act[2] >> 31); }
static uint32_t pmsk(const struct row *r) { return (r->act[2] >> ODI_SW_CF_A2_DS_PMSK_SHIFT) & 0xfU; }
static uint32_t vid_of(const struct row *r) { return (r->rule[1] & ODI_SW_CF_W1_VID_MASK) >> ODI_SW_CF_W1_VID_SHIFT; }

static void call(uint32_t cmd, void *buf, uint32_t len, const char *what)
{
	int rc;

	odi_mock_mark(cmd);
	rc = odi_switch_cmd(cmd, buf, len);
	odi_mock_mark(0x80000000U | cmd);
	if (rc != 0)
		printf("  %s returned %d\n", what, rc);
	ok(rc == 0, what);
}

int main(int argc, char **argv)
{
	struct omci_bdgconn b;
	struct omci_vlan_oper vr;
	static struct row r1[256], r2[256];
	unsigned int m1, e1, m2, e2, i, n1 = 0, n2 = 0;
	FILE *f;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <out-file>\n", argv[0]);
		return 2;
	}

	odi_mock_reset();
	odi_switch_cmd_reset_state();
	puts("cmd 51 on the isp2 profile (VID 10 untagged handoff + multicast):");

	bdg_gen_manual(&vr, 10, 0, 0);
	bdg_conn(&b, 0, OMCI_DIR_BI, 5, 0, 0, &vr);
	call(51, &b, sizeof b, "service_id 0 (both ways, add VID 10) accepted");

	bdg_gen_manual(&vr, 10, 0, 1);
	bdg_conn(&b, 1, OMCI_DIR_DS, 5, 0, 1, &vr);
	call(51, &b, sizeof b, "service_id 1 (downstream multicast) accepted");

	m1 = mark_at(51, 0);
	e1 = mark_at(0x80000033U, m1);
	m2 = mark_at(51, e1);
	e2 = mark_at(0x80000033U, m2);
	rows_of(m1, e1, r1);
	rows_of(m2, e2, r2);
	for (i = 0; i < 256; i++) {
		n1 += r1[i].seen;
		n2 += r2[i].seen;
	}

	puts("exact rows (the derivation, pinned):");
	{
		static const uint32_t us_rule[2] = { 0x00010000U, 0x00000000U };
		static const uint32_t us_mask[2] = { 0x00000000U, 0x8000001fU };
		static const uint32_t us_act[3] = { 0, 0x00240052U, 0x00a40004U };
		static const uint32_t ds_rule[2] = { 0x00010000U, 0x80005008U };
		static const uint32_t ds_mask[2] = { 0x00000000U, 0x007faf10U };
		static const uint32_t ds_act[3] = { 0, 0x00240005U, 0x88240003U };
		static const uint32_t mc_rule[2] = { 0x00010000U, 0x80000008U };
		static const uint32_t mc_mask[2] = { 0x00000000U, 0x007fff10U };
		static const uint32_t mc_act[3] = { 0, 0x00240055U, 0x98240003U };

		ok(n1 == 2 && n2 == 1, "bracket 1 writes two rows, bracket 2 one");
		ok(exact(&r1[254], 1, us_rule, us_mask, us_act), "row 254: upstream, untagged -> add VID 10");
		ok(exact(&r1[64], 0, ds_rule, ds_mask, ds_act), "row 64: downstream VID 10 -> strip, to UNI");
		ok(exact(&r2[65], 0, mc_rule, mc_mask, mc_act), "row 65: downstream multicast VID 0");
	}

	puts("fields the isp2 stock table has too (diag classf get entry):");
	ok((care1(&r1[254]) & (ODI_SW_CF_W1_DS | ODI_SW_CF_W1_STAG | ODI_SW_CF_W1_CTAG)) ==
	   (ODI_SW_CF_W1_DS | ODI_SW_CF_W1_STAG | ODI_SW_CF_W1_CTAG) &&
	   !(r1[254].rule[1] & (ODI_SW_CF_W1_DS | ODI_SW_CF_W1_STAG | ODI_SW_CF_W1_CTAG)) &&
	   !(care1(&r1[254]) & ODI_SW_CF_W1_VID_MASK),
	   "upstream row: no S-tag, no C-tag, any VID");
	ok(cact(&r1[254]) == ODI_SW_CF_CACT_ADD && cvid(&r1[254]) == 10 && cpri(&r1[254]) == 0 &&
	   csact(&r1[254]) == ODI_SW_CF_CSACT_TRANSPARENT,
	   "upstream row: C-tag tagging VID 10 pri 0, S-tag transparent");
	ok((r1[254].act[2] & ODI_SW_CF_A2_US_SID_ACT) &&
	   ((r1[254].act[2] >> ODI_SW_CF_A2_US_FLOW_SHIFT) & 0x7fU) == 0,
	   "upstream row: assign to flow (SID) 0");
	ok((r1[64].rule[1] & ODI_SW_CF_W1_DS) && vid_of(&r1[64]) == 10 &&
	   (care1(&r1[64]) & ODI_SW_CF_W1_VID_MASK) == ODI_SW_CF_W1_VID_MASK &&
	   (r1[64].rule[1] & ODI_SW_CF_W1_CTAG) && !(r1[64].rule[1] & ODI_SW_CF_W1_STAG) &&
	   (care1(&r1[64]) & (ODI_SW_CF_W1_STAG | ODI_SW_CF_W1_CTAG)) ==
	   (ODI_SW_CF_W1_STAG | ODI_SW_CF_W1_CTAG),
	   "downstream row: VID 10, C-tag, no S-tag");
	ok(cact(&r1[64]) == ODI_SW_CF_CACT_DEL && csact(&r1[64]) == ODI_SW_CF_CSACT_DEL &&
	   uni_act(&r1[64]) == ODI_SW_CF_DS_UNI_ACT_FWD && (pmsk(&r1[64]) & 1U),
	   "downstream row: delete C-tag and S-tag, forward to UNI port 0");
	ok((r2[65].rule[1] & ODI_SW_CF_W1_DS) && vid_of(&r2[65]) == 0 &&
	   (care1(&r2[65]) & ODI_SW_CF_W1_VID_MASK) == ODI_SW_CF_W1_VID_MASK &&
	   (r2[65].rule[1] & ODI_SW_CF_W1_CTAG) && !(r2[65].rule[1] & ODI_SW_CF_W1_STAG),
	   "multicast row: VID 0, C-tag, no S-tag");
	ok(uni_act(&r2[65]) == ODI_SW_CF_DS_UNI_ACT_FWD && pmsk(&r2[65]) == 3U,
	   "multicast row: forward to UNI ports 0-1");

	puts("teardown (cmd 50) frees the rows and the VLAN row:");
	{
		int32_t id = 0;
		unsigned int before = odi_mock.log_n;

		ok(odi_switch_cmd(50, &id, sizeof id) == 0 && !odi_sw_cf[254].used &&
		   !odi_sw_cf[64].used && odi_sw_cf[65].used, "service_id 0 released, multicast kept");
		ok(odi_mock.log_n > before, "the release writes (invalid rows, VLAN row 10)");
		bdg_gen_manual(&vr, 10, 0, 0);
		bdg_conn(&b, 0, OMCI_DIR_BI, 5, 0, 0, &vr);
		/* Same class as the multicast row, so it goes after it; the
		 * freed row 64 stays a hole until everything is rebuilt. */
		ok(odi_switch_cmd(51, &b, sizeof b) == 0 && odi_sw_cf[254].used &&
		   !odi_sw_cf[64].used && odi_sw_cf[66].used &&
		   odi_sw_cf[66].rule_w1 == 0x80005008U && odi_sw_cf[65].rule_w1 == 0x80000008U,
		   "re-added: rows 254 and 66, multicast still 65");
	}

	f = fopen(argv[1], "w");
	if (!f) {
		fprintf(stderr, "odi_switch_isp2_test: cannot open %s\n", argv[1]);
		return 1;
	}
	/* Only the two brackets under test: the teardown above is not in
	 * the stock trace.
	 */
	odi_mock_table_flush();
	for (i = m1; i <= e2 && i < odi_mock.log_n; i++)
		fprintf(f, "%llu %c 0x%08x 0x%08x\n", (unsigned long long)odi_mock.log[i].ns,
			odi_mock.log[i].kind, odi_mock.log[i].addr, odi_mock.log[i].val);
	fclose(f);

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
