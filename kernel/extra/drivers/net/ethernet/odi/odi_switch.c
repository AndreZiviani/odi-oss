// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch.c -- module glue for the open switch core: maps the
 * switch-core MMIO window and defines the two accessors odi_switch_tbl.c
 * and odi_switch_dal.c call (odi_switch_reg.h's own header comment has the
 * force-include mechanics).
 *
 * Everything above this file -- the register offsets (odi_switch_hw.h),
 * the table primitives (odi_switch_tbl.c), the DAL leaves
 * (odi_switch_dal.c), the 79-slot command dispatch (odi_switch_cmd.c) --
 * is plain C with no kernel dependency of its own; this is the one file
 * that actually touches MMIO and wires the whole stack into vmlinux
 * (CONFIG_ODI_SWITCH, built in alongside CONFIG_ODI_NIC/CONFIG_ODI_OMCI,
 * not a loadable module -- same as odi_nic.c/odi_omci.c).
 *
 * MMIO base and window size: ODI_SWITCH_MMIO_BASE/_SIZE (odi_switch_hw.h)
 * -- physical base 0x1B000000 (the switch-core register file, NOT
 * 0xB8000000, the s5 boot-kill bug; see that header's own comment
 * ("switch base corrected") for the derivation), size 0xF10000
 * covering every sub-window the mock host model (test/odi_switch_mock.h)
 * and this codebase have ever traced into: switch-core 0x000000-0x1FFFFF,
 * the GPON block at +0x700000, PONQ_COUNT_MASK at +0xF00000 (highest traced
 * offset +0x129*4, well inside the 0xF10000 bound).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/ratelimit.h>

#include "odi_switch_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_dal.h"
#include "odi_replay_blob.h"
#include "odi_switch_cmd.h"
#include "odi_switch_api.h"

#define DRV_NAME "odi_switch"

void __iomem *odi_switch_base;

/* ---- Locking -----------------------------------------------------------
 *
 * Three locks cover the switch core. lockdep is not in our kernel config,
 * so this block is the whole proof; test/odi_switch_mock.h models the
 * three on the host and aborts a test on a recursive acquire.
 *
 * odi_switch_lock (mutex, process context only). Protects the indirect
 * table engine (TABLE_WRITE_WORD, TABLE_CMD, TABLE_STATUS, one handshake
 * per odi_switch_table_write(), and the L2 lookup-table reads, walk and
 * multicast writes of odi_switch_l2.c), the command-layer shadow state in
 * odi_switch_cmd.c (T-CONT, GEM flow, bridge and CF tables), the trigger
 * bookkeeping and loaded parity table in odi_switch_dal.c, the sdkinit
 * mask, the /proc/odi_omci result buffers, and every multi-register
 * sequence: the replays (init_platform, init_parity, init_modload, the
 * sdkinit verbs, gpon_init.bin at the first gponact) and odi_sw_cf_add(),
 * which holds the table engine for about 8,200 writes. Taken at the entry
 * points, never inside the primitives:
 *   - odi_omci_cmd(), netlink OP_CMD. The input callback of a kernel
 *     netlink socket runs in the sendmsg() of each sender, so two
 *     senders run it concurrently;
 *   - odi_omci_proc_write() and the switch half of odi_omci_proc_show();
 *   - odi_sw_ioctl(), every /dev/odi_sw command except DDM_GET;
 *   - odi_rtk_init_proc_write(), every verb, the GPON ones included;
 *   - odi_switch_init(), for the state reset.
 * It may sleep inside (request_firmware() in the replays), and it is
 * never taken from IRQ, softirq or timer context or under a spinlock.
 *
 * odi_switch_dsf_lock (spinlock, IRQ-safe, any context). Protects the
 * downstream-framer state the GPON interrupt path shares with process
 * context: the DSF GEM-port CAM and Alloc-ID CAM handshakes (CTL, WDATA,
 * the DONE poll), the DSF_GEM_FLOW_TYPE words, and the DS slot map in
 * odi_switch_dal.c. Taken inside the primitives
 * (odi_switch_gpon_ds_port_write(), odi_switch_gpon_alloc_write(),
 * odi_sw_gpon_usflow_set(), odi_switch_ds_encrypt(),
 * odi_switch_gpon_encrypt_port()) and around the CAM sequences in
 * odi_gpon_hw.c; callers are process context (cmd 25, ds_encrypt, under
 * odi_switch_lock) and the GPON ISR, timers and verbs (under
 * odi_gpon_lock, IRQs off). It is held for one CAM handshake, a bounded
 * poll of ODI_SWITCH_TBL_MAX_SPINS reads, or the Alloc-ID delete sweep of
 * a deactivation, never sleeps, prints nothing (callers log after
 * release), and nothing is acquired while it is held.
 *
 * odi_i2c_lock (mutex, process context only). Protects the port-1 I2C
 * master byte sequence (I2C_MASTER_SETUP, I2C_BYTE_ADDR, I2C_CMD,
 * I2C_READ_DATA): a DDM read is setup, address, start, poll, read per
 * byte, and a second reader in between returns the wrong bytes. Taken by
 * odi_i2c_read_bytes() (DDM_GET, /proc/odi_omci ddm and i2c), by cmd 10
 * (odi_sw_ponmac_transceiver_get() writes the same I2C_MASTER_SETUP) and
 * by the sdkinit i2c and i2cen verbs, the last two under odi_switch_lock.
 *
 * Lock order, outermost first:
 *   odi_switch_lock -> odi_i2c_lock
 *   odi_switch_lock -> odi_gpon_lock -> odi_switch_dsf_lock
 *   odi_intr_lock -> odi_gpon_lock -> odi_switch_dsf_lock  (hard IRQ)
 * No inversion is possible: the two mutexes are only taken in process
 * context holding no spinlock, so no atomic holder ever waits on them;
 * odi_switch_dsf_lock is a leaf; odi_gpon_lock is never held while
 * odi_intr_lock is taken (odi_gpon_irq_attach() registers with odi_intr
 * before taking it); and odi_i2c_lock never nests with odi_gpon_lock.
 * The other driver locks (odi_omci_reg_lock, odi_omci_last_lock, the
 * odi_nic locks, the odi_wdt flag mutex) are leaves: nothing above is
 * taken while one of them is held, and none is taken under the three
 * above.
 */
DEFINE_MUTEX(odi_switch_lock);
DEFINE_SPINLOCK(odi_switch_dsf_lock);
DEFINE_MUTEX(odi_i2c_lock);

/* No EXPORT_SYMBOL* anywhere in this file: every CONFIG_ODI_* symbol is
 * bool (built into vmlinux, never a loadable module), and built-in
 * callers need no export -- a plain declaration in odi_switch_api.h (the
 * omci-facing entry points) or odi_switch_reg.h (odi_reg_read/write,
 * odi_switch_mmio_ensure) is enough for built-in-to-built-in linkage.
 *
 * odi_switch_platform_init_trigger() is declared below, next to
 * odi_switch_init(): moved out of module init in the s4 fix ("platform
 * init made safe"). Its own rc bookkeeping lives in odi_switch_dal.c now
 * (odi_switch_platform_init_rc_get()), alongside the parity and
 * ds_encrypt triggers it shares its shape with -- all three are pure
 * dal-leaf logic with no kernel dependency, so they belong there rather
 * than here.
 */

/* Bounds guard ("switch base corrected" item 2): the whole s5 incident was
 * one offset landing far outside the mapped window with nothing to catch
 * it before the bare store executed. odi_switch_mmio_offset_in_bounds()
 * (odi_switch_hw.h) is the same check the host suite now runs too
 * (odi_switch_mmio_bounds_test.c) -- refusing here is the last line of
 * defence on target: a read returns 0 and logs (rate-limited) instead of
 * dereferencing outside odi_switch_base's ioremap, and a write is skipped
 * entirely rather than ever reaching __raw_writel with a bad offset.
 */
static DEFINE_RATELIMIT_STATE(odi_switch_mmio_oob_rl, 5 * HZ, 3);

uint32_t odi_reg_read(uint32_t off)
{
	uint32_t val;

	if (!odi_switch_mmio_offset_in_bounds(off)) {
		if (__ratelimit(&odi_switch_mmio_oob_rl))
			pr_err(DRV_NAME ": refusing out-of-bounds read, offset 0x%08x >= 0x%08lx\n",
			       off, ODI_SWITCH_MMIO_SIZE);
		return 0;
	}

	val = __raw_readl(odi_switch_base + off);
	return val;
}

void odi_reg_write(uint32_t off, uint32_t val)
{
	if (!odi_switch_mmio_offset_in_bounds(off)) {
		if (__ratelimit(&odi_switch_mmio_oob_rl))
			pr_err(DRV_NAME ": refusing out-of-bounds write, offset 0x%08x >= 0x%08lx (value 0x%08x dropped)\n",
			       off, ODI_SWITCH_MMIO_SIZE, val);
		return;
	}

	__raw_writel(val, odi_switch_base + off);
}

/* odi_switch_platform_init_mask: the lazy-trigger mask (see
 * odi_switch_platform_init_trigger() below) -- 0 by default, so nothing
 * runs unless a trial explicitly asks, either by setting this at boot
 * (module_param, /sys/module/odi_switch/parameters/platform_init_mask --
 * works for built-in code the same way it does for a loadable module) or
 * by writing to /proc/odi_omci (odi_omci.c), which also fires the
 * trigger immediately rather than waiting for the first OP_CMD.
 */
unsigned int odi_switch_platform_init_mask;
module_param_named(platform_init_mask, odi_switch_platform_init_mask, uint, 0644);
MODULE_PARM_DESC(platform_init_mask,
		  "odi_switch_init_platform() item bitmask for the first OP_CMD lazy trigger (default 0, nothing runs)");

/* odi_switch_platform_init_trigger() (odi_switch_dal.c) is the only
 * caller of odi_switch_init_platform() left in this codebase (s4 hung the
 * kernel calling it from module init ("platform init made safe"): the
 * RAM log ended at "odi_omci: ready" with not one odi_switch line, so
 * module init entered odi_switch_init_platform() and never returned,
 * before even that function's own first per-item log line). Two callers,
 * in order of preference (odi_omci.c):
 *   1. a /proc/odi_omci write ("init_platform" for every item, or
 *      "init_platform 0x3f" for an explicit mask) -- calls this directly
 *      with whatever mask the write parsed to, every time, so a trial can
 *      retry with a narrower mask after a bad boot without changing code.
 *   2. failing that, lazily on the first ODI_OMCI_OP_CMD -- calls this
 *      with odi_switch_platform_init_mask (the module parameter above,
 *      0 by default) the first time an OP_CMD arrives, so a boot that
 *      never gets a manual /proc write still eventually brings the
 *      switch core up once real traffic starts, instead of never at all.
 *
 * The parity triggers (odi_switch_init_parity()/_all()'s own -1/0
 * bookkeeping) and the ds_encrypt trigger live in odi_switch_dal.c too,
 * for the same reason: pure dal-leaf logic with no kernel dependency
 * belongs in the file that is already host-tested as plain C, not here.
 *
 * odi_switch_init_modload()'s own trigger stays in this file, right
 * below: unlike the other three, it drives odi_replay_fw_load() (file
 * I/O, sleeps), which needs __KERNEL__ and cannot move into
 * odi_switch_dal.c without giving that file a kernel dependency.
 */
int odi_switch_modload_init_rc = -1;

int odi_switch_modload_init_rc_get(void)
{
	return odi_switch_modload_init_rc;
}

void odi_switch_modload_init_trigger(uint32_t mask)
{
	struct odi_replay_fw fw;
	int rc;

	lockdep_assert_held(&odi_switch_lock);
	if (mask == 0)
		return;
	rc = odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &fw);
	if (rc) {
		odi_switch_modload_init_rc = rc;
		return;
	}
	odi_switch_init_modload(&fw.blob, mask);
	odi_replay_fw_release(&fw);
	odi_switch_modload_init_rc = 0;
}

/* odi_switch_mmio_ensure() -- idempotent MMIO map, split out of
 * odi_switch_init() below so a caller outside this driver set's own
 * module_init() ordering can force the map early instead of assuming it.
 * CONFIG_ODI_BOARD needs exactly this: the board file (arch/mips/rtl8686/board.c)
 * LED-init entry point is ALSO a plain module_init() (device_initcall,
 * built-in), and a whole-tree System.map check shows its initcall entry ordered
 * BEFORE odi_switch_init()'s own -- core-y (arch/mips) links before
 * drivers-y in the standard kbuild link order, so "Makefile list order
 * decides initcall order" (the reasoning odi_intr.c's own module_init()
 * comment relies on) only holds for two module_init()s in the SAME
 * built-in.o; it does not reach across that boundary. odi_board_init()
 * (odi_board.c) calls this function itself before touching odi_reg_write()
 * so its own replay never depends on which of the two device_initcalls the
 * linker happened to place first -- whichever runs first maps the window,
 * the other one below finds odi_switch_base already set and skips the
 * remap.
 */
int odi_switch_mmio_ensure(void)
{
	if (odi_switch_base)
		return 0;

	/* ODI_SWITCH_MMIO_BASE (odi_switch_hw.h) is already a physical
	 * address (0x1B000000, the switch-core register file -- "switch base
	 * corrected") -- unlike odi_nic.c's own MMIO base, this one is
	 * not a KSEG1 virtual constant, so it is passed to ioremap directly,
	 * with no `& 0x1FFFFFFF` masking. The previous version of this line
	 * both used the wrong base (0xB8000000, the SoC window, not the
	 * switch-core window) and masked it as if it were a KSEG1 alias --
	 * two independent mistakes that happened to compile into one
	 * plausible-looking wrong physical address; see odi_switch_hw.h's
	 * ODI_SWITCH_MMIO_BASE comment for the full derivation.
	 */
	odi_switch_base = ioremap(ODI_SWITCH_MMIO_BASE, ODI_SWITCH_MMIO_SIZE);
	if (!odi_switch_base) {
		pr_err(DRV_NAME ": failed to map MMIO window\n");
		return -ENODEV;
	}

	pr_info(DRV_NAME ": ready, MMIO base 0x%08lx, %lu bytes\n",
		(unsigned long)ODI_SWITCH_MMIO_BASE, ODI_SWITCH_MMIO_SIZE);
	return 0;
}

static int __init odi_switch_init(void)
{
	int rc = odi_switch_mmio_ensure();

	if (rc)
		return rc;

	mutex_lock(&odi_switch_lock);
	odi_switch_cmd_reset_state();
	mutex_unlock(&odi_switch_lock);

	/* odi_switch_init_platform() is deliberately NOT called here any
	 * more -- see odi_switch_platform_init_trigger() (odi_switch_dal.c)
	 * ("platform init made safe"). Module init now only maps MMIO, resets
	 * command state, and reports ready; the platform init itself waits
	 * for a trigger a trial controls, so a bad item costs that trial,
	 * not the boot.
	 */
	return 0;
}

static void __exit odi_switch_exit(void)
{
	if (odi_switch_base) {
		iounmap(odi_switch_base);
		odi_switch_base = NULL;
	}
}

module_init(odi_switch_init);
module_exit(odi_switch_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss open switch core and OMCI command table");
