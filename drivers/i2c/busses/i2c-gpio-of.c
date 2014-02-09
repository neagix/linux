/*
 * drivers/i2c/busses/i2c-gpio-of.c
 *
 * GPIO-based bitbanging I2C driver with OF bindings
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "i2c-gpio-common.h"

#include <linux/of_platform.h>

#include <linux/of_gpio.h>
#include <linux/i2c-algo-bit.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#define DRV_MODULE_NAME	"i2c-gpio-of"
#define DRV_DESCRIPTION	"GPIO-based bitbanging I2C driver with OF bindings"
#define DRV_AUTHOR	"Albert Herranz"

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

/*
 * OF platform bindings.
 *
 */

static int i2c_gpio_of_probe(struct platform_device *odev)
{
	struct i2c_gpio_platform_data *pdata;
	struct i2c_adapter *adap;
	const unsigned long *prop;
	int sda_pin, scl_pin;
	int error = -ENOMEM;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		goto err_alloc_pdata;
	adap = kzalloc(sizeof(*adap), GFP_KERNEL);
	if (!adap)
		goto err_alloc_adap;

	sda_pin = of_get_gpio(odev->dev.of_node, 0);
	scl_pin = of_get_gpio(odev->dev.of_node, 1);
	if (sda_pin < 0 || scl_pin < 0) {
		error = -EINVAL;
		pr_err("%s: invalid GPIO pins, sda=%d/scl=%d\n",
			 odev->dev.of_node->full_name, sda_pin, scl_pin);
		goto err_gpio_pin;
	}

	pdata->sda_pin = sda_pin;
	pdata->scl_pin = scl_pin;

	prop = of_get_property(odev->dev.of_node, "sda-is-open-drain", NULL);
	if (prop)
		pdata->sda_is_open_drain = *prop;
	prop = of_get_property(odev->dev.of_node, "sda-enforce-dir", NULL);
	if (prop)
		pdata->sda_enforce_dir = *prop;
	prop = of_get_property(odev->dev.of_node, "scl-is-open-drain", NULL);
	if (prop)
		pdata->scl_is_open_drain = *prop;
	prop = of_get_property(odev->dev.of_node, "scl-is-output-only", NULL);
	if (prop)
		pdata->scl_is_output_only = *prop;
	prop = of_get_property(odev->dev.of_node, "udelay", NULL);
	if (prop)
		pdata->udelay = *prop;
	prop = of_get_property(odev->dev.of_node, "timeout", NULL);
	if (prop)
		pdata->timeout =  msecs_to_jiffies(*prop);

	error = i2c_gpio_adapter_probe(adap, pdata, &odev->dev,
										odev->dev.of_node->phandle, THIS_MODULE);

	if (error)
		goto err_probe;

	dev_set_drvdata(&odev->dev, adap);



	return 0;

err_probe:
err_gpio_pin:
	kfree(adap);
err_alloc_adap:
	kfree(pdata);
err_alloc_pdata:
	return error;
};

static int i2c_gpio_of_remove(struct platform_device *odev)
{
	struct i2c_gpio_platform_data *pdata;
	struct i2c_adapter *adap;
	struct i2c_algo_bit_data *bit_data;

	adap = dev_get_drvdata(&odev->dev);
	bit_data = adap->algo_data;
	pdata = bit_data->data;

	i2c_gpio_adapter_remove(adap, pdata);
	kfree(pdata);
	kfree(adap);
	return 0;
};

static const struct of_device_id i2c_gpio_of_match[] = {
	{.compatible = "virtual,i2c-gpio",},
	{},
};
MODULE_DEVICE_TABLE(of, i2c_gpio_of_match);

static struct platform_driver i2c_gpio_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = i2c_gpio_of_match,
	},
	.probe = i2c_gpio_of_probe,
	.remove = i2c_gpio_of_remove,
};

static int __init i2c_gpio_of_init(void)
{
	int error;

	error = platform_driver_register(&i2c_gpio_of_driver);
	if (error)
		drv_printk(KERN_ERR, "OF registration failed (%d)\n", error);

	return error;
}

static void __exit i2c_gpio_of_exit(void)
{
	platform_driver_unregister(&i2c_gpio_of_driver);
}

module_init(i2c_gpio_of_init);
module_exit(i2c_gpio_of_exit);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
