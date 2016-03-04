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
 */

#ifndef	_CPQARY3_SCSI_H
#define	_CPQARY3_SCSI_H

#include <sys/types.h>
#include "cpqary3_ciss.h"

#ifdef	__cplusplus
extern "C" {
#endif

/* CISS LUN Addressing MODEs */
#define	PERIPHERIAL_DEV_ADDR 			0x0
#define	LOGICAL_VOL_ADDR 			0x1
#define	MASK_PERIPHERIAL_DEV_ADDR 		0x3
#define	CISS_PHYS_MODE 				0x0

/*
 * Definitions for compatibility with the old array BMIC interface
 * CISS_OPCODE_RLL IS THE OPCODE FOR THE Report Logical Luns command
 */
#define	CISS_SCMD_ARRAY_READ			0x26
#define	CISS_SCMD_ARRAY_WRITE			0x27

#define	CISS_NEW_READ				0xC0
#define	CISS_NEW_WRITE				0xC1
#define	CISS_NO_TIMEOUT				0x00

/*
 * Vendor-specific SCSI Commands
 *
 * These command opcodes are for use in the first byte of the CDB in a
 * CISS_TYPE_CMD XXX message.  These are essentially SCSI commands, but using
 * the vendor-specific part of the opcode space; i.e., 0xC0 through 0xFF.
 */	
#define	CISS_SCMD_REPORT_LOGICAL_LUNS		0xC2
#define	CISS_SCMD_REPORT_PHYSICAL_LUNS		0xC3

/*
 * BMIC Commands
 *
 * These commands are generally not documented in the OpenCISS specification,
 * but _do_ appear in "Compaq Host-Based PCI Array Controller Firmware
 * Specification" (March 1999).
 *
 * These commands are sent to the controller LUN via CISS_SCMD_ARRAY_WRITE 
 * and (CISS_SCMD_ARRAY_READ? XXX) in a command CDB.
 */
#define	BMIC_FLUSH_CACHE			0xC2
#define	BMIC_IDENTIFY_LOGICAL_DRIVE		0x10
#define	BMIC_SENSE_LOGICAL_DRIVE_STATUS		0x12

#define	CISS_MSG_ABORT				0x0
#define	CISS_ABORT_TASK				0x0
#define	CISS_ABORT_TASKSET			0x1
#define	CISS_CTLR_INIT 				0xffff0000

#define	CISS_MSG_RESET				0x1
#define	CISS_RESET_CTLR				0x0
#define	CISS_RESET_TGT				0x3

/*
 * The Controller SCSI ID is 7. Hence, when ever the OS issues a command
 * for a target with ID greater than 7, the intended Logical Drive is
 * actually one less than the issued ID.
 * So, the allignment.
 * The Mapping from OS to the HBA is as follows:
 *	OS Target IDs		HBA taret IDs
 *		0 - 6				0 - 6
 *		7					- (Controller)
 *		8 - 32				7 - 31
 */
#define	CPQARY3_TGT_ALIGNMENT			0x1

#define	CPQARY3_CDBLEN_12			12
#define	CPQARY3_CDBLEN_16			16

/* Fatal SCSI Status */
#define	SCSI_CHECK_CONDITION			0x2
#define	SCSI_COMMAND_TERMINATED			0x22

#pragma pack(1)

typedef struct cpqary3_report_logical_lun_ent {
	LogDevAddr_t cprle_addr;
} cpqary3_report_logical_lun_ent_t;

typedef struct cpqary3_report_logical_lun_extent {
	LogDevAddr_t cprle_addr;
	uint8_t cprle_wwn[16];
} cpqary3_report_logical_lun_extent_t;

typedef struct cpqary3_report_logical_lun {
	uint32_t cprll_datasize; /* Big Endian */
	uint8_t cprll_extflag;
	uint8_t cprll_reserved1[3];
	union {
		cpqary3_report_logical_lun_ent_t ents[MAX_LOGDRV];
		cpqary3_report_logical_lun_extent_t extents[MAX_LOGDRV];
	} cprll_data;
} cpqary3_report_logical_lun_t;

typedef struct cpqary3_report_logical_lun_req {
	uint8_t cprllr_opcode;
	uint8_t cprllr_extflag;
	uint8_t cprllr_reserved1[4];
	uint32_t cprllr_datasize; /* Big Endian */
	uint8_t cprllr_reserved2;
	uint8_t cprllr_control;
} cpqary3_report_logical_lun_req_t;




typedef struct flushcache {
	uint16_t	disable_flag;
	uint8_t		reserved[510];
} flushcache_buf_t;

typedef struct each_logical_lun_data {
	uint32_t	logical_id:30;
	uint32_t	mode:2;
	uint8_t		reserved[4];
} each_ll_data_t;

typedef struct rll_data {
	uint8_t			lunlist_byte3;
	uint8_t			lunlist_byte2;
	uint8_t			lunlist_byte1;
	uint8_t			lunlist_byte0;
	uint32_t		reserved;
	each_ll_data_t	ll_data[MAX_LOGDRV];
} rll_data_t;

typedef struct each_physical_lun_data {
	uint32_t	    DevID;
	uint32_t	    SecLevel;
} each_pl_data_t;

typedef struct rpl_data {
	uint8_t			lunlist_byte3;
	uint8_t			lunlist_byte2;
	uint8_t			lunlist_byte1;
	uint8_t			lunlist_byte0;
	uint32_t		reserved;
	PhysDevAddr_t	pl_data[CPQARY3_MAX_TGT];
} rpl_data_t;


/*
 * Format of the data returned for the IDENTIFY LOGICAL DRIVE Command
 */
typedef struct Identify_Logical_Drive {
	uint16_t	block_size_in_bytes;
	uint32_t	blocks_available;
	uint16_t	cylinders;
	uint8_t		heads;
	uint8_t		general[11];
	uint8_t		sectors;
	uint8_t		checksum;
	uint8_t		fault_tolerance;
	uint8_t		reserved;
	uint8_t		bios_disable_flag;
	uint8_t		reserved1;
	uint32_t	logical_drive_identifier;
	uint8_t		logical_drive_label[64];
	uint8_t		reserved3[418];
} IdLogDrive;

typedef struct Identify_Ld_Status {
	uint8_t		status;			/* Logical Drive Status */
	uint32_t	failure_map;		/* Drive Failure Map */
	uint16_t	read_error_count[32];	/* read error count */
	uint16_t	write_error_count[32];	/* write error count */
	uint8_t		drive_error_data[256];	/* drive error data */
	uint8_t		drq_time_out_count[32];	/* drq timeout count */
	uint32_t	blocks_left_to_recover;	/* blocks yet to recover */
	uint8_t		drive_recovering;	/* drive recovering */
	uint16_t	remap_count[32];	/* remap count */
	uint32_t	replacement_drive_map;	/* replacement drive map */
	uint32_t	active_spare_map;	/* active spare map */
	uint8_t		spare_status;		/* spare status */
	uint8_t		spare_to_replace_map[32];
	uint32_t	replace_ok_map;		/* Marked ok but no rebuild */
	uint8_t		media_exchanged;	/* Media exchanged (see 0xE0) */
	uint8_t		cache_failure;		/* volume failed cache fail */
	uint8_t		expand_failure;		/* volume failed for failure */
	uint8_t		unit_flags;		/* SMART-2 only */

	/*
	 * The following fields are for firmware supporting > 7 drives per
	 * SCSI bus. The "Drives Per SCSI Bus" indicates how many bits /
	 * words (in case of remap count) correspond to each drive.
	 */
	uint16_t	big_failure_map[8];	/* Big Drive Failure Map */
	uint16_t	big_remap_cnt[128];	/* Big Drive Remap  Count */
	uint16_t	big_replace_map[8];	/* Big Replacement Drive Map */
	uint16_t	big_spare_map[8];	/* Big spare drive map */
	uint8_t		big_spare_replace_map[128]; /* Big spare replace map */
	uint16_t	big_replace_ok_map[8];	/* Big replaced marked OK map */
	uint8_t		big_drive_rebuild;	/* Drive Rebuilding - Drive # */
	uint8_t		reserved[36];
} SenseLdStatus;

#pragma pack()

#ifdef	__cplusplus
}
#endif

#endif	/* _CPQARY3_SCSI_H */
