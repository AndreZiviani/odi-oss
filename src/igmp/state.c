#include "state.h"

static uint32_t put_s(char *b, uint32_t at, uint32_t max, const char *s)
{
	while (*s && at < max)
		b[at++] = *s++;
	return at;
}

static uint32_t put_u(char *b, uint32_t at, uint32_t max, uint32_t v)
{
	char d[10];
	int n = 0;

	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n && at < max)
		b[at++] = d[--n];
	return at;
}

static uint32_t put_kv(char *b, uint32_t at, uint32_t max, const char *k, uint32_t v)
{
	at = put_s(b, at, max, k);
	at = put_s(b, at, max, "=");
	at = put_u(b, at, max, v);
	return put_s(b, at, max, "\n");
}

const char *igmp_refusal_name(uint32_t reason)
{
	switch (reason) {
	case IGMP_REFUSE_ACL:   return "acl";
	case IGMP_REFUSE_LIMIT: return "limit";
	case IGMP_REFUSE_FULL:  return "full";
	case IGMP_REFUSE_BW:    return "bw";
	case IGMP_REFUSE_PREVIEW: return "preview";
	}
	return "?";
}

static uint32_t put_group(char *b, uint32_t at, uint32_t max, uint32_t g)
{
	for (int k = 3; k >= 0; k--) {
		at = put_u(b, at, max, (g >> (8 * k)) & 0xff);
		if (k)
			at = put_s(b, at, max, ".");
	}
	return at;
}

uint32_t igmp_state_format(const struct igmp_db *db, char *buf, uint32_t max)
{
	uint32_t at = 0;

	at = put_kv(buf, at, max, "joins", db->joins);
	at = put_kv(buf, at, max, "denied", db->denied);
	at = put_kv(buf, at, max, "groups", (uint32_t)igmp_db_dynamic_count(db));
	/* Up here, before the group lines: the counts survive any cut-off. */
	for (uint32_t r = 1; r < IGMP_REFUSE_REASONS; r++) {
		if (!db->refused[r])
			continue;
		at = put_s(buf, at, max, "refused_");
		at = put_kv(buf, at, max, igmp_refusal_name(r), db->refused[r]);
	}
	if (db->bw_exceeded)
		at = put_kv(buf, at, max, "bw_exceeded", db->bw_exceeded);
	/* The way up, while the reports are taken (uplink.h): a line only once
	 * its count is above zero, like the refusals, so a snooper whose copy
	 * stays a copy writes what it always wrote. */
	if (db->above_version)
		at = put_kv(buf, at, max, "above_version", db->above_version);
	if (db->up_sent)
		at = put_kv(buf, at, max, "up_sent", db->up_sent);
	if (db->up_withheld)
		at = put_kv(buf, at, max, "up_withheld", db->up_withheld);
	if (db->up_limited)
		at = put_kv(buf, at, max, "up_limited", db->up_limited);
	if (db->up_unsent)
		at = put_kv(buf, at, max, "up_unsent", db->up_unsent);
	/* One line a group, not a line a (vid, group): two VLANs of the same
	 * group are one group to the OLT. */
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		const struct igmp_entry *e = &db->g[i];
		int seen = 0;

		if (!e->in_use || e->fixed)
			continue;
		for (int j = 0; j < i && !seen; j++)
			if (db->g[j].in_use && !db->g[j].fixed && db->g[j].group == e->group)
				seen = 1;
		if (seen)
			continue;
		at = put_s(buf, at, max, "group=");
		at = put_group(buf, at, max, e->group);
		at = put_s(buf, at, max, "\n");
	}
	/* The ring, oldest first. */
	for (uint32_t i = 0; i < IGMP_REFUSAL_RING && i < db->refusals; i++) {
		uint32_t idx = db->refusals > IGMP_REFUSAL_RING
			     ? db->refusals - IGMP_REFUSAL_RING + i : i;
		const struct igmp_refusal *r = &db->ring[idx % IGMP_REFUSAL_RING];

		at = put_s(buf, at, max, "refused=");
		at = put_s(buf, at, max, igmp_refusal_name(r->reason));
		at = put_s(buf, at, max, " ");
		at = put_group(buf, at, max, r->group);
		at = put_s(buf, at, max, " vid=");
		at = put_u(buf, at, max, r->vid);
		at = put_s(buf, at, max, " at=");
		at = put_u(buf, at, max, r->at);
		at = put_s(buf, at, max, " n=");
		at = put_u(buf, at, max, r->count);
		at = put_s(buf, at, max, "\n");
	}
	return at >= max ? 0 : at;
}

/* ---- reading it back -------------------------------------------------- */

/* A decimal at *p (up to e); 0 and p unmoved when there are no digits. */
static int rd_u(const char **p, const char *e, uint32_t *v)
{
	const char *s = *p;
	uint64_t x = 0;

	while (s < e && *s >= '0' && *s <= '9' && x <= 0xffffffffu)
		x = x * 10 + (uint64_t)(*s++ - '0');
	if (s == *p || x > 0xffffffffu)
		return 0;
	*v = (uint32_t)x;
	*p = s;
	return 1;
}

static int rd_ch(const char **p, const char *e, char c)
{
	if (*p < e && **p == c) {
		(*p)++;
		return 1;
	}
	return 0;
}

static int rd_lit(const char **p, const char *e, const char *lit)
{
	const char *s = *p;

	while (*lit && s < e && *s == *lit) {
		s++;
		lit++;
	}
	if (*lit)
		return 0;
	*p = s;
	return 1;
}

static int rd_group(const char **p, const char *e, uint32_t *g)
{
	uint32_t a, v = 0;

	for (int i = 0; i < 4; i++) {
		if (!rd_u(p, e, &a) || a > 255 || (i < 3 && !rd_ch(p, e, '.')))
			return 0;
		v = v << 8 | a;
	}
	*g = v;
	return 1;
}

int igmp_state_parse(const char *text, uint32_t len, struct igmp_state_view *v)
{
	uint32_t at = 0;
	int understood = 0;

	for (unsigned i = 0; i < sizeof *v; i++)
		((char *)v)[i] = 0;
	while (at < len) {
		const char *l = text + at, *p = l, *e;
		uint32_t n = 0, x;

		while (at + n < len && l[n] != '\n')
			n++;
		if (at + n >= len)
			break;          /* no newline: the cut-off end */
		e = l + n;
		at += n + 1;
		if (rd_lit(&p, e, "joins=") && rd_u(&p, e, &x)) {
			v->joins = x;
		} else if (rd_lit(&p, e, "denied=") && rd_u(&p, e, &x)) {
			v->denied = x;
		} else if (rd_lit(&p, e, "groups=") && rd_u(&p, e, &x)) {
			v->groups = x;
		} else if (rd_lit(&p, e, "bw_exceeded=") && rd_u(&p, e, &x)) {
			v->bw_exceeded = x;
		} else if (rd_lit(&p, e, "above_version=") && rd_u(&p, e, &x)) {
			v->above_version = x;
		} else if (rd_lit(&p, e, "up_sent=") && rd_u(&p, e, &x)) {
			v->up_sent = x;
		} else if (rd_lit(&p, e, "up_withheld=") && rd_u(&p, e, &x)) {
			v->up_withheld = x;
		} else if (rd_lit(&p, e, "up_limited=") && rd_u(&p, e, &x)) {
			v->up_limited = x;
		} else if (rd_lit(&p, e, "up_unsent=") && rd_u(&p, e, &x)) {
			v->up_unsent = x;
		} else if (rd_lit(&p, e, "refused_")) {
			uint32_t r;

			for (r = 1; r < IGMP_REFUSE_REASONS; r++) {
				const char *q = p;

				if (rd_lit(&q, e, igmp_refusal_name(r)) &&
				    rd_ch(&q, e, '=') && rd_u(&q, e, &x)) {
					v->refused[r] = x;
					break;
				}
			}
			if (r == IGMP_REFUSE_REASONS)
				continue;
		} else if (rd_lit(&p, e, "group=") && v->ngroup < IGMP_MAX_GROUPS) {
			if (!rd_group(&p, e, &x))
				continue;
			v->group[v->ngroup++] = x;
		} else if (rd_lit(&p, e, "refused=")) {
			struct igmp_refusal r;
			uint32_t k, vid;

			for (k = 1; k < IGMP_REFUSE_REASONS; k++) {
				const char *q = p;

				if (rd_lit(&q, e, igmp_refusal_name(k)) &&
				    rd_ch(&q, e, ' ')) {
					p = q;
					break;
				}
			}
			r.reason = (uint8_t)k;
			if (k == IGMP_REFUSE_REASONS || !rd_group(&p, e, &r.group) ||
			    !rd_lit(&p, e, " vid=") || !rd_u(&p, e, &vid) || vid > 4095 ||
			    !rd_lit(&p, e, " at=") || !rd_u(&p, e, &r.at) ||
			    !rd_lit(&p, e, " n=") || !rd_u(&p, e, &r.count))
				continue;
			r.vid = (uint16_t)vid;
			if (v->nrefusal < IGMP_REFUSAL_RING)
				v->ref[v->nrefusal++] = r;
		} else {
			continue;
		}
		understood++;
	}
	return understood;
}
