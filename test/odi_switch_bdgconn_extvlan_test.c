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


/* The last row words written to `table` at `index` (a table-engine write,
 * word 0 first), or 0 when there is none. */
static int tw_last(uint32_t table, uint32_t index, uint32_t *w, unsigned int n)
{
	unsigned int i, j;
	int found = 0;

	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind != 'T' || odi_mock.log[i].addr != ((table << 16) | index))
			continue;
		for (j = 0; j < n; j++)
			w[j] = 0;
		for (j = i + 1; j < odi_mock.log_n && odi_mock.log[j].kind == 'D'; j++)
			if ((odi_mock.log[j].addr & 0xffffffU) == table && (odi_mock.log[j].addr >> 24) < n)
				w[odi_mock.log[j].addr >> 24] = odi_mock.log[j].val;
		found = 1;
	}
	return found;
}

/* One untagged-frame upstream rule that names an EtherType, as omcid sends
 * it: the plan of the entry, with the value in the descriptor. */
static int send_et(const uint8_t *e, uint16_t et, uint16_t out_tpid, int *id_out)
{
	struct evtocd_entry t;
	struct evtocd_plan u;
	struct omci_vlan_oper vr;
	struct omci_bdgconn b;

	evtocd_decode(e, &t);
	if (evtocd_plan(&t, 0x8100, out_tpid, &u))
		return -2;
	evtocd_plan_oper(&u, &vr);
	if (et == 0)
		evtocd_oper_rest(&vr);
	else
		evtocd_oper_ethertype(&vr, et);
	bdg_conn(&b, next_id, OMCI_DIR_US, 5, 4, 5, &vr);
	*id_out = next_id++;
	return odi_switch_cmd(51, &b, sizeof b);
}

/* The ACL entry of a discard, by the service that made it. */
static int acl_entry_of(int id)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == id)
			return odi_sw_bdgconn[i].acl_row < 0 ? -1 :
			       (int)ODI_SW_ACL_DROP_BASE + odi_sw_bdgconn[i].acl_row;
	return -1;
}

static int send_drop(const struct evtocd_drop *d, int pri, uint32_t uni_mask, int *id_out)
{
	struct omci_vlan_oper vr;
	struct omci_bdgconn b;

	evtocd_drop_oper(d, pri, &vr);
	bdg_conn(&b, next_id, OMCI_DIR_US, uni_mask, 4, 5, &vr);
	*id_out = next_id++;
	return odi_switch_cmd(51, &b, sizeof b);
}

/* Whether a frame with `slot` as its C-tag field, entering by `port`, matches
 * the rule of ACL entry `entry`: the 160-bit key as the layout of
 * odi_switch_mcast.c defines it, template at 2:0, port at 10 + port, slot 0
 * in bits 31:16 of the last word, the ones plane and the zeros plane. */
static int acl_hit(int entry, unsigned int tmpl, unsigned int port, uint16_t slot)
{
	uint32_t ones[5], zeros[5], key[5] = { 0, 0, 0, 0, 0 };
	unsigned int i;

	if (!tw_last(ODI_SW_TBL_ACL_PATTERN, (uint32_t)entry, ones, 5) ||
	    !tw_last(ODI_SW_TBL_ACL_PATTERN_MASK, (uint32_t)entry, zeros, 5))
		return 0;
	key[4] = tmpl | (1U << (10 + port)) | ((uint32_t)slot << 16);
	key[0] = 1U << 16;
	if (!(ones[0] & (1U << 16)))
		return 0;               /* not valid */
	for (i = 0; i < 5; i++)
		if ((ones[i] & ~key[i]) || (zeros[i] & key[i]))
			return 0;
	return 1;
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



	puts("the rows another implementation measured through an OLT:");
	{
		struct evtocd_entry t;
		struct evtocd_plan u;
		int id;

		/* One entry writes two tags on an untagged frame: C-VID 410
		 * priority 5 inside, S-VID 2002 priority 3 outside. */
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 3, 2002, 6,  5, 410, 4);
		ok(add_entry(e, 0x8100, 0x88a8, 0, &why) == 0, "untagged -> S-VID 2002 pri 3 outside C-VID 410 pri 5");
		r = row_of(upstream_id, 0);
		care = CARE_US_BASE;
		ok_row("upstream: both planes of one row, as measured",
		       RULE(UNI0), MASK(care, UNI0),
		       (1U << 21) | (1U << 18) | (5U << 15) | (410U << 3) | (1U << 1),
		       US_FLOW | (1U << 21) | (1U << 18) | (3U << 15) | (2002U << 3) | ODI_SW_CF_CSACT_ADD, r);
		ok(r && !r->tmpl && row_index(upstream_id, 0) < 128, "in the first layout, on template 0");
		drop_entry();
		/* The same entry in a row of the second layout, where it was measured. */
		odi_mock_reset();
		odi_switch_cmd_reset_state();
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1009U;
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x8100, 0x88a8, &u);
		{
			struct omci_vlan_oper vr;
			struct omci_bdgconn b;

			evtocd_plan_oper(&u, &vr);
			evtocd_oper_ethertype(&vr, 0x88b5);
			bdg_conn(&b, next_id, OMCI_DIR_US, 5, 4, 5, &vr);
			id = next_id++;
			ok(odi_switch_cmd(51, &b, sizeof b) == 0, "on EtherType 0x88b5 it is a row of the second layout");
		}
		r = row_of(id, 0);
		ok_row("the same action words there",
		       RULE(UNI0), MASK(CARE_US_BASE, UNI0),
		       (1U << 21) | (1U << 18) | (5U << 15) | (410U << 3) | (1U << 1),
		       US_FLOW | (1U << 21) | (1U << 18) | (3U << 15) | (2002U << 3) | ODI_SW_CF_CSACT_ADD, r);
		ok(row_index(id, 0) == 128, "at row 128");
		release(id);

		/* Two pops on an S-VID over any C-tag: the S-tag first, template 1. */
		build(e, 8, 200, 0,  8, 4096, 0, 0,   2, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x88a8, 0x8100, &u) == 0, "S-VID 200 over any C-tag: both tags removed");
		ok(send_leg(&u, OMCI_DIR_DS, &id) == 0, "as a downstream rule");
		r = row_of(id, 1);
		d = W1_DS | W1_ST | W1_CT | W1_VID(200);
		ok_row("downstream: S-VID 200 and a C-tag; C-tag action 2, S-tag action 3, to the UNI",
		       RULE(d), MASK(CARE_DS_BASE | VIDM, d), (2U << 1) | DS_FWD_A1, DS_FWD_A2 | ODI_SW_CF_CSACT_DEL, r);
		ok(r && r->tmpl && row_index(id, 1) < 128, "on template 1, in the first layout");
		release(id);
		ok(send_leg(&u, OMCI_DIR_US, &id) == 0, "and as an upstream one");
		r = row_of(id, 0);
		d = W1_ST | W1_CT | W1_VID(200) | UNI0;
		ok_row("upstream: the same action words, with the stream",
		       RULE(d), MASK(CARE_US_BASE | VIDM, d), (2U << 1), US_FLOW | ODI_SW_CF_CSACT_DEL, r);
		release(id);

		/* A pop and a push of the same protocol is one row that rewrites
		 * the tag; priority 0 is a priority, assigned. */
		build(e, 15, 4096, 0,  5, 410, 0, 0,   1, 15, 4096, 0,  0, 2002, 4);
		ok(add_entry(e, 0x8100, 0x8100, 0, &why) == 0, "C-VID 410 pri 5 -> C-VID 2002 pri 0");
		r = row_of(upstream_id, 0);
		d = W1_CT | W1_VID(410) | W1_PRI(5) | UNI0;
		ok_row("upstream: one row, the priority assigned, 0 included",
		       RULE(d), MASK(CARE_US_BASE | VIDM | PRIM, d),
		       (1U << 21) | (1U << 18) | (0U << 15) | (2002U << 3) | (1U << 1), US_FLOW | S_TRANSP, r);
		r = row_of(downstream_id, 1);
		d = W1_DS | W1_CT | W1_VID(2002) | W1_PRI(0);
		ok_row("downstream: 2002 with priority 0 -> 410 with priority 5",
		       RULE(d), MASK(CARE_DS_BASE | VIDM | PRIM, d),
		       (1U << 21) | (1U << 18) | (5U << 15) | (410U << 3) | (1U << 1) | DS_FWD_A1,
		       DS_FWD_A2 | S_TRANSP, r);
		drop_entry();
	}

	puts("an EtherType on untagged frames, rows of the second layout:");
	{
		static const struct { uint8_t code; uint16_t et[2]; } kinds[] = {
			{ EVTOCD_F_ET_IPOE_0800, { 0x0800, 0 } },
			{ EVTOCD_F_ET_PPPOE, { 0x8863, 0x8864 } },
			{ EVTOCD_F_ET_ARP_0806, { 0x0806, 0 } },
			{ EVTOCD_F_ET_IPV6_86DD, { 0x86dd, 0 } },
			{ EVTOCD_F_ET_EAPOL_888E, { 0x888e, 0 } },
		};
		int ids[8], n = 0, rest_id;
		unsigned int k, j;
		uint32_t w[2];

		odi_mock_reset();
		odi_switch_cmd_reset_state();
		/* The boundary as the stick reads it. */
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1009U;
		for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
			build(e, 15, 4096, 0,  15, 4096, 0, kinds[k].code,   0, 15, 4096, 0,  3, (uint16_t)(100 + k), 4);
			for (j = 0; j < (kinds[k].code == EVTOCD_F_ET_PPPOE ? 2U : 1U); j++)
				ok(send_et(e, kinds[k].et[j], 0x8100, &ids[n++]) == 0, "one rule for the EtherType");
		}
		ok(n == 6, "PPPoE is two rules, 0x8863 and 0x8864, the others one");
		for (k = 0; k < (unsigned)n; k++) {
			static const uint16_t want[6] = { 0x0800, 0x8863, 0x8864, 0x0806, 0x86dd, 0x888e };

			r = row_of(ids[k], 0);
			ok(r && r->rule_w0 == want[k] && r->mask_w0 == (~want[k] & 0xffffU),
			   "the row holds the value in the ones plane and its complement in the zeros plane");
			ok(row_index(ids[k], 0) >= 128, "and sits in the second layout (row 128 up)");
			ok(r && r->rule_w1 == RULE(UNI0) && r->mask_w1 == MASK(CARE_US_BASE, UNI0),
			   "no S-tag, no C-tag, from port 0: it names an untagged frame");
			ok(r && !r->tmpl, "on template 0");
		}
		/* The words another implementation read back from the chip for
		 * 0x88b5: 0x000188b5 in the ones plane, 0x0000774a in the zeros. */
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  3, 777, 4);
		ok(send_et(e, 0x88b5, 0x8100, &ids[n]) == 0, "a literal 0x88b5 (not a G.988 code; pins the words)");
		ok(tw_last(ODI_SW_TBL_CLS_RULE_B, (uint32_t)row_index(ids[n], 0), w, 2) && w[0] == 0x000188b5U,
		   "rule word 0 is 0x000188b5");
		ok(tw_last(ODI_SW_TBL_CLS_MASK_B, (uint32_t)row_index(ids[n], 0), w, 2) && w[0] == 0x0000774aU,
		   "mask word 0 is 0x0000774a on the first write of the row");
		release(ids[n]);
		ok(row_of(ids[n], 0) == NULL, "released");

		/* The rest of the untagged frames: after every one of them. */
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  3, 200, 4);
		ok(send_et(e, 0, 0x8100, &rest_id) == 0, "the rule for the rest of the untagged frames");
		ok(row_index(rest_id, 0) > row_index(ids[0], 0) && row_index(rest_id, 0) > row_index(ids[n - 1], 0) &&
		   row_index(rest_id, 0) == 254, "sits in the bottom region, after every EtherType row");
		r = row_of(rest_id, 0);
		ok(r && r->rule_w0 == 0 && r->mask_w0 == 0, "and names no EtherType");

		/* A row placed by order afterwards does not disturb them: it goes
		 * where it would with none, and they stay where they are. */
		{
			int before[6];

			for (k = 0; k < 6; k++)
				before[k] = row_index(ids[k], 0);
			build(e, 15, 4096, 0,  8, 410, 0, 0,   1, 15, 4096, 0,  8, 1410, 4);
			ok(add_entry(e, 0x8100, 0x8100, 0, &why) == 0, "a translation after them");
			ok(row_index(upstream_id, 0) == 64, "is row 64, the first of the ordered region");
			for (k = 0; k < 6; k++)
				ok(row_index(ids[k], 0) == before[k], "and the EtherType row did not move");
			drop_entry();
		}
		for (k = 0; k < (unsigned)n; k++)
			release(ids[k]);
		release(rest_id);

		/* What the driver refuses. */
		{
			struct omci_vlan_oper vr;
			struct omci_bdgconn b;
			struct evtocd_plan p;
			int id;

			build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  3, 300, 4);
			{
				struct evtocd_entry t;

				evtocd_decode(e, &t);
				(void)evtocd_plan(&t, 0x8100, 0x8100, &p);
			}
			evtocd_plan_oper(&p, &vr);
			evtocd_oper_ethertype(&vr, 0x0800);
			bdg_conn(&b, 95, OMCI_DIR_DS, 5, 4, 5, &vr);
			ok(odi_switch_cmd(51, &b, sizeof b) != 0 && row_of(95, 1) == NULL,
			   "an EtherType downstream: refused");
			evtocd_plan_oper(&p, &vr);
			vr.filter.ethertype = OMCI_ETHTYPE_IP;
			bdg_conn(&b, 96, OMCI_DIR_US, 5, 4, 5, &vr);
			ok(odi_switch_cmd(51, &b, sizeof b) != 0, "a G.988 EtherType code instead of the value: refused");
			evtocd_plan_oper(&p, &vr);
			evtocd_oper_ethertype(&vr, 0x0800);
			vr.filter.inner_mode = OMCI_TAGF_TAGGED | OMCI_TAGF_VID;
			vr.filter.inner.vid = 100;
			bdg_conn(&b, 97, OMCI_DIR_US, 5, 4, 5, &vr);
			ok(odi_switch_cmd(51, &b, sizeof b) != 0 && row_of(97, 0) == NULL,
			   "an EtherType on a tagged frame: refused");
			(void)id;
		}

		/* No second layout (the field reads 0): none to put the row in. */
		odi_mock_reset();
		odi_switch_cmd_reset_state();
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x0009U;
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  3, 100, 4);
		{
			int id;

			ok(send_et(e, 0x0800, 0x8100, &id) != 0 && row_of(id, 0) == NULL,
			   "with no second layout on the chip an EtherType rule is refused, not placed in the first");
		}
		odi_mock_reset();
		odi_switch_cmd_reset_state();
	}

	puts("an entry that discards the C-tagged frames of one VID, an ACL row:");
	{
		struct evtocd_drop d;
		int id, id2, entry;
		uint32_t w[5], tmplw0;

		odi_mock_reset();
		odi_switch_cmd_reset_state();
		/* A stick as it boots: ACL lookup and permit on for the UNI and the PON. */
		odi_mock.regs[odi_mock_slot(0x15040U)] = 0x5U;
		odi_mock.regs[odi_mock_slot(0x15044U)] = 0x5U;
		d.vid = 20;
		d.pri = -1;
		ok(send_drop(&d, -1, 5, &id) == 0, "VID 20, any priority: accepted");
		entry = acl_entry_of(id);
		ok(entry == (int)ODI_SW_ACL_DROP_BASE, "in the first ACL entry of the block");
		ok(row_of(id, 0) == NULL && row_index(id, 1) < 0, "and no classifier row");
		ok(tw_last(ODI_SW_TBL_ACL_PATTERN, (uint32_t)entry, w, 5) && w[0] == 0x00010000U &&
		   w[4] == 0x00140405U, "ones plane: valid; VID 20 in slot 0, port 0, template 5");
		ok(tw_last(ODI_SW_TBL_ACL_PATTERN_MASK, (uint32_t)entry, w, 5) && w[0] == 0 &&
		   w[4] == 0x0febf802U, "zeros plane: the clear bits of VID 20, ports 1..5, template bit 1");
		ok(tw_last(ODI_SW_TBL_ACL_ACTIONS, (uint32_t)entry, w, 3) && w[0] == 0 && w[1] == 0x200U && w[2] == 0,
		   "action: forwarding decision 1 (redirect) with no port: a drop");
		ok(last_write(0x15048U + 4U * (uint32_t)entry) == 0x10U, "the entry applies the forwarding class only");
		tmplw0 = last_write(0x15008U + 4U * 10U);
		ok(tmplw0 != 0xdeadbeefU && (tmplw0 & 0x7fU) == 0x09U, "template 5 slot 0 reads the C-tag");
		ok(odi_mock.regs[odi_mock_slot(0x15040U)] == 0x5U && odi_mock.regs[odi_mock_slot(0x15044U)] == 0x5U,
		   "ACL lookup and permit were on already and stay");
		ok(acl_hit(entry, 5, 0, 20), "VID 20 from the UNI matches");
		ok(acl_hit(entry, 5, 0, (uint16_t)(20 | (5 << 13))), "so does VID 20 with priority 5");
		ok(!acl_hit(entry, 5, 0, 21), "VID 21 does not");
		ok(!acl_hit(entry, 5, 0, 0), "an untagged frame (field 0) does not");
		ok(!acl_hit(entry, 5, 2, 20), "VID 20 from the PON does not: nothing the fibre sends is acted on here");
		ok(!acl_hit(entry, 5, 3, 20), "nor from the CPU port");
		ok(!acl_hit(entry, 3, 0, 20), "nor on another template");
		ok(last_write(ODI_SW_CLASSIFY_PATTERN_SEL(0)) == 0xdeadbeefU, "no classifier register is written");

		d.vid = 20;
		d.pri = 5;
		ok(send_drop(&d, 5, 5, &id2) == 0, "VID 20 priority 5: accepted");
		entry = acl_entry_of(id2);
		ok(entry == (int)ODI_SW_ACL_DROP_BASE + 1, "the next entry");
		ok(tw_last(ODI_SW_TBL_ACL_PATTERN, (uint32_t)entry, w, 5) && w[4] == 0xa0140405U,
		   "ones: priority 5 in bits 15:13 of the slot");
		ok(tw_last(ODI_SW_TBL_ACL_PATTERN_MASK, (uint32_t)entry, w, 5) && w[4] == 0x4febf802U,
		   "zeros: and its clear bits");
		ok(acl_hit(entry, 5, 0, (uint16_t)(20 | (5 << 13))), "VID 20 priority 5 matches");
		ok(!acl_hit(entry, 5, 0, (uint16_t)(20 | (2 << 13))), "VID 20 priority 2 does not");
		ok(!acl_hit(entry, 5, 0, 20), "nor VID 20 priority 0");

		release(id2);
		ok(!acl_hit(entry, 5, 0, (uint16_t)(20 | (5 << 13))), "released: the rule no longer matches");
		ok(last_write(0x15048U + 4U * (uint32_t)entry) == 0x7fU, "and its class word is back at the reset value");
		release(id);
		ok(acl_entry_of(id) < 0, "the first is gone too");

		/* What the driver refuses. */
		d.vid = 0;
		ok(send_drop(&d, -1, 5, &id) != 0, "VID 0: refused (an untagged frame reads the same)");
		d.vid = 4095;
		ok(send_drop(&d, -1, 5, &id) != 0, "VID 4095: refused");
		d.vid = 20;
		ok(send_drop(&d, -1, 4, &id) != 0, "a uni_mask with no UNI in it: refused");
		{
			struct omci_vlan_oper vr;
			struct omci_bdgconn b;

			evtocd_drop_oper(&d, -1, &vr);
			bdg_conn(&b, 98, OMCI_DIR_DS, 5, 4, 5, &vr);
			ok(odi_switch_cmd(51, &b, sizeof b) != 0, "downstream: refused");
		}
		/* The ACL lookup is turned on, never off: a stick that reads it off. */
		odi_mock_reset();
		odi_switch_cmd_reset_state();
		d.vid = 30;
		ok(send_drop(&d, -1, 5, &id) == 0, "on a stick with the lookup off");
		ok(odi_mock.regs[odi_mock_slot(0x15040U)] == 0x1U && odi_mock.regs[odi_mock_slot(0x15044U)] == 0x1U,
		   "the permit and the lookup are turned on for port 0, and only for it");
		{
			unsigned int pw = 0, ew = 0, i;

			for (i = 0; i < odi_mock.log_n; i++) {
				if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == 0x15044U && !pw)
					pw = i + 1;
				if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == 0x15040U && !ew)
					ew = i + 1;
			}
			ok(pw && ew && pw < ew, "the permit first, so no frame meets the lookup without it");
		}
		release(id);

		/* 32 rows, then none. */
		{
			int n = 0, i, ids[40];

			for (i = 0; i < 34; i++) {
				d.vid = (int16_t)(100 + i);
				if (send_drop(&d, -1, 5, &ids[n]) == 0)
					n++;
			}
			ok(n == (int)ODI_SW_ACL_DROP_MAX, "the block holds 32 discards, the next is refused with no room");
			for (i = 0; i < n; i++)
				release(ids[i]);
		}
	}

	/* The voice of an IP host on the host lane (OMCI_POTS_PORTS): omcid sends
	 * its rules with the UNI alone in uni_mask. The same rule with the CPU
	 * bit added -- what the stock choice, an IP host on the CPU port, would
	 * send -- must not reach the CPU either: nothing from the fibre does. */
	puts("an IP host's voice on the host lane, and never the CPU port:");
	{
		static const uint32_t masks[2] = { 1U, 1U | (1U << ODI_SW_CPU_PORT) };
		unsigned int m;

		for (m = 0; m < 2; m++) {
			struct evtocd_entry t;
			struct evtocd_plan u, dp;
			struct omci_vlan_oper vr;
			struct omci_bdgconn b;
			uint8_t dsm = EVTOCD_DSMODE_INVERSE;
			const char *dwhy = 0;
			int uid, did, vlan_ok = 0;
			unsigned int i;

			build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  5, 200, 4);
			evtocd_decode(e, &t);
			ok(!evtocd_plan(&t, 0x8100, 0x8100, &u) && !evtocd_plan_lane(&u, &dsm) &&
			   evtocd_plan_ds(&u, dsm, &dp, &dwhy) == 1,
			   "untagged -> C-VID 200 pri 5, made a rule of the host lane");
			evtocd_plan_oper(&u, &vr);
			bdg_conn(&b, next_id, OMCI_DIR_US, masks[m], 4, 5, &vr);
			uid = next_id++;
			ok(odi_switch_cmd(51, &b, sizeof b) == 0, "its upstream leg is accepted");
			evtocd_plan_oper(&dp, &vr);
			bdg_conn(&b, next_id, OMCI_DIR_DS, masks[m], 4, 5, &vr);
			did = next_id++;
			ok(odi_switch_cmd(51, &b, sizeof b) == 0, "and its downstream leg");
			r = row_of(uid, 0);
			d = W1_CT | W1_VID(200) | UNI0;
			care = CARE_US_BASE | VIDM;
			ok_row("upstream: C-VID 200 from the UNI, any priority -> VID 200 pri 5",
			       RULE(d), MASK(care, d),
			       (1U << 21) | (1U << 18) | (5U << 15) | (200U << 3) | (1U << 1),
			       US_FLOW | S_TRANSP, r);
			r = row_of(did, 1);
			d = W1_DS | W1_CT | W1_VID(200);
			care = CARE_DS_BASE | VIDM;
			ok_row("downstream: C-VID 200 keeps its tag and priority, to the UNI alone",
			       RULE(d), MASK(care, d), (1U << 18) | (200U << 3) | (1U << 1) | DS_FWD_A1,
			       DS_FWD_A2 | S_TRANSP, r);
			for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
				if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == uid)
					vlan_ok = odi_sw_bdgconn[i].vlan_vid == 200U &&
						  odi_sw_bdgconn[i].vlan_val ==
						  ODI_SW_VLAN_ROW(1U | (1U << ODI_SW_PON_PORT), 0);
			ok(vlan_ok, "VLAN 200: the UNI and the PON, tagged; not the CPU port");
			release(uid);
			release(did);
		}
	}

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

	puts("the layout boundary is a count of rows from the top (CLASSIFY_SETUP 12:5):");
	{
		struct evtocd_plan u;
		struct evtocd_entry t;
		int i, id, accepted = 0;

		odi_mock_reset();
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1009U;
		ok(odi_sw_cf_layout1_row() == 128, "count 128 (what we program): pattern 1 from row 128");
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1c09U;
		ok(odi_sw_cf_layout1_row() == 32, "count 224 (the stock value): pattern 1 from row 32, not 224");
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x0009U;
		ok(odi_sw_cf_layout1_row() == 256, "count 0: no row is pattern 1");
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1fe9U;
		ok(odi_sw_cf_layout1_row() == 1, "count 255: pattern 1 from row 1");

		odi_mock_reset();
		odi_switch_cmd_reset_state();
		/* At 224 rows 32 and up are pattern 1, and the S-tag rows start at
		 * 64, so none may be placed. Read from the bottom (224) the first
		 * 160 of them would have been accepted. */
		odi_mock.regs[odi_mock_slot(ODI_SW_CLASSIFY_SETUP_OFF)] = 0x1c09U;
		build(e, 8, 200, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x88a8, 0x8100, &u);
		for (i = 0; i < 4; i++) {
			u.s.f_vid = (int16_t)(200 + i);
			if (send_leg(&u, OMCI_DIR_US, &id) == 0)
				accepted++;
		}
		ok(accepted == 0, "count 224: no S-tag rule is placed on a pattern 1 row");
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
