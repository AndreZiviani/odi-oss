/* igmp_parse, against frames built here.
 *
 * Built for the target and run under qemu, because that is where the parse has
 * to be right: big-endian MIPS, 32-bit longs, no libc, and a checksum whose
 * folding is easy to get subtly wrong on one of those. There is no device and
 * no netlink in any of it -- frame.c touches no syscall, which is the whole
 * reason it is a separate file.
 *
 * Every frame is assembled rather than pasted as a hex blob, so the checksums
 * are computed by the same routine the parser uses. That would hide a checksum
 * that is wrong in a self-consistent way, so the fixed vectors at the end pin
 * the arithmetic against numbers taken from somewhere else.
 */
#include "igmpd.h"
#include "io.h"

static int failures;

static void ok(int cond, const char *what)
{
	if (cond) {
		out_fmt("ok    %s\n", what);
	} else {
		out_fmt("FAIL  %s\n", what);
		failures++;
	}
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* One redirect payload: prefix, Ethernet, optional tag, IPv4 with the router
 * alert, and an IGMP message. Returns the total length. */
static uint32_t build(uint8_t *b, uint8_t port, uint16_t vid, int tagged,
		      uint8_t type, uint32_t group, uint32_t sip, uint8_t ttl)
{
	uint8_t *eth, *ip, *igmp;
	uint32_t off, hlen = 24, ilen = 8;

	for (int i = 0; i < 128; i++)
		b[i] = 0;

	b[0] = port;
	b[1] = (uint8_t)((vid >> 8) & 0x0f);
	b[2] = (uint8_t)(vid & 0xff);

	eth = b + IGMP_RX_PREFIX;
	/* 01:00:5e for an IPv4 multicast destination, low 23 bits of the group */
	eth[0] = 0x01; eth[1] = 0x00; eth[2] = 0x5e;
	eth[3] = (uint8_t)((group >> 16) & 0x7f);
	eth[4] = (uint8_t)((group >> 8) & 0xff);
	eth[5] = (uint8_t)(group & 0xff);
	eth[6] = 0x00; eth[7] = 0x11; eth[8] = 0x22;
	eth[9] = 0x33; eth[10] = 0x44; eth[11] = 0x55;

	if (tagged) {
		put16(eth + 12, ETH_P_8021Q);
		put16(eth + 14, vid);
		put16(eth + 16, ETH_P_IP);
		off = ETH_HDR_LEN + 4;
	} else {
		put16(eth + 12, ETH_P_IP);
		off = ETH_HDR_LEN;
	}

	ip = eth + off;
	ip[0] = (uint8_t)(0x40 | (hlen / 4));
	put16(ip + 2, (uint16_t)(hlen + ilen));
	ip[8] = ttl;
	ip[9] = IPPROTO_IGMP;
	put32(ip + 12, sip);
	put32(ip + 16, group);
	ip[20] = 148;                 /* router alert */
	ip[21] = 4;
	put16(ip + 10, 0);
	put16(ip + 10, igmp_cksum(ip, hlen));

	igmp = ip + hlen;
	igmp[0] = type;
	igmp[1] = 0;
	put32(igmp + 4, group);
	put16(igmp + 2, 0);
	put16(igmp + 2, igmp_cksum(igmp, ilen));

	return IGMP_RX_PREFIX + off + hlen + ilen;
}

int main(void)
{
	uint8_t b[128];
	struct igmp_rx rx;
	uint32_t n;

	/* A tagged v2 report, which is the common case on this device. */
	n = build(b, 3, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	ok(igmp_parse(b, n, &rx) == IGMP_OK, "a tagged v2 report parses");
	ok(rx.port == 3, "the port comes from the prefix, one-based as sent");
	ok(rx.vid == 100 && rx.tagged, "the vlan comes from the tag");
	ok(rx.type == IGMP_V2_REPORT, "the type is a report");
	ok(rx.group == 0xe1010101, "the group address is read");
	ok(rx.sip == 0xc0a80105, "the source address is read");
	ok(rx.router_alert, "the router alert option is found past a 24-byte header");
	ok(rx.ttl == 1, "ttl 1");

	/* Untagged: the prefix is the only place the vlan can come from, and
	 * here it is zero, which is what the driver sends when ctagva is clear. */
	n = build(b, 1, 0, 0, IGMP_V2_LEAVE, 0xe1010101, 0xc0a80105, 1);
	ok(igmp_parse(b, n, &rx) == IGMP_OK, "an untagged leave parses");
	ok(!rx.tagged && rx.vid == 0, "untagged, vlan 0");
	ok(rx.type == IGMP_V2_LEAVE, "the type is a leave");

	/* The prefix carries a vlan the frame does not: only reachable if the
	 * driver ever pushed the prefix without the tag, and the parser has to
	 * prefer the prefix rather than report vlan 0. */
	n = build(b, 2, 0, 0, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	b[1] = 0x01;
	b[2] = 0x2c;                      /* 300 */
	ok(igmp_parse(b, n, &rx) == IGMP_OK, "untagged with a prefix vlan parses");
	ok(rx.vid == 300 && !rx.tagged, "the prefix vlan is used when there is no tag");

	/* Corruption, one field at a time. Each has to be reported as itself:
	 * a parser that returns one generic failure cannot be debugged on a
	 * device with no console. */
	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	b[IGMP_RX_PREFIX + 18 + 10] ^= 0xff;
	ok(igmp_parse(b, n, &rx) == IGMP_IP_CKSUM, "a bad IPv4 checksum is named");

	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	b[n - 1] ^= 0xff;
	ok(igmp_parse(b, n, &rx) == IGMP_IGMP_CKSUM, "a bad IGMP checksum is named");

	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	b[IGMP_RX_PREFIX + 18 + 9] = 17;  /* UDP */
	put16(b + IGMP_RX_PREFIX + 18 + 10, 0);
	put16(b + IGMP_RX_PREFIX + 18 + 10, igmp_cksum(b + IGMP_RX_PREFIX + 18, 24));
	ok(igmp_parse(b, n, &rx) == IGMP_NOT_IGMP, "protocol 17 is not IGMP");

	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	put16(b + IGMP_RX_PREFIX + 16, ETH_P_IPV6);
	ok(igmp_parse(b, n, &rx) == IGMP_IS_MLD, "IPv6 is reported as MLD, not as junk");

	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	put16(b + IGMP_RX_PREFIX + 16, 0x0806);
	ok(igmp_parse(b, n, &rx) == IGMP_NOT_IP, "ARP is not IPv4");

	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	ok(igmp_parse(b, IGMP_RX_PREFIX + 10, &rx) == IGMP_SHORT,
	   "a frame shorter than an Ethernet header is truncated");

	/* The IP total length claiming more than arrived. The checksums are
	 * still right over the bytes that are there, so nothing but this check
	 * catches it -- and computing a checksum past the end of the buffer is
	 * what a parser without it would do. */
	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	put16(b + IGMP_RX_PREFIX + 18 + 2, 400);
	put16(b + IGMP_RX_PREFIX + 18 + 10, 0);
	put16(b + IGMP_RX_PREFIX + 18 + 10, igmp_cksum(b + IGMP_RX_PREFIX + 18, 24));
	ok(igmp_parse(b, n, &rx) == IGMP_SHORT, "a total length past the buffer is truncated");

	/* Ethernet pads to 60 bytes; the datagram is 32. A parser that treated
	 * the frame length as the datagram length would checksum the padding. */
	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 1);
	ok(igmp_parse(b, n + 20, &rx) == IGMP_OK, "trailing padding is ignored");

	/* ttl 255 and no router alert: still a report. Dropping these is how a
	 * snooper stops forwarding traffic somebody asked for. */
	n = build(b, 1, 100, 1, IGMP_V2_REPORT, 0xe1010101, 0xc0a80105, 255);
	ok(igmp_parse(b, n, &rx) == IGMP_OK && rx.ttl == 255,
	   "ttl 255 is accepted and reported");

	/* The checksum, against numbers from outside this file. RFC 1071 gives
	 * 0xddf2 as the sum of its worked example, so the complement is 0x220d.
	 * A checksum that is wrong consistently would pass every test above. */
	{
		static const uint8_t ex[] = {0x00, 0x01, 0xf2, 0x03,
					     0xf4, 0xf5, 0xf6, 0xf7};
		ok(igmp_cksum(ex, sizeof ex) == 0x220d,
		   "the RFC 1071 worked example sums to 0x220d");
		/* A byte on its own is the high half of the word, not the low
		 * half. The odd-length path is only reachable from a v3 report,
		 * so nothing else here would find it wrong. */
		static const uint8_t odd[] = {0xff};
		ok(igmp_cksum(odd, 1) == 0x00ff,
		   "an odd trailing byte is the high half of the word");
	}

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
