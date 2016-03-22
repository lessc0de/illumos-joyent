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

#include <sys/policy.h>
#include "cpqary3.h"

static int cpqary3_attach(dev_info_t *, ddi_attach_cmd_t);
static int cpqary3_detach(dev_info_t *, ddi_detach_cmd_t);
static int cpqary3_ioctl(dev_t, int, intptr_t, int, cred_t *, int *);
static void cpqary3_cleanup(cpqary3_t *);
static int cpqary3_command_comparator(const void *, const void *);

/*
 * Controller soft state.  Each entry is an object of type "cpqary3_t".
 */
void *cpqary3_state;

/*
 * DMA attributes template.  Each controller will make a copy of this template
 * with appropriate customisations; e.g., the Scatter/Gather List Length.
 */
static ddi_dma_attr_t cpqary3_dma_attr_template = {
	DMA_ATTR_V0,		/* ddi_dma_attr version */
	0,			/* Low Address */
	0xFFFFFFFFFFFFFFFF,	/* High Address */
	0x00FFFFFF,		/* Max DMA Counter register */
	0x20,			/* Byte Alignment */
	0x20,			/* Burst Sizes : 32 Byte */
	DMA_UNIT_8,		/* Minimum DMA xfer Size */
	0xFFFFFFFF,		/* Maximum DMA xfer Size */
	/*
	 * Segment boundary restrictions
	 * The addr should not cross 4GB boundry.
	 * This is required to address an issue
	 * in the Surge ASIC, with earlier FW versions.
	 */
	0xFFFFFFFF,
	1,			/* Scatter/Gather List Length */
	512,			/* Device Granularity */
	0			/* DMA flags */
};

/*
 * Device memory access attributes for both device control registers and for
 * command block allocation.
 */
ddi_device_acc_attr_t cpqary3_dev_attributes = {
	DDI_DEVICE_ATTR_V0,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC
};

/*
 * Character-Block Operations Structure
 */
static struct cb_ops cpqary3_cb_ops = {
	scsi_hba_open,		/* cb_open */
	scsi_hba_close,		/* cb_close */
	nodev,			/* cb_strategy */
	nodev,			/* cb_print */
	nodev,			/* cb_dump */
	nodev,			/* cb_read */
	nodev,			/* cb_write */
	cpqary3_ioctl,		/* cb_ioctl */
	nodev,			/* cb_devmap */
	nodev,			/* cb_mmap */
	nodev,			/* cb_segmap */
	nochpoll,		/* cb_chpoll */
	ddi_prop_op,		/* cb_prop_op */
	NULL,			/* cb_stream */
	(int)(D_NEW|D_MP),	/* cb_flag */
	CB_REV,			/* cb_rev */
	nodev,			/* cb_aread */
	nodev			/* cb_awrite */
};

/*
 * Device Operations Structure
 */
static struct dev_ops cpqary3_dev_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* devo_refcnt */
	nodev,			/* devo_getinfo */
	nulldev,		/* devo_identify */
	nulldev,		/* devo_probe */
	cpqary3_attach,		/* devo_attach */
	cpqary3_detach,		/* devo_detach */
	nodev,			/* devo_reset */
	&cpqary3_cb_ops,	/* devo_db_ops */
	NULL,			/* devo_bus_ops */
	nodev,			/* devo_power */
	nodev			/* devo_quiesce */
};

/*
 * Linkage structures
 */
static struct modldrv cpqary3_modldrv = {
	&mod_driverops,		/* Module Type - driver */
	"HP Smart Array",	/* Driver Desc */
	&cpqary3_dev_ops	/* Driver Ops */
};

static struct modlinkage cpqary3_modlinkage = {
	MODREV_1,		/* Loadable module rev. no. */
	&cpqary3_modldrv, 	/* Loadable module */
	NULL 			/* end */
};


int
_init()
{
	int r;

	VERIFY0(ddi_soft_state_init(&cpqary3_state, sizeof (cpqary3_t), 0));

	if ((r = scsi_hba_init(&cpqary3_modlinkage)) != 0) {
		goto fail;
	}

	if ((r = mod_install(&cpqary3_modlinkage)) != 0) {
		scsi_hba_fini(&cpqary3_modlinkage);
		goto fail;
	}

	return (r);

fail:
	ddi_soft_state_fini(&cpqary3_state);
	return (r);
}

int
_fini()
{
	int r;

	if ((r = mod_remove(&cpqary3_modlinkage)) == 0) {
		scsi_hba_fini(&cpqary3_modlinkage);
		ddi_soft_state_fini(&cpqary3_state);
	}

	return (r);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&cpqary3_modlinkage, modinfop));
}

static int
cpqary3_attach(dev_info_t *dip, ddi_attach_cmd_t attach_cmd)
{
	uint32_t instance;
	cpqary3_t *cpq;

	if (attach_cmd != DDI_ATTACH) {
		return (DDI_FAILURE);
	}

	/*
	 * Allocate the per-controller soft state object and get
	 * a pointer to it.
	 */
	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(cpqary3_state, instance) != 0) {
		dev_err(dip, CE_WARN, "could not allocate soft state");
		return (DDI_FAILURE);
	}
	if ((cpq = ddi_get_soft_state(cpqary3_state, instance)) == NULL) {
		dev_err(dip, CE_WARN, "could not get soft state");
		ddi_soft_state_free(cpqary3_state, instance);
		return (DDI_FAILURE);
	}

	/*
	 * Initialise per-controller state object.
	 */
	cpq->dip = dip;
	cpq->cpq_instance = instance;
	cpq->cpq_next_tag = CPQARY3_MIN_TAG_NUMBER;
	list_create(&cpq->cpq_commands, sizeof (cpqary3_command_t),
	    offsetof(cpqary3_command_t, cpcm_link));
	list_create(&cpq->cpq_finishq, sizeof (cpqary3_command_t),
	    offsetof(cpqary3_command_t, cpcm_link_finish));
	list_create(&cpq->cpq_abortq, sizeof (cpqary3_command_t),
	    offsetof(cpqary3_command_t, cpcm_link_abort));
	list_create(&cpq->cpq_volumes, sizeof (cpqary3_volume_t),
	    offsetof(cpqary3_volume_t, cplv_link));
	list_create(&cpq->cpq_targets, sizeof (cpqary3_target_t),
	    offsetof(cpqary3_target_t, cptg_link_ctlr));
	avl_create(&cpq->cpq_inflight, cpqary3_command_comparator,
	    sizeof (cpqary3_command_t), offsetof(cpqary3_command_t,
	    cpcm_node));
	mutex_init(&cpq->cpq_mutex, NULL, MUTEX_DRIVER, NULL);
	cv_init(&cpq->cpq_cv_finishq, NULL, CV_DRIVER, NULL);

	cpq->cpq_init_level |= CPQARY3_INITLEVEL_BASIC;

	/*
	 * Perform basic device setup, including identifying the board, mapping
	 * the I2O registers and the Configuration Table.
	 */
	if (cpqary3_device_setup(cpq) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "device setup failed");
		goto fail;
	}

	/*
	 * Select a Transport Method (e.g. Simple or Performant) and update
	 * the Configuration Table.  This function also waits for the
	 * controller to be come ready.
	 */
	if (cpqary3_ctlr_init(cpq) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "controller initialisation failed");
		goto fail;
	}

	/*
	 * Each controller may have a different Scatter/Gather Element count.
	 * Configure a per-controller set of DMA attributes with the
	 * appropriate S/G size.
	 */
	VERIFY(cpq->cpq_sg_cnt > 0);
	cpq->cpq_dma_attr = cpqary3_dma_attr_template;
	cpq->cpq_dma_attr.dma_attr_sgllen = cpq->cpq_sg_cnt;

	/*
	 * From this point forward, the controller is able to accept commands
	 * and (at least by polling) return command submissions.  Setting this
	 * flag allows the rest of the driver to interact with the device.
	 */
	cpq->cpq_status |= CPQARY3_CTLR_STATUS_RUNNING;

	/*
	 * Now that we have selected a Transport Method, we can configure
	 * the appropriate interrupt handlers.
	 */
	if (cpqary3_interrupts_setup(cpq) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "interrupt handler setup failed");
		goto fail;
	}

	if (cpqary3_hba_setup(cpq) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "SCSI framework setup failed");
		goto fail;
	}

	/*
	 * Set the appropriate Interrupt Mask Register bits to start
	 * command completion interrupts from the controller.
	 */
	cpqary3_intr_set(cpq, B_TRUE);

	/*
	 * Register a periodic function to be called every 5 seconds.
	 */
	cpq->cpq_periodic = ddi_periodic_add(cpqary3_periodic, cpq,
	    5 * NANOSEC, DDI_IPL_0);
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_PERIODIC;

	/*
	 * Discover the set of logical volumes attached to this controller:
	 */
	if (cpqary3_discover_logical_volumes(cpq, 30) != 0) {
		dev_err(dip, CE_WARN, "could not discover logical volumes");
		goto fail;
	}

	/*
	 * Announce the attachment of this controller.
	 */
	ddi_report_dev(dip);

	return (DDI_SUCCESS);

fail:
	cpqary3_cleanup(cpq);
	return (DDI_FAILURE);
}

static int
cpqary3_detach(dev_info_t *dip, ddi_detach_cmd_t detach_cmd)
{
	scsi_hba_tran_t *tran = (scsi_hba_tran_t *)ddi_get_driver_private(dip);
	cpqary3_t *cpq = (cpqary3_t *)tran->tran_hba_private;

	if (detach_cmd != DDI_DETACH) {
		return (DDI_FAILURE);
	}

	/*
	 * First, check to make sure that all SCSI framework targets have
	 * detached.
	 */
	mutex_enter(&cpq->cpq_mutex);
	if (!list_is_empty(&cpq->cpq_targets)) {
		mutex_exit(&cpq->cpq_mutex);
		dev_err(cpq->dip, CE_WARN, "cannot detach; targets still "
		    "using HBA");
		return (DDI_FAILURE);
	}

	/*
	 * Prevent new targets from attaching now:
	 */
	cpq->cpq_status |= CPQARY3_CTLR_STATUS_DETACHING;
	mutex_exit(&cpq->cpq_mutex);

#if 0
	/*
	 * Attempt to have the controller flush its write cache out to disk.
	 * XXX Check for failure?
	 */
	cpqary3_flush_cache(cpq);
#endif

	/*
	 * Clean up all remaining resources.
	 */
	cpqary3_cleanup(cpq);

	return (DDI_SUCCESS);
}

static int
cpqary3_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rval)
{
	cpqary3_t *cpq;
	int inst = MINOR2INST(getminor(dev));
	int status;

	if (secpolicy_sys_config(credp, B_FALSE) != 0) {
		return (EPERM);
	}

	/*
	 * Fetch the soft state object for this instance.
	 */
	if ((cpq = ddi_get_soft_state(cpqary3_state, inst)) == NULL) {
		return (ENODEV);
	}

	switch (cmd) {
	case CPQARY3_IOCTL_PASSTHROUGH:
		status = cpqary3_ioctl_passthrough(cpq, arg, mode, rval);
		break;
	default:
		status = scsi_hba_ioctl(dev, cmd, arg, mode, credp, rval);
		break;
	}

	return (status);
}

static void
cpqary3_cleanup(cpqary3_t *cpq)
{
	cpqary3_interrupts_teardown(cpq);

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_PERIODIC) {
		ddi_periodic_delete(cpq->cpq_periodic);
		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_PERIODIC;
	}

	cpqary3_hba_teardown(cpq);

	cpqary3_ctlr_teardown(cpq);

	cpqary3_device_teardown(cpq);

	if (cpq->cpq_init_level & CPQARY3_INITLEVEL_BASIC) {
		mutex_destroy(&cpq->cpq_mutex);

		cv_destroy(&cpq->cpq_cv_finishq);

		/*
		 * XXX cleanup volumes, targets, etc!
		 */

		/*
		 * XXX avl_destroy, list_destroy, etc
		 */
		cpq->cpq_init_level &= ~CPQARY3_INITLEVEL_BASIC;
	}


	VERIFY0(cpq->cpq_init_level);

	ddi_soft_state_free(cpqary3_state, ddi_get_instance(cpq->dip));
}

/*
 * Comparator for the "cpq_inflight" AVL tree in a "cpqary3_t".  This AVL tree
 * allows a tag ID to be mapped back to the relevant "cpqary_command_t".
 */
static int
cpqary3_command_comparator(const void *lp, const void *rp)
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
