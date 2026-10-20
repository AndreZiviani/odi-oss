/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_ds_ploam_test.c -- the downstream PLOAM drain of
 * odi_gpon_isr_poll() against test/odi_switch_mock.h and its PLOAM FIFO
 * model: one PLOAM_RX interrupt with several messages queued drains them
 * all, in order, each dispatched to the FSM; one queued message is drained
 * exactly as before (one pop handshake); a FIFO_EMPTY that trails the pop
 * by one read does not make a message count twice; a FIFO that never
 * reads empty stops at the bound; and the per-message hook sees each
 * message with the state it found.
 */
#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define TEST_ONU_ID	0x1aU

static const uint8_t test_sn[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };

static void poke(uint32_t off, uint32_t v)
{
	odi_mock.regs[odi_mock_slot(off)] = v;
}

static unsigned int pops(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == ODI_GPON_DSF_PLOAM_RX_CTL_OFF &&
		    odi_mock.log[i].val == 1U)
			n++;
	return n;
}

static void queue(const struct odi_gpon_ploam *m)
{
	uint16_t words[6];

	odi_gpon_ploam_pack_words(m, words);
	odi_mock_ds_ploam_queue(words);
}

static struct odi_gpon_ploam ds(uint8_t onu_id, uint8_t type)
{
	struct odi_gpon_ploam m;

	memset(&m, 0, sizeof(m));
	m.onu_id = onu_id;
	m.type = type;
	return m;
}

/* An ONU in O2 with the interrupt pending for a PLOAM. */
static void setup(struct odi_gpon_fsm *fsm)
{
	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(fsm);
	odi_gpon_fsm_set_serial_number(fsm, test_sn);
	fsm->state = ODI_GPON_STATE_O2;
	poke(ODI_GPON_PONMAC_IRQ_PENDING_OFF, ODI_GPON_PONMAC_IRQ_PENDING_DS_FRAMER);
	poke(ODI_GPON_DSF_IRQ_EVENT_OFF, ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX);
	poke(ODI_GPON_USF_MIN_RESP_DELAY_OFF, 0x9132U);
}

/* The activation burst, all queued before the interrupt is serviced:
 * Upstream_Overhead (O2 -> O3), Assign_ONU-ID (O3 -> O4), Ranging_Time
 * (O4 -> O5). One interrupt must take the ONU all the way.
 */
static void test_three_queued_one_interrupt(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;
	unsigned int n, rx, tx;

	setup(&fsm);
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_UPSTREAM_OVERHEAD);
	m.content[0] = 0x20U;	/* 32 guard bits */
	m.content[3] = 0xaaU;
	m.content[4] = 0xabU;
	m.content[5] = 0x59U;
	m.content[6] = 0x83U;
	queue(&m);
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_ASSIGN_ONU_ID);
	m.content[0] = TEST_ONU_ID;
	memcpy(&m.content[1], test_sn, 8U);
	queue(&m);
	m = ds(TEST_ONU_ID, ODI_GPON_DS_RANGING_TIME);
	m.content[3] = 0x77U;
	m.content[4] = 0x64U;
	queue(&m);

	n = odi_gpon_isr_poll(&fsm, NULL);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(n == 3U, "one interrupt drains all three queued messages");
	CHECK(rx == 3U, "each one is dispatched");
	CHECK(fsm.state == ODI_GPON_STATE_O5, "in order: O2 -> O3 -> O4 -> O5 in one interrupt");
	CHECK(fsm.onu_id == TEST_ONU_ID, "the Assign_ONU-ID in the middle was applied");
	CHECK(pops() == 3U, "three pop handshakes");
	CHECK(odi_mock.ds_ploam_n == 0U, "the FIFO is left empty");
	CHECK(odi_gpon_isr_poll(&fsm, NULL) == 0U, "the next interrupt finds nothing stranded");
}

/* Two copies of the same message, as the react capture shows the stock
 * driver draining in one pass: the second Acknowledge is not delayed to a
 * later interrupt.
 */
static void test_two_copies_two_acks(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;
	unsigned int rx, tx;

	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	m = ds(TEST_ONU_ID, ODI_GPON_DS_BER_INTERVAL);
	m.content[3] = 0x01U;
	m.content[4] = 0x38U;
	m.content[5] = 0x80U;
	queue(&m);
	queue(&m);
	CHECK(odi_gpon_isr_poll(&fsm, NULL) == 2U, "both copies drained in one interrupt");
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(tx == 2U, "and both acknowledged in it");
}

/* One message, poked the way the replay and the other tests do it: the
 * same single drain as before, one pop pair, then the FIFO reads empty.
 */
static void test_one_message_unchanged(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;
	uint16_t words[6];
	unsigned int i, ctl_writes = 0;

	setup(&fsm);
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_UPSTREAM_OVERHEAD);
	odi_gpon_ploam_pack_words(&m, words);
	poke(ODI_GPON_DSF_PLOAM_RX_CTL_OFF, 0U);
	for (i = 0; i < 6U; i++)
		poke(ODI_GPON_DSF_PLOAM_RX_WORD(i), words[i]);

	CHECK(odi_gpon_isr_poll(&fsm, NULL) == 1U, "one queued message, one drained");
	CHECK(fsm.state == ODI_GPON_STATE_O3, "and dispatched");
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == ODI_GPON_DSF_PLOAM_RX_CTL_OFF)
			ctl_writes++;
	CHECK(ctl_writes == 2U, "one pop handshake, IND=0 then IND=1, nothing more");
}

/* FIFO_EMPTY trailing the pop by one read of the control word: the drain
 * must not take the message it just popped for a second one. A second
 * dispatch would send a second Acknowledge, and for a Request_Key a
 * second key under a new Key_Index.
 */
static void test_empty_lags_pop(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;
	unsigned int n, rx, tx;

	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	odi_mock.ds_ploam_lag = 1;
	m = ds(TEST_ONU_ID, ODI_GPON_DS_BER_INTERVAL);
	m.content[4] = 0x38U;
	queue(&m);
	n = odi_gpon_isr_poll(&fsm, NULL);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(n == 1U && rx == 1U, "one queued message is drained once, status lagging or not");
	CHECK(tx == 1U, "and acknowledged once");
	CHECK(pops() == 1U, "no pop of an empty FIFO");

	queue(&m);
	queue(&m);
	queue(&m);
	poke(ODI_GPON_DSF_IRQ_EVENT_OFF, ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX);
	n = odi_gpon_isr_poll(&fsm, NULL);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(n == 3U && rx == 4U && tx == 4U, "three queued, three drained and acknowledged");
	CHECK(odi_mock.ds_ploam_n == 0U, "the FIFO is left empty");

	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	odi_mock.ds_ploam_lag = 1;
	m = ds(TEST_ONU_ID, ODI_GPON_DS_REQUEST_KEY);
	queue(&m);
	(void)odi_gpon_isr_poll(&fsm, NULL);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(rx == 1U && tx == 6U, "one Request_Key, one key: six fragments, not twelve");
}

/* A FIFO_EMPTY that never sets: the drain stops at the bound. */
static void stuck_fifo_hook(uint32_t off, uint32_t val)
{
	(void)val;
	if (off == ODI_GPON_DSF_PLOAM_RX_CTL_OFF)
		odi_mock.regs[odi_mock_slot(off)] &= ~(1U << 5);
}

static void test_stuck_fifo_bounded(void)
{
	struct odi_gpon_fsm fsm;
	unsigned int n;

	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O1;	/* no message acts in O1 */
	poke(ODI_GPON_DSF_PLOAM_RX_CTL_OFF, 0U);
	odi_mock_write_hook = stuck_fifo_hook;
	n = odi_gpon_isr_poll(&fsm, NULL);
	odi_mock_write_hook = NULL;
	CHECK(n == 16U, "a FIFO that never empties is drained 16 times, then left");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_GPON_PONMAC_IRQ_ENABLE_OFF)] == 0x22U,
	      "and the interrupt mask is restored");
}

/* The hook odi_gpon.c logs and re-arms its timers from: once per message,
 * with the state that message found.
 */
static enum odi_gpon_state hook_prev[8];
static uint8_t hook_type[8];
static unsigned int hook_n;

static void msg_hook(enum odi_gpon_state prev, const struct odi_gpon_ploam *msg)
{
	if (hook_n < 8U) {
		hook_prev[hook_n] = prev;
		hook_type[hook_n] = msg->type;
	}
	hook_n++;
}

static void test_hook_per_message(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;

	setup(&fsm);
	hook_n = 0;
	odi_gpon_isr_set_msg_hook(msg_hook);
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_UPSTREAM_OVERHEAD);
	queue(&m);
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_ASSIGN_ONU_ID);
	m.content[0] = TEST_ONU_ID;
	memcpy(&m.content[1], test_sn, 8U);
	queue(&m);
	(void)odi_gpon_isr_poll(&fsm, NULL);
	odi_gpon_isr_set_msg_hook(NULL);
	CHECK(hook_n == 2U, "the hook runs once per message drained");
	CHECK(hook_prev[0] == ODI_GPON_STATE_O2 && hook_type[0] == ODI_GPON_DS_UPSTREAM_OVERHEAD,
	      "first with the state Upstream_Overhead found (O2)");
	CHECK(hook_prev[1] == ODI_GPON_STATE_O3 && hook_type[1] == ODI_GPON_DS_ASSIGN_ONU_ID,
	      "then with the state Assign_ONU-ID found (O3)");
}

/* G.984.3 gives Ranging_Time, Request_Password, Request_Key,
 * Encrypted_Port-ID, Assign_Alloc-ID and Configure_Port-ID as directed
 * messages (clauses 9.2.3.4, 9.2.3.8-9.2.3.10, 9.2.3.13, 9.2.3.14), and
 * the ONU acts on one when its ONU-ID matches (clause 10.2.5.1 d) for
 * Ranging_Time). The MAC filters other ONUs' unicast messages
 * (DS_PLOAM_CFG 0x70b, the boot replay) but passes the broadcast ID, so a
 * broadcast copy of one of these reaches the dispatch: it must change
 * nothing and draw no Acknowledge. The same message with our ONU-ID still
 * works.
 */
static unsigned int us_sent(void)
{
	unsigned int rx, tx;

	odi_gpon_get_ploam_counts(&rx, &tx);
	return tx;
}

static unsigned int writes_to(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

static void test_broadcast_copy_of_directed_message(void)
{
	static const uint8_t directed[] = {
		ODI_GPON_DS_REQUEST_PASSWORD, ODI_GPON_DS_REQUEST_KEY,
		ODI_GPON_DS_ENCRYPTED_PORT_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID,
		ODI_GPON_DS_CONFIGURE_PORT_ID,
	};
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;
	unsigned int i, tx0;

	/* O4: a broadcast Ranging_Time neither ranges nor writes the EqD. */
	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O4;
	fsm.onu_id = TEST_ONU_ID;
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_RANGING_TIME);
	m.content[3] = 0x77U;
	m.content[4] = 0x64U;
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O4, "O4: a broadcast Ranging_Time does not range");
	CHECK(writes_to(ODI_GPON_USF_EQ_DELAY_OFF) == 0U, "O4: a broadcast Ranging_Time writes no EqD");
	m.onu_id = TEST_ONU_ID;
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O5, "O4: our own Ranging_Time still ranges");

	/* O5: a broadcast Ranging_Time does not move the EqD either. */
	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_RANGING_TIME);
	m.content[4] = 0x10U;
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(writes_to(ODI_GPON_USF_EQ_DELAY_OFF) == 0U, "O5: a broadcast Ranging_Time writes no EqD");

	/* O5: the others, broadcast, draw no upstream message and no CAM work. */
	for (i = 0; i < sizeof(directed); i++) {
		char what[96];

		setup(&fsm);
		fsm.state = ODI_GPON_STATE_O5;
		fsm.onu_id = TEST_ONU_ID;
		m = ds(ODI_GPON_ONU_ID_BROADCAST, directed[i]);
		m.content[0] = 0x03U;	/* Encrypted_Port-ID a = b = 1; Configure_Port-ID activate */
		m.content[1] = 0x29U;	/* Port-ID / Alloc-ID 0x291 */
		m.content[2] = 0x10U;
		queue(&m);
		tx0 = us_sent();
		odi_gpon_isr_poll(&fsm, NULL);
		snprintf(what, sizeof what, "O5: broadcast type 0x%02x draws no upstream message", directed[i]);
		CHECK(us_sent() == tx0, what);
		snprintf(what, sizeof what, "O5: broadcast type 0x%02x touches no CAM", directed[i]);
		CHECK(writes_to(ODI_SW_DSF_GEM_CAM_CTL_OFF) == 0U &&
		      writes_to(ODI_SW_DSF_ALLOC_CAM_CTL_OFF) == 0U, what);
	}

	/* Our own Assign_Alloc-ID is still applied and acknowledged. */
	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	m = ds(TEST_ONU_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID);
	m.content[0] = 0x29U;
	m.content[1] = 0x10U;
	m.content[2] = ODI_GPON_ALLOC_ID_TYPE_GEM;	/* type 1: G.984.3 9.2.3.10, the only assigning type */
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(us_sent() == tx0 + 1U, "O5: our own Assign_Alloc-ID is acknowledged");
	CHECK(writes_to(ODI_SW_DSF_ALLOC_CAM_CTL_OFF) > 0U, "O5: our own Assign_Alloc-ID writes the Alloc-ID CAM");

	/* BER_Interval may be broadcast (stock acts on either): still acknowledged. */
	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_BER_INTERVAL);
	m.content[3] = 0x01U;
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(us_sent() == tx0 + 1U, "O5: a broadcast BER_Interval is still acknowledged");
}

/* Key_Switching_Time and BER_Interval (G.984.3 9.2.3.19, 9.2.3.18: "Directed
 * message to one ONU or all ONUs"): taken from our own ONU-ID or the
 * broadcast, never from another ONU's copy, and only while this ONU holds
 * an ONU-ID. ONU-ID 0 is a real ID once assigned.
 */
static uint32_t switch_frame_reg(void)
{
	return odi_mock.regs[odi_mock_slot(ODI_GPON_DSK_SWITCH_FRAME_OFF)];
}

static struct odi_gpon_ploam kst_msg(uint8_t onu_id, uint32_t superframe)
{
	struct odi_gpon_ploam m = ds(onu_id, ODI_GPON_DS_KEY_SWITCHING_TIME);

	m.content[0] = (uint8_t)((superframe >> 24) & 0x3fU);
	m.content[1] = (uint8_t)(superframe >> 16);
	m.content[2] = (uint8_t)(superframe >> 8);
	m.content[3] = (uint8_t)superframe;
	return m;
}

static struct odi_gpon_ploam ber_msg(uint8_t onu_id, uint32_t frames)
{
	struct odi_gpon_ploam m = ds(onu_id, ODI_GPON_DS_BER_INTERVAL);

	m.content[0] = (uint8_t)(frames >> 24);
	m.content[1] = (uint8_t)(frames >> 16);
	m.content[2] = (uint8_t)(frames >> 8);
	m.content[3] = (uint8_t)frames;
	return m;
}

static void test_kst_and_ber_addressing(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;
	unsigned int tx0;

	/* Another ONU's copy: no switch time, no REI interval, no Acknowledge. */
	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	m = kst_msg(TEST_ONU_ID + 1U, 0x00123456U);
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(writes_to(ODI_GPON_DSK_SWITCH_FRAME_OFF) == 0U, "another ONU's Key_Switching_Time arms nothing");
	CHECK(us_sent() == tx0, "another ONU's Key_Switching_Time is not acknowledged");

	odi_gpon_hw_note_ber_interval(0U);
	m = ber_msg(TEST_ONU_ID + 1U, 0x00001000U);
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(odi_gpon_hw_get_ber_interval_frames() == 0U, "another ONU's BER_Interval sets no interval");
	CHECK(us_sent() == tx0, "another ONU's BER_Interval is not acknowledged");

	/* Ours, then a broadcast one: both applied and acknowledged. */
	m = kst_msg(TEST_ONU_ID, 0x00123456U);
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(switch_frame_reg() == 0x00123456U, "our Key_Switching_Time arms the switch");
	CHECK(us_sent() == tx0 + 1U, "our Key_Switching_Time is acknowledged");
	m = kst_msg(ODI_GPON_ONU_ID_BROADCAST, 0x00654321U);
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(switch_frame_reg() == 0x00654321U, "a broadcast Key_Switching_Time arms the switch");
	CHECK(us_sent() == tx0 + 1U, "a broadcast Key_Switching_Time is acknowledged");
	m = ber_msg(TEST_ONU_ID, 0x00001000U);
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(odi_gpon_hw_get_ber_interval_frames() == 0x1000U, "our BER_Interval sets the interval");
	CHECK(us_sent() == tx0 + 1U, "our BER_Interval is acknowledged");

	/* ONU-ID 0 assigned: its own messages count. Without an ONU-ID (O3) the
	 * zero in fsm.onu_id is "none" and a message to ONU 0 is another ONU's.
	 */
	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = 0U;
	m = kst_msg(0U, 0x00111111U);
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(switch_frame_reg() == 0x00111111U, "ONU-ID 0, assigned: its Key_Switching_Time is applied");
	setup(&fsm);
	fsm.state = ODI_GPON_STATE_O3;
	fsm.onu_id = 0U;
	m = ber_msg(0U, 0x00002000U);
	queue(&m);
	tx0 = us_sent();
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(us_sent() == tx0, "O3, no ONU-ID: a BER_Interval to ONU 0 is not ours");
	m = ber_msg(ODI_GPON_ONU_ID_BROADCAST, 0x00002000U);
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(us_sent() == tx0, "O3, no ONU-ID: a broadcast BER_Interval is not acted on either");
}

static uint32_t boh_length(void)
{
	return ODI_GPON_USF_BURST_HDR_SETUP_LENGTH_GET(
		odi_mock.regs[odi_mock_slot(ODI_GPON_USF_BURST_HDR_SETUP_OFF)]);
}

/* Extended_Burst_Length (G.984.3 Table 10-1, 10.2.2 b)): taken in O3 only;
 * what O3 set is not rewritten by a copy that arrives in O4 or O5. */
static void test_ext_burst_length_state(void)
{
	static const uint8_t delim[3] = { 0xab, 0x59, 0x83 };
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam m;
	uint32_t len;

	setup(&fsm);
	odi_gpon_hw_ops()->set_upstream_overhead(NULL, 32U, 0U, 0U, 0xaaU, delim, 0U, 0U);
	fsm.state = ODI_GPON_STATE_O3;
	m = ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_EXT_BURST_LENGTH);
	m.content[0] = 41U;
	m.content[1] = 18U;
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	len = boh_length();
	CHECK(len == 48U, "O3: Extended_Burst_Length sets the pre-ranged overhead (41 + 7)");

	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	poke(ODI_GPON_DSF_IRQ_EVENT_OFF, ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX);
	m.content[0] = 90U;
	m.content[1] = 90U;
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(boh_length() == len, "O5: a repeated Extended_Burst_Length changes nothing");

	fsm.state = ODI_GPON_STATE_O4;
	poke(ODI_GPON_DSF_IRQ_EVENT_OFF, ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX);
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(boh_length() == len, "O4: Extended_Burst_Length changes nothing");

	/* The whole overhead is 128 bytes at most (9.2.3.20 note 2). */
	setup(&fsm);
	odi_gpon_hw_ops()->set_upstream_overhead(NULL, 32U, 0U, 0U, 0xaaU, delim, 0U, 0U);
	fsm.state = ODI_GPON_STATE_O3;
	m.content[0] = 200U;
	m.content[1] = 18U;
	queue(&m);
	odi_gpon_isr_poll(&fsm, NULL);
	CHECK(boh_length() == 128U, "O3: an Extended_Burst_Length past 128 bytes in all is cut at 128");
}

int main(void)
{
	test_three_queued_one_interrupt();
	test_two_copies_two_acks();
	test_one_message_unchanged();
	test_stuck_fifo_bounded();
	test_empty_lags_pop();
	test_hook_per_message();
	test_broadcast_copy_of_directed_message();
	test_kst_and_ber_addressing();
	test_ext_burst_length_state();
	CHECK(odi_mock_locks_idle(), "no lock left held");
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_ds_ploam_test: ok\n");
	return 0;
}
