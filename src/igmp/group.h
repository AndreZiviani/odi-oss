/* Group and source state: who has asked for what, and until when.
 *
 * This is the half the vendor spent 8,224 of its 27,900 lines on, and the half
 * where being subtly wrong forwards multicast to the wrong port rather than
 * failing visibly. So it is kept free of syscalls and of hardware: time comes
 * in as an argument and changes go out as a list, which makes the whole of it
 * testable on assembled input with time driven by the test.
 *
 * Scope is IGMPv2 with the v3 reports RFC 4541 says a snooper may reduce to
 * joins and leaves. No source filtering: an EXCLUDE with sources is treated as
 * a join for the group, which over-forwards rather than under-forwards, and
 * that is the right direction for a device that is not the router.
 *
 * Ports are the driver numbering: the redirect prefix carries src_port_num + 1,
 * so port 1 is physical port 0 and the mask bit is 1 << (port - 1). That is
 * also what the kernel expects back in tx_portmask, which is why the conversion
 * lives in one macro rather than at each end.
 */
#ifndef ODI_IGMP_GROUP_H
#define ODI_IGMP_GROUP_H

#include <stdint.h>
#include "igmpd.h"
#include "policy.h"

#define IGMP_MAX_GROUPS   128
#define IGMP_MAX_PORTS    16

#define IGMP_PORTBIT(p)   ((uint32_t)1 << ((p) - 1))

/* RFC 2236 defaults, in seconds. A policy from the OLT (policy.h) replaces
 * them: the table runs on the policy it was given, and these are what an
 * empty one says. Robustness 2, query interval 125, query
 * response interval 10.
 *
 * The membership interval is the one that matters: a member that stops
 * answering is forgotten after it, and setting it shorter than the querier
 * interval prunes ports that are still watching. 260 is RV * QI + QRI. */
#define IGMP_ROBUSTNESS        2
#define IGMP_QUERY_INTERVAL    125
#define IGMP_QUERY_RESPONSE    10
#define IGMP_MEMBERSHIP_TTL    (IGMP_ROBUSTNESS * IGMP_QUERY_INTERVAL + \
				IGMP_QUERY_RESPONSE)
/* A router port is forgotten a little sooner than a member, so a querier that
 * has gone away stops attracting traffic before the memberships it created
 * expire. RV * QI + QRI / 2. */
#define IGMP_ROUTER_TTL        (IGMP_ROBUSTNESS * IGMP_QUERY_INTERVAL + \
				IGMP_QUERY_RESPONSE / 2)
/* A leave does not remove the port, unless the profile asks for an
 * immediate leave. It shortens that port membership to the last-member time,
 * so a second listener behind the same port still holding the group can
 * answer the OLT's group-specific query, which reaches it as any multicast
 * to a registered group does, and keep it: its answer is a report like any
 * other. Removing on the leave itself is the "fast leave" behaviour, and it
 * cuts off everyone else on the port. */
#define IGMP_LAST_MEMBER_TTL   (IGMP_ROBUSTNESS * 1)

/* The membership interval the profile gives assumes the OLT queries at the
 * profile's query interval, and nothing here sees the OLT's queries (the copy
 * rule matches UNI ingress only). What igmpd does see is the host answering
 * them: one report per query. So a group port also remembers the gap between
 * its own reports and holds the membership for robustness of those gaps plus
 * the response interval, when that is longer than the profile's figure.
 *
 * Reports closer together than the profile's query response interval are
 * retransmissions of one answer (a host repeats an unsolicited report), not
 * two queries, and are not a gap. A gap above IGMP_OBS_GAP_MAX is a host that
 * stopped and came back, not a query interval, and is not learned. The
 * result is bounded by IGMP_OBS_TTL_MAX, so a host reporting rarely cannot
 * pin a group for hours. The profile's figure is never undercut. The history
 * outlives the membership on purpose: with a profile shorter than the real
 * interval the first membership lapses before its second report, and without
 * the history that gap could never be measured. */
#define IGMP_OBS_GAP_MAX       600
#define IGMP_OBS_TTL_MAX       1800
#define IGMP_HIST_MAX          IGMP_MAX_GROUPS

/* The widest static range igmpd turns into one row a group (the hardware
 * cannot say "this range", only name groups). A wider one stays unenforced
 * and keeps the PON's drop of unregistered multicast off, so the stream the
 * range promises still gets through. */
#define IGMP_STATIC_RANGE_MAX  16

enum igmp_change_kind {
	IGMP_CHANGE_SET = 0,    /* this group now has exactly this port mask */
	IGMP_CHANGE_DEL,        /* the last member went; remove the entry */
	IGMP_CHANGE_ROUTER,     /* the router port mask changed */
};

struct igmp_change {
	uint8_t  kind;
	uint16_t vid;
	uint32_t group;         /* 0 for IGMP_CHANGE_ROUTER */
	uint32_t ports;         /* the new mask, after the change */
};

struct igmp_entry {
	uint8_t  in_use;
	uint8_t  fixed;                         /* a static group: held without a join */
	uint16_t vid;
	uint32_t group;
	uint32_t ports;                         /* members, as a mask */
	uint32_t expires[IGMP_MAX_PORTS];       /* per port, monotonic seconds */
};

/* When one port last reported one group, and the gap before that (0: not
 * measured). Keyed like a membership, kept after it lapses. */
struct igmp_hist {
	uint8_t  used;
	uint8_t  port;
	uint16_t vid;
	uint32_t group;
	uint32_t last;
	uint32_t gap;
};

/* Why a join got no state. The numbers are the state file's order. */
enum igmp_refusal_reason {
	IGMP_REFUSE_ACL = 1,    /* outside the OLT's access control lists */
	IGMP_REFUSE_LIMIT,      /* past the group limit of ME 310 */
	IGMP_REFUSE_FULL,       /* the table has no free entry */
	IGMP_REFUSE_REASONS     /* one past the last, for array sizes */
};

/* The last few refusals, for whoever has to answer "why does this channel not
 * play". Eight, fixed: the ring goes into the state file, which omcid reads in
 * a 4096-byte buffer next to up to 128 group lines, so it cannot be allowed to
 * grow; and what a support question needs is the group the set-top box keeps
 * asking for, which it re-reports every query interval, so a handful is as
 * good as a long log. A refusal equal to the newest entry (same reason, VLAN
 * and group: a host repeating an unsolicited report) bumps its count and time
 * instead of taking a slot. */
#define IGMP_REFUSAL_RING   8

struct igmp_refusal {
	uint8_t  reason;        /* enum igmp_refusal_reason; 0 is an unused slot */
	uint16_t vid;
	uint32_t group;
	uint32_t at;            /* monotonic seconds of the latest one */
	uint32_t count;         /* how many times in a row */
};

struct igmp_db {
	struct igmp_entry g[IGMP_MAX_GROUPS];
	struct igmp_hist hist[IGMP_HIST_MAX];
	uint32_t router_ports;
	uint32_t router_expires[IGMP_MAX_PORTS];
	struct igmp_policy pol;                 /* what the OLT provisioned */
	uint32_t joins;                         /* join messages accepted (ME 311 attr 3) */
	uint32_t denied;                        /* joins refused: profile or limit */
	int      static_overflow;               /* a static range too wide to name */
	/* Every refusal since the daemon started (or the policy last said "not
	 * snooping"), by reason, and the last few in order. A table too small
	 * is a number somebody has to be able to read: a snooper that quietly
	 * stops learning looks exactly like one that is working. */
	uint32_t refused[IGMP_REFUSE_REASONS];
	uint32_t refusals;                      /* ring entries ever written */
	struct igmp_refusal ring[IGMP_REFUSAL_RING];
};

void igmp_db_init(struct igmp_db *db);

/* Feed one accepted frame. Returns how many changes were written to out, which
 * is never more than max; anything past that is lost, so max has to be at
 * least 2 -- a single report can both create a group and make its port a
 * router port. */
int igmp_db_rx(struct igmp_db *db, const struct igmp_rx *rx, uint32_t now,
	       struct igmp_change *out, int max);

/* Expire whatever is due at `now`. Call it at least once a second: nothing
 * else moves time forward. */
int igmp_db_tick(struct igmp_db *db, uint32_t now,
		 struct igmp_change *out, int max);

/* Whether the snooper may hold state for this group at all: a multicast group
 * that is not link-local and does not alias a link-local MAC. See group.c. */
int igmp_group_snoopable(uint32_t group);

/* The union of the member ports of every live group sharing this group's L2
 * MAC (low 23 bits), whatever the VID. What the switch entry must hold. */
uint32_t igmp_db_mac_ports(const struct igmp_db *db, uint32_t group);

/* How many groups are live. For the status line, and for the tests. */
int igmp_db_count(const struct igmp_db *db);

/* How many distinct groups a subscriber joined (one group on two VLANs is one):
 * the static groups are not counted against the profile's limit. */
int igmp_db_dynamic_count(const struct igmp_db *db);

/* Run the table on a new policy. The static groups become fixed entries
 * (the UNI port a member, never expiring); a group the access control lists
 * no longer allow is removed. Returns how many changes were written to out;
 * the caller needs room for IGMP_MAX_GROUPS of them. */
int igmp_db_set_policy(struct igmp_db *db, const struct igmp_policy *pol,
		       struct igmp_change *out, int max);

#endif
