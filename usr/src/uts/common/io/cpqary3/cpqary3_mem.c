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

#include <sys/sdt.h>
#include "cpqary3.h"

/*
 * The Driver DMA Limit structure.
 */
static ddi_dma_attr_t cpqary3_ctlr_dma_attr = {
	DMA_ATTR_V0,	/* ddi_dma_attr version */
	0,		/* low address */
	0xFFFFFFFF,	/* high address */
	0x00FFFFFF,	/* Max DMA Counter register */
	0x20,		/* Byte Alignment */
	0x20,		/* burst sizes */
	DMA_UNIT_8,	/* minimum DMA xfer Size */
	0xFFFFFFFF,	/* maximum DMA xfer Size */
	0x0000FFFF, 	/* segment boundary restrictions */
	1,		/* scatter/gather list length */
	512,		/* device granularity */
	0		/* DMA flags */
};

/*
 * Driver device access attr struct
 */
extern ddi_device_acc_attr_t	cpqary3_dev_attributes;

caddr_t
cpqary3_alloc_phyctgs_mem(cpqary3_t *ctlr, size_t size_mempool,
    uint32_t *phyaddr, cpqary3_phyctg_t *phyctgp)
{
	size_t real_len;
	int32_t retvalue;
	caddr_t mempool = NULL;
	uint8_t cleanstat = 0;
	uint32_t cookiecnt;

	/*
	 * Allocation of Physical Contigous Memory follws:
	 * allocate a handle for this memory
	 * Use this handle in allocating memory
	 * bind the handle to this memory
	 * If any of the above fails, return a FAILURE.
	 * If all succeed, update phyaddr to the physical address of the
	 * allocated memory and return the pointer to the virtul allocated
	 * memory.
	 */

	if (DDI_SUCCESS !=
	    (retvalue = ddi_dma_alloc_handle((dev_info_t *)ctlr->dip,
	    &cpqary3_ctlr_dma_attr, DDI_DMA_DONTWAIT, 0,
	    &phyctgp->cpqary3_dmahandle))) {
		switch (retvalue) {
		case DDI_DMA_NORESOURCES:
			cmn_err(CE_CONT, "CPQary3: No resources are available "
			    "to allocate the DMA Handle\n");
			break;

		case DDI_DMA_BADATTR:
			cmn_err(CE_CONT, "CPQary3: Bad attributes in "
			    "ddi_dma_attr cannot allocate the DMA Handle \n");
			break;

		default:
			cmn_err(CE_CONT, "CPQary3: Unexpected Value %x from "
			    "call to allocate the DMA Handle \n", retvalue);
		}
		/* Calling kmem_free to free the memory */
		kmem_free(phyctgp, sizeof (cpqary3_phyctg_t));
		return (NULL);
	}

	cleanstat |= CPQARY3_DMA_ALLOC_HANDLE_DONE;

	retvalue = ddi_dma_mem_alloc(phyctgp->cpqary3_dmahandle,
	    size_mempool, &cpqary3_dev_attributes,
	    DDI_DMA_CONSISTENT, DDI_DMA_DONTWAIT, 0, &mempool, &real_len,
	    &phyctgp->cpqary3_acchandle);

	if (DDI_SUCCESS != retvalue) {
		cmn_err(CE_WARN, "CPQary3: Memory Allocation Failed: "
		    "Increase System Memory");
		cpqary3_free_phyctgs_mem(phyctgp, cleanstat);
		return (NULL);
	}

	phyctgp->real_size = real_len;

	cleanstat |= CPQARY3_DMA_ALLOC_MEM_DONE;

	retvalue = ddi_dma_addr_bind_handle(phyctgp->cpqary3_dmahandle,
	    NULL, mempool, real_len,
	    DDI_DMA_CONSISTENT | DDI_DMA_RDWR, DDI_DMA_DONTWAIT, 0,
	    &phyctgp->cpqary3_dmacookie, &cookiecnt);

	if (DDI_DMA_MAPPED == retvalue) {
		*phyaddr = phyctgp->cpqary3_dmacookie.dmac_address;
		return (mempool);
	}

	switch (retvalue) {
	case DDI_DMA_PARTIAL_MAP:
		cmn_err(CE_CONT, "CPQary3: Allocated the resources for part "
		    "of the object\n");
		break;

	case DDI_DMA_INUSE:
		cmn_err(CE_CONT, "CPQary3: Another I/O transaction is using "
		    "the DMA handle cannot bind to the DMA Handle\n");
		break;

	case DDI_DMA_NORESOURCES:
		cmn_err(CE_CONT, "CPQary3: No resources are available cannot "
		    "bind to the DMA Handle\n");
		break;

	case DDI_DMA_NOMAPPING:
		cmn_err(CE_CONT, "CPQary3: Object cannot be reached by the "
		    "device cannot bind to the DMA Handle\n");
		break;

	case DDI_DMA_TOOBIG:
		cmn_err(CE_CONT, "CPQary3: The object is too big cannot bind "
		    "to the DMA Handle\n");
		cmn_err(CE_WARN, "CPQary3: Mem Scarce : "
		    "Increase System Memory/lomempages");
		break;

	default:
		cmn_err(CE_WARN, "CPQary3 : Unexpected Return Value %x "
		    "from call to bind the DMA Handle", retvalue);
	}

	cpqary3_free_phyctgs_mem(phyctgp, cleanstat);

	mempool = NULL;
	return (mempool);
}

void
cpqary3_free_phyctgs_mem(cpqary3_phyctg_t *cpqary3_phyctgp, uint8_t cleanstat)
{

	if (cpqary3_phyctgp == NULL)
		return;

	/*
	 * Following the reverse prcess that was followed
	 * in allocating physical contigous memory
	 */

	if (cleanstat & CPQARY3_DMA_BIND_ADDR_DONE) {
		(void) ddi_dma_unbind_handle(
		    cpqary3_phyctgp->cpqary3_dmahandle);
	}

	if (cleanstat & CPQARY3_DMA_ALLOC_MEM_DONE) {
		ddi_dma_mem_free(&cpqary3_phyctgp->cpqary3_acchandle);
	}

	if (cleanstat & CPQARY3_DMA_ALLOC_HANDLE_DONE) {
		ddi_dma_free_handle(&cpqary3_phyctgp->cpqary3_dmahandle);
	}
}
