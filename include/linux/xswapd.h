/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/xswapd.h
 *
 * Header definitions for Xiaomi HyperOS xswapd & memcg_xswapd.
 */
#ifndef _LINUX_XSWAPD_H
#define _LINUX_XSWAPD_H

#include <linux/types.h>

struct mem_cgroup;

#ifdef CONFIG_MEMCG
void xswapd_wakeup(void);
bool xswapd_is_enabled(void);
void xswapd_record_refault(struct mem_cgroup *memcg);
#else
static inline void xswapd_wakeup(void) {}
static inline bool xswapd_is_enabled(void) { return false; }
static inline void xswapd_record_refault(struct mem_cgroup *memcg) {}
#endif

#endif /* _LINUX_XSWAPD_H */
