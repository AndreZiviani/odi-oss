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

/* A profile whose only range is 225/8: every other group is refused. */
static void acl225(struct igmp_db *db)
{
	db->pol.snooping = 1;
	db->pol.nallow = 1;
	db->pol.allow[0].lo = 0xe1000000;
	db->pol.allow[0].hi = 0xe1ffffff;
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


static uint32_t slen(const char *s)
{
	uint32_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* A profile from the text omcid writes, on a fresh table. */
static void policy_from(struct igmp_db *db, const char *text)
{
	static struct igmp_policy p;
	struct igmp_change ch[IGMP_MAX_GROUPS];

	igmp_db_init(db);
	igmp_policy_parse(text, slen(text), &p);
	igmp_db_set_policy(db, &p, ch, IGMP_MAX_GROUPS);
}

static int joined(struct igmp_db *db, uint8_t type, uint16_t vid, uint32_t group,
		  uint32_t now)
{
	struct igmp_change ch[8];
	struct igmp_rx rx = mk(type, 1, vid, group);
	uint32_t before = db->joins;

	igmp_db_rx(db, &rx, now, ch, 8);
	return db->joins != before;
}

/* The rules the lists, the version and the previews add to a join. */
static void test_join_rules(void)
{
	static struct igmp_db db;
	struct igmp_change ch[IGMP_MAX_GROUPS];
	struct igmp_rx rx;
	uint8_t body[64];
	uint32_t g = 0xef090001;                /* 239.9.0.1 */
	int n;

	/* The version: a v3 report under version 2 is above it, and is counted
	 * and handled as before (a set-top box does not fall back to v2 when it
	 * sees no query). */
	policy_from(&db, "snooping=1\nigmp_version=2\n");
	rx = mk(IGMP_V3_REPORT, 1, 0, 0);
	rx.body = body;
	rx.nrec = 1;
	rx.body_len = mk_v3(body, 2, 0xe1020202, 0);
	n = igmp_db_rx(&db, &rx, 10, ch, 8);
	ok(n == 1 && db.joins == 1 && igmp_db_count(&db) == 1 && db.above_version == 1 &&
	   db.refusals == 0 && db.denied == 0,
	   "G7: a v3 report under version 2 is held as before, and counted above the version");
	ok(joined(&db, IGMP_V2_REPORT, 0, 0xe1020203, 11) && db.above_version == 1,
	   "G7: a v2 report under version 2 is not above it");
	rx.body_len = mk_v3(body, 3, 0xe1020202, 0);               /* TO_INCLUDE {}: a leave */
	n = igmp_db_rx(&db, &rx, 12, ch, 8);
	ok(db.above_version == 2 && db.refusals == 0, "G7: a v3 leave is counted too");
	policy_from(&db, "snooping=1\nigmp_version=3\n");
	rx.body_len = mk_v3(body, 2, 0xe1020202, 0);
	igmp_db_rx(&db, &rx, 10, ch, 8);
	ok(db.joins == 1 && db.above_version == 0, "G7: the same v3 report under version 3 is not counted");
	policy_from(&db, "snooping=1\n");
	igmp_db_rx(&db, &rx, 10, ch, 8);
	ok(db.joins == 1 && db.above_version == 0, "G7: and with no version in the profile");

	/* SSM: a row with a source, a v3 record naming other sources. */
	policy_from(&db, "snooping=1\nallow=232.1.0.0-232.1.255.255 s=10.0.0.2\n");
	rx.body_len = mk_v3(body, 1, 0xe8010001, 3);               /* INCLUDE 10.0.0.1 .2 .3 */
	igmp_db_rx(&db, &rx, 10, ch, 8);
	ok(db.joins == 1, "G9: INCLUDE sources with the row's among them: in");
	rx.body_len = mk_v3(body, 1, 0xe8010002, 1);               /* INCLUDE 10.0.0.1 */
	igmp_db_rx(&db, &rx, 11, ch, 8);
	ok(db.joins == 1 && db.refused[IGMP_REFUSE_ACL] == 1, "G9: INCLUDE sources without it: refused");
	rx.body_len = mk_v3(body, 2, 0xe8010003, 2);               /* EXCLUDE 10.0.0.1 .2 */
	igmp_db_rx(&db, &rx, 12, ch, 8);
	ok(db.joins == 1 && db.refused[IGMP_REFUSE_ACL] == 2, "G9: EXCLUDE the row's source: refused");
	rx.body_len = mk_v3(body, 2, 0xe8010004, 1);               /* EXCLUDE 10.0.0.1 */
	igmp_db_rx(&db, &rx, 13, ch, 8);
	ok(db.joins == 2, "G9: EXCLUDE another source: in");
	ok(joined(&db, IGMP_V2_REPORT, 0, 0xe8010005, 14), "G9: a v2 report of a source row: in, as before");

	/* The VLAN, from the profile's own tag. */
	policy_from(&db, "snooping=1\nus_tag_control=1\nus_tci=100\n"
		    "allow=225.1.0.0-225.1.255.255 v=100\nallow=225.2.0.0-225.2.255.255 v=200\n");
	ok(joined(&db, IGMP_V2_REPORT, 0, 0xe1010101, 10), "G9: a group of the row for the join's VLAN: in");
	ok(!joined(&db, IGMP_V2_REPORT, 0, 0xe1020101, 11) && db.refused[IGMP_REFUSE_ACL] == 1,
	   "G9: a group of the row for another VLAN: refused");

	/* Preview: 30 s long, 60 s apart, twice. */
	policy_from(&db, "snooping=1\nimmediate_leave=1\nallow=239.9.0.0-239.9.255.255 p=30,60,2,0\n");
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 100), "G13: the first preview begins");
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 129), "G13: a report inside it is the same preview");
	n = igmp_db_tick(&db, 129, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "G13: one second before its end the group is still there");
	n = igmp_db_tick(&db, 130, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL && ch[0].group == g && igmp_db_count(&db) == 0,
	   "G13: at its end the group goes");
	ok(!joined(&db, IGMP_V2_REPORT, 0, g, 189) && db.refused[IGMP_REFUSE_PREVIEW] == 1,
	   "G13: one second before the repeat time has passed: refused, by preview");
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 190), "G13: at the repeat time the second preview begins");
	igmp_db_tick(&db, 220, ch, 8);
	ok(igmp_db_count(&db) == 0 && !joined(&db, IGMP_V2_REPORT, 0, g, 1000) &&
	   db.refused[IGMP_REFUSE_PREVIEW] == 2, "G13: the count is spent: refused for good");
	ok(joined(&db, IGMP_V2_REPORT, 0, g + 1, 1001), "G13: another group has its own count");
	ok(!joined(&db, IGMP_V2_REPORT, 0, g, 1000 + IGMP_PREVIEW_WINDOW - 1),
	   "G13: a reset time of 0 never resets the count");
	policy_from(&db, "snooping=1\nimmediate_leave=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,24\n");
	joined(&db, IGMP_V2_REPORT, 0, g, 100);
	igmp_db_tick(&db, 130, ch, 8);
	ok(!joined(&db, IGMP_V2_REPORT, 0, g, 200), "G13: a count of 1 is spent");
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 100 + IGMP_PREVIEW_WINDOW),
	   "G13: a reset time of 1 to 24: the count resets after a day");
	policy_from(&db, "snooping=1\nimmediate_leave=1\nallow=239.9.0.0-239.9.255.255 p=30,0,0,0\n");
	joined(&db, IGMP_V2_REPORT, 0, g, 100);
	igmp_db_tick(&db, 130, ch, 8);
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 130), "G13: a repeat count of 0 and no repeat time: unlimited, at once");
	/* A leave ends the preview, and the repeat time counts from it. */
	policy_from(&db, "snooping=1\nimmediate_leave=1\nallow=239.9.0.0-239.9.255.255 p=30,60,0,0\n");
	joined(&db, IGMP_V2_REPORT, 0, g, 100);
	rx = mk(IGMP_V2_LEAVE, 1, 0, g);
	igmp_db_rx(&db, &rx, 110, ch, 8);
	ok(!joined(&db, IGMP_V2_REPORT, 0, g, 169) && joined(&db, IGMP_V2_REPORT, 0, g, 170),
	   "G13: after a leave at 110 the next preview is at 170");
	/* A group that is fully authorized by another row is never a preview. */
	policy_from(&db, "snooping=1\nimmediate_leave=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,0\n"
		    "allow=239.9.0.0-239.9.0.255\n");
	joined(&db, IGMP_V2_REPORT, 0, g, 100);
	n = igmp_db_tick(&db, 200, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "G13: a row with no preview covering the group: full access, no timing");
	/* No preview rows: nothing changes. */
	policy_from(&db, "snooping=1\nallow=239.9.0.0-239.9.255.255\n");
	joined(&db, IGMP_V2_REPORT, 0, g, 100);
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 5000), "G13: a profile with no preview rows: as before");

	/* The allowed preview table. */
	policy_from(&db, "snooping=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,0\n"
		    "grant=239.9.0.1 a=0 u=0 d=2\ngrant=226.5.5.5 a=0 u=0\ngrant=239.8.0.9 a=0 u=77\n");
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 10), "G13: a granted group goes in at full access");
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 129), "G13: still, inside its two minutes");
	igmp_db_tick(&db, 130, ch, 8);
	ok(igmp_db_count(&db) == 1, "G13: and not cut at the preview length");
	ok(joined(&db, IGMP_V2_REPORT, 0, 0xe2050505, 20), "G13: a granted group in no row goes in");
	ok(!joined(&db, IGMP_V2_REPORT, 0, 0xef080009, 21), "G13: a grant for another UNI VLAN is not this join's");
	joined(&db, IGMP_V2_REPORT, 0, g, 131);
	igmp_db_tick(&db, 165, ch, 8);
	ok(igmp_db_count(&db) == 1, "G13: after its two minutes it is a preview (it ends 30 s on, the other group stays)");
	/* A group only the table admits survives the policy being written again. */
	policy_from(&db, "snooping=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,0\ngrant=226.5.5.5 a=0 u=0 d=2\n");
	joined(&db, IGMP_V2_REPORT, 0, 0xe2050505, 10);
	{
		static struct igmp_policy same;
		const char *t = "snooping=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,0\ngrant=226.5.5.5 a=0 u=0 d=2\n";

		igmp_policy_parse(t, slen(t), &same);
		n = igmp_db_set_policy(&db, &same, ch, IGMP_MAX_GROUPS);
	}
	ok(n == 0 && igmp_db_count(&db) == 1,
	   "G13: a group in no list but the allowed preview table is not purged by a rewrite of the policy");
	{
		static struct igmp_policy gone;
		const char *t = "snooping=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,0\n";

		igmp_policy_parse(t, slen(t), &gone);
		n = igmp_db_set_policy(&db, &gone, ch, IGMP_MAX_GROUPS);
	}
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL && igmp_db_count(&db) == 0,
	   "G13: and it goes when the OLT takes the row out");
	/* A rewrite of the table that leaves the row alone does not restart its minutes. */
	policy_from(&db, "snooping=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,0\ngrant=239.9.0.1 a=0 u=0 d=2\n");
	joined(&db, IGMP_V2_REPORT, 0, g, 1000);
	{
		static struct igmp_policy np;
		const char *t = "snooping=1\nallow=239.9.0.0-239.9.255.255 p=30,60,1,0\ngrant=239.9.0.1 a=0 u=0 d=5\n";

		igmp_policy_parse(t, slen(t), &np);
		igmp_db_set_policy(&db, &np, ch, IGMP_MAX_GROUPS);
	}
	ok(joined(&db, IGMP_V2_REPORT, 0, g, 1000 + 299) && db.grant_since[0] == 1000,
	   "G13: the OLT lengthening a row moves its end, not its start");
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
		rx = mk(IGMP_V2_REPORT, 1, 10, 0xe5010000 + (uint32_t)i);
		igmp_db_rx(&db, &rx, 9000, ch, 8);
	}
	ok(igmp_db_count(&db) == IGMP_MAX_GROUPS, "the table fills to its limit");
	ok(db.refused[IGMP_REFUSE_FULL] == 4 && db.denied == 0,
	   "and counts the four it turned away, as table-full and not as profile refusals");
	ok(db.ring[0].reason == IGMP_REFUSE_FULL && db.ring[0].group == 0xe5010000u + IGMP_MAX_GROUPS &&
	   db.ring[3].group == 0xe5010000u + IGMP_MAX_GROUPS + 3 && db.refusals == 4,
	   "each of the four is in the ring, in order");


	/* v3 record types that carry no membership. ALLOW_NEW_SOURCES with no
	 * sources adds nothing, BLOCK_OLD_SOURCES only trims a list, and an
	 * unknown type is not readable: none may create or refresh a port. */
	igmp_db_init(&db);
	rx = mk(IGMP_V3_REPORT, 6, 30, 0);
	rx.body = body;
	rx.nrec = 1;
	rx.body_len = mk_v3(body, 5, 0xe3030303, 0);   /* ALLOW_NEW_SOURCES {} */
	n = igmp_db_rx(&db, &rx, 7100, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "a v3 ALLOW with no sources joins nothing");
	rx.body_len = mk_v3(body, 6, 0xe3030303, 2);   /* BLOCK_OLD_SOURCES */
	n = igmp_db_rx(&db, &rx, 7100, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "a v3 BLOCK joins nothing");
	rx.body_len = mk_v3(body, 9, 0xe3030303, 1);   /* not a record type */
	n = igmp_db_rx(&db, &rx, 7100, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "an unknown v3 record type joins nothing");
	rx.body_len = mk_v3(body, 5, 0xe3030303, 1);   /* ALLOW {one source} */
	n = igmp_db_rx(&db, &rx, 7100, ch, 8);
	ok(n == 1 && ch[0].ports == IGMP_PORTBIT(6), "a v3 ALLOW naming a source is a join");
	rx.body_len = mk_v3(body, 6, 0xe3030303, 1);   /* BLOCK on a live group */
	n = igmp_db_rx(&db, &rx, 7101, ch, 8);
	ok(n == 0, "a BLOCK neither refreshes nor leaves a live group");
	n = igmp_db_tick(&db, 7100 + IGMP_MEMBERSHIP_TTL - 1, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "so the ALLOW deadline still stands");

	/* Groups the snooper must not hold state for. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 1, 10, 0x0a000001);       /* not multicast */
	n = igmp_db_rx(&db, &rx, 7200, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "a report for a unicast address is ignored");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe00000fb);       /* 224.0.0.251, mDNS */
	n = igmp_db_rx(&db, &rx, 7200, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "a link-local group is never restricted");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xef0000fb);       /* 239.0.0.251: same MAC */
	n = igmp_db_rx(&db, &rx, 7200, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 0, "a group on a link-local MAC is not restricted either");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe0000100);       /* 224.0.1.0, first outside */
	n = igmp_db_rx(&db, &rx, 7200, ch, 8);
	ok(n == 1, "the first group past the link-local MACs is snooped");
	ok(igmp_group_snoopable(0xeffffffa), "239.255.255.250 (SSDP) is an ordinary group");
	ok(!igmp_group_snoopable(0xdfffffff) && !igmp_group_snoopable(0xf0000000),
	   "the edges of 224.0.0.0/4 are exact");

	/* One hardware row per MAC. 225.1.1.1 and 226.1.1.1 share
	 * 01:00:5e:01:01:01, so the row holds the union and goes only when
	 * both are gone. The same group on two VLANs is the same row too. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 7300, ch, 8);
	rx = mk(IGMP_V2_REPORT, 3, 10, 0xe2010101);
	igmp_db_rx(&db, &rx, 7300, ch, 8);
	ok(igmp_db_mac_ports(&db, 0xe1010101) == (IGMP_PORTBIT(1) | IGMP_PORTBIT(3)),
	   "aliased groups share one MAC row: the union of their ports");
	ok(igmp_db_mac_ports(&db, 0xe2010101) == igmp_db_mac_ports(&db, 0xe1010101),
	   "and either one names it");
	ok(igmp_db_mac_ports(&db, 0xe1010102) == 0, "another MAC is untouched");
	rx = mk(IGMP_V2_REPORT, 1, 20, 0xe1010101);
	igmp_db_rx(&db, &rx, 7310, ch, 8);
	/* 225.1.1.1 on vlan 10 and 226.1.1.1 lapse together (never refreshed);
	 * the vlan 20 copy keeps the row alive, which writing the lapsing
	 * group's own empty mask would not. */
	n = igmp_db_tick(&db, 7300 + IGMP_MEMBERSHIP_TTL, ch, 8);
	ok(n >= 1 && ch[0].kind == IGMP_CHANGE_DEL, "the vlan 10 entry of 225.1.1.1 lapses");
	ok(igmp_db_mac_ports(&db, 0xe1010101) == IGMP_PORTBIT(1),
	   "the row still carries the vlan 20 member after that DEL");
	n = igmp_db_tick(&db, 7310 + IGMP_MEMBERSHIP_TTL, ch, 8);
	ok(n >= 1 && igmp_db_mac_ports(&db, 0xe1010101) == 0,
	   "and is empty only when the last group on the MAC is gone");

	/* B2: ME 311 attribute 3 counts the joins that were accepted. A join
	 * the profile refuses, or the group limit refuses, is in `denied` and
	 * not in `joins`; a refresh of a held group is an accepted join. */
	igmp_db_init(&db);
	db.pol.nallow = 1;
	db.pol.allow[0].lo = 0xe1000000;
	db.pol.allow[0].hi = 0xe1ffffff;
	db.pol.max_groups = 1;
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xef010101);        /* not in the list */
	igmp_db_rx(&db, &rx, 9000, ch, 8);
	ok(db.joins == 0 && db.denied == 1, "B2: a join outside the list is denied, not counted");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 9001, ch, 8);
	igmp_db_rx(&db, &rx, 9011, ch, 8);
	ok(db.joins == 2 && db.denied == 1, "B2: an accepted join and its refresh are both counted");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010102);        /* listed, over the limit */
	igmp_db_rx(&db, &rx, 9012, ch, 8);
	ok(db.joins == 2 && db.denied == 2, "B2: a join over the group limit is denied, not counted");

	/* B3: the limit is on groups. One group on two VLANs is one of them. */
	igmp_db_init(&db);
	db.pol.max_groups = 2;
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 9100, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, 20, 0xe1010101);
	n = igmp_db_rx(&db, &rx, 9101, ch, 8);
	ok(n == 1 && igmp_db_count(&db) == 2 && igmp_db_dynamic_count(&db) == 1,
	   "B3: a group on two VLANs is two entries and one group");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010102);
	n = igmp_db_rx(&db, &rx, 9102, ch, 8);
	ok(n == 1 && db.denied == 0, "B3: the second group fits a limit of two");
	rx = mk(IGMP_V2_REPORT, 1, 30, 0xe1010101);
	n = igmp_db_rx(&db, &rx, 9103, ch, 8);
	ok(n == 1 && db.denied == 0, "B3: the first group on a third VLAN is not a new group");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010103);
	n = igmp_db_rx(&db, &rx, 9104, ch, 8);
	ok(n == 0 && db.denied == 1 && igmp_db_dynamic_count(&db) == 2,
	   "B3: a third group is refused");

	/* B4: expiry follows the host's own reporting interval. The profile
	 * says 60 s (membership 2 x 60 + 10 = 130) but the OLT really queries
	 * every 150 s, so the host reports every 150 s. */
	igmp_db_init(&db);
	db.pol.query_interval = 60;
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 10000, ch, 8);
	n = igmp_db_tick(&db, 10129, ch, 8);
	ok(n == 0, "B4: the first membership holds the profile figure (nothing seen yet)");
	n = igmp_db_tick(&db, 10130, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL,
	   "B4: and lapses at it, once: the first gap cannot be known");
	igmp_db_rx(&db, &rx, 10150, ch, 8);              /* gap 150 measured */
	n = igmp_db_tick(&db, 10150 + 130 + 10, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1,
	   "B4: after the second report the group outlives the profile's 130 s");
	igmp_db_rx(&db, &rx, 10300, ch, 8);
	n = igmp_db_tick(&db, 10300 + 2 * 150 + 10 - 1, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1,
	   "B4: it holds robustness x 150 + response, one second short");
	n = igmp_db_tick(&db, 10300 + 2 * 150 + 10, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL,
	   "B4: and lapses on that deadline when the host stops");

	/* ... never earlier than the profile, whatever the gap. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 11000, ch, 8);
	igmp_db_rx(&db, &rx, 11030, ch, 8);              /* gap 30, shorter */
	n = igmp_db_tick(&db, 11030 + IGMP_MEMBERSHIP_TTL - 1, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "B4: a short gap never shortens the profile interval");
	/* a repeat inside the response interval is one answer, not a gap, and
	 * does not overwrite the interval already measured */
	igmp_db_init(&db);
	db.pol.query_interval = 60;
	igmp_db_rx(&db, &rx, 11100, ch, 8);
	igmp_db_rx(&db, &rx, 11250, ch, 8);              /* gap 150 */
	igmp_db_rx(&db, &rx, 11252, ch, 8);              /* a retransmission */
	n = igmp_db_tick(&db, 11252 + 2 * 150 + 10 - 1, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "B4: a retransmission is not taken for a gap");

	/* bounded above: robustness 16 on a 600 s gap would be 9610 s */
	igmp_db_init(&db);
	db.pol.robustness = 16;
	db.pol.query_interval = 10;
	igmp_db_rx(&db, &rx, 12000, ch, 8);
	igmp_db_rx(&db, &rx, 12600, ch, 8);
	n = igmp_db_tick(&db, 12600 + IGMP_OBS_TTL_MAX - 1, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "B4: the observed interval holds the cap, one second short");
	n = igmp_db_tick(&db, 12600 + IGMP_OBS_TTL_MAX, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL, "B4: and is capped there");
	/* a host that comes back after an hour has not shown a query interval */
	igmp_db_init(&db);
	igmp_db_rx(&db, &rx, 13000, ch, 8);
	igmp_db_rx(&db, &rx, 13000 + IGMP_OBS_GAP_MAX + 1, ch, 8);
	n = igmp_db_tick(&db, 13000 + IGMP_OBS_GAP_MAX + 1 + IGMP_MEMBERSHIP_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL, "B4: a gap beyond the bound is not learned");

	/* B5: a leave without immediate leave, two set-top boxes on port 1 and
	 * one on port 2. igmpd sends no group-specific query; the OLT's reaches
	 * the other listener through the still programmed row and its report is
	 * a join. Only the leaving port's membership is shortened. */
	igmp_db_init(&db);
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 14000, ch, 8);
	rx = mk(IGMP_V2_REPORT, 2, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 14000, ch, 8);
	rx = mk(IGMP_V2_LEAVE, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 14100, ch, 8);
	n = igmp_db_tick(&db, 14100 + IGMP_LAST_MEMBER_TTL - 1, ch, 8);
	ok(n == 0, "B5: the leaving port keeps the row until its last-member time is up");
	rx = mk(IGMP_V2_REPORT, 1, 10, 0xe1010101);      /* answer to the OLT query */
	igmp_db_rx(&db, &rx, 14101, ch, 8);
	n = igmp_db_tick(&db, 14100 + IGMP_LAST_MEMBER_TTL, ch, 8);
	ok(n == 0, "B5: a report after the leave (another box answering) keeps the port");
	rx = mk(IGMP_V2_LEAVE, 1, 10, 0xe1010101);
	igmp_db_rx(&db, &rx, 14200, ch, 8);
	n = igmp_db_tick(&db, 14200 + IGMP_LAST_MEMBER_TTL, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_SET && ch[0].ports == IGMP_PORTBIT(2),
	   "B5: with no answer the port goes at robustness x last-member time, port 2 stays");

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();

	/* The refusal ring: group.h says why it is eight and why a repeat of
	 * the newest entry is not a new one. */
	igmp_db_init(&db);
	acl225(&db);
	for (int i = 0; i < 3; i++) {
		rx = mk(IGMP_V2_REPORT, 1, 7, 0xef010101);
		igmp_db_rx(&db, &rx, 100 + (uint32_t)i, ch, 8);
	}
	ok(db.refusals == 1 && db.ring[0].count == 3 && db.ring[0].at == 102 &&
	   db.refused[IGMP_REFUSE_ACL] == 3,
	   "the same refusal three times in a row is one entry with a count, three in the total");
	rx = mk(IGMP_V2_REPORT, 1, 8, 0xef010101);
	igmp_db_rx(&db, &rx, 103, ch, 8);
	ok(db.refusals == 2 && db.ring[1].vid == 8,
	   "the same group on another VLAN is its own entry");
	rx = mk(IGMP_V2_REPORT, 1, 7, 0xef010101);
	igmp_db_rx(&db, &rx, 104, ch, 8);
	ok(db.refusals == 3, "an older entry coming back after another one is a new entry");
	igmp_db_init(&db);
	acl225(&db);
	for (int i = 0; i < IGMP_REFUSAL_RING + 3; i++) {
		rx = mk(IGMP_V2_REPORT, 1, 7, 0xef000100 + (uint32_t)i);
		igmp_db_rx(&db, &rx, 200 + (uint32_t)i, ch, 8);
	}
	ok(db.refusals == IGMP_REFUSAL_RING + 3 &&
	   db.refused[IGMP_REFUSE_ACL] == IGMP_REFUSAL_RING + 3,
	   "past the ring size the total keeps counting");
	ok(db.ring[(db.refusals - 1) % IGMP_REFUSAL_RING].group == 0xef000100 + IGMP_REFUSAL_RING + 2 &&
	   db.ring[db.refusals % IGMP_REFUSAL_RING].group == 0xef000103,
	   "the newest is the last written and the oldest kept is the one after it");
	test_join_rules();
	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
