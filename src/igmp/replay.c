/* igmpd -r FILE: run the daemon on a transcript instead of the switch.
 *
 * The daemon's decisions (main.c's igmpd_tick and igmpd_frame, over group.c
 * and policy.c) are fed events with a clock the transcript sets, and print
 * what they decide: the policy line, every frame as the dry run describes it,
 * every row it would write or remove, the lease it would take, the router
 * ports, and, while the policy has the reports taken, what it would send up
 * and with which tag ("up:" lines; the route and the send need the kernel, so
 * tag control 0 says only that the switch's row would tag it). Nothing opens
 * /dev/odi_sw or the redirect device, and the output depends on the
 * transcript alone, so a golden file can say what igmpd does.
 *
 * Format: one event a line, `#` starts a comment, blank lines are skipped.
 *
 *   T tick                   the clock reaches second T and nothing else happens
 *   T rx HEX                 one redirect payload as the kernel delivers it:
 *                            port (one-based), VLAN id high nibble (bit 7 of
 *                            that byte: taken, not copied) and low byte, then
 *                            the Ethernet frame (the 802.1Q tag left in it)
 *   T v2join   PORT VID SRC GROUP     a v2 membership report
 *   T v2leave  PORT VID SRC GROUP     a v2 leave (sent to 224.0.0.2)
 *   T v3join   PORT VID SRC GROUP     a v3 report, one IS_EXCLUDE record, no sources
 *   T v3leave  PORT VID SRC GROUP     a v3 report, one TO_INCLUDE record, no sources
 *   T query    PORT VID SRC           a general query (224.0.0.1)
 *                                     PORT is the ONE-BASED port, VID 0 is untagged,
 *                                     SRC and GROUP are dotted quads
 *   T policy                 the lines up to a line `end` become the policy file
 *   T policy-file PATH       the policy file is this file's content now
 *   T nopolicy               the policy file is gone
 *   T state                  print the state file igmpd would write
 *   T status                 print what `igmpd -s` would, from that file and
 *                            the policy in force
 *   T ctr GROUP BYTES        the counter of GROUP's MAC has counted BYTES (the
 *                            low 32 bits, wrapping) since the transcript began;
 *                            the daemon reads it on its next second, as it
 *                            reads the switch's (meter.h)
 *   T meter                  print the meter file igmpd would write
 *
 * T is whole seconds on the synthetic clock, never backwards. Before every
 * event except the three policy ones the daemon runs its once-a-second step if
 * the clock moved (the first event always runs it), exactly as its loop does
 * before it handles a frame: an expiry that falls due at T lands before a frame
 * at T. The policy is read by that step, so a policy event is seen from the next
 * second a tick, rx or state event moves the clock to; put `T+1 tick` after it.
 * The transcript ends with the state file.
 *
 * Output lines of the transcript itself start with "> ", the daemon's follow
 * indented as it prints them. A malformed line stops the run with status 2.
 */
#include "sys.h"
#include "io.h"
#include "igmpd.h"
#include "group.h"
#include "policy.h"
#include "entry.h"
#include "replay.h"

#define MAX_TRANSCRIPT  (96 * 1024)
#define MAX_WORDS       8

static char text[MAX_TRANSCRIPT];
static char pol_buf[IGMP_POLICY_MAX_BYTES];
static struct igmp_db db;

/* The counters a transcript set, by group MAC. */
#define MAX_CTRS 32
static struct { uint32_t mac, bytes; } ctrs[MAX_CTRS];
static uint32_t nctrs;

uint32_t igmp_replay_counter(uint32_t group)
{
	for (uint32_t i = 0; i < nctrs; i++)
		if (ctrs[i].mac == (group & 0x7fffffu))
			return ctrs[i].bytes;
	return 0;
}

static uint32_t read_all(long fd, char *buf, uint32_t max)
{
	uint32_t n = 0;

	while (n < max) {
		long r = sys_read((int)fd, buf + n, max - n);

		if (r <= 0)
			break;
		n += (uint32_t)r;
	}
	return n;
}

static int fail(uint32_t line, const char *why)
{
	out_fmt("line %u: %s\n", line, why);
	out_flush();
	return 2;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Hex digits to bytes, spaces and colons between them ignored. -1 on an odd
 * count or a stray character, otherwise the length. */
static int unhex(const char *s, uint8_t *b, int max)
{
	int n = 0, half = -1;

	for (; *s; s++) {
		int v;

		if (*s == ' ' || *s == ':')
			continue;
		v = hexval(*s);
		if (v < 0)
			return -1;
		if (half < 0) {
			half = v;
			continue;
		}
		if (n >= max)
			return -1;
		b[n++] = (uint8_t)(half << 4 | v);
		half = -1;
	}
	return half < 0 ? n : -1;
}

/* ---- the frames the verbs build ------------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)(v >> 16));
	put16(p + 2, (uint16_t)v);
}

/* The payload the kernel would deliver: prefix, Ethernet, an IPv4 header
 * with the router alert option, the IGMP message. `msg` is the IGMP message
 * with its checksum field zero. The source MAC is made up from the port. The
 * frame is marked taken while the lease the daemon holds takes the reports,
 * as the kernel marks it. */
static uint32_t build(uint8_t *b, uint8_t port, uint16_t vid, uint32_t sip,
		      uint32_t dip, const uint8_t *msg, uint32_t mlen)
{
	uint8_t *eth = b + IGMP_RX_PREFIX, *ip;
	uint32_t at = 12;

	b[0] = port;
	b[1] = (uint8_t)((vid >> 8) & 0x0f);
	if (igmpd_lease() & ODI_SW_MC_SNOOP_TRAP)
		b[1] |= IGMP_RX_TAKEN;
	b[2] = (uint8_t)vid;
	eth[0] = 0x01;
	eth[1] = 0x00;
	eth[2] = 0x5e;
	eth[3] = (uint8_t)((dip >> 16) & 0x7f);
	eth[4] = (uint8_t)(dip >> 8);
	eth[5] = (uint8_t)dip;
	eth[6] = 0x02;
	eth[7] = eth[8] = eth[9] = eth[10] = 0;
	eth[11] = port;
	if (vid) {
		put16(eth + at, ETH_P_8021Q);
		put16(eth + at + 2, vid);
		at += 4;
	}
	put16(eth + at, ETH_P_IP);
	ip = eth + at + 2;
	ip[0] = 0x46;
	ip[1] = 0xc0;
	put16(ip + 2, (uint16_t)(24 + mlen));
	put16(ip + 4, 0);
	put16(ip + 6, 0);
	ip[8] = 1;
	ip[9] = IPPROTO_IGMP;
	put16(ip + 10, 0);
	put32(ip + 12, sip);
	put32(ip + 16, dip);
	ip[20] = 148;
	ip[21] = 4;
	ip[22] = ip[23] = 0;
	put16(ip + 10, igmp_cksum(ip, 24));
	for (uint32_t i = 0; i < mlen; i++)
		ip[24 + i] = msg[i];
	put16(ip + 24 + 2, igmp_cksum(ip + 24, mlen));
	return IGMP_RX_PREFIX + (uint32_t)(ip + 24 + mlen - eth);
}

/* Returns the payload length, or 0 for a verb that is not a frame. */
static uint32_t frame_for(const char *verb, uint8_t port, uint16_t vid,
			  uint32_t sip, uint32_t group, uint8_t *b)
{
	uint8_t m[16];

	for (int i = 0; i < 16; i++)
		m[i] = 0;
	if (str_eq(verb, "v2join") || str_eq(verb, "v2leave")) {
		int leave = verb[2] == 'l';

		m[0] = leave ? IGMP_V2_LEAVE : IGMP_V2_REPORT;
		put32(m + 4, group);
		return build(b, port, vid, sip, leave ? 0xe0000002u : group, m, 8);
	}
	if (str_eq(verb, "v3join") || str_eq(verb, "v3leave")) {
		m[0] = IGMP_V3_REPORT;
		put16(m + 6, 1);
		m[8] = verb[2] == 'l' ? 3 : 2;
		put32(m + 12, group);
		return build(b, port, vid, sip, 0xe0000016u, m, 16);
	}
	if (str_eq(verb, "query")) {
		m[0] = IGMP_QUERY;
		m[1] = 100;
		return build(b, port, vid, sip, 0xe0000001u, m, 8);
	}
	return 0;
}

/* ---- the transcript ---------------------------------------------------- */

/* Split a line in place into words. Returns the count. */
static int words(char *s, char **w, int max)
{
	int n = 0;

	for (;;) {
		while (*s == ' ' || *s == '\t')
			*s++ = 0;
		if (!*s || *s == '#')
			break;
		if (n == max)
			return -1;
		w[n++] = s;
		while (*s && *s != ' ' && *s != '\t')
			s++;
	}
	*s = 0;
	return n;
}

int igmp_replay(const char *path)
{
	static uint8_t pay[IGMP_MAX_PAYLOAD];
	uint32_t len, line = 0, last_tick = 0, now = 0, pol_len = 0;
	int first = 1;
	char *p, *w[MAX_WORDS];
	long fd = 0;

	if (!str_eq(path, "-")) {
		fd = sys_open(path, O_RDONLY);
		if (fd < 0) {
			out_fmt("cannot open %s\n", path);
			out_flush();
			return 2;
		}
	}
	len = read_all(fd, text, MAX_TRANSCRIPT - 1);
	if (fd)
		sys_close((int)fd);
	text[len] = 0;

	igmpd_reset(&db);
	igmpd_replay_clock(0);
	igmpd_replay_policy(pol_buf, 0);

	for (p = text; *p;) {
		char *eol = p;
		char echo[160];
		int nw;
		uint32_t t;

		while (*eol && *eol != '\n')
			eol++;
		if (*eol)
			*eol++ = 0;
		line++;
		{
			char *e = p;

			while (*e == ' ' || *e == '\t')
				e++;
			str_copy(echo, e, sizeof echo);
		}
		nw = words(p, w, MAX_WORDS);
		p = eol;
		if (nw == 0)
			continue;
		if (nw < 0)
			return fail(line, "too many words");
		if (!igmp_parse_num(w[0], &t) || nw < 2)
			return fail(line, "expected: SECONDS EVENT ...");
		if (t < now)
			return fail(line, "the clock does not go backwards");

		/* The policy events change what the file holds and nothing else:
		 * the daemon reads it on its next second. */
		if (str_eq(w[1], "policy")) {
			uint32_t n = 0;

			out_fmt("> %u policy\n", t);
			while (*p) {
				char *l = p, *q;

				while (*p && *p != '\n')
					p++;
				if (*p)
					*p++ = 0;
				line++;
				while (*l == ' ' || *l == '\t')
					l++;
				if (str_eq(l, "end"))
					break;
				for (q = l; *q; q++) {
					if (n + 1 >= sizeof pol_buf)
						return fail(line, "policy too long");
					pol_buf[n++] = *q;
				}
				pol_buf[n++] = '\n';
			}
			pol_len = n;
			igmpd_replay_policy(pol_buf, pol_len);
			continue;
		}
		if (str_eq(w[1], "policy-file") && nw == 3) {
			long pf = sys_open(w[2], O_RDONLY);

			out_fmt("> %u policy-file\n", t);
			if (pf < 0)
				return fail(line, "cannot open the policy file");
			pol_len = read_all(pf, pol_buf, sizeof pol_buf);
			sys_close((int)pf);
			igmpd_replay_policy(pol_buf, pol_len);
			continue;
		}
		if (str_eq(w[1], "nopolicy") && nw == 2) {
			out_fmt("> %u nopolicy\n", t);
			pol_len = 0;
			igmpd_replay_policy(pol_buf, 0);
			continue;
		}

		out_fmt("> %s\n", echo);
		now = t;
		igmpd_replay_clock(now);
		if (now != last_tick || first) {
			last_tick = now;
			igmpd_tick(&db, now, first);
			first = 0;
		}
		if (str_eq(w[1], "tick") && nw == 2) {
			/* the step above is the event */
		} else if (str_eq(w[1], "state") && nw == 2) {
			igmpd_state_dump(&db);
		} else if (str_eq(w[1], "status") && nw == 2) {
			igmpd_status_dump(&db);
		} else if (str_eq(w[1], "meter") && nw == 2) {
			igmpd_meter_dump(&db);
		} else if (str_eq(w[1], "ctr")) {
			uint32_t group, bytes, i;

			if (nw != 4 || !igmp_parse_ipv4(w[2], &group) ||
			    !igmp_parse_num(w[3], &bytes))
				return fail(line, "expected: T ctr GROUP BYTES");
			for (i = 0; i < nctrs && ctrs[i].mac != (group & 0x7fffffu); i++)
				;
			if (i == nctrs) {
				if (nctrs == MAX_CTRS)
					return fail(line, "too many counters");
				ctrs[nctrs++].mac = group & 0x7fffffu;
			}
			ctrs[i].bytes = bytes;
		} else if (str_eq(w[1], "rx") && nw == 3) {
			int n = unhex(w[2], pay, (int)sizeof pay);

			if (n < 0)
				return fail(line, "bad hex");
			igmpd_frame(&db, pay, (uint32_t)n, now);
		} else if (str_eq(w[1], "query")) {
			uint32_t port, vid, sip;

			if (nw != 5 || !igmp_parse_num(w[2], &port) || port == 0 || port > 255 ||
			    !igmp_parse_num(w[3], &vid) || vid > 4095 ||
			    !igmp_parse_ipv4(w[4], &sip))
				return fail(line, "expected: T query PORT VID SRC");
			igmpd_frame(&db, pay, frame_for("query", (uint8_t)port,
				    (uint16_t)vid, sip, 0, pay), now);
		} else if (nw == 6 && !str_eq(w[1], "query") &&
			   frame_for(w[1], 0, 0, 0, 0, pay)) {
			uint32_t port, vid, sip, group;

			if (!igmp_parse_num(w[2], &port) || port == 0 || port > 255 ||
			    !igmp_parse_num(w[3], &vid) || vid > 4095 ||
			    !igmp_parse_ipv4(w[4], &sip) ||
			    !igmp_parse_ipv4(w[5], &group))
				return fail(line, "expected: T VERB PORT VID SRC GROUP");
			igmpd_frame(&db, pay, frame_for(w[1], (uint8_t)port,
				    (uint16_t)vid, sip, group, pay), now);
		} else {
			return fail(line, "unknown event");
		}
		out_flush();
	}
	out("> end\n");
	igmpd_state_dump(&db);
	out_flush();
	return 0;
}
