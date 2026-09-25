// SPDX-License-Identifier: GPL-2.0
/*
 * odi_nic.c -- open driver for the RTL9602C CPU-port Ethernet NIC.
 *
 * Written against our own V1.0-220923 image. Besides the net_devices it
 * gives odi_omci.c two entry points: an RX classification hook, so OMCI
 * frames reach omcid without a net_device of their own, and a TX call
 * that sends a frame with caller-supplied descriptor words.
 *
 * Scope: no multi-WAN, no bridge shortcut path (the kernel bridge on br0
 * already does this), no external-switch support, no hardware LSO, no
 * descriptors in SRAM. This is a bridge ONU with management on br0; the
 * driver is sized for that, not for every mode the stock driver supported.
 *
 * Interface to odi_omci.c: struct odi_tx_words/odi_rx_words are copies
 * of the TX/RX descriptor words themselves, not separate wire structs.
 * odi_nic_tx_words() takes a raw frame, its length and the words the
 * caller filled. An rxhook (odi_rxhook_fn) returns ODI_RXHOOK_STOP,
 * CONTINUE or STOP_NOFREE.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <asm/addrspace.h> /* CPHYSADDR() */
#include <asm/mach-rtl8686/rtl8686-barrier.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/platform_device.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/unaligned.h>
#include "odi_compat.h" /* ODI_NIC_IRQ_NUM only, see its own file header */

#include "odi_nic_hw.h"
#include "odi_nic.h"
#include "odi_wdt.h" /* wdt_pre_reset_hook */

#define DRV_NAME "odi_nic"

/* Per-interrupt and per-RX-descriptor tracing was load-bearing while n1-n17
 * were still chasing the boot hang and the RX drop path; both are cheap but
 * noisy in steady state, so they are gated behind this off-by-default knob
 * instead of removed outright -- the state dump and storm safety net below
 * stay unconditional, they are two lines twice per boot and a last resort,
 * not a trace.
 */
static bool odi_nic_debug;
module_param_named(debug, odi_nic_debug, bool, 0644);
MODULE_PARM_DESC(debug, "trace the first interrupts and RX descriptors (default off)");

/* ---- MMIO access -------------------------------------------------------
 *
 * The register window is a fixed KSEG1 address, not
 * something ioremap discovers at runtime. We keep a raw pointer, but go through
 * readl/writel-style helpers so every access site states its own width
 * (registers are not uniformly 32-bit) rather than relying on
 * pointer arithmetic at each call site.
 */

static void __iomem *odi_mmio;

static inline u32 odi_r32(u32 off) { return readl(odi_mmio + off); }
static inline void odi_w32(u32 off, u32 val) { writel(val, odi_mmio + off); }
static inline u16 odi_r16(u32 off) { return readw(odi_mmio + off); }
static inline void odi_w16(u32 off, u16 val) { writew(val, odi_mmio + off); }
static inline u8 odi_r8(u32 off) { return readb(odi_mmio + off); }
static inline void odi_w8(u32 off, u8 val) { writeb(val, odi_mmio + off); }

/* Descriptor stores the DMA engine must see before the next store: the
 * body of a descriptor before its ownership bit, and every returned
 * ownership bit before the engine is stopped. A sync, which drains the
 * write buffer. Not wmb(): the kernel is built as CPU_R3000, where
 * wmb() emits no instruction (rtl8686-barrier.h).
 */
#define odi_dma_wmb()	rtl8686_sync()

/* ---- Net devices and switch-port mapping -------------------------------
 *
 * eth0 is the CPU-port catch-all: it exists for parity with the rootfs
 * (network.sh references it) but carries nothing our forwarding cares
 * about once eth0.2/eth0.3 claim their ports -- confirmed live in the KB
 * (rtl9601-multi-lan-eth0-2-is-the-host-port.md): LAN traffic arrives on
 * eth0.2, not eth0. nas0 and pon0 are declared for the same rootfs-name
 * parity; their switch port numbers are not pinned down (they are "not
 * relevant to the LAN/host path"), so they
 * are registered but never see traffic through this driver -- carrier
 * stays off and RX/TX on them is not reachable from src_port_num demux.
 * This is a scope choice, not an oversight: nothing in the five success
 * criteria touches nas0 or pon0.
 */
#define ODI_PORT_CATCHALL	0xFFU	/* eth0: never matched by src_port_num */
#define ODI_PORT_LAN0		0U	/* eth0.2 */
#define ODI_PORT_LAN1		1U	/* eth0.3 */
#define ODI_PORT_NONE		0xFEU	/* nas0, pon0: no traffic reaches these here */

#define ODI_NUM_DEVS 5

struct odi_port {
	struct net_device *dev;
	unsigned int port;		/* ODI_PORT_* -- which src_port_num this device claims */
};

/* ---- Shared NIC state: one ring set behind all five net_devices ------- */

struct odi_nic {
	struct odi_rx_desc *rx_ring;
	dma_addr_t rx_ring_dma;
	struct sk_buff **rx_skb;

	struct odi_tx_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	/* Per-slot bounce buffers for odi_nic_xmit_raw() (not skbs -- callers
	 * hand raw payloads, copied into a kmalloc buffer we own end to end).
	 * dma/len are tracked alongside so odi_tx_reclaim() can unmap before
	 * freeing.
	 */
	void **tx_buf;
	dma_addr_t *tx_dma;
	unsigned int *tx_len;

	unsigned int rx_head;	/* next descriptor to inspect */
	unsigned int tx_tail;	/* next descriptor to fill */
	unsigned int tx_reclaim;	/* next descriptor to reclaim */

	struct napi_struct napi;
	struct net_device *napi_dev;	/* NAPI is device-scoped; any of ours will do */
	spinlock_t lock;

	struct odi_port ports[ODI_NUM_DEVS];
	int irq_requested;

	/* Interrupt-storm safety net (n1/n3 hung silently right around IRQ
	 * enable with no oops, 2026-09-21 evening). Counts
	 * interrupts in the first 2s after request_irq(); if more than 5000
	 * arrive, mask everything, disable the line, and say so -- a masked,
	 * limping boot beats an invisible livelock.
	 */
	unsigned long irq_window_start;	/* jiffies at first IRQ */
	unsigned int irq_window_count;
	int irq_storm_tripped;

	/* Post-open state dump (trial n5: full open with IRQ requested and
	 * rings programmed, but zero interrupts arrived and no revert-time
	 * diagnostic said why). Fires twice off the first
	 * ndo_open and nowhere else, so a re-open after ndo_stop does not pile
	 * up extra timers.
	 */
	struct delayed_work state_dump_work;
	int state_dump_fire;		/* 0 before first firing, 1 after it */

	/* TX reclaim while the queues are stopped. There is no TX interrupt,
	 * and reclaim otherwise runs only from NAPI (RX) and ndo_start_xmit,
	 * which a stopped queue no longer calls: with no RX traffic nothing
	 * would ever wake the queues again.
	 */
	struct delayed_work tx_stall_work;

	/* Plain counters, no locking: read racily by the dump, same as every
	 * other diagnostic-only counter in this file (irq_window_count above).
	 */
	unsigned int irq_entries;
	unsigned int napi_polls;
};

static struct odi_nic odi;

/* A minimal platform_device purely so dma_alloc_coherent()/dma_map_single()
 * have a real struct device (with a coherent_dma_mask) to operate on.
 * net_devices from alloc_etherdev() have no dev.parent unless one is set,
 * and passing NULL through to the MIPS dma-mapping code dereferences
 * dev->coherent_dma_mask unconditionally in massage_gfp_flags() -- the
 * first NIC=oss trial on isp1 hung completely silently right where
 * ndo_open would have made this call, with nothing in the RAM log past
 * the last rtk_init crumb (2026-09-21). This driver owns no
 * other platform resource, so a plain platform_device_register_simple()
 * is enough; the recommended use of dma_alloc_coherent()
 * only holds if there is a device to hand it.
 */
static struct platform_device *odi_pdev;


/* ---- RX classification hooks: odi_nic_rxhook_register -----------------
 *
 * odi_omci.c registers here instead of opening its own net_device.
 */

#define ODI_MAX_RXHOOKS 8

struct odi_rxhook {
	int used;
	int portmask;
	int priority;
	odi_rxhook_fn fn;
};

static struct odi_rxhook odi_rxhooks[ODI_MAX_RXHOOKS];
static DEFINE_SPINLOCK(odi_rxhook_lock);

/*
 * Returns 0 on success and -1 when full; the callers test for 0
 * (a void version once left them a garbage status,
 * and the GPON module reported its OMCI hook registration as failed).
 */
int odi_nic_rxhook_register(int portmask, int priority, odi_rxhook_fn rx)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&odi_rxhook_lock, flags);
	for (i = 0; i < ODI_MAX_RXHOOKS; i++) {
		if (!odi_rxhooks[i].used) {
			odi_rxhooks[i].used = 1;
			odi_rxhooks[i].portmask = portmask;
			odi_rxhooks[i].priority = priority;
			odi_rxhooks[i].fn = rx;
			break;
		}
	}
	spin_unlock_irqrestore(&odi_rxhook_lock, flags);
	if (i == ODI_MAX_RXHOOKS) {
		pr_warn(DRV_NAME ": rxhook table full, portmask 0x%x priority %d dropped\n",
			portmask, priority);
		return -1;
	}
	pr_info(DRV_NAME ": rxhook portmask 0x%x priority %d registered\n", portmask, priority);
	return 0;
}

int odi_nic_rxhook_unregister(int portmask, int priority, odi_rxhook_fn rx)
{
	unsigned long flags;
	int i, found = -1;

	spin_lock_irqsave(&odi_rxhook_lock, flags);
	for (i = 0; i < ODI_MAX_RXHOOKS; i++) {
		if (odi_rxhooks[i].used && odi_rxhooks[i].portmask == portmask &&
		    odi_rxhooks[i].priority == priority && odi_rxhooks[i].fn == rx) {
			odi_rxhooks[i].used = 0;
			found = 0;
		}
	}
	spin_unlock_irqrestore(&odi_rxhook_lock, flags);
	return found;
}

static void odi_nic_rxhook_reset(void)
{
	memset(odi_rxhooks, 0, sizeof(odi_rxhooks));
}

/* Walk the matching hooks from the highest priority down, until one
 * consumes the frame. Returns nonzero when the frame
 * was consumed (and freed, or taken over), 0 when the caller should
 * deliver it to the net device.
 */
static int odi_dispatch_rxhooks(const struct odi_rx_desc *d, unsigned int src_port,
				struct sk_buff *skb)
{
	struct odi_rx_words info = { d->opts1, d->addr, d->opts2, d->opts3 };
	int done_prio = INT_MAX;

	for (;;) {
		struct odi_rxhook *best = NULL;
		unsigned long flags;
		odi_rxhook_fn fn;
		int i, ret;

		spin_lock_irqsave(&odi_rxhook_lock, flags);
		for (i = 0; i < ODI_MAX_RXHOOKS; i++) {
			if (!odi_rxhooks[i].used)
				continue;
			if (!(odi_rxhooks[i].portmask & (1 << src_port)))
				continue;
			if (odi_rxhooks[i].priority >= done_prio)
				continue;
			if (!best || odi_rxhooks[i].priority > best->priority)
				best = &odi_rxhooks[i];
		}
		fn = best ? best->fn : NULL;
		if (best)
			done_prio = best->priority;
		spin_unlock_irqrestore(&odi_rxhook_lock, flags);

		if (!fn)
			return 0;
		ret = fn(skb, &info);
		if (ret == ODI_RXHOOK_STOP) {
			dev_kfree_skb(skb);
			return 1;
		}
		if (ret == ODI_RXHOOK_STOP_NOFREE)
			return 1;
	}
}

/* ---- TX with caller-filled descriptor words ---------------------------
 *
 * odi_omci.c sends OMCI replies tagged with an explicit destination port
 * and GEM stream rather than through ndo_start_xmit.
 */
/* struct odi_tx_words (odi_nic.h): a two-field restatement (trials n1 to
 * n13) once read the opts1 word as the port mask, so every OMCI reply went
 * to a nonsense port mask and the OLT never finished the MIB upload (n13:
 * O5 for five minutes, mib sync 0).
 */

/* opts1 bits the caller may set; own, eor, fs, ls and the length are ours. */
#define ODI_TXINFO_OPTS1_KEEP	(ODI_TXD_IP_CSUM | ODI_TXD_L4_CSUM | ODI_TXD_BIT25 | (1U << 24) | \
				 ODI_TXD_ADD_CRC | ODI_TXD_BIT22 | ODI_TXD_NO_LEARN | (7U << 18))

static int odi_nic_xmit_desc(unsigned int port_mask, unsigned int prio, int dislrn,
			      const void *data, unsigned int len);
static int odi_nic_xmit_raw(u32 opts1, u32 opts2, u32 opts3,
			     const void *data, unsigned int len);

/* A raw frame, its length, and the descriptor words the caller filled. */
int odi_nic_tx_words(const void *frame, unsigned short len, const struct odi_tx_words *tx)
{
	if (!frame || !tx)
		return -1;
	if (len > ODI_NIC_RX_BUF_SIZE)
		len = ODI_NIC_RX_BUF_SIZE;
	return odi_nic_xmit_raw(tx->opts1 & ODI_TXINFO_OPTS1_KEEP, tx->opts2, tx->opts3,
				frame, len);
}

/* ---- Ring setup --------------------------------------------------------
 *
 * dma_alloc_coherent gets the uncached mapping and the physical address
 * atomically, with no manual KSEG1 aliasing or cache writeback.
 */
static int odi_rings_alloc(struct device *dev)
{
	size_t rx_sz = sizeof(struct odi_rx_desc) * ODI_RX_RING_DEPTH;
	size_t tx_sz = sizeof(struct odi_tx_desc) * ODI_TX_RING_DEPTH;
	unsigned int i;

	odi.rx_ring = dma_alloc_coherent(dev, rx_sz, &odi.rx_ring_dma, GFP_KERNEL);
	odi.tx_ring = dma_alloc_coherent(dev, tx_sz, &odi.tx_ring_dma, GFP_KERNEL);
	if (!odi.rx_ring || !odi.tx_ring)
		return -ENOMEM;

	odi.rx_skb = kcalloc(ODI_RX_RING_DEPTH, sizeof(*odi.rx_skb), GFP_KERNEL);
	odi.tx_buf = kcalloc(ODI_TX_RING_DEPTH, sizeof(*odi.tx_buf), GFP_KERNEL);
	odi.tx_dma = kcalloc(ODI_TX_RING_DEPTH, sizeof(*odi.tx_dma), GFP_KERNEL);
	odi.tx_len = kcalloc(ODI_TX_RING_DEPTH, sizeof(*odi.tx_len), GFP_KERNEL);
	if (!odi.rx_skb || !odi.tx_buf || !odi.tx_dma || !odi.tx_len)
		return -ENOMEM;

	/* Both ring bases must be 256-byte aligned; the RX ring size is a
	 * natural power of two, the TX ring size is not, so only the TX ring
	 * can fail this in practice -- checked on both regardless, since
	 * either is silent hardware/software desync if it ever happens.
	 */
	if (!odi_desc_aligned(odi.rx_ring, (u32)odi.rx_ring_dma) ||
	    !odi_desc_aligned(odi.tx_ring, (u32)odi.tx_ring_dma)) {
		pr_err(DRV_NAME ": ring base not %u-byte aligned (rx virt %p dma 0x%08x, tx virt %p dma 0x%08x)\n",
		       ODI_DESC_ALIGN, odi.rx_ring, (u32)odi.rx_ring_dma,
		       odi.tx_ring, (u32)odi.tx_ring_dma);
		return -EFAULT;
	}

	for (i = 0; i < ODI_RX_RING_DEPTH; i++) {
		struct sk_buff *skb = dev_alloc_skb(ODI_NIC_RX_ALLOC);
		dma_addr_t dma;

		if (!skb)
			return -ENOMEM;
		dma = dma_map_single(dev, skb->data, ODI_NIC_RX_ALLOC, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, dma)) {
			dev_kfree_skb(skb);
			return -ENOMEM;
		}
		odi.rx_skb[i] = skb;
		odi.rx_ring[i].addr = (u32)dma;
		odi.rx_ring[i].opts1 = ODI_RXD_HANDOFF((i == ODI_RX_RING_DEPTH - 1) ? ODI_RXD_WRAP : 0);
		odi.rx_ring[i].opts2 = 0;
		odi.rx_ring[i].opts3 = 0;
	}

	for (i = 0; i < ODI_TX_RING_DEPTH; i++) {
		odi.tx_ring[i].opts1 = (i == ODI_TX_RING_DEPTH - 1) ? ODI_TXD_WRAP : 0;
	}

	return 0;
}

static void odi_rings_free(struct device *dev)
{
	unsigned int i;

	/* Every non-NULL rx_skb slot is a buffer the ring owns and that is
	 * still mapped at rx_ring[i].addr: odi_rings_alloc() fills a slot only
	 * once its mapping succeeded, and odi_poll() swaps a slot only for a
	 * replacement that is already mapped. rx_skb is allocated after
	 * rx_ring, so rx_ring is valid here whenever rx_skb is.
	 */
	if (odi.rx_skb) {
		for (i = 0; i < ODI_RX_RING_DEPTH; i++) {
			if (!odi.rx_skb[i])
				continue;
			dma_unmap_single(dev, odi.rx_ring[i].addr,
					 ODI_NIC_RX_ALLOC, DMA_FROM_DEVICE);
			dev_kfree_skb(odi.rx_skb[i]);
		}
		kfree(odi.rx_skb);
		odi.rx_skb = NULL;
	}
	if (odi.tx_buf) {
		for (i = 0; i < ODI_TX_RING_DEPTH; i++) {
			if (!odi.tx_buf[i])
				continue;
			dma_unmap_single(dev, odi.tx_dma[i], odi.tx_len[i], DMA_TO_DEVICE);
			kfree(odi.tx_buf[i]);
		}
		kfree(odi.tx_buf);
		odi.tx_buf = NULL;
	}
	kfree(odi.tx_dma);
	odi.tx_dma = NULL;
	kfree(odi.tx_len);
	odi.tx_len = NULL;
	if (odi.rx_ring)
		dma_free_coherent(dev, sizeof(struct odi_rx_desc) * ODI_RX_RING_DEPTH,
				   odi.rx_ring, odi.rx_ring_dma);
	if (odi.tx_ring)
		dma_free_coherent(dev, sizeof(struct odi_tx_desc) * ODI_TX_RING_DEPTH,
				   odi.tx_ring, odi.tx_ring_dma);
	odi.rx_ring = NULL;
	odi.tx_ring = NULL;
}

/* ---- Hardware init, in the required order ------------------------------ */

static void odi_reset_hw(void)
{
	/* ODI_SOC_BLOCK_EN is defined as its KSEG1 alias; ioremap wants the
	 * physical address, and a KSEG1 value handed to it maps a nonexistent
	 * physical page, which hung trials n1 to n4 on the first readl with
	 * no exception. CPHYSADDR() (asm/addrspace.h) is the named form of
	 * the same "& 0x1fffffff" mask every MIPS KSEG0/KSEG1-to-physical
	 * conversion in the kernel proper uses.
	 */
	void __iomem *blk = ioremap(CPHYSADDR(ODI_SOC_BLOCK_EN), 4);

	if (!blk) {
		pr_warn(DRV_NAME ": could not map the block-enable register, skipping the clock-gate cycle\n");
		return;
	}
	/* Full block power/clock-gate cycle: clear the NIC enable bit, wait,
	 * set it again. Not a register-level soft reset.
	 */
	writel(readl(blk) & ~ODI_SOC_BLOCK_EN_NIC, blk);
	mdelay(10);
	writel(readl(blk) | ODI_SOC_BLOCK_EN_NIC, blk);
	iounmap(blk);
}

static void odi_init_hw(struct net_device *mac_dev)
{
	odi_reset_hw();

	odi_w8(ODI_NIC_CTRL + 3 /* CTRL is the byte at offset 0x3B of the 0x38 word */,
	       ODI_NIC_CTRL_RX_CSUM | ODI_NIC_CTRL_RX_JUMBO);

	odi_w16(ODI_NIC_IRQ_STATUS, ODI_NIC_IRQ_ALL);
	odi_w32(ODI_NIC_RING_IRQ_STATUS, 0xFFFFFFFFU);

	/* RING_IRQ_MASK/RING_IRQ_STATUS (0xD0/0xD8) are the multi-ring RX sources for rings 2-6
	 * ONLY. Ring 1, the only ring this driver runs, is entirely
	 * serviced through the legacy IRQ_MASK/IRQ_STATUS pair above. A prior revision
	 * wrote RING_IRQ_MASK=0x1 under the mistaken belief that bit0 there gated ring
	 * 1 -- it does not, and combined with RING_IRQS below it is the
	 * leading suspect for the n1/n3 silent hang (2026-09-21 evening): if
	 * the RX-done signal for ring 1 gets routed
	 * through this multi-ring path by RING_IRQS while our IRQ handler
	 * only ever reads/acks the legacy IRQ_STATUS at 0x3E, the interrupt line
	 * never gets acknowledged and re-asserts forever -- a silent storm
	 * with no oops, exactly the observed symptom. Leave RING_IRQ_MASK at 0: we
	 * never enable rings 2-6, so nothing should need unmasking there.
	 */
	odi_w32(ODI_NIC_RING_IRQ_MASK, 0x0U);

	odi_w32(ODI_NIC_TX_CFG, ODI_NIC_TX_CFG_VAL);

	/* CPU-tag engine bring-up (step 6): off, then on with the full
	 * profile -- writing it once with the final value left the tag
	 * engine in a stale state, so the write is two-step.
	 */
	odi_w32(ODI_NIC_TAG_CTRL, 0);
	odi_w32(ODI_NIC_TAG_CTRL, ODI_NIC_TAG_CTRL_ON);

	/* VDSL is vestigial on this SoC and PON_PORT is not needed for the
	 * LAN/management path this driver serves; write 0 for both rather
	 * than guessing switch-side port numbers that are not documented.
	 */
	odi_w32(ODI_NIC_TAG_CTRL1, ODI_NIC_TAG1_VAL(0, 0, ODI_NIC_TAG1_SID0));

	pr_info(DRV_NAME ": tx ring phys 0x%08x (%u entries), rx ring phys 0x%08x (%u entries)\n",
		(u32)odi.tx_ring_dma, ODI_TX_RING_DEPTH, (u32)odi.rx_ring_dma, ODI_RX_RING_DEPTH);

	odi_w32(ODI_NIC_TX1_RING, (u32)odi.tx_ring_dma);
	odi_w32(ODI_NIC_TX1_INDEX, 0);
	pr_info(DRV_NAME ": tx1_ring wrote 0x%08x read back 0x%08x, tx1_index read back 0x%08x\n",
		(u32)odi.tx_ring_dma, odi_r32(ODI_NIC_TX1_RING), odi_r32(ODI_NIC_TX1_INDEX));

	odi_w32(ODI_NIC_RX1_RING, (u32)odi.rx_ring_dma);

	/* Ring depth, expressed as depth-1, NOT the same
	 * register as RX1_COUNT below -- see the header comment. A 12-bit
	 * value split low/high; ODI_RX_RING_DEPTH (64) fits entirely in the
	 * low byte, so the high nibble is always 0 for us.
	 */
	odi_w8(ODI_NIC_RX1_LAST, (u8)((ODI_RX_RING_DEPTH - 1) & 0xFF));
	odi_w8(ODI_NIC_RX1_LAST_HI, (u8)(((ODI_RX_RING_DEPTH - 1) >> 8) & 0x0F));

	/* RX1_COUNT is an 8-bit register; a 32-bit write here in
	 * a prior revision clobbered FC_ON_LEVEL/FC_OFF_LEVEL/the following byte
	 * with BE-ordered garbage before the explicit 8-bit writes below put
	 * FC_ON_LEVEL/FC_OFF_LEVEL right again -- but left RX1_COUNT itself reading
	 * the wrong byte lane. Write it with the matching width.
	 */
	odi_w8(ODI_NIC_RX1_COUNT, (u8)(ODI_RX_RING_DEPTH & 0xFF));

	pr_info(DRV_NAME ": rx1_ring wrote 0x%08x read back 0x%08x, rx1_last read back %u/%u, rx1_count read back %u\n",
		(u32)odi.rx_ring_dma, odi_r32(ODI_NIC_RX1_RING),
		odi_r8(ODI_NIC_RX1_LAST), odi_r8(ODI_NIC_RX1_LAST_HI), odi_r8(ODI_NIC_RX1_COUNT));
	/* Flow-control watermark pair: the stock values were not captured --
	 * conservative defaults, ring depth / 4 and 3 * ring depth / 4,
	 * since this only affects backpressure timing.
	 */
	odi_w8(ODI_NIC_FC_ON_LEVEL, ODI_RX_RING_DEPTH / 4);
	odi_w8(ODI_NIC_FC_OFF_LEVEL, (ODI_RX_RING_DEPTH * 3) / 4);

	odi_w32(ODI_NIC_R13FC, 0);

	odi_w8(ODI_NIC_PAUSE, odi_r8(ODI_NIC_PAUSE) | ODI_NIC_PAUSE_ON);

	odi_w32(ODI_NIC_STATION0, get_unaligned_be32(mac_dev->dev_addr));
	odi_w32(ODI_NIC_STATION4, ((u32)mac_dev->dev_addr[4] << 24) | ((u32)mac_dev->dev_addr[5] << 16));

	/* RING_IRQS (FIFO_CFG bit 24) belongs with multi-ring RX -- which this
	 * build does not use (ring 1 only), so it is left clear. Setting it alongside RING_IRQ_MASK=0
	 * above would have been the same bug from the other side: routing
	 * ring 1's interrupt away from the legacy IRQ_STATUS our handler reads.
	 */
	odi_w32(ODI_NIC_FIFO_CFG, ODI_NIC_FIFO_RX_2K);

	odi_w32(ODI_NIC_RX_RING_MAP + 0 * 4, ODI_NIC_RX_RING_MAP_1TO1);
	odi_w32(ODI_NIC_RX_RING_MAP + 1 * 4, ODI_NIC_RX_RING_MAP_1TO1);
	odi_w32(ODI_NIC_RX_RING_MAP + 2 * 4, ODI_NIC_RX_RING_MAP_1TO1);
	odi_w32(ODI_NIC_RX_RING_MAP + 3 * 4, ODI_NIC_RX_RING_MAP_1TO1);
	odi_w32(ODI_NIC_RX_RING_MAP + 4 * 4, ODI_NIC_RX_RING_MAP_1TO1);
	odi_w32(ODI_NIC_RX_RING_MAP + 5 * 4, ODI_NIC_RX_RING_MAP_1TO1);

	/* RX accept mode. The captured stock init path does not write RX_FILTER
	 * directly; accepting broadcast, multicast, own MAC and any unicast,
	 * with the hash filter wide open, is the ordinary set_rx_mode result
	 * for a promiscuous bridge port. Left at reset the
	 * MAC accepts nothing: trial n6 saw TX descriptors consumed and not
	 * one RX descriptor returned in 120 s. The switch does the real
	 * filtering by port, so promiscuous here costs nothing.
	 */
	odi_w32(ODI_NIC_MCHASH0, 0xFFFFFFFFU);
	odi_w32(ODI_NIC_MCHASH4, 0xFFFFFFFFU);
	odi_w32(ODI_NIC_RX_FILTER, ODI_NIC_RX_FILTER_ALL);

}

/* Enabling is kept apart from programming: the interrupt mask and the go
 * bits are written only once request_irq and napi_enable have succeeded,
 * so no interrupt can arrive with no handler or a disabled NAPI context.
 */
static void odi_start_hw(void)
{
	odi_w16(ODI_NIC_IRQ_MASK, ODI_NIC_IRQ_RX_OK | ODI_NIC_IRQ_RX_RUNT | ODI_NIC_IRQ_RX_FIFO_FULL | ODI_NIC_IRQ_RX_NO_DESC);
	odi_w32(ODI_NIC_RUN1, ODI_NIC_RUN1_VAL);
	odi_w32(ODI_NIC_RUN, ODI_NIC_RUN_VAL);
}

/* The DMA engine is not reset with the CPU. After a watchdog reset it
 * kept receiving into our ring while U-Boot and the stock kernel were
 * booting, and the stock kernel died at random points (trials n6, n10, n11,
 * 2026-09-21). Plain MMIO and memory writes only, so this is safe from
 * the deadline timer, a reboot notifier and a panic notifier alike.
 */
static void odi_quiesce_hw(void)
{
	unsigned int i;

	if (!odi_mmio)
		return;
	odi_w32(ODI_NIC_RUN, 0);
	odi_w32(ODI_NIC_RUN1, 0);
	odi_w16(ODI_NIC_IRQ_MASK, 0);
	odi_w32(ODI_NIC_RING_IRQ_MASK, 0);
	odi_w16(ODI_NIC_IRQ_STATUS, ODI_NIC_IRQ_ALL);
	odi_w32(ODI_NIC_RING_IRQ_STATUS, 0xFFFFFFFFU);
	if (odi.rx_ring)
		for (i = 0; i < ODI_RX_RING_DEPTH; i++)
			odi.rx_ring[i].opts1 &= ~ODI_RXD_HW;
	odi_dma_wmb();
}

static void odi_stop_hw(void)
{
	odi_quiesce_hw();
	if (odi.irq_requested)
		synchronize_irq(ODI_NIC_IRQ_NUM);
	udelay(10);
}

static int odi_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	odi_quiesce_hw();
	return NOTIFY_DONE;
}

static struct notifier_block odi_reboot_nb = { .notifier_call = odi_reboot_notify };
static struct notifier_block odi_panic_nb = { .notifier_call = odi_reboot_notify };

/* ---- TX ---------------------------------------------------------------
 *
 * One ring behind five net_devices and odi_omci.c. When it cannot take
 * another frame every queue is stopped; reclaim wakes them all once the
 * ring has drained to ODI_TX_WAKE_USED entries in flight, so a stop/wake
 * cycle is not paid on every freed slot.
 */
#define ODI_TX_WAKE_USED	(ODI_TX_RING_DEPTH / 2)
#define ODI_TX_STALL_POLL_MS	10	/* tx_stall_work period while stopped */

static int odi_nic_xmit_desc(unsigned int port_mask, unsigned int prio, int dislrn,
			      const void *data, unsigned int len)
{
	struct odi_tx_desc t;

	odi_nic_tx_pack(&t, port_mask, 0, prio, dislrn);
	return odi_nic_xmit_raw(t.opts1, t.opts2, t.opts3, data, len);
}

/* The one test for "cannot take another frame": odi_nic_xmit_raw() refuses
 * on it and odi_start_xmit() stops the queues on it. Caller holds odi.lock.
 */
static bool odi_tx_full_locked(void)
{
	lockdep_assert_held(&odi.lock);
	return odi_ring_is_full(odi.tx_reclaim, odi_ring_next(odi.tx_tail, ODI_TX_RING_DEPTH),
				ODI_TX_RING_DEPTH);
}

/* One TX descriptor from caller-supplied words: opts1 flags (never own,
 * eor, fs, ls or the length), opts2 (CPU tag), opts3 (port mask, stream).
 */
static int odi_nic_xmit_raw(u32 opts1, u32 opts2, u32 opts3,
			     const void *data, unsigned int len)
{
	unsigned long flags;
	unsigned int idx;
	struct odi_tx_desc *d;
	dma_addr_t dma;
	void *buf;

	spin_lock_irqsave(&odi.lock, flags);

	idx = odi.tx_tail;
	if (odi_tx_full_locked()) {
		spin_unlock_irqrestore(&odi.lock, flags);
		return -ENOBUFS;
	}

	buf = kmalloc(len, GFP_ATOMIC);
	if (!buf) {
		spin_unlock_irqrestore(&odi.lock, flags);
		return -ENOMEM;
	}
	memcpy(buf, data, len);
	dma = dma_map_single(&odi_pdev->dev, buf, len, DMA_TO_DEVICE);
	if (dma_mapping_error(&odi_pdev->dev, dma)) {
		kfree(buf);
		spin_unlock_irqrestore(&odi.lock, flags);
		return -ENOMEM;
	}

	d = &odi.tx_ring[idx];
	/* The hardware FCS and the CPU tag are forced on every frame sent
	 * for these callers, on top of their words; the OMCI module
	 * sets neither, and n16 saw the OLT retransmit every request three
	 * times because the replies left without an FCS.
	 */
	d->opts2 = opts2 | ODI_TXD_TAGGED;
	d->opts3 = opts3;
	d->addr = (u32)dma;
	d->opts1 = (opts1 & ~(ODI_TXD_HW | ODI_TXD_WRAP | ODI_TXD_FIRST | ODI_TXD_LAST | ODI_TXD_SIZE_MASK)) |
		   ODI_TXD_FIRST | ODI_TXD_LAST | ODI_TXD_ADD_CRC | (len & ODI_TXD_SIZE_MASK) |
		   ((idx == ODI_TX_RING_DEPTH - 1) ? ODI_TXD_WRAP : 0);
	odi_dma_wmb();
	d->opts1 |= ODI_TXD_HW;	/* HW bit set last */

	odi.tx_buf[idx] = buf;
	odi.tx_dma[idx] = dma;
	odi.tx_len[idx] = len;
	odi.tx_tail = odi_ring_next(idx, ODI_TX_RING_DEPTH);

	odi_w32(ODI_NIC_RUN, odi_r32(ODI_NIC_RUN) | ODI_NIC_RUN_TX_KICK);

	spin_unlock_irqrestore(&odi.lock, flags);
	return 0;
}

static void odi_tx_wake_queues(void)
{
	int i;

	for (i = 0; i < ODI_NUM_DEVS; i++) {
		struct net_device *dev = odi.ports[i].dev;

		if (dev && netif_running(dev) && netif_queue_stopped(dev))
			netif_wake_queue(dev);
	}
}

static bool odi_tx_any_stopped(void)
{
	int i;

	for (i = 0; i < ODI_NUM_DEVS; i++) {
		struct net_device *dev = odi.ports[i].dev;

		if (dev && netif_running(dev) && netif_queue_stopped(dev))
			return true;
	}
	return false;
}

/* Opportunistic reclaim (our build takes no TX_OK
 * interrupt), called from NAPI poll, from ndo_start_xmit and, while the
 * queues are stopped, from tx_stall_work.
 */
static void odi_tx_reclaim(void)
{
	unsigned long flags;
	unsigned int used;

	spin_lock_irqsave(&odi.lock, flags);
	while (odi.tx_reclaim != odi.tx_tail) {
		struct odi_tx_desc *d = &odi.tx_ring[odi.tx_reclaim];

		if (d->opts1 & ODI_TXD_HW)
			break;
		dma_unmap_single(&odi_pdev->dev, odi.tx_dma[odi.tx_reclaim],
				  odi.tx_len[odi.tx_reclaim], DMA_TO_DEVICE);
		kfree(odi.tx_buf[odi.tx_reclaim]);
		odi.tx_buf[odi.tx_reclaim] = NULL;
		odi.tx_reclaim = odi_ring_next(odi.tx_reclaim, ODI_TX_RING_DEPTH);
	}
	used = odi_ring_used(odi.tx_reclaim, odi.tx_tail, ODI_TX_RING_DEPTH);
	spin_unlock_irqrestore(&odi.lock, flags);

	/* Pairs with the smp_mb() in odi_tx_stop_queues(): either the sender
	 * sees the slots freed above when it re-checks, or this sees its
	 * queues stopped. Either way no queue stays stopped on a drained ring.
	 */
	smp_mb();
	if (used <= ODI_TX_WAKE_USED)
		odi_tx_wake_queues();
}

static void odi_tx_stop_queues(void)
{
	int i;

	for (i = 0; i < ODI_NUM_DEVS; i++)
		if (odi.ports[i].dev)
			netif_stop_queue(odi.ports[i].dev);
	/* Pairs with the smp_mb() in odi_tx_reclaim(). Re-check after the
	 * stop: a reclaim that ran before it saw no stopped queue to wake;
	 * this one wakes them if the ring has drained since.
	 */
	smp_mb();
	odi_tx_reclaim();
	if (odi_tx_any_stopped())
		schedule_delayed_work(&odi.tx_stall_work, msecs_to_jiffies(ODI_TX_STALL_POLL_MS));
}

static void odi_tx_stall_work(struct work_struct *work)
{
	odi_tx_reclaim();
	if (odi_tx_any_stopped())
		schedule_delayed_work(&odi.tx_stall_work, msecs_to_jiffies(ODI_TX_STALL_POLL_MS));
}

static netdev_tx_t odi_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct odi_port *p = netdev_priv(dev);
	unsigned int mask = 1U << p->port;
	unsigned long flags;
	bool full;
	int ret;

	if (p->port == ODI_PORT_CATCHALL)
		mask = (1U << ODI_PORT_LAN0) | (1U << ODI_PORT_LAN1);
	else if (p->port == ODI_PORT_NONE)
		mask = 0;	/* nas0/pon0: not wired to a switch port here, see the note above */

	if (!mask) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	ret = odi_nic_xmit_desc(mask, 0, 0, skb->data, skb->len);
	if (ret == -ENOBUFS) {
		/* Only when another sender (odi_omci.c, which has no queue to
		 * stop) took the last slot since this queue was last checked.
		 */
		odi_tx_stop_queues();
		return NETDEV_TX_BUSY;
	}
	if (ret) {
		dev->stats.tx_dropped++;
	} else {
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += skb->len;
	}
	dev_kfree_skb_any(skb);
	odi_tx_reclaim();

	spin_lock_irqsave(&odi.lock, flags);
	full = odi_tx_full_locked();
	spin_unlock_irqrestore(&odi.lock, flags);
	if (full)
		odi_tx_stop_queues();
	return NETDEV_TX_OK;
}

/* ---- RX and NAPI -------------------------------------------------------
 *
 * Standard NAPI: nothing about the hardware calls for a tasklet.
 */

static struct net_device *odi_dev_for_port(unsigned int src_port)
{
	int i;

	for (i = 0; i < ODI_NUM_DEVS; i++)
		if (odi.ports[i].port == src_port)
			return odi.ports[i].dev;
	/* No specific claimant: deliver to the catch-all -- eth0 sees what
	 * nothing more specific claims.
	 */
	for (i = 0; i < ODI_NUM_DEVS; i++)
		if (odi.ports[i].port == ODI_PORT_CATCHALL)
			return odi.ports[i].dev;
	return NULL;
}

static unsigned int odi_rx_log_count;

static int odi_poll(struct napi_struct *napi, int budget)
{
	int done = 0;

	while (done < budget) {
		struct odi_rx_desc *d = &odi.rx_ring[odi.rx_head];
		struct sk_buff *skb, *fresh;
		dma_addr_t fresh_dma;
		unsigned int len, src_port;
		struct net_device *dev;

		if (d->opts1 & ODI_RXD_HW)
			break;

		len = odi_nic_rx_len(d);
		src_port = odi_nic_rx_src_port(d);
		skb = odi.rx_skb[odi.rx_head];

		/* The first few descriptors the hardware hands back, raw: trial
		 * n7 returned one and every counter stayed at zero, so the
		 * drop happened below and the words are the only witness.
		 * Behind odi_nic.debug now that criterion coverage is done.
		 */
		if (odi_nic_debug && odi_rx_log_count < 6) {
			pr_info(DRV_NAME ": rx desc[%u] opts1=0x%08x opts2=0x%08x opts3=0x%08x len=%u src_port=%u first bytes %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
				odi.rx_head, d->opts1, d->opts2, d->opts3, len, src_port,
				skb->data[0], skb->data[1], skb->data[2], skb->data[3],
				skb->data[4], skb->data[5], skb->data[6], skb->data[7],
				skb->data[8], skb->data[9], skb->data[10], skb->data[11]);
			odi_rx_log_count++;
		}

		if ((d->opts1 & (ODI_RXD_BAD_CRC | ODI_RXD_TOO_LONG)) ||
		    len <= ETH_FCS_LEN || len > ODI_NIC_RX_BUF_SIZE) {
			/* Drop and recycle the same buffer; do not pass to dispatch,
			 * and do not touch the DMA mapping -- it is still the live
			 * mapping set up at the last refill of this descriptor, and
			 * remapping it again here without an intervening unmap
			 * leaked a mapping every time this path ran. The length
			 * comes from the descriptor, not the buffer, so a bad one
			 * is caught before the buffer is given up.
			 */
			dev = odi_dev_for_port(ODI_PORT_CATCHALL);
			if (dev)
				dev->stats.rx_errors++;
			goto requeue;
		}

		dev = odi_dev_for_port(src_port);

		/* The replacement is allocated and mapped before the received
		 * buffer is given up. If either fails, the frame is dropped and
		 * the received buffer, still mapped, goes back to the hardware
		 * as it is: the ring never points at a buffer it does not own.
		 */
		fresh = dev_alloc_skb(ODI_NIC_RX_ALLOC);
		if (fresh) {
			fresh_dma = dma_map_single(&odi_pdev->dev, fresh->data,
						   ODI_NIC_RX_ALLOC, DMA_FROM_DEVICE);
			if (dma_mapping_error(&odi_pdev->dev, fresh_dma)) {
				dev_kfree_skb(fresh);
				fresh = NULL;
			}
		}
		if (!fresh) {
			if (dev)
				dev->stats.rx_dropped++;
			goto requeue;
		}

		dma_unmap_single(&odi_pdev->dev, d->addr,
				  ODI_NIC_RX_ALLOC, DMA_FROM_DEVICE);
		odi.rx_skb[odi.rx_head] = fresh;
		skb_reserve(skb, ODI_NIC_RX_OFFSET);
		skb_put(skb, len - ETH_FCS_LEN);

		if (dev) {
			skb->dev = dev;
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += len;
			/* Hooks see the frame at the MAC header, as the stock
			 * callers expect it; eth_type_trans pulls the header and is
			 * only right for the net device path.
			 */
			if (!odi_dispatch_rxhooks(d, src_port, skb)) {
				skb->protocol = eth_type_trans(skb, dev);
				netif_receive_skb(skb);
			}
		} else {
			dev_kfree_skb(skb);
		}

		/* Only now: the hooks above get the descriptor words as the
		 * hardware returned them, addr included.
		 */
		d->addr = (u32)fresh_dma;
requeue:
		d->opts2 = 0;
		d->opts3 = 0;
		odi_dma_wmb();
		d->opts1 = ODI_RXD_HANDOFF(odi.rx_head == ODI_RX_RING_DEPTH - 1 ? ODI_RXD_WRAP : 0);

		odi.rx_head = odi_ring_next(odi.rx_head, ODI_RX_RING_DEPTH);
		done++;
	}

	odi.napi_polls++;
	odi_tx_reclaim();

	if (done < budget) {
		napi_complete(napi);
		odi_w16(ODI_NIC_IRQ_MASK, ODI_NIC_IRQ_RX_OK | ODI_NIC_IRQ_RX_RUNT | ODI_NIC_IRQ_RX_FIFO_FULL | ODI_NIC_IRQ_RX_NO_DESC);
	}
	return done;
}

/* First 20 interrupts get a full printk when odi_nic.debug is set; storm
 * safety trips at ODI_IRQ_STORM_TRIP_COUNT in the first
 * ODI_IRQ_STORM_WINDOW_MS after the first one regardless of debug (n1/n3
 * hung silently with no oops right around IRQ enable, 2026-09-21 evening
 * -- the signature of an unacknowledged/misrouted interrupt re-firing
 * forever).
 */
#define ODI_IRQ_STORM_WINDOW_MS		2000
#define ODI_IRQ_STORM_TRIP_COUNT	5000
static unsigned int odi_irq_log_count;

static irqreturn_t odi_irq(int irq, void *dev_id)
{
	u16 isr;
	u32 imr_now;

	odi.irq_entries++;	/* every entry, even a tripped or IRQ_NONE one -- the state
				 * dump wants "did the line ever fire at all"
				 */

	if (odi.irq_storm_tripped)
		return IRQ_NONE;

	isr = odi_r16(ODI_NIC_IRQ_STATUS);

	if (odi_nic_debug && odi_irq_log_count < 20) {
		pr_info(DRV_NAME ": irq entry #%u, irq_status=0x%04x\n", odi_irq_log_count, isr);
		odi_irq_log_count++;
	}

	if (!isr) {
		/* Not our line, or (the suspect case) the
		 * real source lives in RING_IRQ_STATUS/multi-ring and this legacy read
		 * never sees it -- either way IRQ_NONE here is what lets the
		 * storm counter below actually count re-entries.
		 */
		goto count_and_check;
	}

	odi_w16(ODI_NIC_IRQ_STATUS, isr);	/* write-1-clear */

	if (isr & (ODI_NIC_IRQ_RX_OK | ODI_NIC_IRQ_RX_NO_DESC | ODI_NIC_IRQ_RX_RUNT | ODI_NIC_IRQ_RX_FIFO_FULL)) {
		odi_w16(ODI_NIC_IRQ_MASK, 0);	/* mask RX sources until poll re-arms them */
		napi_schedule(&odi.napi);
	}

count_and_check:
	if (odi.irq_window_count == 0)
		odi.irq_window_start = jiffies;
	odi.irq_window_count++;

	if (time_before(jiffies, odi.irq_window_start + msecs_to_jiffies(ODI_IRQ_STORM_WINDOW_MS))) {
		if (odi.irq_window_count > ODI_IRQ_STORM_TRIP_COUNT) {
			imr_now = odi_r32(ODI_NIC_RING_IRQ_MASK);
			odi_w16(ODI_NIC_IRQ_MASK, 0);
			odi_w32(ODI_NIC_RING_IRQ_MASK, 0);
			odi.irq_storm_tripped = 1;
			pr_alert(DRV_NAME ": interrupt storm, masked -- last irq_status=0x%04x ring_irq_mask was 0x%08x\n",
				 isr, imr_now);
			disable_irq_nosync(ODI_NIC_IRQ_NUM);
			return IRQ_HANDLED;
		}
	} else {
		/* window rolled over clean: not a storm, reset and keep counting */
		odi.irq_window_start = jiffies;
		odi.irq_window_count = 1;
	}

	return isr ? IRQ_HANDLED : IRQ_NONE;
}

/* First open with no interrupt in 120s (trial n5) left us guessing whether
 * the hardware ever saw the doorbell, whether the ring size stuck, or
 * whether the line just never fired -- printk is the only channel that
 * survives the watchdog revert, so this is the state the postmortem needs,
 * kept to three lines to fit the 8 KB RAM log budget.
 */
#define ODI_STATE_DUMP_DELAY_MS		15000	/* first dump, after ndo_open */
#define ODI_STATE_DUMP_RETRY_MS		30000	/* second dump, only if fire == 1 */
static void odi_state_dump_work(struct work_struct *work)
{
	unsigned int i, tx_own = 0, rx_owned_clear = 0;
	struct net_device *eth0 = odi.ports[0].dev;
	struct net_device *eth0_2 = odi.ports[1].dev;

	(void)work;	/* state lives in the singleton odi, not the work_struct */
	odi.state_dump_fire++;

	pr_info(DRV_NAME ": dump%d regs: irq_status=0x%04x irq_mask=0x%04x ring_irq_mask=0x%08x ring_irq_status=0x%08x run=0x%08x run1=0x%08x xfer=0x%08x pause=0x%02x tag_ctrl=0x%08x fifo_cfg=0x%08x\n",
		odi.state_dump_fire,
		odi_r16(ODI_NIC_IRQ_STATUS), odi_r16(ODI_NIC_IRQ_MASK), odi_r32(ODI_NIC_RING_IRQ_MASK),
		odi_r32(ODI_NIC_RING_IRQ_STATUS), odi_r32(ODI_NIC_RUN), odi_r32(ODI_NIC_RUN1),
		odi_r32(ODI_NIC_XFER_STATUS), odi_r8(ODI_NIC_PAUSE), odi_r32(ODI_NIC_TAG_CTRL),
		odi_r32(ODI_NIC_FIFO_CFG));

	for (i = 0; i < ODI_TX_RING_DEPTH; i++)
		if (odi.tx_ring[i].opts1 & ODI_TXD_HW)
			tx_own++;
	for (i = 0; i < ODI_RX_RING_DEPTH; i++)
		if (!(odi.rx_ring[i].opts1 & ODI_RXD_HW))
			rx_owned_clear++;

	pr_info(DRV_NAME ": dump%d ring: tx1_ring=0x%08x tx1_index=0x%08x rx1_ring=0x%08x rx1_index=0x%08x rx1_count=0x%08x rx_head=%u tx_tail=%u tx_reclaim=%u tx_own=%u rx_clear=%u rxd0_opts1=0x%08x\n",
		odi.state_dump_fire,
		odi_r32(ODI_NIC_TX1_RING), odi_r32(ODI_NIC_TX1_INDEX),
		odi_r32(ODI_NIC_RX1_RING), odi_r32(ODI_NIC_RX1_INDEX), odi_r32(ODI_NIC_RX1_COUNT),
		odi.rx_head, odi.tx_tail, odi.tx_reclaim, tx_own, rx_owned_clear,
		odi.rx_ring[0].opts1);

	pr_info(DRV_NAME ": dump%d counters: irq_entries=%u napi_polls=%u eth0 tx=%lu rx=%lu drop=%lu eth0.2 tx=%lu rx=%lu drop=%lu\n",
		odi.state_dump_fire, odi.irq_entries, odi.napi_polls,
		eth0 ? eth0->stats.tx_packets : 0, eth0 ? eth0->stats.rx_packets : 0,
		eth0 ? eth0->stats.tx_dropped : 0,
		eth0_2 ? eth0_2->stats.tx_packets : 0, eth0_2 ? eth0_2->stats.rx_packets : 0,
		eth0_2 ? eth0_2->stats.tx_dropped : 0);

	if (odi.state_dump_fire == 1)
		schedule_delayed_work(&odi.state_dump_work, msecs_to_jiffies(ODI_STATE_DUMP_RETRY_MS));
}

/* ---- net_device_ops ----------------------------------------------------
 *
 * Only the first opened device brings the shared hardware up; the rest
 * (already administratively down until their own ndo_open) just share the
 * ring set that is already running. netif_carrier is set on for every
 * device at open: the real link source is a switch-side interrupt
 * broadcaster this driver does not subscribe to -- this is an explicit
 * fallback, not an oversight.
 */

static atomic_t odi_open_count = ATOMIC_INIT(0);

static int odi_ndo_open(struct net_device *dev)
{
	pr_info(DRV_NAME ": ndo_open(%s), open_count was %d\n",
		dev->name, atomic_read(&odi_open_count));

	if (atomic_inc_return(&odi_open_count) == 1) {
		int ret = odi_rings_alloc(&odi_pdev->dev);

		if (ret) {
			pr_info(DRV_NAME ": odi_rings_alloc failed, ret %d\n", ret);
			odi_rings_free(&odi_pdev->dev);
			atomic_dec(&odi_open_count);
			return ret;
		}
		odi.napi_dev = dev;
		odi.irq_window_start = 0;
		odi.irq_window_count = 0;
		odi.irq_storm_tripped = 0;
		odi_irq_log_count = 0;
		odi_init_hw(dev);
		pr_info(DRV_NAME ": odi_init_hw done, requesting IRQ %d\n", ODI_NIC_IRQ_NUM);

		ret = request_irq(ODI_NIC_IRQ_NUM, odi_irq, IRQF_SHARED, DRV_NAME, &odi);
		if (ret) {
			pr_info(DRV_NAME ": request_irq failed, ret %d\n", ret);
			odi_rings_free(&odi_pdev->dev);
			atomic_dec(&odi_open_count);
			return ret;
		}
		odi.irq_requested = 1;
		napi_enable(&odi.napi);
		odi_start_hw();
		pr_info(DRV_NAME ": IRQ requested, NAPI enabled, hardware live\n");

		odi.irq_entries = 0;
		odi.napi_polls = 0;
		odi.state_dump_fire = 0;
		INIT_DELAYED_WORK(&odi.state_dump_work, odi_state_dump_work);
		schedule_delayed_work(&odi.state_dump_work, msecs_to_jiffies(ODI_STATE_DUMP_DELAY_MS));
	}

	netif_carrier_on(dev);
	netif_start_queue(dev);
	pr_info(DRV_NAME ": ndo_open(%s) done\n", dev->name);
	return 0;
}

static int odi_ndo_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	netif_carrier_off(dev);

	if (atomic_dec_return(&odi_open_count) == 0) {
		cancel_delayed_work_sync(&odi.state_dump_work);
		napi_disable(&odi.napi);
		/* After napi_disable and with every queue down, nothing can
		 * re-arm it; it must be gone before the rings are freed.
		 */
		cancel_delayed_work_sync(&odi.tx_stall_work);
		odi_stop_hw();
		if (odi.irq_requested) {
			free_irq(ODI_NIC_IRQ_NUM, &odi);
			odi.irq_requested = 0;
		}
		odi_rings_free(&odi_pdev->dev);
	}
	return 0;
}

static int odi_ndo_set_mac_address(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;
	eth_hw_addr_set(dev, addr->sa_data);

	/* Programmed into STATION0/STATION4 only for the device that owns the shared
	 * hardware state; the other four devices keep their own dev_addr for
	 * the stack's benefit (ARP replies, etc.) without a second MAC
	 * register set to write to -- there is only one.
	 */
	if (dev == odi.napi_dev) {
		odi_w32(ODI_NIC_STATION0, get_unaligned_be32(dev->dev_addr));
		odi_w32(ODI_NIC_STATION4, ((u32)dev->dev_addr[4] << 24) |
					((u32)dev->dev_addr[5] << 16));
	}
	return 0;
}

static void odi_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->bus_info, "platform", sizeof(info->bus_info));
}

static const struct ethtool_ops odi_ethtool_ops = {
	.get_drvinfo = odi_get_drvinfo,
};

static const struct net_device_ops odi_netdev_ops = {
	.ndo_open = odi_ndo_open,
	.ndo_stop = odi_ndo_stop,
	.ndo_start_xmit = odi_start_xmit,
	.ndo_set_mac_address = odi_ndo_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	/* .ndo_change_mtu unset: the core enforces min_mtu/max_mtu itself. */
};

/* ---- Module init / exit ------------------------------------------------ */

static const char * const odi_dev_names[ODI_NUM_DEVS] = {
	"eth0", "eth0.2", "eth0.3", "nas0", "pon0",
};
static const unsigned int odi_dev_ports[ODI_NUM_DEVS] = {
	ODI_PORT_CATCHALL, ODI_PORT_LAN0, ODI_PORT_LAN1, ODI_PORT_NONE, ODI_PORT_NONE,
};

static int __init odi_nic_init(void)
{
	int i, ret;

	odi_mmio = ioremap(CPHYSADDR(ODI_NIC_MMIO_BASE), ODI_NIC_MMIO_SIZE);
	if (!odi_mmio) {
		pr_err(DRV_NAME ": failed to map MMIO window\n");
		return -ENODEV;
	}

	odi_pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(odi_pdev)) {
		pr_err(DRV_NAME ": failed to register platform device\n");
		ret = PTR_ERR(odi_pdev);
		odi_pdev = NULL;
		goto err_unmap;
	}
	/* dma_alloc_coherent()/dma_map_single() need a coherent_dma_mask; a
	 * plain platform_device_register_simple() device has none set, which
	 * is the same NULL-shaped hole this device exists to close. 32 bits
	 * covers the stick's memory map many times over.
	 *
	 * dma_set_mask() itself needs dev->dma_mask to be a valid pointer to
	 * write through -- platform_device_register_simple() does not set
	 * one, only the fuller platform_device_register_full() path does.
	 * Without this line dma_set_mask() returns -EIO immediately (it
	 * checks dev->dma_mask is non-NULL before anything else), which
	 * failed odi_nic_init() outright on the second (n2) trial on isp1:
	 * "odi_nic: failed to set DMA mask" was the only odi_nic line in the
	 * RAM log, no net devices were registered at all, and everything
	 * downstream (network.sh, the watchdog ARP check) had nothing to
	 * work with. Found in the RAM log after the trial, NOT re-tested on
	 * hardware -- the two-trial budget for this task was already spent.
	 */
	odi_pdev->dev.dma_mask = &odi_pdev->dev.coherent_dma_mask;
	ret = dma_set_coherent_mask(&odi_pdev->dev, DMA_BIT_MASK(32));
	if (!ret)
		ret = dma_set_mask(&odi_pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		pr_err(DRV_NAME ": failed to set DMA mask\n");
		goto err_pdev;
	}

	spin_lock_init(&odi.lock);
	odi_nic_rxhook_reset();
	INIT_DELAYED_WORK(&odi.tx_stall_work, odi_tx_stall_work);

	/* Before the net devices: the first ndo_open starts the DMA engine,
	 * and from then on a reboot, panic or watchdog reset must stop it.
	 * Unwound below if a net device then fails.
	 */
	register_reboot_notifier(&odi_reboot_nb);
	atomic_notifier_chain_register(&panic_notifier_list, &odi_panic_nb);
	wdt_pre_reset_hook = odi_quiesce_hw;

	for (i = 0; i < ODI_NUM_DEVS; i++) {
		struct net_device *dev = alloc_etherdev(sizeof(struct odi_port));
		struct odi_port *p;

		if (!dev) {
			ret = -ENOMEM;
			goto err_netdevs;
		}
		strscpy(dev->name, odi_dev_names[i], IFNAMSIZ);
		dev->netdev_ops = &odi_netdev_ops;
		dev->ethtool_ops = &odi_ethtool_ops;
		eth_hw_addr_random(dev);	/* placeholder; network.sh sets the real one */

		p = netdev_priv(dev);
		p->dev = dev;
		p->port = odi_dev_ports[i];
		odi.ports[i].dev = dev;
		odi.ports[i].port = odi_dev_ports[i];

		if (i == 0) {
			/* NAPI is registered once, against the first (catch-all)
			 * device; it services the one shared ring set.
			 */
			netif_napi_add_weight(dev, &odi.napi, odi_poll, NAPI_POLL_WEIGHT);
		}

		ret = register_netdev(dev);
		if (ret) {
			free_netdev(dev);
			odi.ports[i].dev = NULL;
			goto err_netdevs;
		}
	}

	pr_info(DRV_NAME ": %d net devices registered, IRQ %d\n", ODI_NUM_DEVS, ODI_NIC_IRQ_NUM);
	return 0;

err_netdevs:
	for (i = 0; i < ODI_NUM_DEVS; i++) {
		if (odi.ports[i].dev) {
			unregister_netdev(odi.ports[i].dev);
			free_netdev(odi.ports[i].dev);
			odi.ports[i].dev = NULL;
		}
	}
	wdt_pre_reset_hook = NULL;
	atomic_notifier_chain_unregister(&panic_notifier_list, &odi_panic_nb);
	unregister_reboot_notifier(&odi_reboot_nb);
err_pdev:
	platform_device_unregister(odi_pdev);
	odi_pdev = NULL;
err_unmap:
	iounmap(odi_mmio);
	odi_mmio = NULL;	/* odi_quiesce_hw() tests it */
	return ret;
}

static void __exit odi_nic_exit(void)
{
	int i;

	/* Safety net: a normal rmmod always goes through ndo_stop first (which
	 * already cancels this), but a forced unload with devices still up
	 * must not leave the timer running against freed state.
	 */
	cancel_delayed_work_sync(&odi.state_dump_work);
	cancel_delayed_work_sync(&odi.tx_stall_work);

	wdt_pre_reset_hook = NULL;
	atomic_notifier_chain_unregister(&panic_notifier_list, &odi_panic_nb);
	unregister_reboot_notifier(&odi_reboot_nb);
	odi_nic_rxhook_reset();
	for (i = 0; i < ODI_NUM_DEVS; i++) {
		if (odi.ports[i].dev) {
			unregister_netdev(odi.ports[i].dev);
			free_netdev(odi.ports[i].dev);
		}
	}
	if (odi_pdev) {
		platform_device_unregister(odi_pdev);
		odi_pdev = NULL;
	}
	iounmap(odi_mmio);
}

module_init(odi_nic_init);
module_exit(odi_nic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Open driver for the RTL9602C CPU-port NIC");
MODULE_AUTHOR("odi-oss");
