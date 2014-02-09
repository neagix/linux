/*
 * arch/powerpc/platforms/embedded6xx/wii.c
 *
 * Nintendo Wii board-specific support
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
#include <linux/irq.h>
#include <linux/seq_file.h>
#include <linux/kexec.h>
#include <linux/exi.h>
#include <linux/gpio.h>

#include <asm/io.h>
#include <asm/machdep.h>
#include <asm/prom.h>
#include <asm/time.h>
#include <asm/starlet.h>
#include <asm/starlet-ios.h>
#include <asm/starlet-mini.h>
#include <asm/udbg.h>

#include "flipper-pic.h"
#include "gcnvi_udbg.h"
#include "usbgecko_udbg.h"


static enum starlet_ipc_flavour starlet_ipc_flavour;

static void wii_death_loop(void)
{
	local_irq_disable();
	for (;;)
		cpu_relax();
}

#ifdef CONFIG_STARLET_IOS

static void wii_ios_restart(char *cmd)
{
	local_irq_disable();

	/* try first to launch The Homebrew Channel... */
	starlet_es_reload_ios_and_launch(STARLET_TITLE_HBC_V107);
	starlet_es_reload_ios_and_launch(STARLET_TITLE_HBC_JODI);
	starlet_es_reload_ios_and_launch(STARLET_TITLE_HBC_HAXX);
	starlet_stm_restart(); /* ..and if that fails, try an assisted restart */

	/* fallback to spinning until the power button pressed */
	for (;;)
		cpu_relax();
}

static void wii_ios_power_off(void)
{
	local_irq_disable();

	/* try an assisted poweroff */
	starlet_stm_power_off();

	/* fallback to spinning until the power button pressed */
	for (;;)
		cpu_relax();
}

#else

static void wii_ios_restart(char *cmd)
{
	wii_death_loop();
}

static void wii_ios_power_off(void)
{
	wii_death_loop();
}

#endif /* CONFIG_STARLET_IOS */

#ifdef CONFIG_STARLET_MINI

static void wii_restart(char *cmd)
{
	void __iomem *hw_resets;

	local_irq_disable();

	hw_resets = ioremap(0x0d800194, 4);
	if (hw_resets) {
		/* clear the system reset pin to cause a reset */
		clear_bit(0, hw_resets);
		iounmap(hw_resets);
	}

	for (;;)
		cpu_relax();
}

static void wii_power_off(void)
{
	void __iomem *gpio;

	local_irq_disable();

	gpio = ioremap(0x0d8000e0, 3*4);
	if (gpio) {
		/* make sure that the poweroff GPIO is configured as output */
		out_be32(gpio + 4, in_be32(gpio + 4) | 0x2);

		/* drive the poweroff GPIO high */
		out_be32(gpio + 0, in_be32(gpio + 0) | 0x2);
		iounmap(gpio);
	}

	for (;;)
		cpu_relax();
}

#else

static void wii_restart(char *cmd)
{
	wii_death_loop();
}

static void wii_power_off(void)
{
	wii_death_loop();
}

#endif /* CONFIG_STARLET_MINI */

static void wii_halt(void)
{
	if (ppc_md.restart)
		ppc_md.restart(NULL);
	wii_death_loop();
}

static void wii_show_cpuinfo(struct seq_file *m)
{
	seq_printf(m, "vendor\t\t: IBM\n");
	seq_printf(m, "machine\t\t: Nintendo Wii\n");
}

int starlet_discover_ipc_flavour(void)
{
	struct mipc_infohdr *hdrp;
	int error;

	error = mipc_discover(&hdrp);
	if (!error) {
		starlet_ipc_flavour = STARLET_IPC_MINI;
		ppc_md.restart = wii_restart;
		ppc_md.power_off = wii_power_off;
	} else {
		starlet_ipc_flavour = STARLET_IPC_IOS;
		ppc_md.restart = wii_ios_restart;
		ppc_md.power_off = wii_ios_power_off;
	}

	return 0;
}

enum starlet_ipc_flavour starlet_get_ipc_flavour(void)
{
	return starlet_ipc_flavour;
}

static void __init wii_setup_arch(void)
{
	ug_udbg_init();
	gcnvi_udbg_init();

	starlet_discover_ipc_flavour();
}

static void __init wii_init_early(void)
{
}

static int __init wii_probe(void)
{
	unsigned long dt_root;

	dt_root = of_get_flat_dt_root();
	if (!of_flat_dt_is_compatible(dt_root, "nintendo,wii"))
		return 0;

	return 1;
}

#ifdef CONFIG_KEXEC
static void wii_shutdown(void)
{
	exi_quiesce();
	flipper_quiesce();
}

static int restore_lowmem_stub(struct kimage *image)
{
	struct device_node *node;
	struct resource res;
	const unsigned long *prop;
	unsigned long dst, src;
	size_t size;
	int error;

	node = of_find_node_by_name(NULL, "lowmem-stub");
	if (!node) {
		printk(KERN_ERR "unable to find node %s\n", "lowmem-stub");
		error = -ENODEV;
		goto out;
	}

	error = of_address_to_resource(node, 0, &res);
	if (error) {
		printk(KERN_ERR "no lowmem-stub range found\n");
		goto out_put;
	}
	dst = res.start;
	size = res.end - res.start + 1;

	prop = of_get_property(node, "save-area", NULL);
	if (!prop) {
		printk(KERN_ERR "unable to find %s property\n", "save-area");
		error = -EINVAL;
		goto out_put;
	}
	src = *prop;

	printk(KERN_DEBUG "lowmem-stub: preparing restore from %08lX to %08lX"
		" (%u bytes)\n", src, dst, size);

	/* schedule a copy of the lowmem stub to its original location */
	error = kimage_add_preserved_region(image, dst, src, PAGE_ALIGN(size));

out_put:
	of_node_put(node);
out:
	return error;
}

static int wii_machine_kexec_prepare(struct kimage *image)
{
	int error;

	error = restore_lowmem_stub(image);
	if (error)
		printk(KERN_ERR "%s: error %d\n", __func__, error);
	return error;
}

static void wii_machine_kexec(struct kimage *image)
{
	local_irq_disable();

#ifdef CONFIG_STARLET_IOS
	/*
	 * Reload IOS to make sure that I/O resources are freed before
	 * the final kexec phase.
	 */
	if (starlet_get_ipc_flavour() == STARLET_IPC_IOS)
		starlet_es_reload_ios_and_discard();
#endif

	default_machine_kexec(image);
}

#endif /* CONFIG_KEXEC */


define_machine(wii) {
	.name			= "wii",
	.probe			= wii_probe,
	.setup_arch		= wii_setup_arch,
	.init_early		= wii_init_early,
	.show_cpuinfo		= wii_show_cpuinfo,
	.halt			= wii_halt,
	.init_IRQ		= flipper_pic_probe,
	.get_irq		= flipper_pic_get_irq,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
#ifdef CONFIG_KEXEC
	.machine_shutdown	= wii_shutdown,
	.machine_kexec_prepare	= wii_machine_kexec_prepare,
	.machine_kexec		= wii_machine_kexec,
#endif
};

