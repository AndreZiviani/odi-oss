// SPDX-License-Identifier: GPL-2.0
/*
 * odi_wdt.c -- odi_wdt.h's own implementation. See that header for the
 * register ground truth. Three tiers, the same shape odi_intr.c already
 * uses:
 *
 *  - Pure functions, no register access at all: odi_wdt_ctrl_encode(),
 *    odi_wdt_kick_value(), odi_wdt_force_reset_value(), and the
 *    deadline-state functions. Compiled and tested identically on both
 *    builds, no mock needed.
 *
 *  - odi_wdt_reg_read()/odi_wdt_reg_write(): the allowlisted KSEG1
 *    accessor, real on __KERNEL__ (a raw pointer against the CPU
 *    system-controller window -- odi_wdt.h has why no ioremap is used or
 *    possible), a small self-contained three-register model on the host
 *    build. odi_wdt_arm()/odi_wdt_kick()/odi_wdt_disable()/
 *    odi_wdt_force_reset() call these; test/odi_wdt_test.c exercises the
 *    real kick/arm sequence against the host model, the same posture
 *    test/odi_intr_test.c uses against test/odi_switch_mock.h.
 *
 *  - __KERNEL__ only: the kicker kthread, the deadline timer, /proc/
 *    luna_watchdog/{watchdog_flag,userland_ok} and module_init/exit.
 *    /proc/luna_watchdog is the same directory name
 *    rootfs/skeleton/etc/init.d/rcS already writes to
 *    -- the /proc path the stock firmware uses, kept so rcS needs no
 *    change and trial-boot behaviour does not move at all.
 *
 * Only watchdog_flag and userland_ok are provided: they are the only
 * entries under that directory anything in this image reads or writes
 * (rcS). The stock firmware has further debug-only entries there; none
 * is reproduced.
 */
#include "odi_wdt.h"

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/errno.h>
#else
#include <string.h>
#include <stdio.h>
#include <errno.h>
#endif

#define DRV_NAME "odi_wdt"

/* ---- Pure functions ----------------------------------------------------- */

uint32_t odi_wdt_ctrl_encode(unsigned int prescale, unsigned int timeout1,
			      unsigned int timeout2, unsigned int reset_mode,
			      int enable)
{
	uint32_t reg = 0;

	reg |= (prescale & ODI_WDT_CTRL_PRESCALE_MASK) << ODI_WDT_CTRL_PRESCALE_SHIFT;
	reg |= (timeout1 & ODI_WDT_CTRL_TIMEOUT1_MASK) << ODI_WDT_CTRL_TIMEOUT1_SHIFT;
	reg |= (timeout2 & ODI_WDT_CTRL_TIMEOUT2_MASK) << ODI_WDT_CTRL_TIMEOUT2_SHIFT;
	reg |= reset_mode & ODI_WDT_CTRL_RESET_MODE_MASK;
	if (enable)
		reg |= (1U << ODI_WDT_CTRL_ENABLE_BIT);
	return reg;
}

uint32_t odi_wdt_kick_value(uint32_t cur_kick)
{
	return cur_kick | (1U << ODI_WDT_KICK_BIT);
}

uint32_t odi_wdt_force_reset_value(void)
{
	return 1U << ODI_WDT_CTRL_ENABLE_BIT;
}

void odi_wdt_deadline_state_init(struct odi_wdt_deadline_state *st)
{
	memset(st, 0, sizeof(*st));
}

void odi_wdt_note_kick(struct odi_wdt_deadline_state *st, unsigned int uptime_s)
{
	st->last_kick_s = uptime_s;
}

unsigned int odi_wdt_deadline_tick(struct odi_wdt_deadline_state *st, unsigned int uptime_s)
{
	unsigned int actions = ODI_WDT_ACTION_NONE;

	if (uptime_s >= st->last_beat_s + ODI_WDT_HEARTBEAT_INTERVAL_S) {
		st->last_beat_s = uptime_s;
		actions |= ODI_WDT_ACTION_HEARTBEAT;
	}

	if (st->watchdog_enabled && st->last_kick_s != 0 &&
	    uptime_s > st->last_kick_s + ODI_WDT_STALL_THRESHOLD_S &&
	    st->stall_reports < ODI_WDT_STALL_MAX_REPORTS) {
		st->stall_reports++;
		actions |= ODI_WDT_ACTION_STALL_REPORT;
	}

	if (st->watchdog_enabled && !st->userland_ok && !st->reset_signaled &&
	    uptime_s > ODI_WDT_USERLAND_DEADLINE_S) {
		st->reset_signaled = 1;
		actions |= ODI_WDT_ACTION_FORCE_RESET;
	}

	return actions;
}

/* ---- Allowlisted register access ---------------------------------------- */

static int odi_wdt_reg_allowed(uint32_t addr)
{
	return addr == ODI_WDT_KICK_REG ||
	       addr == ODI_WDT_STATUS_REG ||
	       addr == ODI_WDT_CTRL_REG;
}

#ifdef __KERNEL__

/* 0xb8xxxxxx is already inside the MIPS KSEG1 window (0xA0000000-
 * 0xBFFFFFFF, unmapped, uncached, identity-mapped to the low 512 MB of
 * physical address space) -- no ioremap is needed or possible for it, the
 * same reasoning odi_switch_sdkinit.c's own odi_switch_sdkinit_soc_write()
 * comment gives for its own (different-window) SoC writes. Refusing
 * anything not on the three-address allowlist above is this driver's own
 * belt-and-braces, same posture as that file and as odi_switch.c's own
 * bounds check on its ioremapped window.
 */
static uint32_t odi_wdt_reg_read(uint32_t addr)
{
	if (!odi_wdt_reg_allowed(addr)) {
		pr_err(DRV_NAME ": refusing read of 0x%08x, not on the allowlist\n", addr);
		return 0;
	}
	return *(volatile uint32_t *)(unsigned long)addr;
}

static void odi_wdt_reg_write(uint32_t addr, uint32_t val)
{
	if (!odi_wdt_reg_allowed(addr)) {
		pr_err(DRV_NAME ": refusing write of 0x%08x=0x%08x, not on the allowlist\n", addr, val);
		return;
	}
	*(volatile uint32_t *)(unsigned long)addr = val;
}

#else /* host build -- three-register model, no real memory behind it */

struct odi_wdt_mock_state {
	uint32_t kick;
	uint32_t status;
	uint32_t ctrl;
	unsigned int refused;		/* accesses odi_wdt_reg_allowed() rejected */
	unsigned int reads;
	unsigned int writes;
};

static struct odi_wdt_mock_state odi_wdt_mock;

static void odi_wdt_mock_reset(void)
{
	memset(&odi_wdt_mock, 0, sizeof(odi_wdt_mock));
}

static uint32_t *odi_wdt_mock_slot(uint32_t addr)
{
	switch (addr) {
	case ODI_WDT_KICK_REG:  return &odi_wdt_mock.kick;
	case ODI_WDT_STATUS_REG: return &odi_wdt_mock.status;
	case ODI_WDT_CTRL_REG: return &odi_wdt_mock.ctrl;
	default: return NULL;
	}
}

static uint32_t odi_wdt_reg_read(uint32_t addr)
{
	uint32_t *slot = odi_wdt_mock_slot(addr);

	odi_wdt_mock.reads++;
	if (!odi_wdt_reg_allowed(addr) || !slot) {
		odi_wdt_mock.refused++;
		return 0;
	}
	return *slot;
}

static void odi_wdt_reg_write(uint32_t addr, uint32_t val)
{
	uint32_t *slot = odi_wdt_mock_slot(addr);

	odi_wdt_mock.writes++;
	if (!odi_wdt_reg_allowed(addr) || !slot) {
		odi_wdt_mock.refused++;
		return;
	}
	*slot = val;
}

#endif /* __KERNEL__ */

/* ---- Arm / kick / disable / force-reset sequences ----------------------- */

/* odi_wdt_arm() -- programs the control register to the operating point odi_wdt.h
 * documents (ODI_WDT_PRESCALE/TIMEOUT1/TIMEOUT2/RESET_MODE, enabled), the same
 * value U-Boot's own en_wdt already wrote before this driver ever runs, and
 * kicks once immediately, in case the watchdog is already running and
 * boot took a while.
 */
static void odi_wdt_kick(void);

static void odi_wdt_arm(void)
{
	odi_wdt_reg_write(ODI_WDT_CTRL_REG,
			   odi_wdt_ctrl_encode(ODI_WDT_PRESCALE, ODI_WDT_TIMEOUT1,
					       ODI_WDT_TIMEOUT2, ODI_WDT_RESET_MODE, 1));
	odi_wdt_kick();
}

static void odi_wdt_kick(void)
{
	uint32_t cur;

	cur = odi_wdt_reg_read(ODI_WDT_KICK_REG);
	odi_wdt_reg_write(ODI_WDT_KICK_REG, odi_wdt_kick_value(cur));
}

/* odi_wdt_disable() -- clears ENABLE only (a read-modify-write, unlike
 * odi_wdt_arm() which writes every field fresh): whatever PRESCALE/TIMEOUT1/
 * TIMEOUT2/RESET_MODE the register already holds are left alone (clear one
 * bit, touch nothing else).
 */
static void odi_wdt_disable(void)
{
	uint32_t cur = odi_wdt_reg_read(ODI_WDT_CTRL_REG);

	odi_wdt_reg_write(ODI_WDT_CTRL_REG, cur & ~(1U << ODI_WDT_CTRL_ENABLE_BIT));
}

static void odi_wdt_force_reset(void)
{
	odi_wdt_reg_write(ODI_WDT_CTRL_REG, odi_wdt_force_reset_value());
}

#ifdef __KERNEL__

/* wdt_pre_reset_hook -- declared in odi_wdt.h, set by odi_nic.c
 * (CONFIG_ODI_NIC) to its own odi_quiesce_hw(), cleared back to NULL on
 * unload. Called, if non-NULL, immediately before the userland deadline
 * forces a reset: an open DMA engine that outlives the CPU reset needs to
 * be stopped first (kernel/extra/drivers/net/ethernet/odi/README.md).
 */
void (*wdt_pre_reset_hook)(void);

static struct odi_wdt_deadline_state odi_wdt_state;
static struct task_struct *odi_wdt_task;
static struct timer_list odi_wdt_deadline_timer;

/* odi_wdt_flag_lock: serialises the writers of watchdog_flag -- the
 * CTRL register arm or disable, odi_wdt_state.watchdog_enabled, and the
 * kicker kthread start or stop in odi_wdt_thread_maintain(). Two writers
 * racing there could both see odi_wdt_task NULL and start two kickers
 * (the second one leaked), or one could kthread_stop() a task the other
 * just replaced. Process context only (the /proc write and module exit),
 * so a mutex: kthread_stop() sleeps. The kicker itself and the deadline
 * timer only read watchdog_enabled and never take it, so stopping the
 * kicker under it cannot deadlock. A leaf: nothing else is taken under it.
 */
static DEFINE_MUTEX(odi_wdt_flag_lock);

static unsigned int odi_wdt_uptime_s(void)
{
	return (unsigned int)((jiffies - INITIAL_JIFFIES) / HZ);
}

static int odi_wdt_kick_thread(void *data)
{
	while (!kthread_should_stop()) {
		/* TASK_INTERRUPTIBLE, not TASK_UNINTERRUPTIBLE: a kthread
		 * receives no signals, so the sleep timing is unchanged, but
		 * an uninterruptible sleeper counts as a D-state task in the
		 * load average -- pinned it at 1.00 on an otherwise idle box.
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(ODI_WDT_KICK_INTERVAL_S * HZ);
		if (READ_ONCE(odi_wdt_state.watchdog_enabled)) {
			odi_wdt_kick();
			odi_wdt_note_kick(&odi_wdt_state, odi_wdt_uptime_s());
		}
	}
	return 0;
}

static void odi_wdt_thread_maintain(int on)
{
	if (on) {
		if (!odi_wdt_task) {
			odi_wdt_task = kthread_create(odi_wdt_kick_thread, NULL, "odi_wdt");
			if (IS_ERR(odi_wdt_task)) {
				pr_err(DRV_NAME ": kthread_create failed, rc=%ld\n",
				       PTR_ERR(odi_wdt_task));
				odi_wdt_task = NULL;
			} else {
				wake_up_process(odi_wdt_task);
				pr_info(DRV_NAME ": kicker kthread running, interval %u s\n",
					ODI_WDT_KICK_INTERVAL_S);
			}
		}
	} else if (odi_wdt_task) {
		kthread_stop(odi_wdt_task);
		odi_wdt_task = NULL;
	}
}

static void odi_wdt_deadline_timer_fn(struct timer_list *odi_timer_arg)
{
	unsigned int uptime_s = odi_wdt_uptime_s();
	unsigned int actions = odi_wdt_deadline_tick(&odi_wdt_state, uptime_s);


	if (actions & ODI_WDT_ACTION_HEARTBEAT)
		pr_info(DRV_NAME ": alive at %u s, free %lu pages, userland_ok=%d\n",
			uptime_s, nr_free_pages(), odi_wdt_state.userland_ok);

	if (actions & ODI_WDT_ACTION_STALL_REPORT) {
		pr_warn(DRV_NAME ": kicker silent for over %u s at %u s uptime -- current %s pid %d\n",
			ODI_WDT_STALL_THRESHOLD_S, uptime_s, current->comm, current->pid);
		dump_stack();
	}

	if (actions & ODI_WDT_ACTION_FORCE_RESET) {
		pr_emerg(DRV_NAME ": userland did not confirm within %u s (uptime %u) -- resetting\n",
			 ODI_WDT_USERLAND_DEADLINE_S, uptime_s);
		if (wdt_pre_reset_hook)
			wdt_pre_reset_hook();
		odi_wdt_force_reset();
		while (1)
			;
	}

	mod_timer(&odi_wdt_deadline_timer, jiffies + ODI_WDT_TICK_INTERVAL_S * HZ);
}

/* ---- /proc/luna_watchdog -------------------------------------------------
 *
 * Same directory name the file this replaces used, kept so
 * rootfs/skeleton/etc/init.d/rcS needs no change: it already tries
 * /proc/watchdog/... first (an older firmware's name for it) and falls
 * back to /proc/luna_watchdog/...
 */
static struct proc_dir_entry *odi_wdt_proc_dir;

static int odi_wdt_flag_show(struct seq_file *seq, void *v)
{
	seq_printf(seq, "watchdog_flag=%d\n", odi_wdt_state.watchdog_enabled);
	return 0;
}

static int odi_wdt_flag_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_wdt_flag_show, NULL);
}

static ssize_t odi_wdt_flag_write(struct file *file, const char __user *buf,
				   size_t size, loff_t *pos)
{
	char tmp[16] = { 0 };
	int len = (size > 15) ? 15 : (int)size;
	unsigned long val;

	if (!buf || copy_from_user(tmp, buf, len))
		return -EFAULT;

	if (kstrtoul(tmp, 0, &val))
		return -EINVAL;

	mutex_lock(&odi_wdt_flag_lock);
	if (val) {
		odi_wdt_arm();
		WRITE_ONCE(odi_wdt_state.watchdog_enabled, 1);
		odi_wdt_thread_maintain(1);
	} else {
		odi_wdt_disable();
		WRITE_ONCE(odi_wdt_state.watchdog_enabled, 0);
		odi_wdt_thread_maintain(0);
	}
	mutex_unlock(&odi_wdt_flag_lock);
	pr_info(DRV_NAME ": watchdog_flag=%d\n", val ? 1 : 0);
	return size;
}

static const struct proc_ops odi_wdt_flag_fops = {
	.proc_open = odi_wdt_flag_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release, .proc_write = odi_wdt_flag_write,
};

static int odi_wdt_userland_show(struct seq_file *seq, void *v)
{
	seq_printf(seq, "userland_ok=%d deadline=%u uptime=%u\n",
		   odi_wdt_state.userland_ok, ODI_WDT_USERLAND_DEADLINE_S, odi_wdt_uptime_s());
	return 0;
}

static int odi_wdt_userland_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_wdt_userland_show, NULL);
}

static ssize_t odi_wdt_userland_write(struct file *file, const char __user *buf,
				       size_t size, loff_t *pos)
{
	char tmp[16] = { 0 };
	int len = (size > 15) ? 15 : (int)size;
	unsigned long val;

	if (!buf || copy_from_user(tmp, buf, len))
		return -EFAULT;

	if (kstrtoul(tmp, 0, &val))
		return -EINVAL;

	odi_wdt_state.userland_ok = val ? 1 : 0;
	pr_info(DRV_NAME ": userland_ok=%d at %u s\n", odi_wdt_state.userland_ok, odi_wdt_uptime_s());
	return size;
}

static const struct proc_ops odi_wdt_userland_fops = {
	.proc_open = odi_wdt_userland_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release, .proc_write = odi_wdt_userland_write,
};

static int __init odi_wdt_init(void)
{
	odi_wdt_deadline_state_init(&odi_wdt_state);

	/* The watchdog was always enabled, unconditionally
	 * -- this build has never shipped it any other way, so there is no
	 * knob to preserve here: always arm and start the kicker at boot.
	 */
	odi_wdt_arm();
	odi_wdt_state.watchdog_enabled = 1;
	odi_wdt_thread_maintain(1);

	odi_wdt_proc_dir = proc_mkdir("luna_watchdog", NULL);
	if (!odi_wdt_proc_dir) {
		pr_err(DRV_NAME ": create /proc/luna_watchdog failed\n");
		return 0; /* never fail boot over a missing /proc entry */
	}
	if (!proc_create("watchdog_flag", 0644, odi_wdt_proc_dir, &odi_wdt_flag_fops))
		pr_err(DRV_NAME ": create /proc/luna_watchdog/watchdog_flag failed\n");
	if (!proc_create("userland_ok", 0644, odi_wdt_proc_dir, &odi_wdt_userland_fops))
		pr_err(DRV_NAME ": create /proc/luna_watchdog/userland_ok failed\n");

	timer_setup(&odi_wdt_deadline_timer, odi_wdt_deadline_timer_fn, 0);
	mod_timer(&odi_wdt_deadline_timer, jiffies + ODI_WDT_TICK_INTERVAL_S * HZ);

	pr_info(DRV_NAME ": armed (prescale=%u timeout1=%u timeout2=%u reset_mode=%u), deadline %u s\n",
		ODI_WDT_PRESCALE, ODI_WDT_TIMEOUT1, ODI_WDT_TIMEOUT2, ODI_WDT_RESET_MODE,
		ODI_WDT_USERLAND_DEADLINE_S);
	return 0;
}

static void __exit odi_wdt_exit(void)
{
	timer_delete_sync(&odi_wdt_deadline_timer);
	mutex_lock(&odi_wdt_flag_lock);
	odi_wdt_disable();
	odi_wdt_thread_maintain(0);
	mutex_unlock(&odi_wdt_flag_lock);
}

module_init(odi_wdt_init);
module_exit(odi_wdt_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss watchdog kicker, CONFIG_ODI_WDT");

#endif /* __KERNEL__ */
