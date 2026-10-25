/* What the OLT provisioned for multicast, as a small text file igmpd reads.
 *
 * omcid owns the managed entities (281 multicast GEM interworking TP, 309
 * multicast operations profile, 310 multicast subscriber config info, 47
 * bridge port); igmpd owns the switch's multicast group rows. They are two
 * processes, so the join between them is a file: a tmpfs file written whole
 * and renamed over the old one (a reader never sees half of it), rewritten
 * a quiet second after the last OMCI frame like the bridge connections are,
 * and removed-by-meaning (snooping=0) when the MIB is reset.
 *
 * The file is what the profile tells the snooper, not what the snooper then
 * does: igmpd decides what it can honour. Format, one key=value per line:
 *
 *   snooping=1|0         the OLT asks for IGMP snooping (function 0) and a
 *                        multicast GEM port carries groups (281 holds a row)
 *   igmp_function=N      attribute 2 of the profile as sent
 *   immediate_leave=0|1
 *   robustness=N         the profile value, 2 when unset or zero
 *   query_interval=S     seconds, 125 when unset or zero
 *   query_max_response=S seconds (the profile counts tenths), 10 when unset
 *   last_member_query=S  seconds (tenths in the profile), 1 when unset
 *   max_groups=N         ME 310 maximum simultaneous groups, 0 = no limit
 *   unauthorized_join=0|1
 *   us_tag_control=N     attribute 5, the upstream IGMP tag control as sent
 *   us_tci=N             attribute 4, the upstream IGMP TCI (16 bits)
 *   us_rate=N            attribute 6, messages a second, 0 = no limit
 *   allow=A-B            a range of the dynamic access control list
 *   static=A-B           a range of the static access control list
 *   allow_overflow=1     the OLT sent more ranges than fit: not enforced
 *
 * The subscriber is the one UNI: the profile is taken from the ME 310 whose
 * instance is a bridge port of the PPTP Ethernet UNI (else of the VEIP, else
 * the first ME 310 there is).
 */
#include "omcid.h"

#define MCAST_POLICY_PATH     "/var/run/odi-mcast.conf"
#define MCAST_POLICY_TMP_PATH "/var/run/odi-mcast.conf.tmp"

#define MCAST_STATE_PATH      "/var/run/odi-mcast.state"
#define CLS_MCAST_OPER_PROFILE 309
#define CLS_MCAST_SUBSCRIBER   310
#define CLS_BRIDGE_PORT        47
#define MC_MAX_RANGES          32

#define TP_PPTP_ETH_UNI        1
#define TP_VEIP                11

/* Starts set: a fresh or resumed omcid writes the file once, so it is never
 * older than the MIB it was derived from. */
int mcast_dirty = 1;

/* The classes the file is derived from. */
int mcast_policy_class(uint16_t cls)
{
	return cls == CLS_MCAST_OPER_PROFILE || cls == CLS_MCAST_SUBSCRIBER ||
	       cls == CLS_BRIDGE_PORT || cls == OMCI_ME_MCAST_GEM_IW_TP;
}

static char pbuf[4096];
static uint32_t plen;
/* What was last written, so a MIB change that leaves the policy as it was
 * (every Set of an unrelated entity) writes nothing. */
static char last[sizeof pbuf];
static uint32_t last_len;
static int last_valid;

static void pc(char ch)
{
	if (plen < sizeof pbuf)
		pbuf[plen++] = ch;
}

static void ps(const char *s)
{
	while (*s)
		pc(*s++);
}

static void pd(uint32_t v)
{
	char d[10];
	int n = 0;

	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		pc(d[--n]);
}

static void pkv(const char *k, uint32_t v)
{
	ps(k);
	pc('=');
	pd(v);
	pc('\n');
}

static void pip(uint32_t a)
{
	for (int i = 3; i >= 0; i--) {
		pd((a >> (8 * i)) & 0xff);
		if (i)
			pc('.');
	}
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* The ME 310 that stands for the UNI, or 0. */
static struct mib_row *subscriber_row(void)
{
	const struct omci_class *c47 = find_class(CLS_BRIDGE_PORT);
	struct mib_row *first = 0, *veip = 0;

	for (int i = 0; i < MIB_ROWS; i++) {
		struct mib_row *r = mib_row_at(i);
		struct mib_row *bp;
		uint32_t tp;

		if (!r || r->classId != CLS_MCAST_SUBSCRIBER)
			continue;
		if (!first)
			first = r;
		bp = c47 ? mib_find(CLS_BRIDGE_PORT, r->inst) : 0;
		if (!bp)
			continue;
		tp = row_u32(bp, c47, 3);                 /* TP type; 4 is the pointer */
		if (tp == TP_PPTP_ETH_UNI)
			return r;
		if (tp == TP_VEIP && !veip)
			veip = r;
	}
	return veip ? veip : first;
}

/* Whether any multicast GEM interworking TP holds an IPv4 or IPv6 row. */
static int gem_carries_groups(void)
{
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = mib_row_at(i);

		if (r && r->classId == OMCI_ME_MCAST_GEM_IW_TP &&
		    (tbl_rows(r, 9) || tbl_rows(r, 10)))
			return 1;
	}
	return 0;
}

/* G.988 table control word of a stored ACL row: the row part is bits 13-11
 * and the row key bits 9-0. The part bits are kept only when the reported
 * OMCC version is 0xA1 or later (mibstore.c), so with an older OMCC every
 * row reads as part 0, which is what such an OLT sends. */
#define ACL_PART(e) (((e)[0] >> 3) & 7)
#define ACL_KEY(e)  (((e)[0] << 8 | (e)[1]) & 0x3ff)

/* Whether the 12 leading bytes of a part 1 or part 2 row make the 4-byte
 * address of the part 0 row an IPv4 address: ten zero bytes, then 0000 or
 * ffff (the preferred form), as G.988 9.3.27 says. Anything else is an IPv6
 * address, which igmpd does not take. */
static int acl_v4_prefix(const uint8_t *p)
{
	for (int i = 0; i < 10; i++)
		if (p[i])
			return 0;
	return p[10] == p[11] && (p[10] == 0x00 || p[10] == 0xff);
}

/* Whether the logical row of `key` has a part `part` that turns the part 0
 * addresses into IPv6 ones. A row without that part is IPv4. */
static int acl_part_is_v6(const struct mib_row *r, unsigned attr, unsigned key,
			  unsigned part)
{
	for (uint8_t i = r->tbl_head; i != MIB_TBL_NONE; i = tblpool[i].next) {
		const uint8_t *e = tblpool[i].data;

		if (tblpool[i].attr == attr && ACL_PART(e) == part &&
		    ACL_KEY(e) == key)
			return !acl_v4_prefix(e + 2);
	}
	return 0;
}

/* One access control list table of the profile, as `key=A-B` lines. A row
 * is the 24 bytes of G.988 part 0: control 2, GEM port 2, VLAN 2, source 4,
 * start 4, end 4, bandwidth 4, reserved 2. Parts 1 and 2 (IPv6 source and
 * destination prefixes, preview fields) are other rows of the same key and
 * are not ranges: they are never read as one, and a part 0 row they make
 * IPv6 is left out, since igmpd enforces the IPv4 list only. An IPv6-only
 * table therefore lists nothing, and nothing is enforced. Returns how many
 * rows it listed, or -1 when there were more than fit. */
static int acl_lines(const struct mib_row *r, unsigned attr, const char *key)
{
	int n = 0;

	for (uint8_t i = r->tbl_head; i != MIB_TBL_NONE; i = tblpool[i].next) {
		const uint8_t *e = tblpool[i].data;
		uint32_t lo, hi;

		if (tblpool[i].attr != attr || ACL_PART(e) != 0)
			continue;
		if (acl_part_is_v6(r, attr, ACL_KEY(e), 1) ||
		    acl_part_is_v6(r, attr, ACL_KEY(e), 2))
			continue;
		lo = be32(e + 10);
		hi = be32(e + 14);
		/* An end below the start is a row nobody can match; an end of
		 * zero is the single-address form some OLTs send. */
		if (!lo)
			continue;
		if (!hi)
			hi = lo;
		if (hi < lo)
			continue;
		if (n >= MC_MAX_RANGES)
			return -1;
		ps(key);
		pc('=');
		pip(lo);
		pc('-');
		pip(hi);
		pc('\n');
		n++;
	}
	return n;
}

static uint32_t or_default(uint32_t v, uint32_t d)
{
	return v ? v : d;
}

/* Tenths of a second to whole seconds, at least one. */
static uint32_t tenths_to_s(uint32_t v, uint32_t d_tenths)
{
	uint32_t s = or_default(v, d_tenths) / 10;

	return s ? s : 1;
}

/* G.988 has the service package table (310 attribute 6) choose the profile
 * by the join's VLAN, and the profile pointer (attribute 2) ignored while the
 * table has rows. That is not done: the pointer's profile is always the one
 * exported. Said once, when an OLT first provisions the table, and again
 * only after the table was emptied. */
static void svcpkg_note(const struct mib_row *sub)
{
	static int said;

	if (!sub || !tbl_rows(sub, 6)) {
		said = 0;
		return;
	}
	if (said)
		return;
	said = 1;
	out_fmt("   mcast: 310/%04x has a multicast service package table, which "
		"does not select a profile by VID here (the pointer's profile is used)\n",
		(long)sub->inst);
}

static void build(void)
{
	const struct omci_class *c309 = find_class(CLS_MCAST_OPER_PROFILE);
	const struct omci_class *c310 = find_class(CLS_MCAST_SUBSCRIBER);
	struct mib_row *sub = subscriber_row(), *prof = 0;
	int snoop = 0, ov = 0;
	uint32_t fun = 0;

	plen = 0;
	svcpkg_note(sub);
	if (sub && c310 && c309)
		prof = mib_find(CLS_MCAST_OPER_PROFILE, (uint16_t)row_u32(sub, c310, 2));
	if (prof)
		fun = row_u32(prof, c309, 2);
	/* Snooping is what function 0 means. Function 1 (snooping with proxy
	 * reporting) and 2 (proxy) want the ONU to answer the OLT queries
	 * itself, which this one does not: the multicast then stays as
	 * transparent as with no profile at all. */
	snoop = prof && fun == 0 && gem_carries_groups();

	ps("# written by omcid from the OLT provisioning, read by igmpd\n");
	pkv("snooping", (uint32_t)snoop);
	if (!prof)
		return;
	pkv("igmp_function", fun);
	pkv("immediate_leave", row_u32(prof, c309, 3) ? 1u : 0u);
	pkv("robustness", or_default(row_u32(prof, c309, 10), 2));
	pkv("query_interval", or_default(row_u32(prof, c309, 12), 125));
	pkv("query_max_response", tenths_to_s(row_u32(prof, c309, 13), 100));
	pkv("last_member_query", tenths_to_s(row_u32(prof, c309, 14), 10));
	pkv("unauthorized_join", row_u32(prof, c309, 15) ? 1u : 0u);
	pkv("us_tag_control", row_u32(prof, c309, 5));
	pkv("us_tci", row_u32(prof, c309, 4));
	pkv("us_rate", row_u32(prof, c309, 6));
	pkv("max_groups", row_u32(sub, c310, 3));
	if (acl_lines(prof, 7, "allow") < 0)
		ov = 1;
	if (acl_lines(prof, 8, "static") < 0)
		ov = 1;
	if (ov)
		pkv("allow_overflow", 1);
}

void mcast_policy_export(void)
{
	build();
	if (plen >= sizeof pbuf)
		return;                         /* cannot happen: ranges are capped */
	if (last_valid && plen == last_len) {
		uint32_t i = 0;

		while (i < plen && pbuf[i] == last[i])
			i++;
		if (i == plen)
			return;
	}
	if (atomic_write(MCAST_POLICY_PATH, MCAST_POLICY_TMP_PATH,
			 (const uint8_t *)pbuf, plen) < 0) {
		last_valid = 0;
		return;
	}
	for (uint32_t i = 0; i < plen; i++)
		last[i] = pbuf[i];
	last_len = plen;
	last_valid = 1;
	out_fmt("   multicast policy written (%d bytes)\n", (long)plen);
}

/* ---- ME 311: what igmpd reports ----------------------------------------- */

/* The state file, whole, into buf; its length, 0 when there is none. igmpd
 * writes it by rename, so what is read is one complete version. */
static uint32_t state_read(char *buf, uint32_t max)
{
	long fd = sys_open(MCAST_STATE_PATH, O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = sys_read((int)fd, buf, max);
	sys_close((int)fd);
	return n > 0 ? (uint32_t)n : 0;
}

/* A number at the start of s, up to a non-digit; the digits' count in *used. */
static uint32_t dec(const char *s, uint32_t n, uint32_t *used)
{
	uint32_t v = 0, i = 0;

	while (i < n && s[i] >= '0' && s[i] <= '9' && i < 10)
		v = v * 10 + (uint32_t)(s[i++] - '0');
	*used = i;
	return v;
}

uint32_t mcast_state_get(const char *key)
{
	static char buf[4096];
	uint32_t n = state_read(buf, sizeof buf), at = 0;

	while (at < n) {
		const char *l = buf + at;
		uint32_t len = 0, k = 0;

		while (at + len < n && l[len] != '\n')
			len++;
		/* A line without its newline is the cut-off end of a file longer
		 * than the buffer: not one to trust. */
		if (at + len >= n)
			break;
		while (key[k] && k < len && l[k] == key[k])
			k++;
		if (!key[k] && k < len && l[k] == '=') {
			uint32_t used;
			uint32_t v = dec(l + k + 1, len - k - 1, &used);

			return used ? v : 0;
		}
		at += len + 1;
	}
	return 0;
}

/* G.988 9.3.28, attribute 6: VLAN id 2, source 4, destination 4, best
 * efforts bandwidth 4, client 4, time since the last join 4, reserved 2.
 * What igmpd knows is the group; the rest is not measured and reads zero. */
uint16_t mcast_active_groups(uint8_t *out, uint16_t max)
{
	static char buf[4096];
	uint32_t n = state_read(buf, sizeof buf), at = 0;
	uint16_t w = 0;

	while (at < n) {
		const char *l = buf + at;
		uint32_t len = 0;

		while (at + len < n && l[len] != '\n')
			len++;
		if (at + len >= n)
			break;                  /* no newline: cut off, not trusted */
		at += len + 1;
		if (len > 6 && l[0] == 'g' && l[1] == 'r' && l[2] == 'o' && l[3] == 'u' &&
		    l[4] == 'p' && l[5] == '=') {
			uint32_t pos = 6, used, a[4];
			int good = 1;

			if (w + 24 > max)
				break;
			for (int i = 0; i < 4 && good; i++) {
				a[i] = pos < len ? dec(l + pos, len - pos, &used) : (used = 0);
				pos += used + 1;
				good = used && a[i] <= 255;
			}
			if (!good)
				continue;
			for (int i = 0; i < 24; i++)
				out[w + i] = 0;
			for (int i = 0; i < 4; i++)
				out[w + 6 + i] = (uint8_t)a[i];
			w = (uint16_t)(w + 24);
		}
	}
	return w;
}
