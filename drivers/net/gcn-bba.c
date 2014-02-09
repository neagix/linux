/**
 * drivers/net/gcn-bba.c
 *
 * Nintendo GameCube Broadband Adapter (BBA) driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2005 Todd Jeffreys
 * Copyright (C) 2004,2005,2006,2007,2008,2009 Albert Herranz
 *
 * Based on previous work by Stefan Esser, Franz Lehner, Costis and tmbinc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define BBA_DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/io.h>
#include <linux/exi.h>
#include <asm/system.h>


#define DRV_MODULE_NAME	"gcn-bba"
#define DRV_DESCRIPTION	"Nintendo GameCube Broadband Adapter (BBA) driver"
#define DRV_AUTHOR	"Albert Herranz, " \
			"Todd Jeffreys"

static char bba_driver_version[] = "1.4i";


#define bba_printk(level, format, arg...) \
	 printk(level DRV_MODULE_NAME ": " format , ## arg)

#ifdef BBA_DEBUG
#  define DBG(fmt, args...) \
	   printk(KERN_ERR "%s: " fmt, __func__ , ## args)
#else
#  define DBG(fmt, args...)
#endif

/*
 * EXpansion Interface glue for the Broadband Adapter.
 *
 */
#define BBA_EXI_ID 0x04020200

#define BBA_EXI_IRQ_CHANNEL 2 /* INT line uses EXI2INTB */
#define BBA_EXI_CHANNEL     0 /* rest of lines use EXI0xxx */
#define BBA_EXI_DEVICE      2 /* chip select, EXI0CSB2 */
#define BBA_EXI_FREQ        5 /* 32MHz */

#define BBA_CMD_IR_MASKALL  0x00
#define BBA_CMD_IR_MASKNONE 0xf8

static inline void bba_select(void);
static inline void bba_deselect(void);
static inline void bba_write(void *data, size_t len);
static inline void bba_read(void *data, size_t len);

static void bba_ins(int reg, void *val, int len);
static void bba_outs(int reg, void *val, int len);

/*
 * Command Registers I/O.
 */

static inline void bba_cmd_ins_nosel(int reg, void *val, int len)
{
	u16 req;
	req = reg << 8;
	bba_write(&req, sizeof(req));
	bba_read(val, len);
}

static void bba_cmd_ins(int reg, void *val, int len)
{
	bba_select();
	bba_cmd_ins_nosel(reg, val, len);
	bba_deselect();
}

static inline void bba_cmd_outs_nosel(int reg, void *val, int len)
{
	u16 req;
	req = (reg << 8) | 0x4000;
	bba_write(&req, sizeof(req));
	bba_write(val, len);
}

static void bba_cmd_outs(int reg, void *val, int len)
{
	bba_select();
	bba_cmd_outs_nosel(reg, val, len);
	bba_deselect();
}

static inline u8 bba_cmd_in8(int reg)
{
	u8 val;
	bba_cmd_ins(reg, &val, sizeof(val));
	return val;
}

static u8 bba_cmd_in8_slow(int reg)
{
	u8 val;
	bba_select();
	bba_cmd_ins_nosel(reg, &val, sizeof(val));
	udelay(200);
	bba_deselect();
	return val;
}

static inline void bba_cmd_out8(int reg, u8 val)
{
	bba_cmd_outs(reg, &val, sizeof(val));
}


/*
 * Registers I/O.
 */

static inline u8 bba_in8(int reg)
{
	u8 val;
	bba_ins(reg, &val, sizeof(val));
	return val;
}

static inline void bba_out8(int reg, u8 val)
{
	bba_outs(reg, &val, sizeof(val));
}

static inline u16 bba_in16(int reg)
{
	u16 val;
	bba_ins(reg, &val, sizeof(val));
	return le16_to_cpup(&val);
}

static inline void bba_out16(int reg, u16 val)
{
	cpu_to_le16s(&val);
	bba_outs(reg, &val, sizeof(val));
}

#define bba_in12(reg)      (bba_in16(reg) & 0x0fff)
#define bba_out12(reg, val) do { bba_out16(reg, (val)&0x0fff); } while (0)

static inline void bba_ins_nosel(int reg, void *val, int len)
{
	u32 req;
	req = (reg << 8) | 0x80000000;
	bba_write(&req, sizeof(req));
	bba_read(val, len);
}

static void bba_ins(int reg, void *val, int len)
{
	bba_select();
	bba_ins_nosel(reg, val, len);
	bba_deselect();
}

static inline void bba_outs_nosel(int reg, void *val, int len)
{
	u32 req;
	req = (reg << 8) | 0xC0000000;
	bba_write(&req, sizeof(req));
	bba_write(val, len);
}

static inline void bba_outs_nosel_continued(void *val, int len)
{
	bba_write(val, len);
}

static void bba_outs(int reg, void *val, int len)
{
	bba_select();
	bba_outs_nosel(reg, val, len);
	bba_deselect();
}


/*
 * Macronix mx98728ec supporting bits.
 *
 */

#define BBA_NCRA 0x00		/* Network Control Register A, RW */
#define   BBA_NCRA_RESET	(1<<0)	/* RESET */
#define   BBA_NCRA_ST0		(1<<1)	/* ST0, Start transmit command/status */
#define   BBA_NCRA_ST1		(1<<2)	/* ST1,  " */
#define   BBA_NCRA_SR		(1<<3)	/* SR, Start Receive */

#define BBA_NCRB 0x01		/* Network Control Register B, RW */
#define   BBA_NCRB_PR		(1<<0)	/* PR, Promiscuous Mode */
#define   BBA_NCRB_CA		(1<<1)	/* CA, Capture Effect Mode */
#define   BBA_NCRB_PM		(1<<2)	/* PM, Pass Multicast */
#define   BBA_NCRB_PB		(1<<3)	/* PB, Pass Bad Frame */
#define   BBA_NCRB_AB		(1<<4)	/* AB, Accept Broadcast */
#define   BBA_NCRB_HBD		(1<<5)	/* HBD, reserved */
#define   BBA_NCRB_RXINTC0	(1<<6)	/* RXINTC, Receive Interrupt Counter */
#define   BBA_NCRB_RXINTC1	(1<<7)	/*  " */
#define     BBA_NCRB_1_PACKET_PER_INT	(0<<6)	/* 0 0 */
#define     BBA_NCRB_2_PACKETS_PER_INT	(1<<6)	/* 0 1 */
#define     BBA_NCRB_4_PACKETS_PER_INT	(2<<6)	/* 1 0 */
#define     BBA_NCRB_8_PACKETS_PER_INT	(3<<6)	/* 1 1 */

#define BBA_LTPS 0x04		/* Last Transmitted Packet Status, RO */
#define BBA_LRPS 0x05		/* Last Received Packet Status, RO */

#define BBA_IMR 0x08		/* Interrupt Mask Register, RW, 00h */
#define   BBA_IMR_FRAGIM	(1<<0) /* FRAGIM, Fragment Counter Int Mask */
#define   BBA_IMR_RIM		(1<<1) /* RIM, Receive Interrupt Mask */
#define   BBA_IMR_TIM		(1<<2) /* TIM, Transmit Interrupt Mask */
#define   BBA_IMR_REIM		(1<<3) /* REIM, Receive Error Interrupt Mask */
#define   BBA_IMR_TEIM		(1<<4) /* TEIM, Transmit Error Interrupt Mask */
#define   BBA_IMR_FIFOEIM	(1<<5) /* FIFOEIM, FIFO Error Interrupt Mask */
#define   BBA_IMR_BUSEIM	(1<<6) /* BUSEIM, BUS Error Interrupt Mask */
#define   BBA_IMR_RBFIM		(1<<7) /* RBFIM, RX Buf Full Interrupt Mask */

#define BBA_IR 0x09		/* Interrupt Register, RW, 00h */
#define   BBA_IR_FRAGI		(1<<0)	/* FRAGI, Fragment Counter Interrupt */
#define   BBA_IR_RI		(1<<1)	/* RI, Receive Interrupt */
#define   BBA_IR_TI		(1<<2)	/* TI, Transmit Interrupt */
#define   BBA_IR_REI		(1<<3)	/* REI, Receive Error Interrupt */
#define   BBA_IR_TEI		(1<<4)	/* TEI, Transmit Error Interrupt */
#define   BBA_IR_FIFOEI		(1<<5)	/* FIFOEI, FIFO Error Interrupt */
#define   BBA_IR_BUSEI		(1<<6)	/* BUSEI, BUS Error Interrupt */
#define   BBA_IR_RBFI		(1<<7)	/* RBFI, RX Buffer Full Interrupt */

#define BBA_BP   0x0a/*+0x0b*/	/* Boundary Page Pointer Register */
#define BBA_TLBP 0x0c/*+0x0d*/	/* TX Low Boundary Page Pointer Register */
#define BBA_TWP  0x0e/*+0x0f*/	/* Transmit Buf Write Page Pointer Register */
#define BBA_TRP  0x12/*+0x13*/	/* Transmit Buf Read Page Pointer Register */
#define BBA_RWP  0x16/*+0x17*/	/* Receive Buffer Write Page Pointer Register */
#define BBA_RRP  0x18/*+0x19*/	/* Receive Buffer Read Page Pointer Register */
#define BBA_RHBP 0x1a/*+0x1b*/	/* Receive High Boundary Page Ptr Register */

#define BBA_RXINTT    0x14/*+0x15*/	/* Receive Interrupt Timer Register */

#define BBA_NAFR_PAR0 0x20	/* Physical Address Register Byte 0 */
#define BBA_NAFR_PAR1 0x21	/* Physical Address Register Byte 1 */
#define BBA_NAFR_PAR2 0x22	/* Physical Address Register Byte 2 */
#define BBA_NAFR_PAR3 0x23	/* Physical Address Register Byte 3 */
#define BBA_NAFR_PAR4 0x24	/* Physical Address Register Byte 4 */
#define BBA_NAFR_PAR5 0x25	/* Physical Address Register Byte 5 */

#define BBA_NWAYC 0x30		/* NWAY Configuration Register, RW, 84h */
#define   BBA_NWAYC_FD		(1<<0)	/* FD, Full Duplex Mode */
#define   BBA_NWAYC_PS100	(1<<1)	/* PS100/10, Port Select 100/10 */
#define   BBA_NWAYC_ANE		(1<<2)	/* ANE, Autonegotiation Enable */
#define   BBA_NWAYC_ANS_RA	(0x01<<3) /* ANS, Restart Autonegotiation */
#define   BBA_NWAYC_LTE		(1<<7)	/* LTE, Link Test Enable */

#define BBA_GCA 0x32		/* GMAC Configuration A Register, RW, 00h */
#define   BBA_GCA_ARXERRB	(1<<3)	/* ARXERRB, Accept RX pkt with error */

#define BBA_MISC 0x3d		/* MISC Control Register 1, RW, 3ch */
#define   BBA_MISC_BURSTDMA	(1<<0)
#define   BBA_MISC_DISLDMA	(1<<1)

#define BBA_TXFIFOCNT 0x3e/*0x3f*/	/* Transmit FIFO Counter Register */
#define BBA_WRTXFIFOD 0x48/*-0x4b*/	/* Write TX FIFO Data Port Register */

#define BBA_MISC2 0x50		/* MISC Control Register 2, RW, 00h */
#define   BBA_MISC2_HBRLEN0	(1<<0)	/* HBRLEN, Host Burst Read Length */
#define   BBA_MISC2_HBRLEN1	(1<<1)	/*  " */
#define   BBA_MISC2_AUTORCVR	(1<<7)	/* Auto RX Full Recovery */

#define BBA_RX_STATUS_BF	(1<<0)
#define BBA_RX_STATUS_CRC	(1<<1)
#define BBA_RX_STATUS_FAE	(1<<2)
#define BBA_RX_STATUS_FO	(1<<3)
#define BBA_RX_STATUS_RW	(1<<4)
#define BBA_RX_STATUS_MF	(1<<5)
#define BBA_RX_STATUS_RF	(1<<6)
#define BBA_RX_STATUS_RERR	(1<<7)

#define BBA_TX_STATUS_CC0	(1<<0)
#define BBA_TX_STATUS_CC1	(1<<1)
#define BBA_TX_STATUS_CC2	(1<<2)
#define BBA_TX_STATUS_CC3	(1<<3)
#define  BBA_TX_STATUS_CCMASK	(0x0f)
#define BBA_TX_STATUS_CRSLOST	(1<<4)
#define BBA_TX_STATUS_UF	(1<<5)
#define BBA_TX_STATUS_OWC	(1<<6)
#define BBA_TX_STATUS_OWN	(1<<7)
#define BBA_TX_STATUS_TERR	(1<<7)

#define BBA_TX_MAX_PACKET_SIZE	1518	/* 14+1500+4 */
#define BBA_RX_MAX_PACKET_SIZE	1536	/* 6 pages * 256 bytes */


/**
 *
 * DRIVER NOTES
 *
 * 1. Packet Memory organization
 *
 * rx: 15 pages of 256 bytes, 2 full sized packets only (6 pages each)
 * tx: through FIFO, not using packet memory
 *
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |1|2|3|4|5|6|7|8|9|A|B|C|D|E|F|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ^                           ^
 * |                           |
 * TLBP                        RHBP
 * BP
 *
 */

#define BBA_INIT_TLBP	0x00
#define BBA_INIT_BP	0x01
#define BBA_INIT_RHBP	0x0f
#define BBA_INIT_RWP	BBA_INIT_BP
#define BBA_INIT_RRP	BBA_INIT_BP

enum {
	__BBA_RBFIM_OFF = 0,
};

struct bba_descr {
#if defined(__BIG_ENDIAN_BITFIELD)
	__u32	status:8,
		packet_len:12,
		next_packet_ptr:12;
#elif defined(__LITTLE_ENDIAN_BITFIELD)
	__u32	next_packet_ptr:12,
		packet_len:12,
		status:8;
#else
#error "Unsupported byte order."
#endif
} __attribute((packed));


struct bba_private {
	spinlock_t		lock;
	unsigned long		flags;
#define BBA_RBFIM_OFF		(1<<__BBA_RBFIM_OFF)

	u32			msg_enable;
	u8			revid;
	u8			__0x04_init[2];
	u8			__0x05_init;

	struct sk_buff		*tx_skb;
	int			rx_work;

	struct task_struct	*io_thread;
	wait_queue_head_t	io_waitq;

	struct net_device	*dev;
	struct net_device_stats	stats;

	struct exi_device	*exi_device;
};

static int bba_event_handler(struct exi_channel *exi_channel,
			     unsigned int event, void *dev0);
static int bba_setup_hardware(struct net_device *dev);

/*
 * Opens the network device.
 */
static int bba_open(struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);
	int retval;

	/* INTs are triggered on EXI channel 2 */
	retval = exi_event_register(to_exi_channel(BBA_EXI_IRQ_CHANNEL),
				    EXI_EVENT_IRQ,
				    priv->exi_device,
				    bba_event_handler, dev,
				    (1 << BBA_EXI_CHANNEL));
	if (retval < 0) {
		bba_printk(KERN_ERR, "unable to register EXI event %d\n",
			   EXI_EVENT_IRQ);
		goto out;
	}

	/* reset the hardware to a known state */
	exi_dev_take(priv->exi_device);
	retval = bba_setup_hardware(dev);
	exi_dev_give(priv->exi_device);

	/* inform the network layer that we are ready */
	netif_start_queue(dev);
out:
	return retval;
}

/*
 * Closes the network device.
 */
static int bba_close(struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);

	/* do not allow more packets to be queued */
	netif_carrier_off(dev);
	netif_stop_queue(dev);

	exi_dev_take(priv->exi_device);

	/* stop receiver */
	bba_out8(BBA_NCRA, bba_in8(BBA_NCRA) & ~BBA_NCRA_SR);

	/* mask all interrupts */
	bba_out8(BBA_IMR, 0x00);

	exi_dev_give(priv->exi_device);

	/* unregister exi event */
	exi_event_unregister(to_exi_channel(BBA_EXI_IRQ_CHANNEL),
			     EXI_EVENT_IRQ);

	return 0;
}

/*
 * Returns the network device statistics.
 */
static struct net_device_stats *bba_get_stats(struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);

	return &priv->stats;
}

/*
 * Starts transmission for a packet.
 * We can't do real hardware i/o here.
 */
static int bba_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);
	unsigned long flags;
	int retval = NETDEV_TX_OK;

	/* we are not able to send packets greater than this */
	if (skb->len > BBA_TX_MAX_PACKET_SIZE) {
		dev_kfree_skb(skb);
		priv->stats.tx_dropped++;
		/* silently drop the package */
		goto out;
	}

	spin_lock_irqsave(&priv->lock, flags);

	/*
	 * If there's no packet pending, store the packet for transmission
	 * and wake up the io thread. Otherwise, we are busy.
	 */
	if (!priv->tx_skb) {
		priv->tx_skb = skb;
		dev->trans_start = jiffies;
		wake_up(&priv->io_waitq);
	} else {
		retval = NETDEV_TX_BUSY;
	}

	/* we can only send one packet at a time through the FIFO */
	netif_stop_queue(dev);

	spin_unlock_irqrestore(&priv->lock, flags);

out:
	return retval;
}

/*
 * Updates transmission error statistics.
 * Caller holds the device lock.
 */
static int bba_tx_err(u8 status, struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);
	int last_tx_errors = priv->stats.tx_errors;

	if (status & BBA_TX_STATUS_TERR) {
		if (status & BBA_TX_STATUS_CCMASK) {
			priv->stats.collisions +=
			    (status & BBA_TX_STATUS_CCMASK);
			priv->stats.tx_errors++;
		}
		if (status & BBA_TX_STATUS_CRSLOST) {
			priv->stats.tx_carrier_errors++;
			priv->stats.tx_errors++;
		}
		if (status & BBA_TX_STATUS_UF) {
			priv->stats.tx_fifo_errors++;
			priv->stats.tx_errors++;
		}
		if (status & BBA_TX_STATUS_OWC) {
			priv->stats.tx_window_errors++;
			priv->stats.tx_errors++;
		}
	}

	if (last_tx_errors != priv->stats.tx_errors) {
		if (netif_msg_tx_err(priv)) {
			bba_printk(KERN_DEBUG, "tx errors, status %8.8x.\n",
				   status);
		}
	}
	return priv->stats.tx_errors;
}

/*
 * Transmits a packet already stored in the driver's internal tx slot.
 */
static int bba_tx(struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);
	struct sk_buff *skb;
	unsigned long flags;
	int retval = NETDEV_TX_OK;

	static u8 pad[ETH_ZLEN] __attribute__ ((aligned(EXI_DMA_ALIGN+1)));
	int pad_len;

	exi_dev_take(priv->exi_device);

	/* if the TXFIFO is in use, we'll try it later when free */
	if (bba_in8(BBA_NCRA) & (BBA_NCRA_ST0 | BBA_NCRA_ST1)) {
		retval = NETDEV_TX_BUSY;
		goto out;
	}

	spin_lock_irqsave(&priv->lock, flags);
	skb = priv->tx_skb;
	priv->tx_skb = NULL;
	spin_unlock_irqrestore(&priv->lock, flags);

	/* tell the card about the length of this packet */
	bba_out12(BBA_TXFIFOCNT, skb->len);

	/*
	 * Store the packet in the TXFIFO, including padding if needed.
	 * Packet transmission tries to make use of DMA transfers.
	 */

	bba_select();
	bba_outs_nosel(BBA_WRTXFIFOD, skb->data, skb->len);
	if (skb->len < ETH_ZLEN) {
		pad_len = ETH_ZLEN - skb->len;
		memset(pad, 0, pad_len);
		bba_outs_nosel_continued(pad, pad_len);
	}
	bba_deselect();

	/* tell the card to send the packet right now */
	bba_out8(BBA_NCRA, (bba_in8(BBA_NCRA) | BBA_NCRA_ST1) & ~BBA_NCRA_ST0);

	/* update statistics */
	priv->stats.tx_bytes += skb->len;
	priv->stats.tx_packets++;

	/* free this packet and remove it from our transmission "queue" */
	dev_kfree_skb(skb);

out:
	exi_dev_give(priv->exi_device);

	return retval;
}

/*
 * Updates reception error statistics.
 * Caller has already taken the exi channel.
 */
static int bba_rx_err(u8 status, struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);
	int last_rx_errors = priv->stats.rx_errors;

	if (status == 0xff) {
		priv->stats.rx_over_errors++;
		priv->stats.rx_errors++;
	} else {
		if (status & BBA_RX_STATUS_RERR) {
			if (status & BBA_RX_STATUS_CRC) {
				priv->stats.rx_crc_errors++;
				priv->stats.rx_errors++;
			}
			if (status & BBA_RX_STATUS_FO) {
				priv->stats.rx_fifo_errors++;
				priv->stats.rx_errors++;
			}
			if (status & BBA_RX_STATUS_RW) {
				priv->stats.rx_length_errors++;
				priv->stats.rx_errors++;
			}
			if (status & BBA_RX_STATUS_BF) {
				priv->stats.rx_over_errors++;
				priv->stats.rx_errors++;
			}
			if (status & BBA_RX_STATUS_RF) {
				priv->stats.rx_length_errors++;
				priv->stats.rx_errors++;
			}
		}
		if (status & BBA_RX_STATUS_FAE) {
			priv->stats.rx_frame_errors++;
			priv->stats.rx_errors++;
		}
	}

	if (last_rx_errors != priv->stats.rx_errors) {
		if (netif_msg_rx_err(priv)) {
			bba_printk(KERN_DEBUG, "rx errors, status %8.8x.\n",
				   status);
		}
	}
	return priv->stats.rx_errors;
}

/*
 * Reception function. Receives up to @budget packets.
 */
static int bba_rx(struct net_device *dev, int budget)
{
	struct bba_private *priv = netdev_priv(dev);
	struct sk_buff *skb;
	struct bba_descr descr;
	int lrps, size;
	unsigned long pos, top;
	unsigned short rrp, rwp;
	int received = 0;

	exi_dev_take(priv->exi_device);

	/* get current receiver pointers */
	rwp = bba_in12(BBA_RWP);
	rrp = bba_in12(BBA_RRP);

	while (netif_running(dev) && received < budget && rrp != rwp) {
		bba_ins(rrp << 8, &descr, sizeof(descr));
		le32_to_cpus((u32 *) &descr);

		size = descr.packet_len - 4;	/* ignore CRC */
		lrps = descr.status;

		/* abort processing in case of errors */
		if (size > BBA_RX_MAX_PACKET_SIZE + 4) {
			DBG("packet too big %d", size);
			continue;
		}

		if ((lrps & (BBA_RX_STATUS_RERR | BBA_RX_STATUS_FAE))) {
			DBG("error %x on received packet\n", lrps);
			bba_rx_err(lrps, dev);
			rwp = bba_in12(BBA_RWP);
			rrp = bba_in12(BBA_RRP);
			continue;
		}

		/* allocate a buffer, omitting the CRC (4 bytes) */
		skb = dev_alloc_skb(size + NET_IP_ALIGN);
		if (!skb) {
			priv->stats.rx_dropped++;
			continue;
		}
		skb->dev = dev;
		skb_reserve(skb, NET_IP_ALIGN);	/* align */
		skb_put(skb, size);

		pos = (rrp << 8) + 4;	/* skip descriptor */
		top = (BBA_INIT_RHBP + 1) << 8;

		if ((pos + size) < top) {
			/* full packet in one chunk */
			bba_ins(pos, skb->data, size);
		} else {
			/* packet wrapped */
			int chunk_size = top - pos;

			bba_ins(pos, skb->data, chunk_size);
			rrp = BBA_INIT_RRP;
			bba_ins(rrp << 8, skb->data + chunk_size,
				size - chunk_size);
		}

		skb->protocol = eth_type_trans(skb, dev);

		dev->last_rx = jiffies;
		priv->stats.rx_bytes += size;
		priv->stats.rx_packets++;

		netif_rx(skb);
		received++;

		/* move read pointer to next packet */
		rrp = descr.next_packet_ptr;
		bba_out12(BBA_RRP, rrp);

		/* get write pointer and continue */
		rwp = bba_in12(BBA_RWP);
	}

	/* there are no more packets pending if we didn't exhaust our budget */
	if (received < budget)
		priv->rx_work = 0;

	/* re-enable RBFI if it was disabled before */
	if (test_and_clear_bit(__BBA_RBFIM_OFF, &priv->flags))
		bba_out8(BBA_IMR, bba_in8(BBA_IMR) | BBA_IMR_RBFIM);

	exi_dev_give(priv->exi_device);

	return received;
}

/*
 * Input/Output thread. Sends and receives packets.
 */
static int bba_io_thread(void *bba_priv)
{
	struct bba_private *priv = bba_priv;
/*	struct task_struct *me = current; */
/*	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 }; */

/*	sched_setscheduler(me, SCHED_FIFO, &param); */

	set_user_nice(current, -20);
	current->flags |= PF_NOFREEZE;
	set_current_state(TASK_RUNNING);

	/*
	 * XXX We currently do not freeze this thread.
	 * The bba is often used to access the root filesystem.
	 */

	while (!kthread_should_stop()) {
		/*
		 * We want to get scheduled at least once every 2 minutes
		 * to avoid a softlockup spurious message...
		 * "INFO: task kbbaiod blocked for more than 120 seconds."
		 */
		wait_event_timeout(priv->io_waitq,
				   priv->rx_work || priv->tx_skb, 90*HZ);
		while (priv->rx_work || priv->tx_skb) {
			if (priv->rx_work)
				bba_rx(priv->dev, 0x0f);
			if (priv->tx_skb)
				bba_tx(priv->dev);
		}
	}
	return 0;
}

/*
 * Handles interrupt work from the network device.
 * Caller has already taken the exi channel.
 */
static void bba_interrupt(struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);
	u8 ir, imr, status, lrps, ltps;
	int loops = 0;

	ir = bba_in8(BBA_IR);
	imr = bba_in8(BBA_IMR);
	status = ir & imr;

	/* close possible races with dev_close */
	if (unlikely(!netif_running(dev))) {
		bba_out8(BBA_IR, status);
		bba_out8(BBA_IMR, 0x00);
		goto out;
	}

	while (status) {
		bba_out8(BBA_IR, status);

		/* avoid multiple receive buffer full interrupts */
		if (status & BBA_IR_RBFI) {
			bba_out8(BBA_IMR, bba_in8(BBA_IMR) & ~BBA_IMR_RBFIM);
			set_bit(__BBA_RBFIM_OFF, &priv->flags);
		}

		if ((status & (BBA_IR_RI | BBA_IR_RBFI))) {
			priv->rx_work = 1;
			wake_up(&priv->io_waitq);
		}
		if ((status & (BBA_IR_TI|BBA_IR_FIFOEI))) {
			/* allow more packets to be sent */
			netif_wake_queue(dev);
		}

		if ((status & (BBA_IR_RBFI|BBA_IR_REI))) {
			lrps = bba_in8(BBA_LRPS);
			bba_rx_err(lrps, dev);
		}
		if (status & BBA_IR_TEI) {
			ltps = bba_in8(BBA_LTPS);
			bba_tx_err(ltps, dev);
		}

		if (status & BBA_IR_FIFOEI)
			DBG("FIFOEI\n");
		if (status & BBA_IR_BUSEI)
			DBG("BUSEI\n");
		if (status & BBA_IR_FRAGI)
			DBG("FRAGI\n");

		ir = bba_in8(BBA_IR);
		imr = bba_in8(BBA_IMR);
		status = ir & imr;

		loops++;
	}

	if (loops > 3)
		DBG("a lot of interrupt work (%d loops)\n", loops);

	/* wake up xmit queue in case transmitter is idle */
	if ((bba_in8(BBA_NCRA) & (BBA_NCRA_ST0 | BBA_NCRA_ST1)) == 0)
		netif_wake_queue(dev);

out:
	return;
}

/*
 * Retrieves the MAC address of the adapter.
 * Caller has already taken the exi channel.
 */
static void bba_retrieve_ether_addr(struct net_device *dev)
{
	bba_ins(BBA_NAFR_PAR0, dev->dev_addr, ETH_ALEN);
	if (!is_valid_ether_addr(dev->dev_addr))
		random_ether_addr(dev->dev_addr);
}

/*
 * Resets the hardware to a known state.
 * Caller has already taken the exi channel.
 */
static void bba_reset_hardware(struct net_device *dev)
{
	struct bba_private *priv = netdev_priv(dev);

	/* unknown, mx register 0x60 */
	bba_out8(0x60, 0);
	udelay(1000);

	/* unknown, command register 0x0f */
	bba_cmd_in8_slow(0x0f);
	udelay(1000);

	/* software reset (write 1 then write 0) */
	bba_out8(BBA_NCRA, BBA_NCRA_RESET);
	udelay(100);
	bba_out8(BBA_NCRA, 0);

	/* unknown, command register 0x01 */
	/* XXX obtain bits needed for challenge/response calculation later */
	priv->revid = bba_cmd_in8(0x01);

	/* unknown, command registers 0x04, 0x05 */
	bba_cmd_outs(0x04, priv->__0x04_init, 2);
	bba_cmd_out8(0x05, priv->__0x05_init);

	/*
	 * These initializations seem to limit the final port speed to 10Mbps
	 * half duplex. Bypassing them, allows one to set other port speeds.
	 * But, remember that the bba spi-like bus clock operates at 32MHz.
	 * ---Albert Herranz
	 */

	/* unknown, mx registers 0x5b, 0x5c, 0x5e */
	bba_out8(0x5b, bba_in8(0x5b) & ~(1 << 7));
	bba_out8(0x5e, 1); /* without this the BBA goes at half the speed */
	bba_out8(0x5c, bba_in8(0x5c) | 4);
	udelay(1000);

	/* accept broadcast, assert int for every packet received */
	bba_out8(BBA_NCRB, BBA_NCRB_AB | BBA_NCRB_1_PACKET_PER_INT);

	/* setup receive interrupt time out, in 40ns units */
	bba_out8(BBA_RXINTT, 0x00);
	bba_out8(BBA_RXINTT+1, 0x06); /* 0x0600 = 61us */

	/* auto RX full recovery */
	bba_out8(BBA_MISC2, BBA_MISC2_AUTORCVR);

	/* initialize packet memory layout */
	bba_out12(BBA_TLBP, BBA_INIT_TLBP);
	bba_out12(BBA_BP, BBA_INIT_BP);
	bba_out12(BBA_RHBP, BBA_INIT_RHBP);

	/* set receive page pointers */
	bba_out12(BBA_RWP, BBA_INIT_RWP);
	bba_out12(BBA_RRP, BBA_INIT_RRP);

	/* packet memory won't contain packets with RW, FO, CRC errors */
	bba_out8(BBA_GCA, BBA_GCA_ARXERRB);
}

/*
 * Prepares the hardware for operation.
 * Caller has already taken the exi channel.
 */
static int bba_setup_hardware(struct net_device *dev)
{
	/* reset hardware to a sane state */
	bba_reset_hardware(dev);

	/* start receiver */
	bba_out8(BBA_NCRA, BBA_NCRA_SR);

	/* clear all interrupts */
	bba_out8(BBA_IR, 0xFF);

	/* enable all interrupts */
	bba_out8(BBA_IMR, 0xFF & ~(BBA_IMR_FIFOEIM /*| BBA_IMR_REIM*/));

	/* unknown, short command registers 0x02 */
	/* XXX enable interrupts on the EXI glue logic */
	bba_cmd_out8(0x02, BBA_CMD_IR_MASKNONE);

	/* DO NOT clear interrupts on the EXI glue logic !!! */
	/* we need that initial interrupts for the challenge/response */

	return 0;		/* OK */
}

/*
 * Calculates a response for a given challenge.
 */
static unsigned long bba_calc_response(unsigned long val,
				       struct bba_private *priv)
{
	u8 revid_0, revid_eth_0, revid_eth_1;
	u8 i0, i1, i2, i3;
	u8 c0, c1, c2, c3;

	revid_0 = priv->revid;
	revid_eth_0 = priv->__0x04_init[0];
	revid_eth_1 = priv->__0x04_init[1];

	i0 = val >> 24;
	i1 = val >> 16;
	i2 = val >> 8;
	i3 = val;

	c0 = (i0 + i1 * 0xc1 + 0x18 + revid_0) ^ (i3 * i2 + 0x90);
	c1 = (i1 + i2 + 0x90) ^ (c0 + i0 - 0xc1);
	c2 = (i2 + 0xc8) ^ (c0 + ((revid_eth_0 + revid_0 * 0x23) ^ 0x19));
	c3 = (i0 + 0xc1) ^ (i3 + ((revid_eth_1 + 0xc8) ^ 0x90));

	return (c0 << 24) | (c1 << 16) | (c2 << 8) | c3;
}

/*
 * Handles IRQ events from the exi layer.
 *
 * We are called from softirq context, and with the exi channel kindly taken
 * for us. We can also safely do exi transfers of less than 32 bytes, which
 * are guaranteed to not sleep by the exi layer.
 */
static int bba_event_handler(struct exi_channel *exi_channel,
			     unsigned int event, void *dev0)
{
	struct net_device *dev = (struct net_device *)dev0;
	struct bba_private *priv = netdev_priv(dev);
	register u8 status, mask;

	/* XXX mask all EXI glue interrupts */
	bba_cmd_out8(0x02, BBA_CMD_IR_MASKALL);

	/* get interrupt status from EXI glue */
	status = bba_cmd_in8(0x03);

	/* start with the usual case */
	mask = (1<<7);

	/* normal interrupt from the macronix chip */
	if (status & mask) {
		/* call our interrupt handler */
		bba_interrupt(dev);
		goto out;
	}

	/* "killing" interrupt, try to not get one of these! */
	mask >>= 1;
	if (status & mask) {
		DBG("bba: killing interrupt!\n");
		/* reset the adapter so that we can continue working */
		bba_setup_hardware(dev);
		goto out;
	}

	/* command error interrupt, haven't seen one yet */
	mask >>= 1;
	if (status & mask)
		goto out;

	/* challenge/response interrupt */
	mask >>= 1;
	if (status & mask) {
		unsigned long response;
		unsigned long challenge;

		/* kids, don't do it without an adult present */
		bba_cmd_out8(0x05, priv->__0x05_init);
		bba_cmd_ins(0x08, &challenge, sizeof(challenge));
		response = bba_calc_response(challenge, priv);
		bba_cmd_outs(0x09, &response, sizeof(response));

		goto out;
	}

	/* challenge/response status interrupt */
	mask >>= 1;
	if (status & mask) {
		/* better get a "1" here ... */
		u8 result = bba_cmd_in8(0x0b);
		if (result != 1) {
			bba_printk(KERN_DEBUG,
				   "challenge failed! (result=%d)\n", result);
		}
		goto out;
	}

	/* should not happen, treat as normal interrupt in any case */
	DBG("bba: unknown interrupt type = %d\n", status);

out:
	/* assert interrupt */
	bba_cmd_out8(0x03, mask);

	/* enable interrupts again */
	bba_cmd_out8(0x02, BBA_CMD_IR_MASKNONE);

	return 1;
}

static struct net_device *bba_dev;

static inline void bba_select(void)
{
	struct bba_private *priv = netdev_priv(bba_dev);
	exi_dev_select(priv->exi_device);

}

static inline void bba_deselect(void)
{
	struct bba_private *priv = netdev_priv(bba_dev);
	exi_dev_deselect(priv->exi_device);
}

static inline void bba_read(void *data, size_t len)
{
	struct bba_private *priv = netdev_priv(bba_dev);
	return exi_dev_read(priv->exi_device, data, len);
}

static inline void bba_write(void *data, size_t len)
{
	struct bba_private *priv = netdev_priv(bba_dev);
	return exi_dev_write(priv->exi_device, data, len);
}

static const struct net_device_ops bba_netdev_ops = {
	.ndo_open		= bba_open,
	.ndo_stop		= bba_close,
	.ndo_start_xmit		= bba_start_xmit,
	.ndo_get_stats		= bba_get_stats,
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

/*
 * Initializes a BroadBand Adapter device.
 */
static int __devinit bba_init_device(struct exi_device *exi_device)
{
	struct net_device *dev;
	struct bba_private *priv;
	int err;

	/* allocate a network device */
	dev = alloc_etherdev(sizeof(*priv));
	if (!dev) {
		bba_printk(KERN_ERR, "unable to allocate net device\n");
		err = -ENOMEM;
		goto err_out;
	}
	SET_NETDEV_DEV(dev, &exi_device->dev);

	/* we use the event system from the EXI driver, so no irq here */
	dev->irq = 0;

	/* network device hooks */
	dev->netdev_ops = &bba_netdev_ops;

	priv = netdev_priv(dev);
	priv->dev = dev;
	priv->exi_device = exi_device;

	spin_lock_init(&priv->lock);

	/* initialization values */
	priv->revid = 0xf0;
	priv->__0x04_init[0] = 0xd1;
	priv->__0x04_init[1] = 0x07;
	priv->__0x05_init = 0x4e;

	/* i/o artifacts */
	priv->tx_skb = NULL;
	priv->rx_work = 0;
	init_waitqueue_head(&priv->io_waitq);
	priv->io_thread = kthread_run(bba_io_thread, priv, "kbbaiod");

	/* the hardware can't do multicast */
	dev->flags &= ~IFF_MULTICAST;

	exi_set_drvdata(exi_device, dev);
	if (bba_dev)
		free_netdev(bba_dev);
	bba_dev = dev;

	/* we need to retrieve the MAC address before registration */
	exi_dev_take(priv->exi_device);
	bba_reset_hardware(dev);
	bba_retrieve_ether_addr(dev);
	exi_dev_give(priv->exi_device);

	/* this makes our device available to the kernel */
	err = register_netdev(dev);
	if (err) {
		bba_printk(KERN_ERR, "cannot register net device, aborting.\n");
		goto err_out_free_dev;
	}

	return 0;

err_out_free_dev:
	exi_set_drvdata(exi_device, NULL);
	free_netdev(dev);
	bba_dev = NULL;

err_out:
	return err;
}

/*
 * Removes a BroadBand Adapter device from the system.
 */
static void __devexit bba_remove(struct exi_device *exi_device)
{
	struct net_device *dev = (struct net_device *)
				 exi_get_drvdata(exi_device);
	struct bba_private *priv;

	if (dev) {
		priv = netdev_priv(dev);

		kthread_stop(priv->io_thread);

		unregister_netdev(dev);
		free_netdev(dev);
		exi_set_drvdata(exi_device, NULL);
		bba_dev = NULL;
	}
	exi_device_put(exi_device);
}

/*
 * Probes for a BroadBand Adapter device.
 * Actually, the exi layer has already probed for us.
 */
static int __devinit bba_probe(struct exi_device *exi_device)
{
	int ret = -ENODEV;

	if (exi_device_get(exi_device))
		ret = bba_init_device(exi_device);

	return ret;
}


static struct exi_device_id bba_eid_table[] = {
	[0] = {
		.channel = BBA_EXI_CHANNEL,
		.device  = BBA_EXI_DEVICE,
		.id      = BBA_EXI_ID
	},
	{ .id = 0 }
};

static struct exi_driver bba_driver = {
	.name = "bba",
	.eid_table = bba_eid_table,
	.frequency = BBA_EXI_FREQ,
	.probe = bba_probe,
	.remove = bba_remove,
};

/**
 *	bba_init_module -  driver initialization routine
 *
 *	Initializes the BroadBand Adapter driver module.
 *
 */
static int __init bba_init_module(void)
{
	bba_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   bba_driver_version);

	return exi_driver_register(&bba_driver);
}

/**
 *	bba_exit_module -  driver exit routine
 *
 *	Removes the BroadBand Adapter driver module.
 *
 */
static void __exit bba_exit_module(void)
{
	exi_driver_unregister(&bba_driver);
}

module_init(bba_init_module);
module_exit(bba_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

