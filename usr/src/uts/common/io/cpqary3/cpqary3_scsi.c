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

#if 0
static uint8_t cpqary3_probe4LVs(cpqary3_t *);
static uint8_t cpqary3_probe4Tapes(cpqary3_t *);


/*
 * Function	:	cpqary3_probe4targets
 * Description	: 	This routine detects all existing logical drives
 *			and updates per target structure.
 * Called By	:  	cpqary3_tgt_init()
 * Parameters	: 	per-controller
 * Calls	:  	cpqary3_probe4LVs(), cpqary3_probe4Tapes()
 * Return Values: 	SUCCESS/ FAILURE
 *			[Shall fail only if Memory Constraints exist, the
 *			controller is defective/does not respond]
 */
uint8_t
cpqary3_probe4targets(cpqary3_t *cpqary3p)
{
	uint8_t rv;

	if ((rv = cpqary3_probe4LVs(cpqary3p)) != CPQARY3_SUCCESS) {
		return (rv);
	}

	if ((rv = cpqary3_probe4Tapes(cpqary3p)) != CPQARY3_SUCCESS) {
		return (rv);
	}

	return (CPQARY3_SUCCESS);
}
#endif

/*
 * Function	: 	cpqary3_send_abortcmd
 * Description	: 	Sends the Abort command to abort
 *			a set of cmds(on a target) or a cmdlist.
 * Called By	: 	cpqary3_abort
 * Parameters	: 	per controller, target_id, cmdlist to abort
 * Calls	:  	cpqary3_synccmd_alloc(), cpqary3_synccmd_send(),
 *			cpqary3_synccmd_free()
 * Return Values: 	SUCCESS - abort cmd submit is successful.
 *			FAILURE - Could not submit the abort cmd.
 */
uint8_t
cpqary3_send_abortcmd(cpqary3_t *cpq, cpqary3_target_t *tgtp,
    cpqary3_command_t *abort_cpcm)
{
	CommandList_t *cmdlistp;
	cpqary3_tag_t *cpqary3_tagp;
	cpqary3_command_t *cpcm;

	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	/*
	 * Occupy the Command List
	 * Update the Command List accordingly
	 * Submit the command and wait for a signal
	 */
	if ((cpcm = cpqary3_synccmd_alloc(cpq, 0)) == NULL) {
		return (ENOMEM);
	}

	cmdlistp = cpcm->cpcm_va_cmd;

	cmdlistp->Header.LUN.PhysDev.TargetId = 0;
	cmdlistp->Header.LUN.PhysDev.Bus = 0;
	cmdlistp->Header.LUN.PhysDev.Mode = PERIPHERIAL_DEV_ADDR;

	cmdlistp->Request.Type.Type = CISS_TYPE_MSG;
	cmdlistp->Request.Type.Attribute = CISS_ATTR_HEADOFQUEUE;
	cmdlistp->Request.Type.Direction = CISS_XFER_NONE;
	cmdlistp->Request.Timeout = CISS_NO_TIMEOUT;
	cmdlistp->Request.CDBLen = CPQARY3_CDBLEN_16;
	cmdlistp->Request.CDB[0] = CISS_MSG_ABORT;

	if (abort_cpcm != NULL) {
		/*
		 * Abort the specified task.
		 */
		cmdlistp->Request.CDB[1] = CISS_ABORT_TASK;

		cpqary3_tagp = (cpqary3_tag_t *)&cmdlistp->Request.CDB[4];
		cpqary3_tagp->tag_value = abort_cpcm->cpcm_tag;

		if (abort_cpcm->cpcm_time_abort != 0) {
			abort_cpcm->cpcm_time_abort = ddi_get_lbolt();
		}
		abort_cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ABORT_SENT;

	} else {
		/*
		 * Abort all tasks for the controller.
		 * XXX Does this cause the controller to fire completion
		 * of all inflight tasks, but marked aborted?
		 */
		cmdlistp->Request.CDB[1] = CISS_ABORT_TASKSET;

		cmdlistp->Header.LUN.LogDev = tgtp->cptg_volume->cplv_addr;
#if 0
		cmdlistp->Header.LUN.LogDev.Mode = LOGICAL_VOL_ADDR;
		cmdlistp->Header.LUN.LogDev.VolId = tgtp->logical_id;
#endif
	}

	if (cpqary3_synccmd_send(cpq, cpcm, 30000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		cpqary3_synccmd_free(cpq, cpcm);
		return (CPQARY3_FAILURE);
	}

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR) {
		cpqary3_synccmd_free(cpq, cpcm);
		return (CPQARY3_FAILURE);
	}

	cpqary3_synccmd_free(cpq, cpcm);

	return (CPQARY3_SUCCESS);
}

int
cpqary3_flush_cache(cpqary3_t *cpqary3p)
{
	cpqary3_command_t *cpcm;
	CommandList_t *cmdlistp;

	/* grab a command and allocate a dma buffer */
	if ((cpcm = cpqary3_synccmd_alloc(cpqary3p,
	    sizeof (flushcache_buf_t))) == NULL) {
		dev_err(cpqary3p->dip, CE_WARN, "flush cache failed: memory");
		return (ENOMEM);
	}

	cmdlistp = cpcm->cpcm_va_cmd;

	cmdlistp->Header.LUN.PhysDev.TargetId = 0;
	cmdlistp->Header.LUN.PhysDev.Bus = 0;
	cmdlistp->Header.LUN.PhysDev.Mode = PERIPHERIAL_DEV_ADDR;

	cmdlistp->Request.CDBLen = CPQARY3_CDBLEN_16;
	cmdlistp->Request.Type.Type = CISS_TYPE_CMD;
	cmdlistp->Request.Type.Attribute = CISS_ATTR_HEADOFQUEUE;
	cmdlistp->Request.Type.Direction = CISS_XFER_WRITE;
	cmdlistp->Request.Timeout = CISS_NO_TIMEOUT;
	cmdlistp->Request.CDB[0] = CISS_SCMD_ARRAY_WRITE;
	cmdlistp->Request.CDB[6] = BMIC_FLUSH_CACHE;
	cmdlistp->Request.CDB[8] = 0x02;

	if (cpqary3_synccmd_send(cpqary3p, cpcm, 90000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		dev_err(cpqary3p->dip, CE_WARN, "flush cache failed: timeout");
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (ETIMEDOUT);
	}

	cpqary3_synccmd_free(cpqary3p, cpcm);
	return (0);
}

#if 0
/*
 * Function	:  	cpqary3_probe4LVs
 * Description	:  	This routine probes for the logical drives
 *			configured on the HP Smart Array controllers
 * Called By	:  	cpqary3_probe4targets()
 * Parameters	:  	per controller
 * Calls	:  	cpqary3_synccmd_alloc(), cpqary3_synccmd_send()
 *			cpqary3_synccmd_free()
 * Return Values:  	None
 */
uint8_t
cpqary3_probe4LVs(cpqary3_t *cpqary3p)
{
	ulong_t			log_lun_no = 0;
	ulong_t			lun_id = 0;
	ulong_t			ld_count = 0;
	ulong_t			i = 0;
	ulong_t			cntr = 0;
	uint32_t		data_addr_len;
	rll_data_t		*rllp;
	CommandList_t		*cmdlistp;
	cpqary3_command_t	*cpcm;

	/*
	 * Occupy the Command List
	 * Allocate Physically Contigous Memory
	 * Update the Command List for Report Logical LUNS (rll) Command
	 * This command detects all existing logical drives.
	 * Submit and Poll for completion
	 */

	if ((cpcm = cpqary3_synccmd_alloc(cpqary3p,
	    sizeof (rll_data_t))) == NULL) {
		return (CPQARY3_FAILURE);
	}

	cmdlistp = cpcm->cpcm_va_cmd;
	rllp = cpcm->cpcm_internal->cpcmi_va;

	cmdlistp->Header.LUN.PhysDev.Mode = MASK_PERIPHERIAL_DEV_ADDR;

	cmdlistp->Request.CDBLen = CPQARY3_CDBLEN_12;
	cmdlistp->Request.Timeout = CISS_NO_TIMEOUT;
	cmdlistp->Request.Type.Type = CISS_TYPE_CMD;
	cmdlistp->Request.Type.Attribute = CISS_ATTR_ORDERED;
	cmdlistp->Request.Type.Direction = CISS_XFER_READ;
	cmdlistp->Request.CDB[0] = CISS_SCMD_REPORT_LOGICAL_LUNS;

	data_addr_len = sizeof (rll_data_t);

	cmdlistp->Request.CDB[6] = (data_addr_len >> 24) & 0xff;
	cmdlistp->Request.CDB[7] = (data_addr_len >> 16) & 0xff;
	cmdlistp->Request.CDB[8] = (data_addr_len >> 8) & 0xff;
	cmdlistp->Request.CDB[9] = (data_addr_len) & 0xff;

	DTRACE_PROBE2(rll_cmd_send, CommandList_t *, cmdlistp,
	    cpqary3_cmdpvt_t *, cpcm);

	if (cpqary3_synccmd_send(cpqary3p, cpcm, 90000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (CPQARY3_FAILURE);
	}

	if ((cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR) &&
	    (cpcm->cpcm_va_err->CommandStatus != CISS_CMD_DATA_UNDERRUN)) {
		dev_err(cpqary3p->dip, CE_WARN, "probe for logical targets "
		    "failed");
		DTRACE_PROBE1(rll_cmd_fail,
		    ErrorInfo_t *, cpcm->cpcm_va_err);
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (CPQARY3_FAILURE);
	}

	log_lun_no = ((rllp->lunlist_byte0 + (rllp->lunlist_byte1 << 8) +
	    (rllp->lunlist_byte2 << 16) + (rllp->lunlist_byte3 << 24)) / 8);

	DTRACE_PROBE2(rll_cmd_result, rll_data_t *, rllp, ulong_t, log_lun_no);

	/*
	 * The following is to restrict the maximum number of supported logical
	 * volumes to 32. This is very important as controller support upto 128
	 * logical volumes and this driver implementation supports only 32.
	 * XXX ... what?
	 */

	if (log_lun_no > MAX_LOGDRV) {
		log_lun_no = MAX_LOGDRV;
	}

	cpqary3p->cpq_ntargets = log_lun_no;
	DTRACE_PROBE1(update_lvlun_count, ulong_t, log_lun_no);

	/*
	 * Update per target structure with relevant information
	 * CPQARY#_TGT_ALLIGNMENT is 1 because of the following mapping:
	 * Target IDs 0-6 	in the OS = Logical Drives 0 - 6 in the HBA
	 * Target ID  7 	in the OS = none in the HBA
	 * Target IDs 8-32 	in the OS = Logical Drives 7 - 31 in the HBA
	 * Everytime we reference a logical drive with ID > 6, we shall use
	 * the alignment.
	 */


	/*
	 * The Logical Drive numbers will not be renumbered in the case of
	 * holes, and the mapping will be done as shown below:
	 *
	 * Logical Drive 0 in the HBA ->  Target ID 0 i.e. cXt0dXsx
	 * Logical Drive 2 in the HBA ->  Target ID 2 i.e. cXt2dXsX
	 * Logical Drive 3 in the HBA ->  Target ID 3 i.e. cXt3dXsX
	 */

	/*
	 * Fix for QXCR1000446657: Logical drives are re numbered after
	 * deleting a Logical drive.
	 * We are using new indexing mechanism to fill the
	 * cpq_targets[],
	 * Check given during memory allocation of cpq_targets
	 * elements, so that memory is not re-allocated each time the
	 * cpqary3_probe4LVs() is called.
	 * Check given while freeing the memory of the cpq_targets[]
	 * elements, when a hole is found in the Logical Drives
	 * configured.
	 */

	/* ensure that the loop will break for cntr = 32 in any case */
	for (cntr = 0; ((ld_count < log_lun_no) && (cntr < MAX_LOGDRV));
	    cntr++) {
		i = ((cntr < CTLR_SCSI_ID) ?
		    cntr : cntr + CPQARY3_TGT_ALIGNMENT);
		lun_id = (rllp->ll_data[ld_count].logical_id & 0xFFFF);
		if (cntr != lun_id) {
			if (cpqary3p->cpq_targets[i]) {
				kmem_free(cpqary3p->cpq_targets[i],
				    sizeof (cpqary3_tgt_t));
				cpqary3p->cpq_targets[i] = NULL;
			}
		} else {
			if (cpqary3p->cpq_targets[i] == NULL &&
			    !(cpqary3p->cpq_targets[i] =
			    (cpqary3_tgt_t *)kmem_zalloc(
			    sizeof (cpqary3_tgt_t), KM_NOSLEEP))) {
				cmn_err(CE_WARN,
				    "CPQary3 : Failed to Detect "
				    "targets, Memory Allocation "
				    "Failure");
				/* Sync Changes */
				cpqary3_synccmd_free(cpqary3p, cpcm);
				/* Sync Changes */
				return (CPQARY3_FAILURE);
			}
			cpqary3p->cpq_targets[i]->logical_id =
			    rllp->ll_data[ld_count].logical_id;
			cpqary3p->cpq_targets[i]->type =
			    CPQARY3_TARGET_LOG_VOL;

			/*
			 * Send "BMIC sense logical drive status
			 * command to set the target type to
			 * CPQARY3_TARGET_NONE in case of logical
			 * drive failure
			 */

			ld_count++;
		}
	}

	for (; cntr < MAX_LOGDRV; cntr++) {
		cpqary3_tgt_t *t;
		i = ((cntr < CTLR_SCSI_ID) ?
		    cntr : cntr + CPQARY3_TGT_ALIGNMENT);
		t = cpqary3p->cpq_targets[i];
		cpqary3p->cpq_targets[i] = NULL;
		if (t != NULL) {
			kmem_free(t, sizeof (*t));
		}
	}

	cpqary3_synccmd_free(cpqary3p, cpcm);

	return (CPQARY3_SUCCESS);
}

/*
 * Function	:  	cpqary3_probe4Tapes
 * Description	:  	This routine probes for the logical drives
 *			configured on the HP Smart Array controllers
 * Called By	:  	cpqary3_probe4targets()
 * Parameters	:  	per controller
 * Calls	:  	cpqary3_synccmd_alloc(), cpqary3_synccmd_send()
 *			cpqary3_synccmd_free()
 * Return Values:  	None
 */
uint8_t
cpqary3_probe4Tapes(cpqary3_t *cpqary3p)
{
	uint8_t			phy_lun_no;
	uint32_t		ii = 0;
	uint8_t			cntr = 0;
	uint32_t		data_addr_len;
	rpl_data_t		*rplp;
	CommandList_t		*cmdlistp;
	cpqary3_command_t	*cpcm;

	/*
	 * Occupy the Command List
	 * Allocate Physically Contigous Memory
	 * Update the Command List for Report Logical LUNS (rll) Command
	 * This command detects all existing logical drives.
	 * Submit and Poll for completion
	 */

	if ((cpcm = cpqary3_synccmd_alloc(cpqary3p, sizeof (rpl_data_t))) ==
	    NULL) {
		return (CPQARY3_FAILURE);
	};

	cmdlistp = cpcm->cpcm_va_cmd;
	rplp = cpcm->cpcm_internal->cpcmi_va;

	/* Sync Changes */

	cmdlistp->Header.LUN.PhysDev.TargetId = 0;
	cmdlistp->Header.LUN.PhysDev.Bus = 0;
	cmdlistp->Header.LUN.PhysDev.Mode = MASK_PERIPHERIAL_DEV_ADDR;

	cmdlistp->Request.CDBLen = CPQARY3_CDBLEN_12;
	cmdlistp->Request.Timeout = CISS_NO_TIMEOUT;
	cmdlistp->Request.Type.Type = CISS_TYPE_CMD;
	cmdlistp->Request.Type.Attribute = CISS_ATTR_ORDERED;
	cmdlistp->Request.Type.Direction = CISS_XFER_READ;
	cmdlistp->Request.CDB[0] = CISS_SCMD_REPORT_PHYSICAL_LUNS;

	data_addr_len = sizeof (rpl_data_t);

	cmdlistp->Request.CDB[6] = (data_addr_len >> 24) & 0xff;
	cmdlistp->Request.CDB[7] = (data_addr_len >> 16) & 0xff;
	cmdlistp->Request.CDB[8] = (data_addr_len >> 8) & 0xff;
	cmdlistp->Request.CDB[9] = (data_addr_len) & 0xff;

	DTRACE_PROBE2(tape_probe_cmd_send,
	    CommandList_t *, cmdlistp, cpqary3_command_t *, cpcm);

	if (cpqary3_synccmd_send(cpqary3p, cpcm, 90000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (CPQARY3_FAILURE);
	}

	if ((cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR) &&
	    (cpcm->cpcm_va_err->CommandStatus != CISS_CMD_DATA_UNDERRUN)) {
		cmn_err(CE_WARN, "CPQary3 : Probe for physical targets "
		    "returned ERROR !");
		DTRACE_PROBE1(tape_probe_cmdfail,
		    ErrorInfo_t *, cpcm->cpcm_va_err);
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (CPQARY3_FAILURE);
	}

	phy_lun_no = ((rplp->lunlist_byte0 +
	    (rplp->lunlist_byte1 << 8) +
	    (rplp->lunlist_byte2 << 16) +
	    (rplp->lunlist_byte3 << 24)) / 8);

	/*
	 *	Update per target structure with relevant information
	 * CPQARY3_TAPE_BASE is 33 because of the following mapping:
	 * Target IDs 0-6 	in the OS = Logical Drives 0 - 6 in the HBA
	 * Target ID  7 	in the OS = none in the HBA
	 * Target IDs 8-32 	in the OS = Logical Drives 7 - 31 in the HBA
	 * Target IDs 33 and above are reserved for Tapes and hence we need
	 * the alignment.
	 */


	/*
	 * HP Smart Array SAS controllers with Firmware revsion 5.14 or
	 * later support
	 * 64 Logical drives. So we are checking
	 * if the controller is SAS or CISS and then assigning the value of the
	 * TAPE BASE accordingly
	 */
	if (cpqary3p->cpq_board->bd_flags & SA_BD_SAS) {
		ii = 0x41;	/* MAX_LOGDRV + 1 - 64 + 1 */
	} else {
		ii = 0x21;	/* MAX_LOGDRV + 1 - 32 + 1 */
	}

	for (cntr = 0; cntr < phy_lun_no; cntr++) {
		if (rplp->pl_data[cntr].Mode == CISS_PHYS_MODE) {
			if (cpqary3p->cpq_targets[ii] == NULL &&
			    !(cpqary3p->cpq_targets[ii] =
			    (cpqary3_tgt_t *)
			    kmem_zalloc(sizeof (cpqary3_tgt_t), KM_NOSLEEP))) {
				cmn_err(CE_WARN, "CPQary3 : Failed to Detect "
				    "targets, Memory Allocation Failure");
				cpqary3_synccmd_free(cpqary3p, cpcm);
				return (CPQARY3_FAILURE);
			}

			bcopy(&(rplp->pl_data[cntr]),
			    &(cpqary3p->cpq_targets[ii]->PhysID),
			    sizeof (PhysDevAddr_t));

			cpqary3p->cpq_targets[ii]->type = CPQARY3_TARGET_TAPE;

			DTRACE_PROBE1(tape_discovered,
			    cpqary3_tgt_t *, cpqary3p->cpq_targets[ii]);

			ii++;
		}
	}

	cpqary3_synccmd_free(cpqary3p, cpcm);

	return (CPQARY3_SUCCESS);

}
#endif
