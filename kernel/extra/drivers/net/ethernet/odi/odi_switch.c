// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch.c -- the kernel half of the switch core: maps the switch-core
 * window and defines odi_reg_read()/odi_reg_write(), the accessors every
 * other switch file calls (odi_switch_reg.h). The register offsets
 * (odi_switch_hw.h), table primitives (odi_switch_tbl.c), leaves
 * (odi_switch_dal.h) and command dispatch (odi_switch_cmd.c) are plain C
 * the host tests build; this file is what touches MMIO.
 *
 * The window: physical 0x1B000000, 0xF10000 bytes, covering the
 * switch-core registers (0x000000-0x1FFFFF), the GPON block (+0x700000)
 * and the PON queue counters (+0xF00000) (odi_switch_hw.h).
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kstrtox.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/ratelimit.h>
#include <linux/iopoll.h>
#include <linux/sysfs.h>

#include "odi_switch_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_dal.h"
#include "odi_replay_blob.h"
#include "odi_switch_cmd.h"
#include "odi_switch_api.h"
#include "odi_switch_l2.h"
#include "odi_nic.h"
#include "odi_wdt.h"

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
 * odi_switch_cmd.c (T-CONT, GEM flow, bridge and CF tables), and every
 * multi-register sequence: the replays (the platform settings, the
 * module-load replay, the sdkinit verbs, gpon_init.bin at the first
 * gponact) and odi_sw_cf_add(),
 * which holds the table engine for about 8,200 writes. Taken at the entry
 * points, never inside the primitives:
 *   - odi_omci_cmd(), netlink OP_CMD. The input callback of a kernel
 *     netlink socket runs in the sendmsg() of each sender, so two
 *     senders run it concurrently;
 *   - odi_omci_proc_write(), the switch_init write;
 *   - odi_sw_ioctl(), every /dev/odi_sw command except DDM_GET;
 *   - odi_init_proc_write(), every verb, the GPON ones included;
 *   - odi_switch_init(), for the state reset.
 *   - the netdevice notifier of odi_switch_pin.c, under RTNL.
 * It may sleep inside (request_firmware() in the replays), and it is
 * never taken from IRQ, softirq or timer context or under a spinlock.
 *
 * odi_switch_dsf_lock (spinlock, IRQ-safe, any context). Protects the
 * downstream-framer state the GPON interrupt path shares with process
 * context: the DSF GEM-port CAM and Alloc-ID CAM handshakes (CTL, WDATA,
 * the DONE poll), the DSF_GEM_FLOW_TYPE words, and the DS slot map in
 * odi_switch_ds_gem.c. Taken inside the primitives
 * (odi_switch_gpon_ds_port_write(), odi_switch_gpon_alloc_write(),
 * odi_sw_gpon_usflow_set(), odi_switch_gpon_encrypt_port()) and around
 * the CAM sequences in odi_gpon_hw.c; callers are process context (cmd
 * 25, under odi_switch_lock) and the GPON ISR, timers and verbs (under
 * odi_gpon_lock, IRQs off). It is held for one CAM handshake, a poll
 * bounded by ODI_SW_TABLE_POLL_US, or the Alloc-ID delete sweep of
 * a deactivation, never sleeps, prints nothing (callers log after
 * release), and nothing is acquired while it is held.
 *
 * odi_i2c_lock (mutex, process context only). Protects the port-1 I2C
 * master byte sequence (I2C_MASTER_SETUP, I2C_WRITE_DATA, I2C_BYTE_ADDR,
 * I2C_CMD, I2C_READ_DATA): a DDM read is setup, address, start, poll, read
 * per byte, and a second reader in between returns the wrong bytes. Taken
 * by odi_i2c_read_bytes() and odi_i2c_write_byte() (DDM_GET; cmd 10,
 * through odi_ddm_get() under odi_switch_lock; and the i2c adapter,
 * odi_i2c_adapter.c, under the i2c core bus lock) and by the sdkinit i2c
 * and i2cen verbs, the last two under odi_switch_lock.
 *
 * Lock order, outermost first:
 *   odi_switch_lock -> odi_i2c_lock
 *   i2c core bus lock (i2c-0) -> odi_i2c_lock
 *   odi_switch_lock -> odi_gpon_lock -> odi_switch_dsf_lock
 *   odi_gpon_lock -> odi_switch_dsf_lock  (the switch interrupt handler)
 *   RTNL -> odi_switch_lock  (the netdevice notifier of odi_switch_pin.c;
 *   nothing under odi_switch_lock takes RTNL)
 * No inversion is possible: the two mutexes are only taken in process
 * context holding no spinlock, so no atomic holder ever waits on them;
 * odi_switch_dsf_lock is a leaf; and odi_i2c_lock never nests with
 * odi_gpon_lock.
 * The other driver locks (odi_omci_reg_lock, odi_omci_last_lock, the
 * odi_nic locks, the odi_wdt flag mutex) are leaves: nothing above is
 * taken while one of them is held, and none is taken under the three
 * above.
 */
DEFINE_MUTEX(odi_switch_lock);
DEFINE_SPINLOCK(odi_switch_dsf_lock);
DEFINE_MUTEX(odi_i2c_lock);

/* Nothing here is exported: every ODI_* symbol is built in, and a
 * declaration in odi_switch_api.h or odi_switch_reg.h is enough.
 */

/* An offset past the mapped window would be a store to an address no bus
 * claims, which stalls the CPU with no timeout: refused and logged here
 * (a read returns 0), the same bound the host mock checks
 * (odi_switch_mmio_offset_in_bounds(), odi_switch_hw.h).
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

/* The switch-core writes the stock OMCI modules made when they loaded:
 * the platform settings, then the module-load replay. rcS runs this once
 * through /proc/odi_omci, after the odi_init main loop and before omcid
 * starts. It lives here, not in odi_switch_platform.c with the
 * steps themselves, because loading modload.bin sleeps and needs the kernel.
 * Returns 0, or the loader error (already logged).
 */
/* Set once the platform settings have run, under odi_switch_lock: from
 * then on a change of odi_switch.host_pause or odi_switch.pon_isolate
 * applies at once.
 */
static bool odi_switch_platform_done;

/* The L2 table survives a warm restart, the CAM included (the SoC reset is
 * not a power cycle), so the rows of whatever ran before this image (the
 * stock firmware's static groups, an earlier pin of a MAC that changed)
 * are still there. Emptied once here, after the platform step turned the
 * CAM on and before anything writes a row; the pins are written again by
 * odi_switch_pin_sync() at the end of the boot. A failure only logs: a
 * stale row is what the stick lived with before this step existed.
 */
static void odi_switch_boot_l2_clean(void)
{
	struct odi_sw_l2_clean_report rep;
	int rc = odi_switch_l2_clean(&rep);

	if (rc)
		pr_warn("odi_switch: L2 table clean stopped at an engine error (%d): %u row(s) found so far\n",
			rc, rep.found);
	else if (rep.left)
		pr_warn("odi_switch: L2 table clean: %u row(s) (%u static) found, %u still in use after %u pass(es)\n",
			rep.found, rep.found_static, rep.left, rep.passes);
	else
		pr_info("odi_switch: L2 table clean: %u row(s) (%u static) left by the previous run removed\n",
			rep.found, rep.found_static);
}

int odi_switch_boot_init(void)
{
	struct odi_replay_fw fw;
	int rc;

	lockdep_assert_held(&odi_switch_lock);
	odi_switch_init_platform();
	odi_switch_platform_done = true;
	odi_switch_boot_l2_clean();
	rc = odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &fw);
	if (rc)
		return rc;
	odi_switch_init_modload(&fw.blob);
	odi_replay_fw_release(&fw);
	/* After the replay, which writes SA_MOVED back to 0 (move). */
	odi_sw_sa_moved_apply();
	/* Again before omcid starts, should the write at network.sh time have
	 * failed: the stick's MACs static on the CPU port (odi_switch_pin.c).
	 */
	odi_switch_pin_sync();
	return 0;
}

/* odi_switch.host_pause: PAUSE frames toward the host on the host port,
 * off (the default) or left to the auto-negotiation. On the command line
 * it is read before the switch is mapped and takes effect with the
 * platform settings; written to /sys/module/odi_switch/parameters/host_pause
 * after them it is applied at once, the same two register writes.
 */
static int odi_switch_host_pause_param_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int rc;

	rc = kstrtobool(val, &on);
	if (rc)
		return rc;
	mutex_lock(&odi_switch_lock);
	odi_sw_host_pause = on;
	if (odi_switch_platform_done)
		odi_sw_host_pause_apply();
	mutex_unlock(&odi_switch_lock);
	return 0;
}

static const struct kernel_param_ops odi_switch_host_pause_param_ops = {
	.set = odi_switch_host_pause_param_set,
	.get = param_get_bool,
};
module_param_cb(host_pause, &odi_switch_host_pause_param_ops, &odi_sw_host_pause, 0644);
MODULE_PARM_DESC(host_pause, "send PAUSE frames to the host when the switch is congested (default: no, the switch drops)");

/* odi_switch.pon_isolate: the PON port isolated from the CPU side, both
 * ways (the default), or left open as the reset value and the stock image
 * leave it.
 * On the command line it takes effect with the platform settings; written
 * to /sys/module/odi_switch/parameters/pon_isolate after them it is
 * applied at once, one register write. The escape hatch should an OLT's
 * OMCI ever need the open mask; the NIC drops every non-OMCI frame from
 * the PON either way (odi_nic.c).
 */
static int odi_switch_pon_isolate_param_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int rc;

	rc = kstrtobool(val, &on);
	if (rc)
		return rc;
	mutex_lock(&odi_switch_lock);
	odi_sw_pon_isolate = on;
	if (odi_switch_platform_done)
		odi_sw_pon_isolate_apply();
	mutex_unlock(&odi_switch_lock);
	return 0;
}

static const struct kernel_param_ops odi_switch_pon_isolate_param_ops = {
	.set = odi_switch_pon_isolate_param_set,
	.get = param_get_bool,
};
module_param_cb(pon_isolate, &odi_switch_pon_isolate_param_ops, &odi_sw_pon_isolate, 0644);
MODULE_PARM_DESC(pon_isolate, "never forward a frame from the PON port to the CPU, nor one from the CPU to the PON by lookup (default: yes; no leaves the port isolation open, as stock)");

/* odi_switch.ds_sched: whether cmd 23 schedules the UNI's downstream queues
 * (docs/SWITCH.md#downstream-queues): strict or weighted, and the weight,
 * from the OLT's class 277 rows. On by default; N leaves the queues as the
 * boot left them (all strict, weight 1), the stock driver's behaviour. Read
 * at each cmd 23, so on the command line it covers the first provisioning,
 * and a write to /sys/module/odi_switch/parameters/ds_sched covers the
 * ones after it without undoing what is already programmed. */
module_param_named(ds_sched, odi_sw_ds_sched, bool, 0644);
MODULE_PARM_DESC(ds_sched, "schedule the UNI downstream queues (strict or weighted, the weight) from the OLT's priority queue rows (default: yes; no leaves them all strict, as stock)");

/* odi_switch.host_autoneg: the 1000BASE-X auto-negotiation of the host
 * port, on (the default) or off (forced 1000BASE-X full duplex). Written to
 * /sys/module/odi_switch/parameters/host_autoneg it applies at once, one
 * register write, whenever the switch window is mapped (the board init maps
 * it before userland starts); on the command line it is stored and the
 * platform settings apply it. Not persistent: HOST_AUTONEG and
 * host-autoneg.sh decide what each boot runs.
 */
static int odi_switch_host_autoneg_param_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int rc;

	rc = kstrtobool(val, &on);
	if (rc)
		return rc;
	mutex_lock(&odi_switch_lock);
	odi_sw_host_autoneg = on;
	if (odi_switch_base)
		odi_sw_host_autoneg_apply();
	mutex_unlock(&odi_switch_lock);
	return 0;
}

static const struct kernel_param_ops odi_switch_host_autoneg_param_ops = {
	.set = odi_switch_host_autoneg_param_set,
	.get = param_get_bool,
};
module_param_cb(host_autoneg, &odi_switch_host_autoneg_param_ops, &odi_sw_host_autoneg, 0644);
MODULE_PARM_DESC(host_autoneg, "1000BASE-X auto-negotiation on the host port (default: yes; no forces 1000BASE-X full duplex)");

/* odi_switch.host_link, read-only: 1 while the host port has a link
 * (PORT_ABILITY(0).LINK_UP, read when asked), 0 without one. What
 * host-autoneg.sh and the web UI read to decide whether a mode works.
 */
static int odi_switch_host_link_param_get(char *buf, const struct kernel_param *kp)
{
	if (!odi_switch_base)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", (unsigned int)odi_sw_host_link_up());
}

/* A built-in parameter is also parsed from the command line, where a
 * missing set would be called anyway; refuse it there.
 */
static int odi_switch_host_link_param_set(const char *val, const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops odi_switch_host_link_param_ops = {
	.set = odi_switch_host_link_param_set,
	.get = odi_switch_host_link_param_get,
};
module_param_cb(host_link, &odi_switch_host_link_param_ops, NULL, 0444);
MODULE_PARM_DESC(host_link, "1 while the host port has a link (read-only)");

int odi_poll_reg(uint32_t off, uint32_t mask, uint32_t want,
		 unsigned int delay_us, unsigned int timeout_us, uint32_t *last)
{
	uint32_t v;
	int rc;

	rc = read_poll_timeout_atomic(odi_reg_read, v, (v & mask) == want,
				      delay_us, timeout_us, false, off);
	if (last)
		*last = v;
	return rc;
}

/* odi_switch_mmio_ensure() -- maps the window once, whoever asks first.
 * The board replay needs it before odi_switch_init() runs: board.c
 * (arch/mips, linked before drivers/) makes its device_initcall earlier,
 * so odi_board_init() calls this itself.
 */
int odi_switch_mmio_ensure(void)
{
	if (odi_switch_base)
		return 0;

	/* A physical address, unlike the KSEG1 base of odi_nic.c. */
	odi_switch_base = ioremap(ODI_SWITCH_MMIO_BASE, ODI_SWITCH_MMIO_SIZE);
	if (!odi_switch_base) {
		pr_err(DRV_NAME ": failed to map MMIO window\n");
		return -ENODEV;
	}

	pr_info(DRV_NAME ": ready, MMIO base 0x%08lx, %lu bytes\n",
		(unsigned long)ODI_SWITCH_MMIO_BASE, ODI_SWITCH_MMIO_SIZE);
	return 0;
}

/* The CPU-port RX rule of odi_wdt (odi_wdt.h): what the switch offered
 * port 3, by the src/diag/src/mib.h counter index (odi_switch_mib.c), and
 * what odi_nic took. Plain MMIO reads of free-running counters, safe from
 * the watchdog timer; the sum wraps like each counter does.
 */
#define ODI_SW_CPU_PORT	3U

static int odi_switch_cpu_rx_sample(u32 *offered, u32 *taken)
{
	static const uint8_t counters[] = {
		7,	/* ifOutUcastPkts */
		8,	/* ifOutMulticastPkts */
		9,	/* ifOutBroadcastPkts */
		6,	/* ifOutDiscards */
		13,	/* dot3InPauseFrames: PAUSE from the CPU MAC (odi_nic) holding it back */
	};
	u32 sum = 0;
	unsigned int i;

	if (!odi_switch_base || !odi_nic_rx_taken(taken))
		return -1;
	for (i = 0; i < ARRAY_SIZE(counters); i++) {
		uint64_t v;

		if (odi_sw_mib_get(ODI_SW_CPU_PORT, counters[i], &v) != 0)
			return -1;
		sum += (u32)v;
	}
	*offered = sum;
	return 0;
}

static const struct odi_wdt_cpu_rx_ops odi_switch_cpu_rx_ops = {
	.sample = odi_switch_cpu_rx_sample,
	.report = odi_nic_report,
};

static int __init odi_switch_init(void)
{
	int rc = odi_switch_mmio_ensure();

	if (rc)
		return rc;

	WRITE_ONCE(wdt_cpu_rx_ops, &odi_switch_cpu_rx_ops);

	mutex_lock(&odi_switch_lock);
	odi_switch_cmd_reset_state();
	mutex_unlock(&odi_switch_lock);

	/* No platform init here: it needs the odi_init "switch" verb to
	 * have run, so rcS runs it through /proc/odi_omci.
	 */
	return 0;
}

module_init(odi_switch_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss open switch core and OMCI command table");
