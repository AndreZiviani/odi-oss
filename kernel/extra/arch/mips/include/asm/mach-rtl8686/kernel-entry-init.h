/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686), RLX5281 kernel entry.
 *
 * At cold reset, before head.S can trust the caches or jump anywhere
 * else, three things measured on this core have to happen in order:
 *
 *   1. Raise CU3 in CP0 Status. The CCTL register below is gated by
 *      it; without this step the writes to it are silently ignored.
 *   2. Disable both on-chip TCM banks, so they do not shadow ordinary
 *      DRAM addresses. CCTL (CP0 register 20) has two independent
 *      sub-registers reachable through the sel field, and both carry
 *      the same bank-disable bits, so both selects need the write.
 *   3. Invalidate both caches through CCTL: dcache writeback plus
 *      invalidate, and icache invalidate, in one write.
 *
 * Our own implementation, written from hardware behaviour measured on
 * this core.
 */
#ifndef __ASM_MACH_RTL8686_KERNEL_ENTRY_INIT_H
#define __ASM_MACH_RTL8686_KERNEL_ENTRY_INIT_H

#include <asm/mach-rtl8686/odi-early-crumb.h>
#include <asm/mach-rtl8686/rlx5281.h>

#define RLX_STATUS_CU3		0x80000000
#define RLX_CCTL_TCM_BANK0_OFF	0x020
#define RLX_CCTL_TCM_BANK1_OFF	0x800

	.macro	kernel_entry_setup
	/* K1EN: the first instructions of kernel_entry. The CPU got here
	 * and can store to DRAM through KSEG1, before any cache or TLB
	 * state can be trusted. No-op unless CONFIG_ODI_EARLY_CRUMBS.
	 * The stash first: K1EN overwrites the last crumb the previous
	 * boot left, and odi_ramlog saves that boot from the stash.
	 */
	odi_crumb_stash t0, t1
	odi_crumb ODI_CRUMB_K1EN, 1, t0, t1

	.set	push
	.set	mips32
	.set	noreorder

	mfc0	t0, CP0_STATUS
	nop
	li	t1, RLX_STATUS_CU3
	or	t0, t0, t1
	mtc0	t0, CP0_STATUS
	nop
	nop

	mtc0	zero, CP0_RLX_CCTL
	nop
	nop
	li	t0, RLX_CCTL_TCM_BANK0_OFF
	mtc0	t0, CP0_RLX_CCTL
	nop
	nop

	mtc0	zero, CP0_RLX_CCTL
	nop
	nop
	li	t0, RLX_CCTL_TCM_BANK1_OFF
	mtc0	t0, CP0_RLX_CCTL
	nop
	nop

	mtc0	zero, CP0_RLX_CCTL, 1
	nop
	nop
	li	t0, RLX_CCTL_TCM_BANK0_OFF
	mtc0	t0, CP0_RLX_CCTL, 1
	nop
	nop

	mtc0	zero, CP0_RLX_CCTL, 1
	nop
	nop
	li	t0, RLX_CCTL_TCM_BANK1_OFF
	mtc0	t0, CP0_RLX_CCTL, 1
	nop
	nop

	mtc0	zero, CP0_RLX_CCTL
	nop
	nop
	li	t0, RLX_CCTL_CACHE_RESET
	mtc0	t0, CP0_RLX_CCTL
	nop
	nop

	.set	pop

	/* K2SU: the COP3/TCM/cache sequence above ran without hanging. */
	odi_crumb ODI_CRUMB_K2SU, 2, t0, t1
	.endm

	/*
	 * This device is single core; nothing to bring up here.
	 */
	.macro	smp_slave_setup
	.endm

#endif /* __ASM_MACH_RTL8686_KERNEL_ENTRY_INIT_H */
