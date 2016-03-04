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
	int types;
	int nintrs = 0, navail = 0;
	unsigned ipri;
	uint_t (*hw_isr)(caddr_t, caddr_t);

	/*
	 * Select the correct hardware interrupt service routine for the
	 * Transport Method we have configured:
	 */
	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		hw_isr = cpqary3_isr_hw_simple;
		break;
	default:
		panic("unknown controller mode");
	}

	/*
	 * Ensure that at least one fixed interrupt is available to us.
	 */
	if (ddi_intr_get_supported_types(cpq->dip, &types) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not get support interrupts");
		goto fail;
	}
	if (!(types & DDI_INTR_TYPE_FIXED)) {
		dev_err(cpq->dip, CE_WARN, "DDI_INTR_TYPE_FIXED not supported");
		goto fail;
	}
	if (ddi_intr_get_nintrs(cpq->dip, DDI_INTR_TYPE_FIXED, &nintrs) !=
	    DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not count fixed interrupts");
		goto fail;
	}
	if (ddi_intr_get_navail(cpq->dip, DDI_INTR_TYPE_FIXED, &navail) !=
	    DDI_SUCCESS || navail < 1) {
		dev_err(cpq->dip, CE_WARN, "no fixed interrupts available");
		goto fail;
	}

	/*
	 * Set the flag first, as we are still expected to call ddi_intr_free()
	 * for a partial, but failed, allocation of interrupts.
	 */
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_INT_ALLOC;
	if (ddi_intr_alloc(cpq->dip, cpq->cpq_interrupts,
	    DDI_INTR_TYPE_FIXED, 0, 1, &cpq->cpq_ninterrupts,
	    DDI_INTR_ALLOC_NORMAL) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "interrupt allocation failed");
		goto fail;
	}

	/*
	 * Ensure that we have not been given a high-level interrupt, as our
	 * interrupt handlers do not support them.
	 */
	if (ddi_intr_get_pri(cpq->cpq_interrupts[0], &ipri) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not determine interrupt "
		    "priority");
		goto fail;
	}
	if (ipri >= ddi_intr_get_hilevel_pri()) {
		dev_err(cpq->dip, CE_WARN, "high level interrupts not "
		    "supported");
		goto fail;
	}

	if (ddi_intr_add_handler(cpq->cpq_interrupts[0], hw_isr,
	    (caddr_t)cpq, NULL) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "adding interrupt failed");
		goto fail;
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_INT_ADDED;

	/*
	 * Enable the interrupt handler.
	 */
	if (ddi_intr_enable(cpq->cpq_interrupts[0]) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "enable interrupt failed");
		goto fail;
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_INT_ENABLED;

	return (DDI_SUCCESS);

fail:
	cpqary3_interrupts_teardown(cpq);
	return (DDI_FAILURE);
}

void
cpqary3_interrupts_teardown(cpqary3_t *cpq)
{
	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_INT_ENABLED) {
		(void) ddi_intr_disable(cpq->cpq_interrupts[0]);

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_INT_ENABLED;
	}

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_INT_ADDED) {
		(void) ddi_intr_remove_handler(cpq->cpq_interrupts[0]);

		cpq->cpq_init_level &= CPQARY3_INITLEVEL_INT_ADDED;
	}

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_INT_ALLOC) {
		for (int i = 0; i < cpq->cpq_ninterrupts; i++) {
			(void) ddi_intr_free(cpq->cpq_interrupts[i]);
		}

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_INT_ALLOC;
	}
}
