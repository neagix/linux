/*
 * drivers/input/keyboard/rvl-stkbd.c
 *
 * Nintendo Wii starlet keyboard driver.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 * NOTES:
 * The keyboard driver requires at least IOS30 installed.
 * LED support is pending.
 */

#define DEBUG

#define DBG(fmt, arg...)        pr_debug(fmt, ##arg)

#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <asm/starlet-ios.h>

#define DRV_MODULE_NAME  "rvl-stkbd"
#define DRV_DESCRIPTION  "Nintendo Wii starlet keyboard driver"
#define DRV_AUTHOR       "Albert Herranz"

static char stkbd_driver_version[] = "0.2i";

#define drv_printk(level, format, arg...) \
	 printk(level DRV_MODULE_NAME ": " format , ## arg)

/*
 * Keyboard events from IOS.
 */
#define STKBD_EV_CONNECT	0x00000000
#define STKBD_EV_DISCONNECT	0x00000001
#define STKBD_EV_REPORT		0x00000002

struct stkbd_event {
	u32 type;
	u32 _unk1;
	union {
		struct {
			u8 modifiers;
			u8 reserved;
			u8 keys[6];
		} report;
		u8 raw_report[8];
	};
} __attribute__((packed));

/*
 * Keyboard device.
 */

enum {
	__STKBD_RUNNING = 1,		/* device is opened and running */
	__STKBD_WAITING_REPORT		/* waiting for IOS report */
};

struct stkbd_keyboard {
	int		fd;

	struct		stkbd_event *event;
	u8		old_raw_report[8];

	unsigned long	flags;
#define STKBD_RUNNING		(1 << __STKBD_RUNNING)
#define STKBD_WAITING_REPORT	(1 << __STKBD_WAITING_REPORT)

	char		name[32];
	struct input_dev *idev;
	unsigned int	usage;

	struct device	*dev;
};

/* device path in IOS for the USB keyboard */
static char stkbd_dev_path[] = "/dev/usb/kbd";

/*
 * Keycodes are standard USB keyboard HID keycodes.
 * We use the same table as in drivers/hid/usbhid/usbkbd.c.
 */

#define NR_SCANCODES 256

static unsigned char stkbd_keycode[NR_SCANCODES] = {
/*000*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*004*/	KEY_A,		KEY_B,		KEY_C,		KEY_D,
/*008*/	KEY_E,		KEY_F,		KEY_G,		KEY_H,
/*012*/	KEY_I,		KEY_J,		KEY_K,		KEY_L,
/*016*/	KEY_M,		KEY_N,		KEY_O,		KEY_P,
/*020*/	KEY_Q,		KEY_R,		KEY_S,		KEY_T,
/*024*/	KEY_U,		KEY_V,		KEY_W,		KEY_X,
/*028*/	KEY_Y,		KEY_Z,		KEY_1,		KEY_2,
/*032*/	KEY_3,		KEY_4,		KEY_5,		KEY_6,
/*036*/	KEY_7,		KEY_8,		KEY_9,		KEY_0,
/*040*/	KEY_ENTER,	KEY_ESC,	KEY_BACKSPACE,	KEY_TAB,
/*044*/	KEY_SPACE,	KEY_MINUS,	KEY_EQUAL,	KEY_LEFTBRACE,
/*048*/	KEY_RIGHTBRACE,	KEY_BACKSLASH,	KEY_BACKSLASH,	KEY_SEMICOLON,
/*052*/	KEY_APOSTROPHE,	KEY_GRAVE,	KEY_COMMA,	KEY_DOT,
/*056*/	KEY_SLASH,	KEY_CAPSLOCK,	KEY_F1,		KEY_F2,
/*060*/	KEY_F3,		KEY_F4,		KEY_F5,		KEY_F6,
/*064*/	KEY_F7,		KEY_F8,		KEY_F9,		KEY_F10,
/*068*/	KEY_F11,	KEY_F12,	KEY_SYSRQ,	KEY_SCROLLLOCK,
/*072*/	KEY_PAUSE,	KEY_INSERT,	KEY_HOME,	KEY_PAGEUP,
/*076*/	KEY_DELETE,	KEY_END,	KEY_PAGEDOWN,	KEY_RIGHT,
/*080*/	KEY_LEFT,	KEY_DOWN,	KEY_UP,		KEY_NUMLOCK,
/*084*/	KEY_KPSLASH,	KEY_KPASTERISK,	KEY_KPMINUS,	KEY_KPPLUS,
/*088*/	KEY_KPENTER,	KEY_KP1,	KEY_KP2,	KEY_KP3,
/*092*/	KEY_KP4,	KEY_KP5,	KEY_KP6,	KEY_KP7,
/*096*/	KEY_KP8,	KEY_KP9,	KEY_KP0,	KEY_KPDOT,
/*100*/	KEY_102ND,	KEY_COMPOSE,	KEY_POWER,	KEY_KPEQUAL,
/*104*/	KEY_F13,	KEY_F14,	KEY_F15,	KEY_F16,
/*108*/	KEY_F17,	KEY_F18,	KEY_F19,	KEY_F20,
/*112*/	KEY_F21,	KEY_F22,	KEY_F23,	KEY_F24,
/*116*/	KEY_OPEN,	KEY_HELP,	KEY_PROPS,	KEY_FRONT,
/*120*/	KEY_STOP,	KEY_AGAIN,	KEY_UNDO,	KEY_CUT,
/*124*/	KEY_COPY,	KEY_PASTE,	KEY_FIND,	KEY_MUTE,
/*128*/	KEY_VOLUMEUP,	KEY_VOLUMEDOWN,	KEY_RESERVED,	KEY_RESERVED,
/*132*/	KEY_RESERVED,	KEY_KPCOMMA,	KEY_RESERVED,	KEY_RO,
/*136*/	KEY_KATAKANAHIRAGANA,	KEY_YEN,	KEY_HENKAN,	KEY_MUHENKAN,
/*140*/	KEY_KPJPCOMMA,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*144*/	KEY_HANGEUL,	KEY_HANJA,	KEY_KATAKANA,	KEY_HIRAGANA,
/*148*/	KEY_ZENKAKUHANKAKU,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*152*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*156*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*160*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*164*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*168*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*172*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*176*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*180*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*184*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*188*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*192*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*196*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*200*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*204*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*208*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*212*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*216*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*220*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
/*224*/	KEY_LEFTCTRL,	KEY_LEFTSHIFT,	KEY_LEFTALT,	KEY_LEFTMETA,
/*228*/	KEY_RIGHTCTRL,	KEY_RIGHTSHIFT,	KEY_RIGHTALT,	KEY_RIGHTMETA,
/*232*/	KEY_PLAYPAUSE,	KEY_STOPCD,	KEY_PREVIOUSSONG,	KEY_NEXTSONG,
/*236*/	KEY_EJECTCD,	KEY_VOLUMEUP,	KEY_VOLUMEDOWN,	KEY_MUTE,
/*240*/	KEY_WWW,	KEY_BACK,	KEY_FORWARD,	KEY_STOP,
/*244*/	KEY_FIND,	KEY_SCROLLUP,	KEY_SCROLLDOWN,	KEY_EDIT,
/*248*/	KEY_SLEEP,	KEY_SCROLLLOCK,	KEY_REFRESH,	KEY_CALC,
/*252*/	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,	KEY_RESERVED,
};


static int stkbd_wait_for_events(struct stkbd_keyboard *kbd);

static void stkbd_dispatch_report(struct stkbd_keyboard *kbd)
{
	struct stkbd_event *event = kbd->event;
	u8 *new = event->raw_report;
	u8 *old = kbd->old_raw_report;
	int i;

	/*
	 * The report is a standard USB HID report for a keyboard.
	 * Modifiers come in byte 0 as variable data and map to
	 * positions 224-231 of the USB HID keyboard/keypad page.
	 */
	for (i = 0; i < 8; i++)
		input_report_key(kbd->idev, stkbd_keycode[i + 224],
				 (new[0] >> i) & 1);

	/*
	 * Byte 1 is reserved and ignored here.
	 * Bytes 2 to 7 contain the indexes of up to 6 keys pressed.
	 * A value of 01 for an index means "Keyboard ErrorRollOver" and is
	 * reported when the keyboard is in error (for example, when too
	 * many keys are simultaneously pressed).
	 * Index values less than or equal to 03 can be safely ignored.
	 */
	for (i = 2; i < 8; i++) {
		/* released keys are old indexes not found in new array */
		if (old[i] > 3 && memscan(new + 2, old[i], 6) == new + 8) {
			if (stkbd_keycode[old[i]])
				input_report_key(kbd->idev,
						 stkbd_keycode[old[i]], 0);
			else
				drv_printk(KERN_WARNING, "unknown key"
					   " (scancode %#x) released.", old[i]);
		}

		/* pressed keys are new indexes not found in old array */
		if (new[i] > 3 && memscan(old + 2, new[i], 6) == old + 8) {
			if (stkbd_keycode[new[i]])
				input_report_key(kbd->idev,
						 stkbd_keycode[new[i]], 1);
			else
				drv_printk(KERN_WARNING, "unknown key"
					   " (scancode %#x) pressed.", new[i]);
		}
	}

	input_sync(kbd->idev);
	memcpy(old, new, 8);
}

static int stkbd_dispatch_ipc_request(struct starlet_ipc_request *req)
{
	struct stkbd_keyboard *kbd;
	struct stkbd_event *event;
	int error;

	/* retrieve the interesting data before freeing the request */
	kbd = req->done_data;
	error = req->result;
	starlet_ipc_free_request(req);

	clear_bit(__STKBD_WAITING_REPORT, &kbd->flags);

	if (kbd->fd == -1)
		return -ENODEV;

	if (error) {
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	} else {
		event = kbd->event;

		switch (event->type) {
		case STKBD_EV_CONNECT:
			drv_printk(KERN_INFO, "keyboard connected\n");
			break;
		case STKBD_EV_DISCONNECT:
			drv_printk(KERN_INFO, "keyboard disconnected\n");
			break;
		case STKBD_EV_REPORT:
			if (test_bit(__STKBD_RUNNING, &kbd->flags))
				stkbd_dispatch_report(kbd);
			break;
		}

		error = stkbd_wait_for_events(kbd);
	}
	return error;
}

static int stkbd_wait_for_events(struct stkbd_keyboard *kbd)
{
	struct stkbd_event *event = kbd->event;
	int error = 0;

	if (!test_and_set_bit(__STKBD_WAITING_REPORT, &kbd->flags)) {
		error = starlet_ioctl_nowait(kbd->fd, 0,
						 NULL, 0,
						 event, sizeof(*event),
						 stkbd_dispatch_ipc_request,
						 kbd);
		if (error)
			drv_printk(KERN_ERR, "ioctl error %d (%04x)\n",
				   error, error);
	}
	return error;
}

/*
 * Input driver hooks.
 *
 */

static DEFINE_MUTEX(open_lock);

static int stkbd_open(struct input_dev *idev)
{
	struct stkbd_keyboard *kbd = input_get_drvdata(idev);
	int error = 0;

	if (!kbd)
		return -ENODEV;

	mutex_lock(&open_lock);
	kbd->usage++;
	set_bit(__STKBD_RUNNING, &kbd->flags);
	mutex_unlock(&open_lock);

	return error;
}

static void stkbd_close(struct input_dev *idev)
{
	struct stkbd_keyboard *kbd = input_get_drvdata(idev);

	if (!kbd)
		return;

	mutex_lock(&open_lock);
	kbd->usage--;
	if (kbd->usage == 0)
		clear_bit(__STKBD_RUNNING, &kbd->flags);
	mutex_unlock(&open_lock);
}

static void stkbd_setup_keyboard(struct input_dev *idev)
{
	int i;

	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_REP, idev->evbit);

	for (i = 0; i < 255; ++i)
		set_bit(stkbd_keycode[i], idev->keybit);
	clear_bit(0, idev->keybit);
}

static int stkbd_init_input_dev(struct stkbd_keyboard *kbd)
{
	struct input_dev *idev;
	int error;

	idev = input_allocate_device();
	if (!idev) {
		drv_printk(KERN_ERR, "failed to allocate input_dev\n");
		return -ENOMEM;
	}

	idev->dev.parent = kbd->dev;

	strcpy(kbd->name, "USB keyboard");
	idev->name = kbd->name;

	input_set_drvdata(idev, kbd);
	kbd->idev = idev;

	stkbd_setup_keyboard(idev);

	idev->open = stkbd_open;
	idev->close = stkbd_close;

	error = input_register_device(kbd->idev);
	if (error) {
		input_free_device(kbd->idev);
		return error;
	}

	return 0;
}

static void stkbd_exit_input_dev(struct stkbd_keyboard *kbd)
{
	input_free_device(kbd->idev);
}

/*
 * Setup routines.
 *
 */

static int stkbd_init(struct stkbd_keyboard *kbd)
{
	struct stkbd_event *event;
	int error;

	event = starlet_kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		drv_printk(KERN_ERR, "failed to allocate stkbd_event\n");
		error = -ENOMEM;
		goto err;
	}
	kbd->event = event;

	kbd->fd = starlet_open(stkbd_dev_path, 0);
	if (kbd->fd < 0) {
		drv_printk(KERN_ERR, "unable to open device %s\n",
			   stkbd_dev_path);
		error = kbd->fd;
		goto err_event;
	}

	error = stkbd_init_input_dev(kbd);
	if (error)
		goto err_fd;

	/* start to grab events from the keyboard */
	error = stkbd_wait_for_events(kbd);
	if (error)
		goto err_input_dev;

	return 0;

err_input_dev:
	stkbd_exit_input_dev(kbd);
err_fd:
	starlet_close(kbd->fd);
err_event:
	starlet_kfree(event);
err:
	return error;
}

static void stkbd_exit(struct stkbd_keyboard *kbd)
{
	stkbd_exit_input_dev(kbd);
	starlet_close(kbd->fd);
	starlet_kfree(kbd->event);
}

/*
 * Driver model helper routines.
 *
 */

static int stkbd_do_probe(struct device *dev)
{
	struct stkbd_keyboard *kbd;
	int error;

	kbd = kzalloc(sizeof(*kbd), GFP_KERNEL);
	if (!kbd) {
		drv_printk(KERN_ERR, "failed to allocate stkbd_keyboard\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, kbd);
	kbd->dev = dev;

	error = stkbd_init(kbd);
	if (error) {
		dev_set_drvdata(dev, NULL);
		kfree(kbd);
	}
	return error;
}

static int stkbd_do_remove(struct device *dev)
{
	struct stkbd_keyboard *kbd = dev_get_drvdata(dev);

	if (kbd) {
		stkbd_exit(kbd);
		dev_set_drvdata(dev, NULL);
		kfree(kbd);
		return 0;
	}
	return -ENODEV;
}


/*
 * OF platform driver hooks.
 *
 */

static int stkbd_of_probe(struct platform_device *odev)

{
	return stkbd_do_probe(&odev->dev);
}

static int __exit stkbd_of_remove(struct platform_device *odev)
{
	return stkbd_do_remove(&odev->dev);
}

static struct of_device_id stkbd_of_match[] = {
	{ .compatible = "nintendo,starlet-ios-keyboard" },
	{ },
};


MODULE_DEVICE_TABLE(of, stkbd_of_match);

static struct platform_driver stkbd_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = stkbd_of_match,
	},
	.probe = stkbd_of_probe,
	.remove = stkbd_of_remove,
};


/*
 * Module interface hooks.
 *
 */

static int __init stkbd_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   stkbd_driver_version);

	return platform_driver_register(&stkbd_of_driver);
}

static void __exit stkbd_exit_module(void)
{
	platform_driver_unregister(&stkbd_of_driver);
}

module_init(stkbd_init_module);
module_exit(stkbd_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
