/* The multicast policy file: parsing, and the questions asked of it. */
#include "policy.h"
#include "entry.h"

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
			} else if (is(l, eq, "allow_overflow") && num(val, vn, &v)) {
				p->overflow = v != 0;
			} else if (is(l, eq, "allow") && range(val, vn, &r)) {
				if (p->nallow < IGMP_POLICY_MAX_RANGES)
					p->allow[p->nallow++] = r;
				else
					p->overflow = 1;
			} else if (is(l, eq, "static") && range(val, vn, &r)) {
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

uint32_t igmp_policy_membership_ttl(const struct igmp_policy *p)
{
	return p->robustness * p->query_interval + p->query_max_response;
}

uint32_t igmp_policy_last_member_ttl(const struct igmp_policy *p)
{
	return p->robustness * p->last_member_query;
}

uint32_t igmp_policy_lease(const struct igmp_policy *p, int static_overflow,
			   uint32_t now, uint32_t learn_until)
{
	if (!p->snooping)
		return 0;
	/* Compared as a difference so it stays right across the wrap of a
	 * 32-bit monotonic second counter. */
	if ((int32_t)(now - learn_until) < 0)
		return ODI_SW_MC_SNOOP_COPY;
	/* A list longer than fits lost some of its static ranges, and a range
	 * too wide to name is not a row: either way a group the OLT promised
	 * the subscriber would be dropped as unregistered. */
	if (static_overflow || p->overflow)
		return ODI_SW_MC_SNOOP_COPY;
	return ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP;
}
