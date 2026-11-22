/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* A kernel for omcid to run against on the build host: the bodies of the
 * sys_*() functions sys_host.h declares.
 *
 * Nothing here reaches the real kernel. That is the point, and it is a rule
 * rather than a convenience: omcid, fed frames a fuzzer made up, opens files
 * under /var and /proc, removes them, sends signals (an accepted OLT Reboot
 * is SIGTERM to pid 1) and arms alarms. So this file makes no system call of
 * its own beyond what malloc() and an fwrite() to stderr do, and includes
 * none of the headers it would need to make one. What omcid sees instead:
 *
 *   files     a table of names and contents in memory. Every directory
 *             exists, a file exists once somebody made it (the harness with
 *             fk_file_put(), or omcid itself), and all of it is gone at the
 *             next fk_reset().
 *   queues    System V message queues by key and id. A message on one of
 *             omcid's own queues comes from the script; what omcid sends to
 *             a client queue is counted and dropped.
 *   netlink   the odi_omci transport (../nl.h): registration, OMCI frames
 *             both ways, and driver commands, each answered at once the way
 *             the stub of respond/drv.c answers them.
 *   clock     starts at a fixed time and moves only when omcid waits: 2 ms
 *             for a receive that times out, the length of a sleep, the
 *             seconds of a pause in the script.
 *   signals   sys_signal() stores the handler, sys_alarm() the deadline on
 *             the clock above, and sys_kill() only writes down what it was
 *             asked. A handler runs when the script or the alarm says so,
 *             called from the receive omcid was waiting in.
 *
 * A run ends when omcid returns from main() or calls sys_exit(); the latter
 * is a longjmp back into fk_run(), and so is the kill of a respawn in the
 * script, after which the files and queues are still there for the omcid
 * that init starts next (fk_respawn()). When the script is done the model lets a
 * few seconds pass, so that the rebuild after the last frame and the
 * once-a-second work run, and then delivers SIGTERM, which omcid handles as
 * it does on a stick.
 *
 * Return values are -errno with the MIPS numbers, as the real layer gives
 * them: omcid compares against those (../omci_msgq.h says why EIDRM is 36).
 *
 * The model also refuses a few things outright, with abort(), because each
 * is a defect in omcid that no sanitizer would report: a frame put on the
 * line that is not 48 bytes with a correct CRC, a signal sent to a process
 * group, and a receive that would block for good (AGENTS.md, every wait is
 * bounded).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "sys_host.h"
#include "fakeos.h"
#include "../nl.h"
#include "../omci_msgq.h"
#include "../omci_tmpfile.h"
#include "omci_caps.h"

#define E_NOENT   2
#define E_IO      5
#define E_BADF    9
#define E_ISDIR  21
#define E_MFILE  24
#define E_NOSPC  28
#define E_NOMSG  35            /* MIPS; 42 elsewhere */
#define E_PROTONOSUPPORT 120   /* MIPS; what qemu-user answers for NETLINK_ODI */
#define E_AFNOSUPPORT    124   /* MIPS */

#define FK_FILES      48
#define FK_PATH       128
#define FK_FILE_MAX   (256u * 1024u)   /* one file; more is -ENOSPC */
#define FK_FDS        32
#define FK_FD_BASE    3                /* 0..2 are the console */
#define FK_QUEUES     8
#define FK_MSGMAX     8192             /* msgmax of the target kernel */
#define FK_SIGS       32

#define FK_POLL_NS    2000000ull       /* a receive timeout, NL_POLL_US */
/* How many times omcid may ask for input without taking the next script
 * event before the model drops it: an event for a queue omcid does not
 * hold would otherwise stop the script for good. */
#define FK_STRIKES    16
/* Seconds that pass, one per receive, after the script: more than
 * REBUILD_MAX_S and PENDING_RETRY_S of omcid.h, so a pending rebuild and a
 * pending retry both run before the end. */
#define FK_SETTLE_S   8

#define AF_UNIX_      1

enum { FD_FREE, FD_FILE, FD_DIR, FD_NETLINK, FD_UNIX };

struct fk_file {
	char path[FK_PATH];
	uint8_t *data;
	size_t len, cap;
	int used;
	unsigned gen;                /* which file this slot holds now */
};

struct fk_fd {
	int kind;
	int file;                  /* FD_FILE: index into files */
	unsigned gen;              /* and the file it was when opened */
	int flags;
	size_t pos;                /* FD_FILE: offset; FD_DIR: next file index */
	char dir[FK_PATH];         /* FD_DIR */
	/* FD_NETLINK: the reply to the last driver command, until read. */
	uint8_t reply[NLMSG_HDR + NL_CMD_REPLY_HDR + NL_CMD_MAX_LEN];
	uint32_t reply_len;
};

struct fk_queue {
	int used;
	int client;                /* made by the harness, not by omcid */
	uint32_t key;
	long id;
};

static struct {
	struct fk_opts opts;
	struct fk_stats stats;
	struct fk_file files[FK_FILES];
	struct fk_fd fds[FK_FDS];
	struct fk_queue queues[FK_QUEUES];
	long next_qid;
	unsigned next_gen;
	int line_fd;               /* the socket registered for OMCI frames */
	uint64_t now_ns;           /* CLOCK_MONOTONIC */
	uint64_t alarm_ns;         /* 0: none pending */
	void (*handler[FK_SIGS])(int);
	const struct fk_event *ev;
	unsigned nev, pos, strikes, settle;
	int running;
	jmp_buf exit_jb;
} fk;

static void die(const char *what)
{
	fprintf(stderr, "fakeos: %s\n", what);
	abort();
}

uint32_t fk_crc32(const uint8_t *d, size_t n)
{
	uint32_t c = 0xffffffffu;

	while (n--) {
		c ^= (uint32_t)*d++ << 24;
		for (int k = 0; k < 8; k++)
			c = (c << 1) ^ ((c >> 31) ? 0x04c11db7u : 0);
	}
	return ~c;
}

/* ---------------------------------------------------------------- control */

/* What omcid printed, kept across the memset of fk_reset() so the buffer
 * is allocated once. */
static char *out_buf;
static size_t out_len, out_cap;

static void mix(const void *buf, size_t n)
{
	const uint8_t *b = buf;
	uint64_t h = fk.stats.digest;

	while (n--)
		h = (h ^ *b++) * 0x100000001b3ull;
	fk.stats.digest = h;
}

static void printed(const void *buf, size_t n)
{
	mix(buf, n);
	if (fk.opts.echo)
		fwrite(buf, 1, n, stderr);
	if (!fk.opts.capture)
		return;
	if (out_len + n > out_cap) {
		size_t cap = (out_len + n) * 2 + 4096;
		char *nb = realloc(out_buf, cap);

		if (!nb)
			die("no memory for the output of omcid");
		out_buf = nb;
		out_cap = cap;
	}
	memcpy(out_buf + out_len, buf, n);
	out_len += n;
}

const char *fk_output(size_t *len)
{
	*len = out_len;
	return out_buf ? out_buf : "";
}

uint64_t fk_digest(void)
{
	return fk.stats.digest;
}

void fk_reset(const struct fk_opts *opts)
{
	for (int i = 0; i < FK_FILES; i++)
		free(fk.files[i].data);
	memset(&fk, 0, sizeof fk);
	out_len = 0;
	fk.stats.digest = 0xcbf29ce484222325ull;
	if (opts)
		fk.opts = *opts;
	fk.line_fd = -1;
	/* Not zero: omcid subtracts timestamps from it, and a stick is never
	 * read in its first second either. */
	fk.now_ns = 1000ull * 1000000000ull;
}

static struct fk_file *file_find(const char *path)
{
	for (int i = 0; i < FK_FILES; i++)
		if (fk.files[i].used && strcmp(fk.files[i].path, path) == 0)
			return &fk.files[i];
	return 0;
}

static struct fk_file *file_make(const char *path)
{
	if (strlen(path) >= FK_PATH)
		return 0;
	for (int i = 0; i < FK_FILES; i++)
		if (!fk.files[i].used) {
			memset(&fk.files[i], 0, sizeof fk.files[i]);
			strcpy(fk.files[i].path, path);
			fk.files[i].used = 1;
			fk.files[i].gen = ++fk.next_gen;
			return &fk.files[i];
		}
	return 0;
}

static long file_append(struct fk_file *f, size_t at, const void *buf, size_t n)
{
	if (at > FK_FILE_MAX || n > FK_FILE_MAX - at)
		n = at < FK_FILE_MAX ? FK_FILE_MAX - at : 0;
	if (!n)
		return -E_NOSPC;
	if (at + n > f->cap) {
		size_t cap = f->cap ? f->cap : 256;
		uint8_t *p;

		while (cap < at + n)
			cap *= 2;
		p = realloc(f->data, cap);
		if (!p)
			return -E_NOSPC;
		f->data = p;
		f->cap = cap;
	}
	if (at > f->len)
		memset(f->data + f->len, 0, at - f->len);
	memcpy(f->data + at, buf, n);
	if (at + n > f->len)
		f->len = at + n;
	return (long)n;
}

void fk_file_put(const char *path, const void *data, size_t len)
{
	struct fk_file *f = file_find(path);

	if (!f)
		f = file_make(path);
	if (!f)
		die("fk_file_put: no room for the file");
	f->len = 0;
	if (len && file_append(f, 0, data, len) < 0)
		die("fk_file_put: file too large");
}

void fk_script(const struct fk_event *ev, unsigned n)
{
	fk.ev = ev;
	fk.nev = n;
	fk.pos = 0;
}

static void fk_exit(int status) __attribute__((noreturn));

static void fk_exit(int status)
{
	if (!fk.running)
		die("sys_exit() with no run in progress");
	longjmp(fk.exit_jb, (status & 0xff) + 1);
}

const struct fk_stats *fk_run(int (*entry)(int, char **), int argc, char **argv)
{
	int j;

	fk.running = 1;
	j = setjmp(fk.exit_jb);
	if (j == 0)
		fk.stats.exit_status = entry(argc, argv);
	else
		fk.stats.exit_status = j - 1;
	fk.running = 0;
	if (!fk.stats.respawn)
		fk.stats.skipped += fk.nev - fk.pos;
	fk.stats.files_open = 0;
	for (int i = 0; i < FK_FDS; i++)
		if (fk.fds[i].kind == FD_FILE || fk.fds[i].kind == FD_DIR)
			fk.stats.files_open++;
	return &fk.stats;
}

void fk_respawn(void)
{
	memset(fk.fds, 0, sizeof fk.fds);
	memset(fk.handler, 0, sizeof fk.handler);
	fk.line_fd = -1;
	fk.alarm_ns = 0;
	fk.strikes = 0;
	fk.settle = 0;
	fk.stats.respawn = 0;
}

void fk_file_move(const char *from, const char *to)
{
	sys_rename(from, to);
}

uint8_t *fk_file_data(const char *path, size_t *len)
{
	struct fk_file *f = file_find(path);

	if (!f || !f->len)
		return 0;
	*len = f->len;
	return f->data;
}

/* ------------------------------------------------------------------ files */

static struct fk_fd *fd_get(int fd)
{
	if (fd < FK_FD_BASE || fd >= FK_FD_BASE + FK_FDS ||
	    fk.fds[fd - FK_FD_BASE].kind == FD_FREE)
		return 0;
	return &fk.fds[fd - FK_FD_BASE];
}

static long fd_new(int kind)
{
	for (int i = 0; i < FK_FDS; i++)
		if (fk.fds[i].kind == FD_FREE) {
			memset(&fk.fds[i], 0, sizeof fk.fds[i]);
			fk.fds[i].kind = kind;
			return i + FK_FD_BASE;
		}
	return -E_MFILE;
}

/* Every directory exists, and a name is one unless it is a file: omcid only
 * ever lists /tmp, and creates files in directories the image always has. */
long sys_create(const char *path, int flags, int mode)
{
	struct fk_file *f = file_find(path);
	int acc = flags & 3;
	long fd;

	(void)mode;
	if (!f && !(flags & O_CREAT)) {
		size_t n = strlen(path);

		/* A directory, if anything is in it or it is one the image
		 * has; only for reading. */
		if (acc != O_RDONLY || n >= FK_PATH)
			return -E_NOENT;
		if (strcmp(path, OMCI_TMP_DIR) != 0) {
			int any = 0;

			for (int i = 0; i < FK_FILES && !any; i++)
				any = fk.files[i].used &&
				      strncmp(fk.files[i].path, path, n) == 0 &&
				      fk.files[i].path[n] == '/';
			if (!any)
				return -E_NOENT;
		}
		fd = fd_new(FD_DIR);
		if (fd >= 0)
			strcpy(fk.fds[fd - FK_FD_BASE].dir, path);
		return fd;
	}
	if (!f)
		f = file_make(path);
	if (!f)
		return -E_NOSPC;
	if ((flags & O_TRUNC) && acc != O_RDONLY)
		f->len = 0;
	fd = fd_new(FD_FILE);
	if (fd >= 0) {
		fk.fds[fd - FK_FD_BASE].file = (int)(f - fk.files);
		fk.fds[fd - FK_FD_BASE].gen = f->gen;
		fk.fds[fd - FK_FD_BASE].flags = flags;
	}
	return fd;
}

long sys_open(const char *path, int flags)
{
	return sys_create(path, flags, 0);
}

long sys_close(int fd)
{
	struct fk_fd *d = fd_get(fd);

	if (!d)
		return -E_BADF;
	if (fd == fk.line_fd)
		fk.line_fd = -1;
	d->kind = FD_FREE;
	return 0;
}

long sys_read(int fd, void *buf, unsigned long n)
{
	struct fk_fd *d = fd_get(fd);
	struct fk_file *f;

	if (fd == STDIN_FILENO)
		return 0;
	if (!d || d->kind != FD_FILE || (d->flags & 3) == O_WRONLY)
		return d && d->kind == FD_DIR ? -E_ISDIR : -E_BADF;
	f = &fk.files[d->file];
	/* The name may be gone (unlink, rename over it) with the descriptor
	 * still open; the model keeps no contents for it then. */
	if (!f->used || f->gen != d->gen || d->pos >= f->len)
		return 0;
	if (n > f->len - d->pos)
		n = f->len - d->pos;
	memcpy(buf, f->data + d->pos, n);
	d->pos += n;
	return (long)n;
}

long sys_write(int fd, const void *buf, unsigned long n)
{
	struct fk_fd *d = fd_get(fd);
	struct fk_file *f;
	long rc;

	if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
		printed(buf, n);
		return (long)n;
	}
	if (!d || d->kind != FD_FILE || (d->flags & 3) == O_RDONLY)
		return -E_BADF;
	f = &fk.files[d->file];
	if (!f->used || f->gen != d->gen)
		return (long)n;
	if (!n)
		return 0;
	if (d->flags & O_APPEND)
		d->pos = f->len;
	rc = file_append(f, d->pos, buf, n);
	if (rc > 0)
		d->pos += (size_t)rc;
	return rc;
}

long sys_unlink(const char *path)
{
	struct fk_file *f = file_find(path);

	if (!f)
		return -E_NOENT;
	free(f->data);
	memset(f, 0, sizeof *f);
	return 0;
}

long sys_rename(const char *from, const char *to)
{
	struct fk_file *f = file_find(from), *t = file_find(to);

	if (!f || strlen(to) >= FK_PATH)
		return -E_NOENT;
	if (f == t)
		return 0;
	if (t) {
		free(t->data);
		memset(t, 0, sizeof *t);
	}
	strcpy(f->path, to);
	return 0;
}

long sys_getdents64(int fd, void *buf, unsigned n)
{
	struct fk_fd *d = fd_get(fd);
	size_t dl, used = 0;

	if (!d || d->kind != FD_DIR)
		return -E_BADF;
	dl = strlen(d->dir);
	for (; d->pos < FK_FILES; d->pos++) {
		const struct fk_file *f = &fk.files[d->pos];
		const char *name = f->path + dl + 1;
		struct dirent64 *e;
		size_t rec;

		if (!f->used || strncmp(f->path, d->dir, dl) != 0 ||
		    f->path[dl] != '/' || strchr(name, '/'))
			continue;
		rec = (offsetof(struct dirent64, d_name) + strlen(name) + 1 + 7)
		      & ~(size_t)7;
		if (used + rec > n)
			break;
		e = (struct dirent64 *)((char *)buf + used);
		memset(e, 0, rec);
		e->d_ino = d->pos + 1;
		e->d_off = (int64_t)(d->pos + 1);
		e->d_reclen = (unsigned short)rec;
		e->d_type = 8;                   /* DT_REG */
		strcpy(e->d_name, name);
		used += rec;
	}
	return (long)used;
}

/* ------------------------------------------------- clock, signals, script */

long sys_clock_gettime(int clk, long *sec, long *nsec)
{
	uint64_t t = fk.now_ns;

	/* The wall clock is the monotonic one plus a fixed date; nothing in
	 * omcid sets it. */
	if (clk == CLOCK_REALTIME)
		t += 1700000000ull * 1000000000ull;
	else if (clk != CLOCK_MONOTONIC)
		return -MQ_EINVAL;
	*sec = (long)(t / 1000000000ull);
	*nsec = (long)(t % 1000000000ull);
	return 0;
}

long sys_signal(int sig, void (*handler)(int))
{
	if (sig < 1 || sig >= FK_SIGS)
		return -MQ_EINVAL;
	fk.handler[sig] = handler;
	return 0;
}

long sys_alarm(unsigned sec)
{
	uint64_t was = fk.alarm_ns;

	fk.alarm_ns = sec ? fk.now_ns + (uint64_t)sec * 1000000000ull : 0;
	return was > fk.now_ns ? (long)((was - fk.now_ns + 999999999ull)
					/ 1000000000ull) : 0;
}

/* Written down, never done. */
long sys_kill(long pid, int sig)
{
	if (pid <= 0)
		die("sys_kill() of a process group or of every process");
	fk.stats.kills++;
	fk.stats.kill_pid = pid;
	fk.stats.kill_sig = sig;
	if (fk.opts.echo)
		fprintf(stderr, "fakeos: kill(%ld, %d) recorded, not sent\n",
			pid, sig);
	return 0;
}

long sys_getpid(void)
{
	return 4242;
}

long sys_gettid(void)
{
	return 4242;
}

void sys_exit(int status)
{
	fk_exit(status);
}

static void raise_sig(int sig)
{
	if (sig >= 1 && sig < FK_SIGS && fk.handler[sig])
		fk.handler[sig](sig);
}

/* One step of the script, taken when omcid waits for input and none of the
 * kind it asked for is next. `line` is the socket omcid is receiving OMCI
 * frames on, when that is where it waits. Returns the length of a frame put
 * in msg, or -EAGAIN. */
static long fk_step(struct fk_fd *line, struct msghdr *msg)
{
	const struct fk_event *e;

	if (fk.alarm_ns && fk.now_ns >= fk.alarm_ns) {
		fk.alarm_ns = 0;
		raise_sig(14);
	}
	if (fk.pos >= fk.nev) {
		/* The script is done: let the daemon settle, then stop it as
		 * init would. A handler that returns, or none, ends the run
		 * here. */
		fk.now_ns += 1000000000ull;
		if (++fk.settle > FK_SETTLE_S) {
			raise_sig(15);
			fk_exit(0);
		}
		return -MQ_EAGAIN;
	}
	e = &fk.ev[fk.pos];
	if (e->kind == FK_PAUSE) {
		fk.pos++;
		fk.strikes = 0;
		fk.now_ns += (uint64_t)e->arg * 1000000000ull;
		return -MQ_EAGAIN;
	}
	if (e->kind == FK_RESPAWN) {
		/* Killed where it waits: no handler runs, nothing is flushed. */
		fk.pos++;
		fk.strikes = 0;
		fk.stats.respawn = 1;
		fk.stats.respawn_data = e->data;
		fk.stats.respawn_len = e->len;
		fk_exit(0);
	}
	if (e->kind == FK_SIGNAL) {
		fk.pos++;
		fk.strikes = 0;
		raise_sig((int)e->arg);
		return -MQ_EAGAIN;
	}
	if (e->kind == FK_LINE_FRAME && line && msg && msg->iovlen >= 1) {
		/* The datagram of ../nl.h: a netlink header whose first word
		 * is the length, four bytes of redirect port, the frame. */
		uint8_t dg[NLMSG_HDR + NL_FRAME_OFF + REDIRECT_MTU];
		uint32_t n = e->len > REDIRECT_MTU ? REDIRECT_MTU : e->len;
		uint32_t total = NLMSG_HDR + NL_FRAME_OFF + n;

		memset(dg, 0, NLMSG_HDR + NL_FRAME_OFF);
		nl_put32(dg, total);
		if (n)
			memcpy(dg + NLMSG_HDR + NL_FRAME_OFF, e->data, n);
		if (total > msg->iov[0].len)
			total = msg->iov[0].len;
		memcpy(msg->iov[0].base, dg, total);
		fk.pos++;
		fk.strikes = 0;
		return (long)total;
	}
	if (++fk.strikes > FK_STRIKES) {
		fk.pos++;
		fk.strikes = 0;
		fk.stats.skipped++;
	}
	fk.now_ns += FK_POLL_NS;
	return -MQ_EAGAIN;
}

long sys_nanosleep(long sec, long nsec)
{
	if (sec < 0 || nsec < 0 || nsec > 999999999)
		return -MQ_EINVAL;
	fk.now_ns += (uint64_t)sec * 1000000000ull + (uint64_t)nsec;
	/* With no line side the main loop paces itself with this sleep, so
	 * this is where the script moves on. */
	if (fk.opts.no_netlink)
		fk_step(0, 0);
	return 0;
}

/* ---------------------------------------------------------------- queues */

static struct fk_queue *queue_by_id(long id)
{
	for (int i = 0; i < FK_QUEUES; i++)
		if (fk.queues[i].used && fk.queues[i].id == id)
			return &fk.queues[i];
	return 0;
}

static struct fk_queue *queue_by_key(uint32_t key)
{
	for (int i = 0; key && i < FK_QUEUES; i++)
		if (fk.queues[i].used && fk.queues[i].key == key)
			return &fk.queues[i];
	return 0;
}

static struct fk_queue *queue_make(uint32_t key, int client)
{
	for (int i = 0; i < FK_QUEUES; i++)
		if (!fk.queues[i].used) {
			fk.queues[i].used = 1;
			fk.queues[i].client = client;
			fk.queues[i].key = key;
			/* Never reused within a run, so an id omcid kept from a
			 * removed queue is invalid, as on the target. */
			fk.queues[i].id = fk.next_qid++;
			return &fk.queues[i];
		}
	return 0;
}

long fk_client_queue(uint32_t key)
{
	struct fk_queue *q = queue_make(key, 1);

	if (!q)
		die("fk_client_queue: no room");
	return q->id;
}

static long mq_get(uint32_t key, int flags)
{
	struct fk_queue *q = queue_by_key(key);

	if (q)
		return (flags & IPC_CREAT) && (flags & IPC_EXCL) ? -MQ_EEXIST : q->id;
	if (!(flags & IPC_CREAT))
		return -E_NOENT;
	q = queue_make(key, 0);
	return q ? q->id : -E_NOSPC;
}

static long mq_snd(long id, unsigned long n, int flags, const void *msg)
{
	struct fk_queue *q = queue_by_id(id);

	(void)msg;
	if (!q || n > FK_MSGMAX)
		return -MQ_EINVAL;
	if (q->client && fk.opts.reply_full) {
		if (!(flags & IPC_NOWAIT))
			die("msgsnd would block for good: the queue is full "
			    "and IPC_NOWAIT is not set");
		return -MQ_EAGAIN;
	}
	if (q->client)
		fk.stats.replies++;
	return 0;
}

static long mq_rcv(long id, unsigned long n, int flags, struct ipc_kludge *k)
{
	struct fk_queue *q = queue_by_id(id);
	const struct fk_event *e = fk.pos < fk.nev ? &fk.ev[fk.pos] : 0;
	uint32_t mtype;
	unsigned long len;

	if (!q)
		return -MQ_EINVAL;
	if (e && !q->client &&
	    ((e->kind == FK_VQ_MSG && q->key == OMCI_MQ_KEY) ||
	     (e->kind == FK_CLI_MSG && q->key != OMCI_MQ_KEY)) && e->len >= 4) {
		memcpy(&mtype, e->data, 4);
		len = e->len - 4;
		fk.pos++;
		fk.strikes = 0;
		/* A type the receiver did not ask for stays queued on the
		 * target, behind nothing; the script has no place to keep it,
		 * so it is dropped, as is a message too long for a receiver
		 * that did not say MSG_NOERROR. */
		if ((k->msgtyp > 0 && (long)mtype != k->msgtyp) ||
		    (k->msgtyp < 0 && (long)mtype > -k->msgtyp))
			return -E_NOMSG;
		if (len > n) {
			if (!(flags & MSG_NOERROR))
				return -MQ_E2BIG;
			len = n;
		}
		memcpy(k->msgp, e->data, 4 + len);
		return (long)len;
	}
	if (!(flags & IPC_NOWAIT))
		die("msgrcv would block for good: the queue is empty and "
		    "IPC_NOWAIT is not set");
	return -E_NOMSG;
}

long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	(void)f;
	if (n != __NR_ipc)
		die("__syscall6() with a number the model does not have");
	switch (a) {
	case IPC_MSGGET:
		return mq_get((uint32_t)b, (int)c);
	case IPC_MSGCTL: {
		struct fk_queue *q = queue_by_id(b);

		if (c != IPC_RMID || !q)
			return -MQ_EINVAL;
		q->used = 0;
		return 0;
	}
	case IPC_MSGSND:
		return mq_snd(b, (unsigned long)c, (int)d, (const void *)e);
	case IPC_MSGRCV:
		return mq_rcv(b, (unsigned long)c, (int)d, (struct ipc_kludge *)e);
	}
	die("ipc() call the model does not have");
	return 0;
}

/* --------------------------------------------------------------- netlink */

/* The capability block of a stick (driver command 3), the one the suites of
 * src/omci pass with -c: one Ethernet UNI on switch port 0, 64 GEM flows. */
static const uint8_t fk_caps[OMCI_CAPS_LEN] = {
	0x02, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
	0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
	0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
	0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
	0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
	0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x40,
	0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x04, 0x40,
};

/* The driver, as the trace build of respond/drv.c answers for it: every
 * command succeeds and hands its argument back, and the few omcid reads
 * something from get a fixed answer. Made-up values throughout; the serial
 * is the one the test suites use. */
static int drv_model(uint32_t cmd, uint8_t *b, uint32_t len)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'T', 0, 0, 0, 1 };
	static const uint8_t devid[8] = { 'R', 'T', 'L', '9', '6', '0', '2', 'C' };
	static const uint16_t ddm[7] = { 0, 0, 0x1900, 33000, 5000, 5000, 1000 };

	switch (cmd) {
	case 3:
		if (len == sizeof fk_caps)
			memcpy(b, fk_caps, len);
		return 0;
	case 4:
	case 15:
		for (uint32_t i = 0; i < len && i < 8; i++)
			b[i] = cmd == 15 ? sn[i] : devid[i];
		return 0;
	case 13:
		/* The ONU state, a word in host order: O5. */
		if (len == 4) {
			uint32_t st = 5;

			memcpy(b, &st, 4);
		}
		return 0;
	case 10:
		if (len >= 12 && b[3] == 7) {
			memset(b + 4, 0, 8);         /* no alarm flag raised */
			return 0;
		}
		if (len < 6 || b[3] < 2 || b[3] > 6)
			return -1;
		b[4] = (uint8_t)(ddm[b[3]] >> 8);
		b[5] = (uint8_t)ddm[b[3]];
		return 0;
	case 0x101:
		/* The bridge table: nothing learned. The words are in host
		 * order (struct omci_bridge_tbl); port and first stay as sent. */
		if (len >= 16)
			memset(b + 8, 0, 8);
		return 0;
	}
	return 0;
}

static void line_frame_check(const uint8_t *f, uint32_t len)
{
	if (fk.opts.echo) {
		fprintf(stderr, "fakeos: line <- ");
		for (uint32_t i = 0; i < len; i++)
			fprintf(stderr, "%02x", f[i]);
		fputc('\n', stderr);
	}
	if (len != 48)
		die("omcid put a frame on the line that is not 48 bytes");
	if (nl_get32(f + 44) != fk_crc32(f, 44))
		die("omcid put a frame on the line with a wrong CRC");
	if (f[3] != 0x0a || nl_get32(f + 40) != 0x28)
		die("omcid put a frame on the line with a wrong device id or trailer");
	mix(f, 48);
	fk.stats.frames_sent++;
}

long sys_socket(int domain, int type, int protocol)
{
	(void)type;
	if (domain == AF_UNIX_)
		return fd_new(FD_UNIX);
	if (domain != AF_NETLINK)
		return -E_AFNOSUPPORT;
	if (protocol != NETLINK_ODI || fk.opts.no_netlink)
		return -E_PROTONOSUPPORT;
	return fd_new(FD_NETLINK);
}

long sys_bind(int fd, const void *addr, unsigned len)
{
	(void)addr;
	(void)len;
	return fd_get(fd) ? 0 : -E_BADF;
}

long sys_setsockopt(int fd, int level, int opt, const void *val, unsigned len)
{
	(void)level;
	(void)opt;
	(void)val;
	(void)len;
	return fd_get(fd) ? 0 : -E_BADF;
}

long sys_sendmsg(int fd, const void *vmsg, int flags)
{
	const struct msghdr *msg = vmsg;
	struct fk_fd *d = fd_get(fd);
	const uint8_t *buf, *b;
	uint32_t len, op;

	(void)flags;
	if (!d || (d->kind != FD_NETLINK && d->kind != FD_UNIX))
		return -E_BADF;
	if (msg->iovlen < 1)
		return 0;
	buf = msg->iov[0].base;
	len = msg->iov[0].len;
	/* A syslog datagram: nobody is listening. */
	if (d->kind == FD_UNIX)
		return (long)len;
	if (len < NLMSG_HDR + 4)
		return (long)len;
	b = buf + NLMSG_HDR;
	op = nl_get16(b);
	if (op == NL_OP_REG && len >= NLMSG_HDR + NL_REG_BODY) {
		if (nl_get16(b + 2) == REDIRECT_TYPE)
			fk.line_fd = b[8] == NL_ACT_REG ? fd : -1;
	} else if (op == NL_OP_SEND && len >= NLMSG_HDR + NL_BODY) {
		uint32_t n = nl_get32(b + 12);

		if (n > len - NLMSG_HDR - NL_BODY)
			die("omcid sent a frame longer than its datagram");
		if (nl_get16(b + 2) == REDIRECT_TYPE)
			line_frame_check(b + NL_BODY, n);
	} else if (op == NL_OP_CMD && len >= NLMSG_HDR + NL_CMD_REQ_HDR) {
		uint32_t cmd = nl_get32(b + 4), n = nl_get32(b + 8);
		uint8_t *r = d->reply + NLMSG_HDR;
		int status;

		if (n > NL_CMD_MAX_LEN || n > len - NLMSG_HDR - NL_CMD_REQ_HDR)
			die("omcid sent a driver command longer than its datagram");
		/* The number and the length, not the bytes: for a command that
		 * reads, they are whatever was in the buffer omcid passes for
		 * the answer, which is not always written first. */
		mix(b + 4, 8);
		fk.stats.drv_calls++;
		if (fk.opts.drv_dead)
			return (long)len;
		memset(d->reply, 0, NLMSG_HDR + NL_CMD_REPLY_HDR);
		memcpy(r + NL_CMD_REPLY_HDR, b + NL_CMD_REQ_HDR, n);
		status = drv_model(cmd, r + NL_CMD_REPLY_HDR, n);
		if (fk.stats.drv_calls == fk.opts.drv_fail_at)
			status = -E_IO;
		d->reply_len = NLMSG_HDR + NL_CMD_REPLY_HDR + n;
		nl_put32(d->reply, d->reply_len);
		nl_put16(r, NL_OP_CMD);
		nl_put32(r + 4, cmd);
		nl_put32(r + 8, (uint32_t)status);
		nl_put32(r + 12, n);
	}
	return (long)len;
}

long sys_recvmsg(int fd, void *vmsg, int flags)
{
	struct msghdr *msg = vmsg;
	struct fk_fd *d = fd_get(fd);

	(void)flags;
	if (!d || d->kind != FD_NETLINK)
		return -E_BADF;
	if (d->reply_len && msg->iovlen >= 1) {
		uint32_t n = d->reply_len;

		if (n > msg->iov[0].len)
			n = msg->iov[0].len;
		memcpy(msg->iov[0].base, d->reply, n);
		d->reply_len = 0;
		return (long)n;
	}
	/* Frames come to the socket that registered for them, and that
	 * receive is where the main loop waits. Any other socket only times
	 * out. */
	if (fd == fk.line_fd)
		return fk_step(d, msg);
	fk.now_ns += FK_POLL_NS;
	return -MQ_EAGAIN;
}
