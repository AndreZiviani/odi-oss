/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_bdgconn_vlan_test.c -- cmd 51 for the rules the class 84
 * forward operation adds (src/omci/respond/apply_bridge.c, bp_rules()):
 * forward-all, untagged only, tagged only, a priority filter. None of
 * these is in a capture of the stock image, so the checks are on the
 * derivation itself: which frames each upstream CF row matches, and the
 * VLAN table the second pass of cmd 51 leaves.
 *
 * A rule that passes tagged frames of any VID has no VID of its own for
 * a VLAN row, so it puts its members (UNI and PON) on every row 2..4094;
 * a rule with a VID keeps its own row; and releasing the pass-any rule
 * takes its members off every row again (odi_switch_bdgconn.c,
 * rule_passes_any_vid()).
 *
 * Usage: odi_switch_bdgconn_vlan_test. Returns non-zero on any failure.
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
	printf("  %-66s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

/* The VLAN table as the write log leaves it: every T row, then the run its
 * closing R says it was. */
static uint32_t vlan[4096];

static void vlan_replay(void)
{
	unsigned int i;
	uint32_t row = 0, val = 0;
	int open = 0;

	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *e = &odi_mock.log[i];

		if (e->kind == 'T' && (e->addr >> 16) == ODI_SW_TBL_VLAN_MEMBERS) {
			row = e->addr & 0xffffU;
			val = (i + 1 < odi_mock.log_n && odi_mock.log[i + 1].kind == 'D')
			      ? odi_mock.log[i + 1].val : 0;
			if (row < 4096)
				vlan[row] = val;
			open = 1;
		} else if (e->kind == 'T') {
			open = 0;
		} else if (e->kind == 'R' && open &&
			   e->addr == ((uint32_t)ODI_SW_TBL_VLAN_MEMBERS << 16 | row)) {
			uint32_t r;

			for (r = row; r < row + e->val && r < 4096; r++)
				vlan[r] = val;
			open = 0;
		}
	}
}

/* Every row 2..4094 holds v, except `except` (0: none), which holds w. */
static int rows_are(uint32_t v, uint32_t except, uint32_t w)
{
	uint32_t r;

	for (r = 2; r <= 4094; r++)
		if (vlan[r] != (r == except ? w : v))
			return 0;
	return 1;
}

static int activate(int id, const struct omci_vlan_oper *vr)
{
	struct omci_bdgconn b;

	bdg_conn(&b, id, OMCI_DIR_BI, 5, 0, 0, vr);
	return odi_switch_cmd(51, &b, sizeof b);
}

/* The upstream CF row a service got. */
static const struct odi_sw_cf_row *us_row(int id)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == id &&
		    odi_sw_bdgconn[i].us_row >= 0)
			return &odi_sw_cf[odi_sw_bdgconn[i].us_row];
	return NULL;
}

static uint32_t care(const struct odi_sw_cf_row *r)
{
	return r ? (r->rule_w1 | r->mask_w1) : 0;
}

int main(void)
{
	struct omci_vlan_oper vr;
	const struct odi_sw_cf_row *r;
	const uint32_t both = ODI_SW_VLAN_ROW(1U | (1U << ODI_SW_PON_PORT), 0);
	int32_t id;

	memset(vlan, 0, sizeof vlan);
	odi_mock_reset();
	odi_switch_cmd_reset_state();

	puts("the rule predicate:");
	bdg_gen_forward_all(&vr);
	ok(rule_passes_any_vid(&vr), "forward-all passes any VID");
	bdg_gen_tagged(&vr);
	ok(rule_passes_any_vid(&vr), "tagged-only passes any VID");
	bdg_gen_pri_filter(&vr, 5);
	ok(rule_passes_any_vid(&vr), "a priority filter passes any VID");
	bdg_gen_untagged(&vr);
	ok(!rule_passes_any_vid(&vr), "untagged-only does not");
	bdg_gen_vid_filter(&vr, 100, -1);
	ok(!rule_passes_any_vid(&vr), "a VID filter does not");
	bdg_gen_manual(&vr, 10, 0, 0);
	ok(!rule_passes_any_vid(&vr), "the manual add-tag rule does not");
	bdg_gen_manual(&vr, 10, 0, 1);
	ok(!rule_passes_any_vid(&vr), "nor its multicast mirror");

	puts("untagged only:");
	bdg_gen_untagged(&vr);
	ok(activate(0, &vr) == 0, "accepted");
	r = us_row(0);
	ok(r && (care(r) & ODI_SW_CF_W1_STAG) && (care(r) & ODI_SW_CF_W1_CTAG) &&
	   !(r->rule_w1 & (ODI_SW_CF_W1_STAG | ODI_SW_CF_W1_CTAG)) &&
	   !(care(r) & ODI_SW_CF_W1_VID_MASK),
	   "upstream matches no S-tag and no C-tag, any VID");
	vlan_replay();
	ok(rows_are(0, 0, 0), "no VLAN row: rows 2..4094 all 0");
	id = 0;
	ok(odi_switch_cmd(50, &id, sizeof id) == 0, "released");

	puts("forward-all, then a VID 100 filter beside it:");
	bdg_gen_forward_all(&vr);
	ok(activate(1, &vr) == 0, "forward-all accepted");
	r = us_row(1);
	ok(r && !(care(r) & (ODI_SW_CF_W1_STAG | ODI_SW_CF_W1_CTAG | ODI_SW_CF_W1_VID_MASK |
			     ODI_SW_CF_W1_PRI_MASK)),
	   "upstream cares about neither tag nor VID nor priority");
	vlan_replay();
	ok(rows_are(both, 0, 0), "every row 2..4094 carries UNI and PON, tagged");
	ok(vlan[4095] == ODI_SW_VLAN_MEMBERS_SENTINEL, "row 4095 keeps the sentinel");
	bdg_gen_vid_filter(&vr, 100, -1);
	ok(activate(2, &vr) == 0, "VID 100 filter accepted");
	vlan_replay();
	ok(rows_are(both, 100, both), "row 100 is its own row; the rest still carry both");
	id = 1;
	ok(odi_switch_cmd(50, &id, sizeof id) == 0, "forward-all released");
	vlan_replay();
	ok(rows_are(0, 100, ODI_SW_VLAN_ROW(1U | (1U << ODI_SW_PON_PORT), 0)),
	   "its members come off every row; VID 100 keeps its row");
	id = 2;
	ok(odi_switch_cmd(50, &id, sizeof id) == 0, "VID filter released");

	puts("tagged only, and a priority filter:");
	bdg_gen_tagged(&vr);
	ok(activate(3, &vr) == 0, "tagged-only accepted");
	r = us_row(3);
	ok(r && (care(r) & ODI_SW_CF_W1_CTAG) && (r->rule_w1 & ODI_SW_CF_W1_CTAG) &&
	   !(care(r) & ODI_SW_CF_W1_VID_MASK),
	   "upstream matches a C-tag of any VID");
	vlan_replay();
	ok(rows_are(both, 0, 0), "every row carries UNI and PON");
	id = 3;
	(void)odi_switch_cmd(50, &id, sizeof id);
	bdg_gen_pri_filter(&vr, 5);
	ok(activate(4, &vr) == 0, "priority 5 filter accepted");
	r = us_row(4);
	ok(r && (care(r) & ODI_SW_CF_W1_PRI_MASK) == ODI_SW_CF_W1_PRI_MASK &&
	   ((r->rule_w1 & ODI_SW_CF_W1_PRI_MASK) >> ODI_SW_CF_W1_PRI_SHIFT) == 5 &&
	   (r->rule_w1 & ODI_SW_CF_W1_CTAG) && !(care(r) & ODI_SW_CF_W1_VID_MASK),
	   "upstream matches priority 5 on a C-tag, any VID");
	vlan_replay();
	ok(rows_are(both, 0, 0), "every row carries UNI and PON");

	if (failures) {
		printf("odi_switch_bdgconn_vlan_test: %d failure(s)\n", failures);
		return 1;
	}
	puts("odi_switch_bdgconn_vlan_test: ok");
	return 0;
}
