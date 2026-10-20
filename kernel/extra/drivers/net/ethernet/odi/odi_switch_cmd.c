// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_cmd.c -- odi_switch_cmd(), the OMCI driver command dispatch
 * (odi_switch_cmd.h), and the commands other than cmd 51/50
 * (odi_switch_bdgconn.c). Every register write goes through a leaf of
 * odi_switch_dal.h, or for cmd 10 the DDM read of odi_ddm.c; no register
 * offset appears here.
 *
 * Values marked "replayed" are what the ISP1 provisioning capture shows
 * (test/fixtures/isp1-260922-boot5.txt) for a command whose argument
 * fields are not decoded. The upstream PON queues (cmd 21, 23, 24, 25) are
 * not replayed: every word comes from the argument, as the stock driver
 * computes it (docs/SWITCH.md#upstream-pon-queues).
 */
#include "odi_switch_cmd.h"
#include "odi_switch_dal.h"
#include "odi_switch_l2.h"
#include "odi_gpon.h"
#include "odi_ddm.h"

#include "uapi/omci_gemflow.h"
#include "uapi/omci_bdgconn.h"
#include "uapi/omci_bridgeport.h"
#include "uapi/omci_caps.h"
#include "uapi/omci_flows.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

/* cmd 21: the T-CONTs, by index. The index is the Alloc-ID's row in the
 * GPON MAC's Alloc-ID CAM, because the hardware ties the two: the upstream
 * scheduler of T-CONT n (its queue mask, PON_SCH_QMAP[n]) serves the bursts
 * the OLT grants to the Alloc-ID in CAM row n, and row 16 is the OMCC's
 * default Alloc-ID with its T-CONT 16. The stock driver works the same way
 * (its T-CONT id is the row it wrote the Alloc-ID to). Rows 0-15 are data
 * T-CONTs. Each holds a block of consecutive logical queues in its group
 * of 32 (odi_switch_dal.h), reserved by cmd 21 for the number of queues
 * omcid asks for, and the ordinals of the block cmd 23 has programmed.
 */
struct odi_sw_tcont_slot {
	uint32_t alloc_id;
	uint32_t live;		/* ordinals with a queue programmed, one bit each */
	uint8_t used;
	uint8_t qbase;		/* first logical queue of the block */
	uint8_t qn;		/* queues in the block, 0: none reserved */
};
static struct odi_sw_tcont_slot odi_sw_tcont[ODI_SW_PONQ_TCONTS];

#ifndef __KERNEL__
/* The host build has no GPON core: unless a test links one in and points
 * this at its lookup (odi_gpon_hw_alloc_id_find()), the tests stand in for
 * an OLT that assigned every Alloc-ID they name, rows from 0 in first-seen
 * order, the order PLOAM Assign_Alloc-ID fills the CAM in (odi_gpon_hw.c).
 */
int (*odi_sw_host_alloc_row)(uint16_t alloc_id);
static uint32_t odi_sw_host_rows[ODI_SW_PONQ_TCONTS];
static unsigned int odi_sw_host_rows_n;
#endif

static int tcont_alloc_row(uint32_t alloc_id)
{
#ifdef __KERNEL__
	return odi_gpon_alloc_row((uint16_t)alloc_id);
#else
	unsigned int i;

	if (odi_sw_host_alloc_row)
		return odi_sw_host_alloc_row((uint16_t)alloc_id);
	for (i = 0; i < odi_sw_host_rows_n; i++)
		if (odi_sw_host_rows[i] == alloc_id)
			return (int)i;
	if (odi_sw_host_rows_n >= ODI_SW_PONQ_TCONTS)
		return -1;
	odi_sw_host_rows[odi_sw_host_rows_n] = alloc_id;
	return (int)odi_sw_host_rows_n++;
#endif
}

/* cmd 25: omcid chooses the flow id (the one the port already holds in
 * that direction, else the lowest free one: uapi/omci_gemflow.h), and the
 * flow id is the table index. The downstream CAM row and the upstream
 * flow slot are both addressed by it, and the upstream CF action queues a
 * frame on the same number, so a GEM port in any other row would not be
 * the one its CF rule names. What each row was given is kept for the
 * OMCI_FLOWS_CMD readback (uapi/omci_flows.h), the first OMCI_FLOWS_MAX.
 */
static uint32_t odi_sw_gem_ds_used[(ODI_SW_CMD_GEM_DS_MAX + 31) / 32];
static uint32_t odi_sw_gem_us_used[(ODI_SW_CMD_GEM_US_MAX + 31) / 32];
static uint32_t odi_sw_gem_ds_gem[OMCI_FLOWS_MAX];
static uint32_t odi_sw_gem_ds_cfg[OMCI_FLOWS_MAX];
static uint32_t odi_sw_gem_us_gem[OMCI_FLOWS_MAX];

/* cmd 25 upstream: the physical queue each flow is mapped to
 * (PON_SID2QID), or ODI_SW_SID_UNMAPPED. Kept so that a queue add makes
 * its flows valid again and a queue delete takes them out, as the stock
 * driver does from the register.
 */
#define ODI_SW_SID_UNMAPPED	0xffU
static uint8_t odi_sw_sid2qid[ODI_SW_CMD_GEM_US_MAX];

static uint32_t ponq_physical(uint32_t t, uint32_t lq)
{
	return ODI_SW_PONQ_GROUP_QUEUES * (t / ODI_SW_PONQ_GROUP_TCONTS) + lq;
}

static uint32_t qmask(uint32_t base, uint32_t n)
{
	if (n >= 32U)
		return 0xffffffffU;
	return ((1U << n) - 1U) << base;
}

/* The logical queues the other T-CONTs of t's group have reserved. */
static uint32_t group_taken(uint32_t t)
{
	uint32_t g = t / ODI_SW_PONQ_GROUP_TCONTS, u, m = 0;

	for (u = g * ODI_SW_PONQ_GROUP_TCONTS; u < (g + 1U) * ODI_SW_PONQ_GROUP_TCONTS; u++)
		if (u != t && odi_sw_tcont[u].qn)
			m |= qmask(odi_sw_tcont[u].qbase, odi_sw_tcont[u].qn);
	return m;
}

/* A block of n queues for T-CONT t: the one it has if big enough, grown
 * in place if the queues after it are free, else (no queue of it live)
 * the first free run of the group. The stock driver reserves the same
 * way (first fit in the group); it would also move the T-CONT to the
 * other group when its own is full, which this refuses instead.
 */
static int tcont_reserve(uint32_t t, uint32_t n)
{
	struct odi_sw_tcont_slot *s = &odi_sw_tcont[t];
	uint32_t taken = group_taken(t), b;

	if (s->qn >= n)
		return 0;
	if (s->qn && s->qbase + n <= ODI_SW_PONQ_GROUP_QUEUES &&
	    !(taken & qmask(s->qbase + s->qn, n - s->qn))) {
		s->qn = (uint8_t)n;
		return 0;
	}
	if (s->live)
		return -1;
	for (b = 0; b + n <= ODI_SW_PONQ_GROUP_QUEUES; b++)
		if (!(taken & qmask(b, n))) {
			s->qbase = (uint8_t)b;
			s->qn = (uint8_t)n;
			return 0;
		}
	return -1;
}


/* Replayed scalars. */
#define ODI_SW_CMD23_PORT_QUEUE_MAP	0xd4U	/* cmd 23, a downstream queue */
#define ODI_SW_CMD30_A408		0xde1U	/* cmd 30, the three UNI PHY words */
#define ODI_SW_CMD30_A412		0x0U
#define ODI_SW_CMD30_A400		0x3a00U
#define ODI_SW_CMD62_AGE_TICKS		3000U	/* cmd 62 when omcid sends 0 */
#define ODI_SW_CMD62_AGE_ON_LINK_DOWN	1U

static const struct odi_switch_flow_ops *odi_sw_flow_ops;

/* Which UNI limits (cmd 53, 67) a port carries that the boot replay did not
 * put there, so a MIB reset puts back only those: ODI_SW_CMD_UNI_LIMIT_*. */
static uint8_t odi_sw_uni_limit_set[ODI_SW_PORT_COUNT];

void odi_switch_cmd_set_flow_ops(const struct odi_switch_flow_ops *ops)
{
	odi_sw_flow_ops = ops;
}

void odi_switch_cmd_reset_state(void)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_PONQ_TCONTS; i++) {
		odi_sw_tcont[i].alloc_id = 0;
		odi_sw_tcont[i].live = 0;
		odi_sw_tcont[i].used = 0;
		odi_sw_tcont[i].qbase = 0;
		odi_sw_tcont[i].qn = 0;
	}
#ifndef __KERNEL__
	odi_sw_host_rows_n = 0;
#endif
	for (i = 0; i < ODI_SW_PORT_COUNT; i++)
		odi_sw_uni_limit_set[i] = 0;
	odi_switch_bdgconn_reset();
	for (i = 0; i < sizeof(odi_sw_gem_ds_used) / sizeof(odi_sw_gem_ds_used[0]); i++)
		odi_sw_gem_ds_used[i] = 0;
	for (i = 0; i < sizeof(odi_sw_gem_us_used) / sizeof(odi_sw_gem_us_used[0]); i++)
		odi_sw_gem_us_used[i] = 0;
	for (i = 0; i < ODI_SW_CMD_GEM_US_MAX; i++)
		odi_sw_sid2qid[i] = ODI_SW_SID_UNMAPPED;
	for (i = 0; i < OMCI_FLOWS_MAX; i++) {
		odi_sw_gem_ds_gem[i] = 0;
		odi_sw_gem_ds_cfg[i] = 0;
		odi_sw_gem_us_gem[i] = 0;
	}
	if (odi_sw_flow_ops && odi_sw_flow_ops->clear)
		odi_sw_flow_ops->clear();
}

/* cmd 62: the ageing time. The argument is a raw 4-byte value; omcid
 * sends 0, which keeps the captured default.
 */
static int cmd_mac_age_time(void *buf, uint32_t len)
{
	uint32_t age_spd = ODI_SW_CMD62_AGE_TICKS, linkdown_ageout = ODI_SW_CMD62_AGE_ON_LINK_DOWN;

	if (buf && len >= sizeof(uint32_t)) {
		uint32_t v = *(const uint32_t *)buf;

		if (v)
			age_spd = v;
	}
	odi_sw_l2_aging_set(age_spd, linkdown_ageout);
	return 0;
}

/* cmd 30: the UNI PHY auto-negotiation, the same values every time (one
 * UNI). cmd 31, the read, is not sent by omcid.
 */
static int cmd_port_auto_nego(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	odi_sw_port_autoneg_get();
	odi_sw_port_autoneg_set(ODI_SW_CMD30_A408, ODI_SW_CMD30_A412, ODI_SW_CMD30_A400);
	return 0;
}

/* cmd 32: {port, state}, the admin state of an Ethernet UNI (ME 11
 * AdministrativeState, omcid's apply_uni.c sends !lock). state 0 forces
 * the port's link down, anything else releases it, as the stock OMCI
 * layer does through the SDK's admin enable (pf_rtl96xx_SetPortState).
 * On this SFP the UNI is the host port: a lock takes the host link down,
 * and the stick's own management path with it, until the OLT unlocks;
 * OMCI itself does not cross that port. The PON and CPU ports are refused:
 * forced down, either would cut the OMCI channel, and with it the unlock.
 * No argument (the replay harness) is an unlock of port 0, the captured
 * bracket.
 */
#define ODI_SW_CMD32_PON_PORT	2U
#define ODI_SW_CMD32_CPU_PORT	3U

struct odi_sw_cmd32_arg {
	uint32_t port;
	uint32_t state;
};

static int cmd_port_state(void *buf, uint32_t len)
{
	struct odi_sw_cmd32_arg a = { .port = ODI_SW_HOST_PORT, .state = 1U };

	if (buf && len >= sizeof(a))
		a = *(const struct odi_sw_cmd32_arg *)buf;
	if (a.port >= ODI_SW_PORT_COUNT || a.port == ODI_SW_CMD32_PON_PORT ||
	    a.port == ODI_SW_CMD32_CPU_PORT) {
		ODI_SW_WARN("cmd 32: port %u state %u refused (not a UNI port)\n",
			    (unsigned int)a.port, (unsigned int)a.state);
		return ODI_SW_EOPNOTSUPP;
	}
	odi_sw_port_admin_set(a.port, a.state != 0U);
	if (!a.state)
		ODI_SW_LOG("cmd 32: port %u admin locked by the OLT: link forced down\n",
			   (unsigned int)a.port);
	return 0;
}

/* cmd 11: {type, threshold}, the ANI-G signal fail or signal degrade
 * threshold the OLT set, as the exponent x of a 10^-x BER (the stock
 * OMCI_SIGNAL_PARA_ts: type 4 SF, 5 SD; the stock driver hands it to its
 * GPON module, which raises the SF/SD alarms against it). The GPON core
 * evaluates them (odi_gpon_ctr.c); out-of-range values are refused and the
 * previous threshold kept.
 */
#define ODI_SW_CMD11_SIG_SF	4U
#define ODI_SW_CMD11_SIG_SD	5U

struct odi_sw_cmd11_arg {
	uint32_t type;
	uint32_t threshold;
};

#ifndef __KERNEL__
/* The host build has no GPON core: a test points this at one. */
int (*odi_sw_host_signal_threshold_set)(unsigned int signal, unsigned int exp);
#endif

static int cmd_signal_parameter(void *buf, uint32_t len)
{
	const struct odi_sw_cmd11_arg *a = (const struct odi_sw_cmd11_arg *)buf;
	unsigned int signal;
	int rc;

	if (!buf || len < sizeof(*a))
		return -1;
	if (a->type == ODI_SW_CMD11_SIG_SF)
		signal = 0U;
	else if (a->type == ODI_SW_CMD11_SIG_SD)
		signal = 1U;
	else
		return ODI_SW_EOPNOTSUPP;	/* LOS/LOF/LOM thresholds: not settable */
#ifdef __KERNEL__
	rc = odi_gpon_signal_threshold_set(signal, a->threshold);
#else
	rc = odi_sw_host_signal_threshold_set ? odi_sw_host_signal_threshold_set(signal, a->threshold) : 0;
#endif
	if (rc) {
		ODI_SW_WARN("cmd 11: %s threshold 10^-%u refused (out of range)\n",
			    signal ? "SD" : "SF", (unsigned int)a->threshold);
		return -1;
	}
	return 0;
}

/* cmd 38: no register write in any capture. */
static int cmd_port_phy_pwrdown(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	return 0;
}

/* cmd 64: {sel, enable, portMask} (uapi/omci_bridgeport.h), a lookup-miss
 * flood mask. As the stock OMCI layer does (pf_rtl96xx_SetFloodingPortMask):
 * sel picks the mask (0 unknown unicast, 1 broadcast, 2 unknown
 * multicast), enable 1 adds portMask's ports, 0 removes them, and every
 * other port keeps its bit; enable 2 (trap) is refused, as stock refuses it.
 * omcid sends one port per class 47 create or delete, so a delete, or ME
 * 45 DiscardUnknown = 1, takes one port out instead of every port.
 *
 * One exception, kept from before: the module-load replay leaves every
 * mask at 0xf, the CPU port included (docs/SWITCH.md), where the stock
 * image has 0x7, and cmd 64 used to write 0x7 to the unknown-unicast
 * mask. So the first provisioning took the CPU out of unknown-unicast
 * flooding, and both ISPs run that way. Keeping the CPU in has not been
 * tried on a stick, so cmd 64 still clears the CPU bit of that mask.
 * No argument (the replay harness) is the captured call: sel 0, enable 1,
 * the PON port.
 */
#define ODI_SW_CMD64_CPU_BIT	(1U << 3)
#define ODI_SW_CMD64_PORTS	((1U << ODI_SW_PORT_COUNT) - 1U)
#define ODI_SW_CMD64_PON_BIT	(1U << 2)

static int cmd_flooding_port_mask(void *buf, uint32_t len)
{
	struct omci_flood f = { .sel = ODI_SW_FLOOD_UNKN_UCAST, .enable = 1U,
				.portMask = ODI_SW_CMD64_PON_BIT };
	uint32_t mask;

	if (buf && len >= sizeof(f))
		f = *(const struct omci_flood *)buf;
	if (f.sel > ODI_SW_FLOOD_UNKN_MCAST || f.enable > 1U) {
		ODI_SW_WARN("cmd 64: sel %u enable %u refused\n", (unsigned int)f.sel,
			    (unsigned int)f.enable);
		return ODI_SW_EOPNOTSUPP;
	}
	mask = odi_sw_l2_flood_mask_get(f.sel);
	if (f.enable)
		mask |= f.portMask & ODI_SW_CMD64_PORTS;
	else
		mask &= ~(f.portMask & ODI_SW_CMD64_PORTS);
	if (f.sel == ODI_SW_FLOOD_UNKN_UCAST)
		mask &= ~ODI_SW_CMD64_CPU_BIT;
	odi_sw_l2_flood_mask_set(f.sel, mask);
	return 0;
}

/* ---- The UNI limits OMCI provisions: learning depth, rates ---------------
 *
 * cmd 53 (ME 45 / ME 47 learning depth) and cmd 67 (ME 47 traffic
 * descriptors) used to be refused, so an OLT that set them got a bridge
 * with neither. They are applied now, and every one is a limit the OLT
 * asked for, never a default we invent: a port nobody provisioned keeps
 * what the boot replay left (1088 addresses, no rate). What a limit can
 * break, and what stops it:
 *
 *   - only UNI ports take one. The PON port carries OMCI and the CPU port
 *     the stick's own management, and a limit there would cut either;
 *   - a learning limit of 0 is refused (it is learning off, not "forward
 *     nothing") and the over-limit action is never written, so a frame from
 *     an address past the limit is still forwarded, unlearned;
 *   - a rate below ODI_SW_CMD67_KBPS_MIN is refused and the port keeps its
 *     previous rate: a descriptor that small is malformed, not a service
 *     plan, and it would cut the host (and the management path behind it)
 *     off. Refusing fails open;
 *   - a MIB reset puts the ports we changed back to the boot state.
 */
#define ODI_SW_CMD_UNI_LIMIT_LEARN	(1U << 0)
#define ODI_SW_CMD_UNI_LIMIT_POLICE	(1U << 1)
#define ODI_SW_CMD_UNI_LIMIT_SHAPE	(1U << 2)
#define ODI_SW_CMD67_KBPS_MIN		1024U	/* 1 Mbit/s */

static int uni_port_ok(uint32_t port)
{
	return port < ODI_SW_PORT_COUNT && port != ODI_SW_CMD32_PON_PORT &&
	       port != ODI_SW_CMD32_CPU_PORT;
}

/* The boot state back on every port a limit was set on, nothing written on
 * the rest. */
static void uni_limits_restore(void)
{
	uint32_t p;

	for (p = 0; p < ODI_SW_PORT_COUNT; p++) {
		uint8_t f = odi_sw_uni_limit_set[p];

		if (f & ODI_SW_CMD_UNI_LIMIT_LEARN)
			odi_sw_l2_learn_limit_set(p, ODI_SW_L2_LEARN_LIMIT_NONE);
		if (f & ODI_SW_CMD_UNI_LIMIT_POLICE)
			odi_sw_uni_police_set(p, ODI_SW_UNI_RATE_NONE);
		if (f & ODI_SW_CMD_UNI_LIMIT_SHAPE)
			odi_sw_uni_shape_set(p, ODI_SW_UNI_RATE_NONE);
		if (f)
			ODI_SW_LOG("port %u: UNI limits back to the boot state\n",
				   (unsigned int)p);
		odi_sw_uni_limit_set[p] = 0;
	}
}

/* cmd 53: {port, limit}, the most addresses the port may learn. */
static int cmd_mac_learn_limit(void *buf, uint32_t len)
{
	struct omci_learn_limit a;
	uint32_t limit;

	if (!buf || len < sizeof(a))
		return -1;
	a = *(const struct omci_learn_limit *)buf;
	if (!uni_port_ok(a.port)) {
		ODI_SW_WARN("cmd 53: port %u refused (not a UNI port)\n",
			    (unsigned int)a.port);
		return ODI_SW_EOPNOTSUPP;
	}
	if (a.limit == 0U) {
		ODI_SW_WARN("cmd 53: port %u limit 0 refused (it would stop learning)\n",
			    (unsigned int)a.port);
		return -22;
	}
	limit = a.limit >= OMCI_LEARN_LIMIT_TABLE ? ODI_SW_L2_LEARN_LIMIT_NONE : a.limit;
	odi_sw_l2_learn_limit_set(a.port, limit);
	odi_sw_uni_limit_set[a.port] |= ODI_SW_CMD_UNI_LIMIT_LEARN;
	ODI_SW_LOG("cmd 53: port %u learns at most %u addresses\n",
		   (unsigned int)a.port, (unsigned int)limit);
	return 0;
}

/* cmd 67: {port, dir, kbit/s}. dir 1 is the inbound descriptor of the UNI,
 * traffic entering the bridge from the port: the ingress meter. dir 2 is the
 * outbound one, traffic leaving to the port: the egress shaper. */
static int cmd_uni_rate(void *buf, uint32_t len)
{
	struct omci_unirate a;
	uint32_t units;

	if (!buf || len < sizeof(a))
		return -1;
	a = *(const struct omci_unirate *)buf;
	if (!uni_port_ok(a.port) ||
	    (a.dir != OMCI_UNIRATE_DIR_IN && a.dir != OMCI_UNIRATE_DIR_OUT)) {
		ODI_SW_WARN("cmd 67: port %u dir %u refused\n",
			    (unsigned int)a.port, (unsigned int)a.dir);
		return ODI_SW_EOPNOTSUPP;
	}
	if (a.kbps >= OMCI_UNIRATE_NOLIMIT) {
		units = ODI_SW_UNI_RATE_NONE;
	} else if (a.kbps < ODI_SW_CMD67_KBPS_MIN) {
		ODI_SW_WARN("cmd 67: port %u dir %u rate %u kbit/s refused (below %u)\n",
			    (unsigned int)a.port, (unsigned int)a.dir,
			    (unsigned int)a.kbps, ODI_SW_CMD67_KBPS_MIN);
		return -22;
	} else {
		units = a.kbps / ODI_SW_UNI_RATE_UNIT_KBPS;
	}
	if (a.dir == OMCI_UNIRATE_DIR_IN) {
		odi_sw_uni_police_set(a.port, units);
		odi_sw_uni_limit_set[a.port] |= ODI_SW_CMD_UNI_LIMIT_POLICE;
	} else {
		odi_sw_uni_shape_set(a.port, units);
		odi_sw_uni_limit_set[a.port] |= ODI_SW_CMD_UNI_LIMIT_SHAPE;
	}
	ODI_SW_LOG("cmd 67: port %u %s %s\n", (unsigned int)a.port,
		   a.dir == OMCI_UNIRATE_DIR_IN ? "ingress" : "egress",
		   units == ODI_SW_UNI_RATE_NONE ? "unlimited" : "limited");
	return 0;
}

/* cmd 63: setPortBridging. The stock driver answers 1 by rewriting the
 * isolation mask of every UNI to "the UNIs and the PON" and 0 to "the PON
 * and the CPU". Both would replace the masks odi_sw_pon_isolate_apply()
 * owns (PORT_ISOLATION, the CPU kept out of the PON side), so neither is
 * written. 1 is what is already true here (the UNI and the PON forward to
 * one another) and succeeds; 0 is refused. omcid sends 1 when a bridge
 * service profile is deleted.
 */
static int cmd_port_bridging(void *buf, uint32_t len)
{
	if (!buf || len < sizeof(uint32_t))
		return -1;
	if (*(const uint32_t *)buf == 1U)
		return 0;
	ODI_SW_WARN("cmd 63: port bridging %u refused (the isolation masks are not OMCI's)\n",
		    (unsigned int)*(const uint32_t *)buf);
	return ODI_SW_EOPNOTSUPP;
}

/* cmd 40: setPauseControl {port, pause time}. A pause time of 0 turns flow
 * control off, which is the state of the host port already (it never sends
 * PAUSE unless odi_switch.host_pause says so, odi_sw_host_pause_apply());
 * anything else would switch it on, and that is the operator's decision
 * and not the OLT's: a stalled PON with it on stops the host's NIC, the
 * reason the default is off.
 */
static int cmd_pause_control(void *buf, uint32_t len)
{
	const uint32_t *a = (const uint32_t *)buf;

	if (!buf || len < 2U * sizeof(uint32_t))
		return -1;
	if (a[1] == 0U)
		return 0;
	ODI_SW_WARN("cmd 40: port %u pause time %u refused (host PAUSE is the host_pause parameter)\n",
		    (unsigned int)a[0], (unsigned int)a[1]);
	return ODI_SW_EOPNOTSUPP;
}

/* ---- ME 50, the bridge table: cmd 61 and OMCI_BRIDGE_TBL_CMD -------------
 *
 * The unicast addresses the L2 table holds for one port, in table order,
 * walked row by row with odi_switch_l2_next() (a bounded engine handshake
 * per row; the whole table is 1088 rows). Multicast rows belong to no port.
 */
#define ODI_SW_CMD61_AGE_DIV	7U	/* the age counter runs 7 .. 0 */

static int bridge_tbl_walk(uint32_t port, uint32_t first, struct omci_bridge_tbl *t,
			   uint32_t *total)
{
	struct odi_sw_l2_row row;
	uint32_t idx = 0, n = 0;
	/* Seconds an age step stands for, as the stock driver scales it:
	 * the ageing time (tenths of a second) over the counter's 7 steps. */
	uint32_t step = (odi_sw_l2_aging_ticks_get() / 10U) / ODI_SW_CMD61_AGE_DIV;

	for (;;) {
		int rc = odi_switch_l2_next(&idx, &row);

		if (rc == ODI_SW_L2_NEXT_NONE)
			break;
		if (rc)
			return rc;
		idx++;
		if (row.type != ODI_SW_L2_UCAST || row.port != port)
			continue;
		if (t && n >= first && n - first < OMCI_BRIDGE_TBL_PAGE) {
			uint8_t *e = t->entry + (n - first) * OMCI_BRIDGE_TBL_ENTRY_LEN;
			uint32_t age = (uint32_t)row.age * step;
			uint16_t info = (uint16_t)(((age & 0xfffU) << 4) |
				((row.flags & ODI_SW_L2_F_STATIC) ? 0U : (1U << 2)) |
				((row.flags & ODI_SW_L2_F_DA_BLOCK) ? 1U : 0U));

			e[0] = (uint8_t)(info >> 8);
			e[1] = (uint8_t)info;
			memcpy(e + 2, row.mac, 6);
			t->count = n - first + 1U;
		}
		n++;
	}
	*total = n;
	return 0;
}

/* cmd 61: {port, count}, the stock wrapper's shape. */
static int cmd_bridge_tbl_port(void *buf, uint32_t len)
{
	struct omci_bridge_tbl_per_port *a = buf;
	uint32_t total;
	int rc;

	if (!buf || len < sizeof(*a))
		return -1;
	if (a->portId >= ODI_SW_PORT_COUNT)
		return -22;
	rc = bridge_tbl_walk(a->portId, 0, NULL, &total);
	if (rc)
		return rc;
	a->cnt = (uint16_t)(total > 0xffffU ? 0xffffU : total);
	return 0;
}

/* OMCI_BRIDGE_TBL_CMD: one page of the entries, see uapi/omci_bridgeport.h. */
static int cmd_bridge_tbl(void *buf, uint32_t len)
{
	struct omci_bridge_tbl *t = buf;
	uint32_t port, first, total = 0;
	int rc;

	if (!buf || len < sizeof(*t))
		return -1;
	port = t->port;
	first = t->first;
	if (port >= ODI_SW_PORT_COUNT)
		return -22;
	memset(t->entry, 0, sizeof(t->entry));
	t->count = 0;
	rc = bridge_tbl_walk(port, first, t, &total);
	if (rc)
		return rc;
	t->total = total;
	return 0;
}

/* Bitmap helpers for the two flow tables. */
static int flow_used(const uint32_t *map, uint32_t id)
{
	return (map[id / 32U] >> (id % 32U)) & 1U;
}

static void flow_mark(uint32_t *map, uint32_t id)
{
	map[id / 32U] |= 1U << (id % 32U);
}

/* cmd 23: a priority queue (uapi/omci_gemflow.h, struct omci_priq), in one
 * of two register shapes, and the argument says which. A downstream queue
 * writes PORT_QUEUE_MAP; the ISP1 capture sends its eight before any T-CONT
 * exists, all with the same word. An upstream queue is queue `index` (its
 * ordinal) of the T-CONT `owner` (the index cmd 21 handed back), and lands
 * on that T-CONT's block: logical queue block start + ordinal. Its type
 * and weight come from wrr and weight, its rates from cir and pir, as the
 * stock driver sets them (a PIR of 0 with a CIR of 0 is no limit).
 * Provisioning the same MIB again writes the same words to the same
 * places. A queue outside the block, or on a T-CONT cmd 21 did not
 * create, is refused: written anyway, it would land in another T-CONT's
 * queues.
 */
static int cmd_pri_queue(void *buf, uint32_t len)
{
	const struct omci_priq *q = (const struct omci_priq *)buf;
	struct odi_sw_tcont_slot *s;
	uint32_t t, ord, lq, pq, weight, pir, i;

	if (!q || len < sizeof(*q))
		return -1;

	if (q->dir == OMCI_GEMFLOW_DS) {
		odi_sw_ponmac_queue_add(ODI_SW_CMD23_PORT_QUEUE_MAP);
		return 0;
	}
	if (q->dir != OMCI_GEMFLOW_US)
		return -1;

	t = q->owner;
	ord = q->index;
	if (t >= ODI_SW_PONQ_TCONTS || !odi_sw_tcont[t].used || ord >= odi_sw_tcont[t].qn) {
		ODI_SW_WARN("cmd 23: queue %u of T-CONT %u has no queue reserved for it\n",
			    (unsigned int)ord, (unsigned int)t);
		return ODI_SW_EOPNOTSUPP;
	}
	s = &odi_sw_tcont[t];
	lq = s->qbase + ord;
	pq = ponq_physical(t, lq);
	weight = q->weight;
	if (weight == 0U)
		weight = 1U;
	if (weight > ODI_SW_PONQ_WEIGHT_MAX)
		weight = ODI_SW_PONQ_WEIGHT_MAX;
	pir = (q->cir == 0U && q->pir == 0U) ? ODI_SW_PONQ_RATE_MAX : q->pir;
	odi_sw_ponq_queue_add(t, lq, pq, q->wrr != 0U, weight, q->cir, pir);
	s->live |= 1U << ord;
	/* The flows already mapped to this queue are valid again. */
	for (i = 0; i < ODI_SW_CMD_GEM_US_MAX; i++)
		if (flow_used(odi_sw_gem_us_used, i) && odi_sw_sid2qid[i] == pq)
			odi_sw_ponq_sidvalid_set(i, 1);
	return 0;
}

/* cmd 24: the upstream queue cmd 23 programmed, taken out: its flows
 * first (PON_SIDVALID), then the queue (odi_sw_ponq_queue_del()). A queue
 * not programmed, or a downstream one (no register), is no error. The
 * T-CONT's block is released with its last queue, for cmd 21 to reserve
 * again.
 */
static int cmd_pri_queue_del(void *buf, uint32_t len)
{
	const struct omci_priq *q = (const struct omci_priq *)buf;
	struct odi_sw_tcont_slot *s;
	uint32_t t, ord, lq, pq, i;

	if (!q || len < sizeof(*q))
		return -1;
	if (q->dir != OMCI_GEMFLOW_US)
		return 0;
	t = q->owner;
	ord = q->index;
	if (t >= ODI_SW_PONQ_TCONTS || ord >= 32U || !(odi_sw_tcont[t].live & (1U << ord)))
		return 0;
	s = &odi_sw_tcont[t];
	lq = s->qbase + ord;
	pq = ponq_physical(t, lq);
	for (i = 0; i < ODI_SW_CMD_GEM_US_MAX; i++)
		if (flow_used(odi_sw_gem_us_used, i) && odi_sw_sid2qid[i] == pq)
			odi_sw_ponq_sidvalid_set(i, 0);
	odi_sw_ponq_queue_del(t, lq, pq);
	s->live &= ~(1U << ord);
	if (!s->live)
		s->qn = 0;
	return 0;
}

/* cmd 0: a MIB reset, the upstream half (uapi/omci_gemflow.h). Every
 * upstream flow is deleted as cmd 25 ena 0 deletes one (GEM 0, not valid,
 * the PBO thresholds for one flow fewer), then every queue cmd 23 put on a
 * data T-CONT as cmd 24 deletes one (drained, out of the T-CONT's mask, the
 * T-CONT disabled with its last queue), and every T-CONT block is freed.
 * After it, cmd 21 reserves on an empty group again. The stock driver's
 * MIB reset (pf_rtl96xx_ResetMib) does this and more: it resets every flow
 * and destroys the logical T-CONTs, and omci_app sends it on an OLT MIB
 * reset. Without it a new provisioning met the old one's queue blocks, and
 * its flows stayed valid. Left alone: the Alloc-ID CAM (PLOAM's, G.984.3
 * 5.5.4), the OMCC (T-CONT 16, flow 64), the downstream GEM rows (no
 * downstream delete here; the bridge connections that forward them are
 * omcid's to take down) and the bridge connections themselves.
 */
static int us_flow_delete(uint32_t slot);

static int cmd_mib_reset(void *buf, uint32_t len)
{
	uint32_t i, t, ord;

	(void)buf; (void)len;
	uni_limits_restore();
	for (i = 0; i < ODI_SW_CMD_GEM_US_MAX; i++)
		(void)us_flow_delete(i);
	for (t = 0; t < ODI_SW_PONQ_TCONTS; t++) {
		struct odi_sw_tcont_slot *s = &odi_sw_tcont[t];

		for (ord = 0; ord < 32U; ord++)
			if (s->live & (1U << ord)) {
				uint32_t lq = s->qbase + ord;

				odi_sw_ponq_queue_del(t, lq, ponq_physical(t, lq));
			}
		s->alloc_id = 0;
		s->live = 0;
		s->used = 0;
		s->qbase = 0;
		s->qn = 0;
	}
	return 0;
}

/* cmd 21: the T-CONT of an Alloc-ID. Word 0 goes in as the number of
 * queues omcid will put on it (1 if 0 or out of range: the stock caller
 * leaves it uninitialised) and comes back as the T-CONT index, the
 * Alloc-ID's CAM row. The row is looked up, never written: an Alloc-ID
 * the OLT has not assigned by PLOAM in this activation is refused, and
 * omcid binds the T-CONT when the Assign_Alloc-ID arrives. The stock
 * driver writes a row here, but its caller sends this only on a T-CONT
 * Set; omcid also sends it when the PLOAM list changes, so a row written
 * here would bring back an Alloc-ID the OLT had just taken away, or one
 * that went with the ONU-ID, and the ONU would answer grants that are not
 * its own (G.984.3 5.5.3, 5.5.4). A row past the data T-CONTs (the
 * OMCC's, or one of rows 17-31, which have no scheduler) or a block that
 * does not fit in the group is refused.
 */
static int cmd_tcont(void *buf, uint32_t len)
{
	struct omci_tcont *t = (struct omci_tcont *)buf;
	uint32_t n;
	int row;

	if (!t || len < sizeof(*t))
		return -1;
	n = t->index;
	if (n == 0U || n > ODI_SW_PONQ_GROUP_QUEUES)
		n = 1U;
	row = tcont_alloc_row(t->alloc_id);
	if (row < 0) {
		ODI_SW_WARN("cmd 21: Alloc-ID %u is not assigned to this ONU by PLOAM\n",
			    (unsigned int)t->alloc_id);
		return ODI_SW_EOPNOTSUPP;
	}
	if (row >= (int)ODI_SW_PONQ_TCONTS) {
		ODI_SW_WARN("cmd 21: Alloc-ID %u has no data T-CONT (CAM row %d)\n",
			    (unsigned int)t->alloc_id, row);
		return ODI_SW_EOPNOTSUPP;
	}
	if (tcont_reserve((uint32_t)row, n)) {
		ODI_SW_WARN("cmd 21: no %u free queues for T-CONT %d\n", (unsigned int)n, row);
		return ODI_SW_EOPNOTSUPP;
	}
	odi_sw_tcont[row].alloc_id = t->alloc_id;
	odi_sw_tcont[row].used = 1;
	t->index = (uint32_t)row;
	return 0;
}

/* 1 + the highest flow id in use: what the readback reports as a count,
 * so that printing rows 0..count-1 shows every programmed row (an unused
 * row in between reads GEM 0).
 */
static uint32_t flow_extent(const uint32_t *map, uint32_t max)
{
	uint32_t id, n = 0;

	for (id = 0; id < max; id++)
		if (flow_used(map, id))
			n = id + 1U;
	return n;
}

static uint32_t us_flows_in_use(void)
{
	uint32_t i, n = 0;

	for (i = 0; i < ODI_SW_CMD_GEM_US_MAX; i++)
		if (flow_used(odi_sw_gem_us_used, i))
			n++;
	return n;
}

/* cmd 25 upstream with ena 0: the flow taken out, as the stock driver's
 * flow delete does it: GEM port 0 in its slot, its valid bit cleared, and
 * the PBO thresholds recomputed for one data flow fewer. A slot not in
 * use is no error.
 */
static int us_flow_delete(uint32_t slot)
{
	if (slot >= ODI_SW_CMD_GEM_US_MAX)
		return ODI_SW_EOPNOTSUPP;
	if (!flow_used(odi_sw_gem_us_used, slot))
		return 0;
	odi_sw_ponq_us_gem_set(slot, 0);
	odi_sw_ponq_sidvalid_set(slot, 0);
	odi_sw_gem_us_used[slot / 32U] &= ~(1U << (slot % 32U));
	odi_sw_ponq_glb_th_set(us_flows_in_use());
	odi_sw_sid2qid[slot] = ODI_SW_SID_UNMAPPED;
	if (slot < OMCI_FLOWS_MAX)
		odi_sw_gem_us_gem[slot] = 0;
	return 0;
}

/* cmd 25: dir picks the table, the flow id the row (see the state
 * above), ena 1 creates or updates the flow and ena 0 deletes it (the
 * stock driver's reading of the field). A downstream flow is Ethernet
 * (FLAGS 2), plus the multicast bit (3) when omcid sets mcast_filter: the
 * stock OMCI layer sets its isFilterMcast for the GEM port of a multicast
 * GEM interworking TP (ME 281) with an address table row
 * (mib_MultiGemIwTp.c, read, not copied), and its flow add writes the bit
 * from it. Both used to be keyed on GEM port 0xfff, which is only ISP1's
 * multicast GEM (downstream only, hence the capture's 6 downstream and 5
 * upstream instances): ISP2's multicast GEM (0xffe) and any other OLT's got
 * 2, and an OLT whose 0xfff is a two-way data port would have had its
 * upstream flow dropped. omcid sends an upstream flow only for a CTP that
 * has one, so nothing is keyed on the port number any more.
 */
static int cmd_gem_flow(void *buf, uint32_t len)
{
	struct omci_gemflow *g = (struct omci_gemflow *)buf;
	int rc;

	if (!g || len < sizeof(*g))
		return -1;

	if (g->dir == OMCI_GEMFLOW_DS) {
		uint32_t idx = g->flow_id;
		uint32_t traffic_cfg = g->mcast_filter ? 3U : 2U;

		if (idx >= ODI_SW_CMD_GEM_DS_MAX)
			return ODI_SW_EOPNOTSUPP;
		/* omcid never deletes a downstream flow, and this driver has
		 * no downstream delete: refused rather than taken for a
		 * create.
		 */
		if (!g->ena) {
			ODI_SW_WARN("cmd 25: downstream flow %u delete not implemented\n",
				    (unsigned int)idx);
			return ODI_SW_EOPNOTSUPP;
		}
		/* A CAM write that did not complete is no flow: omcid does
		 * not claim it on a nonzero return, and neither does this
		 * table.
		 */
		rc = odi_sw_gpon_usflow_set(idx, g->gem_port, traffic_cfg);
		if (rc)
			return rc;
		if (idx < OMCI_FLOWS_MAX) {
			odi_sw_gem_ds_gem[idx] = g->gem_port;
			odi_sw_gem_ds_cfg[idx] = traffic_cfg;
		}
		flow_mark(odi_sw_gem_ds_used, idx);
		if (odi_sw_flow_ops && odi_sw_flow_ops->set)
			odi_sw_flow_ops->set(0, idx, g->gem_port);
		return 0;
	}

	if (g->dir == OMCI_GEMFLOW_US) {
		uint32_t slot = g->flow_id, t = g->tcont, ord = g->queue, pq;
		int creating;

		if (!g->ena)
			return us_flow_delete(slot);
		if (slot >= ODI_SW_CMD_GEM_US_MAX)
			return ODI_SW_EOPNOTSUPP;
		/* The flow queues on queue `queue` (the ordinal) of T-CONT
		 * `tcont`: the queue cmd 23 placed at that ordinal of the
		 * T-CONT's block. A queue the T-CONT has no room for has no
		 * physical queue to map to.
		 */
		if (t >= ODI_SW_PONQ_TCONTS || !odi_sw_tcont[t].used || ord >= odi_sw_tcont[t].qn) {
			ODI_SW_WARN("cmd 25: upstream flow %u on queue %u of T-CONT %u, "
				    "which has no such queue\n", (unsigned int)slot,
				    (unsigned int)ord, (unsigned int)t);
			return ODI_SW_EOPNOTSUPP;
		}
		pq = ponq_physical(t, odi_sw_tcont[t].qbase + ord);
		creating = !flow_used(odi_sw_gem_us_used, slot);
		/* The stock order: GEM port, valid bit, the thresholds (on a
		 * create only), the queue.
		 */
		odi_sw_ponq_us_gem_set(slot, g->gem_port);
		odi_sw_ponq_sidvalid_set(slot, 1);
		if (creating) {
			flow_mark(odi_sw_gem_us_used, slot);
			odi_sw_ponq_glb_th_set(us_flows_in_use());
		}
		odi_sw_ponq_sid2qid_set(slot, pq);
		odi_sw_sid2qid[slot] = (uint8_t)pq;
		if (slot < OMCI_FLOWS_MAX)
			odi_sw_gem_us_gem[slot] = g->gem_port;
		if (odi_sw_flow_ops && odi_sw_flow_ops->set)
			odi_sw_flow_ops->set(1, slot, g->gem_port);
		return 0;
	}

	return -1; /* neither US nor DS -- malformed request */
}

/* OMCI_FLOWS_CMD, ours (uapi/omci_flows.h): the cmd 25 record, read
 * back. Touches no hardware.
 */
static int cmd_flows_get(void *buf, uint32_t len)
{
	struct omci_flows *f = (struct omci_flows *)buf;
	unsigned int i;

	if (!f || len < OMCI_FLOWS_LEN)
		return -1;
	f->ds_count = flow_extent(odi_sw_gem_ds_used, ODI_SW_CMD_GEM_DS_MAX);
	f->us_count = flow_extent(odi_sw_gem_us_used, ODI_SW_CMD_GEM_US_MAX);
	for (i = 0; i < OMCI_FLOWS_MAX; i++) {
		f->ds_gem[i] = odi_sw_gem_ds_gem[i];
		f->ds_cfg[i] = odi_sw_gem_ds_cfg[i];
		f->us_gem[i] = odi_sw_gem_us_gem[i];
	}
	return 0;
}

/* cmd 26: no register write in any capture (the stock driver changes only
 * ACL state no capture shows).
 */
static int cmd_ds_bc_gem_flow(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	return 0;
}

/* cmd 10: one SFF-8472 DDM word of the optics, for the ANI-G Test result
 * omcid sends (src/omci/respond/omcimsg.c, ddm_word()). The argument is
 * {type, data[32]}: type a big-endian word at bytes 0-3, one of the
 * odi_ddm.h numeric selectors (2 temperature, 3 supply voltage, 4 bias
 * current, 5 Tx power, 6 Rx power); the answer is the raw A2h word,
 * big-endian, at data[0..1], the rest of data zeroed. The read is the
 * one DDM_GET makes, but through odi_ddm_get_stock(): the writes are the
 * stock command's, device select A2h, byte address, start, once per byte,
 * and are replayed against its capture (test/odi_switch_cmd_test.sh), so
 * they cannot take the extra high-byte read odi_ddm_get() adds against a
 * torn word. Any other type, a short buffer or
 * a failed read is an error, and omcid then reports 0 for that field.
 *
 * ODI_DDM_ALARM_STATUS is the one other type: the module's own alarm and
 * warning flags, A2h bytes 110-117 (odi_ddm.h has the layout), at
 * data[0..7]. omcid takes the laser bias alarm (ANI-G alarm 6) from them;
 * the module raises them against its own thresholds, which G.988 has no
 * attribute for. It needs room for all eight bytes.
 */
#define ODI_SW_CMD10_DATA_OFF	4U
#define ODI_SW_CMD10_MIN_LEN	(ODI_SW_CMD10_DATA_OFF + 2U)
#define ODI_SW_CMD10_ALARM_LEN	8U

static int cmd_transceiver_status(void *buf, uint32_t len)
{
	uint8_t *b = (uint8_t *)buf;
	uint8_t raw[ODI_DDM_BUF_LEN];
	uint32_t type, i, n;

	if (!b || len < ODI_SW_CMD10_MIN_LEN)
		return -1;
	type = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | b[3];
	if (type == ODI_DDM_ALARM_STATUS) {
		if (len < ODI_SW_CMD10_DATA_OFF + ODI_SW_CMD10_ALARM_LEN)
			return -1;
		n = ODI_SW_CMD10_ALARM_LEN;
	} else if (type >= ODI_DDM_TEMPERATURE && type <= ODI_DDM_RX_POWER) {
		n = 2U;
	} else {
		return -1;
	}
	if (odi_ddm_get_stock((int)type, raw) != 0)
		return -1;
	for (i = ODI_SW_CMD10_DATA_OFF; i < len; i++)
		b[i] = 0;
	for (i = 0; i < n; i++)
		b[ODI_SW_CMD10_DATA_OFF + i] = raw[i];
	return 0;
}

/* cmd 3, 4, 15 and 13 write no register, but omcid reads all four before
 * anything else and logs each one it could not read, so they answer with
 * real content.
 */

/* cmd 3: the 120-byte capability block (uapi/omci_caps.h has the offsets
 * and the values the stock driver reports on this board). The fields with
 * no observed value (RGMII port, POTS ports, queue depths, meters, L2
 * table size) are conservative defaults, marked below.
 */
static int cmd_get_dev_capabilities(void *buf, uint32_t len)
{
	uint8_t *b = (uint8_t *)buf;
	uint32_t i;

	if (!b || len < OMCI_CAPS_LEN)
		return -1;
	for (i = 0; i < (uint32_t)OMCI_CAPS_LEN; i++)
		b[i] = 0;
	for (i = 0; i < OMCI_CAPS_UNI_SLOTS; i++) {
		b[i * 2 + 0] = 0;
		b[i * 2 + 1] = 0xff; /* no slot, for the read loop of omcid */
	}
	b[0 * 2 + 0] = OMCI_UNI_SLOT_PPTP; /* uni slot 0: a PPTP Ethernet UNI */
	b[0 * 2 + 1] = 0;                  /* switch port 0 */
	/* +64 FE ports 0, +68 GE ports 1. */
	b[64] = 0; b[65] = 0; b[66] = 0; b[67] = 0;
	b[68] = 0; b[69] = 0; b[70] = 0; b[71] = 1;
	/* +72 CPU port 3, +76 PON port 2, +80 RGMII port -1 (none; default). */
	b[OMCI_CAPS_OFF_CPUPORT + 3] = 3;
	b[OMCI_CAPS_OFF_PONPORT + 3] = 2;
	b[80] = 0xff; b[81] = 0xff; b[82] = 0xff; b[83] = 0xff; /* RGMII port = -1 */
	/* +84 POTS ports 0: no voice hardware (default). */
	/* +88 T-CONTs 16 (observed). */
	b[OMCI_CAPS_OFF_TCONTS + 3] = 16;
	/* +92 GEM ports 64, +96 T-CONT queues 128 (observed). */
	b[OMCI_CAPS_OFF_FLOWS + 2] = 0; b[OMCI_CAPS_OFF_FLOWS + 3] = 64;
	b[OMCI_CAPS_OFF_PRIQ + 3] = 128;
	/* +100 queues per UNI 8: the queues cmd 23 creates per UNI (inferred). */
	b[OMCI_CAPS_OFF_UNIQ + 3] = 8;
	/* +104/+105 one drop-precedence level per T-CONT and per UNI queue;
	 * +108 meters, +112 reserved meter id, +116 L2 table size 0 (defaults).
	 */
	b[104] = 1;
	b[105] = 1;
	return 0;
}

/* cmd 4: 40 bytes, of which omcid reads the NUL-terminated chip name. */
static int cmd_get_dev_id_version(void *buf, uint32_t len)
{
	static const char devid[9] = "RTL9602C";
	uint8_t *b = (uint8_t *)buf;
	uint32_t i;

	if (!b || len < 40)
		return -1;
	for (i = 0; i < len; i++)
		b[i] = 0;
	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)devid[i];
	return 0;
}

/* cmd 15: the 8-byte G.984 ONU serial number from odi_gpon (4-byte vendor
 * id, 4-byte vendor-specific part) and a NUL; omcid reads 9 bytes.
 */
static int cmd_get_serial_num(void *buf, uint32_t len)
{
	uint8_t *b = (uint8_t *)buf;

	if (!b || len < 9)
		return -1;
#ifdef __KERNEL__
	{
		u8 sn[8];
		int i;

		odi_gpon_sn_get(sn);
		for (i = 0; i < 8; i++)
			b[i] = sn[i];
		b[8] = 0;
	}
	return 0;
#else
	/* Host build: no GPON core, and no host test sends this. */
	(void)b;
	return ODI_SW_EOPNOTSUPP;
#endif
}

/* cmd 13: the ONU state number (O1..O7) as a raw host-order word. */
static int cmd_get_onu_state(void *buf, uint32_t len)
{
	if (!buf || len < sizeof(uint32_t))
		return -1;
#ifdef __KERNEL__
	*(uint32_t *)buf = (uint32_t)odi_gpon_onu_state();
	return 0;
#else
	return ODI_SW_EOPNOTSUPP;
#endif
}

/* cmd 45 and 47, the statistics resets: no register write in any
 * capture, so the stock driver is taken to do nothing, and they succeed.
 */
static int cmd_stat_reset_noop(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	return 0;
}

/* Why a command is refused, for the commands whose absence is a decision or
 * a gap with a known cause rather than "no capture exercises it". It is the
 * line a field log shows, so it names the command's effect. NULL: nothing
 * more is known than that no provisioning run has sent it.
 */
static const char *unsupported_why(uint32_t cmd)
{
	switch (cmd) {
	case 34:
		return "max frame size: the switch has two frame-length profiles, the PON port uses one and the host and CPU ports share the other, so a per-UNI limit cannot be set without moving the CPU's";
	case 52:
		return "DSCP remap: not built, the path from the priority it sets to the P-bit of an upstream frame is not understood";
	case 54:
	case 55:
		return "MAC filters: need ACL rules; omcid does not send them";
	case 56:
		return "S-VLAN TPID: omcid does not send it";
	case 59:
	case 60:
		return "dot1 rate limiter: needs an ACL rule and a meter per GEM port, not built";
	case 71:
		return "UNI QoS: omcid does not send it";
	default:
		return NULL;
	}
}

/* Logged once per command number, not once in all: ODI_SW_CMD_LOG is
 * once per call site, so the first unsupported command of a boot hid every
 * other one, and a field log could not say which commands a new OLT needs.
 */
#define ODI_SW_CMD_NUMBERS	128U
static uint32_t odi_sw_unsupported_seen[ODI_SW_CMD_NUMBERS / 32U];

static int cmd_unsupported(uint32_t cmd, void *buf, uint32_t len)
{
	const char *why;

	(void)buf; (void)len;
	if (cmd >= ODI_SW_CMD_NUMBERS) {
		ODI_SW_WARN("cmd %u: no such command\n", (unsigned int)cmd);
		return ODI_SW_EOPNOTSUPP;
	}
	if (!(odi_sw_unsupported_seen[cmd / 32U] & (1U << (cmd % 32U)))) {
		odi_sw_unsupported_seen[cmd / 32U] |= 1U << (cmd % 32U);
		why = unsupported_why(cmd);
		if (why)
			ODI_SW_LOG("cmd %u not implemented (%s)\n", (unsigned int)cmd, why);
		else
			ODI_SW_LOG("cmd %u not implemented (not exercised on isp1/isp2)\n",
				   (unsigned int)cmd);
	}
	return ODI_SW_EOPNOTSUPP;
}

/* The command numbers are those omcid passes to omci_drv_call(); the
 * stock driver name of each is in the comment. omcid reaches this only
 * through the odi_omci netlink socket.
 */
int odi_switch_cmd(uint32_t cmd, void *buf, uint32_t len)
{
	switch (cmd) {
	case OMCI_CAPS_CMD:	/* 3, getDevCapabilities */
		return cmd_get_dev_capabilities(buf, len);
	case OMCI_DEV_ID_VERSION_CMD:	/* 4, getDevIdVersion */
		return cmd_get_dev_id_version(buf, len);
	case OMCI_SERIAL_NUM_CMD:	/* 15, getSerialNum */
		return cmd_get_serial_num(buf, len);
	case OMCI_ONU_STATE_CMD:	/* 13, getOnuState */
		return cmd_get_onu_state(buf, len);

	case OMCI_AGEING_TIME_CMD:	/* 62, setAgeingTime */
		return cmd_mac_age_time(buf, len);

	case OMCI_PORT_AUTO_NEGO_CMD:	/* 30, setPortAutoNegoAbility */
		return cmd_port_auto_nego(buf, len);

	case OMCI_PORT_STATE_CMD:	/* 32, setPortState */
		return cmd_port_state(buf, len);

	case OMCI_SIGNAL_PARAMETER_CMD:	/* 11, setSignalParameter */
		return cmd_signal_parameter(buf, len);

	case OMCI_PHY_PWRDOWN_CMD:	/* 38, setPhyPwrDown */
		return cmd_port_phy_pwrdown(buf, len);

	case OMCI_FLOOD_CMD:	/* 64, setFloodingPortMask */
		return cmd_flooding_port_mask(buf, len);

	case OMCI_PRIQ_CMD:	/* 23, setPriQueue (also createPriQByTcontId) */
		return cmd_pri_queue(buf, len);

	case OMCI_PRIQ_DEL_CMD:	/* 24, clearPriQueue */
		return cmd_pri_queue_del(buf, len);

	case OMCI_TCONT_CMD:	/* 21, createTcont/updateTcont */
		return cmd_tcont(buf, len);

	case OMCI_MIB_RESET_CMD:	/* 0, resetMib */
		return cmd_mib_reset(buf, len);

	case OMCI_GEMFLOW_CMD:	/* 25, cfgGemFlow/updateGemFlow/updateUsGemFlow */
		return cmd_gem_flow(buf, len);

	case OMCI_BDGCONN_CMD:	/* 51, activeBdgConn */
		return odi_switch_bdgconn_activate(buf, len);

	case OMCI_BDGCONN_OFF_CMD:	/* 50, deactiveBdgConn */
		return odi_switch_bdgconn_deactivate(buf, len);

	case OMCI_TRANSCEIVER_STATUS_CMD:	/* 10, getTransceiverStatus */
		return cmd_transceiver_status(buf, len);

	case OMCI_DS_BC_GEMFLOW_CMD:	/* 26, setDsBcGemFlow */
		return cmd_ds_bc_gem_flow(buf, len);

	case OMCI_FLOWS_CMD:	/* ours: read back the cmd 25 record */
		return cmd_flows_get(buf, len);

	case OMCI_LEARN_LIMIT_CMD:	/* 53, setMacLearnLimit */
		return cmd_mac_learn_limit(buf, len);

	case OMCI_UNIRATE_CMD:	/* 67, setUniPortRate */
		return cmd_uni_rate(buf, len);

	case OMCI_PORT_BRIDGING_CMD:	/* 63, setPortBridging */
		return cmd_port_bridging(buf, len);

	case OMCI_PAUSE_CONTROL_CMD:	/* 40, setPauseControl */
		return cmd_pause_control(buf, len);

	case OMCI_BRIDGE_TBL_PORT_CMD:	/* 61, getBridgeTableByPort */
		return cmd_bridge_tbl_port(buf, len);

	case OMCI_BRIDGE_TBL_CMD:	/* ours: the addresses behind cmd 61 */
		return cmd_bridge_tbl(buf, len);

	case OMCI_RESET_US_FLOW_STAT_CMD:	/* 45, resetUsFlowStat */
	case OMCI_RESET_DS_FLOW_STAT_CMD:	/* 47, resetDsFlowStat */
		return cmd_stat_reset_noop(buf, len);

	default:
		return cmd_unsupported(cmd, buf, len);
	}
}
