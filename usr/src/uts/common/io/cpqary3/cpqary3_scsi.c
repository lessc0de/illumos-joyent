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

#if 0
int
cpqary3_flush_cache(cpqary3_t *cpqary3p)
{
	cpqary3_command_t *cpcm;
	CommandList_t *cmdlistp;

	/* grab a command and allocate a dma buffer */
	if ((cpcm = cpqary3_synccmd_alloc(cpqary3p,
	    sizeof (flushcache_buf_t), KM_NOSLEEP)) == NULL) {
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

	if (cpcm->cpcm_status & CPQARY3_CMD_STATUS_RESET_SENT) {
		cpqary3_synccmd_free(cpqary3p, cpcm);
		return (EIO);
	}

	cpqary3_synccmd_free(cpqary3p, cpcm);
	return (0);
}
#endif
