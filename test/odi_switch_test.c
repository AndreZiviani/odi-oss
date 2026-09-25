/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_test.c -- host-side unit tests for odi_switch_hw.h (field
 * pack/unpack sanity) and the table primitives in odi_switch_tbl.c.
 * Compiled and run with the host cc, no kernel and no target toolchain
 * needed, same as odi_nic_hw_test.c.
 *
 * The expected write sequence for each primitive is the literal sequence
 * the boot3 capture shows for it, by line number, except
 * odi_switch_gpon_alloc_write() (see its comment): no AssignedAllocId ran
 * during that capture, so its expected sequence is built by analogy with
 * the port-table primitive own sequence, not recovered from a trace.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

struct expect_write {
	uint32_t addr;
	uint32_t val;
};

static int mock_log_matches(const struct expect_write *want, unsigned int n)
{
	unsigned int i;

	if (odi_mock.log_n != n)
		return 0;
	for (i = 0; i < n; i++) {
		if (odi_mock.log[i].kind != 'W')
			return 0;
		if (odi_mock.log[i].addr != want[i].addr || odi_mock.log[i].val != want[i].val)
			return 0;
	}
	return 1;
}

/* --- odi_switch_hw.h field pack/unpack sanity ------------------------- */

static void test_hw_field_roundtrip(void)
{
	uint32_t reg = 0;

	/* DSF_GEM_CAM_CTL: the exact word boot3 writes for idx=4,
	 * mode=write (1), req=0 -- 0x00000104.
	 */
	reg = ODI_SW_DSF_GEM_CAM_CTL_OP_SET(reg, 1);
	reg = ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_SET(reg, 4);
	CHECK(reg == 0x00000104U, "DSF_GEM_CAM_CTL pack matches boot3 (idx=4, mode=write)");
	CHECK(ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_GET(reg) == 4, "IDX unpack");
	CHECK(ODI_SW_DSF_GEM_CAM_CTL_OP_GET(reg) == 1, "MODE unpack");

	reg = ODI_SW_DSF_GEM_CAM_CTL_REQ_SET(reg, 1);
	CHECK(reg == 0x00008104U, "REQ bit set matches the second IND write in boot3");
	CHECK(ODI_SW_DSF_GEM_CAM_CTL_REQ_GET(reg) == 1, "REQ unpack");
}

/* --- Table primitives --------------------------------------------------- */

/* boot3 cmd 25, 5th occurrence (raw lines 18180000-tagged region around the
 * 11590000 ns mark): IND=0x104, WR=0x79a, IND=0x8104, TRAFFIC_CFG[4]=2.
 */
static void test_gpon_ds_port_write(void)
{
	static const struct expect_write want[] = {
		{ 0x701100, 0x00000104 },
		{ 0x701104, 0x0000079a },
		{ 0x701100, 0x00008104 },
		{ 0x701410, 0x00000002 },
	};
	int rc;

	odi_mock_reset();
	rc = odi_switch_gpon_ds_port_write(4, 0x79a, 2);
	CHECK(rc == 0, "gpon_ds_port_write reports success");
	CHECK(mock_log_matches(want, 4), "gpon_ds_port_write write log equals boot3 cmd 25 #5");
}

/* Same shape as the port table, minus the TRAFFIC_CFG step. Not present
 * in boot3 -- derived by analogy, not trace-derived.
 */
static void test_gpon_alloc_write(void)
{
	static const struct expect_write want[] = {
		{ 0x7010c0, 0x00000105 }, /* mode=1 (write), idx=5 */
		{ 0x7010c4, 0x00000123 }, /* alloc_id */
		{ 0x7010c0, 0x00008105 }, /* req=1 */
	};
	int rc;

	odi_mock_reset();
	rc = odi_switch_gpon_alloc_write(5, 0x123);
	CHECK(rc == 0, "gpon_alloc_write reports success");
	CHECK(mock_log_matches(want, 3), "gpon_alloc_write write log matches the expected shape");
}

/* boot3 cmd 51, raw lines 239-246: VLAN_INGRESS_CHECK (0xf) rewritten before each
 * of the four PORT_EGRESS_TAG_MODE writes, all zero in this instance.
 */
static void test_vlan_egress_tag_group_write(void)
{
	static const struct expect_write want[] = {
		{ 0x013004, 0xf }, { 0x02a000, 0 },
		{ 0x013004, 0xf }, { 0x02a004, 0 },
		{ 0x013004, 0xf }, { 0x02a008, 0 },
		{ 0x013004, 0xf }, { 0x02a00c, 0 },
	};
	uint32_t tag[4] = { 0, 0, 0, 0 };

	odi_mock_reset();
	odi_switch_vlan_egress_tag_group_write(0xf, tag);
	CHECK(mock_log_matches(want, 8), "vlan_egress_tag_group_write write log equals boot3 lines 239-246");
}

int main(void)
{
	test_hw_field_roundtrip();
	test_gpon_ds_port_write();
	test_gpon_alloc_write();
	test_vlan_egress_tag_group_write();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_test: ok\n");
	return 0;
}
