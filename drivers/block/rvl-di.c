/*
 * drivers/block/rvl-di.c
 *
 * Nintendo Wii Disk Interface (DI) driver
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * Based on Nintendo GameCube Disk Interface (DI) driver
 * Copyright (C) 2005-2009 The GameCube Linux Team
 * Copyright (C) 2005,2006,2007,2009 Albert Herranz
 *
 * Portions based on previous work by Scream|CT.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */
#define DRV_MODULE_NAME "rvl-di"
#define DRV_DESCRIPTION	"Nintendo Wii Disk Interface (DI) driver"
#define DRV_AUTHOR	"Albert Herranz"

#define pr_fmt(fmt) DRV_MODULE_NAME ": " fmt

#define DEBUG

#include <linux/blkdev.h>
#include <linux/cdrom.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fcntl.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timer.h>
#include <linux/io.h>

#include <asm/starlet.h>
#include <asm/dma-mapping.h>


static char di_driver_version[] = "0.1i";

/*
 * Hardware.
 */
#define DI_DMA_ALIGN		0x1f /* 32 bytes */

/* DI Status Register */
#define DI_SR			0x00
#define  DI_SR_BRK		(1<<0)
#define  DI_SR_DEINTMASK	(1<<1)
#define  DI_SR_DEINT		(1<<2)
#define  DI_SR_TCINTMASK	(1<<3)
#define  DI_SR_TCINT		(1<<4)
#define  DI_SR_BRKINTMASK	(1<<5)
#define  DI_SR_BRKINT		(1<<6)

/* DI Cover Register */
#define DI_CVR			0x04
#define  DI_CVR_CVR		(1<<0)
#define  DI_CVR_CVRINTMASK	(1<<1)
#define  DI_CVR_CVRINT		(1<<2)

/* DI Command Buffers */
#define DI_CMDBUF0		0x08
#define DI_CMDBUF1		0x0c
#define DI_CMDBUF2		0x10

/* DI DMA Memory Address Register */
#define DI_MAR			0x14

/* DI DMA Transfer Length Register */
#define DI_LENGTH		0x18

/* DI Control Register */
#define DI_CR			0x1c
#define  DI_CR_TSTART		(1<<0)
#define  DI_CR_DMA		(1<<1)
#define  DI_CR_RW		(1<<2)

/* DI Immediate Data Buffer */
#define DI_DATA			0x20

/* DI Configuration Register */
#define DI_CFG			0x24


/* drive status, status */
#define DI_STATUS(s)		((u8)((s)>>24))

#define DI_STATUS_READY			0x00
#define DI_STATUS_COVER_OPENED		0x01
#define DI_STATUS_DISK_CHANGE		0x02
#define DI_STATUS_NO_DISK		0x03
#define DI_STATUS_MOTOR_STOP		0x04
#define DI_STATUS_DISK_ID_NOT_READ	0x05

/* drive status, error */
#define DI_ERROR(s)		((u32)((s)&0x00ffffff))

#define DI_ERROR_NO_ERROR		0x000000
#define DI_ERROR_MOTOR_STOPPED		0x020400
#define DI_ERROR_DISK_ID_NOT_READ	0x020401
#define DI_ERROR_MEDIUM_NOT_PRESENT	0x023a00
#define DI_ERROR_SEEK_INCOMPLETE	0x030200
#define DI_ERROR_UNRECOVERABLE_READ	0x031100
#define DI_ERROR_INVALID_COMMAND	0x052000
#define DI_ERROR_BLOCK_OUT_OF_RANGE	0x052100
#define DI_ERROR_INVALID_FIELD		0x052400
#define DI_ERROR_MEDIUM_CHANGED		0x062800

#define di_may_retry(s)		((DI_STATUS(s) == DI_STATUS_READY || \
				  DI_STATUS(s) == DI_STATUS_DISK_ID_NOT_READ) \
				 && \
				 (DI_ERROR(s) != DI_ERROR_SEEK_INCOMPLETE))

/* DI Sector Size */
#define DI_SECTOR_SHIFT		11
#define DI_SECTOR_SIZE		(1 << DI_SECTOR_SHIFT) /*2048*/


/* ECMA Standards definitions */
#if 0
#define DI_TYPE_DVD_ROM		0x10 /* 0001 0000 ECMA:267,268 */
#define DI_TYPE_DVD_RAM_26	0x11 /* 0001 0001 ECMA:272 */
#define DI_TYPE_DVD_RAM_47	0x61 /* 0110 0001 ECMA:330 */
#define DI_TYPE_DVD_MINUS_R_29	0x12 /* 0001 0010 ECMA:279 */
#define DI_TYPE_DVD_MINUS_R_47	0x72 /* 0101 0010 ECMA:359 */
#define DI_TYPE_DVD_MINUS_R_DL	0x62 /* 0110 0010 ECMA:382 */
#define DI_TYPE_DVD_MINUS_RW	0x23 /* 0010 0011 ECMA:338 */
#define DI_TYPE_DVD_PLUS_RW_30	0x19 /* 0001 1001 ECMA:274 */
#define DI_TYPE_DVD_PLUS_RW_47	0x29 /* 0010 1001 ECMA:337 */
#define DI_TYPE_DVD_PLUS_RW_HS	0x39 /* 0011 1001 ECMA:371 */
#define DI_TYPE_DVD_PLUS_R	0x1a /* 0001 1010 ECMA:349 */
#define DI_TYPE_DVD_PLUS_RW_DL	0x1d /* 0001 1101 ECMA:374 */
#define DI_TYPE_DVD_PLUS_R_DL	0x1e /* 0001 1110 ECMA:364 */
#endif

#define DI_BOOK_TYPE_DVD_ROM		0x00
#define DI_BOOK_TYPE_DVD_RAM		0x01
#define DI_BOOK_TYPE_DVD_MINUS_R	0x02
#define DI_BOOK_TYPE_DVD_MINUS_RW	0x03
#define DI_BOOK_TYPE_DVD_PLUS_RW	0x09
#define DI_BOOK_TYPE_DVD_PLUS_R		0x0a
#define DI_BOOK_TYPE_DVD_PLUS_RW_DL	0x0d
#define DI_BOOK_TYPE_DVD_PLUS_R_DL	0x0e

#define DI_DISK_SIZE_80MM		0x01
#define DI_DISK_SIZE_120MM		0x00



/* Driver Settings */
#define DI_NAME			DRV_MODULE_NAME
#define DI_MAJOR		60

#define DI_COMMAND_TIMEOUT	20 /* seconds */
#define DI_COMMAND_RETRIES	10 /* times */

#define DI_MOTOR_OFF_TIMEOUT	10

#define KERNEL_SECTOR_SHIFT	9
#define KERNEL_SECTOR_SIZE	(1 << KERNEL_SECTOR_SHIFT) /*512*/


/*
 * Drive Information.
 */
struct di_drive_info {
	u16				rev;
	u16				code;
	u32				date;
	u8				pad[0x18];
};

#if 0
/*
 * Disk ID.
 */
struct di_disk_id {
	u8				id[32];
};
#endif

/*
 * Physical format information (as per ECMA standards).
 */
struct di_phys_format_info {
#if defined(__BIG_ENDIAN_BITFIELD)
	/* Byte 0 - Disk Category and Version Number */
	u8 disk_category:4;
	u8 version_number:4;
	/* Byte 1 - Disk size and maximum transfer rate */
	u8 disk_size:4;
	u8 max_xfer_rate:4;
	/* Byte 2 - Disk structure */
	u8 byte2_bit7_zero:1;
	u8 disk_type:2;
	u8 track_path:1;
	u8 layer_type:4;
	/* Byte 3 - Recording density */
	u8 average_channel_bit_length:4;
	u8 average_track_pitch:4;
#elif defined(__LITTLE_ENDIAN_BITFIELD)
	/* Byte 0 - Disk Category and Version Number */
	u8 version_number:4;
	u8 disk_category:4;
	/* Byte 1 - Disk size and maximum transfer rate */
	u8 max_xfer_rate:4;
	u8 disk_size:4;
	/* Byte 2 - Disk structure */
	u8 layer_type:4;
	u8 track_path:1;
	u8 disk_type:2;
	u8 byte2_bit7_zero:1;
	/* Byte 3 - Recording density */
	u8 average_track_pitch:4;
	u8 average_channel_bit_length:4;
#endif
	__be32 first_data_psn;
	__be32 last_data_psn;
	/* don't care for now about the rest */
};

/*
 * An operation code.
 */
struct di_opcode {
	u16				op;
#define DI_OP(id, flags)		(((u8)(id)<<8)|((u8)(flags)))
#define DI_OP_ID(op)		((u8)((op)>>8))
#define DI_OP_FLAGS(op)		((u8)(op))

#define   DI_DIR_READ		0x00
#define   DI_DIR_WRITE		DI_CR_RW
#define   DI_MODE_IMMED		0x00
#define   DI_MODE_DMA		DI_CR_DMA
#define   DI_IGNORE_ERRORS	(1<<7)

	char				*name;

	u32				cmdbuf0;
};

/*
 * Drive code container.
 */
struct di_drive_code {
	u32				address;
	size_t				len;
	void				*code;
};

struct di_device;

/*
 * A Disk Interface command.
 */
struct di_command {
	u16				opidx;

	u32				cmdbuf0;
	u32				cmdbuf1;
	u32				cmdbuf2;

	void				*data;
	size_t				len;

	dma_addr_t			dma_addr;
	size_t				dma_len;

	void				*done_data;
	void				(*done)(struct di_command *cmd);

	u16				retries;
	u16				max_retries;

	u32				result;

	struct di_device		*ddev;
};

#define di_result_ok(result)	((result) == DI_SR_TCINT)
#define di_command_ok(cmd)	(di_result_ok((cmd)->result))

enum {
	__DI_MEDIA_CHANGED,
	__DI_START_QUEUE,
	__DI_RESETTING,
};

/*
 * The Disk Interface device.
 */
struct di_device {
	spinlock_t			lock;

	int				irq;

	spinlock_t			io_lock;
	void __iomem			*io_base;

	struct di_command		*cmd;
	struct di_command		*failed_cmd;

	struct di_command		status;
	u32				drive_status;

	struct gendisk                  *disk;
	struct request_queue            *queue;
	spinlock_t			queue_lock;

	struct request                  *req;
	struct di_command		req_cmd;

	struct di_drive_code		*drive_code;

	u32				model;
	unsigned long			flags;
#define DI_MEDIA_CHANGED	(1<<__DI_MEDIA_CHANGED)
#define DI_START_QUEUE		(1<<__DI_START_QUEUE)
#define DI_RESETTING		(1<<__DI_RESETTING)

	unsigned long			nr_sectors;

	struct timer_list		motor_off_timer;

#ifdef CONFIG_PROC_FS
	struct proc_dir_entry		*proc;
#endif /* CONFIG_PROC_FS */

	int				ref_count;

	struct device			*dev;
};


u8 di_scratch_frame[DI_SECTOR_SIZE]
		 __attribute__ ((aligned(DI_DMA_ALIGN+1)));

/*
 * We do not accept original media with this driver, as there is currently no
 * general need for that.
 * If you ever develop an application (a media player for example) which works
 * with original media, just change di_accept_gods and recompile.
 */
static const int di_accept_gods;

/*
 * Drive operations table, incomplete.
 * We just include here some of the available functions, in no particular
 * order.
 */
#define CMDBUF(a, b, c, d) (((a)<<24)|((b)<<16)|((c)<<8)|(d))

enum {
	DI_OP_NOP,
	DI_OP_INQ,
	DI_OP_STOPMOTOR,
	DI_OP_READDISKID,
	DI_OP_READSECTOR,
	DI_OP_GETSTATUS,
	DI_OP_READPHYSINFO,
	DI_OP_LAST
};

static struct di_opcode di_opcodes[] = {
	[DI_OP_NOP] = {
		.op = DI_OP(DI_OP_NOP, 0),
		.name = "NOP",
		.cmdbuf0 = 0,
	},
	[DI_OP_INQ] = {
		.op = DI_OP(DI_OP_INQ, DI_DIR_READ | DI_MODE_DMA),
		.name = "INQ",
		.cmdbuf0 = 0x12000000,
	},
	[DI_OP_STOPMOTOR] = {
		.op = DI_OP(DI_OP_STOPMOTOR, DI_DIR_READ | DI_MODE_IMMED),
		.name = "STOPMOTOR",
		.cmdbuf0 = 0xe3000000,
	},
	[DI_OP_READDISKID] = {
		.op = DI_OP(DI_OP_READDISKID, DI_DIR_READ | DI_MODE_DMA),
		.name = "READDISKID",
		.cmdbuf0 = 0xa8000040,
	},
	[DI_OP_READSECTOR] = {
		.op = DI_OP(DI_OP_READSECTOR, DI_DIR_READ | DI_MODE_DMA),
		.name = "READSECTOR",
		.cmdbuf0 = 0xd0000000,
	},
	[DI_OP_GETSTATUS] = {
		.op = DI_OP(DI_OP_GETSTATUS, DI_DIR_READ | DI_MODE_IMMED),
		.name = "GETSTATUS",
		.cmdbuf0 = 0xe0000000,
	},
	[DI_OP_READPHYSINFO] = {
		.op = DI_OP(DI_OP_READPHYSINFO, DI_DIR_READ | DI_MODE_DMA),
		.name = "READPHYSINFO",
		.cmdbuf0 = 0xad000000,
	},
};

#define DI_OP_MAXOP		(DI_OP_LAST-1)
#define DI_OP_CUSTOM		((u16)~0)


static void di_reset(struct di_device *ddev);
static int di_run_command(struct di_command *cmd);

/*
 * Returns the operation code related data for a command.
 */
static inline struct di_opcode *di_get_opcode(struct di_command *cmd)
{
	BUG_ON(cmd->opidx > DI_OP_MAXOP && cmd->opidx != DI_OP_CUSTOM);

	if (cmd->opidx == DI_OP_CUSTOM)
		return cmd->data;
	else
		return &di_opcodes[cmd->opidx];
}

/*
 * Returns the operation code for a command.
 */
static inline u16 di_op(struct di_command *cmd)
{
	return di_get_opcode(cmd)->op;
}


/*
 * Basic initialization for all commands.
 */
static void di_op_basic(struct di_command *cmd,
			struct di_device *ddev, u16 opidx)
{
	struct di_opcode *opcode;

	memset(cmd, 0, sizeof(*cmd));
	cmd->ddev = ddev;
	cmd->opidx = opidx;
	cmd->max_retries = cmd->retries = 0;
	opcode = di_get_opcode(cmd);
	if (opcode)
		cmd->cmdbuf0 = opcode->cmdbuf0;
}

/*
 * Builds an "Inquiry" command.
 */
static void di_op_inq(struct di_command *cmd,
		      struct di_device *ddev,
		      struct di_drive_info *drive_info)
{
	di_op_basic(cmd, ddev, DI_OP_INQ);
	cmd->cmdbuf2 = sizeof(*drive_info);
	cmd->data = drive_info;
	cmd->len = sizeof(*drive_info);
}

/*
 * Builds a "Stop Motor" command.
 */
static inline void di_op_stopmotor(struct di_command *cmd,
				   struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_STOPMOTOR);
}

#if 0
/*
 * Builds a "Read Disc ID" command.
 */
static void di_op_readdiskid(struct di_command *cmd,
			     struct di_device *ddev,
			     struct di_disk_id *disk_id)
{
	di_op_basic(cmd, ddev, DI_OP_READDISKID);
	cmd->cmdbuf2 = sizeof(*disk_id);
	cmd->data = disk_id;
	cmd->len = sizeof(*disk_id);
	cmd->max_retries = cmd->retries = DI_COMMAND_RETRIES;
}
#endif

/*
 * Builds a "Read Sector" command.
 */
static void di_op_readsector(struct di_command *cmd,
			     struct di_device *ddev,
			     u32 sector, void *data, size_t len)
{
	di_op_basic(cmd, ddev, DI_OP_READSECTOR);
	cmd->cmdbuf1 = sector << KERNEL_SECTOR_SHIFT >> DI_SECTOR_SHIFT;
	cmd->cmdbuf2 = len >> DI_SECTOR_SHIFT;
	cmd->data = data;
	cmd->len = len;
	cmd->max_retries = cmd->retries = DI_COMMAND_RETRIES;
}

/*
 * Builds a "Read Physical Info" command.
 */
static void di_op_readphysinfo(struct di_command *cmd,
				struct di_device *ddev,
				u8 sector, void *data)
{
	di_op_basic(cmd, ddev, DI_OP_READPHYSINFO);
	cmd->cmdbuf0 |= (sector << 8);
	cmd->data = data;
	cmd->len = 2048;
}


/*
 * Builds a "get drive status" command.
 */
static inline void di_op_getstatus(struct di_command *cmd,
				   struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_GETSTATUS);
}

/*
 * Returns the printable form of the status part of a drive status.
 */
static char *di_printable_status(u32 drive_status)
{
	char *s = "unknown";

	switch (DI_STATUS(drive_status)) {
	case DI_STATUS_READY:
		s = "ready";
		break;
	case DI_STATUS_COVER_OPENED:
		s = "cover opened";
		break;
	case DI_STATUS_DISK_CHANGE:
		s = "disk change";
		break;
	case DI_STATUS_NO_DISK:
		s = "no disk";
		break;
	case DI_STATUS_MOTOR_STOP:
		s = "motor stop";
		break;
	case DI_STATUS_DISK_ID_NOT_READ:
		s = "disk id not read";
		break;
	}
	return s;
}

/*
 * Returns the printable form of the error part of a drive status.
 */
static char *di_printable_error(u32 drive_status)
{
	char *s = "unknown";

	switch (DI_ERROR(drive_status)) {
	case DI_ERROR_NO_ERROR:
		s = "no error";
		break;
	case DI_ERROR_MOTOR_STOPPED:
		s = "motor stopped";
		break;
	case DI_ERROR_DISK_ID_NOT_READ:
		s = "disk id not read";
		break;
	case DI_ERROR_MEDIUM_NOT_PRESENT:
		s = "medium not present";
		break;
	case DI_ERROR_SEEK_INCOMPLETE:
		s = "seek incomplete";
		break;
	case DI_ERROR_UNRECOVERABLE_READ:
		s = "unrecoverable read";
		break;
	case DI_ERROR_INVALID_COMMAND:
		s = "invalid command";
		break;
	case DI_ERROR_BLOCK_OUT_OF_RANGE:
		s = "block out of range";
		break;
	case DI_ERROR_INVALID_FIELD:
		s = "invalid field";
		break;
	case DI_ERROR_MEDIUM_CHANGED:
		s = "medium changed";
		break;
	}

	return s;
}

static char *di_printable_book_type(u8 book_type)
{
	char *s = "unknown";

	switch (book_type) {
	case DI_BOOK_TYPE_DVD_ROM:
		s = "DVD-ROM";
		break;
	case DI_BOOK_TYPE_DVD_RAM:
		s = "DVD-RAM";
		break;
	case DI_BOOK_TYPE_DVD_MINUS_R:
		s = "DVD-R";
		break;
	case DI_BOOK_TYPE_DVD_MINUS_RW:
		s = "DVD-RW";
		break;
	case DI_BOOK_TYPE_DVD_PLUS_RW:
		s = "DVD+RW";
		break;
	case DI_BOOK_TYPE_DVD_PLUS_R:
		s = "DVD+R";
		break;
	case DI_BOOK_TYPE_DVD_PLUS_RW_DL:
		s = "DVD+RW DL";
		break;
	case DI_BOOK_TYPE_DVD_PLUS_R_DL:
		s = "DVD+R DL";
		break;
	}
	return s;
}

static char *di_printable_disk_size(u8 disk_size)
{
	char *s = "unknown";

	switch (disk_size) {
	case DI_DISK_SIZE_80MM:
		s = "80mm";
		break;
	case DI_DISK_SIZE_120MM:
		s = "120mm";
		break;
	}
	return s;
}

/*
 * Prints the given drive status, only if debug enabled.
 */
static inline void di_debug_print_drive_status(u32 drive_status)
{
	pr_devel("%08x, [%s, %s]\n", drive_status,
		 di_printable_status(drive_status),
		 di_printable_error(drive_status));
}

/*
 * Prints the given drive status.
 */
static void di_print_drive_status(u32 drive_status)
{
	pr_info("drive_status=%08x, [%s, %s]\n", drive_status,
		di_printable_status(drive_status),
		di_printable_error(drive_status));
}

#if 0
/*
 * Prints the given disk identifier.
 */
static void di_print_disk_id(struct di_disk_id *disk_id)
{
	pr_info("disk_id = [%s]\n", disk_id->id);
}
#endif

/*
 *
 * I/O.
 */

/*
 * Converts a request direction into a DMA data direction.
 */
static inline
enum dma_data_direction di_opidx_to_dma_dir(struct di_command *cmd)
{
	u16 op = di_op(cmd);

	if (unlikely(op & DI_DIR_WRITE))
		return DMA_TO_DEVICE;
	else
		return DMA_FROM_DEVICE;
}

/*
 * Starts a DMA transfer.
 */
static void di_start_dma_transfer_raw(struct di_device *ddev,
				      dma_addr_t data, size_t len, int mode)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *sr_reg = io_base + DI_SR;
	unsigned long flags;

	BUG_ON((data & DI_DMA_ALIGN) != 0 ||
	       (len & DI_DMA_ALIGN) != 0);

	/* setup address and length of transfer */
	out_be32(io_base + DI_LENGTH, len);
	out_be32(io_base + DI_MAR, data);

	/* enable the Transfer Complete interrupt */
	spin_lock_irqsave(&ddev->io_lock, flags);
	out_be32(sr_reg, in_be32(sr_reg) | DI_SR_TCINTMASK);
	spin_unlock_irqrestore(&ddev->io_lock, flags);

	/* start the transfer */
	out_be32(io_base + DI_CR, DI_CR_TSTART | DI_CR_DMA |
		 (mode & DI_CR_RW));
}

/*
 * Internal. Busy-waits until a DMA transfer finishes or timeouts.
 */
static int __wait_for_dma_transfer_or_timeout(u32 __iomem *cr_reg,
					      int secs)
{
	unsigned long timeout = jiffies + secs*HZ;

	/* busy-wait for transfer complete */
	while ((in_be32(cr_reg) & DI_CR_TSTART) &&
			time_before(jiffies, timeout))
		cpu_relax();

	return (in_be32(cr_reg) & DI_CR_TSTART) ? -EBUSY : 0;
}

#if 0
/*
 * Busy-waits until DMA transfers are finished.
 */
static void di_wait_for_dma_transfer_raw(struct di_device *ddev)
{
	u32 __iomem *cr_reg = ddev->io_base + DI_CR;
	u32 __iomem *sr_reg = ddev->io_base + DI_SR;
	unsigned long flags;

	/* we don't want TCINTs to disturb us while waiting */
	spin_lock_irqsave(&ddev->io_lock, flags);
	out_be32(sr_reg, in_be32(sr_reg) & ~DI_SR_TCINTMASK);
	spin_unlock_irqrestore(&ddev->io_lock, flags);

	/* if the drive got stuck, reset it */
	if (__wait_for_dma_transfer_or_timeout(cr_reg, DI_COMMAND_TIMEOUT)) {
		pr_devel("dvd stuck!\n");
		di_reset(ddev);
	}

	/* ack and enable the Transfer Complete interrupt */
	spin_lock_irqsave(&ddev->io_lock, flags);
	out_be32(sr_reg, in_be32(sr_reg) | (DI_SR_TCINT|DI_SR_TCINTMASK));
	spin_unlock_irqrestore(&ddev->io_lock, flags);

	return;
}
#endif

/*
 * Quiesces the hardware to a calm and known state.
 */
static void di_quiesce(struct di_device *ddev)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *cr_reg = io_base + DI_CR;
	u32 __iomem *sr_reg = io_base + DI_SR;
	u32 __iomem *cvr_reg = io_base + DI_CVR;
	u32 sr, cvr;
	unsigned long flags;

	spin_lock_irqsave(&ddev->io_lock, flags);

	/* ack and mask dvd io interrupts */
	sr = in_be32(sr_reg);
	sr |= DI_SR_BRKINT | DI_SR_TCINT | DI_SR_DEINT;
	sr &= ~(DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINTMASK);
	out_be32(sr_reg, sr);

	/* ack and mask dvd cover interrupts */
	cvr = in_be32(cvr_reg);
	out_be32(cvr_reg, (cvr | DI_CVR_CVRINT) & ~DI_CVR_CVRINTMASK);

	spin_unlock_irqrestore(&ddev->io_lock, flags);

	/* busy-wait for transfer complete */
	__wait_for_dma_transfer_or_timeout(cr_reg, DI_COMMAND_TIMEOUT);
}

/*
 * Command engine.
 *
 */

/*
 * Outputs the command buffers, and optionally starts a transfer.
 */
static void di_prepare_command(struct di_command *cmd, int tstart)
{
	struct di_opcode *opcode = di_get_opcode(cmd);
	void __iomem *io_base = cmd->ddev->io_base;

	out_be32(io_base + DI_CMDBUF0, cmd->cmdbuf0);
	out_be32(io_base + DI_CMDBUF1, cmd->cmdbuf1);
	out_be32(io_base + DI_CMDBUF2, cmd->cmdbuf2);

	cmd->ddev->drive_status = 0;

	if (tstart) {
		out_be32(io_base + DI_CR, DI_CR_TSTART |
			 (opcode->op & DI_CR_RW));
	}
}

static void di_command_done(struct di_command *cmd);

/*
 * Starts a command by using the immediate mode.
 */
static int di_start_command(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	unsigned long flags;
	int retval = 1;

	spin_lock_irqsave(&ddev->lock, flags);

	BUG_ON(ddev->cmd);

	ddev->cmd = cmd;
	cmd->dma_len = 0; /* no dma here */
	di_prepare_command(cmd, 1);

	spin_unlock_irqrestore(&ddev->lock, flags);

	return retval;
}

/*
 * Starts a command by using the DMA mode.
 */
static int di_start_dma_command(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	unsigned long flags;
	int retval = 1;

	spin_lock_irqsave(&ddev->lock, flags);

	BUG_ON(ddev->cmd);
	BUG_ON(!cmd->len || !cmd->data);

	ddev->cmd = cmd;
	cmd->dma_len = cmd->len;
	cmd->dma_addr = dma_map_single(ddev->dev,
				       cmd->data, cmd->len,
				       di_opidx_to_dma_dir(cmd));
	if (dma_mapping_error(ddev->dev, cmd->dma_addr)) {
		retval = -EIO;
		goto done;
	}

	di_prepare_command(cmd, 0);
	di_start_dma_transfer_raw(ddev, cmd->dma_addr, cmd->dma_len,
				  di_op(cmd) & DI_DIR_WRITE);

done:
	spin_unlock_irqrestore(&ddev->lock, flags);

	return retval;
}

/*
 * Completes a "get drive status" command, after a failed command.
 */
static void di_complete_getstatus(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *data_reg = io_base + DI_DATA;

	ddev->drive_status = in_be32(data_reg);
}

/*
 * Called after a transfer is completed.
 */
static void di_complete_transfer(struct di_device *ddev, u32 result)
{
	struct di_command *cmd;
	struct di_opcode *opcode;
	u32 drive_status;
	unsigned long flags;

	spin_lock_irqsave(&ddev->lock, flags);

	/* do nothing if we have nothing to complete */
	cmd = ddev->cmd;
	if (!cmd) {
		spin_unlock_irqrestore(&ddev->lock, flags);
		goto out;
	}

	/* free the command slot */
	ddev->cmd = NULL;

	spin_unlock_irqrestore(&ddev->lock, flags);

	/* deal with caches after a dma transfer */
	if (cmd->dma_len) {
		dma_unmap_single(ddev->dev,
				 cmd->dma_addr, cmd->dma_len,
				 di_opidx_to_dma_dir(cmd));
#if 1
		/*
		 * Meh...
		 * Someone (the block layer?) seems to dereference parts of
		 * req->buffer after having dma-mapped it and while it is
		 * owned by the low level driver.
		 *
		 * This causes a problem in non-coherent systems as those
		 * parts of the buffer end up with the wrong data if
		 * dereferenced before the DI dma engine writes to them.
		 * Force a cache invalidation again to make sure that we
		 * really get the right data from the device.
		 *
		 * Without this countermeasure you can easily get random
		 * garbage in files while reading them from a iso9660 disc.
		 */
		if (likely(di_opidx_to_dma_dir(cmd) == DMA_FROM_DEVICE))
			__dma_sync(cmd->data, cmd->len, DMA_FROM_DEVICE);
#endif
	}

	opcode = di_get_opcode(cmd);

	/*
	 * If a command fails we check the drive status. Depending on that
	 * we may or not retry later the command.
	 */
	cmd->result = result;
	if (!di_command_ok(cmd)) {
		BUG_ON(ddev->failed_cmd != NULL);

		ddev->failed_cmd = cmd;

		/*
		 * Issue immediately a "get drive status"
		 * after a failed command.
		 */
		cmd = &ddev->status;
		di_op_getstatus(cmd, ddev);
		cmd->done = di_complete_getstatus;
		di_run_command(cmd);
		goto out;
	} else {
		if (cmd->retries != cmd->max_retries) {
			pr_devel("command %s succeeded after %d retries :-)\n",
				 opcode->name, cmd->max_retries - cmd->retries);
		}
	}

	/* complete a successful command, or the MATSHITA one */
	di_command_done(cmd);

	spin_lock_irqsave(&ddev->lock, flags);
	if (ddev->failed_cmd) {
		cmd = ddev->failed_cmd;
		ddev->failed_cmd = NULL;
		spin_unlock_irqrestore(&ddev->lock, flags);

		drive_status = ddev->drive_status;
		opcode = di_get_opcode(cmd);

		/* retry a previously failed command if appropiate */
		if (cmd->retries > 0) {
			if (di_may_retry(drive_status)) {
				pr_devel("command %s failed, %d retries left\n",
					 opcode->name, cmd->retries);
				di_debug_print_drive_status(drive_status);

				cmd->retries--;
				di_run_command(cmd);
				goto out;
			} else {
				pr_devel("command %s failed,"
					 " aborting due to drive status\n",
				    opcode->name);
			}
		} else {
			if (!(opcode->op & DI_IGNORE_ERRORS))
				pr_devel("command %s failed\n", opcode->name);
		}

		if (!(opcode->op & DI_IGNORE_ERRORS))
			di_print_drive_status(drive_status);

		/* complete the failed command */
		di_command_done(cmd);

		/* update the driver status */
		switch (DI_ERROR(drive_status)) {
		case DI_ERROR_MOTOR_STOPPED:
		case DI_ERROR_MEDIUM_NOT_PRESENT:
		case DI_ERROR_MEDIUM_CHANGED:
			set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
			break;
		default:
			break;
		}

	} else {
		spin_unlock_irqrestore(&ddev->lock, flags);
	}

	/* start the block layer queue if someone requested it */
	if (test_and_clear_bit(__DI_START_QUEUE, &ddev->flags)) {
		spin_lock_irqsave(&ddev->queue_lock, flags);
		blk_start_queue(ddev->queue);
		spin_unlock_irqrestore(&ddev->queue_lock, flags);
	}

out:
	return;
}

/*
 * Calls any done hooks.
 */
static void di_command_done(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	unsigned long flags;

	/* if specified, call the completion routine */
	if (cmd->done)
		cmd->done(cmd);

	spin_lock_irqsave(&ddev->lock, flags);
	spin_unlock_irqrestore(&ddev->lock, flags);
}

/*
 * Completion routine.
 */
static void di_wait_done(struct di_command *cmd)
{
	complete(cmd->done_data);
}

/*
 * Runs a command.
 */
static int di_run_command(struct di_command *cmd)
{
	struct di_opcode *opcode = di_get_opcode(cmd);
	int retval;

	if (cmd->retries > cmd->max_retries)
		cmd->retries = cmd->max_retries;

	if (!(opcode->op & DI_MODE_DMA))
		retval = di_start_command(cmd);
	else
		retval = di_start_dma_command(cmd);
	return retval;
}

/*
 * Runs a command and waits.
 * Might sleep if called from user context.
 */
static int di_run_command_and_wait(struct di_command *cmd)
{
	DECLARE_COMPLETION(complete);

	cmd->done_data = &complete;
	cmd->done = di_wait_done;
	if (di_run_command(cmd) > 0)
		wait_for_completion(&complete);
	return cmd->result;
}

/*
 * Interrupt handler for DI interrupts.
 */
static irqreturn_t di_irq_handler(int irq, void *dev0)
{
	struct di_device *ddev = dev0;
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *sr_reg = io_base + DI_SR;
	u32 __iomem *cvr_reg = io_base + DI_CVR;
	u32 sr, cvr, reason, mask;
	unsigned long flags;

	spin_lock_irqsave(&ddev->io_lock, flags);

	sr = in_be32(sr_reg);
	mask = sr & (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINTMASK);
	reason = sr; /* & (mask << 1); */
	if (reason) {
		out_be32(sr_reg, sr | reason);
		spin_unlock_irqrestore(&ddev->io_lock, flags);

		if (reason & DI_SR_BRKINT) {
			pr_devel("BRKINT\n");
			di_complete_transfer(ddev, DI_SR_BRKINT);
		}
		if (reason & DI_SR_TCINT)
			di_complete_transfer(ddev, DI_SR_TCINT);
		if (reason & DI_SR_DEINT)
			di_complete_transfer(ddev, DI_SR_DEINT);

		spin_lock_irqsave(&ddev->io_lock, flags);
	}

	cvr = in_be32(cvr_reg);
	mask = cvr & DI_CVR_CVRINTMASK;
	reason = cvr; /* & (mask << 1); */
	if ((reason & DI_CVR_CVRINT)) {
		out_be32(cvr_reg, cvr | DI_CVR_CVRINT);
		set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
		if (!test_and_clear_bit(__DI_RESETTING, &ddev->flags))
			pr_devel("disk inserted!\n");
	}

	spin_unlock_irqrestore(&ddev->io_lock, flags);

	return IRQ_HANDLED;
}

/*
 * Hard-resets the drive.
 */
static void di_reset(struct di_device *ddev)
{
	void *hw_diflags;
	void *hw_resets;

	hw_resets = ioremap(0x0d800194, 4);
	if (hw_resets) {
		set_bit(__DI_RESETTING, &ddev->flags);
		/* reset dvd unit */
		out_be32(hw_resets, in_be32(hw_resets) & ~(1<<10));
		mdelay(50);
		out_be32(hw_resets, in_be32(hw_resets) | (1<<10));
		iounmap(hw_resets);
	}
	pr_devel("drive reset\n");
	hw_diflags = ioremap(0x0d800180, 4);
	if (hw_diflags) {
		/* enable dvd-video */
		out_be32(hw_diflags, in_be32(hw_diflags) & ~(1<<21));
		iounmap(hw_diflags);
	}
}


/*
 * Misc routines.
 *
 */

/*
 * Retrieves (and prints out) the laser unit model.
 */
static u32 di_retrieve_drive_model(struct di_device *ddev)
{
	struct di_drive_info *di_drive_info;
	struct di_command cmd;

	di_drive_info = (struct di_drive_info *)di_scratch_frame;
	memset(di_drive_info, 0, sizeof(*di_drive_info));
	di_op_inq(&cmd, ddev, di_drive_info);
	di_run_command_and_wait(&cmd);

	pr_info("laser unit: rev=%x, code=%x, date=%x\n",
		di_drive_info->rev, di_drive_info->code,
		di_drive_info->date);

	ddev->model = di_drive_info->date;
	return ddev->model;
}

/*
 * Gets the current drive status.
 */
static u32 di_get_drive_status(struct di_device *ddev)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *data_reg = io_base + DI_DATA;
	struct di_command cmd;
	u32 drive_status;

	di_op_getstatus(&cmd, ddev);
	di_run_command_and_wait(&cmd);
	drive_status = in_be32(data_reg);

	return drive_status;
}

/*
 * Checks if the drive is in a ready state.
 */
static int di_is_drive_ready(struct di_device *ddev)
{
	u32 drive_status;
	int result = 0;

	drive_status = di_get_drive_status(ddev);
	if (DI_STATUS(drive_status) == DI_STATUS_DISK_ID_NOT_READ ||
	    DI_STATUS(drive_status) == DI_STATUS_READY) {
		result = 1;
	}

	return result;
}

/*
 * Spins down the drive, immediatelly.
 */
static void di_spin_down_drive(struct di_device *ddev)
{
	struct di_command cmd;

	di_op_stopmotor(&cmd, ddev);
	di_run_command_and_wait(&cmd);
}

/*
 * Stops the drive's motor, according to a previous schedule.
 */
static void di_motor_off(unsigned long ddev0)
{
	struct di_device *ddev = (struct di_device *)ddev0;
	struct di_command *cmd;
	unsigned long flags;

	/* postpone a bit the motor off if there are pending commands */
	spin_lock_irqsave(&ddev->lock, flags);
	if (!ddev->cmd) {
		ddev->cmd = cmd = &ddev->status;
		spin_unlock_irqrestore(&ddev->lock, flags);
		di_op_stopmotor(cmd, ddev);
		di_prepare_command(cmd, 1);
	} else {
		spin_unlock_irqrestore(&ddev->lock, flags);
		mod_timer(&ddev->motor_off_timer, jiffies + 1*HZ);
	}
}

/*
 * Cancels a previously scheduled motor off.
 */
static inline void di_cancel_motor_off(struct di_device *ddev)
{
	del_timer(&ddev->motor_off_timer);
}

/*
 * Stops the drive's motor after the specified amount of seconds has elapsed.
 */
static void di_schedule_motor_off(struct di_device *ddev, unsigned int secs)
{
	del_timer(&ddev->motor_off_timer);
	ddev->motor_off_timer.expires = jiffies + secs*HZ;
	ddev->motor_off_timer.data = (unsigned long)ddev;
	add_timer(&ddev->motor_off_timer);
}

/*
 * Spins up the drive.
 */
static void di_spin_up_drive(struct di_device *ddev)
{
	/* do nothing if the drive is already spinning */
	if (di_is_drive_ready(ddev))
		goto out;

	di_reset(ddev);
out:
	return;
}


/*
 * Block layer hooks.
 *
 */

static int di_read_toc(struct di_device *ddev)
{
	struct di_phys_format_info *info;
	struct di_command cmd;
	int error;

	di_cancel_motor_off(ddev);

	/* spin up the drive if needed */
	if ((ddev->flags & DI_MEDIA_CHANGED))
		di_spin_up_drive(ddev);

	di_op_readphysinfo(&cmd, ddev, 0, di_scratch_frame);
	di_run_command_and_wait(&cmd);
	if (di_command_ok(&cmd)) {
		info = (struct di_phys_format_info *)di_scratch_frame;

		be32_to_cpus(&info->first_data_psn);
		be32_to_cpus(&info->last_data_psn);

		/* nr_sectors is specified in DI sectors here */
		ddev->nr_sectors = info->last_data_psn - 
					info->first_data_psn + 1;

		pr_devel("%s %s disk found (%lu sectors)\n",
			 di_printable_disk_size(info->disk_size),
			 di_printable_book_type(info->disk_category),
			 ddev->nr_sectors);
		clear_bit(__DI_MEDIA_CHANGED, &ddev->flags);
		error = 0;
	} else {
		ddev->nr_sectors = 0;

		pr_devel("media NOT ready\n");
		di_spin_down_drive(ddev);
		error = -ENOMEDIUM;
	}

	/* transform to kernel sectors */
	ddev->nr_sectors <<= (DI_SECTOR_SHIFT - KERNEL_SECTOR_SHIFT);
	set_capacity(ddev->disk, ddev->nr_sectors);

	return error;
}


static void di_request_done(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	struct request *req;
	unsigned long flags;
	int error = (cmd->result & DI_SR_TCINT) ? 0 : -EIO;

	spin_lock_irqsave(&ddev->lock, flags);
	req = ddev->req;
	ddev->req = NULL;
	spin_unlock_irqrestore(&ddev->lock, flags);

	if (req) {
		spin_lock_irqsave(&ddev->queue_lock, flags);
		__blk_end_request_cur(req, error);
		blk_start_queue(ddev->queue);
		spin_unlock_irqrestore(&ddev->queue_lock, flags);
	}
}

static void di_do_request(struct request_queue *q)
{
	struct di_device *ddev = q->queuedata;
	struct di_command *cmd = &ddev->req_cmd;
	struct request *req;
	unsigned long start;
	unsigned long flags;
	size_t len;
	int error;

	req = blk_peek_request(q);
	while (req) {
		spin_lock_irqsave(&ddev->lock, flags);

		if (ddev->req || ddev->cmd) {
			blk_stop_queue(q);
			if (ddev->cmd)
				set_bit(__DI_START_QUEUE, &ddev->flags);
			spin_unlock_irqrestore(&ddev->lock, flags);
			break;
		}

		blk_start_request(req);
		error = -EIO;

		if (!blk_fs_request(req))
			goto done;

		/* it doesn't make sense to write to this device */
		if (unlikely(rq_data_dir(req) == WRITE)) {
			pr_err("write attempted\n");
			goto done;
		}

		/* it is not a good idea to open the lid ... */
		if ((ddev->flags & DI_MEDIA_CHANGED)) {
			pr_err("media changed, aborting\n");
			goto done;
		}

		/* keep our reads within limits */
		if (blk_rq_pos(req) +
				blk_rq_cur_sectors(req) > ddev->nr_sectors) {
			pr_err("reading past end\n");
			goto done;
		}

		ddev->req = req;
		blk_stop_queue(q);
		spin_unlock_irqrestore(&ddev->lock, flags);

		/* launch the corresponding read sector command */
		start = blk_rq_pos(req);
		len = blk_rq_cur_bytes(req);
		if (len & (DI_SECTOR_SIZE-1))
			pr_devel("len=%u\n", len);

		di_op_readsector(cmd, ddev, start, req->buffer, len);
		cmd->done_data = cmd;
		cmd->done = di_request_done;
		di_run_command(cmd);
		error = 0;
		break;
	done:
		spin_unlock_irqrestore(&ddev->lock, flags);
		if (!__blk_end_request_cur(req, error))
			req = blk_peek_request(q);
	}
}

/*
 * Block device hooks.
 *
 */

static int di_open(struct block_device *bdev, fmode_t mode)
{
	struct di_device *ddev = bdev->bd_disk->private_data;
	struct di_command *cmd;
	DECLARE_COMPLETION(complete);
	unsigned long flags;
	int retval = 0;

	/* this is a read only device */
	if (mode & FMODE_WRITE) {
		retval = -EROFS;
		goto out;
	}

	/*
	 * If we have a pending command, that's a previously scheduled
	 * motor off. Wait for it to terminate before going on.
	 */
	spin_lock_irqsave(&ddev->lock, flags);
	if (ddev->cmd && ddev->ref_count == 0) {
		cmd = ddev->cmd;
		cmd->done_data = &complete;
		cmd->done = di_wait_done;
		spin_unlock_irqrestore(&ddev->lock, flags);
		wait_for_completion(&complete);
	} else {
		spin_unlock_irqrestore(&ddev->lock, flags);
	}

	/* this will take care of validating the media */
	check_disk_change(bdev);
	if (!ddev->nr_sectors) {
		retval = -ENOMEDIUM;
		goto out;
	}

	spin_lock_irqsave(&ddev->queue_lock, flags);

	/* honor exclusive open mode */
	if (ddev->ref_count == -1 ||
	    (ddev->ref_count && (mode & FMODE_EXCL))) {
		retval = -EBUSY;
		goto out_unlock;
	}

	if ((mode & FMODE_EXCL))
		ddev->ref_count = -1;
	else
		ddev->ref_count++;

out_unlock:
	spin_unlock_irqrestore(&ddev->queue_lock, flags);
out:
	return retval;

}

static int di_release(struct gendisk *disk, fmode_t mode)
{
	struct di_device *ddev = disk->private_data;
	unsigned long flags;

	spin_lock_irqsave(&ddev->queue_lock, flags);

	if (ddev->ref_count > 0)
		ddev->ref_count--;
	else
		ddev->ref_count = 0;

	spin_unlock_irqrestore(&ddev->queue_lock, flags);

	if (ddev->ref_count == 0) {
		/*
		 * We do not immediately stop the motor, which saves us
		 * a spin down/spin up in applications that re-open quickly
		 * the device, like mount when -t is not specified.
		 */
		di_schedule_motor_off(ddev, 1);

		set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
	}

	return 0;
}

static int di_revalidate_disk(struct gendisk *disk)
{
	struct di_device *ddev = disk->private_data;
	di_read_toc(ddev);
	return 0;
}

static int di_media_changed(struct gendisk *disk)
{
	struct di_device *ddev = disk->private_data;
	return (ddev->flags & DI_MEDIA_CHANGED) ? 1 : 0;
}

static int di_ioctl(struct block_device *bdev, fmode_t mode,
		    unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int ret;
	struct gendisk *disk = bdev->bd_disk;

	ret = scsi_cmd_ioctl(disk->queue, disk, mode, cmd, argp);
	if (ret != -ENOTTY)
		return ret;

	return -ENOSYS;
}

static struct block_device_operations di_fops = {
	.owner = THIS_MODULE,
	.open = di_open,
	.release = di_release,
	.revalidate_disk = di_revalidate_disk,
	.media_changed = di_media_changed,
	.ioctl = di_ioctl,
};

/*
 * Setup routines.
 *
 */

static int di_init_irq(struct di_device *ddev)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *sr_reg = io_base + DI_SR;
	u32 __iomem *cvr_reg = io_base + DI_CVR;
	u32 sr, cvr;
	unsigned long flags;
	int retval;

	init_timer(&ddev->motor_off_timer);
	ddev->motor_off_timer.function =
		(void (*)(unsigned long))di_motor_off;

	ddev->flags = 0;
	set_bit(__DI_MEDIA_CHANGED, &ddev->flags);

	/* calm down things a bit first */
	di_quiesce(ddev);

	/* request interrupt */
	retval = request_irq(ddev->irq, di_irq_handler, 0,
			     DRV_MODULE_NAME, ddev);
	if (retval) {
		pr_err("request of irq%d failed\n", ddev->irq);
		goto out;
	}

	spin_lock_irqsave(&ddev->io_lock, flags);

	sr = in_be32(sr_reg);
	sr |= DI_SR_BRKINT | DI_SR_TCINT | DI_SR_DEINT;
	sr |= DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINTMASK;
	out_be32(sr_reg, sr);

	cvr = in_be32(cvr_reg);
	out_be32(cvr_reg, cvr | DI_CVR_CVRINT | DI_CVR_CVRINTMASK);

	spin_unlock_irqrestore(&ddev->io_lock, flags);

	di_retrieve_drive_model(ddev);

	di_schedule_motor_off(ddev, DI_MOTOR_OFF_TIMEOUT);

out:
	return retval;
}

static void di_exit_irq(struct di_device *ddev)
{
	/* stop DVD motor */
	di_cancel_motor_off(ddev);
	di_spin_down_drive(ddev);

	di_quiesce(ddev);

	free_irq(ddev->irq, ddev);
}

static int di_init_blk_dev(struct di_device *ddev)
{
	struct gendisk *disk;
	struct request_queue *queue;
	int retval;

	spin_lock_init(&ddev->lock);
	spin_lock_init(&ddev->io_lock);

	ddev->ref_count = 0;

	retval = register_blkdev(DI_MAJOR, DI_NAME);
	if (retval) {
		pr_err("error registering major %d\n", DI_MAJOR);
		goto err_register_blkdev;
	}

	retval = -ENOMEM;
	spin_lock_init(&ddev->queue_lock);
	queue = blk_init_queue(di_do_request, &ddev->queue_lock);
	if (!queue) {
		pr_err("error initializing queue\n");
		goto err_blk_init_queue;
	}

	blk_queue_logical_block_size(queue, DI_SECTOR_SIZE);
	blk_queue_dma_alignment(queue, DI_DMA_ALIGN);
	/* max number of data segments that the driver deals with */
	blk_queue_max_phys_segments(queue, 1);
	/* max number of data segments that the hardware deals with */
	blk_queue_max_hw_segments(queue, 1);
	/* max size for a data segment */
	blk_queue_max_segment_size(queue, 32*1024);
	queue->queuedata = ddev;
	ddev->queue = queue;

	disk = alloc_disk(1);
	if (!disk) {
		pr_err("error allocating disk\n");
		goto err_alloc_disk;
	}

	disk->major = DI_MAJOR;
	disk->first_minor = 0;
	disk->fops = &di_fops;
	strcpy(disk->disk_name, DI_NAME);
	disk->queue = ddev->queue;
	disk->private_data = ddev;
	ddev->disk = disk;

	set_disk_ro(ddev->disk, 1);
	add_disk(ddev->disk);

	retval = 0;
	goto out;

err_alloc_disk:
	blk_cleanup_queue(ddev->queue);
err_blk_init_queue:
	unregister_blkdev(DI_MAJOR, DI_NAME);
err_register_blkdev:
out:
	return retval;
}

static void di_exit_blk_dev(struct di_device *ddev)
{
	if (ddev->disk) {
		del_gendisk(ddev->disk);
		put_disk(ddev->disk);
	}
	if (ddev->queue)
		blk_cleanup_queue(ddev->queue);
	unregister_blkdev(DI_MAJOR, DI_NAME);
}

static int di_init_proc(struct di_device *ddev)
{
#ifdef CONFIG_PROC_FS
#endif /* CONFIG_PROC_FS */
	return 0;
}

static void di_exit_proc(struct di_device *ddev)
{
#ifdef CONFIG_PROC_FS
#endif /* CONFIG_PROC_FS */
}

static int di_init(struct di_device *ddev, struct resource *mem, int irq)
{
	int retval;

	ddev->io_base = ioremap(mem->start, mem->end - mem->start + 1);
	ddev->irq = irq;

	retval = di_init_blk_dev(ddev);
	if (!retval) {
		retval = di_init_irq(ddev);
		if (retval)
			di_exit_blk_dev(ddev);
		else
			di_init_proc(ddev);
	}
	return retval;
}

static void di_exit(struct di_device *ddev)
{
	di_exit_blk_dev(ddev);
	di_exit_irq(ddev);
	di_exit_proc(ddev);
	if (ddev->io_base) {
		iounmap(ddev->io_base);
		ddev->io_base = NULL;
	}
}

/*
 * Driver model helper routines.
 *
 */

static int di_do_probe(struct device *dev,
		       struct resource *mem, int irq)
{
	struct di_device *ddev;
	int retval;

	ddev = kzalloc(sizeof(*ddev), GFP_KERNEL);
	if (!ddev) {
		pr_err("failed to allocate di_device\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, ddev);
	ddev->dev = dev;

	retval = di_init(ddev, mem, irq);
	if (retval) {
		dev_set_drvdata(dev, NULL);
		kfree(ddev);
	}
	return retval;
}

static int di_do_remove(struct device *dev)
{
	struct di_device *ddev = dev_get_drvdata(dev);

	if (ddev) {
		di_exit(ddev);
		dev_set_drvdata(dev, NULL);
		kfree(ddev);
		return 0;
	}
	return -ENODEV;
}

static int di_do_shutdown(struct device *dev)
{
	struct di_device *ddev = dev_get_drvdata(dev);

	if (ddev)
		di_quiesce(ddev);
	return 0;
}

/*
 * OF platform driver hooks.
 *
 */

static int __init di_of_probe(struct of_device *odev,
			      const struct of_device_id *match)
{
	struct resource res;
	int error = -ENODEV;

	if (starlet_get_ipc_flavour() != STARLET_IPC_MINI)
		goto out;

	error = of_address_to_resource(odev->node, 0, &res);
	if (error) {
		pr_err("no io memory range found\n");
		goto out;
	}

	error = di_do_probe(&odev->dev, &res,
			    irq_of_parse_and_map(odev->node, 0));
out:
	return error;
}

static int __exit di_of_remove(struct of_device *odev)
{
	return di_do_remove(&odev->dev);
}

static int di_of_shutdown(struct of_device *odev)
{
	return di_do_shutdown(&odev->dev);
}


static struct of_device_id di_of_match[] = {
	{ .compatible = "nintendo,hollywood-disk" },
	{ },
};

MODULE_DEVICE_TABLE(of, di_of_match);

static struct of_platform_driver di_of_driver = {
	.owner = THIS_MODULE,
	.name = DRV_MODULE_NAME,
	.match_table = di_of_match,
	.probe = di_of_probe,
	.remove = di_of_remove,
	.shutdown = di_of_shutdown,
};

/*
 * Module interface hooks.
 *
 */

static int __init di_init_module(void)
{
	pr_info("%s - version %s\n", DRV_DESCRIPTION, di_driver_version);

	return of_register_platform_driver(&di_of_driver);
}

static void __exit di_exit_module(void)
{
	of_unregister_platform_driver(&di_of_driver);
}

module_init(di_init_module);
module_exit(di_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");
