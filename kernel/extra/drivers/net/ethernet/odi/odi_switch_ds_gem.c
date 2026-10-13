// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_ds_gem.c -- downstream GEM ports: the DS GEM port write of
 * cmd 25 (downstream side), the record of which DS slot holds which GEM
 * port, and the AES bit the Encrypted_Port-ID PLOAM sets on a slot
 * (docs/SWITCH.md#downstream-aes).
 *
 * The slot record is written from process context (cmd 25) and read from
 * the GPON interrupt path; odi_switch_dsf_lock (odi_switch.c) covers it
 * together with the CAM row and the DSF_GEM_FLOW_TYPE word, so the
 * interrupt path never finds a slot whose row is half written. The __
 * variants are for a caller that already holds the lock. Nothing prints
 * under it: the AES path hands FLAGS back and logs after the unlock.
 */
#include "odi_switch_dal.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static uint32_t odi_sw_ds_slot_gem_port_id[ODI_SWITCH_DS_SLOT_COUNT];
static uint8_t odi_sw_ds_slot_valid[ODI_SWITCH_DS_SLOT_COUNT];

/* What the OLT last said about the encryption of each GEM port, by
 * Encrypted_Port-ID (G.984.3 clause 9.2.3.10): known, and on or off, one
 * bit each per 12-bit Port-ID (two 512-byte bitmaps). The PLOAM can come
 * before cmd 25 creates the port (one OLT sends it right after O5, before
 * any OMCI), and a flow cmd 25 creates again after an OMCI MIB reset gets
 * its FLAGS rewritten without the AES bit, with no new PLOAM to set it:
 * either way the bit is applied when the slot is written. Forgotten when
 * the ONU-ID is (odi_switch_ds_aes_forget()): the next activation
 * configures the ports again.
 */
#define ODI_SW_GEM_PORT_IDS	4096U
static uint32_t odi_sw_ds_aes_known[ODI_SW_GEM_PORT_IDS / 32U];
static uint32_t odi_sw_ds_aes_on[ODI_SW_GEM_PORT_IDS / 32U];

static void __odi_switch_ds_aes_note(uint32_t gem_port_id, uint32_t enable)
{
	uint32_t w = (gem_port_id % ODI_SW_GEM_PORT_IDS) / 32U;
	uint32_t bit = 1U << (gem_port_id % 32U);

	lockdep_assert_held(&odi_switch_dsf_lock);
	odi_sw_ds_aes_known[w] |= bit;
	if (enable)
		odi_sw_ds_aes_on[w] |= bit;
	else
		odi_sw_ds_aes_on[w] &= ~bit;
}

/* 1 and *enable when the OLT has said something about gem_port_id. */
static int __odi_switch_ds_aes_lookup(uint32_t gem_port_id, uint32_t *enable)
{
	uint32_t w = (gem_port_id % ODI_SW_GEM_PORT_IDS) / 32U;
	uint32_t bit = 1U << (gem_port_id % 32U);

	lockdep_assert_held(&odi_switch_dsf_lock);
	if (!(odi_sw_ds_aes_known[w] & bit))
		return 0;
	*enable = (odi_sw_ds_aes_on[w] & bit) ? 1U : 0U;
	return 1;
}

/* 0, or -1 for a slot the table does not cover: no AES could ever reach
 * it, so the caller says so rather than leaving it silently clear.
 */
static int __odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id)
{
	lockdep_assert_held(&odi_switch_dsf_lock);
	if (idx >= ODI_SWITCH_DS_SLOT_COUNT)
		return -1;
	odi_sw_ds_slot_gem_port_id[idx] = gem_port_id;
	odi_sw_ds_slot_valid[idx] = 1U;
	return 0;
}

static int __odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out)
{
	uint32_t idx;

	lockdep_assert_held(&odi_switch_dsf_lock);
	for (idx = 0; idx < ODI_SWITCH_DS_SLOT_COUNT; idx++) {
		if (!odi_sw_ds_slot_valid[idx])
			continue;
		if (odi_sw_ds_slot_gem_port_id[idx] == gem_port_id) {
			if (idx_out)
				*idx_out = idx;
			return 1;
		}
	}
	return 0;
}

static void __odi_switch_ds_encrypt_one(uint32_t idx, uint32_t enable,
					uint32_t *before, uint32_t *after);
static void odi_switch_ds_encrypt_log(uint32_t idx, uint32_t before, uint32_t after);

/* The CAM row, its DSF_GEM_FLOW_TYPE word, the slot record and the AES
 * bit the OLT already asked for, in one critical section. The return
 * code of the CAM write is not checked, as in the capture the command
 * has no failure path. Logged after the unlock.
 */
void odi_sw_gpon_usflow_set(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg)
{
	unsigned long flags;
	uint32_t enable = 0, before = 0, after = 0;
	int recorded, aes = 0;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	(void)__odi_switch_gpon_ds_port_write(idx, gem_port_id, traffic_cfg);
	recorded = __odi_switch_ds_slot_record(idx, gem_port_id) == 0;
	if (recorded && __odi_switch_ds_aes_lookup(gem_port_id, &enable) && enable) {
		__odi_switch_ds_encrypt_one(idx, 1U, &before, &after);
		aes = 1;
	}
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);

	if (!recorded)
		ODI_SW_ERR("GEM port 0x%03x DS slot %u is past the %u the table holds: AES never set on it\n",
			   gem_port_id, idx, ODI_SWITCH_DS_SLOT_COUNT);
	if (aes) {
		ODI_SW_LOG("gpon encrypt_port: gem_port_id 0x%03x -> DS slot %u, aes=1 (Encrypted_Port-ID before the flow)\n",
			   gem_port_id, idx);
		odi_switch_ds_encrypt_log(idx, before, after);
	}
}

void odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	(void)__odi_switch_ds_slot_record(idx, gem_port_id);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
}

int odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out)
{
	unsigned long flags;
	int found;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	found = __odi_switch_ds_slot_find(gem_port_id, idx_out);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	return found;
}

void odi_switch_ds_slot_reset(void)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	memset(odi_sw_ds_slot_valid, 0, sizeof(odi_sw_ds_slot_valid));
	memset(odi_sw_ds_aes_known, 0, sizeof(odi_sw_ds_aes_known));
	memset(odi_sw_ds_aes_on, 0, sizeof(odi_sw_ds_aes_on));
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
}

void odi_switch_ds_aes_forget(void)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	memset(odi_sw_ds_aes_known, 0, sizeof(odi_sw_ds_aes_known));
	memset(odi_sw_ds_aes_on, 0, sizeof(odi_sw_ds_aes_on));
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
}

/* Bit 4 (AES) of DSF_GEM_FLOW_TYPE(idx), read-modify-write, with FLAGS
 * before and after returned to the caller.
 */
static void __odi_switch_ds_encrypt_one(uint32_t idx, uint32_t enable,
					uint32_t *before, uint32_t *after)
{
	uint32_t reg;

	lockdep_assert_held(&odi_switch_dsf_lock);
	reg = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(idx));
	*before = ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_GET(reg);
	reg = ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_DECRYPT_SET(reg, enable ? 1U : 0U);
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(idx), reg);
	reg = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(idx));
	*after = ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_GET(reg);
}

static void odi_switch_ds_encrypt_log(uint32_t idx, uint32_t before, uint32_t after)
{
	ODI_SW_LOG("aes: GEM port slot %u, FLAGS 0x%02x -> 0x%02x\n", idx, before, after);
}

/* The flag noted, the slot lookup and the AES-bit RMW as one step, so
 * cmd 25 cannot create or move the slot in between. Returns 1 and the
 * slot with FLAGS before and after when gem_port_id has a slot, 0
 * otherwise (the flag is then applied when cmd 25 creates the slot).
 */
static int odi_switch_ds_encrypt_gem_port(uint32_t gem_port_id, uint32_t enable,
					  uint32_t *idx, uint32_t *before, uint32_t *after)
{
	unsigned long flags;
	int found;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	__odi_switch_ds_aes_note(gem_port_id, enable);
	found = __odi_switch_ds_slot_find(gem_port_id, idx);
	if (found)
		__odi_switch_ds_encrypt_one(*idx, enable, before, after);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	return found;
}

int odi_switch_gpon_encrypt_port(uint16_t gem_port_id, int enable)
{
	uint32_t idx, before, after;

	if (!odi_switch_ds_encrypt_gem_port((uint32_t)gem_port_id, enable ? 1U : 0U,
					    &idx, &before, &after)) {
		ODI_SW_LOG("gpon encrypt_port: gem_port_id 0x%03x, no DS slot recorded for it yet -- aes=%u kept for when it is created\n",
			   gem_port_id, enable ? 1U : 0U);
		return -1;
	}

	ODI_SW_LOG("gpon encrypt_port: gem_port_id 0x%03x -> DS slot %u, aes=%u\n", gem_port_id, idx, enable ? 1U : 0U);
	odi_switch_ds_encrypt_log(idx, before, after);
	return 0;
}
