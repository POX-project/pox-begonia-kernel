# Changelog

All notable changes to the **Pox Kernel** project (`pox-begonia-kernel`) are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.0.0-Beta] - 2026-10-13

> *"We aim for stability, not for anything else."* — **Stand for Justice. Free Palestine 🇵🇸**

### 🌟 Mainline Release Highlights
* **5-Minute Sustained Hardware Benchmark Pass**:
  * Fully verified on physical Xiaomi Redmi Note 8 Pro (Helio G90T / MT6785) running 311s (23 full loops) under continuous heavy load.
  * **Context Switch Latency**: **8.86 µs** (sub-10 µs ultra-responsive multitasking).
  * **Process Fork Overhead**: **0.10 ms** (instant thread/fork creation).
  * **Memory Bandwidth**: **6,310 MB/s** (sustained dual-channel LPDDR4X throughput).
  * **Thermals**: Sustained `< 47°C` with zero thermal throttling.
* **Universal Filesystem Engine**:
  * Native kernel support for **EROFS** (Enhanced Read-Only File System) for modern Android system partitions.
  * Modern **F2FS** backport with rapid background discard garbage collection, inline data optimizations, and atomic write support.
  * Native **Btrfs** support with transparent ZSTD compression, subvolumes, and snapshot management.
  * Full read/write support for **NTFS**, **exFAT**, **FAT32/vfat**, **XFS**, and **UDF**.
* **NoMount 2.1.1 VFS Subsystem (`CONFIG_NOMOUNT=y`)**:
  * Mountless virtual file injection and path redirection engine (`fs/nomount/`).
  * Utilizes kernel `keyctl` control planes, cached red-black trees (`rbtree`), `SRCU` rule storage, IDR UID tracking, and VFS inode/dentry operation hijacking.
  * Zero filesystem mount points created, completely invisible to userspace security detectors.
* **HyperCore xswapd & Adaptive Memory Management**:
  * Proactive background page reclamation engine backported in `mm/vmscan.c`.
  * Dynamic memory pressure triggers eliminate app kill freezes and jank under high multi-tasking RAM pressure.
  * Sysfs init permissions configured for `/dev/memcg/system` and `/sys/module/vmscan/parameters/xswapd_*`.
* **Storage I/O & Cryptography Optimization**:
  * Default UFS I/O scheduler set to zero-overhead **`noop`** for flash storage, bypassing rotational request queues and maximizing NVMe/UFS flash throughput.
  * Kernel crypto user API enabled (`CONFIG_CRYPTO_USER_API_HASH=y`) for hardware-accelerated integrity verification.
* **Power Management & Electrical Specification Compliance**:
  * Full 18W MediaTek Pump Express PE+ 2.0 dual-phase fast charging (up to 3.0A / 18W) on AC wall chargers (`STANDARD_CHARGER` / DCP).
  * Strict hardware-compliant 500mA (500,000 µA) input limit on standard PC USB host ports (`STANDARD_HOST` / SDP) to protect computer motherboards and eliminate USB dropouts during ADB debugging.
* **Xiaomi Power HAL Panic Immunization**:
  * Resolved fatal Xiaomi Power HAL (`NodeLooperThread`) **EL1 Data Abort** kernel panic by preserving `sysctl_sched_migration_cost` in writable `.data` rather than `.rodata`.
* **Sultan Debloat Architecture (`onyx-sultan*`)**:
  * Stripped unneeded vendor diagnostic overhead, logging chatter, and background tracing on dedicated Sultan editions (`CONFIG_SULTAN_DEBLOAT=y`, `CONFIG_SULTAN_NO_LOGS=y`).
  * Dedicated to zero frame-drop gaming and maximum sustained battery endurance.
* **Bundled FolkPatch Root Auto-Flash**:
  * Flashable recovery packages embed the latest FolkPatch release package (`LyraVoid/FolkPatch`).
  * Flashing the recovery zip automatically patches the kernel and stages the FolkPatch manager app to be installed automatically on first boot.
* **Pox Kernel Manager Companion App**:
  * Official companion app (`Pox-Kernel-Manager-v1.0.0-Beta.apk`) with modern dark glassmorphic UI.
  * Real-time thermal and frequency telemetry, governor tuning, battery stats, and one-tap Sultan debloat profile controls.

### 📦 Included Flashable AnyKernel3 Recovery Packages

| Edition | Root Solution / Modules | Package Asset | SHA-256 Checksum |
| :--- | :--- | :--- | :--- |
| **Onyx-Sultan-APatch** | APatch / KernelPatch + NoMount 2.1.1 + Sultan Debloat | `Pox-1.0.0-Beta-Onyx-Sultan-APatch-aff50ad-begonia.zip` | `4c065bbb6451a99e0fd4361e89fe961d57de719b6cecf33e20104e0b37222217` |
| **Onyx-Sultan-APatch (Verified)** | APatch / KernelPatch + Sultan Debloat (Hardware Tested) | `Pox-1.0.0-Beta-Onyx-Sultan-APatch-95c80f5-begonia.zip` | `66287e3deba36a88fd7f6b3ac6b9543ae84bdb92b026808ea7686b9ca68bdd30` |
| **Onyx-APatch** | APatch / KernelPatch + NoMount 2.1.1 | `Pox-1.0.0-Beta-Onyx-APatch-89d7e51-begonia.zip` | `ea8daf10599ae9f538911af78af9a639e06e60c9dd1af436266c1e6a8b18a0ee` |
| **Onyx-KSU-Next** | KernelSU-Next v3.3.0 + NoMount 2.1.1 | `Pox-1.0.0-Beta-Onyx-KSU-Next-3b440af-begonia.zip` | `1ecccac6a9c905975bdc61c2212aa07384c9a8eb3bc3914add33b691c68b902a` |
| **Onyx-ReSukiSU** | ReSukiSu v4.2.0 + SuSFS v2.3.0 + NoMount | `Pox-1.0.0-Beta-Onyx-ReSukiSU-fed7437-begonia.zip` | `448103553da2533c31439f2af04ff2c426e06592f2162d94c5aa8ba2a7a48b4d` |
| **Onyx-BakaSU** | BakaSU v1.1.2 + SuSFS + NoMount | `Pox-1.0.0-Beta-Onyx-BakaSU-62f6628-begonia.zip` | `614fa305b2022902024bd8cfec014afa546aba84da2a82bbcaaea2b14fbb5eb4` |
| **Onyx** | Pure Clean Stock Unrooted (Play Integrity Passing) | `Pox-1.0.0-Beta-Onyx-onyx-f14c1b595-begonia.zip` | `7c0dd34f923a869245da5e260f02179cbba7ebc745663a4a97f65c887a13818c` |
| **Pox Kernel Manager** | Official Companion Management APK | `Pox-Kernel-Manager-v1.0.0-Beta.apk` | `8d6e5bcd434cb783b23ad44e1af4f322a8c8ffad45348a7e023bb415a446a617` |

---

## [0.9] - 2026-10-10 [DEPRECATED]

> [!CAUTION]
> ### ⚠️ Deprecation Notice: Old & Testing Only
> * **DO NOT USE FOR DAILY DRIVING**: Version 0.9 is strictly considered **OLD, DEPRECATED, AND FOR HISTORICAL / TESTING PURPOSES ONLY**.
> * **KNOWN LIMITATIONS**: Lacks universal filesystem backports (EROFS, modern F2FS, Btrfs), lacks safe PC USB 500mA SDP current limits, and contains the `sysctl_sched_migration_cost` `.rodata` bug which triggers EL1 Data Abort kernel panics during userspace boot on Xiaomi power HAL.
> * **UPGRADE**: All users are urged to upgrade immediately to **1.0.0-Beta** or newer from the active branches.

### Initial Development Features
* Initial MT6785 / Helio G90T kernel bringup based on Android 4.14.357 LTS.
* Compiler toolchain modernization to Google Android Clang 11.0.1 (r383902) + GCC 4.9 AArch64.
* Flashable AnyKernel3 packaging workflow for custom recovery installation.
* Initial exploration of kernel root solutions (APatch, KernelPatch, KernelSU-Next).
* Initial prototype implementation of Sultan debloat concepts.

### 📦 Consolidated 0.9 Recovery Packages

| Edition | Description | Recovery ZIP Package | SHA-256 Checksum |
| :--- | :--- | :--- | :--- |
| **Onyx** | Pure Clean Stock / Unrooted | `Pox-0.9-Onyx-onyx-4b91bd46a-begonia.zip` | `6833c15274b1f716da6186991a964281bc96537b7d2cd2ef62be70e785000a1d` |
| **Onyx-APatch** | APatch / KernelPatch Root Solution | `Pox-0.9-Onyx-APatch-onyx-apatch-263ca0fb3-begonia.zip` | `7780bf56e511667f0fc84ff5f1ab8265b6145637d0078849d5f761d5ebcd5fb2` |
| **Onyx-KSU-Next** | KernelSU-Next Root Solution | `Pox-0.9-Onyx-KSU-Next-onyx-ksu-next-41cead4de-begonia.zip` | `544bcd9a825eb9663b2e22af32d18758d3c4e4ed15925d91e7750fe1b5ec3de8` |
| **Onyx-Sultan-APatch** | Sultan Debloat + APatch Root | `Pox-0.9-Onyx-Sultan-APatch-onyx-sultan-apatch-f4e1fa452-begonia.zip` | `85b5c053f1545211da2e51aa5680eed42a92eff0acfd641fe7339a8e88e776ee` |
| **Granite** | Upstream Reference Stability Edition | `Pox-0.9-Granite-0f7e764-begonia.zip` | `2e621ad4ee10de08d7352490a230e6a7248973ff5d3e248b0c6105a17b87d362` |
| **Obsidian** | Upstream Compressed Memory Edition | `Pox-0.9-Obsidian-bc4891b-begonia.zip` | `bd4e6d90d201e1ddfa5d8e81cc15f8966fffcb100f5ba8a5ccc5a7190cf2f3f0` |

---

> *"We aim for stability, not for anything else."* — **Free Palestine 🇵🇸**
