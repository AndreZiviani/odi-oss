// SPDX-License-Identifier: GPL-2.0
/*
 * odi_init.c -- /proc/odi_init: rcS writes one verb at a time, and a
 * read returns the result of the last one. Each verb is one of three:
 *
 *   - "intr", "irq": no-ops; odi_gpon already reset the switch
 *     interrupt registers and requested the line at boot;
 *   - "optics": odi_board_optics(), the first PON step;
 *   - the seven GPON verbs (gpondrv gpondev gponsn gponpw gponact
 *     gpondeact gponstat): odi_gpon_verb();
 *   - every other verb (the 21 switch steps and the PON steps i2c, i2cen,
 *     gpon, rxsd): odi_switch_sdkinit_verb(), the replay of sdkinit.bin.
 *
 * A verb with no data returns -ENOSYS and logs, so a capture gap shows in
 * the boot log and the read-back instead of passing quietly. A misspelt
 * verb gets the same answer.
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
#include "odi_board.h"

/* ---- portable core: which bucket a verb name falls into --------------
 * Host-tested (test/odi_init_test.c, unity build) against stub
 * odi_switch_sdkinit_verb()/odi_gpon_verb() definitions -- the real ones
 * pull in the whole switch/GPON driver, out of scope for this file's own
 * test.
 */
static const char *const odi_init_gpon_verbs[] = {
	"gpondrv", "gpondev", "gponsn", "gponpw", "gponact", "gpondeact", "gponstat",
};

static int odi_init_is_gpon_verb(const char *name)
{
	unsigned int i;

	for (i = 0; i < sizeof(odi_init_gpon_verbs) / sizeof(odi_init_gpon_verbs[0]); i++)
		if (!strcmp(name, odi_init_gpon_verbs[i]))
			return 1;
	return 0;
}

/* Returns 0 on success, -ENOSYS when the verb is known but this boot has
 * nothing to serve it (see file header) -- never calls into anything
 * outside this file's own two dependencies.
 */
static int odi_init_apply(const char *name, const char *arg)
{
	if (!strcmp(name, "intr") || !strcmp(name, "irq"))
		return 0;
	if (!strcmp(name, "optics"))
		return odi_board_optics();
	if (odi_init_is_gpon_verb(name))
		return odi_gpon_verb(name, arg);
	if (odi_switch_sdkinit_verb(name) == 0)
		return 0;
	return -ENOSYS;
}

/* ---- __KERNEL__ only: /proc/odi_init itself ---------------------------
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
#include "odi_soc.h"

static int odi_init_last_ret;

static int odi_init_proc_show(struct seq_file *seq, void *v)
{
	(void)v;
	seq_printf(seq, "%d\n", odi_init_last_ret);
	return 0;
}

static int odi_init_proc_open(struct inode *inode, struct file *file)
{
	(void)inode;
	return single_open(file, odi_init_proc_show, NULL);
}

static ssize_t odi_init_proc_write(struct file *file, const char __user *buffer,
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
	pr_info("odi_init step %s: start\n", name);
	mutex_lock(&odi_switch_lock);
	ret = odi_init_apply(name, arg);
	odi_init_last_ret = ret;
	mutex_unlock(&odi_switch_lock);
	pr_info("odi_init step %s: ret %d\n", name, ret);
	return count;
}

static const struct proc_ops odi_init_proc_fops = {
	.proc_open = odi_init_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = odi_init_proc_write,
};

static int __init odi_init_init(void)
{
	/* The sdkinit replay has SoC records (the gpon verb). */
	(void)odi_soc_ensure();
	if (!proc_create("odi_init", 0644, NULL, &odi_init_proc_fops))
		pr_err("odi_init: /proc/odi_init not created\n");
	return 0;
}
module_init(odi_init_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss /proc/odi_init");
#endif /* __KERNEL__ */
