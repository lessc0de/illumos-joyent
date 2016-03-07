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


void
cpqary3_write_lun_addr_phys(LUNAddr_t *lun, boolean_t masked, unsigned bus,
    unsigned target)
{
	lun->PhysDev.Mode = masked ? MASK_PERIPHERIAL_DEV_ADDR :
	    PERIPHERIAL_DEV_ADDR;

	lun->PhysDev.TargetId = target;
	lun->PhysDev.Bus = bus;

	bzero(&lun->PhysDev.Target, sizeof (lun->PhysDev.Target));
}

void
cpqary3_write_message_common(cpqary3_command_t *cpcm, int type,
    int timeout_secs)
{
	switch (type) {
	case CISS_MSG_ABORT:
	case CISS_MSG_RESET:
	case CISS_MSG_NOP:
		break;

	default:
		panic("unknown message type");
	}

	cpcm->cpcm_va_cmd->Request.Type.Type = CISS_TYPE_MSG;
	cpcm->cpcm_va_cmd->Request.Type.Attribute = CISS_ATTR_HEADOFQUEUE;
	cpcm->cpcm_va_cmd->Request.Type.Direction = CISS_XFER_NONE;
	cpcm->cpcm_va_cmd->Request.Timeout = timeout_secs;
	cpcm->cpcm_va_cmd->Request.CDBLen = 16;
	cpcm->cpcm_va_cmd->Request.CDB[0] = type;
}

void
cpqary3_write_message_abort_one(cpqary3_command_t *cpcm, uint32_t tag)
{
	cpqary3_tag_t cisstag;

	/*
	 * When aborting a particular command, the request is addressed
	 * to the controller.
	 */
	cpqary3_write_lun_addr_phys(&cpcm->cpcm_va_cmd->Header.LUN,
	    B_TRUE, 0, 0);

	cpqary3_write_message_common(cpcm, CISS_MSG_ABORT, 0);

	/*
	 * Abort a single command.
	 */
	cpcm->cpcm_va_cmd->Request.CDB[1] = CISS_ABORT_TASK;

	/*
	 * The CISS Specification says that the tag value for a task-level
	 * abort should be in the CDB in bytes 4-11.
	 */
	bzero(&cisstag, sizeof (cisstag));
	cisstag.tag_value = tag;
	bcopy(&cisstag, &cpcm->cpcm_va_cmd->Request.CDB[4],
	    sizeof (cisstag));
}

void
cpqary3_write_message_abort_all(cpqary3_command_t *cpcm, LogDevAddr_t *addr)
{
	/*
	 * When aborting all tasks for a particular Logical Volume,
	 * the command is addressed not to the controller but to
	 * the Volume itself.
	 */
	cpcm->cpcm_va_cmd->Header.LUN.LogDev = *addr;

	cpqary3_write_message_common(cpcm, CISS_MSG_ABORT, 0);

	/*
	 * Abort all commands for a particular Logical Volume.
	 */
	cpcm->cpcm_va_cmd->Request.CDB[1] = CISS_ABORT_TASKSET;
}

void
cpqary3_write_message_reset_ctlr(cpqary3_command_t *cpcm)
{
	cpqary3_write_lun_addr_phys(&cpcm->cpcm_va_cmd->Header.LUN,
	    B_TRUE, 0, 0);

	cpqary3_write_message_common(cpcm, CISS_MSG_RESET, 0);

	cpcm->cpcm_va_cmd->Request.CDB[1] = CISS_RESET_CTLR;
}

void
cpqary3_write_message_nop(cpqary3_command_t *cpcm, int timeout_secs)
{
	/*
	 * No-op messages are always sent to the controller.
	 */
	cpqary3_write_lun_addr_phys(&cpcm->cpcm_va_cmd->Header.LUN,
	    B_TRUE, 0, 0);

	cpqary3_write_message_common(cpcm, CISS_MSG_NOP, timeout_secs);
}

/*
 * This routine is executed every 15 seconds via ddi_periodic_add(9F).  It
 * checks the health of the controller and looks for submitted commands that
 * have timed out.
 */
void
cpqary3_periodic(void *arg)
{
	cpqary3_t *cpq = arg;
	cpqary3_command_t *cpcm, *cpcm_next;

	mutex_enter(&cpq->cpq_mutex);
	if (!(cpq->cpq_status & CPQARY3_CTLR_STATUS_RUNNING)) {
		/*
		 * The device is currently not active, e.g. due to an
		 * in-progress controller reset.
		 */
		mutex_exit(&cpq->cpq_mutex);
		return;
	}

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
	for (cpcm = avl_first(&cpq->cpq_inflight); cpcm != NULL;
	    cpcm = cpcm_next) {
		/*
		 * Save the next entry now, in case we need to remove this one
		 * from the AVL tree.
		 */
		cpcm_next = AVL_NEXT(&cpq->cpq_inflight, cpcm);

		if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLLED) {
			/*
			 * Polled commands are timed out by the polling
			 * routine.
			 */
			continue;
		}

		if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ABORT_SENT) {
			/*
			 * This command has been aborted; either it will
			 * complete or the controller will be reset.
			 */
			continue;
		}

		if (cpcm->cpcm_expiry == 0) {
			/*
			 * This command has no expiry time.
			 */
			continue;
		}

		if (gethrtime() > cpcm->cpcm_expiry) {
			if (list_link_active(&cpcm->cpcm_link_abort)) {
				/*
				 * Already on the abort queue.
				 */
				continue;
			}

			list_insert_tail(&cpq->cpq_abortq, cpcm);
			cpcm->cpcm_status |= CPQARY3_CMD_STATUS_TIMEOUT;
		}
	}

	/*
	 * Process the completion queue.
	 */
	(void) cpqary3_process_finishq(cpq);

	/*
	 * Process the abort queue.
	 */
	(void) cpqary3_process_abortq(cpq);

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
	 * If the controller is currently being reset, do not allow command
	 * submission.
	 */
	if (!(cpq->cpq_status & CPQARY3_CTLR_STATUS_RUNNING)) {
		return (EIO);
	}

	/*
	 * Do not allow submission of more concurrent commands than the
	 * controller supports.
	 */
	if (avl_numnodes(&cpq->cpq_inflight) >= cpq->cpq_maxcmds) {
		return (EAGAIN);
	}

	/*
	 * Synchronise the Command Block DMA resources to ensure that the
	 * device has a consistent view before we pass it the address.
	 */
	if (ddi_dma_sync(cpcm->cpcm_phyctg.cpqary3_dmahandle, 0,
	    sizeof (CommandList_t), DDI_DMA_SYNC_FORDEV) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "DMA sync failure");
		return (EIO);
	}

	/*
	 * Ensure that this command is not re-used without issuing a new
	 * tag number and performing any appropriate cleanup.
	 */
	VERIFY(!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_USED));
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

	cpcm->cpcm_time_submit = gethrtime();

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_submit_simple(cpq, cpcm);
		return (0);

	case CPQARY3_CTLR_MODE_UNKNOWN:
		break;
	}
	panic("unknown controller mode");
}

static void
cpqary3_process_finishq_one(cpqary3_command_t *cpcm)
{
	cpqary3_t *cpq = cpcm->cpcm_ctlr;

	VERIFY(!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_COMPLETE));
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_COMPLETE;

	switch (cpcm->cpcm_type) {
	case CPQARY3_CMDTYPE_INTERNAL:
		cv_broadcast(&cpcm->cpcm_ctlr->cpq_cv_finishq);
		return;

	case CPQARY3_CMDTYPE_SCSA:
		cpqary3_oscmd_complete(cpcm);
		return;

	case CPQARY3_CMDTYPE_ABORTQ:
		/*
		 * Abort messages sent as part of abort queue processing
		 * do not require any completion activity.
		 */
		mutex_exit(&cpq->cpq_mutex);
		cpqary3_command_free(cpcm);
		mutex_enter(&cpq->cpq_mutex);
		return;
	}

	panic("unknown command type");
}

/*
 * Process commands in the completion queue.
 */
void
cpqary3_process_finishq(cpqary3_t *cpq)
{
	cpqary3_command_t *cpcm;

	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	while ((cpcm = list_remove_head(&cpq->cpq_finishq)) != NULL) {
		/*
		 * Check if this command was in line to be aborted.
		 */
		if (list_link_active(&cpcm->cpcm_link_abort)) {
			/*
			 * This command was in line, but the controller
			 * subsequently completed the command before we
			 * were able to do so.
			 */
			list_remove(&cpq->cpq_abortq, cpcm);
			cpcm->cpcm_status &= ~CPQARY3_CMD_STATUS_TIMEOUT;
		}

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

		/*
		 * Synchronise the Command Block before we read from it,
		 * to ensure a consistent view of whatever the controller
		 * left for us.
		 */
		if (ddi_dma_sync(cpcm->cpcm_phyctg.cpqary3_dmahandle, 0,
		    cpcm->cpcm_phyctg.real_size,
		    DDI_DMA_SYNC_FORCPU) != DDI_SUCCESS) {
			dev_err(cpq->dip, CE_WARN,
			    "finishq DMA sync failure");
			/*
			 * XXX what to do about this?!
			 * Apparently this can only fail if we get the
			 * address range wrong, so it seems best to panic.
			 */
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

/*
 * Process commands in the abort queue.
 */
void
cpqary3_process_abortq(cpqary3_t *cpq)
{
	cpqary3_command_t *cpcm;
	cpqary3_command_t *abort_cpcm = NULL;

	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	if (list_is_empty(&cpq->cpq_abortq)) {
		goto out;
	}

another:
	mutex_exit(&cpq->cpq_mutex);
	if ((abort_cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_ABORTQ,
	    KM_NOSLEEP)) == NULL) {
		/*
		 * No resources available to send abort messages.  We will
		 * try again the next time around.
		 */
		mutex_enter(&cpq->cpq_mutex);
		goto out;
	}
	mutex_enter(&cpq->cpq_mutex);

	while ((cpcm = list_remove_head(&cpq->cpq_abortq)) != NULL) {
		if (!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_INFLIGHT)) {
			/*
			 * This message is not currently inflight, so
			 * no abort is needed.
			 */
			continue;
		}

		if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ABORT_SENT) {
			/*
			 * An abort message has already been sent for
			 * this command.
			 */
			continue;
		}

		/*
		 * Send an abort message for the command.
		 */
		cpqary3_write_message_abort_one(abort_cpcm, cpcm->cpcm_tag);
		if (cpqary3_submit(cpq, abort_cpcm) != 0) {
			/*
			 * The command could not be submitted to the
			 * controller.  Put it back in the abort queue
			 * and give up for now.
			 */
			list_insert_head(&cpq->cpq_abortq, cpcm);
			goto out;
		}
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ABORT_SENT;

		/*
		 * Record some debugging information about the abort we
		 * sent:
		 */
		cpcm->cpcm_abort_time = gethrtime();
		cpcm->cpcm_abort_tag = abort_cpcm->cpcm_tag;

		/*
		 * The abort message was sent.  Release it and
		 * allocate another command.
		 */
		abort_cpcm = NULL;
		goto another;
	}

out:
	cv_broadcast(&cpq->cpq_cv_finishq);
	if (abort_cpcm != NULL) {
		mutex_exit(&cpq->cpq_mutex);
		cpqary3_command_free(abort_cpcm);
		mutex_enter(&cpq->cpq_mutex);
	}
}

int
cpqary3_poll_for(cpqary3_t *cpq, cpqary3_command_t *cpcm)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));
	VERIFY(cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLLED);

	while (!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLL_COMPLETE)) {
		if (cpcm->cpcm_expiry != 0) {
			/*
			 * This command has an expiry time.  Check to see
			 * if it has already passed:
			 */
			if (cpcm->cpcm_expiry < gethrtime()) {
				return (ETIMEDOUT);
			}
		}

		if (ddi_in_panic()) {
			/*
			 * When the system is panicking, there are no
			 * interrupts or other threads.  Drive the polling
			 * loop on our own.
			 */
			(void) cpqary3_retrieve(cpq);
			cpqary3_process_finishq(cpq);
			drv_usecwait(100);
			continue;
		}

		/*
		 * Wait for command completion to return through the regular
		 * interrupt handling path.
		 */
		if (cpcm->cpcm_expiry == 0) {
			cv_wait(&cpq->cpq_cv_finishq, &cpq->cpq_mutex);
		} else {
			/*
			 * Wait only until the expiry time for this command.
			 */
			(void) cv_timedwait_sig_hrtime(&cpq->cpq_cv_finishq,
			    &cpq->cpq_mutex, cpcm->cpcm_expiry);
		}
	}

	/*
	 * Fire the completion callback for this command.  The callback
	 * is responsible for freeing the command, so it may not be
	 * referenced again once this call returns.
	 */
	cpqary3_process_finishq_one(cpcm);

	return (0);
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
cpqary3_ctlr_get_maxsgelements(cpqary3_t *cpq)
{
	return (ddi_get32(cpq->cpq_ct_handle, &cpq->cpq_ct->MaxSGElements));
}

uint32_t
cpqary3_ctlr_get_cmdsoutmax(cpqary3_t *cpq)
{
	return (ddi_get32(cpq->cpq_ct_handle, &cpq->cpq_ct->CmdsOutMax));
}

static uint32_t
cpqary3_ctlr_get_hostdrvsup(cpqary3_t *cpq)
{
	return (ddi_get32(cpq->cpq_ct_handle, &cpq->cpq_ct->HostDrvrSupport));
}

int
cpqary3_ctlr_init(cpqary3_t *cpq)
{
	uint8_t signature[4] = { 'C', 'I', 'S', 'S' };
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

	/*
	 * Initialise an appropriate Transport Method.  For now, this driver
	 * only supports the "Simple" method.
	 */
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
	cpq->cpq_last_heartbeat_time = gethrtime();

	return (DDI_SUCCESS);
}

void
cpqary3_ctlr_teardown(cpqary3_t *cpq)
{
	cpq->cpq_status &= ~CPQARY3_CTLR_STATUS_RUNNING;

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_ctlr_teardown_simple(cpq);
		return;

	case CPQARY3_CTLR_MODE_UNKNOWN:
		return;
	}

	panic("unknown controller mode");
}

int
cpqary3_ctlr_wait_for_state(cpqary3_t *cpq, cpqary3_wait_state_t state)
{
	unsigned wait_usec = 100 * 1000;
	unsigned wait_count = CPQARY3_WAIT_DELAY_SECONDS * 1000000 / wait_usec;

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
	for (unsigned i = 0; i < wait_count; i++) {
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

		if (ddi_in_panic()) {
			/*
			 * There is no sleep for the panicking, so we
			 * must spin wait:
			 */
			drv_usecwait(wait_usec);
		} else {
			/*
			 * Wait for a quarter second and try again.
			 */
			delay(drv_usectohz(wait_usec));
		}
	}

	dev_err(cpq->dip, CE_WARN, "time out waiting for controller "
	    "to enter state \"%s\"", state == CPQARY3_WAIT_STATE_READY ?
	    "ready": "unready");
	return (DDI_FAILURE);
}

void
cpqary3_lockup_check(cpqary3_t *cpq)
{
	/*
	 * Read the current controller heartbeat value.
	 */
	uint32_t heartbeat = ddi_get32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->HeartBeat);

	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	/*
	 * Check to see if the value is the same as last time we looked:
	 */
	if (heartbeat != cpq->cpq_last_heartbeat) {
		/*
		 * The heartbeat value has changed, which suggests that the
		 * firmware in the controller has not yet come to a complete
		 * stop.  Record the new value, as well as the current time.
		 */
		cpq->cpq_last_heartbeat = heartbeat;
		cpq->cpq_last_heartbeat_time = gethrtime();
		return;
	}

	/*
	 * The controller _might_ have been able to signal to us that is
	 * has locked up.  This is a truly unfathomable state of affairs:
	 * If the firmware can tell it has flown off the rails, why not
	 * simply reset the controller?
	 */
	uint32_t odr = cpqary3_get32(cpq, CISS_I2O_OUTBOUND_DOORBELL_STATUS);
	uint32_t spr = cpqary3_get32(cpq, CISS_I2O_SCRATCHPAD);
	if ((odr & CISS_ODR_BIT_LOCKUP) != 0) {
		dev_err(cpq->dip, CE_PANIC, "HP SmartArray firmware has "
		    "reported a critical fault (odr %08x spr %08x)",
		    odr, spr);
	}

	if (gethrtime() > cpq->cpq_last_heartbeat_time + 60 * NANOSEC) {
		dev_err(cpq->dip, CE_PANIC, "HP SmartArray firmware has "
		    "stopped responding (odr %08x spr %08x)",
		    odr, spr);
	}
}

int
cpqary3_ctlr_reset(cpqary3_t *cpq)
{
	cpqary3_command_t *cpcm, *cpcm_nop;
	int r;

	/*
	 * Allocate two commands: one for the soft reset message, which we
	 * cannot free until the controller has reset; and one for the ping we
	 * will use to determine when it is once again functional.
	 */
	if ((cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_INTERNAL,
	    KM_NOSLEEP)) == NULL) {
		return (ENOMEM);
	}
	if ((cpcm_nop = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_INTERNAL,
	    KM_NOSLEEP)) == NULL) {
		cpqary3_command_free(cpcm);
		return (ENOMEM);
	}

	if (ddi_in_panic()) {
		goto skip_check;
	}

	mutex_enter(&cpq->cpq_mutex);
	if (cpq->cpq_status & CPQARY3_CTLR_STATUS_RESETTING) {
		/*
		 * Don't pile on.  One reset is enough.  Wait until
		 * it's complete, and then return success.
		 */
		while (!(cpq->cpq_status & CPQARY3_CTLR_STATUS_RUNNING)) {
			cv_wait(&cpq->cpq_cv_finishq, &cpq->cpq_mutex);
		}
		mutex_exit(&cpq->cpq_mutex);
		cpqary3_command_free(cpcm);
		cpqary3_command_free(cpcm_nop);
		return (0);
	}
	cpq->cpq_status |= CPQARY3_CTLR_STATUS_RESETTING;
	cpq->cpq_last_reset_start = gethrtime();
	mutex_exit(&cpq->cpq_mutex);

skip_check:

	/*
	 * Send a soft reset command to the controller.  If this command
	 * succeeds, there will likely be no completion notification.  Instead,
	 * the device should become unavailable for some period of time and
	 * then become available again.  Once available again, we know the soft
	 * reset has completed and should abort all in-flight commands. XXX
	 */
	cpqary3_write_message_reset_ctlr(cpcm);

	mutex_enter(&cpq->cpq_mutex);

	/*
	 * Disable interrupts now.
	 */
	cpqary3_intr_set(cpq, B_FALSE);

	dev_err(cpq->dip, CE_WARN, "SENDING SOFT RESET MESSAGE");
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_POLLED;
	if ((r = cpqary3_submit(cpq, cpcm)) != 0) {
		dev_err(cpq->dip, CE_PANIC, "could not complete soft reset "
		    ": submit failed (%d)", r);
	}

	/*
	 * Mark every currently inflight command as being reset, including the
	 * soft reset command we just sent.  Once we confirm the reset works,
	 * we can safely report that these commands have failed.
	 */
	for (cpqary3_command_t *t = avl_first(&cpq->cpq_inflight);
	    t != NULL; t = AVL_NEXT(&cpq->cpq_inflight, t)) {
		t->cpcm_status |= CPQARY3_CMD_STATUS_RESET_SENT;
	}

	/*
	 * Now that we have submitted our soft reset command, prevent
	 * the rest of the driver from interacting with the controller.
	 */
	cpq->cpq_status &= ~CPQARY3_CTLR_STATUS_RUNNING;

	/*
	 * We do not expect a completion from the controller for our soft
	 * reset command, but we also cannot remove it from the inflight
	 * list until we know the controller has actually reset.  To do
	 * otherwise would potentially allow the controller to scribble
	 * on the memory we were using.
	 */
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ABANDONED;

	dev_err(cpq->dip, CE_WARN, "WAITING FOR DEVICE TO GO AWAY");
	if (cpqary3_ctlr_wait_for_state(cpq, CPQARY3_WAIT_STATE_UNREADY) !=
	    DDI_SUCCESS) {
		dev_err(cpq->dip, CE_PANIC, "could not complete soft reset "
		    ": controller did not go offline");
	}

	dev_err(cpq->dip, CE_WARN, "WAITING FOR DEVICE TO COME BACK");
	if (cpqary3_ctlr_wait_for_state(cpq, CPQARY3_WAIT_STATE_READY) !=
	    DDI_SUCCESS) {
		dev_err(cpq->dip, CE_PANIC, "could not complete soft reset "
		    ": controller did not come back online");
	}

	/*
	 * In at least the Smart Array P420i, the controller can take 30-45
	 * seconds after the scratchpad register shows it as being available
	 * before it is ready to receive commands.  In order to avoid hitting
	 * it too early with our post-reset ping, we will sleep for 10 seconds
	 * here.
	 */
	dev_err(cpq->dip, CE_WARN, "SLEEPING FOR CONTROLLER TO COME BACK");
	if (ddi_in_panic()) {
		drv_usecwait(10 * MICROSEC);
	} else {
		delay(drv_usectohz(10 * MICROSEC));
	}

	dev_err(cpq->dip, CE_WARN, "REINIT DEVICE");
	cpqary3_ctlr_teardown(cpq);
	if (cpqary3_ctlr_init(cpq) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_PANIC, "could not complete soft reset "
		    ": controller transport could not be configured");
	}

	dev_err(cpq->dip, CE_WARN, "SEND NOP MESSAGE");
	cpqary3_write_message_nop(cpcm_nop, 0);
	cpcm_nop->cpcm_status |= CPQARY3_CMD_STATUS_POLLED;
	cpq->cpq_status |= CPQARY3_CTLR_STATUS_RUNNING;
	if ((r = cpqary3_submit(cpq, cpcm_nop)) != 0) {
		dev_err(cpq->dip, CE_PANIC, "could not complete soft reset "
		    ": post-reset ping could not be submitted (%d)", r);
	}
	cpq->cpq_status &= ~CPQARY3_CTLR_STATUS_RUNNING;

	dev_err(cpq->dip, CE_WARN, "WAITING TO SEE IF WE GET A COMPLETION");
	for (int i = 0; i < 500; i++) {
		/*
		 * Interrupts are still masked at this stage.  The controller
		 * should start up in the Simple Transport mode, so poll
		 * manually for a bit:
		 */
		dev_err(cpq->dip, CE_WARN, "TRY #%d...", i);
		cpqary3_retrieve_simple(cpq);
		cpqary3_process_finishq(cpq);
		if (cpcm_nop->cpcm_status & CPQARY3_CMD_STATUS_POLL_COMPLETE) {
			cpqary3_process_finishq_one(cpcm_nop);
			dev_err(cpq->dip, CE_WARN, "GOT A COMPLETION!");

			VERIFY(cpcm_nop->cpcm_status &
			    CPQARY3_CMD_STATUS_COMPLETE);
			if (cpcm_nop->cpcm_status & CPQARY3_CMD_STATUS_ERROR) {
				dev_err(cpq->dip, CE_WARN, "BUT, AN ERROR!");
			}
			break;
		}
		drv_usecwait(100 * 1000);
	}

	if (!(cpcm_nop->cpcm_status & CPQARY3_CMD_STATUS_COMPLETE)) {
		dev_err(cpq->dip, CE_PANIC, "could not complete soft reset "
		    ": post-reset ping was not returned");
	}

	/*
	 * Now that the controller is working again, we can abort any
	 * commands that were inflight during the reset.
	 */
	cpqary3_command_t *nt;
	for (cpqary3_command_t *t = avl_first(&cpq->cpq_inflight);
	    t != NULL; t = nt) {
		nt = AVL_NEXT(&cpq->cpq_inflight, t);

		if (t->cpcm_status & CPQARY3_CMD_STATUS_RESET_SENT) {
			avl_remove(&cpq->cpq_inflight, t);
			t->cpcm_status &= ~CPQARY3_CMD_STATUS_INFLIGHT;

			list_insert_tail(&cpq->cpq_finishq, t);
		}
	}

	/*
	 * Re-enable interrupts, mark the controller running and
	 * the reset as complete....
	 */
	cpqary3_intr_set(cpq, B_TRUE);
	cpq->cpq_status |= CPQARY3_CTLR_STATUS_RUNNING;
	cpq->cpq_status &= ~CPQARY3_CTLR_STATUS_RESETTING;
	cpq->cpq_last_reset_finish = gethrtime();

	/*
	 * Process the completion queue one last time before we let go
	 * of the mutex.
	 */
	cpqary3_process_finishq(cpq);

	/*
	 * Wake anybody that was waiting for the reset to complete.
	 */
	cv_broadcast(&cpq->cpq_cv_finishq);
	mutex_exit(&cpq->cpq_mutex);

	cpqary3_command_free(cpcm_nop);
	return (0);
}
