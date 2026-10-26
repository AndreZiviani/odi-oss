/* MLD snooping, the IPv6 half of igmpd -- see mld.h. No syscalls. */
#include "mld.h"
#include "odi_sw_ioctl.h"

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void zero(void *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		((uint8_t *)p)[i] = 0;
}

static void copy16(uint8_t *d, const uint8_t *s)
{
	for (int i = 0; i < 16; i++)
		d[i] = s[i];
}

static int same16(const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < 16; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* a < b, a == b, a > b as -1, 0, 1: the addresses in network order compare
 * as numbers byte by byte. */
static int cmp16(const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < 16; i++)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

/* ---- the frame ------------------------------------------------------------ */

uint16_t mld_cksum(const uint8_t src[16], const uint8_t dst[16], const uint8_t *msg,
		   uint32_t len)
{
	uint32_t sum = 0, i;

	/* The pseudo-header: source, destination, the upper-layer length as
	 * 32 bits, three zero bytes and the next header, 58. */
	for (i = 0; i < 16; i += 2)
		sum += rd16(src + i) + rd16(dst + i);
	sum += len >> 16;
	sum += len & 0xffff;
	sum += 58;
	for (i = 0; i + 1 < len; i += 2)
		sum += rd16(msg + i);
	if (i < len)
		sum += (uint32_t)msg[i] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

const char *mld_parse_str(enum mld_parse_rc rc)
{
	switch (rc) {
	case MLD_OK:       return "ok";
	case MLD_SHORT:    return "truncated";
	case MLD_NOT_IPV6: return "not IPv6";
	case MLD_BAD_IP:   return "malformed IPv6 header";
	case MLD_NOT_MLD:  return "not MLD";
	case MLD_BAD_MLD:  return "malformed MLD message";
	case MLD_CKSUM:    return "ICMPv6 checksum";
	}
	return "?";
}

const char *mld_type_str(uint8_t type)
{
	switch (type) {
	case MLD_QUERY:     return "query";
	case MLD_V1_REPORT: return "v1 report";
	case MLD_V1_DONE:   return "done";
	case MLD_V2_REPORT: return "v2 report";
	}
	return "unknown";
}

/* The hop-by-hop options, for the router alert (option 5). Recorded, not
 * required, for the reason frame.c gives for the IPv4 one. */
static int hbh_router_alert(const uint8_t *opt, uint32_t len)
{
	uint32_t i = 0;

	while (i < len) {
		if (opt[i] == 0) {              /* Pad1 */
			i++;
			continue;
		}
		if (i + 1 >= len)
			return 0;
		if (opt[i] == 5)
			return 1;
		i += 2u + opt[i + 1];
	}
	return 0;
}

enum mld_parse_rc mld_parse(const uint8_t *buf, uint32_t len, struct mld_rx *rx)
{
	const uint8_t *eth, *ip;
	uint32_t eth_len, off, plen, end, ext = 0;
	uint16_t ethertype;
	uint8_t nh;

	zero(rx, sizeof *rx);
	if (len < IGMP_RX_PREFIX + ETH_HDR_LEN)
		return MLD_SHORT;
	rx->port = buf[0];
	rx->vid = (uint16_t)(((uint16_t)(buf[1] & 0x0f) << 8) | buf[2]);
	rx->taken = (buf[1] & IGMP_RX_TAKEN) != 0;

	eth = buf + IGMP_RX_PREFIX;
	eth_len = len - IGMP_RX_PREFIX;
	rx->eth = eth;
	rx->eth_len = eth_len;
	rx->dst = eth;
	rx->src = eth + 6;

	/* One C-tag at most, read as frame.c reads it for IGMP. */
	ethertype = rd16(eth + 12);
	off = ETH_HDR_LEN;
	if (ethertype == ETH_P_8021Q) {
		if (eth_len < ETH_HDR_LEN + 4)
			return MLD_SHORT;
		rx->tagged = 1;
		rx->vid = (uint16_t)(rd16(eth + 14) & 0x0fff);
		ethertype = rd16(eth + 16);
		off = ETH_HDR_LEN + 4;
	}
	if (ethertype != ETH_P_IPV6)
		return MLD_NOT_IPV6;
	if (eth_len < off + 40)
		return MLD_SHORT;
	ip = eth + off;
	if ((ip[0] >> 4) != 6)
		return MLD_BAD_IP;
	plen = rd16(ip + 4);
	/* Ethernet may pad; a payload past the frame is a truncation. */
	if (off + 40 + plen > eth_len)
		return MLD_SHORT;
	copy16(rx->sip, ip + 8);
	copy16(rx->dip, ip + 24);
	rx->hop_limit = ip[7];

	nh = ip[6];
	off = 40;
	end = 40 + plen;
	while (nh == 0 || nh == 43 || nh == 60) {
		uint32_t hl;

		if (++ext > MLD_EXT_MAX || off + 2 > end)
			return MLD_BAD_IP;
		hl = 8u * ((uint32_t)ip[off + 1] + 1u);
		if (off + hl > end)
			return MLD_BAD_IP;
		if (nh == 0 && hbh_router_alert(ip + off + 2, hl - 2))
			rx->router_alert = 1;
		nh = ip[off];
		off += hl;
	}
	if (nh != 58)
		return MLD_NOT_MLD;
	rx->body = ip + off;
	rx->body_len = end - off;
	if (rx->body_len < 4)
		return MLD_BAD_MLD;
	rx->type = rx->body[0];
	if (rx->type != MLD_QUERY && rx->type != MLD_V1_REPORT &&
	    rx->type != MLD_V1_DONE && rx->type != MLD_V2_REPORT)
		return MLD_NOT_MLD;
	if (mld_cksum(rx->sip, rx->dip, rx->body, rx->body_len) != 0)
		return MLD_CKSUM;
	if (rx->type == MLD_V2_REPORT) {
		if (rx->body_len < 8)
			return MLD_BAD_MLD;
		/* The records themselves are the table's to walk; here only
		 * that the shortest records of that count fit. */
		rx->nrec = rd16(rx->body + 6);
		if ((uint32_t)rx->nrec * 20 + 8 > rx->body_len)
			return MLD_BAD_MLD;
	} else {
		if (rx->body_len < 24)
			return MLD_BAD_MLD;
		copy16(rx->group, rx->body + 8);
	}
	return MLD_OK;
}

int mld_group_snoopable(const uint8_t g[16])
{
	uint8_t scope = g[1] & 0x0f;

	return g[0] == 0xff && scope >= 3 && scope <= 0xe;
}

void mld_group_mac(uint8_t mac[6], const uint8_t g[16])
{
	mac[0] = 0x33;
	mac[1] = 0x33;
	mac[2] = g[12];
	mac[3] = g[13];
	mac[4] = g[14];
	mac[5] = g[15];
}

/* ---- IPv6 text ------------------------------------------------------------ */

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

int mld_parse_ipv6(const char *s, uint32_t n, uint8_t out[16])
{
	uint16_t f[8];
	int nf = 0, gap = -1;
	uint32_t i = 0;

	if (n < 2)
		return 0;
	if (s[0] == ':') {
		if (s[1] != ':')
			return 0;
		gap = 0;
		i = 2;
	}
	while (i < n) {
		uint32_t v = 0;
		int digits = 0;

		while (i < n && hexval(s[i]) >= 0) {
			if (++digits > 4)
				return 0;
			v = v << 4 | (uint32_t)hexval(s[i++]);
		}
		if (!digits)
			return 0;
		if (nf == 8)
			return 0;
		f[nf++] = (uint16_t)v;
		if (i == n)
			break;
		if (s[i++] != ':' || i == n)
			return 0;
		if (s[i] == ':') {
			if (gap >= 0)
				return 0;
			gap = nf;
			if (++i == n)
				break;
		}
	}
	if (gap < 0 ? nf != 8 : nf > 7)
		return 0;
	for (int k = 0; k < 16; k++)
		out[k] = 0;
	{
		int tail = gap < 0 ? 0 : nf - gap, head = nf - tail;

		for (int k = 0; k < head; k++) {
			out[2 * k] = (uint8_t)(f[k] >> 8);
			out[2 * k + 1] = (uint8_t)f[k];
		}
		for (int k = 0; k < tail; k++) {
			int at = 8 - tail + k;

			out[2 * at] = (uint8_t)(f[head + k] >> 8);
			out[2 * at + 1] = (uint8_t)f[head + k];
		}
	}
	return 1;
}

uint32_t mld_format_ipv6(char *b, const uint8_t a[16])
{
	static const char hex[] = "0123456789abcdef";
	int best = -1, best_len = 1, run = 0;
	uint32_t at = 0;

	/* The longest run of zero fields, the first of equal ones, two or
	 * more long (RFC 5952 4.2). */
	for (int k = 0; k < 8; k++) {
		if (!a[2 * k] && !a[2 * k + 1]) {
			if (++run > best_len) {
				best_len = run;
				best = k - run + 1;
			}
		} else {
			run = 0;
		}
	}
	for (int k = 0; k < 8; k++) {
		uint16_t v = (uint16_t)(a[2 * k] << 8 | a[2 * k + 1]);
		int lead = 1;

		if (k == best) {
			b[at++] = ':';
			if (k == 0)
				b[at++] = ':';
			k += best_len - 1;
			continue;
		}
		for (int sh = 12; sh >= 0; sh -= 4) {
			uint8_t d = (uint8_t)((v >> sh) & 0xf);

			if (d || !lead || !sh) {
				b[at++] = hex[d];
				lead = 0;
			}
		}
		if (k < 7)
			b[at++] = ':';
	}
	b[at] = 0;
	return at;
}

/* ---- the IPv6 lists ------------------------------------------------------- */

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

static int range6(const char *s, uint32_t n, struct mld_range *r)
{
	uint32_t i = 0;

	while (i < n && s[i] != '-')
		i++;
	if (i == n)
		return 0;
	if (!mld_parse_ipv6(s, i, r->lo) || !mld_parse_ipv6(s + i + 1, n - i - 1, r->hi))
		return 0;
	return cmp16(r->lo, r->hi) <= 0;
}

int mld_policy_parse(const char *text, uint32_t len, struct mld_policy *p)
{
	uint32_t at = 0;
	int understood = 0;

	zero(p, sizeof *p);
	while (at < len) {
		const char *l = text + at, *val;
		uint32_t n = 0, eq, vn;
		struct mld_range r;

		while (at + n < len && l[n] != '\n')
			n++;
		at += n + 1;
		for (eq = 0; eq < n && l[eq] != '='; eq++)
			;
		if (n == 0 || l[0] == '#' || eq == n)
			continue;
		val = l + eq + 1;
		vn = n - eq - 1;
		if (is(l, eq, "allow6_overflow") && is(val, vn, "1")) {
			p->overflow = 1;
		} else if (is(l, eq, "allow6") && range6(val, vn, &r)) {
			if (p->nallow < IGMP_POLICY_MAX_RANGES)
				p->allow[p->nallow++] = r;
			else
				p->overflow = 1;
		} else if (is(l, eq, "static6") && range6(val, vn, &r)) {
			if (p->nstatic < IGMP_POLICY_MAX_RANGES)
				p->stat[p->nstatic++] = r;
			else
				p->overflow = 1;
		} else {
			continue;
		}
		understood++;
	}
	return understood;
}

int mld_policy_acl_active(const struct mld_policy *p)
{
	return !p->overflow && (p->nallow || p->nstatic);
}

static int in_ranges(const struct mld_range *r, uint32_t n, const uint8_t g[16])
{
	for (uint32_t i = 0; i < n; i++)
		if (cmp16(g, r[i].lo) >= 0 && cmp16(g, r[i].hi) <= 0)
			return 1;
	return 0;
}

int mld_policy_allows(const struct mld_policy *p, const uint8_t g[16])
{
	if (!mld_policy_acl_active(p))
		return 1;
	return in_ranges(p->allow, p->nallow, g) || in_ranges(p->stat, p->nstatic, g);
}

int mld_policy_takes(const struct igmp_policy *ip, const struct mld_policy *p)
{
	return ip->snooping && mld_policy_acl_active(p) && !ip->unauthorized_join;
}

uint32_t mld_policy_lease(const struct igmp_policy *ip, const struct mld_policy *p,
			  int static_overflow, uint32_t now, uint32_t learn_until,
			  int pon_drop)
{
	if (!ip->snooping)
		return 0;
	/* The same three conditions as the IPv4 drop (policy.c), on the IPv6
	 * lists. */
	if (!pon_drop || (int32_t)(now - learn_until) < 0 || static_overflow || p->overflow)
		return ODI_SW_MC_SNOOP_MLD;
	return ODI_SW_MC_SNOOP_MLD | ODI_SW_MC_SNOOP_PON_DROP6;
}

/* ---- the IPv6 group table ------------------------------------------------- */

void mld_db_init(struct mld_db *db)
{
	zero(db, sizeof *db);
	igmp_policy_defaults(&db->prof);
}

int mld_db_count(const struct mld_db *db)
{
	int n = 0;

	for (int i = 0; i < MLD_MAX_GROUPS; i++)
		if (db->g[i].in_use)
			n++;
	return n;
}

static int group_before(const struct mld_db *db, int below, const uint8_t g[16],
			int dynamic_only)
{
	for (int i = 0; i < below; i++)
		if (db->g[i].in_use && same16(db->g[i].group, g) &&
		    !(dynamic_only && db->g[i].fixed))
			return 1;
	return 0;
}

int mld_db_dynamic_count(const struct mld_db *db)
{
	int n = 0;

	for (int i = 0; i < MLD_MAX_GROUPS; i++)
		if (db->g[i].in_use && !db->g[i].fixed &&
		    !group_before(db, i, db->g[i].group, 1))
			n++;
	return n;
}

static struct mld_entry *find(struct mld_db *db, uint16_t vid, const uint8_t g[16])
{
	for (int i = 0; i < MLD_MAX_GROUPS; i++)
		if (db->g[i].in_use && db->g[i].vid == vid && same16(db->g[i].group, g))
			return &db->g[i];
	return 0;
}

static struct mld_entry *find_or_add(struct mld_db *db, uint16_t vid, const uint8_t g[16])
{
	struct mld_entry *e = find(db, vid, g);

	if (e)
		return e;
	for (int i = 0; i < MLD_MAX_GROUPS; i++) {
		if (!db->g[i].in_use) {
			e = &db->g[i];
			zero(e, sizeof *e);
			e->in_use = 1;
			e->vid = vid;
			copy16(e->group, g);
			return e;
		}
	}
	return 0;
}

static int emit(struct mld_change *out, int max, int n, uint8_t kind, uint16_t vid,
		const uint8_t *g, uint32_t ports)
{
	if (n < max) {
		out[n].kind = kind;
		out[n].vid = vid;
		if (g)
			copy16(out[n].group, g);
		else
			zero(out[n].group, 16);
		out[n].ports = ports;
	}
	return n + 1;
}

static void refuse(struct mld_db *db, uint8_t reason, uint16_t vid, const uint8_t g[16],
		   uint32_t now)
{
	struct mld_refusal *r = 0;

	db->refused[reason]++;
	if (db->refusals) {
		r = &db->ring[(db->refusals - 1) % IGMP_REFUSAL_RING];
		if (r->reason != reason || r->vid != vid || !same16(r->group, g))
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
	copy16(r->group, g);
	r->at = now;
	r->count = 1;
	db->refusals++;
}

/* A port joins, or refreshes what it had: as join() in group.c, the lists,
 * then the limit, then a free entry. */
static int join(struct mld_db *db, uint16_t vid, const uint8_t g[16], uint8_t port,
		uint32_t now, uint32_t others, struct mld_change *out, int max, int n)
{
	struct mld_entry *e;
	uint32_t before;

	/* A reserved or link-local group is no business of the snooper: not
	 * kept, not refused, flooded by the switch. */
	if (port == 0 || port > IGMP_MAX_PORTS || !mld_group_snoopable(g)) {
		db->rx_passable++;
		return n;
	}
	if (!mld_policy_allows(&db->pol, g)) {
		db->denied++;
		db->rx_acl_refused++;
		refuse(db, IGMP_REFUSE_ACL, vid, g, now);
		return n;
	}
	db->rx_passable++;
	e = find(db, vid, g);
	if (!e && db->prof.max_groups && !group_before(db, MLD_MAX_GROUPS, g, 0) &&
	    others + (uint32_t)mld_db_dynamic_count(db) >= db->prof.max_groups) {
		db->denied++;
		refuse(db, IGMP_REFUSE_LIMIT, vid, g, now);
		return n;
	}
	if (!e)
		e = find_or_add(db, vid, g);
	if (!e) {
		refuse(db, IGMP_REFUSE_FULL, vid, g, now);
		return n;
	}
	db->joins++;
	before = e->ports;
	e->ports |= IGMP_PORTBIT(port);
	e->expires[port - 1] = now + igmp_policy_membership_ttl(&db->prof);
	if (e->ports != before)
		n = emit(out, max, n, IGMP_CHANGE_SET, vid, g, e->ports);
	return n;
}

/* A done (or a v2 record that stops listening) shortens the membership to
 * the last-member time, or ends it with an immediate leave: group.c says
 * why. */
static int leave(struct mld_db *db, uint16_t vid, const uint8_t g[16], uint8_t port,
		 uint32_t now, struct mld_change *out, int max, int n)
{
	struct mld_entry *e = find(db, vid, g);
	uint32_t deadline;

	db->rx_passable++;
	if (!e || port == 0 || port > IGMP_MAX_PORTS || e->fixed ||
	    !(e->ports & IGMP_PORTBIT(port)))
		return n;
	if (db->prof.immediate_leave) {
		e->ports &= ~IGMP_PORTBIT(port);
		if (e->ports) {
			n = emit(out, max, n, IGMP_CHANGE_SET, vid, g, e->ports);
		} else {
			n = emit(out, max, n, IGMP_CHANGE_DEL, vid, g, 0);
			e->in_use = 0;
		}
		return n;
	}
	deadline = now + igmp_policy_last_member_ttl(&db->prof);
	if (e->expires[port - 1] > deadline)
		e->expires[port - 1] = deadline;
	return n;
}

static int router_seen(struct mld_db *db, uint8_t port, uint32_t now,
		       struct mld_change *out, int max, int n)
{
	uint32_t before = db->router_ports;

	if (port == 0 || port > IGMP_MAX_PORTS)
		return n;
	db->router_ports |= IGMP_PORTBIT(port);
	db->router_expires[port - 1] = now + igmp_policy_membership_ttl(&db->prof);
	if (db->router_ports != before)
		n = emit(out, max, n, IGMP_CHANGE_ROUTER, 0, 0, db->router_ports);
	return n;
}

int mld_db_rx(struct mld_db *db, const struct mld_rx *rx, uint32_t now, uint32_t others,
	      struct mld_change *out, int max)
{
	int n = 0;

	db->rx_acl_refused = 0;
	db->rx_passable = 0;
	switch (rx->type) {
	case MLD_QUERY:
		n = router_seen(db, rx->port, now, out, max, n);
		break;
	case MLD_V1_REPORT:
		n = join(db, rx->vid, rx->group, rx->port, now, others, out, max, n);
		break;
	case MLD_V1_DONE:
		n = leave(db, rx->vid, rx->group, rx->port, now, out, max, n);
		break;
	case MLD_V2_REPORT: {
		/* RFC 3810 5.2.4: type 1, aux length 1, sources 2, the address
		 * 16, the sources, the auxiliary data. The record types are
		 * IGMPv3's, and so is their reduction to joins and leaves
		 * (group.c, RFC 4541 2.1.1). */
		const uint8_t *p = rx->body + 8;
		uint32_t left = rx->body_len - 8;

		for (uint16_t i = 0; i < rx->nrec; i++) {
			uint32_t nsrc, len;
			uint8_t t;

			if (left < 20)
				break;
			nsrc = rd16(p + 2);
			len = 20 + nsrc * 16 + (uint32_t)p[1] * 4;
			if (len > left)
				break;
			t = p[0];
			if ((t == 1 || t == 3) && nsrc == 0)
				n = leave(db, rx->vid, p + 4, rx->port, now, out, max, n);
			else if (t == 2 || t == 4 || ((t == 1 || t == 3 || t == 5) && nsrc > 0))
				n = join(db, rx->vid, p + 4, rx->port, now, others, out, max, n);
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

int mld_db_tick(struct mld_db *db, uint32_t now, struct mld_change *out, int max)
{
	int n = 0;
	uint32_t before;

	for (int i = 0; i < MLD_MAX_GROUPS; i++) {
		struct mld_entry *e = &db->g[i];

		if (!e->in_use || e->fixed)
			continue;
		before = e->ports;
		for (int p = 0; p < IGMP_MAX_PORTS; p++)
			if ((e->ports & ((uint32_t)1 << p)) && (int32_t)(now - e->expires[p]) >= 0)
				e->ports &= ~((uint32_t)1 << p);
		if (e->ports == before)
			continue;
		if (!e->ports) {
			n = emit(out, max, n, IGMP_CHANGE_DEL, e->vid, e->group, 0);
			e->in_use = 0;
		} else {
			n = emit(out, max, n, IGMP_CHANGE_SET, e->vid, e->group, e->ports);
		}
	}
	before = db->router_ports;
	for (int p = 0; p < IGMP_MAX_PORTS; p++)
		if ((db->router_ports & ((uint32_t)1 << p)) &&
		    (int32_t)(now - db->router_expires[p]) >= 0)
			db->router_ports &= ~((uint32_t)1 << p);
	if (db->router_ports != before)
		n = emit(out, max, n, IGMP_CHANGE_ROUTER, 0, 0, db->router_ports);
	return n;
}

uint32_t mld_db_mac_ports(const struct mld_db *db, const uint8_t g[16])
{
	uint32_t ports = 0;

	for (int i = 0; i < MLD_MAX_GROUPS; i++)
		if (db->g[i].in_use && db->g[i].group[12] == g[12] &&
		    db->g[i].group[13] == g[13] && db->g[i].group[14] == g[14] &&
		    db->g[i].group[15] == g[15])
			ports |= db->g[i].ports;
	return ports;
}

/* A static range is named group by group, up to IGMP_STATIC_RANGE_MAX of
 * them: the widest the hardware is asked to hold, as for IPv4. Its size, or
 * 0 when it is wider. */
static uint32_t range_size(const struct mld_range *r)
{
	uint32_t d;

	for (int i = 0; i < 14; i++)
		if (r->lo[i] != r->hi[i])
			return 0;
	d = (uint32_t)rd16(r->hi + 14) - rd16(r->lo + 14);
	return d < IGMP_STATIC_RANGE_MAX ? d + 1 : 0;
}

static int fix(struct mld_db *db, const uint8_t g[16], struct mld_change *out, int max, int n)
{
	struct mld_entry *e;
	uint32_t before;

	if (!mld_group_snoopable(g))
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

int mld_db_set_policy(struct mld_db *db, const struct igmp_policy *ip,
		      const struct mld_policy *p, struct mld_change *out, int max)
{
	int n = 0;

	db->prof = *ip;
	db->pol = *p;
	db->static_overflow = 0;
	for (uint32_t r = 0; r < p->nstatic; r++) {
		uint32_t size = range_size(&p->stat[r]);
		uint8_t g[16];

		if (!size) {
			db->static_overflow = 1;
			continue;
		}
		copy16(g, p->stat[r].lo);
		for (uint32_t k = 0; k < size; k++) {
			uint16_t low = (uint16_t)(rd16(p->stat[r].lo + 14) + k);

			g[14] = (uint8_t)(low >> 8);
			g[15] = (uint8_t)low;
			n = fix(db, g, out, max, n);
		}
	}
	/* Fixed groups no longer listed are ordinary again (group.c says
	 * why that lets them lapse). */
	for (int i = 0; i < MLD_MAX_GROUPS; i++) {
		struct mld_entry *e = &db->g[i];
		int listed = 0;

		if (!e->in_use || !e->fixed)
			continue;
		for (uint32_t r = 0; r < p->nstatic; r++)
			if (range_size(&p->stat[r]) && in_ranges(&p->stat[r], 1, e->group))
				listed = 1;
		if (!listed)
			e->fixed = 0;
	}
	for (int i = 0; i < MLD_MAX_GROUPS; i++) {
		struct mld_entry *e = &db->g[i];

		if (!e->in_use || e->fixed || mld_policy_allows(p, e->group))
			continue;
		n = emit(out, max, n, IGMP_CHANGE_DEL, e->vid, e->group, 0);
		e->in_use = 0;
		e->ports = 0;
	}
	return n;
}

/* ---- the state file ------------------------------------------------------- */

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

static uint32_t put_g(char *b, uint32_t at, uint32_t max, const uint8_t g[16])
{
	char t[40];

	mld_format_ipv6(t, g);
	return put_s(b, at, max, t);
}

static const char *reason_name(uint32_t r)
{
	switch (r) {
	case IGMP_REFUSE_ACL:   return "acl";
	case IGMP_REFUSE_LIMIT: return "limit";
	case IGMP_REFUSE_FULL:  return "full";
	}
	return "?";
}

uint32_t mld_state_format(const struct mld_db *db, char *buf, uint32_t max)
{
	uint32_t at = 0;

	at = put_kv(buf, at, max, "joins", db->joins);
	at = put_kv(buf, at, max, "denied", db->denied);
	at = put_kv(buf, at, max, "groups", (uint32_t)mld_db_dynamic_count(db));
	for (uint32_t r = 1; r < IGMP_REFUSE_REASONS; r++) {
		if (!db->refused[r])
			continue;
		at = put_s(buf, at, max, "refused_");
		at = put_kv(buf, at, max, reason_name(r), db->refused[r]);
	}
	for (int i = 0; i < MLD_MAX_GROUPS; i++) {
		const struct mld_entry *e = &db->g[i];

		if (!e->in_use || e->fixed || group_before(db, i, e->group, 1))
			continue;
		at = put_s(buf, at, max, "group=");
		at = put_g(buf, at, max, e->group);
		at = put_s(buf, at, max, "\n");
	}
	for (uint32_t i = 0; i < IGMP_REFUSAL_RING && i < db->refusals; i++) {
		uint32_t idx = db->refusals > IGMP_REFUSAL_RING
			     ? db->refusals - IGMP_REFUSAL_RING + i : i;
		const struct mld_refusal *r = &db->ring[idx % IGMP_REFUSAL_RING];

		at = put_s(buf, at, max, "refused=");
		at = put_s(buf, at, max, reason_name(r->reason));
		at = put_s(buf, at, max, " ");
		at = put_g(buf, at, max, r->group);
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

/* A decimal at *p, up to e. */
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

/* An address up to the next space or the end of the line. */
static int rd_g(const char **p, const char *e, uint8_t g[16])
{
	const char *s = *p;

	while (s < e && *s != ' ')
		s++;
	if (!mld_parse_ipv6(*p, (uint32_t)(s - *p), g))
		return 0;
	*p = s;
	return 1;
}

int mld_state_parse(const char *text, uint32_t len, struct mld_state_view *v)
{
	uint32_t at = 0;
	int understood = 0;

	zero(v, sizeof *v);
	while (at < len) {
		const char *l = text + at, *p = l, *e;
		uint32_t n = 0, x, k, vid;

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
		} else if (rd_lit(&p, e, "refused_")) {
			for (k = 1; k < IGMP_REFUSE_REASONS; k++) {
				const char *q = p;

				if (rd_lit(&q, e, reason_name(k)) && rd_lit(&q, e, "=") &&
				    rd_u(&q, e, &x)) {
					v->refused[k] = x;
					break;
				}
			}
			if (k == IGMP_REFUSE_REASONS)
				continue;
		} else if (rd_lit(&p, e, "group=") && v->ngroup < MLD_MAX_GROUPS) {
			if (!rd_g(&p, e, v->group[v->ngroup]) || p != e)
				continue;
			v->ngroup++;
		} else if (rd_lit(&p, e, "refused=")) {
			struct mld_refusal r;

			for (k = 1; k < IGMP_REFUSE_REASONS; k++) {
				const char *q = p;

				if (rd_lit(&q, e, reason_name(k)) && rd_lit(&q, e, " ")) {
					p = q;
					break;
				}
			}
			r.reason = (uint8_t)k;
			if (k == IGMP_REFUSE_REASONS || !rd_g(&p, e, r.group) ||
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
