/* odi_switch_flows_test.c -- OMCI_FLOWS_CMD (src/omci/omci_flows.h), the
 * read-only readback of what cmd 25 programmed. Checks that it returns the
 * rows and slots cmd 25 was given, at their flow ids, that the OMCI channel (GEM
 * 0xfff) is a DS row with the OMCI traffic type and no US slot, that the
 * counters stay exact past OMCI_FLOWS_MAX, that a short buffer is refused,
 * and that the readback itself writes no register. Host-side, against the
 * same mock odi_switch_cmd_test.c uses. Part of `make test-host`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-56s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

static void gem(uint32_t dir, uint32_t flow, uint32_t port)
{
	struct omci_gemflow g;

	memset(&g, 0, sizeof g);
	g.dir = dir;
	g.flow_id = flow;
	g.gem_port = port;
	(void)odi_switch_cmd(OMCI_GEMFLOW_CMD, &g, sizeof g);
}

int main(void)
{
	struct omci_flows f;
	unsigned int before;
	int rc;

	odi_mock_reset();
	odi_switch_cmd_reset_state();
	puts("OMCI_FLOWS_CMD readback:");

	memset(&f, 0xa5, sizeof f);
	ok(odi_switch_cmd(OMCI_FLOWS_CMD, &f, sizeof f) == 0 &&
	   f.ds_count == 0 && f.us_count == 0 && f.ds_gem[0] == 0 && f.us_gem[0] == 0,
	   "empty after reset, unused rows zeroed");

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

	printf("%s (%d failures)\n", failures ? "FAILED" : "all ok", failures);
	return failures != 0;
}
