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
 * Local Functions Definitions
 */
uint8_t cpqary3_check_simple_ctlr_intr(cpqary3_t *cpqary3p);
uint8_t cpqary3_check_perf_ctlr_intr(cpqary3_t *cpqary3p);
uint8_t cpqary3_check_perf_e200_intr(cpqary3_t *cpqary3p);

/*
 * Function	: 	cpqary3_check_simple_ctlr_intr
 * Description	: 	This routine determines if the controller did interrupt.
 * Called By	: 	cpqary3_hw_isr()
 * Parameters	: 	per-controller
 * Calls	: 	None
 * Return Values: 	SUCCESS : This controller did interrupt.
 *			FAILURE : It did not.
 */
uint8_t
cpqary3_check_simple_ctlr_intr(cpqary3_t *cpq)
{
	uint32_t intr_pending_mask = cpq->cpq_board->bd_intrpendmask;

	/*
	 * Read the Interrupt Status Register and
	 * if bit 3 is set, it indicates that we have completed commands
	 * in the controller
	 */
	if (intr_pending_mask &
	    cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS)) {
		return (CPQARY3_SUCCESS);
	}

	return (CPQARY3_FAILURE);
}

/*
 * Function	:      	cpqary3_check_perf_ctlr_intr
 * Description	:      	This routine determines if the
 *			controller did interrupt.
 * Called By	:      	cpqary3_hw_isr()
 * Parameters	:      	per-controller
 * Calls	:      	None
 * Return Values:      	SUCCESS : This controller did interrupt.
 *			FAILURE : It did not.
 */
uint8_t
cpqary3_check_perf_ctlr_intr(cpqary3_t *cpq)
{
	/*
	 * Read the Interrupt Status Register and
	 * if bit 3 is set, it indicates that we have completed commands
	 * in the controller
	 * XXX _which_ bit?
	 */
	if (cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS) & 0x1) {
		return (CPQARY3_SUCCESS);
	}

	return (CPQARY3_FAILURE);
}

/*
 * Function	:      	cpqary3_check_perf_e200_intr
 * Description	:      	This routine determines if the controller
 *			did interrupt.
 * Called By	:      	cpqary3_hw_isr()
 * Parameters	:      	per-controller
 * Calls	:      	None
 * Return Values:      	SUCCESS : This controller did interrupt.
 *			FAILURE : It did not.
 */
uint8_t
cpqary3_check_perf_e200_intr(cpqary3_t *cpq)
{
	/*
	 * Read the Interrupt Status Register and
	 * if bit 3 is set, it indicates that we have completed commands
	 * in the controller
	 */
	if (cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS) & 0x4) {
		return (CPQARY3_SUCCESS);
	}

	return (CPQARY3_FAILURE);
}

/*
 * Read tags and process completion of the associated command until the supply
 * of tags is exhausted.
 */
void
cpqary3_retrieve_simple(cpqary3_t *cpq, uint32_t watchfor, boolean_t *found)
{
	uint32_t opq;
	uint32_t none = 0xffffffff;

	VERIFY(MUTEX_HELD(&cpq->hw_mutex));
	VERIFY(MUTEX_HELD(&cpq->sw_mutex));

	while ((opq = cpqary3_get32(cpq, CISS_I2O_OUTBOUND_POST_Q)) != none) {
		cpqary3_command_t *cpcm;
		uint32_t tag = opq >> 2; /* XXX */

		if ((cpcm = cpqary3_lookup_inflight(cpq, tag)) == NULL) {
			dev_err(cpq->dip, CE_WARN, "spurious tag %x", tag);
			continue;
		}

		avl_remove(&cpq->cpq_inflight, cpcm);
		cpcm->cpcm_inflight = B_FALSE;
		cpcm->cpcm_error = (opq & (0x1 << 1)) != 0;

		if (found != NULL && cpcm->cpcm_tag == watchfor) {
			*found = B_TRUE;
		}

		mutex_exit(&cpq->hw_mutex);
		cpcm->cpcm_complete(cpcm);
		mutex_enter(&cpq->hw_mutex);
	}
}

void
cpqary3_retrieve_performant(cpqary3_t *cpq, uint32_t watchfor, boolean_t *found)
{
	cpqary3_replyq_t *cprq = &cpq->cpq_replyq;

	for (;;) {
		uint32_t ent = cprq->cprq_tags[2 * cprq->cprq_read_index];
		uint32_t tag = ent >> 2; /* XXX */
		cpqary3_command_t *cpcm;

		if ((ent & 0x1) != cprq->cprq_cycle_indicator) {
			break;
		}

		if ((cpcm = cpqary3_lookup_inflight(cpq, tag)) == NULL) {
			dev_err(cpq->dip, CE_WARN, "spurious tag %x", tag);
			continue;
		}

		avl_remove(&cpq->cpq_inflight, cpcm);
		cpcm->cpcm_inflight = B_FALSE;
		cpcm->cpcm_error = (ent & (0x1 << 1)) != 0;

		if (found != NULL && cpcm->cpcm_tag == watchfor) {
			*found = B_TRUE;
		}

		cpcm->cpcm_complete(cpcm);

		if (++cprq->cprq_read_index >= cprq->cprq_ntags) {
			cprq->cprq_read_index = 0;
			if (cprq->cprq_cycle_indicator == 1) {
				cprq->cprq_cycle_indicator = 0;
			} else {
				cprq->cprq_cycle_indicator = 1;
			}
		}
	}
}

/*
 * XXX
 */
int
cpqary3_retrieve(cpqary3_t *cpq)
{
	VERIFY(MUTEX_HELD(&cpq->hw_mutex));
	VERIFY(MUTEX_HELD(&cpq->sw_mutex));

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_retrieve_simple(cpq, 0, NULL);
		return (DDI_SUCCESS);

	case CPQARY3_CTLR_MODE_PERFORMANT:
		cpqary3_retrieve_performant(cpq, 0, NULL);
		return (DDI_SUCCESS);

	case CPQARY3_CTLR_MODE_UNKNOWN:
		break;
	}

	panic("unknown controller mode");
}

/*
 * Function	:  cpqary3_poll_retrieve
 * Description	:  This routine retrieves the completed command from the
 *			controller reply queue in poll mode.
 *			and processes the completed commands.
 * Called By	:  cpqary3_poll
 * Parameters	:  per-controller
 * Calls	:  packet completion routines
 * Return Values:  If the polled command is completed, send back a success.
 *			If not return failure.
 */
uint8_t
cpqary3_poll_retrieve(cpqary3_t *cpq, uint32_t poll_tag)
{
	boolean_t found = B_FALSE;

	VERIFY(MUTEX_HELD(&cpq->hw_mutex));
	VERIFY(MUTEX_HELD(&cpq->sw_mutex));

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_retrieve_simple(cpq, poll_tag, &found);
		break;

	case CPQARY3_CTLR_MODE_PERFORMANT:
		cpqary3_retrieve_performant(cpq, poll_tag, &found);
		break;

	default:
		panic("unknown controller mode");
	}

	return (found ? DDI_SUCCESS : DDI_FAILURE);
}

/*
 * Function	: 	cpqary3_submit
 * Description	: 	This routine submits the command to the Inbound Post Q.
 * Called By	: 	cpqary3_transport(), cpqary3_send_NOE_command(),
 *			cpqary3_disable_NOE_command(),
 *			cpqary3_handle_flag_nointr(),
 *			cpqary3_tick_hdlr(), cpqary3_synccmd_send()
 * Parameters	: 	per-controller, physical address
 * Calls	: 	None
 * Return Values: 	None
 */
int
cpqary3_submit(cpqary3_t *cpq, cpqary3_command_t *cpcm)
{
	VERIFY(MUTEX_HELD(&cpq->hw_mutex));

	/*
	 * If a controller lockup has been detected, reject new command
	 * submissions.
	 */
	if (cpq->controller_lockup == CPQARY3_TRUE) {
		return (EIO);
	}

	/*
	 * XXX
	 * At present, we have no good way to "re-use" an allocated
	 * command structure.  Let's catch any places where this happens.
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

	switch (cpq->cpq_ctlr_mode) {
	case CPQARY3_CTLR_MODE_SIMPLE:
		cpqary3_put32(cpq, CISS_I2O_INBOUND_POST_Q, cpcm->cpcm_pa_cmd);
		break;

	case CPQARY3_CTLR_MODE_PERFORMANT:
		/*
		 * XXX The driver always uses the 0th block fetch count always
		 *
		 * (NB: from spec, the 0x1 here sets "pull from host memory"
		 * mode, and the 0 represents "pull just one command record"
		 */
		cpqary3_put32(cpq, CISS_I2O_INBOUND_POST_Q,
		    cpcm->cpcm_pa_cmd | 0 | 0x1);
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

/*
 * Signal to the controller that we have updated the Configuration Table by
 * writing to the Inbound Doorbell Register.  The controller will, after some
 * number of seconds, acknowledge this by clearing the bit.
 *
 * If successful, return DDI_SUCCESS.  If the controller takes too long to
 * acknowledge, return DDI_FAILURE.
 */
static int
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

static int
cpqary3_cfgtbl_transport_has_support(cpqary3_t *cpq, int xport)
{
	VERIFY(xport == CISS_CFGTBL_XPORT_SIMPLE ||
	    xport == CISS_CFGTBL_XPORT_PERFORMANT);

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

static void
cpqary3_cfgtbl_transport_set(cpqary3_t *cpq, int xport)
{
	VERIFY(xport == CISS_CFGTBL_XPORT_SIMPLE ||
	    xport == CISS_CFGTBL_XPORT_PERFORMANT);

	ddi_put32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->HostWrite.TransportRequest, xport);
}

static int
cpqary3_cfgtbl_transport_confirm(cpqary3_t *cpq, int xport)
{
	VERIFY(xport == CISS_CFGTBL_XPORT_SIMPLE ||
	    xport == CISS_CFGTBL_XPORT_PERFORMANT);

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

static uint32_t
cpqary3_ctlr_get_cmdsoutmax(cpqary3_t *cpq)
{
	uint32_t val;

	if (cpq->cpq_ctlr_mode == CPQARY3_CTLR_MODE_PERFORMANT) {
		if ((val = ddi_get32(cpq->cpq_ct_handle,
		    &cpq->cpq_ct->MaxPerfModeCmdsOutMax)) != 0) {
			return (val);
		}
	}

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

static int
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
	cpqary3_intr_onoff(cpq, CPQARY3_INTR_DISABLE);

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

#if 0
	/*
	 * Set the controller interrupt check routine.
	 */
	cpq->check_ctlr_intr = cpqary3_check_simple_ctlr_intr;
#endif

	return (DDI_SUCCESS);
}

/*
 * XXX
 */
#if 0
static int
cpqary3_ctlr_init_performant(cpqary3_t *cpq)
{
	cpqary3_replyq_t *cprq = &cpq->cpq_replyq;

	VERIFY(cpq->cpq_ctlr_mode == CPQARY3_CTLR_MODE_UNKNOWN);

	if (cpqary3_cfgtbl_transport_has_support(cpq,
	    CISS_CFGTBL_XPORT_PERFORMANT) != DDI_SUCCESS) {
		return (ENOTTY);
	}
	cpq->cpq_ctlr_mode = CPQARY3_CTLR_MODE_PERFORMANT;

	if ((cpq->ctlr_maxcmds = cpqary3_ctlr_get_cmdsoutmax(cpq)) == 0) {
		dev_err(cpq->dip, CE_WARN, "maximum outstanding commands set "
		    "to zero");
		return (EPROTO);
	}

	/*
	 * Initialize the Performant Method Transport Method Table.
	 *
	 * XXX "Number of 4-byte nodes in each reply queue. Same for all reply
	 * queues."  Here we are passing the number of COMMANDS, which is the
	 * number of 8-byte nodes...
	 */
	DDI_PUT32_CP(cpq, &cpq->cp->ReplyQSize, cpq->ctlr_maxcmds);
	DDI_PUT32_CP(cpq, &cpq->cp->ReplyQCount, 1);
	DDI_PUT32_CP(cpq, &cpq->cp->ReplyQCntrAddrLow32, 0);
	DDI_PUT32_CP(cpq, &cpq->cp->ReplyQCntrAddrHigh32, 0);

	/*
	 * Each slot in the Reply Queue consists of two 4 byte integer
	 * fields.
	 */
	size_t qsize = cpq->ctlr_maxcmds * 2 * sizeof (uint32_t);

	if ((cprq->cprq_tags = (void *)cpqary3_alloc_phyctgs_mem(cpq, qsize,
	    &cprq->cprq_tags_pa, &cprq->cprq_phyctg)) == NULL) {
		dev_err(cpq->dip, CE_WARN, "could not allocate replyq");
		return (ENOMEM);
	}

	bzero(cprq->cprq_tags, qsize);
	cprq->cprq_ntags = cpq->ctlr_maxcmds;
	cprq->cprq_cycle_indicator = 1;
	cprq->cprq_read_index = 0;

	DDI_PUT32_CP(cpq, &cpq->cp->ReplyQAddr0Low32, cprq->cprq_tags_pa);
	DDI_PUT32_CP(cpq, &cpq->cp->ReplyQAddr0High32, 0);

	max_blk_fetch_cnt = DDI_GET32(cpq, &ctp->MaxBlockFetchCount);

	/*
	 * For non-proton FW controllers, max_blk_fetch_count is not
	 * implemented in the firmware
	 */

	/*
	 * When blk fetch count is 0, FW auto fetches 564 bytes
	 * corresponding to an optimal S/G of 31
	 */
	if (max_blk_fetch_cnt == 0) {
		BlockFetchCnt[0] = 35;
	} else {
		/*
		 * With MAX_PERF_SG_CNT set to 64, block fetch count
		 * is got by:(sizeof (CommandList_t) + 15)/16
		 */
		if (max_blk_fetch_cnt > 68)
			BlockFetchCnt[0] = 68;
		else
			BlockFetchCnt[0] = max_blk_fetch_cnt;
	}

	DDI_PUT32_CP(cpq, &perf_cfg->BlockFetchCnt[0], BlockFetchCnt[0]);

	/*
	 * Set the Transport Method and flush the changes to the
	 * Configuration Table.
	 */
	cpqary3_cfgtbl_transport_set(cpq, CISS_CFGTBL_XPORT_PERFORMANT);
	if (cpqary3_cfgtbl_flush(cpq) != DDI_SUCCESS) {
		return (EPROTO);
	}

	if (cpqary3_cfgtbl_transport_confirm(cpq,
	    CISS_CFGTBL_XPORT_PERFORMANT) != DDI_SUCCESS) {
		return (EPROTO);
	}

	/*
	 * XXX It's not clear why we check this a second time, but the original
	 * driver did.
	 */
	uint32_t check_again = cpqary3_ctlr_get_cmdsoutmax(cpq);
	if (check_again != cpq->ctlr_maxcmds) {
		dev_err(cpq->dip, CE_WARN, "maximum outstanding commands "
		    "changed during initialisation (was %u, now %u)",
		    cpq->ctlr_maxcmds, check_again);
		return (EPROTO);
	}

	/* SG */
	max_sg_cnt = DDI_GET32(cpq, &ctp->MaxSGElements);
	max_blk_fetch_cnt = DDI_GET32(cpq, &ctp->MaxBlockFetchCount);

	/* 32 byte aligned - size_of_cmdlist */
	size_of_cmdlist = ((sizeof (CommandList_t) + 31) / 32) * 32;
	size_of_HRE  = size_of_cmdlist -
	    (sizeof (SGDescriptor_t) * CISS_MAXSGENTRIES);

	if ((max_blk_fetch_cnt == 0) || (max_sg_cnt == 0) ||
	    ((max_blk_fetch_cnt * 16) <= size_of_HRE)) {
		cpq->sg_cnt = CPQARY3_PERF_SG_CNT;
	} else {
		/*
		 * Get the optimal_sg - no of the SG's that will fit
		 * into the max_blk_fetch_cnt
		 */

		optimal_sg_size = (max_blk_fetch_cnt * 16) - size_of_HRE;

		if (optimal_sg_size < sizeof (SGDescriptor_t)) {
			optimal_sg = CPQARY3_PERF_SG_CNT;
		} else {
			optimal_sg = optimal_sg_size / sizeof (SGDescriptor_t);
		}

		cpq->sg_cnt = MIN(max_sg_cnt, optimal_sg);

		if (cpq->sg_cnt > MAX_PERF_SG_CNT) {
			cpq->sg_cnt = MAX_PERF_SG_CNT;
		}
	}

	/* SG */

	/*
	 * Zero the Upper 32 Address in the Controller
	 */

	DDI_PUT32(cpq, &ctp->HostWrite.Upper32Addr, 0x00000000);

	return (0);
}
#endif

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

#if 0
	if (!(cpqary3p->bddef->bd_flags & SA_BD_SAS)) {
		if ((e = cpqary3_ctlr_init_simple(cpqary3p)) != 0) {
			return (e);
		}
	} else {
		if ((e = cpqary3_ctlr_init_performant(cpqary3p)) != 0) {
			return (e);
		}
	}
#else
	/*
	 * XXX Let's just do Simple mode for now...
	 */
	if ((e = cpqary3_ctlr_init_simple(cpq)) != 0) {
		return (e);
	}
#endif

	cpq->cpq_host_support = cpqary3_ctlr_get_hostdrvsup(cpq);

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
