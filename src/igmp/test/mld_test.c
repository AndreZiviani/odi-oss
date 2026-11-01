/* The IPv6 half of igmpd (mld.c): the MLD parse against frames built here,
 * which groups are snooped (and which never are), the IPv6 lists of the
 * policy file, the lease bits, the table and its refusals, the state file.
 * Pure, with `now` in the test's hand, like group_test.c.
 */
#include "mld.h"
#include "odi_sw_ioctl.h"
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

static void lists(const char *text, struct mld_policy *p)
{
	mld_policy_parse(text, slen(text), p);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void ip6(const char *s, uint8_t out[16])
{
	if (!mld_parse_ipv6(s, slen(s), out))
		out_fmt("(bad test address %s)\n", s);
}

static int same(const uint8_t *a, const uint8_t *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* A redirect payload: prefix (port 1, untagged), Ethernet, IPv6, `hbh`
 * bytes of hop-by-hop header (0: none), the ICMPv6 message with a right
 * checksum. Returns the length. */
static uint32_t frame(uint8_t *b, const char *src, const char *dst, int hbh,
		      const uint8_t *msg, uint32_t mlen)
{
	uint8_t s[16], d[16], *eth = b + 3, *ip = eth + 14;
	uint32_t at = 40;

	for (int i = 0; i < 200; i++)
		b[i] = 0;
	ip6(src, s);
	ip6(dst, d);
	b[0] = 1;
	mld_group_mac(eth, d);
	eth[6] = 0x02;
	eth[11] = 0x09;
	put16(eth + 12, ETH_P_IPV6);
	ip[0] = 0x60;
	put16(ip + 4, (uint16_t)(hbh + mlen));
	ip[6] = hbh ? 0 : 58;
	ip[7] = 1;
	for (int i = 0; i < 16; i++) {
		ip[8 + i] = s[i];
		ip[24 + i] = d[i];
	}
	if (hbh) {
		ip[40] = 58;
		ip[41] = (uint8_t)(hbh / 8 - 1);
		ip[42] = 5;
		ip[43] = 2;
		ip[46] = 1;
		ip[47] = (uint8_t)(hbh - 8);
		at += (uint32_t)hbh;
	}
	for (uint32_t i = 0; i < mlen; i++)
		ip[at + i] = msg[i];
	put16(ip + at + 2, mld_cksum(s, d, ip + at, mlen));
	return 3 + 14 + at + mlen;
}

/* The checksum again, after a test changed a frame frame() built with its
 * hop-by-hop header. */
static void recksum(uint8_t *b, uint32_t n)
{
	uint8_t *ip = b + 3 + 14, *m = ip + 48;

	put16(m + 2, 0);
	put16(m + 2, mld_cksum(ip + 8, ip + 24, m, n - 3 - 14 - 48));
}

static uint32_t v1(uint8_t *b, uint8_t type, const char *group)
{
	uint8_t m[24] = { 0 }, g[16];

	ip6(group, g);
	m[0] = type;
	for (int i = 0; i < 16; i++)
		m[8 + i] = g[i];
	return frame(b, "fe80::2", type == MLD_V1_DONE ? "ff02::2" : group, 8, m, 24);
}

/* A v2 report of records (type, group) with no sources. */
static uint32_t v2(uint8_t *b, int n, const uint8_t *types, const char *const *groups)
{
	uint8_t m[8 + 4 * 20] = { 0 };

	m[0] = MLD_V2_REPORT;
	put16(m + 6, (uint16_t)n);
	for (int k = 0; k < n; k++) {
		uint8_t g[16];

		ip6(groups[k], g);
		m[8 + 20 * k] = types[k];
		for (int i = 0; i < 16; i++)
			m[8 + 20 * k + 4 + i] = g[i];
	}
	return frame(b, "fe80::2", "ff02::16", 8, m, (uint32_t)(8 + 20 * n));
}

static void test_parse(void)
{
	uint8_t b[256], g[16];
	struct mld_rx rx;
	uint32_t n;

	n = v1(b, MLD_V1_REPORT, "ff3e::8000:1");
	ok(mld_parse(b, n, &rx) == MLD_OK, "an MLDv1 report parses");
	ip6("ff3e::8000:1", g);
	ok(rx.type == MLD_V1_REPORT && same(rx.group, g, 16) && rx.port == 1 && !rx.tagged,
	   "its type, group and port");
	ok(rx.router_alert && rx.hop_limit == 1, "the router alert of its hop-by-hop header");
	ok(rx.dst[0] == 0x33 && rx.dst[1] == 0x33 && rx.dst[5] == 0x01, "sent to 33:33 and the group's low bits");

	n = v1(b, MLD_V1_DONE, "ff3e::8000:1");
	ok(mld_parse(b, n, &rx) == MLD_OK && rx.type == MLD_V1_DONE && same(rx.group, g, 16),
	   "a done names the group it leaves");

	{
		static const uint8_t t[2] = { 4, 3 };
		static const char *const gs[2] = { "ff15::1", "ff3e::2" };

		n = v2(b, 2, t, gs);
		ok(mld_parse(b, n, &rx) == MLD_OK && rx.type == MLD_V2_REPORT && rx.nrec == 2,
		   "an MLDv2 report with two records");
		b[n - 1] ^= 1;
		ok(mld_parse(b, n, &rx) == MLD_CKSUM, "a bad checksum is said as one");
		b[n - 1] ^= 1;
		put16(b + 3 + 14 + 48 + 6, 9);
		recksum(b, n);
		ok(mld_parse(b, n, &rx) == MLD_BAD_MLD, "more records than fit are refused");
	}
	{
		uint8_t m[24] = { 0 };

		m[0] = MLD_V1_REPORT;
		m[8] = 0xff;
		m[9] = 0x3e;
		m[23] = 7;
		n = frame(b, "fe80::2", "ff3e::7", 0, m, 24);
		ok(mld_parse(b, n, &rx) == MLD_OK && !rx.router_alert,
		   "without a hop-by-hop header it is still MLD, said without the alert");
		m[0] = 135;
		n = frame(b, "fe80::2", "ff02::1:ff00:7", 8, m, 24);
		ok(mld_parse(b, n, &rx) == MLD_NOT_MLD, "a neighbour solicitation is not MLD");
	}
	n = v1(b, MLD_V1_REPORT, "ff3e::1");
	ok(mld_parse(b, n - 1, &rx) == MLD_SHORT, "a payload past the frame is a truncation");
	b[3 + 12] = 0x08;
	b[3 + 13] = 0x00;
	ok(mld_parse(b, n, &rx) == MLD_NOT_IPV6, "IPv4 is not MLD's");
}

/* Puts `n` tags (TPID and VID each, outermost first) in front of the
 * EtherType of an untagged payload frame() made; no checksum covers it. */
static uint32_t retag(uint8_t *b, uint32_t len, int n, const uint16_t *tpid,
		      const uint16_t *vid)
{
	uint8_t *at = b + 3 + 12;
	uint32_t tail = len - (3 + 12);

	for (uint32_t i = tail; i-- > 0; )
		at[4 * n + i] = at[i];
	for (int t = 0; t < n; t++) {
		put16(at + 4 * t, tpid[t]);
		put16(at + 4 * t + 2, vid[t]);
	}
	return len + 4u * (uint32_t)n;
}

static void ok3(int cond, const char *shape, const char *msg, const char *what)
{
	if (cond) {
		out_fmt("ok    %s, %s: %s\n", shape, msg, what);
	} else {
		out_fmt("FAIL  %s, %s: %s\n", shape, msg, what);
		failures++;
	}
}

/* The kernel hook (odi_igmp_rx.h) admits up to two tags, each 0x8100 or
 * 0x88a8; the parser reads exactly those, as igmp_parse does. */
static void test_tags(void)
{
	static const struct {
		const char *name;
		int n;
		uint16_t tpid[2], vid[2];
	} shape[] = {
		{ "untagged", 0, { 0, 0 }, { 0, 0 } },
		{ "C-tag", 1, { 0x8100, 0 }, { 100, 0 } },
		{ "S-tag", 1, { 0x88a8, 0 }, { 200, 0 } },
		{ "S-tag and C-tag", 2, { 0x88a8, 0x8100 }, { 200, 100 } },
		{ "two C-tags", 2, { 0x8100, 0x8100 }, { 200, 100 } },
	};
	static const uint16_t three_tpid[3] = { 0x88a8, 0x8100, 0x8100 };
	static const uint16_t three_vid[3] = { 300, 200, 100 };
	static const uint8_t rec[1] = { 4 };
	static const char *const recg[1] = { "ff15::1" };
	uint8_t b[256], g[16];
	struct mld_rx rx;
	uint32_t n;

	ip6("ff15::1", g);
	for (unsigned s = 0; s < sizeof shape / sizeof shape[0]; s++) {
		const char *sn = shape[s].name;

		n = v1(b, MLD_V1_REPORT, "ff15::1");
		n = retag(b, n, shape[s].n, shape[s].tpid, shape[s].vid);
		ok3(mld_parse(b, n, &rx) == MLD_OK, sn, "MLDv1", "parses");
		ok3(rx.type == MLD_V1_REPORT && same(rx.group, g, 16) &&
		    rx.tagged == shape[s].n && rx.vid == shape[s].vid[0] &&
		    rx.router_alert, sn, "MLDv1", "type, group, tags, VID and router alert");

		n = v2(b, 1, rec, recg);
		n = retag(b, n, shape[s].n, shape[s].tpid, shape[s].vid);
		ok3(mld_parse(b, n, &rx) == MLD_OK, sn, "MLDv2", "parses");
		ok3(rx.type == MLD_V2_REPORT && rx.nrec == 1 && rx.tagged == shape[s].n &&
		    rx.vid == shape[s].vid[0], sn, "MLDv2", "type, records, tags and VID");
	}

	n = v1(b, MLD_V1_REPORT, "ff15::1");
	n = retag(b, n, 3, three_tpid, three_vid);
	ok(mld_parse(b, n, &rx) == MLD_NOT_IPV6, "three tags are not parsed, as the hook refuses them");

	n = v1(b, MLD_V1_REPORT, "ff15::1");
	n = retag(b, n, 2, shape[3].tpid, shape[3].vid);
	ok(mld_parse(b, 3 + 14 + 6, &rx) == MLD_SHORT, "a tag cut short is short");
}

static void test_text(void)
{
	static const char *const ok_forms[][2] = {
		{ "ff3e::8000:1", "ff3e::8000:1" },
		{ "FF02:0:0:0:0:1:FF00:1234", "ff02::1:ff00:1234" },
		{ "::", "::" },
		{ "::1", "::1" },
		{ "ff02::", "ff02::" },
		{ "1:0:0:2:0:0:0:3", "1:0:0:2::3" },
		{ "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8" },
		{ "ff3e:0:0:0:0:0:0:0", "ff3e::" },
		{ "1:0:2:3:4:5:6:7", "1:0:2:3:4:5:6:7" },
	};
	static const char *const bad[] = { "", ":", ":1", "1:", "1::2::3", "1:2:3:4:5:6:7:8:9",
					   "12345::", "g::1", "1:2", "1:::2", "1.2.3.4" };
	uint8_t a[16];
	char t[40];

	for (unsigned i = 0; i < sizeof ok_forms / sizeof ok_forms[0]; i++) {
		int p = mld_parse_ipv6(ok_forms[i][0], slen(ok_forms[i][0]), a);

		mld_format_ipv6(t, a);
		ok(p && streq(t, ok_forms[i][1]), ok_forms[i][0]);
	}
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
		ok(!mld_parse_ipv6(bad[i], slen(bad[i]), a), "refused as an address");
}

/* The groups that are never snooped: every one of scope 2 or less. They
 * flood, are never refused and never get a row. */
static void test_snoopable(void)
{
	static const char *const never[] = {
		"ff02::1", "ff02::2", "ff02::16", "ff02::1:2", "ff02::fb",
		"ff02::1:ff00:1234", "ff02::1:ffab:cdef", "ff01::1", "ff12::8000:1",
		"ff32::8000:1", "ff00::1", "ff0f::1", "fe80::1", "2001:db8::1", "::",
	};
	static const char *const yes[] = {
		"ff03::1", "ff05::1:3", "ff0e::1", "ff15::1", "ff3e::8000:1",
		"ff3e::1", "ff3e::1:ff00:1", "ff1e::1234",
	};
	uint8_t g[16], mac[6];

	for (unsigned i = 0; i < sizeof never / sizeof never[0]; i++) {
		ip6(never[i], g);
		ok(!mld_group_snoopable(g), never[i]);
	}
	for (unsigned i = 0; i < sizeof yes / sizeof yes[0]; i++) {
		ip6(yes[i], g);
		ok(mld_group_snoopable(g), yes[i]);
	}
	ip6("ff3e::1:ff00:1234", g);
	mld_group_mac(mac, g);
	ok(mac[0] == 0x33 && mac[1] == 0x33 && mac[2] == 0xff && mac[3] == 0x00 &&
	   mac[4] == 0x12 && mac[5] == 0x34, "33:33 and the low 32 bits");
}

static void test_policy(void)
{
	static const char text[] =
		"snooping=1\n"
		"allow=239.0.0.0-239.255.255.255\n"
		"allow6=ff3e:0:0:0:0:0:8000:0-ff3e:0:0:0:0:0:8000:ffff\n"
		"static6=ff15::10-ff15::12\n"
		"allow6=ff3e::9-ff3e::1\n"
		"allow6=nonsense\n";
	struct mld_policy p;
	struct igmp_policy ip;
	uint8_t g[16];

	ok(mld_policy_parse(text, slen(text), &p) == 2, "two IPv6 lines understood");
	ok(p.nallow == 1 && p.nstatic == 1 && !p.overflow,
	   "the backwards range and the nonsense are left out");
	ok(mld_policy_acl_active(&p), "a list with IPv6 rows restricts IPv6");
	ip6("ff3e::8000:42", g);
	ok(mld_policy_allows(&p, g), "inside the dynamic range");
	ip6("ff15::11", g);
	ok(mld_policy_allows(&p, g), "inside the static range");
	ip6("ff3e::8001:0", g);
	ok(!mld_policy_allows(&p, g), "outside both");

	lists("allow=239.0.0.0-239.0.0.9\n", &p);
	ip6("ff3e::1", g);
	ok(!mld_policy_acl_active(&p) && mld_policy_allows(&p, g),
	   "IPv4 rows alone say nothing of IPv6: every IPv6 group is allowed");

	igmp_policy_defaults(&ip);
	ip.snooping = 1;
	mld_policy_parse(text, slen(text), &p);
	ok(mld_policy_lease(&ip, &p, 0, 100, 200, 1) == ODI_SW_MC_SNOOP_MLD,
	   "while learning: the MLD copy alone");
	ok(mld_policy_lease(&ip, &p, 0, 200, 200, 1) ==
	   (ODI_SW_MC_SNOOP_MLD | ODI_SW_MC_SNOOP_PON_DROP6), "then the IPv6 drop");
	ok(mld_policy_lease(&ip, &p, 0, 200, 200, 0) == ODI_SW_MC_SNOOP_MLD,
	   "never the drop without -d");
	ok(mld_policy_lease(&ip, &p, 1, 200, 200, 1) == ODI_SW_MC_SNOOP_MLD,
	   "nor with a static range too wide to name");
	ok(mld_policy_takes(&ip, &p), "IPv6 lists and unauthorized joins discarded: taken");
	ip.unauthorized_join = 1;
	ok(!mld_policy_takes(&ip, &p), "forwarded: left to the switch");
	ip.snooping = 0;
	ok(mld_policy_lease(&ip, &p, 0, 200, 200, 1) == 0, "nothing when not snooping");
}

static struct mld_rx mk(uint8_t type, const char *group)
{
	struct mld_rx rx;

	for (unsigned i = 0; i < sizeof rx; i++)
		((uint8_t *)&rx)[i] = 0;
	rx.type = type;
	rx.port = 1;
	ip6(group, rx.group);
	return rx;
}

static void test_table(void)
{
	static struct mld_db db;
	static struct mld_change ch[16];
	struct igmp_policy ip;
	struct mld_policy p;
	struct mld_rx rx;
	uint8_t g[16];
	int n;

	igmp_policy_defaults(&ip);
	ip.snooping = 1;
	mld_db_init(&db);
	lists("", &p);
	mld_db_set_policy(&db, &ip, &p, ch, 16);

	rx = mk(MLD_V1_REPORT, "ff3e::8000:1");
	n = mld_db_rx(&db, &rx, 10, 0, ch, 16);
	ip6("ff3e::8000:1", g);
	ok(n == 1 && ch[0].kind == IGMP_CHANGE_SET && ch[0].ports == 1 && same(ch[0].group, g, 16),
	   "a join: the group, the UNI a member");
	ok(mld_db_rx(&db, &rx, 20, 0, ch, 16) == 0 && db.joins == 2, "a refresh changes nothing");
	ok(mld_db_tick(&db, 20 + 260, ch, 16) == 1 && ch[0].kind == IGMP_CHANGE_DEL,
	   "lapses one membership interval after its last report");

	rx = mk(MLD_V1_REPORT, "ff3e::8000:1");
	mld_db_rx(&db, &rx, 300, 0, ch, 16);
	rx = mk(MLD_V1_DONE, "ff3e::8000:1");
	ok(mld_db_rx(&db, &rx, 301, 0, ch, 16) == 0, "a done shortens, it does not end");
	ok(mld_db_tick(&db, 303, ch, 16) == 1 && ch[0].kind == IGMP_CHANGE_DEL,
	   "the last-member time later the group goes");

	/* Reserved and link-local groups: never kept, never refused, even
	 * under lists that allow nothing of theirs. */
	lists("allow6=ff3e::8000:0-ff3e::8000:ffff\n", &p);
	mld_db_set_policy(&db, &ip, &p, ch, 16);
	{
		static const char *const rsv[] = { "ff02::1", "ff02::2", "ff02::16", "ff02::1:2",
						   "ff02::1:ff00:1234", "ff12::1" };

		for (unsigned i = 0; i < sizeof rsv / sizeof rsv[0]; i++) {
			rx = mk(MLD_V1_REPORT, rsv[i]);
			ok(mld_db_rx(&db, &rx, 400, 0, ch, 16) == 0 && db.rx_acl_refused == 0 &&
			   db.rx_passable == 1, rsv[i]);
		}
		ok(db.refusals == 0 && db.denied == 0 && mld_db_count(&db) == 0,
		   "none of them was refused or kept");
	}
	rx = mk(MLD_V1_REPORT, "ff3e::9");
	ok(mld_db_rx(&db, &rx, 401, 0, ch, 16) == 0 && db.rx_acl_refused == 1 && db.denied == 1,
	   "a group outside the IPv6 lists is refused");
	ok(db.refusals == 1 && db.ring[0].reason == IGMP_REFUSE_ACL, "into the ring, as acl");
	mld_db_rx(&db, &rx, 402, 0, ch, 16);
	ok(db.refusals == 1 && db.ring[0].count == 2 && db.ring[0].at == 402,
	   "the same refusal again counts in its slot");

	/* The group limit is the UNI's, IPv4 groups included. */
	ip.max_groups = 2;
	lists("", &p);
	mld_db_set_policy(&db, &ip, &p, ch, 16);
	rx = mk(MLD_V1_REPORT, "ff3e::8000:2");
	ok(mld_db_rx(&db, &rx, 500, 1, ch, 16) == 1, "one IPv4 group held, one IPv6: within 2");
	rx = mk(MLD_V1_REPORT, "ff3e::8000:3");
	ok(mld_db_rx(&db, &rx, 500, 1, ch, 16) == 0 && db.refused[IGMP_REFUSE_LIMIT] == 1,
	   "a third group of either family is past the limit");

	/* v2: a record that stops listening, one that joins. */
	ip.max_groups = 0;
	mld_db_set_policy(&db, &ip, &p, ch, 16);
	{
		uint8_t b[256];
		static const uint8_t t[2] = { 3, 2 };
		static const char *const gs[2] = { "ff3e::8000:2", "ff15::5" };

		ok(mld_parse(b, v2(b, 2, t, gs), &rx) == MLD_OK, "a v2 report");
		ip.immediate_leave = 1;
		mld_db_set_policy(&db, &ip, &p, ch, 16);
		n = mld_db_rx(&db, &rx, 600, 0, ch, 16);
		ip6("ff15::5", g);
		ok(n == 2 && ch[0].kind == IGMP_CHANGE_DEL && ch[1].kind == IGMP_CHANGE_SET &&
		   same(ch[1].group, g, 16), "TO_INCLUDE of nothing leaves, IS_EXCLUDE joins");
	}

	/* Static IPv6 groups: fixed, the UNI a member; one too wide is not. */
	lists("static6=ff15::20-ff15::21\nstatic6=ff15::100-ff15::1ff\n", &p);
	n = mld_db_set_policy(&db, &ip, &p, ch, 16);
	ok(n == 3 && ch[0].kind == IGMP_CHANGE_SET && ch[1].kind == IGMP_CHANGE_SET &&
	   ch[2].kind == IGMP_CHANGE_DEL && db.static_overflow,
	   "two fixed groups, a range too wide to name, and ff15::5, now outside the lists, gone");
	ip6("ff15::21", g);
	ok(mld_db_mac_ports(&db, g) == 1, "a fixed group's row names the UNI");
}

static void test_state(void)
{
	static struct mld_db db;
	static struct mld_change ch[8];
	static struct mld_state_view v;
	static char buf[MLD_STATE_MAX];
	struct igmp_policy ip;
	struct mld_policy p;
	struct mld_rx rx;
	uint32_t n;
	uint8_t g[16];

	igmp_policy_defaults(&ip);
	ip.snooping = 1;
	mld_db_init(&db);
	lists("allow6=ff3e::1-ff3e::ff\n", &p);
	mld_db_set_policy(&db, &ip, &p, ch, 8);
	rx = mk(MLD_V1_REPORT, "ff3e::7");
	mld_db_rx(&db, &rx, 5, 0, ch, 8);
	rx = mk(MLD_V1_REPORT, "ff3e::1:0");
	mld_db_rx(&db, &rx, 6, 0, ch, 8);
	n = mld_state_format(&db, buf, sizeof buf);
	ok(n && streq(buf, "joins=1\ndenied=1\ngroups=1\nrefused_acl=1\ngroup=ff3e::7\n"
		      "refused=acl ff3e::1:0 vid=0 at=6 n=1\n"), "the state file");
	ok(mld_state_parse(buf, n, &v) == 6 && v.joins == 1 && v.ngroup == 1 && v.nrefusal == 1,
	   "read back");
	ip6("ff3e::1:0", g);
	ok(same(v.ref[0].group, g, 16) && v.ref[0].at == 6, "the refusal's group and time");
	ok(mld_state_parse(buf, n - 1, &v) == 5 && v.nrefusal == 0, "a cut-off last line is skipped");
}

int main(void)
{
	test_parse();
	test_tags();
	test_text();
	test_snoopable();
	test_policy();
	test_table();
	test_state();
	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
