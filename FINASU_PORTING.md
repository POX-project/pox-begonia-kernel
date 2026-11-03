# FinaSU port — pox-begonia-kernel

## Status
Skeleton only. `VARIANT=finasu` is wired end-to-end into the build system, but the
actual FinaSU kernel sources have **not** been vendored yet, so a build currently
produces a vanilla (unrooted) kernel — not FinaSU.

## What FinaSU is
FinaSU (LyraVoid/FinaSU, `https://github.com/lyravoid/FinaSU`) is a KernelSU-based
root solution for Android. Unlike BakaSU (`ksu.patch`, built in-tree), FinaSU's
kernel side (`finasu/kernel/`) is designed as an **out-of-tree LKM** built with its
own `./build.sh lkm` against a separate kernel `KDIR` (DDK docker image). To ship
it inside a pox flashable kernel zip it must be ported in-tree, i.e. vendored into
the kernel source tree and built built-in (or as a module) with the rest of the
image.

## What has been done (this branch)
- `build.sh`
  - `variant_patch_name()` recognizes `finasu` → `finasu.patch`
  - `parse_variant_tokens()` returns root flavor `finasu`
  - `resolve_variant()` maps `finasu` → root `FinaSU`, defconfig `begonia_finasu_defconfig`
  - `kernel_variant_tag()` → `FinaSU`
- `arch/arm64/configs/begonia_finasu_defconfig` — copy of `begonia_user_defconfig`
  plus `CONFIG_KSU=y`, `CONFIG_KSU_HOOK_TRACEPOINT=y`, `CONFIG_KSU_DYNAMIC_MANAGER=y`,
  `CONFIG_KALLSYMS_ALL=y` (FinaSU Kbuild requires `CONFIG_KALLSYMS_ALL=y` for built-in).
- `patches/finasu.patch` — currently only adds the defconfig (apply-able, verified
  with `git apply --check`).
- `.github/workflows/build.yml` — `finasu` added to the matrix roots (produces
  `finasu`, `finasu-miui`, `finasu-c2`, `finasu-c2-miui` jobs).
- `patches/README.md` — `finasu` added to the Roots and Patch sets tables.

## What still needs to be done (vendoring manifest)
Copy the FinaSU kernel tree into the pox tree, then regenerate `patches/finasu.patch`
to include them. Suggested in-tree location: `drivers/finasu/` (mirroring how
`drivers/kernelsu` is used by apatch).

FinaSU kernel subdirectories to vendor (from `LyraVoid/FinaSU/kernel/`):
```
core/          extension/   feature/       hook/
include/       infra/       manager/       policy/
preinit/       runtime/     selinux/       sulog/
supercall/     tools/
```
Plus top-level `Kbuild`, `Kconfig`, `Makefile` from `finasu/kernel/`.

Required wiring once vendored:
1. `drivers/finasu/Kconfig` — `menu "FinaSU"` with `config KSU` (depends on
   `KPROBES && EXT4_FS`), the hook-backend choice, `KSU_SUSFS`, etc.
2. `drivers/finasu/Makefile` — `obj-y`/`obj-m` for `kernelsu` built from the
   vendored objects (mirrors `finasu/kernel/Kbuild`).
3. `drivers/Kconfig` / `drivers/Makefile` — source `drivers/finasu/Kconfig` and
   `drivers/finasu/Makefile`.
4. `arch/arm64/configs/begonia_finasu_defconfig` — enable the vendored options
   (currently only has the `CONFIG_KSU*` stubs).
5. Regenerate:
   ```bash
   git diff --no-color HEAD -- \
     arch/arm64/configs/begonia_finasu_defconfig drivers/finasu \
     drivers/Kconfig drivers/Makefile > patches/finasu.patch
   git apply --check patches/finasu.patch   # must pass on clean onyx
   ```
6. Build & verify:
   ```bash
   VARIANT=finasu ./build.sh all        # local
   VARIANT=finasu ./build.sh unvariant  # restore clean onyx
   ```
   On GitHub, run the "Build & Release" workflow and pick `finasu` (or `all`).

## Notes
- FinaSU's `Kbuild` errors on SUSFS-inline LKM builds and requires
  `CONFIG_KALLSYMS_ALL=y` for built-in — the defconfig already sets the latter.
- FinaSU authenticates its manager by APK signature; the manager package name /
  signature would go in a `root/finasu.env` packaging profile if you want the APK
  bundled & auto-flashed (like `root/apatch.env`). Without it, finasu behaves like
  `ksu` (manager installed separately).
- A full kernel build needs the TRB Clang 18 toolchain (`kerdevdep/`) and an
  aarch64 cross toolchain — not available in this shell, so the build must run in
  GitHub Actions.