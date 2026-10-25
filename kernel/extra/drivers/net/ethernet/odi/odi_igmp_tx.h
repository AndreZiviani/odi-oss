/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_igmp_tx.h -- the one kind of frame the CPU sends toward the fibre
 * besides an OMCI reply: a report of the host's that igmpd took (the TRAP
 * mode of the snooping lease, odi_switch_mcast.h) and sends up in its place,
 * tagged as the OLT's multicast profile says. Plain C, no kernel dependency,
 * so the host tests build it with the host cc.
 *
 * What may go up is narrow on purpose, and checked here rather than trusted
 * to the daemon: an IPv4 frame of protocol 2 (IGMP), behind at most two VLAN
 * tags, to an IPv4 multicast MAC (01:00:5e, bit 23 clear: every report and
 * leave goes to a group address or 224.0.0.2 or 224.0.0.22), at least the 60
 * bytes of a minimum Ethernet frame (the NIC adds the FCS but does not pad).
 *
 * The descriptor is the one the OMCI reply already uses to reach the OLT
 * (odi_omci.c): PORT_SEL with the PON port alone and the upstream flow as
 * the stream, which is a directed transmit onto that flow, not a lookup; and
 * NO_LEARN, which matters more here than for OMCI: the frame carries the
 * host's source MAC, and a learn from the CPU port would move the host's
 * address there, after which the switch would send the host's own traffic
 * toward the CPU, where the PON isolation drops it.
 */
#ifndef ODI_IGMP_TX_H
#define ODI_IGMP_TX_H

#include "odi_nic_hw.h"
#include "odi_igmp_rx.h"

#define ODI_IGMP_TX_MIN		60u
#define ODI_IGMP_TX_MAX		1536u
#define ODI_IGMP_TX_PON_PORT	2u
/* The stream field is seven bits; an upstream flow id is the same width
 * (CF action bits 30..24, docs/SWITCH.md, "CF rows"). */
#define ODI_IGMP_TX_FLOW_MAX	0x7fu

#define ODI_IGMP_TX_OPTS1	(ODI_TXD_BIT25 | ODI_TXD_NO_LEARN | ODI_TXD_PORT_SEL)
#define ODI_IGMP_TX_OPTS2	(ODI_TXD_TAGGED | ODI_TXD_BIT19)

static inline uint32_t odi_igmp_tx_opts3(uint32_t flow)
{
	return ((1u << ODI_IGMP_TX_PON_PORT) << ODI_TXD_PORTS_SHIFT) |
	       ((flow & ODI_TXD_GEM_MASK) << ODI_TXD_GEM_SHIFT);
}

static inline int odi_igmp_tx_frame_ok(const uint8_t *f, uint32_t len)
{
	if (!f || len < ODI_IGMP_TX_MIN || len > ODI_IGMP_TX_MAX)
		return 0;
	if (f[0] != 0x01 || f[1] != 0x00 || f[2] != 0x5e || (f[3] & 0x80))
		return 0;
	return odi_igmp_frame_wanted(f, len);
}

#endif /* ODI_IGMP_TX_H */
