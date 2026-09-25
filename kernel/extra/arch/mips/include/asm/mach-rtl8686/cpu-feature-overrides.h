/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686, Lexra RLX5281 CPU).
 *
 * Hardcode the feature set instead of leaving it to the runtime
 * current_cpu_data.options probe: single fixed CPU type, so this is a
 * straight compile-time win (smaller/faster, matching the size-minimised
 * -Os build this port targets), and it is how other single-board legacy
 * MIPS ports in this tree (mach-dec, mach-jz4740, ...) already do it.
 *
 * Values are measured, not assumed: ll/sc/sync/madd are present, and
 * mul/clz/teq/beql/bnel trap on this core (fls() must not use clz/clo).
 * No FPU on this board -- MIPS_CPU_FPU is never set for this CPU.
 */
#ifndef __ASM_MACH_RTL8686_CPU_FEATURE_OVERRIDES_H
#define __ASM_MACH_RTL8686_CPU_FEATURE_OVERRIDES_H

#define cpu_has_tlb		1
#define cpu_has_4kex		0
#define cpu_has_3k_cache	0	/* ours: arch/mips/rtl8686/cache.c */
#define cpu_has_4k_cache	0
#define cpu_has_fpu		0
#define cpu_has_32fpr		0
#define cpu_has_counter		0
#define cpu_has_watch		0
#define cpu_has_divec		0
#define cpu_has_vce		0
#define cpu_has_cache_cdex_p	0
#define cpu_has_cache_cdex_s	0
#define cpu_has_prefetch	0
#define cpu_has_mcheck		0
#define cpu_has_ejtag		0
#define cpu_has_llsc		1
#define cpu_has_mips16		0
#define cpu_has_mips16e2	0
#define cpu_has_mdmx		0
#define cpu_has_mips3d		0
#define cpu_has_smartmips	0
#define cpu_has_rixi		0
#define cpu_has_vtag_icache	0
#define cpu_has_dc_aliases	0
#define cpu_has_ic_fills_f_dc	0
#define cpu_has_pindexed_dcache	0
#define cpu_has_dsp		0
#define cpu_has_dsp2		0
#define cpu_has_dsp3		0
#define cpu_has_mipsmt		0
#define cpu_has_userlocal	0
#define cpu_has_clo_clz		0

/*
 * Not overridden here (no #ifndef guard in cpu-features.h, so an override
 * here would just be clobbered by its own unconditional #define, or worse,
 * generate a macro-redefined warning):
 * cpu_has_mips_r/_r1/_r2/_r5/_r6. All already compute to 0 for us since no
 * CPU_MIPS32_R* Kconfig symbol is selected for this CPU (CPU_R3000).
 */

#endif /* __ASM_MACH_RTL8686_CPU_FEATURE_OVERRIDES_H */
