/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_us_ploam_test.c -- the upstream PLOAM push of odi_gpon_hw.c
 * against test/odi_switch_mock.h: a reply goes into the urgent queue only
 * when USF_PLOAM_TX_CTL.URGENT_FULL is clear within the bounded wait, and
 * one that finds the queue full for the whole wait is counted as dropped,
 * never pushed and never counted as sent.
 *
 * URGENT_FULL is a status bit: on the stick a write to the control
 * register does not clear it. The mock register file keeps whatever was
 * written last, so a write hook puts the bit back while the test holds the
 * queue full; the poll then times out only with poll_stuck set (the mock
 * finishes every poll at the first read otherwise, which here is room
 * appearing within the wait).
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

static int queue_full;
static unsigned int pushes;

static void queue_hook(uint32_t off, uint32_t val)
{
	if (off != ODI_GPON_USF_PLOAM_TX_CTL_OFF)
		return;
	if (ODI_GPON_USF_PLOAM_TX_CTL_PUSH_GET(val))
		pushes++;
	if (queue_full)
		odi_mock.regs[odi_mock_slot(off)] |= 1U << 4;	/* URGENT_FULL */
}

static void setup(int full, int stuck)
{
	odi_mock_reset();
	odi_gpon_hw_reset();
	queue_full = full;
	pushes = 0;
	odi_mock.poll_stuck = stuck;
	odi_mock_write_hook = queue_hook;
	if (full)
		odi_mock.regs[odi_mock_slot(ODI_GPON_USF_PLOAM_TX_CTL_OFF)] = 1U << 4;
}

static struct odi_gpon_ploam rei(void)
{
	struct odi_gpon_ploam m;

	odi_gpon_encode_rei(TEST_ONU_ID, 0U, 1U, &m);
	return m;
}

static void test_room_pushes(void)
{
	struct odi_gpon_ploam m = rei();
	unsigned int rx, tx;

	setup(0, 0);
	odi_gpon_hw_send_us_ploam(&m);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(pushes == 1U, "a queue with room takes the push");
	CHECK(tx == 1U, "and the message counts as sent");
	CHECK(odi_gpon_get_ploam_us_dropped() == 0U, "nothing dropped");
}

/* Full at the first look, room within the wait: still pushed. */
static void test_room_within_wait(void)
{
	struct odi_gpon_ploam m = rei();
	unsigned int rx, tx;

	setup(1, 0);
	odi_gpon_hw_send_us_ploam(&m);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(pushes == 1U, "room that appears within the wait takes the push");
	CHECK(tx == 1U && odi_gpon_get_ploam_us_dropped() == 0U, "sent, not dropped");
}

static void test_full_drops(void)
{
	struct odi_gpon_ploam m = rei();
	unsigned int rx, tx;

	setup(1, 1);
	odi_gpon_hw_send_us_ploam(&m);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(pushes == 0U, "a queue full for the whole wait gets no push");
	CHECK(tx == 0U, "and the message does not count as sent");
	CHECK(odi_gpon_get_ploam_us_dropped() == 1U, "it counts as dropped");
}

/* The burst a Request_Key makes, six fragments, against a full queue: six
 * drops, each wait bounded, the lock model left idle.
 */
static void test_request_key_burst_full(void)
{
	struct odi_gpon_fsm fsm;
	struct odi_gpon_ploam msg;
	unsigned int rx, tx;

	setup(1, 1);
	odi_gpon_fsm_init(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	memset(&msg, 0, sizeof(msg));
	msg.onu_id = TEST_ONU_ID;
	msg.type = ODI_GPON_DS_REQUEST_KEY;
	odi_gpon_fsm_handle_event(&fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_PLOAM_RX, &msg);
	odi_gpon_get_ploam_counts(&rx, &tx);
	CHECK(pushes == 0U && tx == 0U, "no fragment pushed into a full queue");
	CHECK(odi_gpon_get_ploam_us_dropped() == 6U, "all six fragments counted as dropped");
	CHECK(odi_mock_locks_idle(), "no lock left held");
}

/* Dying_Gasp: octet 1 the ONU-ID, octet 2 the id 3, the content zero; and
 * the wait variant: left (EMPTY set), queued but stuck, not queued.
 */
static void test_dying_gasp(void)
{
	struct odi_gpon_ploam m;
	unsigned int i;
	int rc;

	odi_gpon_encode_dying_gasp(TEST_ONU_ID, &m);
	CHECK(m.onu_id == TEST_ONU_ID && m.type == 3U, "Dying_Gasp is ONU-ID, then id 3");
	for (i = 0; i < ODI_GPON_PLOAM_CONTENT_LEN; i++)
		CHECK(m.content[i] == 0U, "Dying_Gasp content is zero");

	setup(0, 0);
	rc = odi_gpon_hw_send_us_ploam_wait(&m, 50000U);
	CHECK(rc == 0 && pushes == 1U, "a copy that leaves the queue is reported left");

	setup(0, 1);
	odi_mock.regs[odi_mock_slot(ODI_GPON_USF_PLOAM_TX_CTL_OFF)] = 0;
	rc = odi_gpon_hw_send_us_ploam_wait(&m, 50000U);
	CHECK(rc == -ETIMEDOUT && pushes == 1U, "a copy that stays queued times out, bounded");

	setup(1, 1);
	rc = odi_gpon_hw_send_us_ploam_wait(&m, 50000U);
	CHECK(rc == -EBUSY && pushes == 0U, "a full queue takes no copy");
}

int main(void)
{
	test_room_pushes();
	test_room_within_wait();
	test_full_drops();
	test_request_key_burst_full();
	test_dying_gasp();
	odi_mock_write_hook = NULL;
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_us_ploam_test: ok\n");
	return 0;
}
