/*
 *  Copyright (C) 2004 - 2010 Vladislav Bolkhovitin <vst@vlnb.net>
 *  Copyright (C) 2004 - 2005 Leonid Stoljar
 *  Copyright (C) 2006 Nathaniel Clark <nate@misrule.us>
 *  Copyright (C) 2007 - 2010 ID7 Ltd.
 *
 *  Forward port and refactoring to modern qla2xxx and target/configfs
 *
 *  Copyright (C) 2010-2011 Nicholas A. Bellinger <nab@kernel.org>
 *
 *  Additional file for the target driver support.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */
/*
 * This is the global def file that is useful for including from the
 * target portion.
 */

#ifndef __QLA_TARGET_H
#define __QLA_TARGET_H

#include "qla_def.h"

/*
 * Must be changed on any change in any initiator visible interfaces or
 * data in the target add-on
 */
#define QLA2X_TARGET_MAGIC	269

/*
 * Must be changed on any change in any target visible interfaces or
 * data in the initiator
 */
#define QLA2X_INITIATOR_MAGIC   57222

#define QLA2X_INI_MODE_STR_EXCLUSIVE	"exclusive"
#define QLA2X_INI_MODE_STR_DISABLED	"disabled"
#define QLA2X_INI_MODE_STR_ENABLED	"enabled"

#define QLA2X_INI_MODE_EXCLUSIVE	0
#define QLA2X_INI_MODE_DISABLED		1
#define QLA2X_INI_MODE_ENABLED		2

#define QLA2X00_COMMAND_COUNT_INIT	250
#define QLA2X00_IMMED_NOTIFY_COUNT_INIT 250

/*
 * Used to mark which completion handles (for RIO Status's) are for CTIO's
 * vs. regular (non-target) info. This is checked for in
 * qla2x00_process_response_queue() to see if a handle coming back in a
 * multi-complete should come to the tgt driver or be handled there by qla2xxx
 */
#define CTIO_COMPLETION_HANDLE_MARK	BIT_29
#if (CTIO_COMPLETION_HANDLE_MARK <= MAX_OUTSTANDING_COMMANDS)
#error "Hackish CTIO_COMPLETION_HANDLE_MARK no longer larger than MAX_OUTSTANDING_COMMANDS"
#endif
#define HANDLE_IS_CTIO_COMP(h) (h & CTIO_COMPLETION_HANDLE_MARK)

/* Used to mark CTIO as intermediate */
#define CTIO_INTERMEDIATE_HANDLE_MARK	BIT_30

#ifndef OF_SS_MODE_0
/*
 * ISP target entries - Flags bit definitions.
 */
#define OF_SS_MODE_0        0
#define OF_SS_MODE_1        1
#define OF_SS_MODE_2        2
#define OF_SS_MODE_3        3

#define OF_EXPL_CONF        BIT_5       /* Explicit Confirmation Requested */
#define OF_DATA_IN          BIT_6       /* Data in to initiator */
					/*  (data from target to initiator) */
#define OF_DATA_OUT         BIT_7       /* Data out from initiator */
					/*  (data from initiator to target) */
#define OF_NO_DATA          (BIT_7 | BIT_6)
#define OF_INC_RC           BIT_8       /* Increment command resource count */
#define OF_FAST_POST        BIT_9       /* Enable mailbox fast posting. */
#define OF_CONF_REQ         BIT_13      /* Confirmation Requested */
#define OF_TERM_EXCH        BIT_14      /* Terminate exchange */
#define OF_SSTS             BIT_15      /* Send SCSI status */
#endif

#ifndef DATASEGS_PER_COMMAND32
#define DATASEGS_PER_COMMAND32    3
#define DATASEGS_PER_CONT32       7
#define QLA_MAX_SG32(ql) \
   (((ql) > 0) ? (DATASEGS_PER_COMMAND32 + DATASEGS_PER_CONT32*((ql) - 1)) : 0)

#define DATASEGS_PER_COMMAND64    2
#define DATASEGS_PER_CONT64       5
#define QLA_MAX_SG64(ql) \
   (((ql) > 0) ? (DATASEGS_PER_COMMAND64 + DATASEGS_PER_CONT64*((ql) - 1)) : 0)
#endif

#ifndef DATASEGS_PER_COMMAND_24XX
#define DATASEGS_PER_COMMAND_24XX 1
#define DATASEGS_PER_CONT_24XX    5
#define QLA_MAX_SG_24XX(ql) \
   (min(1270, ((ql) > 0) ? (DATASEGS_PER_COMMAND_24XX + DATASEGS_PER_CONT_24XX*((ql) - 1)) : 0))
#endif

/********************************************************************\
 * ISP Queue types left out of new QLogic driver (from old version)
\********************************************************************/

#ifndef ENABLE_LUN_TYPE
#define ENABLE_LUN_TYPE 0x0B		/* Enable LUN entry. */
/*
 * ISP queue - enable LUN entry structure definition.
 */
typedef struct {
	uint8_t	 entry_type;		/* Entry type. */
	uint8_t	 entry_count;		/* Entry count. */
	uint8_t	 sys_define;		/* System defined. */
	uint8_t	 entry_status;		/* Entry Status. */
	uint32_t sys_define_2;		/* System defined. */
	uint8_t	 reserved_8;
	uint8_t	 reserved_1;
	uint16_t reserved_2;
	uint32_t reserved_3;
	uint8_t	 status;
	uint8_t	 reserved_4;
	uint8_t	 command_count;		/* Number of ATIOs allocated. */
	uint8_t	 immed_notify_count;	/* Number of Immediate Notify entries allocated. */
	uint16_t reserved_5;
	uint16_t timeout;		/* 0 = 30 seconds, 0xFFFF = disable */
	uint16_t reserved_6[20];
} __attribute__((packed)) elun_entry_t;
#define ENABLE_LUN_SUCCESS          0x01
#define ENABLE_LUN_RC_NONZERO       0x04
#define ENABLE_LUN_INVALID_REQUEST  0x06
#define ENABLE_LUN_ALREADY_ENABLED  0x3E
#endif

#ifndef MODIFY_LUN_TYPE
#define MODIFY_LUN_TYPE 0x0C	  /* Modify LUN entry. */
/*
 * ISP queue - modify LUN entry structure definition.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t sys_define_2;		    /* System defined. */
	uint8_t	 reserved_8;
	uint8_t	 reserved_1;
	uint8_t	 operators;
	uint8_t	 reserved_2;
	uint32_t reserved_3;
	uint8_t	 status;
	uint8_t	 reserved_4;
	uint8_t	 command_count;		    /* Number of ATIOs allocated. */
	uint8_t	 immed_notify_count;	    /* Number of Immediate Notify */
	/* entries allocated. */
	uint16_t reserved_5;
	uint16_t timeout;		    /* 0 = 30 seconds, 0xFFFF = disable */
	uint16_t reserved_7[20];
} __attribute__((packed)) modify_lun_entry_t;
#define MODIFY_LUN_SUCCESS	0x01
#define MODIFY_LUN_CMD_ADD BIT_0
#define MODIFY_LUN_CMD_SUB BIT_1
#define MODIFY_LUN_IMM_ADD BIT_2
#define MODIFY_LUN_IMM_SUB BIT_3
#endif

#define GET_TARGET_ID(ha, iocb) ((HAS_EXTENDED_IDS(ha))			\
				 ? le16_to_cpu((iocb)->target.extended)	\
				 : (uint16_t)(iocb)->target.id.standard)

#ifndef IMMED_NOTIFY_TYPE
#define IMMED_NOTIFY_TYPE 0x0D		/* Immediate notify entry. */
/*
 * ISP queue - immediate notify entry structure definition.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t sys_define_2;		    /* System defined. */
	target_id_t target;
	uint16_t lun;
	uint8_t  target_id;
	uint8_t  reserved_1;
	uint16_t status_modifier;
	uint16_t status;
	uint16_t task_flags;
	uint16_t seq_id;
	uint16_t srr_rx_id;
	uint32_t srr_rel_offs;
	uint16_t srr_ui;
#define SRR_IU_DATA_IN		0x1
#define SRR_IU_DATA_OUT		0x5
#define SRR_IU_STATUS		0x7
	uint16_t srr_ox_id;
	uint8_t reserved_2[30];
	uint16_t ox_id;
} __attribute__((packed)) notify_entry_t;
#endif

#ifndef NOTIFY_ACK_TYPE
#define NOTIFY_ACK_TYPE 0x0E	  /* Notify acknowledge entry. */
/*
 * ISP queue - notify acknowledge entry structure definition.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t sys_define_2;		    /* System defined. */
	target_id_t target;
	uint8_t	 target_id;
	uint8_t	 reserved_1;
	uint16_t flags;
	uint16_t resp_code;
	uint16_t status;
	uint16_t task_flags;
	uint16_t seq_id;
	uint16_t srr_rx_id;
	uint32_t srr_rel_offs;
	uint16_t srr_ui;
	uint16_t srr_flags;
	uint16_t srr_reject_code;
	uint8_t  srr_reject_vendor_uniq;
	uint8_t  srr_reject_code_expl;
	uint8_t  reserved_2[26];
	uint16_t ox_id;
} __attribute__((packed)) nack_entry_t;
#define NOTIFY_ACK_SRR_FLAGS_ACCEPT	0
#define NOTIFY_ACK_SRR_FLAGS_REJECT	1

#define NOTIFY_ACK_SRR_REJECT_REASON_UNABLE_TO_PERFORM	0x9

#define NOTIFY_ACK_SRR_FLAGS_REJECT_EXPL_NO_EXPL		0
#define NOTIFY_ACK_SRR_FLAGS_REJECT_EXPL_UNABLE_TO_SUPPLY_DATA	0x2a

#define NOTIFY_ACK_SUCCESS      0x01
#endif

#ifndef ACCEPT_TGT_IO_TYPE
#define ACCEPT_TGT_IO_TYPE 0x16 /* Accept target I/O entry. */
/*
 * ISP queue - Accept Target I/O (ATIO) entry structure definition.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t sys_define_2;		    /* System defined. */
	target_id_t target;
	uint16_t rx_id;
	uint16_t flags;
	uint16_t status;
	uint8_t	 command_ref;
	uint8_t	 task_codes;
	uint8_t	 task_flags;
	uint8_t	 execution_codes;
	uint8_t	 cdb[MAX_CMDSZ];
	uint32_t data_length;
	uint16_t lun;
	uint8_t  initiator_port_name[WWN_SIZE]; /* on qla23xx */
	uint16_t reserved_32[6];
	uint16_t ox_id;
} __attribute__((packed)) atio_entry_t;
#endif

#ifndef CONTINUE_TGT_IO_TYPE
#define CONTINUE_TGT_IO_TYPE 0x17
/*
 * ISP queue - Continue Target I/O (CTIO) entry for status mode 0
 *	       structure definition.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;		    /* System defined handle */
	target_id_t target;
	uint16_t rx_id;
	uint16_t flags;
	uint16_t status;
	uint16_t timeout;		    /* 0 = 30 seconds, 0xFFFF = disable */
	uint16_t dseg_count;		    /* Data segment count. */
	uint32_t relative_offset;
	uint32_t residual;
	uint16_t reserved_1[3];
	uint16_t scsi_status;
	uint32_t transfer_length;
	uint32_t dseg_0_address[0];
} __attribute__((packed)) ctio_common_entry_t;
#define ATIO_PATH_INVALID       0x07
#define ATIO_CANT_PROV_CAP      0x16
#define ATIO_CDB_VALID          0x3D

#define ATIO_EXEC_READ          BIT_1
#define ATIO_EXEC_WRITE         BIT_0
#endif

#ifndef CTIO_A64_TYPE
#define CTIO_A64_TYPE 0x1F
typedef struct {
	ctio_common_entry_t common;
	uint32_t dseg_0_address;	    /* Data segment 0 address. */
	uint32_t dseg_0_length;		    /* Data segment 0 length. */
	uint32_t dseg_1_address;	    /* Data segment 1 address. */
	uint32_t dseg_1_length;		    /* Data segment 1 length. */
	uint32_t dseg_2_address;	    /* Data segment 2 address. */
	uint32_t dseg_2_length;		    /* Data segment 2 length. */
} __attribute__((packed)) ctio_entry_t;
#define CTIO_SUCCESS			0x01
#define CTIO_ABORTED			0x02
#define CTIO_INVALID_RX_ID		0x08
#define CTIO_TIMEOUT			0x0B
#define CTIO_LIP_RESET			0x0E
#define CTIO_TARGET_RESET		0x17
#define CTIO_PORT_UNAVAILABLE		0x28
#define CTIO_PORT_LOGGED_OUT		0x29
#define CTIO_PORT_CONF_CHANGED		0x2A
#define CTIO_SRR_RECEIVED		0x45

#endif

#ifndef CTIO_RET_TYPE
#define CTIO_RET_TYPE	0x17		/* CTIO return entry */
/*
 * ISP queue - CTIO returned entry structure definition.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;		    /* System defined handle. */
	target_id_t target;
	uint16_t rx_id;
	uint16_t flags;
	uint16_t status;
	uint16_t timeout;	    /* 0 = 30 seconds, 0xFFFF = disable */
	uint16_t dseg_count;	    /* Data segment count. */
	uint32_t relative_offset;
	uint32_t residual;
	uint16_t reserved_1[2];
	uint16_t sense_length;
	uint16_t scsi_status;
	uint16_t response_length;
	uint8_t	 sense_data[26];
} __attribute__((packed)) ctio_ret_entry_t;
#endif

#define ATIO_TYPE7 0x06 /* Accept target I/O entry for 24xx */

typedef struct {
	uint8_t  r_ctl;
	uint8_t  d_id[3];
	uint8_t  cs_ctl;
	uint8_t  s_id[3];
	uint8_t  type;
	uint8_t  f_ctl[3];
	uint8_t  seq_id;
	uint8_t  df_ctl;
	uint16_t seq_cnt;
	uint16_t ox_id;
	uint16_t rx_id;
	uint32_t parameter;
} __attribute__((packed)) fcp_hdr_t;

typedef struct {
	uint8_t  d_id[3];
	uint8_t  r_ctl;
	uint8_t  s_id[3];
	uint8_t  cs_ctl;
	uint8_t  f_ctl[3];
	uint8_t  type;
	uint16_t seq_cnt;
	uint8_t  df_ctl;
	uint8_t  seq_id;
	uint16_t rx_id;
	uint16_t ox_id;
	uint32_t parameter;
} __attribute__((packed)) fcp_hdr_le_t;

#define F_CTL_EXCH_CONTEXT_RESP	BIT_23
#define F_CTL_SEQ_CONTEXT_RESIP	BIT_22
#define F_CTL_LAST_SEQ		BIT_20
#define F_CTL_END_SEQ		BIT_19
#define F_CTL_SEQ_INITIATIVE	BIT_16

#define R_CTL_BASIC_LINK_SERV	0x80
#define R_CTL_B_ACC		0x4
#define R_CTL_B_RJT		0x5

typedef struct {
	uint64_t lun;
	uint8_t  cmnd_ref;
	uint8_t  task_attr:3;
	uint8_t  reserved:5;
	uint8_t  task_mgmt_flags;
#define FCP_CMND_TASK_MGMT_CLEAR_ACA		6
#define FCP_CMND_TASK_MGMT_TARGET_RESET		5
#define FCP_CMND_TASK_MGMT_LU_RESET		4
#define FCP_CMND_TASK_MGMT_CLEAR_TASK_SET	2
#define FCP_CMND_TASK_MGMT_ABORT_TASK_SET	1
	uint8_t  wrdata:1;
	uint8_t  rddata:1;
	uint8_t  add_cdb_len:6;
	uint8_t  cdb[16];
	/*
	 * add_cdb is optional and can absent from atio7_fcp_cmnd_t. Size 4 only to
	 * make sizeof(atio7_fcp_cmnd_t) be as expected by BUILD_BUG_ON() in
	 * qla_tgt_init().
	 */
	uint8_t  add_cdb[4];
	/* uint32_t data_length; */
} __attribute__((packed)) atio7_fcp_cmnd_t;

/*
 * ISP queue - Accept Target I/O (ATIO) type 7 entry for 24xx structure
 * definition.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t  fcp_cmnd_len_low;
	uint8_t  fcp_cmnd_len_high:4;
	uint8_t  attr:4;
	uint32_t exchange_addr;
#define ATIO_EXCHANGE_ADDRESS_UNKNOWN		0xFFFFFFFF
	fcp_hdr_t fcp_hdr;
	atio7_fcp_cmnd_t fcp_cmnd;
} __attribute__((packed)) atio7_entry_t;

#define CTIO_TYPE7 0x12 /* Continue target I/O entry (for 24xx) */

/*
 * ISP queue - Continue Target I/O (ATIO) type 7 entry (for 24xx) structure
 * definition.
 */

typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;		    /* System defined handle */
	uint16_t nport_handle;
#define CTIO7_NHANDLE_UNRECOGNIZED	0xFFFF
	uint16_t timeout;
	uint16_t dseg_count;		    /* Data segment count. */
	uint8_t  vp_index;
	uint8_t  add_flags;
	uint8_t  initiator_id[3];
	uint8_t  reserved;
	uint32_t exchange_addr;
} __attribute__((packed)) ctio7_common_entry_t;

typedef struct {
	ctio7_common_entry_t common;
	uint16_t reserved1;
	uint16_t flags;
	uint32_t residual;
	uint16_t ox_id;
	uint16_t scsi_status;
	uint32_t relative_offset;
	uint32_t reserved2;
	uint32_t transfer_length;
	uint32_t reserved3;
	uint32_t dseg_0_address[2];	    /* Data segment 0 address. */
	uint32_t dseg_0_length;		    /* Data segment 0 length. */
} __attribute__((packed)) ctio7_status0_entry_t;

typedef struct {
	ctio7_common_entry_t common;
	uint16_t sense_length;
	uint16_t flags;
	uint32_t residual;
	uint16_t ox_id;
	uint16_t scsi_status;
	uint16_t response_len;
	uint16_t reserved;
	uint8_t sense_data[24];
} __attribute__((packed)) ctio7_status1_entry_t;

typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;		    /* System defined handle */
	uint16_t status;
	uint16_t timeout;
	uint16_t dseg_count;		    /* Data segment count. */
	uint8_t  vp_index;
	uint8_t  reserved1[5];
	uint32_t exchange_address;
	uint16_t reserved2;
	uint16_t flags;
	uint32_t residual;
	uint16_t ox_id;
	uint16_t reserved3;
	uint32_t relative_offset;
	uint8_t  reserved4[24];
} __attribute__((packed)) ctio7_fw_entry_t;

/* CTIO7 flags values */
#define CTIO7_FLAGS_SEND_STATUS		BIT_15
#define CTIO7_FLAGS_TERMINATE		BIT_14
#define CTIO7_FLAGS_CONFORM_REQ		BIT_13
#define CTIO7_FLAGS_DONT_RET_CTIO	BIT_8
#define CTIO7_FLAGS_STATUS_MODE_0	0
#define CTIO7_FLAGS_STATUS_MODE_1	BIT_6
#define CTIO7_FLAGS_EXPLICIT_CONFORM	BIT_5
#define CTIO7_FLAGS_CONFIRM_SATISF	BIT_4
#define CTIO7_FLAGS_DSD_PTR		BIT_2
#define CTIO7_FLAGS_DATA_IN		BIT_1
#define CTIO7_FLAGS_DATA_OUT		BIT_0

/*
 * ISP queue - immediate notify entry structure definition for 24xx.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t reserved;
	uint16_t nport_handle;
	uint16_t reserved_2;
	uint16_t flags;
#define NOTIFY24XX_FLAGS_GLOBAL_TPRLO	BIT_1
#define NOTIFY24XX_FLAGS_PUREX_IOCB	BIT_0
	uint16_t srr_rx_id;
	uint16_t status;
	uint8_t  status_subcode;
	uint8_t  reserved_3;
	uint32_t exchange_address;
	uint32_t srr_rel_offs;
	uint16_t srr_ui;
	uint16_t srr_ox_id;
	uint8_t  reserved_4[19];
	uint8_t  vp_index;
	uint32_t reserved_5;
	uint8_t  port_id[3];
	uint8_t  reserved_6;
	uint16_t reserved_7;
	uint16_t ox_id;
} __attribute__((packed)) notify24xx_entry_t;

#define ELS_PLOGI			0x3
#define ELS_FLOGI			0x4
#define ELS_LOGO			0x5
#define ELS_PRLI			0x20
#define ELS_PRLO			0x21
#define ELS_TPRLO			0x24
#define ELS_PDISC			0x50
#define ELS_ADISC			0x52

/*
 * ISP queue - notify acknowledge entry structure definition for 24xx.
 */
typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;
	uint16_t nport_handle;
	uint16_t reserved_1;
	uint16_t flags;
	uint16_t srr_rx_id;
	uint16_t status;
	uint8_t  status_subcode;
	uint8_t  reserved_3;
	uint32_t exchange_address;
	uint32_t srr_rel_offs;
	uint16_t srr_ui;
	uint16_t srr_flags;
	uint8_t  reserved_4[19];
	uint8_t  vp_index;
	uint8_t  srr_reject_vendor_uniq;
	uint8_t  srr_reject_code_expl;
	uint8_t  srr_reject_code;
	uint8_t  reserved_5[7];
	uint16_t ox_id;
} __attribute__((packed)) nack24xx_entry_t;

/*
 * ISP queue - ABTS received/response entries structure definition for 24xx.
 */
#define ABTS_RECV_24XX		0x54 /* ABTS received (for 24xx) */
#define ABTS_RESP_24XX		0x55 /* ABTS responce (for 24xx) */

typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint8_t  reserved_1[6];
	uint16_t nport_handle;
	uint8_t  reserved_2[2];
	uint8_t  vp_index;
	uint8_t  reserved_3:4;
	uint8_t  sof_type:4;
	uint32_t exchange_address;
	fcp_hdr_le_t fcp_hdr_le;
	uint8_t  reserved_4[16];
	uint32_t exchange_addr_to_abort;
} __attribute__((packed)) abts24_recv_entry_t;

#define ABTS_PARAM_ABORT_SEQ		BIT_0

typedef struct {
	uint16_t reserved;
	uint8_t  seq_id_last;
	uint8_t  seq_id_valid;
#define SEQ_ID_VALID	0x80
#define SEQ_ID_INVALID	0x00
	uint16_t rx_id;
	uint16_t ox_id;
	uint16_t high_seq_cnt;
	uint16_t low_seq_cnt;
} __attribute__((packed)) ba_acc_le_t;

typedef struct {
	uint8_t vendor_uniq;
	uint8_t reason_expl;
	uint8_t reason_code;
#define BA_RJT_REASON_CODE_INVALID_COMMAND	0x1
#define BA_RJT_REASON_CODE_UNABLE_TO_PERFORM	0x9
	uint8_t reserved;
} __attribute__((packed)) ba_rjt_le_t;

typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;
	uint16_t reserved_1;
	uint16_t nport_handle;
	uint16_t control_flags;
#define ABTS_CONTR_FLG_TERM_EXCHG	BIT_0
	uint8_t  vp_index;
	uint8_t  reserved_3:4;
	uint8_t  sof_type:4;
	uint32_t exchange_address;
	fcp_hdr_le_t fcp_hdr_le;
	union {
		ba_acc_le_t ba_acct;
		ba_rjt_le_t ba_rjt;
	} __attribute__((packed)) payload;
	uint32_t reserved_4;
	uint32_t exchange_addr_to_abort;
} __attribute__((packed)) abts24_resp_entry_t;

typedef struct {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;
	uint16_t compl_status;
#define ABTS_RESP_COMPL_SUCCESS		0
#define ABTS_RESP_COMPL_SUBCODE_ERROR	0x31
	uint16_t nport_handle;
	uint16_t reserved_1;
	uint8_t  reserved_2;
	uint8_t  reserved_3:4;
	uint8_t  sof_type:4;
	uint32_t exchange_address;
	fcp_hdr_le_t fcp_hdr_le;
	uint8_t reserved_4[8];
	uint32_t error_subcode1;
#define ABTS_RESP_SUBCODE_ERR_ABORTED_EXCH_NOT_TERM	0x1E
	uint32_t error_subcode2;
	uint32_t exchange_addr_to_abort;
} __attribute__((packed)) abts24_resp_fw_entry_t;

/********************************************************************\
 * Type Definitions used by initiator & target halves
\********************************************************************/

struct qla_tgt_mgmt_cmd;
struct qla_tgt_sess;

struct qla_target_template {

	int (*handle_cmd)(struct scsi_qla_host *, struct qla_tgt_cmd *, uint32_t,
			uint32_t, int, int, int);
	int (*handle_data)(struct qla_tgt_cmd *);
	int (*handle_tmr)(struct qla_tgt_mgmt_cmd *, uint32_t, uint8_t);
	void (*free_cmd)(struct qla_tgt_cmd *);
	void (*free_session)(struct qla_tgt_sess *);

	int (*check_initiator_node_acl)(struct scsi_qla_host *, unsigned char *,
					void *, uint8_t *, uint16_t);
	struct qla_tgt_sess *(*find_sess_by_loop_id)(struct scsi_qla_host *,
						const uint16_t);
	struct qla_tgt_sess *(*find_sess_by_s_id)(struct scsi_qla_host *,
						const uint8_t *);
};

int qla2x00_wait_for_hba_online(struct scsi_qla_host *);

#include <target/target_core_base.h>

#define QLA_TGT_TIMEOUT			10	/* in seconds */

#define QLA_TGT_MAX_HW_PENDING_TIME	60 /* in seconds */

/* Immediate notify status constants */
#define IMM_NTFY_LIP_RESET          0x000E
#define IMM_NTFY_LIP_LINK_REINIT    0x000F
#define IMM_NTFY_IOCB_OVERFLOW      0x0016
#define IMM_NTFY_ABORT_TASK         0x0020
#define IMM_NTFY_PORT_LOGOUT        0x0029
#define IMM_NTFY_PORT_CONFIG        0x002A
#define IMM_NTFY_GLBL_TPRLO         0x002D
#define IMM_NTFY_GLBL_LOGO          0x002E
#define IMM_NTFY_RESOURCE           0x0034
#define IMM_NTFY_MSG_RX             0x0036
#define IMM_NTFY_SRR                0x0045
#define IMM_NTFY_ELS                0x0046

/* Immediate notify task flags */
#define IMM_NTFY_TASK_MGMT_SHIFT    8

#define QLA_TGT_CLEAR_ACA               0x40
#define QLA_TGT_TARGET_RESET            0x20
#define QLA_TGT_LUN_RESET               0x10
#define QLA_TGT_CLEAR_TS                0x04
#define QLA_TGT_ABORT_TS                0x02
#define QLA_TGT_ABORT_ALL_SESS          0xFFFF
#define QLA_TGT_ABORT_ALL               0xFFFE
#define QLA_TGT_NEXUS_LOSS_SESS         0xFFFD
#define QLA_TGT_NEXUS_LOSS              0xFFFC

/* Notify Acknowledge flags */
#define NOTIFY_ACK_RES_COUNT        BIT_8
#define NOTIFY_ACK_CLEAR_LIP_RESET  BIT_5
#define NOTIFY_ACK_TM_RESP_CODE_VALID BIT_4

/* Command's states */
#define QLA_TGT_STATE_NEW               0	/* New command and target processing it */
#define QLA_TGT_STATE_NEED_DATA         1	/* target needs data to continue */
#define QLA_TGT_STATE_DATA_IN           2	/* Data arrived and target is processing */
#define QLA_TGT_STATE_PROCESSED         3	/* target done processing */
#define QLA_TGT_STATE_ABORTED           4	/* Command aborted */

/* Special handles */
#define QLA_TGT_NULL_HANDLE             0
#define QLA_TGT_SKIP_HANDLE             (0xFFFFFFFF & ~CTIO_COMPLETION_HANDLE_MARK)

/* ATIO task_codes field */
#define ATIO_SIMPLE_QUEUE           0
#define ATIO_HEAD_OF_QUEUE          1
#define ATIO_ORDERED_QUEUE          2
#define ATIO_ACA_QUEUE              4
#define ATIO_UNTAGGED               5

/* TM failed response codes, see FCP (9.4.11 FCP_RSP_INFO) */
#define	FC_TM_SUCCESS               0
#define	FC_TM_BAD_FCP_DATA          1
#define	FC_TM_BAD_CMD               2
#define	FC_TM_FCP_DATA_MISMATCH     3
#define	FC_TM_REJECT                4
#define FC_TM_FAILED                5

/*
 * Error code of qla_tgt_pre_xmit_response() meaning that cmd's exchange was
 * terminated, so no more actions is needed and success should be returned
 * to target.
 */
#define QLA_TGT_PRE_XMIT_RESP_CMD_ABORTED	0x1717

#if (BITS_PER_LONG > 32) || defined(CONFIG_HIGHMEM64G)
#define pci_dma_lo32(a) (a & 0xffffffff)
#define pci_dma_hi32(a) ((((a) >> 16)>>16) & 0xffffffff)
#else
#define pci_dma_lo32(a) (a & 0xffffffff)
#define pci_dma_hi32(a) 0
#endif

#define QLA_TGT_SENSE_VALID(sense)  ((sense != NULL) && \
				(((const uint8_t *)(sense))[0] & 0x70) == 0x70)

struct qla_port23_data {
	uint8_t port_name[WWN_SIZE];
	uint16_t loop_id;
};

struct qla_port24_data {
	uint8_t port_name[WWN_SIZE];
	uint16_t loop_id;
	uint16_t reserved;
};

struct qla_tgt {
	struct scsi_qla_host *vha;
	struct qla_hw_data *ha;

	/*
	 * To sync between IRQ handlers and qla_tgt_target_release(). Needed,
	 * because req_pkt() can drop/reaquire HW lock inside. Protected by
	 * HW lock.
	 */
	int irq_cmd_count;

	int datasegs_per_cmd, datasegs_per_cont;

	/* Target's flags, serialized by pha->hardware_lock */
	unsigned int tgt_enable_64bit_addr:1;	/* 64-bits PCI addressing enabled */
	unsigned int link_reinit_iocb_pending:1;
	unsigned int tm_to_unknown:1; /* TM to unknown session was sent */
	unsigned int sess_works_pending:1; /* there are sess_work entries */

	/*
	 * Protected by tgt_mutex AND hardware_lock for writing and tgt_mutex
	 * OR hardware_lock for reading.
	 */
	int tgt_stop; /* the target mode driver is being stopped */
	int tgt_stopped; /* the target mode driver has been stopped */

	/* Count of sessions refering qla_tgt. Protected by hardware_lock. */
	int sess_count;

	/* Protected by hardware_lock. Addition also protected by tgt_mutex. */
	struct list_head sess_list;

	/* Protected by hardware_lock */
	struct list_head del_sess_list;
	struct delayed_work sess_del_work;

	spinlock_t sess_work_lock;
	struct list_head sess_works_list;
	struct work_struct sess_work;

	notify24xx_entry_t link_reinit_iocb;
	wait_queue_head_t waitQ;
	int notify_ack_expected;
	int abts_resp_expected;
	int modify_lun_expected;

	int ctio_srr_id;
	int imm_srr_id;
	spinlock_t srr_lock;
	struct list_head srr_ctio_list;
	struct list_head srr_imm_list;
	struct work_struct srr_work;

	atomic_t tgt_global_resets_count;

	struct list_head tgt_list_entry;
};

/*
 * Equivilant to IT Nexus (Initiator-Target)
 */
struct qla_tgt_sess {
	uint16_t loop_id;
	port_id_t s_id;

	unsigned int conf_compl_supported:1;
	unsigned int deleted:1;
	unsigned int local:1;

	struct se_session *se_sess;
	struct scsi_qla_host *vha;
	struct qla_tgt *tgt;

	int sess_ref; /* protected by hardware_lock */

	struct list_head sess_list_entry;
	unsigned long expires;
	struct list_head del_list_entry;

	uint8_t port_name[WWN_SIZE];
};

struct qla_tgt_cmd {
	struct qla_tgt_sess *sess;
	int state;
	int locked_rsp;
	atomic_t cmd_stop_free;
	struct se_cmd se_cmd;
	/* Sense buffer that will be mapped into outgoing status */
	unsigned char sense_buffer[TRANSPORT_SENSE_BUFFER];

	unsigned int conf_compl_supported:1;/* to save extra sess dereferences */
	unsigned int sg_mapped:1;
	unsigned int free_sg:1;
	unsigned int aborted:1; /* Needed in case of SRR */
	unsigned int write_data_transferred:1;

	struct scatterlist *sg;	/* cmd data buffer SG vector */
	int sg_cnt;		/* SG segments count */
	int bufflen;		/* cmd buffer length */
	int offset;
	uint32_t tag;
	dma_addr_t dma_handle;
	enum dma_data_direction dma_data_direction;

	uint16_t loop_id;		    /* to save extra sess dereferences */
	struct qla_tgt *tgt;		    /* to save extra sess dereferences */
	struct scsi_qla_host *vha;

	union {
		atio7_entry_t atio7;
		atio_entry_t atio2x;
	} __attribute__((packed)) atio;
};

struct qla_tgt_sess_work_param {
	struct list_head sess_works_list_entry;

#define QLA_TGT_SESS_WORK_CMD	0
#define QLA_TGT_SESS_WORK_ABORT	1
#define QLA_TGT_SESS_WORK_TM	2
	int type;

	union {
		struct qla_tgt_cmd *cmd;
		abts24_recv_entry_t abts;
		notify_entry_t tm_iocb;
		atio7_entry_t tm_iocb2;
	};
};

struct qla_tgt_mgmt_cmd {
	uint8_t tmr_func;
	uint8_t fc_tm_rsp;
	struct qla_tgt_sess *sess;
	struct se_cmd se_cmd;
	struct se_tmr_req *se_tmr_req;
	unsigned int flags;
#define Q24_MGMT_SEND_NACK	1
	union {
		atio7_entry_t atio7;
		notify_entry_t notify_entry;
		notify24xx_entry_t notify_entry24;
		abts24_recv_entry_t abts;
	} __attribute__((packed)) orig_iocb;
};

struct qla_tgt_prm {
	struct qla_tgt_cmd *cmd;
	struct qla_tgt *tgt;
	void *pkt;
	struct scatterlist *sg;	/* cmd data buffer SG vector */
	int seg_cnt;
	int req_cnt;
	uint16_t rq_result;
	uint16_t scsi_status;
	unsigned char *sense_buffer;
	int sense_buffer_len;
	int residual;
	int add_status_pkt;
};

struct srr_imm {
	struct list_head srr_list_entry;
	int srr_id;
	union {
		notify_entry_t notify_entry;
		notify24xx_entry_t notify_entry24;
	} __attribute__((packed)) imm;
};

struct srr_ctio {
	struct list_head srr_list_entry;
	int srr_id;
	struct qla_tgt_cmd *cmd;
};

#define QLA_TGT_XMIT_DATA		1
#define QLA_TGT_XMIT_STATUS		2
#define QLA_TGT_XMIT_ALL		(QLA_TGT_XMIT_STATUS|QLA_TGT_XMIT_DATA)

#include <linux/version.h>

extern struct qla_tgt_data qla_target;
/*
 * Internal function prototypes
 */
void qla_tgt_disable_vha(struct scsi_qla_host *);

/*
 * Function prototypes for qla_target.c logic used by qla2xxx LLD code.
 */
extern int qla_tgt_add_target(struct qla_hw_data *, struct scsi_qla_host *);
extern int qla_tgt_remove_target(struct qla_hw_data *, struct scsi_qla_host *);
extern void qla_tgt_fc_port_added(struct scsi_qla_host *, fc_port_t *);
extern void qla_tgt_fc_port_deleted(struct scsi_qla_host *, fc_port_t *);
extern void qla_tgt_set_mode(struct scsi_qla_host *ha);
extern void qla_tgt_clear_mode(struct scsi_qla_host *ha);
extern int __init qla_tgt_init(void);
extern void __exit qla_tgt_exit(void);

static inline bool qla_tgt_mode_enabled(struct scsi_qla_host *ha)
{
	return ha->host->active_mode & MODE_TARGET;
}

static inline bool qla_ini_mode_enabled(struct scsi_qla_host *ha)
{
	return ha->host->active_mode & MODE_INITIATOR;
}

static inline void qla_reverse_ini_mode(struct scsi_qla_host *ha)
{
	if (ha->host->active_mode & MODE_INITIATOR)
		ha->host->active_mode &= ~MODE_INITIATOR;
	else
		ha->host->active_mode |= MODE_INITIATOR;
}

/********************************************************************\
 * ISP Queue types left out of new QLogic driver (from old version)
\********************************************************************/

/*
 * qla2x00_do_en_dis_lun
 *	Issue enable or disable LUN entry IOCB.
 *
 * Input:
 *	ha = adapter block pointer.
 *
 * Caller MUST have hardware lock held. This function might release it,
 * then reaquire.
 */
static inline void
__qla2x00_send_enable_lun(struct scsi_qla_host *vha, int enable)
{
	elun_entry_t *pkt;
	struct qla_hw_data *ha = vha->hw;

	BUG_ON(IS_FWI2_CAPABLE(ha));

	pkt = (elun_entry_t *)qla2x00_alloc_iocbs(vha, 0);
	if (pkt != NULL) {
		pkt->entry_type = ENABLE_LUN_TYPE;
		if (enable) {
			pkt->command_count = QLA2X00_COMMAND_COUNT_INIT;
			pkt->immed_notify_count = QLA2X00_IMMED_NOTIFY_COUNT_INIT;
			pkt->timeout = 0xffff;
		} else {
			pkt->command_count = 0;
			pkt->immed_notify_count = 0;
			pkt->timeout = 0;
		}
		DEBUG2(printk(KERN_DEBUG
			      "scsi%lu:ENABLE_LUN IOCB imm %u cmd %u timeout %u\n",
			      vha->host_no, pkt->immed_notify_count,
			      pkt->command_count, pkt->timeout));

		/* Issue command to ISP */
		qla2x00_isp_cmd(vha, vha->req);

	} else
		qla_tgt_clear_mode(vha);
#if defined(QL_DEBUG_LEVEL_2) || defined(QL_DEBUG_LEVEL_3)
	if (!pkt)
		printk(KERN_ERR "%s: **** FAILED ****\n", __func__);
#endif

	return;
}

/*
 * qla2x00_send_enable_lun
 *      Issue enable LUN entry IOCB.
 *
 * Input:
 *      ha = adapter block pointer.
 *	enable = enable/disable flag.
 */
static inline void
qla2x00_send_enable_lun(struct scsi_qla_host *vha, bool enable)
{
	struct qla_hw_data *ha = vha->hw;

	if (!IS_FWI2_CAPABLE(ha)) {
		unsigned long flags;
		spin_lock_irqsave(&ha->hardware_lock, flags);
		__qla2x00_send_enable_lun(vha, enable);
		spin_unlock_irqrestore(&ha->hardware_lock, flags);
	}
}
/*
 * Exported symbols from qla_target.c LLD logic used by tcm_qla2xxx code..
 */
extern void qla24xx_atio_pkt_all_vps(struct scsi_qla_host *, atio7_entry_t *);
extern void qla_tgt_response_pkt_all_vps(struct scsi_qla_host *, response_t *);
extern int qla_tgt_rdy_to_xfer(struct qla_tgt_cmd *);
extern int qla2xxx_xmit_response(struct qla_tgt_cmd *, int, uint8_t);
extern void qla_tgt_xmit_tm_rsp(struct qla_tgt_mgmt_cmd *);
extern void qla_tgt_free_mcmd(struct qla_tgt_mgmt_cmd *);
extern void qla_tgt_free_cmd(struct qla_tgt_cmd *cmd);
extern void qla_tgt_sess_put(struct qla_tgt_sess *);
extern void qla_tgt_ctio_completion(struct scsi_qla_host *, uint32_t);
extern void qla_tgt_async_event(uint16_t, struct scsi_qla_host *, uint16_t *);
extern void qla_tgt_enable_vha(struct scsi_qla_host *);
extern void qla_tgt_vport_create(struct scsi_qla_host *, struct qla_hw_data *);
extern void qla_tgt_rff_id(struct scsi_qla_host *, struct ct_sns_req *);
extern void qla_tgt_initialize_adapter(struct scsi_qla_host *, struct qla_hw_data *);
extern void qla_tgt_init_atio_q_entries(struct scsi_qla_host *);
extern void qla_tgt_24xx_process_atio_queue(struct scsi_qla_host *);
extern void qla_tgt_24xx_config_rings(struct scsi_qla_host *, device_reg_t __iomem *);
extern void qla_tgt_2x00_config_nvram_stage1(struct scsi_qla_host *, nvram_t *);
extern void qla_tgt_2x00_config_nvram_stage2(struct scsi_qla_host *, init_cb_t *);
extern void qla_tgt_24xx_config_nvram_stage1(struct scsi_qla_host *, struct nvram_24xx *);
extern void qla_tgt_24xx_config_nvram_stage2(struct scsi_qla_host *, struct init_cb_24xx *);
extern void qla_tgt_abort_isp(struct scsi_qla_host *);
extern int qla_tgt_2x00_process_response_error(struct scsi_qla_host *, sts_entry_t *);
extern int qla_tgt_24xx_process_response_error(struct scsi_qla_host *, struct sts_entry_24xx *);
extern void qla_tgt_modify_vp_config(struct scsi_qla_host *, struct vp_config_entry_24xx *);
extern void qla_tgt_probe_one_stage1(struct scsi_qla_host *, struct qla_hw_data *);
extern int qla_tgt_mem_alloc(struct qla_hw_data *);
extern void qla_tgt_mem_free(struct qla_hw_data *);
extern void qla_tgt_stop_phase1(struct qla_tgt *);
extern void qla_tgt_stop_phase2(struct qla_tgt *);

#endif /* __QLA_TARGET_H */