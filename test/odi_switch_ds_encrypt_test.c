/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_ds_encrypt_test.c -- odi_switch_gpon_encrypt_port(), the one
 * path that sets the AES bit of a downstream GEM port: the GPON core calls
 * it on the Encrypted_Port-ID PLOAM of the OLT. Covers the gem_port_id to
 * DS slot lookup (odi_switch_ds_slot_record()/_find()), the set and the
 * clear, that the read-modify-write leaves every other FLAGS bit alone,
 * that a GEM port with no slot yet is left alone at the PLOAM and gets the
 * bit when cmd 25 creates its slot, and that slots 32-63 (the flows cmd 25
 * allows past the old 32-slot table) are covered.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static unsigned int count_writes(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

/* The ISP1 data GEM ports, slots 1-5: FLAGS 0x02 (Ethernet) on ours before
 * the PLOAM, 0x12 (plus AES) on the stock image after it.
 */
static void test_capture_transition(void)
{
	static const uint16_t gem[6] = { 0xfff, 0x59a, 0x69a, 0x71a, 0x79a, 0x61a };
	unsigned int idx;

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	for (idx = 0; idx < 6; idx++) {
		odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(idx), idx ? 0x00000002U : 0x00000003U);
		odi_switch_ds_slot_record(idx, gem[idx]);
	}
	for (idx = 1; idx < 6; idx++)
		CHECK(odi_switch_gpon_encrypt_port(gem[idx], 1) == 0, "a recorded data GEM port is found");
	for (idx = 1; idx < 6; idx++)
		CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(idx)) == 0x00000012U,
		      "slots 1-5: 0x02 becomes 0x12");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(0)) == 1,
	      "slot 0 (OMCI/broadcast) only has the seed write");
}

static void test_clear(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(3), 0x00000012U);
	odi_switch_ds_slot_record(3, 0x71a);
	CHECK(odi_switch_gpon_encrypt_port(0x71a, 0) == 0, "clear: the port is found");
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(3)) == 0x00000002U, "aes 0 clears the bit");
}

/* A field-level read-modify-write: multicast, OMCI and bit 3 survive. */
static void test_rmw_preserves_other_bits(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(2), 0x0000000dU);
	odi_switch_ds_slot_record(2, 0x69a);
	(void)odi_switch_gpon_encrypt_port(0x69a, 1);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0x0000001dU,
	      "only bit 4 changes (0x0d -> 0x1d)");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 2, "one write besides the seed");
}

/* The PLOAM can arrive before cmd 25 created the port: nothing to write. */
static void test_unknown_port(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_switch_ds_slot_record(1, 0x59a);
	odi_mock.log_n = 0;
	CHECK(odi_switch_gpon_encrypt_port(0x123, 1) == -1, "an unknown GEM port returns -1");
	CHECK(odi_mock.log_n == 0, "and writes nothing");
}

/* Encrypted_Port-ID right after O5, before OMCI created the flow (one
 * OLT does exactly that): the bit lands when cmd 25 writes the slot.
 */
static void test_encrypt_before_flow(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	CHECK(odi_switch_gpon_encrypt_port(0x59a, 1) == -1, "no slot yet at the PLOAM");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0, "and nothing written then");
	odi_sw_gpon_usflow_set(1, 0x59a, 2);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0x00000012U,
	      "cmd 25 creates the flow with AES on: 0x12, not 0x02");

	/* a=0 kept the same way: the flow stays clear. */
	CHECK(odi_switch_gpon_encrypt_port(0x69a, 0) == -1, "a clear for a port with no slot");
	odi_sw_gpon_usflow_set(2, 0x69a, 2);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0x00000002U, "is created clear");

	/* A port the OLT said nothing about is created as before. */
	odi_sw_gpon_usflow_set(3, 0x71a, 2);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(3)) == 0x00000002U, "an unnamed port is created clear");
}

/* cmd 25 writing an existing flow again (an OMCI MIB reset re-creates it)
 * rewrites FLAGS; the AES bit the OLT set comes back with it.
 */
static void test_flow_recreated(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_sw_gpon_usflow_set(4, 0x79a, 2);
	CHECK(odi_switch_gpon_encrypt_port(0x79a, 1) == 0, "the PLOAM finds the slot");
	odi_sw_gpon_usflow_set(4, 0x79a, 2);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(4)) == 0x00000012U,
	      "the flow written again keeps AES on");
}

/* The flags belong to one activation: forgotten with the ONU-ID. */
static void test_forget(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	(void)odi_switch_gpon_encrypt_port(0x59a, 1);
	odi_switch_ds_aes_forget();
	odi_sw_gpon_usflow_set(1, 0x59a, 2);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0x00000002U,
	      "a flag forgotten with the ONU-ID is not applied");
}

/* Through the GPON core: an Encrypted_Port-ID for a data port with no flow
 * yet is kept, and the ONU-ID discarded (Deactivate_ONU-ID, a fibre pull
 * past TO2, gpondeact) forgets it.
 */
static void test_gpon_release_forgets(void)
{
	const struct odi_gpon_fsm_ops *ops = odi_gpon_hw_ops();

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_gpon_hw_reset();
	ops->set_onu_id(NULL, 26U);
	(void)odi_gpon_hw_gem_port_encrypted(0x59a, 1);
	odi_sw_gpon_usflow_set(1, 0x59a, 2);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0x00000012U,
	      "kept from the GPON core's Encrypted_Port-ID");

	(void)odi_gpon_hw_gem_port_encrypted(0x69a, 1);
	ops->set_onu_id(NULL, ODI_GPON_ONU_ID_BROADCAST);
	odi_sw_gpon_usflow_set(2, 0x69a, 2);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0x00000002U,
	      "forgotten when the ONU-ID is discarded");
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0x00000012U,
	      "an existing slot keeps its bit");
}

/* cmd 25 accepts flows 0..63; the slot table covered 32. */
static void test_high_slots(void)
{
	uint32_t idx;

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_sw_gpon_usflow_set(40, 0x123, 2);
	odi_sw_gpon_usflow_set(63, 0x124, 2);
	CHECK(odi_switch_ds_slot_find(0x123, &idx) && idx == 40U, "slot 40 is recorded");
	CHECK(odi_switch_gpon_encrypt_port(0x123, 1) == 0 &&
	      odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(40)) == 0x00000012U, "and gets AES");
	CHECK(odi_switch_gpon_encrypt_port(0x124, 1) == 0 &&
	      odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(63)) == 0x00000012U, "so does slot 63, the last cmd 25 allows");
}

int main(void)
{
	test_capture_transition();
	test_clear();
	test_rmw_preserves_other_bits();
	test_unknown_port();
	test_encrypt_before_flow();
	test_flow_recreated();
	test_forget();
	test_high_slots();
	test_gpon_release_forgets();
	CHECK(odi_mock_locks_idle(), "every switch lock released on return (odi_switch_mock.h)");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_ds_encrypt_test: ok\n");
	return 0;
}
