# Pox Kernel for Redmi Note 8 Pro (begonia) 🇵🇸 #FreePalestine

[![GitHub Repo](https://img.shields.io/badge/GitHub-POX--project%2Fpox--begonia--kernel-181717.svg?logo=github)](https://github.com/POX-project/pox-begonia-kernel)
[![Kernel Version](https://img.shields.io/badge/Kernel-Linux%204.14.357-green.svg?logo=linux)](https://github.com/POX-project/pox-begonia-kernel)
[![Pox Release](https://img.shields.io/badge/Pox%20Release-v2.0-blue.svg)](https://github.com/POX-project/pox-begonia-kernel)
[![Device](https://img.shields.io/badge/Device-Redmi%20Note%208%20Pro%20(begonia)-orange.svg?logo=xiaomi)](https://github.com/POX-project/pox-begonia-kernel)
[![SoC](https://img.shields.io/badge/SoC-MediaTek%20Helio%20G90T%20(MT6785)-red.svg)](https://github.com/POX-project/pox-begonia-kernel)
[![Solidarity](https://img.shields.io/badge/Stand%20With-Free%20Palestine%20🇵🇸-red?style=flat&colorA=000000&colorB=007A3D)](https://en.wikipedia.org/wiki/State_of_Palestine)
[![Maintainer](https://img.shields.io/badge/Maintainer-TXO%20R-purple.svg)](https://github.com/txorav)
[![License](https://img.shields.io/badge/License-GPLv2-yellow.svg)](COPYING)

**Pox Kernel** is an advanced, rock-solid custom Linux kernel engineered for the **Xiaomi Redmi Note 8 Pro** (`begonia` / `begonia_in`, MediaTek MT6785 / Helio G90T).

> *"We aim for stability, not for anything else."* — **Stand for Justice. Free Palestine 🇵🇸**

---

> [!CAUTION]
> ### ⚠️ Critical Disclaimer & Legacy Version Advisory
> * **Older Versions (< 2.0 / v0.9 & Prior) are OLD & TESTING ONLY**: All releases prior to **2.0** (including v0.9 and earlier tags) are strictly classified as **OLD, DEPRECATED, AND FOR HISTORICAL / TESTING PURPOSES ONLY**. They lack essential hardware overcurrent protection, stability hotfixes, and vendor HAL data abort immunizations.
> * **NO LIABILITY / WARRANTY DISCLAIMER**: We are **NOT responsible** for anything that happens to you, your device, bricked motherboards, hardware brownouts, bootloops, data loss, or failed alarms. You flash completely at your own risk.
> * **FOLLOW MAINLINE**: For daily driving and stability, always **follow mainline** and only install active supported releases from the current branch matrix (`onyx`, `onyx-sultan-apatch`, etc.). Never flash obsolete, unmaintained legacy builds.

---

## Table of Contents

- [Overview & Hardware Specifications](#overview--hardware-specifications)
- [Release Evolution: 0.9 Legacy to 2.0 Mainline](#release-evolution-09-legacy-to-20-mainline)
  - [Version 0.9 (Legacy / Testing Only - Deprecated)](#1-version-09-legacy--testing-only---deprecated)
  - [Version 2.0 (Mainline / Active Supported)](#2-version-20-mainline--active-supported)
- [Rock Editions & The 6-Branch Matrix](#rock-editions--the-6-branch-matrix)
- [Comprehensive Kernel Addons & Features](#comprehensive-kernel-addons--features)
  - [1. Universal Filesystem & Storage Engine](#1-universal-filesystem--storage-engine)
  - [2. Fast Charging & Power Subsystem](#2-fast-charging--power-subsystem)
  - [3. Zero Frame-Drop Gaming & Sched Engine](#3-zero-frame-drop-gaming--sched-engine)
  - [4. Sultan Debloat Architecture](#4-sultan-debloat-architecture)
  - [5. Hardware Crash & Stability Immunizations](#5-hardware-crash--stability-immunizations)
  - [6. Multi-Flavored Root Ecosystem](#6-multi-flavored-root-ecosystem)
  - [7. Memory & I/O Compression Engine](#7-memory--io-compression-engine)
- [Branch Architecture Matrix](#branch-architecture-matrix)
- [Installation Guide](#installation-guide)
- [On-Device Verification](#on-device-verification)
- [Building from Source](#building-from-source)
- [Repository Governance & Agent Hard Rules](#repository-governance--agent-hard-rules)
- [Project Roadmap & TODO](#project-roadmap--todo)
- [Credits & Acknowledgments](#credits--acknowledgments)
- [Solidarity & Free Palestine](#solidarity--free-palestine)
- [License](#license)

---

## Overview & Hardware Specifications

| Component | Target Specification |
|:---|:---|
| **Device Model** | Xiaomi Redmi Note 8 Pro (`begonia` Global / `begonia_in` India) |
| **SoC** | MediaTek MT6785 (Helio G90T, 12nm FinFET architecture) |
| **CPU Complex** | Octa-Core: 2x Arm Cortex-A76 @ 2.05 GHz + 6x Arm Cortex-A55 @ 2.00 GHz |
| **GPU Complex** | Arm Mali-G76 MC4 (Bifrost Architecture) @ 800 MHz |
| **Base Kernel** | Linux 4.14.357 LTS (arm64 / AArch64) |
| **Partition Scheme** | A-only (`/dev/block/by-name/boot`) |
| **Toolchain** | Google Android Clang 11.0.1 (r383902) + GCC 4.9/9.3 AArch64 Binutils |
| **Packaging** | AnyKernel3 Dynamic Ramdisk Injection (osm0sis template) |
| **Supported OS** | Android 10 (Q), Android 11 (R), Android 12/12L (S), Android 13 (T), Android 14 (U), HyperOS & AOSP |

---

## Release Evolution: 0.9 Legacy to 2.0 Mainline

### 1. Version 0.9 (Legacy / Testing Only - Deprecated)
> **STATUS: OBSOLETE & TESTING ONLY.** Do not use for daily driving. Follow mainline.

The 0.9 series introduced the initial prototype Rock Editions:
* **Granite (0.9 LTS)**: Foundational hardware crash mitigation (UVLO call brownouts, SCP SensorHub deep-sleep watchdog).
* **Obsidian (0.9 iOS-Memory)**: Proactive `watermark_scale_factor = 150`, `page-cluster = 0` ZRAM tuning, and asynchronous memory compaction.
* **Onyx (0.9 Gaming)**: First unified gaming mode controller (`/proc/perfmgr/gaming_mode`), 500μs Schedutil frequency ramps, and FPSGO V3 frame rescue.

*Known 0.9 Limitations*: Lacks universal filesystem backports, lacks safe PC USB SDP power limits, and lacks the EL1 Data Abort fix on `sysctl_sched_migration_cost`.

---

### 2. Version 2.0 (Mainline / Active Supported)
> **STATUS: CURRENT RECOMMENDED MAINLINE RELEASE.**

The 2.0 release upgrades Pox Kernel into a modern downstream distribution:
* **Universal Filesystem Engine**: Integrated EROFS, modernized F2FS with rapid garbage collection, Btrfs with ZSTD compression and subvolumes, native NTFS (read/write), exFAT, and FAT32.
* **18W Fast Charging + Safe 500mA PC USB**: Full Pump Express 2.0 (PE+ 2.0) 3.0A/18W fast charging on AC wall adapters with strict 500mA PC USB SDP enforcement to protect host motherboards and maintain rock-solid ADB connectivity.
* **HAL Panic Immunization**: Fully resolved Xiaomi Power HAL EL1 Data Abort crash by ensuring `sysctl_sched_migration_cost` remains writable in userspace.
* **Universal Sultan Debloat Architecture**: Zero-telemetry, zero-logging-jitter (`CONFIG_SULTAN_DEBLOAT=y`, `CONFIG_SULTAN_NO_LOGS=y`) integrated universally across all targets on the unified `onyx` source branch.
* **Enforced Linux 64-Char UTS_RELEASE**: Dynamically clamped localversion strings in `build.sh` preventing string overflow build failures.
* **Anti-Spaghetti Repository Governance**: Strict [`AGENTS.md`](AGENTS.md) rules enforcing zero-mutation during builds, atomic Conventional Commits, and clean PR quality gates.

---

## Rock Editions & The 6-Branch Matrix

Pox Kernel is structured into modular **Rock Editions**, enabling users to pick the exact balance between stock-clean stability, aggressive debloat, and root hooking engines:

```
                          ┌─────────────────────────────┐
                          │   Pox Kernel (MT6785)       │
                          │   "We aim for stability"    │
                          └──────────────┬──────────────┘
                                         │
        ┌────────────────────────────────┼────────────────────────────────┐
        ▼                                ▼                                ▼
 ┌──────────────┐                 ┌──────────────┐                 ┌──────────────┐
 │   GRANITE    │                 │   OBSIDIAN   │                 │     ONYX     │
 │  (Stability) │                 │ (iOS Memory) │                 │   (Gaming)   │
 └──────┬───────┘                 └──────┬───────┘                 └──────┬───────┘
        │                                │                                │
        ├─ UVLO Call Brownout Fix        ├─ iOS Memory Engine             ├─ Unified Gaming Mode
        ├─ SCP Watchdog Fix              ├─ Watermark Scale 150           ├─ FPSGO Ultra-Rescue
        ├─ PPM Throttling Fix            ├─ Page Cluster 0 (0ns)          ├─ Mali Touch Boost
        └─ APatch KALLSYMS               └─ Multi-Stream LZ4/ZSTD         └─ 500us Schedutil
                                                                          │
                                         ┌────────────────────────────────┴────────────────────────────────┐
                                         ▼                                                                 ▼
                                  ┌──────────────┐                                                  ┌──────────────┐
                                  │ SULTAN BASE  │                                                  │ BUILD-TIME   │
                                  │ (Universal)  │                                                  │   PATCHES    │
                                  └──────┬───────┘                                                  └──────┬───────┘
                                         │                                                                 │
                                         ├─ CONFIG_SULTAN_DEBLOAT=y                                        ├─ VARIANT=vanilla (Clean Base)
                                         ├─ CONFIG_SULTAN_NO_LOGS=y                                        ├─ VARIANT=apatch (APatch + NoMount)
                                         ├─ Zero-logging jitter (loglevel 1)                               ├─ VARIANT=folkpatch (KernelPatch)
                                         └─ Low scheduler migration latency                                └─ VARIANT=ksu,miui,c2 (BakaSU + Overlays)
```

---

## Comprehensive Kernel Addons & Features

### 1. Universal Filesystem & Storage Engine
* **EROFS (Enhanced Read-Only File System)**: Full native support for high-compression system/vendor images with instantaneous page decompression.
* **Modern F2FS (Flash-Friendly File System)**: Backported upstream optimizations, rapid discard background garbage collection, and inline extent caching.
* **Btrfs Integration**: Full in-kernel Btrfs support with transparent ZSTD compression, snapshot subvolumes, and asynchronous chunk allocation.
* **Comprehensive Removable Media Formats**: Built-in support for ext4, ext3, ext2, FAT32/vfat, exFAT, native NTFS (read/write), XFS, ISO9660, UDF (optical/disc media), FUSE, and OverlayFS.

### 2. Fast Charging & Power Subsystem
* **18W Fast Charging Engine**: Enabled MediaTek Pump Express (PE+ 2.0) with up to 3.0A / 3.2A high-speed fast charging on AC wall adapters (`STANDARD_CHARGER` / DCP) with thermal safety guards.
* **Safe PC USB SDP Current Clamping**: Strict compliance with the USB 2.0 Standard Downstream Port specification (500mA / 500000 µA limit on PC data ports). Prevents host motherboard xHCI overcurrent tripping, eliminating USB dropouts during ADB debugging and file transfers.
* **Dynamic MIVR (Minimum Input Voltage Regulation)**: Active monitoring to prevent cable voltage collapse on weak chargers.

### 3. Zero Frame-Drop Gaming & Sched Engine
* **Unified Kernel Gaming Mode Controller**: Exposed sysfs interface at `/proc/perfmgr/gaming_mode`:
  * `0`: Normal everyday balanced governor.
  * `1`: Gaming mode with aggressive frequency hold and tight frame deadlines.
  * `2`: Extreme mode with locked DRAM bandwidth (2133 MHz) and touch boost latency zeroing.
* **MediaTek FPSGO V3 Ultra-Rescue**: Preemptive frame deadline intervention (`rescue_percent = 20%`, variance sensitivity = 15) to prevent stutter during complex 3D scenes.
* **Mali-G76 MC4 Instant Touch Boost**: Immediate clock frequency ramping on touch screen input interrupts, combined with a 20% proactive dynamic performance margin (`DYNAMIC_MARGIN_MODE_PERF`).
* **500μs Schedutil Governor Frequency Ramp**: Reduced scheduler clock ramp intervals from 2000μs down to 500μs with 20ms anti-jitter hold-down.

### 4. Sultan Debloat Architecture (`CONFIG_SULTAN_DEBLOAT=y`)
* **Universal Single-Source Architecture**: Enabled by default across all targets built from the `onyx` branch (`CONFIG_SULTAN_DEBLOAT=y`, `CONFIG_SULTAN_NO_LOGS=y`), guaranteeing debloat benefits across vanilla, APatch, FolkPatch, and KSU builds.
* **Zero Overhead Logging**: Lowers default console log levels (`CONFIG_CONSOLE_LOGLEVEL_DEFAULT=1`, `CONFIG_MESSAGE_LOGLEVEL_DEFAULT=1`) and compiles away noisy `nm_info` mount logging to eliminate console locks, string formatting jitter, and log chatter.
* **Stripped Vendor Diagnostics & Tracing**: Disables redundant tracking overhead (`TOUCHSCREEN_NVT_DEBUG_FS`, `KTRACE`, `SCHED_DEBUG`), unlocking higher FPS consistency and superior battery longevity while safely preserving required MediaTek firmware logging regions.

### 5. Hardware Crash & Stability Immunizations
* **Xiaomi Power HAL `sysctl_sched_migration_cost` Data Abort Fix**: Removed `const_debug` qualifier from `sysctl_sched_migration_cost` in `fair.c` and `sched.h`. Prevents fatal **EL1 Data Abort** kernel panics when Xiaomi's power HAL (`NodeLooperThread`) writes to procfs.
* **Hardware UVLO Battery Collapse Prevention**: Resolves sudden `2sec_reboot` brownouts during high-current operations (voice calls on 2G/3G/4G with battery < 15%).
* **SCP SensorHub Watchdog Protection**: Fixes MT6785 nanohub ringbuffer timeouts during deep sleep.
* **PPM Low-Battery Throttling Bypass**: Prevents severe UI freezing when battery capacity drops below 10%.
* **Linux 64-Character UTS_RELEASE Guard**: Dynamically clamped localversion strings in `build.sh` to strictly obey the Linux kernel 64-character limit.

### 6. Multi-Flavored Root Ecosystem
* **APatch / KernelPatch**: Full `CONFIG_KALLSYMS_ALL=y` symbol exposure with SELinux hook points.
* **KernelSU-Next**: Integrated KernelSU-Next v3.3.0 driver with NoMount VFS support for stealth systemless root.
* **Clean Unrooted**: 100% stock security model with zero hooking modifications for strict banking/Play Integrity compliance.

### 7. Memory & I/O Compression Engine
* **iOS-Style Asynchronous RAM Management**: On-demand page compaction keeping foreground interactive tasks lag-free.
* **`watermark_scale_factor = 150`**: Initiates background `kswapd` page reclaim early, preventing direct reclaim latency spikes.
* **`page-cluster = 0`**: Single-page swapping eliminating read-ahead penalties on compressed ZRAM.
* **Multi-Stream ZRAM with LZ4 / ZSTD**: High-throughput compressed memory swapping.

---

## Target & Variant Matrix (`onyx` Single Source)

`onyx` is the single source branch with Sultan debloat and zero-logging jitter enabled universally. Root flavors and media overlays are modular patch sets under `patches/` applied at build time:

| Target (`VARIANT`) | Edition | Sultan Debloat | Root Subsystem | Description |
|:---|:---|:---:|:---:|:---|
| **`vanilla`** | Onyx (Vanilla) | ✅ | Clean (Unrooted) | Base Onyx Gaming + Sultan debloat, pure unrooted Play Integrity ready |
| **`apatch`** | Onyx-APatch | ✅ | APatch / KernelPatch | Sultan debloat + APatch symbol exposure + NoMount v2.2.0 + bundled installer |
| **`folkpatch`** | Onyx-FolkPatch | ✅ | FolkPatch / KernelPatch | Sultan debloat + FolkPatch symbol exposure + NoMount v2.2.0 + bundled installer |
| **`ksu`** | Onyx-KSU | ✅ | BakaSU (KernelSU) | Sultan debloat + BakaSU v1.0.9 + SuSFS + NoMount v2.2.0 |
| **`miui`** (overlay) | Onyx-MIUI | ✅ | As selected | Sultan debloat + MIUI/HyperOS network hardening and config fragment |
| **`c2`** (overlay) | Onyx-C2 | ✅ | As selected | Sultan debloat + MTK VCU & V4L2 vcodec Codec2 media port |
| **`ksu,miui,c2`** | Onyx-KSU-MIUI-C2 | ✅ | BakaSU (KernelSU) | Full multi-overlay composition with BakaSU root and Codec2 media stack |

---

## Installation Guide

### Prerequisites
1. Redmi Note 8 Pro (`begonia` / `begonia_in`) with an **unlocked bootloader**.
2. Custom Recovery installed (TWRP, OrangeFox, or PBRP).
3. The AnyKernel3 flashable `.zip` corresponding to your preferred branch.

### Flashing via Recovery Sideload (Recommended)
```bash
# 1. Boot device into TWRP Recovery
adb reboot recovery

# 2. Enter Sideload mode on the device
adb shell twrp sideload

# 3. Stream and install the kernel zip
adb sideload Pox-2.0-Onyx-APatch-<commit>-begonia.zip

# 4. Reboot system
adb reboot
```

---

## On-Device Verification

Once booted, open a terminal or run ADB shell:

```bash
# 1. Verify kernel release string
uname -a
# Expected: Linux localhost 4.14.357-Pox-2.0-...

# 2. Check gaming mode status
cat /proc/perfmgr/gaming_mode

# 3. Check fast charge state
cat /sys/devices/platform/charger/fast_charge 2>/dev/null || cat /proc/perfmgr/gaming_mode
```

---

## Building from Source

Pox Kernel includes a completely self-contained build environment:

```bash
# Clone the repository from official POX-project
git clone https://github.com/POX-project/pox-begonia-kernel.git
cd pox-begonia-kernel

# Onyx is the single branch containing everything
git checkout onyx

# Build any target using the patches system (Sultan debloat included by default on all):
VARIANT=vanilla ./build.sh all
VARIANT=apatch ./build.sh all
VARIANT=folkpatch,miui ./build.sh all
VARIANT=ksu,c2 ./build.sh all
VARIANT=ksu,miui,c2 ./build.sh all
```

---

## Repository Governance & Agent Hard Rules

To maintain absolute code quality and prevent regressions, all AI agents and contributors must strictly obey the governance guidelines defined in [`AGENTS.md`](AGENTS.md):

1. **Zero-Mutation Rule**: NEVER edit or stage files while a build, test, or flashing command is actively running.
2. **Anti-Spaghetti Code**: Clean, minimal diffs; standard Linux kernel coding conventions (8-space tabs); no `.rodata` writes on sysctl handlers; standard 500mA PC USB SDP limits.
3. **Anti-Spaghetti Commits**: Strict Conventional Commits (`<type>(<scope>): <summary>`), atomic bisectable changes, with comprehensive technical explanation in the commit body.
4. **Sultan Debloat & Modular Variant Baseline**: `CONFIG_SULTAN_DEBLOAT=y` and `CONFIG_SULTAN_NO_LOGS=y` are universally enabled across all targets on `onyx`. Keep variant patch sets strictly modular and clean-applying in `patches/`.

---

## Project Roadmap & TODO

- [x] **v0.9 Release**: Granite, Obsidian, and Onyx initial stable rollout.
- [x] **v2.0 Core Architecture**: Universal filesystem engine (EROFS, F2FS, Btrfs, NTFS, exFAT), Sultan debloat on base, multi-root variants.
- [x] **18W Fast Charging Engine**: AC charger current boost with safe PC USB 500mA compliance.
- [x] **HAL Panic Fix**: Elimination of EL1 data abort on `sysctl_sched_migration_cost`.
- [x] **Agent Governance**: Strict anti-spaghetti rules and automated quality gates in `AGENTS.md`.
- [x] **POX-project Remote Migration**: Upstream repository forked and synced to `POX-project/pox-begonia-kernel`.
- [x] **Complete Variant Matrix Builds**: Build and package official releases across variants (`vanilla`, `apatch`, `folkpatch`, `ksu`, `miui`, `c2`).
- [x] **Comparative Benchmark Report**: Full on-device 5-minute Perfetto traces and benchmark comparison.
- [ ] **Network & Wi-Fi Zero-Jitter**: Wi-Fi SetCAM latency tuning and TCP BBR v2 upstream backports.
- [ ] **Automated GitHub Actions CI**: Automated multi-branch build pipeline with release artifact generation.

---

## Credits & Acknowledgments

* **Lead Developer & Maintainer**: **TXO R (Pox Project)** ([@txorav](https://github.com/txorav))
* **Linux Kernel Community**: Linus Torvalds and downstream LTS maintainers.
* **MediaTek**: Board support packages and Helio G90T hardware drivers.
* **osm0sis @ XDA**: AnyKernel3 template and flash tools.
* **Sultan Alsawaf (kerneltoast)**: Kernel debloating insights and low-overhead design.
* **Begonia Community**: Testers, developers, and users keeping the Redmi Note 8 Pro thriving.

---

## Solidarity & Free Palestine 🇵🇸

We believe that open-source software and technology should stand for human dignity, justice, and the freedom of all oppressed peoples. 

```
   ████████████████████████████████████████████
   ████████████████████████████████████████████  (Black)
   ████████████████████████████████████████████
   ████████████████████████████████████████████
   ████████████████████████████████████████████  (White)
   ████████████████████████████████████████████
   ████████████████████████████████████████████
   ████████████████████████████████████████████  (Green)
   ████████████████████████████████████████████
          ▲ (Red Triangle on Hoist)
```

> **"Injustice anywhere is a threat to justice everywhere."**  
> We stand in solidarity with the Palestinian people in their struggle for self-determination, freedom, and human rights.  
> **Free Palestine. End the occupation. Peace, justice, and liberty for all.** 🇵🇸

---

## License

Pox Kernel is free software distributed under the terms of the **GNU General Public License version 2 (GPL-2.0)** as published by the Free Software Foundation. See [`COPYING`](COPYING) for complete details.
