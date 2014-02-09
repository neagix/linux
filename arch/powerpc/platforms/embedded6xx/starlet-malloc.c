/*
 * arch/powerpc/platforms/embedded6xx/starlet-malloc.c
 *
 * Nintendo Wii starlet memory allocation library
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 * Notes from the trenches:
 *
 * writes from broadway to mem2
 * - 8 or 16 bit writes to mem2 modify 64 bits
 *   - writing 0xaa results in 0xaaffffffaaffffff being written
 *   - writing 0xaabb results in 0xaabbffffaabbffff being written
 * - 32 bit writes work fine
 * writes from starlet to mem1
 * - data must be 4 byte aligned, length must be 4 byte aligned
 *
 * write protected area (reads after writes do not return written info)
 * 0x13620000 - 0x14000000
 *
 */

#define DEBUG

#define DBG(fmt, arg...)	pr_debug(fmt, ##arg)

#include <linux/kernel.h>
#include <linux/resource.h>
#include <asm/starlet-ios.h>


#define LIB_MODULE_NAME		"starlet-malloc"
#define LIB_DESCRIPTION		"Nintendo Wii starlet malloc library"
#define LIB_AUTHOR		"Albert Herranz"

static char starlet_malloc_lib_version[] = "0.1i";

#define drv_printk(level, format, arg...) \
	 printk(level LIB_MODULE_NAME ": " format , ## arg)


#define STARLET_IOH_ALIGN	31

/*
 * Simple aligned kzalloc and free.
 *
 * Based on the idea proposed by Satya Kiran Popuri
 * http://www.cs.uic.edu/~spopuri/amalloc.html
 */

/**
 *
 */
static void *kzalloc_aligned(size_t size, gfp_t flags, size_t align)
{
	void *ptr, *aligned_ptr;
	size_t aligned_size;

	/* not a power of two */
	if (align & (align - 1))
		return NULL;

	/* worst case allocation size */
	aligned_size = size + align - 1;

	/* add extra space to store allocation delta */
	aligned_size += sizeof(size_t);

	/* allocate all space */
	ptr = kzalloc(aligned_size, flags);
	if (!ptr)
		return NULL;

	/* calculate the aligned address, making room for the delta value */
	aligned_ptr = PTR_ALIGN(ptr + sizeof(size_t), align);

	/* save the delta before the address returned to caller */
	*((size_t *)aligned_ptr - 1) = aligned_ptr - ptr;

	return aligned_ptr;
}

static void kfree_aligned(void *aligned_ptr)
{
	void *ptr;
	size_t delta;

	if (!aligned_ptr)
		return;

	/* retrieve extra allocation delta */
	delta = *((size_t *)aligned_ptr - 1);

	/* calculate original allocation area start */
	ptr = aligned_ptr - delta;

	kfree(ptr);
}


/**
 *
 */
void *starlet_kzalloc(size_t size, gfp_t flags)
{
	return kzalloc_aligned(size, flags, STARLET_IPC_DMA_ALIGN+1);
}

/**
 *
 */
void starlet_kfree(void *ptr)
{
	kfree_aligned(ptr);
}



/*
 * Functions for special input/output buffer allocations.
 *
 * Starlet seems to have a limitation when doing non-32 bit writes to MEM1.
 * This can cause up to a 3 byte data loss when starlet delivers
 * data of an unaligned size.
 * Writes to MEM2 don't have such a limitation.
 *
 * We use special buffers when we need to retrieve data of an unaligned size
 * from starlet.
 *
 */

static int starlet_ioh_init(struct starlet_ioh *ioh, struct resource *mem)
{
	size_t size = mem->end - mem->start + 1;
	rh_info_t *rheap;
	int error = -ENOMEM;

	ioh->base = ioremap_flags(mem->start, size, _PAGE_GUARDED);
	if (!ioh->base) {
		drv_printk(KERN_ERR, "unable to ioremap ioh area\n");
		goto err;
	}
	ioh->base_phys = mem->start;
	ioh->size = size;

	{
		void *first = NULL, *last = NULL;
		u32 *p;

		p = ioh->base + size;
		do {
			p--;
			*p = 0xdeadbabe;
		} while (p != ioh->base);
		__dma_sync(ioh->base, size, DMA_TO_DEVICE);

		p = ioh->base + size;
		do {
			p--;
			if (*p != 0xdeadbabe) {
				if (!last)
					last = p;
				first = p;
			}
		} while (p != ioh->base);

		if (first)
			drv_printk(KERN_INFO, "unreliable writes from"
				   " %p to %p\n", first, last);
	}

	rheap = rh_create(STARLET_IOH_ALIGN+1);
	if (IS_ERR(rheap)) {
		error = PTR_ERR(rheap);
		goto err_rh_create;
	}
	ioh->rheap = rheap;

	error = rh_attach_region(rheap, 0, size);
	if (error)
		goto err_rh_attach_region;

	spin_lock_init(&ioh->lock);

	drv_printk(KERN_INFO, "ioh at 0x%08lx, mapped to 0x%p, size %uk\n",
		   ioh->base_phys, ioh->base, ioh->size / 1024);

	return 0;

err_rh_create:
	iounmap(ioh->base);
err_rh_attach_region:
	rh_destroy(ioh->rheap);
err:
	return error;
}

static struct starlet_ioh *starlet_ioh;

/**
 *
 */
static struct starlet_ioh *starlet_ioh_get(void)
{
	if (unlikely(!starlet_ioh))
		drv_printk(KERN_ERR, "uninitialized ioh instance!\n");
	return starlet_ioh;
}

unsigned long starlet_ioh_virt_to_phys(void *ptr)
{
	struct starlet_ioh *ioh = starlet_ioh_get();
	unsigned long offset;

	if (!ioh || !ptr)
		return 0;

	offset = ptr - ioh->base;
	return ioh->base_phys + offset;
}

/**
 *
 */
void *starlet_ioh_kzalloc_aligned(size_t size, size_t align)
{
	struct starlet_ioh *ioh = starlet_ioh_get();
	unsigned long offset;
	void *ptr;
	unsigned long flags;

	if (!ioh)
		return NULL;

	spin_lock_irqsave(&ioh->lock, flags);
	offset = rh_alloc_align(ioh->rheap, size, align, NULL);
	spin_unlock_irqrestore(&ioh->lock, flags);

	if (IS_ERR_VALUE(offset))
		return NULL;

	ptr = ioh->base + offset;
	memset(ptr, 0, size);

	return ptr;
}

/**
 *
 */
void *starlet_ioh_kzalloc(size_t size)
{
	return starlet_ioh_kzalloc_aligned(size, STARLET_IOH_ALIGN+1);
}

/**
 *
 */
void starlet_ioh_kfree(void *ptr)
{
	struct starlet_ioh *ioh = starlet_ioh_get();
	unsigned long offset;
	unsigned long flags;

	if (!ioh || !ptr)
		return;

	offset = ptr - ioh->base;

	spin_lock_irqsave(&ioh->lock, flags);
	rh_free(ioh->rheap, offset);
	spin_unlock_irqrestore(&ioh->lock, flags);
}

int starlet_ioh_dma_map_sg(struct device *dev, struct starlet_ioh_sg *sgl,
			   int nents, enum dma_data_direction direction)
{
	struct starlet_ioh_sg *sg;
	int i;

	BUG_ON(direction == DMA_NONE);

	starlet_ioh_for_each_sg(sgl, sg, nents, i) {
		if (!sg->buf || sg->len == 0)
			continue;
		__dma_sync(sg->buf, sg->len, direction);
	}
	return nents;
}

void starlet_ioh_dma_unmap_sg(struct device *dev, struct starlet_ioh_sg *sgl,
			      int nents, enum dma_data_direction direction)
{
	/* nothing to do */
}

void starlet_ioh_sg_init_table(struct starlet_ioh_sg *sgl, unsigned int nents)
{
	memset(sgl, 0, nents * sizeof(*sgl));
}

/**
 *
 * @buf: must have been allocated using one of the starlet_ioh_* alloc
 *       functions.
 */
void starlet_ioh_sg_set_buf(struct starlet_ioh_sg *sg, void *buf, size_t len)
{
	struct starlet_ioh *ioh = starlet_ioh_get();
	unsigned long offset;

	if (buf && len) {
		offset = buf - ioh->base;

		sg->buf = buf;
		sg->len = len;
		sg->dma_addr = ioh->base_phys + offset;
	} else {
		sg->buf = NULL;
		sg->len = 0;
		sg->dma_addr = 0;
	}
}


/**
 *
 */
int starlet_malloc_lib_bootstrap(struct resource *mem)
{
	struct starlet_ioh *ioh;
	int error;

	if (starlet_ioh) {
		drv_printk(KERN_WARNING, "already bootstrapped\n");
		return 0;
	}

	drv_printk(KERN_INFO, "%s - version %s\n", LIB_DESCRIPTION,
		   starlet_malloc_lib_version);

	ioh = kzalloc(sizeof(*ioh), GFP_KERNEL);
	if (!ioh) {
		drv_printk(KERN_ERR, "failed to allocate ioh\n");
		return -ENOMEM;
	}

	error = starlet_ioh_init(ioh, mem);
	if (error)
		kfree(ioh);
	else
		starlet_ioh = ioh;

	return error;
}


