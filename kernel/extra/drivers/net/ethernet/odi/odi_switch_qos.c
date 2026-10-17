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
 * data flow add and delete.
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
