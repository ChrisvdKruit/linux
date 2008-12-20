/*********************************************************************************
 * Filename:  target_core_tpg.c
 *
 * This file contains generic Target Portal Group related functions.
 *
 * Copyright (c) 2002, 2003, 2004, 2005 PyX Technologies, Inc.
 * Copyright (c) 2005, 2006, 2007 SBE, Inc. 
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


#define TARGET_CORE_TPG_C

#include <linux/net.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/in.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

#include <iscsi_linux_os.h>
#include <iscsi_linux_defs.h>
#include <iscsi_lists.h>
#include <iscsi_target_error.h>

#include <target_core_base.h>
#include <target_core_device.h>
#include <target_core_hba.h>
#include <target_core_transport.h>
#include <target_core_plugin.h>
#include <target_core_seobj.h>
#include <target_core_fabric_ops.h>

#undef TARGET_CORE_TPG_C

extern se_global_t *se_global;

/*	core_clear_initiator_node_from_tpg():
 *
 *
 */
static void core_clear_initiator_node_from_tpg (
	se_node_acl_t *nacl,
	se_portal_group_t *tpg)
{
	int i;
	se_dev_entry_t *deve;
	se_lun_t *lun;
	se_lun_acl_t *acl;

	spin_lock_bh(&nacl->device_list_lock);
	for (i = 0; i < TRANSPORT_MAX_LUNS_PER_TPG; i++) {
		deve = &nacl->device_list[i];

		if (!(deve->lun_flags & TRANSPORT_LUNFLAGS_INITIATOR_ACCESS))
			continue;

		if (!deve->se_lun) {
			printk(KERN_ERR "%s device entries device pointer is"
				" NULL, but Initiator has access.\n",
				TPG_TFO(tpg)->get_fabric_name());
			continue;
		}

		lun = deve->se_lun;
		spin_unlock_bh(&nacl->device_list_lock);
		core_update_device_list_for_node(lun, deve->mapped_lun,
			TRANSPORT_LUNFLAGS_NO_ACCESS, nacl, tpg, 0);

		spin_lock(&lun->lun_acl_lock);
		for (acl = lun->lun_acl_head; acl; acl = acl->next) {
			if (!(strcmp(acl->initiatorname, nacl->initiatorname)) &&
			     (acl->mapped_lun == deve->mapped_lun))
				break;
		}

		if (!acl) {
			printk(KERN_ERR "Unable to locate se_lun_acl_t for %s, mapped_lun: %u\n",
				nacl->initiatorname, deve->mapped_lun);
			spin_unlock(&lun->lun_acl_lock);
			spin_lock_bh(&nacl->device_list_lock);
			continue;
		}
		
		REMOVE_ENTRY_FROM_LIST(acl, lun->lun_acl_head, lun->lun_acl_tail);
		spin_unlock(&lun->lun_acl_lock);

		spin_lock_bh(&nacl->device_list_lock);
		kfree(acl);
        }
	spin_unlock_bh(&nacl->device_list_lock);

	return;
}

/*	__core_tpg_get_initiator_node_acl():
 *
 *	spin_lock_bh(&tpg->acl_node_lock); must be held when calling
 */
static se_node_acl_t *__core_tpg_get_initiator_node_acl (
	se_portal_group_t *tpg,
	const char *initiatorname)
{
	se_node_acl_t *acl;
	
	for (acl = tpg->acl_node_head; acl; acl = acl->next) {
		if (!(strcmp(acl->initiatorname, initiatorname)))
			return(acl);
	}

	return(NULL);
}

/*	core_tpg_get_initiator_node_acl():
 *
 *
 */     
extern se_node_acl_t *core_tpg_get_initiator_node_acl (
	se_portal_group_t *tpg,
	unsigned char *initiatorname)
{               
	se_node_acl_t *acl;
		                
	spin_lock_bh(&tpg->acl_node_lock);
	for (acl = tpg->acl_node_head; acl; acl = acl->next) {
		if (!(strcmp(acl->initiatorname, initiatorname)) &&
		   (!(acl->nodeacl_flags & NAF_DYNAMIC_NODE_ACL))) {
			spin_unlock_bh(&tpg->acl_node_lock);
			return(acl);
		}
	}
	spin_unlock_bh(&tpg->acl_node_lock);

	return(NULL);
}

/*	core_tpg_add_node_to_devs():
 *
 *
 */
extern void core_tpg_add_node_to_devs (
	se_node_acl_t *acl,
	se_portal_group_t *tpg)
{
	int i = 0;
	u32 lun_access = 0;
	se_lun_t *lun;

	spin_lock(&tpg->tpg_lun_lock);
	for (i = 0; i < TRANSPORT_MAX_LUNS_PER_TPG; i++) {
		lun = &tpg->tpg_lun_list[i];
		if (lun->lun_status != TRANSPORT_LUN_STATUS_ACTIVE)
			continue;

		spin_unlock(&tpg->tpg_lun_lock);

		/*
		 * By default in LIO-Target $FABRIC_MOD, demo_mode_write_protect is ON, or READ_ONLY;
		 */
		if (!(TPG_TFO(tpg)->tpg_check_demo_mode_write_protect(tpg))) {
			if (LUN_OBJ_API(lun)->get_device_access) {
				if (LUN_OBJ_API(lun)->get_device_access(lun->lun_type_ptr) == 0)
					lun_access = TRANSPORT_LUNFLAGS_READ_ONLY;
	                        else
                	                lun_access = TRANSPORT_LUNFLAGS_READ_WRITE;
			} else
				lun_access = TRANSPORT_LUNFLAGS_READ_WRITE;
		} else {
                        /*
                         * Allow only optical drives to issue R/W in default RO demo mode.
                         */
                        if (LUN_OBJ_API(lun)->get_device_type(lun->lun_type_ptr) == TYPE_DISK)
                                lun_access = TRANSPORT_LUNFLAGS_READ_ONLY;
			else
				lun_access = TRANSPORT_LUNFLAGS_READ_WRITE;
		}

		printk("TARGET_CORE[%s]->TPG[%u]_LUN[%u] - Adding %s access"
			" for LUN in Demo Mode\n", TPG_TFO(tpg)->get_fabric_name(),
 			TPG_TFO(tpg)->tpg_get_tag(tpg), lun->unpacked_lun,
			(lun_access == TRANSPORT_LUNFLAGS_READ_WRITE) ?
			"READ-WRITE" : "READ-ONLY");

		core_update_device_list_for_node(lun, lun->unpacked_lun,
				lun_access, acl, tpg, 1);
		spin_lock(&tpg->tpg_lun_lock);
	}
	spin_unlock(&tpg->tpg_lun_lock);

	return;
}

/*      core_set_queue_depth_for_node():
 *
 *
 */
static int core_set_queue_depth_for_node (
        se_portal_group_t *tpg,
        se_node_acl_t *acl)
{
        if (!acl->queue_depth) {
                printk(KERN_ERR "Queue depth for %s Initiator Node: %s is 0,"
			"defaulting to 1.\n", TPG_TFO(tpg)->get_fabric_name(),
			acl->initiatorname);
                acl->queue_depth = 1;
        }

        return(0);
}

/*      core_create_device_list_for_node():
 *
 *
 */
static int core_create_device_list_for_node (se_node_acl_t *nacl)
{
        if (!(nacl->device_list = (se_dev_entry_t *) kzalloc(
                        sizeof(se_dev_entry_t) * TRANSPORT_MAX_LUNS_PER_TPG, GFP_KERNEL))) {
                printk(KERN_ERR "Unable to allocate memory for se_node_acl_t->device_list\n");
                return(-1);
        }
        memset(nacl->device_list, 0, sizeof(se_dev_entry_t) *
                        TRANSPORT_MAX_LUNS_PER_TPG);

        return(0);
}

/*	core_tpg_check_initiator_node_acl()
 *
 *
 */
extern se_node_acl_t *core_tpg_check_initiator_node_acl (
	se_portal_group_t *tpg,
	unsigned char *initiatorname)
{
	se_node_acl_t *acl;

	if ((acl = core_tpg_get_initiator_node_acl(tpg, initiatorname)))
		return(acl);

	if (!(TPG_TFO(tpg)->tpg_check_demo_mode(tpg)))
		return(NULL);

	if (!(acl = (se_node_acl_t *) kzalloc(
			sizeof(se_node_acl_t), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate memory for se_node_acl_t.\n");
		return(NULL);
	}

	spin_lock_init(&acl->device_list_lock);
	spin_lock_init(&acl->nacl_sess_lock);
	acl->queue_depth = TPG_TFO(tpg)->tpg_get_default_depth(tpg);
	snprintf(acl->initiatorname, TRANSPORT_IQN_LEN, "%s", initiatorname);
	acl->se_tpg = tpg;
#ifdef SNMP_SUPPORT
        acl->acl_index = scsi_get_new_index(SCSI_AUTH_INTR_INDEX);
        spin_lock_init(&acl->stats_lock);
#endif /* SNMP_SUPPORT */
	acl->nodeacl_flags |= NAF_DYNAMIC_NODE_ACL;

	TPG_TFO(tpg)->set_default_node_attributes(acl);

	if (core_create_device_list_for_node(acl)  < 0) {
		kfree(acl);
		return(NULL);
	}

	if (core_set_queue_depth_for_node(tpg, acl) < 0) {
		core_free_device_list_for_node(acl, tpg);
		kfree(acl);
		return(NULL);
	}

	core_tpg_add_node_to_devs(acl, tpg);

	spin_lock_bh(&tpg->acl_node_lock);
	ADD_ENTRY_TO_LIST(acl, tpg->acl_node_head, tpg->acl_node_tail);
	tpg->num_node_acls++;
	spin_unlock_bh(&tpg->acl_node_lock);

	printk("%s_TPG[%u] - Added DYNAMIC ACL with TCQ Depth: %d for %s"
		" Initiator Node: %s\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), acl->queue_depth,
		TPG_TFO(tpg)->get_fabric_name(), initiatorname);

	return(acl);
}

EXPORT_SYMBOL(core_tpg_check_initiator_node_acl);

/*	core_tpg_free_node_acls():
 *
 *
 */
extern void core_tpg_free_node_acls (se_portal_group_t *tpg)
{
	se_node_acl_t *acl = NULL, *acl_next = NULL;

	spin_lock_bh(&tpg->acl_node_lock);
	acl = tpg->acl_node_head;
	while (acl) {
		acl_next = acl->next;

		/*
		 * The kfree() for dynamically allocated Node ACLS is done in
		 * iscsi_close_session().
		 */
		if (acl->nodeacl_flags & NAF_DYNAMIC_NODE_ACL) {
			acl = acl_next;
			continue;
		}

		kfree(acl);
		tpg->num_node_acls--;
		acl = acl_next;
	}
	tpg->acl_node_head = tpg->acl_node_tail = NULL;
	spin_unlock_bh(&tpg->acl_node_lock);

	return;
}	

EXPORT_SYMBOL(core_tpg_free_node_acls);

extern void core_tpg_clear_object_luns (se_portal_group_t *tpg)
{
	int i, ret;
	se_lun_t *lun;

	spin_lock(&tpg->tpg_lun_lock);
	for (i = 0; i < TRANSPORT_MAX_LUNS_PER_TPG; i++) {
		lun = &tpg->tpg_lun_list[i];

		if ((lun->lun_status != TRANSPORT_LUN_STATUS_ACTIVE) ||
		    (lun->lun_type_ptr == NULL))
			continue;

		spin_unlock(&tpg->tpg_lun_lock);
		ret = LUN_OBJ_API(lun)->del_obj_from_lun(tpg, lun);
		spin_lock(&tpg->tpg_lun_lock);
	}
	spin_unlock(&tpg->tpg_lun_lock);

	return;
}

EXPORT_SYMBOL(core_tpg_clear_object_luns);

/*	core_tpg_add_initiator_node_acl():
 *
 *
 */
extern se_node_acl_t *core_tpg_add_initiator_node_acl (
	se_portal_group_t *tpg,
	void *fabric_acl_ptr,
	const char *initiatorname,
	u32 queue_depth,
	int *ret)
{
	se_node_acl_t *acl = NULL;

	spin_lock_bh(&tpg->acl_node_lock);
	if ((acl = __core_tpg_get_initiator_node_acl(tpg, initiatorname))) {
		if (acl->nodeacl_flags & NAF_DYNAMIC_NODE_ACL) {
			acl->nodeacl_flags &= ~NAF_DYNAMIC_NODE_ACL;
			printk("%s_TPG[%u] - Replacing dynamic ACL for"
				" %s\n", TPG_TFO(tpg)->get_fabric_name(),
				TPG_TFO(tpg)->tpg_get_tag(tpg), initiatorname);
			spin_unlock_bh(&tpg->acl_node_lock);
			goto done;
		}

		printk(KERN_ERR "ACL entry for %s Initiator"
			" Node %s already exists for TPG %u, ignoring"
			" request.\n",  TPG_TFO(tpg)->get_fabric_name(),
			initiatorname, TPG_TFO(tpg)->tpg_get_tag(tpg));
		spin_unlock_bh(&tpg->acl_node_lock);
		*ret = ERR_ADDINITACL_ACL_EXISTS;
		return(NULL);
	}
	spin_unlock_bh(&tpg->acl_node_lock);

	if (!(acl = (se_node_acl_t *) kzalloc(
			sizeof(se_node_acl_t), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate memory for senode_acl_t.\n");
		*ret = ERR_NO_MEMORY;
		return(NULL);
	}

	spin_lock_init(&acl->device_list_lock);
	acl->fabric_acl_ptr = fabric_acl_ptr;
	acl->queue_depth = queue_depth;
	snprintf(acl->initiatorname, TRANSPORT_IQN_LEN, "%s", initiatorname);
	acl->se_tpg = tpg;
#ifdef SNMP_SUPPORT
	acl->acl_index = scsi_get_new_index(SCSI_AUTH_INTR_INDEX);
	spin_lock_init(&acl->stats_lock);
#endif /* SNMP_SUPPORT */

	TPG_TFO(tpg)->set_default_node_attributes(acl);
      
	if (core_create_device_list_for_node(acl)  < 0) {
		kfree(acl);
		*ret = ERR_NO_MEMORY;
		return(NULL);
	}
	
	if (core_set_queue_depth_for_node(tpg, acl) < 0) {
		core_free_device_list_for_node(acl, tpg);
		kfree(acl);
		*ret = ERR_ADDINITACL_QUEUE_SET_FAILED;
		return(NULL);
	}
	
	spin_lock_bh(&tpg->acl_node_lock);
	ADD_ENTRY_TO_LIST(acl, tpg->acl_node_head, tpg->acl_node_tail);
	tpg->num_node_acls++;
	spin_unlock_bh(&tpg->acl_node_lock);

done:
	printk("%s_TPG[%hu] - Added ACL with TCQ Depth: %d for %s"
		" Initiator Node: %s\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), acl->queue_depth,
		TPG_TFO(tpg)->get_fabric_name(), initiatorname);

	return(acl);
}

EXPORT_SYMBOL(core_tpg_add_initiator_node_acl);

/*	core_tpg_del_initiator_node_acl():
 *
 *
 */
extern int core_tpg_del_initiator_node_acl (
	se_portal_group_t *tpg,
	const char *initiatorname,
	int force)
{
	se_session_t *sess, *init_sess = NULL;
	se_node_acl_t *acl;
	int dynamic_acl = 0;

	spin_lock_bh(&tpg->acl_node_lock);
	if (!(acl = __core_tpg_get_initiator_node_acl(tpg, initiatorname))) {
		printk(KERN_ERR "Access Control List entry for %s Initiator"
			" Node %s does not exists for TPG %hu, ignoring"
			" request.\n", TPG_TFO(tpg)->get_fabric_name(),
			initiatorname, TPG_TFO(tpg)->tpg_get_tag(tpg));
		spin_unlock_bh(&tpg->acl_node_lock);
		return(ERR_INITIATORACL_DOES_NOT_EXIST);
	}
	if (acl->nodeacl_flags & NAF_DYNAMIC_NODE_ACL) {
		acl->nodeacl_flags &= ~NAF_DYNAMIC_NODE_ACL;
		dynamic_acl = 1;
	}
	spin_unlock_bh(&tpg->acl_node_lock);

	spin_lock_bh(&tpg->session_lock);
	list_for_each_entry(sess, &tpg->tpg_sess_list, sess_list) {
		if (sess->se_node_acl != acl)
			continue;

		if (!force) {
			printk(KERN_ERR "Unable to delete Access Control List for"
			" %s Initiator Node: %s while session is operational."
			"  To forcefully delete the session use the \"force=1\""
				" parameter.\n", TPG_TFO(tpg)->get_fabric_name(),
				initiatorname);
			spin_unlock_bh(&tpg->session_lock);

			spin_lock_bh(&tpg->acl_node_lock);
			if (dynamic_acl)
				acl->nodeacl_flags |= NAF_DYNAMIC_NODE_ACL;
			spin_unlock_bh(&tpg->acl_node_lock);
			return(ERR_INITIATORACL_SESSION_EXISTS);
		}
		/*
		 * Determine if the session needs to be closed by our context.
		 */
		if (!(TPG_TFO(tpg)->shutdown_session(sess)))
			continue;

		init_sess = sess;
		break;
	}
	spin_unlock_bh(&tpg->session_lock);

	spin_lock_bh(&tpg->acl_node_lock);
	REMOVE_ENTRY_FROM_LIST(acl, tpg->acl_node_head, tpg->acl_node_tail);
	tpg->num_node_acls--;
	spin_unlock_bh(&tpg->acl_node_lock);

	/*
	 * If the $FABRIC_MOD session for the Initiator Node ACL exists,
	 * forcefully shutdown the $FABRIC_MOD session/nexus.
	 */
	if (init_sess)
		TPG_TFO(tpg)->close_session(init_sess);		

	core_clear_initiator_node_from_tpg(acl, tpg);
	core_free_device_list_for_node(acl, tpg);

	TPG_TFO(tpg)->tpg_release_node_acl(tpg, acl);
	acl->fabric_acl_ptr = NULL;

	printk("%s_TPG[%hu] - Deleted ACL with TCQ Depth: %d for %s"
		" Initiator Node: %s\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), acl->queue_depth,
		TPG_TFO(tpg)->get_fabric_name(), initiatorname);

	kfree(acl);
	return(0);
}

EXPORT_SYMBOL(core_tpg_del_initiator_node_acl);

/*	core_tpg_set_initiator_node_queue_depth():
 *
 *
 */
extern int core_tpg_set_initiator_node_queue_depth (
	se_portal_group_t *tpg,
	unsigned char *initiatorname,
	u32 queue_depth,
	int force)
{
	se_session_t *sess, *init_sess = NULL;
	se_node_acl_t *acl;
	int dynamic_acl = 0;
	
	spin_lock_bh(&tpg->acl_node_lock);
	if (!(acl = __core_tpg_get_initiator_node_acl(tpg, initiatorname))) {
		printk(KERN_ERR "Access Control List entry for %s Initiator"
			" Node %s does not exists for TPG %hu, ignoring"
			" request.\n", TPG_TFO(tpg)->get_fabric_name(),
			initiatorname, TPG_TFO(tpg)->tpg_get_tag(tpg));
		spin_unlock_bh(&tpg->acl_node_lock);
		return(-ENODEV);
	}
	if (acl->nodeacl_flags & NAF_DYNAMIC_NODE_ACL) {
		acl->nodeacl_flags &= ~NAF_DYNAMIC_NODE_ACL;
		dynamic_acl = 1;
	}
	spin_unlock_bh(&tpg->acl_node_lock);

	spin_lock_bh(&tpg->session_lock);
	list_for_each_entry(sess, &tpg->tpg_sess_list, sess_list) {
		if (sess->se_node_acl != acl)
			continue;

		if (!force) {
			printk(KERN_ERR "Unable to change queue depth for %s Initiator"
				" Node: %s while session is operational.  To forcefully"
				" change the queue depth and force session reinstatement"
				" use the \"force=1\" parameter.\n",
				TPG_TFO(tpg)->get_fabric_name(), initiatorname);
			spin_unlock_bh(&tpg->session_lock);

			spin_lock_bh(&tpg->acl_node_lock);
			if (dynamic_acl)
				acl->nodeacl_flags |= NAF_DYNAMIC_NODE_ACL;
			spin_unlock_bh(&tpg->acl_node_lock);
			return(-EEXIST);
		}
		/*
		 * Determine if the session needs to be closed by our context.
		 */
		if (!(TPG_TFO(tpg)->shutdown_session(sess)))
			continue;

		init_sess = sess;
		break;
	}

	/*
	 * User has requested to change the queue depth for a Initiator Node.
	 * Change the value in the Node's se_node_acl_t, and call
	 * core_set_queue_depth_for_node() to add the requested queue depth.
	 *
	 * Finally call  TPG_TFO(tpg)->close_session() to force session
	 * reinstatement to occur if there is an active session for the
	 * $FABRIC_MOD Initiator Node in question.
	 */
	acl->queue_depth = queue_depth;

	if (core_set_queue_depth_for_node(tpg, acl) < 0) {
		spin_unlock_bh(&tpg->session_lock);
		/*
		 * Force session reinstatement if core_set_queue_depth_for_node()
		 * failed, because we assume the $FABRIC_MOD has already the set
		 * session reinstatement bit from TPG_TFO(tpg)->shutdown_session().
		 * called above.
		 */
		if (init_sess)
			TPG_TFO(tpg)->close_session(init_sess);

		spin_lock_bh(&tpg->acl_node_lock);
		if (dynamic_acl)
			acl->nodeacl_flags |= NAF_DYNAMIC_NODE_ACL;
		spin_unlock_bh(&tpg->acl_node_lock);
		return(-EINVAL);
	}
	spin_unlock_bh(&tpg->session_lock);
	/*
	 * If the $FABRIC_MOD session for the Initiator Node ACL exists,
	 * forcefully shutdown the $FABRIC_MOD session/nexus.
	 */
	if (init_sess) 
		TPG_TFO(tpg)->close_session(init_sess);

	printk("Successfuly changed queue depth to: %d for Initiator Node:"
		" %s on %s Target Portal Group: %u\n", queue_depth,
		initiatorname, TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg));

	spin_lock_bh(&tpg->acl_node_lock);
	if (dynamic_acl)
		acl->nodeacl_flags |= NAF_DYNAMIC_NODE_ACL;
	spin_unlock_bh(&tpg->acl_node_lock);
		
	return(0);
}

EXPORT_SYMBOL(core_tpg_set_initiator_node_queue_depth);

extern se_portal_group_t *core_tpg_register (
	struct target_core_fabric_ops *tfo,
	void *tpg_fabric_ptr,
	int se_tpg_type)
{
	se_lun_t *lun;
	se_portal_group_t *se_tpg;
	u32 i;

	if (!(se_tpg = kzalloc(sizeof(se_portal_group_t), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate se_portal_group_t\n");
		return(ERR_PTR(-ENOMEM));
	}

	if (!(se_tpg->tpg_lun_list = kzalloc((sizeof(se_lun_t) *
				TRANSPORT_MAX_LUNS_PER_TPG), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate se_portal_group_t->tpg_lun_list\n");
		kfree(se_tpg);
		return(ERR_PTR(-ENOMEM));
	}

	for (i = 0; i < TRANSPORT_MAX_LUNS_PER_TPG; i++) {
		lun = &se_tpg->tpg_lun_list[i];
		lun->unpacked_lun = i;
		lun->lun_type_ptr = NULL;
		lun->persistent_reservation_check = &core_tpg_persistent_reservation_check;
		lun->persistent_reservation_release = &core_tpg_persistent_reservation_release;
		lun->persistent_reservation_reserve = &core_tpg_persistent_reservation_reserve;
		lun->lun_status = TRANSPORT_LUN_STATUS_FREE;
		spin_lock_init(&lun->lun_acl_lock);
		spin_lock_init(&lun->lun_cmd_lock);
		spin_lock_init(&lun->lun_reservation_lock);
		spin_lock_init(&lun->lun_sep_lock);
	}

	se_tpg->se_tpg_type = se_tpg_type;
	se_tpg->se_tpg_fabric_ptr = tpg_fabric_ptr;
	se_tpg->se_tpg_tfo = tfo;
	INIT_LIST_HEAD(&se_tpg->se_tpg_list);
	INIT_LIST_HEAD(&se_tpg->tpg_sess_list);
	spin_lock_init(&se_tpg->acl_node_lock);
	spin_lock_init(&se_tpg->session_lock);
	spin_lock_init(&se_tpg->tpg_lun_lock);
#warning FIXME: Add se_portal_group to list..

	printk("TARGET_CORE[%s]: Allocated %s se_portal_group_t for endpoint:"
		" %s, Portal Tag: %u\n", tfo->get_fabric_name(),
		(se_tpg->se_tpg_type == TRANSPORT_TPG_TYPE_NORMAL) ?
		"Normal" : "Discovery", (tfo->tpg_get_wwn(se_tpg) == NULL) ?
		"None" : tfo->tpg_get_wwn(se_tpg), tfo->tpg_get_tag(se_tpg));

	return(se_tpg);
}

EXPORT_SYMBOL(core_tpg_register);

extern int core_tpg_deregister (se_portal_group_t *se_tpg)
{
	printk("TARGET_CORE[%s]: Deallocating %s se_portal_group_t for"
		" endpoint: %s Portal Tag %u\n",
		(se_tpg->se_tpg_type == TRANSPORT_TPG_TYPE_NORMAL) ?
		"Normal" : "Discovery", TPG_TFO(se_tpg)->get_fabric_name(),
		TPG_TFO(se_tpg)->tpg_get_wwn(se_tpg),
		TPG_TFO(se_tpg)->tpg_get_tag(se_tpg));

#warning FIXME: Release se_portal_group from list
	
	se_tpg->se_tpg_fabric_ptr = NULL;
	kfree(se_tpg->tpg_lun_list);
	kfree(se_tpg);
	return(0);
}

EXPORT_SYMBOL(core_tpg_deregister);

extern se_lun_t *core_tpg_pre_addlun (
	se_portal_group_t *tpg,
	u32 unpacked_lun,
	int *ret)
{
	se_lun_t *lun;
	
	if (unpacked_lun > (TRANSPORT_MAX_LUNS_PER_TPG-1)) {
		printk(KERN_ERR "%s LUN: %u exceeds TRANSPORT_MAX_LUNS_PER_TPG-1:"
			" %u for Target Portal Group: %u\n",
			TPG_TFO(tpg)->get_fabric_name(),
			unpacked_lun, TRANSPORT_MAX_LUNS_PER_TPG-1,
			TPG_TFO(tpg)->tpg_get_tag(tpg));
		*ret = ERR_LUN_EXCEEDS_MAX;
		return(NULL);
	}

	spin_lock(&tpg->tpg_lun_lock);
	lun = &tpg->tpg_lun_list[unpacked_lun];
	if (lun->lun_status == TRANSPORT_LUN_STATUS_ACTIVE) {
		printk(KERN_ERR "TPG Logical Unit Number: %u is already active"
			" on %s Target Portal Group: %u, ignoring request.\n",
			unpacked_lun, TPG_TFO(tpg)->get_fabric_name(),
			TPG_TFO(tpg)->tpg_get_tag(tpg));
		spin_unlock(&tpg->tpg_lun_lock);
		*ret = ERR_ADDLUN_ALREADY_ACTIVE;
		return(NULL);
	}
	spin_unlock(&tpg->tpg_lun_lock);

	return(lun);
}

EXPORT_SYMBOL(core_tpg_pre_addlun);
	
extern int core_tpg_post_addlun (
	se_portal_group_t *tpg,
	se_lun_t *lun,
	int lun_type,
	u32 lun_access,
	void *lun_ptr,
	struct se_obj_lun_type_s *obj_api)
{
	lun->lun_obj_api = obj_api;
	lun->lun_type_ptr = lun_ptr;
	if (LUN_OBJ_API(lun)->export_obj(lun_ptr, tpg, lun) < 0) {
		lun->lun_type_ptr = NULL;
		lun->lun_obj_api = NULL;
		return(-1);
	}
	
	spin_lock(&tpg->tpg_lun_lock);
	lun->lun_access = lun_access;
	lun->lun_type = lun_type;
	lun->lun_status = TRANSPORT_LUN_STATUS_ACTIVE;
	spin_unlock(&tpg->tpg_lun_lock);

	return(0);
}

EXPORT_SYMBOL(core_tpg_post_addlun);

extern se_lun_t *core_tpg_pre_dellun (
	se_portal_group_t *tpg,
	u32 unpacked_lun,
	int lun_type,
	int *ret)
{
	se_lun_t *lun;
	
	if (unpacked_lun > (TRANSPORT_MAX_LUNS_PER_TPG-1)) {
		printk(KERN_ERR "%s LUN: %u exceeds TRANSPORT_MAX_LUNS_PER_TPG-1:"
			" %u for Target Portal Group: %u\n",
			TPG_TFO(tpg)->get_fabric_name(), unpacked_lun,
			TRANSPORT_MAX_LUNS_PER_TPG-1,
			TPG_TFO(tpg)->tpg_get_tag(tpg));
		*ret = ERR_LUN_EXCEEDS_MAX;
		return(NULL);
	}

	spin_lock(&tpg->tpg_lun_lock);
	lun = &tpg->tpg_lun_list[unpacked_lun];
	if (lun->lun_status != TRANSPORT_LUN_STATUS_ACTIVE) {
		printk(KERN_ERR "%s Logical Unit Number: %u is not active on"
			" Target Portal Group: %u, ignoring request.\n",
			TPG_TFO(tpg)->get_fabric_name(), unpacked_lun,
			TPG_TFO(tpg)->tpg_get_tag(tpg));
		spin_unlock(&tpg->tpg_lun_lock);
		*ret = ERR_DELLUN_NOT_ACTIVE;
		return(NULL);
	}

	if (lun->lun_type != lun_type) {
		printk(KERN_ERR "%s Logical Unit Number: %u type: %d does not"
			" match passed type: %d\n",
			TPG_TFO(tpg)->get_fabric_name(),
			unpacked_lun, lun->lun_type, lun_type);
		spin_unlock(&tpg->tpg_lun_lock);
		*ret = ERR_DELLUN_TYPE_MISMATCH;
		return(NULL);
	}
	spin_unlock(&tpg->tpg_lun_lock);

	core_clear_lun_from_tpg(lun, tpg);

	return(lun);
}

EXPORT_SYMBOL(core_tpg_pre_dellun);

extern int core_tpg_post_dellun (
	se_portal_group_t *tpg,
	se_lun_t *lun)
{
	se_lun_acl_t *acl, *acl_next;
	
	transport_clear_lun_from_sessions(lun);

	LUN_OBJ_API(lun)->unexport_obj(lun->lun_type_ptr, tpg, lun);
	LUN_OBJ_API(lun)->release_obj(lun->lun_type_ptr);
	
	spin_lock(&tpg->tpg_lun_lock);
	lun->lun_status = TRANSPORT_LUN_STATUS_FREE;
	lun->lun_type = 0;
	lun->lun_type_ptr = NULL;
	spin_unlock(&tpg->tpg_lun_lock);

	spin_lock(&lun->lun_acl_lock);
	acl = lun->lun_acl_head;
	while (acl) {
		acl_next = acl->next;
		kfree(acl);
		acl = acl_next;
	}
	lun->lun_acl_head = lun->lun_acl_tail = NULL;
	spin_unlock(&lun->lun_acl_lock);

	return(0);	
}

EXPORT_SYMBOL(core_tpg_post_dellun);
