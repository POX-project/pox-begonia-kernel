# Patch Sets (`patches/`)

`onyx` is the single source branch. Root flavors and media overlays are patch
sets applied **at build time** — no per-variant branches, no merge conflicts
when upstream updates land.

## Roots

| Variant | Kernel side | Manager / flashing |
| --- | --- | --- |
| `vanilla` | none (onyx base) | none |
| `apatch` | `apatch.patch` (APatch defconfig + NoMount v2.2.0) | latest APatch APK bundled, auto-flashed during kernel flash |
| `folkpatch` | `apatch.patch` (same KernelPatch-based kernel side) | latest FolkPatch APK bundled, auto-flashed during kernel flash |
| `ksu` | `ksu.patch` (BakaSU submodule + NoMount v2.2.0 + SuSFS + BakaSU/KSU-Next manager signs) | manager installed separately (BakaSU or KSU-Next) |

`apatch` and `folkpatch` produce a zip that, after writing the kernel, extracts
the bundled manager package and runs its installer against the new boot image
(KernelPatch `InstallAP.sh`), then stages the manager APK for first-boot
install. Set `BUNDLE_ROOT=0` to disable, or `APATCH_ZIP=/path/app.apk` /
`FOLKPATCH_ZIP=/path/app.apk` to bundle a pinned local build instead of latest.

The manager APK is downloaded from the root project's public **latest release**
page (`FolkPatch` / `APatch`), so no GitHub token or Actions secret is needed.
If the release page cannot be resolved, `build.sh` falls back to the public
REST API, and CI emits a warning annotation instead of shipping a zip without
the manager package.

## Overlays

| Overlay | Patch | Adds |
| --- | --- | --- |
| `miui` | `miui.patch` | WLAN/network hardening + `miui.config` fragment (MIUI/HyperOS) |
| `c2` | `c2.patch` | MTK VDEC/VENC + VCU Codec2 media stack |

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
