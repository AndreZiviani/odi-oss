/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686), RLX5281 barriers.
 *
 * The kernel is built as CONFIG_CPU_R3000, which turns CPU_HAS_SYNC
 * off: mainline wmb(), rmb(), dma_wmb() and __sync() then emit no
 * instruction at all. The RLX5281 does implement sync (measured by
 * executing it), and sync is what drains its write buffer, so a store
 * that a DMA engine must see before a later store (a descriptor body
 * before its ownership bit) needs one we emit ourselves.
 *
 * CPU_R3000 also turns CPU_HAS_WB on, so mb(), iob() and the leading
 * barrier of every readl()/writel() call __wbflush, which board.c points
 * at a function that runs rtl8686_sync(): those still issue a sync,
 * through an indirect call.
 *
 * Our own implementation.
 */
#ifndef __ASM_MACH_RTL8686_BARRIER_H
#define __ASM_MACH_RTL8686_BARRIER_H

/* One sync, whatever CPU_HAS_SYNC says: drain the write buffer. */
static __always_inline void rtl8686_sync(void)
{
	__asm__ __volatile__(
		".set	push\n\t"
		".set	mips2\n\t"
		"sync\n\t"
		".set	pop"
		: : : "memory");
}

#endif /* __ASM_MACH_RTL8686_BARRIER_H */
