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

/* Whether an entry below index `below` already holds this group, on any VID. */
static int group_before(const struct igmp_db *db, int below, uint32_t group,
			int dynamic_only)
{
	for (int i = 0; i < below; i++)
		if (db->g[i].in_use && db->g[i].group == group &&
		    !(dynamic_only && db->g[i].fixed))
			return 1;
	return 0;
}

/* Distinct groups, not (vid, group) entries: ME 311 and the OLT's limit are
 * per group, and the same group on two VLANs is one of them. */
int igmp_db_dynamic_count(const struct igmp_db *db)
{
	int n = 0;

	for (int i = 0; i < IGMP_MAX_GROUPS; i++)
		if (db->g[i].in_use && !db->g[i].fixed &&
		    !group_before(db, i, db->g[i].group, 1))
			n++;
	return n;
}

static int group_known(const struct igmp_db *db, uint32_t group)
{
	return group_before(db, IGMP_MAX_GROUPS, group, 0);
}

static struct igmp_hist *hist_find(struct igmp_db *db, uint16_t vid,
				   uint32_t group, uint8_t port)
{
	for (int i = 0; i < IGMP_HIST_MAX; i++)
		if (db->hist[i].used && db->hist[i].port == port &&
		    db->hist[i].vid == vid && db->hist[i].group == group)
			return &db->hist[i];
	return 0;
}

/* Note a report and return the membership interval it earns. The oldest
 * record makes room when the history is full. */
static uint32_t hist_report(struct igmp_db *db, uint16_t vid, uint32_t group,
			    uint8_t port, uint32_t now)
{
	struct igmp_hist *h = hist_find(db, vid, group, port);
	uint32_t ttl = igmp_policy_membership_ttl(&db->pol), obs;

	if (!h) {
		int pick = 0;

		for (int i = 0; i < IGMP_HIST_MAX; i++) {
			if (!db->hist[i].used) {
				pick = i;
				break;
			}
			if ((int32_t)(db->hist[pick].last - db->hist[i].last) > 0)
				pick = i;
		}
		h = &db->hist[pick];
		h->used = 1;
		h->port = port;
		h->vid = vid;
		h->group = group;
		h->gap = 0;
	} else {
		uint32_t gap = now - h->last;

		if (gap > IGMP_OBS_GAP_MAX)
			h->gap = 0;
		else if (gap > db->pol.query_max_response)
			h->gap = gap;
	}
	h->last = now;
	if (!h->gap)
		return ttl;
	obs = db->pol.robustness * h->gap + db->pol.query_max_response;
	if (obs > IGMP_OBS_TTL_MAX)
		obs = IGMP_OBS_TTL_MAX;
	return obs > ttl ? obs : ttl;
}

/* The host said it is leaving: what it reported before says nothing about
 * what it does next. */
static void hist_forget(struct igmp_db *db, uint16_t vid, uint32_t group,
			uint8_t port)
{
	struct igmp_hist *h = hist_find(db, vid, group, port);

	if (h)
		h->used = 0;
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

/* One refusal, into the counts and the ring (group.h says why the ring is
 * small). */
static void refuse(struct igmp_db *db, uint8_t reason, uint16_t vid,
		   uint32_t group, uint32_t now)
{
	struct igmp_refusal *r = 0;

	db->refused[reason]++;
	if (db->refusals) {
		r = &db->ring[(db->refusals - 1) % IGMP_REFUSAL_RING];
		if (r->reason != reason || r->vid != vid || r->group != group)
			r = 0;
	}
	if (r) {
		r->count++;
		r->at = now;
		return;
	}
	r = &db->ring[db->refusals % IGMP_REFUSAL_RING];
	r->reason = reason;
	r->vid = vid;
	r->group = group;
	r->at = now;
	r->count = 1;
	db->refusals++;
}

/* Whether joining `group` takes the imputed bandwidth of the groups held
 * past the profile's maximum. Each group counts once, whatever its VLANs;
 * static groups are not joins and count nothing. A group the lists impute
 * nothing to never does (G.988: 0 is outside the limit). */
static int bw_over(const struct igmp_db *db, uint32_t group)
{
	uint32_t want = igmp_policy_imputed(&db->pol, group), sum = 0;

	if (!db->pol.max_bandwidth || !want)
		return 0;
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		const struct igmp_entry *e = &db->g[i];
		uint32_t b;

		if (!e->in_use || e->fixed || group_before(db, i, e->group, 1))
			continue;
		b = igmp_policy_imputed(&db->pol, e->group);
		sum = sum + b < sum ? 0xffffffffu : sum + b;
	}
	return sum + want < sum || sum + want > db->pol.max_bandwidth;
}

/* The sources an IGMPv3 record names (RFC 3376 4.2.12), for the access
 * control lists' source match. A v1 or v2 report names none. */
struct join_src {
	int            has;
	int            exclude;
	const uint8_t *p;
	uint32_t       n;
};

static const struct join_src no_src = { 0, 0, 0, 0 };

/* Whether the allowed preview table (ME 310 attribute 7) grants this group
 * now: full access for the row's minutes, which begin with the first join
 * that uses it, or until the OLT takes the row out when it says 0. The IPv4
 * rows only; the source address a row names is carried, not matched (a group
 * the subscriber has paid for is paid for whoever sends it). */
static int grant_active(struct igmp_db *db, const struct igmp_join *q, uint32_t now)
{
	for (uint32_t i = 0; i < db->pol.ngrant; i++) {
		const struct igmp_grant *g = &db->pol.grant[i];

		if (g->group != q->group || g->uni != q->uni_vid)
			continue;
		if (g->ani && q->ani_known && g->ani != q->ani_vid)
			continue;
		if (!g->minutes)
			return 1;
		if (!db->grant_since[i])
			db->grant_since[i] = now ? now : 1;
		if ((int32_t)(now - db->grant_since[i]) < (int32_t)(g->minutes * 60u))
			return 1;
	}
	return 0;
}

/* Whether a row of the allowed preview table names this group and has not
 * run out, whatever the VLANs: for what a rewrite of the policy may keep.
 * A group admitted by the table alone is in no list. */
static int grant_covers(const struct igmp_db *db, uint32_t group)
{
	for (uint32_t i = 0; i < db->pol.ngrant; i++) {
		const struct igmp_grant *g = &db->pol.grant[i];

		if (g->group != group)
			continue;
		if (!g->minutes || !db->grant_since[i] ||
		    (int32_t)(db->last_now - db->grant_since[i]) < (int32_t)(g->minutes * 60u))
			return 1;
	}
	return 0;
}

static struct igmp_preview *preview_find(struct igmp_db *db, uint32_t group)
{
	for (int i = 0; i < IGMP_PREVIEW_MAX; i++)
		if (db->preview[i].used && db->preview[i].group == group)
			return &db->preview[i];
	return 0;
}

/* The preview ended at second `t`: the next may begin once the row's repeat
 * time has passed, counted from the end of this one. */
static void preview_finish(struct igmp_preview *s, uint32_t t)
{
	s->active = 0;
	s->has_next = 1;
	s->next_ok = t + s->repeat;
}

/* May this join of a preview-only group go in, and does it begin a preview.
 * G.988: a preview lasts the row's length; between two there are at least the
 * repeat time; the repeat count caps how many (0 is no cap) until the count
 * resets. The slot is the group's state, if it has one. */
static int preview_decide(struct igmp_db *db, uint32_t group,
			  const struct igmp_range *row, uint32_t now,
			  struct igmp_preview **slot, int *begins)
{
	struct igmp_preview *s = preview_find(db, group);

	*slot = s;
	*begins = 0;
	if (s && s->active) {
		if ((int32_t)(now - s->end) < 0)
			return 1;               /* the running preview goes on */
		preview_finish(s, s->end);
	}
	if (s) {
		/* "Do not reset" (0) and the OLT's own reset (255, an action
		 * this does not see) leave the count alone; anything else is
		 * a daily reset, at no fixed time of day (the ONU has no
		 * clock to name one: G.988 allows an indeterminate time). */
		if (row->pv_reset != 0 && row->pv_reset != 255 && s->count &&
		    (int32_t)(now - s->window) >= IGMP_PREVIEW_WINDOW)
			s->count = 0;
		if (s->has_next && (int32_t)(now - s->next_ok) < 0)
			return 0;
		if (row->pv_count && s->count >= row->pv_count)
			return 0;
	}
	*begins = 1;
	return 1;
}

/* A preview begins: the group's slot, made if there is none. A table with no
 * free or finished slot lets the preview run unmetered. */
static void preview_begin(struct igmp_db *db, struct igmp_preview *s,
			  uint32_t group, const struct igmp_range *row, uint32_t now)
{
	if (!s) {
		for (int i = 0; i < IGMP_PREVIEW_MAX && !s; i++)
			if (!db->preview[i].used)
				s = &db->preview[i];
		for (int i = 0; i < IGMP_PREVIEW_MAX && !s; i++)
			if (!db->preview[i].active)
				s = &db->preview[i];
		if (!s)
			return;
		for (unsigned k = 0; k < sizeof *s; k++)
			((uint8_t *)s)[k] = 0;
		s->used = 1;
		s->group = group;
	}
	if (!s->count)
		s->window = now;
	s->count++;
	s->active = 1;
	s->has_next = 0;
	s->repeat = row->pv_repeat;
	s->end = now + row->pv_len;
}

/* The last entry of a group is gone: its preview is over, whatever its
 * length said. */
static void preview_gone(struct igmp_db *db, uint32_t group, uint32_t now)
{
	struct igmp_preview *s = preview_find(db, group);

	if (s && s->active && !group_known(db, group))
		preview_finish(s, now);
}

/* A port joins, or refreshes what it already had. */
static int join(struct igmp_db *db, uint16_t vid, uint32_t group, uint8_t port,
		uint32_t now, struct igmp_change *out, int max, int n,
		const struct join_src *src)
{
	struct igmp_entry *e;
	struct igmp_join q;
	struct igmp_preview *pv = 0;
	const struct igmp_range *row = 0;
	int verdict, begins = 0;
	uint32_t bit, before;

	if (port == 0 || port > IGMP_MAX_PORTS || !igmp_group_snoopable(group)) {
		db->rx_passable++;
		return n;
	}
	/* The profile decides. A join it does not allow gets no state, so the
	 * group is unregistered and, with the PON's drop on, the stream never
	 * reaches the host: the ONU-side enforcement the access control lists
	 * ask for. Whether the report itself still goes upstream is the
	 * profile's unauthorized join behaviour: with the copy alone the switch
	 * forwards it whatever this says; with the report taken (uplink.h) a
	 * message of refused joins alone is withheld unless the profile
	 * forwards them. */
	q.group = group;
	q.uni_vid = vid;
	q.has_src = src->has;
	q.exclude = src->exclude;
	q.srcs = src->p;
	q.nsrc = src->n;
	igmp_policy_ani(&db->pol, &q);
	verdict = igmp_policy_join(&db->pol, &q, &row);
	/* The search order of G.988 (9.3.28 attribute 7): the profile's fully
	 * authorized groups, then the allowed preview table, then its
	 * preview-only groups. */
	if (verdict != IGMP_JOIN_FULL && grant_active(db, &q, now))
		verdict = IGMP_JOIN_FULL;
	if (verdict == IGMP_JOIN_DENY) {
		db->denied++;
		db->rx_acl_refused++;
		refuse(db, IGMP_REFUSE_ACL, vid, group, now);
		return n;
	}
	if (verdict == IGMP_JOIN_PREVIEW &&
	    !preview_decide(db, group, row, now, &pv, &begins)) {
		db->denied++;
		db->rx_acl_refused++;
		refuse(db, IGMP_REFUSE_PREVIEW, vid, group, now);
		return n;
	}
	db->rx_passable++;
	e = find(db, vid, group);
	/* The limit is on groups: a group already held on another VLAN is not a
	 * new one, and neither is a static one. */
	if (!e && db->pol.max_groups && !group_known(db, group) &&
	    (uint32_t)igmp_db_dynamic_count(db) >= db->pol.max_groups) {
		db->denied++;
		refuse(db, IGMP_REFUSE_LIMIT, vid, group, now);
		return n;
	}
	/* The bandwidth is on new groups too, by what the lists impute to
	 * each (G.988 9.3.28 attributes 4 and 5): a join that would take the
	 * total past the maximum is counted, and refused only when the profile
	 * enforces it. A group already held is never refused, so no stream
	 * that plays is cut, not even when the maximum is lowered under it. */
	if (!e && !group_known(db, group) && bw_over(db, group)) {
		db->bw_exceeded++;
		if (db->pol.bw_enforce) {
			db->denied++;
			refuse(db, IGMP_REFUSE_BW, vid, group, now);
			return n;
		}
	}
	if (!e)
		e = find_or_add(db, vid, group);
	if (!e) {
		/* Not in denied: that is the profile's refusals, the ME 311
		 * figure; a full table is igmpd's own limit. */
		refuse(db, IGMP_REFUSE_FULL, vid, group, now);
		return n;
	}
	/* The preview begins only for a join that goes in: one the limit or a
	 * full table refused used none of the count. */
	if (begins)
		preview_begin(db, pv, group, row, now);
	/* Counted here, past every refusal: ME 311 attribute 3 is the joins
	 * that were accepted (G.988 9.3.29), and a refused one is in denied. */
	db->joins++;
	bit = IGMP_PORTBIT(port);
	before = e->ports;
	e->ports |= bit;
	e->expires[port - 1] = now + hist_report(db, vid, group, port, now);
	/* Only a change of membership is a change. A refresh moves an expiry
	 * and nothing the switch can see, and reprogramming the same mask on
	 * every report would be a write per report per group. */
	if (e->ports != before)
		n = emit(out, max, n, IGMP_CHANGE_SET, vid, group, e->ports);
	return n;
}

/* A leave shortens the membership rather than ending it -- group.h says why.
 *
 * What a snooper owes a leave without immediate leave (RFC 4541 2.1.1, G.988
 * 9.3.26 attribute 8) is a group-specific query out of the port and a wait for
 * an answer. igmpd cannot transmit yet, so it does not query. What gives the
 * same outcome here: the host's leave also goes upstream, the OLT sends its
 * own group-specific query, and as the row stays programmed through the wait
 * the query reaches every listener on the port; one that still wants the group
 * answers, and its report (a join below) restores the full interval. If none
 * answers within robustness x last-member interval the port goes. The cost is
 * that this relies on the OLT querying: with an OLT that does not, a second
 * set-top box behind the same port is cut off until its next report. */
static int leave(struct igmp_db *db, uint16_t vid, uint32_t group, uint8_t port,
		 uint32_t now, struct igmp_change *out, int max, int n)
{
	struct igmp_entry *e = find(db, vid, group);
	uint32_t deadline;

	db->rx_passable++;
	if (!e || port == 0 || port > IGMP_MAX_PORTS || e->fixed)
		return n;
	if (!(e->ports & IGMP_PORTBIT(port)))
		return n;
	hist_forget(db, vid, group, port);
	if (db->pol.immediate_leave) {
		e->ports &= ~IGMP_PORTBIT(port);
		if (e->ports) {
			n = emit(out, max, n, IGMP_CHANGE_SET, vid, group, e->ports);
		} else {
			n = emit(out, max, n, IGMP_CHANGE_DEL, vid, group, 0);
			e->in_use = 0;
			preview_gone(db, group, now);
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

	db->last_now = now;
	db->rx_acl_refused = 0;
	db->rx_passable = 0;
	/* A message above the profile's IGMP version (G.988 9.3.27 attribute
	 * 1) is noted and otherwise handled as it always was. */
	if (!igmp_policy_version_ok(&db->pol, rx->type))
		db->above_version++;
	switch (rx->type) {
	case IGMP_QUERY:
		/* Any query makes the port it arrived on a router port -- a
		 * group-specific one as much as a general one, because both
		 * come from whatever is acting as querier. */
		n = router_seen(db, rx->port, now, out, max, n);
		break;

	case IGMP_V1_REPORT:
	case IGMP_V2_REPORT:
		n = join(db, rx->vid, rx->group, rx->port, now, out, max, n, &no_src);
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
				 ((t == 1 || t == 3 || t == 5) && nsrc > 0)) {
				struct join_src js;

				/* The sources are for the lists' source match.
				 * The rest of the table still treats the record
				 * as a join for the group. */
				js.has = nsrc > 0;
				js.exclude = t == 2 || t == 4;
				js.p = p + 8;
				js.n = nsrc;
				n = join(db, rx->vid, grp, rx->port, now,
					 out, max, n, &js);
			}
			else
				db->rx_passable++;
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

	db->last_now = now;
	/* A preview that has run its length is over: the group goes whatever
	 * the host keeps reporting, and its next join is judged by the repeat
	 * time and count. */
	for (int i = 0; i < IGMP_PREVIEW_MAX; i++) {
		struct igmp_preview *pv = &db->preview[i];

		if (!pv->used || !pv->active || (int32_t)(now - pv->end) < 0)
			continue;
		preview_finish(pv, pv->end);
		for (int k = 0; k < IGMP_MAX_GROUPS; k++) {
			struct igmp_entry *e = &db->g[k];

			if (!e->in_use || e->fixed || e->group != pv->group)
				continue;
			for (int p = 0; p < IGMP_MAX_PORTS; p++)
				if (e->ports & ((uint32_t)1 << p))
					hist_forget(db, e->vid, e->group, (uint8_t)(p + 1));
			n = emit(out, max, n, IGMP_CHANGE_DEL, e->vid, e->group, 0);
			e->in_use = 0;
			e->ports = 0;
		}
	}

	/* History is only worth keeping while the host could still come back
	 * with a gap worth measuring. */
	for (int i = 0; i < IGMP_HIST_MAX; i++)
		if (db->hist[i].used &&
		    (int32_t)(now - db->hist[i].last) >
		    IGMP_OBS_TTL_MAX + IGMP_OBS_GAP_MAX)
			db->hist[i].used = 0;

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
			preview_gone(db, e->group, now);
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
	uint32_t since[IGMP_POLICY_MAX_GRANTS];

	/* The allowed preview rows the OLT leaves in place keep the minutes
	 * they have run; a new one begins its minutes now (the first join that
	 * uses it, if the clock is not known yet). The OLT lengthening a row
	 * moves its end, which is what writing a new duration does. */
	for (uint32_t i = 0; i < pol->ngrant; i++) {
		since[i] = db->last_now;
		for (uint32_t k = 0; k < db->pol.ngrant; k++)
			if (db->pol.grant[k].group == pol->grant[i].group &&
			    db->pol.grant[k].ani == pol->grant[i].ani &&
			    db->pol.grant[k].uni == pol->grant[i].uni)
				since[i] = db->grant_since[k];
	}
	db->pol = *pol;
	for (uint32_t i = 0; i < pol->ngrant; i++)
		db->grant_since[i] = since[i];
	for (uint32_t i = pol->ngrant; i < IGMP_POLICY_MAX_GRANTS; i++)
		db->grant_since[i] = 0;
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

		if (!e->in_use || e->fixed || igmp_policy_allows(pol, e->group) ||
		    grant_covers(db, e->group))
			continue;
		n = emit(out, max, n, IGMP_CHANGE_DEL, e->vid, e->group, 0);
		e->in_use = 0;
		e->ports = 0;
	}

	/* A preview the lists no longer call one (the group is fully
	 * authorized now, or gone from them) stops being timed. */
	for (int i = 0; i < IGMP_PREVIEW_MAX; i++) {
		struct igmp_preview *pv = &db->preview[i];
		struct igmp_join q;
		const struct igmp_range *row;

		if (!pv->used || !pv->active)
			continue;
		q.group = pv->group;
		q.uni_vid = 0;
		q.ani_known = 0;
		q.ani_vid = 0;
		q.has_src = q.exclude = 0;
		q.srcs = 0;
		q.nsrc = 0;
		if (igmp_policy_join(pol, &q, &row) != IGMP_JOIN_PREVIEW)
			pv->active = 0;
	}
	return n;
}
