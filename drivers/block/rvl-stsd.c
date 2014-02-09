/*
 * drivers/block/rvl-stsd.c
 *
 * Block driver for the Nintendo Wii SD front slot.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * Based on drivers/block/gcn-sd.c
 *
 * Copyright (C) 2004-2008 The GameCube Linux Team
 * Copyright (C) 2004,2005 Rob Reylink
 * Copyright (C) 2005 Todd Jeffreys
 * Copyright (C) 2005,2006,2007,2008 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define DEBUG

/*#define DBG(fmt, arg...)	pr_debug(fmt, ##arg)*/
#define DBG(fmt, arg...)	drv_printk(KERN_ERR, fmt, ##arg)

#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <asm/starlet.h>
#include <asm/starlet-ios.h>

/*
 * We are not a native MMC driver...
 * But anyway, we try to recycle here some of the available code.
 */
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include "../mmc/host/sdhci.h"

#define DRV_MODULE_NAME "rvl-stsd"
#define DRV_DESCRIPTION "Block driver for the Nintendo Wii SD front slot"
#define DRV_AUTHOR      "Albert Herranz"

static char stsd_driver_version[] = "0.4i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

/*
 * Driver settings.
 */
#define MMC_SHIFT		3	/* 8 partitions */

#define STSD_MAJOR		62
#define STSD_NAME		"rvlsd"

#define KERNEL_SECTOR_SHIFT     9
#define KERNEL_SECTOR_SIZE      (1 << KERNEL_SECTOR_SHIFT)	/*512 */

#define STSD_MAX_SECTORS	16


/*
 * IOS-related constants.
 */

/* ioctls */
#define STSD_IOCTL_SETHSR	1
#define STSD_IOCTL_GETHSR	2
#define STSD_IOCTL_RESET	4
#define STSD_IOCTL_SETCLOCK	6
#define STSD_IOCTL_SENDCMD	7
#define STSD_IOCTL_GETSTATUS	11
#define STSD_IOCTL_GETOCR	12

#define STSD_IOCTLV_SENDCMD	7

/* SD command types */
#define STSD_CMDTYPE_BC		1
#define STSD_CMDTYPE_BCR	2
#define STSD_CMDTYPE_AC		3
#define STSD_CMDTYPE_ADTC	4

/* SD response types */
#define STSD_RSPTYPE_NONE	0
#define STSD_RSPTYPE_R1		1
#define STSD_RSPTYPE_R1B	2
#define STSD_RSPTYPE_R2		3
#define STSD_RSPTYPE_R3		4
#define STSD_RSPTYPE_R4		5
#define STSD_RSPTYPE_R5		6
#define STSD_RSPTYPE_R6		7
#define STSD_RSPTYPE_R7		8

/* card status bits */
#define STSD_STATUS_CARD_INSERTED	(1<<0)
#define STSD_STATUS_CARD_INITIALIZED	(1<<16)

/* IOS errors */
#define STSD_ERR_INVALID_CARD		0xc1000020

/*
 * Hardware registers.
 */

/*
 * Simplified SD Host Controller Specification
 * Version 2.00
 * February 8, 2007
 */

/*
 * SD Host Standard Registers
 *
 */

/* we are recycling the stuff already in "../mmc/host/sdhci.h" */

/* TMCLK*2^a a=[13..27] */
#define STSD_TIMEOUT_CONTROL_DIV(a)	(((a)-13)&0xf)

static char stsd_dev_sdio_slot0[] = "/dev/sdio/slot0";

/*
 * Used to get/set the host controller hardware register values through IOS.
 */
struct stsd_reg_query {
	u32	addr;
	u32	_unk1;
	u32	_unk2;
	u32	size;
	u32	data;
	u32	_unk3;
};

/*
 * Used to send commands to an SD card through IOS.
 */
struct stsd_command {
	u32 opcode;
	u32 cmdtype;
	u32 rsptype;
	u32 arg;
	u32 blk_count;
	u32 blk_size;
	dma_addr_t dma_addr;
	u32 is_dma;
	u32 _unk2;
};

struct stsd_xfer {
	size_t size;
	enum dma_data_direction direction;

	struct starlet_ioh_sg in[2], io[1];
	struct stsd_command *cmd;

	/* one-time initialized members */
	void *reply;
	size_t reply_len;
	dma_addr_t dma_addr;
	void *bounce_buf;
	size_t bounce_buf_size;
	size_t blk_size;
};

enum {
	__STSD_MEDIA_CHANGED = 0,
	__STSD_BAD_CARD,
	__STSD_MANUAL_SETUP,
	__STSD_SDHC,
};

struct stsd_host {
	spinlock_t	lock;
	unsigned long	flags;
#define STSD_MEDIA_CHANGED	(1<<__STSD_MEDIA_CHANGED)
#define STSD_BAD_CARD		(1<<__STSD_BAD_CARD)
#define STSD_MANUAL_SETUP	(1<<__STSD_MANUAL_SETUP)
#define STSD_SDHC		(1<<__STSD_SDHC)

	/* u32		ocr; */
	unsigned int	f_max;
	unsigned int	clock;
	u32		bus_width;

	u16		status;

	/* card related info */
	struct mmc_card	card;

	int refcnt;

	spinlock_t 		queue_lock;
	struct request_queue	*queue;
	struct gendisk 		*disk;
	unsigned int		max_phys_segments;

	struct stsd_xfer	*xfer;

	struct task_struct	*io_thread;
	struct mutex		io_mutex;

	int fd;
	struct device		*dev;
};


static const unsigned int tran_exp[] = {
	10000,		100000,		1000000,	10000000,
	0,		0,		0,		0
};

static const unsigned char tran_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

static const unsigned int tacc_exp[] = {
	1,	10,	100,	1000,	10000,	100000,	1000000, 10000000,
};

static const unsigned int tacc_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};


/*
 * debug section
 *
 */

#if defined(DEBUG) && 0

#define __case_string(_s)	\
case _s:			\
	str = #_s;		\
	break;

static char *stsd_opcode_string(u32 opcode)
{
	char *str = "unknown";

	switch (opcode) {
__case_string(MMC_GO_IDLE_STATE)
__case_string(MMC_SEND_OP_COND)
__case_string(MMC_ALL_SEND_CID)
__case_string(MMC_SET_RELATIVE_ADDR)
__case_string(MMC_SET_DSR)
__case_string(MMC_SWITCH)
__case_string(MMC_SELECT_CARD)
__case_string(MMC_SEND_EXT_CSD)
__case_string(MMC_SEND_CSD)
__case_string(MMC_SEND_CID)
__case_string(MMC_READ_DAT_UNTIL_STOP)
__case_string(MMC_STOP_TRANSMISSION)
__case_string(MMC_SEND_STATUS)
__case_string(MMC_GO_INACTIVE_STATE)
__case_string(MMC_SPI_READ_OCR)
__case_string(MMC_SPI_CRC_ON_OFF)
__case_string(MMC_SET_BLOCKLEN)
__case_string(MMC_READ_SINGLE_BLOCK)
__case_string(MMC_READ_MULTIPLE_BLOCK)
__case_string(MMC_WRITE_DAT_UNTIL_STOP)
__case_string(MMC_SET_BLOCK_COUNT)
__case_string(MMC_WRITE_BLOCK)
__case_string(MMC_WRITE_MULTIPLE_BLOCK)
__case_string(MMC_PROGRAM_CID)
__case_string(MMC_PROGRAM_CSD)
__case_string(MMC_SET_WRITE_PROT)
__case_string(MMC_CLR_WRITE_PROT)
__case_string(MMC_SEND_WRITE_PROT)
__case_string(MMC_ERASE_GROUP_START)
__case_string(MMC_ERASE_GROUP_END)
__case_string(MMC_ERASE)
__case_string(MMC_FAST_IO)
__case_string(MMC_GO_IRQ_STATE)
__case_string(MMC_LOCK_UNLOCK)
/*__case_string(SD_SEND_RELATIVE_ADDR)*/
/*__case_string(SD_SEND_IF_COND)*/
/*__case_string(SD_SWITCH)*/
__case_string(SD_IO_SEND_OP_COND)
__case_string(SD_IO_RW_DIRECT)
__case_string(SD_IO_RW_EXTENDED)
	}

	return str;
}

static char *stsd_rsptype_string(u32 rsptype)
{
	char *str = "unknown";

	switch (rsptype) {
__case_string(STSD_RSPTYPE_NONE)
__case_string(STSD_RSPTYPE_R1)
__case_string(STSD_RSPTYPE_R1B)
__case_string(STSD_RSPTYPE_R2)
__case_string(STSD_RSPTYPE_R3)
__case_string(STSD_RSPTYPE_R4)
__case_string(STSD_RSPTYPE_R5)
__case_string(STSD_RSPTYPE_R6)
__case_string(STSD_RSPTYPE_R7)
	}

	return str;
}

static char *stsd_cmdtype_string(u32 cmdtype)
{
	char *str = "unknown";

	switch (cmdtype) {
__case_string(STSD_CMDTYPE_BC)
__case_string(STSD_CMDTYPE_BCR)
__case_string(STSD_CMDTYPE_AC)
__case_string(STSD_CMDTYPE_ADTC)
	}

	return str;
}

static char *stsd_statusbit_string(u32 statusbit)
{
	char *str = "unknown";

	switch (statusbit) {
__case_string(R1_OUT_OF_RANGE)
__case_string(R1_ADDRESS_ERROR)
__case_string(R1_BLOCK_LEN_ERROR)
__case_string(R1_ERASE_SEQ_ERROR)
__case_string(R1_ERASE_PARAM)
__case_string(R1_WP_VIOLATION)
__case_string(R1_CARD_IS_LOCKED)
__case_string(R1_LOCK_UNLOCK_FAILED)
__case_string(R1_COM_CRC_ERROR)
__case_string(R1_ILLEGAL_COMMAND)
__case_string(R1_CARD_ECC_FAILED)
__case_string(R1_CC_ERROR)
__case_string(R1_ERROR)
__case_string(R1_UNDERRUN)
__case_string(R1_OVERRUN)
__case_string(R1_CID_CSD_OVERWRITE)
__case_string(R1_WP_ERASE_SKIP)
__case_string(R1_CARD_ECC_DISABLED)
__case_string(R1_ERASE_RESET)
__case_string(R1_READY_FOR_DATA)
__case_string(R1_APP_CMD)
	}

	return str;
}

static char *stsd_card_state_string(u32 status)
{
	char *str = "unknown";

	switch (R1_CURRENT_STATE(status)) {
	case 0:
		str = "IDLE";
		break;
	case 1:
		str = "READY";
		break;
	case 2:
		str = "IDENT";
		break;
	case 3:
		str = "STANDBY";
		break;
	case 4:
		str = "TRANSFER";
		break;
	case 5:
		str = "SEND";
		break;
	case 6:
		str = "RECEIVE";
		break;
	case 7:
		str = "PROGRAM";
		break;
	case 8:
		str = "DISCONNECT";
		break;
	}

	return str;
}
static void stsd_print_status(u32 status)
{
	u32 i, bit;

	drv_printk(KERN_INFO, "card state %s\n",
		   stsd_card_state_string(status));

	i = 13;
	for (i = 13; i <= 31; i++) {
		bit = 1 << i;
		if ((status & bit))
			drv_printk(KERN_INFO, "%02d %s\n", i,
				   stsd_statusbit_string(bit));
	}
	bit = 1 << 8;
	if ((status & bit))
		drv_printk(KERN_INFO, "%02d %s\n", 8,
			   stsd_statusbit_string(bit));
	bit = 1 << 5;
	if ((status & bit))
		drv_printk(KERN_INFO, "%02d %s\n", 5,
			   stsd_statusbit_string(bit));
}

static void stsd_print_cid(struct mmc_cid *cid)
{
	drv_printk(KERN_INFO,
		  "manfid = %d\n"
		  "oemid = %d\n"
		  "prod_name = %s\n"
		  "hwrev = %d\n"
		  "fwrev = %d\n"
		  "serial = %08x\n"
		  "year = %d\n"
		  "month = %d\n",
		  cid->manfid,
		  cid->oemid,
		  cid->prod_name,
		  cid->hwrev, cid->fwrev, cid->serial, cid->year, cid->month);
}

static void stsd_print_csd(struct mmc_csd *csd)
{
	drv_printk(KERN_INFO,
		  "mmca_vsn = %d\n"
		  "cmdclass = %d\n"
		  "tacc_clks = %d\n"
		  "tacc_ns = %d\n"
		  "r2w_factor = %d\n"
		  "max_dtr = %d\n"
		  "read_blkbits = %d\n"
		  "write_blkbits = %d\n"
		  "capacity = %d\n"
		  "read_partial = %d\n"
		  "read_misalign = %d\n"
		  "write_partial = %d\n"
		  "write_misalign = %d\n",
		  csd->mmca_vsn,
		  csd->cmdclass,
		  csd->tacc_clks,
		  csd->tacc_ns,
		  csd->r2w_factor,
		  csd->max_dtr,
		  csd->read_blkbits,
		  csd->write_blkbits,
		  csd->capacity,
		  csd->read_partial,
		  csd->read_misalign,
		  csd->write_partial,
		  csd->write_misalign);
}

static void stsd_dump_hs_regs(struct stsd_host *host)
{
	drv_printk(KERN_DEBUG, "============== REGISTER DUMP ==============\n");

	drv_printk(KERN_DEBUG, "Sys addr: 0x%08x | Version:  0x%08x\n",
		   stsd_hsr_in_u32(host, SDHCI_DMA_ADDRESS),
		   stsd_hsr_in_u16(host, SDHCI_HOST_VERSION));
	drv_printk(KERN_DEBUG, "Blk size: 0x%08x | Blk cnt:  0x%08x\n",
		   stsd_hsr_in_u16(host, SDHCI_BLOCK_SIZE),
		   stsd_hsr_in_u16(host, SDHCI_BLOCK_COUNT));
	drv_printk(KERN_DEBUG, "Argument: 0x%08x | Trn mode: 0x%08x\n",
		   stsd_hsr_in_u32(host, SDHCI_ARGUMENT),
		   stsd_hsr_in_u16(host, SDHCI_TRANSFER_MODE));
	drv_printk(KERN_DEBUG, "Present:  0x%08x | Host ctl: 0x%08x\n",
		   stsd_hsr_in_u32(host, SDHCI_PRESENT_STATE),
		   stsd_hsr_in_u8(host, SDHCI_HOST_CONTROL));
	drv_printk(KERN_DEBUG, "Power:    0x%08x | Blk gap:  0x%08x\n",
		   stsd_hsr_in_u8(host, SDHCI_POWER_CONTROL),
		   stsd_hsr_in_u8(host, SDHCI_BLOCK_GAP_CONTROL));
	drv_printk(KERN_DEBUG, "Wake-up:  0x%08x | Clock:    0x%08x\n",
		   stsd_hsr_in_u8(host, SDHCI_WAKE_UP_CONTROL),
		   stsd_hsr_in_u16(host, SDHCI_CLOCK_CONTROL));
	drv_printk(KERN_DEBUG, "Timeout:  0x%08x | Int stat: 0x%08x\n",
		   stsd_hsr_in_u8(host, SDHCI_TIMEOUT_CONTROL),
		   stsd_hsr_in_u32(host, SDHCI_INT_STATUS));
	drv_printk(KERN_DEBUG, "Int enab: 0x%08x | Sig enab: 0x%08x\n",
		   stsd_hsr_in_u32(host, SDHCI_INT_ENABLE),
		   stsd_hsr_in_u32(host, SDHCI_SIGNAL_ENABLE));
	drv_printk(KERN_DEBUG, "AC12 err: 0x%08x | Slot int: 0x%08x\n",
		   stsd_hsr_in_u16(host, SDHCI_ACMD12_ERR),
		   stsd_hsr_in_u16(host, SDHCI_SLOT_INT_STATUS));
	drv_printk(KERN_DEBUG, "Caps:     0x%08x | Max curr: 0x%08x\n",
		   stsd_hsr_in_u32(host, SDHCI_CAPABILITIES),
		   stsd_hsr_in_u32(host, SDHCI_MAX_CURRENT));

	drv_printk(KERN_DEBUG, "===========================================\n");
}

#endif /* DEBUG */

/*
 *
 * MMC/SD data structures manipulation.
 * Borrowed from MMC layer.
 */

#define UNSTUFF_BITS(resp, start, size)				\
	({								\
		const int __size = size;				\
		const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1;	\
		const int __off = 3 - ((start) / 32);			\
		const int __shft = (start) & 31;			\
		u32 __res;						\
									\
		__res = resp[__off] >> __shft;				\
		if (__size + __shft > 32)				\
			__res |= resp[__off-1] << ((32 - __shft) % 32);	\
		__res & __mask;						\
	})

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
static void mmc_decode_cid(struct mmc_card *card)
{
	u32 *resp = card->raw_cid;

	memset(&card->cid, 0, sizeof(struct mmc_cid));

	/*
	 * SD doesn't currently have a version field so we will
	 * have to assume we can parse this.
	 */
	card->cid.manfid		= UNSTUFF_BITS(resp, 120, 8);
	card->cid.oemid			= UNSTUFF_BITS(resp, 104, 16);
	card->cid.prod_name[0]		= UNSTUFF_BITS(resp, 96, 8);
	card->cid.prod_name[1]		= UNSTUFF_BITS(resp, 88, 8);
	card->cid.prod_name[2]		= UNSTUFF_BITS(resp, 80, 8);
	card->cid.prod_name[3]		= UNSTUFF_BITS(resp, 72, 8);
	card->cid.prod_name[4]		= UNSTUFF_BITS(resp, 64, 8);
	card->cid.hwrev			= UNSTUFF_BITS(resp, 60, 4);
	card->cid.fwrev			= UNSTUFF_BITS(resp, 56, 4);
	card->cid.serial		= UNSTUFF_BITS(resp, 24, 32);
	card->cid.year			= UNSTUFF_BITS(resp, 12, 8);
	card->cid.month			= UNSTUFF_BITS(resp, 8, 4);

	card->cid.year += 2000; /* SD cards year offset */
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static int mmc_decode_csd(struct mmc_card *card)
{
	struct mmc_csd *csd = &card->csd;
	unsigned int e, m, csd_struct;
	u32 *resp = card->raw_csd;

	csd_struct = UNSTUFF_BITS(resp, 126, 2);

	switch (csd_struct) {
	case 0:
		m = UNSTUFF_BITS(resp, 115, 4);
		e = UNSTUFF_BITS(resp, 112, 3);
		csd->tacc_ns	 = (tacc_exp[e] * tacc_mant[m] + 9) / 10;
		csd->tacc_clks	 = UNSTUFF_BITS(resp, 104, 8) * 100;

		m = UNSTUFF_BITS(resp, 99, 4);
		e = UNSTUFF_BITS(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

		e = UNSTUFF_BITS(resp, 47, 3);
		m = UNSTUFF_BITS(resp, 62, 12);
		csd->capacity	  = (1 + m) << (e + 2);

		csd->read_blkbits = UNSTUFF_BITS(resp, 80, 4);
		csd->read_partial = UNSTUFF_BITS(resp, 79, 1);
		csd->write_misalign = UNSTUFF_BITS(resp, 78, 1);
		csd->read_misalign = UNSTUFF_BITS(resp, 77, 1);
		csd->r2w_factor = UNSTUFF_BITS(resp, 26, 3);
		csd->write_blkbits = UNSTUFF_BITS(resp, 22, 4);
		csd->write_partial = UNSTUFF_BITS(resp, 21, 1);
		break;
	case 1:
		/*
		 * This is a block-addressed SDHC card. Most
		 * interesting fields are unused and have fixed
		 * values. To avoid getting tripped by buggy cards,
		 * we assume those fixed values ourselves.
		 */
		mmc_card_set_blockaddr(card);

		csd->tacc_ns	 = 0; /* Unused */
		csd->tacc_clks	 = 0; /* Unused */

		m = UNSTUFF_BITS(resp, 99, 4);
		e = UNSTUFF_BITS(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

		m = UNSTUFF_BITS(resp, 48, 22);
		csd->capacity     = (1 + m) << 10;

		csd->read_blkbits = 9;
		csd->read_partial = 0;
		csd->write_misalign = 0;
		csd->read_misalign = 0;
		csd->r2w_factor = 4; /* Unused */
		csd->write_blkbits = 9;
		csd->write_partial = 0;
		break;
	default:
		printk(KERN_ERR "unrecognised CSD structure version %d\n",
			csd_struct);
		return -EINVAL;
	}

	/*stsd_print_csd(csd);*/

	return 0;
}

/*
 * REVISIT maybe get rid of this and specify the rsptype directly
 */
static u32 stsd_opcode_to_rsptype(u32 opcode)
{
	u32 rsptype = STSD_RSPTYPE_R1;

	switch (opcode) {
	case MMC_GO_IDLE_STATE:
	case MMC_SET_DSR:
	case MMC_GO_INACTIVE_STATE:
		rsptype = STSD_RSPTYPE_NONE;
		break;
	case MMC_SWITCH:
	case MMC_STOP_TRANSMISSION:
	case MMC_SET_WRITE_PROT:
	case MMC_CLR_WRITE_PROT:
	case MMC_ERASE:
	case MMC_LOCK_UNLOCK:
		rsptype = STSD_RSPTYPE_R1B;
		break;
	case MMC_ALL_SEND_CID:
	case MMC_SEND_CSD:
	case MMC_SEND_CID:
		rsptype = STSD_RSPTYPE_R2;
		break;
	case MMC_SEND_OP_COND:
	case SD_APP_OP_COND:
		rsptype = STSD_RSPTYPE_R3;
		break;
	case MMC_FAST_IO:
	case SD_IO_SEND_OP_COND:
		rsptype = STSD_RSPTYPE_R4;
		break;
	case MMC_GO_IRQ_STATE:
	case SD_IO_RW_DIRECT:
	case SD_IO_RW_EXTENDED:
		rsptype = STSD_RSPTYPE_R5;
		break;
	case SD_SEND_RELATIVE_ADDR:
		rsptype = STSD_RSPTYPE_R6;
		break;
	case SD_SEND_IF_COND:
		/* WEIRD */
		/*rsptype = STSD_RSPTYPE_R7;*/
		rsptype = STSD_RSPTYPE_R6;
		break;
	default:
		break;
	}

	return rsptype;
}

static inline void stsd_card_set_bad(struct stsd_host *host)
{
	set_bit(__STSD_BAD_CARD, &host->flags);
}

static inline void stsd_card_unset_bad(struct stsd_host *host)
{
	clear_bit(__STSD_BAD_CARD, &host->flags);
}

static inline int stsd_card_is_bad(struct stsd_host *host)
{
	return test_bit(__STSD_BAD_CARD, &host->flags);
}

static inline void stsd_card_set_sdhc(struct stsd_host *host)
{
	set_bit(__STSD_SDHC, &host->flags);
}

static inline void stsd_card_unset_sdhc(struct stsd_host *host)
{
	clear_bit(__STSD_SDHC, &host->flags);
}

static inline int stsd_card_is_sdhc(struct stsd_host *host)
{
	return test_bit(__STSD_SDHC, &host->flags);
}

static inline void stsd_card_set_manual_setup(struct stsd_host *host)
{
	set_bit(__STSD_MANUAL_SETUP, &host->flags);
}

static inline void stsd_card_unset_manual_setup(struct stsd_host *host)
{
	clear_bit(__STSD_MANUAL_SETUP, &host->flags);
}

static inline int stsd_card_needs_manual_setup(struct stsd_host *host)
{
	return test_bit(__STSD_MANUAL_SETUP, &host->flags);
}

static inline int stsd_card_status_is_inserted(u32 status)
{
	return (status & STSD_STATUS_CARD_INSERTED)
			 == STSD_STATUS_CARD_INSERTED;
}

static inline int stsd_card_status_is_initialized(u32 status)
{
	return (status & STSD_STATUS_CARD_INITIALIZED)
			 == STSD_STATUS_CARD_INITIALIZED;
}

/*
 * Hardware.
 *
 */

/*
 * Handy small buffer routines.
 * We use a small static aligned buffer to avoid allocations for short-lived
 * operations involving 1 to 4 byte data transfers to/from IOS.
 *
 */

static u32 stsd_small_buf[L1_CACHE_BYTES / sizeof(u32)]
		    __attribute__ ((aligned(STARLET_IPC_DMA_ALIGN + 1)));
static const size_t stsd_small_buf_size = sizeof(stsd_small_buf_size);
static DEFINE_MUTEX(stsd_small_buf_lock);

static u32 *stsd_small_buf_get(void)
{
	u32 *buf;

	if (!mutex_trylock(&stsd_small_buf_lock))
		buf = starlet_kzalloc(stsd_small_buf_size, GFP_NOIO);
	else {
		memset(stsd_small_buf, 0, stsd_small_buf_size);
		buf = stsd_small_buf;
	}

	return buf;
}

void stsd_small_buf_put(u32 *buf)
{
	if (buf == stsd_small_buf)
		mutex_unlock(&stsd_small_buf_lock);
	else
		starlet_kfree(buf);
}


/*
 * SD Host Standard Registers accessors.
 *
 */

/*
 * @data must be aligned
 * @size must be between 1 and 4
 */
static int __stsd_hsr_in(struct stsd_host *host,
			 u32 addr, u32 *data, size_t size)
{
	struct stsd_reg_query *query;
	int error;

	query = starlet_kzalloc(sizeof(*query), GFP_ATOMIC);
	if (!query)
		return -ENOMEM;

	query->addr = addr;
	query->size = size;

	error = starlet_ioctl(host->fd, STSD_IOCTL_GETHSR,
				  query, sizeof(*query), data, sizeof(*data));

	starlet_kfree(query);

	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}

static int __stsd_hsr_out(struct stsd_host *host,
			   u32 addr, u32 *data, size_t size)
{
	struct stsd_reg_query *query;
	int error;

	query = starlet_kzalloc(sizeof(*query), GFP_ATOMIC);
	if (!query)
		return -ENOMEM;

	query->addr = addr;
	query->size = size;
	query->data = *data;

	error = starlet_ioctl(host->fd, STSD_IOCTL_SETHSR,
				  query, sizeof(*query), NULL, 0);

	starlet_kfree(query);

	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}


static int stsd_hsr_in(struct stsd_host *host,
		       u32 reg, void *buf, size_t size)
{
	u32 *local_buf;
	int error;

	/* we do 8, 16 and 32 bits reads */
	if (size > 4)
		return -EINVAL;

	local_buf = stsd_small_buf_get();
	if (!local_buf)
		return -ENOMEM;

	error = __stsd_hsr_in(host, reg, local_buf, size);
	if (!error) {
		switch (size) {
		case 1:
			*(u8 *)buf = *local_buf & 0xff;
			break;
		case 2:
			*(u16 *)buf = *local_buf & 0xffff;
			break;
		case 4:
			*(u32 *)buf = *local_buf;
			break;
		default:
			BUG();
			break;
		}
	}

	stsd_small_buf_put(local_buf);

	return error;
}

static int stsd_hsr_out(struct stsd_host *host,
			u32 reg, void *buf, size_t size)
{
	u32 *local_buf;
	int error;

	/* we do 8, 16 and 32 bits reads */
	if (size > 4)
		return -EINVAL;

	local_buf = stsd_small_buf_get();
	if (!local_buf)
		return -ENOMEM;

	switch (size) {
	case 1:
		*local_buf = *(u8 *)buf;
		break;
	case 2:
		*local_buf = *(u16 *)buf;
		break;
	case 4:
		*local_buf = *(u32 *)buf;
		break;
	default:
		BUG();
		break;
	}
	error = __stsd_hsr_out(host, reg, local_buf, size);

	stsd_small_buf_put(local_buf);

	return error;
}

#define __declare_stsd_hsr_wait_for_resp(_type)				\
static int stsd_hsr_wait_for_resp_##_type(struct stsd_host *host,	\
				  u32 reg, _type resp, _type resp_mask,	\
				  unsigned long jiffies)		\
{									\
	_type val;							\
	int error;							\
									\
	unsigned long cycles = 10;					\
	while (cycles-- > 0) {						\
		error = stsd_hsr_in(host, reg, &val, sizeof(val));	\
		if (error)						\
			return error;					\
		if ((val & resp_mask) == resp)				\
			return 0;					\
		mdelay(10);						\
	}								\
	return -ENODATA;						\
}

__declare_stsd_hsr_wait_for_resp(u8);
__declare_stsd_hsr_wait_for_resp(u16);

#define __declare_stsd_hsr_in(_type)					\
static inline _type stsd_hsr_in_##_type(struct stsd_host *host, u32 reg) \
{									\
	_type val;							\
									\
	stsd_hsr_in(host, reg, &val, sizeof(val));			\
	return val;							\
}

__declare_stsd_hsr_in(u8);
__declare_stsd_hsr_in(u16);
__declare_stsd_hsr_in(u32);

#define __declare_stsd_hsr_out(_type)					\
static inline void stsd_hsr_out_##_type(struct stsd_host *host, u32 reg,\
					 _type val)			\
{									\
	stsd_hsr_out(host, reg, &val, sizeof(val));			\
}

__declare_stsd_hsr_out(u8);
__declare_stsd_hsr_out(u16);
__declare_stsd_hsr_out(u32);



/*
 * Ioctl helpers.
 *
 */

static int stsd_ioctl_small_read(struct stsd_host *host, int request,
				 void *buf, size_t size)
{
	void *local_buf;
	int error;

	/* we do 8, 16 and 32 bits reads */
	if (size > stsd_small_buf_size) {
		error = -EINVAL;
		goto done;
	}

	local_buf = stsd_small_buf_get();
	if (!local_buf) {
		error = -ENOMEM;
		goto done;
	}

	error = starlet_ioctl(host->fd, request,
				   NULL, 0, local_buf, size);
	if (!error)
		memcpy(buf, local_buf, size);

	stsd_small_buf_put(local_buf);

done:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}

static int stsd_ioctl_small_write(struct stsd_host *host, int request,
				  void *buf, size_t size)
{
	void *local_buf;
	int error;

	/* we do 8, 16 and 32 bits writes */
	if (size > stsd_small_buf_size) {
		error = -EINVAL;
		goto done;
	}

	local_buf = stsd_small_buf_get();
	if (!local_buf) {
		error = -ENOMEM;
		goto done;
	}

	memcpy(local_buf, buf, size);
	error = starlet_ioctl(host->fd, request,
				   local_buf, size, NULL, 0);

	stsd_small_buf_put(local_buf);

done:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}


/*
 * Hardware interfaces.
 *
 */

static int stsd_get_status(struct stsd_host *host, u32 *status)
{
	int error;

	error = stsd_ioctl_small_read(host, STSD_IOCTL_GETSTATUS,
				       status, sizeof(*status));
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}

static void stsd_set_bus_width(struct stsd_host *host, int width)
{
	u8 hcr;

	hcr = stsd_hsr_in_u8(host, SDHCI_HOST_CONTROL);
	if (width == 4) {
		hcr |= SDHCI_CTRL_4BITBUS;
	} else {
		hcr &= ~SDHCI_CTRL_4BITBUS;
		width = 1;
	}
	stsd_hsr_out_u8(host, SDHCI_HOST_CONTROL, hcr);
	host->bus_width = width;
}

static int stsd_set_clock(struct stsd_host *host, unsigned int clock)
{
	int error;
	u32 divisor;

	for (divisor = 1; divisor <= 32; divisor <<= 1) {
		if (host->f_max / divisor <= clock)
			break;
	}

	error = stsd_ioctl_small_write(host, STSD_IOCTL_SETCLOCK,
					&divisor, sizeof(divisor));
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	else
		host->clock = clock;

	return error;
}

static int stsd_reset_card(struct stsd_host *host)
{
	struct mmc_card *card = &host->card;
	int error;
	u32 status;

	stsd_card_unset_bad(host);
	stsd_card_unset_sdhc(host);
	stsd_card_unset_manual_setup(host);

	memset(&card->cid, 0, sizeof(struct mmc_cid));
	memset(&card->csd, 0, sizeof(struct mmc_csd));
	host->card.rca = 0;

	error = stsd_ioctl_small_read(host, STSD_IOCTL_RESET,
				       &status, sizeof(status));
	if (error) {
		if (error != STSD_ERR_INVALID_CARD)
			DBG("%s: error=%d (%08x)\n", __func__, error, error);
	} else {
		host->card.rca = status >> 16;
		host->status = status & 0xffff;
	}

	return error;
}

#if 0
static int stsd_get_ocr(struct stsd_host *host)
{
	int error;
	u32 ocr;

	error = stsd_ioctl_small_read(host, STSD_IOCTL_GETOCR,
				       &ocr, sizeof(ocr));
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	else
		host->ocr = ocr;

	return error;
}
#endif

/*
 * Command engine.
 *
 */

static int stsd_send_command(struct stsd_host *host,
			     u32 opcode, u32 type, u32 arg,
			     void *buf, size_t buf_len)
{
	struct scatterlist in[2], io[1];
	struct stsd_command *cmd;
	u32 *reply;
	size_t reply_len;
	int error;

	reply_len = 4 * sizeof(u32);
	if (buf_len > reply_len)
		return -EINVAL;

	cmd = starlet_kzalloc(sizeof(*cmd), GFP_NOIO);
	if (!cmd)
		return -ENOMEM;

	reply = starlet_kzalloc(reply_len, GFP_NOIO);
	if (!reply) {
		starlet_kfree(cmd);
		return -ENOMEM;
	}

	cmd->opcode = opcode;
	cmd->arg = arg;

	cmd->cmdtype = type;
	cmd->rsptype = stsd_opcode_to_rsptype(opcode);
	if (opcode == MMC_SELECT_CARD && arg == 0)
		cmd->rsptype = STSD_RSPTYPE_NONE;

	if (stsd_card_needs_manual_setup(host)) {
		/*
		 * We need to use ioctlvs, instead of ioctls, to drive
		 * manually initialized cards.
		 * This makes IOS "cooperative" :)
		 */
		sg_init_table(in, 2);
		sg_set_buf(&in[0], cmd, sizeof(*cmd));
		sg_set_buf(&in[1], reply, 0);

		sg_init_table(io, 1);
		sg_set_buf(&io[0], reply, reply_len);

		error = starlet_ioctlv(host->fd, STSD_IOCTL_SENDCMD,
					   2, in, 1, io);
	} else {
		error = starlet_ioctl(host->fd, STSD_IOCTL_SENDCMD,
				      cmd, sizeof(*cmd), reply, reply_len);
	}

	if (error) {
		DBG("%s: error=%d (%08x), opcode=%d\n", __func__,
		    error, error, opcode);
	} else {
		if (buf)
			memcpy(buf, reply, buf_len);
	}

	starlet_kfree(reply);
	starlet_kfree(cmd);

	return error;
}

static int stsd_send_app_command(struct stsd_host *host,
				 u32 opcode, u32 type, u32 arg,
				 void *buf, size_t buf_len)
{
	int error;

	error = stsd_send_command(host, MMC_APP_CMD, STSD_CMDTYPE_AC,
				  host->card.rca << 16, NULL, 0);
	if (!error) {
		error = stsd_send_command(host, opcode, type, arg,
					  buf, buf_len);
	}
	return error;
}


/*
 * Command helpers.
 *
 */


static int stsd_cmd_read_cxd(struct stsd_host *host, int request, void *buf)
{
	int error;
	u32 *q, savedq;
	u8 *p, crc;
	const size_t size = 128/8*sizeof(u8);

	error = stsd_send_command(host, request, STSD_CMDTYPE_AC,
				   host->card.rca << 16, buf, size);

	if (!error) {
		/*
		 * WEIRD,
		 * starlet sends CSD and CID contents in a very special way.
		 *
		 * If the 128 bit register value is:
		 *   0123456789abcdef
		 * starlet will send it as:
		 *   bcde789a3456f012
		 * with byte f (the crc field) zeroed.
		 */

		/* bcde789a3456f012 -> f0123456789abcde */
		q = buf;
		savedq = q[0];
		q[0] = q[3];
		q[3] = savedq;
		savedq = q[1];
		q[1] = q[2];
		q[2] = savedq;

		/* f0123456789abcde -> 0123456789abcdef */
		p = buf;
		crc = p[0];
		memcpy(p, p+1, size-1);
		p[size-1] = crc;
	}
	return error;
}

static int stsd_cmd_read_csd(struct stsd_host *host)
{
	return stsd_cmd_read_cxd(host, MMC_SEND_CSD, host->card.raw_csd);
}

static int stsd_cmd_read_cid(struct stsd_host *host)
{
	return stsd_cmd_read_cxd(host, MMC_SEND_CID, host->card.raw_cid);
}

static int stsd_cmd_all_send_cid(struct stsd_host *host)
{
	const size_t size = 128/8*sizeof(u8);

	/* WEIRD, don't use CMDTYPE_BCR for MMC_ALL_SEND_CID */
	return stsd_send_command(host, MMC_ALL_SEND_CID, 0,
				 host->card.rca << 16,
				 host->card.raw_cid, size);
}

static int stsd_cmd_set_relative_addr(struct stsd_host *host, unsigned int rca)
{
	int error;
	u32 reply;

	error = stsd_send_command(host, MMC_SET_RELATIVE_ADDR, STSD_CMDTYPE_AC,
				  rca, &reply, sizeof(reply));
	if (!error) {
		host->card.rca = reply >> 16;
		/* DBG("rca=%d, new_rca=%x\n", rca, host->card.rca); */
	}
	return error;
}


static int stsd_cmd_select_card(struct stsd_host *host)
{
	return stsd_send_command(host, MMC_SELECT_CARD, STSD_CMDTYPE_AC,
				 host->card.rca << 16,
				 NULL, 0);
}

static int stsd_cmd_deselect_card(struct stsd_host *host)
{
	return stsd_send_command(host, MMC_SELECT_CARD, STSD_CMDTYPE_AC,
				 0,
				 NULL, 0);
}

static int stsd_cmd_set_block_len(struct stsd_host *host, unsigned int len)
{
	return stsd_send_command(host, MMC_SET_BLOCKLEN, STSD_CMDTYPE_AC,
				 len,
				 NULL, 0);
}

static int stsd_app_cmd_set_bus_width(struct stsd_host *host, int width)
{
	int error;
	u16 val;

	if (width == 4)
		val = SD_BUS_WIDTH_4;
	else
		val = SD_BUS_WIDTH_1;

	error = stsd_send_app_command(host, SD_APP_SET_BUS_WIDTH,
				      STSD_CMDTYPE_AC,
				      val, NULL, 0);
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}



static int stsd_setup_host_controller(struct stsd_host *host)
{
	const u32 mask = SDHCI_INT_RESPONSE | SDHCI_INT_DATA_END |
				SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE |
				SDHCI_INT_TIMEOUT | SDHCI_INT_CRC |
				SDHCI_INT_END_BIT | SDHCI_INT_INDEX |
				SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
				SDHCI_INT_ACMD12ERR;
	u8 rst, pwr, clk_idx;
	int error;

	/*
	 * Reset host controller.
	 */

	/* write 1 to the Reset All bit in the Software Reset register ... */
	rst = SDHCI_RESET_ALL;
	stsd_hsr_out_u8(host, SDHCI_SOFTWARE_RESET, rst);

	/* ... then wait for the Reset All bit to be cleared */
	error = stsd_hsr_wait_for_resp_u8(host, SDHCI_SOFTWARE_RESET,
					  0, rst,
					  100*(HZ/1000));
	if (error) {
		drv_printk(KERN_ERR, "host controller didn't get out of"
			   " reset\n");
		goto done;
	}

	/*
	 * Setup interrupt sources.
	 */

	/* ack the interrupt sources that IOS uses ... */
	stsd_hsr_out_u32(host, SDHCI_INT_ENABLE, mask);
	stsd_hsr_in_u32(host, SDHCI_INT_ENABLE);
	/* ... then unmask them */
	stsd_hsr_out_u32(host, SDHCI_SIGNAL_ENABLE, mask);
	stsd_hsr_in_u32(host, SDHCI_SIGNAL_ENABLE);

	/*
	 * Setup bus power.
	 */

	/* FIXME, we should use capabilities register here */
	/* for now use 3.3V setting */
	pwr = SDHCI_POWER_330;

	/* turn on bus power and use selected voltage setting */
	stsd_hsr_out_u8(host, SDHCI_POWER_CONTROL, pwr & ~SDHCI_POWER_ON);
	stsd_hsr_out_u8(host, SDHCI_POWER_CONTROL, pwr | SDHCI_POWER_ON);

	/*
	 * Initialize clocks.
	 */

	/* FIXME, we should use capabilities register here */
	/* for now use index 01h which is base clock divided by 2 */
	clk_idx = 1;

	/* disable clock signalling... */
	stsd_hsr_out_u16(host, SDHCI_CLOCK_CONTROL, 0);
	/* ... then enable internal clock ... */
	stsd_hsr_out_u16(host, SDHCI_CLOCK_CONTROL,
				 SDHCI_CLOCK_INT_EN |
				 (clk_idx << SDHCI_DIVIDER_SHIFT));
	/* ... and wait until it gets stable */
	error = stsd_hsr_wait_for_resp_u16(host, SDHCI_CLOCK_CONTROL,
					   SDHCI_CLOCK_INT_STABLE,
					   SDHCI_CLOCK_INT_STABLE,
					   1*HZ);
	if (error) {
		drv_printk(KERN_ERR, "internal clock didn't get stable\n");
		goto done;
	}

	/* SD clock can be enabled now */
	stsd_hsr_out_u16(host, SDHCI_CLOCK_CONTROL,
				 SDHCI_CLOCK_INT_EN |
				 SDHCI_CLOCK_CARD_EN |
				 (1 << SDHCI_DIVIDER_SHIFT));

	/*
	 * Setup timeout.
	 */

	/* setup timeout to TMCLK * 2^27 */
	stsd_hsr_out_u8(host, SDHCI_TIMEOUT_CONTROL,
				STSD_TIMEOUT_CONTROL_DIV(27));

done:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}

static int stsd_setup_card(struct stsd_host *host)
{
	const u8 check_pattern = 0xaa;
	u32 arg;
	u32 resp[4];
	int i;
	int error;

	/* WEIRD, don't use CMDTYPE_BC for MMC_GO_IDLE_STATE */
	error = stsd_send_command(host, MMC_GO_IDLE_STATE, 0,
				  0, NULL, 0);
	if (error)
		goto done;

#define STSD_VHS(a)	((((a)&0x0f)<<8))
#define STSD_VHS_27_36	STSD_VHS(0x1)

	/* WEIRD, don't use CMDTYPE_BC for SD_SEND_IF_COND */
	arg = STSD_VHS_27_36 | check_pattern;
	error = stsd_send_command(host, SD_SEND_IF_COND, 0,
				  arg, &resp, sizeof(resp));
	if (error)
		goto done;

	if ((resp[0] & 0xff) != check_pattern) {
		DBG("arg=0x%x, resp[0]=0x%x\n", arg, resp[0]);
		error = -ENODEV;
		goto done;
	}

	/*
	 * At this point we have identified a v2.00 SD Memory Card.
	 *
	 */

	/*
	 * Get OCR
	 */

#define STSD_OCR_HCS	(1<<30)	/* Host Capacity Support */
#define STSD_OCR_CCS	(1<<30)	/* Card Capacity Support */

	for (i = 0; i < 100; i++) {
		/* WEIRD, don't use CMDTYPE_BCR for MMC_APP_CMD */
		error = stsd_send_command(host, MMC_APP_CMD, STSD_CMDTYPE_AC,
					  0, NULL, 0);
		if (error)
			goto done;

		/* WEIRD, don't use CMDTYPE_BCR for SD_APP_OP_COND */
		error = stsd_send_command(host, SD_APP_OP_COND, 0,
					  STSD_OCR_HCS|
					  MMC_VDD_32_33|MMC_VDD_33_34,
					  &resp, sizeof(resp));
		if (error)
			goto done;

		if ((resp[0] & MMC_CARD_BUSY) != 0) {
			/* card power up completed */
			break;
		}

		error = -ETIMEDOUT;
		mdelay(10);
	}
	if (error) {
		drv_printk(KERN_ERR, "timed out while trying to get OCR\n");
		goto done;
	}

	if ((resp[0] & STSD_OCR_CCS) != 0) {
		/* high capacity card */
		stsd_card_set_sdhc(host);
	}

	error = stsd_cmd_all_send_cid(host);
	if (error)
		goto done;

	error = stsd_cmd_set_relative_addr(host, 0);
	if (error)
		goto done;

done:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}

static int stsd_reopen_sdio(struct stsd_host *host)
{
	int error = 0;

	starlet_close(host->fd);
	host->fd = starlet_open(stsd_dev_sdio_slot0, 1);
	if (host->fd < 0) {
		drv_printk(KERN_ERR, "unable to re-open %s\n",
			   stsd_dev_sdio_slot0);
		error = -ENODEV;
	}
	return error;
}


static int stsd_welcome_card(struct stsd_host *host)
{
	size_t block_len; /* in bytes */
	u32 status;
	int error;

	mutex_lock(&host->io_mutex);

	/*
	 * Re-open the sdio device if things look wrong.
	 */
	error = stsd_get_status(host, &status);
	if (error == STARLET_EINVAL) {
		error = stsd_reopen_sdio(host);
		if (error)
			goto err_bad_card;
	}

	/*
	 * Try a normal initialization sequence first, and revert to
	 * manual mode if that fails.
	 */

	stsd_reset_card(host);

	error = stsd_get_status(host, &status);
	if (error)
		goto err_bad_card;
	if (!stsd_card_status_is_inserted(status)) {
		drv_printk(KERN_ERR, "no card found\n");
		goto err_bad_card;
	}

	if (!stsd_card_status_is_initialized(status)) {
		/* manual initialization, needed for SDHC support */
		stsd_card_set_manual_setup(host);

		error = stsd_reopen_sdio(host);
		if (error)
			goto err_bad_card;

		error = stsd_setup_host_controller(host);
		if (error)
			goto err_bad_card;

		error = stsd_setup_card(host);
		if (error)
			goto err_bad_card;
	}

#if 0
	/* read Operating Conditions Register */
	error = stsd_get_ocr(host);
	if (error < 0)
		goto err_bad_card;
#endif

	error = stsd_cmd_deselect_card(host);
	if (error)
		goto err_bad_card;

	/* read and decode the Card Specific Data */
	error = stsd_cmd_read_csd(host);
	if (error)
		goto err_bad_card;
	mmc_decode_csd(&host->card);

	/* read and decode the Card Identification Data */
	error = stsd_cmd_read_cid(host);
	if (error)
		goto err_bad_card;
	mmc_decode_cid(&host->card);

	error = stsd_cmd_select_card(host);
	if (error)
		goto err_bad_card;

	stsd_set_clock(host, host->card.csd.max_dtr);

	/* FIXME check if card supports 4 bit bus width */
	stsd_set_bus_width(host, 4);
	error = stsd_app_cmd_set_bus_width(host, 4);
	if (error)
		goto err_bad_card;

	/* setup block length */
	block_len = KERNEL_SECTOR_SIZE;
	error = stsd_cmd_set_block_len(host, block_len);
	if (error)
		goto err_bad_card;

#if 0
	mmc_card_set_present(&host->card);
#endif

	mutex_unlock(&host->io_mutex);

	drv_printk(KERN_INFO, "descr \"%s\", size %luk, block %ub,"
		  " serial %08x\n",
		  host->card.cid.prod_name,
		  (unsigned long)((host->card.csd.capacity / 1024) *
			  (1 << host->card.csd.read_blkbits)),
		  1 << host->card.csd.read_blkbits,
		  host->card.cid.serial);

	error = 0;
	goto out;

err_bad_card:
	mutex_unlock(&host->io_mutex);
	stsd_card_set_bad(host);
out:
	return error;
}


/*
 * Block layer helper routines.
 *
 */

static int stsd_do_block_transfer(struct stsd_host *host, int write,
				  unsigned long start,
				  void *buf, size_t nr_blocks)
{
	struct stsd_xfer *xfer = host->xfer;
	struct stsd_command *cmd = xfer->cmd;
	int error;

	xfer->direction = (write) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	xfer->size = nr_blocks * xfer->blk_size;

	if (xfer->size > xfer->bounce_buf_size) {
		drv_printk(KERN_ERR, "oops, request size %d > %d\n",
				xfer->size, xfer->bounce_buf_size);
		return -ENOMEM;
	}

	/*
	 * This is stupid.
	 * Starlet expects the buffer to be an input iovec (from starlet
	 * point of view) even for reads. Thus, map the buffer explicitly here.
	 */
	if (write)
		memcpy(xfer->bounce_buf, buf, xfer->size);
	__dma_sync(xfer->bounce_buf, xfer->size, xfer->direction);
/*
	xfer->dma_addr = dma_map_single(host->dev, buf,
					xfer->size, xfer->direction);
*/

	starlet_ioh_sg_init_table(xfer->in, 2);
	starlet_ioh_sg_set_buf(&xfer->in[0], cmd, sizeof(*cmd));
	starlet_ioh_sg_set_buf(&xfer->in[1], xfer->bounce_buf, xfer->size);

	starlet_ioh_sg_init_table(xfer->io, 1);
	starlet_ioh_sg_set_buf(&xfer->io[0], xfer->reply, xfer->reply_len);

	cmd->opcode = (write) ? MMC_WRITE_MULTIPLE_BLOCK :
				MMC_READ_MULTIPLE_BLOCK;
	cmd->arg = start;
	cmd->cmdtype = STSD_CMDTYPE_AC; /* STSD_CMDTYPE_ADTC */
	cmd->rsptype = stsd_opcode_to_rsptype(cmd->opcode);
	cmd->blk_count = nr_blocks;
	cmd->blk_size = xfer->blk_size;
	cmd->dma_addr = xfer->dma_addr; /* bounce buf */
	cmd->is_dma = 1;

	error = starlet_ioh_ioctlv(host->fd, STSD_IOCTLV_SENDCMD,
				   2, xfer->in, 1, xfer->io);
/*
	dma_unmap_single(host->dev,
			 xfer->dma_addr, xfer->size, xfer->direction);
*/

	if (!write)
		memcpy(buf, xfer->bounce_buf, xfer->size);

	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}

static int stsd_check_request(struct stsd_host *host, struct request *req)
{
	unsigned long nr_sectors;

	if (req->cmd_type != REQ_TYPE_FS)
		return -EIO;

	if (test_bit(__STSD_MEDIA_CHANGED, &host->flags)) {
		drv_printk(KERN_ERR, "media changed, aborting\n");
		return -ENOMEDIUM;
	}

	/* unit is kernel sectors */
	nr_sectors =
	    host->card.csd.capacity << (host->card.csd.read_blkbits -
					KERNEL_SECTOR_SHIFT);

	/* keep our reads within limits */
	if (blk_rq_pos(req) + blk_rq_cur_sectors(req) > nr_sectors) {
		drv_printk(KERN_ERR, "reading past end, aborting\n");
		return -EINVAL;
	}

	return 0;
}

static int stsd_do_request(struct stsd_host *host, struct request *req)
{
	unsigned long nr_blocks; /* in card blocks */
	unsigned long start;
	int write;
	int error;

	error = stsd_check_request(host, req);
	if (error)
		goto out;

	write = (rq_data_dir(req) == READ) ? 0 : 1;

	start = blk_rq_pos(req);
	if (!stsd_card_is_sdhc(host))
		start <<= KERNEL_SECTOR_SHIFT;
	nr_blocks = blk_rq_cur_sectors(req);

	error = stsd_do_block_transfer(host, write,
					start, req->buffer, nr_blocks);
	if (error)
		DBG("%s: error=%d (%08x), start=%lu, \n", __func__,
		    error, error, start);

out:
	return error;
}

static int stsd_io_thread(void *param)
{
	struct stsd_host *host = param;
	struct request *req;
	unsigned long flags;
	int error;

	current->flags |= PF_NOFREEZE|PF_MEMALLOC;

	mutex_lock(&host->io_mutex);
	for (;;) {
		req = NULL;
		set_current_state(TASK_INTERRUPTIBLE);

		spin_lock_irqsave(&host->queue_lock, flags);
		if (!blk_queue_stopped(host->queue))
			req = blk_fetch_request(host->queue);
		spin_unlock_irqrestore(&host->queue_lock, flags);

		if (!req) {
			if (kthread_should_stop()) {
				set_current_state(TASK_RUNNING);
				break;
			}
			mutex_unlock(&host->io_mutex);
			schedule();
			mutex_lock(&host->io_mutex);
			continue;
		}
		set_current_state(TASK_INTERRUPTIBLE);
		error = stsd_do_request(host, req);

		spin_lock_irqsave(&host->queue_lock, flags);
		__blk_end_request_cur(req, error);
		spin_unlock_irqrestore(&host->queue_lock, flags);
	}
	mutex_unlock(&host->io_mutex);

	return 0;
}

static void stsd_request_func(struct request_queue *q)
{
	struct stsd_host *host = q->queuedata;
	wake_up_process(host->io_thread);
}

/*
 * Block device hooks.
 *
 */

static DEFINE_SEMAPHORE(open_lock);

static int stsd_open(struct block_device *bdev, fmode_t mode)
{
	struct stsd_host *host = bdev->bd_disk->private_data;
	int error = 0;

	if (!host || host->fd < 0)
		return -ENXIO;

	/* honor exclusive open mode */
	if (host->refcnt == -1 ||
	    (host->refcnt && (mode & FMODE_EXCL))) {
		error = -EBUSY;
		goto out;
	}

	/* this takes care of revalidating the media if needed */
	check_disk_change(bdev);
	if (!host->card.csd.capacity) {
		error = -ENOMEDIUM;
		goto out;
	}

	down(&open_lock);

	if ((mode & FMODE_EXCL))
		host->refcnt = -1;
	else
		host->refcnt++;

	up(&open_lock);

out:
	return error;

}

static void stsd_release(struct gendisk *disk, fmode_t mode)
{
	struct stsd_host *host = disk->private_data;

	if (!host)
		return;

	down(&open_lock);

	if (host->refcnt > 0)
		host->refcnt--;
	else
		host->refcnt = 0;

	up(&open_lock);

	if (!host->refcnt && host->fd == -1)
		kfree(host);

}

static int stsd_media_changed(struct gendisk *disk)
{
	struct stsd_host *host = disk->private_data;
	unsigned int last_serial;
	int error;

	/* report a media change for zombies */
	if (!host)
		return 1;

	/* report a media change if someone forced it */
	if (test_bit(__STSD_MEDIA_CHANGED, &host->flags))
		return 1;

	/* REVISIT use the starlet provided iotcl to check the status */

	mutex_lock(&host->io_mutex);

	/* check if the serial number of the card changed */
	last_serial = host->card.cid.serial;
	error = stsd_cmd_deselect_card(host);
	if (!error) {
		error = stsd_cmd_read_cid(host);
		if (!error)
			error = stsd_cmd_select_card(host);
	}

	mutex_unlock(&host->io_mutex);

	if (!error && last_serial == host->card.cid.serial && last_serial)
		clear_bit(__STSD_MEDIA_CHANGED, &host->flags);
	else
		set_bit(__STSD_MEDIA_CHANGED, &host->flags);

	return (host->flags & STSD_MEDIA_CHANGED) ? 1 : 0;
}

static int stsd_revalidate_disk(struct gendisk *disk)
{
	struct stsd_host *host = disk->private_data;
	int error = 0;

	/* report missing medium for zombies */
	if (!host) {
		error = -ENOMEDIUM;
		goto out;
	}

	/* the block layer likes to call us multiple times... */
	if (!stsd_media_changed(host->disk))
		goto out;

	/* get the card into a known status */
	error = stsd_welcome_card(host);
	if (error < 0 || stsd_card_is_bad(host)) {
		drv_printk(KERN_ERR, "card welcome failed\n");
		if (stsd_card_is_bad(host))
			drv_printk(KERN_ERR, "stsd_card_is_bad() true\n");
		if (error < 0)
			drv_printk(KERN_ERR, "error = %d\n", error);
		error = -ENOMEDIUM;
		/* FALL THROUGH */
	}

	/* inform the block layer about various sizes */
	blk_queue_logical_block_size(host->queue, KERNEL_SECTOR_SIZE);
	set_capacity(host->disk, host->card.csd.capacity <<
			 (host->card.csd.read_blkbits - KERNEL_SECTOR_SHIFT));

	clear_bit(__STSD_MEDIA_CHANGED, &host->flags);

out:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}

static int stsd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	geo->cylinders = get_capacity(bdev->bd_disk) / (4 * 16);
	geo->heads = 4;
	geo->sectors = 16;
	return 0;
}

static struct block_device_operations stsd_fops = {
	.owner = THIS_MODULE,
	.open = stsd_open,
	.release = stsd_release,
	.revalidate_disk = stsd_revalidate_disk,
	.media_changed = stsd_media_changed,
	.getgeo = stsd_getgeo,
};

/*
 * Setup routines.
 *
 */

static int stsd_init_xfer(struct stsd_host *host)
{
	struct stsd_xfer *xfer;

	xfer = starlet_kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return -ENOMEM;

	xfer->reply_len = 4 * sizeof(u32);
	xfer->reply = starlet_ioh_kzalloc(xfer->reply_len);
	if (!xfer->reply) {
		starlet_kfree(xfer);
		return -ENOMEM;
	}
	xfer->cmd = starlet_ioh_kzalloc(sizeof(*xfer->cmd));
	if (!xfer->cmd) {
		starlet_ioh_kfree(xfer->reply);
		starlet_kfree(xfer);
		return -ENOMEM;
	}
	xfer->bounce_buf_size = STSD_MAX_SECTORS * KERNEL_SECTOR_SIZE;
	xfer->bounce_buf = starlet_ioh_kzalloc(xfer->bounce_buf_size);
	if (!xfer->bounce_buf) {
		starlet_ioh_kfree(xfer->cmd);
		starlet_ioh_kfree(xfer->reply);
		starlet_kfree(xfer);
		return -ENOMEM;
	}
	xfer->dma_addr = starlet_ioh_virt_to_phys(xfer->bounce_buf);

	xfer->blk_size = KERNEL_SECTOR_SIZE;

	host->xfer = xfer;

	return 0;
}

static void stsd_exit_xfer(struct stsd_host *host)
{
	struct stsd_xfer *xfer = host->xfer;

	starlet_ioh_kfree(xfer->cmd);
	starlet_ioh_kfree(xfer->reply);
	starlet_kfree(host->xfer);
}

static int stsd_init_blk_dev(struct stsd_host *host)
{
	struct gendisk *disk;
	struct request_queue *queue;
	int error;

	mutex_init(&host->io_mutex);

	/* queue */
	error = -ENOMEM;
	spin_lock_init(&host->queue_lock);
	queue = blk_init_queue(stsd_request_func, &host->queue_lock);
	if (!queue) {
		drv_printk(KERN_ERR, "error initializing queue\n");
		goto err_blk_init_queue;
	}
	host->max_phys_segments = 1;
	blk_queue_max_segments(queue, host->max_phys_segments);
	blk_queue_max_hw_sectors(queue, STSD_MAX_SECTORS); /* 16 * 512 = 8K */
	blk_queue_dma_alignment(queue, STARLET_IPC_DMA_ALIGN);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, queue);
	queue->queuedata = host;
	host->queue = queue;

	/* disk */
	disk = alloc_disk(1 << MMC_SHIFT);
	if (!disk) {
		drv_printk(KERN_ERR, "error allocating disk\n");
		goto err_alloc_disk;
	}
	disk->major = STSD_MAJOR;
	disk->first_minor = 0 << MMC_SHIFT;
	disk->fops = &stsd_fops;
	sprintf(disk->disk_name, "%s%c", STSD_NAME, 'a');
	disk->private_data = host;
	disk->queue = host->queue;
	host->disk = disk;

	error = 0;
	goto out;

err_alloc_disk:
	blk_cleanup_queue(host->queue);
	host->queue = NULL;
err_blk_init_queue:
out:
	return error;
}

static void stsd_exit_blk_dev(struct stsd_host *host)
{
	blk_cleanup_queue(host->queue);
	put_disk(host->disk);
}

static int stsd_init_io_thread(struct stsd_host *host)
{
	int result = 0;

	host->io_thread = kthread_run(stsd_io_thread, host, "ksdio");
	if (IS_ERR(host->io_thread)) {
		drv_printk(KERN_ERR, "error creating io thread\n");
		result = PTR_ERR(host->io_thread);
	}
	return result;
}

static void stsd_exit_io_thread(struct stsd_host *host)
{
	if (!IS_ERR(host->io_thread)) {
		wake_up_process(host->io_thread);
		kthread_stop(host->io_thread);
		host->io_thread = ERR_PTR(-EINVAL);
	}
}

static int stsd_init(struct stsd_host *host)
{
	int error;

	host->refcnt = 0;
	spin_lock_init(&host->lock);
	set_bit(__STSD_MEDIA_CHANGED, &host->flags);
	host->f_max = 25000000; /* 25MHz */

	host->fd = starlet_open(stsd_dev_sdio_slot0, 0);
	if (host->fd < 0) {
		drv_printk(KERN_ERR, "unable to open %s\n",
			   stsd_dev_sdio_slot0);
		return -ENODEV;
	}

	error = stsd_init_blk_dev(host);
	if (error)
		goto out;

	error = stsd_init_xfer(host);
	if (error)
		goto err_blk_dev;

	error = stsd_revalidate_disk(host->disk);
#if 0
	if (error < 0 || !mmc_card_present(&host->card)) {
		error = -ENODEV;
		goto err_xfer;
	}
#endif

	error = stsd_init_io_thread(host);
	if (error)
		goto err_xfer;

	add_disk(host->disk);

	return 0;

err_xfer:
	stsd_exit_xfer(host);
err_blk_dev:
	stsd_exit_blk_dev(host);
out:
	return error;
}

static void stsd_exit(struct stsd_host *host)
{
	del_gendisk(host->disk);
	stsd_exit_io_thread(host);
	stsd_exit_xfer(host);
	stsd_exit_blk_dev(host);
	if (host->fd >= 0)
		starlet_close(host->fd);
	host->fd = -1;

}

static void stsd_kill(struct stsd_host *host)
{
	if (host->refcnt > 0) {
		drv_printk(KERN_ERR, "hey! card removed while in use!\n");
		set_bit(__STSD_MEDIA_CHANGED, &host->flags);
	}

	stsd_exit(host);

	/* release the host immediately when not in use */
	if (!host->refcnt)
		kfree(host);
}

/*
 * Driver model helper routines.
 *
 */

static int stsd_do_probe(struct device *dev)
{
	struct stsd_host *host;
	int error;

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host) {
		drv_printk(KERN_ERR, "%s: failed to allocate stsd_host\n",
			    __func__);
		return -ENOMEM;
	}
	dev_set_drvdata(dev, host);
	host->dev = dev;

	error = stsd_init(host);
	if (error) {
		kfree(host);
		dev_set_drvdata(dev, NULL);
	}

	return error;
}

static int stsd_do_remove(struct device *dev)
{
	struct stsd_host *host = dev_get_drvdata(dev);

	if (!host)
		return -ENODEV;

	stsd_kill(host);
	dev_set_drvdata(dev, NULL);

	return 0;
}

/*
 * OF platform device routines.
 *
 */

static int stsd_of_probe(struct platform_device *odev)

{
	return stsd_do_probe(&odev->dev);
}

static int __exit stsd_of_remove(struct platform_device *odev)
{
	return stsd_do_remove(&odev->dev);
}

static struct of_device_id stsd_of_match[] = {
	{ .compatible = "nintendo,starlet-ios-sd" },
	{ },
};

MODULE_DEVICE_TABLE(of, stsd_of_match);

static struct platform_driver stsd_of_driver __refdata = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = stsd_of_match,
	},
	.probe = stsd_of_probe,
	.remove = stsd_of_remove,
};


/*
 * Kernel module interface.
 *
 */

static int __init stsd_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   stsd_driver_version);

	if (register_blkdev(STSD_MAJOR, DRV_MODULE_NAME)) {
		drv_printk(KERN_ERR, "unable to register major %d\n",
			   STSD_MAJOR);
		return -EIO;
	}

	return platform_driver_register(&stsd_of_driver);
}

static void __exit stsd_exit_module(void)
{
	platform_driver_unregister(&stsd_of_driver);
	unregister_blkdev(STSD_MAJOR, DRV_MODULE_NAME);
}

module_init(stsd_init_module);
module_exit(stsd_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

