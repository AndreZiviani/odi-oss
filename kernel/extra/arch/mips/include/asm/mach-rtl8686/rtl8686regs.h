/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686) register map.
 *
 * Addresses are written as their KSEG1 (uncached) alias; RTL8686_PHYS()
 * turns them into the physical address.
 */
#ifndef __ASM_MACH_RTL8686_RTL8686REGS_H
#define __ASM_MACH_RTL8686_RTL8686REGS_H

#define RTL8686_PHYS(kseg1_addr)	((kseg1_addr) & 0x1fffffff)

/* UART0 -- 8250-compatible, 32-bit register stride (regshift 2) */
#define RTL8686_UART0_BASE	RTL8686_PHYS(0xB8002000)
#define RTL8686_UART0_THR	(RTL8686_UART0_BASE + 0x000)
#define RTL8686_UART0_LSR	(RTL8686_UART0_BASE + 0x014)
#define RTL8686_UART0_LSR_THRE	BIT(5)

/* Board interrupt controller: GIMR/GISR pairs for 64 sources (bit N is
 * source N; N >= 32 in GIMR1/GISR1), and seven IRR registers that route
 * each source to a CPU pin IP2-IP7, 4 bits per source.
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

/* Board IRQ sources 0-63; the Linux virq is RTL8686_IRQ_BASE + source. */
#define RTL8686_IRQ_UTMD	2
#define RTL8686_IRQ_FLSH	3
#define RTL8686_IRQ_SWITCH	8
#define RTL8686_IRQ_GPIO1	10
#define RTL8686_IRQ_PERIPHERAL	12
#define RTL8686_IRQ_GMAC	26
#define RTL8686_IRQ_TMO		31
#define RTL8686_IRQ_WDT_PH1TO	39
#define RTL8686_IRQ_TIMER0		43
#define RTL8686_IRQ_TIMER1		44
#define RTL8686_IRQ_UART0	49
#define RTL8686_IRQ_UART1	50

/* The routing read back from the device: every source on IP2 except
 * UART0 (IP3) and TIMER0 (IP7). All six pins chain to the same dispatch
 * (irq.c), so the route does not change which handler runs.
 */
#define RTL8686_IRR0_VAL	0x22222222u
#define RTL8686_IRR1_VAL	0x22222222u
#define RTL8686_IRR2_VAL	0x22222222u
#define RTL8686_IRR3_VAL	0x22222222u
#define RTL8686_IRR4_VAL	0x32222272u /* nibble7=UART0(3), nibble1=TIMER0(7) */
#define RTL8686_IRR5_VAL	0x22222222u
#define RTL8686_IRR6_VAL	0x00000022u

/* TIMER0, the system timer. Older chips of the family place their timer
 * elsewhere; only the RTL9602C layout is described.
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

/* TIMER1: same per-timer block layout as TIMER0, 0x10 bytes further on.
 * Used as a free-running clocksource, no interrupt (time.c). DATA is the
 * 28-bit reload value, CNT the live count, CTRL the enable/mode/divisor
 * word, INT the interrupt enable and pending bits.
 */
#define RTL8686_TIMER1_BASE		(RTL8686_TIMER_BASE + 0x10)
#define RTL8686_TIMER_DATA		0x00
#define RTL8686_TIMER_CNT		0x04
#define RTL8686_TIMER_CTRL		0x08
#define RTL8686_TIMER_INT		0x0C
#define RTL8686_TIMER_BLOCK_SIZE	0x10
#define RTL8686_TIMER1_DIV		64

/* SPI NOR flash: the memory-mapped read window, valid once
 * rtl8686-spiflash.c has configured it (not CFI). U-Boot boots the slots
 * with bootm 0x94080000 and 0x94440000, k0 and k1 in this window.
 */
#define RTL8686_NOR_BASE	RTL8686_PHYS(0xB4000000)
#define RTL8686_NOR_SIZE	(32 * 1024 * 1024)

/* The SPI controller behind it: window configuration and the command
 * engine that erases and programs. The timing register at +0x00 is set
 * by the boot ROM and U-Boot and is not touched.
 */
#define RTL8686_SF_BASE		RTL8686_PHYS(0xB8001200)
#define RTL8686_SF_MAPCFG	0x04	/* read-window configuration */
#define RTL8686_SF_CMDCTL	0x08	/* command engine control and status */
#define RTL8686_SF_CMDDATA	0x0C	/* command engine data */
#define RTL8686_SF_REGS_SIZE	0x10

/* The CPU-port NIC; odi_nic drives it, board code only stops it. Its DMA
 * engine is not reset with the CPU and the loader leaves it running (read
 * at prom_init() on ISP1: RUN 0x400f3330, RUN1 0x30000000, a 16-entry RX
 * ring at KSEG1 0xa1c2dc00). Zero in both RUN registers stops RX and TX
 * DMA. docs/KERNEL.md "The board".
 */
#define RTL8686_NIC_BASE	RTL8686_PHYS(0xB8012000)
#define RTL8686_NIC_RUN		0x1434
#define RTL8686_NIC_RUN1	0x1438

/* The LX bus clock, source of UART0 and TIMER0. Measured: a HZ=100 tick
 * takes prescaler 1000 and TIMER0_PERIOD 2000, i.e. 200 MHz, and UART0
 * keeps its baud rate on the same clock.
 */
#define RTL8686_LX_HZ		200000000u

/* PBO (packet-buffer offload) DMA windows of the PON MAC, downstream (DL)
 * and upstream (UL), each 1 MiB plus a 4 KB barrier page, reserved in
 * plat_mem_setup(). The bases are what every replayed switch init writes
 * to the two PBO base registers: 0x01eff000 to UL (0xf020e8), 0x016ff000
 * to DL (0xf0a0b8) (test/fixtures/isp1-260923-g4-sdkinit-filtered.txt):
 *   DL: [0x016ff000, 0x01800000)   UL: [0x01eff000, 0x02000000)
 * The ramlog pages 0x017ff000 and 0x01fff000 are the barrier pages, so
 * these reservations protect them too.
 */
#define RTL8686_PBO_BARRIER		0x1000u
#define RTL8686_PBO_DL_SIZE		(0x100000u + RTL8686_PBO_BARRIER)
#define RTL8686_PBO_UL_SIZE		(0x100000u + RTL8686_PBO_BARRIER)
#define RTL8686_PBO_DL_OFFSET		0x16ff000u	/* 32 MB - 8 MB bank - DL_SIZE */

#endif /* __ASM_MACH_RTL8686_RTL8686REGS_H */
