/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2016 Joyent, Inc.
 */

#ifndef	_CPQARY3_IOCTL_H
#define	_CPQARY3_IOCTL_H

#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 *	Ioctl Commands
 */
#define	CPQARY3_IOCTL_CMD		('c' << 4)
#define	CPQARY3_IOCTL_PASSTHROUGH	CPQARY3_IOCTL_CMD | (0x01)

typedef struct cpqary3_ioctl_req {
	uint32_t		cppt_for_read;
	void			*cppt_bufp;
	uint32_t		cppt_bufsz;
	uint32_t		cppt_cdblen;
	uint8_t			cppt_cdb[16];
} cpqary3_ioctl_req_t;

#ifdef _KERNEL
typedef struct cpqary3_ioctl_req32 {
	uint32_t		cppt_for_read;
	caddr32_t		cppt_bufp;
	uint32_t		cppt_bufsz;
	uint32_t		cppt_cdblen;
	uint8_t			cppt_cdb[16];
} cpqary3_ioctl_req32_t;
#endif

#ifdef	__cplusplus
}
#endif

#endif	/* _CPQARY3_IOCTL_H */
