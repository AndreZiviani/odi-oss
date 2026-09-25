// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_dal.c -- implementation of the leaves declared in
 * odi_switch_dal.h. See that header for per-leaf trace evidence and the
 * leaf-boundary judgment calls. odi_reg_read/odi_reg_write come from
 * odi_switch_mock.h on the host build, exactly like odi_switch_tbl.c, and
 * from odi_switch_reg.h's __KERNEL__ declarations on the target build.
 */
#include "odi_switch_dal.h"
#include "odi_replay_blob.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/printk.h> /* ODI_SW_STUB_LOG below calls pr_info_once() */
#include <linux/errno.h> /* odi_sw_reg_get/_set's -EINVAL, below */
#else
#include <errno.h> /* same, host build -- EINVAL is 22 on every platform this builds on */
#endif

/* Indirect PHY register addresses read/written through PHY_ACCESS_CMD by
 * odi_sw_port_autoneg_get/_set(). Field-level meaning is not decoded (see
 * the leaf's own doc comment in odi_switch_dal.h), so these are named by
 * address, the same convention that doc comment already uses for the
 * reg_a400/a408/a412 parameters.
 */
#define ODI_SW_PHY_ADDR_A400		0xa400U
#define ODI_SW_PHY_ADDR_A408		0xa408U
#define ODI_SW_PHY_ADDR_A412		0xa412U

/* odi_sw_ponmac_transceiver_get(): the port-1 I2C master device-select
 * word, and the two GPIO pins that gate the two SFP cages' I2C bus. Pin 29
 * carries the caller's gpio_lo/gpio_hi value (it is what toggles between
 * cages); pin 31 is always driven to 1 in this sequence -- its role beyond
 * that is not decoded.
 */
#define ODI_SW_I2C_PORT1_DEVSEL		0x234413aU
#define ODI_SW_GPIO_PIN_I2C_SELECT	29U
#define ODI_SW_GPIO_PIN_I2C_STROBE	31U

/* PONQ_COUNT_MASK table indices with no decoded field-level meaning,
 * named after the parameter that already carries each value (same
 * convention as the PHY addresses above).
 */
#define ODI_SW_PONQ_IDX_QUEUE_BITMASK	207U	/* queue_add_ext() bitmask_207 */
#define ODI_SW_PONQ_IDX_QUEUE_BASE	15U	/* queue_add_ext() base_15 */
#define ODI_SW_PONQ_IDX_QUEUE_VAL	208U	/* queue_add_ext() val_208 */
#define ODI_SW_PONQ_IDX_QUEUE_BITMASK_A	212U	/* queue_add_ext(), !use_213 */
#define ODI_SW_PONQ_IDX_QUEUE_BITMASK_B	213U	/* queue_add_ext(), use_213 */
#define ODI_SW_PONQ_IDX_QUEUE_RESET	60U	/* queue_add_ext(), +n, cleared */
#define ODI_SW_PONQ_IDX_QUEUE_MAX	125U	/* queue_add_ext(), +n, set to 0x3ffffU */
#define ODI_SW_PONQ_IDX_FLOW_BITMASK	37U	/* flow_queue_set() bitmask_37 */
#define ODI_SW_PONQ_IDX_FLOW_WORD	235U	/* flow_queue_set() word235_a/word235_b */
#define ODI_SW_PONQ_IDX_FLOW_VAL_A	20U	/* flow_queue_set(), !use_21 */
#define ODI_SW_PONQ_IDX_FLOW_VAL_B	21U	/* flow_queue_set(), use_21 */
#define ODI_SW_PONQ_IDX_QOS_SCHED_BASE	190U	/* qos_sched_set(), +n */

/* VLAN_MEMBERS full-table sweep (odi_sw_cf_add()): idx range and the
 * sentinel/overwrite values each pass writes, per the trace evidence in
 * the comments below.
 */
#define ODI_SW_VLAN_ID_COUNT		4096U	/* full sweep, idx 0..4095 */
#define ODI_SW_VLAN_ID_LAST_SWEPT	4094U	/* second clear pass upper bound */
#define ODI_SW_VLAN_MEMBERS_SENTINEL	0xf0U	/* full-sweep sentinel */
#define ODI_SW_VLAN_MEMBERS_IDX0_2ND	0xffU	/* idx 0, second write */
#define ODI_SW_VLAN_MEMBERS_IDX1_1ST	0x0003f8ffU	/* idx 1, first overwrite */
#define ODI_SW_VLAN_MEMBERS_IDX1_2ND	0x0003f800U	/* idx 1, second overwrite */

/* Number of switch ports the flood-mask and proto-VLAN-group sweeps cover. */
#define ODI_SW_PORT_COUNT		4U

/* Number of rows in the classify-filter (CF) rule/mask/action tables. */
#define ODI_SW_CF_ROW_COUNT		256U

void odi_sw_l2_aging_set(uint32_t age_spd, uint32_t linkdown_ageout)
{
	uint32_t reg = odi_reg_read(ODI_SW_L2_LOOKUP_SETUP_OFF);

	reg = ODI_SW_L2_LOOKUP_SETUP_AGE_TICKS_SET(reg, age_spd);
	reg = ODI_SW_L2_LOOKUP_SETUP_AGE_ON_LINK_DOWN_SET(reg, linkdown_ageout);
	odi_reg_write(ODI_SW_L2_LOOKUP_SETUP_OFF, reg);
}

void odi_sw_port_autoneg_get(void)
{
	uint32_t reg;

	reg = ODI_SW_PHY_ACCESS_CMD_START_SET(0, 1);
	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);

	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A408);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A412);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);

	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);

	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A408);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A412);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
}

void odi_sw_port_autoneg_set(uint32_t reg_a408, uint32_t reg_a412, uint32_t reg_a400)
{
	uint32_t cmd;

	/* The read-verify pass (odi_sw_port_autoneg_get()) is not
	 * repeated here -- boot3's cmd 30 bracket runs it once, and the
	 * caller (the command table, later) is expected to call _get() then
	 * _set() within the same bracket, as odi_switch_dal_test.c does.
	 */
	odi_reg_write(ODI_SW_PHY_ACCESS_DATA_OFF, ODI_SW_PHY_ACCESS_DATA_WRITE_DATA_SET(0, reg_a408));
	cmd = ODI_SW_PHY_ACCESS_CMD_WRITE_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(cmd, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A408);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);

	odi_reg_write(ODI_SW_PHY_ACCESS_DATA_OFF, ODI_SW_PHY_ACCESS_DATA_WRITE_DATA_SET(0, reg_a412));
	cmd = ODI_SW_PHY_ACCESS_CMD_WRITE_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(cmd, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A412);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);

	/* Read-verify of a400 before the final write -- present in the
	 * captured trace (line 25), not otherwise explained; reproduced as-is.
	 */
	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);

	odi_reg_write(ODI_SW_PHY_ACCESS_DATA_OFF, ODI_SW_PHY_ACCESS_DATA_WRITE_DATA_SET(0, reg_a400));
	cmd = ODI_SW_PHY_ACCESS_CMD_WRITE_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(cmd, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);
}

void odi_sw_port_admin_set(uint32_t port, uint32_t admin_enable)
{
	/* Not observed as a separate register write in the captured trace
	 * (cmd 32): the admin-up/down state is a software flag not traced to
	 * hardware. Parameters kept for the
	 * leaf's documented signature and future callers.
	 */
	(void)port;
	(void)admin_enable;
}

uint32_t odi_sw_port_force_get(uint32_t port)
{
	return odi_reg_read(ODI_SW_PORT_FORCE_SELECT(port));
}

void odi_sw_port_force_set(uint32_t port)
{
	/* boot3: every FORCE_* bit clear -- auto/PHY-driven, no forced
	 * speed/duplex.
	 */
	odi_reg_write(ODI_SW_PORT_FORCE_SELECT(port), 0);
}

uint32_t odi_sw_l2_flood_mask_get(void)
{
	return odi_reg_read(ODI_SW_FLOOD_UNKN_UCAST_PORTS_BASE);
}

void odi_sw_l2_flood_mask_set(uint32_t mask)
{
	unsigned int i;

	/* The register is FLOOD_UNKN_UCAST_PORTS (0x01c028), not
	 * FLOOD_BCAST_PORTS (0x01c020, never written in any capture) --
	 * boot3/boot5 write the address that traces back to this one. It
	 * packs all four ports' flood-enable bits into one word (one bit per
	 * port, item 0..3), so the 4x repeat in the trace is four redundant
	 * writes of the same full word, not four per-port writes.
	 */
	for (i = 0; i < ODI_SW_PORT_COUNT; i++)
		odi_reg_write(ODI_SW_FLOOD_UNKN_UCAST_PORTS_BASE, mask);
}

void odi_sw_ponmac_queue_add(uint32_t th)
{
	/* The register is PORT_QUEUE_MAP (0x01c0c0), not the word at
	 * 0x01c0a0 (never written in any capture) -- same attribution fix as
	 * odi_sw_l2_flood_mask_set() above. PORT_QUEUE_MAP packs
	 * a 2-bit index per port (item 0..3); boot3's 0xd4 does not resolve
	 * cleanly to that 4-item, 2-bit shape (0xd4 needs the full byte), so
	 * it is written raw.
	 */
	odi_reg_write(ODI_SW_PORT_QUEUE_MAP_BASE, th);
}

void odi_sw_ponmac_queue_add_ext(uint32_t n, uint32_t bitmask_207, uint32_t base_15,
				  uint32_t sched_value, uint32_t val_208,
				  uint32_t bitmask_212_213, int use_213)
{
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_BITMASK), bitmask_207);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_BASE), base_15);
	odi_sw_qos_sched_set(n, sched_value);
	/* +208 vs +212/+213 order swaps with n (boot5, all five full-shape
	 * cmd 23 instances checked): n==0 writes +208 then +212/+213 (the
	 * first queue -- see below); n>=1 writes +212/+213 then +208. The
	 * rule is exact across every instance, not a guess: instance 9
	 * (n=0) is 208-then-212; instances 10-13 (n=1..4) are all
	 * 212-or-213-then-208. n==0 is also the only instance where val_208
	 * is 0 -- consistent with a first-queue/steady-state branch that
	 * takes a different code path (and so a different write order) than
	 * every later queue that has to fold its bit into an already
	 * nonzero mask, though reading past the last populated queue is not
	 * confirmed against the register evidence.
	 */
	if (n == 0) {
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_VAL), val_208);
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(use_213 ? ODI_SW_PONQ_IDX_QUEUE_BITMASK_B : ODI_SW_PONQ_IDX_QUEUE_BITMASK_A), bitmask_212_213);
	} else {
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(use_213 ? ODI_SW_PONQ_IDX_QUEUE_BITMASK_B : ODI_SW_PONQ_IDX_QUEUE_BITMASK_A), bitmask_212_213);
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_VAL), val_208);
	}
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_RESET + n), 0);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_MAX + n), 0x3ffffU);
}

void odi_sw_ponmac_transceiver_get(uint32_t gpio_lo, uint32_t gpio_hi)
{
	unsigned int i;

	/* I2C_MASTER_SETUP(1) is the device-select register of the port-1
	 * I2C master odi_i2c_read_bytes() drives, so a DDM read in progress
	 * would carry on with this select word: odi_i2c_lock (odi_switch.c).
	 */
	mutex_lock(&odi_i2c_lock);
	for (i = 0; i < 2; i++) {
		odi_reg_write(ODI_SW_I2C_MASTER_SETUP(1), ODI_SW_I2C_PORT1_DEVSEL);
		odi_reg_write(ODI_SW_PIN_GPIO_SELECT(ODI_SW_GPIO_PIN_I2C_SELECT), gpio_lo);
		odi_reg_write(ODI_SW_PIN_GPIO_SELECT(ODI_SW_GPIO_PIN_I2C_STROBE), 1);
		gpio_lo = gpio_hi;
	}
	mutex_unlock(&odi_i2c_lock);
}

static void __odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id);

void odi_sw_gpon_usflow_set(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg)
{
	unsigned long flags;

	/* The CAM row, its DSF_GEM_FLOW_TYPE word and the slot record change
	 * together under odi_switch_dsf_lock, so the GPON interrupt path
	 * (odi_switch_gpon_encrypt_port()) never finds a slot whose row is
	 * half written.
	 */
	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	(void)__odi_switch_gpon_ds_port_write(idx, gem_port_id, traffic_cfg);
	/* Slot -> gem_port_id bookkeeping for the automatic AES-enable PLOAM
	 * hook (odi_switch_dal.h has the derivation) -- recorded unconditionally,
	 * same posture as the CAM/register write above (odi_switch_gpon_ds_port_
	 * write()'s own return code is likewise not checked here).
	 */
	__odi_switch_ds_slot_record(idx, gem_port_id);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
}

void odi_sw_ponmac_flow_queue_set(uint32_t slot, uint32_t gem_port_id, uint32_t bitmask_37,
				   uint32_t word235_a, uint32_t word235_b,
				   uint32_t val_2021, int use_21)
{
	odi_reg_write(ODI_SW_US_GEM_PORT_MAP(slot), gem_port_id);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_FLOW_BITMASK), bitmask_37);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_FLOW_WORD), word235_a);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_FLOW_WORD), word235_b);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(use_21 ? ODI_SW_PONQ_IDX_FLOW_VAL_B : ODI_SW_PONQ_IDX_FLOW_VAL_A), val_2021);
}

void odi_sw_qos_sched_set(uint32_t n, uint32_t value)
{
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QOS_SCHED_BASE + n), value);
}

/* One pass of the RMA_CTRL block boot3/boot5 write twice per cmd 51
 * instance (boot3 lines 260-310 and 315-364; LINK_MCAST_00/01/02 at
 * 0x1c038/0x1c03c/0x1c040, named after this leaf's first pass).
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
				   const struct odi_sw_vlan_override *vlan_over, unsigned int n_over)
{
	unsigned int i;
	uint32_t idx;
	uint32_t v;

	/* CF rows, in the order the command layer hands them over (see
	 * odi_switch_dal.h for the field layouts). Each row is preceded by
	 * the CLASSIFY_PATTERN_SEL word that holds its template bit -- 32 rows per
	 * word, every row on template 0, so the word is written as 0.
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

	/* VLAN table full sweep, idx 0..4095, all rows to the same sentinel
	 * 0xf0 (boot5: one table write per row -- the captured loop this
	 * mirrors -- folded by odi_mock_table()/the real tracer into one T+D+R).
	 */
	v = ODI_SW_VLAN_MEMBERS_SENTINEL;
	for (idx = 0; idx < ODI_SW_VLAN_ID_COUNT; idx++)
		(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, idx, &v, 1);

	/* VLAN_ACCEPT_FRAMES/VLAN_INGRESS_CHECK/PORT_EGRESS_TAG_MODE interleave,
	 * lines 238-258: VLAN_ACCEPT_FRAMES(0)=0x19, then four
	 * VLAN_INGRESS_CHECK(0)=0xf / PORT_EGRESS_TAG_MODE(port) pairs (Task 2's
	 * primitive), then VLAN_INGRESS_CHECK paired with two unresolved raw
	 * addresses (0x01300c, 0x013010 -- see odi_switch_tbl.h's note on
	 * why these are not named), then VLAN_ACCEPT_FRAMES(0)
	 * again.
	 */
	/* Register map name is VLAN_SETUP (0x013008), not
	 * VLAN_ACCEPT_FRAMES(2) -- boot3/boot5's own write here decodes
	 * cleanly through VLAN_SETUP's four named fields (VID4095_MODE,
	 * VID0_MODE, F_2, FILTER_ON; bit 1 is unused in every
	 * value this leaf writes).
	 */
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF,
		      ODI_SW_VLAN_SETUP_FILTER_ON_SET(
		      ODI_SW_VLAN_SETUP_VID0_MODE_SET(
		      ODI_SW_VLAN_SETUP_VID4095_MODE_SET(0, 1), 1), 1));
	{
		uint32_t tag[4] = { 0, 0, 0, 0 };

		odi_switch_vlan_egress_tag_group_write(0xf, tag);
	}

	/* VLAN idx 1, the sweep's first real overwrite (boot5: differs from
	 * the sweep's 0xf0 sentinel, so it does not extend that run).
	 */
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

	/* RMA_CTRL* template, two passes (lines 260-310, 315-364), each
	 * followed/preceded by the DSCP-remark and IPMC_VLAN_LEAK(0)
	 * writes -- boot3 writes the leaky slot only between the two passes.
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

	/* PROTO_VLAN_GROUP(0..3), each written twice, all 0 (lines 365-372). */
	for (i = 0; i < ODI_SW_PORT_COUNT; i++) {
		odi_reg_write(ODI_SW_PROTO_VLAN_GROUP(i), 0);
		odi_reg_write(ODI_SW_PROTO_VLAN_GROUP(i), 0);
	}

	/* PORT_PROTO_VLAN, 16 slots: PRIO=0, VLAN_INDEX=1, IN_USE=0
	 * (lines 373-388).
	 */
	for (i = 0; i < 16; i++)
		odi_reg_write(ODI_SW_PORT_PROTO_VLAN(i),
			      ODI_SW_PORT_PROTO_VLAN_VLAN_INDEX_SET(0, 1));

	/* Closing VLAN_ACCEPT_FRAMES(0) state-machine sequence and
	 * final VLAN_INGRESS_CHECK-adjacent writes (lines 389-398) -- a
	 * status/latch sweep not decoded further; reproduced
	 * as the literal sequence the captured trace shows.
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

	/* VLAN idx 0, second write of the bracket (0xff, replacing the
	 * sweep's 0xf0 sentinel for this row).
	 */
	v = ODI_SW_VLAN_MEMBERS_IDX0_2ND;
	(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, 0, &v, 1);

	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0x1000U);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0x0U);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0x1000U);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0x0U);

	/* VLAN idx 1, second write (0x0003f800, replacing the first
	 * overwrite 0x0003f8ff -- the low byte drops from 0xff to 0x00).
	 */
	v = ODI_SW_VLAN_MEMBERS_IDX1_2ND;
	(void)odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, 1, &v, 1);

	/* VLAN idx 2..4094, the second (row-by-row) clear pass: 0 everywhere
	 * except the caller's per-connection rows (vlan_over) -- idx 4095 is
	 * never rewritten after the sweep in any boot5 bracket, so the loop
	 * stops at 4094: this is not an artifact of a range cutoff -- boot5's own
	 * complete brackets all end their last run at idx 4094, confirmed
	 * across all twelve).
	 */
	for (idx = 2; idx <= ODI_SW_VLAN_ID_LAST_SWEPT; idx++) {
		v = 0;
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

#ifdef __KERNEL__
#define ODI_SW_STUB_LOG(fmt, ...) pr_info_once("odi_switch: " fmt, ##__VA_ARGS__)
/* Not "once": odi_switch_init_platform() is now a deliberately re-triggerable
 * call, and each trigger's own starting/done pair needs to land in the
 * log, not just the first ever.
 */
#define ODI_SW_INIT_LOG(fmt, ...) pr_info("odi_switch: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define ODI_SW_STUB_LOG(fmt, ...) printf("odi_switch: " fmt, ##__VA_ARGS__)
#define ODI_SW_INIT_LOG(fmt, ...) printf("odi_switch: " fmt, ##__VA_ARGS__)
#endif

/* --- odi_switch_init_platform() ----------------------------------------
 *
 * Items below run in ranked order, items 1-8. Board port numbering (from
 * src/omci/omci_caps.h): the single UNI is switch port 0, the PON port is
 * 2, the CPU port is 3; port 1 is unused on this board. ALL_PORTS below
 * (0..3) covers every port including CPU/PON (VLAN/SVLAN baseline);
 * UNI_PORTS/PON_PORT are the UNI ports plus the PON port (ACL ingress
 * state, item 2). The register writes each item makes are the ones a
 * captured stock module-load writes (odi_switch_dal.h, "module-load
 * replay"), compared against live register reads on a stock stick.
 *
 * Every register/table used below was resolved from the register map
 * extracted from the stock diag binary (src/diag/generated/regmap.c) plus
 * the table descriptors this codebase already built
 * (odi_switch_hw.h's own odi_sw_table_desc[]).
 *
 * NOT called from module init any more (s4's own boot hang -- the RAM log
 * ended at "odi_omci: ready" with not one odi_switch line, so module init
 * entered here and never returned, before this function's own first log
 * line could print).
 * `mask` selects which items run, one bit per item (ODI_SWITCH_INIT_
 * PLATFORM_ITEM(n) = 1 << (n-1)); a caller must ask explicitly -- there
 * is no "run everything" default baked in here, odi_switch.c's trigger
 * plumbing is what decides that. Every item logs "starting" before it
 * touches hardware and "done" (or the "not encoded" stub line) after,
 * so a boot log truncated mid-item, the same way s4's was, still names
 * the exact item that was running when it stopped.
 *
 * Root cause of s4's own hang and s5's own SoC stall, found and fixed separately:
 * odi_switch.c ioremapped the wrong physical base (0xB8000000, the
 * SoC-level chip-control window, 20 KB) for every offset this file
 * writes -- CLASSIFY_SETUP at that base landed 9 MB past the window's end, in
 * address space the bus never claims, and the bare store stalled the CPU
 * with no timeout. The real switch-core base is 0x1B000000
 * (ODI_SWITCH_MMIO_BASE, odi_switch_hw.h); item 1 alone (one CLASSIFY_SETUP
 * write) reproduced the stall on s5 before this was known. Confirmed
 * independently with read-only memprobe on the live OEM stick: CLASSIFY_SETUP,
 * ACL_PORT_ENABLE, FLOOD_BCAST_PORTS and TABLE_CMD at `0x1B000000 + offset` all
 * read plausible values -- see items 1 and 2 below for what those
 * readings imply about each item.
 *
 * Ordering caveat (checked, not the cause this time, still a real
 * consideration for whoever runs this next): on the stock image this
 * platform init ran when the OMCI kernel modules loaded, well after
 * /proc/rtk_init's own "switch" verb had already run. In the captured
 * stock boot the "switch" verb is where the classify, ACL, VLAN, SVLAN
 * and L2 blocks get their baseline writes, so every item below assumes
 * that verb has run first (item 1: classify; item 2: ACL; item 4: VLAN
 * and SVLAN; item 5: L2; item 6: classify again). Calling this function before /proc/rtk_init's "switch" verb
 * has run is expected to be unsafe regardless of which trigger fires it
 * -- the deliberate triggers below (a /proc/odi_omci write, or the first
 * OP_CMD) make a bad call cost one trial instead of one boot, they do
 * not by themselves guarantee the ordering is safe; whoever
 * issues the trigger needs "switch" to have already run.
 */
#define ODI_SWITCH_INIT_PLATFORM_UNI_PORT	0U
#define ODI_SWITCH_INIT_PLATFORM_PON_PORT	2U
#define ODI_SWITCH_INIT_PLATFORM_ALL_PORTS_N	4U

/* Item 8: ACL start-index bookkeeping (the number of ACL rows the system
 * reserves for itself, read from live hardware/config state there is no way
 * to read -- see odi_switch_dal.h's comment on this variable).
 * Zero is NOT a confirmed value; it is the only default that does not
 * pretend to know a number that was never read.
 */
uint32_t odi_switch_acl_start_idx;

void odi_switch_init_platform(uint32_t mask)
{
	uint32_t reg;
	uint32_t port;
	uint32_t i;
	static const uint32_t zero2[2] = { 0, 0 };
	static const uint32_t zero3[3] = { 0, 0, 0 };

	if (mask == 0) {
		ODI_SW_INIT_LOG("init_platform: called with an empty mask, nothing to do\n");
		return;
	}

	/* Item 1: classifier unmatched-frame action -- CLASSIFY_SETUP.US_NO_MATCH_ACTION = 1
	 * (register map field; raw values 0=normal, 1=no-PON-match-required,
	 * 2=drop). Read-modify-write: CLASSIFY_SETUP also
	 * carries the two WAN-interface default fields (F_20_17, F_16_13) and
	 * PATTERN1_COUNT, which this
	 * item does not own.
	 *
	 * Live OEM reading:
	 * `memprobe read 0x1b01600c` = 0x00001c09. Decoded against the field
	 * list above: US_NO_MATCH_ACTION = 0x1c09 & 0x3 = 1 -- ALREADY the value
	 * this item sets. DS_NO_MATCH_ACTION=0, PON_SELECT=1, PATTERN1_COUNT=
	 * 0xE0 (224), F_20_17/F_16_13=0. So on this line item 1
	 * is a NO-OP against the OEM's own running state -- the write still
	 * happens (idempotent: OR-in a bit that is already set changes
	 * nothing), which is the correct behaviour either way, and is left
	 * unchanged. Kept as a read-modify-write regardless: a blind
	 * write here would have zeroed PON_SELECT and PATTERN1_COUNT,
	 * both OEM-set and both outside this item's own field.
	 *
	 * PATTERN1_COUNT divergence: a fresh s6 boot with init_platform triggered reads
	 * CLASSIFY_SETUP=0x1009 (PATTERN1_COUNT=0x80/128), not the OEM's 0xE0/224 --
	 * bits 10-11 (part of PATTERN1_COUNT, lsp5 len8, i.e. bits 5-12)
	 * clear where the OEM has them set. Checked whether this item's own
	 * read-modify-write drops them: it does not -- ODI_SW_CLASSIFY_SETUP_US_NO_MATCH_ACTION_SET
	 * only ever touches bits 0-1, confirmed by its own mask/shift
	 * (odi_switch_hw.h). The value 128 is not our bug at all: the
	 * captured /proc/rtk_init "switch" verb writes 0x80 to this exact
	 * field as its final value -- 128 is not left over
	 * noise, it is the boot default our fresh, unprovisioned
	 * boot correctly shows. The captured module-load never writes this
	 * field -- PATTERN1_COUNT is not a platform-init constant.
	 * The OEM's 224 is CF pattern-1/pattern-0 entry-space allocator
	 * state: it is the boundary between two CF table partitions, and
	 * grows as the stock OMCI modules allocate more
	 * pattern-1 (CLS_RULE_B-style) entries across whatever bridge
	 * connections/services it has provisioned over its uptime -- exactly
	 * the same allocator category as odi_switch_acl_start_idx (item 8)
	 * and the ACL-row bookkeeping cmd 25/26/51 do not yet track. Setting
	 * PATTERN1_COUNT=224 here would not "match the OEM's default": it
	 * would hardcode a live snapshot of that OEM's own accumulated
	 * provisioning history as if it were a boot-time constant, which is
	 * wrong on a fresh line with a different (here: zero) service count
	 * and would need to grow again as our own future CF-table allocator
	 * (not yet built) creates pattern-1 entries. NOT done here --
	 * evidence-backed conclusion, not a guess, and not a fix this item
	 * should make.
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(1)) {
		ODI_SW_INIT_LOG("init_platform: item 1 starting (CLASSIFY_SETUP.US_NO_MATCH_ACTION=1)\n");
		reg = odi_reg_read(ODI_SW_CLASSIFY_SETUP_OFF);
		reg = ODI_SW_CLASSIFY_SETUP_US_NO_MATCH_ACTION_SET(reg, 1);
		odi_reg_write(ODI_SW_CLASSIFY_SETUP_OFF, reg);
		ODI_SW_INIT_LOG("init_platform: item 1 done (CLASSIFY_SETUP.US_NO_MATCH_ACTION=1, permit-without-PON)\n");
	}

	/* Item 2: ACL ingress enable for every UNI port and
	 * the PON port -- ACL_PORT_ENABLE, one bit per port (the captured
	 * "switch" verb, run by rcS before omcimods, clears every port bit
	 * first). Read-modify-write
	 * since other ports' bits (unused on this board) should not be
	 * disturbed. Two separate read-modify-write calls, one per port,
	 * matching the per-port writes the capture shows rather than one
	 * combined write.
	 *
	 * Live OEM reading:
	 * `memprobe read 0x1b015040` = 0x00000005 = bit0 (UNI port 0) and
	 * bit2 (PON port 2) both set, bits1/3 (unused port, CPU port) clear
	 * -- exactly the pattern this item sets. So on this line ACL_PORT_ENABLE is
	 * ALREADY correct at the moment this was read; the "switch" verb
	 * clear-everything step either has not run yet at read time,
	 * already been undone by something else on the OEM image, or this
	 * particular field survives whatever DISABLE does -- not
	 * distinguished by a single snapshot read. Either way this item is a
	 * no-op against that snapshot, and the two RMW calls (already the
	 * existing implementation, unmodified here) mean a call here can
	 * only OR bits in, never clear one the OEM had set.
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(2)) {
		ODI_SW_INIT_LOG("init_platform: item 2 starting (ACL_PORT_ENABLE, UNI+PON ports)\n");
		reg = odi_reg_read(ODI_SW_ACL_PORT_ENABLE_OFF);
		reg |= 1U << ODI_SWITCH_INIT_PLATFORM_UNI_PORT;
		odi_reg_write(ODI_SW_ACL_PORT_ENABLE_OFF, reg);
		reg = odi_reg_read(ODI_SW_ACL_PORT_ENABLE_OFF);
		reg |= 1U << ODI_SWITCH_INIT_PLATFORM_PON_PORT;
		odi_reg_write(ODI_SW_ACL_PORT_ENABLE_OFF, reg);
		ODI_SW_INIT_LOG("init_platform: item 2 done (ACL_PORT_ENABLE, UNI port %u + PON port %u)\n",
				ODI_SWITCH_INIT_PLATFORM_UNI_PORT, ODI_SWITCH_INIT_PLATFORM_PON_PORT);
	}

	/* Item 3: ACL template index 1 = [DMAC0-2, SMAC0-2, CTAG,
	 * GEMPORT] -- LEFT OUT. SW_0x015008 (0x015008) is addressed
	 * by TWO independent indices (template index 0..6, field slot
	 * 0..7). The register map entry
	 * for this register (src/diag/generated/regmap.c)
	 * gives "array offset 7 / array index 0..7 / port index 0..6" but
	 * not the actual address arithmetic combining the two indices into
	 * one offset, and no existing macro in this codebase covers a
	 * 2-dimensional register family. Guessing the stride risks writing
	 * a different template index or field slot than intended -- exactly
	 * the TABLE_KIND hazard the coordinator flagged for the table
	 * descriptor work, same posture here: leave it out rather than
	 * guess. Not redundant with the "switch" verb, which
	 * leaves template index 1 with a
	 * different field set -- CTAG/IP4SIP0-1/VIDRANGE/IPRANGE/
	 * PORTRANGE/IP4DIP0-1 -- so leaving this out means index 1 keeps
	 * that shape instead of DMAC/SMAC/CTAG/GEMIDXLLIDX; any ACL rule
	 * added later that references index 1 assuming the module-load shape is
	 * silently wrong until this item is resolved).
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(3)) {
		ODI_SW_INIT_LOG("init_platform: item 3 starting (ACL template index 1)\n");
		ODI_SW_STUB_LOG("init_platform: item 3 (ACL template index 1) not encoded -- 2D register addressing not resolved, see odi_switch_dal.c\n");
	}

	/* Item 4: VLAN/SVLAN baseline. Live OEM confirmation: a fresh s6 boot
	 * reads VLAN_SETUP=0x0001,
	 * SVLAN_SETUP=0x0000 and PORT_VLAN_INDEX=0x1001 before init_platform runs,
	 * and VLAN_SETUP=0x0018, SVLAN_SETUP=0x0009, PORT_VLAN_INDEX=0 after --
	 * matching the OEM's own 0x18/0x09/0 exactly. So this item is
	 * confirmed real, working, converging behaviour on hardware, not a
	 * no-op and not still-open. VLAN_INGRESS_CHECK is also compared, see
	 * below. SVLAN_UPLINK_PORTS has no live OEM reading (not in either
	 * probe) -- unconfirmed, but was already read-modify-write (per-port
	 * accumulating RMW, see the loop below), so nothing
	 * to change regardless of what a future reading shows.
	 */
	/* VLAN filtering off -- VLAN_SETUP.FILTER_ON = 0 (the "switch" verb
	 * enables it at boot, the same "boot default is the opposite of what
	 * the module load wants" pattern as item 2).
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(4)) {
	ODI_SW_INIT_LOG("init_platform: item 4 starting (VLAN/SVLAN baseline)\n");
	reg = odi_reg_read(ODI_SW_VLAN_SETUP_OFF);
	reg = ODI_SW_VLAN_SETUP_FILTER_ON_SET(reg, 0);
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF, reg);

	/* Port PVID 0 for every port -- PORT_VLAN_INDEX(port).
	 * Written for ports 0 and 1 only:
	 * ODI_SW_PORT_VLAN_INDEX(n) = PORT_VLAN_INDEX_BASE + 4*n (odi_switch_hw.h's
	 * "wide, one-word-per-item" pattern) puts port 2 at 0x013014 and
	 * port 3 at 0x013018 -- 0x013014 is the register map's own separate
	 * SW_0x013014 entry (odi_switch_init_platform_test.c caught this the
	 * same way it caught the PORT_SVLAN_INDEX collision above), and
	 * odi_switch_hw.h's own PORT_VLAN_INDEX macro comment already says only
	 * slots 0 and 1 were ever confirmed by a real trace. Writing ports 2
	 * and 3 under this formula would collide with SW_0x013014 (and
	 * whatever, if anything, real hardware puts after it) instead of
	 * PORT_VLAN_INDEX's own port-2/3 storage -- logged, not guessed.
	 *
	 * VLAN 0 creation and its all-port membership (the VLAN table row 0
	 * membership write) are NOT done
	 * here either: the VLAN table row 0 content this leaf would need is
	 * the same undecoded row-field layout odi_sw_cf_add()'s
	 * own file header already flags as unresolved (no FIELD lines for a
	 * VLAN table row exist in the register map, only for the plain
	 * registers around it).
	 */
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0);
	ODI_SW_STUB_LOG("init_platform: item 4 (VLAN_SETUP.FILTER_ON=0, PORT_VLAN_INDEX ports 0-1) done\n");
	ODI_SW_STUB_LOG("init_platform: item 4 (PORT_VLAN_INDEX ports 2-3, VLAN table row 0 membership) not encoded -- see odi_switch_dal.c\n");

	/* VLAN_INGRESS_CHECK port 1 ingress filter, disabled. A fresh s6 boot reads
	 * VLAN_INGRESS_CHECK=0x0f (all 4 ports' own INGRESS bit set) against the OEM's own
	 * 0x0d (bit 1, port 1, clear) -- nothing in the captured module
	 * load touches VLAN_INGRESS_CHECK at all, so this
	 * divergence is not something item 4 was already meant to close.
	 * The register map gives one INGRESS bit per port, and the "switch"
	 * verb sets EVERY port bit as its default -- matching our own
	 * fresh-boot 0x0f exactly, the same "boot default differs from
	 * what the stock stack later narrows" pattern as items 2 and 4's
	 * VLAN_SETUP/SVLAN_SETUP fields. Unlike PATTERN1_COUNT above, no
	 * captured write clearing port 1 specifically was found in this
	 * pass. It is set here anyway, evidence-backed but not capture-
	 * confirmed: port 1 is unused on this exact board (the single UNI is
	 * switch port 0; port 1 is unused), so
	 * disabling its VLAN ingress filter cannot affect any live traffic
	 * (no frames ever arrive there), and the target value comes from a
	 * live OEM stick of the same hardware class rather than a guess.
	 * Read-modify-write: VLAN_INGRESS_CHECK packs all 4 ports into one word.
	 */
	reg = odi_reg_read(ODI_SW_VLAN_INGRESS_CHECK_BASE);
	reg = ODI_SW_VLAN_INGRESS_CHECK_ON_SET(reg, 1, 0);
	odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK_BASE, reg);
	ODI_SW_INIT_LOG("init_platform: item 4 (VLAN_INGRESS_CHECK port 1 disabled, matching the OEM -- evidence-backed, not capture-confirmed) done\n");

	/* SVLAN service port enabled for every port, SVLAN filtering off,
	 * untagged action = port-based SVID -- SVLAN_UPLINK_PORTS
	 * (per-port EN bit), SVLAN_SETUP.FILTER_ON, SVLAN_SETUP.UNTAGGED_ACTION=2
	 * (the values the captured module load leaves and a live stock stick
	 * reads back).
	 *
	 * Port SVID 0 per port is LEFT OUT: PORT_SVLAN_INDEX(port) is computed
	 * as PORT_SVLAN_INDEX base + 4*port (the "wide,
	 * one-word-per-item" pattern every other per-port register in
	 * odi_switch_hw.h that does not fit one packed word uses), but that
	 * formula puts PORT_SVLAN_INDEX(2) at 0x01400c -- the exact address the
	 * register map assigns to the unrelated SVLAN_SETUP register two
	 * entries later in the same map (odi_switch_init_platform_test.c
	 * caught this: the sweep of SVLAN_SETUP by that formula collided with
	 * this leaf's own separate SVLAN_SETUP writes). PORT_SVLAN_INDEX's real
	 * per-port stride is therefore not 4 bytes, and remains
	 * unresolved -- writing 0 to it under the wrong stride would
	 * silently corrupt SVLAN_SETUP or some other register instead.
	 * Logged, not guessed.
	 */
	for (port = 0; port < ODI_SWITCH_INIT_PLATFORM_ALL_PORTS_N; port++) {
		/* SVLAN_UPLINK_PORTS packs all 4 ports' EN bits into one word
		 * (odi_switch_hw.h field widths); read-modify-write per port,
		 * matching the per-port writes the capture shows
		 * -- a literal (0, port, 1) SET here would clear every
		 * previously-set port's bit instead of accumulating them.
		 */
		reg = odi_reg_read(ODI_SW_SVLAN_UPLINK_PORTS_BASE);
		reg = ODI_SW_SVLAN_UPLINK_PORTS_UPLINK_SET(reg, port, 1);
		odi_reg_write(ODI_SW_SVLAN_UPLINK_PORTS_BASE, reg);
	}
	ODI_SW_STUB_LOG("init_platform: item 4 (per-port SVID) not encoded -- PORT_SVLAN_INDEX per-port address stride not resolved, see odi_switch_dal.c\n");
	reg = odi_reg_read(ODI_SW_SVLAN_SETUP_OFF);
	reg = ODI_SW_SVLAN_SETUP_FILTER_ON_SET(reg, 0);
	reg = ODI_SW_SVLAN_SETUP_UNTAGGED_ACTION_SET(reg, 2); /* port-based SVID */
	odi_reg_write(ODI_SW_SVLAN_SETUP_OFF, reg);
	ODI_SW_STUB_LOG("init_platform: item 4 (SVLAN_UPLINK_PORTS all ports, SVLAN_SETUP.FILTER_ON=0/UNTAGGED_ACTION=2) done\n");
	ODI_SW_INIT_LOG("init_platform: item 4 done (VLAN/SVLAN baseline)\n");
	}

	/* Item 5: L2 CAM enabled -- L2_LOOKUP_SETUP.CAM_OFF = 0 (the field
	 * is a disable bit, per its register map name).
	 * Read-modify-write: L2_LOOKUP_SETUP also carries AGE_TICKS/AGE_ON_LINK_DOWN
	 * (odi_sw_l2_aging_set()'s own fields), which this item does not own
	 * -- a blind write here would zero whatever aging timer the OEM (or
	 * cmd 62 in a live boot) had set.
	 *
	 * No live OEM reading exists for L2_LOOKUP_SETUP (the memprobe pass only
	 * covered CLASSIFY_SETUP/ACL_PORT_ENABLE/FLOOD_BCAST_PORTS/TABLE_CMD),
	 * so whether CAM_OFF is already 0 on this line is unconfirmed; the
	 * read-modify-write above was already in place regardless and needs
	 * no change either way.
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(5)) {
		ODI_SW_INIT_LOG("init_platform: item 5 starting (L2_LOOKUP_SETUP.CAM_OFF=0, L2 CAM)\n");
		reg = odi_reg_read(ODI_SW_L2_LOOKUP_SETUP_OFF);
		reg = ODI_SW_L2_LOOKUP_SETUP_CAM_OFF_SET(reg, 0);
		odi_reg_write(ODI_SW_L2_LOOKUP_SETUP_OFF, reg);
		ODI_SW_INIT_LOG("init_platform: item 5 done (L2_LOOKUP_SETUP.CAM_OFF=0, L2 CAM)\n");
	}

	/* Item 6: the stale-CF sweep (clearing old CF rows before installing
	 * fresh ones) writes an all-zero CLS_RULE_B row plus all-zero
	 * CLS_DS_ACTION/CLS_US_ACTION rows (CLS_MASK_B is untouched by a
	 * delete). This is an inference, not a confirmed bit pattern: every
	 * one of boot5's own 12 real CLS_RULE_B rows
	 * (odi_switch_dal.c's own cf1..cf12
	 * arrays) has word 0 = 0x00010000 and nothing else ever seen there,
	 * consistent with bit 16 being the valid flag and an all-zero row
	 * being the natural "invalid" encoding -- not independently
	 * confirmed, but
	 * grounded in every real row this codebase has ever decoded,
	 * so implemented rather than left out. 256 rows: CLS_RULE_B/
	 * CLS_DS_ACTION/CLS_US_ACTION all declare table size 256
	 * (odi_switch_hw.h's own odi_sw_table_desc[]).
	 *
	 * The DS-direction default-forward CF entry at index 255 is LEFT
	 * OUT: its action word content (a FORCE_FORWARD disposition packed
	 * into a CLS_DS_ACTION row) is not derivable from anything captured
	 * so far,
	 * and unlike the delete sweep above there is no all-zero-row
	 * fallback that plausibly represents "force forward" -- guessing a
	 * non-zero action word here has a real chance of programming the
	 * wrong disposition. Logged, not guessed.
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(6)) {
		ODI_SW_INIT_LOG("init_platform: item 6 starting (stale-CF sweep, 256 rows)\n");
		/* Each odi_switch_table_write() call is individually bounded by
		 * ODI_SWITCH_TBL_MAX_SPINS (odi_switch_tbl.c) and logs its own
		 * rate-limited failure naming the table and row -- this loop
		 * itself is a plain bounded for loop (256 iterations, no
		 * condition that can spin), so nothing extra to bound here; a
		 * failing row is reported by the callee and the sweep continues
		 * to the next row rather than aborting, since every row is
		 * independent and a truncated sweep would leave the remaining
		 * rows in an unknown state.
		 */
		for (i = 0; i < ODI_SW_CF_ROW_COUNT; i++) {
			(void)odi_switch_table_write(ODI_SW_TBL_CLS_RULE_B, i, zero2, 2);
			(void)odi_switch_table_write(ODI_SW_TBL_CLS_DS_ACTION, i, zero3, 3);
			(void)odi_switch_table_write(ODI_SW_TBL_CLS_US_ACTION, i, zero3, 3);
		}
		ODI_SW_INIT_LOG("init_platform: item 6 done (stale-CF sweep, 256 rows x CLS_RULE_B/CLS_DS_ACTION/CLS_US_ACTION)\n");
		ODI_SW_STUB_LOG("init_platform: item 6 (the DS default-forward CF entry) not encoded -- CLS action row packing for FORCE_FORWARD unresolved\n");
	}

	/* Item 7: reserved VIDs 0 and 4095 get the "tag" action --
	 * VLAN_SETUP.VID0_MODE=1, VID4095_MODE=1 (raw value 1 in both
	 * fields, as a live stock stick reads back). Read-modify-
	 * write together with FILTER_ON above -- re-read since that
	 * write already landed.
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(7)) {
		ODI_SW_INIT_LOG("init_platform: item 7 starting (reserved-VID tag action, SVLAN C-TAG priority reference)\n");
		reg = odi_reg_read(ODI_SW_VLAN_SETUP_OFF);
		reg = ODI_SW_VLAN_SETUP_VID0_MODE_SET(reg, 1);
		reg = ODI_SW_VLAN_SETUP_VID4095_MODE_SET(reg, 1);
		odi_reg_write(ODI_SW_VLAN_SETUP_OFF, reg);

		/* SVLAN priority from the C-TAG -- SVLAN_SETUP.PRIO_SOURCE=1
		 * (the 802.1Q C-TAG priority source).
		 */
		reg = odi_reg_read(ODI_SW_SVLAN_SETUP_OFF);
		reg = ODI_SW_SVLAN_SETUP_PRIO_SOURCE_SET(reg, 1);
		odi_reg_write(ODI_SW_SVLAN_SETUP_OFF, reg);
		ODI_SW_INIT_LOG("init_platform: item 7 done (VLAN_SETUP reserved-VID tag action, SVLAN_SETUP C-TAG priority reference)\n");
	}

	/* Item 8: odi_switch_acl_start_idx bookkeeping -- see its own
	 * declaration comment in odi_switch_dal.h. No register write; left
	 * at its zero-initialized default here since there is no way
	 * to read the stock stack's own reserved-ACL-row count.
	 */
	if (mask & ODI_SWITCH_INIT_PLATFORM_ITEM(8)) {
		ODI_SW_INIT_LOG("init_platform: item 8 starting (odi_switch_acl_start_idx)\n");
		ODI_SW_STUB_LOG("init_platform: item 8 (odi_switch_acl_start_idx) left at unconfirmed default 0, no register write\n");
		ODI_SW_INIT_LOG("init_platform: item 8 done (odi_switch_acl_start_idx, no register write)\n");
	}
}

/* odi_switch_platform_init_rc/_trigger() -- the lazy-trigger "did it fire
 * yet" bookkeeping odi_omci.c reads for /proc/odi_omci status. -1 not
 * triggered yet, 0 once a trigger has called odi_switch_init_platform()
 * with a nonzero mask at least once (that function has no failure path to
 * report, so this is "did a trigger fire" rather than a real error code).
 * Static here rather than a plain global odi_omci.c reaches by extern: this
 * state belongs to the trigger, and the trigger is pure dal-leaf logic (no
 * kernel dependency), the same reasoning odi_switch_parity_init_rc below
 * already follows. The mask==0 guard duplicates odi_switch_init_platform()'s
 * own, deliberately: calling the trigger with an empty mask must leave rc at
 * -1 (not triggered), which calling through to a function that itself no-ops
 * on empty mask would not preserve on its own.
 */
static int odi_switch_platform_init_rc = -1;

int odi_switch_platform_init_rc_get(void)
{
	return odi_switch_platform_init_rc;
}

void odi_switch_platform_init_trigger(uint32_t mask)
{
	if (mask == 0)
		return;
	odi_switch_init_platform(mask);
	odi_switch_platform_init_rc = 0;
}

/* --- odi_switch_init_parity() ------------------------------------------
 *
 * tools/regdump's whole-switch A/B dumped a working image (v6, stock OMCI
 * kernel modules restored) and a broken one (s7, SWITCH=oss with
 * odi_switch_init_platform() already run) at the same boot moment. 64
 * addresses differed; ACL template/action/enable, LUT flood and CLASSIFY_SETUP are
 * excluded from this table (most likely OLT-provisioning-dependent state
 * that only one of the two compared captures received -- a blind write
 * there risks programming a rule the OLT never asked for). The 14 entries
 * below are the rest, in the coordinator's
 * own given order, every offset/value/name taken from the stock diag register
 * map's own array/port packing model (tools/regdump/
 * mklist.py: item i of a wide register lands at offset+4*i, i enumerated
 * array-major -- (array_index-array_min)*port_count + (port_index-
 * port_min); confirmed for this map, not assumed, by the PRIO_QUEUE_MAP/
 * PORT_QUEUE_MAP address collision entry 11 documents below, which only
 * resolves to a valid port index under that ordering) plus a column-by-column
 * comparison of two register captures. Port numbering matches odi_switch_init_platform()'s
 * own comment: UNI port 0, PON port 2, CPU port 3, port 1 unused on this
 * board.
 *
 * Every entry was looked up in the same place for all 14 (the
 * register table listing) -- no per-entry
 * behavioural lookup was done,
 * unlike odi_switch_init_platform()'s items: this table's job is bit-for-
 * bit parity with a known-working capture, not a semantic rewrite, so the
 * register map's FIELD lines are used only to explain what each value
 * means, never to recompute it.
 */
static const struct odi_sw_parity_entry odi_switch_parity_table_[] = {
	/* 0: MAX_FRAME_LEN_1 (id 214, 0x011018, single word, no array/port
	 * dimension). Field BYTES, lsp 0 len 14: the max
	 * frame length (bytes) this length-check profile accepts (a sibling
	 * SW_0x023034 exists at 0x023034 for a different profile). v6 =
	 * 0x7ef = 2031 bytes -- large enough for a double-tagged/QinQ frame
	 * plus GEM overhead. s7 = 0: an accepted-max-length of ZERO, which
	 * would treat every non-empty frame checked against this profile as
	 * oversize and drop it at the MAC. The single strongest candidate in
	 * this table for "frames never reach the OLT."
	 */
	{ 0x011018U, 0x000007efU, 0xffffffffU, "MAX_FRAME_LEN_1" },

	/* 1: SW_0x02d82c (id 785, base 0x02d82c, width 10, array 1..7 x
	 * port 0..3 -- one word per (queue, port), wide packing since
	 * 10*28 > 32). Field F_9_0, lsp 0 len 10: the WFQ scheduling
	 * weight for one of internal egress queues 1-7 on one port. The
	 * diverging word (0x02d878, item i=19) decodes to queue 5, port 3
	 * (the CPU port) under (queue-1)*4+port=19 -> queue=5, port=3 -- the
	 * only (queue,port) pair in range that solves the offset, and array-
	 * major ordering is independently confirmed by entry 11's collision
	 * below. v6 weight = 0x7f = 127 (a real, non-maximal weight); s7 = 0
	 * -- the CPU port's queue 5 gets NO scheduled service share on
	 * egress. If OMCI replies leave the CPU port through this queue,
	 * weight 0 under WFQ can starve them indefinitely.
	 */
	{ 0x02d878U, 0x0000007fU, 0xffffffffU, "SW_0x02d82c[q5,p3]" },

	/* 2: SW_0x02d87c (id 788, base 0x02d87c, width 3, array 0..7
	 * x port 0..3, same wide packing as entry 1). Field IDX, lsp 0 len 3:
	 * selects which of 8 auto-policing-rate meters a queue/port pulls
	 * from. The diverging word (0x02d888, item i=3) decodes to queue 0,
	 * port 3 (CPU) under queue*4+port=3 -> queue=0,port=3. This same
	 * word is ALSO SW_0x02d82c's own item for queue 6, port 3 (base
	 * 0x02d82c, offset 0x5c=item 23, (23-1... queue-1)*4+port=23 ->
	 * queue=6,port=3) -- the register map declares both registers at
	 * this address, an overlap odi_switch_hw.h's own header comment
	 * already documents as a real property of this map, not an
	 * extraction bug. v6's raw value (0x1fffff) is far larger than
	 * either field's own documented width (3 bits for IDX, 10 for
	 * F_9_0), confirming the map's field decode for this one word is
	 * incomplete; taken verbatim regardless, since parity with v6's own
	 * capture, not a field-level rewrite, is this table's job. Port 3
	 * (CPU) either way.
	 */
	{ 0x02d888U, 0x001fffffU, 0xffffffffU, "SW_0x02d87c[q0,p3]/SW_0x02d82c[q6,p3] (map collision)" },

	/* 3-6: SW_0x023144 (id 869, base 0x023144, width 32,
	 * array 0..15, port 0..0 -- one word per array item, i = array
	 * index directly). Fields OFFSET (lsp 3 len 8, a byte offset into
	 * the frame the generic classification parser extracts) and FMT
	 * (lsp 0 len 3, the extraction format code). The four diverging
	 * words are array items 11-14 (0x023170/174/178/17c = base + 4*11
	 * .. base + 4*14): entry 3 OFFSET=0x2c(44) FMT=1, entry 4
	 * OFFSET=0x2a(42) FMT=1, entry 5 OFFSET=0x28(40) FMT=1, entry 6
	 * OFFSET=0x26(38) FMT=1 -- four consecutive parser slots, each
	 * pulling a format-1 field two bytes further into the frame than
	 * the last, the shape of successive protocol-header fields used by
	 * ACL/QoS classification. s7 leaves all four at zero (disabled).
	 * Plausibly ACL-classification support rather than a bare transport
	 * blocker (the same family CLASSIFY_SETUP/SW_0x015008 were excluded
	 * for), included anyway per the coordinator's explicit list.
	 */
	{ 0x023170U, 0x00000161U, 0xffffffffU, "SW_0x023144[11]" },
	{ 0x023174U, 0x00000151U, 0xffffffffU, "SW_0x023144[12]" },
	{ 0x023178U, 0x00000141U, 0xffffffffU, "SW_0x023144[13]" },
	{ 0x02317cU, 0x00000131U, 0xffffffffU, "SW_0x023144[14]" },

	/* 7: SW_0x02d018 (id 739, base 0x02d018, width 10, array 0..0,
	 * port 0..3 -- one word per port, i = port directly). Field TH,
	 * lsp 0 len 10: the per-port egress drop threshold, in the flow-
	 * control block's page-count units. The diverging word (0x02d024,
	 * item i=3) is port 3 (CPU). v6 = 0x134 = 308, s7 = 0x34a = 842 --
	 * s7's threshold is HIGHER (more permissive) than v6's, the opposite
	 * direction from "starves traffic": a higher drop threshold should
	 * make drops on this port LESS likely, not more. Included per
	 * instruction; this one points away from, not toward, the OLT
	 * stall, not a candidate root cause by itself.
	 */
	{ 0x02d024U, 0x00000134U, 0xffffffffU, "SW_0x02d018[p3]" },

	/* 8-9: SW_0x025004, meter index 15 of 16 (id 670, base 0x025004,
	 * width 64, array 0..15 -- each item spans TWO 32-bit words, low word
	 * (k=0, register bits 0-31) at offset+8*item, high word (k=1, bits
	 * 32-63) at offset+8*item+4). Fields on the low word: TYPE (lsp 17
	 * len 1), F_16_1 (lsp 1 len 16), IFG (lsp 0 len 1); on the high
	 * word: RATE (lsp 32 len 19, i.e. bits 0-18 of that word). v6 low
	 * (0x02507c) = 0x1388 -> IFG=0, F_16_1=2500, TYPE=0; v6 high
	 * (0x025080) = 0x27fff -> RATE=163839. s7 low = 0x7ffff -> IFG=1,
	 * F_16_1=65535 (max), TYPE=1; s7 high = 0x7ffe -> RATE=32766.
	 * s7's bit pattern (bits 0-18 all set on the low word) reads like
	 * this register's power-on-reset default, not a deliberately
	 * programmed value -- meter 15, the last of 16 global shared meters,
	 * was plausibly never allocated on s7 at all, the same "OLT never
	 * provisioned us" category as the excluded ACL/CLASSIFY_SETUP entries rather
	 * than a boot-time constant. Included per instruction regardless.
	 */
	{ 0x02507cU, 0x00001388U, 0xffffffffU, "SW_0x025004[15].lo" },
	{ 0x025080U, 0x00027fffU, 0xffffffffU, "SW_0x025004[15].hi" },

	/* 10: PRIO_QUEUE_MAP (id 759, base 0x01c0b0, width 3, array 0..7
	 * x port 0..3, wide packing, i = priority*4+port). Field QUEUE,
	 * lsp 0 len 3: maps one of 8 internal priorities to a queue id, per
	 * port. The diverging word (0x01c0bc, item i=3) decodes to priority
	 * 0, port 3 (CPU). v6/s7's raw values (0xe92488 / 0xfac688) both far
	 * exceed the documented 3-bit field width -- the same "map collision,
	 * field decode incomplete" caveat as entry 2 (this word is also
	 * PORT_QUEUE_MAP's own item, entry 11 below); taken verbatim.
	 */
	{ 0x01c0bcU, 0x00e92488U, 0xffffffffU, "PRIO_QUEUE_MAP[pri0,p3]/PORT_QUEUE_MAP (map collision)" },

	/* 11: PORT_QUEUE_MAP (id 760, base 0x01c0c0, width 2, array 0..0,
	 * port 0..3, i = port directly -- item 0 = port 0, address exactly
	 * the base 0x01c0c0). Field IDX, lsp 0 len 2. This same address is
	 * ALSO PRIO_QUEUE_MAP's own item i=4 (offset 0x01c0c0-0x01c0b0=
	 * 0xc/4=4 words from ITS base): under (priority-0)*4+port=4, the
	 * only valid solution with port in 0..3 is priority=1, port=0 --
	 * port-major ordering would instead need priority=0, port=4, which
	 * is out of the register's own port 0..3 range, impossible. This is
	 * what confirms array-major enumeration ((array-array_min)*port_
	 * count+(port-port_min)) for every wide/multi-dimension register in
	 * this table, entries 1, 2 and 10 included, rather than an assumed
	 * convention. v6 = 0xd4 -- the same literal value
	 * odi_sw_ponmac_queue_add() already writes to this exact register
	 * from the per-command path (this file, th=0xd4 in every boot3/
	 * boot5 instance): this entry does not add new information over
	 * what that command path already writes once real UNI/PON traffic
	 * runs cmd 23 -- it only guarantees the value is present before that
	 * first happens.
	 */
	{ 0x01c0c0U, 0x000000d4U, 0xffffffffU, "PORT_QUEUE_MAP[p0]/PRIO_QUEUE_MAP (map collision)" },

	/* 12: SW_0x017008 (id 594, base 0x017008, width 11, array 0..0,
	 * port 0..3, i = port directly). Field NUM, lsp 0 len 11: the max
	 * number of L2 addresses this port may learn. The diverging word
	 * (0x017014, item i=3) is port 3 (CPU). v6 = 1 -- the CPU port capped
	 * to learning a single MAC (its own management address); s7 =
	 * 0x800 = 2048, near the field's own maximum (effectively
	 * unlimited). Lower risk than the WFQ/MAX_LENGTH candidates above --
	 * an L2 learning-table hygiene setting, not an obvious transmit-path
	 * blocker -- but a real, confirmed divergence.
	 */
	{ 0x017014U, 0x00000001U, 0xffffffffU, "SW_0x017008[p3]" },

	/* 13: VLAN_INGRESS_CHECK (id 613, 0x013004, width 1, array 0..0, port
	 * 0..3, NARROW packing: 4 port bits in ONE word, port p's INGRESS
	 * enable bit at bit p). v6 = 0x0f -- all four ports' ingress filter
	 * ENABLED. This directly reverses odi_switch_init_platform()'s own
	 * item 4, which clears port 1's bit (0x0d) on the strength of a
	 * single OEM register read from a DIFFERENT session -- the v6 A/B capture, taken
	 * from a demonstrably WORKING image at the same boot moment as the
	 * broken s7 capture, shows the working configuration is actually all
	 * four bits set. Mask narrowed to 0x0000000f (the register's whole
	 * meaningful width -- see odi_switch_dal.h's comment on why this is
	 * the one entry that is not a full-word mask): item 4's port-1 write
	 * stays the default behaviour on every boot (independent
	 * bisectability, unchanged), and enabling this entry is what flips
	 * the outcome back to 0x0f without editing item 4 itself.
	 */
	{ ODI_SW_VLAN_INGRESS_CHECK_BASE, 0x0000000fU, 0x0000000fU, "VLAN_INGRESS_CHECK (reverses item 4's port-1 write)" },
};

const struct odi_sw_parity_entry *const odi_switch_parity_table = odi_switch_parity_table_;
const unsigned int odi_switch_parity_table_count =
	(unsigned int)(sizeof(odi_switch_parity_table_) / sizeof(odi_switch_parity_table_[0]));

/* --- odi_switch_parity_add()/_clear() and the loaded table --------------
 *
 * s8 ran every one of the 14
 * compiled-default entries above (mask 0x3fff) and the OLT still stalled
 * -- none of them is the cause. What is left
 * of the v6-vs-s7 diff is the set the compiled table deliberately held
 * back (SW_0x015008/SW_0x015048/ACL_PORT_ENABLE, LUT flood, CLASSIFY_SETUP), which
 * needs a way to try without a rebuild -- this loaded table, fed from a
 * file on the config partition through /proc/odi_omci, is that way.
 */
static struct odi_sw_parity_entry odi_switch_parity_loaded_[ODI_SWITCH_PARITY_MAX_LOADED];
static unsigned int odi_switch_parity_loaded_count_;
static int odi_switch_parity_using_loaded_;

int odi_switch_parity_add(uint32_t offset, uint32_t value, uint32_t mask)
{
	struct odi_sw_parity_entry *e;

	if (!odi_switch_mmio_offset_in_bounds(offset)) {
		ODI_SW_INIT_LOG("parity_add: entry REJECTED, offset 0x%08x is outside the mapped MMIO window\n", offset);
		return -1;
	}
	if (odi_switch_parity_loaded_count_ >= ODI_SWITCH_PARITY_MAX_LOADED) {
		ODI_SW_INIT_LOG("parity_add: entry REJECTED, offset 0x%08x -- the loaded table already holds the maximum %u entries\n",
				offset, ODI_SWITCH_PARITY_MAX_LOADED);
		return -1;
	}

	e = &odi_switch_parity_loaded_[odi_switch_parity_loaded_count_];
	e->offset = offset;
	e->value = value;
	e->mask = mask;
	e->name = "loaded (via /proc parity_add)";
	odi_switch_parity_loaded_count_++;
	odi_switch_parity_using_loaded_ = 1;

	ODI_SW_INIT_LOG("parity_add: entry %u accepted, offset=0x%08x value=0x%08x mask=0x%08x (loaded table now active, %u entries)\n",
			odi_switch_parity_loaded_count_ - 1, offset, value, mask,
			odi_switch_parity_loaded_count_);
	return 0;
}

void odi_switch_parity_clear(void)
{
	unsigned int cleared = odi_switch_parity_loaded_count_;

	odi_switch_parity_loaded_count_ = 0;
	odi_switch_parity_using_loaded_ = 0;
	ODI_SW_INIT_LOG("parity_clear: loaded table cleared (%u entries dropped), the %u compiled default entries are active again\n",
			cleared, odi_switch_parity_table_count);
}

const struct odi_sw_parity_entry *odi_switch_parity_active_table(void)
{
	return odi_switch_parity_using_loaded_ ? odi_switch_parity_loaded_ : odi_switch_parity_table_;
}

unsigned int odi_switch_parity_active_count(void)
{
	return odi_switch_parity_using_loaded_ ? odi_switch_parity_loaded_count_ : odi_switch_parity_table_count;
}

int odi_switch_parity_is_loaded(void)
{
	return odi_switch_parity_using_loaded_;
}

/* Shared by odi_switch_init_parity() and odi_switch_init_parity_all() --
 * one entry's read-modify-write plus its before/after log pair, factored
 * out so the two callers cannot drift.
 */
static void odi_switch_parity_apply_entry_(const struct odi_sw_parity_entry *e, unsigned int idx)
{
	uint32_t before, after, reg;

	before = odi_reg_read(e->offset);
	ODI_SW_INIT_LOG("init_parity: entry %u (%s @ 0x%08x) starting, before=0x%08x\n",
			idx, e->name, e->offset, before);
	reg = (before & ~e->mask) | (e->value & e->mask);
	odi_reg_write(e->offset, reg);
	after = odi_reg_read(e->offset);
	ODI_SW_INIT_LOG("init_parity: entry %u (%s @ 0x%08x) done, after=0x%08x\n",
			idx, e->name, e->offset, after);
}

/* Bit-selectable subset of the active table -- entries 0-31 only. A
 * uint32_t mask cannot name an entry past bit 31, which is exactly what a
 * loaded table over 32 entries (tools/regdump/parity-v6.table has more
 * than that) needs odi_switch_init_parity_all() for instead; this function
 * still checks entries 0-31 of ANY active table, loaded or compiled.
 */
void odi_switch_init_parity(uint32_t mask)
{
	const struct odi_sw_parity_entry *table = odi_switch_parity_active_table();
	unsigned int count = odi_switch_parity_active_count();
	unsigned int i;
	uint32_t defined_mask;

	if (mask == 0) {
		ODI_SW_INIT_LOG("init_parity: called with an empty mask, nothing to do\n");
		return;
	}

	for (i = 0; i < count && i < 32U; i++) {
		if (mask & ODI_SWITCH_INIT_PARITY_ENTRY(i))
			odi_switch_parity_apply_entry_(&table[i], i);
	}

	/* count can be up to ODI_SWITCH_PARITY_MAX_LOADED (64) for a loaded
	 * table -- shifting a uint32_t by 32 or more is undefined behaviour,
	 * so the "every bit above the table" mask is clamped at 32 rather
	 * than computed as 1U << count.
	 */
	defined_mask = (count >= 32U) ? 0xffffffffU : ((1U << count) - 1U);
	if (mask & ~defined_mask)
		ODI_SW_STUB_LOG("init_parity: mask 0x%08x has bits beyond the %u active entries (or past bit 31, which init_parity's mask cannot reach -- use bare \"init_parity\" for a table this size), ignored\n", mask, count);
}

/* Every entry of the active table, whatever its size -- the only way to
 * reach entry 32 and beyond of a loaded table. No mask, no partial
 * selection: this is "run everything currently loaded (or the compiled
 * default, if nothing is loaded)".
 */
void odi_switch_init_parity_all(void)
{
	const struct odi_sw_parity_entry *table = odi_switch_parity_active_table();
	unsigned int count = odi_switch_parity_active_count();
	unsigned int i;

	if (count == 0) {
		ODI_SW_INIT_LOG("init_parity: active table is empty, nothing to do\n");
		return;
	}

	for (i = 0; i < count; i++)
		odi_switch_parity_apply_entry_(&table[i], i);
}

/* odi_switch_parity_init_rc/_trigger()/_trigger_all() -- same -1/0 "did it
 * fire" bookkeeping as odi_switch_platform_init_rc above, for the loadable
 * parity mechanism. Static here for the same reason: this is pure
 * dal-leaf logic with no kernel dependency, so it belongs in the file that
 * already owns odi_switch_init_parity()/_all(), not in the __KERNEL__-only
 * odi_switch.c -- odi_omci.c reaches it through odi_switch_api.h, no
 * EXPORT_SYMBOL needed since every CONFIG_ODI_* symbol is built in.
 */
static int odi_switch_parity_init_rc = -1;

int odi_switch_parity_init_rc_get(void)
{
	return odi_switch_parity_init_rc;
}

void odi_switch_parity_init_trigger(uint32_t mask)
{
	if (mask == 0)
		return;
	odi_switch_init_parity(mask);
	odi_switch_parity_init_rc = 0;
}

void odi_switch_parity_init_trigger_all(void)
{
	odi_switch_init_parity_all();
	odi_switch_parity_init_rc = 0;
}

/* --- odi_switch_init_modload() -------------------------------------------
 *
 * modload.bin (odi_replay_blob.h, GENERATED by tools/regtrace/mkmodload.py
 * -- see odi_switch_dal.h's comment on this function for the
 * category/reg_group rules and why order is preserved rather than grouped)
 * is one flat, ordered table covering every register and table-row write
 * the stock OMCI kernel modules make at load time; this function walks it
 * once, applying an entry only when mask selects it.
 */
static int odi_switch_modload_reg_selected(uint32_t mask, const struct odi_sw_modload_event *e,
					     uint32_t *out_value)
{
	if (mask & ODI_SWITCH_INIT_MODLOAD_ITEM(0)) {
		/* Bit 0 dominates: every category-0 register, verbatim,
		 * regardless of reg_group -- the pre-split behaviour,
		 * unchanged (odi_switch_dal.h's own comment on why
		 * ODI_SWITCH_INIT_MODLOAD_ITEM_ALL is unaffected by this
		 * split).
		 */
		*out_value = e->value;
		return 1;
	}

	if (e->reg_group == 0) {
		/* FLOOD family (FLOOD_BCAST_PORTS/FLOOD_UNKN_MCAST_PORTS/LUT_UNKN_UC_
		 * FLOOD, 0x1c020/0x1c024/0x1c028) -- the CPU-forced modifier
		 * bit takes priority over FLOOD's own bit when both are set,
		 * so a trial only has to flip one bit either way.
		 */
		if (mask & (1U << ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_BIT)) {
			*out_value = ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_VALUE;
			return 1;
		}
		if (mask & (1U << ODI_SWITCH_INIT_MODLOAD_REGGROUP_BIT(0))) {
			*out_value = e->value;
			return 1;
		}
		return 0;
	}

	if (mask & (1U << ODI_SWITCH_INIT_MODLOAD_REGGROUP_BIT(e->reg_group))) {
		*out_value = e->value;
		return 1;
	}
	return 0;
}

void odi_switch_init_modload(const struct odi_replay_blob *table, uint32_t mask)
{
	struct odi_sw_modload_event ev;
	const struct odi_sw_modload_event *e = &ev;
	uint32_t i, applied = 0;
	uint8_t verb;

	if (mask == 0) {
		ODI_SW_INIT_LOG("init_modload: called with an empty mask, nothing to do\n");
		return;
	}

	ODI_SW_INIT_LOG("init_modload: starting, mask=0x%08x, %u events in the table\n",
			mask, table->count);

	for (i = 0; i < table->count; i++) {
		uint32_t reg, value;

		odi_replay_blob_switch_event(table, i, &ev, &verb);

		if (e->kind == ODI_SW_MODLOAD_REG) {
			if (!odi_switch_modload_reg_selected(mask, e, &value))
				continue;
			/* Read-modify-write with a full mask, per instruction --
			 * arithmetically the same result as a blind write (the
			 * mask covers every bit), kept as a real read+write so
			 * the RMW shape matches every other trigger this
			 * codebase has (platform init, parity) rather than being
			 * a one-off exception.
			 */
			reg = odi_reg_read(e->offset);
			reg = (reg & 0U) | value;
			odi_reg_write(e->offset, reg);
		} else {
			if (!(mask & ODI_SWITCH_INIT_MODLOAD_ITEM(e->category)))
				continue;
			(void)odi_switch_table_write(e->table, e->offset, e->words, e->n_words);
		}
		applied++;
	}

	ODI_SW_INIT_LOG("init_modload: done, mask=0x%08x, %u of %u events applied\n",
			mask, applied, table->count);
}

/* --- odi_switch_ds_encrypt() ----------------------------------------------
 *
 * odi_switch_dal.h has the full
 * derivation (the Encrypted_Port-ID PLOAM chain, why it is alive in
 * vmlinux but never fires for our own GEM ports, and why this is the
 * minimal fix rather than restoring the stock stack flow bookkeeping).
 * Read-modify-write bit 4 (EN_AES) of DSF_GEM_FLOW_TYPE(idx) for
 * each GEM port slot mask names, leaving every other bit of that word
 * (multicast/Ethernet/OMCI) exactly as odi_sw_gpon_usflow_set() (or
 * whatever else last wrote it) left it.
 */
/* The RMW itself, for a caller holding odi_switch_dsf_lock: FLAGS before
 * and after go back to the caller, which logs them once the lock is
 * released (nothing prints under it, odi_switch.c).
 */
static void __odi_switch_ds_encrypt_one(uint32_t idx, uint32_t enable,
					uint32_t *before, uint32_t *after)
{
	uint32_t reg;

	lockdep_assert_held(&odi_switch_dsf_lock);
	reg = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(idx));
	*before = ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_GET(reg);
	reg = ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_DECRYPT_SET(reg, enable ? 1U : 0U);
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(idx), reg);
	reg = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(idx));
	*after = ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_GET(reg);
}

static void odi_switch_ds_encrypt_log(uint32_t idx, uint32_t before, uint32_t after)
{
	ODI_SW_INIT_LOG("ds_encrypt: GEM port slot %u, FLAGS=0x%02x before\n", idx, before);
	ODI_SW_INIT_LOG("ds_encrypt: GEM port slot %u, FLAGS=0x%02x after\n", idx, after);
}

void odi_switch_ds_encrypt_one(uint32_t idx, uint32_t enable)
{
	unsigned long flags;
	uint32_t before, after;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	__odi_switch_ds_encrypt_one(idx, enable, &before, &after);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	odi_switch_ds_encrypt_log(idx, before, after);
}

void odi_switch_ds_encrypt(uint32_t mask)
{
	unsigned int idx;

	if (mask == 0) {
		ODI_SW_INIT_LOG("ds_encrypt: called with an empty mask, nothing to do\n");
		return;
	}

	ODI_SW_INIT_LOG("ds_encrypt: starting, mask=0x%08x\n", mask);

	for (idx = 0; idx < ODI_SWITCH_DS_SLOT_COUNT; idx++) {
		if (!(mask & (1U << idx)))
			continue;
		odi_switch_ds_encrypt_one(idx, 1U);
	}

	ODI_SW_INIT_LOG("ds_encrypt: done, mask=0x%08x\n", mask);
}

/* odi_switch_ds_encrypt_rc/_trigger() -- same -1/0 bookkeeping as the two
 * triggers above, for odi_switch_ds_encrypt() ("DS GEM encryption flag").
 */
static int odi_switch_ds_encrypt_rc = -1;

int odi_switch_ds_encrypt_rc_get(void)
{
	return odi_switch_ds_encrypt_rc;
}

void odi_switch_ds_encrypt_trigger(uint32_t mask)
{
	if (mask == 0)
		return;
	odi_switch_ds_encrypt(mask);
	odi_switch_ds_encrypt_rc = 0;
}

/* --- DS GEM port slot bookkeeping (odi_switch_dal.h has the derivation) --- */

/* Written from process context (cmd 25) and read from the GPON interrupt
 * path: odi_switch_dsf_lock (odi_switch.c) covers both arrays. The __
 * variants are for a caller that already holds it.
 */
static uint32_t odi_sw_ds_slot_gem_port_id[ODI_SWITCH_DS_SLOT_COUNT];
static uint32_t odi_sw_ds_slot_valid_mask;

static void __odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id)
{
	lockdep_assert_held(&odi_switch_dsf_lock);
	if (idx >= ODI_SWITCH_DS_SLOT_COUNT)
		return;
	odi_sw_ds_slot_gem_port_id[idx] = gem_port_id;
	odi_sw_ds_slot_valid_mask |= (1U << idx);
}

static int __odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out)
{
	uint32_t idx;

	lockdep_assert_held(&odi_switch_dsf_lock);
	for (idx = 0; idx < ODI_SWITCH_DS_SLOT_COUNT; idx++) {
		if (!(odi_sw_ds_slot_valid_mask & (1U << idx)))
			continue;
		if (odi_sw_ds_slot_gem_port_id[idx] == gem_port_id) {
			if (idx_out)
				*idx_out = idx;
			return 1;
		}
	}
	return 0;
}

void odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	__odi_switch_ds_slot_record(idx, gem_port_id);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
}

int odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out)
{
	unsigned long flags;
	int found;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	found = __odi_switch_ds_slot_find(gem_port_id, idx_out);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	return found;
}

void odi_switch_ds_slot_reset(void)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	odi_sw_ds_slot_valid_mask = 0;
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
}

/* Slot lookup and the AES-bit RMW as one step under odi_switch_dsf_lock,
 * so cmd 25 cannot move the slot between the two. Returns 1 and the slot
 * with FLAGS before and after when gem_port_id has a slot, 0 otherwise.
 */
static int odi_switch_ds_encrypt_gem_port(uint32_t gem_port_id, uint32_t enable,
					  uint32_t *idx, uint32_t *before, uint32_t *after)
{
	unsigned long flags;
	int found;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	found = __odi_switch_ds_slot_find(gem_port_id, idx);
	if (found)
		__odi_switch_ds_encrypt_one(*idx, enable, before, after);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	return found;
}

/* --- Automatic AES enable: the PLOAM hook (odi_switch_dal.h has the full
 * derivation, and why (b)/(c) were rejected in favour of this). Pure
 * decode-and-dispatch logic, no kernel dependency -- host-testable exactly
 * like odi_switch_ds_encrypt() above; only the registration call below
 * needs the stock GPON kernel module.
 */
int odi_switch_gpon_ploam_hook(struct odi_sw_gpon_ploam *ploam)
{
	uint32_t gem_port_id, ae_en, idx, before, after;

	if (!ploam || ploam->msg_id != ODI_SW_GPON_PLOAM_DS_ENCRYPTPORT)
		return ODI_SW_GPON_PLOAM_CONTINUE;

	/* Encrypted_Port-ID PLOAM (ITU-T G.984.3): the 12-bit GEM port id is
	 * (data[1]<<4)|(data[2]>>4), the encrypt flag is data[0] bit 0.
	 */
	gem_port_id = ((uint32_t)ploam->data[1] << 4) | ((uint32_t)ploam->data[2] >> 4);
	ae_en = (uint32_t)ploam->data[0] & 0x01U;

	if (!odi_switch_ds_encrypt_gem_port(gem_port_id, ae_en, &idx, &before, &after)) {
		ODI_SW_INIT_LOG("gpon ploam: Encrypted_Port-ID for gem_port_id 0x%03x, no DS slot recorded for it yet -- ignored\n",
				gem_port_id);
		return ODI_SW_GPON_PLOAM_CONTINUE;
	}

	ODI_SW_INIT_LOG("gpon ploam: Encrypted_Port-ID gem_port_id 0x%03x -> DS slot %u, aes=%u\n", gem_port_id, idx, ae_en);
	odi_switch_ds_encrypt_log(idx, before, after);

	/* Observe only -- the stock GPON module's own handling of this PLOAM (still
	 * inert for us since it never finds a flow in its own bookkeeping, and
	 * its PLOAM ack) must still run exactly as if this hook did not exist.
	 */
	return ODI_SW_GPON_PLOAM_CONTINUE;
}

/* odi_switch_gpon_encrypt_port() -- CONFIG_ODI_GPON's own replacement for
 * odi_switch_gpon_ploam_hook() above: with the stock GPON module gone,
 * nothing registers the PLOAM callback any more, so the Encrypted-
 * Port-ID PLOAM never reaches this file through that callback. Instead the
 * FSM/PLOAM core decodes it and calls this function directly with the
 * gem_port_id and the aes bit already pulled out. Same slot lookup and
 * same odi_switch_ds_encrypt_one() call as the hook; a gem_port_id the
 * table has not seen yet is logged and left alone, same as the hook.
 * Returns 0 on success, -1 if no DS slot was recorded for gem_port_id yet.
 */
int odi_switch_gpon_encrypt_port(uint16_t gem_port_id, int enable)
{
	uint32_t idx, before, after;

	if (!odi_switch_ds_encrypt_gem_port((uint32_t)gem_port_id, enable ? 1U : 0U,
					    &idx, &before, &after)) {
		ODI_SW_INIT_LOG("gpon encrypt_port: gem_port_id 0x%03x, no DS slot recorded for it yet -- ignored\n",
				gem_port_id);
		return -1;
	}

	ODI_SW_INIT_LOG("gpon encrypt_port: gem_port_id 0x%03x -> DS slot %u, aes=%u\n", gem_port_id, idx, enable ? 1U : 0U);
	odi_switch_ds_encrypt_log(idx, before, after);
	return 0;
}


/* ---- Phase 3f: register/MIB leaves behind /dev/odi_sw (odi_reg.c) ------ */

int odi_sw_reg_get(uint32_t addr, uint32_t *value)
{
	if (!odi_switch_mmio_offset_in_bounds(addr) || (addr & 3U))
		return -EINVAL;
	*value = odi_reg_read(addr);
	return 0;
}

int odi_sw_reg_set(uint32_t addr, uint32_t value)
{
	if (!odi_switch_mmio_offset_in_bounds(addr) || (addr & 3U))
		return -EINVAL;
	odi_reg_write(addr, value);
	return 0;
}

/* odi_sw_mib_get(): counter is src/diag/src/mib.h's mib_names[] index
 * (0-68), the stock diag counter order, restated here as three
 * mapping tables rather than duplicated by name -- one per register
 * block (PORT_TX_COUNTERS, PORT_RX_COUNTERS, PORT_OAM_COUNTERS,
 * odi_switch_hw.h), each entry giving the block word this counter reads.
 * ODI_SW_MIB_WIDE marks the two 64-bit counters (ifInOctets/ifOutOctets):
 * the register table splits each into two consecutive 32-bit words,
 * low word first (PORT_TX_COUNTERS rows 10/11 for ifOutOctets, RX rows
 * 0/1 for ifInOctets -- the table lists the two halves in that order:
 * the low half at the lower address, the high half right after it.
 * Confirmed against a live counter that had not yet overflowed 32 bits
 * (isp1, f5c: ifInOctets read back as high-word-nonzero/low-word-zero
 * with the two reversed) -- an earlier version of this comment had the
 * halves backwards and so did the read below, which is the word-swap bug
 * the mib_get wide-counter checks in test/odi_reg_test.c now pin.
 *
 * Left unmapped, deliberately: every mib.h index with no register row
 * that names it as clearly as the ones below, chiefly the stock diag
 * duplicate entries -- an index-only-by-direction counter
 * (etherStatsTx.../etherStatsRx...) next to a same-sounding
 * direction-less one (etherStats...) that
 * there is no register evidence to tell apart (etherStatsOctets,
 * etherStatsBroadcastPkts/MulticastPkts, etherStatsUndersizePkts/
 * OversizePkts, etherStatsPkts64..1024Octets, etherStatsTxFragments/
 * TxJabbers/TxCRCAlignErrors, etherStatsRxUndersizeDropPkts), plus four
 * counters (dot1dPortDelayExceedDiscards, dot1dTpHcPortInDiscards,
 * dot3StatsAlignmentErrors, dot3StatsFrameTooLongs, dot3OutPauseOnFrames)
 * no PORT_*_COUNTERS row names at all. Unmapped is simply refused; there
 * is no sockopt to fall back to.
 */
#define ODI_SW_MIB_TX	0
#define ODI_SW_MIB_RX	1
#define ODI_SW_MIB_OAM	2
#define ODI_SW_MIB_WIDE	0x80U	/* combine this row and the next as one
				 * 64-bit value, high word first
				 */

struct odi_sw_mib_map {
	uint8_t counter;	/* mib.h mib_names[] index */
	uint8_t block;		/* ODI_SW_MIB_TX/_RX/_OAM */
	uint8_t row;		/* word offset in the block, ODI_SW_MIB_WIDE set for a pair */
};

static const struct odi_sw_mib_map odi_sw_mib_map[] = {
	/* PORT_TX_COUNTERS */
	{ 53, ODI_SW_MIB_TX, 0 },	/* etherStatsTxMulticastPkts */
	{ 52, ODI_SW_MIB_TX, 1 },	/* etherStatsTxBroadcastPkts */
	{ 43, ODI_SW_MIB_TX, 2 },	/* etherStatsTxUndersizePkts */
	{ 44, ODI_SW_MIB_TX, 3 },	/* etherStatsTxOversizePkts */
	{ 45, ODI_SW_MIB_TX, 4 },	/* etherStatsTxPkts64Octets */
	{ 46, ODI_SW_MIB_TX, 5 },	/* etherStatsTxPkts65to127Octets */
	{ 47, ODI_SW_MIB_TX, 6 },	/* etherStatsTxPkts128to255Octets */
	{ 48, ODI_SW_MIB_TX, 7 },	/* etherStatsTxPkts256to511Octets */
	{ 49, ODI_SW_MIB_TX, 8 },	/* etherStatsTxPkts512to1023Octets */
	{ 50, ODI_SW_MIB_TX, 9 },	/* etherStatsTxPkts1024to1518Octets */
	{ 5,  ODI_SW_MIB_TX, 10 | ODI_SW_MIB_WIDE },	/* ifOutOctets */
	{ 42, ODI_SW_MIB_TX, 10 | ODI_SW_MIB_WIDE },	/* etherStatsTxOctets, same counter */
	{ 18, ODI_SW_MIB_TX, 12 },	/* dot3StatsSingleCollisionFrames */
	{ 19, ODI_SW_MIB_TX, 13 },	/* dot3StatsMultipleCollisionFrames */
	{ 20, ODI_SW_MIB_TX, 14 },	/* dot3StatsDeferredTransmissions */
	{ 21, ODI_SW_MIB_TX, 15 },	/* dot3StatsLateCollisions */
	{ 34, ODI_SW_MIB_TX, 16 },	/* etherStatsCollisions */
	{ 22, ODI_SW_MIB_TX, 17 },	/* dot3StatsExcessiveCollisions */
	{ 14, ODI_SW_MIB_TX, 18 },	/* dot3OutPauseFrames */
	{ 6,  ODI_SW_MIB_TX, 19 },	/* ifOutDiscards */
	{ 51, ODI_SW_MIB_TX, 20 },	/* etherStatsTxPkts1519toMaxOctets */
	{ 11, ODI_SW_MIB_TX, 22 },	/* dot1dTpPortInDiscards */
	{ 7,  ODI_SW_MIB_TX, 23 },	/* ifOutUcastPkts */
	{ 8,  ODI_SW_MIB_TX, 24 },	/* ifOutMulticastPkts */
	{ 9,  ODI_SW_MIB_TX, 25 },	/* ifOutBroadcastPkts */
	/* PORT_RX_COUNTERS */
	{ 0,  ODI_SW_MIB_RX, 0 | ODI_SW_MIB_WIDE },	/* ifInOctets */
	{ 35, ODI_SW_MIB_RX, 2 },	/* etherStatsCRCAlignErrors */
	{ 24, ODI_SW_MIB_RX, 3 },	/* dot3StatsSymbolErrors */
	{ 13, ODI_SW_MIB_RX, 4 },	/* dot3InPauseFrames */
	{ 25, ODI_SW_MIB_RX, 5 },	/* dot3ControlInUnknownOpcodes */
	{ 32, ODI_SW_MIB_RX, 6 },	/* etherStatsFragments */
	{ 33, ODI_SW_MIB_RX, 7 },	/* etherStatsJabbers */
	{ 1,  ODI_SW_MIB_RX, 8 },	/* ifInUcastPkts */
	{ 26, ODI_SW_MIB_RX, 9 },	/* etherStatsDropEvents */
	{ 2,  ODI_SW_MIB_RX, 10 },	/* ifInMulticastPkts */
	{ 3,  ODI_SW_MIB_RX, 11 },	/* ifInBroadcastPkts */
	{ 66, ODI_SW_MIB_RX, 12 },	/* etherStatsRxPkts1519toMaxOctets */
	{ 57, ODI_SW_MIB_RX, 14 },	/* etherStatsRxUndersizePkts */
	{ 59, ODI_SW_MIB_RX, 15 },	/* etherStatsRxOversizePkts */
	{ 60, ODI_SW_MIB_RX, 16 },	/* etherStatsRxPkts64Octets */
	{ 61, ODI_SW_MIB_RX, 17 },	/* etherStatsRxPkts65to127Octets */
	{ 62, ODI_SW_MIB_RX, 18 },	/* etherStatsRxPkts128to255Octets */
	{ 63, ODI_SW_MIB_RX, 19 },	/* etherStatsRxPkts256to511Octets */
	{ 64, ODI_SW_MIB_RX, 20 },	/* etherStatsRxPkts512to1023Octets */
	{ 65, ODI_SW_MIB_RX, 21 },	/* etherStatsRxPkts1024to1518Octets */
	/* PORT_OAM_COUNTERS */
	{ 68, ODI_SW_MIB_OAM, 0 },	/* outOamPduPkts */
	{ 67, ODI_SW_MIB_OAM, 1 },	/* inOamPduPkts */
};

int odi_sw_mib_get(uint32_t port, uint32_t counter, uint64_t *value)
{
	unsigned int i;

	if (port >= 4U)
		return -1;
	for (i = 0; i < sizeof odi_sw_mib_map / sizeof odi_sw_mib_map[0]; i++) {
		const struct odi_sw_mib_map *m = &odi_sw_mib_map[i];
		uint8_t row = m->row & ~ODI_SW_MIB_WIDE;
		uint32_t addr;

		if (m->counter != counter)
			continue;
		switch (m->block) {
		case ODI_SW_MIB_TX:
			addr = ODI_SW_PORT_TX_COUNTERS(port, row);
			break;
		case ODI_SW_MIB_RX:
			addr = ODI_SW_PORT_RX_COUNTERS(port, row);
			break;
		default:
			addr = ODI_SW_PORT_OAM_COUNTERS(port, row);
			break;
		}
		if (m->row & ODI_SW_MIB_WIDE) {
			uint32_t lo = odi_reg_read(addr);
			uint32_t hi = odi_reg_read(addr + 4U);

			*value = ((uint64_t)hi << 32) | lo;
		} else {
			*value = odi_reg_read(addr);
		}
		return 0;
	}
	return -1;
}
