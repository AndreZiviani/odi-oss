// SPDX-License-Identifier: GPL-2.0
/*
 * Cache management for the Realtek RTL9602C / Lexra RLX5281.
 *
 * Whole-cache operations go through the Lexra CCTL register (CP0 $20),
 * per-line ones through the MIPS III `cache` instruction. Nothing is
 * coherent, DMA included. Geometry is fixed (cpu-geometry.h).
 *
 * Installed from plat_mem_setup(); mainline cpu_cache_init() runs later,
 * installs nothing (cpu_has_3k_cache and cpu_has_4k_cache are 0) and only
 * builds the protection map from _page_cachable_default set here.
 * docs/KERNEL.md "The CPU".
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

/* Per-line `cache` op codes, verified on this core in the DMA path. */
#define RLX5281_CACHEOP_D_HIT_INV	0x11
#define RLX5281_CACHEOP_D_HIT_WB_INV	0x15
#define RLX5281_CACHEOP_D_HIT_WB	0x19
/* Combined: write the dcache line back, invalidate the icache line. */
#define RLX5281_CACHEOP_DWB_IINV	0x1b

static unsigned long rlx5281_dcache_size;
static unsigned long rlx5281_icache_size;
static unsigned long rlx5281_dcache_lsize;
static unsigned long rlx5281_icache_lsize;

/*
 * Whole-cache CCTL op: write 0 to arm the register, then the op bits.
 * The two nops after each write are a conservative CP0 hazard margin,
 * not a measured figure.
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

/* Kernel addresses: the dcache is written back before the icache line
 * is dropped, both in the one combined op.
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
 * Whole-icache invalidate. How the icache is indexed is unknown: a
 * per-line invalidate through the KSEG0 alias reaches only lines of that
 * colour, while CCTL drops every line. docs/KERNEL.md "The CPU".
 */
static void rlx5281_invalidate_icache_all(void)
{
	rlx5281_cctl(RLX5281_CCTL_ICACHE_INV);
}

/*
 * Before an unmap (COW, reclaim, migration) and after copy_to_user_page()
 * (ptrace, /proc/pid/mem). The dcache goes through the kernel alias; an
 * executable mapping also loses the whole icache, for the colour reason
 * above. Without RIXI nearly every mapping is VM_EXEC.
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
 * Every path that gives a page new contents for a process ends here
 * before the page is mapped: __update_cache() and copy_user_highpage().
 * R3K TLBs have no no-exec bit, so the page may be executed. Write the
 * dcache back, sync so no icache refill reads DRAM ahead of it, then drop
 * the whole icache: the previous life of the page can have left lines at
 * any colour. docs/KERNEL.md "The CPU".
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

	/* Build the page routines and push them out of the dcache before
	 * their first call.
	 */
	build_clear_page();
	build_copy_page();
	rlx5281___flush_cache_all();

	/* No hardware coherency: ordinary cacheable pages, no coherent mode. */
	_page_cachable_default = _CACHE_CACHABLE_NONCOHERENT;
}
