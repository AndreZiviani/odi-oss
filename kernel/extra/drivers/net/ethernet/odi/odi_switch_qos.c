// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_qos.c -- upstream queue leaves: the PON queue setup of cmd 23
 * in its two shapes, the per-queue scheduling slot, and the flow-to-queue
 * map of cmd 25 (upstream side). odi_switch_dal.h has the contract,
 * docs/SWITCH.md#command-leaves the capture each sequence reproduces.
 *
 * Most of these writes land in PONQ_COUNT_MASK, a table of words whose
 * fields are not decoded; the indexes below are named after the
 * parameter each one carries.
 */
#include "odi_switch_dal.h"
#include "odi_switch_reg.h"

#define ODI_SW_PONQ_IDX_QUEUE_BITMASK	207U	/* queue_add_ext() bitmask_207 */
#define ODI_SW_PONQ_IDX_QUEUE_BASE	15U	/* queue_add_ext() base_15 */
#define ODI_SW_PONQ_IDX_QUEUE_VAL	208U	/* queue_add_ext() val_208 */
#define ODI_SW_PONQ_IDX_QUEUE_BITMASK_A	212U	/* queue_add_ext(), !use_213 */
#define ODI_SW_PONQ_IDX_QUEUE_BITMASK_B	213U	/* queue_add_ext(), use_213 */
#define ODI_SW_PONQ_IDX_QUEUE_RESET	60U	/* queue_add_ext(), +n, cleared */
#define ODI_SW_PONQ_IDX_QUEUE_MAX	125U	/* queue_add_ext(), +n, set to 0x3ffffU */
#define ODI_SW_PONQ_IDX_FLOW_BITMASK	37U	/* flow_queue_set() bitmask_37 */
#define ODI_SW_PONQ_IDX_FLOW_WORD	235U	/* flow_queue_set() word235_a/word235_b */
#define ODI_SW_PONQ_IDX_FLOW_VAL_A	20U	/* flow_queue_set(), !use_21 */
#define ODI_SW_PONQ_IDX_FLOW_VAL_B	21U	/* flow_queue_set(), use_21 */
#define ODI_SW_PONQ_IDX_QOS_SCHED_BASE	190U	/* qos_sched_set(), +n */

/* PORT_QUEUE_MAP holds a 2-bit index per port, which 0xd4 (the only value
 * captured) does not fit, so the word is written raw.
 */
void odi_sw_ponmac_queue_add(uint32_t th)
{
	odi_reg_write(ODI_SW_PORT_QUEUE_MAP_BASE, th);
}

void odi_sw_ponmac_queue_add_ext(uint32_t n, uint32_t bitmask_207, uint32_t base_15,
				  uint32_t sched_value, uint32_t val_208,
				  uint32_t bitmask_212_213, int use_213)
{
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_BITMASK), bitmask_207);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_BASE), base_15);
	odi_sw_qos_sched_set(n, sched_value);
	/* The first queue writes +208 before +212/+213, every later queue
	 * after: the order of every captured instance.
	 */
	if (n == 0) {
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_VAL), val_208);
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(use_213 ? ODI_SW_PONQ_IDX_QUEUE_BITMASK_B : ODI_SW_PONQ_IDX_QUEUE_BITMASK_A), bitmask_212_213);
	} else {
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(use_213 ? ODI_SW_PONQ_IDX_QUEUE_BITMASK_B : ODI_SW_PONQ_IDX_QUEUE_BITMASK_A), bitmask_212_213);
		odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_VAL), val_208);
	}
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_RESET + n), 0);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QUEUE_MAX + n), 0x3ffffU);
}

void odi_sw_ponmac_flow_queue_set(uint32_t slot, uint32_t gem_port_id, uint32_t bitmask_37,
				   uint32_t word235_a, uint32_t word235_b,
				   uint32_t val_2021, int use_21)
{
	odi_reg_write(ODI_SW_US_GEM_PORT_MAP(slot), gem_port_id);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_FLOW_BITMASK), bitmask_37);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_FLOW_WORD), word235_a);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_FLOW_WORD), word235_b);
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(use_21 ? ODI_SW_PONQ_IDX_FLOW_VAL_B : ODI_SW_PONQ_IDX_FLOW_VAL_A), val_2021);
}

void odi_sw_qos_sched_set(uint32_t n, uint32_t value)
{
	odi_reg_write(ODI_SW_PONQ_COUNT_MASK(ODI_SW_PONQ_IDX_QOS_SCHED_BASE + n), value);
}
