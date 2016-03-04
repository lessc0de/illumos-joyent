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

#include <sys/sdt.h>
#include "cpqary3.h"

/*
 * Local Functions Definitions
 */

static int cpqary3_tgt_init(dev_info_t *, dev_info_t *, scsi_hba_tran_t *,
    struct scsi_device *);
static int cpqary3_tgt_probe(struct scsi_device *, int (*)());
static int cpqary3_tran_start(struct scsi_address *, struct scsi_pkt *);
static int cpqary3_tran_reset(struct scsi_address *, int);
static int cpqary3_tran_abort(struct scsi_address *, struct scsi_pkt *);
static int cpqary3_getcap(struct scsi_address *, char *, int);
static int cpqary3_setcap(struct scsi_address *, char *, int, int);
static int cpqary3_dma_alloc(cpqary3_t *, struct scsi_pkt *,
    struct buf *, int, int (*)());
static int cpqary3_dma_move(struct scsi_pkt *, struct buf *, cpqary3_t *);
static int cpqary3_handle_flag_nointr(cpqary3_command_t *, struct scsi_pkt *);
static void cpqary3_dmafree(struct scsi_address *, struct scsi_pkt *);
static void cpqary3_dma_sync(struct scsi_address *, struct scsi_pkt *);
static void cpqary3_tran_destroy_pkt(struct scsi_address *,
    struct scsi_pkt *);
static struct scsi_pkt *cpqary3_tran_init_pkt(struct scsi_address *,
    struct scsi_pkt *, struct buf *, int, int, int, int, int (*callback)(),
    caddr_t);
static int cpqary3_additional_cmd(struct scsi_pkt *scsi_pktp, cpqary3_t *);
static boolean_t cpqary3_is_scsi_read_write(struct scsi_pkt *scsi_pktp);

/*
 * External Variable Declarations
 */

extern ddi_dma_attr_t cpqary3_dma_attr;

void
cpqary3_init_hbatran(cpqary3_t *cpq)
{
	scsi_hba_tran_t	*hba_tran = cpq->cpq_hba_tran;

	/*
	 * Memory for the transport vector has been allocated by now.
	 * initialize all the entry points in this vector
	 */

	hba_tran->tran_hba_private = cpq;

	/* Target Driver Instance Initialization */
	hba_tran->tran_tgt_init = cpqary3_tgt_init;
	hba_tran->tran_tgt_probe = cpqary3_tgt_probe;

	/* Resource Allocation */
	hba_tran->tran_init_pkt = cpqary3_tran_init_pkt;
	hba_tran->tran_destroy_pkt = cpqary3_tran_destroy_pkt;
	hba_tran->tran_sync_pkt = cpqary3_dma_sync;
	hba_tran->tran_dmafree = cpqary3_dmafree;

	/* Command Transport */
	hba_tran->tran_start = cpqary3_tran_start;

	/* Capability Management */
	hba_tran->tran_getcap = cpqary3_getcap;
	hba_tran->tran_setcap = cpqary3_setcap;

	/* Abort and Reset */
	hba_tran->tran_reset = cpqary3_tran_reset;
	hba_tran->tran_abort = cpqary3_tran_abort;
}

/* ARGSUSED */
static int
cpqary3_tgt_init(dev_info_t *hba_dip, dev_info_t *tgt_dip,
    scsi_hba_tran_t *hba_tran, struct scsi_device *sd)
{
	uint32_t	tid = SD2TGT(sd);
	uint32_t	lun = SD2LUN(sd);
	cpqary3_t	*ctlr;
	cpqary3_tgt_t *tgtp;

	ctlr = (cpqary3_t *)hba_tran->tran_hba_private;

	if ((CPQARY3_SUCCESS == cpqary3_probe4targets(ctlr)) &&
	    (ctlr->cpq_ntargets > 0)) {
		(void) cpqary3_detect_target_geometry(ctlr);
	}

	/*
	 * Validate the Target ID
	 * Validate Lun --Ver1.10--
	 * If not a valid target id, return FAILURE.
	 * Derieve the per-controller
	 */

	if ((tid >= CPQARY3_MAX_TGT) || (lun != 0)) {
		DTRACE_PROBE2(tgt_init_notsup,
		    cpqary3_t *, ctlr, uint32_t, tid);
		return (DDI_FAILURE);
	}

	/*
	 * Check to see if a target structure corrresponding to this
	 * target Id exists.(support only for Logical Drives and Controller)
	 * if target exists, update target flags, return SUCCESS
	 * is target does not exist, return FAILURE
	 */

	mutex_enter(&ctlr->cpq_mutex);

	if ((tgtp = cpqary3_target_from_id(ctlr, tid)) == NULL) {
		mutex_exit(&ctlr->cpq_mutex);
		return (DDI_FAILURE);
	}

	tgtp->tgt_dip = tgt_dip;
	tgtp->ctlr_flags = CPQARY3_CAP_DISCON_ENABLED |
	    CPQARY3_CAP_SYNC_ENABLED | CPQARY3_CAP_WIDE_XFER_ENABLED |
	    CPQARY3_CAP_ARQ_ENABLED;

	mutex_exit(&ctlr->cpq_mutex);

	DTRACE_PROBE1(tgt_init_done, uint32_t, tid);

	return (DDI_SUCCESS);
}

void
cpqary3_tgt_free(dev_info_t *hba_dip, dev_info_t *tgt_dip,
    scsi_hba_tran_t *hba_tran, struct scsi_device *sd)
{
	cpqary3_t *cpq = (cpqary3_t *)hba_tran->tran_hba_private;

	mutex_enter(&cpq->cpq_mutex);
	/*
	 * XXX
	 */
	mutex_exit(&cpq->cpq_mutex);
}

static int
cpqary3_tgt_probe(struct scsi_device *sd, int (*waitfunc)())
{
	/*
	 * Probe for the presence of the target, using the scsi_hba_probe().
	 * It inturn issues the SCSI inquiry command that is serviced by our
	 * driver
	 */

	extern int8_t		cpqary3_detect_target_geometry(cpqary3_t *);
	struct scsi_hba_tran	*hba_tran = sd->sd_address.a_hba_tran;
	cpqary3_t		*ctlr = hba_tran->tran_hba_private;

	if ((CPQARY3_SUCCESS == cpqary3_probe4targets(ctlr)) &&
	    (ctlr->cpq_ntargets > 0)) {
		(void) cpqary3_detect_target_geometry(ctlr);
	}

	return (scsi_hba_probe(sd, waitfunc));
}

/* ARGSUSED */
static struct scsi_pkt *
cpqary3_tran_init_pkt(struct scsi_address *sa, struct scsi_pkt *scsi_pktp,
    struct buf *bp, int cmdlen, int statuslen, int tgtlen,
    int flags, int (*callback)(), caddr_t arg)
{
	cpqary3_t *cpq = SA2CTLR(sa);
	boolean_t allocated_packet = B_FALSE;

	cpqary3_pkt_t	*privp;
	struct scsi_pkt	*new_scsi_pktp;

	ASSERT(callback == NULL_FUNC || callback == SLEEP_FUNC);

	if (scsi_pktp == NULL) {
		/*
		 * The framework requires we allocate a new packet
		 * structure via scsi_hba_pkt_alloc(9F).
		 */
		if ((scsi_pktp = scsi_hba_pkt_alloc(cpq->dip, sa, cmdlen,
		    statuslen, tgtlen, sizeof (cpqary3_pkt_t), callback,
		    NULL)) == NULL) {
			return (NULL);
		}
		allocated_packet = B_TRUE;
	}

	privp = (cpqary3_pkt_t *)scsi_pktp->pkt_ha_private;

	if (allocated_packet) {
		/*
		 * Our private SCSI packet object was just allocated by
		 * scsi_hba_pkt_alloc(9F).  Initialise it:
		 */
		bzero(privp, sizeof (*privp));

		/*
		 * Link our private point back to the SCSI framework
		 * packet object, and include the CDB/SCB lengths we
		 * have been passed.
		 */
		privp->scsi_cmd_pkt = scsi_pktp;
		privp->cdb_len = cmdlen;
		privp->scb_len = statuslen;

		if (flags & PKT_DMA_PARTIAL) {
			privp->cmd_flags |= DDI_DMA_PARTIAL;
		}
	}

	if (bp != NULL && allocated_packet) {
		/*
		 * This is a new packet with an associated buffer.  The
		 * framework requires us to allocate appropriate DMA
		 * resources.
		 */
		privp->bf = bp;
		if (cpqary3_dma_alloc(cpq, scsi_pktp, bp, flags,
		    callback) != DDI_SUCCESS) {
			scsi_hba_pkt_free(sa, scsi_pktp);
			return (NULL);
		}
	} else if (bp != NULL && !allocated_packet &&
	    (flags & PKT_DMA_PARTIAL) != 0) {
		/*
		 * This is not a new packet, but a buffer was passed in and we
		 * had previously allocated DMA resources.  This is a request
		 * from the framework to move the DMA resources.
		 */
		if (cpqary3_dma_move(scsi_pktp, bp, cpq) != DDI_SUCCESS) {
			return (NULL);
		}
	}

	return (scsi_pktp);
}

/*
 * Function	:	cpqary3_dma_alloc()
 * Description	: 	This routine services requests for memory (dynamic)
 *			as and when required by the OS.
 * Called By	: 	cpqary3_init_pkt()
 * Parameters	: 	per-controller, SCSI packet, buffer, flag modifier,
 *			callback function
 * Calls	: 	None
 * Return Values: 	SUCCESS / FAILURE
 */
static int
cpqary3_dma_alloc(cpqary3_t *cpqary3p, struct scsi_pkt *scsi_pktp,
    struct buf *bp, int flags, int (*callback)())
{
	int32_t		(*cb)(caddr_t);
	int32_t		retvalue;
	uint32_t	i = 0;
	uint32_t	dma_flags;
	cpqary3_pkt_t	*cpqary3_pktp;
	ddi_dma_attr_t	tmp_dma_attr;

	cpqary3_pktp = (cpqary3_pkt_t *)scsi_pktp->pkt_ha_private;

	ASSERT(callback == NULL_FUNC || callback == SLEEP_FUNC);
	/*
	 * Record the direction of the data transfer, so that it
	 * can be used in appropriate synchronization during cpqary3_sync_pkt()
	 */
	if (bp->b_flags & B_READ) {
		cpqary3_pktp->cmd_flags &= ~CFLAG_DMASEND;
		dma_flags = DDI_DMA_READ;
	} else {
		cpqary3_pktp->cmd_flags |= CFLAG_DMASEND;
		dma_flags = DDI_DMA_WRITE;
	}

	if (flags & PKT_CONSISTENT) {
		cpqary3_pktp->cmd_flags |= CFLAG_CMDIOPB;
		dma_flags |= DDI_DMA_CONSISTENT;
	}

	if (flags & PKT_DMA_PARTIAL) {
		dma_flags |= DDI_DMA_PARTIAL;
	}

	tmp_dma_attr = cpqary3_dma_attr;
	tmp_dma_attr.dma_attr_sgllen = cpqary3p->cpq_sg_cnt;

	cb = (callback == NULL_FUNC) ? DDI_DMA_DONTWAIT : DDI_DMA_SLEEP;

	/*
	 * DMA resources are allocated thru a 2 step protocol :
	 * - allocate a DMA handle
	 * - bind the buffer to the handle
	 * If both the steps succeed, we have succeeded in allocating resources
	 */

	if (DDI_SUCCESS != (retvalue = ddi_dma_alloc_handle(cpqary3p->dip,
	    &tmp_dma_attr, cb, CPQARY3_DMA_NO_CALLBACK,
	    &cpqary3_pktp->cmd_dmahandle))) {
		switch (retvalue) {
		case DDI_DMA_NORESOURCES:
			/*
			 * No Resources are available to be allocated
			 */
			bioerror(bp, CPQARY3_BUFFER_ERROR_CLEAR);
			break;

		case DDI_DMA_BADATTR:
			/*
			 * The attributes stated in our DMA attribute
			 * structure is such that potential DMA resources can
			 * not be allocated.
			 */
			cmn_err(CE_CONT, "CPQary3: DmaAlloc: "
			    "AllocHandle Failed BadAttr\n");
			bioerror(bp, EFAULT);
			break;

		default:
			/*
			 * There is no other possible return value
			 */
			cmn_err(CE_WARN,
			    "CPQary3: dma_alloc: Unexpected Return Value %x "
			    "From call to Allocate DMA Handle \n", retvalue);
			break;
		}
		return (CPQARY3_FAILURE);
	}

	retvalue = ddi_dma_buf_bind_handle(cpqary3_pktp->cmd_dmahandle, bp,
	    dma_flags, cb, CPQARY3_DMA_NO_CALLBACK,
	    &cpqary3_pktp->cmd_dmacookies[0], &cpqary3_pktp->cmd_ncookies);

	switch (retvalue) {
	case DDI_DMA_PARTIAL_MAP :
	case DDI_DMA_MAPPED :
		if (DDI_DMA_PARTIAL_MAP == retvalue) {
			if (ddi_dma_numwin(cpqary3_pktp->cmd_dmahandle,
			    &cpqary3_pktp->cmd_nwin) == DDI_FAILURE) {
				cmn_err(CE_PANIC, "CPQary3: Retrieval of DMA "
				    "windows number failed");
			}

			if (ddi_dma_getwin(cpqary3_pktp->cmd_dmahandle,
			    cpqary3_pktp->cmd_curwin,
			    &cpqary3_pktp->cmd_dma_offset,
			    &cpqary3_pktp->cmd_dma_len,
			    &cpqary3_pktp->cmd_dmacookies[0],
			    &cpqary3_pktp->cmd_ncookies) == DDI_FAILURE) {
				cmn_err(CE_PANIC, "CPQary3: Activation of New "
				    "DMA Window Failed");
			}
		} else {
			cpqary3_pktp->cmd_nwin = 1;
			cpqary3_pktp->cmd_dma_len = 0;
			cpqary3_pktp->cmd_dma_offset = 0;
		}

		cpqary3_pktp->cmd_dmacount = 0;
		i = 0;
		for (;;) {
			cpqary3_pktp->cmd_dmacount +=
			    cpqary3_pktp->cmd_dmacookies[i++].dmac_size;
			/* SG */
			/* Check Out for Limits */
			if (i == cpqary3p->cpq_sg_cnt ||
			    i == cpqary3_pktp->cmd_ncookies)
				break;
			/* SG */

			ddi_dma_nextcookie(cpqary3_pktp->cmd_dmahandle,
			    &cpqary3_pktp->cmd_dmacookies[i]);
		}

		cpqary3_pktp->cmd_cookie = i;
		cpqary3_pktp->cmd_cookiecnt = i;
		cpqary3_pktp->cmd_flags |= CFLAG_DMAVALID;

		scsi_pktp->pkt_resid =
		    bp->b_bcount - cpqary3_pktp->cmd_dmacount;

		return (CPQARY3_SUCCESS);

	case DDI_DMA_NORESOURCES:
		bioerror(bp, CPQARY3_BUFFER_ERROR_CLEAR);
		break;

	case DDI_DMA_NOMAPPING:
		bioerror(bp, EFAULT);
		break;

	case DDI_DMA_TOOBIG:
		bioerror(bp, EINVAL);
		break;

	case DDI_DMA_INUSE:
		cmn_err(CE_PANIC, "CPQary3: Another I/O transaction "
		    "is using the DMA handle");

	default:
		cmn_err(CE_PANIC, "CPQary3: Unexpected ERROR "
		    "returned from Call to Bind Buffer "
		    "to Handle : 0x%X", i);
	}

	ddi_dma_free_handle(&cpqary3_pktp->cmd_dmahandle);
	cpqary3_pktp->cmd_dmahandle = NULL;
	cpqary3_pktp->cmd_flags &= ~CFLAG_DMAVALID;

	return (CPQARY3_FAILURE);

}

/*
 * Function	:	cpqary3_dma_move()
 * Description	: 	This routine gets the next DMA window.
 * Called By	: 	cpqary3_init_pkt()
 * Parameters	: 	per-controller, SCSI packet, buffer
 * Calls	: 	None
 * Return Values: 	SUCCESS / FAILURE
 */
static int
cpqary3_dma_move(struct scsi_pkt *scsi_pktp, struct buf *bp,
    cpqary3_t *cpqary3p)
{
	uint32_t		i = 0;
	cpqary3_pkt_t	*cpqary3_pktp;

	cpqary3_pktp = PKT2PVTPKT(scsi_pktp);

	/*
	 * If there are no more cookies remaining in this window,
	 * must move to the next window first.
	 */
	if (cpqary3_pktp->cmd_cookie == cpqary3_pktp->cmd_ncookies) {
		/* For small pkts, leave things where they are */
		if ((cpqary3_pktp->cmd_curwin == cpqary3_pktp->cmd_nwin) &&
		    (cpqary3_pktp->cmd_nwin == 1))
			return (CPQARY3_SUCCESS);

		/* Shall not be able to move if last window */
		if (++cpqary3_pktp->cmd_curwin >= cpqary3_pktp->cmd_nwin)
			return (CPQARY3_FAILURE);

		if (ddi_dma_getwin(cpqary3_pktp->cmd_dmahandle,
		    cpqary3_pktp->cmd_curwin, &cpqary3_pktp->cmd_dma_offset,
		    &cpqary3_pktp->cmd_dma_len,
		    &cpqary3_pktp->cmd_dmacookies[0],
		    &cpqary3_pktp->cmd_ncookies) == DDI_FAILURE)
			return (CPQARY3_FAILURE);

		cpqary3_pktp->cmd_cookie = 0;
	} else {
		/* Still more cookies in this window - get the next one */
		ddi_dma_nextcookie(cpqary3_pktp->cmd_dmahandle,
		    &cpqary3_pktp->cmd_dmacookies[0]);
	}

	/* Get remaining cookies in this window, up to our maximum */
	for (;;) {
		cpqary3_pktp->cmd_dmacount +=
		    cpqary3_pktp->cmd_dmacookies[i++].dmac_size;
		cpqary3_pktp->cmd_cookie++;
		/* SG */
		/* no. of DATA SEGMENTS */
		if (i == cpqary3p->cpq_sg_cnt ||
		    cpqary3_pktp->cmd_cookie == cpqary3_pktp->cmd_ncookies) {
			break;
		}
		/* SG */

		ddi_dma_nextcookie(cpqary3_pktp->cmd_dmahandle,
		    &cpqary3_pktp->cmd_dmacookies[i]);
	}

	cpqary3_pktp->cmd_cookiecnt = i;
	scsi_pktp->pkt_resid = bp->b_bcount - cpqary3_pktp->cmd_dmacount;

	return (CPQARY3_SUCCESS);

}

static void
cpqary3_set_arq_data(struct scsi_pkt *pkt, uchar_t key)
{
	struct scsi_arq_status *arqstat;

	arqstat = (struct scsi_arq_status *)(pkt->pkt_scbp);

	arqstat->sts_status.sts_chk = 1; /* CHECK CONDITION */
	arqstat->sts_rqpkt_reason = CMD_CMPLT;
	arqstat->sts_rqpkt_resid = 0;
	arqstat->sts_rqpkt_state = STATE_GOT_BUS | STATE_GOT_TARGET |
	    STATE_SENT_CMD | STATE_XFERRED_DATA;
	arqstat->sts_rqpkt_statistics = 0;
	arqstat->sts_sensedata.es_valid = 1;
	arqstat->sts_sensedata.es_class = CLASS_EXTENDED_SENSE;
	arqstat->sts_sensedata.es_key = key;
}

static int
cpqary3_tran_start(struct scsi_address *sa, struct scsi_pkt *scsi_pktp)
{
	cpqary3_t *cpq = SA2CTLR(sa);
	cpqary3_pkt_t *privp = PKT2PVTPKT(scsi_pktp);
	cpqary3_tgt_t *tgtp;
	cpqary3_command_t *cpcm;
	int r;
	boolean_t nointr = (scsi_pktp->pkt_flags & FLAG_NOINTR) != 0;

	/*
	 * Determine the target to which this command is addressed.
	 */
	if ((tgtp = cpqary3_target_from_addr(cpq, sa)) == NULL ||
	    tgtp->type == CPQARY3_TARGET_NONE) {
		/*
		 * This target does not exist.
		 */
		return (TRAN_FATAL_ERROR);
	}

	/*
	 * Initialise the SCSI packet as described in tran_start(9E).  We will
	 * progressively update these fields as the command moves through the
	 * submission and completion states.
	 */
	scsi_pktp->pkt_resid = 0;
	scsi_pktp->pkt_reason = CMD_CMPLT;
	scsi_pktp->pkt_statistics = 0;
	scsi_pktp->pkt_state = 0;

	/*
	 * Check to see if we need any special handling for this SCSI
	 * command.
	 */
	switch (scsi_pktp->pkt_cdbp[0]) {
	case SCMD_FORMAT:
	case SCMD_LOG_SENSE_G1:
	case SCMD_MODE_SELECT:
	case SCMD_PERSISTENT_RESERVE_IN:
		/*
		 * These SCSI commands are allegedly not supported by the
		 * controller firmware.
		 */
		cpqary3_set_arq_data(scsi_pktp, KEY_ILLEGAL_REQUEST);
		scsi_pktp->pkt_reason = CMD_BADMSG;
		scsi_pktp->pkt_state |= STATE_GOT_BUS | STATE_GOT_TARGET |
		    STATE_SENT_CMD | STATE_XFERRED_DATA;
		if (!nointr && scsi_pktp->pkt_comp != NULL) {
			(*scsi_pktp->pkt_comp)(scsi_pktp);
		}
		return (TRAN_ACCEPT);

	case SCMD_SYNCHRONIZE_CACHE:
		/*
		 * Emulate SYNCHRONIZE CACHE with the BMIC Flush Cache
		 * command.
		 */
		scsi_pktp->pkt_state |= STATE_GOT_BUS | STATE_GOT_TARGET |
		    STATE_SENT_CMD;
		if ((r = cpqary3_flush_cache(cpq)) == ENOMEM) {
			return (TRAN_BUSY);
		} else if (r != 0) {
			scsi_pktp->pkt_reason = CMD_TIMEOUT;
			scsi_pktp->pkt_statistics |= STAT_TIMEOUT;
		} else {
			scsi_pktp->pkt_state |= STATE_XFERRED_DATA |
			    STATE_GOT_STATUS;
		}
		if (!nointr && scsi_pktp->pkt_comp != NULL) {
			(*scsi_pktp->pkt_comp)(scsi_pktp);
		}
		return (TRAN_ACCEPT);
	}

	/*
	 * Allocate a command object for this transaction:
	 */
	if ((cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_OS)) == NULL) {
		/*
		 * Signal to the framework to back off.
		 */
		return (TRAN_BUSY);
	}
	cpcm->cpcm_private = privp;
	privp->cmd_command = cpcm;
	if (nointr) {
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_POLLED;
	}

	if ((privp->cmd_flags & CFLAG_CMDIOPB) && privp->cmd_dmahandle) {
		(void) ddi_dma_sync(privp->cmd_dmahandle, 0, 0,
		    DDI_DMA_SYNC_FORDEV);
	}

	/*
	 * Build the command to send to the controller based on the SCSI
	 * packet passed in by the framework.
	 */
	VERIFY(privp->cmd_cookiecnt <= cpq->cpq_sg_cnt);
	cpqary3_build_cmdlist(cpcm, tgtp);

	mutex_enter(&cpq->cpq_mutex);
	if ((r = cpqary3_submit(cpq, cpcm)) != 0) {
		mutex_exit(&cpq->cpq_mutex);

		/*
		 * The command could not be submitted, so free it now.
		 */
		cpqary3_command_free(cpcm);

		/*
		 * Inform the SCSI framework that we could not submit
		 * the command.
		 */
		return (r == EAGAIN ? TRAN_BUSY : TRAN_FATAL_ERROR);
	}

	/*
	 * Update the SCSI packet to reflect submission of the command.
	 */
	scsi_pktp->pkt_state |= STATE_GOT_BUS | STATE_GOT_TARGET |
	    STATE_SENT_CMD;

	if (!nointr) {
		/*
		 * This is a regular command submission, so we return
		 * without polling for completion.
		 */
		mutex_exit(&cpq->cpq_mutex);
		return (TRAN_ACCEPT);
	}

	/*
	 * Poll the controller for completion of the command we submitted.
	 * Once this routine has returned, the completion callback will have
	 * been fired with either an active response (success or error) or a
	 * timeout.  The command is freed by the completion callback, so it
	 * may not be referenced again after this call returns.
	 */
	cpqary3_poll_for(cpq, cpcm);
	mutex_exit(&cpq->cpq_mutex);
	return (TRAN_ACCEPT);
}

/*
 * Function	:	cpqary3_dmafree
 * Description	: 	This routine de-allocates previously allocated
 *			DMA resources.
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, SCSI packet
 * Calls	: 	None
 * Return Values: 	None
 */
/* ARGSUSED */
static void
cpqary3_dmafree(struct scsi_address *sa, struct scsi_pkt *scsi_pktp)
{
	cpqary3_pkt_t	*cpqary3_pktp;

	cpqary3_pktp = PKT2PVTPKT(scsi_pktp);

	/*
	 * If any DMA was succesfully attempted earlier, free all allocated
	 * resources
	 */

	if (cpqary3_pktp->cmd_flags & CFLAG_DMAVALID) {
		if (!cpqary3_pktp->cmd_dmahandle) {
			DTRACE_PROBE(dmafree_null);
			return;
		}
		cpqary3_pktp->cmd_flags &= ~CFLAG_DMAVALID;
		(void) ddi_dma_unbind_handle(cpqary3_pktp->cmd_dmahandle);
		ddi_dma_free_handle(&cpqary3_pktp->cmd_dmahandle);
		cpqary3_pktp->cmd_dmahandle = NULL;
	}
}

/*
 * Function	:	cpqary3_dma_sync
 * Description	: 	This routine synchronizes the CPU's / HBA's view of
 *			the data associated with the pkt, typically by calling
 *			ddi_dma_sync().
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, SCSI packet
 * Calls	: 	None
 * Return Values: 	None
 */
/* ARGSUSED */
static void
cpqary3_dma_sync(struct scsi_address *sa, struct scsi_pkt *scsi_pktp)
{
	cpqary3_pkt_t	*cpqary3_pktp = PKT2PVTPKT(scsi_pktp);

	/*
	 * Check whether DMA was attempted successfully earlier
	 * If yes and
	 * if the command flags is write, then synchronise the device else
	 * synchronise the CPU
	 */

	if (cpqary3_pktp->cmd_flags & CFLAG_DMAVALID) {
		(void) ddi_dma_sync(cpqary3_pktp->cmd_dmahandle,
		    cpqary3_pktp->cmd_dma_offset, cpqary3_pktp->cmd_dma_len,
		    (cpqary3_pktp->cmd_flags & CFLAG_DMASEND) ?
		    DDI_DMA_SYNC_FORDEV : DDI_DMA_SYNC_FORCPU);
	}
}

/*
 * Function	:	cpqary3_destroy_pkt
 * Description	: 	This routine de-allocates previously allocated
 *			resources for the SCSI packet.
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, SCSI packet
 * Calls	: 	None
 * Return Values: 	None
 */
static void
cpqary3_tran_destroy_pkt(struct scsi_address *sa, struct scsi_pkt *scsi_pktp)
{
	cpqary3_pkt_t	*cpqary3_pktp;

	cpqary3_pktp = PKT2PVTPKT(scsi_pktp);

	/*
	 * Deallocate DMA Resources, if allocated.
	 * Free the SCSI Packet.
	 */

	if (cpqary3_pktp->cmd_flags & CFLAG_DMAVALID) {
		if (!cpqary3_pktp->cmd_dmahandle) {
			DTRACE_PROBE(dmafree_null);
		} else {
			cpqary3_pktp->cmd_flags &= ~CFLAG_DMAVALID;

			(void) ddi_dma_unbind_handle(
			    cpqary3_pktp->cmd_dmahandle);
			ddi_dma_free_handle(&cpqary3_pktp->cmd_dmahandle);

			cpqary3_pktp->cmd_dmahandle = NULL;
		}
	}

	scsi_hba_pkt_free(sa, scsi_pktp);
}

/*
 * The tran_reset(9E) entry point is called when the SCSI framework wishes us
 * to reset the SCSI bus (RESET_ALL), a particular target (RESET_TARGET), or a
 * particular logical unit (RESET_LUN).
 *
 * On success, return 1; on failure, return 0.
 */
static int
cpqary3_tran_reset(struct scsi_address *sa, int level)
{
	/*
	 * We currently have no support for resetting the controller.
	 * Signal universal failure to the SCSI framework.
	 */
	return (0);
}

/*
 * Function	:	cpqary3_abort()
 * Description	: 	This routine aborts a particular command or all commands
 *			directed towards a target.
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, SCSI packet
 * Calls	: 	None
 * Return Values: 	SUCCESS / FAILURE
 *			[ abort of concernd command(s) was a success or
 *			a failure. ]
 */
static int
cpqary3_tran_abort(struct scsi_address *sa, struct scsi_pkt *scsi_pktp)
{
	cpqary3_t *cpq = SA2CTLR(sa);
	cpqary3_command_t *cpcm = NULL;
	cpqary3_tgt_t *tgtp;
	int r;

	mutex_enter(&cpq->cpq_mutex);

	if ((tgtp = cpqary3_target_from_addr(cpq, sa)) == NULL) {
		return (0);
	}

	/*
	 * If SCSI packet exists, abort that particular command.
	 * Else, abort all existing commands to the target
	 * In either of the cases, we shall have to wait after the abort
	 * functions are called to return the status.
	 */
	if (scsi_pktp != NULL) {
		cpqary3_pkt_t *pktp = (cpqary3_pkt_t *)scsi_pktp->
		    pkt_ha_private;

		cpcm = pktp->cmd_command;
	}

	r = cpqary3_send_abortcmd(cpq, tgtp, cpcm);

	mutex_exit(&cpq->cpq_mutex);
	return (r == 0 ? 1 : 0);
}

/*
 * Function	:	cpqary3_getcap
 * Description	: 	This routine is called to get the current value of a
 *			capability.(SCSI transport capability)
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, capability identifier, target(s) affected
 * Calls	: 	None
 * Return Values: 	current value of capability / -1 (if unsupported)
 */
static int
cpqary3_getcap(struct scsi_address *sa, char *capstr, int tgtonly)
{
	int index;
	cpqary3_t *cpq = SA2CTLR(sa);
	cpqary3_tgt_t *tgtp = cpqary3_target_from_addr(cpq, sa);

	/*
	 * If requested Capability is not supported, return -1.
	 */
	if ((index = scsi_hba_lookup_capstr(capstr)) == DDI_FAILURE) {
		return (CAP_NOT_DEFINED);
	}

	/*
	 * Getting capability for a particulat target is supported
	 * the generic form of tran_getcap() is unsupported(for all targets)
	 * If directed towards a particular target, return current capability.
	 */
	if (tgtonly == 0) {	/* all targets */
		DTRACE_PROBE1(getcap_alltgt, int, index);
		return (CAP_NOT_DEFINED);
	}

	DTRACE_PROBE1(getcap_index, int, index);

	switch (index) {
	case SCSI_CAP_DMA_MAX:
		return ((int)cpqary3_dma_attr.dma_attr_maxxfer);
	case SCSI_CAP_DISCONNECT:
		return (tgtp->ctlr_flags & CPQARY3_CAP_DISCON_ENABLED);
	case SCSI_CAP_SYNCHRONOUS:
		return (tgtp->ctlr_flags & CPQARY3_CAP_SYNC_ENABLED);
	case SCSI_CAP_WIDE_XFER:
		return (tgtp->ctlr_flags & CPQARY3_CAP_WIDE_XFER_ENABLED);
	case SCSI_CAP_ARQ:
		return ((tgtp->ctlr_flags & CPQARY3_CAP_ARQ_ENABLED) ? 1 : 0);
	case SCSI_CAP_INITIATOR_ID:
		return (CTLR_SCSI_ID);
	case SCSI_CAP_UNTAGGED_QING:
		return (1);
	case SCSI_CAP_TAGGED_QING:
		return (1);
	case SCSI_CAP_SECTOR_SIZE:
		return (cpqary3_dma_attr.dma_attr_granular);
	case SCSI_CAP_TOTAL_SECTORS:
		return (CAP_NOT_DEFINED);
	case SCSI_CAP_GEOMETRY:
		return (cpqary3_target_geometry(sa));
	case SCSI_CAP_RESET_NOTIFICATION:
		return (0);
	default:
		return (CAP_NOT_DEFINED);
	}
}

/*
 * Function	:	cpqary3_setcap
 * Description	: 	This routine is called to set the current value of a
 *			capability.(SCSI transport capability)
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, capability identifier,
 *			new capability value, target(s) affected
 * Calls	: 	None
 * Return Values: 	SUCCESS / FAILURE / -1 (if capability is unsupported)
 */
/* ARGSUSED */
static int
cpqary3_setcap(struct scsi_address *sa, char *capstr, int value, int tgtonly)
{
	int	index;
	int	retstatus = CAP_NOT_DEFINED;

	/*
	 * If requested Capability is not supported, return -1.
	 */
	if ((index = scsi_hba_lookup_capstr(capstr)) == DDI_FAILURE)
		return (retstatus);

	/*
	 * Setting capability for a particulat target is supported
	 * the generic form of tran_setcap() is unsupported(for all targets)
	 * If directed towards a particular target, set & return current
	 * capability.
	 */
	if (!tgtonly) {
		DTRACE_PROBE1(setcap_alltgt, int, index);
		return (retstatus);
	}

	DTRACE_PROBE1(setcap_index, int, index);

	switch (index) {
	case SCSI_CAP_DMA_MAX:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_DISCONNECT:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_SYNCHRONOUS:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_WIDE_XFER:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_ARQ:
		return (1);
	case SCSI_CAP_INITIATOR_ID:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_UNTAGGED_QING:
		return (1);
	case SCSI_CAP_TAGGED_QING:
		return (1);
	case SCSI_CAP_SECTOR_SIZE:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_TOTAL_SECTORS:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_GEOMETRY:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_RESET_NOTIFICATION:
		return (CAP_CHG_NOT_ALLOWED);
	default:
		return (CAP_NOT_DEFINED);
	}
}

void
cpqary3_oscmd_complete(cpqary3_command_t *cpcm)
{
	cpqary3_t	*cpqary3p = cpcm->cpcm_ctlr;
	ErrorInfo_t	*errorinfop = cpcm->cpcm_va_err;
	CommandList_t	*cmdlistp = cpcm->cpcm_va_cmd;
	struct scsi_pkt	*scsi_pktp = cpcm->cpcm_private->scsi_cmd_pkt;
	boolean_t nointr = (scsi_pktp->pkt_flags & FLAG_NOINTR) != 0;

	VERIFY(MUTEX_HELD(&cpqary3p->cpq_mutex));
	VERIFY(cpcm->cpcm_type == CPQARY3_CMDTYPE_OS);

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_TIMEOUT) {
		scsi_pktp->pkt_reason = CMD_TIMEOUT;
		scsi_pktp->pkt_statistics |= STAT_TIMEOUT;
		goto finish;
	}

	if (!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR)) {
		scsi_pktp->pkt_state |= STATE_XFERRED_DATA | STATE_GOT_STATUS;
		goto finish;
	}

	switch (errorinfop->CommandStatus) {
	case CISS_CMD_DATA_OVERRUN:
		scsi_pktp->pkt_reason = CMD_DATA_OVR;
		scsi_pktp->pkt_state |= STATE_XFERRED_DATA | STATE_GOT_STATUS;
		break;

	case CISS_CMD_INVALID:
		DTRACE_PROBE1(invalid_cmd, struct scsi_pkt *, scsi_pktp);
		scsi_pktp->pkt_reason = CMD_BADMSG;
		scsi_pktp->pkt_state |= STATE_GOT_STATUS;
		break;

	case CISS_CMD_PROTOCOL_ERR :
		scsi_pktp->pkt_reason = CMD_BADMSG;
		scsi_pktp->pkt_state |= STATE_GOT_STATUS;
		break;

	case CISS_CMD_HARDWARE_ERR:
	case CISS_CMD_CONNECTION_LOST:
		scsi_pktp->pkt_reason = CMD_INCOMPLETE;
		scsi_pktp->pkt_state = 0; /* XXX ? */
		break;

	case CISS_CMD_ABORTED:
	case CISS_CMD_UNSOLICITED_ABORT:
		scsi_pktp->pkt_reason = CMD_ABORTED;
		scsi_pktp->pkt_statistics |= STAT_ABORTED;
		scsi_pktp->pkt_state = STATE_XFERRED_DATA | STATE_GOT_STATUS;
		break;

	case CISS_CMD_ABORT_FAILED:
		break;

	case CISS_CMD_TIMEOUT:
		scsi_pktp->pkt_reason = CMD_TIMEOUT;
		scsi_pktp->pkt_statistics |= STAT_TIMEOUT;
		break;

	case CISS_CMD_DATA_UNDERRUN:	/* Significant ONLY for Read & Write */
		if (cpqary3_is_scsi_read_write(scsi_pktp)) {
			scsi_pktp->pkt_reason = CMD_CMPLT;
			scsi_pktp->pkt_statistics = 0;
			scsi_pktp->pkt_state =
			    STATE_GOT_BUS | STATE_GOT_TARGET | STATE_SENT_CMD |
			    STATE_XFERRED_DATA | STATE_GOT_STATUS;
			break;
		}
		/* FALLTHROUGH */
	case CISS_CMD_SUCCESS:
	case CISS_CMD_TARGET_STATUS:
		scsi_pktp->pkt_reason = CMD_CMPLT;
		scsi_pktp->pkt_statistics = 0;
		scsi_pktp->pkt_state = STATE_GOT_BUS | STATE_GOT_TARGET |
		    STATE_SENT_CMD | STATE_XFERRED_DATA | STATE_GOT_STATUS;
		break;

	default:	/* Should never Occur !!! */
		scsi_pktp->pkt_reason = CMD_TRAN_ERR;
		break;
	}


	/*
	 * if ever a command completes with a CHECK CONDITION or a
	 * COMMAND_TERMINATED SCSI status, Update the sense data.
	 * NOTE : The CISS_CMD_INVALID command status would always result in a
	 * CHECK CONDITION and hence reach this part of the code.
	 */

	if ((errorinfop->ScsiStatus == SCSI_CHECK_CONDITION) ||
	    (errorinfop->ScsiStatus == SCSI_COMMAND_TERMINATED)) {
		if (errorinfop->SenseLen) {
			struct scsi_arq_status	*arq_statusp;
			arq_statusp =
			    /* LINTED: alignment */
			    (struct scsi_arq_status *)scsi_pktp->pkt_scbp;

			if ((errorinfop->ScsiStatus == SCSI_CHECK_CONDITION)) {
				arq_statusp->sts_status.sts_chk = (uint8_t)1;
			} else {
				arq_statusp->sts_status.sts_chk = (uint8_t)1;
				arq_statusp->sts_status.sts_scsi2 = (uint8_t)1;
			}
			bzero((void *)&(arq_statusp->sts_rqpkt_status),
			    sizeof (struct scsi_status));
			arq_statusp->sts_rqpkt_reason = CMD_CMPLT;
			arq_statusp->sts_rqpkt_resid = 0;
			arq_statusp->sts_rqpkt_state = scsi_pktp->pkt_state;
			arq_statusp->sts_rqpkt_statistics =
			    scsi_pktp->pkt_statistics;
			bcopy((caddr_t)&errorinfop->SenseInfo[0],
			    (caddr_t)(&arq_statusp->sts_sensedata),
			    MIN(errorinfop->SenseLen,
			    cpcm->cpcm_private->scb_len));
			scsi_pktp->pkt_state |= STATE_ARQ_DONE;
		}
	}

finish:
	mutex_exit(&cpqary3p->cpq_mutex);
	cpqary3_command_free(cpcm);
	if (!nointr && scsi_pktp->pkt_comp != NULL) {
		(*scsi_pktp->pkt_comp)(scsi_pktp);
	}
	mutex_enter(&cpqary3p->cpq_mutex);
}

static boolean_t
cpqary3_is_scsi_read_write(struct scsi_pkt *scsi_pktp)
{
	/*
	 * In the scsi packet structure, the first byte is the SCSI Command
	 * OpCode.  We check to see if it is any one of the SCSI Read or Write
	 * opcodes.
	 */
	switch (scsi_pktp->pkt_cdbp[0]) {
	case SPC3_CMD_READ6:
	case SPC3_CMD_READ10:
	case SPC3_CMD_READ12:
	case SPC3_CMD_WRITE6:
	case SPC3_CMD_WRITE10:
	case SPC3_CMD_WRITE12:
		return (B_TRUE);

	default:
		return (B_FALSE);
	}
}
