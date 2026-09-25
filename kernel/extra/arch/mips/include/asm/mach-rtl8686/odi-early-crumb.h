/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 -- CONFIG_ODI_EARLY_CRUMBS.
 *
 * Breadcrumbs for the parts of boot that run before any console, TLB
 * or stack exists. Each crumb stores a 4-byte ASCII tag and a step
 * number straight into DRAM through KSEG1 (no TLB entry needed, valid
 * from the first instruction): ramlog page B header, +8/+12
 * (0xa1fff008/0xa1fff00c), words the ramlog console does not use. The
 * assembly crumbs (odi_crumb below, and crumb-int.S) write the same
 * big-endian layout. The last crumb written is how far the boot got;
 * tools/memprobe reads it back from the stock firmware after a failed
 * trial boot.
 *
 * All of them live in our own files: kernel_entry_setup
 * (kernel-entry-init.h), prom_init, plat_mem_setup, arch_init_irq and
 * plat_time_init (arch/mips/rtl8686/board.c), and the interrupt path.
 * The timer ISR (TICK), the interrupt dispatch (IRQD) and the stub in
 * front of handle_int (IRQE) use the same slot, so after a hang it shows
 * how far the interrupt chain got.
 */
#ifndef __ASM_MACH_RTL8686_ODI_EARLY_CRUMB_H
#define __ASM_MACH_RTL8686_ODI_EARLY_CRUMB_H

/* Ramlog page B, KSEG1, and the two header words the crumbs' own. */
#define ODI_CRUMB_PAGE_B	0xa1fff000
#define ODI_CRUMB_TAG		8	/* 4-byte ASCII tag, big-endian */
#define ODI_CRUMB_STEP		12	/* step number, or a running count */

/*
 * The crumb stash: two words at the end of ramlog page A (its metadata
 * block, drivers/net/ethernet/odi/odi_ramlog.h ODI_RAMLOG_M_CRUMB_TAG,
 * +4088/+4092). kernel_entry_setup moves the previous boot's last crumb
 * there before K1EN overwrites it, so odi_ramlog can save it with the
 * rest of the previous boot's log.
 */
#define ODI_CRUMB_STASH		0xa17ffff8

/* The tags written from assembly, as the big-endian words they store. */
#define ODI_CRUMB_K1EN		0x4b31454e	/* "K1EN", kernel entry */
#define ODI_CRUMB_K2SU		0x4b325355	/* "K2SU", entry setup done */
#define ODI_CRUMB_IRQE		0x49525145	/* "IRQE", interrupt taken */

/* Ordered ring of late events in ramlog page A, KSEG1 (see below). */
#define ODI_CRUMB_RING_BASE	0xa17ff010
#define ODI_CRUMB_RING_LEN	8		/* entries, a power of two */
#define ODI_CRUMB_RING_COUNT	24		/* word index of the count */

#ifdef __ASSEMBLY__

/*
 * odi_crumb tag, step, base, tmp: store tag and step through two scratch
 * registers. li/sw only, no stack, no load: safe from the first
 * instruction of kernel_entry.
 */
#ifdef CONFIG_ODI_EARLY_CRUMBS
	.macro	odi_crumb tag, step, base, tmp
	li	\base, ODI_CRUMB_PAGE_B
	li	\tmp, \tag
	sw	\tmp, ODI_CRUMB_TAG(\base)
	li	\tmp, \step
	sw	\tmp, ODI_CRUMB_STEP(\base)
	.endm

/*
 * odi_crumb_stash base, tmp: copy the page B tag and step words to the
 * crumb stash. Uncached loads and stores only, like odi_crumb. Each lw
 * result is used two instructions later, so no load delay slot is hit
 * under the noreorder of head.S.
 */
	.macro	odi_crumb_stash base, tmp
	li	\base, ODI_CRUMB_PAGE_B
	lw	\tmp, ODI_CRUMB_TAG(\base)
	li	\base, ODI_CRUMB_STASH
	sw	\tmp, 0(\base)
	li	\base, ODI_CRUMB_PAGE_B
	lw	\tmp, ODI_CRUMB_STEP(\base)
	li	\base, ODI_CRUMB_STASH
	sw	\tmp, 4(\base)
	.endm
#else
	.macro	odi_crumb tag, step, base, tmp
	.endm
	.macro	odi_crumb_stash base, tmp
	.endm
#endif

#else /* !__ASSEMBLY__ */

#ifdef CONFIG_ODI_EARLY_CRUMBS

static inline unsigned int odi_crumb_word(const char *tag)
{
	return ((unsigned int)(unsigned char)tag[0] << 24) |
	       ((unsigned int)(unsigned char)tag[1] << 16) |
	       ((unsigned int)(unsigned char)tag[2] << 8)  |
		(unsigned int)(unsigned char)tag[3];
}

static inline void odi_early_crumb(const char *tag, unsigned int step)
{
	volatile unsigned int *p = (volatile unsigned int *)
		(ODI_CRUMB_PAGE_B + ODI_CRUMB_TAG);

	p[0] = odi_crumb_word(tag);
	p[1] = step;
}

#define ODI_EARLY_CRUMB(tag, step)	odi_early_crumb(tag, step)

/*
 * A second, ordered trail for events late in boot, when the single
 * page B slot above is taken over by the TIMER0 tick crumb: the last eight
 * (tag, step, jiffies) triples, 12 bytes each, at ramlog page A +16,
 * followed by a running write count at page A +112. Page A keeps only
 * the first 4080 bytes of console text and stops changing once full,
 * which it is long before userspace, so these words are only
 * overwritten by the text very early in boot. Entry (count - 1) & 7 is
 * the newest.
 */
#include <linux/jiffies.h>

static inline void odi_crumb_ring(const char *tag, unsigned int step)
{
	volatile unsigned int *base = (volatile unsigned int *)ODI_CRUMB_RING_BASE;
	unsigned int n = base[ODI_CRUMB_RING_COUNT];
	volatile unsigned int *e = base + (n & (ODI_CRUMB_RING_LEN - 1)) * 3;

	e[0] = odi_crumb_word(tag);
	e[1] = step;
	e[2] = (unsigned int)jiffies;
	base[ODI_CRUMB_RING_COUNT] = n + 1;
}

#define ODI_CRUMB_RING(tag, step)	odi_crumb_ring(tag, step)

#else /* !CONFIG_ODI_EARLY_CRUMBS */

#define ODI_EARLY_CRUMB(tag, step)	do { } while (0)
#define ODI_CRUMB_RING(tag, step)	do { } while (0)

#endif /* CONFIG_ODI_EARLY_CRUMBS */

#endif /* __ASSEMBLY__ */

#endif /* __ASM_MACH_RTL8686_ODI_EARLY_CRUMB_H */
