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
 * of the hardware comparator, idle and under a flood, the
 * interrupt-storm guard, and the OMCI ring: the route words, where the ring
 * starts, the missed-frame counter, a model of a host flood with one ring
 * and with two, and the multi-ring start, enable and disable sequences
 * against a model of the six RX rings' flow-control comparators (with
 * MULTI_RX set a ring left at its reset values holds PAUSE) and of the
 * engine start (a ring the start did not load loses its first frame).
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

static void test_rx_whole(void)
{
	struct odi_rx_desc d = {0};

	/* The first descriptor seen on a stick: FIRST|LAST, 72 bytes. */
	d.opts1 = 0x36000048U;
	CHECK(odi_nic_rx_whole(&d), "a normal single-descriptor frame is whole");
	d.opts1 = ODI_RXD_FIRST | 0x640;
	CHECK(!odi_nic_rx_whole(&d), "first piece alone is not whole");
	d.opts1 = 0x600;
	CHECK(!odi_nic_rx_whole(&d), "middle piece is not whole");
	d.opts1 = ODI_RXD_LAST | 0x100;
	CHECK(!odi_nic_rx_whole(&d), "last piece alone is not whole");
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
static void write_idx(unsigned int *cpu, unsigned int *last, unsigned int *min_step,
		      unsigned int v)
{
	unsigned int step = (v + ODI_RX_RING_DEPTH - *last) % ODI_RX_RING_DEPTH;

	if (step < *min_step)
		*min_step = step;
	*last = v;
	*cpu = v;
}

static void fc(int *pause, unsigned int cpu, unsigned int hw)
{
	unsigned int avail = (cpu + ODI_RX_RING_DEPTH - hw) % ODI_RX_RING_DEPTH;

	if (!*pause && avail <= ODI_NIC_FC_ON)
		*pause = 1;
	else if (*pause && avail >= ODI_NIC_FC_OFF)
		*pause = 0;
}

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
	for (p = 0; p < POLLS + 1; p++) {
		arrivals = (p < POLLS) ? DEPTH : 0;
		/* the ring fills up before the poll runs */
		while (arrivals && own_hw[hw]) {
			own_hw[hw] = 0; filled[hw] = 1; hw = (hw + 1) % DEPTH; arrivals--; fc(&pause, cpu, hw);
		}
		for (i = 0; i < BUDGET && filled[head]; i++) {
			filled[head] = 0; own_hw[head] = 1;
			head = (head + 1) % DEPTH;
			if (per_desc)
				write_idx(&cpu, &last, &min_step, odi_nic_rx_cpu_idx(head, DEPTH));
			fc(&pause, cpu, hw);
			/* and refills behind the poll */
			if (arrivals && own_hw[hw]) {
				own_hw[hw] = 0; filled[hw] = 1; hw = (hw + 1) % DEPTH; arrivals--; fc(&pause, cpu, hw);
			}
		}
		if (!per_desc && i)
			write_idx(&cpu, &last, &min_step, odi_nic_rx_cpu_idx(head, DEPTH));
		fc(&pause, cpu, hw);
	}
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

/* TX completion in the poll: an entry that brings reclaim work is work for
 * the storm guard, one that finds nothing to reclaim is idle.
 */
static void test_tok_work(void)
{
	struct odi_irq_storm s = {0}, idle = {0};
	int trip = 0, idle_trip = 0;
	unsigned long us;

	CHECK(ODI_NIC_IRQ_TX_OK == 0x40, "TX completion is bit 6");
	CHECK(!(ODI_NIC_IRQ_TX_OK & ODI_NIC_IRQ_RX_SOURCES), "TX completion is not an RX source");
	CHECK(ODI_NIC_IRQ_POLL_SOURCES == (ODI_NIC_IRQ_RX_SOURCES | 0x40U), "the poll re-arms RX and TX completion");
	CHECK(odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 1), "TOK with a descriptor to reclaim is work");
	CHECK(!odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 0), "TOK with nothing to reclaim is not");
	CHECK(odi_nic_irq_has_work(ODI_NIC_IRQ_RX_OK, 0), "an RX source is work whatever the TX ring holds");
	CHECK(!odi_nic_irq_has_work(ODI_NIC_IRQ_TX_ERR, 1), "a source the poll does not own is not");

	/* 5000 TX completions a second for 3 s, each with descriptors to
	 * reclaim: the guard must stay quiet, as it does for RX. */
	for (us = 0; us < 3000000UL; us += 200)
		trip |= odi_irq_storm_note(&s, us / 1000,
					   odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 1));
	CHECK(!trip, "a flood of TOK that each bring reclaim work does not trip the guard");

	/* A TOK that re-fires with nothing to reclaim is a storm. */
	for (us = 0; us < 3000000UL; us += 100)
		idle_trip |= odi_irq_storm_note(&idle, us / 1000,
						odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 0));
	CHECK(idle_trip, "TOK with nothing to reclaim, over and over, still trips it");
}

/* A TOK with nothing to reclaim, while the transmit path keeps queueing
 * frames and reclaiming them first (a busy flood): never a storm. */
static void test_tok_idle_tx_progress(void)
{
	struct odi_irq_storm busy = {0}, stuck = {0}, mixed = {0};
	uint32_t seen_busy = 0, seen_stuck = 0, seen_mixed = 0, queued = 0;
	int busy_trip = 0, stuck_trip = 0, mixed_trip = 0;
	unsigned long us;

	CHECK(!odi_nic_tx_progress(&seen_busy, 0), "no frame queued: no progress");
	CHECK(odi_nic_tx_progress(&seen_busy, 1) && seen_busy == 1, "a frame queued: progress, and remembered");
	CHECK(!odi_nic_tx_progress(&seen_busy, 1), "the same count again: none");
	seen_busy = 0;

	/* 10000 idle TOK a second for 10 s, a frame queued between each. */
	for (us = 0; us < 10000000UL; us += 100) {
		queued++;
		busy_trip |= odi_irq_storm_note(&busy, us / 1000,
			odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 0) ||
			odi_nic_tx_progress(&seen_busy, queued));
	}
	CHECK(!busy_trip, "sustained idle TOK with TX progress never trips");

	/* The same entries with no frame queued at all. */
	for (us = 0; us < 3000000UL; us += 100)
		stuck_trip |= odi_irq_storm_note(&stuck, us / 1000,
			odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 0) ||
			odi_nic_tx_progress(&seen_stuck, 0));
	CHECK(stuck_trip, "idle TOK with no TX progress still trips");

	/* Traffic stops mid-run: the re-firing source is caught after it. */
	queued = 0;
	for (us = 0; us < 2000000UL; us += 100) {
		if (us < 1000000UL)
			queued++;
		mixed_trip |= odi_irq_storm_note(&mixed, us / 1000,
			odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 0) ||
			odi_nic_tx_progress(&seen_mixed, queued));
	}
	for (us = 2000000UL; us < 5000000UL; us += 100)
		mixed_trip |= odi_irq_storm_note(&mixed, us / 1000,
			odi_nic_irq_has_work(ODI_NIC_IRQ_TX_OK, 0) ||
			odi_nic_tx_progress(&seen_mixed, queued));
	CHECK(mixed_trip, "once TX stops, the re-firing TOK trips it");
}

/* The route words: only the two values measured on this chip, 0 (ring 1)
 * and 2 (the OMCI ring), ever appear in a field, and with the OMCI ring
 * on, priority 7 alone names it.
 */
static void test_rx_ring_map(void)
{
	uint32_t on = odi_nic_rx_ring_map(1), off = odi_nic_rx_ring_map(0);
	unsigned int p;

	CHECK(on == 0x20000000U, "omci ring on: the word another implementation measured");
	CHECK(off == 0x00000000U, "omci ring off: every field 0, every priority to ring 1");
	for (p = 0; p < 8; p++) {
		unsigned int f = (on >> (4 * p)) & 0xFU;

		CHECK(f == ODI_NIC_RX_MAP_RING1 || f == ODI_NIC_RX_MAP_OMCI_RING,
		      "no field names a ring that is not programmed");
		CHECK((f == ODI_NIC_RX_MAP_OMCI_RING) == (p == ODI_NIC_OMCI_PRIO),
		      "only the OMCI trap priority goes to the OMCI ring");
	}
	CHECK(ODI_NIC_RX_RING_MAP_WORDS == 6, "six route words, 0x1370 to 0x1384");
	CHECK(ODI_NIC_RUN1_VAL == 0x30010001U, "RUN1, one RX ring: the value it always had");
	CHECK(!(ODI_NIC_RUN1_VAL & ODI_NIC_RUN1_MULTI_RX), "RUN1, one RX ring: MULTI_RX clear");
	CHECK(ODI_NIC_RUN1_VAL_MULTI == 0x32030001U, "RUN1, multi-ring trial: MULTI_RX with the bitmap of rings 1 and 2");
	CHECK(ODI_NIC_RUN1_VAL_MULTI & ODI_NIC_RUN1_MULTI_RX, "RUN1, multi-ring: MULTI_RX set");
	CHECK(ODI_NIC_RX2_RING == 0x1390 && ODI_NIC_RX2_INDEX == 0x1394 && ODI_NIC_RX2_LAST == 0x1396 &&
	      ODI_NIC_RX2_CPU_IDX == 0x1398 && ODI_NIC_RX2_FC == 0x139C, "ring 2 block at 0x1390");
	CHECK(ODI_NIC_RXN_RING(6) == 0x13D0 && ODI_NIC_RXN_FC(6) + 4 <= ODI_NIC_RX1_RING,
	      "ring 6 block at 0x13D0, below ring 1");
	CHECK(ODI_NIC_RX_REASON_OMCI == 246, "the OMCI trap reason");
}

/* The OMCI ring starts where its engine index is, and hands back every
 * descriptor before it; an index beyond the ring is refused.
 */
static void test_rx2_start(void)
{
	unsigned int depth = ODI_RX2_RING_DEPTH, head = 99;

	CHECK(odi_nic_rx2_start(0, depth, &head) && head == 0, "index 0 (after a board reset): start at 0");
	CHECK(odi_nic_rx_cpu_idx(head, depth) == depth - 1, "... and every descriptor is the engine's");
	CHECK(odi_nic_rx2_start(8, depth, &head) && head == 8, "index 8 (after an interface restart): start at 8");
	CHECK(odi_nic_rx_cpu_idx(head, depth) == 7, "... and the CPU index is the one before it");
	CHECK(odi_nic_rx2_start(depth - 1, depth, &head) && head == depth - 1, "the last descriptor is inside");
	head = 5;
	CHECK(!odi_nic_rx2_start(depth, depth, &head) && head == 5, "an index at the depth is beyond the ring");
	CHECK(!odi_nic_rx2_start(0xFFFF, depth, &head) && head == 5, "a stale index of a longer ring is refused");

	CHECK(ODI_NIC_RX2_FC_ON < ODI_NIC_RX2_FC_OFF && ODI_NIC_RX2_FC_OFF < depth,
	      "OMCI ring PAUSE thresholds inside the ring");
	CHECK(ODI_NIC_RX2_FC_ON * ODI_RX_RING_DEPTH == ODI_NIC_FC_ON * depth &&
	      ODI_NIC_RX2_FC_OFF * ODI_RX_RING_DEPTH == ODI_NIC_FC_OFF * depth,
	      "the same share of the ring as ring 1");
	CHECK(ODI_NIC_RX2_FC_VAL(8, 24) == 0x00080018U, "ON in bits 23:16, OFF in bits 7:0");
	CHECK(ODI_NIC_RX2_FC_VAL(0x1FF, 0x1FF) == 0x00FF00FFU, "each threshold kept to its byte");
}

static void test_missed_delta(void)
{
	CHECK(odi_nic_u16_delta(10, 3) == 7, "missed-frame counter: plain difference");
	CHECK(odi_nic_u16_delta(5, 65530) == 11, "missed-frame counter: across the 16-bit wrap");
	CHECK(odi_nic_u16_delta(7, 7) == 0, "missed-frame counter: no change");
}

/* A host flood against OMCI, frame by frame: an engine with the two rings,
 * routing each frame by its priority through the route word, dropping one
 * that finds its ring with no free descriptor (no PAUSE: the worst case,
 * a switch that does not hold frames for the CPU), and a CPU that takes
 * `cpu` frames per tick, the OMCI ring first as odi_poll() does. Each tick
 * brings `flood` host frames (priority 0) and, every `every` ticks, one
 * OMCI frame (priority 7). Returns the OMCI frames lost.
 */
static unsigned int omci_lost_under_flood(int omci_ring, unsigned int flood,
					  unsigned int cpu, unsigned int every)
{
	enum { TICKS = 2000 };
	uint32_t map = odi_nic_rx_ring_map(omci_ring);
	unsigned int depth[2] = { ODI_RX_RING_DEPTH, ODI_RX2_RING_DEPTH };
	unsigned int used[2] = { 0, 0 };
	unsigned int t, i, lost = 0;

	for (t = 0; t < TICKS; t++) {
		unsigned int budget = cpu;

		for (i = 0; i <= flood; i++) {
			int is_omci = (i == flood);
			unsigned int prio = is_omci ? ODI_NIC_OMCI_PRIO : 0;
			unsigned int ring = ((map >> (4 * prio)) & 0xFU) == ODI_NIC_RX_MAP_OMCI_RING;

			if (is_omci && t % every)
				continue;
			/* one descriptor always kept back, as on hardware */
			if (used[ring] + 1 >= depth[ring]) {
				if (is_omci)
					lost++;
				continue;
			}
			used[ring]++;
		}
		/* the poll: the OMCI ring first, then ring 1 */
		while (budget && used[1]) {
			used[1]--;
			budget--;
		}
		while (budget && used[0]) {
			used[0]--;
			budget--;
		}
	}
	return lost;
}

static void test_omci_ring_under_flood(void)
{
	/* A flood ten times what the CPU takes, OMCI every 5 ticks. */
	CHECK(omci_lost_under_flood(0, 640, 64, 5) > 0, "one ring: a host flood costs OMCI frames");
	CHECK(omci_lost_under_flood(1, 640, 64, 5) == 0, "OMCI ring: none lost under the same flood");
	/* A registration burst: an OMCI frame every tick. */
	CHECK(omci_lost_under_flood(1, 640, 64, 1) == 0, "OMCI ring: a registration burst under a flood loses none");
	/* No flood: neither layout loses anything. */
	CHECK(omci_lost_under_flood(0, 10, 64, 1) == 0, "one ring, no flood: nothing lost");
}


/* ---- The six RX rings' flow-control comparators, and the start --------
 *
 * A register file of the ring registers, every one 0 after the block
 * reset, then ring 1 as odi_init_hw_rings() programs it, the engine
 * stopped; then whatever a sequence writes. The comparator of a ring:
 * PAUSE while the descriptors available, its CPU index minus the engine
 * index modulo the depth, are no more than its ON threshold. With MULTI_RX
 * clear only ring 1 is compared; with it set, every ring.
 *
 * The start, as odi_nic_hw.h takes it to be: when RUN starts the engine,
 * it loads the next descriptor of ring 1, and of rings 2 to 6 only with
 * MULTI_RX already set; a ring it has not loaded drops the first frame
 * routed to it (counted in RX_NO_DESC_CNT, the engine index unmoved) and
 * is loaded then. A ring written after it was loaded keeps what was
 * loaded until such a drop. Both orders a stick ran lose one frame in it,
 * as they did on 5 boots of 5: ring 2 programmed live with MULTI_RX, and
 * ring 2 programmed before a start with MULTI_RX clear. This is the
 * hypothesis the fix rests on, not a measured mechanism: the model shows
 * the sequences avoid it, the stick shows whether it holds.
 */
struct ringm {
	uint32_t base;
	unsigned int hw, last, cpu, on;
	uint32_t taken_base;	/* what the engine took in, 0 for nothing */
};

struct nicm {
	struct ringm r[7];	/* by ring number, 1 to 6 */
	uint32_t run1, run;
	uint32_t map[ODI_NIC_RX_RING_MAP_WORDS];
	unsigned int writes, missed;
	int bad_off;		/* a write to a register the model does not know */
};

static void nicm_reset(struct nicm *m)
{
	memset(m, 0, sizeof(*m));
	m->r[1].base = 0x01000000U;
	m->r[1].last = ODI_RX_RING_DEPTH - 1;
	m->r[1].cpu = odi_nic_rx_cpu_idx(0, ODI_RX_RING_DEPTH);
	m->r[1].on = ODI_NIC_FC_ON;
	for (unsigned int i = 0; i < ODI_NIC_RX_RING_MAP_WORDS; i++)
		m->map[i] = odi_nic_rx_ring_map(0);
}

static void nicm_apply(struct nicm *m, const struct odi_nic_regw *w, unsigned int n)
{
	for (unsigned int k = 0; k < n; k++, m->writes++) {
		unsigned int off = w[k].off, r, i;
		int known = 0;

		if (off == ODI_NIC_RUN1 && w[k].width == 32) {
			m->run1 = w[k].val;
			continue;
		}
		if (off == ODI_NIC_RUN && w[k].width == 32) {
			if (!m->run && w[k].val)
				for (r = 1; r <= 6; r++)
					if (r == 1 || (m->run1 & ODI_NIC_RUN1_MULTI_RX))
						m->r[r].taken_base = m->r[r].base;
			m->run = w[k].val;
			continue;
		}
		for (i = 0; i < ODI_NIC_RX_RING_MAP_WORDS; i++)
			if (off == ODI_NIC_RX_RING_MAP + 4 * i && w[k].width == 32) {
				m->map[i] = w[k].val;
				known = 1;
			}
		for (r = 2; r <= 6; r++) {
			if (off == ODI_NIC_RXN_RING(r) && w[k].width == 32) {
				m->r[r].base = w[k].val;
				known = 1;
			} else if (off == ODI_NIC_RXN_LAST(r) && w[k].width == 16) {
				m->r[r].last = w[k].val & 0xFFFFU;
				known = 1;
			} else if (off == ODI_NIC_RXN_CPU_IDX(r) && w[k].width == 32) {
				m->r[r].cpu = w[k].val & 0xFFFU;
				known = 1;
			} else if (off == ODI_NIC_RXN_FC(r) && w[k].width == 32) {
				m->r[r].on = (w[k].val >> 16) & 0xFFU;
				known = 1;
			} else if (off == ODI_NIC_RXN_INDEX(r)) {
				m->bad_off = 1;	/* the engine index is never written */
				known = 1;
			}
		}
		if (!known)
			m->bad_off = 1;
	}
}

static unsigned int ringm_avail(const struct ringm *g)
{
	unsigned int depth = g->last + 1;

	return (g->cpu + depth - g->hw % depth) % depth;
}

/* The first ring holding PAUSE, or 0 for none. */
static unsigned int nicm_pause(const struct nicm *m)
{
	unsigned int r, top = (m->run1 & ODI_NIC_RUN1_MULTI_RX) ? 6 : 1;

	for (r = 1; r <= top; r++)
		if (ringm_avail(&m->r[r]) <= m->r[r].on)
			return r;
	return 0;
}

/* One frame of internal priority prio, routed as the route word says with
 * MULTI_RX set and to ring 1 without; a CPU that keeps up hands the
 * descriptor straight back. Returns the ring it landed on, 0 if dropped.
 */
static unsigned int nicm_frame(struct nicm *m, unsigned int prio)
{
	unsigned int ring = 1, depth;
	struct ringm *g;

	if (!m->run)
		return 0;
	if ((m->run1 & ODI_NIC_RUN1_MULTI_RX) &&
	    ((m->map[0] >> (4 * prio)) & 0xFU) == ODI_NIC_RX_MAP_OMCI_RING)
		ring = 2;
	g = &m->r[ring];
	depth = g->last + 1;
	if (g->taken_base != g->base || !g->base || !ringm_avail(g)) {
		m->missed++;
		g->taken_base = g->base;
		return 0;
	}
	g->hw = (g->hw + 1) % depth;
	g->cpu = odi_nic_rx_cpu_idx(g->hw, depth);
	return ring;
}

static void multi_cfg(struct odi_nic_multi_rx_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->rx2_dma = 0x02000000U;
	c->rx2_depth = ODI_RX2_RING_DEPTH;
	c->park_dma = 0x03000000U;
}

static int regw_is_go(const struct odi_nic_regw *w)
{
	return w->off == ODI_NIC_RUN1 || w->off == ODI_NIC_RUN;
}

/* nicm_reset(), then the start odi_start_hw() writes. */
static unsigned int nicm_start(struct nicm *m, const struct odi_nic_multi_rx_cfg *c,
			       unsigned int *head, int *ok)
{
	struct odi_nic_regw w[ODI_NIC_RX_START_MAX];
	unsigned int n;

	nicm_reset(m);
	n = odi_nic_rx_start(c, w, head, ok);
	nicm_apply(m, w, n);
	return n;
}

static void nicm_on(struct nicm *m)
{
	struct odi_nic_regw w[ODI_NIC_MULTI_RX_MAX];

	nicm_apply(m, w, odi_nic_multi_rx_route(w, 1));
}

static void nicm_off(struct nicm *m)
{
	struct odi_nic_regw w[ODI_NIC_MULTI_RX_MAX];

	nicm_apply(m, w, odi_nic_multi_rx_route(w, 0));
}

/* The enables of the orders a stick ran: the route words with priority 7
 * to ring 2, then MULTI_RX, on a running engine.
 */
static void nicm_on_late(struct nicm *m)
{
	struct odi_nic_regw w[ODI_NIC_MULTI_RX_MAX + 1];
	unsigned int n = odi_nic_multi_rx_route(w, 1);

	n = odi_nic_regw_put(w, n, ODI_NIC_RUN1, 32, ODI_NIC_RUN1_VAL_MULTI);
	nicm_apply(m, w, n);
}

/* The OMCI frames of a burst that reach ring 2. */
static unsigned int omci_burst(struct nicm *m, unsigned int frames)
{
	unsigned int i, got = 0;

	for (i = 0; i < frames; i++)
		got += nicm_frame(m, ODI_NIC_OMCI_PRIO) == 2;
	return got;
}

static void test_multi_rx_reset_rings_hold_pause(void)
{
	struct odi_nic_regw w[ODI_NIC_RX_START_MAX];
	struct odi_nic_multi_rx_cfg c;
	struct nicm m;
	unsigned int n, head, k, kept = 0;
	int ok;

	nicm_reset(&m);
	CHECK(nicm_pause(&m) == 0, "one ring, rings 2..6 at reset: no PAUSE (MULTI_RX clear)");

	/* The start without the parking writes: ring 2 and MULTI_RX, the
	 * rest at reset. This is what took the management path down; the
	 * model must see it.
	 */
	multi_cfg(&c);
	n = odi_nic_rx_start(&c, w, &head, &ok);
	for (k = 0; k < n; k++) {
		unsigned int off = w[k].off;

		if (off >= ODI_NIC_RXN_BLOCK(ODI_NIC_RX_PARK_FIRST) &&
		    off < ODI_NIC_RXN_BLOCK(ODI_NIC_RX_PARK_LAST) + 0x10U)
			continue;
		w[kept++] = w[k];
	}
	nicm_apply(&m, w, kept);
	CHECK(m.run1 & ODI_NIC_RUN1_MULTI_RX, "unparked start: MULTI_RX set");
	CHECK(nicm_pause(&m) == ODI_NIC_RX_PARK_FIRST, "unparked start: ring 3, at reset, holds PAUSE");
}

static void test_multi_rx_start_sequence(void)
{
	struct odi_nic_regw w[ODI_NIC_RX_START_MAX];
	struct odi_nic_multi_rx_cfg c;
	struct nicm m;
	unsigned int n, head = 99, r, i;
	int ok = 0;

	multi_cfg(&c);
	n = odi_nic_rx_start(&c, w, &head, &ok);
	CHECK(ok && n == ODI_NIC_RX_START_MAX, "start: a sequence within its bound");
	CHECK(w[n - 2].off == ODI_NIC_RUN1 && w[n - 2].val == ODI_NIC_RUN1_VAL_MULTI &&
	      w[n - 1].off == ODI_NIC_RUN && w[n - 1].val == ODI_NIC_RUN_VAL,
	      "start: RUN1 with MULTI_RX, then RUN, last");
	for (i = 0; i + 2 < n; i++)
		CHECK(!regw_is_go(&w[i]), "start: every ring written before the go bits");

	nicm_start(&m, &c, &head, &ok);
	CHECK(!m.bad_off, "start: only known ring and go registers written");
	CHECK((m.run1 & ODI_NIC_RUN1_MULTI_RX) && m.run, "start: running, MULTI_RX set");
	CHECK(nicm_pause(&m) == 0, "start: no PAUSE");
	CHECK(head == 0 && ringm_avail(&m.r[2]) == ODI_RX2_RING_DEPTH - 1,
	      "start after a board reset: ring 2 from 0, every descriptor the engine's");
	CHECK(m.r[2].base == c.rx2_dma && m.r[2].last == ODI_RX2_RING_DEPTH - 1 &&
	      m.r[2].on == ODI_NIC_RX2_FC_ON, "start: ring 2 programmed");
	for (r = ODI_NIC_RX_PARK_FIRST; r <= ODI_NIC_RX_PARK_LAST; r++) {
		CHECK(m.r[r].base == c.park_dma && m.r[r].base % ODI_DESC_ALIGN == 0,
		      "start: rings 3..6 on the parking ring");
		CHECK(m.r[r].last == ODI_NIC_RX_PARK_DEPTH - 1 && m.r[r].on == 0 &&
		      ringm_avail(&m.r[r]) == 1, "start: a parked ring has one descriptor and ON 0");
	}
	for (i = 0; i < ODI_NIC_RX_RING_MAP_WORDS; i++)
		CHECK(m.map[i] == odi_nic_rx_ring_map(0), "start: the route words, every priority to ring 1, untouched");
	for (i = 0; i < 100; i++)
		CHECK(nicm_frame(&m, i % 8) == 1, "start, OMCI ring off: every frame on ring 1");
	CHECK(m.missed == 0, "start, OMCI ring off: nothing missed");
}

static void test_multi_rx_on_sequence(void)
{
	struct odi_nic_regw w[ODI_NIC_MULTI_RX_MAX];
	struct odi_nic_multi_rx_cfg c;
	struct nicm m;
	unsigned int n, head = 99, i, p;
	uint32_t run1;
	int ok;

	multi_cfg(&c);
	nicm_start(&m, &c, &head, &ok);
	run1 = m.run1;
	n = odi_nic_multi_rx_route(w, 1);
	CHECK(n == ODI_NIC_MULTI_RX_MAX, "enable: a sequence within its bound");
	for (i = 0; i < n; i++)
		CHECK(w[i].off >= ODI_NIC_RX_RING_MAP &&
		      w[i].off < ODI_NIC_RX_RING_MAP + 4 * ODI_NIC_RX_RING_MAP_WORDS,
		      "enable: the route words only, no ring register and not RUN1");
	nicm_apply(&m, w, n);
	CHECK(!m.bad_off && m.run1 == run1, "enable: only known route registers written");
	CHECK(nicm_pause(&m) == 0, "enable: no ring holds PAUSE");
	for (i = 0; i < ODI_NIC_RX_RING_MAP_WORDS; i++)
		for (p = 0; p < 8; p++) {
			unsigned int f = (m.map[i] >> (4 * p)) & 0xFU;

			CHECK(f == ((p == ODI_NIC_OMCI_PRIO) ? ODI_NIC_RX_MAP_OMCI_RING : ODI_NIC_RX_MAP_RING1),
			      "enable: every route word sends 7 to ring 2 and the rest to ring 1");
		}

	/* The engine takes 40 frames on ring 2 and the driver hands each
	 * back as odi_rx_cpu_idx_update() does: never PAUSE.
	 */
	for (i = 0; i < 40; i++) {
		CHECK(nicm_frame(&m, ODI_NIC_OMCI_PRIO) == 2, "enable: an OMCI frame on ring 2");
		CHECK(nicm_frame(&m, 0) == 1, "enable: a host frame on ring 1");
		CHECK(nicm_pause(&m) == 0, "ring 2 in use: no PAUSE while the CPU keeps up");
	}
}

/* The first OMCI frame after the OMCI ring is turned on: the engine
 * starts with MULTI_RX, so ring 2 is loaded before the first frame comes.
 */
static void test_omci_ring_first_frame(void)
{
	struct odi_nic_regw w[ODI_NIC_RX_START_MAX], go[2], rings[ODI_NIC_RX_START_MAX];
	struct odi_nic_multi_rx_cfg c;
	struct nicm m;
	unsigned int n, k, ngo = 0, nrings = 0, head;
	int ok;

	multi_cfg(&c);
	n = odi_nic_rx_start(&c, w, &head, &ok);
	for (k = 0; k < n; k++) {
		if (regw_is_go(&w[k]))
			go[ngo++] = w[k];
		else
			rings[nrings++] = w[k];
	}
	go[0].val = ODI_NIC_RUN1_VAL;	/* the go bits as before: MULTI_RX clear */

	/* The first order a stick ran: the go bits, then the rings, the
	 * route words and MULTI_RX live. The model loses the first OMCI
	 * frame of the burst, as the stick did, and none after.
	 */
	nicm_reset(&m);
	nicm_apply(&m, go, ngo);
	nicm_apply(&m, rings, nrings);
	nicm_on_late(&m);
	CHECK(nicm_pause(&m) == 0 && m.missed == 0, "rings programmed while running: no PAUSE, nothing missed yet");
	CHECK(omci_burst(&m, 20) == 19 && m.missed == 1, "rings programmed while running: the first OMCI frame is lost");
	CHECK(omci_burst(&m, 98) == 98 && m.missed == 1, "... and none of the next burst");

	/* The second: the rings before the go bits, MULTI_RX clear in them,
	 * set live with the route words. The model loses it too, as the
	 * stick did.
	 */
	nicm_reset(&m);
	nicm_apply(&m, rings, nrings);
	nicm_apply(&m, go, ngo);
	nicm_on_late(&m);
	CHECK(omci_burst(&m, 20) == 19 && m.missed == 1, "rings before the start, MULTI_RX late: the first OMCI frame is lost");

	/* Fresh boot, the OMCI ring turned on live later. */
	multi_cfg(&c);
	nicm_start(&m, &c, &head, &ok);
	CHECK(omci_burst(&m, 20) == 0 && m.missed == 0, "OMCI ring off: OMCI on ring 1, not ring 2");
	nicm_on(&m);
	CHECK(omci_burst(&m, 20) == 20 && m.missed == 0, "live enable: every OMCI frame of the first burst on ring 2");
	nicm_off(&m);
	CHECK(omci_burst(&m, 20) == 0 && m.missed == 0, "off again: OMCI back on ring 1, none lost");
	nicm_on(&m);
	CHECK(omci_burst(&m, 98) == 98 && m.missed == 0, "on again: none lost");

	/* odi_nic.omci_ring=1 at boot: the enable right after the start. */
	multi_cfg(&c);
	nicm_start(&m, &c, &head, &ok);
	nicm_on(&m);
	CHECK(omci_burst(&m, 20) == 20 && m.missed == 0, "enabled at open: every OMCI frame on ring 2");

	/* After an interface restart ring 2 starts at its engine index. */
	multi_cfg(&c);
	c.hw_idx[2] = 19;
	nicm_reset(&m);
	m.r[2].hw = 19;
	n = odi_nic_rx_start(&c, w, &head, &ok);
	nicm_apply(&m, w, n);
	nicm_on(&m);
	CHECK(head == 19 && omci_burst(&m, 40) == 40 && m.missed == 0,
	      "after a restart, from engine index 19 across the wrap: none lost");
}

static void test_multi_rx_engine_indices(void)
{
	struct odi_nic_regw w[ODI_NIC_RX_START_MAX];
	struct odi_nic_multi_rx_cfg c;
	struct nicm m;
	unsigned int n, head = 99;
	int ok = 0;

	/* After an interface restart: ring 2 at 8, ring 4 at 1. */
	nicm_reset(&m);
	multi_cfg(&c);
	c.hw_idx[2] = 8;
	c.hw_idx[4] = 1;
	m.r[2].hw = 8;
	m.r[4].hw = 1;
	n = odi_nic_rx_start(&c, w, &head, &ok);
	nicm_apply(&m, w, n);
	nicm_on(&m);
	CHECK(ok && head == 8, "ring 2 starts at its engine index");
	CHECK(m.r[2].cpu == 7, "ring 2 CPU index one before it");
	CHECK(m.r[4].cpu == 0 && ringm_avail(&m.r[4]) == 1, "a parked ring at engine index 1: CPU index 0");
	CHECK(nicm_pause(&m) == 0, "no PAUSE whatever the engine indices");

	multi_cfg(&c);
	c.hw_idx[2] = ODI_RX2_RING_DEPTH;
	head = 99;
	n = odi_nic_rx_start(&c, w, &head, &ok);
	CHECK(!ok && head == 99, "ring 2 index beyond the ring: refused");
	CHECK(n == 2 && regw_is_go(&w[0]) && regw_is_go(&w[1]), "... and only the go bits written");
	CHECK(w[0].val == ODI_NIC_RUN1_VAL, "... with MULTI_RX clear: one ring, as before the OMCI ring");
	multi_cfg(&c);
	c.hw_idx[6] = ODI_NIC_RX_PARK_DEPTH;
	n = odi_nic_rx_start(&c, w, &head, &ok);
	CHECK(!ok && n == 2, "a parked ring's index beyond two: refused");
}

static void test_multi_rx_off_sequence(void)
{
	struct odi_nic_regw w[ODI_NIC_MULTI_RX_MAX];
	struct odi_nic_multi_rx_cfg c;
	struct nicm m;
	unsigned int n, head, i, p;
	int ok;

	multi_cfg(&c);
	nicm_start(&m, &c, &head, &ok);
	nicm_on(&m);
	n = odi_nic_multi_rx_route(w, 0);
	CHECK(n == ODI_NIC_MULTI_RX_MAX, "disable: the six route words");
	nicm_apply(&m, w, n);
	CHECK((m.run1 & ODI_NIC_RUN1_MULTI_RX) && nicm_pause(&m) == 0, "disable: MULTI_RX stays, no PAUSE");
	for (i = 0; i < ODI_NIC_RX_RING_MAP_WORDS; i++)
		for (p = 0; p < 8; p++)
			CHECK(((m.map[i] >> (4 * p)) & 0xFU) == ODI_NIC_RX_MAP_RING1,
			      "disable: every priority back to ring 1");
	for (p = 0; p < 8; p++)
		CHECK(nicm_frame(&m, p) == 1, "disable: a frame of any priority on ring 1");

	/* The value written while MULTI_RX was clear would, with it set,
	 * send priorities 2 to 6 to rings that are parked.
	 */
	CHECK(((ODI_NIC_RX_RING_MAP_SINGLE >> (4 * 3)) & 0xFU) != ODI_NIC_RX_MAP_RING1,
	      "the old single-ring word is not an all-ring-1 word");

	/* And on again, from the rings as the start left them. */
	nicm_on(&m);
	CHECK((m.run1 & ODI_NIC_RUN1_MULTI_RX) && nicm_pause(&m) == 0, "on, off, on: no PAUSE");
}

int main(void)
{
	test_rx_own_bit();
	test_rx_whole();
	test_rx_len_mask();
	test_tx_own_bit_and_eor();
	test_cpu_tag_pack_for_port();
	test_cpu_tag_unpack_for_rx();
	test_ring_wrap_full_empty();
	test_rx_flow_control();
	test_rx_flow_control_flood();
	test_irq_storm_guard();
	test_tok_work();
	test_tok_idle_tx_progress();
	test_rx_ring_map();
	test_rx2_start();
	test_missed_delta();
	test_omci_ring_under_flood();
	test_multi_rx_reset_rings_hold_pause();
	test_multi_rx_start_sequence();
	test_multi_rx_on_sequence();
	test_omci_ring_first_frame();
	test_multi_rx_engine_indices();
	test_multi_rx_off_sequence();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_nic_hw_test: all checks passed\n");
	return 0;
}
