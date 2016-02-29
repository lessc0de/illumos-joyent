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
 */

#include <sys/sdt.h>
#include "cpqary3.h"

/*
 * Local Functions Definitions
 */

uint8_t cpqary3_format_unit(cpqary3_cmdpvt_t *);

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

/*
 * Function	:	cpqary3_build_cmdlist
 * Description	: 	This routine builds the command list for the specific
 *			opcode.
 * Called By	: 	cpqary3_transport()
 * Parameters	: 	cmdlist pvt struct, target id as received by SA.
 * Calls	: 	None
 * Return Values: 	SUCCESS		: 	Build is successful
 *			FAILURE		: 	Build has Failed
 */
uint8_t
cpqary3_build_cmdlist(cpqary3_command_t *cpcm, uint32_t tid)
{
	int		cntr;
	cpqary3_t	*cpq = cpcm->cpcm_ctlr;
	cpqary3_tgt_t	*tgtp;
	CommandList_t	*cmdlistp;
	cpqary3_pkt_t	*pkt = cpcm->cpcm_private;
	struct buf	*bfp = pkt->bf;

	if ((tgtp = cpq->cpqary3_tgtp[tid]) == NULL) {
		return (CPQARY3_FAILURE);
	}

	cmdlistp = cpcm->cpcm_va_cmd;

	/* Update Cmd Header */
	cmdlistp->Header.SGList = pkt->cmd_cookiecnt;
	cmdlistp->Header.SGTotal = pkt->cmd_cookiecnt;

	if (tgtp->type == CPQARY3_TARGET_CTLR) {
		cmdlistp->Header.LUN.PhysDev.TargetId = 0;
		cmdlistp->Header.LUN.PhysDev.Bus = 0;
		cmdlistp->Header.LUN.PhysDev.Mode = MASK_PERIPHERIAL_DEV_ADDR;
	} else if (tgtp->type == CPQARY3_TARGET_LOG_VOL) {
		cmdlistp->Header.LUN.LogDev.VolId = tgtp->logical_id;
		cmdlistp->Header.LUN.LogDev.Mode = LOGICAL_VOL_ADDR;
	} else if (tgtp->type == CPQARY3_TARGET_TAPE) {
		bcopy(&(tgtp->PhysID), &(cmdlistp->Header.LUN.PhysDev),
		    sizeof (PhysDevAddr_t));

		DTRACE_PROBE1(build_cmdlist_tape, CommandList_t *, cmdlistp);
	}

	/* Cmd Request */
	cmdlistp->Request.CDBLen = pkt->cdb_len;

	bcopy(pkt->scsi_cmd_pkt->pkt_cdbp, cmdlistp->Request.CDB, pkt->cdb_len);

	cmdlistp->Request.Type.Type = CISS_TYPE_CMD;
	cmdlistp->Request.Type.Attribute = CISS_ATTR_ORDERED;

	DTRACE_PROBE2(build_cmdlist_buf, struct buf *, bfp,
	    CommandList_t *, cmdlistp);

	if (bfp && (bfp->b_flags & B_READ))
		cmdlistp->Request.Type.Direction = CISS_XFER_READ;
	else if (bfp && (bfp->b_flags & B_WRITE))
		cmdlistp->Request.Type.Direction = CISS_XFER_WRITE;
	else
		cmdlistp->Request.Type.Direction = CISS_XFER_NONE;
	/*
	 * Looks like the above Direction is going for a toss in case of
	 * MSA20(perticularly for 0x0a-write) connected to SMART Array.
	 * If the above check fails, the below switch should take care.
	 */

	switch (cmdlistp->Request.CDB[0]) {
	case 0x08: /* XXX ? */
	case 0x28: /* XXX ? */
		cmdlistp->Request.Type.Direction = CISS_XFER_READ;
		break;
	case 0x0A: /* XXX ? */
	case 0x2A: /* XXX ? */
		cmdlistp->Request.Type.Direction = CISS_XFER_WRITE;
		break;
	}

	/*
	 * NEED to increase this TimeOut value when the concerned
	 * targets are tape devices(i.e., we need to do it here manually).
	 */
	cmdlistp->Request.Timeout = 2 * pkt->scsi_cmd_pkt->pkt_time;

	for (cntr = 0; cntr < pkt->cmd_cookiecnt; cntr++) {
		cmdlistp->SG[cntr].Addr =
		    pkt->cmd_dmacookies[cntr].dmac_laddress;
		cmdlistp->SG[cntr].Len = (uint32_t)
		    pkt->cmd_dmacookies[cntr].dmac_size;
	}

	return (CPQARY3_SUCCESS);
}


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
cpqary3_send_abortcmd(cpqary3_t *cpqary3p, uint16_t target_id,
    CommandList_t *cmdlist2abortp)
{
	CommandList_t		*cmdlistp;
	cpqary3_tgt_t		*cpqtgtp;
	cpqary3_tag_t		*cpqary3_tagp;
	cpqary3_command_t	*cpcm;

	if (target_id == CTLR_SCSI_ID) {
		return (CPQARY3_FAILURE);
	}

	if ((cpqtgtp = cpqary3p->cpqary3_tgtp[target_id]) == NULL) {
		return (CPQARY3_FAILURE);
	}

	/*
	 * Occupy the Command List
	 * Update the Command List accordingly
	 * Submit the command and wait for a signal
	 */

	/* BGB: CVFIX -> Introduced the call to cpqary3_synccmd_alloc */
	if ((cpcm = cpqary3_synccmd_alloc(cpqary3p, 0)) == NULL) {
		return (CPQARY3_FAILURE);
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

	if (cmdlist2abortp != NULL) { /* Abort this Particular Task */
		cmdlistp->Request.CDB[1] = CISS_ABORT_TASK;
		cpqary3_tagp = (cpqary3_tag_t *)&cmdlistp->Request.CDB[4];
		cpqary3_tagp->tag_value = cmdlist2abortp->Header.Tag.tag_value;
	} else { /* Abort all tasks for this Target */
		cmdlistp->Request.CDB[1] = CISS_ABORT_TASKSET;

		switch (cpqtgtp->type) {
		case CPQARY3_TARGET_LOG_VOL:
			cmdlistp->Header.LUN.LogDev.Mode = LOGICAL_VOL_ADDR;
			cmdlistp->Header.LUN.LogDev.VolId = cpqtgtp->logical_id;
			break;
		case CPQARY3_TARGET_TAPE:
			bcopy(&(cpqtgtp->PhysID),
			    &(cmdlistp->Header.LUN.PhysDev),
			    sizeof (PhysDevAddr_t));
		}
	}

	cpcm->cpcm_complete = cpqary3_synccmd_complete;

	/* BGB: CVFIX -> Introduced a call to cpqary3_synccmd_send */
	if (cpqary3_synccmd_send(cpqary3p, cpcm, 30000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (CPQARY3_FAILURE);
	}

	if (cmdlistp->Header.Tag.error != 0) {
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (CPQARY3_FAILURE);
	}

	cpqary3_synccmd_free(cpqary3p, cpcm);

	return (CPQARY3_SUCCESS);

}


/*
 * Function	: 	cpqary3_fulsh_cache
 * Description	: 	This routine flushes the controller cache.
 * Called By	: 	cpqary3_detach(), cpqary3_additional_cmd()
 * Parameters	: 	per controller
 * Calls	:  	cpqary3_synccmd_alloc(), cpqary3_synccmd_send()
 *			cpqary3_synccmd_free()
 * Return Values:	None
 */
int
cpqary3_flush_cache(cpqary3_t *cpqary3p)
{
	cpqary3_command_t *cpcm;
	CommandList_t *cmdlistp;

	/*
	 * Occupy the Command List
	 * Allocate Physically Contigous Memory for the FLUSH CACHE buffer
	 * Update the Command List accordingly
	 * Submit the command and wait for a signal
	 */

	ASSERT(cpqary3p != NULL);

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
	cmdlistp->Request.CDB[0] = ARRAY_WRITE;
	cmdlistp->Request.CDB[6] = CISS_FLUSH_CACHE; /* 0xC2 */
	cmdlistp->Request.CDB[8] = 0x02;

	cpcm->cpcm_complete = cpqary3_synccmd_complete;

	if (cpqary3_synccmd_send(cpqary3p, cpcm, 90000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		dev_err(cpqary3p->dip, CE_WARN, "flush cache failed: timeout");
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (ETIMEDOUT);
	}

	cpqary3_synccmd_free(cpqary3p, cpcm);
	return (0);
}

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
	cmdlistp->Request.CDB[0] = CISS_OPCODE_RLL;

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

	if ((cmdlistp->Header.Tag.error != 0) &&
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
	 * cpqary3_tgtp[],
	 * Check given during memory allocation of cpqary3_tgtp
	 * elements, so that memory is not re-allocated each time the
	 * cpqary3_probe4LVs() is called.
	 * Check given while freeing the memory of the cpqary3_tgtp[]
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
			if (cpqary3p->cpqary3_tgtp[i]) {
				kmem_free(cpqary3p->cpqary3_tgtp[i],
				    sizeof (cpqary3_tgt_t));
				cpqary3p->cpqary3_tgtp[i] = NULL;
			}
		} else {
			if (cpqary3p->cpqary3_tgtp[i] == NULL &&
			    !(cpqary3p->cpqary3_tgtp[i] =
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
			cpqary3p->cpqary3_tgtp[i]->logical_id =
			    rllp->ll_data[ld_count].logical_id;
			cpqary3p->cpqary3_tgtp[i]->type =
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
		t = cpqary3p->cpqary3_tgtp[i];
		cpqary3p->cpqary3_tgtp[i] = NULL;
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
	cmdlistp->Request.CDB[0] = CISS_OPCODE_RPL;

	data_addr_len = sizeof (rpl_data_t);

	cmdlistp->Request.CDB[6] = (data_addr_len >> 24) & 0xff;
	cmdlistp->Request.CDB[7] = (data_addr_len >> 16) & 0xff;
	cmdlistp->Request.CDB[8] = (data_addr_len >> 8) & 0xff;
	cmdlistp->Request.CDB[9] = (data_addr_len) & 0xff;

	DTRACE_PROBE2(tape_probe_cmd_send,
	    CommandList_t *, cmdlistp, cpqary3_command_t *, cpcm);

	cpcm->cpcm_complete = cpqary3_synccmd_complete;

	if (cpqary3_synccmd_send(cpqary3p, cpcm, 90000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (CPQARY3_FAILURE);
	}

	if ((cmdlistp->Header.Tag.error != 0) &&
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
			if (cpqary3p->cpqary3_tgtp[ii] == NULL &&
			    !(cpqary3p->cpqary3_tgtp[ii] =
			    (cpqary3_tgt_t *)
			    kmem_zalloc(sizeof (cpqary3_tgt_t), KM_NOSLEEP))) {
				cmn_err(CE_WARN, "CPQary3 : Failed to Detect "
				    "targets, Memory Allocation Failure");
				cpqary3_synccmd_free(cpqary3p, cpcm);
				return (CPQARY3_FAILURE);
			}

			bcopy(&(rplp->pl_data[cntr]),
			    &(cpqary3p->cpqary3_tgtp[ii]->PhysID),
			    sizeof (PhysDevAddr_t));

			cpqary3p->cpqary3_tgtp[ii]->type = CPQARY3_TARGET_TAPE;

			DTRACE_PROBE1(tape_discovered,
			    cpqary3_tgt_t *, cpqary3p->cpqary3_tgtp[ii]);

			ii++;
		}
	}

	cpqary3_synccmd_free(cpqary3p, cpcm);

	return (CPQARY3_SUCCESS);

}

/*
 * Function	:    cpqary3_synccmd_complete
 * Description	:    This routine processes the completed commands
 *			using the sync interface and
 *			initiates any callback that is needed.
 * Called By	:    cpqary3_transport
 * Parameters	:    per-command
 * Calls	:    cpqary3_cmdlist_release, cpqary3_synccmd_cleanup
 * Return Values:    None
 */
void
cpqary3_synccmd_complete(cpqary3_command_t *cpcm)
{
	cpqary3_t *cpq = cpcm->cpcm_ctlr;

	VERIFY(MUTEX_HELD(&cpq->sw_mutex));

	/*
	 * XXX This flag is set by the periodic function which we have
	 * disabled for now...
	 */
#if 0
	if (CPQARY3_TIMEOUT == cpqary3_cmdpvtp->cmdpvt_flag) {
		cpqary3_cmdlist_release(cpqary3_cmdpvtp, CPQARY3_NO_MUTEX);
		return;
	}
#endif

	switch (cpcm->cpcm_synccmd_status) {
	case CPQARY3_SYNCCMD_STATUS_TIMEOUT:
		/*
		 * The submitter has abandoned this command, so we
		 * have to free the resources here.
		 */
		mutex_exit(&cpq->sw_mutex);
		cpqary3_command_free(cpcm);
		mutex_enter(&cpq->sw_mutex);
		return;

	case CPQARY3_SYNCCMD_STATUS_SUBMITTED:
		cpcm->cpcm_synccmd_status = CPQARY3_SYNCCMD_STATUS_NONE;

		/*
		 * Fix for Flush Cache Operation Timed out issue:
		 * cv_signal() wakes up only one blocked thread.
		 * We need to use cv_broadcast which unblocks
		 * all the blocked threads()
		 */
		cv_broadcast(&cpq->cv_ioctl_wait);
		return;

	case CPQARY3_SYNCCMD_STATUS_NONE:
		panic("synccmd completed but status is NONE");
		break;
	}

	panic("unknown synccmd status");
}
