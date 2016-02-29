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

#include "cpqary3.h"


static void
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

uint_t
cpqary3_isr_hw_simple(caddr_t arg)
{
	cpqary3_t *cpq = (cpqary3_t *)arg;
	uint32_t isr = cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS);

	mutex_enter(&cpq->hw_mutex);
	if ((isr & cpq->cpq_board->bd_intrpendmask) == 0) {
		/*
		 * Check to see if the firmware has come to rest.  If it has,
		 * this routine will panic the system.
		 */
		cpqary3_lockup_check(cpq);

		mutex_exit(&cpq->hw_mutex);
		return (DDI_INTR_UNCLAIMED);
	}

	/*
	 * Disable interrupts until the soft interrupt handler has had a chance
	 * to read and process replies.
	 */
	cpqary3_intr_onoff(cpq, CPQARY3_INTR_DISABLE);

	cpqary3_trigger_sw_isr(cpq);

	mutex_exit(&cpq->hw_mutex);
	return (DDI_INTR_CLAIMED);
}

uint_t
cpqary3_isr_hw_performant(caddr_t arg)
{
	cpqary3_t *cpq = (cpqary3_t *)arg;
	uint32_t isr = cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS);

	if (isr == 0) {
		/*
		 * Check to see if the firmware has come to rest.  If it has,
		 * this routine will panic the system.
		 */
		cpqary3_lockup_check(cpq);

		return (DDI_INTR_UNCLAIMED);
	}

	uint32_t odr = cpqary3_get32(cpq, CISS_I2O_OUTBOUND_DOORBELL_STATUS);
	if ((odr & 0x1) != 0) {
		uint32_t odr_cl = cpqary3_get32(cpq,
		    CISS_I2O_OUTBOUND_DOORBELL_CLEAR);

		odr_cl |= 0x1;
		cpqary3_put32(cpq, CISS_I2O_OUTBOUND_DOORBELL_CLEAR, odr_cl);

		/*
		 * Read the status register again to ensure the write to clear
		 * is flushed to the controller.
		 */
		(void) cpqary3_get32(cpq, CISS_I2O_OUTBOUND_DOORBELL_STATUS);
	}

	cpqary3_trigger_sw_isr(cpq);

	return (DDI_INTR_CLAIMED);
}

uint_t
cpqary3_isr_sw_simple(caddr_t arg)
{
	cpqary3_t *cpq = (cpqary3_t *)arg;

	/*
	 * Confirm that the hardware interrupt routine scheduled this
	 * software interrupt.
	 */
	mutex_enter(&cpq->sw_mutex);
	mutex_enter(&cpq->hw_mutex);
	if (!cpq->cpq_swintr_flag) {
		mutex_exit(&cpq->hw_mutex);
		mutex_exit(&cpq->sw_mutex);
		return (DDI_INTR_UNCLAIMED);
	}

	cpqary3_retrieve_simple(cpq, 0, NULL);

	/*
	 * Unmask the controller inbound data interrupt.
	 */
	if (!cpq->cpq_intr_off) {
		cpqary3_intr_onoff(cpq, CPQARY3_INTR_ENABLE);
	}

	cpq->cpq_swintr_flag = B_FALSE;
	mutex_exit(&cpq->hw_mutex);
	mutex_exit(&cpq->sw_mutex);
	return (DDI_INTR_CLAIMED);
}

uint_t
cpqary3_isr_sw_performant(caddr_t arg)
{
	cpqary3_t *cpq = (cpqary3_t *)arg;

	/*
	 * Confirm that the hardware interrupt routine scheduled this
	 * software interrupt.
	 */
	mutex_enter(&cpq->sw_mutex);
	mutex_enter(&cpq->hw_mutex);
	if (!cpq->cpq_swintr_flag) {
		mutex_exit(&cpq->hw_mutex);
		mutex_exit(&cpq->sw_mutex);
		return (DDI_INTR_UNCLAIMED);
	}

	cpqary3_retrieve_performant(cpq, 0, NULL);

	cpq->cpq_swintr_flag = B_FALSE;
	mutex_exit(&cpq->hw_mutex);
	mutex_exit(&cpq->sw_mutex);
	return (DDI_INTR_CLAIMED);
}
