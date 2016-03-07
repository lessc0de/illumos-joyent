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

cpqary3_volume_t *
cpqary3_lookup_volume_by_id(cpqary3_t *cpq, unsigned id)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	for (cpqary3_volume_t *cplv = list_head(&cpq->cpq_volumes);
	    cplv != NULL; cplv = list_next(&cpq->cpq_volumes, cplv)) {
		if (cplv->cplv_addr.VolId == id) {
			return (cplv);
		}
	}

	return (NULL);
}

cpqary3_volume_t *
cpqary3_lookup_volume_by_addr(cpqary3_t *cpq, struct scsi_address *sa)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	if (sa->a_lun != 0) {
		return (NULL);
	}

	return (cpqary3_lookup_volume_by_id(cpq, sa->a_target));
}

static int
cpqary3_read_logvols(cpqary3_t *cpq, cpqary3_report_logical_lun_t *cprll)
{
	cpqary3_report_logical_lun_ent_t *ents = cprll->cprll_data.ents;
	uint32_t count = ntohl(cprll->cprll_datasize) /
	    sizeof (cpqary3_report_logical_lun_ent_t);

	if (count > MAX_LOGDRV) {
		count = MAX_LOGDRV;
	}

	for (unsigned i = 0; i < count; i++) {
		cpqary3_volume_t *cplv;

		if ((cplv = cpqary3_lookup_volume_by_id(cpq,
		    ents[i].cprle_addr.VolId)) != NULL) {
			continue;
		}

		dev_err(cpq->dip, CE_WARN, "NEW LOGVOL[%u]: mode %x "
		    "volid %x attr %x", i,
		    ents[i].cprle_addr.Mode,
		    ents[i].cprle_addr.VolId,
		    *((uint32_t *)ents[i].cprle_addr.reserved));

		/*
		 * This is a new Logical Volume, so add it the the list.
		 */
		if ((cplv = kmem_zalloc(sizeof (*cplv), KM_NOSLEEP)) ==
		    NULL) {
			return (ENOMEM);
		}

		cplv->cplv_addr = ents[i].cprle_addr;

		list_create(&cplv->cplv_targets,
		    sizeof (cpqary3_target_t),
		    offsetof(cpqary3_target_t, cptg_link_volume));

		cplv->cplv_ctlr = cpq;
		list_insert_tail(&cpq->cpq_volumes, cplv);
	}

	return (0);
}

static int
cpqary3_read_logvols_ext(cpqary3_t *cpq, cpqary3_report_logical_lun_t *cprll)
{
	cpqary3_report_logical_lun_extent_t *extents =
	    cprll->cprll_data.extents;
	uint32_t count = ntohl(cprll->cprll_datasize) /
	    sizeof (cpqary3_report_logical_lun_extent_t);

	if (count > MAX_LOGDRV) {
		count = MAX_LOGDRV;
	}

	for (unsigned i = 0; i < count; i++) {
		cpqary3_volume_t *cplv;

		if ((cplv = cpqary3_lookup_volume_by_id(cpq,
		    extents[i].cprle_addr.VolId)) != NULL) {
			/*
			 * XXX compare previous WWN with current WWN...
			 */
			continue;
		}

		dev_err(cpq->dip, CE_WARN, "NEW EXT LOGVOL[%u]: mode %x "
		    "volid %x attr %x", i,
		    extents[i].cprle_addr.Mode,
		    extents[i].cprle_addr.VolId,
		    *((uint32_t *)extents[i].cprle_addr.reserved));
		dev_err(cpq->dip, CE_WARN, "-- id %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x",
		    (uint32_t)extents[i].cprle_wwn[0],
		    (uint32_t)extents[i].cprle_wwn[1],
		    (uint32_t)extents[i].cprle_wwn[2],
		    (uint32_t)extents[i].cprle_wwn[3],
		    (uint32_t)extents[i].cprle_wwn[4],
		    (uint32_t)extents[i].cprle_wwn[5],
		    (uint32_t)extents[i].cprle_wwn[6],
		    (uint32_t)extents[i].cprle_wwn[7],
		    (uint32_t)extents[i].cprle_wwn[8],
		    (uint32_t)extents[i].cprle_wwn[9],
		    (uint32_t)extents[i].cprle_wwn[10],
		    (uint32_t)extents[i].cprle_wwn[11],
		    (uint32_t)extents[i].cprle_wwn[12],
		    (uint32_t)extents[i].cprle_wwn[13],
		    (uint32_t)extents[i].cprle_wwn[14],
		    (uint32_t)extents[i].cprle_wwn[15]);

		/*
		 * This is a new Logical Volume, so add it the the list.
		 */
		if ((cplv = kmem_zalloc(sizeof (*cplv), KM_NOSLEEP)) ==
		    NULL) {
			return (ENOMEM);
		}

		cplv->cplv_addr = extents[i].cprle_addr;

		bcopy(extents[i].cprle_wwn, cplv->cplv_wwn, 16);
		cplv->cplv_flags |= CPQARY3_VOL_FLAG_WWN;

		list_create(&cplv->cplv_targets,
		    sizeof (cpqary3_target_t),
		    offsetof(cpqary3_target_t, cptg_link_volume));

		cplv->cplv_ctlr = cpq;
		list_insert_tail(&cpq->cpq_volumes, cplv);
	}

	return (0);
}

/*
 * Discover the currently visible set of Logical Volumes exposed by the
 * controller.
 */
int
cpqary3_discover_logical_volumes(cpqary3_t *cpq, int timeout)
{
	cpqary3_command_t *cpcm;
	cpqary3_report_logical_lun_t *cprll;
	cpqary3_report_logical_lun_req_t cprllr = { 0 };
	int r;

	if (!ddi_in_panic()) {
		mutex_enter(&cpq->cpq_mutex);
		while (cpq->cpq_status & CPQARY3_CTLR_STATUS_DISCOVERY) {
			/*
			 * A discovery is already occuring.  Wait for
			 * completion.
			 */
			cv_wait(&cpq->cpq_cv_finishq, &cpq->cpq_mutex);
		}

		if (gethrtime() < cpq->cpq_last_discovery + 5 * NANOSEC) {
			/*
			 * A discovery completed successfully within the
			 * last five seconds.  Just use the existing data.
			 */
			mutex_exit(&cpq->cpq_mutex);
			return (0);
		}

		cpq->cpq_status |= CPQARY3_CTLR_STATUS_DISCOVERY;
		mutex_exit(&cpq->cpq_mutex);
	}

	/*
	 * Allocate the command to send to the device, including buffer space
	 * for the returned list of Logical Volumes.
	 */
	if ((cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_INTERNAL,
	    KM_NOSLEEP)) == NULL ||
	    cpqary3_command_attach_internal(cpq, cpcm,
	    sizeof (cpqary3_report_logical_lun_t), KM_NOSLEEP) != 0) {
		r = ENOMEM;
		mutex_enter(&cpq->cpq_mutex);
		goto out;
	}

	cprll = cpcm->cpcm_internal->cpcmi_va;

	/*
	 * According to the CISS Specification, the Report Logical LUNs
	 * command is sent to the controller itself.  The Masked Peripheral
	 * Device addressing mode is used, with LUN of 0.
	 */
	cpqary3_write_lun_addr_phys(&cpcm->cpcm_va_cmd->Header.LUN, B_TRUE,
	    0, 0);

	cpcm->cpcm_va_cmd->Request.CDBLen = 12;
	cpcm->cpcm_va_cmd->Request.Timeout = timeout;
	cpcm->cpcm_va_cmd->Request.Type.Type = CISS_TYPE_CMD;
	cpcm->cpcm_va_cmd->Request.Type.Attribute = CISS_ATTR_ORDERED;
	cpcm->cpcm_va_cmd->Request.Type.Direction = CISS_XFER_READ;

	/*
	 * The Report Logical LUNs command is essentially a vendor-specific
	 * SCSI command, which we assemble into the CDB region of the command
	 * block.
	 */
	cprllr.cprllr_opcode = CISS_SCMD_REPORT_LOGICAL_LUNS;
	cprllr.cprllr_extflag = 1;
	cprllr.cprllr_datasize = htonl(sizeof (cpqary3_report_logical_lun_t));
	bcopy(&cprllr, &cpcm->cpcm_va_cmd->Request.CDB[0], 16);

	mutex_enter(&cpq->cpq_mutex);

	/*
	 * Send the command to the device.
	 */
	cpcm->cpcm_status |= CPQARY3_CMD_STATUS_POLLED;
	if (cpqary3_submit(cpq, cpcm) != 0) {
		r = EIO;
		goto out;
	}

	/*
	 * Poll for completion.
	 */
	cpcm->cpcm_expiry = gethrtime() + timeout * NANOSEC;
	if ((r = cpqary3_poll_for(cpq, cpcm)) != 0) {
		VERIFY(r == ETIMEDOUT);
		VERIFY0(cpcm->cpcm_status & CPQARY3_CMD_STATUS_POLL_COMPLETE);

		/*
		 * The command timed out; abandon it now.  Remove the POLLED
		 * flag so that the periodic routine will send an abort to
		 * clean it up next time around.
		 */
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ABANDONED;
		cpcm->cpcm_status &= ~CPQARY3_CMD_STATUS_POLLED;
		cpcm = NULL;
		goto out;
	}

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_RESET_SENT) {
		/*
		 * The controller was reset while we were trying to discover
		 * logical volumes.  Report failure.
		 */
		r = EIO;
		goto out;
	}

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR) {
		ErrorInfo_t *ei = cpcm->cpcm_va_err;

		if (ei->CommandStatus != CISS_CMD_DATA_UNDERRUN) {
			dev_err(cpq->dip, CE_WARN, "logical volume discovery"
			    "error: status 0x%x", ei->CommandStatus);
			r = EIO;
			goto out;
		}
	}

	if ((cprll->cprll_extflag & 0x1) != 0) {
		r = cpqary3_read_logvols_ext(cpq, cprll);
	} else {
		r = cpqary3_read_logvols(cpq, cprll);
	}

	if (r == 0) {
		/*
		 * Update the time of the last successful Logical Volume
		 * discovery:
		 */
		cpq->cpq_last_discovery = gethrtime();
	}

out:
	cpq->cpq_status &= ~CPQARY3_CTLR_STATUS_DISCOVERY;
	cv_broadcast(&cpq->cpq_cv_finishq);
	mutex_exit(&cpq->cpq_mutex);

	if (cpcm != NULL) {
		cpqary3_command_free(cpcm);
	}
	return (r);
}
