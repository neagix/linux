/*
 * arch/powerpc/include/asm/starlet.h
 *
 * Definitions for the Starlet co-processor
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __STARLET_H
#define __STARLET_H


enum starlet_ipc_flavour {
	STARLET_IPC_IOS,
	STARLET_IPC_MINI,
};

int starlet_discover_ipc_flavour(void);
enum starlet_ipc_flavour starlet_get_ipc_flavour(void);

#endif /* __STARLET_H */
