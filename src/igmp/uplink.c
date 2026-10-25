/* The way back up -- see uplink.h. No syscalls. */
#include "uplink.h"

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

int igmp_up_read_tags(const uint8_t *f, uint32_t len, struct igmp_up_tags *t)
{
	uint32_t at = 12;
	uint16_t type;

	t->stag = t->ctag = 0;
	t->stci = t->ctci = 0;
	if (len < ETH_HDR_LEN)
		return 0;
	type = rd16(f + at);
	if (type == IGMP_TPID_S) {
		if (len < at + 6)
			return 0;
		t->stag = 1;
		t->stci = rd16(f + at + 2);
		at += 4;
		type = rd16(f + at);
	}
	if (type == IGMP_TPID_C) {
		if (len < at + 6)
			return 0;
		t->ctag = 1;
		t->ctci = rd16(f + at + 2);
		at += 4;
		type = rd16(f + at);
	}
	t->ethertype = type;
	t->hdr_len = at + 2;
	return 1;
}

enum igmp_up_verdict igmp_up_decide(const struct igmp_policy *p, uint8_t type,
				    int all_refused, struct igmp_up_rate *rate,
				    uint32_t now)
{
	/* The OLT is the querier. A query from the host side (a router or a
	 * second querier behind the stick) has no business on the fibre, and
	 * an OLT that took it seriously could stop querying itself. */
	if (type == IGMP_QUERY)
		return IGMP_UP_QUERY;
	/* G.988 attribute 15, false (the default): "the ONU silently discards
	 * the IGMP join request" for a group the dynamic list does not allow,
	 * and a v3 report all of whose groups it refuses. A report that also
	 * leaves a group, or joins one allowed, goes. */
	if (all_refused && !p->unauthorized_join)
		return IGMP_UP_WITHHELD;
	if (p->us_rate) {
		if (!rate->started || rate->sec != now) {
			rate->started = 1;
			rate->sec = now;
			rate->sent = 0;
		}
		if (rate->sent >= p->us_rate)
			return IGMP_UP_RATE;
		rate->sent++;
	}
	return IGMP_UP_SEND;
}

const char *igmp_up_verdict_str(enum igmp_up_verdict v)
{
	switch (v) {
	case IGMP_UP_SEND:     return "sent";
	case IGMP_UP_WITHHELD: return "withheld: every join outside the access control lists";
	case IGMP_UP_QUERY:    return "not sent: a query from the host side";
	case IGMP_UP_RATE:     return "dropped: over the profile's upstream IGMP rate";
	}
	return "?";
}

/* out = the first `at` bytes of in, `ntags` tags, then in from `rest`, padded
 * with zeros to the minimum frame. Returns the length, or 0 when it does not
 * fit. */
static uint32_t splice(const uint8_t *in, uint32_t len, uint32_t rest,
		       const uint16_t *tags, int ntags, uint8_t *out, uint32_t max)
{
	uint32_t n = 12 + 4 * (uint32_t)ntags + (len - rest), at = 12;

	if (rest > len || n > max)
		return 0;
	for (uint32_t i = 0; i < 12; i++)
		out[i] = in[i];
	for (int k = 0; k < ntags; k++) {
		wr16(out + at, tags[2 * k]);
		wr16(out + at + 2, tags[2 * k + 1]);
		at += 4;
	}
	for (uint32_t i = rest; i < len; i++)
		out[at++] = in[i];
	while (at < IGMP_UP_MIN_FRAME) {
		if (at >= max)
			return 0;
		out[at++] = 0;
	}
	return at;
}

uint32_t igmp_up_retag(const uint8_t *in, uint32_t len, uint32_t control,
		       uint16_t tci, uint8_t *out, uint32_t max,
		       uint16_t *vid, uint8_t *pri)
{
	struct igmp_up_tags t;
	uint16_t tags[4];
	int n = 0;

	if (!igmp_up_read_tags(in, len, &t))
		return 0;
	if (control == IGMP_US_TAG_ADD || (!t.stag && !t.ctag)) {
		/* Added in front of whatever the frame has. */
		if (control < IGMP_US_TAG_ADD || control > IGMP_US_TAG_VID)
			return 0;
		tags[0] = IGMP_TPID_C;
		tags[1] = tci;
		*vid = tci & 0x0fff;
		*pri = (uint8_t)(tci >> 13);
		return splice(in, len, 12, tags, 1, out, max);
	}
	/* Replace the outermost tag's TCI, all of it (2) or its VID alone,
	 * the priority and DEI kept (3); its TPID and any tag behind it stay. */
	{
		uint16_t old = t.stag ? t.stci : t.ctci, now;

		if (control == IGMP_US_TAG_REPLACE)
			now = tci;
		else if (control == IGMP_US_TAG_VID)
			now = (uint16_t)((old & 0xf000) | (tci & 0x0fff));
		else
			return 0;
		tags[n++] = t.stag ? IGMP_TPID_S : IGMP_TPID_C;
		tags[n++] = now;
		if (t.stag && t.ctag) {
			tags[n++] = IGMP_TPID_C;
			tags[n++] = t.ctci;
		}
		*vid = now & 0x0fff;
		*pri = (uint8_t)(now >> 13);
		return splice(in, len, t.hdr_len - 2, tags, n / 2, out, max);
	}
}

/* One tag plane of a row's treatment applied to the tag the frame has (or
 * has not): add or replace with what the row assigns, the rest the frame's
 * own (docs/SWITCH.md, "Extended VLAN rules"). */
static uint16_t assigned(int had, uint16_t tci, uint32_t src, uint32_t vid, uint32_t pri)
{
	uint16_t v = had ? tci : 0;

	if (src & ODI_SW_US_SRC_VID)
		v = (uint16_t)((v & 0xf000) | (vid & 0x0fff));
	if (src & ODI_SW_US_SRC_PRI)
		v = (uint16_t)((v & 0x1fff) | ((pri & 7) << 13));
	return v;
}

uint32_t igmp_up_treat(const uint8_t *in, uint32_t len,
		       const struct odi_sw_mc_route *r, uint8_t *out, uint32_t max)
{
	struct igmp_up_tags t;
	uint16_t tags[4];
	int n = 0, s, c;
	uint16_t stci, ctci;

	if (!igmp_up_read_tags(in, len, &t))
		return 0;
	s = t.stag;
	stci = t.stci;
	switch (r->s_act) {
	case ODI_SW_SACT_ADD:
	case ODI_SW_SACT_ADD_TOO:
		stci = assigned(s, stci, r->s_src, r->s_vid, r->s_pri);
		s = 1;
		break;
	case ODI_SW_SACT_DEL:
		s = 0;
		break;
	case ODI_SW_SACT_REWRITE:
		return 0;
	default:
		break;
	}
	c = t.ctag;
	ctci = t.ctci;
	switch (r->c_act) {
	case ODI_SW_CACT_ADD:
		ctci = assigned(c, ctci, r->c_src, r->c_vid, r->c_pri);
		c = 1;
		break;
	case ODI_SW_CACT_DEL:
		c = 0;
		break;
	default:
		break;
	}
	if (s) {
		tags[n++] = IGMP_TPID_S;
		tags[n++] = stci;
	}
	if (c) {
		tags[n++] = IGMP_TPID_C;
		tags[n++] = ctci;
	}
	return splice(in, len, t.hdr_len - 2, tags, n / 2, out, max);
}
