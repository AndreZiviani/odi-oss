/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_mock.h -- register access primitives for odi_switch, either
 * real MMIO (__KERNEL__) or a host register model plus write log (host
 * tests). This is the one file odi_switch_tbl.c and the command table call
 * into for every register touch, so the host build and the target build
 * share every line of logic above this point.
 *
 * Host model: a flat array covers the 0x000000-0x1FFFFF switch-core span
 * in 4-byte units (0x80000 words), the 0x700000-0x70FFFF GPON block, and
 * the 0xF00000-0xF0FFFF counter/PONQ_COUNT_MASK block (added for
 * odi_switch_dal.c), each remapped into the
 * same array at a fixed offset -- see odi_mock_slot() below. That is
 * sparse enough for every address ever traced against these drivers
 * (the highest switch-core offset seen is under 0x040000; the GPON block
 * tops out under 0x710000; PONQ_COUNT_MASK tops out under 0xF00F00) without a
 * hash table. An address outside every window aborts the test binary
 * loudly rather than silently corrupting an unrelated slot.
 *
 * Under __KERNEL__ the same two functions become __raw_readl/__raw_writel
 * against odi_switch_base, an ioremap-ed pointer odi_switch.c sets up once
 * at ODI_SWITCH_MMIO_BASE (odi_switch_hw.h). odi_reg_read/odi_reg_write
 * never appear directly in odi_switch_tbl.c or the command table -- always
 * through these two, so ODI_SWITCH_HOST is the only thing that changes
 * between the two builds.
 */
#ifndef ODI_SWITCH_MOCK_H
#define ODI_SWITCH_MOCK_H

#ifdef __KERNEL__

#include <linux/io.h>
#include <linux/types.h>

extern void __iomem *odi_switch_base;

static inline uint32_t odi_reg_read(uint32_t off)
{
	return __raw_readl(odi_switch_base + off);
}

static inline void odi_reg_write(uint32_t off, uint32_t val)
{
	__raw_writel(val, odi_switch_base + off);
}

/* No target-side table tracer exists yet (odi_switch.ko does not go
 * through the tracer hooks of the regtrace capture kernel -- it is raw
 * MMIO). ODI_SW_TABLE_TRACE is a host-only verification aid; a no-op
 * under __KERNEL__.
 */
#define ODI_SW_TABLE_TRACE(kind, table, index, words, n) do { } while (0)

#else /* host build */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* odi_switch_mmio_offset_in_bounds()/ODI_SWITCH_MMIO_SIZE -- the same
 * bound odi_switch.c's real ioremap enforces on target. Included here,
 * not just by the test .c files
 * that already pull it in after this header, so odi_mock_slot() below
 * can check every offset against it directly: the mock's own three
 * sub-windows model where an offset physically lands, but say nothing
 * about whether that offset is inside the single contiguous region the
 * real ioremap actually maps -- a leaf could address a sub-window this
 * mock happily serves that a narrower real ioremap size would leave
 * unmapped. This is the host-side half of that guard; odi_switch.c's
 * odi_reg_read()/odi_reg_write() are the target-side half.
 */
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"

/* Switch-core window: 0x000000-0x1FFFFF, 4-byte units. */
#define ODI_MOCK_SW_BASE	0x000000U
#define ODI_MOCK_SW_SIZE	0x200000U
/* GPON block: 0x700000-0x70FFFF, remapped after the switch-core window. */
#define ODI_MOCK_GPON_BASE	0x700000U
#define ODI_MOCK_GPON_SIZE	0x010000U
/* Counter/PONQ_COUNT_MASK block: 0xF00000-0xF0FFFF, remapped after the GPON
 * window.
 */
#define ODI_MOCK_CNT_BASE	0xf00000U
#define ODI_MOCK_CNT_SIZE	0x010000U

#define ODI_MOCK_WORDS	((ODI_MOCK_SW_SIZE + ODI_MOCK_GPON_SIZE + ODI_MOCK_CNT_SIZE) / 4U)

struct odi_mock_write {
	uint64_t ns;
	char kind;		/* 'W' for a register write, 'M' for a mark */
	uint32_t addr;		/* register offset, or the mark tag for 'M' */
	uint32_t val;		/* written value, or 0 for 'M' */
};

/* odi_switch_init_platform()'s item 6 CF-table sweep alone logs ~2800
 * entries (256 rows x 3 tables x ~4 entries/row) before the first
 * command mark even opens, on top of whatever the 91-bracket replay
 * itself needs -- 4096 silently truncated both, corrupting brackets
 * that happened to fall after the cutoff. Bumped generously rather than
 * trimmed to the exact current total, so the next addition to either
 * side does not silently reintroduce the same truncation.
 */
#define ODI_MOCK_LOG_MAX	32768

struct odi_mock_state {
	uint32_t regs[ODI_MOCK_WORDS];
	struct odi_mock_write log[ODI_MOCK_LOG_MAX];
	unsigned int log_n;
	uint64_t ns;		/* fake timestamp, incremented per entry */
};

static struct odi_mock_state odi_mock;

static inline unsigned int odi_mock_slot(uint32_t off)
{
	/* Real ioremap bound first ("switch base corrected" item 2): a leaf
	 * addressing past ODI_SWITCH_MMIO_SIZE would silently corrupt
	 * whatever memory follows the real ioremap on target -- the same
	 * class of mistake that put the s5 CLASSIFY_SETUP write 9 MB past the
	 * SoC-window's actual end. Checked before the sub-window logic below
	 * so a bug in that logic's own bounds (all three sub-windows happen
	 * to sit inside ODI_SWITCH_MMIO_SIZE today, but nothing enforced
	 * that) cannot mask an offset the real target would refuse.
	 */
	if (!odi_switch_mmio_offset_in_bounds(off)) {
		fprintf(stderr,
			"odi_switch_mock: address 0x%08x is outside the mapped ioremap window "
			"(ODI_SWITCH_MMIO_SIZE 0x%08lx) -- this would corrupt memory on target\n",
			off, (unsigned long)ODI_SWITCH_MMIO_SIZE);
		abort();
	}
	if (off < ODI_MOCK_SW_SIZE)
		return off / 4U;
	if (off >= ODI_MOCK_GPON_BASE && off < ODI_MOCK_GPON_BASE + ODI_MOCK_GPON_SIZE)
		return (ODI_MOCK_SW_SIZE + (off - ODI_MOCK_GPON_BASE)) / 4U;
	if (off >= ODI_MOCK_CNT_BASE && off < ODI_MOCK_CNT_BASE + ODI_MOCK_CNT_SIZE)
		return (ODI_MOCK_SW_SIZE + ODI_MOCK_GPON_SIZE + (off - ODI_MOCK_CNT_BASE)) / 4U;
	fprintf(stderr, "odi_switch_mock: address 0x%08x outside the modeled window\n", off);
	abort();
}

static inline void odi_mock_table_pending_reset(void); /* forward decl, defined below */

static inline void odi_mock_reset(void)
{
	memset(&odi_mock, 0, sizeof(odi_mock));
	odi_mock_table_pending_reset();
}

static inline uint32_t odi_reg_read(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

/* odi_switch_mmio_ensure() host stub -- the mock own register array (regs[])
 * always exists, no ioremap concept to lazily set up, so this is just
 * "always ready" -- odi_board_init() (CONFIG_ODI_BOARD, odi_board.c) calls
 * the real one before its first odi_reg_write(), and this is what host
 * tests link against instead (odi_switch.c itself, where the real one
 * lives, is never compiled into a host unity build).
 */
static inline int odi_switch_mmio_ensure(void)
{
	return 0;
}

/* Host lock model: the kernel spinlock and mutex calls the driver files
 * make, and the three lock objects they name (odi_switch.c has the lock
 * order and what each one protects; the real definitions are __KERNEL__
 * only, like odi_reg_read()/odi_reg_write()). A host test is single
 * threaded, so each lock is a depth counter, and the one thing it can
 * catch is what lockdep would (lockdep is not in our kernel config): a
 * second acquire of a lock already held, which deadlocks on target, and
 * a release of a lock that is not held. Either aborts the test.
 *
 * lockdep_assert_held() is enforced for the spinlock only. The mutex is
 * taken by whole entry points (netlink, ioctl, /proc writes) that no host
 * test drives; the tests call the primitives underneath directly, so an
 * enforced mutex assert would only test the tests.
 */
struct odi_mock_lock {
	int depth;
	int assert_enforced;
	const char *name;
};

typedef struct {
	struct odi_mock_lock m;
} spinlock_t;

struct mutex {
	struct odi_mock_lock m;
};

#define DEFINE_SPINLOCK(x)	spinlock_t x = { { 0, 1, #x } }
#define DEFINE_MUTEX(x)		struct mutex x = { { 0, 0, #x } }

static inline void odi_mock_lock_acquire(struct odi_mock_lock *l)
{
	if (l->depth) {
		fprintf(stderr, "odi_switch_mock: %s acquired while already held (self-deadlock on target)\n",
			l->name);
		abort();
	}
	l->depth = 1;
}

static inline void odi_mock_lock_release(struct odi_mock_lock *l)
{
	if (!l->depth) {
		fprintf(stderr, "odi_switch_mock: %s released while not held\n", l->name);
		abort();
	}
	l->depth = 0;
}

static inline void odi_mock_lock_assert_held(const struct odi_mock_lock *l)
{
	if (l->assert_enforced && !l->depth) {
		fprintf(stderr, "odi_switch_mock: %s not held where the caller must hold it\n",
			l->name);
		abort();
	}
}

#define spin_lock_irqsave(l, flags) \
	do { (flags) = 0; odi_mock_lock_acquire(&(l)->m); } while (0)
#define spin_unlock_irqrestore(l, flags) \
	do { (void)(flags); odi_mock_lock_release(&(l)->m); } while (0)
#define mutex_lock(l)		odi_mock_lock_acquire(&(l)->m)
#define mutex_unlock(l)		odi_mock_lock_release(&(l)->m)
#define lockdep_assert_held(l)	odi_mock_lock_assert_held(&(l)->m)

/* The lock objects: static, one copy per unity-build test, and marked
 * unused because a test that never reaches a given lock would otherwise
 * fail -Werror.
 */
static __attribute__((unused)) DEFINE_MUTEX(odi_switch_lock);
static __attribute__((unused)) DEFINE_SPINLOCK(odi_switch_dsf_lock);
static __attribute__((unused)) DEFINE_MUTEX(odi_i2c_lock);

/* True when none of the three is held: what a test checks after a call
 * returns, error paths included.
 */
static inline int odi_mock_locks_idle(void)
{
	return !odi_switch_lock.m.depth && !odi_switch_dsf_lock.m.depth && !odi_i2c_lock.m.depth;
}

/* TABLE_CMD/STS/WR_DATA/RD_DATA, 0x012000-0x01202c -- the real
 * regtrace skip list drops every write in this range, which is exactly why
 * boot5's captured brackets carry no W entries for the table-access
 * handshake odi_switch_table_write() performs. The mock reproduces that
 * silently, the same way the real skip list does, so odi_switch_table_write()
 * can issue the real handshake (portable to __KERNEL__) without polluting
 * the host write log with entries no boot5 fixture will ever show.
 */
#define ODI_MOCK_TBL_ACCESS_LO	0x012000U
#define ODI_MOCK_TBL_ACCESS_HI	0x01202cU

static inline void odi_reg_write(uint32_t off, uint32_t val)
{
	odi_mock.regs[odi_mock_slot(off)] = val;
	if (off >= ODI_MOCK_TBL_ACCESS_LO && off <= ODI_MOCK_TBL_ACCESS_HI)
		return;
	/* Deliberately does NOT touch the table run-tracking state
	 * (odi_mock_table()'s pending struct, below): boot5 shows a run's
	 * R close can sit well after a burst of unrelated plain-register
	 * writes (the VLAN full-table sweep's R comes after the
	 * VLAN_INGRESS_CHECK/EGRESS_TAG group's nine writes, boot5 lines
	 * 256-265) -- T/D print immediately when a run starts, so nothing
	 * here needs to force an early close.
	 */
	if (odi_mock.log_n < ODI_MOCK_LOG_MAX) {
		struct odi_mock_write *e = &odi_mock.log[odi_mock.log_n++];

		e->ns = odi_mock.ns;
		e->kind = 'W';
		e->addr = off;
		e->val = val;
	}
	odi_mock.ns += 10000; /* arbitrary but monotonic, like the real ring uptime_ns field */
}

/* A poll register that always reads back "done" -- every bounded spin in a
 * table primitive sees completion on the first read, so host tests never
 * block. odi_reg_read() itself only returns whatever was last written
 * (regs[] starts zeroed, i.e. not-done); primitives that poll a completion
 * bit should call this after issuing the request instead of odi_reg_read(),
 * or the hardware bit in a real target build never gets a chance to be checked.
 */
static inline uint32_t odi_mock_poll_done(uint32_t off, uint32_t done_mask)
{
	(void)off;
	return done_mask;
}

/* Command-bracket marks, same shape as the kernel regtrace ring: tag bit 31
 * is the phase (0 before, 1 after), bits 0..30 a tag the caller chooses.
 * Used by odi_switch_test.c to bracket the writes of a primitive the same
 * way apply.c brackets an OMCI command, so compare.py can be pointed at a
 * dump from the mock directly.
 */
/* Table-op run tracking, matching boot5's own T/D/R shape: T+D print
 * IMMEDIATELY the moment a row is written (never deferred -- boot5's
 * VLAN sweep shows its T+D ahead of a nine-write burst of unrelated
 * plain registers, boot5 lines 254-265, so waiting to see whether the
 * run continues before printing anything would put those writes in the
 * wrong place). Only the closing R -- "this run turned out to be N rows
 * long" -- is deferred, and only until the run actually breaks (a
 * table_write() call to a different table/index/value arrives, a
 * command-bracket mark closes, or the log is dumped): whatever plain
 * register writes happen in between do not affect it, matching the
 * sweep's own gap. Kept as trace-only state, disjoint from
 * odi_mock.regs[]/log[] indices: only odi_mock_table() touches it.
 */
#define ODI_MOCK_TBL_MAX_WORDS	8U

struct odi_mock_tbl_pending {
	int active;		/* an R close may still be owed for this run */
	char kind;		/* 'T' (write) or 't' (read) */
	uint32_t table;
	uint32_t index;		/* run's first index -- what R's addr repeats */
	uint32_t words[ODI_MOCK_TBL_MAX_WORDS];
	uint32_t n_words;
	uint32_t run_len;
};

static struct odi_mock_tbl_pending odi_mock_tbl_pending;

static inline void odi_mock_table_pending_reset(void)
{
	memset(&odi_mock_tbl_pending, 0, sizeof(odi_mock_tbl_pending));
}

/* Emits the deferred R for the currently-open run, if it ended up longer
 * than one row (a length-1 "run" never gets an R, matching boot5). Safe
 * to call with nothing pending.
 */
static inline void odi_mock_table_flush(void)
{
	struct odi_mock_tbl_pending *p = &odi_mock_tbl_pending;

	if (!p->active)
		return;

	if (p->run_len > 1) {
		if (odi_mock.log_n < ODI_MOCK_LOG_MAX) {
			struct odi_mock_write *e = &odi_mock.log[odi_mock.log_n++];

			e->ns = odi_mock.ns;
			e->kind = 'R';
			e->addr = (p->table << 16) | p->index;
			e->val = p->run_len;
		}
		odi_mock.ns += 10000;
	}

	p->active = 0;
}

/* Records one table_write()/table_read() call (kind 'T'/'t', table id,
 * row index, row words). A call whose table/words/kind match the open
 * run and whose index continues it (index == pending index + pending run
 * length) extends the run silently (no new T+D -- boot5 never repeats
 * them for a run's later rows). Any other call closes the previous run
 * (emitting its R if it ended up longer than one row) and starts a new
 * one, printing this call's own T+D immediately.
 */
static inline void odi_mock_table(char kind, uint32_t table, uint32_t index,
				   const uint32_t *words, uint32_t n_words)
{
	struct odi_mock_tbl_pending *p = &odi_mock_tbl_pending;
	unsigned int i;

	if (n_words > ODI_MOCK_TBL_MAX_WORDS) {
		fprintf(stderr, "odi_switch_mock: table %u row has %u words, max %u\n",
			table, n_words, ODI_MOCK_TBL_MAX_WORDS);
		abort();
	}

	if (p->active && p->kind == kind && p->table == table && p->n_words == n_words &&
	    index == p->index + p->run_len &&
	    memcmp(words, p->words, n_words * sizeof(words[0])) == 0) {
		p->run_len++;
		return;
	}

	odi_mock_table_flush();

	if (odi_mock.log_n < ODI_MOCK_LOG_MAX) {
		struct odi_mock_write *e = &odi_mock.log[odi_mock.log_n++];

		e->ns = odi_mock.ns;
		e->kind = kind;
		e->addr = (table << 16) | index;
		e->val = 0;
	}
	odi_mock.ns += 10000;

	for (i = 0; i < n_words; i++) {
		if (odi_mock.log_n < ODI_MOCK_LOG_MAX) {
			struct odi_mock_write *e = &odi_mock.log[odi_mock.log_n++];

			e->ns = odi_mock.ns;
			e->kind = 'D';
			e->addr = (i << 24) | table;
			e->val = words[i];
		}
		odi_mock.ns += 10000;
	}

	p->active = 1;
	p->kind = kind;
	p->table = table;
	p->index = index;
	p->n_words = n_words;
	p->run_len = 1;
	memcpy(p->words, words, n_words * sizeof(words[0]));
}

#define ODI_SW_TABLE_TRACE(kind, table, index, words, n) \
	odi_mock_table((kind), (table), (index), (words), (n))

static inline void odi_mock_mark(uint32_t tag)
{
	odi_mock_table_flush();
	if (odi_mock.log_n < ODI_MOCK_LOG_MAX) {
		struct odi_mock_write *e = &odi_mock.log[odi_mock.log_n++];

		e->ns = odi_mock.ns;
		e->kind = 'M';
		e->addr = tag;
		e->val = 0;
	}
	odi_mock.ns += 10000;
}

/* Ring-dump format: "<uptime_ns> W <addr> <value>" / "<uptime_ns> M <tag> 0",
 * one line per log entry, exactly what compare.py and decode.py parse.
 */
static inline void odi_mock_dump(FILE *f)
{
	unsigned int i;

	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++) {
		struct odi_mock_write *e = &odi_mock.log[i];

		fprintf(f, "%llu %c 0x%08x 0x%08x\n",
			(unsigned long long)e->ns, e->kind, e->addr, e->val);
	}
}

#endif /* __KERNEL__ */

#endif /* ODI_SWITCH_MOCK_H */
