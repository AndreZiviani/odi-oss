/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_log.h -- the ACL log counters, shared by everything that
 * counts with them: the probes of diag acl-probe (odi_switch_acl_probe.h,
 * counters 16 to 31) and the multicast group counters
 * (odi_switch_mc_count.h, counters 0 to 15).
 *
 * The layout is another open implementation's measurements, checked here
 * by the probe run of docs/SWITCH.md ("Measured on a stick"): 32 counters
 * of 32 bits from LOG_CNT, free-running (a read does not clear them), and
 * LOG_TYPE, a bit a pair of counters (2k, 2k + 1), 1 counting bytes, FCS
 * included, and 0 frames. LOG_WIDE (0x034004), which would join a pair
 * into one 64-bit counter, is never written: it has not been read on a
 * stick either, and every user here reads its counter often enough for 32
 * bits (odi_switch_mc_count.h says how often).
 *
 * LOG_TYPE has more than one owner, so it is kept as a shadow here and
 * each owner changes the bits of its own pairs alone; the shadow is written
 * whole. The register itself is never read: it is outside every register
 * read made on a stick so far, and a register is not read here on the
 * strength of a guess. Its reset value is taken as 0, frames everywhere,
 * which is also what is left when every owner has gone.
 *
 * Plain C with no kernel dependency; called with odi_switch_lock held.
 */
#ifndef ODI_SWITCH_LOG_H
#define ODI_SWITCH_LOG_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define ODI_SW_LOG_COUNTERS		32U
#define ODI_SW_LOG_CNT_OFF		0x32600U
#define ODI_SW_LOG_TYPE_OFF		0x34008U

/* What counter `counter` counts from now on: bytes (1) or frames (0). It
 * sets the unit of the whole pair, so a caller owns both counters of the
 * pair it names. */
void odi_sw_log_unit(uint32_t counter, uint32_t bytes);

/* The counter word as it stands. */
uint32_t odi_sw_log_read(uint32_t counter);

#endif /* ODI_SWITCH_LOG_H */
