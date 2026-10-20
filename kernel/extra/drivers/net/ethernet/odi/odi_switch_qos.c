// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_qos.c -- the PON queue leaves: the downstream queue map of
 * cmd 23, and the upstream PON scheduler (queue add and delete, cmd 23 and
 * 24) and upstream flow (cmd 25) registers. odi_switch_dal.h has the
 * contract, docs/SWITCH.md#upstream-pon-queues the evidence.
 *
 * The upstream registers sit in the window odi_switch_hw.h calls
 * PONQ_COUNT_MASK, which is not one register: each index below is a
 * register of the chip's register map, named as the map names it. What
 * each does was read from the stock driver's queue add, queue delete and
 * flow add (the Realtek RTL9602C SDK, dal_rtl9602c_ponmac.c,
 * dal_rtl9602c_gpon.c and the GPON module's gpon_res.c, read as a
 * reference); applied to the arguments ISP1's provisioning sends, these
 * leaves write every word of the stock capture.
 *
 * Every field write is a read-modify-write of the register, as the stock
 * driver makes it: the registers start from the values the sdkinit and
 * module-load replays leave (weight 1 on every queue, the OMCC T-CONT 16
 * enabled with queue 64), and only the field of one queue or one T-CONT
 * changes.
 */
#include "odi_switch_dal.h"
#include "odi_switch_reg.h"

#define ODI_SW_PONQ_DRN_CMD		15U	/* DRN_CMD */
#define ODI_SW_PONQ_SID2QID		20U	/* PON_SID2QID: 7 bits per flow, 4 per word */
#define ODI_SW_PONQ_SIDVALID		37U	/* PON_SIDVALID: 1 bit per flow, 32 per word */
#define ODI_SW_PONQ_CIR_RATE		60U	/* PON_QID_CIR_RATE[queue] */
#define ODI_SW_PONQ_PIR_RATE		125U	/* PON_QID_PIR_RATE[queue] */
#define ODI_SW_PONQ_SCH_QMAP		190U	/* PON_SCH_QMAP[T-CONT]: its queue mask */
#define ODI_SW_PONQ_TCONT_EN		207U	/* PON_TCONT_EN: 1 bit per T-CONT */
#define ODI_SW_PONQ_WFQ_TYPE		208U	/* PON_WFQ_TYPE: 1 bit per queue, 1 = WFQ */
#define ODI_SW_PONQ_WFQ_WEIGHT		212U	/* PON_WFQ_WEIGHT: 10 bits per queue, 3 per word */
#define ODI_SW_PONQ_SID_GLB_TH		235U	/* PON_SID_GLB_TH: ON_TH 28:16, OFF_TH 12:0 */
#define ODI_SW_PONQ_SID_RPV_TH		236U	/* PON_SID_RPV_TH[flow]: same fields */

/* DRN_CMD: DRN_IDX 9:3 the queue, QUEUE_MODE 2 (drain one queue, not a
 * T-CONT), DRN_PS 1 (start), DRN_FLG 0 (set while the drain runs).
 */
#define ODI_SW_DRN_IDX_SHIFT		3U
#define ODI_SW_DRN_QUEUE_MODE		(1U << 2)
#define ODI_SW_DRN_PS			(1U << 1)
#define ODI_SW_DRN_FLG			(1U << 0)
/* The stock driver polls DRN_FLG up to 10000 times with no delay. */
#define ODI_SW_DRN_POLL_DELAY_US	1U
#define ODI_SW_DRN_POLL_US		10000U

#define ODI_SW_PONQ_WEIGHT_BITS		10U
#define ODI_SW_PONQ_WEIGHTS_PER_WORD	3U
#define ODI_SW_PONQ_TH_MASK		0x1fffU
#define ODI_SW_PONQ_TH_ON_SHIFT		16U
#define ODI_SW_PONQ_SID2QID_BITS	7U
#define ODI_SW_PONQ_SID2QID_PER_WORD	4U

/* The global PBO thresholds: ON = 7904 - (n - 1) * step and OFF = 7744 -
 * (n - 1) * step for n upstream data flows, where step is the per-flow ON
 * threshold of flow 0 (PON_SID_RPV_TH[0]); ON never goes below 2000, and
 * OFF is then 1840. The stock driver recomputes them on every upstream
 * data flow add and delete. Read from the stock kernel of our image
 * (dal_rtl9602c_gpon_flowctrl_adjust_byFlowNum, at 0x80131f50 of the
 * V1.0-220923 vmlinux: the four constants, the sid 0 step and the floor
 * test are its instructions), which has no cap at five flows; the 5.10 GPL
 * SDK's dal_rtl9602c_gpon.c has the same function.
 */
#define ODI_SW_PONQ_GLB_ON_BASE		7904U
#define ODI_SW_PONQ_GLB_OFF_BASE	7744U
#define ODI_SW_PONQ_GLB_ON_FLOOR	2000U
#define ODI_SW_PONQ_GLB_OFF_FLOOR	1840U

static uint32_t ponq_off(uint32_t idx)
{
	return ODI_SW_PONQ_COUNT_MASK(idx);
}

static void ponq_rmw(uint32_t idx, uint32_t mask, uint32_t val)
{
	uint32_t v = odi_reg_read(ponq_off(idx));

	odi_reg_write(ponq_off(idx), (v & ~mask) | (val & mask));
}

/* PORT_QUEUE_MAP holds a 2-bit group index per port, four ports. 0xd4 is
 * port 0 (the UNI) on group 0 with the other ports as the init left them,
 * which is what the stock driver's read-modify-write of port 0 gives for a
 * queue with no drop precedence marking; the only value captured.
 */
void odi_sw_ponmac_queue_add(uint32_t th)
{
	odi_reg_write(ODI_SW_PORT_QUEUE_MAP_BASE, th);
}

/* ---- The UNI egress scheduler: downstream queues (cmd 23, direction DS) ---
 *
 * Registers read on a stick and checked against another open
 * implementation's measurements of the same chip (docs/SWITCH.md#downstream-
 * queues); local to this file, like the UNI limit registers in
 * odi_switch_port.c, because they are not in the diag register table.
 *
 *   0x01c0b0 + 4p      priority to queue: eight 3-bit entries, priority i
 *                      at bits 3i+2..3i
 *   0x02d85c + 4p      queue type: bit q set = weighted, clear = strict
 *   0x02d824 + 4(p/2)  weight of queue 0: 16 bits, port p odd in 31..16
 *   0x02d82c + 12p + 4((q-1)/3)
 *                      weights of queues 1..7: 10 bits, three to a word
 *
 * The scheduler serves the strict queues first, the highest queue number
 * first, and the weighted queues after all of them, by weight. SCHED_CTRL
 * (0x02d800) is left alone: its bit 17 clear counts the weights in bytes,
 * which the reset state and the stock init both have. The shaper registers
 * of cmd 67 (0x02d804, 0x02d8b8) are not touched.
 */
#define ODI_SW_DSQ_PRIO_MAP_BASE	0x1c0b0U
#define ODI_SW_DSQ_WFQ_BASE		0x2d85cU
#define ODI_SW_DSQ_W0_BASE		0x2d824U
#define ODI_SW_DSQ_WN_BASE		0x2d82cU
#define ODI_SW_DSQ_WN_PORT_STRIDE	12U
#define ODI_SW_DSQ_W0_MAX		0xffffU
#define ODI_SW_DSQ_WN_MAX		0x3ffU
#define ODI_SW_DSQ_WN_BITS		10U
/* The weight of the queue in the register's units, 4 for each OMCI unit.
 * Two weighted queues share the bytes in the ratio of their weights, but
 * with a granularity: fields 1 and 3 shared 29/71 %, 4 and 12 25/75 %
 * (another open implementation's measurements), so a weight of 4 * w keeps
 * the OLT's ratios within a percent where w alone is off by several. */
#define ODI_SW_DSQ_WEIGHT_SCALE		4U
/* Priority i to queue i: what the stock init writes for port 0 and
 * the UNI's reset state does not (every priority to queue 0). */
#define ODI_SW_DSQ_PRIO_MAP_IDENTITY	0x00fac688U

bool odi_sw_ds_sched = true;

/* A word is written only when it changes: a queue the OLT leaves at the
 * default adds nothing to the boot's register stream. */
static void dsq_weight_set(uint32_t port, uint32_t q, uint32_t weight)
{
	uint32_t reg, shift, mask, v, nv;

	if (q == 0U) {
		reg = ODI_SW_DSQ_W0_BASE + 4U * (port / 2U);
		shift = 16U * (port % 2U);
		mask = ODI_SW_DSQ_W0_MAX;
		if (weight > mask)
			weight = mask;
	} else {
		reg = ODI_SW_DSQ_WN_BASE + ODI_SW_DSQ_WN_PORT_STRIDE * port +
		      4U * ((q - 1U) / 3U);
		shift = ODI_SW_DSQ_WN_BITS * ((q - 1U) % 3U);
		mask = ODI_SW_DSQ_WN_MAX;
		if (weight > mask)
			weight = mask;
	}
	v = odi_reg_read(reg);
	nv = (v & ~(mask << shift)) | (weight << shift);
	if (nv != v)
		odi_reg_write(reg, nv);
}

static void dsq_type_set(uint32_t port, uint32_t q, int wfq)
{
	uint32_t reg = ODI_SW_DSQ_WFQ_BASE + 4U * port;
	uint32_t v = odi_reg_read(reg), nv;

	nv = wfq ? (v | (1U << q)) : (v & ~(1U << q));
	if (nv != v)
		odi_reg_write(reg, nv);
}

/* Queue q of a port: weighted with weight (OMCI units, 1 or more) or
 * strict. A strict queue keeps the reset weight, 1, so that a later
 * change to weighted starts from a known field. The weight goes in before
 * the queue turns weighted and the type changes before the weight goes
 * back, so no instant has a weighted queue at a stale weight. The caller
 * holds odi_switch_lock. */
void odi_sw_ds_queue_sched(uint32_t port, uint32_t q, int wfq, uint32_t weight)
{
	uint32_t w = wfq ? weight * ODI_SW_DSQ_WEIGHT_SCALE : 1U;

	if (wfq) {
		dsq_weight_set(port, q, w);
		dsq_type_set(port, q, 1);
	} else {
		dsq_type_set(port, q, 0);
		dsq_weight_set(port, q, w);
	}
}

/* The port's priority to queue map as identity, written only when it is
 * not already: a stick that went through the stock init has it, and the
 * boot replay stays as it was. Without it a queue's number would not be
 * the priority of the frames that reach it. */
void odi_sw_ds_prio_map_identity(uint32_t port)
{
	uint32_t reg = ODI_SW_DSQ_PRIO_MAP_BASE + 4U * port;

	if (odi_reg_read(reg) != ODI_SW_DSQ_PRIO_MAP_IDENTITY)
		odi_reg_write(reg, ODI_SW_DSQ_PRIO_MAP_IDENTITY);
}

static void ponq_drain(uint32_t pq)
{
	uint32_t last = 0;

	odi_reg_write(ponq_off(ODI_SW_PONQ_DRN_CMD),
		      (pq << ODI_SW_DRN_IDX_SHIFT) | ODI_SW_DRN_QUEUE_MODE | ODI_SW_DRN_PS);
	/* The stock driver gives up on the queue when the drain does not
	 * finish. This goes on with a warning instead: the replay this
	 * replaced never waited, and a queue not drained is an empty queue
	 * on every path that reaches here (a queue is added before any flow
	 * maps to it, and deleted after its flows are).
	 */
	if (odi_poll_reg(ponq_off(ODI_SW_PONQ_DRN_CMD), ODI_SW_DRN_FLG, 0,
			 ODI_SW_DRN_POLL_DELAY_US, ODI_SW_DRN_POLL_US, &last))
		ODI_SW_WARN("PON queue %u: drain not done (DRN_CMD 0x%08x)\n",
			    (unsigned int)pq, (unsigned int)last);
}

static void ponq_wfq_type_set(uint32_t pq, int wfq)
{
	ponq_rmw(ODI_SW_PONQ_WFQ_TYPE + pq / 32U, 1U << (pq % 32U), wfq ? 1U << (pq % 32U) : 0U);
}

static void ponq_wfq_weight_set(uint32_t pq, uint32_t weight)
{
	uint32_t shift = (pq % ODI_SW_PONQ_WEIGHTS_PER_WORD) * ODI_SW_PONQ_WEIGHT_BITS;

	ponq_rmw(ODI_SW_PONQ_WFQ_WEIGHT + pq / ODI_SW_PONQ_WEIGHTS_PER_WORD,
		 ODI_SW_PONQ_WEIGHT_MAX << shift, weight << shift);
}

/* A CIR or PIR in the register's units: one less than asked, except for
 * 0, 1 and the maximum, as the stock driver writes it.
 */
static void ponq_rate_set(uint32_t idx, uint32_t rate)
{
	if (rate > ODI_SW_PONQ_RATE_MAX)
		rate = ODI_SW_PONQ_RATE_MAX;
	if (rate != 0U && rate != 1U && rate != ODI_SW_PONQ_RATE_MAX)
		rate--;
	ponq_rmw(idx, ODI_SW_PONQ_RATE_MAX, rate);
}

void odi_sw_ponq_queue_add(uint32_t t, uint32_t lq, uint32_t pq, int wfq,
			   uint32_t weight, uint32_t cir, uint32_t pir)
{
	uint32_t qmap = odi_reg_read(ponq_off(ODI_SW_PONQ_SCH_QMAP + t));

	if (qmap == 0U)
		ponq_rmw(ODI_SW_PONQ_TCONT_EN, 1U << t, 1U << t);
	if (!(qmap & (1U << lq))) {
		ponq_drain(pq);
		odi_reg_write(ponq_off(ODI_SW_PONQ_SCH_QMAP + t), qmap | (1U << lq));
	}
	/* Strict: the type, then weight 1. WFQ: the weight, then the type. */
	if (!wfq) {
		ponq_wfq_type_set(pq, 0);
		ponq_wfq_weight_set(pq, 1U);
	} else {
		ponq_wfq_weight_set(pq, weight);
		ponq_wfq_type_set(pq, 1);
	}
	ponq_rate_set(ODI_SW_PONQ_CIR_RATE + pq, cir);
	ponq_rate_set(ODI_SW_PONQ_PIR_RATE + pq, pir);
}

void odi_sw_ponq_queue_del(uint32_t t, uint32_t lq, uint32_t pq)
{
	uint32_t qmap;

	if (!(odi_reg_read(ponq_off(ODI_SW_PONQ_TCONT_EN)) & (1U << t)))
		return;
	qmap = odi_reg_read(ponq_off(ODI_SW_PONQ_SCH_QMAP + t));
	if (!(qmap & (1U << lq)))
		return;
	ponq_drain(pq);
	qmap &= ~(1U << lq);
	odi_reg_write(ponq_off(ODI_SW_PONQ_SCH_QMAP + t), qmap);
	if (qmap == 0U)
		ponq_rmw(ODI_SW_PONQ_TCONT_EN, 1U << t, 0U);
}

void odi_sw_ponq_sidvalid_set(uint32_t sid, int valid)
{
	uint32_t bit = 1U << (sid % 32U);

	ponq_rmw(ODI_SW_PONQ_SIDVALID + sid / 32U, bit, valid ? bit : 0U);
}

void odi_sw_ponq_us_gem_set(uint32_t slot, uint32_t gem_port_id)
{
	odi_reg_write(ODI_SW_US_GEM_PORT_MAP(slot), gem_port_id);
}

void odi_sw_ponq_sid2qid_set(uint32_t sid, uint32_t pq)
{
	uint32_t shift = (sid % ODI_SW_PONQ_SID2QID_PER_WORD) * ODI_SW_PONQ_SID2QID_BITS;

	ponq_rmw(ODI_SW_PONQ_SID2QID + sid / ODI_SW_PONQ_SID2QID_PER_WORD,
		 ODI_SW_PONQ_QUEUE_MASK << shift, pq << shift);
}

void odi_sw_ponq_glb_th_set(uint32_t nflows)
{
	uint32_t step = (odi_reg_read(ponq_off(ODI_SW_PONQ_SID_RPV_TH)) >> ODI_SW_PONQ_TH_ON_SHIFT) &
			ODI_SW_PONQ_TH_MASK;
	uint32_t on = ODI_SW_PONQ_GLB_ON_FLOOR, off = ODI_SW_PONQ_GLB_OFF_FLOOR;
	uint32_t drop;

	if (nflows == 0U)
		nflows = 1U;
	drop = (nflows - 1U) * step;
	if (drop <= ODI_SW_PONQ_GLB_ON_BASE - ODI_SW_PONQ_GLB_ON_FLOOR) {
		on = ODI_SW_PONQ_GLB_ON_BASE - drop;
		off = ODI_SW_PONQ_GLB_OFF_BASE - drop;
	}
	ponq_rmw(ODI_SW_PONQ_SID_GLB_TH, ODI_SW_PONQ_TH_MASK << ODI_SW_PONQ_TH_ON_SHIFT,
		 on << ODI_SW_PONQ_TH_ON_SHIFT);
	ponq_rmw(ODI_SW_PONQ_SID_GLB_TH, ODI_SW_PONQ_TH_MASK, off);
}
