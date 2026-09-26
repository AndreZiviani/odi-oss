/* The group table. No syscalls, no hardware -- see group.h for why. */
#include "group.h"

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

void igmp_db_init(struct igmp_db *db)
{
	for (unsigned i = 0; i < sizeof *db; i++)
		((uint8_t *)db)[i] = 0;
}

int igmp_db_count(const struct igmp_db *db)
{
	int n = 0;

	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use)
			n++;
	return n;
}

static struct igmp_entry *find(struct igmp_db *db, uint16_t vid, uint32_t group)
{
	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use && db->g[i].vid == vid &&
		    db->g[i].group == group)
			return &db->g[i];
	return 0;
}

static struct igmp_entry *find_or_add(struct igmp_db *db, uint16_t vid,
				      uint32_t group)
{
	struct igmp_entry *e = find(db, vid, group);

	if (e)
		return e;
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		if (!db->g[i].in_use) {
			e = &db->g[i];
			for (unsigned k = 0; k < sizeof *e; k++)
				((uint8_t *)e)[k] = 0;
			e->in_use = 1;
			e->vid = vid;
			e->group = group;
			return e;
		}
	}
	/* Full. Counted rather than silently ignored: a snooper that quietly
	 * stops learning looks exactly like one that is working, and the table
	 * being too small is a number somebody has to be able to read. */
	db->dropped++;
	return 0;
}

static int emit(struct igmp_change *out, int max, int n, uint8_t kind,
		uint16_t vid, uint32_t group, uint32_t ports)
{
	if (n < max) {
		out[n].kind = kind;
		out[n].vid = vid;
		out[n].group = group;
		out[n].ports = ports;
	}
	return n + 1;
}

/* A port joins, or refreshes what it already had. */
static int join(struct igmp_db *db, uint16_t vid, uint32_t group, uint8_t port,
		uint32_t now, struct igmp_change *out, int max, int n)
{
	struct igmp_entry *e;
	uint32_t bit, before;

	if (port == 0 || port > IGMP_MAX_PORTS)
		return n;
	e = find_or_add(db, vid, group);
	if (!e)
		return n;
	bit = IGMP_PORTBIT(port);
	before = e->ports;
	e->ports |= bit;
	e->expires[port - 1] = now + IGMP_MEMBERSHIP_TTL;
	/* Only a change of membership is a change. A refresh moves an expiry
	 * and nothing the switch can see, and reprogramming the same mask on
	 * every report would be a write per report per group. */
	if (e->ports != before)
		n = emit(out, max, n, IGMP_CHANGE_SET, vid, group, e->ports);
	return n;
}

/* A leave shortens the membership rather than ending it -- group.h says why. */
static int leave(struct igmp_db *db, uint16_t vid, uint32_t group, uint8_t port,
		 uint32_t now, struct igmp_change *out, int max, int n)
{
	struct igmp_entry *e = find(db, vid, group);
	uint32_t deadline;

	(void)out;
	(void)max;
	if (!e || port == 0 || port > IGMP_MAX_PORTS)
		return n;
	if (!(e->ports & IGMP_PORTBIT(port)))
		return n;
	deadline = now + IGMP_LAST_MEMBER_TTL;
	/* Never extend: a leave from a port whose membership is already about
	 * to lapse must not give it more time. */
	if (e->expires[port - 1] > deadline)
		e->expires[port - 1] = deadline;
	return n;
}

static int router_seen(struct igmp_db *db, uint8_t port, uint32_t now,
		       struct igmp_change *out, int max, int n)
{
	uint32_t bit, before;

	if (port == 0 || port > IGMP_MAX_PORTS)
		return n;
	bit = IGMP_PORTBIT(port);
	before = db->router_ports;
	db->router_ports |= bit;
	db->router_expires[port - 1] = now + IGMP_ROUTER_TTL;
	if (db->router_ports != before)
		n = emit(out, max, n, IGMP_CHANGE_ROUTER, 0, 0,
			 db->router_ports);
	return n;
}

/* One v3 group record. RFC 3376 gives the header as
 *
 *     u8 type, u8 aux_len, u16 nsources, u32 group, u32 sources[nsources]
 *
 * and aux_len counts 32-bit words of auxiliary data after the sources. Returns
 * the record length, or 0 when it does not fit in what is left.
 */
static uint32_t v3_record_len(const uint8_t *p, uint32_t avail)
{
	uint32_t nsrc, len;

	if (avail < 8)
		return 0;
	nsrc = rd16(p + 2);
	len = 8 + nsrc * 4 + (uint32_t)p[1] * 4;
	if (len > avail)
		return 0;
	return len;
}

int igmp_db_rx(struct igmp_db *db, const struct igmp_rx *rx, uint32_t now,
	       struct igmp_change *out, int max)
{
	int n = 0;

	switch (rx->type) {
	case IGMP_QUERY:
		/* Any query makes the port it arrived on a router port -- a
		 * group-specific one as much as a general one, because both
		 * come from whatever is acting as querier. */
		n = router_seen(db, rx->port, now, out, max, n);
		break;

	case IGMP_V1_REPORT:
	case IGMP_V2_REPORT:
		n = join(db, rx->vid, rx->group, rx->port, now, out, max, n);
		break;

	case IGMP_V2_LEAVE:
		n = leave(db, rx->vid, rx->group, rx->port, now, out, max, n);
		break;

	case IGMP_V3_REPORT: {
		/* Records start at +8; igmp_parse has already checked that
		 * nrec cannot exceed what the shortest records would fill. */
		const uint8_t *p = rx->body + 8;
		uint32_t left = rx->body_len - 8;

		for (uint16_t i = 0; i < rx->nrec; i++) {
			uint32_t len = v3_record_len(p, left);
			uint32_t grp, nsrc;
			uint8_t t;

			if (len == 0)
				break;
			t = p[0];
			nsrc = rd16(p + 2);
			grp = rd32(p + 4);

			/* RFC 4541 section 2.1.1: a snooper may reduce v3 to
			 * joins and leaves. INCLUDE with no sources is the
			 * only shape that means "stop"; everything else is
			 * somebody asking for traffic, and treating an
			 * EXCLUDE-with-sources as a plain join over-forwards
			 * rather than cutting a listener off. */
			if ((t == 1 || t == 3) && nsrc == 0)
				n = leave(db, rx->vid, grp, rx->port, now,
					  out, max, n);
			else
				n = join(db, rx->vid, grp, rx->port, now,
					 out, max, n);
			p += len;
			left -= len;
		}
		break;
	}
	default:
		break;
	}
	return n;
}

int igmp_db_tick(struct igmp_db *db, uint32_t now,
		 struct igmp_change *out, int max)
{
	int n = 0;

	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		struct igmp_entry *e = &db->g[i];
		uint32_t before;

		if (!e->in_use)
			continue;
		before = e->ports;
		for (int p = 0; p < IGMP_MAX_PORTS; p++) {
			if (!(e->ports & ((uint32_t)1 << p)))
				continue;
			/* Compared as a difference so it stays right across
			 * the wrap of a 32-bit monotonic second counter --
			 * 136 years away, but the alternative costs nothing. */
			if ((int32_t)(now - e->expires[p]) >= 0)
				e->ports &= ~((uint32_t)1 << p);
		}
		if (e->ports == before)
			continue;
		if (e->ports == 0) {
			n = emit(out, max, n, IGMP_CHANGE_DEL, e->vid,
				 e->group, 0);
			e->in_use = 0;
		} else {
			n = emit(out, max, n, IGMP_CHANGE_SET, e->vid,
				 e->group, e->ports);
		}
	}

	{
		uint32_t before = db->router_ports;

		for (int p = 0; p < IGMP_MAX_PORTS; p++) {
			if (!(db->router_ports & ((uint32_t)1 << p)))
				continue;
			if ((int32_t)(now - db->router_expires[p]) >= 0)
				db->router_ports &= ~((uint32_t)1 << p);
		}
		if (db->router_ports != before)
			n = emit(out, max, n, IGMP_CHANGE_ROUTER, 0, 0,
				 db->router_ports);
	}
	return n;
}
