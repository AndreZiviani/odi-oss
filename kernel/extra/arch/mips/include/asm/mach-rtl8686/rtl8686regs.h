/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686) register map.
 *
 * Register offsets and the IRQ routing table below are hardware facts
 * fixed by this SoC's boot ROM/silicon, not something this port gets to
 * redesign. All addresses are KSEG1 (0xB8xxxxxx), i.e. already the
 * uncached-direct-mapped physical alias -- ioremap() below strips the
 * high bits back to the physical address the same way every other MIPS
 * register header does.
 */
#ifndef __ASM_MACH_RTL8686_RTL8686REGS_H
#define __ASM_MACH_RTL8686_RTL8686REGS_H

#define RTL8686_PHYS(kseg1_addr)	((kseg1_addr) & 0x1fffffff)

/* UART0 -- 8250-compatible, 32-bit register stride (regshift 2) */
#define RTL8686_UART0_BASE	RTL8686_PHYS(0xB8002000)
#define RTL8686_UART0_THR	(RTL8686_UART0_BASE + 0x000)
#define RTL8686_UART0_LSR	(RTL8686_UART0_BASE + 0x014)
#define RTL8686_UART0_LSR_THRE	BIT(5)

/* Board-level interrupt controller: two 32-bit GIMR/GISR register pairs
 * covering all 64 logical board IRQ sources directly (bit N = source N,
 * N<32 in GIMR0/GISR0, N>=32 in GIMR1/GISR1), plus seven IRR routing
 * registers that steer each source to one of CPU interrupt pins IP2-IP7
 * (4 bits per source, see the IRRn_VAL values below).
 */
#define RTL8686_GIMR0		RTL8686_PHYS(0xB8003000)
#define RTL8686_GIMR1		RTL8686_PHYS(0xB8003004)
#define RTL8686_GISR0		RTL8686_PHYS(0xB8003008)
#define RTL8686_GISR1		RTL8686_PHYS(0xB800300C)
#define RTL8686_IRR0		RTL8686_PHYS(0xB8003010)
#define RTL8686_IRR1		RTL8686_PHYS(0xB8003014)
#define RTL8686_IRR2		RTL8686_PHYS(0xB8003018)
#define RTL8686_IRR3		RTL8686_PHYS(0xB800301C)
#define RTL8686_IRR4		RTL8686_PHYS(0xB8003020)
#define RTL8686_IRR5		RTL8686_PHYS(0xB8003024)
#define RTL8686_IRR6		RTL8686_PHYS(0xB8003028)
#define RTL8686_IRQREGS_SIZE	0x2C

/* Logical board IRQ numbers (0-63, offset by RTL8686_IRQ_BASE for the
 * Linux virq).
 */
#define RTL8686_IRQ_UTMD	2
#define RTL8686_IRQ_FLSH	3
#define RTL8686_IRQ_SWITCH	8
#define RTL8686_IRQ_GPIO1	10
#define RTL8686_IRQ_PERIPHERAL	12
#define RTL8686_IRQ_TMO		31
#define RTL8686_IRQ_WDT_PH1TO	39
#define RTL8686_IRQ_TIMER0		43
#define RTL8686_IRQ_TIMER1		44
#define RTL8686_IRQ_UART0	49
#define RTL8686_IRQ_UART1	50

/* Each nibble is the CPU-pin route of one board IRQ source (2-7), as
 * actually programmed on the device. Only UART0 (nibble in IRR4, routed
 * to IP3) and TIMER0 (also IRR4, routed to IP7) differ from the otherwise
 * uniform IP2 every other source routes to; every CPU pin IP2-IP7 is
 * chained to the same dispatch function below regardless, so this
 * difference does not change which handler runs, only bookkeeping.
 */
#define RTL8686_IRR0_VAL	0x22222222u
#define RTL8686_IRR1_VAL	0x22222222u
#define RTL8686_IRR2_VAL	0x22222222u
#define RTL8686_IRR3_VAL	0x22222222u
#define RTL8686_IRR4_VAL	0x32222272u /* nibble7=UART0(3), nibble1=TIMER0(7) */
#define RTL8686_IRR5_VAL	0x22222222u
#define RTL8686_IRR6_VAL	0x00000022u

/* System timer (TIMER0) register layout of this SoC: TIMER0_PERIOD, TIMER0_CTRL and
 * TIMER0_IRQ at 0xB8003200, +0x08 and +0x0C. Older chips of the family place
 * their timer differently; that layout is not used on the RTL9602C and
 * is not described here.
 */
#define RTL8686_TIMER_BASE		RTL8686_PHYS(0xB8003200)
#define RTL8686_TIMER0_PERIOD	(RTL8686_TIMER_BASE + 0x00)
#define RTL8686_TIMER0_CTRL		(RTL8686_TIMER_BASE + 0x08)
#define RTL8686_TIMER0_IRQ		(RTL8686_TIMER_BASE + 0x0C)
#define RTL8686_TIMER_EN		BIT(28)
#define RTL8686_TIMER_PERIODIC	BIT(24)
#define RTL8686_TIMER_IRQ_EN		BIT(20)
#define RTL8686_TIMER_IRQ_PEND		BIT(16)
#define RTL8686_TIMER_PRESCALE	1000

/* SPI NOR flash: the memory-mapped read window (raw bytes, valid only
 * once drivers/mtd/devices/rtl8686-spiflash.c has opened it; the flash is
 * not CFI-probeable) ...
 */
/* KSEG1 0xB4000000: U-Boot boots the slots with bootm 0x94080000 and
 * 0x94440000, the flash offsets of k0 and k1 in this window.
 */
#define RTL8686_NOR_BASE	RTL8686_PHYS(0xB4000000)
#define RTL8686_NOR_SIZE	(32 * 1024 * 1024)

/* ... and the SPI controller behind it, at 0xB8001200: the window
 * configuration and the command engine that erases and programs. Only
 * the three registers the driver uses are named; the controller timing
 * register at +0x00 is already set by the boot ROM and U-Boot.
 */
#define RTL8686_SF_BASE		RTL8686_PHYS(0xB8001200)
#define RTL8686_SF_MAPCFG	0x04	/* read-window configuration */
#define RTL8686_SF_CMDCTL	0x08	/* command engine control and status */
#define RTL8686_SF_CMDDATA	0x0C	/* command engine data */
#define RTL8686_SF_REGS_SIZE	0x10

/* The CPU-port NIC -- odi_nic drives it; board code only ever stops it.
 * Its DMA engine is not reset with the CPU, and the loader that ran before
 * this kernel leaves it running: on ISP1, 2026-09-24, prom_init() read
 * RUN 0x400f3330, RUN1 0x30000000 and an RX ring of 16 descriptors at
 * 0xa1c2dc00 (a KSEG1 pointer, which the stock kernel never programs;
 * U-Boot's driver is the likely owner). Zero in both run registers stops
 * RX and TX DMA -- odi_quiesce_hw() in odi_nic.c does the same before our
 * own resets.
 */
#define RTL8686_NIC_BASE	RTL8686_PHYS(0xB8012000)
#define RTL8686_NIC_RUN		0x1434
#define RTL8686_NIC_RUN1	0x1438

/* The LX bus clock, the source of both UART0 and TIMER0. It is
 * a constant on this board: measured on the device, TIMER0 runs a tick of
 * HZ 100 with a prescaler of 1000 and a TIMER0_PERIOD of 2000, which is 200
 * MHz, and the UART0 console keeps its baud rate on the same clock.
 */
#define RTL8686_LX_HZ		200000000u

/* PBO (packet-buffer-offload) DMA windows -- the DL (downstream) and UL
 * (upstream) DRAM ranges the PON MAC's PBO engine claims, carved out of
 * general-purpose RAM by memblock_reserve() in plat_mem_setup() (board.c)
 * rather than handed to the page allocator. Each is 0x100000 (1 MiB) of
 * window plus a PAGE_SIZE barrier above it.
 *
 * The window bases are whatever the replayed switch init writes to the
 * two PBO base registers, and every capture writes the same pair: the UL
 * base register (0xf020e8) gets 0x01eff000 and the DL one (0xf0a0b8)
 * gets 0x016ff000 (test/fixtures/isp1-260923-g4-sdkinit-filtered.txt).
 * Both follow one arithmetic rule, cross-checked against our own register
 * captures: UL = mem_size - (1 MiB + 4 KB), DL = mem_size - bank_size -
 * (1 MiB + 4 KB), with bank_size = mem_size / 4 on this 4-bank 32 MB part.
 * So
 *   DL: [0x016ff000, 0x01800000)   UL: [0x01eff000, 0x02000000)
 * The DL reservation used to start at 0x01700000, one page short of the
 * arithmetic above: pfn 0x16ff, the first page the engine owns, went to
 * the page allocator.
 *
 * Both DRAM pages this board's own ramlog uses (0x017ff000 and
 * 0x01fff000) are the barrier pages of these two windows, so reserving
 * the windows already protects them from the allocator -- no separate
 * reservation needed.
 */
#define RTL8686_PBO_BARRIER		0x1000u
#define RTL8686_PBO_DL_SIZE		(0x100000u + RTL8686_PBO_BARRIER)
#define RTL8686_PBO_UL_SIZE		(0x100000u + RTL8686_PBO_BARRIER)
#define RTL8686_PBO_DL_OFFSET		0x16ff000u	/* 32 MB - 8 MB bank - DL_SIZE */

#endif /* __ASM_MACH_RTL8686_RTL8686REGS_H */
