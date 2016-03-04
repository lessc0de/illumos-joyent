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

static boolean_t cpqary3_is_scsi_read_write(struct scsi_pkt *scsi_pktp);

/*
 * External Variable Declarations
 */

extern ddi_dma_attr_t cpqary3_dma_attr;

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

/*
 * Function	:	cpqary3_getcap
 * Description	: 	This routine is called to get the current value of a
 *			capability.(SCSI transport capability)
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, capability identifier, target(s) affected
 * Calls	: 	None
 * Return Values: 	current value of capability / -1 (if unsupported)
 */
int
cpqary3_getcap(struct scsi_address *sa, char *capstr, int tgtonly)
{
	int index;
#if 0
	cpqary3_t *cpq = SA2CTLR(sa);
	cpqary3_tgt_t *tgtp = cpqary3_target_from_addr(cpq, sa);
#endif

	/*
	 * If requested Capability is not supported, return -1.
	 */
	if ((index = scsi_hba_lookup_capstr(capstr)) == DDI_FAILURE) {
		return (CAP_NOT_DEFINED);
	}

	/*
	 * Getting capability for a particulat target is supported
	 * the generic form of tran_getcap() is unsupported(for all targets)
	 * If directed towards a particular target, return current capability.
	 */
	if (tgtonly == 0) {	/* all targets */
		DTRACE_PROBE1(getcap_alltgt, int, index);
		return (CAP_NOT_DEFINED);
	}

	DTRACE_PROBE1(getcap_index, int, index);

	switch (index) {
	case SCSI_CAP_DMA_MAX:
		return ((int)cpqary3_dma_attr.dma_attr_maxxfer);
#if 0
	case SCSI_CAP_DISCONNECT:
		return (tgtp->ctlr_flags & CPQARY3_CAP_DISCON_ENABLED);
	case SCSI_CAP_SYNCHRONOUS:
		return (tgtp->ctlr_flags & CPQARY3_CAP_SYNC_ENABLED);
	case SCSI_CAP_WIDE_XFER:
		return (tgtp->ctlr_flags & CPQARY3_CAP_WIDE_XFER_ENABLED);
	case SCSI_CAP_ARQ:
		return ((tgtp->ctlr_flags & CPQARY3_CAP_ARQ_ENABLED) ? 1 : 0);
	case SCSI_CAP_INITIATOR_ID:
		return (CTLR_SCSI_ID);
#endif
	case SCSI_CAP_UNTAGGED_QING:
		return (1);
	case SCSI_CAP_TAGGED_QING:
		return (1);
	case SCSI_CAP_SECTOR_SIZE:
		return (cpqary3_dma_attr.dma_attr_granular);
	case SCSI_CAP_TOTAL_SECTORS:
		return (CAP_NOT_DEFINED);
#if 0
	case SCSI_CAP_GEOMETRY:
		return (cpqary3_target_geometry(sa));
#endif
	case SCSI_CAP_RESET_NOTIFICATION:
		return (0);
	default:
		return (CAP_NOT_DEFINED);
	}
}

/*
 * Function	:	cpqary3_setcap
 * Description	: 	This routine is called to set the current value of a
 *			capability.(SCSI transport capability)
 * Called By	: 	kernel
 * Parameters	: 	SCSI address, capability identifier,
 *			new capability value, target(s) affected
 * Calls	: 	None
 * Return Values: 	SUCCESS / FAILURE / -1 (if capability is unsupported)
 */
/* ARGSUSED */
int
cpqary3_setcap(struct scsi_address *sa, char *capstr, int value, int tgtonly)
{
	int	index;
	int	retstatus = CAP_NOT_DEFINED;

	/*
	 * If requested Capability is not supported, return -1.
	 */
	if ((index = scsi_hba_lookup_capstr(capstr)) == DDI_FAILURE)
		return (retstatus);

	/*
	 * Setting capability for a particulat target is supported
	 * the generic form of tran_setcap() is unsupported(for all targets)
	 * If directed towards a particular target, set & return current
	 * capability.
	 */
	if (!tgtonly) {
		DTRACE_PROBE1(setcap_alltgt, int, index);
		return (retstatus);
	}

	DTRACE_PROBE1(setcap_index, int, index);

	switch (index) {
	case SCSI_CAP_DMA_MAX:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_DISCONNECT:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_SYNCHRONOUS:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_WIDE_XFER:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_ARQ:
		return (1);
	case SCSI_CAP_INITIATOR_ID:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_UNTAGGED_QING:
		return (1);
	case SCSI_CAP_TAGGED_QING:
		return (1);
	case SCSI_CAP_SECTOR_SIZE:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_TOTAL_SECTORS:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_GEOMETRY:
		return (CAP_CHG_NOT_ALLOWED);
	case SCSI_CAP_RESET_NOTIFICATION:
		return (CAP_CHG_NOT_ALLOWED);
	default:
		return (CAP_NOT_DEFINED);
	}
}

void
cpqary3_oscmd_complete(cpqary3_command_t *cpcm)
{
	cpqary3_t	*cpqary3p = cpcm->cpcm_ctlr;
	ErrorInfo_t	*errorinfop = cpcm->cpcm_va_err;
	CommandList_t	*cmdlistp = cpcm->cpcm_va_cmd;
	struct scsi_pkt	*scsi_pktp = cpcm->cpcm_scsa->cpcms_pkt;

	VERIFY(MUTEX_HELD(&cpqary3p->cpq_mutex));
	VERIFY(cpcm->cpcm_type == CPQARY3_CMDTYPE_OS);

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_TIMEOUT) {
		scsi_pktp->pkt_reason = CMD_TIMEOUT;
		scsi_pktp->pkt_statistics |= STAT_TIMEOUT;
		goto finish;
	}

	if (!(cpcm->cpcm_status & CPQARY3_CMD_STATUS_ERROR)) {
		scsi_pktp->pkt_state |= STATE_XFERRED_DATA | STATE_GOT_STATUS;
		goto finish;
	}

	switch (errorinfop->CommandStatus) {
	case CISS_CMD_DATA_OVERRUN:
		scsi_pktp->pkt_reason = CMD_DATA_OVR;
		scsi_pktp->pkt_state |= STATE_XFERRED_DATA | STATE_GOT_STATUS;
		break;

	case CISS_CMD_INVALID:
		DTRACE_PROBE1(invalid_cmd, struct scsi_pkt *, scsi_pktp);
		scsi_pktp->pkt_reason = CMD_BADMSG;
		scsi_pktp->pkt_state |= STATE_GOT_STATUS;
		break;

	case CISS_CMD_PROTOCOL_ERR :
		scsi_pktp->pkt_reason = CMD_BADMSG;
		scsi_pktp->pkt_state |= STATE_GOT_STATUS;
		break;

	case CISS_CMD_HARDWARE_ERR:
	case CISS_CMD_CONNECTION_LOST:
		scsi_pktp->pkt_reason = CMD_INCOMPLETE;
		scsi_pktp->pkt_state = 0; /* XXX ? */
		break;

	case CISS_CMD_ABORTED:
	case CISS_CMD_UNSOLICITED_ABORT:
		scsi_pktp->pkt_reason = CMD_ABORTED;
		scsi_pktp->pkt_statistics |= STAT_ABORTED;
		scsi_pktp->pkt_state = STATE_XFERRED_DATA | STATE_GOT_STATUS;
		break;

	case CISS_CMD_ABORT_FAILED:
		break;

	case CISS_CMD_TIMEOUT:
		scsi_pktp->pkt_reason = CMD_TIMEOUT;
		scsi_pktp->pkt_statistics |= STAT_TIMEOUT;
		break;

	case CISS_CMD_DATA_UNDERRUN:	/* Significant ONLY for Read & Write */
		if (cpqary3_is_scsi_read_write(scsi_pktp)) {
			scsi_pktp->pkt_reason = CMD_CMPLT;
			scsi_pktp->pkt_statistics = 0;
			scsi_pktp->pkt_state =
			    STATE_GOT_BUS | STATE_GOT_TARGET | STATE_SENT_CMD |
			    STATE_XFERRED_DATA | STATE_GOT_STATUS;
			break;
		}
		/* FALLTHROUGH */
	case CISS_CMD_SUCCESS:
	case CISS_CMD_TARGET_STATUS:
		scsi_pktp->pkt_reason = CMD_CMPLT;
		scsi_pktp->pkt_statistics = 0;
		scsi_pktp->pkt_state = STATE_GOT_BUS | STATE_GOT_TARGET |
		    STATE_SENT_CMD | STATE_XFERRED_DATA | STATE_GOT_STATUS;
		break;

	default:	/* Should never Occur !!! */
		scsi_pktp->pkt_reason = CMD_TRAN_ERR;
		break;
	}


	/*
	 * if ever a command completes with a CHECK CONDITION or a
	 * COMMAND_TERMINATED SCSI status, Update the sense data.
	 * NOTE : The CISS_CMD_INVALID command status would always result in a
	 * CHECK CONDITION and hence reach this part of the code.
	 */

	if ((errorinfop->ScsiStatus == SCSI_CHECK_CONDITION) ||
	    (errorinfop->ScsiStatus == SCSI_COMMAND_TERMINATED)) {
		if (errorinfop->SenseLen) {
			struct scsi_arq_status	*arq_statusp;
			arq_statusp =
			    /* LINTED: alignment */
			    (struct scsi_arq_status *)scsi_pktp->pkt_scbp;

			if ((errorinfop->ScsiStatus == SCSI_CHECK_CONDITION)) {
				arq_statusp->sts_status.sts_chk = (uint8_t)1;
			} else {
				arq_statusp->sts_status.sts_chk = (uint8_t)1;
				arq_statusp->sts_status.sts_scsi2 = (uint8_t)1;
			}
			bzero((void *)&(arq_statusp->sts_rqpkt_status),
			    sizeof (struct scsi_status));
			arq_statusp->sts_rqpkt_reason = CMD_CMPLT;
			arq_statusp->sts_rqpkt_resid = 0;
			arq_statusp->sts_rqpkt_state = scsi_pktp->pkt_state;
			arq_statusp->sts_rqpkt_statistics =
			    scsi_pktp->pkt_statistics;
			bcopy((caddr_t)&errorinfop->SenseInfo[0],
			    (caddr_t)(&arq_statusp->sts_sensedata),
			    MIN(errorinfop->SenseLen,
			    cpcm->cpcm_scsa->cpcms_pkt->pkt_scblen));
			scsi_pktp->pkt_state |= STATE_ARQ_DONE;
		}
	}

finish:
	mutex_exit(&cpqary3p->cpq_mutex);
	scsi_hba_pkt_comp(scsi_pktp);
	mutex_enter(&cpqary3p->cpq_mutex);
}

static boolean_t
cpqary3_is_scsi_read_write(struct scsi_pkt *scsi_pktp)
{
	/*
	 * In the scsi packet structure, the first byte is the SCSI Command
	 * OpCode.  We check to see if it is any one of the SCSI Read or Write
	 * opcodes.
	 */
	switch (scsi_pktp->pkt_cdbp[0]) {
	case SPC3_CMD_READ6:
	case SPC3_CMD_READ10:
	case SPC3_CMD_READ12:
	case SPC3_CMD_WRITE6:
	case SPC3_CMD_WRITE10:
	case SPC3_CMD_WRITE12:
		return (B_TRUE);

	default:
		return (B_FALSE);
	}
}
