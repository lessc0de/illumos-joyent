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

uint_t
cpqary3_isr_hw_simple(caddr_t arg1, caddr_t arg2)
{
	cpqary3_t *cpq = (cpqary3_t *)arg1;
	uint32_t isr = cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS);

	mutex_enter(&cpq->cpq_mutex);

	/*
	 * Check to see if this interrupt came from the device:
	 */
	if ((isr & cpq->cpq_board->bd_intrpendmask) == 0) {
		/*
		 * Check to see if the firmware has come to rest.  If it has,
		 * this routine will panic the system.
		 */
		cpqary3_lockup_check(cpq);

		mutex_exit(&cpq->cpq_mutex);
		return (DDI_INTR_UNCLAIMED);
	}

	/*
	 * The interrupt was from our controller, so collect any pending
	 * command completions.
	 */
	cpqary3_retrieve_simple(cpq);

	/*
	 * Process any commands in the completion queue.
	 */
	cpqary3_process_finishq(cpq);

	mutex_exit(&cpq->cpq_mutex);
	return (DDI_INTR_CLAIMED);
}

/*
 * Read tags and process completion of the associated command until the supply
 * of tags is exhausted.
 */
void
cpqary3_retrieve_simple(cpqary3_t *cpq)
{
	uint32_t opq;
	uint32_t none = 0xffffffff;

	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	while ((opq = cpqary3_get32(cpq, CISS_I2O_OUTBOUND_POST_Q)) != none) {
		uint32_t tag = CISS_OPQ_READ_TAG(opq);
		cpqary3_command_t *cpcm;

		if ((cpcm = cpqary3_lookup_inflight(cpq, tag)) == NULL) {
			dev_err(cpq->dip, CE_WARN, "spurious tag %x", tag);
			continue;
		}

		avl_remove(&cpq->cpq_inflight, cpcm);
		cpcm->cpcm_status &= ~CPQARY3_CMD_STATUS_INFLIGHT;
		if (CISS_OPQ_READ_ERROR(opq) != 0) {
			cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ERROR;
		}

		/*
		 * Push this command onto the completion queue.
		 */
		list_insert_tail(&cpq->cpq_finishq, cpcm);
	}
}

/*
 * Submit a command to the controller by posting it to the Inbound Post Queue
 * Register.
 */
void
cpqary3_submit_simple(cpqary3_t *cpq, cpqary3_command_t *cpcm)
{
	cpqary3_put32(cpq, CISS_I2O_INBOUND_POST_Q, cpcm->cpcm_pa_cmd);
}

int
cpqary3_ctlr_init_simple(cpqary3_t *cpq)
{
	VERIFY(cpq->cpq_ctlr_mode == CPQARY3_CTLR_MODE_UNKNOWN);

	if (cpqary3_cfgtbl_transport_has_support(cpq,
	    CISS_CFGTBL_XPORT_SIMPLE) != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}
	cpq->cpq_ctlr_mode = CPQARY3_CTLR_MODE_SIMPLE;

	/*
	 * Disable device interrupts while we are setting up.
	 */
	cpqary3_intr_set(cpq, B_FALSE);

	if ((cpq->cpq_maxcmds = cpqary3_ctlr_get_cmdsoutmax(cpq)) == 0) {
		dev_err(cpq->dip, CE_WARN, "maximum outstanding commands set "
		    "to zero");
		return (DDI_FAILURE);
	}

	/*
	 * XXX ?
	 */
	cpq->cpq_sg_cnt = CPQARY3_SG_CNT;

	/*
	 * Zero the upper 32 bits of the address in the Controller.
	 */
	ddi_put32(cpq->cpq_ct_handle, &cpq->cpq_ct->HostWrite.Upper32Addr, 0);

	/*
	 * Set the Transport Method and flush the changes to the
	 * Configuration Table.
	 */
	cpqary3_cfgtbl_transport_set(cpq, CISS_CFGTBL_XPORT_SIMPLE);
	if (cpqary3_cfgtbl_flush(cpq) != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}

	if (cpqary3_cfgtbl_transport_confirm(cpq,
	    CISS_CFGTBL_XPORT_SIMPLE) != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}

	/*
	 * XXX It's not clear why we check this a second time, but the original
	 * driver did.
	 */
	uint32_t check_again = cpqary3_ctlr_get_cmdsoutmax(cpq);
	if (check_again != cpq->cpq_maxcmds) {
		dev_err(cpq->dip, CE_WARN, "maximum outstanding commands "
		    "changed during initialisation (was %u, now %u)",
		    cpq->cpq_maxcmds, check_again);
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}
