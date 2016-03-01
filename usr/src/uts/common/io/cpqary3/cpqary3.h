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

#ifdef	__cplusplus
extern "C" {
#endif

/*
 *	Ioctl Commands
 */
#define	CPQARY3_IOCTL_CMD		('c' << 4)
#define	CPQARY3_IOCTL_BMIC_PASS		CPQARY3_IOCTL_CMD | 0x04
#define	CPQARY3_IOCTL_SCSI_PASS		CPQARY3_IOCTL_CMD | 0x08

/* Some Useful definations */
#define	CPQARY3_FAILURE		0
#define	CPQARY3_SUCCESS		1

#define	CTLR_SCSI_ID		7
#define	CPQARY3_LD_FAILED	1

typedef enum cpqary3_init_level {
	CPQARY3_INITLEVEL_BASIC =		(0x1 << 0),
	CPQARY3_INITLEVEL_I2O_MAPPED =		(0x1 << 1),
	CPQARY3_INITLEVEL_CFGTBL_MAPPED =	(0x1 << 2),
	CPQARY3_INITLEVEL_PERIODIC =		(0x1 << 3),
	CPQARY3_INITLEVEL_INT_HW_HANDLER =	(0x1 << 4),
	CPQARY3_INITLEVEL_INT_SW_HANDLER =	(0x1 << 5),
	CPQARY3_INITLEVEL_MUTEX =		(0x1 << 6),
	CPQARY3_INITLEVEL_MINOR_NODE =		(0x1 << 7),
	CPQARY3_INITLEVEL_HBA_ALLOC =		(0x1 << 8),
	CPQARY3_INITLEVEL_HBA_ATTACH =		(0x1 << 9),
} cpqary3_init_level_t;

/*
 * Commands issued to the controller carry a (generally 32-bit, though with two
 * reserved signalling bits) identifying tag number.  In order to avoid having
 * the controller confuse us by double-reporting the completion of a particular
 * tag, we try to reuse them as infrequently as possible.  In practice, this
 * means looping through a range of values.  The minimum and maximum value are
 * defined below.
 */
#define	CPQARY3_MIN_TAG_NUMBER		0x00000100
#define	CPQARY3_MAX_TAG_NUMBER		0x0fffffff

/*
 * Definitions to support waiting for the controller to converge on a
 * particular state; ready or not ready.  These are used with
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
#define	MAX_TAPE		28
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

/*
 * Convenient macros for reading/writing Configuration table registers
 */
#define	DDI_GET8(ctlr, regp)	 		\
	ddi_get8((ctlr)->ct_handle, (uint8_t *)(regp))
#define	DDI_PUT8(ctlr, regp, value)		\
	ddi_put8((ctlr)->ct_handle, (uint8_t *)(regp), (value))
#define	DDI_GET16(ctlr, regp)	 		\
	ddi_get16((ctlr)->ct_handle, (uint16_t *)(regp))
#define	DDI_PUT16(ctlr, regp, value)	\
	ddi_put16((ctlr)->ct_handle, (uint16_t *)(regp), (value))
#define	DDI_GET32(ctlr, regp)	 		\
	ddi_get32((ctlr)->ct_handle, (uint32_t *)(regp))
#define	DDI_PUT32(ctlr, regp, value) 	\
	ddi_put32((ctlr)->ct_handle, (uint32_t *)(regp), (value))
#define	DDI_PUT32_CP(ctlr, regp, value)   \
	ddi_put32((ctlr)->cp_handle, (uint32_t *)(regp), (value))

#define	CPQARY3_BUFFER_ERROR_CLEAR	0x0	/* to be used with bioerror */
#define	CPQARY3_DMA_NO_CALLBACK		0x0	/* to be used with DMA calls */
#define	CPQARY3_DMA_ALLOC_HANDLE_DONE	0x01
#define	CPQARY3_DMA_ALLOC_MEM_DONE	0x02
#define	CPQARY3_DMA_BIND_ADDR_DONE	0x04
#define	CPQARY3_FREE_PHYCTG_MEM		0x07
#define	CPQARY3_SYNCCMD_SEND_WAITSIG	(0x0001)

/*
 * Include the driver specific relevant header files here.
 */
#include "cpqary3_ciss.h"
#include "cpqary3_mem.h"
#include "cpqary3_scsi.h"

/*
 * Per Target Structure
 */
typedef struct cpqary3_target {
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
} cpqary3_tgt_t;


/*
 * Values for "type" on "cpqary3_tgt_t".
 */
#define	CPQARY3_TARGET_NONE		0	/* No Device */
#define	CPQARY3_TARGET_CTLR		1	/* Controller */
#define	CPQARY3_TARGET_LOG_VOL		2	/* Logical Volume */
#define	CPQARY3_TARGET_TAPE		3	/* SCSI Device - Tape */


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

typedef struct cpqary3_replyq {
	unsigned cprq_cycle_indicator;
	size_t cprq_ntags;
	uint32_t *cprq_tags;
	size_t cprq_read_index;
	uint32_t cprq_tags_pa; /* XXX wrong type */
	cpqary3_phyctg_t cprq_phyctg;
} cpqary3_replyq_t;

typedef struct cpqary3 cpqary3_t;
/*
 * Per Controller Structure
 */
struct cpqary3 {
	dev_info_t		*dip;
	int			cpq_instance;

	cpqary3_init_level_t	cpq_init_level;

	cpqary3_ctlr_mode_t	cpq_ctlr_mode;
	unsigned		cpq_ntargets;
	uint32_t		cpq_board_id;
	cpqary3_bd_t		*cpq_board;

	uint32_t		cpq_host_support;
	uint32_t		cpq_bus_support;
	uint32_t		cpq_maxcmds;
	uint32_t		cpq_sg_cnt;

	/*
	 * Controller Heartbeat Tracking
	 */
	uint32_t		cpq_last_heartbeat;
	clock_t			cpq_last_heartbeat_lbolt;

	uint32_t		cpq_next_tag;

	boolean_t		cpq_intr_off;

	cpqary3_replyq_t	cpq_replyq;
	avl_tree_t		cpq_inflight;
	list_t			cpq_commands;	/* List of all commands. */

	/* Condition Variables used */
	kcondvar_t		cv_immediate_wait;
	kcondvar_t		cv_flushcache_wait;
	kcondvar_t		cv_abort_wait;
	kcondvar_t		cv_ioctl_wait; /* Variable for ioctls */

	ddi_iblock_cookie_t	cpq_int_hw_cookie;
	ddi_iblock_cookie_t	cpq_int_sw_cookie;

	kmutex_t		hw_mutex;
	kmutex_t		sw_mutex;
	ddi_softintr_t		cpqary3_softintr_id;
	boolean_t		cpq_swintr_flag;
	ddi_periodic_t		cpq_periodic;
	uint8_t			cpqary3_tick_hdlr;
	scsi_hba_tran_t		*cpq_hba_tran;
	cpqary3_tgt_t		*cpqary3_tgtp[CPQARY3_MAX_TGT];

	/*
	 * PCI Configuration Registers
	 * 0x10	Primary I2O Memory BAR 	- for Host Interface
	 * 0x14	Primary DRAM 1 BAR	- for Transport Configuration Table
	 *
	 * Host Interface Registers
	 * Offset from Primary I2O Memory BAR
	 * 0x20 Inbound Doorbell	- for interrupting controller
	 * 0x30	Outbound List Status 	- for signalling status of Reply Q
	 * 0x34	Outbound Interrupt Mask	- for masking Interrupts to host
	 * 0x40	Host Inbound Queue	- Request Q
	 * 0x44	Host Outbound Queue	- reply Q
	 *
	 * Offset from Primary DRAM 1 BAR
	 * 0x00	Configuration Table 	- for Controller Transport Layer
	 */

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

	boolean_t		controller_lockup;
	boolean_t		lockup_logged;
	uint32_t		poll_flag;
};

typedef struct cpqary3_command cpqary3_command_t;
typedef struct cpqary3_command_internal cpqary3_command_internal_t;
typedef struct cpqary3_pkt cpqary3_pkt_t;

typedef enum cpqary3_synccmd_status {
	CPQARY3_SYNCCMD_STATUS_NONE = 0,
	CPQARY3_SYNCCMD_STATUS_SUBMITTED,
	CPQARY3_SYNCCMD_STATUS_TIMEOUT
} cpqary3_synccmd_status_t;

typedef enum cpqary3_command_type {
	CPQARY3_CMDTYPE_NONE = 0,
	CPQARY3_CMDTYPE_OS,
	CPQARY3_CMDTYPE_SYNCCMD,
} cpqary3_command_type_t;

struct cpqary3_command {
	uint32_t cpcm_tag;
	cpqary3_command_type_t cpcm_type;

	cpqary3_t *cpcm_ctlr;

	list_node_t cpcm_link;		/* Linkage for allocated list. */
	avl_node_t cpcm_node;		/* Inflight AVL membership. */
	boolean_t cpcm_inflight;
	boolean_t cpcm_error;
	boolean_t cpcm_free_on_complete;
	boolean_t cpcm_used;

	clock_t cpcm_time_submit;

	cpqary3_synccmd_status_t cpcm_synccmd_status;

	cpqary3_pkt_t *cpcm_private;
	cpqary3_command_internal_t *cpcm_internal;

	void (*cpcm_complete)(cpqary3_command_t *);

	/*
	 * Physical allocation tracking:
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


/* cmd_flags */
#define	CFLAG_DMASEND	0x01
#define	CFLAG_CMDIOPB	0x02
#define	CFLAG_DMAVALID	0x04

/*
 * Driver Private Packet
 */
struct cpqary3_pkt {
	struct scsi_pkt		*scsi_cmd_pkt;
	ddi_dma_win_t		prev_winp;
	ddi_dma_seg_t		prev_segp;
	ddi_dma_cookie_t	cmd_dmacookies[MAX_PERF_SG_CNT];
	uint32_t		cmd_ncookies;
	uint32_t		cmd_cookie;
	uint32_t		cmd_cookiecnt;
	uint32_t		cmd_nwin;
	uint32_t		cmd_curwin;
	off_t			cmd_dma_offset;
	size_t			cmd_dma_len;
	size_t			cmd_dmacount;
	struct buf		*bf;
	ddi_dma_handle_t   	cmd_dmahandle;
	uint32_t		bytes;
	uint32_t		cmd_flags;
	uint32_t		cdb_len;
	uint32_t		scb_len;
	cpqary3_command_t	*cmd_command;
};

#pragma pack(1)

typedef struct cpqary3_ioctlresp {
	/* Driver Revision */
	struct cpqary3_revision {
		uint8_t		minor; /* Version */
		uint8_t		major;
		uint8_t		mm;    /* Revision Date */
		uint8_t		dd;
		uint16_t	yyyy;
	} cpqary3_drvrev;

	/* HBA Info */
	struct cpqary3_ctlr {
		uint8_t		num_of_tgts; /* No of Logical Drive */
		uint8_t		*name;
	} cpqary3_ctlr;
} cpqary3_ioctlresp_t;

typedef struct cpqary3_ioctlreq {
	cpqary3_ioctlresp_t	*cpqary3_ioctlrespp;
} cpqary3_ioctlreq_t;

#pragma pack()

/* Driver function definitions */

void cpqary3_init_hbatran(cpqary3_t *);
void cpqary3_periodic(void *);
int cpqary3_flush_cache(cpqary3_t *);
void cpqary3_intr_onoff(cpqary3_t *, uint8_t);
void cpqary3_lockup_intr_onoff(cpqary3_t *, uint8_t);
uint16_t cpqary3_init_ctlr_resource(cpqary3_t *);
int32_t cpqary3_ioctl_driver_info(uintptr_t, int);
int32_t cpqary3_ioctl_ctlr_info(uintptr_t, cpqary3_t *, int);
int32_t cpqary3_ioctl_bmic_pass(uintptr_t, cpqary3_t *, int);
int32_t cpqary3_ioctl_scsi_pass(uintptr_t, cpqary3_t *, int);
uint8_t cpqary3_probe4targets(cpqary3_t *);
int cpqary3_submit(cpqary3_t *, cpqary3_command_t *);
int cpqary3_retrieve(cpqary3_t *, uint32_t, boolean_t *);
void cpqary3_retrieve_simple(cpqary3_t *, uint32_t, boolean_t *);
int cpqary3_target_geometry(struct scsi_address *);
int8_t cpqary3_detect_target_geometry(cpqary3_t *);
uint8_t cpqary3_send_abortcmd(cpqary3_t *, uint16_t, CommandList_t *);
void cpqary3_memfini(cpqary3_t *, uint8_t);
int16_t cpqary3_meminit(cpqary3_t *);
uint8_t cpqary3_build_cmdlist(cpqary3_command_t *cpqary3_cmdpvtp, uint32_t tid);
void cpqary3_lockup_check(cpqary3_t *);
void cpqary3_oscmd_complete(cpqary3_command_t *);

/*
 * Memory management.
 */
caddr_t cpqary3_alloc_phyctgs_mem(cpqary3_t *, size_t, uint32_t *,
    cpqary3_phyctg_t *);
void cpqary3_free_phyctgs_mem(cpqary3_phyctg_t *, uint8_t);

#if 0
void cpqary3_free_phyctgs_mem(cpqary3_phyctg_t *, uint8_t);
caddr_t cpqary3_alloc_phyctgs_mem(cpqary3_t *, size_t, uint32_t *,
    cpqary3_phyctg_t *);
#endif

/*
 * Synchronous command routines.
 */
cpqary3_command_t *cpqary3_synccmd_alloc(cpqary3_t *, size_t);
void cpqary3_synccmd_free(cpqary3_t *, cpqary3_command_t *);
int cpqary3_synccmd_send(cpqary3_t *, cpqary3_command_t *, clock_t, int);
void cpqary3_synccmd_complete(cpqary3_command_t *);

/*
 * Interrupt service routines.
 */
uint32_t cpqary3_isr_hw_simple(caddr_t);
uint32_t cpqary3_isr_sw_simple(caddr_t);
void cpqary3_trigger_sw_isr(cpqary3_t *);
int cpqary3_interrupts_setup(cpqary3_t *);
void cpqary3_interrupts_teardown(cpqary3_t *);

/*
 * Controller initialisation routines.
 */
int cpqary3_ctlr_init(cpqary3_t *);
int cpqary3_ctlr_wait_for_state(cpqary3_t *, cpqary3_wait_state_t);
int cpqary3_ctlr_init_simple(cpqary3_t *);
int cpqary3_cfgtbl_flush(cpqary3_t *);
int cpqary3_cfgtbl_transport_has_support(cpqary3_t *, int);
void cpqary3_cfgtbl_transport_set(cpqary3_t *, int);
int cpqary3_cfgtbl_transport_confirm(cpqary3_t *, int);
uint32_t cpqary3_ctlr_get_cmdsoutmax(cpqary3_t *);

/*
 * Command object management.
 */
cpqary3_command_t *cpqary3_command_alloc(cpqary3_t *, cpqary3_command_type_t);
cpqary3_command_internal_t *cpqary3_command_internal_alloc(cpqary3_t *, size_t);
void cpqary3_command_free(cpqary3_command_t *);
cpqary3_command_t *cpqary3_lookup_inflight(cpqary3_t *, uint32_t);
void cpqary3_command_reuse(cpqary3_command_t *);

/*
 * Device management routines.
 */
uint32_t cpqary3_get32(cpqary3_t *, offset_t);
void cpqary3_put32(cpqary3_t *, offset_t, uint32_t);
int cpqary3_device_setup(cpqary3_t *);
void cpqary3_device_teardown(cpqary3_t *);


#ifdef	__cplusplus
}
#endif

#endif	/* _CPQARY3_H */
