/* igmpd -s -- see status.h. */
#include "sys.h"
#include "io.h"
#include "status.h"
#include "state.h"

static void out_group(uint32_t g)
{
	out_fmt("%u.%u.%u.%u", (g >> 24) & 0xff, (g >> 16) & 0xff,
		(g >> 8) & 0xff, g & 0xff);
}

void igmp_policy_describe(const struct igmp_policy *p)
{
	uint32_t t = p->us_tci;

	out_fmt("policy: %s, function %d, %s leave, %d/%d/%d s, limit %d, "
		"%d allow + %d static ranges%s\n",
		p->snooping ? "snooping" : "not snooping",
		(long)p->function, p->immediate_leave ? "immediate" : "timed",
		(long)p->robustness, (long)p->query_interval,
		(long)p->last_member_query, (long)p->max_groups,
		(long)p->nallow, (long)p->nstatic,
		p->overflow ? " (too many: not enforced)" : "");
	/* The join rules past the plain ranges, only when the profile has any:
	 * a profile of bare ranges reads as it always did. */
	{
		uint32_t rows = 0, vl = 0, sr = 0, pv = 0;

		for (uint32_t i = 0; i < p->nallow; i++) {
			rows++;
			vl += p->allow[i].vid != 0;
			sr += p->allow[i].src != 0;
			pv += p->allow[i].pv_len != 0;
		}
		if (p->snooping && ((p->igmp_version && p->igmp_version < 3) ||
				    vl || sr || pv || p->ngrant)) {
			const char *sep = "  join rules: ";

			if (p->igmp_version && p->igmp_version < 3) {
				out_fmt("%sIGMP version %u at most (a message above it is only counted)", sep, p->igmp_version);
				sep = "; ";
			}
			if (vl) {
				out_fmt("%s%u of %u rows name a VLAN", sep, vl, rows);
				sep = "; ";
			}
			if (sr) {
				out_fmt("%s%u name a source", sep, sr);
				sep = "; ";
			}
			if (pv) {
				out_fmt("%s%u are previews", sep, pv);
				sep = "; ";
			}
			if (p->ngrant)
				out_fmt("%s%u allowed preview groups", sep, p->ngrant);
			out("\n");
		}
	}
	/* A second line only when the reports are taken: a profile the switch
	 * already forwards as asked reads as it always did. */
	if (!igmp_policy_takes(p))
		return;
	out("  upstream: reports taken and sent by igmpd; tag ");
	switch (p->us_tag_control) {
	case IGMP_US_TAG_ADD:
		out_fmt("added, vid %u pri %u", t & 0x0fff, t >> 13);
		break;
	case IGMP_US_TAG_REPLACE:
		out_fmt("replaced by vid %u pri %u", t & 0x0fff, t >> 13);
		break;
	case IGMP_US_TAG_VID:
		out_fmt("vid replaced by %u", t & 0x0fff);
		break;
	default:
		out("as the switch tags it");
		break;
	}
	if (p->us_rate)
		out_fmt("; at most %u a second", p->us_rate);
	if (igmp_policy_acl_active(p))
		out(p->unauthorized_join ? "; unauthorized joins forwarded" :
		    "; unauthorized joins withheld");
	out("\n");
}

static void out_ranges(const char *kind, const struct igmp_range *r, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		out_fmt("  %s ", kind);
		out_group(r[i].lo);
		out(" - ");
		out_group(r[i].hi);
		out("\n");
	}
}

static const char *reason_text(uint32_t r)
{
	switch (r) {
	case IGMP_REFUSE_ACL:   return "outside the access control lists";
	case IGMP_REFUSE_LIMIT: return "past the group limit";
	case IGMP_REFUSE_FULL:  return "no free table entry";
	case IGMP_REFUSE_PREVIEW: return "preview used up";
	}
	return "unknown reason";
}

void igmp_status_print(const struct igmp_policy *p, int have_policy,
		       const char *state, uint32_t len, uint32_t now)
{
	static struct igmp_state_view v;

	if (have_policy) {
		igmp_policy_describe(p);
		out_ranges("allow", p->allow, p->nallow);
		out_ranges("static", p->stat, p->nstatic);
	} else {
		out("policy: none (omcid has not written one: not snooping)\n");
	}
	if (!len) {
		out("state: none (igmpd has not written one: not running, or run without -w)\n");
		return;
	}
	igmp_state_parse(state, len, &v);
	out_fmt("joins %u, refused by the profile %u (lists %u, group limit %u), "
		"table full %u\n", v.joins, v.denied, v.refused[IGMP_REFUSE_ACL],
		v.refused[IGMP_REFUSE_LIMIT], v.refused[IGMP_REFUSE_FULL]);
	if (v.refused[IGMP_REFUSE_PREVIEW])
		out_fmt("also refused: preview used up %u\n", v.refused[IGMP_REFUSE_PREVIEW]);
	if (v.above_version)
		out_fmt("above the profile's IGMP version %u (handled as any other)\n",
			v.above_version);
	if (v.up_sent || v.up_withheld || v.up_limited || v.up_unsent)
		out_fmt("sent up %u, withheld %u, over the rate %u, not sent %u\n",
			v.up_sent, v.up_withheld, v.up_limited, v.up_unsent);
	out_fmt("groups held: %u\n", v.groups);
	for (uint32_t i = 0; i < v.ngroup; i++) {
		out("  ");
		out_group(v.group[i]);
		out("\n");
	}
	if (!v.nrefusal) {
		out("recent refusals: none\n");
		return;
	}
	out("recent refusals, newest first:\n");
	for (uint32_t i = v.nrefusal; i-- > 0;) {
		const struct igmp_refusal *r = &v.ref[i];

		out("  ");
		out_group(r->group);
		out_fmt("  vid %u  %s (%s)", (uint32_t)r->vid,
			reason_text(r->reason), igmp_refusal_name(r->reason));
		if (r->count > 1)
			out_fmt("  x%u", r->count);
		if (now && now >= r->at)
			out_fmt("  %u s ago\n", now - r->at);
		else
			out_fmt("  at %u s\n", r->at);
	}
}

/* The whole file into buf; its length, 0 when there is none. */
static uint32_t slurp(const char *path, char *buf, uint32_t max)
{
	long fd = sys_open(path, O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = sys_read((int)fd, buf, max);
	sys_close((int)fd);
	return n > 0 ? (uint32_t)n : 0;
}

int igmp_status_run(const char *state_path, const char *policy_path)
{
	static char sbuf[IGMP_STATE_MAX + 1], pbuf[IGMP_POLICY_MAX_BYTES];
	struct igmp_policy p;
	uint32_t sl, pl;
	long sec = 0, nsec = 0;
	uint32_t now = 0;

	/* The daemon's monotonic clock stamps the refusals, so ages are only
	 * meaningful against its own files. */
	if (!state_path && sys_clock_gettime(CLOCK_MONOTONIC, &sec, &nsec) == 0)
		now = (uint32_t)sec;
	sl = slurp(state_path ? state_path : IGMP_STATE_PATH, sbuf, IGMP_STATE_MAX);
	pl = slurp(policy_path ? policy_path : IGMP_POLICY_PATH, pbuf, sizeof pbuf);
	igmp_policy_parse(pbuf, pl, &p);
	igmp_status_print(&p, pl != 0, sbuf, sl, now);
	out_flush();
	return 0;
}
