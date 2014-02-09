/*
 * drivers/block/gcn-aram.c
 *
 * Nintendo GameCube Auxiliary RAM (ARAM) block driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2005,2007,2008,2009 Albert Herranz
 *
 * Based on previous work by Franz Lehner.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/blkdev.h>
#include <linux/dma-mapping.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/interrupt.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/slab.h>


#define DRV_MODULE_NAME "gcn-aram"
#define DRV_DESCRIPTION "Nintendo GameCube Auxiliary RAM (ARAM) block driver"
#define DRV_AUTHOR      "Todd Jeffreys <todd@voidpointer.org>, " \
			"Albert Herranz"

static char aram_driver_version[] = "4.0i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)


/*
 * Hardware.
 */
#define ARAM_DMA_ALIGN		0x1f	/* 32 bytes */

#define DSP_CSR			0x00a
#define  DSP_CSR_RES		(1<<0)
#define  DSP_CSR_PIINT		(1<<1)
#define  DSP_CSR_HALT		(1<<2)
#define  DSP_CSR_AIDINT		(1<<3)
#define  DSP_CSR_AIDINTMASK	(1<<4)
#define  DSP_CSR_ARINT		(1<<5)
#define  DSP_CSR_ARINTMASK	(1<<6)
#define  DSP_CSR_DSPINT		(1<<7)
#define  DSP_CSR_DSPINTMASK	(1<<8)
#define  DSP_CSR_DSPDMA		(1<<9)
#define  DSP_CSR_RESETXXX	(1<<11)

#define AR_SIZE			0x012

#define AR_MODE			0x016
#define   AR_MODE_ACCELERATOR	(1 << 0)

#define AR_REFRESH		0x01a

#define AR_DMA_MMADDR		0x020

#define AR_DMA_ARADDR		0x024

#define AR_DMA_CNT_H		0x028
#define   AR_READ		(1 << 31)
#define   AR_WRITE		0

#define AR_DMA_CNT_L		0x02a

#define AR_DMA_CNT		AR_DMA_CNT_H

/*
 * Driver settings
 */
#define ARAM_NAME		DRV_MODULE_NAME
#define ARAM_MAJOR		Z2RAM_MAJOR /* we share the major */

#define ARAM_SECTOR_SIZE	PAGE_SIZE

#define ARAM_BUFFERSIZE		(16*1024*1024)

/*
 * Driver data.
 */
struct aram_drvdata {
	spinlock_t			queue_lock;

	spinlock_t			io_lock;
	void __iomem			*io_base;
	int				irq;

	struct block_device_operations	fops;
	struct gendisk			*disk;
	struct request_queue		*queue;

	struct request			*req;	/* protected by ->io_lock */
	dma_addr_t			dma_addr;
	size_t				dma_len;

	int				ref_count;

	struct device			*dev;
};


static inline enum dma_data_direction rq_dir_to_dma_dir(struct request *req)
{
	if (rq_data_dir(req) == READ)
		return DMA_FROM_DEVICE;
	else
		return DMA_TO_DEVICE;
}

static inline int rq_dir_to_aram_dir(struct request *req)
{
	if (rq_data_dir(req) == READ)
		return AR_READ;
	else
		return AR_WRITE;
}

static void aram_start_dma_transfer(struct aram_drvdata *drvdata,
				    unsigned long aram_addr)
{
	void __iomem *io_base = drvdata->io_base;
	dma_addr_t dma_addr = drvdata->dma_addr;
	size_t dma_len = drvdata->dma_len;

	/* DMA transfers require proper alignment */
	BUG_ON((dma_addr & ARAM_DMA_ALIGN) != 0 ||
	       (dma_len & ARAM_DMA_ALIGN) != 0);

	out_be32(io_base + AR_DMA_MMADDR, dma_addr);
	out_be32(io_base + AR_DMA_ARADDR, aram_addr);

	/* writing the low-word kicks off the DMA */
	out_be32(io_base + AR_DMA_CNT,
		 rq_dir_to_aram_dir(drvdata->req) | dma_len);
}

static irqreturn_t aram_irq_handler(int irq, void *dev0)
{
	struct aram_drvdata *drvdata = dev0;
	struct request *req;
	u16 __iomem *csr_reg = drvdata->io_base + DSP_CSR;
	u16 csr;
	unsigned long flags;

	spin_lock_irqsave(&drvdata->io_lock, flags);

	csr = in_be16(csr_reg);

	/*
	 * Do nothing if the interrupt is not targetted for us.
	 * We share this interrupt with the sound driver.
	 */
	if (!(csr & DSP_CSR_ARINT)) {
		spin_unlock_irqrestore(&drvdata->io_lock, flags);
		return IRQ_NONE;
	}

	/* strictly ack the ARAM interrupt, and nothing more */
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT);
	out_be16(csr_reg, csr);

	/* pick up request in service */
	req = drvdata->req;
	drvdata->req = NULL;
	if (drvdata->dma_len) {
		dma_unmap_single(drvdata->dev,
				 drvdata->dma_addr, drvdata->dma_len,
				 rq_dir_to_dma_dir(req));
		drvdata->dma_len = 0;
	}

	spin_unlock_irqrestore(&drvdata->io_lock, flags);

	if (!req) {
		drv_printk(KERN_ERR, "ignoring interrupt, no request\n");
		goto out;
	}

	spin_lock(&drvdata->queue_lock);
	__blk_end_request_cur(req, 0);
	blk_start_queue(drvdata->queue);
	spin_unlock(&drvdata->queue_lock);

out:
	return IRQ_HANDLED;
}

static void aram_do_request(struct request_queue *q)
{
	struct aram_drvdata *drvdata = q->queuedata;
	struct request *req;
	unsigned long aram_addr;
	size_t len;
	unsigned long flags;
	int error;

	req = blk_peek_request(q);
	while (req) {
		spin_lock_irqsave(&drvdata->io_lock, flags);
		if (drvdata->req) {
			blk_stop_queue(q);
			spin_unlock_irqrestore(&drvdata->io_lock, flags);
			break;
		}

		blk_start_request(req);
		error = -EIO;

		if (req->cmd_type != REQ_TYPE_FS)
			goto done;

		/* calculate the ARAM address and length */
		aram_addr = blk_rq_pos(req) << 9;
		len = blk_rq_cur_bytes(req);

		/* give up if the request goes out of bounds */
		if (aram_addr + len > ARAM_BUFFERSIZE) {
			drv_printk(KERN_ERR, "bad access: block=%lu,"
				   " size=%u\n", (unsigned long)blk_rq_pos(req),
				    len);
			goto done;
		}

		drvdata->req = req;
		spin_unlock_irqrestore(&drvdata->io_lock, flags);

		/* perform DMA mappings and start the transfer */
		drvdata->dma_len = len;
		drvdata->dma_addr = dma_map_single(drvdata->dev,
						   req->buffer, len,
						   rq_dir_to_dma_dir(req));
		aram_start_dma_transfer(drvdata, aram_addr);

		/* one request at a time */
		break;
	done:
		spin_unlock_irqrestore(&drvdata->io_lock, flags);
		if (!__blk_end_request_cur(req, error))
			req = blk_peek_request(q);
	}
}

/*
 * Block device hooks.
 *
 */

static int aram_open(struct block_device *bdev, fmode_t mode)
{
	struct aram_drvdata *drvdata = bdev->bd_disk->private_data;
	unsigned long flags;
	int retval = 0;

	spin_lock_irqsave(&drvdata->queue_lock, flags);

	/* only allow a minor of 0 to be opened */
	if (MINOR(bdev->bd_dev)) {
		retval =  -ENODEV;
		goto out;
	}

	/* honor exclusive open mode */
	if (drvdata->ref_count == -1 ||
	    (drvdata->ref_count && (mode & FMODE_EXCL))) {
		retval = -EBUSY;
		goto out;
	}

	if ((mode & FMODE_EXCL))
		drvdata->ref_count = -1;
	else
		drvdata->ref_count++;

out:
	spin_unlock_irqrestore(&drvdata->queue_lock, flags);
	return retval;
}

static void aram_release(struct gendisk *disk, fmode_t mode)
{
	struct aram_drvdata *drvdata = disk->private_data;
	unsigned long flags;

	spin_lock_irqsave(&drvdata->queue_lock, flags);
	if (drvdata->ref_count > 0)
		drvdata->ref_count--;
	else
		drvdata->ref_count = 0;
	spin_unlock_irqrestore(&drvdata->queue_lock, flags);
}

static int aram_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	geo->cylinders = get_capacity(bdev->bd_disk) / (4 * 16);
	geo->heads = 4;
	geo->sectors = 16;
	return 0;
}

static struct block_device_operations aram_fops = {
	.owner = THIS_MODULE,
	.open = aram_open,
	.release = aram_release,
	.getgeo = aram_getgeo,
};


/*
 * Setup routines.
 *
 */

static int aram_init_blk_dev(struct aram_drvdata *drvdata)
{
	struct gendisk *disk;
	struct request_queue *queue;
	int retval;

	drvdata->ref_count = 0;

	retval = register_blkdev(ARAM_MAJOR, ARAM_NAME);
	if (retval)
		goto err_register_blkdev;

	retval = -ENOMEM;
	spin_lock_init(&drvdata->queue_lock);
	spin_lock_init(&drvdata->io_lock);
	queue = blk_init_queue(aram_do_request, &drvdata->queue_lock);
	if (!queue)
		goto err_blk_init_queue;

	blk_queue_logical_block_size(queue, ARAM_SECTOR_SIZE);
	blk_queue_dma_alignment(queue, ARAM_DMA_ALIGN);
	blk_queue_max_segments(queue, 1);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, queue);
	queue->queuedata = drvdata;
	drvdata->queue = queue;

	disk = alloc_disk(1);
	if (!disk)
		goto err_alloc_disk;

	disk->major = ARAM_MAJOR;
	disk->first_minor = 0;
	disk->fops = &aram_fops;
	strcpy(disk->disk_name, ARAM_NAME);
	disk->queue = drvdata->queue;
	set_capacity(disk, ARAM_BUFFERSIZE >> 9);
	disk->private_data = drvdata;
	drvdata->disk = disk;

	add_disk(drvdata->disk);

	retval = 0;
	goto out;

err_alloc_disk:
	blk_cleanup_queue(drvdata->queue);
err_blk_init_queue:
	unregister_blkdev(ARAM_MAJOR, ARAM_NAME);
err_register_blkdev:
out:
	return retval;
}

static void aram_exit_blk_dev(struct aram_drvdata *drvdata)
{
	if (drvdata->disk) {
		del_gendisk(drvdata->disk);
		put_disk(drvdata->disk);
	}
	if (drvdata->queue)
		blk_cleanup_queue(drvdata->queue);
	unregister_blkdev(ARAM_MAJOR, ARAM_NAME);
}

static void aram_quiesce(struct aram_drvdata *drvdata)
{
	u16 __iomem *csr_reg = drvdata->io_base + DSP_CSR;
	u16 csr;
	unsigned long flags;

	/*
	 * Disable ARAM interrupts, but do not accidentally ack non-ARAM ones.
	 */
	spin_lock_irqsave(&drvdata->io_lock, flags);
	csr = in_be16(csr_reg);
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT | DSP_CSR_ARINTMASK);
	out_be16(csr_reg, csr);
	spin_unlock_irqrestore(&drvdata->io_lock, flags);

	/* wait until pending transfers are finished */
	while (in_be16(csr_reg) & DSP_CSR_DSPDMA)
		cpu_relax();
}

static int aram_init_irq(struct aram_drvdata *drvdata)
{
	u16 __iomem *csr_reg = drvdata->io_base + DSP_CSR;
	u16 csr;
	unsigned long flags;
	int retval;

	retval = request_irq(drvdata->irq, aram_irq_handler,
			     IRQF_DISABLED | IRQF_SHARED,
			     DRV_MODULE_NAME, drvdata);
	if (retval) {
		drv_printk(KERN_ERR, "request of IRQ %d failed\n",
			   drvdata->irq);
		goto out;
	}

	/*
	 * Enable ARAM interrupts, and route them to the processor.
	 * Make sure to preserve the AI and DSP interrupts.
	 */
	spin_lock_irqsave(&drvdata->io_lock, flags);
	csr = in_be16(csr_reg);
	csr |= (DSP_CSR_ARINT | DSP_CSR_ARINTMASK | DSP_CSR_PIINT);
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT);
	out_be16(csr_reg, csr);
	spin_unlock_irqrestore(&drvdata->io_lock, flags);

out:
	return retval;
}

static void aram_exit_irq(struct aram_drvdata *drvdata)
{
	aram_quiesce(drvdata);

	free_irq(drvdata->irq, drvdata);
}

static int aram_init(struct aram_drvdata *drvdata,
		     struct resource *mem, int irq)
{
	int retval;

	drvdata->io_base = ioremap(mem->start, mem->end - mem->start + 1);
	drvdata->irq = irq;

	retval = aram_init_blk_dev(drvdata);
	if (!retval) {
		retval = aram_init_irq(drvdata);
		if (retval)
			aram_exit_blk_dev(drvdata);
	}
	return retval;
}

static void aram_exit(struct aram_drvdata *drvdata)
{
	aram_exit_blk_dev(drvdata);
	aram_exit_irq(drvdata);
	if (drvdata->io_base) {
		iounmap(drvdata->io_base);
		drvdata->io_base = NULL;
	}
}

/*
 * Driver model helper routines.
 *
 */

static int aram_do_probe(struct device *dev, struct resource *mem,
			 int irq)
{
	struct aram_drvdata *drvdata;
	int retval;

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		drv_printk(KERN_ERR, "failed to allocate aram_drvdata\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, drvdata);
	drvdata->dev = dev;

	retval = aram_init(drvdata, mem, irq);
	if (retval) {
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
	}
	return retval;
}

static int aram_do_remove(struct device *dev)
{
	struct aram_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata) {
		aram_exit(drvdata);
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
		return 0;
	}
	return -ENODEV;
}

static int aram_do_shutdown(struct device *dev)
{
	struct aram_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata)
		aram_quiesce(drvdata);
	return 0;
}


/*
 * OF platform device routines.
 *
 */

static int aram_of_probe(struct platform_device *odev)

{
	struct resource res;
	int retval;

	retval = of_address_to_resource(odev->dev.of_node, 0, &res);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return -ENODEV;
	}

	return aram_do_probe(&odev->dev,
			     &res, irq_of_parse_and_map(odev->dev.of_node, 0));
}

static int __exit aram_of_remove(struct platform_device *odev)
{
	return aram_do_remove(&odev->dev);
}

static void aram_of_shutdown(struct platform_device *odev)
{
	aram_do_shutdown(&odev->dev);
}


static struct of_device_id aram_of_match[] = {
	{ .compatible = "nintendo,flipper-auxram" },
	{ },
};


MODULE_DEVICE_TABLE(of, aram_of_match);

static struct platform_driver aram_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = aram_of_match,
	},
	.probe = aram_of_probe,
	.remove = aram_of_remove,
	.shutdown = aram_of_shutdown,
};

/*
 * Module interfaces.
 *
 */

static int __init aram_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   aram_driver_version);

	return platform_driver_register(&aram_of_driver);
}

static void __exit aram_exit_module(void)
{
	platform_driver_unregister(&aram_of_driver);
}

module_init(aram_init_module);
module_exit(aram_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

