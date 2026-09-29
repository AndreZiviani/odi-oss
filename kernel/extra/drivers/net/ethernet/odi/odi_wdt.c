// SPDX-License-Identifier: GPL-2.0
/*
 * odi_wdt.c -- the watchdog of odi_wdt.h, in three parts:
 *
 *  - pure functions (the register encodings and the deadline decision),
 *    tested on the host with a fake clock;
 *  - the arm, kick, disable and force-reset sequences, through odi_soc.c,
 *    tested on the host against test/odi_soc_mock.h;
 *  - kernel only: the kicker thread, the deadline timer, the restart
 *    handler, the halt/power-off notifier and /proc/odi_wdt (watchdog_flag,
 *    userland_ok, register, ping, clients; the stock firmware calls the
 *    directory /proc/luna_watchdog, a name our own entries owe nothing to).
 *
 * The kernel is the ONLY owner of the hardware watchdog: it stops kicking
 * (so the board resets) when any of three rules fails -- (a) the one-shot
 * boot confirmation (userland_ok) within ODI_WDT_USERLAND_DEADLINE_S; (b) a
 * registered client's own ping deadline (register/ping/clients below); (c)
 * MemAvailable held below a floor for several consecutive checks. There is
 * no separate userland process guessing at any of this from /proc (the
 * v1.0.2 health-kicker) -- see docs/SETTINGS.md, "Watchdog rules".
 */
#include "odi_wdt.h"
#include "odi_soc.h"
#include "odi_ramlog.h"

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

/* Tiny, portable (kernel and host) string helpers -- no dependency on which
 * libc/kernel string.h flavour is in scope, and no assumption the caller's
 * name is already NUL-terminated within ODI_WDT_CLIENT_NAME_LEN.
 */
static int odi_wdt_streq(const char *a, const char *b)
{
	unsigned int i;

	for (i = 0; i < ODI_WDT_CLIENT_NAME_LEN; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == '\0')
			return 1;
	}
	return 1;
}

static void odi_wdt_strlcpy(char *dst, const char *src, unsigned int size)
{
	unsigned int i;

	for (i = 0; i + 1 < size && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static struct odi_wdt_client *client_find(struct odi_wdt_deadline_state *st, const char *name)
{
	int i;

	for (i = 0; i < (int)ODI_WDT_MAX_CLIENTS; i++) {
		if (st->clients[i].deadline_s && odi_wdt_streq(st->clients[i].name, name))
			return &st->clients[i];
	}
	return NULL;
}

int odi_wdt_client_register(struct odi_wdt_deadline_state *st, const char *name,
			     unsigned int deadline_s, unsigned int uptime_s)
{
	struct odi_wdt_client *c = client_find(st, name);
	int i;

	if (!c) {
		for (i = 0; i < (int)ODI_WDT_MAX_CLIENTS; i++) {
			if (!st->clients[i].deadline_s) {
				c = &st->clients[i];
				odi_wdt_strlcpy(c->name, name, ODI_WDT_CLIENT_NAME_LEN);
				break;
			}
		}
	}
	if (!c)
		return -1;
	c->deadline_s = deadline_s;
	/* Armed from registration, not from the first ping -- see the header
	 * comment. A re-registration (the idempotent path above) also resets
	 * the clock, same as the client's own first ping would: rcS re-running
	 * in a development boot must not carry a stale, already-expired
	 * deadline into the new run.
	 */
	c->last_ping_s = uptime_s;
	c->armed = 1;
	return (int)(c - st->clients);
}

int odi_wdt_client_ping(struct odi_wdt_deadline_state *st, const char *name,
			 unsigned int uptime_s)
{
	struct odi_wdt_client *c = client_find(st, name);

	if (!c)
		return -1;
	c->last_ping_s = uptime_s;
	c->armed = 1;
	return 0;
}

unsigned int odi_wdt_deadline_tick(struct odi_wdt_deadline_state *st, unsigned int uptime_s,
				    unsigned long free_kb)
{
	unsigned int actions = ODI_WDT_ACTION_NONE;
	int i;

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

	if (st->watchdog_enabled) {
		for (i = 0; i < (int)ODI_WDT_MAX_CLIENTS; i++) {
			struct odi_wdt_client *c = &st->clients[i];

			if (!c->deadline_s || !c->armed || c->reset_signaled)
				continue;
			if (uptime_s > c->last_ping_s + c->deadline_s) {
				c->reset_signaled = 1;
				actions |= ODI_WDT_ACTION_FORCE_RESET | ODI_WDT_ACTION_CLIENT_MISS;
			}
		}
	}

	if (st->watchdog_enabled) {
		if (free_kb < ODI_WDT_MEM_FLOOR_KB) {
			if (st->mem_low_streak < ODI_WDT_MEM_FLOOR_CONSEC)
				st->mem_low_streak++;
		} else {
			st->mem_low_streak = 0;
		}
		if (st->mem_low_streak >= ODI_WDT_MEM_FLOOR_CONSEC && !st->mem_reset_signaled) {
			st->mem_reset_signaled = 1;
			actions |= ODI_WDT_ACTION_FORCE_RESET | ODI_WDT_ACTION_MEM_FLOOR;
		}
	}

	return actions;
}

/* The same miss test odi_wdt_log_client_miss() uses to name a client. */
static int odi_wdt_client_missed(const struct odi_wdt_client *c, unsigned int uptime_s)
{
	return c->deadline_s && c->armed && c->reset_signaled &&
	       uptime_s > c->last_ping_s + c->deadline_s;
}

uint32_t odi_wdt_reset_reason(const struct odi_wdt_deadline_state *st, unsigned int actions,
			       unsigned int uptime_s, const char **client)
{
	int i;

	if (!(actions & ODI_WDT_ACTION_FORCE_RESET))
		return ODI_RAMLOG_REASON_NONE;
	if (actions & ODI_WDT_ACTION_MEM_FLOOR)
		return ODI_RAMLOG_REASON_WDT_MEM;
	if (actions & ODI_WDT_ACTION_CLIENT_MISS) {
		for (i = 0; i < (int)ODI_WDT_MAX_CLIENTS; i++) {
			if (odi_wdt_client_missed(&st->clients[i], uptime_s)) {
				*client = st->clients[i].name;
				break;
			}
		}
		return ODI_RAMLOG_REASON_WDT_CLIENT;
	}
	return ODI_RAMLOG_REASON_WDT_USERLAND;
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

static void odi_wdt_log_client_miss(unsigned int uptime_s)
{
	int i;

	for (i = 0; i < (int)ODI_WDT_MAX_CLIENTS; i++) {
		struct odi_wdt_client *c = &odi_wdt_state.clients[i];

		if (odi_wdt_client_missed(c, uptime_s))
			pr_emerg(DRV_NAME ": client %.*s missed its %u s deadline (last ping %u s ago) -- resetting\n",
				 (int)ODI_WDT_CLIENT_NAME_LEN, c->name,
				 c->deadline_s, uptime_s - c->last_ping_s);
	}
}

static void odi_wdt_deadline_timer_fn(struct timer_list *odi_timer_arg)
{
	unsigned int uptime_s = odi_wdt_uptime_s();
	unsigned long free_kb = si_mem_available() * (PAGE_SIZE / 1024);
	unsigned int actions = odi_wdt_deadline_tick(&odi_wdt_state, uptime_s, free_kb);

	if (actions & ODI_WDT_ACTION_HEARTBEAT)
		pr_info(DRV_NAME ": alive at %u s, free %lu pages (%lu KB available), userland_ok=%d\n",
			uptime_s, nr_free_pages(), free_kb, odi_wdt_state.userland_ok);

	if (actions & ODI_WDT_ACTION_STALL_REPORT) {
		pr_warn(DRV_NAME ": kicker silent for over %u s at %u s uptime -- current %s pid %d\n",
			ODI_WDT_STALL_THRESHOLD_S, uptime_s, current->comm, current->pid);
		dump_stack();
	}

	/* Each rule logs its own line, clearly, before the reset -- this is
	 * read from the DRAM ramlog on the next boot (docs/SETTINGS.md). The
	 * rule is also recorded first, as the ramlog reset reason, so the
	 * next boot has it as reason= even when the text is cut short.
	 */
	if (actions & ODI_WDT_ACTION_FORCE_RESET) {
		const char *client = NULL;

		odi_ramlog_note_reason(odi_wdt_reset_reason(&odi_wdt_state, actions, uptime_s, &client),
				       client);
	}
	if (actions & ODI_WDT_ACTION_MEM_FLOOR)
		pr_emerg(DRV_NAME ": MemAvailable %lu KB below the %u KB floor for %u consecutive checks -- resetting\n",
			 free_kb, ODI_WDT_MEM_FLOOR_KB, ODI_WDT_MEM_FLOOR_CONSEC);
	if (actions & ODI_WDT_ACTION_CLIENT_MISS)
		odi_wdt_log_client_miss(uptime_s);
	if ((actions & ODI_WDT_ACTION_FORCE_RESET) &&
	    !(actions & (ODI_WDT_ACTION_MEM_FLOOR | ODI_WDT_ACTION_CLIENT_MISS)))
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

/* Parses "<name> <deadline_s>" (register) or "<name>" (ping) out of a
 * proc write. copy_from_user once, into a fixed buffer -- neither a client
 * name nor a deadline needs more than this.
 */
static int odi_wdt_parse_write(const char __user *buf, size_t size, char *name,
				unsigned int *deadline_s)
{
	char tmp[32] = { 0 };
	int len = (size >= sizeof(tmp)) ? (int)sizeof(tmp) - 1 : (int)size;
	char *p = tmp;
	int i;

	if (!buf || copy_from_user(tmp, buf, len))
		return -EFAULT;
	while (*p == ' ')
		p++;
	for (i = 0; i < (int)ODI_WDT_CLIENT_NAME_LEN - 1 && *p && *p != ' ' && *p != '\n'; i++, p++)
		name[i] = *p;
	name[i] = '\0';
	if (i == 0)
		return -EINVAL;
	if (deadline_s) {
		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			return -EINVAL;
		*deadline_s = 0;
		while (*p >= '0' && *p <= '9') {
			*deadline_s = *deadline_s * 10 + (unsigned int)(*p - '0');
			p++;
		}
	}
	return 0;
}

/* /proc/odi_wdt/register -- "<name> <deadline_s>", e.g. "omcid 60". rcS
 * writes this once per required client at boot (docs/SETTINGS.md,
 * "Watchdog rules"). Idempotent: registering an already-known name just
 * updates its deadline. Arms immediately, counted from now: a client that
 * registers and then never pings at all is caught by its own deadline,
 * the same as one that pinged once and then stalled (odi_wdt.h).
 */
static ssize_t odi_wdt_register_write(struct file *file, const char __user *buf,
				       size_t size, loff_t *pos)
{
	char name[ODI_WDT_CLIENT_NAME_LEN];
	unsigned int deadline_s;
	int rc;

	rc = odi_wdt_parse_write(buf, size, name, &deadline_s);
	if (rc)
		return rc;
	if (odi_wdt_client_register(&odi_wdt_state, name, deadline_s,
				     odi_wdt_uptime_s()) < 0) {
		pr_err(DRV_NAME ": register: no free client slot for %s\n", name);
		return -ENOSPC;
	}
	pr_info(DRV_NAME ": client %s registered, deadline %u s\n", name, deadline_s);
	return size;
}

static const struct proc_ops odi_wdt_register_fops = {
	.proc_write = odi_wdt_register_write,
};

/* /proc/odi_wdt/ping -- "<name>", e.g. "omcid", written every few seconds
 * from the client's OWN main loop (src/omci/respond/main.c). Arms the
 * client's deadline on its first call. A ping from a name nobody
 * registered is refused (-EINVAL): it is a configuration mismatch, not a
 * client to silently start trusting.
 */
static ssize_t odi_wdt_ping_write(struct file *file, const char __user *buf,
				   size_t size, loff_t *pos)
{
	char name[ODI_WDT_CLIENT_NAME_LEN];
	int rc;

	rc = odi_wdt_parse_write(buf, size, name, NULL);
	if (rc)
		return rc;
	if (odi_wdt_client_ping(&odi_wdt_state, name, odi_wdt_uptime_s()) < 0)
		return -EINVAL;
	return size;
}

static const struct proc_ops odi_wdt_ping_fops = {
	.proc_write = odi_wdt_ping_write,
};

/* /proc/odi_wdt/clients -- read-only, one line per registered slot, for
 * debugging on the running stick (docs/SETTINGS.md): name, its deadline,
 * whether the first ping has armed it yet, and how long ago the last ping
 * landed.
 */
static int odi_wdt_clients_show(struct seq_file *seq, void *v)
{
	unsigned int uptime_s = odi_wdt_uptime_s();
	int i;

	for (i = 0; i < (int)ODI_WDT_MAX_CLIENTS; i++) {
		struct odi_wdt_client *c = &odi_wdt_state.clients[i];

		if (!c->deadline_s)
			continue;
		seq_printf(seq, "name=%.*s deadline=%u armed=%d last_ping_age=%u\n",
			   (int)ODI_WDT_CLIENT_NAME_LEN, c->name, c->deadline_s, c->armed,
			   c->armed ? uptime_s - c->last_ping_s : 0);
	}
	return 0;
}

static int odi_wdt_clients_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_wdt_clients_show, NULL);
}

static const struct proc_ops odi_wdt_clients_fops = {
	.proc_open = odi_wdt_clients_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
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
	if (!proc_create("register", 0200, odi_wdt_proc_dir, &odi_wdt_register_fops))
		pr_err(DRV_NAME ": create /proc/odi_wdt/register failed\n");
	if (!proc_create("ping", 0200, odi_wdt_proc_dir, &odi_wdt_ping_fops))
		pr_err(DRV_NAME ": create /proc/odi_wdt/ping failed\n");
	if (!proc_create("clients", 0444, odi_wdt_proc_dir, &odi_wdt_clients_fops))
		pr_err(DRV_NAME ": create /proc/odi_wdt/clients failed\n");

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
