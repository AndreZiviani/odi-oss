/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_igmp_rx.h -- the frames igmpd is handed: which host-port frames they
 * are, and how they are framed on the redirect channel. Plain C, no kernel
 * dependency, so test/odi_omci_test.c builds it with the host cc.
 *
 * The copies come from the ACL rules of odi_switch_mcast.c, which match
 * IPv4 protocol 2 on the UNI port and, while the lease holds MLD, the MLD
 * types of ICMPv6. The switch does not say which rule sent a frame to the
 * CPU in a way this driver relies on (the receive reason of a rule's copy
 * is not part of its contract here), so the hook recognises the frame
 * itself: from the UNI port, IPv4 protocol 2, or an MLD message. Anything
 * else the UNI sends the CPU (ARP, unicast to the stick) goes on to the net
 * device as before.
 *
 * Framing, the same transport omcid has on redirect type 1 with the prefix
 * the IGMP uid has (src/igmp/igmpd.h IGMP_RX_PREFIX): the 16-byte netlink
 * header, then three bytes, the source port ONE-based and the VLAN id in
 * twelve bits, then the Ethernet frame. Bit 7 of the second byte, above the
 * VLAN id, says the frame was taken rather than copied (the lease's TRAP
 * mode): the switch did not send it on, so it goes upstream only if igmpd
 * sends it.
 */
#ifndef ODI_IGMP_RX_H
#define ODI_IGMP_RX_H

#include "odi_omci_wire.h"

#define ODI_IGMP_REDIRECT_TYPE	4u
#define ODI_IGMP_RX_PREFIX	3u
#define ODI_IGMP_RX_TAKEN	0x80u	/* in prefix byte 1 */
#define ODI_IGMP_UNI_PORT	0u

#define ODI_IGMP_ETH_P_IP	0x0800u
#define ODI_IGMP_ETH_P_IPV6	0x86ddu
#define ODI_IGMP_ETH_P_8021Q	0x8100u
#define ODI_IGMP_ETH_P_8021AD	0x88a8u
#define ODI_IGMP_IPPROTO	2u

/* MLD (RFC 2710, RFC 3810): ICMPv6 messages, behind a hop-by-hop header
 * that carries the router alert. */
#define ODI_MLD_NH_ICMPV6	58u
#define ODI_MLD_QUERY		130u
#define ODI_MLD_V1_REPORT	131u
#define ODI_MLD_V1_DONE		132u
#define ODI_MLD_V2_REPORT	143u
/* Extension headers walked to find the ICMPv6 message, at most: an MLD
 * message has one, the hop-by-hop. */
#define ODI_MLD_EXT_MAX		4u

/* An IPv4 frame, behind at most two VLAN tags, whose protocol is IGMP. The
 * checksum and the IGMP message are igmpd's to judge: a bad frame is
 * worth seeing there, and costs nothing here.
 */
static inline int odi_igmp_frame_wanted(const uint8_t *f, uint32_t len)
{
	uint32_t off = 12;
	unsigned int tags = 0;
	uint16_t type;

	if (len < 14)
		return 0;
	type = (uint16_t)((f[off] << 8) | f[off + 1]);
	while ((type == ODI_IGMP_ETH_P_8021Q || type == ODI_IGMP_ETH_P_8021AD) && tags < 2) {
		off += 4;
		tags++;
		if (len < off + 2)
			return 0;
		type = (uint16_t)((f[off] << 8) | f[off + 1]);
	}
	if (type != ODI_IGMP_ETH_P_IP)
		return 0;
	off += 2;
	/* The fixed IPv4 header: version 4, header length of 5 words or more,
	 * and the protocol octet inside what was received. */
	if (len < off + 20)
		return 0;
	if ((f[off] >> 4) != 4 || (f[off] & 0x0f) < 5)
		return 0;
	return f[off + 9] == ODI_IGMP_IPPROTO;
}

/* The ICMPv6 type of an MLD message (one of the four above), behind at most
 * two VLAN tags and at most ODI_MLD_EXT_MAX extension headers (hop-by-hop,
 * routing, destination options), or 0 for any other frame. As for IGMP, the
 * checksum and the message are igmpd's to judge. */
static inline uint32_t odi_mld_frame_type(const uint8_t *f, uint32_t len)
{
	uint32_t off = 12, ext = 0;
	unsigned int tags = 0;
	uint16_t type;
	uint8_t nh;

	if (len < 14)
		return 0;
	type = (uint16_t)((f[off] << 8) | f[off + 1]);
	while ((type == ODI_IGMP_ETH_P_8021Q || type == ODI_IGMP_ETH_P_8021AD) && tags < 2) {
		off += 4;
		tags++;
		if (len < off + 2)
			return 0;
		type = (uint16_t)((f[off] << 8) | f[off + 1]);
	}
	if (type != ODI_IGMP_ETH_P_IPV6)
		return 0;
	off += 2;
	/* The fixed IPv6 header, version 6, its next header at byte 6. */
	if (len < off + 40 || (f[off] >> 4) != 6)
		return 0;
	nh = f[off + 6];
	off += 40;
	while (nh == 0 || nh == 43 || nh == 60) {
		if (++ext > ODI_MLD_EXT_MAX || len < off + 2)
			return 0;
		nh = f[off];
		off += 8u * ((uint32_t)f[off + 1] + 1u);
	}
	if (nh != ODI_MLD_NH_ICMPV6 || len < off + 1)
		return 0;
	switch (f[off]) {
	case ODI_MLD_QUERY:
	case ODI_MLD_V1_REPORT:
	case ODI_MLD_V1_DONE:
	case ODI_MLD_V2_REPORT:
		return f[off];
	}
	return 0;
}

/* Whether the IGMP RX hook takes a frame: only while the lease holds the
 * copy, only from the UNI port, and only IGMP. A frame from the PON port is
 * never one of them, whatever the lease holds and whatever it carries: the
 * hook leaves it to the NIC, which drops every PON frame no hook took
 * (docs/SWITCH.md, "PON port isolation"). */
static inline int odi_igmp_rx_takes(uint32_t copy_on, uint32_t src_port,
				    const uint8_t *f, uint32_t len)
{
	return copy_on && src_port == ODI_IGMP_UNI_PORT && odi_igmp_frame_wanted(f, len);
}

/* The same for MLD: only while the lease holds the MLD copy, only from the
 * UNI port, and only an MLD message. Never a frame from the PON, the OLT's
 * MLD queries included. */
static inline int odi_mld_rx_takes(uint32_t mld_on, uint32_t src_port,
				   const uint8_t *f, uint32_t len)
{
	return mld_on && src_port == ODI_IGMP_UNI_PORT && odi_mld_frame_type(f, len) != 0;
}

static inline uint32_t odi_igmp_rx_total(uint32_t framelen)
{
	return ODI_OMCI_NLMSG_HDR + ODI_IGMP_RX_PREFIX + framelen;
}

/* port is the hardware port number: the prefix carries it plus one. taken:
 * the frame was taken, not copied. */
static inline uint32_t odi_igmp_build_rx(uint8_t *buf, uint32_t port, uint32_t vid,
					 int taken, const uint8_t *frame, uint32_t framelen)
{
	uint32_t total = odi_igmp_rx_total(framelen);

	odi_omci_put32(buf + 0, total);
	odi_omci_put32(buf + 4, 0);
	odi_omci_put32(buf + 8, 0);
	odi_omci_put32(buf + 12, 0);
	buf[ODI_OMCI_NLMSG_HDR] = (uint8_t)(port + 1u);
	buf[ODI_OMCI_NLMSG_HDR + 1] = (uint8_t)(((vid >> 8) & 0x0f) |
						(taken ? ODI_IGMP_RX_TAKEN : 0u));
	buf[ODI_OMCI_NLMSG_HDR + 2] = (uint8_t)vid;
	memcpy(buf + ODI_OMCI_NLMSG_HDR + ODI_IGMP_RX_PREFIX, frame, framelen);
	return total;
}

#endif /* ODI_IGMP_RX_H */
