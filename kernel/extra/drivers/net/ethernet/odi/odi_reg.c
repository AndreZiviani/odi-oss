// SPDX-License-Identifier: GPL-2.0
/*
 * odi_reg.c -- /dev/odi_sw, a misc device exposing the switch-core
 * register and per-port MIB accessors of odi_switch_mib.c (which
 * has the counter mapping and what is left out), and the L2 lookup table of odi_switch_l2.c: row readback, the
 * valid-row walk behind `diag l2-table`, and the L2 multicast add/delete
 * igmpd programs groups with, its snooping lease and the way it sends a
 * taken report up; the ACL probes of `diag acl-probe`
 * (odi_switch_acl_probe.h), off until a shell on the stick asks; and the
 * multicast group counters igmpd meters its groups with
 * (odi_switch_mc_count.h).
 * Replaces the stock kernel's handling of RTK_OPT_REGISTER,
 * RTK_OPT_ADDRESS_GET/SET and RTK_OPT_STAT_PORT for our own userland:
 * src/diag reads this device directly. There is no vendor sockopt
 * fallback anywhere in this tree.
 *
 * Built into CONFIG_ODI_SWITCH alongside odi_switch.c -- not a separate
 * Kconfig symbol, since every accessor here is switch-core-register work
 * odi_switch.c already owns the MMIO window for.
 *
 * RTK_OPT_TRANSCEIVER (DDM, ODI_SW_IOC_DDM_GET) is answered here too, but
 * not through the switch-core leaves: it needs no per-leaf register mapping,
 * only the I2C indirect-controller trigger sequence odi_i2c.c reproduces
 * from a capture of a working vendor DDM read, and odi_ddm.c's own
 * selector table. OMCI cmd 10 (odi_switch_cmd.c) reads its numeric
 * fields through the same odi_ddm_get(). odi_ddm.h only
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
#include <linux/sched.h>
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
#include "odi_switch_mcast.h"
#include "odi_switch_acl_probe.h"
#include "odi_switch_mc_count.h"

#define DRV_NAME "odi_reg"

/* odi_switch_l2.c answers -1 for an engine that stayed busy and plain
 * negative errno values otherwise.
 */
static long odi_sw_l2_errno(int rc)
{
	return rc == -1 ? -EBUSY : rc;
}

static long odi_sw_ioctl_cmd(struct file *f, unsigned int cmd, void __user *argp);

/* The open file that holds the snooping lease (odi_switch_mcast.h), if any.
 * Read and written only under odi_switch_lock.
 */
static struct file *odi_sw_snoop_owner;

/* The open file that holds the ACL probes (odi_switch_acl_probe.h), if
 * any, under odi_switch_lock like the lease. */
static struct file *odi_sw_probe_owner;

/* The open file that holds the multicast group counters
 * (odi_switch_mc_count.h), if any, under odi_switch_lock too. */
static struct file *odi_sw_mcc_owner;

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

	if (cmd == ODI_SW_IOC_DDM_GET)
		return odi_sw_ioctl_cmd(f, cmd, argp);

	mutex_lock(&odi_switch_lock);
	rc = odi_sw_ioctl_cmd(f, cmd, argp);
	mutex_unlock(&odi_switch_lock);
	return rc;
}

/* The close of the file that holds the lease, however it comes: the
 * snooper exiting, being killed, or never calling back. The switch goes
 * back to flooding and the rows it wrote are deleted.
 */
static int odi_sw_release(struct inode *inode, struct file *f)
{
	(void)inode;
	mutex_lock(&odi_switch_lock);
	if (odi_sw_snoop_owner == f) {
		odi_switch_mcast_release();
		odi_sw_snoop_owner = NULL;
		pr_info(DRV_NAME ": snooping lease released\n");
	}
	if (odi_sw_probe_owner == f) {
		odi_sw_probe_release();
		odi_sw_probe_owner = NULL;
		pr_info(DRV_NAME ": ACL probes removed\n");
	}
	if (odi_sw_mcc_owner == f) {
		odi_sw_mc_count_release();
		odi_sw_mcc_owner = NULL;
		pr_info(DRV_NAME ": multicast group counters removed\n");
	}
	mutex_unlock(&odi_switch_lock);
	return 0;
}

/* The commands themselves; odi_sw_ioctl() above decides the locking. */
static long odi_sw_ioctl_cmd(struct file *f, unsigned int cmd, void __user *argp)
{
	switch (cmd) {
	case ODI_SW_IOC_REG_GET: {
		struct odi_sw_reg r;
		int rc;

		if (copy_from_user(&r, argp, sizeof r))
			return -EFAULT;
		rc = odi_sw_reg_get(r.addr, &r.value);
		if (rc)
			return rc == -EPERM ? -EPERM : -EINVAL;
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
		/* The lease holder's rows are tracked, so its close deletes
		 * them; anyone else's (igmpd -j) are the caller's to remove. */
		if (cmd == ODI_SW_IOC_L2_MC_ADD) {
			rc = f == odi_sw_snoop_owner ?
			     odi_switch_mcast_row_add(&m.req, &m.index) :
			     odi_switch_l2_mcast_add(&m.req, &m.index);
			found = 1;
		} else {
			rc = f == odi_sw_snoop_owner ?
			     odi_switch_mcast_row_del(&m.req, &found) :
			     odi_switch_l2_mcast_del(&m.req, &found);
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
	case ODI_SW_IOC_MC_SNOOP: {
		struct odi_sw_mc_snoop sn;
		int rc;

		if (copy_from_user(&sn, argp, sizeof(sn)))
			return -EFAULT;
		/* One holder at a time: a second snooper must not take the
		 * first one's lease, and a one-shot tool must not release it.
		 * Nor while ACL probes are in: a probe of the host's IGMP and
		 * the copy rule would decide for each other. */
		if (odi_sw_snoop_owner && odi_sw_snoop_owner != f)
			return -EBUSY;
		if (sn.flags && odi_sw_probe_owner)
			return -EBUSY;
		if (!sn.flags) {
			if (odi_sw_snoop_owner == f) {
				odi_switch_mcast_release();
				odi_sw_snoop_owner = NULL;
			}
			return 0;
		}
		rc = odi_switch_mcast_snoop_set(sn.flags);
		if (rc)
			return rc;
		odi_sw_snoop_owner = f;
		return 0;
	}
	case ODI_SW_IOC_MC_ROUTE: {
		struct odi_sw_mc_route r;
		int rc;

		if (copy_from_user(&r, argp, sizeof(r)))
			return -EFAULT;
		rc = odi_switch_mcast_route(&r);
		if (rc)
			return rc;
		if (copy_to_user(argp, &r, sizeof(r)))
			return -EFAULT;
		return 0;
	}
	case ODI_SW_IOC_MC_SEND: {
		/* Static, not on the stack: 1.5 KiB is a lot of a kernel
		 * stack, and every ioctl here runs under odi_switch_lock, so
		 * there is one caller at a time. */
		static struct odi_sw_mc_send m;

		/* The holder of the lease alone: what goes up is a report it
		 * took, and nothing is anyone else's to send. */
		if (f != odi_sw_snoop_owner)
			return -EPERM;
		if (copy_from_user(&m, argp, sizeof(m)))
			return -EFAULT;
		if (m.len > sizeof(m.frame))
			return -EINVAL;
		return odi_switch_mcast_send(m.frame, m.len, m.flow);
	}
	case ODI_SW_IOC_ACL_PROBE: {
		struct odi_sw_acl_probe pr;
		int rc;

		if (copy_from_user(&pr, argp, sizeof(pr)))
			return -EFAULT;
		/* Reading is open to anyone; installing and removing belong
		 * to the file that installed first, until it closes. */
		if (pr.op != ODI_SW_PROBE_GET && odi_sw_probe_owner && odi_sw_probe_owner != f)
			return -EBUSY;
		rc = odi_sw_probe_request(&pr);
		if (rc)
			return rc;
		if (pr.op == ODI_SW_PROBE_SET && !odi_sw_probe_owner) {
			odi_sw_probe_owner = f;
			pr_info(DRV_NAME ": ACL probe installed, held by pid %d\n",
				task_pid_nr(current));
		}
		if (!odi_sw_probe_active())
			odi_sw_probe_owner = NULL;
		if (copy_to_user(argp, &pr, sizeof(pr)))
			return -EFAULT;
		return 0;
	}
	case ODI_SW_IOC_MC_COUNT: {
		struct odi_sw_mc_count mc;
		int rc;

		if (copy_from_user(&mc, argp, sizeof(mc)))
			return -EFAULT;
		/* As the probes: reading is open to anyone; installing and
		 * removing belong to the file that installed first, until it
		 * closes. */
		if (mc.op != ODI_SW_MCC_GET && odi_sw_mcc_owner && odi_sw_mcc_owner != f)
			return -EBUSY;
		rc = odi_sw_mc_count_request(&mc);
		if (rc)
			return rc;
		if (mc.op == ODI_SW_MCC_SET && !odi_sw_mcc_owner) {
			odi_sw_mcc_owner = f;
			pr_info(DRV_NAME ": multicast group counter installed, held by pid %d\n",
				task_pid_nr(current));
		}
		if (!odi_sw_mc_count_active())
			odi_sw_mcc_owner = NULL;
		if (copy_to_user(argp, &mc, sizeof(mc)))
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
	.release        = odi_sw_release,
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
