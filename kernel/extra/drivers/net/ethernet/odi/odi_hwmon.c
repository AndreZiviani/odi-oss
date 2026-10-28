// SPDX-License-Identifier: GPL-2.0
/*
 * odi_hwmon.c -- the SoC die temperature as a standard hwmon device,
 * /sys/class/hwmon/hwmonN with name rtl9602c_soc and temp1_input in
 * millidegrees C (odi_hwmon.h has the registers and the caveat: the scale
 * is not calibrated).
 *
 * The reading is read-only: each sample takes the two words with
 * odi_reg_read(), a plain MMIO load of words the boot already leaves
 * enabled, with no wait. On top of it sit soft limits, temp1_max and
 * temp1_crit (writable by root, millidegrees, not persisted), the two alarm
 * flags, and a delayed work that samples every 10 s so a crossing is logged
 * even when nobody reads sysfs. Logging is on transitions only. Nothing
 * powers off or throttles: the scale is not verified (odi_hwmon.h). The
 * hwmon core needs a struct device, so a platform device with no driver
 * carries it.
 */
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/workqueue.h>

#include "odi_hwmon.h"
#include "odi_switch_reg.h"

#define DRV_NAME "odi_hwmon"

#define ODI_HWMON_POLL_MS	10000

static DEFINE_MUTEX(odi_hwmon_lock);
static struct odi_therm_alarms odi_alarms;
static struct device *odi_hwmon_dev;
static struct delayed_work odi_hwmon_work;

/* The one reading path: sysfs reads and the periodic check both come here,
 * so alarms, logs and notifications cannot disagree. Caller holds the lock.
 */
static int odi_hwmon_sample_locked(long *mdeg)
{
	unsigned int ev;
	long t;
	int rc = odi_therm_alarm_sample(&odi_alarms,
					odi_reg_read(ODI_THERM_CTRL_OFF),
					odi_reg_read(ODI_THERM_STATUS_OFF),
					&t, &ev);

	if (rc)
		return rc;
	if (mdeg)
		*mdeg = t;
	if (ev & ODI_THERM_EV_MAX_SET)
		pr_warn(DRV_NAME ": die temperature %ld mC reached max %ld mC (scale not verified)\n",
			t, odi_alarms.max);
	if (ev & ODI_THERM_EV_CRIT_SET)
		pr_crit(DRV_NAME ": die temperature %ld mC reached crit %ld mC (scale not verified)\n",
			t, odi_alarms.crit);
	if (ev & ODI_THERM_EV_CRIT_CLR)
		pr_info(DRV_NAME ": crit alarm cleared at %ld mC\n", t);
	if (ev & ODI_THERM_EV_MAX_CLR)
		pr_info(DRV_NAME ": max alarm cleared at %ld mC\n", t);
	if (odi_hwmon_dev) {
		if (ev & (ODI_THERM_EV_MAX_SET | ODI_THERM_EV_MAX_CLR))
			hwmon_notify_event(odi_hwmon_dev, hwmon_temp,
					   hwmon_temp_max_alarm, 0);
		if (ev & (ODI_THERM_EV_CRIT_SET | ODI_THERM_EV_CRIT_CLR))
			hwmon_notify_event(odi_hwmon_dev, hwmon_temp,
					   hwmon_temp_crit_alarm, 0);
	}
	return 0;
}

static void odi_hwmon_poll(struct work_struct *w)
{
	mutex_lock(&odi_hwmon_lock);
	odi_hwmon_sample_locked(NULL);
	mutex_unlock(&odi_hwmon_lock);
	schedule_delayed_work(&odi_hwmon_work, msecs_to_jiffies(ODI_HWMON_POLL_MS));
}

static umode_t odi_hwmon_is_visible(const void *data,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	if (type != hwmon_temp)
		return 0;
	switch (attr) {
	case hwmon_temp_input:
	case hwmon_temp_max_alarm:
	case hwmon_temp_crit_alarm:
		return 0444;
	case hwmon_temp_max:
	case hwmon_temp_crit:
		return 0644;
	}
	return 0;
}

static int odi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	long t;
	int rc = 0;

	if (type != hwmon_temp)
		return -EOPNOTSUPP;
	mutex_lock(&odi_hwmon_lock);
	switch (attr) {
	case hwmon_temp_input:
		rc = odi_hwmon_sample_locked(val);
		break;
	case hwmon_temp_max:
		*val = odi_alarms.max;
		break;
	case hwmon_temp_crit:
		*val = odi_alarms.crit;
		break;
	case hwmon_temp_max_alarm:
	case hwmon_temp_crit_alarm:
		/* A failed sample leaves the last known state. */
		odi_hwmon_sample_locked(&t);
		*val = attr == hwmon_temp_max_alarm ? odi_alarms.max_alarm
						    : odi_alarms.crit_alarm;
		break;
	default:
		rc = -EOPNOTSUPP;
	}
	mutex_unlock(&odi_hwmon_lock);
	return rc;
}

static int odi_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long val)
{
	long t;
	int rc;

	if (type != hwmon_temp)
		return -EOPNOTSUPP;
	mutex_lock(&odi_hwmon_lock);
	if (attr == hwmon_temp_max)
		rc = odi_therm_set_max(&odi_alarms, val);
	else if (attr == hwmon_temp_crit)
		rc = odi_therm_set_crit(&odi_alarms, val);
	else
		rc = -EOPNOTSUPP;
	if (!rc)
		odi_hwmon_sample_locked(&t);	/* re-evaluate against the new limit */
	mutex_unlock(&odi_hwmon_lock);
	return rc;
}

static const struct hwmon_ops odi_hwmon_ops = {
	.is_visible	= odi_hwmon_is_visible,
	.read		= odi_hwmon_read,
	.write		= odi_hwmon_write,
};

static const struct hwmon_channel_info * const odi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_CRIT |
				 HWMON_T_MAX_ALARM | HWMON_T_CRIT_ALARM),
	NULL
};

static const struct hwmon_chip_info odi_hwmon_chip = {
	.ops	= &odi_hwmon_ops,
	.info	= odi_hwmon_info,
};

static int __init odi_hwmon_init(void)
{
	struct platform_device *pdev;
	struct device *hwmon;
	int rc = odi_switch_mmio_ensure();

	if (rc)
		return rc;

	pdev = platform_device_register_simple("odi_soc_temp", PLATFORM_DEVID_NONE,
					       NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	odi_therm_alarms_init(&odi_alarms);
	hwmon = devm_hwmon_device_register_with_info(&pdev->dev, "rtl9602c_soc",
						     NULL, &odi_hwmon_chip, NULL);
	if (IS_ERR(hwmon)) {
		platform_device_unregister(pdev);
		pr_err(DRV_NAME ": hwmon registration failed: %ld\n", PTR_ERR(hwmon));
		return PTR_ERR(hwmon);
	}
	odi_hwmon_dev = hwmon;
	INIT_DELAYED_WORK(&odi_hwmon_work, odi_hwmon_poll);
	schedule_delayed_work(&odi_hwmon_work, msecs_to_jiffies(ODI_HWMON_POLL_MS));
	return 0;
}
device_initcall(odi_hwmon_init);
