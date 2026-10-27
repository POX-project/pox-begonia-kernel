/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Pox Kernel Project - TXO R
 * Dedicated Pox Zero Frame-Drop Gaming Mode Controller for Redmi Note 8 Pro (begonia)
 */

#ifndef _GAMING_MODE_H_
#define _GAMING_MODE_H_

#include <linux/proc_fs.h>

#define GAMING_MODE_POWERSAVE -1
#define GAMING_MODE_DISABLED   0
#define GAMING_MODE_ENABLED    1
#define GAMING_MODE_EXTREME    2
#define GAMING_MODE_CUSTOM     3
#define KPROFILES_MODE_CUSTOM  4

struct pox_kprofile_custom_tunables {
	int schedutil_up_little_us;
	int schedutil_down_little_us;
	int schedutil_up_big_us;
	int schedutil_down_big_us;
	int top_app_boost;
	int fg_boost;
	int gpu_boost;
	int dram_boost;
	int touch_boost;
	int rescue_opp;
};

#define COLOR_MODE_STANDARD  0
#define COLOR_MODE_REFERENCE 1
#define COLOR_MODE_TRUE_TONE 1
#define COLOR_MODE_VIVID     2
#define COLOR_MODE_SLOG3     3
#define COLOR_MODE_CUSTOM    4

#define HBM_MODE_OFF         0
#define HBM_MODE_L1          1
#define HBM_MODE_L2          2
#define HBM_MODE_L3          3

int gaming_mode_set(int mode);
int gaming_mode_get(void);
int pox_true_tone_set(int enable);
int pox_true_tone_get(void);
int color_mode_set(int mode);
int color_mode_get(void);
int pox_color_custom_set(int sat, int contrast, int brightness, int sat_adj);
void pox_color_custom_get(int *sat, int *contrast, int *brightness, int *sat_adj);
int hbm_mode_set(int mode);
int hbm_mode_get(void);
int camera_4k60_set(int force);
int camera_4k60_get(void);
int camera_slog3_set(int enable);
int camera_slog3_get(void);
void pox_camera_launch_boost(int enable);
int pox_gaming_mode_get(void);
int torch_brightness_set(int val);
int torch_brightness_get(void);
int kprofiles_mode_set(int mode);
int kprofiles_mode_get(void);
int pox_kprofile_custom_set(const struct pox_kprofile_custom_tunables *tunables);
void pox_kprofile_custom_get(struct pox_kprofile_custom_tunables *tunables);
int pox_health_calibration_set(int mode);
int pox_health_calibration_get(void);
int pox_opp_voltage_guard_set(int enable);
int pox_opp_voltage_guard_get(void);
int pox_usb_cdrom_set(int mode);
int pox_usb_cdrom_get(void);
int pox_battery_esr_set(int mohm);
int pox_battery_esr_get(void);
int pox_opp_sag_ceiling_set(int khz);
int pox_opp_sag_ceiling_get(void);
int pox_thermal_gradient_set(int mc_s);
int pox_thermal_gradient_get(void);
int init_gaming_mode(struct proc_dir_entry *parent);

#endif /* _GAMING_MODE_H_ */
