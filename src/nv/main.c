/* nv — read and write the U-Boot environment, without libmib.
 *
 * The stock /bin/nv is a 15 KB CLI over the environment accessors in
 * libmib.so, which is proprietary. Everything it needs is in /proc/mtd and a
 * crc32, so this replaces it with no vendor library:
 *
 *     nv getenv [parameter_name]
 *     nv setenv [-c 1|2] parameter_name value
 *     nv fallback [parameter_name]
 *     nv commit slot
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
 * which is what the redundancy is designed for. Settle the rule before
 * relying on this to arm a trial boot.
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

/* Load one NAMED copy, valid or not, for `setenv -c N`. The flags byte is
 * preserved by env_set, so writing the losing copy leaves it losing -- this
 * cannot change which copy U-Boot picks, only what the loser says. */
static unsigned int env_load_copy(int copy, struct mtd_part *part)
{
	struct mtd_part m;
	const char *name = copy == 2 ? "env2" : "env";

	if (!read_part(name, &m, blk))
		return 0;
	m.copy = copy;
	*part = m;
	return m.size;
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

static int env_store(const struct mtd_part *m)
{
	return env_store_buf(m, blk);
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
 * tell -- no root=31:, or an index that is neither. */
static int running_slot(void)
{
	static char cmd[1024];
	struct mtd_part m;
	const char *p;
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
	for (p = cmd; *p; p++) {
		if ((p == cmd || p[-1] == ' ') && str_has_prefix(p, "root=31:"))
			break;
	}
	if (!*p)
		return -1;
	p += 8;
	if (!digit(*p))
		return -1;
	idx = (int)hex_or_dec(&p, 0);
	if (*p && *p != ' ' && *p != '\n')
		return -1;
	if (mtd_find("r0", &m) && m.index == idx)
		return 0;
	if (mtd_find("r1", &m) && m.index == idx)
		return 1;
	return -1;
}

static int commit(const char *arg)
{
	struct env_io io = { 0, io_read, io_write };
	struct env_commit_report r;
	int slot = str_eq(arg, "0") ? 0 : str_eq(arg, "1") ? 1 : -1;
	int running = running_slot();
	int rc = env_commit(&io, slot, running, &r);

	out_fmt("nv: commit %s: %s\n", arg, env_commit_msg(rc));
	if (rc == NVC_NOT_RUNNING) {
		if (running < 0)
			out("nv: running slot unknown (no root=31:N naming r0 or r1)\n");
		else
			out_fmt("nv: running slot is %d\n", (long)running);
	}
	if (r.primary)
		out_fmt("nv: primary copy %d %s, fallback copy %d %s\n",
			(long)r.primary, r.wrote_primary ? "written" : "unchanged",
			(long)r.fallback, r.wrote_fallback ? "written" : "unchanged");
	return rc == NVC_DONE || rc == NVC_ALREADY ? 0 : 1;
}

static void usage(void)
{
	out("Usage: nv getenv [parameter_name]\n"
	    "       nv setenv [-c 1|2] parameter_name value\n"
	    "       nv fallback [parameter_name]\n"
	    "       nv commit slot\n");
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
		if (argc != 3) {
			usage();
			return 1;
		}
		return commit(argv[2]);
	}
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
		int copy = 0;
		int a = 2;

		/* -c N writes a NAMED copy rather than the winning one. It is
		 * the only way to make a stale fallback agree: setenv without
		 * it writes back the copy it read, so the loser stays stale
		 * however many times it is run. */
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
		if (copy) {
			len = env_load_copy(copy, &part);
			if (!len) {
				out_fmt("nv: copy %d is not valid -- refusing to"
					" write it\n", (long)copy);
				return 1;
			}
		} else {
			/* What this erase costs if it is interrupted, said
			 * before it happens rather than discovered after. */
			if (!alt_len) {
				out("nv: WARNING -- there is no valid second copy.\n");
				out("nv: this erase leaves the board with NO good\n");
				out("nv: environment until the write completes.\n");
			} else {
				char av[512];
				int had = env_get(alt, alt_len, argv[a], av, sizeof av);
				const char *want = argc > a + 1 ? argv[a + 1] : 0;

				if (!had)
					out_fmt("nv: NOTE -- the fallback copy (%d) does"
						" not set %s\n", (long)alt_part.copy, argv[a]);
				else if (!want || !str_eq(av, want))
					out_fmt("nv: NOTE -- the fallback copy (%d) still has"
						" %s=%s\n", (long)alt_part.copy, argv[a], av);
				if (!had || !want || !str_eq(av, want)) {
					out("nv: if this write is interrupted, U-Boot uses"
					    " that value.\n");
					out_fmt("nv: to make them agree: nv setenv -c %d %s"
						" <value>\n", (long)alt_part.copy, argv[a]);
				}
			}
		}
		if (!env_set(blk, len, argv[a], argc > a + 1 ? argv[a + 1] : 0)) {
			out_fmt("Error: cannot set %s\n", argv[a]);
			return 1;
		}
		if (!env_store(&part))
			return 1;
		return 0;
	}

	usage();
	return 1;
}
