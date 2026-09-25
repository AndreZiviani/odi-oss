/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686), RLX5281 core parameters.
 *
 * Measured cache/TLB geometry -- hardware facts, not a design choice
 * this port gets to pick.
 */
#ifndef __ASM_MACH_RTL8686_CPU_GEOMETRY_H
#define __ASM_MACH_RTL8686_CPU_GEOMETRY_H

#define RTL8686_DCACHE_SIZE		(32 << 10)
#define RTL8686_ICACHE_SIZE		(64 << 10)
#define RTL8686_DCACHE_LINE		32
#define RTL8686_ICACHE_LINE		32
#define RTL8686_TLB_ENTRIES		64

#endif /* __ASM_MACH_RTL8686_CPU_GEOMETRY_H */
