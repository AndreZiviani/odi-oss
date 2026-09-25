// SPDX-License-Identifier: GPL-2.0
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686) board file.
 *
 * No device tree: this is a single fixed board with no variants (one SoC,
 * one memory size, one UART, one NOR flash window), so a plain board file
 * -- prom_init()/plat_mem_setup()/arch_init_irq()/plat_time_init() plus
 * platform_device registration in a device_initcall -- is simpler than a
 * DT the board will never need a second copy of.
 *
 * Board init in one file. GPIO/LED/USB-PHY/
 * pushbutton board code is NOT ported here -- out of scope for this
 * build. The switch/NIC/GPON/OMCI/watchdog/ramlog drivers this board
 * needs are separate files under drivers/net/ethernet/odi/ (CONFIG_ODI_*),
 * built in alongside this board file, not part of it.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>
#include <linux/irqchip.h>
#include <linux/io.h>

#include <asm/bootinfo.h>
#include <asm/cacheflush.h>
#include <asm/fw/fw.h>
#include <asm/time.h>
#include <asm/reboot.h>
#include <asm/irq_cpu.h>
#include <asm/wbflush.h>

#include <asm/mach-rtl8686/rtl8686regs.h>
#include <asm/mach-rtl8686/rtl8686-barrier.h>
#include <asm/mach-rtl8686/odi-early-crumb.h>
#ifdef CONFIG_ODI_EARLY_CRUMBS
#include <asm/setup.h>
#include <asm/mach-rtl8686/odi-irq-debug.h>
#endif

#include "rtl8686.h"

#ifdef CONFIG_ODI_BOARD
/*
 * The captured LED/I2C-core register replay
 * (drivers/net/ethernet/odi/odi_board.c). A fresh call here: there is no
 * prior board init path on this port to hook into instead.
 */
extern void odi_board_init(void);
#endif

#ifdef CONFIG_ODI_RAMLOG
/* odi-oss: the earliest available capture point on this port -- see the
 * prom_init() call below and odi_ramlog.h for the full early-console
 * design.
 */
extern void odi_ramlog_early_console_init(void);
#endif

/* 32 MB DRAM, the only configuration of this board. */
#define RTL8686_MEM_SIZE	(32 << 20)

/* The LX bus clock, which drives both UART0 and the TIMER0 system timer
 * (time.c reads it through this symbol). rtl8686regs.h has the
 * measurement behind the constant.
 */
unsigned int rtl8686_cpu_hz = RTL8686_LX_HZ;

/* The CPU is built as CPU_R3000, which selects CPU_HAS_WB: mb(), iob()
 * and the barrier readl()/writel() put before each access all call
 * through this pointer. Here it drains the write buffer with a sync,
 * the instruction those barriers were with CPU_HAS_SYNC (which
 * CPU_R3000 turns off). Set statically, so it is valid from the first
 * instruction: cache.c calls iob() from plat_mem_setup().
 */
static void rtl8686_wbflush(void)
{
	rtl8686_sync();
}

void (*__wbflush)(void) = rtl8686_wbflush;

const char *get_system_type(void)
{
	return "Realtek RTL9602C (ODI DFP-34X-2C2)";
}

void __init prom_init(void)
{
	/* U-Boot bootm passes the command line as argc/argv in a0/a1, built
	 * per boot slot: the running kernel/rootfs pair is marked ro, the
	 * linux/rootfs aliases point at it and root= follows (31:5 for slot
	 * 0, 31:7 for slot 1). CONFIG_CMDLINE is the slot-0 line and only a
	 * fallback when no argv arrives (MIPS_CMDLINE_FROM_BOOTLOADER).
	 * Booting slot 1 on the CONFIG_CMDLINE fallback line mounts the
	 * wrong root. Copy argv before anything else in prom_init touches
	 * DRAM, the argv block lives in memory U-Boot owned.
	 */
	fw_init_cmdline();
	ODI_EARLY_CRUMB("K8PB", 8);	/* prom_init() entered, argv copied */

	/* Stop the NIC DMA the loader left running (rtl8686regs.h), before
	 * this kernel owns a single page. odi_nic resets the NIC only at its
	 * first ndo_open, in rcS: until then every frame the NIC took in went
	 * through the loader descriptors, the payload into its buffers and
	 * the descriptor write-back into its ring slot -- by then page-cache
	 * pages of ours. On 618n1/618n2 the ring slot at 0x01c2dc00 was
	 * busybox text (0x421c00), and every busybox exec after that took
	 * the same SIGBUS.
	 */
	__raw_writel(0, (void __iomem *)CKSEG1ADDR(RTL8686_NIC_BASE + RTL8686_NIC_RUN));
	__raw_writel(0, (void __iomem *)CKSEG1ADDR(RTL8686_NIC_BASE + RTL8686_NIC_RUN1));

#ifdef CONFIG_ODI_RAMLOG
	/* Earliest board hook available on this port -- see odi_ramlog.h and
	 * odi_ramlog.c for why this is not literal head.S HEAD-tag timing
	 * (unreachable without an assembly hunk, out of scope here), but is
	 * the earliest point prom_init()'s own C entry gives us, well before
	 * console_initcall() (the previous best available). Stamps both DRAM
	 * pages and registers a CON_BOOT console immediately, so a hang
	 * anywhere from here through console_init() still leaves a log; the
	 * console_initcall() registration further down hands over to the
	 * final non-boot console with no duplicate lines (printk's own
	 * register_console() unregisters CON_BOOT consoles when a non-boot
	 * one registers).
	 */
	odi_ramlog_early_console_init();
#endif
	/* After the ramlog init, which clears the page B header words:
	 * from here on printk reaches DRAM too.
	 */
	ODI_EARLY_CRUMB("K9PA", 9);	/* prom_init() about to return */
}

/* SYS_HAS_EARLY_PRINTK (selected by MACH_RTL8686) needs this: a byte-at-a-
 * time polling write straight to the fixed KSEG1 UART0 window, valid from
 * the earliest boot (no ioremap needed, KSEG1 is always direct-mapped
 * uncached). Only for output before the real 8250 driver (rtl8686_uart0_
 * device below) is probed.
 */
void prom_putchar(char c)
{
	void __iomem *lsr = (void __iomem *)CKSEG1ADDR(RTL8686_UART0_LSR);
	void __iomem *thr = (void __iomem *)CKSEG1ADDR(RTL8686_UART0_THR);
	unsigned int timeout = 100000;

	while (!(readl(lsr) & RTL8686_UART0_LSR_THRE) && --timeout)
		;
	writel(c & 0xff, thr);
}

/*
 * free_initmem() calls this first, just before the __init sections go
 * back to the page allocator. Every line the icache holds for __init code
 * is dropped here, once, so a freed init page never depends on the
 * per-page hook in cache.c to be safe to run from user space. Not __init
 * itself: the return path must not refill an init line after the flush.
 */
void prom_free_prom_memory(void)
{
	flush_icache_all();
}

void __init plat_mem_setup(void)
{
	ODI_EARLY_CRUMB("KAMS", 10);	/* plat_mem_setup() entered */
	memblock_add(0, RTL8686_MEM_SIZE);

	/* PBO DMA windows -- rtl8686regs.h has the derivation. Both sit one
	 * window-plus-barrier below a boundary: the UL one below the top of
	 * RAM, the DL one below the last 8 MB DRAM bank. Without these, the
	 * PON MAC's PBO engine and the page allocator share physical RAM.
	 */
	memblock_reserve(RTL8686_PBO_DL_OFFSET, RTL8686_PBO_DL_SIZE);
	memblock_reserve(RTL8686_MEM_SIZE - RTL8686_PBO_UL_SIZE, RTL8686_PBO_UL_SIZE);

	/* The caches, from here rather than from mainline cpu_cache_init():
	 * see cache.c. Earlier than cpu_cache_init() ran it, which only
	 * means the flush hooks are live sooner.
	 */
	rlx5281_cache_init();
	ODI_EARLY_CRUMB("KBCI", 11);	/* caches installed */
}

static void rtl8686_machine_restart(char *command)
{
	/* odi_wdt.c (CONFIG_ODI_WDT) drives the hardware watchdog, but no
	 * restart handler wires it to this hook yet, so `reboot` still has
	 * no watchdog-triggered reset path here. Spin instead of
	 * silently returning, matching that state honestly
	 * rather than pretending to restart.
	 */
	local_irq_disable();
	while (1)
		;
}

static void rtl8686_machine_halt(void)
{
	local_irq_disable();
	while (1)
		;
}

static int __init rtl8686_reboot_setup(void)
{
	_machine_restart = rtl8686_machine_restart;
	_machine_halt = rtl8686_machine_halt;
	return 0;
}
arch_initcall(rtl8686_reboot_setup);

/*
 * IRQ. mips_cpu_irq_init() (drivers/irqchip/irq-mips-cpu.c,
 * CONFIG_IRQ_MIPS_CPU) brings up the 8 CPU-level interrupt pins at
 * MIPS_CPU_IRQ_BASE..+7; rtl8686_irq_init() (irq.c) brings up the
 * board-level GIMR0/GIMR1 controller at RTL8686_IRQ_BASE..+63 and chains
 * it onto the CPU pins the routing table assigns: IP2 (most sources), IP3
 * (UART0), IP5 (PCIe), IP6 (switch), IP7 (TIMER0, the system timer).
 */

void __init arch_init_irq(void)
{
	ODI_EARLY_CRUMB("KCIR", 12);	/* arch_init_irq() entered */
	mips_cpu_irq_init();
	rtl8686_irq_init();
#ifdef CONFIG_ODI_EARLY_CRUMBS
	/* The IRQE stub in front of handle_int (crumb-int.S). trap_init()
	 * has already installed handle_int; interrupts are still off.
	 */
	set_except_vector(EXCCODE_INT, odi_crumb_handle_int);
#endif
	ODI_EARLY_CRUMB("KDIR", 13);	/* arch_init_irq() about to return */
}

/*
 * Timer. time.c programs TIMER0 for a fixed
 * HZ-periodic tick (this hardware has no usable oneshot/programmable-
 * compare mode reachable from Linux -- periodic is not a shortcut here,
 * it is what the hardware offers) and registers the RTL8686_IRQ_BASE-
 * relative TIMER0 IRQ.
 */

#ifdef CONFIG_ODI_EARLY_CRUMBS
/* Through late_time_init: start_kernel() calls it with interrupts on,
 * right before calibrate_delay(), which is where a dead timer hangs.
 */
static void __init rtl8686_irq_debug_dump(void)
{
	ODI_EARLY_CRUMB("KGLT", 16);	/* late_time_init reached */
	odi_irq_debug_dump();
}
#endif

void __init plat_time_init(void)
{
	ODI_EARLY_CRUMB("KETM", 14);	/* plat_time_init() entered */
	rtl8686_clockevent_init();
#ifdef CONFIG_ODI_EARLY_CRUMBS
	late_time_init = rtl8686_irq_debug_dump;
#endif
	ODI_EARLY_CRUMB("KFTM", 15);	/* plat_time_init() about to return */
}

/*
 * Platform devices: UART0 (8250). The memory-mapped NOR flash window
 * (drivers/mtd/devices/rtl8686-spiflash.c) is self-contained -- fixed physical
 * window, own module_init(), no platform_device needed for a single
 * always-present MTD map.
 */
static struct plat_serial8250_port rtl8686_uart0_data[] = {
	{
		.mapbase	= RTL8686_UART0_BASE,
		.irq		= RTL8686_IRQ_BASE + RTL8686_IRQ_UART0,
		.uartclk	= RTL8686_LX_HZ,
		.regshift	= 2,
		.iotype		= UPIO_MEM32,
		.flags		= UPF_SKIP_TEST | UPF_IOREMAP | UPF_FIXED_TYPE,
		.type		= PORT_16550A,
	},
	{ },
};

static struct platform_device rtl8686_uart0_device = {
	.name	= "serial8250",
	.id	= PLAT8250_DEV_PLATFORM,
	.dev	= {
		.platform_data = rtl8686_uart0_data,
	},
};

static int __init rtl8686_devices_init(void)
{
	platform_device_register(&rtl8686_uart0_device);

#ifdef CONFIG_ODI_BOARD
	odi_board_init();
#endif

	return 0;
}
device_initcall(rtl8686_devices_init);
