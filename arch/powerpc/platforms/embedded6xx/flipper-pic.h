/*
 * arch/powerpc/platforms/embedded6xx/flipper-pic.h
 *
 * Nintendo GameCube/Wii interrupt controller support.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2007,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __FLIPPER_PIC_H
#define __FLIPPER_PIC_H

#define FLIPPER_NR_IRQS		32

/*
 * Each interrupt has a corresponding bit in both
 * the Interrupt Cause (ICR) and Interrupt Mask (IMR) registers.
 *
 * Enabling/disabling an interrupt line involves asserting/clearing
 * the corresponding bit in IMR. ACK'ing a request simply involves
 * asserting the corresponding bit in ICR.
 */
#define FLIPPER_ICR		0x00
#define FLIPPER_ICR_RSS		(1<<16) /* reset switch state */

#define FLIPPER_IMR		0x04

#define FLIPPER_RESET		0x24

unsigned int flipper_pic_get_irq(void);
void __init flipper_pic_probe(void);

void flipper_quiesce(void);
void flipper_platform_reset(void);
int flipper_is_reset_button_pressed(void);

#endif
