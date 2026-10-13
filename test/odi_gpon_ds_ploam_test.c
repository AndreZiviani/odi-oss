/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_ds_ploam_test.c -- the downstream PLOAM drain of
 * odi_gpon_isr_poll() against test/odi_switch_mock.h and its PLOAM FIFO
 * model: one PLOAM_RX interrupt with several messages queued drains them
 * all, in order, each dispatched to the FSM; one queued message is drained
 * exactly as before (one pop handshake); a FIFO that never reads empty
 * stops at the bound; and the per-message hook sees each message with the
 * state it found.
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

int main(void)
{
	test_three_queued_one_interrupt();
	test_two_copies_two_acks();
	test_one_message_unchanged();
	test_stuck_fifo_bounded();
	test_hook_per_message();
	CHECK(odi_mock_locks_idle(), "no lock left held");
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_ds_ploam_test: ok\n");
	return 0;
}
