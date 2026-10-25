/* The way up (uplink.c): what igmpd sends in place of a report the switch
 * took, byte for byte, for every upstream IGMP tag control of G.988 9.3.27
 * (attribute 5, with the TCI of attribute 4), for the classifier treatments
 * tag control 0 applies, and the decision and rate around them. The expected
 * frames are written out here from the standard's wording, not computed by
 * the code under test.
 */
#include "uplink.h"
#include "group.h"
#include "policy.h"
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

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static void zero(uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		p[i] = 0;
}

/* The MACs and the IPv4 + IGMP part of a v2 report for 239.1.1.1, 32 bytes
 * (a 24-byte header with the router alert, an 8-byte message); checksums are
 * not this file's business and are left as they come. */
static const uint8_t macs[12] = {
	0x01, 0x00, 0x5e, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x09,
};
static const uint8_t ipigmp[32] = {
	0x46, 0xc0, 0x00, 0x20, 0, 0, 0, 0, 0x01, 0x02, 0x12, 0x34,
	0xc0, 0xa8, 0x01, 0x0a, 0xef, 0x01, 0x01, 0x01, 0x94, 0x04, 0, 0,
	0x16, 0x00, 0xab, 0xcd, 0xef, 0x01, 0x01, 0x01,
};

/* A frame: the MACs, the tags given (TPID, TCI pairs), 0x0800, the IPv4 and
 * IGMP bytes, padded with zeros to `pad` (the switch hands the CPU a padded
 * minimum frame). Returns the length. */
static uint32_t frame(uint8_t *f, const uint16_t *tags, int ntags, uint32_t pad)
{
	uint32_t at = 0;

	zero(f, 128);
	for (int i = 0; i < 12; i++)
		f[at++] = macs[i];
	for (int k = 0; k < ntags; k++) {
		f[at++] = (uint8_t)(tags[2 * k] >> 8);
		f[at++] = (uint8_t)tags[2 * k];
		f[at++] = (uint8_t)(tags[2 * k + 1] >> 8);
		f[at++] = (uint8_t)tags[2 * k + 1];
	}
	f[at++] = 0x08;
	f[at++] = 0x00;
	for (int i = 0; i < 32; i++)
		f[at++] = ipigmp[i];
	return at < pad ? pad : at;
}

#define TCI(pri, dei, vid) ((uint16_t)(((pri) << 13) | ((dei) << 12) | (vid)))

static void test_tags(void)
{
	uint8_t f[128];
	struct igmp_up_tags t;
	uint16_t sc[4] = { 0x88a8, TCI(3, 0, 200), 0x8100, TCI(5, 0, 410) };
	uint16_t cc[4] = { 0x8100, TCI(1, 0, 11), 0x8100, TCI(2, 0, 12) };
	uint32_t n;

	n = frame(f, 0, 0, 60);
	ok(igmp_up_read_tags(f, n, &t) && !t.stag && !t.ctag && t.ethertype == 0x0800 &&
	   t.hdr_len == 14, "untagged: no tag, EtherType at 12");
	n = frame(f, sc + 2, 1, 60);
	ok(igmp_up_read_tags(f, n, &t) && !t.stag && t.ctag && t.ctci == TCI(5, 0, 410) &&
	   t.ethertype == 0x0800 && t.hdr_len == 18, "one C-tag");
	n = frame(f, sc, 2, 60);
	ok(igmp_up_read_tags(f, n, &t) && t.stag && t.stci == TCI(3, 0, 200) && t.ctag &&
	   t.ctci == TCI(5, 0, 410) && t.hdr_len == 22, "an S-tag in front of a C-tag");
	n = frame(f, cc, 2, 60);
	ok(igmp_up_read_tags(f, n, &t) && !t.stag && t.ctag && t.ctci == TCI(1, 0, 11) &&
	   t.ethertype == 0x8100 && t.hdr_len == 18,
	   "two C-tags: the outer one is the C-tag, the inner one payload (as the classifier reads it)");
	ok(!igmp_up_read_tags(f, 15, &t), "a tag cut off is not read");
}

/* Tag control 1: "add a VLAN tag (including P bits) ... specified by the
 * upstream IGMP TCI attribute", in front of whatever the frame has. */
static void test_add(void)
{
	uint8_t in[128], out[160], want[128];
	uint16_t add[2] = { 0x8100, TCI(5, 0, 420) };
	uint16_t two[4] = { 0x8100, TCI(5, 0, 420), 0x8100, TCI(2, 0, 11) };
	uint16_t vid;
	uint8_t pri;
	uint32_t n, w;

	n = frame(in, 0, 0, 60);
	w = frame(want, add, 1, 64);
	ok(igmp_up_retag(in, n, 1, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == w && w == 64 &&
	   same(out, want, w) && vid == 420 && pri == 5,
	   "1, untagged: 0x8100 vid 420 pri 5 after the MACs, the rest as it was (64 bytes)");
	n = frame(in, two + 2, 1, 64);
	w = frame(want, two, 2, 68);
	ok(igmp_up_retag(in, n, 1, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == w &&
	   same(out, want, w) && vid == 420,
	   "1, tagged: the TCI goes in front, the host's tag stays behind it");
	n = frame(in, 0, 0, 46);
	w = frame(want, add, 1, 60);
	ok(igmp_up_retag(in, n, 1, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == 60 &&
	   same(out, want, 60), "a short frame is padded to the 60 bytes of a minimum frame");
	ok(igmp_up_retag(in, n, 1, TCI(5, 0, 420), out, 59, &vid, &pri) == 0,
	   "a frame that does not fit the buffer is not built");
	ok(igmp_up_retag(in, n, 0, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == 0 &&
	   igmp_up_retag(in, n, 4, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == 0,
	   "controls 0 and 4 are not re-tags");
}

/* Tag control 2: "replace the entire TCI (VLAN ID plus P bits) ... If the
 * received IGMP/MLD traffic is untagged, an add operation is performed." */
static void test_replace(void)
{
	uint8_t in[128], out[160], want[128];
	uint16_t host[2] = { 0x8100, TCI(2, 1, 11) };
	uint16_t rep[2] = { 0x8100, TCI(5, 0, 420) };
	uint16_t sc_in[4] = { 0x88a8, TCI(3, 0, 200), 0x8100, TCI(2, 0, 11) };
	uint16_t sc_out[4] = { 0x88a8, TCI(5, 0, 420), 0x8100, TCI(2, 0, 11) };
	uint16_t vid;
	uint8_t pri;
	uint32_t n, w;

	n = frame(in, host, 1, 64);
	w = frame(want, rep, 1, 64);
	ok(igmp_up_retag(in, n, 2, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == w &&
	   same(out, want, w) && vid == 420 && pri == 5,
	   "2, tagged vid 11 pri 2 DEI 1: the whole TCI becomes vid 420 pri 5 DEI 0");
	n = frame(in, 0, 0, 60);
	w = frame(want, rep, 1, 64);
	ok(igmp_up_retag(in, n, 2, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == w &&
	   same(out, want, w), "2, untagged: added, as 1 would");
	n = frame(in, sc_in, 2, 68);
	w = frame(want, sc_out, 2, 68);
	ok(igmp_up_retag(in, n, 2, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == w &&
	   same(out, want, w), "2, S-tag over C-tag: the outer tag's TCI, its TPID and the C-tag kept");
}

/* Tag control 3: "replace only the VLAN ID ... retaining the original DEI
 * and P bits ... If the received IGMP/MLD traffic is untagged, an add
 * operation is performed, with DEI and P bits also taken from the upstream
 * IGMP TCI attribute." */
static void test_vid(void)
{
	uint8_t in[128], out[160], want[128];
	uint16_t host[2] = { 0x8100, TCI(6, 1, 11) };
	uint16_t rep[2] = { 0x8100, TCI(6, 1, 420) };
	uint16_t add[2] = { 0x8100, TCI(5, 1, 420) };
	uint16_t vid;
	uint8_t pri;
	uint32_t n, w;

	n = frame(in, host, 1, 64);
	w = frame(want, rep, 1, 64);
	ok(igmp_up_retag(in, n, 3, TCI(5, 0, 420), out, sizeof out, &vid, &pri) == w &&
	   same(out, want, w) && vid == 420 && pri == 6,
	   "3, tagged vid 11 pri 6 DEI 1: vid 420, the host's pri 6 and DEI 1 kept");
	n = frame(in, 0, 0, 60);
	w = frame(want, add, 1, 64);
	ok(igmp_up_retag(in, n, 3, TCI(5, 1, 420), out, sizeof out, &vid, &pri) == w &&
	   same(out, want, w) && pri == 5,
	   "3, untagged: the whole TCI added, pri 5 and DEI 1 from the profile");
}

static struct odi_sw_mc_route route(uint32_t cact, uint32_t cvid, uint32_t cpri, uint32_t csrc,
				    uint32_t sact, uint32_t svid, uint32_t spri, uint32_t ssrc)
{
	struct odi_sw_mc_route r;

	for (unsigned i = 0; i < sizeof r; i++)
		((uint8_t *)&r)[i] = 0;
	r.verdict = ODI_SW_US_FWD;
	r.c_act = cact;
	r.c_vid = cvid;
	r.c_pri = cpri;
	r.c_src = csrc;
	r.s_act = sact;
	r.s_vid = svid;
	r.s_pri = spri;
	r.s_src = ssrc;
	return r;
}

/* Tag control 0: the frame leaves as the classifier row would have sent it
 * (docs/SWITCH.md, "Extended VLAN rules": add or replace with what the row
 * assigns, the rest the frame's own; delete; leave). */
static void test_treat(void)
{
	uint8_t in[128], out[160], want[128];
	uint16_t isp[2] = { 0x8100, TCI(0, 0, 11) };
	uint16_t host[2] = { 0x8100, TCI(5, 0, 410) };
	uint16_t xl[2] = { 0x8100, TCI(5, 0, 1410) };
	uint16_t s_c[4] = { 0x88a8, TCI(3, 0, 2002), 0x8100, TCI(5, 0, 410) };
	struct odi_sw_mc_route r;
	uint32_t n, w;

	n = frame(in, 0, 0, 60);
	r = route(ODI_SW_CACT_ADD, 11, 0, ODI_SW_US_SRC_VID | ODI_SW_US_SRC_PRI, 4, 0, 0, 0);
	w = frame(want, isp, 1, 64);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == w && same(out, want, w),
	   "an untagged report on the row that adds vid 11 pri 0 (ISP1's untagged service)");
	n = frame(in, host, 1, 64);
	r = route(ODI_SW_CACT_ADD, 1410, 0, ODI_SW_US_SRC_VID, 4, 0, 0, 0);
	w = frame(want, xl, 1, 64);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == w && same(out, want, w),
	   "a translation 410 to 1410, priority not assigned: the frame's pri 5 kept");
	r = route(ODI_SW_CACT_TRANSPARENT, 0, 0, 0, 4, 0, 0, 0);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == n && same(out, in, n),
	   "a transparent row: the frame as it came");
	r = route(ODI_SW_CACT_DEL, 0, 0, 0, 4, 0, 0, 0);
	w = frame(want, 0, 0, 60);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == 60 && same(out, want, 60),
	   "a row that deletes the C-tag: untagged, padded back to 60");
	r = route(ODI_SW_CACT_TRANSPARENT, 0, 0, 0, ODI_SW_SACT_ADD, 2002, 3,
		  ODI_SW_US_SRC_VID | ODI_SW_US_SRC_PRI);
	w = frame(want, s_c, 2, 68);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == w && same(out, want, w),
	   "an S-tag 2002 pri 3 pushed in front of the C-tag");
	n = frame(in, s_c, 2, 68);
	r = route(ODI_SW_CACT_TRANSPARENT, 0, 0, 0, ODI_SW_SACT_DEL, 0, 0, 0);
	w = frame(want, host, 1, 64);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == w && same(out, want, w),
	   "the S-tag popped, the C-tag left");
	r = route(ODI_SW_CACT_DEL, 0, 0, 0, ODI_SW_SACT_DEL, 0, 0, 0);
	w = frame(want, 0, 0, 60);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == 60 && same(out, want, 60),
	   "both tags removed");
	r = route(ODI_SW_CACT_TRANSPARENT, 0, 0, 0, ODI_SW_SACT_REWRITE, 0, 0, 0);
	ok(igmp_up_treat(in, n, &r, out, sizeof out) == 0,
	   "an S-tag rewritten into a C-tag, which nobody has measured: not built");
}

static void test_decide(void)
{
	struct igmp_policy p;
	struct igmp_up_rate rate = { 0, 0, 0 };

	parse("snooping=1\nallow=225.0.0.0-225.255.255.255\n", &p);
	ok(igmp_up_decide(&p, IGMP_V2_REPORT, 1, &rate, 10) == IGMP_UP_WITHHELD,
	   "a join the lists refuse, unauthorized joins discarded: withheld");
	ok(igmp_up_decide(&p, IGMP_V2_REPORT, 0, &rate, 10) == IGMP_UP_SEND,
	   "a join they allow: sent");
	ok(igmp_up_decide(&p, IGMP_V3_REPORT, 0, &rate, 10) == IGMP_UP_SEND,
	   "a v3 report with one record allowed or a leave in it: sent");
	ok(igmp_up_decide(&p, IGMP_V2_LEAVE, 0, &rate, 10) == IGMP_UP_SEND, "a leave: sent");
	ok(igmp_up_decide(&p, IGMP_QUERY, 0, &rate, 10) == IGMP_UP_QUERY,
	   "a query from the host side: not sent");
	ok(igmp_up_decide(&p, 0, 0, &rate, 10) == IGMP_UP_SEND,
	   "a message that did not parse: sent as the switch would have");
	parse("snooping=1\nallow=225.0.0.0-225.255.255.255\nunauthorized_join=1\n", &p);
	ok(igmp_up_decide(&p, IGMP_V2_REPORT, 1, &rate, 10) == IGMP_UP_SEND,
	   "unauthorized joins forwarded: the refused join goes");

	/* Rate 2 a second: two go in each second, the rest are dropped, and a
	 * message not sent does not count against it. */
	parse("snooping=1\nus_rate=2\nallow=225.0.0.0-225.255.255.255\n", &p);
	ok(igmp_up_decide(&p, IGMP_V2_REPORT, 0, &rate, 20) == IGMP_UP_SEND &&
	   igmp_up_decide(&p, IGMP_V2_REPORT, 1, &rate, 20) == IGMP_UP_WITHHELD &&
	   igmp_up_decide(&p, IGMP_V2_REPORT, 0, &rate, 20) == IGMP_UP_SEND,
	   "rate 2: two sent in one second, a withheld one between them not counted");
	ok(igmp_up_decide(&p, IGMP_V2_REPORT, 0, &rate, 20) == IGMP_UP_RATE &&
	   igmp_up_decide(&p, IGMP_V2_LEAVE, 0, &rate, 20) == IGMP_UP_RATE,
	   "the third and fourth in that second: over the rate, a leave too");
	ok(igmp_up_decide(&p, IGMP_V2_REPORT, 0, &rate, 21) == IGMP_UP_SEND,
	   "the next second starts again");
	parse("snooping=1\nus_rate=0\n", &p);
	for (int i = 0; i < 50; i++)
		if (igmp_up_decide(&p, IGMP_V2_REPORT, 0, &rate, 30) != IGMP_UP_SEND)
			p.us_rate = 99;
	ok(p.us_rate == 0, "rate 0: no limit (fifty in a second all sent)");
}

int main(void)
{
	test_tags();
	test_add();
	test_replace();
	test_vid();
	test_treat();
	test_decide();
	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
