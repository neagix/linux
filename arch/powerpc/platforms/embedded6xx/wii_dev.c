/*
 * arch/powerpc/platforms/embedded6xx/wii_dev.c
 *
 * Nintendo Wii platform device setup.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_platform.h>

#include <asm/machdep.h>

static struct of_device_id wii_of_bus[] = {
	{ .compatible = "nintendo,hollywood", },
#ifdef CONFIG_STARLET_IOS
	{ .compatible = "nintendo,starlet-ios-ipc", },
#endif
#ifdef CONFIG_STARLET_MINI
	{ .compatible = "twiizers,starlet-mini-ipc", },
#endif
	{ },
};

static int __init wii_device_probe(void)
{
	struct device_node *np;

	if (!machine_is(wii))
		return 0;

	of_platform_bus_probe(NULL, wii_of_bus, NULL);

	np = of_find_compatible_node(NULL, NULL, "nintendo,hollywood-mem2");
	if (np) {
		of_platform_device_create(np, NULL, NULL);
		of_node_put(np);
	}

	return 0;
}
device_initcall(wii_device_probe);

