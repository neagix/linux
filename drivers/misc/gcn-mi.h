/*
 * drivers/misc/gcn-mi.h
 *
 * Nintendo GameCube Memory Interface driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004,2005,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __GCN_MI_H
#define __GCN_MI_H

#define MI_MAX_REGIONS  4

#define MI_PROT_NONE 0x00
#define MI_PROT_RO   0x01
#define MI_PROT_WO   0x02
#define MI_PROT_RW   0x03

int gcn_mi_region_protect(unsigned long physlo, unsigned long physhi, int type);
int gcn_mi_region_unprotect(int region);
void gcn_mi_region_unprotect_all(void);

#endif /* __GCN_MI_H */

