// SPDX-License-Identifier: GPL-2.0
/*
 * odi_rtk_init.c -- /proc/rtk_init under CONFIG_ODI_SDK_OUT.
 *
 * A from-scratch replacement for /proc/rtk_init, built only under
 * CONFIG_ODI_SDK_OUT (Kconfig, depends on CONFIG_ODI_BOARD), providing
 * the exact same /proc/rtk_init read/write contract rcS already writes
 * to. No proprietary switch SDK is ever linked into this build, so
 * there is no fallback into it: this file is the only implementation
 * /proc/rtk_init ever has here.
 *
 * CONFIG_ODI_SDK_OUT depending on CONFIG_ODI_BOARD means CONFIG_ODI_GPON,
 * CONFIG_ODI_SDKINIT and CONFIG_ODI_INTR are already forced on by the
 * time this file is ever built -- so
 * every verb rcS ever writes is one of exactly three things, none of
 * them a call into a proprietary SDK:
 *
 *   - "intr"/"irq": a documented no-op. odi_intr_init() (odi_intr.c,
 *     CONFIG_ODI_INTR) already did everything either verb used to at
 *     boot (CHIP_IRQ_SETUP/IMR/IMS reset, request_irq(8, "apl_sw")), before
 *     rcS ever reaches either one.
 *   - the seven GPON PON-step verbs (gpondrv gpondev gponsn gponpw
 *     gponact gpondeact gponstat): odi_gpon_verb().
 *   - every other verb (the 20 switch-table steps, plus the four PON
 *     steps i2c/i2cen/gpon/rxsd that share this same node before the
 *     main rtk_init loop, per rcS/network.sh): odi_switch_sdkinit_verb(),
 *     which resolves against the captured replay tables
 *     (/lib/firmware/odi/sdkinit.bin) -- proven to cover every verb this
 *     image's boot actually exercises (odi-oss image f9, 2026-09-23,
 *     "every runtime path of our image is now served by our own code").
 *
 * A verb this table has no data for is not
 * silently accepted -- it returns -ENOSYS and logs, so a capture gap
 * shows up as a boot-log line and a non-zero /proc/rtk_init read, not as
 * quiet nothing.
 *
 * This also means a genuinely unknown verb name (a typo; nothing rcS
 * itself ever writes) is not distinguished from a known verb with no
 * replay data for this boot -- both return -ENOSYS here, since every
 * valid verb this file accepts routes through the same two functions
 * instead of one match arm apiece. Worth
 * revisiting if a startup ever needs to tell "typo" from "capture gap"
 * apart from the log line's verb name alone.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <stddef.h>
#include <string.h>
#include <errno.h>
#endif

#include "odi_switch_sdkinit.h"
#include "odi_gpon.h"

/* ---- portable core: which bucket a verb name falls into --------------
 * Host-tested (test/odi_rtk_init_test.c, unity build) against stub
 * odi_switch_sdkinit_verb()/odi_gpon_verb() definitions -- the real ones
 * pull in the whole switch/GPON driver, out of scope for this file's own
 * test.
 */
static const char *const odi_rtk_init_gpon_verbs[] = {
	"gpondrv", "gpondev", "gponsn", "gponpw", "gponact", "gpondeact", "gponstat",
};

static int odi_rtk_init_is_gpon_verb(const char *name)
{
	unsigned int i;

	for (i = 0; i < sizeof(odi_rtk_init_gpon_verbs) / sizeof(odi_rtk_init_gpon_verbs[0]); i++)
		if (!strcmp(name, odi_rtk_init_gpon_verbs[i]))
			return 1;
	return 0;
}

/* Returns 0 on success, -ENOSYS when the verb is known but this boot has
 * nothing to serve it (see file header) -- never calls into anything
 * outside this file's own two dependencies.
 */
static int odi_rtk_init_apply(const char *name, const char *arg)
{
	if (!strcmp(name, "intr") || !strcmp(name, "irq"))
		return 0;
	if (odi_rtk_init_is_gpon_verb(name))
		return odi_gpon_verb(name, arg);
	if (odi_switch_sdkinit_verb(name) == 0)
		return 0;
	return -ENOSYS;
}

/* ---- __KERNEL__ only: /proc/rtk_init itself ---------------------------
 * A 64-byte
 * line, verb and an optional argument split on the first space, trailing
 * newline/spaces stripped. single_open() is fine here (unlike
 * /proc/rtk_regtrace) -- the read side is one decimal number, the last
 * verb's return value.
 */
#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/printk.h>

#include "odi_switch_reg.h" /* odi_switch_lock */

static int odi_rtk_init_last_ret;

static int odi_rtk_init_proc_show(struct seq_file *seq, void *v)
{
	(void)v;
	seq_printf(seq, "%d\n", odi_rtk_init_last_ret);
	return 0;
}

static int odi_rtk_init_proc_open(struct inode *inode, struct file *file)
{
	(void)inode;
	return single_open(file, odi_rtk_init_proc_show, NULL);
}

static ssize_t odi_rtk_init_proc_write(struct file *file, const char __user *buffer,
				       size_t count, loff_t *pos)
{
	char name[64];
	char *arg = NULL;
	unsigned long n = count < sizeof(name) - 1 ? count : sizeof(name) - 1;
	int ret;

	(void)file;
	(void)pos;
	if (copy_from_user(name, buffer, n))
		return -EFAULT;
	name[n] = 0;
	while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == ' '))
		name[--n] = 0;
	arg = strchr(name, ' ');
	if (arg) {
		*arg++ = 0;
		while (*arg == ' ')
			arg++;
	}
	/* Every verb replays into the switch core, the GPON ones included
	 * (gponact writes gpon_init.bin through the table engine), so each
	 * one runs whole under odi_switch_lock (odi_switch.c). rcS writes
	 * the verbs one at a time; the lock is for any other writer.
	 */
	pr_info("rtk_init step %s: start\n", name);
	mutex_lock(&odi_switch_lock);
	ret = odi_rtk_init_apply(name, arg);
	odi_rtk_init_last_ret = ret;
	mutex_unlock(&odi_switch_lock);
	pr_info("rtk_init step %s: ret %d\n", name, ret);
	return count;
}

static const struct proc_ops odi_rtk_init_proc_fops = {
	.proc_open = odi_rtk_init_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = odi_rtk_init_proc_write,
};

static int __init odi_rtk_init_init(void)
{
	if (!proc_create("rtk_init", 0644, NULL, &odi_rtk_init_proc_fops))
		pr_err("odi_rtk_init: /proc/rtk_init not created\n");
	return 0;
}
module_init(odi_rtk_init_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss /proc/rtk_init under CONFIG_ODI_SDK_OUT");
#endif /* __KERNEL__ */
