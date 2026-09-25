/* Parsing and validating a trapped IGMP frame.
 *
 * Deliberately separate from the daemon: this file touches no syscall, so the
 * whole of it is testable on the host against synthetic frames, and the test
 * is where the header-length and checksum arithmetic is actually checked.
 */
#include "igmpd.h"

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

uint16_t igmp_cksum(const uint8_t *p, uint32_t len)
{
	uint32_t sum = 0;
	uint32_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += rd16(p + i);
	/* An odd trailing byte is the high half of a zero-padded word. This is
	 * reachable for IGMP -- an IPv4 header is always even, a v3 report is
	 * not necessarily. */
	if (i < len)
		sum += (uint32_t)p[i] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

const char *igmp_parse_str(enum igmp_parse_rc rc)
{
	switch (rc) {
	case IGMP_OK:         return "ok";
	case IGMP_SHORT:      return "truncated";
	case IGMP_NOT_IP:     return "not IPv4";
	case IGMP_IS_MLD:     return "IPv6 (MLD)";
	case IGMP_BAD_IP:     return "malformed IPv4 header";
	case IGMP_IP_CKSUM:   return "IPv4 checksum";
	case IGMP_NOT_IGMP:   return "not protocol 2";
	case IGMP_BAD_IGMP:   return "malformed IGMP message";
	case IGMP_IGMP_CKSUM: return "IGMP checksum";
	}
	return "?";
}

const char *igmp_type_str(uint8_t type)
{
	switch (type) {
	case IGMP_QUERY:     return "query";
	case IGMP_V1_REPORT: return "v1 report";
	case IGMP_V2_REPORT: return "v2 report";
	case IGMP_V2_LEAVE:  return "leave";
	case IGMP_V3_REPORT: return "v3 report";
	}
	return "unknown";
}

/* Walk the IPv4 options looking for the router alert, option 148.
 *
 * Not a validity test: RFC 2236 senders are supposed to set it and plenty do
 * not, and dropping a report because an option is missing is how a snooper
 * stops forwarding traffic someone asked for. Recorded, and left to policy.
 */
static int has_router_alert(const uint8_t *opt, uint32_t len)
{
	uint32_t i = 0;

	while (i < len) {
		uint8_t t = opt[i];

		if (t == 0)                     /* end of list */
			return 0;
		if (t == 1) {                   /* no-op, one byte */
			i++;
			continue;
		}
		if (i + 1 >= len)
			return 0;
		if (opt[i + 1] < 2)             /* a length that cannot advance */
			return 0;
		if (t == 148)
			return 1;
		i += opt[i + 1];
	}
	return 0;
}

enum igmp_parse_rc igmp_parse(const uint8_t *buf, uint32_t len,
			      struct igmp_rx *rx)
{
	const uint8_t *eth, *ip, *body;
	uint32_t eth_len, off, hlen, total, body_len;
	uint16_t ethertype;

	for (unsigned i = 0; i < sizeof *rx; i++)
		((uint8_t *)rx)[i] = 0;

	if (len < IGMP_RX_PREFIX + ETH_HDR_LEN)
		return IGMP_SHORT;

	rx->port = buf[0];
	rx->vid = (uint16_t)(((uint16_t)(buf[1] & 0x0f) << 8) | buf[2]);

	eth = buf + IGMP_RX_PREFIX;
	eth_len = len - IGMP_RX_PREFIX;
	rx->eth = eth;
	rx->eth_len = eth_len;
	rx->dst = eth;
	rx->src = eth + 6;

	ethertype = rd16(eth + 12);
	off = ETH_HDR_LEN;
	if (ethertype == ETH_P_8021Q) {
		if (eth_len < ETH_HDR_LEN + 4)
			return IGMP_SHORT;
		rx->tagged = 1;
		/* The tag the driver re-inserted. It agrees with the prefix,
		 * and is preferred because it is what the frame itself says --
		 * the prefix is only a fallback for an untagged frame. */
		rx->vid = (uint16_t)(rd16(eth + 14) & 0x0fff);
		ethertype = rd16(eth + 16);
		off = ETH_HDR_LEN + 4;
	}
	rx->ethertype = ethertype;

	if (ethertype == ETH_P_IPV6)
		return IGMP_IS_MLD;
	if (ethertype != ETH_P_IP)
		return IGMP_NOT_IP;

	if (eth_len < off + 20)
		return IGMP_SHORT;
	ip = eth + off;
	rx->ip = ip;

	if ((ip[0] >> 4) != 4)
		return IGMP_BAD_IP;
	hlen = (uint32_t)(ip[0] & 0x0f) * 4;
	if (hlen < 20 || off + hlen > eth_len)
		return IGMP_BAD_IP;
	rx->ip_hlen = hlen;

	total = rd16(ip + 2);
	if (total < hlen)
		return IGMP_BAD_IP;
	/* The frame may be longer than the datagram -- Ethernet pads to 60
	 * bytes and an IGMP packet is well under that. Shorter is a truncation
	 * and the checksums below would be computed over the wrong bytes. */
	if (off + total > eth_len)
		return IGMP_SHORT;

	rx->ttl = ip[8];
	rx->sip = rd32(ip + 12);
	rx->dip = rd32(ip + 16);
	if (hlen > 20)
		rx->router_alert = has_router_alert(ip + 20, hlen - 20);

	if (igmp_cksum(ip, hlen) != 0)
		return IGMP_IP_CKSUM;
	if (ip[9] != IPPROTO_IGMP)
		return IGMP_NOT_IGMP;

	body = ip + hlen;
	body_len = total - hlen;
	rx->body = body;
	rx->body_len = body_len;
	if (body_len < 8)
		return IGMP_BAD_IGMP;

	rx->type = body[0];
	rx->max_resp = body[1];
	if (igmp_cksum(body, body_len) != 0)
		return IGMP_IGMP_CKSUM;

	if (rx->type == IGMP_V3_REPORT) {
		/* Group records, not a group address. The records themselves
		 * are the caller business; what is checked here is only that
		 * the count does not run off the end of the shortest possible
		 * set of records. */
		rx->nrec = rd16(body + 6);
		if ((uint32_t)rx->nrec * 8 + 8 > body_len)
			return IGMP_BAD_IGMP;
	} else {
		rx->group = rd32(body + 4);
	}
	return IGMP_OK;
}
