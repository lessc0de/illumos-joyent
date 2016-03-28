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

static char *
cpqary3_interrupt_type_name(int type)
{
	switch (type) {
	case DDI_INTR_TYPE_MSI:
		return ("MSI");
	case DDI_INTR_TYPE_FIXED:
		return ("fixed");
	default:
		return ("?");
	}
}

static int
cpqary3_interrupts_disable(cpqary3_t *cpq)
{
	if (cpq->cpq_interrupt_cap & DDI_INTR_FLAG_BLOCK) {
		return (ddi_intr_block_disable(cpq->cpq_interrupts,
		    cpq->cpq_ninterrupts));
	} else {
		VERIFY(cpq->cpq_ninterrupts == 0);

		return (ddi_intr_disable(cpq->cpq_interrupts[0]));
	}
}

static int
cpqary3_interrupts_enable(cpqary3_t *cpq)
{
	if (cpq->cpq_interrupt_cap & DDI_INTR_FLAG_BLOCK) {
		return (ddi_intr_block_enable(cpq->cpq_interrupts,
		    cpq->cpq_ninterrupts));
	} else {
		VERIFY(cpq->cpq_ninterrupts == 0);

		return (ddi_intr_enable(cpq->cpq_interrupts[0]));
	}
}

static void
cpqary3_interrupts_free(cpqary3_t *cpq)
{
	for (int i = 0; i < cpq->cpq_ninterrupts; i++) {
		(void) ddi_intr_free(cpq->cpq_interrupts[i]);
	}
	cpq->cpq_ninterrupts = 0;
	cpq->cpq_interrupt_type = 0;
	cpq->cpq_interrupt_cap = 0;
}

static int
cpqary3_interrupts_alloc(cpqary3_t *cpq, int type)
{
	int nintrs = 0;
	int navail = 0;

	if (ddi_intr_get_nintrs(cpq->dip, type, &nintrs) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not count %s interrupts",
		    cpqary3_interrupt_type_name(type));
		return (DDI_FAILURE);
	}
	if (nintrs < 1) {
		dev_err(cpq->dip, CE_WARN, "no %s interrupts supported",
		    cpqary3_interrupt_type_name(type));
		return (DDI_FAILURE);
	}

	if (ddi_intr_get_navail(cpq->dip, type, &navail) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not count available %s "
		    "interrupts", cpqary3_interrupt_type_name(type));
		return (DDI_FAILURE);
	}
	if (navail < 1) {
		dev_err(cpq->dip, CE_WARN, "no %s interrupts available",
		    cpqary3_interrupt_type_name(type));
		return (DDI_FAILURE);
	}

	if (ddi_intr_alloc(cpq->dip, cpq->cpq_interrupts,
	    type, 0, 1, &cpq->cpq_ninterrupts, DDI_INTR_ALLOC_STRICT) !=
	    DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "%s interrupt allocation failed",
		    cpqary3_interrupt_type_name(type));
		cpqary3_interrupts_free(cpq);
		return (DDI_FAILURE);
	}

	cpq->cpq_init_level |= CPQARY3_INITLEVEL_INT_ALLOC;
	cpq->cpq_interrupt_type = type;
	return (DDI_SUCCESS);
}

int
cpqary3_interrupts_setup(cpqary3_t *cpq)
{
	int types;
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

	if (ddi_intr_get_supported_types(cpq->dip, &types) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not get support interrupts");
		goto fail;
	}

	/*
	 * Try for an MSI first.
	 */
	if (types & DDI_INTR_TYPE_MSI) {
		if (cpqary3_interrupts_alloc(cpq, DDI_INTR_TYPE_MSI) ==
		    DDI_SUCCESS) {
			goto add_handler;
		}
	}

	/*
	 * Otherwise, fall back to fixed interrupts.
	 */
	if (types & DDI_INTR_TYPE_FIXED) {
		if (cpqary3_interrupts_alloc(cpq, DDI_INTR_TYPE_FIXED) ==
		    DDI_SUCCESS) {
			goto add_handler;
		}
	}

	/*
	 * We were unable to allocate any interrupts.
	 */
	dev_err(cpq->dip, CE_WARN, "interrupt allocation failed");
	goto fail;

add_handler:
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

	if (ddi_intr_get_cap(cpq->cpq_interrupts[0],
	    &cpq->cpq_interrupt_cap) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not get %s interrupt cap",
		    cpqary3_interrupt_type_name(cpq->cpq_interrupt_type));
		goto fail;
	}

	if (ddi_intr_add_handler(cpq->cpq_interrupts[0], hw_isr,
	    (caddr_t)cpq, NULL) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "adding %s interrupt failed",
		    cpqary3_interrupt_type_name(cpq->cpq_interrupt_type));
		goto fail;
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_INT_ADDED;

	/*
	 * Enable the interrupt handler.
	 */
	if (cpqary3_interrupts_enable(cpq) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "enable %s interrupt failed",
		    cpqary3_interrupt_type_name(cpq->cpq_interrupt_type));
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
		(void) cpqary3_interrupts_disable(cpq);

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_INT_ENABLED;
	}

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_INT_ADDED) {
		(void) ddi_intr_remove_handler(cpq->cpq_interrupts[0]);

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_INT_ADDED;
	}

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_INT_ALLOC) {
		cpqary3_interrupts_free(cpq);

		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_INT_ALLOC;
	}
}
