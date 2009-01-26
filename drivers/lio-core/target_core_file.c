/*********************************************************************************
 * Filename:  target_core_file.c
 *
 * This file contains the Storage Engine <-> FILEIO transport specific functions.
 *
 * Copyright (c) 2005 PyX Technologies, Inc.
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


#define TARGET_CORE_FILE_C

#include <linux/version.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>

#include <iscsi_linux_os.h> 
#include <iscsi_linux_defs.h>
                                
#include <iscsi_debug.h>
#include <iscsi_protocol.h>
#include <iscsi_target_core.h>
#include <target_core_base.h>
#include <iscsi_target_device.h>
#include <target_core_transport.h>
#include <iscsi_target_util.h>
#include <target_core_file.h>
#include <iscsi_target_error.h>

#undef TARGET_CORE_FILE_C
 
extern se_global_t *se_global;
extern struct block_device *__linux_blockdevice_claim (int, int, void *, int *);
extern struct block_device *linux_blockdevice_claim(int, int, void *);
extern int linux_blockdevice_release(int, int, struct block_device *);
extern int linux_blockdevice_check(int, int);

/*	fd_attach_hba(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int fd_attach_hba (se_hba_t *hba, u32 host_id)
{
	fd_host_t *fd_host;

	if (!(fd_host = kzalloc(sizeof(fd_host_t), GFP_KERNEL))) {
		TRACE_ERROR("Unable to allocate memory for fd_host_t\n");
		return(-1);
	}

	fd_host->fd_host_id = host_id;
	
	atomic_set(&hba->left_queue_depth, FD_HBA_QUEUE_DEPTH);
	atomic_set(&hba->max_queue_depth, FD_HBA_QUEUE_DEPTH);
	hba->hba_ptr = (void *) fd_host;
	hba->transport = &fileio_template;

	PYXPRINT("CORE_HBA[%d] - %s FILEIO HBA Driver %s on"
		" Generic Target Core Stack %s\n", hba->hba_id, PYX_ISCSI_VENDOR,
		FD_VERSION, PYX_ISCSI_VERSION);
	PYXPRINT("CORE_HBA[%d] - Attached FILEIO HBA: %u to Generic Target Core with"
		" TCQ Depth: %d MaxSectors: %u\n", hba->hba_id, fd_host->fd_host_id,
		atomic_read(&hba->max_queue_depth), FD_MAX_SECTORS);

	return(0);
}

/*	fd_detach_hba(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int fd_detach_hba (se_hba_t *hba)
{
	fd_host_t *fd_host;
	
	if (!hba->hba_ptr) {
		TRACE_ERROR("hba->hba_ptr is NULL!\n");
		return(-1);
	}
	fd_host = (fd_host_t *) hba->hba_ptr;

	PYXPRINT("CORE_HBA[%d] - Detached FILEIO HBA: %u from Generic Target Core\n",
			hba->hba_id, fd_host->fd_host_id);

	kfree(fd_host);
	hba->hba_ptr = NULL;
	
	return(0);
}

extern int fd_claim_phydevice (se_hba_t *hba, se_device_t *dev)
{
	fd_dev_t *fd_dev = (fd_dev_t *)dev->dev_ptr;
	struct block_device *bd;

	if (!fd_dev->fd_claim_bd)
		return(0);
	
	if (dev->dev_flags & DF_READ_ONLY) {
		PYXPRINT("FILEIO: Using previously claimed %p Major:Minor - %d:%d\n",
		fd_dev->fd_bd, fd_dev->fd_major, fd_dev->fd_minor);
	} else {
		PYXPRINT("FILEIO: Claiming %p Major:Minor - %d:%d\n", fd_dev,
			fd_dev->fd_major, fd_dev->fd_minor);

		if (!(bd = linux_blockdevice_claim(fd_dev->fd_major, fd_dev->fd_minor,
				(void *)fd_dev)))
			return(-1);

		fd_dev->fd_bd = bd;
		fd_dev->fd_bd->bd_contains = bd;
	}

	return(0);
}

extern int fd_release_phydevice (se_device_t *dev)
{
	fd_dev_t *fd_dev = (fd_dev_t *)dev->dev_ptr;

	if (!fd_dev->fd_claim_bd)
		return(0);

	if (!fd_dev->fd_bd)
		return(0);
	
	if (dev->dev_flags & DF_READ_ONLY) {
		PYXPRINT("FILEIO: Calling blkdev_put() for Major:Minor - %d:%d\n",
			fd_dev->fd_major, fd_dev->fd_minor);
		blkdev_put((struct block_device *)fd_dev->fd_bd, FMODE_READ);
	} else {	
		PYXPRINT("FILEIO: Releasing Major:Minor - %d:%d\n", fd_dev->fd_major,
			fd_dev->fd_minor);
		linux_blockdevice_release(fd_dev->fd_major, fd_dev->fd_minor,
			(struct block_device *)fd_dev->fd_bd);
	}
	
	fd_dev->fd_bd = NULL;
	
	return(0);
}

extern void *fd_allocate_virtdevice (se_hba_t *hba, const char *name)
{
	fd_dev_t *fd_dev;
	fd_host_t *fd_host = (fd_host_t *) hba->hba_ptr;

	if (!(fd_dev = (fd_dev_t *) kzalloc(sizeof(fd_dev_t), GFP_KERNEL))) {
		TRACE_ERROR("Unable to allocate memory for fd_dev_t\n");
		return(NULL);
	}

	fd_dev->fd_host = fd_host;
	
	printk("FILEIO: Allocated fd_dev for %p\n", name);

	return(fd_dev);
}

/*	fd_create_virtdevice(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern se_device_t *fd_create_virtdevice (
	se_hba_t *hba,
	se_subsystem_dev_t *se_dev,
	void *p)
{
	char *dev_p = NULL;
	se_device_t *dev;
	fd_dev_t *fd_dev = (fd_dev_t *) p;
	fd_host_t *fd_host = (fd_host_t *) hba->hba_ptr;
	mm_segment_t old_fs;
	struct block_device *bd = NULL;
	struct file *file;
	int dev_flags = 0, flags, ret = 0;
#if 0
	if (strlen(di->fd_dev_name) > FD_MAX_DEV_NAME) {
		TRACE_ERROR("di->fd_dev_name exceeds FD_MAX_DEV_NAME: %d\n",
				FD_MAX_DEV_NAME);
		return(0);
	}

	fd_dev->fd_dev_id = di->fd_device_id;
	fd_dev->fd_host = fd_host;
	fd_dev->fd_dev_size = di->fd_device_size;
	fd_dev->fd_claim_bd = di->fd_claim_bd;
	sprintf(fd_dev->fd_dev_name, "%s", di->fd_dev_name);
#endif
	
	old_fs = get_fs();
	set_fs(get_ds());
	dev_p = getname(fd_dev->fd_dev_name);	
	set_fs(old_fs);

	if (IS_ERR(dev_p)) {
		TRACE_ERROR("getname(%s) failed: %lu\n", fd_dev->fd_dev_name,
				IS_ERR(dev_p));
		goto fail;
	}
#if 0	
	if (di->no_create_file)
		flags = O_RDWR | O_LARGEFILE;
	else
		flags = O_RDWR | O_CREAT | O_LARGEFILE;
#else
	flags = O_RDWR | O_CREAT | O_LARGEFILE;
#endif
//	flags |= O_DIRECT;
	
	file = filp_open(dev_p, flags, 0600);

	if (IS_ERR(file) || !file || !file->f_dentry) {
		TRACE_ERROR("filp_open(%s) failed\n", dev_p);
		goto fail;
	}

	fd_dev->fd_file = file;

	/*
	 * If we are claiming a blockend for this struct file, we extract fd_dev->fd_size
	 * from struct block_device.
	 *
	 * Otherwise, we use the passed fd_size= from target-ctl.
	 */
#warning FIXME: Complete fd_dev->fd_claim_bd = 1 support
#if 0
	if (fd_dev->fd_claim_bd) {
		fd_dev->fd_major = di->iblock_major;
		fd_dev->fd_minor = di->iblock_minor;
		
		PYXPRINT("FILEIO: Claiming %p Major:Minor - %d:%d\n", fd_dev,
			fd_dev->fd_major, fd_dev->fd_minor);

		if ((bd = __linux_blockdevice_claim(fd_dev->fd_major, fd_dev->fd_minor,
				(void *)fd_dev, &ret)))
			if (ret == 1)
				dev_flags |= DF_CLAIMED_BLOCKDEV;
			else if (di->force) {
				dev_flags |= DF_READ_ONLY;	
				PYXPRINT("FILEIO: DF_READ_ONLY for Major:Minor - %d:%d\n",
					di->iblock_major, di->iblock_minor);
			} else {	
				TRACE_ERROR("WARNING: Unable to claim block device. Only use"
					" force=1 for READ-ONLY access.\n");
				goto fail;
			}
		else
			goto fail;

		fd_dev->fd_bd = bd;
		if (dev_flags & DF_CLAIMED_BLOCKDEV)
			fd_dev->fd_bd->bd_contains = bd;

		/*
		 * Determine the number of bytes for this FILEIO device from struct block_device.
		 */
		fd_dev->fd_dev_size = ((unsigned long long)bd->bd_disk->capacity * 512);
#if 0	
		TRACE_ERROR("FILEIO: Using fd_dev_size %llu from struct block_device\n",
				fd_dev->fd_dev_size);
#endif
	}
#endif
	dev_flags |= DF_DISABLE_STATUS_THREAD;
	
	if (!(dev = transport_add_device_to_core_hba(hba, &fileio_template,
				se_dev, dev_flags, (void *)fd_dev)))
		goto fail;

	fd_dev->fd_dev_id = fd_host->fd_host_dev_id_count++;
	fd_dev->fd_queue_depth = dev->queue_depth;
	
	PYXPRINT("CORE_FILE[%u] - Added LIO FILEIO Device ID: %u at %s,"
		" %llu total bytes\n", fd_host->fd_host_id, fd_dev->fd_dev_id,
			fd_dev->fd_dev_name, fd_dev->fd_dev_size);
	
	putname(dev_p);
	
	return(dev);
fail:
	if (fd_dev->fd_file) {
		filp_close(fd_dev->fd_file, NULL);
		fd_dev->fd_file = NULL;
	}
	putname(dev_p);
	kfree(fd_dev);
	return(NULL);
}

/*	fd_activate_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int fd_activate_device (se_device_t *dev)
{
	fd_dev_t *fd_dev = (fd_dev_t *) dev->dev_ptr;
	fd_host_t *fd_host = fd_dev->fd_host;
	
	PYXPRINT("CORE_FILE[%u] - Activating Device with TCQ: %d at FILEIO"
		" Device ID: %d\n", fd_host->fd_host_id, fd_dev->fd_queue_depth,
		fd_dev->fd_dev_id);

	return(0);
}

/*	fd_deactivate_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void fd_deactivate_device (se_device_t *dev)
{
	fd_dev_t *fd_dev = (fd_dev_t *) dev->dev_ptr;
	fd_host_t *fd_host = fd_dev->fd_host;

	PYXPRINT("CORE_FILE[%u] - Deactivating Device with TCQ: %d at FILEIO"
		" Device ID: %d\n", fd_host->fd_host_id, fd_dev->fd_queue_depth,
		fd_dev->fd_dev_id);

	return;
}

/*	fd_free_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void fd_free_device (void *p)
{
	fd_dev_t *fd_dev = (fd_dev_t *) p;

	if (fd_dev->fd_file) {
		filp_close(fd_dev->fd_file, NULL);
		fd_dev->fd_file = NULL;
	}

	kfree(fd_dev);
	return;
}

/*	fd_transport_complete(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int fd_transport_complete (se_task_t *task)
{
	return(0);
}

/*	fd_allocate_request(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void *fd_allocate_request (
	se_task_t *task,
	se_device_t *dev)
{
	fd_request_t *fd_req;
	
	if (!(fd_req = (fd_request_t *) kmalloc(sizeof(fd_request_t), GFP_KERNEL))) {
		TRACE_ERROR("Unable to allocate fd_request_t\n");
		return(NULL);
	}
	memset(fd_req, 0, sizeof(fd_request_t));

	fd_req->fd_dev = (fd_dev_t *) dev->dev_ptr;

	return((void *)fd_req);
}

extern void fd_get_evpd_prod (unsigned char *buf, u32 size, se_device_t *dev)
{
	snprintf(buf, size, "FILEIO");
	return;
}

extern void fd_get_evpd_sn (unsigned char *buf, u32 size, se_device_t *dev)
{
	fd_dev_t *fdev = (fd_dev_t *) dev->dev_ptr;
	se_hba_t *hba = dev->se_hba;

	snprintf(buf, size, "%u_%u", hba->hba_id, fdev->fd_dev_id);	
	return;
}

/*	fd_emulate_inquiry():
 *
 *
 */
extern int fd_emulate_inquiry (se_task_t *task)
{
	unsigned char prod[64], se_location[128];
	se_cmd_t *cmd = TASK_CMD(task);
	fd_dev_t *fdev = (fd_dev_t *) task->se_dev->dev_ptr;
	se_hba_t *hba = task->se_dev->se_hba;
	
	memset(prod, 0, 64);
	memset(se_location, 0, 128);
	
	sprintf(prod, "FILEIO");
	sprintf(se_location, "%u_%u", hba->hba_id, fdev->fd_dev_id);
		
	return(transport_generic_emulate_inquiry(cmd, TYPE_DISK, prod, FD_VERSION,
		se_location));
}

/*	fd_emulate_read_cap():
 *
 *
 */
static int fd_emulate_read_cap (se_task_t *task)
{
	fd_dev_t *fd_dev = (fd_dev_t *) task->se_dev->dev_ptr;
	u32 blocks = (fd_dev->fd_dev_size / FD_BLOCKSIZE);
	
	if ((fd_dev->fd_dev_size / FD_BLOCKSIZE) >= 0x00000000ffffffff)
		blocks = 0xffffffff;
	
	return(transport_generic_emulate_readcapacity(TASK_CMD(task), blocks, FD_BLOCKSIZE));
}

static int fd_emulate_read_cap16 (se_task_t *task)
{
	fd_dev_t *fd_dev = (fd_dev_t *) task->se_dev->dev_ptr;
	unsigned long long blocks_long = (fd_dev->fd_dev_size / FD_BLOCKSIZE);
	
	return(transport_generic_emulate_readcapacity_16(TASK_CMD(task), blocks_long, FD_BLOCKSIZE));
}

/*	fd_emulate_scsi_cdb():
 *
 *
 */
static int fd_emulate_scsi_cdb (se_task_t *task)
{
	int ret;
	se_cmd_t *cmd = TASK_CMD(task);
	fd_request_t *fd_req = (fd_request_t *) task->transport_req;

	switch (fd_req->fd_scsi_cdb[0]) {
	case INQUIRY:
		if (fd_emulate_inquiry(task) < 0)
			return(PYX_TRANSPORT_INVALID_CDB_FIELD);
		break;
	case READ_CAPACITY:
		if ((ret = fd_emulate_read_cap(task)) < 0)
			return(ret);
		break;
	case MODE_SENSE:
		if ((ret = transport_generic_emulate_modesense(TASK_CMD(task),
				fd_req->fd_scsi_cdb, fd_req->fd_buf, 0, TYPE_DISK)) < 0)
			return(ret);
		break;
	case MODE_SENSE_10:
		if ((ret = transport_generic_emulate_modesense(TASK_CMD(task),
				fd_req->fd_scsi_cdb, fd_req->fd_buf, 1, TYPE_DISK)) < 0)
			return(ret);
		break;
	case SERVICE_ACTION_IN:
		if ((T_TASK(cmd)->t_task_cdb[1] & 0x1f) != SAI_READ_CAPACITY_16) {
			TRACE_ERROR("Unsupported SA: 0x%02x\n", T_TASK(cmd)->t_task_cdb[1] & 0x1f);
			return(PYX_TRANSPORT_UNKNOWN_SAM_OPCODE);
		}
		if ((ret = fd_emulate_read_cap16(task)) < 0)
			return(ret);
		break;
	case ALLOW_MEDIUM_REMOVAL:
	case ERASE:
	case LOAD_UNLOAD_MEDIUM:
	case REZERO_UNIT:
	case SEEK_10:
	case SPACE:
	case START_STOP:
	case SYNCHRONIZE_CACHE:
	case TEST_UNIT_READY:
	case VERIFY:
	case WRITE_FILEMARKS:
	case RESERVE:
	case RESERVE_10:
	case RELEASE:
	case RELEASE_10:
		break;
	default:
		TRACE_ERROR("Unsupported SCSI Opcode: 0x%02x for FILEIO\n",
				fd_req->fd_scsi_cdb[0]);
		return(PYX_TRANSPORT_UNKNOWN_SAM_OPCODE);
	}

	task->task_scsi_status = GOOD;
	transport_complete_task(task, 1);
	
	return(PYX_TRANSPORT_SENT_TO_TRANSPORT);
}

static inline int fd_iovec_alloc (fd_request_t *req)
{
	if (!(req->fd_iovs = kmalloc(sizeof(struct iovec) * req->fd_sg_count, GFP_KERNEL))) {
		TRACE_ERROR("Unable to allocate req->fd_iovs\n");
		return(-1);
	}
	memset(req->fd_iovs, 0, sizeof(struct iovec) * req->fd_sg_count);

	return(0);
}

static inline int fd_seek (struct file *fd, unsigned long long lba)
{
	mm_segment_t old_fs;
	unsigned long long offset;
	
	old_fs = get_fs();
	set_fs(get_ds());
	if (fd->f_op->llseek)
		offset = fd->f_op->llseek(fd, lba * FD_BLOCKSIZE, 0);
	else
		offset = default_llseek(fd, lba * FD_BLOCKSIZE, 0);
	set_fs(old_fs);
#if 0
	PYXPRINT("lba: %llu : FD_BLOCKSIZE: %d\n", lba, FD_BLOCKSIZE);
	PYXPRINT("offset from llseek: %llu\n", offset);
	PYXPRINT("(lba * FD_BLOCKSIZE): %llu\n", (lba * FD_BLOCKSIZE));
#endif	
	if (offset != (lba * FD_BLOCKSIZE)) {
		TRACE_ERROR("offset: %llu not equal to LBA: %llu\n",
			offset, (lba * FD_BLOCKSIZE));
		return(-1);
	}

	return(0);
}

static int fd_do_readv (fd_request_t *req, se_task_t *task)
{
	int ret = 0;
	u32 i;
	mm_segment_t old_fs;
	struct file *fd = req->fd_dev->fd_file;
	struct scatterlist *sg = task->task_sg;
	struct iovec iov[req->fd_sg_count];	

	memset(iov, 0, sizeof(struct iovec) + req->fd_sg_count);

	if (fd_seek(fd, req->fd_lba) < 0)
		return(-1);

	for (i = 0; i < req->fd_sg_count; i++) {
		iov[i].iov_len = sg[i].length;
		iov[i].iov_base = sg_virt(&sg[i]); 
	}

	old_fs = get_fs();
	set_fs(get_ds());
	ret = vfs_readv(fd, &iov[0], req->fd_sg_count, &fd->f_pos);
	set_fs(old_fs);

	if (ret < 0) {
		TRACE_ERROR("vfs_readv() returned %d\n", ret);
		return(-1);
	}

	return(1);
}

#if 0

static void fd_aio_intr (struct kiocb *kcb)
{
	se_task_t *task = (se_task_t *)kcb->private;

	printk("Got AIO_READ Response: task: %p\n", task);

	task->task_scsi_status = GOOD;
	transport_complete_task(task, 1);
	return;
}

static ssize_t fd_aio_retry (struct kiocb *kcb)
{
	TRACE_ERROR("fd_aio_retry() called for %p\n", kcb);
	return(0);
}

static int fd_do_aio_read (fd_request_t *req, se_task_t *task)
{
	int ret = 0;
	u32 i, length = 0;
	unsigned long long offset, lba = req->fd_lba;;
	mm_segment_t old_fs;
	struct file *fd = req->fd_dev->fd_file;
	struct scatterlist *sg = task->task_sg;
	struct iovec *iov;
	struct kiocb	*iocb;

	if (fd_iovec_alloc(req) < 0)
		return(-1);

       old_fs = get_fs();
	set_fs(get_ds());
         if (fd->f_op->llseek)
             offset = fd->f_op->llseek(fd, lba * FD_BLOCKSIZE, 0);
              else
         offset = default_llseek(fd, lba * FD_BLOCKSIZE, 0);
        set_fs(old_fs);

        PYXPRINT("lba: %llu : FD_BLOCKSIZE: %d\n", lba, FD_BLOCKSIZE);
        PYXPRINT("offset from llseek: %llu\n", offset);
        PYXPRINT("(lba * FD_BLOCKSIZE): %llu\n", (lba * FD_BLOCKSIZE));

       if (offset != (lba * FD_BLOCKSIZE)) {
                TRACE_ERROR("offset: %llu not equal to LBA: %llu\n",
                        offset, (lba * FD_BLOCKSIZE));
                return(-1);
        }


	TRACE_ERROR("req->fd_lba: %llu\n", req->fd_lba);
	
	for (i = 0; i < req->fd_sg_count; i++) {
		iov = &req->fd_iovs[i];
		TRACE_ERROR("sg->length: %d sg->page: %p\n", sg[i].length, sg[i].page);
		length += sg[i].length;
		iov->iov_len = sg[i].length;
		iov->iov_base = sg_virt(&sg[i]);
		TRACE_ERROR("iov_iov_len: %d iov_iov_base: %p\n", iov->iov_len, iov->iov_base);
	}
	
	init_sync_kiocb(&req->fd_iocb, fd);
	req->fd_iocb.ki_opcode = IOCB_CMD_PREAD;
	req->fd_iocb.ki_nbytes = length;
	req->fd_iocb.private = (void *) task;
	req->fd_iocb.ki_dtor = &fd_aio_intr;
	req->fd_iocb.ki_retry = &fd_aio_retry;
	
	PYXPRINT("Launching AIO_READ: %p iovecs: %p total length: %u\n",
		&req->fd_iocb, &req->fd_iovs[0], length);

	PYXPRINT("fd->f_pos: %d\n", fd->f_pos);
	PYXPRINT("req->fd_iocb.ki_pos: %d\n", req->fd_iocb.ki_pos);

	old_fs = get_fs();
	set_fs(get_ds());
	ret = __generic_file_aio_read(&req->fd_iocb, &req->fd_iovs[0], req->fd_sg_count, &fd->f_pos);
	set_fs(old_fs);

	PYXPRINT("__generic_file_aio_read() returned %d\n", ret);

	if (ret <= 0) 
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);
	
	if (ret != length) {
		TRACE_ERROR("ret [%d] != LENGTH [%d]\n", ret, length);
	}

	return(1);
}

extern void fd_sendfile_free_DMA (se_cmd_t *cmd)
{
	TRACE_ERROR("Release reference to pages now..\n");
	return;
}

static int fd_sendactor (read_descriptor_t * desc, struct page *page, unsigned long offset, unsigned long size)
{
	unsigned long count = desc->count;
	se_task_t *task = desc->arg.data;
	fd_request_t *req = (fd_request_t *) task->transport_req;	
	struct scatterlist *sg = task->task_sg;

//	PYXPRINT("page: %p offset: %lu size: %lu\n", page, offset, size);

	__free_page(sg[req->fd_cur_offset].page);

//	TRACE_ERROR("page_address(page): %p\n", page_address(page));
	sg[req->fd_cur_offset].page = page;
	sg[req->fd_cur_offset].offset = offset;
	sg[req->fd_cur_offset].length = size;

//	PYXPRINT("sg[%d:%p].page %p length: %d\n", req->fd_cur_offset, &sg[req->fd_cur_offset],
//		sg[req->fd_cur_offset].page, sg[req->fd_cur_offset].length);

	req->fd_cur_size += size;
//	TRACE_ERROR("fd_cur_size: %u\n", req->fd_cur_size);
	
	req->fd_cur_offset++;

	desc->count--;
	desc->written += size;
	return(size);
}

static int fd_do_sendfile (fd_request_t *req, se_task_t *task)
{
	int ret = 0;
	struct file *fd = req->fd_dev->fd_file;

	if (fd_seek(fd, req->fd_lba) < 0)
		return(-1);

	TASK_CMD(task)->transport_free_DMA = &fd_sendfile_free_DMA;
	
	ret = fd->f_op->sendfile(fd, &fd->f_pos, req->fd_sg_count, fd_sendactor, (void *)task);	

	if (ret < 0) {
		TRACE_ERROR("fd->f_op->sendfile() returned %d\n", ret);
		return(-1);
	}

	return(1);
}

#endif

static int fd_do_writev (fd_request_t *req, se_task_t *task)
{
	int ret = 0;
	u32 i;
	struct file *fd = req->fd_dev->fd_file;
	struct scatterlist *sg = task->task_sg;
	mm_segment_t old_fs;
	struct iovec iov[req->fd_sg_count];

	memset(iov, 0, sizeof(struct iovec) + req->fd_sg_count);
	
	if (fd_seek(fd, req->fd_lba) < 0)
		return(-1);
	
	for (i = 0; i < req->fd_sg_count; i++) {
		iov[i].iov_len = sg[i].length;
		iov[i].iov_base = sg_virt(&sg[i]);
	}

	old_fs = get_fs();
	set_fs(get_ds());
	ret = vfs_writev(fd, &iov[0], req->fd_sg_count, &fd->f_pos);
	set_fs(old_fs);

	if (ret < 0) {
		TRACE_ERROR("vfs_writev() returned %d\n", ret);
		return(-1);
	}

	return(1);
}

#if 0

static int fd_do_aio_write (fd_request_t *req, se_task_t *task)
{
	int ret = 0;
	u32 i, length = 0;
	unsigned long long offset, lba = req->fd_lba;
	mm_segment_t old_fs;
	struct file *fd = req->fd_dev->fd_file;
	struct scatterlist *sg = task->task_sg;
	struct iovec *iov;
	struct kiocb    *iocb;

	if (fd_iovec_alloc(req) < 0)
		return(-1);

	old_fs = get_fs();
	set_fs(get_ds());
	if (fd->f_op->llseek)
		offset = fd->f_op->llseek(fd, lba * FD_BLOCKSIZE, 0);
	else
		offset = default_llseek(fd, lba * FD_BLOCKSIZE, 0);
	set_fs(old_fs);

	PYXPRINT("lba: %llu : FD_BLOCKSIZE: %d\n", lba, FD_BLOCKSIZE);
	PYXPRINT("offset from llseek: %llu\n", offset);
	PYXPRINT("(lba * FD_BLOCKSIZE): %llu\n", (lba * FD_BLOCKSIZE));

	if (offset != (lba * FD_BLOCKSIZE)) {
		TRACE_ERROR("offset: %llu not equal to LBA: %llu\n",
			offset, (lba * FD_BLOCKSIZE));
		return(-1);
	}

	for (i = 0; i < req->fd_sg_count; i++) {
		iov = &req->fd_iovs[i];
		TRACE_ERROR("sg->length: %d sg->page: %p\n", sg[i].length, sg[i].page);

		length += sg[i].length;
		iov->iov_len = sg[i].length;
		iov->iov_base = sg_virt(&sg[i]);
		TRACE_ERROR("iov_iov_len: %d iov_iov_base: %p\n", iov->iov_len, iov->iov_base);
	}

	init_sync_kiocb(&req->fd_iocb, fd);
	req->fd_iocb.ki_opcode = IOCB_CMD_PWRITE;
	req->fd_iocb.ki_nbytes = length;
	req->fd_iocb.private = (void *) task;
	req->fd_iocb.ki_dtor = &fd_aio_intr;
	req->fd_iocb.ki_retry = &fd_aio_retry;

	PYXPRINT("Launching AIO_WRITE: %p iovecs: %p total length: %u\n",
		&req->fd_iocb, &req->fd_iovs[0], length);

	PYXPRINT("fd->f_pos: %d\n", fd->f_pos);
	PYXPRINT("req->fd_iocb.ki_pos: %d\n", req->fd_iocb.ki_pos);

	old_fs = get_fs();
	set_fs(get_ds());
	ret = generic_file_aio_write_nolock(&req->fd_iocb, &req->fd_iovs[0], req->fd_sg_count, &fd->f_pos);
	set_fs(old_fs);

	PYXPRINT("generic_file_aio_write_nolock() returned %d\n", ret);

	if (ret <= 0)
		return(PYX_TRANSPORT_LOGICAL_UNIT_COMMUNICATION_FAILURE);

	if (ret != length) {
		TRACE_ERROR("ret [%d] != WRITE LENGTH [%d]\n", ret, length);
	}

	return(1);
}

#endif

extern int fd_do_task (se_task_t *task)
{
	int ret = 0;
	fd_request_t *req = (fd_request_t *) task->transport_req;

	if (!(TASK_CMD(task)->se_cmd_flags & SCF_SCSI_DATA_SG_IO_CDB))
		return(fd_emulate_scsi_cdb(task));

	req->fd_lba = task->task_lba;
	req->fd_size = task->task_size;

	if (req->fd_data_direction == FD_DATA_READ) {
//		ret = fd_do_aio_read(req, task);
//		ret = fd_do_sendfile(req, task);
		ret = fd_do_readv(req, task);
	} else {
//		ret = fd_do_aio_write(req, task);
		ret = fd_do_writev(req, task);
	}

	if (ret < 0)       
		return(ret);

	if (ret) {
		task->task_scsi_status = GOOD;   
		transport_complete_task(task, 1);
	}	

	return(PYX_TRANSPORT_SENT_TO_TRANSPORT);
}

/*	fd_free_task(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern void fd_free_task (se_task_t *task)
{
	fd_request_t *req;

	req = (fd_request_t *) task->transport_req;
	kfree(req->fd_iovs);
	
	kfree(req);
	
	return;
}

extern ssize_t fd_set_configfs_dev_params (se_hba_t *hba,
					       se_subsystem_dev_t *se_dev,
					       const char *page, ssize_t count)
{
	fd_dev_t *fd_dev = (fd_dev_t *) se_dev->se_dev_su_ptr;
	char *buf, *cur, *ptr, *ptr2, *endptr;
	int params = 0;

	if (!(buf = kzalloc(count, GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate memory for temporary buffer\n");
		return(0);
	}
	memcpy(buf, page, count);
	cur = buf;

#warning FIXME: Finish major/minor for real backend blockdevice
	while (cur) {
		if (!(ptr = strstr(cur, "=")))
			goto out;

		*ptr = '\0';
		ptr++;

		if ((ptr2 = strstr(cur, "fd_dev_name"))) {
			transport_check_dev_params_delim(ptr, &cur);
			ptr = strstrip(ptr);
			snprintf(fd_dev->fd_dev_name, FD_MAX_DEV_NAME, "%s", ptr);
			PYXPRINT("FILEIO: Referencing Path: %s\n", fd_dev->fd_dev_name);
			fd_dev->fbd_flags |= FBDF_HAS_PATH;
			params++;
		} else if ((ptr2 = strstr(cur, "fd_dev_size"))) {
			transport_check_dev_params_delim(ptr, &cur);
			fd_dev->fd_dev_size = simple_strtoull(ptr, &endptr, 0);
			PYXPRINT("FILEIO: Referencing Size: %llu\n", fd_dev->fd_dev_size);
			fd_dev->fbd_flags |= FBDF_HAS_SIZE;
			params++;
		} else
			cur = NULL;
	}

out:
	kfree(buf);
	return((params) ? count : -EINVAL);
}

#warning FIXME: Finish major/minor for real backend blockdevice
extern ssize_t fd_check_configfs_dev_params (se_hba_t *hba, se_subsystem_dev_t *se_dev)
{
	fd_dev_t *fd_dev = (fd_dev_t *) se_dev->se_dev_su_ptr;

	if (!(fd_dev->fbd_flags & FBDF_HAS_PATH) ||
	    !(fd_dev->fbd_flags & FBDF_HAS_SIZE)) {
		TRACE_ERROR("Missing fd_dev_name= and fd_dev_size=\n");
		return(-1);
	}
	
	return(0);
}

extern void __fd_get_dev_info (fd_dev_t *, char *, int *);

extern ssize_t fd_show_configfs_dev_params (se_hba_t *hba, se_subsystem_dev_t *se_dev, char *page)
{
	fd_dev_t *fd_dev = (fd_dev_t *) se_dev->se_dev_su_ptr;
	int bl = 0;

	__fd_get_dev_info(fd_dev, page, &bl);
	return((ssize_t)bl);
}

extern void fd_get_plugin_info (void *p, char *b, int *bl)
{
	*bl += sprintf(b+*bl, "%s FILEIO Plugin %s\n", PYX_ISCSI_VENDOR, FD_VERSION);
	
	return;
}

extern void fd_get_hba_info (se_hba_t *hba, char *b, int *bl)
{
	fd_host_t *fd_host = (fd_host_t *)hba->hba_ptr;

	*bl += sprintf(b+*bl, "iSCSI Host ID: %u  FD Host ID: %u\n",
		 hba->hba_id, fd_host->fd_host_id);
	*bl += sprintf(b+*bl, "        LIO FILEIO HBA\n");

	return;
}

extern void fd_get_dev_info (se_device_t *dev, char *b, int *bl)
{
	fd_dev_t *fd_dev = (fd_dev_t *) dev->dev_ptr;

	__fd_get_dev_info(fd_dev, b, bl);
	return;
}

extern void __fd_get_dev_info (fd_dev_t *fd_dev, char *b, int *bl)
{
	*bl += sprintf(b+*bl, "LIO FILEIO ID: %u", fd_dev->fd_dev_id);
	*bl += sprintf(b+*bl, "        File: %s  Size: %llu  ",
		fd_dev->fd_dev_name, fd_dev->fd_dev_size);	

	if (fd_dev->fd_bd) {
		struct block_device *bd = fd_dev->fd_bd;
		
		*bl += sprintf(b+*bl, "%s\n",
				(!bd->bd_contains) ? "" :
				(bd->bd_holder == (fd_dev_t *)fd_dev) ?
					"CLAIMED: FILEIO" : "CLAIMED: OS");
	} else
		*bl += sprintf(b+*bl, "\n");

	return;
}

/*	fd_map_task_non_SG():
 *
 *
 */
extern void fd_map_task_non_SG (se_task_t *task)
{
	se_cmd_t *cmd = TASK_CMD(task);
	fd_request_t *req = (fd_request_t *) task->transport_req;

	req->fd_bufflen		= task->task_size;
	req->fd_buf		= (void *) T_TASK(cmd)->t_task_buf;
	req->fd_sg_count	= 0;
	
	return;
}

/*	fd_map_task_SG():
 *
 *
 */
extern void fd_map_task_SG (se_task_t *task)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;

	req->fd_bufflen		= task->task_size;
	req->fd_buf		= NULL;
	req->fd_sg_count	= task->task_sg_num;

	return;
}

/*      fd_CDB_inquiry():
 *
 *
 */
extern int fd_CDB_inquiry (se_task_t *task, u32 size)
{      
	fd_request_t *req = (fd_request_t *) task->transport_req;
		        
	req->fd_data_direction  = FD_DATA_READ;
			        
	/*
	 * This causes 255 instead of the requested 256 bytes
	 * to be returned.  This can be safely ignored for now,
	 * and take the Initiators word on INQUIRY data lengths.
	 */
#if 0
	cmd->data_length	= req->fd_bufflen;
#endif
	fd_map_task_non_SG(task);

	return(0);
}

/*      fd_CDB_none():
 *
 *
 */
extern int fd_CDB_none (se_task_t *task, u32 size)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;

	req->fd_data_direction	= FD_DATA_NONE;
	req->fd_bufflen		= 0;
	req->fd_sg_count	= 0;
	req->fd_buf		= NULL;

	return(0);
}

/*	fd_CDB_read_non_SG():
 *
 *
 */
extern int fd_CDB_read_non_SG (se_task_t *task, u32 size)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;

	req->fd_data_direction = FD_DATA_READ;
	fd_map_task_non_SG(task);

	return(0);
}

/*	fd_CDB_read_SG):
 *
 *
 */
extern int fd_CDB_read_SG (se_task_t *task, u32 size)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;

	req->fd_data_direction = FD_DATA_READ;
	fd_map_task_SG(task);

	return(req->fd_sg_count);
}

/*	fd_CDB_write_non_SG():
 *
 *
 */
extern int fd_CDB_write_non_SG (se_task_t *task, u32 size)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;
	
	req->fd_data_direction = FD_DATA_WRITE;
	fd_map_task_non_SG(task);
	
	return(0);
}

/*	fd_CDB_write_SG():
 *
 *
 */
extern int fd_CDB_write_SG (se_task_t *task, u32 size)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;

	req->fd_data_direction = FD_DATA_WRITE;
	fd_map_task_SG(task);

	return(req->fd_sg_count);
}

/*	fd_check_lba():
 *
 *
 */
extern int fd_check_lba (unsigned long long lba, se_device_t *dev)
{
	return(0);
}

/*	fd_check_for_SG(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern int fd_check_for_SG (se_task_t *task)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;
	
	return(req->fd_sg_count);
}

/*	fd_get_cdb(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern unsigned char *fd_get_cdb (se_task_t *task)
{
	fd_request_t *req = (fd_request_t *) task->transport_req;

	return(req->fd_scsi_cdb);
}

/*	fd_get_blocksize(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern u32 fd_get_blocksize (se_device_t *dev)
{
	return(FD_BLOCKSIZE);
}

/*	fd_get_device_rev(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern u32 fd_get_device_rev (se_device_t *dev)
{
	return(SCSI_2); 
}

/*	fd_get_device_type(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern u32 fd_get_device_type (se_device_t *dev)
{
	return(TYPE_DISK);
}

/*	fd_get_dma_length(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern u32 fd_get_dma_length (u32 task_size, se_device_t *dev)
{
	return(PAGE_SIZE);
}

/*	fd_get_max_sectors(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern u32 fd_get_max_sectors (se_device_t *dev)
{
	return(FD_MAX_SECTORS);
}

/*	fd_get_queue_depth(): (Part of se_subsystem_api_t template)
 *
 *
 */
extern u32 fd_get_queue_depth (se_device_t *dev)
{
	return(FD_DEVICE_QUEUE_DEPTH);
}

extern u32 fd_get_max_queue_depth (se_device_t *dev)
{
	return(FD_MAX_DEVICE_QUEUE_DEPTH);
}
