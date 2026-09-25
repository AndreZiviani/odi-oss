/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_tbl.h -- indirect table access primitives for the switch core
 * and GPON MAC block: one function per handshake identified from the
 * boot3 OMCI provisioning capture and from the GPON MAC block
 * downstream-port-ID and alloc-ID/T-CONT table handshakes. Plain C, no
 * kernel dependency: compiled on
 * the host by test/odi_switch_test.c against test/odi_switch_mock.h, and
 * later linked unmodified into odi_switch.ko.
 *
 * Every polling primitive here returns 0 on success, -1 if its bounded
 * spin timed out (never happens on the host: the odi_switch_poll()
 * helper in odi_switch_mock.h returns done on the first check there).
 */
#ifndef ODI_SWITCH_TBL_H
#define ODI_SWITCH_TBL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "odi_switch_hw.h"

#ifndef ODI_SWITCH_TBL_MAX_SPINS
#define ODI_SWITCH_TBL_MAX_SPINS	1000U
#endif

/* -EOPNOTSUPP, spelled as the plain negative number rather than the
 * <errno.h>/<linux/errno.h> macro: this codebase is host-tested with the
 * *build host's* libc errno.h (test/odi_switch_tbl_desc_test.c and
 * friends assert the literal value back), and EOPNOTSUPP is NOT the same
 * number on every host libc (95 on Linux, 102 on macOS) -- the target
 * kernel (MIPS, Linux errno numbering) is always 95, so a named constant
 * tied to the host's own errno.h would silently return the wrong value
 * on a non-Linux build host. One definition shared by every file that
 * used to spell out its own copy (odi_switch_cmd.c's own
 * ODI_SW_EOPNOTSUPP, odi_switch_tbl.c's bare -95 returns).
 */
#ifndef ODI_SW_EOPNOTSUPP
#define ODI_SW_EOPNOTSUPP	(-95)
#endif

/*
 * GPON downstream GEM port-ID table, confirmed against
 * boot3 cmd 25 (the 11590000 ns bracket, 5th occurrence of cmd 25):
 * write DSF_GEM_CAM_CTL with
 * MODE=write, IDX=idx, REQ=0; write the GEM Port-ID into
 * DSF_GEM_CAM_WDATA; write DSF_GEM_CAM_CTL again with REQ=1 to fire
 * the operation; poll DSF_GEM_CAM_CTL until DONE reads
 * back set; then write the traffic-type byte into
 * DSF_GEM_FLOW_TYPE[idx] unconditionally -- the source does not gate
 * this write on the result of the poll step.
 *
 * idx: table row (the downstream flow index, 0..127).
 * gem_port_id: 12-bit GEM Port-ID (GEM_PORT).
 * traffic_cfg: the packed is_eth/is_omci/is_mcast/aes_en byte
 *   (FLAGS, 5 bits used) -- callers build this value; this
 *   primitive only places it.
 */
int odi_switch_gpon_ds_port_write(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg);

/* The same handshake for a caller that already holds odi_switch_dsf_lock
 * (odi_switch.c): a sequence that must stay whole around it, such as the
 * CAM search ahead of the write in odi_gpon_hw_gem_port_encrypted(), or
 * the slot bookkeeping after it in odi_sw_gpon_usflow_set().
 * odi_switch_gpon_ds_port_write() above takes the lock itself.
 */
int __odi_switch_gpon_ds_port_write(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg);

/*
 * GPON alloc-ID / T-CONT table. Identical shape to the
 * downstream port table above, against DSF_ALLOC_CAM_CTL/_WR instead
 * of _PORT_IND/_WR, minus the final TRAFFIC_CFG-equivalent write (an
 * alloc-ID has no type/classification sibling register). Not exercised in
 * boot3 (no OMCI AssignedAllocId ran during that capture); this primitive
 * is derived from the register spec alone, not from a trace, and
 * odi_switch_test.c marks its assertion as spec-derived rather than
 * trace-derived for that reason.
 *
 * idx: alloc-ID table row (0..31).
 * alloc_id: 12-bit G.984.3 Alloc-ID (ALLOC_ID).
 */
int odi_switch_gpon_alloc_write(uint32_t idx, uint32_t alloc_id);

/* Both GPON CAM primitives above take odi_switch_dsf_lock (odi_switch.c)
 * themselves, so they are safe from the GPON interrupt path and from
 * process context alike. odi_switch_table_write() below is not: the
 * caller holds odi_switch_lock, a mutex, because the table engine is only
 * ever driven from process context.
 */

/*
 * VLAN egress-tag per-port group: the repeated multi-register pattern
 * boot3 cmd 51 writes for the four ports together (decoded lines around
 * the 18180000 ns mark, raw lines 239-246): VLAN_INGRESS_CHECK (0x013004)
 * rewritten to the same mask
 * before each of the four PORT_EGRESS_TAG_MODE (0x02a000 + 4*port) writes, one
 * pass over ports 0..3. No poll or done bit -- these are plain register
 * writes, not an indirect CAM handshake (unlike the two GPON primitives
 * above). The other 0x013000/0x01300c/0x013010 writes in the same
 * bracket have a register identity at overlapping register-map addresses
 * (VLAN_ACCEPT_FRAMES vs. VLAN_INGRESS_CHECK, both reached through the
 * same per-port array register addressing) that
 * remains unresolved, so they are deliberately not implemented as
 * a primitive here rather than guessed.
 *
 * ingress_mask: the VLAN_INGRESS_CHECK word written before each tag write
 *   (0xf in every instance boot3 shows).
 * tag: the four PORT_EGRESS_TAG_MODE values, port 0..3 in order.
 */
void odi_switch_vlan_egress_tag_group_write(uint32_t ingress_mask, const uint32_t tag[4]);

/*
 * odi_switch_table_write -- the switch-core TABLE_CMD indirect table
 * handshake, for every table
 * id odi_switch_hw.h's odi_sw_table_desc[] resolves (the L2 funnel:
 * ACL_*, CF_*, VLAN -- odi_switch_hw.h has the full funnel membership
 * list and the per-table type/size/datareg_num/addr_offset values).
 * Register field
 * layout from the register table's own TABLE_CMD/STS
 * FIELD lines:
 *
 *   1. write TABLE_WRITE_WORD[0..n-1], WORD ORDER REVERSED: WR_DATA[0]
 *      gets data[n-1] (the caller's LAST word), WR_DATA[n-1] gets
 *      data[0] -- the captured stock table writes reverse the word order
 *      the same way; confirmed against the capture,
 *      not a free choice.
 *   2. read TABLE_CMD (read-modify-write: the register carries the
 *      SPA and RESERVED fields this write does not otherwise touch).
 *   3. set ADDR = index + the table's addr_offset (0 for VLAN, CF_MASK_48_*
 *      and CF_ACTION_*; +size for every CF_RULE_48_* variant; +128 for
 *      ACL_PATTERN -- odi_switch_hw.h's odi_sw_table_desc[] comment has the
 *      source detail), METHOD = 1, IS_WRITE = 1 (write),
 *      TABLE_KIND = the table's descriptor type (a small hardware id, NOT
 *      the same number as the table's enum odi_sw_table index -- e.g.
 *      VLAN is type 1, CLS_RULE_B is type 4), START = 1
 *      (fire); write TABLE_CMD.
 *   4. poll TABLE_STATUS.IN_PROGRESS until clear (bounded spin, same
 *      shape as odi_switch_gpon_ds_port_write()'s poll; not yet
 *      implemented for a real target build, see odi_switch_tbl.c).
 *
 * A table id odi_sw_table_desc[] does not resolve (LUT/L34/HSx funnels,
 * or an L2-funnel id not verified against a capture) returns
 * -EOPNOTSUPP rather than writing a TABLE_KIND with no evidence to back it up.
 *
 * None of this handshake appears as W entries in any capture: boot3's
 * skip list (0x012000-0x012fff) and boot5's tracer both drop it --
 * boot5's T/D/R records come from a hook one level above this handshake,
 * at the stock table-write entry itself. So the CTRL/WR_DATA register content this
 * function produces cannot be checked against a capture; instead
 * test/odi_switch_tbl_desc_test.c asserts it directly against the
 * descriptor and the FIELD lines (one row per table, all five boot5
 * exercises), and odi_mock_table() (test/odi_switch_mock.h) separately
 * checks the table/index/data triple against every dal-cmd51-*.txt
 * fixture.
 *
 * table: an enum odi_sw_table value (odi_switch_hw.h).
 * index: the table row, BEFORE any addr_offset (matches what a capture's
 *   T mark shows -- the offset is this function's own internal detail).
 * data, n_words: the row's data words, in the order odi_mock_table()/the
 *   D records in a capture list them (word 0 first) -- this function
 *   does its own reversal onto the wire, the caller never sees it.
 */
int odi_switch_table_write(uint32_t table, uint32_t index, const uint32_t *data, uint32_t n_words);

/* No table-read function: no boot3/boot5 bracket this codebase has ever
 * captured contains a 't' (table read) entry -- boot5's cmd 51 brackets
 * are write-only, per its header line (`tables=201 runs=46`, all T/D/R).
 * A prior unverified read-side implementation (same handshake with
 * IS_WRITE=0) was removed with no capture ever exercising it, no test
 * covering it, and no caller; reintroduce it against a real 't'-entry
 * capture if one ever turns up. The L2 lookup table is the exception, and
 * it has its own reader: odi_switch_l2.c drives this same register block
 * under TABLE_KIND 0, with the access methods and the status word the LUT
 * needs and these tables do not.
 */

#endif /* ODI_SWITCH_TBL_H */
