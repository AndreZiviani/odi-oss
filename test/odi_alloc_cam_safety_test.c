/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_alloc_cam_safety_test.c -- the Alloc-ID CAM holds only the Alloc-IDs
 * the OLT assigned to this ONU, by PLOAM, in the current activation, through
 * every way an activation ends and whatever omcid sends.
 *
 * The CAM decides which upstream grants the ONU answers: a row left or put
 * back for an Alloc-ID that is not ours makes the stick transmit in another
 * ONU's slots on a shared PON. The rules checked here are G.984.3's
 * (itu/g984.3.txt), not this driver's:
 *
 *   - 5.5.4: a T-CONT is mapped to an Alloc-ID "which has been previously
 *     assigned to the given ONU via the PLOAM messaging channel". So driver
 *     command 21 (omcid binding a T-CONT ME to its Alloc-ID) never makes an
 *     Alloc-ID ours.
 *   - 5.5.3: Alloc-IDs are assigned by Assign_Alloc-ID type 1 and reversed
 *     by type 255; the default Alloc-ID (the ONU-ID) "cannot be
 *     de-allocated"; "All Alloc-ID assignments, including the default
 *     Alloc-ID assignment, shall be lost upon ONU deactivation."
 *   - 9.2.3.10: types 0, 2 and 3-254 assign nothing; for type 2 the ONU
 *     withholds the acknowledgement.
 *   - Table 5-1 and 9.2.3: a directed message carries the ONU-ID it is for;
 *     an ONU that holds no ONU-ID (O1, O2, O3, O7) has none to match.
 *
 * Scenario: a first activation (ONU-ID 26), a deallocation, the reserved
 * types, LOS into O6 and TO2 out of it, messages for ONU 0 (another ONU)
 * while this one has no ONU-ID, a second activation as ONU 0, a
 * Deactivate_ONU-ID, a Disable_Serial_Number (O7) and gpondeact. After each
 * step every Alloc-ID omcid had bound is sent through command 21 again, as
 * omcid's rebuild does when the PLOAM list changes, and the CAM, modelled
 * from its request handshake, must hold exactly what the OLT assigned in
 * the activation in progress.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"

#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_gemflow.h"

static int failures;

#define CHECK(cond, what) do {						\
	if (cond) {							\
		printf("ok    %s\n", what);				\
	} else {							\
		printf("FAIL  %s\n", what);				\
		failures++;						\
	}								\
} while (0)

static const uint8_t our_sn[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
static const uint8_t no_pw[10];

/* ---- The Alloc-ID CAM, modelled from its handshake (as odi_reregister_test.c).
 * A write (mode 1) takes the Alloc-ID from DSF_ALLOC_CAM_WDATA; a delete
 * (mode 3) clears the row.
 */
static struct {
	int valid;
	uint32_t alloc_id;
} cam[32];

static void cam_hook(uint32_t off, uint32_t val)
{
	unsigned int row = val & 0x1fU, op = (val >> 8) & 0x3U;

	if (off != ODI_SW_DSF_ALLOC_CAM_CTL_OFF || !(val & (1U << 15)))
		return;
	if (op == 1U) {
		cam[row].valid = 1;
		cam[row].alloc_id = odi_mock.regs[odi_mock_slot(ODI_SW_DSF_ALLOC_CAM_WDATA_OFF)] & 0xfffU;
	} else if (op == 3U) {
		cam[row].valid = 0;
		cam[row].alloc_id = 0;
	}
}

/* The CAM holds exactly these Alloc-IDs (any rows), and nothing else. */
static int cam_is(const uint16_t *ids, unsigned int n)
{
	unsigned int r, i, valid = 0;

	for (r = 0; r < 32U; r++) {
		int want = 0;

		if (!cam[r].valid)
			continue;
		valid++;
		for (i = 0; i < n; i++)
			want |= cam[r].alloc_id == ids[i];
		if (!want) {
			printf("      row %u holds Alloc-ID %u, which is not assigned\n", r,
			       (unsigned int)cam[r].alloc_id);
			return 0;
		}
	}
	if (valid != n)
		printf("      %u rows valid, %u expected\n", valid, n);
	return valid == n;
}

/* ---- Downstream PLOAM, three copies, one interrupt pass each ------------ */

static void poke(uint32_t off, uint32_t val)
{
	odi_mock.regs[odi_mock_slot(off)] = val;
}

static void ploam(struct odi_gpon_fsm *fsm, uint8_t onu_id, uint8_t type, const uint8_t content[10])
{
	struct odi_gpon_ploam msg;
	uint16_t words[6];
	unsigned int i, rep;

	msg.onu_id = onu_id;
	msg.type = type;
	memcpy(msg.content, content, sizeof(msg.content));
	odi_gpon_ploam_pack_words(&msg, words);
	for (rep = 0; rep < 3U; rep++) {
		poke(ODI_GPON_PONMAC_IRQ_PENDING_OFF, ODI_GPON_PONMAC_IRQ_PENDING_DS_FRAMER);
		poke(ODI_GPON_DSF_IRQ_EVENT_OFF, ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX);
		poke(ODI_GPON_DSF_PLOAM_RX_CTL_OFF, 0U);
		for (i = 0; i < 6U; i++)
			poke(ODI_GPON_DSF_PLOAM_RX_WORD(i), words[i]);
		if (odi_gpon_isr_poll(fsm, NULL) != 1U) {
			fprintf(stderr, "FAIL: PLOAM type 0x%02x was not drained\n", type);
			exit(1);
		}
	}
}

static void alloc_msg(struct odi_gpon_fsm *fsm, uint8_t onu_id, uint16_t alloc_id, uint8_t type)
{
	uint8_t c[10] = { 0 };

	c[0] = (uint8_t)(alloc_id >> 4);
	c[1] = (uint8_t)((alloc_id & 0xfU) << 4);
	c[2] = type;
	ploam(fsm, onu_id, ODI_GPON_DS_ASSIGN_ALLOC_ID, c);
}

/* gponact, Upstream_Overhead (O2 -> O3), and, if onu_id is not 0xff,
 * Assign_ONU-ID (-> O4) and Ranging_Time (-> O5).
 */
static void activate(struct odi_gpon_fsm *fsm, uint8_t onu_id)
{
	static const uint8_t overhead[10] = { 0x20, 0x00, 0x00, 0xaa, 0xab, 0x59, 0x83, 0x20, 0x00, 0x00 };
	static const uint8_t ranging[10] = { 0x00, 0x00, 0x03, 0x77, 0x64 };
	uint8_t onu[10] = { 0 };

	odi_gpon_verb_activate(fsm);
	ploam(fsm, ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_UPSTREAM_OVERHEAD, overhead);
	if (onu_id == ODI_GPON_ONU_ID_BROADCAST)
		return;
	onu[0] = onu_id;
	memcpy(&onu[1], our_sn, sizeof(our_sn));
	ploam(fsm, ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_ASSIGN_ONU_ID, onu);
	ploam(fsm, onu_id, ODI_GPON_DS_RANGING_TIME, ranging);
}

static void event(struct odi_gpon_fsm *fsm, enum odi_gpon_event ev)
{
	odi_gpon_fsm_handle_event(fsm, odi_gpon_hw_ops(), NULL, ev, NULL);
}

/* ---- Driver command 21, as omcid sends it ------------------------------- */

static int cmd21(uint16_t alloc_id)
{
	struct omci_tcont t;

	t.index = 1U;
	t.alloc_id = alloc_id;
	if (odi_switch_cmd(OMCI_TCONT_CMD, &t, sizeof(t)))
		return -1;
	return (int)t.index;
}

/* Every Alloc-ID any OMCI T-CONT of this test was ever set to, sent again:
 * what omcid's rebuild sends after a change of the PLOAM list.
 */
static const uint16_t omci_set[] = { 282, 794, 999, 1000, 1001, 2000, 3000 };

static void rebuild(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(omci_set) / sizeof(omci_set[0]); i++)
		(void)cmd21(omci_set[i]);
}

int main(void)
{
	struct odi_gpon_fsm fsm;
	unsigned int rx, tx0, tx1;
	static const uint8_t port0[10] = { 0x00, 0x01, 0xa0 };	/* Port-ID 26 for ONU 0, activate */

	odi_mock_reset();
	odi_mock_write_hook = cam_hook;
	odi_switch_cmd_reset_state();
	odi_gpon_hw_reset();
	odi_sw_host_alloc_row = odi_gpon_hw_alloc_id_find;
	odi_gpon_fsm_init(&fsm);
	odi_gpon_fsm_set_serial_number(&fsm, our_sn);
	odi_gpon_fsm_set_password(&fsm, no_pw);

	/* 1. First activation, ONU-ID 26, two Alloc-IDs by PLOAM. */
	activate(&fsm, 26U);
	CHECK(fsm.state == ODI_GPON_STATE_O5, "activation 1 reaches O5 as ONU 26");
	alloc_msg(&fsm, 26U, 282U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	alloc_msg(&fsm, 26U, 794U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	{
		static const uint16_t want[] = { 26, 282, 794 };

		CHECK(cam_is(want, 3), "the CAM holds the ONU-ID and the two assigned Alloc-IDs");
	}
	CHECK(cmd21(282U) == 0 && cmd21(794U) == 1, "cmd 21 finds an assigned Alloc-ID at its row");
	CHECK(cmd21(999U) < 0, "cmd 21 refuses an Alloc-ID the OLT never assigned by PLOAM (5.5.4)");
	rebuild();
	{
		static const uint16_t want[] = { 26, 282, 794 };

		CHECK(cam_is(want, 3), "no command 21 writes the CAM");
	}

	/* 2. A deallocation, then omcid's rebuild with the T-CONT still set to it. */
	alloc_msg(&fsm, 26U, 794U, ODI_GPON_ALLOC_ID_TYPE_DEALLOCATE);
	rebuild();
	{
		static const uint16_t want[] = { 26, 282 };

		CHECK(cam_is(want, 2), "a deallocated Alloc-ID stays out of the CAM after a rebuild");
	}
	CHECK(cmd21(794U) < 0, "cmd 21 refuses the deallocated Alloc-ID");
	alloc_msg(&fsm, 26U, 26U, ODI_GPON_ALLOC_ID_TYPE_DEALLOCATE);
	{
		static const uint16_t want[] = { 26, 282 };

		CHECK(cam_is(want, 2), "the default Alloc-ID cannot be deallocated (5.5.3)");
	}

	/* 3. The reserved types assign nothing; type 2 is not acknowledged. */
	odi_gpon_get_ploam_counts(&rx, &tx0);
	alloc_msg(&fsm, 26U, 1000U, ODI_GPON_ALLOC_ID_TYPE_DBA);
	odi_gpon_get_ploam_counts(&rx, &tx1);
	CHECK(tx1 == tx0, "type 2 (DBA) is not acknowledged (9.2.3)");
	alloc_msg(&fsm, 26U, 1001U, 0U);
	odi_gpon_get_ploam_counts(&rx, &tx0);
	CHECK(tx0 == tx1 + 3U, "type 0 is acknowledged, once per copy");
	rebuild();
	{
		static const uint16_t want[] = { 26, 282 };

		CHECK(cam_is(want, 2), "types 0 and 2 put no Alloc-ID in the CAM");
	}

	/* 4. LOS in O5: O6 keeps the ONU-ID and its Alloc-IDs (a directed
	 * POPUP returns to O5 with them); TO2 ends the activation.
	 */
	event(&fsm, ODI_GPON_EVENT_LOS);
	CHECK(fsm.state == ODI_GPON_STATE_O6, "LOS in O5 goes to O6");
	{
		static const uint16_t want[] = { 26, 282 };

		CHECK(cam_is(want, 2), "O6 keeps the rows");
	}
	event(&fsm, ODI_GPON_EVENT_TO2_EXPIRE);
	CHECK(fsm.state == ODI_GPON_STATE_O1, "TO2 goes to O1");
	CHECK(cam_is(NULL, 0), "TO2 empties the CAM: the assignments are lost with the ONU-ID");
	rebuild();
	CHECK(cam_is(NULL, 0), "after TO2, omcid's rebuild puts nothing back");

	/* 5. No ONU-ID held (O2, then O3): another ONU, ONU 0, is assigned an
	 * Alloc-ID and its OMCC port.
	 */
	event(&fsm, ODI_GPON_EVENT_LOS_CLEAR);
	CHECK(fsm.state == ODI_GPON_STATE_O2, "LOS clear goes to O2");
	alloc_msg(&fsm, 0U, 2000U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	CHECK(cam_is(NULL, 0), "in O2, an Assign_Alloc-ID for ONU 0 is not ours");
	activate(&fsm, ODI_GPON_ONU_ID_BROADCAST);
	CHECK(fsm.state == ODI_GPON_STATE_O3, "Upstream_Overhead goes to O3");
	alloc_msg(&fsm, 0U, 2000U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	ploam(&fsm, 0U, ODI_GPON_DS_CONFIGURE_PORT_ID, port0);
	rebuild();
	CHECK(cam_is(NULL, 0), "in O3, an Assign_Alloc-ID for ONU 0 is not ours, and a rebuild adds nothing");
	CHECK(odi_reg_read(ODI_SW_US_GEM_PORT_MAP(64U)) == 0U,
	      "in O3, a Configure_Port-ID for ONU 0 does not touch our OMCC port");

	/* 6. Second activation, as ONU 0 this time: messages for ONU 0 are
	 * ours now, and nothing of activation 1 or of the other ONU comes back.
	 */
	activate(&fsm, 0U);
	CHECK(fsm.state == ODI_GPON_STATE_O5, "activation 2 reaches O5 as ONU 0");
	alloc_msg(&fsm, 0U, 3000U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	rebuild();
	{
		static const uint16_t want[] = { 0, 3000 };

		CHECK(cam_is(want, 2), "activation 2: the CAM holds ONU-ID 0 and 3000 only");
	}
	CHECK(cmd21(3000U) == 0, "cmd 21 finds 3000 at row 0");
	CHECK(cmd21(282U) < 0, "cmd 21 refuses activation 1's Alloc-ID");

	/* 7. Deactivate_ONU-ID (O5 -> O2). */
	{
		static const uint8_t none[10];

		ploam(&fsm, 0U, ODI_GPON_DS_DEACTIVATE_ONU_ID, none);
	}
	CHECK(fsm.state == ODI_GPON_STATE_O2, "Deactivate_ONU-ID goes to O2");
	rebuild();
	CHECK(cam_is(NULL, 0), "after Deactivate_ONU-ID the CAM stays empty through a rebuild");

	/* 8. Disable_Serial_Number (O7): a third activation, then disabled. */
	activate(&fsm, 26U);
	alloc_msg(&fsm, 26U, 282U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	{
		uint8_t dsn[10] = { 0 };

		dsn[0] = ODI_GPON_DISABLE_SN_DISABLE;
		memcpy(&dsn[1], our_sn, sizeof(our_sn));
		ploam(&fsm, ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_DISABLE_SERIAL_NUMBER, dsn);
	}
	CHECK(fsm.state == ODI_GPON_STATE_O7, "Disable_Serial_Number goes to O7");
	alloc_msg(&fsm, 0U, 2000U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	alloc_msg(&fsm, 26U, 282U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	rebuild();
	CHECK(cam_is(NULL, 0), "in O7 nothing arms the CAM: not a rebuild, not a message for ONU 0 or the old ONU-ID");

	/* 9. gpondeact from O5. */
	activate(&fsm, 26U);
	alloc_msg(&fsm, 26U, 794U, ODI_GPON_ALLOC_ID_TYPE_GEM);
	odi_gpon_verb_deactivate(&fsm);
	rebuild();
	CHECK(cam_is(NULL, 0), "after gpondeact the CAM stays empty through a rebuild");

	if (failures) {
		fprintf(stderr, "odi_alloc_cam_safety_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_alloc_cam_safety_test: ok\n");
	return 0;
}
