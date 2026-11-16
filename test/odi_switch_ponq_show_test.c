/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* odi_switch_ponq_show_test.c -- /proc/odi_gpon/ponq (odi_switch_ponq_snap()
 * and odi_switch_ponq_show()): after cmd 21, 23 and 25 provision two
 * T-CONTs, three queues and three upstream flows, the file shows each of
 * them as the registers hold them, next to what the commands recorded; a
 * flow deleted and a register changed behind the driver's back show up as
 * such; and the copy never reads DRN_CMD. Host-side, against the register
 * mock. Part of `make test-host`.
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

static char text[8192];
static size_t text_n;

static void put(void *ctx, const char *line)
{
	size_t n = strlen(line);

	(void)ctx;
	if (text_n + n < sizeof text) {
		memcpy(text + text_n, line, n + 1);
		text_n += n;
	}
}

static int drn_reads;

static void count_drn(uint32_t off)
{
	if (off == ODI_SW_PONQ_COUNT_MASK(15U))
		drn_reads++;
}

static void show(void)
{
	static struct odi_switch_ponq_snap snap;

	text_n = 0;
	text[0] = 0;
	drn_reads = 0;
	odi_mock_read_hook = count_drn;
	odi_switch_ponq_snap(&snap);
	odi_mock_read_hook = NULL;
	odi_switch_ponq_show(&snap, put, NULL);
}

static int has(const char *line)
{
	char want[128];

	snprintf(want, sizeof want, "%s\n", line);
	return strstr(text, want) != NULL;
}

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

int main(void)
{
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_sw_host_alloc_row = NULL;
	odi_mock_ponq_boot_state();

	puts("as the boot replays leave it:");
	show();
	ok(has("tcont 16 en 1 qmap 0x00000001 block -"), "the OMCC T-CONT 16, queue 64, nothing recorded");
	ok(has("queue 64 wfq 0 weight 1 cir 0 pir 262143"), "queue 64: strict, weight 1, no limit");
	ok(has("flow 64 valid 1 gem 0 queue 64 recorded 0"), "the OMCC flow 64 on queue 64");
	ok(!strstr(text, "tcont 0 "), "no data T-CONT");
	ok(!strstr(text, "flow 0 "), "no data flow: unmapped rows are not printed");
	ok(drn_reads == 0, "DRN_CMD not read");

	puts("two T-CONTs, three queues, three flows:");
	ok(tcont(0x200, 2) == 0, "T-CONT 0, two queues");
	ok(tcont(0x201, 1) == 1, "T-CONT 1, one queue");
	ok(queue(0, 0, 0, 1, 0, 0) == 0, "T-CONT 0 ordinal 0, strict, no rate");
	ok(queue(0, 1, 1, 30, 100, 200) == 0, "T-CONT 0 ordinal 1, WFQ 30, CIR 100 PIR 200");
	ok(queue(1, 0, 1, 4, 0, 0) == 0, "T-CONT 1 ordinal 0, WFQ 4");
	ok(flow(0, 1000, 0, 0, 1) == 0, "flow 0: GEM 1000 on T-CONT 0 ordinal 0");
	ok(flow(1, 1001, 0, 1, 1) == 0, "flow 1: GEM 1001 on T-CONT 0 ordinal 1");
	ok(flow(2, 1002, 1, 0, 1) == 0, "flow 2: GEM 1002 on T-CONT 1 ordinal 0");
	show();
	ok(has("tcont 0 en 1 qmap 0x00000003 block 0+2"), "T-CONT 0: logical 0-1, block 0+2");
	ok(has("tcont 1 en 1 qmap 0x00000004 block 2+1"), "T-CONT 1: logical 2, block 2+1");
	ok(has("queue 0 wfq 0 weight 1 cir 0 pir 262143"), "queue 0: strict, no limit");
	ok(has("queue 1 wfq 1 weight 30 cir 99 pir 199"), "queue 1: WFQ 30, rates one less than asked");
	ok(has("queue 2 wfq 1 weight 4 cir 0 pir 262143"), "queue 2: WFQ 4");
	ok(has("flow 0 valid 1 gem 1000 queue 0 recorded 1"), "flow 0");
	ok(has("flow 1 valid 1 gem 1001 queue 1 recorded 1"), "flow 1");
	ok(has("flow 2 valid 1 gem 1002 queue 2 recorded 1"), "flow 2");
	ok(drn_reads == 0, "DRN_CMD not read");

	puts("a flow deleted, and a register changed behind the driver:");
	ok(flow(2, 0, 0, 0, 0) == 0, "flow 2 deleted");
	odi_mock.regs[odi_mock_slot(ODI_SW_US_GEM_PORT_MAP(1))] = 999U;
	show();
	ok(!strstr(text, "flow 2 "), "flow 2: neither valid nor recorded, not printed");
	ok(has("flow 1 valid 1 gem 999 queue 1 recorded 1"), "flow 1 shows the register, not the record");

	puts("a T-CONT of the second group:");
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_mock_ponq_boot_state();
	{
		int i;

		for (i = 0; i < 8; i++)
			ok(tcont(0x300 + (uint32_t)i, 1) == i, "a T-CONT of the first group");
	}
	ok(tcont(0x308, 1) == 8, "T-CONT 8");
	ok(queue(8, 0, 0, 1, 0, 0) == 0, "its queue");
	show();
	ok(has("tcont 8 en 1 qmap 0x00000001 block 0+1"), "T-CONT 8: logical 0 of the second group");
	ok(has("queue 32 wfq 0 weight 1 cir 0 pir 262143"), "physical queue 32");
	ok(!strstr(text, "tcont 0 en 1"), "T-CONT 0 has a block but no queue: not enabled");
	ok(has("tcont 0 en 0 qmap 0x00000000 block 0+1"), "T-CONT 0 printed for its block");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
