// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_mib.c -- the register and MIB counter leaves behind
 * /dev/odi_sw (odi_reg.c): a switch-core word at a checked offset, and a
 * port counter by the diag counter index.
 */
#include "odi_switch_dal.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

int odi_sw_reg_get(uint32_t addr, uint32_t *value)
{
	if (!odi_switch_mmio_offset_in_bounds(addr) || (addr & 3U))
		return -EINVAL;
	*value = odi_reg_read(addr);
	return 0;
}

int odi_sw_reg_set(uint32_t addr, uint32_t value)
{
	if (!odi_switch_mmio_offset_in_bounds(addr) || (addr & 3U))
		return -EINVAL;
	odi_reg_write(addr, value);
	return 0;
}

/* One entry per counter index of src/diag/src/mib.h mib_names[] (the
 * stock diag counter order, 0-68) that a counter register names
 * unambiguously: the block and the word in it. A wide counter is two
 * words, the low half first. An index not listed is refused: mostly a
 * stock duplicate (a direction-less name beside a direction-specific
 * one) that no register tells apart (docs/SWITCH.md#mib-counters).
 */
#define ODI_SW_MIB_TX	0
#define ODI_SW_MIB_RX	1
#define ODI_SW_MIB_OAM	2
#define ODI_SW_MIB_WIDE	0x80U	/* this row and the next, low word first */

struct odi_sw_mib_map {
	uint8_t counter;	/* mib.h mib_names[] index */
	uint8_t block;		/* ODI_SW_MIB_TX/_RX/_OAM */
	uint8_t row;		/* word offset in the block, ODI_SW_MIB_WIDE set for a pair */
};

static const struct odi_sw_mib_map odi_sw_mib_map[] = {
	/* PORT_TX_COUNTERS */
	{ 53, ODI_SW_MIB_TX, 0 },	/* etherStatsTxMulticastPkts */
	{ 52, ODI_SW_MIB_TX, 1 },	/* etherStatsTxBroadcastPkts */
	{ 43, ODI_SW_MIB_TX, 2 },	/* etherStatsTxUndersizePkts */
	{ 44, ODI_SW_MIB_TX, 3 },	/* etherStatsTxOversizePkts */
	{ 45, ODI_SW_MIB_TX, 4 },	/* etherStatsTxPkts64Octets */
	{ 46, ODI_SW_MIB_TX, 5 },	/* etherStatsTxPkts65to127Octets */
	{ 47, ODI_SW_MIB_TX, 6 },	/* etherStatsTxPkts128to255Octets */
	{ 48, ODI_SW_MIB_TX, 7 },	/* etherStatsTxPkts256to511Octets */
	{ 49, ODI_SW_MIB_TX, 8 },	/* etherStatsTxPkts512to1023Octets */
	{ 50, ODI_SW_MIB_TX, 9 },	/* etherStatsTxPkts1024to1518Octets */
	{ 5,  ODI_SW_MIB_TX, 10 | ODI_SW_MIB_WIDE },	/* ifOutOctets */
	{ 42, ODI_SW_MIB_TX, 10 | ODI_SW_MIB_WIDE },	/* etherStatsTxOctets, same counter */
	{ 18, ODI_SW_MIB_TX, 12 },	/* dot3StatsSingleCollisionFrames */
	{ 19, ODI_SW_MIB_TX, 13 },	/* dot3StatsMultipleCollisionFrames */
	{ 20, ODI_SW_MIB_TX, 14 },	/* dot3StatsDeferredTransmissions */
	{ 21, ODI_SW_MIB_TX, 15 },	/* dot3StatsLateCollisions */
	{ 34, ODI_SW_MIB_TX, 16 },	/* etherStatsCollisions */
	{ 22, ODI_SW_MIB_TX, 17 },	/* dot3StatsExcessiveCollisions */
	{ 14, ODI_SW_MIB_TX, 18 },	/* dot3OutPauseFrames */
	{ 6,  ODI_SW_MIB_TX, 19 },	/* ifOutDiscards */
	{ 51, ODI_SW_MIB_TX, 20 },	/* etherStatsTxPkts1519toMaxOctets */
	{ 11, ODI_SW_MIB_TX, 22 },	/* dot1dTpPortInDiscards */
	{ 7,  ODI_SW_MIB_TX, 23 },	/* ifOutUcastPkts */
	{ 8,  ODI_SW_MIB_TX, 24 },	/* ifOutMulticastPkts */
	{ 9,  ODI_SW_MIB_TX, 25 },	/* ifOutBroadcastPkts */
	/* PORT_RX_COUNTERS */
	{ 0,  ODI_SW_MIB_RX, 0 | ODI_SW_MIB_WIDE },	/* ifInOctets */
	{ 35, ODI_SW_MIB_RX, 2 },	/* etherStatsCRCAlignErrors */
	{ 24, ODI_SW_MIB_RX, 3 },	/* dot3StatsSymbolErrors */
	{ 13, ODI_SW_MIB_RX, 4 },	/* dot3InPauseFrames */
	{ 25, ODI_SW_MIB_RX, 5 },	/* dot3ControlInUnknownOpcodes */
	{ 32, ODI_SW_MIB_RX, 6 },	/* etherStatsFragments */
	{ 33, ODI_SW_MIB_RX, 7 },	/* etherStatsJabbers */
	{ 1,  ODI_SW_MIB_RX, 8 },	/* ifInUcastPkts */
	{ 26, ODI_SW_MIB_RX, 9 },	/* etherStatsDropEvents */
	{ 2,  ODI_SW_MIB_RX, 10 },	/* ifInMulticastPkts */
	{ 3,  ODI_SW_MIB_RX, 11 },	/* ifInBroadcastPkts */
	{ 66, ODI_SW_MIB_RX, 12 },	/* etherStatsRxPkts1519toMaxOctets */
	{ 57, ODI_SW_MIB_RX, 14 },	/* etherStatsRxUndersizePkts */
	{ 59, ODI_SW_MIB_RX, 15 },	/* etherStatsRxOversizePkts */
	{ 60, ODI_SW_MIB_RX, 16 },	/* etherStatsRxPkts64Octets */
	{ 61, ODI_SW_MIB_RX, 17 },	/* etherStatsRxPkts65to127Octets */
	{ 62, ODI_SW_MIB_RX, 18 },	/* etherStatsRxPkts128to255Octets */
	{ 63, ODI_SW_MIB_RX, 19 },	/* etherStatsRxPkts256to511Octets */
	{ 64, ODI_SW_MIB_RX, 20 },	/* etherStatsRxPkts512to1023Octets */
	{ 65, ODI_SW_MIB_RX, 21 },	/* etherStatsRxPkts1024to1518Octets */
	/* PORT_OAM_COUNTERS */
	{ 68, ODI_SW_MIB_OAM, 0 },	/* outOamPduPkts */
	{ 67, ODI_SW_MIB_OAM, 1 },	/* inOamPduPkts */
};

int odi_sw_mib_get(uint32_t port, uint32_t counter, uint64_t *value)
{
	unsigned int i;

	if (port >= 4U)
		return -1;
	for (i = 0; i < sizeof odi_sw_mib_map / sizeof odi_sw_mib_map[0]; i++) {
		const struct odi_sw_mib_map *m = &odi_sw_mib_map[i];
		uint8_t row = m->row & ~ODI_SW_MIB_WIDE;
		uint32_t addr;

		if (m->counter != counter)
			continue;
		switch (m->block) {
		case ODI_SW_MIB_TX:
			addr = ODI_SW_PORT_TX_COUNTERS(port, row);
			break;
		case ODI_SW_MIB_RX:
			addr = ODI_SW_PORT_RX_COUNTERS(port, row);
			break;
		default:
			addr = ODI_SW_PORT_OAM_COUNTERS(port, row);
			break;
		}
		if (m->row & ODI_SW_MIB_WIDE) {
			uint32_t lo = odi_reg_read(addr);
			uint32_t hi = odi_reg_read(addr + 4U);

			*value = ((uint64_t)hi << 32) | lo;
		} else {
			*value = odi_reg_read(addr);
		}
		return 0;
	}
	return -1;
}
