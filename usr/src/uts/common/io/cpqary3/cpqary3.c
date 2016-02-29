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

#include "cpqary3.h"

/*
 * Local Autoconfiguration Function Prototype Declations
 */

int cpqary3_attach(dev_info_t *, ddi_attach_cmd_t);
int cpqary3_detach(dev_info_t *, ddi_detach_cmd_t);
int cpqary3_ioctl(dev_t, int, intptr_t, int, cred_t *, int *);

/*
 * Local Functions Definitions
 */

static void cpqary3_cleanup(cpqary3_t *);
static uint8_t cpqary3_update_ctlrdetails(cpqary3_t *, uint32_t *);
static int cpqary3_command_comparator(const void *, const void *);

/*
 * Global Variables Definitions
 */

static char cpqary3_brief[]    =	"HP Smart Array Driver";
void *cpqary3_state;

/*
 * HBA minor number schema
 *
 * The minor numbers for any minor device nodes that we create are
 * governed by the SCSA framework.  We use the macros below to
 * fabricate minor numbers for nodes that we own.
 *
 * See sys/impl/transport.h for more info.
 */

/* Macro to extract interface from minor number */
#define	CPQARY3_MINOR2INTERFACE(_x)  ((_x) & (TRAN_MINOR_MASK))

/* Base of range assigned to HBAs: */
#define	SCSA_MINOR_HBABASE  (32)

/* Our minor nodes: */
#define	CPQARY3_MINOR  (0 + SCSA_MINOR_HBABASE)

/* Convenience macros to convert device instances to minor numbers */
#define	CPQARY3_INST2x(_i, _x)    (((_i) << INST_MINOR_SHIFT) | (_x))
#define	CPQARY3_INST2CPQARY3(_i)  CPQARY3_INST2x(_i, CPQARY3_MINOR)

/*
 * The Driver DMA Limit structure.
 * Data used for SMART Integrated Array Controller shall be used.
 */

ddi_dma_attr_t cpqary3_dma_attr = {
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
	CPQARY3_SG_CNT,		/* Scatter/Gather List Length */
	512,			/* Device Granularity */
	0			/* DMA flags */
};

/*
 * The Device Access Attribute Structure.
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
	/* HPQacucli Changes */
	scsi_hba_open,
	scsi_hba_close,
	/* HPQacucli Changes */
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
	CB_REV,
	nodev,
	nodev
};

/*
 * Device Operations Structure
 */
static struct dev_ops cpqary3_dev_ops = {
	DEVO_REV,		/* Driver Build Version */
	0,			/* Driver reference count */
	nodev,			/* Get Info */
	nulldev,		/* Identify not required */
	nulldev,		/* Probe, obselete for s2.6 and up */
	cpqary3_attach,		/* Attach routine */
	cpqary3_detach,		/* Detach routine */
	nodev,			/* Reset */
	&cpqary3_cb_ops,	/* Entry Points for C&B drivers */
	NULL,			/* Bus ops */
	nodev			/* cpqary3_power */
};

/*
 * Linkage structures
 */
static struct modldrv cpqary3_modldrv = {
	&mod_driverops,		/* Module Type - driver */
	cpqary3_brief,		/* Driver Desc */
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

	/*
	 * Allocate Soft State Resources; if failure, return.
	 */
	VERIFY0(ddi_soft_state_init(&cpqary3_state, sizeof (cpqary3_t),
	    MAX_CTLRS));

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
	int  retvalue;

	/* Unload the Driver(loadable module) */

	if ((retvalue = mod_remove(&cpqary3_modlinkage)) == 0) {

		/* Cancel the registeration for the HBA Interface */
		scsi_hba_fini(&cpqary3_modlinkage);

		/* dealloacte soft state resources of the driver */
		ddi_soft_state_fini(&cpqary3_state);
	}

	return (retvalue);
}


int
_info(struct modinfo *modinfop)
{
	/*
	 * Get the module information.
	 */
	return (mod_info(&cpqary3_modlinkage, modinfop));
}


int
cpqary3_attach(dev_info_t *dip, ddi_attach_cmd_t attach_cmd)
{
	int8_t		minor_node_name[14];
	uint32_t	instance;
	uint32_t	retvalue;
	cpqary3_t	*cpq;		/* per-controller */
	ddi_dma_attr_t	tmp_dma_attr;
	uint_t		(*hw_isr)(caddr_t);
	uint_t		(*sw_isr)(caddr_t);

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
	avl_create(&cpq->cpq_inflight, cpqary3_command_comparator,
	    sizeof (cpqary3_command_t), offsetof(cpqary3_command_t,
	    cpcm_node));
	cv_init(&cpq->cv_immediate_wait, NULL, CV_DRIVER, NULL);
	cv_init(&cpq->cv_noe_wait, NULL, CV_DRIVER, NULL);
	cv_init(&cpq->cv_flushcache_wait, NULL, CV_DRIVER, NULL);
	cv_init(&cpq->cv_abort_wait, NULL, CV_DRIVER, NULL);
	cv_init(&cpq->cv_ioctl_wait, NULL, CV_DRIVER, NULL);
	cpq->cpqary3_tgtp[CTLR_SCSI_ID] = kmem_zalloc(sizeof (cpqary3_tgt_t),
	    KM_SLEEP);
	cpq->cpqary3_tgtp[CTLR_SCSI_ID]->type = CPQARY3_TARGET_CTLR;

	cpq->cpq_init_level |= CPQARY3_INITLEVEL_BASIC;

	/*
	 * Perform basic device setup, including identifying the board, mapping
	 * the I2O registers and the Configuration Table.
	 */
	if (cpqary3_device_setup(cpq) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "device setup failed");
		cpqary3_cleanup(cpq);
		return (DDI_FAILURE);
	}

	/*
	 * Select a Transport Method (e.g. Simple or Performant) and update
	 * the Configuration Table.  This function also waits for the
	 * controller to be come ready.
	 */
	if (cpqary3_ctlr_init(cpq) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "controller initialisation failed");
		cpqary3_cleanup(cpq);
		return (DDI_FAILURE);
	}

	/*
	 * Now that we have selected a Transport Method, we can configure
	 * the appropriate interrupt handlers.
	 */
	if (cpqary3_interrupts_setup(cpq) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "interrupt handler setup failed");
		cpqary3_cleanup(cpq);
		return (DDI_FAILURE);
	}

	/*
	 * Allocate HBA transport structure
	 */
	if ((cpq->hba_tran = scsi_hba_tran_alloc(dip, SCSI_HBA_CANSLEEP)) ==
	    NULL) {
		dev_err(dip, CE_WARN, "scsi_hba_tran_alloc failed");
		cpqary3_cleanup(cpq);
		return (DDI_FAILURE);
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_HBA_ALLOC;

	/*
	 * Set private field for the HBA tran structure.
	 * Initialise the HBA tran entry points.
	 * Attach the controller to HBA.
	 */
	cpqary3_init_hbatran(cpq);

	tmp_dma_attr = cpqary3_dma_attr;
	tmp_dma_attr.dma_attr_sgllen = cpq->cpq_sg_cnt;

	/*
	 * Register the DMA attributes and the transport vectors
	 * of each instance of the  HBA device.
	 */
	if (scsi_hba_attach_setup(dip, &tmp_dma_attr, cpq->hba_tran,
	    SCSI_HBA_TRAN_CLONE) == DDI_FAILURE) {
		dev_err(dip, CE_WARN, "scsi_hba_attach_setup failed");
		cpqary3_cleanup(cpq);
		return (DDI_FAILURE);
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_HBA_ATTACH;

	/*
	 * Create a minor node for Ioctl interface.
	 * The nomenclature used will be "cpqary3" immediately followed by
	 * the current driver instance in the system.
	 * for e.g.: 	for 0th instance : cpqary3,0
	 * 				for 1st instance : cpqary3,1
	 */
	(void) sprintf(minor_node_name, "cpqary3,%d", instance);

	if (ddi_create_minor_node(dip, minor_node_name, S_IFCHR,
	    CPQARY3_INST2CPQARY3(instance), DDI_NT_SCSI_NEXUS, 0) !=
	    DDI_SUCCESS) {
		cmn_err(CE_NOTE, "CPQary3 : Failed to create minor node");
		cpqary3_cleanup(cpq);
		return (DDI_FAILURE);
	}
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_MINOR_NODE;

	/* Enable the Controller Interrupt */
	cpqary3_intr_onoff(cpq, CPQARY3_INTR_ENABLE);
	if (cpq->cpq_host_support & 0x4) {
		cpqary3_lockup_intr_onoff(cpq, CPQARY3_LOCKUP_INTR_ENABLE);
	}

	/*
	 * Register a periodic function to be called every 15 seconds.
	 */
	cpq->cpq_periodic = ddi_periodic_add(cpqary3_periodic, cpq,
	    15 * NANOSEC, DDI_IPL_0);
	cpq->cpq_init_level |= CPQARY3_INITLEVEL_PERIODIC;

	/*
	 * XXX OK, maybe later...
	 */
#if 0
	/* NOE */
	if (cpqary3p->noe_support == 1) {
		/* Enable the Notification on Event in this controller */
		if (CPQARY3_SUCCESS ==
		    cpqary3_send_NOE_command(cpqary3p,
		    NULL, CPQARY3_NOE_INIT)) {
			cleanstatus |= CPQARY3_NOE_INIT_DONE;
		} else {
			cmn_err(CE_CONT, "CPQary3 : Failed to initialize "
			    "NOTIFICATION ON EVENT \n");
		}
	}
	/* NOE */
#endif

	/* Report that an Instance of the Driver is Attached Successfully */
	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}


int
cpqary3_detach(dev_info_t *dip, ddi_detach_cmd_t detach_cmd)
{
	cpqary3_t	*cpqary3p;
	scsi_hba_tran_t	*hba_tran;

	/* Return failure, If Command is not DDI_DETACH */

	if (DDI_DETACH != detach_cmd) {
		return (DDI_FAILURE);
	}

	/*
	 *  Get scsi_hba_tran structure.
	 *  Get per controller structure.
	 */

	hba_tran = (scsi_hba_tran_t *)ddi_get_driver_private(dip);
	cpqary3p = (cpqary3_t *)hba_tran->tran_hba_private;

	/* Flush the cache */

	cpqary3_flush_cache(cpqary3p);

	/* Undo cpqary3_attach */

	cpqary3_cleanup(cpqary3p);

	return (DDI_SUCCESS);

}


int
cpqary3_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rval)
{
	cpqary3_t *cpq;
	minor_t cpqary3_minor_num;
	int instance;
	int status;

	/*
	 * XXX
	 */
#if 0
	if (secpolicy_sys_config(credp, B_FALSE) != 0) {
		return (EPERM);
	}
#endif

	/*
	 * Fetch the soft state object for this instance.
	 */
	cpq = ddi_get_soft_state(cpqary3_state, MINOR2INST(getminor(dev)));
	if (cpq == NULL) {
		return (ENODEV);
	}

	switch (cmd) {
#if 0
	case CPQARY3_IOCTL_BMIC_PASS:
		status = *rval = cpqary3_ioctl_bmic_pass(arg, cpq, mode);
		break;

	case CPQARY3_IOCTL_SCSI_PASS:
		status = *rval = cpqary3_ioctl_scsi_pass(arg, cpq, mode);
		break;
#endif

	default:
		status = scsi_hba_ioctl(dev, cmd, arg, mode, credp, rval);
		break;
	}

	return (status);
}


/*
 * Function	: 	cqpary3_cleanup
 * Description	: 	This routine frees all allocated resources.
 * Called By	: 	kernel
 * Parameters	: 	per-controller, bit-map(stating what all to clean)
 * Return Values: 	None
 */
static void
cpqary3_cleanup(cpqary3_t *cpqary3p)
{
	int8_t		node_name[10];
	clock_t		cpqary3_lbolt;
	uint32_t	targ;

	ASSERT(cpqary3p != NULL);

	/*
	 * Disable the NOE command
	 * Free the Command Memory Pool
	 * destroy all conditional variables
	 */

	/*
	 * XXX maybe later
	 */
#if 0
	/*
	 * We have removed NOE functionality from the
	 * driver. So commenting the below piece of code
	 */

	if (status & CPQARY3_NOE_INIT_DONE) {
		if (CPQARY3_SUCCESS == cpqary3_disable_NOE_command(cpqary3p)) {
			mutex_enter(&cpqary3p->hw_mutex);
			cpqary3_lbolt = ddi_get_lbolt();
			if (DDI_FAILURE ==
			    cv_timedwait_sig(&cpqary3p->cv_noe_wait,
			    &cpqary3p->hw_mutex,
			    cpqary3_lbolt + drv_usectohz(3000000))) {
				cmn_err(CE_NOTE,
				    "CPQary3: Resume signal for disable NOE "
				    "command not received \n");
			}
			mutex_exit(&cpqary3p->hw_mutex);
		}
	}
#endif

	cpqary3_interrupts_teardown(cpqary3p);

	if (cpqary3p->cpq_init_level & CPQARY3_INITLEVEL_PERIODIC) {
		ddi_periodic_delete(cpqary3p->cpq_periodic);
		cpqary3p->cpq_init_level &= ~CPQARY3_INITLEVEL_PERIODIC;
	}

	if (cpqary3p->cpq_init_level & CPQARY3_INITLEVEL_MINOR_NODE) {
		(void) sprintf(node_name, "cpqary3%d",
		ddi_get_instance(cpqary3p->dip));
		ddi_remove_minor_node(cpqary3p->dip, node_name);
		cpqary3p->cpq_init_level &= ~CPQARY3_INITLEVEL_MINOR_NODE;
	}

	if (cpqary3p->cpq_init_level & CPQARY3_INITLEVEL_HBA_ATTACH) {
		(void) scsi_hba_detach(cpqary3p->dip);
		cpqary3p->cpq_init_level &= ~CPQARY3_INITLEVEL_HBA_ATTACH;
	}

	if (cpqary3p->cpq_init_level & CPQARY3_INITLEVEL_HBA_ALLOC) {
		scsi_hba_tran_free(cpqary3p->hba_tran);
		cpqary3p->cpq_init_level &= ~CPQARY3_INITLEVEL_HBA_ALLOC;
	}

	if (cpqary3p->cpq_init_level & CPQARY3_INITLEVEL_BASIC) {
		mutex_enter(&cpqary3p->hw_mutex);

		cv_destroy(&cpqary3p->cv_abort_wait);
		cv_destroy(&cpqary3p->cv_flushcache_wait);
		cv_destroy(&cpqary3p->cv_noe_wait);
		cv_destroy(&cpqary3p->cv_immediate_wait);
		cv_destroy(&cpqary3p->cv_ioctl_wait);

		for (targ = 0; targ < CPQARY3_MAX_TGT;  targ++) {
			if (cpqary3p->cpqary3_tgtp[targ] == NULL)
				continue;
			kmem_free(cpqary3p->cpqary3_tgtp[targ],
			    sizeof (cpqary3_tgt_t));
			cpqary3p->cpqary3_tgtp[targ] = NULL;
		}

		mutex_exit(&cpqary3p->hw_mutex);

		/*
		 * XXX avl_destroy, list_destroy, etc
		 */
		cpqary3p->cpq_init_level &= ~CPQARY3_INITLEVEL_BASIC;
	}

	cpqary3_device_teardown(cpqary3p);

	VERIFY0(cpqary3p->cpq_init_level);

	ddi_soft_state_free(cpqary3_state, ddi_get_instance(cpqary3p->dip));
}

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
