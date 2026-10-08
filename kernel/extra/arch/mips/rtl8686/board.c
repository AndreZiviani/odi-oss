// SPDX-License-Identifier: GPL-2.0
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686) board file.
 *
 * One fixed board with no variants, so no device tree. The networking,
 * watchdog and ramlog drivers are in drivers/net/ethernet/odi/.
 * docs/KERNEL.md "The board".
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
#include <asm/idle.h>
#include <asm/time.h>
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

#ifdef CONFIG_ODI_SWITCH
/* The LED/I2C-core register replay, drivers/net/ethernet/odi/odi_board.c. */
extern void odi_board_init(void);
#endif

#ifdef CONFIG_ODI_RAMLOG
extern void odi_ramlog_early_console_init(void);
#endif

/* 32 MB DRAM, the only configuration of this board. */
#define RTL8686_MEM_SIZE	(32 << 20)

/* The LX bus clock (the clock UART0 and the SoC timers count from), not
 * the CPU clock; time.c reads it here.
 */
unsigned int rtl8686_lx_hz = RTL8686_LX_HZ;

/* CPU_R3000 selects CPU_HAS_WB, so mb(), iob() and the barrier before
 * each readl()/writel() call through this pointer; a sync drains the
 * write buffer. Set statically: cache.c calls iob() from plat_mem_setup().
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
	/* U-Boot passes a per-slot command line in argv (root=31:5 slot 0,
	 * 31:7 slot 1); CONFIG_CMDLINE is only the slot-0 fallback. Copy it
	 * first: the argv block lives in memory U-Boot owned.
	 */
	fw_init_cmdline();
	ODI_EARLY_CRUMB("K8PB", 8);	/* prom_init() entered, argv copied */

	/* Stop the NIC DMA the loader left running before this kernel owns a
	 * page: until odi_nic resets it at its first ndo_open, every frame is
	 * written through the loader ring into our pages. docs/KERNEL.md
	 * "The board".
	 */
	__raw_writel(0, (void __iomem *)CKSEG1ADDR(RTL8686_NIC_BASE + RTL8686_NIC_RUN));
	__raw_writel(0, (void __iomem *)CKSEG1ADDR(RTL8686_NIC_BASE + RTL8686_NIC_RUN1));

#ifdef CONFIG_ODI_RAMLOG
	/* The earliest C hook: stamps both ramlog pages and registers a
	 * CON_BOOT console, so a hang before console_init() still leaves a
	 * log. The console_initcall() registration replaces it without
	 * duplicate lines.
	 */
	odi_ramlog_early_console_init();
#endif
	/* After the ramlog init, which clears the page B header words. */
	ODI_EARLY_CRUMB("K9PA", 9);	/* prom_init() about to return */
}

/* Early printk: a polled write to UART0 through KSEG1, usable before
 * ioremap and before the 8250 driver probes.
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
 * free_initmem() calls this just before the __init sections are freed:
 * drop every icache line of __init code once. Not __init itself, so the
 * return path refills no init line after the flush.
 */
void prom_free_prom_memory(void)
{
	flush_icache_all();
}

void __init plat_mem_setup(void)
{
	ODI_EARLY_CRUMB("KAMS", 10);	/* plat_mem_setup() entered */
	memblock_add(0, RTL8686_MEM_SIZE);

	/* The PON MAC PBO DMA windows (rtl8686regs.h): UL below the top of
	 * RAM, DL below the last 8 MB bank.
	 */
	memblock_reserve(RTL8686_PBO_DL_OFFSET, RTL8686_PBO_DL_SIZE);
	memblock_reserve(RTL8686_MEM_SIZE - RTL8686_PBO_UL_SIZE, RTL8686_PBO_UL_SIZE);

	/* Installed here, not by cpu_cache_init(): see cache.c. */
	rlx5281_cache_init();
	ODI_EARLY_CRUMB("KBCI", 11);	/* caches installed */
}

/*
 * No _machine_restart or _machine_halt hook: machine_restart() runs the
 * restart handler of odi_wdt.c, and halt/power-off end in machine_hang(),
 * where the unkicked watchdog resets the board.
 */

/*
 * The 8 CPU pins at MIPS_CPU_IRQ_BASE, then the board controller (irq.c)
 * at RTL8686_IRQ_BASE, chained onto IP2-IP7.
 */

void __init arch_init_irq(void)
{
	ODI_EARLY_CRUMB("KCIR", 12);	/* arch_init_irq() entered */
	mips_cpu_irq_init();
	rtl8686_irq_init();
#ifdef CONFIG_ODI_EARLY_CRUMBS
	/* The IRQE stub in front of handle_int (crumb-int.S); interrupts
	 * are still off.
	 */
	set_except_vector(EXCCODE_INT, odi_crumb_handle_int);
#endif
	ODI_EARLY_CRUMB("KDIR", 13);	/* arch_init_irq() about to return */
}

#ifdef CONFIG_ODI_EARLY_CRUMBS
/* late_time_init runs with interrupts on, right before
 * calibrate_delay(), which is where a dead timer hangs.
 */
static void __init rtl8686_irq_debug_dump(void)
{
	ODI_EARLY_CRUMB("KGLT", 16);	/* late_time_init reached */
	odi_irq_debug_dump();
}
#endif

/*
 * The idle loop. CPU_R3000 has no wait instruction of its own, so without
 * a cpu_wait the generic loop spins at full speed whenever nothing runs.
 * The RLX5281 has the Lexra SLEEP instruction (0x42000038: COP0, function
 * 0x38; the assembler here does not know the mnemonic), which stops the
 * core until the next interrupt. arch_cpu_idle() is entered with
 * interrupts off and must return with them off. Interrupts are enabled
 * just before SLEEP so one can wake it; one that lands between the two is
 * taken at once and SLEEP then waits for the next, at most one 4 ms tick
 * later (the tick is periodic for this reason, kernel/618/config).
 */
static void __cpuidle rtl8686_sleep(void)
{
	raw_local_irq_enable();
	__asm__ __volatile__(
		"	.set	push		\n"
		"	.set	noreorder	\n"
		"	.word	0x42000038	\n"	/* sleep */
		"	nop			\n"
		"	nop			\n"
		"	.set	pop		\n");
	raw_local_irq_disable();
}

void __init plat_time_init(void)
{
	ODI_EARLY_CRUMB("KETM", 14);	/* plat_time_init() entered */
	/* After trap_init() and check_wait(), which sets nothing for R3000. */
	cpu_wait = rtl8686_sleep;
	rtl8686_clockevent_init();
#ifdef CONFIG_ODI_EARLY_CRUMBS
	late_time_init = rtl8686_irq_debug_dump;
#endif
	ODI_EARLY_CRUMB("KFTM", 15);	/* plat_time_init() about to return */
}

/*
 * UART0 is the only platform device; the NOR flash driver
 * (rtl8686-spiflash.c) maps its fixed window itself.
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

#ifdef CONFIG_ODI_SWITCH
	odi_board_init();
#endif

	return 0;
}
device_initcall(rtl8686_devices_init);
