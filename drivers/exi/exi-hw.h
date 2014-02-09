/*
 * drivers/exi/exi-hw.h
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

#ifndef __EXI_HW_H
#define __EXI_HW_H

#include <linux/exi.h>
#include <linux/interrupt.h>
#include <linux/resource.h>
#include <asm/atomic.h>


#define EXI_MAX_CHANNELS	3  /* channels on the EXI bus */
#define EXI_DEVICES_PER_CHANNEL	3  /* number of devices per EXI channel */
#define EXI_MAX_EVENTS		3  /* types of events on the EXI bus */

#define EXI_CLK_1MHZ		0
#define EXI_CLK_2MHZ		1
#define EXI_CLK_4MHZ		2
#define EXI_CLK_8MHZ		3
#define EXI_CLK_16MHZ		4
#define EXI_CLK_32MHZ		5

#define EXI_MAX_FREQ		7
#define EXI_FREQ_SCAN		EXI_CLK_8MHZ

#define EXI_READ		0
#define EXI_WRITE		1

#define EXI_IDI_MAX_SIZE	4


#define EXI_DMA_ALIGN		0x1f /* 32 bytes */

#define EXI_CHANNEL_SPACING	0x14

#define EXI_CSR			0x00
#define   EXI_CSR_EXIINTMASK	(1<<0)
#define   EXI_CSR_EXIINT	(1<<1)
#define   EXI_CSR_TCINTMASK	(1<<2)
#define   EXI_CSR_TCINT		(1<<3)
#define   EXI_CSR_CLKMASK	(0x7<<4)
#define     EXI_CSR_CLK_1MHZ	(EXI_CLK_1MHZ<<4)
#define     EXI_CSR_CLK_2MHZ	(EXI_CLK_2MHZ<<4)
#define     EXI_CSR_CLK_4MHZ	(EXI_CLK_4MHZ<<4)
#define     EXI_CSR_CLK_8MHZ	(EXI_CLK_8MHZ<<4)
#define     EXI_CSR_CLK_16MHZ	(EXI_CLK_16MHZ<<4)
#define     EXI_CSR_CLK_32MHZ	(EXI_CLK_32MHZ<<4)
#define   EXI_CSR_CSMASK	(0x7<<7)
#define     EXI_CSR_CS_0	(0x1<<7)  /* Chip Select 001 */
#define     EXI_CSR_CS_1	(0x2<<7)  /* Chip Select 010 */
#define     EXI_CSR_CS_2	(0x4<<7)  /* Chip Select 100 */
#define   EXI_CSR_EXTINMASK	(1<<10)
#define   EXI_CSR_EXTIN		(1<<11)
#define   EXI_CSR_EXT		(1<<12)

#define EXI_MAR			0x04

#define EXI_LENGTH		0x08

#define EXI_CR			0x0c
#define   EXI_CR_TSTART		(1<<0)
#define   EXI_CR_DMA		(1<<1)
#define   EXI_CR_READ		(0<<2)
#define   EXI_CR_WRITE		(1<<2)
#define   EXI_CR_READ_WRITE	(2<<2)
#define   EXI_CR_TLEN(len)	(((len)-1)<<4)

#define EXI_DATA		0x10

enum {
	__EXI_DMABUSY = 0,
	__EXI_EXT,
};

/*
 * For registering event handlers with the exi layer.
 */
struct exi_event {
	int			id;		/* event id */
	struct exi_device	*owner;		/* device owning of the event */
	exi_event_handler_t	handler;
	void			*data;
	unsigned int		channel_mask;	/* channels used by handler */
};

/*
 * This structure represents an exi channel.
 */
struct exi_channel {
	spinlock_t		lock;		/* misc channel lock */

	int			channel;
	unsigned long		flags;
#define EXI_DMABUSY	(1<<__EXI_DMABUSY)
#define EXI_EXT		(1<<__EXI_EXT)

	spinlock_t		io_lock;	/* serializes access to CSR */
	void __iomem		*io_base;

	struct exi_device	*owner;
	wait_queue_head_t	wait_queue;

	struct exi_command	*queued_cmd;
	struct exi_command	post_cmd;

	unsigned long		csr;
	struct tasklet_struct	tasklet;

	unsigned long		stats_idi_xfers;
	unsigned long		stats_dma_xfers;
	unsigned long		stats_xfers;

	struct exi_event	events[EXI_MAX_EVENTS];
};

extern struct exi_device *exi_channel_owner(struct exi_channel *exi_channel);
extern int exi_get_ext_line(struct exi_channel *exi_channel);
extern void exi_update_ext_status(struct exi_channel *exi_channel);

extern void exi_hw_quiesce(void);

extern int exi_hw_init(char *name, struct resource *mem, unsigned int irq);
extern void exi_hw_exit(struct resource *mem, unsigned int irq);

#define exi_is_taken(x) ((x)->owner)

/*
 * Internal.
 * Declare simple transfer functions for single bytes, words and dwords,
 * and build a general transfer function based on that.
 */
#define __declare__exi_transfer_raw(_type, _val, _data, _on_write, _on_read) \
static inline void __exi_transfer_raw_##_type(struct exi_channel *exi_channel,\
					      _type * _data, int mode)	\
{									\
	u32 __iomem *csr_reg = exi_channel->io_base + EXI_CSR;		\
	u32 __iomem *data_reg = exi_channel->io_base + EXI_DATA;	\
	u32 __iomem *cr_reg = exi_channel->io_base + EXI_CR;		\
	u32 _val = ~0;							\
	unsigned long flags;						\
									\
	/*								\
	 * On reads we write too some known value to EXIxDATA because	\
	 * information currently stored there is leaked to the		\
	 * MOSI line, confusing some hardware.				\
	 */								\
	if (((mode&0xf) != EXI_OP_READ)) /* write or read-write */	\
		_on_write;						\
	out_be32(data_reg, _val);					\
									\
	/* start transfer */						\
	_val = EXI_CR_TSTART | EXI_CR_TLEN(sizeof(_type)) | (mode&0xf);	\
	out_be32(cr_reg, _val);						\
									\
	/* wait for transfer completion */				\
	while (in_be32(cr_reg) & EXI_CR_TSTART)				\
		cpu_relax();						\
									\
	/* XXX check if we need that on immediate mode */		\
	/* assert transfer complete interrupt */			\
	spin_lock_irqsave(&exi_channel->io_lock, flags);		\
	out_be32(csr_reg, in_be32(csr_reg) | EXI_CSR_TCINT);		\
	spin_unlock_irqrestore(&exi_channel->io_lock, flags);		\
									\
	if ((mode&0xf) != EXI_OP_WRITE) { /* read or read-write */	\
		_val = in_be32(data_reg);				\
		_on_read;						\
	}								\
}

#define __declare__exi_transfer_raw_simple(_type) \
__declare__exi_transfer_raw(						\
		_type, _v, _d,						\
		_v = *(_d) << (32 - (8*sizeof(_type))),			\
		*(_d) = (_type)(_v >> (32 - (8*sizeof(_type))))		\
	)

__declare__exi_transfer_raw_simple(u8)
__declare__exi_transfer_raw_simple(u16)
__declare__exi_transfer_raw_simple(u32)

extern wait_queue_head_t exi_bus_waitq;

#endif /* __EXI_HW_H */
