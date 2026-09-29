/* SPDX-License-Identifier: GPL-2.0 */
/*
 * isp1_boot5_cmds.h -- the 91 driver commands of the ISP1 provisioning
 * capture (test/fixtures/isp1-260922-boot5.txt), in capture order, with
 * the arguments omcid sent. odi_switch_cmd_test.c compares them bracket
 * for bracket against the capture; boot_golden_test.c replays them as the
 * last phase of the ISP1 boot golden. `call` issues one command.
 *
 * Needs omci_gemflow.h, omci_bdgconn.h and bdgconn_rules.h included first.
 */
#ifndef ISP1_BOOT5_CMDS_H
#define ISP1_BOOT5_CMDS_H

/* cmd 25, DS side: table idx 0..5, GEM Port-ID, in creation order
 * (boot5 decode confirms the same six rows).
 */
static const uint32_t ds_port_id[6] = { 0xfff, 0x59a, 0x69a, 0x71a, 0x79a, 0x61a };

/* cmd 25, US side: the four data GEM ports, same order minus the reserved
 * 0xfff OMCI port (US is downstream-mirror-only for data GEMs).
 */
static const uint32_t us_port_id[5] = { 0x59a, 0x69a, 0x71a, 0x79a, 0x61a };

static void isp1_boot5_replay(void (*call)(uint32_t cmd, void *buf, uint32_t len))
{
	int i;

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

	/* 14-21: cmd 23, instances 1-8 -- the eight downstream priority
	 * queues, with the arguments omcid sends for them
	 * (test/fixtures/omci-drv-golden-isp1.txt): dir DS, switch port and
	 * priority 0, and the Weight of each class 277 row (WRR when it is 2
	 * or more).
	 */
	{
		static const uint16_t ds_weight[8] = { 1, 1, 0x1e, 0x18, 0x13, 0x0e, 0x09, 0x04 };

		for (i = 0; i < 8; i++) {
			struct omci_priq pq;

			memset(&pq, 0, sizeof pq);
			pq.weight = ds_weight[i];
			pq.wrr = ds_weight[i] >= 2;
			pq.dir = OMCI_GEMFLOW_DS;
			call(23, &pq, sizeof pq);
		}
	}

	/* 22: cmd 13 -- state poll. */
	call(13, NULL, 0);

	/* 23-27: cmd 10, instances 1-5 -- SFP DDM reads for one GetTransceiverStatus. */
	for (i = 0; i < 5; i++)
		call(10, NULL, 0);

	/* 28: cmd 13 -- state poll. */
	call(13, NULL, 0);

	/* 29-38: cmd 21 then cmd 23 (full program), 5 times -- one pair per
	 * T-CONT. The queue is the only one of its T-CONT (ordinal 0), on the
	 * T-CONT index cmd 21 handed back, with the Weight omcid sends for it
	 * (the same golden).
	 */
	for (i = 0; i < 5; i++) {
		static const uint16_t us_weight[5] = { 1, 0x1e, 0x18, 0x04, 0x04 };
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
		pq.index = 0;
		pq.owner = (uint16_t)t.index;
		pq.weight = us_weight[i];
		pq.wrr = us_weight[i] >= 2;
		pq.dir = OMCI_GEMFLOW_US;
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
}

#endif /* ISP1_BOOT5_CMDS_H */
