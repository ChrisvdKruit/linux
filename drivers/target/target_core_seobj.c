/*******************************************************************************
 * Filename:  target_core_seobj.c
 *
 * Copyright (c) 2006-2007 SBE, Inc.  All Rights Reserved.
 * Copyright (c) 2007-2009 Rising Tide Software, Inc.
 * Copyright (c) 2008-2009 Linux-iSCSI.org
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


#include <linux/string.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/in.h>

#include <target/target_core_base.h>
#include <target/target_core_device.h>
#include <target/target_core_tpg.h>
#include <target/target_core_transport.h>
#include <target/target_core_fabric_ops.h>
#include <target/target_core_configfs.h>

#include "target_core_seobj.h"

int dev_obj_export(void *p, struct se_portal_group *tpg, struct se_lun *lun)
{
	struct se_device *dev  = (struct se_device *)p;
	struct se_port *port;

	port = core_alloc_port(dev);
	if (!(port))
		return -1;

	lun->se_dev = dev;
	se_dev_start(p);

	atomic_inc(&dev->dev_export_obj.obj_access_count);
	core_export_port(dev, tpg, port, lun);
	return 0;
}

void dev_obj_unexport(void *p, struct se_portal_group *tpg, struct se_lun *lun)
{
	struct se_device *dev  = (struct se_device *)p;
	struct se_port *port = lun->lun_sep;

	spin_lock(&dev->se_port_lock);
	spin_lock(&lun->lun_sep_lock);
	if (lun->lun_type_ptr == NULL) {
		spin_unlock(&dev->se_port_lock);
		spin_unlock(&lun->lun_sep_lock);
		return;
	}
	spin_unlock(&lun->lun_sep_lock);

	atomic_dec(&dev->dev_export_obj.obj_access_count);
	core_release_port(dev, port);
	spin_unlock(&dev->se_port_lock);

	se_dev_stop(p);
	lun->se_dev = NULL;
}

int dev_obj_max_sectors(void *p)
{
	struct se_device *dev  = (struct se_device *)p;

	if (TRANSPORT(dev)->transport_type == TRANSPORT_PLUGIN_PHBA_PDEV) {
		return (DEV_ATTRIB(dev)->max_sectors >
			TRANSPORT(dev)->get_max_sectors(dev) ?
			TRANSPORT(dev)->get_max_sectors(dev) :
			DEV_ATTRIB(dev)->max_sectors);
	} else
		return DEV_ATTRIB(dev)->max_sectors;
}

unsigned long long dev_obj_end_lba(void *p)
{
	struct se_device *dev  = (struct se_device *)p;

	 return dev->dev_sectors_total + 1;
}

int dev_obj_do_se_mem_map(
	void *p,
	struct se_task *task,
	struct list_head *se_mem_list,
	void *in_mem,
	struct se_mem *in_se_mem,
	struct se_mem **out_se_mem,
	u32 *se_mem_cnt,
	u32 *task_offset_in)
{
	struct se_device *dev  = (struct se_device *)p;
	u32 task_offset = *task_offset_in;
	int ret = 0;
	/*
	 * se_subsystem_api_t->do_se_mem_map is used when internal allocation
	 * has been done by the transport plugin.
	 */
	if (TRANSPORT(dev)->do_se_mem_map) {
		ret = TRANSPORT(dev)->do_se_mem_map(task, se_mem_list,
				in_mem, in_se_mem, out_se_mem, se_mem_cnt,
				task_offset_in);
		if (ret == 0)
			T_TASK(task->task_se_cmd)->t_task_se_num += *se_mem_cnt;

		return ret;
	}

	/*
	 * Assume default that transport plugin speaks preallocated
	 * scatterlists.
	 */
	if (!(transport_calc_sg_num(task, in_se_mem, task_offset)))
		return -1;

	/*
	 * struct se_task->task_sg now contains the struct scatterlist array.
	 */
	return transport_map_mem_to_sg(task, se_mem_list, task->task_sg,
		in_se_mem, out_se_mem, se_mem_cnt, task_offset_in);
}

int dev_obj_get_mem_buf(void *p, struct se_cmd *cmd)
{
	struct se_device *dev  = (struct se_device *)p;

	cmd->transport_allocate_resources = (TRANSPORT(dev)->allocate_buf) ?
		TRANSPORT(dev)->allocate_buf : &transport_generic_allocate_buf;
	cmd->transport_free_resources = (TRANSPORT(dev)->free_buf) ?
		TRANSPORT(dev)->free_buf : NULL;

	return 0;
}

int dev_obj_get_mem_SG(void *p, struct se_cmd *cmd)
{
	struct se_device *dev  = (struct se_device *)p;

	cmd->transport_allocate_resources = (TRANSPORT(dev)->allocate_DMA) ?
		TRANSPORT(dev)->allocate_DMA : &transport_generic_get_mem;
	cmd->transport_free_resources = (TRANSPORT(dev)->free_DMA) ?
		TRANSPORT(dev)->free_DMA : NULL;

	return 0;
}

map_func_t dev_obj_get_map_SG(void *p, int rw)
{
	struct se_device *dev  = (struct se_device *)p;

	return (rw == SE_DIRECTION_WRITE) ? dev->transport->cdb_write_SG :
		dev->transport->cdb_read_SG;
}

map_func_t dev_obj_get_map_non_SG(void *p, int rw)
{
	struct se_device *dev  = (struct se_device *)p;

	return (rw == SE_DIRECTION_WRITE) ? dev->transport->cdb_write_non_SG :
		dev->transport->cdb_read_non_SG;
}

map_func_t dev_obj_get_map_none(void *p)
{
	struct se_device *dev  = (struct se_device *)p;

	return dev->transport->cdb_none;
}

int dev_obj_check_online(void *p)
{
	struct se_device *dev  = (struct se_device *)p;
	int ret;

	spin_lock(&dev->dev_status_lock);
	ret = ((dev->dev_status & TRANSPORT_DEVICE_ACTIVATED) ||
	       (dev->dev_status & TRANSPORT_DEVICE_DEACTIVATED)) ? 0 : 1;
	spin_unlock(&dev->dev_status_lock);

	return ret;
}

int dev_obj_check_shutdown(void *p)
{
	struct se_device *dev  = (struct se_device *)p;
	int ret;

	spin_lock(&dev->dev_status_lock);
	ret = (dev->dev_status & TRANSPORT_DEVICE_SHUTDOWN);
	spin_unlock(&dev->dev_status_lock);

	return ret;
}
