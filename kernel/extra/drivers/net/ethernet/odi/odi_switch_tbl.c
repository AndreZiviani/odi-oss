// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_tbl.c -- implementation of the table primitives declared in
 * odi_switch_tbl.h. See that header for the per-primitive trace evidence.
 *
 * odi_reg_read/odi_reg_write come from odi_switch_mock.h on the host build
 * (test/odi_switch_test.c includes it before this file) and from
 * odi_switch_reg.h's __KERNEL__ declarations (defined in odi_switch.c) on
 * the target build.
 */
#include "odi_switch_tbl.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/printk.h>
#include <linux/ratelimit.h>
#endif

#ifdef __KERNEL__
static int odi_switch_poll(uint32_t off, uint32_t mask, unsigned int max_spins)
{
	unsigned int i;

	for (i = 0; i < max_spins; i++) {
		if ((odi_reg_read(off) & mask) == mask)
			return 0;
	}
	return -1;
}

/* Waits for mask's bits to CLEAR rather than become set -- odi_switch_
 * table_write()/_read() need this (TABLE_STATUS.IN_PROGRESS, 1 while busy,
 * 0 when done), and odi_switch_poll() only supports the opposite
 * direction. Kept separate rather than adding an invert flag to
 * odi_switch_poll() itself: the two GPON handshakes (odi_switch_tbl.h)
 * already call odi_switch_poll() with its existing "wait for set"
 * semantics and are unaffected, so there is nothing to migrate.
 */
static int odi_switch_poll_clear(uint32_t off, uint32_t mask, unsigned int max_spins)
{
	unsigned int i;

	for (i = 0; i < max_spins; i++) {
		if ((odi_reg_read(off) & mask) == 0)
			return 0;
	}
	return -1;
}
#else
/* Host build: the mock has no hardware state machine to bring the done bit
 * up over time, so a real spin here would just time out. odi_switch_poll()
 * stands in for "the operation is already complete by the time we check",
 * which is what a real, working handshake looks like from the point of
 * view of the caller -- the point of the host test is the write sequence, not timing.
 */
static int odi_switch_poll(uint32_t off, uint32_t mask, unsigned int max_spins)
{
	(void)off;
	(void)mask;
	(void)max_spins;
	return 0;
}

static int odi_switch_poll_clear(uint32_t off, uint32_t mask, unsigned int max_spins)
{
	(void)off;
	(void)mask;
	(void)max_spins;
	return 0;
}
#endif

/* A timed-out table poll (odi_switch_table_write()/_read() below) names
 * the table and row it was on, rate-limited so a caller looping over many
 * rows (e.g. odi_switch_init_platform()'s item 6 sweep) cannot flood
 * dmesg the same way the s4 boot hang this replaces could not be seen at
 * all. Host build: no-op, matching
 * odi_switch_poll_clear()'s own always-succeeds host stance above.
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

	rc = odi_switch_poll(ODI_SW_DSF_GEM_CAM_CTL_OFF,
			      1U << 14 /* DONE */, ODI_SWITCH_TBL_MAX_SPINS);

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

	rc = odi_switch_poll(ODI_SW_DSF_ALLOC_CAM_CTL_OFF,
			      1U << 14 /* DONE */, ODI_SWITCH_TBL_MAX_SPINS);
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

/* Table descriptors, L2 funnel only (odi_switch_hw.h has the full
 * derivation and the funnel membership list). Indexed by enum
 * odi_sw_table; an entry with datareg_num == 0 is not in this array on
 * purpose -- no real table has zero data words, so that is the sentinel
 * for "not a table resolved as part of the L2 funnel" and
 * odi_switch_table_write()/_read() refuse it rather than guess a TABLE_KIND.
 */
static const struct odi_sw_table_desc odi_sw_table_desc[ODI_SW_TBL_NAT_PARSER_SNAP + 1] = {
	ODI_SW_TABLE_DESC_INIT
};

/* The L2-funnel handshake:
 *   1. write TABLE_WRITE_WORD[0..n-1], WORD ORDER REVERSED -- the
 *      captured stock table writes put the caller's LAST data word in
 *      WR_DATA[0] and the caller's FIRST in WR_DATA[n-1]. Confirmed
 *      against the captured rows, not guessed;
 *      getting this backwards on real hardware writes a row with its
 *      words swapped, not a row to the wrong table -- still wrong, still
 *      worth getting right.
 *   2. read-modify-write TABLE_CMD: ADDR = index + descriptor's
 *      addr_offset (CF_RULE_48_* rows are offset by the table's own
 *      size, ACL_PATTERN by a flat 128 -- neither of our five other tables
 *      has an offset), METHOD = 1, IS_WRITE = 1/0 for write/read,
 *      TABLE_KIND = descriptor's type (NOT table & 0x7 -- that placeholder
 *      is gone; TABLE_KIND is a small hardware id unrelated to our enum's
 *      numbering, e.g. VLAN is type 1 while CLS_RULE_B is type 4).
 *   3. poll TABLE_STATUS.IN_PROGRESS.
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

	/* Wait for TABLE_STATUS.IN_PROGRESS (bit 13) to clear, bounded by
	 * ODI_SWITCH_TBL_MAX_SPINS (odi_switch_tbl.h, 1000). Previously
	 * called with mask=0 through odi_switch_poll() (which waits for bits
	 * to become SET) -- "(read & 0) == 0" is trivially true on the very
	 * first read, so that call always reported success immediately and
	 * never actually checked IN_PROGRESS, on host or target. This was not
	 * the s4 boot hang's cause (a poll that returns instantly cannot
	 * spin), but it also means no evidence exists either way that this
	 * handshake completes in bounded time on real hardware; fixed to
	 * check the real bit now that it is finally exercised, and to log
	 * (rate-limited) if the cap is ever hit instead of returning a
	 * silent -1.
	 */
	rc = odi_switch_poll_clear(ODI_SW_TABLE_STATUS_OFF, 1U << 13 /* IN_PROGRESS */,
				    ODI_SWITCH_TBL_MAX_SPINS);
	if (rc)
		ODI_SW_TBL_POLL_TIMEOUT_LOG("write", table, index);

	ODI_SW_TABLE_TRACE('T', table, index, data, n_words);

	return rc;
}
