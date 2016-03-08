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

static boolean_t
cpqary3_device_is_controller(struct scsi_device *sd)
{
	return (sd->sd_address.a_target == CPQARY3_CONTROLLER_TARGET &&
	    sd->sd_address.a_lun == 0);
}

static int
cpqary3_tran_tgt_init(dev_info_t *hba_dip, dev_info_t *tgt_dip,
    scsi_hba_tran_t *hba_tran, struct scsi_device *sd)
{
	cpqary3_t *cpq = (cpqary3_t *)hba_tran->tran_hba_private;
	cpqary3_volume_t *cplv;
	cpqary3_target_t *cptg;

	/*
	 * Check to see if new logical volumes are available.
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

	mutex_enter(&cpq->cpq_mutex);

	if (cpq->cpq_status & CPQARY3_CTLR_STATUS_DETACHING) {
		/*
		 * We are detaching.  Do not accept any more requests to
		 * attach targets from the framework.
		 */
		mutex_exit(&cpq->cpq_mutex);
		kmem_free(cptg, sizeof (*cptg));
		return (DDI_FAILURE);
	}

	/*
	 * Check to see if this is the SCSI address of the pseudo target
	 * representing the Smart Array controller itself.
	 */
	if (cpqary3_device_is_controller(sd)) {
		cptg->cptg_controller_target = B_TRUE;
		goto skip_logvol;
	}

	/*
	 * Look for a logical volume for the SCSI address of this target.
	 */
	if ((cplv = cpqary3_lookup_volume_by_addr(cpq, &sd->sd_address)) ==
	    NULL) {
		mutex_exit(&cpq->cpq_mutex);
		kmem_free(cptg, sizeof (*cptg));
		return (DDI_FAILURE);
	}

	cptg->cptg_volume = cplv;
	list_insert_tail(&cplv->cplv_targets, cptg);

skip_logvol:
	/*
	 * Link this target object to the controller:
	 */
	cptg->cptg_ctlr = cpq;
	list_insert_tail(&cpq->cpq_targets, cptg);

	cptg->cptg_scsi_dev = sd;
	VERIFY(sd->sd_dev == tgt_dip);

	/*
	 * We passed SCSI_HBA_TRAN_CLONE to scsi_hba_attach(9F), so
	 * we can stash our target-specific data structure on the
	 * (cloned) "hba_tran" without affecting the private
	 * private data pointers of the HBA or of other targets.
	 */
	hba_tran->tran_tgt_private = cptg;

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

	mutex_enter(&cpq->cpq_mutex);

	/*
	 * XXX Make sure that there are no outstanding commands for this
	 * target.
	 */

	/*
	 * Remove this target from the tracking lists:
	 */
	if (!cptg->cptg_controller_target) {
		list_remove(&cplv->cplv_targets, cptg);
	}
	list_remove(&cpq->cpq_targets, cptg);

	mutex_exit(&cpq->cpq_mutex);

	kmem_free(cptg, sizeof (*cptg));
}

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
		dev_err(cpq->dip, CE_WARN, "oversize SCB: had %u, needed %u",
		    CISS_SENSEINFOBYTES, pkt->pkt_scblen);
		return (-1);
	}

	/*
	 * Allocate our command block:
	 */
	if ((cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_SCSA,
	    kmflags)) == NULL) {
		return (-1);
	}
	cpcm->cpcm_scsa = cpcms;
	cpcms->cpcms_command = cpcm;
	cpcms->cpcms_pkt = pkt;

	pkt->pkt_cdbp = &cpcm->cpcm_va_cmd->Request.CDB[0];
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
		 * The controller is, according to the CISS Specification,
		 * always LUN 0 in the peripheral device addressing mode.
		 */
		cpqary3_write_lun_addr_phys(&cpcm->cpcm_va_cmd->Header.LUN,
		    B_TRUE, 0, 0);
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
	 * If this SCSI packet has a timeout, configure an appropriate
	 * expiry time:
	 */
	if (pkt->pkt_time != 0) {
		cpcm->cpcm_expiry = gethrtime() + pkt->pkt_time * NANOSEC;
	}

	/*
	 * Submit the command to the controller.
	 */
	mutex_enter(&cpq->cpq_mutex);
	cpq->cpq_stats.cpqs_tran_starts++;
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

static int
cpqary3_tran_reset(struct scsi_address *sa, int level)
{
	scsi_hba_tran_t *tran = sa->a_hba_tran;
	cpqary3_t *cpq = (cpqary3_t *)tran->tran_hba_private;
	int r;
	cpqary3_command_t *cpcm;

	/*
	 * The framework has requested some kind of SCSI reset.  A
	 * controller-level soft reset can take a very long time -- often on
	 * the order of 30-60 seconds -- but might well be our only option if
	 * the controller is non-responsive.
	 *
	 * First, check if the controller is responding to pings.
	 */
again:
	if ((cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_INTERNAL,
	    KM_NOSLEEP)) == NULL) {
		return (0);
	}

	cpqary3_write_message_nop(cpcm, 15);

	mutex_enter(&cpq->cpq_mutex);
	cpq->cpq_stats.cpqs_tran_resets++;
	if (ddi_in_panic()) {
		goto skip_check;
	}

	if (cpq->cpq_status & CPQARY3_CTLR_STATUS_RESETTING) {
		/*
		 * The controller is already resetting.  Wait for that
		 * to finish.
		 */
		while (cpq->cpq_status & CPQARY3_CTLR_STATUS_RESETTING) {
			cv_wait(&cpq->cpq_cv_finishq, &cpq->cpq_mutex);
		}
	}

skip_check:
	/*
	 * Submit our ping to the controller.
	 */
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_POLLED;
	cpcm->cpcm_expiry = gethrtime() + 15 * NANOSEC;
	if (cpqary3_submit(cpq, cpcm) != 0) {
		mutex_exit(&cpq->cpq_mutex);
		cpqary3_command_free(cpcm);
		return (0);
	}

	if ((r = cpqary3_poll_for(cpq, cpcm)) != 0) {
		VERIFY(r == ETIMEDOUT);
		VERIFY0(cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLL_COMPLETE);

		/*
		 * The ping command timed out.  Abandon it now.
		 */
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ABANDONED;
		cpcm->cpcm_status &= ~CPQARY3_CMD_STATUS_POLLED;

	} else if ((cpcm->cpcm_status & CPQARY3_CMD_STATUS_RESET_SENT) ||
	    (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR)) {
		/*
		 * The command completed in error, or a controller reset
		 * was sent while we were trying to ping.
		 */
		mutex_exit(&cpq->cpq_mutex);
		cpqary3_command_free(cpcm);
		mutex_enter(&cpq->cpq_mutex);

	} else {
		VERIFY(cpcm->cpcm_status & CPQARY3_CMD_STATUS_COMPLETE);

		/*
		 * The controller is responsive, and a full soft reset would be
		 * extremely disruptive to the system.  Given our spotty
		 * support for some SCSI commands (which can upset the target
		 * drivers) and the historically lax behaviour of the "cpqary3"
		 * driver, we grit our teeth and pretend we were able to
		 * perform a reset.
		 */
		mutex_exit(&cpq->cpq_mutex);
		cpqary3_command_free(cpcm);
		return (1);
	}

	/*
	 * If a reset has been initiated in the last 90 seconds, try
	 * another ping.
	 */
	if (gethrtime() < cpq->cpq_last_reset_start + 90 * NANOSEC) {
		dev_err(cpq->dip, CE_WARN, "controller ping failed, but was "
		    "recently reset; retrying ping");
		mutex_exit(&cpq->cpq_mutex);

		/*
		 * Sleep for a second first.
		 */
		if (ddi_in_panic()) {
			drv_usecwait(1 * MICROSEC);
		} else {
			delay(drv_usectohz(1 * MICROSEC));
		}
		goto again;
	}

	dev_err(cpq->dip, CE_WARN, "controller ping failed; "
	    "resetting controller");
	if (cpqary3_ctlr_reset(cpq) != 0) {
		dev_err(cpq->dip, CE_WARN, "controller reset failure");
		return (0);
	}

	return (1);
}

static int
cpqary3_tran_abort(struct scsi_address *sa, struct scsi_pkt *pkt)
{
	scsi_hba_tran_t *tran = sa->a_hba_tran;
	cpqary3_t *cpq = (cpqary3_t *)tran->tran_hba_private;
	cpqary3_command_t *cpcm = NULL;
	cpqary3_command_t *abort_cpcm;

	if ((abort_cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_INTERNAL,
	    KM_NOSLEEP)) == NULL) {
		/*
		 * No resources available to send an abort message.
		 */
		return (0);
	}

	mutex_enter(&cpq->cpq_mutex);
	cpq->cpq_stats.cpqs_tran_aborts++;
	if (pkt != NULL) {
		/*
		 * The framework wants us to abort a specific SCSI packet.
		 */
		cpqary3_command_scsa_t *cpcms = (cpqary3_command_scsa_t *)
		    pkt->pkt_ha_private;
		cpcm = cpcms->cpcms_command;

		if (!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_INFLIGHT)) {
			/*
			 * This message is not currently inflight, so we
			 * cannot abort it.
			 */
			goto fail;
		}

		if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ABORT_SENT) {
			/*
			 * An abort message for this command has already been
			 * sent to the controller.  Return failure.
			 */
			goto fail;
		}

		cpqary3_write_message_abort_one(abort_cpcm, cpcm->cpcm_tag);
	} else {
		/*
		 * The framework wants us to abort every inflight command
		 * for the target with this address.
		 */
		cpqary3_target_t *cptg = (cpqary3_target_t *)tran->
		    tran_tgt_private;

		if (cptg->cptg_volume == NULL) {
			/*
			 * We currently do not support sending an abort
			 * to anything but a Logical Volume.
			 */
			goto fail;
		}

		cpqary3_write_message_abort_all(abort_cpcm,
		    &cptg->cptg_volume->cplv_addr);
	}

	/*
	 * Submit the abort message to the controller.
	 */
	abort_cpcm->cpcm_status |= CPQARY3_CMD_STATUS_POLLED;
	if (cpqary3_submit(cpq, abort_cpcm) != 0) {
		goto fail;
	}

	if (pkt != NULL) {
		/*
		 * Record some debugging information about the abort we
		 * sent:
		 */
		cpcm->cpcm_abort_time = gethrtime();
		cpcm->cpcm_abort_tag = abort_cpcm->cpcm_tag;

		/*
		 * Mark the command as aborted so that we do not send
		 * a second abort message:
		 */
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ABORT_SENT;
	}

	/*
	 * Poll for completion of the abort message.  Note that this function
	 * only fails if we set a timeout on the command, which we have not
	 * done.
	 */
	VERIFY0(cpqary3_poll_for(cpq, abort_cpcm));

	if ((abort_cpcm->cpcm_status & CPQARY3_CMD_STATUS_RESET_SENT) ||
	    (abort_cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR)) {
		/*
		 * Either the controller was reset or the abort command
		 * failed.
		 */
		goto fail;
	}

	/*
	 * The command was successfully aborted.
	 */
	mutex_exit(&cpq->cpq_mutex);
	cpqary3_command_free(abort_cpcm);
	return (1);

fail:
	mutex_exit(&cpq->cpq_mutex);
	cpqary3_command_free(abort_cpcm);
	return (0);
}

int
cpqary3_hba_setup(cpqary3_t *cpq)
{
	scsi_hba_tran_t *tran;

	if ((tran = scsi_hba_tran_alloc(cpq->dip, SCSI_HBA_CANSLEEP)) ==
	    NULL) {
		dev_err(cpq->dip, CE_WARN, "could not allocate SCSA "
		    "resources");
		return (DDI_FAILURE);
	}

	cpq->cpq_hba_tran = tran;
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

	tran->tran_setup_pkt = cpqary3_tran_setup_pkt;
	tran->tran_teardown_pkt = cpqary3_tran_teardown_pkt;
	tran->tran_hba_len = sizeof (cpqary3_command_scsa_t);

#if 0
	/*
	 * XXX We should set "tran_interconnect_type" appropriately.
	 * e.g. to INTERCONNECT_SAS for SAS controllers.  How to tell?
	 * Who knows.
	 */
	tran->tran_interconnect_type = INTERCONNECT_SAS;
#endif

	if (scsi_hba_attach_setup(cpq->dip, &cpq->cpq_dma_attr, tran,
	    SCSI_HBA_TRAN_CLONE) != DDI_SUCCESS) {
		dev_err(cpq->dip, CE_WARN, "could not attach to SCSA "
		    "framework");
		scsi_hba_tran_free(tran);
		return (DDI_FAILURE);
	}

	cpq->cpq_init_level |= CPQARY3_INITLEVEL_SCSA;
	return (DDI_SUCCESS);
}

void
cpqary3_hba_teardown(cpqary3_t *cpq)
{
	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_SCSA) {
		VERIFY(scsi_hba_detach(cpq->dip) != DDI_FAILURE);
		scsi_hba_tran_free(cpq->cpq_hba_tran);
		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_SCSA;
	}
}
