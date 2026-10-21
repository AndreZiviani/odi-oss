// SPDX-License-Identifier: GPL-2.0
/*
 * odi_hwmon.c -- the SoC die temperature as a standard hwmon device,
 * /sys/class/hwmon/hwmonN with name rtl9602c_soc and temp1_input in
 * millidegrees C (odi_hwmon.h has the registers and the caveat: the scale
 * is not calibrated).
 *
 * Read-only. Each read takes the two words with odi_reg_read(), a plain
 * MMIO load of words the boot already leaves enabled, with no lock and no
 * wait. The hwmon core needs a struct device, so a platform device with no
 * driver carries it.
 */
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/printk.h>

#include "odi_hwmon.h"
#include "odi_switch_reg.h"

#define DRV_NAME "odi_hwmon"

static umode_t odi_hwmon_is_visible(const void *data,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input)
		return 0444;
	return 0;
}

static int odi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	return odi_therm_decode(odi_reg_read(ODI_THERM_CTRL_OFF),
				odi_reg_read(ODI_THERM_STATUS_OFF), val);
}

static const struct hwmon_ops odi_hwmon_ops = {
	.is_visible	= odi_hwmon_is_visible,
	.read		= odi_hwmon_read,
};

static const struct hwmon_channel_info * const odi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
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

	hwmon = devm_hwmon_device_register_with_info(&pdev->dev, "rtl9602c_soc",
						     NULL, &odi_hwmon_chip, NULL);
	if (IS_ERR(hwmon)) {
		platform_device_unregister(pdev);
		pr_err(DRV_NAME ": hwmon registration failed: %ld\n", PTR_ERR(hwmon));
		return PTR_ERR(hwmon);
	}
	return 0;
}
device_initcall(odi_hwmon_init);
