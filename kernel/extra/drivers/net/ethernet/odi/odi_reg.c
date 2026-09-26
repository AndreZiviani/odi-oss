// SPDX-License-Identifier: GPL-2.0
/*
 * odi_reg.c -- /dev/odi_sw, a misc device exposing the switch-core
 * register and per-port MIB accessors of odi_switch_mib.c (which
 * has the counter mapping and what is left out), and the L2 lookup table of odi_switch_l2.c: row readback, the
 * valid-row walk behind `diag l2-table`, and the L2 multicast add/delete
 * igmpd programs groups with. Replaces the stock kernel's handling of RTK_OPT_REGISTER,
 * RTK_OPT_ADDRESS_GET/SET and RTK_OPT_STAT_PORT for our
 * own userland: src/diag reads this device directly. There is no vendor
 * sockopt fallback anywhere in this tree.
 *
 * Built into CONFIG_ODI_SWITCH alongside odi_switch.c -- not a separate
 * Kconfig symbol, since every accessor here is switch-core-register work
 * odi_switch.c already owns the MMIO window for.
 *
 * RTK_OPT_TRANSCEIVER (DDM, ODI_SW_IOC_DDM_GET) is answered here too, but
 * not through the switch-core leaves: it needs no per-leaf register mapping,
 * only the I2C indirect-controller trigger sequence odi_i2c.c reproduces
 * from a capture of a working vendor DDM read, and odi_ddm.c's own
 * selector table. (odi_switch_port.c
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
#include "odi_switch_l2.h"

#define DRV_NAME "odi_reg"

/* odi_switch_l2.c answers -1 for an engine that stayed busy and plain
 * negative errno values otherwise.
 */
static long odi_sw_l2_errno(int rc)
{
	return rc == -1 ? -EBUSY : rc;
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
	case ODI_SW_IOC_L2_GET:
	case ODI_SW_IOC_L2_NEXT: {
		struct odi_sw_l2_row row;
		u32 index;
		int rc;

		if (copy_from_user(&row, argp, sizeof(row)))
			return -EFAULT;
		index = row.index;
		if (cmd == ODI_SW_IOC_L2_GET)
			rc = odi_switch_l2_read(index, &row);
		else
			rc = odi_switch_l2_next(&index, &row);
		if (rc)
			return odi_sw_l2_errno(rc);
		if (copy_to_user(argp, &row, sizeof(row)))
			return -EFAULT;
		return 0;
	}
	case ODI_SW_IOC_L2_MC_ADD:
	case ODI_SW_IOC_L2_MC_DEL: {
		struct odi_sw_l2_mcast m;
		int rc, found = 0;

		if (copy_from_user(&m, argp, sizeof(m)))
			return -EFAULT;
		m.index = 0;
		if (cmd == ODI_SW_IOC_L2_MC_ADD) {
			rc = odi_switch_l2_mcast_add(&m.req, &m.index);
			found = 1;
		} else {
			rc = odi_switch_l2_mcast_del(&m.req, &found);
		}
		if (rc)
			return odi_sw_l2_errno(rc);
		m.found = found;
		if (copy_to_user(argp, &m, sizeof(m)))
			return -EFAULT;
		return 0;
	}
	case ODI_SW_IOC_L2_MODE: {
		struct odi_sw_l2_mode mode;

		mode.ipmc_on_group = odi_switch_l2_ipmc_mode();
		mode.rows = odi_switch_l2_rows();
		if (copy_to_user(argp, &mode, sizeof(mode)))
			return -EFAULT;
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

module_init(odi_reg_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("odi-oss /dev/odi_sw -- register/MIB/L2 table ioctls (stock sockopt replacement)");
