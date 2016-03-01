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
 * Copyright 2016 Joyent, Inc.
 */

/*
 * This module contains routines that program the controller. All
 * operations  viz.,  initialization of  controller,  submision &
 * retrieval  of  commands, enabling &  disabling of  interrupts,
 * checking interrupt status are performed here.
 */

#include <sys/sdt.h>
#include "cpqary3.h"

/*
 * XXX
 */
int
cpqary3_retrieve(cpqary3_t *cpq, uint32_t want_tag, boolean_t *found)
{
	VERIFY(MUTEX_HELD(&cpq->hw_mutex));
	VERIFY(MUTEX_HELD(&cpq->sw_mutex));

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_retrieve_simple(cpq, want_tag, found);
		return (DDI_SUCCESS);

	case CPQARY3_CTLR_MODE_UNKNOWN:
		break;
	}

	panic("unknown controller mode");
}

int
cpqary3_submit(cpqary3_t *cpq, cpqary3_command_t *cpcm)
{
	VERIFY(MUTEX_HELD(&cpq->hw_mutex));

	/*
	 * If a controller lockup has been detected, reject new command
	 * submissions.
	 */
	if (cpq->controller_lockup) {
		return (EIO);
	}

	/*
	 * Ensure that this command is not re-used without issuing a new
	 * tag number and performing any appropriate cleanup.
	 */
	VERIFY(!cpcm->cpcm_used);
	cpcm->cpcm_used = B_TRUE;

	/*
	 * Insert this command into the inflight AVL.
	 */
	avl_index_t where;
	if (avl_find(&cpq->cpq_inflight, cpcm, &where) != NULL) {
		dev_err(cpq->dip, CE_PANIC, "duplicate submit tag %x",
		    cpcm->cpcm_tag);
	}
	avl_insert(&cpq->cpq_inflight, cpcm, where);

	VERIFY(cpcm->cpcm_inflight == B_FALSE);
	cpcm->cpcm_inflight = B_TRUE;
	cpcm->cpcm_time_submit = ddi_get_lbolt();

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_put32(cpq, CISS_I2O_INBOUND_POST_Q, cpcm->cpcm_pa_cmd);
		break;

	default:
		panic("unknown controller mode");
	}

	return (0);
}


/*
 * Function	: 	cpqary3_intr_onoff
 * Description	: 	This routine enables/disables the HBA interrupt.
 * Called By	: 	cpqary3_attach(), ry3_handle_flag_nointr(),
 *			cpqary3_tick_hdlr(), cpqary3_init_ctlr_resource()
 * Parameters	: 	per-controller, flag stating enable/disable
 * Calls	: 	None
 * Return Values: 	None
 */
void
cpqary3_intr_onoff(cpqary3_t *cpq, uint8_t flag)
{
	/*
	 * Read the Interrupt Mask Register.
	 */
	uint32_t imr = cpqary3_get32(cpq, CISS_I2O_INTERRUPT_MASK);

	VERIFY(flag == CPQARY3_INTR_ENABLE || flag == CPQARY3_INTR_DISABLE);

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		if (flag == CPQARY3_INTR_ENABLE) {
			imr &= ~INTR_SIMPLE_MASK;
		} else {
			imr |= INTR_SIMPLE_MASK;
		}
		break;

	default:
		if (flag == CPQARY3_INTR_ENABLE) {
			imr &= ~cpq->cpq_board->bd_intrmask;
		} else {
			imr |= cpq->cpq_board->bd_intrmask;
		}
		break;
	}

	cpqary3_put32(cpq, CISS_I2O_INTERRUPT_MASK, imr);
}

/*
 * Function	: 	cpqary3_lockup_intr_onoff
 * Description	: 	This routine enables/disables the lockup interrupt.
 * Called By	: 	cpqary3_attach(), cpqary3_handle_flag_nointr(),
 *			cpqary3_tick_hdlr(), cpqary3_hw_isr,
 *			cpqary3_init_ctlr_resource()
 * Parameters	: 	per-controller, flag stating enable/disable
 * Calls	: 	None
 * Return Values: 	None
 */
void
cpqary3_lockup_intr_onoff(cpqary3_t *cpq, uint8_t flag)
{
	/*
	 * Read the Interrupt Mask Register.
	 */
	uint32_t imr = cpqary3_get32(cpq, CISS_I2O_INTERRUPT_MASK);

	/*
	 * Enable or disable firmware lockup interrupts from the controller
	 * based on the flag.
	 */
	if (flag == CPQARY3_LOCKUP_INTR_ENABLE) {
		imr &= ~cpq->cpq_board->bd_lockup_intrmask;
	} else {
		VERIFY(flag == CPQARY3_LOCKUP_INTR_DISABLE);

		imr |= cpq->cpq_board->bd_lockup_intrmask;
	}

	cpqary3_put32(cpq, CISS_I2O_INTERRUPT_MASK, imr);
}

void
cpqary3_trigger_sw_isr(cpqary3_t *cpq)
{
	boolean_t trigger = B_FALSE;

	VERIFY(MUTEX_HELD(&cpq->hw_mutex));

	if (!cpq->cpq_swintr_flag) {
		trigger = B_TRUE;
		cpq->cpq_swintr_flag = B_TRUE;
	}

	if (trigger) {
		ddi_trigger_softintr(cpq->cpqary3_softintr_id);
	}
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
