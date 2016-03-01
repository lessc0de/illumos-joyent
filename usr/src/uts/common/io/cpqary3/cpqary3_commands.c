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

#include "cpqary3.h"

static size_t
cpqary3_round_up(size_t offset)
{
	size_t gran = 0x20;

	return ((offset + (gran - 1)) & ~(gran - 1));
}

cpqary3_command_t *
cpqary3_command_alloc(cpqary3_t *cpq, cpqary3_command_type_t type)
{
	cpqary3_command_t *cpcm;

	if ((cpcm = kmem_zalloc(sizeof (*cpcm), KM_NOSLEEP)) == NULL) {
		return (NULL);
	}

	cpcm->cpcm_ctlr = cpq;

	switch (type) {
	case CPQARY3_CMDTYPE_OS:
		cpcm->cpcm_complete = cpqary3_oscmd_complete;
		break;

	case CPQARY3_CMDTYPE_SYNCCMD:
		cpcm->cpcm_complete = cpqary3_synccmd_complete;
		break;

	default:
		panic("unexpected type");
	}
	cpcm->cpcm_type = type;

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
	 * Populate Fields.
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
cpqary3_command_reuse(cpqary3_command_t *cpcm)
{
	cpqary3_t *cpq = cpcm->cpcm_ctlr;

	mutex_enter(&cpq->sw_mutex);

	/*
	 * Make sure the command is not currently inflight.
	 */
	VERIFY(!cpcm->cpcm_inflight);
	if (!cpcm->cpcm_used) {
		/*
		 * If the command has not yet been issued to the controller,
		 * this is a no-op.
		 */
		mutex_exit(&cpq->sw_mutex);
		return;
	}
	cpcm->cpcm_used = B_FALSE;

	/*
	 * Grab a new tag number for this command.  We aim to avoid reusing tag
	 * numbers as much as possible, so as to avoid spurious double
	 * completion from the controller.
	 */
	cpcm->cpcm_tag = cpq->cpq_next_tag;
	if (++cpq->cpq_next_tag > 0xfffff) {
		cpq->cpq_next_tag = 0x54321;
	}

	/*
	 * Populate fields.
	 */
	cpcm->cpcm_va_cmd->Header.Tag.tag_value = cpcm->cpcm_tag;

	mutex_exit(&cpq->sw_mutex);
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
