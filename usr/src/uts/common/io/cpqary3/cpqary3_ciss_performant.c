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
	if ((cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS) & 0x1) != 0) {
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
	if ((cpqary3_get32(cpq, CISS_I2O_INTERRUPT_STATUS) & 0x4) != 0) {
		return (CPQARY3_SUCCESS);
	}

	return (CPQARY3_FAILURE);
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
