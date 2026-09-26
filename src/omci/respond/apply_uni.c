/* The UNI side: the PPTP Ethernet UNI (class 11), the MAC bridge port
 * (class 47) and service profile (class 45) work on a UNI port, the dot1
 * rate limiter (class 298), flooding, and the DSCP-to-P-bit map of classes
 * 171 and 130. See omcid.h.
 */
#include "omcid.h"

/* AutoDetectCfg (class 11 attribute 3) to the ability word of the
 * auto-negotiation command. */
static int ethuni_auto_ability(uint32_t cfg, uint32_t *ability)
{
	switch (cfg) {
	case 0x00: *ability = 0xfc000000u; return 0;
	case 0x01: *ability = 0x40000000u; return 0;
	case 0x02: *ability = 0x10000000u; return 0;
	case 0x03: *ability = 0x04000000u; return 0;
	case 0x04: *ability = 0x54000000u; return 0;
	case 0x10: *ability = 0xc0000000u; return 0;
	case 0x11: *ability = 0x80000000u; return 0;
	case 0x12: *ability = 0x20000000u; return 0;
	case 0x13: *ability = 0x08000000u; return 0;
	case 0x14: *ability = 0xa8000000u; return 0;
	case 0x20: *ability = 0x0c000000u; return 0;
	case 0x30: *ability = 0x30000000u; return 0;
	default: return -1;
	}
}

/* ------------------------------------------------------ class 47's own work
 *
 * Three things a MAC bridge port config data row drives directly, all of
 * them only when its TP is a PPTP Ethernet UNI.
 */

/* The MAC learning limit falls back in three steps: the port's own
 * NumOfAllowedMac, else the bridge service profile's MacLearningDepth, else
 * a global default the stock daemon keeps in its runtime state, which is
 * not recovered. So the third step is a refusal rather than a guess: -1
 * means send nothing. A wrong limit is a silent forwarding fault, and zero
 * is not a safe stand-in, because the stock daemon never sends zero.
 */
static int mbpcd_learn_limit(const struct mib_row *r, const struct omci_class *c)
{
	const struct omci_class *sp = find_class(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE);
	struct mib_row *pr;
	uint32_t v = row_u32(r, c, 13);          /* NumOfAllowedMac */

	if (v)
		return (int)v;
	if (!sp)
		return -1;
	pr = mib_find(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE, (uint16_t)row_u32(r, c, 1));
	if (!pr)
		return -1;
	v = row_u32(pr, sp, 9);                  /* MacLearningDepth */
	return v ? (int)v : -1;
}

/* A UNI port rate from a traffic descriptor: its PIR in bytes per second as
 * kbit/s; a descriptor that does not resolve is no limit, as in the stock
 * stack. */
static void mbpcd_uni_rate(uint16_t inst, int port, uint32_t dir, uint32_t tdPtr)
{
	const struct omci_class *td = find_class(OMCI_ME_TRAFFIC_DESCRIPTOR);
	struct mib_row *tdr = td ? mib_find(OMCI_ME_TRAFFIC_DESCRIPTOR, (uint16_t)tdPtr) : 0;
	struct omci_unirate u;
	int rc;

	u.port = (uint32_t)port;
	u.dir = dir;
	u.kbps = OMCI_UNIRATE_NOLIMIT;
	if (tdr) {
		uint32_t pir = row_u32(tdr, td, 2);

		u.kbps = (pir << 3) >> 10;
		if (u.kbps == 0)
			u.kbps = OMCI_UNIRATE_NOLIMIT;
	}
	rc = apply_hw ? omci_drv_call(OMCI_UNIRATE_CMD, &u, sizeof u) : 0;
	out_fmt("   [%s 47/%04x] uni port %d dir %d rate %d kbit/s (td %04x) -> %d\n",
		apply_hw ? "hw" : "dry", (long)inst, (long)u.port, (long)u.dir,
		(long)u.kbps, (long)tdPtr, (long)rc);
}

/* The DSCP-to-P-bit map, unpacked and sent -- command 52.
 *
 * Two classes carry the same 24-byte attribute and both send it here, with
 * different guards:
 *
 *   171  the extended VLAN tagging operation: sent when the attribute is in
 *        the mask and the map is not all zero (the stock stack compares it
 *        against 24 zero bytes and returns).
 *   130  the 802.1p mapper: sent when the attribute is in the mask and the
 *        mapper's UnmarkFrmOpt is 0 (G.988: DSCP to P-bit). No all-zero test.
 *
 * Three bits per code point, most significant first: eight groups of three
 * bytes become eight P-bits each.
 */
static void dscp_remap_send(const uint8_t *p, const char *who, uint16_t inst)
{
	uint8_t remap[64];
	int rc;

	for (unsigned g = 0; g < 8; g++) {
		uint32_t w = ((uint32_t)p[g * 3] << 16)
			   | ((uint32_t)p[g * 3 + 1] << 8)
			   | p[g * 3 + 2];

		for (unsigned j = 0; j < 8; j++) {
			remap[g * 8 + j] = (uint8_t)((w & 0x00e00000u) >> 21);
			w <<= 3;
		}
	}
	out_fmt("   dscp remap %d %d %d %d ... %d\n",
		(long)remap[0], (long)remap[1], (long)remap[2],
		(long)remap[3], (long)remap[63]);
	if (!apply_hw)
		return;
	/* The wrapper's buf[64..128] is its own shadow of the last map and a
	 * changed-bits mask; neither is sent, so 64 is the whole payload. */
	rc = omci_setDscpRemap(remap);
	out_fmt("   [hw %s/%04x] dscp remap -> %d\n", who, (long)inst, (long)rc);
}

/* Command 64. `enable` is the create-side test of the stock stack on class
 * 45's DiscardUnknow, (byte != 1), so a 2 floods. */
static void flood_mask(const char *who, uint16_t inst, uint32_t portMask,
		       uint32_t enable)
{
	struct omci_flood f;
	int rc;

	f.sel = 0;
	f.enable = enable;
	f.portMask = portMask;
	rc = apply_hw ? omci_drv_call(OMCI_FLOOD_CMD, &f, sizeof f) : 0;
	out_fmt("   [%s %s/%04x] flooding mask %x enable %d -> %d\n",
		apply_hw ? "hw" : "dry", who, (long)inst, (long)portMask,
		(long)enable, (long)rc);
}

/* Class 47 sends the one port it is adding or removing; class 45 sends the
 * bridge's whole membership. Same command, same descriptor. */
static void mbpcd_flood(uint16_t inst, int port, uint32_t enable)
{
	flood_mask("47", inst, 1u << (unsigned)port, enable);
}

/* Everything class 47 drives for a PPTP Ethernet UNI, in one place, because
 * create, set and delete differ only in which of these they call and with what.
 * Returns the switch port, or -1 when the TP pointer names no UNI. */
static int mbpcd_uni_port(const struct mib_row *r, const struct omci_class *c,
			  uint16_t inst)
{
	int port;

	if (row_u32(r, c, 3) != MBPCD_TP_PPTP_ETH_UNI)
		return -1;
	port = uni_switch_port((uint16_t)row_u32(r, c, 4), OMCI_UNI_SLOT_PPTP);
	if (port < 0)
		out_fmt("   [47/%04x] tp pointer %04x is in no capability slot\n",
			(long)inst, (long)row_u32(r, c, 4));
	return port;
}

/* ------------------------------------------------------- dot1 rate limiter
 *
 * Class 298 holds a parent bridge, a TP type and three traffic descriptor
 * pointers -- upstream unicast flood, broadcast and multicast payload -- and
 * programs one driver slot per pointer that resolves.
 *
 * The slot number is ours in the same way a GEM flow id is: the driver's table
 * is indexed by it and the descriptor's first two words are what identify the
 * entry, so all that has to hold is that a delete names the slot its set used.
 */
static struct {
	uint8_t used;
	uint32_t portMask;
	uint32_t kind;
} dot1rl[OMCI_DOT1RL_SLOTS];

/* A slot already holding this (portMask, kind), else, when setting, the
 * first free one. -1 is "no such slot" on a delete and "table full" on a
 * set; both are refused, as in the stock stack. */
static int dot1rl_slot(uint32_t portMask, uint32_t kind, int alloc)
{
	int free_slot = -1;

	for (int i = 0; i < OMCI_DOT1RL_SLOTS; i++) {
		if (dot1rl[i].used) {
			if (dot1rl[i].portMask == portMask &&
			    dot1rl[i].kind == kind)
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	return alloc ? free_slot : -1;
}

/* The PPTP Ethernet UNIs of a bridge, from our own store: every class 47 row
 * in this bridge whose TP is a PPTP Ethernet UNI, mapped through the
 * capability table to a switch port. A bridge of VEIPs comes out as 0 here;
 * bridge_veip_port_mask() is its other half.
 */
uint32_t bridge_uni_port_mask(uint16_t bridge_id)
{
	const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	uint32_t mask = 0;

	if (!bp)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		int port;

		if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		if (row_u32(&mib[i], bp, 1) != bridge_id)
			continue;
		if (row_u32(&mib[i], bp, 3) != MBPCD_TP_PPTP_ETH_UNI)
			continue;
		port = uni_switch_port((uint16_t)row_u32(&mib[i], bp, 4),
				       OMCI_UNI_SLOT_PPTP);
		if (port >= 0)
			mask |= 1u << (unsigned)port;
	}
	return mask;
}

/* The same for a bridge whose ports are VEIPs: the union of the Ethernet
 * UNIs each VEIP port fronts. */
static uint32_t bridge_veip_port_mask(uint16_t bridge_id)
{
	const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
	uint32_t mask = 0;

	if (!bp)
		return 0;
	for (int i = 0; i < MIB_ROWS; i++) {
		if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			continue;
		if (row_u32(&mib[i], bp, 1) != bridge_id)
			continue;
		if (row_u32(&mib[i], bp, 3) != MBPCD_TP_VEIP)
			continue;
		mask |= veip_uni_port_mask((uint16_t)row_u32(&mib[i], bp, 4));
	}
	return mask;
}

/* One of the three rates. `drop` forces the delete; otherwise a traffic
 * descriptor that does not resolve is itself a delete, as a pointer of 0
 * or one naming nothing is in the stock stack. */
static void dot1rl_one(uint16_t inst, uint32_t portMask, uint32_t kind,
		       uint16_t tdPtr, int drop)
{
	const struct omci_class *td = find_class(OMCI_ME_TRAFFIC_DESCRIPTOR);
	struct mib_row *tdr = (!drop && td) ? mib_find(OMCI_ME_TRAFFIC_DESCRIPTOR, tdPtr) : 0;
	struct omci_dot1rl d;
	int slot = dot1rl_slot(portMask, kind, tdr != 0);
	int rc;

	if (slot < 0) {
		if (tdr)
			out_fmt("   [298/%04x] kind %d: no free rate limiter "
				"slot\n", (long)inst, (long)kind);
		return;                          /* a delete of nothing is not
						  * an error; the stock stack
						  * tolerates rc 15 here */
	}
	d.slot = (uint32_t)slot;
	d.portMask = portMask;
	d.kind = kind;
	d.cir = tdr ? row_u32(tdr, td, 1) : 0;
	d.cbs = tdr ? row_u32(tdr, td, 3) : 0;
	rc = apply_hw ? omci_drv_call(tdr ? OMCI_DOT1RL_SET_CMD
					  : OMCI_DOT1RL_DEL_CMD,
				      &d, sizeof d) : 0;
	out_fmt("   [%s 298/%04x] %s slot %d mask %x kind %d cir %d cbs %d -> %d\n",
		apply_hw ? "hw" : "dry", (long)inst, tdr ? "set" : "del",
		(long)slot, (long)portMask, (long)kind, (long)d.cir,
		(long)d.cbs, (long)rc);
	if (rc == 0) {
		dot1rl[slot].used = (uint8_t)(tdr != 0);
		dot1rl[slot].portMask = portMask;
		dot1rl[slot].kind = kind;
	}
}

/* The handler's body, for a create, a set or a delete. */
void dot1rl_apply(const struct omci_class *c, uint16_t inst,
		  const struct mib_row *r, int drop, int all)
{
	uint32_t mask;

	if (row_u32(r, c, 2) == 2) {             /* TpType */
		out_fmt("   [298/%04x] rate limiter for 802.1p mapper is not "
			"supported\n", (long)inst);
		return;
	}
	mask = bridge_uni_port_mask((uint16_t)row_u32(r, c, 1));
	if (mask == 0) {
		/* A bridge whose ports are VEIPs rather than PPTP UNIs, which
		 * is what the ISP2 OLT builds. The stock stack picks between
		 * the two on runtime state we cannot read; trying the PPTP
		 * shape first and falling back gives the same answer on both. */
		mask = bridge_veip_port_mask((uint16_t)row_u32(r, c, 1));
	}
	if (mask == 0) {
		out_fmt("   [298/%04x] no associated pptp eth uni is found\n",
			(long)inst);
		return;
	}
	for (unsigned k = 0; k < 3; k++) {
		/* `all` on a create or a delete; on a Set only the pointers
		 * this Set moved, the guard of the stock stack. */
		if (!all && !mib_changed(r, c, 3 + k))
			continue;
		dot1rl_one(inst, mask, k, (uint16_t)row_u32(r, c, 3 + k), drop);
	}
}

/* Class 11, a PPTP Ethernet UNI. mask is what the OLT just wrote, and only
 * those attributes are applied: an attribute never set is an absence, and
 * sending its default (a max frame size of 0) stops the port forwarding.
 * The port is the one the capability slots name (uni_switch_port()). */
void apply_pptp_uni(const struct omci_class *c, uint16_t inst,
		    const struct mib_row *r, uint16_t mask, int creating)
{
	int rc;

	int port = uni_switch_port(inst, OMCI_UNI_SLOT_PPTP);

	if (port < 0) {
		out_fmt("   [%s] uni %04x is in no capability slot\n",
			apply_hw ? "hw" : "dry",
			(long)inst);
		return;
	}
	if (!creating && (mask & (1u << (16 - 3)))) { /* AutoDetectCfg */
		uint32_t cfg = row_u32(r, c, 3), ability;

		if (ethuni_auto_ability(cfg, &ability) != 0) {
			out_fmt("   [%s] uni %04x port %d auto %x unsupported\n",
				apply_hw ? "hw" : "dry", (long)inst,
				(long)port, (long)cfg);
		} else {
			rc = apply_hw
			   ? omci_setPortAutoNegoAbility(pair((uint32_t)port, ability)) : 0;
			out_fmt("   [%s] uni %04x port %d auto %x ability %x -> %d\n",
				apply_hw ? "hw" : "dry", (long)inst,
				(long)port, (long)cfg, (long)ability, (long)rc);
		}
	}
	if (!creating && (mask & (1u << (16 - 4)))) { /* LoopbackCfg */
		uint32_t cfg = row_u32(r, c, 4);

		if (cfg != 0 && cfg != 3) {
			out_fmt("   [%s] uni %04x port %d loopback %d unsupported\n",
				apply_hw ? "hw" : "dry", (long)inst,
				(long)port, (long)cfg);
		} else {
			rc = apply_hw
			   ? omci_setPhyLoopback(pair((uint32_t)port, cfg == 3)) : 0;
			out_fmt("   [%s] uni %04x port %d loopback %d -> %d\n",
				apply_hw ? "hw" : "dry", (long)inst,
				(long)port, (long)(cfg == 3), (long)rc);
		}
	}
	if (mask & (1u << (16 - 5))) {           /* AdminState */
		uint32_t lock = row_u32(r, c, 5) == 1;

		rc = apply_hw
		   ? omci_setPortState(pair((uint32_t)port, !lock)) : 0;
		out_fmt("   [%s] uni %04x port %d state %d -> %d\n",
			apply_hw ? "hw" : "dry",
			(long)inst, (long)port, (long)!lock, (long)rc);
		if (apply_hw)
			omci_setPhyPwrDown(pair((uint32_t)port, lock));
	}
	if (mask & (1u << (16 - 8))) {           /* MaxFrameSize */
		uint32_t v = row_u32(r, c, 8);

		rc = apply_hw
		   ? omci_setMaxFrameSize(pair((uint32_t)port, v)) : 0;
		out_fmt("   [%s] uni %04x port %d max frame %d -> %d\n",
			apply_hw ? "hw" : "dry",
			(long)inst, (long)port, (long)v, (long)rc);
	}
	if (!creating && (mask & (1u << (16 - 10)))) { /* PauseTime */
		uint32_t pause = row_u32(r, c, 10);

		rc = apply_hw
		   ? omci_setPauseControl(pair((uint32_t)port, pause)) : 0;
		out_fmt("   [%s] uni %04x port %d pause %d -> %d\n",
			apply_hw ? "hw" : "dry", (long)inst,
			(long)port, (long)pause, (long)rc);
	}
	return;
}

/* Class 47, a MAC bridge port config data row: the learning limit, the
 * flooding bit and the port rates of a UNI port, and the broadcast flow
 * lookup. */
void apply_mbpcd(const struct omci_class *c, uint16_t inst,
		 const struct mib_row *r, uint16_t mask, int creating)
{
	int port;

	bc_gem_update();
	port = mbpcd_uni_port(r, c, inst);
	if (port < 0)
		return;
	/* The create arm sends the learning limit unconditionally; the
	 * set arm sends it only when attribute 13 is in the mask. */
	if (creating || (mask & (1u << (16 - 13)))) {
		int lim = mbpcd_learn_limit(r, c);

		if (lim < 0) {
			out_fmt("   [47/%04x] no mac learn limit: neither "
				"attribute set and the global default is "
				"not recovered\n", (long)inst);
		} else {
			int rc2 = apply_hw
				? omci_setMacLearnLimit((uint32_t)port,
							(uint32_t)lim)
				: 0;

			out_fmt("   [%s 47/%04x] mac learn limit port %d "
				"= %d -> %d\n", apply_hw ? "hw" : "dry",
				(long)inst, (long)port, (long)lim,
				(long)rc2);
		}
	}
	/* Flooding is create-side only. The enable comes from the
	 * bridge service profile, not from this row. */
	if (creating) {
		const struct omci_class *sp = find_class(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE);
		struct mib_row *pr = sp
			? mib_find(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE, (uint16_t)row_u32(r, c, 1)) : 0;
		uint32_t discard = pr ? row_u32(pr, sp, 8) : 0;

		mbpcd_flood(inst, port, discard != 1);
	}
	/* Traffic descriptors are set-side only, one attribute each.
	 * Direction 1 is the INBOUND descriptor here -- see
	 * omci_bridgeport.h. */
	if (!creating && (mask & (1u << (16 - 12))))
		mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_IN,
			       row_u32(r, c, 12));
	if (!creating && (mask & (1u << (16 - 11))))
		mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_OUT,
			       row_u32(r, c, 11));
	return;
}

/* Class 47 deleted, in the order of the stock stack; the row is still in
 * the store. */
void delete_mbpcd(const struct omci_class *c, uint16_t inst)
{
	const struct mib_row *r = mib_find(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA, inst);
	int port;

	if (!r)
		return;
	port = mbpcd_uni_port(r, c, inst);
	if (port >= 0) {
		/* The stock stack puts the limit back to its global
		 * default, which is not recovered: the port keeps the
		 * limit it had. A divergence, logged, not an omission. */
		out_fmt("   [47/%04x] delete: mac learn limit left as "
			"set, the global default is not recovered\n",
			(long)inst);
		mbpcd_flood(inst, port, 0);
		mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_IN,
			       0xffff);
		mbpcd_uni_rate(inst, port, OMCI_UNIRATE_DIR_OUT,
			       0xffff);
	}
	if (row_u32(r, c, 3) == MBPCD_TP_GEM_IWTP)
		bc_gem_withdraw(inst);
	return;
}

/* Class 171, the extended VLAN tagging operation. Its VLAN rules do not go
 * to the driver from here: they are carried in the bridge connection
 * (apply_bridge.c). What goes, straight off a Set, is the DSCP-to-P-bit
 * map, when that attribute is in the mask and not all zero (ISP1's is, so
 * this has never run there). */
void apply_ext_vlan_dscp(const struct omci_class *c, uint16_t inst,
			 const struct mib_row *r, uint16_t mask)
{
	unsigned k;

	const uint8_t *p;
	int off, any = 0;

	k = 8;                           /* DscpToPbitMapping */
	if (!(mask & (1u << (16 - k))))
		return;
	off = attr_offset(c, k);
	if (off < 0 || off + 24 > MIB_ROW_MAX)
		return;
	p = r->data + off;
	for (int i = 0; i < 24; i++)
		if (p[i])
			any = 1;
	if (!any)
		return;
	dscp_remap_send(p, "171", inst);
	return;
}

/* Class 130, the 802.1p mapper: its one contact with the hardware is the
 * DSCP map, when UnmarkFrmOpt says unmarked frames take their P-bit from
 * DSCP. The rest of its handling in the stock stack is MIB-tree
 * bookkeeping, which this stack does by walking the store; the other two
 * driver paths need a class 280 traffic descriptor (no OLT seen provisions
 * one) or are the command 65 bug of omci_gemflow.h. Dormant on ISP1 too:
 * all five mappers carry UnmarkFrmOpt 1 and an empty map. */
void apply_mapper_dscp(const struct omci_class *c, uint16_t inst,
		       const struct mib_row *r, uint16_t mask)
{
	unsigned k;

	const uint8_t *p;
	int off;

	k = 11;                          /* DscpMap2Pbit */
	if (!(mask & (1u << (16 - k))))
		return;
	if (row_u32(r, c, 10) != MAP8021P_UNMARKED_DSCP_TO_PBIT)
		return;
	off = attr_offset(c, k);
	if (off < 0 || off + 24 > MIB_ROW_MAX)
		return;
	p = r->data + off;
	dscp_remap_send(p, "130", inst);
	return;
}

/* Class 45, a MAC bridge service profile, on a Set: flooding and the
 * learning limit when their attribute changed, and the ageing time. Port
 * bridging is set on a delete (delete_mbsp()), not here. The fourth arm of
 * the stock handler dispatches to a plugin this image does not have and
 * does nothing on this device. */
void apply_mbsp(const struct omci_class *c, uint16_t inst,
		const struct mib_row *r, uint16_t mask, int creating)
{
	unsigned k;
	int rc;

	if (!creating) {
		unsigned kk = 8;         /* DiscardUnknow */

		if ((mask & (1u << (16 - kk))) && mib_changed(r, c, kk)) {
			uint32_t m = bridge_uni_port_mask(inst);

			if (m)
				flood_mask("45", inst, m,
					   row_u32(r, c, kk) != 1);
			else
				out_fmt("   [45/%04x] flooding: no pptp "
					"eth uni in this bridge\n",
					(long)inst);
		}
		kk = 9;                  /* MacLearningDepth */
		if ((mask & (1u << (16 - kk))) && mib_changed(r, c, kk)) {
			/* The command names ONE port, so it is sent
			 * only when the bridge has exactly one, as in
			 * the stock stack. */
			const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
			int port = -1, n = 0;

			for (int i = 0; bp && i < MIB_ROWS; i++) {
				if (!mib[i].used || mib[i].classId != OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
					continue;
				if (row_u32(&mib[i], bp, 1) != inst)
					continue;
				if (row_u32(&mib[i], bp, 3)
				    != MBPCD_TP_PPTP_ETH_UNI)
					continue;
				n++;
				port = uni_switch_port((uint16_t)
					row_u32(&mib[i], bp, 4),
					OMCI_UNI_SLOT_PPTP);
			}
			if (n == 1 && port >= 0) {
				uint32_t lim = row_u32(r, c, kk);
				int rc2 = apply_hw
					? omci_setMacLearnLimit(
						(uint32_t)port, lim) : 0;

				out_fmt("   [%s 45/%04x] mac learn "
					"limit port %d = %d -> %d\n",
					apply_hw ? "hw" : "dry",
					(long)inst, (long)port,
					(long)lim, (long)rc2);
			} else {
				out_fmt("   [45/%04x] mac learn limit: "
					"bridge has %d pptp eth uni, "
					"not one\n", (long)inst, (long)n);
			}
		}
	}
	k = 10;                          /* DynamicFilteringAgeingTime */
	if (!(mask & (1u << (16 - k))))
		return;
	{
		uint32_t age = row_u32(r, c, k);

		/* Zero means the default, 300 s, not zero. The register of
		 * cmd 62 counts tenths of a second, so the default is sent
		 * as 3000 (the delete does the same). A non-zero value from
		 * the OLT is passed through unscaled: nothing yet pins its
		 * unit, so it is not guessed. */
		if (age == 0)
			age = 300 * 10;
		out_fmt("   ageing time %d\n", (long)age);
		if (!apply_hw)
			return;
		rc = omci_setAgeingTime(age);
	}
	out_fmt("   [hw] %s -> %d\n", "ageing time", (long)rc);
}

/* Class 45 deleted: the ageing time back to its 300 s default (3000 in the
 * tenths of a second of cmd 62) and port bridging on. Both constants,
 * neither from the row. */
void delete_mbsp(uint16_t inst)
{
	out_fmt("   [45/%04x] delete: ageing 300, port bridging 1\n", (long)inst);
	if (!apply_hw)
		return;
	omci_setAgeingTime(300 * 10);
	omci_setPortBridging(1);
}

/* The UNI state a MIB reset throws away (mib_reset_all()). */
void uni_reset(void)
{
	for (int i = 0; i < OMCI_DOT1RL_SLOTS; i++)
		dot1rl[i].used = 0;
}
