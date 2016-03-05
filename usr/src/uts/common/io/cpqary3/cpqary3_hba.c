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

static int
cpqary3_tran_tgt_init(dev_info_t *hba_dip, dev_info_t *tgt_dip,
    scsi_hba_tran_t *hba_tran, struct scsi_device *sd)
{
	cpqary3_t *cpq = (cpqary3_t *)hba_tran->tran_hba_private;
	cpqary3_volume_t *cplv;
	cpqary3_target_t *cptg;

	/*
	 * XXX Check to see if new logical volumes are available.
	 */
	if (cpqary3_discover_logical_volumes(cpq, 15) != 0) {
		dev_err(cpq->dip, CE_WARN, "discover logical volumes failure");
		return (DDI_FAILURE);
	}

	if ((cptg = kmem_zalloc(sizeof (*cptg), KM_NOSLEEP)) == NULL) {
		dev_err(cpq->dip, CE_WARN, "could not allocate target object "
		    "due to memory exhaustion");
		return (DDI_FAILURE);
	}

	/*
	 * XXX Expose the controller as a target... SIGH
	 */
	if (sd->sd_address.a_target == 7 && sd->sd_address.a_lun == 0) {
		cptg->cptg_controller_target = B_TRUE;
		mutex_enter(&cpq->cpq_mutex);
		list_insert_tail(&cpq->cpq_targets, cptg);
		goto skip;
	}

	/*
	 * Look for a logical volume for the SCSI address of this target.
	 */
	mutex_enter(&cpq->cpq_mutex);
	if ((cplv = cpqary3_lookup_volume_by_addr(cpq, &sd->sd_address)) ==
	    NULL) {
		mutex_exit(&cpq->cpq_mutex);
		kmem_free(cptg, sizeof (*cptg));
		return (DDI_FAILURE);
	}

	cptg->cptg_volume = cplv;
	list_insert_tail(&cplv->cplv_targets, cptg);

skip:
	cptg->cptg_scsi_dev = sd;
	VERIFY(sd->sd_dev == tgt_dip); /* XXX */

	/*
	 * We passed SCSI_HBA_TRAN_CLONE to scsi_hba_attach(9F), so
	 * we can stash our target-specific data structure on the
	 * (cloned) "hba_tran" without affecting the HBA-level
	 * private data pointer.
	 */
	hba_tran->tran_tgt_private = cptg;

	/*
	 * XXX
	 * Note that we used to turn on these caps:
	 * 	CPQARY3_CAP_DISCON_ENABLED
	 * 	CPQARY3_CAP_SYNC_ENABLED
	 * 	CPQARY3_CAP_WIDE_XFER_ENABLED
	 * 	CPQARY3_CAP_ARQ_ENABLED
	 */

	mutex_exit(&cpq->cpq_mutex);
	return (DDI_SUCCESS);
}

static void
cpqary3_tran_tgt_free(dev_info_t *hba_dip, dev_info_t *tgt_dip,
    scsi_hba_tran_t *hba_tran, struct scsi_device *sd)
{
	cpqary3_t *cpq = (cpqary3_t *)hba_tran->tran_hba_private;
	cpqary3_target_t *cptg = (cpqary3_target_t *)hba_tran->tran_tgt_private;
	cpqary3_volume_t *cplv = cptg->cptg_volume;

	VERIFY(cptg->cptg_scsi_dev == sd);

	/*
	 * XXX Make sure that there are no outstanding commands for this
	 * target.
	 */

	mutex_enter(&cpq->cpq_mutex);
	if (cptg->cptg_controller_target) {
		/*
		 * XXX
		 */
		list_remove(&cpq->cpq_targets, cptg);
	} else {
		list_remove(&cplv->cplv_targets, cptg);
	}
	mutex_exit(&cpq->cpq_mutex);

	kmem_free(cptg, sizeof (*cptg));
}

#if 0
static int
cpqary3_tran_pkt_constructor(struct scsi_pkt *pkt, scsi_hba_tran_t *tran,
    int kmflags)
{
	cpqary3_t *cpq = tran->tran_hba_private;
	cpqary3_command_scsa_t *cpcms =
	    (cpqary3_command_scsa_t *)pkt->pkt_hba_private;
}
#endif

static int
cpqary3_tran_setup_pkt(struct scsi_pkt *pkt, int (*callback)(caddr_t),
    caddr_t arg)
{
	scsi_hba_tran_t *tran = pkt->pkt_address.a_hba_tran;
	cpqary3_t *cpq = (cpqary3_t *)tran->tran_hba_private;
	cpqary3_target_t *cptg = (cpqary3_target_t *)tran->tran_tgt_private;
	cpqary3_command_scsa_t *cpcms = (cpqary3_command_scsa_t *)
	    pkt->pkt_ha_private;
	cpqary3_command_t *cpcm;
	int kmflags = callback == SLEEP_FUNC ? KM_SLEEP : KM_NOSLEEP;

	/*
	 * The SCSI framework has allocated a packet, and our private
	 * per-packet object.
	 *
	 * We choose not to have the framework pre-allocate memory
	 * for the command (CDB) and status (SCB) blocks.  Instead, we
	 * will make available the memory in the command block itself.
	 */

	/*
	 * Check that we have enough space in the command object for the
	 * request from the target driver:
	 */
	if (pkt->pkt_cdblen > 16) {
		/*
		 * The CDB member of the Request Block of a controller
		 * command is fixed at 16 bytes.
		 */
		dev_err(cpq->dip, CE_WARN, "oversize CDB: had %u, needed %u",
		    16, pkt->pkt_cdblen);
		return (-1);
	}
	if (pkt->pkt_scblen > CISS_SENSEINFOBYTES) {
		/*
		 * The SCB is the "SenseInfo[]" member of the "ErrorInfo_t".
		 * This is statically allocated; make sure it is big enough.
		 */
		dev_err(cpq->dip, CE_WARN, "oversize SENSE BYTES: had %u, "
		    "needed %u", CISS_SENSEINFOBYTES, pkt->pkt_scblen);
		return (-1);
	}

	/*
	 * Allocate our command block:
	 */
	if ((cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_OS, kmflags)) ==
	    NULL) {
		return (-1);
	}
	cpcm->cpcm_scsa = cpcms;
	cpcms->cpcms_command = cpcm;
	cpcms->cpcms_pkt = pkt;

	pkt->pkt_cdbp = cpcm->cpcm_va_cmd->Request.CDB;
	cpcm->cpcm_va_cmd->Request.CDBLen = pkt->pkt_cdblen;

	pkt->pkt_scbp = (uchar_t *)&cpcm->cpcm_va_err->SenseInfo;
	/*
	 * XXX we should enable/disable AUTOMATIC REQUEST SENSE?
	 * (see: tran_setup_pkt(9E))
	 */

	cpcm->cpcm_target = cptg;
	/*
	 * XXX We should link our command into the target_t via some list
	 */

	return (0);
}

static void
cpqary3_tran_teardown_pkt(struct scsi_pkt *pkt)
{
	scsi_hba_tran_t *tran = pkt->pkt_address.a_hba_tran;
	cpqary3_t *cpq = (cpqary3_t *)tran->tran_hba_private;
	cpqary3_command_scsa_t *cpcms = (cpqary3_command_scsa_t *)
	    pkt->pkt_ha_private;
	cpqary3_command_t *cpcm = cpcms->cpcms_command;

	/*
	 * XXX We should remove ourselves from the target_t list...
	 */

	cpqary3_command_free(cpcm);

	pkt->pkt_cdbp = NULL;
	pkt->pkt_scbp = NULL;
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
cpqary3_tran_start(struct scsi_address *sa, struct scsi_pkt *pkt)
{
	scsi_hba_tran_t *tran = pkt->pkt_address.a_hba_tran;
	cpqary3_t *cpq = (cpqary3_t *)tran->tran_hba_private;
	cpqary3_command_scsa_t *cpcms = (cpqary3_command_scsa_t *)
	    pkt->pkt_ha_private;
	cpqary3_command_t *cpcm = cpcms->cpcms_command;
	int r;

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_TRAN_START) {
		/*
		 * This is a retry of a command that has already been
		 * used once.  Assign it a new tag number.
		 */
		cpqary3_command_reuse(cpcm);
	}
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_TRAN_START;

	/*
	 * The sophisticated firmware in this controller cannot possibly bear
	 * the following SCSI commands.  It appears to return a response with
	 * the status STATUS_ACA_ACTIVE (0x30), which is not something we
	 * expect.  Instead, fake up a failure response.
	 */
	switch (pkt->pkt_cdbp[0]) {
	case SCMD_FORMAT:
	case SCMD_LOG_SENSE_G1:
	case SCMD_MODE_SELECT:
	case SCMD_PERSISTENT_RESERVE_IN:
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_TRAN_IGNORED;
		dev_err(cpq->dip, CE_WARN, "ignored SCSI cmd %02x",
		    (unsigned)pkt->pkt_cdbp[0]); /* XXX */
		cpqary3_set_arq_data(pkt, KEY_ILLEGAL_REQUEST);
		pkt->pkt_reason = CMD_BADMSG;
		pkt->pkt_state |= STATE_GOT_BUS | STATE_GOT_TARGET |
		    STATE_SENT_CMD | STATE_XFERRED_DATA;
		scsi_hba_pkt_comp(pkt);
		return (TRAN_ACCEPT);
	}

	if (pkt->pkt_flags & FLAG_NOINTR) {
		/*
		 * We must sleep and wait for the completion of this command.
		 */
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_POLLED;
	}

	/*
	 * Because we provide a tran_setup_pkt(9E) entrypoint, we must now
	 * set up the Scatter/Gather List in the Command to reflect any
	 * DMA resources passed to us by the framework.
	 */
	if (pkt->pkt_numcookies > cpq->cpq_sg_cnt) {
		/*
		 * More DMA cookies than we are prepared to handle.
		 */
		dev_err(cpq->dip, CE_WARN, "too many DMA cookies (got %u;"
		    " expected %u)", pkt->pkt_numcookies, cpq->cpq_sg_cnt);
		return (TRAN_BADPKT);
	}
	cpcm->cpcm_va_cmd->Header.SGList = pkt->pkt_numcookies;
	cpcm->cpcm_va_cmd->Header.SGTotal = pkt->pkt_numcookies;
	for (unsigned i = 0; i < pkt->pkt_numcookies; i++) {
		cpcm->cpcm_va_cmd->SG[i].Addr =
		    pkt->pkt_cookies[i].dmac_laddress;
		cpcm->cpcm_va_cmd->SG[i].Len =
		    pkt->pkt_cookies[i].dmac_size;
	}

	if (cpcm->cpcm_target->cptg_controller_target) {
		/*
		 * XXX use cpqary3_write_lun_addr_phys()
		 */
		LUNAddr_t *lun = &cpcm->cpcm_va_cmd->Header.LUN;

		lun->PhysDev.Mode = MASK_PERIPHERIAL_DEV_ADDR;
		lun->PhysDev.TargetId = 0;
		lun->PhysDev.Bus = 0;

		bzero(&lun->PhysDev.Target, sizeof (lun->PhysDev.Target));
	} else {
		/*
		 * Copy logical volume address from the target object:
		 */
		cpcm->cpcm_va_cmd->Header.LUN.LogDev = cpcm->cpcm_target->
		    cptg_volume->cplv_addr;
	}

	/*
	 * Initialise the command block.
	 */
	cpcm->cpcm_va_cmd->Request.CDBLen = pkt->pkt_cdblen;
	cpcm->cpcm_va_cmd->Request.Type.Type = CISS_TYPE_CMD;
	cpcm->cpcm_va_cmd->Request.Type.Attribute = CISS_ATTR_ORDERED;
	cpcm->cpcm_va_cmd->Request.Timeout = pkt->pkt_time;
	if (pkt->pkt_numcookies > 0) {
		/*
		 * There are DMA resources; set the transfer direction
		 * appropriately:
		 */
		if (pkt->pkt_dma_flags & DDI_DMA_READ) {
			cpcm->cpcm_va_cmd->Request.Type.Direction =
			    CISS_XFER_READ;
		} else if (pkt->pkt_dma_flags & DDI_DMA_WRITE) {
			cpcm->cpcm_va_cmd->Request.Type.Direction =
			    CISS_XFER_WRITE;
		} else {
			cpcm->cpcm_va_cmd->Request.Type.Direction =
			    CISS_XFER_NONE;
		}
	} else {
		/*
		 * No DMA resources means no transfer.
		 */
		cpcm->cpcm_va_cmd->Request.Type.Direction = CISS_XFER_NONE;
	}

	/*
	 * Initialise the SCSI packet as described in tran_start(9E).  We will
	 * progressively update these fields as the command moves through the
	 * submission and completion states.
	 */
	pkt->pkt_resid = 0;
	pkt->pkt_reason = CMD_CMPLT;
	pkt->pkt_statistics = 0;
	pkt->pkt_state = 0;

	/*
	 * XXX Synchronise DMA for device to see changes to the command
	 * block...? XXX this is being done in cpqary3_submit() now ...
	 */
#if 0
	if (ddi_dma_sync(cpcm->cpcm_phyctg->cpqary3_dmahandle, 0,
	    sizeof (CommandList_t), DDI_DMA_SYNC_FORDEV) != DDI_SUCCESS) {
		dev_err(cpq->cpq, CE_WARN, "DMA sync failure");
		return (TRAN_FATAL_ERROR);
	}
#endif

	/*
	 * XXX I don't _think_ we need to synchronise the DMA stuff we
	 * were _passed_ (in the SCSI packet).  Need to make sure, though.
	 * I think the documentation could be clearer about this...
	 */

	/*
	 * Submit the command to the controller!
	 */
	mutex_enter(&cpq->cpq_mutex);
	if ((r = cpqary3_submit(cpq, cpcm)) != 0) {
		mutex_exit(&cpq->cpq_mutex);

		dev_err(cpq->dip, CE_WARN, "cpqary3_submit failed %d", r);

		/*
		 * Inform the SCSI framework that we could not submit
		 * the command.
		 */
		return (r == EAGAIN ? TRAN_BUSY : TRAN_FATAL_ERROR);
	}

	/*
	 * Update the SCSI packet to reflect submission of the command.
	 */
	pkt->pkt_state |= STATE_GOT_BUS | STATE_GOT_TARGET | STATE_SENT_CMD;

	if (pkt->pkt_flags & FLAG_NOINTR) {
		/*
		 * Poll the controller for completion of the command we
		 * submitted.  Once this routine has returned, the completion
		 * callback will have been fired with either an active response
		 * (success or error) or a timeout.  The command is freed by
		 * the completion callback, so it may not be referenced again
		 * after this call returns.
		 */
		cpqary3_poll_for(cpq, cpcm);
	}

	mutex_exit(&cpq->cpq_mutex);
	return (TRAN_ACCEPT);
}


#if 0
static struct scsi_pkt *
cpqary3_tran_init_pkt(struct scsi_address *sa, struct scsi_pkt *pkt,
    struct buf *bp, int cmdlen, int statuslen, int tgtlen,
    int flags, int (*callback)(), caddr_t arg)
{
	cpqary3_t *cpq = (cpqary3_t *)sa->a_hba_tran->tran_hba_private;
	cpqary3_target_t *cptg = (cpqary3_target_t *)sa->a_hba_tran->
	    tran_tgt_private;
	boolean_t allocated_packet = B_FALSE;
	cpqary3_command_scsa_t *cpcms;

	if (pkt == NULL) {
		/*
		 * The framework requires we allocate a new packet
		 * structure via scsi_hba_pkt_alloc(9F).
		 */
		if ((pkt = scsi_hba_pkt_alloc(cpq->dip, sa, cmdlen, statuslen,
		    tgtlen, sizeof (*cpcms), callback, arg)) == NULL) {
			return (NULL);
		}
		allocated_packet = B_TRUE;
	}
	cpcms = (cpqary3_command_scsa_t *)pkt->pkt_ha_private;

	if (allocated_packet) {
		/*
		 * Our private SCSI packet object was just allocated by
		 * scsi_hba_pkt_alloc(9F).  Initialise it:
		 */
		cpcms->cpcms_pkt = pkt;
	}

	if (bp != NULL && allocated_packet) {
		/*
		 * This is a new packet with an associated buffer.  The
		 * framework requires us to allocate appropriate DMA
		 * resources.
		 */
		if (cpqary3_dma_alloc(cpq, pkt, bp, flags, callback) !=
		    DDI_SUCCESS) {
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
#endif

static int
cpqary3_tran_reset(struct scsi_address *sa, int level)
{
	/*
	 * We currently have no earthly idea how to reset the controller.
	 * Signal our universal, abject failure to the SCSI framework.
	 */
	return (0);
}

static int
cpqary3_tran_abort(struct scsi_address *sa, struct scsi_pkt *pkt)
{
	scsi_hba_tran_t *tran = sa->a_hba_tran;
	cpqary3_t *cpq = (cpqary3_t *)tran->tran_hba_private;
	cpqary3_target_t *cptg = (cpqary3_target_t *)tran->tran_tgt_private;
	cpqary3_command_t *cpcm = NULL;
	int r;

	if (pkt != NULL) {
		cpqary3_command_scsa_t *cpcms = (cpqary3_command_scsa_t *)
		    pkt->pkt_ha_private;
		cpcm = cpcms->cpcms_command;
	}

	mutex_enter(&cpq->cpq_mutex);
	/*
	 * XXX
	 */
	r = cpqary3_send_abortcmd(cpq, cptg, cpcm);
	mutex_exit(&cpq->cpq_mutex);

	return (r == 0 ? 1 : 0);
}

void
cpqary3_hba_setup(cpqary3_t *cpq)
{
	scsi_hba_tran_t *tran = cpq->cpq_hba_tran;

	tran->tran_hba_private = cpq;

	tran->tran_tgt_init = cpqary3_tran_tgt_init;
	tran->tran_tgt_probe = scsi_hba_probe;
	tran->tran_tgt_free = cpqary3_tran_tgt_free;

	tran->tran_start = cpqary3_tran_start;
	tran->tran_reset = cpqary3_tran_reset;
	tran->tran_abort = cpqary3_tran_abort;

	/*
	 * XXX these are still the old ones
	 */
	tran->tran_getcap = cpqary3_getcap;
	tran->tran_setcap = cpqary3_setcap;

#if 0
	/*
	 * XXX Old style:
	 */
	tran->tran_init_pkt = XXX;
	tran->tran_destroy_pkt = XXX;
	tran->tran_dmafree = XXX;
	tran->tran_sync_pkt = XXX;
#else
	/*
	 * XXX New style:
	 */
	tran->tran_setup_pkt = cpqary3_tran_setup_pkt;
	tran->tran_teardown_pkt = cpqary3_tran_teardown_pkt;
	tran->tran_hba_len = sizeof (cpqary3_command_scsa_t);
#endif

	/*
	 * XXX We should set "tran_interconnect_type" appropriately.
	 * e.g. to INTERCONNECT_SAS for SAS controllers.  How to tell?
	 * Who knows.
	 */
}
