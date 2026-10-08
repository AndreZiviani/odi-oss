// SPDX-License-Identifier: GPL-2.0
/*
 * odi_soc.c -- the SoC-window accessors and their allowlist (odi_soc.h).
 * The host unity builds get the window from test/odi_soc_mock.h.
 */
#include "odi_soc.h"

#ifdef __KERNEL__
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/printk.h>
#define ODI_SOC_LOG_ERR(fmt, ...) pr_err("odi_soc: " fmt, ##__VA_ARGS__)

static void __iomem *odi_soc_base;

int odi_soc_ensure(void)
{
	if (odi_soc_base)
		return 0;
	odi_soc_base = ioremap(ODI_SOC_PHYS, ODI_SOC_SIZE);
	if (!odi_soc_base) {
		ODI_SOC_LOG_ERR("failed to map the SoC window\n");
		return -ENODEV;
	}
	return 0;
}

static int odi_soc_mapped(void)
{
	return odi_soc_base != NULL;
}

/* Native byte order, no swap: the registers are CPU-endian. */
static uint32_t odi_soc_raw_read(uint32_t off)
{
	return __raw_readl(odi_soc_base + off);
}

static void odi_soc_raw_write(uint32_t off, uint32_t val)
{
	__raw_writel(val, odi_soc_base + off);
}
#else
#include <errno.h>
#include <stdio.h>
#define ODI_SOC_LOG_ERR(fmt, ...) fprintf(stderr, "odi_soc: " fmt, ##__VA_ARGS__)

int odi_soc_ensure(void)
{
	return 0;
}

static int odi_soc_mapped(void)
{
	return 1;
}

#define odi_soc_raw_read(off)		odi_soc_mock_read(off)
#define odi_soc_raw_write(off, val)	odi_soc_mock_write(off, val)
#endif

/* Every offset any driver reads or writes, and nothing else. */
static const uint32_t odi_soc_allowlist[] = {
	SOC_CLK_RST_EN,
	SOC_IP_EN,
	SOC_WDT_KICK,
	SOC_WDT_STATUS,
	SOC_WDT_CTRL,
	SOC_GPIO_DIR,
	SOC_GPIO_DATA,
	SOC_GPIO_B1_DIR,
	SOC_GPIO_B1_DATA,
};

static int odi_soc_allowed(uint32_t off)
{
	unsigned int i;

	for (i = 0; i < sizeof(odi_soc_allowlist) / sizeof(odi_soc_allowlist[0]); i++)
		if (odi_soc_allowlist[i] == off)
			return 1;
	return 0;
}

uint32_t odi_soc_read(uint32_t off)
{
	if (!odi_soc_mapped() || !odi_soc_allowed(off)) {
		ODI_SOC_LOG_ERR("refusing read of offset 0x%04x\n", (unsigned int)off);
		return 0;
	}
	return odi_soc_raw_read(off);
}

int odi_soc_write(uint32_t off, uint32_t val)
{
	if (!odi_soc_mapped())
		return -ENODEV;
	if (!odi_soc_allowed(off)) {
		ODI_SOC_LOG_ERR("refusing write of offset 0x%04x = 0x%08x\n",
				(unsigned int)off, (unsigned int)val);
		return -EPERM;
	}
	odi_soc_raw_write(off, val);
	return 0;
}
