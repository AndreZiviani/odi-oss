// SPDX-License-Identifier: GPL-2.0
/*
 * odi_wdt.c -- the watchdog of odi_wdt.h, in three parts:
 *
 *  - pure functions (the register encodings and the deadline decision),
 *    tested on the host with a fake clock;
 *  - the arm, kick, disable and force-reset sequences, through odi_soc.c,
 *    tested on the host against test/odi_soc_mock.h;
 *  - kernel only: the kicker thread, the deadline timer, the restart
 *    handler, the halt/power-off notifier and /proc/odi_wdt (two entries,
 *    watchdog_flag and userland_ok; the stock firmware calls the directory
 *    /proc/luna_watchdog, a name our own entries owe nothing to).
 */
#include "odi_wdt.h"
#include "odi_soc.h"

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
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
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

void odi_wdt_note_health(struct odi_wdt_deadline_state *st, unsigned int uptime_s)
{
	st->last_health_s = uptime_s;
	if (!st->health_period_s)
		st->health_period_s = ODI_WDT_HEALTH_PERIOD_S;
	st->health_armed = 1;
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

	if (st->watchdog_enabled && st->health_armed && st->health_period_s &&
	    !st->health_reset_signaled &&
	    uptime_s > st->last_health_s + st->health_period_s) {
		st->health_reset_signaled = 1;
		actions |= ODI_WDT_ACTION_FORCE_RESET | ODI_WDT_ACTION_HEALTH_MISS;
	}

	return actions;
}

/* ---- Register access ------------------------------------------------------ */

/* Through odi_soc.c and its allowlist; test/odi_soc_mock.h on the host. */
static uint32_t odi_wdt_reg_read(uint32_t off)
{
	return odi_soc_read(off);
}

static void odi_wdt_reg_write(uint32_t off, uint32_t val)
{
	(void)odi_soc_write(off, val);
}

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
	odi_wdt_reg_write(SOC_WDT_CTRL,
			   odi_wdt_ctrl_encode(ODI_WDT_PRESCALE, ODI_WDT_TIMEOUT1,
					       ODI_WDT_TIMEOUT2, ODI_WDT_RESET_MODE, 1));
	odi_wdt_kick();
}

static void odi_wdt_kick(void)
{
	uint32_t cur;

	cur = odi_wdt_reg_read(SOC_WDT_KICK);
	odi_wdt_reg_write(SOC_WDT_KICK, odi_wdt_kick_value(cur));
}

/* odi_wdt_disable() -- clears ENABLE only (a read-modify-write, unlike
 * odi_wdt_arm() which writes every field fresh): whatever PRESCALE/TIMEOUT1/
 * TIMEOUT2/RESET_MODE the register already holds are left alone (clear one
 * bit, touch nothing else).
 */
static void odi_wdt_disable(void)
{
	uint32_t cur = odi_wdt_reg_read(SOC_WDT_CTRL);

	odi_wdt_reg_write(SOC_WDT_CTRL, cur & ~(1U << ODI_WDT_CTRL_ENABLE_BIT));
}

/* odi_wdt_force_reset() -- the shortest timeout, then a kick. The timeout
 * fields do not restart the count: measured on the stick, the control
 * write alone reset the board about 1.05 s later, and the same write
 * followed by a kick about 0.33 s later. The kick clears the count, so
 * the short timeout runs from zero.
 */
static void odi_wdt_force_reset(void)
{
	odi_wdt_reg_write(SOC_WDT_CTRL, odi_wdt_force_reset_value());
	odi_wdt_kick();
}

#ifdef __KERNEL__

/* wdt_pre_reset_hook -- declared in odi_wdt.h, set by odi_nic.c
 * (CONFIG_ODI_NIC) to its own odi_quiesce_hw(), cleared back to NULL on
 * unload. Called, if non-NULL, immediately before every reset this
 * driver forces, from the userland deadline and from the restart handler: an open DMA engine that outlives the CPU reset needs to
 * be stopped first (kernel/extra/drivers/net/ethernet/odi/README.md).
 */
void (*wdt_pre_reset_hook)(void);

/* Stop the NIC DMA, then reset through the watchdog. The quiesce is MMIO
 * writes only, so this is safe with interrupts off. The hook is called
 * again here even on an orderly reboot, where the odi_nic reboot notifier
 * already ran it: an emergency restart (panic, sysrq) skips the notifiers.
 */
static void odi_wdt_reset_now(void)
{
	if (wdt_pre_reset_hook)
		wdt_pre_reset_hook();
	odi_wdt_force_reset();
}

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

	if (actions & ODI_WDT_ACTION_HEALTH_MISS)
		pr_emerg(DRV_NAME ": health kick missed for over %u s (uptime %u) -- resetting\n",
			 odi_wdt_state.health_period_s, uptime_s);
	else if (actions & ODI_WDT_ACTION_FORCE_RESET)
		pr_emerg(DRV_NAME ": userland did not confirm within %u s (uptime %u) -- resetting\n",
			 ODI_WDT_USERLAND_DEADLINE_S, uptime_s);

	if (actions & ODI_WDT_ACTION_FORCE_RESET) {
		odi_wdt_reset_now();
		while (1)
			;
	}

	mod_timer(&odi_wdt_deadline_timer, jiffies + ODI_WDT_TICK_INTERVAL_S * HZ);
}

/* ---- Restart, halt and power-off ----------------------------------------
 *
 * machine_restart() has no board hook on this port, so it runs the restart
 * handler chain, and this is the only handler on it: the watchdog is the
 * one reset the SoC has. Priority 128 is the level the kernel documents
 * for the default handler that restarts the whole system. It runs last,
 * after the reboot notifiers and device shutdown, or directly from an
 * emergency restart. odi_wdt_force_reset() resets the board about 0.33 s
 * later; the wait after it is only a fallback, well above that. If the
 * board is still running after it, the watchdog is re-armed at its normal operating
 * point and the handler returns: machine_restart() then masks interrupts
 * and hangs, nothing kicks, and the reset comes within the normal window,
 * which is where every reboot ended before this handler existed.
 */
#define ODI_WDT_RESTART_WAIT_MS	3000U

static int odi_wdt_restart(struct notifier_block *nb, unsigned long mode, void *cmd)
{
	local_irq_disable();
	pr_emerg(DRV_NAME ": restarting through the watchdog\n");
	odi_wdt_reset_now();
	mdelay(ODI_WDT_RESTART_WAIT_MS);
	pr_emerg(DRV_NAME ": no reset after %u ms, re-armed for the normal timeout\n",
		 ODI_WDT_RESTART_WAIT_MS);
	odi_wdt_arm();
	return NOTIFY_DONE;
}

static struct notifier_block odi_wdt_restart_nb = {
	.notifier_call	= odi_wdt_restart,
	.priority	= 128,
};

/* Halt and power-off. The stick has no power switch the SoC can drive, and
 * a halted stick in a cage nobody can reach is only useful once it resets,
 * so both end the same way: the watchdog armed at its normal operating
 * point (even if watchdog_flag had turned it off), then mainline
 * machine_hang(), with interrupts masked and nothing kicking. The board
 * resets within the normal window, about 42 s, and a trial boot falls back
 * to the committed slot as it would on a hang. The odi_nic reboot notifier
 * stops the NIC DMA on these paths too.
 */
static int odi_wdt_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	if (action != SYS_HALT && action != SYS_POWER_OFF)
		return NOTIFY_DONE;
	mutex_lock(&odi_wdt_flag_lock);
	odi_wdt_arm();
	WRITE_ONCE(odi_wdt_state.watchdog_enabled, 1);
	mutex_unlock(&odi_wdt_flag_lock);
	pr_emerg(DRV_NAME ": %s: the watchdog resets the board in about 42 s\n",
		 action == SYS_HALT ? "halt" : "power-off");
	return NOTIFY_DONE;
}

static struct notifier_block odi_wdt_reboot_nb = {
	.notifier_call	= odi_wdt_reboot_notify,
};

/* ---- /proc/odi_wdt -------------------------------------------------
 *
 * The stock firmware calls this /proc/luna_watchdog; this is our own name.
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

/* /proc/odi_wdt/health_kick -- periodic health confirmation, independent of
 * userland_ok above. A write of any value arms it (odi_wdt_note_health())
 * and resets the deadline; reading shows the period and how long ago the
 * last kick landed. Nothing writes this unless a supervised health kicker
 * is running (docs/SETTINGS.md), so a stick that never starts one behaves
 * exactly as before this feature existed.
 */
static int odi_wdt_health_show(struct seq_file *seq, void *v)
{
	unsigned int uptime_s = odi_wdt_uptime_s();

	seq_printf(seq, "period=%u armed=%d last_kick=%u uptime=%u\n",
		   odi_wdt_state.health_period_s, odi_wdt_state.health_armed,
		   odi_wdt_state.last_health_s, uptime_s);
	return 0;
}

static int odi_wdt_health_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_wdt_health_show, NULL);
}

static ssize_t odi_wdt_health_write(struct file *file, const char __user *buf,
				     size_t size, loff_t *pos)
{
	odi_wdt_note_health(&odi_wdt_state, odi_wdt_uptime_s());
	pr_info(DRV_NAME ": health_kick, period=%u\n", odi_wdt_state.health_period_s);
	return size;
}

static const struct proc_ops odi_wdt_health_fops = {
	.proc_open = odi_wdt_health_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release, .proc_write = odi_wdt_health_write,
};

static int __init odi_wdt_init(void)
{
	odi_wdt_deadline_state_init(&odi_wdt_state);
	if (odi_soc_ensure() != 0)
		return 0; /* nothing to kick: U-Boot armed it, so it resets in about 42 s */

	/* The watchdog was always enabled, unconditionally
	 * -- this build has never shipped it any other way, so there is no
	 * knob to preserve here: always arm and start the kicker at boot.
	 */
	odi_wdt_arm();
	odi_wdt_state.watchdog_enabled = 1;
	odi_wdt_thread_maintain(1);

	odi_wdt_proc_dir = proc_mkdir("odi_wdt", NULL);
	if (!odi_wdt_proc_dir) {
		pr_err(DRV_NAME ": create /proc/odi_wdt failed\n");
		return 0; /* never fail boot over a missing /proc entry */
	}
	if (!proc_create("watchdog_flag", 0644, odi_wdt_proc_dir, &odi_wdt_flag_fops))
		pr_err(DRV_NAME ": create /proc/odi_wdt/watchdog_flag failed\n");
	if (!proc_create("userland_ok", 0644, odi_wdt_proc_dir, &odi_wdt_userland_fops))
		pr_err(DRV_NAME ": create /proc/odi_wdt/userland_ok failed\n");
	if (!proc_create("health_kick", 0644, odi_wdt_proc_dir, &odi_wdt_health_fops))
		pr_err(DRV_NAME ": create /proc/odi_wdt/health_kick failed\n");

	register_restart_handler(&odi_wdt_restart_nb);
	register_reboot_notifier(&odi_wdt_reboot_nb);

	timer_setup(&odi_wdt_deadline_timer, odi_wdt_deadline_timer_fn, 0);
	mod_timer(&odi_wdt_deadline_timer, jiffies + ODI_WDT_TICK_INTERVAL_S * HZ);

	pr_info(DRV_NAME ": armed (prescale=%u timeout1=%u timeout2=%u reset_mode=%u), deadline %u s\n",
		ODI_WDT_PRESCALE, ODI_WDT_TIMEOUT1, ODI_WDT_TIMEOUT2, ODI_WDT_RESET_MODE,
		ODI_WDT_USERLAND_DEADLINE_S);
	return 0;
}

module_init(odi_wdt_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss watchdog kicker, CONFIG_ODI_WDT");

#endif /* __KERNEL__ */
