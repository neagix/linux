/*
 * arch/powerpc/platforms/embedded6xx/usbgecko_udbg.c
 *
 * udbg serial input/output routines for the USB Gecko adapter.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <asm/io.h>
#include <asm/prom.h>
#include <asm/udbg.h>

#include <mm/mmu_decl.h>

#include "usbgecko_udbg.h"


#define EXI_CLK_32MHZ           5

#define EXI_CSR                 0x00
#define   EXI_CSR_CLKMASK       (0x7<<4)
#define     EXI_CSR_CLK_32MHZ   (EXI_CLK_32MHZ<<4)
#define   EXI_CSR_CSMASK        (0x7<<7)
#define     EXI_CSR_CS_0        (0x1<<7)  /* Chip Select 001 */

#define EXI_CR                  0x0c
#define   EXI_CR_TSTART         (1<<0)
#define   EXI_CR_WRITE		(1<<2)
#define   EXI_CR_READ_WRITE     (2<<2)
#define   EXI_CR_TLEN(len)      (((len)-1)<<4)

#define EXI_DATA                0x10

#define UG_READ_ATTEMPTS	100
#define UG_WRITE_ATTEMPTS	100


static void __iomem *ug_io_base;

/*
 * Performs one input/output transaction between the spi host and the usbgecko.
 */
static u32 ug_io_transaction(u32 in)
{
	u32 __iomem *csr_reg = ug_io_base + EXI_CSR;
	u32 __iomem *data_reg = ug_io_base + EXI_DATA;
	u32 __iomem *cr_reg = ug_io_base + EXI_CR;
	u32 csr, data, cr;

	/* select */
	csr = EXI_CSR_CLK_32MHZ | EXI_CSR_CS_0;
	out_be32(csr_reg, csr);

	/* read/write */
	data = in;
	out_be32(data_reg, data);
	cr = EXI_CR_TLEN(2) | EXI_CR_READ_WRITE | EXI_CR_TSTART;
	out_be32(cr_reg, cr);

	while (in_be32(cr_reg) & EXI_CR_TSTART)
		barrier();

	/* deselect */
	out_be32(csr_reg, 0);

	/* result */
	data = in_be32(data_reg);

	return data;
}

/*
 * Returns true if an usbgecko adapter is found.
 */
static int ug_is_adapter_present(void)
{
	if (!ug_io_base)
		return 0;

	return ug_io_transaction(0x90000000) == 0x04700000;
}

/*
 * Returns true if the TX fifo is ready for transmission.
 */
static int ug_is_txfifo_ready(void)
{
	return ug_io_transaction(0xc0000000) & 0x04000000;
}

/*
 * Tries to transmit a character.
 * If the TX fifo is not ready the result is undefined.
 */
static void ug_raw_putc(char ch)
{
	ug_io_transaction(0xb0000000 | (ch << 20));
}

/*
 * Transmits a character.
 * It silently fails if the TX fifo is not ready after a number of retries.
 */
static void ug_putc(char ch)
{
	int count = UG_WRITE_ATTEMPTS;

	if (!ug_io_base)
		return;

	if (ch == '\n')
		ug_putc('\r');

	while (!ug_is_txfifo_ready() && count--)
		barrier();
	if (count)
		ug_raw_putc(ch);
}

#if 0
/*
 * Trasmits a null terminated character string.
 */
static void ug_puts(char *s)
{
	while (*s)
		ug_putc(*s++);
}
#endif

/*
 * Returns true if the RX fifo is ready for transmission.
 */
static int ug_is_rxfifo_ready(void)
{
	return ug_io_transaction(0xd0000000) & 0x04000000;
}

/*
 * Tries to receive a character.
 * If a character is unavailable the function returns -1.
 */
static int ug_raw_getc(void)
{
	u32 data = ug_io_transaction(0xa0000000);
	if (data & 0x08000000)
		return (data >> 16) & 0xff;
	else
		return -1;
}

/*
 * Receives a character.
 * It fails if the RX fifo is not ready after a number of retries.
 */
static int ug_getc(void)
{
	int count = UG_READ_ATTEMPTS;

	if (!ug_io_base)
		return -1;

	while (!ug_is_rxfifo_ready() && count--)
		barrier();
	return ug_raw_getc();
}

/*
 * udbg functions.
 *
 */

/*
 * Transmits a character.
 */
void ug_udbg_putc(char ch)
{
	ug_putc(ch);
}

/*
 * Receives a character. Waits until a character is available.
 */
static int ug_udbg_getc(void)
{
	int ch;

	while ((ch = ug_getc()) == -1)
		barrier();
	return ch;
}

/*
 * Receives a character. If a character is not available, returns -1.
 */
static int ug_udbg_getc_poll(void)
{
	if (!ug_is_rxfifo_ready())
		return -1;
	return ug_getc();
}

/*
 * Retrieves and prepares the virtual address needed to access the hardware.
 */
static void __iomem *ug_udbg_setup_io_base(struct device_node *np)
{
	phys_addr_t paddr;
	const unsigned int *reg;

	reg = of_get_property(np, "reg", NULL);
	if (reg) {
		paddr = of_translate_address(np, reg);
		if (paddr) {
			ug_io_base = ioremap(paddr, reg[1]);
			return ug_io_base;
		}
	}
	return NULL;
}

/*
 * USB Gecko udbg support initialization.
 */
void __init ug_udbg_init(void)
{
	struct device_node *np = NULL;
	struct device_node *stdout;
	const char *path;

	if (ug_io_base)
		udbg_printf("%s: early -> final\n", __func__);

	if (!of_chosen) {
		udbg_printf("%s: missing of_chosen\n", __func__);
		goto done;
	}

	path = of_get_property(of_chosen, "linux,stdout-path", NULL);
	if (!path) {
		udbg_printf("%s: missing %s property", __func__,
			    "linux,stdout-path");
		goto done;
	}

	stdout = of_find_node_by_path(path);
	if (!stdout) {
		udbg_printf("%s: missing path %s", __func__, path);
		goto done;
	}

	for (np = NULL;
	    (np = of_find_compatible_node(np, NULL, "usbgecko,usbgecko"));)
		if (np == stdout)
			break;

	of_node_put(stdout);
	if (!np) {
		udbg_printf("%s: stdout is not an usbgecko", __func__);
		goto done;
	}

	if (!ug_udbg_setup_io_base(np)) {
		udbg_printf("%s: failed to setup io base", __func__);
		goto done;
	}

	if (!ug_is_adapter_present()) {
		udbg_printf("usbgecko_udbg: not found\n");
		ug_io_base = NULL;
	} else {
		udbg_putc = ug_udbg_putc;
		udbg_getc = ug_udbg_getc;
		udbg_getc_poll = ug_udbg_getc_poll;
		udbg_printf("usbgecko_udbg: ready\n");
	}

done:
	if (np)
		of_node_put(np);
	return;
}

#ifdef CONFIG_PPC_EARLY_DEBUG_USBGECKO

/*
 * USB Gecko early debug support initialization for udbg.
 *
 */
void __init udbg_init_usbgecko(void)
{
	unsigned long vaddr, paddr;

#if defined(CONFIG_GAMECUBE)
	paddr = 0x0c000000;
#elif defined(CONFIG_WII)
	paddr = 0x0d000000;
#else
#error Invalid platform for USB Gecko based early debugging.
#endif

	vaddr = 0xc0000000 | paddr;
	setbat(1, vaddr, paddr, 128*1024, PAGE_KERNEL_NCG);

	ug_io_base = (void __iomem *)(vaddr | 0x6814);

	udbg_putc = ug_udbg_putc;
	udbg_getc = ug_udbg_getc;
	udbg_getc_poll = ug_udbg_getc_poll;
}

#endif /* CONFIG_PPC_EARLY_DEBUG_USBGECKO */
