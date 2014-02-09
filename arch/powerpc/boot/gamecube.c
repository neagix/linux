/*
 * arch/powerpc/boot/gamecube.c
 *
 * Nintendo GameCube/Wii platforms
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <stddef.h>
#include "stdio.h"
#include "types.h"
#include "io.h"
#include "ops.h"

#include "ugecon.h"

BSS_STACK(8192);

/*
 * We enter with the MMU enabled and some legacy memory mappings active.
 *
 * We leave the MMU enabled, but we switch to an identity mapped memory
 * scheme as expected by the start code.
 *
 */
asm ("\n\
.text\n\
.globl _zimage_start\n\
_zimage_start:\n\
\n\
	isync\n\
	/* IBAT3,DBAT3 for first 16Mbytes */\n\
	li	8, 0x01ff	/* 16MB */\n\
	li      9, 0x0002	/* rw */\n\
	mtspr   0x216, 8	/* IBAT3U */\n\
	mtspr   0x217, 9	/* IBAT3L */\n\
	mtspr   0x21e, 8	/* DBAT3U */\n\
	mtspr   0x21f, 9	/* DBAT3L */\n\
\n\
	sync\n\
	isync\n\
\n\
	li	3, 0\n\
	li	4, 0\n\
	li	5, 0\n\
\n\
	bcl-    20,4*cr7+so,1f\n\
1:\n\
	mflr    8\n\
	clrlwi  8, 8, 3\n\
	addi    8, 8, 2f - 1b\n\
	mtlr    8\n\
	blr\n\
2:\n\
	b _zimage_start_lib\n\
");

/*
 *
 */
void platform_init(unsigned long r3, unsigned long r4, unsigned long r5)
{
	u32 heapsize = 16*1024*1024 - (u32)_end;

	simple_alloc_init(_end, heapsize, 32, 64);
	fdt_init(_dtb_start);

	if (!ug_grab_io_base() && ug_is_adapter_present())
		console_ops.write = ug_console_write;
}

