/*
 * linux/include/linux/nfsd/stats.h
 *
 * Statistics for NFS server.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef _UAPILINUX_NFSD_STATS_H
#define _UAPILINUX_NFSD_STATS_H

#include <linux/nfs4.h>

/* thread usage wraps every one hundred thousand seconds (approx one day) */
#define	NFSD_USAGE_WRAP	(HZ*100000)

#endif /* _UAPILINUX_NFSD_STATS_H */
