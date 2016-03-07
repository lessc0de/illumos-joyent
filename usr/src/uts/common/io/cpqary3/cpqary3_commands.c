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

static int
cpqary3_check_command_type(cpqary3_command_type_t type)
{
	/*
	 * Note that we leave out the default case in order to utilise
	 * compiler warnings about missed enum values.
	 */
	switch (type) {
	case CPQARY3_CMDTYPE_ABORTQ:
	case CPQARY3_CMDTYPE_SCSA:
	case CPQARY3_CMDTYPE_INTERNAL:
		return (type);
	}

	panic("unexpected command type");
}

cpqary3_command_t *
cpqary3_command_alloc(cpqary3_t *cpq, cpqary3_command_type_t type,
    int kmflags)
{
	cpqary3_command_t *cpcm;

	VERIFY(kmflags == KM_SLEEP || kmflags == KM_NOSLEEP);

	if ((cpcm = kmem_zalloc(sizeof (*cpcm), kmflags)) == NULL) {
		return (NULL);
	}

	cpcm->cpcm_ctlr = cpq;
	cpcm->cpcm_type = cpqary3_check_command_type(type);

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
	    contig_size, &cpcm->cpcm_pa_cmd, &cpcm->cpcm_phyctg,
	    kmflags)) == NULL) {
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
	cpcm->cpcm_va_cmd->ErrDesc.Addr = cpcm->cpcm_pa_err;
	cpcm->cpcm_va_cmd->ErrDesc.Len = sizeof (ErrorInfo_t);

	/*
	 * Insert into the per-controller command list.
	 */
	mutex_enter(&cpq->cpq_mutex);
	list_insert_tail(&cpq->cpq_commands, cpcm);
	mutex_exit(&cpq->cpq_mutex);

	return (cpcm);
}

int
cpqary3_command_attach_internal(cpqary3_t *cpq, cpqary3_command_t *cpcm,
    size_t len, int kmflags)
{
	cpqary3_command_internal_t *cpcmi;

	VERIFY(kmflags == KM_SLEEP || kmflags == KM_NOSLEEP);

	if ((cpcmi = kmem_zalloc(sizeof (*cpcmi), kmflags)) == NULL) {
		return (ENOMEM);
	}

	if ((cpcmi->cpcmi_va = (void *)cpqary3_alloc_phyctgs_mem(cpq, len,
	    &cpcmi->cpcmi_pa, &cpcmi->cpcmi_phyctg, kmflags)) == NULL) {
		kmem_free(cpcmi, sizeof (*cpcmi));
		return (ENOMEM);
	}

	bzero(cpcmi->cpcmi_va, cpcmi->cpcmi_len);

	cpcm->cpcm_internal = cpcmi;

	cpcm->cpcm_va_cmd->SG[0].Addr = cpcmi->cpcmi_pa;
	cpcm->cpcm_va_cmd->SG[0].Len = len;
	cpcm->cpcm_va_cmd->Header.SGList = 1;
	cpcm->cpcm_va_cmd->Header.SGTotal = 1;

	return (0);
}

void
cpqary3_command_reuse(cpqary3_command_t *cpcm)
{
	cpqary3_t *cpq = cpcm->cpcm_ctlr;

	mutex_enter(&cpq->cpq_mutex);

	/*
	 * Make sure the command is not currently inflight, then
	 * reset the command status.
	 */
	VERIFY(!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_INFLIGHT));
	cpcm->cpcm_status = CPQARY3_CMD_STATUS_REUSED;

	/*
	 * Clear the previous tag value.
	 */
	cpcm->cpcm_tag = 0;
	cpcm->cpcm_va_cmd->Header.Tag.tag_value = 0;

	mutex_exit(&cpq->cpq_mutex);
}

void
cpqary3_command_free(cpqary3_command_t *cpcm)
{
	cpqary3_t *cpq = cpcm->cpcm_ctlr;

	/*
	 * Ensure the object we are about to free is not currently in the
	 * inflight AVL.
	 */
	VERIFY(!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_INFLIGHT));

	if (cpcm->cpcm_internal != NULL) {
		cpqary3_command_internal_t *cpcmi = cpcm->cpcm_internal;

		cpqary3_free_phyctgs_mem(&cpcmi->cpcmi_phyctg,
		    CPQARY3_FREE_PHYCTG_MEM);
		kmem_free(cpcmi, sizeof (*cpcmi));
	}

	cpqary3_free_phyctgs_mem(&cpcm->cpcm_phyctg, CPQARY3_FREE_PHYCTG_MEM);

	mutex_enter(&cpq->cpq_mutex);
	list_remove(&cpq->cpq_commands, cpcm);
	mutex_exit(&cpq->cpq_mutex);

	kmem_free(cpcm, sizeof (*cpcm));
}

cpqary3_command_t *
cpqary3_lookup_inflight(cpqary3_t *cpq, uint32_t tag)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	cpqary3_command_t srch;

	srch.cpcm_tag = tag;

	return (avl_find(&cpq->cpq_inflight, &srch, NULL));
}
