// SPDX-License-Identifier: GPL-2.0
/*
 * Cache management for the Realtek RTL9602C / Lexra RLX5281.
 *
 * The RLX5281 is an R3000-class core (built as CPU_R3000) with one
 * Realtek/Lexra addition this file depends on: CP0 register 20, the
 * CCTL register, drives whole-cache writeback/invalidate operations.
 * Per-line operations go through the ordinary MIPS III `cache`
 * instruction instead. There is no hardware cache coherency and DMA
 * is non-coherent, so every DMA-facing hook below has to move data
 * through one of these two paths by hand.
 *
 * Sizes and line lengths are fixed for this core (icache 64 KB,
 * dcache 32 KB, 32-byte lines on both) and come from cpu-geometry.h rather
 * than a runtime probe.
 *
 * rlx5281_cache_init() is called from plat_mem_setup() (board.c), so no
 * mainline file has to know about it. Mainline cpu_cache_init() runs
 * later in setup_arch(); with cpu_has_3k_cache and cpu_has_4k_cache 0 it
 * installs nothing and only builds the protection map, from the
 * _page_cachable_default this init has already set.
 *
 * Our own implementation, written from hardware behaviour measured on
 * this core.
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/mmu_context.h>
#include <asm/isadep.h>
#include <asm/io.h>
#include <asm/bootinfo.h>
#include <asm/cpu.h>
#include <asm/cpu-features.h>
#include <asm/cacheflush.h>
#include <asm/mach-rtl8686/rtl8686-barrier.h>

#include "rtl8686.h"
#include <asm/mach-rtl8686/cpu-geometry.h>
#include <asm/mach-rtl8686/rlx5281.h>

extern void cache_noop(void);

/*
 * CCTL: CP0 register 20, whole-cache operations (RLX5281_CCTL_REG and the
 * RLX5281_CCTL_* bits, asm/mach-rtl8686/rlx5281.h -- the one shared home
 * for these, also used by kernel-entry-init.h's cold-reset sequence). A
 * plain two-operand mtc0 to it assembles at any ISA level.
 */

/*
 * Per-line `cache` op codes, measured working on this core in the DMA
 * path today. MIPS III encoding: needs .set mips3 to assemble.
 */
#define RLX5281_CACHEOP_D_HIT_INV	0x11
#define RLX5281_CACHEOP_D_HIT_WB_INV	0x15
#define RLX5281_CACHEOP_D_HIT_WB	0x19
/*
 * Combined per-line op: write the dcache line back, then invalidate the
 * icache line. The one icache op this core runs in production.
 */
#define RLX5281_CACHEOP_DWB_IINV	0x1b

static unsigned long rlx5281_dcache_size;
static unsigned long rlx5281_icache_size;
static unsigned long rlx5281_dcache_lsize;
static unsigned long rlx5281_icache_lsize;

/*
 * Whole-cache CCTL op: write 0 to arm the register, then write the op
 * bits. Two dead cycles after each write are needed before the next
 * CP0 access; a conservative hazard margin, not a measured figure.
 */
static inline void rlx5281_cctl(unsigned int op)
{
	__asm__ __volatile__(
	"	.set	push\n"
	"	.set	mips32\n"
	"	.set	noreorder\n"
	"	mtc0	$0, $%1\n"
	"	nop\n"
	"	nop\n"
	"	mtc0	%0, $%1\n"
	"	nop\n"
	"	nop\n"
	"	.set	pop\n"
	:
	: "r" (op), "i" (RLX5281_CCTL_REG)
	: "memory");
}

/* Single per-line cache maintenance instruction at addr. */
#define rlx5281_cacheop(op, addr)					\
	__asm__ __volatile__(						\
	"	.set	push\n"						\
	"	.set	mips3\n"					\
	"	cache	%0, 0(%1)\n"					\
	"	.set	pop\n"						\
	:								\
	: "i" (op), "r" (addr)						\
	: "memory")

static __always_inline void rlx5281_dcache_line_op(unsigned int op, unsigned long start,
				    unsigned long end)
{
	unsigned long lsize = rlx5281_dcache_lsize;
	unsigned long addr = start & ~(lsize - 1);

	for (; addr < end; addr += lsize)
		rlx5281_cacheop(op, addr);
}

static __always_inline void rlx5281_icache_line_op(unsigned int op, unsigned long start,
				    unsigned long end)
{
	unsigned long lsize = rlx5281_icache_lsize;
	unsigned long addr = start & ~(lsize - 1);

	for (; addr < end; addr += lsize)
		rlx5281_cacheop(op, addr);
}

/*
 * Range helpers: whole-cache CCTL when the range is bigger than the
 * cache it targets, per-line cache ops otherwise.
 */
static void rlx5281_writeback_dcache_range(unsigned long start, unsigned long end)
{
	/*
	 * Write-back plus invalidate, a superset of write-back: the plain
	 * write-back ops (CCTL 0x100, cache 0x19) were never exercised here.
	 */
	if (end - start > rlx5281_dcache_size) {
		rlx5281_cctl(RLX5281_CCTL_DCACHE_WB_INV);
		return;
	}
	rlx5281_dcache_line_op(RLX5281_CACHEOP_D_HIT_WB_INV, start, end);
}

static void rlx5281_invalidate_dcache_range(unsigned long start, unsigned long end)
{
	/*
	 * A whole-cache plain invalidate would drop unrelated dirty lines;
	 * past the cache size write everything back and invalidate instead.
	 */
	if (end - start > rlx5281_dcache_size) {
		rlx5281_cctl(RLX5281_CCTL_DCACHE_WB_INV);
		return;
	}
	rlx5281_dcache_line_op(RLX5281_CACHEOP_D_HIT_INV, start, end);
}

static void rlx5281_flush_dcache_range(unsigned long start, unsigned long end)
{
	if (end - start > rlx5281_dcache_size) {
		rlx5281_cctl(RLX5281_CCTL_DCACHE_WB_INV);
		return;
	}
	rlx5281_dcache_line_op(RLX5281_CACHEOP_D_HIT_WB_INV, start, end);
}

static void rlx5281_probe_cache(void)
{
	rlx5281_dcache_size = RTL8686_DCACHE_SIZE;
	rlx5281_icache_size = RTL8686_ICACHE_SIZE;
	rlx5281_dcache_lsize = RTL8686_DCACHE_LINE;
	rlx5281_icache_lsize = RTL8686_ICACHE_LINE;
}

/*
 * flush_icache_range and friends: the dcache side has to land its
 * writes in memory before the icache side is told to forget what it
 * had cached for the same bytes, or the fetch path can still see the
 * old instructions. An unmapped KSEG0 alias is fine for both halves
 * on this core.
 */
static void rlx5281_flush_icache_range(unsigned long start, unsigned long end)
{
	if (end - start > rlx5281_icache_size) {
		rlx5281_cctl(RLX5281_CCTL_DCACHE_WB_INV | RLX5281_CCTL_ICACHE_INV);
		return;
	}
	rlx5281_icache_line_op(RLX5281_CACHEOP_DWB_IINV, start, end);
}

static void rlx5281___flush_cache_all(void)
{
	rlx5281_cctl(RLX5281_CCTL_DCACHE_WB_INV | RLX5281_CCTL_ICACHE_INV);
}

static void rlx5281_flush_cache_range(struct vm_area_struct *vma,
				       unsigned long start, unsigned long end)
{
	/* No ASID yet: nothing of this mm can be in the caches. */
	if (cpu_context(smp_processor_id(), vma->vm_mm) == 0)
		return;
	if (vma->vm_flags & VM_EXEC)
		rlx5281_cctl(RLX5281_CCTL_DCACHE_WB_INV | RLX5281_CCTL_ICACHE_INV);
	else
		rlx5281_cctl(RLX5281_CCTL_DCACHE_WB_INV);
}

/*
 * Whole-icache invalidate. The icache has no coherence with the dcache,
 * and nothing here establishes how it is indexed: if it uses more index
 * bits than the 4 KB page offset (64 KB of it would need 16 ways not to),
 * a per-line invalidate through the KSEG0 alias of a page reaches only
 * the lines filled at that colour, and lines of the same physical page
 * filled through a user mapping at another colour survive it. CCTL drops
 * every line whatever address filled it.
 */
static void rlx5281_invalidate_icache_all(void)
{
	rlx5281_cctl(RLX5281_CCTL_ICACHE_INV);
}

/*
 * Before an unmap (COW, reclaim, migration) and after a ptrace or
 * /proc/pid/mem write into a mapped page (copy_to_user_page()). The
 * dcache side goes through the kernel alias. For an executable mapping
 * the icache side is the whole cache, for the colour reason above:
 * copy_to_user_page() changes bytes the process may already hold in the
 * icache at its own address. On this CPU nearly every mapping is VM_EXEC
 * (no RIXI, so READ_IMPLIES_EXEC for every binary without PT_GNU_STACK),
 * so this runs on most COW faults, next to the flush_data_cache_page()
 * of the copy; a second invalidate right after the first only costs the
 * lines fetched in between.
 */
static void rlx5281_flush_cache_page(struct vm_area_struct *vma,
				      unsigned long addr, unsigned long pfn)
{
	unsigned long kaddr = KSEG0ADDR(pfn << PAGE_SHIFT);

	if (vma->vm_flags & VM_EXEC) {
		rlx5281_icache_line_op(RLX5281_CACHEOP_DWB_IINV, kaddr, kaddr + PAGE_SIZE);
		rtl8686_sync();
		rlx5281_invalidate_icache_all();
	} else {
		rlx5281_dcache_line_op(RLX5281_CACHEOP_D_HIT_WB_INV, kaddr, kaddr + PAGE_SIZE);
	}
}

/*
 * The one hook every path that gives a physical page new contents for a
 * process goes through before the page is mapped: __update_cache() (a
 * page cache folio marked by flush_dcache_folio(): a squashfs, jffs2 or
 * tmpfs fill, a write(), a folio compaction migrated) and
 * copy_user_highpage() (every COW copy). On R3K TLBs there is no no-exec
 * bit, so every such mapping may be executed, and nothing else on these
 * paths touches the icache.
 *
 * Three steps. The combined per-line op writes the new bytes back from
 * the dcache (and drops the icache lines at the kernel colour). The sync
 * drains the write buffer, so an icache refill cannot read DRAM ahead of
 * the writeback. Then the whole icache goes: the page's previous life
 * (text of a reclaimed or migrated page cache folio, of an exited
 * process, freed initmem) can have left valid lines for it at any colour,
 * and the per-line op alone misses those. 618n1, with only the per-line
 * op, took a store address error at an EPC whose bytes in memory are
 * `jr ra` (docs/KERNEL.md).
 */
static void rlx5281_flush_data_cache_page(unsigned long addr)
{
	rlx5281_icache_line_op(RLX5281_CACHEOP_DWB_IINV, addr, addr + PAGE_SIZE);
	rtl8686_sync();
	rlx5281_invalidate_icache_all();
}

/* cacheflush(2) and the other user-range callers pass user addresses,
 * which a per-line cache op would translate through the TLB; the whole
 * cache is cheap next to a system call and cannot fault.
 */
static void rlx5281_flush_icache_user_range(unsigned long start, unsigned long end)
{
	rlx5281___flush_cache_all();
}

static void rlx5281_flush_kernel_vmap_range(unsigned long vaddr, int size)
{
	rlx5281_flush_dcache_range(vaddr, vaddr + size);
}

static void rlx5281_dma_cache_wback_inv(unsigned long start, unsigned long size)
{
	BUG_ON(size == 0);
	iob();	/* drain the write buffer before the device looks */
	rlx5281_flush_dcache_range(start, start + size);
}

static void rlx5281_dma_cache_wback(unsigned long start, unsigned long size)
{
	BUG_ON(size == 0);
	iob();	/* drain the write buffer before the device looks */
	rlx5281_writeback_dcache_range(start, start + size);
}

static void rlx5281_dma_cache_inv(unsigned long start, unsigned long size)
{
	BUG_ON(size == 0);
	iob();	/* drain the write buffer before the device looks */
	rlx5281_invalidate_dcache_range(start, start + size);
}

void rlx5281_cache_init(void)
{
	extern void build_clear_page(void);
	extern void build_copy_page(void);

	rlx5281_probe_cache();

	flush_cache_all = (void *) cache_noop;
	__flush_cache_all = rlx5281___flush_cache_all;
	flush_cache_mm = (void *) cache_noop;
	flush_cache_range = rlx5281_flush_cache_range;
	flush_cache_page = rlx5281_flush_cache_page;
	flush_icache_range = rlx5281_flush_icache_range;
	local_flush_icache_range = rlx5281_flush_icache_range;
	__flush_icache_user_range = rlx5281_flush_icache_user_range;
	__local_flush_icache_user_range = rlx5281_flush_icache_user_range;
	flush_icache_all = rlx5281___flush_cache_all;

	__flush_cache_vmap = (void *) cache_noop;
	__flush_cache_vunmap = (void *) cache_noop;

	__flush_kernel_vmap_range = rlx5281_flush_kernel_vmap_range;

	flush_data_cache_page = rlx5281_flush_data_cache_page;

	_dma_cache_wback_inv = rlx5281_dma_cache_wback_inv;
	_dma_cache_wback = rlx5281_dma_cache_wback;
	_dma_cache_inv = rlx5281_dma_cache_inv;

	pr_info("Primary instruction cache %ldkB, linesize %ld bytes.\n",
		rlx5281_icache_size >> 10, rlx5281_icache_lsize);
	pr_info("Primary data cache %ldkB, linesize %ld bytes.\n",
		rlx5281_dcache_size >> 10, rlx5281_dcache_lsize);

	/* The generated page routines go through the new icache, so build
	 * them now and push them out of the dcache before anyone calls them.
	 */
	build_clear_page();
	build_copy_page();
	rlx5281___flush_cache_all();

	/* No hardware coherency: ordinary cacheable pages, no coherent mode. */
	_page_cachable_default = _CACHE_CACHABLE_NONCOHERENT;
}
