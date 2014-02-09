/*
 * arch/powerpc/platforms/embedded6xx/starlet-mipc.c
 *
 * IPC driver for the 'mini' firmware replacement for Starlet
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */
#define DRV_MODULE_NAME		"starlet-mipc"
#define pr_fmt(fmt) DRV_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/delay.h>	/* for mdelay() */
#include <asm/pgtable.h>	/* for _PAGE_KERNEL_NC */
#include <asm/time.h>		/* for get_tbl() */
#include <asm/starlet-mini.h>

#include "hlwd-pic.h"


#define DRV_DESCRIPTION		"IPC driver for 'mini'"
#define DRV_AUTHOR		"Albert Herranz"

static char mipc_driver_version[] = "0.4i";


/*
 * Hardware registers
 */
#define MIPC_TXBUF	0x00	/* data from cpu to starlet */

#define MIPC_CSR		0x04
#define   MIPC_CSR_TXSTART	(1<<0)	/* start transmit */
#define   MIPC_CSR_TBEI		(1<<1)	/* tx buf empty int */
#define   MIPC_CSR_RBFI		(1<<2)	/* rx buf full int */
#define   MIPC_CSR_RXRDY	(1<<3)	/* receiver ready */
#define   MIPC_CSR_RBFIMASK	(1<<4)	/* rx buf full int mask */
#define   MIPC_CSR_TBEIMASK	(1<<5)	/* tx buf empty int mask */

#define MIPC_RXBUF	0x08	/* data from starlet to cpu */


#define MIPC_MIN_VER	1
#define MIPC_MAX_VER	1

#define MIPC_INITIAL_TAG	1

#define MIPC_SYS_IO_TIMEOUT	(250*1000)	/* usecs */
#define MIPC_DEV_TIMEOUT	(10*1000*1000)	/* usecs */


/*
 * Firmware request.
 *
 */
struct mipc_req {
	union {
		struct {
			u8 flags;
			u8 device;
			u16 req;
		};
		u32 code;
	};
	u32 tag;
	u32 args[MIPC_REQ_MAX_ARGS];
} __attribute__ ((packed));

/*
 *
 */
struct mipc_device {
	void __iomem *io_base;
	int irq;

	struct device *dev;

	spinlock_t call_lock;	/* serialize firmware calls */
	spinlock_t io_lock;	/* serialize access to io registers */

	struct mipc_infohdr *hdr;

	struct mipc_req *in_ring;
	size_t in_ring_size;
	volatile u16 intail_idx;

	struct mipc_req *out_ring;
	size_t out_ring_size;
	volatile u16 outhead_idx;

	u32 tag;
};

#define __spin_event_timeout(condition, timeout_usecs, result, __end_tbl) \
	for (__end_tbl = get_tbl() + tb_ticks_per_usec * timeout_usecs;	\
	     !(result = (condition)) && (int)(__end_tbl - get_tbl()) > 0;)

/*
 * Update control and status register.
 */
static inline void mipc_update_csr(void __iomem *io_base, u32 val)
{
	u32 csr;

	csr = in_be32(io_base + MIPC_CSR);
	/* preserve interrupt masks */
	csr &= MIPC_CSR_RBFIMASK | MIPC_CSR_TBEIMASK;
	csr |= val;
	out_be32(io_base + MIPC_CSR, csr);
}

static u16 mipc_peek_outtail(void __iomem *io_base)
{
	return in_be32(io_base + MIPC_RXBUF) & 0xffff;
}

static u16 mipc_peek_inhead(void __iomem *io_base)
{
	return in_be32(io_base + MIPC_RXBUF) >> 16;
}

static u16 mipc_peek_first_intail(void __iomem *io_base)
{
	return in_be32(io_base + MIPC_TXBUF) & 0xffff;
}

static u16 mipc_peek_first_outhead(void __iomem *io_base)
{
	return in_be32(io_base + MIPC_TXBUF) >> 16;
}

static void mipc_poke_intail(struct mipc_device *ipc_dev, u16 val)
{
	void __iomem *io_base = ipc_dev->io_base;
	unsigned long flags;

	spin_lock_irqsave(&ipc_dev->io_lock, flags);
	out_be32(io_base + MIPC_TXBUF,
		 (in_be32(io_base + MIPC_TXBUF) & 0xffff0000) | val);
	spin_unlock_irqrestore(&ipc_dev->io_lock, flags);
}

static void mipc_poke_outhead(struct mipc_device *ipc_dev, u16 val)
{
	void __iomem *io_base = ipc_dev->io_base;
	unsigned long flags;

	spin_lock_irqsave(&ipc_dev->io_lock, flags);
	out_be32(io_base + MIPC_TXBUF,
		 (in_be32(io_base + MIPC_TXBUF) & 0x0000ffff) | val<<16);
	spin_unlock_irqrestore(&ipc_dev->io_lock, flags);
}



static u16 mipc_get_next_intail(struct mipc_device *ipc_dev)
{
	return (ipc_dev->intail_idx + 1) & (ipc_dev->in_ring_size - 1);
}

static u16 mipc_get_next_outhead(struct mipc_device *ipc_dev)
{
	return (ipc_dev->outhead_idx + 1) & (ipc_dev->out_ring_size - 1);
}

static void mipc_print_req(struct mipc_req *req)
{
	int i;

	pr_info("req %pP = {\n", req);
	pr_cont("code = %08X, tag = %08X\n", req->code, req->tag);
	for (i = 0; i < MIPC_REQ_MAX_ARGS; i++)
		pr_cont("arg[%d] = %08X\n", i, req->args[i]);
	pr_cont("}\n");
}

#ifdef DEBUG_RINGS
static void mipc_dump_ring(struct mipc_req *req, size_t count)
{
	int i;

	for (i = 0; i < count; i++)
		pr_devel("%d: %X (%08X)\n", i, req[i].tag, req[i].code);
}
#endif

static void mipc_print_status(struct mipc_device *ipc_dev)
{
	size_t in_size, out_size;

	in_size = ipc_dev->in_ring_size * sizeof(*ipc_dev->in_ring);
	out_size = ipc_dev->out_ring_size * sizeof(*ipc_dev->out_ring);

	pr_info("ppc: intail_idx=%u, outhead_idx=%u\n",
		ipc_dev->intail_idx, ipc_dev->outhead_idx);
	pr_cont("arm: inhead_idx=%u, outtail_idx=%u\n",
		mipc_peek_inhead(ipc_dev->io_base),
		mipc_peek_outtail(ipc_dev->io_base));
	pr_cont("in_ring=%uK@%p, out_ring=%uK@%p\n",
		in_size / 1024, ipc_dev->in_ring,
		out_size / 1024, ipc_dev->out_ring);
}

static int mipc_send_req(struct mipc_device *ipc_dev, unsigned long timeout,
			 struct mipc_req *req)
{
	void __iomem *io_base = ipc_dev->io_base;
	struct mipc_req *firm_req;
	unsigned long ctx;
	int result;
	int error = 0;

	if (mipc_peek_inhead(io_base) == mipc_get_next_intail(ipc_dev)) {
		pr_err("%s queue full\n", "ppc->arm ipc");
		__spin_event_timeout(mipc_peek_inhead(io_base) !=
				     mipc_get_next_intail(ipc_dev),
				     timeout, result, ctx) {
			/* busy wait */
			cpu_relax();
		}
		if (!result) {
			pr_err("%s queue drain timed out\n", "ppc->arm ipc");
			error = -EIO;
			goto out;
		}
	}

	firm_req = ipc_dev->in_ring + ipc_dev->intail_idx;
	*firm_req = *req;
	ipc_dev->intail_idx = mipc_get_next_intail(ipc_dev);
	mipc_poke_intail(ipc_dev, ipc_dev->intail_idx);
	mipc_update_csr(ipc_dev->io_base, MIPC_CSR_TXSTART);
out:
	if (error)
		pr_devel("exit %d\n", error);
	return error;
}

static int __mipc_recv_req(struct mipc_device *ipc_dev, unsigned long timeout,
			   struct mipc_req *req)
{
	void __iomem *io_base = ipc_dev->io_base;
	struct mipc_req *firm_req;
	unsigned long ctx;
	int result;
	int error = 0;

	__spin_event_timeout(mipc_peek_outtail(io_base) != ipc_dev->outhead_idx,
			     timeout, result, ctx) {
		/* busy wait */
		cpu_relax();
	}
	if (mipc_peek_outtail(io_base) == ipc_dev->outhead_idx) {
		error = -EIO;
		goto out;
	}
	firm_req = ipc_dev->out_ring + ipc_dev->outhead_idx;
	*req = *firm_req;
	ipc_dev->outhead_idx = mipc_get_next_outhead(ipc_dev);
	mipc_poke_outhead(ipc_dev, ipc_dev->outhead_idx);
out:
	return error;
}

static int mipc_recv_req(struct mipc_device *ipc_dev, unsigned long timeout,
			 struct mipc_req *req)
{
	int error;

	error = __mipc_recv_req(ipc_dev, timeout, req);
	if (error)
		pr_devel("arm->ppc ipc request timed out (%d)\n", error);
	return error;
}

static int mipc_recv_tagged(struct mipc_device *ipc_dev,
				unsigned long timeout,
				u32 code, u32 tag,
				struct mipc_req *req)
{
	unsigned long ctx;
	int result;
	int error;

	error = mipc_recv_req(ipc_dev, timeout, req);
	if (error)
		goto out;

	__spin_event_timeout(req->code == code && req->tag == tag,
			     timeout, result, ctx) {
		pr_devel("expected: code=%08X, tag=%08X\n", code, tag);
		mipc_print_req(req);
		pr_devel("+++ status\n");
		mipc_print_status(ipc_dev);
#ifdef DEBUG_RINGS
		pr_devel("+++ in_ring\n");
		mipc_dump_ring(ipc_dev->in_ring, ipc_dev->in_ring_size);
		pr_devel("+++ out_ring\n");
		mipc_dump_ring(ipc_dev->out_ring, ipc_dev->out_ring_size);
#endif

		error = mipc_recv_req(ipc_dev, timeout, req);
		if (error)
			goto out;
	}
	if (!result) {
		pr_err("%s: recv timed out\n", __func__);
		error = -EIO;
		goto out;
	} else
		error = 0;

out:
	if (error)
		pr_devel("exit %d\n", error);
	return error;
}

static void __mipc_fill_req(struct mipc_req *req, u32 code)
{
	memset(req, 0, sizeof(*req));
	req->code = code;
}

static int mipc_sendrecv_call(struct mipc_device *ipc_dev,
			      unsigned long timeout,
			      struct mipc_req *req, struct mipc_req *resp)
{
	unsigned long flags;
	int error;

	spin_lock_irqsave(&ipc_dev->call_lock, flags);
	req->tag = ipc_dev->tag++;
	error = mipc_send_req(ipc_dev, timeout, req);
	if (error)
		goto out;
	error = mipc_recv_tagged(ipc_dev, timeout, req->code, req->tag, resp);
out:
	spin_unlock_irqrestore(&ipc_dev->call_lock, flags);

	return error;
}

static int mipc_sendrecv1_call(struct mipc_device *ipc_dev,
			       unsigned long timeout,
			       struct mipc_req *resp, u32 code, u32 arg)
{
	struct mipc_req req;

	__mipc_fill_req(&req, code);
	req.args[0] = arg;
	return mipc_sendrecv_call(ipc_dev, timeout, &req, resp);
}


static int mipc_send_call(struct mipc_device *ipc_dev, unsigned long timeout,
			  struct mipc_req *req)
{
	unsigned long flags;
	int error;

	spin_lock_irqsave(&ipc_dev->call_lock, flags);
	req->tag = ipc_dev->tag++;
	error = mipc_send_req(ipc_dev, timeout, req);
	spin_unlock_irqrestore(&ipc_dev->call_lock, flags);

	return error;
}

static int mipc_send2_call(struct mipc_device *ipc_dev, unsigned long timeout,
			   u32 code, u32 arg1, u32 arg2)
{
	struct mipc_req req;

	__mipc_fill_req(&req, code);
	req.args[0] = arg1;
	req.args[1] = arg2;
	return mipc_send_call(ipc_dev, timeout, &req);
}

static int mipc_send3_call(struct mipc_device *ipc_dev, unsigned long timeout,
			   u32 code, u32 arg1, u32 arg2, u32 arg3)
{
	struct mipc_req req;

	__mipc_fill_req(&req, code);
	req.args[0] = arg1;
	req.args[1] = arg2;
	req.args[2] = arg3;
	return mipc_send_call(ipc_dev, timeout, &req);
}

static int mipc_flush_send(struct mipc_device *ipc_dev, unsigned long timeout)
{
	void __iomem *io_base = ipc_dev->io_base;
	unsigned long ctx;
	int result;
	int error = 0;

	__spin_event_timeout(mipc_peek_inhead(io_base) == ipc_dev->intail_idx,
			     timeout, result, ctx) {
		/* busy wait */
		cpu_relax();
	}
	if (!result) {
		pr_err("%s: flush timed out\n", __func__);
		error = -EIO;
		goto out;
	}
out:
	if (error)
		pr_devel("exit %d\n", error);
	return error;
}

static void mipc_flush_recv(struct mipc_device *ipc_dev,
			    unsigned long timeout)
{
	struct mipc_req req;
	int error;

	do {
		error = __mipc_recv_req(ipc_dev, timeout, &req);
	} while (!error);
}



static struct mipc_device *mipc_device_instance;

struct mipc_device *mipc_get_device(void)
{
	if (!mipc_device_instance)
		pr_err("uninitialized device instance!\n");
	return mipc_device_instance;
}

static int mipc_ping(struct mipc_device *ipc_dev, unsigned long timeout)
{
	struct mipc_req resp;
	int error;

	error = mipc_sendrecv1_call(ipc_dev, timeout, &resp, MIPC_SYS_PING, 0);
	if (error)
		pr_devel("exit %d\n", error);
	return error;
}

#define __declare_ipc_send2_accessor(_name, _suffix, _size, _call) \
void mipc_##_name##_suffix(_size a, void __iomem *addr) \
{									\
	struct mipc_device *ipc_dev = mipc_get_device();		\
	int error;							\
									\
	error = mipc_send2_call(ipc_dev, MIPC_SYS_IO_TIMEOUT, _call,	\
				(u32)addr, a);				\
	if (!error)							\
		return;							\
									\
	pr_devel(__stringify(_name, _suffix) "(%p,%x)\n", addr, a);	\
	BUG();								\
}

#define __declare_ipc_send3_accessor(_name, _suffix, _size, _call) \
void mipc_##_name##_suffix(_size a, _size b, void __iomem *addr) \
{									\
	struct mipc_device *ipc_dev = mipc_get_device();		\
	int error;							\
									\
	error = mipc_send3_call(ipc_dev, MIPC_SYS_IO_TIMEOUT, _call,	\
				(u32)addr, a, b);			\
	if (!error)							\
		return;							\
									\
	pr_devel(__stringify(_name, _suffix) "(%p,%x,%x)\n", addr, a, b);\
	BUG();								\
}

#define __declare_ipc_sendrecv1_accessor(_name, _suffix, _size, _call) \
_size mipc_##_name##_suffix(void __iomem *addr) \
{									\
	struct mipc_device *ipc_dev = mipc_get_device();		\
	struct mipc_req resp;						\
	int error;							\
									\
	error = mipc_sendrecv1_call(ipc_dev, MIPC_SYS_IO_TIMEOUT,	\
				    &resp, _call, (u32)addr);		\
	if (!error)							\
		return resp.args[0];					\
									\
	pr_devel(__stringify(_name, _suffix) "(%p)\n", addr);		\
	BUG();								\
	return 0;							\
}

__declare_ipc_sendrecv1_accessor(read, l, unsigned int, MIPC_SYS_READ32)
__declare_ipc_sendrecv1_accessor(read, w, unsigned short, MIPC_SYS_READ16)
__declare_ipc_sendrecv1_accessor(read, b, unsigned char, MIPC_SYS_READ8)

__declare_ipc_send2_accessor(write, l, unsigned int, MIPC_SYS_WRITE32)
__declare_ipc_send2_accessor(write, w, unsigned short, MIPC_SYS_WRITE16)
__declare_ipc_send2_accessor(write, b, unsigned char, MIPC_SYS_WRITE8)

__declare_ipc_send2_accessor(setbit, l, unsigned int, MIPC_SYS_SET32)
__declare_ipc_send2_accessor(clearbit, l, unsigned int, MIPC_SYS_CLEAR32)
__declare_ipc_send3_accessor(clrsetbits, l, unsigned int, MIPC_SYS_MASK32)

void mipc_wmb(void)
{
	struct mipc_device *ipc_dev = mipc_get_device();
	int error;

	error = mipc_ping(ipc_dev, MIPC_SYS_IO_TIMEOUT);
	if (!error)
		return;

	pr_devel(__stringify(_name, _suffix) "()\n");
	BUG();
}

void __iomem *mipc_ioremap(phys_addr_t addr, unsigned long size)
{
	return (void __iomem *)addr;
}

void mipc_iounmap(volatile void __iomem *addr)
{
	/* nothing to do */
}

/*
 *
 *
 */

#define BITOP_MASK(nr)          (1UL << ((nr) % BITS_PER_LONG))
#define BITOP_WORD(nr)          ((nr) / BITS_PER_LONG)

void mipc_clear_bit(int nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	mipc_clearbitl(mask, p);
}

void mipc_set_bit(int nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	mipc_setbitl(mask, p);
}

void mipc_clrsetbits_be32(const volatile u32 __iomem *addr, u32 clear, u32 set)
{
	mipc_clrsetbitsl(clear, set, (void __iomem *)addr);
}

u32 mipc_in_be32(const volatile u32 __iomem *addr)
{
	return mipc_readl((void __iomem *)addr);
}

void mipc_out_be32(const volatile u32 __iomem *addr, u32 val)
{
	mipc_writel(val, (void __iomem *)addr);
}

u16 mipc_in_be16(const volatile u16 __iomem *addr)
{
	return mipc_readw((void __iomem *)addr);
}

void mipc_out_be16(const volatile u16 __iomem *addr, u16 val)
{
	mipc_writew(val, (void __iomem *)addr);
}

u8 mipc_in_8(const volatile u8 __iomem *addr)
{
	return mipc_readb((void __iomem *)addr);
}

void mipc_out_8(const volatile u8 __iomem *addr, u8 val)
{
	mipc_writeb(val, (void __iomem *)addr);
}




static int mipc_check_address(phys_addr_t pa)
{
	if (pa < 0x10000000 || pa > 0x14000000)
		return -EINVAL;
	return 0;
}

int mipc_discover(struct mipc_infohdr **hdrp)
{
	struct mipc_infohdr *hdr;
	char magic[4];
	phys_addr_t *p;
	int error;

	/* REVISIT, infohdr pointer should come from dts */

	/* grab mini information header location */
	p = ioremap(0x13fffffc, 4);
	if (!p) {
		pr_err("unable to ioremap mini ipc header ptr\n");
		error = -ENOMEM;
		goto out;
	}
	/* check that the header pointer points to MEM2 */
	if (mipc_check_address(*p)) {
		pr_devel("wrong mini ipc header address %pP\n", (void *)*p);
		error = -ENODEV;
		goto out_unmap_p;
	}

	hdr = (struct mipc_infohdr *)ioremap_prot(*p, sizeof(*hdr),
						      PAGE_KERNEL);
	if (!hdr) {
		pr_err("unable to ioremap mini ipc header\n");
		error = -ENOMEM;
		goto out_unmap_p;
	}
	__dma_sync(hdr, sizeof(*hdr), DMA_FROM_DEVICE);

	memcpy(magic, hdr->magic, 3);
	magic[3] = 0;
	if (memcmp(magic, "IPC", 3)) {
		pr_devel("wrong magic \"%s\"\n", magic);
		error = -ENODEV;
		goto out_unmap_hdr;
	}
	if (hdr->version < MIPC_MIN_VER && hdr->version > MIPC_MAX_VER) {
		pr_err("unsupported mini ipc version %d"
			   " (min %d, max %d)\n", hdr->version,
			   MIPC_MIN_VER, MIPC_MAX_VER);
		error = -ENODEV;
		goto out_unmap_hdr;
	}
	if (mipc_check_address(hdr->mem2_boundary)) {
		pr_err("invalid mem2_boundary %pP\n",
		       (void *)hdr->mem2_boundary);
		error = -EINVAL;
		goto out_unmap_hdr;
	}
	if (mipc_check_address(hdr->ipc_in)) {
		pr_err("invalid ipc_in %pP\n", (void *)hdr->ipc_in);
		error = -EINVAL;
		goto out_unmap_hdr;
	}
	if (mipc_check_address(hdr->ipc_out)) {
		pr_err("invalid ipc_out %pP\n", (void *)hdr->ipc_out);
		error = -EINVAL;
		goto out_unmap_hdr;
	}

	*hdrp = hdr;
	error = 0;
	goto out_unmap_p;

out_unmap_hdr:
	iounmap(hdr);
out_unmap_p:
	iounmap(p);
out:
	return error;
}

static void mipc_print_infohdr(struct mipc_infohdr *hdr)
{
	pr_info("magic=%c%c%c, version=%d, mem2_boundary=%pP\n",
		hdr->magic[0], hdr->magic[1], hdr->magic[2],
		hdr->version,
		(void *)hdr->mem2_boundary);
	pr_cont("ipc_in[%u] @ %pP, ipc_out[%u] @ %pP\n",
		hdr->ipc_in_size, (void *)hdr->ipc_in,
		hdr->ipc_out_size, (void *)hdr->ipc_out);
}

static int mipc_do_simple_tests = 0;

#ifndef MODULE
static int __init mipc_simple_tests_setup(char *str)
{
	if (*str)
		return 0;
	mipc_do_simple_tests = 1;
	return 1;
}
__setup("mipc_simple_tests", mipc_simple_tests_setup);
#endif

static unsigned long tbl_to_ns(unsigned long tbl)
{
	return (tbl * 1000) / tb_ticks_per_usec;
}

static void mipc_simple_tests(struct mipc_device *ipc_dev)
{
	void __iomem *io_base = ipc_dev->io_base;
	void *gpio;
	unsigned long t0;
	unsigned long t_read, t_write;
	unsigned long t_mipc_read, t_mipc_write, t_mipc_ping;
	u32 val;
	int i;

	gpio = mipc_ioremap(0x0d8000c0, 4);
	if (!gpio) {
		pr_err("ioremap failed\n");
		return;
	}

	for (i = 0; i < 64000; i++) {
		t0 = get_tbl();
		in_be32(io_base + MIPC_CSR);
		t_read = get_tbl() - t0;

		t0 = get_tbl();
		out_be32(io_base + MIPC_CSR, 0);
		t_write = get_tbl() - t0;

		t0 = get_tbl();
		val = mipc_readl(gpio);
		t_mipc_read = get_tbl() - t0;

		t0 = get_tbl();
		mipc_writel(val & ~0x20, gpio);
		t_mipc_write = get_tbl() - t0;

		t0 = get_tbl();
		mipc_ping(ipc_dev, MIPC_SYS_IO_TIMEOUT);
		t_mipc_ping = get_tbl() - t0;
	}

	pr_info("io timings in timebase ticks"
		" (1 usec = %lu ticks)\n", tb_ticks_per_usec);
	pr_cont("mmio: read=%lu (%lu ns), write=%lu (%lu ns)\n",
		t_read, tbl_to_ns(t_read), t_write, tbl_to_ns(t_write));
	pr_cont("mipc: read=%lu (%lu ns), write=%lu (%lu ns)\n",
		t_mipc_read, tbl_to_ns(t_mipc_read),
		t_mipc_write, tbl_to_ns(t_mipc_write));
	pr_cont("mipc: ping=%lu (%lu ns)\n",
		t_mipc_ping, tbl_to_ns(t_mipc_ping));

	mipc_iounmap(gpio);
}

static void mipc_shutdown_mini_devs(struct mipc_device *ipc_dev)
{
	struct mipc_req resp;
	int error;

	error = mipc_sendrecv1_call(ipc_dev, MIPC_DEV_TIMEOUT, &resp,
				    _MIPC(_MIPC_SLOW, _MIPC_DEV_SDHC,
					   _MIPC_SDHC_EXIT), 0);
	if (error)
		pr_err("unable to shutdown mini SDHC subsystem\n");
}

static void mipc_starlet_fixups(struct mipc_device *ipc_dev)
{
	void __iomem *gpio;

	/*
	 * Try to turn off the front led and sensor bar.
	 * (not strictly starlet-only stuff but anyway...)
	 */
	gpio = mipc_ioremap(0x0d8000c0, 4);
	if (gpio) {
		mipc_clearbitl(0x120, gpio);
		mipc_iounmap(gpio);
	}

	/* tell 'mini' to relinquish control of hardware */
	mipc_shutdown_mini_devs(ipc_dev);
}

static void mipc_init_ahbprot(struct mipc_device *ipc_dev)
{
	void __iomem *hw_ahbprot = (void __iomem *)0x0d800064;
	u32 initial_ahbprot, ahbprot;

	initial_ahbprot = mipc_readl(hw_ahbprot);
	if (initial_ahbprot != 0xffffffff) {
		pr_debug("AHBPROT=%08X (before)\n", initial_ahbprot);
		mipc_writel(0xffffffff, hw_ahbprot);
	}

	ahbprot = mipc_readl(hw_ahbprot);
	if (initial_ahbprot != ahbprot)
		pr_debug("AHBPROT=%08X (after)\n", ahbprot);
	if (ahbprot != 0xffffffff)
		pr_err("failed to set AHBPROT\n");
}

static int mipc_init(struct mipc_device *ipc_dev, struct resource *mem, int irq)
{
	struct mipc_infohdr *hdr;
	void __iomem *io_base;
	size_t io_size, in_size, out_size;
	int error;

	error = mipc_discover(&hdr);
	if (error) {
		pr_err("unable to find mini ipc instance\n");
		goto out;
	}

	spin_lock_init(&ipc_dev->call_lock);
	spin_lock_init(&ipc_dev->io_lock);

	io_size = mem[0].end - mem[0].start + 1;
	io_base = ipc_dev->io_base = ioremap(mem[0].start, io_size);
	ipc_dev->irq = irq;

	ipc_dev->hdr = hdr;

	mipc_print_infohdr(hdr);

	in_size = hdr->ipc_in_size * sizeof(*ipc_dev->in_ring);
	ipc_dev->in_ring = ioremap(hdr->ipc_in, in_size);
	ipc_dev->in_ring_size = hdr->ipc_in_size;
	ipc_dev->intail_idx = mipc_peek_first_intail(io_base);

	out_size = hdr->ipc_out_size * sizeof(*ipc_dev->out_ring);
	ipc_dev->out_ring = ioremap(hdr->ipc_out, out_size);
	ipc_dev->out_ring_size = hdr->ipc_out_size;
	ipc_dev->outhead_idx = mipc_peek_first_outhead(io_base);

	ipc_dev->tag = MIPC_INITIAL_TAG;
	mipc_device_instance = ipc_dev;

	mipc_print_status(ipc_dev);

	mipc_flush_send(ipc_dev, 5*1000);
	mipc_flush_recv(ipc_dev, 5*1000);
	error = mipc_ping(ipc_dev, 1*1000*1000);
	if (error)
		goto out;

	pr_info("ping OK\n");
	if (mipc_do_simple_tests)
		mipc_simple_tests(ipc_dev);

	mipc_init_ahbprot(ipc_dev);
	mipc_starlet_fixups(ipc_dev);

out:
	return error;
}

static void mipc_exit(struct mipc_device *ipc_dev)
{
	if (ipc_dev->in_ring)
		iounmap(ipc_dev->in_ring);
	if (ipc_dev->out_ring)
		iounmap(ipc_dev->out_ring);
}


/*
 * Driver model helper routines.
 *
 */

static int mipc_do_probe(struct device *dev, struct resource *mem, int irq)
{
	struct mipc_device *ipc_dev;
	int error;

	ipc_dev = kzalloc(sizeof(*ipc_dev), GFP_KERNEL);
	if (!ipc_dev) {
		pr_err("failed to allocate ipc_dev\n");
		error = -ENOMEM;
		goto out;
	}
	dev_set_drvdata(dev, ipc_dev);
	ipc_dev->dev = dev;

	error = mipc_init(ipc_dev, mem, irq);
	if (error) {
		dev_set_drvdata(dev, NULL);
		kfree(ipc_dev);
		goto out;
	}

	pr_info("ready\n");
	hlwd_pic_probe();

out:
	return error;
}

static int mipc_do_remove(struct device *dev)
{
	struct mipc_device *ipc_dev = dev_get_drvdata(dev);
	int error = 0;

	if (!ipc_dev) {
		error = -ENODEV;
		goto out;
	}

	mipc_exit(ipc_dev);
	dev_set_drvdata(dev, NULL);
	kfree(ipc_dev);
out:
	return error;
}

static int mipc_do_shutdown(struct device *dev)
{
	struct mipc_device *ipc_dev = dev_get_drvdata(dev);
	int error = 0;

	if (!ipc_dev) {
		error = -ENODEV;
		goto out;
	}
out:
	return error;
}

/*
 * OF platform driver hooks.
 *
 */

static int mipc_of_probe(struct platform_device *odev)
{
	struct resource mem[2];
	int error;

	error = of_address_to_resource(odev->dev.of_node, 0, &mem[0]);
	if (error) {
		pr_err("no io memory range found (%d)\n", error);
		goto out;
	}

	error = mipc_do_probe(&odev->dev, mem,
			      irq_of_parse_and_map(odev->dev.of_node, 0));
out:
	return error;
}

static int mipc_of_remove(struct platform_device *odev)
{
	return mipc_do_remove(&odev->dev);
}

static void mipc_of_shutdown(struct platform_device *odev)
{
	mipc_do_shutdown(&odev->dev);
}

static struct of_device_id mipc_of_match[] = {
	{ .compatible = "twiizers,starlet-mini-ipc" },
	{ },
};

MODULE_DEVICE_TABLE(of, mipc_of_match);

static struct platform_driver mipc_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = mipc_of_match,
	},
	.probe = mipc_of_probe,
	.remove = mipc_of_remove,
	.shutdown = mipc_of_shutdown,
};

/*
 * Kernel module interface hooks.
 *
 */

static int __init mipc_init_module(void)
{
	pr_info("%s - version %s\n", DRV_DESCRIPTION, mipc_driver_version);

	return platform_driver_register(&mipc_of_driver);
}

static void __exit mipc_exit_module(void)
{
	platform_driver_unregister(&mipc_of_driver);
}

module_init(mipc_init_module);
module_exit(mipc_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

