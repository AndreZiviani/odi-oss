/* MLD snooping: the IPv6 half of igmpd (RFC 2710 MLDv1, RFC 3810 MLDv2).
 *
 * Opt-in (igmpd -6, MLD_SNOOPING): the switch then copies (or takes) the
 * host's MLD as it does its IGMP (odi_switch_mcast.h, ODI_SW_MC_SNOOP_MLD),
 * on the same redirect channel with the same prefix, and this file keeps
 * what those reports imply: one table of IPv6 groups beside the IPv4 one of
 * group.h, run on the same profile (ME 309 is the IGMP/MLD profile: one set
 * of timers, one immediate leave, one group limit, one upstream tag), with
 * the access control lists' IPv6 rows (G.988 9.3.27, row parts 0 and 2)
 * as its own lists. The rows it asks for are keyed on the group's MAC,
 * 33:33 and the low 32 bits of the address, like the IPv4 ones.
 *
 * What is never snooped: a group of scope 2 or less (ff02::/16 and the other
 * link-local scopes: all nodes ff02::1, all routers ff02::2, DHCPv6
 * ff02::1:2, MLDv2 ff02::16, the solicited-node groups of neighbour
 * discovery). RFC 4541 2.1.2 has a snooper flood those, and pruning them is
 * what cut router advertisements and DHCPv6 off in another open
 * implementation's measurements. Such a group gets no state, no row and no
 * refusal, whatever the lists say; the switch keeps them out of the PON's
 * drop (odi_switch_mcast.h, PON_DROP6).
 *
 * Unlike IPv4, a group whose MAC another group shares is snooped: the MAC
 * keeps 32 bits of 128, so ff3e::1 and ff02::1 share 33:33:00:00:00:01, but a
 * row here always names the UNI and the PON, which is where the switch floods
 * an unknown group to anyway, so a row on a reserved group's MAC prunes
 * nothing; and refusing it would leave the group to the drop.
 *
 * Pure, like group.c and frame.c: no syscalls, time in as an argument and
 * changes out as a list, so the tests drive it with synthetic frames.
 */
#ifndef ODI_IGMP_MLD_H
#define ODI_IGMP_MLD_H

#include <stdint.h>
#include "igmpd.h"
#include "group.h"
#include "policy.h"

#define MLD_QUERY           130
#define MLD_V1_REPORT       131
#define MLD_V1_DONE         132
#define MLD_V2_REPORT       143

#define MLD_MAX_GROUPS      128
/* Extension headers walked to the ICMPv6 message: an MLD message has the
 * hop-by-hop one, carrying the router alert. */
#define MLD_EXT_MAX         4

enum mld_parse_rc {
	MLD_OK = 0,
	MLD_SHORT,              /* truncated before a header was complete */
	MLD_NOT_IPV6,           /* some other ethertype */
	MLD_BAD_IP,             /* version or length disagree */
	MLD_NOT_MLD,            /* IPv6, but not an MLD message */
	MLD_BAD_MLD,            /* too short for its type */
	MLD_CKSUM,              /* ICMPv6 checksum wrong */
};

struct mld_rx {
	uint8_t  port;          /* as received: one-based */
	uint16_t vid;           /* 0 when the frame was untagged */
	int      tagged;
	int      taken;         /* the switch took it: upstream only if sent */

	const uint8_t *eth;     /* the Ethernet frame, prefix removed */
	uint32_t eth_len;
	const uint8_t *dst;
	const uint8_t *src;

	uint8_t  sip[16];
	uint8_t  dip[16];
	uint8_t  hop_limit;
	int      router_alert;  /* hop-by-hop option 5 present */

	const uint8_t *body;    /* the ICMPv6 message */
	uint32_t body_len;
	uint8_t  type;
	uint8_t  group[16];     /* the multicast address field; zero for a v2 report */
	uint16_t nrec;          /* address records, v2 report only */
};

/* Parse one redirect payload, prefix included (igmpd.h has the prefix).
 * Everything rx points at points into buf. */
enum mld_parse_rc mld_parse(const uint8_t *buf, uint32_t len, struct mld_rx *rx);
const char *mld_parse_str(enum mld_parse_rc rc);
const char *mld_type_str(uint8_t type);

/* The ICMPv6 checksum (RFC 4443 2.3) of a message between src and dst: 0
 * over a message whose own checksum is right. */
uint16_t mld_cksum(const uint8_t src[16], const uint8_t dst[16], const uint8_t *msg,
		   uint32_t len);

/* A group the snooper may keep state for: a multicast address of scope 3 or
 * wider (the header says why nothing narrower is). */
int mld_group_snoopable(const uint8_t g[16]);

/* 33:33 and the low 32 bits of the address (RFC 2464 7). */
void mld_group_mac(uint8_t mac[6], const uint8_t g[16]);

/* IPv6 text: the forms of RFC 4291 2.2 (one "::", no embedded IPv4) in
 * s[0..n), and the RFC 5952 form out (lower case, the longest run of two or
 * more zero fields as "::"). The parse returns 1 or 0; the format writes at
 * most 40 bytes, NUL included, and returns the length without it. */
int mld_parse_ipv6(const char *s, uint32_t n, uint8_t out[16]);
uint32_t mld_format_ipv6(char *b, const uint8_t a[16]);

/* ---- the IPv6 rows of the access control lists -------------------------- */

/* What omcid writes for them (respond/mcast.c): allow6=A-B, static6=A-B,
 * allow6_overflow=1, each address in the RFC 4291 text form. The rest of
 * the policy file is policy.h's. */
struct mld_range {
	uint8_t lo[16], hi[16];         /* inclusive, network order */
};

struct mld_policy {
	int      overflow;              /* more ranges than fit: not enforced */
	uint32_t nallow, nstatic;
	struct mld_range allow[IGMP_POLICY_MAX_RANGES];
	struct mld_range stat[IGMP_POLICY_MAX_RANGES];
};

/* Parse the IPv6 lines of the policy file's text; p is emptied first.
 * Returns how many lines it understood. */
int mld_policy_parse(const char *text, uint32_t len, struct mld_policy *p);

/* The lists restrict IPv6 groups only once they hold an IPv6 row: an OLT
 * that provisions IPv4 ranges alone has said nothing about IPv6, and a list
 * read as refusing every IPv6 group would break service a transparent ONU
 * carries. */
int mld_policy_acl_active(const struct mld_policy *p);
int mld_policy_allows(const struct mld_policy *p, const uint8_t g[16]);

/* Whether the IPv6 lists alone ask for the reports to be taken: they
 * restrict something and the profile discards unauthorized joins. The
 * other reasons (a tag, a rate) are the profile's, igmp_policy_takes(). */
int mld_policy_takes(const struct igmp_policy *ip, const struct mld_policy *p);

/* The ODI_SW_MC_SNOOP_* flags MLD adds to the lease at second `now`: none
 * while the profile does not ask for snooping; the MLD copy; and the IPv6
 * drop once the learning interval is over, when `pon_drop` asks for drops at
 * all (igmpd -d), unless an IPv6 static range was too wide to name or the
 * IPv6 lists were longer than fit. */
uint32_t mld_policy_lease(const struct igmp_policy *ip, const struct mld_policy *p,
			  int static_overflow, uint32_t now, uint32_t learn_until,
			  int pon_drop);

/* ---- the IPv6 group table ------------------------------------------------ */

struct mld_change {
	uint8_t  kind;          /* enum igmp_change_kind */
	uint16_t vid;
	uint8_t  group[16];     /* zero for IGMP_CHANGE_ROUTER */
	uint32_t ports;
};

struct mld_entry {
	uint8_t  in_use;
	uint8_t  fixed;                         /* a static group */
	uint16_t vid;
	uint8_t  group[16];
	uint32_t ports;
	uint32_t expires[IGMP_MAX_PORTS];
};

struct mld_refusal {
	uint8_t  reason;        /* enum igmp_refusal_reason; 0 an unused slot */
	uint16_t vid;
	uint8_t  group[16];
	uint32_t at;
	uint32_t count;
};

struct mld_db {
	struct mld_entry g[MLD_MAX_GROUPS];
	uint32_t router_ports;
	uint32_t router_expires[IGMP_MAX_PORTS];
	struct igmp_policy prof;                /* the profile's timers and limit */
	struct mld_policy pol;                  /* the IPv6 lists */
	uint32_t joins, denied;
	int      static_overflow;
	/* As group.h keeps them for IPv4: every refusal by reason, and the last
	 * few in order. */
	uint32_t refused[IGMP_REFUSE_REASONS];
	uint32_t refusals;
	struct mld_refusal ring[IGMP_REFUSAL_RING];
	/* The last message mld_db_rx() read, as for IPv4 (uplink.h). */
	uint32_t rx_acl_refused;
	uint32_t rx_passable;
};

void mld_db_init(struct mld_db *db);

/* Feed one accepted message. `others` is how many IPv4 groups the
 * subscriber holds: the profile's group limit is on every group of the UNI,
 * whichever its family. Returns how many changes there were (see group.h,
 * igmp_db_rx(), for what a count above `max` means). */
int mld_db_rx(struct mld_db *db, const struct mld_rx *rx, uint32_t now, uint32_t others,
	      struct mld_change *out, int max);
int mld_db_tick(struct mld_db *db, uint32_t now, struct mld_change *out, int max);

/* Run the table on a new profile and lists: static groups become fixed, a
 * group the lists no longer allow goes. */
int mld_db_set_policy(struct mld_db *db, const struct igmp_policy *ip,
		      const struct mld_policy *p, struct mld_change *out, int max);

/* The members of every live group sharing this group's MAC. */
uint32_t mld_db_mac_ports(const struct mld_db *db, const uint8_t g[16]);

int mld_db_count(const struct mld_db *db);
int mld_db_dynamic_count(const struct mld_db *db);

/* ---- what igmpd tells about it ------------------------------------------- */

/* The IPv6 state file, written beside the IPv4 one while -6 is given and
 * read by `igmpd -s`; omcid does not read it. The keys of state.h, a group
 * written in IPv6 text:
 *
 *   joins=N, denied=N, groups=N, refused_acl=N, refused_limit=N, refused_full=N
 *   group=ff3e::1
 *   refused=acl ff3e::2 vid=0 at=T n=C
 */
#define MLD_STATE_PATH      "/var/run/odi-mcast6.state"
#define MLD_STATE_TMP_PATH  "/var/run/odi-mcast6.state.tmp"
#define MLD_STATE_MAX       (6 * 32 + MLD_MAX_GROUPS * 48 + IGMP_REFUSAL_RING * 96)

uint32_t mld_state_format(const struct mld_db *db, char *buf, uint32_t max);

struct mld_state_view {
	uint32_t joins, denied, groups;
	uint32_t refused[IGMP_REFUSE_REASONS];
	uint32_t ngroup;
	uint8_t  group[MLD_MAX_GROUPS][16];
	uint32_t nrefusal;                      /* oldest first */
	struct mld_refusal ref[IGMP_REFUSAL_RING];
};

int mld_state_parse(const char *text, uint32_t len, struct mld_state_view *v);

#endif
