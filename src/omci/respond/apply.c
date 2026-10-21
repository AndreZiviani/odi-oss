/* What a Create, a Set or a Delete of a managed entity does to the
 * hardware: the dispatch by class, the delete, and the MIB reset. The work
 * per class is in apply_qos.c (T-CONTs, queues, GEM flows), apply_uni.c
 * (the UNI, its bridge ports, rate limits, flooding, DSCP) and
 * apply_bridge.c (the bridge connections); drv.c reaches the driver. See
 * omcid.h, and src/omci/README.md for what each handler reproduces of the
 * stock stack.
 */
#include "omcid.h"

int apply_hw;           /* -a: actually program the switch */

/* Whether the ONU created this instance itself (omci_autonomous[]): the
 * UNIs, the VEIP and the IP host, which have a row in the store only if the
 * OLT set something on one. */
int instance_is_autonomous(uint16_t cls, uint16_t inst)
{
	for (unsigned i = 0; i < aut_count; i++)
		if (aut[i].classId == cls && aut[i].inst == inst)
			return 1;
	return 0;
}

/* Everything a MIB reset throws away: the rows, and the state that
 * describes them (the flow tables, the T-CONT map, the queue plan, the
 * rate limiter slots, the broadcast flow). Left in place, the T-CONT map
 * (32 slots, no eviction) filled up over re-rangings until every GEM flow
 * naming a T-CONT was skipped. Both reset paths, the OMCI one and the CLI
 * one, call this. */
void mib_reset_all(void)
{
	for (int i = 0; i < MIB_ROWS; i++) {
		if (mib[i].used)
			tbl_free(&mib[i]);
		mib[i].used = 0;
	}
	qos_reset();
	uni_reset();
	alarm_mib_reset();
	mib_data_sync = 0;
	retx_clear();
	upl_total = 0;
	/* The bridge connections were built from the MIB just thrown away.
	 * The stock reset clears the datapath with the MIB; here the rebuild
	 * that follows (one quiet second, respond/main.c) deactivates every
	 * one of them and builds none from the empty MIB, so an OLT that
	 * provisions fewer services after the reset, or none, does not leave
	 * the old ones forwarding. */
	conn_dirty = 1;
	mcast_dirty = 1;
	/* A reset MIB and a snapshot of the one before it must never both be
	 * on disk: a respawn right after this reset, before the OLT sends
	 * anything to snapshot again, must fall back to re-registration, not
	 * resume into a MIB the OLT just discarded. */
	snapshot_invalidate();
}

/* What a Delete does to hardware, which for most classes is nothing. Called
 * before mib_del(), so the row is still readable. */
void apply_delete(const struct omci_class *c, uint16_t inst)
{
	if (c && (c->classId == OMCI_ME_TCONT ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  (c->classId == OMCI_ME_TRAFFIC_SCHEDULER &&
		   us_scheduler_referenced(inst)) ||
		  (c->classId == OMCI_ME_PRIORITY_QUEUE &&
		   us_queue_referenced(inst)) ||
		  (c->classId == OMCI_ME_TRAFFIC_DESCRIPTOR &&
		   us_td_referenced(inst))))
		qos_dirty = 1;
	if (c && (c->classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA ||
		  c->classId == OMCI_ME_VLAN_TAGGING_FILTER_DATA ||
		  c->classId == OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA ||
		  c->classId == OMCI_ME_GEM_IW_TP ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  c->classId == OMCI_ME_MCAST_GEM_IW_TP))
		conn_dirty = 1;
	if (!c)
		return;
	if (mcast_policy_class(c->classId))
		mcast_dirty = 1;
	if (c->classId == OMCI_ME_DOT1_RATE_LIMITER) {
		const struct mib_row *r = mib_find(OMCI_ME_DOT1_RATE_LIMITER, inst);

		if (r)
			dot1rl_apply(c, inst, r, 1, 1);
		return;
	}
	if (c->classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA) {
		delete_mbpcd(c, inst);
		return;
	}
	if (c->classId == OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE)
		delete_mbsp(inst);
	if (c->classId == OMCI_ME_MCAST_GEM_IW_TP)
		mcast_flag_refresh(inst);
}

/* Class 131, OLT-G: a Set that carries ToDInfo sends its fourteen bytes
 * straight to command 70. An OLT distributes time of day this way,
 * unsolicited, on a working line. */
static void apply_olt_g(const struct omci_class *c, const struct mib_row *r,
			uint16_t mask)
{
	unsigned k;
	int off, rc;

	k = 4;                           /* ToDInfo */
	if (!(mask & (1u << (16 - k))))
		return;
	off = attr_offset(c, k);
	if (off < 0 || off + 14 > MIB_ROW_MAX)
		return;
	out_fmt("   tod info %02x%02x%02x%02x ...\n",
		(long)r->data[off], (long)r->data[off + 1],
		(long)r->data[off + 2], (long)r->data[off + 3]);
	if (!apply_hw)
		return;
	{
		uint8_t tod[14];

		for (int i = 0; i < 14; i++)
			tod[i] = r->data[off + i];
		rc = omci_setTodInfo(tod);
	}
	out_fmt("   [hw] %s -> %d\n", "tod info", (long)rc);
}

/* Class 263, ANI-G: the GEM block length, attribute 3 (G.988 9.2.1; index
 * 3 of attr_Anig, after EntityID, SRInd and NumOfTcont). */
static void apply_ani_g(const struct omci_class *c, const struct mib_row *r,
			uint16_t mask)
{
	unsigned k;
	int rc;

	/* Signal fail and signal degrade thresholds (6 and 7) go to the
	 * kernel, which raises the alarms against them: driver command 11,
	 * as the stock AnigDrvCfg sends it on a Set of either. */
	anig_signal_thresholds((mask & (1u << (16 - 6))) ? (int)row_u32(r, c, 6) : -1,
			       (mask & (1u << (16 - 7))) ? (int)row_u32(r, c, 7) : -1);
	if (!apply_hw)
		return;
	k = 3;                           /* GEM block length */
	if (!(mask & (1u << (16 - k))))
		return;
	rc = omci_setGemBlkLen(row_u32(r, c, k));
	out_fmt("   [hw] %s -> %d\n", "gem block length", (long)rc);
}

/* The tables (flow ids, the T-CONT map, the services) are kept with or
 * without -a: they are ours, and they are what lets a dry run say what
 * would be programmed. Only the driver calls are gated on apply_hw, one by
 * one, in the handlers. */
void apply_entity(const struct omci_class *c, uint16_t inst,
		  const struct mib_row *r, uint16_t mask, int creating)
{
	if (c && (c->classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA ||
		  c->classId == OMCI_ME_VLAN_TAGGING_FILTER_DATA ||
		  c->classId == OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE ||
		  c->classId == OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA ||
		  c->classId == OMCI_ME_GEM_IW_TP ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  c->classId == OMCI_ME_MCAST_GEM_IW_TP))
		conn_dirty = 1;
	if (c && mcast_policy_class(c->classId))
		mcast_dirty = 1;
	/* A traffic scheduler (class 278) matters only when a planned
	 * upstream queue names it, and then only its Policy (attribute 3:
	 * strict or WRR, us_queue_decode()), on a create or a Set that
	 * carries it. Any Set used to rebuild the whole upstream. */
	if (c && (c->classId == OMCI_ME_TCONT ||
		  c->classId == OMCI_ME_GEM_PORT_CTP ||
		  (c->classId == OMCI_ME_TRAFFIC_SCHEDULER &&
		   (creating || (mask & (1u << (16 - 3)))) &&
		   us_scheduler_referenced(inst)) ||
		  (c->classId == OMCI_ME_PRIORITY_QUEUE &&
		   us_queue_referenced(inst)) ||
		  (c->classId == OMCI_ME_TRAFFIC_DESCRIPTOR &&
		   us_td_referenced(inst))))
		qos_dirty = 1;

	switch (c->classId) {
	case OMCI_ME_TCONT:
		/* Applied by us_qos_rebuild(), once the queue graph settles. */
		return;
	case OMCI_ME_PRIORITY_QUEUE:
		apply_priq(inst);
		return;
	case OMCI_ME_GEM_PORT_CTP:
		apply_gem_ctp(c, r);
		return;
	case OMCI_ME_PPTP_ETH_UNI:
		apply_pptp_uni(c, inst, r, mask, creating);
		return;
	case OMCI_ME_MCAST_GEM_IW_TP:
		/* Its address tables decide the multicast flag of the CTP's
		 * downstream flow (apply_qos.c, ctp_mcast_rows). */
		mcast_flag_refresh(-1);
		bc_gem_update();
		return;
	case OMCI_ME_GEM_IW_TP:
		/* A step on the way to the broadcast flow; the OLT creates
		 * these and class 47 in no fixed order. */
		bc_gem_update();
		return;
	case OMCI_ME_DOT1_RATE_LIMITER:
		/* A create sends all three rates, a Set only the ones it moved. */
		dot1rl_apply(c, inst, r, 0, creating);
		return;
	case OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA:
		apply_mbpcd(c, inst, r, mask, creating);
		return;
	case OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA:
		apply_ext_vlan_dscp(c, inst, r, mask);
		return;
	case OMCI_ME_DOT1P_MAPPER_SERVICE_PROFILE:
		apply_mapper_dscp(c, inst, r, mask);
		return;
	case OMCI_ME_OLT_G:
		apply_olt_g(c, r, mask);
		return;
	case OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE:
		apply_mbsp(c, inst, r, mask, creating);
		return;
	case OMCI_ME_ANI_G:
		apply_ani_g(c, r, mask);
		return;
	default:
		return;
	}
}
