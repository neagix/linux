/*
 * drivers/usb/host/ohci-hlwd.c
 *
 * Nintendo Wii (Hollywood) USB Open Host Controller Interface.
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * Based on ohci-ppc-of.c
 *
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 * (C) Copyright 2002 Hewlett-Packard Company
 * (C) Copyright 2006 Sylvain Munaut <tnt@246tNt.com>
 *
 * Bus glue for OHCI HC on the of_platform bus
 *
 * Modified for of_platform bus from ohci-sa1111.c
 *
 * This file is licenced under the GPL.
 */

#include <linux/signal.h>
#include <linux/of_platform.h>

#include <asm/prom.h>
#include <asm/starlet.h>
#include <asm/time.h>	/* for get_tbl() */

#define DRV_MODULE_NAME "ohci-hlwd"
#define DRV_DESCRIPTION "Nintendo Wii OHCI Host Controller"
#define DRV_AUTHOR      "Albert Herranz"

#define HLWD_EHCI_CTL 0x0d0400cc	/* vendor control register */
#define HLWD_EHCI_CTL_OH0INTE	(1<<11)	/* oh0 interrupt enable */
#define HLWD_EHCI_CTL_OH1INTE	(1<<12)	/* oh1 interrupt enable */

#define __spin_event_timeout(condition, timeout_usecs, result, __end_tbl) \
        for (__end_tbl = get_tbl() + tb_ticks_per_usec * timeout_usecs; \
             !(result = (condition)) && (int)(__end_tbl - get_tbl()) > 0;)


static DEFINE_SPINLOCK(control_quirk_lock);

void ohci_hlwd_control_quirk(struct ohci_hcd *ohci)
{
	static struct ed *ed; /* empty ED */
	struct td *td; /* dummy TD */
	__hc32 head;
	__hc32 current;
	unsigned long ctx;
	int result;
	unsigned long flags;

	/*
	 * One time only.
	 * Allocate and keep a special empty ED with just a dummy TD.
	 */
	if (!ed) {
		ed = ed_alloc(ohci, GFP_ATOMIC);
		if (!ed)
			return;

		td = td_alloc(ohci, GFP_ATOMIC);
		if (!td) {
			ed_free(ohci, ed);
			ed = NULL;
			return;
		}

		ed->hwNextED = 0;
		ed->hwTailP = ed->hwHeadP = cpu_to_hc32(ohci,
							td->td_dma & ED_MASK);
		ed->hwINFO |= cpu_to_hc32(ohci, ED_OUT);
		wmb();
	}

	spin_lock_irqsave(&control_quirk_lock, flags);

	/*
	 * The OHCI USB host controllers on the Nintendo Wii
	 * video game console stop working when new TDs are
	 * added to a scheduled control ED after a transfer has
	 * has taken place on it.
	 *
	 * Before scheduling any new control TD, we make the
	 * controller happy by always loading a special control ED
	 * with a single dummy TD and letting the controller attempt
	 * the transfer.
	 * The controller won't do anything with it, as the special
	 * ED has no TDs, but it will keep the controller from failing
	 * on the next transfer.
	 */
	head = ohci_readl(ohci, &ohci->regs->ed_controlhead);
	if (head) {
		/*
		 * Load the special empty ED and tell the controller to
		 * process the control list.
		 */
		ohci_writel(ohci, ed->dma, &ohci->regs->ed_controlhead);
		ohci_writel (ohci, ohci->hc_control | OHCI_CTRL_CLE,
			     &ohci->regs->control);
		ohci_writel (ohci, OHCI_CLF, &ohci->regs->cmdstatus);

		/* spin until the controller is done with the control list  */
		current = ohci_readl(ohci, &ohci->regs->ed_controlcurrent);
		__spin_event_timeout(!current, 10 /* usecs */, result, ctx) {
			cpu_relax();
			current = ohci_readl(ohci,
					     &ohci->regs->ed_controlcurrent);
		}

		/* restore the old control head and control settings */
		ohci_writel (ohci, ohci->hc_control, &ohci->regs->control);
		ohci_writel(ohci, head, &ohci->regs->ed_controlhead);
	}

	spin_unlock_irqrestore(&control_quirk_lock, flags);
}

void ohci_hlwd_bulk_quirk(struct ohci_hcd *ohci)
{
	/*
	 * There seem to be issues too with the bulk list processing on the
	 * OHCI controller found in the Nintendo Wii video game console.
	 * The exact problem remains still unidentified, but adding a small
	 * delay seems to workaround it.
	 *
	 * As an example, without this quirk the wiimote controller stops
	 * responding after a few seconds because one of its bulk endpoint
	 * descriptors gets stuck.
	 */
	udelay(250);
}

static int ohci_hlwd_start(struct usb_hcd *hcd)
{
	struct ohci_hcd	*ohci = hcd_to_ohci(hcd);
	void __iomem *ehci_ctl;
	int error;

	error = ohci_init(ohci);
	if (error)
		goto out;

	ehci_ctl = ioremap(HLWD_EHCI_CTL, 4);
	if (!ehci_ctl) {
		printk(KERN_ERR __FILE__ ": ioremap failed\n");
		error = -EBUSY;
		ohci_stop(hcd);
		goto out;
	}

	/* enable notification of OHCI interrupts */
	out_be32(ehci_ctl, in_be32(ehci_ctl) |
		 0xe0000 | HLWD_EHCI_CTL_OH0INTE | HLWD_EHCI_CTL_OH1INTE);
	iounmap(ehci_ctl);

	error = ohci_run(ohci);
	if (error) {
		pr_err("can't start %s", ohci_to_hcd(ohci)->self.bus_name);
		ohci_stop(hcd);
		goto out;
	}

out:
	return error;
}

static const struct hc_driver ohci_hlwd_hc_driver = {
	.description =		hcd_name,
	.product_desc =		"Nintendo Wii OHCI Host Controller",
	.hcd_priv_size =	sizeof(struct ohci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq =			ohci_irq,
	.flags =		HCD_USB11 | HCD_BOUNCE_DMA_MEM,

	/*
	 * basic lifecycle operations
	 */
	.start =		ohci_hlwd_start,
	.stop =			ohci_stop,
	.shutdown = 		ohci_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue =		ohci_urb_enqueue,
	.urb_dequeue =		ohci_urb_dequeue,
	.endpoint_disable =	ohci_endpoint_disable,

	/*
	 * scheduling support
	 */
	.get_frame_number =	ohci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data =	ohci_hub_status_data,
	.hub_control =		ohci_hub_control,
#ifdef	CONFIG_PM
	.bus_suspend =		ohci_bus_suspend,
	.bus_resume =		ohci_bus_resume,
#endif
	.start_port_reset =	ohci_start_port_reset,
};


static int ohci_hcd_hlwd_probe(struct platform_device *op)
{
	struct device_node *dn = op->dev.of_node;
	struct usb_hcd *hcd;
	struct ohci_hcd	*ohci = NULL;
	struct resource res;
	dma_addr_t coherent_mem_addr;
	size_t coherent_mem_size;
	int irq;
	int error = -ENODEV;

	if (usb_disabled())
		goto out;

	if (starlet_get_ipc_flavour() != STARLET_IPC_MINI)
		goto out;

	dev_dbg(&op->dev, "initializing " DRV_MODULE_NAME " USB Controller\n");

	error = of_address_to_resource(dn, 0, &res);
	if (error)
		goto out;

	hcd = usb_create_hcd(&ohci_hlwd_hc_driver, &op->dev, DRV_MODULE_NAME);
	if (!hcd) {
		error = -ENOMEM;
		goto out;
	}

	hcd->rsrc_start = res.start;
	hcd->rsrc_len = resource_size(&res);

	error = of_address_to_resource(dn, 1, &res);
	if (error) {
		/* satisfy coherent memory allocations from mem1 or mem2 */
		dev_warn(&op->dev, "using normal memory\n");
	} else {
		coherent_mem_addr = res.start;
		coherent_mem_size = res.end - res.start + 1;
		if (!dma_declare_coherent_memory(&op->dev, coherent_mem_addr,
						 coherent_mem_addr,
						 coherent_mem_size,
						 DMA_MEMORY_MAP |
						 DMA_MEMORY_EXCLUSIVE)) {
			dev_err(&op->dev, "error declaring %u bytes of"
				" coherent memory at 0x%p\n",
				coherent_mem_size, (void *)coherent_mem_addr);
			error = -EBUSY;
			goto err_decl_coherent;
		}
	}

	irq = irq_of_parse_and_map(dn, 0);
	if (irq == NO_IRQ) {
		printk(KERN_ERR __FILE__ ": irq_of_parse_and_map failed\n");
		error = -EBUSY;
		goto err_irq;
	}

	hcd->regs = ioremap(hcd->rsrc_start, hcd->rsrc_len);
	if (!hcd->regs) {
		printk(KERN_ERR __FILE__ ": ioremap failed\n");
		error = -EBUSY;
		goto err_ioremap;
	}

	ohci = hcd_to_ohci(hcd);
	ohci->flags |= OHCI_QUIRK_WII;

	ohci_hcd_init(ohci);

	error = usb_add_hcd(hcd, irq, IRQF_DISABLED);
	if (error)
		goto err_add_hcd;

	return 0;

err_add_hcd:
	iounmap(hcd->regs);
err_ioremap:
	irq_dispose_mapping(irq);
err_irq:
	dma_release_declared_memory(&op->dev);
err_decl_coherent:
	usb_put_hcd(hcd);
out:
	return error;
}

static int ohci_hcd_hlwd_remove(struct platform_device *op)
{
	struct usb_hcd *hcd = dev_get_drvdata(&op->dev);

	dev_set_drvdata(&op->dev, NULL);

	dev_dbg(&op->dev, "stopping " DRV_MODULE_NAME " USB Controller\n");

	usb_remove_hcd(hcd);
	iounmap(hcd->regs);
	irq_dispose_mapping(hcd->irq);
	dma_release_declared_memory(&op->dev);
	usb_put_hcd(hcd);

	return 0;
}

static void ohci_hcd_hlwd_shutdown(struct platform_device *op)
{
	struct usb_hcd *hcd = dev_get_drvdata(&op->dev);

	if (hcd->driver->shutdown)
		hcd->driver->shutdown(hcd);


}


static struct of_device_id ohci_hcd_hlwd_match[] = {
	{
		.compatible = "nintendo,hollywood-usb-ohci",
	},
	{},
};
MODULE_DEVICE_TABLE(of, ohci_hcd_hlwd_match);

static struct platform_driver ohci_hcd_hlwd_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = ohci_hcd_hlwd_match,
	},
	.probe		= ohci_hcd_hlwd_probe,
	.remove		= ohci_hcd_hlwd_remove,
	.shutdown 	= ohci_hcd_hlwd_shutdown,
};

