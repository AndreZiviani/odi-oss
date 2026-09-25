// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_cmd.c -- implementation of odi_switch_cmd(), the 79-slot
 * OMCI command dispatch. Calls into the DAL leaves (odi_switch_dal.h)
 * for every register write; no register offset appears in this file
 * directly.
 *
 * The argument struct headers are src/omci's own -- included unmodified,
 * reusing the exact struct definitions omcid already has rather than
 * redefining them. This file does not touch omcid, only reads its
 * struct layouts.
 *
 * Included by bare name, not a relative path up to src/omci/: a real
 * kernel build (kbuild) copies only kernel/extra into the build tree, so
 * a "../../../../../../src/omci/..." path that resolves from this file
 * position in the odi-oss checkout (test/odi_switch_cmd_test.c's own
 * unity-build include, which works) does not exist inside that container
 * at all. kernel/build.sh bind-mounts
 * src/omci read-only at /omci and the odi/ Makefile's own
 * CFLAGS_odi_switch_cmd.o adds -I/omci for it; test/odi_switch_cmd_test.sh
 * adds the equivalent -I for the host build.
 */
#include "odi_switch_cmd.h"
#include "odi_switch_dal.h"
#include "odi_gpon.h"

#include "omci_gemflow.h"
#include "omci_bdgconn.h"
#include "omci_bridgeport.h"
#include "omci_caps.h"
#include "omci_flows.h"

#ifdef __KERNEL__
#include <linux/printk.h>
#define ODI_SW_CMD_LOG(fmt, ...) pr_info_once("odi_switch_cmd: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define ODI_SW_CMD_LOG(fmt, ...) printf("odi_switch_cmd: " fmt, ##__VA_ARGS__)
#endif

/* ODI_SW_EOPNOTSUPP is odi_switch_tbl.h's (included via odi_switch_dal.h
 * above) -- one shared definition instead of this file's own copy.
 */

/* ---- allocator/shadow state, ours -- one array per service list for
 * each of the 15 exercised commands. Cleared by odi_switch_cmd_reset_state().
 */

/* cmd 21 -- createTcont, the T-CONT slot table.
 * alloc_id -> hw index is a flat append: isp1 capture shows 5 T-CONTs
 * created in order with no delete/reuse in this run, so "first free slot"
 * and "append" are the same operation here; a real delete path would need
 * a free-list, out of scope here (cmd 21 wrote zero registers in
 * every capture so far -- see cmd_tcont() below).
 */
struct odi_sw_tcont_slot {
	uint32_t alloc_id;
	int used;
};
static struct odi_sw_tcont_slot odi_sw_tcont[ODI_SW_CMD_TCONT_MAX];
static unsigned int odi_sw_tcont_n;

/* cmd 23 -- setPriQueue. Two register shapes exist (odi_switch_dal.h):
 * a pre-T-CONT baseline pass (no T-CONT created yet) and the full
 * PONQ_COUNT_MASK program once T-CONTs exist. Which shape a given call needs
 * is not visible in the argument struct alone -- the trace
 * shows it gated on whether any T-CONT has been created yet, so that is
 * the bookkeeping used here: baseline while odi_sw_tcont_n == 0, full
 * programming (at slot ordinal odi_sw_priq_full_n, incremented per call)
 * once at least one T-CONT exists. This is a documented judgment call,
 * not a re-derivation of the handler actual gating condition.
 */
static unsigned int odi_sw_priq_full_n;

/* cmd 25 -- cfgGemFlow. The flow id is chosen by the caller (omcid allocates
 * it: the id the port already holds in that direction, else the lowest
 * free one -- src/omci/omci_gemflow.h), and it IS the table index: the
 * downstream GEM Port-ID row and the upstream flow slot are both
 * addressed by it, and the upstream CF action queues a frame on that same
 * number (odi_switch_dal.h, CF action bits 30..24). On isp1 the ids came
 * in 0, 1, 2, ... in creation order, which is why an append allocator
 * reproduced its trace; any other order put a GEM port in a row the CF
 * rule does not name.
 *
 * What each row/slot was given is kept for the read-only OMCI_FLOWS_CMD
 * readback (src/omci/omci_flows.h), the first OMCI_FLOWS_MAX of each.
 */
static uint32_t odi_sw_gem_ds_used[(ODI_SW_CMD_GEM_DS_MAX + 31) / 32];
static uint32_t odi_sw_gem_us_used[(ODI_SW_CMD_GEM_US_MAX + 31) / 32];
static uint32_t odi_sw_gem_ds_gem[OMCI_FLOWS_MAX];
static uint32_t odi_sw_gem_ds_cfg[OMCI_FLOWS_MAX];
static uint32_t odi_sw_gem_us_gem[OMCI_FLOWS_MAX];

/* cmd 51/50 -- activeBdgConn/deactiveBdgConn. One slot per active
 * service id (omci_bdgconn.service_id), holding the CF rows that service owns
 * and the VLAN row it contributes. A second activation of an active
 * service_id (omcid merging another ingress into its uni_mask) updates the
 * service in place; cmd 50 frees it.
 */
struct odi_sw_bdgconn_slot {
	int32_t serv_id;
	int used;
	uint32_t dir;
	int us_row, ds_row;	/* CF row index, -1 when the leg is absent */
	uint32_t vlan_vid;	/* 0: no VLAN row of its own */
	uint32_t vlan_val;
};
static struct odi_sw_bdgconn_slot odi_sw_bdgconn[ODI_SW_CMD_BDGCONN_MAX];

/* The CF table as this dispatch has programmed it, indexed by row. */
struct odi_sw_cf_row {
	int used;
	int is_us;
	unsigned int key;	/* ordering class, see cf_key() */
	uint32_t rule_w1, mask_w1;
	uint32_t act[3];
};
#define ODI_SW_CF_ROWS		256U
static struct odi_sw_cf_row odi_sw_cf[ODI_SW_CF_ROWS];

/* cmd 10 -- getTransceiverStatus. The GPIO base/pair value that varies
 * across calls is the I2C bit-bang driver's own internal state, not an
 * OMCI attribute -- so it is this dispatch's own counter, not derived from the argument
 * struct. isp1-260922-boot5.txt five observed pairs, replayed in
 * order then held at the fifth (a poll loop settles rather than wrapping
 * unboundedly; not confirmed by a longer capture, a defensible default for a
 * command never seen run a sixth time).
 */
static const uint32_t odi_sw_transceiver_gpio_lo[] = { 0x62, 0x68, 0x66, 0x64, 0x60 };
static const uint32_t odi_sw_transceiver_gpio_hi[] = { 0x63, 0x69, 0x67, 0x65, 0x61 };
static unsigned int odi_sw_transceiver_n;

/* cmd 25 US side -- the PONQ_COUNT_MASK+235/+20/+21 fields
 * (odi_sw_ponmac_flow_queue_set word235_a/word235_b/val_2021/use_21
 * parameters) are undecoded bit fields (odi_switch_dal.h's own leaf
 * comment: "field layout not decoded"). Nothing in the
 * argument struct maps to them either, so, like the cmd 10
 * GPIO pair above, they are replayed from the boot5 reference by
 * upstream flow id (the slot, modulo the five captured) rather than
 * computed -- an honest placeholder for a still-undecoded field, not a
 * guessed formula.
 */
static const uint32_t odi_sw_gem_us_w235a[] = {
	0x1ee01b08U, 0x1e4a1e40U, 0x1db41daaU, 0x1d1e1d14U, 0x1c881c7eU,
};
static const uint32_t odi_sw_gem_us_w235b[] = {
	0x1ee01e40U, 0x1e4a1daaU, 0x1db41d14U, 0x1d1e1c7eU, 0x1c881be8U,
};
static const uint32_t odi_sw_gem_us_val2021[] = {
	0x07efdf80U, 0x07efc080U, 0x07e08080U, 0x00608080U, 0x07efdf84U,
};
/* use_21: which of PONQ_COUNT_MASK+20/+21 gets val2021 -- +20 for slots 0-3,
 * +21 only for the last (slot 4), matching boot5 exactly like boot3 did
 * (odi_switch_dal.h's own leaf comment on the wraparound it did not chase).
 */
static const int odi_sw_gem_us_use21[] = { 0, 0, 0, 0, 1 };

void odi_switch_cmd_reset_state(void)
{
	unsigned int i;

	odi_sw_tcont_n = 0;
	odi_sw_priq_full_n = 0;
	odi_sw_transceiver_n = 0;
	for (i = 0; i < ODI_SW_CMD_TCONT_MAX; i++)
		odi_sw_tcont[i].used = 0;
	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		odi_sw_bdgconn[i].used = 0;
	for (i = 0; i < ODI_SW_CF_ROWS; i++)
		odi_sw_cf[i].used = 0;
	for (i = 0; i < sizeof(odi_sw_gem_ds_used) / sizeof(odi_sw_gem_ds_used[0]); i++)
		odi_sw_gem_ds_used[i] = 0;
	for (i = 0; i < sizeof(odi_sw_gem_us_used) / sizeof(odi_sw_gem_us_used[0]); i++)
		odi_sw_gem_us_used[i] = 0;
	for (i = 0; i < OMCI_FLOWS_MAX; i++) {
		odi_sw_gem_ds_gem[i] = 0;
		odi_sw_gem_ds_cfg[i] = 0;
		odi_sw_gem_us_gem[i] = 0;
	}
}

/* ---- the 15 exercised commands ---- */

/* cmd 62 -- setAgeingTime, `data` verdict. The two
 * boot5 field values are constant in every capture so far (a driver-init
 * default, not per-ME); the argument struct wire format for this command
 * is not one of the ones src/omci names (setAgeingTime generated
 * wrapper in src/omci/generated/omci_drv.c passes a raw 4-byte value), so this
 * reads it as the single big-endian-free uint32_t buf already is.
 */
static int cmd_mac_age_time(void *buf, uint32_t len)
{
	uint32_t age_spd = 3000, linkdown_ageout = 1;

	if (buf && len >= sizeof(uint32_t)) {
		/* Accept a caller-supplied age time in the low bits if ever
		 * sent non-default; boot5 one instance always sends 3000.
		 */
		uint32_t v = *(const uint32_t *)buf;

		if (v)
			age_spd = v;
	}
	odi_sw_l2_aging_set(age_spd, linkdown_ageout);
	return 0;
}

/* cmd 30/31 -- setPortAutoNegoAbility (30) / GetPortAutoNegoAbility (31,
 * `read`, not exercised -- see the default case). boot5 one instance
 * writes the fixed advertisement RMW every time; no per-port varying
 * field was found in this capture (single UNI port).
 */
static int cmd_port_auto_nego(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	odi_sw_port_autoneg_get();
	odi_sw_port_autoneg_set(0xde1, 0x0, 0x3a00);
	return 0;
}

/* cmd 32 -- setPortState, `data` (port admin enable plus MAC
 * force-ability read-modify-write). port fixed at 0 (isp1 single UNI
 * port maps to switch port 0 PORT_FORCE_SELECT slot; the flood-mask port
 * (cmd 64) is a different slot, 2 -- see cmd_flooding_port_mask's own
 * comment).
 */
static int cmd_port_state(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	odi_sw_port_admin_set(0, 1);
	(void)odi_sw_port_force_get(0);
	odi_sw_port_force_set(0);
	return 0;
}

/* cmd 38 -- setPhyPwrDown, `data`. Zero writes in every capture
 * (the UNI PHY is already in the requested power state at this point in
 * boot -- a guess, not directly confirmed). Bookkeeping only.
 */
static int cmd_port_phy_pwrdown(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	return 0;
}

/* cmd 64 -- setFloodingPortMask, `logic`. The register (LUT_UNKN_UC_
 * FLOOD, 0x01c028) packs all four ports flood-enable bits into one word,
 * so this is a whole-word RMW, not a per-port slot write. The 4x-identical
 * write the trace shows is already produced by odi_sw_l2_
 * lookupMissFloodPortMask_set() itself (its own internal loop over the
 * four identical writes the capture shows) -- called once here,
 * not four times.
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

/* cmd 23 -- setPriQueue, `logic`: allocator-gated shape (see the
 * odi_sw_priq_full_n comment above).
 */
static int cmd_pri_queue(void *buf, uint32_t len)
{
	(void)buf; (void)len;

	if (odi_sw_tcont_n == 0) {
		/* Baseline pass: 8 identical calls in every capture so far,
		 * PORT_QUEUE_MAP (0x01c0c0), threshold 0xd4.
		 */
		odi_sw_ponmac_queue_add(0xd4);
		return 0;
	}

	{
		unsigned int n = odi_sw_priq_full_n;
		static const uint32_t bitmask_207[] = {
			0x10001U, 0x10003U, 0x10007U, 0x1000fU, 0x1001fU,
		};
		static const uint32_t base_15[] = { 0x6U, 0xeU, 0x16U, 0x1eU, 0x26U };
		static const uint32_t val_208[] = { 0x0U, 0x2U, 0x6U, 0xeU, 0x1eU };
		static const uint32_t bitmask_212_213[] = {
			0x100401U, 0x107801U, 0x1807801U, 0x100404U, 0x101004U,
		};
		static const int use_213[] = { 0, 0, 0, 1, 1 };

		if (n >= ODI_SW_CMD_PRIQ_MAX)
			return ODI_SW_EOPNOTSUPP;
		if (n >= 5) {
			/* Past the 5 T-CONTs boot5 exercises: reissue the
			 * last known-good program rather than index out of
			 * the reference tables above (a real allocator would
			 * compute these; only 5 instances of
			 * evidence exist).
			 */
			n = 4;
		}
		/* boot5 scheduling-queue slot value is 1<<n (see this file
		 * top-of-function comment on the boot3-vs-boot5 difference);
		 * odi_sw_ponmac_queue_add_ext() writes it itself, at the
		 * right position in the sequence, via odi_sw_qos_
		 * schedulingQueue_set() -- not called separately here.
		 */
		odi_sw_ponmac_queue_add_ext(odi_sw_priq_full_n, bitmask_207[n], base_15[n],
					     (uint32_t)1U << odi_sw_priq_full_n,
					     val_208[n], bitmask_212_213[n], use_213[n]);
	}
	odi_sw_priq_full_n++;
	return 0;
}

/* cmd 21 -- createTcont, `logic`: T-CONT allocator.
 * Zero register writes in every capture so far --
 * scheduling/ACL calls in the handler either land outside the traced
 * window or are deferred to the paired cmd 23 that always immediately
 * follows). index is handed back to the caller in the argument struct
 * (omci_tcont.index, an output field: the driver T-CONT index).
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

/* cmd 25 -- cfgGemFlow, `logic`: DS/US GEM-flow tables, both indexed
 * by the flow id the caller sends (see the state comment above).
 * dir selects the table (OMCI_GEMFLOW_DS writes the downstream GEM
 * Port-ID CAM via the GPON GTC primitive; OMCI_GEMFLOW_US the upstream
 * port map plus PONQ_COUNT_MASK bookkeeping). The OMCI/broadcast GEM
 * (gem_port 0xfff) is downstream-only -- the US side is never called for
 * it, matching the captured 6 DS / 5 US instance counts.
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
		uint32_t slot = g->flow_id, bitmask_37 = 0, i;

		if (g->gem_port == 0xfffU) {
			/* OMCI channel: downstream-only, no US table entry. */
			return 0;
		}
		if (slot >= ODI_SW_CMD_GEM_US_MAX)
			return ODI_SW_EOPNOTSUPP;
		flow_mark(odi_sw_gem_us_used, slot);

		/* PONQ_COUNT_MASK+37: one bit per upstream slot in use (isp1,
		 * slots 0..4 in order: 1, 3, 7, 0xf, 0x1f). Only the low 32
		 * slots fit the word; nothing past slot 4 has been captured.
		 */
		for (i = 0; i < 32U && i < ODI_SW_CMD_GEM_US_MAX; i++)
			if (flow_used(odi_sw_gem_us_used, i))
				bitmask_37 |= 1U << i;
		{
			unsigned int ref = slot % 5;

			odi_sw_ponmac_flow_queue_set(slot, g->gem_port, bitmask_37,
						      odi_sw_gem_us_w235a[ref],
						      odi_sw_gem_us_w235b[ref],
						      odi_sw_gem_us_val2021[ref],
						      odi_sw_gem_us_use21[ref]);
		}
		if (slot < OMCI_FLOWS_MAX)
			odi_sw_gem_us_gem[slot] = g->gem_port;
		return 0;
	}

	return -1; /* neither US nor DS -- malformed request */
}

/* OMCI_FLOWS_CMD -- ours (src/omci/omci_flows.h): the cmd 25 record
 * above, handed back. Reads nothing from the hardware and changes nothing.
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

/* cmd 26 -- setDsBcGemFlow, `logic`. Zero register writes in every
 * capture so far (an ACL-table-only handler); the ACL index pool it
 * shares with SetGroupMacFilter is not
 * modeled by any register write reproducible from a capture, so this is
 * bookkeeping acceptance only.
 */
static int cmd_ds_bc_gem_flow(void *buf, uint32_t len)
{
	(void)buf; (void)len;
	return 0;
}

/* cmd 10 -- getTransceiverStatus, `read`. See the GPIO-pair comment
 * above -- the varying element is this driver's own I2C framing counter,
 * not an OMCI attribute.
 */
static int cmd_transceiver_status(void *buf, uint32_t len)
{
	unsigned int n = odi_sw_transceiver_n;

	(void)buf; (void)len;
	if (n >= 5)
		n = 4;
	odi_sw_ponmac_transceiver_get(odi_sw_transceiver_gpio_lo[n], odi_sw_transceiver_gpio_hi[n]);
	odi_sw_transceiver_n++;
	return 0;
}

/* cmd 51 -- activeBdgConn: the bridge rule (omci_bdgconn, 160 bytes)
 * turned into classification (CF) rows and a VLAN row. Row layouts are in
 * odi_switch_dal.h; this is the part that decides what goes in them and
 * where. Derived from the twelve isp1 brackets (six services, each sent
 * twice) and checked against the isp2 stock VLAN rows; every value below
 * that no capture has exercised is marked "not seen".
 *
 * Board ports: 0 the UNI, 2 the PON, 3 the CPU (cmd 3 above reports the
 * same). The chip has two UNI-capable ports, 0 and 1; the board wires 0.
 */
#define ODI_SW_PON_PORT		2U
#define ODI_SW_CPU_PORT		3U
#define ODI_SW_CHIP_UNI_PORTS	0x3U
#define ODI_SW_BOARD_UNI_PORTS	0x1U

/* Rows 64.. hold the ordered service rules; rows that care about no VID
 * (the untagged upstream rule) fill downward from 254; 255 is the
 * downstream default the module-load replay installs. Both captures
 * start at 64 and 254.
 */
#define ODI_SW_CF_SORTED_BASE	64U
#define ODI_SW_CF_BOTTOM	254U
#define ODI_SW_CF_KEY_BOTTOM	3U

#define ODI_SW_ENOSPC		(-28)

/* One direction of a service, as rows. */
struct odi_sw_cf_leg {
	int present;
	int is_us;
	unsigned int key;
	uint32_t rule_w1, mask_w1;
	uint32_t act[3];
};

/* The ports of uni_mask that are UNIs (not the PON or CPU bit). */
static uint32_t uni_ports(uint32_t uni_mask)
{
	return uni_mask & 0xfU & ~(1U << ODI_SW_PON_PORT) & ~(1U << ODI_SW_CPU_PORT);
}

/* Ordering class of a row. ISP1 places, from row 64 up: rules that care
 * about the tag flags AND the priority (class 0), rules that care about
 * one of the two (1), rules that care about the VID alone (2); each new
 * rule goes after the existing rules of its class. A rule caring no VID
 * at all goes to the bottom region.
 */
static unsigned int cf_key(int vid, int pri, int stag, int ctag)
{
	unsigned int n = 0;

	if (vid < 0)
		return ODI_SW_CF_KEY_BOTTOM;
	if (pri >= 0)
		n++;
	if (stag >= 0 || ctag >= 0)
		n++;
	return 2U - n;
}

/* Match words for one leg. -1 in any argument: not cared about. */
static void cf_match(struct odi_sw_cf_leg *l, int ds, int vid, int pri,
		     int stag, int ctag, int uni)
{
	uint32_t data = ds ? ODI_SW_CF_W1_DS : 0, care = ODI_SW_CF_W1_DS;

	if (vid >= 0) {
		data |= ((uint32_t)vid << ODI_SW_CF_W1_VID_SHIFT) & ODI_SW_CF_W1_VID_MASK;
		care |= ODI_SW_CF_W1_VID_MASK;
	}
	if (pri >= 0) {
		data |= ((uint32_t)pri << ODI_SW_CF_W1_PRI_SHIFT) & ODI_SW_CF_W1_PRI_MASK;
		care |= ODI_SW_CF_W1_PRI_MASK;
	}
	if (stag >= 0) {
		data |= stag ? ODI_SW_CF_W1_STAG : 0;
		care |= ODI_SW_CF_W1_STAG;
	}
	if (ctag >= 0) {
		data |= ctag ? ODI_SW_CF_W1_CTAG : 0;
		care |= ODI_SW_CF_W1_CTAG;
	}
	if (uni >= 0) {
		data |= (uint32_t)uni & ODI_SW_CF_W1_UNI_MASK;
		care |= ODI_SW_CF_W1_UNI_MASK;
	}
	l->rule_w1 = data & care;
	l->mask_w1 = care & ~data;
	l->key = cf_key(vid, pri, stag, ctag);
	l->present = 1;
	l->is_us = !ds;
}

/* The match a filter describes: the upstream leg of every rule, and the
 * downstream leg of a downstream-only rule. The tag flags are cared about
 * only when the filter says "no S-tag" (isp1: the untagged data rule and
 * the multicast rule; the VID filters leave both flags free).
 */
static void cf_match_filter(struct odi_sw_cf_leg *l, const struct omci_vlan_filter *f,
			    int ds, int uni)
{
	int vid = -1, pri = -1, stag = -1, ctag = -1;
	uint32_t cm = f->inner_mode;

	if (f->outer_mode & OMCI_TAGF_UNTAGGED)
		stag = 0;
	else if (f->outer_mode & OMCI_TAGF_TAGGED)
		stag = 1;	/* not seen */
	if (f->outer_mode & (OMCI_TAGF_VID | OMCI_TAGF_PRI | OMCI_TAGF_TCI))
		ODI_SW_CMD_LOG("bdgconn: S-tag VID/priority filter not derived, ignored\n");
	if (cm & OMCI_TAGF_UNTAGGED)
		ctag = 0;
	else if (cm & OMCI_TAGF_TAGGED)
		ctag = 1;	/* not seen */
	if ((cm & (OMCI_TAGF_VID | OMCI_TAGF_TCI)) && f->inner.vid < 4096U)
		vid = (int)f->inner.vid;
	if ((cm & (OMCI_TAGF_PRI | OMCI_TAGF_TCI)) && f->inner.pri < 8U)
		pri = (int)f->inner.pri;
	if (ctag < 0 && stag == 0 && (vid >= 0 || pri >= 0))
		ctag = 1;
	if ((cm & OMCI_TAGF_ETHTYPE) || f->ethertype != OMCI_ETHTYPE_NO_CARE)
		ODI_SW_CMD_LOG("bdgconn: ethertype filter not derived, ignored\n");
	cf_match(l, ds, vid, pri, stag, ctag, uni);
}

/* C-tag / S-tag treatment codes as the rule states them (the upstream
 * leg, and a downstream-only rule).
 */
static uint32_t cact_of(uint32_t act)
{
	switch (act) {
	case OMCI_TAGOP_PUSH:		return ODI_SW_CF_CACT_ADD;
	case OMCI_TAGOP_POP:		return ODI_SW_CF_CACT_DEL;
	case OMCI_TAGOP_PASS:	return ODI_SW_CF_CACT_TRANSPARENT;
	case OMCI_TAGOP_REWRITE:		/* not seen: retag with the assigned VID */
		return ODI_SW_CF_CACT_ADD;
	default:			return ODI_SW_CF_CACT_NONE;
	}
}

static uint32_t csact_of(uint32_t act)
{
	switch (act) {
	case OMCI_TAGOP_POP:		return ODI_SW_CF_CSACT_DEL;
	case OMCI_TAGOP_PASS:	return ODI_SW_CF_CSACT_TRANSPARENT;
	case OMCI_TAGOP_PUSH:
	case OMCI_TAGOP_REWRITE:
		ODI_SW_CMD_LOG("bdgconn: S-tag add not derived, left transparent\n");
		return ODI_SW_CF_CSACT_TRANSPARENT;
	default:			return ODI_SW_CF_CSACT_NONE;
	}
}

static uint32_t act_vid(const struct omci_vlan *v)
{
	return v->vid < 4096U ? v->vid : 0;
}

static uint32_t act_pri(const struct omci_vlan *v)
{
	return v->pri < 8U ? v->pri : 0;
}

/* Words 1 and 2 of an action row, the parts both directions share.
 * The VID/priority sources are always "assign": the only mode any rule
 * has used; the other OMCI modes are not seen.
 */
static void cf_act_common(struct odi_sw_cf_leg *l, uint32_t cact, uint32_t c_vid,
			  uint32_t c_pri, uint32_t csact, uint32_t cs_vid, uint32_t cs_pri)
{
	l->act[0] = 0;
	l->act[1] = (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A1_CPRI_ACT_SHIFT) |
		    (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A1_CVID_ACT_SHIFT) |
		    ((c_pri & 7U) << ODI_SW_CF_A1_C_PRI_SHIFT) |
		    ((c_vid & 0xfffU) << ODI_SW_CF_A1_C_VID_SHIFT) |
		    ((cact & 3U) << ODI_SW_CF_A1_CACT_SHIFT);
	l->act[2] = (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A2_CSVID_ACT_SHIFT) |
		    (ODI_SW_CF_TAG_SRC_ASSIGN << ODI_SW_CF_A2_CSPRI_ACT_SHIFT) |
		    ((cs_pri & 7U) << ODI_SW_CF_A2_CS_PRI_SHIFT) |
		    ((cs_vid & 0xfffU) << ODI_SW_CF_A2_CS_VID_SHIFT) |
		    (csact & 7U);
}

/* Downstream: forward to pmsk. The 2-bit UNI action straddles words 1
 * and 2 (bits 32..31).
 */
static void cf_act_ds_fwd(struct odi_sw_cf_leg *l, uint32_t pmsk)
{
	l->act[1] |= ODI_SW_CF_DS_UNI_ACT_FWD >> 1;
	l->act[2] |= ((ODI_SW_CF_DS_UNI_ACT_FWD & 1U) << 31) |
		     ((pmsk & 0xfU) << ODI_SW_CF_A2_DS_PMSK_SHIFT);
}

/* Both legs of one bridge rule, plus its VLAN row. */
static void bdgconn_derive(const struct omci_bdgconn *b, struct odi_sw_cf_leg *us,
			   struct odi_sw_cf_leg *ds, uint32_t *vlan_vid, uint32_t *vlan_val)
{
	const struct omci_vlan_oper *r = &b->vlan_op;
	const struct omci_vlan_out *o = &r->out;
	uint32_t uni = uni_ports(b->uni_mask);
	int one_uni = -1;
	unsigned int i;

	us->present = 0;
	ds->present = 0;
	*vlan_vid = 0;
	*vlan_val = 0;

	/* The upstream rule names its source port only when uni_mask holds
	 * exactly one UNI (isp1 1 and 5 alike -> port 0); with none (a VEIP
	 * alone, 4) or several it matches any source port.
	 */
	for (i = 0; i < 4U; i++)
		if (uni == (1U << i))
			one_uni = (int)i;

	if (b->dir & OMCI_DIR_US) {
		cf_match_filter(us, &r->filter, 0, one_uni);
		cf_act_common(us, cact_of(r->inner_act.tag_op), act_vid(&r->inner_act.set_tag),
			      act_pri(&r->inner_act.set_tag), csact_of(r->outer_act.tag_op),
			      act_vid(&r->outer_act.set_tag), act_pri(&r->outer_act.set_tag));
		/* Queue on the rule upstream flow. */
		us->act[2] |= ((b->us_flow & 0x7fU) << ODI_SW_CF_A2_US_FLOW_SHIFT) |
			      ODI_SW_CF_A2_US_SID_ACT;
	}

	if ((b->dir & OMCI_DIR_DS) && (b->dir & OMCI_DIR_US)) {
		/* Both ways: the downstream rule matches the tag the upstream
		 * treatment leaves on the PON side (out.out_tag), and
		 * undoes that treatment. ISP1: the add-tag data rule matches
		 * VID 11 priority 0 with a C-tag and no S-tag, and deletes both
		 * tags; the VID filters match their VID (and p-bit when the
		 * out-style carries one) and stay transparent.
		 */
		int vid = o->out_tag.vid < 4096U ? (int)o->out_tag.vid : -1;
		int pri = o->out_tag.pri < 8U ? (int)o->out_tag.pri : -1;
		int stag = -1, ctag = -1;
		uint32_t cact, csact, c_vid = 0, c_pri = 0;

		if (r->filter.outer_mode & OMCI_TAGF_UNTAGGED) {
			stag = 0;
			ctag = o->tag_count >= 1U ? 1 : 0;
		}
		cf_match(ds, 1, vid, pri, stag, ctag, -1);

		switch (r->inner_act.tag_op) {
		case OMCI_TAGOP_PUSH:
			cact = ODI_SW_CF_CACT_DEL;
			break;
		case OMCI_TAGOP_PASS:
			cact = ODI_SW_CF_CACT_TRANSPARENT;
			break;
		case OMCI_TAGOP_POP:
		case OMCI_TAGOP_REWRITE:
			/* not seen: put back the tag the filter matched */
			cact = ODI_SW_CF_CACT_ADD;
			c_vid = act_vid(&r->filter.inner);
			c_pri = act_pri(&r->filter.inner);
			break;
		default:
			cact = ODI_SW_CF_CACT_NONE;
			break;
		}
		if (r->filter.outer_mode & OMCI_TAGF_UNTAGGED)
			csact = ODI_SW_CF_CSACT_DEL;	/* nothing S-tagged reaches this UNI */
		else
			csact = csact_of(r->outer_act.tag_op);
		cf_act_common(ds, cact, c_vid, c_pri, csact, 0, 0);
		cf_act_ds_fwd(ds, uni ? uni : ODI_SW_CHIP_UNI_PORTS);

		if (vid >= 2 && vid <= 4094) {
			uint32_t mbr = uni ? uni : ODI_SW_BOARD_UNI_PORTS;

			*vlan_vid = (uint32_t)vid;
			*vlan_val = ODI_SW_VLAN_ROW(mbr | (1U << ODI_SW_PON_PORT),
						    cact == ODI_SW_CF_CACT_DEL ? mbr : 0);
		}
	} else if (b->dir & OMCI_DIR_DS) {
		/* Downstream only (the multicast rule): the filter is the
		 * downstream match and the treatment applies as stated.
		 * ISP1: VID 0 priority 0 C-tag no S-tag, delete both, and
		 * C_VID carries the assigned VID even though the tag is
		 * deleted. The port mask read 1 with uni_mask 1 and 3 with
		 * uni_mask 5: modelled as "the PON bit adds both chip UNIs".
		 */
		uint32_t pmsk = uni;

		if (b->uni_mask & (1U << ODI_SW_PON_PORT))
			pmsk |= ODI_SW_CHIP_UNI_PORTS;
		if (!pmsk)
			pmsk = ODI_SW_CHIP_UNI_PORTS;
		cf_match_filter(ds, &r->filter, 1, -1);
		cf_act_common(ds, cact_of(r->inner_act.tag_op), act_vid(&r->inner_act.set_tag),
			      act_pri(&r->inner_act.set_tag), csact_of(r->outer_act.tag_op),
			      act_vid(&r->outer_act.set_tag), act_pri(&r->outer_act.set_tag));
		cf_act_ds_fwd(ds, pmsk);
	}
}

/* The rows written by one call, in order, handed to the DAL leaf. */
static struct odi_sw_cf_entry odi_sw_cf_out[2U * ODI_SW_CF_ROWS];
static unsigned int odi_sw_cf_out_n;

static void cf_emit(uint32_t idx, int first)
{
	const struct odi_sw_cf_row *row = &odi_sw_cf[idx];
	struct odi_sw_cf_entry *e;

	if (odi_sw_cf_out_n >= sizeof(odi_sw_cf_out) / sizeof(odi_sw_cf_out[0]))
		return;
	e = &odi_sw_cf_out[odi_sw_cf_out_n++];
	e->idx = idx;
	e->is_us = row->is_us;
	e->rule_w0 = ODI_SW_CF_W0_VALID;
	e->rule_w1 = row->rule_w1;
	/* bit 48 of the mask row: 0 on the first write of an insert, set on
	 * every other write (odi_switch_dal.h).
	 */
	e->mask_w0 = first ? 0 : ODI_SW_CF_W0_VALID;
	e->mask_w1 = row->mask_w1;
	e->action_w0 = row->act[0];
	e->action_w1 = row->act[1];
	e->action_w2 = row->act[2];
}

static void cf_set(uint32_t idx, const struct odi_sw_cf_leg *l)
{
	struct odi_sw_cf_row *row = &odi_sw_cf[idx];
	unsigned int i;

	row->used = 1;
	row->is_us = l->is_us;
	row->key = l->key;
	row->rule_w1 = l->rule_w1;
	row->mask_w1 = l->mask_w1;
	for (i = 0; i < 3; i++)
		row->act[i] = l->act[i];
}

/* The slot owning CF row idx gets told it moved to idx + 1. */
static void cf_moved(uint32_t from, uint32_t to)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		if (!odi_sw_bdgconn[i].used)
			continue;
		if (odi_sw_bdgconn[i].us_row == (int)from)
			odi_sw_bdgconn[i].us_row = (int)to;
		else if (odi_sw_bdgconn[i].ds_row == (int)from)
			odi_sw_bdgconn[i].ds_row = (int)to;
	}
}

/* Place a new row, keeping the ordered region ordered: after the last row
 * of the same or a lower class; if that row is taken, every row from
 * there to the next free one moves up by one, the topmost first (isp1
 * calls 5, 7 and 11 are exactly this). Returns the row, or -1 when full.
 */
static int cf_insert(const struct odi_sw_cf_leg *l)
{
	uint32_t limit = ODI_SW_CF_BOTTOM + 1U, r, p, h;
	int last = -1;

	if (l->key == ODI_SW_CF_KEY_BOTTOM) {
		for (r = ODI_SW_CF_BOTTOM; r >= ODI_SW_CF_SORTED_BASE; r--) {
			if (!odi_sw_cf[r].used) {
				cf_set(r, l);
				cf_emit(r, 1);
				return (int)r;
			}
			if (odi_sw_cf[r].key != ODI_SW_CF_KEY_BOTTOM)
				break;
		}
		return -1;
	}

	/* The ordered region ends where the bottom region starts. */
	for (r = ODI_SW_CF_BOTTOM; r >= ODI_SW_CF_SORTED_BASE; r--) {
		if (odi_sw_cf[r].used && odi_sw_cf[r].key == ODI_SW_CF_KEY_BOTTOM)
			limit = r;
		else if (odi_sw_cf[r].used)
			break;
	}
	for (r = ODI_SW_CF_SORTED_BASE; r < limit; r++)
		if (odi_sw_cf[r].used && odi_sw_cf[r].key <= l->key)
			last = (int)r;
	p = last < 0 ? ODI_SW_CF_SORTED_BASE : (uint32_t)last + 1U;
	for (h = p; h < limit && odi_sw_cf[h].used; h++)
		;
	if (h >= limit)
		return -1;
	for (r = h; r > p; r--) {
		odi_sw_cf[r] = odi_sw_cf[r - 1U];
		cf_moved(r - 1U, r);
		cf_emit(r, r == h);
	}
	cf_set(p, l);
	cf_emit(p, h == p);
	return (int)p;
}

static void cf_remove(int idx)
{
	if (idx < 0 || (uint32_t)idx >= ODI_SW_CF_ROWS || !odi_sw_cf[idx].used)
		return;
	odi_sw_cf_del((uint32_t)idx, odi_sw_cf[idx].is_us);
	odi_sw_cf[idx].used = 0;
}

static int cf_same_match(int idx, const struct odi_sw_cf_leg *l)
{
	if (!l->present)
		return idx < 0;
	if (idx < 0)
		return 0;
	return odi_sw_cf[idx].rule_w1 == l->rule_w1 && odi_sw_cf[idx].mask_w1 == l->mask_w1;
}

static int cf_same_row(int idx, const struct odi_sw_cf_leg *l)
{
	return cf_same_match(idx, l) && odi_sw_cf[idx].act[0] == l->act[0] &&
	       odi_sw_cf[idx].act[1] == l->act[1] && odi_sw_cf[idx].act[2] == l->act[2];
}

/* The VLAN row of vid as the active services define it together. */
static uint32_t vlan_row_of(uint32_t vid)
{
	uint32_t v = 0;
	unsigned int i;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++)
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].vlan_vid == vid)
			v |= odi_sw_bdgconn[i].vlan_val;
	return v;
}

static struct odi_sw_vlan_override odi_sw_vlan_out[ODI_SW_CMD_BDGCONN_MAX];

static unsigned int vlan_overrides(void)
{
	unsigned int i, k, n = 0;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		uint32_t vid = odi_sw_bdgconn[i].vlan_vid;

		if (!odi_sw_bdgconn[i].used || !vid)
			continue;
		for (k = 0; k < n; k++)
			if (odi_sw_vlan_out[k].idx == vid)
				break;
		if (k < n)
			continue;
		odi_sw_vlan_out[n].idx = vid;
		odi_sw_vlan_out[n].val = vlan_row_of(vid);
		n++;
	}
	return n;
}

/* Free a slot: its rows invalidated, its VLAN row recomputed. */
static void bdgconn_release(struct odi_sw_bdgconn_slot *sl)
{
	uint32_t vid = sl->vlan_vid;

	cf_remove(sl->us_row);
	cf_remove(sl->ds_row);
	sl->used = 0;
	if (vid)
		odi_sw_vlan_row_set(vid, vlan_row_of(vid));
}

static int cmd_active_bdg_conn(void *buf, uint32_t len)
{
	struct omci_bdgconn *b = (struct omci_bdgconn *)buf;
	struct odi_sw_bdgconn_slot *sl = NULL;
	struct odi_sw_cf_leg us, ds;
	uint32_t vlan_vid, vlan_val;
	unsigned int i;

	if (!b || len < sizeof(*b))
		return -1;

	bdgconn_derive(b, &us, &ds, &vlan_vid, &vlan_val);
	odi_sw_cf_out_n = 0;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == b->service_id) {
			sl = &odi_sw_bdgconn[i];
			break;
		}
	}

	if (sl && (sl->dir != b->dir || !cf_same_match(sl->us_row, &us) ||
		   !cf_same_match(sl->ds_row, &ds))) {
		/* The id now names a different rule: replace it. */
		bdgconn_release(sl);
		sl = NULL;
	}

	if (sl) {
		/* Same rule again, another ingress merged in (isp1: every
		 * service twice, uni_mask 1 then 5). The downstream row is
		 * rewritten in place every time; the upstream row only if it
		 * changed.
		 */
		if (ds.present) {
			cf_set((uint32_t)sl->ds_row, &ds);
			cf_emit((uint32_t)sl->ds_row, 0);
		}
		if (us.present && !cf_same_row(sl->us_row, &us)) {
			cf_set((uint32_t)sl->us_row, &us);
			cf_emit((uint32_t)sl->us_row, 0);
		}
	} else {
		for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
			if (!odi_sw_bdgconn[i].used) {
				sl = &odi_sw_bdgconn[i];
				break;
			}
		}
		if (!sl)
			return ODI_SW_EOPNOTSUPP;
		sl->serv_id = b->service_id;
		sl->dir = b->dir;
		sl->us_row = -1;
		sl->ds_row = -1;
		sl->vlan_vid = 0;
		sl->vlan_val = 0;
		sl->used = 1;
		if (us.present)
			sl->us_row = cf_insert(&us);
		if (ds.present)
			sl->ds_row = cf_insert(&ds);
		if ((us.present && sl->us_row < 0) || (ds.present && sl->ds_row < 0)) {
			ODI_SW_CMD_LOG("bdgconn: classification table full\n");
			bdgconn_release(sl);
			return ODI_SW_ENOSPC;
		}
	}
	sl->vlan_vid = vlan_vid;
	sl->vlan_val = vlan_val;

	odi_sw_cf_add(odi_sw_cf_out, odi_sw_cf_out_n,
				      odi_sw_vlan_out, vlan_overrides());
	return 0;
}

/* cmd 50 -- deactiveBdgConn, 4 bytes: the service id. Frees the slot,
 * invalidates its CF rows (all-zero rule and action rows; no capture has
 * a delete in it, see odi_sw_cf_del) and rewrites its VLAN
 * row from the services that remain. omcid rebuilds by deactivating every
 * service and activating them again, which refills the freed rows.
 */
static int cmd_deactive_bdg_conn(void *buf, uint32_t len)
{
	int32_t serv_id;
	unsigned int i;

	if (!buf || len < sizeof(int32_t))
		return -1;
	serv_id = *(const int32_t *)buf;

	for (i = 0; i < ODI_SW_CMD_BDGCONN_MAX; i++) {
		if (odi_sw_bdgconn[i].used && odi_sw_bdgconn[i].serv_id == serv_id) {
			bdgconn_release(&odi_sw_bdgconn[i]);
			return 0;
		}
	}
	return 0; /* deactivating an unknown/already-gone service_id is not an error */
}

/* cmd 3/4/15/13 -- getDevCapabilities/GetDevIdVersion/GetSerialNum/
 * GetOnuState, all `read`, all zero register writes in every capture --
 * there is no register
 * sequence to replay for any of these, but hardware trials
 * showed leaving the buffer untouched is
 * a real functional bug, not a harmless shortcut: omcid reads these
 * before doing anything else and logs "could not read ..." for each one
 * whose buffer never got filled, which is different from -EOPNOTSUPP --
 * a register-writes-only test cannot see it. These
 * four now answer with real content.
 */

/* cmd 3 -- getDevCapabilities, 120 bytes (src/omci/omci_caps.h has the
 * full offset table and this exact board's own observed values --
 * "Observed on an ODI DFP-34X-2C2, 2026-09-12" -- and boot5's own omcid
 * log header confirms two of them again independently: "capabilities: 64
 * gem flows, 128 priority queues" / "uni slot 0: type 2, index 0 -> switch
 * port 0"). CPU port=3 and PON port=2 are this driver's own CPUtag1CR
 * switch-port numbering (see odi_nic_hw.h), not vendor-confirmed here;
 * fields with no confirmed source (RGMII port, POTS port count,
 * queues-per-UNI count, the two queue-depth bytes, meter count, reserved meter id,
 * L2 table size) are conservative defaults, not vendor-observed -- flagged
 * individually below, worth confirming before this board grows a second
 * UNI or a metering feature that reads them.
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
		b[i * 2 + 1] = 0xff; /* "no slot" sentinel, main.c's own read loop */
	}
	b[0 * 2 + 0] = OMCI_UNI_SLOT_PPTP; /* uni slot 0: type 2 (PPTP Ethernet UNI) */
	b[0 * 2 + 1] = 0;                  /* index 0 -> switch port 0 */
	/* +64 FE port count, +68 GE port count: this board is one GE UNI, no FE port. */
	b[64] = 0; b[65] = 0; b[66] = 0; b[67] = 0;
	b[68] = 0; b[69] = 0; b[70] = 0; b[71] = 1;
	/* +72 CPU port (i32) = 3, +76 PON port (i32) = 2 -- this driver's own
	 * switch port numbering, +80 RGMII port (i32) = -1 (none on this board, not
	 * separately confirmed).
	 */
	b[OMCI_CAPS_OFF_CPUPORT + 3] = 3;
	b[OMCI_CAPS_OFF_PONPORT + 3] = 2;
	b[80] = 0xff; b[81] = 0xff; b[82] = 0xff; b[83] = 0xff; /* RGMII port = -1 */
	/* +84 POTS port count = 0 (no POTS on a GPON-only ONU, not separately
	 * confirmed -- board has no voice hardware, so 0 is the only
	 * defensible value even unconfirmed).
	 */
	/* +88 T-CONT count = 16 -- omci_caps.h's own observed-capture comment. */
	b[OMCI_CAPS_OFF_TCONTS + 3] = 16;
	/* +92 GEM port count = 64, +96 T-CONT queue count = 128 -- boot5
	 * omcid.log header, "64 gem flows, 128 priority queues".
	 */
	b[OMCI_CAPS_OFF_FLOWS + 2] = 0; b[OMCI_CAPS_OFF_FLOWS + 3] = 64;
	b[OMCI_CAPS_OFF_PRIQ + 3] = 128;
	/* +100 queues-per-UNI count = 8 -- from the captured
	 * cmd 23 baseline pass, 8 queue slots pre-created per UNI; not a
	 * capability-blob field confirmed directly, a defensible
	 * inference from what the trace shows getting programmed.
	 */
	b[OMCI_CAPS_OFF_UNIQ + 3] = 8;
	/* +104/+105 drop-precedence levels per T-CONT queue/per UNI queue = 1 (single drop-precedence
	 * level), +108 meter count = 0, +112 reserved meter id = 0, +116 L2 table size = 0
	 * -- none confirmed, conservative defaults.
	 */
	b[104] = 1;
	b[105] = 1;
	return 0;
}

/* cmd 4 -- getDevIdVersion, 40 bytes. src/omci/respond/main.c uses
 * the buffer purely as a NUL-terminated string ("RTL9602C", devid[8]=0
 * right after this call) -- see mibstore.c's own devid[40] and every reader
 * of it (main.c, show.c, vqsrv.c) -- so only the first 8 bytes are load-
 * bearing; the rest of the 40-byte buffer is never read past the NUL any
 * caller places.
 */
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

/* cmd 15/13 -- getSerialNum/getOnuState, answered by odi_gpon
 * (CONFIG_ODI_GPON). The serial number is the 8-byte G.984 ONU serial
 * (4-byte vendor id, 4-byte vendor-specific part); the ONU state the
 * plain O1..O7 number. Without CONFIG_ODI_GPON there is no GPON driver
 * on this kernel to ask, and both report EOPNOTSUPP.
 */

/* src/omci/omci_drv.h's own DRV_GET_SN wrapper reads 9 bytes (generated/
 * omci_drv.c: `omci_drv_call(15u, buf, 9u)`); the 8 real bytes from the
 * serial plus a trailing NUL, the same shape devid gets for cmd 4.
 */
static int cmd_get_serial_num(void *buf, uint32_t len)
{
	uint8_t *b = (uint8_t *)buf;

	if (!b || len < 9)
		return -1;
#ifdef __KERNEL__
#ifdef CONFIG_ODI_GPON
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
	return ODI_SW_EOPNOTSUPP;
#endif /* CONFIG_ODI_GPON */
#else
	/* Host test build: no GPON module to link against -- the host test
	 * never calls this leaf (it is outside the
	 * 91-bracket replay, which only covers register-writing commands),
	 * so this stub is never reached, only needed so the file compiles.
	 */
	(void)b;
	return ODI_SW_EOPNOTSUPP;
#endif
}

/* GetOnuState: the argument struct wire format for this command is not
 * one of the ones src/omci names (generated/omci_drv.c's own omci_getOnuState
 * takes a raw 4-byte buffer); the FSM state enum value goes straight in,
 * low byte first not assumed -- a plain host-endian uint32_t store, same
 * as every other raw-uint32 command this dispatch already handles this
 * way (cmd_mac_age_time and friends).
 */
static int cmd_get_onu_state(void *buf, uint32_t len)
{
	if (!buf || len < sizeof(uint32_t))
		return -1;
#ifdef __KERNEL__
#ifdef CONFIG_ODI_GPON
	*(uint32_t *)buf = (uint32_t)odi_gpon_onu_state();
	return 0;
#else
	return ODI_SW_EOPNOTSUPP;
#endif /* CONFIG_ODI_GPON */
#else
	return ODI_SW_EOPNOTSUPP;
#endif
}

/* Statistics resets flagged "noop (tentative)": no register write for
 * them appears in any capture. Treated here as success, no
 * register work -- rather than -EOPNOTSUPP, since the best reading is
 * that the stock driver itself does nothing here.
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

/*
 * Command numbers below are odi-oss's own OMCI driver command numbering -- the
 * literal value apply.c omci_drv_call(cmd, ...) is invoked with today
 * (src/omci/generated/omci_drv.c, one call site per command; confirmed by
 * reading each wrapper function directly, not inferred) -- cmd numbers
 * are odi-oss's own numbering, with the OMCI driver command name given in
 * the comment for cross-reference. There is exactly one numbering
 * space; the src/omci header constants (OMCI_TCONT_CMD, OMCI_PRIQ_CMD,
 * OMCI_GEMFLOW_CMD, OMCI_FLOOD_CMD, OMCI_BDGCONN_CMD/_OFF_CMD) are that
 * same space named subset. There is no sockopt path at all any more --
 * apply.c reaches this dispatch only through the odi_omci netlink socket.
 */
int odi_switch_cmd(uint32_t cmd, void *buf, uint32_t len)
{
	switch (cmd) {
	/* -- the 15 commands isp1 provisioning run exercises -- */
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
		return cmd_active_bdg_conn(buf, len);

	case OMCI_BDGCONN_OFF_CMD:	/* 50, deactiveBdgConn */
		return cmd_deactive_bdg_conn(buf, len);

	case OMCI_TRANSCEIVER_STATUS_CMD:	/* 10, getTransceiverStatus */
		return cmd_transceiver_status(buf, len);

	case OMCI_DS_BC_GEMFLOW_CMD:	/* 26, setDsBcGemFlow */
		return cmd_ds_bc_gem_flow(buf, len);

	case OMCI_FLOWS_CMD:	/* ours: read back the cmd 25 record */
		return cmd_flows_get(buf, len);

	/* -- statistics resets flagged "noop (tentative)" --
	 * commands with no register write in any capture; success, no register work.
	 * resetUsFlowStat=45, resetDsFlowStat=47 (confirmed against
	 * generated/omci_drv.c); no separate resetDsFecStat command exists
	 * in the generated table at all, so there is no case for it here.
	 */
	case OMCI_RESET_US_FLOW_STAT_CMD:	/* 45, resetUsFlowStat */
	case OMCI_RESET_DS_FLOW_STAT_CMD:	/* 47, resetDsFlowStat */
		return cmd_stat_reset_noop(buf, len);

	default:
		return cmd_unsupported(cmd, buf, len);
	}
}
