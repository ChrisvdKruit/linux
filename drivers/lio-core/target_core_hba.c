/*********************************************************************************
 * Filename:  target_core_hba.c
 *
 * This file copntains the iSCSI HBA Transport related functions.
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


#define TARGET_CORE_HBA_C

#include <linux/net.h>
#include <linux/string.h>
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
#include <iscsi_target_ioctl.h>
#include <iscsi_target_ioctl_defs.h>
#include <iscsi_target_error.h>
#include <target_core_device.h>
#include <iscsi_target_device.h>
#include <target_core_hba.h>
#include <iscsi_target_tpg.h>
#include <target_core_transport.h>
#include <iscsi_target_util.h>

#include <target_core_plugin.h>
#include <target_core_seobj.h>

#undef TARGET_CORE_HBA_C

extern se_global_t *se_global;

extern se_hba_t *iscsi_get_hba_from_ptr (void *p)
{
	se_hba_t *hba;
	u32 i;
	
	spin_lock(&se_global->hba_lock);	
	for (i = 0; i < ISCSI_MAX_GLOBAL_HBAS; i++) {
		hba = &se_global->hba_list[i];
		
		if (hba->hba_status != HBA_STATUS_ACTIVE)	
			continue;
		
		if (!hba->hba_ptr)
			continue;

		if (hba->hba_ptr == p) {
			spin_unlock(&se_global->hba_lock);

			down_interruptible(&hba->hba_access_sem);
			return((signal_pending(current)) ? NULL : hba);
		}
	}
	spin_unlock(&se_global->hba_lock);

	return(NULL);
}

EXPORT_SYMBOL(iscsi_get_hba_from_ptr);
	
extern se_hba_t *__core_get_hba_from_id (se_hba_t *hba)
{
	down_interruptible(&hba->hba_access_sem);
	return((signal_pending(current)) ? NULL : hba);
}

extern se_hba_t *core_get_hba_from_id (u32 hba_id, int addhba)
{
	se_hba_t *hba;
	
	if (hba_id > (ISCSI_MAX_GLOBAL_HBAS-1)) {
		TRACE_ERROR("iSCSI HBA_ID: %u exceeds ISCSI_MAX_GLOBAL_HBAS-1: %u\n",
			hba_id, ISCSI_MAX_GLOBAL_HBAS-1);
		return(NULL);
	}

	hba = &se_global->hba_list[hba_id];

	if (!addhba && !(hba->hba_status & HBA_STATUS_ACTIVE))
		return(NULL);

	return(__core_get_hba_from_id(hba));
}

EXPORT_SYMBOL(core_get_hba_from_id);

extern se_hba_t *core_get_next_free_hba (void)
{
	se_hba_t *hba;
	u32 i;

	spin_lock(&se_global->hba_lock);     
	for (i = 0; i < ISCSI_MAX_GLOBAL_HBAS; i++) {
		hba = &se_global->hba_list[i];
		if (hba->hba_status != HBA_STATUS_FREE)
			continue;

		spin_unlock(&se_global->hba_lock);
		return(__core_get_hba_from_id(hba));
	}
	spin_unlock(&se_global->hba_lock);

	TRACE_ERROR("Unable to locate next free HBA\n");
	return(NULL);
}

extern void core_put_hba (se_hba_t *hba)
{
	up(&hba->hba_access_sem);
	return;
}

EXPORT_SYMBOL(core_put_hba);

/*	iscsi_hba_check_addhba_params();
 *
 *
 */
extern int iscsi_hba_check_addhba_params (
	struct iscsi_target *tg,
	se_hbainfo_t *hi)
{
	int ret = 0;
	se_subsystem_api_t *t;

	if (!(tg->params_set & PARAM_HBA_ID)) {
		TRACE_ERROR("hba_id must be set for addhbatotarget\n");
		return(ERR_HBA_MISSING_PARAMS);
	}
	hi->hba_type = tg->hba_type;
	
	if (!(t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, tg->hba_type, &ret)))
		return(ret);

	if ((ret = t->check_hba_params(hi, tg, 0)) < 0)
		return(ret);
	
	return(0);
}

EXPORT_SYMBOL(iscsi_hba_check_addhba_params);

/*	iscsi_hba_add_hba():
 *
 *
 */
extern int iscsi_hba_add_hba (
	se_hba_t *hba,
	se_hbainfo_t *hi,
	struct iscsi_target *tg)
{
	int ret = 0;
	se_subsystem_api_t *t;
	
	if (hba->hba_status & HBA_STATUS_ACTIVE)
                return(ERR_ADDTHBA_ALREADY_ACTIVE);

	atomic_set(&hba->max_queue_depth, 0);
	atomic_set(&hba->left_queue_depth, 0);
	hba->hba_info.hba_type = hi->hba_type;
	hba->hba_info.hba_id = hi->hba_id;
	
	if (!(t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hi->hba_type, &ret)))
		return(ret);
	
	if ((ret = t->check_hba_params(&hba->hba_info, tg, 0)) < 0)
		return(ret);
	
	if (t->check_ghost_id(hi))
		return(-1);

	if (t->attach_hba(NULL, hba, hi) < 0)
		return(-1);
	
	hba->type = hi->hba_type;

	hba->hba_status &= ~HBA_STATUS_FREE;
	hba->hba_status |= HBA_STATUS_ACTIVE;

	PYXPRINT("CORE_HBA[%d] - Attached HBA to Generic Target Core\n", hba->hba_id);

	return(0);
}

EXPORT_SYMBOL(iscsi_hba_add_hba);

static int iscsi_shutdown_hba (
	se_hba_t *hba)
{
	int ret = 0;
	se_subsystem_api_t *t;

	if (!(t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hba->type, &ret)))
		return(ret);

	if (t->detach_hba(hba) < 0)
		return(ERR_SHUTDOWN_DETACH_HBA);

	return(0);
}

/*	iscsi_hba_del_hba():
 *
 *
 */
extern int iscsi_hba_del_hba (
	se_hba_t *hba)
{
	se_device_t *dev, *dev_next;
	
	if (!(hba->hba_status & HBA_STATUS_ACTIVE)) {
		TRACE_ERROR("HBA ID: %d Status: INACTIVE, ignoring delhbafromtarget"
			" request\n", hba->hba_id);
		return(ERR_DELHBA_NOT_ACTIVE);
	}

	/*
	 * Do not allow the se_hba_t to be released if references exist to
	 * from se_device_t->se_lun_t.
	 */
	if (iscsi_check_devices_access(hba) < 0) {
		TRACE_ERROR("CORE_HBA[%u] - **ERROR** - Unable to release HBA"
			" with active LUNs\n", hba->hba_id);
		return(ERR_DELHBA_SHUTDOWN_FAILED);
	}

	spin_lock(&hba->device_lock);
	dev = hba->device_head;
	while (dev) {
		dev_next = dev->next;

		se_clear_dev_ports(dev);
		spin_unlock(&hba->device_lock);
		
		se_release_device_for_hba(dev);

		spin_lock(&hba->device_lock);
		dev = dev_next;
	}
	spin_unlock(&hba->device_lock);

	iscsi_shutdown_hba(hba);
	
	hba->hba_status &= ~HBA_STATUS_ACTIVE;
	hba->hba_status |= HBA_STATUS_FREE;

	PYXPRINT("CORE_HBA[%d] - Detached HBA from Generic Target Core\n", hba->hba_id);

	return(0);
}

EXPORT_SYMBOL(iscsi_hba_del_hba);

static void se_hba_transport_shutdown (se_hba_t *hba)
{
	if (!(HBA_TRANSPORT(hba)->shutdown_hba))
		return;

	HBA_TRANSPORT(hba)->shutdown_hba(hba);
	return;
}

extern void iscsi_disable_all_hbas (void)
{
	int i;
	se_hba_t *hba;

	spin_lock(&se_global->hba_lock);
	for (i = 0; i < ISCSI_MAX_GLOBAL_HBAS; i++) {
		hba = &se_global->hba_list[i];

		if (!(hba->hba_status & HBA_STATUS_ACTIVE))
			continue;

		spin_unlock(&se_global->hba_lock);
		iscsi_disable_devices_for_hba(hba);
		spin_lock(&se_global->hba_lock);
	}
	spin_unlock(&se_global->hba_lock);

	return;
}

EXPORT_SYMBOL(iscsi_disable_all_hbas);

/*	iscsi_hba_del_all_hbas():
 *
 *
 */
extern void iscsi_hba_del_all_hbas (void)
{
	int i;
	se_hba_t *hba;

	spin_lock(&se_global->hba_lock);
	for (i = 0; i < ISCSI_MAX_GLOBAL_HBAS; i++) {
		hba = &se_global->hba_list[i];
	
		if (!(hba->hba_status & HBA_STATUS_ACTIVE))
			continue;
#if 0
		PYXPRINT("Shutting down HBA ID: %d, HBA_TYPE: %d, STATUS: 0x%08x\n",
			hba->hba_id, hba->type, hba->hba_status);
#endif		
		spin_unlock(&se_global->hba_lock);
		se_hba_transport_shutdown(hba);
		iscsi_hba_del_hba(hba);
		spin_lock(&se_global->hba_lock);
	}
	spin_unlock(&se_global->hba_lock);

	return;
}

EXPORT_SYMBOL(iscsi_hba_del_all_hbas);
