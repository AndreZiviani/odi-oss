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
#else
#include <stdint.h>
#include <stdio.h>
#define ODI_SW_LOG(fmt, ...) printf("odi_switch: " fmt, ##__VA_ARGS__)
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

/* cmd 32: PORT_FORCE_SELECT of a port. The admin state the command also
 * carries has no register write in any capture.
 */
uint32_t odi_sw_port_force_get(uint32_t port);
void odi_sw_port_force_set(uint32_t port);

/* cmd 64: FLOOD_UNKN_UCAST_PORTS, one bit per port. */
uint32_t odi_sw_l2_flood_mask_get(void);
void odi_sw_l2_flood_mask_set(uint32_t mask);

/* cmd 10: the I2C select of the transceiver status read, twice: the
 * device-select word, then GPIO 29 (gpio_lo, then gpio_hi) and GPIO 31.
 */
void odi_sw_ponmac_transceiver_get(uint32_t gpio_lo, uint32_t gpio_hi);

/* --- odi_switch_qos.c ----------------------------------------------------- */

/* cmd 23, a downstream queue: one write of PORT_QUEUE_MAP. */
void odi_sw_ponmac_queue_add(uint32_t th);

/* cmd 23, an upstream queue on T-CONT n: seven PONQ_COUNT_MASK writes,
 * the scheduler word of T-CONT n among them (odi_sw_qos_sched_set()) and
 * +207, the set of T-CONTs in use (bitmask_207). The order of +208 and
 * +212/+213 depends on n (T-CONT 0 writes +208 first); use_213 picks +213
 * over +212.
 */
void odi_sw_ponmac_queue_add_ext(uint32_t n, uint32_t bitmask_207, uint32_t base_15,
				  uint32_t sched_value, uint32_t val_208,
				  uint32_t bitmask_212_213, int use_213);

/* cmd 25, upstream side: US_GEM_PORT_MAP(slot), then word sidvalid_word of
 * PON_SIDVALID (+37), PON_SID_GLB_TH (+235) twice (the ON then the OFF
 * threshold, each written as the whole word), and word sid2qid_word of
 * PON_SID2QID (+20). The caller computes every word.
 */
void odi_sw_ponmac_flow_queue_set(uint32_t slot, uint32_t gem_port_id,
				   uint32_t sidvalid_word, uint32_t sidvalid,
				   uint32_t glb_th_on, uint32_t glb_th_off,
				   uint32_t sid2qid_word, uint32_t sid2qid);

/* The scheduler word of T-CONT n, PONQ_COUNT_MASK +190+n. */
void odi_sw_qos_sched_set(uint32_t n, uint32_t value);

/* --- odi_switch_ds_gem.c -------------------------------------------------- */

/* The DS slots, the index range of DSF_GEM_FLOW_TYPE. */
#define ODI_SWITCH_DS_SLOT_COUNT 32U

/* cmd 25, downstream side (despite the name): the DS GEM port CAM row and
 * TRAFFIC_CFG of slot idx (odi_switch_gpon_ds_port_write()), and the slot
 * record. traffic_cfg: FLAGS, 3 for the OMCI/broadcast GEM port, 2 for data.
 */
void odi_sw_gpon_usflow_set(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg);

/* Which DS slot holds which GEM port: recorded when cmd 25 creates the
 * port, looked up when an Encrypted_Port-ID PLOAM names it. _reset is for
 * the host tests, which run several scenarios in one process.
 */
void odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id);
int odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out);
void odi_switch_ds_slot_reset(void);

/* Sets (enable 1) or clears the AES bit of the DS GEM port an
 * Encrypted_Port-ID PLOAM names; called by the GPON core (odi_gpon_hw.c).
 * Returns 0, or -1 when no DS slot is recorded for gem_port_id yet.
 */
int odi_switch_gpon_encrypt_port(uint16_t gem_port_id, int enable);

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
