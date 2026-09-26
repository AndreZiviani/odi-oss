/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_tbl.h -- the indirect table handshakes of the switch core and
 * the GPON block: the downstream GEM port CAM, the Alloc-ID CAM, the VLAN
 * egress tag group and the table engine behind TABLE_CMD. Plain C,
 * host-testable (test/odi_switch_test.c, test/odi_switch_tbl_desc_test.c).
 *
 * A polling primitive returns 0, or -ETIMEDOUT when its poll ran out
 * (odi_poll_reg(), odi_switch_reg.h); in the host mock the operation is
 * done at the first read.
 */
#ifndef ODI_SWITCH_TBL_H
#define ODI_SWITCH_TBL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "odi_switch_hw.h"

/* The bound of every table-engine and CAM poll: 1000 reads 1 us apart.
 * The bound was 1000 back-to-back reads; each read now waits at least as
 * long as one did, so an operation that finished inside the old bound
 * finishes inside this one. A timeout costs about a millisecond with
 * interrupts off (the CAMs, under odi_switch_dsf_lock).
 */
#define ODI_SW_TABLE_POLL_DELAY_US	1U
#define ODI_SW_TABLE_POLL_US		1000U

/* -EOPNOTSUPP as the target kernel numbers it (95 on MIPS Linux). Spelled
 * as a number because the host tests assert the value and a host libc may
 * number it differently (macOS: 102).
 */
#ifndef ODI_SW_EOPNOTSUPP
#define ODI_SW_EOPNOTSUPP	(-95)
#endif

/* The downstream GEM port CAM: DSF_GEM_CAM_CTL (write, row idx), the GEM
 * port id into DSF_GEM_CAM_WDATA, DSF_GEM_CAM_CTL again with REQ set,
 * poll for DONE, then DSF_GEM_FLOW_TYPE(idx) = traffic_cfg whatever the
 * poll returned, as the capture of cmd 25 shows.
 *
 * idx: the row (the downstream flow id, 0..127). gem_port_id: 12 bits.
 * traffic_cfg: FLAGS (odi_switch_hw.h), built by the caller.
 */
int odi_switch_gpon_ds_port_write(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg);

/* The same for a caller that already holds odi_switch_dsf_lock, for a
 * sequence that must stay whole around it: the CAM search before the
 * write in odi_gpon_hw_gem_port_encrypted(), the slot record after it in
 * odi_sw_gpon_usflow_set().
 */
int __odi_switch_gpon_ds_port_write(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg);

/* The Alloc-ID CAM: the same handshake on DSF_ALLOC_CAM_CTL/_WDATA,
 * without the flow type word. No capture exercises it: it follows the
 * register table, and odi_switch_test.c says so.
 *
 * idx: the row (0..31). alloc_id: 12-bit G.984.3 Alloc-ID.
 */
int odi_switch_gpon_alloc_write(uint32_t idx, uint32_t alloc_id);

/* The two CAM primitives take odi_switch_dsf_lock themselves, so they are
 * safe from the GPON interrupt path. odi_switch_table_write() is not: its
 * caller holds odi_switch_lock, a mutex, since the table engine is driven
 * from process context only.
 */

/* The VLAN egress tag group of cmd 51: VLAN_INGRESS_CHECK = ingress_mask,
 * then PORT_EGRESS_TAG_MODE(port) = tag[port], for ports 0..3. Plain
 * writes, no handshake.
 */
void odi_switch_vlan_egress_tag_group_write(uint32_t ingress_mask, const uint32_t tag[4]);

/* One row of a table behind TABLE_CMD, for the tables odi_sw_table_desc[]
 * describes (odi_switch_hw.h):
 *
 *   1. TABLE_WRITE_WORD[0..n-1] in reverse: WORD[0] gets data[n-1], the
 *      order the captured stock writes use;
 *   2. TABLE_CMD read-modify-write: ROW = index + addr_offset, METHOD 1,
 *      IS_WRITE 1, TABLE_KIND = the descriptor type, START 1;
 *   3. poll TABLE_STATUS until the engine is idle (bounded).
 *
 * A table the descriptors do not cover returns -EOPNOTSUPP rather than
 * guess a TABLE_KIND. No capture records this handshake itself (the
 * tracer logs the row one level above it, as T/D records), so
 * test/odi_switch_tbl_desc_test.c checks the register words against the
 * descriptors, and the mock checks table, index and data against the
 * cmd 51 fixtures.
 *
 * index: the row before addr_offset, as a T record shows it. data: word 0
 * first, as D records list them.
 */
int odi_switch_table_write(uint32_t table, uint32_t index, const uint32_t *data, uint32_t n_words);

/* There is no table read: no capture holds one for these tables. The L2
 * table has its own reader, odi_switch_l2.c, on the same register block
 * under TABLE_KIND 0.
 */

#endif /* ODI_SWITCH_TBL_H */
