/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_los_test.c -- a fibre pull and its return, through the line
 * sampling (odi_gpon_line_sample(), odi_gpon_isr.c), the FSM and the real
 * register leaves, against the host register mock.
 *
 * The ONU is ranged on ISP1 (Upstream_Overhead, Assign_ONU-ID, Ranging_Time,
 * Configure_Port-ID for the OMCC port, five Assign_Alloc-ID), then:
 *
 *   - LOS and LOF set in DSF_ALARM_STATE with no interrupt pending and no
 *     PLOAM queued, what a pulled fibre leaves: the interrupt path alone
 *     sees nothing, and one line sample (what the line poll timer of
 *     odi_gpon.c runs every 100 ms) takes O5 to O6, with the port link
 *     forced down and every row still in place (POPUP could bring it back);
 *   - TO2 expires: O1, the ONU-ID registers back at broadcast and every row
 *     the activation wrote released (the Alloc-ID CAM, modelled from its
 *     handshake as in odi_reregister_test.c, the OMCC GEM port, its flow
 *     type, upstream map and stream-valid gate);
 *   - the light comes back: O2, and a ranging on another ONU-ID with two
 *     Alloc-IDs instead of five leaves exactly those rows, none of the
 *     first activation;
 *   - LOF alone, LOM alone, the light back inside the TO2 window,
 *     Deactivate_ONU-ID (the same release), and an ONU this side
 *     deactivated (gpondeact), which the line no longer moves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define ONU_ID		26U	/* ISP1 assigns it; not an identity value */
#define ONU_ID_2	27U	/* the ID a second ranging gets here */
#define OMCC_GEM_ROW	64U
#define OMCC_ALLOC_ROW	16U

#define LOS	ODI_GPON_DSF_ALARM_STATE_LOS_NOW
#define LOF	ODI_GPON_DSF_ALARM_STATE_LOF_NOW
#define LOM	ODI_GPON_DSF_ALARM_STATE_LOM_NOW
#define FEC	ODI_GPON_DSF_ALARM_STATE_FEC_NOW

static const uint8_t dummy_sn[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
static const uint8_t no_pw[10];

/* The five Alloc-IDs ISP1 assigns, in its order (/proc/odi_gpon/status on ISP1). */
static const uint16_t isp1_alloc[5] = { 282, 794, 1050, 1306, 538 };

/* ---- The Alloc-ID CAM, modelled from its handshake (odi_reregister_test.c) */

struct cam_row {
	int valid;
	uint32_t alloc_id;
};
static struct cam_row cam[32];

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

static unsigned int cam_valid_rows(void)
{
	unsigned int row, n = 0;

	for (row = 0; row < 32U; row++)
		n += cam[row].valid ? 1U : 0U;
	return n;
}

/* ---- Registers --------------------------------------------------------- */

static void poke(uint32_t off, uint32_t val)
{
	odi_mock.regs[odi_mock_slot(off)] = val;
}

static uint32_t peek(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

static uint32_t hw_state(void)
{
	return ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_GET(peek(ODI_GPON_DSF_ONU_STATE_OFF));
}

static uint32_t hw_ds_onu_id(void)
{
	return ODI_GPON_DSF_ONU_STATE_ASSIGNED_ONU_ID_GET(peek(ODI_GPON_DSF_ONU_STATE_OFF));
}

static uint32_t hw_us_onu_id(void)
{
	return (peek(ODI_GPON_USF_ONU_ID_OFF) >> 8) & 0xffU;
}

/* ---- Downstream PLOAM, one interrupt pass per message (sent 3x) --------- */

static void ploam(struct odi_gpon_fsm *fsm, uint8_t onu_id, uint8_t type,
		  const uint8_t content[10])
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
	poke(ODI_GPON_PONMAC_IRQ_PENDING_OFF, 0U);
	poke(ODI_GPON_DSF_IRQ_EVENT_OFF, 0U);
}

/* Ranges the ONU from O2 to O5 on onu_id, with the OMCC port on the same
 * number and the first n_alloc of the ISP1 Alloc-IDs.
 */
static void range(struct odi_gpon_fsm *fsm, uint8_t onu_id, unsigned int n_alloc)
{
	static const uint8_t overhead[10] = { 0x20, 0x00, 0x00, 0xaa, 0xab, 0x59, 0x83, 0x20, 0x00, 0x00 };
	static const uint8_t ranging[10] = { 0x00, 0x00, 0x03, 0x77, 0x64 };
	uint8_t c[10] = { 0 };
	unsigned int i;

	ploam(fsm, ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_UPSTREAM_OVERHEAD, overhead);
	c[0] = onu_id;
	memcpy(&c[1], dummy_sn, sizeof(dummy_sn));
	ploam(fsm, ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_ASSIGN_ONU_ID, c);
	ploam(fsm, onu_id, ODI_GPON_DS_RANGING_TIME, ranging);

	memset(c, 0, sizeof(c));
	c[0] = 0x01U;				/* activate */
	c[1] = (uint8_t)(onu_id >> 4);		/* Port-ID, 12 bits */
	c[2] = (uint8_t)((onu_id & 0xfU) << 4);
	ploam(fsm, onu_id, ODI_GPON_DS_CONFIGURE_PORT_ID, c);

	for (i = 0; i < n_alloc; i++) {
		memset(c, 0, sizeof(c));
		c[0] = (uint8_t)(isp1_alloc[i] >> 4);
		c[1] = (uint8_t)((isp1_alloc[i] & 0xfU) << 4);
		c[2] = ODI_GPON_ALLOC_ID_TYPE_GEM;
		ploam(fsm, onu_id, ODI_GPON_DS_ASSIGN_ALLOC_ID, c);
	}
}

/* A fresh ONU in O5 on ISP1: gponact (after a boot, as the reload does),
 * then the ranging.
 */
static void boot_to_o5(struct odi_gpon_fsm *fsm)
{
	odi_mock_reset();
	odi_mock_write_hook = cam_hook;
	memset(cam, 0, sizeof(cam));
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(fsm);
	odi_gpon_fsm_set_serial_number(fsm, dummy_sn);
	odi_gpon_fsm_set_password(fsm, no_pw);
	odi_gpon_verb_activate(fsm);
	range(fsm, ONU_ID, 5U);
}

/* What the activation leaves, by name. */
static void check_active(const struct odi_gpon_fsm *fsm, uint8_t onu_id, unsigned int n_alloc,
			 const char *when)
{
	unsigned int row;
	uint16_t ids[32];
	char what[160];

	snprintf(what, sizeof(what), "%s: O5 on ONU-ID %u", when, (unsigned int)onu_id);
	CHECK(fsm->state == ODI_GPON_STATE_O5 && fsm->onu_id == onu_id && hw_state() == 5U &&
	      hw_ds_onu_id() == onu_id && hw_us_onu_id() == onu_id, what);
	snprintf(what, sizeof(what), "%s: Alloc-ID CAM row 16 holds the ONU-ID", when);
	CHECK(cam[OMCC_ALLOC_ROW].valid && cam[OMCC_ALLOC_ROW].alloc_id == onu_id, what);
	for (row = 0; row < n_alloc; row++) {
		snprintf(what, sizeof(what), "%s: Alloc-ID CAM row %u holds %u", when, row,
			 (unsigned int)isp1_alloc[row]);
		CHECK(cam[row].valid && cam[row].alloc_id == isp1_alloc[row], what);
	}
	snprintf(what, sizeof(what), "%s: %u valid Alloc-ID CAM rows, nothing else", when, n_alloc + 1U);
	CHECK(cam_valid_rows() == n_alloc + 1U, what);
	snprintf(what, sizeof(what), "%s: /proc/odi_gpon/status lists %u Alloc-IDs", when, n_alloc);
	CHECK(odi_gpon_get_alloc_ids(ids, 32U) == n_alloc, what);
	snprintf(what, sizeof(what), "%s: the OMCC GEM port is up (US map, stream valid)", when);
	CHECK(peek(ODI_SW_US_GEM_PORT_MAP(OMCC_GEM_ROW)) == onu_id &&
	      peek(ODI_SW_PONQ_STREAM_VALID(2U)) == 1U, what);
	snprintf(what, sizeof(what), "%s: the PON port accepts frames (MAX_FRAME_LEN_1)", when);
	CHECK(peek(ODI_SW_MAX_FRAME_LEN_1_OFF) == 0x7efU, what);
	snprintf(what, sizeof(what), "%s: no switch lock left held", when);
	CHECK(odi_mock_locks_idle(), what);
}

/* What a fall to O1 must leave: no ONU-ID, no row of the activation. */
static void check_released(const struct odi_gpon_fsm *fsm, const char *when)
{
	uint16_t ids[32];
	char what[160];

	snprintf(what, sizeof(what), "%s: ONU-ID broadcast in both directions", when);
	CHECK(fsm->onu_id == 0U && hw_ds_onu_id() == ODI_GPON_ONU_ID_BROADCAST &&
	      hw_us_onu_id() == ODI_GPON_ONU_ID_BROADCAST, what);
	snprintf(what, sizeof(what), "%s: every Alloc-ID CAM row released, the default one included", when);
	CHECK(cam_valid_rows() == 0U, what);
	snprintf(what, sizeof(what), "%s: /proc/odi_gpon/status lists no Alloc-ID", when);
	CHECK(odi_gpon_get_alloc_ids(ids, 32U) == 0U, what);
	snprintf(what, sizeof(what), "%s: the OMCC GEM port is down (flow type, US map, stream valid)", when);
	CHECK(peek(ODI_SW_DSF_GEM_FLOW_TYPE(OMCC_GEM_ROW)) == 0U &&
	      peek(ODI_SW_US_GEM_PORT_MAP(OMCC_GEM_ROW)) == 0U &&
	      peek(ODI_SW_PONQ_STREAM_VALID(2U)) == 0U, what);
	snprintf(what, sizeof(what), "%s: no switch lock left held", when);
	CHECK(odi_mock_locks_idle(), what);
}

static void sample(struct odi_gpon_fsm *fsm, uint32_t alarms, int watch,
		   struct odi_gpon_line_sample *smp)
{
	poke(ODI_GPON_DSF_ALARM_STATE_OFF, alarms);
	odi_gpon_line_sample(fsm, watch, smp);
}

/* The bug this file exists for: in O5 with the fibre pulled, nothing
 * reaches the interrupt path, and the state machine has to be told by
 * the sampling alone.
 */
static void test_fibre_pull_and_return(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_line_sample smp;

	boot_to_o5(&fsm);
	check_active(&fsm, ONU_ID, 5U, "first ranging");

	/* No interrupt, no PLOAM: the interrupt path finds nothing to do. */
	poke(ODI_GPON_DSF_ALARM_STATE_OFF, LOS | LOF);
	CHECK(odi_gpon_isr_poll(&fsm, NULL) == 0U && fsm.state == ODI_GPON_STATE_O5,
	      "with no interrupt pending the interrupt path alone leaves the ONU in O5");

	/* One line sample: O5 -> O6. */
	sample(&fsm, LOS | LOF, 1, &smp);
	CHECK(smp.fed && smp.event == ODI_GPON_EVENT_LOS && strcmp(smp.cause, "los") == 0,
	      "the line sample feeds LOS, cause los");
	CHECK(smp.alarms == (LOS | LOF), "the sample reports the alarm register as read");
	CHECK(fsm.state == ODI_GPON_STATE_O6 && hw_state() == 6U,
	      "a loss in O5 with no PLOAM traffic is noticed: O6, written to the hardware");
	CHECK(peek(ODI_SW_MAX_FRAME_LEN_1_OFF) == 0U, "entering O6 forces the PON port link down");
	CHECK(fsm.onu_id == ONU_ID && cam_valid_rows() == 6U && hw_ds_onu_id() == ONU_ID,
	      "O6 keeps the ONU-ID and every row, for a POPUP to resume on");

	/* More samples with the light still gone change nothing. */
	sample(&fsm, LOS | LOF, 1, &smp);
	CHECK(!smp.fed && fsm.state == ODI_GPON_STATE_O6, "O6 ignores the alarm level");

	/* No POPUP: TO2 runs out (odi_gpon.c own timer feeds this event). */
	odi_gpon_fsm_handle_event(&fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_TO2_EXPIRE, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O1 && hw_state() == 1U, "TO2 expiry: O1, written to the hardware");
	check_released(&fsm, "after TO2");

	sample(&fsm, LOS | LOF, 1, &smp);
	CHECK(!smp.fed && fsm.state == ODI_GPON_STATE_O1, "O1 stays while the light is gone");

	/* The light comes back: O1 -> O2, then a ranging from scratch, on
	 * another ONU-ID and with only two of the five Alloc-IDs.
	 */
	sample(&fsm, 0U, 1, &smp);
	CHECK(smp.fed && smp.event == ODI_GPON_EVENT_LOS_CLEAR && strcmp(smp.cause, "los_cleared") == 0,
	      "the light back feeds LOS_CLEAR, cause los_cleared");
	CHECK(fsm.state == ODI_GPON_STATE_O2 && hw_state() == 2U, "the light back: O2");
	range(&fsm, ONU_ID_2, 2U);
	check_active(&fsm, ONU_ID_2, 2U, "second ranging after the fibre returned");
}

/* LOF without LOS (light, no framing) is the same row of Table 10-1; LOM
 * and FEC are not.
 */
static void test_lof_alone_and_lom(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_line_sample smp;

	boot_to_o5(&fsm);
	sample(&fsm, LOM | FEC, 1, &smp);
	CHECK(!smp.fed && fsm.state == ODI_GPON_STATE_O5, "LOM and FEC alone leave O5 alone");

	sample(&fsm, LOF | FEC, 1, &smp);
	CHECK(smp.fed && strcmp(smp.cause, "lof") == 0 && fsm.state == ODI_GPON_STATE_O6,
	      "LOF alone takes O5 to O6, cause lof");
}

/* A MAC that is not clocked answers 0xca0eca0f, which has LOS and LOF set:
 * it is not a sample, in any state.
 */
static void test_dark_mac_is_not_a_loss(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_line_sample smp;

	boot_to_o5(&fsm);
	sample(&fsm, ODI_GPON_NO_RESPONSE, 1, &smp);
	CHECK(smp.dark && !smp.fed && fsm.state == ODI_GPON_STATE_O5,
	      "the no-decode reply is not fed as a loss: O5 stays O5");
	sample(&fsm, LOS | LOF, 1, &smp);
	CHECK(!smp.dark && smp.fed && fsm.state == ODI_GPON_STATE_O6,
	      "a real LOS|LOF right after is still a loss");
}

/* The light back inside the TO2 window and no POPUP: TO2 still ends O6,
 * and with no LOS edge left to see, the very next sample takes O1 on to
 * O2 (odi_gpon.c samples right after the TO2 event for this).
 */
static void test_light_back_inside_to2(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_line_sample smp;

	boot_to_o5(&fsm);
	sample(&fsm, LOS, 1, &smp);
	sample(&fsm, 0U, 1, &smp);
	CHECK(!smp.fed && fsm.state == ODI_GPON_STATE_O6,
	      "the light back inside TO2 leaves the ONU in O6, waiting for POPUP");
	odi_gpon_fsm_handle_event(&fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_TO2_EXPIRE, NULL);
	check_released(&fsm, "TO2 with the light already back");
	sample(&fsm, 0U, 1, &smp);
	CHECK(smp.fed && fsm.state == ODI_GPON_STATE_O2, "the next sample takes O1 straight to O2");
}

/* A directed POPUP inside the window: back to O5 with every row, and the
 * PON port link up again.
 */
static void test_popup_resumes(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_line_sample smp;
	static const uint8_t none[10];

	boot_to_o5(&fsm);
	sample(&fsm, LOS, 1, &smp);
	sample(&fsm, 0U, 1, &smp);
	ploam(&fsm, ONU_ID, ODI_GPON_DS_POPUP, none);
	check_active(&fsm, ONU_ID, 5U, "after a directed POPUP");
}

/* Deactivate_ONU-ID discards the ONU-ID through the same leaf, so the same
 * rows go (G.984.3 9.2.3.5: ONU-ID, OMCI Port-ID and Alloc-IDs discarded).
 */
static void test_deactivate_releases_the_same(void)
{
	struct odi_gpon_fsm fsm;
	static const uint8_t none[10];

	boot_to_o5(&fsm);
	ploam(&fsm, ONU_ID, ODI_GPON_DS_DEACTIVATE_ONU_ID, none);
	CHECK(fsm.state == ODI_GPON_STATE_O2 && hw_state() == 2U, "Deactivate_ONU-ID: O2");
	check_released(&fsm, "after Deactivate_ONU-ID");
	range(&fsm, ONU_ID, 5U);
	check_active(&fsm, ONU_ID, 5U, "ranged again after Deactivate_ONU-ID");
}

/* An ONU this side deactivated (gpondeact; odi_gpon.c stops watching the
 * line) stays in O1 when the light is there; only reading happens.
 */
static void test_unwatched_line_only_read(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_line_sample smp;

	boot_to_o5(&fsm);
	odi_gpon_verb_deactivate(&fsm);
	check_released(&fsm, "after gpondeact");
	sample(&fsm, 0U, 0, &smp);
	CHECK(!smp.fed && fsm.state == ODI_GPON_STATE_O1,
	      "an unwatched line does not take a deactivated ONU to O2");
	sample(&fsm, LOS, 0, &smp);
	CHECK(!smp.fed && smp.alarms == LOS, "an unwatched line is still read and reported");

	boot_to_o5(&fsm);
	sample(&fsm, LOS, 0, &smp);
	CHECK(!smp.fed && fsm.state == ODI_GPON_STATE_O5, "an unwatched line feeds nothing in O5 either");
}

int main(void)
{
	test_fibre_pull_and_return();
	test_lof_alone_and_lom();
	test_dark_mac_is_not_a_loss();
	test_light_back_inside_to2();
	test_popup_resumes();
	test_deactivate_releases_the_same();
	test_unwatched_line_only_read();

	if (failures) {
		fprintf(stderr, "odi_gpon_los_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_gpon_los_test: ok (fibre pull in O5 with no PLOAM traffic, TO2, release, re-ranging)\n");
	return 0;
}
