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
#define DRV_MODULE_NAME "wii"
#define pr_fmt(fmt) DRV_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/seq_file.h>
#include <linux/kexec.h>
#include <linux/of_platform.h>
#include <linux/memblock.h>
#include <mm/mmu_decl.h>
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
#include "hlwd-pic.h"
#include "gcnvi_udbg.h"
#include "usbgecko_udbg.h"

/* control block */
#define HW_CTRL_COMPATIBLE	"nintendo,hollywood-control"

#define HW_CTRL_RESETS		0x94
#define HW_CTRL_RESETS_SYS	(1<<0)

/* gpio */
#define HW_GPIO_COMPATIBLE	"nintendo,hollywood-gpio"

#define HW_GPIO_BASE(idx)	(idx * 0x20)
#define HW_GPIO_OUT(idx)	(HW_GPIO_BASE(idx) + 0)
#define HW_GPIO_DIR(idx)	(HW_GPIO_BASE(idx) + 4)

#define HW_GPIO_SHUTDOWN	(1<<1)
#define HW_GPIO_SLOT_LED	(1<<5)
#define HW_GPIO_SENSOR_BAR	(1<<8)


static void __iomem *hw_ctrl;
static void __iomem *hw_gpio;

unsigned long wii_hole_start;
unsigned long wii_hole_size;

static enum starlet_ipc_flavour starlet_ipc_flavour;


static int __init page_aligned(unsigned long x)
{
	return !(x & (PAGE_SIZE-1));
}

void __init wii_memory_fixups(void)
{
	struct memblock_region *p = memblock.memory.regions;

	/*
	 * This is part of a workaround to allow the use of two
	 * discontinuous RAM ranges on the Wii, even if this is
	 * currently unsupported on 32-bit PowerPC Linux.
	 *
	 * We coalesce the two memory ranges of the Wii into a
	 * single range, then create a reservation for the "hole"
	 * between both ranges.
	 */

	BUG_ON(memblock.memory.cnt != 2);
	BUG_ON(!page_aligned(p[0].base) || !page_aligned(p[1].base));

	/* trim unaligned tail */
	memblock_remove(ALIGN(p[1].base + p[1].size, PAGE_SIZE),
			(phys_addr_t)ULLONG_MAX);

	/* determine hole, add & reserve them */
	wii_hole_start = ALIGN(p[0].base + p[0].size, PAGE_SIZE);
	wii_hole_size = p[1].base - wii_hole_start;
	memblock_add(wii_hole_start, wii_hole_size);
	memblock_reserve(wii_hole_start, wii_hole_size);

	BUG_ON(memblock.memory.cnt != 1);
	__memblock_dump_all();

	/* allow ioremapping the address space in the hole */
	__allow_ioremap_reserved = 1;
}

unsigned long __init wii_mmu_mapin_mem2(unsigned long top)
{
	unsigned long delta, size, bl;
	unsigned long max_size = (256<<20);

	/* MEM2 64MB@0x10000000 */
	delta = wii_hole_start + wii_hole_size;
	size = top - delta;
	for (bl = 128<<10; bl < max_size; bl <<= 1) {
		if (bl * 2 > size)
			break;
	}
	setbat(4, PAGE_OFFSET+delta, delta, bl, PAGE_KERNEL_X);
	return delta + bl;
}

static void wii_spin(void)
{
	local_irq_disable();
	for (;;)
		cpu_relax();
}

static void __iomem *wii_ioremap_hw_regs(char *name, char *compatible)
{
	void __iomem *hw_regs = NULL;
	struct device_node *np;
	struct resource res;
	int error = -ENODEV;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np) {
		pr_err("no compatible node found for %s\n", compatible);
		goto out;
	}
	error = of_address_to_resource(np, 0, &res);
	if (error) {
		pr_err("no valid reg found for %s\n", np->name);
		goto out_put;
	}

	hw_regs = ioremap(res.start, resource_size(&res));
	if (hw_regs) {
		pr_info("%s at 0x%08x mapped to 0x%p\n", name,
			res.start, hw_regs);
	}

out_put:
	of_node_put(np);
out:
	return hw_regs;
}

static void __init wii_setup_arch(void)
{
	hw_ctrl = wii_ioremap_hw_regs("hw_ctrl", HW_CTRL_COMPATIBLE);
	hw_gpio = wii_ioremap_hw_regs("hw_gpio", HW_GPIO_COMPATIBLE);
	if (hw_gpio) {
		/* turn off the front blue led and IR light */
		clrbits32(hw_gpio + HW_GPIO_OUT(0),
			  HW_GPIO_SLOT_LED | HW_GPIO_SENSOR_BAR);
	}

	ug_udbg_init();
	gcnvi_udbg_init();
	starlet_discover_ipc_flavour();
}

#ifdef CONFIG_STARLET_IOS
static void wii_restart(char *cmd)
{
	local_irq_disable();

	/* try first to launch The Homebrew Channel... */
	starlet_es_reload_ios_and_launch(STARLET_TITLE_HBC_V107);
	starlet_es_reload_ios_and_launch(STARLET_TITLE_HBC_JODI);
	starlet_es_reload_ios_and_launch(STARLET_TITLE_HBC_HAXX);
	/* ..and if that fails, try an assisted restart */
	starlet_stm_restart();

	/* fallback to spinning until the power button pressed */
	for (;;)
		cpu_relax();
 }

static void wii_power_off(void)
{
	local_irq_disable();

	/* try an assisted poweroff */
	starlet_stm_power_off();

	/* fallback to spinning until the power button pressed */
	for (;;)
		cpu_relax();
}

#elif defined CONFIG_STARLET_MINI /* end of CONFIG_STARLET_IOS */
static void wii_restart(char *cmd)
{
	local_irq_disable();

	if (hw_ctrl) {
		/* clear the system reset pin to cause a reset */
		clrbits32(hw_ctrl + HW_CTRL_RESETS, HW_CTRL_RESETS_SYS);
	}
	wii_spin();
}

static void wii_power_off(void)
{
	local_irq_disable();

	if (hw_gpio) {
		/* make sure that the poweroff GPIO is configured as output */
		setbits32(hw_gpio + HW_GPIO_DIR(1), HW_GPIO_SHUTDOWN);

		/* drive the poweroff GPIO high */
		setbits32(hw_gpio + HW_GPIO_OUT(1), HW_GPIO_SHUTDOWN);
	}
	wii_spin();
}
#endif /* CONFIG_STARLET_MINI */

static void wii_halt(void)
{
	if (ppc_md.restart)
		ppc_md.restart(NULL);
	wii_spin();
}

static void __init wii_init_early(void)
{
	ug_udbg_init();
}

static void __init wii_pic_probe(void)
{
	flipper_pic_probe();
#ifdef CONFIG_HLWD_PIC
	hlwd_pic_probe();
#endif
}

static int __init wii_probe(void)
{
	unsigned long dt_root;

	dt_root = of_get_flat_dt_root();
	if (!of_flat_dt_is_compatible(dt_root, "nintendo,wii"))
		return 0;

	return 1;
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
	} else {
		starlet_ipc_flavour = STARLET_IPC_IOS;
	}

	ppc_md.restart = wii_restart;
	ppc_md.power_off = wii_power_off;

	return 0;
}

enum starlet_ipc_flavour starlet_get_ipc_flavour(void)
{
	return starlet_ipc_flavour;
}

#ifdef CONFIG_KEXEC

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


static void wii_shutdown(void)
{
#ifdef CONFIG_HLWD_PIC
	hlwd_quiesce();
#endif
	exi_quiesce();
	flipper_quiesce();
}

define_machine(wii) {
	.name			= "wii",
	.probe			= wii_probe,
	.init_early		= wii_init_early,
	.setup_arch		= wii_setup_arch,
	.restart		= wii_restart,
	.power_off		= wii_power_off,
	.show_cpuinfo		= wii_show_cpuinfo,
	.halt			= wii_halt,
	.init_IRQ		= wii_pic_probe,
	.get_irq		= flipper_pic_get_irq,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
	.machine_shutdown	= wii_shutdown,
#ifdef CONFIG_KEXEC	/* REMOVE THIS (as of 2.6.39)? */
	.machine_kexec_prepare	= wii_machine_kexec_prepare,
	.machine_kexec		= wii_machine_kexec,
#endif
};

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

