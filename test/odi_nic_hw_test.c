/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_nic_hw_test.c -- host-side unit test for odi_nic_hw.h. Compiled and
 * run with the host cc, no kernel and no target toolchain needed: the
 * header has no kernel dependency.
 *
 * Covers: RX/TX descriptor word layout (own bit position,
 * length masks), CPU-tag pack for a frame to a given port and unpack of a
 * received tag (src port, reason, VLAN fields), ring index arithmetic
 * (wrap, full, empty), the RX flow-control CPU index against a model
 * of the hardware comparator, idle and under a flood, and the
 * interrupt-storm guard.
 */
#include <stdio.h>
#include <string.h>
#include "../kernel/extra/drivers/net/ethernet/odi/odi_nic_hw.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static void test_rx_own_bit(void)
{
	struct odi_rx_desc d = {0};

	CHECK(ODI_RXD_HW == 0x80000000U, "own bit is bit 31");
	d.opts1 = ODI_RXD_HW;
	CHECK((d.opts1 & ODI_RXD_HW) != 0, "own bit set is readable back");
	d.opts1 = 0;
	CHECK((d.opts1 & ODI_RXD_HW) == 0, "own bit clear is readable back");
}

static void test_rx_len_mask(void)
{
	struct odi_rx_desc d = {0};

	/* 14-bit length field: max value 0x3FFF, a value one bit wider
	 * must not leak into the flag bits above it.
	 */
	d.opts1 = 0x7FFF; /* 15 bits all set */
	CHECK(odi_nic_rx_len(&d) == 0x3FFF, "14-bit length field masks correctly");
	CHECK((d.opts1 & ODI_RXD_BIT14) != 0, "bit 14 is distinct from the length field");
}

static void test_tx_own_bit_and_eor(void)
{
	struct odi_tx_desc d = {0};

	CHECK(ODI_TXD_HW == 0x80000000U, "TX own bit is bit 31");
	CHECK(ODI_TXD_WRAP == 0x40000000U, "TX eor bit is bit 30");
	d.opts1 = ODI_TXD_HW | ODI_TXD_WRAP;
	CHECK((d.opts1 & ODI_TXD_HW) && (d.opts1 & ODI_TXD_WRAP),
	      "own and eor coexist without overlap");
}

static void test_cpu_tag_pack_for_port(void)
{
	struct odi_tx_desc d = {0};
	unsigned vlan = 0x123; /* 12-bit VID, exercises the vidl/vidh split */

	odi_nic_tx_pack(&d, 0x04 /* port 2 as a single-bit mask */, vlan, 5, 1);

	CHECK(((d.opts3 >> ODI_TXD_PORTS_SHIFT) & ODI_TXD_PORTS_MASK) == 0x04,
	      "tx_portmask carries the requested destination port bit");
	CHECK((d.opts2 & ODI_TXD_TAGGED) != 0, "cputag bit is set for CPU-port TX");
	CHECK(((d.opts2 >> ODI_TXD_VID_LO_SHIFT) & ODI_TXD_VID_LO_MASK) == (vlan & 0xFF),
	      "vidl carries the low 8 bits of the VLAN id");
	CHECK((d.opts2 & ODI_TXD_VID_HI_MASK) == ((vlan >> 8) & 0xF),
	      "vidh carries the high 4 bits of the VLAN id");
	CHECK((d.opts1 & ODI_TXD_NO_LEARN) != 0, "dislrn requested is honoured");
	CHECK((d.opts1 & ODI_TXD_ADD_CRC) != 0, "crc generation is always requested");
}

static void test_cpu_tag_unpack_for_rx(void)
{
	struct odi_rx_desc d = {0};
	int valid;
	unsigned vlan;

	/* src_port_num = 3 (bits 31:27), reason = 0x11 (bits 19:12) */
	d.opts3 = (3U << ODI_RXD_SRC_PORT_SHIFT) | (0x11U << ODI_RXD_REASON_SHIFT);
	d.opts2 = ODI_RXD_CTAG | 0x0456;

	CHECK(odi_nic_rx_src_port(&d) == 3, "src_port_num unpacks to the switch port");
	CHECK(odi_nic_rx_reason(&d) == 0x11, "reason code unpacks correctly");
	vlan = odi_nic_rx_vlan(&d, &valid);
	CHECK(valid == 1, "ctagva marks the VLAN field valid");
	CHECK(vlan == 0x0456, "cvlan_tag value unpacks correctly");
}

static void test_ring_wrap_full_empty(void)
{
	unsigned depth = 4; /* 3 usable slots, one reserved to disambiguate full/empty */
	unsigned head = 0, tail = 0;

	CHECK(odi_ring_is_empty(head, tail), "fresh ring is empty");
	CHECK(!odi_ring_is_full(head, tail, depth), "fresh ring is not full");

	tail = odi_ring_next(tail, depth);
	CHECK(!odi_ring_is_empty(head, tail), "ring with one entry is not empty");

	tail = odi_ring_next(tail, depth);
	tail = odi_ring_next(tail, depth);
	CHECK(odi_ring_is_full(head, tail, depth), "ring fills after depth-1 inserts");
	CHECK(odi_ring_used(head, tail, depth) == depth - 1, "a full ring holds depth-1 entries");
	CHECK(odi_ring_used(2, 1, depth) == depth - 1, "used count survives tail wrapping past head");
	CHECK(odi_ring_used(1, 1, depth) == 0, "used count is 0 on an empty ring");

	/* wrap: advancing past the last index returns to 0 */
	CHECK(odi_ring_next(depth - 1, depth) == 0, "index wraps at the ring boundary");

	head = odi_ring_next(head, depth);
	CHECK(!odi_ring_is_full(head, tail, depth), "consuming one entry un-fulls the ring");
}

/* The comparator as the flow-control registers describe it: the hardware
 * counts the descriptors from its own index up to the CPU index as
 * available, asserts PAUSE at <= FC_ON and releases it at >= FC_OFF.
 * Walks the hardware index through `laps` laps with the CPU keeping up
 * (every descriptor handed back before the next frame), and returns how
 * many times PAUSE was asserted. `track` 0 is a CPU index written once at
 * init and never again.
 */
static unsigned int fc_asserts(int track, unsigned int init_idx, unsigned int laps)
{
	unsigned int depth = ODI_RX_RING_DEPTH, cpu = init_idx, hw, n, asserts = 0;
	int paused = 0;

	for (n = 0; n < laps * depth; n++) {
		unsigned int avail;

		hw = n % depth;			/* the next descriptor the engine fills */
		avail = (cpu + depth - hw) % depth;
		if (!paused && avail <= ODI_NIC_FC_ON) {
			paused = 1;
			asserts++;
		} else if (paused && avail >= ODI_NIC_FC_OFF) {
			paused = 0;
		}
		/* the frame lands in hw, the poll hands it back */
		if (track)
			cpu = odi_nic_rx_cpu_idx(odi_ring_next(hw, depth), depth);
	}
	return asserts;
}

static void test_rx_flow_control(void)
{
	unsigned int depth = ODI_RX_RING_DEPTH;

	CHECK(odi_nic_rx_cpu_idx(0, depth) == depth - 1, "a fresh ring hands every descriptor to the hardware");
	CHECK(odi_nic_rx_cpu_idx(1, depth) == 0, "after descriptor 0 comes back the index is 0");
	CHECK(odi_nic_rx_cpu_idx(depth - 1, depth) == depth - 2, "the index trails next-to-inspect by one");

	CHECK(ODI_NIC_FC_ON < ODI_NIC_FC_OFF, "PAUSE is released above where it is asserted");
	CHECK(ODI_NIC_FC_OFF < depth, "the release point is reachable in this ring");
	CHECK(depth <= 256 && ODI_NIC_FC_OFF <= 255, "index and thresholds fit their low byte");

	CHECK(fc_asserts(1, odi_nic_rx_cpu_idx(0, depth), 100) == 0,
	      "an index kept current never asserts PAUSE while the CPU keeps up");
	/* The bug this replaces: the index written once, as the depth. Once
	 * per lap, and once more when the first frame lands on it.
	 */
	CHECK(fc_asserts(0, depth, 100) >= 100 && fc_asserts(0, depth, 100) <= 101,
	      "a frozen index asserts PAUSE once per lap of an empty ring");
}

/* A flood, descriptor by descriptor: one frame arrives for every one the
 * poll takes (the CPU never gets ahead), DMA governed by own bits alone,
 * NAPI budget 64 (NAPI_POLL_WEIGHT) against a 64-deep ring. Records the
 * CPU index writes, either after every descriptor (per_desc) or once at
 * the end of each poll, and returns the smallest step between two writes.
 * At the end the flood stops, the last poll drains the ring, and *paused
 * says whether the comparator is still asserting PAUSE.
 */
static unsigned int flood(int per_desc, int *paused)
{
	enum { DEPTH = ODI_RX_RING_DEPTH, BUDGET = 64, POLLS = 200 };
	int own_hw[DEPTH], filled[DEPTH];
	unsigned int hw = 0, head = 0, cpu = odi_nic_rx_cpu_idx(0, DEPTH), i, p;
	unsigned int min_step = DEPTH, last = cpu, arrivals;
	int pause = 0;

	for (i = 0; i < DEPTH; i++) {
		own_hw[i] = 1;
		filled[i] = 0;
	}
#define WRITE_IDX(v) do { unsigned int s_ = ((v) + DEPTH - last) % DEPTH; \
		if (s_ < min_step) min_step = s_; last = (v); cpu = (v); } while (0)
#define FC() do { unsigned int a_ = (cpu + DEPTH - hw) % DEPTH; \
		if (!pause && a_ <= ODI_NIC_FC_ON) pause = 1; \
		else if (pause && a_ >= ODI_NIC_FC_OFF) pause = 0; } while (0)
	for (p = 0; p < POLLS + 1; p++) {
		arrivals = (p < POLLS) ? DEPTH : 0;
		/* the ring fills up before the poll runs */
		while (arrivals && own_hw[hw]) {
			own_hw[hw] = 0; filled[hw] = 1; hw = (hw + 1) % DEPTH; arrivals--; FC();
		}
		for (i = 0; i < BUDGET && filled[head]; i++) {
			filled[head] = 0; own_hw[head] = 1;
			head = (head + 1) % DEPTH;
			if (per_desc)
				WRITE_IDX(odi_nic_rx_cpu_idx(head, DEPTH));
			FC();
			/* and refills behind the poll */
			if (arrivals && own_hw[hw]) {
				own_hw[hw] = 0; filled[hw] = 1; hw = (hw + 1) % DEPTH; arrivals--; FC();
			}
		}
		if (!per_desc && i)
			WRITE_IDX(odi_nic_rx_cpu_idx(head, DEPTH));
		FC();
	}
#undef WRITE_IDX
#undef FC
	*paused = pause;
	return min_step;
}

static void test_rx_flow_control_flood(void)
{
	int paused;

	/* Once per poll, a poll that takes the whole budget writes the same
	 * byte as the poll before: the index did not move, to the hardware. */
	CHECK(flood(0, &paused) == 0, "per-poll writes: a full-budget poll repeats the last index");
	CHECK(flood(1, &paused) == 1, "per-descriptor writes: every write moves the index by exactly one");
	CHECK(!paused, "after the flood the comparator releases PAUSE");
}

/* The storm guard the flood tripped, and its replacement. The old policy
 * counted every interrupt entry; the new one only entries that schedule
 * no RX work (odi_irq_storm_note()).
 */
static int old_storm_note(struct odi_irq_storm *s, unsigned long now_ms)
{
	if (s->count == 0 || now_ms - s->window_start_ms >= ODI_IRQ_STORM_WINDOW_MS) {
		s->window_start_ms = now_ms;
		s->count = 0;
	}
	return ++s->count > ODI_IRQ_STORM_TRIP_COUNT;
}

static void test_irq_storm_guard(void)
{
	struct odi_irq_storm olds = {0}, news = {0}, stuck = {0};
	int old_trip = 0, new_trip = 0, stuck_trip = 0;
	unsigned long us;

	/* 5000 frames a second for 3 s, one interrupt each, every one of them
	 * scheduling the poll: the NAPI shape of a CPU that keeps up. */
	for (us = 0; us < 3000000UL; us += 200) {
		old_trip |= old_storm_note(&olds, us / 1000);
		new_trip |= odi_irq_storm_note(&news, us / 1000, 1);
	}
	CHECK(old_trip, "the old guard trips on a flood of legitimate interrupts (the test E wedge)");
	CHECK(!new_trip, "the new guard ignores interrupts that schedule the poll");

	/* A status bit that fires again as soon as it is acked, no work. */
	for (us = 0; us < 3000000UL; us += 100)
		stuck_trip |= odi_irq_storm_note(&stuck, us / 1000, 0);
	CHECK(stuck_trip, "an interrupt that does no work still trips it");

	/* A slow trickle of idle entries never does. */
	stuck.count = 0;
	stuck_trip = 0;
	for (us = 0; us < 60000000UL; us += 1000)
		stuck_trip |= odi_irq_storm_note(&stuck, us / 1000, 0);
	CHECK(!stuck_trip, "1000 idle entries a second stay under the trip count");
}

int main(void)
{
	test_rx_own_bit();
	test_rx_len_mask();
	test_tx_own_bit_and_eor();
	test_cpu_tag_pack_for_port();
	test_cpu_tag_unpack_for_rx();
	test_ring_wrap_full_empty();
	test_rx_flow_control();
	test_rx_flow_control_flood();
	test_irq_storm_guard();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_nic_hw_test: all checks passed\n");
	return 0;
}
