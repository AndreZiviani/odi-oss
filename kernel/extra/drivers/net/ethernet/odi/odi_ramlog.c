// SPDX-License-Identifier: GPL-2.0
/*
 * odi_ramlog.c -- odi_ramlog.h's own implementation. Two halves, the same
 * shape every other odi_* driver in this directory uses:
 *
 *  - Plain C, no kernel dependency, compiled on both builds: the on-
 *    memory format primitives (odi_ramlog_rd32()/wr32(), odi_ramlog_page_
 *    reset(), odi_ramlog_boot_stamp(), odi_ramlog_write() and its two
 *    static helpers), and the previous-boot half (odi_ramlog_parse_slot(),
 *    odi_ramlog_meta_stamp(), odi_ramlog_save_prev(), odi_ramlog_render()).
 *    test/odi_ramlog_test.c exercises this half
 *    directly, unity-build style, against a pair of on-stack byte arrays
 *    standing in for the two DRAM pages.
 *
 *  - __KERNEL__ only: the struct console glue -- odi_ramlog_console_
 *    write() (the .write callback) and odi_ramlog_console_init(), a
 *    console_initcall() (the earliest initcall level printk defines,
 *    run from start_kernel()'s own console_init(), well before any
 *    device_initcall()/module_init() driver probe -- the closest this
 *    stable API gets to "early console" without the earlycon/devicetree
 *    machinery this non-DT board has no other user of). CON_PRINTBUFFER
 *    tells register_console() to replay the log_buf lines already
 *    printed before this call runs, so nothing between the first printk
 *    and this initcall is lost; CON_ENABLED keeps it active regardless of
 *    what the "console=" cmdline names (this driver has no tty of its
 *    own for anything to select).
 */
#include "odi_ramlog.h"

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <generated/utsrelease.h>
#include <asm/bootinfo.h>
#else
#include <stdio.h>
#include <string.h>
#endif

#define DRV_NAME "odi_ramlog"

#ifdef __KERNEL__
#define odi_ramlog_rb(page, off)	__raw_readb((page) + (off))
#define odi_ramlog_wb(page, off, v)	__raw_writeb((v), (page) + (off))
#else
#define odi_ramlog_rb(page, off)	((page)[off])
#define odi_ramlog_wb(page, off, v)	((page)[off] = (v))
#endif

/* ---- On-memory format primitives ---------------------------------------- */

uint32_t odi_ramlog_rd32(const unsigned char ODI_RAMLOG_MEM *page, unsigned int off)
{
	return ((uint32_t)odi_ramlog_rb(page, off) << 24) |
	       ((uint32_t)odi_ramlog_rb(page, off + 1) << 16) |
	       ((uint32_t)odi_ramlog_rb(page, off + 2) << 8) |
	       (uint32_t)odi_ramlog_rb(page, off + 3);
}

void odi_ramlog_wr32(unsigned char ODI_RAMLOG_MEM *page, unsigned int off, uint32_t val)
{
	odi_ramlog_wb(page, off, (unsigned char)(val >> 24));
	odi_ramlog_wb(page, off + 1, (unsigned char)(val >> 16));
	odi_ramlog_wb(page, off + 2, (unsigned char)(val >> 8));
	odi_ramlog_wb(page, off + 3, (unsigned char)val);
}

void odi_ramlog_page_reset(unsigned char ODI_RAMLOG_MEM *page, uint32_t magic,
			    uint32_t tag1, uint32_t tag2)
{
	odi_ramlog_wr32(page, 0, magic);
	odi_ramlog_wr32(page, 4, 0);
	odi_ramlog_wr32(page, 8, tag1);
	odi_ramlog_wr32(page, 12, tag2);
}

void odi_ramlog_boot_stamp(unsigned char ODI_RAMLOG_MEM *page_a, unsigned char ODI_RAMLOG_MEM *page_b)
{
	odi_ramlog_page_reset(page_a, ODI_RAMLOG_MAGIC_A, ODI_RAMLOG_TAG_HEAD, ODI_RAMLOG_TAG_JSK);
	odi_ramlog_page_reset(page_b, ODI_RAMLOG_MAGIC_B, 0, 0);
}

/* odi_ramlog_ensure() -- lazy re-stamp if this page's own magic does not
 * already match: a cold power cycle wipes DRAM (a watchdog reset does
 * not), or odi_ramlog_boot_
 * stamp() has genuinely never run yet. In the normal path boot_stamp()
 * already set a valid magic, so this is a no-op read on every call after
 * the first -- called once per odi_ramlog_write() rather than once per
 * byte (the earlier port's ramlog_putc() checked per byte; no observable difference,
 * since within one write() call nothing can wipe DRAM out from under it).
 */
static void odi_ramlog_ensure(unsigned char ODI_RAMLOG_MEM *page, uint32_t magic)
{
	if (odi_ramlog_rd32(page, 0) != magic)
		odi_ramlog_page_reset(page, magic, 0, 0);
}

/* odi_ramlog_a_append() -- page A: the first ODI_RAMLOG_A_DATA bytes ever
 * written, then frozen: once count reaches ODI_RAMLOG_A_DATA, further
 * bytes are silently dropped and count stops advancing. The cap stops
 * short of the metadata block at the end of the page.
 */
static void odi_ramlog_a_append(unsigned char ODI_RAMLOG_MEM *page, const char *s, unsigned int len)
{
	uint32_t n = odi_ramlog_rd32(page, 4);
	unsigned int i;

	for (i = 0; i < len && n < ODI_RAMLOG_A_DATA; i++, n++)
		odi_ramlog_wb(page, ODI_RAMLOG_HDR + n, (unsigned char)s[i]);
	odi_ramlog_wr32(page, 4, n);
}

/* odi_ramlog_b_append() -- page B: a ring of the last ODI_RAMLOG_DATA
 * bytes, count advancing without limit. Matches the earlier port's
 * `bb[16 + (n % RAMLOG_DATA)] = c; n++` exactly -- tools/memprobe/
 * ramlog-read.sh reconstructs chronological order from count % 4080.
 */
static void odi_ramlog_b_append(unsigned char ODI_RAMLOG_MEM *page, const char *s, unsigned int len)
{
	uint32_t n = odi_ramlog_rd32(page, 4);
	unsigned int i;

	for (i = 0; i < len; i++, n++)
		odi_ramlog_wb(page, ODI_RAMLOG_HDR + (n % ODI_RAMLOG_DATA), (unsigned char)s[i]);
	odi_ramlog_wr32(page, 4, n);
}

void odi_ramlog_write(unsigned char ODI_RAMLOG_MEM *page_a, unsigned char ODI_RAMLOG_MEM *page_b,
		       const char *s, unsigned int len)
{
	odi_ramlog_ensure(page_a, ODI_RAMLOG_MAGIC_A);
	odi_ramlog_ensure(page_b, ODI_RAMLOG_MAGIC_B);
	odi_ramlog_a_append(page_a, s, len);
	odi_ramlog_b_append(page_b, s, len);
}

/* ---- Boot metadata and the previous-boot copy --------------------------- */

static int odi_ramlog_slot_of(const char *v, size_t n)
{
	if ((n == 4 && !strncmp(v, "31:5", 4)) || (n == 14 && !strncmp(v, "/dev/mtdblock5", 14)))
		return 0;
	if ((n == 4 && !strncmp(v, "31:7", 4)) || (n == 14 && !strncmp(v, "/dev/mtdblock7", 14)))
		return 1;
	return -1;
}

uint32_t odi_ramlog_parse_slot(const char *cmdline)
{
	uint32_t slot = ODI_RAMLOG_SLOT_UNKNOWN;
	const char *p;

	if (!cmdline)
		return slot;
	for (p = cmdline; *p; p++) {
		const char *v;
		int s;

		if ((p != cmdline && p[-1] != ' ') || strncmp(p, "root=", 5))
			continue;
		v = p + 5;
		s = odi_ramlog_slot_of(v, strcspn(v, " "));
		slot = s < 0 ? ODI_RAMLOG_SLOT_UNKNOWN : (uint32_t)s;
	}
	return slot;
}

uint32_t odi_ramlog_meta_stamp(unsigned char ODI_RAMLOG_MEM *page_a, uint32_t slot,
			       const char *build_id)
{
	unsigned char ODI_RAMLOG_MEM *m = page_a + ODI_RAMLOG_META_OFF;
	uint32_t boot = 1;
	unsigned int i;

	if (odi_ramlog_rd32(m, ODI_RAMLOG_M_MAGIC) == ODI_RAMLOG_MAGIC_M)
		boot = odi_ramlog_rd32(m, ODI_RAMLOG_M_BOOT) + 1;
	odi_ramlog_wr32(m, ODI_RAMLOG_M_BOOT, boot);
	odi_ramlog_wr32(m, ODI_RAMLOG_M_SLOT, slot);
	odi_ramlog_wr32(m, ODI_RAMLOG_M_FMT, ODI_RAMLOG_META_FMT);
	for (i = 0; i < ODI_RAMLOG_BUILD_LEN; i++) {
		unsigned char c = (build_id && i < ODI_RAMLOG_BUILD_LEN - 1) ? (unsigned char)*build_id : 0;

		odi_ramlog_wb(m, ODI_RAMLOG_M_BUILD + i, c);
		if (c)
			build_id++;
	}
	/* The magic last: a reset in the middle leaves no half-written block
	 * that passes for a valid one.
	 */
	odi_ramlog_wr32(m, ODI_RAMLOG_M_MAGIC, ODI_RAMLOG_MAGIC_M);
	return boot;
}

void odi_ramlog_save_prev(unsigned char *prev, const unsigned char ODI_RAMLOG_MEM *page_a,
			  const unsigned char ODI_RAMLOG_MEM *page_b, int crumbs_stashed)
{
	unsigned char *b = prev + ODI_RAMLOG_PAGE_SIZE;
	unsigned int i;

	for (i = 0; i < ODI_RAMLOG_PAGE_SIZE; i++) {
		prev[i] = odi_ramlog_rb(page_a, i);
		b[i] = odi_ramlog_rb(page_b, i);
	}
	if (crumbs_stashed)
		for (i = 0; i < 8; i++)
			b[8 + i] = prev[ODI_RAMLOG_META_OFF + ODI_RAMLOG_M_CRUMB_TAG + i];
}

static void odi_ramlog_emit_str(odi_ramlog_emit_fn emit, void *ctx, const char *s)
{
	emit(ctx, s, (unsigned int)strlen(s));
}

/* A crumb tag as text: the four ASCII bytes when printable, else hex. */
static void odi_ramlog_tag_str(const unsigned char *t, char *out, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		if (t[i] < 32 || t[i] > 126)
			break;
	if (i == 4)
		snprintf(out, len, "%c%c%c%c", t[0], t[1], t[2], t[3]);
	else
		snprintf(out, len, "0x%08x", (unsigned int)odi_ramlog_rd32(t, 0));
}

static void odi_ramlog_slot_str(uint32_t slot, char *out, unsigned int len)
{
	if (slot == ODI_RAMLOG_SLOT_UNKNOWN)
		snprintf(out, len, "?");
	else
		snprintf(out, len, "%u", (unsigned int)slot);
}

static void odi_ramlog_emit_text(odi_ramlog_emit_fn emit, void *ctx,
				 const unsigned char *t, unsigned int len)
{
	if (len)
		emit(ctx, (const char *)t, len);
}

void odi_ramlog_render(const unsigned char *prev, uint32_t now_boot, uint32_t now_slot,
		       odi_ramlog_emit_fn emit, void *ctx)
{
	const unsigned char *a = prev, *b = prev + ODI_RAMLOG_PAGE_SIZE;
	const unsigned char *m = a + ODI_RAMLOG_META_OFF;
	uint32_t na = odi_ramlog_rd32(a, 4), nb = odi_ramlog_rd32(b, 4);
	int a_ok = odi_ramlog_rd32(a, 0) == ODI_RAMLOG_MAGIC_A;
	int b_ok = odi_ramlog_rd32(b, 0) == ODI_RAMLOG_MAGIC_B;
	int m_ok = a_ok && odi_ramlog_rd32(m, ODI_RAMLOG_M_MAGIC) == ODI_RAMLOG_MAGIC_M;
	char line[160], tag[12], slot[12], build[ODI_RAMLOG_BUILD_LEN];
	unsigned int i;

	odi_ramlog_slot_str(now_slot, slot, sizeof(slot));
	snprintf(line, sizeof(line), "this boot: boot=%u slot=%s\n", (unsigned int)now_boot, slot);
	if (!a_ok && !b_ok) {
		snprintf(line + strlen(line), sizeof(line) - strlen(line),
			 "previous boot: none (page A magic 0x%08x, page B magic 0x%08x)\n",
			 (unsigned int)odi_ramlog_rd32(a, 0), (unsigned int)odi_ramlog_rd32(b, 0));
		odi_ramlog_emit_str(emit, ctx, line);
		return;
	}
	odi_ramlog_emit_str(emit, ctx, line);

	odi_ramlog_tag_str(b + 8, tag, sizeof(tag));
	if (m_ok) {
		for (i = 0; i < ODI_RAMLOG_BUILD_LEN - 1; i++) {
			unsigned char c = m[ODI_RAMLOG_M_BUILD + i];

			if (!c)
				break;
			build[i] = (c < 32 || c > 126) ? '?' : (char)c;
		}
		build[i] = 0;
		odi_ramlog_slot_str(odi_ramlog_rd32(m, ODI_RAMLOG_M_SLOT), slot, sizeof(slot));
		snprintf(line, sizeof(line),
			 "previous boot: boot=%u slot=%s build=%s crumb=%s/%u\n",
			 (unsigned int)odi_ramlog_rd32(m, ODI_RAMLOG_M_BOOT), slot,
			 build[0] ? build : "?", tag, (unsigned int)odi_ramlog_rd32(b, 12));
	} else {
		snprintf(line, sizeof(line),
			 "previous boot: no metadata block (an older image) crumb=%s/%u\n",
			 tag, (unsigned int)odi_ramlog_rd32(b, 12));
	}
	odi_ramlog_emit_str(emit, ctx, line);

	if (a_ok) {
		unsigned int cap = m_ok ? ODI_RAMLOG_A_DATA : ODI_RAMLOG_DATA;
		unsigned int len = na < cap ? na : cap;

		snprintf(line, sizeof(line), "---- page A: first %u bytes ----\n", len);
		odi_ramlog_emit_str(emit, ctx, line);
		odi_ramlog_emit_text(emit, ctx, a + ODI_RAMLOG_HDR, len);
		if (len && a[ODI_RAMLOG_HDR + len - 1] != '\n')
			emit(ctx, "\n", 1);
	}
	if (b_ok) {
		const unsigned char *ring = b + ODI_RAMLOG_HDR;
		unsigned int len = nb > ODI_RAMLOG_DATA ? ODI_RAMLOG_DATA : nb;
		unsigned int p = nb > ODI_RAMLOG_DATA ? nb % ODI_RAMLOG_DATA : 0;

		snprintf(line, sizeof(line), "---- page B: last %u of %u bytes ----\n",
			 len, (unsigned int)nb);
		odi_ramlog_emit_str(emit, ctx, line);
		odi_ramlog_emit_text(emit, ctx, ring + p, len - p);
		odi_ramlog_emit_text(emit, ctx, ring, p);
		if (len && ring[(p ? p : len) - 1] != '\n')
			emit(ctx, "\n", 1);
	}
}

/* ---- struct console glue ------------------------------------------------- */

#ifdef __KERNEL__

#define ODI_RAMLOG_PAGE_A ((unsigned char __iomem *)ODI_RAMLOG_PAGE_A_KSEG1)
#define ODI_RAMLOG_PAGE_B ((unsigned char __iomem *)ODI_RAMLOG_PAGE_B_KSEG1)

/* Classic console .write callback: printk core hands us exactly the bytes
 * of one or more already-formatted log lines, each already ending in a
 * trailing newline -- no reformatting expected or done here.
 * odi_ramlog_write() mirrors the buffer verbatim, unlike the earlier port's
 * ramlog_write() which added one newline itself per call (that call was
 * per log_store() message with the newline stripped; this callback's own
 * buffer already carries it).
 */
static void odi_ramlog_console_write(struct console *co, const char *s, unsigned int count)
{
	odi_ramlog_write(ODI_RAMLOG_PAGE_A, ODI_RAMLOG_PAGE_B, s, count);
}

static struct console odi_ramlog_console = {
	.name  = "ramlog",
	.write = odi_ramlog_console_write,
	/* CON_CONSDEV is what actually triggers the boot-console handover
	 * described below on odi_ramlog_console_early: register_console()
	 * (kernel/printk/printk.c) only auto-unregisters CON_BOOT consoles
	 * when the newly registered one has CON_CONSDEV set and CON_BOOT
	 * clear. Without it here, the check silently never matched and
	 * odi_ramlog_console_early stayed registered forever alongside this
	 * one -- both consoles writing every line, a duplicated boot log.
	 */
	.flags = CON_PRINTBUFFER | CON_ENABLED | CON_CONSDEV,
	.index = -1,
};

/* odi_ramlog_early_active -- set once odi_ramlog_early_console_init() has
 * run. __initdata is safe: nothing reads it past console_initcall(), which
 * itself only runs during boot, before init/main.c frees the .init
 * sections. Plain bool/int (no locking): both writes and the one read
 * happen on the boot CPU, strictly ordered by program order (prom_init()
 * always runs before start_kernel() reaches console_init()), the same
 * single-threaded-boot assumption odi_wdt.c's own init-time globals make.
 */
static bool odi_ramlog_early_active __initdata;

/* odi_ramlog_console_early -- CON_BOOT: printk's own register_console()
 * (kernel/printk/printk.c) unregisters every CON_BOOT console once a
 * non-boot console with CON_CONSDEV set registers -- see the flags comment
 * on odi_ramlog_console below, which is what actually arms that handover.
 * Exactly one of the two is meant to be the active writer at a time, no
 * line mirrored twice. Same .write callback, same DRAM pages: the two
 * struct console instances exist only so both can be independently
 * registered/unregistered by that handover.
 */
static struct console odi_ramlog_console_early = {
	.name  = "ramlog",
	.write = odi_ramlog_console_write,
	.flags = CON_BOOT | CON_PRINTBUFFER | CON_ENABLED,
	.index = -1,
};

/* The build id stamped into the metadata block: ODI_RAMLOG_BUILD_ID from
 * the Makefile (kernel/build.sh ODI_BUILD_ID), else the kernel release.
 */
#ifndef ODI_RAMLOG_BUILD_ID
#define ODI_RAMLOG_BUILD_ID UTS_RELEASE
#endif

/* The previous boot, as it left both pages: bss, so it is zeroed before
 * prom_init() runs and stays for /proc to read.
 */
static unsigned char odi_ramlog_prev[ODI_RAMLOG_PREV_SIZE];
static uint32_t odi_ramlog_boot, odi_ramlog_slot = ODI_RAMLOG_SLOT_UNKNOWN;

/* odi_ramlog_start() -- save the previous boot, then stamp this one. The
 * first thing either registration path does, before any write reaches
 * the pages. `cmdline` is searched for root= first, `fallback` only when
 * it names no slot: at prom_init() arcs_cmdline holds just the bootloader
 * arguments, and BUILTIN_EXTEND puts the built-in line in front of them,
 * so a root= there is the last one and wins.
 */
static void __init odi_ramlog_start(const char *cmdline, const char *fallback)
{
	odi_ramlog_slot = odi_ramlog_parse_slot(cmdline);
	if (odi_ramlog_slot == ODI_RAMLOG_SLOT_UNKNOWN && fallback)
		odi_ramlog_slot = odi_ramlog_parse_slot(fallback);
	odi_ramlog_save_prev(odi_ramlog_prev, ODI_RAMLOG_PAGE_A, ODI_RAMLOG_PAGE_B,
			     IS_ENABLED(CONFIG_ODI_EARLY_CRUMBS));
	odi_ramlog_boot_stamp(ODI_RAMLOG_PAGE_A, ODI_RAMLOG_PAGE_B);
	odi_ramlog_boot = odi_ramlog_meta_stamp(ODI_RAMLOG_PAGE_A, odi_ramlog_slot,
						ODI_RAMLOG_BUILD_ID);
}

#ifdef CONFIG_CMDLINE_BOOL
#define ODI_RAMLOG_CMDLINE_BUILTIN CONFIG_CMDLINE
#else
#define ODI_RAMLOG_CMDLINE_BUILTIN NULL
#endif

/* odi_ramlog_early_console_init() -- see odi_ramlog.h for the full design.
 * Called from arch/mips/rtl8686/board.c's prom_init() (kernel/618).
 */
void __init odi_ramlog_early_console_init(void)
{
	odi_ramlog_start(arcs_cmdline, ODI_RAMLOG_CMDLINE_BUILTIN);
	register_console(&odi_ramlog_console_early);
	odi_ramlog_early_active = true;
	pr_info(DRV_NAME ": early console registered, mirroring into DRAM pages 0x%08lx/0x%08lx\n",
		ODI_RAMLOG_PAGE_A_PHYS, ODI_RAMLOG_PAGE_B_PHYS);
}

static int __init odi_ramlog_console_init(void)
{
	/* Do NOT re-run odi_ramlog_boot_stamp() when the early console
	 * already did: it unconditionally zeroes both pages' count (by
	 * design -- a fresh boot must not append onto a watchdog-surviving
	 * previous trial's bytes, see test_boot_stamp_starts_fresh_even_
	 * with_valid_prior_content in test/odi_ramlog_test.c), which would
	 * erase everything the early console captured between prom_init()
	 * and here.
	 */
	if (!odi_ramlog_early_active) {
		odi_ramlog_start(boot_command_line, NULL);
	} else {
		/* The early console already mirrored (and, itself being
		 * CON_PRINTBUFFER, replayed) every line printk has logged so
		 * far. Registering this console with CON_PRINTBUFFER too
		 * would replay all of it a second time -- a boot log showed
		 * exactly that, every pre-handover line printed twice. Drop it
		 * here; new lines keep mirroring normally via
		 * .write(). A build with no early console at all
		 * (odi_ramlog_early_active always false there) keeps
		 * CON_PRINTBUFFER as before.
		 */
		odi_ramlog_console.flags &= ~CON_PRINTBUFFER;
	}
	register_console(&odi_ramlog_console);
	pr_info(DRV_NAME ": console registered, mirroring into DRAM pages 0x%08lx/0x%08lx\n",
		ODI_RAMLOG_PAGE_A_PHYS, ODI_RAMLOG_PAGE_B_PHYS);
	return 0;
}
console_initcall(odi_ramlog_console_init);

/* ---- /proc/odi_ramlog_prev, /proc/odi_ramlog_prev_raw ------------------- */

static void odi_ramlog_seq_emit(void *ctx, const char *s, unsigned int len)
{
	seq_write(ctx, s, len);
}

static int odi_ramlog_prev_show(struct seq_file *m, void *v)
{
	odi_ramlog_render(odi_ramlog_prev, odi_ramlog_boot, odi_ramlog_slot,
			  odi_ramlog_seq_emit, m);
	return 0;
}

static ssize_t odi_ramlog_prev_raw_read(struct file *f, char __user *buf,
					size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(buf, count, ppos, odi_ramlog_prev,
				       sizeof(odi_ramlog_prev));
}

static const struct proc_ops odi_ramlog_prev_raw_ops = {
	.proc_read   = odi_ramlog_prev_raw_read,
	.proc_lseek  = default_llseek,
};

static int __init odi_ramlog_proc_init(void)
{
	struct proc_dir_entry *raw;

	proc_create_single("odi_ramlog_prev", 0400, NULL, odi_ramlog_prev_show);
	raw = proc_create("odi_ramlog_prev_raw", 0400, NULL, &odi_ramlog_prev_raw_ops);
	if (raw)
		proc_set_size(raw, sizeof(odi_ramlog_prev));
	pr_info(DRV_NAME ": boot %u slot %d, previous boot saved in /proc/odi_ramlog_prev\n",
		(unsigned int)odi_ramlog_boot, (int)odi_ramlog_slot);
	return 0;
}
device_initcall(odi_ramlog_proc_init);

#endif /* __KERNEL__ */
