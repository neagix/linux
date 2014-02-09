/*
 * arch/powerpc/include/asm/starlet-ios.h
 *
 * Nintendo Wii Starlet IOS definitions
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __STARLET_IOS_H
#define __STARLET_IOS_H

#include <linux/types.h>
#include <linux/spinlock_types.h>
#include <linux/platform_device.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/timer.h>
#include <asm/rheap.h>
#define STARLET_TITLE_HBC_V107	0x00010001AF1BF516ULL	/* HBC v1.0.7+ */
#define STARLET_TITLE_HBC_JODI	0x000100014A4F4449ULL
#define STARLET_TITLE_HBC_HAXX	0x0001000148415858ULL

#define STARLET_EINVAL	-4

#define STARLET_IPC_DMA_ALIGN   0x1f /* 32 bytes */

struct starlet_ipc_request;

/* input/output heap */
struct starlet_ioh {
	spinlock_t lock;
	rh_info_t *rheap;
	unsigned long base_phys;
	void *base;
	size_t size;
};

/* pseudo-scatterlist support for the input/output heap */
struct starlet_ioh_sg {
	void *buf;
	size_t len;
	dma_addr_t dma_addr;
};

/* inter-process communication device abstraction */
struct starlet_ipc_device {
	unsigned long flags;

	void __iomem *io_base;
	int irq;

	struct dma_pool *dma_pool;	/* to allocate requests */
	struct starlet_ioh *ioh;	/* to allocate special io buffers */

	unsigned int random_id;

	spinlock_t list_lock;
	struct list_head outstanding_list;
	unsigned long nr_outstanding;
	struct list_head pending_list;
	unsigned long nr_pending;

	struct timer_list timer;

	struct starlet_ipc_request *req; /* for requests causing a ios reboot */

	struct device *dev;
};

/* iovec entry suitable for ioctlv */
struct starlet_iovec {
	dma_addr_t dma_addr;
	u32 dma_len;
};

typedef int (*starlet_ipc_callback_t)(struct starlet_ipc_request *req);

struct starlet_ipc_request {
	/* begin starlet firmware request format */
	u32 cmd;				/* 0x00 */
	s32 result;				/* 0x04 */
	union {					/* 0x08 */
		s32 fd;
		u32 req_cmd;
	};
	union {
		struct {
			dma_addr_t pathname;	/* 0x0c */
			u32 mode;		/* 0x10 */
		} open;
		struct {
			u32 request;		/* 0x0c */
			dma_addr_t ibuf;	/* 0x10 */
			u32 ilen;		/* 0x14 */
			dma_addr_t obuf;	/* 0x18 */
			u32 olen;		/* 0x1c */
		} ioctl;
		struct {
			u32 request;		/* 0x0c */
			u32 argc_in;		/* 0x10 */
			u32 argc_io;		/* 0x14 */
			dma_addr_t iovec_da;	/* 0x18 */
		} ioctlv;
		u32 argv[5];			/* 0x0c,0x10,0x14,0x18,0x1c */
	};
	/* end starlet firmware request format */

	/*
	 * A signature is used to discard bogus requests from earlier
	 * IPC instances.
	 */
	unsigned int sig;

	dma_addr_t dma_addr;	/* request dma address */

	/* ioctlv related data */
	struct starlet_iovec *iovec;
	size_t iovec_size;

	unsigned sgl_nents_in;
	unsigned sgl_nents_io;
	union {
		struct scatterlist *sgl_in;
		struct starlet_ioh_sg *ioh_sgl_in;
	};
	union {
		struct scatterlist *sgl_io;
		struct starlet_ioh_sg *ioh_sgl_io;
	};

	void *done_data;
	starlet_ipc_callback_t done;

	starlet_ipc_callback_t complete;

	unsigned long jiffies;

	struct list_head node; /* for queueing */

	struct starlet_ipc_device *ipc_dev;
};



/* from starlet-malloc.c */

extern int starlet_malloc_lib_bootstrap(struct resource *mem);

extern void *starlet_kzalloc(size_t size, gfp_t flags);
extern void starlet_kfree(void *ptr);

extern void *starlet_ioh_kzalloc(size_t size);
extern void starlet_ioh_kfree(void *ptr);

extern unsigned long starlet_ioh_virt_to_phys(void *ptr);

extern void starlet_ioh_sg_init_table(struct starlet_ioh_sg *sgl,
				      unsigned int nents);
extern void starlet_ioh_sg_set_buf(struct starlet_ioh_sg *sg,
				   void *buf, size_t len);

#define starlet_ioh_for_each_sg(sgl, sg, nr, __i) \
	for (__i = 0, sg = (sgl); __i < nr; __i++, sg++)

extern int starlet_ioh_dma_map_sg(struct device *dev,
				  struct starlet_ioh_sg *sgl, int nents,
				  enum dma_data_direction direction);
extern void starlet_ioh_dma_unmap_sg(struct device *dev,
				     struct starlet_ioh_sg *sgl, int nents,
				     enum dma_data_direction direction);
/* from starlet-ipc.c */

extern struct starlet_ipc_device *starlet_ipc_get_device(void);

extern struct starlet_ipc_request *
starlet_ipc_alloc_request(struct starlet_ipc_device *ipc_dev, gfp_t flags);
extern void starlet_ipc_free_request(struct starlet_ipc_request *req);


extern int starlet_open(const char *pathname, int flags);
extern int starlet_open_polled(const char *pathname, int flags,
			       unsigned long usecs);
extern int starlet_close(int fd);
extern int starlet_close_polled(int fd, unsigned long usecs);

extern int starlet_ioctl(int fd, int request,
			 void *ibuf, size_t ilen,
			 void *obuf, size_t olen);
extern int starlet_ioctl_nowait(int fd, int request,
				void *ibuf, size_t ilen,
				void *obuf, size_t olen,
				starlet_ipc_callback_t callback,
				void *arg);
extern int starlet_ioctl_polled(int fd, int request,
				void *ibuf, size_t ilen,
				void *obuf, size_t olen, unsigned long usecs);

extern int starlet_ioctlv(int fd, int request,
			      unsigned int nents_in,
			      struct scatterlist *sgl_in,
			      unsigned int nents_out,
			      struct scatterlist *sgl_out);
extern int starlet_ioctlv_nowait(int fd, int request,
				 unsigned int nents_in,
				 struct scatterlist *sgl_in,
				 unsigned int nents_out,
				 struct scatterlist *sgl_out,
				 starlet_ipc_callback_t callback,
				 void *arg);
extern int starlet_ioctlv_polled(int fd, int request,
				 unsigned int nents_in,
				 struct scatterlist *sgl_in,
				 unsigned int nents_out,
				 struct scatterlist *sgl_out,
				 unsigned long usecs);
extern int starlet_ioctlv_and_reboot(int fd, int request,
					 unsigned int nents_in,
					 struct scatterlist *sgl_in,
					 unsigned int nents_out,
					 struct scatterlist *sgl_out);

extern int starlet_ioh_ioctlv(int fd, int request,
		       unsigned int nents_in,
		       struct starlet_ioh_sg *ioh_sgl_in,
		       unsigned int nents_io,
		       struct starlet_ioh_sg *ioh_sgl_io);
extern int starlet_ioh_ioctlv_nowait(int fd, int request,
				     unsigned int nents_in,
				     struct starlet_ioh_sg *ioh_sgl_in,
				     unsigned int nents_io,
				     struct starlet_ioh_sg *ioh_sgl_io,
				     starlet_ipc_callback_t callback,
				     void *arg);

/* from starlet-es.c */

extern int starlet_es_reload_ios_and_discard(void);
extern int starlet_es_reload_ios_and_launch(u64 title);

/* from starlet-stm.c */

extern void starlet_stm_restart(void);
extern void starlet_stm_power_off(void);

#endif /* __STARLET_IOS_H */
