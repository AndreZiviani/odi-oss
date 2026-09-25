// SPDX-License-Identifier: GPL-2.0
/*
 * SPI NOR flash of the ODI DFP-34X-2C2 (Realtek RTL9602C/RTL8686).
 *
 * The flash hangs off a SoC SPI controller with two faces:
 *
 *  - a command engine, driven one byte at a time through a control
 *    register (CMDCTL) and a data register (CMDDATA): chip select,
 *    opcode, 3-byte address, data, chip select off;
 *  - a memory-mapped read window at physical 0x14000000, live once the
 *    map-configuration register (MAPCFG) names the read opcode and the
 *    flash size. The stock boot line "C22017/MMIO16-1" is this window.
 *
 * The chip is a JEDEC C22017 (Macronix, 8 MB), read from a live boot log.
 * The driver sizes itself from the JEDEC capacity byte. It uses the
 * standard opcodes (0x03 read, 0x02 page program, 0x20 4 KB erase, 0x06
 * write enable, 0x05 status, 0x9f id), single-wire I/O and 3-byte
 * addresses, which cover the 16 MB a single chip of this kind can have.
 *
 * This is a plain mtd_info driver, not a spi-mem controller: the board
 * has one flash, no SPI bus and no devicetree, and the id is logged, not
 * matched against a table. Reads are memcpy_fromio() from the window;
 * erase and program go through the command engine. The erase size is a
 * flat 4096, the size /proc/mtd shows on this board.
 *
 * Register values seen on a stick running the stock firmware (read one
 * register at a time, 2026-09-23): MAPCFG 0x03c00000 (opcode 0x03, size
 * code 6 for 8 MB, 3-byte, single-wire -- what sf_map_window() below
 * programs), CMDCTL 0xc8000c10 at idle.
 *
 * Fields are mask/shift pairs, not C bitfields: the port is big-endian
 * and bitfield order is an ABI detail.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/io.h>

#include <asm/mach-rtl8686/rtl8686regs.h>

/* MAPCFG: what the memory-mapped window sends to the flash. */
#define MAPCFG_OPCODE		GENMASK(31, 24)
#define MAPCFG_SIZE		GENMASK(23, 21)	/* 128 KB << code */
#define MAPCFG_OPC_WIDTH	GENMASK(19, 18)
#define MAPCFG_ADR_WIDTH	GENMASK(17, 16)
#define MAPCFG_WAIT		GENMASK(15, 13)
#define MAPCFG_DAT_WIDTH	GENMASK(12, 11)
#define MAPCFG_ADDR4		BIT(9)		/* 4-byte addresses; kept clear */
#define MAPCFG_SIZE_MAX		7u

/* CMDCTL: the command engine. The chip-select bits are active low. */
#define CMDCTL_CS0_N		BIT(31)
#define CMDCTL_CS1_N		BIT(30)	/* no second chip on this board */
#define CMDCTL_NBYTES		GENMASK(29, 28)	/* bytes per CMDDATA access, minus one */
#define CMDCTL_READY		BIT(27)
#define CMDCTL_WIDTH		GENMASK(26, 25)
#define CMDCTL_CS0_OFF		BIT(11)	/* status: chip select 0 not driven */
#define CMDCTL_CS1_OFF		BIT(10)
#define CMDCTL_IDLE		BIT(4)

#define CMDCTL_CS_N		(CMDCTL_CS0_N | CMDCTL_CS1_N)
#define CMDCTL_CS_OFF		(CMDCTL_CS0_OFF | CMDCTL_CS1_OFF)
#define CMDCTL_DONE		(CMDCTL_READY | CMDCTL_IDLE)

#define SF_ONE_BYTE		0u
#define SF_THREE_BYTES		2u
#define SF_SINGLE_WIRE		0u

#define SF_OP_WREN		0x06
#define SF_OP_STATUS		0x05
#define SF_OP_ID		0x9f
#define SF_OP_READ		0x03
#define SF_OP_PROGRAM		0x02
#define SF_OP_ERASE_4K		0x20
#define SF_STATUS_BUSY		BIT(0)

#define SF_PAGE			256u
#define SF_SECTOR		4096u

#define SF_PROGRAM_MS		100
#define SF_ERASE_MS		2000
/* Register reads to spend on one wait for the command engine. Counted in
 * reads rather than time: each is an uncached MMIO load, the flash-side
 * timeouts above are the real limit, and a wedged engine then returns
 * -EIO instead of hanging the caller.
 */
#define SF_SPIN			200000

struct rtl8686_sf {
	void __iomem *regs;	/* the controller, RTL8686_SF_REGS_SIZE bytes */
	void __iomem *window;	/* the read window, RTL8686_NOR_SIZE bytes */
	struct mutex lock;
	struct mtd_info mtd;
};

static inline u32 sf_ctl(struct rtl8686_sf *sf)
{
	return readl(sf->regs + RTL8686_SF_CMDCTL);
}

static inline void sf_set_ctl(struct rtl8686_sf *sf, u32 v)
{
	writel(v, sf->regs + RTL8686_SF_CMDCTL);
}

/* Spin on CMDCTL until (value & mask) == want, or, with until_differs,
 * until it no longer is.
 */
static int sf_spin(struct rtl8686_sf *sf, u32 mask, u32 want, bool until_differs)
{
	int left = SF_SPIN;
	u32 v = sf_ctl(sf);

	while (((v & mask) == want) == until_differs) {
		if (!--left)
			return -EIO;
		cpu_relax();
		v = sf_ctl(sf);
	}
	return 0;
}

static int sf_idle(struct rtl8686_sf *sf)
{
	return sf_spin(sf, CMDCTL_DONE, CMDCTL_DONE, false);
}

/* Drop chip select. A read through the window can leave one driven, so
 * both are pulsed on and then off, and the engine must report both off
 * and itself idle before the next command.
 */
static int sf_release(struct rtl8686_sf *sf)
{
	int ret = sf_idle(sf);

	if (ret)
		return ret;
	sf_set_ctl(sf, sf_ctl(sf) & ~CMDCTL_CS_N);
	udelay(1);
	sf_set_ctl(sf, sf_ctl(sf) | CMDCTL_CS_N);
	udelay(1);
	return sf_spin(sf, CMDCTL_CS_N | CMDCTL_DONE, CMDCTL_CS_N | CMDCTL_DONE, false);
}

/* Send one byte group through CMDDATA, left-aligned, and wait for it. */
static int sf_put(struct rtl8686_sf *sf, u32 word)
{
	writel(word, sf->regs + RTL8686_SF_CMDDATA);
	return sf_idle(sf);
}

enum sf_data { SF_NO_DATA, SF_DATA_IN, SF_DATA_OUT };

/* One command on chip 0: opcode, then an address if addr >= 0, then len
 * data bytes in or out, one CMDDATA access per byte.
 */
static int sf_command(struct rtl8686_sf *sf, u8 op, s64 addr,
		      enum sf_data dir, u8 *buf, u32 len)
{
	u32 ctl;
	u32 n;
	int ret;

	ret = sf_idle(sf);
	if (!ret)
		ret = sf_release(sf);
	if (ret)
		return ret;

	ctl = sf_ctl(sf) & ~(CMDCTL_CS0_N | CMDCTL_NBYTES | CMDCTL_WIDTH);
	ctl |= FIELD_PREP(CMDCTL_NBYTES, SF_ONE_BYTE) |
	       FIELD_PREP(CMDCTL_WIDTH, SF_SINGLE_WIRE);
	sf_set_ctl(sf, ctl);

	/* The select takes a moment to reach the pin: wait until the status
	 * no longer shows both chips released.
	 */
	ret = sf_spin(sf, CMDCTL_CS_OFF, CMDCTL_CS_OFF, true);
	if (!ret)
		ret = sf_put(sf, (u32)op << 24);
	if (ret)
		return ret;

	if (addr >= 0) {
		ctl = (ctl & ~CMDCTL_NBYTES) | FIELD_PREP(CMDCTL_NBYTES, SF_THREE_BYTES);
		sf_set_ctl(sf, ctl);
		ret = sf_put(sf, ((u32)addr & 0xffffffu) << 8);
		if (ret)
			return ret;
	}

	if (dir != SF_NO_DATA) {
		ctl &= ~CMDCTL_NBYTES;
		sf_set_ctl(sf, ctl);
		for (n = 0; n < len; n++) {
			if (dir == SF_DATA_IN) {
				buf[n] = readl(sf->regs + RTL8686_SF_CMDDATA) >> 24;
				ret = sf_idle(sf);
			} else {
				ret = sf_put(sf, (u32)buf[n] << 24);
			}
			if (ret)
				return ret;
		}
	}

	ret = sf_idle(sf);
	return ret ? ret : sf_release(sf);
}

static int sf_busy_wait(struct rtl8686_sf *sf, unsigned int ms)
{
	unsigned long until = jiffies + msecs_to_jiffies(ms);
	u8 status;
	int ret;

	for (;;) {
		ret = sf_command(sf, SF_OP_STATUS, -1, SF_DATA_IN, &status, 1);
		if (ret)
			return ret;
		if (!(status & SF_STATUS_BUSY))
			return 0;
		if (time_after(jiffies, until))
			return -ETIMEDOUT;
		cond_resched();
	}
}

/* A command that changes the flash: write enable, the command, then wait
 * for the flash to finish.
 */
static int sf_modify(struct rtl8686_sf *sf, u8 op, u32 addr,
		     u8 *buf, u32 len, unsigned int ms)
{
	int ret = sf_command(sf, SF_OP_WREN, -1, SF_NO_DATA, NULL, 0);

	if (!ret)
		ret = sf_command(sf, op, addr, buf ? SF_DATA_OUT : SF_NO_DATA, buf, len);
	return ret ? ret : sf_busy_wait(sf, ms);
}

/* The MAPCFG size code: the window covers 128 KB << code, up to code 7. */
static u32 sf_size_code(u32 bytes)
{
	u32 code = 0;

	while (code < MAPCFG_SIZE_MAX && (SZ_128K << code) < bytes)
		code++;
	return code;
}

/* Open the read window: opcode 0x03, single-wire, no wait cycles,
 * 3-byte addresses, sized to the chip. Reads are plain loads after this.
 */
static int sf_map_window(struct rtl8686_sf *sf, u32 bytes)
{
	void __iomem *cfg = sf->regs + RTL8686_SF_MAPCFG;
	u32 v;
	int ret;

	ret = sf_idle(sf);
	if (ret)
		return ret;

	v = readl(cfg) & ~(MAPCFG_OPCODE | MAPCFG_SIZE | MAPCFG_OPC_WIDTH |
			   MAPCFG_ADR_WIDTH | MAPCFG_WAIT | MAPCFG_DAT_WIDTH |
			   MAPCFG_ADDR4);
	v |= FIELD_PREP(MAPCFG_OPCODE, SF_OP_READ) |
	     FIELD_PREP(MAPCFG_SIZE, sf_size_code(bytes));
	writel(v, cfg);

	ret = readl_poll_timeout(cfg, v, !(v & MAPCFG_ADDR4), 1, 10000);
	return ret ? ret : sf_idle(sf);
}

static int rtl8686_sf_read(struct mtd_info *mtd, loff_t from, size_t len,
			   size_t *retlen, u_char *buf)
{
	struct rtl8686_sf *sf = mtd->priv;

	if (from < 0 || from + len > mtd->size)
		return -EINVAL;

#ifdef CONFIG_ODI_EARLY_CRUMBS
	static unsigned int odi_nrd;
	bool odi_log = odi_nrd++ < 12;

	if (odi_log)
		pr_info("odi_dbg: spiflash read 0x%llx len %zu\n", (unsigned long long)from, len);
#endif
	mutex_lock(&sf->lock);
	memcpy_fromio(buf, sf->window + from, len);
	mutex_unlock(&sf->lock);
#ifdef CONFIG_ODI_EARLY_CRUMBS
	if (odi_log)
		pr_info("odi_dbg: spiflash read done, first word %08x\n", *(u32 *)buf);
#endif

	*retlen = len;
	return 0;
}

static int rtl8686_sf_write(struct mtd_info *mtd, loff_t to, size_t len,
			    size_t *retlen, const u_char *buf)
{
	struct rtl8686_sf *sf = mtd->priv;
	size_t done = 0;
	int ret = 0;

	*retlen = 0;
	if (to < 0 || to + len > mtd->size)
		return -EINVAL;

	mutex_lock(&sf->lock);
	while (done < len) {
		u32 at = to + done;
		size_t n = min_t(size_t, len - done, SF_PAGE - at % SF_PAGE);

		ret = sf_modify(sf, SF_OP_PROGRAM, at, (u8 *)buf + done, n,
				SF_PROGRAM_MS);
		if (ret)
			break;
		done += n;
	}
	mutex_unlock(&sf->lock);

	*retlen = done;
	return ret;
}

static int rtl8686_sf_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct rtl8686_sf *sf = mtd->priv;
	u32 at = instr->addr;
	int ret = 0;

	if (instr->addr + instr->len > mtd->size)
		return -EINVAL;
	if ((instr->addr | instr->len) % SF_SECTOR)
		return -EINVAL;

	mutex_lock(&sf->lock);
	for (; at < instr->addr + instr->len; at += SF_SECTOR) {
		ret = sf_modify(sf, SF_OP_ERASE_4K, at, NULL, 0, SF_ERASE_MS);
		if (ret) {
			instr->fail_addr = at;
			break;
		}
	}
	mutex_unlock(&sf->lock);

	return ret;
}

/* cmdlinepart only, named so that a parser added to the config later
 * cannot take precedence over the mtdparts= this board boots with.
 */
static const char * const rtl8686_sf_parsers[] = { "cmdlinepart", NULL };

static int __init rtl8686_sf_init(void)
{
	struct rtl8686_sf *sf;
	u8 id[3];
	u32 jedec, bytes;
	int ret;

	sf = kzalloc(sizeof(*sf), GFP_KERNEL);
	if (!sf)
		return -ENOMEM;
	mutex_init(&sf->lock);

	ret = -ENOMEM;
	sf->regs = ioremap(RTL8686_SF_BASE, RTL8686_SF_REGS_SIZE);
	if (!sf->regs)
		goto out_free;
	sf->window = ioremap(RTL8686_NOR_BASE, RTL8686_NOR_SIZE);
	if (!sf->window)
		goto out_regs;

	ret = sf_command(sf, SF_OP_ID, -1, SF_DATA_IN, id, sizeof(id));
	if (ret) {
		pr_err("rtl8686-spiflash: JEDEC ID read failed (%d)\n", ret);
		goto out_window;
	}
	jedec = (id[0] << 16) | (id[1] << 8) | id[2];
	pr_info("rtl8686-spiflash: JEDEC ID %06x\n", jedec);

	bytes = 1u << id[2];
	if (!bytes || bytes > RTL8686_NOR_SIZE) {
		pr_warn("rtl8686-spiflash: capacity byte 0x%02x out of range, capping at %u B\n",
			id[2], RTL8686_NOR_SIZE);
		bytes = RTL8686_NOR_SIZE;
	}

	ret = sf_map_window(sf, bytes);
	if (ret) {
		pr_err("rtl8686-spiflash: read window setup failed (%d)\n", ret);
		goto out_window;
	}

	/* The device name the mtdparts= on the command line refers to. */
	sf->mtd.name = "rtk_spi_nor_mtd";
	sf->mtd.type = MTD_NORFLASH;
	sf->mtd.flags = MTD_CAP_NORFLASH;
	sf->mtd.size = bytes;
	sf->mtd.erasesize = SF_SECTOR;
	sf->mtd.writesize = 1;
	sf->mtd.owner = THIS_MODULE;
	sf->mtd._erase = rtl8686_sf_erase;
	sf->mtd._read = rtl8686_sf_read;
	sf->mtd._write = rtl8686_sf_write;
	sf->mtd.priv = sf;

	ret = mtd_device_parse_register(&sf->mtd, rtl8686_sf_parsers, NULL, NULL, 0);
	if (ret) {
		pr_err("rtl8686-spiflash: mtd_device_parse_register failed (%d)\n", ret);
		goto out_window;
	}
	return 0;

out_window:
	iounmap(sf->window);
out_regs:
	iounmap(sf->regs);
out_free:
	kfree(sf);
	return ret;
}
device_initcall(rtl8686_sf_init);
