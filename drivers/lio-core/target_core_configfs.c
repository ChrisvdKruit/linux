/*********************************************************************************
 * Filename:  target_core_configfs.c
 *
 * This file contains ConfigFS logic for the Generic Target Engine project.
 *
 * Copyright (c) 2008, 2009  Nicholas A. Bellinger <nab@linux-iscsi.org>
 *
 * based on configfs Copyright (C) 2005 Oracle.  All rights reserved.
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
 ****************************************************************************/

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/utsrelease.h>
#include <linux/utsname.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/unistd.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/configfs.h>

#include <target_core_base.h>
#include <target_core_device.h>
#include <target_core_hba.h>
#include <target_core_plugin.h>
#include <target_core_transport.h>

#ifdef SNMP_SUPPORT
#include <linux/proc_fs.h>
#include <iscsi_target_mib.h>
#endif /* SNMP_SUPPORT */

#include <target_core_fabric_ops.h>
#include <target_core_configfs.h>
#include <configfs_macros.h>

extern se_global_t *se_global;

struct config_group target_core_hbagroup;

struct list_head g_tf_list;
struct mutex g_tf_lock;

/*
 * Temporary pointer required for target_core_mod to operate..
 */
struct target_core_fabric_ops *iscsi_fabric_ops = NULL;

/*
 * Tempory function required for target_core_mod to operate..
 */
extern struct target_core_fabric_ops *target_core_get_iscsi_ops (void)
{
	return(iscsi_fabric_ops);
}

struct target_core_configfs_attribute {
	struct configfs_attribute attr;
	ssize_t (*show)(void *, char *);
	ssize_t (*store)(void *, const char *, size_t);
};

/*
 * Attributes for /sys/kernel/config/target/
 */
static ssize_t target_core_attr_show (struct config_item *item,
				      struct configfs_attribute *attr,
				      char *page)
{
	return(sprintf(page, "Target Engine Core ConfigFS Infrastructure %s"
		" on %s/%s on "UTS_RELEASE"\n", TARGET_CORE_CONFIGFS_VERSION,
		utsname()->sysname, utsname()->machine));
}

static struct configfs_item_operations target_core_item_ops = {
	.show_attribute = target_core_attr_show,
};

static struct configfs_attribute target_core_item_attr_version = {
	.ca_owner	= THIS_MODULE,
	.ca_name	= "version",
	.ca_mode	= S_IRUGO,
};

static struct target_fabric_configfs *target_core_get_fabric (
	const char *name)
{
	struct target_fabric_configfs *tf;

	if (!(name))
		return(NULL);
	
	mutex_lock(&g_tf_lock);
	list_for_each_entry(tf, &g_tf_list, tf_list) {
		if (!(strcmp(tf->tf_name, name))) {
			atomic_inc(&tf->tf_access_cnt);
			mutex_unlock(&g_tf_lock);
			return(tf);
		}
	}
	mutex_unlock(&g_tf_lock);

	return(NULL);
}

/*
 * Called from struct target_core_group_ops->make_group()
 */
static struct config_group *target_core_register_fabric (
	struct config_group *group,
	const char *name)
{
	struct config_group *fabric_cg;
	struct target_fabric_configfs *tf;
	char *ptr;
	int ret;

	printk("Target_Core_ConfigFS: REGISTER -> group: %p name: %s\n", group, name);

        if (!(fabric_cg = kzalloc(sizeof(struct config_group), GFP_KERNEL)))
                return(ERR_PTR(-ENOMEM));
	/*
	 * Load LIO Target for $CONFIGFS/target/iscsi :-)
	 */
	if ((ptr = strstr(name, "iscsi"))) {
		if ((ret = request_module("iscsi_target_mod")) < 0) {
			printk(KERN_ERR "request_module() failed for"
				" iscsi_target_mod.ko: %d\n", ret);
			kfree(fabric_cg);
			return(ERR_PTR(-EINVAL));
		}
	} else {
		printk(KERN_ERR "Unsupported configfs target fabric %s\n", name);
		kfree(fabric_cg);
		return(ERR_PTR(-EINVAL));
	}

	if (!(tf = target_core_get_fabric(name))) {
		printk(KERN_ERR "target_core_get_fabric() failed for %s\n", name);
		kfree(fabric_cg);
		return(ERR_PTR(-EINVAL));
	}
	printk("Target_Core_ConfigFS: REGISTER -> Located fabric: %s\n", tf->tf_name);

	/*
	 * On a successful target_core_get_fabric() look, the returned 
	 * struct target_fabric_configfs *tf will contain a usage reference.
	 */
	printk("Target_Core_ConfigFS: REGISTER -> %p\n", tf->tf_fabric_cit);
        config_group_init_type_name(&tf->tf_group, name, tf->tf_fabric_cit);
        printk("Target_Core_ConfigFS: REGISTER -> Allocated Fabric: %s\n",
			tf->tf_group.cg_item.ci_name);

	tf->tf_fabric = &tf->tf_group.cg_item;
	printk("Target_Core_ConfigFS: REGISTER -> Set tf->tf_fabric for %s\n", name);
		
	return(&tf->tf_group);
}

/*
 * Called from struct target_core_group_ops->drop_item()
mkdir 
 */
static void target_core_deregister_fabric (
	struct config_group *group,
	struct config_item *item)
{
	struct target_fabric_configfs *tf = container_of(
		to_config_group(item), struct target_fabric_configfs, tf_group);

	printk("Target_Core_ConfigFS: DEREGISTER -> Looking up %s in tf list\n",
			config_item_name(item));

	printk("Target_Core_ConfigFS: DEREGISTER -> located fabric: %s\n", tf->tf_name);
	atomic_dec(&tf->tf_access_cnt);

	printk("Target_Core_ConfigFS: DEREGISTER -> Releasing tf->tf_fabric for %s\n", tf->tf_name);
	tf->tf_fabric = NULL;

	printk("Target_Core_ConfigFS: DEREGISTER -> Releasing ci %s\n",
			config_item_name(item));
	config_item_put(item);

	return;
}

static struct configfs_group_operations target_core_group_ops = {
	.make_group	= &target_core_register_fabric,
	.drop_item	= &target_core_deregister_fabric,
};

/*
 * All item attributes appearing in /sys/kernel/target/ appear here.
 */
static struct configfs_attribute *target_core_item_attrs[] = {
	&target_core_item_attr_version,
	NULL,
};

/* 
 * Provides Fabrics Groups and Item Attributes for /sys/kernel/config/target/
 */
static struct config_item_type target_core_fabrics_item = {
	.ct_item_ops	= &target_core_item_ops,
	.ct_group_ops	= &target_core_group_ops,
	.ct_attrs	= target_core_item_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem target_core_fabrics = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "target",
			.ci_type = &target_core_fabrics_item,
		},
	},
};

static struct configfs_subsystem *target_core_subsystem[] = {
	&target_core_fabrics,
	NULL,
};

//##############################################################################
// Start functions called by external Target Fabrics Modules
//##############################################################################

/*
 * First function called by fabric modules to:
 *
 * 1) Allocate a struct target_fabric_configfs and save the *fabric_cit pointer.
 * 2) Add struct target_fabric_configfs to g_tf_list
 * 3) Return struct target_fabric_configfs to fabric module to be passed
 *    into target_fabric_configfs_register().
 */
extern struct target_fabric_configfs *target_fabric_configfs_init (
	struct config_item_type *fabric_cit,
	const char *name)
{
	struct target_fabric_configfs *tf;

	if (!(fabric_cit)) {
		printk(KERN_ERR "Missing struct config_item_type * pointer\n");
		return(NULL);
	}
	if (!(name)) {
		printk(KERN_ERR "Unable to locate passed fabric name\n");
		return(NULL);
	}
	if (strlen(name) > TARGET_FABRIC_NAME_SIZE) {
		printk(KERN_ERR "Passed name: %s exceeds TARGET_FABRIC_NAME_SIZE\n", name);
		return(NULL);
	}

	if (!(tf = kzalloc(sizeof(struct target_fabric_configfs), GFP_KERNEL)))
		return(ERR_PTR(-ENOMEM));

	INIT_LIST_HEAD(&tf->tf_list);
	atomic_set(&tf->tf_access_cnt, 0);
	tf->tf_fabric_cit = fabric_cit;
	tf->tf_subsys = target_core_subsystem[0];
	snprintf(tf->tf_name, TARGET_FABRIC_NAME_SIZE, "%s", name);

	mutex_lock(&g_tf_lock);
	list_add_tail(&tf->tf_list, &g_tf_list);
	mutex_unlock(&g_tf_lock);

	printk("<<<<<<<<<<<<<<<<<<<<<< BEGIN FABRIC API >>>>>>>>>>>>>>>>>>>>>>\n");
	printk("Initialized struct target_fabric_configfs: %p for %s\n", tf, tf->tf_name);
	return(tf);
}

/*
 * Called by fabric plugins after FAILED target_fabric_configfs_register() call..
 */
extern void target_fabric_configfs_free (
	struct target_fabric_configfs *tf)
{
	mutex_lock(&g_tf_lock);
	list_del(&tf->tf_list);
        mutex_unlock(&g_tf_lock);

	kfree(tf);
	return;
}

/* 
 * Note that config_group_find_item() calls config_item_get() and grabs the 
 * reference to the returned struct config_item *
 * It will be released with config_put_item() in target_fabric_configfs_deregister()
 */
extern struct config_item *target_fabric_configfs_find_by_name (
	struct configfs_subsystem *target_su,
	const char *name)
{
	struct config_item *fabric;

	mutex_lock(&target_su->su_mutex);
	fabric = config_group_find_item(&target_su->su_group, name);
	mutex_unlock(&target_su->su_mutex);

	return(fabric);
}

/*
 * Called 2nd from fabric module with returned parameter of
 * struct target_fabric_configfs * from target_fabric_configfs_init().
 * 
 * Upon a successful registration, the new fabric's struct config_item is return.
 * Also, a pointer to this struct is set in the passted struct target_fabric_configfs.
 */
extern int target_fabric_configfs_register (
	struct target_fabric_configfs *tf)
{
	struct config_group *su_group;

	if (!(tf)) {
		printk(KERN_ERR "Unable to locate target_fabric_configfs pointer\n");
		return(-EINVAL);
	}
	if (!(tf->tf_subsys)) {
		printk(KERN_ERR "Unable to target struct config_subsystem pointer\n");
		return(-EINVAL);
	}
	if (!(su_group = &tf->tf_subsys->su_group)) {
		printk(KERN_ERR "Unable to locate target struct config_group pointer\n");
		return(-EINVAL);
	}
	printk("<<<<<<<<<<<<<<<<<<<<<< END FABRIC API >>>>>>>>>>>>>>>>>>>>>>\n");

#warning FIXME: Remove temporary pointer to iscsi_fabric_ops..
	iscsi_fabric_ops = &tf->tf_ops;

	return(0);
}

extern void target_fabric_configfs_deregister (
	struct target_fabric_configfs *tf)
{
	struct config_group *su_group;
	struct configfs_subsystem *su;

	if (!(tf)) {
		printk(KERN_ERR "Unable to locate passed target_fabric_configfs\n");
		return;
	}
	if (!(su = tf->tf_subsys)) {
		printk(KERN_ERR "Unable to locate passed tf->tf_subsys pointer\n");
		return;
	}
	if (!(su_group = &tf->tf_subsys->su_group)) {
		printk(KERN_ERR "Unable to locate target struct config_group pointer\n");
		return;
	}

	printk("<<<<<<<<<<<<<<<<<<<<<< BEGIN FABRIC API >>>>>>>>>>>>>>>>>>>>>>\n");
	mutex_lock(&g_tf_lock);
	if (atomic_read(&tf->tf_access_cnt)) {
		mutex_unlock(&g_tf_lock);
		printk(KERN_ERR "Non zero tf->tf_access_cnt for fabric %s\n",
			tf->tf_name);
		BUG();
        }
	list_del(&tf->tf_list);
	mutex_unlock(&g_tf_lock);

	printk("Target_Core_ConfigFS: DEREGISTER -> Releasing tf: %s\n", tf->tf_name);
	tf->tf_fabric_cit = NULL;
	tf->tf_subsys = NULL;
	kfree(tf);

	printk("<<<<<<<<<<<<<<<<<<<<<< END FABRIC API >>>>>>>>>>>>>>>>>>>>>>\n");
	return;
}

EXPORT_SYMBOL(target_fabric_configfs_init);
EXPORT_SYMBOL(target_fabric_configfs_free);
EXPORT_SYMBOL(target_fabric_configfs_register);
EXPORT_SYMBOL(target_fabric_configfs_deregister);

//##############################################################################
// Stop functions called by external Target Fabrics Modules
//##############################################################################

// Start functions for struct config_item_type target_core_dev_attrib_cit

#define DEF_DEV_ATTRIB_SHOW(_name)						\
static ssize_t target_core_dev_show_attr_##_name (				\
	struct se_dev_attrib_s *da,						\
	char *page)								\
{										\
	se_device_t *dev;							\
	se_subsystem_dev_t *se_dev = da->da_sub_dev;				\
	ssize_t rb;								\
										\
	spin_lock(&se_dev->se_dev_lock);					\
	if (!(dev = se_dev->se_dev_ptr)) {					\
		spin_unlock(&se_dev->se_dev_lock); 				\
		return(-ENODEV);						\
	}									\
	rb = snprintf(page, PAGE_SIZE, "%u\n", (u32)DEV_ATTRIB(dev)->_name);	\
	spin_unlock(&se_dev->se_dev_lock);					\
										\
	return(rb);								\
}									

#define DEF_DEV_ATTRIB_STORE(_name)						\
static ssize_t target_core_dev_store_attr_##_name (				\
	struct se_dev_attrib_s *da,						\
	const char *page,							\
	size_t count)								\
{										\
	se_device_t *dev;							\
	se_subsystem_dev_t *se_dev = da->da_sub_dev;				\
	char *endptr;								\
	u32 val;								\
	int ret;								\
										\
	spin_lock(&se_dev->se_dev_lock);					\
	if (!(dev = se_dev->se_dev_ptr)) {					\
		spin_unlock(&se_dev->se_dev_lock);				\
		return(-ENODEV);						\
	}									\
	val = simple_strtoul(page, &endptr, 0);					\
	ret = se_dev_set_##_name(dev, (u32)val);				\
	spin_unlock(&se_dev->se_dev_lock);					\
										\
	return((!ret) ? count : -EINVAL);					\
}

#define DEF_DEV_ATTRIB(_name)							\
DEF_DEV_ATTRIB_SHOW(_name);							\
DEF_DEV_ATTRIB_STORE(_name);

#define DEF_DEV_ATTRIB_RO(_name)						\
DEF_DEV_ATTRIB_SHOW(_name);

CONFIGFS_EATTR_STRUCT(target_core_dev_attrib, se_dev_attrib_s);
#define SE_DEV_ATTR(_name, _mode)						\
static struct target_core_dev_attrib_attribute target_core_dev_attrib_##_name = \
		__CONFIGFS_EATTR(_name, _mode,					\
		target_core_dev_show_attr_##_name,				\
		target_core_dev_store_attr_##_name);		

#define SE_DEV_ATTR_RO(_name);							\
static struct target_core_dev_attrib_attribute target_core_dev_attrib_##_name = \
		__CONFIGFS_EATTR_RO(_name,					\
		target_core_dev_show_attr_##_name);

DEF_DEV_ATTRIB(status_thread);
SE_DEV_ATTR(status_thread, S_IRUGO | S_IWUSR);

DEF_DEV_ATTRIB(status_thread_tur);
SE_DEV_ATTR(status_thread_tur, S_IRUGO | S_IWUSR);

DEF_DEV_ATTRIB_RO(hw_max_sectors);
SE_DEV_ATTR_RO(hw_max_sectors);

DEF_DEV_ATTRIB(max_sectors);
SE_DEV_ATTR(max_sectors, S_IRUGO | S_IWUSR);

DEF_DEV_ATTRIB_RO(hw_queue_depth);
SE_DEV_ATTR_RO(hw_queue_depth);

DEF_DEV_ATTRIB(queue_depth);
SE_DEV_ATTR(queue_depth, S_IRUGO | S_IWUSR);

DEF_DEV_ATTRIB(task_timeout);
SE_DEV_ATTR(task_timeout, S_IRUGO | S_IWUSR);

CONFIGFS_EATTR_OPS(target_core_dev_attrib, se_dev_attrib_s, da_group);

static struct configfs_attribute *target_core_dev_attrib_attrs[] = {
	&target_core_dev_attrib_status_thread.attr,
	&target_core_dev_attrib_status_thread_tur.attr,
	&target_core_dev_attrib_hw_max_sectors.attr,
	&target_core_dev_attrib_max_sectors.attr,
	&target_core_dev_attrib_hw_queue_depth.attr,
	&target_core_dev_attrib_queue_depth.attr,
	&target_core_dev_attrib_task_timeout.attr,
	NULL,
};

static struct configfs_item_operations target_core_dev_attrib_ops = {
	.show_attribute		= target_core_dev_attrib_attr_show,
	.store_attribute	= target_core_dev_attrib_attr_store,
};

static struct config_item_type target_core_dev_attrib_cit = {
	.ct_item_ops		= &target_core_dev_attrib_ops,
	.ct_attrs		= target_core_dev_attrib_attrs,	
	.ct_owner		= THIS_MODULE,	
};

// End functions for struct config_item_type target_core_dev_attrib_cit

//  Start functions for struct config_item_type target_core_dev_wwn_cit

CONFIGFS_EATTR_STRUCT(target_core_dev_wwn, t10_wwn_s);
#define SE_DEV_WWN_ATTR(_name, _mode)						\
static struct target_core_dev_wwn_attribute target_core_dev_wwn_##_name =	\
		__CONFIGFS_EATTR(_name, _mode,					\
		target_core_dev_wwn_show_attr_##_name,				\
		target_core_dev_wwn_store_attr_##_name);

#define SE_DEV_WWN_ATTR_RO(_name);						\
static struct target_core_dev_wwn_attribute target_core_dev_wwn_##_name =	\
		__CONFIGFS_EATTR_RO(_name,					\
		target_core_dev_wwn_show_attr_##_name);

/*
 * EVPD page 0x80 Unit serial
 */
static ssize_t target_core_dev_wwn_show_attr_evpd_unit_serial (
	struct t10_wwn_s *t10_wwn,
	char *page)
{
	se_subsystem_dev_t *se_dev = t10_wwn->t10_sub_dev;
	se_device_t *dev;

        if (!(dev = se_dev->se_dev_ptr)) 
                return(-ENODEV);

	return(sprintf(page, "T10 EVPD Unit Serial Number:: %s\n",
		&t10_wwn->unit_serial[0]));
}

static ssize_t target_core_dev_wwn_store_attr_evpd_unit_serial (
	struct t10_wwn_s *t10_wwn,
	const char *page,
	size_t count)
{
	return(-ENOMEM);
}

SE_DEV_WWN_ATTR(evpd_unit_serial, S_IRUGO | S_IWUSR);

/*
 * EVPD page 0x83 Protocol Identifier
 */
static ssize_t target_core_dev_wwn_show_attr_evpd_protocol_identifier (
	struct t10_wwn_s *t10_wwn,
	char *page)
{
	se_subsystem_dev_t *se_dev = t10_wwn->t10_sub_dev;
	se_device_t *dev;
	t10_evpd_t *evpd;
	unsigned char buf[EVPD_TMP_BUF_SIZE];
	ssize_t len = 0;

	if (!(dev = se_dev->se_dev_ptr))
		return(-ENODEV);

	memset(buf, 0, EVPD_TMP_BUF_SIZE);

	spin_lock(&t10_wwn->t10_evpd_lock);
	list_for_each_entry(evpd, &t10_wwn->t10_evpd_list, evpd_list) {
		if (!(evpd->protocol_identifier_set))
			continue;

		transport_dump_evpd_proto_id(evpd, buf, EVPD_TMP_BUF_SIZE);

		if ((len + strlen(buf) > PAGE_SIZE))
			break;

		len += sprintf(page+len, "%s", buf);
	}
	spin_unlock(&t10_wwn->t10_evpd_lock);

	return(len);
}

static ssize_t target_core_dev_wwn_store_attr_evpd_protocol_identifier (
	struct t10_wwn_s *t10_wwn,
	const char *page,
	size_t count)
{
	return(-ENOSYS);
}

SE_DEV_WWN_ATTR(evpd_protocol_identifier, S_IRUGO | S_IWUSR);

/*
 * Generic wrapper for dumping EVPD identifiers by assoication.
 */
#define DEF_DEV_WWN_ASSOC_SHOW(_name, _assoc)					\
static ssize_t target_core_dev_wwn_show_attr_##_name (				\
	struct t10_wwn_s *t10_wwn,						\
	char *page)								\
{										\
	se_subsystem_dev_t *se_dev = t10_wwn->t10_sub_dev;			\
	se_device_t *dev;							\
	t10_evpd_t *evpd;							\
	unsigned char buf[EVPD_TMP_BUF_SIZE];					\
	ssize_t len = 0;							\
										\
	if (!(dev = se_dev->se_dev_ptr))					\
		return(-ENODEV);						\
										\
	spin_lock(&t10_wwn->t10_evpd_lock);					\
	list_for_each_entry(evpd, &t10_wwn->t10_evpd_list, evpd_list) {		\
		if (evpd->association != _assoc)				\
			continue;						\
										\
		memset(buf, 0, EVPD_TMP_BUF_SIZE);				\
		transport_dump_evpd_assoc(evpd, buf, EVPD_TMP_BUF_SIZE);	\
		if ((len + strlen(buf) > PAGE_SIZE))				\
			break;							\
		len += sprintf(page+len, "%s", buf);				\
										\
		memset(buf, 0, EVPD_TMP_BUF_SIZE);				\
		transport_dump_evpd_ident_type(evpd, buf, EVPD_TMP_BUF_SIZE);	\
		if ((len + strlen(buf) > PAGE_SIZE))				\
			break;							\
		len += sprintf(page+len, "%s", buf);				\
										\
		memset(buf, 0, EVPD_TMP_BUF_SIZE);				\
		transport_dump_evpd_ident(evpd, buf, EVPD_TMP_BUF_SIZE);	\
		if ((len + strlen(buf) > PAGE_SIZE))				\
			break;							\
		len += sprintf(page+len, "%s", buf);				\
	}									\
	spin_unlock(&t10_wwn->t10_evpd_lock);					\
										\
	return(len);								\
}

/*
 * EVPD page 0x83 Assoication: Logical Unit
 */
DEF_DEV_WWN_ASSOC_SHOW(evpd_assoc_logical_unit, 0x00);

static ssize_t target_core_dev_wwn_store_attr_evpd_assoc_logical_unit (
	struct t10_wwn_s *t10_wwn,
	const char *page,
	size_t count)
{
	return(-ENOSYS);
}

SE_DEV_WWN_ATTR(evpd_assoc_logical_unit, S_IRUGO | S_IWUSR);

/*
 * EVPD page 0x83 Association: Target Port
 */
DEF_DEV_WWN_ASSOC_SHOW(evpd_assoc_target_port, 0x10);

static ssize_t target_core_dev_wwn_store_attr_evpd_assoc_target_port (
	struct t10_wwn_s *t10_wwn,
	const char *page,
	size_t count)
{
        return(-ENOSYS);
}

SE_DEV_WWN_ATTR(evpd_assoc_target_port, S_IRUGO | S_IWUSR);

/*
 * EVPD page 0x83 Association: SCSI Target Device
 */
DEF_DEV_WWN_ASSOC_SHOW(evpd_assoc_scsi_target_device, 0x20);

static ssize_t target_core_dev_wwn_store_attr_evpd_assoc_scsi_target_device (
	struct t10_wwn_s *t10_wwn,
	const char *page,
	size_t count)
{
        return(-ENOSYS);
}

SE_DEV_WWN_ATTR(evpd_assoc_scsi_target_device, S_IRUGO | S_IWUSR);

CONFIGFS_EATTR_OPS(target_core_dev_wwn, t10_wwn_s, t10_wwn_group);

static struct configfs_attribute *target_core_dev_wwn_attrs[] = {
	&target_core_dev_wwn_evpd_unit_serial.attr,
	&target_core_dev_wwn_evpd_protocol_identifier.attr,
	&target_core_dev_wwn_evpd_assoc_logical_unit.attr,
	&target_core_dev_wwn_evpd_assoc_target_port.attr,
	&target_core_dev_wwn_evpd_assoc_scsi_target_device.attr,
	NULL,
};

static struct configfs_item_operations target_core_dev_wwn_ops = {
	.show_attribute		= target_core_dev_wwn_attr_show,
	.store_attribute	= target_core_dev_wwn_attr_store,
};

static struct config_item_type target_core_dev_wwn_cit = {
	.ct_item_ops		= &target_core_dev_wwn_ops,
	.ct_attrs		= target_core_dev_wwn_attrs,
	.ct_owner		= THIS_MODULE,
};

//  End functions for struct config_item_type target_core_dev_wwn_cit

//  Start functions for struct config_item_type target_core_dev_pr_cit

CONFIGFS_EATTR_STRUCT(target_core_dev_pr, se_subsystem_dev_s);
#define SE_DEV_PR_ATTR(_name, _mode)						\
static struct target_core_dev_pr_attribute target_core_dev_pr_##_name = 	\
	__CONFIGFS_EATTR(_name, _mode,						\
	target_core_dev_wwn_show_attr_##_name,					\
	target_core_dev_wwn_store_attr_##_name);

#define SE_DEV_PR_ATTR_RO(_name);						\
static struct target_core_dev_pr_attribute target_core_dev_pr_##_name =		\
	__CONFIGFS_EATTR_RO(_name,						\
	target_core_dev_pr_show_attr_##_name);					\

/*
 * res_active
 */
static ssize_t target_core_dev_pr_show_spc3_res (
	struct se_device_s *dev,
	char *page,
	ssize_t *len)
{
	*len += sprintf(page+*len, "Not Implemented yet\n");
	return(*len);
}

static ssize_t target_core_dev_pr_show_spc2_res (
        struct se_device_s *dev,
        char *page,
	ssize_t *len)
{
	se_node_acl_t *se_nacl;

	spin_lock(&dev->dev_reservation_lock);
	if (!(se_nacl = dev->dev_reserved_node_acl)) {
		*len += sprintf(page+*len, "None\n");
		spin_unlock(&dev->dev_reservation_lock);
		return(*len);
	}
	*len += sprintf(page+*len, "FABRIC[%s]: Initiator: %s\n",
		TPG_TFO(se_nacl->se_tpg)->get_fabric_name(),
		se_nacl->initiatorname);
	spin_unlock(&dev->dev_reservation_lock);

	return(*len);
}

static ssize_t target_core_dev_pr_show_attr_res_active (
	struct se_subsystem_dev_s *su_dev,
	char *page)
{
	ssize_t len = 0;

	switch (T10_RES(su_dev)->res_type) {
	case SPC3_PERSISTENT_RESERVATIONS:
		target_core_dev_pr_show_spc3_res(su_dev->se_dev_ptr,
				page, &len);
		break;
	case SPC2_RESERVATIONS:
		target_core_dev_pr_show_spc2_res(su_dev->se_dev_ptr,
				page, &len);
		break;
	case SPC_PASSTHROUGH:
		len += sprintf(page+len, "None\n");
		break;
	default:
		len += sprintf(page+len, "Unknown\n");
		break;
	}

	return(len);
}

SE_DEV_PR_ATTR_RO(res_active);

/*
 * res_type
 */
static ssize_t target_core_dev_pr_show_attr_res_type (
	struct se_subsystem_dev_s *su_dev,
	char *page)
{
	ssize_t len = 0;
	
	switch (T10_RES(su_dev)->res_type) {
	case SPC3_PERSISTENT_RESERVATIONS:
		len = sprintf(page, "SPC3_PERSISTENT_RESERVATIONS\n");
		break;
	case SPC2_RESERVATIONS:
		len = sprintf(page, "SPC2_RESERVATIONS\n");
		break;
	case SPC_PASSTHROUGH:
		len = sprintf(page, "SPC_PASSTHROUGH\n");
		break;
	default:
		len = sprintf(page, "UNKNOWN\n");
		break;
	}

	return(len);
}

SE_DEV_PR_ATTR_RO(res_type);

CONFIGFS_EATTR_OPS(target_core_dev_pr, se_subsystem_dev_s, se_dev_pr_group);

static struct configfs_attribute *target_core_dev_pr_attrs[] = {
	&target_core_dev_pr_res_active.attr,
	&target_core_dev_pr_res_type.attr,
	NULL,
};

static struct configfs_item_operations target_core_dev_pr_ops = {
	.show_attribute		= target_core_dev_pr_attr_show,
	.store_attribute	= target_core_dev_pr_attr_store,
};

static struct config_item_type target_core_dev_pr_cit = {
	.ct_item_ops		= &target_core_dev_pr_ops,
	.ct_attrs		= target_core_dev_pr_attrs,
	.ct_owner		= THIS_MODULE,
};

//  End functions for struct config_item_type target_core_dev_pr_cit

//  Start functions for struct config_item_type target_core_dev_cit

static ssize_t target_core_show_dev_info (void *p, char *page)
{
	se_subsystem_dev_t *se_dev = (se_subsystem_dev_t *)p;
	se_hba_t *hba = se_dev->se_dev_hba;
	se_subsystem_api_t *t;
	int ret = 0, bl = 0;
	ssize_t read_bytes = 0;

	t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hba->type, &ret);
	if (!t || (ret != 0)) 
		return(0);
	
	if (se_dev->se_dev_ptr) {
		transport_dump_dev_state(se_dev->se_dev_ptr, page, &bl);
		read_bytes += bl;
	}

	read_bytes += t->show_configfs_dev_params(hba, se_dev, page+read_bytes);
	return(read_bytes);
}

static struct target_core_configfs_attribute target_core_attr_dev_info = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "info",
		    .ca_mode = S_IRUGO },
	.show	= target_core_show_dev_info,
	.store	= NULL,
};

static ssize_t target_core_store_dev_control (void *p, const char *page, size_t count)
{
	se_subsystem_dev_t *se_dev = (se_subsystem_dev_t *)p;
	se_hba_t *hba = se_dev->se_dev_hba;
	se_subsystem_api_t *t;
	int ret = 0;

	if (!(se_dev->se_dev_su_ptr)) {
		printk(KERN_ERR "Unable to locate se_subsystem_dev_t>se_dev_su_ptr\n");
		return(-EINVAL);
	}

	t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hba->type, &ret);
        if (!t || (ret != 0))
		return(-EINVAL);

	return(t->set_configfs_dev_params(hba, se_dev, page, count));
}

static struct target_core_configfs_attribute target_core_attr_dev_control = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "control",
		    .ca_mode = S_IWUSR },
	.show	= NULL,
	.store	= target_core_store_dev_control,
};

static ssize_t target_core_store_dev_fd (void *p, const char *page, size_t count)
{
	se_subsystem_dev_t *se_dev = (se_subsystem_dev_t *)p;	
	se_device_t *dev;
	se_hba_t *hba = se_dev->se_dev_hba;
	se_subsystem_api_t *t;
	int ret = 0;

	if (se_dev->se_dev_ptr) {
		printk(KERN_ERR "se_dev->se_dev_ptr already set, ignoring fd request\n");
		return(-EEXIST);
	}

	t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hba->type, &ret);		
	if (!t || (ret != 0))
		return(-EINVAL);

	if (!(t->create_virtdevice_from_fd)) {
		printk(KERN_ERR "se_subsystem_api_t->create_virtdevice_from_fd()"
			" NULL for: %s\n", hba->transport->name);
		return(-EINVAL);
	}
	/*
	 * The subsystem PLUGIN is responsible for calling target_core_mod
	 * symbols to claim the underlying struct block_device for TYPE_DISK.
	 */
	dev = t->create_virtdevice_from_fd(se_dev, page);
	if (!(dev) || IS_ERR(dev))
		goto out;

	se_dev->se_dev_ptr = dev;
	printk("Target_Core_ConfigFS: Registered %s se_dev->se_dev_ptr: %p"
		" from fd\n", hba->transport->name, se_dev->se_dev_ptr);
	return(count);
out:
	return(-EINVAL);
}

static struct target_core_configfs_attribute target_core_attr_dev_fd = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "fd",
		    .ca_mode = S_IWUSR },
	.show	= NULL,
	.store	= target_core_store_dev_fd,
};

static ssize_t target_core_store_dev_enable (void *p, const char *page, size_t count)
{
	se_subsystem_dev_t *se_dev = (se_subsystem_dev_t *)p;
	se_device_t *dev;
	se_hba_t *hba = se_dev->se_dev_hba;
	se_subsystem_api_t *t;
	char *ptr;
	int ret = 0;
	
	if (!(ptr = strstr(page, "1"))) {
		printk(KERN_ERR "For dev_enable ops, only valid value is \"1\"\n");
		return(-EINVAL);
	}
	if ((se_dev->se_dev_ptr)) {
		printk(KERN_ERR "se_dev->se_dev_ptr already set for storage object\n");
		return(-EEXIST);
	}

	t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hba->type, &ret);
	if (!t || (ret != 0)) 
		return(-EINVAL);

	if (t->check_configfs_dev_params(hba, se_dev) < 0)
		return(-EINVAL);

	dev = t->create_virtdevice(hba, se_dev, se_dev->se_dev_su_ptr);
	if (!(dev) || IS_ERR(dev))
		return(-EINVAL);

	se_dev->se_dev_ptr = dev;
	printk("Target_Core_ConfigFS: Registered se_dev->se_dev_ptr: %p\n", se_dev->se_dev_ptr);

	return(count);
}

static struct target_core_configfs_attribute target_core_attr_dev_enable = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "enable",
		    .ca_mode = S_IWUSR },
	.show	= NULL,
	.store	= target_core_store_dev_enable,
};

static struct configfs_attribute *lio_core_dev_attrs[] = {
	&target_core_attr_dev_info.attr,
	&target_core_attr_dev_control.attr,
	&target_core_attr_dev_fd.attr,
	&target_core_attr_dev_enable.attr,
	NULL,
};

static ssize_t target_core_dev_show (struct config_item *item,
				     struct configfs_attribute *attr,
				     char *page)
{
	se_subsystem_dev_t *se_dev = container_of(
			to_config_group(item), se_subsystem_dev_t, se_dev_group);
	struct target_core_configfs_attribute *tc_attr = container_of(
			attr, struct target_core_configfs_attribute, attr);

	if (!(tc_attr->show))
		return(-EINVAL);
	
	return(tc_attr->show((void *)se_dev, page));
}

static ssize_t target_core_dev_store (struct config_item *item,
				      struct configfs_attribute *attr,
				      const char *page, size_t count)
{
	se_subsystem_dev_t *se_dev = container_of(
			to_config_group(item), se_subsystem_dev_t, se_dev_group);
	struct target_core_configfs_attribute *tc_attr = container_of(
			attr, struct target_core_configfs_attribute, attr);

	if (!(tc_attr->store))
		return(-EINVAL);

	return(tc_attr->store((void *)se_dev, page, count));
}

static struct configfs_item_operations target_core_dev_item_ops = {
	.release		= NULL,
	.show_attribute		= target_core_dev_show,
	.store_attribute	= target_core_dev_store,
};

static struct config_item_type target_core_dev_cit = {
	.ct_item_ops		= &target_core_dev_item_ops,
	.ct_attrs		= lio_core_dev_attrs,
	.ct_owner		= THIS_MODULE,
};

// End functions for struct config_item_type target_core_dev_cit

// Start functions for struct config_item_type target_core_hba_cit

static struct config_group *target_core_call_createdev (
	struct config_group *group,
	const char *name)
{
	se_subsystem_dev_t *se_dev;	
	se_hba_t *hba, *hba_p;
	se_subsystem_api_t *t;
	struct config_item *hba_ci;
	struct config_group *dev_cg = NULL;
	int ret = 0;

	if (!(hba_ci = &group->cg_item)) {
		printk(KERN_ERR "Unable to locate config_item hba_ci\n");
		return(NULL);
	}

	if (!(hba_p = container_of(to_config_group(hba_ci), se_hba_t, hba_group))) {
		printk(KERN_ERR "Unable to locate se_hba_t from struct config_item\n");
		return(NULL);
	}

	if (!(hba = core_get_hba_from_id(hba_p->hba_id, 0)))
		return(NULL);
	/*
	 * Locate the se_subsystem_api_t from parent's se_hba_t.
	 */
	t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hba->type, &ret);
	if (!t || (ret != 0)) {
		core_put_hba(hba);
		return(NULL);
	}

	if (!(se_dev = kzalloc(sizeof(se_subsystem_dev_t), GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate memory for se_subsystem_dev_t\n");
		return(NULL);
	}
	INIT_LIST_HEAD(&se_dev->t10_wwn.t10_evpd_list);
	spin_lock_init(&se_dev->t10_wwn.t10_evpd_lock);
	spin_lock_init(&se_dev->se_dev_lock);

	se_dev->t10_wwn.t10_sub_dev = se_dev;
	se_dev->se_dev_attrib.da_sub_dev = se_dev;

	se_dev->se_dev_hba = hba;
	dev_cg = &se_dev->se_dev_group;

	if (!(dev_cg->default_groups = kzalloc(sizeof(struct config_group) * 4,
			GFP_KERNEL)))
		goto out;

	/*
	 * Set se_dev_ptr from se_subsystem_api_t returned void ptr..
	 */
	if (!(se_dev->se_dev_su_ptr = t->allocate_virtdevice(hba, name))) {
		printk(KERN_ERR "Unable to locate subsystem dependent pointer from"
				" allocate_virtdevice()\n");
		goto out;
	}

	config_group_init_type_name(&se_dev->se_dev_group, name,
			&target_core_dev_cit);
	config_group_init_type_name(&se_dev->se_dev_attrib.da_group, "attrib",
			&target_core_dev_attrib_cit);
	config_group_init_type_name(&se_dev->se_dev_pr_group, "pr",
			&target_core_dev_pr_cit);
	config_group_init_type_name(&se_dev->t10_wwn.t10_wwn_group, "wwn",
			&target_core_dev_wwn_cit);
	dev_cg->default_groups[0] = &se_dev->se_dev_attrib.da_group;
	dev_cg->default_groups[1] = &se_dev->se_dev_pr_group;
	dev_cg->default_groups[2] = &se_dev->t10_wwn.t10_wwn_group;
	dev_cg->default_groups[3] = NULL;

	printk("Target_Core_ConfigFS: Allocated se_subsystem_dev_t: %p se_dev_su_ptr: %p\n",
			se_dev, se_dev->se_dev_su_ptr);

	core_put_hba(hba);
	return(&se_dev->se_dev_group);
out:
	if (dev_cg)
		kfree(dev_cg->default_groups);
	kfree(se_dev);
	core_put_hba(hba);
	return(NULL);
}

static void target_core_call_freedev (
	struct config_group *group,
	struct config_item *item)
{
	se_subsystem_dev_t *se_dev = container_of(to_config_group(item), se_subsystem_dev_t, se_dev_group);
	se_hba_t *hba, *hba_p;
	se_subsystem_api_t *t;
	int ret = 0;

	if (!(hba_p = se_dev->se_dev_hba)) {
		printk(KERN_ERR "Unable to locate se_hba_t from se_subsystem_dev_t\n");
		goto out;
	}

	if (!(hba = core_get_hba_from_id(hba_p->hba_id, 0)))
		goto out;

	t = (se_subsystem_api_t *)plugin_get_obj(PLUGIN_TYPE_TRANSPORT, hba->type, &ret);
	if (!t || (ret != 0))
		goto hba_out;

	config_item_put(item);

	/*
	 * This pointer will set when the storage is enabled with:
	 * `echo 1 > $CONFIGFS/core/$HBA/$DEV/dev_enable`
	 */	
	if (se_dev->se_dev_ptr) {
		printk("Target_Core_ConfigFS: Calling se_free_virtual_device() for"
			" se_dev_ptr: %p\n", se_dev->se_dev_ptr);

		if ((ret = se_free_virtual_device(se_dev->se_dev_ptr, hba)) < 0)
			goto hba_out;
	} else {
		/*
		 * Release se_subsystem_dev_t->se_dev_su_ptr..
		 */
		printk("Target_Core_ConfigFS: Calling t->free_device() for"
			" se_dev_su_ptr: %p\n", se_dev->se_dev_su_ptr);

		t->free_device(se_dev->se_dev_su_ptr);
	}

	printk("Target_Core_ConfigFS: Deallocating se_subsystem_dev_t: %p\n", se_dev);

hba_out:
	core_put_hba(hba);
out:
	kfree(se_dev);
	return;
}

static struct configfs_group_operations target_core_hba_group_ops = {
	.make_group		= target_core_call_createdev,
	.drop_item		= target_core_call_freedev,
};

static ssize_t target_core_hba_show (struct config_item *item,
				struct configfs_attribute *attr,
				char *page)
{
	se_hba_t *hba = container_of(to_config_group(item), se_hba_t, hba_group);

	if (!(hba)) {
		printk(KERN_ERR "Unable to locate se_hba_t\n");
		return(0);
	}

	return(sprintf(page, "HBA Index: %d plugin: %s version: %s\n",
			hba->hba_id, hba->transport->name,
			TARGET_CORE_CONFIGFS_VERSION));
}

static struct configfs_attribute target_core_hba_attr = {
	.ca_owner		= THIS_MODULE,
	.ca_name		= "hba_info",
	.ca_mode		= S_IRUGO,
};

static struct configfs_item_operations target_core_hba_item_ops = {
	.show_attribute		= target_core_hba_show,
};

static struct configfs_attribute *target_core_hba_attrs[] = {
	&target_core_hba_attr,
	NULL,
};

static struct config_item_type target_core_hba_cit = {
	.ct_item_ops		= &target_core_hba_item_ops,
	.ct_group_ops		= &target_core_hba_group_ops,
	.ct_attrs		= target_core_hba_attrs,
	.ct_owner		= THIS_MODULE,
};

static struct config_group *target_core_call_addhbatotarget (
	struct config_group *group,
	const char *name)
{
	char *se_plugin_str, *str, *str2, *endptr;
	se_hba_t *hba;
	se_plugin_t *se_plugin;
	char buf[TARGET_CORE_NAME_MAX_LEN];
	u32 plugin_dep_id;
	int hba_type = 0, ret;

	memset(buf, 0, TARGET_CORE_NAME_MAX_LEN);
	if (strlen(name) > TARGET_CORE_NAME_MAX_LEN) {
		printk(KERN_ERR "Passed *name strlen(): %d exceeds"
			" TARGET_CORE_NAME_MAX_LEN: %d\n", (int)strlen(name),
			TARGET_CORE_NAME_MAX_LEN);
		return(ERR_PTR(-ENAMETOOLONG));
	}
	snprintf(buf, TARGET_CORE_NAME_MAX_LEN, "%s", name);

	if (!(str = strstr(buf, "_"))) {
		printk(KERN_ERR "Unable to locate \"_\" for $SUBSYSTEM_PLUGIN_$HOST_ID\n");
		return(ERR_PTR(-EINVAL));
	}
	se_plugin_str = buf;

	/*
	 * Special case for subsystem plugins that have "_" in their names.
	 * Namely rd_direct and rd_mcp..
	 */
	if ((str2 = strstr(str+1, "_"))) {
		*str2 = '\0'; /* Terminate for *se_plugin_str */
		str2++; /* Skip to start of plugin dependent ID */
	} else {
		*str = '\0'; /* Terminate for *se_plugin_str */
		str++; /* Skip to start of plugin dependent ID */
	}

	if (!(se_plugin = transport_core_get_plugin_by_name(se_plugin_str)))
		return(ERR_PTR(-EINVAL));

	hba_type = se_plugin->plugin_type;
	plugin_dep_id = simple_strtoul(str, &endptr, 0);
	printk("Target_Core_ConfigFS: Located se_plugin: %p plugin_name: %s"
		" hba_type: %d plugin_dep_id: %u\n", se_plugin,
		se_plugin->plugin_name, hba_type, plugin_dep_id);

	if (!(hba = core_get_next_free_hba()))
		return(ERR_PTR(-EINVAL));
	
	hba->type = hba_type;

	if ((ret = se_core_add_hba(hba, plugin_dep_id)) < 0)
		goto out;

	config_group_init_type_name(&hba->hba_group, name, &target_core_hba_cit);

	core_put_hba(hba);
	return(&hba->hba_group);
out:
	hba->type = 0;
	core_put_hba(hba);
	return(ERR_PTR(ret));
}


static void target_core_call_delhbafromtarget (
	struct config_group *group,
	struct config_item *item)
{
	se_hba_t *hba_p = container_of(to_config_group(item), se_hba_t, hba_group);
	se_hba_t *hba = NULL;
	int ret;

	if (!(hba_p)) {
		printk(KERN_ERR "Unable to locate se_hba_t from struct config_item\n");
		return;
	}

	if (!(hba = core_get_hba_from_id(hba_p->hba_id, 0)))
		return;
	
	config_item_put(item);

	ret = se_core_del_hba(hba);
	core_put_hba(hba);
	return;

}

static struct configfs_group_operations target_core_ops = {
	.make_group	= target_core_call_addhbatotarget,
	.drop_item	= target_core_call_delhbafromtarget,
};

static struct config_item_type target_core_cit = {
//	.ct_item_ops	= &target_core_item_ops,
	.ct_group_ops	= &target_core_ops,
//	.ct_attrs	= target_core_attrs,
	.ct_owner	= THIS_MODULE,
};

// Stop functions for struct config_item_type target_core_hba_cit

extern int target_core_init_configfs (void)
{
	struct config_group *target_cg;
	struct configfs_subsystem *subsys;
#ifdef SNMP_SUPPORT
	struct proc_dir_entry *scsi_target_proc;
#endif
	int ret;

	printk("TARGET_CORE[0]: Loading Generic Kernel Storage Engine: %s on %s/%s"
		" on "UTS_RELEASE"\n", TARGET_CORE_VERSION, utsname()->sysname,
		utsname()->machine);

	subsys = target_core_subsystem[0];	
	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);

	/*
	 * Create $CONFIGFS/target/core default group for HBA <-> Storage Object
	 */
	target_cg = &subsys->su_group;
	if (!(target_cg->default_groups = kzalloc(sizeof(struct config_group) * 2,
			GFP_KERNEL))) {
		printk(KERN_ERR "Unable to allocate core_cg\n");
		return(-ENOMEM);
	}

	config_group_init_type_name(&target_core_hbagroup,
		 	"core", &target_core_cit);
	target_cg->default_groups[0] = &target_core_hbagroup;
	target_cg->default_groups[1] = NULL;

	/*
	 * Initialize the global vars, and then register the target subsystem
	 * with configfs.
	 */
	INIT_LIST_HEAD(&g_tf_list);
	mutex_init(&g_tf_lock);
#ifdef SNMP_SUPPORT
	init_scsi_index_table();
#endif
	if ((ret = configfs_register_subsystem(subsys)) < 0) {
		printk(KERN_ERR "Error %d while registering subsystem %s\n",
			ret, subsys->su_group.cg_item.ci_namebuf);
		return(-1);
	}
	printk("TARGET_CORE[0]: Initialized ConfigFS Fabric Infrastructure: "
		""TARGET_CORE_CONFIGFS_VERSION" on %s/%s on "UTS_RELEASE"\n",
			utsname()->sysname, utsname()->machine);

	if ((ret = init_se_global()) < 0)
		goto out;

	plugin_load_all_classes();
#ifdef SNMP_SUPPORT
	if (!(scsi_target_proc = proc_mkdir("scsi_target", 0))) {
		printk(KERN_ERR "proc_mkdir(scsi_target, 0) failed\n");
		goto out;
	}
	if ((ret = init_scsi_target_mib()) < 0)
		goto out;
#endif
	return(0);

out:
	configfs_unregister_subsystem(subsys);
#ifdef SNMP_SUPPORT
	remove_proc_entry("scsi_target", 0);
#endif
	plugin_unload_all_classes();
	release_se_global();
	return(-1);
}

extern void target_core_exit_configfs (void)
{
	struct configfs_subsystem *subsys;
	struct config_item *item;
	int i;

	se_global->in_shutdown = 1;

	subsys = target_core_subsystem[0];

	for (i = 0; subsys->su_group.default_groups[i]; i++) {
		item = &subsys->su_group.default_groups[i]->cg_item;	
		subsys->su_group.default_groups[i] = NULL;
		config_item_put(item);
	}

	configfs_unregister_subsystem(subsys);
	printk("TARGET_CORE[0]: Released ConfigFS Fabric Infrastructure\n");

#ifdef SNMP_SUPPORT
	remove_scsi_target_mib();
	remove_proc_entry("scsi_target", 0);
#endif
	plugin_unload_all_classes();
	release_se_global();

	return;
}

MODULE_DESCRIPTION("Target_Core_Mod/ConfigFS");
MODULE_AUTHOR("nab@Linux-iSCSI.org");
MODULE_LICENSE("GPL");

module_init(target_core_init_configfs);
module_exit(target_core_exit_configfs);
