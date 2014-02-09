/*
 * drivers/video/gcn-vifb.c
 *
 * Nintendo GameCube/Wii Video Interface (VI) frame buffer driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Michael Steil <mist@c64.org>
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2006,2007,2008,2009 Albert Herranz
 *
 * Based on vesafb (c) 1998 Gerd Knorr <kraxel@goldbach.in-berlin.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/wait.h>
#include <linux/io.h>
#ifdef CONFIG_WII_AVE_RVL
#include <linux/i2c.h>
#endif

#define DRV_MODULE_NAME   "gcn-vifb"
#define DRV_DESCRIPTION   "Nintendo GameCube/Wii Video Interface (VI) driver"
#define DRV_AUTHOR        "Michael Steil <mist@c64.org>, " \
			  "Todd Jeffreys <todd@voidpointer.org>, " \
			  "Albert Herranz"

static char vifb_driver_version[] = "2.1i";

#define drv_printk(level, format, arg...) \
	 printk(level DRV_MODULE_NAME ": " format , ## arg)


/*
 * Hardware registers.
 */

#define __declare_vi_reg_set_field(reg_size, reg, field_size, field, \
				   mask, shift) \
static inline reg_size vi_##reg##_set_##field(reg_size reg, field_size field) \
{									\
	reg &= ~(mask << shift);					\
	reg |= (field & mask) << shift;					\
	return reg;							\
}

#define __declare_vi_reg_clear_field(reg_size, reg, field, mask, shift) \
static inline reg_size vi_##reg##_clear_##field(reg_size reg)	\
{									\
	reg &= ~(mask << shift);					\
	return reg;							\
}

#define __declare_vi_reg_get_field(reg_size, reg, field_size, field, \
				   mask, shift) \
static inline field_size vi_##reg##_get_##field(reg_size reg)	\
{									\
	return (reg>>shift)&mask;					\
}

#define __declare_vi_reg_field(reg_size, reg, field_size, field, \
			       mask, shift) \
static inline reg_size vi_##reg##_##field(field_size field)	\
{									\
	return (field & mask) << shift;					\
}

#define __vi_reg_field(reg_size, reg, field_size, field, mask, shift) \
__declare_vi_reg_set_field(reg_size, reg, field_size, field, mask, shift) \
__declare_vi_reg_clear_field(reg_size, reg, field, mask, shift) 	  \
__declare_vi_reg_get_field(reg_size, reg, field_size, field, mask, shift) \
__declare_vi_reg_field(reg_size, reg, field_size, field, mask, shift)


#define VI_VTR			0x00 /* Vertical Timing, 16 bits */
__vi_reg_field(u16, vtr, u16, acv, 0x3ff, 4);		/* ACtive Video */
__vi_reg_field(u16, vtr, u8, equ, 0xf, 0);		/* EQUalization pulse */

#define VI_DCR			0x02 /* Display Configuration, 16 bits */
__vi_reg_field(u16, dcr, u8, fmt, 0x3, 8);		/* Format */
__vi_reg_field(u16, dcr, u8, le1, 0x3, 6);		/* Latch Enable 1 */
__vi_reg_field(u16, dcr, u8, le0, 0x3, 4);		/* Latch Enable 0 */
__vi_reg_field(u16, dcr, u8, dlr, 0x1, 3);		/* 3D mode */
__vi_reg_field(u16, dcr, u8, nin, 0x1, 2);		/* Non-Interlaced */
__vi_reg_field(u16, dcr, u8, rst, 0x1, 1);		/* Reset */
__vi_reg_field(u16, dcr, u8, enb, 0x1, 0);		/* Enable */

#define VI_HTR0			0x04 /* Horizontal Timing 0, 32 bits */
__vi_reg_field(u32, htr0, u8, hcs, 0x7f, 24);		/* Horz Color Start */
__vi_reg_field(u32, htr0, u8, hce, 0x7f, 16);		/* Horz Color End */
__vi_reg_field(u32, htr0, u16, hlw, 0x1ff, 0);		/* Half Line Width */

#define VI_HTR1			0x08 /* Horizontal Timing 1, 32 bits */
__vi_reg_field(u32, htr1, u16, hbs, 0x3ff, 17);		/* Horz Blank Start */
__vi_reg_field(u32, htr1, u16, hbe, 0x3ff, 7);		/* Horz Blank End */
__vi_reg_field(u32, htr1, u8, hsy, 0x7f, 0);		/* Horz Sync Width */

#define VI_VTO			0x0c /* Vertical Timing Odd, 32 bits */
__vi_reg_field(u32, vto, u16, psb, 0x3ff, 16);		/* Post Blanking */
__vi_reg_field(u32, vto, u16, prb, 0x3ff, 0);		/* Pre Blanking */

#define VI_VTE			0x10 /* Vertical Timing Even, 32 bits */
__vi_reg_field(u32, vte, u16, psb, 0x3ff, 16);		/* Post Blanking */
__vi_reg_field(u32, vte, u16, prb, 0x3ff, 0);		/* Pre Blanking */

#define VI_BBOI			0x14 /* Burst Blanking Odd Interval, 32 bits */
__vi_reg_field(u32, bboi, u16, be3, 0x7ff, 21);
__vi_reg_field(u32, bboi, u8, bs3, 0x1f, 16);
__vi_reg_field(u32, bboi, u16, be1, 0x7ff, 5);
__vi_reg_field(u32, bboi, u8, bs1, 0x1f, 0);

#define VI_BBEI			0x18 /* Burst Blanking Even Interval, 32 bits */
__vi_reg_field(u32, bbei, u16, be4, 0x7ff, 21);
__vi_reg_field(u32, bbei, u8, bs4, 0x1f, 16);
__vi_reg_field(u32, bbei, u16, be2, 0x7ff, 5);
__vi_reg_field(u32, bbei, u8, bs2, 0x1f, 0);

#define VI_TFBL			0x1c /* Top Field Base (L), 32 bits */
__vi_reg_field(u32, tfbl, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, tfbl, u8, xof, 0xf, 24);		/* X Offset */
__vi_reg_field(u32, tfbl, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_TFBR			0x20 /* Top Field Base (R), 32 bits */
__vi_reg_field(u32, tfbr, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, tfbr, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_BFBL			0x24 /* Bottom Field Base (L), 32 bits */
__vi_reg_field(u32, bfbl, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, bfbl, u8, xof, 0xf, 24);		/* X Offset */
__vi_reg_field(u32, bfbl, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_BFBR			0x28 /* Bottom Field Base (R), 32 bits */
__vi_reg_field(u32, bfbr, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, bfbr, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_DPV			0x2c /* Display Position Vertical, 16 bits */
__vi_reg_field(u16, dpv, u16, val, 0x7ff, 0);

#define VI_DPH			0x2e /* Display Position Horizontal, 16 bits */
__vi_reg_field(u16, dph, u16, val, 0x7ff, 0);

#define VI_DI0			0x30 /* Display Interrupt 0, 32 bits */
#define VI_DI1			0x34 /* Display Interrupt 1, 32 bits */
#define VI_DI2			0x38 /* Display Interrupt 2, 32 bits */
#define VI_DI3			0x3C /* Display Interrupt 3, 32 bits */
__vi_reg_field(u32, dix, u8, irq, 0x1, 31);
__vi_reg_field(u32, dix, u8, enb, 0x1, 28);
__vi_reg_field(u32, dix, u16, vct, 0x3ff, 16);
__vi_reg_field(u32, dix, u16, hct, 0x3ff, 0);

#define VI_DL0			0x40 /* Display Latch 0, 32 bits */

#define VI_DL1			0x44 /* Display Latch 1, 32 bits */

#define VI_PCR			0x48 /* Picture Configuration, 16 bits */
__vi_reg_field(u16, pcr, u8, wpl, 0xff, 8);	/* reads per line in words */
__vi_reg_field(u16, pcr, u8, std, 0xff, 0);	/* stride per line in words */

#define VI_HSR			0x4a /* Horizontal Scaling, 16 bits */
__vi_reg_field(u16, hsr, u8, hs_en, 0x1, 12);
__vi_reg_field(u16, hsr, u16, stp, 0x1ff, 0);

#define VI_FCT0			0x4c /* Filter Coeficient Table 0, 32 bits */
#define VI_FCT1			0x50 /* Filter Coeficient Table 1, 32 bits */
#define VI_FCT2			0x54 /* Filter Coeficient Table 2, 32 bits */
#define VI_FCT3			0x58 /* Filter Coeficient Table 3, 32 bits */
#define VI_FCT4			0x5c /* Filter Coeficient Table 4, 32 bits */
#define VI_FCT5			0x60 /* Filter Coeficient Table 5, 32 bits */
#define VI_FCT6			0x64 /* Filter Coeficient Table 6, 32 bits */

#define VI_AA			0x68 /* Anti-aliasing, 32 bits */

#define VI_CLK			0x6c /* Video Clock, 16 bits */
__vi_reg_field(u16, clk, u8, _54mhz, 0x1, 0);

#define VI_SEL			0x6e /* DTV Status, 16 bits */
__vi_reg_field(u16, sel, u8, component, 0x1, 0);

#define VI_HSW			0x70 /* Horizontal Scaling Width, 16 bits */
__vi_reg_field(u16, hsw, u16, width, 0x3ff, 0);

#define VI_HBE			0x72 /* Horizontal Border End, 16 bits */
#define VI_HBS			0x74 /* Horizontal Border Start, 16 bits */

#define VI_UNK1			0x76 /* Unknown1, 16 bits */
#define VI_UNK2			0x78 /* Unknown2, 32 bits */
#define VI_UNK3			0x7c /* Unknown3, 32 bits */


enum {
	VI_SCAN_DONTCARE = 0,
	VI_SCAN_INTERLACED,
	VI_SCAN_PROGRESSIVE,
};
enum {
	VI_RATE_DONTCARE = 0,
	VI_RATE_50Hz,
	VI_RATE_60Hz,
};
enum {
	VI_TV_DONTCARE = 0,
	VI_TV_NTSC,
	VI_TV_PAL,
};


/*
 * Video modes and timings.
 *
 */

enum {
	VI_VM_NTSC_480i = 0,
	VI_VM_NTSC_480p,
	VI_VM_PAL_576i50,
	VI_VM_PAL_480i60,
	VI_VM_PAL_480p,
};

enum vi_video_format {
	VI_FMT_NTSC = 0,
	VI_FMT_PAL,
	VI_FMT_MPAL,
	VI_FMT_DEBUG,
};

enum vi_tv_mode_flags {
	__PAL_COLOR,	/* vs NTSC_COLOR */
	__PROGRESSIVE,	/* vs interlaced */
};

#define VI_VMF_PAL_COLOR	(1<<__PAL_COLOR)
#define VI_VMF_PROGRESSIVE	(1<<__PROGRESSIVE)


#define VI_VERT_ALIGN		0x1	/* in lines-1 */
#define VI_HORZ_ALIGN		0xf	/* in pixels-1 */
#define VI_HORZ_WORD_SIZE	32	/* bytes */


/*
 * Video mode timings.
 */
struct vi_mode_timings {
	/* VERTICAL SETTINGS */

	/*
	 * NTSC 480i
	 * 1 field = 262.5 lines (242.5 active, 20 blank)
	 * 1 frame = 2 fields = 2 x 262.5 = 525 lines (485 active, 40 blank)
	 *
	 * PAL 576i
	 * 1 field = 312.5 lines (287.5 active, 25 blank)
	 * 1 frame = 2 fields = 2 x 312.5 = 625 lines (575 active, 50 blank)
	 *
	 * NOTES:
	 * - the start of sync is considered the start of a line
	 * - the width of a half line is the width of a line divided by two
	 *
	 */

	/*
	 * Vertical position of the first active video line (0=top).
	 */
	unsigned int ypos;

	/*
	 * Horizontal position in pixels where the vertical blanking
	 * interval starts. Used for signaling the start of the vertical
	 * retrace.
	 */
	unsigned int htrap;

	/*
	 * Vertical position in field lines where the vertical blanking
	 * interval starts. Used for signaling the start of the vertical
	 * retrace.
	 */
	unsigned int vtrap;

	/*
	 * Active Video, specified in number of field lines.
	 */
	u16	acv;
	/*
	 * Equalization pulse, specified in number of half lines.
	 */
	u8	equ;

	/*
	 * Pre-blanking, specified in half lines.
	 */
	u16	prb_odd;
	u16	prb_even;

	/*
	 * Post-blanking, specified in half lines.
	 */
	u16	psb_odd;
	u16	psb_even;

	/*
	 * NOTE:
	 * Irrespective of what patent 6,609,977 says:
	 * - "bs*" seems to tell where the burst blanking for the current
	 *   field ends
	 * - "be*" seems to tell where the next burst blanking starts
	 */

	/*
	 * Patent says: "Start to burst blanking start in half lines".
	 */
	u8	bs1;
	u8	bs2;
	u8	bs3;
	u8	bs4;

	/*
	 * Patent says: "Start to burst blanking end in half lines".
	 */
	u16	be1;
	u16	be2;
	u16	be3;
	u16	be4;

	/* HORIZONTAL SETTINGS */

	/*
	 * A = Blank Start to Horizontal Sync Start, "Front Porch"
	 *     right_margin
	 * B = Horizontal Sync Width
	 *     hsync_len
	 * C = Horizontal Sync End to Blank End, "Back Porch"
	 *     left_margin
	 * D = Horizontal Line Width
	 *     hsync_len + left_margin + xres + right_margin
	 * E = Horizontal Visible Width
	 *     xres
	 *
	 *               :<-----------------D----------------->:
	 *           :   :     :     :<----------E-------->:   :
	 *           :<A>:<-B->:<-C->:                     :<A>:<-B->:<-C->:
	 *           :   :     :     :                     :   :     :     :
	 *        ___                 __________//_________                 __
	 *           |               |                     |               |
	 *           |               |                     |               |
	 * Blank     |___       _____|                     |___       _____|
	 *               |     |                               |     |
	 * Sync          |_____|                               |_____|
	 *
	 *
	 * f = Sync Start to Color Burst Start
	 * g = Color Burst Width
	 *
	 *  :       :             :                  :
	 *  :<--A-->:<-----B----->:<-------C-------->:
	 *  :       :             :                  :
	 *                                             _ Peak white level
	 *  |                            Color       |
	 *  |                            Burst       |
	 *  |_______               ______|||||||||___| _ Blanking level
	 *          | Sync        |      |||||||||
	 *          |_____________|                    _ Sync level
	 *
	 *  :       :                    :       :   :
	 *  :       :<---------f-------->:<--g-->:   :
	 *  :<--------------- A + B + C ------------>:
	 *  :                                        :
	 *
	 */

	/* Half (horizontal) line width, in pixel clocks (D/2)  */
	u16 hlw;

	/* Horizontal Sync Width, in pixel clocks (B) */
	u8 hsy;

	/* NOTE
	 * The color burst interval falls within the back porch,
	 * i.e. hcs must be greater than B and hce lower than B+C.
	 */

	/* Horizontal sync start to color burst start in pixel clocks (f) */
	u8 hcs;
	/* Horizontal sync start to color burst end in pixel clocks (f+g) */
	u8 hce;

	/*
	 * The following two settings depend on the effective horizontal
	 * line length, as they rely on A or C.
	 */

	/* Half line to horizontal blank start (D/2 - A)*/
	u16 hbs;

	/* Horizontal sync start to horizontal blank end (B+C)*/
	u16 hbe;

};

/*
 * TV mode.
 */
struct vi_tv_mode {
	char *name;
	__u32 flags;
	int width;		/* visible width in pixels */
	int height;		/* visible height in lines */
	int lines;		/* total lines */
};

/*
 * Video control data structure.
 */
struct vi_ctl {
	spinlock_t lock;

	void __iomem *io_base;
	unsigned int irq;

	int in_vtrace;
	wait_queue_head_t vtrace_waitq;

	int visible_page;
	unsigned long page_address[2];
	unsigned long flip_pending;

	struct vi_tv_mode *mode;
	struct vi_mode_timings timings;
	int has_component_cable:1;	/* at last detection time */

	struct fb_info *info;
#ifdef CONFIG_WII_AVE_RVL
	struct i2c_client *i2c_client;
#endif
};


/*
 * TV Mode Table
 */
static struct vi_tv_mode vi_tv_modes[] = {
	[VI_VM_NTSC_480i] = {
		.name = "NTSC 480i",
		.width = 640,
		.height = 480,
		.lines = 525,
	},
	[VI_VM_NTSC_480p] = {
		.name = "NTSC 480p",
		.flags = VI_VMF_PROGRESSIVE,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
	[VI_VM_PAL_576i50] = {
		.name = "PAL 576i",
		.flags = VI_VMF_PAL_COLOR,
		.width = 640,
		.height = 574,
		.lines = 625,
	},
	[VI_VM_PAL_480i60] = {
		.name = "PAL 480i 60Hz",
		.flags = VI_VMF_PAL_COLOR,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
	[VI_VM_PAL_480p] = {
		.name = "PAL 480p",
		.flags = VI_VMF_PROGRESSIVE|VI_VMF_PAL_COLOR,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
};

/*
 * Filter Coeficient Table
 */
static const u32 vi_fct[] = {
	0x1AE771F0, 0x0DB4A574, 0x00C1188E, 0xC4C0CBE2,
	0xFCECDECF, 0x13130F08, 0x00080C0F,
};


/*
 * Default fix and var framebuffer data.
 */
static struct fb_fix_screeninfo vifb_fix = {
	.id = DRV_MODULE_NAME,
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR,	/* lies, lies, lies, ... */
	.accel = FB_ACCEL_NONE,
};

static struct fb_var_screeninfo vifb_var = {
	.activate = FB_ACTIVATE_NOW,
	.width = 640,
	.height = 480,
	.bits_per_pixel = 16,
	.vmode = FB_VMODE_INTERLACED,
};


/*
 * Setup parameters.
 */
static int want_ypan = 1;		/* 0..nothing, 1..ypan */

/* use old behaviour for video mode settings */
static int nostalgic;

static int force_scan;
static int force_rate;
static int force_tv;

static u32 pseudo_palette[17];



/*
 *
 *
 */

#ifdef CONFIG_WII_AVE_RVL
static int vi_ave_setup(struct vi_ctl *ctl);
static int vi_ave_get_video_format(struct vi_ctl *ctl,
				   enum vi_video_format *fmt);
#endif

/* some glue to the gx side */
static inline void gcngx_dispatch_vtrace(struct vi_ctl *ctl)
{
}


/*
 *
 * Color space handling.
 */

/*
 * RGB to YCbYCr conversion support bits.
 * We are using here the ITU.BT-601 Y'CbCr standard.
 *
 * References:
 * - "Colour Space Conversions" by Adrian Ford and Alan Roberts, 1998
 *   (google for coloureq.pdf)
 *
 */

#define RGB2YUV_SHIFT   16
#define RGB2YUV_LUMA    16
#define RGB2YUV_CHROMA 128

#define Yr ((int)(0.299 * (1<<RGB2YUV_SHIFT)))
#define Yg ((int)(0.587 * (1<<RGB2YUV_SHIFT)))
#define Yb ((int)(0.114 * (1<<RGB2YUV_SHIFT)))

#define Ur ((int)(-0.169 * (1<<RGB2YUV_SHIFT)))
#define Ug ((int)(-0.331 * (1<<RGB2YUV_SHIFT)))
#define Ub ((int)(0.500 * (1<<RGB2YUV_SHIFT)))

#define Vr ((int)(0.500 * (1<<RGB2YUV_SHIFT)))	/* same as Ub */
#define Vg ((int)(-0.419 * (1<<RGB2YUV_SHIFT)))
#define Vb ((int)(-0.081 * (1<<RGB2YUV_SHIFT)))

/*
 * Converts two 16bpp rgb pixels into a dual yuy2 pixel.
 */
static inline uint32_t rgbrgb16toycbycr(uint16_t rgb1, uint16_t rgb2)
{
	register int Y1, Cb, Y2, Cr;
	register int r1, g1, b1;
	register int r2, g2, b2;
	register int r, g, b;

	/* fast path, thanks to bohdy */
	if (!(rgb1 | rgb2))
		return 0x00800080;	/* black, black */

	/* RGB565 */
	r1 = ((rgb1 >> 11) & 0x1f);
	g1 = ((rgb1 >> 5) & 0x3f);
	b1 = ((rgb1 >> 0) & 0x1f);

	/* fast (approximated) scaling to 8 bits, thanks to Masken */
	r1 = (r1 << 3) | (r1 >> 2);
	g1 = (g1 << 2) | (g1 >> 4);
	b1 = (b1 << 3) | (b1 >> 2);

	Y1 = clamp(((Yr * r1 + Yg * g1 + Yb * b1) >> RGB2YUV_SHIFT)
		   + RGB2YUV_LUMA, 16, 235);
	if (rgb1 == rgb2) {
		/* this is just another fast path */
		Y2 = Y1;
		r = r1;
		g = g1;
		b = b1;
	} else {
		/* same as we did for r1 before */
		r2 = ((rgb2 >> 11) & 0x1f);
		g2 = ((rgb2 >> 5) & 0x3f);
		b2 = ((rgb2 >> 0) & 0x1f);
		r2 = (r2 << 3) | (r2 >> 2);
		g2 = (g2 << 2) | (g2 >> 4);
		b2 = (b2 << 3) | (b2 >> 2);

		Y2 = clamp(((Yr * r2 + Yg * g2 + Yb * b2) >> RGB2YUV_SHIFT)
			   + RGB2YUV_LUMA,
			   16, 235);

		r = (r1 + r2) / 2;
		g = (g1 + g2) / 2;
		b = (b1 + b2) / 2;
	}

	Cb = clamp(((Ur * r + Ug * g + Ub * b) >> RGB2YUV_SHIFT)
		   + RGB2YUV_CHROMA, 16, 240);
	Cr = clamp(((Vr * r + Vg * g + Vb * b) >> RGB2YUV_SHIFT)
		   + RGB2YUV_CHROMA, 16, 240);

	return (((uint8_t) Y1) << 24) | (((uint8_t) Cb) << 16) |
	    (((uint8_t) Y2) << 8) | (((uint8_t) Cr) << 0);
}


/*
 * Video mode timings calculation.
 *
 * Please, refer to the definition of "struct vi_mode_timings" for
 * a explanation of the different constants involved.
 *
 * References:
 * - http://www.pembers.freeserve.co.uk/World-TV-Standards
 */

static inline int vi_vmode_is_progressive(__u32 vmode)
{
	return (vmode & FB_VMODE_MASK) == FB_VMODE_NONINTERLACED;
}

static int vi_calc_horz_timings(struct vi_mode_timings *timings,
				 struct fb_var_screeninfo *var,
				 u16 width, u16 max_active_width,
				 u16 A, u8 B, u16 C, u16 D,
				 u8 f, u16 g)
{
	u16 extra_blanking, margin;

	if (width > max_active_width)
		return -EINVAL;

	/* adjusted horizontal settings */
	extra_blanking = max_active_width - width;
	margin = extra_blanking / 2;
	A += margin;
	C += extra_blanking - margin;

	timings->hlw = D / 2;
	timings->hsy = B;
	timings->hcs = f;
	timings->hce = f + g;
	timings->hbs = (D/2) - A;
	timings->hbe = B + C;

	/*
	 * Start of the blanking interval, between the first and second fields,
	 * begins after the last half line of the field.
	 */
	timings->htrap = (D / 2) + 1;

	var->left_margin = C;
	var->right_margin = A;
	var->hsync_len = B;

	return 0;
}

static int vi_ntsc_525_calc_horz_timings(struct vi_mode_timings *timings,
					  struct fb_var_screeninfo *var,
					  u16 width)
{
	u16 max_active_width;
	u16 A, C, D, g;
	u8 B, f;

	/* standard horizontal settings for 714 pixels */
	D = 858;		/* pixel clocks (H=63.556us, 13.5MHz clock) */
	max_active_width = 714;	/* (52.9us) 714.15 pixel clocks */
	B = 64;			/* ( 4.7us)  63.45 pixel clocks */
	f = 71;			/* ( 5.3us)  71.55 pixel clocks */
	g = 34;			/* ( 2.5us)  33.75 pixel clocks */
	A = 20;			/* ( 1.5us)  20.25 pixel clocks */
	C = 60;			/* ( 4.5us)  60.75 pixel clocks */

	return vi_calc_horz_timings(timings, var, width, max_active_width,
				    A, B, C, D, f, g);
}

static int vi_calc_vert_timings(struct vi_mode_timings *timings,
				struct fb_var_screeninfo *var,
				u16 height, u16 max_active_height,
				u16 P, u16 Q, u8 equ)
{
	u16 extra_blanking, margin, prb, psb;
	u8 interlace_bias;
	u8 shift;

	if (height > max_active_height)
		return -EINVAL;

	extra_blanking = max_active_height - height;	/* in frame lines */
	margin = extra_blanking / 2;			/* centered margins */
	prb = margin; 					/* in half lines */
	psb = extra_blanking - margin;			/* in half lines */

	/*
	 * Start of the blanking interval, between the first and second fields,
	 * begins after the last line of the field.
	 */
	if (vi_vmode_is_progressive(var->vmode)) {
		timings->acv = height;
		timings->vtrap = prb + height;
		interlace_bias = 0;
		shift = 1;
	} else {
		timings->acv = height / 2;
		timings->vtrap = (prb + height) / 2;
		interlace_bias = 1;
		shift = 0;
	}

	timings->equ = equ << shift;
	var->vsync_len = (3 * timings->equ) / 2; /* pre-eq + sync + post-eq */

	/*
	 * prb_* is specified as the number of half-lines since the end of
	 * the post-equalizing period.
	 * psb_* is specified as the number of half-lines from the end of
	 * the field.
	 */

	timings->ypos = margin;

	if (timings->ypos & 0x01) {
		/* odd field (1,3,5,...) */
		timings->prb_odd = (P + interlace_bias + prb) << shift;
		timings->psb_odd = (Q - interlace_bias + psb) << shift;
		timings->prb_even = (P + prb) << shift;
		timings->psb_even = (Q + psb) << shift;
	} else {
		/* even field (2,4,6,...) */
		timings->prb_even = (P + interlace_bias + prb) << shift;
		timings->psb_even = (Q - interlace_bias + psb) << shift;
		timings->prb_odd = (P + prb) << shift;
		timings->psb_odd = (Q + psb) << shift;
	}

	var->upper_margin = (Q + prb) / 2;
	var->lower_margin = (P + psb) / 2;

	return 0;
}

static int vi_ntsc_525_calc_vert_timings(struct vi_mode_timings *timings,
					 struct fb_var_screeninfo *var,
					 u16 height)
{
	u16 max_active_height;
	u16 P, Q;
	u8 equ;

	/* standard vertical settings for 484 active lines */
	max_active_height = 484;	/* 2 * 242.5 = 485 (*1) */

	/* blanking interval */
	/* from start of line 10, field 1 to end of line 20, field 1 */
	P = 2 * (20-10 + 1);
	Q = 1;	/* (*1) field line compensation for 484 vs 485 lines */

	equ = 2 * 3;	/* 3 lines of equalization */

	return vi_calc_vert_timings(timings, var, height, max_active_height,
				    P, Q, equ);
}

static int vi_pal_625_calc_timings(struct vi_mode_timings *timings,
				   struct fb_var_screeninfo *var,
				   unsigned int width, unsigned int height)
{
	u16 max_active_height, max_active_width;
	u16 A, C, D, g, P, Q;
	u8 B, f, equ;
	int error;

	/* standard horizontal settings for 702 pixels */
	D = 864;		/* pixel clocks (H=64us, 13.5MHz clock) */
	max_active_width = 702; /* (51.95us) 701.32 pixel clocks */
	B = 64;			/* ( 4.7us)   63.45 pixel clocks */
	f = 75;			/* ( 5.6us)   75.6  pixel clocks */
	g = 30;			/* ( 2.25us)  30.38 pixel clocks */
	A = 22;			/* ( 1.65us)  22.27 pixel clocks */
	C = 76;			/* ( 5.7us)   76.95 pixel clocks */

	error = vi_calc_horz_timings(timings, var, width, max_active_width,
				     A, B, C, D, f, g);
	if (error)
		goto err_out;

	/* standard vertical settings for 574 active lines */
	max_active_height = 574;	/* 2 * 287.5 = 575 (*1) */

	/* blanking interval */
	/* from start of line 6, field 1 to mid of line 23, field 1 */
	P = (2 * (23-6 + 1)) - 1;
	Q = 1;	/* (*1) field line compensation for 574 vs 575 lines */

	equ = 2 * 2.5;		/* 2.5 lines of equalization */

	error = vi_calc_vert_timings(timings, var, height, max_active_height,
				     P, Q, equ);
	if (error)
		goto err_out;

	/*
	 * Location of the 9 lines of burst blanking for each field
	 * (settings expressed in half lines).
	 */

	/* from start of line 1, field 1 to end of line 6, field 1 */
	timings->bs1 = 2 * (6-1 + 1);

	/* from start of line 1, field 1 to end of line 309, field 2 */
	timings->be1 = 2 * (309-1 + 1);

	/* from mid of line 313, field 2 to end of line 318, field 2 */
	timings->bs2 = (2 * (318-313 + 1)) - 1;

	/* from mid of line 313, field 2 to end of line 621, field 2 */
	timings->be2 = (2 * (612-617 + 1)) - 1;

	/* from start of line 1, field 3 to end of line 5, field 3 */
	timings->bs3 = 2 * (5-1 + 1);

	/* from start of line 1, field 3 to end of line 310, field 4 */
	timings->be3 = 2 * (310-1 + 1);

	/* from mid of line 313, field 4 to end of line 319, field 4 */
	timings->bs4 = (2 * (319-313 + 1)) - 1;

	/* from mid of line 313, field 4 to end of line 622, field 4 */
	timings->be4 = (2 * (622-313 + 1)) - 1;

err_out:
	return error;
}

static int vi_ntsc_525_calc_timings(struct vi_mode_timings *timings,
				    struct fb_var_screeninfo *var,
				    unsigned int width, unsigned int height)
{
	int error;

	error = vi_ntsc_525_calc_horz_timings(timings, var, width);
	if (error)
		goto err_out;

	error = vi_ntsc_525_calc_vert_timings(timings, var, height);
	if (error)
		goto err_out;

	/*
	 * Location of the 9 lines of burst blanking for each field
	 * (settings expressed in half lines).
	 */

	/* from start of line 4, field 1 to end of line 9, field 1 */
	timings->bs1 = 2 * (9-4 + 1);

	/* from start of line 4, field 1 to end of line 263, field 2 */
	timings->be1 = 2 * (263-4 + 1);

	/* from mid of line 266, field 2 to end of line 272, field 2 */
	timings->bs2 = (2 * (272-266 + 1)) - 1;

	/* from mid of line 266, field 2 to end of line 525, field 2 */
	timings->be2 = (2 * (525-266 + 1)) - 1;

	/* from start of line 4, field 3 to end of line 9, field 3 */
	timings->bs3 = 2 * (9-4 + 1);

	/* from start of line 4, field 3 to end of line 263, field 4 */
	timings->be3 = 2 * (263-4 + 1);

	/* from mid of line 266, field 4 to end of line 272, field 4 */
	timings->bs4 = (2 * (272-266 + 1)) - 1;

	/* from mid of line 266, field 4 to end of line 525, field 4 */
	timings->be4 = (2 * (525-266 + 1)) - 1;

err_out:
	return error;
}

static int vi_ntsc_525_prog_calc_timings(struct vi_mode_timings *timings,
					 struct fb_var_screeninfo *var,
					 unsigned int width,
					 unsigned int height)
{
	int error;

	error = vi_ntsc_525_calc_horz_timings(timings, var, width);
	if (error)
		goto err_out;

	error = vi_ntsc_525_calc_vert_timings(timings, var, height);
	if (error)
		goto err_out;

	/*
	 * Location of the 18 lines of burst blanking
	 * (settings expressed in half lines).
	 */

	/*
	 * |0 0 0 0 0 0|0 0 0 1 1 1|1 1 1 1 1 1|
	 * |1,2,3,4,5,6|7,8,9,0,1,2|3,4,5,6,7,8|
	 * :pre-equ    :sync       : post-equ  :
	 */

	/* from start of line 7 to end of line 18 */
	timings->bs1 = 2 * (18-7 + 1);
	timings->bs2 = timings->bs3 = timings->bs4 = timings->bs1;

	/* from start of line 7 to end of line 525 (last) */
	timings->be1 = 2 * (525-7 + 1);
	timings->be2 = timings->be3 = timings->be4 = timings->be1;

err_out:
	return error;
}



/*
 * Video hardware support.
 *
 */

static inline int vi_has_component_cable(struct vi_ctl *ctl)
{
	return vi_sel_get_component(in_be16(ctl->io_base + VI_SEL));
}

/*
 * Get video mode reported by hardware.
 * 0=NTSC, 1=PAL, 2=MPAL, 3=debug
 */
static inline enum vi_video_format vi_get_video_format(struct vi_ctl *ctl)
{
	return vi_dcr_get_fmt(in_be16(ctl->io_base + VI_DCR));
}

static inline int vi_video_format_is_ntsc(struct vi_ctl *ctl)
{
	return vi_get_video_format(ctl) == VI_FMT_NTSC;
}

static void vi_reset_video(struct vi_ctl *ctl)
{
	void __iomem *io_base = ctl->io_base;
	u16 dcr;

	dcr = in_be16(io_base + VI_DCR);
	out_be16(io_base + VI_DCR, vi_dcr_set_rst(dcr, 1));
	out_be16(io_base + VI_DCR, vi_dcr_clear_rst(dcr));
}

/*
 * Try to determine current TV video mode.
 */
static void vi_detect_tv_mode(struct vi_ctl *ctl)
{
	struct vi_tv_mode *modes = vi_tv_modes, *mode;
	void __iomem *io_base = ctl->io_base;
	char *guess = "";
	enum vi_video_format fmt;
	int ntsc_idx, pal_idx;
	u16 dcr;
	int error;

	dcr = in_be16(io_base + VI_DCR);

	ctl->has_component_cable = vi_has_component_cable(ctl);

	if ((force_scan == VI_SCAN_PROGRESSIVE &&
					ctl->has_component_cable) ||
	    (force_scan != VI_SCAN_INTERLACED &&
					ctl->has_component_cable &&
					vi_dcr_get_nin(dcr))) {
		/* progressive modes */
		ntsc_idx = VI_VM_NTSC_480p;
		pal_idx = VI_VM_PAL_480p;
	} else {
		/* interlaced modes */
		ntsc_idx = VI_VM_NTSC_480i;
		if (force_rate == VI_RATE_50Hz ||
		    (force_rate != VI_RATE_60Hz &&
		     vi_dcr_get_fmt(dcr) == VI_FMT_PAL))
			pal_idx = VI_VM_PAL_576i50;
		else
			pal_idx = VI_VM_PAL_480i60;
	}

	if (force_tv == VI_TV_PAL ||
	    (force_tv != VI_TV_NTSC && pal_idx == VI_VM_PAL_576i50))
		fmt = VI_FMT_PAL;
	else if (force_tv == VI_TV_NTSC)
		fmt = VI_FMT_NTSC;
	else {
#ifdef CONFIG_WII_AVE_RVL
		/*
		 * Look at the audio/video encoder to detect true PAL vs NTSC.
		 */
		error = vi_ave_get_video_format(ctl, &fmt);
		if (error) {
			guess = " (initial guess)";
			if (force_tv == VI_TV_PAL ||
			    pal_idx == VI_VM_PAL_576i50)
				fmt = VI_FMT_PAL;
			else
				fmt = VI_FMT_NTSC;
		}
#else
		error = 0;
		fmt = vi_get_video_format(ctl);
#endif
	}
	switch (fmt) {
	case VI_FMT_PAL:
		mode = modes + pal_idx;
		break;
	case VI_FMT_MPAL:
	case VI_FMT_DEBUG:
		/* we currently don't support MPAL or DEBUG, sorry */
		/* FALLTHROUGH */
	case VI_FMT_NTSC:
	default:
		mode = modes + ntsc_idx;
		break;
	}

	ctl->mode = mode;

	drv_printk(KERN_INFO, "%s%s\n", mode->name, guess);
}

/*
 * Initialize the video hardware for a given TV mode.
 */
static int vi_setup_tv_mode(struct vi_ctl *ctl)
{
	void __iomem *io_base = ctl->io_base;
	struct vi_mode_timings *timings = &ctl->timings;
	struct fb_var_screeninfo *var = &ctl->info->var;
	unsigned int bytes_per_pixel = var->bits_per_pixel / 8;
	struct vi_tv_mode *mode;
	int has_component_cable;
	u16 std, ppl;

	/* we need to re-detect the tv mode if the cable type changes */
	has_component_cable = vi_has_component_cable(ctl);
	if ((ctl->has_component_cable && !has_component_cable) ||
	    (!ctl->has_component_cable && has_component_cable))
		vi_detect_tv_mode(ctl);

	mode = ctl->mode;

	out_be16(io_base + VI_DCR,
		 vi_dcr_fmt((mode->lines == 625) ? VI_FMT_PAL : VI_FMT_NTSC) |
		 vi_dcr_nin((mode->flags & VI_VMF_PROGRESSIVE) ?  1 : 0) |
		 vi_dcr_enb(1));

	out_be16(io_base + VI_VTR,
		 vi_vtr_equ(timings->equ) | vi_vtr_acv(timings->acv));

	out_be32(io_base + VI_HTR0,
		 vi_htr0_hcs(timings->hcs) | vi_htr0_hce(timings->hce) |
		 vi_htr0_hlw(timings->hlw));

	out_be32(io_base + VI_HTR1,
		 vi_htr1_hbs(timings->hbs) | vi_htr1_hbe(timings->hbe) |
		 vi_htr1_hsy(timings->hsy));

	out_be32(io_base + VI_VTO,
		 vi_vto_prb(timings->prb_odd) | vi_vto_psb(timings->psb_odd));

	out_be32(io_base + VI_VTE,
		 vi_vte_prb(timings->prb_even) | vi_vte_psb(timings->psb_even));

	out_be32(io_base + VI_BBOI,
		 vi_bboi_bs1(timings->bs1) | vi_bboi_be1(timings->be1) |
		 vi_bboi_bs3(timings->bs3) | vi_bboi_be3(timings->be3));

	out_be32(io_base + VI_BBEI,
		 vi_bbei_bs2(timings->bs2) | vi_bbei_be2(timings->be2) |
		 vi_bbei_bs4(timings->bs4) | vi_bbei_be4(timings->be4));

	/* used only for 3D stuff */
	out_be32(io_base + VI_TFBR, 0);
	out_be32(io_base + VI_BFBR, 0);

	std = (var->xres_virtual * bytes_per_pixel) / VI_HORZ_WORD_SIZE;
	if (!(mode->flags & VI_VMF_PROGRESSIVE))
		std *= 2;
	ppl = _ALIGN_UP((var->xoffset & VI_HORZ_ALIGN) + var->xres,
			VI_HORZ_ALIGN+1);
	out_be16(io_base + VI_PCR,
		 vi_pcr_std(std) |
		 vi_pcr_wpl((ppl * bytes_per_pixel) / VI_HORZ_WORD_SIZE));

	/* scaler is disabled */
	out_be16(io_base + VI_HSR, vi_hsr_stp(256) | vi_hsr_hs_en(0));

	/* filter coeficient table, anti-aliasing */
	out_be32(io_base + VI_FCT0, vi_fct[0]);
	out_be32(io_base + VI_FCT1, vi_fct[1]);
	out_be32(io_base + VI_FCT2, vi_fct[2]);
	out_be32(io_base + VI_FCT3, vi_fct[3]);
	out_be32(io_base + VI_FCT4, vi_fct[4]);
	out_be32(io_base + VI_FCT5, vi_fct[5]);
	out_be32(io_base + VI_FCT6, vi_fct[6]);
	out_be32(io_base + VI_AA, 0x00ff0000);

	/* clock */
	out_be16(io_base + VI_CLK,
		 vi_clk__54mhz((mode->flags & VI_VMF_PROGRESSIVE) ? 1 : 0));

	/* superfluous, no scaler */
	out_be16(io_base + VI_HSW, vi_hsw_width(var->xres));

	/* borders for DEBUG mode encoder, not used in retail consoles */
	out_be16(io_base + VI_HBE, 0);
	out_be16(io_base + VI_HBS, 0);

	/* whatever */
	out_be16(io_base + VI_UNK1, 0x00ff);
	out_be32(io_base + VI_UNK2, 0x00ff00ff);
	out_be32(io_base + VI_UNK3, 0x00ff00ff);

#ifdef CONFIG_WII_AVE_RVL
	vi_ave_setup(ctl);
#endif

	return 0;
}

/*
 * Set the address from where the video encoder will display data on screen.
 */
void vi_set_framebuffer(struct vi_ctl *ctl, u32 addr)
{
	struct fb_info *info = ctl->info;
	void __iomem *io_base = ctl->io_base;
	u32 top, bot;
	u8 xof;

	top = bot = addr;
	if (!vi_vmode_is_progressive(info->var.vmode)) {
		if (ctl->timings.ypos & 0x01)
			top += info->fix.line_length;
		else
			bot += info->fix.line_length;
	}
	xof = (top / 2) & VI_HORZ_ALIGN;

	out_be32(io_base + VI_TFBL,
		 vi_tfbl_pob(1) | vi_tfbl_xof(xof) | vi_tfbl_fba(top >> 5));
	out_be32(io_base + VI_BFBL, vi_bfbl_pob(1) | vi_bfbl_fba(bot >> 5));
}

/*
 * Swap the visible and back pages.
 */
static inline void vi_flip_page(struct vi_ctl *ctl)
{
	ctl->visible_page ^= 1;
	vi_set_framebuffer(ctl, ctl->page_address[ctl->visible_page]);

	ctl->flip_pending = 0;
}

static void vi_enable_interrupts(struct vi_ctl *ctl, int enable)
{
	void __iomem *io_base = ctl->io_base;

	if (enable) {
		/*
		 * We use DI0 and DI1 to signal the retrace interval.
		 */

		/* start of the vertical retrace */
		out_be32(io_base + VI_DI1,
			 vi_dix_irq(1) | vi_dix_enb(1) |
			 vi_dix_vct(ctl->timings.vtrap) |
			 vi_dix_hct(ctl->timings.htrap));

		/* end of the vertical retrace */
		out_be32(io_base + VI_DI0,
			 vi_dix_irq(1) | vi_dix_enb(1) |
			 vi_dix_vct(1) |
			 vi_dix_hct(1));
	} else {
		out_be32(io_base + VI_DI0, 0);
		out_be32(io_base + VI_DI1, 0);
	}
	/* these two are currently not used */
	out_be32(io_base + VI_DI2, 0);
	out_be32(io_base + VI_DI3, 0);
}

static void vi_dispatch_vtrace(struct vi_ctl *ctl)
{
	unsigned long flags;

	spin_lock_irqsave(&ctl->lock, flags);
	if (ctl->flip_pending)
		vi_flip_page(ctl);
	spin_unlock_irqrestore(&ctl->lock, flags);

	wake_up_interruptible(&ctl->vtrace_waitq);
}

static irqreturn_t vi_irq_handler(int irq, void *dev)
{
	struct fb_info *info = dev_get_drvdata((struct device *)dev);
	struct vi_ctl *ctl = info->par;
	void __iomem *io_base = ctl->io_base;
	u32 val;

	/* DI0 and DI1 are used to account for the vertical retrace */
	val = in_be32(io_base + VI_DI0);
	if (vi_dix_get_irq(val)) {
		ctl->in_vtrace = 0;
		gcngx_dispatch_vtrace(ctl); /* backwards compatibility */

		out_be32(io_base + VI_DI0, vi_dix_clear_irq(val));
		return IRQ_HANDLED;
	}
	val = in_be32(io_base + VI_DI1);
	if (vi_dix_get_irq(val)) {
		ctl->in_vtrace = 1;
		vi_dispatch_vtrace(ctl);
		gcngx_dispatch_vtrace(ctl); /* backwards compatibility */

		out_be32(io_base + VI_DI1, vi_dix_clear_irq(val));
		return IRQ_HANDLED;
	}

	/* currently unused, just in case */
	val = in_be32(io_base + VI_DI2);
	if (vi_dix_get_irq(val)) {
		out_be32(io_base + VI_DI2, vi_dix_clear_irq(val));
		return IRQ_HANDLED;
	}
	val = in_be32(io_base + VI_DI3);
	if (vi_dix_get_irq(val)) {
		out_be32(io_base + VI_DI3, vi_dix_clear_irq(val));
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

#ifdef CONFIG_WII_AVE_RVL

/*
 * Audio/Video Encoder hardware support.
 *
 */

/*
 * I/O accessors.
 */

static int vi_ave_outs(struct i2c_client *client, u8 reg,
		       void *data, size_t len)
{
	struct i2c_adapter *adap = client->adapter;
	struct i2c_msg msg[1];
	u8 buf[34];
	s32 result;
	int error = -EINVAL;

	if (len > sizeof(buf)-1)
		goto err_out;

	buf[0] = reg;
	memcpy(&buf[1], data, len);

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = len+1;
	msg[0].buf = buf;

	result = i2c_transfer(adap, msg, 1);
	if (result < 0)
		error = result;
	else if (result == 1)
		error = 0;
	else
		error = -EIO;

err_out:
	if (error)
		drv_printk(KERN_ERR, "RVL-AVE: "
			   "error (%d) writing to register %02Xh\n",
			   error, reg);
	return error;
}

static int vi_ave_out8(struct i2c_client *client, u8 reg, u8 data)
{
	return vi_ave_outs(client, reg, &data, sizeof(data));
}

static int vi_ave_out16(struct i2c_client *client, u8 reg, u16 data)
{
	cpu_to_be16s(&data);
	return vi_ave_outs(client, reg, &data, sizeof(data));
}

static int vi_ave_out32(struct i2c_client *client, u8 reg, u32 data)
{
	cpu_to_be32s(&data);
	return vi_ave_outs(client, reg, &data, sizeof(data));
}

static int vi_ave_ins(struct i2c_client *client, u8 reg,
		      void *data, size_t len)
{
	struct i2c_adapter *adap = client->adapter;
	struct i2c_msg msg[2];
	s32 result;
	int error;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = sizeof(reg);
	msg[0].buf = &reg;

	msg[1].addr = client->addr;
	msg[1].flags = (client->flags & I2C_M_TEN) | I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = data;

	result = i2c_transfer(adap, msg, 2);
	if (result < 0)
		error = result;
	else if (result == 2)
		error = 0;
	else
		error = -EIO;

	if (error)
		drv_printk(KERN_ERR, "RVL-AVE: "
			   "error (%d) reading from register %02Xh\n",
			   error, reg);

	return error;
}

static int vi_ave_in8(struct i2c_client *client, u8 reg, u8 *data)
{
	return vi_ave_ins(client, reg, data, sizeof(*data));
}


/*
 * Try to detect current video format.
 */
static int vi_ave_get_video_format(struct vi_ctl *ctl,
				   enum vi_video_format *fmt)
{
	u8 val = 0xff;
	int error = -ENODEV;

	if (!ctl->i2c_client)
		goto err_out;

	error = vi_ave_in8(ctl->i2c_client, 0x01, &val);
	if (error)
		goto err_out;

	if ((val & 0x1f) == 2)
		*fmt = VI_FMT_PAL;
	else
		*fmt = VI_FMT_NTSC;
err_out:
	return error;
}


static u8 vi_ave_gamma[] = {
	0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00,
	0x10, 0x00, 0x10, 0x00, 0x10, 0x20, 0x40, 0x60,
	0x80, 0xa0, 0xeb, 0x10, 0x00, 0x20, 0x00, 0x40,
	0x00, 0x60, 0x00, 0x80, 0x00, 0xa0, 0x00, 0xeb,
	0x00
};

/*
 * Initialize the audio/video encoder.
 */
static int vi_ave_setup(struct vi_ctl *ctl)
{
	struct i2c_client *client;
	u8 macrovision[26];
	u8 component, format, pal60;

	if (!ctl->i2c_client)
		return -ENODEV;

	client = ctl->i2c_client;
	memset(macrovision, 0, sizeof(macrovision));

	/*
	 * Magic initialization sequence borrowed from libogc.
	 */

	vi_ave_out8(client, 0x6a, 1);
	vi_ave_out8(client, 0x65, 1);

	/*
	 * NOTE
	 * We _can't_ use the fmt field in DCR to derive "format" here.
	 * DCR uses fmt=0 (NTSC) also for PAL 525 modes.
	 */

	format = 0;		/* default to NTSC */
	if ((ctl->mode->flags & VI_VMF_PAL_COLOR) != 0)
		format = 2;	/* PAL */
	component = (ctl->has_component_cable) ? 1<<5 : 0;
	vi_ave_out8(client, 0x01, component | format);

	vi_ave_out8(client, 0x00, 0);
	vi_ave_out16(client, 0x71, 0x8e8e);
	vi_ave_out8(client, 0x02, 7);
	vi_ave_out16(client, 0x05, 0x0000);
	vi_ave_out16(client, 0x08, 0x0000);
	vi_ave_out32(client, 0x7a, 0x00000000);
	vi_ave_outs(client, 0x40, macrovision, sizeof(macrovision));
	vi_ave_out8(client, 0x0a, 0);
	vi_ave_out8(client, 0x03, 1);
	vi_ave_outs(client, 0x10, vi_ave_gamma, sizeof(vi_ave_gamma));
	vi_ave_out8(client, 0x04, 1);

	vi_ave_out32(client, 0x7a, 0x00000000);
	vi_ave_out16(client, 0x08, 0x0000);

	vi_ave_out8(client, 0x03, 1);

	/* clear bit 1 otherwise red and blue get swapped  */
	if (ctl->has_component_cable)
		vi_ave_out8(client, 0x62, 0);

	/* PAL 480i/60 supposedly needs a "filter" */
	pal60 = !!(format == 2 && ctl->mode->lines == 525);
	vi_ave_out8(client, 0x6e, pal60);

	return 0;
}

static struct vi_ctl *first_vi_ctl;
static struct i2c_client *first_vi_ave;

static int vi_attach_ave(struct vi_ctl *ctl, struct i2c_client *client)
{
	if (!ctl)
		return -ENODEV;
	if (!client)
		return -EINVAL;

	spin_lock(&ctl->lock);
	if (!ctl->i2c_client) {
		ctl->i2c_client = i2c_use_client(client);
		spin_unlock(&ctl->lock);
		drv_printk(KERN_INFO, "AVE-RVL support loaded\n");
		return 0;
	}
	spin_unlock(&ctl->lock);
	return -EBUSY;
}

static void vi_dettach_ave(struct vi_ctl *ctl)
{
	struct i2c_client *client;

	if (!ctl)
		return;

	spin_lock(&ctl->lock);
	if (ctl->i2c_client) {
		client = ctl->i2c_client;
		ctl->i2c_client = NULL;
		spin_unlock(&ctl->lock);
		i2c_release_client(client);
		drv_printk(KERN_INFO, "AVE-RVL support unloaded\n");
		return;
	}
	spin_unlock(&ctl->lock);
}

static int vi_ave_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int error;

	/* attach first a/v encoder to first framebuffer */
	if (!first_vi_ave) {
		first_vi_ave = client;
		error = vi_attach_ave(first_vi_ctl, client);
		if (!error) {
			/* setup again the video mode using the a/v encoder */
			vi_detect_tv_mode(first_vi_ctl);
			vi_setup_tv_mode(first_vi_ctl);
		}
	}
	return 0;
}

static int vi_ave_remove(struct i2c_client *client)
{
	if (first_vi_ave == client)
		first_vi_ave = NULL;
	return 0;
}

static const struct i2c_device_id vi_ave_id[] = {
	{ "wii-ave-rvl", 0 },
	{ }
};

static struct i2c_driver vi_ave_driver = {
	.driver = {
		.name	= DRV_MODULE_NAME,
	},
	.probe		= vi_ave_probe,
	.remove		= vi_ave_remove,
	.id_table	= vi_ave_id,
};

#endif /* CONFIG_WII_AVE_RVL */


/*
 * Linux framebuffer support routines.
 *
 */

/*
 * This is just a quick, dirty and cheap way of getting right colors on the
 * linux framebuffer console.
 */
unsigned int vifb_writel(unsigned int rgbrgb, void *address)
{
	uint16_t *rgb = (uint16_t *)&rgbrgb;
	return fb_writel_real(rgbrgb16toycbycr(rgb[0], rgb[1]), address);
}

static int vifb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp, struct fb_info *info)
{
	/*
	 *  Set a single color register. The values supplied are
	 *  already rounded down to the hardware's capabilities
	 *  (according to the entries in the `var' structure). Return
	 *  != 0 for invalid regno.
	 */

	if (regno >= info->cmap.len)
		return 1;

	switch (info->var.bits_per_pixel) {
	case 16:
		if (info->var.red.offset == 10) {
			/* 1:5:5:5, not used currently */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800) >> 1) |
			    ((green & 0xf800) >> 6) | ((blue & 0xf800) >> 11);
		} else {
			/* 0:5:6:5 */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800)) |
			    ((green & 0xfc00) >> 5) | ((blue & 0xf800) >> 11);
		}
		break;
	case 8:
	case 15:
	case 24:
	case 32:
		break;
	}
	return 0;
}

/*
 * Pan the display by altering the framebuffer address in hardware.
 */
static int vifb_pan_display(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	void __iomem *io_base = ctl->io_base;
	unsigned int bytes_per_pixel = info->var.bits_per_pixel / 8;
	unsigned long flags;
	int offset;
	u16 ppl;

	ppl = _ALIGN_UP((var->xoffset & VI_HORZ_ALIGN) + var->xres,
			VI_HORZ_ALIGN+1);
	out_be16(io_base + VI_PCR,
		 vi_pcr_set_wpl(in_be16(io_base + VI_PCR),
				(ppl * bytes_per_pixel) / VI_HORZ_WORD_SIZE));

	offset = (var->yoffset * info->fix.line_length) +
		 var->xoffset * bytes_per_pixel;
	vi_set_framebuffer(ctl, info->fix.smem_start + offset);

	spin_lock_irqsave(&ctl->lock, flags);
	if (info->fix.smem_start + offset >= ctl->page_address[1])
		ctl->visible_page = 1;
	else
		ctl->visible_page = 0;
	spin_unlock_irqrestore(&ctl->lock, flags);

	return 0;
}

static int vifb_check_var_timings(struct fb_var_screeninfo *var,
				  struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	struct vi_tv_mode *mode = ctl->mode;
	struct vi_mode_timings timings;
	u32 yres = var->yres;
	int error = -EINVAL;

	if (nostalgic && yres == 576)
		yres = 574;

	if (vi_vmode_is_progressive(var->vmode)) {
		/* 480p */
		error = vi_ntsc_525_prog_calc_timings(&timings, var,
						      var->xres, var->yres);
	} else {
		if (mode->lines == 625)
			/* 576i */
			error = vi_pal_625_calc_timings(&timings, var,
							var->xres, yres);
		else
			/* 480i */
			error = vi_ntsc_525_calc_timings(&timings, var,
							 var->xres, var->yres);
	}
	if (error)
		goto err_out;

	ctl->timings = timings;
	var->pixclock = KHZ2PICOS(13.5 * 1000);
	var->sync = FB_SYNC_BROADCAST;

	error = 0;

err_out:
	return error;
}

/*
 * Check var and eventually tweak it to something supported.
 * Do not modify par here.
 */
static int vifb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	struct vi_tv_mode *mode = ctl->mode;
	int error = -EINVAL;
	unsigned int bytes_per_pixel;
	__u32 xres, yres, xres_virtual, yres_virtual;

	/* we support only 16bpp */
	if (var->bits_per_pixel != 16) {
		drv_printk(KERN_ERR, "unsupported depth %u\n",
				var->bits_per_pixel);
		goto err_out;
	}

	yres = var->yres;
	if (yres & VI_VERT_ALIGN)
		yres = _ALIGN_UP(yres, VI_VERT_ALIGN+1);
	if (yres > mode->height) {
		if (!nostalgic) {
			drv_printk(KERN_ERR, "yres %u out of bounds\n", yres);
			goto err_out;
		}
		if (!(mode->height == 574 && yres == 576))
			yres = mode->height;
	}
	if (yres < 16) {
		/* XXX, fbcon will happily page fault for yres < 13 */
		yres = 16;
	}
	if (!yres)
		yres = mode->height;

	yres_virtual = var->yres_virtual;
	if (!yres_virtual || yres_virtual < yres)
		yres_virtual = yres;

	xres = var->xres;
	if (xres & VI_HORZ_ALIGN)
		xres = _ALIGN_UP(xres, VI_HORZ_ALIGN+1);
	if (xres > mode->width) {
		drv_printk(KERN_ERR, "xres %u out of bounds\n", var->xres);
		goto err_out;
	}
	if (!xres)
		xres = mode->width;

	xres_virtual = var->xres_virtual;
	if (xres_virtual & VI_HORZ_ALIGN)
		xres_virtual = _ALIGN_UP(xres_virtual, VI_HORZ_ALIGN+1);
	if (!xres_virtual || xres_virtual < xres)
		xres_virtual = xres;

	bytes_per_pixel = var->bits_per_pixel / 8;
	if (xres_virtual * yres_virtual * bytes_per_pixel >
		info->fix.smem_len) {
		drv_printk(KERN_ERR, "not enough memory for virtual resolution"
			   " (%ux%ux%u)\n",
			   xres_virtual, yres_virtual,
			   var->bits_per_pixel);
		goto err_out;
	}

	var->xres = xres;
	var->yres = yres;
	var->xres_virtual = xres_virtual;
	var->yres_virtual = yres_virtual;

	var->xoffset = 0;
	var->yoffset = 0;

	var->grayscale = 0;

	/* we support ony 16 bits per pixel */
	var->red.offset = 11;
	var->red.length = 5;
	var->green.offset = 5;
	var->green.length = 6;
	var->blue.offset = 0;
	var->blue.length = 5;
	var->transp.offset = 0;
	var->transp.length = 0;

	var->nonstd = 0;	/* lies... */

	/* enable non-interlaced mode if supported */
	if (force_scan != VI_SCAN_INTERLACED && ctl->has_component_cable) {
		var->vmode = (mode->flags & VI_VMF_PROGRESSIVE) ?
					FB_VMODE_NONINTERLACED :
					FB_VMODE_INTERLACED;
	} else
		var->vmode = FB_VMODE_INTERLACED;

	error = vifb_check_var_timings(var, info);
	if (error)
		goto err_out;

err_out:
	return error;
}

/*
 * Set the video mode according to info->var.
 */
static int vifb_set_par(struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	struct fb_var_screeninfo *var = &info->var;
	unsigned long flags;

	/* horizontal line in bytes */
	info->fix.line_length = var->xres_virtual * (var->bits_per_pixel / 8);

	ctl->page_address[0] = info->fix.smem_start;
	if (var->yres * info->fix.line_length <= info->fix.smem_len / 2)
		ctl->page_address[1] =
		    info->fix.smem_start + var->yres * info->fix.line_length;
	else
		ctl->page_address[1] = info->fix.smem_start;

	/* set page 0 as the visible page and cancel pending flips */
	spin_lock_irqsave(&ctl->lock, flags);
	ctl->visible_page = 1;
	vi_flip_page(ctl);
	spin_unlock_irqrestore(&ctl->lock, flags);

	info->flags = FBINFO_FLAG_DEFAULT;
	if (want_ypan) {
		info->fix.xpanstep = 2;
		info->fix.ypanstep = 1;
		info->flags |= FBINFO_HWACCEL_YPAN;
	} else {
		info->fix.xpanstep = 0;
		info->fix.ypanstep = 0;
	}

	vi_setup_tv_mode(ctl);

	/* enable the video retrace handling */
	vi_enable_interrupts(ctl, 1);

	return 0;
}

static int vifb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	unsigned long off;
	unsigned long start;
	u32 len;

	off = vma->vm_pgoff << PAGE_SHIFT;

	/* frame buffer memory */
	start = info->fix.smem_start;
	len = PAGE_ALIGN((start & ~PAGE_MASK) + info->fix.smem_len);
	start &= PAGE_MASK;
	if ((vma->vm_end - vma->vm_start + off) > len)
		return -EINVAL;
	off += start;
	vma->vm_pgoff = off >> PAGE_SHIFT;

	/* this is an IO map, tell maydump to skip this VMA */
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;

	/* we share RAM between the cpu and the video hardware */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
				vma->vm_end - vma->vm_start,
				vma->vm_page_prot))
		return -EAGAIN;
	return 0;
}

static int vifb_ioctl(struct fb_info *info,
		       unsigned int cmd, unsigned long arg)
{
	struct vi_ctl *ctl = info->par;
	void __user *argp;
	unsigned long flags;
	int page;

	switch (cmd) {
	case FBIOWAITRETRACE:
		interruptible_sleep_on(&ctl->vtrace_waitq);
		return signal_pending(current) ? -EINTR : 0;
	case FBIOFLIPHACK:
		/*
		 * If arg == NULL then
		 *   Try to flip the video page as soon as possible.
		 *   Returns the current visible video page number.
		 */
		if (!arg) {
			spin_lock_irqsave(&ctl->lock, flags);
			if (ctl->in_vtrace)
				vi_flip_page(ctl);
			else
				ctl->flip_pending = 1;
			spin_unlock_irqrestore(&ctl->lock, flags);
			return ctl->visible_page;
		}

		/*
		 * If arg != NULL then
		 *   Wait until the video page number pointed by arg
		 *   is not visible.
		 *   Returns the current visible video page number.
		 */
		argp = (void __user *)arg;
		if (copy_from_user(&page, argp, sizeof(int)))
			return -EFAULT;

		if (page != 0 && page != 1)
			return -EINVAL;

		spin_lock_irqsave(&ctl->lock, flags);
		ctl->flip_pending = 0;
		if (ctl->visible_page == page) {
			if (ctl->in_vtrace) {
				vi_flip_page(ctl);
			} else {
				ctl->flip_pending = 1;
				spin_unlock_irqrestore(&ctl->lock, flags);
				interruptible_sleep_on(&ctl->vtrace_waitq);
				return signal_pending(current) ?
					-EINTR : ctl->visible_page;
			}
		}
		spin_unlock_irqrestore(&ctl->lock, flags);
		return ctl->visible_page;
	}
	return -EINVAL;
}


struct fb_ops vifb_ops = {
	.owner = THIS_MODULE,
	.fb_setcolreg = vifb_setcolreg,
	.fb_pan_display = vifb_pan_display,
	.fb_ioctl = vifb_ioctl,
	.fb_set_par = vifb_set_par,
	.fb_check_var = vifb_check_var,
	.fb_mmap = vifb_mmap,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

/*
 * Driver model helper routines.
 *
 */

static int vifb_do_probe(struct device *dev,
			 struct resource *mem, unsigned int irq,
			 unsigned long xfb_start, unsigned long xfb_size)
{
	struct fb_info *info;
	struct vi_ctl *ctl;
	int video_cmap_len;
	int error = -EINVAL;

	info = framebuffer_alloc(sizeof(struct vi_ctl), dev);
	if (!info)
		goto err_framebuffer_alloc;

	info->fbops = &vifb_ops;
	info->var = vifb_var;
	info->fix = vifb_fix;

	ctl = info->par;
	ctl->info = info;

	/* first things first */
	ctl->io_base = ioremap(mem->start, mem->end - mem->start + 1);
	ctl->irq = irq;

	/*
	 * Location and size of the external framebuffer.
	 */
	info->fix.smem_start = xfb_start;
	info->fix.smem_len = xfb_size;

	if (!request_mem_region(info->fix.smem_start, info->fix.smem_len,
				DRV_MODULE_NAME)) {
		drv_printk(KERN_WARNING,
			   "failed to request video memory at %p\n",
			   (void *)info->fix.smem_start);
	}

	info->screen_base = ioremap(info->fix.smem_start, info->fix.smem_len);
	if (!info->screen_base) {
		drv_printk(KERN_ERR,
			   "failed to ioremap video memory at %p (%dk)\n",
			   (void *)info->fix.smem_start,
			   info->fix.smem_len / 1024);
		error = -EIO;
		goto err_ioremap;
	}

	spin_lock_init(&ctl->lock);
	init_waitqueue_head(&ctl->vtrace_waitq);

	vi_reset_video(ctl);
	vi_detect_tv_mode(ctl);

	if (!nostalgic) {
		/* by default, start with overscan compensation */
		info->var.xres = 576;
		if (ctl->mode->height == 574)
			info->var.yres = 516;
		else
			info->var.yres = 432;
	} else {
		info->var.xres = ctl->mode->width;
		info->var.yres = ctl->mode->height;
	}

	ctl->visible_page = 0;
	ctl->flip_pending = 0;

	drv_printk(KERN_INFO,
		   "framebuffer at 0x%p, mapped to 0x%p, size %dk\n",
		   (void *)info->fix.smem_start, info->screen_base,
		   info->fix.smem_len / 1024);

	video_cmap_len = 16;
	info->pseudo_palette = pseudo_palette;
	if (fb_alloc_cmap(&info->cmap, video_cmap_len, 0)) {
		error = -ENOMEM;
		goto err_alloc_cmap;
	}

	error = vifb_check_var(&info->var, info);
	if (error)
		goto err_check_var;

	drv_printk(KERN_INFO, "mode is %dx%dx%d\n", info->var.xres,
		   info->var.yres, info->var.bits_per_pixel);

	dev_set_drvdata(dev, info);

	vi_enable_interrupts(ctl, 0);

	error = request_irq(ctl->irq, vi_irq_handler, 0, DRV_MODULE_NAME, dev);
	if (error) {
		drv_printk(KERN_ERR, "unable to register IRQ %u\n", ctl->irq);
		goto err_request_irq;
	}

	/* now register us */
	if (register_framebuffer(info) < 0) {
		error = -EINVAL;
		goto err_register_framebuffer;
	}

#ifdef CONFIG_WII_AVE_RVL
	if (!first_vi_ctl)
		first_vi_ctl = ctl;

	/* try to attach the a/v encoder now */
	vi_attach_ave(ctl, first_vi_ave);
#endif

	printk(KERN_INFO "fb%d: %s frame buffer device\n",
	       info->node, info->fix.id);

	return 0;

err_register_framebuffer:
	free_irq(ctl->irq, 0);
err_check_var:
err_request_irq:
	fb_dealloc_cmap(&info->cmap);
err_alloc_cmap:
	iounmap(info->screen_base);
err_ioremap:
	release_mem_region(info->fix.smem_start, info->fix.smem_len);

	dev_set_drvdata(dev, NULL);
	iounmap(ctl->io_base);
	framebuffer_release(info);
err_framebuffer_alloc:
	return error;
}

static int vifb_do_remove(struct device *dev)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct vi_ctl *ctl = info->par;

	if (!info)
		return -ENODEV;

	free_irq(ctl->irq, dev);
	unregister_framebuffer(info);
	fb_dealloc_cmap(&info->cmap);
	iounmap(info->screen_base);
	release_mem_region(info->fix.smem_start, info->fix.smem_len);

	dev_set_drvdata(dev, NULL);
	iounmap(ctl->io_base);

#ifdef CONFIG_WII_AVE_RVL
	vi_dettach_ave(ctl);
	if (first_vi_ctl == ctl)
		first_vi_ctl = NULL;
#endif
	framebuffer_release(info);
	return 0;
}

static int vifb_do_shutdown(struct device *dev)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct vi_ctl *ctl = info->par;
	void __iomem *io_base = ctl->io_base;

	vi_enable_interrupts(ctl, 0);
	vi_reset_video(ctl);
	out_be16(io_base + VI_DCR, vi_dcr_enb(0));

	return 0;
}

#ifndef MODULE

static int vifb_setup(char *options)
{
	char *this_opt;

	if (!options || !*options)
		return 0;

	drv_printk(KERN_INFO, "options: %s\n", options);

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;

		if (!strcmp(this_opt, "redraw"))
			want_ypan = 0;
		else if (!strcmp(this_opt, "interlaced"))
			force_scan = VI_SCAN_INTERLACED;
		else if (!strcmp(this_opt, "progressive"))
			force_scan = VI_SCAN_PROGRESSIVE;
		else if (!strcmp(this_opt, "50Hz"))
			force_rate = VI_RATE_50Hz;
		else if (!strcmp(this_opt, "60Hz"))
			force_rate = VI_RATE_60Hz;
		else if (!strncmp(this_opt, "tv=", 3)) {
			if (!strncmp(this_opt + 3, "PAL", 3))
				force_tv = VI_TV_PAL;
			else if (!strncmp(this_opt + 3, "NTSC", 4))
				force_tv = VI_TV_NTSC;
		} else if (!strcmp(this_opt, "nostalgic"))
			nostalgic = 1;
	}

	if (force_scan == VI_SCAN_PROGRESSIVE || force_tv == VI_TV_NTSC) {
		if (force_rate == VI_RATE_50Hz) {
			drv_printk(KERN_INFO, "ignoring forced 50Hz setting\n");
			force_rate = VI_RATE_DONTCARE;
		}
	}
	return 0;
}

#endif	/* MODULE */


/*
 * OF platform driver hooks.
 *
 */

static int vifb_of_probe(struct platform_device *odev)
{
	struct resource res;
	const unsigned long *prop;
	unsigned long xfb_start, xfb_size;
	int retval;

	retval = of_address_to_resource(odev->dev.of_node, 0, &res);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return -ENODEV;
	}

	prop = of_get_property(odev->dev.of_node, "xfb-start", NULL);
	if (!prop) {
		drv_printk(KERN_ERR, "no xfb start found\n");
		return -ENODEV;
	}
	xfb_start = *prop;

	prop = of_get_property(odev->dev.of_node, "xfb-size", NULL);
	if (!prop) {
		drv_printk(KERN_ERR, "no xfb size found\n");
		return -ENODEV;
	}
	xfb_size = *prop;

	return vifb_do_probe(&odev->dev,
			     &res, irq_of_parse_and_map(odev->dev.of_node, 0),
			     xfb_start, xfb_size);
}

static int __exit vifb_of_remove(struct platform_device *odev)
{
	return vifb_do_remove(&odev->dev);
}

static void vifb_of_shutdown(struct platform_device *odev)
{
	vifb_do_shutdown(&odev->dev);
}


static struct of_device_id vifb_of_match[] = {
	{ .compatible = "nintendo,flipper-vi", },
	{ .compatible = "nintendo,hollywood-vi", },
	{ },
};

MODULE_DEVICE_TABLE(of, vifb_of_match);

static struct platform_driver vifb_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = vifb_of_match,
	},
	.probe = vifb_of_probe,
	.remove = vifb_of_remove,
	.shutdown = vifb_of_shutdown,
};

/*
 * Module interface hooks
 *
 */

static int __init vifb_init_module(void)
{
	int error;
	char *option = NULL;

	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   vifb_driver_version);

#ifndef MODULE
	if (fb_get_options(DRV_MODULE_NAME, &option))
		return -ENODEV;
	if (!option) {
		/* for backwards compatibility */
		if (fb_get_options("gcnfb", &option))
			return -ENODEV;
	}
	error = vifb_setup(option);
#endif

#ifdef CONFIG_WII_AVE_RVL
	error = i2c_add_driver(&vi_ave_driver);
	if (error)
		drv_printk(KERN_ERR, "failed to register AVE (%d)\n", error);
#endif

	return platform_driver_register(&vifb_of_driver);
}

static void __exit vifb_exit_module(void)
{
	platform_driver_unregister(&vifb_of_driver);
#ifdef CONFIG_WII_AVE_RVL
	i2c_del_driver(&vi_ave_driver);
#endif
}

module_init(vifb_init_module);
module_exit(vifb_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
