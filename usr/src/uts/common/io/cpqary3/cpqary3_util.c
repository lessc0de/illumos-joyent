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

int cpqary3_target_geometry(struct scsi_address *);
int8_t cpqary3_detect_target_geometry(cpqary3_t *);

/*
 * Function	: 	cpqary3_read_conf_file
 * Description	: 	This routine reads the driver configuration file.
 * Called By	: 	cpqary3_attach()
 * Parameters	: 	device-information pointer, per_controller
 * Calls	: 	None
 * Return Values: 	None
 */
void
cpqary3_read_conf_file(dev_info_t *dip, cpqary3_t *cpqary3p)
{
	char		*ptr;

	cpqary3p->noe_support = 0;

	/*
	 * Plugin the code necessary to read from driver's conf file.
	 * As of now, we are not interested in reading the onf file
	 * for any purpose.
	 *
	 * eg. :
	 *
	 * retvalue = ddi_getprop(DDI_DEV_T_NONE, dip, DDI_PROP_DONTPASS,
	 *	"cpqary3_online_debug", -1);
	 */

	/*
	 *  We are calling ddi_prop_lookup_string
	 *  which gets the property value, which is passed at
	 *  the grub menu.  
	 */
	if (ddi_prop_lookup_string(DDI_DEV_T_ANY, dip, 0,
	    "cpqary3_noesupport", &ptr) == DDI_PROP_SUCCESS) {
		if (strcmp("on", ptr) == 0) {
			cpqary3p->noe_support = 1;
		}
		if (strcmp("off", ptr) == 0) {
			cpqary3p->noe_support = 0;
		}
		ddi_prop_free(ptr);
	}
}

void
cpqary3_lockup_check(cpqary3_t *cpq)
{
	/*
	 * Read the current controller heartbeat value.
	 */
	uint32_t heartbeat = ddi_get32(cpq->ct_handle, &cpq->ct->HeartBeat);

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
	uint32_t odr = ddi_get32(cpq->odr_handle, cpq->odr);
	uint32_t spr = ddi_get32(cpq->spr0_handle, cpq->spr0);
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

/*
 * Function	: 	cpqary3_periodic
 * Description	: 	This routine is called once in 15 seconds to detect any
 *			command that is pending with the controller and has
 *			timed out.
 * Called By	: 	kernel
 * Parameters	: 	per_controller
 * Calls	: 	None
 * Return Values: 	None
 */
void
cpqary3_periodic(void *arg)
{
	cpqary3_t *cpq = arg;
	uint32_t no_cmds;

	cpqary3_lockup_check(cpq);

	/*
	 * XXX This should be re-tooled to use "cpq_inflight".
	 */
#if 0
	mutex_enter(&cpq->sw_mutex);
	no_cmds = (uint32_t)((cpq->ctlr_maxcmds / 3) * NO_OF_CMDLIST_IN_A_BLK);
	for (uint32_t i = 0; i < no_cmds; i++) {
		cpqary3_cmdpvt_t *local = &cpq->cmdmemlistp->pool[i];
		cpqary3_pkt_t *pktp;
		struct scsi_pkt *scsi_pktp;
		clock_t cpqary3_lbolt;

		ASSERT(local != NULL);
		if ((pktp = MEM2PVTPKT(local)) == NULL) {
			continue;
		}

		if ((local->cmdpvt_flag == CPQARY3_TIMEOUT) ||
		    (local->cmdpvt_flag == CPQARY3_RESET)) {
			continue;
		}

		if (local->occupied != CPQARY3_OCCUPIED) {
			continue;
		}

		scsi_pktp = pktp->scsi_cmd_pkt;
		cpqary3_lbolt = ddi_get_lbolt();
		if ((scsi_pktp) && (scsi_pktp->pkt_time)) {
			clock_t cpqary3_ticks = cpqary3_lbolt -
			    pktp->cmd_start_time;

			if ((drv_hztousec(cpqary3_ticks) / 1000000) >
			    scsi_pktp->pkt_time) {
				scsi_pktp->pkt_reason = CMD_TIMEOUT;
				scsi_pktp->pkt_statistics = STAT_TIMEOUT;
				scsi_pktp->pkt_state = STATE_GOT_BUS |
				    STATE_GOT_TARGET | STATE_SENT_CMD;
				local->cmdpvt_flag = CPQARY3_TIMEOUT;

				/* This should always be the case */
				if (scsi_pktp->pkt_comp != NULL) {
					mutex_exit(&cpq->sw_mutex);
					(*scsi_pktp->pkt_comp)(scsi_pktp);
					mutex_enter(&cpq->sw_mutex);
					continue;
				}
			}
		}
	}
	mutex_exit(&cpq->sw_mutex);
#endif
}

cpqary3_command_t *
cpqary3_lookup_inflight(cpqary3_t *cpq, uint32_t tag)
{
	VERIFY(MUTEX_HELD(&cpq->hw_mutex));

	cpqary3_command_t srch;

	srch.cpcm_tag = tag;

	return (avl_find(&cpq->cpq_inflight, &srch, NULL));
}

static int
cpqary3_comparator(const void *lp, const void *rp)
{
	const cpqary3_command_t *l = lp;
	const cpqary3_command_t *r = rp;

	if (l->cpcm_tag > r->cpcm_tag) {
		return (1);
	} else if (l->cpcm_tag < r->cpcm_tag) {
		return (-1);
	} else {
		return (0);
	}
}

/*
 * Function	: 	cpqary3_init_ctlr_resource
 * Description	: 	This routine initializes the command list, initializes
 *			the controller, enables the interrupt.
 * Called By	: 	cpqary3_attach()
 * Parameters	: 	per_controller
 * Calls	: 	cpqary3_init_ctlr(), cpqary3_meminit(),
 * 			cpqary3_intr_onoff(),
 * Return Values: 	SUCCESS / FAILURE
 *			[ Shall return failure if any of the mandatory
 *			initializations / setup of resources fail ]
 */
uint16_t
cpqary3_init_ctlr_resource(cpqary3_t *ctlr)
{
	list_create(&ctlr->cpq_commands, sizeof (cpqary3_command_t),
	    offsetof(cpqary3_command_t, cpcm_link));
	avl_create(&ctlr->cpq_inflight, cpqary3_comparator,
	    sizeof (cpqary3_command_t), offsetof(cpqary3_command_t, cpcm_node));

	/*
	 * Initialize the Controller
	 * Alocate Memory Pool for driver supported number of Commands
	 * return if not successful
	 * Allocate target structure for controller and initialize the same
	 * Detect all existing targets and allocate target structure for each
	 * Determine geometry for all existing targets
	 * Initialize the condition variables
	 */
	if (cpqary3_ctlr_init(ctlr) != 0) {
		dev_err(ctlr->dip, CE_WARN, "cpqary3_ctlr_init failed");
		return (CPQARY3_FAILURE);
	}

	/*
	 * XXX
	 */
#if 0
	if (cpqary3_meminit(ctlr) != CPQARY3_SUCCESS) {
		return (CPQARY3_FAILURE);
	}
#endif

	ctlr->cpqary3_tgtp[CTLR_SCSI_ID] = kmem_zalloc(sizeof (cpqary3_tgt_t),
	    KM_NOSLEEP);
	if (!(ctlr->cpqary3_tgtp[CTLR_SCSI_ID])) {
		cmn_err(CE_WARN, "CPQary3: Target Initialization Failed");
#if 0
		cpqary3_memfini(ctlr, CPQARY3_MEMLIST_DONE |
		    CPQARY3_PHYCTGS_DONE | CPQARY3_CMDMEM_DONE);
#endif
		return (CPQARY3_FAILURE);
	}
	ctlr->cpqary3_tgtp[CTLR_SCSI_ID]->type = CPQARY3_TARGET_CTLR;

	cpqary3_intr_onoff(ctlr, CPQARY3_INTR_DISABLE);

	/*
	 * Initialize all condition variables :
	 * for the immediate call back
	 * for the disable noe
	 * for fulsh cache
	 * for probe device
	 */

	cv_init(&ctlr->cv_immediate_wait, NULL, CV_DRIVER, NULL);
	cv_init(&ctlr->cv_noe_wait, NULL, CV_DRIVER, NULL);
	cv_init(&ctlr->cv_flushcache_wait, NULL, CV_DRIVER, NULL);
	cv_init(&ctlr->cv_abort_wait, NULL, CV_DRIVER, NULL);
	cv_init(&ctlr->cv_ioctl_wait, NULL, CV_DRIVER, NULL);

	return (CPQARY3_SUCCESS);
}

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
	cpqary3_tgt_t	*tgtp = ctlr->cpqary3_tgtp[SA2TGT(sa)];

	/*
	 * The target CHS are stored in the per-target structure
	 * during attach time. Use these values
	 */
	return ((tgtp->properties.drive.heads << 16) |
	    tgtp->properties.drive.sectors);
}

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
cpqary3_synccmd_alloc(cpqary3_t *cpq, size_t bufsz)
{
	cpqary3_command_t *cpcm;

	if ((cpcm = cpqary3_command_alloc(cpq)) == NULL) {
		return (NULL);
	}

	cpcm->cpcm_type = CPQARY3_CMDTYPE_SYNCCMD;

	if (bufsz == 0) {
		return (cpcm);
	}

	if ((cpcm->cpcm_internal = cpqary3_command_internal_alloc(cpq,
	    bufsz)) == NULL) {
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

	mutex_enter(&cpq->sw_mutex);
	if (cpcm->cpcm_synccmd_status == CPQARY3_SYNCCMD_STATUS_SUBMITTED) {
		/*
		 * command is still pending (case #3 above).
		 * mark the command as abandoned and let
		 * cpqary3_process_pkt() clean it up.
		 */
		cpcm->cpcm_synccmd_status = CPQARY3_SYNCCMD_STATUS_TIMEOUT;
		mutex_exit(&cpq->sw_mutex);
		return;
	}
	cpcm->cpcm_synccmd_status = CPQARY3_SYNCCMD_STATUS_NONE;
	mutex_exit(&cpq->sw_mutex);

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

	/*  compute absolute timeout, if necessary  */
	if (timeoutms > 0) {
		absto = ddi_get_lbolt() + drv_usectohz(timeoutms * 1000);
	}

	/*  heed signals during wait?  */
	if (flags & CPQARY3_SYNCCMD_SEND_WAITSIG) {
		waitsig = B_TRUE;
	}

	/*  acquire the sw mutex for our wait  */
	mutex_enter(&cpqary3p->sw_mutex);
	mutex_enter(&cpqary3p->hw_mutex);

	VERIFY(cpcm->cpcm_synccmd_status == CPQARY3_SYNCCMD_STATUS_NONE);
	cpcm->cpcm_synccmd_status = CPQARY3_SYNCCMD_STATUS_SUBMITTED;

	if (cpqary3_submit(cpqary3p, cpcm) != 0) {
		mutex_exit(&cpqary3p->hw_mutex);
		mutex_exit(&cpqary3p->sw_mutex);
		return (-1);
	}
	mutex_exit(&cpqary3p->hw_mutex);

	/*  wait for command completion, timeout, or signal  */
	while (cpcm->cpcm_synccmd_status == CPQARY3_SYNCCMD_STATUS_SUBMITTED) {
		kmutex_t *mt = &cpqary3p->sw_mutex;
		kcondvar_t *cv = &cpqary3p->cv_ioctl_wait;

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

	mutex_exit(&cpqary3p->sw_mutex);
	return (rc);
}

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

	/* Cmd Reques */
	cmdlistp->Request.CDBLen = CPQARY3_CDBLEN_16;
	cmdlistp->Request.CDB[0] = 0x26;
	cmdlistp->Request.CDB[6] = BMIC_IDENTIFY_LOGICAL_DRIVE;
	cmdlistp->Request.CDB[7] = (sizeof (IdLogDrive) >> 8) & 0xff;
	cmdlistp->Request.CDB[8] = sizeof (IdLogDrive) & 0xff;
	cmdlistp->Request.Type.Type = CISS_TYPE_CMD;
	cmdlistp->Request.Type.Attribute = CISS_ATTR_HEADOFQUEUE;
	cmdlistp->Request.Type.Direction = CISS_XFER_READ;

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

	for (i = 0; ld_count < ctlr->num_of_targets; i++) {
		if (i == CTLR_SCSI_ID || ctlr->cpqary3_tgtp[i] == NULL) {
			/*  Go to the Next logical target  */
			continue;
		}

		bzero(idlogdrive, sizeof (IdLogDrive));
		cmdlistp->Request.CDB[1] = ctlr->cpqary3_tgtp[i]->logical_id;
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
		    (ctlr->cpqary3_tgtp[i]->logical_id) >> 16;
		cmdlistp->Header.LUN.PhysDev.Mode =
		    (cmdlistp->Header.LUN.PhysDev.Bus > 0) ?
		    MASK_PERIPHERIAL_DEV_ADDR :	PERIPHERIAL_DEV_ADDR;

		cpcm->cpcm_complete = cpqary3_synccmd_complete;

		/*
		 * Submit the command
		 * Poll for its completion
		 * If polling is not successful, something is wrong
		 * with the controler
		 * Return FAILURE (No point in continuing if h/w is
		 * faulty !!!)
		 */

		if (cpqary3_synccmd_send(ctlr, cpcm, 90000,
		    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
			/* Timed out */
			cpqary3_synccmd_free(ctlr, cpcm);
			return (CPQARY3_FAILURE);
		}

		if (cmdlistp->Header.Tag.error != 0 &&
		    cpcm->cpcm_va_err->CommandStatus != 2) {
			DTRACE_PROBE1(id_logdrv_fail,
			    ErrorInfo_t *, cpcm->cpcm_va_err);
			cpqary3_synccmd_free(ctlr, cpcm);
			return (CPQARY3_FAILURE);
		}

		ctlr->cpqary3_tgtp[i]->properties.drive.heads =
		    idlogdrive->heads;
		ctlr->cpqary3_tgtp[i]->properties.drive.sectors =
		    idlogdrive->sectors;

		DTRACE_PROBE2(tgt_geometry_detect,
		    int, i, IdLogDrive *, idlogdrive);

		ld_count++;
	}

	cpqary3_synccmd_free(ctlr, cpcm);

	return (CPQARY3_SUCCESS);
}
