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
	 * Look for a logical volume for the SCSI address of this target.
	 */
	mutex_enter(&cpq->cpq_mutex);
	if ((cplv = cpqary3_lookup_volume_by_addr(cpq, sd)) == NULL) {
		mutex_exit(&cpq_mutex);
		kmem_free(cptg, sizeof (*cptg));
		return (DDI_FAILURE);
	}

	cptg->cptg_scsi_dev = sd;
	VERIFY(sd->sd_dev == tgt_dip); /* XXX */

	cptg->cptg_volume = cplv;
	list_insert_tail(&cplv->cplv_targets, cptg);

	/*
	 * We passed SCSI_HBA_TRAN_CLONE to scsi_hba_attach(9F), so
	 * we can stash our target-specific data structure on the
	 * (cloned) "hba_tran" without affecting the HBA-level
	 * private data pointer.
	 */
	hba_tran->tran_tgt_private = cptg;

	mutex_exit(&cpq_mutex);
	return (DDI_SUCCESS);
}

static void
cpqary3_tran_tgt_free(dev_info_t *hba_dip, dev_info_t *tgt_dip,
    scsi_hba_tran_t *hba_tran, struct scsi_device *sd)
{
	cpqary3_target_t *cptg = (cpqary3_target_t *)hba_tran->tran_tgt_private;
	cpqary3_volume_t *cplv = cptg->cptg_volume;

	/*
	 * XXX Make sure that there are no outstanding commands for this
	 * target.
	 */

	mutex_enter(&cpq->cpq_mutex);
	list_insert_remove(&cplv->cplv_targets, cptg);
	mutex_exit(&cpq_mutex);

	kmem_free(cptg, sizeof (*cptg));
}

void
cpqary3_hba_setup(cpqary3_t *cpq)
{
	scsi_hba_tran_t *tran = cpq->cpq_hba_tran;

	tran->tran_hba_private = cpq;

	tran->tran_tgt_init = cpqary3_tran_tgt_init;
	tran->tran_tgt_probe = scsi_hba_probe;
	tran->tran_tgt_free = cpqary3_tran_tgt_free;

	tran->tran_start = XXX;
	tran->tran_reset = XXX;
	tran->tran_abort = XXX;

	tran->tran_getcap = XXX;
	tran->tran_setcap = XXX;

	tran->tran_init_pkt = XXX;
	tran->tran_destroy_pkt = XXX;
	tran->tran_dmafree = XXX;
	tran->tran_sync_pkt = XXX;

	/*
	 * XXX We should set "tran_interconnect_type" appropriately.
	 * e.g. to INTERCONNECT_SAS for SAS controllers.  How to tell?
	 * Who knows.
	 */
}
