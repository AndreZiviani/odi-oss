/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_mmio_bounds_test.c -- unit test for
 * odi_switch_mmio_offset_in_bounds() itself (odi_switch_hw.h), plus a
 * regression check that the mock's own three sub-windows (switch-core,
 * GPON, PONQ_COUNT_MASK) all sit inside ODI_SWITCH_MMIO_SIZE -- the specific
 * assumption fixed after the s5 boot-kill (a write landing outside the
 * mapped window with nothing to catch it). Every other host test already
 * exercises this bound indirectly through test/odi_switch_mock.h's
 * odi_mock_slot() (every odi_reg_read()/odi_reg_write() call passes
 * through it); this file is the direct, minimal check of the bound
 * itself.
 */
#include <stdio.h>

#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

int main(void)
{
	CHECK(odi_switch_mmio_offset_in_bounds(0), "offset 0 is in bounds");
	CHECK(odi_switch_mmio_offset_in_bounds(ODI_SWITCH_MMIO_SIZE - 1),
	      "the last in-bounds offset (SIZE-1) is in bounds");
	CHECK(!odi_switch_mmio_offset_in_bounds(ODI_SWITCH_MMIO_SIZE),
	      "offset == SIZE is out of bounds (exclusive upper bound)");
	CHECK(!odi_switch_mmio_offset_in_bounds(0xFFFFFFFFU),
	      "a wildly out-of-range offset is out of bounds");

	/* The base itself: confirms this file (and everything else that
	 * includes odi_switch_hw.h) is building against the corrected
	 * physical base, not the old 0xB8000000 SoC-window constant that
	 * caused the s5 incident -- a silent revert of ODI_SWITCH_MMIO_BASE
	 * would not otherwise fail any host test, since the host mock never
	 * looks at the base itself, only at offsets.
	 */
	CHECK(ODI_SWITCH_MMIO_BASE == 0x1B000000UL,
	      "ODI_SWITCH_MMIO_BASE is the switch-core physical base (0x1B000000), "
	      "not the SoC-window constant (0xB8000000) that stalled the SoC in s5");

	/* Every sub-window this codebase's own mock models (odi_switch_mock.h)
	 * must fit inside the mapped size, or a host test could pass while
	 * addressing memory the real ioremap never covers.
	 */
	CHECK(odi_switch_mmio_offset_in_bounds(0x1FFFFFU), "switch-core window top is in bounds");
	CHECK(odi_switch_mmio_offset_in_bounds(0x70FFFFU), "GPON window top is in bounds");
	CHECK(odi_switch_mmio_offset_in_bounds(0xF00234U),
	      "PONQ_COUNT_MASK highest traced slot (+0x129*4) is in bounds");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_mmio_bounds_test: ok\n");
	return 0;
}
