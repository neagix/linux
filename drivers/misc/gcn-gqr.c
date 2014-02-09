/*
 * drivers/misc/gcn-gqr.c
 *
 * Nintendo GameCube GQR driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2007,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ctype.h>
#include <linux/sysctl.h>
#include <linux/types.h>
#include <linux/uaccess.h>

static u32 gqr_values[8];
static struct ctl_table_header *gqr_table_header;

#define SPR_GQR0 912
#define SPR_GQR1 913
#define SPR_GQR2 914
#define SPR_GQR3 915
#define SPR_GQR4 916
#define SPR_GQR5 917
#define SPR_GQR6 918
#define SPR_GQR7 919

#define MFSPR_CASE(i) case (i): (*((u32 *)table->data) = mfspr(SPR_GQR##i))
#define MTSPR_CASE(i) case (i): mtspr(SPR_GQR##i, *((u32 *)table->data))

static int proc_dogqr(struct ctl_table *table, int write,
		      void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int r;

	if (!write) {		/* if they are reading, update the variable */
		switch (table->data - (void *)gqr_values) {
			MFSPR_CASE(0); break;
			MFSPR_CASE(1); break;
			MFSPR_CASE(2); break;
			MFSPR_CASE(3); break;
			MFSPR_CASE(4); break;
			MFSPR_CASE(5); break;
			MFSPR_CASE(6); break;
			MFSPR_CASE(7); break;
		default:
			return -EFAULT;	/* shouldn't happen */
		}
	}

	r = proc_dointvec(table, write, buffer, lenp, ppos);

	if ((r == 0) && write) {  /* if they are writing, update the reg */
		switch (table->data - (void *)gqr_values) {
			MTSPR_CASE(0); break;
			MTSPR_CASE(1); break;
			MTSPR_CASE(2); break;
			MTSPR_CASE(3); break;
			MTSPR_CASE(4); break;
			MTSPR_CASE(5); break;
			MTSPR_CASE(6); break;
			MTSPR_CASE(7); break;
		default:
			return -EFAULT;	/* shouldn't happen */
		}
	}

	return r;
}

#define DECLARE_GQR(i) {  \
		.ctl_name     = CTL_UNNUMBERED,       \
		.procname     = "gqr" #i,         \
		.data         = gqr_values + i,   \
		.maxlen       = sizeof(int),      \
		.mode         = 0644,             \
		.proc_handler = &proc_dogqr       \
	}

static struct ctl_table gqr_members[] = {
	DECLARE_GQR(0),
	DECLARE_GQR(1),
	DECLARE_GQR(2),
	DECLARE_GQR(3),
	DECLARE_GQR(4),
	DECLARE_GQR(5),
	DECLARE_GQR(6),
	DECLARE_GQR(7),
	{ .ctl_name = 0 }
};

static struct ctl_table gqr_table[] = {
	{
		.ctl_name = CTL_UNNUMBERED,
		.procname = "gqr",
		.mode     = 0555,
		.child    = gqr_members,
	},
	{ .ctl_name = 0 }
};

int __init gcngqr_init(void)
{
	gqr_table_header = register_sysctl_table(gqr_table);
	if (!gqr_table_header) {
		printk(KERN_ERR "Unable to register GQR sysctl table\n");
		return -ENOMEM;
	}
	return 0;
}

void __exit gcngqr_exit(void)
{
	unregister_sysctl_table(gqr_table_header);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Todd Jeffreys <todd@voidpointer.org>");
module_init(gcngqr_init);
module_exit(gcngqr_exit);
