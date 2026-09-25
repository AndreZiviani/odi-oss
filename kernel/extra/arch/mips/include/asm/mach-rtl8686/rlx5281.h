/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Realtek/Lexra RLX5281 CCTL: CP0 register 20, the whole-cache
 * writeback/invalidate register (see arch/mips/rtl8686/cache.c's own
 * comment for how this differs from the per-line MIPS III `cache`
 * instruction that the rest of the cache code uses).
 *
 * One shared home for both consumers, instead of two independent
 * copies: kernel-entry-init.h (raw GAS, before head.S can trust the
 * caches) needs the "$20" register token for a plain `mtc0 rt, $20`;
 * cache.c's inline asm needs a plain immediate for its "i" constraint.
 * Both are the same CP0 register and the same reset bits.
 */
#ifndef __ASM_MACH_RTL8686_RLX5281_H
#define __ASM_MACH_RTL8686_RLX5281_H

#define RLX5281_CCTL_REG		20

#ifdef __ASSEMBLY__
#define CP0_RLX_CCTL			$20
#endif

#define RLX5281_CCTL_DCACHE_INV		0x001
#define RLX5281_CCTL_ICACHE_INV		0x002
#define RLX5281_CCTL_DCACHE_WB		0x100
#define RLX5281_CCTL_DCACHE_WB_INV	0x200

/* dcache writeback+invalidate, icache invalidate, in one CCTL write --
 * kernel_entry_setup's cold-reset cache invalidate (kernel-entry-init.h)
 * and rlx5281_cache_init()'s own reset (cache.c) both want exactly this.
 */
#define RLX_CCTL_CACHE_RESET		(RLX5281_CCTL_DCACHE_WB_INV | RLX5281_CCTL_ICACHE_INV)

#endif /* __ASM_MACH_RTL8686_RLX5281_H */
