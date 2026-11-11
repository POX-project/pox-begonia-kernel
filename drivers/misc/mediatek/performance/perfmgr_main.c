/*
 * Copyright (C) 2018 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <mt-plat/fpsgo_common.h>

#include "tchbst.h"
#include "io_ctrl.h"
#include "boost_ctrl.h"
#include "mtk_perfmgr_internal.h"
#include "topo_ctrl.h"
#include "uload_ind.h"
#include "syslimiter.h"


int clstr_num;

static int perfmgr_probe(struct platform_device *dev)
{
	return 0;
}

struct platform_device perfmgr_device = {
	.name   = "perfmgr",
	.id        = -1,
};

static int perfmgr_suspend(struct device *dev)
{
#ifdef CONFIG_MTK_PERFMGR_TOUCH_BOOST
	ktch_suspend();
#endif
	return 0;
}

static int perfmgr_resume(struct device *dev)
{
	return 0;
}
static int perfmgr_remove(struct platform_device *dev)
{

	/*TODO: workaround for k414
	 * topo_ctrl_exit();
	 */
	cpu_ctrl_exit();
	syslimiter_exit();

	return 0;
}
static struct platform_driver perfmgr_driver = {
	.probe      = perfmgr_probe,
	.remove     = perfmgr_remove,
	.driver     = {
		.name = "perfmgr",
		.pm = &(const struct dev_pm_ops){
			.suspend = perfmgr_suspend,
			.resume = perfmgr_resume,
		},
	},
};

static int perfmgr_main_data_init(void)
{

	/* get cluster number from topo_ctrl */
	clstr_num = topo_ctrl_get_nr_clusters();

	return 0;
}

static int gaming_mode;
static DEFINE_MUTEX(gaming_mode_lock);

static int perfmgr_gaming_mode_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", READ_ONCE(gaming_mode));
	return 0;
}

static int perfmgr_gaming_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, perfmgr_gaming_mode_show, PDE_DATA(inode));
}

static ssize_t perfmgr_gaming_mode_write(struct file *filp, const char __user *ubuf,
					size_t cnt, loff_t *data)
{
	char buf[32];
	int val, ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (cnt == 0 || cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = '\0';
	ret = kstrtoint(strim(buf), 10, &val);
	if (ret)
		return ret;

	if (val < 0 || val > 2)
		return -EINVAL;

	mutex_lock(&gaming_mode_lock);
	WRITE_ONCE(gaming_mode, val);
	if (val == 0) {
		/* Balanced mode: standard frame margins */
		fbt_cpu_set_ultra_rescue(0);
		fbt_cpu_set_rescue_percent(33);
		fbt_cpu_set_variance(40);
	} else {
		/* Gaming (1) & Turbo (2): 20% deadline rescue with 15 variance */
		fbt_cpu_set_ultra_rescue(1);
		fbt_cpu_set_rescue_percent(20);
		fbt_cpu_set_variance(15);
	}
	pr_info("[perfmgr] gaming_mode set to %d\n", val);
	mutex_unlock(&gaming_mode_lock);

	return cnt;
}

static const struct file_operations perfmgr_gaming_mode_fops = {
	.open = perfmgr_gaming_mode_open,
	.read = seq_read,
	.write = perfmgr_gaming_mode_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/*--------------------INIT------------------------*/

static int __init init_perfmgr(void)
{
	struct proc_dir_entry *perfmgr_root = NULL;
	int ret = 0;

	ret = platform_device_register(&perfmgr_device);
	if (ret)
		return ret;
	ret = platform_driver_register(&perfmgr_driver);
	if (ret)
		return ret;

	perfmgr_main_data_init();

	perfmgr_root = proc_mkdir("perfmgr", NULL);
	pr_debug("MTK_TOUCH_BOOST function init_perfmgr_touch\n");

	if (perfmgr_root)
		proc_create("gaming_mode", 0644, perfmgr_root,
			    &perfmgr_gaming_mode_fops);

#ifdef CONFIG_MTK_BASE_POWER
	init_tchbst(perfmgr_root);
	init_boostctrl(perfmgr_root);
	syslimiter_init(perfmgr_root);
#endif
	init_perfctl(perfmgr_root);

#ifdef CONFIG_MTK_LOAD_TRACKER
	init_uload_ind(NULL);
#endif
	return 0;
}
device_initcall(init_perfmgr);
