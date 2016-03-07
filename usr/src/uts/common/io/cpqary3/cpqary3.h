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

#ifndef	_CPQARY3_H
#define	_CPQARY3_H

#include <sys/types.h>
#include <sys/pci.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/conf.h>
#include <sys/map.h>
#include <sys/modctl.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/scsi/scsi.h>
#include <sys/scsi/impl/spc3_types.h>
#include <sys/devops.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include <cpqary3_ciss.h>
#include <cpqary3_bd.h>
#include <cpqary3_ioctl.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	CPQARY3_LD_FAILED	1

typedef enum cpqary3_init_level {
	CPQARY3_INITLEVEL_BASIC =		(0x1 << 0),
	CPQARY3_INITLEVEL_I2O_MAPPED =		(0x1 << 1),
	CPQARY3_INITLEVEL_CFGTBL_MAPPED =	(0x1 << 2),
	CPQARY3_INITLEVEL_PERIODIC =		(0x1 << 3),
	CPQARY3_INITLEVEL_INT_ALLOC =		(0x1 << 4),
	CPQARY3_INITLEVEL_INT_ADDED =		(0x1 << 5),
	CPQARY3_INITLEVEL_INT_ENABLED =		(0x1 << 6),
	CPQARY3_INITLEVEL_SCSA =		(0x1 << 7),
} cpqary3_init_level_t;

/*
 * Commands issued to the controller carry a (generally 32-bit, though with
 * two reserved signalling bits) identifying tag number.  In order to avoid
 * having the controller confuse us by double-reporting the completion of a
 * particular tag, we try to reuse them as infrequently as possible.  In
 * practice, this means looping through a range of values.  The minimum and
 * maximum value are defined below.
 */
#define	CPQARY3_MIN_TAG_NUMBER		0x00000100
#define	CPQARY3_MAX_TAG_NUMBER		0x0fffffff

/*
 * Definitions to support waiting for the controller to converge on a
 * particular state: ready or not ready.  These are used with
 * cpqary3_ctlr_wait_for_state().
 */
#define	CPQARY3_WAIT_DELAY_SECONDS	120
typedef enum cpqary3_wait_state {
	CPQARY3_WAIT_STATE_READY = 1,
	CPQARY3_WAIT_STATE_UNREADY
} cpqary3_wait_state_t;

typedef enum cpqary3_ctlr_mode {
	CPQARY3_CTLR_MODE_UNKNOWN = 0,
	CPQARY3_CTLR_MODE_SIMPLE
} cpqary3_ctlr_mode_t;

/*
 * Defines for Maximum and Default Settings.
 */
#define	MAX_LOGDRV		64	/* Max supported Logical Drivers */
#define	MAX_CTLRS		8	/* Max supported Controllers */

/*
 * In addition to Logical Volumes, we also expose the controller at a
 * pseudo target address on the SCSI bus we are essentially pretending to be.
 */
#define	CPQARY3_CONTROLLER_TARGET		128

/*
 * NOTE: When changing the below two entries, Max SG count in cpqary3_ciss.h
 * should also be changed.
 */
#define	MAX_PERF_SG_CNT		64	/* Maximum S/G in performant mode */
#define	CPQARY3_SG_CNT		30	/* minimum S/G in simple mode */
#define	CPQARY3_PERF_SG_CNT	31	/* minimum S/G for performant mode */


#define	CPQARY3_MAX_TGT		(MAX_LOGDRV + MAX_TAPE + 1)

/*
 * SCSI Capabilities Related IDs
 */
#define	CPQARY3_CAP_DISCON_ENABLED		0x01
#define	CPQARY3_CAP_SYNC_ENABLED		0x02
#define	CPQARY3_CAP_WIDE_XFER_ENABLED		0x04
#define	CPQARY3_CAP_ARQ_ENABLED			0x08
#define	CPQARY3_CAP_TAG_QING_ENABLED		0x10
#define	CPQARY3_CAP_TAG_QING_SUPP		0x20
#define	CPQARY3_CAP_UNTAG_DRV_QING_ENABLED	0x40

/*
 * Defines for HBA
 */
#define	CAP_NOT_DEFINED		-1
#define	CAP_CHG_NOT_ALLOWED	0
#define	CAP_CHG_SUCCESS		1

/*
 * Macros for Data Access
 */

/* SCSI Addr to Per Controller */
#define	SA2CTLR(saddr)	((cpqary3_t *)((saddr)->a_hba_tran->tran_hba_private))
#define	SA2TGT(sa)	(sa)->a_target	/* SCSI Addr to Target ID */
#define	SD2TGT(sd)	(sd)->sd_address.a_target /* SCSI Dev to Target ID */
#define	SD2LUN(sd)	(sd)->sd_address.a_lun	/* SCSI Dev to Lun */
#define	SD2SA(sd)	((sd)->sd_address)	/* SCSI Dev to SCSI Addr */

/* SCSI Dev to Per Controller */
#define	SD2CTLR(sd)	\
	((cpqary3_t *)sd->sd_address.a_hba_tran->tran_hba_private)

#define	PKT2PVTPKT(sp)  	((cpqary3_pkt_t *)((sp)->pkt_ha_private))
#define	PVTPKT2MEM(p)		((cpqary3_cmdpvt_t *)p->memp)
#define	MEM2CMD(m)		((CommandList_t *)m->cmdlist_memaddr)
#define	SP2CMD(sp)		MEM2CMD(PVTPKT2MEM(PKT2PVTPKT(sp)))
#define	CTLR2MEMLISTP(ctlr)	((cpqary3_cmdmemlist_t *)ctlr->cmdmemlistp)
#define	MEM2PVTPKT(m)		((cpqary3_pkt_t *)m->pvt_pkt)
#define	MEM2DRVPVT(m)		((cpqary3_private_t *)m->driverdata)
#define	TAG2MEM(ctlr, tag)	\
	((cpqary3_cmdpvt_t *)(CTLR2MEMLISTP(ctlr)->pool[tag]))

/* MACROS */
#define	CPQARY3_SWAP(val)   		((val >> 8) | ((val & 0xff) << 8))
#define	CPQARY3_SEC2HZ(x)		drv_usectohz((x) * 1000000)

#define	CPQARY3_BUFFER_ERROR_CLEAR	0x0	/* to be used with bioerror */
#define	CPQARY3_DMA_NO_CALLBACK		0x0	/* to be used with DMA calls */
#define	CPQARY3_DMA_ALLOC_HANDLE_DONE	0x01
#define	CPQARY3_DMA_ALLOC_MEM_DONE	0x02
#define	CPQARY3_DMA_BIND_ADDR_DONE	0x04
#define	CPQARY3_FREE_PHYCTG_MEM		0x07

/*
 * Include the driver specific relevant header files here.
 */
#include "cpqary3_ciss.h"
#include "cpqary3_mem.h"
#include "cpqary3_scsi.h"

#if 0
/*
 * Per Target Structure
 */
typedef enum cpqary3_target_type {
	CPQARY3_TARGET_TYPE_CONTROLLER = 1,
	CPQARY3_TARGET_TYPE_VOLUME
} cpqary3_target_type_t;
typedef struct cpqary3_target cpqary3_target_t;
struct cpqary3_target {
	/*
	 * Controller-side logical unit number.  The OS-side target number can
	 * be calculated by XXX
	 */
	unsigned		cpqt_logical_id;
	cpqary3_target_type_t	cpqt_type;
	dev_info_t		*cpqt_dip;
};

typedef struct cpqary3_target cpqary3_tgt_t;
struct cpqary3_target {
	uint32_t	logical_id:30; /* at most 64 : 63 drives + 1 CTLR */
	uint32_t	type:2; /* CPQARY3_TARGET_* values */
	PhysDevAddr_t	PhysID;
	union {
		struct {
			uint8_t	id;
			uint8_t	bus;
		} scsi;		/* To support tapes */
		struct {
			uint8_t	heads;
			uint8_t	sectors;
		} drive;	/* Logical drives */
	} properties;

	uint32_t	ctlr_flags;
	dev_info_t	*tgt_dip;
	ddi_dma_attr_t	dma_attrs;
};


/*
 * Values for "type" on "cpqary3_tgt_t".
 */
#define	CPQARY3_TARGET_NONE		0	/* No Device */
#define	CPQARY3_TARGET_CTLR		1	/* Controller */
#define	CPQARY3_TARGET_LOG_VOL		2	/* Logical Volume */
#define	CPQARY3_TARGET_TAPE		3	/* SCSI Device - Tape */
#endif


/*
 * Interrupt status and mask values
 */
#define	INTR_DISABLE_5300_MASK		0x00000008l
#define	INTR_DISABLE_5I_MASK		0x00000004l

#define	OUTBOUND_LIST_5300_EXISTS	0x00000008l
#define	OUTBOUND_LIST_5I_EXISTS		0x00000004l

#define	INTR_PERF_MASK			0x00000001l

#define	INTR_PERF_LOCKUP_MASK		0x00000004l

#define	INTR_E200_PERF_MASK		0x00000004l

#define	INTR_SIMPLE_MASK		0x00000008l
#define	INTR_SIMPLE_LOCKUP_MASK		0x0000000cl

#define	INTR_SIMPLE_5I_MASK		0x00000004l
#define	INTR_SIMPLE_5I_LOCKUP_MASK	0x0000000cl

typedef enum cpqary3_controller_status {
	/*
	 * A Logical Volume discovery is currently occuring.
	 */
	CPQARY3_CTLR_STATUS_DISCOVERY =		(0x1 << 0),

	/*
	 * An attempt is being made to detach the controller instance.
	 */
	CPQARY3_CTLR_STATUS_DETACHING =		(0x1 << 1),

	/*
	 * The controller is believed to be functioning correctly.  The driver
	 * is to allow command submission, process interrupts, and perform
	 * periodic background maintenance.
	 */
	CPQARY3_CTLR_STATUS_RUNNING =		(0x1 << 2),

	/*
	 * The controller is currently being reset.
	 */
	CPQARY3_CTLR_STATUS_RESETTING =		(0x1 << 3),
} cpqary3_controller_status_t;

/*
 * Per Controller Structure
 */
typedef struct cpqary3 cpqary3_t;
struct cpqary3 {
	dev_info_t		*dip;
	int			cpq_instance;
	cpqary3_controller_status_t cpq_status;

	/*
	 * Controller model-specific data.
	 */
	uint32_t		cpq_board_id;
	cpqary3_bd_t		*cpq_board;

	/*
	 * Controller configuration discovered during initialisation.
	 */
	uint32_t		cpq_host_support;
	uint32_t		cpq_bus_support;
	uint32_t		cpq_maxcmds;
	uint32_t		cpq_sg_cnt;

	/*
	 * The transport mode of the controller.
	 */
	cpqary3_ctlr_mode_t	cpq_ctlr_mode;

	/*
	 * The current initialisation level of the driver.  Bits in this field
	 * are set during initialisation and unset during cleanup of the
	 * allocated resources.
	 */
	cpqary3_init_level_t	cpq_init_level;

	/*
	 * Essentially everything is protected by "cpq_mutex".  When the
	 * completion queue is updated, threads sleeping on "cpq_cv_finishq"
	 * are awoken.
	 */
	kmutex_t		cpq_mutex;
	kcondvar_t		cpq_cv_finishq;

	list_t			cpq_volumes;
	list_t			cpq_targets;

	/*
	 * Controller Heartbeat Tracking
	 */
	uint32_t		cpq_last_heartbeat;
	hrtime_t		cpq_last_heartbeat_time;

	hrtime_t		cpq_last_discovery;
	hrtime_t		cpq_last_reset_start;
	hrtime_t		cpq_last_reset_finish;

	/*
	 * Command object tracking.  These lists, and all commands within the
	 * lists, are protected by "cpq_mutex".
	 */
	uint32_t		cpq_next_tag;
	avl_tree_t		cpq_inflight;
	list_t			cpq_commands;	/* List of all commands. */
	list_t			cpq_finishq;	/* List of completed commands. */
	list_t			cpq_abortq;	/* List of commands to abort. */

	/*
	 * Controller interrupt handler registration.
	 */
	ddi_intr_handle_t	cpq_interrupts[1];
	int			cpq_ninterrupts;

	ddi_periodic_t		cpq_periodic;

	scsi_hba_tran_t		*cpq_hba_tran;

	ddi_dma_attr_t		cpq_dma_attr;

	/*
	 * Access to the I2O Registers:
	 */
	unsigned		cpq_i2o_bar;
	caddr_t			cpq_i2o_space;
	ddi_acc_handle_t	cpq_i2o_handle;

	/*
	 * Access to the Configuration Table:
	 */
	unsigned		cpq_ct_bar;
	uint32_t		cpq_ct_baseaddr;
	CfgTable_t		*cpq_ct;
	ddi_acc_handle_t	cpq_ct_handle;
};

/*
 * Logical Volume Structure
 */
typedef enum cpqary3_volume_flags {
	CPQARY3_VOL_FLAG_WWN =			(0x1 << 0),
} cpqary3_volume_flags_t;
typedef struct cpqary3_volume {
	LogDevAddr_t		cplv_addr;
	cpqary3_volume_flags_t	cplv_flags;

	uint8_t			cplv_wwn[16];

	cpqary3_t		*cplv_ctlr;
	list_node_t		cplv_link;

	/*
	 * List of SCSA targets currently attached to this Logical Volume:
	 */
	list_t			cplv_targets;
} cpqary3_volume_t;

/*
 * Per-Target Structure
 */
typedef struct cpqary3_target {
	struct scsi_device	*cptg_scsi_dev;
	boolean_t		cptg_controller_target;

	/*
	 * Linkage back to the Logical Volume that this target represents:
	 */
	cpqary3_volume_t	*cptg_volume;
	list_node_t		cptg_link_volume;

	/*
	 * Linkage back to the controller:
	 */
	cpqary3_t		*cptg_ctlr;
	list_node_t		cptg_link_ctlr;
} cpqary3_target_t;


typedef struct cpqary3_command cpqary3_command_t;
typedef struct cpqary3_command_internal cpqary3_command_internal_t;
typedef struct cpqary3_command_scsa cpqary3_command_scsa_t;
typedef struct cpqary3_pkt cpqary3_pkt_t;

typedef enum cpqary3_command_status {
	/*
	 * When a command is submitted to the controller, it is marked USED
	 * to avoid accidental reuse of the command without reinitialising
	 * critical fields.  The submitted command is also marked INFLIGHT
	 * to reflect its inclusion in the "cpq_inflight" AVL tree.  When
	 * the command is completed by the controller, INFLIGHT is unset.
	 */
	CPQARY3_CMD_STATUS_USED =		(0x1 << 0),
	CPQARY3_CMD_STATUS_INFLIGHT =		(0x1 << 1),

	/*
	 * This flag is set during abort queue processing to record that this
	 * command was aborted in response to an expired timeout, and not some
	 * other cancellation.  If the controller is able to abort the command,
	 * we use this flag to let the SCSI framework know that the command
	 * timed out.
	 */
	CPQARY3_CMD_STATUS_TIMEOUT =		(0x1 << 2),

	/*
	 * The controller set the error bit when completing this command.
	 * Details of the particular fault may be read from the error
	 * information written by the controller.
	 */
	CPQARY3_CMD_STATUS_ERROR =		(0x1 << 3),

	/*
	 * This command has been abandoned by the original submitter.  This
	 * could happen if the command did not complete in a timely fashion.
	 * When it reaches the finish queue it will be freed without further
	 * processing.
	 */
	CPQARY3_CMD_STATUS_ABANDONED =		(0x1 << 4),

	/*
	 * This command has made it through the completion queue and had final
	 * processing performed.
	 */
	CPQARY3_CMD_STATUS_COMPLETE =		(0x1 << 5),

	/*
	 * A polled message will be ignored by the regular processing of the
	 * completion queue.  The blocking function doing the polling is
	 * responsible for watching the command on which it has set the POLLED
	 * flag.  Regular completion queue processing (which might happen in
	 * the polling function, or it might happen in the interrupt handler)
	 * will set POLL_COMPLETE once it is out of the finish queue
	 * altogether.
	 */
	CPQARY3_CMD_STATUS_POLLED =		(0x1 << 6),
	CPQARY3_CMD_STATUS_POLL_COMPLETE =	(0x1 << 7),

	/*
	 * An abort message has been sent to the controller in an attempt to
	 * cancel this command.
	 */
	CPQARY3_CMD_STATUS_ABORT_SENT =		(0x1 << 8),

	/*
	 * This command has been passed to our tran_start(9E) handler.
	 */
	CPQARY3_CMD_STATUS_TRAN_START =		(0x1 << 9),

	/*
	 * This command was for a SCSI command that we are explicitly avoiding
	 * sending to the controller.
	 */
	CPQARY3_CMD_STATUS_TRAN_IGNORED =	(0x1 << 10),

	/*
	 * This command has been submitted once, and subsequently passed to
	 * cpqary3_command_reuse().
	 */
	CPQARY3_CMD_STATUS_REUSED =		(0x1 << 11),

	/*
	 * A controller reset has been issued, so a response for this command
	 * is not expected.  If one arrives before the controller reset has
	 * taken effect, it likely cannot be trusted.
	 */
	CPQARY3_CMD_STATUS_RESET_SENT =		(0x1 << 12),
} cpqary3_command_status_t;

typedef enum cpqary3_command_type {
	CPQARY3_CMDTYPE_INTERNAL = 1,
	CPQARY3_CMDTYPE_ABORTQ,
	CPQARY3_CMDTYPE_SCSA,
} cpqary3_command_type_t;

struct cpqary3_command {
	uint32_t cpcm_tag;
	cpqary3_command_type_t cpcm_type;
	cpqary3_command_status_t cpcm_status;

	cpqary3_t *cpcm_ctlr;
	cpqary3_target_t *cpcm_target;

	list_node_t cpcm_link;		/* Linkage for allocated list. */
	list_node_t cpcm_link_finish;	/* Linkage for completion list. */
	list_node_t cpcm_link_abort;	/* Linkage for abort list. */
	avl_node_t cpcm_node;		/* Inflight AVL membership. */

	hrtime_t cpcm_time_submit;
	hrtime_t cpcm_time_complete;

	hrtime_t cpcm_expiry;

	/*
	 * The time at which an abort message was sent to try and terminate
	 * this command, as well as the tag of the abort message itself:
	 */
	hrtime_t cpcm_abort_time;
	uint32_t cpcm_abort_tag;

	/*
	 * Ancillary data objects.  Only one of these will be allocated for any
	 * given command, but we nonetheless resist the temptation to use a
	 * union of pointers in order to make incorrect usage obvious.
	 */
	cpqary3_command_scsa_t *cpcm_scsa;
	cpqary3_command_internal_t *cpcm_internal;

	/*
	 * Physical allocation tracking for the actual command to send to the
	 * controller.
	 */
	cpqary3_phyctg_t cpcm_phyctg;

	CommandList_t *cpcm_va_cmd;
	uint32_t cpcm_pa_cmd; /* XXX wrong type */

	ErrorInfo_t *cpcm_va_err;
	uint32_t cpcm_pa_err; /* XXX wrong type */
};

/*
 * Commands issued internally to the driver (as opposed to by the HBA
 * framework) generally require a buffer in which to assemble the command body,
 * and for receiving the response from the controller.  The following object
 * tracks this (optional) extra buffer.
 */
struct cpqary3_command_internal {
	cpqary3_phyctg_t cpcmi_phyctg;

	void *cpcmi_va;
	uint32_t cpcmi_pa; /* XXX wrong type */
	size_t cpcmi_len;
};

/*
 * Commands issued via the SCSI framework have a number of additional
 * properties.
 */
struct cpqary3_command_scsa {
	struct scsi_pkt		*cpcms_pkt;
	cpqary3_command_t	*cpcms_command;
};


/* Driver function definitions */

void cpqary3_periodic(void *);
int cpqary3_flush_cache(cpqary3_t *);
uint16_t cpqary3_init_ctlr_resource(cpqary3_t *);
uint8_t cpqary3_probe4targets(cpqary3_t *);
int cpqary3_submit(cpqary3_t *, cpqary3_command_t *);
void cpqary3_submit_simple(cpqary3_t *, cpqary3_command_t *);
int cpqary3_retrieve(cpqary3_t *);
void cpqary3_retrieve_simple(cpqary3_t *);
int cpqary3_target_geometry(struct scsi_address *);
int8_t cpqary3_detect_target_geometry(cpqary3_t *);
uint8_t cpqary3_send_abortcmd(cpqary3_t *, cpqary3_target_t *, cpqary3_command_t *);
void cpqary3_memfini(cpqary3_t *, uint8_t);
int16_t cpqary3_meminit(cpqary3_t *);
void cpqary3_build_cmdlist(cpqary3_command_t *cpqary3_cmdpvtp,
    cpqary3_target_t *);
void cpqary3_lockup_check(cpqary3_t *);
void cpqary3_oscmd_complete(cpqary3_command_t *);

int cpqary3_poll_for(cpqary3_t *, cpqary3_command_t *);

/*
 * Memory management.
 */
caddr_t cpqary3_alloc_phyctgs_mem(cpqary3_t *, size_t, uint32_t *,
    cpqary3_phyctg_t *, int);
void cpqary3_free_phyctgs_mem(cpqary3_phyctg_t *, uint8_t);

/*
 * Interrupt service routines.
 */
int cpqary3_interrupts_setup(cpqary3_t *);
void cpqary3_interrupts_teardown(cpqary3_t *);
uint32_t cpqary3_isr_hw_simple(caddr_t, caddr_t);

/*
 * Interrupt enable/disable routines.
 */
void cpqary3_intr_set(cpqary3_t *, boolean_t);

/*
 * Controller initialisation routines.
 */
int cpqary3_ctlr_init(cpqary3_t *);
void cpqary3_ctlr_teardown(cpqary3_t *);
int cpqary3_ctlr_reset(cpqary3_t *);
int cpqary3_ctlr_ping(cpqary3_t *, int);
int cpqary3_ctlr_wait_for_state(cpqary3_t *, cpqary3_wait_state_t);
int cpqary3_ctlr_init_simple(cpqary3_t *);
void cpqary3_ctlr_teardown_simple(cpqary3_t *);
int cpqary3_cfgtbl_flush(cpqary3_t *);
int cpqary3_cfgtbl_transport_has_support(cpqary3_t *, int);
void cpqary3_cfgtbl_transport_set(cpqary3_t *, int);
int cpqary3_cfgtbl_transport_confirm(cpqary3_t *, int);
uint32_t cpqary3_ctlr_get_cmdsoutmax(cpqary3_t *);
uint32_t cpqary3_ctlr_get_maxsgelements(cpqary3_t *);

/*
 * Device enumeration routines.
 */
int cpqary3_discover_logical_volumes(cpqary3_t *, int);
cpqary3_volume_t *cpqary3_lookup_volume_by_id(cpqary3_t *, unsigned);
cpqary3_volume_t *cpqary3_lookup_volume_by_addr(cpqary3_t *,
    struct scsi_address *);

#if 0
cpqary3_target_t *cpqary3_target_from_id(cpqary3_t *, unsigned);
cpqary3_target_t *cpqary3_target_from_addr(cpqary3_t *, struct scsi_address *);
#endif
int cpqary3_setcap(struct scsi_address *, char *, int, int);
int cpqary3_getcap(struct scsi_address *, char *, int);

int cpqary3_hba_setup(cpqary3_t *);
void cpqary3_hba_teardown(cpqary3_t *);

void cpqary3_process_finishq(cpqary3_t *);
void cpqary3_process_abortq(cpqary3_t *);

/*
 * Command object management.
 */
cpqary3_command_t *cpqary3_command_alloc(cpqary3_t *, cpqary3_command_type_t,
    int);
int cpqary3_command_attach_internal(cpqary3_t *, cpqary3_command_t *, size_t,
    int);
void cpqary3_command_free(cpqary3_command_t *);
cpqary3_command_t *cpqary3_lookup_inflight(cpqary3_t *, uint32_t);
void cpqary3_command_reuse(cpqary3_command_t *);

/*
 * XXX
 */
void cpqary3_write_lun_addr_phys(LUNAddr_t *, boolean_t, unsigned, unsigned);
void cpqary3_write_message_abort_one(cpqary3_command_t *, uint32_t);
void cpqary3_write_message_abort_all(cpqary3_command_t *, LogDevAddr_t *);
void cpqary3_write_message_nop(cpqary3_command_t *, int);

/*
 * Device management routines.
 */
int cpqary3_device_setup(cpqary3_t *);
void cpqary3_device_teardown(cpqary3_t *);
uint32_t cpqary3_get32(cpqary3_t *, offset_t);
void cpqary3_put32(cpqary3_t *, offset_t, uint32_t);

/*
 * Routines for ioctl(2) handling.
 */
int cpqary3_ioctl_passthrough(cpqary3_t *, intptr_t, int, int *);


#ifdef	__cplusplus
}
#endif

#endif	/* _CPQARY3_H */
