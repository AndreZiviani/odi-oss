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
 * boot5.txt` (no --summary), read bracket by bracket. That direct read is
 * also what corrected two fields an earlier boot3-based pass got only
 * approximately right for boot5:
 *
 *   - cmd 23 per-queue scheduling-queue value (odi_sw_qos_sched_
 *     set `value` parameter, PONQ_COUNT_MASK+190..194): boot5 writes 1<<n
 *     (1, 2, 4, 8, 0x10 for instances 9-13), not the constant 0 the
 *     odi_switch_dal.h leaf comment records for boot3 -- the two captures
 *     genuinely differ here, boot5 is what this test replays against, and
 *     the leaf itself is unchanged (it already takes the value as a
 *     parameter).
 *   - cmd 25 US PONQ_COUNT_MASK+235/+20/+21 words: undecoded fields
 *     (odi_switch_dal.h own leaf comment), replayed here from boot5 by
 *     GEM slot ordinal rather than computed, same posture as odi_switch_
 *     cmd.c internal lookup tables.
 *
 * cmd 51 (activeBdgConn): the CF and VLAN rows are DERIVED from the
 * omci_bdgconn argument (odi_switch_cmd.c, cmd_active_bdg_conn), so this
 * test sends the twelve descriptors omcid actually sent on isp1 -- six
 * services, each twice (boot5 omcid log) -- built by the same generators
 * (bdgconn_rules.h mirrors src/omci/respond/apply.c). It used to send
 * twelve zeroed descriptors, which only worked while cmd 51 replayed the
 * twelve captured brackets by call number. cmd 25 likewise carries the
 * flow ids omcid allocated (0..5 downstream, 0..4 upstream, same log):
 * the tables are indexed by them now.
 *
 * cmd 23 instances #10-13 used to be an expected DIFF (a write-order swap
 * between PONQ_COUNT_MASK+208 and +212/+213); odi_sw_ponmac_queue_add_ext()
 * now tracks it, see odi_switch_cmd_test.sh.
 *
 * Usage: odi_switch_cmd_test <out-file>. Writes the whole 91-bracket replay
 * as one ring dump.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"

#include "../src/omci/omci_gemflow.h"
#include "../src/omci/omci_bdgconn.h"
#include "bdgconn_rules.h"

static void call(uint32_t cmd, void *buf, uint32_t len)
{
	odi_mock_mark(cmd);
	(void)odi_switch_cmd(cmd, buf, len);
	odi_mock_mark(0x80000000U | cmd);
}

/* cmd 25, DS side: table idx 0..5, GEM Port-ID, in creation order
 * (boot5 decode confirms the same six rows).
 */
static const uint32_t ds_port_id[6] = { 0xfff, 0x59a, 0x69a, 0x71a, 0x79a, 0x61a };

/* cmd 25, US side: the four data GEM ports, same order minus the reserved
 * 0xfff OMCI port (US is downstream-mirror-only for data GEMs).
 */
static const uint32_t us_port_id[5] = { 0x59a, 0x69a, 0x71a, 0x79a, 0x61a };

int main(int argc, char **argv)
{
	FILE *f;
	int i;

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

	/* ODI_SWITCH_CMD_TEST_INIT_PLATFORM=1 runs odi_switch_init_platform()
	 * with every item enabled (odi_switch_dal.c, "odi_switch platform
	 * init") before the replay below -- odi_switch.c no longer calls
	 * this from module init at all (s4's boot hang; the call was moved
	 * out of module init to make it safe, and a real boot only reaches
	 * it via a deliberate /proc/odi_omci write or the first OP_CMD), so
	 * this env var stands
	 * in for that trigger here. Used by odi_switch_init_platform_test.sh
	 * to confirm the platform-init writes (which happen before any mark)
	 * do not disturb the 91-bracket comparison. Off by default so this
	 * file's own odi_switch_cmd_test.sh behaviour is unchanged.
	 */
	if (getenv("ODI_SWITCH_CMD_TEST_INIT_PLATFORM"))
		odi_switch_init_platform(ODI_SWITCH_INIT_PLATFORM_ITEM_ALL);

	/* 1: cmd 13 -- early GET-phase O5/O8 poll. */
	call(13, NULL, 0);

	/* 2: cmd 62 -- SetMacAgeTime, bridge init default. */
	{
		uint32_t v = 3000;

		call(62, &v, sizeof v);
	}

	/* 3: cmd 30 -- SetPortAutoNegoAbility, single UNI port. */
	call(30, NULL, 0);

	/* 4: cmd 32 -- SetPortState, admin up. */
	call(32, NULL, 0);

	/* 5: cmd 38 -- SetPortPhyPwrDown, redundant set (0 writes). */
	call(38, NULL, 0);

	/* 6: cmd 64 -- SetFloodingPortMask, port 2, enable. */
	{
		struct omci_flood fl = { .sel = 0, .enable = 1, .portMask = 1u << 2 };

		call(64, &fl, sizeof fl);
	}

	/* 7: cmd 25, DS instance 1 -- the OMCI/broadcast GEM (idx 0, 0xfff). */
	{
		struct omci_gemflow g;

		memset(&g, 0, sizeof g);
		g.flow_id = 0;
		g.gem_port = ds_port_id[0];
		g.dir = OMCI_GEMFLOW_DS;
		call(25, &g, sizeof g);
	}

	/* 8: cmd 26 -- SetDsBcGemFlow, ACL-only (0 writes). */
	call(26, NULL, 0);

	/* 9-13: cmd 25, DS instances 2-6 -- the four data GEM ports. */
	for (i = 1; i < 6; i++) {
		struct omci_gemflow g;

		memset(&g, 0, sizeof g);
		g.flow_id = (uint32_t)i;
		g.gem_port = ds_port_id[i];
		g.dir = OMCI_GEMFLOW_DS;
		call(25, &g, sizeof g);
	}

	/* 14-21: cmd 23, instances 1-8 -- pre-T-CONT flow-control baseline. */
	for (i = 0; i < 8; i++) {
		struct omci_priq pq;

		memset(&pq, 0, sizeof pq);
		call(23, &pq, sizeof pq);
	}

	/* 22: cmd 13 -- state poll. */
	call(13, NULL, 0);

	/* 23-27: cmd 10, instances 1-5 -- SFP DDM reads for one GetTransceiverStatus. */
	for (i = 0; i < 5; i++)
		call(10, NULL, 0);

	/* 28: cmd 13 -- state poll. */
	call(13, NULL, 0);

	/* 29-38: cmd 21 then cmd 23 (full program), 5 times -- one pair per T-CONT. */
	for (i = 0; i < 5; i++) {
		struct omci_tcont t;
		struct omci_priq pq;

		memset(&t, 0, sizeof t);
		t.alloc_id = (uint32_t)(0x100 + i); /* isp1 real alloc-IDs are not
						     * visible in this register-only
						     * capture (cmd 21 writes zero
						     * registers) -- any distinct value
						     * per T-CONT exercises the
						     * allocator identically.
						     */
		call(21, &t, sizeof t);

		memset(&pq, 0, sizeof pq);
		call(23, &pq, sizeof pq);
	}

	/* 39-43: cmd 25, US instances 7-11 -- the four data GEM ports, upstream mirror. */
	for (i = 0; i < 5; i++) {
		struct omci_gemflow g;

		memset(&g, 0, sizeof g);
		g.flow_id = (uint32_t)i;
		g.gem_port = us_port_id[i];
		g.dir = OMCI_GEMFLOW_US;
		call(25, &g, sizeof g);
	}

	/* 44-55: cmd 51, instances 1-12 -- bridge connection activation.
	 * The six services omcid built on isp1 (boot5 omcid log, lines
	 * 701-712: service_id, GEM, flow ids, uni_mask), each sent twice: first
	 * from the UNI ingress (uni_mask 1), then from the VEIP, which merges
	 * into the same service (uni_mask 5). The VLAN rules are what
	 * bdgconn_rebuild() generates for isp1 MIB (stock CLI dump conn, the
	 * same six): VID 11 is VLAN_MANU_TAG_VID, the untagged handoff.
	 */
	{
		static const struct {
			int serv;
			uint32_t dir, us, ds;
			int kind;	/* 0 manual, 1 VID filter, 2 multicast */
			unsigned vid;
			int pbit;
		} svc[6] = {
			{ 0, OMCI_DIR_BI, 4, 5, 0, 11, -1 },	/* GEM 1562 */
			{ 1, OMCI_DIR_BI, 3, 4, 1, 14, -1 },	/* GEM 1946 */
			{ 2, OMCI_DIR_BI, 2, 3, 1, 13, 4 },	/* GEM 1818 */
			{ 3, OMCI_DIR_BI, 1, 2, 1, 12, 5 },	/* GEM 1690 */
			{ 4, OMCI_DIR_BI, 0, 1, 1, 10, -1 },	/* GEM 1434 */
			{ 5, OMCI_DIR_DS, 0, 0, 2, 11, -1 },	/* GEM 4095 */
		};

		for (i = 0; i < 12; i++) {
			const int k = i / 2;
			struct omci_bdgconn b;
			struct omci_vlan_oper vr;

			if (svc[k].kind == 1)
				bdg_gen_vid_filter(&vr, svc[k].vid, svc[k].pbit);
			else
				bdg_gen_manual(&vr, (int)svc[k].vid, 0, svc[k].kind == 2);
			bdg_conn(&b, svc[k].serv, svc[k].dir, (i & 1) ? 5u : 1u,
				 svc[k].us, svc[k].ds, &vr);
			call(51, &b, sizeof b);
		}
	}

	/* 56-91: cmd 13 x36 -- the closing O5/O8 poll run. */
	for (i = 0; i < 36; i++)
		call(13, NULL, 0);

	fprintf(f, "# odi_switch_cmd replay, 91 brackets\n");
	odi_mock_dump(f);
	fclose(f);
	return 0;
}
