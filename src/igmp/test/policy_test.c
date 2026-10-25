/* The multicast policy: the file omcid writes, and what the group table does
 * once it runs on it. Pure arithmetic on text and calls with `now` in the
 * test's hand, like group_test.c.
 */
#include "group.h"
#include "policy.h"
#include "entry.h"
#include "state.h"
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

static uint32_t slen(const char *s)
{
	uint32_t n = 0;

	while (s[n])
		n++;
	return n;
}

static void parse(const char *text, struct igmp_policy *p)
{
	igmp_policy_parse(text, slen(text), p);
}

static struct igmp_rx mk(uint8_t type, uint8_t port, uint32_t group)
{
	struct igmp_rx rx;

	for (unsigned i = 0; i < sizeof rx; i++)
		((uint8_t *)&rx)[i] = 0;
	rx.type = type;
	rx.port = port;
	rx.group = group;
	return rx;
}

#define G(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static void test_parse(void)
{
	struct igmp_policy p;
	int n;

	igmp_policy_defaults(&p);
	ok(!p.snooping && p.robustness == 2 && p.query_interval == 125 &&
	   p.query_max_response == 10 && p.last_member_query == 1,
	   "defaults: not snooping, the RFC 2236 timers");
	ok(igmp_policy_membership_ttl(&p) == IGMP_MEMBERSHIP_TTL,
	   "the default membership interval is 260 s");

	n = igmp_policy_parse("", 0, &p);
	ok(n == 0 && !p.snooping, "an empty file understands nothing and does not snoop");

	parse("# written by omcid\n"
	      "snooping=1\n"
	      "igmp_function=0\n"
	      "immediate_leave=1\n"
	      "robustness=3\n"
	      "query_interval=60\n"
	      "query_max_response=5\n"
	      "last_member_query=2\n"
	      "unauthorized_join=1\n"
	      "max_groups=8\n"
	      "allow=225.0.0.0-225.255.255.255\n"
	      "allow=239.1.1.1-239.1.1.9\n"
	      "static=226.1.2.3-226.1.2.3\n", &p);
	ok(p.snooping && p.immediate_leave && p.unauthorized_join, "the flags");
	ok(p.robustness == 3 && p.query_interval == 60 && p.query_max_response == 5 &&
	   p.last_member_query == 2 && p.max_groups == 8, "the numbers");
	ok(p.nallow == 2 && p.allow[0].lo == G(225, 0, 0, 0) && p.allow[0].hi == G(225, 255, 255, 255) &&
	   p.allow[1].lo == G(239, 1, 1, 1) && p.allow[1].hi == G(239, 1, 1, 9), "two allow ranges, in order");
	ok(p.nstatic == 1 && p.stat[0].lo == p.stat[0].hi && p.stat[0].lo == G(226, 1, 2, 3),
	   "one static range");
	ok(igmp_policy_membership_ttl(&p) == 3 * 60 + 5, "membership interval: robustness * interval + response");
	ok(igmp_policy_last_member_ttl(&p) == 6, "last-member time: robustness * last member interval");

	/* No line ending, a key this igmpd does not know, junk values. */
	parse("snooping=1\nfuture_key=7\nrobustness=abc\nquery_interval=\nmax_groups=4", &p);
	ok(p.snooping && p.robustness == 2 && p.query_interval == 125 && p.max_groups == 4,
	   "unknown keys and bad values are skipped, the last line needs no newline");
	parse("snooping=2\n", &p);
	ok(!p.snooping, "snooping is exactly 1");
	parse("robustness=0\nquery_interval=0\nlast_member_query=0\n", &p);
	ok(p.robustness == 1 && p.query_interval == 1 && p.last_member_query == 1,
	   "zero timers are raised to 1 rather than expiring members at once");
	parse("robustness=1000\nquery_interval=99999999\n", &p);
	ok(p.robustness == 16 && p.query_interval == 86400, "absurd timers are capped");
	parse("allow=239.0.0.9-239.0.0.1\nallow=nonsense\nallow=1.2.3.4\nstatic=a-b\n", &p);
	ok(p.nallow == 0 && p.nstatic == 0, "an inverted, malformed or single-address range is dropped");

	/* More ranges than fit: flagged, and then not enforced. */
	{
		static char big[IGMP_POLICY_MAX_BYTES];
		uint32_t at = 0;

		for (int i = 0; i < IGMP_POLICY_MAX_RANGES + 2; i++) {
			const char *l = "allow=239.0.0.1-239.0.0.2\n";

			for (uint32_t k = 0; k < slen(l); k++)
				big[at++] = l[k];
		}
		igmp_policy_parse(big, at, &p);
		ok(p.nallow == IGMP_POLICY_MAX_RANGES && p.overflow, "past the cap: kept 32, overflow flagged");
		ok(!igmp_policy_acl_active(&p) && igmp_policy_allows(&p, G(224, 5, 5, 5)),
		   "an overflowed list restricts nothing");
	}
}

static void test_acl(void)
{
	struct igmp_policy p;

	parse("snooping=1\n", &p);
	ok(!igmp_policy_acl_active(&p) && igmp_policy_allows(&p, G(239, 9, 9, 9)),
	   "no list, no restriction: the OLT is the authority");
	parse("snooping=1\nallow=225.0.0.0-225.255.255.255\nstatic=226.1.1.1-226.1.1.1\n", &p);
	ok(igmp_policy_acl_active(&p), "a list restricts");
	ok(igmp_policy_allows(&p, G(225, 0, 0, 0)) && igmp_policy_allows(&p, G(225, 255, 255, 255)),
	   "both ends of an allow range are inside it");
	ok(!igmp_policy_allows(&p, G(224, 255, 255, 255)) && !igmp_policy_allows(&p, G(226, 0, 0, 0)),
	   "one outside either end is not");
	ok(igmp_policy_allows(&p, G(226, 1, 1, 1)), "a static group is allowed");
	ok(!igmp_policy_allows(&p, G(239, 1, 1, 1)), "any other is not");
}

static void test_limits(void)
{
	static struct igmp_db db;
	struct igmp_policy p;
	struct igmp_change ch[IGMP_MAX_GROUPS];
	struct igmp_rx rx;
	int n;

	/* A group limit of 2, a profile ACL. */
	igmp_db_init(&db);
	parse("snooping=1\nmax_groups=2\nallow=239.0.0.0-239.255.255.255\n", &p);
	igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);

	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	n = igmp_db_rx(&db, &rx, 100, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_SET, "an allowed group joins");
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 2));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 3));
	n = igmp_db_rx(&db, &rx, 100, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 2, "the third group is past the limit and gets no state");
	ok(db.denied == 1 && db.joins == 2, "denied and join counters: the refused join is not a join");
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	n = igmp_db_rx(&db, &rx, 110, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 2 && db.denied == 1, "a refresh of a held group is not counted against the limit");
	rx = mk(IGMP_V2_REPORT, 1, G(238, 1, 1, 1));
	n = igmp_db_rx(&db, &rx, 110, ch, 8);
	ok(n == 0 && db.denied == 2, "a group outside the list is refused");

	/* A group frees its place when it lapses. */
	n = igmp_db_tick(&db, 100 + igmp_policy_membership_ttl(&p) + 20, ch, 8);
	ok(n >= 1 && igmp_db_count(&db) == 0, "everything lapsed");
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 3));
	n = igmp_db_rx(&db, &rx, 1000, ch, 8);
	ok(n == 1, "and the place is free");

	/* A narrower list on a live table removes what it no longer allows. */
	parse("snooping=1\nallow=239.1.1.3-239.1.1.3\n", &p);
	igmp_db_init(&db);
	parse("snooping=1\nallow=239.0.0.0-239.255.255.255\n", &p);
	igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 3));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	parse("snooping=1\nallow=239.1.1.3-239.1.1.3\n", &p);
	n = igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL && ch[0].group == G(239, 1, 1, 1) &&
	   igmp_db_count(&db) == 1, "a new list removes the group it no longer allows, and only that");
}

static void test_leave(void)
{
	static struct igmp_db db;
	struct igmp_policy p;
	struct igmp_change ch[8];
	struct igmp_rx rx;
	int n;

	/* Without immediate leave: the last-member time, from the profile. */
	igmp_db_init(&db);
	parse("snooping=1\nrobustness=3\nlast_member_query=2\n", &p);
	igmp_db_set_policy(&db, &p, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	rx = mk(IGMP_V2_LEAVE, 1, G(239, 1, 1, 1));
	n = igmp_db_rx(&db, &rx, 200, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "a leave without immediate leave keeps the group for now");
	n = igmp_db_tick(&db, 200 + 6 - 1, ch, 8);
	ok(n == 0, "one second before robustness * last member interval it is still there");
	n = igmp_db_tick(&db, 200 + 6, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL, "and gone on the deadline");

	/* A report in the window keeps it: the answer to the OLT's query. */
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 300, ch, 8);
	rx = mk(IGMP_V2_LEAVE, 1, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 400, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 402, ch, 8);
	n = igmp_db_tick(&db, 410, ch, 8);
	ok(n == 0 && igmp_db_count(&db) == 1, "a report after the leave holds the membership");

	/* Immediate leave: at once, and only that port. */
	igmp_db_init(&db);
	parse("snooping=1\nimmediate_leave=1\n", &p);
	igmp_db_set_policy(&db, &p, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	rx = mk(IGMP_V2_REPORT, 3, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	rx = mk(IGMP_V2_LEAVE, 1, G(239, 1, 1, 1));
	n = igmp_db_rx(&db, &rx, 101, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_SET && ch[0].ports == IGMP_PORTBIT(3),
	   "immediate leave takes that port out now, the other stays");
	rx = mk(IGMP_V2_LEAVE, 3, G(239, 1, 1, 1));
	n = igmp_db_rx(&db, &rx, 102, ch, 8);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL && igmp_db_count(&db) == 0,
	   "the last one out removes the group");
}

static void test_static(void)
{
	static struct igmp_db db;
	struct igmp_policy p;
	static struct igmp_change ch[IGMP_MAX_GROUPS];
	struct igmp_rx rx;
	int n;

	igmp_db_init(&db);
	parse("snooping=1\nallow=225.0.0.0-225.0.0.255\nstatic=226.1.1.1-226.1.1.3\n", &p);
	n = igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	ok(n == 3 && ch[0].kind == IGMP_CHANGE_SET && ch[0].ports == IGMP_PORTBIT(1),
	   "each static group is a row with the UNI as its member");
	ok(igmp_db_count(&db) == 3 && igmp_db_dynamic_count(&db) == 0 && !db.static_overflow,
	   "three live, none of them counted against the group limit");
	n = igmp_db_tick(&db, 100 + 100000, ch, IGMP_MAX_GROUPS);
	ok(n == 0 && igmp_db_count(&db) == 3, "a static group never expires");
	rx = mk(IGMP_V2_LEAVE, 1, G(226, 1, 1, 2));
	n = igmp_db_rx(&db, &rx, 100, ch, 8);
	igmp_db_tick(&db, 101 + 100, ch, 8);
	ok(igmp_db_count(&db) == 3, "a leave does not take it away");

	/* Setting the same policy again changes nothing. */
	n = igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	ok(n == 0, "the same policy again emits nothing");

	/* A wider range than one row a group can name: not enforced, said so. */
	parse("snooping=1\nstatic=226.1.1.0-226.1.1.255\n", &p);
	n = igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	ok(db.static_overflow, "a static range of 256 groups is too wide to name");
	n = igmp_db_tick(&db, 301, ch, IGMP_MAX_GROUPS);
	ok(igmp_db_count(&db) == 0, "and the groups of the old list are gone");

	/* A static group let go of is held by its joins, and only by them. */
	igmp_db_init(&db);
	parse("snooping=1\nstatic=226.1.1.1-226.1.1.2\n", &p);
	igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	rx = mk(IGMP_V2_REPORT, 1, G(226, 1, 1, 1));
	igmp_db_rx(&db, &rx, 500, ch, 8);
	parse("snooping=1\n", &p);
	igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	n = igmp_db_tick(&db, 501, ch, IGMP_MAX_GROUPS);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_DEL && ch[0].group == G(226, 1, 1, 2) &&
	   igmp_db_count(&db) == 1,
	   "a delisted static group nobody joined lapses at once; the joined one stays");
	n = igmp_db_tick(&db, 500 + IGMP_MEMBERSHIP_TTL - 1, ch, IGMP_MAX_GROUPS);
	ok(n == 0, "the joined one keeps its own deadline, not the delisting time");
	n = igmp_db_tick(&db, 500 + IGMP_MEMBERSHIP_TTL, ch, IGMP_MAX_GROUPS);
	ok(n == 1 && igmp_db_count(&db) == 0, "and lapses on it");

	/* A link-local group is never restricted, so never held. */
	parse("snooping=1\nstatic=224.0.0.1-224.0.0.2\n", &p);
	n = igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	ok(n == 0 && igmp_db_count(&db) == 0, "224.0.0.x is flooded, not given a row");
}

static int starts(const char *buf, uint32_t n, const char *prefix)
{
	for (uint32_t i = 0; prefix[i]; i++)
		if (i >= n || buf[i] != prefix[i])
			return 0;
	return 1;
}

static void test_lease(void)
{
	struct igmp_policy p;

	parse("snooping=0\n", &p);
	ok(igmp_policy_lease(&p, 0, 1000, 0, 1) == 0, "not snooping: no lease");
	parse("snooping=1\n", &p);
	ok(igmp_policy_lease(&p, 0, 1000, 1260, 1) == ODI_SW_MC_SNOOP_COPY,
	   "snooping, still learning: the copy alone, the PON keeps flooding");
	ok(igmp_policy_lease(&p, 0, 1259, 1260, 1) == ODI_SW_MC_SNOOP_COPY, "one second before the end of learning");
	ok(igmp_policy_lease(&p, 0, 1260, 1260, 1) == (ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP),
	   "at the end of learning: the copy and the drop");
	ok(igmp_policy_lease(&p, 1, 5000, 1260, 1) == ODI_SW_MC_SNOOP_COPY,
	   "a static range too wide to name: the drop stays off");
	parse("snooping=1\nallow_overflow=1\n", &p);
	ok(igmp_policy_lease(&p, 0, 5000, 1260, 1) == ODI_SW_MC_SNOOP_COPY,
	   "lists longer than fit: the drop stays off, their static groups could not be named");
	parse("snooping=1\n", &p);
	ok(igmp_policy_lease(&p, 0, 5000, 1260, 0) == ODI_SW_MC_SNOOP_COPY,
	   "drop not opted in: the copy alone, long after learning");
	ok(igmp_policy_lease(&p, 0, 1000, 1260, 0) == ODI_SW_MC_SNOOP_COPY,
	   "drop not opted in, still learning: the copy alone");
	/* The second counter wraps between the start of learning and the end of
	 * it: the deadline is 0x10 and now is 0xfffffff0. */
	ok(igmp_policy_lease(&p, 0, 0xfffffff0u, 0x10u, 1) == ODI_SW_MC_SNOOP_COPY,
	   "before a deadline that lies past the wrap: still learning");
	ok(igmp_policy_lease(&p, 0, 0x10u, 0x10u, 1) == (ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP),
	   "and on it, after the wrap: the drop");
}

static void test_state(void)
{
	static struct igmp_db db;
	struct igmp_policy p;
	static struct igmp_change ch[IGMP_MAX_GROUPS];
	static char buf[IGMP_STATE_MAX];
	struct igmp_rx rx;
	uint32_t n;
	static const char want[] =
		"joins=2\ndenied=1\ngroups=2\ngroup=239.1.1.1\ngroup=239.1.1.2\n";

	igmp_db_init(&db);
	parse("snooping=1\nmax_groups=2\nallow=239.0.0.0-239.255.255.255\nstatic=226.1.1.1-226.1.1.1\n", &p);
	igmp_db_set_policy(&db, &p, ch, IGMP_MAX_GROUPS);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 1));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 2));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	rx = mk(IGMP_V2_REPORT, 1, G(239, 1, 1, 3));
	igmp_db_rx(&db, &rx, 100, ch, 8);
	n = igmp_state_format(&db, buf, sizeof buf);
	ok(n == sizeof want - 1 && starts(buf, n, want), "the state file: counters, then the joined groups");
	ok(igmp_state_format(&db, buf, 10) == 0, "a buffer too small is reported, not truncated");
}

int main(void)
{
	test_lease();
	test_state();
	test_parse();
	test_acl();
	test_limits();
	test_leave();
	test_static();
	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
