/* Host-side tests for `nv commit` (commit.c), against a fake pair of
 * environment partitions that can fail at every step the real one can: a
 * write that erases and then dies, a write that lands wrong, a copy that
 * cannot be read or fails its CRC.
 *
 *     cc -o t test_commit.c ../commit.c ../env.c && ./t
 */
#include <stdio.h>
#include <string.h>
#include "../env.h"
#include "../commit.h"

#define PART 8192

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

/* The fake flash: two partitions and the ways each can misbehave. */
struct fake {
	uint8_t part[3][PART];      /* [1] env, [2] env2 */
	int unreadable[3];
	int write_dies[3];          /* erase lands, the write never does */
	int write_flips[3];         /* the write lands with one bit wrong */
	int reads, garble_from;     /* reads of env2 return junk from this read on */
	int writes[8], nwrites;     /* which copy, in order */
};

static uint32_t f_read(void *ctx, int copy, uint8_t *buf)
{
	struct fake *f = ctx;

	f->reads++;
	if (f->unreadable[copy])
		return 0;
	memcpy(buf, f->part[copy], PART);
	if (copy == 2 && f->garble_from && f->reads >= f->garble_from)
		buf[ENV_HDR_LEN + 3] ^= 0x01;
	return PART;
}

static int f_write(void *ctx, int copy, const uint8_t *buf, uint32_t len)
{
	struct fake *f = ctx;

	if (len != PART)
		return 0;
	if (f->nwrites < 8)
		f->writes[f->nwrites++] = copy;
	memset(f->part[copy], 0xff, PART);
	if (f->write_dies[copy])
		return 0;
	memcpy(f->part[copy], buf, PART);
	if (f->write_flips[copy])
		f->part[copy][ENV_HDR_LEN + 1] ^= 0x04;
	return 1;
}

static void build(uint8_t *b, uint8_t flags, const char *const *pairs, int n)
{
	uint32_t w = ENV_HDR_LEN;

	memset(b, 0, PART);
	b[ENV_HDR_CRC] = flags;
	for (int i = 0; i < n; i++) {
		size_t l = strlen(pairs[i]);

		memcpy(b + w, pairs[i], l);
		w += l;
		b[w++] = 0;
	}
	b[w++] = 0;
	env_crc_store(b, env_crc32(b + ENV_HDR_LEN, PART - ENV_HDR_LEN));
}

/* The shape a trial leaves: the operator armed sw_tryactive=1 from a stick
 * committed to slot 0, which wrote env (flags 0); U-Boot then booted slot 1
 * and saved, which landed in env2 with a higher flags byte. env2 is the
 * primary and says sw_active=1 sw_commit=0; env is the fallback and still
 * says sw_active=0 sw_tryactive=1. */
static void trial(struct fake *f)
{
	const char *fb[] = { "bootdelay=1", "sw_active=0", "sw_commit=0",
			     "sw_tryactive=1", "sw_version0=V1", "sw_version1=V2" };
	const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=0",
			     "sw_tryactive=2", "sw_version0=V1", "sw_version1=V2" };

	memset(f, 0, sizeof *f);
	build(f->part[1], 0x00, fb, 6);
	build(f->part[2], 0x01, pr, 6);
}

static int val(struct fake *f, int copy, const char *key, const char *want)
{
	char v[64];

	return env_get(f->part[copy], PART, key, v, sizeof v) && !strcmp(v, want);
}

/* Every pair other than sw_commit is byte-identical before and after. */
static int others_same(const uint8_t *a, const uint8_t *b)
{
	uint32_t pa = 0;
	const char *x;
	int n = 0;

	while ((x = env_next(a, PART, &pa)) != 0) {
		char v[64];
		char k[64];
		const char *eq = strchr(x, '=');

		if (!eq || (size_t)(eq - x) >= sizeof k)
			return 0;
		memcpy(k, x, (size_t)(eq - x));
		k[eq - x] = 0;
		if (!strcmp(k, "sw_commit"))
			continue;
		if (!env_get(b, PART, k, v, sizeof v) || strcmp(v, eq + 1))
			return 0;
		n++;
	}
	return n > 0;
}

int main(void)
{
	static struct fake f;
	static uint8_t before1[PART], before2[PART];
	struct env_io io = { &f, f_read, f_write };
	struct env_commit_report r;
	int rc;

	/* env_pick, the one rule both the reader and commit use. */
	{
		uint8_t a[8] = { 0 }, b[8] = { 0 };

		a[ENV_HDR_CRC] = 0; b[ENV_HDR_CRC] = 1;
		ck(env_pick(a, 1, b, 1) == 2, "pick: the higher flags byte wins");
		a[ENV_HDR_CRC] = 5; b[ENV_HDR_CRC] = 5;
		ck(env_pick(a, 1, b, 1) == 1, "pick: a tie goes to env");
		ck(env_pick(a, 0, b, 1) == 2 && env_pick(a, 1, b, 0) == 1,
		   "pick: one valid copy is used whatever its flags");
		ck(env_pick(a, 0, b, 0) == 0, "pick: none valid is 0");
	}

	/* The trial, committed. */
	trial(&f);
	memcpy(before1, f.part[1], PART);
	memcpy(before2, f.part[2], PART);
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_DONE, "a trial of slot 1, running slot 1: committed");
	ck(r.primary == 2 && r.fallback == 1, "env2 (higher flags) is the primary");
	ck(f.nwrites == 2 && f.writes[0] == 2 && f.writes[1] == 1,
	   "the primary is written first, the fallback second");
	ck(val(&f, 1, "sw_commit", "1") && val(&f, 2, "sw_commit", "1"),
	   "both copies read sw_commit=1");
	ck(val(&f, 2, "sw_active", "1") && val(&f, 1, "sw_active", "0"),
	   "sw_active is untouched in both copies");
	ck(f.part[1][ENV_HDR_CRC] == 0x00 && f.part[2][ENV_HDR_CRC] == 0x01,
	   "both flags bytes are kept, so the same copy goes on winning");
	ck(env_valid(f.part[1], PART) && env_valid(f.part[2], PART),
	   "both copies verify");
	ck(others_same(before1, f.part[1]) && others_same(before2, f.part[2]),
	   "no pair other than sw_commit changed in either copy");

	/* Again: nothing to do, nothing written. */
	f.nwrites = 0;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_ALREADY && f.nwrites == 0, "a second commit writes nothing");

	/* Half committed by hand (a plain setenv on the primary only). */
	trial(&f);
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=1",
				     "sw_tryactive=2" };
		build(f.part[2], 0x01, pr, 4);
	}
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_DONE && f.nwrites == 1 && f.writes[0] == 1 && r.wrote_fallback &&
	   !r.wrote_primary && val(&f, 1, "sw_commit", "1"),
	   "primary already committed: only the fallback is written");

	/* Refusals: nothing written in any of them. */
	trial(&f);
	rc = env_commit(&io, 0, 1, &r);
	ck(rc == NVC_NOT_RUNNING && f.nwrites == 0, "a slot that is not running is refused");
	rc = env_commit(&io, 1, -1, &r);
	ck(rc == NVC_NOT_RUNNING && f.nwrites == 0, "an unknown running slot is refused");
	rc = env_commit(&io, 2, 2, &r);
	ck(rc == NVC_BAD_SLOT && f.nwrites == 0, "slot 2 is refused");
	rc = env_commit(&io, 0, 0, &r);
	ck(rc == NVC_NOT_ACTIVE && f.nwrites == 0,
	   "running slot 0 but sw_active=1 in the primary: refused");
	{
		const char *pr[] = { "bootdelay=1", "sw_commit=0" };

		build(f.part[2], 0x01, pr, 2);
		rc = env_commit(&io, 1, 1, &r);
		ck(rc == NVC_NOT_ACTIVE && f.nwrites == 0, "no sw_active at all: refused");
	}
	trial(&f);
	f.part[1][ENV_HDR_LEN + 2] ^= 0x10;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_NO_PAIR && f.nwrites == 0, "the fallback failing its CRC: refused");
	trial(&f);
	f.part[2][ENV_HDR_LEN + 2] ^= 0x10;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_NO_PAIR && f.nwrites == 0,
	   "the primary failing its CRC: refused, not committed to the other copy");
	trial(&f);
	f.unreadable[1] = 1;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_NO_PAIR && f.nwrites == 0, "an unreadable copy: refused");

	/* The primary fails: the fallback must not be touched. */
	trial(&f);
	memcpy(before1, f.part[1], PART);
	f.write_dies[2] = 1;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_WRITE_PRIMARY && f.nwrites == 1 && !memcmp(before1, f.part[1], PART),
	   "the primary write dies: the fallback is left exactly as it was");
	ck(env_valid(f.part[1], PART) && val(&f, 1, "sw_commit", "0"),
	   "and still verifies, naming the old committed slot");
	trial(&f);
	memcpy(before1, f.part[1], PART);
	f.write_flips[2] = 1;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_VERIFY_PRIMARY && f.nwrites == 1 && !memcmp(before1, f.part[1], PART),
	   "the primary reads back wrong: the fallback is not written");

	/* The fallback fails after the primary verified. */
	trial(&f);
	f.write_dies[1] = 1;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_WRITE_FALLBACK && r.wrote_primary && val(&f, 2, "sw_commit", "1"),
	   "the fallback write dies: reported, the primary is already committed");
	trial(&f);
	f.write_flips[1] = 1;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_VERIFY_FALLBACK, "the fallback reads back wrong: reported");

	/* Everything wrote and verified, and the final read disagrees. */
	trial(&f);
	f.garble_from = 5;          /* reads 1-2 load, 3 and 4 verify, 5-6 final */
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_FINAL, "a final read-back that does not verify is reported");

	/* Equal flags: env is the primary, and is written first. */
	{
		const char *pr[] = { "sw_active=0", "sw_commit=1" };
		const char *fb[] = { "sw_active=1", "sw_commit=1" };

		memset(&f, 0, sizeof f);
		build(f.part[1], 0x03, pr, 2);
		build(f.part[2], 0x03, fb, 2);
		rc = env_commit(&io, 0, 0, &r);
		ck(rc == NVC_DONE && r.primary == 1 && f.writes[0] == 1 && f.writes[1] == 2,
		   "a flags tie: env is the primary and goes first");
	}

	for (int i = NVC_DONE; i <= NVC_FINAL; i++)
		if (!strcmp(env_commit_msg(i), "unknown result"))
			ck(0, "every result has a message");

	printf("%s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
