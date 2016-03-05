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

void
cpqary3_lockup_check(cpqary3_t *cpq)
{
	/*
	 * Read the current controller heartbeat value.
	 */
	uint32_t heartbeat = ddi_get32(cpq->cpq_ct_handle,
	    &cpq->cpq_ct->HeartBeat);

	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	/*
	 * Check to see if the value is the same as last time we looked:
	 */
	if (heartbeat != cpq->cpq_last_heartbeat) {
		/*
		 * The heartbeat value has changed, which suggests that the
		 * firmware in the controller has not yet come to a complete
		 * stop.  Record the new value, as well as the current time.
		 */
		cpq->cpq_last_heartbeat = heartbeat;
		cpq->cpq_last_heartbeat_lbolt = ddi_get_lbolt();
		return;
	}

	/*
	 * The controller _might_ have been able to signal to us that is
	 * has locked up.  This is a truly unfathomable state of affairs:
	 * If the firmware can tell it has flown off the rails, why not
	 * simply reset the controller?
	 */
	uint32_t odr = cpqary3_get32(cpq, CISS_I2O_OUTBOUND_DOORBELL_STATUS);
	uint32_t spr = cpqary3_get32(cpq, CISS_I2O_SCRATCHPAD);
	if ((odr & CISS_ODR_BIT_LOCKUP) != 0) {
		dev_err(cpq->dip, CE_PANIC, "HP SmartArray firmware has "
		    "reported a critical fault (odr %08x spr %08x)",
		    odr, spr);
	}

	clock_t expiry = cpq->cpq_last_heartbeat_lbolt + CPQARY3_SEC2HZ(60);
	if (ddi_get_lbolt() >= expiry) {
		dev_err(cpq->dip, CE_PANIC, "HP SmartArray firmware has "
		    "stopped responding (odr %08x spr %08x)",
		    odr, spr);
	}
}

cpqary3_command_t *
cpqary3_lookup_inflight(cpqary3_t *cpq, uint32_t tag)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	cpqary3_command_t srch;

	srch.cpcm_tag = tag;

	return (avl_find(&cpq->cpq_inflight, &srch, NULL));
}

#if 0
cpqary3_target_t *
cpqary3_target_from_id(cpqary3_t *cpq, unsigned id)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	if (id >= CPQARY3_MAX_TGT) {
		return (NULL);
	}

	return (cpq->cpq_targets[id]);
}

cpqary3_tgt_t *
cpqary3_target_from_addr(cpqary3_t *cpq, struct scsi_address *sa)
{
	VERIFY(MUTEX_HELD(&cpq->cpq_mutex));

	return (cpqary3_target_from_id(cpq, sa->a_target));
}
#endif

#if 0
/*
 * Function	: 	cpqary3_target_geometry
 * Description	: 	This function returns the geometry for the target.
 * Called By	: 	cpqary3_getcap()
 * Parameters	:	Target SCSI address
 * Calls	:	None
 * Return Values: 	Device Geometry
 */
int
cpqary3_target_geometry(struct scsi_address *sa)
{
	cpqary3_t	*ctlr = SA2CTLR(sa);
	cpqary3_tgt_t	*tgtp = ctlr->cpq_targets[SA2TGT(sa)];

	/*
	 * The target CHS are stored in the per-target structure
	 * during attach time. Use these values
	 */
	return ((tgtp->properties.drive.heads << 16) |
	    tgtp->properties.drive.sectors);
}
#endif

/*
 * Function	:   	cpqary3_synccmd_alloc
 * Description	:   	This function allocates the DMA buffer for the commands
 * Called By	:   	cpqary3_ioctl_send_bmiccmd(),
 *			cpqary3_ioctl_send_scsicmd()
 *			cpqary3_send_abortcmd(), cpqary3_flush_cache(),
 *			cpqary3_probe4LVs(), cpqary3_probe4Tapes(),
 *			cpqary3_detect_target_geometry()
 * Parameters	:   	per_controller, buffer size
 * Calls	:   	cpqary3_alloc_phyctgs_mem(), cpqary3_cmdlist_occupy()
 * Return Values:   	memp
 */
cpqary3_command_t *
cpqary3_synccmd_alloc(cpqary3_t *cpq, size_t bufsz, int kmflags)
{
	cpqary3_command_t *cpcm;

	if ((cpcm = cpqary3_command_alloc(cpq, CPQARY3_CMDTYPE_SYNCCMD,
	    kmflags)) == NULL) {
		return (NULL);
	}

	if (bufsz == 0) {
		return (cpcm);
	}

	if ((cpcm->cpcm_internal = cpqary3_command_internal_alloc(cpq,
	    bufsz, kmflags)) == NULL) {
		cpqary3_command_free(cpcm);
		return (NULL);
	}

	cpcm->cpcm_va_cmd->SG[0].Addr = cpcm->cpcm_internal->cpcmi_pa;
	cpcm->cpcm_va_cmd->SG[0].Len = bufsz;
	cpcm->cpcm_va_cmd->Header.SGList = 1;
	cpcm->cpcm_va_cmd->Header.SGTotal = 1;

	return (cpcm);
}

/*
 * Function	:   	cpqary3_synccmd_free
 * Description	:   	This routine frees the command and the
 *			associated resources.
 * Called By	:   	cpqary3_ioctl_send_bmiccmd(),
 *			cpqary3_ioctl_send_scsicmd()
 *			cpqary3_send_abortcmd(), cpqary3_flush_cache(),
 *			cpqary3_probe4LVs(), cpqary3_probe4Tapes(),
 *			cpqary3_detect_target_geometry()
 * Parameters	:   	per_controller, per_command_memory
 * Calls	:   	cpqary3_synccmd_cleanup()
 * Return Values:   	NONE
 */
void
cpqary3_synccmd_free(cpqary3_t *cpq, cpqary3_command_t *cpcm)
{
	/*
	 * so, the user is done with this command packet.
	 * we have three possible scenarios here:
	 *
	 * 1) the command was never submitted to the controller
	 *
	 * or
	 *
	 * 2) the command has completed at the controller and has
	 *    been fully processed by the interrupt processing
	 *    mechanism and is no longer on the submitted or
	 *    retrieve queues.
	 *
	 * or
	 *
	 * 3) the command is not yet complete at the controller,
	 *    and/or hasn't made it through cpqary3_process_pkt()
	 *    yet.
	 *
	 * For cases (1) and (2), we can go ahead and free the
	 * command and the associated resources.  For case (3), we
	 * must mark the command as no longer needed, and let
	 * cpqary3_process_pkt() clean it up instead.
	 */

	mutex_enter(&cpq->cpq_mutex);
	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_INFLIGHT) {
		cpcm->cpcm_status |= CPQARY3_CMD_STATUS_ABANDONED;
		mutex_exit(&cpq->cpq_mutex);
		return;
	}
	mutex_exit(&cpq->cpq_mutex);

	/*
	 * command was either never submitted or has completed
	 * (cases #1 and #2 above).  so, clean it up.
	 */
	cpqary3_command_free(cpcm);
}

/*
 * Function	:   	cpqary3_synccmd_send
 * Description	:   	This routine sends the command to the controller
 * Called By	:	cpqary3_ioctl_send_bmiccmd(),
 * 			cpqary3_ioctl_send_scsicmd()
 * 			cpqary3_send_abortcmd(), cpqary3_flush_cache(),
 * 			cpqary3_probe4LVs(), cpqary3_probe4Tapes(),
 * 			cpqary3_detect_target_geometry()
 * Parameters	:   	per_controller, per_command_memory, timeout value,
 * 			flag(wait for reply)
 * Calls	:   	cpqary3_submit(), cpqary3_add2submitted_cmdq()
 * Return Values:   	SUCCESS / FAILURE
 */
int
cpqary3_synccmd_send(cpqary3_t *cpqary3p, cpqary3_command_t *cpcm,
    clock_t timeoutms, int flags)
{
	clock_t		absto = 0;  /* absolute timeout */
	boolean_t waitsig = B_FALSE;
	int		rc = 0;

	VERIFY(cpcm->cpcm_type == CPQARY3_CMDTYPE_SYNCCMD);

	/*  compute absolute timeout, if necessary  */
	if (timeoutms > 0) {
		absto = ddi_get_lbolt() + drv_usectohz(timeoutms * 1000);
	}

	/*  heed signals during wait?  */
	if (flags & CPQARY3_SYNCCMD_SEND_WAITSIG) {
		waitsig = B_TRUE;
	}

	/*  acquire the sw mutex for our wait  */
	mutex_enter(&cpqary3p->cpq_mutex);

	if (cpqary3_submit(cpqary3p, cpcm) != 0) {
		mutex_exit(&cpqary3p->cpq_mutex);
		return (-1);
	}

	/*  wait for command completion, timeout, or signal  */
	while (!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_COMPLETE)) {
		kmutex_t *mt = &cpqary3p->cpq_mutex;
		kcondvar_t *cv = &cpqary3p->cpq_cv_finishq;

		/*  wait with the request behavior  */
		if (absto) {
			clock_t crc;
			if (waitsig) {
				crc = cv_timedwait_sig(cv, mt, absto);
			} else {
				crc = cv_timedwait(cv, mt, absto);
			}
			if (crc > 0) {
				rc = 0;
			} else {
				rc = -1;
			}
		} else {
			if (waitsig) {
				rc = cv_wait_sig(cv, mt);
				if (rc > 0) {
					rc = 0;
				} else {
					rc = -1;
				}
			} else {
				cv_wait(cv, mt);
				rc = 0;
			}
		}

		/*
		 * if our wait was interrupted (timeout),
		 * then break here
		 */
		if (rc) {
			break;
		}
	}

	mutex_exit(&cpqary3p->cpq_mutex);
	return (rc);
}

#if 0
/*
 * Function	: 	cpqary3_detect_target_geometry
 * Description	: 	This function determines the geometry for all
 *			the existing targets for the controller.
 * Called By	:	cpqary3_tgt_init()
 * Parameters	:	per controller
 * Calls	:	cpqary3_synccmd_alloc(), cpqary3_synccmd_send()
 *			cpqary3_synccmd_free()
 * Return Values: 	SUCCESS / FAILURE
 *			[ Shall return failure only if Memory constraints exist
 *			or controller does not respond ]
 */
int8_t
cpqary3_detect_target_geometry(cpqary3_t *ctlr)
{
	int			i;
	int8_t			ld_count = 0;
	int8_t			loop_cnt = 0;
	IdLogDrive		*idlogdrive;
	CommandList_t		*cmdlistp;
	cpqary3_command_t *cpcm;

	/*
	 * Occupy a Command List
	 * Allocate Memory for return data
	 * If error, RETURN 0.
	 * get the Request Block from the CommandList
	 * Fill in the Request Packet with the corresponding values
	 * Submit the Command and Poll for its completion
	 * If success, continue else RETURN 0
	 */

	if ((cpcm = cpqary3_synccmd_alloc(ctlr, sizeof (IdLogDrive))) ==
	    NULL) {
		return (CPQARY3_FAILURE);
	}

	cmdlistp = cpcm->cpcm_va_cmd;
	idlogdrive = cpcm->cpcm_internal->cpcmi_va;

	cmdlistp->Request.CDBLen = CPQARY3_CDBLEN_16;
	cmdlistp->Request.Type.Type = CISS_TYPE_CMD;
	cmdlistp->Request.Type.Attribute = CISS_ATTR_HEADOFQUEUE;
	cmdlistp->Request.Type.Direction = CISS_XFER_READ;
	cmdlistp->Request.CDB[0] = CISS_SCMD_ARRAY_READ;
	cmdlistp->Request.CDB[6] = BMIC_IDENTIFY_LOGICAL_DRIVE;
	cmdlistp->Request.CDB[7] = (sizeof (IdLogDrive) >> 8) & 0xff;
	cmdlistp->Request.CDB[8] = sizeof (IdLogDrive) & 0xff;

	/*
	 * For all the Targets that exist, issue an IDENTIFY LOGICAL DRIVE.
	 * That returns values which includes the dsired Geometry also.
	 * Update the Geometry in the per-target structure.
	 * NOTE : When the loop is executed for i=controller's SCSI ID, just
	 * increament by one so that we are talking to the next logical
	 * drive in our per-target structure.
	 */

	/*
	 * Fix for QXCR1000446657: Logical drives are re numbered
	 * after deleting a Logical drive.
	 * introduced, new variable ld_count, which gets
	 * incremented when the Target ID is found.
	 * And for i=controller's SCSI ID and LDs with holes are found,
	 * we continue talking to
	 * the next logical drive in the per-target structure
	 */

	for (i = 0; ld_count < ctlr->cpq_ntargets; i++) {
		if (i == CTLR_SCSI_ID || ctlr->cpq_targets[i] == NULL) {
			/*  Go to the Next logical target  */
			continue;
		}

		bzero(idlogdrive, sizeof (IdLogDrive));
		cmdlistp->Request.CDB[1] = ctlr->cpq_targets[i]->logical_id;
		/* Always zero */
		cmdlistp->Header.LUN.PhysDev.TargetId = 0;
		/*
		 * Logical volume Id numbering scheme is as follows
		 * 0x00000, 0x00001, ... - for Direct Attached
		 * 0x10000, 0x10001, ... - If 1st Port of HBA is
		 * connected to  MSA20 / MSA500
		 * 0x20000, 0x20001, ... - If 2nd Port of HBA is
		 * connected to MSA20 / MSA500
		 */
		cmdlistp->Header.LUN.PhysDev.Bus =
		    (ctlr->cpq_targets[i]->logical_id) >> 16;
		cmdlistp->Header.LUN.PhysDev.Mode =
		    (cmdlistp->Header.LUN.PhysDev.Bus > 0) ?
		    MASK_PERIPHERIAL_DEV_ADDR :	PERIPHERIAL_DEV_ADDR;

		/*
		 * Submit the command
		 * Poll for its completion
		 * If polling is not successful, something is wrong
		 * with the controler
		 * Return FAILURE (No point in continuing if h/w is
		 * faulty !!!)
		 */

		cpqary3_command_reuse(cpcm);

		if (cpqary3_synccmd_send(ctlr, cpcm, 90000,
		    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
			/* Timed out */
			cpqary3_synccmd_free(ctlr, cpcm);
			return (CPQARY3_FAILURE);
		}

		if ((cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR) &&
		    cpcm->cpcm_va_err->CommandStatus != 2) {
			DTRACE_PROBE1(id_logdrv_fail,
			    ErrorInfo_t *, cpcm->cpcm_va_err);
			cpqary3_synccmd_free(ctlr, cpcm);
			return (CPQARY3_FAILURE);
		}

		ctlr->cpq_targets[i]->properties.drive.heads =
		    idlogdrive->heads;
		ctlr->cpq_targets[i]->properties.drive.sectors =
		    idlogdrive->sectors;

		DTRACE_PROBE2(tgt_geometry_detect,
		    int, i, IdLogDrive *, idlogdrive);

		ld_count++;
	}

	cpqary3_synccmd_free(ctlr, cpcm);

	return (CPQARY3_SUCCESS);
}
#endif
