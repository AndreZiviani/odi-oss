/* odi_switch_flows_test.c -- OMCI_FLOWS_CMD (src/omci/omci_flows.h), the
 * read-only readback of what cmd 25 programmed. Checks that it returns the
 * rows and slots cmd 25 was given, at their flow ids, that the OMCI channel (GEM
 * 0xfff) is a DS row with the OMCI traffic type and no US slot, that the
 * counters stay exact past OMCI_FLOWS_MAX, that a short buffer is refused,
 * and that the readback itself writes no register. Then the upstream queue
 * map cmd 25 writes (PON_SID2QID): each flow on its own T-CONT's queue,
 * past five flows too (issue #42). Host-side, against the
 * same mock odi_switch_cmd_test.c uses. Part of `make test-host`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-56s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

static uint32_t reg(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

/* An upstream flow on queue 0 of T-CONT index tcont; the cmd 25 result. */
static int us(uint32_t flow, uint32_t port, uint32_t tcont)
{
	struct omci_gemflow g;

	memset(&g, 0, sizeof g);
	g.dir = OMCI_GEMFLOW_US;
	g.flow_id = flow;
	g.gem_port = port;
	g.tcont = tcont;
	g.ena = 1;
	return odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g);
}

/* T-CONTs 0..n-1 (Alloc-IDs 0x100.., rows in first-seen order on the
 * host), each with one queue, as omcid provisions them: cmd 21 asking
 * for one queue, then cmd 23 for ordinal 0. */
static void tconts(uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		struct omci_tcont t = { .index = 1, .alloc_id = 0x100 + i };
		struct omci_priq q;

		(void)odi_switch_cmd(OMCI_TCONT_CMD, &t, sizeof t);
		memset(&q, 0, sizeof q);
		q.owner = (uint16_t)t.index;
		q.weight = 1;
		q.dir = OMCI_GEMFLOW_US;
		(void)odi_switch_cmd(OMCI_PRIQ_CMD, &q, sizeof q);
	}
}

static void gem(uint32_t dir, uint32_t flow, uint32_t port)
{
	struct omci_gemflow g;

	memset(&g, 0, sizeof g);
	g.dir = dir;
	g.flow_id = flow;
	g.gem_port = port;
	g.ena = 1;
	(void)odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g);
}

/* The flow hook (odi_switch_cmd.h): what the GPON core is told. */
static int hook_set_n, hook_clear_n;
static int hook_us[16];
static uint32_t hook_idx[16], hook_gem[16];

static void hook_set(int us_dir, uint32_t idx, uint32_t gem_port)
{
	if (hook_set_n < 16) {
		hook_us[hook_set_n] = us_dir;
		hook_idx[hook_set_n] = idx;
		hook_gem[hook_set_n] = gem_port;
	}
	hook_set_n++;
}

static void hook_clear(void)
{
	hook_clear_n++;
}

static const struct odi_switch_flow_ops hook_ops = { .set = hook_set, .clear = hook_clear };

int main(void)
{
	struct omci_flows f;
	unsigned int before;
	int rc;

	odi_mock_reset();
	odi_switch_cmd_reset_state();
	puts("OMCI_FLOWS_CMD readback:");
	odi_mock_ponq_boot_state();

	memset(&f, 0xa5, sizeof f);
	ok(odi_switch_cmd(OMCI_FLOWS_CMD, &f, sizeof f) == 0 &&
	   f.ds_count == 0 && f.us_count == 0 && f.ds_gem[0] == 0 && f.us_gem[0] == 0,
	   "empty after reset, unused rows zeroed");

	tconts(1);
	gem(OMCI_GEMFLOW_DS, 0, 0xfff);
	gem(OMCI_GEMFLOW_US, 0, 0xfff);          /* OMCI channel: no US slot */
	gem(OMCI_GEMFLOW_DS, 1, 1100);
	gem(OMCI_GEMFLOW_US, 0, 1100);
	gem(OMCI_GEMFLOW_DS, 2, 1101);
	gem(OMCI_GEMFLOW_US, 1, 1101);

	before = odi_mock.log_n;
	rc = odi_switch_cmd(OMCI_FLOWS_CMD, &f, sizeof f);
	ok(rc == 0, "answers 0");
	ok(odi_mock.log_n == before, "writes no register");
	ok(f.ds_count == 3 && f.us_count == 2, "3 DS rows, 2 US slots");
	ok(f.ds_gem[0] == 0xfff && f.ds_cfg[0] == 3, "row 0: OMCI channel, type 3");
	ok(f.ds_gem[1] == 1100 && f.ds_cfg[1] == 2 && f.ds_gem[2] == 1101,
	   "rows 1-2: in creation order, type 2");
	ok(f.us_gem[0] == 1100 && f.us_gem[1] == 1101 && f.us_gem[2] == 0,
	   "US slots in creation order");

	for (int i = 0; i < 20; i++)
		gem(OMCI_GEMFLOW_DS, 3 + (uint32_t)i, 2000 + (uint32_t)i);
	rc = odi_switch_cmd(OMCI_FLOWS_CMD, &f, sizeof f);
	ok(rc == 0 && f.ds_count == 23 && f.ds_gem[OMCI_FLOWS_MAX - 1] == 2000 + 11,
	   "count exact past OMCI_FLOWS_MAX, first rows kept");

	/* The flow id is the row: a flow arriving out of order lands on its
	 * own row, not the next free one, and a gap reads GEM 0. */
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	tconts(1);
	gem(OMCI_GEMFLOW_DS, 3, 657);
	gem(OMCI_GEMFLOW_DS, 0, 0xfff);
	gem(OMCI_GEMFLOW_US, 2, 657);
	rc = odi_switch_cmd(OMCI_FLOWS_CMD, &f, sizeof f);
	ok(rc == 0 && f.ds_count == 4 && f.ds_gem[3] == 657 && f.ds_gem[0] == 0xfff &&
	   f.ds_gem[1] == 0 && f.ds_gem[2] == 0,
	   "DS rows indexed by the caller flow id");
	ok(f.us_count == 3 && f.us_gem[2] == 657 && f.us_gem[0] == 0,
	   "US slots indexed by the caller flow id");
	ok(odi_switch_cmd(OMCI_GEMFLOW_CMD, &(struct omci_gemflow){ .flow_id = ODI_SW_CMD_GEM_DS_MAX,
			  .gem_port = 1, .dir = OMCI_GEMFLOW_DS }, OMCI_GEMFLOW_LEN) != 0,
	   "a flow id past the table is refused");

	ok(odi_switch_cmd(OMCI_FLOWS_CMD, &f, OMCI_FLOWS_LEN - 1) == -1,
	   "short buffer refused");
	ok(OMCI_FLOWS_LEN == 188, "wire size 188 (fits NL_CMD_MAX_LEN 256)");

	/* PON_SID2QID: 7 bits a flow, four a word, 63 for a flow not mapped.
	 * Six flows, one per T-CONT (the MTS shape of issue #42): the sixth
	 * maps flow 5 and leaves flows 0-3 alone. Replaying flow 0's word for
	 * it, as the driver used to, unmapped flows 1-3. */
	puts("cmd 25 upstream queue map:");
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_mock_ponq_boot_state();
	tconts(6);
	for (uint32_t i = 0; i < 6; i++)
		rc = us(i, 269 + 128 * i, i);
	ok(rc == 0, "six flows on six T-CONTs accepted");
	ok(reg(ODI_SW_PONQ_COUNT_MASK(20)) == 0x00608080U,
	   "flows 0-3 -> queues 0-3 after the sixth flow");
	ok(reg(ODI_SW_PONQ_COUNT_MASK(21)) == (4U | 5U << 7 | 0x3fU << 14 | 0x3fU << 21),
	   "flow 4 -> queue 4, flow 5 -> queue 5, 6-7 unmapped");
	ok(reg(ODI_SW_PONQ_COUNT_MASK(37)) == 0x3fU, "PON_SIDVALID: six flows");
	/* 7904 - 5 * 150, 7744 - 5 * 150: the formula goes on past five. */
	ok(reg(ODI_SW_PONQ_COUNT_MASK(235)) == ((7154U << 16) | 6994U),
	   "global thresholds for six flows");

	/* The queue is the T-CONT's, not the flow id's. */
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_mock_ponq_boot_state();
	tconts(2);
	us(0, 300, 1);
	us(1, 301, 1);
	us(2, 302, 0);
	ok(reg(ODI_SW_PONQ_COUNT_MASK(20)) == (1U | 1U << 7 | 0U << 14 | 0x3fU << 21),
	   "two flows on T-CONT 1 share queue 1");
	ok(reg(ODI_SW_PONQ_COUNT_MASK(235)) == 0x1db41d14U,
	   "global thresholds by flows in use (three)");

	/* A flow on a queue its T-CONT does not have is refused. */
	{
		struct omci_gemflow g;
		unsigned int n = odi_mock.log_n;

		memset(&g, 0, sizeof g);
		g.dir = OMCI_GEMFLOW_US;
		g.flow_id = 3;
		g.gem_port = 303;
		g.queue = 1;
		g.ena = 1;
		ok(odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g) != 0 && odi_mock.log_n == n,
		   "a flow on queue 1 of a one-queue T-CONT is refused, nothing written");
		ok(us(4, 304, ODI_SW_PONQ_TCONTS) != 0 && odi_mock.log_n == n,
		   "a flow on a T-CONT past 15 is refused, nothing written");
	}

	/* The GPON core hears of each flow cmd 25 creates, and of the reset;
	 * a refused flow and the OMCI channel upstream (no slot) say nothing.
	 */
	puts("flow hook:");
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_switch_cmd_set_flow_ops(&hook_ops);
	tconts(3);
	gem(OMCI_GEMFLOW_DS, 0, 0xfff);
	gem(OMCI_GEMFLOW_US, 0, 0xfff);
	gem(OMCI_GEMFLOW_DS, 1, 1434);
	ok(us(4, 1562, 2) == 0, "an upstream flow is created");
	ok(us(5, 1563, ODI_SW_PONQ_TCONTS) != 0, "and one on a bad T-CONT is refused");
	gem(OMCI_GEMFLOW_DS, ODI_SW_CMD_GEM_DS_MAX, 1);
	ok(hook_set_n == 3, "three flows reported");
	ok(hook_us[0] == 0 && hook_idx[0] == 0 && hook_gem[0] == 0xfff, "DS 0, the OMCI channel");
	ok(hook_us[1] == 0 && hook_idx[1] == 1 && hook_gem[1] == 1434, "DS 1 on its GEM port");
	ok(hook_us[2] == 1 && hook_idx[2] == 4 && hook_gem[2] == 1562, "US 4 on its GEM port");
	ok(hook_clear_n == 0, "nothing cleared yet");
	odi_switch_cmd_reset_state();
	ok(hook_clear_n == 1, "the reset is reported");
	odi_switch_cmd_set_flow_ops(NULL);
	gem(OMCI_GEMFLOW_DS, 1, 1434);
	ok(hook_set_n == 3, "no hook, nobody told");

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
