/*
 * arch/powerpc/platforms/embedded6xx/flipper-pic.c
 *
 * Nintendo GameCube/Wii interrupt controller support.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2007,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */
#define DRV_MODULE_NAME "flipper-pic"
#define pr_fmt(fmt) DRV_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <asm/io.h>

#include "flipper-pic.h"

/*
 * IRQ chip hooks.
 *
 */

static void flipper_pic_mask_and_ack(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	clear_bit(irq, io_base + FLIPPER_IMR);
	set_bit(irq, io_base + FLIPPER_ICR);
}

static void flipper_pic_ack(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	set_bit(irq, io_base + FLIPPER_ICR);
}

static void flipper_pic_mask(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	clear_bit(irq, io_base + FLIPPER_IMR);
}

static void flipper_pic_unmask(unsigned int virq)
{
	int irq = virq_to_hw(virq);
	void __iomem *io_base = get_irq_chip_data(virq);

	set_bit(irq, io_base + FLIPPER_IMR);
}


static struct irq_chip flipper_pic = {
	.typename	= "flipper-pic",
	.ack		= flipper_pic_ack,
	.mask_ack	= flipper_pic_mask_and_ack,
	.mask		= flipper_pic_mask,
	.unmask		= flipper_pic_unmask,
};

/*
 * IRQ host hooks.
 *
 */

static struct irq_host *flipper_irq_host;

static int flipper_pic_map(struct irq_host *h, unsigned int virq,
			   irq_hw_number_t hwirq)
{
	set_irq_chip_data(virq, h->host_data);
	get_irq_desc(virq)->status |= IRQ_LEVEL;
	set_irq_chip_and_handler(virq, &flipper_pic, handle_level_irq);
	return 0;
}

static void flipper_pic_unmap(struct irq_host *h, unsigned int irq)
{
	set_irq_chip_data(irq, NULL);
	set_irq_chip(irq, NULL);
}

static int flipper_pic_match(struct irq_host *h, struct device_node *np)
{
	return 1;
}


static struct irq_host_ops flipper_irq_host_ops = {
	.map = flipper_pic_map,
	.unmap = flipper_pic_unmap,
	.match = flipper_pic_match,
};

/*
 * Platform hooks.
 *
 */

static void __flipper_quiesce(void __iomem *io_base)
{
	/* mask and ack all IRQs */
	out_be32(io_base + FLIPPER_IMR, 0x00000000);
	out_be32(io_base + FLIPPER_ICR, 0xffffffff);
}

struct irq_host * __init flipper_pic_init(struct device_node *np)
{
	struct irq_host *irq_host;
	struct resource res;
	void __iomem *io_base;
	int retval;

	retval = of_address_to_resource(np, 0, &res);
	if (retval) {
		pr_err("no io memory range found\n");
		return NULL;
	}
	io_base = ioremap(res.start, res.end - res.start + 1);

	pr_info("controller at 0x%08x mapped to 0x%p\n", res.start, io_base);

	__flipper_quiesce(io_base);

	irq_host = irq_alloc_host(np, IRQ_HOST_MAP_LINEAR, FLIPPER_NR_IRQS,
				  &flipper_irq_host_ops, -1);
	if (!irq_host) {
		pr_err("failed to allocate irq_host\n");
		return NULL;
	}

	irq_host->host_data = io_base;

	return irq_host;
}

unsigned int flipper_pic_get_irq(void)
{
	void __iomem *io_base = flipper_irq_host->host_data;
	int irq;
	u32 irq_status;

	irq_status = in_be32(io_base + FLIPPER_ICR) &
		     in_be32(io_base + FLIPPER_IMR);
	if (irq_status == 0)
		return -1;	/* no more IRQs pending */

	__asm__ __volatile__("cntlzw %0,%1" : "=r"(irq) : "r"(irq_status));
	return irq_linear_revmap(flipper_irq_host, 31 - irq);
}

/*
 * Probe function.
 *
 */

void __init flipper_pic_probe(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "nintendo,flipper-pic");
	BUG_ON(!np);

	flipper_irq_host = flipper_pic_init(np);
	BUG_ON(!flipper_irq_host);

	irq_set_default_host(flipper_irq_host);

	of_node_put(np);
}

/*
 * Misc functions related to the flipper chipset.
 *
 */

/**
 * flipper_quiesce() - quiesce flipper irq controller
 *
 * Mask and ack all interrupt sources.
 *
 */
void flipper_quiesce(void)
{
	void __iomem *io_base = flipper_irq_host->host_data;

	__flipper_quiesce(io_base);
}

/*
 * Resets the platform.
 */
void flipper_platform_reset(void)
{
	void __iomem *io_base;

	if (flipper_irq_host && flipper_irq_host->host_data) {
		io_base = flipper_irq_host->host_data;
		out_8(io_base + FLIPPER_RESET, 0x00);
	}
}

/*
 * Returns non-zero if the reset button is pressed.
 */
int flipper_is_reset_button_pressed(void)
{
	void __iomem *io_base;
	u32 icr;

	if (flipper_irq_host && flipper_irq_host->host_data) {
		io_base = flipper_irq_host->host_data;
		icr = in_be32(io_base + FLIPPER_ICR);
		return !(icr & FLIPPER_ICR_RSS);
	}
	return 0;
}

