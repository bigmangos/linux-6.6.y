// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys DesignWare I2C adapter driver.
 *
 * Based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */
#include <linux/acpi.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/units.h>

#include "i2c-designware-core.h"

static u32 i2c_dw_get_clk_rate_khz(struct dw_i2c_dev *dev)
{
	return clk_get_rate(dev->clk) / KILO;
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id dw_i2c_acpi_match[] = {
	{ "INT33C2", 0 },
	{ "INT33C3", 0 },
	{ "INT3432", 0 },
	{ "INT3433", 0 },
	{ "80860F41", ACCESS_NO_IRQ_SUSPEND },
	{ "808622C1", ACCESS_NO_IRQ_SUSPEND },
	{ "AMD0010", ACCESS_INTR_MASK },
	{ "AMDI0010", ACCESS_INTR_MASK },
	{ "AMDI0019", ACCESS_INTR_MASK | ARBITRATION_SEMAPHORE },
	{ "AMDI0510", 0 },
	{ "APMC0D0F", 0 },
	{ "HISI02A1", 0 },
	{ "HISI02A2", 0 },
	{ "HISI02A3", 0 },
	{ "HYGO0010", ACCESS_INTR_MASK },
	{ }
};
MODULE_DEVICE_TABLE(acpi, dw_i2c_acpi_match);
#endif

#ifdef CONFIG_OF
#define BT1_I2C_CTL			0x100
#define BT1_I2C_CTL_ADDR_MASK		GENMASK(7, 0)
#define BT1_I2C_CTL_WR			BIT(8)
#define BT1_I2C_CTL_GO			BIT(31)
#define BT1_I2C_DI			0x104
#define BT1_I2C_DO			0x108

static int bt1_i2c_read(void *context, unsigned int reg, unsigned int *val)
{
	struct dw_i2c_dev *dev = context;
	int ret;

	/*
	 * Note these methods shouldn't ever fail because the system controller
	 * registers are memory mapped. We check the return value just in case.
	 */
	ret = regmap_write(dev->sysmap, BT1_I2C_CTL,
			   BT1_I2C_CTL_GO | (reg & BT1_I2C_CTL_ADDR_MASK));
	if (ret)
		return ret;

	return regmap_read(dev->sysmap, BT1_I2C_DO, val);
}

static int bt1_i2c_write(void *context, unsigned int reg, unsigned int val)
{
	struct dw_i2c_dev *dev = context;
	int ret;

	ret = regmap_write(dev->sysmap, BT1_I2C_DI, val);
	if (ret)
		return ret;

	return regmap_write(dev->sysmap, BT1_I2C_CTL,
		BT1_I2C_CTL_GO | BT1_I2C_CTL_WR | (reg & BT1_I2C_CTL_ADDR_MASK));
}

static struct regmap_config bt1_i2c_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.fast_io = true,
	.reg_read = bt1_i2c_read,
	.reg_write = bt1_i2c_write,
	.max_register = DW_IC_COMP_TYPE,
};

static int bt1_i2c_request_regs(struct dw_i2c_dev *dev)
{
	dev->sysmap = syscon_node_to_regmap(dev->dev->of_node->parent);
	if (IS_ERR(dev->sysmap))
		return PTR_ERR(dev->sysmap);

	dev->map = devm_regmap_init(dev->dev, NULL, dev, &bt1_i2c_cfg);
	return PTR_ERR_OR_ZERO(dev->map);
}

#define MSCC_ICPU_CFG_TWI_DELAY		0x0
#define MSCC_ICPU_CFG_TWI_DELAY_ENABLE	BIT(0)
#define MSCC_ICPU_CFG_TWI_SPIKE_FILTER	0x4

static int mscc_twi_set_sda_hold_time(struct dw_i2c_dev *dev)
{
	writel((dev->sda_hold_time << 1) | MSCC_ICPU_CFG_TWI_DELAY_ENABLE,
	       dev->ext + MSCC_ICPU_CFG_TWI_DELAY);

	return 0;
}

static int dw_i2c_of_configure(struct platform_device *pdev)
{
	struct dw_i2c_dev *dev = platform_get_drvdata(pdev);

	switch (dev->flags & MODEL_MASK) {
	case MODEL_MSCC_OCELOT:
		dev->ext = devm_platform_ioremap_resource(pdev, 1);
		if (!IS_ERR(dev->ext))
			dev->set_sda_hold_time = mscc_twi_set_sda_hold_time;
		break;
	default:
		break;
	}

	return 0;
}

static const struct of_device_id dw_i2c_of_match[] = {
	{ .compatible = "snps,designware-i2c", },
	{ .compatible = "mscc,ocelot-i2c", .data = (void *)MODEL_MSCC_OCELOT },
	{ .compatible = "baikal,bt1-sys-i2c", .data = (void *)MODEL_BAIKAL_BT1 },
	{},
};
MODULE_DEVICE_TABLE(of, dw_i2c_of_match);
#else
static int bt1_i2c_request_regs(struct dw_i2c_dev *dev)
{
	return -ENODEV;
}

static inline int dw_i2c_of_configure(struct platform_device *pdev)
{
	return -ENODEV;
}
#endif

static int txgbe_i2c_request_regs(struct dw_i2c_dev *dev)
{
	dev->map = dev_get_regmap(dev->dev->parent, NULL);
	if (!dev->map)
		return -ENODEV;

	return 0;
}

static void dw_i2c_plat_pm_cleanup(struct dw_i2c_dev *dev)
{
	pm_runtime_disable(dev->dev);

	if (dev->shared_with_punit)
		pm_runtime_put_noidle(dev->dev);
}

static int dw_i2c_plat_request_regs(struct dw_i2c_dev *dev)
{
	struct platform_device *pdev = to_platform_device(dev->dev);
	int ret;

	switch (dev->flags & MODEL_MASK) {
	case MODEL_BAIKAL_BT1:
		ret = bt1_i2c_request_regs(dev);
		break;
	case MODEL_WANGXUN_SP:
		ret = txgbe_i2c_request_regs(dev);
		break;
	default:
		dev->base = devm_platform_ioremap_resource(pdev, 0);
		ret = PTR_ERR_OR_ZERO(dev->base);
		break;
	}

	return ret;
}

static const struct dmi_system_id dw_i2c_hwmon_class_dmi[] = {
	{
		.ident = "Qtechnology QT5222",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Qtechnology"),
			DMI_MATCH(DMI_PRODUCT_NAME, "QT5222"),
		},
	},
	{ } /* terminate list */
};

static const struct i2c_dw_semaphore_callbacks i2c_dw_semaphore_cb_table[] = {
#ifdef CONFIG_I2C_DESIGNWARE_BAYTRAIL
	{
		.probe = i2c_dw_baytrail_probe_lock_support,
	},
#endif
#ifdef CONFIG_I2C_DESIGNWARE_AMDPSP
	{
		.probe = i2c_dw_amdpsp_probe_lock_support,
	},
#endif
	{}
};

static int i2c_dw_probe_lock_support(struct dw_i2c_dev *dev)
{
	const struct i2c_dw_semaphore_callbacks *ptr;
	int i = 0;
	int ret;

	ptr = i2c_dw_semaphore_cb_table;

	dev->semaphore_idx = -1;

	while (ptr->probe) {
		ret = ptr->probe(dev);
		if (ret) {
			/*
			 * If there is no semaphore device attached to this
			 * controller, we shouldn't abort general i2c_controller
			 * probe.
			 */
			if (ret != -ENODEV)
				return ret;

			i++;
			ptr++;
			continue;
		}

		dev->semaphore_idx = i;
		break;
	}

	return 0;
}

static void i2c_dw_remove_lock_support(struct dw_i2c_dev *dev)
{
	if (dev->semaphore_idx < 0)
		return;

	if (i2c_dw_semaphore_cb_table[dev->semaphore_idx].remove)
		i2c_dw_semaphore_cb_table[dev->semaphore_idx].remove(dev);
}

static int dw_i2c_plat_probe(struct platform_device *pdev)
{
	struct device *device = &pdev->dev;
	struct i2c_adapter *adap;
	struct dw_i2c_dev *dev;
	struct i2c_timings *t;
	int irq, ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dev = devm_kzalloc(device, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->flags = (uintptr_t)device_get_match_data(device);
	if (device_property_present(device, "wx,i2c-snps-model"))
		dev->flags = MODEL_WANGXUN_SP | ACCESS_POLLING;

	dev->dev = device;
	dev->irq = irq;
	platform_set_drvdata(pdev, dev);

	ret = dw_i2c_plat_request_regs(dev);
	if (ret)
		return ret;

	dev->rst = devm_reset_control_get_optional_exclusive(device, NULL);
	if (IS_ERR(dev->rst))
		return PTR_ERR(dev->rst);

	reset_control_deassert(dev->rst);

	t = &dev->timings;
	i2c_parse_fw_timings(&pdev->dev, t, false);

	i2c_dw_adjust_bus_speed(dev);

	if (pdev->dev.of_node)
		dw_i2c_of_configure(pdev);

	if (has_acpi_companion(&pdev->dev))
		i2c_dw_acpi_configure(&pdev->dev);

	ret = i2c_dw_validate_speed(dev);
	if (ret)
		goto exit_reset;

	ret = i2c_dw_probe_lock_support(dev);
	if (ret)
		goto exit_reset;

	i2c_dw_configure(dev);

	/* Optional interface clock */
	dev->pclk = devm_clk_get_optional(device, "pclk");
	if (IS_ERR(dev->pclk)) {
		ret = PTR_ERR(dev->pclk);
		goto exit_reset;
	}

	dev->clk = devm_clk_get_optional(device, NULL);
	if (IS_ERR(dev->clk)) {
		ret = PTR_ERR(dev->clk);
		goto exit_reset;
	}

	ret = i2c_dw_prepare_clk(dev, true);
	if (ret)
		goto exit_reset;

	if (dev->clk) {
		u64 clk_khz;

		dev->get_clk_rate_khz = i2c_dw_get_clk_rate_khz;
		clk_khz = dev->get_clk_rate_khz(dev);

		if (!dev->sda_hold_time && t->sda_hold_ns)
			dev->sda_hold_time =
				DIV_S64_ROUND_CLOSEST(clk_khz * t->sda_hold_ns, MICRO);
	}

	adap = &dev->adapter;
	adap->owner = THIS_MODULE;
	adap->class = dmi_check_system(dw_i2c_hwmon_class_dmi) ?
					I2C_CLASS_HWMON : I2C_CLASS_DEPRECATED;
	ACPI_COMPANION_SET(&adap->dev, ACPI_COMPANION(&pdev->dev));
	adap->dev.of_node = pdev->dev.of_node;
	adap->nr = -1;

	if (dev->flags & ACCESS_NO_IRQ_SUSPEND)
		dev_pm_set_driver_flags(device, DPM_FLAG_SMART_PREPARE);
	else
		dev_pm_set_driver_flags(device, DPM_FLAG_SMART_PREPARE | DPM_FLAG_SMART_SUSPEND);

	device_enable_async_suspend(device);

	/* The code below assumes runtime PM to be disabled. */
	WARN_ON(pm_runtime_enabled(device));

	pm_runtime_set_autosuspend_delay(device, 1000);
	pm_runtime_use_autosuspend(device);
	pm_runtime_set_active(device);

	if (dev->shared_with_punit)
		pm_runtime_get_noresume(device);

	pm_runtime_enable(device);

	ret = i2c_dw_probe(dev);
	if (ret)
		goto exit_probe;

	return ret;

exit_probe:
	dw_i2c_plat_pm_cleanup(dev);
exit_reset:
	reset_control_assert(dev->rst);
	return ret;
}

static void dw_i2c_plat_remove(struct platform_device *pdev)
{
	struct dw_i2c_dev *dev = platform_get_drvdata(pdev);
	struct device *device = &pdev->dev;

	pm_runtime_get_sync(device);

	i2c_del_adapter(&dev->adapter);

	i2c_dw_disable(dev);

	pm_runtime_dont_use_autosuspend(device);
	pm_runtime_put_sync(device);
	dw_i2c_plat_pm_cleanup(dev);

	i2c_dw_remove_lock_support(dev);

	reset_control_assert(dev->rst);
}

static int dw_i2c_plat_prepare(struct device *dev)
{
	/*
	 * If the ACPI companion device object is present for this device, it
	 * may be accessed during suspend and resume of other devices via I2C
	 * operation regions, so tell the PM core and middle layers to avoid
	 * skipping system suspend/resume callbacks for it in that case.
	 */
	return !has_acpi_companion(dev);
}

static int dw_i2c_plat_runtime_suspend(struct device *dev)
{
	struct dw_i2c_dev *i_dev = dev_get_drvdata(dev);

	if (i_dev->shared_with_punit)
		return 0;

	i2c_dw_disable(i_dev);
	i2c_dw_prepare_clk(i_dev, false);

	return 0;
}

static int dw_i2c_plat_suspend(struct device *dev)
{
	struct dw_i2c_dev *i_dev = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&i_dev->adapter);

	return dw_i2c_plat_runtime_suspend(dev);
}

static int dw_i2c_plat_runtime_resume(struct device *dev)
{
	struct dw_i2c_dev *i_dev = dev_get_drvdata(dev);

	if (!i_dev->shared_with_punit)
		i2c_dw_prepare_clk(i_dev, true);

	i_dev->init(i_dev);

	return 0;
}

static int dw_i2c_plat_resume(struct device *dev)
{
	struct dw_i2c_dev *i_dev = dev_get_drvdata(dev);

	dw_i2c_plat_runtime_resume(dev);
	i2c_mark_adapter_resumed(&i_dev->adapter);

	return 0;
}

static const struct dev_pm_ops dw_i2c_dev_pm_ops = {
	.prepare = pm_sleep_ptr(dw_i2c_plat_prepare),
	LATE_SYSTEM_SLEEP_PM_OPS(dw_i2c_plat_suspend, dw_i2c_plat_resume)
	RUNTIME_PM_OPS(dw_i2c_plat_runtime_suspend, dw_i2c_plat_runtime_resume, NULL)
};

/* Work with hotplug and coldplug */
MODULE_ALIAS("platform:i2c_designware");

static struct platform_driver dw_i2c_driver = {
	.probe = dw_i2c_plat_probe,
	.remove_new = dw_i2c_plat_remove,
	.driver		= {
		.name	= "i2c_designware",
		.of_match_table = of_match_ptr(dw_i2c_of_match),
		.acpi_match_table = ACPI_PTR(dw_i2c_acpi_match),
		.pm	= pm_ptr(&dw_i2c_dev_pm_ops),
	},
};

static int __init dw_i2c_init_driver(void)
{
	return platform_driver_register(&dw_i2c_driver);
}
subsys_initcall(dw_i2c_init_driver);

static void __exit dw_i2c_exit_driver(void)
{
	platform_driver_unregister(&dw_i2c_driver);
}
module_exit(dw_i2c_exit_driver);

MODULE_AUTHOR("Baruch Siach <baruch@tkos.co.il>");
MODULE_DESCRIPTION("Synopsys DesignWare I2C bus adapter");
MODULE_LICENSE("GPL");
