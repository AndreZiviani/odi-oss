/* MIB counter names, by the index ODI_SW_IOC_MIB_GET takes (odi_switch_dal.c,
 * odi_sw_mib_get()). The index is the ABI, and it is the stock CLI's counter
 * order, so the order matters. The names are RFC 1213/1493/2665 names and are
 * also the labels the exporter matches in `mib dump counter` output. */
#ifndef ODI_MIB_H
#define ODI_MIB_H

static const char *const mib_names[] = {
	"ifInOctets",  /* 0 */
	"ifInUcastPkts",
	"ifInMulticastPkts",
	"ifInBroadcastPkts",
	"ifInDiscards",
	"ifOutOctets",
	"ifOutDiscards",
	"ifOutUcastPkts",
	"ifOutMulticastPkts",
	"ifOutBroadcastPkts",
	"dot1dPortDelayExceedDiscards",  /* 10 */
	"dot1dTpPortInDiscards",
	"dot1dTpHcPortInDiscards",
	"dot3InPauseFrames",
	"dot3OutPauseFrames",
	"dot3OutPauseOnFrames",
	"dot3StatsAlignmentErrors",
	"dot3StatsFcsErrors",
	"dot3StatsSingleCollisionFrames",
	"dot3StatsMultipleCollisionFrames",
	"dot3StatsDeferredTransmissions",  /* 20 */
	"dot3StatsLateCollisions",
	"dot3StatsExcessiveCollisions",
	"dot3StatsFrameTooLongs",
	"dot3StatsSymbolErrors",
	"dot3ControlInUnknownOpcodes",
	"etherStatsDropEvents",
	"etherStatsOctets",
	"etherStatsBroadcastPkts",
	"etherStatsMulticastPkts",
	"etherStatsUndersizePkts",  /* 30 */
	"etherStatsOversizePkts",
	"etherStatsFragments",
	"etherStatsJabbers",
	"etherStatsCollisions",
	"etherStatsCRCAlignErrors",
	"etherStatsPkts64Octets",
	"etherStatsPkts65to127Octets",
	"etherStatsPkts128to255Octets",
	"etherStatsPkts256to511Octets",
	"etherStatsPkts512to1023Octets",  /* 40 */
	"etherStatsPkts1024to1518Octets",
	"etherStatsTxOctets",
	"etherStatsTxUndersizePkts",
	"etherStatsTxOversizePkts",
	"etherStatsTxPkts64Octets",
	"etherStatsTxPkts65to127Octets",
	"etherStatsTxPkts128to255Octets",
	"etherStatsTxPkts256to511Octets",
	"etherStatsTxPkts512to1023Octets",
	"etherStatsTxPkts1024to1518Octets",  /* 50 */
	"etherStatsTxPkts1519toMaxOctets",
	"etherStatsTxBroadcastPkts",
	"etherStatsTxMulticastPkts",
	"etherStatsTxFragments",
	"etherStatsTxJabbers",
	"etherStatsTxCRCAlignErrors",
	"etherStatsRxUndersizePkts",
	"etherStatsRxUndersizeDropPkts",
	"etherStatsRxOversizePkts",
	"etherStatsRxPkts64Octets",  /* 60 */
	"etherStatsRxPkts65to127Octets",
	"etherStatsRxPkts128to255Octets",
	"etherStatsRxPkts256to511Octets",
	"etherStatsRxPkts512to1023Octets",
	"etherStatsRxPkts1024to1518Octets",
	"etherStatsRxPkts1519toMaxOctets",
	"inOamPduPkts",
	"outOamPduPkts",
};

#endif
