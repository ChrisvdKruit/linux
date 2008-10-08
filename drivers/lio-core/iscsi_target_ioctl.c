/*********************************************************************************
 * Filename:  iscsi_target_ioctl.c
 *
 * This file contains the functions related to the Target IOCTL.
 *
 * Copyright (c) 2003, 2004, 2005 PyX Technologies, Inc.
 * Copyright (c) 2005, 2006, 2007 SBE, Inc. 
 * Copyright (c) 2007 Rising Tide Software, Inc.
 *
 * Nicholas A. Bellinger <nab@kernel.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *********************************************************************************/


#define ISCSI_TARGET_IOCTL_C

#include <linux/module.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/proc_fs.h>

#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

#include <asm/uaccess.h>

#include <iscsi_linux_os.h>
#include <iscsi_linux_defs.h>

#include <iscsi_protocol.h>
#include <iscsi_target_core.h>
#include <iscsi_target_error.h>
#include <iscsi_debug.h>
#include <iscsi_target_ioctl.h>
#include <iscsi_target_ioctl_defs.h>
#include <iscsi_target_device.h>
#include <target_core_hba.h>
#include <iscsi_target_nodeattrib.h>
#include <iscsi_target_tpg.h>
#include <target_core_transport.h>
#include <iscsi_target_util.h>
#include <iscsi_target.h>
#include <iscsi_parameters.h>

#include <target_core_plugin.h>
#include <target_core_seobj.h>
#include <target_core_feature_obj.h>
#include <target_core_feature_plugins.h>

#include <iscsi_target_info.h>

#undef ISCSI_TARGET_IOCTL_C

extern iscsi_global_t *iscsi_global;
extern int iscsi_send_async_msg (iscsi_conn_t *, __u16, __u8, __u8);
extern int iscsi_target_release_phase1(int);
extern void iscsi_target_release_phase2(void);

/* Symbols from target_core_mod */
extern int se_free_virtual_device (struct se_device_s *, struct se_hba_s *);
extern int linux_blockdevice_check (int, int);
extern int se_dev_set_attrib (struct se_device_s *, u32, u32, int);

static int get_out_count (int cmd, struct iscsi_target *t)
{
	switch (cmd) {
	case ISCSI_TARGET_HBA_DEV_INFO:
	  if (iscsi_get_dev_info_count_for_hba(t->hba_id, &t->out_count) < 0)
		  return(ERR_NO_MEMORY);
	  break;
	case ISCSI_TARGET_LUN_INFO:
	  if (iscsi_get_lun_info_count_for_tpg(t->targetname, t->tpgt, &t->out_count) < 0)
		  return(ERR_TPG_DOES_NOT_EXIST);
	  break;
	case ISCSI_TARGET_GLOBAL_HBA_INFO:
	  if (iscsi_get_hba_info_count_for_global(&t->out_count) < 0)
		  return(ERR_NO_MEMORY);
	  break;
	case ISCSI_TARGET_SESS_INFO:
	  if (iscsi_get_sess_info_count_for_tpg(t->targetname, t->tpgt, &t->out_count) < 0)
		  return(ERR_TPG_DOES_NOT_EXIST);
	  break;
	case ISCSI_TARGET_GLOBAL_TPG_INFO:
	  if (iscsi_get_tpg_info_count_for_global(t->targetname, &t->out_count) < 0)
		  return(ERR_NO_MEMORY);
	  break;
	case ISCSI_TARGET_TPG_INFO:
	  if (iscsi_get_tpg_info_count_for_tpg(t->targetname, t->tpgt, &t->out_count) < 0)
		  return(ERR_TPG_DOES_NOT_EXIST);
	  break;
	case ISCSI_TARGET_PLUGIN_INFO:
	  if (iscsi_get_plugin_count(&t->out_count) < 0)
		  return(ERR_PLUGIN_DOES_NOT_EXIST);
	  break;
	case ISCSI_TARGET_LISTNODEATTRIB:
	  t->out_count = 1;
	  break;
	case ISCSI_TARGET_LISTTPGATTRIB:
	  t->out_count = 1;
	  break;
	case ISCSI_TARGET_LISTGNINFO:
	  core_get_tiqn_count(&t->out_count);
	  break;
	case ISCSI_TARGET_LISTGNPINFO:
	  core_get_np_count(&t->out_count);
	  break;
	case ISCSI_TARGET_LISTGHBADEVINFO:
	  iscsi_get_global_dev_info_count(&t->out_count);
	  break;
	case ISCSI_TARGET_LISTDEVATTRIB:
          t->out_count = 1;
	  break;
	default:
	  return(-1);
	}

	return(0);
}

static int check_info_out (int cmd, struct iscsi_target *t)
{
	int ret = 0;
  
	if (t->out_count_cur >= t->out_count)
		return(0);
	
	switch (cmd) {
	case ISCSI_TARGET_HBA_DEV_INFO:
	  t->out_size = iscsi_get_hba_dev_info(t->hba_id,
					       t->out_buf, t->out_buf_size,
					       (t->out_state == INFO_GOT_OUT_COUNT),
					       t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_LUN_INFO:
	  t->out_size = iscsi_tpg_get_lun_info(t->targetname, t->tpgt,
					       t->out_buf, t->out_buf_size,
					       (t->out_state == INFO_GOT_OUT_COUNT),
					       t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_GLOBAL_HBA_INFO:
	  t->out_size = iscsi_get_hba_info(
					   t->out_buf, t->out_buf_size,
					   (t->out_state == INFO_GOT_OUT_COUNT),
					   t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_PLUGIN_INFO:
	  t->out_size = iscsi_get_plugin_info(
					      t->out_buf, t->out_buf_size,
					      (t->out_state == INFO_GOT_OUT_COUNT),
					      t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_SESS_INFO:
	  t->out_size = iscsi_tpg_get_sess_info(t->targetname, t->tpgt,
						t->out_buf, t->out_buf_size,
						(t->out_state == INFO_GOT_OUT_COUNT),
						t->out_count_cur, 1);
	  break;	
	case ISCSI_TARGET_GLOBAL_TPG_INFO:
	  t->out_size = iscsi_tpg_get_global_tpg_info(t->targetname,
						      t->out_buf, t->out_buf_size,
						      (t->out_state == INFO_GOT_OUT_COUNT),
						      t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_TPG_INFO:
	  t->out_size = iscsi_tpg_get_tpg_info(t->targetname, t->tpgt,
					       t->out_buf, t->out_buf_size,
					       (t->out_state == INFO_GOT_OUT_COUNT),
					       t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_LISTNODEATTRIB:
	  iscsi_get_node_attrib_info (t->targetname, t->tpgt, t->keytext,
				      t->out_buf, t->out_buf_size,
				      (t->out_state == INFO_GOT_OUT_COUNT),
				      t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_LISTTPGATTRIB:
	  iscsi_get_tpg_attrib_info (t->targetname, t->tpgt, t->keytext,
				     t->out_buf, t->out_buf_size,
				     (t->out_state == INFO_GOT_OUT_COUNT),
				     t->out_count_cur, 1);
	  break;
	case ISCSI_TARGET_LISTGNINFO:
	  	t->out_size = core_list_gninfo (
				t->out_buf, t->out_buf_size,
				(t->out_state == INFO_GOT_OUT_COUNT),
				t->out_count_cur, 1);
		break;
	case ISCSI_TARGET_LISTGNPINFO:
		t->out_size = core_list_gnpinfo (
				t->out_buf, t->out_buf_size,
				(t->out_state == INFO_GOT_OUT_COUNT),
				t->out_count_cur, 1);
		break;
	case ISCSI_TARGET_LISTGHBADEVINFO:
		t->out_size = iscsi_get_global_dev_info (t->out_buf, t->out_buf_size,
				(t->out_state == INFO_GOT_OUT_COUNT),
				t->out_count_cur, 1);
		break;
	case ISCSI_TARGET_LISTDEVATTRIB:
		t->out_size = se_dev_get_attrib_info(t);
		break;
	default:
	  return(-1);
	}

	if (t->out_size < 0) {
		t->out_state = INFO_ERROR;
		return(-1);
	}
	if (++t->out_count_cur == t->out_count)
		t->out_state = INFO_DONE;
	else {
		ret = 1;
		t->out_state = INFO_GOT_MORE_DATA;
	}

	return(ret);
}

/*      iscsi_open():
 *
 *      Used by the IOCTL.
 */
extern int iscsi_open (struct inode *inode, struct file *filp)
{
	return(0);
}

/*      iscsi_close():
 *
 *      Used by the IOCTL.
 */
extern int iscsi_close(struct inode *inode, struct file *filp)
{
	return(0);
}

/*
 * In T/I mode, used by initiator to release se_hba_t from storage engine
 * before LUN shutdown.
 */
extern int se_delhbafromtarget (void *p)
{
	se_hba_t *hba;
	int ret;

	if (!(hba = iscsi_get_hba_from_ptr(p)))
		return(ERR_HBA_CANNOT_LOCATE);

	ret = iscsi_hba_del_hba(hba);

	core_put_hba(hba);
	return(ret);
}

EXPORT_SYMBOL(se_delhbafromtarget);

/*      iscsi_target_ioctl():
 *
 *
 */
extern int iscsi_ioctl (
			struct inode *inode,
			struct file *filp,
			unsigned int cmd,
			unsigned long arg)
{
	int network_transport = 0, ret = 0;
	u32 lun_access = 0;
	se_device_t *dev = NULL;
	se_dev_transport_info_t devt_info;
	se_hba_t *hba = NULL;
	se_hbainfo_t hba_info;
	iscsi_portal_group_t *tpg = NULL;
	iscsi_session_t	*sess = NULL;
	iscsi_tiqn_t *tiqn = NULL;
	struct iscsi_target *t;
	
	if (!(t = (struct iscsi_target *) kmalloc(sizeof(struct iscsi_target), GFP_KERNEL)))
		return(-ENOMEM);
  
	if (COPY_FROM_USER(t, arg, sizeof(struct iscsi_target)) != 0) {
		kfree(t);
		TRACE_ERROR("copy_from_user() failed.\n");
		return(-EFAULT);
	}

	if (!iscsi_global->targetname_set && cmd != ISCSI_TARGET_EEPROM_DATA &&
					     cmd != ISCSI_TARGET_SHUTDOWN)
	{

		if (cmd != ISCSI_TARGET_SETTARGETNAME)
		{
			TRACE_ERROR("iSCSI Target Node Name is not set\n");
			ret = ERR_TARGETNAME_NOT_SET;
			goto dumpout;
		}
	}
  
	switch (cmd) {
	case ISCSI_TARGET_LOGOUT_SESS:
	  //#warning FIXME: ISCSI_TARGET_LOGOUT_SESS is stubbed
#if 0	
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  goto dumpout;
	  if (!(sess = iscsi_get_tpg_session_from_sid(tpgt, t->sid))) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_send_async_msg(sess->conn_head, 0,
			       ASYNC_EVENT_REQUEST_LOGOUT, 0);
	  iscsi_dec_session_usage_count(sess);	
	  iscsi_put_tpg(tpg);
#endif
	  break;
	
	case ISCSI_TARGET_LUN_INFO:
	case ISCSI_TARGET_GLOBAL_HBA_INFO:
	case ISCSI_TARGET_HBA_DEV_INFO:
	case ISCSI_TARGET_GLOBAL_RAID_INFO:
	case ISCSI_TARGET_GLOBAL_SIG_INFO:
	case ISCSI_TARGET_PLUGIN_INFO:
	case ISCSI_TARGET_RAID_INFO:
	case ISCSI_TARGET_SIG_INFO:
	case ISCSI_TARGET_SESS_INFO:
	case ISCSI_TARGET_GLOBAL_TPG_INFO:
	case ISCSI_TARGET_TPG_INFO:
	case ISCSI_TARGET_LISTNODEATTRIB:
	case ISCSI_TARGET_LISTTPGATTRIB:
	case ISCSI_TARGET_LISTGNINFO:
	case ISCSI_TARGET_LISTGNPINFO:
	case ISCSI_TARGET_LISTDEVATTRIB:
	  switch (t->out_state) {
	  case INFO_GET_OUT_COUNT:
	    if ((ret = get_out_count(cmd, t)) < 0) {
		    t->out_state = INFO_ERROR;
		    goto dumpout;
	    }
	    t->out_buf_size = IOCTL_BUFFER_LEN;
	    t->out_state = INFO_GOT_OUT_COUNT;
	    ret = 1;
	    break;
	  case INFO_GOT_OUT_COUNT:
	  case INFO_GOT_MORE_DATA:
	    if ((ret = check_info_out(cmd, t)) < 0)
		    goto dumpout;
	    break;
	  case INFO_ERROR:
	    TRACE_ERROR("INFO_ERROR for 0x%08x\n", cmd);
	  default:
	    TRACE_ERROR("Unknown t->out_state for INFO\n");
	    t->out_state = INFO_ERROR;
	    goto dumpout;
	  }
	  break;
	case ISCSI_TARGET_LISTTPGPARAMS:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0)))
		  goto dumpout;
	  iscsi_tpg_dump_params(tpg);
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_LISTSESSPARAMS:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0)))
		  goto dumpout;
	  if (!(sess = iscsi_get_tpg_session_from_sid(tpg, t->sid))) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_print_session_params(sess);
	  iscsi_dec_session_usage_count(sess);
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_SETTARGETNAME:
	  if (iscsi_global->targetname_set) {
		  TRACE_ERROR("iSCSI Target Name already set\n");
		  ret = ERR_TARGETNAME_ALREADY_SET;
		  goto dumpout;
	  }
	  if ((tiqn = core_add_tiqn(t->targetname, &ret))) {
		  snprintf(iscsi_global->targetname, ISCSI_IQN_LEN,
				 "%s", t->targetname);
		  iscsi_global->targetname_set = 1;
		  iscsi_global->global_tiqn = tiqn;
	  }
	  break;
	case ISCSI_TARGET_SETTPGPARAM:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_NOT_ACTIVE;
		  goto dumpout;
	  }
	  if ((ret = iscsi_change_param_value(t->keytext, SENDER_TARGET,
					      tpg->param_list, 1)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_SETSESSPARAM:
	  //#warning FIXME: ISCSI_TARGET_SETSESSPARAM is incomplete
	  break;
	case ISCSI_TARGET_ADDTPG:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 1))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_add_portal_group(tiqn, tpg)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_DELTPG:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_NOT_ACTIVE;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_del_portal_group(tiqn, tpg, t->force)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_ENABLETPG:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_enable_portal_group(tpg)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_DISABLETPG:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_disable_portal_group(tpg, t->force)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_SETTPGATTRIB:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_set_attributes(tpg, t->ta_attrib,
					      t->ta_value)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_ADDNPTOTPG:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
		
	  if (t->net_params_set & PARAM_NET_SCTP_TCP)
		  network_transport = ISCSI_SCTP_TCP;
	  else if (t->net_params_set & PARAM_NET_SCTP_UDP)
		  network_transport = ISCSI_SCTP_UDP;
	  else
		  network_transport = ISCSI_TCP;
		
	  if ((ret = iscsi_tpg_add_network_portal(tpg, t, network_transport)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_DELNPFROMTPG:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	
	  if (t->net_params_set & PARAM_NET_SCTP_TCP)
		  network_transport = ISCSI_SCTP_TCP;
	  else if (t->net_params_set & PARAM_NET_SCTP_UDP)
		  network_transport = ISCSI_SCTP_UDP;
	  else
		  network_transport = ISCSI_TCP;
		
	  if ((ret = iscsi_tpg_del_network_portal(tpg, t, network_transport)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_ADDHBATOTARGET:
	  memset(&hba_info, 0, sizeof(se_hbainfo_t));
	  hba_info.hba_id = t->hba_id;
	  hba_info.hba_type = t->hba_type;
	  hba_info.internal_dma_alloc = t->internal_dma_alloc;
	  hba_info.os_claim_devices = t->os_claim_devices;
	  if ((ret = iscsi_hba_check_addhba_params(t, &hba_info)) < 0)
		  goto dumpout;
	  if (!(hba = core_get_hba_from_id(t->hba_id, 1))) {
		  ret = ERR_HBA_CANNOT_LOCATE;
		  goto dumpout;	
	  }
	  if ((ret = iscsi_hba_add_hba(hba, &hba_info, t)) < 0) {
		  core_put_hba(hba);
		  goto dumpout;
	  }
	  core_put_hba(hba);
	  break;
	case ISCSI_TARGET_DELHBAFROMTARGET:
	  if (!(hba = core_get_hba_from_id(t->hba_id, 0))) {
		  ret = ERR_HBA_CANNOT_LOCATE;
		  goto dumpout;
	  }
	  if ((ret = iscsi_hba_del_hba(hba)) < 0) {
		  core_put_hba(hba);
		  goto dumpout;
	  }
	  core_put_hba(hba);
	  break;
	case ISCSI_TARGET_ADDNODETOTPG:
#if 0
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_add_initiator_node_acl(tpg, (const char *)t->keytext,
						      t->queue_depth)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_DELNODEFROMTPG:
#if 0
	  if (!(tpg =  core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_del_initiator_node_acl(tpg, t->keytext,
						      t->force)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_SETNODEQUEUEDEPTH:
#if 0
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_tpg_set_initiator_node_queue_depth(tpg,
							      t->keytext, t->queue_depth, t->force)) < 0) 
	  {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_CREATEVIRTDEV:
#if 0
	  memset(&dev_info, 0, sizeof(se_devinfo_t));
	  dev_info.hba_type = t->hba_type;
	  if (!(hba = core_get_hba_from_id(t->hba_id, 0))) {
		  ret = ERR_HBA_CANNOT_LOCATE;
		  goto dumpout;
	  }
	  if ((ret = iscsi_check_hba_for_virtual_device(t, &dev_info, hba)) < 0) {
		  core_put_hba(hba);
		  goto dumpout;
	  }
	  if ((ret = iscsi_create_virtual_device(hba, &dev_info, t)) < 0) {
		  core_put_hba(hba);
		  goto dumpout;
	  }
	  core_put_hba(hba);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_FREEVIRTDEV:
#if 0
	  memset(&devt_info, 0, sizeof(se_dev_transport_info_t));
	  devt_info.hba_id = t->hba_id;
 	
	  if (!(dev = transport_core_locate_dev(t, &devt_info, &ret))) 
		  goto dumpout;

	  if (!(hba = dev->iscsi_hba)) {
		  ret = ERR_UNKNOWN_ERROR;
		  goto dumpout;
	  }
	  if ((ret = se_free_virtual_device(dev, dev->iscsi_hba)) < 0) {
		  core_put_hba(hba);
		  goto dumpout;
	  }
	  core_put_hba(hba);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_ADDLUNTODEV:
#if 0
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }

	  memset(&devt_info, 0, sizeof(se_dev_transport_info_t));
	  devt_info.hba_id = t->hba_id;

	  if (!(dev = transport_core_locate_dev(t, &devt_info, &ret))) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  if ((ret = iscsi_dev_add_lun(tpg, dev->iscsi_hba, dev, &devt_info)) < 0) {
		  core_put_hba(dev->iscsi_hba);
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  core_put_hba(dev->iscsi_hba);
	  iscsi_put_tpg(tpg);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_DELLUNFROMDEV:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_dev_del_lun(tpg, t->iscsi_lun)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_ADDNODETOLUN:
#if 0
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if (!(t->params_set & PARAM_MAPPED_LUN))
		  t->mapped_lun = t->iscsi_lun;
	  if (t->params_set & PARAM_LUN_ACCESS)
		  lun_access = (!t->lun_access) ? ISCSI_LUNFLAGS_READ_ONLY :
			  ISCSI_LUNFLAGS_READ_WRITE;
	  else
		  lun_access = ISCSI_LUNFLAGS_READ_WRITE;
	  
	  if ((ret = iscsi_dev_add_initiator_node_lun_acl(tpg, t->iscsi_lun,
			  t->mapped_lun, lun_access, t->keytext)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_DELNODEFROMLUN:
#if 0
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if (!(t->params_set & PARAM_MAPPED_LUN))
		  t->mapped_lun = t->iscsi_lun;
	  
	  if ((ret = iscsi_dev_del_initiator_node_lun_acl(tpg,
			  t->iscsi_lun, t->mapped_lun, t->keytext)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
#else
	goto dumpout;
#endif
	case ISCSI_TARGET_SETNODEATTRIB:
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if ((ret = iscsi_set_initiator_node_attribute(tpg, t->keytext,
							t->na_attrib, t->na_value)) < 0) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }
	  iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_DEBUG_ERL:
#ifdef DEBUG_ERL
	  if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
		  ret = ERR_TPG_DOES_NOT_EXIST;
		  goto dumpout;
	  }
	  if (!(sess = iscsi_get_tpg_session_from_sid(
						      tpg, t->sid))) {
		  iscsi_put_tpg(tpg);
		  goto dumpout;
	  }

	  spin_lock(&iscsi_global->debug_erl_lock);
	  ISCSI_DEBUG_ERL(iscsi_global)->debug_type = t->debug_type;
	  ISCSI_DEBUG_ERL(iscsi_global)->cid = t->cid;
	  ISCSI_DEBUG_ERL(iscsi_global)->tpgt = ISCSI_TPG_S(sess)->tpgt;
	  ISCSI_DEBUG_ERL(iscsi_global)->sid = sess->sid;
	  ISCSI_DEBUG_ERL(iscsi_global)->debug_erl = 1;
	  ISCSI_DEBUG_ERL(iscsi_global)->count = t->count;
	  spin_unlock(&iscsi_global->debug_erl_lock);
	  iscsi_dec_session_usage_count(sess);
	  iscsi_put_tpg(tpg);
	  break;
#else  /* DEBUG_ERL */
	  ret = 0;
	  goto dumpout;
#endif /* DEBUG_ERL */
	  
	case ISCSI_TARGET_DEBUG_DEV:
#ifdef DEBUG_DEV
	  memset(&devt_info, 0, sizeof(se_dev_transport_info_t));
	  devt_info.hba_id = t->hba_id;

	  if (!(dev = transport_core_locate_dev(t, &devt_info, &ret)))
		  goto dumpout;

	  spin_lock_irq(&iscsi_global->debug_dev_lock);
	  if (t->debug_type)
		  dev->dev_flags &= ~DF_DEV_DEBUG;
	  else
		  dev->dev_flags |= DF_DEV_DEBUG;
		
	  switch (hba->type) {
	  case PSCSI:
	    PYXPRINT("iSCSI_HBA[%u] - Set %s for PSCSI %d/%d/%d\n",
		     hba->hba_id, (t->debug_type) ? "ONLINE" : "OFFLINE",
		     devt_info.scsi_channel_id, devt_info.scsi_target_id,
		     devt_info.scsi_lun_id);
	    break;
	  case FILEIO:
	    PYXPRINT("iSCSI_HBA[%u] - Set %s for FILEIO: %u\n",
		     hba->hba_id, (t->debug_type) ? "ONLINE" : "OFFLINE",
		     devt_info.fd_device_id);
	    break;
	  case RAMDISK_DR:
	  case RAMDISK_MCP:
	    PYXPRINT("iSCSI_HBA[%u] - Set %s for RAMDISK: %u\n",
		     hba->hba_id, (t->debug_type) ? "ONLINE" : "OFFLINE",
		     devt_info.rd_device_id);
	    break;
	  default:
	    PYXPRINT("iSCSI_HBA[%u] - DEBUG_DEV unknown hba_type: %d\n",
		     hba->hba_id, hba->type);
	    break;
	  }
	  spin_unlock_irq(&iscsi_global->debug_dev_lock);

	  core_put_hba(dev->iscsi_hba);
	  break;
#else /* DEBUG_DEV */
	  goto dumpout;
#endif /* DEBUG_DEV */
#ifdef PSCSI
	case ISCSI_TARGET_PSCSI_MOUNT_STATUS:
	  TRACE_ERROR("ISCSI_TARGET_PSCSI_MOUNT_STATUS has been removed, please use"
			" ISCSI_TARGET_BLOCKDEV_CHECK!!\n");
	  ret = -1;
	  break;
#endif
	case ISCSI_TARGET_SETLUNACCESS:
		if (!(tpg = core_get_tpg_from_iqn(t->targetname, &tiqn, t->tpgt, 0))) {
			ret = ERR_TPG_DOES_NOT_EXIST;
			goto dumpout;
		}
		if (t->params_set & PARAM_LUN_ACCESS)
			lun_access |= (!t->lun_access) ? ISCSI_LUNFLAGS_READ_ONLY :
				ISCSI_LUNFLAGS_READ_WRITE;

		ret = iscsi_dev_set_initiator_node_lun_access(tpg, t->mapped_lun,
				lun_access, t->keytext);

		iscsi_put_tpg(tpg);
	  break;
	case ISCSI_TARGET_BLOCKDEV_CHECK:
		ret = linux_blockdevice_check(t->iblock_major, t->iblock_minor);
		break;

	case ISCSI_TARGET_SHUTDOWN:
		if (iscsi_target_release_phase1(0) < 0) {
			ret = ERR_IN_SHUTDOWN;
			goto dumpout;
		}
		iscsi_target_release_phase2();
		
		break;
	case ISCSI_TARGET_ADDTIQN:
		tiqn = core_add_tiqn(t->targetname, &ret);
		break;
	case ISCSI_TARGET_DELTIQN:
		ret = core_del_tiqn(t->targetname);
		break;
	case ISCSI_TARGET_ADDNPTOCORE:
		if (t->net_params_set & PARAM_NET_SCTP_TCP)
			network_transport = ISCSI_SCTP_TCP;
		else if (t->net_params_set & PARAM_NET_SCTP_UDP)
			network_transport = ISCSI_SCTP_UDP;
		else
			network_transport = ISCSI_TCP;

		core_add_np(t, network_transport, &ret);
		if (ret != 0)
			goto dumpout;
		break;
	case ISCSI_TARGET_DELNPFROMCORE:
		if (t->net_params_set & PARAM_NET_SCTP_TCP)
			network_transport = ISCSI_SCTP_TCP;
		else if (t->net_params_set & PARAM_NET_SCTP_UDP)
			network_transport = ISCSI_SCTP_UDP;
		else
			network_transport = ISCSI_TCP;

		if ((ret = core_del_np(t, network_transport)) < 0)
			goto dumpout;
		break;
	case ISCSI_TARGET_SETDEVATTRIB:
		memset(&devt_info, 0, sizeof(se_dev_transport_info_t));
		devt_info.hba_id = t->hba_id;

		if (!(dev = transport_core_locate_dev(t, &devt_info, &ret)))
			goto dumpout;
		if ((ret = se_dev_set_attrib(dev, t->da_attrib, t->da_value,
				t->force)) < 0) {
			core_put_hba(dev->iscsi_hba);
			goto dumpout;
		}
		core_put_hba(dev->iscsi_hba);
		break;
	default:
	  	TRACE_ERROR("Unknown IOCTL cmd: 0x%08x\n", cmd);
		goto dumpout;
	}

 dumpout:
	t->ioctl_ret = ret;

	if (COPY_TO_USER(arg, t, sizeof(struct iscsi_target))) {
		TRACE_ERROR("copy_to_user() failed.\n");
		kfree(t);
		return(-EFAULT);
	}

	kfree(t);
	return(ret);
}

extern int iscsi_targetname (void) { return (iscsi_global->targetname_set); }

EXPORT_SYMBOL(iscsi_targetname);

extern void se_set_forcechanoffline (void *p)
{
	if (iscsi_global->ti_forcechanoffline) {
		TRACE_ERROR("iscsi_global->ti_forcechanoffline already set!\n");
		return;
	}

	iscsi_global->ti_forcechanoffline = p;
	
	return;
}

EXPORT_SYMBOL(se_set_forcechanoffline);

#ifdef CONFIG_COMPAT

extern long iscsi_compat_ioctl (struct file *filp,
				unsigned int cmd,
				unsigned long arg)
{
	return (long) iscsi_ioctl(filp->f_dentry->d_inode, filp, cmd, (unsigned int) arg);
}

#endif
