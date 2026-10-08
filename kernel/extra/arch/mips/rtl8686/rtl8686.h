/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686): what the files of this
 * directory call in one another. Nothing outside arch/mips/rtl8686 uses it.
 */
#ifndef __RTL8686_H
#define __RTL8686_H

#include <linux/init.h>
#include <linux/linkage.h>

/* board.c: the LX bus clock, read by time.c. */
extern unsigned int rtl8686_lx_hz;

/* irq.c, from arch_init_irq(). */
void __init rtl8686_irq_init(void);

/* time.c, from plat_time_init(). */
void __init rtl8686_clockevent_init(void);

/* cache.c, from plat_mem_setup(). */
void rlx5281_cache_init(void);

/* crumb-int.S, CONFIG_ODI_EARLY_CRUMBS: the IRQE stub for EXCCODE_INT. */
asmlinkage void odi_crumb_handle_int(void);

#endif /* __RTL8686_H */
