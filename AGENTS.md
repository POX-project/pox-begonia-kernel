# Agent Rules & Repository Guide — Pox Kernel (`pox-begonia-kernel`)

## 1. Repository Overview

Single-base + patch-variant build. `onyx` is the sole source branch. Root flavors
and media overlays are patch sets applied at build time by `build.sh` — no
per-variant branches, no merge conflicts when upstream updates land.

- **Base branch**: `onyx` (this branch)
- **Kernel**: Linux 4.14.357 for Redmi Note 8 Pro (`begonia`, MT6785)
- **Release name**: `4.14.357-Pox` (set via `CUSTOM_LOCALVERSION="-Pox"` in `build.sh`)
- **Variant/commit exposure**: `/proc/perfmgr/pox_build` (key=value), consumed by
  `pox-kernel-manager`

### 1.1 Build Variants

| Variant | Root flavor | Media overlay | Patch files applied |
|---|---|---|---|
| `vanilla` | none | none | (none) |
| `apatch` | APatch | none | `apatch.patch` |
| `folkpatch` | FolkPatch | none | `folkpatch.patch` (self-contained: KernelPatch kernel side + `root/folkpatch.env`) |
| `ksu` | BakaSU | none | `ksu.patch` |
| `miui` | (any root) | MIUI/HyperOS | `miui.patch` |
| `c2` | (any root) | Codec2 | `c2.patch` |

Combine with commas: `VARIANT=ksu,c2`, `VARIANT=apatch,miui`. `miui` and `c2`
compose — `VARIANT=ksu,miui,c2` applies both overlays (always in the fixed order
`miui` then `c2`, reverted in reverse by `unvariant`).

Build locally:
```bash
VARIANT=ksu ./build.sh all            # build
VARIANT=ksu ./build.sh apply          # apply patches only (no build)
./build.sh unvariant                  # restore clean onyx tree
./build.sh patches                    # list patch sets
```

CI: **Actions → Build & Release → Run workflow**, pick root + overlay.

---

## 2. What Each Patch Does

### 2.1 `patches/apatch.patch` (used by `apatch`)

**Kernel side:**
- Adds `begonia_apatch_defconfig` (APatch-enabled defconfig)
- Adds `drivers/kernelsu` symlink → `../KernelSU/kernel` (deleted by patch, restored at build)
- Adds `fs/nomount/*` (NoMount v2.2.0) for path redirection
- KernelPatch/APatch kernel hooks (KALLSYMS, etc.)

**Packaging:**
- Adds `root/apatch.env`, the APatch packaging profile
- `build.sh` sources `root/<root flavor>.env` generically and bundles the APK named there
- Auto-flashes the manager during kernel flash via `InstallAP.sh`
- Stages manager APK for first-boot install via `pox_root_install.sh` / `init.pox_root.rc`
- Set `BUNDLE_ROOT=0` to disable; `APATCH_ZIP`/`FOLKPATCH_ZIP` to pin a local APK

### 2.1.1 `patches/folkpatch.patch` (standalone, used by `folkpatch`)

Self-contained: the same KernelPatch kernel side as `apatch.patch` plus
`root/folkpatch.env` (repo `LyraVoid/FolkPatch`, `folkpatch.zip`,
`FolkPatch.apk`, manager package `me.yuki.folk`). `VARIANT=folkpatch` applies
only this patch, so `APPLIED_PATCHES` reports `folkpatch` — it does **not**
stack on `apatch.patch`. Regenerate it from `apatch.patch` when the shared
kernel side changes (keep the same hunks, swap the `root/*.env` entry).

### 2.2 `patches/ksu.patch` (used by `ksu`)

**Kernel side:**
- `.gitmodules` + `KernelSU` gitlink → BakaSU (`https://github.com/Baka-SU/BakaSU.git`)
- Adds `bakasu_defconfig`
- Adds `fs/nomount/*` (NoMount v2.2.0) + `fs/susfs.c` (SuSFS)
- `ksu-patches/ksu/*.patch` applied on top of the KernelSU submodule:
  - `0002-allow-susfs-build-on-nongki.patch` — allows SUSFS build on non-GKI 4.14
- `kernel/reboot.c` guard: `reboot` syscall is consumed **only** when
  `magic1 == KSU_INSTALL_MAGIC1 (0xDEADBEEF)`; all other reboots fall through to
  `kernel_restart()`. This prevents the "powers off instead of restarting" bug.

**Packaging (build.sh):**
- BakaSU manager installed separately (not bundled); `ksu-patches/ksu/` applied via
  `git apply --directory=KernelSU`

### 2.3 `patches/miui.patch` (overlay, combinable with `c2`)

**Kernel side:**
- WLAN driver hardening (`wmt_cdev_wifi.c`, `gl_init.c`): module reference counting
  for buffer handlers, NVRAM registration timing, `cfg80211` module notifier to sweep
  stale wiphys on module unload
- `net/wireless/core.c`: module notifier to unregister wiphys before module memory
  is freed
- `arch/arm64/configs/miui.config` fragment:
  - Disables `CONFIG_BPF_UNPRIV_DEFAULT_OFF` and `CONFIG_BUG_ON_DATA_CORRUPTION`
    (MIUI vendor WLAN driver compatibility)
  - Enables `CONFIG_IKCONFIG_STOCK_DEFCONFIG=y` so `/proc/config.gz` reports the
    stock OEM config (hides customized config from MIUI/HyperOS userspace, preventing
    the "internal problem with your device" warning)

### 2.4 `patches/c2.patch` (overlay, combinable with `miui`)

**Kernel side (Saikrishna1504 Codec2 port):**
- `begonia_c2_defconfig` (Codec2-enabled defconfig)
- `drivers/media/platform/mtk-vcodec/mt6785/mtk_vcodec_dec_pm.c` — MMQoS frequency
  management (release MMQoS request when idle)
- `drivers/media/platform/mtk-vcodec/mt6785/mtk_vcodec_enc_pm.c` — same for encoder
- `drivers/media/platform/mtk-vcu/*` — VCU driver (ported from S (alps-mp-s0-mp1))
- `drivers/media/platform/mtk-vcodec/*` — V4L2 vcodec driver (vdec/venc) for Codec2
- UAPI headers: `mtk_vcu_controls.h`, `v4l2-controls.h`, `videodev2.h`
- `drivers/media/v4l2-core/v4l2-ctrls.c` — registers MTK V4L2 controls

### 2.5 `patches/sultan.patch` — REMOVED

Sultan debloat architecture has been merged into the `onyx` base. `CONFIG_SULTAN_DEBLOAT`
and `CONFIG_SULTAN_NO_LOGS` are now in `init/Kconfig` and available to all variants.
No separate `sultan` variant exists anymore.

---

## 3. Updating KernelSU (BakaSU)

KernelSU is a git submodule at `KernelSU/` pointing to `https://github.com/Baka-SU/BakaSU.git`.

### 3.1 Check current pinned commit

```bash
cd /content/pox-begonia-kernel
git ls-tree HEAD KernelSU   # or: cat .gitmodules
```

### 3.2 Update to latest BakaSU

```bash
cd KernelSU
git fetch origin
git checkout <new-commit-hash>
cd ..
# Verify the kernel builds with the new submodule
VARIANT=ksu ./build.sh apply
# If ksu-patches fail to apply, update them (see 3.3)
```

### 3.3 Updating ksu-patches

The patches in `ksu-patches/ksu/*.patch` are applied with `--directory=KernelSU`.
If a patch fails after a submodule update:

1. Check which patch fails: `VARIANT=ksu ./build.sh apply` prints the name.
2. The patch context (line numbers) may have shifted. Regenerate:
   ```bash
   # Apply the patch manually with fuzz, fix, then regenerate
   git -C KernelSU apply --check ../ksu-patches/ksu/000X-*.patch
   ```
3. If SUSFS compat needs updating, check `kernel/tools/susfs_compat.mk` in the
   new BakaSU — the `$(error Please wait susfs update.)` block may have been
   removed upstream (making `0002-allow-susfs-build-on-nongki.patch` unnecessary).
4. If KSU-Next manager sign support is needed, re-add `0001-add-ksunext-manager-sign.patch`.
   Currently removed (BakaSU-only).

### 3.4 Regenerating ksu.patch

After updating the submodule or ksu-patches, regenerate the full patch:

```bash
# 1. Start from clean onyx
git checkout -- . && git clean -fd
# 2. Apply ksu variant
VARIANT=ksu ./build.sh apply
# 3. Generate patch (kernel sources + configs + .gitmodules + KernelSU gitlink)
git diff --no-color HEAD -- \
  arch/arm64/configs drivers fs include kernel lib mm net security scripts \
  .gitmodules KernelSU ksu-patches > patches/ksu.patch
# 4. Verify
git checkout -- . && git clean -fd
git apply --check patches/ksu.patch
```

### 3.5 Key BakaSU files

| File | Purpose |
|---|---|
| `KernelSU/kernel/supercall/supercall.c` | `ksu_handle_sys_reboot()` — returns `-EINVAL` for non-KSU magic |
| `KernelSU/kernel/manager/apk_sign.c` | Manager APK signature verification |
| `KernelSU/kernel/tools/susfs_compat.mk` | SUSFS compatibility checks |
| `uapi/supercall.h` | `KSU_INSTALL_MAGIC1 = 0xDEADBEEF` |

---

## 4. Updating NoMount

NoMount is vendored in `fs/nomount/` (v2.2.0, upstream dev 8175c9d).

### 4.1 Current version

```bash
grep NOMOUNT_BASE_VERSION fs/nomount/nomount.h
# Should print: #define NOMOUNT_BASE_VERSION "22"
```

### 4.2 Updating to a newer upstream version

1. Fetch the new NoMount source from upstream (https://github.com/nomount/nomount or the
   version used by BakaSU).
2. Replace `fs/nomount/nomount.c` and `fs/nomount/nomount.h`.
3. Update `fs/nomount/Kconfig` and `fs/nomount/Makefile` if needed.
4. Update `NOMOUNT_BASE_VERSION` in `nomount.h`.
5. Regenerate `apatch.patch` and `ksu.patch` (both include NoMount files):
   ```bash
   # For apatch:
   git diff --no-color HEAD -- fs/nomount arch/arm64/configs/begonia_apatch_defconfig \
     drivers/kernelsu > patches/apatch.patch
   # For ksu: regenerate full ksu.patch (see 3.4)
   ```

### 4.3 NoMount wire protocol

The wire protocol (`nm_payload` / `NM_CMD_*`) is unchanged between v2.1.1 and v2.2.0,
so installed NoMount userspace modules remain compatible.

---

## 5. Updating SUSFS

SUSFS is integrated via `fs/susfs.c` (added by `ksu.patch`).

### 5.1 Current state

- SUSFS version: v2.3.0 (compatible with BakaSU main)
- The `0002-allow-susfs-build-on-nongki.patch` removes the `$(error)` block in
  `susfs_compat.mk` that suspends the build pending simonpunk's SUSFS sync.

### 5.2 Updating SUSFS

1. Check if BakaSU's new version requires a different SUSFS version.
2. Update `fs/susfs.c` and `fs/susfs.h` (if separate).
3. Update `ksu-patches/ksu/0002-allow-susfs-build-on-nongki.patch` if the
   `$(error)` block has changed or been removed upstream.
4. If BakaSU reworked `selinux_hide` (post_execve_hook), the SUSFS compat
   checks in `susfs_compat.mk` may need updating.
5. Regenerate `ksu.patch`.

### 5.3 SUSFS compat checks

`kernel/tools/susfs_compat.mk` checks for:
- `ksu_selinux_hide_running` in `security/selinux/hooks.c` (manual hook)
- `ksu_install_su_fd` in `fs/exec.c` (direct call)

If these are absent, SUSFS falls back to auto hooks. The compat `.mk` file is
part of the base BakaSU submodule.

---

## 6. Updating the Codec2 / c2 Patch

The `c2.patch` ports Saikrishna1504's VCU and V4L2 vcodec driver for Codec2.

### 6.1 Current contents

- `begonia_c2_defconfig` — Codec2-enabled defconfig
- `drivers/media/platform/mtk-vcodec/mt6785/mtk_vcodec_dec_pm.c` — MMQoS (release
  when idle)
- `drivers/media/platform/mtk-vcodec/mt6785/mtk_vcodec_enc_pm.c` — same for encoder
- `drivers/media/platform/mtk-vcu/*` — VCU driver
- `drivers/media/platform/mtk-vcodec/*` — V4L2 vcodec driver (vdec/venc)
- UAPI headers and `v4l2-ctrls.c` changes

### 6.2 Updating c2.patch

The VCU/V4L2 port is in the base tree (not just in c2.patch). To update:

1. Fetch the new Saikrishna1504 commit.
2. Apply the changes to a clean `onyx` tree.
3. The MMQoS changes (dec_pm, enc_pm) should remain in `c2.patch` only.
4. Regenerate `c2.patch`:
   ```bash
   # Apply c2 variant changes on top of onyx
   git diff --no-color HEAD -- \
     arch/arm64/configs/begonia_c2_defconfig \
     drivers/media/platform/mtk-vcodec/mt6785/mtk_vcodec_dec_pm.c \
     drivers/media/platform/mtk-vcodec/mt6785/mtk_vcodec_enc_pm.c \
     drivers/media/platform/mtk-vcu \
     drivers/media/platform/mtk-vcodec \
     drivers/media/v4l2-core/v4l2-ctrls.c \
     include/uapi/linux/mtk_vcu_controls.h \
     include/uapi/linux/v4l2-controls.h \
     include/uapi/linux/videodev2.h \
     > patches/c2.patch
   ```

### 6.3 Important: c2-only files

The VCU/V4L2 port files (`mtk-vcu/*`, `mtk_vcodec_*.c` modifications) are currently
in the base tree. If they need to be c2-only, they must be removed from base and
added to `c2.patch`. This is a large change — see the commit history for context.

---

## 7. Governance Rules (Condensed)

### 7.1 Concurrency & Mutation Safety
- Never edit files while a build is running.
- Verify all background tasks have exited before modifying.

### 7.2 Code Quality
- Linux kernel coding style (8-space tabs, braces on same line).
- Minimal targeted diffs. No gratuitous reformatting.
- No `const` on vendor-writable sysctls (prevents EL1 Data Abort panics).
- USB SDP: `STANDARD_HOST` ≤ 500mA. High power (3A/18W) only for AC chargers.
- UTS_RELEASE ≤ 64 characters.

### 7.3 Git Hygiene
- Conventional Commits: `<type>(<scope>): <summary>`
- Atomic commits, one logical change per commit.
- No WIP/lazy commits. Mandatory rationale in commit body.
- Linear history, rebase cleanly.

### 7.4 Variant Patch Rules
- A patch must apply cleanly to `onyx` with `git apply` (no fuzz).
- Keep patches scoped to their variant. Shared fixes belong on `onyx`.
- Defconfigs added by a patch must match the resolver.
- Root patches are applied before overlays.
- `miui` and `c2` are independent overlays and may be combined
  (`VARIANT=ksu,miui,c2`). They touch disjoint files (WLAN/config vs vcodec) and
  are applied in a fixed order (`OVERLAY_ORDER` in `build.sh`) so token order
  does not matter.
- Regenerate patches after any base change:
  ```bash
  git apply --check patches/<name>.patch
  VARIANT=<name> ./build.sh apply
  ```

### 7.5 Build Verification
- `bash -n build.sh` must pass.
- Workflow YAML must parse.
- `git diff --check` for whitespace.
- Test each affected variant: `VARIANT=<name> ./build.sh apply` + `unvariant`.

---

## 8. File Reference

| File | Purpose |
|---|---|
| `build.sh` | Build system: variant resolution, patch application, packaging, metadata |
| `patches/apatch.patch` | APatch kernel side + NoMount (used by apatch, folkpatch) |
| `patches/ksu.patch` | BakaSU + SuSFS + NoMount + reboot guard (used by ksu) |
| `patches/miui.patch` | MIUI/HyperOS WLAN compat + config.gz spoof |
| `patches/c2.patch` | Codec2 VCU/V4L2 port + MMQoS (used by c2) |
| `patches/README.md` | Patch set documentation |
| `drivers/misc/mediatek/performance/gaming_mode.c` | `/proc/perfmgr/pox_build` interface |
| `.github/workflows/build.yml` | CI: build, release, Telegram notification |
| `pox-kernel-manager/` | Android app consuming `/proc/perfmgr/pox_build` |

---

## TODO

- **Version bump to 2.0**: When the user says "were gonna release this", change `KERNEL_VERSION` in `build.sh` from `1.0` to `2.0` and update any version references in `README.md` and workflow.

---

## 9. Recent Changes (Oct 26 2026)

- **Manager CI fixed** (`pox-kernel-manager`): repaired the broken Telegram-share
  commit (`0bcbcdb`) on branch `fix/ci-telegram-share` (`6e8df8f`), then
  fast-forwarded `main`. Fixes: truncated `DashboardScreen.kt`, bogus imports
  (`TexturePaint`, `LocalContextAware`), `info.model`/`info.sdk` field mismatches,
  missing `R.drawable.bg_pox_default` (now a sunset gradient), `FLAG_ACTIVITY_NEW_TASK`
  and `take(8)` in `TelegramShare`. Verified green on `main` (run `40373776051`).
- **Kernel Telegram single message**: workflow now sends one message containing the
  last 6 commits (subjects truncated to fit 1024 chars), Gofile link, Actions run
  link and the flashable zip. Secrets: `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`.
  Verified (run `40372291536`).
- **Kernel compiler**: pinned TRB Clang 18 (`voltageos/clang` @ `b0fa406b`),
  cache key `kerdevdep-trb-clang-b0fa406b`, restored `CROSS_COMPILE=aarch64-linux-gnu-`,
  dropped GCC 4.9.
- **Fingerprint**: reverted the `29d95a258` probe workaround in
  `drivers/input/fingerprint/goodix_5658/gf_spi_tee.c` (commit `3ded321bf`) so
  chip-ID failure tears down cleanly instead of leaving a dead `/dev/goodix_fp`.
  Further diagnosis blocked on device logs (dmesg/logcat, `/dev/goodix_fp` presence).

---

## 10. Recent Changes (Oct 28 2026)

- **Overlays compose**: `miui` and `c2` are no longer mutually exclusive. They
  touch disjoint files (WLAN/config vs `mt6785` vcodec DVFS), so
  `VARIANT=ksu,miui,c2` is valid. `build.sh` applies overlays in `OVERLAY_ORDER`
  (`miui` then `c2`) regardless of token order, ignores duplicate tokens, and
  `unvariant` reverts in the reverse order. The workflow no longer hard-fails
  when both overlay inputs are checked.
- **c2 MMQoS is overlay-only**: the idle `PM_QOS_DEFAULT_VALUE` release in
  `mtk_vcodec_dec_pm.c` / `mtk_vcodec_enc_pm.c` lives in `patches/c2.patch`, not
  in the `onyx` base. Never apply it directly on base — that makes `c2.patch`
  unappliable and breaks every `VARIANT=c2` build.
