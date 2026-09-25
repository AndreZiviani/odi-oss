/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_dal.h -- our own DAL leaves, one function per register-write
 * group the v3 OMCI provisioning capture (boot3) exercised through plain
 * switch-core registers. Plain C, no kernel
 * dependency -- built on the host against test/odi_switch_mock.h exactly
 * like odi_switch_tbl.c, and later compiled unmodified with __KERNEL__
 * into odi_switch.ko.
 *
 * Every body here is the literal register sequence the trace shows for
 * that leaf, with the fields the analysis identified as varying (GEM
 * Port-ID, table index, queue ordinal, flood-port slot, ...) turned into
 * parameters. Where the analysis found no varying field at all -- cmd 51's
 * plain-register template, section 2 of the analysis -- the function
 * takes the OMCI argument struct's fields for documentation purposes only
 * and reissues the fixed template every time; this is not a shortcut, it
 * is the finding: activeBdgConn's register-visible part is idempotent.
 *
 * Leaf -> OMCI driver command mapping:
 *
 *   odi_sw_l2_aging_set                    cmd 62 setAgeingTime
 *   odi_sw_port_autoneg_get/_set cmd 30 setPortAutoNegoAbility
 *   odi_sw_port_admin_set            cmd 32 setPortState
 *   odi_sw_port_force_get/_set   cmd 32 setPortState
 *   odi_sw_l2_flood_mask_get/_set  cmd 64 setFloodingPortMask
 *   odi_sw_ponmac_queue_add                cmd 23 setPriQueue, baseline shape (instances 1-8)
 *   odi_sw_ponmac_queue_add_ext            cmd 23 setPriQueue, counter-mask shape (instances 9-13)
 *   odi_sw_ponmac_transceiver_get          cmd 10 getTransceiverStatus
 *   odi_sw_gpon_usflow_set              cmd 25 cfgGemFlow, DS side
 *   odi_sw_ponmac_flow_queue_set           cmd 25 cfgGemFlow, US side
 *   odi_sw_qos_sched_set          cmd 23 ext / cmd 25
 *   odi_sw_cf_add           cmd 51 activeBdgConn, plain-register part only
 *
 * odi_sw_ponmac_queue_add / _ext: the capture shows the queue-add step
 * producing two unrelated write shapes depending on caller state, both
 * under cmd 23 -- a one-register flow-control-threshold pass before any
 * T-CONT exists (setPriQueue); and a seven-register counter-mask
 * program once T-CONTs are being created, interleaved with cmd 21 in the
 * trace. Kept as two functions rather than one branching on a
 * flag: the two shapes touch disjoint register sets and sharing a name
 * would hide that split from a reader, not clarify it.
 *
 * odi_sw_qos_sched_set: both cmd 23 (extended) and cmd 25
 * brackets in the capture touch the per-queue scheduling registers, and
 * the capture does not label individual writes by leaf. This pass attributes the per-queue-ordinal PONQ_COUNT_MASK
 * slot (+190..+194, the queue index itself) to
 * schedulingQueue_get/_set, since that matches the leaf's name most
 * directly -- boot3's cmd 23 (extended) bracket writes that slot as its
 * third write, so odi_sw_ponmac_queue_add_ext() calls
 * odi_sw_qos_sched_set() at that point rather than writing the
 * slot itself, keeping one function owning that register. This is a
 * judgment call of our own, from the register evidence alone.
 *
 * odi_sw_cf_add covers only the named plain-register subset
 * (CLASSIFY_PATTERN_SEL, RMA_CTRL*, LUT
 * limits, PORT_PROTO_VLAN, the DSCP remark control) plus the
 * VLAN_INGRESS_CHECK/PORT_EGRESS_TAG_MODE group via odi_switch_vlan_egress_tag_group_write()
 * (a shared primitive, reused rather than duplicated). It does not cover:
 * the CF rule table payload itself was added later (see the cmd 51 leaf
 * below, which now documents the CF and VLAN row layouts);
 * nor the three VLAN_ACCEPT_FRAMES-adjacent raw addresses
 * (0x013008/0x01300c/0x013010) odi_switch_tbl.h deliberately leaves
 * unresolved. Those two gaps are exercised directly by the host test as
 * raw odi_reg_write() calls alongside this leaf, to reconstruct the full
 * boot3 bracket for compare.py -- they are not part of this leaf's body
 * because their register identity is not confirmed, and inventing a name
 * for an unresolved register is worse than leaving it a bare offset.
 */
#ifndef ODI_SWITCH_DAL_H
#define ODI_SWITCH_DAL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "odi_switch_hw.h"

/* Item-selection bit for odi_switch_init_platform()'s mask, and for
 * odi_switch.c's trigger plumbing that builds one -- shared here so both
 * sides use the same numbering (items 1-8) instead of restating it.
 */
#define ODI_SWITCH_INIT_PLATFORM_ITEM(n)	(1U << ((n) - 1U))
#define ODI_SWITCH_INIT_PLATFORM_ITEM_ALL	0xffU
#include "odi_switch_tbl.h"

/* cmd 62 -- L2 ageing time. Read-modify-write: L2_LOOKUP_SETUP carries four
 * other fields (CAM_FULL_ACTION, ARP_AS_KNOWN,
 * the two multicast hash-mode bits F_26 and F_23, CAM_OFF) this leaf does
 * not own, so the
 * current value is read first and only AGE_TICKS/AGE_ON_LINK_DOWN are
 * changed -- the trace's single write (boot3 line 11) is consistent with
 * a register that reset to 0 at boot, so a fresh host/target run shows
 * the same RMW collapsing to one write.
 *
 * age_spd: AGE_TICKS, 21-bit aging-speed/timer value (3000 in boot3).
 * linkdown_ageout: AGE_ON_LINK_DOWN, 1 in boot3.
 */
void odi_sw_l2_aging_set(uint32_t age_spd, uint32_t linkdown_ageout);

/* cmd 30 -- PHY auto-negotiation ability get/set, the indirect MDIO
 * primitive (odi_switch_tbl.h section 3.3), UNI port 0's PHY (MDIO
 * addresses 0/8/0x12, device/page prefix 0x2000 for read, 0x6000 for
 * write, per the ADR values boot3 shows). _get issues the four read
 * commands (0/8/0x12, twice -- read then verify, matching the trace's
 * repeated pair); _set does the same read-verify pass then the
 * read-modify-write of the three PHY registers.
 *
 * reg_a408, reg_a412, reg_a400: the three 16-bit values written back
 * (advertisement-related registers, per the handler's get-then-set
 * shape) -- field-level meaning not decoded, so these are
 * named by MDIO address rather than a semantic field name.
 */
void odi_sw_port_autoneg_get(void);
void odi_sw_port_autoneg_set(uint32_t reg_a408, uint32_t reg_a412, uint32_t reg_a400);

/* cmd 32 -- port admin enable + MAC force-ability get/set.
 * boot3 shows one write, PORT_FORCE_SELECT(port)=0 -- every FORCE_* bit
 * clear, i.e. auto/PHY-driven, no forced speed/duplex (the UNI port has a
 * real PHY). admin_enable is accepted for the mapping table's sake but not
 * observed as a separate register write in this capture (cmd 32) -- the
 * leaf's admin-state effect is a software flag not traced to hardware.
 *
 * port: per-port PORT_FORCE_SELECT slot (0 in boot3, isp1's UNI port).
 */
void odi_sw_port_admin_set(uint32_t port, uint32_t admin_enable);
uint32_t odi_sw_port_force_get(uint32_t port);
void odi_sw_port_force_set(uint32_t port);

/* cmd 64 -- lookup-miss flood port mask get/set. boot3/boot5 write the
 * same value 4 times to FLOOD_UNKN_UCAST_PORTS (0x01c028, traced to this
 * name, not FLOOD_BCAST_PORTS at 0x01c020, which no capture
 * ever writes). The register
 * packs all four ports' flood-enable bits into one word, so the 4x
 * repeat (a loop over 4 condition/case paths in the 188-line handler)
 * writes the same full word four times, not four different ports; this
 * leaf reproduces that repeat count rather than collapsing it, since the
 * repeat count itself is what compare.py checks bracket-for-bracket.
 *
 * mask: the full FLOOD_UNKN_UCAST_PORTS word (0x7 in boot3/boot5 -- three
 *   flood-eligible ports set).
 */
uint32_t odi_sw_l2_flood_mask_get(void);
void odi_sw_l2_flood_mask_set(uint32_t mask);

/* cmd 23, instances 1-8 -- PON queue add, pre-T-CONT baseline pass:
 * one write to PORT_QUEUE_MAP (0x01c0c0, traced to this
 * register, not the word at 0x01c0a0, which no capture ever
 * writes). th = 0xd4 in boot3/boot5, identical across all 8 instances.
 */
void odi_sw_ponmac_queue_add(uint32_t th);

/* cmd 23, instances 9-13 -- PON queue add, full counter-mask
 * program once a T-CONT exists (interleaved with cmd 21 in the trace).
 * Seven writes, boot3 instance 9 (lines 147-155) as the reference values,
 * in trace order:
 *
 *   1. PONQ_COUNT_MASK+207        bitmask_207  (cumulative, one bit per queue)
 *   2. PONQ_COUNT_MASK+15         base_15      (+8 per instance in the trace)
 *   3. PONQ_COUNT_MASK+(190+n)    sched_value  (via odi_sw_qos_sched_set --
 *      this is the leaf-boundary call noted above: the per-queue-ordinal
 *      slot is written from inside this bracket, by the scheduling-queue
 *      leaf, not duplicated here)
 *   4/5. PONQ_COUNT_MASK+208 (val_208) and PONQ_COUNT_MASK+212-or-213
 *      (bitmask_212_213, 212 for boot5 instances 9-11, 213 for 12-13 --
 *      caller picks with use_213) -- ORDER DEPENDS ON n. All five
 *      full-shape boot5 instances checked (not just two): n==0 (the
 *      first queue, instance 9) writes +208 then +212; n>=1 (instances
 *      10-13) write +212-or-213 then +208, every time, no exception.
 *      n==0 is also the only instance where val_208 is 0 (its natural
 *      "nothing accumulated yet" value) -- consistent with, but not
 *      confirmed as, a first-queue code path distinct from every later
 *      queue's fold-into-an-already-nonzero-mask path. This ordering is
 *      fixed here (odi_switch_cmd_test.sh checks it) since the rule the
 *      boot5 data gives is exact, not guessed.
 *   6. PONQ_COUNT_MASK+(60+n)     0 (constant across every boot3/boot5 instance)
 *   7. PONQ_COUNT_MASK+(125+n)    0x3ffff (constant across every boot3/boot5 instance)
 *
 * n: the per-instance slot offset shared by the 190/60/125 sub-ranges,
 *   and what selects the +208-vs-+212/213 order above.
 */
void odi_sw_ponmac_queue_add_ext(uint32_t n, uint32_t bitmask_207, uint32_t base_15,
				  uint32_t sched_value, uint32_t val_208,
				  uint32_t bitmask_212_213, int use_213);

/* cmd 10 -- transceiver status. Software bit-bang I2C read of the
 * SFP DDM block: I2C_MASTER_SETUP(1) held constant, then two GPIO toggle pairs
 * on PIN_GPIO_SELECT(29) (the varying element across the 5 boot3 instances)
 * bracketing PIN_GPIO_SELECT(31)=1. gpio_lo/gpio_hi are the two values boot3
 * shows for slot 29 (0x62/0x63 in instance 1); slot 31 is always 1.
 */
void odi_sw_ponmac_transceiver_get(uint32_t gpio_lo, uint32_t gpio_hi);

/* cmd 25, DS side -- GEM flow set (the DS GEM-port-ID CAM
 * write; despite the OMCI-side name this handler programs the downstream
 * table).
 * A thin wrapper over odi_switch_gpon_ds_port_write() -- no
 * separate register beyond that primitive's own handshake plus the
 * unconditional TRAFFIC_CFG write it already performs.
 *
 * idx: DS table row. gem_port_id: 12-bit GEM Port-ID.
 * traffic_cfg: FLAGS (3 for the OMCI/broadcast GEM, 2 for data).
 */
void odi_sw_gpon_usflow_set(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg);

/* cmd 25, US side -- flow-to-queue map. Direct US port-map write
 * plus GEM-flow counter-mask bookkeeping (PONQ_COUNT_MASK+37/+20/+21, and the
 * two-word RMW at +235; boot3 line range 200-206, instance 7).
 *
 * slot: US_GEM_PORT_MAP index (0..4 in boot3).
 * gem_port_id: 12-bit GEM Port-ID, US mirror of the DS table entry.
 * bitmask_37: PONQ_COUNT_MASK+37 cumulative bitmask value for this instance.
 * word235_a, word235_b: the two values boot3 writes to PONQ_COUNT_MASK+235
 *   in sequence (an RMW of two sub-fields in that word).
 * val_2021: the 0x07efdf80-shaped value boot3's final write of the
 *   bracket carries (field layout not decoded).
 * use_21: which of PONQ_COUNT_MASK+20/+21 gets val_2021 -- boot3 writes +20
 *   for 4 of its 5 US instances and +21 only for the last (idx 4),
 *   suggesting a wraparound not chased further.
 */
void odi_sw_ponmac_flow_queue_set(uint32_t slot, uint32_t gem_port_id, uint32_t bitmask_37,
				   uint32_t word235_a, uint32_t word235_b,
				   uint32_t val_2021, int use_21);

/* cmd 25 -- per-queue scheduling set. The per-queue
 * ordinal slot of PONQ_COUNT_MASK (+190..+194 in boot3, one new slot per
 * T-CONT/queue activated -- the slot index itself is the varying
 * element). value is 0 in every boot3 instance
 * read (the slot's mere existence, not its content, is what the
 * ordinal encodes here). No cmd exercises the read side (cmd 23 ext) in
 * any capture available; the getter was removed with it, unused.
 *
 * n: the per-queue slot offset (0 in boot3 instance 9 -> PONQ_COUNT_MASK+190).
 */
void odi_sw_qos_sched_set(uint32_t n, uint32_t value);

/* cmd 51 -- classifier entry add, complete: the plain-register
 * template (bit-for-bit identical across every boot3/boot5 instance, and
 * across the tail of the isp2 stock bracket too) plus the table payload:
 * the CF rows this call writes and the VLAN table sweep/tail. One call
 * reproduces the whole W and T/D/R sequence of one cmd 51 bracket.
 *
 * WHAT DECIDES WHICH CF ROWS ARE WRITTEN is not decided here: the
 * command layer (odi_switch_cmd.c, cmd_active_bdg_conn) derives the rows
 * from the OMCI bridge rule and keeps the table ordered; this leaf writes
 * what it is handed, in order. The row layouts it writes are decoded
 * below.
 *
 * ---- CF (classification) rows: what the fields mean ----
 *
 * Three tables, same row index: CLS_RULE_B and CLS_MASK_B
 * (2 words each) hold the match, CLS_US_ACTION or CLS_DS_ACTION (3 words)
 * the treatment -- the direction bit of the rule selects which action
 * table the hardware consults. Word 0 of a row is its most significant
 * word (the order odi_switch_table_write() takes and a capture lists).
 * Row 255 is the downstream catch-all the module-load replay installs
 * (force forward, no tag change). The captured allocation puts the more
 * specific rows at lower indexes, which implies the lowest matching index
 * wins -- inferred from that ordering, not measured.
 *
 * The match is a TCAM data/care pair, not value/mask: for every bit,
 * RULE = data AND care, MASK = care AND NOT data, so a bit set in either
 * word is cared about and the two words never overlap. Every service row
 * of both captures has this property, and the stock diag CLI
 * prints the rows back as databit/carebit with care = RULE | MASK.
 * Match fields (bit numbers over the 64-bit pair, w0 bits = n - 32):
 *   48       VALID (RULE only; MASK has no field here -- see mask_w0)
 *   47..32   ethertype / inner-tag word (never cared in any capture)
 *   31       U_D, direction: 1 downstream (frame from the PON), 0 upstream
 *   30..23   TOS / GEM index (never cared: downstream rows match on the
 *            tag only, whatever GEM port the frame arrived on)
 *   22..11   VID of the outer tag
 *   10..8    priority (p-bit) of the outer tag
 *   7..5     internal priority (never cared)
 *   4        frame carries an S-tag
 *   3        frame carries a C-tag
 *   2..0     source port (upstream rows: the UNI the frame came in on)
 * Confirmed by value: U_D, VID (10..14), PRI (4, 5 and 0 cared, 8 not),
 * both tag flags and UNI all change in step with the OMCI input across the
 * 12 isp1 brackets; the rest is layout only, never seen non-zero.
 *
 * Action fields, 67 bits over 3 words (w0 = bits 95..64, w1 = 63..32,
 * w2 = 31..0). Common to both directions:
 *   66..61   DSCP value, 60 DSCP remark enable, 59..57 CF priority,
 *   56       CF priority enable                     (all 0 so far)
 *   55..53   C-tag priority source, 52..50 C-tag VID source (1 = take the
 *            C_PRI / C_VID below; the only value any row has used)
 *   49..47   C_PRI, 46..35 C_VID
 *   34..33   C-tag action: 0 none, 1 add (tag), 2 delete, 3 transparent
 *   2..0     S-tag action: 3 delete, 4 transparent (the only two seen;
 *            0 is no operation)
 *   17..15   CS_PRI, 14..3 CS_VID (S-tag values, 0 in every row)
 * Upstream only:
 *   32..31   drop/trap (0: forward)
 *   30..24   upstream flow (stream) id the frame is queued on -- the
 *            us_flow of the bridge rule, 0..4 on isp1
 *   23       take that flow id (1 on every service row)
 *   22..21   S-tag VID source, 20..18 S-tag priority source (1)
 * Downstream only:
 *   32..31   UNI action: 3 forward to the ports in UNI_PMSK (every service
 *            row; the stock diag reads it back as "Forward"), 1 the
 *            row-255 default ("Force forward")
 *   30..27   UNI_PMSK, egress port mask (bit n = switch port n)
 *   23..21   S-tag VID source, 20..18 S-tag priority source (1)
 *
 * mask_w0 bit 16: MASK has no field at bit 48, and the capture shows the
 * bit 0 on the first row written by an insert and 1 on every other row
 * write (the rows an insert shifts, and a rewrite in place). No effect on
 * matching as far as either capture shows; reproduced so the host replay
 * stays byte-identical. The caller decides it per write.
 *
 * CLASSIFY_PATTERN_SEL: one bit per CF row, 32 rows per register word; the
 * leaf writes the word holding the row (row 254 -> word 7, rows 64..95 ->
 * word 2) before each row, all rows on template 0, so the word is 0.
 *
 * ---- VLAN table row (1 word, indexed by VID) ----
 *   3..0     member ports (bit n = switch port n: 0 UNI, 2 PON, 3 CPU)
 *   7..4     untagged ports: members that send the frame without a tag
 *   8        FID/MSTI, 9 S-VLAN check IVL/SVL, 10 IVL/SVL
 *   17..11   extension port mask
 * A service VLAN is 0x15 when the UNI sees it untagged (members UNI + PON,
 * untagged on the UNI: isp1 VID 11, isp2 stock VID 10) and 0x5 when the
 * UNI sees it tagged (isp1 VIDs 10, 12-14). With VLAN filtering on, a
 * frame whose VID row does not list the ingress port is dropped at
 * ingress -- the likely reading of isp2 discarding every downstream frame
 * at the PON port while this table was replayed from isp1 (VID 10 had
 * no row until the ninth replayed call).
 *
 * cf, n_cf: the rows for this call, in write order.
 * vlan_over, n_over: sparse (idx, value) rows 2..4094 for the second
 *   (row-by-row) VLAN pass -- every row not listed writes 0; rows 0, 1
 *   and the 0..4095 sweep never vary and are handled internally.
 */
struct odi_sw_cf_entry {
	uint32_t idx;
	int is_us;		/* selects CLS_US_ACTION vs CLS_DS_ACTION */
	uint32_t rule_w0, rule_w1;
	uint32_t mask_w0, mask_w1;
	uint32_t action_w0, action_w1, action_w2;
};

struct odi_sw_vlan_override {
	uint32_t idx;		/* 2..4094 */
	uint32_t val;
};

/* CF match fields, positions in rule/mask word 1 (w0 holds bits 63..32). */
#define ODI_SW_CF_W0_VALID		(1U << 16)
#define ODI_SW_CF_W1_DS			(1U << 31)
#define ODI_SW_CF_W1_VID_SHIFT		11
#define ODI_SW_CF_W1_VID_MASK		(0xfffU << ODI_SW_CF_W1_VID_SHIFT)
#define ODI_SW_CF_W1_PRI_SHIFT		8
#define ODI_SW_CF_W1_PRI_MASK		(0x7U << ODI_SW_CF_W1_PRI_SHIFT)
#define ODI_SW_CF_W1_STAG		(1U << 4)
#define ODI_SW_CF_W1_CTAG		(1U << 3)
#define ODI_SW_CF_W1_UNI_MASK		0x7U

/* CF action fields: word 1 (bits 63..32) and word 2 (bits 31..0). Bit 32
 * is the high bit of the 2-bit field at 32..31, split across the words.
 */
#define ODI_SW_CF_A1_CPRI_ACT_SHIFT	21
#define ODI_SW_CF_A1_CVID_ACT_SHIFT	18
#define ODI_SW_CF_A1_C_PRI_SHIFT	15
#define ODI_SW_CF_A1_C_VID_SHIFT	3
#define ODI_SW_CF_A1_CACT_SHIFT		1
#define ODI_SW_CF_A2_US_FLOW_SHIFT	24	/* 7 bits */
#define ODI_SW_CF_A2_US_SID_ACT		(1U << 23)
#define ODI_SW_CF_A2_DS_PMSK_SHIFT	27	/* 4 bits */
#define ODI_SW_CF_A2_CSVID_ACT_SHIFT	21
#define ODI_SW_CF_A2_CSPRI_ACT_SHIFT	18
#define ODI_SW_CF_A2_CS_PRI_SHIFT	15
#define ODI_SW_CF_A2_CS_VID_SHIFT	3

#define ODI_SW_CF_CACT_NONE		0U
#define ODI_SW_CF_CACT_ADD		1U
#define ODI_SW_CF_CACT_DEL		2U
#define ODI_SW_CF_CACT_TRANSPARENT	3U
#define ODI_SW_CF_CSACT_NONE		0U
#define ODI_SW_CF_CSACT_DEL		3U
#define ODI_SW_CF_CSACT_TRANSPARENT	4U
#define ODI_SW_CF_DS_UNI_ACT_FWD	3U	/* forward to UNI_PMSK */
#define ODI_SW_CF_TAG_SRC_ASSIGN	1U	/* the *_VID_ACT / *_PRI_ACT value */

/* VLAN row fields. */
#define ODI_SW_VLAN_ROW_MBR_MASK	0xfU
#define ODI_SW_VLAN_ROW_UNTAG_SHIFT	4
#define ODI_SW_VLAN_ROW(mbr, untag) \
	(((uint32_t)(mbr) & 0xfU) | (((uint32_t)(untag) & 0xfU) << ODI_SW_VLAN_ROW_UNTAG_SHIFT))

void odi_sw_cf_add(const struct odi_sw_cf_entry *cf, unsigned int n_cf,
				   const struct odi_sw_vlan_override *vlan_over, unsigned int n_over);

/* Invalidate CF row idx: an all-zero rule row (VALID clear) and an
 * all-zero action row in the table the row used. No capture shows a
 * delete; this is the natural encoding the layout above implies, and the
 * shape odi_switch_init_platform() item 6 already uses for its sweep.
 */
void odi_sw_cf_del(uint32_t idx, int is_us);

/* Write one VLAN table row directly (a service leaving: its VID row
 * back to 0). Not a captured sequence; the full cmd 51 bracket rewrites
 * every row anyway on the next activation.
 */
void odi_sw_vlan_row_set(uint32_t vid, uint32_t val);

/* cmd 9 (getUsDBRuStatus) and cmd 11 (setSignalParameter), the stock OMCI
 * driver "Optical info and control" command group: there is no
 * boot3 evidence for either, and their stub implementations (log once,
 * return -EOPNOTSUPP) had no caller either -- removed.
 */

/* odi_switch_init_platform() -- the one-shot module-load platform init
 * the stock OMCI kernel modules make at load time, which
 * nothing in our own image replicates
 * under SWITCH=oss. NOT called from module init (doing that hung the
 * kernel on an earlier boot attempt): odi_switch.c calls this
 * only from a deliberate trigger (a /proc/odi_omci write, or lazily on
 * the first ODI_OMCI_OP_CMD), well after module init has returned, so a
 * bad call costs a trial rather than a boot.
 *
 * mask: which items to run, one bit per item -- ODI_SWITCH_INIT_PLATFORM_
 * ITEM(n) (odi_switch_dal.c) is 1 << (n-1) for item n. mask == 0 is a no-op (logged, not silently
 * ignored) -- there is no "run everything" default; a caller must name
 * the items explicitly, so a trial can enable item 1 alone, see whether
 * it survives, then add more. See odi_switch_dal.c for the register/
 * table writes, which items could not be encoded with confidence
 * (left out, logged once, not guessed), and the ordering caveat (every
 * implemented item needs its own switch-core block already inited by
 * /proc/rtk_init's "switch" verb -- calling this before that has run is
 * expected to be unsafe no matter which trigger fires it).
 *
 * odi_switch_acl_start_idx: item 8's bookkeeping -- the ACL table row
 * offset the boot-time default entries already occupy
 * (a runtime counter the stock stack keeps, read from
 * live hardware/config state there is no way to read). Zero-initialized, NOT a
 * confirmed value -- any future ACL-row allocator (cmd 25/26/51's own
 * ACL rule add path, not yet index-aware) must add this
 * offset to its own row index before writing, or it can collide with
 * rows the boot-time init already placed. Exported so that future
 * code has somewhere to read it from once it is actually populated.
 */
extern uint32_t odi_switch_acl_start_idx;

void odi_switch_init_platform(uint32_t mask);

/* odi_switch_platform_init_trigger()/_rc_get() -- the lazy "did it fire
 * yet" wrapper odi_omci.c drives (odi_switch_dal.c has the -1/0
 * convention). No EXPORT_SYMBOL: every CONFIG_ODI_* symbol is built in.
 */
void odi_switch_platform_init_trigger(uint32_t mask);
int odi_switch_platform_init_rc_get(void);

/* odi_switch_init_parity()/odi_switch_parity_add() -- the v6-vs-s7 register
 * parity mechanism: a working image (v6, stock OMCI kernel modules restored) and
 * a broken one (s7, SWITCH=oss with odi_switch_init_platform() already run)
 * were dumped register-by-register at the same boot moment (tools/regdump).
 * 64 addresses differed. The first 14
 * (odi_switch_parity_table[] below, the compiled default) were tried on s8
 * with every entry applying cleanly and the OLT still stalling
 * -- none of them is the cause. What is left is the set
 * "v6 parity table" held back (SW_0x015008/SW_0x015048/ACL_PORT_ENABLE, LUT
 * flood, CLASSIFY_SETUP), made loadable from a file instead of
 * compiled in, so trying them (and any later diff) needs no rebuild.
 *
 * Two tables can be active, never both: the compiled default
 * (odi_switch_parity_table[], odi_switch_parity_table_count entries, fixed
 * at build time) or a loaded table (up to ODI_SWITCH_PARITY_MAX_LOADED
 * entries, built at runtime by odi_switch_parity_add() calls). Loading the
 * first entry after boot (or after odi_switch_parity_clear()) switches the
 * active table from compiled to loaded; odi_switch_parity_clear() switches
 * back. odi_switch_parity_active_table()/_active_count()/_is_loaded() read
 * back whichever is active -- odi_switch_init_parity()/_all() always act on
 * the active table, never directly on odi_switch_parity_table[].
 *
 * odi_switch_parity_add(offset, value, mask): appends one entry to the
 * loaded table. offset is checked against odi_switch_mmio_offset_in_bounds()
 * first -- refused (logged, not silently dropped) if it falls outside the
 * mapped MMIO window, the same guard odi_reg_read/_write() themselves use,
 * so a bad offset from a hand-edited or corrupted table file cannot reach
 * a real MMIO access. Refused the same way, table full, once
 * ODI_SWITCH_PARITY_MAX_LOADED entries are loaded. Returns 0 on success,
 * -1 on either refusal.
 *
 * odi_switch_parity_clear(): empties the loaded table and switches back to
 * the compiled default -- lets a trial reset without a reboot.
 *
 * Same trigger posture as odi_switch_init_platform(): NOT called from
 * module init or from the lazy first-OP_CMD path. Four /proc/odi_omci
 * writes exist (odi_omci.c): `parity_add <offset> <value> <mask>` (hex or
 * decimal, kstrtouint base 0) calls odi_switch_parity_add(); `parity_clear`
 * calls odi_switch_parity_clear(); `init_parity` (bare) calls
 * odi_switch_init_parity_all() -- every entry of whichever table is
 * active, any size; `init_parity <mask>` calls odi_switch_init_parity(mask)
 * -- bit n (zero-based, ODI_SWITCH_INIT_PARITY_ENTRY(n)) selects entry n,
 * but only entries 0-31: a uint32_t mask cannot address a loaded table
 * bigger than 32 entries past that point, which is what the bare
 * "init_parity" (odi_switch_init_parity_all(), no mask, any table size) is
 * for. rcS reads /var/config/parity.table on the config partition (via
 * rootfs/skeleton/etc/scripts/parity-load.sh) right after the
 * init_platform trigger -- absent by default, so a normal boot is
 * unchanged until a bisection trial creates that file; tools/regdump/
 * mkparity.py turns a regdump diff.py capture into one, and tools/regdump/
 * parity-v6.table is the reference table generated from the v6-vs-s7 diff
 * (every configuration register, ACL/LUT-flood/CLASSIFY_SETUP included this time).
 * mask == 0 on the explicit-mask write is a documented no-op, same as
 * odi_switch_init_platform().
 *
 * Each entry is a read-modify-write: reg = (read(offset) & ~mask) |
 * (value & mask). Every compiled-default entry uses a full-word mask
 * (0xffffffff) -- v6's own captured value, reproduced bit-for-bit, reserved
 * bits included, since the goal is parity with the one image known to
 * work, not a field-level rewrite -- except entry 13 (VLAN_INGRESS_CHECK), whose
 * mask is narrowed to the register's own 4 meaningful bits (0x0000000f)
 * because odi_switch_init_platform()'s item 4 already manages one of those
 * bits (port 1) on every boot; entry 13 is the deliberate reversal of that
 * item's own earlier port-1 write. Loaded entries (tools/regdump/mkparity.py's own output) use
 * full-word masks throughout -- none of the held-back registers are known
 * to be shared with a field this codebase separately manages.
 */
#define ODI_SWITCH_PARITY_MAX_LOADED	64U

struct odi_sw_parity_entry {
	uint32_t offset;
	uint32_t value;
	uint32_t mask;
	const char *name;
};

extern const struct odi_sw_parity_entry *const odi_switch_parity_table;
extern const unsigned int odi_switch_parity_table_count;

#define ODI_SWITCH_INIT_PARITY_ENTRY(n)	(1U << (n))

int odi_switch_parity_add(uint32_t offset, uint32_t value, uint32_t mask);
void odi_switch_parity_clear(void);
const struct odi_sw_parity_entry *odi_switch_parity_active_table(void);
unsigned int odi_switch_parity_active_count(void);
int odi_switch_parity_is_loaded(void);

void odi_switch_init_parity(uint32_t mask);
void odi_switch_init_parity_all(void);

/* odi_switch_parity_init_trigger()/_trigger_all()/_rc_get() -- same "did it
 * fire" wrapper shape as odi_switch_platform_init_trigger() above.
 */
void odi_switch_parity_init_trigger(uint32_t mask);
void odi_switch_parity_init_trigger_all(void);
int odi_switch_parity_init_rc_get(void);

/* odi_switch_init_modload() -- the module-load replay: the stock OMCI kernel
 * modules were captured writing 6718
 * plain registers and 1032 table rows at load time (regtrace v5, rcS
 * cleared the ring immediately before insmod and dumped it immediately
 * after, so nothing else could have contributed a write). odi_switch_
 * init_platform() and the parity mechanism (odi_switch_init_parity())
 * only ever covered plain registers a whole-switch dump could see; table
 * contents (classify/ACL/VLAN/L2 rows) live behind TBL_ACCESS and were
 * invisible to every register-level tool built before this capture. The headline finding: CLS_RULE_B[255]/
 * CLS_MASK_B[255]/CLS_DS_ACTION[255] is a wildcard-match, downstream-
 * direction classify rule with UNI_ACT=CF_DS_UNI_ACT_FORCE_FORWARD --
 * odi_switch_init_platform()'s own US_NO_MATCH_ACTION (item 1) only covers the
 * upstream unmatched fallback, so nothing in this codebase currently
 * installs this rule's downstream counterpart.
 *
 * /lib/firmware/odi/modload.bin (odi_replay_blob.h, GENERATED by
 * tools/regtrace/mkmodload.py from the module-load register capture --
 * checked in under rootfs/skeleton, not regenerated at build time, so the
 * build does not need the capture) holds one record per write, IN THE
 * CAPTURE'S OWN ORDER: a register entry (kind ODI_SW_MODLOAD_REG) is a plain
 * read-modify-write with a full mask (odi_reg_read()/_write()). The
 * 0x012000-0x01202c TABLE_CMD/STS/WR_DATA/RD_DATA plumbing IS
 * present as raw W lines in this specific capture (the kernel-side skip
 * list that hides it in an OMCI-command capture is not set up this early)
 * but mkmodload.py drops those 4131 writes from the register category on
 * purpose: they are the mechanics behind the very table rows this same
 * capture records as T/D, which odi_switch_table_write() below already
 * reconstructs correctly and in the right WR_DATA-then-CTRL-then-poll
 * order -- replaying the raw writes too would be redundant at best, and
 * a mask-driven partial replay of them divorced from that lockstep could
 * fire the indirect-access start bit against stale WR_DATA at worst. A
 * table entry (kind ODI_SW_MODLOAD_TABLE) goes through odi_switch_
 * table_write() with the row's own table id,
 * index and words. Order is preserved deliberately, not grouped by
 * category, because this capture folds no table run at all (0 `R`
 * entries) -- the captured sweep interleaves four CF tables per loop
 * iteration, so no two consecutive table ops ever share a table id and
 * the host mock's own run-folder (test/odi_switch_mock.h) never gets two
 * candidate rows in a row; replaying in a different order risks folding
 * something the real hardware never folded.
 *
 * category (0-5, ODI_SWITCH_INIT_MODLOAD_ITEM(category) selects it in
 * the mask): 0 plain registers, 1 CF-family bulk-clear sweep rows, 2
 * CF-family specific rule rows (a repeat write to an already-swept
 * index -- mkmodload.py's own rule, mechanical and content-independent:
 * the SECOND write to any one (table, index) pair in this capture can
 * only be a deliberate overwrite), 3 the VLAN row, 4 the L2_UNICAST row, 5 the
 * ACL_ACTIONS/ACL_PATTERN/ACL_PATTERN_MASK rows. SW_0x015008/SW_0x015048/
 * ACL_PORT_ENABLE and CLASSIFY_SETUP -- the registers "v6 parity table"/"parity table from
 * a file" already held back as probable OLT provisioning -- are NOT part
 * of this replay either: the CF-family/VLAN/L2_UNICAST/ACL TABLE rows above
 * are a different thing entirely (row contents behind TABLE_CMD, not
 * plain registers), so nothing here duplicates or overrides that earlier
 * judgment call. (LUT flood is different: it IS part of this replay, as
 * category-0 reg_group 0 below -- s13 found it is where the LAN cut
 * lives, so it needed its own bit, not an exclusion.)
 *
 * category-0 (register) events carry a second selector, reg_group,
 * added by the modload category 0 split: s13 (17:50/17:53)
 * found that mask 0x3f (every category) gets the OLT to provision the
 * ONU through odi_switch for the first time ever, but cuts the LAN
 * asymmetrically (unicast works, broadcast from the LAN to the CPU does
 * not); mask 0x3e (every category except 0) keeps the LAN up but the OLT
 * stalls again -- so BOTH the fix and the cut live inside category 0's
 * 57-family, 2587-event register set, and needed splitting to tell them
 * apart. The bit a reg_group selects, when bit 0 is NOT set (see below):
 *
 *   reg_group  mask bit  family
 *   ---------  --------  ------
 *   0          6         FLOOD -- the three LUT flood-mask addresses,
 *                         0x1c020/0x1c024/0x1c028 (FLOOD_BCAST_PORTS/
 *                         FLOOD_UNKN_MCAST_PORTS/FLOOD_UNKN_UCAST_PORTS), s13's own
 *                         suspect for the LAN cut. Bit 30
 *                         (ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_BIT) is a
 *                         separate, independent modifier: when set, every
 *                         FLOOD event writes 0x0000000f (every port,
 *                         including the CPU) instead of its own captured
 *                         value -- if BOTH bit 6 and bit 30 are set, bit
 *                         30 wins (checked first), so a trial only has to
 *                         flip one bit either way rather than clear the
 *                         other.
 *   1..N       7..6+N     every other register's own family, by
 *                         register address: mkmodload.py's FAMILIES table
 *                         (a register it does not list falls into its own
 *                         "OTHER_<name>" family rather than a wrong one).
 *                         That table's order is the reg_group order and
 *                         is append-only, so a mask bit keeps its family
 *                         across regenerations; mkmodload.py prints the
 *                         current assignment when it writes modload.bin.
 *
 * Bit 0 keeps its original meaning from before this split -- EVERY
 * category-0 register, verbatim, regardless of reg_group -- so
 * ODI_SWITCH_INIT_MODLOAD_ITEM_ALL (0x3f) is UNCHANGED and still
 * reproduces the exact original full replay (s13's own working mask).
 * When bit 0 is set, reg_group's own bits (6 and 7..) and the CPU-forced-
 * flood bit (30) are ignored entirely -- bit 0 is the "do not bother
 * splitting it, give me every register" shortcut, and family-level
 * bisection is only meaningful with bit 0 clear.
 *
 * Same trigger posture as odi_switch_init_platform()/init_parity(): NOT
 * called from module init or the lazy first-OP_CMD path. `init_modload
 * <mask>` on /proc/odi_omci (odi_omci.c) and rcS's own /var/config/
 * modload.mask read (right alongside init_platform's own trigger) are the
 * only two callers. mask == 0 is a documented no-op.
 *
 * The table itself is not compiled in: odi_switch_modload_init_trigger()
 * (odi_switch.c) loads modload.bin with odi_replay_fw_load(), passes it
 * here, and releases it once this returns.
 */
enum odi_sw_modload_event_kind {
	ODI_SW_MODLOAD_REG = 0,
	ODI_SW_MODLOAD_TABLE = 1,
	/* ODI_SW_MODLOAD_SOC ("SDK-init replay" only, kind lowercase `w` in the
	 * raw regtrace format): a write to the SoC-window physical register
	 * block (0xb8xxxxxx, MIPS KSEG1) -- a different bus entirely from the
	 * switch-SDK register bus odi_reg_write() reaches, and one
	 * tools/regtrace/mksdkinit.py silently excluded until it was found to
	 * be dropping the PON-PBO IP-enable write this way. `offset` carries the
	 * full physical address (not a switch-core MMIO offset), `value` the
	 * word to write. odi_switch_sdkinit_apply() (odi_switch_sdkinit.c)
	 * refuses any address not on its own allowlist rather than trusting
	 * the generated data file alone.
	 */
	ODI_SW_MODLOAD_SOC = 2,
};

struct odi_sw_modload_event {
	uint8_t kind;		/* enum odi_sw_modload_event_kind */
	uint8_t category;	/* 0-5, see above; unused (0) for a SOC entry */
	uint8_t reg_group;	/* category 0 only: 0 (FLOOD) or 1..N, see above;
				 * 0 and otherwise unused for a table or SOC entry
				 */
	uint16_t n_words;	/* table entries only; 0 for a register or SOC entry */
	uint16_t table;		/* table entries only: enum odi_sw_table id; 0 for
				 * a register or SOC entry
				 */
	uint32_t offset;	/* register: MMIO offset. table: row index. SOC:
				 * the full physical address (0xb8xxxxxx)
				 */
	uint32_t value;		/* register and SOC entries only */
	uint32_t words[5];	/* table entries only, zero-padded past n_words
				 * (5 is this capture's own widest row, ACL_PATTERN/
				 * ACL_PATTERN_MASK -- odi_sw_table_desc[]'s own datareg_num
				 * max)
				 */
};

#define ODI_SWITCH_INIT_MODLOAD_ITEM(n)		(1U << (n))
#define ODI_SWITCH_INIT_MODLOAD_ITEM_ALL		0x3fU	/* bits 0-5, UNCHANGED by the split */

/* reg_group r (category-0 events only) selects mask bit 6+r -- reg_group 0
 * (FLOOD) is bit 6, reg_group 1 is bit 7, and so on. Bits 7.. go as high
 * as the generated table's own family count needs (mkmodload.py prints
 * the exact list); this codebase caps it at
 * bit 29 (24 families including FLOOD) to leave bit 30 free below and
 * bit 31 spare.
 */
#define ODI_SWITCH_INIT_MODLOAD_REGGROUP_BIT(reg_group)	(6U + (uint32_t)(reg_group))

/* A modifier, not a category or a reg_group: when set (and bit 0 clear),
 * every FLOOD (reg_group 0) event writes 0x0000000f instead of its own
 * captured value -- "all four ports flood, CPU port 3 included" instead
 * of the captured 0x7 (three ports, CPU excluded) s13 ties to the LAN
 * cut. Takes priority over bit 6 (FLOOD's own bit) when both are set.
 */
#define ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_BIT		30U
#define ODI_SWITCH_INIT_MODLOAD_FLOOD_CPU_VALUE	0x0000000fU

struct odi_replay_blob;	/* odi_replay_blob.h */

void odi_switch_init_modload(const struct odi_replay_blob *table, uint32_t mask);

/* odi_switch_ds_encrypt() -- the DS GEM encryption flag. A
 * full-stream capture of the working stock image against odi_switch
 * found the whole data-path gap:
 * DSF_GEM_FLOW_TYPE[1..5] (the downstream data GEM ports) ends at
 * 0x12 on the stock stack and 0x02 on ours -- bit 4
 * (ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_DECRYPT, odi_switch_hw.h) is clear on
 * every one. Downstream data frames on a GEM port the OLT has told the
 * ONU to encrypt are AES-CTR ciphertext on the fibre; a receiver that
 * never turns its own decrypt bit on for that port drops or garbles
 * every one of them, which is why no DHCP offer or ping reply ever came
 * back on odi_switch even after "module-load replay" fixed OLT
 * acceptance -- the upstream half was fine all along.
 *
 * When the stock firmware sets it: the capture shows bit 4 of the
 * DSF_GEM_FLOW_TYPE word being set, together with the DS GEM CAM
 * re-write, right after the OLT sends the G.984.3 "Encrypted_Port-ID"
 * PLOAM message (the OLT names a GEM port and an enable bit) -- an
 * autonomous, OLT-driven event handled from the GPON downstream
 * interrupt, not an OMCI command, matching where the burst lands in the
 * capture (14.92-14.94s, outside every command bracket). The other bits
 * of that word are multicast (bit 0), Ethernet (bit 1) and OMCI (bit 2),
 * all visible in the same capture.
 *
 * On the mixed image (stock GPON code still in the kernel, our switch
 * layer creating the DS GEM ports) the PLOAM is received and acked, yet
 * the bit never lands: the stock handler only updates ports it created
 * itself, and odi_switch's own DS GEM port creation
 * (odi_switch_gpon_ds_port_write(), odi_switch_tbl.c) writes the CAM and
 * FLAGS registers directly at the hardware level. Registering our ports
 * with the stock stack would mean depending on its internal, unstable
 * data structures, a corruption/panic risk calling into built-in kernel
 * code this project does not own. Not done.
 *
 * What this function does instead: the minimal, register-level action --
 * read-modify-write bit 4 of DSF_GEM_FLOW_TYPE(idx) for each GEM
 * port slot mask names, through the same register odi_switch_gpon_ds_
 * port_write() already writes, preserving every other bit (multicast/
 * Ethernet/OMCI) already set for that port. Deliberately NOT reproduced:
 * the CAM read-then-rewrite (0x701080/0x701100/0x701104) the capture shows
 * around the write -- it re-asserts the SAME gem_port_id already in the CAM
 * (the encrypt bit does not change which GEM port a slot maps to), so it changes
 * nothing a fresh RMW of the plain register does not already achieve;
 * the PONMAC_IRQ_ENABLE toggle (0x700040, 0x22 then back to 0) around it --
 * an interrupt-mask save/restore protecting the stock ISR
 * from re-entering while it reprograms the CAM, not something this
 * codepath running outside that ISR needs; and the PLOAM ack transmit
 * (0x7050c0/0x7050e0-0x7050f4) -- the register map names this range as
 * the upstream PLOAM message queue (index and data words), not
 * an AES key/decrypt block -- the send of the
 * ENCRYPTPORT PLOAM's acknowledgement, structurally part of the same
 * handler but a separate protocol action this codebase does not have the
 * PLOAM framing/sequence state to reproduce safely; sending a malformed
 * ack risks confusing the OLT's own PLOAM state machine, while not
 * sending one at all only means the OLT does not get positive
 * confirmation for a message whose actual effect (the encryption bit)
 * this function sets by itself anyway.
 *
 * Trigger: `ds_encrypt <mask>` on /proc/odi_omci (odi_omci.c) and rcS's
 * own /var/config/ds_encrypt.mask read, same shape as every other trigger
 * this codebase has -- a manual/config-gated stand-in for the PLOAM event
 * that could not be reproduced cleanly, so the fix's effect can be
 * proven on hardware first (echo ds_encrypt <mask> once the ONU is
 * provisioned) before any future pass invests in the fuller, riskier
 * bookkeeping-registration fix above. mask bit n selects GEM port slot n
 * (0-31); mask == 0 is a documented no-op.
 */
void odi_switch_ds_encrypt(uint32_t mask);

/* odi_switch_ds_encrypt_trigger()/_rc_get() -- same "did it fire" wrapper
 * shape as odi_switch_platform_init_trigger() above.
 */
void odi_switch_ds_encrypt_trigger(uint32_t mask);
int odi_switch_ds_encrypt_rc_get(void);

/* odi_switch_ds_encrypt_one() -- the single-slot primitive odi_switch_ds_
 * encrypt(mask) is now built on: read-modify-write bit 4 (EN_AES) of
 * DSF_GEM_FLOW_TYPE(idx) to exactly `enable` (0 or 1), leaving every
 * other bit untouched. odi_switch_ds_encrypt(mask) only ever calls this
 * with enable=1 (it has no notion of turning AES back off); the automatic
 * PLOAM hook below calls it with the OLT's own aes bit, set or clear.
 */
void odi_switch_ds_encrypt_one(uint32_t idx, uint32_t enable);

/* --- DS GEM port slot bookkeeping ------------------------------------------
 *
 * gem_port_id -> DS table slot, the reverse of what odi_sw_gpon_usflow_
 * set() (cmd 25, DS side) is given: it takes a slot and a gem_port_id going
 * in, and until now nothing kept the reverse mapping. odi_switch_ds_slot_
 * record() is called from odi_sw_gpon_usflow_set() itself, at DS GEM
 * port creation; odi_switch_ds_slot_find() is what the automatic AES-enable
 * PLOAM hook below uses to turn an OLT-named gem_port_id back into the slot
 * odi_switch_ds_encrypt_one() needs. 32 slots, matching odi_switch_ds_
 * encrypt()'s own mask width and DSF_GEM_FLOW_TYPE's own index range.
 */
#define ODI_SWITCH_DS_SLOT_COUNT 32U

void odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id);
int odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out);

/*
 * Test-only: host tests replay several scenarios in one process and need a
 * clean table between them, the same role odi_switch_cmd_reset_state() (
 * odi_switch_cmd.c) plays for that file's own static tables. Never called
 * from kernel code -- a real boot never needs to forget a slot mapping.
 */
void odi_switch_ds_slot_reset(void);

/* --- Automatic AES enable -------------
 *
 * odi_switch_ds_encrypt(mask) above (the manual `ds_encrypt <mask>` trigger)
 * proved the actual fix on hardware -- s16, every acceptance criterion
 * passed -- but needs an operator to read a gem_port_id -> slot
 * mapping off a capture and pick the mask by hand. Three ways to make it
 * automatic were considered, in the order they were tried:
 *
 * (a) CHOSEN. Observe the OLT's own Encrypted_Port-ID PLOAM directly,
 *     through a hook this codebase can own without touching any stock
 *     data structure. The stock kernel exports a PLOAM-callback
 *     registration entry point (an exported symbol of the stock
 *     vmlinux). The callback gets a 12-byte message record (onu_id, msg_id,
 *     data[10], struct odi_sw_gpon_ploam below) for every PLOAM addressed
 *     to this ONU, before the stock handling; returning
 *     ODI_SW_GPON_PLOAM_CONTINUE (0) leaves the stock handling, including
 *     its PLOAM ack, running exactly as it always has. This hook only
 *     observes; it never suppresses or replaces stock processing.
 * (b) REJECTED. Have odi_switch's own cmd 25 dispatch decide when to flip
 *     aes_en, once it can see which slot carries which gem_port_id (the same
 *     bookkeeping (a) needed anyway, now odi_switch_ds_slot_record() below).
 *     There is nothing to hook there: the Encrypted_Port-ID PLOAM is a
 *     GTC-interrupt-driven autonomous event (see
 *     odi_switch_ds_encrypt()'s own doc), never an OMCI command, so no
 *     OMCI command dispatch ever sees it, cmd 25 included -- (b) would be
 *     watching a path the event never travels.
 * (c) REJECTED as the primary mechanism. Unconditionally set aes_en on every
 *     data DS GEM port (never the OMCI/broadcast port, never multicast) at
 *     creation, gated by a module parameter defaulting on. This trades a
 *     real risk -- a port the OLT does NOT ask to be encrypted would then
 *     be decrypted into garbage by this ONU, with no way to tell from here
 *     that the OLT never sent the PLOAM this codebase would otherwise be
 *     reacting to -- for not depending on the stock export existing.
 *     Every capture available (isp1, s15/s16) shows the OLT
 *     encrypting every data GEM port, so the risk has not been observed
 *     yet, but "not observed yet" is not "impossible", and (a) carries none
 *     of it while still working automatically. Not implemented; if a
 *     future OLT is ever seen NOT to send ENCRYPTPORT for a live data port,
 *     this is the fallback to build, module parameter and all.
 *
 * The message itself (ITU-T G.984.3 Encrypted_Port-ID): gem_port_id = (data[1]<<4)|(data[2]>>4) (12 bits, spanning a
 * full byte and a nibble), aes = data[0]&0x01. odi_switch_gpon_ploam_hook()
 * restates exactly that decode, looks the gem_port_id up via odi_switch_ds_
 * slot_find(), and calls odi_switch_ds_encrypt_one(idx, aes) when found --
 * set OR clear, unlike the manual trigger, which only ever sets. A gem_
 * port_id the table has not seen yet (the PLOAM arrives before the
 * matching cmd 25) is logged and left alone; there is nothing to write.
 *
 * Registration is lazy, from odi_omci_cmd()'s first OP_CMD (odi_omci.c),
 * the same posture already established for odi_switch_platform_init_
 * trigger() and for the same reason: GPON must already be ranging and
 * registered (the registration fails before that) by the
 * time any OMCI command can possibly arrive, so that first call is a safe,
 * already-proven place to register from -- no new rcS gate needed.
 */
struct odi_sw_gpon_ploam {
	uint8_t onu_id;
	uint8_t msg_id;
	uint8_t data[10];
};

#define ODI_SW_GPON_PLOAM_DS_ENCRYPTPORT	0x08U
#define ODI_SW_GPON_PLOAM_CONTINUE		0

/* Not const: the registration entry point (below, #ifdef __KERNEL__)
 * takes a non-const callback, and actually requires the function pointer it is
 * given to match; a const-qualified parameter here compiled and ran fine
 * (MIPS o32 has no ABI difference either way) but tripped -Wincompatible-
 * pointer-types (s17 acceptance build) -- fixed by
 * matching the expected type instead of by silencing the warning.
 */
int odi_switch_gpon_ploam_hook(struct odi_sw_gpon_ploam *ploam);

/* odi_switch_gpon_encrypt_port() -- CONFIG_ODI_GPON's own replacement for
 * odi_switch_gpon_ploam_hook() above, called directly by the FSM/PLOAM
 * core with the Encrypted-Port-ID PLOAM gem_port_id and aes bit
 * already decoded, instead of through a stock GPON module callback.
 * Same slot lookup and same odi_switch_ds_encrypt_one() call as the hook.
 * Returns 0 on success, -1 if no DS slot was recorded for gem_port_id yet.
 */
int odi_switch_gpon_encrypt_port(uint16_t gem_port_id, int enable);

/* No odi_switch_gpon_ploam_register(): it would have registered
 * odi_switch_gpon_ploam_hook() above with a GPON stack that takes a PLOAM
 * callback -- none exists anywhere in this tree (odi_gpon calls
 * odi_switch_gpon_encrypt_port() directly instead), so the registration
 * function was a permanent -EOPNOTSUPP stub with no caller once
 * odi_omci.c's own dead CONFIG_ODI_GPON=n lazy-trigger path was removed;
 * both are gone now.
 */

/* The four register-only diag/omci sockopts served directly
 * against the switch-core MMIO primitive instead of the stock sockopt
 * handler, behind /dev/odi_sw (odi_reg.c). No stock code runs in any of these three
 * paths.
 *
 * odi_sw_reg_get/_set: RTK_OPT_ADDRESS_GET/SET and RTK_OPT_REGISTER
 * together -- both are a plain switch-core MMIO word at a caller-given
 * offset, with no field decode, so one pair of leaves serves every one of
 * them. Bounds-checked against odi_switch_mmio_offset_in_bounds()
 * (odi_switch_hw.h) before touching odi_reg_read/_write, so an
 * out-of-range offset is refused here rather than silently reading 0 or
 * dropping a write with no way for the caller to tell why. Returns 0 on
 * success, -1 if addr is out of bounds or unaligned.
 */
int odi_sw_reg_get(uint32_t addr, uint32_t *value);
int odi_sw_reg_set(uint32_t addr, uint32_t value);

/* odi_sw_mib_get(): RTK_OPT_STAT_PORT. counter is the same index src/diag/
 * src/mib.h's mib_names[] already uses (the stock diag counter
 * ordering, 0-68) -- callers do not need a second numbering. Answered
 * from the PORT_TX_COUNTERS/RX_MIB/OAM_MIB register blocks
 * (odi_switch_hw.h) for the counters that could be attributed to one of
 * those blocks unambiguously (odi_switch_dal.c has the per-counter
 * mapping and, for each one left out, why: mostly the stock diag
 * exposing the same underlying counter under more than one index --
 * direction-less and direction-specific names both present -- with no
 * register evidence to say which of the duplicates a given index reads).
 * Returns 0 and fills *value on a mapped counter for an existing port,
 * -1 otherwise (unmapped counter, or port >= 4) -- there is no sockopt
 * to fall back to, same as an absent /dev/odi_sw.
 */
int odi_sw_mib_get(uint32_t port, uint32_t counter, uint64_t *value);

#endif /* ODI_SWITCH_DAL_H */
