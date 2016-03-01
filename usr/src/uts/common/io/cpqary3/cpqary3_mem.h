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
 * Copyright (C) 2013 Hewlett-Packard Development Company, L.P.
 */

#ifndef	_CPQARY3_Q_MEM_H
#define	_CPQARY3_Q_MEM_H

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * This structure is meant to store the handle of the physically contiguous
 * memory blcoks that will be allocated during the _meminit().
 * The no. of blocks that will be allocated will be decide at run time
 * depending upon the maximum outstanding commands supported by the controller.
 * each block is physically contiguous & can hold 3 commands.
 */
typedef struct cpqary3_phyctg {
	size_t			real_size;
	ddi_dma_handle_t	cpqary3_dmahandle;
	ddi_acc_handle_t	cpqary3_acchandle;
	ddi_dma_cookie_t	cpqary3_dmacookie;
} cpqary3_phyctg_t;

#ifdef	__cplusplus
}
#endif

#endif	/* _CPQARY3_Q_MEM_H */
