/*
 * arch/powerpc/platforms/embedded6xx/gcn-rsw.c
 *
 * Nintendo GameCube/Wii reset switch (RSW) driver.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Stefan Esser
 * Copyright (C) 2004,2005,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of_platform.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/kexec.h>

/* for flipper hardware registers */
#include "flipper-pic.h"

#define DRV_MODULE_NAME      "gcn-rsw"
#define DRV_DESCRIPTION      "Nintendo GameCube/Wii Reset SWitch (RSW) driver"
#define DRV_AUTHOR           "Stefan Esser <se@nopiracy.de>, " \
			     "Albert Herranz"

static char rsw_driver_version[] = "1.0i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)


#define RSW_NORMAL_TIMEOUT      3	/* seconds */
#define RSW_EMERGENCY_PUSHES   10

enum rsw_state {
	IDLE = 0,		/* nothing to do */
	NORMAL_RESET,		/* reboot requested */
	EMERGENCY_RESET,	/* try emergency reboot */
};

struct rsw_drvdata {
	enum rsw_state state;
	struct timer_list timer;
	unsigned long jiffies;
	int pushes;
	int timeout;
	spinlock_t lock;

	void __iomem *io_base;
	unsigned int irq;

	struct device *dev;
};


/*
 * Tells if the reset button is pressed.
 */
static int rsw_is_button_pressed(void __iomem *io_base)
{
	u32 icr = in_be32(io_base + FLIPPER_ICR);

	drv_printk(KERN_INFO, "%x\n", icr);
	return !(icr & FLIPPER_ICR_RSS);
}

/*
 * Invokes a normal system restart.
 */
static void rsw_normal_restart(unsigned long dummy)
{
	ctrl_alt_del();
}

/*
 * Performs a low level system restart.
 */
static void rsw_emergency_restart(void)
{
#ifdef CONFIG_KEXEC
	struct kimage *image;
	image = xchg(&kexec_image, 0);
	if (image)
		machine_kexec(image);
#endif
	machine_restart(NULL);
}

/*
 * Handles the interrupt associated to the reset button.
 */
static irqreturn_t rsw_handler(int irq, void *data)
{
	struct rsw_drvdata *drvdata = (struct rsw_drvdata *)data;
	unsigned long flags;

	if (!rsw_is_button_pressed(drvdata->io_base)) {
		/* nothing to do */
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&drvdata->lock, flags);

	/* someone pushed the reset button */
	switch (drvdata->state) {
	case IDLE:
		drvdata->state = NORMAL_RESET;
		printk(KERN_EMERG "Rebooting in %d seconds...\n",
		       drvdata->timeout);
		printk(KERN_WARNING
		       "Push the Reset button again to cancel reboot!\n");

		/* schedule a reboot in a few seconds */
		init_timer(&drvdata->timer);
		drvdata->timer.expires = jiffies + drvdata->timeout * HZ;
		drvdata->timer.function =
		    (void (*)(unsigned long))rsw_normal_restart;
		add_timer(&drvdata->timer);
		drvdata->jiffies = jiffies;
		break;
	case NORMAL_RESET:
		if (time_before(jiffies,
				drvdata->jiffies + drvdata->timeout * HZ)) {
			/* the reset button was hit again before deadline */
			del_timer(&drvdata->timer);
			drvdata->state = IDLE;
			printk(KERN_EMERG "Reboot cancelled!\n");
		} else {
			/*
			 * Time expired. System should be now restarting.
			 * Go to emergency mode in case something goes bad.
			 */
			drvdata->state = EMERGENCY_RESET;
			drvdata->pushes = 0;
			printk(KERN_WARNING
			       "SWITCHED TO EMERGENCY RESET MODE!\n"
			       "Push %d times the Reset button to force"
			       " a hard reset!\n"
			       "NOTE THAT THIS COULD CAUSE DATA LOSS!\n",
			       RSW_EMERGENCY_PUSHES);
		}
		break;
	case EMERGENCY_RESET:
		/* force a hard reset if the user insists ... */
		if (++drvdata->pushes >= RSW_EMERGENCY_PUSHES) {
			spin_unlock_irqrestore(&drvdata->lock, flags);
			rsw_emergency_restart();
			return IRQ_HANDLED;
		} else {
			printk(KERN_INFO "%d/%d\n", drvdata->pushes,
			       RSW_EMERGENCY_PUSHES);
		}
		break;
	}

	spin_unlock_irqrestore(&drvdata->lock, flags);

	return IRQ_HANDLED;
}

/*
 * Setup routines.
 *
 */

static int rsw_init(struct rsw_drvdata *drvdata, struct resource *mem, int irq)
{
	int retval;

	drvdata->io_base = ioremap(mem->start, mem->end - mem->start + 1);
	drvdata->irq = irq;

	spin_lock_init(&drvdata->lock);
	drvdata->state = IDLE;
	drvdata->timeout = RSW_NORMAL_TIMEOUT;

	retval = request_irq(drvdata->irq, rsw_handler, 0,
			     DRV_MODULE_NAME, drvdata);
	if (retval) {
		drv_printk(KERN_ERR, "request of IRQ %d failed\n",
			   drvdata->irq);
	}
	return retval;
}

static void rsw_exit(struct rsw_drvdata *drvdata)
{
	free_irq(drvdata->irq, drvdata);
	if (drvdata->io_base) {
		iounmap(drvdata->io_base);
		drvdata->io_base = NULL;
	}
}

/*
 * Driver model helper routines.
 *
 */

static int rsw_do_probe(struct device *dev, struct resource *mem, int irq)
{
	struct rsw_drvdata *drvdata;
	int retval;

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		drv_printk(KERN_ERR, "failed to allocate rsw_drvdata\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, drvdata);
	drvdata->dev = dev;

	retval = rsw_init(drvdata, mem, irq);
	if (retval) {
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
	}
	return retval;
}

static int rsw_do_remove(struct device *dev)
{
	struct rsw_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata) {
		rsw_exit(drvdata);
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
		return 0;
	}
	return -ENODEV;
}

/*
 * OF platform driver hooks.
 *
 */

static int __init rsw_of_probe(struct of_device *odev,
			       const struct of_device_id *match)
{
	struct resource mem;
	int retval;

	retval = of_address_to_resource(odev->node, 0, &mem);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return -ENODEV;
	}

	return rsw_do_probe(&odev->dev,
			    &mem, irq_of_parse_and_map(odev->node, 0));
}

static int __exit rsw_of_remove(struct of_device *odev)
{
	return rsw_do_remove(&odev->dev);
}

static struct of_device_id rsw_of_match[] = {
	{.compatible = "nintendo,flipper-resetswitch"},
	{.compatible = "nintendo,hollywood-resetswitch"},
	{},
};

MODULE_DEVICE_TABLE(of, rsw_of_match);

static struct of_platform_driver rsw_of_driver = {
	.owner = THIS_MODULE,
	.name = DRV_MODULE_NAME,
	.match_table = rsw_of_match,
	.probe = rsw_of_probe,
	.remove = rsw_of_remove,
};

/*
 * Kernel module hooks.
 *
 */

static int __init rsw_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   rsw_driver_version);

	return of_register_platform_driver(&rsw_of_driver);
}

static void __exit rsw_exit_module(void)
{
	of_unregister_platform_driver(&rsw_of_driver);
}

module_init(rsw_init_module);
module_exit(rsw_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
