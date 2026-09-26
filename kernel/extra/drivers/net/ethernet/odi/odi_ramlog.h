/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_ramlog.h -- CONFIG_ODI_RAMLOG, a struct console driver that mirrors
 * every console line into DRAM.
 *
 * WHY a console driver instead of patching printk.c directly: an earlier
 * version of this mirror patched log_store(), a printk-core internal
 * that kernel 5.x replaced with a lockless ring buffer
 * (printk_ringbuffer) -- that approach has no target past roughly 5.0
 * and cannot be forward-ported hunk-for-hunk. struct console,
 * register_console() and the CON_* flags are the one interface printk
 * has kept stable for this whole span; a driver against that API builds
 * unchanged across kernel versions -- odi_ramlog.c has no
 * __KERNEL__-version branch at all, only the one __KERNEL__ vs host
 * branch every other odi_* driver already uses.
 *
 * On-memory format -- UNCHANGED from the earlier printk-patch mirror,
 * byte for byte, so tools/memprobe/ramlog-read.sh (the stock-side reader,
 * run from whichever image boots after a revert) needs no change
 * either:
 *
 *   Two DRAM pages the memory map leaves out survive a watchdog reset
 *   (not a cold power cycle):
 *   physical 0x017ff000 ("page A") and 0x01fff000 ("page B"), KSEG1
 *   (uncached) at 0xa17ff000 / 0xa1fff000. Each page:
 *
 *     offset  0   u32   magic (ODI_RAMLOG_MAGIC_A or _B)
 *     offset  4   u32   count
 *     offset  8   u32   tag1  (page A only: HEAD, see below)
 *     offset 12   u32   tag2  (page A only: JSK, see below)
 *     offset 16   ODI_RAMLOG_DATA (4080) bytes of log text
 *
 *   Page A: the first ODI_RAMLOG_A_DATA (4016) bytes ever written, then
 *   frozen -- count stops advancing once it reaches ODI_RAMLOG_A_DATA.
 *   The last ODI_RAMLOG_META_SIZE (64) bytes of the page hold the boot
 *   metadata block below instead of text. An older reader that takes
 *   min(count, 4080) bytes from +16 still reads exactly the text, because
 *   count never exceeds 4016; a page written by an older image (text up
 *   to 4080 bytes, no metadata) is told apart by the metadata magic.
 *   Page B: a ring of the LAST ODI_RAMLOG_DATA bytes -- count keeps
 *   advancing without limit, byte n lands at offset 16 + (n % 4080);
 *   ramlog-read.sh reconstructs chronological order from count % 4080.
 *
 *   Both u32 fields are stored big-endian (odi_ramlog_wr32()/odi_ramlog_
 *   rd32() below do this explicitly, byte by byte, rather than through a
 *   native word store the way an earlier port did through a plain
 *   `volatile u32 *`): this target is a fixed big-endian MIPS32 part either
 *   way (that earlier native-word store and ramlog-read.sh's own
 *   struct.unpack(">I...") already agree on that), but writing the byte
 *   order out explicitly removes any
 *   dependency on the compiler assumed CPU endianness -- the same
 *   bytes land in DRAM regardless of what -EB/-EL the toolchain defaults
 *   to, and the host test (odi_ramlog_test.c, run on an x86_64/arm64
 *   little-endian host) exercises the exact byte layout the reader parses
 *   instead of the host's own native order.
 *
 * HEAD/JSK tags: an earlier port's arch/mips/kernel/head.S hunk stamped these before
 * any C code ran at all -- HEAD the instant execution reached that point
 * in the boot assembly, JSK ("jump to start_kernel") the instant just
 * before start_kernel() was called -- proving how far a failed trial got
 * even when it never reached printk. A console driver cannot reach that
 * low, not even the early-console path below: literal head.S timing needs
 * an assembly hunk, out of scope here. Accepted tradeoff:
 * odi_ramlog_boot_stamp() stamps both tags together, once, so the
 * two-stage distinction the earlier mirror could make is lost, but
 * ramlog-read.sh does not care what the two tag bytes say, only that
 * they are there.
 *
 * Boot metadata block, page A +4032 (ODI_RAMLOG_META_OFF), 64 bytes,
 * stamped by odi_ramlog_meta_stamp() right after odi_ramlog_boot_stamp():
 *
 *     +0   u32   magic ODI_RAMLOG_MAGIC_M ("RLGM")
 *     +4   u32   boot counter: previous value + 1 when the magic was
 *                valid, else 1 (a power cycle restarts it; boots of an
 *                image that does not write the pages are not counted)
 *     +8   u32   slot, 0 or 1, from the last root= on the command line
 *                (31:5 is slot 0, 31:7 slot 1); ODI_RAMLOG_SLOT_UNKNOWN
 *     +12  u32   ODI_RAMLOG_META_FMT, the layout version of this block
 *     +16  40    build id, NUL padded (ODI_RAMLOG_BUILD_ID, see below)
 *     +56  u32   crumb stash: tag  } written by kernel_entry_setup, NOT
 *     +60  u32   crumb stash: step } by this driver (see below)
 *
 * The build id is ODI_BUILD_ID from kernel/build.sh (the image VERSION
 * when one is set, else the same odi-oss-<date>-<rev> default
 * image/build.sh uses), passed to this one object by the Makefile; a
 * build without it falls back to UTS_RELEASE.
 *
 * Previous-boot capture: odi_ramlog_save_prev() copies both pages, 8 KB,
 * into a static buffer before this boot writes anything to them, and
 * /proc/odi_ramlog_prev (decoded) and /proc/odi_ramlog_prev_raw (the 8 KB)
 * expose that copy. Nothing in this driver writes the pages before the
 * copy, but the early crumbs (CONFIG_ODI_EARLY_CRUMBS) do: K1EN at the
 * first instruction of kernel_entry overwrites the page B +8/+12 words
 * where the previous boot left its last crumb, the one piece of a failed
 * boot that says how far it got. So kernel_entry_setup moves those two
 * words to the crumb stash above (odi_crumb_stash in odi-early-crumb.h,
 * eight instructions, before K1EN), and odi_ramlog_save_prev() puts them
 * back into the saved page B header. The saved copy is then the two pages
 * exactly as the previous boot left them, except the stash words, which
 * hold the crumb of the boot before that one.
 *
 * Early console (kernel/618 board hook): arch/mips/rtl8686/board.c's own
 * prom_init() -- much earlier than console_init() -- calls
 * odi_ramlog_early_console_init() (this file's __KERNEL__ half) instead:
 * it runs the same odi_ramlog_boot_stamp() but registers a CON_BOOT
 * console immediately, so a hang anywhere between prom_init() and
 * console_initcall() still leaves a log. odi_ramlog_console_init() then
 * notices the early console already ran (odi_ramlog_early_active) and
 * skips re-stamping -- re-running odi_ramlog_boot_stamp() a second time
 * would zero the count both pages already hold, erasing everything the
 * early console captured -- and just registers the final non-boot
 * console; printk's own register_console() unregisters CON_BOOT consoles
 * the moment a non-boot one registers, so there is exactly one active
 * writer at any point and no line is ever mirrored twice.
 */
#ifndef ODI_RAMLOG_H
#define ODI_RAMLOG_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

/* ---- On-memory format --------------------------------------------------- */

#define ODI_RAMLOG_MAGIC_A	0x524c4741U	/* "RLGA" */
#define ODI_RAMLOG_MAGIC_B	0x524c4742U	/* "RLGB" */
#define ODI_RAMLOG_TAG_HEAD	0x48454144U	/* "HEAD" */
#define ODI_RAMLOG_TAG_JSK	0x4a534b20U	/* "JSK " */

#define ODI_RAMLOG_MAGIC_M	0x524c474dU	/* "RLGM", the metadata block */

#define ODI_RAMLOG_HDR		16U		/* magic + count + tag1 + tag2 */
#define ODI_RAMLOG_DATA		4080U		/* one 4096-byte page minus ODI_RAMLOG_HDR */
#define ODI_RAMLOG_PAGE_SIZE	(ODI_RAMLOG_HDR + ODI_RAMLOG_DATA)

/* Page A metadata block, see the file header. */
#define ODI_RAMLOG_META_SIZE	64U
#define ODI_RAMLOG_META_OFF	(ODI_RAMLOG_PAGE_SIZE - ODI_RAMLOG_META_SIZE)	/* 4032 */
#define ODI_RAMLOG_A_DATA	(ODI_RAMLOG_DATA - ODI_RAMLOG_META_SIZE)	/* 4016 */
#define ODI_RAMLOG_META_FMT	1U
#define ODI_RAMLOG_M_MAGIC	0U
#define ODI_RAMLOG_M_BOOT	4U
#define ODI_RAMLOG_M_SLOT	8U
#define ODI_RAMLOG_M_FMT	12U
#define ODI_RAMLOG_M_BUILD	16U
#define ODI_RAMLOG_BUILD_LEN	40U
#define ODI_RAMLOG_M_CRUMB_TAG	56U		/* = ODI_CRUMB_STASH in odi-early-crumb.h */
#define ODI_RAMLOG_M_CRUMB_STEP	60U
#define ODI_RAMLOG_SLOT_UNKNOWN	0xffffffffU

/* The saved copy: page A then page B, as the previous boot left them. */
#define ODI_RAMLOG_PREV_SIZE	(2U * ODI_RAMLOG_PAGE_SIZE)

/* Physical addresses, board-fact (32 MB DRAM, two pages the memory map
 * leaves out), and their KSEG1 (uncached) virtual form.
 */
#define ODI_RAMLOG_PAGE_A_PHYS	0x017ff000UL
#define ODI_RAMLOG_PAGE_B_PHYS	0x01fff000UL
#define ODI_RAMLOG_PAGE_A_KSEG1	0xa17ff000UL
#define ODI_RAMLOG_PAGE_B_KSEG1	0xa1fff000UL

/* ---- Portable core (no __KERNEL__ dependency, host-testable) ----------
 *
 * Every function below takes a pointer to one whole ODI_RAMLOG_PAGE_SIZE
 * page. In the kernel it is an __iomem cookie, the KSEG1 address above
 * (the ramlog runs before ioremap() exists), read and written with
 * __raw_readb()/__raw_writeb(); on the host it is a plain byte array.
 */
#ifdef __KERNEL__
#define ODI_RAMLOG_MEM	__iomem
#else
#define ODI_RAMLOG_MEM
#endif

uint32_t odi_ramlog_rd32(const unsigned char ODI_RAMLOG_MEM *page, unsigned int off);
void odi_ramlog_wr32(unsigned char ODI_RAMLOG_MEM *page, unsigned int off, uint32_t val);

/* odi_ramlog_page_reset() -- unconditionally (re)stamp one page's own header:
 * magic, count = 0, and the two tag words (page B always passes tag1 =
 * tag2 = 0; only page A carries HEAD/JSK). The data bytes at offset
 * ODI_RAMLOG_HDR.. are left exactly as they are -- the magic mismatch is
 * what marks them stale, not the bytes themselves.
 */
void odi_ramlog_page_reset(unsigned char ODI_RAMLOG_MEM *page, uint32_t magic,
			    uint32_t tag1, uint32_t tag2);

/* odi_ramlog_boot_stamp() -- the console-API equivalent of the earlier
 * port's head.S hunk (see the file header above for the tradeoff): call once, before
 * the first odi_ramlog_write(), typically right before register_console().
 */
void odi_ramlog_boot_stamp(unsigned char ODI_RAMLOG_MEM *page_a, unsigned char ODI_RAMLOG_MEM *page_b);

/* odi_ramlog_write() -- append `len` bytes of `s` to both pages: page A
 * (odi_ramlog_a_append(), capped, frozen once full) and page B
 * (odi_ramlog_b_append(), a wrapping ring), lazily re-stamping either
 * page first (odi_ramlog_ensure()) if its magic does not already match --
 * the same one-time-in-practice lazy init the earlier port's ramlog_putc()
 * did per byte, done once per call here instead. This is the function the console
 * .write callback (odi_ramlog.c) calls directly with whatever buffer and
 * count printk hands it.
 */
void odi_ramlog_write(unsigned char ODI_RAMLOG_MEM *page_a, unsigned char ODI_RAMLOG_MEM *page_b,
		       const char *s, unsigned int len);

/* odi_ramlog_parse_slot() -- the boot slot named by the LAST root= token
 * of `cmdline` (31:5 or /dev/mtdblock5 is slot 0, 31:7 or /dev/mtdblock7
 * slot 1), ODI_RAMLOG_SLOT_UNKNOWN for any other value or none at all.
 */
uint32_t odi_ramlog_parse_slot(const char *cmdline);

/* odi_ramlog_meta_stamp() -- write this boot's metadata block into page A.
 * Reads the previous block first, for the boot counter. Returns the new
 * counter.
 */
uint32_t odi_ramlog_meta_stamp(unsigned char ODI_RAMLOG_MEM *page_a, uint32_t slot,
			       const char *build_id);

/* odi_ramlog_save_prev() -- copy both pages into `prev`
 * (ODI_RAMLOG_PREV_SIZE bytes). With `crumbs_stashed` set (the kernel was
 * built with CONFIG_ODI_EARLY_CRUMBS, so kernel_entry_setup moved the page
 * B crumb words into the page A stash), the saved page B +8/+12 are taken
 * from the stash instead.
 */
void odi_ramlog_save_prev(unsigned char *prev, const unsigned char ODI_RAMLOG_MEM *page_a,
			  const unsigned char ODI_RAMLOG_MEM *page_b, int crumbs_stashed);

/* odi_ramlog_render() -- the decoded text of a saved copy, the same
 * reconstruction tools/memprobe/ramlog-read.sh does: one metadata line,
 * then page A (the head of the boot log) and page B (the tail ring, in
 * chronological order). Output goes through `emit` in pieces, so the
 * kernel can hand them straight to seq_write() without an 8 KB bounce
 * buffer. `now_boot`/`now_slot` describe the running boot, for the second
 * line.
 */
typedef void (*odi_ramlog_emit_fn)(void *ctx, const char *s, unsigned int len);
void odi_ramlog_render(const unsigned char *prev, uint32_t now_boot, uint32_t now_slot,
		       odi_ramlog_emit_fn emit, void *ctx);

/* ---- Early console (kernel-only; kernel/618 board hook) -----------------
 *
 * odi_ramlog_early_console_init() -- see the file header above. Declared
 * unconditionally here (odi_ramlog.c defines it whenever __KERNEL__ is
 * set, same as odi_ramlog_console_init()); board.c's own extern is what
 * actually gates the call on CONFIG_ODI_RAMLOG.
 */
#ifdef __KERNEL__
void odi_ramlog_early_console_init(void);
#endif

#endif /* ODI_RAMLOG_H */
