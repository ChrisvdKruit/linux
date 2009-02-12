/*********************************************************************************
 * Filename:  target_core_device.c (based on iscsi_target_device.c)
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


#define TARGET_CORE_DEVICE_C

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
#include <scsi/scsi.h>

#include <iscsi_linux_os.h>
#include <iscsi_linux_defs.h>
#include <iscsi_lists.h>

#include <target_core_base.h>
#include <iscsi_target_error.h>
#include <target_core_device.h>
#include <target_core_hba.h>
#include <target_core_alua.h>
#include <target_core_pr.h>
#include <target_core_tpg.h>
#include <target_core_transport.h>
#include <target_core_fabric_ops.h>

#include <target_core_plugin.h>
#include <target_core_seobj.h>

#undef TARGET_CORE_DEVICE_C

extern se_global_t *se_global;

extern struct block_device *linux_blockdevice_claim_bd (struct block_device *bd, void *claim_ptr)
{
	if (blkdev_get(bd, FMODE_WRITE|FMODE_READ) < 0)
		return(NULL);	
	/*
	 * If no claim pointer was passed from claimee, use struct block_device.
	 */
	if (!claim_ptr)
		claim_ptr = (void *)bd;

	if (bd_claim(bd, claim_ptr) < 0) {
		blkdev_put(bd, FMODE_WRITE|FMODE_READ);
		return(NULL);
	}
	
	return(bd);
}

extern struct block_device *__linux_blockdevice_claim (int major, int minor, void *claim_ptr, int *ret)
{
	dev_t dev;
	struct block_device *bd;

	dev = MKDEV(major, minor);

	if (!(bd = bdget(dev))) {
		*ret = -1;
		return(NULL);
	}

	if (blkdev_get(bd, FMODE_WRITE|FMODE_READ) < 0) {
		*ret = -1;
		return(NULL);
	}
	/*
	 * If no claim pointer was passed from claimee, use struct block_device.
	 */
	if (!claim_ptr)
		claim_ptr = (void *)bd;

	if (bd_claim(bd, claim_ptr) < 0) {
#if 0
		printk("Using previously claimed Major:Minor - %d:%d\n",
				major, minor);
#endif
		*ret = 0;
		return(bd);
	}

	*ret = 1;
	return(bd);
}

extern struct block_device *linux_blockdevice_claim (int major, int minor, void *claim_ptr)
{
	dev_t dev;
	struct block_device *bd;

	dev = MKDEV(major, minor);

	if (!(bd = bdget(dev)))
		return(NULL);

	if (blkdev_get(bd, FMODE_WRITE|FMODE_READ) < 0)
		return(NULL);
	/*
	 * If no claim pointer was passed from claimee, use struct block_device.
	 */
	if (!claim_ptr)
		claim_ptr = (void *)bd;

	if (bd_claim(bd, claim_ptr) < 0) {
		blkdev_put(bd, FMODE_WRITE|FMODE_READ);
		return(NULL);
	}
	
	return(bd);
}

extern int linux_blockdevice_release (int major, int minor, struct block_device *bd_p)
{
	dev_t dev;
	struct block_device *bd;

	if (!bd_p) {
		dev = MKDEV(major, minor);

		if (!(bd = bdget(dev)))
			return(-1);
	} else
		bd = bd_p;

	bd_release(bd);
	blkdev_put(bd, FMODE_WRITE|FMODE_READ);

	return(0);
}

extern int linux_blockdevice_check (int major, int minor)
{
	struct block_device *bd;

	if (!(bd = linux_blockdevice_claim(major, minor, NULL)))
		return(-1);
	/*
	 * Blockdevice was able to be claimed, now unclaim it and return success.
	 */
	linux_blockdevice_release(major, minor, NULL);

	return(0);
}

EXPORT_SYMBOL(linux_blockdevice_check);

extern int se_check_devices_access (se_hba_t *hba)
{
	int ret = 0;
	se_device_t *dev = NULL, *dev_next = NULL;

	spin_lock(&hba->device_lock);
	dev = hba->device_head;
	while (dev) {
		dev_next = dev->next;

		if (DEV_OBJ_API(dev)->check_count(&dev->dev_feature_obj) != 0) {
			printk(KERN_ERR "check_count(&dev->dev_feature_obj): %u\n",
				DEV_OBJ_API(dev)->check_count(&dev->dev_feature_obj));
			ret = -1;
		}

		dev = dev_next;
	}
	spin_unlock(&hba->device_lock);

	return(ret);		
}

/*	se_disable_devices_for_hba():
 *
 *
 */
extern void se_disable_devices_for_hba (se_hba_t *hba)
{
	se_device_t *dev, *dev_next;

	spin_lock(&hba->device_lock);
	dev = hba->device_head;
	while (dev) {
		dev_next = dev->next;
		
		spin_lock(&dev->dev_status_lock);
		if ((dev->dev_status & TRANSPORT_DEVICE_ACTIVATED) ||
		    (dev->dev_status & TRANSPORT_DEVICE_DEACTIVATED) ||
		    (dev->dev_status & TRANSPORT_DEVICE_OFFLINE_ACTIVATED) ||
		    (dev->dev_status & TRANSPORT_DEVICE_OFFLINE_DEACTIVATED)) {
			dev->dev_status |= TRANSPORT_DEVICE_SHUTDOWN;
			dev->dev_status &= ~TRANSPORT_DEVICE_ACTIVATED;
			dev->dev_status &= ~TRANSPORT_DEVICE_DEACTIVATED;
			dev->dev_status &= ~TRANSPORT_DEVICE_OFFLINE_ACTIVATED;
			dev->dev_status &= ~TRANSPORT_DEVICE_OFFLINE_DEACTIVATED;

			up(&dev->dev_queue_obj->thread_sem);
		}
		spin_unlock(&dev->dev_status_lock);

		dev = dev_next;
	}
	spin_unlock(&hba->device_lock);

	return;
}

extern int __transport_get_lun_for_cmd (
	se_cmd_t *se_cmd,
	u32 unpacked_lun)
{
	se_dev_entry_t *deve;
	se_lun_t *se_lun = NULL;
	se_session_t *se_sess = SE_SESS(se_cmd);
	unsigned long flags;
	int read_only = 0;

	spin_lock_bh(&SE_NODE_ACL(se_sess)->device_list_lock);
	deve = se_cmd->se_deve = &SE_NODE_ACL(se_sess)->device_list[unpacked_lun];
	if (deve->lun_flags & TRANSPORT_LUNFLAGS_INITIATOR_ACCESS) {
                if (se_cmd) {
                        deve->total_cmds++;
                        deve->total_bytes += se_cmd->data_length;

                        if (se_cmd->data_direction == SE_DIRECTION_WRITE) {
                                if (deve->lun_flags & TRANSPORT_LUNFLAGS_READ_ONLY) {
                                        read_only = 1;
                                        goto out;
                                }
#ifdef SNMP_SUPPORT
                                deve->write_bytes += se_cmd->data_length;
#endif /* SNMP_SUPPORT */
                        } else if (se_cmd->data_direction == SE_DIRECTION_READ) {
#ifdef SNMP_SUPPORT
                                deve->read_bytes += se_cmd->data_length;
#endif /* SNMP_SUPPORT */
                        }
                }
                deve->deve_cmds++;

                se_lun = se_cmd->se_lun = deve->se_lun;
                se_cmd->orig_fe_lun = unpacked_lun;
                se_cmd->se_orig_obj_api = ISCSI_LUN(se_cmd)->lun_obj_api;
                se_cmd->se_orig_obj_ptr = ISCSI_LUN(se_cmd)->lun_type_ptr;
		se_cmd->se_cmd_flags |= SCF_SE_LUN_CMD;
        }
out:
        spin_unlock_bh(&SE_NODE_ACL(se_sess)->device_list_lock);

        if (!se_lun) {
		if (read_only) {
			se_cmd->scsi_sense_reason = WRITE_PROTECTED;
                        printk("TARGET_CORE[%s]: Detected WRITE_PROTECTED LUN"
				" Access for 0x%08x\n",
				CMD_TFO(se_cmd)->get_fabric_name(),
				unpacked_lun);
		} else {
			se_cmd->scsi_sense_reason = NON_EXISTENT_LUN;
                        printk("TARGET_CORE[%s]: Detected NON_EXISTENT_LUN"
				" Access for 0x%08x\n",
				CMD_TFO(se_cmd)->get_fabric_name(),
				unpacked_lun);
                }
		se_cmd->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
                return(-1);
        }
        /*
         * Determine if the se_lun_t is online.
         */
        if (LUN_OBJ_API(se_lun)->check_online(se_lun->lun_type_ptr) != 0) {
		se_cmd->scsi_sense_reason = NON_EXISTENT_LUN;	
		se_cmd->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
                return(-1);
	}

#ifdef SNMP_SUPPORT
	{
	se_device_t *dev = se_lun->se_dev;
	spin_lock(&dev->stats_lock);
	dev->num_cmds++;
	if (se_cmd->data_direction == SE_DIRECTION_WRITE)
		dev->write_bytes += se_cmd->data_length;
	else if (se_cmd->data_direction == SE_DIRECTION_READ)
		dev->read_bytes += se_cmd->data_length;
	spin_unlock(&dev->stats_lock);
	}
#endif /* SNMP_SUPPORT */

        /*
         * Add the iscsi_cmd_t to the se_lun_t's cmd list.  This list is used
         * for tracking state of iscsi_cmd_ts during LUN shutdown events.
         */
        spin_lock_irqsave(&se_lun->lun_cmd_lock, flags);
        ADD_ENTRY_TO_LIST_PREFIX(l, se_cmd, se_lun->lun_cmd_head, se_lun->lun_cmd_tail);
        atomic_set(&T_TASK(se_cmd)->transport_lun_active, 1);
#if 0
        printk("Adding ITT: 0x%08x to LUN LIST[%d]\n",
		CMD_TFO(se_cmd)->get_task_tag(se_cmd), se_lun->unpacked_lun);
#endif
        spin_unlock_irqrestore(&se_lun->lun_cmd_lock, flags);

	return(0);
}

#warning FIXME: Complete transport_get_lun_for_tmr()
extern int transport_get_lun_for_tmr (
	se_cmd_t *se_cmd,
	u32 unpacked_lun)
{
	return(-1);
}

extern int core_free_device_list_for_node (se_node_acl_t *nacl, se_portal_group_t *tpg)
{
        se_dev_entry_t *deve;
        se_lun_t *lun;
	u32 i;

        if (!nacl->device_list)
                return(0);

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
                spin_lock_bh(&nacl->device_list_lock);
        }
        spin_unlock_bh(&nacl->device_list_lock);

        kfree(nacl->device_list);
        nacl->device_list = NULL;

        return(0);
}

extern void core_dec_lacl_count (se_node_acl_t *se_nacl, se_cmd_t *se_cmd)
{
	se_dev_entry_t *deve;

	spin_lock_bh(&se_nacl->device_list_lock);
	deve = &se_nacl->device_list[se_cmd->orig_fe_lun];
	deve->deve_cmds--;
	spin_unlock_bh(&se_nacl->device_list_lock);
	
	return;
}

extern void core_update_device_list_access (
        u32 mapped_lun,
        u32 lun_access,
        se_node_acl_t *nacl)
{
        se_dev_entry_t *deve;

        spin_lock_bh(&nacl->device_list_lock);
        deve = &nacl->device_list[mapped_lun];
        if (lun_access & TRANSPORT_LUNFLAGS_READ_WRITE) {
                deve->lun_flags &= ~TRANSPORT_LUNFLAGS_READ_ONLY;
                deve->lun_flags |= TRANSPORT_LUNFLAGS_READ_WRITE;
        } else {
                deve->lun_flags &= ~TRANSPORT_LUNFLAGS_READ_WRITE;
                deve->lun_flags |= TRANSPORT_LUNFLAGS_READ_ONLY;
        }
        spin_unlock_bh(&nacl->device_list_lock);

        return;
}

EXPORT_SYMBOL(core_update_device_list_access);

/*      core_update_device_list_for_node():
 *
 *
 */
extern void core_update_device_list_for_node (
        se_lun_t *lun,
        u32 mapped_lun,
        u32 lun_access,
        se_node_acl_t *nacl,
        se_portal_group_t *tpg,
        int enable)
{
        se_dev_entry_t *deve;

        spin_lock_bh(&nacl->device_list_lock);
        deve = &nacl->device_list[mapped_lun];
        if (enable) {
                deve->se_lun = lun;
                deve->mapped_lun = mapped_lun;
                deve->lun_flags |= TRANSPORT_LUNFLAGS_INITIATOR_ACCESS;

                if (lun_access & TRANSPORT_LUNFLAGS_READ_WRITE) {
                        deve->lun_flags &= ~TRANSPORT_LUNFLAGS_READ_ONLY;
                        deve->lun_flags |= TRANSPORT_LUNFLAGS_READ_WRITE;
                } else {
                        deve->lun_flags &= ~TRANSPORT_LUNFLAGS_READ_WRITE;
                        deve->lun_flags |= TRANSPORT_LUNFLAGS_READ_ONLY;
                }
#ifdef SNMP_SUPPORT
                deve->creation_time = get_jiffies_64();
                deve->attach_count++;
#endif /* SNMP_SUPPORT */
        } else {
                deve->se_lun = NULL;
                deve->lun_flags = 0;
                deve->creation_time = 0;
        }
        spin_unlock_bh(&nacl->device_list_lock);

        return;
}

/*      core_clear_lun_from_tpg():
 *
 *
 */
extern void core_clear_lun_from_tpg (se_lun_t *lun, se_portal_group_t *tpg)
{
        se_node_acl_t *nacl, *nacl_next;
        se_dev_entry_t *deve;
	u32 i;

        spin_lock_bh(&tpg->acl_node_lock);
        nacl = tpg->acl_node_head;
        while (nacl) {
                nacl_next = nacl->next;
                spin_unlock_bh(&tpg->acl_node_lock);

                spin_lock_bh(&nacl->device_list_lock);
                for (i = 0; i < TRANSPORT_MAX_LUNS_PER_TPG; i++) {
                        deve = &nacl->device_list[i];
                        if (lun != deve->se_lun)
                                continue;
                        spin_unlock_bh(&nacl->device_list_lock);

                        core_update_device_list_for_node(lun, deve->mapped_lun,
                                TRANSPORT_LUNFLAGS_NO_ACCESS, nacl, tpg, 0);

                        spin_lock_bh(&nacl->device_list_lock);
                }
                spin_unlock_bh(&nacl->device_list_lock);

                spin_lock_bh(&tpg->acl_node_lock);
                nacl = nacl_next;
        }
        spin_unlock_bh(&tpg->acl_node_lock);

        return;
}

extern se_port_t *core_alloc_port (se_device_t *dev)
{
	se_port_t *port, *port_tmp;
	
	if (!(port = kzalloc(sizeof(se_port_t), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate se_port_t\n");
		return(NULL);
	}
	INIT_LIST_HEAD(&port->sep_list);
	INIT_LIST_HEAD(&port->sep_tg_pt_gp_list);
	spin_lock_init(&port->sep_alua_lock);

	spin_lock(&dev->se_port_lock);
	if (dev->dev_port_count == 0x0000ffff) {
		printk(KERN_WARNING "Reached dev->dev_port_count == 0x0000ffff\n");
		spin_unlock(&dev->se_port_lock);
		return(NULL);
	}
again:
	/*
	 * Allocate the next RELATIVE TARGET PORT IDENTIFER for this se_device_t.
	 * Here is the table from spc4r17 section 7.7.3.8.
	 * 
	 *    Table 473 -- RELATIVE TARGET PORT IDENTIFIER field
	 *
	 * Code      Description
	 * 0h        Reserved
	 * 1h        Relative port 1, historically known as port A
	 * 2h        Relative port 2, historically known as port B
	 * 3h to FFFFh    Relative port 3 through 65 535
	 */
	if (!(port->sep_rtpi = dev->dev_rpti_counter++))
		goto again;

	list_for_each_entry(port_tmp, &dev->dev_sep_list, sep_list) {
		/*
		 * Make sure RELATIVE TARGET PORT IDENTIFER is unique
		 * for 16-bit wrap..
		 */
		if (port->sep_rtpi == port_tmp->sep_rtpi) 
			goto again;
	}
	spin_unlock(&dev->se_port_lock);

	return(port);
}

extern void core_export_port (
	se_device_t *dev,
	se_portal_group_t *tpg,
	se_port_t *port,
	se_lun_t *lun)
{
	se_subsystem_dev_t *su_dev = SU_DEV(dev);

	spin_lock(&dev->se_port_lock);
	spin_lock(&lun->lun_sep_lock);
	port->sep_tpg = tpg;
	port->sep_lun = lun;
	lun->lun_sep = port;
	spin_unlock(&lun->lun_sep_lock);

	list_add_tail(&port->sep_list, &dev->dev_sep_list);
	spin_unlock(&dev->se_port_lock);

	if (T10_ALUA(su_dev)->alua_type == SPC3_ALUA_EMULATED) {
		core_alua_attach_tg_pt_gp(port, se_global->default_tg_pt_gp);
		printk("%s/%s: Adding to default ALUA Target Port Group:"
			" core/alua/tg_pt_gps/default_tg_pt_gp\n",
			TRANSPORT(dev)->name, TPG_TFO(tpg)->get_fabric_name());
	}

	dev->dev_port_count++;
#ifdef SNMP_SUPPORT
	port->sep_index = port->sep_rtpi; /* RELATIVE TARGET PORT IDENTIFER */
#endif
	return;
}

/*
 *	Called with se_device_t->se_port_lock spinlock held.
 */
extern void core_release_port (se_device_t *dev, se_port_t *port)
{
	core_alua_put_tg_pt_gp(port, 1);

	list_del(&port->sep_list);
	dev->dev_port_count--;
	kfree(port);

	return;
}

extern int transport_core_report_lun_response (se_cmd_t *se_cmd)
{
	se_dev_entry_t *deve;
	se_lun_t *se_lun;
	se_session_t *se_sess = SE_SESS(se_cmd);
	se_task_t *se_task;
	unsigned char *buf = (unsigned char *)T_TASK(se_cmd)->t_task_buf;
	u32 cdb_offset = 0, lun_count = 0, offset = 8;
	u64 i, lun;

	list_for_each_entry(se_task, &T_TASK(se_cmd)->t_task_list, t_list)
		break;
	
	if (!(se_task)) {
		printk(KERN_ERR "Unable to locate se_task_t for se_cmd_t\n");
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);
	}

	/*
	 * If no se_session_t pointer is present, this se_cmd_t is
	 * coming via a target_core_mod PASSTHROUGH op, and not through
	 * a $FABRIC_MOD.  In that case, report LUN=0 only.
	 */
	if (!(se_sess)) {
		lun = 0;
		buf[offset++] = ((lun >> 56) & 0xff);
		buf[offset++] = ((lun >> 48) & 0xff);
		buf[offset++] = ((lun >> 40) & 0xff);
		buf[offset++] = ((lun >> 32) & 0xff);
		buf[offset++] = ((lun >> 24) & 0xff);
		buf[offset++] = ((lun >> 16) & 0xff);
		buf[offset++] = ((lun >> 8) & 0xff);
		buf[offset++] = (lun & 0xff);
		lun_count = 1;
		goto done;	
	}

	spin_lock_bh(&SE_NODE_ACL(se_sess)->device_list_lock);
	for (i = 0; i < TRANSPORT_MAX_LUNS_PER_TPG; i++) {
		deve = &SE_NODE_ACL(se_sess)->device_list[i];
		if (!(deve->lun_flags & TRANSPORT_LUNFLAGS_INITIATOR_ACCESS))
			continue;
		se_lun = deve->se_lun; 
		/*
		 * We determine the correct LUN LIST LENGTH even once we
		 * have reached the initial allocation length.
		 * See SPC2-R20 7.19.
		 */
		lun_count++;
		if ((cdb_offset + 8) >= se_cmd->data_length)
			continue;

		lun = cpu_to_be64(CMD_TFO(se_cmd)->pack_lun(deve->mapped_lun));
		buf[offset++] = ((lun >> 56) & 0xff);
		buf[offset++] = ((lun >> 48) & 0xff);
		buf[offset++] = ((lun >> 40) & 0xff);
		buf[offset++] = ((lun >> 32) & 0xff);
		buf[offset++] = ((lun >> 24) & 0xff);
		buf[offset++] = ((lun >> 16) & 0xff);
		buf[offset++] = ((lun >> 8) & 0xff);
		buf[offset++] = (lun & 0xff);
		cdb_offset += 8;
	}
	spin_unlock_bh(&SE_NODE_ACL(se_sess)->device_list_lock);

	/*
	 * See SPC3 r07, page 159.
	 */
done:
	lun_count *= 8;
	buf[0] = ((lun_count >> 24) & 0xff);
	buf[1] = ((lun_count >> 16) & 0xff);
	buf[2] = ((lun_count >> 8) & 0xff);
	buf[3] = (lun_count & 0xff);

	return(PYX_TRANSPORT_SENT_TO_TRANSPORT);
}

/*	se_release_device_for_hba():
 *
 *
 */
extern void se_release_device_for_hba (se_device_t *dev)
{
	se_hba_t *hba = dev->se_hba;

	if ((dev->dev_status & TRANSPORT_DEVICE_ACTIVATED) ||
	    (dev->dev_status & TRANSPORT_DEVICE_DEACTIVATED) ||
	    (dev->dev_status & TRANSPORT_DEVICE_SHUTDOWN) ||
	    (dev->dev_status & TRANSPORT_DEVICE_OFFLINE_ACTIVATED) ||
	    (dev->dev_status & TRANSPORT_DEVICE_OFFLINE_DEACTIVATED))
		se_dev_stop(dev);

	transport_generic_free_device(dev);

	spin_lock(&hba->device_lock);
	REMOVE_ENTRY_FROM_LIST(dev, hba->device_head, hba->device_tail);
	hba->dev_count--;
	spin_unlock(&hba->device_lock);

	core_scsi3_free_all_registrations(dev);
	se_release_evpd_for_dev(dev);
		
	kfree(dev->dev_status_queue_obj);
	kfree(dev->dev_queue_obj);
	kfree(dev);

	return;
}

extern void se_release_evpd_for_dev (se_device_t *dev)
{
	t10_evpd_t *evpd, *evpd_tmp;

	spin_lock(&DEV_T10_WWN(dev)->t10_evpd_lock);
	list_for_each_entry_safe(evpd, evpd_tmp,
			&DEV_T10_WWN(dev)->t10_evpd_list, evpd_list) {
		list_del(&evpd->evpd_list);
		kfree(evpd);
	}
	spin_unlock(&DEV_T10_WWN(dev)->t10_evpd_lock);

	return;
}

extern int transport_get_lun_for_cmd (
        se_cmd_t *se_cmd,
	unsigned char *cdb,
        u32 unpacked_lun)
{
	return(__transport_get_lun_for_cmd(se_cmd, unpacked_lun));
}

EXPORT_SYMBOL(transport_get_lun_for_cmd);

/*
 * Called with se_hba_t->device_lock held.
 */
extern void se_clear_dev_ports (se_device_t *dev)
{
	se_hba_t *hba = dev->se_hba;	
	se_lun_t *lun;
	se_portal_group_t *tpg;
	se_port_t *sep, *sep_tmp;

	spin_lock(&dev->se_port_lock);
	list_for_each_entry_safe(sep, sep_tmp, &dev->dev_sep_list, sep_list) {
		spin_unlock(&dev->se_port_lock);
		spin_unlock(&hba->device_lock);

		lun = sep->sep_lun;
		tpg = sep->sep_tpg;
		spin_lock(&lun->lun_sep_lock);
		if (lun->lun_type_ptr == NULL) {
			spin_unlock(&lun->lun_sep_lock);
			continue;
		}
		spin_unlock(&lun->lun_sep_lock);

		LUN_OBJ_API(lun)->del_obj_from_lun(tpg, lun);
		
		spin_lock(&hba->device_lock);
		spin_lock(&dev->se_port_lock);
	}
	spin_unlock(&dev->se_port_lock);

	return;
}

/*	se_free_virtual_device():
 *
 *	Used for IBLOCK, RAMDISK, and FILEIO Transport Drivers.
 */
extern int se_free_virtual_device (se_device_t *dev, se_hba_t *hba)
{
	spin_lock(&hba->device_lock);
	se_clear_dev_ports(dev);
	spin_unlock(&hba->device_lock);

	core_alua_free_lu_gp_mem(dev);	
	se_release_device_for_hba(dev);
	
	return(0);
}

EXPORT_SYMBOL(se_free_virtual_device);

extern void se_dev_start (se_device_t *dev)
{
	se_hba_t *hba = dev->se_hba;
	
        spin_lock(&hba->device_lock);
	DEV_OBJ_API(dev)->inc_count(&dev->dev_obj);
        if (DEV_OBJ_API(dev)->check_count(&dev->dev_obj) == 1) {
		if (dev->dev_status & TRANSPORT_DEVICE_DEACTIVATED) {
			dev->dev_status &= ~TRANSPORT_DEVICE_DEACTIVATED;
			dev->dev_status |= TRANSPORT_DEVICE_ACTIVATED;
		} else if (dev->dev_status & TRANSPORT_DEVICE_OFFLINE_DEACTIVATED) {
			dev->dev_status &= ~TRANSPORT_DEVICE_OFFLINE_DEACTIVATED;
			dev->dev_status |= TRANSPORT_DEVICE_OFFLINE_ACTIVATED;	
		}
	}
        spin_unlock(&hba->device_lock);

	return;
}

extern void se_dev_stop (se_device_t *dev)
{
	se_hba_t *hba = dev->se_hba;

	spin_lock(&hba->device_lock);
	DEV_OBJ_API(dev)->dec_count(&dev->dev_obj);
        if (DEV_OBJ_API(dev)->check_count(&dev->dev_obj) == 0) {
		if (dev->dev_status & TRANSPORT_DEVICE_ACTIVATED) {
			dev->dev_status &= ~TRANSPORT_DEVICE_ACTIVATED;
			dev->dev_status |= TRANSPORT_DEVICE_DEACTIVATED;
		} else if (dev->dev_status & TRANSPORT_DEVICE_OFFLINE_ACTIVATED) {
			dev->dev_status &= ~TRANSPORT_DEVICE_OFFLINE_ACTIVATED;
			dev->dev_status |= TRANSPORT_DEVICE_OFFLINE_DEACTIVATED;
		}
	}
	spin_unlock(&hba->device_lock);

	while (atomic_read(&hba->dev_mib_access_count)) 
		msleep(10);

	return;
}

extern void se_dev_set_default_attribs (se_device_t *dev)
{
	DEV_ATTRIB(dev)->status_thread = DA_STATUS_THREAD;
	DEV_ATTRIB(dev)->status_thread_tur = DA_STATUS_THREAD_TUR;
	DEV_ATTRIB(dev)->emulate_reservations = DA_EMULATE_RESERVATIONS;
	DEV_ATTRIB(dev)->emulate_alua = DA_EMULATE_ALUA;
	/*
	 * max_sectors is based on subsystem plugin dependent requirements.
	 */
	DEV_ATTRIB(dev)->hw_max_sectors = TRANSPORT(dev)->get_max_sectors(dev);
	DEV_ATTRIB(dev)->max_sectors = TRANSPORT(dev)->get_max_sectors(dev);
	/*
	 * queue_depth is based on subsystem plugin dependent requirements.
	 */
	DEV_ATTRIB(dev)->hw_queue_depth = TRANSPORT(dev)->get_queue_depth(dev);
	DEV_ATTRIB(dev)->queue_depth = TRANSPORT(dev)->get_queue_depth(dev);
	/*
	 * task_timeout is based on device type.
	 */
#if 1
	// Disabled by default due to known BUG in some cases when task_timeout fires..
	// task_timeout, status_thread and status_thread_tur may end up being removed in v3.0..
	DEV_ATTRIB(dev)->task_timeout = 0;
#else
	DEV_ATTRIB(dev)->task_timeout = transport_get_default_task_timeout(dev);
#endif

	return;
}

extern int se_dev_set_task_timeout (se_device_t *dev, u32 task_timeout)
{
	if (task_timeout > DA_TASK_TIMEOUT_MAX) {
		printk(KERN_ERR "dev[%p]: Passed task_timeout: %u larger then"
			" DA_TASK_TIMEOUT_MAX\n", dev, task_timeout);
		return(-1);
	} else {
		DEV_ATTRIB(dev)->task_timeout = task_timeout;
		printk("dev[%p]: Set SE Device task_timeout: %u\n", dev, task_timeout);
	}

	return(0);
}

extern int se_dev_set_status_thread (se_device_t *dev, int flag)
{
	if ((flag != 0) && (flag != 1)) {
	 	printk(KERN_ERR "Illegal value %d\n", flag);
		return(-1);		
	}

	if (!flag) {
		if (DEV_ATTRIB(dev)->status_thread) {
			if (DEV_OBJ_API(dev)->check_count(&dev->dev_export_obj)) {
				printk(KERN_ERR "dev[%p]: Unable to stop SE Device Status"
					" Thread while dev_export_obj: %d count exists\n",
					dev, DEV_OBJ_API(dev)->check_count(&dev->dev_export_obj));
				return(-1);
			}
			if (!(dev->dev_flags & DF_DISABLE_STATUS_THREAD))
	        	        DEV_OBJ_API(dev)->stop_status_thread((void *)dev);
		}
	} else {
		if (!(DEV_ATTRIB(dev)->status_thread))
			DEV_OBJ_API(dev)->start_status_thread((void *)dev, 1);
	}

	DEV_ATTRIB(dev)->status_thread = flag;
	printk("dev[%p]: SE Device Status Thread: %s\n", dev, (flag) ?
			"Enabled" : "Disabled");
	return(0);
}

extern int se_dev_set_status_thread_tur (se_device_t *dev, int flag)
{
	int start = 0;

	if ((flag != 0) && (flag != 1)) {
		printk(KERN_ERR "Illegal value %d\n", flag);
		return(-1);
	}

	if (flag && !(DEV_ATTRIB(dev)->status_thread_tur))
		start = 1;

	DEV_ATTRIB(dev)->status_thread_tur = flag;
	if (start)
		transport_start_status_timer(dev);
	printk("dev[%p]: SE Device Status Thread TUR: %s\n", dev, (flag) ?
			"Enabled" : "Disabled");
	return(0);
}

/*
 * Note, this can only be called on unexported SE Device Object.
 */
extern int se_dev_set_queue_depth (se_device_t *dev, u32 queue_depth)
{
	u32 orig_queue_depth = dev->queue_depth;

	if (DEV_OBJ_API(dev)->check_count(&dev->dev_export_obj)) {
		printk(KERN_ERR "dev[%p]: Unable to change SE Device TCQ while"
			" dev_export_obj: %d count exists\n", dev,
			DEV_OBJ_API(dev)->check_count(&dev->dev_export_obj));
		return(-1);
	}
	if (!(queue_depth)) {
		printk(KERN_ERR "dev[%p]: Illegal ZERO value for queue_depth\n", dev);
		return(-1);
	}
	
	if (TRANSPORT(dev)->transport_type == TRANSPORT_PLUGIN_PHBA_PDEV) {
		if (queue_depth > TRANSPORT(dev)->get_queue_depth(dev)) {
			printk(KERN_ERR "dev[%p]: Passed queue_depth: %u exceeds"
				" LIO-Core/SE_Device TCQ: %u\n", dev, queue_depth,
				TRANSPORT(dev)->get_queue_depth(dev));
			return(-1);
		}
	} else {
		if (queue_depth > TRANSPORT(dev)->get_queue_depth(dev)) {
			if (!(TRANSPORT(dev)->get_max_queue_depth)) {
				printk(KERN_ERR "dev[%p]: Unable to locate "
					"get_max_queue_depth() function"
					" pointer\n", dev);
				return(-1);
			}
			if (queue_depth > TRANSPORT(dev)->get_max_queue_depth(dev)) {
				printk(KERN_ERR "dev[%p]: Passed queue_depth: %u exceeds"
				" LIO-Core/SE_Device MAX TCQ: %u\n", dev, queue_depth,
					TRANSPORT(dev)->get_max_queue_depth(dev));
				return(-1);
			}
		}
	}
		
	DEV_ATTRIB(dev)->queue_depth = dev->queue_depth = queue_depth;
	if (queue_depth > orig_queue_depth)
		atomic_add(queue_depth - orig_queue_depth, &dev->depth_left);	
	else if (queue_depth < orig_queue_depth)
		atomic_sub(orig_queue_depth - queue_depth, &dev->depth_left);

	printk("dev[%p]: SE Device TCQ Depth changed to: %u\n", dev, queue_depth);
	return(0);
}

#warning FIXME: Forcing max_sectors greater than get_max_sectors() disabled
extern int se_dev_set_max_sectors (se_device_t *dev, u32 max_sectors)
{
	int force = 0; /* Force setting for VDEVS */

	if (DEV_OBJ_API(dev)->check_count(&dev->dev_export_obj)) {
		printk(KERN_ERR "dev[%p]: Unable to change SE Device max_sectors"
			" while dev_export_obj: %d count exists\n", dev,
			DEV_OBJ_API(dev)->check_count(&dev->dev_export_obj));
		return(-1);
	}
	if (!(max_sectors)) {
		printk(KERN_ERR "dev[%p]: Illegal ZERO value for max_sectors\n", dev);
		return(-1);
	}
	if (max_sectors < DA_STATUS_MAX_SECTORS_MIN) {
		printk(KERN_ERR "dev[%p]: Passed max_sectors: %u less than"
			" DA_STATUS_MAX_SECTORS_MIN: %u\n", dev, max_sectors,
				DA_STATUS_MAX_SECTORS_MIN);
		return(-1);
	}
	if (TRANSPORT(dev)->transport_type == TRANSPORT_PLUGIN_PHBA_PDEV) {
		if (max_sectors > TRANSPORT(dev)->get_max_sectors(dev)) {
			 printk(KERN_ERR "dev[%p]: Passed max_sectors: %u greater than"
				" LIO-Core/SE_Device max_sectors: %u\n", dev,
				max_sectors, TRANSPORT(dev)->get_max_sectors(dev));
			 return(-1);
		}
	} else {
		if (!(force) && (max_sectors > TRANSPORT(dev)->get_max_sectors(dev))) {
			printk(KERN_ERR "dev[%p]: Passed max_sectors: %u greater than"
				" LIO-Core/SE_Device max_sectors: %u, use force=1 to override.\n",
				dev, max_sectors, TRANSPORT(dev)->get_max_sectors(dev));
			return(-1);
		}
		if (max_sectors > DA_STATUS_MAX_SECTORS_MAX) {
			printk(KERN_ERR "dev[%p]: Passed max_sectors: %u greater than"
				" DA_STATUS_MAX_SECTORS_MAX: %u\n", dev,
				max_sectors, DA_STATUS_MAX_SECTORS_MAX);
			return(-1);
		}
	}

	DEV_ATTRIB(dev)->max_sectors = max_sectors;
	printk("dev[%p]: SE Device max_sectors changed to %u\n",
			dev, max_sectors);
	return(0);
}

extern se_lun_t *core_dev_add_lun (
	se_portal_group_t *tpg,
	se_hba_t *hba,
	se_device_t *dev,
	u32 lun,
	int *ret)
{
        se_lun_t *lun_p;
        u32 lun_access = 0;

        if (DEV_OBJ_API(dev)->check_count(&dev->dev_access_obj) != 0) {
                printk(KERN_ERR "Unable to export se_device_t while dev_access_obj: %d\n",
                        DEV_OBJ_API(dev)->check_count(&dev->dev_access_obj));
                *ret = ERR_OBJ_ACCESS_COUNT;
                return(NULL);
        }

        if (!(lun_p = core_tpg_pre_addlun(tpg, lun, ret)))
                return(NULL);

        if (DEV_OBJ_API(dev)->get_device_access((void *)dev) == 0)
                lun_access = TRANSPORT_LUNFLAGS_READ_ONLY;
        else
                lun_access = TRANSPORT_LUNFLAGS_READ_WRITE;

        if (core_tpg_post_addlun(tpg, lun_p, TRANSPORT_LUN_TYPE_DEVICE, lun_access,
                              dev, dev->dev_obj_api) < 0) {
                *ret = ERR_EXPORT_FAILED;
                return(NULL);
        }

        printk("%s_TPG[%u]_LUN[%u] - Activated %s Logical Unit from"
                " CORE HBA: %u\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), lun_p->unpacked_lun,
		TPG_TFO(tpg)->get_fabric_name(), hba->hba_id);
        /*
         * Update LUN maps for dynamically added initiators when generate_node_acl
         * is enabled.
         */
	if (TPG_TFO(tpg)->tpg_check_demo_mode(tpg)) {
                se_node_acl_t *acl;
                spin_lock_bh(&tpg->acl_node_lock);
                for (acl = tpg->acl_node_head; acl; acl = acl->next) {
                        if (acl->nodeacl_flags & NAF_DYNAMIC_NODE_ACL) {
                                spin_unlock_bh(&tpg->acl_node_lock);
                                core_tpg_add_node_to_devs(acl, tpg);
                                spin_lock_bh(&tpg->acl_node_lock);
                        }
                }
                spin_unlock_bh(&tpg->acl_node_lock);
        }

        *ret = 0;
        return(lun_p);
}

EXPORT_SYMBOL(core_dev_add_lun);

/*      core_dev_del_lun():
 *
 *
 */
extern int core_dev_del_lun (
        se_portal_group_t *tpg,
        u32 unpacked_lun)
{
        se_lun_t *lun;
        int ret = 0;

        if (!(lun = core_tpg_pre_dellun(tpg, unpacked_lun, TRANSPORT_LUN_TYPE_DEVICE, &ret)))
                return(ret);

        core_tpg_post_dellun(tpg, lun);

        printk("%s_TPG[%u]_LUN[%u] - Deactivated %s Logical Unit from"
                " device object\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), unpacked_lun,
		TPG_TFO(tpg)->get_fabric_name());

        return(0);
}

EXPORT_SYMBOL(core_dev_del_lun);

extern se_lun_t *core_get_lun_from_tpg (se_portal_group_t *tpg, u32 unpacked_lun)
{
        se_lun_t *lun;

        spin_lock(&tpg->tpg_lun_lock);
        if (unpacked_lun > (TRANSPORT_MAX_LUNS_PER_TPG-1)) {
                printk(KERN_ERR "%s LUN: %u exceeds TRANSPORT_MAX_LUNS_PER_TPG-1:"
                        " %u for Target Portal Group: %hu\n",
			TPG_TFO(tpg)->get_fabric_name(), unpacked_lun,
                        TRANSPORT_MAX_LUNS_PER_TPG-1, TPG_TFO(tpg)->tpg_get_tag(tpg));
                        spin_unlock(&tpg->tpg_lun_lock);
                return(NULL);
        }
        lun = &tpg->tpg_lun_list[unpacked_lun];

        if (lun->lun_status != TRANSPORT_LUN_STATUS_FREE) {
                printk(KERN_ERR "%s Logical Unit Number: %u is not free on"
                        " Target Portal Group: %hu, ignoring request.\n",
                        TPG_TFO(tpg)->get_fabric_name(), unpacked_lun,
			TPG_TFO(tpg)->tpg_get_tag(tpg));
                spin_unlock(&tpg->tpg_lun_lock);
                return(NULL);
        }
        spin_unlock(&tpg->tpg_lun_lock);

        return(lun);
}

EXPORT_SYMBOL(core_get_lun_from_tpg);

/*      core_dev_get_lun():
 *
 *
 */
static se_lun_t *core_dev_get_lun (se_portal_group_t *tpg, u32 unpacked_lun)
{
        se_lun_t *lun;

        spin_lock(&tpg->tpg_lun_lock);
        if (unpacked_lun > (TRANSPORT_MAX_LUNS_PER_TPG-1)) {
                printk(KERN_ERR "%s LUN: %u exceeds TRANSPORT_MAX_LUNS_PER_TPG-1:"
                        " %u for Target Portal Group: %hu\n",
			TPG_TFO(tpg)->get_fabric_name(), unpacked_lun,
			TRANSPORT_MAX_LUNS_PER_TPG-1, TPG_TFO(tpg)->tpg_get_tag(tpg));
                spin_unlock(&tpg->tpg_lun_lock);
                return(NULL);
        }
        lun = &tpg->tpg_lun_list[unpacked_lun];

        if (lun->lun_status != TRANSPORT_LUN_STATUS_ACTIVE) {
                printk(KERN_ERR "%s Logical Unit Number: %u is not active on"
                        " Target Portal Group: %hu, ignoring request.\n",
			TPG_TFO(tpg)->get_fabric_name(), unpacked_lun,
			TPG_TFO(tpg)->tpg_get_tag(tpg));
                spin_unlock(&tpg->tpg_lun_lock);
                return(NULL);
        }
        spin_unlock(&tpg->tpg_lun_lock);

        return(lun);
}

extern se_lun_acl_t *core_dev_init_initiator_node_lun_acl (
        se_portal_group_t *tpg,
        u32 mapped_lun,
        char *initiatorname,
        int *ret)
{
        se_lun_acl_t *lacl;
        se_node_acl_t *nacl;

        if (strlen(initiatorname) > TRANSPORT_IQN_LEN) {
                printk(KERN_ERR "%s InitiatorName exceeds maximum size.\n",
			TPG_TFO(tpg)->get_fabric_name());
                *ret = -EOVERFLOW;
                return(NULL);
        }
        if (!(nacl = core_tpg_get_initiator_node_acl(tpg, initiatorname))) {
                *ret = -EINVAL;
                return(NULL);
        }
        if (!(lacl = (se_lun_acl_t *) kzalloc(sizeof(se_lun_acl_t), GFP_KERNEL))) {
                printk(KERN_ERR "Unable to allocate memory for se_lun_acl_t.\n");
                *ret = -ENOMEM;
                return(NULL);
        }

        lacl->mapped_lun = mapped_lun;
        lacl->se_lun_nacl = nacl;
        snprintf(lacl->initiatorname, TRANSPORT_IQN_LEN, "%s", initiatorname);

        return(lacl);
}

EXPORT_SYMBOL(core_dev_init_initiator_node_lun_acl);

extern int core_dev_add_initiator_node_lun_acl (
        se_portal_group_t *tpg,
        se_lun_acl_t *lacl,
        u32 unpacked_lun,
        u32 lun_access)
{
        se_lun_t *lun;
        se_node_acl_t *nacl;

        if (!(lun = core_dev_get_lun(tpg, unpacked_lun))) {
                printk(KERN_ERR "%s Logical Unit Number: %u is not active on"
                        " Target Portal Group: %hu, ignoring request.\n",
		TPG_TFO(tpg)->get_fabric_name(), unpacked_lun,
                        TPG_TFO(tpg)->tpg_get_tag(tpg));
                return(-EINVAL);
        }

        if (!(nacl = core_tpg_get_initiator_node_acl(tpg, lacl->initiatorname)))
                return(-EINVAL);

        spin_lock(&lun->lun_acl_lock);
        ADD_ENTRY_TO_LIST(lacl, lun->lun_acl_head, lun->lun_acl_tail);
        spin_unlock(&lun->lun_acl_lock);

        if ((lun->lun_access & TRANSPORT_LUNFLAGS_READ_ONLY) &&
            (lun_access & TRANSPORT_LUNFLAGS_READ_WRITE))
                lun_access = TRANSPORT_LUNFLAGS_READ_ONLY;

        lacl->se_lun = lun;

        core_update_device_list_for_node(lun, lacl->mapped_lun,
                        lun_access, nacl, tpg, 1);

        printk("%s_TPG[%hu]_LUN[%u->%u] - Added %s ACL for iSCSI"
                " InitiatorNode: %s\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), unpacked_lun, lacl->mapped_lun,
                (lun_access & TRANSPORT_LUNFLAGS_READ_WRITE) ? "RW" : "RO",
                        lacl->initiatorname);
        return(0);
}

EXPORT_SYMBOL(core_dev_add_initiator_node_lun_acl);

/*      core_dev_del_initiator_node_lun_acl():
 *
 *
 */
extern int core_dev_del_initiator_node_lun_acl (
        se_portal_group_t *tpg,
        se_lun_t *lun,
        se_lun_acl_t *lacl)
{
        se_node_acl_t *nacl;

        if (!(nacl = core_tpg_get_initiator_node_acl(tpg, lacl->initiatorname)))
                return(ERR_DELLUNACL_NODE_ACL_MISSING);

        spin_lock(&lun->lun_acl_lock);
        REMOVE_ENTRY_FROM_LIST(lacl, lun->lun_acl_head, lun->lun_acl_tail);
        spin_unlock(&lun->lun_acl_lock);

        core_update_device_list_for_node(lun, lacl->mapped_lun,
                TRANSPORT_LUNFLAGS_NO_ACCESS, nacl, tpg, 0);

        lacl->se_lun = NULL;

        printk("%s_TPG[%hu]_LUN[%u] - Removed ACL for iSCSI InitiatorNode:"
                " %s Mapped LUN: %u\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), lun->unpacked_lun,
                        lacl->initiatorname, lacl->mapped_lun);
        return(0);
}

EXPORT_SYMBOL(core_dev_del_initiator_node_lun_acl);

extern void core_dev_free_initiator_node_lun_acl (
        se_portal_group_t *tpg,
        se_lun_acl_t *lacl)
{
        printk("%s_TPG[%hu] - Freeing ACL for %s InitiatorNode: %s"
                " Mapped LUN: %u\n", TPG_TFO(tpg)->get_fabric_name(),
		TPG_TFO(tpg)->tpg_get_tag(tpg), 
		TPG_TFO(tpg)->get_fabric_name(),
		lacl->initiatorname, lacl->mapped_lun);

        kfree(lacl);
        return;
}

EXPORT_SYMBOL(core_dev_free_initiator_node_lun_acl);
