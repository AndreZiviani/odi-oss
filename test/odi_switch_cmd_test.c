/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_cmd_test.c -- replays the 91 command brackets of the boot5
 * OMCI provisioning capture, in trace order, through
 * odi_switch_cmd(), and dumps the resulting
 * write log in ring-dump format so odi_switch_cmd_test.sh can run
 * tools/regtrace/compare.py directly against boot5 itself.
 *
 * The 91-call table below -- cmd number, order, and instance count, plus
 * every per-instance argument-struct field -- was built from a direct
 * decode.py run over the boot5 capture (the reference this test compares
 * against): `python3 tools/regtrace/decode.py <regmap>
 * boot5.txt` (no --summary), read bracket by bracket.
 *
 * cmd 51 (activeBdgConn): the CF and VLAN rows are DERIVED from the
 * omci_bdgconn argument (odi_switch_cmd.c, cmd_active_bdg_conn), so this
 * test sends the twelve descriptors omcid actually sent on isp1 -- six
 * services, each twice (boot5 omcid log) -- built by the same generators
 * (bdgconn_rules.h mirrors src/omci/respond/apply_bridge.c). It used to send
 * twelve zeroed descriptors, which only worked while cmd 51 replayed the
 * twelve captured brackets by call number. cmd 25 likewise carries the
 * flow ids omcid allocated (0..5 downstream, 0..4 upstream, same log):
 * the tables are indexed by them now.
 *
 * cmd 21, 23 and 25 upstream carry what omcid sends (the queue count, the
 * ordinal, the weight), and every word of their brackets is computed from
 * it (docs/SWITCH.md#upstream-pon-queues), on the PON queue block as the
 * boot replays leave it (odi_mock_ponq_boot_state()). Nothing is replayed
 * by T-CONT or flow ordinal any more.
 *
 * Usage: odi_switch_cmd_test <out-file>. Writes the whole 91-bracket replay
 * as one ring dump.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_gemflow.h"
#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_bdgconn.h"
#include "bdgconn_rules.h"
#include "isp1_boot5_cmds.h"

static void call(uint32_t cmd, void *buf, uint32_t len)
{
	odi_mock_mark(cmd);
	(void)odi_switch_cmd(cmd, buf, len);
	odi_mock_mark(0x80000000U | cmd);
}

int main(int argc, char **argv)
{
	FILE *f;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <out-file>\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "w");
	if (!f) {
		fprintf(stderr, "odi_switch_cmd_test: cannot open %s\n", argv[1]);
		return 1;
	}

	odi_mock_reset();
	odi_switch_cmd_reset_state();
	/* The PON queue block as the boot replays leave it: cmd 23 and 25
	 * change one field of it at a time.
	 */
	odi_mock_ponq_boot_state();
	/* And the flood masks: cmd 64 adds one port to what is there. */
	odi_mock_flood_boot_state();

	/* The brackets are the stock image, which leaves PAUSE toward the
	 * host to the auto-negotiation: its cmd 32 writes PORT_FORCE_SELECT 0.
	 * Ours keeps TX_PAUSE forced there by default;
	 * odi_host_pause_test.c checks that.
	 */
	odi_sw_host_pause = true;
	/* The capture is the stock driver's, which writes the one word of a
	 * downstream queue and no schedule, and gives every downstream row the
	 * port default priority; the schedule is compared in
	 * odi_switch_dsq_test.c and in the boot golden, the priority of the
	 * rows in odi_switch_dssteer_test.c. */
	odi_sw_ds_sched = false;
	odi_sw_ds_steer = false;

	/* ODI_SWITCH_CMD_TEST_INIT_PLATFORM=1 runs the platform init first,
	 * as a boot does: odi_switch_init_platform_test.sh uses it to check
	 * that those writes (before any mark) leave the 91 brackets alone.
	 */
	if (getenv("ODI_SWITCH_CMD_TEST_INIT_PLATFORM"))
		odi_switch_init_platform();

	isp1_boot5_replay(call);

	fprintf(f, "# odi_switch_cmd replay, 91 brackets\n");
	odi_mock_dump(f);
	fclose(f);
	return 0;
}
