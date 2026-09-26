/* igmpd -- IGMP/MLD snooping for the RTL9602C, freestanding.
 *
 * The frames do not arrive on a netdev. The switch traps IGMP and MLD control
 * packets to the CPU with rx reason 215, and igmp_drv hands them to whichever
 * userland app registered for packet-redirect uid 4. That is the same
 * mechanism omcid already speaks on uid 1, with a different prefix. That is
 * the stock kernel; on ours nothing delivers uid 4 yet (main.c).
 *
 * This header is the wire format and nothing else. The state machine lives
 * above it.
 */
#ifndef ODI_IGMPD_H
#define ODI_IGMPD_H

#include <stdint.h>

/* The IGMP redirect uid, 4 in both directions: what we register for, and what
 * we address a transmit to. */
#define IGMP_REDIRECT_UID   4
/* The stock IGMP daemon payload limit, and the MTU it registers. The kernel drops
 * anything longer than the registered MTU, so this is a contract, not a
 * preference. */
#define IGMP_MAX_PAYLOAD    1600

/* Receive: three bytes ahead of the Ethernet frame.
 *
 *   [0] source port, ONE-BASED -- the driver sends src_port_num + 1
 *   [1] VLAN id, high nibble
 *   [2] VLAN id, low byte
 *
 * When the frame arrived tagged the driver also re-inserts the 802.1Q tag it
 * stripped, so the id is in two places and they agree. When it arrived
 * untagged both bytes are zero and no tag is present. */
#define IGMP_RX_PREFIX      3

/* Transmit: eight bytes ahead of the frame, native byte order.
 *
 *   u32 portMask   which ports to send on
 *   u32 sid        stream id, used only upstream on GPON
 *
 * A mask naming some ether ports but not the PON port is silently widened by
 * the kernel to every ether port. Only an empty mask, or one that includes the
 * PON port, is passed through as given. */
#define IGMP_TX_PREFIX      8

#define ETH_HDR_LEN         14
#define ETH_P_8021Q         0x8100
#define ETH_P_IP            0x0800
#define ETH_P_IPV6          0x86dd

#define IPPROTO_IGMP        2
#define IPPROTO_ICMPV6      58

/* RFC 2236 and RFC 3376. v3 reports are type 0x22 and carry group records
 * rather than one group address. */
#define IGMP_QUERY          0x11
#define IGMP_V1_REPORT      0x12
#define IGMP_V2_REPORT      0x16
#define IGMP_V2_LEAVE       0x17
#define IGMP_V3_REPORT      0x22

/* Why a frame was not accepted. Kept as an enum rather than a boolean because
 * the dump mode has to say which check failed -- a snooper that silently drops
 * is indistinguishable from one that is not receiving at all. */
enum igmp_parse_rc {
	IGMP_OK = 0,
	IGMP_SHORT,             /* truncated before a header was complete */
	IGMP_NOT_IP,            /* some other ethertype */
	IGMP_IS_MLD,            /* IPv6, which this does not handle yet */
	IGMP_BAD_IP,            /* version, ihl or length disagree */
	IGMP_IP_CKSUM,          /* IPv4 header checksum wrong */
	IGMP_NOT_IGMP,          /* IPv4, but not protocol 2 */
	IGMP_BAD_IGMP,          /* too short for its type */
	IGMP_IGMP_CKSUM,        /* IGMP checksum wrong */
};

struct igmp_rx {
	uint8_t  port;          /* as received: one-based */
	uint16_t vid;           /* 0 when the frame was untagged */
	int      tagged;        /* an 802.1Q tag was present in the frame */

	const uint8_t *eth;     /* the Ethernet frame, prefix removed */
	uint32_t eth_len;
	const uint8_t *dst;     /* 6 bytes */
	const uint8_t *src;     /* 6 bytes */
	uint16_t ethertype;     /* after the tag, when there was one */

	const uint8_t *ip;      /* IPv4 header */
	uint32_t ip_hlen;
	uint32_t sip;
	uint32_t dip;
	uint8_t  ttl;
	int      router_alert;  /* IP option 148 present */

	const uint8_t *body;    /* the IGMP message */
	uint32_t body_len;
	uint8_t  type;
	uint8_t  max_resp;
	uint32_t group;         /* the group address, 0 for a v3 report */
	uint16_t nrec;          /* group records, v3 report only */
};

/* Parse one redirect payload -- prefix included -- into rx.
 *
 * Everything rx points at points into buf, so buf has to outlive it. Returns
 * IGMP_OK or the first check that failed; rx is filled as far as the parse got,
 * which is what lets the dump say "IPv4, bad checksum" rather than "rejected".
 */
enum igmp_parse_rc igmp_parse(const uint8_t *buf, uint32_t len,
			      struct igmp_rx *rx);

const char *igmp_parse_str(enum igmp_parse_rc rc);
const char *igmp_type_str(uint8_t type);

/* The ones complement sum RFC 1071 defines, over an even or odd length. */
uint16_t igmp_cksum(const uint8_t *p, uint32_t len);

#endif
