/* The multicast policy file: parsing, and the questions asked of it. */
#include "policy.h"
#include "entry.h"
#include "igmpd.h"

#define DEF_ROBUSTNESS   2
#define DEF_QUERY_INTV   125
#define DEF_QUERY_RESP   10
#define DEF_LAST_MEMBER  1

/* Bounds on what an OLT may ask for. A robustness of 0 would expire a
 * member at once, and a year-long query interval would keep a gone member
 * for a year; either is a mistake, not a policy. */
#define MAX_ROBUSTNESS   16
#define MAX_SECONDS      86400u

void igmp_policy_defaults(struct igmp_policy *p)
{
	for (unsigned i = 0; i < sizeof *p; i++)
		((uint8_t *)p)[i] = 0;
	p->robustness = DEF_ROBUSTNESS;
	p->query_interval = DEF_QUERY_INTV;
	p->query_max_response = DEF_QUERY_RESP;
	p->last_member_query = DEF_LAST_MEMBER;
}

/* Whether s[0..n) is exactly key. */
static int is(const char *s, uint32_t n, const char *key)
{
	uint32_t i = 0;

	while (key[i]) {
		if (i >= n || s[i] != key[i])
			return 0;
		i++;
	}
	return i == n;
}

/* A decimal number in s[0..n), up to a newline: 0 when it is not one. */
static int num(const char *s, uint32_t n, uint32_t *out)
{
	uint32_t v = 0;

	if (n == 0)
		return 0;
	for (uint32_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		if (v > 429496729u || (v == 429496729u && s[i] > '5'))
			return 0;
		v = v * 10 + (uint32_t)(s[i] - '0');
	}
	*out = v;
	return 1;
}

static int ip(const char *s, uint32_t n, uint32_t *out)
{
	char b[16];

	if (n == 0 || n >= sizeof b)
		return 0;
	for (uint32_t i = 0; i < n; i++)
		b[i] = s[i];
	b[n] = 0;
	return igmp_parse_ipv4(b, out);
}

static int range(const char *s, uint32_t n, struct igmp_range *r)
{
	uint32_t i = 0;

	while (i < n && s[i] != '-')
		i++;
	if (i == n)
		return 0;
	if (!ip(s, i, &r->lo) || !ip(s + i + 1, n - i - 1, &r->hi))
		return 0;
	return r->lo <= r->hi;
}

/* "A-B N": a range, one space, its bandwidth. */
static int bw_range(const char *s, uint32_t n, struct igmp_bw_range *b)
{
	struct igmp_range r;
	uint32_t i = 0;

	while (i < n && s[i] != ' ')
		i++;
	if (i == n || !range(s, i, &r) || !num(s + i + 1, n - i - 1, &b->bytes))
		return 0;
	b->lo = r.lo;
	b->hi = r.hi;
	return 1;
}

/* One `key=value` token of a row's tail (s[0..n)), into r. A token this
 * parser does not know, or cannot read, leaves the field as it was: the
 * wildcard, so a newer omcid never makes an older igmpd stricter. */
static void row_token(const char *s, uint32_t n, struct igmp_range *r)
{
	uint32_t v, k = 0, vals[4] = { 0, 0, 0, 0 };
	const char *val = s + 2;
	uint32_t vn = n > 2 ? n - 2 : 0;

	if (n < 3 || s[1] != '=')
		return;
	switch (s[0]) {
	case 'v':
		if (num(val, vn, &v) && v >= 1 && v <= 4095)
			r->vid = (uint16_t)v;
		break;
	case 'g':
		if (num(val, vn, &v) && v <= 0xffff)
			r->gem = (uint16_t)v;
		break;
	case 's':
		ip(val, vn, &r->src);
		break;
	case 'b':
		if (num(val, vn, &v))
			r->imputed_bw = v;
		break;
	case 'p':
		/* length,repeat time,repeat count,reset time */
		while (k < 4) {
			uint32_t e = 0;

			while (e < vn && val[e] != ',')
				e++;
			if (!num(val, e, &vals[k]) || vals[k] > 0xffff)
				return;
			k++;
			if (e == vn)
				break;
			val += e + 1;
			vn -= e + 1;
		}
		if (k != 4)
			return;
		r->pv_len = (uint16_t)vals[0];
		r->pv_repeat = (uint16_t)vals[1];
		r->pv_count = (uint16_t)vals[2];
		r->pv_reset = (uint16_t)vals[3];
		break;
	}
}

/* A range and the tokens after it, as omcid writes a row: `A-B v=1410
 * s=10.0.0.1 g=4000 b=125000 p=30,60,3,0`, the tokens in any order. */
static int row(const char *s, uint32_t n, struct igmp_range *r)
{
	uint32_t i = 0;

	r->vid = r->gem = r->pv_len = r->pv_repeat = r->pv_count = r->pv_reset = 0;
	r->src = r->imputed_bw = 0;
	while (i < n && s[i] != ' ')
		i++;
	if (!range(s, i, r))
		return 0;
	while (i < n) {
		uint32_t at;

		while (i < n && s[i] == ' ')
			i++;
		at = i;
		while (i < n && s[i] != ' ')
			i++;
		if (i > at)
			row_token(s + at, i - at, r);
	}
	return 1;
}

/* `G s=SRC a=ANI u=UNI d=MINUTES`, a row of the allowed preview table. */
static int grant(const char *s, uint32_t n, struct igmp_grant *g)
{
	uint32_t i = 0;

	g->src = 0;
	g->ani = g->uni = g->minutes = 0;
	while (i < n && s[i] != ' ')
		i++;
	if (!ip(s, i, &g->group))
		return 0;
	while (i < n) {
		uint32_t at, v;

		while (i < n && s[i] == ' ')
			i++;
		at = i;
		while (i < n && s[i] != ' ')
			i++;
		if (i - at < 3 || s[at + 1] != '=')
			continue;
		if (s[at] == 's')
			ip(s + at + 2, i - at - 2, &g->src);
		else if (num(s + at + 2, i - at - 2, &v) && v <= 0xffff) {
			if (s[at] == 'a')
				g->ani = (uint16_t)v;
			else if (s[at] == 'u')
				g->uni = (uint16_t)v;
			else if (s[at] == 'd')
				g->minutes = (uint16_t)v;
		}
	}
	return 1;
}

static uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

int igmp_policy_parse(const char *text, uint32_t len, struct igmp_policy *p)
{
	uint32_t at = 0;
	int understood = 0;

	igmp_policy_defaults(p);
	while (at < len) {
		const char *l = text + at;
		uint32_t n = 0, eq, v;
		struct igmp_range r;
		struct igmp_bw_range b;
		struct igmp_grant g;

		while (at + n < len && l[n] != '\n')
			n++;
		at += n + 1;
		if (n == 0 || l[0] == '#')
			continue;
		for (eq = 0; eq < n && l[eq] != '='; eq++)
			;
		if (eq == n)
			continue;
		{
			const char *val = l + eq + 1;
			uint32_t vn = n - eq - 1;

			if (is(l, eq, "snooping") && num(val, vn, &v)) {
				p->snooping = v == 1;
			} else if (is(l, eq, "igmp_function") && num(val, vn, &v)) {
				p->function = v;
			} else if (is(l, eq, "igmp_version") && num(val, vn, &v)) {
				p->igmp_version = v;
			} else if (is(l, eq, "querier") && ip(val, vn, &p->querier)) {
				/* stored only: nothing is sent */
			} else if (is(l, eq, "immediate_leave") && num(val, vn, &v)) {
				p->immediate_leave = v != 0;
			} else if (is(l, eq, "robustness") && num(val, vn, &v)) {
				p->robustness = clamp(v, 1, MAX_ROBUSTNESS);
			} else if (is(l, eq, "query_interval") && num(val, vn, &v)) {
				p->query_interval = clamp(v, 1, MAX_SECONDS);
			} else if (is(l, eq, "query_max_response") && num(val, vn, &v)) {
				p->query_max_response = clamp(v, 1, MAX_SECONDS);
			} else if (is(l, eq, "last_member_query") && num(val, vn, &v)) {
				p->last_member_query = clamp(v, 1, MAX_SECONDS);
			} else if (is(l, eq, "max_groups") && num(val, vn, &v)) {
				p->max_groups = v;
			} else if (is(l, eq, "unauthorized_join") && num(val, vn, &v)) {
				p->unauthorized_join = v != 0;
			} else if (is(l, eq, "us_tag_control") && num(val, vn, &v)) {
				p->us_tag_control = v;
			} else if (is(l, eq, "us_tci") && num(val, vn, &v)) {
				p->us_tci = v & 0xffff;
			} else if (is(l, eq, "us_rate") && num(val, vn, &v)) {
				p->us_rate = v;
			} else if (is(l, eq, "allow_overflow") && num(val, vn, &v)) {
				p->overflow = v != 0;
			} else if (is(l, eq, "allow") && row(val, vn, &r)) {
				if (p->nallow < IGMP_POLICY_MAX_RANGES)
					p->allow[p->nallow++] = r;
				else
					p->overflow = 1;
			} else if (is(l, eq, "max_bandwidth") && num(val, vn, &v)) {
				p->max_bandwidth = v;
			} else if (is(l, eq, "bw_enforce") && num(val, vn, &v)) {
				p->bw_enforce = v != 0;
			} else if (is(l, eq, "bw") && bw_range(val, vn, &b)) {
				if (p->nbw < IGMP_POLICY_MAX_RANGES)
					p->bw[p->nbw++] = b;
			} else if (is(l, eq, "grant") && grant(val, vn, &g)) {
				/* Only ever widens what is let in, so a table
				 * past the cap loses its last rows quietly. */
				if (p->ngrant >= IGMP_POLICY_MAX_GRANTS)
					continue;
				p->grant[p->ngrant++] = g;
			} else if (is(l, eq, "static") && row(val, vn, &r)) {
				if (p->nstatic < IGMP_POLICY_MAX_RANGES)
					p->stat[p->nstatic++] = r;
				else
					p->overflow = 1;
			} else {
				continue;
			}
			understood++;
		}
	}
	return understood;
}

int igmp_policy_acl_active(const struct igmp_policy *p)
{
	return !p->overflow && (p->nallow || p->nstatic);
}

static int in_ranges(const struct igmp_range *r, uint32_t n, uint32_t g)
{
	for (uint32_t i = 0; i < n; i++)
		if (g >= r[i].lo && g <= r[i].hi)
			return 1;
	return 0;
}

int igmp_policy_allows(const struct igmp_policy *p, uint32_t group)
{
	if (!igmp_policy_acl_active(p))
		return 1;
	return in_ranges(p->allow, p->nallow, group) ||
	       in_ranges(p->stat, p->nstatic, group);
}

uint32_t igmp_policy_imputed(const struct igmp_policy *p, uint32_t group)
{
	for (uint32_t i = 0; i < p->nbw; i++)
		if (group >= p->bw[i].lo && group <= p->bw[i].hi)
			return p->bw[i].bytes;
	return 0;
}

void igmp_policy_ani(const struct igmp_policy *p, struct igmp_join *j)
{
	j->ani_known = 0;
	j->ani_vid = 0;
	if (p->us_tag_control >= IGMP_US_TAG_ADD && p->us_tag_control <= IGMP_US_TAG_VID) {
		j->ani_known = 1;
		j->ani_vid = (uint16_t)(p->us_tci & 0x0fff);
	} else if (j->uni_vid) {
		/* Passed as it is, unless an extended VLAN rule moves it. */
		j->ani_known = 1;
		j->ani_vid = j->uni_vid;
	}
}

/* Whether a row of the dynamic list names this VLAN. */
static int vid_named(const struct igmp_policy *p, uint16_t vid)
{
	for (uint32_t i = 0; i < p->nallow; i++)
		if (p->allow[i].vid == vid)
			return 1;
	return 0;
}

/* Whether the join names this source: the row's source against the
 * join's. */
static int source_ok(const struct igmp_range *r, const struct igmp_join *j)
{
	int in = 0;

	if (!r->src || !j->has_src)
		return 1;
	for (uint32_t i = 0; i < j->nsrc; i++) {
		const uint8_t *a = j->srcs + 4 * i;
		uint32_t v = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 |
			     (uint32_t)a[2] << 8 | a[3];

		if (v == r->src)
			in = 1;
	}
	/* INCLUDE wants only these sources, EXCLUDE all but these. */
	return j->exclude ? !in : in;
}

int igmp_policy_join(const struct igmp_policy *p, const struct igmp_join *j,
		     const struct igmp_range **row)
{
	int preview = 0, by_vlan;

	*row = 0;
	if (!igmp_policy_acl_active(p))
		return IGMP_JOIN_FULL;
	if (in_ranges(p->stat, p->nstatic, j->group))
		return IGMP_JOIN_FULL;
	/* A row's VLAN is the one its stream runs on downstream, and the
	 * message that asks for it need not be on that VLAN (the IGMP tag
	 * control may name another). It is held against the join only when the
	 * join is on a VLAN some row names: that join is addressed to a
	 * stream VLAN, and a group of another VLAN's rows is not for it. A
	 * join on any other VLAN is judged by the ranges alone, as before. */
	by_vlan = j->ani_known && vid_named(p, j->ani_vid);
	for (uint32_t i = 0; i < p->nallow; i++) {
		const struct igmp_range *r = &p->allow[i];

		if (j->group < r->lo || j->group > r->hi)
			continue;
		if (r->vid && by_vlan && r->vid != j->ani_vid)
			continue;
		if (!source_ok(r, j))
			continue;
		if (!r->pv_len)
			return IGMP_JOIN_FULL;
		if (!preview) {
			preview = 1;
			*row = r;
		}
	}
	return preview ? IGMP_JOIN_PREVIEW : IGMP_JOIN_DENY;
}

int igmp_policy_version_ok(const struct igmp_policy *p, uint8_t type)
{
	uint32_t v = p->igmp_version;

	if (!p->snooping || v == 0 || v >= 3)
		return 1;
	switch (type) {
	case IGMP_V2_REPORT:
	case IGMP_V2_LEAVE:
		return v >= 2;
	case IGMP_V3_REPORT:
		return 0;
	}
	return 1;
}

uint32_t igmp_policy_membership_ttl(const struct igmp_policy *p)
{
	return p->robustness * p->query_interval + p->query_max_response;
}

uint32_t igmp_policy_last_member_ttl(const struct igmp_policy *p)
{
	return p->robustness * p->last_member_query;
}

int igmp_policy_takes(const struct igmp_policy *p)
{
	if (!p->snooping)
		return 0;
	/* A tag control G.988 does not define is not one to guess at: the
	 * report goes as the switch sends it, which is control 0. */
	if (p->us_tag_control >= IGMP_US_TAG_ADD && p->us_tag_control <= IGMP_US_TAG_VID)
		return 1;
	if (p->us_rate)
		return 1;
	return igmp_policy_acl_active(p) && !p->unauthorized_join;
}

uint32_t igmp_policy_lease(const struct igmp_policy *p, int static_overflow,
			   uint32_t now, uint32_t learn_until, int pon_drop)
{
	uint32_t copy = ODI_SW_MC_SNOOP_COPY;

	if (!p->snooping)
		return 0;
	if (igmp_policy_takes(p))
		copy |= ODI_SW_MC_SNOOP_TRAP;
	/* The drop is the one part that can cut a stream, so it is opt-in
	 * (igmpd -d); without it the copy and the state run and the PON
	 * keeps flooding. */
	if (!pon_drop)
		return copy;
	/* Compared as a difference so it stays right across the wrap of a
	 * 32-bit monotonic second counter. */
	if ((int32_t)(now - learn_until) < 0)
		return copy;
	/* A list longer than fits lost some of its static ranges, and a range
	 * too wide to name is not a row: either way a group the OLT promised
	 * the subscriber would be dropped as unregistered. */
	if (static_overflow || p->overflow)
		return copy;
	return copy | ODI_SW_MC_SNOOP_PON_DROP;
}
