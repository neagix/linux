/*
 * drivers/exi/exi-driver.c
 *
 * Nintendo GameCube EXternal Interface (EXI) driver model routines.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Arthur Othieno <a.othieno@bluewin.ch>
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2005,2006,2007,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/delay.h>
#include <linux/exi.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/dma-mapping.h>

#define DRV_MODULE_NAME	"exi"
#define DRV_DESCRIPTION	"Nintendo GameCube/Wii EXternal Interface (EXI) driver"
#define DRV_AUTHOR	"Arthur Othieno <a.othieno@bluewin.ch>, " \
			"Todd Jeffreys <todd@voidpointer.org>, " \
			"Albert Herranz"

static char exi_driver_version[] = "4.0i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)


struct exi_map_id_to_name {
	unsigned int id;
	char *name;
};


static void exi_bus_device_release(struct device *dev);
static int exi_bus_match(struct device *dev, struct device_driver *drv);


static struct bus_type exi_bus_type = {
	.name = "exi",
	.match = exi_bus_match,
};
EXPORT_SYMBOL(exi_bus_type);

static struct device exi_bus_devices[EXI_MAX_CHANNELS] = {
	[0] = {
		.init_name = "exi0",
		.release = exi_bus_device_release,
	},
	[1] = {
		.init_name = "exi1",
		.release = exi_bus_device_release,
	},
	[2] = {
		.init_name = "exi2",
		.release = exi_bus_device_release,
	},
};

static struct exi_device exi_devices[EXI_MAX_CHANNELS][EXI_DEVICES_PER_CHANNEL];

static struct exi_map_id_to_name exi_map_id_to_name[] = {
	{ .id = EXI_ID_NONE, .name = "(external card)" },
	{ .id = 0xffff1698,  .name = "GameCube Mask ROM/RTC/SRAM/UART" },
	{ .id = 0xfffff308,  .name = "Wii Mask ROM/RTC/SRAM/UART" },
	{ .id = 0x00000004,  .name = "Memory Card 59" },
	{ .id = 0x00000008,  .name = "Memory Card 123" },
	{ .id = 0x00000010,  .name = "Memory Card 251" },
	{ .id = 0x00000020,  .name = "Memory Card 507" },
	{ .id = 0x00000040,  .name = "Memory Card 1019" },
	{ .id = 0x00000080,  .name = "Memory Card 2043" },
	{ .id = 0x01010000,  .name = "USB Adapter" },
	{ .id = 0x01020000,  .name = "NPDP GDEV" },
	{ .id = 0x02020000,  .name = "Modem" },
	{ .id = 0x03010000,  .name = "Marlin?" },
	{ .id = 0x04020200,  .name = "BroadBand Adapter (DOL-015)" },
	{ .id = 0x04120000,  .name = "AD16" },
	{ .id = 0x05070000,  .name = "IS Viewer" },
	{ .id = 0x0a000000,  .name = "Microphone (DOL-022)" },
	{ .id = 0 }
};

/*
 * Internal. Return the friendly name of an exi identifier.
 */
static const char *exi_name_id(unsigned int id)
{
	struct exi_map_id_to_name *map = exi_map_id_to_name;

	while (map->id) {
		if (map->id == id)
			return map->name;
		map++;
	}
	return "Unknown";
}

/*
 * Internal. Check if an exi device matches a given exi device id.
 */
static int exi_device_match_one(const struct exi_device_id *eid,
				const struct exi_device *exi_device)
{
	/*
	 * We allow drivers to claim devices that do not provide
	 * EXI identifiers by matching directly on channel/device.
	 * These drivers must use EXI_ID_NONE on their eids.
	 */
	if (eid->id == exi_device->eid.id || eid->id == EXI_ID_NONE) {
		/* match against channel and device */
		if (exi_device->eid.channel == eid->channel &&
		    exi_device->eid.device == eid->device) {
			return 1;
		}
	}
	return 0;
}

/*
 * Internal. Check if an exi device matches a given set of exi device ids.
 * Return the exi device identifier or %NULL if there is no match.
 */
static const struct exi_device_id *
exi_device_match(const struct exi_device_id *eids,
		 const struct exi_device *exi_device)
{
	while (eids && eids->id) {
		if (exi_device_match_one(eids, exi_device))
			return eids;
		eids++;
	}
	return NULL;
}

/*
 * Internal. Used to check if an exi device is supported by an exi driver.
 */
static int exi_bus_match(struct device *dev, struct device_driver *drv)
{
	struct exi_device *exi_device = to_exi_device(dev);
	struct exi_driver *exi_driver = to_exi_driver(drv);
	const struct exi_device_id *eids = exi_driver->eid_table;

	if (eids && exi_device_match(eids, exi_device))
		return 1;
	return 0;
}

/*
 * Internal. Bus device release.
 */
static void exi_bus_device_release(struct device *dev)
{
	drv_printk(KERN_WARNING, "exi_bus_device_release called!\n");
}

static void exi_device_release(struct device *dev);

/*
 * Internal. Initialize an exi_device structure.
 */
static void exi_device_init(struct exi_device *exi_device,
			    unsigned int channel, unsigned int device)
{
	memset(exi_device, 0, sizeof(*exi_device));

	exi_device->eid.id = EXI_ID_INVALID;
	exi_device->eid.channel = channel;
	exi_device->eid.device = device;
	exi_device->frequency = EXI_FREQ_SCAN;
	exi_device->exi_channel = to_exi_channel(channel);

	exi_device->dev.parent = &exi_bus_devices[channel];
	exi_device->dev.bus = &exi_bus_type;
	dev_set_name(&exi_device->dev, "exi%01x:%01x", channel, device);
	exi_device->dev.platform_data = to_exi_channel(channel);
	set_dma_ops(&exi_device->dev, &dma_direct_ops);
	exi_device->dev.release = exi_device_release;
}

/*
 * Internal. Device release.
 */
static void exi_device_release(struct device *dev)
{
	struct exi_device *exi_device = to_exi_device(dev);
	unsigned int channel, device;

	channel = exi_device->eid.channel;
	device = exi_device->eid.device;

	exi_device_init(exi_device, channel, device);
}

/**
 *	exi_device_get - Increments the reference count of the exi device
 *	@exi_device:	device being referenced
 *
 *	Each live reference to an exi device should be refcounted.
 *	A pointer to the device with the incremented reference counter
 *	is returned.
 */
struct exi_device *exi_device_get(struct exi_device *exi_device)
{
	if (exi_device)
		get_device(&exi_device->dev);
	return exi_device;
}
EXPORT_SYMBOL(exi_device_get);

/**
 *	exi_device_put  -  Releases a use of the exi device
 *	@exi_device:	device that's been disconnected
 *
 *	Must be called when a user of a device is finished with it.
 */
void exi_device_put(struct exi_device *exi_device)
{
	if (exi_device)
		put_device(&exi_device->dev);
}
EXPORT_SYMBOL(exi_device_put);

/**
 *	exi_get_exi_device  -  Returns a reference to an exi device
 *	@exi_channel:	exi channel where the device is located
 *	@device:	device number within the channel
 */
struct exi_device *exi_get_exi_device(struct exi_channel *exi_channel,
				      int device)
{
	/* REVISIT, take a ref here? */
	return &exi_devices[to_channel(exi_channel)][device];
}
EXPORT_SYMBOL(exi_get_exi_device);

/*
 * Internal. Call device driver probe function on match.
 */
static int exi_device_probe(struct device *dev)
{
	struct exi_device *exi_device = to_exi_device(dev);
	struct exi_driver *exi_driver = to_exi_driver(dev->driver);
	const struct exi_device_id *eid;
	int retval = -ENODEV;

	if (!exi_driver->eid_table)
		goto out;

	eid = exi_device_match(exi_driver->eid_table, exi_device);
	if (eid) {
		exi_device->frequency = exi_driver->frequency;
		if (exi_driver->probe)
			retval = exi_driver->probe(exi_device);
	}
	if (retval >= 0)
		retval = 0;

out:
	return retval;
}

/*
 * Internal. Call device driver remove function.
 */
static int exi_device_remove(struct device *dev)
{
	struct exi_device *exi_device = to_exi_device(dev);
	struct exi_driver *exi_driver = to_exi_driver(dev->driver);

	if (exi_driver->remove)
		exi_driver->remove(exi_device);

	return 0;
}


/**
 *      exi_driver_register - register an EXI device driver.
 *      @driver: driver structure to register.
 *
 *      Registers an EXI device driver with the bus
 *      and consequently with the driver model core.
 */
int exi_driver_register(struct exi_driver *driver)
{
	driver->driver.name = driver->name;
	driver->driver.bus = &exi_bus_type;
	driver->driver.probe = exi_device_probe;
	driver->driver.remove = exi_device_remove;

	return driver_register(&driver->driver);
}
EXPORT_SYMBOL(exi_driver_register);

/**
 *      exi_driver_unregister - unregister an EXI device driver.
 *      @driver: driver structure to unregister.
 *
 *      Unregisters an EXI device driver with the bus
 *      and consequently with the driver model core.
 */
void exi_driver_unregister(struct exi_driver *driver)
{
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL(exi_driver_unregister);


/*
 * Internal. Re-scan a given device.
 */
static void exi_device_rescan(struct exi_device *exi_device)
{
	unsigned int id;
	int error;

	/* now ID the device */
	id = exi_get_id(exi_device);

	if (exi_device->eid.id != EXI_ID_INVALID) {
		/* device removed or changed */
		drv_printk(KERN_INFO, "about to remove [%s] id=0x%08x %s\n",
			   dev_name(&exi_device->dev),
			   exi_device->eid.id,
			   exi_name_id(exi_device->eid.id));
		device_unregister(&exi_device->dev);
		drv_printk(KERN_INFO, "remove completed\n");
		exi_device->eid.id = EXI_ID_INVALID;
	}

	if (id != EXI_ID_INVALID) {
		/* a new device has been found */
		drv_printk(KERN_INFO, "about to add [%s] id=0x%08x %s\n",
			   dev_name(&exi_device->dev),
			   id, exi_name_id(id));
		exi_device->eid.id = id;
		error = device_register(&exi_device->dev);
		if (error) {
			drv_printk(KERN_INFO, "add failed (%d)\n", error);
			exi_device->eid.id = EXI_ID_INVALID;
		} else
			drv_printk(KERN_INFO, "add completed\n");
	}

	exi_update_ext_status(exi_get_exi_channel(exi_device));
}

/*
 * Internal. Re-scan a given exi channel, looking for added, changed and
 * removed exi devices.
 */
static void exi_channel_rescan(struct exi_channel *exi_channel)
{
	struct exi_device *exi_device;
	unsigned int channel, device;

	/* add the exi devices underneath the parents */
	for (device = 0; device < EXI_DEVICES_PER_CHANNEL; ++device) {
		channel = to_channel(exi_channel);
		exi_device = &exi_devices[channel][device];
		exi_device_rescan(exi_device);
	}
}

/*
 * Internal. Scans all the exi channels looking for exi devices.
 */
static void exi_bus_rescan(void)
{
	struct exi_channel *exi_channel;
	unsigned int channel;

	for (channel = 0; channel < EXI_MAX_CHANNELS; ++channel) {
		exi_channel = to_exi_channel(channel);
		exi_channel_rescan(exi_channel);
	}
}


static struct task_struct *exi_bus_task;
wait_queue_head_t exi_bus_waitq;

/*
 * Internal. Looks for new, changed or removed devices.
 */
static int exi_bus_thread(void *__unused)
{
	struct exi_channel *exi_channel;
	struct exi_device *exi_device;
	unsigned int channel;
	int is_loaded, was_loaded;

	while (!kthread_should_stop()) {
		/* scan the memcard slot channels for device changes */
		for (channel = 0; channel <= 1; ++channel) {
			exi_channel = to_exi_channel(channel);

			is_loaded = exi_get_ext_line(exi_channel);
			was_loaded = (exi_channel->flags & EXI_EXT) ? 1 : 0;

			if (is_loaded ^ was_loaded) {
				exi_device = &exi_devices[channel][0];
				exi_device_rescan(exi_device);
			}
		}

		sleep_on_timeout(&exi_bus_waitq, HZ);
	}

	return 0;
}

/**
 * exi_quiesce() - quiesce EXI hardware
 *
 * Put the EXI hardware into a calm state.
 */
void exi_quiesce(void)
{
	exi_hw_quiesce();
}

/*
 *
 */
static int exi_init(struct resource *mem, unsigned int irq)
{
	struct exi_channel *exi_channel;
	struct exi_device *exi_device;
	unsigned int channel, device;
	int retval;

	retval = exi_hw_init(DRV_MODULE_NAME, mem, irq);
	if (retval)
		goto err_hw_init;

	/* register root devices */
	for (channel = 0; channel < EXI_MAX_CHANNELS; ++channel) {
		retval = device_register(&exi_bus_devices[channel]);
		if (retval)
			goto err_device_register;
	}

	/* initialize devices */
	for (channel = 0; channel < EXI_MAX_CHANNELS; ++channel) {
		exi_channel = to_exi_channel(channel);
		for (device = 0; device < EXI_DEVICES_PER_CHANNEL; ++device) {
			exi_device = &exi_devices[channel][device];
			exi_device_init(exi_device, channel, device);
		}
	}

	/* register the bus */
	retval = bus_register(&exi_bus_type);
	if (retval)
		goto err_bus_register;

	/* now enumerate through the bus and add all detected devices */
	exi_bus_rescan();

	/* setup a thread to manage plugable devices */
	init_waitqueue_head(&exi_bus_waitq);
	exi_bus_task = kthread_run(exi_bus_thread, NULL, "kexid");
	if (IS_ERR(exi_bus_task))
		drv_printk(KERN_WARNING, "failed to start exi kernel thread\n");

	return 0;

err_bus_register:
err_device_register:
	while (--channel > 0)
		device_unregister(&exi_bus_devices[channel]);
	exi_hw_exit(mem, irq);
err_hw_init:
	return retval;
}

/*
 *
 */
static int __init exi_layer_init(void)
{
	struct device_node *np;
	struct resource res;
	int retval;

	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   exi_driver_version);

	np = of_find_compatible_node(NULL, NULL, "nintendo,flipper-exi");
	if (!np) {
		np = of_find_compatible_node(NULL, NULL,
					     "nintendo,hollywood-exi");
		if (!np)
			return -ENODEV;
	}

	retval = of_address_to_resource(np, 0, &res);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return -ENOMEM;
	}

	retval = exi_init(&res, irq_of_parse_and_map(np, 0));
	of_node_put(np);

	return retval;
}
postcore_initcall(exi_layer_init);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

