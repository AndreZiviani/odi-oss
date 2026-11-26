/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Serve the actual native CLI protocol from synthetic service slots only.
 * No device is opened; any driver call fails the fixture. */
#include "omcid.h"

static int calls;

int __wrap_omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)cmd; (void)buf; (void)len;
	calls++;
	return -1;
}

int main(int argc, char **argv)
{
	struct omci_bdgconn *r = &servtab[1];

	apply_hw = argc < 2 || !str_eq(argv[1], "dry");
	if (argc < 2 || !str_eq(argv[1], "empty")) {
		r->in_use = 1;
		r->service_id = 1;
		r->dir = OMCI_DIR_BI;
		r->uni_mask = 1;
		r->us_flow = 1;
		r->ds_flow = 2;
		r->vlan_op.rule_gen = OMCI_VLAN_OPER_FILTER_SINGLETAG;
		r->vlan_op.filter.outer_mode = OMCI_TAGF_ANY;
		r->vlan_op.filter.inner_mode = OMCI_TAGF_VID;
		r->vlan_op.filter.inner.vid = 37;
		r->vlan_op.out.out_tag.vid = 37;
		r->vlan_op.outer_act.tag_op = OMCI_TAGOP_PASS;
		r->vlan_op.inner_act.tag_op = OMCI_TAGOP_PASS;
		flow_us[1].used = flow_ds[2].used = 1;
		flow_us[1].port = flow_ds[2].port = 20;

		/* Transparent QinQ: neither VID is stated by the installed rule. */
		servtab[2] = *r;
		servtab[2].service_id = 2;
		gen_no_vlan_filter_rule(&servtab[2].vlan_op);
		/* A stale, uninstalled descriptor must not contribute its VID. */
		servtab[3] = *r;
		servtab[3].in_use = 0;
		servtab[3].vlan_op.filter.inner.vid = 123;
		servtab[3].vlan_op.out.out_tag.vid = 123;
		servtab[4] = *r;
		servtab[4].in_use = SERV_PENDING;
		servtab[4].service_id = 4;

		/* One direction, with a known outer VID and an unknown inner VID. */
		servtab[5] = *r;
		servtab[5].service_id = 5;
		servtab[5].dir = OMCI_DIR_DS;
		servtab[5].vlan_op.rule_gen = OMCI_VLAN_OPER_EXTVLAN_GEN;
		servtab[5].vlan_op.filter.outer_mode = OMCI_TAGF_TAGGED | OMCI_TAGF_VID;
		servtab[5].vlan_op.filter.outer.vid = 81;
		servtab[5].vlan_op.filter.inner_mode = OMCI_TAGF_TAGGED;
		servtab[5].vlan_op.outer_act.tag_op = OMCI_TAGOP_POP;
	}
	cliq = mq_open(OMCLI_KEY, 1);
	if (cliq < 0)
		return 1;
	out("ready\n");
	out_flush();
	for (int i = 0; i < 5000; i++) {
		if (cli_poll()) {
			mq_remove(cliq);
			return calls ? 2 : 0;
		}
		sys_nanosleep(0, 2000000);
	}
	mq_remove(cliq);
	return 3;
}
