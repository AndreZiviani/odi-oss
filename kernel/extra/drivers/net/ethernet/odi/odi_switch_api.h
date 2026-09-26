/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_api.h -- the odi_omci.c-facing entry points into the switch
 * core (odi_switch.c and the odi_switch_dal.h leaves), one header instead of odi_omci.c
 * carrying its own hand-copied `extern` for each one.
 *
 * Every CONFIG_ODI_* symbol is bool (built into vmlinux, never a loadable
 * module), so none of this needs EXPORT_SYMBOL* -- a plain declaration is
 * enough for built-in-to-built-in linkage, the same as any other internal
 * kernel interface.
 *
 * odi_reg_read()/odi_reg_write()/odi_switch_mmio_ensure() are declared in
 * odi_switch_reg.h instead (they are also force-included into
 * odi_switch_tbl.c and the leaves).
 */
#ifndef ODI_SWITCH_API_H
#define ODI_SWITCH_API_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* odi_switch.c: the platform settings and the module-load replay, run
 * once by the switch_init write of /proc/odi_omci. Caller holds
 * odi_switch_lock.
 */
int odi_switch_boot_init(void);

#endif /* ODI_SWITCH_API_H */
