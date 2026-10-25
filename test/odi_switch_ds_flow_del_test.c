/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_ds_flow_del_test.c -- cmd 25 downstream with ena 0, the delete
 * of a downstream GEM flow, and cmd 0, the MIB reset that deletes them all,
 * against a model of the GEM port CAM hung on the mock's write hook
 * (test/odi_switch_mock.h):
 *
 *   - a request (REQ set) is answered at once: OP 1 stores WDATA, OP 2 sets
 *     ROW_VALID when the row holds an entry and puts it in RDATA, OP 3
 *     clears the row unless the case made it sticky; DONE is set, except
 *     with poll_stuck, an engine that never answers.
 *
 * A delete counts only when the row reads back clear: the CAM row and the
 * flow type word cleared, the slot record dropped (an Encrypted_Port-ID for
 * the old port then finds no slot, and a new port in the row gets no AES
 * bit it was not given), the readback row emptied. A delete that does not
 * complete, or a row that does not clear, is an error and keeps the flow.
 * /proc/odi_gpon/cam's GEM rows read the model back.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define ROWS	128U
#define REQ	(1U << 15)

static struct {
	int valid[ROWS];
	uint32_t entry[ROWS];
	int sticky[ROWS];
} cam;

static uint32_t *reg(uint32_t off)
{
	return &odi_mock.regs[odi_mock_slot(off)];
}

static void cam_write(uint32_t off, uint32_t val)
{
	unsigned int row = val & 0x7fU, op = (val >> 8) & 3U;
	uint32_t out = val;

	if (off != ODI_SW_DSF_GEM_CAM_CTL_OFF || !(val & REQ))
		return;
	switch (op) {
	case 1:
		cam.valid[row] = 1;
		cam.entry[row] = *reg(ODI_SW_DSF_GEM_CAM_WDATA_OFF) & 0xfffU;
		break;
	case 2:
		if (cam.valid[row])
			out |= ODI_SW_DSF_GEM_CAM_CTL_ROW_VALID;
		*reg(ODI_SW_DSF_GEM_CAM_RDATA_OFF) = cam.entry[row];
		break;
	case 3:
		if (!cam.sticky[row])
			cam.valid[row] = 0;
		break;
	}
	if (!odi_mock.poll_stuck)
		out |= ODI_SW_DSF_GEM_CAM_CTL_DONE;
	*reg(off) = out;
}

/* CAM requests of one op on row `row` since the log was last cleared. */
static unsigned int requests(uint32_t op, unsigned int row)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *w = &odi_mock.log[i];

		if (w->kind == 'W' && w->addr == ODI_SW_DSF_GEM_CAM_CTL_OFF && (w->val & REQ) &&
		    ((w->val >> 8) & 3U) == op && (w->val & 0x7fU) == row)
			n++;
	}
	return n;
}

static void setup(void)
{
	odi_mock_reset();
	memset(&cam, 0, sizeof cam);
	odi_mock_write_hook = cam_write;
	odi_switch_cmd_reset_state();
	odi_switch_ds_slot_reset();
}

static int ds(uint32_t flow, uint32_t port, uint32_t mcast, uint32_t ena)
{
	struct omci_gemflow g;

	memset(&g, 0, sizeof g);
	g.dir = OMCI_GEMFLOW_DS;
	g.flow_id = flow;
	g.gem_port = port;
	g.mcast_filter = mcast;
	g.ena = ena;
	return odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g);
}

static uint32_t ds_count(void)
{
	struct omci_flows f;

	memset(&f, 0, sizeof f);
	(void)odi_switch_cmd(OMCI_FLOWS_CMD, &f, OMCI_FLOWS_LEN);
	return f.ds_count;
}

/* Rows of the model holding `port`. */
static unsigned int rows_with(uint32_t port)
{
	unsigned int r, n = 0;

	for (r = 0; r < ROWS; r++)
		n += cam.valid[r] && cam.entry[r] == port;
	return n;
}

/* An extra multicast port's flow, withdrawn: the row clear in the CAM and
 * in the flow type word, the slot and the readback row gone.
 */
static void test_delete(void)
{
	uint32_t idx;

	setup();
	CHECK(ds(0, 0xfff, 1, 1) == 0 && ds(1, 0xfa0, 1, 1) == 0, "two multicast flows");
	CHECK(cam.valid[1] && cam.entry[1] == 0xfa0, "row 1 holds 0xfa0");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0x03U, "typed Ethernet, multicast");
	odi_mock.log_n = 0;
	CHECK(ds(1, 0xfa0, 0, 0) == 0, "cmd 25 ena 0 deletes downstream flow 1");
	CHECK(requests(3, 1) == 1 && requests(2, 1) == 1, "a clear of row 1, then a read of it");
	CHECK(!cam.valid[1] && rows_with(0xfa0) == 0, "row 1 is clear, 0xfa0 is in no row");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0U, "its flow type word is 0");
	CHECK(!odi_switch_ds_slot_find(0xfa0, &idx), "no DS slot for the port");
	CHECK(ds_count() == 1U, "the readback holds flow 0 alone");
	CHECK(cam.valid[0] && *reg(ODI_SW_DSF_GEM_FLOW_TYPE(0)) == 0x03U, "flow 0 untouched");
}

/* The row given to another port after the delete: no AES bit the old port
 * had, an Encrypted_Port-ID for the old port finds no slot, and the old port
 * is in no row.
 */
static void test_reuse_after_delete(void)
{
	uint32_t idx;

	setup();
	CHECK(ds(2, 0xfa0, 1, 1) == 0, "flow 2 on 0xfa0");
	CHECK(odi_switch_gpon_encrypt_port(0xfa0, 1) == 0, "the OLT encrypts 0xfa0");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0x13U, "row 2 decrypts: 0x13");
	CHECK(ds(2, 0xfa0, 0, 0) == 0, "flow 2 deleted");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0U, "the AES bit goes with the type word");
	CHECK(odi_switch_gpon_encrypt_port(0xfa0, 1) != 0, "an Encrypted_Port-ID for 0xfa0 finds no slot");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0U, "and writes nothing into row 2");
	CHECK(ds(2, 0xfa1, 1, 1) == 0, "flow 2 given to 0xfa1");
	CHECK(cam.entry[2] == 0xfa1 && rows_with(0xfa0) == 0, "row 2 holds 0xfa1, 0xfa0 none");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0x03U, "0x03: no AES bit 0xfa1 was not given");
	CHECK(odi_switch_ds_slot_find(0xfa1, &idx) && idx == 2U, "slot 2 is 0xfa1");
	CHECK(ds(3, 0xfa0, 1, 1) == 0, "0xfa0 back, on flow 3");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(3)) == 0x13U, "with the AES the OLT asked for, kept");
	CHECK(rows_with(0xfa0) == 1U, "in one row");
}

/* A delete that never completes: an error, and nothing forgotten, so omcid
 * keeps the id and its retry deletes the row.
 */
static void test_delete_not_done(void)
{
	uint32_t idx;

	setup();
	CHECK(ds(4, 0xfa2, 1, 1) == 0, "flow 4 on 0xfa2");
	odi_mock.poll_stuck = 1;
	CHECK(ds(4, 0xfa2, 0, 0) != 0, "a delete that does not complete fails");
	odi_mock.poll_stuck = 0;
	CHECK(odi_switch_ds_slot_find(0xfa2, &idx) && idx == 4U, "the slot is kept");
	CHECK(ds_count() == 5U, "and the readback row");
	CHECK(ds(4, 0xfa2, 0, 0) == 0, "the retry deletes it");
	CHECK(!cam.valid[4] && ds_count() == 0U, "row 4 clear, no flow left");
}

/* A clear the CAM does not take: the read back finds the row valid, and
 * the delete fails rather than free a row that still holds the port.
 */
static void test_row_does_not_clear(void)
{
	uint32_t idx;

	setup();
	CHECK(ds(5, 0xfa3, 1, 1) == 0, "flow 5 on 0xfa3");
	cam.sticky[5] = 1;
	CHECK(ds(5, 0xfa3, 0, 0) == -EIO, "a row still valid after the clear is -EIO");
	CHECK(odi_switch_ds_slot_find(0xfa3, &idx) && idx == 5U, "the slot is kept");
	CHECK(ds_count() == 6U, "and the readback row");
	CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(5)) == 0U, "the type word is 0 all the same");
	cam.sticky[5] = 0;
	CHECK(ds(5, 0xfa3, 0, 0) == 0 && ds_count() == 0U, "the retry deletes it");
}

/* A flow id cmd 25 never created is cleared all the same, and is no error;
 * one past the table is refused.
 */
static void test_delete_unused(void)
{
	setup();
	cam.valid[9] = 1;
	cam.entry[9] = 0x123;
	odi_mock.log_n = 0;
	CHECK(ds(9, 0, 0, 0) == 0, "deleting flow 9, never created, is no error");
	CHECK(requests(3, 9) == 1 && !cam.valid[9], "and clears row 9");
	CHECK(ds(ODI_SW_CMD_GEM_DS_MAX, 0, 0, 0) == ODI_SW_EOPNOTSUPP, "flow 64 is refused");
	CHECK(requests(3, 64) == 0, "and the OMCC row is not touched");
}

/* cmd 0: every downstream flow cmd 25 created goes; the OMCC row stays. */
static void test_mib_reset(void)
{
	uint32_t none = 0, idx;

	setup();
	cam.valid[64] = 1;
	cam.entry[64] = 0x01a;	/* the OMCC, the GPON core's */
	CHECK(ds(0, 0xfff, 1, 1) == 0 && ds(1, 0x59a, 0, 1) == 0 && ds(7, 0xfa0, 1, 1) == 0,
	      "flows 0, 1 and 7");
	CHECK(odi_switch_cmd(OMCI_MIB_RESET_CMD, &none, sizeof none) == 0, "cmd 0 accepted");
	CHECK(!cam.valid[0] && !cam.valid[1] && !cam.valid[7], "rows 0, 1 and 7 clear");
	CHECK(!*reg(ODI_SW_DSF_GEM_FLOW_TYPE(0)) && !*reg(ODI_SW_DSF_GEM_FLOW_TYPE(1)) &&
	      !*reg(ODI_SW_DSF_GEM_FLOW_TYPE(7)), "and their type words");
	CHECK(cam.valid[64] && cam.entry[64] == 0x01a, "the OMCC row is left");
	CHECK(ds_count() == 0U && !odi_switch_ds_slot_find(0x59a, &idx), "no flow, no slot");
	CHECK(ds(0, 0xffe, 1, 1) == 0 && rows_with(0xfff) == 0, "the next provisioning starts clean");
}

/* /proc/odi_gpon/cam's GEM rows: what the hardware holds, and nothing for
 * a deleted flow.
 */
static void test_cam_readback(void)
{
	int hw[ODI_GPON_HW_GEM_CAM_ROWS];
	uint32_t type[ODI_GPON_HW_GEM_CAM_ROWS];

	setup();
	CHECK(ds(0, 0xfff, 1, 1) == 0 && ds(1, 0xfa0, 1, 1) == 0, "two flows");
	odi_gpon_hw_gem_cam_read(hw, type);
	CHECK(hw[0] == 0xfff && type[0] == 0x03U, "row 0: 0xfff, 0x03");
	CHECK(hw[1] == 0xfa0 && type[1] == 0x03U, "row 1: 0xfa0, 0x03");
	CHECK(ds(1, 0xfa0, 0, 0) == 0, "flow 1 deleted");
	odi_gpon_hw_gem_cam_read(hw, type);
	CHECK(hw[1] == -1 && type[1] == 0U, "row 1 reads clear");
	CHECK(hw[2] == -1 && hw[64] == -1, "as do the rows never written");
	odi_mock.poll_stuck = 1;
	odi_gpon_hw_gem_cam_read(hw, type);
	odi_mock.poll_stuck = 0;
	CHECK(hw[0] == -2 && hw[64] == -2, "a CAM that never answers reads -2");
}

int main(void)
{
	test_delete();
	test_reuse_after_delete();
	test_delete_not_done();
	test_row_does_not_clear();
	test_delete_unused();
	test_mib_reset();
	test_cam_readback();
	CHECK(odi_mock_locks_idle(), "every switch lock released on return");
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_ds_flow_del_test: ok\n");
	return 0;
}
