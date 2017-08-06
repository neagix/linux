/*
 * arch/powerpc/platforms/embedded6xx/gcnvi_udbg.h
 *
 * Nintendo GameCube/Wii framebuffer udbg output support.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __GCNVI_UDBG_H
#define __GCNVI_UDBG_H

#ifdef CONFIG_GAMECUBE_VIDEO_UDBG

extern void __init gcnvi_udbg_init(void);

#else

static inline void __init gcnvi_udbg_init(void)
{
}

#endif /* CONFIG_GAMECUBE_VIDEO_UDBG */

#endif /* __GCNVI_UDBG_H */
