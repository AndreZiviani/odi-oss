/* The group table, with time driven by the test.
 *
 * group.c takes `now` as an argument and touches no syscall, so a membership
 * that should lapse after 260 seconds is checked in two calls rather than in
 * 260 seconds. Every timer here is checked on both sides of its deadline --
 * one second before and one second after -- because an off-by-one in an expiry
 * is invisible in any test that only looks well after it.
 */
#include "group.h"
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

static struct igmp_rx mk(uint8_t type, uint8_t port, uint16_t vid,
			 uint32_t group)
{
	struct igmp_rx rx;

	for (unsigned i = 0; i < sizeof rx; i++)
		((uint8_t *)&rx)[i] = 0;
	rx.type = type;
	rx.port = port;
	rx.vid = vid;
	rx.group = group;
	return rx;
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

/* A v3 report body holding one group record. `srcs` is the source count the
 * record claims and carries. */
static uint32_t mk_v3(uint8_t *body, uint8_t rectype, uint32_t group,
		      uint16_t srcs)
{
	uint32_t len = 8 + 8 + (uint32_t)srcs * 4;
	uint8_t *r = body + 8;

	for (uint32_t i = 0; i < len; i++)
		body[i] = 0;
	body[0] = IGMP_V3_REPORT;
	put16(body + 6, 1);             /* one group record */
	r[0] = rectype;
	r[1] = 0;                       /* no auxiliary data */
	put16(r + 2, srcs);
	put32(r + 4, group);
	for (uint16_t i = 0; i < srcs; i++)
		put32(r + 8 + i * 4, 0x0a000001 + i);
	return len;
}

int main(void)
{
	static struct igmp_db db;
	struct igmp_change ch[8];
	struct igmp_rx rx;
	uint8_t body[64];
	int n;

	igmp_db_init(&db);

	/* A report creates the group and names the port. */
	rx = mk(IGMP_V2_REPORT, 2, 100, 0xe1010101);
	n = igmp_db_rx(&db, &rx, 1000, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_SET, "a report emits one change");
	ok(ch[0].ports == IGMP_PORTBIT(2), "port 2 is bit 1, not bit 2");
	ok(ch[0].vid == 100 && ch[0].group == 0xe1010101, "the change names the group");
	ok(igmp_db_count(&db) == 1, "one group is live");

	/* The same report again is a refresh, and the switch cannot see a
	 * refresh. Re-emitting would be a hardware write per report. */
	n = igmp_db_rx(&db, &rx, 1010, ch, 8);
	ok(n == 0, "a repeated report emits nothing");

	/* A second port joins the same group. */
	rx = mk(IGMP_V2_REPORT, 3, 100, 0xe1010101);
	n = igmp_db_rx(&db, &rx, 1020, ch, 8);
	ok(n == 1 && ch[0].ports == (IGMP_PORTBIT(2) | IGMP_PORTBIT(3)),
	   "a second port is added to the mask");
	ok(igmp_db_count(&db) == 1, "still one group");

	/* The same group on another vlan is a different entry: the switch
	 * forwards per vlan and merging them would leak traffic across. */
	rx = mk(IGMP_V2_REPORT, 4, 200, 0xe1010101);
	n = igmp_db_rx(&db, &rx, 1030, ch, 8);
	ok(n == 1 && ch[0].vid == 200 && ch[0].ports == IGMP_PORTBIT(4),
	   "the same group on another vlan is a separate entry");
	ok(igmp_db_count(&db) == 2, "two groups now");

	/* Expiry, on both sides of the deadline. Port 2 was refreshed at 1010,
	 * port 3 joined at 1020. */
	n = igmp_db_tick(&db, 1010 + IGMP_MEMBERSHIP_TTL - 1, ch, 8);
	ok(n == 0, "nothing expires one second early");
	n = igmp_db_tick(&db, 1010 + IGMP_MEMBERSHIP_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_SET &&
	   ch[0].ports == IGMP_PORTBIT(3),
	   "port 2 lapses exactly on its deadline, port 3 survives");
	n = igmp_db_tick(&db, 1020 + IGMP_MEMBERSHIP_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL,
	   "the last member removes the entry");
	/* The vlan 200 entry joined ten seconds later and lapses ten seconds
	 * later. Each membership carries its own deadline; a single table-wide
	 * one would have taken this entry with the other. */
	ok(igmp_db_count(&db) == 1, "the other vlan is untouched");
	n = igmp_db_tick(&db, 1030 + IGMP_MEMBERSHIP_TTL, ch, 8);
	ok(n == 1 && ch[0].vid == 200 && ch[0].kind == IGMP_CHANGE_DEL,
	   "and lapses on its own deadline");
	ok(igmp_db_count(&db) == 0, "the table is empty again");

	/* A leave shortens the membership, it does not end it. Anything else
	 * cuts off a second listener on the same port. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 5, 10, 0xe2020202);
	igmp_db_rx(&db, &rx, 2000, ch, 8);
	rx = mk(IGMP_V2_LEAVE, 5, 10, 0xe2020202);
	n = igmp_db_rx(&db, &rx, 2000, ch, 8);
	ok(n == 0, "a leave emits nothing by itself");
	ok(igmp_db_count(&db) == 1, "and the group is still there");
	n = igmp_db_tick(&db, 2000 + IGMP_LAST_MEMBER_TTL - 1, ch, 8);
	ok(n == 0, "the port survives to the last-member deadline");
	n = igmp_db_tick(&db, 2000 + IGMP_LAST_MEMBER_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL, "and goes on it");

	/* A report inside the last-member window keeps the port: this is the
	 * other listener answering, and it is the whole reason a leave does not
	 * remove immediately. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 5, 10, 0xe2020202);
	igmp_db_rx(&db, &rx, 3000, ch, 8);
	rx = mk(IGMP_V2_LEAVE, 5, 10, 0xe2020202);
	igmp_db_rx(&db, &rx, 3000, ch, 8);
	rx = mk(IGMP_V2_REPORT, 5, 10, 0xe2020202);
	igmp_db_rx(&db, &rx, 3001, ch, 8);
	n = igmp_db_tick(&db, 3000 + IGMP_LAST_MEMBER_TTL, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1,
	   "a report inside the last-member window rescues the port");

	/* A leave must never EXTEND a membership that was already closer to
	 * lapsing than the last-member interval. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 5, 10, 0xe2020202);
	igmp_db_rx(&db, &rx, 4000, ch, 8);
	rx = mk(IGMP_V2_LEAVE, 5, 10, 0xe2020202);
	igmp_db_rx(&db, &rx, 4000 + IGMP_MEMBERSHIP_TTL - 1, ch, 8);
	n = igmp_db_tick(&db, 4000 + IGMP_MEMBERSHIP_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL,
	   "a late leave does not extend the membership it shortens");

	/* Router ports. Any query, general or group-specific. */
	igmp_db_init(&db);
	rx = mk(IGMP_QUERY, 1, 10, 0);
	n = igmp_db_rx(&db, &rx, 5000, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_ROUTER &&
	   ch[0].ports == IGMP_PORTBIT(1), "a query makes its port a router port");
	n = igmp_db_rx(&db, &rx, 5010, ch, 8);
	ok(n == 0, "a second query on the same port emits nothing");
	n = igmp_db_tick(&db, 5010 + IGMP_ROUTER_TTL - 1, ch, 8);
	ok(n == 0, "the router port survives to its deadline");
	n = igmp_db_tick(&db, 5010 + IGMP_ROUTER_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_ROUTER && ch[0].ports == 0,
	   "and lapses on it");
	/* Sooner than a membership, so a querier that goes away stops
	 * attracting traffic before the memberships it created expire. */
	ok(IGMP_ROUTER_TTL < IGMP_MEMBERSHIP_TTL,
	   "a router port is forgotten sooner than a member");

	/* v3, reduced to joins and leaves the way RFC 4541 allows. */
	igmp_db_init(&db);
	rx = mk(IGMP_V3_REPORT, 6, 30, 0);
	rx.body = body;
	rx.nrec = 1;

	rx.body_len = mk_v3(body, 4, 0xe3030303, 0);   /* CHANGE_TO_EXCLUDE {} */
	n = igmp_db_rx(&db, &rx, 6000, ch, 8);
	ok(n == 1 && ch[0].ports == IGMP_PORTBIT(6),
	   "a v3 EXCLUDE with no sources is a join");

	rx.body_len = mk_v3(body, 3, 0xe3030303, 0);   /* CHANGE_TO_INCLUDE {} */
	igmp_db_rx(&db, &rx, 6000, ch, 8);
	n = igmp_db_tick(&db, 6000 + IGMP_LAST_MEMBER_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL,
	   "a v3 INCLUDE with no sources is a leave");

	/* An INCLUDE that names sources is somebody asking for traffic, not
	 * somebody leaving. Getting this backwards cuts off a listener. */
	igmp_db_init(&db);
	rx.body_len = mk_v3(body, 1, 0xe3030303, 2);   /* MODE_IS_INCLUDE, 2 */
	n = igmp_db_rx(&db, &rx, 7000, ch, 8);
	ok(n == 1 && ch[0].ports == IGMP_PORTBIT(6),
	   "a v3 INCLUDE naming sources is a join, not a leave");

	/* Ports the driver cannot have sent. Ignored rather than shifted into
	 * some other port bit: 1 << (0 - 1) is not a port, it is undefined. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 0, 10, 0xe4040404);
	n = igmp_db_rx(&db, &rx, 8000, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "port 0 is ignored");
	rx = mk(IGMP_V2_REPORT, IGMP_MAX_PORTS + 1, 10, 0xe4040404);
	n = igmp_db_rx(&db, &rx, 8000, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "a port past the table is ignored");

	/* A full table counts what it refused. A snooper that quietly stops
	 * learning looks exactly like one that is working. */
	igmp_db_init(&db);
	for (int i = 0; i < IGMP_MAX_GROUPS + 4; i++) {
		rx = mk(IGMP_V2_REPORT, 1, 10, 0xe5000000 + (uint32_t)i);
		igmp_db_rx(&db, &rx, 9000, ch, 8);
	}
	ok(igmp_db_count(&db) == IGMP_MAX_GROUPS, "the table fills to its limit");
	ok(db.dropped == 4, "and counts the four it turned away");

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
