/*
 * arch/powerpc/platforms/embedded6xx/gamecube_dev.c
 *
 * Nintendo GameCube platform device setup.
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

static struct of_device_id gamecube_of_bus[] = {
	{ .compatible = "nintendo,flipper", },
	{ },
};

static int __init gamecube_device_probe(void)
{
	if (!machine_is(gamecube))
		return 0;

	of_platform_bus_probe(NULL, gamecube_of_bus, NULL);
	return 0;
}
device_initcall(gamecube_device_probe);
