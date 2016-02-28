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

static caddr_t cpqary3_alloc_phyctgs_mem(cpqary3_t *, size_t, uint32_t *,
    cpqary3_phyctg_t *);
static void cpqary3_free_phyctgs_mem(cpqary3_phyctg_t *, uint8_t);

/*
 * Local Functions Definitions
 */
uint8_t	cleanstatus = 0;

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

/*
 * Function	: 	cpqary3_alloc_phyctgs_mem
 * Description	: 	This routine allocates Physically Contiguous Memory
 *			for Commands or Scatter/Gather.
 * Called By	:	cpqary3_meminit(), cpqary3_send_NOE_command()
 *			cpqary3_synccmd_alloc()
 * Parameters	: 	per-controller, size,
 *			physical address that is sent back, per-physical
 * Calls	:	cpqary3_free_phyctgs_mem(), ddi_dma_addr_bind_handle(),
 *			ddi_dma_alloc_handle(), ddi_dma_mem_alloc()
 * Return Values: 	Actually, this function sends back 2 values, one as an
 *			explicit return and the other by updating a
 * 			pointer-parameter:
 * 			Virtual Memory Pointer to the allocated Memory(caddr_t),
 * 			Physical Address of the allocated Memory(phyaddr)
 */
static caddr_t
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

/*
 * Function	: 	cpqary3_free_phyctg_mem ()
 * Description	: 	This routine frees the Physically contigous memory
 *			that was allocated using ddi_dma operations.
 *			It also fress any related memory that was occupied.
 * Called By	: 	cpqary3_alloc_phyctgs_mem(), cpqary3_memfini(),
 *			cpqary3_send_NOE_command(), cpqary3_NOE_handler(),
 *			cpqary3_synccmd_alloc(), cpqary3_synccmd_cleanup()
 * Parameters	: 	per-physical, identifier(what all to free)
 * Calls	: 	None
 */
static void
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

static size_t
cpqary3_round_up(size_t offset)
{
	size_t gran = 0x20;

	return ((offset + (gran - 1)) & ~(gran - 1));
}

cpqary3_command_t *
cpqary3_command_alloc(cpqary3_t *cpq)
{
	cpqary3_command_t *cpcm;

	if ((cpcm = kmem_zalloc(sizeof (*cpcm), KM_NOSLEEP)) == NULL) {
		return (NULL);
	}

	cpcm->cpcm_ctlr = cpq;

	/*
	 * Grab a new tag number for this command.  We aim to avoid reusing tag
	 * numbers as much as possible, so as to avoid spurious double
	 * completion from the controller.
	 */
	mutex_enter(&cpq->sw_mutex);
	cpcm->cpcm_tag = cpq->cpq_next_tag;
	if (++cpq->cpq_next_tag > 0xfffff) {
		cpq->cpq_next_tag = 0x54321;
	}
	mutex_exit(&cpq->sw_mutex);

	size_t contig_size = 0;
	size_t errorinfo_offset;

	contig_size += cpqary3_round_up(sizeof (CommandList_t));

	errorinfo_offset = contig_size;
	contig_size += cpqary3_round_up(sizeof (ErrorInfo_t));

	/*
	 * Allocate physmem for CommandList_t (cpcm_va_cmd), and ErrorInfo_t
	 * (cpcm_va_err).
	 * XXX
	 *
	 * 	- 0x20 aligned CommandList_t
	 * 		header
	 * 		request block
	 * 		error descriptor
	 * 		scatter/gather list
	 *	- 0x20 aligned ErrorInfo_t
	 *	- ...?
	 */

	if ((cpcm->cpcm_va_cmd = (void *)cpqary3_alloc_phyctgs_mem(cpq,
	    contig_size, &cpcm->cpcm_pa_cmd, &cpcm->cpcm_phyctg)) == NULL) {
		kmem_free(cpcm, sizeof (*cpcm));
		return (NULL);
	}
	cpcm->cpcm_va_err = (void *)((caddr_t)cpcm->cpcm_va_cmd +
	    errorinfo_offset);
	cpcm->cpcm_pa_err = cpcm->cpcm_pa_cmd + errorinfo_offset;

	/*
	 * Ensure we asked for, and received, the correct physical alignment:
	 */
	VERIFY0(cpcm->cpcm_pa_cmd & 0x1f);
	VERIFY0(cpcm->cpcm_pa_err & 0x1f);

	/*
	 * XXX Populate Fields.
	 */
	bzero(cpcm->cpcm_va_cmd, contig_size);
	cpcm->cpcm_va_cmd->Header.Tag.tag_value = cpcm->cpcm_tag;
	cpcm->cpcm_va_cmd->ErrDesc.Addr = cpcm->cpcm_pa_err;
	cpcm->cpcm_va_cmd->ErrDesc.Len = sizeof (ErrorInfo_t);

	/*
	 * Insert into the per-controller command list.
	 */
	mutex_enter(&cpq->sw_mutex);
	list_insert_tail(&cpq->cpq_commands, cpcm);
	mutex_exit(&cpq->sw_mutex);

	return (cpcm);
}

cpqary3_command_internal_t *
cpqary3_command_internal_alloc(cpqary3_t *cpq, size_t len)
{
	cpqary3_command_internal_t *cpcmi;

	if ((cpcmi = kmem_zalloc(sizeof (*cpcmi), KM_NOSLEEP)) == NULL) {
		return (NULL);
	}

	if ((cpcmi->cpcmi_va = (void *)cpqary3_alloc_phyctgs_mem(cpq, len,
	    &cpcmi->cpcmi_pa, &cpcmi->cpcmi_phyctg)) == NULL) {
		kmem_free(cpcmi, sizeof (*cpcmi));
		return (NULL);
	}

	bzero(cpcmi->cpcmi_va, cpcmi->cpcmi_len);

	return (cpcmi);
}

void
cpqary3_command_free(cpqary3_command_t *cpcm)
{
	cpqary3_t *cpq = cpcm->cpcm_ctlr;

	/*
	 * Ensure the object we are about to free is not currently in the
	 * inflight AVL.
	 */
	VERIFY(!cpcm->cpcm_inflight);

	if (cpcm->cpcm_internal != NULL) {
		cpqary3_command_internal_t *cpcmi = cpcm->cpcm_internal;

		cpqary3_free_phyctgs_mem(&cpcmi->cpcmi_phyctg,
		    CPQARY3_FREE_PHYCTG_MEM);
		kmem_free(cpcmi, sizeof (*cpcmi));
	}

	cpqary3_free_phyctgs_mem(&cpcm->cpcm_phyctg, CPQARY3_FREE_PHYCTG_MEM);

	mutex_enter(&cpq->sw_mutex);
	list_remove(&cpq->cpq_commands, cpcm);
	mutex_exit(&cpq->sw_mutex);

	kmem_free(cpcm, sizeof (*cpcm));
}
