/* Metering the joined groups -- see meter.h. */
#include "io.h"
#include "meter.h"

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* Two groups with the same low 23 bits share a MAC, a row and a counter. */
static uint32_t mac_of(uint32_t group)
{
	return group & 0x7fffffu;
}

void igmp_meter_init(struct igmp_meter *m)
{
	uint8_t *b = (uint8_t *)m;

	for (unsigned i = 0; i < sizeof *m; i++)
		b[i] = 0;
}

/* The dynamic entry of the table for (vid, group), or 0. */
static const struct igmp_entry *held(const struct igmp_db *db, uint16_t vid,
				     uint32_t group)
{
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		const struct igmp_entry *e = &db->g[i];

		if (e->in_use && !e->fixed && e->vid == vid && e->group == group)
			return e;
	}
	return 0;
}

static struct igmp_meter_grp *grp_find(struct igmp_meter *m, uint16_t vid,
				       uint32_t group)
{
	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (m->g[i].used && m->g[i].vid == vid && m->g[i].group == group)
			return &m->g[i];
	return 0;
}

static const struct igmp_meter_grp *grp_get(const struct igmp_meter *m,
					    uint16_t vid, uint32_t group)
{
	return grp_find((struct igmp_meter *)m, vid, group);
}

static void note(struct igmp_meter *m, const struct igmp_db *db, uint16_t vid,
		 uint32_t group, uint32_t client, uint32_t now)
{
	struct igmp_meter_grp *g;

	if (!held(db, vid, group))
		return;
	g = grp_find(m, vid, group);
	for (int i = 0; !g && i < IGMP_MAX_GROUPS; i++)
		if (!m->g[i].used) {
			g = &m->g[i];
			g->used = 1;
			g->vid = vid;
			g->group = group;
		}
	if (!g)
		return;
	g->client = client;
	g->joined = now;
}

void igmp_meter_rx(struct igmp_meter *m, const struct igmp_db *db,
		   const struct igmp_rx *rx, uint32_t now)
{
	if (rx->type == IGMP_V1_REPORT || rx->type == IGMP_V2_REPORT) {
		note(m, db, rx->vid, rx->group, rx->sip, now);
		return;
	}
	if (rx->type != IGMP_V3_REPORT)
		return;
	/* The records as group.c walks them (igmp_parse has bounded nrec),
	 * and the same ones counted as joins: an EXCLUDE, or an INCLUDE,
	 * TO_INCLUDE or ALLOW naming sources. */
	{
		const uint8_t *p = rx->body + 8;
		uint32_t left = rx->body_len - 8;

		for (uint16_t i = 0; i < rx->nrec && left >= 8; i++) {
			uint32_t nsrc = (uint32_t)(p[2] << 8 | p[3]);
			uint32_t len = 8 + nsrc * 4 + (uint32_t)p[1] * 4;
			uint8_t t = p[0];

			if (len > left)
				break;
			if (t == 2 || t == 4 || ((t == 1 || t == 3 || t == 5) && nsrc > 0))
				note(m, db, rx->vid, rd32(p + 4), rx->sip, now);
			p += len;
			left -= len;
		}
	}
}

static void out_ip(uint32_t a)
{
	out_fmt("%u.%u.%u.%u", (a >> 24) & 0xff, (a >> 16) & 0xff,
		(a >> 8) & 0xff, a & 0xff);
}

static struct igmp_meter_ctr *ctr_of(struct igmp_meter *m, uint32_t group)
{
	for (int i = 0; i < IGMP_METER_SLOTS; i++)
		if (m->c[i].used && mac_of(m->c[i].group) == mac_of(group))
			return &m->c[i];
	return 0;
}

static const struct igmp_meter_ctr *ctr_get(const struct igmp_meter *m, uint32_t group)
{
	return ctr_of((struct igmp_meter *)m, group);
}

/* Whether the table holds a dynamic group on this MAC. */
static int mac_held(const struct igmp_db *db, uint32_t group)
{
	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use && !db->g[i].fixed &&
		    mac_of(db->g[i].group) == mac_of(group))
			return 1;
	return 0;
}

static void ctr_drop(struct igmp_meter *m, const struct igmp_meter_ops *ops, int slot)
{
	if (ops->remove)
		(void)ops->remove((uint32_t)slot);
	m->c[slot].used = 0;
}

/* Put a counter in for `group`'s MAC. A refusal holds every install back
 * for a while, and is said once until an install works again. Returns 0 or
 * the error. */
static int ctr_add(struct igmp_meter *m, const struct igmp_meter_ops *ops,
		   int slot, uint32_t group, uint32_t now)
{
	struct igmp_meter_ctr *c = &m->c[slot];
	uint32_t base = 0;
	int rc = ops->install((uint32_t)slot, group);

	if (!rc && ops->read((uint32_t)slot, group, &base)) {
		(void)ops->remove((uint32_t)slot);
		rc = -5;
	}
	if (rc) {
		if (!m->refused) {
			out("  metering: no counter for ");
			out_ip(group);
			out_fmt(" (%d): its flow is unknown, asked again in %d s\n",
				rc, IGMP_METER_RETRY_S);
		}
		m->refused = 1;
		m->retry_at = now + IGMP_METER_RETRY_S;
		return rc;
	}
	m->refused = 0;
	c->used = 1;
	c->moved = 0;
	c->flow = IGMP_FLOW_UNKNOWN;
	c->group = group;
	c->last = base;
	c->grew_at = now;
	c->win_at = now;
	c->win_bytes = 0;
	c->bps = 0;
	return 0;
}

/* One read of one counter, and what it says. */
static void ctr_sample(struct igmp_meter *m, const struct igmp_meter_ops *ops,
		       int slot, uint32_t now)
{
	struct igmp_meter_ctr *c = &m->c[slot];
	uint32_t v, d;

	if (ops->read((uint32_t)slot, c->group, &v)) {
		/* A counter that cannot be read says nothing: out, and in
		 * again on the next second, as a new one. */
		out("  metering: the counter of ");
		out_ip(c->group);
		out(" did not read: put in again\n");
		ctr_drop(m, ops, slot);
		return;
	}
	/* Modulo 2^32: one wrap a second needs 34 Gbit/s. */
	d = v - c->last;
	c->last = v;
	if (d) {
		c->moved = 1;
		c->grew_at = now;
		if (c->flow == IGMP_FLOW_LOST) {
			out("  flowing again: ");
			out_ip(c->group);
			out("\n");
		}
		c->flow = IGMP_FLOW_UP;
	} else if (c->moved && c->flow == IGMP_FLOW_UP &&
		   now - c->grew_at >= IGMP_METER_LOST_S) {
		c->flow = IGMP_FLOW_LOST;
		out("  lost: ");
		out_ip(c->group);
		out_fmt(": nothing arrived for %d s\n", IGMP_METER_LOST_S);
	}
	c->win_bytes += d;
	if (now - c->win_at >= IGMP_METER_WINDOW_S) {
		c->bps = c->win_bytes / (now - c->win_at);
		c->win_at = now;
		c->win_bytes = 0;
	}
}

void igmp_meter_tick(struct igmp_meter *m, const struct igmp_db *db,
		     const struct igmp_meter_ops *ops, int on, uint32_t now)
{
	/* The clients of groups the table no longer holds. */
	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (m->g[i].used && !held(db, m->g[i].vid, m->g[i].group))
			m->g[i].used = 0;
	/* The counters of MACs no group of the table is on. */
	for (int i = 0; i < IGMP_METER_SLOTS; i++)
		if (m->c[i].used && (!on || !mac_held(db, m->c[i].group)))
			ctr_drop(m, ops, i);
	if (!on || !ops->install)
		return;
	/* A counter for each MAC without one, in table order, while there
	 * are rules to give. */
	if ((int32_t)(now - m->retry_at) >= 0) {
		for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
			const struct igmp_entry *e = &db->g[i];
			int slot = -1;

			if (!e->in_use || e->fixed || ctr_of(m, e->group))
				continue;
			for (int k = 0; k < IGMP_METER_SLOTS && slot < 0; k++)
				if (!m->c[k].used)
					slot = k;
			if (slot < 0 || ctr_add(m, ops, slot, e->group, now))
				break;
		}
	}
	for (int i = 0; i < IGMP_METER_SLOTS; i++)
		if (m->c[i].used)
			ctr_sample(m, ops, i, now);
}

void igmp_meter_release(struct igmp_meter *m, const struct igmp_meter_ops *ops)
{
	for (int i = 0; i < IGMP_METER_SLOTS; i++)
		if (m->c[i].used)
			ctr_drop(m, ops, i);
}

int igmp_meter_flow(const struct igmp_meter *m, uint32_t group)
{
	const struct igmp_meter_ctr *c = ctr_get(m, group);

	return c ? c->flow : IGMP_FLOW_UNKNOWN;
}

/* ---- the file -------------------------------------------------------- */

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

static uint32_t put_ip(char *b, uint32_t at, uint32_t max, uint32_t a)
{
	for (int k = 3; k >= 0; k--) {
		at = put_u(b, at, max, (a >> (8 * k)) & 0xff);
		if (k)
			at = put_s(b, at, max, ".");
	}
	return at;
}

static const char *flow_name(int flow)
{
	return flow == IGMP_FLOW_UP ? "up" : flow == IGMP_FLOW_LOST ? "lost" : "unknown";
}

uint32_t igmp_meter_format(const struct igmp_meter *m, const struct igmp_db *db,
			   char *buf, uint32_t max)
{
	uint32_t at = 0, bps = 0, lost = 0;

	for (int i = 0; i < IGMP_METER_SLOTS; i++)
		if (m->c[i].used)
			bps = bps + m->c[i].bps < bps ? 0xffffffffu : bps + m->c[i].bps;
	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use && !db->g[i].fixed &&
		    igmp_meter_flow(m, db->g[i].group) == IGMP_FLOW_LOST)
			lost++;
	at = put_s(buf, at, max, "bps=");
	at = put_u(buf, at, max, bps);
	at = put_s(buf, at, max, "\nlost=");
	at = put_u(buf, at, max, lost);
	at = put_s(buf, at, max, "\n");
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		const struct igmp_entry *e = &db->g[i];
		const struct igmp_meter_grp *g;
		const struct igmp_meter_ctr *c;

		if (!e->in_use || e->fixed)
			continue;
		g = grp_get(m, e->vid, e->group);
		c = ctr_get(m, e->group);
		at = put_s(buf, at, max, "group=");
		at = put_ip(buf, at, max, e->group);
		at = put_s(buf, at, max, " vid=");
		at = put_u(buf, at, max, e->vid);
		at = put_s(buf, at, max, " client=");
		at = put_ip(buf, at, max, g ? g->client : 0);
		at = put_s(buf, at, max, " joined=");
		at = put_u(buf, at, max, g ? g->joined : 0);
		if (c) {
			at = put_s(buf, at, max, " bps=");
			at = put_u(buf, at, max, c->bps);
		}
		at = put_s(buf, at, max, " flow=");
		at = put_s(buf, at, max, flow_name(c ? c->flow : IGMP_FLOW_UNKNOWN));
		at = put_s(buf, at, max, "\n");
	}
	return at >= max ? 0 : at;
}
