/*
 * arch/powerpc/platforms/embedded6xx/hlwd-gpio.c
 *
 * Nintendo Wii (Hollywood) GPIO driver
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>

#define DRV_MODULE_NAME "hlwd-gpio"

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)


struct hlwd_gpio_chip {
	struct of_mm_gpio_chip mmchip;
	spinlock_t lock;
};

struct hlwd_gpio_regs {
	__be32 out, dir, in;
};


static inline struct hlwd_gpio_chip *
to_hlwd_gpio_chip(struct of_mm_gpio_chip *mm_gc)
{
	return container_of(mm_gc, struct hlwd_gpio_chip, mmchip);
}

static int hlwd_gpio_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct hlwd_gpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);
	unsigned int val;

	val = !!(in_be32(&regs->in) & pin_mask);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);

	return val;
}

static void hlwd_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct hlwd_gpio_chip *st_gc = to_hlwd_gpio_chip(mm_gc);
	struct hlwd_gpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);
	u32 data;
	unsigned long flags;

	spin_lock_irqsave(&st_gc->lock, flags);
	data = in_be32(&regs->in) & ~pin_mask;
	if (val)
		data |= pin_mask;
	out_be32(&regs->out, data);
	spin_unlock_irqrestore(&st_gc->lock, flags);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);
}

static int hlwd_gpio_dir_in(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct hlwd_gpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);

	clrbits32(&regs->dir, pin_mask);

	return 0;
}

static int hlwd_gpio_dir_out(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct hlwd_gpio_regs __iomem *regs = mm_gc->regs;
	u32 pin_mask = 1 << (31 - gpio);

	setbits32(&regs->dir, pin_mask);
	hlwd_gpio_set(gc, gpio, val);

	return 0;
}

int hlwd_gpio_add32(struct device_node *np)
{
	struct of_mm_gpio_chip *mm_gc;
	struct of_gpio_chip *of_gc;
	struct gpio_chip *gc;
	struct hlwd_gpio_chip *st_gc;
	const unsigned long *prop;
	int error;

	st_gc = kzalloc(sizeof(*st_gc), GFP_KERNEL);
	if (!st_gc)
		return -ENOMEM;

	spin_lock_init(&st_gc->lock);
	mm_gc = &st_gc->mmchip;
	of_gc = &mm_gc->of_gc;
	gc = &of_gc->gc;

	prop = of_get_property(np, "#gpio-cells", NULL);
	if (prop && *prop >= 2)
		of_gc->gpio_cells = *prop;
	else
		of_gc->gpio_cells = 2; /* gpio pin number, flags */

	gc->ngpio = 32;
	gc->direction_input = hlwd_gpio_dir_in;
	gc->direction_output = hlwd_gpio_dir_out;
	gc->get = hlwd_gpio_get;
	gc->set = hlwd_gpio_set;

	error = of_mm_gpiochip_add(np, mm_gc);
	if (!error)
		drv_printk(KERN_INFO, "%s: added %u gpios at %p\n",
			   np->name, gc->ngpio, mm_gc->regs);
	return error;
}

static int hlwd_gpio_init(void)
{
	struct device_node *np;
	int error;

	for_each_compatible_node(np, NULL, "nintendo,hollywood-gpio") {
		error = hlwd_gpio_add32(np);
		if (error < 0)
			drv_printk(KERN_ERR, "error %d adding gpios"
				   " for %s\n", error, np->full_name);
	}
	return 0; /* whatever */
}
arch_initcall(hlwd_gpio_init);

