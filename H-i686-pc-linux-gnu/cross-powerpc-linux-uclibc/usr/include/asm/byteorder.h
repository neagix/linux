#ifndef _ASM_POWERPC_BYTEORDER_H
#define _ASM_POWERPC_BYTEORDER_H

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <asm/types.h>


#ifdef __GNUC__

#ifndef __STRICT_ANSI__
#define __BYTEORDER_HAS_U64__
#ifndef __powerpc64__
#define __SWAB_64_THRU_32__
#endif /* __powerpc64__ */
#endif /* __STRICT_ANSI__ */

#endif /* __GNUC__ */

#include <linux/byteorder/big_endian.h>

#endif /* _ASM_POWERPC_BYTEORDER_H */
