/* odi_switch_ponq_test.c -- the upstream PON queues of cmd 21, 23, 24 and
 * 25 in the shapes neither ISP1 nor ISP2 provisions (odi_switch_cmd_test.c
 * covers ISP1's against the stock capture): several queues on one T-CONT,
 * a T-CONT of the second group (index 8-15), more than five T-CONTs, the
 * queue rates, the queue delete, the flow delete, the T-CONT index as the
 * Alloc-ID CAM row, and the refusals. The expected words follow the
 * register layout of docs/SWITCH.md#upstream-pon-queues. Host-side,
 * against the register mock. Part of `make test-host`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-62s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

static uint32_t reg(uint32_t idx)
{
	return odi_mock.regs[odi_mock_slot(ODI_SW_PONQ_COUNT_MASK(idx))];
}

#define DRN_CMD		15U
#define SID2QID		20U
#define SIDVALID	37U
#define CIR		60U
#define PIR		125U
#define QMAP		190U
#define TCONT_EN	207U
#define WFQ_TYPE	208U
#define WFQ_WEIGHT	212U
#define GLB_TH		235U

static void fresh(void)
{
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_sw_host_alloc_row = NULL;
	odi_mock_ponq_boot_state();
}

/* cmd 21: the T-CONT index of alloc, n queues reserved; -1 if refused. */
static int tcont(uint32_t alloc, uint32_t n)
{
	struct omci_tcont t = { .index = n, .alloc_id = alloc };

	if (odi_switch_cmd(OMCI_TCONT_CMD, &t, sizeof t))
		return -1;
	return (int)t.index;
}

static int queue(uint32_t t, uint32_t ord, int wrr, uint32_t weight, uint32_t cir, uint32_t pir)
{
	struct omci_priq q;

	memset(&q, 0, sizeof q);
	q.owner = (uint16_t)t;
	q.index = (uint16_t)ord;
	q.wrr = (uint32_t)wrr;
	q.weight = (uint16_t)weight;
	q.cir = cir;
	q.pir = pir;
	q.dir = OMCI_GEMFLOW_US;
	return odi_switch_cmd(OMCI_PRIQ_CMD, &q, sizeof q);
}

static int queue_del(uint32_t t, uint32_t ord)
{
	struct omci_priq q;

	memset(&q, 0, sizeof q);
	q.owner = (uint16_t)t;
	q.index = (uint16_t)ord;
	q.dir = OMCI_GEMFLOW_US;
	return odi_switch_cmd(OMCI_PRIQ_DEL_CMD, &q, sizeof q);
}

static int flow(uint32_t slot, uint32_t gem, uint32_t t, uint32_t ord, uint32_t ena)
{
	struct omci_gemflow g;

	memset(&g, 0, sizeof g);
	g.flow_id = slot;
	g.gem_port = gem;
	g.tcont = t;
	g.queue = ord;
	g.ena = ena;
	g.dir = OMCI_GEMFLOW_US;
	return odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g);
}

static uint32_t sid2qid(uint32_t sid)
{
	return (reg(SID2QID + sid / 4U) >> ((sid % 4U) * 7U)) & 0x7fU;
}

static uint32_t weight_of(uint32_t pq)
{
	return (reg(WFQ_WEIGHT + pq / 3U) >> ((pq % 3U) * 10U)) & 0x3ffU;
}

static uint32_t wfq_of(uint32_t pq)
{
	return (reg(WFQ_TYPE + pq / 32U) >> (pq % 32U)) & 1U;
}

/* The host's stand-in for the GPON core's CAM: a fixed table. */
static uint16_t cam[32];
static int cam_row(uint16_t alloc_id)
{
	for (int i = 0; i < 32; i++)
		if (cam[i] == alloc_id)
			return i;
	return -1;
}

int main(void)
{
	int t0, t1, rc;

	puts("three queues on one T-CONT:");
	fresh();
	t0 = tcont(0x200, 3);
	ok(t0 == 0, "first T-CONT, index 0");
	ok(queue(0, 0, 0, 1, 0, 0) == 0, "ordinal 0, strict");
	ok(queue(0, 1, 1, 10, 0, 0) == 0, "ordinal 1, WFQ weight 10");
	ok(queue(0, 2, 1, 20, 0, 0) == 0, "ordinal 2, WFQ weight 20");
	ok(reg(QMAP + 0) == 0x7U, "queue mask: logical 0-2");
	ok(reg(TCONT_EN) == 0x10001U, "T-CONT 0 enabled, the OMCC kept");
	ok(wfq_of(0) == 0 && wfq_of(1) == 1 && wfq_of(2) == 1, "types: SP, WFQ, WFQ");
	ok(weight_of(0) == 1 && weight_of(1) == 10 && weight_of(2) == 20, "weights 1, 10, 20");
	ok(reg(PIR + 1) == 0x3ffffU && reg(CIR + 1) == 0, "no rate: CIR 0, PIR max");
	ok(queue(0, 3, 0, 1, 0, 0) != 0, "ordinal 3 is outside the block: refused");
	ok(flow(0, 100, 0, 0, 1) == 0 && flow(1, 101, 0, 2, 1) == 0, "flows on ordinals 0 and 2");
	ok(sid2qid(0) == 0 && sid2qid(1) == 2, "flows mapped to queues 0 and 2");
	ok(flow(2, 102, 0, 3, 1) != 0, "a flow on ordinal 3: refused");
	t1 = tcont(0x201, 2);
	ok(t1 == 1, "second T-CONT, index 1");
	ok(queue(1, 0, 0, 1, 0, 0) == 0 && queue(1, 1, 0, 1, 0, 0) == 0, "its two queues");
	ok(reg(QMAP + 1) == ((1U << 3) | (1U << 4)), "its block follows the first: logical 3-4");
	ok(reg(DRN_CMD) == ((4U << 3) | 6U), "the last queue added was drained: queue 4");
	ok(tcont(0x200, 3) == 0 && queue(0, 1, 1, 10, 0, 0) == 0 && reg(QMAP + 0) == 0x7U,
	   "provisioning again: same index, same block");
	ok(tcont(0x202, 30) == -1, "30 more queues do not fit in the group: refused");

	puts("rates:");
	fresh();
	tcont(0x300, 1);
	queue(0, 0, 0, 1, 100, 200);
	ok(reg(CIR + 0) == 99U && reg(PIR + 0) == 199U, "CIR 100, PIR 200 written as 99, 199");
	queue(0, 0, 0, 1, 0, 0x7fffffffU);
	ok(reg(CIR + 0) == 0 && reg(PIR + 0) == 0x3ffffU, "a PIR past the maximum is the maximum");
	queue(0, 0, 0, 1, 1, 0);
	ok(reg(CIR + 0) == 1U && reg(PIR + 0) == 0, "CIR 1 kept as 1, PIR 0 with a CIR stays 0");

	puts("T-CONTs past five, and in the second group:");
	fresh();
	for (uint32_t i = 0; i < 9; i++) {
		tcont(0x400 + i, 1);
		queue(i, 0, 1, 4, 0, 0);
		if (i == 5)
			ok(reg(DRN_CMD) == ((5U << 3) | 6U), "T-CONT 5 drains its own queue 5");
	}
	ok(reg(QMAP + 7) == (1U << 7), "T-CONT 7: logical 7 of group 0");
	ok(reg(QMAP + 8) == 1U, "T-CONT 8: logical 0 of group 1");
	ok(wfq_of(32) == 1 && weight_of(32) == 4, "its queue is physical 32");
	ok(flow(8, 800, 8, 0, 1) == 0 && sid2qid(8) == 32U, "its flow maps to queue 32");
	ok(reg(TCONT_EN) == (0x10000U | 0x1ffU), "nine T-CONTs enabled");

	puts("queue and flow delete:");
	fresh();
	tcont(0x500, 2);
	queue(0, 0, 0, 1, 0, 0);
	queue(0, 1, 0, 1, 0, 0);
	flow(0, 50, 0, 0, 1);
	flow(1, 51, 0, 1, 1);
	ok(reg(SIDVALID) == 0x3U && reg(GLB_TH) == ((7754U << 16) | 7594U), "two flows valid");
	ok(flow(1, 0, 0, 0, 0) == 0, "flow 1 deleted (ena 0)");
	ok(odi_mock.regs[odi_mock_slot(ODI_SW_US_GEM_PORT_MAP(1))] == 0 && reg(SIDVALID) == 0x1U,
	   "its slot carries GEM 0 and is not valid");
	ok(reg(GLB_TH) == ((7904U << 16) | 7744U), "thresholds for one flow again");
	ok(flow(1, 0, 0, 0, 0) == 0, "deleting it again is no error");
	ok(queue_del(0, 0) == 0, "queue 0 deleted");
	ok(reg(SIDVALID) == 0 && reg(QMAP + 0) == 0x2U, "its flow invalid, its bit out of the mask");
	ok(reg(TCONT_EN) == 0x10001U, "T-CONT still enabled: one queue left");
	ok(queue_del(0, 1) == 0 && reg(QMAP + 0) == 0 && reg(TCONT_EN) == 0x10000U,
	   "last queue deleted: T-CONT disabled");
	ok(queue_del(0, 1) == 0, "deleting it again is no error");
	ok(tcont(0x501, 4) == 1 && queue(1, 0, 0, 1, 0, 0) == 0 && reg(QMAP + 1) == 1U,
	   "the released block is free for the next T-CONT");
	ok(queue(0, 0, 0, 1, 0, 0) != 0, "T-CONT 0 released its block: no queue until cmd 21");
	ok(tcont(0x500, 2) == 0 && queue(0, 0, 0, 1, 0, 0) == 0 && reg(QMAP + 0) == (1U << 4),
	   "cmd 21 again: a new block, after T-CONT 1's");

	puts("T-CONT index is the Alloc-ID's CAM row:");
	fresh();
	memset(cam, 0, sizeof cam);
	cam[0] = 700;	/* assigned first by PLOAM */
	cam[1] = 600;
	cam[16] = 26;	/* the ONU-ID, the OMCC */
	cam[20] = 900;
	odi_sw_host_alloc_row = cam_row;
	ok(tcont(600, 1) == 1, "Alloc-ID 600 in row 1: index 1, whatever the order of cmd 21");
	ok(tcont(700, 1) == 0, "Alloc-ID 700 in row 0: index 0");
	ok(tcont(26, 1) == -1, "the OMCC's Alloc-ID: refused");
	ok(tcont(900, 1) == -1, "a row with no scheduler (20): refused");
	ok(tcont(901, 1) == -1, "no row at all: refused");
	odi_sw_host_alloc_row = NULL;

	puts("downstream delete is refused, not taken for a create:");
	fresh();
	{
		struct omci_gemflow g;
		unsigned int n = odi_mock.log_n;

		memset(&g, 0, sizeof g);
		g.flow_id = 1;
		g.gem_port = 300;
		g.dir = OMCI_GEMFLOW_DS;
		rc = odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g);
		ok(rc != 0 && odi_mock.log_n == n, "refused, nothing written");
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
