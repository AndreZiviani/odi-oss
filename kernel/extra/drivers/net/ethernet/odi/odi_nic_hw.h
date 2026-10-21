/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_nic_hw.h -- register offsets, descriptor layout and CPU-tag pack and
 * unpack for the RTL9602C CPU-port NIC: a from-scratch statement of the
 * register map and descriptor bit layout, as hardware facts.
 *
 * Plain C with fixed-width types, no kernel dependency, so odi_nic_hw_test.c
 * (test/odi_nic_hw_test.c in the odi-oss repo) can compile and run it with
 * the host cc, unmodified, before it is ever built for the target.
 */
#ifndef ODI_NIC_HW_H
#define ODI_NIC_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

/* ---- MMIO window ------------------------------------------------------ */

#define ODI_NIC_MMIO_BASE	0xB8012000UL	/* KSEG1, fixed */
#define ODI_NIC_MMIO_SIZE	0x2000UL	/* 8 KiB, generous over the observed 0x1438 span */

/* SoC block-enable register: separate MMIO window, only one register of
 * it matters to this driver (the NIC block reset).
 */
#define ODI_SOC_BLOCK_EN		0xB8000600UL
#define ODI_SOC_BLOCK_EN_NIC		(1U << 1)

/* ---- Registers, offsets relative to ODI_NIC_MMIO_BASE ---------------- */

#define ODI_NIC_STATION0		0x00	/* station MAC, bytes 0-3 (32-bit) */
#define ODI_NIC_STATION4		0x04	/* station MAC, bytes 4-5 in the high 16 bits (32-bit) */
#define ODI_NIC_MCHASH0		0x08	/* multicast hash table, 64 bits (two 32-bit words) */

#define ODI_NIC_CTRL		0x38	/* 8-bit access at offset 0x3B within this word */
#define ODI_NIC_CTRL_RX_CSUM	(1U << 1)	/* bit 1, not bit 0 as first assumed (n8) */
#define ODI_NIC_CTRL_RX_JUMBO		(1U << 3)

#define ODI_NIC_IRQ_MASK		0x3C	/* 16-bit */
#define ODI_NIC_IRQ_STATUS		0x3E	/* 16-bit, write-1-clear */
#define ODI_NIC_IRQ_RX_OK		(1U << 0)
#define ODI_NIC_IRQ_RX_RUNT	(1U << 2)
#define ODI_NIC_IRQ_RX_FIFO_FULL		(1U << 4)	/* RX FIFO overflow: n7/n8 saw it on every interrupt */
#define ODI_NIC_IRQ_RX_NO_DESC		(1U << 5)	/* ring 1 descriptor unavailable */
/* TX completion: a descriptor went back to the CPU. Same bit position in
 * the mask and the status register. Whether the hardware raises it for every
 * frame with the IO_CMD value we program is judged from the tok_irqs counter
 * in /proc/odi_nic, not assumed: odi_nic.c reclaims without it all the same.
 */
#define ODI_NIC_IRQ_TX_OK		(1U << 6)
#define ODI_NIC_IRQ_TX_ERR		(1U << 7)
#define ODI_NIC_IRQ_TX_NO_DESC		(1U << 9)
#define ODI_NIC_IRQ_SOFT		(1U << 10)
#define ODI_NIC_IRQ_LINK	(1U << 8)	/* defined, not the real link source */
#define ODI_NIC_IRQ_ALL	0xFFFFU
/* The sources odi_irq() hands to the poll, and the mask the poll re-arms:
 * the RX sources, and TX completion for the reclaim.
 */
#define ODI_NIC_IRQ_RX_SOURCES	(ODI_NIC_IRQ_RX_OK | ODI_NIC_IRQ_RX_RUNT | \
				 ODI_NIC_IRQ_RX_FIFO_FULL | ODI_NIC_IRQ_RX_NO_DESC)
#define ODI_NIC_IRQ_POLL_SOURCES	(ODI_NIC_IRQ_RX_SOURCES | ODI_NIC_IRQ_TX_OK)

/* Whether an interrupt entry brings the poll work. An RX source always
 * does. A TX completion does only while a descriptor is waiting to be
 * reclaimed: one the reclaim in ndo_start_xmit already took is an entry
 * with nothing to do, and the storm guard counts it as such. A TOK that has
 * descriptors to reclaim is work, never a storm.
 */
static inline int odi_nic_irq_has_work(uint32_t isr, int tx_reclaimable)
{
	return (isr & ODI_NIC_IRQ_RX_SOURCES) ||
	       ((isr & ODI_NIC_IRQ_TX_OK) && tx_reclaimable);
}

#define ODI_NIC_XFER_STATUS		0x34	/* 32-bit, aggregate TX/RX status */

#define ODI_NIC_TX_CFG		0x40	/* 32-bit */
#define ODI_NIC_TX_CFG_VAL		0x0C00U

#define ODI_NIC_MCHASH4		0x0C	/* second half of the multicast hash */
#define ODI_NIC_RX_FILTER		0x44	/* 32-bit RX accept mode; the reset value accepts nothing */
#define ODI_NIC_RX_FILTER_ANY	(1U << 0)
#define ODI_NIC_RX_FILTER_OWN	(1U << 1)
#define ODI_NIC_RX_FILTER_MCAST	(1U << 2)
#define ODI_NIC_RX_FILTER_BCAST	(1U << 3)
#define ODI_NIC_RX_FILTER_ALL	(ODI_NIC_RX_FILTER_ANY | ODI_NIC_RX_FILTER_OWN | \
				 ODI_NIC_RX_FILTER_MCAST | ODI_NIC_RX_FILTER_BCAST)

#define ODI_NIC_TAG_CTRL	0x48	/* 32-bit, CPU-tag engine control */
#define ODI_NIC_TAG_RX_ON	(1U << 31)
/* TX_SIZE and RX_SIZE are 2-bit size codes at bit 27 and bit 16, not flags; the
 * stock firmware has code 2 in both, confirmed against the live stock
 * register (0x901eff04).
 */
#define ODI_NIC_TAG_TX_SIZE(c)	((u32)(c) << 27)
#define ODI_NIC_TAG_RX_SIZE(c)	((u32)(c) << 16)
#define ODI_NIC_TAG_MODE7	(7U << 18)	/* bits 20:18 = 7, as the stock register reads */
#define ODI_NIC_TAG_PMASK	(0xFFU << 8)	/* bits 15:8, as the stock register reads */
#define ODI_NIC_TAG_PVER	(0x04U << 0)	/* bits 7:0, as the stock register reads */
#define ODI_NIC_TAG_CTRL_ON	(ODI_NIC_TAG_RX_ON | ODI_NIC_TAG_TX_SIZE(2) | \
				 ODI_NIC_TAG_RX_SIZE(2) | ODI_NIC_TAG_MODE7 | \
				 ODI_NIC_TAG_PMASK | ODI_NIC_TAG_PVER)

#define ODI_NIC_FIFO_CFG		0x4C	/* 32-bit */
#define ODI_NIC_FIFO_RX_2K	(2U << 28)	/* RX FIFO size select, 2 KiB */
#define ODI_NIC_FIFO_RING_IRQS	(1U << 24)	/* interrupt split, multi-ring RX */

#define ODI_NIC_TAG_CTRL1	0x50	/* 32-bit */
#define ODI_NIC_TAG1_SID0	64U
#define ODI_NIC_TAG1_VAL(vdsl_port, pon_port, sid) \
	((((uint32_t)(vdsl_port) & 0xF) << 4) | ((uint32_t)(pon_port) & 0xF) | \
	 (((uint32_t)(sid) & 0xFFFFFU) << 8))

/* The MAC status register. Its high byte (bits 31:24) is the byte at 0x58 on
 * this big-endian target, and the driver reads and writes it as a byte.
 * Bits 7:5 are control, bits 4:0 are status and read-only. Decode
 * cross-checked against an independent driver for a later chip of the same
 * family; values unchanged. The byte reads 0xf0 on a stick running this
 * driver (bits 7:4 set, 3:0 clear), which fits the decode below.
 */
#define ODI_NIC_MSR		0x58	/* 8-bit access */
#define ODI_NIC_MSR_FORCE_FC	(1U << 7)	/* force flow control */
#define ODI_NIC_MSR_RX_FC_EN	(1U << 6)	/* RX flow control enable */
#define ODI_NIC_MSR_TX_FC_EN	(1U << 5)	/* TX flow control enable */
#define ODI_NIC_MSR_SPEED_1000	(1U << 4)	/* status: 1000 Mb/s */
#define ODI_NIC_MSR_SPEED_10	(1U << 3)	/* status: 10 Mb/s */
/* Status: link. It reads clear on a running stick whose CPU port carries
 * traffic, so what it tracks on this chip is unconfirmed; nothing here
 * reads it.
 */
#define ODI_NIC_MSR_LINK	(1U << 2)
#define ODI_NIC_MSR_TX_PAUSE_ST	(1U << 1)	/* status: TX pause */
#define ODI_NIC_MSR_RX_PAUSE_ST	(1U << 0)	/* status: RX pause */

/* The three flow-control controls the driver sets together, as the stock
 * firmware has them. The write is a read-modify-write of the byte, so the
 * status bits are written back as read.
 */
#define ODI_NIC_MSR_FC_ON	(ODI_NIC_MSR_FORCE_FC | ODI_NIC_MSR_RX_FC_EN | ODI_NIC_MSR_TX_FC_EN)
typedef char odi_nic_msr_fc_on_is_0xe0[(ODI_NIC_MSR_FC_ON == 0xE0U) ? 1 : -1];

#define ODI_NIC_RING_IRQ_MASK		0xD0	/* multi-ring RX mask, 32-bit */
#define ODI_NIC_RING_IRQ_STATUS		0xD8	/* multi-ring, write-1-clear */

/* Ring-control registers. Our build uses TX ring 1, RX ring 1 and the
 * OMCI ring (RX ring 2) -- no dual-band, no multi-WAN.
 */
#define ODI_NIC_TX1_RING		0x1300
#define ODI_NIC_TX1_INDEX		0x1304

/* The priority-to-ring route register at 0x1370: one field per internal
 * priority, eight of them, each a ring number in its own 4-bit slot,
 * priority n at bits 4n+3:4n. Not one word per ring. The words that follow
 * it, up to 0x1384, are written with the same value; what they are is not
 * decoded, and writing them the same keeps every field of every word naming
 * a programmed ring. The engine consults them only with RUN1 MULTI_RX set:
 * with it clear every frame lands in ring 1 whatever they say.
 * Cross-checked against an independent driver for a later chip of the same
 * family (the register and its purpose).
 */
#define ODI_NIC_RX_RING_MAP	0x1370	/* the route register; six words are written, step 4 */
#define ODI_NIC_RX_RING_MAP_WORDS	6U
/* The single-ring value: priorities 0 and 1 to field 0, 2..7 to 1..6, as
 * the driver has always written it with MULTI_RX clear, where it has no
 * effect. Kept so that the single-ring register state is unchanged.
 */
#define ODI_NIC_RX_RING_MAP_SINGLE	0x65432100U

/* Which ring a field names. 0 is ring 1 (RX1_RING) and 2 the OMCI ring
 * (RX2_RING); another open implementation's measurements on this chip:
 * with priority 7 set to 2 and every other field 0, each of the 421 OMCI
 * messages of a registration arrived on the ring at 0x1390 and every other
 * frame on the one at 0x13F0, and with every field 0 all of them on the
 * one ring. What the other values name is not established, so with
 * MULTI_RX set no field is ever written with one: a field naming a ring
 * that is not programmed would have the engine write wherever that ring's
 * base points.
 */
#define ODI_NIC_RX_MAP_RING1		0U
#define ODI_NIC_RX_MAP_OMCI_RING	2U
#define ODI_NIC_RX_MAP_FIELD(prio, ring)	((uint32_t)(ring) << (4U * (prio)))

/* The internal priority the switch gives the OMCI frames it traps to the
 * CPU: the sdkinit replay writes 7 to the OMCI trap configuration
 * (0x0111f8, bits 2:0), as the stock firmware does, and nothing else
 * changes it.
 */
#define ODI_NIC_OMCI_PRIO		7U

/* The route words: OMCI to its own ring (with MULTI_RX set), or the
 * single-ring value (with it clear).
 */
static inline uint32_t odi_nic_rx_ring_map(int omci_ring)
{
	return omci_ring ? ODI_NIC_RX_MAP_FIELD(ODI_NIC_OMCI_PRIO, ODI_NIC_RX_MAP_OMCI_RING) :
			   ODI_NIC_RX_RING_MAP_SINGLE;
}

/* RX rings 2 to 6 each have a register block of 16 bytes, ring 2 at 0x1390
 * up to ring 6 at 0x13D0 (ring 1 has its own layout, at 0x13F0 and 0x1430
 * below). Facts from another open implementation's measurements on this
 * chip, for ring 2: the base takes the bus address of descriptor 0; LAST
 * (16-bit) the depth minus one; CPU_IDX the index of the last descriptor
 * handed back, written as a whole word (the ring-1 counterpart is one
 * byte); and the engine index (16-bit) cannot be written: it keeps the
 * value it had when the engine stopped, across a restart of the interface,
 * and reads 0 after a board reset. The driver therefore starts a ring where
 * the engine is (odi_nic_rx2_start()). A frame on ring 2 raises the same
 * IRQ_RX_OK as one on ring 1 while FIFO_CFG RING_IRQS is clear. Rings 3 to
 * 6 are taken to have the same block (not measured).
 *
 * FC holds the ring's own PAUSE thresholds, taken to be ON in bits 23:16
 * and OFF in bits 7:0 (not established), compared like ring 1's: PAUSE
 * at no more than ON descriptors available (CPU_IDX minus the engine
 * index, modulo the depth). Ring 2's are written at the share of the ring
 * that ring 1's 16/48 are of 64, so it asserts PAUSE only with three
 * quarters of it waiting for the CPU.
 */
#define ODI_NIC_RXN_BLOCK(n)		(0x1390U + 0x10U * ((unsigned int)(n) - 2U))
#define ODI_NIC_RXN_RING(n)		(ODI_NIC_RXN_BLOCK(n) + 0x0U)
#define ODI_NIC_RXN_INDEX(n)		(ODI_NIC_RXN_BLOCK(n) + 0x4U)	/* 16-bit, read-only in effect */
#define ODI_NIC_RXN_LAST(n)		(ODI_NIC_RXN_BLOCK(n) + 0x6U)	/* 16-bit, depth - 1 */
#define ODI_NIC_RXN_CPU_IDX(n)		(ODI_NIC_RXN_BLOCK(n) + 0x8U)	/* 32-bit */
#define ODI_NIC_RXN_FC(n)		(ODI_NIC_RXN_BLOCK(n) + 0xCU)
#define ODI_NIC_RX2_RING		ODI_NIC_RXN_RING(2)
#define ODI_NIC_RX2_INDEX		ODI_NIC_RXN_INDEX(2)
#define ODI_NIC_RX2_LAST		ODI_NIC_RXN_LAST(2)
#define ODI_NIC_RX2_CPU_IDX		ODI_NIC_RXN_CPU_IDX(2)
#define ODI_NIC_RX2_FC			ODI_NIC_RXN_FC(2)
#define ODI_NIC_RX2_FC_VAL(on, off)	((((uint32_t)(on) & 0xFFU) << 16) | ((uint32_t)(off) & 0xFFU))

/* The rings the driver does not use with MULTI_RX set, 3 to 6. */
#define ODI_NIC_RX_PARK_FIRST		3U
#define ODI_NIC_RX_PARK_LAST		6U
#define ODI_NIC_RX_PARK_NUM		(ODI_NIC_RX_PARK_LAST - ODI_NIC_RX_PARK_FIRST + 1U)

/* Where the OMCI ring starts. The engine index cannot be moved, so the
 * driver's next descriptor to inspect is the engine's own; returns 0 when
 * the index is beyond the ring (only a longer ring left by earlier software
 * can leave one), and the OMCI ring must then not be routed to, since the
 * engine would write past it.
 */
static inline int odi_nic_rx2_start(unsigned int hw_idx, unsigned int depth,
				    unsigned int *head)
{
	if (hw_idx >= depth)
		return 0;
	*head = hw_idx;
	return 1;
}

/* RX_NO_DESC_CNT (16-bit, free running at 0x2A): frames the engine dropped
 * for want of a free descriptor, on any ring. Another open implementation's
 * measurements on this chip: it advanced by the frames the switch handed
 * the CPU port minus the frames the driver took, modulo 65536, and wraps in
 * under a second under a flood, so the driver folds the 16-bit difference
 * in at every poll.
 */
#define ODI_NIC_RX_NO_DESC_CNT		0x2A

static inline uint32_t odi_nic_u16_delta(uint16_t now, uint16_t last)
{
	return (uint16_t)(now - last);
}

#define ODI_NIC_RX1_RING		0x13F0
#define ODI_NIC_RX1_INDEX		0x13F4
/* RX ring 1 depth, expressed as (depth - 1), split low byte / high nibble
 * because the field is wider than one byte lane. Left at its reset value,
 * the hardware does not know where the ring ends. Not the same thing as
 * RX1_CPU_IDX below, which also starts at depth - 1 but then moves.
 */
#define ODI_NIC_RX1_LAST	0x13F6	/* low byte of (depth - 1) */
#define ODI_NIC_RX1_LAST_HI	0x13F7	/* high nibble, bits 8:11 of (depth - 1) */
#define ODI_NIC_R13FC		0x13FC

/* RX ring 1 flow control. The NIC asserts PAUSE toward the switch CPU
 * port (with ODI_NIC_MSR_TX_FC_EN on) when the descriptors
 * available to it -- from its own position up to the last one the CPU
 * handed back -- fall to FC_ON, and releases it at FC_OFF. The CPU index is
 * the driver's to keep current, like a tail doorbell: the hardware does not
 * learn it from the own bits. Left at its init value, the hardware index
 * comes within FC_ON of it once per lap of the ring and PAUSE is asserted
 * with the ring empty. The live stock register (word 0x96103000, RXCDO
 * index 152 in the same read) shows the index two behind the hardware and
 * thresholds 16/48.
 *
 * RX1_CPU_IDX is bits 7:0 of the index; bits 11:8 are the high nibble of
 * the byte at 0x1433, whose low nibble holds bits 11:8 of FC_ON (bits 11:8
 * of FC_OFF are elsewhere). With ODI_RX_RING_DEPTH and both thresholds
 * below 256 every high nibble is 0, the reset value, and none is written.
 */
#define ODI_NIC_RX1_CPU_IDX	0x1430	/* last RX descriptor handed back, bits 7:0 */
#define ODI_NIC_FC_ON_LEVEL	0x1431	/* assert PAUSE at <= this many available */
#define ODI_NIC_FC_OFF_LEVEL	0x1432	/* release PAUSE at >= this many available */

/* A margin in descriptors, not a fraction of the ring: what it has to
 * cover is the frames that still arrive between the assert and the switch
 * acting on it. These are the stock values; with the CPU index kept
 * current they fire only with 48 of 64 descriptors waiting for the CPU.
 */
#define ODI_NIC_FC_ON		16U
#define ODI_NIC_FC_OFF		48U

/* The CPU index after the descriptor before next_to_inspect was handed
 * back: the one just returned, or depth - 1 (all of them) on a fresh ring.
 */
static inline unsigned int odi_nic_rx_cpu_idx(unsigned int next_to_inspect, unsigned int depth)
{
	return next_to_inspect ? next_to_inspect - 1 : depth - 1;
}

#define ODI_NIC_RUN		0x1434	/* master go/doorbell register, 32-bit */
#define ODI_NIC_RUN_TX_KICK	(1U << 0)	/* ring 1 doorbell -- the only ring we use */
/* The full start word, not merely the packet timer field (timer = 15,
 * bits 11:8). This is the value the register holds on a stick running
 * the stock firmware, read one register at a time, and ours reads back
 * the same. Writing the timer field alone left RX and TX disabled: full
 * open, zero interrupts, no traffic at all. Bit meanings beyond the
 * timer field are not decoded here.
 */
#define ODI_NIC_RUN_VAL	0xC059F130U

#define ODI_NIC_RUN1		0x1438
/* Descriptor format (bits 29:28) and TX ring 1 on the high queue (bit 0)
 * in both values. With one RX ring: 0x30010001, which the driver has always
 * written (bit 16 is taken to select RX ring 1 in a ring bitmap; it reads
 * back 0). With more: bit 25 (MULTI_RX) set and the bitmap bits clear,
 * 0x32000001, the value the register reads on a stick running the stock
 * firmware (which runs three RX rings) and the value another open
 * implementation writes to run the same two rings as this driver. Bit
 * meanings beyond these fields are not decoded here.
 */
#define ODI_NIC_RUN1_MULTI_RX	(1U << 25)
#define ODI_NIC_RUN1_VAL	0x30010001U
#define ODI_NIC_RUN1_VAL_MULTI	0x32000001U

/* ---- Interrupt-storm guard -------------------------------------------
 *
 * The signature of a storm is an interrupt that does no work: a status bit
 * that is acked and fires again, or the line firing while the poll is
 * already scheduled with the RX sources masked. An entry that schedules the
 * poll is never one: with NAPI and a CPU that keeps up, every received frame
 * is its own interrupt, so a flood of thousands of frames a second is
 * thousands of legitimate interrupts a second. Counting those tripped the
 * guard and left RX off for good. `now_ms` is any millisecond clock; the
 * window restarts once it has run out.
 */
#define ODI_IRQ_STORM_WINDOW_MS		2000U
#define ODI_IRQ_STORM_TRIP_COUNT	5000U

struct odi_irq_storm {
	unsigned long window_start_ms;
	unsigned int count;	/* idle entries in the current window, 0 = no window */
};

/* Returns 1 when this entry trips the guard. */
static inline int odi_irq_storm_note(struct odi_irq_storm *s, unsigned long now_ms,
				      int scheduled_work)
{
	if (scheduled_work)
		return 0;
	if (s->count == 0 || now_ms - s->window_start_ms >= ODI_IRQ_STORM_WINDOW_MS) {
		s->window_start_ms = now_ms;
		s->count = 0;
	}
	return ++s->count > ODI_IRQ_STORM_TRIP_COUNT;
}

/* A TX-completion entry that finds nothing to reclaim is not a storm while
 * frames were queued since the last such entry: the transmit path reclaimed
 * the descriptor before the interrupt ran, which is what a busy single
 * stream does thousands of times a second. `seen` is the caller's copy of
 * the queued-frame counter at the last such entry; returns 1 (and updates
 * it) when the counter advanced. A TOK that re-fires with no TX activity at
 * all returns 0 and stays an idle entry for the guard.
 */
static inline int odi_nic_tx_progress(uint32_t *seen, uint32_t queued_now)
{
	if (queued_now == *seen)
		return 0;
	*seen = queued_now;
	return 1;
}

/* ---- Descriptor rings -------------------------------------------------- */

#define ODI_DESC_ALIGN		256U	/* ring base must be 256-byte aligned */
#define ODI_RX_RING_DEPTH	64U	/* memory-budget default */
/* The OMCI ring. A registration brings some 420 OMCI messages in a few
 * seconds and the OLT then sends one every few seconds; a ring of 32,
 * served first by every poll, took 442 of 442 during a registration under
 * a host flood in another open implementation's measurements on this chip.
 */
#define ODI_RX2_RING_DEPTH	32U
#define ODI_NIC_RX2_FC_ON	(ODI_RX2_RING_DEPTH / 4U)
#define ODI_NIC_RX2_FC_OFF	(ODI_RX2_RING_DEPTH * 3U / 4U)
#define ODI_TX_RING_DEPTH	32U
#define ODI_NIC_RX_BUF_SIZE	1600U	/* memory-budget default */
/* The DMA engine starts every frame at buffer + 2 (4N+2, seen in trial
 * n10 as a 2-byte prefix before the destination MAC) and reports the
 * length with the 4-byte FCS included (64 for a minimum frame). The
 * buffer is allocated and mapped 2 bytes longer than the size handed off.
 */
#define ODI_NIC_RX_OFFSET	2U
#define ODI_NIC_RX_ALLOC	(ODI_NIC_RX_BUF_SIZE + ODI_NIC_RX_OFFSET)

/* RX descriptor: 4 x 32-bit words = 16 bytes. */
struct odi_rx_desc {
	uint32_t opts1;
	uint32_t addr;
	uint32_t opts2;
	uint32_t opts3;
};

/* TX descriptor: 5 x 32-bit words = 20 bytes; opts4 (LSO) is
 * always zero in our build (no hardware LSO).
 */
struct odi_tx_desc {
	uint32_t opts1;
	uint32_t addr;
	uint32_t opts2;
	uint32_t opts3;
	uint32_t opts4;
};

/* RX opts1 (word 0) */
#define ODI_RXD_HW		(1U << 31)
#define ODI_RXD_WRAP		(1U << 30)
#define ODI_RXD_FIRST		(1U << 29)
#define ODI_RXD_LAST		(1U << 28)
#define ODI_RXD_BAD_CRC		(1U << 27)
#define ODI_RXD_BAD_IPCSUM		(1U << 26)
#define ODI_RXD_BAD_L4CSUM		(1U << 25)
#define ODI_RXD_BIT24		(1U << 24)
#define ODI_RXD_FRAGMENT		(1U << 23)
#define ODI_RXD_PPPOE	(1U << 22)
#define ODI_RXD_TOO_LONG		(1U << 21)
#define ODI_RXD_PROTO_SHIFT	17
#define ODI_RXD_PROTO_MASK	0xFU
#define ODI_RXD_ROUTED	(1U << 16)
#define ODI_RXD_BIT15	(1U << 15)
#define ODI_RXD_BIT14		(1U << 14)
#define ODI_RXD_SIZE_MASK	0x3FFFU		/* 14-bit, jumbo build */
/* On hand-off to the hardware the same field carries the BUFFER size; a
 * descriptor given with 0 here takes one frame and then the FIFO overflows
 * (trials n7, n8). An earlier revision described the field for the return
 * path only.
 */
#define ODI_RXD_HANDOFF(eor)	(ODI_RXD_HW | (eor) | (ODI_NIC_RX_BUF_SIZE & ODI_RXD_SIZE_MASK))

/* RX opts2 (word 2) */
#define ODI_RXD_TAGGED		(1U << 31)
#define ODI_RXD_PTP	(1U << 30)
#define ODI_RXD_STAG	(1U << 29)
#define ODI_RXD_GEM_SHIFT 20
#define ODI_RXD_GEM_MASK	0x7FU
#define ODI_RXD_CTAG		(1U << 16)
#define ODI_RXD_CTAG_MASK	0xFFFFU

/* RX opts3 (word 3) */
/* opts3 carries, among other fields, source port 4 bits (31-28) and reason
 * 8 bits (20-13) -- the two this driver actually reads. An earlier revision
 * had a 5-bit source port at 27 and the reason at 12 (trials n10 to n14
 * mislabelled ports 0 and 2 as 1 and 4).
 *
 * A destination-port-mask field and an internal-priority field were also
 * named here once (shift 20/mask 0x7F, shift 9/mask 0x7), but they were
 * never read by this driver, and their shifts do not agree with each other
 * (20 overlaps the reason field's own top bit) -- removed rather than kept
 * wrong; re-derive both against a capture before adding them back.
 */
#define ODI_RXD_SRC_PORT_SHIFT	28
#define ODI_RXD_SRC_PORT_MASK	0xFU
#define ODI_RXD_REASON_SHIFT	13
#define ODI_RXD_REASON_MASK	0xFFU

/* TX opts1 (word 0) */
#define ODI_TXD_HW		(1U << 31)
#define ODI_TXD_WRAP		(1U << 30)
#define ODI_TXD_FIRST		(1U << 29)
#define ODI_TXD_LAST		(1U << 28)
#define ODI_TXD_IP_CSUM		(1U << 27)
#define ODI_TXD_L4_CSUM		(1U << 26)
#define ODI_TXD_BIT25		(1U << 25)
#define ODI_TXD_ADD_CRC		(1U << 23)
#define ODI_TXD_BIT22		(1U << 22)
#define ODI_TXD_NO_LEARN		(1U << 21)
#define ODI_TXD_PORT_SEL	(1U << 18)
#define ODI_TXD_SIZE_MASK	0x1FFFFU	/* 17-bit, jumbo-capable */

/* TX opts2 (word 2) */
#define ODI_TXD_TAGGED		(1U << 31)
#define ODI_TXD_SET_PRI		(1U << 30)
#define ODI_TXD_TPRI_SHIFT	27
#define ODI_TXD_TPRI_MASK	0x7U
#define ODI_TXD_VLAN_OP_SHIFT 25
#define ODI_TXD_BIT19		(1U << 19)
#define ODI_TXD_VLAN_OP_MASK  0x3U
#define ODI_TXD_VLAN_OP_NONE	0U
#define ODI_TXD_VLAN_OP_ADD	1U
#define ODI_TXD_VLAN_OP_DEL	2U
#define ODI_TXD_VLAN_OP_SET	3U
#define ODI_TXD_VID_LO_SHIFT	8
#define ODI_TXD_VID_LO_MASK	0xFFU
#define ODI_TXD_PCP_SHIFT	5
#define ODI_TXD_PCP_MASK	0x7U
#define ODI_TXD_DEI		(1U << 4)
#define ODI_TXD_VID_HI_MASK	0xFU

/* TX opts3 (word 3) */
#define ODI_TXD_PORTS_SHIFT	23
#define ODI_TXD_PORTS_MASK	0x3FU
#define ODI_TXD_GEM_SHIFT	16
#define ODI_TXD_GEM_MASK	0x7FU

/* ---- CPU-tag pack / unpack ---------------------------------------------
 *
 * The tag has no standalone wire struct on this link -- the switch's
 * CPU-tag engine reads and writes these fields directly in the
 * descriptor's opts2/opts3 words. Packing for TX and unpacking for RX are
 * therefore descriptor-field helpers, not a byte-buffer codec.
 */

static inline void odi_nic_tx_pack(struct odi_tx_desc *d, unsigned int port_mask,
				    unsigned int vlan_id, unsigned int prio, int dislrn)
{
	d->opts2 = ODI_TXD_TAGGED |
		   ((prio & ODI_TXD_TPRI_MASK) << ODI_TXD_TPRI_SHIFT) |
		   (ODI_TXD_VLAN_OP_NONE << ODI_TXD_VLAN_OP_SHIFT) |
		   ((vlan_id & 0xFF) << ODI_TXD_VID_LO_SHIFT) |
		   ((prio & ODI_TXD_PCP_MASK) << ODI_TXD_PCP_SHIFT) |
		   ((vlan_id >> 8) & ODI_TXD_VID_HI_MASK);
	d->opts3 = (port_mask & ODI_TXD_PORTS_MASK) << ODI_TXD_PORTS_SHIFT;
	d->opts1 = ODI_TXD_ADD_CRC;
	if (dislrn)
		d->opts1 |= ODI_TXD_NO_LEARN;
}

/* A frame that fits its buffer arrives in one descriptor with FIRST and
 * LAST both set (opts1 0x36000048 on the first frame logged on a stick, and
 * another open implementation's measurements agree); a frame longer than
 * the buffer arrives as several, FIRST alone, then none, then LAST alone,
 * and only the first carries the CPU tag. A piece without both has no source
 * port, so it must never be taken for a frame of its own.
 */
static inline int odi_nic_rx_whole(const struct odi_rx_desc *d)
{
	return (d->opts1 & (ODI_RXD_FIRST | ODI_RXD_LAST)) ==
	       (ODI_RXD_FIRST | ODI_RXD_LAST);
}

static inline unsigned int odi_nic_rx_src_port(const struct odi_rx_desc *d)
{
	return (d->opts3 >> ODI_RXD_SRC_PORT_SHIFT) & ODI_RXD_SRC_PORT_MASK;
}

/* The switch port frames from the fibre enter by. The only frames from it
 * the CPU wants are OMCI, which odi_omci.c takes as an RX hook (reason
 * 246); odi_poll() drops every other one before any net_device sees it,
 * so nothing from the fibre reaches the IP stack
 * (docs/SWITCH.md#pon-port-isolation).
 */
#define ODI_NIC_PON_PORT	2U

static inline int odi_nic_rx_from_pon(const struct odi_rx_desc *d)
{
	return odi_nic_rx_src_port(d) == ODI_NIC_PON_PORT;
}

/* ---- TX from the IP stack ----------------------------------------------
 *
 * Which switch port a net_device stands for (its RX demux and its TX). The
 * catch-all is eth0, which receives what no other device claims; NONE is
 * nas0 and pon0, which the rootfs scripts name and nothing here wires to
 * a port.
 */
#define ODI_NIC_DEV_PORT_LAN0		0U	/* eth0.2: the host port */
#define ODI_NIC_DEV_PORT_LAN1		1U	/* eth0.3: port 1, not wired on this board */
#define ODI_NIC_DEV_PORT_NONE		0xFEU	/* nas0, pon0 */
#define ODI_NIC_DEV_PORT_CATCHALL	0xFFU	/* eth0 */

#define ODI_NIC_CPU_PORT		3U

/* The destination port mask of a frame from the IP stack, by the device it
 * leaves on: the host-side ports only, so the stick's own ARP, broadcasts
 * and replies are never addressed to the fibre
 * (docs/SWITCH.md#pon-port-isolation). Never the PON port and never the
 * CPU port; never 0 for a device that transmits, because a zero mask is
 * the hardware lookup, which floods a broadcast to every port, the PON
 * included. 0 means "do not send" (nas0, pon0, anything unknown).
 * PORT_SEL is never set on these frames: it is what puts a frame onto a
 * PON stream (the OMCI replies, odi_omci.c), not what makes the mask
 * binding.
 */
static inline unsigned int odi_nic_stack_tx_mask(unsigned int dev_port)
{
	switch (dev_port) {
	case ODI_NIC_DEV_PORT_LAN0:
		return 1U << ODI_NIC_DEV_PORT_LAN0;
	case ODI_NIC_DEV_PORT_LAN1:
		return 1U << ODI_NIC_DEV_PORT_LAN1;
	case ODI_NIC_DEV_PORT_CATCHALL:
		return (1U << ODI_NIC_DEV_PORT_LAN0) | (1U << ODI_NIC_DEV_PORT_LAN1);
	default:
		return 0;
	}
}

/* The RX reason of an OMCI frame trapped to the CPU (odi_omci.c keys its
 * hook on it).
 */
#define ODI_NIC_RX_REASON_OMCI		246U

static inline unsigned int odi_nic_rx_reason(const struct odi_rx_desc *d)
{
	return (d->opts3 >> ODI_RXD_REASON_SHIFT) & ODI_RXD_REASON_MASK;
}

static inline unsigned int odi_nic_rx_vlan(const struct odi_rx_desc *d, int *valid)
{
	*valid = (d->opts2 & ODI_RXD_CTAG) ? 1 : 0;
	return d->opts2 & ODI_RXD_CTAG_MASK;
}

static inline unsigned int odi_nic_rx_len(const struct odi_rx_desc *d)
{
	return d->opts1 & ODI_RXD_SIZE_MASK;
}

/* ---- Ring index arithmetic --------------------------------------------
 *
 * Software bookkeeping only (which slot to fill/inspect next); the real
 * hardware/software handoff is each descriptor's own `own` bit. One slot
 * is kept empty so `full` and `empty` are distinguishable
 * without a separate counter -- the standard two-pointer convention.
 */

static inline unsigned int odi_ring_next(unsigned int idx, unsigned int depth)
{
	unsigned int n = idx + 1;
	return (n == depth) ? 0 : n;
}

static inline int odi_ring_is_empty(unsigned int head, unsigned int tail)
{
	return head == tail;
}

static inline int odi_ring_is_full(unsigned int head, unsigned int tail, unsigned int depth)
{
	return odi_ring_next(tail, depth) == head;
}

/* Entries between head (oldest in flight) and tail (next to fill). */
static inline unsigned int odi_ring_used(unsigned int head, unsigned int tail,
					 unsigned int depth)
{
	return (tail >= head) ? tail - head : tail + depth - head;
}

/* TX ring size (32 * 20 = 640 B) is not a power of two, unlike RX, so it has
 * no natural alignment guarantee from allocator rounding. Checked against
 * both the DMA and virtual address after
 * dma_alloc_coherent(), since either could be non-aligned in principle.
 */
static inline int odi_desc_aligned(const void *virt, uint32_t dma_addr)
{
	return !((uintptr_t)virt % ODI_DESC_ALIGN) && !(dma_addr % ODI_DESC_ALIGN);
}

/* ---- Multi-ring RX: the enable and disable sequences --------------------
 *
 * With RUN1 MULTI_RX set the engine runs the flow-control comparator of
 * every RX ring, 1 to 6, not only of ring 1, and asserts PAUSE toward the
 * switch for any of them. A ring left at its reset values (base 0, LAST 0,
 * CPU_IDX 0, thresholds 0) has no descriptor available, which is no more
 * than its ON threshold of 0: PAUSE held for good, whatever rings 1 and 2
 * hold. Another open implementation's measurements on this chip, which run
 * MULTI_RX with rings 3 to 6 unprogrammed: with the flow-control bits of
 * MSR set, a counter of the switch CPU port climbed with no traffic and
 * nothing was transmitted; with them clear (as it runs), transmit worked.
 * This driver keeps flow control on, so before MULTI_RX is set every ring
 * it does not use is parked: pointed at a two-descriptor ring of its own
 * that the CPU owns (the engine never writes it), CPU_IDX one behind the
 * engine index, so one descriptor is always available, and both thresholds
 * 0, so the comparator can never reach PAUSE. Nothing is ever routed to a
 * parked ring.
 *
 * The sequences are lists of register writes, in order, which the driver
 * performs one by one (odi_nic.c, odi_rx_multi_apply()) and the host test
 * runs against a model of the six comparators. Enable: park rings 3 to 6,
 * program ring 2, write the route words, and set MULTI_RX last, so the
 * engine sees no ring before it is complete. Disable: clear MULTI_RX first,
 * then the single-ring route words; the rings stay as they are, unused.
 */
#define ODI_NIC_RX_PARK_DEPTH		2U
/* The parking ring's allocation: the descriptors at 0, the buffer they
 * point at from ODI_NIC_PARK_BUF_OFF, as large as any RX buffer.
 */
#define ODI_NIC_PARK_BUF_OFF		256U
#define ODI_NIC_PARK_ALLOC		(ODI_NIC_PARK_BUF_OFF + 2048U)

struct odi_nic_regw {
	uint16_t off;
	uint8_t width;		/* 8, 16 or 32 */
	uint32_t val;
};

/* What the enable needs to know: the bus addresses of ring 2 and of the
 * parking ring, ring 2's depth, and the engine index of rings 2 to 6 as
 * read just before.
 */
struct odi_nic_multi_rx_cfg {
	uint32_t rx2_dma;
	unsigned int rx2_depth;
	uint32_t park_dma;
	unsigned int hw_idx[ODI_NIC_RX_PARK_LAST + 1];	/* by ring number, 2 to 6 used */
};

#define ODI_NIC_MULTI_RX_ON_MAX		(4U * ODI_NIC_RX_PARK_NUM + 4U + ODI_NIC_RX_RING_MAP_WORDS + 1U)
#define ODI_NIC_MULTI_RX_OFF_MAX	(1U + ODI_NIC_RX_RING_MAP_WORDS)

static inline unsigned int odi_nic_regw_put(struct odi_nic_regw *w, unsigned int n,
					    uint16_t off, uint8_t width, uint32_t val)
{
	w[n].off = off;
	w[n].width = width;
	w[n].val = val;
	return n + 1;
}

/* Fills w (ODI_NIC_MULTI_RX_ON_MAX entries) and *rx2_head, the driver's
 * next descriptor to inspect on ring 2; returns the number of writes, or 0
 * with nothing to write when an engine index is beyond its ring (only a
 * longer ring left by earlier software can leave one): the engine would
 * then walk past the ring, so MULTI_RX stays clear.
 */
static inline unsigned int odi_nic_multi_rx_on(const struct odi_nic_multi_rx_cfg *c,
					       struct odi_nic_regw *w, unsigned int *rx2_head)
{
	unsigned int n = 0, r, i, head;

	for (r = ODI_NIC_RX_PARK_FIRST; r <= ODI_NIC_RX_PARK_LAST; r++)
		if (c->hw_idx[r] >= ODI_NIC_RX_PARK_DEPTH)
			return 0;
	if (!odi_nic_rx2_start(c->hw_idx[2], c->rx2_depth, &head))
		return 0;

	for (r = ODI_NIC_RX_PARK_FIRST; r <= ODI_NIC_RX_PARK_LAST; r++) {
		n = odi_nic_regw_put(w, n, ODI_NIC_RXN_FC(r), 32, ODI_NIC_RX2_FC_VAL(0, 0));
		n = odi_nic_regw_put(w, n, ODI_NIC_RXN_RING(r), 32, c->park_dma);
		n = odi_nic_regw_put(w, n, ODI_NIC_RXN_LAST(r), 16, ODI_NIC_RX_PARK_DEPTH - 1U);
		n = odi_nic_regw_put(w, n, ODI_NIC_RXN_CPU_IDX(r), 32,
				     odi_nic_rx_cpu_idx(c->hw_idx[r], ODI_NIC_RX_PARK_DEPTH));
	}

	n = odi_nic_regw_put(w, n, ODI_NIC_RX2_RING, 32, c->rx2_dma);
	n = odi_nic_regw_put(w, n, ODI_NIC_RX2_LAST, 16, c->rx2_depth - 1U);
	n = odi_nic_regw_put(w, n, ODI_NIC_RX2_CPU_IDX, 32, odi_nic_rx_cpu_idx(head, c->rx2_depth));
	n = odi_nic_regw_put(w, n, ODI_NIC_RX2_FC, 32,
			     ODI_NIC_RX2_FC_VAL(ODI_NIC_RX2_FC_ON, ODI_NIC_RX2_FC_OFF));

	for (i = 0; i < ODI_NIC_RX_RING_MAP_WORDS; i++)
		n = odi_nic_regw_put(w, n, ODI_NIC_RX_RING_MAP + 4U * i, 32, odi_nic_rx_ring_map(1));

	n = odi_nic_regw_put(w, n, ODI_NIC_RUN1, 32, ODI_NIC_RUN1_VAL_MULTI);
	*rx2_head = head;
	return n;
}

/* Fills w (ODI_NIC_MULTI_RX_OFF_MAX entries); returns the number of writes. */
static inline unsigned int odi_nic_multi_rx_off(struct odi_nic_regw *w)
{
	unsigned int n = 0, i;

	n = odi_nic_regw_put(w, n, ODI_NIC_RUN1, 32, ODI_NIC_RUN1_VAL);
	for (i = 0; i < ODI_NIC_RX_RING_MAP_WORDS; i++)
		n = odi_nic_regw_put(w, n, ODI_NIC_RX_RING_MAP + 4U * i, 32, odi_nic_rx_ring_map(0));
	return n;
}

#endif /* ODI_NIC_HW_H */
