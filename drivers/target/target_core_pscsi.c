/*******************************************************************************
 * Filename:  target_core_pscsi.c
 *
 * This file contains the generic target mode <-> Linux SCSI subsystem plugin.
 *
 * Copyright (c) 2003, 2004, 2005 PyX Technologies, Inc.
 * Copyright (c) 2005, 2006, 2007 SBE, Inc.
 * Copyright (c) 2007-2010 Rising Tide Systems
 * Copyright (c) 2008-2010 Linux-iSCSI.org
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
 ******************************************************************************/

#include <linux/version.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/blkdev.h>
#include <linux/blk_types.h>
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

#include <target/target_core_base.h>
#include <target/target_core_device.h>
#include <target/target_core_transport.h>

#include "target_core_pscsi.h"

#define ISPRINT(a)  ((a >= ' ') && (a <= '~'))

static struct se_subsystem_api pscsi_template;

static void pscsi_req_done(struct request *, int);
static void __pscsi_get_dev_info(struct pscsi_dev_virt *, char *, int *);

/*	pscsi_get_sh():
 *
 *
 */
static struct Scsi_Host *pscsi_get_sh(u32 host_no)
{
	struct Scsi_Host *sh = NULL;

	sh = scsi_host_lookup(host_no);
	if (IS_ERR(sh)) {
		printk(KERN_ERR "Unable to locate SCSI HBA with Host ID:"
				" %u\n", host_no);
		return NULL;
	}

	return sh;
}

/*	pscsi_attach_hba():
 *
 * 	pscsi_get_sh() used scsi_host_lookup() to locate struct Scsi_Host.
 *	from the passed SCSI Host ID.
 */
static int pscsi_attach_hba(struct se_hba *hba, u32 host_id)
{
	int hba_depth;
	struct pscsi_hba_virt *phv;

	phv = kzalloc(sizeof(struct pscsi_hba_virt), GFP_KERNEL);
	if (!(phv)) {
		printk(KERN_ERR "Unable to allocate struct pscsi_hba_virt\n");
		return -1;
	}
	phv->phv_host_id = host_id;
	phv->phv_mode = PHV_VIRUTAL_HOST_ID;
	hba_depth = PSCSI_VIRTUAL_HBA_DEPTH;
	atomic_set(&hba->left_queue_depth, hba_depth);
	atomic_set(&hba->max_queue_depth, hba_depth);

	hba->hba_ptr = (void *)phv;

	printk(KERN_INFO "CORE_HBA[%d] - TCM SCSI HBA Driver %s on"
		" Generic Target Core Stack %s\n", hba->hba_id,
		PSCSI_VERSION, TARGET_CORE_MOD_VERSION);
	printk(KERN_INFO "CORE_HBA[%d] - Attached SCSI HBA to Generic"
		" Target Core with TCQ Depth: %d\n", hba->hba_id,
		atomic_read(&hba->max_queue_depth));

	return 0;
}

/*	pscsi_detach_hba(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int pscsi_detach_hba(struct se_hba *hba)
{
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)hba->hba_ptr;
	struct Scsi_Host *scsi_host = phv->phv_lld_host;

	if (scsi_host) {
		scsi_host_put(scsi_host);

		printk(KERN_INFO "CORE_HBA[%d] - Detached SCSI HBA: %s from"
			" Generic Target Core\n", hba->hba_id,
			(scsi_host->hostt->name) ? (scsi_host->hostt->name) :
			"Unknown");
	} else
		printk(KERN_INFO "CORE_HBA[%d] - Detached Virtual SCSI HBA"
			" from Generic Target Core\n", hba->hba_id);

	kfree(phv);
	hba->hba_ptr = NULL;

	return 0;
}

static int pscsi_pmode_enable_hba(struct se_hba *hba, unsigned long mode_flag)
{
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)hba->hba_ptr;
	struct Scsi_Host *sh = phv->phv_lld_host;
	int hba_depth = PSCSI_VIRTUAL_HBA_DEPTH;
	/*
	 * Release the struct Scsi_Host
	 */
	if (!(mode_flag)) {
		if (!(sh))
			return 0;

		phv->phv_lld_host = NULL;
		phv->phv_mode = PHV_VIRUTAL_HOST_ID;
		atomic_set(&hba->left_queue_depth, hba_depth);
		atomic_set(&hba->max_queue_depth, hba_depth);

		printk(KERN_INFO "CORE_HBA[%d] - Disabled pSCSI HBA Passthrough"
			" %s\n", hba->hba_id, (sh->hostt->name) ?
			(sh->hostt->name) : "Unknown");

		scsi_host_put(sh);
		return 0;
	}
	/*
	 * Otherwise, locate struct Scsi_Host from the original passed
	 * pSCSI Host ID and enable for phba mode
	 */
	sh = pscsi_get_sh(phv->phv_host_id);
	if (!(sh)) {
		printk(KERN_ERR "pSCSI: Unable to locate SCSI Host for"
			" phv_host_id: %d\n", phv->phv_host_id);
		return -1;
	}
	/*
	 * Usually the SCSI LLD will use the hostt->can_queue value to define
	 * its HBA TCQ depth.  Some other drivers (like 2.6 megaraid) don't set
	 * this at all and set sh->can_queue at runtime.
	 */
	hba_depth = (sh->hostt->can_queue > sh->can_queue) ?
		sh->hostt->can_queue : sh->can_queue;

	atomic_set(&hba->left_queue_depth, hba_depth);
	atomic_set(&hba->max_queue_depth, hba_depth);

	phv->phv_lld_host = sh;
	phv->phv_mode = PHV_LLD_SCSI_HOST_NO;

	printk(KERN_INFO "CORE_HBA[%d] - Enabled pSCSI HBA Passthrough %s\n",
		hba->hba_id, (sh->hostt->name) ? (sh->hostt->name) : "Unknown");

	return 1;
}

/*	pscsi_add_device_to_list():
 *
 *
 */
static struct se_device *pscsi_add_device_to_list(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	struct pscsi_dev_virt *pdv,
	struct scsi_device *sd,
	int dev_flags)
{
	struct se_device *dev;

	/*
	 * Some pseudo SCSI HBAs do not fill in sector_size
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
			printk(KERN_ERR "Unable to set sector_size for %d\n",
					sd->type);
			return NULL;
		}

		if (sd->sector_size) {
			printk(KERN_ERR "Set broken SCSI Device"
				" %d:%d:%d sector_size to %d\n", sd->channel,
				sd->id, sd->lun, sd->sector_size);
		}
	}

	if (!sd->queue_depth) {
		sd->queue_depth = PSCSI_DEFAULT_QUEUEDEPTH;

		printk(KERN_ERR "Set broken SCSI Device %d:%d:%d"
			" queue_depth to %d\n", sd->channel, sd->id,
				sd->lun, sd->queue_depth);
	}
	/*
	 * Set the pointer pdv->pdv_sd to from passed struct scsi_device,
	 * which has already been referenced with Linux SCSI code with
	 * scsi_device_get() in this file's pscsi_create_virtdevice().
	 *
	 * The passthrough operations called by the transport_add_device_*
	 * function below will require this pointer to be set for passthroug
	 *  ops.
	 *
	 * For the shutdown case in pscsi_free_device(), this struct
	 * scsi_device  reference is released with Linux SCSI code
	 * scsi_device_put() and the pdv->pdv_sd cleared.
	 */
	pdv->pdv_sd = sd;

	dev = transport_add_device_to_core_hba(hba, &pscsi_template,
				se_dev, dev_flags, (void *)pdv);
	if (!(dev)) {
		pdv->pdv_sd = NULL;
		return NULL;
	}

	/*
	 * For TYPE_TAPE, attempt to determine blocksize with MODE_SENSE.
	 */
	if (sd->type == TYPE_TAPE) {
		unsigned char *buf = NULL, cdb[MAX_COMMAND_SIZE];
		struct se_cmd *cmd;
		u32 blocksize;

		memset(cdb, 0, MAX_COMMAND_SIZE);
		cdb[0] = MODE_SENSE;
		cdb[4] = 0x0c; /* 12 bytes */

		cmd = transport_allocate_passthrough(&cdb[0],
				DMA_FROM_DEVICE, 0, NULL, 0, 12, dev);
		if (!(cmd)) {
			printk(KERN_ERR "Unable to determine blocksize for"
				" TYPE_TAPE\n");
			goto out;
		}

		if (transport_generic_passthrough(cmd) < 0) {
			printk(KERN_ERR "Unable to determine blocksize for"
				" TYPE_TAPE\n");
			goto out;
		}

		buf = (unsigned char *)T_TASK(cmd)->t_task_buf;
		blocksize = (buf[9] << 16) | (buf[10] << 8) | (buf[11]);

		/*
		 * If MODE_SENSE still returns zero, set the default value
		 * to 1024.
		 */
		sd->sector_size = blocksize;
		if (!(sd->sector_size))
			sd->sector_size = 1024;

		transport_passthrough_release(cmd);
	}
out:
	return dev;
}

static void *pscsi_allocate_virtdevice(struct se_hba *hba, const char *name)
{
	struct pscsi_dev_virt *pdv;

	pdv = kzalloc(sizeof(struct pscsi_dev_virt), GFP_KERNEL);
	if (!(pdv)) {
		printk(KERN_ERR "Unable to allocate memory for struct pscsi_dev_virt\n");
		return NULL;
	}
	pdv->pdv_se_hba = hba;

	printk(KERN_INFO "PSCSI: Allocated pdv: %p for %s\n", pdv, name);
	return (void *)pdv;
}

/*
 * Called with struct Scsi_Host->host_lock called.
 */
static struct se_device *pscsi_create_type_disk(
	struct scsi_device *sd,
	struct pscsi_dev_virt *pdv,
	struct se_subsystem_dev *se_dev,
	struct se_hba *hba)
{
	struct se_device *dev;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)pdv->pdv_se_hba->hba_ptr;
	struct Scsi_Host *sh = sd->host;
	struct block_device *bd;
	u32 dev_flags = 0;

	if (scsi_device_get(sd)) {
		printk(KERN_ERR "scsi_device_get() failed for %d:%d:%d:%d\n",
			sh->host_no, sd->channel, sd->id, sd->lun);
		spin_unlock_irq(sh->host_lock);
		return NULL;
	}
	spin_unlock_irq(sh->host_lock);
	/*
	 * Claim exclusive struct block_device access to struct scsi_device
	 * for TYPE_DISK using supplied udev_path
	 */
	bd = open_bdev_exclusive(se_dev->se_dev_udev_path,
				FMODE_WRITE|FMODE_READ, pdv);
	if (!(bd)) {
		printk("pSCSI: open_bdev_exclusive() failed\n");
		scsi_device_put(sd);
		return NULL;
	}
	pdv->pdv_bd = bd;

	dev = pscsi_add_device_to_list(hba, se_dev, pdv, sd, dev_flags);
	if (!(dev)) {
		close_bdev_exclusive(pdv->pdv_bd, FMODE_WRITE|FMODE_READ);
		scsi_device_put(sd);
		return NULL;
	}
	printk(KERN_INFO "CORE_PSCSI[%d] - Added TYPE_DISK for %d:%d:%d:%d\n",
		phv->phv_host_id, sh->host_no, sd->channel, sd->id, sd->lun);

	return dev;
}

/*
 * Called with struct Scsi_Host->host_lock called.
 */
static struct se_device *pscsi_create_type_rom(
	struct scsi_device *sd,
	struct pscsi_dev_virt *pdv,
	struct se_subsystem_dev *se_dev,
	struct se_hba *hba)
{
	struct se_device *dev;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)pdv->pdv_se_hba->hba_ptr;
	struct Scsi_Host *sh = sd->host;
	u32 dev_flags = 0;

	if (scsi_device_get(sd)) {
		printk(KERN_ERR "scsi_device_get() failed for %d:%d:%d:%d\n",
			sh->host_no, sd->channel, sd->id, sd->lun);
		spin_unlock_irq(sh->host_lock);
		return NULL;
	}
	spin_unlock_irq(sh->host_lock);

	dev = pscsi_add_device_to_list(hba, se_dev, pdv, sd, dev_flags);
	if (!(dev)) {
		scsi_device_put(sd);
		return NULL;
	}
	printk(KERN_INFO "CORE_PSCSI[%d] - Added Type: %s for %d:%d:%d:%d\n",
		phv->phv_host_id, scsi_device_type(sd->type), sh->host_no,
		sd->channel, sd->id, sd->lun);

	return dev;
}

/*
 *Called with struct Scsi_Host->host_lock called.
 */
static struct se_device *pscsi_create_type_other(
	struct scsi_device *sd,
	struct pscsi_dev_virt *pdv,
	struct se_subsystem_dev *se_dev,
	struct se_hba *hba)
{
	struct se_device *dev;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)pdv->pdv_se_hba->hba_ptr;
	struct Scsi_Host *sh = sd->host;
	u32 dev_flags = 0;

	spin_unlock_irq(sh->host_lock);
	dev = pscsi_add_device_to_list(hba, se_dev, pdv, sd, dev_flags);
	if (!(dev))
		return NULL;

	printk(KERN_INFO "CORE_PSCSI[%d] - Added Type: %s for %d:%d:%d:%d\n",
		phv->phv_host_id, scsi_device_type(sd->type), sh->host_no,
		sd->channel, sd->id, sd->lun);

	return dev;
}

static struct se_device *pscsi_create_virtdevice(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	void *p)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *)p;
	struct se_device *dev;
	struct scsi_device *sd;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)hba->hba_ptr;
	struct Scsi_Host *sh = phv->phv_lld_host;
	int legacy_mode_enable = 0;

	if (!(pdv)) {
		printk(KERN_ERR "Unable to locate struct pscsi_dev_virt"
				" parameter\n");
		return NULL;
	}
	/*
	 * If not running in PHV_LLD_SCSI_HOST_NO mode, locate the
	 * struct Scsi_Host we will need to bring the TCM/pSCSI object online
	 */
	if (!(sh)) {
		if (phv->phv_mode == PHV_LLD_SCSI_HOST_NO) {
			printk(KERN_ERR "pSCSI: Unable to locate struct"
				" Scsi_Host for PHV_LLD_SCSI_HOST_NO\n");
			return NULL;
		}
		/*
		 * For the newer PHV_VIRUTAL_HOST_ID struct scsi_device
		 * reference, we enforce that udev_path has been set
		 */
		if (!(se_dev->su_dev_flags & SDF_USING_UDEV_PATH)) {
			printk(KERN_ERR "pSCSI: udev_path attribute has not"
				" been set before ENABLE=1\n");
			return NULL;
		}
		/*
		 * If no scsi_host_id= was passed for PHV_VIRUTAL_HOST_ID,
		 * use the original TCM hba ID to reference Linux/SCSI Host No
		 * and enable for PHV_LLD_SCSI_HOST_NO mode.
		 */
		if (!(pdv->pdv_flags & PDF_HAS_VIRT_HOST_ID)) {
			spin_lock(&hba->device_lock);
			if (!(list_empty(&hba->hba_dev_list))) {
				printk(KERN_ERR "pSCSI: Unable to set hba_mode"
					" with active devices\n");
				spin_unlock(&hba->device_lock);
				return NULL;
			}
			spin_unlock(&hba->device_lock);

			if (pscsi_pmode_enable_hba(hba, 1) != 1)
				return NULL;

			legacy_mode_enable = 1;
			hba->hba_flags |= HBA_FLAGS_PSCSI_MODE;
			sh = phv->phv_lld_host;
		} else {
			sh = pscsi_get_sh(pdv->pdv_host_id);
			if (!(sh)) {
				printk(KERN_ERR "pSCSI: Unable to locate"
					" pdv_host_id: %d\n", pdv->pdv_host_id);
				return NULL;
			}
		}
	} else {
		if (phv->phv_mode == PHV_VIRUTAL_HOST_ID) {
			printk(KERN_ERR "pSCSI: PHV_VIRUTAL_HOST_ID set while"
				" struct Scsi_Host exists\n");
			return NULL;
		}
	}

	spin_lock_irq(sh->host_lock);
	list_for_each_entry(sd, &sh->__devices, siblings) {
		if ((pdv->pdv_channel_id != sd->channel) ||
		    (pdv->pdv_target_id != sd->id) ||
		    (pdv->pdv_lun_id != sd->lun))
			continue;
		/*
		 * Functions will release the held struct scsi_host->host_lock
		 * before calling calling pscsi_add_device_to_list() to register
		 * struct scsi_device with target_core_mod.
		 */
		switch (sd->type) {
		case TYPE_DISK:
			dev = pscsi_create_type_disk(sd, pdv, se_dev, hba);
			break;
		case TYPE_ROM:
			dev = pscsi_create_type_rom(sd, pdv, se_dev, hba);
			break;
		default:
			dev = pscsi_create_type_other(sd, pdv, se_dev, hba);
			break;
		}

		if (!(dev)) {
			if (phv->phv_mode == PHV_VIRUTAL_HOST_ID)
				scsi_host_put(sh);
			else if (legacy_mode_enable) {
				pscsi_pmode_enable_hba(hba, 0);
				hba->hba_flags &= ~HBA_FLAGS_PSCSI_MODE;
			}
			pdv->pdv_sd = NULL;
			return NULL;
		}
		return dev;
	}
	spin_unlock_irq(sh->host_lock);

	printk(KERN_ERR "pSCSI: Unable to locate %d:%d:%d:%d\n", sh->host_no,
		pdv->pdv_channel_id,  pdv->pdv_target_id, pdv->pdv_lun_id);

	if (phv->phv_mode == PHV_VIRUTAL_HOST_ID)
		scsi_host_put(sh);
	else if (legacy_mode_enable) {
		pscsi_pmode_enable_hba(hba, 0);
		hba->hba_flags &= ~HBA_FLAGS_PSCSI_MODE;
	}

	return NULL;
}

/*	pscsi_activate_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int pscsi_activate_device(struct se_device *dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *) pdv->pdv_se_hba->hba_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	struct Scsi_Host *sh = sd->host;

	printk(KERN_INFO "CORE_PSCSI[%d] - Activating Device with TCQ: %d at"
		" SCSI Location (Host/Channel/Target/LUN) %d/%d/%d/%d\n",
		phv->phv_host_id, sd->queue_depth, sh->host_no, sd->channel,
		sd->id, sd->lun);

	return 0;
}

/*	pscsi_deactivate_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
static void pscsi_deactivate_device(struct se_device *dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *) pdv->pdv_se_hba->hba_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	struct Scsi_Host *sh = sd->host;

	printk(KERN_INFO "CORE_PSCSI[%d] - Deactivating Device with TCQ: %d at"
		" SCSI Location (Host/Channel/Target/LUN) %d/%d/%d/%d\n",
		phv->phv_host_id, sd->queue_depth, sh->host_no, sd->channel,
		sd->id, sd->lun);
}

/*	pscsi_free_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
static void pscsi_free_device(void *p)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) p;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *) pdv->pdv_se_hba->hba_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	if (sd) {
		/*
		 * Release exclusive pSCSI internal struct block_device claim for
		 * struct scsi_device with TYPE_DISK from pscsi_create_type_disk()
		 */
		if ((sd->type == TYPE_DISK) && pdv->pdv_bd) {
        		close_bdev_exclusive(pdv->pdv_bd,
					FMODE_WRITE|FMODE_READ);
			pdv->pdv_bd = NULL;
		}
		/*
		 * For HBA mode PHV_LLD_SCSI_HOST_NO, release the reference
		 * to struct Scsi_Host now.
		 */
		if ((phv->phv_mode == PHV_LLD_SCSI_HOST_NO) &&
		    (phv->phv_lld_host != NULL))
			scsi_host_put(phv->phv_lld_host);

		if ((sd->type == TYPE_DISK) || (sd->type == TYPE_ROM))
			scsi_device_put(sd);

		pdv->pdv_sd = NULL;
	}

	kfree(pdv);
}

/*	pscsi_transport_complete():
 *
 *
 */
static int pscsi_transport_complete(struct se_task *task)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) task->se_dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	int result;
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;
	unsigned char *cdb = &pt->pscsi_cdb[0];

	result = pt->pscsi_result;
	/*
	 * Hack to make sure that Write-Protect modepage is set if R/O mode is
	 * forced.
	 */
	if (((cdb[0] == MODE_SENSE) || (cdb[0] == MODE_SENSE_10)) &&
	     (status_byte(result) << 1) == SAM_STAT_GOOD) {
		if (!TASK_CMD(task)->se_deve)
			goto after_mode_sense;

		if (TASK_CMD(task)->se_deve->lun_flags &
				TRANSPORT_LUNFLAGS_READ_ONLY) {
			unsigned char *buf = (unsigned char *)
				T_TASK(task->task_se_cmd)->t_task_buf;

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
	 * Hack to correctly obtain the initiator requested blocksize for
	 * TYPE_TAPE.  Since this value is dependent upon each tape media,
	 * struct scsi_device->sector_size will not contain the correct value
	 * by default, so we go ahead and set it so
	 * TRANSPORT(dev)->get_blockdev() returns the correct value to the
	 * storage engine.
	 */
	if (((cdb[0] == MODE_SELECT) || (cdb[0] == MODE_SELECT_10)) &&
	      (status_byte(result) << 1) == SAM_STAT_GOOD) {
		unsigned char *buf;
		struct scatterlist *sg = task->task_sg;
		u16 bdl;
		u32 blocksize;

		buf = sg_virt(&sg[0]);
		if (!(buf)) {
			printk(KERN_ERR "Unable to get buf for scatterlist\n");
			goto after_mode_select;
		}

		if (cdb[0] == MODE_SELECT)
			bdl = (buf[3]);
		else
			bdl = (buf[6] << 8) | (buf[7]);

		if (!bdl)
			goto after_mode_select;

		if (cdb[0] == MODE_SELECT)
			blocksize = (buf[9] << 16) | (buf[10] << 8) |
					(buf[11]);
		else
			blocksize = (buf[13] << 16) | (buf[14] << 8) |
					(buf[15]);

		sd->sector_size = blocksize;
	}
after_mode_select:

	if (status_byte(result) & CHECK_CONDITION)
		return 1;

	return 0;
}

/*	pscsi_allocate_request(): (Part of se_subsystem_api_t template)
 *
 *
 */
static void *pscsi_allocate_request(
	struct se_task *task,
	struct se_device *dev)
{
	struct pscsi_plugin_task *pt;

	pt = kzalloc(sizeof(struct pscsi_plugin_task), GFP_KERNEL);
	if (!(pt)) {
		printk(KERN_ERR "Unable to allocate struct pscsi_plugin_task\n");
		return NULL;
	}

	return pt;
}

static inline void pscsi_blk_init_request(
	struct se_task *task,
	struct pscsi_plugin_task *pt)
{
	/*
	 * Defined as "scsi command" in include/linux/blkdev.h.
	 */
	pt->pscsi_req->cmd_type = REQ_TYPE_BLOCK_PC;
	/*
	 * Setup the done function pointer for struct request,
	 * also set the end_io_data pointer.to struct se_task.
	 */
	pt->pscsi_req->end_io = pscsi_req_done;
	pt->pscsi_req->end_io_data = (void *)task;
	/*
	 * Load the referenced struct se_task's SCSI CDB into
	 * include/linux/blkdev.h:struct request->cmd
	 */
	pt->pscsi_req->cmd_len = scsi_command_size(pt->pscsi_cdb);
	memcpy(pt->pscsi_req->cmd, pt->pscsi_cdb, pt->pscsi_req->cmd_len);
	/*
	 * Setup pointer for outgoing sense data.
	 */
	pt->pscsi_req->sense = (void *)&pt->pscsi_sense[0];
	pt->pscsi_req->sense_len = 0;
}

/*
 * Used for pSCSI data payloads for all *NON* SCF_SCSI_DATA_SG_IO_CDB
*/
static int pscsi_blk_get_request(struct se_task *task)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) task->se_dev->dev_ptr;

	pt->pscsi_req = blk_get_request(pdv->pdv_sd->request_queue,
			(pt->pscsi_direction == DMA_TO_DEVICE), GFP_KERNEL);
	if (!(pt->pscsi_req) || IS_ERR(pt->pscsi_req)) {
		printk(KERN_ERR "PSCSI: blk_get_request() failed: %ld\n",
				IS_ERR(pt->pscsi_req));
		return PYX_TRANSPORT_LU_COMM_FAILURE;
	}
	/*
	 * Setup the newly allocated struct request for REQ_TYPE_BLOCK_PC,
	 * and setup rq callback, CDB and sense.
	 */
	pscsi_blk_init_request(task, pt);
	return 0;
}

/*      pscsi_do_task(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int pscsi_do_task(struct se_task *task)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) task->se_dev->dev_ptr;
	/*
	 * Set the struct request->timeout value based on peripheral
	 * device type from SCSI.
	 */
	if (pdv->pdv_sd->type == TYPE_DISK)
		pt->pscsi_req->timeout = PS_TIMEOUT_DISK;
	else
		pt->pscsi_req->timeout = PS_TIMEOUT_OTHER;

	pt->pscsi_req->retries = PS_RETRY;
	/*
	 * Queue the struct request into the struct scsi_device->request_queue.
	 */
	blk_execute_rq_nowait(pdv->pdv_sd->request_queue, NULL,
			      pt->pscsi_req, 1, pscsi_req_done);

	return PYX_TRANSPORT_SENT_TO_TRANSPORT;
}

/*	pscsi_free_task(): (Part of se_subsystem_api_t template)
 *
 *
 */
static void pscsi_free_task(struct se_task *task)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *)task->transport_req;
	/*
	 * We do not release the bio(s) here associated with this task, as
	 * this is handled by bio_put() and pscsi_bi_endio().
	 */
	kfree(pt);
}

static ssize_t pscsi_set_configfs_dev_params(struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	const char *page,
	ssize_t count)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) se_dev->se_dev_su_ptr;
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)hba->hba_ptr;
	char *buf, *cur, *ptr, *ptr2;
	unsigned long scsi_host_id, scsi_channel_id;
	unsigned long scsi_target_id, scsi_lun_id;
	int params = 0, ret;

	buf = kzalloc(count, GFP_KERNEL);
	if (!(buf)) {
		printk(KERN_ERR "Unable to allocate memory for temporary"
				" buffer\n");
		return -ENOMEM;
	}
	memcpy(buf, page, count);
	cur = buf;

	while (cur) {
		ptr = strstr(cur, "=");
		if (!(ptr))
			goto out;

		*ptr = '\0';
		ptr++;

		ptr2 = strstr(cur, "scsi_host_id");
		if ((ptr2)) {
			transport_check_dev_params_delim(ptr, &cur);
			if (phv->phv_mode == PHV_LLD_SCSI_HOST_NO) {
				printk(KERN_ERR "PSCSI[%d]: Unable to accept"
					" scsi_host_id while phv_mode =="
					" PHV_LLD_SCSI_HOST_NO\n",
					phv->phv_host_id);
				break;
			}
			ret = strict_strtoul(ptr, 0, &scsi_host_id);
			if (ret < 0) {
				printk(KERN_ERR "strict_strtoul() failed for"
					" scsi_hostl_id=\n");
				break;
			}
			pdv->pdv_host_id = (int)scsi_host_id;
			printk(KERN_INFO "PSCSI[%d]: Referencing SCSI Host ID:"
				" %d\n", phv->phv_host_id, pdv->pdv_host_id);
			pdv->pdv_flags |= PDF_HAS_VIRT_HOST_ID;
			params++;
			continue;
		}
		ptr2 = strstr(cur, "scsi_channel_id");
		if ((ptr2)) {
			transport_check_dev_params_delim(ptr, &cur);
			ret = strict_strtoul(ptr, 0, &scsi_channel_id);
			if (ret < 0) {
				printk(KERN_ERR "strict_strtoul() failed for"
					" scsi_channel_id=\n");
				break;
			}
			pdv->pdv_channel_id = (int)scsi_channel_id;
			printk(KERN_INFO "PSCSI[%d]: Referencing SCSI Channel"
				" ID: %d\n",  phv->phv_host_id,
				pdv->pdv_channel_id);
			pdv->pdv_flags |= PDF_HAS_CHANNEL_ID;
			params++;
			continue;
		}
		ptr2 = strstr(cur, "scsi_target_id");
		if ((ptr2)) {
			transport_check_dev_params_delim(ptr, &cur);
			ret = strict_strtoul(ptr, 0, &scsi_target_id);
			if (ret < 0) {
				printk("strict_strtoul() failed for"
					" strict_strtoul()\n");
				break;
			}
			pdv->pdv_target_id = (int)scsi_target_id;
			printk(KERN_INFO "PSCSI[%d]: Referencing SCSI Target"
				" ID: %d\n", phv->phv_host_id,
				pdv->pdv_target_id);
			pdv->pdv_flags |= PDF_HAS_TARGET_ID;
			params++;
			continue;
		}
		ptr2 = strstr(cur, "scsi_lun_id");
		if ((ptr2)) {
			transport_check_dev_params_delim(ptr, &cur);
			ret = strict_strtoul(ptr, 0, &scsi_lun_id);
			if (ret < 0) {
				printk("strict_strtoul() failed for"
					" scsi_lun_id=\n");
				break;
			}
			pdv->pdv_lun_id = (int)scsi_lun_id;
			printk(KERN_INFO "PSCSI[%d]: Referencing SCSI LUN ID:"
				" %d\n", phv->phv_host_id, pdv->pdv_lun_id);
			pdv->pdv_flags |= PDF_HAS_LUN_ID;
			params++;
		} else
			cur = NULL;
	}

out:
	kfree(buf);
	return (params) ? count : -EINVAL;
}

static ssize_t pscsi_check_configfs_dev_params(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) se_dev->se_dev_su_ptr;

	if (!(pdv->pdv_flags & PDF_HAS_CHANNEL_ID) ||
	    !(pdv->pdv_flags & PDF_HAS_TARGET_ID) ||
	    !(pdv->pdv_flags & PDF_HAS_LUN_ID)) {
		printk(KERN_ERR "Missing scsi_channel_id=, scsi_target_id= and"
			" scsi_lun_id= parameters\n");
		return -1;
	}

	return 0;
}

static ssize_t pscsi_show_configfs_dev_params(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	char *page)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) se_dev->se_dev_su_ptr;
	int bl = 0;

	__pscsi_get_dev_info(pdv, page, &bl);
	return (ssize_t)bl;
}

static void pscsi_get_plugin_info(void *p, char *b, int *bl)
{
	*bl += sprintf(b + *bl, "TCM SCSI Plugin %s\n", PSCSI_VERSION);
}

static void pscsi_get_hba_info(struct se_hba *hba, char *b, int *bl)
{
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *)hba->hba_ptr;
	struct Scsi_Host *sh = phv->phv_lld_host;

	*bl += sprintf(b + *bl, "Core Host ID: %u  PHV Host ID: %u\n",
		 hba->hba_id, phv->phv_host_id);
	if (sh)
		*bl += sprintf(b + *bl, "        SCSI HBA ID %u: %s  <local>\n",
			sh->host_no, (sh->hostt->name) ?
			(sh->hostt->name) : "Unknown");
}

static void pscsi_get_dev_info(struct se_device *dev, char *b, int *bl)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;

	__pscsi_get_dev_info(pdv, b, bl);
}

static void __pscsi_get_dev_info(struct pscsi_dev_virt *pdv, char *b, int *bl)
{
	struct pscsi_hba_virt *phv = (struct pscsi_hba_virt *) pdv->pdv_se_hba->hba_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;
	unsigned char host_id[16];
	int i;

	if (phv->phv_mode == PHV_VIRUTAL_HOST_ID)
		snprintf(host_id, 16, "%d", pdv->pdv_host_id);
	else
		snprintf(host_id, 16, "PHBA Mode");

	*bl += sprintf(b + *bl, "SCSI Device Bus Location:"
		" Channel ID: %d Target ID: %d LUN: %d Host ID: %s\n",
		pdv->pdv_channel_id, pdv->pdv_target_id, pdv->pdv_lun_id,
		host_id);

	if (sd) {
		*bl += sprintf(b + *bl, "        ");
		*bl += sprintf(b + *bl, "Vendor: ");
		for (i = 0; i < 8; i++) {
			if (ISPRINT(sd->vendor[i]))   /* printable character? */
				*bl += sprintf(b + *bl, "%c", sd->vendor[i]);
			else
				*bl += sprintf(b + *bl, " ");
		}
		*bl += sprintf(b + *bl, " Model: ");
		for (i = 0; i < 16; i++) {
			if (ISPRINT(sd->model[i]))   /* printable character ? */
				*bl += sprintf(b + *bl, "%c", sd->model[i]);
			else
				*bl += sprintf(b + *bl, " ");
		}
		*bl += sprintf(b + *bl, " Rev: ");
		for (i = 0; i < 4; i++) {
			if (ISPRINT(sd->rev[i]))   /* printable character ? */
				*bl += sprintf(b + *bl, "%c", sd->rev[i]);
			else
				*bl += sprintf(b + *bl, " ");
		}
		*bl += sprintf(b + *bl, "\n");
	}
}

static void pscsi_bi_endio(struct bio *bio, int error)
{
	bio_put(bio);
}

static inline struct bio *pscsi_get_bio(struct pscsi_dev_virt *pdv, int sg_num)
{
	struct bio *bio;
	/*
	 * Use bio_malloc() following the comment in for bio -> struct request
	 * in block/blk-core.c:blk_make_request()
	 */
	bio = bio_kmalloc(GFP_KERNEL, sg_num);
	if (!(bio)) {
		printk(KERN_ERR "PSCSI: bio_kmalloc() failed\n");
		return NULL;
	}
	bio->bi_end_io = pscsi_bi_endio;

	return bio;
}

#if 0
#define DEBUG_PSCSI(x...) printk(x)
#else
#define DEBUG_PSCSI(x...)
#endif

/*      pscsi_map_task_SG():
 *
 *
 */
static int pscsi_map_task_SG(struct se_task *task)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) task->se_dev->dev_ptr;
	struct bio *bio = NULL, *hbio = NULL, *tbio = NULL;
	struct page *page;
	struct scatterlist *sg;
	u32 data_len = task->task_size, i, len, bytes, off;
	int nr_pages = (task->task_size + task->task_sg[0].offset +
			PAGE_SIZE - 1) >> PAGE_SHIFT;
	int nr_vecs = 0, ret = 0;
	int rw = (TASK_CMD(task)->data_direction == DMA_TO_DEVICE);

	if (!task->task_size)
		return 0;
	/*
	 * For SCF_SCSI_DATA_SG_IO_CDB, Use fs/bio.c:bio_add_page() to setup
	 * the bio_vec maplist from TC< struct se_mem -> task->task_sg ->
	 * struct scatterlist memory.  The struct se_task->task_sg[] currently needs
	 * to be attached to struct bios for submission to Linux/SCSI using
	 * struct request to struct scsi_device->request_queue.
	 *
	 * Note that this will be changing post v2.6.28 as Target_Core_Mod/pSCSI
	 * is ported to upstream SCSI passthrough functionality that accepts
	 * struct scatterlist->page_link or struct page as a paraemeter.
	 */
	DEBUG_PSCSI("PSCSI: nr_pages: %d\n", nr_pages);

	for_each_sg(task->task_sg, sg, task->task_sg_num, i) {
		page = sg_page(sg);
		off = sg->offset;
		len = sg->length;

		DEBUG_PSCSI("PSCSI: i: %d page: %p len: %d off: %d\n", i,
			page, len, off);

		while (len > 0 && data_len > 0) {
			bytes = min_t(unsigned int, len, PAGE_SIZE - off);
			bytes = min(bytes, data_len);

			if (!(bio)) {
				nr_vecs = min_t(int, BIO_MAX_PAGES, nr_pages);
				nr_pages -= nr_vecs;
				/*
				 * Calls bio_kmalloc() and sets bio->bi_end_io()
				 */
				bio = pscsi_get_bio(pdv, nr_vecs);
				if (!(bio))
					goto fail;

				if (rw)
					bio->bi_rw |= REQ_WRITE;

				DEBUG_PSCSI("PSCSI: Allocated bio: %p,"
					" dir: %s nr_vecs: %d\n", bio,
					(rw) ? "rw" : "r", nr_vecs);
				/*
				 * Set *hbio pointer to handle the case:
				 * nr_pages > BIO_MAX_PAGES, where additional
				 * bios need to be added to complete a given
				 * struct se_task
				 */
				if (!hbio)
					hbio = tbio = bio;
				else
					tbio = tbio->bi_next = bio;
			}

			DEBUG_PSCSI("PSCSI: Calling bio_add_pc_page() i: %d"
				" bio: %p page: %p len: %d off: %d\n", i, bio,
				page, len, off);

			ret = bio_add_pc_page(pdv->pdv_sd->request_queue,
					bio, page, bytes, off);
			if (ret != bytes)
				goto fail;

			DEBUG_PSCSI("PSCSI: bio->bi_vcnt: %d nr_vecs: %d\n",
				bio->bi_vcnt, nr_vecs);

			if (bio->bi_vcnt > nr_vecs) {
				DEBUG_PSCSI("PSCSI: Reached bio->bi_vcnt max:"
					" %d i: %d bio: %p, allocating another"
					" bio\n", bio->bi_vcnt, i, bio);
				/*
				 * Clear the pointer so that another bio will
				 * be allocated with pscsi_get_bio() above, the
				 * current bio has already been set *tbio and
				 * bio->bi_next.
				 */
				bio = NULL;
			}

			page++;
			len -= bytes;
			data_len -= bytes;
			off = 0;
		}
	}
	/*
	 * Starting with v2.6.31, call blk_make_request() passing in *hbio to
	 * allocate the pSCSI task a struct request.
	 */
	pt->pscsi_req = blk_make_request(pdv->pdv_sd->request_queue,
				hbio, GFP_KERNEL);
	if (!(pt->pscsi_req)) {
		printk(KERN_ERR "pSCSI: blk_make_request() failed\n");
		goto fail;
	}
	/*
	 * Setup the newly allocated struct request for REQ_TYPE_BLOCK_PC,
	 * and setup rq callback, CDB and sense.
	 */
	pscsi_blk_init_request(task, pt);

	return task->task_sg_num;
fail:
	while (hbio) {
		bio = hbio;
		hbio = hbio->bi_next;
		bio->bi_next = NULL;
		bio_endio(bio, 0);
	}
	return ret;
}

/*	pscsi_map_task_non_SG():
 *
 *
 */
static int pscsi_map_task_non_SG(struct se_task *task)
{
	struct se_cmd *cmd = TASK_CMD(task);
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) task->se_dev->dev_ptr;
	int ret = 0;

	if (!task->task_size)
		return 0;

	ret = blk_rq_map_kern(pdv->pdv_sd->request_queue,
			pt->pscsi_req, T_TASK(cmd)->t_task_buf,
			task->task_size, GFP_KERNEL);
	if (ret < 0) {
		printk(KERN_ERR "PSCSI: blk_rq_map_kern() failed: %d\n", ret);
		return PYX_TRANSPORT_LU_COMM_FAILURE;
	}
	return 0;
}

static int pscsi_CDB_none(struct se_task *task, u32 size)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;

	pt->pscsi_direction = DMA_NONE;

	return pscsi_blk_get_request(task);
}

/*	pscsi_CDB_read_non_SG():
 *
 *
 */
static int pscsi_CDB_read_non_SG(struct se_task *task, u32 size)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;

	pt->pscsi_direction = DMA_FROM_DEVICE;

	if (pscsi_blk_get_request(task) < 0)
		return PYX_TRANSPORT_LU_COMM_FAILURE;

	return pscsi_map_task_non_SG(task);
}

/*	pscsi_CDB_read_SG():
 *
 *
 */
static int pscsi_CDB_read_SG(struct se_task *task, u32 size)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;

	pt->pscsi_direction = DMA_FROM_DEVICE;
	/*
	 * pscsi_map_task_SG() calls block/blk-core.c:blk_make_request()
	 * for >= v2.6.31 pSCSI
	 */
	if (pscsi_map_task_SG(task) < 0)
		return PYX_TRANSPORT_LU_COMM_FAILURE;

	return task->task_sg_num;
}

/*	pscsi_CDB_write_non_SG():
 *
 *
 */
static int pscsi_CDB_write_non_SG(struct se_task *task, u32 size)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;

	pt->pscsi_direction = DMA_TO_DEVICE;

	if (pscsi_blk_get_request(task) < 0)
		return PYX_TRANSPORT_LU_COMM_FAILURE;

	return pscsi_map_task_non_SG(task);
}

/*	pscsi_CDB_write_SG():
 *
 *
 */
static int pscsi_CDB_write_SG(struct se_task *task, u32 size)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;

	pt->pscsi_direction = DMA_TO_DEVICE;
	/*
	 * pscsi_map_task_SG() calls block/blk-core.c:blk_make_request()
	 * for >= v2.6.31 pSCSI
	 */
	if (pscsi_map_task_SG(task) < 0)
		return PYX_TRANSPORT_LU_COMM_FAILURE;

	return task->task_sg_num;
}

/*	pscsi_check_lba():
 *
 *
 */
static int pscsi_check_lba(unsigned long long lba, struct se_device *dev)
{
	return 0;
}

/*	pscsi_check_for_SG():
 *
 *
 */
static int pscsi_check_for_SG(struct se_task *task)
{
	return task->task_sg_num;
}

/*	pscsi_get_cdb():
 *
 *
 */
static unsigned char *pscsi_get_cdb(struct se_task *task)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;

	return pt->pscsi_cdb;
}

/*	pscsi_get_sense_buffer():
 *
 *
 */
static unsigned char *pscsi_get_sense_buffer(struct se_task *task)
{
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *) task->transport_req;

	return (unsigned char *)&pt->pscsi_sense[0];
}

/*	pscsi_get_blocksize():
 *
 *
 */
static u32 pscsi_get_blocksize(struct se_device *dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return sd->sector_size;
}

/*	pscsi_get_device_rev():
 *
 *
 */
static u32 pscsi_get_device_rev(struct se_device *dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return (sd->scsi_level - 1) ? sd->scsi_level - 1 : 1;
}

/*	pscsi_get_device_type():
 *
 *
 */
static u32 pscsi_get_device_type(struct se_device *dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return sd->type;
}

/*	pscsi_get_dma_length():
 *
 *
 */
static u32 pscsi_get_dma_length(u32 task_size, struct se_device *dev)
{
	return PAGE_SIZE;
}

/*	pscsi_get_max_sectors():
 *
 *
 */
static u32 pscsi_get_max_sectors(struct se_device *dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return (sd->host->max_sectors > sd->request_queue->limits.max_sectors) ?
		sd->request_queue->limits.max_sectors : sd->host->max_sectors;
}

/*	pscsi_get_queue_depth():
 *
 *
 */
static u32 pscsi_get_queue_depth(struct se_device *dev)
{
	struct pscsi_dev_virt *pdv = (struct pscsi_dev_virt *) dev->dev_ptr;
	struct scsi_device *sd = (struct scsi_device *) pdv->pdv_sd;

	return sd->queue_depth;
}

/*	pscsi_handle_SAM_STATUS_failures():
 *
 *
 */
static inline void pscsi_process_SAM_status(
	struct se_task *task,
	struct pscsi_plugin_task *pt)
{
	task->task_scsi_status = status_byte(pt->pscsi_result);
	if ((task->task_scsi_status)) {
		task->task_scsi_status <<= 1;
		printk(KERN_INFO "PSCSI Status Byte exception at task: %p CDB:"
			" 0x%02x Result: 0x%08x\n", task, pt->pscsi_cdb[0],
			pt->pscsi_result);
	}

	switch (host_byte(pt->pscsi_result)) {
	case DID_OK:
		transport_complete_task(task, (!task->task_scsi_status));
		break;
	default:
		printk(KERN_INFO "PSCSI Host Byte exception at task: %p CDB:"
			" 0x%02x Result: 0x%08x\n", task, pt->pscsi_cdb[0],
			pt->pscsi_result);
		task->task_scsi_status = SAM_STAT_CHECK_CONDITION;
		task->task_error_status = PYX_TRANSPORT_UNKNOWN_SAM_OPCODE;
		TASK_CMD(task)->transport_error_status =
					PYX_TRANSPORT_UNKNOWN_SAM_OPCODE;
		transport_complete_task(task, 0);
		break;
	}

	return;
}

static void pscsi_req_done(struct request *req, int uptodate)
{
	struct se_task *task = (struct se_task *)req->end_io_data;
	struct pscsi_plugin_task *pt = (struct pscsi_plugin_task *)task->transport_req;

	pt->pscsi_result = req->errors;
	pt->pscsi_resid = req->resid_len;

	pscsi_process_SAM_status(task, pt);
	__blk_put_request(req->q, req);
	pt->pscsi_req = NULL;
}

static struct se_subsystem_api pscsi_template = {
	.name			= "pscsi",
	.type			= PSCSI,
	.transport_type		= TRANSPORT_PLUGIN_PHBA_PDEV,
	.external_submod	= 1,
	.cdb_none		= pscsi_CDB_none,
	.cdb_read_non_SG	= pscsi_CDB_read_non_SG,
	.cdb_read_SG		= pscsi_CDB_read_SG,
	.cdb_write_non_SG	= pscsi_CDB_write_non_SG,
	.cdb_write_SG		= pscsi_CDB_write_SG,
	.attach_hba		= pscsi_attach_hba,
	.detach_hba		= pscsi_detach_hba,
	.pmode_enable_hba	= pscsi_pmode_enable_hba,
	.activate_device	= pscsi_activate_device,
	.deactivate_device	= pscsi_deactivate_device,
	.allocate_virtdevice	= pscsi_allocate_virtdevice,
	.create_virtdevice	= pscsi_create_virtdevice,
	.free_device		= pscsi_free_device,
	.transport_complete	= pscsi_transport_complete,
	.allocate_request	= pscsi_allocate_request,
	.do_task		= pscsi_do_task,
	.free_task		= pscsi_free_task,
	.check_configfs_dev_params = pscsi_check_configfs_dev_params,
	.set_configfs_dev_params = pscsi_set_configfs_dev_params,
	.show_configfs_dev_params = pscsi_show_configfs_dev_params,
	.create_virtdevice_from_fd = NULL,
	.get_plugin_info	= pscsi_get_plugin_info,
	.get_hba_info		= pscsi_get_hba_info,
	.get_dev_info		= pscsi_get_dev_info,
	.check_lba		= pscsi_check_lba,
	.check_for_SG		= pscsi_check_for_SG,
	.get_cdb		= pscsi_get_cdb,
	.get_sense_buffer	= pscsi_get_sense_buffer,
	.get_blocksize		= pscsi_get_blocksize,
	.get_device_rev		= pscsi_get_device_rev,
	.get_device_type	= pscsi_get_device_type,
	.get_dma_length		= pscsi_get_dma_length,
	.get_max_sectors	= pscsi_get_max_sectors,
	.get_queue_depth	= pscsi_get_queue_depth,
	.write_pending		= NULL,
};

int __init pscsi_module_init(void)
{
	int ret;

	INIT_LIST_HEAD(&pscsi_template.sub_api_list);

	ret = transport_subsystem_register(&pscsi_template, THIS_MODULE);
	if (ret < 0)
		return ret;

	return 0;
}

void pscsi_module_exit(void)
{
	transport_subsystem_release(&pscsi_template);
}

MODULE_DESCRIPTION("TCM PSCSI subsystem plugin");
MODULE_AUTHOR("nab@Linux-iSCSI.org");
MODULE_LICENSE("GPL");

module_init(pscsi_module_init);
module_exit(pscsi_module_exit);
