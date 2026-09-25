/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_reg.h -- __KERNEL__ declarations for the odi_reg_read()/
 * odi_reg_write() pair odi_switch_tbl.c, odi_switch_dal.c, odi_gpon_hw.c,
 * odi_gpon_isr.c and odi_gpon_init.c all call. Defined in odi_switch.c;
 * every other __KERNEL__ caller only needs the prototypes, which is all
 * this header carries. Each of those five files `#include`s this header
 * itself (guarded by __KERNEL__, a no-op on the host build) -- the host
 * build needs no such thing: test/odi_switch_mock.h already defines
 * odi_reg_read()/odi_reg_write() as static inline, included ahead of each
 * of those files in one unity-build translation unit, so this header is
 * never reached from that path.
 *
 * Also carries the __KERNEL__ definition of ODI_SW_TABLE_TRACE, a no-op:
 * the table-row trace is a host-test aid (test/odi_switch_mock.h), and
 * the kernel has no tracer to hand the rows to.
 */
#ifndef ODI_SWITCH_REG_H
#define ODI_SWITCH_REG_H

#ifdef __KERNEL__
#include <linux/types.h>

uint32_t odi_reg_read(uint32_t off);
void odi_reg_write(uint32_t off, uint32_t val);

/* odi_switch_mmio_ensure() -- idempotent MMIO map (odi_switch.c's own
 * comment on this function has the why: CONFIG_ODI_BOARD's own odi_board_init()
 * -- arch/mips/rtl8686/board.c -- calls this
 * itself before any odi_reg_write(), since its own device_initcall
 * (core-y, arch/mips) can run before odi_switch_init()'s own (drivers-y).
 */
int odi_switch_mmio_ensure(void);

/* The switch-core locks, defined in odi_switch.c, whose comment above
 * their definitions has what each protects, which contexts take it, and
 * the lock order. The host build gets its own model of all three from
 * test/odi_switch_mock.h instead, the same split as odi_reg_read().
 */
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/lockdep.h>

extern struct mutex odi_switch_lock;
extern spinlock_t odi_switch_dsf_lock;
extern struct mutex odi_i2c_lock;

#ifndef ODI_SW_TABLE_TRACE
#define ODI_SW_TABLE_TRACE(kind, table, index, words, n) do { } while (0)
#endif
#endif

#endif /* ODI_SWITCH_REG_H */
