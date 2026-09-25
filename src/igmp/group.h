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

#define IGMP_MAX_GROUPS   128
#define IGMP_MAX_PORTS    16

#define IGMP_PORTBIT(p)   ((uint32_t)1 << ((p) - 1))

/* RFC 2236 defaults, in seconds. Robustness 2, query interval 125, query
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
/* A leave does not remove the port. It shortens that port membership to the
 * last-member interval, so a second listener on the same port still holding
 * the group can answer the group-specific query and keep it. Removing on the
 * leave itself is the "fast leave" behaviour, and it cuts off everyone else on
 * the port. */
#define IGMP_LAST_MEMBER_TTL   (IGMP_ROBUSTNESS * 1)

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
	uint16_t vid;
	uint32_t group;
	uint32_t ports;                         /* members, as a mask */
	uint32_t expires[IGMP_MAX_PORTS];       /* per port, monotonic seconds */
};

struct igmp_db {
	struct igmp_entry g[IGMP_MAX_GROUPS];
	uint32_t router_ports;
	uint32_t router_expires[IGMP_MAX_PORTS];
	uint32_t dropped;                       /* joins refused, table full */
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

/* How many groups are live. For the status line, and for the tests. */
int igmp_db_count(const struct igmp_db *db);

#endif
