// SPDX-License-Identifier: GPL-2.0
/*
 * mm/xswapd.c
 *
 * Xiaomi HyperOS / HyperCore xswapd (Xiaomi Swap Daemon) &
 * memcg_xswapd per-cgroup asynchronous page reclamation engine.
 *
 * Copyright (C) 2026 Pox Project / HyperCore Contributors
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/wait.h>
#include <linux/swap.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/xswapd.h>

static struct task_struct *xswapd_task;
static DECLARE_WAIT_QUEUE_HEAD(xswapd_wait);

/* Tunables */
static unsigned int xswapd_enabled = 1;
static unsigned long xswapd_watermark_kb;
static unsigned int xswapd_min_score = 1; /* Skip score 0 (foreground apps) */
static unsigned long xswapd_max_scan_pages = 32768; /* 128MB per cycle */
static unsigned long xswapd_refault_threshold = 512;
static unsigned int xswapd_interval_ms = 1000;

/* Internal triggers & stats */
static atomic_t xswapd_wake_req = ATOMIC_INIT(0);
static atomic_t xswapd_force_trigger = ATOMIC_INIT(0);
static atomic64_t xswapd_stat_runs = ATOMIC64_INIT(0);
static atomic64_t xswapd_stat_wakeups = ATOMIC64_INIT(0);
static atomic64_t xswapd_stat_thrash_skips = ATOMIC64_INIT(0);
static atomic64_t xswapd_stat_reclaimed_pages = ATOMIC64_INIT(0);

static unsigned long xswapd_get_available_pages(void)
{
	unsigned long free_pages = global_zone_page_state(NR_FREE_PAGES);
	unsigned long inactive_file = global_node_page_state(NR_INACTIVE_FILE);
	unsigned long reclaimable_slab = global_node_page_state(NR_SLAB_RECLAIMABLE);

	return free_pages + inactive_file + reclaimable_slab;
}

static bool xswapd_watermark_needs_reclaim(void)
{
	unsigned long target_pages;
	unsigned long avail_pages;

	if (!xswapd_watermark_kb)
		return false;

	target_pages = xswapd_watermark_kb >> (PAGE_SHIFT - 10);
	avail_pages = xswapd_get_available_pages();

	return avail_pages < target_pages;
}

void xswapd_wakeup(void)
{
	if (!xswapd_enabled || !xswapd_task)
		return;

	atomic_set(&xswapd_wake_req, 1);
	atomic64_inc(&xswapd_stat_wakeups);
	wake_up_interruptible(&xswapd_wait);
}
EXPORT_SYMBOL_GPL(xswapd_wakeup);

bool xswapd_is_enabled(void)
{
	return xswapd_enabled != 0;
}
EXPORT_SYMBOL_GPL(xswapd_is_enabled);

void xswapd_record_refault(struct mem_cgroup *memcg)
{
	if (memcg)
		atomic64_inc(&memcg->xswapd_refaults);
}
EXPORT_SYMBOL_GPL(xswapd_record_refault);

static void xswapd_do_reclaim_cycle(void)
{
	struct mem_cgroup *memcg;
	unsigned long target_watermark_pages = xswapd_watermark_kb >> (PAGE_SHIFT - 10);
	unsigned long avail_pages = xswapd_get_available_pages();
	unsigned long needed_pages;
	unsigned long total_reclaimed = 0;
	bool forced = (atomic_xchg(&xswapd_force_trigger, 0) != 0);

	if (avail_pages >= target_watermark_pages && !forced)
		return;

	needed_pages = (target_watermark_pages > avail_pages) ?
		       (target_watermark_pages - avail_pages) :
		       (xswapd_max_scan_pages / 2);
	needed_pages = min_t(unsigned long, needed_pages, xswapd_max_scan_pages);

	atomic64_inc(&xswapd_stat_runs);

	for (memcg = mem_cgroup_iter(NULL, NULL, NULL);
	     memcg != NULL;
	     memcg = mem_cgroup_iter(NULL, memcg, NULL)) {
		unsigned long batch;
		unsigned long nr;
		u64 cur_refaults;

		if (mem_cgroup_is_root(memcg))
			continue;

		if (!memcg->xswapd_enable)
			continue;

		/* App state / score filtering: skip foreground apps (score 0) */
		if (memcg->xswapd_score < xswapd_min_score)
			continue;

		/* Thrashing prevention (refault rate check) */
		cur_refaults = atomic64_read(&memcg->xswapd_refaults);
		if ((cur_refaults - memcg->xswapd_last_refaults) > xswapd_refault_threshold) {
			atomic64_inc(&xswapd_stat_thrash_skips);
			memcg->xswapd_last_refaults = cur_refaults;
			continue;
		}
		memcg->xswapd_last_refaults = cur_refaults;

		batch = min_t(unsigned long, needed_pages - total_reclaimed,
			      SWAP_CLUSTER_MAX * 8);

		nr = try_to_free_mem_cgroup_pages(memcg, batch, GFP_KERNEL, true);
		if (nr > 0) {
			atomic64_add(nr, &memcg->xswapd_reclaimed);
			atomic64_add(nr, &xswapd_stat_reclaimed_pages);
			total_reclaimed += nr;
		}

		if (total_reclaimed >= needed_pages) {
			mem_cgroup_iter_break(NULL, memcg);
			break;
		}

		cond_resched();
	}
}

static int xswapd_thread(void *data)
{
	set_user_nice(current, 5);

	while (!kthread_should_stop()) {
		wait_event_freezable_timeout(
			xswapd_wait,
			atomic_read(&xswapd_wake_req) ||
			atomic_read(&xswapd_force_trigger) ||
			xswapd_watermark_needs_reclaim() ||
			kthread_should_stop(),
			msecs_to_jiffies(xswapd_interval_ms)
		);

		if (kthread_should_stop())
			break;

		atomic_set(&xswapd_wake_req, 0);

		if (xswapd_enabled)
			xswapd_do_reclaim_cycle();
	}

	return 0;
}

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", xswapd_enabled);
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	xswapd_enabled = val ? 1 : 0;
	if (xswapd_enabled)
		xswapd_wakeup();

	return count;
}
static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);

static ssize_t trigger_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&xswapd_force_trigger));
}

static ssize_t trigger_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	atomic_set(&xswapd_force_trigger, 1);
	xswapd_wakeup();
	return count;
}
static struct kobj_attribute trigger_attr = __ATTR_RW(trigger);

static ssize_t watermark_kb_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%lu\n", xswapd_watermark_kb);
}

static ssize_t watermark_kb_store(struct kobject *kobj, struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned long val;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	xswapd_watermark_kb = val;
	return count;
}
static struct kobj_attribute watermark_kb_attr = __ATTR_RW(watermark_kb);

static ssize_t min_score_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", xswapd_min_score);
}

static ssize_t min_score_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;
	if (val > 100)
		return -EINVAL;

	xswapd_min_score = val;
	return count;
}
static struct kobj_attribute min_score_attr = __ATTR_RW(min_score);

static ssize_t max_scan_pages_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%lu\n", xswapd_max_scan_pages);
}

static ssize_t max_scan_pages_store(struct kobject *kobj, struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned long val;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;
	if (!val)
		return -EINVAL;

	xswapd_max_scan_pages = min_t(unsigned long, val, 65536UL);
	return count;
}
static struct kobj_attribute max_scan_pages_attr = __ATTR_RW(max_scan_pages);

static ssize_t refault_threshold_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%lu\n", xswapd_refault_threshold);
}

static ssize_t refault_threshold_store(struct kobject *kobj, struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned long val;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	xswapd_refault_threshold = val;
	return count;
}
static struct kobj_attribute refault_threshold_attr = __ATTR_RW(refault_threshold);

static ssize_t interval_ms_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", xswapd_interval_ms);
}

static ssize_t interval_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;
	if (val < 100 || val > 60000)
		return -EINVAL;

	xswapd_interval_ms = val;
	return count;
}
static struct kobj_attribute interval_ms_attr = __ATTR_RW(interval_ms);

static ssize_t reclaimed_pages_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%llu\n",
			 (unsigned long long)atomic64_read(&xswapd_stat_reclaimed_pages));
}
static struct kobj_attribute reclaimed_pages_attr = __ATTR_RO(reclaimed_pages);

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned long avail_kb = xswapd_get_available_pages() << (PAGE_SHIFT - 10);
	unsigned long long reclaimed_mb =
		(atomic64_read(&xswapd_stat_reclaimed_pages) << PAGE_SHIFT) >> 20;

	return scnprintf(buf, PAGE_SIZE,
			 "state: %s\n"
			 "enabled: %u\n"
			 "runs: %llu\n"
			 "wakeups: %llu\n"
			 "thrash_skips: %llu\n"
			 "reclaimed_mb: %llu\n"
			 "available_kb: %lu\n"
			 "watermark_kb: %lu\n",
			 xswapd_task ? "running" : "stopped",
			 xswapd_enabled,
			 (unsigned long long)atomic64_read(&xswapd_stat_runs),
			 (unsigned long long)atomic64_read(&xswapd_stat_wakeups),
			 (unsigned long long)atomic64_read(&xswapd_stat_thrash_skips),
			 reclaimed_mb,
			 avail_kb,
			 xswapd_watermark_kb);
}
static struct kobj_attribute status_attr = __ATTR_RO(status);

static struct attribute *xswapd_attrs[] = {
	&enabled_attr.attr,
	&trigger_attr.attr,
	&watermark_kb_attr.attr,
	&min_score_attr.attr,
	&max_scan_pages_attr.attr,
	&refault_threshold_attr.attr,
	&interval_ms_attr.attr,
	&reclaimed_pages_attr.attr,
	&status_attr.attr,
	NULL,
};

static const struct attribute_group xswapd_attr_group = {
	.attrs = xswapd_attrs,
};

static struct kobject *xswapd_kobj;

static int __init xswapd_init(void)
{
	int ret;
	unsigned long default_wm_pages;

	/* Default watermark: 12% of total RAM, clamped between 256MB and 1GB */
	default_wm_pages = (totalram_pages * 12) / 100;
	default_wm_pages = clamp_t(unsigned long, default_wm_pages,
				   (256UL * 1024) >> (PAGE_SHIFT - 10),
				   (1024UL * 1024) >> (PAGE_SHIFT - 10));
	xswapd_watermark_kb = default_wm_pages << (PAGE_SHIFT - 10);

	xswapd_kobj = kobject_create_and_add("xswapd", kernel_kobj);
	if (xswapd_kobj) {
		ret = sysfs_create_group(xswapd_kobj, &xswapd_attr_group);
		if (ret)
			pr_warn("xswapd: failed to create sysfs group: %d\n", ret);
	}

	xswapd_task = kthread_run(xswapd_thread, NULL, "xswapd");
	if (IS_ERR(xswapd_task)) {
		pr_err("xswapd: failed to spawn kernel daemon\n");
		ret = PTR_ERR(xswapd_task);
		xswapd_task = NULL;
		return ret;
	}

	pr_info("xswapd: Xiaomi HyperCore xswapd daemon initialized (watermark=%lu KB)\n",
		xswapd_watermark_kb);

	return 0;
}
late_initcall(xswapd_init);
