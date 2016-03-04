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

/*
 * This routine is executed every 15 seconds via ddi_periodic_add(9F).  It
 * checks the health of the controller and looks for submitted commands that
 * have timed out.
 */
void
cpqary3_periodic(void *arg)
{
	cpqary3_t *cpq = arg;
	clock_t now;
	cpqary3_command_t *cpcm, *cpcm_next;

	mutex_enter(&cpq->cpq_mutex);

	/*
	 * Check on the health of the controller firmware.  Note that if the
	 * controller has locked up, this routine will panic the system.
	 */
	cpqary3_lockup_check(cpq);

	/*
	 * Run the retrieval routine, in case the controller has become
	 * stuck or we have somehow missed an interrupt.
	 */
	(void) cpqary3_retrieve(cpq);

	/*
	 * Check inflight commands to see if they have timed out.
	 */
	now = ddi_get_lbolt();
	for (cpcm = avl_first(&cpq->cpq_inflight); cpcm != NULL;
	    cpcm = cpcm_next) {
		cpqary3_pkt_t *privp;
		struct scsi_pkt *pkt;
		clock_t exp;

		/*
		 * Save the next entry now, in case we need to remove this one
		 * from the AVL tree.
		 */
		cpcm_next = AVL_NEXT(&cpq->cpq_inflight, cpcm);

		/*
		 * We only need to process command timeout for commands
		 * issued on behalf of SCSA.
		 */
		if (cpcm->cpcm_type != CPQARY3_CMDTYPE_OS ||
		    (privp = cpcm->cpcm_private) == NULL ||
		    (pkt = privp->scsi_cmd_pkt) == NULL) {
			continue;
		}

		if (pkt->pkt_time == 0) {
			/*
			 * This SCSI command has no timeout.
			 */
			continue;
		}

		if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLLED) {
			/*
			 * Polled commands are timed out by the polling
			 * routine.
			 */
			continue;
		}

		exp = CPQARY3_SEC2HZ(pkt->pkt_time) + cpcm->cpcm_time_submit;
		if (exp < now) {
			/*
			 * This command has timed out.  Set the appropriate
			 * status bit and push it onto the completion
			 * queue.
			 */
			cpcm->cpcm_status |= CPQARY3_CMD_STATUS_TIMEOUT;
			cpcm->cpcm_status &= ~CPQARY3_CMD_STATUS_INFLIGHT;
			avl_remove(&cpq->cpq_inflight, cpcm);
			list_insert_tail(&cpq->cpq_finishq, cpcm);
		}
	}

	/*
	 * Process the completion queue.
	 */
	(void) cpqary3_process_finishq(cpq);

	mutex_exit(&cpq->cpq_mutex);
}

int
cpqary3_retrieve(cpqary3_t *cpq)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_retrieve_simple(cpq);
		return (DDI_SUCCESS);

	case CPQARY3_CTLR_MODE_UNKNOWN:
		break;
	}

	panic("unknown controller mode");
}

/*
 * Submit a command to the controller.
 */
int
cpqary3_submit(cpqary3_t *cpq, cpqary3_command_t *cpcm)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	/*
	 * Do not allow submission of more concurrent commands than the
	 * controller supports.
	 */
	if (avl_numnodes(&cpq->cpq_inflight) >= cpq->cpq_maxcmds) {
		return (EAGAIN);
	}

	/*
	 * Ensure that this command is not re-used without issuing a new
	 * tag number and performing any appropriate cleanup.
	 */
	VERIFY(!(cpcm->cpcm_status & ~CPQARY3_CMD_STATUS_USED));
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_USED;

	/*
	 * Insert this command into the inflight AVL.
	 */
	avl_index_t where;
	if (avl_find(&cpq->cpq_inflight, cpcm, &where) != NULL) {
		dev_err(cpq->dip, CE_PANIC, "duplicate submit tag %x",
		    cpcm->cpcm_tag);
	}
	avl_insert(&cpq->cpq_inflight, cpcm, where);

	VERIFY(!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_INFLIGHT));
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_INFLIGHT;

	cpcm->cpcm_time_submit = ddi_get_lbolt();

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_submit_simple(cpq, cpcm);
		break;

	default:
		panic("unknown controller mode");
	}

	return (0);
}

static void
cpqary3_process_finishq_one(cpqary3_command_t *cpcm)
{
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_COMPLETE;

	switch (cpcm->cpcm_type) {
	case CPQARY3_CMDTYPE_SYNCCMD:
		cv_broadcast(&cpcm->cpcm_ctlr->cpq_cv_finishq);
		return;

	case CPQARY3_CMDTYPE_OS:
		cpqary3_oscmd_complete(cpcm);
		return;

	default:
		break;
	}

	panic("unknown command type");
}

void
cpqary3_process_finishq(cpqary3_t *cpq)
{
	cpqary3_command_t *cpcm;

	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	while ((cpcm = list_remove_head(&cpq->cpq_finishq)) != NULL) {
		/*
		 * Check if this command has been abandoned by the original
		 * submitter.  If it has, free it now to avoid a leak.
		 */
		if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ABANDONED) {
			mutex_exit(&cpq->cpq_mutex);
			cpqary3_command_free(cpcm);
			mutex_enter(&cpq->cpq_mutex);
			continue;
		}

		if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLLED) {
			/*
			 * This command will be picked up and processed
			 * by "cpqary3_poll_for()" once the CV is triggered
			 * at the end of processing.
			 */
			cpcm->cpcm_status |= CPQARY3_CMD_STATUS_POLL_COMPLETE;
			continue;
		}

		cpqary3_process_finishq_one(cpcm);
	}

	cv_broadcast(&cpq->cpq_cv_finishq);
}

void
cpqary3_poll_for(cpqary3_t *cpq, cpqary3_command_t *cpcm)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));
	VERIFY(cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLLED);

	while (!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLL_COMPLETE)) {
		cv_wait(&cpq->cpq_cv_finishq, &cpq->cpq_mutex);
	}

#if 0
	/*
	 * Ensure this command is no longer in the inflight AVL.
	 */
	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_INFLIGHT) {
		cpcm->cpcp_status &= ~CPQARY3_CMD_STATUS_INFLIGHT;
		avl_remove(&cpq->cpq_inflight, cpcm);

		/*
		 * Mark it timed out.
		 */
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_TIMEOUT |
		    CPQARY3_CMD_STATUS_COMPLETE;
	}
#endif

	/*
	 * Fire the completion callback for this command.  The callback
	 * is responsible for freeing the command, so it may not be
	 * referenced again once this call returns.
	 */
	cpqary3_process_finishq_one(cpcm);
}

void
cpqary3_intr_set(cpqary3_t *cpq, boolean_t enabled)
{
	/*
	 * Read the Interrupt Mask Register.
	 */
	uint32_t imr = cpqary3_get32(cpq, CISS_I2O_INTERRUPT_MASK);

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		if (enabled) {
			imr &= ~INTR_SIMPLE_MASK;
		} else {
			imr |= INTR_SIMPLE_MASK;
		}
		break;

	default:
		if (enabled) {
			imr &= ~cpq->cpq_board->bd_intrmask;
		} else {
			imr |= cpq->cpq_board->bd_intrmask;
		}
		break;
	}

	cpqary3_put32(cpq, CISS_I2O_INTERRUPT_MASK, imr);
}

void
cpqary3_lockup_intr_set(cpqary3_t *cpq, boolean_t enabled)
{
	/*
	 * Read the Interrupt Mask Register.
	 */
	uint32_t imr = cpqary3_get32(cpq, CISS_I2O_INTERRUPT_MASK);

	/*
	 * Enable or disable firmware lockup interrupts from the controller
	 * based on the flag.
	 */
	if (enabled) {
		imr &= ~cpq->cpq_board->bd_lockup_intrmask;
	} else {
		imr |= cpq->cpq_board->bd_lockup_intrmask;
	}

	cpqary3_put32(cpq, CISS_I2O_INTERRUPT_MASK, imr);
}

/*
 * Signal to the controller that we have updated the Configuration Table by
 * writing to the Inbound Doorbell Register.  The controller will, after some
 * number of seconds, acknowledge this by clearing the bit.
 *
 * If successful, return DDI_SUCCESS.  If the controller takes too long to
 * acknowledge, return DDI_FAILURE.
 */
int
cpqary3_cfgtbl_flush(cpqary3_t *cpq)
{
	/*
	 * Read the current value of the Inbound Doorbell Register.
	 */
	uint32_t idr = cpqary3_get32(cpq, CISS_I2O_INBOUND_DOORBELL);

	/*
	 * Signal the Configuration Table change to the controller.
	 */
	idr |= CISS_IDR_BIT_CFGTBL_CHANGE;
	cpqary3_put32(cpq, CISS_I2O_INBOUND_DOORBELL, idr);

	/*
	 * Wait for the controller to acknowledge the change.
	 */
	for (unsigned i = 0; i < CISS_INIT_TIME; i++) {
		idr = cpqary3_get32(cpq, CISS_I2O_INBOUND_DOORBELL);

		if ((idr & CISS_IDR_BIT_CFGTBL_CHANGE) == 0) {
			return (DDI_SUCCESS);
		}

		/*
		 * Wait for one second before trying again.
		 */
		delay(drv_usectohz(1000000));
	}

	dev_err(cpq->dip, CE_WARN, "time out expired before controller "
	    "configuration completed");
	return (DDI_FAILURE);
}

int
cpqary3_cfgtbl_transport_has_support(cpqary3_t *cpq, int xport)
{
	VERIFY(xport == CISS_CFGTBL_XPORT_SIMPLE);

	/*
	 * Read the current value of the TransportSupport field in the
	 * Configuration Table.
	 */
	uint32_t xport_active = ddi_get32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->TransportSupport);

	/*
	 * Check that the desired transport method is supported by the
	 * controller:
	 */
	if ((xport_active & xport) == 0) {
		dev_err(cpq->dip, CE_WARN, "controller does not support "
		    "method \"%s\"", xport == CISS_CFGTBL_XPORT_SIMPLE ?
		    "simple" : "performant");
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

void
cpqary3_cfgtbl_transport_set(cpqary3_t *cpq, int xport)
{
	VERIFY(xport == CISS_CFGTBL_XPORT_SIMPLE);

	ddi_put32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->HostWrite.TransportRequest, xport);
}

int
cpqary3_cfgtbl_transport_confirm(cpqary3_t *cpq, int xport)
{
	VERIFY(xport == CISS_CFGTBL_XPORT_SIMPLE);

	/*
	 * Read the current value of the TransportActive field in the
	 * Configuration Table.
	 */
	uint32_t xport_active = ddi_get32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->TransportActive);

	/*
	 * Check that the desired transport method is now active:
	 */
	if ((xport_active & xport) == 0) {
		dev_err(cpq->dip, CE_WARN, "failed to enable transport "
		    "method \"%s\"", xport == CISS_CFGTBL_XPORT_SIMPLE ?
		    "simple" : "performant");
		return (DDI_FAILURE);
	}

	/*
	 * Ensure that the controller is now ready to accept commands.
	 */
	if ((xport_active & CISS_CFGTBL_READY_FOR_COMMANDS) == 0) {
		dev_err(cpq->dip, CE_WARN, "controller not ready to "
		    "accept commands");
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

uint32_t
cpqary3_ctlr_get_cmdsoutmax(cpqary3_t *cpq)
{
	uint32_t val;

	return (ddi_get32(cpq->cpq_ct_handle, &cpq->cpq_ct->CmdsOutMax));
}

static uint32_t
cpqary3_ctlr_get_hostdrvsup(cpqary3_t *cpq)
{
	if (!cpq->cpq_board->bd_is_e200 && !cpq->cpq_board->bd_is_ssll) {
		uint32_t val = ddi_get32(cpq->cpq_ct_handle,
		    &cpq->cpq_ct->HostDrvrSupport);

		/*
		 * XXX This is "bit 2" in the "Host Driver Specific Support"
		 * field of the Configuration Table.  According to the CISS
		 * spec, this is "Interrupt Host upon Controller Lockup"
		 * Enable.
		 *
		 * It's not clear why we _set_ this bit, but then it's not yet
		 * clear how this table entry is even supposed to work.
		 */
		val |= 0x04;

		ddi_put32(cpq->cpq_ct_handle, &cpq->cpq_ct->HostDrvrSupport,
		    val);
	}

	return (ddi_get32(cpq->cpq_ct_handle, &cpq->cpq_ct->HostDrvrSupport));
}

int
cpqary3_ctlr_init(cpqary3_t *cpq)
{
	uint8_t signature[4] = { 'C', 'I', 'S', 'S' };
	CfgTable_t *ctp = cpq->cpq_ct;
	int e;

	if ((e = cpqary3_ctlr_wait_for_state(cpq,
	    CPQARY3_WAIT_STATE_READY) != DDI_SUCCESS)) {
		return (e);
	}

	/*
	 * The configuration table contains an ASCII signature ("CISS") which
	 * should be checked as we initialise the controller.
	 * See: "9.1 Configuration Table" in CISS Specification.
	 */
	for (unsigned i = 0; i < 4; i++) {
		if (ddi_get8(cpq->cpq_ct_handle, &cpq->cpq_ct->Signature[i]) !=
		    signature[i]) {
			dev_err(cpq->dip, CE_WARN, "invalid signature "
			    "detected");
			return (DDI_FAILURE);
		}
	}

	if ((e = cpqary3_ctlr_init_simple(cpq)) != 0) {
		return (e);
	}

	/*
	 * Save some common feature support bitfields.
	 */
	cpq->cpq_host_support = cpqary3_ctlr_get_hostdrvsup(cpq);
	cpq->cpq_bus_support = ddi_get32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->BusTypes);

	/*
	 * Read initial controller heartbeat value and mark the current
	 * reading time.
	 */
	cpq->cpq_last_heartbeat = ddi_get32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->HeartBeat);
	cpq->cpq_last_heartbeat_lbolt = ddi_get_lbolt();

	return (0);
}

int
cpqary3_ctlr_wait_for_state(cpqary3_t *cpq, cpqary3_wait_state_t state)
{
	VERIFY(state == CPQARY3_WAIT_STATE_READY ||
	    state == CPQARY3_WAIT_STATE_UNREADY);

	/*
	 * Read from the Scratchpad Register until the expected ready signature
	 * is detected.  This behaviour is not described in the CISS
	 * specification.
	 *
	 * If the device is not in the desired state immediately, sleep for a
	 * second and try again.  If the device has not become ready in 300
	 * seconds, give up.
	 */
	for (unsigned i = 0; i < CPQARY3_WAIT_DELAY_SECONDS; i++) {
		uint32_t spr = cpqary3_get32(cpq, CISS_I2O_SCRATCHPAD);

		switch (state) {
		case CPQARY3_WAIT_STATE_READY:
			if (spr == CISS_SCRATCHPAD_INITIALISED) {
				return (DDI_SUCCESS);
			}
			break;

		case CPQARY3_WAIT_STATE_UNREADY:
			if (spr != CISS_SCRATCHPAD_INITIALISED) {
				return (DDI_SUCCESS);
			}
			break;
		}

		/*
		 * Wait for a second and try again.
		 */
		delay(drv_usectohz(1000000));
	}

	dev_err(cpq->dip, CE_WARN, "time out waiting for controller "
	    "to enter state \"%s\"", state == CPQARY3_WAIT_STATE_READY ?
	    "ready": "unready");
	return (DDI_FAILURE);
}
