/*
 * Bitbanging I2C bus driver using the GPIO API
 *
 * Copyright (C) 2007 Atmel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "i2c-gpio-common.h"

#include <linux/init.h>
#include <linux/platform_device.h>

/*
 * Platform bindings.
 *
 */

static int __devinit i2c_gpio_probe(struct platform_device *pdev)
{
	struct i2c_gpio_platform_data *pdata;
	struct i2c_adapter *adap;
	int error;

	pdata = pdev->dev.platform_data;
	if (!pdata)
		return -ENXIO;

	error = -ENOMEM;
	adap = kzalloc(sizeof(*adap), GFP_KERNEL);
	if (!adap)
		goto err_alloc_adap;
	error = i2c_gpio_adapter_probe(adap, pdata, &pdev->dev, pdev->id,
				       THIS_MODULE);
	if (error)
		goto err_probe;

	platform_set_drvdata(pdev, adap);

	return 0;

err_probe:
	kfree(adap);
err_alloc_adap:
	return error;
}

static int __devexit i2c_gpio_remove(struct platform_device *pdev)
{
	struct i2c_adapter *adap;
	struct i2c_gpio_platform_data *pdata;

	adap = platform_get_drvdata(pdev);
	pdata = pdev->dev.platform_data;

	i2c_gpio_adapter_remove(adap, pdata);
	kfree(adap);
	return 0;
}

static struct platform_driver i2c_gpio_driver = {
	.driver		= {
		.name	= "i2c-gpio",
		.owner	= THIS_MODULE,
	},
	.probe		= i2c_gpio_probe,
	.remove		= __devexit_p(i2c_gpio_remove),
};

static int __init i2c_gpio_init(void)
{
	int error;

	error = platform_driver_register(&i2c_gpio_driver);
	if (error)
		printk(KERN_ERR "i2c-gpio: registration failed (%d)\n", error);

	return error;
}
module_init(i2c_gpio_init);

static void __exit i2c_gpio_exit(void)
{
	platform_driver_unregister(&i2c_gpio_driver);
}
module_exit(i2c_gpio_exit);

MODULE_AUTHOR("Haavard Skinnemoen <hskinnemoen@atmel.com>");
MODULE_DESCRIPTION("Platform-independent bitbanging I2C driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:i2c-gpio");
