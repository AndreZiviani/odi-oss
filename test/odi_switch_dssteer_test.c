/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_dssteer_test.c -- which queue a downstream frame takes. The
 * frame's internal priority picks it (the UNI's priority to queue map is the
 * identity), and a downstream CF row can assign that priority; the rows of a
 * bridge connection carry the priority of the queue its GEM port's flow
 * (cmd 25, ds_pq_pri) points at. Host-side, against the register mock. Part
 * of `make test-host`.
 *
 * What is checked: the action word the row gets (bit 56 enables, bits 59..57
 * the priority), in whichever order the flow and the connection arrive; that
 * a priority changing later rewrites the action word alone; that a queue
 * which is not a UNI's, a priority past the eight queues, ds_steer off and a
 * MIB reset all leave or put the row back at the port default; and that an
 * OLT that provisions nothing writes nothing extra.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

#include "bdgconn_rules.h"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-70s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

#define MAP0	0x1c0b0U
#define PRI_EN	ODI_SW_CF_A1_PRI_EN

static void setup(void)
{
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_sw_ds_steer = true;
	odi_mock.regs[odi_mock_slot(MAP0)] = 0x00fac688U;
	odi_mock.log_n = 0;
}

/* omcid's cmd 25 for a downstream flow: the GEM port, the queue's priority
 * and the UNI port it hangs off. */
static int ds_flow(uint32_t flow, uint32_t gem, uint32_t pq_pri, uint32_t port)
{
	struct omci_gemflow g;

	memset(&g, 0, sizeof g);
	g.dir = OMCI_GEMFLOW_DS;
	g.flow_id = flow;
	g.gem_port = gem;
	g.ena = 1;
	g.ds_pq_pri = pq_pri;
	g.ds_port = port;
	return odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g);
}

/* The data service of ISP1: untagged on the UNI, VID 11 on the PON side,
 * both directions, on downstream flow `flow`. */
static int data_service(int id, uint32_t flow)
{
	struct omci_vlan_oper vr;
	struct omci_bdgconn b;

	bdg_gen_manual(&vr, 11, 0, 0);
	bdg_conn(&b, id, OMCI_DIR_BI, 1, 0, flow, &vr);
	return odi_switch_cmd(51, &b, sizeof b);
}

/* The multicast rule: downstream only, on its own flow. */
static int mcast_service(int id, uint32_t flow)
{
	struct omci_vlan_oper vr;
	struct omci_bdgconn b;

	bdg_gen_manual(&vr, 0, 0, 1);
	bdg_conn(&b, id, OMCI_DIR_DS, 5, 0, flow, &vr);
	return odi_switch_cmd(51, &b, sizeof b);
}

static const struct odi_sw_cf_row *ds_row(int id)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == id &&
		    odi_sw_bdgconn[i].ds_row >= 0)
			return &odi_sw_cf[odi_sw_bdgconn[i].ds_row];
	return NULL;
}

static int ds_row_idx(int id)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == id)
			return odi_sw_bdgconn[i].ds_row;
	return -1;
}

/* The priority the row assigns, -1 when it assigns none. */
static int row_pri(const struct odi_sw_cf_row *r)
{
	if (!r || !(r->act[1] & PRI_EN))
		return -1;
	return (int)((r->act[1] & ODI_SW_CF_A1_PRI_MASK) >> ODI_SW_CF_A1_PRI_SHIFT);
}

/* Table writes to the action table since the log was cleared. */
static unsigned int act_writes(void)
{
	unsigned int i, n = 0;

	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'T' &&
		    (odi_mock.log[i].addr >> 16) == ODI_SW_TBL_CLS_DS_ACTION)
			n++;
	return n;
}

static unsigned int table_writes(void)
{
	unsigned int i, n = 0;

	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'T')
			n++;
	return n;
}

/* The words of the last action row written to the DS action table. */
static int last_act(uint32_t words[3])
{
	int i;

	odi_mock_table_flush();
	for (i = (int)odi_mock.log_n - 1; i >= 0; i--) {
		if (odi_mock.log[i].kind == 'T' &&
		    (odi_mock.log[i].addr >> 16) == ODI_SW_TBL_CLS_DS_ACTION) {
			words[0] = odi_mock.log[i + 1].val;
			words[1] = odi_mock.log[i + 2].val;
			words[2] = odi_mock.log[i + 3].val;
			return odi_mock.log[i + 1].kind == 'D';
		}
	}
	return 0;
}

static void test_flow_then_connection(void)
{
	uint32_t w[3];

	puts("the flow arrives first, then its connection:");
	setup();
	ok(ds_flow(5, 1562, 7, 0) == 0, "cmd 25, flow 5, queue priority 7 on the UNI");
	ok(data_service(0, 5) == 0, "the data service on flow 5");
	ok(row_pri(ds_row(0)) == 7, "its downstream row assigns priority 7");
	ok(last_act(w) && w[1] == ds_row(0)->act[1], "the action row written is the one the row holds");
	ok(ODI_SW_CF_A1_PRI_SHIFT == 25 && PRI_EN == (1U << 24),
	   "priority in bits 59..57 (word 1 bits 27..25), enable bit 56");
}

static void test_connection_then_flow(void)
{
	uint32_t w[3];
	int idx;

	puts("the connection arrives first, then its flow:");
	setup();
	ok(data_service(0, 5) == 0, "the data service on flow 5");
	ok(row_pri(ds_row(0)) == -1, "no queue yet: the row assigns nothing");
	idx = ds_row_idx(0);
	odi_mock.log_n = 0;
	ok(ds_flow(5, 1562, 3, 0) == 0, "cmd 25, queue priority 3");
	ok(row_pri(ds_row(0)) == 3, "the row now assigns priority 3");
	ok(act_writes() == 1, "one write to the action table");
	ok(table_writes() == 1, "and no other table is touched");
	ok(last_act(w) && w[1] == ds_row(0)->act[1] && w[2] == ds_row(0)->act[2],
	   "it carries the row's words 1 and 2");
	ok(idx == ds_row_idx(0), "the row did not move");
}

static void test_change_and_same(void)
{
	setup();
	(void)data_service(0, 5);
	(void)ds_flow(5, 1562, 7, 0);
	odi_mock.log_n = 0;
	ok(ds_flow(5, 1562, 7, 0) == 0, "the same flow again");
	ok(act_writes() == 0, "writes nothing to the classifier");
	ok(ds_flow(5, 1562, 2, 0) == 0, "the queue moves to priority 2");
	ok(row_pri(ds_row(0)) == 2 && act_writes() == 1, "one action write, priority 2");
	ok(ds_flow(5, 1562, 0, 0) == 0, "and to priority 0");
	ok(row_pri(ds_row(0)) == 0, "priority 0 is assigned, not left to the default");
}

static void test_not_steered(void)
{
	setup();
	(void)data_service(0, 5);
	ok(ds_flow(5, 1562, 7, 2) == 0 && row_pri(ds_row(0)) == -1,
	   "a queue of the PON port (a VEIP's) is not steered");
	ok(ds_flow(5, 1562, 7, 3) == 0 && row_pri(ds_row(0)) == -1, "nor one of the CPU port");
	ok(ds_flow(5, 1562, 8, 0) == 0 && row_pri(ds_row(0)) == -1,
	   "nor a priority past the eight queues");
	ok(ds_flow(5, 1562, 0xffff, 0) == 0 && row_pri(ds_row(0)) == -1, "0xffff either");
	ok(ds_flow(5, 1562, 4, 0) == 0 && row_pri(ds_row(0)) == 4, "a UNI queue is");
	ok(ds_flow(5, 1562, 7, 2) == 0 && row_pri(ds_row(0)) == -1,
	   "and moving the pointer to a non-UNI queue takes the priority off again");
}

static void test_flows_are_separate(void)
{
	struct omci_vlan_oper vr;
	struct omci_bdgconn b;

	setup();
	(void)ds_flow(1, 1434, 6, 0);
	(void)ds_flow(5, 1562, 2, 0);
	(void)data_service(0, 5);
	bdg_gen_vid_filter(&vr, 100, -1);
	bdg_conn(&b, 1, OMCI_DIR_BI, 1, 1, 1, &vr);
	ok(odi_switch_cmd(51, &b, sizeof b) == 0, "a second service on flow 1");
	ok(row_pri(ds_row(0)) == 2 && row_pri(ds_row(1)) == 6,
	   "each row has the priority of its own flow");
	odi_mock.log_n = 0;
	(void)ds_flow(1, 1434, 5, 0);
	ok(row_pri(ds_row(0)) == 2 && row_pri(ds_row(1)) == 5, "a change of one flow leaves the other");
	ok(act_writes() == 1, "and writes one row");
	ok(mcast_service(2, 0) == 0 && row_pri(ds_row(2)) == -1,
	   "a downstream-only rule on a flow with no queue yet has none");
	(void)ds_flow(0, 4095, 7, 0);
	ok(row_pri(ds_row(2)) == 7, "and gets it with the flow's cmd 25");
}

static void test_reactivation(void)
{
	int idx;

	setup();
	(void)ds_flow(5, 1562, 7, 0);
	(void)data_service(0, 5);
	idx = ds_row_idx(0);
	ok(data_service(0, 5) == 0, "the same service activated again (uni_mask merge)");
	ok(ds_row_idx(0) == idx && row_pri(ds_row(0)) == 7, "same row, still steered");
	odi_mock.log_n = 0;
	{
		int32_t id = 0;

		ok(odi_switch_cmd(50, &id, sizeof id) == 0, "released");
	}
	ok(data_service(0, 5) == 0 && row_pri(ds_row(0)) == 7, "and added again: steered from the start");
}

static void test_defaults(void)
{
	uint32_t a1;

	puts("an OLT that provisions no queue:");
	setup();
	(void)data_service(0, 5);
	a1 = ds_row(0)->act[1];
	ok(!(a1 & (PRI_EN | ODI_SW_CF_A1_PRI_MASK)), "the row has no priority bits");

	setup();
	ds_flow(5, 1562, 7, 0);
	ok(odi_sw_ds_steer, "ds_steer defaults on");
	setup();
	odi_sw_ds_steer = false;
	ok(ds_flow(5, 1562, 7, 0) == 0 && data_service(0, 5) == 0 && row_pri(ds_row(0)) == -1,
	   "ds_steer off: the row keeps the port default priority");
	ok(ds_row(0)->act[1] == a1, "and its action word is the unsteered one, bit for bit");
	ok(odi_mock.regs[odi_mock_slot(MAP0)] == 0x00fac688U, "the priority map is untouched");
}

static void test_map_identity(void)
{
	setup();
	odi_mock.regs[odi_mock_slot(MAP0)] = 0U;
	(void)ds_flow(5, 1562, 7, 0);
	ok(odi_mock.regs[odi_mock_slot(MAP0)] == 0x00fac688U,
	   "a steered flow makes the UNI's priority map the identity, so priority n is queue n");
	setup();
	odi_mock.regs[odi_mock_slot(MAP0)] = 0U;
	(void)ds_flow(5, 1562, 7, 2);
	ok(odi_mock.regs[odi_mock_slot(MAP0)] == 0U, "a flow that is not steered does not touch it");
}

static void test_mib_reset(void)
{
	setup();
	(void)ds_flow(5, 1562, 7, 0);
	(void)data_service(0, 5);
	odi_mock.log_n = 0;
	ok(odi_switch_cmd(0, NULL, 0) == 0, "a MIB reset");
	ok(row_pri(ds_row(0)) == -1, "the row is back at the port default");
	ok(act_writes() == 1, "with one action write");
	odi_mock.log_n = 0;
	ok(odi_switch_cmd(0, NULL, 0) == 0 && table_writes() == 0, "a second reset has nothing to put back");
	(void)data_service(0, 5);
	ok(row_pri(ds_row(0)) == -1, "and a connection made now is not steered until its flow says so");
}

int main(void)
{
	test_flow_then_connection();
	test_connection_then_flow();
	test_change_and_same();
	test_not_steered();
	test_flows_are_separate();
	test_reactivation();
	test_defaults();
	test_map_identity();
	test_mib_reset();
	if (failures) {
		printf("odi_switch_dssteer_test: %d failure(s)\n", failures);
		return 1;
	}
	puts("odi_switch_dssteer_test: ok");
	return 0;
}
