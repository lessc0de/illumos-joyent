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
