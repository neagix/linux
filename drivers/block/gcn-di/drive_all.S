/*
 * DVD+/-R compatible "cactus" firmware extensions
 * Copyright (C) 2005-2009 The GameCube Linux Team
 * Copyright (C) 2005,2006,2009 Albert Herranz
 *
 * Originally based on analysis of Cobra 1.0 drive code released by tmbinc
 * on dextrose.com.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 * This code is compatible with binutils 2.15 limited mn10200 support.
 * And it intentionally lacks the audio fix and DRE recovery features.
 *
 */


.equ UNICR,			0xfc44
.equ 	UNID,			(1<<0)
.equ ADB0,			0xfcd2
.equ ADB1,			0xfcd6
.equ ADBCTL,			0xfcda
.equ 	ADB0CK,			(1<<0)
.equ 	ADB1CK,			(1<<1)
.equ 	ADB0ON,			(1<<2)
.equ 	ADB1ON,			(1<<3)


.equ irq_handler_vector,	0x804c		/* 04, 06, 08, Panasonic Q */
.equ irq_depth,			0x805b		/* 04, 06, 08, Panasonic Q */

.equ fake_command,		0xaa
.equ set_drive_status_command,	0xee		/* same as in gcn-di.c */
.equ enable_extensions_command,	0x55		/* same as in gcn-di.c */

.equ get_drive_status_command,	0xe0


#if DRIVE_MODEL == 0x20020402 /* 04 */

	.equ cmdbuf0,			0x80b4
	.equ drive_status,		0x81b0
	.equ drive_status2,		0x8194

	.equ bert,			0x8084
	.equ ernie,			0x819c
	.equ cactus,			0x40ecf9

	.section	absolute

	.equ adb1_break_address,	0x089cd6
	.org 0x089d4e
	adb1_fixup_exit:

	.equ adb0_break_address,	0x08ae28
	.org 0x08ae33
	adb0_fixup_exit:

	.equ disable_extensions_when_called_from,	0x0885c1 /* 04, 08 */

#elif DRIVE_MODEL == 0x20010608 /* 06 */

	.equ cmdbuf0,			0x80a8
	.equ drive_status,		0x81a4
	.equ drive_status2,		0x8188

	.equ bert,			0x8078
	.equ ernie,			0x8190
	.equ cactus,			0x40ed02

	.section	absolute

	.equ adb1_break_address,	0x089d42
	.org 0x089dd0
	adb1_fixup_exit:

	.equ adb0_break_address,	0x08b145
	.org 0x08b150
	adb0_fixup_exit:

	.equ disable_extensions_when_called_from,	0x0885b9

#elif DRIVE_MODEL == 0x20020823 /* 08 */

	.equ cmdbuf0,			0x80b0
	.equ drive_status,		0x81ac
	.equ drive_status2,		0x8190

	.equ bert,			0x8080
	.equ ernie,			0x8198
	.equ cactus,			0x40ecf5

	.section	absolute

	.equ adb1_break_address,	0x089d32
	.org 0x089da7
	adb1_fixup_exit:

	.equ adb0_break_address,	0x08ae75
	.org 0x08ae80
	adb0_fixup_exit:

	.equ disable_extensions_when_called_from,	0x0885c1 /* 04, 08 */

#elif DRIVE_MODEL == 0x20010831 /* Panasonic Q */

	.equ cmdbuf0,			0x80aa
	.equ drive_status,		0x81a6
	.equ drive_status2,		0x818a

	.equ bert,			0x8078
	.equ ernie,			0x8192
	.equ cactus,			0x40ed02

	.section	absolute

	.equ adb1_break_address,	0x089e39
	.org 0x089f84
	adb1_fixup_exit:

	.equ adb0_break_address,	0x08b302
	.org 0x08b30d
	adb0_fixup_exit:

	.equ disable_extensions_when_called_from,	0x08867f

#else
	#error Sorry, unsupported drive.
#endif


	.section	.text
	.global		_start
	.global		_exit

/*
 * We are launched now through the 'func' debug command.
 */
_start:
_main:
	/* disable interrupts, do not disturb */
	and	0xf7ff, psw

	/* replace the current irq handler with ours */
	mov	our_irq_handler, a0
	mov	a0, (irq_handler_vector)

	/* setup our extending functions ... */
	mov	adb1_break_address, a0
	mov	a0, (ADB1)
	mov	adb0_break_address, a0
	mov	a0, (ADB0)

	/* ... and enable them */
	mov	ADB1ON|ADB0ON, d0
	movb	d0, (ADBCTL)

	rts


our_irq_handler:
	/* check for Address Break 0 */
	mov	(ADBCTL), d0
	and	ADB0CK, d0
	bne	adb0_break_handler

	/* check for Address Break 1 */
	mov	(ADBCTL), d0
	and	ADB1CK, d0
	bne	adb1_break_handler

	/* XXX not sure about this one... */
//	mov	0x0c, d0
//	movb	d0, (0x819a)

	/* tell the drive to please accept the disk */
	mov	cactus, a0
	mov	2, d0
	bset	d0, (a0)

	/* this seems to avoid errors if the drive idles for too long */
	mov	(bert), d0
	mov	d0, (ernie)

	/* save current command ... */
	mov	cmdbuf0, a0
	movbu	(0, a0), d0
	movb	d0, (saved_cmdbuf0)

	/* ... and place a temporary fake command, to detect new commands */
	mov	fake_command, d0
	movb	d0, (a0)

	/* call the original handler */
	mov	(saved_irq_handler), a0
	jsr	(a0)

	/* if our fake command changed, we assume a new command has arrived */
	mov	cmdbuf0, a0
	movbu	(0, a0), d0
	cmp	fake_command, d0
	bne	extra_command_parser

	/* if there is no new command, restore the previously saved command */
	movb	(saved_cmdbuf0), d0
	movb	d0, (a0)

	rts


extra_command_parser:
	/* "set drive status" command */
	cmp	set_drive_status_command, d0
	beq	set_drive_status

	/* "enable extensions" command */
	cmp	enable_extensions_command, d0
	bne	done

enable_or_disable_extensions:
	/* 0x55, 0xZZ, 0x00, 0x00 */
	/* ZZ=0 disable, otherwise enable */
	movbu	(cmdbuf0+1), d0
	jsr	di_enable_or_disable_extensions
	jmp	get_drive_status

set_drive_status:
	/* 0xee, 0xZZ, 0xYY, 0x00 */
	/* ZZ=drive_status, YY=drive_status2 */
	movbu	(cmdbuf0+1), d0
	movb	d0, (drive_status)
	movbu	(cmdbuf0+2), d0
	movb	d0, (drive_status2)


get_drive_status:
	/*
	 * This saves us an invalid command error and updates the status
	 * accordingly. In fact, our extended command becomes a "get status"
	 * command.
	 */
	mov	cmdbuf0, a0
	mov	get_drive_status_command, d0
	movb	d0, (a0)
	sub	d0, d0
	movb	d0, (1,a0)
	movb	d0, (2,a0)
	movb	d0, (3,a0)

done:
	rts


/*
 * This is how the stacks look like when our interrupt handler is called.
 *
 * Our interrupt handler is in fact not the real interrupt handler, but
 * just a subroutine called by the real interrupt handler.
 * That's why we just RTS and not RTI from our interrupt handler.
 *
 *    |        |            |        |
 *  00| d0    0| <- old a3  |        |
 *  02|       8|            |        |
 *  04| d1    6|            |        |
 *  06|       4|            |        |
 *  08| d2    2|            |        |
 *  0a|       0|            |        |
 *  0c| d3    8|            |        |
 *  0e|       6|            |        |
 *  10| a0    4|            |        |
 *  12|       2|            |        |
 *  14| a1    0|            |        |
 *  16|       8|            |        |
 *  18| a2    6|            |        |
 *  1a|       4|            |        |
 *  1c| MDR   2|            |        |
 *  1e| PSW    |            |        |
 *  20| PC lo  |            | PC lo  | <- a3
 *  22| PC hi  |            | PC hi  |
 *    :        :            | old a3 |
 *    | ...    |            |        |
 *    +--------+            +--------+ <- (0x8ea1c) for drive 04
 *    normal context stack  interrupt context stack
 *
 */

adb0_break_handler:
	mov	(ADBCTL), d0
	and	~ADB0CK, d0
	mov	(ADB0), a1
	jmp	address_break_handler

adb1_break_handler:
	mov	(ADBCTL), d0
	and	~ADB1CK, d0
	mov	(ADB1), a1

address_break_handler:
	/* ack the interrupt */
	movb	d0, (ADBCTL)
	movbu	(UNICR), d0
	and	~UNID, d0
	movb	d0, (UNICR)

	cmp	adb0_break_address, a1
	bne	1f
	mov	adb0_fixup, a1
	jmp	2f

1:
	cmp	adb1_break_address, a1
	bne	2f
	mov	adb1_fixup, a1

2:
	/* point to the previous stack pointer */
	mov	a3, a0
	add	4, a0

	/*
	 * Special case. When entering interrupt context the first time,
	 * the old stack is pushed in the interrupt stack before calling us.
	 */
	movbu	(irq_depth), d0
	cmp	1, d0
	bne	1f
	mov	(4, a3), a0	/* get the old stack pointer */
1:
	/* overwrite the original return address (look at the stack layout) */
	mov	a1, (0x20, a0)

	/*
	 * We disable the extensions when an original disc is found.
	 *
	 * We do that by checking if we were called from a piece of
	 * code reached only when original discs are inserted. Tricky.
	 */

	/* 0x20 + 0x10 + 0x04 = 0x34 */
	mov	(0x34, a0), a1
	cmp	disable_extensions_when_called_from, a1
	bne	9f	/* else, do nothing */

di_disable_extensions:
	mov	0, d0

di_enable_or_disable_extensions:
	/* enable additional media if extensions are enabled */
	mov	0, d1
	cmp	0, d0
	beq	1f
	mov	ADB0ON|ADB1ON, d1
1:
	movb	d1, (ADBCTL)

	mov	enable_extensions, a1
	movb	d0, (a1)

9:
	rts

di_enable_extensions:
	mov	1, d0
	jmp	di_enable_or_disable_extensions


adb0_fixup:
	/* disable interrupts, XXX really needed here...? */
	and	0xf7ff, psw

	/* check if we need to tweak things or not */
	movbu	(enable_extensions), d1
	cmp	0, d1
	beq	1f

	/* deal with the dvd seed */
	mov	(a0), d1
	cmp	0x00f000, d1		/* controller setup */
	bne	2f
	mov	0x00, d0		/* seed 0x00 for normal DVD */
	movb	d0, (0x09, a0)
2:
	/* skip the extra field */
	cmp	0x06, d1		/* transfer sector buffer */
	bne	1f
	mov	(0x06, a0), d1
	add	6, d1			/* skip it */
	mov	d1, (0x06, a0)
1:
	jmp	adb0_fixup_exit


adb1_fixup:
	jmp	adb1_fixup_exit


.align 2
saved_irq_handler:
#if DRIVE_MODEL != 0x20010831
	.long	0x00080A74	/* 04, 06, 08 */
#else
	.long	0x00080AA4	/* Panasonic Q */
#endif
enable_extensions:
	.byte	0x01
saved_cmdbuf0:
	.byte	0x00

_exit:

