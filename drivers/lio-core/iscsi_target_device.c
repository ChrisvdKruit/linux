/*********************************************************************************
 * Filename:  iscsi_target_device.c
 *
 * This file contains the iSCSI Virtual Device and Disk Transport
 * agnostic related functions.
 *
 * Copyright (c) 2003, 2004, 2005 PyX Technologies, Inc.
 * Copyright (c) 2005-2006 SBE, Inc.  All Rights Reserved.
 * Copyright (c) 2007 Rising Tide Software, Inc.
 * Copyright (c) 2008 Linux-iSCSI.org
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


#define ISCSI_TARGET_DEVICE_C

#include <linux/net.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/in.h>
#include <net/sock.h>
#include <net/tcp.h>

#include <iscsi_linux_os.h>
#include <iscsi_linux_defs.h>

#include <iscsi_lists.h>
#include <iscsi_debug.h>
#include <iscsi_protocol.h>
#include <iscsi_target_core.h>
#include <target_core_base.h>
#include <iscsi_target_error.h>
#include <iscsi_target_device.h>
#include <target_core_device.h>
#include <target_core_hba.h>
#include <iscsi_target_tpg.h>
#include <target_core_transport.h>
#include <iscsi_target_util.h>

#include <target_core_plugin.h>
#include <target_core_seobj.h>

#undef ISCSI_TARGET_DEVICE_C

extern se_global_t *iscsi_global;
extern __u32 iscsi_unpack_lun (unsigned char *);

/*	iscsi_get_lun():
 *
 *
 */
//#warning FIXME v2.8: Breakage in iscsi_get_lun() for TMRs
extern se_lun_t *iscsi_get_lun (
	iscsi_conn_t *conn,
	u64 lun)
{
	u32 unpacked_lun;
	se_dev_entry_t *deve;
	se_lun_t *iscsi_lun = NULL;
	iscsi_portal_group_t *tpg = conn->tpg;
	iscsi_session_t *sess = SESS(conn);

	unpacked_lun = iscsi_unpack_lun((unsigned char *)&lun);

	if (unpacked_lun > (ISCSI_MAX_LUNS_PER_TPG-1)) {
		TRACE_ERROR("iSCSI LUN: %u exceeds ISCSI_MAX_LUNS_PER_TPG-1:"
			" %u for Target Portal Group: %hu\n", unpacked_lun,
			ISCSI_MAX_LUNS_PER_TPG-1, tpg->tpgt);
		return(NULL);
	}

	spin_lock_bh(&SESS_NODE_ACL(sess)->device_list_lock);
	deve = &SESS_NODE_ACL(sess)->device_list[unpacked_lun];
	if (deve->lun_flags & ISCSI_LUNFLAGS_INITIATOR_ACCESS) {
#if 0
		TRACE_ERROR("deve->deve_cmds incremented to %d\n", deve->deve_cmds);
#endif
		iscsi_lun = deve->iscsi_lun;
	}
	spin_unlock_bh(&SESS_NODE_ACL(sess)->device_list_lock);

	if (!iscsi_lun) {
		TRACE_ERROR("Unable to find Active iSCSI LUN: 0x%08x on"
			" iSCSI TPG: %hu\n", unpacked_lun, tpg->tpgt);
		return(NULL);
	}

	return(iscsi_lun);
}

/*	iscsi_get_lun_for_cmd():
 *	
 *	Returns (0) on success
 * 	Returns (1) on REPORT_LUN cdb
 * 	Returns (< 0) on failure
 */
extern int iscsi_get_lun_for_cmd (
	iscsi_cmd_t *cmd,
	unsigned char *cdb,
	u64 lun)
{
	iscsi_conn_t *conn = CONN(cmd);
	iscsi_portal_group_t *tpg = ISCSI_TPG_C(conn);
	u32 unpacked_lun;
	int ret;

	unpacked_lun = iscsi_unpack_lun((unsigned char *)&lun);
	if (unpacked_lun > (ISCSI_MAX_LUNS_PER_TPG-1)) {
		TRACE_ERROR("iSCSI LUN: %u exceeds ISCSI_MAX_LUNS_PER_TPG-1:"
			" %u for Target Portal Group: %hu\n", unpacked_lun,
			ISCSI_MAX_LUNS_PER_TPG-1, tpg->tpgt);
		return(-1);
	}

	if ((ret = transport_get_lun_for_cmd(SE_CMD(cmd), cdb, unpacked_lun)) < 0)
		return(ret);
	if (ret > 0) /* For cdb[0] == REPORT_LUNS */
		return(ret);

	cmd->cmd_flags |= ICF_SE_LUN_CMD;	
	return(0);
}

/*	iscsi_determine_maxcmdsn():
 * 
 *
 */
extern void iscsi_determine_maxcmdsn (iscsi_session_t *sess, iscsi_node_acl_t *acl)
{
	/*
	 * This is a discovery session, the single queue slot was already assigned in
	 * iscsi_login_zero_tsih().  Since only Logout and Text Opcodes are allowed
	 * during discovery we do not have to worry about the HBA's queue depth here.
	 */
	if (SESS_OPS(sess)->SessionType)
		return;	

	/*
	 * This is a normal session, set the Session's CmdSN window to the
	 * iscsi_node_acl_t->queue_depth value set by iscsi_add_acl_for_tpg() or
	 * iscsi_change_queue_depth().  The value in iscsi_node_acl_t->queue_depth
	 * has already been validated as a legal value in
	 * iscsi_set_queue_depth_for_node().
	 */
	sess->cmdsn_window = acl->queue_depth;
	sess->max_cmd_sn = (sess->max_cmd_sn + acl->queue_depth) - 1;

	return;
}

/*	iscsi_increment_maxcmdsn();
 *
 *	
 */
extern void iscsi_increment_maxcmdsn (iscsi_cmd_t *cmd, iscsi_session_t *sess)
{
	if (cmd->immediate_cmd || cmd->maxcmdsn_inc)
		return;

	cmd->maxcmdsn_inc = 1;
	
	spin_lock(&sess->cmdsn_lock);
	sess->max_cmd_sn += 1;
	TRACE(TRACE_ISCSI, "Updated MaxCmdSN to 0x%08x\n", sess->max_cmd_sn);
	spin_unlock(&sess->cmdsn_lock);
	
	return;
}

/*	iscsi_set_queue_depth_for_node():
 *
 *
 */
extern int iscsi_set_queue_depth_for_node (
	iscsi_portal_group_t *tpg,
	iscsi_node_acl_t *acl)
{
	if (!acl->queue_depth) {
		TRACE_ERROR("Queue depth for iSCSI Initiator Node: %s is 0,"
			" defaulting to 1.\n", acl->initiatorname);
		acl->queue_depth = 1;
	}

	return(0);
}

/*	iscsi_create_device_list_for_node():
 *
 *
 */
extern int iscsi_create_device_list_for_node (iscsi_node_acl_t *nacl, iscsi_portal_group_t *tpg)
{
	if (!(nacl->device_list = (se_dev_entry_t *) kmalloc(
			sizeof(se_dev_entry_t) * ISCSI_MAX_LUNS_PER_TPG, GFP_KERNEL))) {
		TRACE_ERROR("Unable to allocate memory for session"
				" device list.\n");
		return(-1);
	}
	memset(nacl->device_list, 0, sizeof(se_dev_entry_t) *
			ISCSI_MAX_LUNS_PER_TPG);

	return(0);
}

/*	iscsi_free_device_list_for_node():
 *
 *
 */
extern int iscsi_free_device_list_for_node (iscsi_node_acl_t *nacl, iscsi_portal_group_t *tpg)
{
	__u32 i;
	se_dev_entry_t *deve;
	se_lun_t *iscsi_lun;

	if (!nacl->device_list)
		return(0);
		
	spin_lock_bh(&nacl->device_list_lock);
	for (i = 0; i < ISCSI_MAX_LUNS_PER_TPG; i++) {
		deve = &nacl->device_list[i];
		
		if (!(deve->lun_flags & ISCSI_LUNFLAGS_INITIATOR_ACCESS))
			continue;
		
		if (!deve->iscsi_lun) {
			TRACE_ERROR("iSCSI device entries device pointer is"
				" NULL, but Initiator has access.\n");
			continue;
		}

		iscsi_lun = deve->iscsi_lun;

		spin_unlock_bh(&nacl->device_list_lock);
		iscsi_update_device_list_for_node(iscsi_lun, deve->mapped_lun,
				ISCSI_LUNFLAGS_NO_ACCESS, nacl, tpg, 0);
		spin_lock_bh(&nacl->device_list_lock);
	}
	spin_unlock_bh(&nacl->device_list_lock);

	kfree(nacl->device_list);
	nacl->device_list = NULL;
	
	return(0);
}

extern void iscsi_update_device_list_access (
	u32 mapped_lun,
	u32 lun_access,
	iscsi_node_acl_t *nacl)
{
	se_dev_entry_t *deve;

	spin_lock_bh(&nacl->device_list_lock);
	deve = &nacl->device_list[mapped_lun];
	if (lun_access & ISCSI_LUNFLAGS_READ_WRITE) {
		deve->lun_flags &= ~ISCSI_LUNFLAGS_READ_ONLY;
		deve->lun_flags |= ISCSI_LUNFLAGS_READ_WRITE;
	} else {
		deve->lun_flags &= ~ISCSI_LUNFLAGS_READ_WRITE;
		deve->lun_flags |= ISCSI_LUNFLAGS_READ_ONLY;
	}
	spin_unlock_bh(&nacl->device_list_lock);

	return;
}

/*	iscsi_update_device_list_for_node():
 *
 *
 */
extern void iscsi_update_device_list_for_node (
	se_lun_t *lun,
	u32 mapped_lun,
	u32 lun_access,
	iscsi_node_acl_t *nacl,
	iscsi_portal_group_t *tpg,
	int enable)
{
	se_dev_entry_t *deve;

	spin_lock_bh(&nacl->device_list_lock);
	deve = &nacl->device_list[mapped_lun];
	if (enable) {
		deve->iscsi_lun = lun;
		deve->mapped_lun = mapped_lun;
		deve->lun_flags |= ISCSI_LUNFLAGS_INITIATOR_ACCESS;	

		if (lun_access & ISCSI_LUNFLAGS_READ_WRITE) {
			deve->lun_flags &= ~ISCSI_LUNFLAGS_READ_ONLY;
			deve->lun_flags |= ISCSI_LUNFLAGS_READ_WRITE;
		} else {
			deve->lun_flags &= ~ISCSI_LUNFLAGS_READ_WRITE;
			deve->lun_flags |= ISCSI_LUNFLAGS_READ_ONLY;
		}
#ifdef SNMP_SUPPORT
		deve->creation_time = get_jiffies_64();
		deve->attach_count++;
#endif /* SNMP_SUPPORT */
	} else {
		deve->iscsi_lun = NULL;
		deve->lun_flags = 0;
		deve->creation_time = 0;
	}
	spin_unlock_bh(&nacl->device_list_lock);
		
	return;
}

//#define DEBUG_CLEAR_LUN
#ifdef DEBUG_CLEAR_LUN
#define DEBUG_CLEAR_L(x...) PYXPRINT(x)
#else
#define DEBUG_CLEAR_L(x...)
#endif

/*      iscsi_clear_lun_from_tpg():
 *
 *
 */
extern void iscsi_clear_lun_from_tpg (se_lun_t *lun, iscsi_portal_group_t *tpg)
{
	u32 i;
        iscsi_node_acl_t *nacl, *nacl_next;
	se_dev_entry_t *deve;

        spin_lock_bh(&tpg->acl_node_lock);
        nacl = tpg->acl_node_head;
        while (nacl) {
                nacl_next = nacl->next;
		spin_unlock_bh(&tpg->acl_node_lock);

		spin_lock_bh(&nacl->device_list_lock);
		for (i = 0; i < ISCSI_MAX_LUNS_PER_TPG; i++) {
			deve = &nacl->device_list[i];
			if (lun != deve->iscsi_lun)
				continue;
			spin_unlock_bh(&nacl->device_list_lock);

	                iscsi_update_device_list_for_node(lun, deve->mapped_lun,
				ISCSI_LUNFLAGS_NO_ACCESS, nacl, tpg, 0);

			spin_lock_bh(&nacl->device_list_lock);
		}
		spin_unlock_bh(&nacl->device_list_lock);
			
		spin_lock_bh(&tpg->acl_node_lock);
                nacl = nacl_next;
        }
        spin_unlock_bh(&tpg->acl_node_lock);

        return;
}

/*	iscsi_dev_add_lun():
 *
 *
 */
extern se_lun_t *iscsi_dev_add_lun (
	iscsi_portal_group_t *tpg,
	se_hba_t *hba,
	se_device_t *dev,
	u32 lun,
	int *ret)
{
	se_lun_t *lun_p;
	u32 lun_access = 0;
	
	if (DEV_OBJ_API(dev)->check_count(&dev->dev_access_obj) != 0) {
		TRACE_ERROR("Unable to export se_device_t while dev_access_obj: %d\n",
			DEV_OBJ_API(dev)->check_count(&dev->dev_access_obj));
		*ret = ERR_OBJ_ACCESS_COUNT;
		return(NULL);
	}
				
#warning FIXME: Make check_tcq the default..?
#if 0
	spin_lock(&hba->device_lock);
	if (dti->check_tcq) {
		if (transport_check_device_tcq(dev, dti->iscsi_lun,
				dti->queue_depth) < 0) {
			spin_unlock(&hba->device_lock);
			return(ERR_ADDLUN_CHECK_TCQ_FAILED);
		}
	}
	spin_unlock(&hba->device_lock);
#endif
#if 0
	/*
	 * Now we claim exclusive access to the OS dependant block device.
	 */
	if (transport_generic_claim_phydevice(dev) < 0)
		return(ERR_BLOCKDEV_CLAIMED);
#endif	
	if (!(lun_p = iscsi_tpg_pre_addlun(tpg, lun, ret)))
		return(NULL);

	if (DEV_OBJ_API(dev)->get_device_access((void *)dev) == 0)
		lun_access = ISCSI_LUNFLAGS_READ_ONLY;
	else
		lun_access = ISCSI_LUNFLAGS_READ_WRITE;
	
	if (iscsi_tpg_post_addlun(tpg, lun_p, ISCSI_LUN_TYPE_DEVICE, lun_access,
			      dev, dev->dev_obj_api) < 0) {
		*ret = ERR_EXPORT_FAILED;
		return(NULL);
	}

	PYXPRINT("iSCSI_TPG[%hu]_LUN[%u] - Activated iSCSI Logical Unit from"
		" CORE HBA: %u\n", tpg->tpgt, lun_p->iscsi_lun, hba->hba_id);
	
	/*
	 * Update LUN maps for dynamically added initiators when generate_node_acl
	 * is enabled.
	 */
	if (ISCSI_TPG_ATTRIB(tpg)->generate_node_acls) {
		iscsi_node_acl_t *acl;
		spin_lock_bh(&tpg->acl_node_lock);
		for (acl = tpg->acl_node_head; acl; acl = acl->next) {
			if (acl->nodeacl_flags & NAF_DYNAMIC_NODE_ACL) {	
				spin_unlock_bh(&tpg->acl_node_lock);
				iscsi_tpg_add_node_to_devs(acl, tpg);	
				spin_lock_bh(&tpg->acl_node_lock);
			}
		}
		spin_unlock_bh(&tpg->acl_node_lock);
	}

	*ret = 0;
	return(lun_p);
}

/*	iscsi_dev_del_lun():
 *
 *
 */
extern int iscsi_dev_del_lun (
	iscsi_portal_group_t *tpg,
	__u32 iscsi_lun)
{
	se_lun_t *lun;
	int ret = 0;

	if (!(lun = iscsi_tpg_pre_dellun(tpg, iscsi_lun, ISCSI_LUN_TYPE_DEVICE, &ret)))
		return(ret);

	iscsi_tpg_post_dellun(tpg, lun);

	PYXPRINT("iSCSI_TPG[%hu]_LUN[%u] - Deactivated iSCSI Logical Unit from"
		" device object\n", tpg->tpgt, iscsi_lun);
	
	return(0);
}

extern se_lun_t *iscsi_get_lun_from_tpg (iscsi_portal_group_t *tpg, u32 lun)
{
	se_lun_t *iscsi_lun;

	spin_lock(&tpg->tpg_lun_lock);
	if (lun > (ISCSI_MAX_LUNS_PER_TPG-1)) {
		TRACE_ERROR("iSCSI LUN: %u exceeds ISCSI_MAX_LUNS_PER_TPG-1:"
			" %u for Target Portal Group: %hu\n", lun,
			ISCSI_MAX_LUNS_PER_TPG-1, tpg->tpgt);
			spin_unlock(&tpg->tpg_lun_lock);
		return(NULL);
	}
	iscsi_lun = &tpg->tpg_lun_list[lun];

	if (iscsi_lun->lun_status != ISCSI_LUN_STATUS_FREE) {
		TRACE_ERROR("iSCSI Logical Unit Number: %u is not free on"
			" iSCSI Target Portal Group: %hu, ignoring request.\n",
			lun, tpg->tpgt);
		spin_unlock(&tpg->tpg_lun_lock);
		return(NULL);
	}
	spin_unlock(&tpg->tpg_lun_lock);

	return(iscsi_lun);
}

/*	iscsi_dev_get_lun():
 *
 *
 */
static se_lun_t *iscsi_dev_get_lun (iscsi_portal_group_t *tpg, u32 lun)
{
	se_lun_t *iscsi_lun;

	spin_lock(&tpg->tpg_lun_lock);
	if (lun > (ISCSI_MAX_LUNS_PER_TPG-1)) {
		TRACE_ERROR("iSCSI LUN: %u exceeds ISCSI_MAX_LUNS_PER_TPG-1:"
			" %u for Target Portal Group: %hu\n", lun,
			ISCSI_MAX_LUNS_PER_TPG-1, tpg->tpgt);
		spin_unlock(&tpg->tpg_lun_lock);
		return(NULL);
	}
	iscsi_lun = &tpg->tpg_lun_list[lun];

	if (iscsi_lun->lun_status != ISCSI_LUN_STATUS_ACTIVE) {
		TRACE_ERROR("iSCSI Logical Unit Number: %u is not active on"	
			" iSCSI Target Portal Group: %hu, ignoring request.\n",
			lun, tpg->tpgt);
		spin_unlock(&tpg->tpg_lun_lock);
		return(NULL);
	}
	spin_unlock(&tpg->tpg_lun_lock);
	
	return(iscsi_lun);
}

extern se_lun_acl_t *iscsi_dev_init_initiator_node_lun_acl (
	iscsi_portal_group_t *tpg,
	u32 mapped_lun,
	char *initiatorname,
	int *ret)
{
	se_lun_acl_t *lacl;
	iscsi_node_acl_t *nacl;

	if (strlen(initiatorname) > ISCSI_IQN_LEN) {
		TRACE_ERROR("iSCSI InitiatorName exceeds maximum size.\n"); 
		*ret = -EOVERFLOW;
		return(NULL);
	}
	if (!(nacl = iscsi_tpg_get_initiator_node_acl(tpg, initiatorname))) {
		*ret = -EINVAL;
		return(NULL);
	}
	if (!(lacl = (se_lun_acl_t *) kmalloc(sizeof(se_lun_acl_t), GFP_KERNEL))) {	
		TRACE_ERROR("Unable to allocate memory for se_lun_acl_t.\n");
		*ret = -ENOMEM;
		return(NULL);
	}
	memset(lacl, 0, sizeof(se_lun_acl_t));

	lacl->mapped_lun = mapped_lun;
	lacl->se_lun_nacl = nacl;
	snprintf(lacl->initiatorname, ISCSI_IQN_LEN, "%s", initiatorname);

	return(lacl);
}

extern int iscsi_dev_add_initiator_node_lun_acl (
        iscsi_portal_group_t *tpg,
	se_lun_acl_t *lacl,
        u32 lun,
        u32 lun_access)
{
	se_lun_t *iscsi_lun;
	iscsi_node_acl_t *nacl;

	if (!(iscsi_lun = iscsi_dev_get_lun(tpg, lun))) {
		TRACE_ERROR("iSCSI Logical Unit Number: %u is not active on"
			" iSCSI Target Portal Group: %hu, ignoring request.\n",
                                lun, tpg->tpgt);
                return(-EINVAL);
        }

	if (!(nacl = iscsi_tpg_get_initiator_node_acl(tpg, lacl->initiatorname)))
		return(-EINVAL);

	spin_lock(&iscsi_lun->lun_acl_lock);
	ADD_ENTRY_TO_LIST(lacl, iscsi_lun->lun_acl_head, iscsi_lun->lun_acl_tail);
	spin_unlock(&iscsi_lun->lun_acl_lock);

	if ((iscsi_lun->lun_access & ISCSI_LUNFLAGS_READ_ONLY) &&
	    (lun_access & ISCSI_LUNFLAGS_READ_WRITE))
		lun_access = ISCSI_LUNFLAGS_READ_ONLY;

	lacl->iscsi_lun = iscsi_lun;
	
	iscsi_update_device_list_for_node(iscsi_lun, lacl->mapped_lun,
			lun_access, nacl, tpg, 1);

	PYXPRINT("iSCSI_TPG[%hu]_LUN[%u->%u] - Added %s ACL for iSCSI"
		" InitiatorNode: %s\n", tpg->tpgt, lun, lacl->mapped_lun,
		(lun_access & ISCSI_LUNFLAGS_READ_WRITE) ? "RW" : "RO",
			lacl->initiatorname);
	return(0);
}

/*	iscsi_dev_del_initiator_node_lun_acl():
 *
 *
 */
extern int iscsi_dev_del_initiator_node_lun_acl (
	iscsi_portal_group_t *tpg,
	se_lun_t *iscsi_lun,
	se_lun_acl_t *lacl)
{
	iscsi_node_acl_t *nacl;

	if (!(nacl = iscsi_tpg_get_initiator_node_acl(tpg, lacl->initiatorname)))
		return(ERR_DELLUNACL_NODE_ACL_MISSING);

	spin_lock(&iscsi_lun->lun_acl_lock);
	REMOVE_ENTRY_FROM_LIST(lacl, iscsi_lun->lun_acl_head, iscsi_lun->lun_acl_tail);
	spin_unlock(&iscsi_lun->lun_acl_lock);
	
	iscsi_update_device_list_for_node(iscsi_lun, lacl->mapped_lun,
		ISCSI_LUNFLAGS_NO_ACCESS, nacl, tpg, 0);
	
	lacl->iscsi_lun = NULL;

	PYXPRINT("iSCSI_TPG[%hu]_LUN[%u] - Removed ACL for iSCSI InitiatorNode:"
		" %s Mapped LUN: %u\n", tpg->tpgt, iscsi_lun->iscsi_lun,
			lacl->initiatorname, lacl->mapped_lun);
	return(0);
}

extern void iscsi_dev_free_initiator_node_lun_acl (
	iscsi_portal_group_t *tpg,
	se_lun_acl_t *lacl)
{
	PYXPRINT("iSCSI_TPG[%hu] - Freeing ACL for iSCSI InitiatorNode: %s"
		" Mapped LUN: %u\n", tpg->tpgt, lacl->initiatorname, lacl->mapped_lun);

	kfree(lacl);
	return;
}
