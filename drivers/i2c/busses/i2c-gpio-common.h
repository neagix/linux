/*
 * Bitbanging I2C bus driver using the GPIO API
 *
 * Copyright (C) 2007 Atmel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __I2C_GPIO_COMMON_H
#define __I2C_GPIO_COMMON_H

#include <linux/i2c.h>
#include <linux/i2c-gpio.h>
#include <linux/module.h>
#include <linux/device.h>

int i2c_gpio_adapter_probe(struct i2c_adapter *adap,
			   struct i2c_gpio_platform_data *pdata,
			   struct device *parent, int id,
			   struct module *owner);
int i2c_gpio_adapter_remove(struct i2c_adapter *adap,
			    struct i2c_gpio_platform_data *pdata);

#endif /* __I2C_GPIO_COMMON_H */
