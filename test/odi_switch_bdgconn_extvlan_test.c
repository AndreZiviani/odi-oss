/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_bdgconn_extvlan_test.c -- cmd 51 for the rules of the extended
 * VLAN tagging operation (class 171), from the table entry to the classifier
 * row. The chain is the one the stick runs: an entry's 16 bytes go through
 * omcid's decode and plan (src/omci/extvlan.c, built here for the host),
 * become the two bridge rules omcid sends (rule_gen
 * OMCI_VLAN_OPER_EXTVLAN_GEN, one per direction), and cmd 51 writes the
 * rows. What is asserted is the rows, bit for bit, and the registers
 * around them.
 *
 * The expected words are worked out by hand from the row layout in
 * docs/SWITCH.md, "CF rows", not read back from the code under test. Two of
 * them are also what another open implementation read back from the same
 * chip for the same operation: a translation of C-VID 410 to 1410 has upstream
 * action word 1 0x00042c12 and rule word 0x000cd008 there, and the same here
 * (the rows here add the source port and the S-tag flag the stock rows
 * compare, which that implementation does not).
 *
 * None of the stock captures has one of these rules (neither of our lines
 * provisions class 171 tagging), so nothing here replays a capture; the
 * rows of the shapes both lines do use are pinned by the replay tests, and
 * the last section here asserts that the registers they see are unchanged
 * by a rule of this kind being absent.
 *
 * Usage: odi_switch_bdgconn_extvlan_test. Returns non-zero on any failure.
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
	printf("  %-72s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

static void ok_row(const char *label, uint32_t rule, uint32_t mask,
		   uint32_t a1, uint32_t a2, const struct odi_sw_cf_row *r)
{
	if (!r) {
		printf("  %-72s FAIL (no such row)\n", label);
		failures++;
		return;
	}
	if (r->rule_w1 != rule || r->mask_w1 != mask || r->act[0] != 0 ||
	    r->act[1] != a1 || r->act[2] != a2) {
		printf("  %-72s FAIL\n", label);
		printf("      rule %08x mask %08x act1 %08x act2 %08x (wanted rule %08x mask %08x act1 %08x act2 %08x)\n",
		       r->rule_w1, r->mask_w1, r->act[1], r->act[2], rule, mask, a1, a2);
		failures++;
		return;
	}
	printf("  %-72s ok\n", label);
}

/* An entry from the fields a dump prints, as in extvlan_test.c. */
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

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

/* The two services of one entry (upstream, then downstream if it has one):
 * what omcid's bdgconn_add() sends, with UNI 0 and the PON in uni_mask. */
static int next_id;

static int send_leg(const struct evtocd_plan *p, uint32_t dir, int *id_out)
{
	struct omci_vlan_oper vr;
	struct omci_bdgconn b;

	evtocd_plan_oper(p, &vr);
	bdg_conn(&b, next_id, dir, 5, 4, 5, &vr);
	*id_out = next_id++;
	return odi_switch_cmd(51, &b, sizeof b);
}

static const struct odi_sw_cf_row *row_of(int id, int ds)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == id) {
			int r = ds ? odi_sw_bdgconn[i].ds_row : odi_sw_bdgconn[i].us_row;

			return r >= 0 ? &odi_sw_cf[r] : NULL;
		}
	return NULL;
}

static int row_index(int id, int ds)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == id)
			return ds ? odi_sw_bdgconn[i].ds_row : odi_sw_bdgconn[i].us_row;
	return -1;
}

static void release(int id)
{
	int32_t v = id;

	(void)odi_switch_cmd(50, &v, sizeof v);
}

/* The last write to register `off` in the log, or 0xdeadbeef when none. */
static uint32_t last_write(uint32_t off)
{
	uint32_t v = 0xdeadbeefU;
	unsigned int i;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == off)
			v = odi_mock.log[i].val;
	return v;
}

static int wrote(uint32_t off)
{
	return last_write(off) != 0xdeadbeefU;
}

/* Care words for the shapes: the direction bit, the VID and priority fields
 * it names, both tag flags, and the source port of an upstream rule. */
#define UNI0	0U			/* source port 0, from uni_mask 5 */
#define W1_DS	ODI_SW_CF_W1_DS
#define W1_ST	ODI_SW_CF_W1_STAG
#define W1_CT	ODI_SW_CF_W1_CTAG
#define W1_VID(v)	((uint32_t)(v) << ODI_SW_CF_W1_VID_SHIFT)
#define W1_PRI(p)	((uint32_t)(p) << ODI_SW_CF_W1_PRI_SHIFT)
#define CARE_US_BASE	(W1_DS | W1_ST | W1_CT | ODI_SW_CF_W1_UNI_MASK)
#define CARE_DS_BASE	(W1_DS | W1_ST | W1_CT)
#define VIDM	ODI_SW_CF_W1_VID_MASK
#define PRIM	ODI_SW_CF_W1_PRI_MASK

/* Upstream action word 2 from the stream: flow 4, take it. */
#define US_FLOW		((4U << ODI_SW_CF_A2_US_FLOW_SHIFT) | ODI_SW_CF_A2_US_SID_ACT)
/* Downstream: forward to the UNI port mask 1, UNI action 3 (low bit in
 * word 2 bit 31, high bit in word 1 bit 0). */
#define DS_FWD_A1	1U
#define DS_FWD_A2	(0x80000000U | (1U << ODI_SW_CF_A2_DS_PMSK_SHIFT))

#define S_TRANSP	4U		/* S-tag action: leave the frame as it is */

/* data/care to the rule and mask words: rule = data, mask = care & ~data. */
#define RULE(d)		(d)
#define MASK(care, d)	((care) & ~(d))

/* A plan, both directions, sent as two services; the rows come back by id. */
static int upstream_id, downstream_id;

static int add_entry(const uint8_t *e, uint16_t in_tpid, uint16_t out_tpid, uint8_t dsmode,
		     const char **why_out)
{
	struct evtocd_entry t;
	struct evtocd_plan u, d;
	const char *why, *dwhy = 0;

	evtocd_decode(e, &t);
	why = evtocd_plan(&t, in_tpid, out_tpid, &u);
	if (why) {
		*why_out = why;
		return -1;
	}
	if (send_leg(&u, OMCI_DIR_US, &upstream_id))
		return -2;
	downstream_id = -1;
	if (evtocd_plan_ds(&u, dsmode, &d, &dwhy))
		if (send_leg(&d, OMCI_DIR_DS, &downstream_id))
			return -3;
	return 0;
}

static void drop_entry(void)
{
	release(upstream_id);
	if (downstream_id >= 0)
		release(downstream_id);
}

int main(void)
{
	uint8_t e[EVTOCD_ENTRY_LEN];
	const struct odi_sw_cf_row *r;
	const char *why = 0;
	uint32_t care, d;

	odi_mock_reset();
	odi_switch_cmd_reset_state();

	puts("translation, C-VID 410 to 1410, priority kept (the single-tag case):");
	build(e, 15, 4096, 0,  8, 410, 0, 0,   1, 15, 4096, 0,  8, 1410, 4);
	ok(add_entry(e, 0x8100, 0x8100, 0, &why) == 0, "both legs accepted");
	r = row_of(upstream_id, 0);
	d = W1_CT | W1_VID(410) | UNI0;
	care = CARE_US_BASE | VIDM;
	/* C action 1 (tag) with the assigned VID, no priority assigned: the
	 * word the other implementation read back from the chip. */
	ok_row("upstream: C-tag VID 410 from port 0 -> assign 1410, keep priority",
	       RULE(d), MASK(care, d), 0x00042c12U, US_FLOW | S_TRANSP, r);
	r = row_of(downstream_id, 1);
	d = W1_DS | W1_CT | W1_VID(1410);
	care = CARE_DS_BASE | VIDM;
	ok_row("downstream: C-tag VID 1410 -> assign 410, keep priority, to the UNI",
	       RULE(d), MASK(care, d), (1U << 18) | (410U << 3) | (1U << 1) | DS_FWD_A1,
	       DS_FWD_A2 | S_TRANSP, r);
	ok(!r || !r->tmpl, "neither row uses the S-tag template");
	ok(!wrote(ODI_SW_CLASSIFY_TEMPLATE(1)), "and the template register is not written");
	ok(last_write(ODI_SW_CLASSIFY_PATTERN_SEL(2)) == 0, "the pattern word of the rows is 0");
	ok(odi_sw_bdgconn[0].vlan_vid == 410 || odi_sw_bdgconn[1].vlan_vid == 410,
	   "VLAN row for 410 is claimed by the upstream leg");
	drop_entry();

	puts("the operator vector: VID 410 priorities 0, 5 and 6, each its own rule:");
	{
		static const uint8_t pris[3] = { 0, 5, 6 };
		int ids[3], dids[3], i;
		int idx[3];

		for (i = 0; i < 3; i++) {
			build(e, 15, 4096, 0,  pris[i], 410, 0, 0,   1, 15, 4096, 0,  8, 1410, 4);
			ok(add_entry(e, 0x8100, 0x8100, 0, &why) == 0, "entry accepted");
			ids[i] = upstream_id;
			dids[i] = downstream_id;
		}
		for (i = 0; i < 3; i++) {
			r = row_of(ids[i], 0);
			d = W1_CT | W1_VID(410) | W1_PRI(pris[i]) | UNI0;
			care = CARE_US_BASE | VIDM | PRIM;
			ok_row("upstream: 410 with this priority, assigned 1410 and that priority",
			       RULE(d), MASK(care, d),
			       (1U << 21) | (1U << 18) | ((uint32_t)pris[i] << 15) | (1410U << 3) | (1U << 1),
			       US_FLOW | S_TRANSP, r);
			r = row_of(dids[i], 1);
			d = W1_DS | W1_CT | W1_VID(1410) | W1_PRI(pris[i]);
			care = CARE_DS_BASE | VIDM | PRIM;
			ok_row("downstream: 1410 with it, back to 410 and that priority",
			       RULE(d), MASK(care, d),
			       (1U << 21) | (1U << 18) | ((uint32_t)pris[i] << 15) | (410U << 3) | (1U << 1) | DS_FWD_A1,
			       DS_FWD_A2 | S_TRANSP, r);
			idx[i] = row_index(ids[i], 0);
		}
		ok(idx[0] != idx[1] && idx[1] != idx[2] && idx[0] != idx[2],
		   "the three upstream rows are three rows, not one");
		/* A wider rule for the same VID goes after them: the lowest
		 * matching row wins, so the specific ones must come first. */
		build(e, 15, 4096, 0,  8, 410, 0, 0,   1, 15, 4096, 0,  8, 1410, 4);
		ok(add_entry(e, 0x8100, 0x8100, 0, &why) == 0, "a VID-only rule for 410 beside them");
		ok(row_index(upstream_id, 0) > idx[0] && row_index(upstream_id, 0) > idx[1] &&
		   row_index(upstream_id, 0) > idx[2],
		   "sits after the three priority rows");
		drop_entry();
		for (i = 0; i < 3; i++) {
			release(ids[i]);
			release(dids[i]);
		}
	}

	puts("untagged frames: add a C-tag, add an S-tag, add both:");
	build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  2, 100, 4);
	ok(add_entry(e, 0x8100, 0x8100, 0, &why) == 0, "untagged -> C-tag VID 100 pri 2");
	r = row_of(upstream_id, 0);
	care = CARE_US_BASE;
	ok_row("upstream: no S-tag, no C-tag; tag VID 100 pri 2 (the manual-VLAN shape)",
	       RULE(UNI0), MASK(care, UNI0),
	       (1U << 21) | (1U << 18) | (2U << 15) | (100U << 3) | (1U << 1), US_FLOW | S_TRANSP, r);
	r = row_of(downstream_id, 1);
	d = W1_DS | W1_CT | W1_VID(100) | W1_PRI(2);
	care = CARE_DS_BASE | VIDM | PRIM;
	ok_row("downstream: C-tag VID 100 pri 2 -> remove it",
	       RULE(d), MASK(care, d), (2U << 1) | DS_FWD_A1, DS_FWD_A2 | S_TRANSP, r);
	drop_entry();

	build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  3, 300, EVTOCD_T_TPID_OUTPUT_DEI_0);
	ok(add_entry(e, 0x8100, 0x88a8, 0, &why) == 0, "untagged -> S-tag VID 300 pri 3");
	r = row_of(upstream_id, 0);
	care = CARE_US_BASE;
	ok_row("upstream: S-tag action 1 (add), VID 300, pri 3, C-tag transparent",
	       RULE(UNI0), MASK(care, UNI0), (3U << 1),
	       US_FLOW | (1U << 21) | (1U << 18) | (3U << 15) | (300U << 3) | ODI_SW_CF_CSACT_ADD, r);
	ok(r && !r->tmpl, "an untagged match needs no S-tag template");
	r = row_of(downstream_id, 1);
	d = W1_DS | W1_ST | W1_VID(300) | W1_PRI(3);
	care = CARE_DS_BASE | VIDM | PRIM;
	ok_row("downstream: S-tag VID 300 pri 3 -> pop (action 3), on the S-tag template",
	       RULE(d), MASK(care, d), (3U << 1) | DS_FWD_A1,
	       DS_FWD_A2 | ODI_SW_CF_CSACT_DEL, r);
	ok(r && r->tmpl, "the S-VID is compared on template 1");
	ok(last_write(ODI_SW_CLASSIFY_TEMPLATE(1)) == 0x55U,
	   "which is written to read the S-tag in both directions (0x55)");
	{
		int ix = row_index(downstream_id, 1);

		ok(ix >= 0 && last_write(ODI_SW_CLASSIFY_PATTERN_SEL((unsigned)ix / 32U)) ==
		   (1U << ((unsigned)ix % 32U)), "and the row's pattern bit is set, alone in its word");
	}
	drop_entry();

	build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 1, 300, 6,  2, 400, 4);
	ok(add_entry(e, 0x8100, 0x88a8, 0, &why) == 0, "untagged -> S-VID 300 outside C-VID 400");
	r = row_of(upstream_id, 0);
	care = CARE_US_BASE;
	ok_row("upstream: both planes in one row",
	       RULE(UNI0), MASK(care, UNI0),
	       (1U << 21) | (1U << 18) | (2U << 15) | (400U << 3) | (1U << 1),
	       US_FLOW | (1U << 21) | (1U << 18) | (1U << 15) | (300U << 3) | ODI_SW_CF_CSACT_ADD, r);
	drop_entry();

	puts("S-tag in front of a C-tag (double tagging), and out again:");
	build(e, 15, 4096, 0,  8, 100, 0, 0,   0, 15, 4096, 0,  3, 200, EVTOCD_T_TPID_OUTPUT_DEI_0);
	ok(add_entry(e, 0x8100, 0x88a8, 0, &why) == 0, "C-VID 100 -> S-VID 200 pri 3 pushed");
	r = row_of(upstream_id, 0);
	d = W1_CT | W1_VID(100) | UNI0;
	care = CARE_US_BASE | VIDM;
	ok_row("upstream: C-VID 100 from port 0; S-tag added, C-tag left",
	       RULE(d), MASK(care, d), (3U << 1),
	       US_FLOW | (1U << 21) | (1U << 18) | (3U << 15) | (200U << 3) | ODI_SW_CF_CSACT_ADD, r);
	r = row_of(downstream_id, 1);
	d = W1_DS | W1_ST | W1_CT | W1_VID(200) | W1_PRI(3);
	care = CARE_DS_BASE | VIDM | PRIM;
	ok_row("downstream: S-VID 200 pri 3 over a C-tag -> pop the S-tag (C-VID not compared)",
	       RULE(d), MASK(care, d), (3U << 1) | DS_FWD_A1, DS_FWD_A2 | ODI_SW_CF_CSACT_DEL, r);
	ok(r && r->tmpl, "on the S-tag template");
	drop_entry();

	build(e, 8, 200, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  15, 4096, 0);
	ok(add_entry(e, 0x88a8, 0x8100, 0, &why) == 0, "S-VID 200 over any C-tag: pop the S-tag");
	r = row_of(upstream_id, 0);
	d = W1_ST | W1_CT | W1_VID(200) | UNI0;
	care = CARE_US_BASE | VIDM;
	ok_row("upstream: S-VID 200 and a C-tag; S-tag action 3",
	       RULE(d), MASK(care, d), (3U << 1),
	       US_FLOW | ODI_SW_CF_CSACT_DEL, r);
	ok(r && r->tmpl, "template 1 again");
	r = row_of(downstream_id, 1);
	ok(r != NULL, "and the way back is a rule too (S-tag pushed back, priority 0)");
	ok_row("downstream: C-tag behind nothing -> add S-tag 200, pri 0",
	       RULE(W1_DS | W1_CT), MASK(CARE_DS_BASE, W1_DS | W1_CT), (3U << 1) | DS_FWD_A1,
	       DS_FWD_A2 | (1U << 21) | (1U << 18) | (200U << 3) | ODI_SW_CF_CSACT_ADD, r);
	drop_entry();

	puts("S-tag replaced:");
	build(e, 8, 200, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  2, 300, EVTOCD_T_TPID_OUTPUT_DEI_0);
	why = 0;
	ok(add_entry(e, 0x88a8, 0x88a8, 0, &why) == 0, "S-VID 200 -> 300 pri 2 accepted upstream");
	ok(downstream_id < 0, "with no way back, since its priority was not named");
	r = row_of(upstream_id, 0);
	d = W1_ST | W1_CT | W1_VID(200) | UNI0;
	care = CARE_US_BASE | VIDM;
	ok_row("upstream: S action 1 replaces the tag (the same action as an add)",
	       RULE(d), MASK(care, d), (3U << 1),
	       US_FLOW | (1U << 21) | (1U << 18) | (2U << 15) | (300U << 3) | ODI_SW_CF_CSACT_ADD, r);
	drop_entry();

	puts("what the driver refuses:");
	{
		struct evtocd_plan p;
		struct omci_vlan_oper vr;
		struct omci_bdgconn b;
		int id;

		build(e, 8, 200, 0,  8, 100, 4, 0,   1, 15, 4096, 0,  15, 4096, 0);
		memset(&p, 0, sizeof p);
		p.s.in = p.s.out = p.c.in = p.c.out = 1;
		p.s.f_vid = 200;
		p.c.f_vid = 100;
		p.s.f_pri = p.c.f_pri = -1;
		p.s.op = EVTOCD_OP_POP;
		p.c.op = EVTOCD_OP_PASS;
		p.s.vid = p.c.vid = p.s.pri = p.c.pri = -1;
		evtocd_plan_oper(&p, &vr);
		bdg_conn(&b, 90, OMCI_DIR_US, 5, 4, 5, &vr);
		ok(odi_switch_cmd(51, &b, sizeof b) != 0, "a filter on the S-VID and the C-VID: refused");
		id = 90;
		ok(row_of(id, 0) == NULL, "and no row was made");
		memset(&p, 0, sizeof p);
		p.c.in = p.c.out = 1;
		p.c.f_vid = 100;
		p.c.f_pri = p.s.f_vid = p.s.f_pri = -1;
		p.c.op = EVTOCD_OP_REWRITE;
		p.c.vid = 200;
		p.c.pri = -1;
		evtocd_plan_oper(&p, &vr);
		bdg_conn(&b, 91, OMCI_DIR_BI, 5, 4, 5, &vr);
		ok(odi_switch_cmd(51, &b, sizeof b) != 0, "one descriptor for both directions: refused");
		memset(&p, 0, sizeof p);
		p.s.in = p.s.out = 1;
		p.s.op = EVTOCD_OP_REWRITE;
		p.s.vid = 300;
		p.s.pri = -1;
		p.s.f_vid = p.s.f_pri = p.c.f_vid = p.c.f_pri = -1;
		evtocd_plan_oper(&p, &vr);
		bdg_conn(&b, 92, OMCI_DIR_US, 5, 4, 5, &vr);
		ok(odi_switch_cmd(51, &b, sizeof b) != 0, "an S-tag replaced with no priority given (a keep): refused");
	}

	puts("the S-tag template stays off the rows past the layout boundary:");
	{
		struct evtocd_plan u;
		struct evtocd_entry t;
		int ids[80], n = 0, i, refused = 0;

		odi_mock_reset();
		odi_switch_cmd_reset_state();
		/* CLASSIFY_SETUP as the stick reads it: second layout from row
		 * 128. 64 rows fit below it; the 65th S-tag rule does not. */
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1009U;
		build(e, 8, 200, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x88a8, 0x8100, &u);
		for (i = 0; i < 70; i++) {
			int id;

			u.s.f_vid = (int16_t)(200 + i);
			if (send_leg(&u, OMCI_DIR_US, &id) == 0)
				ids[n++] = id;
			else
				refused++;
		}
		ok(n == 64 && refused == 6, "64 S-tag rules fit in rows 64..127, the next six are refused");
		ok(n < 1 || row_index(ids[n - 1], 0) == 127, "the last one is row 127");
	}

	puts("registers a line with no rule of this kind sees:");
	{
		unsigned int i, w = 0;

		odi_mock_reset();
		odi_switch_cmd_reset_state();
		{
			struct omci_vlan_oper vr;
			struct omci_bdgconn b;

			bdg_gen_manual(&vr, 11, 0, 0);
			bdg_conn(&b, 0, OMCI_DIR_BI, 5, 4, 5, &vr);
			(void)odi_switch_cmd(51, &b, sizeof b);
		}
		for (i = 0; i < odi_mock.log_n; i++)
			if (odi_mock.log[i].kind == 'W' &&
			    odi_mock.log[i].addr == ODI_SW_CLASSIFY_TEMPLATE(1))
				w++;
		ok(w == 0, "the manual-VLAN rule never writes the template register");
		for (i = 0; i < odi_mock.log_n; i++)
			if (odi_mock.log[i].kind == 'W' &&
			    odi_mock.log[i].addr == ODI_SW_CLASSIFY_PATTERN_SEL(2) &&
			    odi_mock.log[i].val != 0)
				w++;
		ok(w == 0, "and every pattern word it writes is 0");
	}

	if (failures) {
		printf("odi_switch_bdgconn_extvlan_test: %d failure(s)\n", failures);
		return 1;
	}
	puts("odi_switch_bdgconn_extvlan_test: ok");
	return 0;
}
