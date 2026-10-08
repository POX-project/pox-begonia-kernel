# Pox Kernel for Redmi Note 8 Pro (begonia)

[![GitHub Repo](https://img.shields.io/badge/GitHub-txorav%2Fpox--kernel--begonia-181717.svg?logo=github)](https://github.com/txorav/pox-kernel-begonia)
[![Kernel Version](https://img.shields.io/badge/Kernel-Linux%204.14.357-green.svg?logo=linux)](https://github.com/txorav/pox-kernel-begonia)
[![Pox Release](https://img.shields.io/badge/Pox%20Release-v0.9-blue.svg)](https://github.com/txorav/pox-kernel-begonia)
[![Device](https://img.shields.io/badge/Device-Redmi%20Note%208%20Pro%20(begonia)-orange.svg?logo=xiaomi)](https://github.com/txorav/pox-kernel-begonia)
[![SoC](https://img.shields.io/badge/SoC-MediaTek%20Helio%20G90T%20(MT6785)-red.svg)](https://github.com/txorav/pox-kernel-begonia)
[![Maintainer](https://img.shields.io/badge/Maintainer-TXO%20R-purple.svg)](https://github.com/txorav)
[![License](https://img.shields.io/badge/License-GPLv2-yellow.svg)](COPYING)

**Pox Kernel** is an advanced, rock-solid custom Linux kernel engineered for the **Redmi Note 8 Pro** (`begonia` / `begonia_in`, MediaTek Helio G90T / MT6785).

> *"We aim for stability, not for anything else."*

---

## Table of Contents

- [Overview & Specifications](#overview--specifications)
- [Rock Editions (v0.9)](#rock-editions-v09)
  - [Granite (Stability Foundation)](#1-granite-rock-solid-stability-foundation)
  - [Obsidian (Memory Enhanced)](#2-obsidian-ios-style-compressed-memory)
  - [Onyx (Zero Frame-Drop Gaming)](#3-onyx-zero-frame-drop-gaming)
- [Repository & Branch Organization](#repository--branch-organization)
- [Installation Guide](#installation-guide)
- [Verifying on Device](#verifying-on-device)
- [Building from Source](#building-from-source)
- [Credits & Acknowledgments](#credits--acknowledgments)
- [License](#license)

---

## Overview & Specifications

| Component | Specification |
|:---|:---|
| **Device** | Xiaomi Redmi Note 8 Pro (`begonia` / `begonia_in`) |
| **SoC** | MediaTek MT6785 / Helio G90T (12nm FinFET) |
| **CPU Architecture** | Octa-Core: 2x Arm Cortex-A76 @ 2.05 GHz + 6x Arm Cortex-A55 @ 2.00 GHz |
| **GPU** | Arm Mali-G76 MC4 @ 800 MHz |
| **Base Kernel** | Linux 4.14.357 LTS (arm64 / AArch64) |
| **Partition Scheme** | A-only (`/dev/block/by-name/boot`) |
| **Root Compatibility** | APatch / KernelPatch ready (`CONFIG_KALLSYMS_ALL=y`), Magisk, KernelSU |
| **Compiler Toolchain** | TheRagingBeast TRB Clang 18.0.0 (17092023) |

---

## Rock Editions (v0.9)

Pox Kernel releases are categorized into distinct **Rock Editions**, designed to cater to different daily driving and high-load usage profiles:

```
                          ┌─────────────────────────────┐
                          │     Pox Kernel (MT6785)     │
                          │   "We aim for stability"    │
                          └──────────────┬──────────────┘
                                         │
        ┌────────────────────────────────┼────────────────────────────────┐
        ▼                                ▼                                ▼
 ┌──────────────┐                 ┌──────────────┐                 ┌──────────────┐
 │   GRANITE    │                 │   OBSIDIAN   │                 │     ONYX     │
 │  (v0.9 LTS)  │                 │  (v0.9 iOS)  │                 │ (v0.9 Gaming)│
 └──────┬───────┘                 └──────┬───────┘                 └──────┬───────┘
        │                                │                                │
        ├─ UVLO Call Reboot Fix          ├─ iOS Memory Engine             ├─ Unified Gaming Mode
        ├─ SCP Watchdog Fix              ├─ Watermark Scale 150           ├─ FPSGO Ultra-Rescue
        ├─ PPM Throttling Fix            ├─ Page Cluster 0 (0ns)          ├─ Mali-G76 Touch Boost
        └─ APatch KALLSYMS               └─ Multi-Stream LZ4 ZRAM         └─ 500us Schedutil Ramp
```

### 1. Granite: Rock-Solid Stability Foundation
- **Target Branches**: `main` / `granite`
- **Key Features**:
  - **Hardware UVLO Battery Collapse Prevention**: Resolves sudden `2sec_reboot` brownout panics during high-current operations (voice calls on 2G/3G/4G with battery < 15%).
  - **SCP SensorHub Watchdog Protection**: Fixes MT6785 SensorHub nanohub ringbuffer timeouts during deep sleep.
  - **PPM Low-Battery Lag Elimination**: Disables crippling CPU clock throttling when the battery drops below 10%, ensuring smooth UI responsiveness until shutdown.
  - **APatch / KernelPatch Ready**: Full `CONFIG_KALLSYMS_ALL=y` symbol table exposed for clean, systemless kernel hooking without bootloop risks.

### 2. Obsidian: iOS-Style Compressed Memory
- **Target Branch**: `obsidian`
- **Key Features**:
  - **iOS-Style Asynchronous RAM Management**: Transparent, on-demand memory compaction keeping foreground processes completely jitter-free.
  - **`watermark_scale_factor = 150`**: Increases the buffer between free pages and the low watermark, initiating background kswapd reclaim long before any app frame drops.
  - **`page-cluster = 0`**: Single-page I/O swapping eliminating read-ahead overhead and reducing ZRAM decompression latency to effectively zero.
  - **Balanced Swappiness (100) & Low VFS Pressure (60)**: Keeps active app caches pinned in memory, preventing aggressive background app termination when multitasking.

### 3. Onyx: Zero Frame-Drop Gaming
- **Target Branch**: `onyx`
- **Key Features**:
  - **Unified Kernel Gaming Mode Controller**: Direct kernel interface (`/proc/perfmgr/gaming_mode`) toggleable between Normal (`0`), Gaming (`1`), and Extreme (`2`).
  - **ROM Performance Auto-Trigger**: Automatically activates full gaming profile when Performance Mode or GameSpace is toggled in any ROM (MIUI / HyperOS, LineageOS, PixelOS, Chaldea).
  - **MediaTek FPSGO V3 Ultra-Rescue**: Preemptive frame deadline rescue tightening (`rescue_percent = 20%`, variance sensitivity = 15) and DRAM bandwidth locking to 2133 MHz on heavy draw calls.
  - **Mali-G76 MC4 Instant Touch Boost & Headroom**: Instant GPU clock ramp on input events + 20% proactive DVFS margin (`DYNAMIC_MARGIN_MODE_PERF`).
  - **500μs Schedutil Frequency Ramp**: Reduces scheduler clock step delays from 2000μs down to 500μs with 20ms anti-jitter hold-down.

---

## Repository & Branch Organization

| Branch Name | Edition Name | Version | Purpose & Contents |
|:---|:---|:---:|:---|
| **`main`** / **`granite`** | **Granite** | `0.9` | Official stable release branch with core stability fixes |
| **`obsidian`** | **Obsidian** | `0.9` | Granite + iOS-Style compressed memory engine |
| **`onyx`** | **Onyx** | `0.9` | Obsidian + Zero Frame-Drop Gaming Controller (Unrooted) |
| **`onyx-apatch`** | **Onyx-APatch** | `0.9` | Onyx + APatch & KernelPatch ready (`CONFIG_KALLSYMS_ALL=y`) |
| **`onyx-ksu-next`** | **Onyx-KSU-Next** | `0.9` | Onyx + KernelSU-Next v3.3.0 integration |
| **`onyx-resukisu`** | **Onyx-ReSukiSu** | `0.9` | Onyx + ReSukiSu v4.2.0 + SuSFS v2.3.0 + NoMount 2.0.0 |
| **`onyx-kaeru`** | **Onyx-Kaeru** | `0.9` | Onyx + Kaeru Bootloader compatibility & Standalone Merged DTB + TheRagingBeast TRB Clang 18 toolchain |

---

## Installation Guide

### Prerequisites
1. Redmi Note 8 Pro with an unlocked bootloader.
2. Custom Recovery installed (TWRP, OrangeFox, or PBRP).
3. Download the flashable AnyKernel3 zip corresponding to your preferred Rock Edition.

### Flashing via Recovery
1. Reboot into recovery (`Power` + `Volume Up`).
2. *(Optional but recommended)* Create a NANDroid backup of your current `Boot` partition.
3. Tap **Install** and navigate to your downloaded package:
   - Example: `Pox-0.9-Onyx-onyx-<commit>-begonia.zip`
4. Swipe to confirm flash.
5. The AnyKernel3 installer will verify the device, display the edition details, inject ramdisk init scripts, and flash the kernel image.
6. Tap **Reboot System**.

---

## Verifying on Device

Once booted, verify the installation in your ROM:

### 1. Android Settings
Navigate to **Settings** → **About Phone** → **Android Version / Kernel Version**:
```text
4.14.357-Pox-0.9-Onyx-gaming-2c3077e4c
TXO_R@PoxKernel #2 SMP PREEMPT Thu Sep 17 16:25:05 CET 2026
```

### 2. Terminal / Termux / ADB
```bash
# Check kernel version
uname -r
# Output: 4.14.357-Pox-0.9-Onyx-gaming-2c3077e4c

# Check full compile banner
cat /proc/version
# Output: Linux version 4.14.357-Pox-0.9-Onyx-gaming-2c3077e4c (TXO_R@PoxKernel) ...

# Check gaming mode status (Onyx Edition)
cat /proc/perfmgr/gaming_mode
```

---

## Building from Source

Pox Kernel features a fully self-contained build workflow. No external toolchain installation is necessary.

```bash
# Clone the repository
git clone https://github.com/txorav/pox-kernel-begonia.git
cd pox-kernel-begonia

# Build kernel and package AnyKernel3 flashable zip
./build.sh

# Build specific components
./build.sh kernel      # Compile kernel Image.gz-dtb
./build.sh zip         # Package AnyKernel3 flashable zip
./build.sh menuconfig  # Interactive kernel menuconfig
./build.sh clean       # Clean build objects
./build.sh distclean   # Wipe entire build directory
```

### Dynamic Customization Flags
You can dynamically override build metadata without changing the source tree:
```bash
KERNEL_NAME="Pox" KERNEL_VERSION="0.9" VERSION_NAME="Onyx" ./build.sh
```

---

## Credits & Acknowledgments

- **Lead Developer & Maintainer**: **TXO R (Pox Project)** ([@txorav](https://github.com/txorav))
- **Linux Kernel Organization**: Linus Torvalds and the worldwide Linux kernel developer community.
- **MediaTek Inc.**: MT6785 / Helio G90T board support packages and performance drivers.
- **osm0sis @ XDA**: AnyKernel3 flashable zip packaging template.
- **Redmi Note 8 Pro Community**: Developers and testers keeping `begonia` fast and reliable.

---

## License

Pox Kernel is free software distributed under the terms of the **GNU General Public License version 2 (GPL-2.0)**. Refer to [COPYING](COPYING) for complete licensing terms.
