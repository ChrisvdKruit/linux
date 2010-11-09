/*******************************************************************************
 * Filename:  target_core_file.c
 *
 * This file contains the Storage Engine <-> FILEIO transport specific functions
 *
 * Copyright (c) 2005 PyX Technologies, Inc.
 * Copyright (c) 2005-2006 SBE, Inc.  All Rights Reserved.
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
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>

#include <target/target_core_base.h>
#include <target/target_core_device.h>
#include <target/target_core_transport.h>

#include "target_core_file.h"

#if 1
#define DEBUG_FD_CACHE(x...) printk(x)
#else
#define DEBUG_FD_CACHE(x...)
#endif

#if 1
#define DEBUG_FD_FUA(x...) printk(x)
#else
#define DEBUG_FD_FUA(x...)
#endif

static struct se_subsystem_api fileio_template;

static void __fd_get_dev_info(struct fd_dev *, char *, int *);

/*	fd_attach_hba(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int fd_attach_hba(struct se_hba *hba, u32 host_id)
{
	struct fd_host *fd_host;

	fd_host = kzalloc(sizeof(struct fd_host), GFP_KERNEL);
	if (!(fd_host)) {
		printk(KERN_ERR "Unable to allocate memory for struct fd_host\n");
		return -1;
	}

	fd_host->fd_host_id = host_id;

	atomic_set(&hba->left_queue_depth, FD_HBA_QUEUE_DEPTH);
	atomic_set(&hba->max_queue_depth, FD_HBA_QUEUE_DEPTH);
	hba->hba_ptr = (void *) fd_host;

	printk(KERN_INFO "CORE_HBA[%d] - TCM FILEIO HBA Driver %s on Generic"
		" Target Core Stack %s\n", hba->hba_id, FD_VERSION,
		TARGET_CORE_MOD_VERSION);
	printk(KERN_INFO "CORE_HBA[%d] - Attached FILEIO HBA: %u to Generic"
		" Target Core with TCQ Depth: %d MaxSectors: %u\n",
		hba->hba_id, fd_host->fd_host_id,
		atomic_read(&hba->max_queue_depth), FD_MAX_SECTORS);

	return 0;
}

/*	fd_detach_hba(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int fd_detach_hba(struct se_hba *hba)
{
	struct fd_host *fd_host;

	if (!hba->hba_ptr) {
		printk(KERN_ERR "hba->hba_ptr is NULL!\n");
		return -1;
	}
	fd_host = (struct fd_host *) hba->hba_ptr;

	printk(KERN_INFO "CORE_HBA[%d] - Detached FILEIO HBA: %u from Generic"
		" Target Core\n", hba->hba_id, fd_host->fd_host_id);

	kfree(fd_host);
	hba->hba_ptr = NULL;

	return 0;
}

static void *fd_allocate_virtdevice(struct se_hba *hba, const char *name)
{
	struct fd_dev *fd_dev;
	struct fd_host *fd_host = (struct fd_host *) hba->hba_ptr;

	fd_dev = kzalloc(sizeof(struct fd_dev), GFP_KERNEL);
	if (!(fd_dev)) {
		printk(KERN_ERR "Unable to allocate memory for struct fd_dev\n");
		return NULL;
	}

	fd_dev->fd_host = fd_host;

	printk(KERN_INFO "FILEIO: Allocated fd_dev for %p\n", name);

	return fd_dev;
}

/*	fd_create_virtdevice(): (Part of se_subsystem_api_t template)
 *
 *
 */
static struct se_device *fd_create_virtdevice(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	void *p)
{
	char *dev_p = NULL;
	struct se_device *dev;
	struct se_dev_limits dev_limits;
	struct queue_limits *limits;
	struct fd_dev *fd_dev = (struct fd_dev *) p;
	struct fd_host *fd_host = (struct fd_host *) hba->hba_ptr;
	mm_segment_t old_fs;
	struct file *file;
	struct inode *inode = NULL;
	int dev_flags = 0, flags;

	memset(&dev_limits, 0, sizeof(struct se_dev_limits));

	old_fs = get_fs();
	set_fs(get_ds());
	dev_p = getname(fd_dev->fd_dev_name);
	set_fs(old_fs);

	if (IS_ERR(dev_p)) {
		printk(KERN_ERR "getname(%s) failed: %lu\n",
			fd_dev->fd_dev_name, IS_ERR(dev_p));
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
/*	flags |= O_DIRECT; */
	/*
	 * If fd_buffered_io=1 has not been set explictly (the default),
	 * use O_SYNC to force FILEIO writes to disk.
	 */
	if (!(fd_dev->fbd_flags & FDBD_USE_BUFFERED_IO))
		flags |= O_SYNC;

	file = filp_open(dev_p, flags, 0600);

	if (IS_ERR(file) || !file || !file->f_dentry) {
		printk(KERN_ERR "filp_open(%s) failed\n", dev_p);
		goto fail;
	}
	fd_dev->fd_file = file;
	/*
	 * If using a block backend with this struct file, we extract
	 * fd_dev->fd_[block,dev]_size from struct block_device.
	 *
	 * Otherwise, we use the passed fd_size= from configfs
	 */
	inode = file->f_mapping->host;
	if (S_ISBLK(inode->i_mode)) {
		struct request_queue *q;
		/*
		 * Setup the local scope queue_limits from struct request_queue->limits
		 * to pass into transport_add_device_to_core_hba() as struct se_dev_limits.
		 */
		q = bdev_get_queue(inode->i_bdev);
		limits = &dev_limits.limits;
		limits->logical_block_size = bdev_logical_block_size(inode->i_bdev);
		limits->max_hw_sectors = queue_max_hw_sectors(q);
		limits->max_sectors = queue_max_sectors(q);
		/*
		 * Determine the number of bytes from i_size_read() minus
		 * one (1) logical sector from underlying struct block_device
		 */
		fd_dev->fd_block_size = bdev_logical_block_size(inode->i_bdev);
		fd_dev->fd_dev_size = (i_size_read(file->f_mapping->host) -
		  		       fd_dev->fd_block_size);

		printk(KERN_INFO "FILEIO: Using size: %llu bytes from struct"
			" block_device blocks: %llu logical_block_size: %d\n",
			fd_dev->fd_dev_size,
			div_u64(fd_dev->fd_dev_size, fd_dev->fd_block_size),
			fd_dev->fd_block_size);
	} else {
		if (!(fd_dev->fbd_flags & FBDF_HAS_SIZE)) {
			printk(KERN_ERR "FILEIO: Missing fd_dev_size="
				" parameter, and no backing struct"
				" block_device\n");
			goto fail;
		}

		limits = &dev_limits.limits;
		limits->logical_block_size = FD_BLOCKSIZE;
		limits->max_hw_sectors = FD_MAX_SECTORS;
		limits->max_sectors = FD_MAX_SECTORS;
		fd_dev->fd_block_size = FD_BLOCKSIZE;
	}

	dev_limits.hw_queue_depth = FD_MAX_DEVICE_QUEUE_DEPTH;
	dev_limits.queue_depth = FD_DEVICE_QUEUE_DEPTH;

	dev = transport_add_device_to_core_hba(hba, &fileio_template,
				se_dev, dev_flags, (void *)fd_dev,
				&dev_limits, "FILEIO", FD_VERSION);
	if (!(dev))
		goto fail;

	fd_dev->fd_dev_id = fd_host->fd_host_dev_id_count++;
	fd_dev->fd_queue_depth = dev->queue_depth;

	printk(KERN_INFO "CORE_FILE[%u] - Added TCM FILEIO Device ID: %u at %s,"
		" %llu total bytes\n", fd_host->fd_host_id, fd_dev->fd_dev_id,
			fd_dev->fd_dev_name, fd_dev->fd_dev_size);

	putname(dev_p);
	return dev;
fail:
	if (fd_dev->fd_file) {
		filp_close(fd_dev->fd_file, NULL);
		fd_dev->fd_file = NULL;
	}
	putname(dev_p);
	return NULL;
}

/*	fd_free_device(): (Part of se_subsystem_api_t template)
 *
 *
 */
static void fd_free_device(void *p)
{
	struct fd_dev *fd_dev = (struct fd_dev *) p;

	if (fd_dev->fd_file) {
		filp_close(fd_dev->fd_file, NULL);
		fd_dev->fd_file = NULL;
	}

	kfree(fd_dev);
}

/*	fd_transport_complete(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int fd_transport_complete(struct se_task *task)
{
	return 0;
}

/*	fd_allocate_request(): (Part of se_subsystem_api_t template)
 *
 *
 */
static void *fd_allocate_request(
	struct se_task *task,
	struct se_device *dev)
{
	struct fd_request *fd_req;

	fd_req = kzalloc(sizeof(struct fd_request), GFP_KERNEL);
	if (!(fd_req)) {
		printk(KERN_ERR "Unable to allocate struct fd_request\n");
		return NULL;
	}

	fd_req->fd_dev = dev->dev_ptr;

	return (void *)fd_req;
}

static inline int fd_iovec_alloc(struct fd_request *req)
{
	req->fd_iovs = kzalloc(sizeof(struct iovec) * req->fd_sg_count,
				GFP_KERNEL);
	if (!(req->fd_iovs)) {
		printk(KERN_ERR "Unable to allocate req->fd_iovs\n");
		return -1;
	}

	return 0;
}

static int fd_do_readv(struct fd_request *req, struct se_task *task)
{
	struct file *fd = req->fd_dev->fd_file;
	struct scatterlist *sg = task->task_sg;
	struct iovec *iov;
	mm_segment_t old_fs;
	loff_t pos = (req->fd_lba * DEV_ATTRIB(task->se_dev)->block_size);
	int ret = 0, i;

	iov = kzalloc(sizeof(struct iovec) * req->fd_sg_count, GFP_KERNEL);
	if (!(iov)) {
		printk(KERN_ERR "Unable to allocate fd_do_readv iov[]\n");
		return -1;
	}

	for (i = 0; i < req->fd_sg_count; i++) {
		iov[i].iov_len = sg[i].length;
		iov[i].iov_base = sg_virt(&sg[i]);
	}

	old_fs = get_fs();
	set_fs(get_ds());
	ret = vfs_readv(fd, &iov[0], req->fd_sg_count, &pos);
	set_fs(old_fs);

	kfree(iov);
	/*
	 * Return zeros and GOOD status even if the READ did not return
	 * the expected virt_size for struct file w/o a backing struct
	 * block_device.
	 */
	if (S_ISBLK(fd->f_dentry->d_inode->i_mode)) {
		if (ret < 0 || ret != req->fd_size) {
			printk(KERN_ERR "vfs_readv() returned %d,"
				" expecting %d for S_ISBLK\n", ret,
				(int)req->fd_size);
			return -1;
		}
	} else {
		if (ret < 0) {
			printk(KERN_ERR "vfs_readv() returned %d for non"
				" S_ISBLK\n", ret);
			return -1;
		}
	}

	return 1;
}

#if 0

static void fd_sendfile_free_DMA(struct se_cmd *cmd)
{
	printk(KERN_INFO "Release reference to pages now..\n");
}

static static int fd_sendactor(
	read_descriptor_t *desc,
	struct page *page,
	unsigned long offset,
	unsigned long size)
{
	unsigned long count = desc->count;
	struct se_task *task = desc->arg.data;
	struct fd_request *req = (struct fd_request *) task->transport_req;
	struct scatterlist *sg = task->task_sg;

	printk(KERN_INFO "page: %p offset: %lu size: %lu\n", page,
			offset, size);

	__free_page(sg[req->fd_cur_offset].page);

	printk(KERN_INFO "page_address(page): %p\n", page_address(page));
	sg[req->fd_cur_offset].page = page;
	sg[req->fd_cur_offset].offset = offset;
	sg[req->fd_cur_offset].length = size;

	printk(KERN_INFO "sg[%d:%p].page %p length: %d\n", req->fd_cur_offset,
		&sg[req->fd_cur_offset], sg[req->fd_cur_offset].page,
		sg[req->fd_cur_offset].length);

	req->fd_cur_size += size;
	printk(KERN_INFO "fd_cur_size: %u\n", req->fd_cur_size);

	req->fd_cur_offset++;

	desc->count--;
	desc->written += size;
	return size;
}

static int fd_do_sendfile(struct fd_request *req, struct se_task *task)
{
	int ret = 0;
	struct file *fd = req->fd_dev->fd_file;

	if (fd_seek(fd, req->fd_lba, DEV_ATTRIB(task->se_dev)->block_size) < 0)
		return -1;

	TASK_CMD(task)->transport_free_DMA = &fd_sendfile_free_DMA;

	ret = fd->f_op->sendfile(fd, &fd->f_pos, req->fd_sg_count,
			fd_sendactor, (void *)task);

	if (ret < 0) {
		printk(KERN_ERR "fd->f_op->sendfile() returned %d\n", ret);
		return -1;
	}

	return 1;
}
#endif

static int fd_do_writev(struct fd_request *req, struct se_task *task)
{
	struct file *fd = req->fd_dev->fd_file;
	struct scatterlist *sg = task->task_sg;
	struct iovec *iov;
	mm_segment_t old_fs;
	loff_t pos = (req->fd_lba * DEV_ATTRIB(task->se_dev)->block_size);
	int ret, i = 0;

	iov = kzalloc(sizeof(struct iovec) * req->fd_sg_count, GFP_KERNEL);
	if (!(iov)) {
		printk(KERN_ERR "Unable to allocate fd_do_writev iov[]\n");
		return -1;
	}

	for (i = 0; i < req->fd_sg_count; i++) {
		iov[i].iov_len = sg[i].length;
		iov[i].iov_base = sg_virt(&sg[i]);
	}

	old_fs = get_fs();
	set_fs(get_ds());
	ret = vfs_writev(fd, &iov[0], req->fd_sg_count, &pos);
	set_fs(old_fs);

	kfree(iov);

	if (ret < 0 || ret != req->fd_size) {
		printk(KERN_ERR "vfs_writev() returned %d\n", ret);
		return -1;
	}

	return 1;
}

/*
 * Called from transport_generic_synchronize_cache() to flush the entire
 * struct file (and possibly backing struct block_device) using vfs_fsync().
 */
static int fd_do_sync_cache(struct se_cmd *cmd, int immed)
{
	struct fd_dev *fd_dev = cmd->se_dev->dev_ptr;
	struct file *fd = fd_dev->fd_file;
	int ret;

	ret = vfs_fsync(fd, 0);
	if (ret != 0) {
		printk(KERN_ERR "FILEIO: vfs_fsync(fd, 0) returned: %d\n", ret);
		return ret;
	}
	DEBUG_FD_CACHE("FILEIO: vfs_fsync(fd, 0) called, immed: %d\n", immed);
	return ret;
}

/*
 * Called from transport_generic_synchronize_cache() to flush LBA range
 */
int __fd_do_sync_cache_range(
	struct se_cmd *cmd,
	unsigned long long lba,
	u32 size_in_bytes)
{
	struct se_device *dev = cmd->se_dev;
	struct fd_dev *fd_dev = dev->dev_ptr;
	struct file *fd = fd_dev->fd_file;
	loff_t start = (lba * DEV_ATTRIB(dev)->block_size);
	loff_t bytes;
	int ret, immed = (T_TASK(cmd)->t_task_cdb[1] & 0x2);
	/*
	 * If the Immediate bit is set, queue up the GOOD response
	 * for this SYNCHRONIZE_CACHE op
	 */
	if (immed)
		transport_complete_sync_cache(cmd, 1);
	/*
	 * Determine if we will be flushing the entire device.
	 */
	if ((T_TASK(cmd)->t_task_lba == 0) && (cmd->data_length == 0)) {
		ret = fd_do_sync_cache(cmd, immed);		
		if (!(immed))
			transport_complete_sync_cache(cmd, (ret == 0) ? 1 : 0);
		return 0;
	}
	/*
	 * If a explict number of bytes fo flush has been provied by
	 * the initiator, use this value with vfs_sync_range().  Otherwise
	 * bytes = LLONG_MAX (matching fs/sync.c:vfs_fsync().
	 */
	bytes = (size_in_bytes != 0) ? size_in_bytes : LLONG_MAX;
	ret = vfs_fsync_range(fd, start, bytes, 0);
	if (ret != 0) {
		printk(KERN_ERR "FILEIO: vfs_fsync_range() failed: %d\n", ret);
		if (!(immed))
			transport_complete_sync_cache(cmd, 0);
		return -1;
	}
	DEBUG_FD_CACHE("FILEIO: vfs_fsync_range(): LBA: %llu Starting offset:"
		" %llu, bytes: %llu, immed: %d\n", lba, (unsigned long long)start,
		(unsigned long long)bytes, immed);

	if (!(immed))
		transport_complete_sync_cache(cmd, 1);

	return 0;
}

/*
 * Called by target_core_transport():transport_emulate_control_cdb()
 * to emulate SYCHRONIZE_CACHE_*
 */
void fd_emulate_sync_cache(struct se_task *task)
{
	struct se_cmd *cmd = TASK_CMD(task);

	__fd_do_sync_cache_range(cmd, T_TASK(cmd)->t_task_lba, cmd->data_length);
}

/*
 * Tell TCM Core that we are capable of WriteCache emulation for
 * an underlying struct se_device.
 */
int fd_emulated_write_cache(struct se_device *dev)
{
	return 1;
}

int fd_emulated_dpo(struct se_device *dev)
{
	return 0;
}
/*
 * Tell TCM Core that we will be emulating Forced Unit Access (FUA) for WRITEs
 * for TYPE_DISK.
 */
int fd_emulated_fua_write(struct se_device *dev)
{
	return 1;
}

int fd_emulated_fua_read(struct se_device *dev)
{
	return 0;
}

/*
 * WRITE Force Unit Access (FUA) emulation on a per struct se_task
 * LBA range basis..
 */
static inline int fd_emulate_write_fua(
	struct se_cmd *cmd,
	struct se_task *task)
{
	DEBUG_FD_CACHE("FILEIO: FUA WRITE LBA: %llu, bytes: %u\n",
			task->task_lba, task->task_size);

	return __fd_do_sync_cache_range(cmd, task->task_lba, task->task_size);
}

static int fd_do_task(struct se_task *task)
{
	struct se_cmd *cmd = task->task_se_cmd;
	struct se_device *dev = cmd->se_dev;
	struct fd_request *req = task->transport_req;
	int ret = 0;

	req->fd_lba = task->task_lba;
	req->fd_size = task->task_size;
	/*
	 * Call vectorized fileio functions to map struct scatterlist
	 * physical memory addresses to struct iovec virtual memory.
	 */
	if (req->fd_data_direction == FD_DATA_READ)
		ret = fd_do_readv(req, task);
	else
		ret = fd_do_writev(req, task);

	if (ret < 0)
		return ret;

	if (ret) {
		/*
		 * Check for Forced Unit Access WRITE emulation
		 */
		if ((DEV_ATTRIB(dev)->emulate_write_cache > 0) &&
		    (DEV_ATTRIB(dev)->emulate_fua_write > 0) &&
		    (req->fd_data_direction == FD_DATA_WRITE) &&
		    (T_TASK(cmd)->t_tasks_fua)) {
			/*
			 * We might need to be a bit smarter here
			 * and return some sense data to let the initiator
			 * know the FUA WRITE cache sync failed..?
			 */
			ret = fd_emulate_write_fua(cmd, task);
			if (ret < 0) {
				printk(KERN_ERR "FILEIO: fd_emulate"
					"_write_fua() failed\n");
			}
		}

		task->task_scsi_status = GOOD;
		transport_complete_task(task, 1);
	}

	return PYX_TRANSPORT_SENT_TO_TRANSPORT;
}

/*	fd_free_task(): (Part of se_subsystem_api_t template)
 *
 *
 */
static void fd_free_task(struct se_task *task)
{
	struct fd_request *req = task->transport_req;

	kfree(req->fd_iovs);
	kfree(req);
}

static ssize_t fd_set_configfs_dev_params(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	const char *page, ssize_t count)
{
	struct fd_dev *fd_dev = (struct fd_dev *) se_dev->se_dev_su_ptr;
	char *buf, *cur, *ptr, *ptr2;
	int params = 0;
	/*
	 * Make sure we take into account the NULL terminator when copying
	 * the const buffer here..
	 */
	buf = kzalloc(count + 1, GFP_KERNEL);
	if (!(buf)) {
		printk(KERN_ERR "Unable to allocate memory for"
				" temporary buffer\n");
		return 0;
	}
	memcpy(buf, page, count);
	cur = buf;

	while (cur) {
		ptr = strstr(cur, "=");
		if (!(ptr))
			goto out;

		*ptr = '\0';
		ptr++;

		ptr2 = strstr(cur, "fd_dev_name");
		if (ptr2) {
			transport_check_dev_params_delim(ptr, &cur);
			ptr = strstrip(ptr);
			snprintf(fd_dev->fd_dev_name, FD_MAX_DEV_NAME,
					"%s", ptr);
			printk(KERN_INFO "FILEIO: Referencing Path: %s\n",
					fd_dev->fd_dev_name);
			fd_dev->fbd_flags |= FBDF_HAS_PATH;
			params++;
			continue;
		}
		ptr2 = strstr(cur, "fd_dev_size");
		if (ptr2) {
			transport_check_dev_params_delim(ptr, &cur);
			if (strict_strtoull(ptr, 0, &fd_dev->fd_dev_size) < 0) {
				printk(KERN_ERR "strict_strtoull() failed for"
						" fd_dev_size=\n");
				continue;
			}
			printk(KERN_INFO "FILEIO: Referencing Size: %llu"
					" bytes\n", fd_dev->fd_dev_size);
			fd_dev->fbd_flags |= FBDF_HAS_SIZE;
			params++;
			continue;
		}
		ptr2 = strstr(cur, "fd_buffered_io");
		if (ptr2) {
			transport_check_dev_params_delim(ptr, &cur);
			if (strncmp(ptr, "1", 1))
				continue;

			printk(KERN_INFO "FILEIO: Using buffered I/O"
				" operations for struct fd_dev\n");

			fd_dev->fbd_flags |= FDBD_USE_BUFFERED_IO;
			params++;
			continue;
		} else
			cur = NULL;
	}

out:
	kfree(buf);
	return (params) ? count : -EINVAL;
}

static ssize_t fd_check_configfs_dev_params(struct se_hba *hba, struct se_subsystem_dev *se_dev)
{
	struct fd_dev *fd_dev = (struct fd_dev *) se_dev->se_dev_su_ptr;

	if (!(fd_dev->fbd_flags & FBDF_HAS_PATH)) {
		printk(KERN_ERR "Missing fd_dev_name=\n");
		return -1;
	}

	return 0;
}

static ssize_t fd_show_configfs_dev_params(
	struct se_hba *hba,
	struct se_subsystem_dev *se_dev,
	char *page)
{
	struct fd_dev *fd_dev = (struct fd_dev *) se_dev->se_dev_su_ptr;
	int bl = 0;

	__fd_get_dev_info(fd_dev, page, &bl);
	return (ssize_t)bl;
}

static void fd_get_plugin_info(void *p, char *b, int *bl)
{
	*bl += sprintf(b + *bl, "TCM FILEIO Plugin %s\n", FD_VERSION);
}

static void fd_get_hba_info(struct se_hba *hba, char *b, int *bl)
{
	struct fd_host *fd_host = (struct fd_host *)hba->hba_ptr;

	*bl += sprintf(b + *bl, "SE Host ID: %u  FD Host ID: %u\n",
		 hba->hba_id, fd_host->fd_host_id);
	*bl += sprintf(b + *bl, "        TCM FILEIO HBA\n");
}

static void fd_get_dev_info(struct se_device *dev, char *b, int *bl)
{
	struct fd_dev *fd_dev = dev->dev_ptr;

	__fd_get_dev_info(fd_dev, b, bl);
}

static void __fd_get_dev_info(struct fd_dev *fd_dev, char *b, int *bl)
{
	*bl += sprintf(b + *bl, "TCM FILEIO ID: %u", fd_dev->fd_dev_id);
	*bl += sprintf(b + *bl, "        File: %s  Size: %llu  Mode: %s\n",
		fd_dev->fd_dev_name, fd_dev->fd_dev_size,
		(fd_dev->fbd_flags & FDBD_USE_BUFFERED_IO) ?
		"Buffered" : "Synchronous");
}

/*	fd_map_task_non_SG():
 *
 *
 */
static void fd_map_task_non_SG(struct se_task *task)
{
	struct se_cmd *cmd = TASK_CMD(task);
	struct fd_request *req = task->transport_req;

	req->fd_bufflen		= task->task_size;
	req->fd_buf		= (void *) T_TASK(cmd)->t_task_buf;
	req->fd_sg_count	= 0;
}

/*	fd_map_task_SG():
 *
 *
 */
static void fd_map_task_SG(struct se_task *task)
{
	struct fd_request *req = task->transport_req;

	req->fd_bufflen		= task->task_size;
	req->fd_buf		= NULL;
	req->fd_sg_count	= task->task_sg_num;
}

/*      fd_CDB_none():
 *
 *
 */
static int fd_CDB_none(struct se_task *task, u32 size)
{
	struct fd_request *req = task->transport_req;

	req->fd_data_direction	= FD_DATA_NONE;
	req->fd_bufflen		= 0;
	req->fd_sg_count	= 0;
	req->fd_buf		= NULL;

	return 0;
}

/*	fd_CDB_read_non_SG():
 *
 *
 */
static int fd_CDB_read_non_SG(struct se_task *task, u32 size)
{
	struct fd_request *req = task->transport_req;

	req->fd_data_direction = FD_DATA_READ;
	fd_map_task_non_SG(task);

	return 0;
}

/*	fd_CDB_read_SG):
 *
 *
 */
static int fd_CDB_read_SG(struct se_task *task, u32 size)
{
	struct fd_request *req = task->transport_req;

	req->fd_data_direction = FD_DATA_READ;
	fd_map_task_SG(task);

	return req->fd_sg_count;
}

/*	fd_CDB_write_non_SG():
 *
 *
 */
static int fd_CDB_write_non_SG(struct se_task *task, u32 size)
{
	struct fd_request *req = task->transport_req;

	req->fd_data_direction = FD_DATA_WRITE;
	fd_map_task_non_SG(task);

	return 0;
}

/*	fd_CDB_write_SG():
 *
 *
 */
static int fd_CDB_write_SG(struct se_task *task, u32 size)
{
	struct fd_request *req = task->transport_req;

	req->fd_data_direction = FD_DATA_WRITE;
	fd_map_task_SG(task);

	return req->fd_sg_count;
}

/*	fd_check_lba():
 *
 *
 */
static int fd_check_lba(unsigned long long lba, struct se_device *dev)
{
	return 0;
}

/*	fd_check_for_SG(): (Part of se_subsystem_api_t template)
 *
 *
 */
static int fd_check_for_SG(struct se_task *task)
{
	struct fd_request *req = task->transport_req;

	return req->fd_sg_count;
}

/*	fd_get_cdb(): (Part of se_subsystem_api_t template)
 *
 *
 */
static unsigned char *fd_get_cdb(struct se_task *task)
{
	struct fd_request *req = task->transport_req;

	return req->fd_scsi_cdb;
}

/*	fd_get_device_rev(): (Part of se_subsystem_api_t template)
 *
 *
 */
static u32 fd_get_device_rev(struct se_device *dev)
{
	return SCSI_SPC_2; /* Returns SPC-3 in Initiator Data */
}

/*	fd_get_device_type(): (Part of se_subsystem_api_t template)
 *
 *
 */
static u32 fd_get_device_type(struct se_device *dev)
{
	return TYPE_DISK;
}

/*	fd_get_dma_length(): (Part of se_subsystem_api_t template)
 *
 *
 */
static u32 fd_get_dma_length(u32 task_size, struct se_device *dev)
{
	return PAGE_SIZE;
}

static sector_t fd_get_blocks(struct se_device *dev)
{
	struct fd_dev *fd_dev = dev->dev_ptr;
	unsigned long long blocks_long = div_u64(fd_dev->fd_dev_size,
			DEV_ATTRIB(dev)->block_size);

	return blocks_long;
}

static struct se_subsystem_api fileio_template = {
	.name			= "fileio",
	.owner			= THIS_MODULE,
	.type			= FILEIO,
	.transport_type		= TRANSPORT_PLUGIN_VHBA_PDEV,
	.attach_hba		= fd_attach_hba,
	.detach_hba		= fd_detach_hba,
	.cdb_none		= fd_CDB_none,
	.cdb_read_non_SG	= fd_CDB_read_non_SG,
	.cdb_read_SG		= fd_CDB_read_SG,
	.cdb_write_non_SG	= fd_CDB_write_non_SG,
	.cdb_write_SG		= fd_CDB_write_SG,
	.allocate_virtdevice	= fd_allocate_virtdevice,
	.create_virtdevice	= fd_create_virtdevice,
	.free_device		= fd_free_device,
	.dpo_emulated		= fd_emulated_dpo,
	.fua_write_emulated	= fd_emulated_fua_write,
	.fua_read_emulated	= fd_emulated_fua_read,
	.write_cache_emulated	= fd_emulated_write_cache,
	.transport_complete	= fd_transport_complete,
	.allocate_request	= fd_allocate_request,
	.do_task		= fd_do_task,
	.do_sync_cache		= fd_emulate_sync_cache,
	.free_task		= fd_free_task,
	.check_configfs_dev_params = fd_check_configfs_dev_params,
	.set_configfs_dev_params = fd_set_configfs_dev_params,
	.show_configfs_dev_params = fd_show_configfs_dev_params,
	.get_plugin_info	= fd_get_plugin_info,
	.get_hba_info		= fd_get_hba_info,
	.get_dev_info		= fd_get_dev_info,
	.check_lba		= fd_check_lba,
	.check_for_SG		= fd_check_for_SG,
	.get_cdb		= fd_get_cdb,
	.get_device_rev		= fd_get_device_rev,
	.get_device_type	= fd_get_device_type,
	.get_dma_length		= fd_get_dma_length,
	.get_blocks		= fd_get_blocks,
};

static int __init fileio_module_init(void)
{
	return transport_subsystem_register(&fileio_template);
}

static void fileio_module_exit(void)
{
	transport_subsystem_release(&fileio_template);
}

MODULE_DESCRIPTION("TCM FILEIO subsystem plugin");
MODULE_AUTHOR("nab@Linux-iSCSI.org");
MODULE_LICENSE("GPL");

module_init(fileio_module_init);
module_exit(fileio_module_exit);
