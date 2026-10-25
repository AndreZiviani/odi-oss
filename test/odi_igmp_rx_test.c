/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_igmp_rx_test.c -- the frames the kernel hands igmpd (odi_igmp_rx.h),
 * checked against igmpd's own parser (src/igmp/frame.c, which has no
 * syscalls and builds on the host): the delivery the hook builds must be
 * the delivery igmp_parse() reads, port, VLAN and frame in the places the
 * daemon takes them from, so the two ends cannot drift apart.
 */
#include <stdio.h>
#include <string.h>

#include "../kernel/extra/drivers/net/ethernet/odi/odi_igmp_rx.h"
#include "../src/igmp/igmpd.h"
#include "../src/igmp/frame.c"
#include "../src/igmp/group.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* An IGMPv2 report for 239.1.2.3, untagged or behind `ntags` VLAN tags,
 * with valid checksums. Returns the length. */
static uint32_t v2_report(uint8_t *f, unsigned int ntags, uint16_t tpid)
{
	uint8_t *p = f;
	uint8_t *ip;
	uint16_t c;
	unsigned int i;

	memset(f, 0, 64);
	memcpy(p, "\x01\x00\x5e\x01\x02\x03", 6);
	memcpy(p + 6, "\x02\x00\x00\x00\x00\x09", 6);
	p += 12;
	for (i = 0; i < ntags; i++) {
		*p++ = (uint8_t)(tpid >> 8);
		*p++ = (uint8_t)tpid;
		*p++ = 0x00;
		*p++ = (uint8_t)(0x0b + i);
	}
	*p++ = 0x08;
	*p++ = 0x00;
	ip = p;
	memset(ip, 0, 24);
	ip[0] = 0x46;		/* version 4, 6 words: the router alert option */
	ip[3] = 32;		/* total length: 24 + 8 */
	ip[8] = 1;
	ip[9] = 2;
	memcpy(ip + 12, "\xc0\xa8\x01\x09", 4);
	memcpy(ip + 16, "\xef\x01\x02\x03", 4);
	ip[20] = 0x94; ip[21] = 0x04;
	c = igmp_cksum(ip, 24);
	ip[10] = (uint8_t)(c >> 8);
	ip[11] = (uint8_t)c;
	p = ip + 24;
	p[0] = 0x16;
	memcpy(p + 4, "\xef\x01\x02\x03", 4);
	c = igmp_cksum(p, 8);
	p[2] = (uint8_t)(c >> 8);
	p[3] = (uint8_t)c;
	return (uint32_t)(p + 8 - f);
}

static void test_classifier(void)
{
	uint8_t f[128];
	uint32_t n;

	n = v2_report(f, 0, 0x8100);
	CHECK(odi_igmp_frame_wanted(f, n), "an untagged IGMP report");
	n = v2_report(f, 1, 0x8100);
	CHECK(odi_igmp_frame_wanted(f, n), "behind one C-tag");
	n = v2_report(f, 2, 0x88a8);
	CHECK(odi_igmp_frame_wanted(f, n), "behind two tags");
	n = v2_report(f, 3, 0x8100);
	CHECK(!odi_igmp_frame_wanted(f, n), "three tags are not looked into");

	n = v2_report(f, 0, 0x8100);
	f[12 + 2 + 9] = 17;
	CHECK(!odi_igmp_frame_wanted(f, n), "UDP is not wanted");
	f[12 + 2 + 9] = 2;
	f[12] = 0x86; f[13] = 0xdd;
	CHECK(!odi_igmp_frame_wanted(f, n), "IPv6 is not wanted: igmpd does not do MLD");
	f[12] = 0x08; f[13] = 0x06;
	CHECK(!odi_igmp_frame_wanted(f, n), "ARP is not wanted");
	f[12] = 0x08; f[13] = 0x00;
	f[14] = 0x65;
	CHECK(!odi_igmp_frame_wanted(f, n), "an IP version other than 4");
	f[14] = 0x44;
	CHECK(!odi_igmp_frame_wanted(f, n), "an IP header shorter than 5 words");
	f[14] = 0x46;
	CHECK(odi_igmp_frame_wanted(f, n), "restored, wanted again");

	CHECK(!odi_igmp_frame_wanted(f, 13), "shorter than an Ethernet header");
	CHECK(!odi_igmp_frame_wanted(f, 14 + 19), "shorter than an IPv4 header");
	CHECK(!odi_igmp_frame_wanted(f, 0), "empty");
	f[12] = 0x81; f[13] = 0x00;
	CHECK(!odi_igmp_frame_wanted(f, 16), "a tag cut off");
}

static void test_delivery_parses(void)
{
	uint8_t f[128], buf[256];
	struct igmp_rx rx;
	uint32_t n, total;

	n = v2_report(f, 0, 0x8100);
	total = odi_igmp_build_rx(buf, 0, 0, 0, f, n);
	CHECK(total == odi_igmp_rx_total(n) && total == 16u + 3u + n, "the length");
	CHECK(odi_omci_get32(buf) == total, "the netlink length field");
	CHECK(ODI_IGMP_REDIRECT_TYPE == IGMP_REDIRECT_UID, "the redirect type is igmpd's uid");
	CHECK(ODI_IGMP_RX_PREFIX == IGMP_RX_PREFIX, "the prefix is what igmpd reads");
	/* igmpd's receive loop strips the 16-byte netlink header (nl_recv) and
	 * parses from there. */
	CHECK(igmp_parse(buf + ODI_OMCI_NLMSG_HDR, total - ODI_OMCI_NLMSG_HDR, &rx) == IGMP_OK,
	      "igmp_parse accepts the delivery");
	CHECK(rx.port == 1, "the UNI port (0) is port 1 in the prefix: ONE-based");
	CHECK(rx.type == IGMP_V2_REPORT && rx.group == 0xef010203u, "a report for 239.1.2.3");
	CHECK(!rx.tagged && rx.vid == 0, "untagged, no VLAN");
	CHECK(IGMP_PORTBIT(rx.port) == 1u, "which is hardware port bit 0, what igmpd programs");

	/* A tagged frame: the prefix carries the VLAN id too. */
	n = v2_report(f, 1, 0x8100);
	total = odi_igmp_build_rx(buf, 0, 0x00b, 0, f, n);
	CHECK(igmp_parse(buf + ODI_OMCI_NLMSG_HDR, total - ODI_OMCI_NLMSG_HDR, &rx) == IGMP_OK,
	      "a tagged delivery parses");
	CHECK(rx.tagged && rx.vid == 0x00b, "the VLAN id");
	CHECK(buf[ODI_OMCI_NLMSG_HDR + 1] == 0 && buf[ODI_OMCI_NLMSG_HDR + 2] == 0x0b, "prefix bytes 1 and 2");
	odi_igmp_build_rx(buf, 0, 0xabc, 0, f, n);
	CHECK(buf[ODI_OMCI_NLMSG_HDR + 1] == 0x0a && buf[ODI_OMCI_NLMSG_HDR + 2] == 0xbc,
	      "a twelve-bit id: high nibble then low byte");
	odi_igmp_build_rx(buf, 0, 0xfabc, 0, f, n);
	CHECK(buf[ODI_OMCI_NLMSG_HDR + 1] == 0x0a, "bits above the VLAN id are cut");
	CHECK(igmp_parse(buf + ODI_OMCI_NLMSG_HDR, total - ODI_OMCI_NLMSG_HDR, &rx) == IGMP_OK &&
	      !rx.taken, "a copy is not marked taken");

	/* Taken, not copied (the lease's TRAP mode): bit 7 of prefix byte 1,
	 * which igmpd reads as "upstream only if I send it". */
	total = odi_igmp_build_rx(buf, 0, 0x00b, 1, f, n);
	CHECK(ODI_IGMP_RX_TAKEN == IGMP_RX_TAKEN, "the taken bit is the one igmpd reads");
	CHECK(buf[ODI_OMCI_NLMSG_HDR + 1] == 0x80 && buf[ODI_OMCI_NLMSG_HDR + 2] == 0x0b,
	      "taken: bit 7 above the VLAN id");
	CHECK(igmp_parse(buf + ODI_OMCI_NLMSG_HDR, total - ODI_OMCI_NLMSG_HDR, &rx) == IGMP_OK &&
	      rx.taken && rx.vid == 0x00b && rx.port == 1,
	      "igmp_parse reads it as taken, the VLAN and port as before");
	odi_igmp_build_rx(buf, 0, 0xfff, 1, f, n);
	CHECK(buf[ODI_OMCI_NLMSG_HDR + 1] == 0x8f, "the bit does not touch a twelve-bit VLAN id");
}

/* What the hook takes: from the UNI port only, only IGMP, only while the
 * lease holds the copy (or the take). Never a frame from the PON. */
static void test_hook_takes(void)
{
	uint8_t f[128];
	uint32_t n = v2_report(f, 0, 0x8100), port;

	CHECK(odi_igmp_rx_takes(1, 0, f, n), "IGMP from the UNI port, the copy on: taken");
	CHECK(!odi_igmp_rx_takes(0, 0, f, n), "the copy off: left to the net device");
	for (port = 1; port < 8; port++)
		CHECK(!odi_igmp_rx_takes(1, port, f, n), "from any other port, the PON (2) above all: not taken");
	f[23] = 17;
	CHECK(!odi_igmp_rx_takes(1, 0, f, n), "UDP from the UNI: not taken");
}

int main(void)
{
	test_classifier();
	test_delivery_parses();
	test_hook_takes();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_igmp_rx_test: all checks passed\n");
	return 0;
}
