/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_api.h -- the odi_omci.c-facing entry points into the switch
 * core (odi_switch.c, odi_switch_dal.c), one header instead of odi_omci.c
 * carrying its own hand-copied `extern` for each one.
 *
 * Every CONFIG_ODI_* symbol is bool (built into vmlinux, never a loadable
 * module), so none of this needs EXPORT_SYMBOL* -- a plain declaration is
 * enough for built-in-to-built-in linkage, the same as any other internal
 * kernel interface.
 *
 * odi_reg_read()/odi_reg_write()/odi_switch_mmio_ensure() are declared in
 * odi_switch_reg.h instead of here (that header's own comment has why:
 * they are also force-included into odi_switch_tbl.c/odi_switch_dal.c).
 * The "trigger" functions below are lazy "run this once, remember whether
 * it ran" wrappers around the dal leaves odi_switch_dal.h already
 * declares directly (odi_switch_init_platform(), odi_switch_init_parity(),
 * odi_switch_ds_encrypt(), ...); odi_omci.c's /proc/odi_omci write
 * handlers call the triggers, its status read calls the _rc_get()
 * getters.
 */
#ifndef ODI_SWITCH_API_H
#define ODI_SWITCH_API_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* odi_switch.c */
extern unsigned int odi_switch_platform_init_mask; /* module_param, default 0 */
void odi_switch_modload_init_trigger(uint32_t mask);
int odi_switch_modload_init_rc_get(void);

/* odi_switch_dal.c -- pure dal-leaf logic, no kernel dependency, so the
 * trigger and its rc bookkeeping live there instead of in odi_switch.c.
 */
void odi_switch_platform_init_trigger(uint32_t mask);
int odi_switch_platform_init_rc_get(void);

void odi_switch_parity_init_trigger(uint32_t mask);
void odi_switch_parity_init_trigger_all(void);
int odi_switch_parity_init_rc_get(void);

void odi_switch_ds_encrypt_trigger(uint32_t mask);
int odi_switch_ds_encrypt_rc_get(void);

#endif /* ODI_SWITCH_API_H */
