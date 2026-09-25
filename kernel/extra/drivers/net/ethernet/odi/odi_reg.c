// SPDX-License-Identifier: GPL-2.0
/*
 * odi_reg.c -- /dev/odi_sw, a misc device exposing the switch-core
 * register, SoC-address, and per-port MIB accessors odi_switch_dal.c
 * implements (odi_switch_dal.h has the per-leaf mapping and what is left
 * out). Replaces the stock kernel's handling of RTK_OPT_REGISTER,
 * RTK_OPT_ADDRESS_GET/SET, RTK_OPT_SOC_GET and RTK_OPT_STAT_PORT for our
 * own userland: src/diag reads this device directly. There is no vendor
 * sockopt fallback anywhere in this tree.
 *
 * Built into CONFIG_ODI_SWITCH alongside odi_switch.c -- not a separate
 * Kconfig symbol, since every accessor here is switch-core-register work
 * odi_switch.c already owns the MMIO window for.
 *
 * SoC-address get (RTK_OPT_SOC_GET, ioctl ODI_SW_IOC_SOC_GET) is answered
 * here directly rather than in odi_switch_dal.c, because there is nothing
 * to map yet: this SoC-level register window (physical 0x18000000, 20 KB,
 * a different block entirely from the switch-core window odi_switch.c
 * maps) has no address in it this codebase has ever seen read or written
 * by a capture, and no userland path in this tree calls RTK_OPT_SOC_GET at
 * all -- src/diag has no such command. Answering with a real MMIO mapping
 * and an empty allowlist is the
 * safe middle ground: the ioctl exists and fails cleanly (-ENXIO) for
 * everything, rather than either lying with an unmapped read or leaving
 * this address space with no ioctl at all. Extend
 * odi_sw_soc_allowed() once a specific SoC offset is actually needed and
 * its safety confirmed by a trace -- never by inference from this file's
 * own comments: this same SoC window is the one an earlier incident on
 * this driver mistook the switch-core window for, with a write that hung
 * the SoC (see odi_switch_hw.h's own history for that address).
 *
 * RTK_OPT_TRANSCEIVER (DDM, ODI_SW_IOC_DDM_GET) is answered here too, but
 * not through odi_switch_dal.c: it needs no per-leaf register mapping,
 * only the I2C indirect-controller trigger sequence odi_i2c.c reproduces
 * from a capture of a working vendor DDM read, and odi_ddm.c's own
 * selector table. (odi_switch_dal.c's own
 * odi_sw_ponmac_transceiver_get() is a GPIO enable pair from an unrelated
 * OMCI command, not this read, and is untouched by this.) odi_ddm.h only
 * names selectors 0-6 (ODI_DDM_*): the module serial number (our
 * selector 7, RTK_DDM_SN) has no captured reference and is refused here
 * (odi_ddm_get()'s default case); src/diag has no command that reads it
 * either.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
/*
 * linux/ratelimit.h: pr_info_ratelimited() needs DEFINE_RATELIMIT_STATE,
 * which this kernel's linux/printk.h does not pull in on its own -- same
 * reasoning odi_omci.c's own linux/ratelimit.h include already established.
 */
#include <linux/ratelimit.h>

#include "odi_reg.h"
#include "odi_switch_dal.h"
#include "odi_switch_reg.h" /* odi_switch_lock */
#include "odi_ddm.h"

#define DRV_NAME "odi_reg"

/* No SoC offset has a confirmed-safe row yet -- see this file's own
 * header comment. Refuses everything rather than defining an empty
 * table: a placeholder entry (even {0}) would read as a real one. Add a
 * real comparison here once a specific offset earns a row from an actual
 * trace.
 */
static int odi_sw_soc_allowed(uint32_t addr)
{
	(void)addr;
	return 0;
}

static long odi_sw_ioctl_cmd(unsigned int cmd, void __user *argp);

/* Every command but DDM_GET runs under odi_switch_lock (odi_switch.c), so
 * a register or MIB read never lands in the middle of a table sequence
 * another entry point is running. DDM_GET needs only the I2C master,
 * which odi_i2c_read_bytes() locks itself (odi_i2c_lock); taking
 * odi_switch_lock for it too would only make the exporter DDM poll wait
 * behind a CF rebuild.
 */
static long odi_sw_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	long rc;

	(void)f;
	if (cmd == ODI_SW_IOC_DDM_GET)
		return odi_sw_ioctl_cmd(cmd, argp);

	mutex_lock(&odi_switch_lock);
	rc = odi_sw_ioctl_cmd(cmd, argp);
	mutex_unlock(&odi_switch_lock);
	return rc;
}

/* The commands themselves; odi_sw_ioctl() above decides the locking. */
static long odi_sw_ioctl_cmd(unsigned int cmd, void __user *argp)
{
	switch (cmd) {
	case ODI_SW_IOC_REG_GET: {
		struct odi_sw_reg r;

		if (copy_from_user(&r, argp, sizeof r))
			return -EFAULT;
		if (odi_sw_reg_get(r.addr, &r.value) != 0)
			return -EINVAL;
		if (copy_to_user(argp, &r, sizeof r))
			return -EFAULT;
		return 0;
	}
	case ODI_SW_IOC_REG_SET: {
		struct odi_sw_reg r;

		if (copy_from_user(&r, argp, sizeof r))
			return -EFAULT;
		if (odi_sw_reg_set(r.addr, r.value) != 0)
			return -EINVAL;
		return 0;
	}
	case ODI_SW_IOC_SOC_GET: {
		struct odi_sw_soc s;

		if (copy_from_user(&s, argp, sizeof s))
			return -EFAULT;
		if (!odi_sw_soc_allowed(s.addr))
			return -ENXIO;
		/*
		 * Unreachable until odi_sw_soc_allow[] gains an entry -- see
		 * this file's header comment; no SoC-window ioremap exists
		 * yet either, on purpose.
		 */
		return -ENXIO;
	}
	case ODI_SW_IOC_MIB_GET: {
		struct odi_sw_mib m;

		if (copy_from_user(&m, argp, sizeof m))
			return -EFAULT;
		if (odi_sw_mib_get(m.port, m.counter, &m.value) != 0)
			return -EOPNOTSUPP;
		if (copy_to_user(argp, &m, sizeof m))
			return -EFAULT;
		return 0;
	}
	case ODI_SW_IOC_DDM_GET: {
		struct odi_sw_ddm d;

		if (copy_from_user(&d, argp, sizeof d)) {
			pr_info_ratelimited(DRV_NAME ": DDM_GET copy_from_user fault\n");
			return -EFAULT;
		}
		if (odi_ddm_get((int)d.type, d.raw) != 0)
			return -EOPNOTSUPP;
		if (copy_to_user(argp, &d, sizeof d)) {
			pr_info_ratelimited(DRV_NAME ": DDM_GET copy_to_user fault, type=%u\n", d.type);
			return -EFAULT;
		}
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations odi_sw_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = odi_sw_ioctl,
};

static struct miscdevice odi_sw_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "odi_sw",
	.fops  = &odi_sw_fops,
};

static int __init odi_reg_init(void)
{
	int rc = misc_register(&odi_sw_miscdev);

	if (rc)
		pr_err(DRV_NAME ": misc_register failed, rc=%d\n", rc);
	else
		pr_info(DRV_NAME ": /dev/odi_sw ready\n");
	return rc;
}

static void __exit odi_reg_exit(void)
{
	misc_deregister(&odi_sw_miscdev);
}

module_init(odi_reg_init);
module_exit(odi_reg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss /dev/odi_sw -- register/SoC/MIB ioctls (stock sockopt replacement)");
