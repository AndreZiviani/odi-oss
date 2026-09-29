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

int env_commit(const struct env_io *io, int slot, int running,
	       struct env_commit_report *r)
{
	const char *want = slot == 0 ? "0" : "1";
	uint32_t n1, n2, pn, fn;
	uint8_t *p, *f;
	int ok1, ok2, rc;

	r->primary = r->fallback = 0;
	r->wrote_primary = r->wrote_fallback = 0;
	if (slot != 0 && slot != 1)
		return NVC_BAD_SLOT;
	if (running != slot)
		return NVC_NOT_RUNNING;

	n1 = io->read(io->ctx, 1, c1);
	n2 = io->read(io->ctx, 2, c2);
	ok1 = n1 && env_valid(c1, n1);
	ok2 = n2 && env_valid(c2, n2);
	if (!ok1 || !ok2)
		return NVC_NO_PAIR;
	r->primary = env_pick(c1, 1, c2, 1);
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

	if (!has(p, pn, "sw_commit", want)) {
		if (!env_set(p, pn, "sw_commit", want))
			return NVC_NO_ROOM;
		rc = put(io, r->primary, p, pn, NVC_WRITE_PRIMARY, NVC_VERIFY_PRIMARY);
		if (rc != NVC_DONE)
			return rc;
		r->wrote_primary = 1;
	}
	if (!has(f, fn, "sw_commit", want)) {
		if (!env_set(f, fn, "sw_commit", want))
			return NVC_NO_ROOM;
		rc = put(io, r->fallback, f, fn, NVC_WRITE_FALLBACK, NVC_VERIFY_FALLBACK);
		if (rc != NVC_DONE)
			return rc;
		r->wrote_fallback = 1;
	}

	/* Both again, from the partitions, as a reader will see them: still
	 * both valid, the same copy still winning (env_set keeps the flags
	 * byte), sw_commit in both, sw_active untouched. */
	n1 = io->read(io->ctx, 1, c1);
	n2 = io->read(io->ctx, 2, c2);
	if (!n1 || !n2 || !env_valid(c1, n1) || !env_valid(c2, n2))
		return NVC_FINAL;
	if (env_pick(c1, 1, c2, 1) != r->primary)
		return NVC_FINAL;
	p  = r->primary == 1 ? c1 : c2;
	pn = r->primary == 1 ? n1 : n2;
	if (!has(c1, n1, "sw_commit", want) || !has(c2, n2, "sw_commit", want) ||
	    !has(p, pn, "sw_active", want))
		return NVC_FINAL;
	return r->wrote_primary || r->wrote_fallback ? NVC_DONE : NVC_ALREADY;
}

const char *env_commit_msg(int rc)
{
	switch (rc) {
	case NVC_DONE:            return "committed in both copies, read back";
	case NVC_ALREADY:         return "both copies already commit this slot, nothing written";
	case NVC_BAD_SLOT:        return "the slot must be 0 or 1";
	case NVC_NOT_RUNNING:     return "that is not the slot this kernel was booted from (root= in /proc/cmdline), nothing written";
	case NVC_NO_PAIR:         return "both environment copies must be readable and valid, nothing written";
	case NVC_NOT_ACTIVE:      return "sw_active in the primary copy does not name that slot, nothing written";
	case NVC_NO_ROOM:         return "sw_commit does not fit in the environment block";
	case NVC_WRITE_PRIMARY:   return "the write of the primary copy failed; the fallback copy was not touched";
	case NVC_VERIFY_PRIMARY:  return "the primary copy did not read back as written; the fallback copy was not touched";
	case NVC_WRITE_FALLBACK:  return "the primary copy is committed and verified, the write of the fallback copy failed";
	case NVC_VERIFY_FALLBACK: return "the primary copy is committed and verified, the fallback copy did not read back as written";
	case NVC_FINAL:           return "the final read-back of both copies does not agree";
	default:                  return "unknown result";
	}
}
