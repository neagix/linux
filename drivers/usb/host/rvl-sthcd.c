/*
 * drivers/usb/host/rvl-sthcd.c
 *
 * USB Host Controller driver for the Nintendo Wii
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008 Maarten ter Huurne
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 *
 * TODO
 * - cleanup debuging mess
 *
 */

#ifdef CONFIG_HIGHMEM
#error Sorry, this driver cannot currently work if HIGHMEM is y
#endif

#define DBG(fmt, arg...)	drv_printk(KERN_DEBUG, fmt, ##arg)

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/usb.h>
#include <asm/starlet.h>
#include <asm/starlet-ios.h>

#include "../core/hcd.h"
#include "../core/hub.h"

#define DRV_MODULE_NAME "rvl-sthcd"
#define DRV_DESCRIPTION "USB Host Controller driver for the Nintendo Wii"
#define DRV_AUTHOR      "Maarten ter Huurne, " \
			"Albert Herranz"

static char sthcd_driver_version[] = "0.5i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)


/* TODO: Use enum instead? */
#define STHCD_IOCTLV_CONTROLREQ		0
#define STHCD_IOCTLV_BULKREQ		1
#define STHCD_IOCTLV_INTRREQ		2
#define STHCD_IOCTL_SUSPENDDEVICE	5
#define STHCD_IOCTL_RESUMEDEVICE	6
#define STHCD_IOCTLV_GETDEVICELIST	12
#define STHCD_IOCTL_DEVICEREMOVALNOTIFY	26
#define STHCD_IOCTLV_DEVICEINSERTNOTIFY	27

/*
 * The Nintendo Wii has only 2 external USB ports (plus 1 internal USB port),
 * but the starlet API provides access to USB devices in a port independent
 * way. This is true also for USB devices attached to external hubs.
 *
 * Our HCD model currently maps one starlet USB device to one HCD port, thus
 * we need additional ports here.
 */
#define STHCD_MAX_DEVIDS	15
#define STHCD_MAX_PORTS		STHCD_MAX_DEVIDS

/*
 * We get error -7008 after performing large transfers.
 * Using this arbitrary limit makes things work.
 */
#define STHCD_MAX_CHUNK_SIZE	(2048)

#define STHCD_PORT_MAX_RESETS	2	/* maximum number of consecutive
					 * resets allowed for a port */
#define STHCD_RESCAN_INTERVAL	5	/* seconds */

#define starlet_ioh_sg_entry(sg, ptr) \
	starlet_ioh_sg_set_buf((sg), (ptr), sizeof(*(ptr)))


struct sthcd_hcd;
struct sthcd_port;
struct sthcd_oh;

/*
 * starlet USB device abstraction (udev).
 *
 */
struct sthcd_udev {
	u16 idVendor;
	u16 idProduct;
	int fd;				/* starlet file descriptor */

	u16 devnum;			/* USB address set by kernel */

	struct list_head node;		/* in list of connected devices */
	struct sthcd_oh *oh;		/* parent Open Host controller */

	struct list_head pep_list;	/* list of private endpoints */
};

/*
 * starlet USB device identifier.
 *
 */
struct sthcd_devid {
	u32 _unk1;
	u16 idVendor;
	u16 idProduct;
};

enum {
	__STHCD_PORT_INUSE = 0,
	__STHCD_PORT_DOOMED,
};


/*
 * "Virtual" HCD USB port.
 *
 */
struct sthcd_port {
	unsigned long flags;
#define STHCD_PORT_INUSE	(1 << __STHCD_PORT_INUSE)
#define STHCD_PORT_DOOMED	(1 << __STHCD_PORT_DOOMED)

	u32 status_change;
	unsigned nr_resets;

	struct sthcd_udev udev;	/* one udev per port */
};

/*
 * starlet Open Host controller abstraction (oh).
 *
 */
struct sthcd_oh {
	unsigned int index;
	int fd;					/* starlet file descriptor */

	unsigned int max_devids;
	struct sthcd_devid *new_devids;
	struct sthcd_devid *devids;
	unsigned int nr_devids;			/* actual no of devices */

	struct sthcd_hcd *hcd;			/* parent Host Controller */
};

/*
 * Host Controller (hcd).
 *
 */
struct sthcd_hcd {
	spinlock_t lock;

	struct sthcd_oh oh[2];

	struct sthcd_port *ports;	/* array of ports */
	unsigned int nr_ports;

	struct list_head device_list;	/* list of connected devices */

	wait_queue_head_t rescan_waitq;	/* wait queue for the rescan task */
	struct task_struct *rescan_task;
};


/*
 * Private endpoint (pep).
 *
 * A pep takes care of the transfers for an endpoint.
 */

struct sthcd_ctrl_params_in {
	struct usb_ctrlrequest req;
	u8 _unk1; /* timeout? */
};
struct sthcd_ctrl_xfer_ctx {
	struct starlet_ioh_sg in[6];
	struct sthcd_ctrl_params_in *params_in;
};

struct sthcd_bulk_intr_params_in {
	u8 bEndpointAddress;
	u16 wLength;
};
struct sthcd_bulk_intr_xfer_ctx {
	struct starlet_ioh_sg in[2];
	struct sthcd_bulk_intr_params_in *params_in;
};

enum {
	__STHCD_PEP_DISABLED = 0,
	__STHCD_PEP_XFERBUSY,		/* pep is actively xferring data */
};

struct sthcd_pep {
	unsigned long flags;
#define STHCD_PEP_DISABLED	(1 << __STHCD_PEP_DISABLED)
#define STHCD_PEP_XFERBUSY	(1 << __STHCD_PEP_XFERBUSY)

	unsigned long outstanding;

	struct usb_host_endpoint *ep;	/* associated endpoint */
	struct sthcd_hcd *sthcd;	/* associated hcd */

	/* local copy of endpoint descriptor bmAttributes */
	__u8	bmAttributes;

	/* xfer context data */

	struct urb *urb;		/* urb being transferred */

	struct sthcd_udev *udev;	/* udev for this urb */
	struct list_head node;		/* in list of peps for this udev */

	size_t io_xfer_offset;		/* number of bytes transferred */
	void *io_buf;			/* data buffer */
	size_t io_buf_len;		/* length of io_buf */

	int request;			/* ioctlv request */
	union {
		struct sthcd_bulk_intr_xfer_ctx *bulk_intr;
		struct sthcd_ctrl_xfer_ctx *ctrl;
	} ctx;				/* transfer context */

	unsigned int nents_in;		/* number of input sg entries */
	struct starlet_ioh_sg *in;	/* input sg list */
	struct starlet_ioh_sg io[1];	/* input/output sg list */
};


/*
 * Debugging facilities.
 *
 */

#if 0
static inline void print_buffer(void *buf, u32 size)
{
	int i;
	for (i = 0; i < (size + 3) / 4; i += 4) {
		u32 *data = &((u32 *)buf)[i];
		printk(KERN_INFO "  %08X %08X %08X %08X\n",
			data[0], data[1], data[2], data[3]
			);
	}
}
#endif

/*
 * Type conversion routines.
 *
 */

static inline struct sthcd_hcd *hcd_to_sthcd(struct usb_hcd *hcd)
{
	return (struct sthcd_hcd *)(hcd->hcd_priv);
}

static inline struct usb_hcd *sthcd_to_hcd(struct sthcd_hcd *sthcd)
{
	return container_of((void *)sthcd, struct usb_hcd, hcd_priv);
}

static inline struct sthcd_port *udev_to_port(struct sthcd_udev *_udev)
{
	return container_of(_udev, struct sthcd_port, udev);
}


/*
 * Private End Point abstraction.
 *
 */

static inline struct sthcd_pep *ep_to_pep(struct usb_host_endpoint *ep)
{
	return ep->hcpriv;
}

static inline int pep_is_enabled(struct sthcd_pep *pep)
{
	return !test_bit(__STHCD_PEP_DISABLED, &pep->flags);
}

static int sthcd_pep_alloc_ctrl_xfer_ctx(struct sthcd_pep *pep)
{
	struct sthcd_ctrl_xfer_ctx *ctx;
	struct sthcd_ctrl_params_in *params_in;
	int error;

	ctx = starlet_kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx) {
		error = -ENOMEM;
		goto done;
	}

	params_in = starlet_ioh_kzalloc(sizeof(*params_in));
	if (!params_in) {
		starlet_kfree(ctx);
		error = -ENOMEM;
		goto done;
	}

	ctx->params_in = params_in;

	starlet_ioh_sg_init_table(ctx->in, 6);
	starlet_ioh_sg_entry(&ctx->in[0], &params_in->req.bRequestType);
	starlet_ioh_sg_entry(&ctx->in[1], &params_in->req.bRequest);
	starlet_ioh_sg_entry(&ctx->in[2], &params_in->req.wValue);
	starlet_ioh_sg_entry(&ctx->in[3], &params_in->req.wIndex);
	starlet_ioh_sg_entry(&ctx->in[4], &params_in->req.wLength);
	starlet_ioh_sg_entry(&ctx->in[5], &params_in->_unk1);

	pep->ctx.ctrl = ctx;

	pep->nents_in = ARRAY_SIZE(ctx->in);
	pep->in = ctx->in;

	error = 0;

done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

static void sthcd_pep_free_ctrl_xfer_ctx(struct sthcd_pep *pep)
{
	struct sthcd_ctrl_xfer_ctx *ctx = pep->ctx.ctrl;

	if (ctx) {
		starlet_ioh_kfree(ctx->params_in);
		starlet_kfree(ctx);
		pep->ctx.ctrl = NULL;
	}
}

static int sthcd_pep_alloc_bulk_intr_xfer_ctx(struct sthcd_pep *pep)
{
	struct sthcd_bulk_intr_xfer_ctx *ctx;
	struct sthcd_bulk_intr_params_in *params_in;
	int error;

	ctx = starlet_kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx) {
		error = -ENOMEM;
		goto done;
	}

	params_in = starlet_ioh_kzalloc(sizeof(*params_in));
	if (!params_in) {
		starlet_kfree(ctx);
		error = -ENOMEM;
		goto done;
	}

	ctx->params_in = params_in;

	starlet_ioh_sg_init_table(ctx->in, 2);
	starlet_ioh_sg_entry(&ctx->in[0], &params_in->bEndpointAddress);
	starlet_ioh_sg_entry(&ctx->in[1], &params_in->wLength);

	pep->ctx.bulk_intr = ctx;

	pep->nents_in = ARRAY_SIZE(ctx->in);
	pep->in = ctx->in;

	error = 0;

done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

static void sthcd_pep_free_bulk_intr_xfer_ctx(struct sthcd_pep *pep)
{
	struct sthcd_bulk_intr_xfer_ctx *ctx = pep->ctx.bulk_intr;

	if (ctx) {
		starlet_ioh_kfree(ctx->params_in);
		starlet_kfree(ctx);
		pep->ctx.bulk_intr = NULL;
	}
}

static int sthcd_pep_alloc_xfer_ctx(struct sthcd_pep *pep)
{
	unsigned int xfer_type = pep->bmAttributes &
					 USB_ENDPOINT_XFERTYPE_MASK;
	int error;

	switch (xfer_type) {
	case USB_ENDPOINT_XFER_CONTROL:
		error = sthcd_pep_alloc_ctrl_xfer_ctx(pep);
		break;
	case USB_ENDPOINT_XFER_BULK:
	case USB_ENDPOINT_XFER_INT:
		error = sthcd_pep_alloc_bulk_intr_xfer_ctx(pep);
		break;
	default:
		error = -ENXIO;
		break;
	}

	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

static void sthcd_pep_free_xfer_ctx(struct sthcd_pep *pep)
{
	unsigned int xfer_type = pep->bmAttributes &
					 USB_ENDPOINT_XFERTYPE_MASK;

	switch (xfer_type) {
	case USB_ENDPOINT_XFER_CONTROL:
		sthcd_pep_free_ctrl_xfer_ctx(pep);
		break;
	case USB_ENDPOINT_XFER_BULK:
	case USB_ENDPOINT_XFER_INT:
		sthcd_pep_free_bulk_intr_xfer_ctx(pep);
		break;
	default:
		DBG("%s: invalid endpoint xfer type %u\n", __func__,
			xfer_type);
		break;
	}
}

static int sthcd_pep_alloc_xfer_io_buf(struct sthcd_pep *pep, size_t size)
{
	/* REVISIT, size must be greater than 0 */
	size_t io_buf_size = size + 32;
	int error;

	pep->io_buf = starlet_ioh_kzalloc(io_buf_size);
	if (!pep->io_buf) {
		error = -ENOMEM;
		goto done;
	}

	starlet_ioh_sg_init_table(pep->io, 1);
	starlet_ioh_sg_set_buf(&pep->io[0], pep->io_buf, size);

	error = 0;

done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

static void sthcd_pep_free_xfer_io_buf(struct sthcd_pep *pep)
{
	if (pep->io_buf) {
		starlet_ioh_sg_set_buf(&pep->io[0], NULL, 0);
		starlet_ioh_kfree(pep->io_buf);
		pep->io_buf = NULL;
	}
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_init(struct sthcd_pep *pep, struct sthcd_hcd *sthcd,
			  struct usb_host_endpoint *ep)
{
	int error;

	BUG_ON(!ep);

	pep->sthcd = sthcd;
	pep->ep = ep;
	pep->bmAttributes = ep->desc.bmAttributes;

	error = sthcd_pep_alloc_xfer_ctx(pep);
	if (error)
		goto done;

done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static void sthcd_pep_exit(struct sthcd_pep *pep)
{
	BUG_ON(pep->urb);
	BUG_ON(!pep->ep);

	sthcd_pep_free_xfer_ctx(pep);

	pep->ep = NULL;
	pep->sthcd = NULL;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static struct sthcd_pep *sthcd_pep_alloc(struct sthcd_hcd *sthcd,
					 struct usb_host_endpoint *ep)
{
	struct sthcd_pep *pep;
	int error;

	pep = kzalloc(sizeof(*pep), GFP_ATOMIC);
	if (!pep)
		return NULL;

	error = sthcd_pep_init(pep, sthcd, ep);
	if (error) {
		kfree(pep);
		return NULL;
	}

	return pep;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static void sthcd_pep_free(struct sthcd_pep *pep)
{
	sthcd_pep_exit(pep);
	kfree(pep);
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static struct sthcd_udev *sthcd_find_udev_by_num(struct sthcd_hcd *sthcd,
						 u16 devnum)
{
	struct sthcd_udev *udev;

	list_for_each_entry(udev, &sthcd->device_list, node) {
		if (udev->devnum == devnum)
			return udev;
	}
	DBG("%s: udev %u not found\n", __func__, devnum);
	return NULL;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_takein_urb(struct sthcd_pep *pep, struct urb *urb)
{
	struct sthcd_hcd *sthcd = pep->sthcd;
	struct sthcd_udev *udev;
	int error = 0;

	if (!pep_is_enabled(pep)) {
		error = -ESHUTDOWN;
		goto done;
	}

	if (pep->urb) {
		error = -EBUSY;
		goto done;
	}

	if (unlikely(!pep->udev)) {
		BUG_ON(!urb->dev);
		udev = sthcd_find_udev_by_num(sthcd, urb->dev->devnum);
		if (!udev) {
			error = -ENODEV;
			goto done;
		}
		pep->udev = udev;
		list_add_tail(&pep->node, &udev->pep_list);
	}

	pep->urb = urb;
done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static void sthcd_pep_takeout_urb(struct sthcd_pep *pep)
{
	WARN_ON(!pep->urb);

	pep->urb = NULL;
	if (pep->udev)
		list_del_init(&pep->node);
	pep->udev = NULL;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static void sthcd_pep_setup_ctrl_xfer(struct sthcd_pep *pep)
{
	struct urb *urb = pep->urb;
	struct sthcd_ctrl_xfer_ctx *ctx = pep->ctx.ctrl;
	struct sthcd_ctrl_params_in *params_in;

	params_in = ctx->params_in;
	memcpy(&params_in->req, urb->setup_packet, sizeof(params_in->req));
	params_in->req.wLength = cpu_to_le16(pep->io_buf_len);
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static void sthcd_pep_setup_bulk_intr_xfer(struct sthcd_pep *pep)
{
	struct urb *urb = pep->urb;
	struct sthcd_bulk_intr_xfer_ctx *ctx = pep->ctx.bulk_intr;
	struct sthcd_bulk_intr_params_in *params_in;

	params_in = ctx->params_in;
	params_in->bEndpointAddress = urb->ep->desc.bEndpointAddress;
	params_in->wLength = pep->io_buf_len;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_setup_xfer(struct sthcd_pep *pep)
{
	struct urb *urb = pep->urb;
	int request;
	int error = 0;

	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:
		request = STHCD_IOCTLV_CONTROLREQ;
		sthcd_pep_setup_ctrl_xfer(pep);
		break;
	case PIPE_INTERRUPT:
		request = STHCD_IOCTLV_INTRREQ;
		sthcd_pep_setup_bulk_intr_xfer(pep);
		break;
	case PIPE_BULK:
		request = STHCD_IOCTLV_BULKREQ;
		sthcd_pep_setup_bulk_intr_xfer(pep);
		break;
	default:
		error = -EINVAL;
		break;
	}

	if (!error) {
		pep->request = request;
		starlet_ioh_sg_set_buf(&pep->io[0],
					pep->io_buf, pep->io_buf_len);
	}

	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_setup_next_xfer(struct sthcd_pep *pep)
{
	struct urb *urb = pep->urb;
	int retval = 0;
	int error;

	if (pep->io_xfer_offset < urb->transfer_buffer_length) {
		pep->io_buf_len = urb->transfer_buffer_length -
					 pep->io_xfer_offset;
		if (pep->io_buf_len > STHCD_MAX_CHUNK_SIZE)
			pep->io_buf_len = STHCD_MAX_CHUNK_SIZE;

		retval = pep->io_buf_len;

		error = sthcd_pep_setup_xfer(pep);
		if (error)
			retval = error;
	}

	if (retval < 0)
		DBG("%s: retval=%d (%x)\n", __func__, retval, retval);
	return retval;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_setup_first_xfer(struct sthcd_pep *pep)
{
	struct urb *urb = pep->urb;
	int retval;
	int error;

	pep->io_xfer_offset = 0;
	pep->io_buf_len = urb->transfer_buffer_length;
	if (pep->io_buf_len > STHCD_MAX_CHUNK_SIZE)
		pep->io_buf_len = STHCD_MAX_CHUNK_SIZE;

	retval = pep->io_buf_len;

	error = sthcd_pep_setup_xfer(pep);
	if (error)
		retval = error;

	if (retval < 0)
		DBG("%s: retval=%d (%x)\n", __func__, retval, retval);
	return retval;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static void sthcd_pep_finish_xfer(struct sthcd_pep *pep, int xfer_len)
{
	struct urb *urb = pep->urb;

	if (xfer_len <= 0)
		goto done;

	BUG_ON(!urb);
	BUG_ON(!pep->io_buf);

	/*
	 * For IN transfers, copy the received chunk data into the urb
	 * xfer buffer.
	 */
	if (usb_urb_dir_in(urb)) {
		/* device -> host */
		BUG_ON(!urb->transfer_buffer);
		memcpy(urb->transfer_buffer + pep->io_xfer_offset,
			       pep->io_buf, xfer_len);
	}

	pep->io_xfer_offset += xfer_len;

done:
	return;
}

static int sthcd_pep_xfer_callback(struct starlet_ipc_request *req);

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_start_xfer(struct sthcd_pep *pep)
{
	struct urb *urb = pep->urb;
	struct sthcd_udev *udev = pep->udev;
	int error;

	BUG_ON(!urb);

	/* udev was disconnected */
	if (unlikely(!udev)) {
		error = -ENODEV;
		goto done;
	}

	if (!pep_is_enabled(pep)) {
		error = -ESHUTDOWN;
		goto done;
	}

	/* for OUT transfers, copy the data to send into the pep xfer buffer */
	if (pep->io_buf_len > 0) {
		if (usb_urb_dir_out(urb)) {
			/* host -> device */
			BUG_ON(!urb->transfer_buffer);
			memcpy(pep->io_buf,
			       urb->transfer_buffer + pep->io_xfer_offset,
			       pep->io_buf_len);
		}
	}

	starlet_ioh_sg_set_buf(&pep->io[0],
				pep->io_buf, pep->io_buf_len);

	/* start an async transfer */
	error = starlet_ioh_ioctlv_nowait(udev->fd, pep->request,
					  pep->nents_in, pep->in, 1, pep->io,
					  sthcd_pep_xfer_callback, pep);
	if (!error)
		pep->outstanding++;

done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_giveback_urb(struct sthcd_hcd *sthcd,
			      struct urb *urb, int status)
__releases(sthcd->lock) __acquires(sthcd->lock)
{
	struct usb_hcd *hcd = sthcd_to_hcd(sthcd);

	/*
	 * Release the hcd lock here as the callback may need to
	 * hold it again.
	 */
	spin_unlock(&sthcd->lock);
	usb_hcd_giveback_urb(hcd, urb, status);
	spin_lock(&sthcd->lock);

	return status;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
struct urb *sthcd_find_next_urb_in_ep(struct usb_host_endpoint *ep)
{
	if (list_empty(&ep->urb_list))
		return NULL;
	else
		return list_first_entry(&ep->urb_list, struct urb, urb_list);
}

static int sthcd_pep_send_urb(struct sthcd_pep *pep, struct urb *urb);

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_cond_send_next_urb(struct sthcd_pep *pep)
{
	struct urb *urb;
	int retval = 0;
	int error;

	/* schedule next urb if any */
	urb = sthcd_find_next_urb_in_ep(pep->ep);
	if (urb) {
		error = sthcd_pep_send_urb(pep, urb);
		if (!error) {
			retval = 1;
			goto done;
		} else {
			retval = error;
		}
	}
done:
	if (retval < 0)
		DBG("%s: retval=%d (%x)\n", __func__, retval, retval);
	return retval;
}

/*
 *
 * Context: interrupts disabled, hcd lock held
 */
static int sthcd_pep_send_urb(struct sthcd_pep *pep, struct urb *urb)
{
	struct sthcd_port *port = NULL;
	struct sthcd_hcd *sthcd;
	struct usb_ctrlrequest *req;
	u16 typeReq, wValue;
	int retval, fake;
	int error;

	/*
	 * Unconditionally fail urbs targetted at doomed ports.
	 */
	if (pep->udev) {
		port = udev_to_port(pep->udev);
		if (test_bit(__STHCD_PORT_DOOMED, &port->flags)) {
			error = -ENODEV;
			goto done;
		}
	}

	if (test_and_set_bit(__STHCD_PEP_XFERBUSY, &pep->flags)) {
		/*
		 * There is a pep xfer in progress.
		 * Our urb is already queued on the usb device, so do nothing
		 * here and rely on the pep xfer callback to do the actual
		 * work when it's done with the current urb in flight.
		 */
		error = 0;
		goto done;
	}

	/* we can have one ongoing urb only */
	error = sthcd_pep_takein_urb(pep, urb);
	if (error)
		goto done;

	urb->hcpriv = urb;	/* mark urb in use */

	retval = sthcd_pep_setup_first_xfer(pep);
	if (retval < 0) {
		error = retval;
		goto err_setup_xfer;
	}

	fake = 0;
	if (pep->request == STHCD_IOCTLV_CONTROLREQ) {
		req = (struct usb_ctrlrequest *)urb->setup_packet;
		typeReq = (req->bRequestType << 8) | req->bRequest;
		wValue = le16_to_cpu(req->wValue);

		switch (typeReq) {
		case DeviceOutRequest | USB_REQ_SET_ADDRESS: /* 0005 */
			if (urb->dev->devnum != 0) {
				/* REVISIT, never reached */
				drv_printk(KERN_WARNING,
					   "address change %u->%u\n",
					   urb->dev->devnum, wValue);
			}
			/*
			 * We are guaranteed to have an udev because the takein
			 * was successful.
			 */
			pep->udev->devnum = wValue;
			urb->actual_length = 0;

			/* clear the port reset count, we have an address */
			if (wValue) {
				/*
				 * We need to retrieve the port again
				 * as we might have entered the function
				 * without an udev assigned to the pep.
				 */
				port = udev_to_port(pep->udev);
				port->nr_resets = 0;
			}
			fake = 1;
			break;
		default:
			break;
		}
	}

	if (fake) {
		sthcd = pep->sthcd;
		/* finish this fake urb synchronously... */
		usb_hcd_unlink_urb_from_ep(sthcd_to_hcd(sthcd), urb);
		sthcd_giveback_urb(sthcd, urb, 0);
		/* ... and proceed with the next urb, if applicable */
		sthcd_pep_cond_send_next_urb(pep);
	} else {
		/* allocate an io buffer for this transfer */
		error = sthcd_pep_alloc_xfer_io_buf(pep, pep->io_buf_len);
		if (error)
			goto err_alloc_io_buf;

		/* ... and start the first transfer */
		error = sthcd_pep_start_xfer(pep);
		if (error)
			goto err_start_xfer;
	}

	return 0;

err_start_xfer:
	sthcd_pep_free_xfer_io_buf(pep);
err_alloc_io_buf:
err_setup_xfer:
	sthcd_pep_takeout_urb(pep);
	urb->hcpriv = NULL;
done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

static void sthcd_pep_print(struct sthcd_pep *pep)
{
	struct usb_device *udev;
	u16 idVendor, idProduct;

	idVendor = idProduct = 0xffff;
	if (pep->urb) {
		udev = pep->urb->dev;
		if (udev) {
			idVendor = le16_to_cpu(udev->descriptor.idVendor);
			idProduct = le16_to_cpu(udev->descriptor.idProduct);
		}
	}
	DBG("(%04X:%04X) request=%d,"
	    " io_buf=%p, io_buf_len=%u, io_xfer_offset=%u\n",
		idVendor, idProduct,
		pep->request,
		pep->io_buf,
		pep->io_buf_len,
		pep->io_xfer_offset);
}

/*
 *
 * Context: in interrupt
 */
static int sthcd_pep_xfer_callback(struct starlet_ipc_request *req)
{
	int xfer_len = req->result;
	struct sthcd_pep *pep = req->done_data;
	int status = 0;
	struct sthcd_port *port;
	struct sthcd_hcd *sthcd;
	struct usb_hcd *hcd;
	struct urb *urb;
	int retval;
	unsigned long flags;
	int error;

	starlet_ipc_free_request(req);

	sthcd = pep->sthcd;
	spin_lock_irqsave(&sthcd->lock, flags);

	hcd = sthcd_to_hcd(sthcd);

	pep->outstanding--;

	urb = pep->urb;
	if (!urb) {
		/*
		 * starlet completed an URB that was already dequeued.
		 *
		 * We must free here the memory used by the pep, including
		 * I/O buffers, avoiding dereferencing any USB stack data
		 * pointed by the pep, as it may be invalid now.
		 */
		sthcd_pep_free_xfer_io_buf(pep);
		sthcd_pep_free(pep);
		goto done;
	}

	/* sanity checks, determine transfer status and length */
	if (xfer_len < 0) {
		status = xfer_len;
		xfer_len = 0;

		if (status != -7004 && status != -7003 && status != -7005) {
			drv_printk(KERN_ERR, "request completed"
				   " with error %d\n", status);
			sthcd_pep_print(pep);
		}

		switch (status) {
		case -7003:
		case -7004:
			/* endpoint stall */
			status = -EPIPE;
			break;
		case -7005:
			/* nak? */
			status = -ECONNRESET;
			break;
		case -7008:
		case -7022:
		case -4:
			/* FALL-THROUGH */
		default:
			/*
			 * We got an unknown, probably un-retryable, error.
			 * Flag the port as unuseable. The associated
			 * device will be disconnected ASAP.
			 */
			port = udev_to_port(pep->udev);
			set_bit(__STHCD_PORT_DOOMED, &port->flags);
			DBG("%s: error %d on port %d, doomed!\n", __func__,
			    status, port - pep->sthcd->ports + 1);

			/* also, do not use the pep for xfers anymore */
			set_bit(__STHCD_PEP_DISABLED, &pep->flags);
			status = -ENODEV;
			break;
		}
	} else {
		if (usb_pipecontrol(urb->pipe)) {
			/*
			 * starlet includes the length of the request
			 * into the reply for control transfers.
			 * We need to substract the request size from
			 * the reply len to get the actual data size.
			 */
			xfer_len -= sizeof(struct usb_ctrlrequest);
			if (xfer_len < 0) {
				drv_printk(KERN_ERR, "request incomplete,"
					   " %d bytes short\n",
					   -xfer_len);
				status = -EPIPE;
				xfer_len = 0;
			}
		}
		if (xfer_len > pep->io_buf_len) {
			DBG("%s: xfer len %u larger than xfer buf"
			    " len %u\n", __func__,
			    xfer_len, pep->io_buf_len);
			xfer_len = pep->io_buf_len;
		}

	}

	if (xfer_len > 0) {
		sthcd_pep_finish_xfer(pep, xfer_len);

		/*
		 * Only schedule the next chunk if we didn't get a short xfer
		 * and the pep is still active
		 */
		if (xfer_len == pep->io_buf_len && pep_is_enabled(pep)) {
			retval = sthcd_pep_setup_next_xfer(pep);
			if (retval <= 0) {
				/* an error happened or all chunks were done */
				status = retval;
			} else {
				/* next xfer */
				sthcd_pep_start_xfer(pep);
				goto done;
			}
		}
	}

	sthcd_pep_free_xfer_io_buf(pep);
	urb->actual_length = pep->io_xfer_offset;

	/* at this point, we are done with this urb */
	clear_bit(__STHCD_PEP_XFERBUSY, &pep->flags);

	sthcd_pep_takeout_urb(pep);

	BUG_ON(!sthcd);
	BUG_ON(!urb);

	error = usb_hcd_check_unlink_urb(hcd, urb, status);
	if (!error) {
		usb_hcd_unlink_urb_from_ep(hcd, urb);

		/* give back this urb */
		sthcd_giveback_urb(sthcd, urb, status);
	} else {
		/* REVISIT, paranoid */
		DBG("%s: error checking unlink\n", __func__);
	}

	/* if applicable, launch the next urb in this endpoint queue */
	sthcd_pep_cond_send_next_urb(pep);

done:
	spin_unlock_irqrestore(&sthcd->lock, flags);

	return 0;
}


/*
 * starlet USB device "udev" abstraction.
 *
 *
 */


static struct sthcd_udev *sthcd_get_free_udev(struct sthcd_hcd *sthcd)
{
	struct sthcd_port *port;
	struct sthcd_udev *udev;
	int i;

	port = sthcd->ports;
	for (i = 0; i < sthcd->nr_ports; i++, port++) {
		udev = &port->udev;
		if (!test_and_set_bit(__STHCD_PORT_INUSE, &port->flags))
			return udev;
	}
	return NULL;
}

static struct sthcd_udev *sthcd_find_udev_by_ids(struct sthcd_hcd *sthcd,
						 u16 idVendor, u16 idProduct)
{
	struct sthcd_udev *udev;

	list_for_each_entry(udev, &sthcd->device_list, node) {
		if (udev->idVendor == idVendor && udev->idProduct == idProduct)
			return udev;
	}
	return NULL;
}

#if 0
static int sthcd_udev_suspend(struct sthcd_udev *udev)
{
	int error;

	error = starlet_ioctl(udev->fd, STHCD_IOCTL_SUSPENDDEVICE,
			      NULL, 0, NULL, 0);
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}

static int sthcd_udev_resume(struct sthcd_udev *udev)
{
	int error;

	error = starlet_ioctl(udev->fd, STHCD_IOCTL_RESUMEDEVICE,
			      NULL, 0, NULL, 0);
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}
#endif

static int sthcd_udev_close(struct sthcd_udev *udev)
{
	int fd = udev->fd;

	udev->fd = -1;
	return starlet_close(fd);
}

static int sthcd_udev_open(struct sthcd_udev *udev)
{
	struct sthcd_oh *oh = udev->oh;
	char pathname[32];
	int error;

	if (udev->fd != -1) {
		drv_printk(KERN_WARNING, "udev %04X.%04X already opened,"
			   " closing it first\n",
			   udev->idVendor, udev->idProduct);
		sthcd_udev_close(udev);
	}

	snprintf(pathname, sizeof(pathname), "/dev/usb/oh%u/%04x/%04x",
		 oh->index, udev->idVendor, udev->idProduct);
	error = starlet_open(pathname, 0);
	if (error < 0) {
		drv_printk(KERN_ERR, "open %s failed\n", pathname);
		return error;
	}
	udev->fd = error;

	return 0;
}

static void sthcd_udev_exit(struct sthcd_udev *udev)
{
	struct sthcd_hcd *sthcd;
	struct sthcd_pep *pep;
	unsigned long flags;

	sthcd = udev->oh->hcd;

	spin_lock_irqsave(&sthcd->lock, flags);

	/* remove from the list of connected devices */
	list_del_init(&udev->node);

	/* unlink all associated peps */
	list_for_each_entry(pep, &udev->pep_list, node) {
		if (pep->udev) {
			pep->udev = NULL;
			list_del_init(&pep->node);
		}
	}

	spin_unlock_irqrestore(&sthcd->lock, flags);

	sthcd_udev_close(udev);

	udev->idVendor = 0;
	udev->idProduct = 0;
	udev->oh = NULL;
	udev->devnum = 0;
}

static int sthcd_udev_init(struct sthcd_udev *udev,
			   struct sthcd_oh *oh,
			   u16 idVendor, u16 idProduct)
{
	struct sthcd_hcd *sthcd = oh->hcd;
	int error;
	unsigned long flags;

	INIT_LIST_HEAD(&udev->pep_list);

	udev->idVendor = idVendor;
	udev->idProduct = idProduct;
	udev->oh = oh;
	udev->fd = -1;
	udev->devnum = 0;

	error = sthcd_udev_open(udev);
	if (error)
		return error;

	spin_lock_irqsave(&sthcd->lock, flags);
	list_add_tail(&udev->node, &sthcd->device_list);
	spin_unlock_irqrestore(&sthcd->lock, flags);

	return error;
}


/*
 * Hub emulation routines.
 *
 */

#define STHCD_USB_DT_HUB_TOTAL_SIZE \
	(USB_DT_HUB_NONVAR_SIZE + 2*((STHCD_MAX_PORTS + 1 + 7) / 8))

static struct usb_hub_descriptor sthcd_hub_hub_descr = {
	.bDescLength		= STHCD_USB_DT_HUB_TOTAL_SIZE,
	.bDescriptorType	= USB_DT_HUB,
	.bNbrPorts		= STHCD_MAX_PORTS,
	.wHubCharacteristics	= 0x0000,
	.bPwrOn2PwrGood		= 0,
	.bHubContrCurrent	= 0,
};


static int
sthcd_hub_control_standard(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
			   u16 wIndex, char *buf, u16 wLength)
{
	int retval = -EINVAL;

	switch (typeReq) {
	case DeviceOutRequest | USB_REQ_SET_CONFIGURATION: /* 0009 */
		if (wValue != 1) {
			drv_printk(KERN_INFO, "invalid configuration %d\n",
				   wValue);
		} else {
			retval = 0;
		}
		break;
	case DeviceRequest | USB_REQ_GET_STATUS: /* 8000 */
		if (wLength < 2) {
			retval = -ENOMEM;
		} else {
			buf[0] = (1 << USB_DEVICE_SELF_POWERED);
			buf[1] = 0;
			retval = 2;
		}
		break;
	default:
		drv_printk(KERN_WARNING, "%s: request %04X not supported\n",
			   __func__, typeReq);
		break;
	}
	DBG("%s: retval=%d (%x)\n", __func__, retval, retval);
	return retval;
}

static int
sthcd_hub_control_hub(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
		      u16 wIndex, void *buf, u16 wLength)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);
	struct usb_hub_status *hub_status;
	struct usb_hub_descriptor *hub_descr;
	size_t size, port_array_size;
	u8 *p;
	int retval = -EINVAL;

	switch (typeReq) {
	case GetHubStatus:	/* 0xA000 */
		size = sizeof(*hub_status);
		if (wLength < size) {
			retval = -ENOMEM;
		} else {
			hub_status = buf;
			hub_status->wHubStatus = 0x0000; /* no problems */
			hub_status->wHubChange = 0x0000; /* no changes */
			retval = size;
		}
		break;
	case GetHubDescriptor:	/* 0xA006 */
		/*
		 * For the DeviceRemovable and PortPwrCtrlMask fields:
		 *  bit 0 is reserved.
		 *  bit 1 is the internal (oh1) port, which is non-removable.
		 *  bit 2..nr_ports+1 are the external (oh0) ports.
		 */
		port_array_size = (1 + sthcd->nr_ports + 7) / 8;
		size = USB_DT_HUB_NONVAR_SIZE + 2*port_array_size;

		if (wLength < size) {
			retval = -ENOMEM;
		} else {
			p = buf;

			memcpy(p, &sthcd_hub_hub_descr, USB_DT_HUB_NONVAR_SIZE);
			p += USB_DT_HUB_NONVAR_SIZE;

			/* fixup the descriptor with the real number of ports */
			hub_descr = buf;
			hub_descr->bDescLength = size;
			hub_descr->bNbrPorts = sthcd->nr_ports;

			/* DeviceRemovable field, table 11-13 Hub Descriptor */
			memset(p, 0, port_array_size);
			*p |= 0x02;	/* port 1 is non-removable */
			p += port_array_size;

			/* PortPwrCtrlMask field, table 11-13 Hub Descriptor */
			memset(p, 0xff, port_array_size);

			retval = size;
		}
		break;
	default:
		drv_printk(KERN_WARNING, "%s: request %04X not supported\n",
			   __func__, typeReq);
		break;
	}

	if (retval < 0)
		DBG("%s: retval=%d (%x)\n", __func__, retval, retval);
	return retval;
}


static int
sthcd_hub_control_port(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
		       u16 wIndex, void *buf, u16 wLength)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);
	struct sthcd_port *port;
	unsigned long flags;
	int retval = 0;

	if (wIndex == 0 || wIndex > sthcd->nr_ports) {
		DBG("%s: invalid port %u\n", __func__, wIndex);
		return -EINVAL;
	}

	spin_lock_irqsave(&sthcd->lock, flags);

	wIndex--;
	port = &sthcd->ports[wIndex];

	switch (typeReq) {
	case GetPortStatus:	/* 0xA300 */
		if (test_bit(__STHCD_PORT_DOOMED, &port->flags)) {
			/* disconnect */
			if (!!(port->status_change & USB_PORT_STAT_CONNECTION))
				port->status_change |=
					(USB_PORT_STAT_C_CONNECTION<<16);
			port->status_change &= ~USB_PORT_STAT_CONNECTION;
		}
		/* REVISIT wait 50ms before clearing the RESET state */
		if (port->status_change & USB_PORT_STAT_RESET) {
			port->nr_resets++;
			if (port->nr_resets > 2) {
				DBG("%s: port %d was reset %u time(s),"
				    " doomed!\n", __func__,
				    wIndex+1, port->nr_resets);
				set_bit(__STHCD_PORT_DOOMED, &port->flags);
			}
			if (!(port->status_change & USB_PORT_STAT_ENABLE))
				port->status_change |=
						(USB_PORT_STAT_C_ENABLE << 16);
			port->status_change &= ~USB_PORT_STAT_RESET;
			port->status_change |= (USB_PORT_STAT_ENABLE |
						(USB_PORT_STAT_C_RESET << 16));
			port->udev.devnum = 0;
		}
		retval = 4;
		((__le32 *) buf)[0] = cpu_to_le32(port->status_change);
		break;
	case ClearPortFeature:	/* 0x2301 */
		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			port->status_change &= USB_PORT_STAT_POWER;
			break;
		case USB_PORT_FEAT_SUSPEND:
		case USB_PORT_FEAT_POWER:
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:	/* 0x14 */
			break;
		default:
			goto error;
		}
		port->status_change &= ~(1 << wValue);
		break;
	case SetPortFeature:	/* 0x2303 */
		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
		case USB_PORT_FEAT_SUSPEND:
		case USB_PORT_FEAT_POWER: /* 0x08 */
			break;
		case USB_PORT_FEAT_RESET: /* 0x04 */
			/* REVISIT, free all related resources here */
			break;
		default:
			goto error;
		}
		port->status_change |= 1 << wValue;
		break;
	default:
		drv_printk(KERN_WARNING, "%s: request %04X not supported\n",
			   __func__, typeReq);
error:
		retval = -EPIPE;
		break;
	}

	spin_unlock_irqrestore(&sthcd->lock, flags);

	return retval;
}

static int sthcd_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
			     u16 wIndex, char *buf, u16 wLength)
{
	u8 bmRequestType;
	int retval = -EINVAL;

	/*
	 * starlet never answers to requests on device 0/0, so we emulate it.
	 */

	bmRequestType = typeReq >> 8;

	switch (bmRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		/* generic requests */
		retval = sthcd_hub_control_standard(hcd, typeReq, wValue,
						    wIndex, buf, wLength);
		break;
	case USB_TYPE_CLASS:
		/* hub-specific requests */
		switch (bmRequestType & USB_RECIP_MASK) {
		case USB_RECIP_DEVICE:
			/* hub */
			retval = sthcd_hub_control_hub(hcd, typeReq, wValue,
						       wIndex, buf, wLength);
			break;
		case USB_RECIP_OTHER:
			/* port */
			retval = sthcd_hub_control_port(hcd, typeReq, wValue,
							wIndex, buf, wLength);
			break;
		default:
			drv_printk(KERN_WARNING, "%s: request %04X"
				   " not supported\n", __func__, typeReq);
			break;
		}
		break;
	default:
		drv_printk(KERN_WARNING, "%s: request %04X not supported\n",
			   __func__, typeReq);
		break;
	}

	if (retval > 0)
		retval = 0;
	if (retval < 0)
		DBG("%s: retval=%d (%x)\n", __func__, retval, retval);
	return retval;
}

static int sthcd_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);
	u16 *p = (u16 *)buf;
	struct sthcd_port *port;
	unsigned long flags;
	int i, result;

	if (!HC_IS_RUNNING(hcd->state))
		return -ESHUTDOWN;

#if 0
	if (timer_pending(&hcd->rh_timer))
		return 0;
#endif

	/* FIXME, this code assumes at least 9 and no more than 15 ports */
	BUG_ON(sthcd->nr_ports > 15 || sthcd->nr_ports < 8);

	spin_lock_irqsave(&sthcd->lock, flags);

	port = sthcd->ports;
	for (i = 0, *p = 0; i < sthcd->nr_ports; i++, port++) {
		if ((port->status_change & 0xffff0000) != 0) {
			*p |= 1 << (i+1);
			/* REVISIT */
		}
	}
	*p = le16_to_cpu(*p);
	result = (*p != 0) ? 2 : 0;

	spin_unlock_irqrestore(&sthcd->lock, flags);

/*	DBG("%s: poll cycle, changes=%04x\n", __func__, *p); */

	return result;
}


/*
 * "OH" abstraction.
 *
 */

static int sthcd_oh_insert_udev(struct sthcd_oh *oh,
				u16 idVendor, u16 idProduct)
{
	struct sthcd_hcd *sthcd = oh->hcd;
	struct sthcd_udev *udev;
	struct sthcd_port *port;
	unsigned long flags;
	int error;

	drv_printk(KERN_INFO, "inserting device %04X.%04X\n",
		   idVendor, idProduct);

	udev = sthcd_get_free_udev(sthcd);
	if (!udev) {
		drv_printk(KERN_ERR, "no free udevs!\n");
		return -EBUSY;
	}

	error = sthcd_udev_init(udev, oh, idVendor, idProduct);
	if (!error) {
		spin_lock_irqsave(&sthcd->lock, flags);

		port = udev_to_port(udev);
		/* notify a connection event */
		port->status_change = USB_PORT_STAT_POWER |
					USB_PORT_STAT_CONNECTION |
					(USB_PORT_STAT_C_CONNECTION<<16);

		spin_unlock_irqrestore(&sthcd->lock, flags);
	}
	return error;
}

static int sthcd_oh_remove_udev(struct sthcd_oh *oh,
				u16 idVendor, u16 idProduct)
{
	struct sthcd_hcd *sthcd = oh->hcd;
	struct sthcd_udev *udev;
	struct sthcd_port *port;
	u32 old_status;
	unsigned long flags;
	int error = 0;

	udev = sthcd_find_udev_by_ids(sthcd, idVendor, idProduct);
	if (!udev) {
		/* normally reached for ignored hubs */
		error = -ENODEV;
	} else {
		drv_printk(KERN_INFO, "removing device %04X.%04X\n",
			   idVendor, idProduct);
		sthcd_udev_exit(udev);

		spin_lock_irqsave(&sthcd->lock, flags);

		port = udev_to_port(udev);
		clear_bit(__STHCD_PORT_INUSE, &port->flags);
		clear_bit(__STHCD_PORT_DOOMED, &port->flags);
		port->nr_resets = 0;
		/* notify a disconnection event */
		old_status = port->status_change;
		port->status_change = USB_PORT_STAT_POWER;
		if ((old_status & USB_PORT_STAT_CONNECTION) != 0)
			port->status_change |= (USB_PORT_STAT_C_CONNECTION<<16);

		spin_unlock_irqrestore(&sthcd->lock, flags);
	}
	return error;
}


/*
 * Non-atomic context (synchronous call).
 */
static int sthcd_usb_control_msg(int fd,
				 __u8 request, __u8 requesttype,
				 __u16 value, __u16 index,
				 void *data, __u16 size,
				 int timeout)
{
	struct sthcd_ctrl_params_in *params_in;
	struct starlet_ioh_sg in[6];
	struct starlet_ioh_sg io[1];
	int error;

	params_in = starlet_ioh_kzalloc(sizeof(*params_in));
	if (!params_in) {
		error = -ENOMEM;
		goto done;
	}

	params_in->req.bRequestType = requesttype;
	params_in->req.bRequest = request;
	params_in->req.wValue = cpu_to_le16p(&value);
	params_in->req.wIndex = cpu_to_le16p(&index);
	params_in->req.wLength = cpu_to_le16p(&size);
	params_in->_unk1 = timeout; /* seconds? */

	starlet_ioh_sg_init_table(in, 6);
	starlet_ioh_sg_entry(&in[0], &params_in->req.bRequestType);
	starlet_ioh_sg_entry(&in[1], &params_in->req.bRequest);
	starlet_ioh_sg_entry(&in[2], &params_in->req.wValue);
	starlet_ioh_sg_entry(&in[3], &params_in->req.wIndex);
	starlet_ioh_sg_entry(&in[4], &params_in->req.wLength);
	starlet_ioh_sg_entry(&in[5], &params_in->_unk1);

	starlet_ioh_sg_init_table(io, 1);
	starlet_ioh_sg_set_buf(&io[0], data, size);

	error = starlet_ioh_ioctlv(fd, STHCD_IOCTLV_CONTROLREQ,
				   6, in, 1, io);

	starlet_ioh_kfree(params_in);

	if (error > 0) {
		/* adjust size for successful control xfers */
		error -= sizeof(struct usb_ctrlrequest);
		if (error < 0)
			error = -EINVAL;
	}

done:
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	return error;
}


static int sthcd_oh_check_hub(struct sthcd_oh *oh, u16 idVendor, u16 idProduct)
{
	char pathname[32];
	struct usb_device_descriptor *descriptor;
	int fd;
	int i;
	int retval;

	descriptor = starlet_ioh_kzalloc(USB_DT_DEVICE_SIZE);
	if (!descriptor) {
		retval = -ENOMEM;
		goto done;
	}

	snprintf(pathname, sizeof(pathname), "/dev/usb/oh%u/%04x/%04x",
		 oh->index, idVendor, idProduct);
	retval = starlet_open(pathname, 0);
	if (retval < 0) {
		drv_printk(KERN_ERR, "open %s failed\n", pathname);
		starlet_ioh_kfree(descriptor);
		goto done;
	}
	fd = retval;

	for (i = 0; i < 3; i++) {
		retval = sthcd_usb_control_msg(fd, USB_REQ_GET_DESCRIPTOR,
					      USB_DIR_IN,
					      USB_DT_DEVICE << 8, 0,
					      descriptor, USB_DT_DEVICE_SIZE,
					      0);
		if (retval != -7005)
			break;
		DBG("%s: attempt %d, retval=%d (%x)\n", __func__,
		    i, retval, retval);
	}

	starlet_close(fd);

	if (retval >= USB_DT_DEVICE_SIZE) {
		/* tell if a hub was found */
		retval = (descriptor->bDeviceClass == USB_CLASS_HUB) ? 1 : 0;
	} else {
		if (retval >= 0)
			retval = -EINVAL;	/* short descriptor */
	}

	starlet_ioh_kfree(descriptor);

done:
	if (retval < 0)
		DBG("%s: retval=%d (%x)\n", __func__, retval, retval);
	return retval;
}

struct sthcd_getdevicelist_params_in {
	u8 devid_count;
	u8 _type;
};
struct sthcd_getdevicelist_params_io {
	u8 devid_count;
	struct sthcd_devid devids[0];
};

static int sthcd_get_device_list(struct sthcd_hcd *sthcd, int fd,
				 struct sthcd_devid *devids, size_t nr_devids)
{
	struct starlet_ioh_sg in[2], io[2];
	struct sthcd_getdevicelist_params_in *params_in;
	struct sthcd_getdevicelist_params_io *params_io;
	size_t size = nr_devids * sizeof(struct sthcd_devid);
	int error;

	if (!nr_devids)
		return -EINVAL;

	params_in = starlet_ioh_kzalloc(sizeof(*params_in));
	if (!params_in)
		return -ENOMEM;

	params_io = starlet_ioh_kzalloc(sizeof(*params_io) + size);
	if (!params_io) {
		starlet_ioh_kfree(params_in);
		return -ENOMEM;
	}

	params_in->devid_count = nr_devids;
	params_in->_type = 0;

	starlet_ioh_sg_init_table(in, 2);
	starlet_ioh_sg_entry(&in[0], &params_in->devid_count);
	starlet_ioh_sg_entry(&in[1], &params_in->_type);

	starlet_ioh_sg_init_table(io, 2);
	starlet_ioh_sg_entry(&io[0], &params_io->devid_count);
	starlet_ioh_sg_set_buf(&io[1], &params_io->devids, size);

	error = starlet_ioh_ioctlv(fd, STHCD_IOCTLV_GETDEVICELIST,
				   2, in, 2, io);

	if (error < 0) {
		DBG("%s: error=%d (%x)\n", __func__, error, error);
	} else {
		memcpy(devids, params_io->devids, size);
		error = params_io->devid_count;
	}

	starlet_ioh_kfree(params_in);
	starlet_ioh_kfree(params_io);

	return error;
}

static int sthcd_devid_match(struct sthcd_devid *id1, struct sthcd_devid *id2)
{
	return id1->idVendor == id2->idVendor &&
	       id1->idProduct == id2->idProduct;
}

static int sthcd_devid_find(struct sthcd_devid *haystack, size_t count,
			    struct sthcd_devid *needle)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (sthcd_devid_match(&haystack[i], needle))
			return 1;
	}
	return 0;
}

static int sthcd_oh_rescan(struct sthcd_oh *oh)
{
	static unsigned int poll_cycles;
	struct usb_hcd *hcd = sthcd_to_hcd(oh->hcd);
	struct sthcd_devid *p;
	int nr_new_devids, i;
	int changes;
	int error;

	error = sthcd_get_device_list(oh->hcd, oh->fd, oh->new_devids,
				      oh->max_devids);
	if (error < 0)
		return error;

	nr_new_devids = error;
	changes = 0;

	for (i = 0; i < oh->nr_devids; i++) {
		p = &oh->devids[i];
		if (!sthcd_devid_find(oh->new_devids, nr_new_devids, p)) {
			/* removal */
			error = sthcd_oh_remove_udev(oh, p->idVendor,
						     p->idProduct);
			if (!error)
				changes++;
		}
	}

	for (i = 0; i < nr_new_devids; i++) {
		p = &oh->new_devids[i];
		if (!sthcd_devid_find(oh->devids, oh->nr_devids, p)) {
			/* insertion */
			error = sthcd_oh_check_hub(oh, p->idVendor,
						   p->idProduct);
			if (error == 0) {
				/* not a hub, register the usb device */
				error = sthcd_oh_insert_udev(oh, p->idVendor,
							     p->idProduct);
				if (!error)
					changes++;
			} else {
				drv_printk(KERN_INFO,
					   "ignoring hub %04X.%04X\n",
					   p->idVendor, p->idProduct);
			}
		}
	}

	memcpy(oh->devids, oh->new_devids, nr_new_devids * sizeof(*p));
	oh->nr_devids = nr_new_devids;

	/*
	 * FIXME
	 * We ask here the USB layer to explicitly poll for root hub changes
	 * until we get at least two complete rescan cycles without changes.
	 *
	 * Otherwise, for unknown reasons, we end up missing the detection of
	 * some devices, even if the insertion/removal of these devices is
	 * properly signaled in port->status_change.
	 */
	if (changes) {
#if 1
		if (!poll_cycles) {
			hcd->poll_rh = 1;
			usb_hcd_poll_rh_status(hcd);
		}
		poll_cycles = 2;
	} else {
		if (!poll_cycles)
			hcd->poll_rh = 0;
		else
			poll_cycles--;
#else
		usb_hcd_poll_rh_status(hcd);
#endif
	}

	return 0;
}

static int sthcd_oh_init(struct sthcd_oh *oh, unsigned int index,
			 struct sthcd_hcd *sthcd, size_t max_devids)
{
	char pathname[16];
	int error;

	if (index != 0 && index != 1)
		return -EINVAL;

	snprintf(pathname, sizeof(pathname), "/dev/usb/oh%u", index);
	error = starlet_open(pathname, 0);
	if (error < 0)
		return error;

	oh->fd = error;
	oh->devids = kzalloc(2 * max_devids * sizeof(struct sthcd_devid),
			     GFP_KERNEL);
	if (!oh->devids) {
		starlet_close(oh->fd);
		return -ENOMEM;
	}

	oh->new_devids = oh->devids + max_devids;

	oh->max_devids = max_devids;
	oh->nr_devids = 0;

	oh->index = index;
	oh->hcd = sthcd;

	return 0;
}

static void sthcd_oh_exit(struct sthcd_oh *oh)
{
	starlet_close(oh->fd);
	oh->fd = -1;
	kfree(oh->devids);
	oh->devids = NULL;
}

static int sthcd_rescan_thread(void *arg)
{
	struct sthcd_hcd *sthcd = arg;
	struct sthcd_oh *oh;

	/*
	 * REVISIT
	 * We may need to rescan oh1 if bluetooth dongle disconnects.
	 */

	/* oh1 has non-removable devices only, so just scan it once */
	sthcd_oh_rescan(&sthcd->oh[1]);

	oh = &sthcd->oh[0];

	while (!kthread_should_stop()) {
		sthcd_oh_rescan(oh);

		/* re-check again after the configured interval */
		sleep_on_timeout(&sthcd->rescan_waitq,
				 STHCD_RESCAN_INTERVAL*HZ);
	}
	return 0;
}


/*
 *
 *
 */

static int sthcd_init(struct usb_hcd *hcd)
{
	return 0;
}

static int sthcd_start(struct usb_hcd *hcd)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);
	int error;

	/*
	 * This is to prevent a spurious error from the kernel usb stack
	 * as we do not make use of interrupts.
	 */
	set_bit(HCD_FLAG_SAW_IRQ, &hcd->flags);

	hcd->uses_new_polling = 1;

	/* oh0 is the external bus */
	error = sthcd_oh_init(&sthcd->oh[0], 0, sthcd, STHCD_MAX_DEVIDS);
	if (error < 0) {
		DBG("%s: error=%d (%x)\n", __func__, error, error);
		return error;
	}

	/* oh1 is the internal bus, used only by the bluetooth dongle */
	error = sthcd_oh_init(&sthcd->oh[1], 1, sthcd, 1);
	if (error < 0) {
		DBG("%s: error=%d (%x)\n", __func__, error, error);
		sthcd_oh_exit(&sthcd->oh[0]);
		return error;
	}

	hcd->state = HC_STATE_RUNNING;

	/* device insertion/removal is managed by the rescan thread */
	sthcd->rescan_task = kthread_run(sthcd_rescan_thread, sthcd, "ksthcd");
	if (IS_ERR(sthcd->rescan_task))
		drv_printk(KERN_ERR, "failed to start rescan thread\n");

	return 0;
}

static void sthcd_stop(struct usb_hcd *hcd)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);

	if (!IS_ERR(sthcd->rescan_task)) {
		kthread_stop(sthcd->rescan_task);
		sthcd->rescan_task = ERR_PTR(-EINVAL);
	}

	sthcd_oh_exit(&sthcd->oh[0]);
	sthcd_oh_exit(&sthcd->oh[1]);

	hcd->state &= ~HC_STATE_RUNNING;
}

static int sthcd_get_frame_number(struct usb_hcd *hcd)
{
	DBG("%s: CALLED\n", __func__);
	return 0;
}


static int sthcd_urb_enqueue(struct usb_hcd *hcd, struct urb *urb,
			     gfp_t mem_flags)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);
	struct usb_host_endpoint *ep;
	struct sthcd_pep *pep;
	unsigned long flags;
	int error;

	spin_lock_irqsave(&sthcd->lock, flags);

	/* REVISIT, paranoid */
	if (urb->status != -EINPROGRESS) {
		DBG("%s: status != -EINPROGRESS\n", __func__);
		error = urb->status;
		goto done;
	}

	error = usb_hcd_link_urb_to_ep(hcd, urb);
	if (error)
		goto done;

	ep = urb->ep;

	/* allocate a pep for each endpoint on first use */
	if (!ep->hcpriv) {
		pep = sthcd_pep_alloc(sthcd, ep);
		if (!pep) {
			error = -ENOMEM;
			goto err_linked;
		}
		ep->hcpriv = pep;
	} else {
		pep = ep->hcpriv;
	}

	error = sthcd_pep_send_urb(pep, urb);
	if (!error)
		goto done;

err_linked:
	usb_hcd_unlink_urb_from_ep(hcd, urb);
done:
	spin_unlock_irqrestore(&sthcd->lock, flags);
	return error;
}

static int sthcd_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);
	struct usb_host_endpoint *ep;
	struct sthcd_pep *pep;
	unsigned long flags;
	int error;

	spin_lock_irqsave(&sthcd->lock, flags);

	error = usb_hcd_check_unlink_urb(hcd, urb, status);
	if (error)
		goto done;

	ep = urb->ep;
	pep = ep_to_pep(ep);
	if (pep && pep->urb == urb) {
		/*
		 * There is an urb in flight.
		 *
		 * We deattach the urb from the pep and leave the pep to the
		 * callback function, which will free it upon completion,
		 * without further action.
		 */
		sthcd_pep_takeout_urb(pep);
		ep->hcpriv = NULL;
	}

	usb_hcd_unlink_urb_from_ep(hcd, urb);
	sthcd_giveback_urb(sthcd, urb, status);

done:
	spin_unlock_irqrestore(&sthcd->lock, flags);

#if 0
	if (error < 0)
		DBG("%s: error=%d (%x)\n", __func__, error, error);
#endif
	return error;
}

static void sthcd_endpoint_disable(struct usb_hcd *hcd,
				   struct usb_host_endpoint *ep)
{
	struct sthcd_hcd *sthcd = hcd_to_sthcd(hcd);
	struct sthcd_pep *pep;
	unsigned long flags;

	spin_lock_irqsave(&sthcd->lock, flags);
	pep = ep->hcpriv;

	/* do nothing if the pep was already freed */
	if (!pep)
		goto done;

	if (pep->urb) {
		/*
		 * There is an urb in flight.
		 *
		 * Disable the private endpoint and take the urb out of it.
		 * The callback function will take care of freeing the pep
		 * when the starlet call completes.
		 */
		set_bit(__STHCD_PEP_DISABLED, &pep->flags);
		sthcd_pep_takeout_urb(pep);
	} else {
		/* the pep can be freed immediately when no urb is in flight */
		sthcd_pep_free(pep);
	}
	ep->hcpriv = NULL;

done:
	spin_unlock_irqrestore(&sthcd->lock, flags);
}


static const struct hc_driver starlet_hc_driver = {
	.description =		DRV_MODULE_NAME,
	.product_desc =		"Nintendo Wii USB Host Controller",
	.hcd_priv_size =	sizeof(struct sthcd_hcd),

	.irq =			NULL,
	.flags =		HCD_USB11,

	/* REVISIT, power management calls not yet supported */

	.reset =		sthcd_init,
	.start =		sthcd_start,
	.stop =			sthcd_stop,

	.get_frame_number =	sthcd_get_frame_number,

	.urb_enqueue =		sthcd_urb_enqueue,
	.urb_dequeue =		sthcd_urb_dequeue,
	.endpoint_disable =	sthcd_endpoint_disable,

	.hub_status_data =	sthcd_hub_status_data,
	.hub_control =		sthcd_hub_control,
};

static int __devinit sthcd_driver_probe(struct device *dev)
{
	struct sthcd_hcd *sthcd;
	struct usb_hcd *hcd;
	int error = -ENOMEM;

	if (starlet_get_ipc_flavour() != STARLET_IPC_IOS)
		return -ENODEV;

	/*
	 * We can't use normal dma as starlet requires MEM2 buffers
	 * to work properly in all cases.
	 */
	dev->dma_mask = NULL;

	hcd = usb_create_hcd(&starlet_hc_driver, dev, DRV_MODULE_NAME);
	if (!hcd)
		goto err;

	sthcd = hcd_to_sthcd(hcd);
	spin_lock_init(&sthcd->lock);

	sthcd->nr_ports = STHCD_MAX_PORTS;
	sthcd->ports = kzalloc(sthcd->nr_ports * sizeof(struct sthcd_port),
			       GFP_KERNEL);
	if (!sthcd->ports)
		goto err_alloc_ports;

	INIT_LIST_HEAD(&sthcd->device_list);
	init_waitqueue_head(&sthcd->rescan_waitq);

	error = usb_add_hcd(hcd, 0, 0);
	if (error) {
		drv_printk(KERN_INFO, "%s: error %d adding hcd\n",
			   __func__, error);
		goto err_add;
	}

	return 0;

err_add:
	kfree(sthcd->ports);
err_alloc_ports:
	usb_put_hcd(hcd);
err:
	return error;
}

static int __devexit sthcd_driver_remove(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);
	return 0;
}


/*
 * Open Firmware platform device routines
 *
 */

static int __init sthcd_of_probe(struct of_device *odev,
	const struct of_device_id *match)
{
	return sthcd_driver_probe(&odev->dev);
}

static int __exit sthcd_of_remove(struct of_device *odev)
{
	return sthcd_driver_remove(&odev->dev);
}

static struct of_device_id sthcd_of_match[] = {
	{ .compatible = "nintendo,starlet-ios-hcd" },
	{ },
};

MODULE_DEVICE_TABLE(of, sthcd_of_match);

static struct of_platform_driver sthcd_of_driver = {
	.owner = THIS_MODULE,
	.name = DRV_MODULE_NAME,
	.match_table = sthcd_of_match,
	.probe = sthcd_of_probe,
	.remove = sthcd_of_remove,
};


/*
 * Linux module framework
 *
 */

static int __init sthcd_module_init(void)
{
	if (usb_disabled())
		return -ENODEV;

	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   sthcd_driver_version);

	return of_register_platform_driver(&sthcd_of_driver);
}

static void __exit sthcd_module_exit(void)
{
	of_unregister_platform_driver(&sthcd_of_driver);
}

module_init(sthcd_module_init);
module_exit(sthcd_module_exit);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

