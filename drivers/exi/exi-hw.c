/*
 * drivers/exi/exi-hw.c
 *
 * Nintendo GameCube EXpansion Interface support. Hardware routines.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2005,2006,2007,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 * IMPLEMENTATION NOTES
 *
 * The EXI Layer provides the following primitives:
 *
 * 	op			atomic?
 * 	------------------	-------
 * 	take			yes
 * 	give			yes
 * 	select			yes
 * 	deselect		yes
 * 	transfer		yes/no (1)
 *
 * These primitives are encapsulated in several APIs.
 * See include/linux/exi.h for additional information.
 *
 * 1. Kernel Contexts
 *
 * User, softirq and hardirq contexts are supported, with some limitations.
 *
 * Launching EXI operations in softirq or hardirq context requires kernel
 * coordination to ensure channels are free before use.
 *
 * The EXI Layer Event System delivers events in softirq context, but it already
 * makes provisions to ensure that channels are useable by the event handlers.
 * Events are delivered only when the channels on the event handler
 * channel mask are all deselected. This allows one to run EXI commands in
 * softirq context from the EXI event handlers.
 *
 * "take" operations in user context will sleep if necessary until the
 * channel is "given".
 *
 *
 * 2. Transfers
 *
 * The EXI Layer provides a transfer API to perform read and write
 * operations.
 * By default, transfers partially or totally suitable for DMA will be
 * partially or totally processed through DMA. The EXI Layer takes care of
 * splitting a transfer in several pieces so the best transfer method is
 * used each time.
 *
 * (1) A immediate mode transfer is atomic, but a DMA transfer is not.
 */

/*#define EXI_DEBUG 1*/

#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <linux/exi.h>
#include "exi-hw.h"


#define drv_printk(level, format, arg...) \
	printk(level "exi: " format , ## arg)

#ifdef EXI_DEBUG
#  define DBG(fmt, args...) \
	   printk(KERN_ERR "%s: " fmt, __func__ , ## args)
#else
#  define DBG(fmt, args...)
#endif


static void exi_tasklet(unsigned long param);


/* io memory base for EXI */
static void __iomem *exi_io_mem;


/*
 * These are the available exi channels.
 */
static struct exi_channel exi_channels[EXI_MAX_CHANNELS] = {
	[0] = {
		.channel = 0,
		.lock = __SPIN_LOCK_UNLOCKED(exi_channels[0].lock),
		.io_lock = __SPIN_LOCK_UNLOCKED(exi_channels[0].io_lock),
		.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(
				exi_channels[0].wait_queue),
	},
	[1] = {
		.channel = 1,
		.lock = __SPIN_LOCK_UNLOCKED(exi_channels[1].lock),
		.io_lock = __SPIN_LOCK_UNLOCKED(exi_channels[1].io_lock),
		.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(
				exi_channels[1].wait_queue),
	},
	[2] = {
		.channel = 2,
		.lock = __SPIN_LOCK_UNLOCKED(exi_channels[2].lock),
		.io_lock = __SPIN_LOCK_UNLOCKED(exi_channels[2].io_lock),
		.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(
				exi_channels[2].wait_queue),
	},
};

/* handy iterator for exi channels */
#define exi_channel_for_each(pos) \
	for (pos = &exi_channels[0]; pos < &exi_channels[EXI_MAX_CHANNELS]; \
		pos++)

/* conversions between channel numbers and exi channel structures */
#define __to_exi_channel(channel) (&exi_channels[channel])
#define __to_channel(exi_channel) (exi_channel->channel)

/**
 *	to_exi_channel  -  returns an exi_channel given a channel number
 *	@channel:	channel number
 *
 *	Return the exi_channel structure associated to a given channel.
 */
struct exi_channel *to_exi_channel(unsigned int channel)
{
	if (channel > EXI_MAX_CHANNELS)
		return NULL;

	return __to_exi_channel(channel);
}
EXPORT_SYMBOL(to_exi_channel);

/**
 *	to_channel  -  returns a channel number given an exi channel
 *	@exi_channel:	channel
 *
 *	Return the channel number for a given exi_channel structure.
 */
unsigned int to_channel(struct exi_channel *exi_channel)
{
	BUG_ON(exi_channel == NULL);

	return __to_channel(exi_channel);
}
EXPORT_SYMBOL(to_channel);

/**
 *	exi_channel_owner  -  returns the owner of the given channel
 *	@exi_channel:	channel
 *
 *	Return the device owning a given exi_channel structure.
 */
struct exi_device *exi_channel_owner(struct exi_channel *exi_channel)
{
	return exi_channel->owner;
}


/*
 *
 *
 */

/**
 *	exi_select_raw  -  selects a device on an exi channel
 *	@exi_channel:	channel
 *	@device:	device number on channel
 *	@freq:		clock frequency index
 *
 *	Select a given device on a specified EXI channel by setting its
 *	CS line, and use the specified clock frequency when doing transfers.
 */
void exi_select_raw(struct exi_channel *exi_channel, unsigned int device,
		   unsigned int freq)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	BUG_ON(device > EXI_DEVICES_PER_CHANNEL ||
	       freq > EXI_MAX_FREQ);

	/*
	 * Preserve interrupt masks while setting the CS line bits.
	 */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = in_be32(csr_reg);
	csr &= (EXI_CSR_EXTINMASK | EXI_CSR_TCINTMASK | EXI_CSR_EXIINTMASK);
	csr |= ((1<<device) << 7) | (freq << 4);
	out_be32(csr_reg, csr);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
}
EXPORT_SYMBOL(exi_select_raw);


/**
 *	exi_deselect_raw  -  deselects all devices on an exi channel
 *	@exi_channel:	channel
 *
 *	Deselect any device previously selected on the specified EXI
 *	channel by unsetting all CS lines.
 */
void exi_deselect_raw(struct exi_channel *exi_channel)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	/*
	 * Preserve interrupt masks while clearing the CS line bits.
	 */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = in_be32(csr_reg);
	csr &= (EXI_CSR_EXTINMASK | EXI_CSR_TCINTMASK | EXI_CSR_EXIINTMASK);
	out_be32(csr_reg, csr);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
}
EXPORT_SYMBOL(exi_deselect_raw);

/**
 *	exi_transfer_raw  -  performs an exi transfer using immediate mode
 *	@exi_channel:	channel
 *	@data:		pointer to data being read/writen
 *	@len:		length of data
 *	@mode:		direction of transfer (EXI_OP_{READ,READWRITE,WRITE})
 *
 *	Read or write data on a given EXI channel.
 *
 */
void exi_transfer_raw(struct exi_channel *exi_channel,
		      void *data, size_t len, int mode)
{
	while (len >= 4) {
		__exi_transfer_raw_u32(exi_channel, data, mode);
		exi_channel->stats_xfers++;
		data += 4;
		len -= 4;
	}

	switch (len) {
	case 1:
		__exi_transfer_raw_u8(exi_channel, data, mode);
		exi_channel->stats_xfers++;
		break;
	case 2:
		__exi_transfer_raw_u16(exi_channel, data, mode);
		exi_channel->stats_xfers++;
		break;
	case 3:
		/* XXX optimize this case */
		__exi_transfer_raw_u16(exi_channel, data, mode);
		exi_channel->stats_xfers++;
		__exi_transfer_raw_u8(exi_channel, data+2, mode);
		exi_channel->stats_xfers++;
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(exi_transfer_raw);

/*
 * Internal. Start a transfer using "interrupt-driven immediate" mode.
 */
static void exi_start_idi_transfer_raw(struct exi_channel *exi_channel,
				       void *data, size_t len, int mode)
{
	void __iomem *io_base = exi_channel->io_base;
	u32 __iomem *csr_reg = io_base + EXI_CSR;
	u32 val = ~0;
	unsigned long flags;

	BUG_ON(len < 1 || len > 4);

	exi_channel->stats_idi_xfers++;
	exi_channel->stats_xfers++;

	if ((mode & EXI_OP_WRITE)) {
		switch (len) {
		case 1:
			val = *((u8 *)data) << 24;
			break;
		case 2:
			val = *((u16 *)data) << 16;
			break;
		case 3:
			val = *((u16 *)data) << 16;
			val |= *((u8 *)data+2) << 8;
			break;
		case 4:
			val = *((u32 *)data);
			break;
		default:
			break;
		}
	}

	out_be32(io_base + EXI_DATA, val);

	/* enable the Transfer Complete interrupt */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	out_be32(csr_reg, in_be32(csr_reg) | EXI_CSR_TCINTMASK);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);

	/* start the transfer */
	out_be32(io_base + EXI_CR,
		 EXI_CR_TSTART | EXI_CR_TLEN(len) | (mode&0xf));
}

/*
 * Internal. Finish a transfer using "interrupt-driven immediate" mode.
 */
static void exi_end_idi_transfer_raw(struct exi_channel *exi_channel,
				     void *data, size_t len, int mode)
{
	void __iomem *io_base = exi_channel->io_base;
	u32 val = ~0;

	BUG_ON(len < 1 || len > 4);

	if ((mode&0xf) != EXI_OP_WRITE) {
		val = in_be32(io_base + EXI_DATA);
		switch (len) {
		case 1:
			*((u8 *)data) = (u8)(val >> 24);
			break;
		case 2:
			*((u16 *)data) = (u16)(val >> 16);
			break;
		case 3:
			*((u16 *)data) = (u16)(val >> 16);
			*((u8 *)data+2) = (u8)(val >> 8);
			break;
		case 4:
			*((u32 *)data) = (u32)(val);
			break;
		default:
			break;
		}
	}
}

/*
 * Internal. Start a transfer using DMA mode.
 */
static void exi_start_dma_transfer_raw(struct exi_channel *exi_channel,
				       dma_addr_t data, size_t len, int mode)
{
	void __iomem *io_base = exi_channel->io_base;
	u32 __iomem *csr_reg = io_base + EXI_CSR;
	unsigned long flags;

	BUG_ON((data & EXI_DMA_ALIGN) != 0 ||
	       (len & EXI_DMA_ALIGN) != 0);

	exi_channel->stats_dma_xfers++;
	exi_channel->stats_xfers++;

	/*
	 * We clear the DATA register here to avoid confusing some
	 * special hardware, like SD cards.
	 * Indeed, we need all 1s here.
	 */
	out_be32(io_base + EXI_DATA, ~0);

	/* setup address and length of transfer */
	out_be32(io_base + EXI_MAR, data);
	out_be32(io_base + EXI_LENGTH, len);

	/* enable the Transfer Complete interrupt */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	out_be32(csr_reg, in_be32(csr_reg) | EXI_CSR_TCINTMASK);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);

	/* start the transfer */
	out_be32(io_base + EXI_CR, EXI_CR_TSTART | EXI_CR_DMA | (mode&0xf));
}


/*
 * Internal. Busy-wait until a DMA mode transfer operation completes.
 */
static void exi_wait_for_transfer_raw(struct exi_channel *exi_channel)
{
	u32 __iomem *cr_reg = exi_channel->io_base + EXI_CR;
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	unsigned long flags;
	unsigned long deadline = jiffies + 2*HZ;
	int borked = 0;

	/* we don't want TCINTs to disturb us while waiting */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	out_be32(csr_reg, in_be32(csr_reg) & ~EXI_CSR_TCINTMASK);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);

	/* busy-wait for transfer complete */
	while ((in_be32(cr_reg)&EXI_CR_TSTART) && !borked) {
		cpu_relax();
		borked = time_after(jiffies, deadline);
	}

	if (borked) {
		drv_printk(KERN_ERR, "exi transfer took too long, "
			   "is your hardware ok?");
	}

	/* ack the Transfer Complete interrupt */
	spin_lock_irqsave(&exi_channel->io_lock, flags);
	out_be32(csr_reg, in_be32(csr_reg) | EXI_CSR_TCINT);
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
}


/*
 *
 *
 */

static void exi_command_done(struct exi_command *cmd);

/*
 * Internal. Initialize an exi_channel structure.
 */
void exi_channel_init(struct exi_channel *exi_channel, unsigned int channel)
{
	memset(exi_channel, 0, sizeof(*exi_channel));
	exi_channel->events[EXI_EVENT_IRQ].id = EXI_EVENT_IRQ;
	exi_channel->events[EXI_EVENT_INSERT].id = EXI_EVENT_INSERT;
	exi_channel->events[EXI_EVENT_TC].id = EXI_EVENT_TC;

	spin_lock_init(&exi_channel->lock);
	spin_lock_init(&exi_channel->io_lock);
	init_waitqueue_head(&exi_channel->wait_queue);

	exi_channel->channel = channel;
	exi_channel->io_base = exi_io_mem + channel * EXI_CHANNEL_SPACING;

	tasklet_init(&exi_channel->tasklet,
		     exi_tasklet, (unsigned long)exi_channel);
}

/*
 * Internal. Check if an exi channel has delayed work to do.
 */
static void exi_check_pending_work(void)
{
	struct exi_channel *exi_channel;

	exi_channel_for_each(exi_channel) {
		if (exi_channel->csr)
			tasklet_schedule(&exi_channel->tasklet);
	}
}

/*
 * Internal. Finish a DMA transfer.
 * Caller holds the channel lock.
 */
static void exi_end_dma_transfer(struct exi_channel *exi_channel)
{
	struct exi_command *cmd;

	cmd = exi_channel->queued_cmd;
	if (cmd) {
		BUG_ON(!(exi_channel->flags & EXI_DMABUSY));

		exi_channel->flags &= ~EXI_DMABUSY;
		dma_unmap_single(&exi_channel->owner->dev,
				 cmd->dma_addr, cmd->dma_len,
				 (cmd->opcode == EXI_OP_READ) ?
				 DMA_FROM_DEVICE : DMA_TO_DEVICE);

		exi_channel->queued_cmd = NULL;
	}
}

/*
 * Internal. Finish an "interrupt-driven immediate" transfer.
 * Caller holds the channel lock.
 *
 * If more data is pending transfer, it schedules a new transfer.
 * Returns zero if no more transfers are required, non-zero otherwise.
 *
 */
static int exi_end_idi_transfer(struct exi_channel *exi_channel)
{
	struct exi_command *cmd;
	int len, offset;
	unsigned int balance = 16 /* / sizeof(u32) */;

	cmd = exi_channel->queued_cmd;
	if (cmd) {
		BUG_ON((exi_channel->flags & EXI_DMABUSY));

		len = (cmd->bytes_left > 4) ? 4 : cmd->bytes_left;
		offset = cmd->len - cmd->bytes_left;
		exi_end_idi_transfer_raw(exi_channel,
					 cmd->data + offset, len,
					 cmd->opcode);
		cmd->bytes_left -= len;

		if (balance && cmd->bytes_left > 0) {
			offset += len;
			len = (cmd->bytes_left > balance) ?
					balance : cmd->bytes_left;
			exi_transfer_raw(exi_channel,
					 cmd->data + offset, len, cmd->opcode);
			cmd->bytes_left -= len;
		}

		if (cmd->bytes_left > 0) {
			offset = cmd->len - cmd->bytes_left;
			len = (cmd->bytes_left > 4) ? 4 : cmd->bytes_left;

			exi_start_idi_transfer_raw(exi_channel,
						   cmd->data + offset, len,
						   cmd->opcode);
		} else {
			exi_channel->queued_cmd = NULL;
		}
	}

	return (exi_channel->queued_cmd) ? 1 : 0;
}

/*
 * Internal. Wait until a single transfer completes, and launch callbacks
 * when the whole transfer is completed.
 */
static int exi_wait_for_transfer_one(struct exi_channel *exi_channel)
{
	struct exi_command *cmd;
	unsigned long flags;
	int pending = 0;

	spin_lock_irqsave(&exi_channel->lock, flags);

	exi_wait_for_transfer_raw(exi_channel);

	cmd = exi_channel->queued_cmd;
	if (cmd) {
		if ((exi_channel->flags & EXI_DMABUSY)) {
			/* dma transfers need just one transfer */
			exi_end_dma_transfer(exi_channel);
		} else {
			pending = exi_end_idi_transfer(exi_channel);
		}

		spin_unlock_irqrestore(&exi_channel->lock, flags);

		if (!pending)
			exi_command_done(cmd);
		goto out;
	}

	spin_unlock_irqrestore(&exi_channel->lock, flags);
out:
	return pending;
}

#if 0
/*
 * Internal. Wait until a full transfer completes and launch callbacks.
 */
static void exi_wait_for_transfer(struct exi_channel *exi_channel)
{
	while (exi_wait_for_transfer_one(exi_channel))
		cpu_relax();
}
#endif

/*
 * Internal. Call any done hooks.
 */
static void exi_command_done(struct exi_command *cmd)
{
	/* if specified, call the completion routine */
	if (cmd->done)
		cmd->done(cmd);
}

/*
 * Internal. Take a channel.
 */
static int exi_take_channel(struct exi_channel *exi_channel,
			    struct exi_device *exi_device, int wait)
{
	unsigned long flags;
	int result = 0;

	BUG_ON(!exi_device);

	spin_lock_irqsave(&exi_channel->lock, flags);
	while (exi_channel->owner) {
		spin_unlock_irqrestore(&exi_channel->lock, flags);
		if (!wait)
			return -EBUSY;
		wait_event(exi_channel->wait_queue,
			   !exi_channel->owner);
		spin_lock_irqsave(&exi_channel->lock, flags);
	}
	exi_channel->owner = exi_device;
	spin_unlock_irqrestore(&exi_channel->lock, flags);

	return result;
}

/*
 * Internal. Give a channel.
 */
static int exi_give_channel(struct exi_channel *exi_channel)
{
	WARN_ON(exi_channel->owner == NULL);
	exi_channel->owner = NULL;
	wake_up(&exi_channel->wait_queue);
	return 0;
}

/*
 * Internal. Perform the post non-DMA transfer associated to a DMA transfer.
 */
static void exi_cmd_post_transfer(struct exi_command *cmd)
{
	struct exi_channel *exi_channel = cmd->exi_channel;
	struct exi_command *post_cmd = &exi_channel->post_cmd;

	DBG("channel=%d\n", exi_channel->channel);

	exi_transfer_raw(exi_channel, post_cmd->data, post_cmd->len,
			 post_cmd->opcode);

	cmd->done_data = post_cmd->done_data;
	cmd->done = post_cmd->done;
	exi_op_nop(post_cmd, exi_channel);
	exi_command_done(cmd);
}


#define exi_align_next(x) (void *) \
			  (((unsigned long)(x)+EXI_DMA_ALIGN)&~EXI_DMA_ALIGN)
#define exi_align_prev(x) (void *) \
			  ((unsigned long)(x)&~EXI_DMA_ALIGN)
#define exi_is_aligned(x) (void *) \
			  (!((unsigned long)(x)&EXI_DMA_ALIGN))

/*
 * Internal. Perform a transfer.
 * Caller holds the channel lock.
 */
static int exi_cmd_transfer(struct exi_command *cmd)
{
	struct exi_channel *exi_channel = cmd->exi_channel;
	struct exi_command *post_cmd = &exi_channel->post_cmd;
	void *pre_data, *data, *post_data;
	unsigned int pre_len, len, post_len;
	int opcode;
	int result = 0;

	BUG_ON(!exi_channel->owner);

	len = cmd->len;
	if (!len)
		goto done;

	DBG("channel=%d, opcode=%d\n", exi_channel->channel, cmd->opcode);

	opcode = cmd->opcode;
	data = cmd->data;

	/* interrupt driven immediate transfer... */
	if ((cmd->flags & EXI_CMD_IDI)) {
		exi_channel->queued_cmd = cmd;
		exi_channel->flags &= ~EXI_DMABUSY;

		cmd->bytes_left = cmd->len;
		len = (cmd->bytes_left > 4) ? 4 : cmd->bytes_left;
		exi_start_idi_transfer_raw(exi_channel, data, len, opcode);

		result = 1; /* wait */
		goto done;
	}

	/*
	 * We can't do DMA transfers unless we have at least 32 bytes.
	 * And we won't do DMA transfers if user requests that.
	 */
	if (len < EXI_DMA_ALIGN+1 || (cmd->flags & EXI_CMD_NODMA)) {
		exi_transfer_raw(exi_channel, data, len, opcode);
		goto done;
	}

	/*
	 * |_______________|______...______|_______________| DMA alignment
	 *     <--pre_len--><---- len -----><-post_len->
	 *     +-----------+------...------+-----------+
	 *     | pre_data  | data          | post_data |
	 *     | non-DMA   | DMA           | non-DMA   |
	 *     +-----------+------...------+-----------+
	 *       < 32 bytes  N*32 bytes      < 32 bytes
	 *     |<--------->|<-----...----->|<--------->|
	 *     <-------------- cmd->len --------------->
	 */

	pre_data = data;
	post_data = exi_align_prev(pre_data+len);
	data = exi_align_next(pre_data);

	pre_len = data - pre_data;
	post_len = (pre_data + len) - post_data;
	len = post_data - data;

	/*
	 * Coalesce pre and post data transfers if no DMA transfer is possible.
	 */
	if (!len) {
		/*
		 * Maximum transfer size here is 31+31=62 bytes.
		 */
		exi_transfer_raw(exi_channel, pre_data,
				 pre_len + post_len, opcode);
		goto done;
	}

	/*
	 * The first unaligned chunk can't use DMA.
	 */
	if (pre_len > 0) {
		/*
		 * Maximum transfer size here is 31 bytes.
		 */
		exi_transfer_raw(exi_channel, pre_data, pre_len, opcode);
	}

	/*
	 * Perform a DMA transfer on the aligned data, followed by a non-DMA
	 * data transfer on the remaining data.
	 */
	if (post_len > 0) {
		/*
		 * Maximum transfer size here will be 31 bytes.
		 */
		exi_op_transfer(post_cmd, exi_channel,
				post_data, post_len, opcode);
		post_cmd->done_data = cmd->done_data;
		post_cmd->done = cmd->done;
		cmd->done_data = NULL;
		cmd->done = exi_cmd_post_transfer;
	}

	exi_channel->queued_cmd = cmd;
	exi_channel->flags |= EXI_DMABUSY;

	cmd->dma_len = len;
	cmd->dma_addr = dma_map_single(&exi_channel->owner->dev,
				       data, len,
				       (cmd->opcode == EXI_OP_READ) ?
				       DMA_FROM_DEVICE : DMA_TO_DEVICE);

	exi_start_dma_transfer_raw(exi_channel, cmd->dma_addr, len, opcode);

	result = 1; /* wait */

done:
	return result;
}

/**
 *	exi_run_command  -  executes a single exi command
 *	@cmd:	the command to execute
 *
 *	Context: user
 *
 *	Run just one command.
 *
 */
static int exi_run_command(struct exi_command *cmd)
{
	struct exi_channel *exi_channel = cmd->exi_channel;
	struct exi_device *exi_device = cmd->exi_device;
	unsigned long flags;
	int wait = !(cmd->flags & EXI_CMD_NOWAIT);
	int result = 0;

	if (cmd->opcode != EXI_OP_TAKE)
		WARN_ON(exi_channel->owner != exi_device);

	switch (cmd->opcode) {
	case EXI_OP_NOP:
		break;
	case EXI_OP_TAKE:
		result = exi_take_channel(exi_channel, exi_device, wait);
		break;
	case EXI_OP_GIVE:
		result = exi_give_channel(exi_channel);
		if (!exi_channel->owner)
			exi_check_pending_work();
		break;
	case EXI_OP_SELECT:
		exi_select_raw(exi_channel, exi_device->eid.device,
			       exi_device->frequency);
		break;
	case EXI_OP_DESELECT:
		exi_deselect_raw(exi_channel);
		break;
	case EXI_OP_READ:
	case EXI_OP_READWRITE:
	case EXI_OP_WRITE:
		spin_lock_irqsave(&exi_channel->lock, flags);
		result = exi_cmd_transfer(cmd);
		spin_unlock_irqrestore(&exi_channel->lock, flags);
		break;
	default:
		result = -ENOSYS;
		break;
	}

	/*
	 * We check for delayed work every time the channel becomes
	 * idle.
	 */
	if (!result)
		exi_command_done(cmd);

	return result;
}


/*
 * Internal. Completion routine.
 */
static void exi_wait_done(struct exi_command *cmd)
{
	complete(cmd->done_data);
}

/*
 * Internal. Run a command and wait.
 * Might sleep if called from user context. Otherwise will busy-wait.
 */
static int exi_run_command_and_wait(struct exi_command *cmd)
{
	DECLARE_COMPLETION(complete);
	int result;

	cmd->done_data = &complete;
	cmd->done = exi_wait_done;
	result = exi_run_command(cmd);
	if (result > 0) {
		wait_for_completion(&complete);
		result = 0;
	}
	return result;
}

/**
 *	exi_take  -  reserves an exi channel for exclusive use by a device
 *	@exi_device:	exi device making the reservation
 *	@wait:		wait for the operation to complete
 *
 *	Reserves the channel of a given EXI device.
 */
int exi_take(struct exi_device *exi_device, int wait)
{
	struct exi_command cmd;

	exi_op_take(&cmd, exi_device);
	if (!wait)
		cmd.flags |= EXI_CMD_NOWAIT;
	return exi_run_command(&cmd);
}
EXPORT_SYMBOL(exi_take);

/**
 *	exi_give  -  releases an exi channel
 *	@exi_device:	exi device making the release
 *
 *	Releases the channel of a given EXI device.
 */
int exi_give(struct exi_device *exi_device)
{
	struct exi_command cmd;

	exi_op_give(&cmd, exi_device->exi_channel);
	return exi_run_command(&cmd);
}
EXPORT_SYMBOL(exi_give);

/**
 *	exi_select  -  selects a exi device
 *	@exi_device:	exi device being selected
 *
 *	Selects a given EXI device.
 */
void exi_select(struct exi_device *exi_device)
{
	struct exi_command cmd;

	exi_op_select(&cmd, exi_device);
	exi_run_command(&cmd);
}
EXPORT_SYMBOL(exi_select);

/**
 *	exi_deselect  -  deselects all devices on an exi channel
 *	@exi_channel:	channel
 *
 *	Deselects all EXI devices on the given channel.
 *
 */
void exi_deselect(struct exi_channel *exi_channel)
{
	struct exi_command cmd;

	exi_op_deselect(&cmd, exi_channel);
	exi_run_command(&cmd);
}
EXPORT_SYMBOL(exi_deselect);

/**
 *	exi_transfer  -  Performs a read or write EXI transfer.
 *	@exi_channel:	channel
 *	@data:		pointer to data being read/written
 *	@len:		length of data
 *	@opcode:	operation code (EXI_OP_{READ,READWRITE,WRITE})
 *
 *	Read or write data on a given EXI channel.
 */
void exi_transfer(struct exi_channel *exi_channel, void *data, size_t len,
		   int opcode, unsigned long flags)
{
	struct exi_command cmd;

	exi_op_transfer(&cmd, exi_channel, data, len, opcode);
	cmd.flags |= flags;
	exi_run_command_and_wait(&cmd);
}
EXPORT_SYMBOL(exi_transfer);

/*
 * Internal. Release several previously reserved channels, according to a
 * channel mask.
 */
static void __give_some_channels(unsigned int channel_mask)
{
	struct exi_channel *exi_channel;
	unsigned int channel;

	for (channel = 0; channel_mask && channel < EXI_MAX_CHANNELS;
	     channel++) {
		if ((channel_mask & (1<<channel))) {
			channel_mask &= ~(1<<channel);
			exi_channel = __to_exi_channel(channel);
			exi_channel->owner = NULL;
		}
	}
}

/*
 * Internal. Try to reserve atomically several channels, according to a
 * channel mask.
 */
static inline int __try_take_some_channels(unsigned int channel_mask,
					   struct exi_device *exi_device)
{
	struct exi_channel *exi_channel;
	unsigned int channel, taken_channel_mask = 0;
	unsigned long flags;
	int result = 0;

	for (channel = 0; channel_mask && channel < EXI_MAX_CHANNELS;
	     channel++) {
		if ((channel_mask & (1<<channel))) {
			channel_mask &= ~(1<<channel);
			exi_channel = __to_exi_channel(channel);
			spin_lock_irqsave(&exi_channel->lock, flags);
			if (exi_channel->owner) {
				spin_unlock_irqrestore(&exi_channel->lock,
						       flags);
				result = -EBUSY;
				break;
			}
			exi_channel->owner = exi_device;
			taken_channel_mask |= (1<<channel);
			spin_unlock_irqrestore(&exi_channel->lock, flags);
		}
	}

	if (result)
		__give_some_channels(taken_channel_mask);

	return result;
}

/*
 * Internal. Determine if we can trigger an exi event.
 */
static inline int exi_can_trigger_event(struct exi_event *event)
{
	return !__try_take_some_channels(event->channel_mask, event->owner);
}

/*
 * Internal. Finish an exi event invocation.
 */
static inline void exi_finish_event(struct exi_event *event)
{
	__give_some_channels(event->channel_mask);
}

/*
 * Internal. Trigger an exi event.
 */
static inline int exi_trigger_event(struct exi_channel *exi_channel,
				    struct exi_event *event)
{
	exi_event_handler_t handler;
	int result = 0;

	handler = event->handler;
	if (handler)
		result = handler(exi_channel, event->id, event->data);
	return result;
}

/*
 * Internal. Conditionally trigger an exi event.
 */
static void exi_cond_trigger_event(struct exi_channel *exi_channel,
				   unsigned int event_id, int csr_mask)
{
	struct exi_event *event;
	unsigned long flags;

	if ((exi_channel->csr & csr_mask)) {
		event = &exi_channel->events[event_id];
		if (exi_can_trigger_event(event)) {
			spin_lock_irqsave(&exi_channel->lock, flags);
			exi_channel->csr &= ~csr_mask;
			spin_unlock_irqrestore(&exi_channel->lock, flags);
			exi_trigger_event(exi_channel, event);
			exi_finish_event(event);
		}
	}

	return;
}

/*
 * Internal. Tasklet used to execute delayed work.
 */
static void exi_tasklet(unsigned long param)
{
	struct exi_channel *exi_channel = (struct exi_channel *)param;

	DBG("channel=%d, csr=%08lx\n", exi_channel->channel, exi_channel->csr);

	if (exi_channel->queued_cmd) {
		DBG("tasklet while xfer in flight on channel %d, csr = %08lx\n",
		    exi_channel->channel, exi_channel->csr);
	}

	/*
	 * We won't lauch event handlers if any of the channels we
	 * provided on event registration is in use.
	 */

	/*exi_cond_trigger_event(exi_channel, EXI_EVENT_TC, EXI_CSR_TCINT);*/
	exi_cond_trigger_event(exi_channel, EXI_EVENT_IRQ, EXI_CSR_EXIINT);
	exi_cond_trigger_event(exi_channel, EXI_EVENT_INSERT, EXI_CSR_EXTIN);
}

/*
 * Internal. Interrupt handler for EXI interrupts.
 */
static irqreturn_t exi_irq_handler(int irq, void *dev_id)
{
	struct exi_channel *exi_channel;
	u32 __iomem *csr_reg;
	u32 csr, status, mask;
	unsigned long flags;

	exi_channel_for_each(exi_channel) {
		csr_reg = exi_channel->io_base + EXI_CSR;

		/*
		 * Determine if we have pending interrupts on this channel,
		 * and which ones.
		 */
		spin_lock_irqsave(&exi_channel->io_lock, flags);

		csr = in_be32(csr_reg);
		mask = csr & (EXI_CSR_EXTINMASK |
			      EXI_CSR_TCINTMASK | EXI_CSR_EXIINTMASK);
		status = csr & (mask << 1);
		if (!status) {
			spin_unlock_irqrestore(&exi_channel->io_lock, flags);
			continue;
		}

		/* XXX do not signal TC events for now... */
		exi_channel->csr |= (status & ~EXI_CSR_TCINT);

		DBG("channel=%d, csr=%08lx\n", exi_channel->channel,
		    exi_channel->csr);

		/* ack all for this channel */
		out_be32(csr_reg, csr | status);

		spin_unlock_irqrestore(&exi_channel->io_lock, flags);

		if ((status & EXI_CSR_TCINT))
			exi_wait_for_transfer_one(exi_channel);
		if ((status & EXI_CSR_EXTIN))
			wake_up(&exi_bus_waitq);

		if (exi_channel->csr && !exi_is_taken(exi_channel))
			tasklet_schedule(&exi_channel->tasklet);
	}
	return IRQ_HANDLED;
}

/*
 * Internal. Enable an exi event.
 */
static int exi_enable_event(struct exi_channel *exi_channel,
			    unsigned int event_id)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = in_be32(csr_reg);

	/* ack and enable the associated interrupt */
	switch (event_id) {
	case EXI_EVENT_INSERT:
		out_be32(csr_reg, csr | (EXI_CSR_EXTIN | EXI_CSR_EXTINMASK));
		break;
	case EXI_EVENT_TC:
		/*out_be32(csr_reg,
			   csr | (EXI_CSR_TCINT | EXI_CSR_TCINTMASK));*/
		break;
	case EXI_EVENT_IRQ:
		out_be32(csr_reg, csr | (EXI_CSR_EXIINT | EXI_CSR_EXIINTMASK));
		break;
	}
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
	return 0;
}

/*
 * Internal. Disable an exi event.
 */
static int exi_disable_event(struct exi_channel *exi_channel,
			     unsigned int event_id)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 csr;
	unsigned long flags;

	spin_lock_irqsave(&exi_channel->io_lock, flags);
	csr = in_be32(csr_reg);

	/* ack and disable the associated interrupt */
	switch (event_id) {
	case EXI_EVENT_INSERT:
		out_be32(csr_reg, (csr | EXI_CSR_EXTIN) & ~EXI_CSR_EXTINMASK);
		break;
	case EXI_EVENT_TC:
		/*out_be32(csr_reg,
			   (csr | EXI_CSR_TCINT) & ~EXI_CSR_TCINTMASK);*/
		break;
	case EXI_EVENT_IRQ:
		out_be32(csr_reg, (csr | EXI_CSR_EXIINT) & ~EXI_CSR_EXIINTMASK);
		break;
	}
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);
	return 0;
}

/**
 *	exi_event_register  -  Registers an event on a given channel.
 *	@exi_channel:	channel
 *	@event_id:	event id
 *	@handler:	event handler
 *	@data:		data passed to event handler
 *
 *	Register a handler to be called whenever a specified event happens
 *	on the given channel.
 */
int exi_event_register(struct exi_channel *exi_channel, unsigned int event_id,
		       struct exi_device *exi_device,
		       exi_event_handler_t handler, void *data,
		       unsigned int channel_mask)
{
	struct exi_event *event;
	int result = 0;

	BUG_ON(event_id > EXI_MAX_EVENTS);

	event = &exi_channel->events[event_id];

	spin_lock(&exi_channel->lock);
	if (event->handler) {
		result = -EBUSY;
		goto out;
	}
	event->owner = exi_device;
	event->handler = handler;
	event->data = data;
	event->channel_mask = channel_mask;
	exi_enable_event(exi_channel, event_id);

out:
	spin_unlock(&exi_channel->lock);
	return result;
}
EXPORT_SYMBOL(exi_event_register);

/**
 *	exi_event_unregister  -  Unregisters an event on a given channel.
 *	@exi_channel:	channel
 *	@event_id:	event id
 *
 *	Unregister a previously registered event handler.
 */
int exi_event_unregister(struct exi_channel *exi_channel, unsigned int event_id)
{
	struct exi_event *event;
	int result = 0;

	BUG_ON(event_id > EXI_MAX_EVENTS);

	event = &exi_channel->events[event_id];

	spin_lock(&exi_channel->lock);
	exi_disable_event(exi_channel, event_id);
	event->owner = NULL;
	event->handler = NULL;
	event->data = NULL;
	event->channel_mask = 0;
	spin_unlock(&exi_channel->lock);

	return result;
}
EXPORT_SYMBOL(exi_event_unregister);

/*
 * Internal. Quiesce a channel.
 */
static void exi_quiesce_channel(struct exi_channel *exi_channel, u32 csr_mask)
{
	/* wait for dma transfers to complete */
	exi_wait_for_transfer_raw(exi_channel);

	/* ack and mask all interrupts */
	out_be32(exi_channel->io_base + EXI_CSR,
		EXI_CSR_TCINT  | EXI_CSR_EXIINT | EXI_CSR_EXTIN | csr_mask);
}

/*
 * Internal. Quiesce all channels.
 */
static void exi_quiesce_all_channels(u32 csr_mask)
{
	struct exi_channel *exi_channel;

	exi_channel_for_each(exi_channel) {
		exi_quiesce_channel(exi_channel, csr_mask);
	}
}

/**
 *	exi_get_id  -  Returns the EXI ID of a device
 *	@exi_channel:	channel
 *	@device:	device number on channel
 *	@freq:		clock frequency index
 *
 *	Returns the EXI ID of an EXI device on a given channel.
 *	Might sleep.
 */
u32 exi_get_id(struct exi_device *exi_device)
{
	struct exi_channel *exi_channel = exi_device->exi_channel;
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	u32 id = EXI_ID_INVALID;
	u16 cmd = 0;

	/* ask for the EXI id */
	exi_dev_take(exi_device);
	exi_dev_select(exi_device);
	exi_dev_write(exi_device, &cmd, sizeof(cmd));
	exi_dev_read(exi_device, &id, sizeof(id));
	exi_dev_deselect(exi_device);
	exi_dev_give(exi_device);

	/* "canonicalize" the id */
	if (!id)
		id = EXI_ID_INVALID;
	/*
	 * We return a EXI_ID_NONE if there is some unidentified device
	 * inserted in memcard slot A or memcard slot B.
	 * This, for example, allows the SD/MMC driver to see inserted cards.
	 */
	if (id == EXI_ID_INVALID) {
		if ((__to_channel(exi_channel) == 0 ||
		     __to_channel(exi_channel) == 1)
		    && exi_device->eid.device == 0) {
			if (in_be32(csr_reg) & EXI_CSR_EXT)
				id = EXI_ID_NONE;
		}
	}

	return id;
}
EXPORT_SYMBOL(exi_get_id);

/*
 * Tells if there is a device inserted in one of the memory card slots.
 */
int exi_get_ext_line(struct exi_channel *exi_channel)
{
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;
	return (in_be32(csr_reg) & EXI_CSR_EXT) ? 1 : 0;
}

/*
 * Saves the current insertion status of a given channel.
 */
void exi_update_ext_status(struct exi_channel *exi_channel)
{
	if (exi_get_ext_line(exi_channel))
		exi_channel->flags |= EXI_EXT;
	else
		exi_channel->flags &= ~EXI_EXT;
}

void exi_hw_quiesce(void)
{
	exi_quiesce_all_channels(0);
}

/*
 * Pseudo-Internal. Initialize basic channel structures and hardware.
 */
int exi_hw_init(char *module_name, struct resource *mem, unsigned int irq)
{
	struct exi_channel *exi_channel;
	int channel;
	int result;

	exi_io_mem = ioremap(mem->start, mem->end - mem->start + 1);
	if (!exi_io_mem) {
		drv_printk(KERN_ERR, "ioremap failed\n");
		return -ENOMEM;
	}

	for (channel = 0; channel < EXI_MAX_CHANNELS; channel++) {
		exi_channel = __to_exi_channel(channel);

		/* initialize a channel structure */
		exi_channel_init(exi_channel, channel);
	}

	/* calm down the hardware and allow extractions */
	exi_quiesce_all_channels(EXI_CSR_EXTINMASK);

	/* register the exi interrupt handler */
	result = request_irq(irq, exi_irq_handler, 0, module_name, NULL);
	if (result)
		drv_printk(KERN_ERR, "failed to register IRQ %d\n", irq);

	return result;
}

/*
 * Pseudo-Internal.
 */
void exi_hw_exit(struct resource *mem, unsigned int irq)
{
	exi_quiesce_all_channels(0);
	iounmap(exi_io_mem);
	free_irq(irq, NULL);
}
