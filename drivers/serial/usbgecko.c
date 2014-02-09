/*
 * drivers/serial/usbgecko.c
 *
 * Console and TTY driver for the USB Gecko adapter.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define UG_DEBUG

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include <linux/exi.h>

#define DRV_MODULE_NAME "usbgecko"
#define DRV_DESCRIPTION "Console and TTY driver for the USB Gecko adapter"
#define DRV_AUTHOR      "Albert Herranz"

static char ug_driver_version[] = "0.1i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

/*
 *
 * EXI related definitions.
 */
#define UG_SLOTA_CHANNEL	0	/* EXI0xxx */
#define UG_SLOTA_DEVICE		0	/* chip select, EXI0CSB0 */

#define UG_SLOTB_CHANNEL	1	/* EXI1xxx */
#define UG_SLOTB_DEVICE		0	/* chip select, EXI1CSB0 */

#define UG_SPI_CLK_IDX		EXI_CLK_32MHZ


struct ug_adapter {
	struct exi_device *exi_device;
	struct task_struct *poller;
	struct mutex mutex;
	int refcnt;
};

static struct ug_adapter ug_adapters[2];


/*
 *
 * Hardware interface.
 */

/*
 *
 */
static void ug_exi_io_transaction(struct exi_device *exi_device, u16 i, u16 *o)
{
	u16 data;

	exi_dev_select(exi_device);
	data = i;
	exi_dev_readwrite(exi_device, &data, 2);
	exi_dev_deselect(exi_device);
	*o = data;
}

#if 0
/*
 *
 */
static void ug_io_transaction(struct ug_adapter *adapter, u16 i, u16 *o)
{
	struct exi_device *exi_device = adapter->exi_device;

	if (exi_device)
		ug_exi_io_transaction(exi_device, i, o);
}
#endif

/*
 *
 */
static int ug_check_adapter(struct exi_device *exi_device)
{
	u16 data;

	exi_dev_take(exi_device);
	ug_exi_io_transaction(exi_device, 0x9000, &data);
	exi_dev_give(exi_device);

	return data == 0x0470;
}

#if 0
/*
 *
 */
static int ug_is_txfifo_empty(struct ug_adapter *adapter)
{
	struct exi_device *exi_device = adapter->exi_device;
	u16 data;

	if (!exi_device)
		return 0;

	if (!exi_dev_try_take(exi_device)) {
		ug_exi_io_transaction(exi_device, 0xC000, &data);
		exi_dev_give(exi_device);
		return data & 0x0400;
	}
	return 0;
}

/*
 *
 */
static int ug_is_rxfifo_empty(struct ug_adapter *adapter)
{
	struct exi_device *exi_device = adapter->exi_device;
	u16 data;

	if (!exi_device)
		return 0;

	if (!exi_dev_try_take(exi_device)) {
		ug_exi_io_transaction(exi_device, 0xD000, &data);
		exi_dev_give(exi_device);
		return data & 0x0400;
	}
	return 0;
}

/*
 *
 */
static int ug_putc(struct ug_adapter *adapter, char c)
{
	struct exi_device *exi_device = adapter->exi_device;
	u16 data;

	if (!exi_device)
		return 0;

	if (!exi_dev_try_take(exi_device)) {
		ug_exi_io_transaction(exi_device, 0xB000|(c<<4), &data);
		exi_dev_give(exi_device);
		return data & 0x0400;
	}
	return 0;
}

/*
 *
 */
static int ug_getc(struct ug_adapter *adapter, char *c)
{
	struct exi_device *exi_device = adapter->exi_device;
	u16 data;

	if (!exi_device)
		return 0;

	if (!exi_dev_try_take(exi_device)) {
		ug_exi_io_transaction(exi_device, 0xA000, &data);
		exi_dev_give(exi_device);
		if ((data & 0x0800)) {
			*c = data & 0xff;
			return 1;
		}
	}
	return 0;
}
#endif

/*
 *
 */
static int ug_safe_putc(struct ug_adapter *adapter, char c)
{
	struct exi_device *exi_device = adapter->exi_device;
	u16 data;

	if (!exi_device)
		return 0;

	if (!exi_dev_try_take(exi_device)) {
		ug_exi_io_transaction(exi_device, 0xC000, &data);
		if ((data & 0x0400))
			ug_exi_io_transaction(exi_device, 0xB000|(c<<4), &data);
		exi_dev_give(exi_device);
		return data & 0x0400;
	}
	return 0;
}

/*
 *
 */
static int ug_safe_getc(struct ug_adapter *adapter, char *c)
{
	struct exi_device *exi_device = adapter->exi_device;
	u16 data;

	if (!exi_device)
		return 0;

	if (!exi_dev_try_take(exi_device)) {
		ug_exi_io_transaction(exi_device, 0xD000, &data);
		if ((data & 0x0400))  {
			ug_exi_io_transaction(exi_device, 0xA000, &data);
			exi_dev_give(exi_device);
			if ((data & 0x0800)) {
				*c = data & 0xff;
				return 1;
			}
		} else {
			exi_dev_give(exi_device);
		}
	}
	return 0;
}


/*
 *
 * Linux console interface.
 */

/*
 *
 */
static void ug_console_write(struct console *co, const char *buf,
			      unsigned int count)
{
	struct ug_adapter *adapter = co->data;
	char *b = (char *)buf;

	while (count--) {
		if (*b == '\n')
			ug_safe_putc(adapter, '\r');
		ug_safe_putc(adapter, *b++);
	}
}

/*
 *
 */
static int ug_console_read(struct console *co, char *buf,
			    unsigned int count)
{
	struct ug_adapter *adapter = co->data;
	int i;
	char c;

	i = count;
	while (i--) {
		ug_safe_getc(adapter, &c);
		*buf++ = c;
	}
	return count;
}

static struct tty_driver *ug_tty_driver;

static struct tty_driver *ug_console_device(struct console *co, int *index)
{
	*index = co->index;
	return ug_tty_driver;
}


static struct console ug_consoles[] = {
	{
		.name   = DRV_MODULE_NAME "0",
		.write  = ug_console_write,
		.read   = ug_console_read,
		.device = ug_console_device,
		.flags  = CON_PRINTBUFFER | CON_ENABLED,
		.index  = 0,
		.data	= &ug_adapters[0],
	},
	{
		.name   = DRV_MODULE_NAME "1",
		.write  = ug_console_write,
		.read   = ug_console_read,
		.device = ug_console_device,
		.flags  = CON_PRINTBUFFER | CON_ENABLED,
		.index  = 1,
		.data	= &ug_adapters[1],
	},
};


/*
 *
 * Linux tty driver.
 */

static int ug_tty_poller(void *tty_)
{
	struct sched_param param = { .sched_priority = 1 };
	struct tty_struct *tty = tty_;
	struct ug_adapter *adapter;
	int count, chunk;
	const int max_outstanding = 32;
	char ch;

	sched_setscheduler(current, SCHED_FIFO, &param);
	set_task_state(current, TASK_RUNNING);

	chunk = 0;
	while (!kthread_should_stop()) {
		count = 0;
		adapter = tty->driver_data;
		if (adapter)
			count = ug_safe_getc(adapter, &ch);
		set_task_state(current, TASK_INTERRUPTIBLE);
		if (count) {
			tty_insert_flip_char(tty, ch, TTY_NORMAL);
			if (chunk++ > max_outstanding) {
				tty_flip_buffer_push(tty);
				chunk = 0;
			}
		} else {
			if (chunk) {
				tty_flip_buffer_push(tty);
				chunk = 0;
			}
			schedule_timeout(1);
		}
		set_task_state(current, TASK_RUNNING);
	}

	return 0;
}

static int ug_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct ug_adapter *adapter;
	int index;
	int retval = 0;

	index = tty->index;
	adapter = &ug_adapters[index];

	mutex_lock(&adapter->mutex);

	if (!adapter->exi_device) {
		mutex_unlock(&adapter->mutex);
		return -ENODEV;
	}

	if (!adapter->refcnt) {
		adapter->poller = kthread_run(ug_tty_poller, tty, "kugtty");
		if (IS_ERR(adapter->poller)) {
			drv_printk(KERN_ERR, "error creating poller thread\n");
			mutex_unlock(&adapter->mutex);
			return -ENOMEM;
		}
	}

	adapter->refcnt++;
	tty->driver_data = adapter;

	mutex_unlock(&adapter->mutex);

	return retval;
}

static void ug_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct ug_adapter *adapter;
	int index;

	index = tty->index;
	adapter = &ug_adapters[index];

	mutex_lock(&adapter->mutex);

	adapter->refcnt--;
	if (!adapter->refcnt) {
		if (!IS_ERR(adapter->poller))
			kthread_stop(adapter->poller);
		adapter->poller = ERR_PTR(-EINVAL);
		tty->driver_data = NULL;
	}

	mutex_unlock(&adapter->mutex);
}

static int ug_tty_write(struct tty_struct *tty,
			 const unsigned char *buf, int count)
{
	struct ug_adapter *adapter = tty->driver_data;
	char *b = (char *)buf;
	int index;
	int i;

	if (!adapter)
		return -ENODEV;

	index = tty->index;
	adapter = &ug_adapters[index];
	for (i = 0; i < count; i++)
		ug_safe_putc(adapter, *b++);
	return count;
}

static int ug_tty_write_room(struct tty_struct *tty)
{
	return 0x123; /* whatever */
}

static int ug_tty_chars_in_buffer(struct tty_struct *tty)
{
	return 0; /* unbuffered */
}


static const struct tty_operations ug_tty_ops = {
	.open = ug_tty_open,
	.close = ug_tty_close,
	.write = ug_tty_write,
	.write_room = ug_tty_write_room,
	.chars_in_buffer = ug_tty_chars_in_buffer,
};


static int ug_tty_init(void)
{
	struct tty_driver *driver;
	int retval;

	driver = alloc_tty_driver(2);
	if (!driver)
		return -ENOMEM;
	driver->name = DRV_MODULE_NAME "con";
	driver->major = TTY_MAJOR;
	driver->minor_start = 64;
	driver->type = TTY_DRIVER_TYPE_SYSCONS;
	driver->init_termios = tty_std_termios;
	tty_set_operations(driver, &ug_tty_ops);
	retval = tty_register_driver(driver);
	if (retval) {
		put_tty_driver(driver);
		return retval;
	}
	ug_tty_driver = driver;
	return 0;
}

static void ug_tty_exit(void)
{
	struct tty_driver *driver = ug_tty_driver;

	ug_tty_driver = NULL;
	if (driver) {
		tty_unregister_driver(driver);
		put_tty_driver(driver);
	}
}



/*
 *
 * EXI layer interface.
 */

/*
 *
 */
static int ug_probe(struct exi_device *exi_device)
{
	struct console *console;
	struct ug_adapter *adapter;
	unsigned int slot;

	/* don't try to drive a device which already has a real identifier */
	if (exi_device->eid.id != EXI_ID_NONE)
		return -ENODEV;

	if (!ug_check_adapter(exi_device))
		return -ENODEV;

	slot = to_channel(exi_get_exi_channel(exi_device));
	console = &ug_consoles[slot];
	adapter = console->data;

	drv_printk(KERN_INFO, "USB Gecko detected in memcard slot-%c\n",
		   'A'+slot);

	adapter->poller = ERR_PTR(-EINVAL);
	mutex_init(&adapter->mutex);
	adapter->refcnt = 0;

	adapter->exi_device = exi_device_get(exi_device);
	exi_set_drvdata(exi_device, adapter);
	register_console(console);

	ug_tty_init();

	return 0;
}

/*
 * Makes unavailable the USB Gecko adapter identified by the EXI device
 * `exi_device'.
 */
static void ug_remove(struct exi_device *exi_device)
{
	struct console *console;
	struct ug_adapter *adapter;
	unsigned int slot;

	slot = to_channel(exi_get_exi_channel(exi_device));
	console = &ug_consoles[slot];
	adapter = console->data;

	if (adapter->refcnt)
		drv_printk(KERN_ERR, "adapter removed while in use!\n");

	ug_tty_exit();

	unregister_console(console);
	exi_set_drvdata(exi_device, NULL);
	adapter->exi_device = NULL;
	exi_device_put(exi_device);

	mutex_destroy(&adapter->mutex);

	drv_printk(KERN_INFO, "USB Gecko removed from memcard slot-%c\n",
		   'A'+slot);
}

static struct exi_device_id ug_eid_table[] = {
	[0] = {
	       .channel = UG_SLOTA_CHANNEL,
	       .device = UG_SLOTA_DEVICE,
	       .id = EXI_ID_NONE,
	       },
	[1] = {
	       .channel = UG_SLOTB_CHANNEL,
	       .device = UG_SLOTB_DEVICE,
	       .id = EXI_ID_NONE,
	       },
	{.id = 0}
};

static struct exi_driver ug_exi_driver = {
	.name = DRV_MODULE_NAME,
	.eid_table = ug_eid_table,
	.frequency = UG_SPI_CLK_IDX,
	.probe = ug_probe,
	.remove = ug_remove,
};


/*
 *
 * Module interface.
 */

static int __init ug_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   ug_driver_version);

	return exi_driver_register(&ug_exi_driver);
}

static void __exit ug_exit_module(void)
{
	exi_driver_unregister(&ug_exi_driver);
}

module_init(ug_init_module);
module_exit(ug_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

