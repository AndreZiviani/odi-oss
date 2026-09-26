/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_irq_test.c -- the switch interrupt line of odi_gpon_isr.c
 * against test/odi_switch_mock.h: the boot reset, the GPON unmask that
 * keeps the ACL source the acl replay enabled, and the demux, which acks
 * every pending and enabled source, serviced or not. The mock register
 * file does not model write-1-to-clear, so an ack is checked as a write
 * of that bit to CHIP_IRQ_PENDING in the write log.
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

#define GPON_BIT	ODI_SW_CHIP_IRQ_ENABLE_GPON
#define ACL_BIT		ODI_SW_CHIP_IRQ_ENABLE_ACL

static unsigned int gpon_calls;
static void gpon_handler(void) { gpon_calls++; }

static void set_reg(uint32_t off, uint32_t v) { odi_mock.regs[odi_mock_slot(off)] = v; }
static uint32_t get_reg(uint32_t off) { return odi_mock.regs[odi_mock_slot(off)]; }

static unsigned int writes_to(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

static void test_reset(void)
{
	odi_mock_reset();
	set_reg(ODI_SW_CHIP_IRQ_ENABLE_OFF, 0x480U);
	odi_gpon_chip_irq_reset();
	CHECK(odi_mock.log_n == 3, "reset is three writes");
	CHECK(odi_mock.log[0].addr == ODI_SW_CHIP_IRQ_SETUP_OFF && odi_mock.log[0].val == 0,
	      "polarity high first");
	CHECK(odi_mock.log[1].addr == ODI_SW_CHIP_IRQ_ENABLE_OFF && odi_mock.log[1].val == 0,
	      "then every source masked");
	CHECK(odi_mock.log[2].addr == ODI_SW_CHIP_IRQ_PENDING_OFF && odi_mock.log[2].val == 0x7ffffU,
	      "then all 19 latched statuses cleared");
}

static void test_enable_keeps_acl(void)
{
	odi_mock_reset();
	set_reg(ODI_SW_CHIP_IRQ_ENABLE_OFF, ACL_BIT);	/* what the acl replay leaves */
	odi_gpon_chip_irq_enable();
	CHECK(get_reg(ODI_SW_CHIP_IRQ_ENABLE_OFF) == 0x480U,
	      "GPON unmasked next to ACL: 0x480, the value of a working stick");
}

static void test_demux_gpon(void)
{
	odi_mock_reset();
	gpon_calls = 0;
	set_reg(ODI_SW_CHIP_IRQ_ENABLE_OFF, GPON_BIT | ACL_BIT);
	set_reg(ODI_SW_CHIP_IRQ_PENDING_OFF, GPON_BIT);
	CHECK(odi_gpon_chip_irq_demux(gpon_handler) == GPON_BIT, "the GPON source is serviced");
	CHECK(gpon_calls == 1, "the GPON handler runs once");
	CHECK(writes_to(ODI_SW_CHIP_IRQ_PENDING_OFF) == 1 &&
	      get_reg(ODI_SW_CHIP_IRQ_PENDING_OFF) == GPON_BIT, "and its bit is acked");
}

/* The storm case: ACL enabled by the replay, pending, nothing services it. */
static void test_demux_acks_unserviced(void)
{
	odi_mock_reset();
	gpon_calls = 0;
	set_reg(ODI_SW_CHIP_IRQ_ENABLE_OFF, GPON_BIT | ACL_BIT);
	set_reg(ODI_SW_CHIP_IRQ_PENDING_OFF, ACL_BIT);
	CHECK(odi_gpon_chip_irq_demux(gpon_handler) == ACL_BIT, "the ACL source counts as serviced");
	CHECK(gpon_calls == 0, "the GPON handler does not run for it");
	CHECK(writes_to(ODI_SW_CHIP_IRQ_PENDING_OFF) == 1 &&
	      get_reg(ODI_SW_CHIP_IRQ_PENDING_OFF) == ACL_BIT,
	      "and it is acked, or the level-triggered line stays up");
}

static void test_demux_both_in_order(void)
{
	odi_mock_reset();
	gpon_calls = 0;
	set_reg(ODI_SW_CHIP_IRQ_ENABLE_OFF, GPON_BIT | ACL_BIT);
	set_reg(ODI_SW_CHIP_IRQ_PENDING_OFF, GPON_BIT | ACL_BIT);
	CHECK(odi_gpon_chip_irq_demux(gpon_handler) == (GPON_BIT | ACL_BIT), "both serviced");
	CHECK(gpon_calls == 1, "GPON handled once");
	CHECK(writes_to(ODI_SW_CHIP_IRQ_PENDING_OFF) == 2, "one ack per source");
}

static void test_demux_spurious_and_masked(void)
{
	odi_mock_reset();
	gpon_calls = 0;
	set_reg(ODI_SW_CHIP_IRQ_ENABLE_OFF, GPON_BIT);
	set_reg(ODI_SW_CHIP_IRQ_PENDING_OFF, 0);
	CHECK(odi_gpon_chip_irq_demux(gpon_handler) == 0, "nothing pending: spurious");
	set_reg(ODI_SW_CHIP_IRQ_PENDING_OFF, ACL_BIT);
	CHECK(odi_gpon_chip_irq_demux(gpon_handler) == 0, "pending but masked: not ours to ack");
	CHECK(gpon_calls == 0 && writes_to(ODI_SW_CHIP_IRQ_PENDING_OFF) == 0, "no handler, no ack");
}

int main(void)
{
	test_reset();
	test_enable_keeps_acl();
	test_demux_gpon();
	test_demux_acks_unserviced();
	test_demux_both_in_order();
	test_demux_spurious_and_masked();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_irq_test: ok\n");
	return 0;
}
