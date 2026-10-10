# Pox Kernel for Redmi Note 8 Pro (begonia) 🇵🇸 #FreePalestine

[![GitHub Repo](https://img.shields.io/badge/GitHub-POX--project%2Fpox--begonia--kernel-181717.svg?logo=github)](https://github.com/POX-project/pox-begonia-kernel)
[![Kernel Version](https://img.shields.io/badge/Kernel-Linux%204.14.357-green.svg?logo=linux)](https://github.com/POX-project/pox-begonia-kernel)
[![Pox Release](https://img.shields.io/badge/Pox%20Release-v1.0.0--Beta-blue.svg)](https://github.com/POX-project/pox-begonia-kernel)
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
> * **Older Versions (< 1.0.0-Beta / v0.9 & Prior) are OLD & TESTING ONLY**: All releases prior to **1.0.0-Beta** (including v0.9 and earlier tags) are strictly classified as **OLD, DEPRECATED, AND FOR HISTORICAL / TESTING PURPOSES ONLY**. They lack essential hardware overcurrent protection, stability hotfixes, and vendor HAL data abort immunizations.
> * **NO LIABILITY / WARRANTY DISCLAIMER**: We are **NOT responsible** for anything that happens to you, your device, bricked motherboards, hardware brownouts, bootloops, data loss, or failed alarms. You flash completely at your own risk.
> * **FOLLOW MAINLINE**: For daily driving and stability, always **follow mainline** and only install active supported releases from the current branch matrix (`onyx`, `onyx-sultan-apatch`, etc.). Never flash obsolete, unmaintained legacy builds.

---

## Table of Contents

- [Overview & Hardware Specifications](#overview--hardware-specifications)
- [Release Evolution: 0.9 Legacy to 1.0.0-Beta Mainline](#release-evolution-09-legacy-to-100-beta-mainline)
  - [Version 0.9 (Legacy / Testing Only - Deprecated)](#1-version-09-legacy--testing-only---deprecated)
  - [Version 1.0.0-Beta (Mainline / Active Supported)](#2-version-100-beta-mainline--active-supported)
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

## Release Evolution: 0.9 Legacy to 1.0.0-Beta Mainline

### 1. Version 0.9 (Legacy / Testing Only - Deprecated)
> **STATUS: OBSOLETE & TESTING ONLY.** Do not use for daily driving. Follow mainline.

The 0.9 series introduced the initial prototype Rock Editions:
* **Granite (0.9 LTS)**: Foundational hardware crash mitigation (UVLO call brownouts, SCP SensorHub deep-sleep watchdog).
* **Obsidian (0.9 iOS-Memory)**: Proactive `watermark_scale_factor = 150`, `page-cluster = 0` ZRAM tuning, and asynchronous memory compaction.
* **Onyx (0.9 Gaming)**: First unified gaming mode controller (`/proc/perfmgr/gaming_mode`), 500μs Schedutil frequency ramps, and FPSGO V3 frame rescue.

*Known 0.9 Limitations*: Lacks universal filesystem backports, lacks safe PC USB SDP power limits, and lacks the EL1 Data Abort fix on `sysctl_sched_migration_cost`.

---

### 2. Version 1.0.0-Beta (Mainline / Active Supported)
> **STATUS: CURRENT RECOMMENDED MAINLINE RELEASE.**

The 1.0.0-Beta release upgrades Pox Kernel into a modern downstream distribution across the confirmed 6-branch matrix:
* **Universal Filesystem Engine**: Integrated EROFS, modernized F2FS with rapid garbage collection, Btrfs with ZSTD compression and subvolumes, native NTFS (read/write), exFAT, and FAT32.
* **18W Fast Charging + Safe 500mA PC USB**: Full Pump Express 2.0 (PE+ 2.0) 3.0A/18W fast charging on AC wall adapters with strict 500mA PC USB SDP enforcement to protect host motherboards and maintain rock-solid ADB connectivity.
* **HAL Panic Immunization**: Fully resolved Xiaomi Power HAL EL1 Data Abort crash by ensuring `sysctl_sched_migration_cost` remains writable in userspace.
* **Sultan Debloat Architecture**: Optional zero-telemetry, zero-logging-jitter editions on dedicated `onyx-sultan*` branches.
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
                                  │  ONYX ROOT   │                                                  │ ONYX SULTAN  │
                                  │   FLAVORS    │                                                  │   DEBLOAT    │
                                  └──────┬───────┘                                                  └──────┬───────┘
                                         │                                                                 │
                                         ├─ onyx (Clean Unrooted)                                          ├─ onyx-sultan (Debloated Clean)
                                         ├─ onyx-apatch (APatch Ready)                                     ├─ onyx-sultan-apatch (Debloat + APatch)
                                         └─ onyx-ksu-next (KernelSU-Next)                                  └─ onyx-sultan-ksu-next (Debloat + KSU)
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
* **Strict Branch Isolation**: Exclusively active on `onyx-sultan*` branches to ensure pure, unadulterated stock behavior on standard `onyx*`.
* **Zero Overhead Logging**: Eliminates unnecessary console lock acquisition and string formatting overhead for non-critical kernel logs (`vprintk_emit` filter).
* **Stripped Vendor Spyware & Tracing**: Strips heavy diagnostic hooks, AEE exception recorders, and redundant debug overhead, unlocking higher FPS consistency and superior battery longevity.

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

## Branch Architecture Matrix

| Branch | Edition | Sultan Debloat | Root Subsystem | Description |
|:---|:---|:---:|:---:|:---|
| **`onyx`** | Onyx | ❌ | Clean (Unrooted) | Clean gaming kernel, zero root hooks, pure Play Integrity ready |
| **`onyx-apatch`** | Onyx-APatch | ❌ | APatch / KernelPatch | Onyx Gaming + Full KALLSYMS symbol table for APatch |
| **`onyx-ksu-next`** | Onyx-KSU-Next | ❌ | KernelSU-Next v3.3.0 | Onyx Gaming + Native KernelSU-Next driver & NoMount VFS |
| **`onyx-sultan`** | Onyx-Sultan | ✅ | Clean (Unrooted) | Sultan debloat + Onyx Gaming, pure unrooted edition |
| **`onyx-sultan-apatch`** | Onyx-Sultan-APatch | ✅ | APatch / KernelPatch | Sultan debloat + Onyx Gaming + APatch root support |
| **`onyx-sultan-ksu-next`** | Onyx-Sultan-KSU-Next | ✅ | KernelSU-Next v3.3.0 | Sultan debloat + Onyx Gaming + KernelSU-Next v3.3.0 |
| **`main` / `granite`** | Granite | ❌ | APatch Ready | Long-term stability branch with hardware safety fixes |
| **`obsidian`** | Obsidian | ❌ | APatch Ready | Granite + iOS compressed memory tuning |
| **`onyx-kaeru`** | Onyx-Kaeru | ❌ | APatch / Kaeru | Merged DTB support for Kaeru custom bootloader |

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
adb sideload Pox-1.0.0-Beta-Onyx-Sultan-APatch-<commit>-begonia.zip

# 4. Reboot system
adb reboot
```

---

## On-Device Verification

Once booted, open a terminal or run ADB shell:

```bash
# 1. Verify kernel release string
uname -a
# Expected: Linux localhost 4.14.357-Pox-1.0.0-Beta-...

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

# Checkout the desired branch
git checkout onyx-sultan-apatch

# Compile kernel Image.gz-dtb
./build.sh kernel

# Package the AnyKernel3 flashable zip
./build.sh zip

# Compile and package in one step
./build.sh
```

---

## Repository Governance & Agent Hard Rules

To maintain absolute code quality and prevent regressions, all AI agents and contributors must strictly obey the governance guidelines defined in [`AGENTS.md`](AGENTS.md):

1. **Zero-Mutation Rule**: NEVER edit or stage files while a build, test, or flashing command is actively running.
2. **Anti-Spaghetti Code**: Clean, minimal diffs; standard Linux kernel coding conventions (8-space tabs); no `.rodata` writes on sysctl handlers; standard 500mA PC USB SDP limits.
3. **Anti-Spaghetti Commits**: Strict Conventional Commits (`<type>(<scope>): <summary>`), atomic bisectable changes, with comprehensive technical explanation in the commit body.
4. **Strict Branch Isolation**: Never leak `CONFIG_SULTAN_DEBLOAT` into regular `onyx*` branches. Maintain pure boundaries between root flavors.

---

## Project Roadmap & TODO

- [x] **v0.9 Release**: Granite, Obsidian, and Onyx initial stable rollout.
- [x] **1.0.0-Beta Core Architecture**: Universal filesystem engine (EROFS, F2FS, Btrfs, NTFS, exFAT).
- [x] **18W Fast Charging Engine**: AC charger current boost with safe PC USB 500mA compliance.
- [x] **HAL Panic Fix**: Elimination of EL1 data abort on `sysctl_sched_migration_cost`.
- [x] **Agent Governance**: Strict anti-spaghetti rules and automated quality gates in `AGENTS.md`.
- [x] **POX-project Remote Migration**: Upstream repository forked and synced to `POX-project/pox-begonia-kernel`.
- [ ] **Complete 6-Branch Matrix Builds**: Build and package official 1.0.0-Beta releases across all 6 targets:
  - [x] `onyx-sultan-apatch`
  - [ ] `onyx-apatch`
  - [ ] `onyx-ksu-next`
  - [ ] `onyx`
  - [ ] `onyx-sultan`
  - [ ] `onyx-sultan-ksu-next`
- [ ] **Comparative Benchmark Report**: Full on-device 5-minute Perfetto traces and benchmark comparison between v0.9 baseline and 1.0.0-Beta.
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
