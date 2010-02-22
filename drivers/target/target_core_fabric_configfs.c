/*******************************************************************************
* Filename: target_core_fabric_configfs.c
 *
 * This file contains generic fabric module configfs infrastructure for
 * TCM v4.x code
 *
 * Copyright (c) 2010 Rising Tide Systems
 * Copyright (c) 2010 Linux-iSCSI.org
 *
 * Copyright (c) 2010 Nicholas A. Bellinger <nab@linux-iscsi.org>
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

#include <target/target_core_base.h>
#include <target/target_core_device.h>
#include <target/target_core_hba.h>
#include <target/target_core_plugin.h>
#include <target/target_core_seobj.h>
#include <target/target_core_transport.h>
#include <target/target_core_alua.h>
#include <target/target_core_pr.h>
#include <target/target_core_fabric_ops.h>
#include <target/target_core_fabric_configfs.h>
#include <target/target_core_configfs.h>
#include <target/configfs_macros.h>

#define TF_CIT_SETUP(_name, _item_ops, _group_ops, _attrs)		\
static void target_fabric_setup_##_name##_cit(struct target_fabric_configfs *tf) \
{									\
	struct target_fabric_configfs_template *tfc = &tf->tf_cit_tmpl;	\
	struct config_item_type *cit = &tfc->tfc_##_name##_cit;		\
									\
	cit->ct_item_ops = _item_ops;					\
	cit->ct_group_ops = _group_ops;					\
	cit->ct_attrs = _attrs;						\
	cit->ct_owner = tf->tf_module;					\
	printk("Setup generic %s\n", __stringify(_name));		\
}

/* Start of tfc_tpg_acl_cit */

static struct configfs_group_operations target_fabric_acl_group_ops = {
	.make_group	= NULL,
	.drop_item	= NULL,
};

TF_CIT_SETUP(tpg_acl, NULL, &target_fabric_acl_group_ops, NULL);

/* End of tfc_tpg_acl_cit */

/* Start of tfc_tpg_np_base_cit */

CONFIGFS_EATTR_OPS(target_fabric_np_base, se_tpg_np_s, tpg_np_group);

static struct configfs_item_operations target_fabric_np_base_item_ops = {
	.show_attribute		= target_fabric_np_base_attr_show,
	.store_attribute	= target_fabric_np_base_attr_store,
};

TF_CIT_SETUP(tpg_np_base, &target_fabric_np_base_item_ops, NULL, NULL);

/* End of tfc_tpg_np_base_cit */

/* Start of tfc_tpg_np_cit */

static struct config_group *target_fabric_make_np(
	struct config_group *group,
	const char *name)
{
	struct se_portal_group_s *se_tpg = container_of(group,
				struct se_portal_group_s, tpg_np_group);
	struct target_fabric_configfs *tf = se_tpg->se_tpg_wwn->wwn_tf;
	struct se_tpg_np_s *se_tpg_np;

	if (!(tf->tf_ops.fabric_make_np)) {
		printk(KERN_ERR "tf->tf_ops.fabric_make_np is NULL\n");
		return ERR_PTR(-ENOSYS);
	}

	se_tpg_np = tf->tf_ops.fabric_make_np(se_tpg, group, name);
	if (!(se_tpg_np) || IS_ERR(se_tpg_np))
		return ERR_PTR(-EINVAL);

	config_group_init_type_name(&se_tpg_np->tpg_np_group, name,
			&TF_CIT_TMPL(tf)->tfc_tpg_np_base_cit);						

	return &se_tpg_np->tpg_np_group;
}

static void target_fabric_drop_np(
	struct config_group *group,
	struct config_item *item)
{
	struct se_portal_group_s *se_tpg = container_of(group,
				struct se_portal_group_s, tpg_np_group);
	struct target_fabric_configfs *tf = se_tpg->se_tpg_wwn->wwn_tf;
	struct se_tpg_np_s *se_tpg_np = container_of(to_config_group(item),
				struct se_tpg_np_s, tpg_np_group);

	config_item_put(item);
	tf->tf_ops.fabric_drop_np(se_tpg_np);
}

static struct configfs_group_operations target_fabric_np_group_ops = {
	.make_group	= &target_fabric_make_np,
	.drop_item	= &target_fabric_drop_np,
};

TF_CIT_SETUP(tpg_np, NULL, &target_fabric_np_group_ops, NULL);

/* End of tfc_tpg_np_cit */

/* Start of tfc_tpg_port_cit */

CONFIGFS_EATTR_STRUCT(target_fabric_port, se_lun_s);
#define TCM_PORT_ATTR(_name, _mode)					\
static struct target_fabric_port_attribute target_fabric_port_##_name =	\
	__CONFIGFS_EATTR(_name, _mode,					\
	target_fabric_port_show_attr_##_name,				\
	target_fabric_port_store_attr_##_name);

#define TCM_PORT_ATTOR_RO(_name)					\
	__CONFIGFS_EATTR_RO(_name,					\
	target_fabric_port_show_attr_##_name);

/*
 * alua_tg_pt_gp
 */
static ssize_t target_fabric_port_show_attr_alua_tg_pt_gp(
	struct se_lun_s *lun,
	char *page)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_show_tg_pt_gp_info(lun->lun_sep, page);
}

static ssize_t target_fabric_port_store_attr_alua_tg_pt_gp(
	struct se_lun_s *lun,
	const char *page,
	size_t count)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_store_tg_pt_gp_info(lun->lun_sep, page, count);
}

TCM_PORT_ATTR(alua_tg_pt_gp, S_IRUGO | S_IWUSR);

/*
 * alua_tg_pt_offline
 */
static ssize_t target_fabric_port_show_attr_alua_tg_pt_offline(
	struct se_lun_s *lun,
	char *page)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_show_offline_bit(lun, page);
}

static ssize_t target_fabric_port_store_attr_alua_tg_pt_offline(
	struct se_lun_s *lun,
	const char *page,
	size_t count)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_store_offline_bit(lun, page, count);
}

TCM_PORT_ATTR(alua_tg_pt_offline, S_IRUGO | S_IWUSR);

/*
 * alua_tg_pt_status
 */
static ssize_t target_fabric_port_show_attr_alua_tg_pt_status(
	struct se_lun_s *lun,
	char *page)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_show_secondary_status(lun, page);
}

static ssize_t target_fabric_port_store_attr_alua_tg_pt_status(
	struct se_lun_s *lun,
	const char *page,
	size_t count)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_store_secondary_status(lun, page, count);
}

TCM_PORT_ATTR(alua_tg_pt_status, S_IRUGO | S_IWUSR);

/*
 * alua_tg_pt_write_md
 */
static ssize_t target_fabric_port_show_attr_alua_tg_pt_write_md(
	struct se_lun_s *lun,
	char *page)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_show_secondary_write_metadata(lun, page);
}

static ssize_t target_fabric_port_store_attr_alua_tg_pt_write_md(
	struct se_lun_s *lun,
	const char *page,
	size_t count)
{
	if (!(lun->lun_sep))
		return -ENODEV;

	return core_alua_store_secondary_write_metadata(lun, page, count);
}

TCM_PORT_ATTR(alua_tg_pt_write_md, S_IRUGO | S_IWUSR);


static struct configfs_attribute *target_fabric_port_attrs[] = {
	&target_fabric_port_alua_tg_pt_gp.attr,
	&target_fabric_port_alua_tg_pt_offline.attr,
	&target_fabric_port_alua_tg_pt_status.attr,
	&target_fabric_port_alua_tg_pt_write_md.attr,
	NULL,
};

CONFIGFS_EATTR_OPS(target_fabric_port, se_lun_s, lun_group);

static int target_fabric_port_link(
	struct config_item *lun_ci,
	struct config_item *se_dev_ci)
{
	struct config_item *tpg_ci;
	struct se_device_s *dev;
	struct se_lun_s *lun = container_of(to_config_group(lun_ci),
				struct se_lun_s, lun_group);
	struct se_lun_s *lun_p;
	struct se_portal_group_s *se_tpg;
	struct se_subsystem_dev_s *se_dev = container_of(
				to_config_group(se_dev_ci), se_subsystem_dev_t,
				se_dev_group);
	struct target_fabric_configfs *tf;
	int ret;

	tpg_ci = &lun_ci->ci_parent->ci_group->cg_item;
	se_tpg = container_of(to_config_group(tpg_ci),
				struct se_portal_group_s, tpg_group);
	tf = se_tpg->se_tpg_wwn->wwn_tf;

	if (lun->lun_type_ptr != NULL) {
		printk(KERN_ERR "Port Symlink already exists\n");
		return -EEXIST;
	}
	
	dev = se_dev->se_dev_ptr;
	if (!(dev)) {
		printk(KERN_ERR "Unable to locate se_device_t pointer from"
			" %s\n", config_item_name(se_dev_ci));
		ret = -ENODEV;
		goto out;
	}

	lun_p = core_dev_add_lun(se_tpg, dev->se_hba, dev,
				lun->unpacked_lun);
	if ((IS_ERR(lun_p)) || !(lun_p)) {
		printk(KERN_ERR "core_dev_add_lun() failed\n");
		ret = -EINVAL;
		goto out;
	}

	if (tf->tf_ops.fabric_post_link) {
		/*
		 * Call the optional fabric_post_link() to allow a
		 * fabric module to setup any additional state once
		 * core_dev_add_lun() has been called..
		 */
		tf->tf_ops.fabric_post_link(se_tpg, lun);
	}

	return 0;
out:
	return ret;
}

static int target_fabric_port_check_link(
	struct config_item *lun_ci,
	struct config_item *se_dev_ci)
{
	struct se_lun_s *lun = container_of(to_config_group(lun_ci),
				struct se_lun_s, lun_group);

	return atomic_read(&lun->lun_acl_count) ? -EPERM : 0;
}

static int target_fabric_port_unlink(
	struct config_item *lun_ci,
	struct config_item *se_dev_ci)
{
	struct se_lun_s *lun = container_of(to_config_group(lun_ci),
				struct se_lun_s, lun_group);
	struct se_portal_group_s *se_tpg = lun->lun_sep->sep_tpg;
	struct target_fabric_configfs *tf = se_tpg->se_tpg_wwn->wwn_tf;

	if (tf->tf_ops.fabric_pre_unlink) {
		/*
		 * Call the optional fabric_pre_unlink() to allow a
		 * fabric module to release any additional stat before
		 * core_dev_del_lun() is called.
		*/
		tf->tf_ops.fabric_pre_unlink(se_tpg, lun);
	}

	core_dev_del_lun(se_tpg, lun->unpacked_lun);
	return 0;
}

static struct configfs_item_operations target_fabric_port_item_ops = {
	.show_attribute		= target_fabric_port_attr_show,
	.store_attribute	= target_fabric_port_attr_store,
	.allow_link		= target_fabric_port_link,
	.check_link		= target_fabric_port_check_link,
	.drop_link		= target_fabric_port_unlink,
};

TF_CIT_SETUP(tpg_port, &target_fabric_port_item_ops, NULL, target_fabric_port_attrs);

/* End of tfc_tpg_port_cit */

/* Start of tfc_tpg_lun_cit */

static struct config_group *target_fabric_make_lun(
	struct config_group *group,
	const char *name)
{
	struct se_lun_s *lun;
	struct se_portal_group_s *se_tpg = container_of(group,
			struct se_portal_group_s, tpg_lun_group);
	struct target_fabric_configfs *tf = se_tpg->se_tpg_wwn->wwn_tf;
	
	char *str, *endptr;
	u32 unpacked_lun;
	int ret;

	str = strstr(name, "_");
	if (!(str)) {
		printk(KERN_ERR "Unable to locate \'_\" in"
				" \"lun_$LUN_NUMBER\"\n");
		return NULL;
	}
	str++; /* Advance over _ delim.. */
	unpacked_lun = simple_strtoul(str, &endptr, 0);

	lun = core_get_lun_from_tpg(se_tpg, unpacked_lun);
	if (!(lun))
		return ERR_PTR(-EINVAL);

	config_group_init_type_name(&lun->lun_group, name,
			&TF_CIT_TMPL(tf)->tfc_tpg_port_cit);

	return &lun->lun_group;
}

static void target_fabric_drop_lun(
	struct config_group *group,
	struct config_item *item)
{
	struct se_portal_group_s *se_tpg = container_of(group,
			struct se_portal_group_s, tpg_lun_group);
	struct se_lun_s *lun = container_of(to_config_group(item),
			se_lun_t, lun_group);

	config_item_put(item);
}

static struct configfs_group_operations target_fabric_lun_group_ops = {
	.make_group	= &target_fabric_make_lun,
	.drop_item	= &target_fabric_drop_lun,
};

TF_CIT_SETUP(tpg_lun, NULL, &target_fabric_lun_group_ops, NULL);

/* End of tfc_tpg_lun_cit */

/* Start of tfc_tpg_attrib_cit */

CONFIGFS_EATTR_OPS(target_fabric_tpg_attrib, se_portal_group_s, tpg_attrib_group);

static struct configfs_item_operations target_fabric_tpg_attrib_item_ops = {
	.show_attribute		= target_fabric_tpg_attrib_attr_show,
	.store_attribute	= target_fabric_tpg_attrib_attr_store,
};

TF_CIT_SETUP(tpg_attrib, &target_fabric_tpg_attrib_item_ops, NULL, NULL);

/* End of tfc_tpg_attrib_cit */

/* Start of tfc_tpg_param_cit */

CONFIGFS_EATTR_OPS(target_fabric_tpg_param, se_portal_group_s, tpg_param_group);

static struct configfs_item_operations target_fabric_tpg_param_item_ops = {
	.show_attribute		= target_fabric_tpg_param_attr_show,
	.store_attribute	= target_fabric_tpg_param_attr_store,
};

TF_CIT_SETUP(tpg_param, &target_fabric_tpg_param_item_ops, NULL, NULL);

/* End of tfc_tpg_param_cit */

/* Start of tfc_tpg_base_cit */
/*
 * For use with TF_TPG_ATTR() and TF_TPG_ATTR_RO()
 */
CONFIGFS_EATTR_OPS(target_fabric_tpg, se_portal_group_s, tpg_group);

static struct configfs_item_operations target_fabric_tpg_base_item_ops = {
	.show_attribute		= target_fabric_tpg_attr_show,
	.store_attribute	= target_fabric_tpg_attr_store,
};

TF_CIT_SETUP(tpg_base, &target_fabric_tpg_base_item_ops, NULL, NULL);

/* End of tfc_tpg_base_cit */

/* Start of tfc_tpg_cit */

static struct config_group *target_fabric_make_tpg(
	struct config_group *group,
	const char *name)
{
	struct se_wwn_s *wwn = container_of(group, struct se_wwn_s, wwn_group);
	struct target_fabric_configfs *tf = wwn->wwn_tf;
	struct se_portal_group_s *se_tpg;

	if (!(tf->tf_ops.fabric_make_tpg)) {
		printk(KERN_ERR "tf->tf_ops.fabric_make_tpg is NULL\n");
		return ERR_PTR(-ENOSYS);
	}

	se_tpg = tf->tf_ops.fabric_make_tpg(wwn, group, name);
	if (!(se_tpg) || IS_ERR(se_tpg))
		return ERR_PTR(-EINVAL);
	/*
	 * Setup default groups from pre-allocated se_tpg->tpg_default_groups
	 */
	se_tpg->tpg_group.default_groups = se_tpg->tpg_default_groups;
	se_tpg->tpg_group.default_groups[0] = &se_tpg->tpg_lun_group;
	se_tpg->tpg_group.default_groups[1] = &se_tpg->tpg_np_group;
	se_tpg->tpg_group.default_groups[2] = &se_tpg->tpg_acl_group;
	se_tpg->tpg_group.default_groups[3] = &se_tpg->tpg_attrib_group;
	se_tpg->tpg_group.default_groups[4] = &se_tpg->tpg_param_group;
	se_tpg->tpg_group.default_groups[5] = NULL;

	config_group_init_type_name(&se_tpg->tpg_group, name,
			&TF_CIT_TMPL(tf)->tfc_tpg_base_cit);
	config_group_init_type_name(&se_tpg->tpg_lun_group, "lun",
			&TF_CIT_TMPL(tf)->tfc_tpg_lun_cit);
	config_group_init_type_name(&se_tpg->tpg_np_group, "np",
			&TF_CIT_TMPL(tf)->tfc_tpg_np_cit);
	config_group_init_type_name(&se_tpg->tpg_acl_group, "acls",
			&TF_CIT_TMPL(tf)->tfc_tpg_acl_cit);
	config_group_init_type_name(&se_tpg->tpg_attrib_group, "attrib",
			&TF_CIT_TMPL(tf)->tfc_tpg_attrib_cit);
	config_group_init_type_name(&se_tpg->tpg_param_group, "param",
			&TF_CIT_TMPL(tf)->tfc_tpg_param_cit);
			
	return &se_tpg->tpg_group;
}

static void target_fabric_drop_tpg(
	struct config_group *group,
	struct config_item *item)
{
	struct se_wwn_s *wwn = container_of(group, struct se_wwn_s, wwn_group);
	struct target_fabric_configfs *tf = wwn->wwn_tf;
	struct se_portal_group_s *se_tpg = container_of(to_config_group(item),
				struct se_portal_group_s, tpg_group);
	struct config_group *tpg_cg = &se_tpg->tpg_group;
	struct config_item *df_item;
	int i;
	/*
	 * Release default groups, but do not release tpg_cg->default_groups
	 * memory as it is statically allocated at se_tpg->tpg_default_groups.
	 */
	for (i = 0; tpg_cg->default_groups[i]; i++) {
		df_item = &tpg_cg->default_groups[i]->cg_item;
		tpg_cg->default_groups[i] = NULL;
		config_item_put(df_item);
	}

	config_item_put(item);
	tf->tf_ops.fabric_drop_tpg(se_tpg);	
}

static struct configfs_group_operations target_fabric_tpg_group_ops = {
	.make_group	= target_fabric_make_tpg,
	.drop_item	= target_fabric_drop_tpg,
};

TF_CIT_SETUP(tpg, NULL, &target_fabric_tpg_group_ops, NULL);

/* End of tfc_tpg_cit */

/* Start of tfc_wwn_cit */

static struct config_group *target_fabric_make_wwn(
	struct config_group *group,
	const char *name)
{
	struct target_fabric_configfs *tf = container_of(group,
				struct target_fabric_configfs, tf_group);
	struct se_wwn_s *wwn;

	if (!(tf->tf_ops.fabric_make_wwn)) {
		printk(KERN_ERR "tf->tf_ops.fabric_make_wwn is NULL\n");
		return ERR_PTR(-ENOSYS);
	}

	wwn = tf->tf_ops.fabric_make_wwn(tf, group, name);
	if (!(wwn) || IS_ERR(wwn))
		return ERR_PTR(-EINVAL);

	wwn->wwn_tf = tf;
	config_group_init_type_name(&wwn->wwn_group, name,
			&TF_CIT_TMPL(tf)->tfc_tpg_cit);

	return &wwn->wwn_group;
}

static void target_fabric_drop_wwn(
	struct config_group *group,
	struct config_item *item)
{
	struct target_fabric_configfs *tf = container_of(group,
				struct target_fabric_configfs, tf_group);
	struct se_wwn_s *wwn = container_of(to_config_group(item),
				se_wwn_t, wwn_group);
	
	config_item_put(item);
	tf->tf_ops.fabric_drop_wwn(wwn);
}

static struct configfs_group_operations target_fabric_wwn_group_ops = {
	.make_group	= target_fabric_make_wwn,
	.drop_item	= target_fabric_drop_wwn,
};
/*
 * For use with TF_WWN_ATTR() and TF_WWN_ATTR_RO()
 */
CONFIGFS_EATTR_OPS(target_fabric_wwn, target_fabric_configfs, tf_group);

static struct configfs_item_operations target_fabric_wwn_item_ops = {
	.show_attribute		= target_fabric_wwn_attr_show,
	.store_attribute	= target_fabric_wwn_attr_store,
};	

TF_CIT_SETUP(wwn, &target_fabric_wwn_item_ops, &target_fabric_wwn_group_ops, NULL);

/* End of tfc_wwn_cit */

int target_fabric_setup_cits(struct target_fabric_configfs *tf)
{
	target_fabric_setup_wwn_cit(tf);	
	target_fabric_setup_tpg_cit(tf);
	target_fabric_setup_tpg_base_cit(tf);
	target_fabric_setup_tpg_port_cit(tf);
	target_fabric_setup_tpg_lun_cit(tf);
	target_fabric_setup_tpg_np_cit(tf);
	target_fabric_setup_tpg_np_base_cit(tf);
	target_fabric_setup_tpg_attrib_cit(tf);
	target_fabric_setup_tpg_param_cit(tf);
	target_fabric_setup_tpg_acl_cit(tf);

	return 0;
}
