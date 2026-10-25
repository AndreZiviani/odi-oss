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

/* The membership interval: robustness * query interval + response time.
 * Setting it shorter than the querier's own interval prunes members that
 * are still watching. */
uint32_t igmp_policy_membership_ttl(const struct igmp_policy *p);

/* How long a member that left stays, with no immediate leave: the
 * last-member time, robustness * last member query interval. Within it the
 * OLT's group-specific query reaches the hosts and a listener that is still
 * there answers, which refreshes the membership. */
uint32_t igmp_policy_last_member_ttl(const struct igmp_policy *p);

/* The ODI_SW_MC_SNOOP_* flags to hold at second `now`: nothing when the OLT
 * does not ask for snooping; the copy alone until `learn_until`, so that
 * every group the host already watches can report once before an
 * unregistered one is a group nobody wants; then the copy and the PON's drop,
 * unless a static range was too wide to name, or the lists were longer than
 * fit (their groups would be dropped as unregistered). `pon_drop` is the
 * opt-in for the drop (igmpd -d): 0 holds the copy alone, always. */
uint32_t igmp_policy_lease(const struct igmp_policy *p, int static_overflow,
			   uint32_t now, uint32_t learn_until, int pon_drop);

#endif
