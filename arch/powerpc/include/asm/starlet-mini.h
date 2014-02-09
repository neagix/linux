/*
 * arch/powerpc/include/asm/starlet-mini.h
 *
 * Definitions for the 'mini' firmware replacement for Starlet
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __STARLET_MINI_H
#define __STARLET_MINI_H

#ifdef CONFIG_STARLET_MINI

/*
 * mini ipc call numbering scheme
 */

#define _MIPC_FAST        0x01
#define _MIPC_SLOW        0x00

#define _MIPC_DEV_SYS     0x00
#define _MIPC_DEV_NAND    0x01
#define _MIPC_DEV_SDHC    0x02
#define _MIPC_DEV_KEYS    0x03
#define _MIPC_DEV_AES     0x04
#define _MIPC_DEV_BOOT2   0x05
#define _MIPC_DEV_PPC     0x06
#define _MIPC_DEV_SDMMC   0x07

#define _MIPC_SYS_PING    0x0000
#define _MIPC_SYS_JUMP    0x0001
#define _MIPC_SYS_GETVERS 0x0002
#define _MIPC_SYS_GETGITS 0x0003
#define _MIPC_SYS_WRITE32 0x0100
#define _MIPC_SYS_WRITE16 0x0101
#define _MIPC_SYS_WRITE8  0x0102
#define _MIPC_SYS_READ32  0x0103
#define _MIPC_SYS_READ16  0x0104
#define _MIPC_SYS_READ8   0x0105
#define _MIPC_SYS_SET32   0x0106
#define _MIPC_SYS_SET16   0x0107
#define _MIPC_SYS_SET8    0x0108
#define _MIPC_SYS_CLEAR32 0x0109
#define _MIPC_SYS_CLEAR16 0x010a
#define _MIPC_SYS_CLEAR8  0x010b
#define _MIPC_SYS_MASK32  0x010c
#define _MIPC_SYS_MASK16  0x010d
#define _MIPC_SYS_MASK8   0x010e

#define _MIPC_NAND_RESET  0x0000
#define _MIPC_NAND_GETID  0x0001
#define _MIPC_NAND_READ   0x0002
#define _MIPC_NAND_WRITE  0x0003
#define _MIPC_NAND_ERASE  0x0004
#define _MIPC_NAND_STATUS 0x0005

#define _MIPC_SDHC_DISCOVER 0x0000
#define _MIPC_SDHC_EXIT	    0x0001

#define _MIPC_SDMMC_ACK   0x0000
#define _MIPC_SDMMC_READ  0x0001
#define _MIPC_SDMMC_WRITE 0x0002
#define _MIPC_SDMMC_STATE 0x0003
#define _MIPC_SDMMC_SIZE  0x0004

#define _MIPC_KEYS_GETOTP 0x0000
#define _MIPC_KEYS_GETEEP 0x0001

#define _MIPC_AES_RESET   0x0000
#define _MIPC_AES_SETIV   0x0001
#define _MIPC_AES_SETKEY  0x0002
#define _MIPC_AES_DECRYPT 0x0003

#define _MIPC_BOOT2_RUN   0x0000
#define _MIPC_BOOT2_TMD   0x0001

#define _MIPC_PPC_BOOT    0x0000


/*
 *
 */

#define _MIPC_MODEBITS	8
#define _MIPC_DEVBITS	8
#define _MIPC_NRBITS	16

#define _MIPC_MODEMASK	((1 << _MIPC_MODEBITS)-1)
#define _MIPC_DEVMASK	((1 << _MIPC_DEVBITS)-1)
#define _MIPC_NRMASK	((1 << _MIPC_NRBITS)-1)

#define _MIPC_MODESHIFT	(_MIPC_DEVSHIFT + _MIPC_DEVBITS)
#define _MIPC_DEVSHIFT	(_MIPC_NRSHIFT + _MIPC_NRBITS)
#define _MIPC_NRSHIFT	0

#define _MIPC(mode, dev, nr) \
	(((mode) << _MIPC_MODESHIFT) | \
	 ((dev) << _MIPC_DEVSHIFT) | \
	 ((nr) << _MIPC_NRSHIFT))

#define _MIPC_FAST_SYS(nr)	_MIPC(_MIPC_FAST, _MIPC_DEV_SYS, nr)

#define MIPC_SYS_PING		_MIPC_FAST_SYS(_MIPC_SYS_PING)
#define MIPC_SYS_WRITE32	_MIPC_FAST_SYS(_MIPC_SYS_WRITE32)
#define MIPC_SYS_WRITE16	_MIPC_FAST_SYS(_MIPC_SYS_WRITE16)
#define MIPC_SYS_WRITE8		_MIPC_FAST_SYS(_MIPC_SYS_WRITE8)
#define MIPC_SYS_READ32		_MIPC_FAST_SYS(_MIPC_SYS_READ32)
#define MIPC_SYS_READ16		_MIPC_FAST_SYS(_MIPC_SYS_READ16)
#define MIPC_SYS_READ8		_MIPC_FAST_SYS(_MIPC_SYS_READ8)
#define MIPC_SYS_SET32		_MIPC_FAST_SYS(_MIPC_SYS_SET32)
#define MIPC_SYS_SET16		_MIPC_FAST_SYS(_MIPC_SYS_SET16)
#define MIPC_SYS_SET8		_MIPC_FAST_SYS(_MIPC_SYS_SET8)
#define MIPC_SYS_CLEAR32	_MIPC_FAST_SYS(_MIPC_SYS_CLEAR32)
#define MIPC_SYS_CLEAR16	_MIPC_FAST_SYS(_MIPC_SYS_CLEAR16)
#define MIPC_SYS_CLEAR8		_MIPC_FAST_SYS(_MIPC_SYS_CLEAR8)
#define MIPC_SYS_MASK32		_MIPC_FAST_SYS(_MIPC_SYS_MASK32)
#define MIPC_SYS_MASK16		_MIPC_FAST_SYS(_MIPC_SYS_MASK16)
#define MIPC_SYS_MASK8		_MIPC_FAST_SYS(_MIPC_SYS_MASK8)

#define MIPC_REQ_MAX_ARGS	6

struct mipc_infohdr {
	char magic[3];
	u8 version;
	phys_addr_t mem2_boundary;
	phys_addr_t ipc_in;
	size_t ipc_in_size;
	phys_addr_t ipc_out;
	size_t ipc_out_size;
};

struct mipc_device;
struct mipc_req;

extern int mipc_discover(struct mipc_infohdr **hdrp);

extern u32 mipc_in_be32(const volatile u32 __iomem *addr);
extern u16 mipc_in_be16(const volatile u16 __iomem *addr);
extern u8 mipc_in_8(const volatile u8 __iomem *addr);

extern void mipc_out_be32(const volatile u32 __iomem *addr, u32 val);
extern void mipc_out_be16(const volatile u16 __iomem *addr, u16 val);
extern void mipc_out_8(const volatile u8 __iomem *addr, u8 val);

extern void mipc_clear_bit(int nr, volatile unsigned long *addr);
extern void mipc_set_bit(int nr, volatile unsigned long *addr);
extern void mipc_clrsetbits_be32(const volatile u32 __iomem *addr,
				 u32 clear, u32 set);

extern void mipc_wmb(void);

extern void __iomem *mipc_ioremap(phys_addr_t addr, unsigned long size);
extern void mipc_iounmap(volatile void __iomem *addr);

#else

struct mipc_infohdr;

static inline int mipc_discover(struct mipc_infohdr **hdrp)
{
	return -ENODEV;
}

#endif /* CONFIG_STARLET_MINI */

#endif /* __STARLET_MINI_H */

