/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_intr_test.c -- host-side unit test for odi_intr.c own dispatch loop
 * (odi_intr_dispatch_once()), against test/odi_switch_mock.h.
 *
 * Covers: a registered handler is called and its bit is W1C-acked; a
 * pending&enabled bit with NO registered handler is still W1C-acked (the
 * `acl` case -- odi_intr.c own ISR comment: this is what keeps an unfolded
 * type from storming the shared line); a pending-but-masked bit is left
 * untouched (not this switch's
 * problem to report -- it did not raise the line); a spurious pass (IMS &
 * IMR == 0) touches no register and counts as spurious, not dispatched;
 * odi_intr_enable()'s bit-position-equals-ordinal mapping against the real
 * ODI_SW_CHIP_IRQ_ENABLE_* field accessors (not a private assumption -- checked
 * against odi_switch_hw.h itself); and odi_intr_register()/_enable()'s
 * range check.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_intr.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_intr.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static unsigned int gpon_calls;
static void gpon_handler(void) { gpon_calls++; }

static void set_ims(uint32_t v) { odi_mock.regs[odi_mock_slot(ODI_SW_CHIP_IRQ_PENDING_OFF)] = v; }
static uint32_t get_ims(void) { return odi_mock.regs[odi_mock_slot(ODI_SW_CHIP_IRQ_PENDING_OFF)]; }

/* odi_switch_mock.h own odi_reg_write() is a plain store into a flat
 * register array -- it does not model the real CHIP_IRQ_PENDING hardware own
 * write-1-to-clear behaviour (odi_switch_hw.h own header comment: "sibling
 * to CHIP_IRQ_ENABLE ... write-1-to-clear per-type interrupt status"). So an ack
 * is checked here as "odi_intr_dispatch_once() issued a write of exactly
 * this bit to CHIP_IRQ_PENDING", against the write log, not by re-reading the
 * (unmodeled) post-write register content.
 */
static int log_has_write(uint32_t addr, uint32_t val)
{
	unsigned int i;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr &&
		    odi_mock.log[i].val == val)
			return 1;
	return 0;
}

/* odi_intr_enable()'s bit == ordinal mapping, checked against the real
 * odi_switch_hw.h field accessors, not just this file own assumption: the
 * GPON ordinal (10) must land exactly on CHIP_IRQ_ENABLE bit 10, ACL (7)
 * on bit 7, LINK_CHANGE (0) on bit 0 -- the three ordinals this codebase names
 * anywhere.
 */
static void test_bit_equals_ordinal(void)
{
	uint32_t reg;

	odi_mock_reset();
	odi_intr_test_reset();

	CHECK(odi_intr_enable(ODI_INTR_TYPE_GPON, 1) == 0, "enable(GPON) returns 0");
	reg = odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF);
	CHECK(ODI_SW_CHIP_IRQ_ENABLE_GPON_GET(reg) == 1, "enable(GPON, 1) sets CHIP_IRQ_ENABLE.GPON (bit 10)");
	CHECK(reg == (1U << 10), "enable(GPON, 1) alone leaves IMR == 0x400");

	CHECK(odi_intr_enable(ODI_INTR_TYPE_ACL_ACTION, 1) == 0, "enable(ACL) returns 0");
	reg = odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF);
	CHECK(ODI_SW_CHIP_IRQ_ENABLE_ACL_GET(reg) == 1, "enable(ACL, 1) sets CHIP_IRQ_ENABLE.ACL (bit 7)");
	CHECK(reg == ((1U << 10) | (1U << 7)), "IMR now carries both GPON and ACL bits, nothing else");

	CHECK(odi_intr_enable(ODI_INTR_TYPE_GPON, 0) == 0, "enable(GPON, 0) returns 0");
	reg = odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF);
	CHECK(ODI_SW_CHIP_IRQ_ENABLE_GPON_GET(reg) == 0, "enable(GPON, 0) clears CHIP_IRQ_ENABLE.GPON again");
	CHECK(reg == (1U << 7), "IMR now carries only the ACL bit");
}

/* The real boot ordering, reproduced: the `acl` sdkinit replay's last
 * event (sdkinit.bin) blindly overwrites the whole of IMR to
 * 0x00000080 (bit 7) -- simulated here with a raw odi_reg_write(), the
 * same "read-modify-write with a full mask" shape odi_switch_sdkinit_
 * apply() itself uses. That always runs, and completes, before
 * odi_gpon_irq_attach() ever calls odi_intr_enable(GPON, 1) from the
 * "gpondrv" pon-step (rootfs/skeleton/etc/init.d/rcS: the main /proc/
 * rtk_init loop that carries "acl" finishes entirely before the separate
 * pon-steps loop that carries "gpondrv" even starts). odi_intr_enable()
 * own read-back must see that bit and preserve it -- a blind write here
 * would silently erase acl's own enable. Final IMR: 0x00000480, the same
 * value already recorded as known-good on this board (bits 7 ACL + 10
 * GPON).
 */
static void test_acl_replay_bit_survives_gpon_enable(void)
{
	uint32_t reg;

	odi_mock_reset();
	odi_intr_test_reset();

	odi_reg_write(ODI_SW_CHIP_IRQ_ENABLE_OFF, 0x00000080U); /* acl sdkinit replay's own last event */
	CHECK(odi_intr_enable(ODI_INTR_TYPE_GPON, 1) == 0, "enable(GPON, 1) after the acl replay returns 0");

	reg = odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF);
	CHECK(reg == 0x00000480U, "IMR ends up 0x480 -- acl's bit 7 preserved, GPON's bit 10 added");
	CHECK(ODI_SW_CHIP_IRQ_ENABLE_ACL_GET(reg) == 1, "bit 7 (ACL) survived");
	CHECK(ODI_SW_CHIP_IRQ_ENABLE_GPON_GET(reg) == 1, "bit 10 (GPON) was added");
}

/* register()/enable() range check: a type >= ODI_INTR_TYPE_COUNT is
 * refused, touching neither the handler table nor IMR.
 */
static void test_range_check(void)
{
	odi_mock_reset();
	odi_intr_test_reset();

	CHECK(odi_intr_register(ODI_INTR_TYPE_COUNT, gpon_handler) == -EINVAL,
	      "register() refuses a type at the boundary");
	CHECK(odi_intr_enable(ODI_INTR_TYPE_COUNT, 1) == -EINVAL,
	      "enable() refuses a type at the boundary");
	CHECK(odi_mock.log_n == 0, "neither refusal touched a register");
}

/* The ordinary case: GPON registered and enabled, IMS shows it pending --
 * the handler runs exactly once and its bit is W1C-acked.
 */
static void test_registered_handler_dispatched_and_acked(void)
{
	odi_mock_reset();
	odi_intr_test_reset();
	gpon_calls = 0;

	CHECK(odi_intr_register(ODI_INTR_TYPE_GPON, gpon_handler) == 0, "register(GPON) returns 0");
	CHECK(odi_intr_enable(ODI_INTR_TYPE_GPON, 1) == 0, "enable(GPON, 1) returns 0");

	set_ims(1U << ODI_INTR_TYPE_GPON);
	CHECK(odi_intr_dispatch_once() == 1, "dispatch_once() reports serviced");
	CHECK(gpon_calls == 1, "the registered handler ran exactly once");
	CHECK(log_has_write(ODI_SW_CHIP_IRQ_PENDING_OFF, 1U << ODI_INTR_TYPE_GPON),
	      "the GPON bit was W1C-acked (a write of 1<<10 reached CHIP_IRQ_PENDING)");

	{
		struct odi_intr_status st;

		odi_intr_status_get(&st);
		CHECK(st.total_count == 1, "total_count is 1");
		CHECK(st.spurious_count == 0, "spurious_count is 0");
		CHECK(st.dispatched[ODI_INTR_TYPE_GPON] == 1, "dispatched[GPON] is 1");
		CHECK(st.unhandled[ODI_INTR_TYPE_GPON] == 0, "unhandled[GPON] is 0");
	}
}

/* The storm-prevention case (odi_intr.c own ISR comment): a type enabled
 * in hardware by something ELSE (standing in for the vendor `acl` step,
 * which still enables the ACL interrupt bit itself under
 * CONFIG_ODI_INTR) with no odi_intr_register() call for it at all. When
 * it goes pending, dispatch_once() must still W1C-ack it -- an un-acked
 * enabled&pending bit is a permanently-asserted shared line, the same
 * class of storm already hit once on the unmodified vendor path.
 */
static void test_unregistered_enabled_type_still_acked(void)
{
	odi_mock_reset();
	odi_intr_test_reset();

	CHECK(odi_intr_enable(ODI_INTR_TYPE_ACL_ACTION, 1) == 0, "enable(ACL, 1), standing in for the vendor acl step");
	set_ims(1U << ODI_INTR_TYPE_ACL_ACTION);

	CHECK(odi_intr_dispatch_once() == 1, "dispatch_once() still reports serviced");
	CHECK(log_has_write(ODI_SW_CHIP_IRQ_PENDING_OFF, 1U << ODI_INTR_TYPE_ACL_ACTION),
	      "the ACL bit was W1C-acked even with no handler registered");

	{
		struct odi_intr_status st;

		odi_intr_status_get(&st);
		CHECK(st.dispatched[ODI_INTR_TYPE_ACL_ACTION] == 0, "dispatched[ACL] stayed 0 -- no handler ran");
		CHECK(st.unhandled[ODI_INTR_TYPE_ACL_ACTION] == 1, "unhandled[ACL] is 1");
	}
}

/* A bit pending in IMS but masked (IMR clear) did not raise the shared
 * line -- dispatch_once() must leave it alone: no handler call, no W1C,
 * not counted as dispatched or unhandled.
 */
static void test_masked_pending_bit_untouched(void)
{
	odi_mock_reset();
	odi_intr_test_reset();
	gpon_calls = 0;

	/* IMR left at 0 (nothing enabled); IMS shows GPON pending anyway --
	 * e.g. a stale status bit some earlier, now-disabled, type left
	 * behind. */
	set_ims(1U << ODI_INTR_TYPE_GPON);

	CHECK(odi_intr_dispatch_once() == 0, "dispatch_once() reports spurious (nothing pending&enabled)");
	CHECK(gpon_calls == 0, "no handler ran (none is registered, and the bit is masked anyway)");
	CHECK(get_ims() == (1U << ODI_INTR_TYPE_GPON), "the masked bit was left exactly as it was -- no W1C");
	CHECK(odi_mock.log_n == 0, "no register write happened at all");
}

/* A genuinely empty pass: IMS & IMR == 0 entirely. Counts as spurious,
 * touches nothing.
 */
static void test_spurious_pass(void)
{
	struct odi_intr_status st;

	odi_mock_reset();
	odi_intr_test_reset();

	CHECK(odi_intr_dispatch_once() == 0, "dispatch_once() reports spurious");
	CHECK(odi_mock.log_n == 0, "an empty pass writes nothing");

	odi_intr_status_get(&st);
	CHECK(st.total_count == 0, "total_count stayed 0");
	CHECK(st.spurious_count == 1, "spurious_count is 1");
}

int main(void)
{
	test_bit_equals_ordinal();
	test_acl_replay_bit_survives_gpon_enable();
	test_range_check();
	test_registered_handler_dispatched_and_acked();
	test_unregistered_enabled_type_still_acked();
	test_masked_pending_bit_untouched();
	test_spurious_pass();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_intr_test: ok\n");
	return 0;
}
