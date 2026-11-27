# Patch Sets (`patches/`)

`onyx` is the single source branch with Sultan debloat and zero-logging jitter
(`CONFIG_SULTAN_DEBLOAT=y`, `CONFIG_SULTAN_NO_LOGS=y`) applied universally as the base
across ALL targets. Root flavors and media overlays are patch sets applied **at build time**
— no per-variant branches, no merge conflicts when upstream updates land.

## Roots

| Variant | Kernel side | Manager / flashing |
| --- | --- | --- |
| `vanilla` | none (onyx base) | none |
| `apatch` | `apatch.patch` (APatch defconfig + NoMount v2.2.0 + `root/apatch.env`) | latest APatch APK bundled, auto-flashed during kernel flash |
| `folkpatch` | `folkpatch.patch` (self-contained: KernelPatch kernel side + `root/folkpatch.env`) | latest FolkPatch APK bundled, auto-flashed during kernel flash |
| `ksu` | `ksu.patch` (BakaSU submodule + NoMount v2.2.0 + SuSFS + BakaSU/KSU-Next manager signs) | manager installed separately (BakaSU or KSU-Next) |

Each root patch adds a packaging profile at `root/<flavor>.env` (repo, zip/APK
names, manager package). `build.sh` sources it generically, so the flavor
definition lives in the patch, not in the build script. The resulting zip,
after writing the kernel, extracts the bundled manager package and runs its
installer against the new boot image (KernelPatch `InstallAP.sh`), then stages
the manager APK for first-boot install. Set `BUNDLE_ROOT=0` to disable, or
`APATCH_ZIP=/path/app.apk` / `FOLKPATCH_ZIP=/path/app.apk` to bundle a pinned
local build instead of latest.

The manager APK is downloaded from the root project's public **latest release**
page (`FolkPatch` / `APatch`), so no GitHub token or Actions secret is needed.
If the release page cannot be resolved, `build.sh` falls back to the public
REST API, and CI emits a warning annotation instead of shipping a zip without
the manager package.

## Overlays

| Overlay | Patch | Adds |
| --- | --- | --- |
| `miui` | `miui.patch` | WLAN/network hardening + `miui.config` fragment (MIUI/HyperOS) |
| `c2` | `c2.patch` | MTK VDEC/VENC + VCU Codec2 media stack (full VCU + V4L2 port) |
| `overclock` | `overclock.patch` | CPU/GPU/CCI overclock, every thermal/power/charging limit removed, Droidspaces container support |

`overclock` is a performance set, not a root: it composes with every root and
with the media overlays (`VARIANT=folkpatch,overclock`, `VARIANT=ksu,overclock,c2`).
It is applied before the media overlays so the two never fight over a file.
It also ships `arch/arm64/configs/droidspaces.config`, which `build.sh` merges
into whichever defconfig the root selected.

### `overclock` in detail

| Area | Change |
| --- | --- |
| Little CPU | 2.15 GHz top OPP (was 2.0 GHz), 16 OPPs with OC voltages |
| Big CPU | 2.30 GHz top OPP (was 2.0 GHz), 16 OPPs with OC voltages |
| CCI | 1.5 GHz top OPP |
| GPU | 950 MHz top OPP, 105 mV bump, dynamic power table updated |
| CPU/GPU/CCI DVFS tables | `mt6785.dts` + upower tables rebuilt for the new OPPs |
| EEM | `VMAX` raised to 0x7F (1.19375 V) so the OC OPPs are not clipped |
| Thermal | CPU/LTE/GPU trip points raised to 118–130 °C, all coolers (`atm`, `dtm`, `gpu_cus`, `vpu_cus`, `char_curr_cus`, `mutt`, `cam`, `backlight_cus`, `flashlight_cus`, `shutdown`, `kshutdown`, `sysrst`) neutralized |
| PBM / Power Throttling | PBM budget throttling (`DISABLE_PBM_FEATURE`) and MD battery-OC/low-volt/percent throttling disabled |
| PPM | DLPT / thermal / pwr_thro / user / hard-user / force-limit / LCM-off policies disabled; `ut` / `sys_boost` max caps blocked |
| FPSGO / EARA | thermal-aware frame-drop paths and thermal budget disabled |
| GED | GPU DVFS thermal ceiling removed, gaming boost forced, procfs downclock/fixed-volt overrides blocked |
| CPUFreq / `gaming_mode.c` | `scaling_max_freq` locked to `cpuinfo.max_freq`, `powersave`/`conservative`/`userspace` governors blocked, EXTREME profile locked, brownout derates removed |
| Charging | charger thermal floors and input-current throttling removed (USB SDP 500 mA cap kept) |
| Droidspaces | `droidspaces.config` fragment plus 4.14 backports (`waitid(P_PIDFD)`, `CLONE_PIDFD` `parent_tidptr`, `STATX_MNT_ID`, `STATX_ATTR_MOUNT_ROOT`, `utimensat(AT_EMPTY_PATH)`, `faccessat2`, `RTM_GETNEXTHOP` / `RTNLGRP_NEXTHOP` + SELinux rows) |

The thermal risk is deliberate: the OPPs are valid silicon for MT6785, but the
phone needs external cooling. Do not flash this build on a device that relies
on its own thermal throttling.


`c2` is the only set that carries the MT6785 VCU + V4L2 vcodec port. Without
it the build uses the legacy vendor vcodec driver, which is what the
OMX-based MIUI/HyperOS vendor stack expects.

Overlays are independent patch sets (WLAN/config vs vcodec) and **compose**:
`VARIANT=ksu,miui,c2` applies both. They are always applied in the fixed order
`miui` then `c2` regardless of how they were listed, and `./build.sh unvariant`
reverts them in the reverse order.

## Selecting a variant

Local:

```bash
./build.sh patches                     # list patch sets
VARIANT=vanilla ./build.sh all
VARIANT=apatch ./build.sh all
VARIANT=folkpatch,miui ./build.sh all
VARIANT=ksu,c2 ./build.sh all
VARIANT=ksu,miui,c2 ./build.sh all      # both overlays
VARIANT=folkpatch,overclock ./build.sh all   # max performance + Droidspaces
VARIANT=ksu,overclock,c2 ./build.sh all      # performance + Codec2
./build.sh unvariant                  # restore the clean onyx tree
```

`build.sh` refuses to build if a patch set from an earlier run is still
applied but was not requested (for example `ksu` followed by `vanilla`), and
`unvariant` reverses the applied sets (overlays first) so the tree can be
reused safely.

GitHub Actions: **Build & Release → Run workflow**, pick a root and either or
both overlays. `VARIANT` also falls back to the branch/tag name, so `onyx-ksu-next`
still builds the KSU edition. Successful builds are mirrored to Gofile and
posted to Telegram as one message (zip + Gofile link + commit + changelog +
Actions run URL) when the `TELEGRAM_BOT_TOKEN` / `TELEGRAM_CHAT_ID` secrets are
configured.

## Patch sets

| Patch | Files | Notes |
| --- | --- | --- |
| `apatch.patch` | `begonia_apatch_defconfig`, `drivers/kernelsu` symlink, `fs/nomount/*` | used by both `apatch` and `folkpatch` |
| `ksu.patch` | `.gitmodules` + `KernelSU` gitlink (BakaSU), `bakasu_defconfig`, `fs/nomount/*`, `fs/susfs.c`, `ksu-patches/ksu/*` | root drivers, applied before overlays |
| `miui.patch` | WLAN drivers, `miui.config` | overlay |
| `c2.patch` | MTK VCODEC/VCU, V4L2 uapi | overlay |
| `overclock.patch` | CPU/GPU/CCI OPP + DVFS, thermal/PBM/PPM/charger/GED/FPSGO, runtime cap locks, Droidspaces 4.14 backports + `droidspaces.config` | performance set, composes with roots and overlays |

## Updating a patch

1. Collect the change on a branch or local tree based on `onyx`.
2. Regenerate the patch against the *current* `onyx` tree, limited to kernel
   sources and configs — never include `build.sh`, workflows, docs or
   `.gitignore` noise:

   ```bash
   git diff --no-color onyx..my-variant -- \
     arch/arm64/configs drivers fs include kernel lib mm net security scripts \
     .gitmodules KernelSU ksu-patches > patches/<name>.patch
   ```

   For a root that adds a submodule, keep the `KernelSU` gitlink and
   `.gitmodules` hunks; `build.sh` runs `git submodule update --init` after
   applying the patch. Fixes that must live inside the KernelSU submodule go
   to `ksu-patches/ksu/*.patch` (applied by `build.sh` with
   `--directory=KernelSU`), never into `KernelSU/` directly.

3. Verify it applies to a clean tree:

   ```bash
   git apply --check patches/<name>.patch
   VARIANT=<name> ./build.sh patches
   ```

4. Commit. The next workflow run with that variant picks it up.

## Rules

- A patch must apply cleanly to `onyx` with `git apply` (no fuzz). If it stops
  applying after an upstream sync, rebase the source change and regenerate.
- Keep patches scoped to their variant. Shared fixes belong on `onyx`.
- Defconfigs added by a patch must match the resolver
  (`begonia_apatch_defconfig`, `bakasu_defconfig`).
- Root patches are applied before overlays; keep them independently applicable.
