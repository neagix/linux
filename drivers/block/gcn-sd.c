/*
 * drivers/block/gcn-sd.c
 *
 * MMC/SD card block driver for the Nintendo GameCube/Wii
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004,2005 Rob Reylink
 * Copyright (C) 2005 Todd Jeffreys
 * Copyright (C) 2005,2006,2007,2008,2009 Albert Herranz
 * Copyleft  (C) 2012, Gerrit Pannek
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 * This is a block device driver for the Nintendo SD Card Adapter (DOL-019)
 * and compatible hardware.
 * The driver has been tested with SPI-enabled MMC cards and SD cards.
 *
 * The following table shows the device major and minors needed to access
 * MMC/SD cards:
 *
 * +------+-------------+-------+-------+
 * | Slot | Target      | Major | Minor |
 * +======+=============+=======+=======+
 * | A    | disk        | 61    | 0     |
 * | A    | partition 1 | 61    | 1     |
 * | A    | partition 2 | 61    | 2     |
 * | A    | partition 3 | 61    | 3     |
 * | A    | partition 4 | 61    | 4     |
 * | A    | partition 5 | 61    | 5     |
 * | A    | partition 6 | 61    | 6     |
 * | A    | partition 7 | 61    | 7     |
 * +------+-------------+-------+-------+
 * | B    | disk        | 61    | 8     |
 * | B    | partition 1 | 61    | 9     |
 * | B    | partition 2 | 61    | 10    |
 * | B    | partition 3 | 61    | 11    |
 * | B    | partition 4 | 61    | 12    |
 * | B    | partition 5 | 61    | 13    |
 * | B    | partition 6 | 61    | 14    |
 * | B    | partition 7 | 61    | 15    |
 * +------+-------------+-------+-------+
 *
 * For example, run "mknod /dev/gcnsdb1 b 61 9" to create a device file
 * to access the 1st partition on the card inserted in memcard slot B.
 *
 */

#define SD_DEBUG

#include <linux/blkdev.h>
#include <linux/crc-ccitt.h>
#include <linux/delay.h>
#include <linux/hdreg.h>
#include <linux/init.h>	
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/slab.h>

/*
 * The existing Linux MMC layer does not support SPI operation yet.
 * Anyway, we try to recycle here some common code.
 */
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include <linux/exi.h>

#define DRV_MODULE_NAME "gcn-sd"
#define DRV_DESCRIPTION "MMC/SD/SDHC card block driver for the Nintendo GameCube/Wii"
#define DRV_AUTHOR	"Rob Reylink, " \
			"Todd Jeffreys, " \
			"Albert Herranz" \
			"Gerrit Pannek"

static char sd_driver_version[] = "4.2";

#define sd_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

#ifdef SD_DEBUG
#  define DBG(fmt, args...) \
	   printk(KERN_ERR "%s: " fmt, __func__ , ## args)
#else
#  define DBG(fmt, args...)
#endif


/*
 *
 * EXI related definitions.
 */
#define SD_SLOTA_CHANNEL	0	/* EXI0xxx */
#define SD_SLOTA_DEVICE		0	/* chip select, EXI0CSB0 */

#define SD_SLOTB_CHANNEL	1	/* EXI1xxx */
#define SD_SLOTB_DEVICE		0	/* chip select, EXI1CSB0 */

#define SD_SPI_CLK		16000000
#define SD_SPI_CLK_IDX		EXI_CLK_16MHZ


/*
 *
 * MMC/SD related definitions.
 */

/* cycles in 8 clock units */
#define SD_IDLE_CYCLES		80
#define SD_FINISH_CYCLES	8

/* several times in 8 clock units */
#define MMC_SPI_N_CR		8	/* card response time */

/* data start and stop tokens */
#define MMC_SPI_TOKEN_START_SINGLE_BLOCK_READ		0xfe
#define MMC_SPI_TOKEN_START_MULTIPLE_BLOCK_READ		0xfe
#define MMC_SPI_TOKEN_START_SINGLE_BLOCK_WRITE		0xfe
#define MMC_SPI_TOKEN_START_MULTIPLE_BLOCK_WRITE	0xfc
#define MMC_SPI_TOKEN_STOP_MULTIPLE_BLOCK_WRITE		0xfd

/* data response */
#define DR_SPI_MASK				0x1f
#define DR_SPI_DATA_ACCEPTED			0x05
#define DR_SPI_DATA_REJECTED_CRC_ERROR		0x0b
#define DR_SPI_DATA_REJECTED_WRITE_ERROR	0x0d

/* this is still a missing command in the current MMC framework ... */
#define MMC_READ_OCR		58

/*
 * OCR Bit positions to 10s of Vdd mV.
 */
static const unsigned short mmc_ocr_bit_to_vdd[] = {
	150, 155, 160, 165, 170, 180, 190, 200,
	210, 220, 230, 240, 250, 260, 270, 280,
	290, 300, 310, 320, 330, 340, 350, 360
};

static const unsigned int tran_exp[] = {
	10000, 100000, 1000000, 10000000,
	0, 0, 0, 0
};

static const unsigned char tran_mant[] = {
	0, 10, 12, 13, 15, 20, 25, 30,
	35, 40, 45, 50, 55, 60, 70, 80,
};

static const unsigned int tacc_exp[] = {
	1, 10, 100, 1000, 10000, 100000, 1000000, 10000000,
};

static const unsigned int tacc_mant[] = {
	0, 10, 12, 13, 15, 20, 25, 30,
	35, 40, 45, 50, 55, 60, 70, 80,
};

unsigned short is_sdhc = 0;

/*
 * Driver settings.
 */
#define MMC_SHIFT		3	/* 8 partitions */

#define SD_MAJOR		61
#define SD_NAME			"gcnsd"

#define KERNEL_SECTOR_SHIFT     9
#define KERNEL_SECTOR_SIZE      (1 << KERNEL_SECTOR_SHIFT)	/*512 */

enum {
	__SD_MEDIA_CHANGED = 0,
	__SD_BAD_CARD,
};


/*
 * Raw MMC/SD command.
 */
struct sd_command {
	u8 cmd;
	u32 arg;
	u8 crc;
} __attribute__ ((__packed__));	/* do not add padding, please */

/*
 * MMC/SD host.
 *
 * We have one host for each memory card slot. And a host can only drive a
 * single card each time.
 */
struct sd_host {
	spinlock_t		lock;

	int refcnt;
	unsigned long flags;
#define SD_MEDIA_CHANGED	(1<<__SD_MEDIA_CHANGED)
#define SD_BAD_CARD		(1<<__SD_BAD_CARD)

	/* card related info */
	struct mmc_card card;

	/* timeouts in 8 clock cycles */
	unsigned long read_timeout;
	unsigned long write_timeout;

	/* operations condition register */
	u32 ocr_avail;		/* just 3.3V for the GameCube */
	u32 ocr;

	/* last card response */
	u8 resp;

	/* frequency */
	unsigned int clock;
	u8 exi_clock;

	/* command buffer */
	struct sd_command cmd;

	spinlock_t 		queue_lock;
	struct request_queue	*queue;

	struct gendisk 		*disk;

	struct task_struct	*io_thread;
	struct mutex		io_mutex;

	struct exi_device	*exi_device;
};

static void sd_kill(struct sd_host *host);


/*
* This takes care of setting the global variable to check if it's
* a SDHC-Card or not
*/
static void sd_card_set_type(short value) 
{
	is_sdhc = value;
}

static int sd_card_is_sdhc(void) 
{
	return is_sdhc;
} 


static void sd_card_set_bad(struct sd_host *host)
{
	set_bit(__SD_BAD_CARD, &host->flags);
}

static int sd_card_is_bad(struct sd_host *host)
{
	return test_bit(__SD_BAD_CARD, &host->flags);
}

/*
 *
 * MMC/SD data structures manipulation.
 */

/*
 * FIXME: use a faster method (table)
 * (the in-kernel crc 16 (ccitt crc) tables seem not compatible with us)
 */
static u16 crc_xmodem_update(u16 crc, u8 data)
{
	int i;

	crc = crc ^ ((u16) data << 8);

	for (i = 0; i < 8; i++) {
		if (crc & 0x8000)
			crc = (crc << 1) ^ 0x1021;
		else
			crc <<= 1;
	}

	return crc;
}

#define UNSTUFF_BITS(resp, start, size)				\
	({							\
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

	if (mmc_card_sd(card)) {
		card->cid.manfid = UNSTUFF_BITS(resp, 120, 8);
		card->cid.oemid = UNSTUFF_BITS(resp, 104, 16);
		card->cid.prod_name[0] = UNSTUFF_BITS(resp, 96, 8);
		card->cid.prod_name[1] = UNSTUFF_BITS(resp, 88, 8);
		card->cid.prod_name[2] = UNSTUFF_BITS(resp, 80, 8);
		card->cid.prod_name[3] = UNSTUFF_BITS(resp, 72, 8);
		card->cid.prod_name[4] = UNSTUFF_BITS(resp, 64, 8);
		card->cid.hwrev = UNSTUFF_BITS(resp, 60, 4);
		card->cid.fwrev = UNSTUFF_BITS(resp, 56, 4);
		card->cid.serial = UNSTUFF_BITS(resp, 24, 32);
		card->cid.year = UNSTUFF_BITS(resp, 12, 8);
		card->cid.month = UNSTUFF_BITS(resp, 8, 4);
		card->cid.year += 2000;
	} else {
		/*
		 * The selection of the format here is guesswork based upon
		 * information people have sent to date.
		 */
		switch (card->csd.mmca_vsn) {
		case 0:	/* MMC v1.0 - v1.2 */
		case 1:	/* MMC v1.4 */
			card->cid.manfid = UNSTUFF_BITS(resp, 104, 24);
			card->cid.prod_name[0] = UNSTUFF_BITS(resp, 96, 8);
			card->cid.prod_name[1] = UNSTUFF_BITS(resp, 88, 8);
			card->cid.prod_name[2] = UNSTUFF_BITS(resp, 80, 8);
			card->cid.prod_name[3] = UNSTUFF_BITS(resp, 72, 8);
			card->cid.prod_name[4] = UNSTUFF_BITS(resp, 64, 8);
			card->cid.prod_name[5] = UNSTUFF_BITS(resp, 56, 8);
			card->cid.prod_name[6] = UNSTUFF_BITS(resp, 48, 8);
			card->cid.hwrev = UNSTUFF_BITS(resp, 44, 4);
			card->cid.fwrev = UNSTUFF_BITS(resp, 40, 4);
			card->cid.serial = UNSTUFF_BITS(resp, 16, 24);
			card->cid.month = UNSTUFF_BITS(resp, 12, 4);
			card->cid.year = UNSTUFF_BITS(resp, 8, 4);
			card->cid.year += 1997;
			break;
		case 2:	/* MMC v2.0 - v2.2 */
		case 3:	/* MMC v3.1 - v3.3 */
			card->cid.manfid = UNSTUFF_BITS(resp, 120, 8);
			card->cid.oemid = UNSTUFF_BITS(resp, 104, 16);
			card->cid.prod_name[0] = UNSTUFF_BITS(resp, 96, 8);
			card->cid.prod_name[1] = UNSTUFF_BITS(resp, 88, 8);
			card->cid.prod_name[2] = UNSTUFF_BITS(resp, 80, 8);
			card->cid.prod_name[3] = UNSTUFF_BITS(resp, 72, 8);
			card->cid.prod_name[4] = UNSTUFF_BITS(resp, 64, 8);
			card->cid.prod_name[5] = UNSTUFF_BITS(resp, 56, 8);
			card->cid.serial = UNSTUFF_BITS(resp, 16, 32);
			card->cid.month = UNSTUFF_BITS(resp, 12, 4);
			card->cid.year = UNSTUFF_BITS(resp, 8, 4);
			card->cid.year += 1997;
			break;
		default:
			sd_printk(KERN_ERR, "card has unknown MMCA"
				  " version %d\n", card->csd.mmca_vsn);
			break;
		}
	}
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static void mmc_decode_csd(struct mmc_card *card)
{
	struct mmc_csd *csd = &card->csd;
	unsigned int e, m, csd_struct;
	u32 *resp = card->raw_csd;

	/*
	 * We only understand CSD structure v1.0, v1.1 and v2.
	 * v2 has extra information in bits 15, 11 and 10.
	 */
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

		sd_card_set_type(1);
		break;
	default:
		printk("%s: unrecognised CSD structure version %d\n",
			mmc_hostname(card->host), csd_struct);
		return;
	}
}

#if 0
static void sd_print_cid(struct mmc_cid *cid)
{
	sd_printk(KERN_INFO,
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
#endif

/* */
static inline unsigned int ms_to_cycles(unsigned int ms, unsigned int clock)
{
	return ms * (clock / 1000);
}

/* */
static unsigned int sd_set_clock(struct sd_host *host, unsigned int clock)
{
	if (clock >= 32000000) {
		host->clock = 32000000;
		host->exi_clock = EXI_CLK_32MHZ;
	} else if (clock >= 16000000) {
		host->clock = 16000000;
		host->exi_clock = EXI_CLK_16MHZ;
	} else if (clock >= 8000000) {
		host->clock = 8000000;
		host->exi_clock = EXI_CLK_8MHZ;
	} else if (clock >= 4000000) {
		host->clock = 4000000;
		host->exi_clock = EXI_CLK_4MHZ;
	} else if (clock >= 2000000) {
		host->clock = 2000000;
		host->exi_clock = EXI_CLK_2MHZ;
	} else {
		host->clock = 1000000;
		host->exi_clock = EXI_CLK_1MHZ;
	}
	return host->clock;
}

/* */
static void sd_calc_timeouts(struct sd_host *host)
{
	/*
	 * FIXME: calculate timeouts from card information
	 * (use safe defaults for now)
	 */
	host->read_timeout = ms_to_cycles(100, host->clock);
	host->write_timeout = ms_to_cycles(250, host->clock);
}

/*
 *
 * SPI I/O support routines, including some handy SPI to EXI language
 * translations.
 */

/* */
static inline void spi_cs_low(struct sd_host *host)
{
	exi_dev_take(host->exi_device);
	exi_dev_select(host->exi_device);
}

/* */
static inline void spi_cs_high(struct sd_host *host)
{
	exi_dev_deselect(host->exi_device);
	exi_dev_give(host->exi_device);
}

/* */
static inline void spi_write(struct sd_host *host, void *data, size_t len)
{
	exi_dev_write(host->exi_device, data, len);
}

/* */
static inline void spi_read(struct sd_host *host, void *data, size_t len)
{
	/*
	 * Houston, we have a problem.
	 *
	 * The EXI hardware implementation seems to use a shift register which
	 * outputs data from the MSB to the MOSI line and inputs data from
	 * the MISO line into the LSB.
	 * When a read operation is performed, data from the MISO line
	 * is entered into the shift register LSB as expected. But also the
	 * data already present in the shift register is sent out through the
	 * MOSI line from the MSB.
	 * This is in fact the "feature" that enabled tmbinc to dump the IPL.
	 *
	 * When interfacing with SD cards, this causes us a serious problem.
	 *
	 * We are required to send all ones (1s) while reading data from
	 * the SD card. Otherwise, the card can interpret the data sent as
	 * commands (if they start with the bit pattern 01 for example).
	 *
	 * If we use the EXI immediate mode transfer, we can workaround the
	 * situation by writing all 1s to the DATA register before reading
	 * (this is indeed automatically done by the EXI layer).
	 * But we simply can't do that when using EXI DMA transfers (these
	 * kind of transfers do not allow bidirectional operation).
	 *
	 * Given that no EXI DMA read transfers seem reliable, we fallback
	 * to the "interrupt-driven" immediate mode of the EXI layer.
	 * This will help reducing CPU monopolization on large reads.
	 *
	 */
	exi_dev_transfer(host->exi_device, data, len, EXI_OP_READ, EXI_CMD_IDI);
}

/* cycles are expressed in 8 clock cycles */
static void spi_burn_cycles(struct sd_host *host, int cycles)
{
	u8 d;

	while (cycles-- > 0) {
		d = 0xff;
		spi_write(host, &d, sizeof(d));
	}
}

/* cycles are expressed in 8 clock cycles */
static int spi_wait_for_resp(struct sd_host *host,
			     u8 resp, u8 resp_mask, unsigned long cycles)
{
	u8 data;

	while (cycles-- > 0) {
		spi_read(host, &data, sizeof(data));
		if ((data & resp_mask) == resp) {
			host->resp = data;
			return data;
		}
	}
	return -ENODATA;
}

/*
 *
 */
static int sd_read_data(struct sd_host *host, void *data, size_t len, int token)
{
	int retval = 0;

	if (token) {
		retval = spi_wait_for_resp(host, token, 0xff,
					   host->read_timeout);
		if (retval < 0)
			goto out;
	}
	spi_read(host, data, len);
	retval = 0;

out:
	return retval;
}

/*
 *
 */
static int sd_write_data(struct sd_host *host, void *data, size_t len,
			  int token)
{
	u16 crc;
	u8 t, *d;
	size_t l;
	int retval = 0;

	/* FIXME, rewrite this a bit */
	{
		crc = 0;
		d = data;
		l = len;

		while (l-- > 0)
			crc = crc_xmodem_update(crc, *d++);
	}

	/* send the write block token */
	t = token;
	spi_write(host, &t, sizeof(t));

	/* send the data */
	spi_write(host, data, len);

	/* send the crc */
	spi_write(host, &crc, sizeof(crc));

	/* get the card data response */
	retval = spi_wait_for_resp(host, 0x01, 0x11, host->write_timeout);
	if (retval < 0)
		goto out;
	if ((retval & DR_SPI_MASK) != DR_SPI_DATA_ACCEPTED) {
		DBG("data response=%02x\n", retval);
		retval = -EIO;
		goto out;
	}

	/* wait for the busy signal to clear */
	retval = spi_wait_for_resp(host, 0xff, 0xff, host->write_timeout);
	if (retval < 0)
		goto out;

	retval = 0;

out:
	return retval;
}

/*
 *
 * MMC/SD command transactions related routines.
 */

/* */
static inline void sd_cmd(struct sd_command *cmd, u8 opcode, u32 arg)
{
	cmd->cmd = 0x40 | opcode;
	cmd->arg = arg;
	cmd->crc = 0x01;	/* FIXME, crc is not currently used */
}

static inline void sd_cmd_crc(struct sd_command *cmd, u8 opcode, u32 arg, u8 crc)
{
	cmd->cmd = 0x40 + opcode;
	cmd->arg = arg;
	cmd->crc = crc;	
}

/* */
static inline void sd_cmd_go_idle_state(struct sd_command *cmd)
{
	cmd->cmd = 0x40;
	cmd->arg = 0;
	cmd->crc = 0x95;
}

/* */
static inline void sd_debug_print_cmd(struct sd_command *cmd)
{
	DBG("cmd = %d, arg = %08x, crc = %02x\n",
	    cmd->cmd & ~0x40, cmd->arg, cmd->crc);
}

/*
 *
 */
static int sd_start_command(struct sd_host *host, struct sd_command *cmd)
{
	int retval = 0;

	/* select the card by driving CS low */
	spi_cs_low(host);

	/* send the command through the MOSI line */
	spi_write(host, cmd, sizeof(*cmd));

	/*
	 * Wait for the card response.
	 * Card responses come in the MISO line and have the most significant
	 * bit cleared.
	 */
	retval = spi_wait_for_resp(host, 0x00, 0x80, MMC_SPI_N_CR);

	if (retval > 0 && !(retval & 0x01) && cmd->cmd != 0x40)
		DBG("command = %d, response = 0x%02x\n", cmd->cmd & ~0x40,
		    retval);

	return retval;
}

/*
 *
 */
static void sd_end_command(struct sd_host *host)
{
	/* wait 8 clock cycles as dictated by the specification */
	spi_burn_cycles(host, SD_FINISH_CYCLES);

	/* deselect the card by driving CS high */
	spi_cs_high(host);
}

/*
 *
 */
static int sd_run_no_data_command(struct sd_host *host, struct sd_command *cmd)
{
	int retval;

	/* send command, wait for response, and burn extra cycles */
	retval = sd_start_command(host, cmd);
	sd_end_command(host);

	return retval;
}

/*
 *
 */
static int sd_generic_read(struct sd_host *host,
			   u8 opcode, u32 arg,
			   void *data, size_t len, int token)
{
	struct sd_command *cmd = &host->cmd;
	u16 crc, calc_crc = 0xffff;
	u8 *d;
	size_t l;
	int retval;

	/* build raw command */
	sd_cmd(cmd, opcode, arg);

	/* select card, send command and wait for response */
	retval = sd_start_command(host, cmd);
	if (retval < 0)
		goto out;
	if (retval != 0x00) {
		retval = -EIO;
		goto out;
	}

	/* wait for read token, then read data */
	retval = sd_read_data(host, data, len, token);
	if (retval < 0)
		goto out;	

	/* read trailing crc */
	spi_read(host, &crc, sizeof(crc));

	retval = 0;

	/* FIXME, rewrite this a bit */
	{
		calc_crc = 0;
		d = data;
		l = len;

		while (l-- > 0)
			calc_crc = crc_xmodem_update(calc_crc, *d++);

		if (calc_crc != crc)
			retval = -EIO;
	}

out:
	/* burn extra cycles and deselect card */
	sd_end_command(host);

	if (retval < 0) {
		DBG("read, offset=%u, len=%d\n", arg, len);
		DBG("crc=%04x, calc_crc=%04x, %s\n", crc, calc_crc,
		    (retval < 0) ? "failed" : "ok");
	}

	return retval;
}

/*
 *
 */
static int sd_generic_write(struct sd_host *host,
			    u8 opcode, u32 arg,
			    void *data, size_t len, int token)
{
	struct sd_command *cmd = &host->cmd;
	int retval;

	/* build raw command */
	sd_cmd(cmd, opcode, arg);

	/* select card, send command and wait for response */
	retval = sd_start_command(host, cmd);
	if (retval < 0)
		goto out;
	if (retval != 0x00) {
		retval = -EIO;
		goto out;
	}

	/* send data token, data and crc, get data response */
	retval = sd_write_data(host, data, len, token);
	if (retval < 0)
		goto out;

	retval = 0;

out:
	/* burn extra cycles and deselect card */
	sd_end_command(host);

	if (retval < 0)
		DBG("write, offset=%d, len=%d\n", arg, len);

	return retval;
}

/*
 *
 */
static int sd_read_ocr(struct sd_host *host)
{
	struct sd_command *cmd = &host->cmd;
	int retval;

	memset(&host->ocr, 0, sizeof(host->ocr));

	sd_cmd(cmd, MMC_READ_OCR, 0);

	/* select card, send command and wait for response */
	retval = sd_start_command(host, cmd);
	if (retval < 0)
		goto out;

	/* the OCR contents come immediately after the card response */
	spi_read(host, &host->ocr, sizeof(host->ocr));
	retval = 0;

out:
	/* burn extra cycles and deselect card */
	sd_end_command(host);
	return retval;
}

/*
 *
 */
static inline int sd_read_csd(struct sd_host *host)
{
	memset(&host->card.raw_csd, 0, sizeof(host->card.raw_csd));
	return sd_generic_read(host, MMC_SEND_CSD, 0,
			       &host->card.raw_csd,
			       sizeof(host->card.raw_csd),
			       MMC_SPI_TOKEN_START_SINGLE_BLOCK_READ);
}

/*
 *
 */
static inline int sd_read_cid(struct sd_host *host)
{
	memset(&host->card.raw_cid, 0, sizeof(host->card.raw_cid));
	return sd_generic_read(host, MMC_SEND_CID, 0,
			       &host->card.raw_cid,
			       sizeof(host->card.raw_cid),
			       MMC_SPI_TOKEN_START_SINGLE_BLOCK_READ);
}

/*
 *
 */
static inline int sd_read_single_block(struct sd_host *host,
				       unsigned long start,
				       void *data, size_t len)
{
	int retval;
	int attempts = 3;

	if (test_bit(__SD_MEDIA_CHANGED, &host->flags))
		attempts = 1;

	while (attempts > 0) {
		retval = sd_generic_read(host, MMC_READ_SINGLE_BLOCK, start,
					 data, len,
					 MMC_SPI_TOKEN_START_SINGLE_BLOCK_READ);
		if (retval >= 0)
			break;
		attempts--;
		DBG("start=%lu, data=%p, len=%d, retval = %d\n", start, data,
		    len, retval);
	}
	return retval;
}

/*
 *
 */
static inline int sd_write_single_block(struct sd_host *host,
					 unsigned long start,
					 void *data, size_t len)
{
	int retval;

	retval = sd_generic_write(host, MMC_WRITE_BLOCK, start,
				  data, len,
				  MMC_SPI_TOKEN_START_SINGLE_BLOCK_WRITE);
	if (retval < 0)
		DBG("start=%lu, data=%p, len=%d, retval = %d\n", start, data,
		    len, retval);

	return retval;
}

/*
 *
 */
static int sd_reset_sequence(struct sd_host *host)
{
	struct sd_command *cmd = &host->cmd;
	u8 d;
	int i;
	int retval = 0;
	u32 cmd_ret = 0;

	host->card.state = 0;
	


	/*
	 * Wait at least 80 dummy clock cycles with the card deselected
	 * and with the MOSI line continuously high.
	 */
	exi_dev_take(host->exi_device);
	exi_dev_deselect(host->exi_device);
	for (i = 0; i < SD_IDLE_CYCLES; i++) {
		d = 0xff;
		exi_dev_write(host->exi_device, &d, sizeof(d));
	}
	exi_dev_give(host->exi_device);

	/*
	 * Send a CMD0, card must ack with "idle state" (0x01).
	 * This puts the card into SPI mode and soft resets it.
	 * CRC checking is disabled by default.
	 */
	for (i = 0; i < 255; i++) {
		/* CMD0 */
		sd_cmd_go_idle_state(cmd);
		retval = sd_run_no_data_command(host, cmd);
		if (retval < 0) {
			retval = -ENODEV;
			goto out;
		}
		if (retval == R1_SPI_IDLE) {
			break;
		}
	}
	if (retval != R1_SPI_IDLE) {
		retval = -ENODEV;
		goto out;
	}

	/*
	* Send CMD8 and CMD58 for SDHC support
	*/
	for (i = 0; i < 8; i++) {
		sd_cmd_crc(cmd, SD_SEND_IF_COND, 0x01AA, 0x87); //CMD8 + Check and CRC
		retval = sd_start_command(host, cmd); 
		if(retval==0x01) { 	
			memset(&cmd_ret, 0, sizeof(cmd_ret));
			spi_read(host, &cmd_ret, sizeof(cmd_ret));
			sd_end_command(host); 	
			if (cmd_ret == 0x01AA) { //Check if CMD8 is alright
				sd_cmd(cmd, MMC_SPI_READ_OCR, 0);//CMD58
				retval = sd_start_command(host, cmd);	

				memset(&cmd_ret, 0, sizeof(cmd_ret));
				spi_read(host, &cmd_ret, sizeof(cmd_ret));
				sd_end_command(host);		

				if(retval==0x01) { //Everything is alright
					break;
				} 
			} 
			break;
		} 		
		else if(retval==0x05) { 
			//NO SDHC-Card
			break;
		} 
	}	
	
	/*
	 * Send a ACMD41 to activate card initialization process.
	 * SD card must ack with "ok" (0x00).
	 * MMC card will report "invalid command" (0x04).
	 */
	for (i = 0; i < 65535; i++) {
		/* ACMD41 = CMD55 + CMD41 */
		sd_cmd(cmd, MMC_APP_CMD, 0);
		retval = sd_run_no_data_command(host, cmd);
		if (retval < 0) {
			retval = -ENODEV;
			goto out;
		}

		sd_cmd(cmd, SD_APP_OP_COND, (1<<30)|0x100000);
		retval = sd_run_no_data_command(host, cmd);
		if (retval < 0) {
			retval = -ENODEV;
			goto out;
		}
		if (retval == 0x00) {
			/* we found a SD card */
			mmc_card_set_present(&host->card);
			host->card.type = MMC_TYPE_SD;
			break;
		} else if(retval != 0x01) {
			DBG("ACMD41 return: %d\n", retval);
		}

		if ((retval & R1_SPI_ILLEGAL_COMMAND)) {
			/* this looks like a MMC card */
			break;
		}
	}

	/*
	 * MMC cards require CMD1 to activate card initialization process.
	 * MMC card must ack with "ok" (0x00)
	 */
	if (!mmc_card_sd(&host->card)) {
		for (i = 0; i < 65535; i++) {
			sd_cmd(cmd, MMC_SEND_OP_COND, 0);
			retval = sd_run_no_data_command(host, cmd);
			if (retval < 0) {
				retval = -ENODEV;
				goto out;
			}
			if (retval == 0x00) {
				/* we found a MMC card */
				mmc_card_set_present(&host->card);
				break;
			}
		}
		if (retval != 0x00) {
			DBG("MMC card, bad, retval=%02x\n", retval);
			sd_card_set_bad(host);
		}
	}

out:
	return retval;
}

/*
 *
 */
static int sd_welcome_card(struct sd_host *host)
{
	int retval;

	/* soft reset the card */
	retval = sd_reset_sequence(host);
	if (retval < 0 || sd_card_is_bad(host))
		goto out;

	/* read Operating Conditions Register */
	retval = sd_read_ocr(host);
	if (retval < 0)
		goto err_bad_card;

	/* refuse to drive cards reporting voltage ranges out of scope */
	if (!(host->ocr & host->ocr_avail)) {
		sd_printk(KERN_WARNING, "reported OCR (%08x)"
			  " indicates that it is not safe to use this"
			  " card with a GameCube\n", host->ocr);
		retval = -ENODEV;
		goto err_bad_card;
	}

	/* read and decode the Card Specific Data */
	retval = sd_read_csd(host);
	if (retval < 0)
		goto err_bad_card;
	mmc_decode_csd(&host->card);

	/* calculate some card access related timeouts */
	sd_calc_timeouts(host);

	/* read and decode the Card Identification Data */
	retval = sd_read_cid(host);
	if (retval < 0)
		goto err_bad_card;
	mmc_decode_cid(&host->card);

	sd_printk(KERN_INFO, "slot%d: descr \"%s\", size %lublk, block %ub,"
		  " serial %08x\n",
		  to_channel(exi_get_exi_channel(host->exi_device)),
		  host->card.cid.prod_name,
		  (unsigned long)((host->card.csd.capacity)),
		  1 << host->card.csd.read_blkbits,
		  host->card.cid.serial);

	retval = 0;
	goto out;

err_bad_card:
	sd_card_set_bad(host);
out:
	return retval;
}

/*
 * Block layer.
 */

/*
 * Performs a read request for SD.
 */
static int sd_read_request(struct sd_host *host, struct request *req)
{
	int i;
	unsigned long nr_blocks; /* in card blocks */
	size_t block_len; /* in bytes */
	unsigned long start;
	void *buf = req->buffer;
	int retval;

	/*
	 * It seems that some cards do not accept single block reads for the
	 * read block length reported by the card.
	 * For now, we perform only 512 byte single block reads.
	 */

	start = blk_rq_pos(req) << KERNEL_SECTOR_SHIFT;

	nr_blocks = blk_rq_cur_sectors(req);
	block_len = 1 << KERNEL_SECTOR_SHIFT;

	for (i = 0; i < nr_blocks; i++) {
		retval = sd_read_single_block(host, start, buf, block_len);
		if (retval < 0)
			break;

		start += block_len;
		buf += block_len;
	}

	/* number of kernel sectors transferred */
#if 0
	retval = i << (host->card.csd.read_blkbits - KERNEL_SECTOR_SHIFT);
#else
	retval = i;
#endif

	return retval;
}

/*
 * Performs a read request for SDHC.
 */
static int sdhc_read_request(struct sd_host *host, struct request *req)
{
	int i;
	unsigned long nr_blocks; /* in card blocks */
	size_t block_len; /* in bytes */
	unsigned long start;
	void *buf = req->buffer;
	int retval;

	start = blk_rq_pos(req);

	nr_blocks = blk_rq_cur_sectors(req);
	block_len = 1 << KERNEL_SECTOR_SHIFT;

	for (i = 0; i < nr_blocks; i++) {
		retval = sd_read_single_block(host, start, buf, block_len);
		if (retval < 0)
			break;

		start ++;
		buf += block_len;
	}

	/* number of kernel sectors transferred */
	retval = i;

	return retval;
}

/*
 * Performs a write request for SD.
 */
static int sd_write_request(struct sd_host *host, struct request *req)
{
	int i;
	unsigned long nr_blocks; /* in card blocks */
	size_t block_len; /* in bytes */
	unsigned long start;
	void *buf = req->buffer;
	int retval;

	/* FIXME?, maybe should use 2^WRITE_BL_LEN blocks */

	/* kernel sectors and card write blocks are both 512 bytes long */
	start = blk_rq_pos(req) << KERNEL_SECTOR_SHIFT;
	nr_blocks = blk_rq_cur_sectors(req);
	block_len = 1 << KERNEL_SECTOR_SHIFT;

	for (i = 0; i < nr_blocks; i++) {
		retval = sd_write_single_block(host, start, buf, block_len);
		if (retval < 0)
			break;

		start += block_len;
		buf += block_len;
	}

	/* number of kernel sectors transferred */
	retval = i;

	return retval;
}

/*
 * Performs a write request for SDHC.
 */
static int sdhc_write_request(struct sd_host *host, struct request *req)
{
	int i;
	unsigned long nr_blocks; /* in card blocks */
	size_t block_len; /* in bytes */
	unsigned long start;
	void *buf = req->buffer;
	int retval;

	/* FIXME?, maybe should use 2^WRITE_BL_LEN blocks */

	/* kernel sectors and card write blocks are both 512 bytes long */
	start = blk_rq_pos(req);
	nr_blocks = blk_rq_cur_sectors(req);
	block_len = 1 << KERNEL_SECTOR_SHIFT;

	for (i = 0; i < nr_blocks; i++) {
		retval = sd_write_single_block(host, start, buf, block_len);
		if (retval < 0)
			break;

		start ++;
		buf += block_len;
	}

	/* number of kernel sectors transferred */
	retval = i;

	return retval;
}

/*
 * Verifies if a request should be dispatched or not.
 *
 * Returns:
 *  <0 in case of error.
 *  0  if request passes the checks
 */
static int sd_check_request(struct sd_host *host, struct request *req)
{
	unsigned long nr_sectors;

	if (req->cmd_type != REQ_TYPE_FS)
		return -EIO;

	if (test_bit(__SD_MEDIA_CHANGED, &host->flags)) {
		sd_printk(KERN_ERR, "media changed, aborting\n");
		return -ENOMEDIUM;
	}

	/* unit is kernel sectors */
	nr_sectors =
	    host->card.csd.capacity << (host->card.csd.read_blkbits - KERNEL_SECTOR_SHIFT);

	/* keep our reads within limits */

	if ((blk_rq_pos(req) + blk_rq_cur_sectors(req)) > nr_sectors) {
		sd_printk(KERN_ERR, "reading past end, aborting\n");
		return -EINVAL;
	} 

	return 0;
}

/*
 * Request dispatcher.
 */
static int sd_do_request(struct sd_host *host, struct request *req)
{
	int nr_sectors = 0;
	int error;

	error = sd_check_request(host, req);
	if (error) {
		nr_sectors = error;
		goto out;
	}

	
	//I added the 2 different read/write functions, so we just need one if-else and should perform much better ^^
	switch (rq_data_dir(req)) {
	case WRITE:
		if(sd_card_is_sdhc()==0) 		
			nr_sectors = sd_write_request(host, req);
		else
			nr_sectors = sdhc_write_request(host, req);
		break;
	case READ:
		if(sd_card_is_sdhc()==0) 		
			nr_sectors = sd_read_request(host, req);
		else
			nr_sectors = sdhc_read_request(host, req);	
		break;
	}

out:
	return nr_sectors;
}

/*
 * Input/Output thread.
 */
static int sd_io_thread(void *param)
{
	struct sd_host *host = param;
	struct request *req;
	unsigned long flags;
	int nr_sectors;
	int error;

#if 0
	/*
	 * We are going to perfom badly due to the read problem explained
	 * above. At least, be nice with other processes trying to use the
	 * cpu.
	 */
	set_user_nice(current, 0);
#endif

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
		nr_sectors = sd_do_request(host, req);
		error = (nr_sectors < 0) ? nr_sectors : 0;

		spin_lock_irqsave(&host->queue_lock, flags);
		__blk_end_request(req, error, nr_sectors << 9);
		spin_unlock_irqrestore(&host->queue_lock, flags);
	}
	mutex_unlock(&host->io_mutex);

	return 0;
}

/*
 * Block layer request function.
 * Wakes up the IO thread.
 */
static void sd_request_func(struct request_queue *q)
{
	struct sd_host *host = q->queuedata;
	wake_up_process(host->io_thread);
}

/*
 *
 * Driver interface.
 */

static DEFINE_SEMAPHORE(open_lock);

/*
 * Opens the drive device.
 */
static int sd_open(struct block_device *bdev, fmode_t mode)
{
	struct sd_host *host = bdev->bd_disk->private_data;
	int retval = 0;

	if (!host || !host->exi_device)
		return -ENXIO;

	/* honor exclusive open mode */
	if (host->refcnt == -1 ||
	    (host->refcnt && (mode & FMODE_EXCL))) {
		retval = -EBUSY;
		goto out;
	}

	/* this takes care of revalidating the media if needed */
	check_disk_change(bdev);
	if (!host->card.csd.capacity) {
		retval = -ENOMEDIUM;
		goto out;
	}

	down(&open_lock);

	if ((mode & FMODE_EXCL))
		host->refcnt = -1;
	else
		host->refcnt++;

	up(&open_lock);

out:
	return retval;

}

/*
 * Releases the drive device.
 */
static void sd_release(struct gendisk *disk, fmode_t mode)
{
	struct sd_host *host = disk->private_data;

	//if (!host)
	//	return -ENXIO;

	down(&open_lock);

	if (host->refcnt > 0)
		host->refcnt--;
	else
		host->refcnt = 0;

	up(&open_lock);

	/* lazy removal of unreferenced zombies */
	if (!host->refcnt && !host->exi_device)
		kfree(host);
}

/*
 * Checks if media changed.
 */
static int sd_media_changed(struct gendisk *disk)
{
	struct sd_host *host = disk->private_data;
	unsigned int last_serial;
	int retval;

	/* report a media change for zombies */
	if (!host)
		return 1;

	/* report a media change if someone forced it */
	if (test_bit(__SD_MEDIA_CHANGED, &host->flags))
		return 1;

	/* check if the serial number of the card changed */
	last_serial = host->card.cid.serial;
	retval = sd_read_cid(host);
	if (!retval && last_serial == host->card.cid.serial && last_serial)
		clear_bit(__SD_MEDIA_CHANGED, &host->flags);
	else
		set_bit(__SD_MEDIA_CHANGED, &host->flags);

	return (host->flags & SD_MEDIA_CHANGED) ? 1 : 0;
}

/*
 * Checks if media is still valid.
 */
static int sd_revalidate_disk(struct gendisk *disk)
{
	struct sd_host *host = disk->private_data;
	int retval = 0;

	/* report missing medium for zombies */
	if (!host) {
		retval = -ENOMEDIUM;
		goto out;
	}

	/* the block layer likes to call us multiple times... */
	if (!sd_media_changed(host->disk))
		goto out;

	/* get the card into a known status */
	retval = sd_welcome_card(host);
	if (retval < 0 || sd_card_is_bad(host)) {
		retval = -ENOMEDIUM;
		goto out;
	}

	/* inform the block layer about various sizes */
	blk_queue_logical_block_size(host->queue, 1 << KERNEL_SECTOR_SHIFT);
	set_capacity(host->disk, host->card.csd.capacity /*<< (host->card.csd.read_blkbits - KERNEL_SECTOR_SHIFT)*/);

	clear_bit(__SD_MEDIA_CHANGED, &host->flags);

out:
	return retval;
}

static int sd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	geo->cylinders = get_capacity(bdev->bd_disk) / (4 * 16);
	geo->heads = 4;
	geo->sectors = 16;
	return 0;
}


static const struct block_device_operations sd_fops = {
	.owner = THIS_MODULE,
	.open = sd_open,
	.release = sd_release,
	.revalidate_disk = sd_revalidate_disk,
	.media_changed = sd_media_changed,
	.getgeo = sd_getgeo,
};

/*
 * Initializes the block layer interfaces.
 */
static int sd_init_blk_dev(struct sd_host *host)
{
	struct gendisk *disk;
	struct request_queue *queue;
	int channel;
	int retval;

	channel = to_channel(exi_get_exi_channel(host->exi_device));

	/* queue */
	retval = -ENOMEM;
	spin_lock_init(&host->queue_lock);
	queue = blk_init_queue(sd_request_func, &host->queue_lock);
	if (!queue) {
		sd_printk(KERN_ERR, "error initializing queue\n");
		goto err_blk_init_queue;
	}
	blk_queue_dma_alignment(queue, EXI_DMA_ALIGN);
	blk_queue_max_segments(queue, 1);
	blk_queue_max_hw_sectors(queue, 8);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, queue);
	queue->queuedata = host;
	host->queue = queue;

	/* disk */
	disk = alloc_disk(1 << MMC_SHIFT);
	if (!disk) {
		sd_printk(KERN_ERR, "error allocating disk\n");
		goto err_alloc_disk;
	}
	disk->major = SD_MAJOR;
	disk->first_minor = channel << MMC_SHIFT;
	disk->fops = &sd_fops;
	sprintf(disk->disk_name, "%s%c", SD_NAME, 'a' + channel);
	disk->private_data = host;
	disk->queue = host->queue;
	host->disk = disk;

	retval = 0;
	goto out;

err_alloc_disk:
	blk_cleanup_queue(host->queue);
	host->queue = NULL;
err_blk_init_queue:
out:
	return retval;
}

/*
 * Exits the block layer interfaces.
 */
static void sd_exit_blk_dev(struct sd_host *host)
{
	blk_cleanup_queue(host->queue);
	put_disk(host->disk);
}


/*
 * Initializes and launches the IO thread.
 */
static int sd_init_io_thread(struct sd_host *host)
{
	int channel;
	int result = 0;

	channel = to_channel(exi_get_exi_channel(host->exi_device));

	mutex_init(&host->io_mutex);
	host->io_thread = kthread_run(sd_io_thread, host,
				      "ksdiod/%c", 'a' + channel);
	if (IS_ERR(host->io_thread)) {
		sd_printk(KERN_ERR, "error creating io thread\n");
		result = PTR_ERR(host->io_thread);
	}
	return result;
}

/*
 * Terminates and waits for the IO thread to complete.
 */
static void sd_exit_io_thread(struct sd_host *host)
{
	if (!IS_ERR(host->io_thread)) {
		wake_up_process(host->io_thread);
		kthread_stop(host->io_thread);
		host->io_thread = ERR_PTR(-EINVAL);
	}
}

/*
 * Initializes a host.
 */
static int sd_init(struct sd_host *host)
{
	int retval;

	spin_lock_init(&host->lock);

	host->refcnt = 0;
	set_bit(__SD_MEDIA_CHANGED, &host->flags);

	host->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	sd_set_clock(host, SD_SPI_CLK);
	sd_calc_timeouts(host);

	retval = sd_init_blk_dev(host);
	if (!retval) {
		retval = sd_revalidate_disk(host->disk);
		if (retval < 0 || !mmc_card_present(&host->card)) {
			retval = -ENODEV;
			goto err_blk_dev;
		}

		retval = sd_init_io_thread(host);
		if (retval)
			goto err_blk_dev;

		add_disk(host->disk);
	}

	return retval;

err_blk_dev:
	sd_exit_blk_dev(host);
	return retval;
}

/*
 * Deinitializes (exits) a host.
 */
static void sd_exit(struct sd_host *host)
{
	del_gendisk(host->disk);
	sd_exit_io_thread(host);
	sd_exit_blk_dev(host);
}

/*
 * Terminates a host.
 */
static void sd_kill(struct sd_host *host)
{
	if (host->refcnt > 0) {
		sd_printk(KERN_ERR, "hey! card removed while in use!\n");
		set_bit(__SD_MEDIA_CHANGED, &host->flags);
	}

	sd_exit(host);
	host->exi_device = NULL;

	/* release the host immediately when not in use */
	if (!host->refcnt)
		kfree(host);
}


/*
 * EXI layer interface.
 *
 */

/*
 * Checks if the given EXI device is a MMC/SD card and makes it available
 * if true.
 */
static int sd_probe(struct exi_device *exi_device)
{
	struct sd_host *host;
	int retval;

	/* don't try to drive a device which already has a real identifier */
	if (exi_device->eid.id != EXI_ID_NONE)
		return -ENODEV;

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	host->exi_device = exi_device_get(exi_device);
	WARN_ON(exi_get_drvdata(exi_device));
	exi_set_drvdata(exi_device, host);
	retval = sd_init(host);
	if (retval) {
		exi_set_drvdata(exi_device, NULL);
		host->exi_device = NULL;
		kfree(host);
		exi_device_put(exi_device);
	}
	return retval;
}

/*
 * Makes unavailable the MMC/SD card identified by the EXI device `exi_device'.
 */
static void sd_remove(struct exi_device *exi_device)
{
	struct sd_host *host = exi_get_drvdata(exi_device);

	WARN_ON(!host);
	WARN_ON(!host->exi_device);

	exi_set_drvdata(exi_device, NULL);
	if (host)
		sd_kill(host);
	exi_device_put(exi_device);
}

static struct exi_device_id sd_eid_table[] = {
	[0] = {
	       .channel = SD_SLOTA_CHANNEL,
	       .device = SD_SLOTA_DEVICE,
	       .id = EXI_ID_NONE,
	       },
	[1] = {
	       .channel = SD_SLOTB_CHANNEL,
	       .device = SD_SLOTB_DEVICE,
	       .id = EXI_ID_NONE,
	       },
	{.id = 0}
};

static struct exi_driver sd_driver = {
	.driver		= {
		.name = DRV_MODULE_NAME,
	},
	.eid_table = sd_eid_table,
	.frequency = SD_SPI_CLK_IDX,
	.probe = sd_probe,
	.remove = sd_remove,
};


/*
 * Kernel module interface.
 *
 */

/*
 *
 */
static int __init sd_init_module(void)
{
	int retval = 0;

	sd_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		  sd_driver_version);

	if (register_blkdev(SD_MAJOR, DRV_MODULE_NAME)) {
		sd_printk(KERN_ERR, "unable to register major %d\n", SD_MAJOR);
		retval = -EIO;
		goto out;
	}

	retval = exi_driver_register(&sd_driver);

out:
	return retval;
}

/*
 *
 */
static void __exit sd_exit_module(void)
{
	unregister_blkdev(SD_MAJOR, DRV_MODULE_NAME);
	exi_driver_unregister(&sd_driver);
}

module_init(sd_init_module);
module_exit(sd_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

