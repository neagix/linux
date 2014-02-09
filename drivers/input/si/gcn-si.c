/*
 * drivers/input/gcn-si.c
 *
 * Nintendo GameCube/Wii Serial Interface (SI) driver.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Steven Looman
 * Copyright (C) 2005,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/* #define SI_DEBUG */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/timer.h>

/*
 * This keymap is for a datel adapter + normal US keyboard.
 */
#include "gcn-keymap.h"

/*
 * Defining HACK_FORCE_KEYBOARD_PORT allows one to specify a port that
 * will be identified as a keyboard port in case the port gets incorrectly
 * identified.
 */
#define HACK_FORCE_KEYBOARD_PORT


#define DRV_MODULE_NAME  "gcn-si"
#define DRV_DESCRIPTION  "Nintendo GameCube/Wii Serial Interface (SI) driver"
#define DRV_AUTHOR       "Steven Looman <steven@krx.nl>, " \
			 "Albert Herranz"

static char si_driver_version[] = "1.0i";

#define drv_printk(level, format, arg...) \
	 printk(level DRV_MODULE_NAME ": " format , ## arg)

#define SI_MAX_PORTS		4		/* the four controller ports */
#define SI_REFRESH_TIME	(HZ/100)	/* polling interval */

/*
 * Hardware registers
 */
#define SI_PORT_SPACING	12

#define SICOUTBUF(i)	(0x00 + (i)*SI_PORT_SPACING)
#define SICINBUFH(i)	(0x04 + (i)*SI_PORT_SPACING)
#define SICINBUFL(i)	(0x08 + (i)*SI_PORT_SPACING)
#define SIPOLL		0x30
#define SICOMCSR	0x34
#define SISR		0x38
#define SIEXILK		0x3c

#define ID_PAD		0x0900
#define ID_KEYBOARD	0x0820
#define ID_WIRELESS_BIT (1 << 15)
#define ID_WAVEBIRD_BIT (1 << 8)

#define PAD_START	(1 << 28)
#define PAD_Y		(1 << 27)
#define PAD_X		(1 << 26)
#define PAD_B		(1 << 25)
#define PAD_A		(1 << 24)
#define PAD_LT		(1 << 22)
#define PAD_RT		(1 << 21)
#define PAD_Z		(1 << 20)
#define PAD_UP		(1 << 19)
#define PAD_DOWN	(1 << 18)
#define PAD_RIGHT	(1 << 17)
#define PAD_LEFT	(1 << 16)


struct si_keyboard_status {
	unsigned char old[3];
};

enum si_control_type {
	CTL_PAD,
	CTL_KEYBOARD,
	CTL_UNKNOWN
};

struct si_drvdata;

struct si_port {
	unsigned int index;
	struct si_drvdata *drvdata;

	u32 id; /* SI id */

	enum si_control_type type;
	unsigned int raw[2];

	struct input_dev *idev;
	struct timer_list timer;
	char name[32];

	union {
		struct si_keyboard_status keyboard;
	};

};

struct si_drvdata {
	unsigned long flags;
#define SI_QUIESCE	(1<<0)

	struct si_port ports[SI_MAX_PORTS];

	void __iomem *io_base;

	struct device *dev;
};


#ifdef HACK_FORCE_KEYBOARD_PORT

static int si_force_keyboard_port = -1;

#ifdef MODULE
module_param_named(force_keyboard_port, si_force_keyboard_port, int, 0644);
MODULE_PARM_DESC(force_keyboard_port, "port n becomes a keyboard port if"
		 " automatic identification fails");
#else
static int __init si_force_keyboard_port_setup(char *line)
{
	if (sscanf(line, "%d", &si_force_keyboard_port) != 1)
		si_force_keyboard_port = -1;
	return 1;
}
__setup("force_keyboard_port=", si_force_keyboard_port_setup);
#endif /* MODULE */

#endif /* HACK_FORCE_KEYBOARD_PORT */


/*
 * Hardware.
 *
 */

static void si_reset_all(void __iomem *io_base)
{
	int i;

	/* clear all SI registers */

	for (i = 0; i < SI_MAX_PORTS; ++i)
		out_be32(io_base + SICOUTBUF(i), 0);
	for (i = 0; i < SI_MAX_PORTS; ++i)
		out_be32(io_base + SICINBUFH(i), 0);
	for (i = 0; i < SI_MAX_PORTS; ++i)
		out_be32(io_base + SICINBUFL(i), 0);
	out_be32(io_base + SIPOLL, 0);
	out_be32(io_base + SICOMCSR, 0);
	out_be32(io_base + SISR, 0);

	/* these too... */
	out_be32(io_base + 0x80, 0);
	out_be32(io_base + 0x84, 0);
	out_be32(io_base + 0x88, 0);
	out_be32(io_base + 0x8c, 0);
	out_be32(io_base + 0x90, 0);
	out_be32(io_base + 0x94, 0);
	out_be32(io_base + 0x98, 0);
	out_be32(io_base + 0x9c, 0);
	out_be32(io_base + 0xa0, 0);
	out_be32(io_base + 0xa4, 0);
	out_be32(io_base + 0xa8, 0);
	out_be32(io_base + 0xac, 0);
}

static void si_set_rumbling(void __iomem *io_base, unsigned int index,
			    int rumble)
{
	out_be32(io_base + SICOUTBUF(index), 0x00400000 | (rumble) ? 1 : 0);
	out_be32(io_base + SISR, 0x80000000);
}

static void si_wait_transfer_done(void __iomem *io_base)
{
	unsigned long deadline = jiffies + 2*HZ;
	int borked = 0;

	while (!(in_be32(io_base + SICOMCSR) & (1 << 31)) && !borked) {
		cpu_relax();
		borked = time_after(jiffies, deadline);
	}

	if (borked) {
		drv_printk(KERN_ERR, "serial transfer took too long, "
			   "is your hardware ok?");
	}

	out_be32(io_base + SICOMCSR,
		 in_be32(io_base + SICOMCSR) | (1 << 31)); /* ack IRQ */
}

static u32 si_get_controller_id(void __iomem *io_base,
					  unsigned int index)
{
	out_be32(io_base + SICOUTBUF(index), 0);
	out_be32(io_base + SIPOLL, 0);

	out_be32(io_base + SISR, 0x80000000);
	out_be32(io_base + SICOMCSR, 0xd0010001 | index << 1);
	si_wait_transfer_done(io_base);

	return in_be32(io_base + 0x80) >> 16;
}

static void si_setup_polling(struct si_drvdata *drvdata)
{
	void __iomem *io_base = drvdata->io_base;
	unsigned long pad_bits = 0;
	int i;

	for (i = 0; i < SI_MAX_PORTS; ++i) {
		switch (drvdata->ports[i].type) {
		case CTL_PAD:
			out_be32(io_base + SICOUTBUF(i), 0x00400300);
			break;
		case CTL_KEYBOARD:
			out_be32(io_base + SICOUTBUF(i), 0x00540000);
			break;
		default:
			continue;
		}
		pad_bits |= 1 << (7 - i);
	}
	out_be32(io_base + SIPOLL, 0x00f70200 | pad_bits);

	out_be32(io_base + SISR, 0x80000000);
	out_be32(io_base + SICOMCSR, 0xc0010801);
	si_wait_transfer_done(io_base);
}

static void si_timer(unsigned long data)
{
	struct si_port *port = (struct si_port *)data;
	unsigned int index = port->index;
	void __iomem *io_base = port->drvdata->io_base;
	unsigned long raw[2];
	unsigned char key[3];
	unsigned char oldkey;
	int i;

	raw[0] = in_be32(io_base + SICINBUFH(index));
	raw[1] = in_be32(io_base + SICINBUFL(index));

	switch (port->type) {
	case CTL_PAD:
		/* buttons */
		input_report_key(port->idev, BTN_A, raw[0] & PAD_A);
		input_report_key(port->idev, BTN_B, raw[0] & PAD_B);
		input_report_key(port->idev, BTN_X, raw[0] & PAD_X);
		input_report_key(port->idev, BTN_Y, raw[0] & PAD_Y);
		input_report_key(port->idev, BTN_Z, raw[0] & PAD_Z);
		input_report_key(port->idev, BTN_TL,
				 raw[0] & PAD_LT);
		input_report_key(port->idev, BTN_TR,
				 raw[0] & PAD_RT);
		input_report_key(port->idev, BTN_START,
				 raw[0] & PAD_START);
		input_report_key(port->idev, BTN_0, raw[0] & PAD_UP);
		input_report_key(port->idev, BTN_1, raw[0] & PAD_RIGHT);
		input_report_key(port->idev, BTN_2, raw[0] & PAD_DOWN);
		input_report_key(port->idev, BTN_3, raw[0] & PAD_LEFT);

		/* axis */
		/* a stick */
		input_report_abs(port->idev, ABS_X,
				 raw[0] >> 8 & 0xFF);
		input_report_abs(port->idev, ABS_Y,
				 0xFF - (raw[0] >> 0 & 0xFF));

		/* b pad */
		if (raw[0] & PAD_RIGHT)
			input_report_abs(port->idev, ABS_HAT0X, 1);
		else if (raw[0] & PAD_LEFT)
			input_report_abs(port->idev, ABS_HAT0X, -1);
		else
			input_report_abs(port->idev, ABS_HAT0X, 0);

		if (raw[0] & PAD_DOWN)
			input_report_abs(port->idev, ABS_HAT0Y, 1);
		else if (raw[0] & PAD_UP)
			input_report_abs(port->idev, ABS_HAT0Y, -1);
		else
			input_report_abs(port->idev, ABS_HAT0Y, 0);

		/* c stick */
		input_report_abs(port->idev, ABS_RX,
				 raw[1] >> 24 & 0xFF);
		input_report_abs(port->idev, ABS_RY,
				 raw[1] >> 16 & 0xFF);

		/* triggers */
		input_report_abs(port->idev, ABS_BRAKE,
				 raw[1] >> 8 & 0xFF);
		input_report_abs(port->idev, ABS_GAS,
				 raw[1] >> 0 & 0xFF);

		break;

	case CTL_KEYBOARD:
		/*
		raw nibbles:
		  [4]<C>[0][0][0][0][0][0] <1H><1L><2H><2L><3H><3L><X><C>
		where:
		  [n] = fixed to n
		  <nH> <nL> = high / low nibble of n-th key pressed
			(0 if not pressed)
		  <X> = <1H> xor <2H> xor <3H>
		  <C> = counter: 0, 0, 1, 1, 2, 2, ..., F, F, 0, 0, ...
		*/
		key[0] = (raw[1] >> 24) & 0xFF;
		key[1] = (raw[1] >> 16) & 0xFF;
		key[2] = (raw[1] >>  8) & 0xFF;

		/* check if anything was released */
		for (i = 0; i < 3; ++i) {
			oldkey = port->keyboard.old[i];
			if (oldkey != key[0] &&
			    oldkey != key[1] && oldkey != key[2])
				input_report_key(port->idev,
						 gamecube_keymap[oldkey], 0);
		}

		/* report keys */
		for (i = 0; i < 3; ++i) {
			if (key[i])
				input_report_key(port->idev,
						 gamecube_keymap[key[i]], 1);
			port->keyboard.old[i] = key[i];
		}
		break;

	default:
		break;
	}

	input_sync(port->idev);

	if (!(port->drvdata->flags & SI_QUIESCE))
		mod_timer(&port->timer, jiffies + SI_REFRESH_TIME);
}

/*
 * Input driver hooks.
 *
 */

static int si_open(struct input_dev *idev)
{
	struct si_port *port = input_get_drvdata(idev);

	init_timer(&port->timer);
	port->timer.function = si_timer;
	port->timer.data = (unsigned long)port;
	port->timer.expires = jiffies + SI_REFRESH_TIME;
	add_timer(&port->timer);

	return 0;
}

static void si_close(struct input_dev *idev)
{
	struct si_port *port = input_get_drvdata(idev);

	del_timer(&port->timer);
}

static int si_event(struct input_dev *idev, unsigned int type,
		    unsigned int code, int value)
{
	struct si_port *port = input_get_drvdata(idev);
	unsigned int index = port->index;
	void __iomem *io_base = port->drvdata->io_base;

	if (type == EV_FF) {
		if (code == FF_RUMBLE)
			si_set_rumbling(io_base, index, value);
	}

	return value;
}

static int si_setup_pad(struct input_dev *idev)
{
	struct ff_device *ff;
	int retval;

	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_ABS, idev->evbit);

	set_bit(BTN_A, idev->keybit);
	set_bit(BTN_B, idev->keybit);
	set_bit(BTN_X, idev->keybit);
	set_bit(BTN_Y, idev->keybit);
	set_bit(BTN_Z, idev->keybit);
	set_bit(BTN_TL, idev->keybit);
	set_bit(BTN_TR, idev->keybit);
	set_bit(BTN_START, idev->keybit);
	set_bit(BTN_0, idev->keybit);
	set_bit(BTN_1, idev->keybit);
	set_bit(BTN_2, idev->keybit);
	set_bit(BTN_3, idev->keybit);

	/* a stick */
	set_bit(ABS_X, idev->absbit);
	idev->absinfo[ABS_X].minimum = 0;
	idev->absinfo[ABS_X].maximum = 255;
	idev->absinfo[ABS_X].fuzz = 8;
	idev->absinfo[ABS_X].flat = 8;

	set_bit(ABS_Y, idev->absbit);
	idev->absinfo[ABS_Y].minimum = 0;
	idev->absinfo[ABS_Y].maximum = 255;
	idev->absinfo[ABS_Y].fuzz = 8;
	idev->absinfo[ABS_Y].flat = 8;

	/* b pad */
	input_set_abs_params(idev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(idev, ABS_HAT0Y, -1, 1, 0, 0);

	/* c stick */
	set_bit(ABS_RX, idev->absbit);
	idev->absinfo[ABS_RX].minimum = 0;
	idev->absinfo[ABS_RX].maximum = 255;
	idev->absinfo[ABS_RX].fuzz = 8;
	idev->absinfo[ABS_RX].flat = 8;

	set_bit(ABS_RY, idev->absbit);
	idev->absinfo[ABS_RY].minimum = 0;
	idev->absinfo[ABS_RY].maximum = 255;
	idev->absinfo[ABS_RY].fuzz = 8;
	idev->absinfo[ABS_RY].flat = 8;

	/* triggers */
	set_bit(ABS_GAS, idev->absbit);
	idev->absinfo[ABS_GAS].minimum = -255;
	idev->absinfo[ABS_GAS].maximum = 255;
	idev->absinfo[ABS_GAS].fuzz = 16;
	idev->absinfo[ABS_GAS].flat = 16;

	set_bit(ABS_BRAKE, idev->absbit);
	idev->absinfo[ABS_BRAKE].minimum = -255;
	idev->absinfo[ABS_BRAKE].maximum = 255;
	idev->absinfo[ABS_BRAKE].fuzz = 16;
	idev->absinfo[ABS_BRAKE].flat = 16;

	/* rumbling */
	set_bit(EV_FF, idev->evbit);
	set_bit(FF_RUMBLE, idev->ffbit);
	retval = input_ff_create(idev, 1);
	if (retval)
		return retval;
	ff = idev->ff;
	idev->event = si_event;
	return 0;
}

static void si_setup_keyboard(struct input_dev *idev)
{
	int i;

	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_REP, idev->evbit);

	for (i = 0; i < 255; ++i)
		set_bit(gamecube_keymap[i], idev->keybit);
}

static int si_port_probe(struct si_port *port)
{
	unsigned int index = port->index;
	void __iomem *io_base = port->drvdata->io_base;
	struct input_dev *idev;
	int retval = 0;

	/*
	 * Determine input device type from SI id.
	 */
	port->id = si_get_controller_id(io_base, index);
	if (port->id == ID_PAD) {
		port->type = CTL_PAD;
		strcpy(port->name, "standard pad");
	} else if (port->id & ID_WIRELESS_BIT) {
		/* wireless pad */
		port->type = CTL_PAD;
		strcpy(port->name, (port->id & ID_WAVEBIRD_BIT) ?
		       "Nintendo Wavebird" : "wireless pad");
	} else if (port->id == ID_KEYBOARD) {
		port->type = CTL_KEYBOARD;
		strcpy(port->name, "keyboard");
	} else {
		port->type = CTL_UNKNOWN;
		if (port->id) {
			sprintf(port->name, "unknown (%x)",
				port->id);
#ifdef HACK_FORCE_KEYBOARD_PORT
			if (index+1 == si_force_keyboard_port) {
				drv_printk(KERN_WARNING,
					  "port %d forced to keyboard mode\n",
					  index+1);
				port->id = ID_KEYBOARD;
				port->type = CTL_KEYBOARD;
				strcpy(port->name, "keyboard (forced)");
			}
#endif /* HACK_FORCE_KEYBOARD_PORT */
		} else {
			strcpy(port->name, "not present");
		}
	}

	if (port->type == CTL_UNKNOWN) {
		retval = -ENODEV;
		goto done;
	}

	idev = input_allocate_device();
	if (!idev) {
		drv_printk(KERN_ERR, "failed to allocate input_dev\n");
		retval = -ENOMEM;
		goto done;
	}

	idev->open = si_open;
	idev->close = si_close;
	idev->name = port->name;

	switch (port->type) {
	case CTL_PAD:
		retval = si_setup_pad(idev);
		break;
	case CTL_KEYBOARD:
		si_setup_keyboard(idev);
		break;
	default:
		break;
	}

	if (retval) {
		input_free_device(idev);
		goto done;
	}

	input_set_drvdata(idev, port);
	port->idev = idev;

done:
	return retval;
}

/*
 * Setup routines.
 *
 */

static int si_init(struct si_drvdata *drvdata, struct resource *mem)
{
	struct si_port *port;
	int index;
	int retval;
	int error;

	drvdata->io_base = ioremap(mem->start, mem->end - mem->start + 1);

	si_reset_all(drvdata->io_base);
	for (index = 0; index < SI_MAX_PORTS; ++index) {
		port = &drvdata->ports[index];

		memset(port, 0, sizeof(*port));
		port->index = index;
		port->drvdata = drvdata;

		retval = si_port_probe(port);
		if (!retval) {
			error = input_register_device(port->idev);
			if (error) {
				drv_printk(KERN_ERR,
					   "input device registration failed"
					   " (%d) for port %d", error, index+1);
				port->idev = NULL;
			} else
				drv_printk(KERN_INFO, "port %d: %s\n",
					   index+1, port->name);
		}
	}

	si_setup_polling(drvdata);

	return 0;
}

static void si_exit(struct si_drvdata *drvdata)
{
	struct si_port *port;
	int index;

	for (index = 0; index < SI_MAX_PORTS; ++index) {
		port = &drvdata->ports[index];
		if (port->idev)
			input_unregister_device(port->idev);
	}

	if (drvdata->io_base) {
		iounmap(drvdata->io_base);
		drvdata->io_base = NULL;
	}
}

/*
 * Driver model helper routines.
 *
 */

static int si_do_probe(struct device *dev, struct resource *mem)
{
	struct si_drvdata *drvdata;
	int retval;

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		drv_printk(KERN_ERR, "failed to allocate si_drvdata\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, drvdata);
	drvdata->dev = dev;

	retval = si_init(drvdata, mem);
	if (retval) {
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
	}
	return retval;
}

static int si_do_remove(struct device *dev)
{
	struct si_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata) {
		si_exit(drvdata);
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
		return 0;
	}
	return -ENODEV;
}

static int si_do_shutdown(struct device *dev)
{
	struct si_drvdata *drvdata = dev_get_drvdata(dev);
	int i;

	if (drvdata) {
		drvdata->flags |= SI_QUIESCE;
		for (i = 0; i < SI_MAX_PORTS; ++i)
			del_timer_sync(&drvdata->ports[i].timer);
		si_reset_all(drvdata->io_base);
	}
	return 0;
}


/*
 * OF platform driver hooks.
 *
 */

static int si_of_probe(struct platform_device *odev)
{
	struct resource mem;
	int retval;

	retval = of_address_to_resource(odev->dev.of_node, 0, &mem);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return -ENODEV;
	}

	return si_do_probe(&odev->dev, &mem);
}

static int __exit si_of_remove(struct platform_device *odev)
{
	return si_do_remove(&odev->dev);
}

static void si_of_shutdown(struct platform_device *odev)
{
	si_do_shutdown(&odev->dev);
}

static struct of_device_id si_of_match[] = {
	{ .compatible = "nintendo,flipper-si" },
	{ .compatible = "nintendo,hollywood-si" },
	{ },
};


MODULE_DEVICE_TABLE(of, si_of_match);

static struct platform_driver si_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = si_of_match,
	},
	.probe = si_of_probe,
	.remove = si_of_remove,
	.shutdown = si_of_shutdown,
};


/*
 * Module interface hooks.
 *
 */

static int __init si_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   si_driver_version);

	return platform_driver_register(&si_of_driver);
}

static void __exit si_exit_module(void)
{
	platform_driver_unregister(&si_of_driver);
}

module_init(si_init_module);
module_exit(si_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

