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

void odi_sw_cf_add(const struct odi_sw_cf_entry *cf, unsigned int n_cf,
				   const struct odi_sw_vlan_override *vlan_over, unsigned int n_over,
				   uint32_t vlan_default)
{
	unsigned int i;
	uint32_t idx;
	uint32_t v;

	/* The CF rows in the order handed over, each after the
	 * CLASSIFY_PATTERN_SEL word holding its template bit (32 rows per
	 * word; every row is on template 0, so the word is 0).
	 */
	for (i = 0; i < n_cf; i++) {
		uint32_t rule[2] = { cf[i].rule_w0, cf[i].rule_w1 };
		uint32_t mask[2] = { cf[i].mask_w0, cf[i].mask_w1 };
		uint32_t action[3] = { cf[i].action_w0, cf[i].action_w1, cf[i].action_w2 };

		odi_reg_write(ODI_SW_CLASSIFY_PATTERN_SEL(cf[i].idx / 32U),
			      ODI_SW_CLASSIFY_PATTERN_SEL_PATTERN_SET(0, 0));

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

void odi_sw_vlan_row_set(uint32_t vid, uint32_t val)
{
	(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, vid, &val, 1);
}
