/*********************************************************************************
 * Filename:  target_core_pscsi.c
 *
 * This file contains the iSCSI <-> Parallel SCSI transport specific functions.
 *
 * Copyright (c) 2003, 2004, 2005 PyX Technologies, Inc.
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


#define TARGET_CORE_PSCSI_C

#include <linux/version.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/genhd.h>
#include <linux/cdrom.h>
#include <linux/file.h>

#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_host.h>
#include <sd.h>
#include <sr.h>

#include <iscsi_linux_os.h>
#include <iscsi_linux_defs.h>

#include <iscsi_debug.h>
#include <iscsi_protocol.h>
#include <iscsi_target_core.h>
#include <target_core_base.h>
#include <iscsi_target_device.h>
#include <target_core_transport.h>
#include <iscsi_target_util.h>
#include <target_core_pscsi.h>
#include <iscsi_target_error.h>

#include <target_core_plugin.h>
#include <target_core_seobj.h>
#include <target_core_seobj_plugins.h>
#include <target_core_transport_plugin.h>
				
#undef TARGET_CORE_PSCSI_C

#define ISPRINT(a)  ((a >=' ')&&(a <= '~'))

#warning FIXME: Obtain via IOCTL for Initiator Drivers
#define INIT_CORE_NAME          "SBE, Inc. iSCSI Initiator Core"

extern se_global_t *se_global;
extern struct block_device *linux_blockdevice_claim(int, int, void *);
extern int linux_blockdevice_release(int, int, struct block_device *);
extern int linux_blockdevice_check(int, int);

/*	pscsi_get_sh():
 *
 *
 */
static struct Scsi_Host *pscsi_get_sh (u32 host_no)
{
	struct Scsi_Host *sh = NULL;
	
	sh = scsi_host_lookup(host_no);
	if (IS_ERR(sh)) {
		TRACE_ERROR("Unable to locate Parallel SCSI HBA with Host ID:"
				" %u\n", host_no);
		return(NULL);
	}

	return(sh);
}

/*	pscsi_check_sd():
 *
 *	Should be called with scsi_device_get(sd) held
 */
extern int pscsi_check_sd (struct scsi_device *sd)
{
	struct gendisk *disk;
	struct scsi_disk *sdisk;

	if (!sd) {
		TRACE_ERROR("struct scsi_device is NULL!\n");
		return(-1);
	}
	
	if (sd->type != TYPE_DISK)
		return(0);

	/*
	 * Some struct scsi_device of Type: Direct-Access, namely the
	 * SGI Univerisal Xport do not have a corrasponding block device.
	 * We skip these for now.
	 */
	if (!(sdisk = dev_get_drvdata(&sd->sdev_gendev)))
		return(-1);

	disk = (struct gendisk *) sdisk->disk;
	if (!(disk->major)) {
		TRACE_ERROR("dev_get_drvdata() failed\n");
		return(-1);
	}

	if (linux_blockdevice_check(disk->major, disk->first_minor) < 0)
		return(-1);

	return(0);
}

/*	pscsi_claim_sd():
 *
 *	Should be called with scsi_device_get(sd) held
 */
extern int pscsi_claim_sd (struct scsi_device *sd)
{
	struct block_device *bdev;
	struct gendisk *disk;
	struct scsi_disk *sdisk;

	if (!sd) {
		TRACE_ERROR("struct scsi_device is NULL!\n");
		return(-1);
	}
	
	if (sd->type != TYPE_DISK)
		return(0);
	
	/*
	 * Some struct scsi_device of Type: Direct-Access, namely the
	 * SGI Univerisal Xport do not have a corrasponding block device.
	 * We skip these for now.
	 */
	if (!(sdisk = dev_get_drvdata(&sd->sdev_gendev)))
		return(-1);

	disk = (struct gendisk *) sdisk->disk;
	if (!(disk->major)) {
		TRACE_ERROR("dev_get_drvdata() failed\n");
		return(-1);
	}

	PYXPRINT("PSCSI: Claiming %p Major:Minor - %d:%d\n", sd, disk->major, disk->first_minor);
	
	if (!(bdev = linux_blockdevice_claim(disk->major, disk->first_minor, (void *)sd)))
		return(-1);

	return(0);
}

/*	pscsi_release_sd()
 *
 * 	Should be called with scsi_device_get(sd) held
 */
extern int pscsi_release_sd (struct scsi_device *sd)
{
	struct gendisk *disk;
	struct scsi_disk *sdisk;
	
	if (!sd) {
		TRACE_ERROR("struct scsi_device is NULL!\n");
		return(-1);
	}

	if (sd->type != TYPE_DISK)
		return(0);
	
	/*
	 * Some struct scsi_device of Type: Direct-Access, namely the
	 * SGI Univerisal Xport do not have a corrasponding block device.
	 * We skip these for now.
	 */
	if (!(sdisk = dev_get_drvdata(&sd->sdev_gendev)))
		return(-1);

	disk = (struct gendisk *) sdisk->disk;
	if (!(disk->major)) {
		TRACE_ERROR("dev_get_drvdata() failed\n");
		return(-1);
	}

	PYXPRINT("PSCSI: Releasing Major:Minor - %d:%d\n", disk->major, disk->first_minor);

	return(linux_blockdevice_release(disk->major, disk->first_minor, NULL));
}

/*	pscsi_attach_hba():
 *
 * 	pscsi_get_sh() used scsi_host_lookup() to locate struct Scsi_Host.
 *	from the passed SCSI Host ID.
 */
extern int pscsi_attach_hba (se_hba_t *hba, u32 host_id)
{
	int hba_depth, max_sectors;
	struct Scsi_Host *sh;

	if (!(sh = pscsi_get_sh(host_id)))
		return(-EINVAL);

	max_sectors = sh->max_sectors;

	/*
	 * Usually the SCSI LLD will use the hostt->can_queue value to define its
	 * HBA TCQ depth.  Some other drivers (like 2.6 megaraid) don't set this
	 * at all and set sh->can_queue at runtime.
	 */
	hba_depth = (sh->hostt->can_queue > sh->can_queue) ?
		sh->hostt->can_queue : sh->can_queue;
	atomic_set(&hba->left_queue_depth, hba_depth);
	atomic_set(&hba->max_queue_depth, hba_depth);

	hba->hba_ptr = (void *) sh;
	hba->transport = &pscsi_template;
	
	PYXPRINT("CORE_HBA[%d] - %s Parallel SCSI HBA Driver %s on Generic"
		" Target Core Stack %s\n", hba->hba_id, PYX_ISCSI_VENDOR,
		PSCSI_VERSION, PYX_ISCSI_VERSION);
	PYXPRINT("CORE_HBA[%d] - %s\n", hba->hba_id, (sh->hostt->name) ?
			(sh->hostt->name) : "Unknown");
	PYXPRINT("CORE_HBA[%d] - Attached Parallel SCSI HBA to Generic Target Core"
		" with TCQ Depth: %d MaxSectors: %hu\n", hba->hba_id,
		atomic_read(&hba->max_queue_depth), max_sectors);

	return(0);
}

/*	pscsi_detach_hba(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int pscsi_detach_hba (se_hba_t *hba)
{
	struct Scsi_Host *scsi_host = (struct Scsi_Host *) hba->hba_ptr;
	
	scsi_host_put(scsi_host);
	
	PYXPRINT("CORE_HBA[%d] - Detached Parallel SCSI HBA: %s from Generic Target Core\n",
		hba->hba_id, (scsi_host->hostt->name) ? (scsi_host->hostt->name) : "Unknown");	

	hba->hba_ptr = NULL;
	
	return(0);
}

/*	pscsi_add_device_to_list():
 *
 *	FIXME: We are going to want to increment struct scsi_device->access_count
 *	       either here or in pscsi_activate_device().
 */
extern se_device_t *pscsi_add_device_to_list (
	se_hba_t *hba,
	se_subsystem_dev_t *se_dev,
	pscsi_dev_virt_t *pdv,
	struct scsi_device *sd,
	int dev_flags)
{
	se_device_t *dev;
	
	/*
	 * Some pseudo Parallel SCSI HBAs do not fill in sector_size
	 * correctly. (See ide-scsi.c)  So go ahead and setup sane
	 * values.
	 */
	if (!sd->sector_size) {
		switch (sd->type) {
		case TYPE_DISK:
			sd->sector_size = 512;
			break;
		case TYPE_ROM:
			sd->sector_size = 2048;
			break;
		case TYPE_TAPE: /* The Tape may not be in the drive */
			break;
		case TYPE_MEDIUM_CHANGER: /* Control CDBs only */
			break;
		default:
			TRACE_ERROR("Unable to set sector_size for %d\n",
					sd->type);
			return(NULL);
		}

		if (sd->sector_size) {
			TRACE_ERROR("Set broken Parallel SCSI Device %d:%d:%d"
				" sector_size to %d\n", sd->channel, sd->id,
					sd->lun, sd->sector_size);
		}
	}

	if (!sd->queue_depth) {
		sd->queue_depth = PSCSI_DEFAULT_QUEUEDEPTH;

		TRACE_ERROR("Set broken Parallel SCSI Device %d:%d:%d"
			" queue_depth to %d\n", sd->channel, sd->id,
				sd->lun, sd->queue_depth);
	}
	/*
	 * Set the pointer pdv->pdv_sd to from passed struct scsi_device,
	 * which has already been referenced with Linux SCSI code with
	 * scsi_device_get() in this file's pscsi_create_virtdevice().
	 *
	 * The passthrough operations called by the transport_add_device_* function
	 * below will require this pointer to be set for passthrough ops.
	 *
	 * For the shutdown case in pscsi_free_device(), this struct scsi_device 
	 * reference is released with Linux SCSI code scsi_device_put() and the
	 * pdv->pdv_sd cleared.
	 */
	pdv->pdv_sd = sd;

	if (!(dev = transport_add_device_to_core_hba(hba, &pscsi_template,
				se_dev, dev_flags, (void *)pdv))) {
		pdv->pdv_sd = NULL;
		return(NULL);
	}
	
	/*
	 * For TYPE_TAPE, attempt to determine blocksize with MODE_SENSE.
	 */
	if (sd->type == TYPE_TAPE) {
		unsigned char *buf = NULL, cdb[SCSI_CDB_SIZE];
		iscsi_cmd_t *cmd;
		u32 blocksize;

		memset(cdb, 0, SCSI_CDB_SIZE);
		cdb[0] = MODE_SENSE;
		cdb[4] = 0x0c; /* 12 bytes */
		
		if (!(cmd = transport_allocate_passthrough(&cdb[0], ISCSI_READ, 0, NULL, 0,
				12, DEV_OBJ_API(dev), dev))) {
			TRACE_ERROR("Unable to determine blocksize for TYPE_TAPE\n");
			goto out;
		}

		if (transport_generic_passthrough(cmd) < 0) {
			TRACE_ERROR("Unable to determine blocksize for TYPE_TAPE\n");
			goto out;
		}

		buf = (unsigned char *)T_TASK(cmd)->t_task_buf;
		blocksize = (buf[9] << 16) | (buf[10] << 8) | (buf[11]);

		/*
		 * If MODE_SENSE still returns zero, set the default value to 1024.
		 */
		if (!(sd->sector_size = blocksize))
			sd->sector_size = 1024;

		transport_passthrough_release(cmd);
	}
out:
	return(dev);
}

extern int pscsi_claim_phydevice (se_hba_t *hba, se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *)pdv->pdv_sd;
	
	return(pscsi_claim_sd(sd));
}

extern int pscsi_release_phydevice (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *)pdv->pdv_sd;
	
	return(pscsi_release_sd(sd));
}

extern void *pscsi_allocate_virtdevice (se_hba_t *hba, const char *name)
{
	pscsi_dev_virt_t *pdv;	

	if (!(pdv = kzalloc(sizeof(pscsi_dev_virt_t), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate memory for pscsi_dev_virt_t\n");
		return(NULL);
	}
	pdv->pdv_se_hba = hba;

	printk("PSCSI: Allocated pdv: %p for %s\n", pdv, name);
	return((void *)pdv);
}

/*
 * Called with struct Scsi_Host->host_lock called.
 */
extern se_device_t *pscsi_create_type_disk (
	struct scsi_device *sd,
	pscsi_dev_virt_t *pdv,
	se_subsystem_dev_t *se_dev,
	se_hba_t *hba)
{
	se_device_t *dev;
	struct Scsi_Host *sh = sd->host;
	u32 dev_flags = 0;

	if (scsi_device_get(sd)) {
		printk(KERN_ERR "scsi_device_get() failed for %d:%d:%d:%d\n",
			sh->host_no, sd->channel, sd->id, sd->lun);
		spin_unlock_irq(sh->host_lock);
		return(NULL);
	}
	spin_unlock_irq(sh->host_lock);

	if (pscsi_check_sd(sd) < 0) {
		scsi_device_put(sd);
		printk(KERN_ERR "pscsi_check_sd() failed for %d:%d:%d:%d\n",
			sh->host_no, sd->channel, sd->id, sd->lun);
		return(NULL);
	}
	if (!(pscsi_claim_sd(sd))) {
		dev_flags |= DF_CLAIMED_BLOCKDEV;
		dev_flags |= DF_PERSISTENT_CLAIMED_BLOCKDEV;
	}	
	if (!(dev = pscsi_add_device_to_list(hba, se_dev, pdv, sd, dev_flags))) {
		scsi_device_put(sd);
		return(NULL);
	}
	PYXPRINT("CORE_PSCSI[%d] - Added TYPE_DISK for %d:%d:%d\n",
		sh->host_no, sd->channel, sd->id, sd->lun);

	return(dev);
}

/*
 * Called with struct Scsi_Host->host_lock called.
 */
extern se_device_t *pscsi_create_type_rom (
	struct scsi_device *sd,
	pscsi_dev_virt_t *pdv,
	se_subsystem_dev_t *se_dev,
	se_hba_t *hba)
{
	se_device_t *dev;
	struct Scsi_Host *sh = sd->host;
	u32 dev_flags = 0;

	if (scsi_device_get(sd)) {
		printk(KERN_ERR "scsi_device_get() failed for %d:%d:%d:%d\n",
			sh->host_no, sd->channel, sd->id, sd->lun);
		spin_unlock_irq(sh->host_lock);
		return(NULL);
	}
	spin_unlock_irq(sh->host_lock);

	if (!(dev = pscsi_add_device_to_list(hba, se_dev, pdv, sd, dev_flags))) {
		scsi_device_put(sd);
		return(NULL);
	}
	PYXPRINT("CORE_PSCSI[%d] - Added Type: %s for %d:%d:%d\n",
		sh->host_no, scsi_device_type(sd->type), sd->channel,
		sd->id, sd->lun);

	return(dev);	
}

/*
 *Called with struct Scsi_Host->host_lock called.
 */
extern se_device_t *pscsi_create_type_other (
	struct scsi_device *sd,
	pscsi_dev_virt_t *pdv,
	se_subsystem_dev_t *se_dev,
	se_hba_t *hba)
{
	se_device_t *dev;
	struct Scsi_Host *sh = sd->host;
	u32 dev_flags = 0;

	spin_unlock_irq(sh->host_lock);
	if (!(dev = pscsi_add_device_to_list(hba, se_dev, pdv, sd, dev_flags)))
		return(NULL);
	
	PYXPRINT("CORE_PSCSI[%d] - Added Type: %s for %d:%d:%d\n",
		sh->host_no, scsi_device_type(sd->type), sd->channel,
		sd->id, sd->lun);

	return(dev);
}

extern se_device_t *pscsi_create_virtdevice (
	se_hba_t *hba,
	se_subsystem_dev_t *se_dev,
	void *p)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *)p;
	struct scsi_device *sd;
	struct Scsi_Host *sh = (struct Scsi_Host *) hba->hba_ptr;

	if (!(pdv)) {
		printk(KERN_ERR "Unable to locate pscsi_dev_virt_t parameter\n");
		return(NULL);
	}

	spin_lock_irq(sh->host_lock);
	list_for_each_entry(sd, &sh->__devices, siblings) {
		if (!(pdv->pdv_channel_id == sd->channel) ||
		    !(pdv->pdv_target_id == sd->id) ||
		    !(pdv->pdv_lun_id == sd->lun))
			continue;
		/*
		 * Functions will release struct scsi_host->host_lock
		 */
		switch (sd->type) {
		case TYPE_DISK:
			return(pscsi_create_type_disk(sd, pdv, se_dev, hba));
		case TYPE_ROM:
			return(pscsi_create_type_rom(sd, pdv, se_dev, hba));
		default:
			return(pscsi_create_type_other(sd, pdv, se_dev, hba));
		}
	}
	spin_unlock_irq(sh->host_lock);

	printk(KERN_ERR "Unable to locate %d:%d:%d:%d\n", sh->host_no,
		pdv->pdv_channel_id,  pdv->pdv_target_id, pdv->pdv_lun_id);

	return(NULL);
}

/*	pscsi_activate_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int pscsi_activate_device (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	struct Scsi_Host *sh = sd->host;
	
	PYXPRINT("CORE_PSCSI[%d] - Activating Device with TCQ: %d at Parallel"
		" SCSI Location (Channel/Target/LUN) %d/%d/%d\n", sh->host_no,
		 sd->queue_depth, sd->channel, sd->id, sd->lun);

	return(0);
}

/*	pscsi_deactivate_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void pscsi_deactivate_device (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	struct Scsi_Host *sh = sd->host;
	
	PYXPRINT("CORE_PSCSI[%d] - Deactivating Device with TCQ: %d at Parallel"
		" SCSI Location (Channel/Target/LUN) %d/%d/%d\n", sh->host_no,
		sd->queue_depth, sd->channel, sd->id, sd->lun);
	
	return;
}

/*	pscsi_free_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void pscsi_free_device (void *p)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) p;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	if (sd) {
		if ((sd->type == TYPE_DISK) || (sd->type == TYPE_ROM)) 
			scsi_device_put(sd);

		pdv->pdv_sd = NULL;
	}
	
	kfree(pdv);
	return;
}

/*	pscsi_transport_complete():
 *
 *
 */
extern int pscsi_transport_complete (se_task_t *task)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) task->iscsi_dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	void *pscsi_buf;
	int result;
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;
	unsigned char *cdb = &pt->pscsi_cdb[0];

	result = pt->pscsi_result;
	pscsi_buf = pt->pscsi_buf;

# ifdef LINUX_EVPD_PAGE_CHECK
	if ((cdb[0] == INQUIRY) && host_byte(result) == DID_OK) {
		u32 len = 0;
		unsigned char *dst = (unsigned char *)pscsi_buf, *iqn = NULL;
		unsigned char buf[EVPD_BUF_LEN];
//#warning FIXME v2.8: se_obj_api usage
		se_hba_t *hba = task->iscsi_dev->iscsi_hba;

		/*
		 * The Initiator port did not request EVPD information.
		 */
		if (!(cdb[1] & 0x1)) {
			task->task_scsi_status = GOOD;
			return(0);
		}
			
		/*
		 * Assume the HBA did the right thing..
		 */
		if (dst[3] != 0x00) {
			task->task_scsi_status = GOOD;
			return(0);
		}

		memset(buf, 0, EVPD_BUF_LEN);
		memset(dst, 0, task->task_size);
		buf[0] = sd->type;

		switch (cdb[2]) {
		case 0x00:
			buf[1] = 0x00;
			buf[3] = 3;
			buf[4] = 0x0;
			buf[5] = 0x80; 
			buf[6] = 0x83;
			len = 3;
			break;
		case 0x80:
			iqn = transport_get_iqn_sn();
			buf[1] = 0x80;
			len += sprintf((unsigned char *)&buf[4], "%s:%u_%u_%u_%u",
				iqn, hba->hba_id, sd->channel, sd->id, sd->lun);
			buf[3] = len;
			break;
		case 0x83:
			iqn = transport_get_iqn_sn();
			buf[1] = 0x83;
			/* Start Identifier Page */
			buf[4] = 0x2; /* ASCII */
			buf[5] = 0x1; 
			buf[6] = 0x0;
			len += sprintf((unsigned char *)&buf[8], "SBEi-INC");
			len += sprintf((unsigned char *)&buf[16], "PSCSI:%s:%u_%u_%u_%u",
					iqn, hba->hba_id, sd->channel, sd->id, sd->lun);
			buf[7] = len; /* Identifer Length */
			len += 4;
			buf[3] = len; /* Page Length */
			break;
		default:
			break;
		}
				
		if ((len + 4) > task->task_size) {
			TRACE_ERROR("Inquiry EVPD Length: %u larger than"
				" req->sr_bufflen: %u\n", (len + 4), task->task_size);
			memcpy(dst, buf, task->task_size);
		} else
			memcpy(dst, buf, (len + 4));
		
		/*
		 * Fake the GOOD SAM status here too.
		 */
		task->task_scsi_status = GOOD;	
		return(0);
	}

# endif /* LINUX_EVPD_PAGE_CHECK */

	/*
	 * Hack to make sure that Write-Protect modepage is set if R/O mode is forced.
	 */
	if (((cdb[0] == MODE_SENSE) || (cdb[0] == MODE_SENSE_10)) &&
	     (status_byte(result) << 1) == SAM_STAT_GOOD) {
		if (!task->iscsi_cmd->iscsi_deve)
			goto after_mode_sense;

		if (task->iscsi_cmd->iscsi_deve->lun_flags & ISCSI_LUNFLAGS_READ_ONLY) {
			unsigned char *buf = (unsigned char *)pscsi_buf;

			if (cdb[0] == MODE_SENSE_10) {
				if (!(buf[3] & 0x80))
					buf[3] |= 0x80;
			} else {
				if (!(buf[2] & 0x80))
					buf[2] |= 0x80;
			}
		}
	}
after_mode_sense:

	if (sd->type != TYPE_TAPE)
		goto after_mode_select;
	
	/*
	 * Hack to correctly obtain the initiator requested blocksize for TYPE_TAPE.
	 * Since this value is dependent upon each tape media, struct scsi_device->sector_size
	 * will not contain the correct value by default, so we go ahead and set it so
	 * TRANSPORT(dev)->get_blockdev() returns the correct value to the storage engine.
	 */
	if (((cdb[0] == MODE_SELECT) || (cdb[0] == MODE_SELECT_10)) &&
	      (status_byte(result) << 1) == SAM_STAT_GOOD) {
		unsigned char *buf;
		struct scatterlist *sg = (struct scatterlist *)pscsi_buf;
		u16 bdl;
		u32 blocksize;
		
		if (!(buf = sg_virt(&sg[0]))) {
			TRACE_ERROR("Unable to get buf for scatterlist\n");
			goto after_mode_select;
		}

		if (cdb[0] == MODE_SELECT)
			bdl = (buf[3]);
		else
			bdl = (buf[6] << 8) | (buf[7]);

		if (!bdl)
			goto after_mode_select;

		if (cdb[0] == MODE_SELECT)
			blocksize = (buf[9] << 16) | (buf[10] << 8) | (buf[11]);
		else
			blocksize = (buf[13] << 16) | (buf[14] << 8) | (buf[15]);

		sd->sector_size = blocksize;
	}
after_mode_select:
	
	if (status_byte(result) & CHECK_CONDITION)
		return(1);

	return(0);
}

/*	pscsi_allocate_request(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void *pscsi_allocate_request (
	se_task_t *task,
	se_device_t *dev)
{
	pscsi_plugin_task_t *pt;
	
	if (!(pt = kzalloc(sizeof(pscsi_plugin_task_t), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate pscsi_plugin_task_t\n");
		return(NULL);
	}

	return(pt);
}

extern void pscsi_get_evpd_prod (unsigned char *buf, u32 size, se_device_t *dev)
{
	snprintf(buf, size, "PSCSI");
	return;
}

extern void pscsi_get_evpd_sn (unsigned char *buf, u32 size, se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	se_hba_t *hba = dev->iscsi_hba;

	snprintf(buf, size, "%u_%u_%u_%u", hba->hba_id, sd->channel, sd->id, sd->lun);
	return;
}

static int pscsi_blk_get_request (se_task_t *task)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) task->iscsi_dev->dev_ptr;

	pt->pscsi_req = blk_get_request(pdv->pdv_sd->request_queue,
			(pt->pscsi_direction == DMA_TO_DEVICE), GFP_KERNEL);
	if (!(pt->pscsi_req) || IS_ERR(pt->pscsi_req)) {
		printk(KERN_ERR "PSCSI: blk_get_request() failed: %ld\n",
				IS_ERR(pt->pscsi_req));
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);
	}
	/*
	 * Defined as "scsi command" in include/linux/blkdev.h.
	 */
	pt->pscsi_req->cmd_type = REQ_TYPE_BLOCK_PC;
	/*
	 * Setup the done function pointer for struct request,
	 * also set the end_io_data pointer.to se_task_t.
	 */
	pt->pscsi_req->end_io = pscsi_req_done;
	pt->pscsi_req->end_io_data = (void *)task;
	/*
	 * Load the referenced se_task_t's SCSI CDB into
	 * include/linux/blkdev.h:struct request->cmd
	 */
	pt->pscsi_req->cmd_len = COMMAND_SIZE(pt->pscsi_cdb[0]);
	memcpy(pt->pscsi_req->cmd, pt->pscsi_cdb, pt->pscsi_req->cmd_len);
	/*
	 * Setup pointer for outgoing sense data.
	 */
	pt->pscsi_req->sense = (void *)&pt->pscsi_sense[0];
	pt->pscsi_req->sense_len = 0;

	return(0);
}

/*      pscsi_do_task(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int pscsi_do_task (se_task_t *task)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) task->iscsi_dev->dev_ptr;
	struct gendisk *gd = NULL;
	/*
	 * Grab pointer to struct gendisk for TYPE_DISK and TYPE_ROM
	 * cases (eg: cases where struct scsi_device has a backing
	 * struct block_device.  Also set the struct request->timeout
	 * value based on peripheral device type (from SCSI).
	 */
	if (pdv->pdv_sd->type == TYPE_DISK) {
		struct scsi_disk *sdisk = dev_get_drvdata(
					&pdv->pdv_sd->sdev_gendev);
		gd = sdisk->disk;
		pt->pscsi_req->timeout = PS_TIMEOUT_DISK;
	} else if (pdv->pdv_sd->type == TYPE_ROM) {
		struct scsi_cd *scd = dev_get_drvdata(
					&pdv->pdv_sd->sdev_gendev);
		gd = scd->disk;
		pt->pscsi_req->timeout = PS_TIMEOUT_OTHER;
	} else
		pt->pscsi_req->timeout = PS_TIMEOUT_OTHER;

	pt->pscsi_req->retries = PS_RETRY;
	/*
	 * Queue the struct request into the struct scsi_device->request_queue.
	 */
	blk_execute_rq_nowait(pdv->pdv_sd->request_queue, gd,
			      pt->pscsi_req, 1, pscsi_req_done);

	return(PYX_TRANSPORT_SENT_TO_TRANSPORT);
}

/*	pscsi_free_task(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void pscsi_free_task (se_task_t *task)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;
	kfree(pt);
	return;
}

extern ssize_t pscsi_set_configfs_dev_params (se_hba_t *hba,
					      se_subsystem_dev_t *se_dev,
					      const char *page, ssize_t count)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) se_dev->se_dev_su_ptr;
	struct Scsi_Host *sh = (struct Scsi_Host *) hba->hba_ptr;
	char *buf, *cur, *ptr, *ptr2, *endptr;
	int params = 0;

	if (!(buf = kzalloc(count, GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate memory for temporary buffer\n");
		return(-ENOMEM);
	}
	memcpy(buf, page, count);
	cur = buf;

	while (cur) {
		if (!(ptr = strstr(cur, "=")))
			goto out;

		*ptr = '\0';
		ptr++;

		if ((ptr2 = strstr(cur, "scsi_channel_id"))) {
			transport_check_dev_params_delim(ptr, &cur);
			pdv->pdv_channel_id = simple_strtoul(ptr, &endptr, 0);
			PYXPRINT("PSCSI[%d]: Referencing SCSI Channel ID: %d\n", 
				sh->host_no, pdv->pdv_channel_id);
			pdv->pdv_flags |= PDF_HAS_CHANNEL_ID;
			params++;
		} else if ((ptr2 = strstr(cur, "scsi_target_id"))) {
			transport_check_dev_params_delim(ptr, &cur);
			pdv->pdv_target_id = simple_strtoul(ptr, &endptr, 0);
			PYXPRINT("PSCSI[%d]: Referencing SCSI Target ID: %d\n",
				sh->host_no, pdv->pdv_target_id);
			pdv->pdv_flags |= PDF_HAS_TARGET_ID;
			params++;
		} else if ((ptr2 = strstr(cur, "scsi_lun_id"))) {
			transport_check_dev_params_delim(ptr, &cur);
			pdv->pdv_lun_id = simple_strtoul(ptr, &endptr, 0);
			PYXPRINT("PSCSI[%d]: Referencing SCSI LUN ID: %d\n",
				sh->host_no, pdv->pdv_lun_id);
			pdv->pdv_flags |= PDF_HAS_LUN_ID;
			params++;
		} else
			cur = NULL;
#warning FIXME: Add evpd_unit_serial= and evpd_dev_ident= parameter support for ConfigFS
	}

out:
	kfree(buf);
	return((params) ? count : -EINVAL);
}

extern ssize_t pscsi_check_configfs_dev_params (se_hba_t *hba, se_subsystem_dev_t *se_dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) se_dev->se_dev_su_ptr;

	if (!(pdv->pdv_flags & PDF_HAS_CHANNEL_ID) ||
	    !(pdv->pdv_flags & PDF_HAS_TARGET_ID) ||
	    !(pdv->pdv_flags & PDF_HAS_TARGET_ID)) {
		TRACE_ERROR("Missing scsi_channel_id=, scsi_target_id= and"
			" scsi_lun_id= parameters\n");
		return(-1);
	}
	
	return(0);
}

extern void __pscsi_get_dev_info (pscsi_dev_virt_t *, char *, int *);

extern ssize_t pscsi_show_configfs_dev_params (se_hba_t *hba, se_subsystem_dev_t *se_dev, char *page)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) se_dev->se_dev_su_ptr;
	int bl = 0;

	__pscsi_get_dev_info(pdv, page, &bl);
	return((ssize_t)bl);
}

extern se_device_t *pscsi_create_virtdevice_from_fd (
	se_subsystem_dev_t *se_dev,
	const char *page)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) se_dev->se_dev_su_ptr;
	se_device_t *dev = NULL;
	se_hba_t *hba = se_dev->se_dev_hba;
	struct block_device *bd = NULL;
	struct file *filp;
	struct gendisk *gd = NULL;
	struct inode *inode;
	struct scsi_device *sd = NULL;
	struct Scsi_Host *sh = (struct Scsi_Host *)hba->hba_ptr;
	char *p = (char *)page;
	int fd;

	fd = simple_strtol(p, &p, 0);
	if ((fd < 3 || fd > 7)) {
		printk(KERN_ERR "PSCSI: Illegal value of file descriptor: %d\n", fd);
		return(ERR_PTR(-EINVAL));
	}
	if (!(filp = fget(fd))) {
		printk(KERN_ERR "PSCSI: Unable to fget() fd: %d\n", fd);
		return(ERR_PTR(-EBADF));
	}
	if (!(inode = igrab(filp->f_mapping->host))) {
		printk(KERN_ERR "PSCSI: Unable to locate struct inode for struct"
				" block_device fd\n");
		fput(filp);
		return(ERR_PTR(-EINVAL));
	}
	/*
	 * Look for struct scsi_device with a backing struct block_device.
	 *
	 * This means struct scsi_device->type == TYPE_DISK && TYPE_ROM.
	 */
	if (S_ISBLK(inode->i_mode)) {
		if (!(bd = I_BDEV(filp->f_mapping->host))) {
			printk(KERN_ERR "PSCSI: Unable to locate struct"
				" block_device from I_BDEV()\n");
			iput(inode);
			fput(filp);
			return(ERR_PTR(-EINVAL));
		}
		if (!(gd = bd->bd_disk)) {
			printk(KERN_ERR "PSCSI: Unable to locate struct gendisk"
				" from struct block_device\n");
			iput(inode);
			fput(filp);
			return(ERR_PTR(-EINVAL));
		}
		/*
		 * This struct gendisk->driver_fs() is marked as "// remove'
		 * in include/linux/genhd.h..
		 *
		 * Currently in drivers/scsi/s[d,r].c:s[d,r]_probe(), this
		 * pointer gets set by struct scsi_device->sdev_gendev.
		 *
		 * Is there a better way to locate struct scsi_device from
		 * struct inode..?
		 */
		if (!(gd->driverfs_dev)) {
			printk(KERN_ERR "PSCSI: struct gendisk->driverfs_dev"
					" is NULL!\n");
			iput(inode);
			fput(filp);
			return(ERR_PTR(-EINVAL));
		}
		if (!(sd = to_scsi_device(gd->driverfs_dev))) {
			printk(KERN_ERR "PSCSI: Unable to locate struct scsi_device"
				" from struct gendisk->driverfs_dev\n");
			iput(inode);
			fput(filp);
			return(ERR_PTR(-EINVAL));
		}
		if (sd->host != sh) {
			printk(KERN_ERR "PSCSI: Trying to attach scsi_device"
				" Host ID: %d, but se_hba_t has SCSI Host ID: %d\n",
				sd->host->host_no, sh->host_no);
			iput(inode);
			fput(filp);
			return(ERR_PTR(-EINVAL));
		}
		/*
		 * pscsi_create_type_[disk,rom]() will release host_lock..
		 */
		spin_lock_irq(sh->host_lock);
		switch (sd->type) {
		case TYPE_DISK:
			dev = pscsi_create_type_disk(sd, pdv, se_dev, se_dev->se_dev_hba);
			break;
		case TYPE_ROM:
			dev = pscsi_create_type_rom(sd, pdv, se_dev, se_dev->se_dev_hba);
			break;
		default:
			printk(KERN_ERR "PSCSI: Unable to handle type S_ISBLK() =="
				" TRUE Type: %s\n", scsi_device_type(sd->type));
			spin_unlock_irq(sh->host_lock);
			iput(inode);
			fput(filp);
			return(ERR_PTR(-ENOSYS));
		}
	} else if (S_ISCHR(inode->i_mode)) {
#warning FIXME: Figure how to get struct scsi_device from character device's struct inode
		printk(KERN_ERR "SCSI Character Device unsupported via"
			" configfs/fd  method.  Use configfs/control instead\n");
		iput(inode);
		fput(filp);
		return(ERR_PTR(-ENOSYS));
	} else {
		printk(KERN_ERR "PSCSI: File destriptor is not SCSI block or character"
			" device, ignoring\n");
		iput(inode);
		fput(filp);
		return(ERR_PTR(-ENODEV));
	}

	iput(inode);
	fput(filp);
	return(dev);
}

extern void pscsi_get_plugin_info (void *p, char *b, int *bl)
{
	*bl += sprintf(b+*bl, "%s Parallel SCSI Plugin %s\n", PYX_ISCSI_VENDOR, PSCSI_VERSION);

	return;
}

extern void pscsi_get_hba_info (se_hba_t *hba, char *b, int *bl)
{
	struct Scsi_Host *sh = (struct Scsi_Host *) hba->hba_ptr;

	*bl += sprintf(b+*bl, "iSCSI Host ID: %u  SCSI Host ID: %u\n",
			 hba->hba_id, sh->host_no);
	*bl += sprintf(b+*bl, "        Parallel SCSI HBA: %s  <local>\n",
		(sh->hostt->name) ? (sh->hostt->name) : "Unknown");

	return;	
}

extern void pscsi_get_dev_info (se_device_t *dev, char *b, int *bl)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;

	__pscsi_get_dev_info(pdv, b, bl);
	return;
}

extern void __pscsi_get_dev_info (pscsi_dev_virt_t *pdv, char *b, int *bl)
{
	int i;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	*bl += sprintf(b+*bl, "SCSI Device Bus Location:"
		" Channel ID: %d Target ID: %d LUN: %d\n",
		pdv->pdv_channel_id, pdv->pdv_target_id, pdv->pdv_lun_id);

	if (sd) {
		*bl += sprintf(b+*bl, "        ");
		*bl += sprintf(b+*bl, "Vendor: ");
		for (i = 0; i < 8; i++) {
			if (ISPRINT(sd->vendor[i]))   /* printable character ? */
				*bl += sprintf(b+*bl, "%c", sd->vendor[i]);
			else
				*bl += sprintf(b+*bl, " ");
		}
		*bl += sprintf(b+*bl, " Model: ");
		for (i = 0; i < 16; i++) {
			if (ISPRINT(sd->model[i]))   /* printable character ? */
				*bl += sprintf(b+*bl, "%c", sd->model[i]);
			else
				*bl += sprintf(b+*bl, " ");
		}
		*bl += sprintf(b+*bl, " Rev: ");
		for (i = 0; i < 4; i++) {
			if (ISPRINT(sd->rev[i]))   /* printable character ? */
				*bl += sprintf(b+*bl, "%c", sd->rev[i]);
			else
				*bl += sprintf(b+*bl, " ");
		}

		if (sd->type == TYPE_DISK) {
			struct scsi_disk *sdisk = dev_get_drvdata(&sd->sdev_gendev);
			struct gendisk *disk = (struct gendisk *) sdisk->disk;
			struct block_device *bdev = bdget(MKDEV(disk->major, disk->first_minor));
	
			bdev->bd_disk = disk;
			*bl += sprintf(b+*bl, "   %s\n", (!bdev->bd_holder) ? "" :
					(bdev->bd_holder == (struct scsi_device *)sd) ?
					"CLAIMED: PSCSI" : "CLAIMED: OS");
		} else
			*bl += sprintf(b+*bl, "\n");
	}

	return;
}

extern int scsi_req_map_sg(struct request *, struct scatterlist *, int,  unsigned , gfp_t );

/*      pscsi_map_task_SG(): 
 *
 *
 */
extern int pscsi_map_task_SG (se_task_t *task)
{
        pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;
	int ret = 0;

	pt->pscsi_buf = (void *)task->task_sg;

	if (!task->task_size)
		return(0);
#if 0
	if ((ret = blk_rq_map_sg(pdv->pdv_sd->request_queue,
			pt->pscsi_req, task->task_sg)) < 0) {
		printk(KERN_ERR "PSCSI: blk_rq_map_sg() returned %d\n", ret);
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);
	}
#else
	if ((ret = scsi_req_map_sg(pt->pscsi_req, task->task_sg,
			task->task_sg_num, task->task_size, GFP_KERNEL)) < 0) {
		printk(KERN_ERR "PSCSI: scsi_req_map_sg() failed: %d\n", ret);
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);
	}
#endif
	return(0);
}

/*	pscsi_map_task_non_SG():
 *
 *
 */
extern int pscsi_map_task_non_SG (se_task_t *task)
{
	iscsi_cmd_t *cmd = task->iscsi_cmd;
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;
	unsigned char *buf = (unsigned char *) T_TASK(cmd)->t_task_buf;
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) task->iscsi_dev->dev_ptr;
	int ret = 0;

	pt->pscsi_buf = (void *)buf;

	if (!task->task_size)
		return(0);

	if ((ret = blk_rq_map_kern(pdv->pdv_sd->request_queue,
			pt->pscsi_req, pt->pscsi_buf,
			task->task_size, GFP_KERNEL)) < 0) {
		printk(KERN_ERR "PSCSI: blk_rq_map_kern() failed: %d\n", ret);
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);
	}
	return(0);
}

/*	pscsi_CDB_inquiry():
 *
 *
 */
extern int pscsi_CDB_inquiry (se_task_t *task, u32 size)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	pt->pscsi_direction = DMA_FROM_DEVICE;
	if (pscsi_blk_get_request(task) < 0)
		return(-1);

	return(pscsi_map_task_non_SG(task));
}

extern int pscsi_CDB_none (se_task_t *task, u32 size)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	pt->pscsi_direction = DMA_NONE;

	return(pscsi_blk_get_request(task));
}

/*	pscsi_CDB_read_non_SG():
 *
 *
 */
extern int pscsi_CDB_read_non_SG (se_task_t *task, u32 size)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	pt->pscsi_direction = DMA_FROM_DEVICE;

	if (pscsi_blk_get_request(task) < 0)
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);

	return(pscsi_map_task_non_SG(task));
}

/*	pscsi_CDB_read_SG():
 *
 *
 */
extern int pscsi_CDB_read_SG (se_task_t *task, u32 size)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	pt->pscsi_direction = DMA_FROM_DEVICE;

	if (pscsi_blk_get_request(task) < 0)
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);

	if (pscsi_map_task_SG(task) < 0)
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);

	return(task->task_sg_num);
}

/*	pscsi_CDB_write_non_SG():
 *
 *
 */
extern int pscsi_CDB_write_non_SG (se_task_t *task, u32 size)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	pt->pscsi_direction = DMA_TO_DEVICE;

	if (pscsi_blk_get_request(task) < 0)
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);

	return(pscsi_map_task_non_SG(task));
}

/*	pscsi_CDB_write_SG():
 *
 *
 */
extern int pscsi_CDB_write_SG (se_task_t *task, u32 size)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	pt->pscsi_direction = DMA_TO_DEVICE;

	if (pscsi_blk_get_request(task) < 0)
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);

	if (pscsi_map_task_SG(task) < 0)
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);
	
	return(task->task_sg_num);
}

/*	pscsi_check_lba():
 *
 *
 */
extern int pscsi_check_lba (unsigned long long lba, se_device_t *dev)
{
	return(0);
}

/*	pscsi_check_for_SG():
 *
 *
 */
extern int pscsi_check_for_SG (se_task_t *task)
{
	return(task->task_sg_num);
}

/*	pscsi_get_cdb():
 *
 *
 */
extern unsigned char *pscsi_get_cdb (se_task_t *task)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	return(pt->pscsi_cdb);
}

/*	pscsi_get_sense_buffer():
 *
 *
 */
extern unsigned char *pscsi_get_sense_buffer (se_task_t *task)
{
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *) task->transport_req;

	return((unsigned char *)&pt->pscsi_sense[0]);
}

/*	pscsi_get_blocksize():
 *
 *
 */
extern u32 pscsi_get_blocksize (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return(sd->sector_size);
}

/*	pscsi_get_device_rev():
 *
 *
 */
extern u32 pscsi_get_device_rev (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	
	return((sd->scsi_level - 1) ? sd->scsi_level - 1 : 1);
}

/*	pscsi_get_device_type():
 *
 *
 */
extern u32 pscsi_get_device_type (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return(sd->type);
}

/*	pscsi_get_dma_length():
 *
 *
 */
extern u32 pscsi_get_dma_length (u32 task_size, se_device_t *dev)
{
	return(PAGE_SIZE);
}

/*	pscsi_get_max_sectors():
 *
 *
 */
extern u32 pscsi_get_max_sectors (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	return((sd->host->max_sectors > sd->request_queue->max_sectors) ?
		sd->request_queue->max_sectors : sd->host->max_sectors);
}

/*	pscsi_get_queue_depth():
 *
 *
 */
extern u32 pscsi_get_queue_depth (se_device_t *dev)
{
	pscsi_dev_virt_t *pdv = (pscsi_dev_virt_t *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return(sd->queue_depth);
}

extern void pscsi_shutdown_hba (se_hba_t *hba)
{
	struct Scsi_Host *sh = (struct Scsi_Host *)hba->hba_ptr;

	if (strcmp(sh->hostt->name, INIT_CORE_NAME)) {
		/*
		 * Perhaps we could force a host-reset here if outstanding tasks
		 * have not come back..
		 */
		return;
	}
#warning FIXME: iscsi_global breakage in iscsi_target_pscsi.c
#if 0	
	if (!iscsi_global->ti_forcechanoffline)
		return;
	
	/*
	 * Notify the iSCSI Initiator to perform pause/forcechanoffline operations
	 */
	iscsi_global->ti_forcechanoffline(hba->hba_ptr);
#endif
	return;
}

/*	pscsi_handle_SAM_STATUS_failures():
 *
 *
 */
static inline void pscsi_process_SAM_status (
	se_task_t *task,
	pscsi_plugin_task_t *pt)
{
	if ((task->task_scsi_status = status_byte(pt->pscsi_result))) {
		task->task_scsi_status <<= 1;
		PYXPRINT("PSCSI Status Byte exception at task: %p CDB: 0x%02x"
			" Result: 0x%08x\n", task, pt->pscsi_cdb[0],
			pt->pscsi_result);
	}

	switch (host_byte(pt->pscsi_result)) {
	case DID_OK:
		transport_complete_task(task, (!task->task_scsi_status));
		break;
	default:
		PYXPRINT("PSCSI Host Byte exception at task: %p CDB: 0x%02x"
			" Result: 0x%08x\n", task, pt->pscsi_cdb[0],
			pt->pscsi_result);
		task->task_scsi_status = SAM_STAT_CHECK_CONDITION;
		task->task_error_status = PYX_TRANSPORT_UNKNOWN_SAM_OPCODE;
		task->iscsi_cmd->transport_error_status = PYX_TRANSPORT_UNKNOWN_SAM_OPCODE;
		transport_complete_task(task, 0);
		break;
	}

	return;
}

extern void pscsi_req_done (struct request *req, int uptodate)
{
	se_task_t *task = (se_task_t *)req->end_io_data;	
	pscsi_plugin_task_t *pt = (pscsi_plugin_task_t *)task->transport_req;

	pt->pscsi_result = req->errors;
	pt->pscsi_resid = req->data_len;
	
	pscsi_process_SAM_status(task, pt);
	__blk_put_request(req->q, req);
	pt->pscsi_req = NULL;

	return;
}
