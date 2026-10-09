// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_cmd.c -- odi_switch_cmd(), the OMCI driver command dispatch
 * (odi_switch_cmd.h), and the commands other than cmd 51/50
 * (odi_switch_bdgconn.c). Every register write goes through a leaf of
 * odi_switch_dal.h; no register offset appears here.
 *
 * Values marked "replayed" are what the ISP1 provisioning capture shows
 * per instance (test/fixtures/isp1-260922-boot5.txt), written by instance
 * ordinal because their fields are not decoded.
 */
#include "odi_switch_cmd.h"
#include "odi_switch_dal.h"
#include "odi_gpon.h"

#include "uapi/omci_gemflow.h"
#include "uapi/omci_bdgconn.h"
#include "uapi/omci_bridgeport.h"
#include "uapi/omci_caps.h"
#include "uapi/omci_flows.h"

/* cmd 21: the T-CONT list, Alloc-ID to index, appended in creation order.
 * No capture deletes a T-CONT, and cmd 21 writes no register.
 */
struct odi_sw_tcont_slot {
	uint32_t alloc_id;
	int used;
};
static struct odi_sw_tcont_slot odi_sw_tcont[ODI_SW_CMD_TCONT_MAX];
static unsigned int odi_sw_tcont_n;

/* cmd 23, upstream: the T-CONTs that have their queue, one bit per T-CONT
 * index (the scheduler the queue belongs to). PONQ_COUNT_MASK +207 is this
 * set plus the OMCC T-CONT (below), rewritten by every upstream queue.
 */
static uint32_t odi_sw_us_sched_used;

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

/* cmd 10, replayed: the GPIO 29 pair of each call. It varies from call to
 * call with no OMCI attribute behind it, so it is counted here: the five
 * captured pairs in order, then the fifth again (no capture has a sixth).
 */
static const uint32_t odi_sw_cmd10_gpio_lo[] = { 0x62, 0x68, 0x66, 0x64, 0x60 };
static const uint32_t odi_sw_cmd10_gpio_hi[] = { 0x63, 0x69, 0x67, 0x65, 0x61 };
static unsigned int odi_sw_transceiver_n;

/* cmd 25 upstream, PON_SID_GLB_TH: the global ON/OFF thresholds, two
 * writes (ON, then OFF) per flow, by the number of upstream flows in use.
 * The capture has one to five; each flow lowers ON by 150, OFF trails it
 * by 160. Past five the fifth pair is kept: a sixth value would be a
 * guess, and the fifth pair is what a six-flow ISP ran with (issue #42).
 */
static const uint32_t odi_sw_cmd25_glb_th_on[] = {
	0x1ee01b08U, 0x1e4a1e40U, 0x1db41daaU, 0x1d1e1d14U, 0x1c881c7eU,
};
static const uint32_t odi_sw_cmd25_glb_th_off[] = {
	0x1ee01e40U, 0x1e4a1daaU, 0x1db41d14U, 0x1d1e1c7eU, 0x1c881be8U,
};

/* cmd 25 upstream, PON_SID2QID: the upstream queue of each flow, 7 bits a
 * flow, four flows a word. Kept here and written a whole word at a time;
 * a flow nothing has mapped reads 63, the value the capture shows for
 * every flow not yet programmed.
 */
#define ODI_SW_SID2QID_BITS	7U
#define ODI_SW_SID2QID_PER_WORD	4U
#define ODI_SW_SID2QID_UNMAPPED	0x3fU
static uint8_t odi_sw_sid2qid[ODI_SW_CMD_GEM_US_MAX];

static uint32_t sid2qid_word(uint32_t w)
{
	uint32_t i, v = 0;

	for (i = 0; i < ODI_SW_SID2QID_PER_WORD; i++)
		v |= (uint32_t)odi_sw_sid2qid[w * ODI_SW_SID2QID_PER_WORD + i]
		     << (i * ODI_SW_SID2QID_BITS);
	return v;
}

/* cmd 23, upstream, replayed: the PONQ_COUNT_MASK words of the five
 * captured queues whose fields are not decoded, by T-CONT index (one queue
 * per T-CONT in the capture, T-CONT n holding queue n).
 */
static const uint32_t odi_sw_cmd23_base_15[] = { 0x6U, 0xeU, 0x16U, 0x1eU, 0x26U };
static const uint32_t odi_sw_cmd23_val_208[] = { 0x0U, 0x2U, 0x6U, 0xeU, 0x1eU };
static const uint32_t odi_sw_cmd23_bitmask_212_213[] = {
	0x100401U, 0x107801U, 0x1807801U, 0x100404U, 0x101004U,
};
static const int odi_sw_cmd23_use_213[] = { 0, 0, 0, 1, 1 };

/* The T-CONT schedulers of PONQ_COUNT_MASK: +190+n is the word of T-CONT
 * n, +207 the set of T-CONTs in use, one bit each. T-CONT 16 is the OMCC,
 * on the default Alloc-ID (the ONU-ID, Alloc-ID CAM row 16): the module-load
 * replay writes its word, +206 = 1, and +207 = 0x10000 before any OMCI, and
 * the ISP1 capture adds one bit per T-CONT to that 0x10000 (0x10001 ..
 * 0x1001f). So an upstream queue may use T-CONTs 0-15 only: n = 16 would
 * overwrite the OMCC word and n = 17 would be +207 itself. That is also the
 * 16 T-CONTs cmd 3 reports.
 */
#define ODI_SW_CMD23_US_TCONTS		16U
#define ODI_SW_CMD23_OMCC_TCONT_BIT	(1U << 16)

/* Replayed scalars. */
#define ODI_SW_CMD23_PORT_QUEUE_MAP	0xd4U	/* cmd 23, a downstream queue */
#define ODI_SW_CMD30_A408		0xde1U	/* cmd 30, the three UNI PHY words */
#define ODI_SW_CMD30_A412		0x0U
#define ODI_SW_CMD30_A400		0x3a00U
#define ODI_SW_CMD62_AGE_TICKS		3000U	/* cmd 62 when omcid sends 0 */
#define ODI_SW_CMD62_AGE_ON_LINK_DOWN	1U

void odi_switch_cmd_reset_state(void)
{
	unsigned int i;

	odi_sw_tcont_n = 0;
	odi_sw_us_sched_used = 0;
	odi_sw_transceiver_n = 0;
	for (i = 0; i < ODI_SW_CMD_TCONT_MAX; i++)
		odi_sw_tcont[i].used = 0;
	odi_switch_bdgconn_reset();
	for (i = 0; i < sizeof(odi_sw_gem_ds_used) / sizeof(odi_sw_gem_ds_used[0]); i++)
		odi_sw_gem_ds_used[i] = 0;
	for (i = 0; i < sizeof(odi_sw_gem_us_used) / sizeof(odi_sw_gem_us_used[0]); i++)
		odi_sw_gem_us_used[i] = 0;
	for (i = 0; i < ODI_SW_CMD_GEM_US_MAX; i++)
		odi_sw_sid2qid[i] = ODI_SW_SID2QID_UNMAPPED;
	for (i = 0; i < OMCI_FLOWS_MAX; i++) {
		odi_sw_gem_ds_gem[i] = 0;
		odi_sw_gem_ds_cfg[i] = 0;
		odi_sw_gem_us_gem[i] = 0;
	}
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

/* cmd 32: port 0, the UNI (the flood mask of cmd 64 is a different
 * register).
 */
static int cmd_port_state(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	(void)odi_sw_port_force_get(0);
	odi_sw_port_force_set(0);
	return 0;
}

/* cmd 38: no register write in any capture. */
static int cmd_port_phy_pwrdown(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	return 0;
}

/* cmd 64: the unknown-unicast flood mask, 0x7 (UNI, port 1, PON) when
 * enabled. The leaf repeats the write as the capture does.
 */
static int cmd_flooding_port_mask(void *buf, uint32_t len)
{
	struct omci_flood *f = (struct omci_flood *)buf;
	uint32_t mask = 7;

	if (f && len >= sizeof(*f)) {
		mask = f->enable ? 7 : 0;
	}
	(void)odi_sw_l2_flood_mask_get();
	odi_sw_l2_flood_mask_set(mask);
	return 0;
}

/* cmd 23: a priority queue (uapi/omci_gemflow.h, struct omci_priq), in one
 * of two register shapes, and the argument says which. A downstream queue
 * writes PORT_QUEUE_MAP; the ISP1 capture sends its eight before any T-CONT
 * exists, all with the same word. An upstream queue writes the scheduler
 * words of the T-CONT it names (owner, the index cmd 21 handed back): its
 * slot is that T-CONT, not a count of calls, so provisioning the same MIB
 * again -- a MIB reset, a respawned omcid, a re-registration -- writes the
 * same words to the same places. Only the captured shape is programmed,
 * one queue per T-CONT on T-CONTs 0-15; anything else is refused rather
 * than written into the words of another T-CONT, the OMCC among them.
 */
static int cmd_pri_queue(void *buf, uint32_t len)
{
	const struct omci_priq *q = (const struct omci_priq *)buf;
	unsigned int t, ref;

	if (!q || len < sizeof(*q))
		return -1;

	if (q->dir == OMCI_GEMFLOW_DS) {
		odi_sw_ponmac_queue_add(ODI_SW_CMD23_PORT_QUEUE_MAP);
		return 0;
	}
	if (q->dir != OMCI_GEMFLOW_US)
		return -1;

	t = q->owner;
	if (t >= ODI_SW_CMD23_US_TCONTS || q->index != 0) {
		ODI_SW_CMD_LOG("cmd 23: upstream queue %u of T-CONT %u not supported "
			       "(one queue per T-CONT, T-CONTs 0-%u)\n", (unsigned int)q->index,
			       t, ODI_SW_CMD23_US_TCONTS - 1U);
		return ODI_SW_EOPNOTSUPP;
	}
	/* Past the five captured T-CONTs, the undecoded words of the last. */
	ref = t < 5U ? t : 4U;
	odi_sw_us_sched_used |= 1U << t;
	odi_sw_ponmac_queue_add_ext(t, ODI_SW_CMD23_OMCC_TCONT_BIT | odi_sw_us_sched_used,
				     odi_sw_cmd23_base_15[ref], 1U << t,
				     odi_sw_cmd23_val_208[ref], odi_sw_cmd23_bitmask_212_213[ref],
				     odi_sw_cmd23_use_213[ref]);
	return 0;
}

/* cmd 21: no register write in any capture. The index goes back to
 * omcid in the argument (omci_tcont.index).
 */
static int cmd_tcont(void *buf, uint32_t len)
{
	struct omci_tcont *t = (struct omci_tcont *)buf;
	uint32_t alloc_id = 0;
	unsigned int i;

	if (t && len >= sizeof(*t))
		alloc_id = t->alloc_id;

	for (i = 0; i < odi_sw_tcont_n; i++) {
		if (odi_sw_tcont[i].used && odi_sw_tcont[i].alloc_id == alloc_id) {
			if (t && len >= sizeof(*t))
				t->index = i;
			return 0;
		}
	}
	if (odi_sw_tcont_n >= ODI_SW_CMD_TCONT_MAX)
		return ODI_SW_EOPNOTSUPP;
	odi_sw_tcont[odi_sw_tcont_n].alloc_id = alloc_id;
	odi_sw_tcont[odi_sw_tcont_n].used = 1;
	if (t && len >= sizeof(*t))
		t->index = odi_sw_tcont_n;
	odi_sw_tcont_n++;
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

/* cmd 25: dir picks the table, the flow id the row (see the state
 * above). The OMCI/broadcast GEM port (0xfff) is downstream only, which
 * gives the 6 downstream and 5 upstream instances of the capture.
 */
static int cmd_gem_flow(void *buf, uint32_t len)
{
	struct omci_gemflow *g = (struct omci_gemflow *)buf;

	if (!g || len < sizeof(*g))
		return -1;

	if (g->dir == OMCI_GEMFLOW_DS) {
		uint32_t idx = g->flow_id;
		uint32_t traffic_cfg = (g->gem_port == 0xfffU) ? 3U : 2U;

		if (idx >= ODI_SW_CMD_GEM_DS_MAX)
			return ODI_SW_EOPNOTSUPP;
		odi_sw_gpon_usflow_set(idx, g->gem_port, traffic_cfg);
		if (idx < OMCI_FLOWS_MAX) {
			odi_sw_gem_ds_gem[idx] = g->gem_port;
			odi_sw_gem_ds_cfg[idx] = traffic_cfg;
		}
		flow_mark(odi_sw_gem_ds_used, idx);
		return 0;
	}

	if (g->dir == OMCI_GEMFLOW_US) {
		uint32_t slot = g->flow_id, sidvalid = 0, n = 0, i;

		if (g->gem_port == 0xfffU) {
			/* OMCI channel: downstream-only, no US table entry. */
			return 0;
		}
		if (slot >= ODI_SW_CMD_GEM_US_MAX)
			return ODI_SW_EOPNOTSUPP;
		/* The flow queues on its T-CONT's queue. cmd 23 gives T-CONT
		 * t one queue, physical queue t (its PON_SCH_QMAP word is
		 * 1 << t), and refuses any other, so a flow on queue 0 of
		 * T-CONT t maps to queue t.
		 */
		if (g->tcont >= ODI_SW_CMD23_US_TCONTS || g->queue != 0) {
			ODI_SW_CMD_LOG("cmd 25: upstream flow %u on queue %u of T-CONT %u "
				       "not supported (one queue per T-CONT, T-CONTs 0-%u)\n",
				       (unsigned int)slot, (unsigned int)g->queue,
				       (unsigned int)g->tcont, ODI_SW_CMD23_US_TCONTS - 1U);
			return ODI_SW_EOPNOTSUPP;
		}
		flow_mark(odi_sw_gem_us_used, slot);
		odi_sw_sid2qid[slot] = (uint8_t)g->tcont;

		/* PON_SIDVALID: one bit per upstream flow in use, the word
		 * this flow is in (1, 3, 7, 0xf, 0x1f on ISP1).
		 */
		for (i = 0; i < 32U; i++)
			if (flow_used(odi_sw_gem_us_used, (slot & ~31U) + i))
				sidvalid |= 1U << i;
		for (i = 0; i < ODI_SW_CMD_GEM_US_MAX; i++)
			if (flow_used(odi_sw_gem_us_used, i))
				n++;
		{
			unsigned int ref = n < 5U ? n - 1U : 4U;
			uint32_t w = slot / ODI_SW_SID2QID_PER_WORD;

			odi_sw_ponmac_flow_queue_set(slot, g->gem_port, slot / 32U, sidvalid,
						      odi_sw_cmd25_glb_th_on[ref],
						      odi_sw_cmd25_glb_th_off[ref],
						      w, sid2qid_word(w));
		}
		if (slot < OMCI_FLOWS_MAX)
			odi_sw_gem_us_gem[slot] = g->gem_port;
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

/* cmd 10 (odi_sw_cmd10_gpio_lo above has the replay). */
static int cmd_transceiver_status(void *buf, uint32_t len)
{
	unsigned int n = odi_sw_transceiver_n;

	(void)buf; (void)len;
	if (n >= 5)
		n = 4;
	odi_sw_ponmac_transceiver_get(odi_sw_cmd10_gpio_lo[n], odi_sw_cmd10_gpio_hi[n]);
	odi_sw_transceiver_n++;
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

static int cmd_unsupported(uint32_t cmd, void *buf, uint32_t len)
{
	(void)buf; (void)len;
	ODI_SW_CMD_LOG("cmd %u not implemented (not exercised on isp1/isp2)\n",
		       cmd);
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

	case OMCI_PHY_PWRDOWN_CMD:	/* 38, setPhyPwrDown */
		return cmd_port_phy_pwrdown(buf, len);

	case OMCI_FLOOD_CMD:	/* 64, setFloodingPortMask */
		return cmd_flooding_port_mask(buf, len);

	case OMCI_PRIQ_CMD:	/* 23, setPriQueue (also createPriQByTcontId) */
		return cmd_pri_queue(buf, len);

	case OMCI_TCONT_CMD:	/* 21, createTcont/updateTcont */
		return cmd_tcont(buf, len);

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

	case OMCI_RESET_US_FLOW_STAT_CMD:	/* 45, resetUsFlowStat */
	case OMCI_RESET_DS_FLOW_STAT_CMD:	/* 47, resetDsFlowStat */
		return cmd_stat_reset_noop(buf, len);

	default:
		return cmd_unsupported(cmd, buf, len);
	}
}
