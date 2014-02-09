/*
 * drivers/block/gcn-di/gcn-di.c
 *
 * Nintendo GameCube Disk Interface (DI) driver
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

#define DI_DEBUG

#define DRV_MODULE_NAME	"gcn-di"
#define DRV_DESCRIPTION	"Nintendo GameCube Disk Interface (DI) driver"
#define DRV_AUTHOR	"Albert Herranz"

static char di_driver_version[] = "1.0i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

#ifdef DI_DEBUG
#  define DBG(fmt, args...) \
	   printk(KERN_ERR "%s: " fmt, __func__ , ## args)
#else
#  define DBG(fmt, args...)
#endif


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
#define DI_MAX_SECTORS		712880


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

/*
 * Disk ID.
 */
struct di_disk_id {
	u8				id[32];
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
	__DI_INTEROPERABLE = 0,
	__DI_MEDIA_CHANGED,
	__DI_START_QUEUE,
	__DI_RESETTING,
	__DI_AVOID_DEBUG,
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
#define DI_INTEROPERABLE	(1<<__DI_INTEROPERABLE)
#define DI_MEDIA_CHANGED	(1<<__DI_MEDIA_CHANGED)
#define DI_START_QUEUE		(1<<__DI_START_QUEUE)
#define DI_RESETTING		(1<<__DI_RESETTING)
#define DI_AVOID_DEBUG		(1<<__DI_AVOID_DEBUG)

	unsigned long			nr_sectors;

	struct timer_list		motor_off_timer;

#ifdef CONFIG_PROC_FS
	struct proc_dir_entry		*proc;
#endif /* CONFIG_PROC_FS */

	int				ref_count;

	struct device			*dev;
};


static struct di_drive_info di_drive_info
		 __attribute__ ((aligned(DI_DMA_ALIGN+1)));

/*
 * We do not accept original media with this driver, as there is currently no
 * general need for that.
 * If you ever develop an application (a media player for example) which works
 * with original media, just change di_accept_gods and recompile.
 */
static const int di_accept_gods;

/*
 * Drive firmware extensions.
 *
 */

#define DI_DRIVE_CODE_BASE	0x40d000
#define DI_DRIVE_IRQ_VECTOR	0x00804c

/*
 * Drive 04 (20020402) firmware extensions.
 */

#include "drive_20020402.h"

static struct di_drive_code drive_20020402 = {
	.address = DI_DRIVE_CODE_BASE,
	.len = sizeof(drive_20020402_firmware),
	.code = (u8 *)drive_20020402_firmware,
};

/*
 * Drive 06 (20010608) firmware extensions.
 */

#include "drive_20010608.h"

static struct di_drive_code drive_20010608 = {
	.address = DI_DRIVE_CODE_BASE,
	.len = sizeof(drive_20010608_firmware),
	.code = (u8 *)drive_20010608_firmware,
};

/*
 * Drive 08 (20020823) firmware extensions.
 */

#include "drive_20020823.h"

static struct di_drive_code drive_20020823 = {
	.address = DI_DRIVE_CODE_BASE,
	.len = sizeof(drive_20020823_firmware),
	.code = (u8 *)drive_20020823_firmware,
};

/*
 * Panasonic Q (20010831) firmware extensions.
 */

#include "drive_20010831.h"

static struct di_drive_code drive_20010831 = {
	.address = DI_DRIVE_CODE_BASE,
	.len = sizeof(drive_20010831_firmware),
	.code = (u8 *)drive_20010831_firmware,
};


/*
 * Drive operations table, incomplete.
 * We just include here some of the available functions, in no particular
 * order.
 */
#define CMDBUF(a, b, c, d) (((a)<<24)|((b)<<16)|((c)<<8)|(d))

static struct di_opcode di_opcodes[] = {

#define DI_OP_NOP		0
	[DI_OP_NOP] = {
		.op = DI_OP(DI_OP_NOP, 0),
		.name = "NOP",
		.cmdbuf0 = 0,
	},

#define DI_OP_INQ		(DI_OP_NOP+1)
	[DI_OP_INQ] = {
		.op = DI_OP(DI_OP_INQ, DI_DIR_READ | DI_MODE_DMA),
		.name = "INQ",
		.cmdbuf0 = 0x12000000,
	},

#define DI_OP_STOPMOTOR		(DI_OP_INQ+1)
	[DI_OP_STOPMOTOR] = {
		.op = DI_OP(DI_OP_STOPMOTOR, DI_DIR_READ | DI_MODE_IMMED),
		.name = "STOPMOTOR",
		.cmdbuf0 = 0xe3000000,
	},

#define DI_OP_READDISKID	(DI_OP_STOPMOTOR+1)
	[DI_OP_READDISKID] = {
		.op = DI_OP(DI_OP_READDISKID, DI_DIR_READ | DI_MODE_DMA),
		.name = "READDISKID",
		.cmdbuf0 = 0xa8000040,
	},

#define DI_OP_READSECTOR	(DI_OP_READDISKID+1)
	[DI_OP_READSECTOR] = {
		.op = DI_OP(DI_OP_READSECTOR, DI_DIR_READ | DI_MODE_DMA),
		.name = "READSECTOR",
		.cmdbuf0 = 0xa8000000,
	},

#define DI_OP_ENABLE1		(DI_OP_READSECTOR+1)
	[DI_OP_ENABLE1] = {
		.op = DI_OP(DI_OP_ENABLE1, DI_DIR_READ | DI_MODE_IMMED),
		.name = "MATSHITA",
		.cmdbuf0 = 0,
	},

#define DI_OP_ENABLE2		(DI_OP_ENABLE1+1)
	[DI_OP_ENABLE2] = {
		.op = DI_OP(DI_OP_ENABLE2, DI_DIR_READ | DI_MODE_IMMED),
		.name = "DVD-GAME",
		.cmdbuf0 = 0,
	},

/*
 * The following commands are available in debug mode only.
 */

#define DI_OP_READMEM		(DI_OP_ENABLE2+1)
	[DI_OP_READMEM] = {
		.op = DI_OP(DI_OP_READMEM, DI_DIR_READ | DI_MODE_IMMED),
		.name = "READMEM",
		.cmdbuf0 = 0xfe010000,
	},

#define DI_OP_WRITEMEM		(DI_OP_READMEM+1)
	[DI_OP_WRITEMEM] = {
		.op = DI_OP(DI_OP_WRITEMEM, DI_DIR_READ | DI_MODE_DMA),
		.name = "WRITEMEM",
		.cmdbuf0 = 0xfe010100,
	},

#define DI_OP_FUNC		(DI_OP_WRITEMEM+1)
	[DI_OP_FUNC] = {
		.op = DI_OP(DI_OP_FUNC, DI_DIR_READ | DI_MODE_IMMED),
		.name = "FUNC",
		.cmdbuf0 = 0xfe120000,
	},

#define DI_OP_GETSTATUS		(DI_OP_FUNC+1)
	[DI_OP_GETSTATUS] = {
		.op = DI_OP(DI_OP_GETSTATUS, DI_DIR_READ | DI_MODE_IMMED),
		.name = "GETSTATUS",
		.cmdbuf0 = 0xe0000000,
	},

/* thanks to blackcheck for pointing this one */
#define DI_OP_SPINMOTOR		(DI_OP_GETSTATUS+1)
	[DI_OP_SPINMOTOR] = {
		.op = DI_OP(DI_OP_SPINMOTOR, DI_DIR_READ | DI_MODE_IMMED),
		.name = "SPINMOTOR",
		.cmdbuf0 = 0xfe110001,
#define DI_SPINMOTOR_MASK	0x0000ff00
#define DI_SPINMOTOR_DOWN	0x00000000
#define DI_SPINMOTOR_UP		0x00000100
#define DI_SPINMOTOR_CHECKDISK	0x00008000
	},

/*
 * The following commands are part of the firmware extensions.
 */

#define DI_OP_SETSTATUS		(DI_OP_SPINMOTOR+1)
	[DI_OP_SETSTATUS] = {
		.op = DI_OP(DI_OP_SETSTATUS, DI_DIR_READ | DI_MODE_IMMED),
		.name = "SETSTATUS",
		.cmdbuf0 = 0xee000000,
#define DI_SETSTATUS_MASK	0x00ff0000
#define DI_SETSTATUS_SHIFT	16
	},

#define DI_OP_ENABLEEXTENSIONS	(DI_OP_SETSTATUS+1)
	[DI_OP_ENABLEEXTENSIONS] = {
		.op = DI_OP(DI_OP_ENABLEEXTENSIONS, DI_DIR_READ|DI_MODE_IMMED|
							DI_IGNORE_ERRORS),
		.name = "ENABLEEXTENSIONS",
		.cmdbuf0 = 0x55000000,
#define DI_ENABLEEXTENSIONS_MASK	0x00ff0000
#define DI_ENABLEEXTENSIONS_SHIFT	16
	},

#define DI_OP_MAXOP		DI_OP_ENABLEEXTENSIONS
};

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

/*
 * Builds a "Read Sector" command.
 */
static void di_op_readsector(struct di_command *cmd,
			     struct di_device *ddev,
			     u32 sector, void *data, size_t len)
{
	di_op_basic(cmd, ddev, DI_OP_READSECTOR);
	cmd->cmdbuf1 = sector;
	cmd->cmdbuf2 = len;
	cmd->data = data;
	cmd->len = len;
	cmd->max_retries = cmd->retries = DI_COMMAND_RETRIES;
}

/*
 * Builds the first enable command.
 */
static void di_op_enable1(struct di_command *cmd, struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_ENABLE1);
	cmd->cmdbuf0 = CMDBUF(0xff, 0x01, 'M', 'A');
	cmd->cmdbuf1 = CMDBUF('T', 'S', 'H', 'I');
	cmd->cmdbuf2 = CMDBUF('T', 'A', 0x02, 0x00);
}

/*
 * Builds the second enable command.
 */
static void di_op_enable2(struct di_command *cmd, struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_ENABLE2);
	cmd->cmdbuf0 = CMDBUF(0xff, 0x00, 'D', 'V');
	cmd->cmdbuf1 = CMDBUF('D', '-', 'G', 'A');
	cmd->cmdbuf2 = CMDBUF('M', 'E', 0x03, 0x00);
}

/*
 * Builds a "Read Memory" command.
 * Requires debug mode enabled.
 */
static inline void di_op_readmem(struct di_command *cmd,
				 struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_READMEM);
	cmd->cmdbuf2 = 0x00010000;
}

/*
 * Builds a "Invoke func" command.
 * Requires debug mode enabled.
 */
static inline void di_op_func(struct di_command *cmd,
			      struct di_device *ddev, u32 address)
{
	di_op_basic(cmd, ddev, DI_OP_FUNC);
	cmd->cmdbuf1 = address;
	cmd->cmdbuf2 = CMDBUF('f', 'u', 'n', 'c');
}

/*
 * Builds a "Write Memory" command.
 * Requires debug mode enabled.
 */
static inline void di_op_writemem(struct di_command *cmd,
				 struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_WRITEMEM);
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
 * Builds a "spin motor" command.
 * Requires debug mode enabled.
 */
static void di_op_spinmotor(struct di_command *cmd,
			    struct di_device *ddev, u32 flags)
{
	di_op_basic(cmd, ddev, DI_OP_SPINMOTOR);
	cmd->cmdbuf0 |= (flags & DI_SPINMOTOR_MASK);
}

/*
 * Builds a "set drive status" command.
 * Requires debug mode enabled.
 */
static void di_op_setstatus(struct di_command *cmd,
			    struct di_device *ddev, u8 status)
{
	di_op_basic(cmd, ddev, DI_OP_SETSTATUS);
	cmd->cmdbuf0 |= ((status << DI_SETSTATUS_SHIFT) & DI_SETSTATUS_MASK);
}

/*
 * Builds a "enable extensions" command.
 * The extended firmware will transparently disable the extensions when
 * original media is found.
 * Requires debug mode enabled.
 */
static void di_op_enableextensions(struct di_command *cmd,
				  struct di_device *ddev, u8 enable)
{
	di_op_basic(cmd, ddev, DI_OP_ENABLEEXTENSIONS);
	cmd->cmdbuf0 |= ((enable << DI_ENABLEEXTENSIONS_SHIFT) &
			DI_ENABLEEXTENSIONS_MASK);
}

/*
 * Builds a customized command.
 */
static inline void di_op_custom(struct di_command *cmd,
				struct di_device *ddev,
				struct di_opcode *opcode)
{
	di_op_basic(cmd, ddev, DI_OP_NOP);
	cmd->opidx = DI_OP_CUSTOM;
	cmd->data = opcode;
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

/*
 * Prints the given drive status, only if debug enabled.
 */
static inline void di_debug_print_drive_status(u32 drive_status)
{
	DBG("%08x, [%s, %s]\n", drive_status,
	    di_printable_status(drive_status),
	    di_printable_error(drive_status));
}

/*
 * Prints the given drive status.
 */
static void di_print_drive_status(u32 drive_status)
{
	drv_printk(KERN_INFO, "drive_status=%08x, [%s, %s]\n", drive_status,
		   di_printable_status(drive_status),
		   di_printable_error(drive_status));
}

/*
 * Prints the given disk identifier.
 */
static void di_print_disk_id(struct di_disk_id *disk_id)
{
	drv_printk(KERN_INFO, "disk_id = [%s]\n", disk_id->id);
}

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

	if ((op & DI_DIR_WRITE))
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
	out_be32(io_base + DI_CR, DI_CR_TSTART | DI_CR_DMA | (mode&0x4));
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
		DBG("dvd stuck!\n");
		di_reset(ddev);
	}

	/* ack and enable the Transfer Complete interrupt */
	spin_lock_irqsave(&ddev->io_lock, flags);
	out_be32(sr_reg, in_be32(sr_reg) | (DI_SR_TCINT|DI_SR_TCINTMASK));
	spin_unlock_irqrestore(&ddev->io_lock, flags);

	return;
}

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

	/*DBG("buf0 = 0x%08x, buf1 = 0x%08x, buf2 = 0x%08x\n",
	    cmd->cmdbuf0, cmd->cmdbuf1, cmd->cmdbuf2);*/

	out_be32(io_base + DI_CMDBUF0, cmd->cmdbuf0);
	out_be32(io_base + DI_CMDBUF1, cmd->cmdbuf1);
	out_be32(io_base + DI_CMDBUF2, cmd->cmdbuf2);

	cmd->ddev->drive_status = 0;

	if (tstart)
		out_be32(io_base + DI_CR, DI_CR_TSTART | (opcode->op & 0x6));
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

	ddev->cmd = cmd;
	cmd->dma_len = cmd->len;
	cmd->dma_addr = dma_map_single(ddev->dev,
				       cmd->data, cmd->len,
				       di_opidx_to_dma_dir(cmd));

	di_prepare_command(cmd, 0);
	di_start_dma_transfer_raw(ddev, cmd->dma_addr, cmd->dma_len,
				  di_op(cmd) & DI_DIR_WRITE);

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
	}

	opcode = di_get_opcode(cmd);

	/*
	 * If a command fails we check the drive status. Depending on that
	 * we may or not retry later the command.
	 */
	cmd->result = result;
	if (!di_command_ok(cmd)) {
		/* the MATSHITA command always reports failure, ignore it */
		if (DI_OP_ID(opcode->op) != DI_OP_ENABLE1) {
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
		}
	} else {
		if (cmd->retries != cmd->max_retries) {
			DBG("command %s succeeded after %d retries :-)\n",
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
				DBG("command %s failed, %d retries left\n",
				    opcode->name, cmd->retries);
				di_debug_print_drive_status(drive_status);

				cmd->retries--;
				di_run_command(cmd);
				goto out;
			} else {
				DBG("command %s failed,"
				    " aborting due to drive status\n",
				    opcode->name);
			}
		} else {
			if (!(opcode->op & DI_IGNORE_ERRORS))
				DBG("command %s failed\n", opcode->name);
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
	/* if specified, call the completion routine */
	if (cmd->done)
		cmd->done(cmd);
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

		if (reason & DI_SR_TCINT)
			di_complete_transfer(ddev, DI_SR_TCINT);
		if (reason & DI_SR_BRKINT) {
			DBG("BRKINT\n");
			di_complete_transfer(ddev, DI_SR_BRKINT);
		}
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
		if (test_and_clear_bit(__DI_RESETTING, &ddev->flags)) {
			if (!test_bit(__DI_AVOID_DEBUG, &ddev->flags)) {
				if (ddev->flags & DI_INTEROPERABLE) {
					DBG("extensions loaded"
					    " and hopefully working\n");
				}
			}
		} else {
			DBG("dvd cover interrupt\n");
		}
	}

	spin_unlock_irqrestore(&ddev->io_lock, flags);

	return IRQ_HANDLED;
}

/*
 * Hard-resets the drive.
 */
static void di_reset(struct di_device *ddev)
{
	u32 __iomem *reset_reg = (u32 __iomem *)0xcc003024;
	u32 reset;

#define FLIPPER_RESET_DVD 0x00000004

	/* set flags, but preserve the alien firmware flag */
	ddev->flags = (ddev->flags & DI_AVOID_DEBUG) |
		      DI_RESETTING | DI_MEDIA_CHANGED;

	reset = in_be32(reset_reg);
	out_be32(reset_reg, (reset & ~FLIPPER_RESET_DVD) | 1);
	mdelay(500);
	out_be32(reset_reg, (reset | FLIPPER_RESET_DVD) | 1);
	mdelay(500);

	DBG("drive reset\n");
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
	struct di_command cmd;

	memset(&di_drive_info, 0, sizeof(di_drive_info));
	di_op_inq(&cmd, ddev, &di_drive_info);
	di_run_command_and_wait(&cmd);

	drv_printk(KERN_INFO, "laser unit: rev=%x, code=%x, date=%x\n",
		   di_drive_info.rev, di_drive_info.code,
		   di_drive_info.date);

	ddev->model = di_drive_info.date;
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
 *
 * Firmware handling.
 */

/*
 * Reads a long word from drive addressable memory.
 * Requires debug mode enabled.
 */
static int di_fw_read_meml(struct di_device *ddev,
			   unsigned long *data, unsigned long address)
{
	void __iomem *io_base = ddev->io_base;
	struct di_command cmd;
	int result = -1;

	di_op_readmem(&cmd, ddev);
	cmd.cmdbuf1 = address;
	di_run_command_and_wait(&cmd);
	if (di_command_ok(&cmd)) {
		*data = in_be32(io_base + DI_DATA);
		result = 0;
	}
	return result;
}

/*
 * Retrieves the current active interrupt handler for the drive.
 * Requires debug mode enabled.
 */
static unsigned long di_fw_get_irq_handler(struct di_device *ddev)
{
	unsigned long data = 0;

	di_fw_read_meml(ddev, &data, DI_DRIVE_IRQ_VECTOR);
	return data;
}

/*
 * Patches drive addressable memory.
 * Requires debug mode enabled.
 */
static void di_fw_patch_mem(struct di_device *ddev, u32 address,
			    void *data, size_t len)
{
	struct di_command cmd;
	struct di_opcode opcode;
	int chunk_size;
	const int max_chunk_size = 3 * sizeof(cmd.cmdbuf0);

	while (len > 0) {
		/* we can write in groups of 12 bytes at max */
		if (len > max_chunk_size)
			chunk_size = max_chunk_size;
		else
			chunk_size = len;

		/* prepare for writing to drive's memory ... */
		di_op_writemem(&cmd, ddev);
		cmd.cmdbuf1 = address;
		cmd.cmdbuf2 = chunk_size << 16;
		di_run_command_and_wait(&cmd);
		if (!di_command_ok(&cmd))
			break;

		/* ... and actually write to it */
		opcode.op = DI_OP(DI_OP_CUSTOM, DI_DIR_READ | DI_MODE_IMMED);
		opcode.name = "custom write";
		di_op_custom(&cmd, ddev, &opcode);
		memcpy(&cmd.cmdbuf0, data, chunk_size);
		di_run_command(&cmd);

		/*
		 * We can't rely on drive operating as expected here, so we
		 * explicitly poll for end of transfer and timeout eventually.
		 * Anyway, we assume everything was ok.
		 */
		di_wait_for_dma_transfer_raw(ddev);
		di_complete_transfer(ddev, DI_SR_TCINT);

		/* ok, next chunk */
		address += chunk_size;
		data += chunk_size;
		len -= chunk_size;
	}
}

/*
 * Runs a series of patches.
 * Requires debug mode enabled.
 */
static void di_fw_patch(struct di_device *ddev,
			struct di_drive_code *section, int nr_sections)
{
	while (nr_sections > 0) {
		di_fw_patch_mem(ddev, section->address,
				section->code, section->len);
		section++;
		nr_sections--;
	}
}

/*
 * Selects the appropiate drive code for each drive.
 */
static int di_select_drive_code(struct di_device *ddev)
{
	ddev->drive_code = NULL;

	switch (ddev->model) {
	case 0x20020402:
		ddev->drive_code = &drive_20020402;
		break;
	case 0x20010608:
		ddev->drive_code = &drive_20010608;
		break;
	case 0x20020823:
		ddev->drive_code = &drive_20020823;
		break;
	case 0x20010831:
		ddev->drive_code = &drive_20010831;
		break;
	default:
		drv_printk(KERN_ERR, "sorry, drive %x is not yet"
			   " supported\n",
			   di_drive_info.date);
		break;
	}

	return (ddev->drive_code) ? 0 : -EINVAL;
}

/*
 *
 */
static u8 parking_code[] = {
	0xa0,				/* sub	d0, d0 */
	0xc4, 0xda, 0xfc,		/* movb	d0, (ADBCTL) */
	0xf4, 0x74, 0x74, 0x0a, 0x08,	/* mov	0x080a74, a0 */ /* fixup */
	0xf7, 0x20, 0x4c, 0x80,		/* mov	a0, (0x804c) */
	0xfe,				/* rts */
};

/*
 * Parks (disables) any existing alien drive code.
 * Requires debug mode enabled.
 */
static u32 di_park_firmware(struct di_device *ddev)
{
	struct di_command cmd;
	u32 irq_handler, original_irq_handler;
	u32 load_address;

	/* calculate an appropiate load address for the parking code */
	irq_handler = le32_to_cpu(di_fw_get_irq_handler(ddev));
	load_address = (irq_handler >= 0x400000) ? 0x008502 : 0x40c600;

	/* get the original interrupt handler */
	irq_handler = (ddev->model != 0x20010831) ? 0x00080A74 : 0x00080AA4;
	original_irq_handler = irq_handler;

	/* fix the parking code to match our drive model */
	cpu_to_le32s(&irq_handler);
	memcpy(parking_code + 6, &irq_handler, 3);

	/* load and call it */
	di_fw_patch_mem(ddev, load_address, parking_code, sizeof(parking_code));
	di_op_func(&cmd, ddev, load_address);
	di_run_command_and_wait(&cmd);

	/*
	 * Check if the parking code did its job.
	 * The drive should be running now under the original irq handler.
	 */
	irq_handler = le32_to_cpu(di_fw_get_irq_handler(ddev));
	if (irq_handler != original_irq_handler) {
		drv_printk(KERN_ERR, "parking failed!\n");
		di_reset(ddev);
	} else {
		DBG("parking done, irq handler = %08x\n", irq_handler);
	}

	/* drive is not patched anymore here */
	clear_bit(__DI_INTEROPERABLE, &ddev->flags);

	return di_get_drive_status(ddev);
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
 * Enables the "debug" command set.
 */
static int di_enable_debug_commands(struct di_device *ddev)
{
	struct di_command cmd;

	/* send these two consecutive enable commands */
	di_op_enable1(&cmd, ddev);
	di_run_command_and_wait(&cmd);
	di_op_enable2(&cmd, ddev);
	return di_run_command_and_wait(&cmd);
}

/*
 * Tries to determine if firmware extensions are currently installed.
 * Requires debug mode enabled.
 */
static int di_has_alien_drive_code(struct di_device *ddev)
{
	unsigned long address;
	int result = 1;

	/*
	 * We assume that alien drive code is in place if the interrupt handler
	 * is not pointing to ROM address space.
	 */
	address = di_fw_get_irq_handler(ddev);
	address = le32_to_cpu(address);
	if (address) {
		if ((address & 0xffff0000) == 0x00080000)
			result = 0;
	}
	return result;
}

/*
 * Enables some workarounds and/or special behaviours depending on
 * the drive firmware found.
 * Requires debug mode enabled.
 */
static void di_init_alien_drive_code_quirks(struct di_device *ddev)
{
	unsigned long fingerprint;

	/*
	 * Test if a xenogc/duoq is installed.
	 */
	if (!di_fw_read_meml(ddev, &fingerprint, 0x40c60a)) {
		if (fingerprint == 0xf710fff7) {
			drv_printk(KERN_INFO, "drivechip: xenogc/duoq\n");
			set_bit(__DI_AVOID_DEBUG, &ddev->flags);
		}
	}
}

/*
 * Configures the drive to accept DVD-R and DVD+R media.
 */
static void di_make_interoperable(struct di_device *ddev)
{
	struct di_command cmd;

	if (!ddev->drive_code || test_bit(__DI_AVOID_DEBUG, &ddev->flags))
		return;

	/* calm things down */
	di_spin_down_drive(ddev);

	/* disable any alien drive code */
	di_enable_debug_commands(ddev);
	di_park_firmware(ddev);

	/* re-enable debug commands */
	di_enable_debug_commands(ddev);

	/* load our own drive code extensions */
	di_fw_patch(ddev, ddev->drive_code, 1);

	/*
	 * The drive will become interoperable now.
	 * Here we go...!
	 */
	set_bit(__DI_INTEROPERABLE, &ddev->flags);
	di_op_func(&cmd, ddev, DI_DRIVE_CODE_BASE);
	di_run_command_and_wait(&cmd);

	/* this checks if drive is still working... */
	di_get_drive_status(ddev);
}

/*
 * Ensures that the debug features of the drive firmware are working.
 */
static int di_probe_debug_features(struct di_device *ddev)
{
	int result;

	/* do nothing on unknown drive models */
	if (!ddev->drive_code)
		return -EINVAL;

	result = di_enable_debug_commands(ddev);
	if (!di_result_ok(result)) {
		DBG("uhmm, debug commands seem banned...\n");
		DBG("... let's hard reset the drive!\n");

		di_reset(ddev);
		result = di_enable_debug_commands(ddev);
	}

	return di_result_ok(result) ? 0 : -EINVAL;
}

/*
 * Probes the existing firmware features and determines the best operation
 * mode.
 */
static void di_probe_firmware(struct di_device *ddev)
{
	if (di_probe_debug_features(ddev)) {
		/* we can't use debug features, thus try to avoid them */
		drv_printk(KERN_INFO, "firmware: debug features do not work,"
			   " using standard command set\n");
		set_bit(__DI_AVOID_DEBUG, &ddev->flags);
	} else {
		if (di_has_alien_drive_code(ddev)) {
			drv_printk(KERN_INFO, "firmware: patched drive\n");
			/* enable some workarounds if required */
			di_init_alien_drive_code_quirks(ddev);
		} else {
			drv_printk(KERN_INFO, "firmware: unpatched drive\n");
		}
	}
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
static void di_spin_up_drive(struct di_device *ddev, u8 enable_extensions)
{
	struct di_command cmd;
	u32 drive_status;
	unsigned int attempts = 2;

	/* do nothing if the drive is already spinning */
	if (di_is_drive_ready(ddev))
		goto out;

	if (test_bit(__DI_AVOID_DEBUG, &ddev->flags)) {
		/* don't use debug commands, let's hope a drivechip is there */
		di_reset(ddev);
	} else {
		while (attempts-- > 0) {
			if (!test_bit(__DI_INTEROPERABLE, &ddev->flags))
				di_make_interoperable(ddev);

			/*
			 * We only re-enable the extensions if the drive is not
			 * in a pending read disk id state. Otherwise, we assume
			 * that the drive has already accepted the disk.
			 */
			drive_status = di_get_drive_status(ddev);
			if (DI_STATUS(drive_status) !=
			    DI_STATUS_DISK_ID_NOT_READ) {
				di_op_enableextensions(&cmd, ddev,
						       enable_extensions);
				di_run_command_and_wait(&cmd);
			}

			/* the spin motor command requires the debug mode */
			di_enable_debug_commands(ddev);
			di_op_spinmotor(&cmd, ddev, DI_SPINMOTOR_UP);
			di_run_command_and_wait(&cmd);

			if (!ddev->drive_status) {
				di_op_setstatus(&cmd, ddev,
						DI_STATUS_DISK_ID_NOT_READ+1);
				cmd.cmdbuf0 |= 0x00000300; /* XXX cheqmate */
				di_run_command_and_wait(&cmd);
			} else {
				if (DI_ERROR(ddev->drive_status) !=
				    DI_ERROR_MEDIUM_NOT_PRESENT)
					di_reset(ddev);
					continue;
			}
			break;
		}
	}
out:
	return;
}


/*
 * Block layer hooks.
 *
 */

static int di_read_toc(struct di_device *ddev)
{
	static struct di_disk_id disk_id
			 __attribute__ ((aligned(DI_DMA_ALIGN+1)));
	struct di_command cmd;
	int accepted_media = 0;
	int retval = 0;
	const u8 enable_extensions = 1;

	di_cancel_motor_off(ddev);

	/* spin up the drive if needed */
	if ((ddev->flags & DI_MEDIA_CHANGED))
		di_spin_up_drive(ddev, enable_extensions);

	/* check that disk id can be read and that the media is appropiate */
	memset(&disk_id, 0, sizeof(disk_id));
	di_op_readdiskid(&cmd, ddev, &disk_id);
	di_run_command_and_wait(&cmd);
	if (di_command_ok(&cmd)) {
		if (disk_id.id[0] && memcmp(disk_id.id, "GBL", 3) &&
		    !di_accept_gods) {
			di_print_disk_id(&disk_id);
			drv_printk(KERN_ERR, "sorry, gamecube media"
				   " support is disabled\n");
		} else {
			accepted_media = 1;
		}
	} else {
		set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
	}

	if (accepted_media) {
		/*
		 * This is currently hardcoded. Scream|CT got this number
		 * by reading up to where the lens physically allowed.
		 *
		 * This is currently causing us problems.
		 * For example, recent 'mount' versions will read 4k from
		 * the end of the device when guessing filesystem types.
		 * The end of device we are reporting is not the real one,
		 * so the drive will fail to read that part if it was not
		 * burned.
		 *
		 * As a temporary solution, specify always a filesystem
		 * type when using mount, or fill the whole disk when
		 * burning.
		 */
		ddev->nr_sectors = DI_MAX_SECTORS; /* in DVD sectors */
		clear_bit(__DI_MEDIA_CHANGED, &ddev->flags);

		DBG("media ready for operation\n");
	} else {
		ddev->nr_sectors = 0;
		retval = -ENOMEDIUM;

		di_spin_down_drive(ddev);

		DBG("media NOT ready\n");
	}

	/* transform to kernel sectors */
	ddev->nr_sectors <<= (DI_SECTOR_SHIFT - KERNEL_SECTOR_SHIFT);
	set_capacity(ddev->disk, ddev->nr_sectors);

	return retval;
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
		if (rq_data_dir(req) == WRITE) {
			drv_printk(KERN_ERR, "write attempted\n");
			goto done;
		}

		/* it is not a good idea to open the lid ... */
		if ((ddev->flags & DI_MEDIA_CHANGED)) {
			drv_printk(KERN_ERR, "media changed, aborting\n");
			goto done;
		}

		/* keep our reads within limits */
		if (blk_rq_pos(req) +
				 blk_rq_cur_sectors(req) > ddev->nr_sectors) {
			drv_printk(KERN_ERR, "reading past end\n");
			goto done;
		}

		ddev->req = req;
		spin_unlock_irqrestore(&ddev->lock, flags);

		/* launch the corresponding read sector command */
		start = blk_rq_pos(req) << KERNEL_SECTOR_SHIFT;
		len = blk_rq_cur_bytes(req);

		di_op_readsector(cmd, ddev, start >> 2,
				 req->buffer, len);
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
	switch (cmd) {
	case CDROMMULTISESSION:
		/* struct cdrom_multisession */
		break;
	case CDROMSTART:
		break;
	case CDROMSTOP:
		break;
	case CDROMREADTOCHDR:
		/* struct cdrom_tochdr */
		break;
	case CDROMREADTOCENTRY:
		/* struct cdrom_tocentry */
		break;
	case CDROMREADMODE2:
	case CDROMREADMODE1:
	case CDROMREADRAW:
		/* struct cdrom_read (1-2048, 2-2336,RAW-2352) */
		break;
	case CDROM_GET_MCN:
		/* retrieve the universal product code */
		/* struct cdrom_mcn */
		break;
	case CDROMRESET:
		/* reset the drive */
		break;
	case BLKRAGET:
	case BLKFRAGET:
	case BLKROGET:
	case BLKBSZGET:
	case BLKSSZGET:
	case BLKSECTGET:
	case BLKGETSIZE:
	case BLKGETSIZE64:
	case BLKFLSBUF:
		return ioctl_by_bdev(bdev, cmd, arg);
	default:
		return -EINVAL;
	}
	return -EINVAL;
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
		drv_printk(KERN_ERR, "request of irq%d failed\n", ddev->irq);
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
	di_select_drive_code(ddev);

	di_probe_firmware(ddev);

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
		drv_printk(KERN_ERR, "error registering major %d\n", DI_MAJOR);
		goto err_register_blkdev;
	}

	retval = -ENOMEM;
	spin_lock_init(&ddev->queue_lock);
	queue = blk_init_queue(di_do_request, &ddev->queue_lock);
	if (!queue) {
		drv_printk(KERN_ERR, "error initializing queue\n");
		goto err_blk_init_queue;
	}

	blk_queue_logical_block_size(queue, DI_SECTOR_SIZE);
	blk_queue_dma_alignment(queue, DI_DMA_ALIGN);
	blk_queue_max_phys_segments(queue, 1);
	blk_queue_max_hw_segments(queue, 1);
	queue->queuedata = ddev;
	ddev->queue = queue;

	disk = alloc_disk(1);
	if (!disk) {
		drv_printk(KERN_ERR, "error allocating disk\n");
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
		drv_printk(KERN_ERR, "failed to allocate di_device\n");
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
	int retval;

	retval = of_address_to_resource(odev->node, 0, &res);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return -ENODEV;
	}

	return di_do_probe(&odev->dev,
			   &res, irq_of_parse_and_map(odev->node, 0));
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
	{ .compatible = "nintendo,flipper-disk" },
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
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   di_driver_version);

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
