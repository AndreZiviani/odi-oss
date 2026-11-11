/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* See commit.h for what this does and why in this order. */
#include "env.h"
#include "commit.h"

/* Three blocks: the two copies as read, and a read-back. Static, not on the
 * stack: env_set already puts one ENV_MAX buffer there. */
static uint8_t c1[ENV_MAX], c2[ENV_MAX], back[ENV_MAX];

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* 1 when `key` is present in the block and equals `want` exactly. */
static int has(const uint8_t *buf, uint32_t len, const char *key,
	       const char *want)
{
	char v[16];
	int i = 0;

	if (!env_get(buf, len, key, v, sizeof v))
		return 0;
	while (want[i] && v[i] == want[i])
		i++;
	return want[i] == '\0' && v[i] == '\0';
}

/* Write one copy and read it back. The comparison is against the whole
 * block as it was meant to land, so it catches a bit that did not program,
 * a short write and a stale read alike, not just the one key. */
static int put(const struct env_io *io, int copy, const uint8_t *want,
	       uint32_t len, int werr, int verr)
{
	if (!io->write(io->ctx, copy, want, len))
		return werr;
	if (io->read(io->ctx, copy, back) != len || !same(back, want, len))
		return verr;
	return NVC_DONE;
}

/* What a copy must say once committed: sw_commit=<slot> and mupgrade_en=0.
 * 1 when it already does. */
static int done(const uint8_t *buf, uint32_t len, const char *want)
{
	return has(buf, len, "sw_commit", want) && has(buf, len, "mupgrade_en", "0");
}

/* Set whichever of the two is missing or different, in one rebuild, so the
 * copy is still written (and erased) once. 0 when it does not fit. */
static int apply(uint8_t *buf, uint32_t len, const char *want)
{
	if (!has(buf, len, "sw_commit", want) &&
	    !env_set(buf, len, "sw_commit", want))
		return 0;
	if (!has(buf, len, "mupgrade_en", "0") &&
	    !env_set(buf, len, "mupgrade_en", "0"))
		return 0;
	return 1;
}

/* The image for the copy being repaired: the new environment, byte for
 * byte, with the flags byte env_repair_flags picks. The CRC does not cover
 * the flags byte, so it stays right. */
static uint8_t rep[ENV_MAX];

static uint32_t repair_image(int l, const uint8_t *w, uint32_t wn, int *keeps)
{
	for (uint32_t i = 0; i < wn; i++)
		rep[i] = w[i];
	rep[ENV_HDR_CRC] = env_repair_flags(l, w[ENV_HDR_CRC], keeps);
	return wn;
}

int env_commit(const struct env_io *io, int slot, int running,
	       struct env_commit_report *r)
{
	const char *want = slot == 0 ? "0" : "1";
	uint32_t n1, n2, pn, fn;
	uint8_t *p, *f;
	int ok1, ok2, rc, bad, need_p, expect, keeps = 1;

	r->primary = r->fallback = 0;
	r->wrote_primary = r->wrote_fallback = 0;
	r->repaired = 0;
	if (slot != 0 && slot != 1)
		return NVC_BAD_SLOT;
	if (running != slot)
		return NVC_NOT_RUNNING;

	n1 = io->read(io->ctx, 1, c1);
	n2 = io->read(io->ctx, 2, c2);
	ok1 = n1 && env_valid(c1, n1);
	ok2 = n2 && env_valid(c2, n2);
	if (!ok1 && !ok2)
		return NVC_NO_PAIR;
	bad = !ok1 || !ok2;
	/* With one valid copy it is the primary whatever its flags say. */
	r->primary = env_pick(c1, ok1, c2, ok2);
	r->fallback = 3 - r->primary;
	p  = r->primary == 1 ? c1 : c2;
	pn = r->primary == 1 ? n1 : n2;
	f  = r->primary == 1 ? c2 : c1;
	fn = r->primary == 1 ? n2 : n1;

	/* U-Boot's own record of the slot it started. Read from the primary
	 * only: the fallback holds whatever the environment said before the
	 * last save, which after a trial boot is the slot booted before it. */
	if (!has(p, pn, "sw_active", want))
		return NVC_NOT_ACTIVE;

	need_p = !done(p, pn, want);
	expect = r->primary;
	if (need_p && !apply(p, pn, want))
		return NVC_NO_ROOM;

	if (bad) {
		/* One copy is invalid, so the valid one is the only good
		 * environment and is not touched until the other holds the
		 * new one, verified. The copy written first is the invalid
		 * one: it takes the whole new environment, with a flags byte
		 * that keeps the primary winning where it can (env.h). A
		 * power cut anywhere leaves a valid copy: the old primary
		 * until the repaired copy verifies, either one after. */
		repair_image(r->fallback, p, pn, &keeps);
		rc = put(io, r->fallback, rep, pn, NVC_WRITE_REPAIR, NVC_VERIFY_REPAIR);
		if (rc != NVC_DONE)
			return rc;
		r->repaired = r->fallback;
		r->wrote_fallback = 1;
		if (!keeps)
			expect = r->fallback;
		if (need_p) {
			rc = put(io, r->primary, p, pn, NVC_WRITE_PRIMARY, NVC_VERIFY_PRIMARY);
			if (rc != NVC_DONE)
				return rc;
			r->wrote_primary = 1;
		}
	} else {
		if (need_p) {
			rc = put(io, r->primary, p, pn, NVC_WRITE_PRIMARY, NVC_VERIFY_PRIMARY);
			if (rc != NVC_DONE)
				return rc;
			r->wrote_primary = 1;
		}
		if (!done(f, fn, want)) {
			if (!apply(f, fn, want))
				return NVC_NO_ROOM;
			rc = put(io, r->fallback, f, fn, NVC_WRITE_FALLBACK, NVC_VERIFY_FALLBACK);
			if (rc != NVC_DONE)
				return rc;
			r->wrote_fallback = 1;
		}
	}

	/* Both again, from the partitions, as a reader will see them: both
	 * valid, the expected copy winning (the primary, which env_set and the
	 * repaired flags byte keep winning), sw_commit and mupgrade_en=0 in
	 * both, sw_active untouched. */
	n1 = io->read(io->ctx, 1, c1);
	n2 = io->read(io->ctx, 2, c2);
	if (!n1 || !n2 || !env_valid(c1, n1) || !env_valid(c2, n2))
		return NVC_FINAL;
	if (env_pick(c1, 1, c2, 1) != expect)
		return NVC_FINAL;
	p  = expect == 1 ? c1 : c2;
	pn = expect == 1 ? n1 : n2;
	if (!done(c1, n1, want) || !done(c2, n2, want) ||
	    !has(p, pn, "sw_active", want))
		return NVC_FINAL;
	return r->wrote_primary || r->wrote_fallback ? NVC_DONE : NVC_ALREADY;
}

/* 1 when the block holds exactly key=value (value NULL: no such key). The
 * block is walked, not env_get-ed, so a long value is compared whole. */
static int holds(const uint8_t *buf, uint32_t len, const char *key,
		 const char *value)
{
	uint32_t pos = 0;
	const char *pair;
	int found = 0;

	while ((pair = env_next(buf, len, &pos)) != 0) {
		int i = 0;

		while (key[i] && pair[i] == key[i])
			i++;
		if (key[i] != '\0' || pair[i] != '=')
			continue;
		found = 1;
		if (!value)
			return 0;
		const char *v = pair + i + 1;
		int j = 0;

		while (value[j] && v[j] == value[j])
			j++;
		if (value[j] != '\0' || v[j] != '\0')
			return 0;
	}
	return value ? found : 1;
}

int env_setenv(const struct env_io *io, int copy, const char *key,
	       const char *value, struct env_set_report *r)
{
	uint32_t n1, n2, n;
	uint8_t *b;
	int ok1, ok2, win, tgt, v, l, rc, keeps;

	r->target = r->repaired = r->wrote_target = 0;
	n1 = io->read(io->ctx, 1, c1);
	n2 = io->read(io->ctx, 2, c2);
	ok1 = n1 && env_valid(c1, n1);
	ok2 = n2 && env_valid(c2, n2);
	if (!ok1 && !ok2)
		return NVS_NONE_VALID;
	win = env_pick(c1, ok1, c2, ok2);
	tgt = copy ? copy : win;
	r->target = tgt;

	if (ok1 && ok2) {
		b = tgt == 1 ? c1 : c2;
		n = tgt == 1 ? n1 : n2;
		if (!env_set(b, n, key, value))
			return NVS_NO_ROOM;
		if (!holds(b, n, key, value))
			return NVS_NOT_SET;
		rc = put(io, tgt, b, n, NVS_WRITE, NVS_VERIFY);
		if (rc != NVC_DONE)
			return rc;
		r->wrote_target = 1;
		return NVS_DONE;
	}

	/* One valid copy, v: the only good environment. The new block is built
	 * and checked in memory first, then written to the invalid copy l and
	 * read back, and only then is v touched. */
	v = ok1 ? 1 : 2;
	l = 3 - v;
	b = v == 1 ? c1 : c2;
	n = v == 1 ? n1 : n2;
	if (!env_set(b, n, key, value))
		return NVS_NO_ROOM;
	if (!holds(b, n, key, value))
		return NVS_NOT_SET;
	repair_image(l, b, n, &keeps);
	rc = put(io, l, rep, n, NVS_WRITE_REPAIR, NVS_VERIFY_REPAIR);
	if (rc != NVC_DONE)
		return rc;
	r->repaired = l;
	if (tgt == l)
		return NVS_DONE;         /* -c named the invalid copy: just that one */
	/* The winner is what U-Boot reads, so the change only takes effect once
	 * it is rewritten; with the repaired copy already good this erase no
	 * longer leaves the board without an environment. */
	rc = put(io, v, b, n, NVS_WRITE, NVS_VERIFY);
	if (rc != NVC_DONE)
		return rc;
	r->wrote_target = 1;
	return NVS_DONE;
}

const char *env_setenv_msg(int rc)
{
	switch (rc) {
	case NVS_DONE:          return "written and read back";
	case NVS_NONE_VALID:    return "neither environment copy is valid, nothing written";
	case NVS_NO_ROOM:       return "the new value does not fit in the environment block, nothing written";
	case NVS_NOT_SET:       return "the new block does not hold the value asked for, nothing written";
	case NVS_WRITE_REPAIR:  return "the write of the invalid copy failed; the valid copy was not touched";
	case NVS_VERIFY_REPAIR: return "the invalid copy did not read back as written; the valid copy was not touched";
	case NVS_WRITE:         return "the write failed";
	case NVS_VERIFY:        return "the copy written did not read back as written";
	default:                return "unknown result";
	}
}

int env_commit_guarded(const struct env_io *io, struct commit_aids *a,
		       int force, int slot, int running,
		       struct env_commit_report *r)
{
	a->has_confirm_arp = a->exists && a->confirm_arp &&
			     a->exists(a->ctx, a->confirm_arp);
	a->has_breadcrumbs = a->exists && a->breadcrumbs &&
			     a->exists(a->ctx, a->breadcrumbs);
	if (a->has_confirm_arp && !force) {
		r->primary = r->fallback = 0;
		r->wrote_primary = r->wrote_fallback = 0;
		r->repaired = 0;
		return NVC_CONFIRM_ARP;
	}
	return env_commit(io, slot, running, r);
}

const char *env_commit_msg(int rc)
{
	switch (rc) {
	case NVC_DONE:            return "committed in both copies, read back";
	case NVC_ALREADY:         return "both copies already commit this slot, nothing written";
	case NVC_BAD_SLOT:        return "the slot must be 0 or 1";
	case NVC_NOT_RUNNING:     return "that is not the slot this kernel was booted from (root= in /proc/cmdline), nothing written";
	case NVC_NO_PAIR:         return "neither environment copy is valid, nothing written";
	case NVC_NOT_ACTIVE:      return "sw_active in the primary copy does not name that slot, nothing written";
	case NVC_NO_ROOM:         return "sw_commit and mupgrade_en do not fit in the environment block";
	case NVC_WRITE_PRIMARY:   return "the write of the primary copy failed; the fallback copy was not touched";
	case NVC_VERIFY_PRIMARY:  return "the primary copy did not read back as written; the fallback copy was not touched";
	case NVC_WRITE_FALLBACK:  return "the primary copy is committed and verified, the write of the fallback copy failed";
	case NVC_VERIFY_FALLBACK: return "the primary copy is committed and verified, the fallback copy did not read back as written";
	case NVC_FINAL:           return "the final read-back of both copies does not agree";
	case NVC_CONFIRM_ARP:     return "refused, the confirm-arp development file exists, nothing written";
	case NVC_WRITE_REPAIR:    return "the write of the invalid copy failed; the valid copy was not touched";
	case NVC_VERIFY_REPAIR:   return "the invalid copy did not read back as written; the valid copy was not touched";
	default:                  return "unknown result";
	}
}

int cmdline_root_index(const char *cmd)
{
	const char *p, *last = 0;
	int idx = 0;

	for (p = cmd; *p; p++)
		if ((p == cmd || p[-1] == ' ') && p[0] == 'r' && p[1] == 'o' &&
		    p[2] == 'o' && p[3] == 't' && p[4] == '=')
			last = p + 5;
	if (!last || last[0] != '3' || last[1] != '1' || last[2] != ':')
		return -1;
	p = last + 3;
	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9' && idx < 1000)
		idx = idx * 10 + (*p++ - '0');
	if (*p && *p != ' ' && *p != '\n')
		return -1;
	return idx;
}
