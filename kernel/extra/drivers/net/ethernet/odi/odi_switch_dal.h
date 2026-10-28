/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_dal.h -- the switch-core leaves the OMCI driver commands
 * (odi_switch_cmd.c) and the switch init are built from, one function per
 * group of register writes. Each body is the register sequence a capture
 * of the stock image shows for that command, with the fields that vary
 * between instances as parameters; docs/SWITCH.md#command-leaves has the
 * evidence for each.
 *
 *   odi_switch_port.c      L2 ageing, PHY autoneg, port force, flood mask,
 *                          the transceiver I2C select (cmd 62, 30, 32, 64, 10)
 *   odi_switch_qos.c       PON queues, queue scheduling, the upstream
 *                          flow-to-queue map (cmd 23, 25)
 *   odi_switch_ds_gem.c    the downstream GEM port, its slot, the AES bit
 *                          (cmd 25, Encrypted_Port-ID)
 *   odi_switch_cf.c        CF rows and the VLAN table (cmd 51)
 *   odi_switch_platform.c  the platform settings and the module-load replay
 *   odi_switch_mib.c       register and counter reads for /dev/odi_sw
 *
 * Plain C: the host tests build these files against test/odi_switch_mock.h.
 */
#ifndef ODI_SWITCH_DAL_H
#define ODI_SWITCH_DAL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/printk.h>
#define ODI_SW_LOG(fmt, ...) pr_info("odi_switch: " fmt, ##__VA_ARGS__)
#define ODI_SW_WARN(fmt, ...) pr_warn_ratelimited("odi_switch: " fmt, ##__VA_ARGS__)
#define ODI_SW_ERR(fmt, ...) pr_err("odi_switch: " fmt, ##__VA_ARGS__)
#else
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define ODI_SW_LOG(fmt, ...) printf("odi_switch: " fmt, ##__VA_ARGS__)
#define ODI_SW_WARN(fmt, ...) printf("odi_switch: " fmt, ##__VA_ARGS__)
#define ODI_SW_ERR(fmt, ...) printf("odi_switch: " fmt, ##__VA_ARGS__)
#endif

#include "odi_switch_hw.h"
#include "odi_switch_tbl.h"

/* Switch ports: UNI 0, PON 2, CPU 3; port 1 is unused on this board. */
#define ODI_SW_PORT_COUNT		4U

/* --- odi_switch_port.c ---------------------------------------------------- */

/* cmd 62: AGE_TICKS and AGE_ON_LINK_DOWN of L2_LOOKUP_SETUP, a
 * read-modify-write that leaves its other fields alone.
 */
void odi_sw_l2_aging_set(uint32_t age_spd, uint32_t linkdown_ageout);

/* cmd 30: the UNI PHY auto-negotiation registers through the indirect PHY
 * access. _get issues the read commands (each address twice); _set writes
 * the three values back. Their fields are not decoded, so the parameters
 * are named by PHY address.
 */
void odi_sw_port_autoneg_get(void);
void odi_sw_port_autoneg_set(uint32_t reg_a408, uint32_t reg_a412, uint32_t reg_a400);

/* cmd 32: the admin state of a port. enable 0 forces its link down
 * (PORT_FORCE_ABILITY LINK 0, then PORT_FORCE_SELECT LINK 1), enable 1
 * clears that select bit. Bit 4 of each word only: the host-port PAUSE
 * force (below) is kept. A forced host port (odi_sw_host_forced) keeps its
 * select bit either way and moves the forced LINK value instead.
 */
void odi_sw_port_admin_set(uint32_t port, int enable);

/* The host port: the UNI, switch port 0, the SerDes link to the host. */
#define ODI_SW_HOST_PORT		0U

/* Whether the host port may send PAUSE frames to the host: false (the
 * default) forces its TX_PAUSE ability off, true leaves it to the
 * auto-negotiation with the host. Receiving PAUSE is not touched either
 * way. The kernel parameter odi_switch.host_pause (odi_switch.c) sets it.
 * docs/SWITCH.md#host-port-pause has why.
 */
extern bool odi_sw_host_pause;

/* Applies odi_sw_host_pause to the host port: the forced TX_PAUSE value
 * (0), then its FORCE_SELECT bit, and logs the ability the port then runs
 * with. Idempotent; the caller holds odi_switch_lock.
 */
void odi_sw_host_pause_apply(void);

/* Whether the PON port is isolated from the CPU, both ways: true (the
 * default) clears the CPU side from the PON port's isolation mask, so no
 * frame from the PON is ever forwarded to the CPU, and the PON port from
 * the CPU port's, so no frame the CPU sends is forwarded to the PON by
 * lookup; false leaves both masks all open, as the reset value and the
 * stock image do. The kernel
 * parameter odi_switch.pon_isolate (odi_switch.c) sets it.
 * docs/SWITCH.md#pon-port-isolation has why.
 */
extern bool odi_sw_pon_isolate;

/* Applies odi_sw_pon_isolate to the PON and CPU ports' isolation masks
 * (one word) and logs the masks it then reads. Idempotent; the caller holds odi_switch_lock.
 */
void odi_sw_pon_isolate_apply(void);

/* SA_MOVED of the PON port set to "drop the frame, keep the row" (the
 * other ports unchanged). Written after the module-load replay, which
 * writes the register back to 0. Caller holds odi_switch_lock.
 */
void odi_sw_sa_moved_apply(void);

/* Whether the host port runs the 1000BASE-X auto-negotiation (clause 37)
 * with the host: true (the default, what the stock image and every host
 * seen so far run) or false, the PCS forced to 1000BASE-X full duplex for a
 * host whose port has auto-negotiation off. The kernel parameter
 * odi_switch.host_autoneg (odi_switch.c) sets it; userland drives it from
 * HOST_AUTONEG (docs/SWITCH.md#host-port-auto-negotiation).
 */
extern bool odi_sw_host_autoneg;

/* Applies odi_sw_host_autoneg to HOST_FIBER_CTRL: one read-modify-write of
 * the AUTONEG bit (with AUTONEG_RESTART when it turns on), none when the
 * bit already matches. Logs the word before and after. The caller holds
 * odi_switch_lock.
 */
void odi_sw_host_autoneg_apply(void);

/* Whether the host port runs forced, link up, full duplex, 1000 Mbit/s,
 * whatever its SerDes reports: false (the default) leaves the port to its
 * SerDes; true is what the host SerDes modes other than Fiber 1G need
 * (2500BASE-X and HiSGMII, HOST_SERDES), where no 1000BASE-X PCS reports a
 * link to the port. The kernel parameter odi_switch.host_forced
 * (odi_switch.c) sets it; userland drives it from HOST_SERDES
 * (docs/SWITCH.md#host-port-serdes-mode).
 */
extern bool odi_sw_host_forced;

/* Applies odi_sw_host_forced to PORT_FORCE_ABILITY(0) and
 * PORT_FORCE_SELECT(0), keeping an admin lock (cmd 32) across, then the
 * PAUSE policy again, and logs the result. The caller holds
 * odi_switch_lock.
 */
void odi_sw_host_forced_apply(void);

/* LINK_UP of PORT_ABILITY(0): 1 while the host port has a link. A plain
 * read, safe without the lock. On a forced host port (odi_sw_host_forced)
 * it is the forced value, so it says nothing about the host.
 */
uint32_t odi_sw_host_link_up(void);

/* cmd 64: the lookup-miss flood masks, one bit per port, by type (the
 * stock omci_flood_type_t order): unknown unicast, broadcast, unknown
 * multicast.
 */
#define ODI_SW_FLOOD_UNKN_UCAST	0U
#define ODI_SW_FLOOD_BCAST	1U
#define ODI_SW_FLOOD_UNKN_MCAST	2U
uint32_t odi_sw_l2_flood_mask_get(uint32_t type);
void odi_sw_l2_flood_mask_set(uint32_t type, uint32_t mask);

/* The ageing time as L2_LOOKUP_SETUP holds it, in tenths of a second (what
 * cmd 62 wrote). A plain read, for the bridge table of ME 50.
 */
uint32_t odi_sw_l2_aging_ticks_get(void);

/* cmd 53: how many addresses a UNI port may learn. The hardware counts per
 * port in 11 bits, two ports a word; ODI_SW_L2_LEARN_LIMIT_NONE is what the
 * boot replay leaves in every port (the table size, 1088), so it is also
 * "no limit". What happens to a frame from a new address past the limit is
 * LEARN_LIMIT_ACT (0x01c00c), 0 at reset: forward it without learning. That
 * register is never written: a drop there would turn a limit into an
 * outage. A read-modify-write, the other port of the word is kept. The
 * caller holds odi_switch_lock.
 */
#define ODI_SW_L2_LEARN_LIMIT_NONE	1088U
uint32_t odi_sw_l2_learn_limit_get(uint32_t port);
void odi_sw_l2_learn_limit_set(uint32_t port, uint32_t limit);

/* cmd 67: the rate of a UNI port, in units of 8 kbit/s (the unit of both
 * registers). _police is the ingress meter of the port (frames entering
 * from the host), _shape the egress shaper (frames leaving to the host).
 * ODI_SW_UNI_RATE_NONE is the reset value of both, the 19-bit maximum, "no
 * limit". The shaper of port 0 only holds if a second register carries
 * half the value in units of 16 kbit/s (SHAPE_2G5_CFG, bits 18..0, its bit
 * 19 the 2.5G line-rate override, left alone); _shape writes both. Both are
 * read-modify-writes that keep the neighbouring bits (the meter's flow
 * control bit among them). The caller holds odi_switch_lock.
 */
#define ODI_SW_UNI_RATE_NONE		0x7ffffU
#define ODI_SW_UNI_RATE_UNIT_KBPS	8U
uint32_t odi_sw_uni_police_get(uint32_t port);
void odi_sw_uni_police_set(uint32_t port, uint32_t units);
uint32_t odi_sw_uni_shape_get(uint32_t port);
void odi_sw_uni_shape_set(uint32_t port, uint32_t units);

/* --- odi_switch_qos.c ----------------------------------------------------- */

/* cmd 23, a downstream queue: one write of PORT_QUEUE_MAP. */
void odi_sw_ponmac_queue_add(uint32_t th);

/* cmd 23, a downstream queue, the scheduler half: queue q of a UNI port is
 * weighted at weight (OMCI units) or strict; the port's priority to queue
 * map is identity. odi_sw_ds_sched (module parameter ds_sched, default on)
 * is the switch for it. */
extern bool odi_sw_ds_sched;
/* Which downstream queue a frame takes: its internal priority, which a
 * downstream CF row can assign. odi_sw_ds_steer (module parameter
 * ds_steer, default on) lets the rows of a bridge connection carry the
 * priority of the queue its GEM port's flow was given (cmd 25, ds_pq_pri). */
extern bool odi_sw_ds_steer;
void odi_sw_ds_queue_sched(uint32_t port, uint32_t q, int wfq, uint32_t weight);
void odi_sw_ds_prio_map_identity(uint32_t port);
#define ODI_SW_DSQ_QUEUES		8U

/* The upstream PON queues. The chip has 65: T-CONTs (PON schedulers) 0-7
 * share queues 0-31, T-CONTs 8-15 share 32-63, and the OMCC T-CONT 16 has
 * queue 64. A T-CONT's queue mask (PON_SCH_QMAP) holds logical queues, the
 * queue's number within its group of 32, so physical = 32 * (t / 8) +
 * logical.
 */
#define ODI_SW_PONQ_TCONTS		16U	/* data T-CONTs, 0-15 */
#define ODI_SW_PONQ_GROUP_TCONTS	8U
#define ODI_SW_PONQ_GROUP_QUEUES	32U
#define ODI_SW_PONQ_QUEUE_MASK		0x7fU	/* a queue number, 7 bits */
#define ODI_SW_PONQ_WEIGHT_MAX		0x3ffU	/* PON_WFQ_WEIGHT, 10 bits */
#define ODI_SW_PONQ_RATE_MAX		0x3ffffU	/* PON_QID_CIR/PIR_RATE, 18 bits */

/* cmd 23, an upstream queue: logical queue lq (physical pq) joins T-CONT
 * t. The T-CONT is enabled with its first queue; a queue not yet in the
 * mask is drained, then added; then its type (wfq: weighted fair, else
 * strict) and weight (1..ODI_SW_PONQ_WEIGHT_MAX; 1 for a strict queue),
 * and its CIR and PIR (ODI_SW_PONQ_RATE_MAX: no limit).
 */
void odi_sw_ponq_queue_add(uint32_t t, uint32_t lq, uint32_t pq, int wfq,
			   uint32_t weight, uint32_t cir, uint32_t pir);

/* cmd 24: the reverse. Nothing when the queue is not in the mask; the
 * T-CONT is disabled with its last queue. The caller takes the flows of
 * the queue out of PON_SIDVALID first.
 */
void odi_sw_ponq_queue_del(uint32_t t, uint32_t lq, uint32_t pq);

/* cmd 25, upstream side: the flow's GEM port (US_GEM_PORT_MAP), its valid
 * bit (PON_SIDVALID), its queue (PON_SID2QID), and the global PBO
 * thresholds for nflows upstream data flows (PON_SID_GLB_TH, the ON then
 * the OFF field).
 */
void odi_sw_ponq_us_gem_set(uint32_t slot, uint32_t gem_port_id);
void odi_sw_ponq_sidvalid_set(uint32_t sid, int valid);
void odi_sw_ponq_sid2qid_set(uint32_t sid, uint32_t pq);
void odi_sw_ponq_glb_th_set(uint32_t nflows);

/* --- odi_switch_ds_gem.c -------------------------------------------------- */

/* The DS slots: every downstream flow cmd 25 can create, rows 0..63 of
 * the GEM CAM and DSF_GEM_FLOW_TYPE (ODI_SW_CMD_GEM_DS_MAX). The CAM has
 * 128 rows; the GPON core keeps row 64 for the OMCC port.
 */
#define ODI_SWITCH_DS_SLOT_COUNT 64U

/* cmd 25, downstream side (despite the name): the DS GEM port CAM row and
 * TRAFFIC_CFG of slot idx (odi_switch_gpon_ds_port_write()), the slot
 * record, and the AES bit when an Encrypted_Port-ID for the port came
 * before it. traffic_cfg: FLAGS, 3 for the OMCI/broadcast GEM port, 2 for
 * data. Returns 0, or -ETIMEDOUT when the CAM write did not complete; no
 * slot is recorded and no AES bit set then. A slot past
 * ODI_SWITCH_DS_SLOT_COUNT is written but not recorded, and logged as an
 * error: AES could never be set on it.
 */
int odi_sw_gpon_usflow_set(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg);

/* cmd 25 downstream with ena 0: slot idx deleted, its CAM row cleared
 * and read back clear, its flow type word 0 (AES bit included), and its
 * slot record dropped (__odi_switch_gpon_ds_port_clear()). Returns 0, or
 * -ETIMEDOUT / -EIO when the row did not read back clear; the record is
 * kept then, and the failure logged.
 */
int odi_sw_gpon_ds_flow_del(uint32_t idx);

/* Which DS slot holds which GEM port: recorded when cmd 25 creates the
 * port, looked up when an Encrypted_Port-ID PLOAM names it. _reset, which
 * also forgets every Encrypted_Port-ID, is for the host tests, which run
 * several scenarios in one process.
 */
void odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id);
int odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out);
void odi_switch_ds_slot_reset(void);

/* Sets (enable 1) or clears the AES bit of the DS GEM port an
 * Encrypted_Port-ID PLOAM names; called by the GPON core (odi_gpon_hw.c).
 * The flag is kept per GEM port either way, and cmd 25 applies it to a
 * slot it creates for that port later. Returns 0, or -1 when no DS slot is
 * recorded for gem_port_id yet.
 */
int odi_switch_gpon_encrypt_port(uint16_t gem_port_id, int enable);

/* Forgets every flag Encrypted_Port-ID left: called by the GPON core when
 * it discards the ONU-ID (odi_gpon_hw.c), since the OLT configures the
 * ports again for the next activation. The AES bits already set on
 * existing slots stay.
 */
void odi_switch_ds_aes_forget(void);

/* The GEM ports the downstream path decrypts (at most `max`): each DS slot
 * with the AES bit set in hardware, and each port the OLT asked to decrypt
 * that has no slot yet. Returns the count; for /proc/odi_gpon/status.
 */
unsigned int odi_switch_gpon_aes_ports(uint16_t *out, unsigned int max);

/* --- odi_switch_cf.c ------------------------------------------------------ */

/* A CF (classification) row is three tables at one index: CLS_RULE_B and
 * CLS_MASK_B hold the match, CLS_US_ACTION or CLS_DS_ACTION the
 * treatment, chosen by the direction bit of the match. Word 0 is the most
 * significant word. Row 255 is the downstream catch-all of the module-load
 * replay. docs/SWITCH.md#cf-rows has the full field layout and how it was
 * decoded; the fields in use:
 *
 * The match is a data/care pair: RULE = data AND care, MASK = care AND NOT
 * data, so a bit set in either word is cared about. Bits over the 64-bit
 * pair (word 1 holds 31..0):
 *   48 VALID (RULE only), 31 direction (1 downstream), 22..11 outer VID,
 *   10..8 outer priority, 4 has an S-tag, 3 has a C-tag, 2..0 source port.
 * mask_w0 bit 16 is not a match field: the stock driver writes it 0 on the
 * row an insert adds and 1 on every other row write, and so does ours.
 *
 * Action, 67 bits over three words (word 1 is 63..32, word 2 is 31..0):
 *   55..53 / 52..50 C-tag priority / VID source (1: the values below),
 *   49..47 C-tag priority, 46..35 C-tag VID, 34..33 C-tag action,
 *   2..0 S-tag action, 17..15 / 14..3 S-tag priority / VID.
 *   Upstream: 30..24 the upstream flow id, 23 take it, 22..21 / 20..18
 *   S-tag VID / priority source. Downstream: 32..31 UNI action (3: forward
 *   to the ports in 30..27), 30..27 egress port mask, 23..21 / 20..18
 *   S-tag VID / priority source.
 *
 * A VLAN table row (one word, by VID): 3..0 member ports, 7..4 the members
 * that send untagged, 8 FID/MSTI, 9 S-VLAN IVL/SVL, 10 IVL/SVL, 17..11
 * extension port mask. The stock default row words (for example 0x0003f8ff)
 * are not decoded beyond the member and untag nibbles our code uses; a later
 * chip of the family uses a different, wider layout.
 */
struct odi_sw_cf_entry {
	uint32_t idx;
	int is_us;		/* selects CLS_US_ACTION vs CLS_DS_ACTION */
	uint32_t rule_w0, rule_w1;
	uint32_t mask_w0, mask_w1;
	uint32_t action_w0, action_w1, action_w2;
	int tmpl;		/* 1: the row compares the S-tag (template 1); last,
				 * so a positional initializer of the old fields
				 * still reads as it did */
};

struct odi_sw_vlan_override {
	uint32_t idx;		/* 2..4094 */
	uint32_t val;
};

/* CF match fields, positions in rule/mask word 1 (w0 holds bits 63..32). */
#define ODI_SW_CF_W0_VALID		(1U << 16)
/* Rule and mask word 0, bits 15:0, in a row of the second layout (rows from
 * ODI_SW_CLASSIFY_SETUP_PATTERN1_FIRST_ROW() up, template 0): the frame's
 * EtherType, as the bits that must be 1 (rule) and 0 (mask). The first layout
 * holds other keys there, so a row never writes them unless it names an
 * EtherType. */
#define ODI_SW_CF_W0_ET_MASK		0xffffU
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
/* Downstream rows: the frame's internal priority, which picks its egress
 * queue on the UNI port. Bits 59..57 hold it and bit 56 makes the row
 * assign it; with bit 56 clear the frame keeps the port default (0). The
 * per-row action class that gates it (CLS_ACT_EN bits 4:0) reads 0x3f. */
#define ODI_SW_CF_A1_PRI_EN		(1U << 24)
#define ODI_SW_CF_A1_PRI_SHIFT		25
#define ODI_SW_CF_A1_PRI_MASK		(0x7U << ODI_SW_CF_A1_PRI_SHIFT)
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
#define ODI_SW_CF_CSACT_ADD		1U	/* TPID 0x88a8; replaces the S-tag of a frame that has one */
#define ODI_SW_CF_CSACT_DEL		3U
#define ODI_SW_CF_CSACT_TRANSPARENT	4U
#define ODI_SW_CF_DS_UNI_ACT_FWD	3U	/* forward to UNI_PMSK */
#define ODI_SW_CF_TAG_SRC_ASSIGN	1U	/* the *_VID_ACT / *_PRI_ACT value */

/* The last VLAN row the second pass of cmd 51 rewrites; 4095 keeps the
 * first-pass sentinel. */
#define ODI_SW_VLAN_ID_LAST_SWEPT	4094U

/* VLAN row fields. */
#define ODI_SW_VLAN_ROW_MBR_MASK	0xfU
#define ODI_SW_VLAN_ROW_UNTAG_SHIFT	4
#define ODI_SW_VLAN_ROW(mbr, untag) \
	(((uint32_t)(mbr) & 0xfU) | (((uint32_t)(untag) & 0xfU) << ODI_SW_VLAN_ROW_UNTAG_SHIFT))

/* The whole sequence of one cmd 51: the CF rows in the order given, the
 * VLAN table in two passes and the fixed register template around them.
 * vlan_over lists the rows 2..4094 of the second VLAN pass that differ
 * from vlan_default, which every other row of that pass gets (0 unless a
 * service passes any VID, odi_switch_bdgconn.c).
 */
void odi_sw_cf_add(const struct odi_sw_cf_entry *cf, unsigned int n_cf,
		   const struct odi_sw_vlan_override *vlan_over, unsigned int n_over,
		   uint32_t vlan_default);

/* Invalidates CF row idx: an all-zero rule row and action row. No capture
 * shows a delete; this is the encoding the layout implies.
 */
void odi_sw_cf_del(uint32_t idx, int is_us);

/* The ACL rows of the extended VLAN discards (cmd 51, rule_gen
 * OMCI_VLAN_OPER_EXTVLAN_DROP): entries ODI_SW_ACL_DROP_BASE up, template
 * ODI_SW_ACL_DROP_TEMPLATE (slot 0 the C-tag: VID in bits 11:0, priority in
 * 15:13, 0 for a frame without one). The snooping copy owns entry 8 and
 * template 6, the module-load replay entry 1 and template 3. */
#define ODI_SW_ACL_DROP_BASE		16U
#define ODI_SW_ACL_DROP_MAX		32U
#define ODI_SW_ACL_DROP_TEMPLATE	5U

/* Discard the C-tagged frames of VID `vid` (and priority `pri`, -1 for any)
 * that enter by a port of `ports` (a bit a port). Turns the ACL lookup and
 * the default permit on for those ports first, never off. Returns 0 or the
 * table engine's error, with the entry left invalid. */
int odi_sw_acl_drop_set(uint32_t entry, uint32_t ports, int vid, int pri);
/* The rule stops matching, then goes. */
void odi_sw_acl_drop_del(uint32_t entry);
/* The three action words of one CF row written alone, the rule and the mask
 * left as they are (a downstream row's priority changed under it). */
void odi_sw_cf_act_write(uint32_t idx, int is_us, const uint32_t act[3]);

/* Forget which rows were last written on the S-tag template: the rows
 * are gone, and so are the pattern bits that said so. */
void odi_sw_cf_reset(void);

/* The first row that is matched with the second rule layout, from
 * CLASSIFY_SETUP: 256 minus the count in bits 12:5, the count being rows
 * from the top (256 when the register names none). A row past it is not
 * one the S-tag template is known to work on. */
uint32_t odi_sw_cf_layout1_row(void);

/* One VLAN table row (a service leaving: its VID row back to 0). */
void odi_sw_vlan_row_set(uint32_t vid, uint32_t val);

/* --- odi_switch_platform.c ------------------------------------------------ */

/* The platform settings, after the odi_init "switch" verb; rcS runs them
 * once through /proc/odi_omci, before omcid starts.
 */
void odi_switch_init_platform(void);

struct odi_replay_blob;	/* odi_replay_blob.h */

/* Replays every record of modload.bin, which has the production selection
 * applied to it (docs/SWITCH.md#module-load-replay).
 */
void odi_switch_init_modload(const struct odi_replay_blob *table);

/* --- odi_switch_mib.c ----------------------------------------------------- */

/* A switch-core word at addr, refused with -EINVAL when addr is out of
 * the mapped window (odi_switch_mmio_offset_in_bounds()) or unaligned.
 */
int odi_sw_reg_get(uint32_t addr, uint32_t *value);
int odi_sw_reg_set(uint32_t addr, uint32_t value);

/* Counter `counter` (the index of src/diag/src/mib.h mib_names[]) of a
 * port: 0 and *value, or -1 for an unmapped counter or a port >= 4.
 */
int odi_sw_mib_get(uint32_t port, uint32_t counter, uint64_t *value);

#endif /* ODI_SWITCH_DAL_H */
