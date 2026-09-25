/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_gpon_ploam_test.c -- "open stack by default":
 * odi_switch_gpon_ploam_hook(), the automatic AES-enable path. Covers the
 * Encrypted_Port-ID decode (gem_port_id = (data[1]<<4)|(data[2]>>4), aes =
 * data[0]&0x01), the gem_port_id -> DS slot lookup odi_switch_ds_slot_
 * record()/_find() provide, the set-and-clear RMW itself, the
 * unknown-gem_port_id no-op case, and that every message type other than
 * Encrypted_Port-ID is left alone. Host-only: odi_switch_gpon_
 * ploam_register() itself (the call into the stock GPON module) is a stub
 * outside __KERNEL__ and not exercised here.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* Builds an Encrypted_Port-ID PLOAM exactly as the OLT would (module/gpon/
 * gpon_ploam.c's own decode, mirrored in reverse): gem_port_id in data[1]
 * (high 8 bits) and the top nibble of data[2] (low 4 bits), aes in bit 0
 * of data[0].
 */
static void build_encryptport(struct odi_sw_gpon_ploam *p, uint32_t onu_id,
			       uint32_t gem_port_id, uint32_t ae_en)
{
	memset(p, 0, sizeof *p);
	p->onu_id = (uint8_t)onu_id;
	p->msg_id = ODI_SW_GPON_PLOAM_DS_ENCRYPTPORT;
	p->data[0] = (uint8_t)(ae_en & 0x01U);
	p->data[1] = (uint8_t)((gem_port_id >> 4) & 0xffU);
	p->data[2] = (uint8_t)((gem_port_id & 0x0fU) << 4);
}

static void test_known_port_enable(void)
{
	struct odi_sw_gpon_ploam p;
	uint32_t reg;
	int rc;

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(2), 0x00000002U); /* is_eth, AES clear */
	odi_switch_ds_slot_record(2, 0x69a);

	build_encryptport(&p, 1, 0x69a, 1);
	rc = odi_switch_gpon_ploam_hook(&p);

	CHECK(rc == ODI_SW_GPON_PLOAM_CONTINUE, "hook always returns CONTINUE (observe, do not suppress)");
	reg = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(2));
	CHECK(reg == 0x00000012U, "known gem_port_id, aes=1: slot's AES bit set, 0x02 -> 0x12");
}

static void test_known_port_disable(void)
{
	struct odi_sw_gpon_ploam p;
	uint32_t reg;

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(3), 0x00000012U); /* is_eth, AES already set */
	odi_switch_ds_slot_record(3, 0x71a);

	build_encryptport(&p, 1, 0x71a, 0);
	odi_switch_gpon_ploam_hook(&p);

	reg = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(3));
	CHECK(reg == 0x00000002U,
	      "known gem_port_id, aes=0: slot's AES bit cleared -- the manual ds_encrypt trigger cannot do this, the hook can");
}

static void test_unknown_port_is_noop(void)
{
	struct odi_sw_gpon_ploam p;

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_switch_ds_slot_record(2, 0x69a); /* some other, unrelated slot recorded */

	build_encryptport(&p, 1, 0x9999 & 0xfff, 1); /* never recorded */
	odi_switch_gpon_ploam_hook(&p);

	CHECK(odi_mock.log_n == 0, "gem_port_id with no recorded DS slot: no register write at all");
}

static void test_other_message_types_ignored(void)
{
	struct odi_sw_gpon_ploam p;
	unsigned int t;

	odi_switch_ds_slot_reset();
	odi_switch_ds_slot_record(2, 0x69a);

	for (t = 0; t <= 0x0c; t++) {
		if (t == ODI_SW_GPON_PLOAM_DS_ENCRYPTPORT)
			continue;
		odi_mock_reset();
		memset(&p, 0, sizeof p);
		p.msg_id = (uint8_t)t;
		p.data[1] = 0x69; p.data[2] = 0xa0; /* would decode to gem_port_id 0x69a if misparsed */
		CHECK(odi_switch_gpon_ploam_hook(&p) == ODI_SW_GPON_PLOAM_CONTINUE,
		      "non-ENCRYPTPORT message: still returns CONTINUE");
		CHECK(odi_mock.log_n == 0, "non-ENCRYPTPORT message: no register write");
	}
}

static void test_gem_port_id_decode(void)
{
	struct odi_sw_gpon_ploam p;

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(5), 0x00000002U);
	odi_switch_ds_slot_record(5, 0xfff); /* 12-bit max, exercises both decode halves fully set */

	build_encryptport(&p, 1, 0xfff, 1);
	odi_switch_gpon_ploam_hook(&p);

	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(5)) == 0x00000012U,
	      "gem_port_id 0xfff (data[1]=0xff, data[2] top nibble=0xf) decodes and matches correctly");
}

int main(void)
{
	test_known_port_enable();
	test_known_port_disable();
	test_unknown_port_is_noop();
	test_other_message_types_ignored();
	test_gem_port_id_decode();
	CHECK(odi_mock_locks_idle(), "every switch lock released on return (odi_switch_mock.h)");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_gpon_ploam_test: ok\n");
	return 0;
}
