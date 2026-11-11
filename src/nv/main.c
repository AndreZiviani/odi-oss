/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* nv — read and write the U-Boot environment, without libmib.
 *
 * The stock /bin/nv is a 15 KB CLI over the environment accessors in
 * libmib.so, which is proprietary. Everything it needs is in /proc/mtd and a
 * crc32, so this replaces it with no vendor library:
 *
 *     nv getenv [parameter_name]
 *     nv setenv [-c 1|2] parameter_name value
 *     nv fallback [parameter_name]
 *     nv commit [--force] slot
 *
 * It matters more than its size: `sw_tryactive` and `sw_commit` are set
 * through it, and those are the one-shot safe-boot mechanism this project
 * flashes through. An image that cannot write the U-Boot environment cannot
 * arm its own trial boot.
 *
 * See env.h for the block layout, which was read off a device.
 *
 *
 * getenv is verified against the vendor's own nv on a stick, byte for byte.
 * The WRITE path is NOT, and this is deliberately not in the image overlay.
 *
 * On the redundant pair: isp2 has both copies valid, with flags 0x00 in `env`
 * and 0x01 in `env2`, and the vendor reports "Valid environment: 2" -- so the
 * higher flags byte wins. That is enough to read correctly and not enough to
 * settle whether a writer should INCREMENT the flags (a sequence counter) or
 * set them to a fixed active marker; two observations cannot separate those.
 *
 * So setenv writes back the SAME partition it read and preserves the flags
 * byte. That is correct under either reading -- the copy that was already
 * winning goes on winning -- and it degrades the right way: a power cut
 * mid-write leaves that copy invalid and U-Boot falls back to the other, which
 * is stale but good. What it does NOT do is ping-pong between the copies,
 * which is what the redundancy is designed for. Whether U-Boot itself
 * increments the flags byte is still not known, and nv never does.
 *
 * That leaves a second copy that can sit stale forever, and two things exist
 * for it. `fallback` READS it -- it is the value U-Boot uses if the active
 * copy is ever left invalid, which an interrupted setenv is exactly how it
 * happens, so any guard reading sw_commit needs both answers. And `setenv -c
 * N` writes a NAMED copy, which is the only way to make the stale one agree:
 * a plain setenv writes back the copy it read, so the loser stays stale
 * however many times it is run. Because env_set preserves the flags byte,
 * writing the loser leaves it losing -- this cannot change which copy U-Boot
 * picks, only what the loser says.
 *
 * When one copy is INVALID (an interrupted write is how), the valid one is
 * the only good environment and is never erased while it is the only one:
 * setenv and commit build the new block in memory, write it to the invalid
 * copy first, read it back, and only then write the valid copy. That repairs
 * the pair as a side effect of the write the operator asked for, with no
 * separate repair verb. The repaired copy is given the flags byte that keeps
 * the valid copy winning until the sequence is over (env.h,
 * env_repair_flags). commit.h has the sequences.
 *
 * Writers (`setenv`, `commit`) are serialised with flock(2) on the env
 * partition device node (env_lock below), and every copy written is read
 * back and compared.
 *
 * `commit slot` is the one operation that writes both: sw_commit=<slot> into
 * the primary, read back, then into the fallback, read back, then both read
 * again -- refusing any slot that is not the running one (root= in
 * /proc/cmdline) and not sw_active in the primary. commit.h has the
 * sequence and why it is safe to interrupt. It is the one way to commit
 * both copies, so nobody has to act on the NOTE lines setenv prints.
 */
#include "sys.h"
#include "io.h"
#include "env.h"
#include "commit.h"
#include "lock.h"

/* MEMERASE = _IOW('M', 2, struct erase_info_user).
 *
 * MIPS does NOT use the asm-generic direction bits: arch/rlx/include/asm/
 * ioctl.h sets _IOC_NONE 1, _IOC_READ 2, _IOC_WRITE 4 and _IOC_SIZEBITS 13,
 * so this is 0x80084D02 where x86 would compute 0x40084D02. Computed from
 * the ioctl composition formula above, not copied from memory. */
#define MEMERASE 0x80084D02u

struct erase_info_user {
	unsigned int start;
	unsigned int length;
};

struct mtd_part {
	int  index;
	unsigned int size;
	unsigned int erasesize;
	int  copy;               /* 1 for "env", 2 for "env2" -- what the
				  * vendor prints as "Valid environment: N" */
};

static int digit(char c) { return c >= '0' && c <= '9'; }

static unsigned int hex_or_dec(const char **p, int hex)
{
	unsigned int v = 0;
	const char *s = *p;

	for (;;) {
		unsigned int d;

		if (digit(*s))
			d = (unsigned int)(*s - '0');
		else if (hex && *s >= 'a' && *s <= 'f')
			d = (unsigned int)(*s - 'a') + 10;
		else if (hex && *s >= 'A' && *s <= 'F')
			d = (unsigned int)(*s - 'A') + 10;
		else
			break;
		v = v * (hex ? 16u : 10u) + d;
		s++;
	}
	*p = s;
	return v;
}

/* /proc/mtd lines look like:
 *
 *     mtd1: 00002000 00001000 "env"
 *
 * The partition is found by NAME, because the numbering is not fixed: this
 * board has fourteen and the stock nv looks the same way. */
static int mtd_find(const char *want, struct mtd_part *out)
{
	char line[160];
	int fd = (int)sys_open("/proc/mtd", O_RDONLY);

	if (fd < 0)
		return 0;
	while (read_line(fd, line, sizeof line) > 0) {
		const char *p = line;

		if (!str_has_prefix(p, "mtd"))
			continue;
		p += 3;
		out->index = (int)hex_or_dec(&p, 0);
		if (*p != ':')
			continue;
		p++;
		while (*p == ' ')
			p++;
		out->size = hex_or_dec(&p, 1);
		while (*p == ' ')
			p++;
		out->erasesize = hex_or_dec(&p, 1);
		while (*p && *p != '"')
			p++;
		if (*p != '"')
			continue;
		p++;
		int i = 0;
		while (want[i] && p[i] == want[i])
			i++;
		if (want[i] == '\0' && p[i] == '"') {
			sys_close(fd);
			return 1;
		}
	}
	sys_close(fd);
	return 0;
}

static void mtd_path(int index, char *out)
{
	char *p = out;

	for (const char *s = "/dev/mtd"; *s; s++)
		*p++ = *s;
	if (index >= 10)
		*p++ = (char)('0' + index / 10);
	*p++ = (char)('0' + index % 10);
	*p = '\0';
}

static uint8_t blk[ENV_MAX];

/* The copy that did NOT win. Kept because two questions need it and neither
 * can be answered from the winner alone: what U-Boot would fall back to if
 * this copy became invalid, and whether an erase here leaves any good
 * environment at all while the write is in flight. */
static uint8_t      alt[ENV_MAX];
static struct mtd_part alt_part;
static unsigned int alt_len;              /* 0 when there is no valid other copy */

/* The whole partition, raw: its size, or 0 when it cannot be read in full. */
static unsigned int read_raw(const char *name, struct mtd_part *m, uint8_t *dst)
{
	char path[16];
	unsigned int got = 0;
	int fd;

	if (!mtd_find(name, m) || m->size == 0 || m->size > ENV_MAX)
		return 0;
	mtd_path(m->index, path);
	fd = (int)sys_open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	while (got < m->size) {
		long n = sys_read(fd, dst + got, m->size - got);

		if (n <= 0)
			break;
		got += (unsigned int)n;
	}
	sys_close(fd);
	return got == m->size ? m->size : 0;
}

static int read_part(const char *name, struct mtd_part *m, uint8_t *dst)
{
	return read_raw(name, m, dst) && env_valid(dst, m->size);
}

/* Both copies are read, and the valid one with the HIGHER flags byte wins --
 * which is what the vendor does: on isp2, env has flags 0x00, env2 has 0x01,
 * and the stock nv reports "Valid environment: 2". With only one valid copy
 * that one is used, which is the point of the redundant pair. */
static unsigned int env_load(struct mtd_part *part)
{
	static uint8_t other[ENV_MAX];
	struct mtd_part m1, m2;
	int ok1 = read_part("env", &m1, blk);
	int ok2 = read_part("env2", &m2, other);

	alt_len = 0;
	if (ok1)
		m1.copy = 1;
	if (ok2)
		m2.copy = 2;
	if (ok1 && ok2) {
		if (env_pick(blk, ok1, other, ok2) == 2) {
			for (unsigned int i = 0; i < m1.size; i++)
				alt[i] = blk[i];
			alt_part = m1;
			alt_len = m1.size;
			for (unsigned int i = 0; i < m2.size; i++)
				blk[i] = other[i];
			*part = m2;
			return m2.size;
		}
		for (unsigned int i = 0; i < m2.size; i++)
			alt[i] = other[i];
		alt_part = m2;
		alt_len = m2.size;
		*part = m1;
		return m1.size;
	}
	if (ok1) {
		*part = m1;
		return m1.size;
	}
	if (ok2) {
		for (unsigned int i = 0; i < m2.size; i++)
			blk[i] = other[i];
		*part = m2;
		return m2.size;
	}
	return 0;
}

static int env_store_buf(const struct mtd_part *m, const uint8_t *src)
{
	struct erase_info_user e;
	char path[16];
	int fd;
	unsigned int done = 0;

	mtd_path(m->index, path);
	fd = (int)sys_open(path, O_RDWR);
	if (fd < 0) {
		out_fmt("nv: cannot open %s for writing\n", path);
		return 0;
	}
	/* A whole number of erase blocks, or the erase is refused. */
	e.start = 0;
	e.length = m->size;
	if (sys_ioctl(fd, MEMERASE, &e) < 0) {
		out("nv: MEMERASE failed\n");
		sys_close(fd);
		return 0;
	}
	while (done < m->size) {
		long n = sys_write(fd, src + done, m->size - done);

		if (n <= 0) {
			out("nv: short write -- the environment is now ERASED\n");
			sys_close(fd);
			return 0;
		}
		done += (unsigned int)n;
	}
	sys_close(fd);
	return 1;
}

/* One writer at a time. Two nv processes (an ssh session and the web UI, say)
 * each read the block, erase the partition and write it back: unserialised,
 * one erases while the other writes and a copy is left half-written, or one
 * update is lost (a sw_commit the UI reported as committed). flock(2) on the
 * env partition's device node, held until this process exits, is the kernel's
 * own mechanism for that and needs no lock file; the node is the same inode
 * for every caller, and both copies are covered by locking the one named
 * "env". Readers do not take it: an erase in flight shows them an invalid
 * copy, and they already fall back to the other.
 *
 * Bounded: 10 s of 100 ms polls, then it gives up rather than wait for ever.
 * If flock itself fails for any reason other than "held" (a kernel without
 * it) the write goes ahead unlocked, as it always did, and says so.
 *
 * The syscall number and constants are in lock.h, held to a real kernel by
 * test/flock_probe.c under qemu-user; the call has not run on a stick yet. */
static int env_lock(void)
{
	struct mtd_part m;
	char path[16];
	long fd, rc;
	int i;

	if (!mtd_find("env", &m))
		return 1;
	mtd_path(m.index, path);
	fd = sys_open(path, O_RDONLY);
	if (fd < 0)
		return 1;
	for (i = 0; i < 100; i++) {
		rc = syscall3(NV_NR_FLOCK, fd, NV_LOCK_EX | NV_LOCK_NB, 0);
		if (rc == 0)
			return 1;                /* held until exit */
		if (rc != -NV_EAGAIN) {
			out("nv: flock is not available; writing without the lock\n");
			return 1;
		}
		sys_nanosleep(0, 100 * 1000 * 1000);
	}
	out("nv: another nv has held the environment for 10 s; not writing\n");
	return 0;
}

/* The partition I/O `nv commit` runs on (commit.h). Each call looks the
 * partition up by name again, so nothing cached can point a write at the
 * wrong one. */
static const char *copy_name(int copy)
{
	return copy == 2 ? "env2" : "env";
}

static uint32_t io_read(void *ctx, int copy, uint8_t *buf)
{
	struct mtd_part m;

	(void)ctx;
	return read_raw(copy_name(copy), &m, buf);
}

static int io_write(void *ctx, int copy, const uint8_t *buf, uint32_t len)
{
	struct mtd_part m;

	(void)ctx;
	if (!mtd_find(copy_name(copy), &m) || m.size != len)
		return 0;
	return env_store_buf(&m, buf);
}

/* The slot the kernel was booted from: U-Boot passes root=31:N, 31 being
 * mtdblock and N the index of the rootfs partition, and the partitions are
 * named r0 and r1 (the same reading image/fwu.sh makes). -1 when it cannot
 * tell -- no root=31:, or an index that is neither. Which root= counts is
 * cmdline_root_index()'s business (commit.h). */
static int running_slot(void)
{
	static char cmd[1024];
	struct mtd_part m;
	long n;
	int fd = (int)sys_open("/proc/cmdline", O_RDONLY);
	int idx;

	if (fd < 0)
		return -1;
	n = sys_read(fd, cmd, sizeof cmd - 1);
	sys_close(fd);
	if (n <= 0)
		return -1;
	cmd[n] = '\0';
	idx = cmdline_root_index(cmd);
	if (idx < 0)
		return -1;
	if (mtd_find("r0", &m) && m.index == idx)
		return 0;
	if (mtd_find("r1", &m) && m.index == idx)
		return 1;
	return -1;
}

static int file_exists(void *ctx, const char *path)
{
	int fd = (int)sys_open(path, O_RDONLY);

	(void)ctx;
	if (fd < 0)
		return 0;
	sys_close(fd);
	return 1;
}

static int commit(const char *arg, int force)
{
	struct env_io io = { 0, io_read, io_write };
	struct commit_aids aids = { 0, file_exists, NV_CONFIRM_ARP_PATH,
				    NV_BREADCRUMBS_PATH, 0, 0 };
	struct env_commit_report r;
	int slot = str_eq(arg, "0") ? 0 : str_eq(arg, "1") ? 1 : -1;
	int running = running_slot();
	int rc = env_commit_guarded(&io, &aids, force, slot, running, &r);

	out_fmt("nv: commit %s: %s\n", arg, env_commit_msg(rc));
	if (rc == NVC_CONFIRM_ARP) {
		out_fmt("nv: %s exists: each boot then waits for an ARP reply from the address in it,\n"
			"nv: and a committed slot has no trial to fall back to. If that host stops\n"
			"nv: answering, the stick resets every 120 s until the safe mode comes up.\n"
			"nv: Remove the file (rm %s), or commit anyway with: nv commit --force %s\n",
			aids.confirm_arp, aids.confirm_arp, arg);
		return 1;
	}
	if (rc == NVC_NOT_RUNNING) {
		if (running < 0)
			out("nv: running slot unknown (no root=31:N naming r0 or r1)\n");
		else
			out_fmt("nv: running slot is %d\n", (long)running);
	}
	if (r.repaired)
		out_fmt("nv: copy %d was not valid; it was written first, from the new environment, and verified\n",
			(long)r.repaired);
	if (r.primary)
		out_fmt("nv: primary copy %d %s, fallback copy %d %s\n",
			(long)r.primary, r.wrote_primary ? "written" : "unchanged",
			(long)r.fallback, r.wrote_fallback ? "written" : "unchanged");
	if (rc == NVC_DONE || rc == NVC_ALREADY) {
		if (aids.has_confirm_arp)
			out_fmt("nv: warning: committed with %s present (--force); each boot waits for an ARP reply from the address in it\n",
				aids.confirm_arp);
		if (aids.has_breadcrumbs)
			out_fmt("nv: note: %s exists (a development aid meant to be temporary)\n",
				aids.breadcrumbs);
	}
	return rc == NVC_DONE || rc == NVC_ALREADY ? 0 : 1;
}

static void usage(void)
{
	out("Usage: nv getenv [parameter_name]\n"
	    "       nv setenv [-c 1|2] parameter_name value\n"
	    "       nv fallback [parameter_name]\n"
	    "       nv commit [--force] slot\n"
	    "  commit refuses while /etc/config/confirm-arp exists (each boot would wait\n"
	    "  for an ARP reply, and a committed slot has no trial to fall back to);\n"
	    "  --force (or -f) commits anyway\n");
}

int main(int argc, char **argv)
{
	struct mtd_part part;
	unsigned int len;

	if (argc < 2) {
		usage();
		return 1;
	}
	/* Before env_load: commit reads both copies itself, through the same
	 * partition I/O it writes with, and has its own answer for a missing
	 * or invalid one. */
	if (str_eq(argv[1], "commit")) {
		int force = 0;

		if (argc == 4 && (str_eq(argv[2], "--force") || str_eq(argv[2], "-f")))
			force = 1;
		else if (argc != 3) {
			usage();
			return 1;
		}
		if (!env_lock())
			return 1;
		return commit(argv[argc - 1], force);
	}
	/* Before the read, not before the write: the block is read-modified-
	 * written, so the read has to be inside the lock too. */
	if (str_eq(argv[1], "setenv") && !env_lock())
		return 1;
	len = env_load(&part);
	if (!len) {
		out("nv: no valid environment in env or env2\n");
		return 1;
	}

	if (str_eq(argv[1], "getenv")) {
		if (argc < 3) {
			uint32_t pos = 0;
			const char *pair;

			out_fmt("Valid environment: %d\n", (long)part.copy);
			while ((pair = env_next(blk, len, &pos)) != 0)
				out_fmt("%s\n", pair);
			/* The stock nv ends with a blank line. Matching it
			 * costs one character and makes `cmp` a usable test
			 * against the vendor. */
			out_char('\n');
			return 0;
		}
		{
			char v[512];

			/* A key that is not there prints NOTHING and exits 0 --
			 * that is what the stock nv does, and boot scripts read
			 * it as `nv getenv k | awk -F= '{print $2}'` and test
			 * the result for emptiness. An error line here would
			 * still pass that test, but only by accident. */
			if (!env_get(blk, len, argv[2], v, sizeof v))
				return 0;
			out_fmt("%s=%s\n", argv[2], v);
			return 0;
		}
	}

	/* The copy that did NOT win. This is what U-Boot falls back to if the
	 * active copy is ever left invalid -- by an interrupted `setenv`, which
	 * is the realistic way it happens -- so it is the second answer any
	 * guard reading sw_commit needs. Prints NOTHING and exits 1 when there
	 * is no valid other copy, which is a different state from "the key is
	 * not set there" and has to stay distinguishable. */
	if (str_eq(argv[1], "fallback")) {
		if (!alt_len) {
			out("nv: no valid second copy of the environment\n");
			return 1;
		}
		if (argc < 3) {
			uint32_t pos = 0;
			const char *pair;

			out_fmt("Fallback environment: %d\n", (long)alt_part.copy);
			while ((pair = env_next(alt, alt_len, &pos)) != 0)
				out_fmt("%s\n", pair);
			out_char('\n');
			return 0;
		}
		{
			char v[512];

			if (!env_get(alt, alt_len, argv[2], v, sizeof v))
				return 0;
			out_fmt("%s=%s\n", argv[2], v);
			return 0;
		}
	}

	if (str_eq(argv[1], "setenv")) {
		struct env_io io = { 0, io_read, io_write };
		struct env_set_report sr;
		const char *value;
		int copy = 0;
		int a = 2;
		int rc;

		/* -c N writes a NAMED copy rather than the winning one. With
		 * both copies valid it is the only way to make a stale
		 * fallback agree: setenv without it writes back the copy it
		 * read, so the loser stays stale however many times it is
		 * run. Naming the invalid copy repairs just that one. */
		if (argc > 3 && str_eq(argv[2], "-c")) {
			if (str_eq(argv[3], "1"))
				copy = 1;
			else if (str_eq(argv[3], "2"))
				copy = 2;
			else {
				out("nv: -c takes 1 or 2\n");
				return 1;
			}
			a = 4;
		}
		if (argc < a + 1) {
			usage();
			return 1;
		}
		value = argc > a + 1 ? argv[a + 1] : 0;
		if (!alt_len) {
			/* The other copy is invalid: it is written first, from
			 * the new block, so the valid copy is never erased
			 * while it is the only one (commit.h, env_setenv). */
			if (copy == 3 - part.copy)
				out_fmt("nv: copy %d is not valid; only it is written and read back\n",
					(long)copy);
			else
				out_fmt("nv: copy %d is not valid; it is written first and read back,"
					" then copy %d\n", (long)(3 - part.copy), (long)part.copy);
		} else if (!copy) {
			/* What this erase costs if it is interrupted, said
			 * before it happens rather than discovered after. */
			char av[512];
			int had = env_get(alt, alt_len, argv[a], av, sizeof av);

			if (!had)
				out_fmt("nv: NOTE -- the fallback copy (%d) does"
					" not set %s\n", (long)alt_part.copy, argv[a]);
			else if (!value || !str_eq(av, value))
				out_fmt("nv: NOTE -- the fallback copy (%d) still has"
					" %s=%s\n", (long)alt_part.copy, argv[a], av);
			if (!had || !value || !str_eq(av, value)) {
				out("nv: if this write is interrupted, U-Boot uses"
				    " that value.\n");
				out_fmt("nv: to make them agree: nv setenv -c %d %s"
					" <value>\n", (long)alt_part.copy, argv[a]);
			}
		}
		rc = env_setenv(&io, copy, argv[a], value, &sr);
		if (rc != NVS_DONE) {
			if (rc == NVS_NO_ROOM || rc == NVS_NOT_SET)
				out_fmt("Error: cannot set %s\n", argv[a]);
			out_fmt("nv: setenv %s: %s\n", argv[a], env_setenv_msg(rc));
			return 1;
		}
		if (sr.repaired)
			out_fmt("nv: copy %d was not valid and is now written and verified\n",
				(long)sr.repaired);
		return 0;
	}

	usage();
	return 1;
}
