// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_tbl.c -- the table handshakes of odi_switch_tbl.h. The
 * register accessors are those of odi_switch_reg.h in the kernel and of
 * test/odi_switch_mock.h in the host unity build.
 */
#include "odi_switch_tbl.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/printk.h>
#include <linux/ratelimit.h>
#endif

/* A table poll that timed out names the table and row, rate-limited
 * because a caller may loop over many rows (the platform CF sweep).
 */
#ifdef __KERNEL__
#define ODI_SW_TBL_POLL_TIMEOUT_LOG(op, table, index) \
	do { \
		static DEFINE_RATELIMIT_STATE(_rl, 5 * HZ, 3); \
		if (__ratelimit(&_rl)) \
			pr_err("odi_switch: table %s timed out, table %u index %u\n", \
			       (op), (unsigned int)(table), (unsigned int)(index)); \
	} while (0)
#else
#define ODI_SW_TBL_POLL_TIMEOUT_LOG(op, table, index) do { } while (0)
#endif

int __odi_switch_gpon_ds_port_write(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg)
{
	uint32_t ind = 0;
	int rc;

	lockdep_assert_held(&odi_switch_dsf_lock);

	ind = ODI_SW_DSF_GEM_CAM_CTL_OP_SET(ind, 1); /* write */
	ind = ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_SET(ind, idx);
	odi_reg_write(ODI_SW_DSF_GEM_CAM_CTL_OFF, ind);

	odi_reg_write(ODI_SW_DSF_GEM_CAM_WDATA_OFF,
		      ODI_SW_DSF_GEM_CAM_WDATA_GEM_PORT_SET(0, gem_port_id));

	ind = ODI_SW_DSF_GEM_CAM_CTL_REQ_SET(ind, 1);
	odi_reg_write(ODI_SW_DSF_GEM_CAM_CTL_OFF, ind);

	rc = odi_poll_reg(ODI_SW_DSF_GEM_CAM_CTL_OFF, ODI_SW_DSF_GEM_CAM_CTL_DONE,
			  ODI_SW_DSF_GEM_CAM_CTL_DONE, ODI_SW_TABLE_POLL_DELAY_US,
			  ODI_SW_TABLE_POLL_US, NULL);

	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(idx),
		      ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_SET(0, traffic_cfg));

	return rc;
}

int odi_switch_gpon_ds_port_write(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	rc = __odi_switch_gpon_ds_port_write(idx, gem_port_id, traffic_cfg);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	return rc;
}

int odi_switch_gpon_alloc_write(uint32_t idx, uint32_t alloc_id)
{
	unsigned long flags;
	uint32_t ind = 0;
	int rc;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	ind = ODI_SW_DSF_ALLOC_CAM_CTL_OP_SET(ind, 1); /* write */
	ind = ODI_SW_DSF_ALLOC_CAM_CTL_CAM_ROW_SET(ind, idx);
	odi_reg_write(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, ind);

	odi_reg_write(ODI_SW_DSF_ALLOC_CAM_WDATA_OFF,
		      ODI_SW_DSF_ALLOC_CAM_WDATA_ALLOC_ID_SET(0, alloc_id));

	ind = ODI_SW_DSF_ALLOC_CAM_CTL_REQ_SET(ind, 1);
	odi_reg_write(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, ind);

	rc = odi_poll_reg(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, ODI_SW_DSF_ALLOC_CAM_CTL_DONE,
			  ODI_SW_DSF_ALLOC_CAM_CTL_DONE, ODI_SW_TABLE_POLL_DELAY_US,
			  ODI_SW_TABLE_POLL_US, NULL);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);

	return rc;
}

void odi_switch_vlan_egress_tag_group_write(uint32_t ingress_mask, const uint32_t tag[4])
{
	unsigned int port;

	for (port = 0; port < 4; port++) {
		odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK_BASE, ingress_mask);
		odi_reg_write(ODI_SW_PORT_EGRESS_TAG_MODE(port), tag[port]);
	}
}

/* Indexed by enum odi_sw_table. A table with no entry has datareg_num 0,
 * and odi_switch_table_write() refuses it rather than guess a TABLE_KIND.
 */
static const struct odi_sw_table_desc odi_sw_table_desc[ODI_SW_TBL_NAT_PARSER_SNAP + 1] = {
	ODI_SW_TABLE_DESC_INIT
};

/* odi_switch_tbl.h has the handshake. The word order is reversed onto the
 * wire as the captured stock writes do; the other way round would write
 * the row with its words swapped.
 */
int odi_switch_table_write(uint32_t table, uint32_t index, const uint32_t *data, uint32_t n_words)
{
	const struct odi_sw_table_desc *desc;
	uint32_t ctrl;
	uint32_t i;
	int rc;

	if (table >= sizeof(odi_sw_table_desc) / sizeof(odi_sw_table_desc[0]))
		return ODI_SW_EOPNOTSUPP;
	desc = &odi_sw_table_desc[table];
	if (desc->datareg_num == 0 || n_words != desc->datareg_num)
		return ODI_SW_EOPNOTSUPP; /* unresolved table, or caller's word count is wrong */

	/* The data words, TABLE_CMD and the poll are one handshake on one
	 * shared engine: odi_switch_lock (odi_switch.c) keeps a second
	 * process-context caller out between them.
	 */
	lockdep_assert_held(&odi_switch_lock);

	for (i = 0; i < n_words; i++)
		odi_reg_write(ODI_SW_TABLE_WRITE_WORD(i), data[n_words - 1U - i]);

	ctrl = odi_reg_read(ODI_SW_TABLE_CMD_OFF);
	ctrl = ODI_SW_TABLE_CMD_ROW_SET(ctrl, index + desc->addr_offset);
	ctrl = ODI_SW_TABLE_CMD_METHOD_SET(ctrl, 1);
	ctrl = ODI_SW_TABLE_CMD_IS_WRITE_SET(ctrl, 1); /* write */
	ctrl = ODI_SW_TABLE_CMD_TABLE_KIND_SET(ctrl, desc->type);
	ctrl = ODI_SW_TABLE_CMD_START_SET(ctrl, 1); /* fire */
	odi_reg_write(ODI_SW_TABLE_CMD_OFF, ctrl);

	/* IN_PROGRESS clears when the row is written; a timeout is logged. */
	rc = odi_poll_reg(ODI_SW_TABLE_STATUS_OFF, ODI_SW_TABLE_STATUS_IN_PROGRESS, 0,
			  ODI_SW_TABLE_POLL_DELAY_US, ODI_SW_TABLE_POLL_US, NULL);
	if (rc)
		ODI_SW_TBL_POLL_TIMEOUT_LOG("write", table, index);

	ODI_SW_TABLE_TRACE('T', table, index, data, n_words);

	return rc;
}
