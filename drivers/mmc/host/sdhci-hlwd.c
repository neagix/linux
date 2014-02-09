/*
 * drivers/mmc/host/sdhci-hlwd.c
 *
 * Nintendo Wii Secure Digital Host Controller Interface.
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * Based on sdhci-of.c
 *
 * Copyright (c) 2007 Freescale Semiconductor, Inc.
 * Copyright (c) 2009 MontaVista Software, Inc.
 *
 * Authors: Xiaobo Xie <X.Xie@freescale.com>
 *          Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/mmc/host.h>
#include <asm/starlet.h>
#include <asm/starlet-mini.h>
#include "sdhci.h"

#define DRV_MODULE_NAME "sdhci-hlwd"
#define DRV_DESCRIPTION "Nintendo Wii Secure Digital Host Controller Interface"
#define DRV_AUTHOR      "Albert Herranz"

static char sdhci_hlwd_driver_version[] = "0.1i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

#define DBG(fmt, arg...)	drv_printk(KERN_DEBUG, "%s: " fmt, \
					   __func__, ## arg)


struct sdhci_hlwd_data {
	unsigned int quirks;
	struct sdhci_ops ops;
};

struct sdhci_hlwd_host {
	u16 xfer_mode_shadow;
};

static u32 sdhci_hlwd_readl(struct sdhci_host *host, int reg)
{
	return in_be32(host->ioaddr + reg);
}

static u16 sdhci_hlwd_readw(struct sdhci_host *host, int reg)
{
	return in_be16(host->ioaddr + (reg ^ 0x2));
}

static u8 sdhci_hlwd_readb(struct sdhci_host *host, int reg)
{
	return in_8(host->ioaddr + (reg ^ 0x3));
}

static void sdhci_hlwd_writel(struct sdhci_host *host, u32 val, int reg)
{
	out_be32(host->ioaddr + reg, val);
	udelay(5);
}

static void sdhci_hlwd_writew(struct sdhci_host *host, u16 val, int reg)
{
	struct sdhci_hlwd_host *hlwd_host = sdhci_priv(host);
	int base = reg & ~0x3;
	int shift = (reg & 0x2) * 8;

	switch (reg) {
	case SDHCI_TRANSFER_MODE:
		/*
		 * Postpone this write, we must do it together with a
		 * command write that is down below.
		 */
		hlwd_host->xfer_mode_shadow = val;
		return;
	case SDHCI_COMMAND:
		sdhci_hlwd_writel(host, val << 16 | hlwd_host->xfer_mode_shadow,
				  SDHCI_TRANSFER_MODE);
		return;
	}
	clrsetbits_be32(host->ioaddr + base,
			0xffff << shift, val << shift);
	udelay(5);
}

static void sdhci_hlwd_writeb(struct sdhci_host *host, u8 val, int reg)
{
	int base = reg & ~0x3;
	int shift = (reg & 0x3) * 8;

	clrsetbits_be32(host->ioaddr + base , 0xff << shift, val << shift);
	udelay(5);
}

static struct sdhci_hlwd_data sdhci_hlwd = {
	.quirks = SDHCI_QUIRK_32BIT_DMA_ADDR |
		  SDHCI_QUIRK_32BIT_DMA_SIZE,
	.ops = {
		.readl = sdhci_hlwd_readl,
		.readw = sdhci_hlwd_readw,
		.readb = sdhci_hlwd_readb,
		.writel = sdhci_hlwd_writel,
		.writew = sdhci_hlwd_writew,
		.writeb = sdhci_hlwd_writeb,
	},
};


#ifdef CONFIG_PM

static int sdhci_hlwd_suspend(struct of_device *ofdev, pm_message_t state)
{
	struct sdhci_host *host = dev_get_drvdata(&ofdev->dev);

	return mmc_suspend_host(host->mmc, state);
}

static int sdhci_hlwd_resume(struct of_device *ofdev)
{
	struct sdhci_host *host = dev_get_drvdata(&ofdev->dev);

	return mmc_resume_host(host->mmc);
}

#else

#define sdhci_hlwd_suspend NULL
#define sdhci_hlwd_resume NULL

#endif

static int __devinit sdhci_hlwd_probe(struct of_device *ofdev,
				      const struct of_device_id *match)
{
	struct device_node *np = ofdev->node;
	struct sdhci_hlwd_data *sdhci_hlwd_data = match->data;
	struct sdhci_host *host;
	struct sdhci_hlwd_host *hlwd_host;
	struct resource res;
	int error;

	if (starlet_get_ipc_flavour() != STARLET_IPC_MINI) {
		error = -ENODEV;
		goto out;
	}

	if (!of_device_is_available(np)) {
		error = -ENODEV;
		goto out;
	}

	host = sdhci_alloc_host(&ofdev->dev, sizeof(*hlwd_host));
	if (!host) {
		DBG("unable to allocate sdhci_host\n");
		error = -ENODEV;
		goto out;
	}

	dev_set_drvdata(&ofdev->dev, host);

	error = of_address_to_resource(np, 0, &res);
	if (error) {
		DBG("of_address_to_resource failed (%d)\n", error);
		goto err_no_address;
	}

	host->ioaddr = ioremap(res.start, resource_size(&res));
	if (!host->ioaddr) {
		DBG("ioremap failed\n");
		error = -EINVAL;
		goto err_ioremap;
	}

	host->irq = irq_of_parse_and_map(np, 0);
	if (!host->irq) {
		DBG("irq_of_parse_and_map failed\n");
		error = -EINVAL;
		goto err_no_irq;
	}

	host->hw_name = dev_name(&ofdev->dev);
	if (sdhci_hlwd_data) {
		host->quirks = sdhci_hlwd_data->quirks;
		host->ops = &sdhci_hlwd_data->ops;
	}

	error = sdhci_add_host(host);
	if (error) {
		DBG("sdhci_add_host failed\n");
		goto err_add_host;
	}

	return 0;

err_add_host:
	irq_dispose_mapping(host->irq);
err_no_irq:
	iounmap(host->ioaddr);
err_ioremap:
err_no_address:
	sdhci_free_host(host);
out:
	return error;
}

static int __devexit sdhci_hlwd_remove(struct of_device *ofdev)
{
	struct sdhci_host *host = dev_get_drvdata(&ofdev->dev);

	sdhci_remove_host(host, 0);
	irq_dispose_mapping(host->irq);
	iounmap(host->ioaddr);
	sdhci_free_host(host);
	return 0;
}

static const struct of_device_id sdhci_hlwd_match[] = {
	{ .compatible = "nintendo,hollywood-sdhci", .data = &sdhci_hlwd, },
	{},
};
MODULE_DEVICE_TABLE(of, sdhci_hlwd_match);

static struct of_platform_driver sdhci_hlwd_driver = {
	.driver.name = DRV_MODULE_NAME,
	.match_table = sdhci_hlwd_match,
	.probe = sdhci_hlwd_probe,
	.remove = __devexit_p(sdhci_hlwd_remove),
	.suspend = sdhci_hlwd_suspend,
	.resume	= sdhci_hlwd_resume,
};

static int __init sdhci_hlwd_init(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   sdhci_hlwd_driver_version);

	return of_register_platform_driver(&sdhci_hlwd_driver);
}
module_init(sdhci_hlwd_init);

static void __exit sdhci_hlwd_exit(void)
{
	of_unregister_platform_driver(&sdhci_hlwd_driver);
}
module_exit(sdhci_hlwd_exit);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

