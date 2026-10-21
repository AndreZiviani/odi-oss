// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_cf.c -- the bridge-connection leaves: the whole register and
 * table sequence of one cmd 51 (the CF rows it is handed, the VLAN table
 * sweep and the fixed register template around them), the CF row delete
 * and the single VLAN row write. The command layer (odi_switch_cmd.c)
 * decides which rows; odi_switch_dal.h has the row layouts and
 * docs/SWITCH.md#bridge-connections the capture this reproduces.
 *
 * The plain-register part of cmd 51 is the same in every captured
 * instance, on both ISPs, so it is replayed as a fixed template: the
 * finding is that it is idempotent, not that it was left undecoded.
 */
#include "odi_switch_dal.h"
#include "odi_switch_reg.h"
#include "odi_switch_mcast.h"

/* The VLAN table: 4096 rows by VID. */
#define ODI_SW_VLAN_ID_COUNT		4096U
#define ODI_SW_VLAN_MEMBERS_SENTINEL	0xf0U	/* the first pass, every row */
#define ODI_SW_VLAN_MEMBERS_IDX0_2ND	0xffU	/* row 0, second write */
#define ODI_SW_VLAN_MEMBERS_IDX1_1ST	0x0003f8ffU	/* row 1, first write */
#define ODI_SW_VLAN_MEMBERS_IDX1_2ND	0x0003f800U	/* row 1, second write */

/* One pass of the reserved-multicast (LINK_MCAST) block, written twice per
 * cmd 51, as captured, repeats included.
 */
static void classify_rma_ctrl_pass(void)
{
	odi_reg_write(ODI_SW_LINK_MCAST_00_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_01_OFF, 0x20U);
	odi_reg_write(ODI_SW_LINK_MCAST_02_OFF, 0x20U);
	odi_reg_write(ODI_SW_LINK_MCAST_03_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_08_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_0D_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_0E_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_04_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_10_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_11_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_12_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_18_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_1A_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_13_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_20_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_21_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_22_OFF, 0);
}

/* Template bit of each of the 256 rows, 32 to a CLASSIFY_PATTERN_SEL word,
 * as last written: the register word holds the bits of 32 rows, so a row
 * written alone has to carry its 31 neighbours with it.
 */
static uint32_t odi_sw_cf_tmpl_bits[256 / 32];

void odi_sw_cf_reset(void)
{
	unsigned int i;

	for (i = 0; i < 256 / 32; i++)
		odi_sw_cf_tmpl_bits[i] = 0;
}

uint32_t odi_sw_cf_layout1_row(void)
{
	return ODI_SW_CLASSIFY_SETUP_PATTERN1_FIRST_ROW(odi_reg_read(ODI_SW_CLASSIFY_SETUP_OFF));
}

/* Template 1 compares the S-tag, both ways. Written when the first row on it
 * goes in, never for a bridge that has none, so a line without S-tag rules
 * sees the register writes it always saw.
 */
static void classify_template_s(void)
{
	uint32_t v = 0;

	v = ODI_SW_CLASSIFY_TEMPLATE_US_VID_SET(v, 1);
	v = ODI_SW_CLASSIFY_TEMPLATE_US_PRI_SET(v, 1);
	v = ODI_SW_CLASSIFY_TEMPLATE_DS_VID_SET(v, 1);
	v = ODI_SW_CLASSIFY_TEMPLATE_DS_PRI_SET(v, 1);
	odi_reg_write(ODI_SW_CLASSIFY_TEMPLATE(1), v);
}

void odi_sw_cf_add(const struct odi_sw_cf_entry *cf, unsigned int n_cf,
				   const struct odi_sw_vlan_override *vlan_over, unsigned int n_over,
				   uint32_t vlan_default)
{
	unsigned int i;
	uint32_t idx;
	uint32_t v;

	/* The CF rows in the order handed over, each after the
	 * CLASSIFY_PATTERN_SEL word holding its template bit (32 rows per
	 * word; every row the stock stack builds is on template 0, so the word
	 * is 0 there, and stays 0 for any bridge with no S-tag filter).
	 */
	int tmpl_cfg = 0;

	for (i = 0; i < n_cf; i++) {
		uint32_t rule[2] = { cf[i].rule_w0, cf[i].rule_w1 };
		uint32_t mask[2] = { cf[i].mask_w0, cf[i].mask_w1 };
		uint32_t action[3] = { cf[i].action_w0, cf[i].action_w1, cf[i].action_w2 };
		uint32_t *bits = &odi_sw_cf_tmpl_bits[(cf[i].idx / 32U) % (256 / 32)];
		uint32_t bit = 1U << (cf[i].idx % 32U);

		if (cf[i].tmpl) {
			if (!tmpl_cfg) {
				classify_template_s();
				tmpl_cfg = 1;
			}
			*bits |= bit;
		} else {
			*bits &= ~bit;
		}
		odi_reg_write(ODI_SW_CLASSIFY_PATTERN_SEL(cf[i].idx / 32U), *bits);

		(void)odi_switch_table_write(ODI_SW_TBL_CLS_RULE_B, cf[i].idx, rule, 2);
		(void)odi_switch_table_write(ODI_SW_TBL_CLS_MASK_B, cf[i].idx, mask, 2);
		(void)odi_switch_table_write(cf[i].is_us ? ODI_SW_TBL_CLS_US_ACTION : ODI_SW_TBL_CLS_DS_ACTION,
					      cf[i].idx, action, 3);
	}

	/* First VLAN pass: every row to the sentinel, one row write each. */
	v = ODI_SW_VLAN_MEMBERS_SENTINEL;
	for (idx = 0; idx < ODI_SW_VLAN_ID_COUNT; idx++)
		(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, idx, &v, 1);

	odi_reg_write(ODI_SW_VLAN_SETUP_OFF,
		      ODI_SW_VLAN_SETUP_FILTER_ON_SET(
		      ODI_SW_VLAN_SETUP_VID0_MODE_SET(
		      ODI_SW_VLAN_SETUP_VID4095_MODE_SET(0, 1), 1), 1));
	{
		uint32_t tag[4] = { 0, 0, 0, 0 };

		odi_switch_vlan_egress_tag_group_write(0xf, tag);
	}

	v = ODI_SW_VLAN_MEMBERS_IDX1_1ST;
	(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, 1, &v, 1);

	/* PORT_VLAN_INDEX(n) is a word holding the PVID of ports 2n and 2n+1:
	 * (0) is ports 0-1, (1) is ports 2-3, and each write below replaces
	 * the whole word, as captured.
	 */
	odi_reg_write(ODI_SW_VLAN_ACCEPT_FRAMES(0), 0);
	odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK(0), 0xfU);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0x1U);
	odi_reg_write(ODI_SW_VLAN_ACCEPT_FRAMES(0), 0);
	odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK(0), 0xfU);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0x1001U);
	odi_reg_write(ODI_SW_VLAN_ACCEPT_FRAMES(0), 0);
	odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK(0), 0xfU);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0x1U);
	odi_reg_write(ODI_SW_VLAN_ACCEPT_FRAMES(0), 0);
	odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK(0), 0xfU);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0x1001U);
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF,
		      ODI_SW_VLAN_SETUP_FILTER_ON_SET(
		      ODI_SW_VLAN_SETUP_VID0_MODE_SET(
		      ODI_SW_VLAN_SETUP_VID4095_MODE_SET(0, 1), 1), 1));

	/* Two passes of the reserved-multicast block, the IPMC leak slot only
	 * between them.
	 */
	classify_rma_ctrl_pass();
	odi_reg_write(ODI_SW_DSCP_REMARK_MAP(15), ODI_SW_DSCP_REMARK_MAP_DSCP_SET(0, 0));
	odi_reg_write(ODI_SW_LINK_MCAST_CDP_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_SSTP_OFF, 0);
	odi_reg_write(ODI_SW_IPMC_VLAN_LEAK(0), 0);
	odi_reg_write(ODI_SW_IPMC_VLAN_LEAK(0), 0);
	odi_reg_write(ODI_SW_IPMC_VLAN_LEAK(0), 0);
	odi_reg_write(ODI_SW_IPMC_VLAN_LEAK(0), 0);
	classify_rma_ctrl_pass();
	odi_reg_write(ODI_SW_LINK_MCAST_CDP_OFF, 0);
	odi_reg_write(ODI_SW_LINK_MCAST_SSTP_OFF, 0);

	for (i = 0; i < ODI_SW_PORT_COUNT; i++) {
		odi_reg_write(ODI_SW_PROTO_VLAN_GROUP(i), 0);
		odi_reg_write(ODI_SW_PROTO_VLAN_GROUP(i), 0);
	}

	/* 16 slots: priority 0, VLAN index 1, not in use. */
	for (i = 0; i < 16; i++)
		odi_reg_write(ODI_SW_PORT_PROTO_VLAN(i),
			      ODI_SW_PORT_PROTO_VLAN_VLAN_INDEX_SET(0, 1));

	/* VLAN_SETUP stepped through these values, as captured; not decoded
	 * further.
	 */
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF,
		      ODI_SW_VLAN_SETUP_FILTER_ON_SET(
		      ODI_SW_VLAN_SETUP_VID0_MODE_SET(
		      ODI_SW_VLAN_SETUP_VID4095_MODE_SET(0, 1), 1), 1));
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF,
		      ODI_SW_VLAN_SETUP_FILTER_ON_SET(
		      ODI_SW_VLAN_SETUP_VID4095_MODE_SET(0, 1), 1));
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF, ODI_SW_VLAN_SETUP_FILTER_ON_SET(0, 1));
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF, 0);
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF, ODI_SW_VLAN_SETUP_VID0_MODE_SET(0, 1));
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF,
		      ODI_SW_VLAN_SETUP_VID0_MODE_SET(
		      ODI_SW_VLAN_SETUP_VID4095_MODE_SET(0, 1), 1));

	v = ODI_SW_VLAN_MEMBERS_IDX0_2ND;
	(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, 0, &v, 1);

	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0x1000U);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0x0U);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0x1000U);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0x0U);

	v = ODI_SW_VLAN_MEMBERS_IDX1_2ND;
	(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, 1, &v, 1);

	/* Second VLAN pass, rows 2..4094: vlan_default (0 in every capture)
	 * except the service rows the caller lists. Row 4095 keeps the
	 * sentinel: no capture rewrites it.
	 */
	for (idx = 2; idx <= ODI_SW_VLAN_ID_LAST_SWEPT; idx++) {
		v = vlan_default;
		for (i = 0; i < n_over; i++) {
			if (vlan_over[i].idx == idx) {
				v = vlan_over[i].val;
				break;
			}
		}
		(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, idx, &v, 1);
	}
}

void odi_sw_cf_del(uint32_t idx, int is_us)
{
	static const uint32_t zero[3] = { 0, 0, 0 };

	(void)odi_switch_table_write(ODI_SW_TBL_CLS_RULE_B, idx, zero, 2);
	(void)odi_switch_table_write(is_us ? ODI_SW_TBL_CLS_US_ACTION : ODI_SW_TBL_CLS_DS_ACTION,
				      idx, zero, 3);
}

void odi_sw_cf_act_write(uint32_t idx, int is_us, const uint32_t act[3])
{
	(void)odi_switch_table_write(is_us ? ODI_SW_TBL_CLS_US_ACTION : ODI_SW_TBL_CLS_DS_ACTION,
				      idx, act, 3);
}

void odi_sw_vlan_row_set(uint32_t vid, uint32_t val)
{
	(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, vid, &val, 1);
}

/* ---- the ACL rows of the extended VLAN discards ---------------------------
 *
 * The classifier has no drop action; the ACL has one, and it decides before
 * the classifier. The layout is the one odi_switch_mcast.c writes (a rule is
 * two entries of one table, the bits that must be 1 and the bits that must
 * be 0, the valid bit in the first and written last; the action a third
 * entry with its own class word), and the field and the action are another
 * open implementation's measurements: the C-tag field, VID 11:0 and
 * priority 15:13, reads 0 for a frame without a C-tag, so VIDs 1..4094 never
 * match one; a forwarding decision of 1 (redirect) with an empty port mask
 * drops. Its rule acts on the frames of its own ports only.
 */
#define ACL_ACT_FWD_REDIRECT		1U
#define ACL_FIELD_CTAG			0x09U
#define ACL_PORT_SHIFT			10U
#define ACL_PORTS_ALL			0x3fU
#define ACL_TMPL_BITS			7U
#define ACL_CTAG_VID			0x0fffU
#define ACL_CTAG_PRI			0xe000U
#define ACL_CTAG_PRI_SHIFT		13U

/* Slot 0 of the template reads the C-tag, slots 1..7 compare nothing. The
 * unused top bits of each template word are left as they are. */
static void acl_template_set(void)
{
	uint32_t first = 8U * ODI_SW_ACL_DROP_TEMPLATE;
	uint32_t w0 = ODI_SW_ACL_TMPL_FIELD_OFF + 4U * (first / 4U);
	uint32_t w1 = w0 + 4U;
	uint32_t v0 = (odi_reg_read(w0) & ~0x0fffffffU) |
		      (ACL_FIELD_CTAG << (7U * (first % 4U)));
	uint32_t v1 = odi_reg_read(w1) & ~0x0fffffffU;

	if (odi_reg_read(w0) != v0)
		odi_reg_write(w0, v0);
	if (odi_reg_read(w1) != v1)
		odi_reg_write(w1, v1);
}

int odi_sw_acl_drop_set(uint32_t entry, uint32_t ports, int vid, int pri)
{
	static const uint32_t zero[ODI_SW_MC_RULE_WORDS] = { 0 };
	uint32_t ones[ODI_SW_MC_RULE_WORDS] = { 0 }, zeros[ODI_SW_MC_RULE_WORDS] = { 0 };
	uint32_t act[ODI_SW_MC_ACTION_WORDS] = { 0, 0, 0 };
	uint32_t slot = (uint32_t)vid & ACL_CTAG_VID, care = ACL_CTAG_VID;
	uint32_t permit, en;
	int rc;

	if (vid < 1 || vid > 4094 || pri > 7 || !(ports & 0xfU) || (ports & ~0xfU))
		return -22;
	if (pri >= 0) {
		slot |= (uint32_t)pri << ACL_CTAG_PRI_SHIFT;
		care |= ACL_CTAG_PRI;
	}
	ones[0] = ODI_SW_MC_RULE_VALID;
	ones[4] = ((slot & care) << 16) | (ports << ACL_PORT_SHIFT) | ODI_SW_ACL_DROP_TEMPLATE;
	zeros[4] = ((~slot & care & 0xffffU) << 16) |
		   ((ACL_PORTS_ALL & ~ports) << ACL_PORT_SHIFT) |
		   (ACL_TMPL_BITS & ~ODI_SW_ACL_DROP_TEMPLATE);
	act[1] = ACL_ACT_FWD_REDIRECT << 9;

	/* The permit for a frame no rule matches before the lookup, or a port
	 * with the lookup on drops everything; neither is ever taken away. */
	permit = odi_reg_read(ODI_SW_ACL_DEFAULT_PERMIT_OFF);
	if ((permit & ports) != ports)
		odi_reg_write(ODI_SW_ACL_DEFAULT_PERMIT_OFF, permit | ports);
	en = odi_reg_read(ODI_SW_MC_ACL_PORT_EN_OFF);
	if ((en & ports) != ports)
		odi_reg_write(ODI_SW_MC_ACL_PORT_EN_OFF, en | ports);

	acl_template_set();
	/* The rule cannot match until its valid bit, in the ones plane, is
	 * written last. */
	rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, zero, ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN_MASK, entry, zeros,
					    ODI_SW_MC_RULE_WORDS);
	if (!rc)
		rc = odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, entry, act,
					    ODI_SW_MC_ACTION_WORDS);
	if (rc)
		return rc;
	odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * entry, ODI_SW_ACL_ACT_EN_FWD);
	rc = odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, ones, ODI_SW_MC_RULE_WORDS);
	if (rc)
		odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * entry, ODI_SW_ACL_ACT_EN_RESET);
	return rc;
}

void odi_sw_acl_drop_del(uint32_t entry)
{
	static const uint32_t zero[ODI_SW_MC_RULE_WORDS] = { 0 };
	static const uint32_t zact[ODI_SW_MC_ACTION_WORDS] = { 0, 0, 0 };

	/* The valid bit first, so the rule stops matching before anything
	 * under it changes. */
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN, entry, zero, ODI_SW_MC_RULE_WORDS);
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_PATTERN_MASK, entry, zero, ODI_SW_MC_RULE_WORDS);
	(void)odi_switch_table_write(ODI_SW_TBL_ACL_ACTIONS, entry, zact, ODI_SW_MC_ACTION_WORDS);
	/* A released entry gets the reset class word back, never zero: zero
	 * chains an entry to the one before it. */
	odi_reg_write(ODI_SW_ACL_ACT_EN_OFF + 4U * entry, ODI_SW_ACL_ACT_EN_RESET);
}
