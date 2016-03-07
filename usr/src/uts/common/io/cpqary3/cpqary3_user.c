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

#include <sys/file.h>

int
cpqary3_ioctl_passthrough(cpqary3_t *cpq, intptr_t udata, int mode,
    int *rval)
{
	return (EINVAL);
#if 0
	int status = 0;
	void *ubufp;
	uint32_t ubufsz;
	unsigned cdblen;
	boolean_t for_read;
	cpqary3_command_t *cpcm;
	STRUCT_DECL(cpqary3_ioctl_req, cppt);

	if ((mode & FWRITE) == 0) {
		return (EACCES);
	}

	STRUCT_INIT(cppt, get_udatamodel());

	if (ddi_copyin((void *)udata, STRUCT_BUF(cppt), STRUCT_SIZE(cppt),
	    mode) != 0) {
		return (EFAULT);
	}

	for_read = STRUCT_FGET(cppt, cppt_for_read) == 0 ? B_FALSE : B_TRUE;
	ubufp = STRUCT_FGETP(cppt, cppt_bufp);
	ubufsz = STRUCT_FGET(cppt, cppt_bufsz);

	/*
	 * XXX better checks?
	 */
	if (ubufsz > 1024 * 1024) {
		return (EINVAL);
	}
	if ((cdblen = STRUCT_FGET(cppt, cppt_cdblen)) > 16) {
		return (EINVAL);
	}

	/*
	 * Allocate a sync command to send to the controller.
	 */
	if ((cpcm = cpqary3_synccmd_alloc(cpq, ubufsz, KM_NOSLEEP)) ==
	    NULL) {
		return (ENOMEM);
	}

	/*
	 * Load data buffer from request into the command internal buffer.
	 */
	if (ddi_copyin(ubufp, cpcm->cpcm_internal->cpcmi_va, ubufsz,
	    mode) != 0) {
		cpqary3_synccmd_free(cpq, cpcm);
		return (EFAULT);
	}

	/*
	 * XXX These pass-through requests target the controller LUN for
	 * now...
	 */
	LUNAddr_t *lun = &cpcm->cpcm_va_cmd->Header.LUN;
	lun->PhysDev.Mode = MASK_PERIPHERIAL_DEV_ADDR;
	lun->PhysDev.TargetId = 0;
	lun->PhysDev.Bus = 0;
	bzero(&lun->PhysDev.Target, sizeof (lun->PhysDev.Target));

	bcopy(STRUCT_FGET(cppt, cppt_cdb), cpcm->cpcm_va_cmd->Request.CDB,
	    cdblen);
	cpcm->cpcm_va_cmd->Request.CDBLen = cdblen;

	cpcm->cpcm_va_cmd->Request.Type.Type = CISS_TYPE_CMD;
	cpcm->cpcm_va_cmd->Request.Type.Attribute = CISS_ATTR_SIMPLE;
	cpcm->cpcm_va_cmd->Request.Type.Direction = for_read ?
	    CISS_XFER_READ : CISS_XFER_WRITE;
	cpcm->cpcm_va_cmd->Request.Timeout = 30;

	/*
	 * Submit command to controller!
	 */
	if (cpqary3_synccmd_send(cpq, cpcm, 30 * 1000,
	    CPQARY3_SYNCCMD_SEND_WAITSIG) != 0) {
		status = EIO;
	} else {
		/*
		 * We succeeded in submitting the command to the controller,
		 * so copy the data back to the user.
		 */
		if (ddi_copyout(cpcm->cpcm_internal->cpcmi_va,
		    ubufp, ubufsz, mode) != 0) {
			status = EFAULT;
		}
	}

	cpqary3_synccmd_free(cpq, cpcm);
	
	if (status == 0) {
		*rval = 0;
	}
	return (status);
#endif
}
