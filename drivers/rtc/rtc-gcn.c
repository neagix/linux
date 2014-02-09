/*
 * drivers/rtc/rtc-gcn.c
 *
 * Nintendo GameCube/Wii RTC/SRAM driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2005,2008,2009 Albert Herranz
 *
 * Based on gamecube_time.c from Torben Nielsen.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/exi.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/rtc.h>
#include <linux/time.h>
#include <asm/machdep.h>

#define DRV_MODULE_NAME   "rtc-gcn"
#define DRV_DESCRIPTION   "Nintendo GameCube/Wii RTC/SRAM driver"
#define DRV_AUTHOR        "Torben Nielsen, " \
			  "Albert Herranz"

static char gcnrtc_driver_version[] = "1.0i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)


#define RTC_EXI_GCN_ID	0xffff1698
#define RTC_EXI_RVL_ID	0xfffff308

#define RTC_EXI_CHANNEL	0
#define RTC_EXI_DEVICE	1
#define RTC_EXI_FREQ	3 /* 8MHz */

#define RTC_OFFSET	946684800L


struct gcn_sram {
	u16     csum1;
	u16     csum2;
	u32     ead0;
	u32     ead1;
	int     bias;
	s8      horz_display_offset;
	u8      ntd;
	u8      language;
	u8      flags;
	u8      reserved[44];
};

struct gcnrtc_drvdata {
	spinlock_t		lock;
	struct exi_device	*dev;

	struct rtc_device	*rtc_dev;

	struct gcn_sram         sram;
};

static struct gcnrtc_drvdata gcnrtc_drvdata;

/*
 * Hardware interfaces.
 *
 */

/*
 * Loads the SRAM contents.
 * Context: user.
 */
static void sram_load(struct exi_device *dev)
{
	struct gcnrtc_drvdata *drvdata = exi_get_drvdata(dev);
	struct gcn_sram *sram = &drvdata->sram;
	u32 req;

	exi_dev_take(dev);

	/* select the SRAM device */
	exi_dev_select(dev);

	/* send the appropriate command */
	req = 0x20000100;
	exi_dev_write(dev, &req, sizeof(req));

	/* read the SRAM data */
	exi_dev_read(dev, sram, sizeof(*sram));

	/* deselect the SRAM device */
	exi_dev_deselect(dev);

	exi_dev_give(dev);

	return;
}

/*
 * Gets the hardware clock date and time.
 * Context: user.
 */
static unsigned long gcnrtc_read_time(struct exi_device *dev)
{
	unsigned long a = 0;

	exi_dev_take(dev);

	/* select the SRAM device */
	exi_dev_select(dev);

	/* send the appropriate command */
	a = 0x20000000;
	exi_dev_write(dev, &a, sizeof(a));

	/* read the time and date value */
	exi_dev_read(dev, &a, sizeof(a));

	/* deselect the RTC device */
	exi_dev_deselect(dev);

	exi_dev_give(dev);

	return a;
}

/*
 * Sets the hardware clock date and time to @aval.
 * Context: user, interrupt (adjtimex).
 */
static int gcnrtc_write_time(struct exi_device *dev, unsigned long aval)
{
	u32 req;
	int retval;

	/*
	 * We may get called from the timer interrupt. In that case,
	 * we could fail if the exi channel used to access the RTC
	 * is busy. If this happens, we just return an error. The timer
	 * interrupt code is prepared to deal with such case.
	 */

	retval = exi_dev_try_take(dev);
	if (!retval) {
		/* select the RTC device */
		exi_dev_select(dev);

		/* send the appropriate command */
		req = 0xa0000000;
		exi_dev_write(dev, &req, sizeof(req));

		/* set the new time and date value */
		exi_dev_write(dev, &aval, sizeof(aval));

		/* deselect the RTC device */
		exi_dev_deselect(dev);

		exi_dev_give(dev);
	}
	return retval;
}

/*
 * Platform time functions.
 *
 */

/*
 * Platform specific function to return the current date and time.
 */
static void gcnrtc_plat_rtc_get_time(struct rtc_time *t)
{
	struct gcnrtc_drvdata *drvdata = &gcnrtc_drvdata;
	unsigned long nowtime;

	if (!drvdata->dev)
		return;

	nowtime = gcnrtc_read_time(drvdata->dev) +
		  drvdata->sram.bias + RTC_OFFSET;
	rtc_time_to_tm(nowtime, t);
}

/*
 * Platform specific function to set the current date and time.
 *
 */
static int gcnrtc_plat_rtc_set_time(struct rtc_time *t)
{
	struct gcnrtc_drvdata *drvdata = &gcnrtc_drvdata;
	unsigned long nowtime;

	if (!drvdata->dev)
		return -ENODEV;

	rtc_tm_to_time(t, &nowtime);
	return gcnrtc_write_time(drvdata->dev,
				 nowtime - RTC_OFFSET - drvdata->sram.bias);
}

/*
 * RTC class driver.
 *
 */

/*
 *
 */
static int gcnrtc_rtc_read_time(struct device *dev, struct rtc_time *t)
{
	gcnrtc_plat_rtc_get_time(t);
	return 0;
}

/*
 *
 */
static int gcnrtc_rtc_set_time(struct device *dev, struct rtc_time *t)
{
	return gcnrtc_plat_rtc_set_time(t);
}

static const struct rtc_class_ops gcnrtc_ops = {
	.read_time      = gcnrtc_rtc_read_time,
	.set_time       = gcnrtc_rtc_set_time,
};


/*
 * EXI driver.
 *
 */

/*
 *
 */
static int gcnrtc_probe(struct exi_device *dev)
{
	struct gcnrtc_drvdata *drvdata = &gcnrtc_drvdata;
	unsigned long flags;
	int retval = -ENODEV;

	if (exi_device_get(dev)) {
		spin_lock_init(&drvdata->lock);

		exi_set_drvdata(dev, drvdata);
		drvdata->dev = dev;

		memset(&drvdata->sram, 0, sizeof(struct gcn_sram));
		sram_load(dev);

		spin_lock_irqsave(&drvdata->lock, flags);
		ppc_md.set_rtc_time = gcnrtc_plat_rtc_set_time;
		ppc_md.get_rtc_time = gcnrtc_plat_rtc_get_time;
		spin_unlock_irqrestore(&drvdata->lock, flags);

		drvdata->rtc_dev = rtc_device_register(DRV_MODULE_NAME,
						       &dev->dev,
						       &gcnrtc_ops,
						       THIS_MODULE);
		retval = 0;
	}

	return retval;
}

/*
 *
 */
static void gcnrtc_remove(struct exi_device *dev)
{
	struct gcnrtc_drvdata *drvdata = exi_get_drvdata(dev);
	unsigned long flags;

	if (drvdata) {
		spin_lock_irqsave(&drvdata->lock, flags);
		ppc_md.set_rtc_time = NULL;
		ppc_md.get_rtc_time = NULL;
		spin_unlock_irqrestore(&drvdata->lock, flags);

		if (!IS_ERR(drvdata->rtc_dev))
			rtc_device_unregister(drvdata->rtc_dev);
	}
	exi_device_put(dev);
}


static struct exi_device_id gcnrtc_eid_table[] = {
	{
		.channel = RTC_EXI_CHANNEL,
		.device  = RTC_EXI_DEVICE,
		.id      = RTC_EXI_GCN_ID
	},
	{
		.channel = RTC_EXI_CHANNEL,
		.device  = RTC_EXI_DEVICE,
		.id      = RTC_EXI_RVL_ID
	},
	{ },
};

static struct exi_driver gcnrtc_driver = {
	.name = DRV_MODULE_NAME,
	.eid_table = gcnrtc_eid_table,
	.frequency = RTC_EXI_FREQ,
	.probe = gcnrtc_probe,
	.remove = gcnrtc_remove,
};


/*
 *
 */
static int __init gcnrtc_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n",
		   DRV_DESCRIPTION, gcnrtc_driver_version);

	return exi_driver_register(&gcnrtc_driver);
}

/*
 *
 */
static void __exit gcnrtc_exit_module(void)
{
	exi_driver_unregister(&gcnrtc_driver);
}

module_init(gcnrtc_init_module);
module_exit(gcnrtc_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");
