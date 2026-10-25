/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_bdgconn_uplink_test.c -- the upstream rows read back for the
 * reports igmpd takes and sends up itself (odi_switch_bdgconn.c,
 * odi_switch_bdgconn_us_classify(), _us_by_tag(), _us_flow_known()).
 *
 * The rows are the ones cmd 51 really programs, from the rules omcid sends
 * (bdgconn_rules.h, and the extended VLAN plans of src/omci/extvlan.c): the
 * shapes of ISP1's services (the untagged service tagged VID 11 priority 0,
 * VID filters passing tagged frames), an EtherType row and the rest of the
 * untagged frames after it, a translation of VID 410 to 1410 that keeps the
 * priority, and a discard of VID 99. Each frame is then asked about the way
 * the classifier would see it, and the answer must be the row docs/SWITCH.md
 * says matches it (the discards first, then the lowest matching row) with
 * that row's treatment and flow.
 *
 * Usage: odi_switch_bdgconn_uplink_test. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../src/omci/extvlan.c"

#include "bdgconn_rules.h"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-76s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* A class 171 entry from the fields a dump prints, as extvlan_test.c. */
static void build(uint8_t *e, uint8_t fop, uint16_t fov, uint8_t fot,
		  uint8_t fip, uint16_t fiv, uint8_t fit, uint8_t et,
		  uint8_t rt, uint8_t top, uint16_t tov, uint8_t tot,
		  uint8_t tip, uint16_t tiv, uint8_t tit)
{
	put32(e + 0, ((uint32_t)fop << 28) | ((uint32_t)fov << 15) | ((uint32_t)fot << 12));
	put32(e + 4, ((uint32_t)fip << 28) | ((uint32_t)fiv << 15) | ((uint32_t)fit << 12) | et);
	put32(e + 8, ((uint32_t)rt << 30) | ((uint32_t)top << 16) | ((uint32_t)tov << 3) | tot);
	put32(e + 12, ((uint32_t)tip << 16) | ((uint32_t)tiv << 3) | tit);
}

static int next_id = 1;

static int send_rule(const struct omci_vlan_oper *vr, uint32_t dir, uint32_t us_flow)
{
	struct omci_bdgconn b;

	bdg_conn(&b, next_id++, dir, 5, us_flow, us_flow, vr);
	return odi_switch_cmd(51, &b, sizeof b);
}

static struct odi_sw_us_tags untagged(uint16_t et)
{
	struct odi_sw_us_tags t;

	memset(&t, 0, sizeof t);
	t.ethertype = et;
	return t;
}

static struct odi_sw_us_tags ctagged(uint32_t vid, uint32_t pri)
{
	struct odi_sw_us_tags t = untagged(0x8100);

	t.ctag = 1;
	t.cvid = vid;
	t.cpri = pri;
	t.ethertype = 0x0800;
	return t;
}

static struct odi_sw_us_treat classify(struct odi_sw_us_tags t)
{
	struct odi_sw_us_treat out;

	odi_switch_bdgconn_us_classify(&t, &out);
	return out;
}

static void isp1_shapes(void)
{
	struct omci_vlan_oper vr;
	struct odi_sw_us_treat tr;
	uint32_t flow = 0, row = 0;

	puts("ISP1's shapes: untagged tagged VID 11, VID 10 passed, VID 12 priority 4 passed:");
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	bdg_gen_manual(&vr, 11, 0, 0);
	ok(send_rule(&vr, OMCI_DIR_BI, 2) == 0, "the untagged service, flow 2");
	bdg_gen_vid_filter(&vr, 10, -1);
	ok(send_rule(&vr, OMCI_DIR_BI, 1) == 0, "VID 10, flow 1");
	bdg_gen_vid_filter(&vr, 12, 4);
	ok(send_rule(&vr, OMCI_DIR_BI, 3) == 0, "VID 12 priority 4, flow 3");

	tr = classify(untagged(0x0800));
	ok(tr.verdict == ODI_SW_US_FWD && tr.flow == 2 && tr.c_act == ODI_SW_CF_CACT_ADD &&
	   tr.c_vid == 11 && tr.c_pri == 0 && tr.c_src == (ODI_SW_US_SRC_VID | ODI_SW_US_SRC_PRI),
	   "an untagged report: flow 2, a C-tag VID 11 priority 0 added");
	ok(odi_sw_cf[tr.row].used && odi_sw_cf[tr.row].is_us, "the row is an upstream row in the table");
	tr = classify(ctagged(10, 3));
	ok(tr.verdict == ODI_SW_US_FWD && tr.flow == 1 && tr.c_act == ODI_SW_CF_CACT_TRANSPARENT,
	   "a report tagged VID 10 priority 3: flow 1, the tag left as it is");
	tr = classify(ctagged(12, 4));
	ok(tr.verdict == ODI_SW_US_FWD && tr.flow == 3, "VID 12 priority 4: flow 3");
	tr = classify(ctagged(12, 5));
	ok(tr.verdict == ODI_SW_US_NONE, "VID 12 priority 5: no row, no stream (the switch drops it)");
	tr = classify(ctagged(77, 0));
	ok(tr.verdict == ODI_SW_US_NONE, "VID 77: no row");

	ok(odi_switch_bdgconn_us_by_tag(11, 0, &flow, &row) == 0 && flow == 2,
	   "the row that carries VID 11 priority 0 up: the untagged service's, flow 2");
	ok(odi_switch_bdgconn_us_by_tag(11, 5, &flow, &row) != 0,
	   "VID 11 priority 5: none (that row fixes priority 0)");
	ok(odi_switch_bdgconn_us_by_tag(10, 5, &flow, &row) == 0 && flow == 1,
	   "VID 10 any priority: flow 1 (the row passes the frame's own)");
	ok(odi_switch_bdgconn_us_by_tag(12, 4, &flow, &row) == 0 && flow == 3, "VID 12 priority 4: flow 3");
	ok(odi_switch_bdgconn_us_by_tag(12, 0, &flow, &row) != 0, "VID 12 priority 0: none");
	ok(odi_switch_bdgconn_us_by_tag(420, 5, &flow, &row) != 0, "VID 420: no row carries it");

	ok(odi_switch_bdgconn_us_flow_known(1) && odi_switch_bdgconn_us_flow_known(2) &&
	   odi_switch_bdgconn_us_flow_known(3), "flows 1, 2 and 3 are upstream flows");
	ok(!odi_switch_bdgconn_us_flow_known(0) && !odi_switch_bdgconn_us_flow_known(4) &&
	   !odi_switch_bdgconn_us_flow_known(127), "0, 4 and 127 are not");

	odi_mock_reset();
	odi_switch_cmd_reset_state();
	tr = classify(untagged(0x0800));
	ok(tr.verdict == ODI_SW_US_NONE && !odi_switch_bdgconn_us_flow_known(2),
	   "after the MIB reset: no row, no flow");
}

static void extvlan_shapes(void)
{
	uint8_t e[EVTOCD_ENTRY_LEN];
	struct evtocd_entry t;
	struct evtocd_plan u;
	struct omci_vlan_oper vr;
	struct evtocd_drop d;
	struct odi_sw_us_treat tr;
	uint32_t flow = 0, row = 0;

	puts("extended VLAN rules: an EtherType, the rest, a translation, a discard:");
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1009U;
	/* Untagged IPoE tagged VID 100, every other untagged frame VID 200,
	 * both priority 3. */
	build(e, 15, 4096, 0,  15, 4096, 0, EVTOCD_F_ET_IPOE_0800,   0, 15, 4096, 0,  3, 100, 4);
	evtocd_decode(e, &t);
	ok(!evtocd_plan(&t, 0x8100, 0x8100, &u), "the IPoE entry plans");
	evtocd_plan_oper(&u, &vr);
	evtocd_oper_ethertype(&vr, 0x0800);
	ok(send_rule(&vr, OMCI_DIR_US, 4) == 0, "IPoE untagged -> VID 100, flow 4");
	build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  3, 200, 4);
	evtocd_decode(e, &t);
	ok(!evtocd_plan(&t, 0x8100, 0x8100, &u), "the rest of the untagged frames plans");
	evtocd_plan_oper(&u, &vr);
	evtocd_oper_rest(&vr);
	ok(send_rule(&vr, OMCI_DIR_US, 5) == 0, "the rest, untagged -> VID 200, flow 5");
	/* C-VID 410 to 1410, the priority kept. */
	build(e, 15, 4096, 0,  8, 410, 0, 0,   1, 15, 4096, 0,  8, 1410, 4);
	evtocd_decode(e, &t);
	ok(!evtocd_plan(&t, 0x8100, 0x8100, &u), "the translation plans");
	evtocd_plan_oper(&u, &vr);
	ok(send_rule(&vr, OMCI_DIR_US, 6) == 0, "410 -> 1410, flow 6");
	d.vid = 99;
	d.pri = -1;
	evtocd_drop_oper(&d, -1, &vr);
	ok(send_rule(&vr, OMCI_DIR_US, 4) == 0, "a discard of VID 99");

	tr = classify(untagged(0x0800));
	ok(tr.verdict == ODI_SW_US_FWD && tr.flow == 4 && tr.c_act == ODI_SW_CF_CACT_ADD && tr.c_vid == 100,
	   "an untagged IGMP report is IPoE: the EtherType row, VID 100, flow 4");
	ok(tr.c_pri == 3 && tr.c_src == (ODI_SW_US_SRC_VID | ODI_SW_US_SRC_PRI), "priority 3 assigned");
	tr = classify(untagged(0x86dd));
	ok(tr.verdict == ODI_SW_US_FWD && tr.flow == 5 && tr.c_vid == 200,
	   "an untagged IPv6 frame: the rest, VID 200, flow 5");
	tr = classify(ctagged(410, 5));
	ok(tr.verdict == ODI_SW_US_FWD && tr.flow == 6 && tr.c_act == ODI_SW_CF_CACT_ADD &&
	   tr.c_vid == 1410 && tr.c_src == ODI_SW_US_SRC_VID,
	   "tagged 410 priority 5: VID 1410 assigned, the priority not (the frame keeps 5)");
	tr = classify(ctagged(99, 2));
	ok(tr.verdict == ODI_SW_US_DISCARD && tr.flow == 0, "tagged 99: discarded, no flow");
	tr = classify(ctagged(98, 2));
	ok(tr.verdict == ODI_SW_US_NONE, "tagged 98: no row");

	ok(odi_switch_bdgconn_us_by_tag(1410, 5, &flow, &row) == 0 && flow == 6,
	   "the row that carries VID 1410 up keeps any priority: flow 6");
	ok(odi_switch_bdgconn_us_by_tag(100, 3, &flow, &row) == 0 && flow == 4,
	   "VID 100 priority 3 (what the IPoE row assigns): flow 4");
	ok(odi_switch_bdgconn_us_by_tag(100, 0, &flow, &row) != 0, "VID 100 priority 0: none");
	ok(odi_switch_bdgconn_us_by_tag(99, 0, &flow, &row) != 0, "VID 99, discarded: nothing carries it");
	ok(odi_switch_bdgconn_us_flow_known(6) && !odi_switch_bdgconn_us_flow_known(7),
	   "flow 6 is an upstream flow, 7 is not");
}

int main(void)
{
	isp1_shapes();
	extvlan_shapes();
	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
