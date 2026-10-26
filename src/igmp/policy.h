/* What the OLT provisioned for multicast, as omcid writes it (omcid's
 * respond/mcast.c has the file format and the units). Parsing and the
 * questions asked of it are pure: no syscalls, so the tests drive them with
 * text.
 *
 * Unknown keys are ignored and a line that does not parse leaves its
 * default, so a newer omcid writing a key this igmpd does not know cannot
 * stop the snooper.
 */
#ifndef ODI_IGMP_POLICY_H
#define ODI_IGMP_POLICY_H

#include <stdint.h>

#define IGMP_POLICY_PATH      "/var/run/odi-mcast.conf"
#define IGMP_POLICY_MAX_RANGES 32
#define IGMP_POLICY_MAX_BYTES  4096

struct igmp_range {
	uint32_t lo, hi;                /* inclusive, host order */
};

/* The imputed bandwidth of the groups of one dynamic list range (G.988
 * 9.3.27 attribute 7, bytes 18 to 21 of the row), in bytes a second. */
struct igmp_bw_range {
	uint32_t lo, hi;
	uint32_t bytes;
};

struct igmp_policy {
	int      snooping;              /* the OLT asks for it: 0 otherwise */
	uint32_t function;              /* ME 309 attribute 2 as sent */
	int      immediate_leave;
	uint32_t robustness;            /* count */
	uint32_t query_interval;        /* seconds */
	uint32_t query_max_response;    /* seconds */
	uint32_t last_member_query;     /* seconds */
	uint32_t max_groups;            /* 0: no limit */
	int      unauthorized_join;     /* 1: the OLT lets them through */
	/* ME 309 attributes 4 to 6: what the host's IGMP goes upstream with.
	 * Tag control 0 leaves it as the switch treats any frame; 1 adds the
	 * TCI as a tag, 2 replaces the whole TCI of the tag there (adds it to
	 * an untagged frame), 3 replaces only the VID (an untagged frame gets
	 * the whole TCI). Rate: messages a second, 0 no limit. */
	uint32_t us_tag_control;
	uint32_t us_tci;
	uint32_t us_rate;
	/* ME 310 attributes 4 and 5: the most imputed bandwidth the subscriber
	 * may join, bytes a second (0: no limit), and whether a join past it
	 * is refused (1) or only counted (0). */
	uint32_t max_bandwidth;
	int      bw_enforce;
	uint32_t nbw;
	struct igmp_bw_range bw[IGMP_POLICY_MAX_RANGES];
	int      overflow;              /* more ranges than fit: not enforced */
	uint32_t nallow, nstatic;
	struct igmp_range allow[IGMP_POLICY_MAX_RANGES];
	struct igmp_range stat[IGMP_POLICY_MAX_RANGES];
};

/* No policy: not snooping, the RFC 2236 timers. */
void igmp_policy_defaults(struct igmp_policy *p);

/* Parse the file's text into p, which is reset to the defaults first.
 * Returns how many lines it understood. */
int igmp_policy_parse(const char *text, uint32_t len, struct igmp_policy *p);

/* Whether the access control lists restrict anything: at least one range
 * was provisioned and they all fitted. An empty list is no restriction --
 * the OLT is the authority, and an ONU that blocks a stream because a
 * table was never provisioned breaks service that a transparent one
 * would have carried. */
int igmp_policy_acl_active(const struct igmp_policy *p);

/* A group the subscriber may join: no restriction, or inside a dynamic or
 * a static range. */
int igmp_policy_allows(const struct igmp_policy *p, uint32_t group);

/* The imputed bandwidth of a group, bytes a second: that of the first
 * dynamic list range holding it, 0 when none says (G.988: 0 is a group no
 * bandwidth limit applies to). */
uint32_t igmp_policy_imputed(const struct igmp_policy *p, uint32_t group);

/* The membership interval: robustness * query interval + response time.
 * Setting it shorter than the querier's own interval prunes members that
 * are still watching. */
uint32_t igmp_policy_membership_ttl(const struct igmp_policy *p);

/* How long a member that left stays, with no immediate leave: the
 * last-member time, robustness * last member query interval. Within it the
 * OLT's group-specific query reaches the hosts and a listener that is still
 * there answers, which refreshes the membership. */
uint32_t igmp_policy_last_member_ttl(const struct igmp_policy *p);

/* The upstream tag controls G.988 defines (9.3.27 attribute 5). */
#define IGMP_US_TAG_PASS      0
#define IGMP_US_TAG_ADD       1
#define IGMP_US_TAG_REPLACE   2
#define IGMP_US_TAG_VID       3

/* Whether the host's IGMP has to be taken and sent upstream by igmpd rather
 * than forwarded by the switch: the profile asks for a tag (control 1 to 3),
 * a rate, or the discard of joins outside its lists (unauthorized join
 * behaviour 0, the G.988 default, with lists that restrict something). With
 * none of these the switch's own forwarding is already what the profile
 * says, and the copy is left a copy. */
int igmp_policy_takes(const struct igmp_policy *p);

/* The ODI_SW_MC_SNOOP_* flags to hold at second `now`: nothing when the OLT
 * does not ask for snooping; the copy alone until `learn_until`, so that
 * every group the host already watches can report once before an
 * unregistered one is a group nobody wants; then the copy and the PON's drop,
 * unless a static range was too wide to name, or the lists were longer than
 * fit (their groups would be dropped as unregistered). `pon_drop` is the
 * opt-in for the drop (igmpd -d): 0 holds the copy alone, always. The copy
 * is a take (ODI_SW_MC_SNOOP_TRAP) whenever igmp_policy_takes() says so,
 * from the start: it does not wait for the learning interval, since sending
 * the report as the profile says is not a pruning. */
uint32_t igmp_policy_lease(const struct igmp_policy *p, int static_overflow,
			   uint32_t now, uint32_t learn_until, int pon_drop);

#endif
