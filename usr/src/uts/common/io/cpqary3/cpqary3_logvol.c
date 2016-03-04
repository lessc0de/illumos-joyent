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

static void
cpqary3_write_lun_addr_phys(LUNAddr_t *lun, boolean_t masked, unsigned bus,
    unsigned target)
{
	lun->PhysDev.Mode = masked ? MASK_PERIPHERIAL_DEV_ADDR :
	    PERIPHERIAL_DEV_ADDR;

	lun->PhysDev.TargetId = target;
	lun->PhysDev.Bus = bus;

	bzero(&lun->PhysDev.Target, sizeof (lun->PhysDev.Target));
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
	CommandList_t *cl;
	cpqary3_report_logical_lun_req_t cprllr = { 0 };
	int r;

	/*
	 * Allocate the command to send to the device, including buffer space
	 * for the returned list of Logical Volumes.
	 */
	if ((cpcm = cpqary3_synccmd_alloc(cpq,
	    sizeof (cpqary3_report_logical_lun_t))) == NULL) {
		return (ENOMEM);
	}
	cprll = cpcm->cpcm_internal->cpcmi_va;
	cl = cpcm->cpcm_va_cmd;

	/*
	 * According to the CISS Specification, the Report Logical LUNs
	 * command is sent to the controller itself.  The Masked Peripheral
	 * Device addressing mode is used, with LUN of 0.
	 */
	cpqary3_write_lun_addr_phys(&cl->Header.LUN, B_TRUE, 0, 0);

	cl->Request.CDBLen = 12;
	cl->Request.Timeout = 0;
	cl->Request.Type.Type = CISS_TYPE_CMD;
	cl->Request.Type.Attribute = CISS_ATTR_ORDERED;
	cl->Request.Type.Direction = CISS_XFER_READ;

	/*
	 * The Report Logical LUNs command is essentially a vendor-specific
	 * SCSI command, which we assemble into the CDB region of the command
	 * block.
	 */
	cprllr.cprllr_opcode = CISS_SCMD_REPORT_LOGICAL_LUNS;
	cprllr.cprllr_extflag = 1;
	cprllr.cprllr_datasize = htonl(sizeof (cpqary3_report_logical_lun_t));
	bcopy(&cprllr, cl->Request.CDB, 16);

	if (cpqary3_synccmd_send(cpq, cpcm, timeout * 1000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		cpqary3_synccmd_free(cpq, cpcm);
		return (EIO);
	}

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR) {
		ErrorInfo_t *ei = cpcm->cpcm_va_err;

		dev_err(cpq->dip, CE_WARN, "RLL ERROR: %x", ei->CommandStatus);
		if (ei->CommandStatus != CISS_CMD_DATA_UNDERRUN) {
			/*
			 * XXX This is fatal, then...
			 */
			cpqary3_synccmd_free(cpq, cpcm);
			return (EIO);
		}
	}

	mutex_enter(&cpq->cpq_mutex);
	if ((cprll->cprll_extflag & 0x1) != 0) {
		r = cpqary3_read_logvols_ext(cpq, cprll);
	} else {
		r = cpqary3_read_logvols(cpq, cprll);
	}
	mutex_exit(&cpq->cpq_mutex);

	cpqary3_synccmd_free(cpq, cpcm);

	return (r);
}
