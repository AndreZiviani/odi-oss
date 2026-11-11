/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

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
	int tear_at;                /* the Nth write (1-based) is cut half way: first half lands, rest stays erased */
	int refuse_from;            /* from the Nth write on, the write is refused before it erases anything */
	int lost;                   /* writes begun while the OTHER copy was not valid: an erase that could leave none */
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
	if (f->refuse_from && f->nwrites + 1 >= f->refuse_from)
		return 0;
	if (!env_valid(f->part[3 - copy], PART))
		f->lost++;
	if (f->nwrites < 8)
		f->writes[f->nwrites++] = copy;
	memset(f->part[copy], 0xff, PART);
	if (f->tear_at && f->nwrites == f->tear_at) {
		memcpy(f->part[copy], buf, PART / 2);
		return 0;
	}
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
			     "sw_tryactive=1", "sw_version0=V1", "sw_version1=V2",
			     "mupgrade_en=1" };
	const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=0",
			     "sw_tryactive=2", "sw_version0=V1", "sw_version1=V2",
			     "mupgrade_en=1" };

	memset(f, 0, sizeof *f);
	build(f->part[1], 0x00, fb, 7);
	build(f->part[2], 0x01, pr, 7);
}

static int val(struct fake *f, int copy, const char *key, const char *want)
{
	char v[64];

	return env_get(f->part[copy], PART, key, v, sizeof v) && !strcmp(v, want);
}

/* Every pair other than sw_commit and mupgrade_en is byte-identical before and after. */
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
		if (!strcmp(k, "sw_commit") || !strcmp(k, "mupgrade_en"))
			continue;
		if (!env_get(b, PART, k, v, sizeof v) || strcmp(v, eq + 1))
			return 0;
		n++;
	}
	return n > 0;
}

/* Which of the two toggle files exist. */
struct present { int confirm_arp, breadcrumbs; };

static int p_exists(void *ctx, const char *path)
{
	struct present *p = ctx;

	return strstr(path, "confirm-arp") ? p->confirm_arp : strstr(path, "breadcrumbs") ? p->breadcrumbs : 0;
}


static struct fake rf;
static struct env_io rio = { &rf, f_read, f_write };

static int valid_count(struct fake *f)
{
	return env_valid(f->part[1], PART) + env_valid(f->part[2], PART);
}

static int pick_now(struct fake *f)
{
	return env_pick(f->part[1], env_valid(f->part[1], PART),
			f->part[2], env_valid(f->part[2], PART));
}

/* The trial pair with one copy damaged (a write cut by a power loss: the
 * first half landed, the rest is erased flash). */
static void damaged(struct fake *f, int bad)
{
	trial(f);
	memset(f->part[bad] + PART / 2, 0xff, PART / 2);
}

static void repair_tests(void)
{
	struct env_commit_report r;
	struct env_set_report sr;
	static uint8_t keep[PART];
	int rc, keeps, w;

	/* The flags byte rule. */
	ck(env_repair_flags(2, 0, &keeps) == 0 && keeps, "flags: env2 repaired beside env 0: 0, a tie env wins");
	ck(env_repair_flags(2, 7, &keeps) == 7 && keeps, "flags: env2 repaired beside env 7: 7, a tie env wins");
	ck(env_repair_flags(1, 1, &keeps) == 0 && keeps, "flags: env repaired beside env2 1: 0, env2 keeps winning");
	ck(env_repair_flags(1, 9, &keeps) == 8 && keeps, "flags: env repaired beside env2 9: 8");
	ck(env_repair_flags(1, 0, &keeps) == 0 && !keeps,
	   "flags: env repaired beside env2 0: nothing lower, env would win, reported");
	{
		uint8_t a[8] = { 0 }, b[8] = { 0 };

		for (int wf = 0; wf < 256; wf++) {
			a[ENV_HDR_CRC] = (uint8_t)wf;                         /* env2 wins with wf */
			b[ENV_HDR_CRC] = env_repair_flags(1, (uint8_t)wf, &keeps);
			if (keeps != (env_pick(b, 1, a, 1) == 2))
				ck(0, "flags: keeps agrees with env_pick (env repaired)");
			b[ENV_HDR_CRC] = (uint8_t)wf;                         /* env wins with wf */
			a[ENV_HDR_CRC] = env_repair_flags(2, (uint8_t)wf, &keeps);
			if (!keeps || env_pick(b, 1, a, 1) != 1)
				ck(0, "flags: env2 repaired never beats env");
		}
		ck(1, "flags: all 256 winner values checked against env_pick");
	}

	/* commit, env invalid (the loser of the trial layout): repaired first. */
	damaged(&rf, 1);
	memcpy(keep, rf.part[2], PART);
	rc = env_commit(&rio, 1, 1, &r);
	ck(rc == NVC_DONE && r.primary == 2 && r.repaired == 1, "commit, env invalid: committed, env reported repaired");
	ck(rf.nwrites == 2 && rf.writes[0] == 1 && rf.writes[1] == 2, "  the invalid copy is written first, the valid one second");
	ck(rf.lost == 0, "  no erase began while the other copy was not valid");
	ck(valid_count(&rf) == 2 && val(&rf, 1, "sw_commit", "1") && val(&rf, 2, "sw_commit", "1") &&
	   val(&rf, 1, "mupgrade_en", "0") && val(&rf, 2, "mupgrade_en", "0"), "  both copies valid and committed");
	ck(val(&rf, 1, "sw_active", "1") && val(&rf, 2, "sw_active", "1"), "  sw_active carried over from the valid copy, not invented");
	ck(rf.part[1][ENV_HDR_CRC] == 0x00 && rf.part[2][ENV_HDR_CRC] == 0x01 && pick_now(&rf) == 2,
	   "  flags 0x00/0x01, env2 still the one picked");

	/* ...and the state between the two writes: repaired copy verified, the
	 * valid one still as it was, the pick unchanged. */
	damaged(&rf, 1);
	memcpy(keep, rf.part[2], PART);
	rf.refuse_from = 2;
	rc = env_commit(&rio, 1, 1, &r);
	ck(rc == NVC_WRITE_PRIMARY && rf.nwrites == 1 && rf.writes[0] == 1, "commit cut between the two writes: reported");
	ck(!memcmp(keep, rf.part[2], PART), "  the valid copy is byte for byte what it was");
	ck(valid_count(&rf) == 2 && pick_now(&rf) == 2 && val(&rf, 1, "sw_commit", "1") && val(&rf, 2, "sw_commit", "0"),
	   "  repaired copy valid with the new value, the old copy still picked");

	/* commit, env2 invalid: env is the primary; the repaired env2 ties and env keeps winning. */
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=0", "sw_tryactive=2" };

		memset(&rf, 0, sizeof rf);
		build(rf.part[1], 0x00, pr, 4);
		build(rf.part[2], 0x01, pr, 4);
		memset(rf.part[2] + 100, 0xff, 200);
		rf.refuse_from = 2;
		rc = env_commit(&rio, 1, 1, &r);
		ck(rc == NVC_WRITE_PRIMARY && rf.writes[0] == 2 && r.primary == 1 && r.repaired == 2, "commit, env2 invalid: env2 first");
		ck(valid_count(&rf) == 2 && pick_now(&rf) == 1 && rf.part[2][ENV_HDR_CRC] == 0x00,
		   "  repaired env2 gets the flags byte of env, a tie, env still picked");
		rf.refuse_from = 0;
		rf.nwrites = 0;
		rc = env_commit(&rio, 1, 1, &r);
		ck(rc == NVC_DONE && pick_now(&rf) == 1 && val(&rf, 1, "sw_commit", "1") && val(&rf, 2, "sw_commit", "1"),
		   "  run again: committed in both");
	}

	/* The corner: env2 holds flags 0, env is invalid. */
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=0" };

		memset(&rf, 0, sizeof rf);
		build(rf.part[2], 0x00, pr, 3);
		build(rf.part[1], 0x00, pr, 3);
		memset(rf.part[1] + 100, 0xff, 200);
		rc = env_commit(&rio, 1, 1, &r);
		ck(rc == NVC_DONE && rf.lost == 0 && valid_count(&rf) == 2 && val(&rf, 1, "sw_commit", "1") &&
		   val(&rf, 2, "sw_commit", "1"), "env2 flags 0, env invalid: committed, both valid, none lost");
		ck(rf.writes[0] == 1 && rf.writes[1] == 2, "  repaired copy still first");
	}

	/* The valid copy already committed: only the invalid one is written. */
	damaged(&rf, 1);
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=1", "mupgrade_en=0" };

		build(rf.part[2], 0x01, pr, 4);
	}
	rc = env_commit(&rio, 1, 1, &r);
	ck(rc == NVC_DONE && rf.nwrites == 1 && rf.writes[0] == 1 && !r.wrote_primary && r.wrote_fallback,
	   "commit, valid copy already committed: just the invalid one is written");

	/* Refusals kept with one invalid copy. */
	damaged(&rf, 1);
	rc = env_commit(&rio, 0, 1, &r);
	ck(rc == NVC_NOT_RUNNING && rf.nwrites == 0, "one invalid copy: another slot than the running one still refused");
	rc = env_commit(&rio, 0, 0, &r);
	ck(rc == NVC_NOT_ACTIVE && rf.nwrites == 0, "one invalid copy: slot the valid copy does not name as sw_active refused");
	rc = env_commit(&rio, 2, 2, &r);
	ck(rc == NVC_BAD_SLOT && rf.nwrites == 0, "one invalid copy: slot 2 refused");
	trial(&rf);
	memset(rf.part[2] + PART / 2, 0xff, PART / 2);          /* env2 (sw_active=1) invalid; env says sw_active=0 */
	rc = env_commit(&rio, 1, 1, &r);
	ck(rc == NVC_NOT_ACTIVE && rf.nwrites == 0, "only the stale copy valid (sw_active=0), running slot 1: refused, nothing written");

	/* Faults on the repair write: the valid copy is not touched. */
	damaged(&rf, 1);
	memcpy(keep, rf.part[2], PART);
	rf.write_dies[1] = 1;
	rc = env_commit(&rio, 1, 1, &r);
	ck(rc == NVC_WRITE_REPAIR && rf.nwrites == 1 && !memcmp(keep, rf.part[2], PART) && valid_count(&rf) == 1,
	   "repair write dies: reported, the valid copy untouched and still valid");
	damaged(&rf, 1);
	memcpy(keep, rf.part[2], PART);
	rf.write_flips[1] = 1;
	rc = env_commit(&rio, 1, 1, &r);
	ck(rc == NVC_VERIFY_REPAIR && rf.nwrites == 1 && !memcmp(keep, rf.part[2], PART),
	   "repair reads back wrong: reported, the valid copy untouched");
	damaged(&rf, 1);
	{
		/* No room: nothing may be erased. */
		static char big[ENV_MAX];

		memset(big, 'x', sizeof big - 1);
		rc = env_setenv(&rio, 0, "k", big, &sr);
		ck(rc == NVS_NO_ROOM && rf.nwrites == 0, "setenv with a value that does not fit: nothing erased");
		rc = env_setenv(&rio, 0, "nosuchkey", NULL, &sr);
		ck(rc == NVS_NO_ROOM && rf.nwrites == 0, "setenv deleting an absent key: nothing erased");
	}

	/* Power cut (a torn write) at every step: never zero valid copies,
	 * and no erase ever begins while the other copy is invalid. */
	for (int bad = 1; bad <= 2; bad++) {
		for (int step = 1; step <= 2; step++) {
			damaged(&rf, bad);
			rf.tear_at = step;
			rc = env_commit(&rio, 1, 1, &r);
			ck(rc != NVC_DONE && rf.lost == 0 && valid_count(&rf) >= 1,
			   "commit, power cut at a write, one copy invalid: a valid copy is left");
			damaged(&rf, bad);
			rf.tear_at = step;
			rc = env_setenv(&rio, 0, "sw_tryactive", "1", &sr);
			ck(rc != NVS_DONE && rf.lost == 0 && valid_count(&rf) >= 1,
			   "setenv, power cut at a write, one copy invalid: a valid copy is left");
			/* the same, then the next run finishes the repair */
			rf.tear_at = 0;
			rc = env_setenv(&rio, 0, "sw_tryactive", "1", &sr);
			ck(rc == NVS_DONE && valid_count(&rf) == 2 && val(&rf, 1, "sw_tryactive", "1") &&
			   val(&rf, 2, "sw_tryactive", "1"), "  and a second setenv completes the repair, both agree");
		}
	}

	/* setenv. Both valid: as before, one write to the copy read. */
	trial(&rf);
	rc = env_setenv(&rio, 0, "sw_tryactive", "0", &sr);
	ck(rc == NVS_DONE && rf.nwrites == 1 && rf.writes[0] == 2 && !sr.repaired && val(&rf, 2, "sw_tryactive", "0") &&
	   val(&rf, 1, "sw_tryactive", "1"), "setenv, both valid: only the copy read is written");
	trial(&rf);
	rc = env_setenv(&rio, 1, "sw_tryactive", "0", &sr);
	ck(rc == NVS_DONE && rf.nwrites == 1 && rf.writes[0] == 1 && rf.part[1][ENV_HDR_CRC] == 0 &&
	   rf.part[2][ENV_HDR_CRC] == 1, "setenv -c 1, both valid: that copy only, flags kept");
	trial(&rf);
	rf.write_flips[2] = 1;
	rc = env_setenv(&rio, 0, "sw_tryactive", "0", &sr);
	ck(rc == NVS_VERIFY, "setenv, both valid: a bad read-back is reported");

	/* One invalid copy. */
	for (w = 1; w <= 2; w++) {
		int bad = 3 - w;

		damaged(&rf, bad);
		memcpy(keep, rf.part[w], PART);
		rf.refuse_from = 2;
		rc = env_setenv(&rio, 0, "sw_tryactive", "0", &sr);
		ck(rc == NVS_WRITE && rf.nwrites == 1 && rf.writes[0] == bad && sr.repaired == bad,
		   "setenv, one copy invalid: the invalid copy is written first");
		ck(!memcmp(keep, rf.part[w], PART) && pick_now(&rf) == w && valid_count(&rf) == 2,
		   "  the valid copy untouched until the read-back passed, both valid, same copy picked");
		rf.refuse_from = 0;
		rf.nwrites = 0;
		damaged(&rf, bad);
		rc = env_setenv(&rio, 0, "sw_tryactive", "0", &sr);
		ck(rc == NVS_DONE && rf.nwrites == 2 && rf.writes[0] == bad && rf.writes[1] == w && rf.lost == 0,
		   "  then the valid copy is rewritten, so the change takes effect");
		ck(val(&rf, 1, "sw_tryactive", "0") && val(&rf, 2, "sw_tryactive", "0") && valid_count(&rf) == 2,
		   "  both copies agree and verify");
		ck(rf.part[1][ENV_HDR_CRC] == 0x00 && rf.part[2][ENV_HDR_CRC] == (w == 2 ? 0x01 : 0x00) && pick_now(&rf) == w,
		   "  flags: env2 0x01 as before when it was the valid one, a tie 0x00/0x00 when env2 was repaired; the same copy picked");
	}
	damaged(&rf, 1);
	memcpy(keep, rf.part[2], PART);
	rc = env_setenv(&rio, 1, "sw_tryactive", "0", &sr);
	ck(rc == NVS_DONE && rf.nwrites == 1 && rf.writes[0] == 1 && !memcmp(keep, rf.part[2], PART) &&
	   val(&rf, 1, "sw_tryactive", "0") && valid_count(&rf) == 2, "setenv -c naming the invalid copy: only it is written");
	damaged(&rf, 1);
	rc = env_setenv(&rio, 2, "sw_tryactive", "0", &sr);
	ck(rc == NVS_DONE && rf.nwrites == 2 && rf.writes[0] == 1 && rf.writes[1] == 2 && rf.lost == 0,
	   "setenv -c naming the only valid copy: the invalid one is still repaired first");
	damaged(&rf, 1);
	rf.write_dies[1] = 1;
	memcpy(keep, rf.part[2], PART);
	rc = env_setenv(&rio, 0, "sw_tryactive", "0", &sr);
	ck(rc == NVS_WRITE_REPAIR && !memcmp(keep, rf.part[2], PART), "setenv: repair write dies, the valid copy untouched");
	damaged(&rf, 1);
	rf.write_flips[1] = 1;
	memcpy(keep, rf.part[2], PART);
	rc = env_setenv(&rio, 0, "sw_tryactive", "0", &sr);
	ck(rc == NVS_VERIFY_REPAIR && !memcmp(keep, rf.part[2], PART), "setenv: repair reads back wrong, the valid copy untouched");
	damaged(&rf, 1);
	memset(rf.part[2] + PART / 2, 0xff, PART / 2);
	rc = env_setenv(&rio, 0, "k", "v", &sr);
	ck(rc == NVS_NONE_VALID && rf.nwrites == 0, "setenv, no valid copy: refused, nothing written");
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
	   "no pair other than sw_commit and mupgrade_en changed in either copy");
	ck(val(&f, 1, "mupgrade_en", "0") && val(&f, 2, "mupgrade_en", "0"),
	   "both copies read mupgrade_en=0 (it was 1)");

	/* Again: nothing to do, nothing written. */
	f.nwrites = 0;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_ALREADY && f.nwrites == 0, "a second commit writes nothing");

	/* Half committed by hand (a plain setenv on the primary only). */
	trial(&f);
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=1",
				     "sw_tryactive=2", "mupgrade_en=0" };
		build(f.part[2], 0x01, pr, 5);
	}
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_DONE && f.nwrites == 1 && f.writes[0] == 1 && r.wrote_fallback &&
	   !r.wrote_primary && val(&f, 1, "sw_commit", "1"),
	   "primary already committed: only the fallback is written");

	/* A copy with no mupgrade_en gets it; one already 0 is not rewritten
	 * for it; sw_commit already right but mupgrade_en=1 still writes. */
	trial(&f);
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=0" };
		const char *fb[] = { "bootdelay=1", "sw_active=0", "sw_commit=1",
				     "mupgrade_en=0" };

		build(f.part[2], 0x01, pr, 3);
		build(f.part[1], 0x00, fb, 4);
		memcpy(before1, f.part[1], PART);
	}
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_DONE && f.nwrites == 1 && f.writes[0] == 2 && !r.wrote_fallback &&
	   !memcmp(before1, f.part[1], PART) && val(&f, 2, "mupgrade_en", "0") &&
	   val(&f, 2, "sw_commit", "1"),
	   "variable absent in the primary: added with sw_commit; a fallback already right is untouched");
	trial(&f);
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=1",
				     "mupgrade_en=1" };

		build(f.part[2], 0x01, pr, 4);
		build(f.part[1], 0x00, pr, 4);
	}
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_DONE && f.nwrites == 2 && val(&f, 1, "mupgrade_en", "0") &&
	   val(&f, 2, "mupgrade_en", "0"),
	   "sw_commit already right but mupgrade_en=1: both copies still written once each");
	{
		const char *pr[] = { "bootdelay=1", "sw_active=1", "sw_commit=1",
				     "mupgrade_en=0" };

		build(f.part[2], 0x01, pr, 4);
		build(f.part[1], 0x00, pr, 4);
	}
	f.nwrites = 0;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_ALREADY && f.nwrites == 0, "both already committed with mupgrade_en=0: nothing written");

	/* Interrupted between the two copies: the primary carries both
	 * changes, the fallback is still the old valid copy. */
	trial(&f);
	f.write_dies[1] = 1;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_WRITE_FALLBACK && val(&f, 2, "sw_commit", "1") &&
	   val(&f, 2, "mupgrade_en", "0") && env_valid(f.part[2], PART),
	   "interrupted before the fallback: the primary is valid and has both changes");

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
	/* One invalid copy is repaired, not refused (repair_tests below);
	 * both invalid, or both unreadable, is. */
	trial(&f);
	f.part[1][ENV_HDR_LEN + 2] ^= 0x10;
	f.part[2][ENV_HDR_LEN + 2] ^= 0x10;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_NO_PAIR && f.nwrites == 0, "both copies failing their CRC: refused");
	trial(&f);
	f.unreadable[1] = 1;
	f.unreadable[2] = 1;
	rc = env_commit(&io, 1, 1, &r);
	ck(rc == NVC_NO_PAIR && f.nwrites == 0, "both copies unreadable: refused");

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

	/* The running slot's root=: the last one, as the kernel takes it. The
	 * built-in slot-0 line comes first on every boot (issue #42). */
	{
		const char *b = "console=ttyS0,115200 root=31:5 mtdparts=x print-fatal-signals=1";

		ck(cmdline_root_index("console=ttyS0,115200 root=31:5 mtdparts=x\n") == 5,
		   "one root=: slot 0");
		ck(cmdline_root_index("console=ttyS0,115200 mtdparts=y root=31:7\n") == 7,
		   "one root=: slot 1");
		{
			char two[256];

			snprintf(two, sizeof two, "%s %s", b, "console=ttyS0,115200 mtdparts=y root=31:7\n");
			ck(cmdline_root_index(two) == 7, "built-in root=31:5 then U-Boot root=31:7: the last wins");
			snprintf(two, sizeof two, "%s %s", b, "console=ttyS0,115200 mtdparts=y root=31:5\n");
			ck(cmdline_root_index(two) == 5, "both root=31:5: slot 0");
		}
		ck(cmdline_root_index("root=31:7 root=/dev/ram\n") == -1,
		   "a last root= that is not 31:N: unknown");
		ck(cmdline_root_index("console=ttyS0 noroot=31:7\n") == -1, "no root=: unknown");
		ck(cmdline_root_index("root=31:7x\n") == -1, "trailing junk: unknown");
		ck(cmdline_root_index("root=31:\n") == -1, "no index: unknown");
	}

	/* The development-aid guard (confirm-arp refuses, breadcrumbs.on only
	 * reports). The fake "filesystem" is a list of existing paths. */
	{
		struct fake f;
		struct env_commit_report r;
		struct commit_aids a;
		struct present p;
		struct env_io io = { &f, f_read, f_write };
		int rc;

		memset(&a, 0, sizeof a);
		a.ctx = &p;
		a.exists = p_exists;
		a.confirm_arp = "/x/confirm-arp";
		a.breadcrumbs = "/x/breadcrumbs.on";

		trial(&f);
		memset(&p, 0, sizeof p);
		rc = env_commit_guarded(&io, &a, 0, 1, 1, &r);
		ck(rc == NVC_DONE && !a.has_confirm_arp && !a.has_breadcrumbs && f.nwrites == 2,
		   "neither toggle: a normal commit, nothing flagged");

		trial(&f);
		memset(&p, 0, sizeof p);
		p.confirm_arp = 1;
		rc = env_commit_guarded(&io, &a, 0, 1, 1, &r);
		ck(rc == NVC_CONFIRM_ARP && a.has_confirm_arp, "confirm-arp present: refused");
		ck(f.nwrites == 0 && f.reads == 0, "  and nothing was read or written");
		ck(val(&f, 1, "sw_commit", "0") && val(&f, 2, "sw_commit", "0"), "  both copies still commit slot 0");

		rc = env_commit_guarded(&io, &a, 1, 1, 1, &r);
		ck(rc == NVC_DONE && a.has_confirm_arp && val(&f, 1, "sw_commit", "1") && val(&f, 2, "sw_commit", "1"),
		   "confirm-arp present with force: committed in both copies, still flagged for the warning");

		trial(&f);
		memset(&p, 0, sizeof p);
		p.breadcrumbs = 1;
		rc = env_commit_guarded(&io, &a, 0, 1, 1, &r);
		ck(rc == NVC_DONE && a.has_breadcrumbs && !a.has_confirm_arp,
		   "breadcrumbs.on only: committed, flagged for the note");

		trial(&f);
		memset(&p, 0, sizeof p);
		p.confirm_arp = 1;
		rc = env_commit_guarded(&io, &a, 0, 0, 1, &r);
		ck(rc == NVC_CONFIRM_ARP, "the refusal comes before the slot checks");
		rc = env_commit_guarded(&io, &a, 1, 0, 1, &r);
		ck(rc == NVC_NOT_RUNNING, "force does not skip the other refusals");
	}

	repair_tests();

	for (int i = NVC_DONE; i <= NVC_VERIFY_REPAIR; i++)
		if (!strcmp(env_commit_msg(i), "unknown result"))
			ck(0, "every result has a message");
	for (int i = NVS_DONE; i <= NVS_VERIFY; i++)
		if (!strcmp(env_setenv_msg(i), "unknown result"))
			ck(0, "every setenv result has a message");

	printf("%s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
