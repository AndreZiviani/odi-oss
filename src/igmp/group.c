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
	igmp_policy_defaults(&db->pol);
}

int igmp_db_count(const struct igmp_db *db)
{
	int n = 0;

	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use)
			n++;
	return n;
}

int igmp_db_dynamic_count(const struct igmp_db *db)
{
	int n = 0;

	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use && !db->g[i].fixed)
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

/* A group the snooper may keep state for. Outside 224.0.0.0/4 it is not a
 * multicast group at all (a host bug or a crafted frame), and an entry for it
 * would be programmed under whatever MAC its low bits give. RFC 4541 section
 * 2.1.2: groups in 224.0.0.0/24 are link-local control traffic and have to be
 * flooded, never restricted to the ports that asked. The L2 key keeps only 23
 * bits, so every group whose low 23 bits are below 256 (225.0.0.251,
 * 239.0.0.5, ...) lands on the same 01:00:5e:00:00:xx MAC as a link-local
 * one, and a static entry for it would restrict that traffic too. */
int igmp_group_snoopable(uint32_t group)
{
	if ((group >> 28) != 0xe)
		return 0;
	if ((group & 0x007fffffu) < 0x100u)
		return 0;
	return 1;
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

	if (port == 0 || port > IGMP_MAX_PORTS || !igmp_group_snoopable(group))
		return n;
	db->joins++;
	/* The profile decides. A join it does not allow gets no state, so the
	 * group is unregistered and, with the PON's drop on, the stream never
	 * reaches the host: the ONU-side enforcement the access control lists
	 * ask for. The report itself still goes upstream (the switch forwards
	 * it, the copy is only a copy), which is the "forward" behaviour of an
	 * ONU that leaves the OLT as the authority. */
	if (!igmp_policy_allows(&db->pol, group)) {
		db->denied++;
		return n;
	}
	e = find(db, vid, group);
	if (!e && db->pol.max_groups &&
	    (uint32_t)igmp_db_dynamic_count(db) >= db->pol.max_groups) {
		db->denied++;
		return n;
	}
	if (!e)
		e = find_or_add(db, vid, group);
	if (!e)
		return n;
	bit = IGMP_PORTBIT(port);
	before = e->ports;
	e->ports |= bit;
	e->expires[port - 1] = now + igmp_policy_membership_ttl(&db->pol);
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

	if (!e || port == 0 || port > IGMP_MAX_PORTS || e->fixed)
		return n;
	if (!(e->ports & IGMP_PORTBIT(port)))
		return n;
	if (db->pol.immediate_leave) {
		e->ports &= ~IGMP_PORTBIT(port);
		if (e->ports) {
			n = emit(out, max, n, IGMP_CHANGE_SET, vid, group, e->ports);
		} else {
			n = emit(out, max, n, IGMP_CHANGE_DEL, vid, group, 0);
			e->in_use = 0;
		}
		return n;
	}
	deadline = now + igmp_policy_last_member_ttl(&db->pol);
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
			 * joins and leaves. Record types (RFC 3376 4.2.12):
			 * 1 IS_INCLUDE, 2 IS_EXCLUDE, 3 TO_INCLUDE, 4
			 * TO_EXCLUDE, 5 ALLOW_NEW_SOURCES, 6 BLOCK_OLD_SOURCES.
			 * INCLUDE with no sources is the only shape that means
			 * "stop". An EXCLUDE, or an INCLUDE / ALLOW naming
			 * sources, is somebody asking for traffic; treating it
			 * as a plain join over-forwards rather than cutting a
			 * listener off. ALLOW with no sources adds nothing, and
			 * BLOCK only trims a source list: neither creates a
			 * membership, and an unknown type is not ours to read. */
			if ((t == 1 || t == 3) && nsrc == 0)
				n = leave(db, rx->vid, grp, rx->port, now,
					  out, max, n);
			else if (t == 2 || t == 4 ||
				 ((t == 1 || t == 3 || t == 5) && nsrc > 0))
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

		if (!e->in_use || e->fixed)
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

/* The members of every live group that shares this group's MAC, over all VIDs:
 * what the one hardware entry for that MAC has to carry. The L2 key is the MAC
 * on filtering id 0, so two groups aliased 32 to 1, or one group on two VLANs,
 * are a single row, and writing either one's own mask would overwrite the
 * other's. */
uint32_t igmp_db_mac_ports(const struct igmp_db *db, uint32_t group)
{
	uint32_t ports = 0;

	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use &&
		    (db->g[i].group & 0x007fffffu) == (group & 0x007fffffu))
			ports |= db->g[i].ports;
	return ports;
}

/* Make `g` a fixed group on the UNI port, as the static list asks. */
static int fix(struct igmp_db *db, uint32_t g, struct igmp_change *out, int max,
	       int n)
{
	struct igmp_entry *e;
	uint32_t before;

	if (!igmp_group_snoopable(g))
		return n;
	e = find_or_add(db, 0, g);
	if (!e)
		return n;
	before = e->ports;
	e->fixed = 1;
	e->ports |= IGMP_PORTBIT(1);
	if (e->ports != before)
		n = emit(out, max, n, IGMP_CHANGE_SET, 0, g, e->ports);
	return n;
}

int igmp_db_set_policy(struct igmp_db *db, const struct igmp_policy *pol,
		       struct igmp_change *out, int max)
{
	int n = 0;

	db->pol = *pol;
	db->static_overflow = 0;

	/* The static groups of the new list become fixed. What was fixed before
	 * and is not in it any more is let go: it is an ordinary entry again,
	 * held by whatever joins it has (a fixed entry never refreshes an expiry
	 * of its own, so one nobody joined has a past deadline and lapses at the
	 * next tick). */
	for (uint32_t r = 0; r < pol->nstatic; r++) {
		uint32_t lo = pol->stat[r].lo, hi = pol->stat[r].hi;

		if (hi - lo >= IGMP_STATIC_RANGE_MAX) {
			db->static_overflow = 1;
			continue;
		}
		for (uint32_t g = lo;; g++) {
			n = fix(db, g, out, max, n);
			if (g == hi)
				break;
		}
	}
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		struct igmp_entry *e = &db->g[i];
		int listed = 0;

		if (!e->in_use || !e->fixed)
			continue;
		for (uint32_t r = 0; r < pol->nstatic; r++)
			if (e->group >= pol->stat[r].lo && e->group <= pol->stat[r].hi &&
			    pol->stat[r].hi - pol->stat[r].lo < IGMP_STATIC_RANGE_MAX)
				listed = 1;
		if (!listed)
			e->fixed = 0;
	}

	/* A joined group the lists no longer allow goes at once. */
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		struct igmp_entry *e = &db->g[i];

		if (!e->in_use || e->fixed || igmp_policy_allows(pol, e->group))
			continue;
		n = emit(out, max, n, IGMP_CHANGE_DEL, e->vid, e->group, 0);
		e->in_use = 0;
		e->ports = 0;
	}
	return n;
}
