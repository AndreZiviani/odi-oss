/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bdgconn_rules.h -- the bridge rules omcid sends with cmd 51, rebuilt for
 * the host tests. The three builders mirror the generators in
 * src/omci/respond/apply_bridge.c field for field (rule_init, gen_manual_vlan_rule,
 * gen_vid_filter_rule): those are static inside omcid and pull in its whole
 * MIB, so they are restated here rather than linked. If one of them
 * changes, the replay tests that use this header stop matching their
 * captures, which is the point.
 */
#ifndef BDGCONN_RULES_H
#define BDGCONN_RULES_H

#include <string.h>

#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_bdgconn.h"

static inline void bdg_rule_init(struct omci_bdgconn *r)
{
	memset(r, 0, sizeof(*r));
	r->vlan_op.filter.outer_mode = OMCI_TAGF_ANY;
	r->vlan_op.filter.inner_mode = OMCI_TAGF_ANY;
	r->vlan_op.outer_act.tag_op = OMCI_TAGOP_PASS;
	r->vlan_op.inner_act.tag_op = OMCI_TAGOP_PASS;
	r->vlan_op.out.out_tag.pri = OMCI_PRI_ANY;
	r->vlan_op.out.out_tag.vid = OMCI_VID_ANY;
}

/* The untagged handoff from VLAN_MANU_TAG_VID (isMc 0), or the downstream
 * multicast rule built from the same config (isMc 1).
 */
static inline void bdg_gen_manual(struct omci_vlan_oper *vr, int vid, int pri, int isMc)
{
	struct omci_bdgconn tmp;

	bdg_rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_EXTVLAN;
	vr->filter.outer_mode = OMCI_TAGF_UNTAGGED;
	vr->filter.inner_mode = isMc ? (OMCI_TAGF_VID | OMCI_TAGF_PRI) : OMCI_TAGF_UNTAGGED;
	vr->filter.outer.pri = 0;
	vr->filter.outer.vid = 0;
	vr->filter.outer.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.inner.pri = 0;
	vr->filter.inner.vid = isMc ? 0u : (uint32_t)vid;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->outer_act.tag_op = isMc ? OMCI_TAGOP_POP : OMCI_TAGOP_PASS;
	vr->outer_act.vid_op = OMCI_VIDOP_SET;
	vr->outer_act.pri_op = OMCI_PRIOP_SET;
	vr->inner_act.tag_op = isMc ? OMCI_TAGOP_POP : OMCI_TAGOP_PUSH;
	vr->inner_act.vid_op = OMCI_VIDOP_SET;
	vr->inner_act.pri_op = OMCI_PRIOP_SET;
	vr->inner_act.set_tag.pri = (uint32_t)pri;
	vr->inner_act.set_tag.vid = (uint32_t)vid;
	vr->inner_act.set_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	vr->out.is_default = 0;
	vr->out.is_mcast = isMc ? 1u : 0u;
	vr->out.tag_count = isMc ? 0u : 1u;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.ds_mode = 0;
	vr->out.ds_tag_op = 0;
	vr->out.out_tag.pri = isMc ? 0u : (uint32_t)pri;
	vr->out.out_tag.vid = isMc ? 0u : (uint32_t)vid;
	vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
}

/* A tagged service: FILTER VID, or VID+PBIT when pbit >= 0. */
static inline void bdg_gen_vid_filter(struct omci_vlan_oper *vr, unsigned vid, int pbit)
{
	struct omci_bdgconn tmp;

	bdg_rule_init(&tmp);
	*vr = tmp.vlan_op;
	vr->rule_gen = OMCI_VLAN_OPER_FILTER_SINGLETAG;
	vr->filter.outer_mode = OMCI_TAGF_ANY;
	vr->filter.inner_mode = OMCI_TAGF_VID | (pbit >= 0 ? OMCI_TAGF_PRI : 0);
	vr->filter.inner.pri = pbit >= 0 ? (uint32_t)pbit : 0;
	vr->filter.inner.vid = vid;
	vr->filter.inner.tpid = OMCI_FILTER_TPID_DO_NOT;
	vr->filter.ethertype = OMCI_ETHTYPE_NO_CARE;
	vr->outer_act.tag_op = OMCI_TAGOP_PASS;
	vr->outer_act.vid_op = OMCI_VIDOP_SET;
	vr->outer_act.pri_op = OMCI_PRIOP_SET;
	vr->inner_act.tag_op = OMCI_TAGOP_PASS;
	vr->inner_act.vid_op = OMCI_VIDOP_SET;
	vr->inner_act.pri_op = OMCI_PRIOP_SET;
	vr->inner_act.set_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
	vr->out.is_default = 0;
	vr->out.is_mcast = 0;
	vr->out.tag_count = 1;
	vr->out.tpid = OMCI_OUT_TPID_8100;
	vr->out.ds_mode = 0;
	vr->out.ds_tag_op = 0;
	vr->out.out_tag.pri = pbit >= 0 ? (uint32_t)pbit : OMCI_PRI_ANY;
	vr->out.out_tag.vid = vid;
	vr->out.out_tag.tpid = OMCI_TREAT_TPID_COPY_INNER;
}

/* The 160-byte descriptor as bdgconn_add() sends it. */
static inline void bdg_conn(struct omci_bdgconn *b, int service_id, uint32_t dir, uint32_t uni_mask,
			    uint32_t us_flow, uint32_t ds_flow, const struct omci_vlan_oper *vr)
{
	bdg_rule_init(b);
	b->vlan_op = *vr;
	b->in_use = 1;
	b->service_id = service_id;
	b->dir = dir;
	b->uni_mask = uni_mask;
	if (dir & OMCI_DIR_US) {
		b->us_flow = us_flow;
		b->us_dp_flow = us_flow;
	}
	if (dir & OMCI_DIR_DS)
		b->ds_flow = ds_flow;
}

#endif /* BDGCONN_RULES_H */
