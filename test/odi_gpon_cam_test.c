/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_cam_test.c -- the GPON CAM requests and their completion,
 * against test/odi_switch_mock.h: every delete (MODE 3) and the search
 * before the Encrypted_Port-ID rewrite waits for DONE, and a write or
 * delete that does not complete moves no bookkeeping -- no Alloc-ID row,
 * GEM port or DS slot recorded for a write that did not land, and a row
 * whose delete did not land kept, so the next release deletes it again.
 *
 * The mock finishes every poll at the first read; poll_stuck makes every
 * poll time out, an engine that never answers. Requests are read from the
 * write log: a control-word write with REQ (bit 15) set.
 */
#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define ONU_ID		26U
#define OMCC_GEM	0x01aU
#define REQ		(1U << 15)

/* CAM requests of one op logged since the log was last cleared, on the
 * Alloc-ID (ctl 0x7010c0) or GEM (0x701100) control word; row -1 is any.
 */
static unsigned int requests(uint32_t ctl, uint32_t op, int row)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *w = &odi_mock.log[i];

		if (w->kind != 'W' || w->addr != ctl || !(w->val & REQ))
			continue;
		if (((w->val >> 8) & 0x3U) != op)
			continue;
		if (row >= 0 && (w->val & 0x7fU) != (uint32_t)row)
			continue;
		n++;
	}
	return n;
}

static unsigned int writes_to(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

static unsigned int alloc_ids(void)
{
	uint16_t ids[32];

	return odi_gpon_get_alloc_ids(ids, 32U);
}

static void setup(void)
{
	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_switch_ds_slot_reset();
}

/* Assign_Alloc-ID whose CAM write never completes: not recorded, and the
 * next copy of the message writes again.
 */
static void test_alloc_write_not_done(void)
{
	setup();
	odi_mock.poll_stuck = 1;
	CHECK(odi_gpon_hw_alloc_id_assign(0x123) < 0, "an Alloc-ID write that does not complete fails");
	CHECK(alloc_ids() == 0U, "and the Alloc-ID is not listed as held");
	odi_mock.poll_stuck = 0;
	odi_mock.log_n = 0;
	CHECK(odi_gpon_hw_alloc_id_assign(0x123) == 0, "the next copy of the message writes row 0");
	CHECK(requests(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, 1U, 0) == 1U, "with a CAM write, not the dedup");
	CHECK(alloc_ids() == 1U, "and now it is held");
}

/* The default (ONU-ID) row: a write that did not land is not deleted at
 * the release, as a row this layer never placed.
 */
static void test_default_row_write_not_done(void)
{
	const struct odi_gpon_fsm_ops *ops = odi_gpon_hw_ops();

	setup();
	odi_mock.poll_stuck = 1;
	ops->set_onu_id(NULL, ONU_ID);
	odi_mock.poll_stuck = 0;
	odi_mock.log_n = 0;
	ops->set_onu_id(NULL, ODI_GPON_ONU_ID_BROADCAST);
	CHECK(requests(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, 3U, 16) == 0U,
	      "no delete for a default row whose write never completed");
}

/* An Assign_Alloc-ID for the ONU-ID itself (some OLTs send one) names the
 * default row: no CAM write, no pool row taken, not listed, and the next
 * Alloc-ID still gets row 0, the row its T-CONT index uses.
 */
static void test_default_alloc_id_dedups(void)
{
	const struct odi_gpon_fsm_ops *ops = odi_gpon_hw_ops();

	setup();
	ops->set_onu_id(NULL, ONU_ID);
	odi_mock.log_n = 0;
	CHECK(odi_gpon_hw_alloc_id_assign(ONU_ID) == (int)ODI_GPON_HW_ALLOC_ROW_DEFAULT,
	      "the ONU-ID as an Alloc-ID is the default row");
	CHECK(requests(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, 1U, -1) == 0U, "with no CAM write");
	CHECK(alloc_ids() == 0U, "and it is not listed as a held Alloc-ID");
	CHECK(odi_gpon_hw_alloc_id_assign(0x123) == 0, "the next Alloc-ID gets row 0");
	CHECK(odi_gpon_hw_alloc_id_release(ONU_ID) < 0, "a deallocation of the default one is ignored");
	CHECK(requests(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, 3U, -1) == 0U, "no delete of row 16");
	ops->set_onu_id(NULL, ODI_GPON_ONU_ID_BROADCAST);
	odi_mock.log_n = 0;
	CHECK(odi_gpon_hw_alloc_id_assign(ONU_ID) == 0, "after the release the value is forgotten");
}

/* A release whose deletes do not complete: the first timeout ends the
 * deletes, the rows stay held, and the next release deletes them.
 */
static void test_release_delete_not_done(void)
{
	const struct odi_gpon_fsm_ops *ops = odi_gpon_hw_ops();

	setup();
	ops->set_onu_id(NULL, ONU_ID);
	(void)odi_gpon_hw_alloc_id_assign(0x101);
	(void)odi_gpon_hw_alloc_id_assign(0x102);
	(void)odi_gpon_hw_alloc_id_assign(0x103);
	odi_gpon_hw_gem_port_configure(OMCC_GEM, 1);
	CHECK(alloc_ids() == 3U, "three Alloc-IDs held before the release");

	odi_mock.poll_stuck = 1;
	odi_mock.log_n = 0;
	ops->set_onu_id(NULL, ODI_GPON_ONU_ID_BROADCAST);
	CHECK(requests(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, 3U, -1) == 1U,
	      "the first delete that does not complete ends the deletes");
	CHECK(requests(ODI_SW_DSF_GEM_CAM_CTL_OFF, 3U, -1) == 0U,
	      "no GEM delete into a block that is not answering");
	CHECK(alloc_ids() == 3U, "the rows not deleted stay held");

	odi_mock.poll_stuck = 0;
	odi_mock.log_n = 0;
	ops->set_onu_id(NULL, ODI_GPON_ONU_ID_BROADCAST);
	CHECK(requests(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, 3U, -1) == 4U,
	      "the next release deletes them: rows 0, 1, 2 and the default row 16");
	CHECK(requests(ODI_SW_DSF_GEM_CAM_CTL_OFF, 3U, 64) == 1U, "and the OMCC GEM row");
	CHECK(alloc_ids() == 0U, "nothing held after it");
}

/* Assign_Alloc-ID type 255 whose delete does not complete: still held. */
static void test_dealloc_not_done(void)
{
	setup();
	(void)odi_gpon_hw_alloc_id_assign(0x201);
	odi_mock.poll_stuck = 1;
	CHECK(odi_gpon_hw_alloc_id_release(0x201) < 0, "a deallocation that does not complete fails");
	CHECK(alloc_ids() == 1U, "and the Alloc-ID stays held for the next copy");
	odi_mock.poll_stuck = 0;
	CHECK(odi_gpon_hw_alloc_id_release(0x201) == 0, "the next copy deletes row 0");
	CHECK(alloc_ids() == 0U, "released");
}

/* Configure_Port-ID whose CAM write does not complete: no upstream map, no
 * stream gate, not recorded as the OMCC port.
 */
static void test_gem_configure_not_done(void)
{
	setup();
	odi_mock.poll_stuck = 1;
	odi_gpon_hw_gem_port_configure(OMCC_GEM, 1);
	CHECK(writes_to(ODI_SW_US_GEM_PORT_MAP(64U)) == 0U, "no upstream map for a row not written");
	CHECK(writes_to(ODI_SW_PONQ_STREAM_VALID(2U)) == 0U, "and no stream gate");
	odi_mock.poll_stuck = 0;
	odi_mock.log_n = 0;
	odi_gpon_hw_gem_port_configure(OMCC_GEM, 1);
	CHECK(writes_to(ODI_SW_US_GEM_PORT_MAP(64U)) == 1U, "the next copy writes the upstream map");
}

/* Encrypted_Port-ID for the OMCC port: the CAM search must finish before
 * the write is issued on the same control word.
 */
static void test_encrypted_search_not_done(void)
{
	setup();
	odi_gpon_hw_gem_port_configure(OMCC_GEM, 1);
	odi_mock.poll_stuck = 1;
	odi_mock.log_n = 0;
	CHECK(odi_gpon_hw_gem_port_encrypted(OMCC_GEM, 0) < 0, "a search that does not finish fails");
	CHECK(requests(ODI_SW_DSF_GEM_CAM_CTL_OFF, 2U, 64) == 1U, "the search was issued");
	CHECK(requests(ODI_SW_DSF_GEM_CAM_CTL_OFF, 1U, -1) == 0U, "no write over a search still running");
	odi_mock.poll_stuck = 0;
	odi_mock.log_n = 0;
	CHECK(odi_gpon_hw_gem_port_encrypted(OMCC_GEM, 0) == 1, "a search that finishes is followed");
	CHECK(requests(ODI_SW_DSF_GEM_CAM_CTL_OFF, 1U, 64) == 1U, "by the write");
}

/* Encrypted_Port-ID with 'a' set on the OMCC port: the row decrypts
 * (FLAGS 0x14, OMCI plus AES), as the stock module sets it on its OMCC
 * flow; 'a' clear and a new Configure_Port-ID put back the bare 0x04.
 */
static void test_encrypted_omcc(void)
{
	setup();
	odi_gpon_hw_gem_port_configure(OMCC_GEM, 1);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(64U)) == 0x04U, "configured: OMCI, no AES");
	CHECK(odi_gpon_hw_gem_port_encrypted(OMCC_GEM, 1) == 1, "the OMCC row is rewritten");
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(64U)) == 0x14U, "with the AES bit: 0x14");
	CHECK(odi_gpon_hw_gem_port_encrypted(OMCC_GEM, 0) == 1, "a clear is applied too");
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(64U)) == 0x04U, "back to 0x04");
	(void)odi_gpon_hw_gem_port_encrypted(OMCC_GEM, 1);
	odi_gpon_hw_gem_port_configure(OMCC_GEM, 1);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(64U)) == 0x04U,
	      "Configure_Port-ID writes the port unencrypted, as stock");
}

/* cmd 25 downstream whose CAM write does not complete: an error to omcid,
 * no DS slot (an Encrypted_Port-ID for the port finds none) and no flow in
 * the readback.
 */
static void test_cmd25_ds_not_done(void)
{
	struct omci_gemflow g;
	struct omci_flows f;
	uint32_t idx;

	setup();
	memset(&g, 0, sizeof g);
	g.dir = OMCI_GEMFLOW_DS;
	g.flow_id = 3;
	g.gem_port = 0x59a;
	g.ena = 1;
	odi_mock.poll_stuck = 1;
	CHECK(odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g) != 0, "cmd 25 reports the CAM write that failed");
	odi_mock.poll_stuck = 0;
	CHECK(!odi_switch_ds_slot_find(0x59a, &idx), "no DS slot recorded for it");
	memset(&f, 0, sizeof f);
	(void)odi_switch_cmd(OMCI_FLOWS_CMD, &f, OMCI_FLOWS_LEN);
	CHECK(f.ds_count == 0U, "and no downstream flow in the readback");
	CHECK(odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g) == 0, "the retry succeeds");
	CHECK(odi_switch_ds_slot_find(0x59a, &idx) && idx == 3U, "and records slot 3");
}

int main(void)
{
	test_alloc_write_not_done();
	test_default_row_write_not_done();
	test_default_alloc_id_dedups();
	test_release_delete_not_done();
	test_dealloc_not_done();
	test_gem_configure_not_done();
	test_encrypted_search_not_done();
	test_encrypted_omcc();
	test_cmd25_ds_not_done();
	CHECK(odi_mock_locks_idle(), "every switch lock released on return");
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_cam_test: ok\n");
	return 0;
}
