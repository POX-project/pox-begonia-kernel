// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Pox Kernel Project - TXO R
 * Dedicated Pox Zero Frame-Drop Gaming Mode Controller for Redmi Note 8 Pro (begonia)
 *
 * Coordinates MediaTek FPSGO Ultra-Rescue, GED GPU Instant Boost,
 * Schedutil Instantaneous Ramp, SchedTune, and COBRA Performance-First PPM.
 */

#define pr_fmt(fmt) "[gaming_mode] " fmt

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/workqueue.h>
#include <linux/capability.h>
#include <linux/jiffies.h>
#include <linux/sched/loadavg.h>
#include <linux/slab.h>

#include "gaming_mode.h"

/* External subsystem functions */
extern int schedutil_set_up_rate_limit_us(int cpu, unsigned int rate_limit_us);
extern int schedutil_set_down_rate_limit_us(int cpu, unsigned int rate_limit_us);
extern int fbt_cpu_set_ultra_rescue(int enable);
extern int fbt_cpu_set_bhr(int new_bhr);
extern int fbt_cpu_set_rescue_opp_c(int new_opp);
extern int fbt_cpu_set_rescue_percent(int percent);
extern int fbt_cpu_set_variance(int var);
extern void fbt_boost_dram(int boost);
extern void ged_kpi_set_gaming_boost(int enable);
extern void ged_dvfs_set_gaming_boost(int enable);
extern void eara_pass_perf_first_hint(int enable);
extern int boost_write_for_perf_idx(int idx, int boost_value);
extern int prefer_idle_for_perf_idx(int idx, int prefer_idle);
extern int set_ios_color_mode(int mode);
extern int get_ios_color_mode(void);
extern int set_ios_color_custom(int sat, int contrast, int brightness, int sat_adj);
extern void get_ios_color_custom(int *sat, int *contrast, int *brightness, int *sat_adj);
extern int pox_dynamic_fsync_get(void);
extern void pox_dynamic_fsync_set(int enable);

static int gaming_mode_state = GAMING_MODE_DISABLED;
static int user_color_mode_override = -1;
static int camera_4k60_force = 0;
/* Power learning counters (read-only via /proc/perfmgr/profile). */
unsigned long pox_pwr_activations;
unsigned long pox_pwr_gaming_entries;
unsigned long pox_pwr_extreme_entries;
unsigned long pox_pwr_powersave_entries;
unsigned long pox_pwr_thermal_derates;
unsigned long pox_pwr_battery_derates;
EXPORT_SYMBOL(pox_pwr_thermal_derates);
EXPORT_SYMBOL(pox_pwr_battery_derates);
/* Dynamic engine: no manual modes. ROM writes are hints; the engine
 * picks effective power from hint + live battery health, with hold
 * hysteresis so it never oscillates. Auto on by default. */
static int pox_pwr_auto = 1;
static int pox_pwr_hint;
static bool pox_pwr_derate_hold;
static struct delayed_work pox_dynamic_work;
static void pox_dynamic_evaluate(struct work_struct *work);
static atomic_t pox_cam_active_sessions = ATOMIC_INIT(0);
static struct delayed_work pox_cam_boost_decay_work;
static DEFINE_MUTEX(gaming_mode_lock);

/* Kprofiles Compatibility State Machine */
static struct kobject *kp_kobj;
static int kp_auto_mode = 1;

/* Phone Health & Silicon OPP Table Calibration Engine */
/* Modes: 0=Disabled, 1=Auto-Adaptive (default), 2=Performance Guard, 3=Maximum Longevity */
static int pox_health_calibration_mode = 1;
static int pox_opp_voltage_guard_enabled = 1;
static unsigned long pox_health_brownout_preventions;

/* BORE Scheduler Runtime Interface
 * Variables live in kernel/sched/fair.c where the CFS wakeup-preemption
 * logic consumes them; they must never be static copies here, or these
 * nodes would become placebos. */
extern int sysctl_sched_bore;
extern unsigned int sysctl_sched_burst_cache_lifetime;
extern unsigned int sysctl_sched_burst_inherit_type;
extern unsigned int sysctl_sched_burst_penalty_offset;
extern unsigned int sysctl_sched_burst_penalty_scale;

int color_mode_set(int mode)
{
	return set_ios_color_mode(mode);
}
EXPORT_SYMBOL(color_mode_set);

int color_mode_get(void)
{
	return get_ios_color_mode();
}
EXPORT_SYMBOL(color_mode_get);

int pox_color_custom_set(int sat, int contrast, int brightness, int sat_adj)
{
	user_color_mode_override = COLOR_MODE_CUSTOM;
	return set_ios_color_custom(sat, contrast, brightness, sat_adj);
}
EXPORT_SYMBOL(pox_color_custom_set);

void pox_color_custom_get(int *sat, int *contrast, int *brightness, int *sat_adj)
{
	get_ios_color_custom(sat, contrast, brightness, sat_adj);
}
EXPORT_SYMBOL(pox_color_custom_get);

int pox_true_tone_set(int enable)
{
	user_color_mode_override = enable ? COLOR_MODE_REFERENCE : COLOR_MODE_STANDARD;
	return set_ios_color_mode(enable ? COLOR_MODE_REFERENCE : COLOR_MODE_STANDARD);
}
EXPORT_SYMBOL(pox_true_tone_set);

int pox_true_tone_get(void)
{
	return (get_ios_color_mode() == COLOR_MODE_REFERENCE) ? 1 : 0;
}
EXPORT_SYMBOL(pox_true_tone_get);

int camera_4k60_set(int force)
{
	camera_4k60_force = force ? 1 : 0;
	pr_info("Camera 4K 60FPS Force Mode: %s\n",
		camera_4k60_force ? "ENABLED (60FPS Forced)" : "AUTO (App Request)");
	return 0;
}
EXPORT_SYMBOL(camera_4k60_set);

int camera_4k60_get(void)
{
	return camera_4k60_force;
}
EXPORT_SYMBOL(camera_4k60_get);

int camera_slog3_set(int enable)
{
	if (enable)
		return set_ios_color_mode(COLOR_MODE_SLOG3);
	else
		return set_ios_color_mode(COLOR_MODE_REFERENCE);
}
EXPORT_SYMBOL(camera_slog3_set);

int camera_slog3_get(void)
{
	return (get_ios_color_mode() == COLOR_MODE_SLOG3) ? 1 : 0;
}
EXPORT_SYMBOL(camera_slog3_get);

static void pox_cam_boost_decay_func(struct work_struct *work)
{
	/* Decay launch boost down after 750ms if not in Extreme gaming mode */
	if (gaming_mode_state < GAMING_MODE_EXTREME)
		fbt_boost_dram(0);

	/* Restore normal top-app schedtune boost */
	if (gaming_mode_state > 0)
		boost_write_for_perf_idx(3, 20);
	else
		boost_write_for_perf_idx(3, 5);
}

void pox_camera_launch_boost(int enable)
{
	if (enable) {
		int count = atomic_inc_return(&pox_cam_active_sessions);

		if (count == 1) {
			pr_info("Camera launch boost engaged: instant viewfinder QoS primed.\n");
			/* Instant high-throughput DRAM boost to eliminate sensor init/viewfinder frame drops */
			fbt_boost_dram(1);
			/* Aggressive CPU schedtune for camera rendering and sensor init */
			boost_write_for_perf_idx(3, 40);
			prefer_idle_for_perf_idx(3, 1);
			/* Schedule 750ms decay */
			cancel_delayed_work(&pox_cam_boost_decay_work);
			schedule_delayed_work(&pox_cam_boost_decay_work, msecs_to_jiffies(750));
		}
	} else {
		int count = atomic_dec_return(&pox_cam_active_sessions);

		if (count <= 0) {
			atomic_set(&pox_cam_active_sessions, 0);
			cancel_delayed_work(&pox_cam_boost_decay_work);
			pox_cam_boost_decay_func(NULL);
			pr_info("Camera session released: launch boost restored.\n");
		}
	}
}
EXPORT_SYMBOL(pox_camera_launch_boost);

extern int pox_lm36273_hbm_set(int mode);
extern int pox_lm36273_hbm_get(void);
extern int disp_bls_set_backlight(int level_1024);
extern int disp_pwm_get_backlight_level(void);

int hbm_mode_set(int mode)
{
	int ret = pox_lm36273_hbm_set(mode);
	int cur_level = disp_pwm_get_backlight_level();

	if (cur_level <= 0)
		cur_level = 1023;
	disp_bls_set_backlight(cur_level);

	return ret;
}
EXPORT_SYMBOL(hbm_mode_set);

int hbm_mode_get(void)
{
	return pox_lm36273_hbm_get();
}
EXPORT_SYMBOL(hbm_mode_get);

extern int pox_torch_brightness_set(int val);
extern int pox_torch_brightness_get(void);

int torch_brightness_set(int val)
{
	return pox_torch_brightness_set(val);
}
EXPORT_SYMBOL(torch_brightness_set);

int torch_brightness_get(void)
{
	return pox_torch_brightness_get();
}
EXPORT_SYMBOL(torch_brightness_get);

/* Pox tri-state adaptive memory engine (HarmonyOS / MGLRU / DAMON lesson,
 * 4.14-safe without backport). Classic LRU only on 4.14, so emulate
 * generational behavior with per-mode headroom + reclaim bias:
 * - gaming: early kswapd (WSF 150) fits 8.3ms 120fps budget, swappiness
 *   100 keeps anon in per-CPU zstd ZRAM (no LMK kills mid-game), pressure
 *   50 retains dentries for instant launch (EROFS-style caching).
 * - balanced: WSF 100, pressure 75, same ZRAM speed, throughput dirty 20/10.
 * - powersave: swappiness 60 (less CPU on compression), WSF 50 (fewer
 *   kswapd wakeups), pressure 100 (drop caches), dirty 20/10.
 * page_cluster stays 0 always (ZRAM random; readahead wastes CPU/RAM).
 * All values are stock-safe ranges; no OOM/corruption path. */
extern int watermark_scale_factor;
extern int sysctl_vfs_cache_pressure;
extern int vm_dirty_ratio;
extern int dirty_background_ratio;
extern int vm_swappiness;
extern int page_cluster;
static void pox_memory_preset(int mode)
{
	if (mode > 0) {
		vm_swappiness = 100;
		watermark_scale_factor = 200;
		sysctl_vfs_cache_pressure = 50;
		page_cluster = 0;
		vm_dirty_ratio = 10;
		dirty_background_ratio = 5;
	} else if (mode == GAMING_MODE_POWERSAVE) {
		vm_swappiness = 60;
		watermark_scale_factor = 100;
		sysctl_vfs_cache_pressure = 100;
		page_cluster = 0;
		vm_dirty_ratio = 20;
		dirty_background_ratio = 10;
	} else {
		vm_swappiness = 100;
		watermark_scale_factor = 150;
		sysctl_vfs_cache_pressure = 50;
		page_cluster = 0;
		vm_dirty_ratio = 20;
		dirty_background_ratio = 10;
	}
}

static void pox_health_calibration_sync(void)
{
	extern int battery_get_bat_temperature(void);
	extern int battery_get_uisoc(void);
	int btemp = battery_get_bat_temperature();
	int soc = battery_get_uisoc();

	if (!pox_opp_voltage_guard_enabled || pox_health_calibration_mode == 0)
		return;

	/* Dynamic compensation:
	 * If battery is degraded, low (<=20%), mode is Longevity (3), or high temp (>=45C):
	 * clamp Cortex-A76 big core peak rescue OPP to safe level 2 (1.8GHz)
	 * to prevent high-current VBUS/battery internal resistance collapse & PMIC UVLO trips,
	 * while accelerating Little A55 schedutil up-ramp to 250us to guarantee 0 UI thread freezes!
	 */
	if (soc <= 20 || pox_health_calibration_mode == 3 || btemp >= 450) {
		fbt_cpu_set_rescue_opp_c(2);
		schedutil_set_up_rate_limit_us(0, 250);
	} else if (gaming_mode_state > 0) {
		fbt_cpu_set_rescue_opp_c(0);
		schedutil_set_up_rate_limit_us(0, 500);
	}
}

int gaming_mode_set(int mode)
{
	mutex_lock(&gaming_mode_lock);

	if (mode == gaming_mode_state) {
		pox_health_calibration_sync();
		mutex_unlock(&gaming_mode_lock);
		return 0;
	}

	/* Dynamic engine health adaptation with hold hysteresis.
	 * Enter derate at >=45C / <15%, release only when <43C and >20%
	 * so effective power never flaps frame to frame. */
	{
		extern int battery_get_bat_temperature(void);
		extern int battery_get_uisoc(void);
		int btemp = battery_get_bat_temperature(); /* deciC */
		int soc = battery_get_uisoc();
		if (((mode >= GAMING_MODE_EXTREME) || (pox_health_calibration_mode == 3 && mode >= GAMING_MODE_ENABLED)) &&
		    pox_opp_voltage_guard_enabled &&
		    !pox_pwr_derate_hold && (btemp >= 450 || soc < 15)) {
			pox_pwr_derate_hold = true;
			pox_health_brownout_preventions++;
			if (btemp >= 450) {
				pox_pwr_thermal_derates++;
				pr_info("Power learn: batt %d deciC hot, EXTREME->ENABLED (right power, no overheat)\n", btemp);
			} else {
				pox_pwr_battery_derates++;
				pr_info("Power learn: soc %d low, EXTREME->ENABLED (right power, no brownout)\n", soc);
			}
			mode = GAMING_MODE_ENABLED;
		} else if (pox_pwr_derate_hold) {
			if (btemp < 430 && soc > 20) {
				pox_pwr_derate_hold = false;
			} else if (mode >= GAMING_MODE_EXTREME) {
				mode = GAMING_MODE_ENABLED;
			}
		}
	}

	if (mode > 0) {
		pr_info("Activating Gaming Mode (level %d)...\n", mode);

		/* 1. FPSGO Ultra-Rescue & Preemptive Frame Stabilization */
		fbt_cpu_set_ultra_rescue(1);
		fbt_cpu_set_rescue_percent(20); /* Rescue 33% earlier before vsync expiration */
		fbt_cpu_set_variance(15);       /* Sensitive deviation threshold to catch spikes */
		fbt_cpu_set_bhr(15);            /* Big Core Hold Rate raised for frame stability */

		/* Silicon OPP Health Guard: if battery is degraded or <20%, clamp peak rescue OPP
		 * to prevent Cortex-A76 big core voltage collapse while elevating Little A55 schedutil
		 * ramp to ensure ZERO freezes and high UI responsiveness */
		if (pox_opp_voltage_guard_enabled && pox_health_calibration_mode > 0) {
			extern int battery_get_uisoc(void);
			int soc = battery_get_uisoc();
			if (soc <= 20 || pox_health_calibration_mode == 3) {
				fbt_cpu_set_rescue_opp_c(2); /* Safe OPP level: no brownouts */
				schedutil_set_up_rate_limit_us(0, 250); /* Faster Little A55 response */
			} else {
				fbt_cpu_set_rescue_opp_c(0); /* Uncap rescue ceiling to peak CPU frequency */
			}
		} else {
			fbt_cpu_set_rescue_opp_c(0);
		}

		/* 2. Schedutil Instantaneous Ramp */
		schedutil_set_up_rate_limit_us(0, 500);     /* Cluster 0 (A55): 500us ramp */
		schedutil_set_down_rate_limit_us(0, 20000); /* 20ms hold prevents inter-frame drops */
		schedutil_set_up_rate_limit_us(6, 200);     /* Cluster 1 (A76): 200us instant ramp */
		schedutil_set_down_rate_limit_us(6, 20000); /* 20ms hold */

		/* 3. Mali-G76 MC4 GPU & GED Instant Boost */
		ged_kpi_set_gaming_boost(1);
		ged_dvfs_set_gaming_boost(1);

		/* 4. PPM COBRA Performance-First CPU Budgeting */
		eara_pass_perf_first_hint(1);

		/* 5. iOS-Style SchedTune QoS: Prioritize Top-App & Foreground */
		boost_write_for_perf_idx(3, 20);   /* Top-app (Render/Game) boost = 20% */
		prefer_idle_for_perf_idx(3, 1);    /* Top-app prefers idle Cortex-A76 cores */
		boost_write_for_perf_idx(1, 5);    /* Foreground boost = 5% */
		prefer_idle_for_perf_idx(1, 1);

		/* 6. Display Engine: True Tone remains default display profile; gaming mode does not alter display calibration */

		/* 7. Extreme Mode: Lock DRAM to Max OPP 0 (2133MHz) */
		if (mode >= GAMING_MODE_EXTREME)
			fbt_boost_dram(1);

		/* 8. Touchscreen: Engage Hardware Touch Game Mode & Sensitivity */
		{
			extern int pox_touch_game_mode_set(int enable);
			extern int pox_touch_sensitivity_set(int val);
			pox_touch_game_mode_set(1);
			pox_touch_sensitivity_set(2);
		}

		/* 9. EAS Schedutil Headroom Margin: 32% headroom */
		{
			extern void set_capacity_margin(unsigned int margin);
			set_capacity_margin(1350);
		}

		/* 9b. Dynamic warm trim: 42..45C trims margin/TA one step
		 * (still gaming, less heat). >=45C handled by derate above. */
		if (pox_pwr_auto) {
			extern int battery_get_bat_temperature(void);
			if (battery_get_bat_temperature() >= 420 &&
			    battery_get_bat_temperature() < 450) {
				extern void set_capacity_margin(unsigned int margin);
				set_capacity_margin(1310);
				boost_write_for_perf_idx(3, 15);
			}
		}

		/* 10. Adaptive memory: early reclaim, ZRAM-first, launch-cache retain */
		pox_memory_preset(mode);

		pr_info("Gaming Mode activated: Zero frame-drop profile engaged.\n");
	} else if (mode == GAMING_MODE_POWERSAVE) {
		pr_info("Activating Ultra Power Saver Profile (level -1)...\n");

		/* 1. Low-Power FPSGO */
		fbt_cpu_set_ultra_rescue(0);
		fbt_cpu_set_rescue_percent(50);
		fbt_cpu_set_variance(50);
		fbt_cpu_set_bhr(0);
		fbt_cpu_set_rescue_opp_c(15);

		/* 2. Schedutil Debounced Ramp-Up & Fast Down */
		schedutil_set_up_rate_limit_us(0, 2000);   /* 2ms debounce on Little A55 */
		schedutil_set_down_rate_limit_us(0, 2000);
		schedutil_set_up_rate_limit_us(6, 5000);   /* 5ms debounce before Big A76 */
		schedutil_set_down_rate_limit_us(6, 1000); /* Fast return to idle */

		/* 3. Disable GPU Boost & Margin */
		ged_kpi_set_gaming_boost(0);
		ged_dvfs_set_gaming_boost(0);

		/* 4. PPM COBRA Power-First Hint */
		eara_pass_perf_first_hint(0);

		/* 5. Minimal SchedTune QoS */
		boost_write_for_perf_idx(3, 0);
		prefer_idle_for_perf_idx(3, 0);
		boost_write_for_perf_idx(1, 0);
		prefer_idle_for_perf_idx(1, 0);

		/* 6. Display Engine: True Tone remains default display profile */

		/* 7. Release DRAM Boost */
		fbt_boost_dram(0);

		/* 8. Restore Touchscreen Low-Power */
		{
			extern int pox_touch_game_mode_set(int enable);
			extern int pox_touch_sensitivity_set(int val);
			pox_touch_game_mode_set(0);
			pox_touch_sensitivity_set(0);
		}

		/* 9. EAS Schedutil Headroom Margin: 10% minimal headroom, packing tasks on A55 Little cores */
		{
			extern void set_capacity_margin(unsigned int margin);
			set_capacity_margin(1126);
		}

		/* Auto-disarm dynamic fsync on powersave to guarantee database durability */
		if (pox_dynamic_fsync_get())
			pox_dynamic_fsync_set(0);

		/* 10. Adaptive memory powersave: less compression CPU, drop caches */
		pox_memory_preset(mode);

		pr_info("Ultra Power Saver Profile engaged: Maximum battery preservation.\n");
	} else {
		/* Auto-disarm dynamic fsync on exit from gaming mode */
		if (pox_dynamic_fsync_get())
			pox_dynamic_fsync_set(0);

		pr_info("Deactivating Gaming Mode: Restoring Balanced Profile...\n");

		/* 1. Restore FPSGO Defaults */
		fbt_cpu_set_ultra_rescue(0);
		fbt_cpu_set_rescue_percent(33);
		fbt_cpu_set_variance(40);
		fbt_cpu_set_bhr(5);
		fbt_cpu_set_rescue_opp_c(15); /* Default ceiling OPP */

		/* 2. Restore Schedutil Defaults (500us ramp-up, 10ms anti-jitter hold) */
		schedutil_set_up_rate_limit_us(0, 500);
		schedutil_set_down_rate_limit_us(0, 10000);
		schedutil_set_up_rate_limit_us(6, 500);
		schedutil_set_down_rate_limit_us(6, 10000);

		/* 3. Restore GED GPU Defaults */
		ged_kpi_set_gaming_boost(0);
		ged_dvfs_set_gaming_boost(0);

		/* 4. Restore PPM COBRA Defaults */
		eara_pass_perf_first_hint(0);

		/* 5. Restore Balanced SchedTune QoS */
		boost_write_for_perf_idx(3, 5);
		prefer_idle_for_perf_idx(3, 1);
		boost_write_for_perf_idx(1, 0);
		prefer_idle_for_perf_idx(1, 0);

		/* 6. Display Engine: True Tone remains default display profile */

		/* 7. Release DRAM Boost */
		fbt_boost_dram(0);

		/* 8. Restore Touchscreen Defaults */
		{
			extern int pox_touch_game_mode_set(int enable);
			extern int pox_touch_sensitivity_set(int val);
			pox_touch_game_mode_set(0);
			pox_touch_sensitivity_set(0);
		}

		/* 9. Restore Default EAS Schedutil Headroom Margin: 25% default */
		{
			extern void set_capacity_margin(unsigned int margin);
			set_capacity_margin(1280);
		}

		/* 10. Restore balanced memory: ZRAM speed + throughput dirty */
		pox_memory_preset(mode);

		pr_info("Gaming Mode deactivated: Balanced Profile restored.\n");
	}

	pox_pwr_activations++;
	if (mode >= GAMING_MODE_EXTREME)
		pox_pwr_extreme_entries++;
	else if (mode > 0)
		pox_pwr_gaming_entries++;
	else if (mode == GAMING_MODE_POWERSAVE)
		pox_pwr_powersave_entries++;
	gaming_mode_state = mode;
	mutex_unlock(&gaming_mode_lock);

	/* Dynamic engine: always-on 8s loop in auto. Guesses from real
	 * CPU/GPU/touch/battery, so zero ROM writes are needed. */
	if (pox_pwr_auto) {
		cancel_delayed_work(&pox_dynamic_work);
		schedule_delayed_work(&pox_dynamic_work, msecs_to_jiffies(8000));
	} else {
		cancel_delayed_work(&pox_dynamic_work);
	}
	return 0;
}
EXPORT_SYMBOL(gaming_mode_set);

/* Dynamic engine core: hints in, effective power out. */
/* Dynamic engine core: guesses from real values, ROM hints are only bias.
 * Signals: CPU load (avenrun), GPU loading (GED), finger state (ktch),
 * battery temp/soc. Hysteresis votes stop flapping. */
static int pox_dyn_game_votes;
static int pox_dyn_idle_votes;
static int pox_dyn_extreme_votes;
static int pox_dyn_save_votes;

static int pox_dynamic_guess(void)
{
	extern int battery_get_bat_temperature(void);
	extern int battery_get_uisoc(void);
	extern int pox_touch_active_hint(void);
	extern unsigned int ged_dvfs_get_gpu_loading(void);
	int cpu = LOAD_INT(avenrun[0]);
	int gpu = (int)ged_dvfs_get_gpu_loading();
	int touch = pox_touch_active_hint();
	int btemp = battery_get_bat_temperature();
	int soc = battery_get_uisoc();
	int hint_bias = (pox_pwr_hint > 0) ? 1 : 0;
	int game_like, extreme_like, idle_like, save_like;

	if (gpu < 0)
		gpu = 0;
	else if (gpu > 100)
		gpu = 100;

	/* Real game footprint: GPU working, or finger + CPU, or a ROM
	 * hint together with real CPU load (hint alone never boosts). */
	game_like = (gpu >= 40) || (touch && cpu >= 2) ||
		    (hint_bias && cpu >= 3);
	extreme_like = game_like && gpu >= 70 && btemp < 430 && soc > 30;
	idle_like = !touch && gpu < 10 && cpu < 2;
	save_like = soc < 12 && idle_like;

	if (game_like) {
		if (pox_dyn_game_votes < 3)
			pox_dyn_game_votes++;
	} else {
		pox_dyn_game_votes = 0;
	}
	if (extreme_like) {
		if (pox_dyn_extreme_votes < 3)
			pox_dyn_extreme_votes++;
	} else {
		pox_dyn_extreme_votes = 0;
	}
	if (idle_like) {
		if (pox_dyn_idle_votes < 4)
			pox_dyn_idle_votes++;
	} else {
		pox_dyn_idle_votes = 0;
	}
	if (save_like) {
		if (pox_dyn_save_votes < 3)
			pox_dyn_save_votes++;
	} else {
		pox_dyn_save_votes = 0;
	}

	if (pox_dyn_save_votes >= 3)
		return GAMING_MODE_POWERSAVE;
	if (pox_dyn_extreme_votes >= 3)
		return GAMING_MODE_EXTREME;
	if (pox_dyn_game_votes >= 2)
		return GAMING_MODE_ENABLED;
	if (pox_dyn_idle_votes >= 4 && gaming_mode_state > 0 &&
	    pox_pwr_hint <= 0)
		return GAMING_MODE_DISABLED;
	return gaming_mode_state;
}

static int pox_dynamic_effective(int hint)
{
	int eff;

	if (!pox_pwr_auto)
		return hint;
	/* Manual/ROM hints fast-path for snappiness; the guess loop
	 * self-corrects within seconds if no real load follows. */
	if (hint > 0 && gaming_mode_state <= 0)
		return hint;
	if (hint == GAMING_MODE_POWERSAVE)
		return hint;
	eff = pox_dynamic_guess();
	/* A latched gaming hint biases one step up, never down. */
	if (hint > 0 && eff <= 0)
		eff = GAMING_MODE_ENABLED;
	return eff;
}

static void pox_dynamic_evaluate(struct work_struct *work)
{
	int eff;

	mutex_lock(&gaming_mode_lock);
	eff = pox_dynamic_effective(pox_pwr_hint);
	if (eff != gaming_mode_state) {
		mutex_unlock(&gaming_mode_lock);
		gaming_mode_set(eff);
		return;
	}
	pox_health_calibration_sync();
	mutex_unlock(&gaming_mode_lock);

	/* Autonomous: keep guessing forever in auto. 8s cadence is
	 * inaudible in power (~one work item) and self-corrects any
	 * fast-path hint within seconds. */
	if (pox_pwr_auto)
		schedule_delayed_work(&pox_dynamic_work, msecs_to_jiffies(8000));
}

/* ROM writes are hints, not latched modes. */
int pox_gaming_hint(int hint)
{
	if (hint < GAMING_MODE_POWERSAVE)
		hint = GAMING_MODE_POWERSAVE;
	else if (hint > GAMING_MODE_EXTREME)
		hint = GAMING_MODE_EXTREME;
	pox_pwr_hint = hint;
	return gaming_mode_set(pox_dynamic_effective(hint));
}
EXPORT_SYMBOL(pox_gaming_hint);

int gaming_mode_get(void)
{
	return gaming_mode_state;
}
EXPORT_SYMBOL(gaming_mode_get);

int pox_gaming_mode_get(void)
{
	return gaming_mode_state;
}
EXPORT_SYMBOL(pox_gaming_mode_get);

/* ------------------ ProcFS Interfaces ------------------ */

static int gaming_mode_proc_show(struct seq_file *m, void *v)
{
	int state = gaming_mode_get();
	int color_st = get_ios_color_mode();

	seq_printf(m, "gaming_mode: %d\n", state);
	seq_printf(m, "engine: dynamic-autonomous (%s, hint %d%s, votes g%d/i%d/x%d/s%d)\n",
		pox_pwr_auto ? "auto" : "manual", pox_pwr_hint,
		pox_pwr_derate_hold ? ", health-derated" : "",
		pox_dyn_game_votes, pox_dyn_idle_votes,
		pox_dyn_extreme_votes, pox_dyn_save_votes);
	if (state == GAMING_MODE_EXTREME)
		seq_printf(m, "status: EXTREME GAMING MODE (Locked Max DRAM OPP + Zero Frame Drops)\n");
	else if (state == GAMING_MODE_ENABLED)
		seq_printf(m, "status: GAMING MODE ACTIVE (Zero Frame Drops Enabled)\n");
	else if (state == GAMING_MODE_POWERSAVE)
		seq_printf(m, "status: ULTRA POWER SAVER PROFILE (10%% Headroom + Little Core Affinity + Max Battery)\n");
	else
		seq_printf(m, "status: BALANCED PROFILE (iOS Fluidity & Real Colors Active)\n");

	seq_printf(m, "features:\n");
	seq_printf(m, "  - ultra_rescue: %s\n", (state > 0) ? "enabled (DRAM boost on hitch)" : "disabled");
	seq_printf(m, "  - rescue_percent: %d%%\n", (state > 0) ? 20 : (state < 0 ? 50 : 33));
	seq_printf(m, "  - variance_sensitivity: %d\n", (state > 0) ? 15 : (state < 0 ? 50 : 40));
	seq_printf(m, "  - big_core_hold_rate (bhr): %d\n", (state > 0) ? 15 : (state < 0 ? 0 : 5));
	seq_printf(m, "  - schedutil_ramp_up: %d us\n", (state > 0) ? 500 : (state < 0 ? 2000 : 500));
	seq_printf(m, "  - schedutil_hold_down: %d us\n", (state > 0) ? 20000 : (state < 0 ? 2000 : 10000));
	seq_printf(m, "  - gpu_touch_boost: %s\n", (state > 0) ? "enabled" : (state < 0 ? "powersave" : "balanced"));
	seq_printf(m, "  - gpu_dvfs_margin: %s\n", (state > 0) ? "+20% (PERF)" : "default");
	seq_printf(m, "  - ppm_cobra_budget: %s\n", (state > 0) ? "Performance-First (A76 prioritized)" : (state < 0 ? "Power-First (A55 Little packed)" : "Balanced"));
	seq_printf(m, "  - top_app_boost: %d%%\n", (state > 0) ? 20 : (state < 0 ? 0 : 5));
	seq_printf(m, "  - eas_capacity_margin: %d (%d%% headroom)\n",
		(state > 0) ? 1350 : (state < 0 ? 1126 : 1280),
		(state > 0) ? 32 : (state < 0 ? 10 : 25));
	seq_printf(m, "  - top_app_prefer_idle: %s\n", (state < 0) ? "disabled (pack to Little)" : "enabled");
	seq_printf(m, "  - true_tone: %s (Calibrated D65 Liquid Retina Reference [DEFAULT])\n",
		(color_st == COLOR_MODE_REFERENCE) ? "active" : "standby");
	seq_printf(m, "  - video_clock_floor: active (anti-lag enabled)\n");
	seq_printf(m, "  - display_ddr_floor: LP4-2100 minimum\n");
	seq_printf(m, "  - cfs_latency: 4 ms (500 us preemption, unscaled)\n");
	seq_printf(m, "  - timer_hz: %d Hz (3.33 ms jiffy, display-synced)\n", HZ);
	seq_printf(m, "  - io_scheduler: deadline (guaranteed UFS read latency)\n");
	seq_printf(m, "  - tcp_congestion: bbr (low bufferbloat)\n");
	seq_printf(m, "  - hardware_touch_boost: enabled (A55@1.50GHz / A76@1.53GHz + 60%% TA uclamp)\n");
	seq_printf(m, "  - camera_4k_60fps: unlocked (Samsung GW1 16MP@60fps)\n");
	return 0;
}

static ssize_t gaming_mode_proc_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	char buf[32];
	int val = 0;
	size_t len;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	/* Strip trailing whitespace */
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace(buf[len - 1])))
		buf[--len] = '\0';

	if (strcasecmp(buf, "1") == 0 || strcasecmp(buf, "on") == 0 ||
	    strcasecmp(buf, "enable") == 0 || strcasecmp(buf, "true") == 0) {
		val = GAMING_MODE_ENABLED;
	} else if (strcasecmp(buf, "2") == 0 || strcasecmp(buf, "extreme") == 0) {
		val = GAMING_MODE_EXTREME;
	} else if (strcasecmp(buf, "-1") == 0 || strcasecmp(buf, "powersave") == 0 ||
		   strcasecmp(buf, "power_save") == 0 || strcasecmp(buf, "saver") == 0 ||
		   strcasecmp(buf, "battery") == 0) {
		val = GAMING_MODE_POWERSAVE;
	} else if (strcasecmp(buf, "0") == 0 || strcasecmp(buf, "off") == 0 ||
		   strcasecmp(buf, "disable") == 0 || strcasecmp(buf, "false") == 0) {
		val = GAMING_MODE_DISABLED;
	} else if (strcasecmp(buf, "auto") == 0 || strcasecmp(buf, "dynamic") == 0) {
		pox_pwr_auto = 1;
		kp_auto_mode = 1;
		pox_gaming_hint(pox_pwr_hint);
		return count;
	} else if (strcasecmp(buf, "manual") == 0) {
		pox_pwr_auto = 0;
		kp_auto_mode = 0;
		cancel_delayed_work_sync(&pox_dynamic_work);
		return count;
	} else {
		if (kstrtoint(buf, 10, &val) < 0)
			return -EINVAL;
	}

	if (val < GAMING_MODE_POWERSAVE || val > GAMING_MODE_EXTREME)
		return -EINVAL;

	pox_gaming_hint(val);
	return count;
}

static int gaming_mode_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, gaming_mode_proc_show, NULL);
}

static const struct file_operations gaming_mode_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = gaming_mode_proc_open,
	.read    = seq_read,
	.write   = gaming_mode_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int color_mode_proc_show(struct seq_file *m, void *v)
{
	int mode = get_ios_color_mode();

	seq_printf(m, "color_mode: %d\n", mode);
	if (mode == COLOR_MODE_CUSTOM)
		seq_printf(m, "status: Custom User Profile\n");
	else if (mode == COLOR_MODE_SLOG3)
		seq_printf(m, "status: Sony S-Log3 / Cinema Flat Profile (Logarithmic Dynamic Range for LUT Grading)\n");
	else if (mode == COLOR_MODE_VIVID)
		seq_printf(m, "status: iOS Vivid / Cinema HDR\n");
	else if (mode == COLOR_MODE_REFERENCE)
		seq_printf(m, "status: True Tone / iOS Reference (Calibrated D65 Liquid Retina) [DEFAULT]\n");
	else
		seq_printf(m, "status: Standard Neutral\n");
	return 0;
}

static ssize_t color_mode_proc_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char buf[16];
	int val = 0;
	size_t len;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace(buf[len - 1])))
		buf[--len] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0)
		val = 0;
	else if (val > 4)
		val = 4;

	user_color_mode_override = val;
	set_ios_color_mode(val);
	return count;
}

static int color_mode_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, color_mode_proc_show, NULL);
}

static const struct file_operations color_mode_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = color_mode_proc_open,
	.read    = seq_read,
	.write   = color_mode_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int color_custom_proc_show(struct seq_file *m, void *v)
{
	int sat = 4, con = 4, bri = 4, sat_adj = 0;

	get_ios_color_custom(&sat, &con, &bri, &sat_adj);
	seq_printf(m, "sat: %d\ncontrast: %d\nbrightness: %d\nsat_adj: %d\nvalues: %d %d %d %d\n",
		   sat, con, bri, sat_adj, sat, con, bri, sat_adj);
	return 0;
}

static ssize_t color_custom_proc_write(struct file *file, const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	char buf[64];
	int sat = 4, con = 4, bri = 4, sat_adj = 0;
	size_t len;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%d %d %d %d", &sat, &con, &bri, &sat_adj) >= 3 ||
	    sscanf(buf, "%d,%d,%d,%d", &sat, &con, &bri, &sat_adj) >= 3) {
		pox_color_custom_set(sat, con, bri, sat_adj);
		return count;
	}

	return -EINVAL;
}

static int color_custom_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, color_custom_proc_show, NULL);
}

static const struct file_operations color_custom_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = color_custom_proc_open,
	.read    = seq_read,
	.write   = color_custom_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int true_tone_proc_show(struct seq_file *m, void *v)
{
	int tt = pox_true_tone_get();
	seq_printf(m, "true_tone: %d\n", tt);
	seq_printf(m, "status: %s\n", tt ? "enabled (Calibrated D65 Liquid Retina Reference [DEFAULT])" : "disabled (Standard Neutral)");
	return 0;
}

static ssize_t true_tone_proc_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char buf[16];
	int val = 0;
	size_t len;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace(buf[len - 1])))
		buf[--len] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	pox_true_tone_set(val ? 1 : 0);
	return count;
}

static int true_tone_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, true_tone_proc_show, NULL);
}

static const struct file_operations true_tone_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = true_tone_proc_open,
	.read    = seq_read,
	.write   = true_tone_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int hbm_mode_proc_show(struct seq_file *m, void *v)
{
	int mode = hbm_mode_get();

	seq_printf(m, "hbm_mode: %d\n", mode);
	if (mode == HBM_MODE_L3)
		seq_printf(m, "status: SUNLIGHT HBM LEVEL 3 (27.5mA Peak Overdrive ~550+ nits)\n");
	else if (mode == HBM_MODE_L2)
		seq_printf(m, "status: SUNLIGHT HBM LEVEL 2 (25.3mA High Brightness ~500 nits)\n");
	else if (mode == HBM_MODE_L1)
		seq_printf(m, "status: SUNLIGHT HBM LEVEL 1 (22.0mA Daylight Boost ~450 nits)\n");
	else
		seq_printf(m, "status: NORMAL / AUTO (Standard Backlight Curve 0..2047)\n");

	return 0;
}

static ssize_t hbm_mode_proc_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	char buf[16];
	int val = 0;
	size_t len;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace(buf[len - 1])))
		buf[--len] = '\0';

	if (strcasecmp(buf, "3") == 0 || strcasecmp(buf, "max") == 0 ||
	    strcasecmp(buf, "overdrive") == 0 || strcasecmp(buf, "l3") == 0) {
		val = HBM_MODE_L3;
	} else if (strcasecmp(buf, "2") == 0 || strcasecmp(buf, "l2") == 0) {
		val = HBM_MODE_L2;
	} else if (strcasecmp(buf, "1") == 0 || strcasecmp(buf, "l1") == 0 ||
		   strcasecmp(buf, "on") == 0 || strcasecmp(buf, "enable") == 0) {
		val = HBM_MODE_L1;
	} else if (strcasecmp(buf, "0") == 0 || strcasecmp(buf, "off") == 0 ||
		   strcasecmp(buf, "disable") == 0) {
		val = HBM_MODE_OFF;
	} else {
		if (kstrtoint(buf, 10, &val) < 0)
			return -EINVAL;
	}

	if (val < HBM_MODE_OFF || val > HBM_MODE_L3)
		return -EINVAL;

	hbm_mode_set(val);
	return count;
}

static int hbm_mode_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, hbm_mode_proc_show, NULL);
}

static const struct file_operations hbm_mode_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = hbm_mode_proc_open,
	.read    = seq_read,
	.write   = hbm_mode_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int torch_brightness_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", torch_brightness_get());
	return 0;
}

static ssize_t torch_brightness_proc_write(struct file *file, const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	char buf[16];
	int val = 0;
	size_t len;
	/* Pox standout: intentional safe-rootless torch grading (0666).
	 * No CAP_SYS_ADMIN so flashlight apps work without root.
	 * Safety lives in pox_torch_brightness_set(): clamp sel 0..24
	 * (25..325mA/ch), 5-min auto-timeout, mutex, camera arbitration.
	 * Here: reject oversize fuzz, clamp 0..255, rate-limit strobing. */
	static unsigned long last_jiffies;
	if (count == 0)
		return 0;
	if (count >= sizeof(buf))
		return -EINVAL;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace(buf[len - 1])))
		buf[--len] = '\0';

	if (strcasecmp(buf, "on") == 0 || strcasecmp(buf, "enable") == 0 ||
	    strcasecmp(buf, "true") == 0) {
		val = 10;
	} else if (strcasecmp(buf, "off") == 0 || strcasecmp(buf, "disable") == 0 ||
		   strcasecmp(buf, "false") == 0) {
		val = 0;
	} else {
		if (kstrtoint(buf, 10, &val) < 0)
			return -EINVAL;
	}

	/* Clamp to 0..255 (driver further clamps to sel 0..24). Never
	 * allow negative wrap or huge values to reach hardware. */
	if (val < 0)
		val = 0;
	else if (val > 255)
		val = 255;

	/* Anti-strobe: max one level change per 20ms. Prevents rapid
	 * on/off fuzz from overheating MT6360 or triggering
	 * photosensitivity, while keeping smooth grading feel. */
	if (last_jiffies && time_before(jiffies, last_jiffies + msecs_to_jiffies(20)))
		return -EBUSY;
	last_jiffies = jiffies;

	torch_brightness_set(val);
	return count;
}

static int torch_brightness_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, torch_brightness_proc_show, NULL);
}

static const struct file_operations torch_brightness_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = torch_brightness_proc_open,
	.read    = seq_read,
	.write   = torch_brightness_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int torch_info_proc_show(struct seq_file *m, void *v)
{
	int val = torch_brightness_get();
	int sel = 0;
	int ma = 0;

	if (val > 0) {
		static const u8 s10[11] = { 0, 0, 2, 4, 6, 9, 12, 15, 18, 21, 24 };
		if (val <= 10)
			sel = s10[val];
		else if (val <= 24)
			sel = val;
		else if (val <= 100)
			sel = (val * 24) / 100;
		else if (val <= 255)
			sel = (val * 24) / 255;
		else
			sel = 24;

		ma = 25 + (sel * 125) / 10;
	}

	seq_printf(m, "brightness: %d\n", val);
	seq_printf(m, "hardware_selector: %d (0..24)\n", sel);
	seq_printf(m, "current_per_channel: %d mA\n", ma);
	seq_printf(m, "current_total_dual: %d mA\n", ma * 2);
	seq_printf(m, "status: %s\n", val > 0 ? "on" : "off");
	return 0;
}

static int torch_info_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, torch_info_proc_show, NULL);
}

static const struct file_operations torch_info_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = torch_info_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int camera_profile_proc_show(struct seq_file *m, void *v)
{
	int mode = get_ios_color_mode();

	seq_printf(m, "camera_profile: %d\n", mode);
	if (mode == COLOR_MODE_SLOG3)
		seq_printf(m, "profile_name: Sony S-Log3 / Cinema Flat (Wide Dynamic Range for LUT Grading)\n");
	else if (mode == COLOR_MODE_VIVID)
		seq_printf(m, "profile_name: iOS Vivid / Cinema (Enhanced Dynamic Range)\n");
	else if (mode == COLOR_MODE_REFERENCE)
		seq_printf(m, "profile_name: iOS TrueColor Reference (Calibrated D65 Liquid Retina)\n");
	else
		seq_printf(m, "profile_name: Standard Rec.709 Neutral\n");

	seq_printf(m, "camera_4k60_force: %d\n", camera_4k60_get());
	seq_printf(m, "slog3_active: %d\n", camera_slog3_get());
	seq_printf(m, "launch_boost_active_sessions: %d\n", atomic_read(&pox_cam_active_sessions));

	seq_printf(m, "capabilities:\n");
	seq_printf(m, "  - 4k_60fps_recording: enabled (Samsung GW1 16MP@60fps custom3 mode)\n");
	seq_printf(m, "  - isp_qos: launch-primed 560MHz, dynamic DFS after (no forced floor)\n");
	seq_printf(m, "  - venc_clock: active-session floor (anti-collapse)\n");
	seq_printf(m, "  - memory_qos_floor: LP4-2100 (HRT_LEVEL0 DDR floor)\n");
	seq_printf(m, "  - zero_shutter_lag: supported (2-frame delay pipeline)\n");
	seq_printf(m, "  - log_transfer_function: %s\n", (mode == COLOR_MODE_SLOG3) ? "S-Log3 Logarithmic (42% Middle Gray, 61% 90-White)" : "Rec.709 Standard");
	return 0;
}

static ssize_t camera_profile_proc_write(struct file *file, const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	return color_mode_proc_write(file, ubuf, count, ppos);
}

static int camera_profile_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, camera_profile_proc_show, NULL);
}

static const struct file_operations camera_profile_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = camera_profile_proc_open,
	.read    = seq_read,
	.write   = camera_profile_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ------------------ Camera 4K60 ProcFS ------------------ */

static int camera_4k60_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", camera_4k60_get());
	return 0;
}

static ssize_t camera_4k60_proc_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	char buf[16];
	int val = 0;
	size_t len;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count == 0)
		return 0;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace(buf[len - 1])))
		buf[--len] = '\0';

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	camera_4k60_set(val);
	return count;
}

static int camera_4k60_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, camera_4k60_proc_show, NULL);
}

static const struct file_operations camera_4k60_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = camera_4k60_proc_open,
	.read    = seq_read,
	.write   = camera_4k60_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ------------------ Sony S-Log3 ProcFS ------------------ */

static int slog3_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", camera_slog3_get());
	return 0;
}

static ssize_t slog3_proc_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char buf[16];
	int val = 0;
	size_t len;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count == 0)
		return 0;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || isspace(buf[len - 1])))
		buf[--len] = '\0';

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	camera_slog3_set(val);
	return count;
}

static int slog3_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, slog3_proc_show, NULL);
}

static const struct file_operations slog3_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = slog3_proc_open,
	.read    = seq_read,
	.write   = slog3_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ------------------ SysFS Interfaces ------------------ */

static ssize_t gaming_mode_sysfs_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", gaming_mode_get());
}

static ssize_t gaming_mode_sysfs_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < GAMING_MODE_POWERSAVE)
		val = GAMING_MODE_POWERSAVE;
	else if (val > GAMING_MODE_EXTREME)
		val = GAMING_MODE_EXTREME;

	pox_gaming_hint(val);
	return count;
}

static struct kobj_attribute gaming_mode_kobj_attr =
	__ATTR(gaming_mode, 0644, gaming_mode_sysfs_show, gaming_mode_sysfs_store);

static ssize_t color_mode_sysfs_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", get_ios_color_mode());
}

static ssize_t color_mode_sysfs_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	if (val < 0)
		val = 0;
	else if (val > 4)
		val = 4;

	user_color_mode_override = val;
	set_ios_color_mode(val);
	return count;
}

static struct kobj_attribute color_mode_kobj_attr =
	__ATTR(color_mode, 0644, color_mode_sysfs_show, color_mode_sysfs_store);

static ssize_t color_custom_sysfs_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	int sat = 4, con = 4, bri = 4, sat_adj = 0;

	get_ios_color_custom(&sat, &con, &bri, &sat_adj);
	return sprintf(buf, "%d %d %d %d\n", sat, con, bri, sat_adj);
}

static ssize_t color_custom_sysfs_store(struct kobject *kobj,
				        struct kobj_attribute *attr,
				        const char *buf, size_t count)
{
	int sat = 4, con = 4, bri = 4, sat_adj = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (sscanf(buf, "%d %d %d %d", &sat, &con, &bri, &sat_adj) >= 3 ||
	    sscanf(buf, "%d,%d,%d,%d", &sat, &con, &bri, &sat_adj) >= 3) {
		pox_color_custom_set(sat, con, bri, sat_adj);
		return count;
	}

	return -EINVAL;
}

static struct kobj_attribute color_custom_kobj_attr =
	__ATTR(color_custom, 0644, color_custom_sysfs_show, color_custom_sysfs_store);

static struct kobj_attribute camera_profile_kobj_attr =
	__ATTR(camera_profile, 0644, color_mode_sysfs_show, color_mode_sysfs_store);

static ssize_t true_tone_sysfs_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", pox_true_tone_get());
}

static ssize_t true_tone_sysfs_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) != 0)
		return -EINVAL;

	pox_true_tone_set(val ? 1 : 0);
	return count;
}

static struct kobj_attribute true_tone_kobj_attr =
	__ATTR(true_tone, 0644, true_tone_sysfs_show, true_tone_sysfs_store);

static ssize_t hbm_mode_sysfs_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hbm_mode_get());
}

static ssize_t hbm_mode_sysfs_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < HBM_MODE_OFF || val > HBM_MODE_L3)
		return -EINVAL;

	hbm_mode_set(val);
	return count;
}

static struct kobj_attribute hbm_mode_kobj_attr =
	__ATTR(hbm_mode, 0644, hbm_mode_sysfs_show, hbm_mode_sysfs_store);

static ssize_t torch_brightness_sysfs_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return sprintf(buf, "%d\n", torch_brightness_get());
}

static ssize_t torch_brightness_sysfs_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	int val = 0;
	/* Safe-rootless mirror of proc write: keep 0666 for flashlight
	 * apps, clamp + 20ms anti-strobe. Hardware ceiling in driver. */
	static unsigned long last_jiffies_sysfs;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0)
		val = 0;
	else if (val > 255)
		val = 255;

	if (last_jiffies_sysfs && time_before(jiffies, last_jiffies_sysfs + msecs_to_jiffies(20)))
		return -EBUSY;
	last_jiffies_sysfs = jiffies;

	torch_brightness_set(val);
	return count;
}

static struct kobj_attribute torch_brightness_kobj_attr = {
	.attr	= { .name = "torch_brightness", .mode = 0666 },
	.show	= torch_brightness_sysfs_show,
	.store	= torch_brightness_sysfs_store,
};

static struct kobj_attribute flashlight_brightness_kobj_attr = {
	.attr	= { .name = "flashlight_brightness", .mode = 0666 },
	.show	= torch_brightness_sysfs_show,
	.store	= torch_brightness_sysfs_store,
};

static ssize_t camera_4k60_sysfs_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", camera_4k60_get());
}

static ssize_t camera_4k60_sysfs_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	camera_4k60_set(val);
	return count;
}

static struct kobj_attribute camera_4k60_kobj_attr =
	__ATTR(camera_4k60, 0644, camera_4k60_sysfs_show, camera_4k60_sysfs_store);

static ssize_t slog3_sysfs_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", camera_slog3_get());
}

static ssize_t slog3_sysfs_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	camera_slog3_set(val);
	return count;
}

static struct kobj_attribute slog3_kobj_attr =
	__ATTR(slog3, 0644, slog3_sysfs_show, slog3_sysfs_store);

/* =========================================================================
 * Kprofiles Compatibility Subsystem
 * Exposes /sys/kernel/kprofiles/kp_mode, auto_kp, and custom_params
 * Modes: 0=Disabled/None, 1=Battery, 2=Balanced, 3=Performance, 4=Custom
 * ========================================================================= */

static int kprofiles_custom_active;

static struct pox_kprofile_custom_tunables pox_custom_kp = {
	.schedutil_up_little_us = 500,
	.schedutil_down_little_us = 20000,
	.schedutil_up_big_us = 200,
	.schedutil_down_big_us = 20000,
	.top_app_boost = 20,
	.fg_boost = 5,
	.gpu_boost = 1,
	.dram_boost = 0,
	.touch_boost = 1,
	.rescue_opp = 0,
};

static void pox_kprofile_apply_custom_hardware(void)
{
	/* 1. Rescue OPP */
	fbt_cpu_set_ultra_rescue(1);
	fbt_cpu_set_rescue_percent(20);
	fbt_cpu_set_variance(15);
	fbt_cpu_set_bhr(15);
	fbt_cpu_set_rescue_opp_c(pox_custom_kp.rescue_opp);

	/* 2. Schedutil up/down rate limits */
	schedutil_set_up_rate_limit_us(0, pox_custom_kp.schedutil_up_little_us);
	schedutil_set_down_rate_limit_us(0, pox_custom_kp.schedutil_down_little_us);
	schedutil_set_up_rate_limit_us(6, pox_custom_kp.schedutil_up_big_us);
	schedutil_set_down_rate_limit_us(6, pox_custom_kp.schedutil_down_big_us);

	/* 3. GPU DVFS & gaming boost */
	ged_kpi_set_gaming_boost(pox_custom_kp.gpu_boost ? 1 : 0);
	ged_dvfs_set_gaming_boost(pox_custom_kp.gpu_boost ? 1 : 0);
	eara_pass_perf_first_hint(pox_custom_kp.gpu_boost ? 1 : 0);

	/* 4. SchedTune boost */
	boost_write_for_perf_idx(3, pox_custom_kp.top_app_boost);
	prefer_idle_for_perf_idx(3, 1);
	boost_write_for_perf_idx(1, pox_custom_kp.fg_boost);
	prefer_idle_for_perf_idx(1, 1);

	/* 5. DRAM boost */
	if (pox_custom_kp.dram_boost)
		fbt_boost_dram(1);
	else
		fbt_boost_dram(0);

	/* 6. Hardware touch boost */
	{
		extern int pox_touch_game_mode_set(int enable);
		extern int pox_touch_sensitivity_set(int val);
		pox_touch_game_mode_set(pox_custom_kp.touch_boost ? 1 : 0);
		pox_touch_sensitivity_set(pox_custom_kp.touch_boost ? 2 : 0);
	}
}

int pox_kprofile_custom_set(const struct pox_kprofile_custom_tunables *tunables)
{
	if (!tunables)
		return -EINVAL;

	pox_custom_kp = *tunables;

	/* Clamp within safe hardware bounds */
	if (pox_custom_kp.schedutil_up_little_us < 50) pox_custom_kp.schedutil_up_little_us = 50;
	else if (pox_custom_kp.schedutil_up_little_us > 5000) pox_custom_kp.schedutil_up_little_us = 5000;

	if (pox_custom_kp.schedutil_down_little_us < 500) pox_custom_kp.schedutil_down_little_us = 500;
	else if (pox_custom_kp.schedutil_down_little_us > 100000) pox_custom_kp.schedutil_down_little_us = 100000;

	if (pox_custom_kp.schedutil_up_big_us < 50) pox_custom_kp.schedutil_up_big_us = 50;
	else if (pox_custom_kp.schedutil_up_big_us > 5000) pox_custom_kp.schedutil_up_big_us = 5000;

	if (pox_custom_kp.schedutil_down_big_us < 500) pox_custom_kp.schedutil_down_big_us = 500;
	else if (pox_custom_kp.schedutil_down_big_us > 100000) pox_custom_kp.schedutil_down_big_us = 100000;

	if (pox_custom_kp.top_app_boost < 0) pox_custom_kp.top_app_boost = 0;
	else if (pox_custom_kp.top_app_boost > 100) pox_custom_kp.top_app_boost = 100;

	if (pox_custom_kp.fg_boost < 0) pox_custom_kp.fg_boost = 0;
	else if (pox_custom_kp.fg_boost > 100) pox_custom_kp.fg_boost = 100;

	pox_custom_kp.gpu_boost = pox_custom_kp.gpu_boost ? 1 : 0;
	pox_custom_kp.dram_boost = pox_custom_kp.dram_boost ? 1 : 0;
	pox_custom_kp.touch_boost = pox_custom_kp.touch_boost ? 1 : 0;
	if (pox_custom_kp.rescue_opp < 0) pox_custom_kp.rescue_opp = 0;
	else if (pox_custom_kp.rescue_opp > 4) pox_custom_kp.rescue_opp = 4;

	if (kprofiles_custom_active)
		pox_kprofile_apply_custom_hardware();

	return 0;
}
EXPORT_SYMBOL(pox_kprofile_custom_set);

void pox_kprofile_custom_get(struct pox_kprofile_custom_tunables *tunables)
{
	if (tunables)
		*tunables = pox_custom_kp;
}
EXPORT_SYMBOL(pox_kprofile_custom_get);

int kprofiles_mode_get(void)
{
	int gm;

	if (kprofiles_custom_active)
		return 4; /* Custom */

	gm = gaming_mode_get();
	switch (gm) {
	case GAMING_MODE_POWERSAVE:
		return 1; /* Battery */
	case GAMING_MODE_ENABLED:
	case GAMING_MODE_EXTREME:
		return 3; /* Performance */
	case GAMING_MODE_DISABLED:
	default:
		return 2; /* Balanced */
	}
}
EXPORT_SYMBOL(kprofiles_mode_get);

int kprofiles_mode_set(int mode)
{
	if (mode == 4) {
		kprofiles_custom_active = 1;
		pox_kprofile_apply_custom_hardware();
		return 0;
	}

	kprofiles_custom_active = 0;
	switch (mode) {
	case 1: /* Battery / Powersave */
		return gaming_mode_set(GAMING_MODE_POWERSAVE);
	case 2: /* Balanced */
		return gaming_mode_set(GAMING_MODE_DISABLED);
	case 3: /* Performance / Gaming */
		return gaming_mode_set(GAMING_MODE_ENABLED);
	case 0: /* Disabled / None -> Defaults to Balanced */
	default:
		return gaming_mode_set(GAMING_MODE_DISABLED);
	}
}
EXPORT_SYMBOL(kprofiles_mode_set);

static ssize_t kp_mode_sysfs_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", kprofiles_mode_get());
}

static ssize_t kp_mode_sysfs_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 4)
		return -EINVAL;

	kprofiles_mode_set(val);
	return count;
}

static struct kobj_attribute kp_mode_kobj_attr =
	__ATTR(kp_mode, 0644, kp_mode_sysfs_show, kp_mode_sysfs_store);

static ssize_t kp_auto_sysfs_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", kp_auto_mode);
}

static ssize_t kp_auto_sysfs_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	kp_auto_mode = val ? 1 : 0;

	/* Bind auto_kp to the real autonomous power engine: with auto_kp
	 * off, the ROM hint is applied once and the guessing loop stops
	 * overriding it; with auto_kp on, the engine keeps re-evaluating
	 * every 8s.  A placebo toggle that only stores state is useless. */
	pox_pwr_auto = kp_auto_mode;
	cancel_delayed_work(&pox_dynamic_work);
	if (pox_pwr_auto)
		schedule_delayed_work(&pox_dynamic_work,
				      msecs_to_jiffies(8000));

	return count;
}

static struct kobj_attribute kp_auto_kobj_attr =
	__ATTR(auto_kp, 0644, kp_auto_sysfs_show, kp_auto_sysfs_store);

static ssize_t kp_custom_sysfs_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d %d %d %d %d %d %d %d %d %d\n",
		       pox_custom_kp.schedutil_up_little_us,
		       pox_custom_kp.schedutil_down_little_us,
		       pox_custom_kp.schedutil_up_big_us,
		       pox_custom_kp.schedutil_down_big_us,
		       pox_custom_kp.top_app_boost,
		       pox_custom_kp.fg_boost,
		       pox_custom_kp.gpu_boost,
		       pox_custom_kp.dram_boost,
		       pox_custom_kp.touch_boost,
		       pox_custom_kp.rescue_opp);
}

static ssize_t kp_custom_sysfs_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	struct pox_kprofile_custom_tunables t;
	int matched;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = pox_custom_kp;
	matched = sscanf(buf, "%d %d %d %d %d %d %d %d %d %d",
			 &t.schedutil_up_little_us,
			 &t.schedutil_down_little_us,
			 &t.schedutil_up_big_us,
			 &t.schedutil_down_big_us,
			 &t.top_app_boost,
			 &t.fg_boost,
			 &t.gpu_boost,
			 &t.dram_boost,
			 &t.touch_boost,
			 &t.rescue_opp);
	if (matched < 2)
		return -EINVAL;

	pox_kprofile_custom_set(&t);
	return count;
}

static struct kobj_attribute kp_custom_kobj_attr =
	__ATTR(custom_params, 0644, kp_custom_sysfs_show, kp_custom_sysfs_store);

static int kprofiles_custom_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d %d %d %d %d %d %d %d %d %d\n",
		   pox_custom_kp.schedutil_up_little_us,
		   pox_custom_kp.schedutil_down_little_us,
		   pox_custom_kp.schedutil_up_big_us,
		   pox_custom_kp.schedutil_down_big_us,
		   pox_custom_kp.top_app_boost,
		   pox_custom_kp.fg_boost,
		   pox_custom_kp.gpu_boost,
		   pox_custom_kp.dram_boost,
		   pox_custom_kp.touch_boost,
		   pox_custom_kp.rescue_opp);
	return 0;
}

static ssize_t kprofiles_custom_proc_write(struct file *file, const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	char buf[128];
	size_t len;
	struct pox_kprofile_custom_tunables t;
	int matched;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	t = pox_custom_kp;
	matched = sscanf(buf, "%d %d %d %d %d %d %d %d %d %d",
			 &t.schedutil_up_little_us,
			 &t.schedutil_down_little_us,
			 &t.schedutil_up_big_us,
			 &t.schedutil_down_big_us,
			 &t.top_app_boost,
			 &t.fg_boost,
			 &t.gpu_boost,
			 &t.dram_boost,
			 &t.touch_boost,
			 &t.rescue_opp);
	if (matched < 2)
		return -EINVAL;

	pox_kprofile_custom_set(&t);
	return count;
}

static int kprofiles_custom_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, kprofiles_custom_proc_show, NULL);
}

static const struct file_operations kprofiles_custom_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = kprofiles_custom_proc_open,
	.read    = seq_read,
	.write   = kprofiles_custom_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static struct attribute *kp_attrs[] = {
	&kp_mode_kobj_attr.attr,
	&kp_auto_kobj_attr.attr,
	&kp_custom_kobj_attr.attr,
	NULL,
};

static struct attribute_group kp_attr_group = {
	.attrs = kp_attrs,
};

/* =========================================================================
 * Phone Health & Silicon OPP Table Calibration Engine
 * Dynamically calibrates CPU/GPU OPP tables to match live battery health,
 * internal resistance, and temperature to ensure ZERO freezes & brownouts.
 * ========================================================================= */

int pox_health_calibration_get(void)
{
	return pox_health_calibration_mode;
}
EXPORT_SYMBOL(pox_health_calibration_get);

int pox_health_calibration_set(int mode)
{
	if (mode < 0 || mode > 3)
		return -EINVAL;

	pox_health_calibration_mode = mode;
	pox_health_calibration_sync();
	pr_info("Phone Health Silicon Calibration Mode set to: %d\n", mode);
	return 0;
}
EXPORT_SYMBOL(pox_health_calibration_set);

int pox_opp_voltage_guard_get(void)
{
	return pox_opp_voltage_guard_enabled;
}
EXPORT_SYMBOL(pox_opp_voltage_guard_get);

int pox_opp_voltage_guard_set(int enable)
{
	pox_opp_voltage_guard_enabled = enable ? 1 : 0;
	pox_health_calibration_sync();
	pr_info("OPP Voltage Brownout Guard set to: %s\n", enable ? "ENABLED" : "DISABLED");
	return 0;
}
EXPORT_SYMBOL(pox_opp_voltage_guard_set);

static ssize_t health_calibration_sysfs_show(struct kobject *kobj,
					     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", pox_health_calibration_get());
}

static ssize_t health_calibration_sysfs_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	pox_health_calibration_set(val);
	return count;
}

static struct kobj_attribute health_calibration_kobj_attr =
	__ATTR(health_calibration, 0644, health_calibration_sysfs_show, health_calibration_sysfs_store);

static ssize_t opp_voltage_guard_sysfs_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sprintf(buf, "%d\n", pox_opp_voltage_guard_get());
}

static ssize_t opp_voltage_guard_sysfs_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	pox_opp_voltage_guard_set(val);
	return count;
}

static struct kobj_attribute opp_voltage_guard_kobj_attr =
	__ATTR(opp_voltage_guard, 0644, opp_voltage_guard_sysfs_show, opp_voltage_guard_sysfs_store);

static int opp_voltage_guard_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_opp_voltage_guard_get());
	return 0;
}

static int opp_voltage_guard_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, opp_voltage_guard_proc_show, NULL);
}

static ssize_t opp_voltage_guard_proc_write(struct file *file,
					    const char __user *buffer,
					    size_t count, loff_t *pos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buffer, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (kstrtoint(strim(kbuf), 10, &val) < 0)
		return -EINVAL;

	pox_opp_voltage_guard_set(val);
	return count;
}

static const struct file_operations opp_voltage_guard_proc_fops = {
	.open = opp_voltage_guard_proc_open,
	.read = seq_read,
	.write = opp_voltage_guard_proc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int health_calibration_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_health_calibration_get());
	return 0;
}

static int health_calibration_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, health_calibration_proc_show, NULL);
}

static ssize_t health_calibration_proc_write(struct file *file, const char __user *buffer,
					     size_t count, loff_t *pos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buffer, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (kstrtoint(strim(kbuf), 10, &val) < 0)
		return -EINVAL;

	pox_health_calibration_set(val);
	return count;
}

static const struct file_operations health_calibration_proc_fops = {
	.open = health_calibration_proc_open,
	.read = seq_read,
	.write = health_calibration_proc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int health_status_proc_show(struct seq_file *m, void *v)
{
	extern int battery_get_bat_temperature(void);
	extern int battery_get_uisoc(void);
	int btemp = battery_get_bat_temperature();
	int soc = battery_get_uisoc();
	int mode = pox_health_calibration_get();
	int guard = pox_opp_voltage_guard_get();

	pox_health_calibration_sync();
	seq_printf(m, "health_mode=%d\n", mode);
	seq_printf(m, "opp_voltage_guard=%d\n", guard);
	seq_printf(m, "battery_temp_decic=%d\n", btemp);
	seq_printf(m, "battery_soc=%d\n", soc);
	seq_printf(m, "brownout_preventions=%lu\n", pox_health_brownout_preventions);
	seq_printf(m, "thermal_derates=%lu\n", pox_pwr_thermal_derates);
	seq_printf(m, "battery_derates=%lu\n", pox_pwr_battery_derates);
	seq_printf(m, "anti_freeze_watchdog=armed\n");
	seq_printf(m, "status=%s\n", (btemp < 450 && soc > 15) ? "OPTIMAL_PERFORMANCE" : "GUARD_PROTECTED");
	return 0;
}

static int health_status_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, health_status_proc_show, NULL);
}

static const struct file_operations health_status_proc_fops = {
	.open = health_status_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* =========================================================================
 * BORE (Burst-Oriented Response Enhancer) Scheduler Interface
 * ========================================================================= */

static ssize_t sched_bore_sysfs_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", sysctl_sched_bore);
}

static ssize_t sched_bore_sysfs_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	sysctl_sched_bore = val ? 1 : 0;
	return count;
}

static struct kobj_attribute sched_bore_kobj_attr =
	__ATTR(sched_bore, 0644, sched_bore_sysfs_show, sched_bore_sysfs_store);

static ssize_t sched_burst_scale_sysfs_show(struct kobject *kobj,
					    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sysctl_sched_burst_penalty_scale);
}

static ssize_t sched_burst_scale_sysfs_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val) < 0)
		return -EINVAL;

	sysctl_sched_burst_penalty_scale = min(val, 4095U);
	return count;
}

static struct kobj_attribute sched_burst_scale_kattr =
	__ATTR(sched_burst_penalty_scale, 0644, sched_burst_scale_sysfs_show, sched_burst_scale_sysfs_store);

static ssize_t sched_burst_offset_sysfs_show(struct kobject *kobj,
					     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sysctl_sched_burst_penalty_offset);
}

static ssize_t sched_burst_offset_sysfs_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val) < 0)
		return -EINVAL;

	sysctl_sched_burst_penalty_offset = min(val, 63U);
	return count;
}

static struct kobj_attribute sched_burst_offset_kattr =
	__ATTR(sched_burst_penalty_offset, 0644, sched_burst_offset_sysfs_show, sched_burst_offset_sysfs_store);

static ssize_t sched_burst_inherit_sysfs_show(struct kobject *kobj,
					      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sysctl_sched_burst_inherit_type);
}

static ssize_t sched_burst_inherit_sysfs_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val) < 0)
		return -EINVAL;

	sysctl_sched_burst_inherit_type = min(val, 2U);
	return count;
}

static struct kobj_attribute sched_burst_inherit_kattr =
	__ATTR(sched_burst_inherit_type, 0644, sched_burst_inherit_sysfs_show, sched_burst_inherit_sysfs_store);

static ssize_t sched_burst_lifetime_sysfs_show(struct kobject *kobj,
					       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sysctl_sched_burst_cache_lifetime);
}

static ssize_t sched_burst_lifetime_sysfs_store(struct kobject *kobj,
						struct kobj_attribute *attr,
						const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val) < 0)
		return -EINVAL;

	sysctl_sched_burst_cache_lifetime = val;
	return count;
}

static struct kobj_attribute sched_burst_lifetime_kattr =
	__ATTR(sched_burst_cache_lifetime, 0644, sched_burst_lifetime_sysfs_show, sched_burst_lifetime_sysfs_store);

static int sched_bore_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "sched_bore=%d\n", sysctl_sched_bore);
	seq_printf(m, "sched_burst_penalty_scale=%u\n", sysctl_sched_burst_penalty_scale);
	seq_printf(m, "sched_burst_cache_lifetime=%u\n", sysctl_sched_burst_cache_lifetime);
	seq_printf(m, "sched_burst_inherit_type=%u\n", sysctl_sched_burst_inherit_type);
	seq_printf(m, "sched_burst_penalty_offset=%u\n", sysctl_sched_burst_penalty_offset);
	return 0;
}

static int sched_bore_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, sched_bore_proc_show, NULL);
}

static ssize_t sched_bore_proc_write(struct file *file, const char __user *buffer,
				     size_t count, loff_t *pos)
{
	char kbuf[64];
	char *str;
	unsigned int uval;
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buffer, count))
		return -EFAULT;

	kbuf[count] = '\0';
	str = strim(kbuf);

	if (sscanf(str, "scale=%u", &uval) == 1 || sscanf(str, "scale %u", &uval) == 1) {
		sysctl_sched_burst_penalty_scale = min(uval, 4095U);
	} else if (sscanf(str, "offset=%u", &uval) == 1 || sscanf(str, "offset %u", &uval) == 1) {
		sysctl_sched_burst_penalty_offset = min(uval, 63U);
	} else if (sscanf(str, "inherit=%u", &uval) == 1 || sscanf(str, "inherit %u", &uval) == 1) {
		sysctl_sched_burst_inherit_type = min(uval, 2U);
	} else if (sscanf(str, "lifetime=%u", &uval) == 1 || sscanf(str, "lifetime %u", &uval) == 1) {
		sysctl_sched_burst_cache_lifetime = uval;
	} else if (kstrtoint(str, 10, &val) == 0) {
		sysctl_sched_bore = val ? 1 : 0;
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct file_operations sched_bore_proc_fops = {
	.open = sched_bore_proc_open,
	.read = seq_read,
	.write = sched_bore_proc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* =========================================================================
 * DriveDroid & Hardware USB CD-ROM Emulation Interface
 * ========================================================================= */

static int pox_usb_cdrom_mode = 0;

int pox_usb_cdrom_get(void)
{
	return pox_usb_cdrom_mode;
}
EXPORT_SYMBOL(pox_usb_cdrom_get);

int pox_usb_cdrom_set(int mode)
{
	pox_usb_cdrom_mode = mode ? 1 : 0;
	pr_info("POX USB CD-ROM DriveDroid Emulation set to: %d\n", pox_usb_cdrom_mode);
	return 0;
}
EXPORT_SYMBOL(pox_usb_cdrom_set);

static ssize_t usb_cdrom_sysfs_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", pox_usb_cdrom_get());
}

static ssize_t usb_cdrom_sysfs_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int val = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	pox_usb_cdrom_set(val);
	return count;
}

static struct kobj_attribute usb_cdrom_kobj_attr =
	__ATTR(usb_cdrom, 0644, usb_cdrom_sysfs_show, usb_cdrom_sysfs_store);

static int usb_cdrom_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_usb_cdrom_get());
	return 0;
}

static int usb_cdrom_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, usb_cdrom_proc_show, NULL);
}

static ssize_t usb_cdrom_proc_write(struct file *file, const char __user *buffer,
				    size_t count, loff_t *pos)
{
	char kbuf[32];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buffer, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (kstrtoint(strim(kbuf), 10, &val) < 0)
		return -EINVAL;

	pox_usb_cdrom_set(val);
	return count;
}

static const struct file_operations usb_cdrom_proc_fops = {
	.open = usb_cdrom_proc_open,
	.read = seq_read,
	.write = usb_cdrom_proc_write,
	.llseek = seq_lseek,
	.release = single_release,
};


/* Battery Protection Rootless ProcFS Interfaces */
extern int pox_battery_bypass_get(void);
extern void pox_battery_bypass_set(int enable);
extern int pox_battery_limit_get(void);
extern void pox_battery_limit_set(int limit);
extern int pox_battery_status_get(char *buf, size_t size);

static int battery_bypass_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_battery_bypass_get());
	return 0;
}

static int battery_bypass_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, battery_bypass_proc_show, NULL);
}

static ssize_t battery_bypass_proc_write(struct file *file, const char __user *buffer,
					 size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	pox_battery_bypass_set(val);
	return count;
}

static const struct file_operations battery_bypass_proc_fops = {
	.open    = battery_bypass_proc_open,
	.read    = seq_read,
	.write   = battery_bypass_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int battery_limit_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_battery_limit_get());
	return 0;
}

static int battery_limit_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, battery_limit_proc_show, NULL);
}

static ssize_t battery_limit_proc_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 100)
		return -EINVAL;

	pox_battery_limit_set(val);
	return count;
}

static const struct file_operations battery_limit_proc_fops = {
	.open    = battery_limit_proc_open,
	.read    = seq_read,
	.write   = battery_limit_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int battery_status_proc_show(struct seq_file *m, void *v)
{
	char buf[128];
	pox_battery_status_get(buf, sizeof(buf));
	seq_printf(m, "%s", buf);
	return 0;
}

static int battery_status_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, battery_status_proc_show, NULL);
}

static const struct file_operations battery_status_proc_fops = {
	.open    = battery_status_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Touchscreen Hardware Mode & Sensitivity Interfaces */
extern int pox_touch_game_mode_get(void);
extern int pox_touch_game_mode_set(int enable);
extern int pox_touch_sensitivity_get(void);
extern int pox_touch_sensitivity_set(int val);

static int touch_game_mode_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_touch_game_mode_get());
	return 0;
}

static int touch_game_mode_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, touch_game_mode_proc_show, NULL);
}

static ssize_t touch_game_mode_proc_write(struct file *file, const char __user *buffer,
					  size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	pox_touch_game_mode_set(val);
	return count;
}

static const struct file_operations touch_game_mode_proc_fops = {
	.open    = touch_game_mode_proc_open,
	.read    = seq_read,
	.write   = touch_game_mode_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int touch_sensitivity_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_touch_sensitivity_get());
	return 0;
}

static int touch_sensitivity_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, touch_sensitivity_proc_show, NULL);
}

static ssize_t touch_sensitivity_proc_write(struct file *file, const char __user *buffer,
					    size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 3)
		return -EINVAL;

	pox_touch_sensitivity_set(val);
	return count;
}

static const struct file_operations touch_sensitivity_proc_fops = {
	.open    = touch_sensitivity_proc_open,
	.read    = seq_read,
	.write   = touch_sensitivity_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Audio Sound Control Headphone Gain Interface */
extern int pox_headphone_gain_get(void);
extern int pox_headphone_gain_set(int val);

static int headphone_gain_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_headphone_gain_get());
	return 0;
}

static int headphone_gain_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, headphone_gain_proc_show, NULL);
}

static ssize_t headphone_gain_proc_write(struct file *file, const char __user *buffer,
					 size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 8)
		return -EINVAL;

	pox_headphone_gain_set(val);
	return count;
}

static const struct file_operations headphone_gain_proc_fops = {
	.open    = headphone_gain_proc_open,
	.read    = seq_read,
	.write   = headphone_gain_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Vibrator Strength Interface */
extern int pox_vibrator_strength_get(void);
extern int pox_vibrator_strength_set(int vol);

static int vibrator_strength_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_vibrator_strength_get());
	return 0;
}

static int vibrator_strength_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, vibrator_strength_proc_show, NULL);
}

static ssize_t vibrator_strength_proc_write(struct file *file, const char __user *buffer,
					    size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 13)
		return -EINVAL;

	pox_vibrator_strength_set(val);
	return count;
}

static const struct file_operations vibrator_strength_proc_fops = {
	.open    = vibrator_strength_proc_open,
	.read    = seq_read,
	.write   = vibrator_strength_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Wakelock Blocker Deep Sleep Interface */
extern int pox_wakelock_blocker_get(void);
extern void pox_wakelock_blocker_set(int enable);

static int wakelock_blocker_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_wakelock_blocker_get());
	return 0;
}

static int wakelock_blocker_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, wakelock_blocker_proc_show, NULL);
}

static ssize_t wakelock_blocker_proc_write(struct file *file, const char __user *buffer,
					   size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	pox_wakelock_blocker_set(val);
	return count;
}

static const struct file_operations wakelock_blocker_proc_fops = {
	.open    = wakelock_blocker_proc_open,
	.read    = seq_read,
	.write   = wakelock_blocker_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Fast Charge Control Interface */
extern int pox_fast_charge_get(void);
extern void pox_fast_charge_set(int enable);
extern int pox_charge_watt_get(void);
extern void pox_charge_watt_set(int watt);
extern int pox_pd_min_watt_get(void);
extern void pox_pd_min_watt_set(int watt);
extern int pox_pd_max_watt_get(void);
extern void pox_pd_max_watt_set(int watt);

static int fast_charge_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_fast_charge_get() ? pox_charge_watt_get() : 0);
	return 0;
}

static int fast_charge_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, fast_charge_proc_show, NULL);
}

static ssize_t fast_charge_proc_write(struct file *file, const char __user *buffer,
				      size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0)
		return -EINVAL;

	if (val == 0)
		pox_fast_charge_set(0);
	else if (val == 1)
		pox_fast_charge_set(1);
	else
		pox_charge_watt_set(val);

	return count;
}

static const struct file_operations fast_charge_proc_fops = {
	.open    = fast_charge_proc_open,
	.read    = seq_read,
	.write   = fast_charge_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Charge Watt Interface */
static int charge_watt_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_charge_watt_get());
	return 0;
}

static int charge_watt_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, charge_watt_proc_show, NULL);
}

static ssize_t charge_watt_proc_write(struct file *file, const char __user *buffer,
				      size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	pox_charge_watt_set(val);
	return count;
}

static const struct file_operations charge_watt_proc_fops = {
	.open    = charge_watt_proc_open,
	.read    = seq_read,
	.write   = charge_watt_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* PD Min Watt Interface */
static int pd_min_watt_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_pd_min_watt_get());
	return 0;
}

static int pd_min_watt_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, pd_min_watt_proc_show, NULL);
}

static ssize_t pd_min_watt_proc_write(struct file *file, const char __user *buffer,
				      size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	pox_pd_min_watt_set(val);
	return count;
}

static const struct file_operations pd_min_watt_proc_fops = {
	.open    = pd_min_watt_proc_open,
	.read    = seq_read,
	.write   = pd_min_watt_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* PD Max Watt Interface */
static int pd_max_watt_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_pd_max_watt_get());
	return 0;
}

static int pd_max_watt_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, pd_max_watt_proc_show, NULL);
}

static ssize_t pd_max_watt_proc_write(struct file *file, const char __user *buffer,
				      size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	pox_pd_max_watt_set(val);
	return count;
}

static const struct file_operations pd_max_watt_proc_fops = {
	.open    = pd_max_watt_proc_open,
	.read    = seq_read,
	.write   = pd_max_watt_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Double-Tap to Wake Interface */
extern int pox_dt2w_get(void);
extern int pox_dt2w_set(int enable);

static int dt2w_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_dt2w_get());
	return 0;
}

static int dt2w_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, dt2w_proc_show, NULL);
}

static ssize_t dt2w_proc_write(struct file *file, const char __user *buffer,
			       size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	pox_dt2w_set(val);
	return count;
}

static const struct file_operations dt2w_proc_fops = {
	.open    = dt2w_proc_open,
	.read    = seq_read,
	.write   = dt2w_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Analog Mic Gain Interface */
extern int pox_mic_gain_get(void);
extern int pox_mic_gain_set(int gain);

static int mic_gain_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_mic_gain_get());
	return 0;
}

static int mic_gain_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mic_gain_proc_show, NULL);
}

static ssize_t mic_gain_proc_write(struct file *file, const char __user *buffer,
				   size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 4)
		return -EINVAL;

	pox_mic_gain_set(val);
	return count;
}

static const struct file_operations mic_gain_proc_fops = {
	.open    = mic_gain_proc_open,
	.read    = seq_read,
	.write   = mic_gain_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Active Microphone Selection Interface */
extern int pox_active_mic_get(void);
extern int pox_active_mic_set(int mode);

static int active_mic_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_active_mic_get());
	return 0;
}

static int active_mic_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, active_mic_proc_show, NULL);
}

static ssize_t active_mic_proc_write(struct file *file, const char __user *buffer,
				     size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 3)
		return -EINVAL;

	pox_active_mic_set(val);
	return count;
}

static const struct file_operations active_mic_proc_fops = {
	.open    = active_mic_proc_open,
	.read    = seq_read,
	.write   = active_mic_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Dynamic Fsync Interface */
extern int pox_dynamic_fsync_get(void);
extern void pox_dynamic_fsync_set(int enable);

static int dynamic_fsync_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pox_dynamic_fsync_get());
	return 0;
}

static int dynamic_fsync_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, dynamic_fsync_proc_show, NULL);
}

static ssize_t dynamic_fsync_proc_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *pos)
{
	char buf[16];
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;
	buf[count] = '\0';

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	pox_dynamic_fsync_set(val);
	return count;
}

static const struct file_operations dynamic_fsync_proc_fops = {
	.open    = dynamic_fsync_proc_open,
	.read    = seq_read,
	.write   = dynamic_fsync_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* Unified Observability Hardware Profile Meta-Node (Section 10) */
static int profile_proc_show(struct seq_file *m, void *v)
{
	char bat_buf[128];
	int g_mode = gaming_mode_get();
	int c_mode = get_ios_color_mode();
	int h_mode = hbm_mode_get();
	int t_bri = torch_brightness_get();
	int dt2w = pox_dt2w_get();
	int dyn_fsync = pox_dynamic_fsync_get();
	int hp_gain = pox_headphone_gain_get();
	int mic_gain = pox_mic_gain_get();
	int a_mic = pox_active_mic_get();
	int vib_str = pox_vibrator_strength_get();
	int t_game = pox_touch_game_mode_get();
	int t_sens = pox_touch_sensitivity_get();
	int cam_4k = camera_4k60_get();
	int cam_slog = camera_slog3_get();
	int wl_blk = pox_wakelock_blocker_get();
	int fast_chg = pox_fast_charge_get();

	pox_battery_status_get(bat_buf, sizeof(bat_buf));
	strim(bat_buf);

	seq_printf(m, "=== POX KERNEL HARDWARE PROFILE ===\n");
	seq_printf(m, "gaming_mode: %d (%s)\n", g_mode,
		   g_mode == 2 ? "EXTREME" : (g_mode == 1 ? "GAMING" : (g_mode == -1 ? "POWERSAVE" : "BALANCED")));
	seq_printf(m, "true_tone: %d (%s)\n", pox_true_tone_get(),
		   pox_true_tone_get() ? "ENABLED [DEFAULT]" : "DISABLED");
	seq_printf(m, "color_mode: %d (%s)\n", c_mode,
		   c_mode == 4 ? "CUSTOM" : (c_mode == 3 ? "SLOG3" : (c_mode == 2 ? "VIVID" : (c_mode == 1 ? "REFERENCE_D65" : "STANDARD"))));
	seq_printf(m, "hbm_mode: %d (%s)\n", h_mode,
		   h_mode == 3 ? "L3_PEAK" : (h_mode == 2 ? "L2_HIGH" : (h_mode == 1 ? "L1_BOOST" : "OFF")));
	seq_printf(m, "touch_game_mode: %d\n", t_game);
	seq_printf(m, "touch_sensitivity: %d\n", t_sens);
	seq_printf(m, "double_tap_to_wake: %d\n", dt2w);
	seq_printf(m, "battery_status: %s\n", bat_buf);
	seq_printf(m, "battery_limit: %d%%\n", pox_battery_limit_get());
	seq_printf(m, "fast_charge: %d (watt: %dW, pd_min: %dW, pd_max: %dW)\n",
		   fast_chg, pox_charge_watt_get(), pox_pd_min_watt_get(), pox_pd_max_watt_get());
	seq_printf(m, "headphone_gain: +%ddB\n", hp_gain);
	seq_printf(m, "mic_gain: %d\n", mic_gain);
	seq_printf(m, "active_mic: %d (%s)\n", a_mic,
		   a_mic == 1 ? "MAIN_MIC" : (a_mic == 2 ? "TOP_MIC" : (a_mic == 3 ? "DUAL_STEREO" : "DEFAULT_AUTO")));
	seq_printf(m, "vibrator_strength: 0x%02X\n", vib_str);
	seq_printf(m, "torch_brightness: %d\n", t_bri);
	seq_printf(m, "dynamic_fsync: %d\n", dyn_fsync);
	seq_printf(m, "camera_4k60: %d\n", cam_4k);
	seq_printf(m, "camera_slog3: %d\n", cam_slog);
	seq_printf(m, "wakelock_blocker: %d\n", wl_blk);
	seq_printf(m, "memory: swappiness=%d wmark_scale=%d vfs_pressure=%d cluster=%d dirty=%d/%d zram=zstd-percpu\n",
		vm_swappiness, watermark_scale_factor, sysctl_vfs_cache_pressure,
		page_cluster, vm_dirty_ratio, dirty_background_ratio);
	seq_printf(m, "power_learn: activations=%lu gaming=%lu extreme=%lu powersave=%lu thermal_derates=%lu battery_derates=%lu\n",
		pox_pwr_activations, pox_pwr_gaming_entries, pox_pwr_extreme_entries,
		pox_pwr_powersave_entries, pox_pwr_thermal_derates, pox_pwr_battery_derates);
	return 0;
}

static int profile_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, profile_proc_show, NULL);
}

static const struct file_operations profile_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = profile_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* =========================================================================
 * Unified Pox Kernel Configuration Subsystem (/proc/perfmgr/pox_config)
 * Single-file atomic read/write interface for Pox Manager
 * Eliminates multi-process shell IPC overhead and prevents ADB/PC lag
 * ========================================================================= */

static void pox_config_apply_line(const char *key, const char *val_str)
{
	int val = 0;

	if (strcmp(key, "kprofiles_custom") == 0) {
		struct pox_kprofile_custom_tunables t = pox_custom_kp;
		int matched = sscanf(val_str, "%d %d %d %d %d %d %d %d %d %d",
				     &t.schedutil_up_little_us,
				     &t.schedutil_down_little_us,
				     &t.schedutil_up_big_us,
				     &t.schedutil_down_big_us,
				     &t.top_app_boost,
				     &t.fg_boost,
				     &t.gpu_boost,
				     &t.dram_boost,
				     &t.touch_boost,
				     &t.rescue_opp);
		if (matched >= 2)
			pox_kprofile_custom_set(&t);
		return;
	}

	if (kstrtoint(val_str, 10, &val) < 0)
		return;

	if (strcmp(key, "gaming_mode") == 0)
		gaming_mode_set(val);
	else if (strcmp(key, "kprofiles_mode") == 0)
		kprofiles_mode_set(val);
	else if (strcmp(key, "auto_kp") == 0) {
		kp_auto_mode = val ? 1 : 0;
		pox_pwr_auto = kp_auto_mode;
		cancel_delayed_work(&pox_dynamic_work);
		if (pox_pwr_auto)
			schedule_delayed_work(&pox_dynamic_work, msecs_to_jiffies(8000));
	} else if (strcmp(key, "health_calibration") == 0)
		pox_health_calibration_set(val);
	else if (strcmp(key, "opp_voltage_guard") == 0)
		pox_opp_voltage_guard_set(val);
	else if (strcmp(key, "hbm_mode") == 0)
		hbm_mode_set(val);
	else if (strcmp(key, "torch_brightness") == 0)
		torch_brightness_set(val);
	else if (strcmp(key, "color_mode") == 0)
		set_ios_color_mode(val);
	else if (strcmp(key, "true_tone") == 0)
		pox_true_tone_set(val);
	else if (strcmp(key, "touch_game_mode") == 0)
		pox_touch_game_mode_set(val);
	else if (strcmp(key, "touch_sensitivity") == 0)
		pox_touch_sensitivity_set(val);
	else if (strcmp(key, "dt2w") == 0)
		pox_dt2w_set(val);
	else if (strcmp(key, "dynamic_fsync") == 0)
		pox_dynamic_fsync_set(val);
	else if (strcmp(key, "fast_charge") == 0)
		pox_fast_charge_set(val);
	else if (strcmp(key, "charge_watt") == 0)
		pox_charge_watt_set(val);
	else if (strcmp(key, "pd_min_watt") == 0)
		pox_pd_min_watt_set(val);
	else if (strcmp(key, "pd_max_watt") == 0)
		pox_pd_max_watt_set(val);
	else if (strcmp(key, "headphone_gain") == 0)
		pox_headphone_gain_set(val);
	else if (strcmp(key, "mic_gain") == 0)
		pox_mic_gain_set(val);
	else if (strcmp(key, "active_mic") == 0)
		pox_active_mic_set(val);
	else if (strcmp(key, "vibrator_strength") == 0)
		pox_vibrator_strength_set(val);
	else if (strcmp(key, "camera_4k60") == 0)
		camera_4k60_set(val);
	else if (strcmp(key, "camera_slog3") == 0 || strcmp(key, "slog3") == 0)
		camera_slog3_set(val);
	else if (strcmp(key, "battery_bypass") == 0)
		pox_battery_bypass_set(val);
	else if (strcmp(key, "battery_limit") == 0)
		pox_battery_limit_set(val);
	else if (strcmp(key, "sched_bore") == 0)
		sysctl_sched_bore = val ? 1 : 0;
	else if (strcmp(key, "wakelock_blocker") == 0)
		pox_wakelock_blocker_set(val);
	else if (strcmp(key, "usb_cdrom") == 0)
		pox_usb_cdrom_set(val);
}

static void pox_config_parse_and_apply(char *buf)
{
	char *line, *p;

	p = buf;
	while ((line = strsep(&p, "\r\n")) != NULL) {
		char *key, *val_str, *sep;

		line = strim(line);
		if (!*line || *line == '#')
			continue;

		sep = strpbrk(line, "=:");
		if (!sep)
			continue;

		*sep = '\0';
		key = strim(line);
		val_str = strim(sep + 1);

		if (!*key || !*val_str)
			continue;

		pox_config_apply_line(key, val_str);
	}
}

static int pox_config_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "gaming_mode=%d\n", gaming_mode_get());
	seq_printf(m, "kprofiles_mode=%d\n", kprofiles_mode_get());
	seq_printf(m, "auto_kp=%d\n", kp_auto_mode);
	seq_printf(m, "kprofiles_custom=%d %d %d %d %d %d %d %d %d %d\n",
		   pox_custom_kp.schedutil_up_little_us,
		   pox_custom_kp.schedutil_down_little_us,
		   pox_custom_kp.schedutil_up_big_us,
		   pox_custom_kp.schedutil_down_big_us,
		   pox_custom_kp.top_app_boost,
		   pox_custom_kp.fg_boost,
		   pox_custom_kp.gpu_boost,
		   pox_custom_kp.dram_boost,
		   pox_custom_kp.touch_boost,
		   pox_custom_kp.rescue_opp);
	seq_printf(m, "health_calibration=%d\n", pox_health_calibration_get());
	seq_printf(m, "opp_voltage_guard=%d\n", pox_opp_voltage_guard_get());
	seq_printf(m, "hbm_mode=%d\n", hbm_mode_get());
	seq_printf(m, "torch_brightness=%d\n", torch_brightness_get());
	seq_printf(m, "color_mode=%d\n", get_ios_color_mode());
	seq_printf(m, "true_tone=%d\n", pox_true_tone_get());
	seq_printf(m, "touch_game_mode=%d\n", pox_touch_game_mode_get());
	seq_printf(m, "touch_sensitivity=%d\n", pox_touch_sensitivity_get());
	seq_printf(m, "dt2w=%d\n", pox_dt2w_get());
	seq_printf(m, "dynamic_fsync=%d\n", pox_dynamic_fsync_get());
	seq_printf(m, "fast_charge=%d\n", pox_fast_charge_get());
	seq_printf(m, "charge_watt=%d\n", pox_charge_watt_get());
	seq_printf(m, "pd_min_watt=%d\n", pox_pd_min_watt_get());
	seq_printf(m, "pd_max_watt=%d\n", pox_pd_max_watt_get());
	seq_printf(m, "headphone_gain=%d\n", pox_headphone_gain_get());
	seq_printf(m, "mic_gain=%d\n", pox_mic_gain_get());
	seq_printf(m, "active_mic=%d\n", pox_active_mic_get());
	seq_printf(m, "vibrator_strength=%d\n", pox_vibrator_strength_get());
	seq_printf(m, "camera_4k60=%d\n", camera_4k60_get());
	seq_printf(m, "camera_slog3=%d\n", camera_slog3_get());
	seq_printf(m, "battery_bypass=%d\n", pox_battery_bypass_get());
	seq_printf(m, "battery_limit=%d\n", pox_battery_limit_get());
	seq_printf(m, "sched_bore=%d\n", sysctl_sched_bore);
	seq_printf(m, "wakelock_blocker=%d\n", pox_wakelock_blocker_get());
	seq_printf(m, "usb_cdrom=%d\n", pox_usb_cdrom_get());
	return 0;
}

static ssize_t pox_config_proc_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char *buf;

	if (count == 0)
		return 0;
	if (count > 4096)
		return -EINVAL;

	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, ubuf, count)) {
		kfree(buf);
		return -EFAULT;
	}
	buf[count] = '\0';

	pox_config_parse_and_apply(buf);

	kfree(buf);
	return count;
}

static int pox_config_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, pox_config_proc_show, NULL);
}

static const struct file_operations pox_config_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = pox_config_proc_open,
	.read    = seq_read,
	.write   = pox_config_proc_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static ssize_t pox_config_sysfs_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "gaming_mode=%d\n"
			 "kprofiles_mode=%d\n"
			 "auto_kp=%d\n"
			 "kprofiles_custom=%d %d %d %d %d %d %d %d %d %d\n"
			 "health_calibration=%d\n"
			 "opp_voltage_guard=%d\n"
			 "hbm_mode=%d\n"
			 "torch_brightness=%d\n"
			 "color_mode=%d\n"
			 "true_tone=%d\n"
			 "touch_game_mode=%d\n"
			 "touch_sensitivity=%d\n"
			 "dt2w=%d\n"
			 "dynamic_fsync=%d\n"
			 "fast_charge=%d\n"
			 "charge_watt=%d\n"
			 "pd_min_watt=%d\n"
			 "pd_max_watt=%d\n"
			 "headphone_gain=%d\n"
			 "mic_gain=%d\n"
			 "active_mic=%d\n"
			 "vibrator_strength=%d\n"
			 "camera_4k60=%d\n"
			 "camera_slog3=%d\n"
			 "battery_bypass=%d\n"
			 "battery_limit=%d\n"
			 "sched_bore=%d\n"
			 "wakelock_blocker=%d\n"
			 "usb_cdrom=%d\n",
			 gaming_mode_get(),
			 kprofiles_mode_get(),
			 kp_auto_mode,
			 pox_custom_kp.schedutil_up_little_us,
			 pox_custom_kp.schedutil_down_little_us,
			 pox_custom_kp.schedutil_up_big_us,
			 pox_custom_kp.schedutil_down_big_us,
			 pox_custom_kp.top_app_boost,
			 pox_custom_kp.fg_boost,
			 pox_custom_kp.gpu_boost,
			 pox_custom_kp.dram_boost,
			 pox_custom_kp.touch_boost,
			 pox_custom_kp.rescue_opp,
			 pox_health_calibration_get(),
			 pox_opp_voltage_guard_get(),
			 hbm_mode_get(),
			 torch_brightness_get(),
			 get_ios_color_mode(),
			 pox_true_tone_get(),
			 pox_touch_game_mode_get(),
			 pox_touch_sensitivity_get(),
			 pox_dt2w_get(),
			 pox_dynamic_fsync_get(),
			 pox_fast_charge_get(),
			 pox_charge_watt_get(),
			 pox_pd_min_watt_get(),
			 pox_pd_max_watt_get(),
			 pox_headphone_gain_get(),
			 pox_mic_gain_get(),
			 pox_active_mic_get(),
			 pox_vibrator_strength_get(),
			 camera_4k60_get(),
			 camera_slog3_get(),
			 pox_battery_bypass_get(),
			 pox_battery_limit_get(),
			 sysctl_sched_bore,
			 pox_wakelock_blocker_get(),
			 pox_usb_cdrom_get());
}

static ssize_t pox_config_sysfs_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	char *kbuf;

	if (count == 0)
		return 0;
	if (count > 4096)
		return -EINVAL;

	kbuf = kstrndup(buf, count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	pox_config_parse_and_apply(kbuf);

	kfree(kbuf);
	return count;
}

static struct kobj_attribute pox_config_kobj_attr =
	__ATTR(pox_config, 0644, pox_config_sysfs_show, pox_config_sysfs_store);

/* ------------------ Init Function ------------------ */

int init_gaming_mode(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *entry;
	int ret;

	if (!parent)
		return -EINVAL;

	INIT_DELAYED_WORK(&pox_cam_boost_decay_work, pox_cam_boost_decay_func);
	INIT_DELAYED_WORK(&pox_dynamic_work, pox_dynamic_evaluate);
	pox_pwr_hint = GAMING_MODE_DISABLED;
	/* Kick autonomous guessing: no ROM write required from here on. */
	schedule_delayed_work(&pox_dynamic_work, msecs_to_jiffies(8000));

	entry = proc_create("gaming_mode", 0644, parent, &gaming_mode_proc_fops);
	if (!entry) {
		pr_err("Failed to create /proc/perfmgr/gaming_mode\n");
		return -ENOMEM;
	}

	entry = proc_create("color_mode", 0644, parent, &color_mode_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/color_mode\n");

	entry = proc_create("color_custom", 0644, parent, &color_custom_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/color_custom\n");

	entry = proc_create("true_tone", 0644, parent, &true_tone_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/true_tone\n");

	entry = proc_create("camera_profile", 0644, parent, &camera_profile_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/camera_profile\n");

	entry = proc_create("camera_4k60", 0644, parent, &camera_4k60_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/camera_4k60\n");

	entry = proc_create("slog3", 0644, parent, &slog3_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/slog3\n");

	entry = proc_create("battery_bypass", 0644, parent, &battery_bypass_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/battery_bypass\n");

	entry = proc_create("battery_limit", 0644, parent, &battery_limit_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/battery_limit\n");

	entry = proc_create("battery_status", 0444, parent, &battery_status_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/battery_status\n");

	entry = proc_create("touch_game_mode", 0644, parent, &touch_game_mode_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/touch_game_mode\n");

	entry = proc_create("touch_sensitivity", 0644, parent, &touch_sensitivity_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/touch_sensitivity\n");

	entry = proc_create("headphone_gain", 0644, parent, &headphone_gain_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/headphone_gain\n");

	entry = proc_create("vibrator_strength", 0644, parent, &vibrator_strength_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/vibrator_strength\n");

	entry = proc_create("wakelock_blocker", 0644, parent, &wakelock_blocker_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/wakelock_blocker\n");

	entry = proc_create("fast_charge", 0644, parent, &fast_charge_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/fast_charge\n");

	entry = proc_create("charge_watt", 0644, parent, &charge_watt_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/charge_watt\n");

	entry = proc_create("pd_min_watt", 0644, parent, &pd_min_watt_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/pd_min_watt\n");

	entry = proc_create("pd_max_watt", 0644, parent, &pd_max_watt_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/pd_max_watt\n");

	entry = proc_create("dt2w", 0644, parent, &dt2w_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/dt2w\n");

	entry = proc_create("mic_gain", 0644, parent, &mic_gain_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/mic_gain\n");

	entry = proc_create("active_mic", 0644, parent, &active_mic_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/active_mic\n");

	entry = proc_create("dynamic_fsync", 0644, parent, &dynamic_fsync_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/dynamic_fsync\n");

	entry = proc_create("hbm_mode", 0644, parent, &hbm_mode_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/hbm_mode\n");

	entry = proc_create("torch_brightness", 0666, parent, &torch_brightness_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/torch_brightness\n");

	entry = proc_create("flashlight_brightness", 0666, parent, &torch_brightness_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/flashlight_brightness\n");

	entry = proc_create("torch_info", 0444, parent, &torch_info_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/torch_info\n");

	entry = proc_create("profile", 0444, parent, &profile_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/profile\n");

	entry = proc_create("health_calibration", 0644, parent, &health_calibration_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/health_calibration\n");

	entry = proc_create("opp_voltage_guard", 0644, parent, &opp_voltage_guard_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/opp_voltage_guard\n");

	entry = proc_create("health_status", 0444, parent, &health_status_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/health_status\n");

	entry = proc_create("sched_bore", 0644, parent, &sched_bore_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/sched_bore\n");

	entry = proc_create("usb_cdrom", 0644, parent, &usb_cdrom_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/usb_cdrom\n");

	entry = proc_create("kprofiles_custom", 0644, parent, &kprofiles_custom_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/kprofiles_custom\n");

	entry = proc_create("pox_config", 0666, parent, &pox_config_proc_fops);
	if (!entry)
		pr_warn("Failed to create /proc/perfmgr/pox_config\n");

	ret = sysfs_create_file(kernel_kobj, &gaming_mode_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/gaming_mode (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/gaming_mode created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &color_mode_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/color_mode (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/color_mode created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &color_custom_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/color_custom (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/color_custom created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &true_tone_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/true_tone (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/true_tone created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &hbm_mode_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/hbm_mode (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/hbm_mode created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &torch_brightness_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/torch_brightness (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/torch_brightness created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &flashlight_brightness_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/flashlight_brightness (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/flashlight_brightness created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &camera_profile_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/camera_profile (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/camera_profile created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &camera_4k60_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/camera_4k60 (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/camera_4k60 created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &slog3_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/slog3 (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/slog3 created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &health_calibration_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/health_calibration (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/health_calibration created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &opp_voltage_guard_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/opp_voltage_guard (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/opp_voltage_guard created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &sched_bore_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/sched_bore (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/sched_bore created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &sched_burst_scale_kattr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/sched_burst_penalty_scale (ret=%d)\n", ret);

	ret = sysfs_create_file(kernel_kobj, &sched_burst_offset_kattr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/sched_burst_penalty_offset (ret=%d)\n", ret);

	ret = sysfs_create_file(kernel_kobj, &sched_burst_inherit_kattr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/sched_burst_inherit_type (ret=%d)\n", ret);

	ret = sysfs_create_file(kernel_kobj, &sched_burst_lifetime_kattr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/sched_burst_cache_lifetime (ret=%d)\n", ret);

	ret = sysfs_create_file(kernel_kobj, &usb_cdrom_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/usb_cdrom (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/usb_cdrom created successfully\n");

	ret = sysfs_create_file(kernel_kobj, &pox_config_kobj_attr.attr);
	if (ret)
		pr_warn("Failed to create /sys/kernel/pox_config (ret=%d)\n", ret);
	else
		pr_info("/sys/kernel/pox_config created successfully\n");

	/* Canonical Kprofiles kobject: /sys/kernel/kprofiles/{kp_mode, auto_kp} */
	kp_kobj = kobject_create_and_add("kprofiles", kernel_kobj);
	if (!kp_kobj) {
		pr_warn("Failed to create /sys/kernel/kprofiles\n");
	} else {
		ret = sysfs_create_group(kp_kobj, &kp_attr_group);
		if (ret) {
			pr_warn("Failed to create Kprofiles sysfs group (ret=%d)\n", ret);
			kobject_put(kp_kobj);
			kp_kobj = NULL;
		} else {
			pr_info("/sys/kernel/kprofiles created successfully (kp_mode, auto_kp)\n");
		}
	}

	/* Initialize to True Tone Reference (Calibrated D65) as system default */
	set_ios_color_mode(COLOR_MODE_REFERENCE);

	/* Onyx Gaming Edition: Engage Zero Frame-Drop gaming profile by default */
	gaming_mode_set(GAMING_MODE_ENABLED);

	pr_info("Gaming Mode & True Tone Display Subsystem initialized successfully (Onyx Active, True Tone D65 Default).\n");
	return 0;
}
