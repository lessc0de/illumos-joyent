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

int
cpqary3_interrupts_setup(cpqary3_t *cpq)
{
	uint_t (*hw_isr)(caddr_t);
	uint_t (*sw_isr)(caddr_t);

	if (ddi_get_soft_iblock_cookie(cpq->dip, DDI_SOFTINT_HIGH,
	    &cpq->cpq_int_sw_cookie) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "ddi_get_soft_iblock_cookie failed");
		goto fail;
	}

	if (ddi_get_iblock_cookie(cpq->dip, 0, &cpq->cpq_int_hw_cookie) !=
	    DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "ddi_get_iblock_cookie (hw) failed");
		goto fail;
	}

	mutex_init(&cpq->sw_mutex, NULL, MUTEX_DRIVER,
	    (void *)cpq->cpq_int_sw_cookie);
	mutex_init(&cpq->hw_mutex, NULL, MUTEX_DRIVER,
	    (void *)cpq->cpq_int_hw_cookie);
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_MUTEX;

	/*
	 * Select the correct hardware interrupt service routine for the
	 * Transport Method we have configured:
	 */
	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		hw_isr = cpqary3_isr_hw_simple;
		sw_isr = cpqary3_isr_sw_simple;
		break;
	case CPQARY3_CTLR_MODE_PERFORMANT:
		hw_isr = cpqary3_isr_hw_performant;
		sw_isr = cpqary3_isr_sw_performant;
		break;
	default:
		panic("unknown controller mode");
	}

	if (ddi_add_softintr(cpq->dip,  DDI_SOFTINT_HIGH,
	    &cpq->cpqary3_softintr_id, &cpq->cpq_int_sw_cookie, NULL, sw_isr,
	    (caddr_t)cpq) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "ddi_add_softintr failed");
		goto fail;
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_INT_SW_HANDLER;

	if (ddi_add_intr(cpq->dip, 0, &cpq->cpq_int_hw_cookie, NULL, hw_isr,
	    (caddr_t)cpq) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "ddi_add_intr (hw) failed");
		goto fail;
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_INT_HW_HANDLER;

	return (DDI_SUCCESS);

fail:
	cpqary3_interrupts_teardown(cpq);
	return (DDI_FAILURE);
}

void
cpqary3_interrupts_teardown(cpqary3_t *cpq)
{
	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_INT_HW_HANDLER) {
		ddi_remove_intr(cpq->dip, 0, cpq->cpq_int_hw_cookie);

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_INT_HW_HANDLER;
	}

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_INT_SW_HANDLER) {
		ddi_remove_softintr(cpq->cpqary3_softintr_id);

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_INT_SW_HANDLER;
	}

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_MUTEX) {
		mutex_destroy(&cpq->sw_mutex);
		mutex_destroy(&cpq->hw_mutex);

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_MUTEX;
	}
}
