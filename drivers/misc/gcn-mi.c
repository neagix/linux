/*
 * drivers/misc/gcn-mi.c
 *
 * Nintendo GameCube Memory Interface (MI) driver.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004,2005,2007,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/io.h>

#include "gcn-mi.h"


#define MI_IRQ	7

#define MI_BASE			0xcc004000
#define MI_SIZE			0x80

#define MI_PROT_REGION0		((u32 __iomem *)(MI_BASE+0x00))
#define MI_PROT_REGION1		((u32 __iomem *)(MI_BASE+0x04))
#define MI_PROT_REGION2		((u32 __iomem *)(MI_BASE+0x08))
#define MI_PROT_REGION3		((u32 __iomem *)(MI_BASE+0x0c))

#define MI_PROT_TYPE		((u16 __iomem *)(MI_BASE+0x10))

#define MI_IMR			((u16 __iomem *)(MI_BASE+0x1c))
#define MI_ICR			((u16 __iomem *)(MI_BASE+0x1e))

#define MI_0x4020		((u16 __iomem *)(MI_BASE+0x20))

#define MI_ADDRLO		((u16 __iomem *)(MI_BASE+0x22))
#define MI_ADDRHI		((u16 __iomem *)(MI_BASE+0x24))

#define MI_PAGE_SHIFT	10
#define MI_PAGE_MASK	(~((1 << MI_PAGE_SHIFT) - 1))
#define MI_PAGE_SIZE	(1UL << MI_PAGE_SHIFT)

struct mi_private {
	struct device *device;
	int	irq;
	int	nr_regions;
	int	regions_bitmap;
	unsigned long faults[MI_MAX_REGIONS+1];
	unsigned long last_address;
	unsigned long last_address_faults;
	spinlock_t lock;
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry *proc_file;
#endif
};

static struct mi_private *mi_private;

#define DRV_MODULE_NAME      "gcn-mi"
#define DRV_DESCRIPTION      "Nintendo GameCube Memory Interface driver"
#define DRV_AUTHOR           "Albert Herranz"

#define PFX DRV_MODULE_NAME ": "
#define mi_printk(level, format, arg...) \
	printk(level PFX format , ## arg)

/*
 *
 */
static irqreturn_t mi_handler(int this_irq, void *data)
{
	struct mi_private *priv = (struct mi_private *)data;
	unsigned long flags;
	int region, cause, ack;
	unsigned long address;

	spin_lock_irqsave(&priv->lock, flags);

	address = in_be16(MI_ADDRLO) | (in_be16(MI_ADDRHI)<<16);

	ack = 0;
	cause = in_be16(MI_ICR);

	/* a fault was detected in some of the registered regions */
	if ((cause & 0xf) != 0) {
		for (region = 0; region < MI_MAX_REGIONS; region++) {
			if ((cause & (1 << region)) != 0) {
				priv->faults[region]++;
				mi_printk(KERN_INFO, "bad access on region #%d"
					  " at 0x%lx\n", region, address);
			}
		}
	}

	/* a fault was detected out of any registered region */
	if ((cause & (1 << 4)) != 0) {
		priv->faults[MI_MAX_REGIONS]++;
		if (address == priv->last_address) {
			priv->last_address_faults++;
		} else {
			if (priv->last_address_faults > 0) {
#if 0
				mi_printk(KERN_INFO, "bad access"
					  " at 0x%lx (%lu times)\n",
					  priv->last_address,
					  priv->last_address_faults);
#endif
			}
			priv->last_address = address;
			priv->last_address_faults = 1;
		}
	}
	ack |= cause;
	out_be16(MI_ICR, ack); /* ack int */
	out_be16(MI_0x4020, 0); /* kind of ack */

	spin_unlock_irqrestore(&priv->lock, flags);

	return IRQ_HANDLED;
}

#ifdef CONFIG_PROC_FS
/*
 *
 */
static int mi_proc_read(char *page, char **start,
			    off_t off, int count,
			    int *eof, void *data)
{
	struct mi_private *priv = (struct mi_private *)data;
	int len;
	int region;

	len = sprintf(page, "# <region> <faults>\n");
	for (region = 0; region < MI_MAX_REGIONS; region++) {
		if ((priv->regions_bitmap & (1<<region)) != 0) {
			len += sprintf(page+len, "%d\t%lu\n",
				       region, priv->faults[region]);
		}
	}
	len += sprintf(page+len, "%s\t%lu\n",
		       "none", priv->faults[MI_MAX_REGIONS]);

	return len;
}

#endif /* CONFIG_PROC_FS */

/*
 *
 */
static int mi_setup_irq(struct mi_private *priv)
{
	int retval;

	retval = request_irq(priv->irq, mi_handler, 0, DRV_MODULE_NAME, priv);
	if (retval)
		mi_printk(KERN_ERR, "request of irq%d failed\n", priv->irq);
	else
		out_be16(MI_IMR, (1<<4)); /* do not mask all MI interrupts */

	return retval;
}

/*
 *
 */
static int mi_probe(struct device *device, struct resource *mem, int irq)
{
	struct mi_private *priv;
	int retval;

	priv = kmalloc(sizeof(struct mi_private), GFP_KERNEL);
	if (!priv) {
		retval = -ENOMEM;
		goto err;
	}

	memset(priv, 0, sizeof(*priv));
	/*
	int region;
	priv->nr_regions = 0;
	priv->regions_bitmap = 0;
	for( region = 0; region < MI_MAX_REGIONS; region++ ) {
		priv->faults[region] = 0;
	}
	priv->last_address_faults = 0;
	*/
	spin_lock_init(&priv->lock);

	priv->device = device;
	dev_set_drvdata(priv->device, priv);

	priv->irq = irq;
	retval = mi_setup_irq(priv);
	if (retval)
		goto err_setup_irq;

#ifdef CONFIG_PROC_FS
	{
	struct platform_device *pdev = to_platform_device(device);
	priv->proc_file = create_proc_read_entry(dev_name(&pdev->dev),
						 0444, NULL,
						 mi_proc_read, priv);
	}
#endif /* CONFIG_PROC_FS */

	mi_private = priv;

	return 0;

err_setup_irq:
	dev_set_drvdata(priv->device, NULL);
	kfree(priv);
err:
	return retval;
}

/*
 *
 */
static void mi_shutdown(struct mi_private *priv)
{
	gcn_mi_region_unprotect_all();
}

/*
 *
 */
static void mi_remove(struct mi_private *priv)
{
#ifdef CONFIG_PROC_FS
	struct platform_device *pdev = to_platform_device(priv->device);
	remove_proc_entry(dev_name(&pdev->dev), NULL);
#endif /* CONFIG_PROC_FS */

	mi_shutdown(priv);

	/* free interrupt handler */
	free_irq(priv->irq, priv);

	kfree(priv);
	mi_private = NULL;
}

/*
 *
 */
static int __init mi_drv_probe(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct resource *mem;
	int irq;

	irq = platform_get_irq(pdev, 0);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	mi_printk(KERN_INFO, "%s\n", DRV_DESCRIPTION);

	return mi_probe(device, mem, irq);
}

/*
 *
 */
static int mi_drv_remove(struct device *device)
{
	struct mi_private *priv = dev_get_drvdata(device);

	if (priv) {
		mi_remove(priv);
		dev_set_drvdata(device, NULL);
	}

	return 0;
}

/*
 *
 */
static void mi_drv_shutdown(struct device *device)
{
	struct mi_private *priv = dev_get_drvdata(device);

	if (priv)
		mi_shutdown(priv);
}

static struct device_driver mi_device_driver = {
	.name = "mi",
	.bus = &platform_bus_type,
	.probe = mi_drv_probe,
	.remove = mi_drv_remove,
	.shutdown = mi_drv_shutdown,
};

static struct resource mi_resources[] = {
	[0] = {
		.start = MI_BASE,
		.end = MI_BASE + MI_SIZE - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = MI_IRQ,
		.end = MI_IRQ,
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device mi_device = {
	.name = "mi",
	.id = 0,
	.num_resources = ARRAY_SIZE(mi_resources),
	.resource = mi_resources,
};

/*
 *
 */
static int __init mi_init(void)
{
	int retval = 0;

	retval = driver_register(&mi_device_driver);
	if (!retval)
		retval = platform_device_register(&mi_device);

	return retval;
}

/*
 *
 */
static void __exit mi_exit(void)
{
	platform_device_unregister(&mi_device);
	driver_unregister(&mi_device_driver);
}

module_init(mi_init);
module_exit(mi_exit);


/* public interface */

/*
 *
 */
int gcn_mi_region_protect(unsigned long physlo, unsigned long physhi, int type)
{
	struct mi_private *priv = mi_private;
	int region, free_regions;
	u16 pagelo, pagehi;

	if (!priv)
		return -ENODEV;

	if (type < MI_PROT_NONE || type > MI_PROT_RW)
		return -EINVAL;

	if ((physlo & ~MI_PAGE_MASK) != 0 || (physhi & ~MI_PAGE_MASK) != 0)
		return -EINVAL;

	free_regions = MI_MAX_REGIONS - priv->nr_regions;
	if (free_regions <= 0)
		return -ENOMEM;
	for (region = 0; region < MI_MAX_REGIONS; region++) {
		if ((priv->regions_bitmap & (1<<region)) == 0)
			break;
	}
	if (region >= MI_MAX_REGIONS)
		return -ENOMEM;
	priv->regions_bitmap |= (1 << region);
	priv->nr_regions++;

	out_be16(MI_PROT_TYPE,
		(in_be16(MI_PROT_TYPE) & ~(3 << 2*region))|(type << 2*region));
	pagelo = physlo >> MI_PAGE_SHIFT;
	pagehi = (physhi >> MI_PAGE_SHIFT) - 1;
	out_be32(MI_PROT_REGION0 + 4*region, (pagelo << 16) | pagehi);
	out_be16(MI_IMR, in_be16(MI_IMR) | (1 << region));

	mi_printk(KERN_INFO, "protected region #%d"
		  " from 0x%0lx to 0x%0lx with 0x%0x\n", region,
		  (unsigned long)(pagelo << MI_PAGE_SHIFT),
		  (unsigned long)(((pagehi+1) << MI_PAGE_SHIFT) - 1),
		  type);

	return region;
}

/*
 *
 */
int gcn_mi_region_unprotect(int region)
{
	struct mi_private *priv = mi_private;

	if (!priv)
		return -ENODEV;

	if (region < 0 || region > MI_MAX_REGIONS)
		return -EINVAL;

	out_be16(MI_IMR, in_be16(MI_IMR) & ~(1 << region));
	out_be32(MI_PROT_REGION0 + 4*region, 0);
	out_be16(MI_PROT_TYPE,
		 in_be16(MI_PROT_TYPE) | (MI_PROT_RW << 2*region));

	if ((priv->regions_bitmap & (1<<region)) != 0)
		mi_printk(KERN_INFO, "region #%d unprotected\n", region);

	priv->regions_bitmap &= ~(1 << region);
	priv->nr_regions--;

	return 0;
}

/*
 *
 */
void gcn_mi_region_unprotect_all(void)
{
	int region;

	out_be16(MI_IMR, 0);
	for (region = 0; region < MI_MAX_REGIONS; region++)
		gcn_mi_region_unprotect(region);
}

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
