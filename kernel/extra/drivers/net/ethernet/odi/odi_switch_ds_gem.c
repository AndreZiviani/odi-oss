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

static uint32_t odi_sw_ds_slot_gem_port_id[ODI_SWITCH_DS_SLOT_COUNT];
static uint32_t odi_sw_ds_slot_valid_mask;

static void __odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id)
{
	lockdep_assert_held(&odi_switch_dsf_lock);
	if (idx >= ODI_SWITCH_DS_SLOT_COUNT)
		return;
	odi_sw_ds_slot_gem_port_id[idx] = gem_port_id;
	odi_sw_ds_slot_valid_mask |= (1U << idx);
}

static int __odi_switch_ds_slot_find(uint32_t gem_port_id, uint32_t *idx_out)
{
	uint32_t idx;

	lockdep_assert_held(&odi_switch_dsf_lock);
	for (idx = 0; idx < ODI_SWITCH_DS_SLOT_COUNT; idx++) {
		if (!(odi_sw_ds_slot_valid_mask & (1U << idx)))
			continue;
		if (odi_sw_ds_slot_gem_port_id[idx] == gem_port_id) {
			if (idx_out)
				*idx_out = idx;
			return 1;
		}
	}
	return 0;
}

/* The CAM row, its DSF_GEM_FLOW_TYPE word and the slot record, in one
 * critical section. The return code of the CAM write is not checked, as
 * in the capture the command has no failure path.
 */
void odi_sw_gpon_usflow_set(uint32_t idx, uint32_t gem_port_id, uint32_t traffic_cfg)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	(void)__odi_switch_gpon_ds_port_write(idx, gem_port_id, traffic_cfg);
	__odi_switch_ds_slot_record(idx, gem_port_id);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
}

void odi_switch_ds_slot_record(uint32_t idx, uint32_t gem_port_id)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	__odi_switch_ds_slot_record(idx, gem_port_id);
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
	odi_sw_ds_slot_valid_mask = 0;
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

/* Slot lookup and the AES-bit RMW as one step, so cmd 25 cannot move the
 * slot between the two. Returns 1 and the slot with FLAGS before and
 * after when gem_port_id has a slot, 0 otherwise.
 */
static int odi_switch_ds_encrypt_gem_port(uint32_t gem_port_id, uint32_t enable,
					  uint32_t *idx, uint32_t *before, uint32_t *after)
{
	unsigned long flags;
	int found;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
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
		ODI_SW_LOG("gpon encrypt_port: gem_port_id 0x%03x, no DS slot recorded for it yet -- ignored\n",
			   gem_port_id);
		return -1;
	}

	ODI_SW_LOG("gpon encrypt_port: gem_port_id 0x%03x -> DS slot %u, aes=%u\n", gem_port_id, idx, enable ? 1U : 0U);
	odi_switch_ds_encrypt_log(idx, before, after);
	return 0;
}
