/*
 * arch/powerpc/platforms/embedded6xx/starlet-stm.c
 *
 * Nintendo Wii starlet STM routines
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define DBG(fmt, arg...)        drv_printk(KERN_INFO, fmt, ##arg)

#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <asm/starlet-ios.h>


/*
 * /dev/stm/immediate
 *
 */

#define STARLET_STM_HOTRESET	0x2001
#define STARLET_STM_SHUTDOWN	0x2003

#define STARLET_STM_TIMEOUT	1000000	/* usecs */

#define STARLET_DEV_STM_IMMEDIATE	"/dev/stm/immediate"

#define drv_printk(level, format, arg...) \
	 printk(level "starlet-stm: " format , ## arg)


static const char dev_stm_immediate[] = STARLET_DEV_STM_IMMEDIATE;

/* private aligned buffer for restart/power_off operations */
static u32 starlet_stm_buf[(STARLET_IPC_DMA_ALIGN+1)/sizeof(u32)]
		 __attribute__ ((aligned(STARLET_IPC_DMA_ALIGN+1)));

/*
 *
 */
static void starlet_stm_common_restart(int request, u32 value)
{
	static int already_in_restart;
	u32 *buf = starlet_stm_buf;
	size_t len = sizeof(starlet_stm_buf);
	int fd;
	int error;

	if (already_in_restart) {
		drv_printk(KERN_ERR, "previous restart attempt failed,"
			   " halting\n");
		goto halt;
	}

	drv_printk(KERN_INFO, "trying IPC restart...\n");
	already_in_restart = 1;

	fd = starlet_open_polled(dev_stm_immediate, 0, STARLET_STM_TIMEOUT);
	if (fd < 0) {
		drv_printk(KERN_ERR, "failed to open %s\n", dev_stm_immediate);
		error = fd;
		goto done;
	}

	*buf = value;
	error = starlet_ioctl_polled(fd, request, buf, len, buf, len,
				     STARLET_STM_TIMEOUT);
	if (error < 0) {
		drv_printk(KERN_ERR, "ioctl %d failed\n", request);
		starlet_close_polled(fd, STARLET_STM_TIMEOUT);
	} else {
halt:
		for (;;)
			cpu_relax();
	}

done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
}

/*
 *
 */
void starlet_stm_restart(void)
{
	starlet_stm_common_restart(STARLET_STM_HOTRESET, 0);
}
/*EXPORT_SYMBOL_GPL(starlet_stm_restart);*/

/*
 *
 */
void starlet_stm_power_off(void)
{
	starlet_stm_common_restart(STARLET_STM_SHUTDOWN, 0);
}
/*EXPORT_SYMBOL_GPL(starlet_stm_power_off);*/


